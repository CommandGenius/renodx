#include "./common.hlsl"

sampler2D RGBSTextureSampler : register(s0);
float4 InputScale            : register(c0);
float4 g_flFogFactor         : register(c3);
float4 g_LinearFogColor      : register(c29);
float4 cLightScale           : register(c30);

static const float SKY_HDR_BOOST = 2.f;

float3 RGBS(float2 uv) {
  float4 texel = tex2D(RGBSTextureSampler, uv);
  return texel.rgb * texel.a;
}

float4 main(float2 uv00 : TEXCOORD0, float2 uv01 : TEXCOORD1, float2 uv10 : TEXCOORD2, float2 uv11 : TEXCOORD3, float2 texel_coord : TEXCOORD4) : COLOR0 {
  float2 blend = frac(texel_coord);
  float3 row0 = lerp(RGBS(uv00), RGBS(uv10), blend.x);
  float3 row1 = lerp(RGBS(uv01), RGBS(uv11), blend.x);
  float3 sky = lerp(row0, row1, blend.y) * InputScale.rgb * SKY_HDR_BOOST;
  return float4(lerp(sky * cLightScale.x, g_LinearFogColor.rgb, g_flFogFactor.x), 1.f);
}
