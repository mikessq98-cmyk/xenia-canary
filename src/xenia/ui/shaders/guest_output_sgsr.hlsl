// Snapdragon(TM) Game Super Resolution v1 (SGSR) upscaling pass for the guest
// output presentation chain. Ported from
// third_party/Magpie/src/Effects/SGSR.hlsl (itself from
// github.com/SnapdragonStudios/snapdragon-gsr, BSD-3-Clause) to a raw
// pixel shader compatible with Xenia's guest output paint flow:
// - Vertex shader: the shared guest_output_triangle_strip_rect VS (only
//   SV_Position is interpolated; texture coordinates are derived from the
//   pixel position, which is possible because SGSR always runs as an
//   intermediate pass with the output rectangle at the origin).
// - b0 (pixel-visible root constants): XeSgsrConstants.
// - t0: the guest output texture, s0: linear clamp static sampler.

cbuffer XeSgsrConstants : register(b0) {
  float2 xe_sgsr_output_size_inv;
  float2 xe_sgsr_input_size;
  float2 xe_sgsr_input_size_inv;
  // Edge refinement strength; 2.0 is the SGSR default.
  float xe_sgsr_edge_sharpness;
  // Luma delta treated as an edge; the SGSR default 8.0 PRE-DIVIDED by 255 on
  // the CPU.
  float xe_sgsr_edge_threshold;
};

Texture2D xe_sgsr_input : register(t0);
SamplerState xe_sgsr_sampler : register(s0);

#define UseEdgeDirection

float XeSgsrFastLanczos2(float x) {
  float wA = x - 4.0;
  float wB = x * wA - wA;
  wA *= wA;
  return wB * wA;
}

#if defined(UseEdgeDirection)
float2 XeSgsrWeightY(float dx, float dy, float c, float3 data)
#else
float2 XeSgsrWeightY(float dx, float dy, float c, float data)
#endif
{
#if defined(UseEdgeDirection)
  float std = data.x;
  float2 dir = data.yz;
  float edgeDis = ((dx * dir.y) + (dy * dir.x));
  float x = (((dx * dx) + (dy * dy)) +
             ((edgeDis * edgeDis) *
              ((clamp(((c * c) * std), 0.0, 1.0) * 0.7) + -1.0)));
#else
  float std = data;
  float x = ((dx * dx) + (dy * dy)) * 0.5 + clamp(abs(c) * std, 0.0, 1.0);
#endif
  float w = XeSgsrFastLanczos2(x);
  return float2(w, w * c);
}

float2 XeSgsrEdgeDirection(float4 left, float4 right) {
  float2 dir;
  float RxLz = (right.x + (-left.z));
  float RwLy = (right.w + (-left.y));
  float2 delta;
  delta.x = (RxLz + RwLy);
  delta.y = (RxLz + (-RwLy));
  float lengthInv =
      rsqrt((delta.x * delta.x + 3.075740e-05) + (delta.y * delta.y));
  dir.x = (delta.x * lengthInv);
  dir.y = (delta.y * lengthInv);
  return dir;
}

float4 XeSgsrGatherGreen(float2 p) {
  return xe_sgsr_input.GatherGreen(xe_sgsr_sampler, p);
}

#ifdef XE_SGSR_BASE_CATROM
// Catmull-Rom (bicubic B=0, C=0.5) base image instead of plain bilinear -
// sharper flat areas and chroma (SGSR itself only refines luma on detected
// edges on top of this base). 4x4-tap kernel folded into 9 bilinear samples
// via weighted offsets; straight-line code (no loops or branches).
float3 XeSgsrSampleBase(float2 uv) {
  float2 sample_pos = uv * xe_sgsr_input_size;
  float2 tex_pos_1 = floor(sample_pos - 0.5) + 0.5;
  float2 f = sample_pos - tex_pos_1;
  float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
  float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
  float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
  float2 w3 = f * f * (-0.5 + 0.5 * f);
  float2 w12 = w1 + w2;
  float2 offset12 = w2 / w12;
  float2 tex_pos_0 = (tex_pos_1 - 1.0) * xe_sgsr_input_size_inv;
  float2 tex_pos_3 = (tex_pos_1 + 2.0) * xe_sgsr_input_size_inv;
  float2 tex_pos_12 = (tex_pos_1 + offset12) * xe_sgsr_input_size_inv;
  float3 result;
  result = xe_sgsr_input
               .SampleLevel(xe_sgsr_sampler, float2(tex_pos_0.x, tex_pos_0.y),
                            0).xyz *
           (w0.x * w0.y);
  result += xe_sgsr_input
                .SampleLevel(xe_sgsr_sampler, float2(tex_pos_12.x, tex_pos_0.y),
                             0).xyz *
            (w12.x * w0.y);
  result += xe_sgsr_input
                .SampleLevel(xe_sgsr_sampler, float2(tex_pos_3.x, tex_pos_0.y),
                             0).xyz *
            (w3.x * w0.y);
  result += xe_sgsr_input
                .SampleLevel(xe_sgsr_sampler, float2(tex_pos_0.x, tex_pos_12.y),
                             0).xyz *
            (w0.x * w12.y);
  result +=
      xe_sgsr_input
          .SampleLevel(xe_sgsr_sampler, float2(tex_pos_12.x, tex_pos_12.y), 0)
          .xyz *
      (w12.x * w12.y);
  result += xe_sgsr_input
                .SampleLevel(xe_sgsr_sampler, float2(tex_pos_3.x, tex_pos_12.y),
                             0).xyz *
            (w3.x * w12.y);
  result += xe_sgsr_input
                .SampleLevel(xe_sgsr_sampler, float2(tex_pos_0.x, tex_pos_3.y),
                             0).xyz *
            (w0.x * w3.y);
  result += xe_sgsr_input
                .SampleLevel(xe_sgsr_sampler, float2(tex_pos_12.x, tex_pos_3.y),
                             0).xyz *
            (w12.x * w3.y);
  result += xe_sgsr_input
                .SampleLevel(xe_sgsr_sampler, float2(tex_pos_3.x, tex_pos_3.y),
                             0).xyz *
            (w3.x * w3.y);
  // The negative Catmull-Rom lobes can produce slight under/overshoot.
  return saturate(result);
}
#else
float3 XeSgsrSampleBase(float2 uv) {
  return xe_sgsr_input.SampleLevel(xe_sgsr_sampler, uv, 0).xyz;
}
#endif

