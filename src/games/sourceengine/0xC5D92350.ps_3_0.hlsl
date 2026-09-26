#include "./common.hlsl"

sampler2D TexSampler : register(s0);
sampler2D SceneSampler : register(s1);

float4 main(float2 uv : TEXCOORD0) : COLOR0 {
  float3 bloom = tex2D(TexSampler, uv).rgb;
  if (!IsHdrPipeline()) return float4(bloom, 1.f);

  float3 composite = FramebufferToGamma(tex2D(SceneSampler, uv).rgb) + bloom * RENODX_BLOOM_VALID;
  return float4(ToneMapScene(renodx::color::srgb::Decode(max(composite, 0.f))), 1.f);
}
