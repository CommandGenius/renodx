#include "./common.hlsl"

float4 g_vWeights : register(c0);

sampler2D TexSampler : register(s0);

float4 main(float2 uv : TEXCOORD0) : COLOR0 {
  float4 vColor = tex2D(TexSampler, uv);
  vColor.rgb = saturate(vColor.rgb * 0.0625f) * 16.f;
  float3 vTemp = vColor.rgb * vColor.rgb + vColor.rgb;

  float4 o;
  if (RENODX_TONE_MAP_TYPE != 0) {
    o.xyz = vTemp * g_vWeights.x;
  } else {
    float lum = max(max(vTemp.x, max(vTemp.y, vTemp.z)), 1.f);
    o.xyz = saturate(vTemp * g_vWeights.x / lum);
  }
  o.xyz = ScaleToGameBrightness(o.xyz);
  o.w = saturate(vColor.w + InjectionGuard());
  return o;
}
