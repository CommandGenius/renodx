#include "./common.hlsl"

sampler2D TexSampler : register(s0);
float4 g_vAlpha      : register(c0);
float4 g_vDesaturate : register(c1);
float4 g_vTint       : register(c2);

float4 main(float2 uv : TEXCOORD0) : COLOR0 {
  float3 blur = renodx::color::srgb::DecodeSafe(tex2D(TexSampler, uv).rgb);
  blur = lerp(blur, dot(blur, float3(0.2125f, 0.7154f, 0.0721f)), saturate(g_vDesaturate.x)) * g_vTint.rgb;
  if (!IsHdrPipeline()) return float4(renodx::color::srgb::Encode(blur), g_vAlpha.x);
  return float4(ToneMapScene(blur), g_vAlpha.x);
}
