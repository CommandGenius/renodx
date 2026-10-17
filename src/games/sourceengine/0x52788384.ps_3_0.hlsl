#include "./common.hlsl"

sampler2D NoiseSampler : register(s0);
float4 Tint : register(c0);

float4 main(float2 uv : TEXCOORD0) : COLOR0 {
  const float4 noise = tex2D(NoiseSampler, uv);
  const float3 added = noise.rgb * Tint.rgb;
  const float keep = lerp(Tint.w, 1.f, noise.x);
  if (!IsHdrPipeline()) return float4(added, keep);
  return float4(renodx::draw::RenderIntermediatePass(renodx::color::srgb::DecodeSafe(added)), keep);
}
