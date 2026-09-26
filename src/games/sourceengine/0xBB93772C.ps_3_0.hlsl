#include "./shared.h"

sampler2D YTextureSampler : register(s0);
sampler2D cRTextureSampler : register(s1);
sampler2D cBTextureSampler : register(s2);

float4 g_FogParams : register(c12);
float4 g_LinearFogColor : register(c29);

float4 main(float2 uv : TEXCOORD0, float4 fog : TEXCOORD1) : COLOR0 {
  float4 ycc = float4(
      tex2D(YTextureSampler, uv).x,
      tex2D(cRTextureSampler, uv).x,
      tex2D(cBTextureSampler, uv).x,
      1.f);
  float3 rgb = float3(
      dot(ycc, float4(1.16412354f, 1.59579468f, 0.f, -0.87065506f)),
      dot(ycc, float4(1.16412354f, -0.813476562f, -0.391448975f, 0.529705048f)),
      dot(ycc, float4(1.16412354f, 0.f, 2.01782227f, -1.08166885f)));

  float fog_factor = saturate(min(fog.w * g_FogParams.w - g_FogParams.x, g_FogParams.z));
  fog_factor *= fog_factor;
  return float4(renodx::color::srgb::Decode(saturate(lerp(rgb, g_LinearFogColor.rgb, fog_factor))), 1.f);
}
