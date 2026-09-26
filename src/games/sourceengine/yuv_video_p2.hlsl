#include "./common.hlsl"

#ifndef FOG
#define FOG 0
#endif
#ifndef VERTEX_ALPHA
#define VERTEX_ALPHA 0
#endif
#ifndef FOG_2D
#define FOG_2D 0
#endif

sampler2D YTextureSampler  : register(s0);
sampler2D cRTextureSampler : register(s1);
sampler2D cBTextureSampler : register(s2);

float4 g_EyePos_SpecExponent : register(c11);
float4 g_FogParams           : register(c12);
float4 g_LinearFogColor      : register(c29);

float4 main(float2 uv : TEXCOORD0, float3 worldPos : TEXCOORD1, float vertexAlpha : TEXCOORD2) : COLOR0 {
  float4 ycc = float4(tex2D(YTextureSampler, uv).x, tex2D(cRTextureSampler, uv).x, tex2D(cBTextureSampler, uv).x, 1.f);
  float3 rgb = float3(
      dot(ycc, float4(1.16412354f, 1.59579468f, 0.f, -0.87065506f)),
      dot(ycc, float4(1.16412354f, -0.813476562f, -0.391448975f, 0.529705048f)),
      dot(ycc, float4(1.16412354f, 0.f, 2.01782227f, -1.08166885f)));
#if (FOG == 1)
#if (FOG_2D == 1)
  float fog_distance = length(g_EyePos_SpecExponent.xy - worldPos.xy);
#else
  float fog_distance = length(g_EyePos_SpecExponent.xyz - worldPos);
#endif
  float fog = saturate(fog_distance * g_FogParams.w + g_FogParams.x);
  fog = min(fog, g_FogParams.z);
  fog *= fog;
  rgb = lerp(rgb, g_LinearFogColor.rgb, fog);
#endif
#if (VERTEX_ALPHA == 1)
  float alpha = vertexAlpha;
#else
  float alpha = 1.f;
#endif
  return float4(GammaSpaceOutput(rgb), alpha);
}
