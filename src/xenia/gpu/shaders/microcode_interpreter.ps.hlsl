/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// Executes guest pixel shader microcode read from guest memory, instead of
// being translated into one host shader per material.
//
// The arithmetic that justifies it: this driver charges 358-808 ms to compile a
// translated material, a title has hundreds of them, and they arrive while a
// level streams in. This compiles once - 19776 bytes, less than the smallest of
// three titles' median materials - and covers all of them. It pays with a loop
// and two switches per pixel, on a GPU measured at 10-20% busy.
//
// It is bound exactly like a translated pixel shader: the same constant
// buffers at the same registers, the same shared memory raw SRV. That is what
// lets it stand in for one inside a real draw rather than being a pass bolted
// onto the frame - which is what took the device down twice when tried the
// other way.
//
// The instruction layout is the one ucode.h defines. Anything decoded wrongly
// here executes as garbage, so the bit positions are the first thing to check
// when the picture is wrong.

// Constant buffers, matching DxbcShaderTranslator::CbufferRegister.
cbuffer xe_system_constants : register(b0) {
  uint4 xe_system_constants_data[64];
};
cbuffer xe_float_constants : register(b1) {
  float4 xe_float_constants_data[256];
};
cbuffer xe_bool_loop_constants : register(b2) {
  uint4 xe_bool_loop_constants_data[8];
};
cbuffer xe_fetch_constants : register(b3) {
  uint4 xe_fetch_constants_data[48];
};

// Guest memory, at SRVMainRegister::kSharedMemory in SRVSpace::kMain. The
// microcode is already in here - it is guest data - so nothing has to be
// uploaded for the interpreter to read it.
ByteAddressBuffer xe_shared_memory : register(t0, space0);

// Where in guest memory this draw's pixel shader microcode starts, and how
// many ALU instructions to run. Passed through unused system constant slots so
// no root signature change is needed.
#define kXeUcodeAddressSlot 60
#define kXeUcodeCountSlot 61

#define kXeMaxRegisters 64
#define kXeMaxAluInstructions 512

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

// Swizzles are component-relative: each two-bit field is added to the index of
// the component it applies to, so all zeroes means .xyzw.
float4 XeSwizzle(float4 v, uint swizzle) {
  return float4(v[((swizzle >> 0) + 0) & 3], v[((swizzle >> 2) + 1) & 3],
                v[((swizzle >> 4) + 2) & 3], v[((swizzle >> 6) + 3) & 3]);
}

