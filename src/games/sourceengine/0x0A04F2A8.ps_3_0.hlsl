#include "./common.hlsl"

sampler2D BaseSampler : register(s0);
float4 g_Color     : register(c0);
float4 g_FogParams : register(c12);
float4 g_FogColor  : register(c29);

float4 main(float2 uv : TEXCOORD0, float4 vertex_color : COLOR0, float4 fog : TEXCOORD7) : COLOR0 {
  const float4 base = saturate(tex2D(BaseSampler, uv) * vertex_color);
  float3 color = lerp(g_Color.rgb, base.rgb, base.a);
  const float fog_factor = min(saturate(fog.w * g_FogParams.w - g_FogParams.x), g_FogParams.z);
  color = lerp(color, g_FogColor.rgb, fog_factor * fog_factor);
  if (!IsHdrPipeline()) return float4(color, base.a);
  return float4(pow(max(2.f * color, 0.f), 2.2f) * 0.5f, base.a);
}
