/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// A pixel shader that executes guest ALU microcode read from a buffer, instead
// of being translated into one shader per material.
//
// The point is arithmetic, not elegance: this driver charges 358-808 ms to
// compile a translated material and a title has hundreds of them, arriving
// while the level streams in. One interpreter compiles once and covers all of
// them. It pays for that with a loop and two switches per pixel, on a GPU
// measured at 10-20% busy.
//
// The instruction layout below is the real one from ucode.h - three dwords per
// ALU instruction, with the fields in the order the hardware defines them.
// Anything decoded wrongly here executes as garbage, so the bit positions are
// the part to check first when the picture is wrong.

// Guest microcode dwords for the shader being interpreted.
ByteAddressBuffer xe_microcode : register(t0);
// Guest float constants, four floats each.
ByteAddressBuffer xe_float_constants : register(t1);
// Guest texture fetch constants - format, dimension, swizzle and signedness.
ByteAddressBuffer xe_texture_fetch_constants : register(t3);
Texture2DArray<float4> xe_texture : register(t2);
SamplerState xe_sampler : register(s0);

cbuffer XeInterpreterConstants : register(b0) {
  uint xe_ucode_offset_dwords;
  uint xe_ucode_alu_count;
  uint xe_register_count;
  uint xe_interpolator_count;
};

#define kXeMaxRegisters 64

// ---------------------------------------------------------------------------
// Vector opcodes, from AluVectorOpcode.
#define kXeVectorAdd 0
#define kXeVectorMul 1
#define kXeVectorMax 2
#define kXeVectorMin 3
#define kXeVectorSeq 4
#define kXeVectorSgt 5
#define kXeVectorSge 6
#define kXeVectorSne 7
#define kXeVectorFrc 8
#define kXeVectorTrunc 9
#define kXeVectorFloor 10
#define kXeVectorMad 11
#define kXeVectorCndEq 12
#define kXeVectorCndGe 13
#define kXeVectorCndGt 14
#define kXeVectorDp4 15
#define kXeVectorDp3 16
#define kXeVectorDp2Add 17
#define kXeVectorCube 18
#define kXeVectorMax4 19
#define kXeVectorSetpEqPush 20
#define kXeVectorSetpNePush 21
#define kXeVectorSetpGtPush 22
#define kXeVectorSetpGePush 23
#define kXeVectorKillEq 24
#define kXeVectorKillGt 25
#define kXeVectorKillGe 26
#define kXeVectorKillNe 27
#define kXeVectorDst 28
#define kXeVectorMaxA 29

// Scalar opcodes, from AluScalarOpcode.
#define kXeScalarAdds 0
#define kXeScalarAddsPrev 1
#define kXeScalarMuls 2
#define kXeScalarMulsPrev 3
#define kXeScalarMaxs 5
#define kXeScalarMins 6
#define kXeScalarSeqs 7
#define kXeScalarSgts 8
#define kXeScalarSges 9
#define kXeScalarSnes 10
#define kXeScalarFrcs 11
#define kXeScalarTruncs 12
#define kXeScalarFloors 13
#define kXeScalarExp 14
#define kXeScalarLogc 15
#define kXeScalarLog 16
#define kXeScalarRcpc 17
#define kXeScalarRcpf 18
#define kXeScalarRcp 19
#define kXeScalarRsqc 20
#define kXeScalarRsqf 21
#define kXeScalarRsq 22
#define kXeScalarMaxAs 23
#define kXeScalarMaxAsf 24
#define kXeScalarSubs 25
#define kXeScalarSubsPrev 26
#define kXeScalarKillsEq 35
#define kXeScalarKillsGt 36
#define kXeScalarKillsGe 37
#define kXeScalarKillsNe 38
#define kXeScalarKillsOne 39
#define kXeScalarSqrt 40
#define kXeScalarMulsc0 42
#define kXeScalarMulsc1 43
#define kXeScalarAddsc0 44
#define kXeScalarAddsc1 45
#define kXeScalarSubsc0 46
#define kXeScalarSubsc1 47
#define kXeScalarSin 48
#define kXeScalarCos 49
#define kXeScalarRetainPrev 50

// ---------------------------------------------------------------------------

// Component-relative swizzle: each 2-bit field is added to the component index
// it applies to, which is how the guest encodes ".xyzw" as all zeroes.
float4 XeSwizzle(float4 v, uint swizzle) {
  return float4(v[((swizzle >> 0) + 0) & 3], v[((swizzle >> 2) + 1) & 3],
                v[((swizzle >> 4) + 2) & 3], v[((swizzle >> 6) + 3) & 3]);
}

