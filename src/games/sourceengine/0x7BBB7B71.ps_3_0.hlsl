#include "./common.hlsl"

sampler2D BaseTextureSampler : register(s0);

float4 g_DiffuseModulation : register(c1);
float4 g_ShaderControls    : register(c12);
float4 g_EyePos            : register(c20);
float4 g_FogParams         : register(c21);
float4 g_LinearFogColor    : register(c29);
float4 cLightScale         : register(c30);

float4 main(float2 uv : TEXCOORD0, float4 vertexColor : TEXCOORD2, float3 fogCoord : TEXCOORD6, float3 worldPos : TEXCOORD7) : COLOR0 {
  float4 base = tex2D(BaseTextureSampler, uv);

  float water_range = g_ShaderControls.x * g_FogParams.y - worldPos.z;
  float eye_range = g_ShaderControls.x * g_EyePos.z - worldPos.z;
  float water_fog = (eye_range * eye_range) == 0.f ? 1.f : saturate(water_range / eye_range);
  float f = saturate(min(fogCoord.z * g_FogParams.w * water_fog - g_FogParams.x, g_FogParams.z));
  float fog = f * f + (f - f * f) * g_ShaderControls.x;

  float alpha = base.a * g_DiffuseModulation.a;
  alpha = lerp(alpha, alpha * vertexColor.a, g_ShaderControls.w);
  float3 base_rgb = RENODX_SRGB_WRITE_OFF == 1.f ? renodx::color::srgb::Decode(saturate(base.rgb)) : base.rgb;
  float3 color = base_rgb * g_DiffuseModulation.rgb * vertexColor.rgb;

  float fog_alpha = lerp(alpha, f, g_ShaderControls.z);
  float out_alpha = g_ShaderControls.y * (fogCoord.z * g_LinearFogColor.w - fog_alpha) + fog_alpha;
  float3 result = lerp(color * cLightScale.x, g_LinearFogColor.rgb, fog);
  return float4(result, out_alpha);
}
