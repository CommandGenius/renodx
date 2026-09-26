#include "./common.hlsl"

#ifndef VERTEXCOLOR
#define VERTEXCOLOR 1
#endif
#ifndef BLENDTINTBYBASEALPHA
#define BLENDTINTBYBASEALPHA 0
#endif
#ifndef VERTEXFOG
#define VERTEXFOG 0
#endif

sampler2D BaseTextureSampler : register(s0);

float4 g_DiffuseModulation                       : register(c1);
float4 g_ShaderControls                          : register(c12);
float4 g_flFogMaxDensityScalar_flWaterFogOORange : register(c19);
float4 g_EyePos_BaseTextureTranslucency          : register(c20);
float4 g_FogParams                               : register(c21);
float4 g_LinearFogColor                          : register(c29);
float4 cLightScale                               : register(c30);

float4 main(float2 uv : TEXCOORD0, float4 vertexFog : TEXCOORD3, float4 vertexColor : TEXCOORD6, float2 worldPos : TEXCOORD7) : COLOR0 {
  float4 base = tex2D(BaseTextureSampler, uv);

  float alpha = lerp(1.f, base.a, g_EyePos_BaseTextureTranslucency.w) * g_DiffuseModulation.a;
  alpha = lerp(alpha, alpha * vertexColor.a, g_ShaderControls.w);

#if (BLENDTINTBYBASEALPHA == 1)
  float3 tint = lerp(1.f, g_DiffuseModulation.rgb, base.a);
#else
  float3 tint = g_DiffuseModulation.rgb;
#endif
#if (VERTEXCOLOR == 1)
  tint *= vertexColor.rgb;
#endif
  float3 color = base.rgb * tint;

#if (VERTEXFOG == 1)
  float fog = vertexFog.w * vertexFog.w;
#else
  float fog = saturate(length(g_EyePos_BaseTextureTranslucency.xy - worldPos) * g_FogParams.w + g_FogParams.x);
  fog = min(fog, g_FogParams.z * g_flFogMaxDensityScalar_flWaterFogOORange.x);
  fog *= fog;
#endif

  float3 result = lerp(color * cLightScale.x, g_LinearFogColor.rgb, fog);
  return float4(GammaSpaceOutput(result), alpha);
}
