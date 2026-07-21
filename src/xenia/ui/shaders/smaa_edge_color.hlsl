// SMAA 1x - Pass 1 variant: color edge detection (SMAA v2.8 API).
// Detects a SUPERSET of the luma variant's edges: the max per-channel delta
// is always >= the luma-weighted delta, so everything luma detection sees is
// seen here too, plus chromatic edges of similar luminance that the luma
// variant misses. Slightly costlier (3-channel deltas instead of 1 luma).
// Output: RG8 edges texture (cleared to 0 before the pass).
// Bindings: b0 = XeSmaaConstants, t0 = source color (non-sRGB view),
//           s0 = linear clamp, s1 = point clamp.

#include "smaa_xe_common.hlsli"

Texture2D xe_smaa_color : register(t0);

struct XeSmaaEdgeVertexOutput {
  float4 position : SV_Position;
  float2 texcoord : TEXCOORD0;
  float4 offset0 : TEXCOORD1;
  float4 offset1 : TEXCOORD2;
  float4 offset2 : TEXCOORD3;
};

XeSmaaEdgeVertexOutput xe_smaa_edge_color_vs(uint vertex_id : SV_VertexID) {
  XeSmaaEdgeVertexOutput output;
  output.position = XeSmaaFullscreenTriangle(vertex_id, output.texcoord);
  float4 offsets[3];
  SMAAEdgeDetectionVS(output.texcoord, offsets);
  output.offset0 = offsets[0];
  output.offset1 = offsets[1];
  output.offset2 = offsets[2];
  return output;
}

float2 xe_smaa_edge_color_ps(XeSmaaEdgeVertexOutput input) : SV_Target {
  float4 offsets[3] = {input.offset0, input.offset1, input.offset2};
  return SMAAColorEdgeDetectionPS(input.texcoord, offsets,
                                  SMAATexturePass2D(xe_smaa_color));
}
