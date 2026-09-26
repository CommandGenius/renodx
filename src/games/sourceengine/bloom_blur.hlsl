#include "./common.hlsl"

sampler2D TexSampler : register(s0);
float4 g_vPsTapOffset[3]          : register(c0);
float4 g_vScaleFactor             : register(c3);
float4 g_vUvOffsetToNeighborPixel : register(c4);

float4 Tap(float2 uv, float offset) {
  return tex2D(TexSampler, uv + g_vUvOffsetToNeighborPixel.xy * offset);
}

#if (TAPS == 13)
float4 main(float2 uv : TEXCOORD0) : COLOR0 {
  float4 sum = Tap(uv, 0.f) * 0.122764997f;
  sum += (Tap(uv, -1.46455705f) + Tap(uv, 1.46455705f)) * 0.218676999f;
  sum += (Tap(uv, -3.4179101f) + Tap(uv, 3.4179101f)) * 0.137740001f;
  sum += (Tap(uv, -5.37268591f) + Tap(uv, 5.37268591f)) * 0.059928f;
  sum += (Tap(uv, -7.32958603f) + Tap(uv, 7.32958603f)) * 0.0180040002f;
  sum += (Tap(uv, -9.28917217f) + Tap(uv, 9.28917217f)) * 0.00373300002f;
  sum += (Tap(uv, -11.251852f) + Tap(uv, 11.251852f)) * 0.000533999992f;
  return float4(saturate(sum.rgb * g_vScaleFactor.xyz), sum.a);
}
#elif (TAPS == 5)
float4 main(float2 uv : TEXCOORD0) : COLOR0 {
  float4 sum = Tap(uv, 0.f) * 0.399049997f;
  sum += (Tap(uv, -1.18242502f) + Tap(uv, 1.18242502f)) * 0.296041995f;
  sum += (Tap(uv, -3.f) + Tap(uv, 3.f)) * 0.00443299999f;
  return float4(saturate(sum.rgb * g_vScaleFactor.xyz), sum.a);
}
#else
float4 main(float2 uv0 : TEXCOORD0, float2 uv1 : TEXCOORD1, float2 uv2 : TEXCOORD2, float2 uv3 : TEXCOORD3,
            float2 uv4 : TEXCOORD4, float2 uv5 : TEXCOORD5, float2 uv6 : TEXCOORD6) : COLOR0 {
  float4 sum = (saturate(tex2D(TexSampler, uv1)) + saturate(tex2D(TexSampler, uv4))) * 0.218500003f;
  sum += saturate(tex2D(TexSampler, uv0)) * 0.201299995f;
  sum += (saturate(tex2D(TexSampler, uv2)) + saturate(tex2D(TexSampler, uv5))) * 0.0820999965f;
  sum += (saturate(tex2D(TexSampler, uv3)) + saturate(tex2D(TexSampler, uv6))) * 0.0461000018f;
  sum += (saturate(tex2D(TexSampler, uv0 + g_vPsTapOffset[0].xy)) + saturate(tex2D(TexSampler, uv0 - g_vPsTapOffset[0].xy))) * 0.0262000002f;
  sum += (saturate(tex2D(TexSampler, uv0 + g_vPsTapOffset[1].xy)) + saturate(tex2D(TexSampler, uv0 - g_vPsTapOffset[1].xy))) * 0.0162000004f;
  sum += (saturate(tex2D(TexSampler, uv0 + g_vPsTapOffset[2].xy)) + saturate(tex2D(TexSampler, uv0 - g_vPsTapOffset[2].xy))) * 0.0102000004f;
  return float4(saturate(sum.rgb * g_vScaleFactor.xyz), sum.a);
}
#endif
