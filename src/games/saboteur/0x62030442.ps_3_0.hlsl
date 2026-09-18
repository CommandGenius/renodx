#include "./common.hlsl"

float4 g_vWeights : register(c0);

sampler2D TexSampler  : register(s0);
sampler2D TexSampler2 : register(s1);

float4 main(float2 uv : TEXCOORD0) : COLOR0 {
  float3 vColor2 = saturate(tex2D(TexSampler2, uv).rgb * 0.0625f) * 16.f * g_vWeights.y;
  float4 vColor = tex2D(TexSampler, uv);
  vColor.rgb = saturate(vColor.rgb * 0.0625f) * 16.f;
  float3 vTemp = vColor.rgb * vColor.rgb + vColor.rgb;

  float4 o;
  if (RENODX_TONE_MAP_TYPE != 0) {
    o.xyz = vTemp * g_vWeights.x + vColor2;
  } else {
    float lum = max(max(vTemp.x, max(vTemp.y, vTemp.z)), 1.f);
    o.xyz = saturate(vTemp * g_vWeights.x / lum + vColor2);
  }
  o.xyz = ScaleToGameBrightness(o.xyz);
  o.w = saturate(vColor.w + InjectionGuard());
  return o;
}
