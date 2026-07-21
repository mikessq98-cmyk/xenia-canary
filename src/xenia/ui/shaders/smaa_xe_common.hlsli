// Shared prologue for Xenia's SMAA 1x passes (third_party/SMAA v2.8).
// Establishes the environment SMAA.hlsl expects:
// - SMAA_RT_METRICS as a runtime value (dynamic resolution) from root
//   constants: (1/width, 1/height, width, height) of the SMAA working size.
// - Explicitly register-bound samplers shared by ALL passes, so one static
//   sampler layout (s0 = linear clamp, s1 = point clamp) serves every pass
//   regardless of fxc dead-code elimination (see SMAA_XE_EXTERNAL_SAMPLERS in
//   the vendored SMAA.hlsl).
// The quality preset (SMAA_PRESET_*) is injected per bytecode variant with
// fxc /D; the runtime cvar picks the variant.

#ifndef XENIA_UI_SHADERS_SMAA_XE_COMMON_HLSLI_
#define XENIA_UI_SHADERS_SMAA_XE_COMMON_HLSLI_

cbuffer XeSmaaConstants : register(b0) {
  float4 xe_smaa_rt_metrics;
};

SamplerState LinearSampler : register(s0);
SamplerState PointSampler : register(s1);
#define SMAA_XE_EXTERNAL_SAMPLERS 1

#define SMAA_RT_METRICS xe_smaa_rt_metrics
// SMAA_HLSL_4, not SMAA_HLSL_4_1: the 4_1 profile's Gather in the blend
// weight pass crashes the Xbox UWP driver's shader compiler (newbe_xs.dll,
// access violation compiling "quality 0, pass 1"); the 4 profile uses plain
// SampleLevel everywhere.
#define SMAA_HLSL_4 1

// The Xbox UWP driver's shader compiler (newbe_xs.dll) access-violates while
// flattening SMAA's data-dependent `while` search loops. The vendored SMAA.hlsl
// rewrites those loops as bounded, statically-unrolled `for` loops guarded by
// this macro; forcing [unroll] turns them into straight-line, branch-free code
// (state simply freezes once the stop condition is met), which compiles cleanly.
#define SMAA_XE_SEARCH_UNROLL [unroll]

#include "SMAA.hlsl"

// Fullscreen triangle covering the render target; texcoord (0,0) at the top
// left, (1,1) at the bottom right of the visible area.
float4 XeSmaaFullscreenTriangle(uint vertex_id, out float2 texcoord) {
  texcoord = float2((vertex_id << 1) & 2, vertex_id & 2);
  return float4(texcoord * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

#endif  // XENIA_UI_SHADERS_SMAA_XE_COMMON_HLSLI_
