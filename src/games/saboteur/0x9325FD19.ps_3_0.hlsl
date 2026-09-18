#include "./common.hlsl"

float4 cQuad0       : register(c0);
float4 g_Resolution : register(c52);

sampler2D Flare              : register(s0);
sampler2D SkyGradientSampler : register(s1);

float4 main(float2 uv : TEXCOORD2, float2 vpos : VPOS) : COLOR0 {
  float fMask = saturate(1.f - tex2D(SkyGradientSampler, g_Resolution.xy * vpos).w);
  float4 o = fMask * (tex2D(Flare, uv) * cQuad0);
  o.xyz = ScaleToGameBrightness(o.xyz);
  o.w = saturate(o.w + InjectionGuard());
  return o;
}
