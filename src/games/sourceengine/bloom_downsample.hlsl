#include "./common.hlsl"

#ifndef LINEAR_INPUT
#define LINEAR_INPUT 0
#endif

sampler2D TexSampler : register(s0);
float4 params : register(c0);

float3 Tap(float2 uv) {
  float3 tap = tex2D(TexSampler, uv).rgb;
#if (LINEAR_INPUT == 1)
  tap = renodx::color::srgb::Encode(saturate(DisplayScene(tap)));
#else
  if (RENODX_LINEAR_INPUT == 2.f) tap = renodx::color::srgb::Encode(saturate(DisplayScene(tap)));
#endif
  return tap;
}

float4 main(float2 uv0 : TEXCOORD0, float2 uv1 : TEXCOORD1, float2 uv2 : TEXCOORD2, float2 uv3 : TEXCOORD3) : COLOR0 {
  float3 average = (Tap(uv0) + Tap(uv1) + Tap(uv2) + Tap(uv3)) * 0.25f;
  float3 bloom = pow(max(average, 0.f), params.w) * dot(average, params.xyz);
  return float4(saturate(bloom), dot(average, float3(0.299f, 0.587f, 0.114f)));
}
