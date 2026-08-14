/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// FEASIBILITY PROBE, not a rendering path. Nothing binds this yet.
//
// It exists to answer three questions that decide whether a GPU microcode
// interpreter is worth months of work, and it answers them at BUILD time,
// on the build machine, without a console:
//   - how big is the DXBC of a shader that decodes and executes guest ALU
//     microcode at runtime, against the 23-62 KB of a translated material;
//   - does it compile at all, given the register file has to be a dynamically
//     indexed array;
//   - what shape does the dispatch take - one switch over 30 vector and 50
//     scalar opcodes.
//
// The structure is what matters for those answers, so the expensive parts are
// real: the indexable register file, the operand fetch with swizzle and
// modifiers, the full vector opcode switch, and the common scalar ones. The
// texture path is a single generic sample rather than the full format decode -
// that part would be shared with the existing translator either way.

// Guest microcode, as uint dwords.
ByteAddressBuffer xe_microcode : register(t0);
// Guest float constants.
ByteAddressBuffer xe_float_constants : register(t1);
Texture2DArray<float4> xe_texture : register(t2);
SamplerState xe_sampler : register(s0);

cbuffer XeInterpreterConstants : register(b0) {
  // Where this shader's microcode starts, and how many instruction pairs.
  uint xe_ucode_offset_dwords;
  uint xe_ucode_alu_count;
  uint xe_register_count;
  uint xe_interpolator_count;
};

#define kXeMaxRegisters 64

// A guest ALU instruction is two 32-bit words for the vector half and one more
// for the scalar half plus operand encoding; the probe reads three dwords and
// decodes the fields the real thing would.
struct XeAluWords {
  uint w0;
  uint w1;
  uint w2;
};

float4 XeApplyModifiers(float4 v, uint negate, uint absolute) {
  v = absolute ? abs(v) : v;
  return negate ? -v : v;
}

float4 XeSwizzle(float4 v, uint swizzle) {
  return float4(v[(swizzle >> 0) & 3], v[(swizzle >> 2) & 3],
                v[(swizzle >> 4) & 3], v[(swizzle >> 6) & 3]);
}

