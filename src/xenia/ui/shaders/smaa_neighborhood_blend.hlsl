// SMAA 1x - Pass 3: neighborhood blending, produces the anti-aliased image
// (SMAA v2.8 API).
// Bindings: b0 = XeSmaaConstants, t0 = ORIGINAL source color (same input as
//           pass 1), t1 = blend weights (pass 2),
//           s0 = linear clamp, s1 = point clamp.

#include "smaa_xe_common.hlsli"

Texture2D xe_smaa_color : register(t0);
Texture2D xe_smaa_blend : register(t1);

struct XeSmaaNeighborhoodVertexOutput {
  float4 position : SV_Position;
  float2 texcoord : TEXCOORD0;
  float4 offset : TEXCOORD1;
};

XeSmaaNeighborhoodVertexOutput xe_smaa_neighborhood_blend_vs(
    uint vertex_id : SV_VertexID) {
  XeSmaaNeighborhoodVertexOutput output;
  output.position = XeSmaaFullscreenTriangle(vertex_id, output.texcoord);
  SMAANeighborhoodBlendingVS(output.texcoord, output.offset);
  return output;
}

float4 xe_smaa_neighborhood_blend_ps(XeSmaaNeighborhoodVertexOutput input)
    : SV_Target {
  return SMAANeighborhoodBlendingPS(input.texcoord, input.offset,
                                    SMAATexturePass2D(xe_smaa_color),
                                    SMAATexturePass2D(xe_smaa_blend));
}