// A scalar source takes one component, selected the same way.
float XeSwizzleScalar(float4 v, uint swizzle) {
  return v[(swizzle >> 6) & 3];
}

float4 XeLoadSource(uint reg, uint swizzle, uint negate, uint from_temporary,
                    uint absolute_constants, float4 regs[kXeMaxRegisters]) {
  float4 value;
  if (from_temporary) {
    value = regs[reg & (kXeMaxRegisters - 1)];
  } else {
    value = asfloat(xe_float_constants.Load4((reg & 0xFF) << 4));
    if (absolute_constants) {
      value = abs(value);
    }
  }
  value = XeSwizzle(value, swizzle);
  return negate ? -value : value;
}

float4 XeExecuteVector(uint opcode, float4 src0, float4 src1, float4 src2,
                       inout bool kill) {
  switch (opcode) {
    case kXeVectorAdd:
      return src0 + src1;
    case kXeVectorMul:
      return src0 * src1;
    case kXeVectorMax:
      return max(src0, src1);
    case kXeVectorMin:
      return min(src0, src1);
    case kXeVectorSeq:
      return src0 == src1 ? 1.0 : 0.0;
    case kXeVectorSgt:
      return src0 > src1 ? 1.0 : 0.0;
    case kXeVectorSge:
      return src0 >= src1 ? 1.0 : 0.0;
    case kXeVectorSne:
      return src0 != src1 ? 1.0 : 0.0;
    case kXeVectorFrc:
      return frac(src0);
    case kXeVectorTrunc:
      return trunc(src0);
    case kXeVectorFloor:
      return floor(src0);
    case kXeVectorMad:
      return src0 * src1 + src2;
    case kXeVectorCndEq:
      return src0 == 0.0 ? src1 : src2;
    case kXeVectorCndGe:
      return src0 >= 0.0 ? src1 : src2;
    case kXeVectorCndGt:
      return src0 > 0.0 ? src1 : src2;
    case kXeVectorDp4:
      return dot(src0, src1);
    case kXeVectorDp3:
      return dot(src0.xyz, src1.xyz);
    case kXeVectorDp2Add:
      return dot(src0.xy, src1.xy) + src2.x;
    case kXeVectorCube: {
      // Cube map face selection: src0 is (z, y, x, ...) per the guest layout.
      float3 c = src0.zyx;
      float3 a = abs(c);
      float major = max(a.x, max(a.y, a.z));
      float2 st;
      float face;
      if (a.x >= a.y && a.x >= a.z) {
        st = float2(c.z, -c.y) * (c.x >= 0.0 ? 1.0 : -1.0);
        face = c.x >= 0.0 ? 0.0 : 1.0;
      } else if (a.y >= a.z) {
        st = float2(c.x, c.z) * (c.y >= 0.0 ? 1.0 : -1.0);
        face = c.y >= 0.0 ? 2.0 : 3.0;
      } else {
        st = float2(c.x, -c.y) * (c.z >= 0.0 ? 1.0 : -1.0);
        face = c.z >= 0.0 ? 4.0 : 5.0;
      }
      return float4(st.x, st.y, 2.0 * major, face);
    }
    case kXeVectorMax4:
      return max(max(src0.x, src0.y), max(src0.z, src0.w));
    case kXeVectorKillEq:
      kill = kill || any(src0 == src1);
      return 0.0;
    case kXeVectorKillGt:
      kill = kill || any(src0 > src1);
      return 0.0;
    case kXeVectorKillGe:
      kill = kill || any(src0 >= src1);
      return 0.0;
    case kXeVectorKillNe:
      kill = kill || any(src0 != src1);
      return 0.0;
    case kXeVectorDst:
      return float4(1.0, src0.y * src1.y, src0.z, src1.w);
    case kXeVectorMaxA:
      return max(src0, src1);
    default:
      return src0;
  }
}