float4 XeLoadSource(uint reg, uint swizzle, uint negate, uint from_temporary,
                    uint absolute_constants, float4 regs[kXeMaxRegisters]) {
  float4 value;
  if (from_temporary) {
    value = regs[reg & (kXeMaxRegisters - 1)];
  } else {
    value = xe_float_constants_data[reg & 0xFF];
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
    case kXeVectorAdd: return src0 + src1;
    case kXeVectorMul: return src0 * src1;
    case kXeVectorMax: return max(src0, src1);
    case kXeVectorMin: return min(src0, src1);
    case kXeVectorSeq: return src0 == src1 ? 1.0 : 0.0;
    case kXeVectorSgt: return src0 > src1 ? 1.0 : 0.0;
    case kXeVectorSge: return src0 >= src1 ? 1.0 : 0.0;
    case kXeVectorSne: return src0 != src1 ? 1.0 : 0.0;
    case kXeVectorFrc: return frac(src0);
    case kXeVectorTrunc: return trunc(src0);
    case kXeVectorFloor: return floor(src0);
    case kXeVectorMad: return src0 * src1 + src2;
    case kXeVectorCndEq: return src0 == 0.0 ? src1 : src2;
    case kXeVectorCndGe: return src0 >= 0.0 ? src1 : src2;
    case kXeVectorCndGt: return src0 > 0.0 ? src1 : src2;
    case kXeVectorDp4: return dot(src0, src1);
    case kXeVectorDp3: return dot(src0.xyz, src1.xyz);
    case kXeVectorDp2Add: return dot(src0.xy, src1.xy) + src2.x;
    case kXeVectorCube: {
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
    case kXeVectorMax4: return max(max(src0.x, src0.y), max(src0.z, src0.w));
    case kXeVectorKillEq: kill = kill || any(src0 == src1); return 0.0;
    case kXeVectorKillGt: kill = kill || any(src0 > src1); return 0.0;
    case kXeVectorKillGe: kill = kill || any(src0 >= src1); return 0.0;
    case kXeVectorKillNe: kill = kill || any(src0 != src1); return 0.0;
    case kXeVectorDst: return float4(1.0, src0.y * src1.y, src0.z, src1.w);
    case kXeVectorMaxA: return max(src0, src1);
    default: return src0;
  }
}

float XeExecuteScalar(uint opcode, float a, float b, inout float previous,
                      inout bool kill) {
  switch (opcode) {
    case kXeScalarAdds: return a + b;
    case kXeScalarAddsPrev: return a + previous;
    case kXeScalarMuls: return a * b;
    case kXeScalarMulsPrev: return a * previous;
    case kXeScalarMaxs: return max(a, b);
    case kXeScalarMins: return min(a, b);
    case kXeScalarSeqs: return a == 0.0 ? 1.0 : 0.0;
    case kXeScalarSgts: return a > 0.0 ? 1.0 : 0.0;
    case kXeScalarSges: return a >= 0.0 ? 1.0 : 0.0;
    case kXeScalarSnes: return a != 0.0 ? 1.0 : 0.0;
    case kXeScalarFrcs: return frac(a);
    case kXeScalarTruncs: return trunc(a);
    case kXeScalarFloors: return floor(a);
    case kXeScalarExp: return exp2(a);
    case kXeScalarLogc: { float t = log2(a); return isinf(t) ? -3.402823466e+38 : t; }
    case kXeScalarLog: return log2(a);
    case kXeScalarRcpc: { float t = rcp(a); return isinf(t) ? 3.402823466e+38 : t; }
    case kXeScalarRcpf:
    case kXeScalarRcp: return a == 0.0 ? 0.0 : rcp(a);
    case kXeScalarRsqc: { float t = rsqrt(a); return isinf(t) ? 3.402823466e+38 : t; }
    case kXeScalarRsqf:
    case kXeScalarRsq: return a <= 0.0 ? 0.0 : rsqrt(a);
    case kXeScalarMaxAs:
    case kXeScalarMaxAsf: return max(a, b);
    case kXeScalarSubs: return a - b;
    case kXeScalarSubsPrev: return a - previous;
    case kXeScalarKillsEq: kill = kill || (a == 0.0); return 0.0;
    case kXeScalarKillsGt: kill = kill || (a > 0.0); return 0.0;
    case kXeScalarKillsGe: kill = kill || (a >= 0.0); return 0.0;
    case kXeScalarKillsNe: kill = kill || (a != 0.0); return 0.0;
    case kXeScalarKillsOne: kill = kill || (a == 1.0); return 0.0;
    case kXeScalarSqrt: return sqrt(max(a, 0.0));
    case kXeScalarMulsc0:
    case kXeScalarMulsc1: return a * b;
    case kXeScalarAddsc0:
    case kXeScalarAddsc1: return a + b;
    case kXeScalarSubsc0:
    case kXeScalarSubsc1: return a - b;
    case kXeScalarSin: return sin(a);
    case kXeScalarCos: return cos(a);
    case kXeScalarRetainPrev: return previous;
    default: return a;
  }
}

float4 main(float4 position : SV_Position) : SV_Target {
  float4 regs[kXeMaxRegisters];
  [unroll] for (uint init = 0; init < 4; ++init) {
    regs[init] = position * float(init + 1);
  }
  [loop] for (uint clear = 4; clear < kXeMaxRegisters; ++clear) {
    regs[clear] = float4(0.0, 0.0, 0.0, 0.0);
  }

  uint ucode_address = xe_system_constants_data[kXeUcodeAddressSlot].x;
  // Bounded here as well as by the count that arrives: a loop whose limit
  // comes from outside is a hung GPU and a lost device when it comes wrong,
  // not a wrong picture.
  uint alu_count =
      min(xe_system_constants_data[kXeUcodeCountSlot].x, kXeMaxAluInstructions);

  float previous_scalar = 0.0;
  bool kill = false;

  uint pc = 0;
  [loop] while (pc < alu_count) {
    uint base = ucode_address + pc * 12;
    uint word0 = xe_shared_memory.Load(base);
    uint word1 = xe_shared_memory.Load(base + 4);
    uint word2 = xe_shared_memory.Load(base + 8);
    ++pc;

    uint vector_dest = word0 & 0x3F;
    uint abs_constants = (word0 >> 7) & 1;
    uint scalar_dest = (word0 >> 8) & 0x3F;
    uint export_data = (word0 >> 15) & 1;
    uint vector_write_mask = (word0 >> 16) & 0xF;
    uint scalar_write_mask = (word0 >> 20) & 0xF;
    uint vector_clamp = (word0 >> 24) & 1;
    uint scalar_clamp = (word0 >> 25) & 1;
    uint scalar_opcode = (word0 >> 26) & 0x3F;

    uint src3_swiz = word1 & 0xFF;
    uint src2_swiz = (word1 >> 8) & 0xFF;
    uint src1_swiz = (word1 >> 16) & 0xFF;
    uint src3_negate = (word1 >> 24) & 1;
    uint src2_negate = (word1 >> 25) & 1;
    uint src1_negate = (word1 >> 26) & 1;

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

    float4 vector_result =
        XeExecuteVector(vector_opcode, src0, src1, src2, kill);
    if (vector_clamp) {
      vector_result = saturate(vector_result);
    }

    float scalar_a = src0[(src1_swiz >> 6) & 3];
    float scalar_b = src1[(src2_swiz >> 6) & 3];
    float scalar_result = XeExecuteScalar(scalar_opcode, scalar_a, scalar_b,
                                          previous_scalar, kill);
    if (scalar_clamp) {
      scalar_result = saturate(scalar_result);
    }
    previous_scalar = scalar_result;

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

  // No texture sampling yet, deliberately. Reading a bindless descriptor that
  // the draw did not fill in is a page fault at VA 0 and a lost device on this
  // driver, and that failure has already been paid for twice today. Colour
  // comes from the interpreted arithmetic alone, so a first run says whether
  // the ALU path is right without being able to die on descriptors.
  return regs[0];
}