float3 XeSgsrYuv(float2 uv, float4 con1) {
  float3 pix = XeSgsrSampleBase(uv);

  float2 imgCoord = ((uv.xy * con1.zw) + float2(-0.5, 0.5));
  float2 imgCoordPixel = floor(imgCoord);
  float2 coord = (imgCoordPixel * con1.xy);
  float2 pl = (imgCoord + (-imgCoordPixel));
  float4 left = XeSgsrGatherGreen(coord);

  float edgeVote = abs(left.z - left.y) + abs(pix.y - left.y) +
                   abs(pix.y - left.z);
  [branch] if (edgeVote > xe_sgsr_edge_threshold) {
    coord.x += con1.x;

    float4 right = XeSgsrGatherGreen(coord + float2(con1.x, 0.0));
    float4 upDown;
    upDown.xy = XeSgsrGatherGreen(coord + float2(0.0, -con1.y)).wz;
    upDown.zw = XeSgsrGatherGreen(coord + float2(0.0, con1.y)).yx;

    float mean = (left.y + left.z + right.x + right.w) * 0.25;
    left = left - float4(mean, mean, mean, mean);
    right = right - float4(mean, mean, mean, mean);
    upDown = upDown - float4(mean, mean, mean, mean);
    float pix_G = pix.y - mean;

    float sum = (((((abs(left.x) + abs(left.y)) + abs(left.z)) + abs(left.w)) +
                  (((abs(right.x) + abs(right.y)) + abs(right.z)) +
                   abs(right.w))) +
                 (((abs(upDown.x) + abs(upDown.y)) + abs(upDown.z)) +
                  abs(upDown.w)));
    float sumMean = 1.014185e+01 / sum;
    float std = (sumMean * sumMean);

#if defined(UseEdgeDirection)
    float3 data = float3(std, XeSgsrEdgeDirection(left, right));
#else
    float data = std;
#endif

    float2 aWY = XeSgsrWeightY(pl.x, pl.y + 1.0, upDown.x, data);
    aWY += XeSgsrWeightY(pl.x - 1.0, pl.y + 1.0, upDown.y, data);
    aWY += XeSgsrWeightY(pl.x - 1.0, pl.y - 2.0, upDown.z, data);
    aWY += XeSgsrWeightY(pl.x, pl.y - 2.0, upDown.w, data);
    aWY += XeSgsrWeightY(pl.x + 1.0, pl.y - 1.0, left.x, data);
    aWY += XeSgsrWeightY(pl.x, pl.y - 1.0, left.y, data);
    aWY += XeSgsrWeightY(pl.x, pl.y, left.z, data);
    aWY += XeSgsrWeightY(pl.x + 1.0, pl.y, left.w, data);
    aWY += XeSgsrWeightY(pl.x - 1.0, pl.y - 1.0, right.x, data);
    aWY += XeSgsrWeightY(pl.x - 2.0, pl.y - 1.0, right.y, data);
    aWY += XeSgsrWeightY(pl.x - 2.0, pl.y, right.z, data);
    aWY += XeSgsrWeightY(pl.x - 1.0, pl.y, right.w, data);

    float finalY = aWY.y / aWY.x;

    float max4 = max(max(left.y, left.z), max(right.x, right.w));
    float min4 = min(min(left.y, left.z), min(right.x, right.w));
    finalY = clamp(xe_sgsr_edge_sharpness * finalY, min4, max4);

    float deltaY = finalY - pix_G;

    pix = saturate(pix + deltaY);
  }
  return pix;
}

float4 xe_guest_output_sgsr_ps(float4 position : SV_Position) : SV_Target {
  // SGSR is always an intermediate pass with the output rectangle at the
  // origin, so the position maps to the texture coordinates directly.
  float2 uv = position.xy * xe_sgsr_output_size_inv;
  float4 con1 = float4(xe_sgsr_input_size_inv, xe_sgsr_input_size);
  return float4(XeSgsrYuv(uv, con1), 1.0);
}