float XeExecuteScalar(uint opcode, float a, float b, inout float previous,
                      inout bool kill) {
  switch (opcode) {
    case kXeScalarAdds:
      return a + b;
    case kXeScalarAddsPrev:
      return a + previous;
    case kXeScalarMuls:
      return a * b;
    case kXeScalarMulsPrev:
      return a * previous;
    case kXeScalarMaxs:
      return max(a, b);
    case kXeScalarMins:
      return min(a, b);
    case kXeScalarSeqs:
      return a == 0.0 ? 1.0 : 0.0;
    case kXeScalarSgts:
      return a > 0.0 ? 1.0 : 0.0;
    case kXeScalarSges:
      return a >= 0.0 ? 1.0 : 0.0;
    case kXeScalarSnes:
      return a != 0.0 ? 1.0 : 0.0;
    case kXeScalarFrcs:
      return frac(a);
    case kXeScalarTruncs:
      return trunc(a);
    case kXeScalarFloors:
      return floor(a);
    case kXeScalarExp:
      return exp2(a);
    case kXeScalarLogc: {
      float t = log2(a);
      return isinf(t) ? -3.402823466e+38 : t;
    }
    case kXeScalarLog:
      return log2(a);
    case kXeScalarRcpc: {
      float t = rcp(a);
      return isinf(t) ? 3.402823466e+38 : t;
    }
    case kXeScalarRcpf:
    case kXeScalarRcp:
      return a == 0.0 ? 0.0 : rcp(a);
    case kXeScalarRsqc: {
      float t = rsqrt(a);
      return isinf(t) ? 3.402823466e+38 : t;
    }
    case kXeScalarRsqf:
    case kXeScalarRsq:
      return a <= 0.0 ? 0.0 : rsqrt(a);
    case kXeScalarMaxAs:
    case kXeScalarMaxAsf:
      return max(a, b);
    case kXeScalarSubs:
      return a - b;
    case kXeScalarSubsPrev:
      return a - previous;
    case kXeScalarKillsEq:
      kill = kill || (a == 0.0);
      return 0.0;
    case kXeScalarKillsGt:
      kill = kill || (a > 0.0);
      return 0.0;
    case kXeScalarKillsGe:
      kill = kill || (a >= 0.0);
      return 0.0;
    case kXeScalarKillsNe:
      kill = kill || (a != 0.0);
      return 0.0;
    case kXeScalarKillsOne:
      kill = kill || (a == 1.0);
      return 0.0;
    case kXeScalarSqrt:
      return sqrt(max(a, 0.0));
    case kXeScalarMulsc0:
    case kXeScalarMulsc1:
      return a * b;
    case kXeScalarAddsc0:
    case kXeScalarAddsc1:
      return a + b;
    case kXeScalarSubsc0:
    case kXeScalarSubsc1:
      return a - b;
    case kXeScalarSin:
      return sin(a);
    case kXeScalarCos:
      return cos(a);
    case kXeScalarRetainPrev:
      return previous;
    default:
      return a;
  }
}

