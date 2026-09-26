#include "./common.hlsl"

sampler2D BaseTextureSampler : register(s0);

float4 g_DiffuseModulation              : register(c1);
float4 g_ShaderControls                 : register(c12);
float4 g_EyePos_BaseTextureTranslucency : register(c20);
float4 g_FogParams                      : register(c21);
float4 g_LinearFogColor                 : register(c29);
float4 cLightScale                      : register(c30);

float4 main(float2 uv : TEXCOORD0, float4 worldPos_fog : TEXCOORD7, float4 vertexColor : TEXCOORD6) : COLOR0 {
  float4 base = tex2D(BaseTextureSampler, uv);

  float alpha = lerp(1.f, base.a, g_EyePos_BaseTextureTranslucency.w) * g_DiffuseModulation.a;
  alpha = lerp(alpha, alpha * vertexColor.a, g_ShaderControls.w);
  float out_alpha = (abs(g_ShaderControls.y) == 0.f) ? alpha : g_LinearFogColor.w * worldPos_fog.w;

  float tint_weight = saturate(base.a + g_ShaderControls.x);
  float3 tint = lerp(1.f, g_DiffuseModulation.rgb, tint_weight) * vertexColor.rgb;
  float3 color = base.rgb * tint;

  float fog = saturate(length(g_EyePos_BaseTextureTranslucency.xyz - worldPos_fog.xyz) * g_FogParams.w + g_FogParams.x);
  fog = min(fog, g_FogParams.z);
  fog *= fog;

  float3 result = lerp(color * cLightScale.x, g_LinearFogColor.rgb, fog);
  return float4(GammaSpaceOutput(result), out_alpha);
}
