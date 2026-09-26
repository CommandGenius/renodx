#include "./common.hlsl"

sampler2D TexSampler : register(s0);
float4 g_flDimValue : register(c0);

float4 main(float2 uv : TEXCOORD0) : COLOR0 {
  float3 bloom = saturate(tex2D(TexSampler, uv).rgb * saturate(g_flDimValue.x));
  float alpha = pow(max(bloom.r, max(bloom.g, bloom.b)), 0.8f);
  return float4(renodx::draw::RenderIntermediatePass(renodx::color::srgb::Decode(bloom)), alpha);
}