float4 main(float4 position : SV_Position,
            float4 interpolators[4] : TEXCOORD0) : SV_Target {
  // The register file. Dynamically indexed, which is the structural cost this
  // probe exists to price - it becomes an indexable temp array in DXBC.
  float4 regs[kXeMaxRegisters];
  [unroll] for (uint init = 0; init < 4; ++init) {
    regs[init] = interpolators[init];
  }
  [loop] for (uint clear = 4; clear < kXeMaxRegisters; ++clear) {
    regs[clear] = float4(0.0, 0.0, 0.0, 0.0);
  }

  uint pc = 0;
  [loop] while (pc < xe_ucode_alu_count) {
    uint base = (xe_ucode_offset_dwords + pc * 3) << 2;
    XeAluWords words;
    words.w0 = xe_microcode.Load(base);
    words.w1 = xe_microcode.Load(base + 4);
    words.w2 = xe_microcode.Load(base + 8);
    ++pc;

    // Decode the fields the translator decodes.
    uint vector_opcode = (words.w0 >> 0) & 0x1F;
    uint scalar_opcode = (words.w0 >> 5) & 0x3F;
    uint vector_dest = (words.w1 >> 0) & 0x3F;
    uint vector_write_mask = (words.w1 >> 6) & 0xF;
    uint scalar_dest = (words.w1 >> 10) & 0x3F;
    uint scalar_write_mask = (words.w1 >> 16) & 0xF;
    uint src0_reg = (words.w2 >> 0) & 0x3F;
    uint src1_reg = (words.w2 >> 6) & 0x3F;
    uint src2_reg = (words.w2 >> 12) & 0x3F;
    uint src0_swizzle = (words.w2 >> 18) & 0xFF;
    uint src1_swizzle = (words.w0 >> 11) & 0xFF;
    uint src2_swizzle = (words.w0 >> 19) & 0xFF;
    uint src_negate = (words.w1 >> 20) & 7;
    uint src_absolute = (words.w1 >> 23) & 7;
    uint is_constant = (words.w1 >> 26) & 7;

    // Operand fetch: register file or float constants, then swizzle and
    // modifiers. This is per-source, three times per instruction.
    float4 src0 = (is_constant & 1)
                      ? asfloat(xe_float_constants.Load4(src0_reg << 4))
                      : regs[src0_reg & (kXeMaxRegisters - 1)];
    float4 src1 = (is_constant & 2)
                      ? asfloat(xe_float_constants.Load4(src1_reg << 4))
                      : regs[src1_reg & (kXeMaxRegisters - 1)];
    float4 src2 = (is_constant & 4)
                      ? asfloat(xe_float_constants.Load4(src2_reg << 4))
                      : regs[src2_reg & (kXeMaxRegisters - 1)];
    src0 = XeApplyModifiers(XeSwizzle(src0, src0_swizzle), src_negate & 1,
                            src_absolute & 1);
    src1 = XeApplyModifiers(XeSwizzle(src1, src1_swizzle), src_negate & 2,
                            src_absolute & 2);
    src2 = XeApplyModifiers(XeSwizzle(src2, src2_swizzle), src_negate & 4,
                            src_absolute & 4);

    float4 vector_result = float4(0.0, 0.0, 0.0, 0.0);
    switch (vector_opcode) {
      case 0: vector_result = src0 + src1; break;                    // add
      case 1: vector_result = src0 * src1; break;                    // mul
      case 2: vector_result = max(src0, src1); break;                // max
      case 3: vector_result = min(src0, src1); break;                // min
      case 4: vector_result = src0 == src1 ? 1.0 : 0.0; break;       // seq
      case 5: vector_result = src0 > src1 ? 1.0 : 0.0; break;        // sgt
      case 6: vector_result = src0 >= src1 ? 1.0 : 0.0; break;       // sge
      case 7: vector_result = src0 != src1 ? 1.0 : 0.0; break;       // sne
      case 8: vector_result = frac(src0); break;                     // frc
      case 9: vector_result = trunc(src0); break;                    // trunc
      case 10: vector_result = floor(src0); break;                   // floor
      case 11: vector_result = src0 * src1 + src2; break;            // mad
      case 12: vector_result = src2 >= 0.0 ? src0 : src1; break;     // cndeq-ish
      case 13: vector_result = src2 > 0.0 ? src0 : src1; break;      // cndgt
      case 14: vector_result = src2 >= 0.0 ? src0 : src1; break;     // cndge
      case 15: vector_result = dot(src0.xyzw, src1.xyzw); break;     // dp4
      case 16: vector_result = dot(src0.xyz, src1.xyz); break;       // dp3
      case 17: vector_result = dot(src0.xy, src1.xy) + src0.z; break;  // dp2add
      case 18: {                                                     // cube
        float3 c = src0.xyz;
        float3 a = abs(c);
        vector_result = float4(c.x, c.y, a.z, max(a.x, max(a.y, a.z)));
      } break;
      case 19: vector_result = max(src0, src1) * src2; break;        // max4-ish
      case 20: vector_result = src0 >= src1 ? 1.0 : 0.0; break;      // setp_eq
      case 21: vector_result = src0 != src1 ? 1.0 : 0.0; break;      // setp_ne
      case 22: vector_result = src0 > src1 ? 1.0 : 0.0; break;       // setp_gt
      case 23: vector_result = src0 >= src1 ? 1.0 : 0.0; break;      // setp_ge
      case 24: vector_result = src0; break;                          // kill_eq
      case 25: vector_result = src0; break;                          // kill_gt
      case 26: vector_result = src0; break;                          // kill_ge
      case 27: vector_result = src0; break;                          // kill_ne
      case 28: vector_result = src0 * src1 + src2; break;            // dst
      case 29: vector_result = max(src0, src1); break;               // maxa
      default: vector_result = src0; break;
    }

    float scalar_result = 0.0;
    float s0 = src0.x;
    float s1 = src1.x;
    switch (scalar_opcode) {
      case 0: scalar_result = s0 + s1; break;
      case 1: scalar_result = s0 + s1; break;
      case 2: scalar_result = s0 * s1; break;
      case 3: scalar_result = min(s0, s1); break;
      case 4: scalar_result = s0 == s1 ? 1.0 : 0.0; break;
      case 5: scalar_result = s0 > s1 ? 1.0 : 0.0; break;
      case 6: scalar_result = s0 >= s1 ? 1.0 : 0.0; break;
      case 7: scalar_result = s0 != s1 ? 1.0 : 0.0; break;
      case 8: scalar_result = frac(s0); break;
      case 9: scalar_result = trunc(s0); break;
      case 10: scalar_result = floor(s0); break;
      case 11: scalar_result = exp2(s0); break;
      case 12: scalar_result = log2(max(s0, 1e-30)); break;
      case 13: scalar_result = rcp(s0); break;
      case 14: scalar_result = rsqrt(max(s0, 1e-30)); break;
      case 15: scalar_result = sqrt(max(s0, 0.0)); break;
      case 16: scalar_result = sin(s0); break;
      case 17: scalar_result = cos(s0); break;
      case 18: scalar_result = max(s0, s1); break;
      case 19: scalar_result = s0 * s1; break;
      case 20: scalar_result = saturate(s0); break;
      default: scalar_result = s0; break;
    }

    // Write back, respecting the write masks - a per-component select, which
    // is what the translator emits too.
    uint vd = vector_dest & (kXeMaxRegisters - 1);
    float4 v = regs[vd];
    v.x = (vector_write_mask & 1) ? vector_result.x : v.x;
    v.y = (vector_write_mask & 2) ? vector_result.y : v.y;
    v.z = (vector_write_mask & 4) ? vector_result.z : v.z;
    v.w = (vector_write_mask & 8) ? vector_result.w : v.w;
    regs[vd] = v;

    uint sd = scalar_dest & (kXeMaxRegisters - 1);
    float4 s = regs[sd];
    s.x = (scalar_write_mask & 1) ? scalar_result : s.x;
    s.y = (scalar_write_mask & 2) ? scalar_result : s.y;
    s.z = (scalar_write_mask & 4) ? scalar_result : s.z;
    s.w = (scalar_write_mask & 8) ? scalar_result : s.w;
    regs[sd] = s;
  }

  // One generic texture read, so the probe is not measured without any.
  float4 sampled =
      xe_texture.SampleLevel(xe_sampler, float3(regs[0].xy, 0.0), 0.0);
  return regs[1] * sampled;
}
