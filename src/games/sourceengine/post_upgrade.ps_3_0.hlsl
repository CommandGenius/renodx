#include "./common.hlsl"

sampler2D GradedSampler       : register(s0);
sampler2D UntonemappedSampler : register(s1);
sampler2D BloomSampler        : register(s2);
float4 PostParams             : register(c0);

float4 main(float2 uv : TEXCOORD0) : COLOR0 {
  float3 scene = tex2D(UntonemappedSampler, uv).rgb;
  if (PostParams.y == 1.f) {
    if (!IsHdrPipeline()) return float4(saturate(scene), 1.f);
    return float4(ToneMapScene(BoundHdr(scene)), 1.f);
  }

  float3 graded_gamma = tex2D(GradedSampler, uv).rgb;
  if (!IsHdrPipeline()) return float4(graded_gamma, 1.f);

  float3 scene_gamma = FramebufferToGamma(scene);
  scene_gamma += PostParams.x * tex2D(BloomSampler, uv).rgb * RENODX_BLOOM_VALID;
  float3 untonemapped = renodx::color::srgb::Decode(max(scene_gamma, 0.f));

  float3 neutral_sdr = (RENODX_TONE_MAP_TYPE == 0.f) ? saturate(untonemapped)
                                                     : untonemapped / max(1.f, renodx::math::Max(untonemapped));
  float3 graded_sdr = lerp(neutral_sdr, renodx::color::srgb::DecodeSafe(graded_gamma), RENODX_COLOR_GRADE_STRENGTH);
  return float4(ToneMapScene(untonemapped, graded_sdr, neutral_sdr), 1.f);
}