float4 main(float4 position : SV_Position) : SV_Target {
  // The register file, indexed by values only known at run time - the cost
  // this design trades a compilation for.
  float4 regs[kXeMaxRegisters];
  [unroll] for (uint init = 0; init < 4; ++init) {
    regs[init] = position * float(init + 1);
  }
  [loop] for (uint clear = 4; clear < kXeMaxRegisters; ++clear) {
    regs[clear] = float4(0.0, 0.0, 0.0, 0.0);
  }

  float previous_scalar = 0.0;
  bool kill = false;

  uint pc = 0;
  [loop] while (pc < xe_ucode_alu_count) {
    uint base = (xe_ucode_offset_dwords + pc * 3) << 2;
    uint word0 = xe_microcode.Load(base);
    uint word1 = xe_microcode.Load(base + 4);
    uint word2 = xe_microcode.Load(base + 8);
    ++pc;

    // Word 0, in the order ucode.h declares the bitfield.
    uint vector_dest = word0 & 0x3F;
    uint abs_constants = (word0 >> 7) & 1;
    uint scalar_dest = (word0 >> 8) & 0x3F;
    uint export_data = (word0 >> 15) & 1;
    uint vector_write_mask = (word0 >> 16) & 0xF;
    uint scalar_write_mask = (word0 >> 20) & 0xF;
    uint vector_clamp = (word0 >> 24) & 1;
    uint scalar_clamp = (word0 >> 25) & 1;
    uint scalar_opcode = (word0 >> 26) & 0x3F;

    // Word 1.
    uint src3_swiz = word1 & 0xFF;
    uint src2_swiz = (word1 >> 8) & 0xFF;
    uint src1_swiz = (word1 >> 16) & 0xFF;
    uint src3_negate = (word1 >> 24) & 1;
    uint src2_negate = (word1 >> 25) & 1;
    uint src1_negate = (word1 >> 26) & 1;

    // Word 2.
    uint src3_reg = word2 & 0xFF;
    uint src2_reg = (word2 >> 8) & 0xFF;
    uint src1_reg = (word2 >> 16) & 0xFF;
    uint vector_opcode = (word2 >> 24) & 0x1F;
    uint src3_sel = (word2 >> 29) & 1;
    uint src2_sel = (word2 >> 30) & 1;
    uint src1_sel = (word2 >> 31) & 1;

    float4 src0 = XeLoadSource(src1_reg, src1_swiz, src1_negate, src1_sel,
                               abs_constants, regs);
    float4 src1 = XeLoadSource(src2_reg, src2_swiz, src2_negate, src2_sel,
                               abs_constants, regs);
    float4 src2 = XeLoadSource(src3_reg, src3_swiz, src3_negate, src3_sel,
                               abs_constants, regs);

    float4 vector_result = XeExecuteVector(vector_opcode, src0, src1, src2, kill);
    if (vector_clamp) {
      vector_result = saturate(vector_result);
    }

    // The scalar half takes its operand from the first source's selected
    // component, and keeps a running "previous" the *_prev opcodes read.
    float scalar_a = XeSwizzleScalar(src0, src1_swiz);
    float scalar_b = XeSwizzleScalar(src1, src2_swiz);
    float scalar_result =
        XeExecuteScalar(scalar_opcode, scalar_a, scalar_b, previous_scalar, kill);
    if (scalar_clamp) {
      scalar_result = saturate(scalar_result);
    }
    previous_scalar = scalar_result;

    // Exports write both halves to the vector destination; otherwise each half
    // writes its own register.
    uint vd = vector_dest & (kXeMaxRegisters - 1);
    float4 v = regs[vd];
    v.x = (vector_write_mask & 1) ? vector_result.x : v.x;
    v.y = (vector_write_mask & 2) ? vector_result.y : v.y;
    v.z = (vector_write_mask & 4) ? vector_result.z : v.z;
    v.w = (vector_write_mask & 8) ? vector_result.w : v.w;
    regs[vd] = v;

    uint sd = export_data ? vd : (scalar_dest & (kXeMaxRegisters - 1));
    float4 s = regs[sd];
    s.x = (scalar_write_mask & 1) ? scalar_result : s.x;
    s.y = (scalar_write_mask & 2) ? scalar_result : s.y;
    s.z = (scalar_write_mask & 4) ? scalar_result : s.z;
    s.w = (scalar_write_mask & 8) ? scalar_result : s.w;
    regs[sd] = s;
  }

  if (kill) {
    discard;
  }

  // The texture path. This is the part the translator spends 62% of its output
  // on - 2516 bytes and 63 instructions per fetch, because format, signedness,
  // swizzle and filtering all come from the fetch constants at run time. An
  // interpreter pays it ONCE instead of once per fetch in every shader, which
  // is the whole argument for it, so the cost has to be in the measurement.
  uint4 fetch_words = xe_texture_fetch_constants.Load4(0);
  uint dimension = (fetch_words.z >> 9) & 3;
  uint num_format = (fetch_words.w >> 15) & 1;
  uint swizzle = (fetch_words.w >> 16) & 0xFFF;
  uint signs = (fetch_words.x >> 2) & 0xFF;

  float3 coordinates = float3(saturate(regs[0].xy), 0.0);
  if (dimension == 2) {
    coordinates.z = saturate(regs[0].z) * 5.0;
  }

  float4 sampled = xe_texture.SampleLevel(xe_sampler, coordinates, 0.0);

  // Per-component sign handling: unsigned, signed, or biased.
  float4 converted;
  [unroll] for (uint c = 0; c < 4; ++c) {
    uint component_sign = (signs >> (c * 2)) & 3;
    float v = sampled[c];
    if (component_sign == 1) {
      v = v * 2.0 - 1.0;
    } else if (component_sign == 2) {
      v = v - 0.5;
    }
    converted[c] = v;
  }

  // Integer formats are delivered normalized and have to be scaled back.
  if (num_format == 1) {
    converted *= 255.0;
  }

  // Destination swizzle, three bits per component, with the constant sources
  // the guest encoding allows.
  float4 swizzled;
  [unroll] for (uint s = 0; s < 4; ++s) {
    uint selector = (swizzle >> (s * 3)) & 7;
    if (selector == 4) {
      swizzled[s] = 0.0;
    } else if (selector == 5) {
      swizzled[s] = 1.0;
    } else {
      swizzled[s] = converted[selector & 3];
    }
  }

  return regs[1] * swizzled;
}
