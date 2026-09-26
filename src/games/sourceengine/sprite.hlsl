#include "./common.hlsl"

#ifndef VERTEXCOLOR
#define VERTEXCOLOR 0
#endif
#ifndef CONSTANTCOLOR
#define CONSTANTCOLOR 0
#endif
#ifndef HDRTYPE
#define HDRTYPE 0
#endif
#ifndef HDRENABLED
#define HDRENABLED 0
#endif
#ifndef PIXELFOGTYPE
#define PIXELFOGTYPE 0
#endif

static const float SPRITE_HDR_BOOST = 2.f;

sampler2D TexSampler : register(s0);

float4 g_Color               : register(c0);
float4 g_HDRColorScale       : register(c1);
float4 g_FogParams           : register(c12);
float4 g_EyePos_SpecExponent : register(c20);
float4 g_LinearFogColor      : register(c29);
float4 cLightScale           : register(c30);

float4 main(float2 uv : TEXCOORD0, float4 vertexColor : TEXCOORD2, float4 worldPos_projPosZ : TEXCOORD7) : COLOR0 {
  float4 sample = tex2D(TexSampler, uv);
#if (VERTEXCOLOR == 1)
  sample *= vertexColor;
#endif
#if (CONSTANTCOLOR == 1)
  sample *= g_Color;
#endif
#if (HDRTYPE != 0 && HDRENABLED == 1)
  sample.rgb *= g_HDRColorScale.x;
#endif

#if (PIXELFOGTYPE == 1)
  float depth_from_water = g_FogParams.y - worldPos_projPosZ.z;
  float depth_from_eye = g_EyePos_SpecExponent.z - worldPos_projPosZ.z;
  float fog = saturate(saturate(depth_from_water / depth_from_eye) * worldPos_projPosZ.w * g_FogParams.w);
#elif (PIXELFOGTYPE == 2)
  float fog = min(g_FogParams.z, saturate(distance(g_EyePos_SpecExponent.xyz, worldPos_projPosZ.xyz) * g_FogParams.w - g_FogParams.x));
  fog *= fog;
#else
  float fog = saturate(min(g_FogParams.z, worldPos_projPosZ.w * g_FogParams.w - g_FogParams.x));
  fog *= fog;
#endif

  float3 result = lerp(sample.rgb * cLightScale.w, g_LinearFogColor.rgb, fog);
  if (RENODX_SRGB_WRITE_OFF == 1.f) result = GammaSpaceOutput(result) * SPRITE_HDR_BOOST;
  return float4(result, sample.a);
}
