// SMAA 1x - Pass 2: blending weight calculation (SMAA v2.8 API).
// Output: RGBA8 blend weights texture (cleared to 0 before the pass).
// Bindings: b0 = XeSmaaConstants, t0 = edges (pass 1), t1 = AreaTex LUT
//           (160x560 RG8), t2 = SearchTex LUT (64x16 R8),
//           s0 = linear clamp, s1 = point clamp.

#include "smaa_xe_common.hlsli"

Texture2D xe_smaa_edges : register(t0);
Texture2D xe_smaa_area : register(t1);
Texture2D xe_smaa_search : register(t2);

struct XeSmaaBlendWeightVertexOutput {
  float4 position : SV_Position;
  float2 texcoord : TEXCOORD0;
  float2 pixcoord : TEXCOORD1;
  float4 offset0 : TEXCOORD2;
  float4 offset1 : TEXCOORD3;
  float4 offset2 : TEXCOORD4;
};

XeSmaaBlendWeightVertexOutput xe_smaa_blend_weight_vs(
    uint vertex_id : SV_VertexID) {
  XeSmaaBlendWeightVertexOutput output;
  output.position = XeSmaaFullscreenTriangle(vertex_id, output.texcoord);
  float4 offsets[3];
  SMAABlendingWeightCalculationVS(output.texcoord, output.pixcoord, offsets);
  output.offset0 = offsets[0];
  output.offset1 = offsets[1];
  output.offset2 = offsets[2];
  return output;
}

float4 xe_smaa_blend_weight_ps(XeSmaaBlendWeightVertexOutput input)
    : SV_Target {
  float4 offsets[3] = {input.offset0, input.offset1, input.offset2};
  // Zero subsample indices: SMAA 1x (no temporal/spatial multisampling).
  return SMAABlendingWeightCalculationPS(
      input.texcoord, input.pixcoord, offsets,
      SMAATexturePass2D(xe_smaa_edges), SMAATexturePass2D(xe_smaa_area),
      SMAATexturePass2D(xe_smaa_search), float4(0.0, 0.0, 0.0, 0.0));
}
