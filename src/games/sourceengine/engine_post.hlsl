#include "./common.hlsl"

#ifndef AA_ENABLE
#define AA_ENABLE 0
#endif
#ifndef AA_QUALITY_MODE
#define AA_QUALITY_MODE 0
#endif
#ifndef AA_REDUCE_ONE_PIXEL_LINE_BLUR
#define AA_REDUCE_ONE_PIXEL_LINE_BLUR 0
#endif
#ifndef COL_CORRECT_NUM_LOOKUPS
#define COL_CORRECT_NUM_LOOKUPS 0
#endif

sampler2D BaseTextureSampler            : register(s0);
sampler2D FBTextureSampler              : register(s1);
sampler3D ColorCorrectionVolumeTexture0 : register(s2);
sampler3D ColorCorrectionVolumeTexture1 : register(s3);
sampler3D ColorCorrectionVolumeTexture2 : register(s4);
sampler3D ColorCorrectionVolumeTexture3 : register(s5);

float4 psTapOffs_Packed             : register(c0);
float4 tweakables                   : register(c1);
float4 uvTransform                  : register(c2);
float4 ColorCorrectionDefaultWeight : register(c3);
float4 ColorCorrectionVolumeWeights : register(c4);
float4 BloomFactor                  : register(c5);

float3 SampleFramebuffer(float2 uv) {
  return FramebufferToGamma(tex2D(FBTextureSampler, uv).rgb);
}

float3 PerformColorCorrection(float3 color) {
#if (COL_CORRECT_NUM_LOOKUPS > 0)
  float3 lut_uv = color * (31.f / 32.f) + (0.5f / 32.f);
  float3 graded = color * ColorCorrectionDefaultWeight.x;
  graded += tex3D(ColorCorrectionVolumeTexture0, lut_uv).rgb * ColorCorrectionVolumeWeights.x;
#if (COL_CORRECT_NUM_LOOKUPS > 1)
  graded += tex3D(ColorCorrectionVolumeTexture1, lut_uv).rgb * ColorCorrectionVolumeWeights.y;
#endif
#if (COL_CORRECT_NUM_LOOKUPS > 2)
  graded += tex3D(ColorCorrectionVolumeTexture2, lut_uv).rgb * ColorCorrectionVolumeWeights.z;
#endif
#if (COL_CORRECT_NUM_LOOKUPS > 3)
  graded += tex3D(ColorCorrectionVolumeTexture3, lut_uv).rgb * ColorCorrectionVolumeWeights.w;
#endif
  return graded;
#else
  return color;
#endif
}

#if (AA_ENABLE == 1)
float3 PerformAA(float3 baseColor, float2 fbTexCoord) {
#if (AA_QUALITY_MODE == 0)
  const float COLOUR_DELTA_BASE = 0.5f;
  const float DELTA_SCALE = 0.75f;
#else
  const float COLOUR_DELTA_BASE = 0.65f;
  const float DELTA_SCALE = 1.0f;
#endif
  const float COLOUR_DELTA_CONTRAST = 100.f;
  const float MAX_LERP_FACTOR = 0.66f;

  float4 texelDelta = psTapOffs_Packed * tweakables.w;

  float3 a = SampleFramebuffer(fbTexCoord + texelDelta.yz);
  float3 b = SampleFramebuffer(fbTexCoord + texelDelta.xy);
  float3 c = SampleFramebuffer(fbTexCoord - texelDelta.yz);
  float3 d = SampleFramebuffer(fbTexCoord - texelDelta.xy);
#if (AA_QUALITY_MODE == 1)
  float3 e = SampleFramebuffer(fbTexCoord + texelDelta.wz);
  float3 f = SampleFramebuffer(fbTexCoord - texelDelta.wz);
  texelDelta.y = texelDelta.z;
  float3 g = SampleFramebuffer(fbTexCoord + texelDelta.xy);
  float3 h = SampleFramebuffer(fbTexCoord - texelDelta.xy);
#endif

  float3 dA = a - baseColor;
  float3 dB = b - baseColor;
  float3 dC = c - baseColor;
  float3 dD = d - baseColor;
  float4 deltas = float4(dot(dA, dA), dot(dB, dB), dot(dC, dC), dot(dD, dD));
  deltas = sqrt(DELTA_SCALE * DELTA_SCALE * (deltas / 3.f));
  float4 weights = deltas;
#if (AA_QUALITY_MODE == 1)
  float3 dE = e - baseColor;
  float3 dF = f - baseColor;
  float3 dG = g - baseColor;
  float3 dH = h - baseColor;
  float4 deltas2 = float4(dot(dE, dE), dot(dF, dF), dot(dG, dG), dot(dH, dH));
  deltas2 = sqrt(DELTA_SCALE * DELTA_SCALE * (deltas2 / 3.f));
  float4 weights2 = deltas2;
#endif

  float4 lumS = float4(dot(a, a), dot(b, b), dot(c, c), dot(d, d));
  lumS.xy = max(lumS.xy, lumS.wz);
  lumS.x = max(lumS.x, lumS.y);
  float maxLumS = max(lumS.x, dot(baseColor, baseColor));
#if (AA_QUALITY_MODE == 1)
  lumS = float4(dot(e, e), dot(f, f), dot(g, g), dot(h, h));
  lumS.xy = max(lumS.xy, lumS.wz);
  lumS.x = max(lumS.x, lumS.y);
  maxLumS = max(lumS.x, maxLumS);
#endif
  float lumScale = rsqrt(max(maxLumS, 1e-10f));
  weights *= lumScale;
#if (AA_QUALITY_MODE == 1)
  weights2 *= lumScale;
#endif

  float colourDeltaBase = tweakables.z * COLOUR_DELTA_BASE;
  weights = saturate(colourDeltaBase + COLOUR_DELTA_CONTRAST * (weights - colourDeltaBase));
#if (AA_QUALITY_MODE == 1)
  weights2 = saturate(colourDeltaBase + COLOUR_DELTA_CONTRAST * (weights2 - colourDeltaBase));
#endif

  float unlikeSum = dot(weights, 1.f);
  float3 unlike = weights.x * a + weights.y * b + weights.z * c + weights.w * d;
#if (AA_QUALITY_MODE == 1)
  unlikeSum += dot(weights2, 1.f);
  unlike += weights2.x * e + weights2.y * f + weights2.z * g + weights2.w * h;
#endif
  unlike = (unlikeSum > 0.f) ? unlike / unlikeSum : baseColor;

  float adjustedUnlikeSum = unlikeSum;
#if (AA_REDUCE_ONE_PIXEL_LINE_BLUR == 1)
  const float ONE_PIXEL_LINE_BIAS_BASE = 0.4f;
  const float ONE_PIXEL_LINE_BIAS_CONTRAST = 16.f;
  float2 unlikeCentroid = 0.f;
  unlikeCentroid.x += dot(1.f - weights, float4(0, +1, 0, -1));
  unlikeCentroid.y += dot(1.f - weights, float4(+1, 0, -1, 0));
#if (AA_QUALITY_MODE == 0)
  unlikeCentroid /= 4.f - unlikeSum;
#else
  unlikeCentroid.x += dot(1.f - weights2, float4(-1, +1, +1, -1));
  unlikeCentroid.y += dot(1.f - weights2, float4(+1, -1, +1, -1));
  unlikeCentroid /= 8.f - unlikeSum;
#endif
  float onePixelLineBias = 1.f - saturate(length(unlikeCentroid));
  onePixelLineBias = tweakables.y * saturate(ONE_PIXEL_LINE_BIAS_BASE + ONE_PIXEL_LINE_BIAS_CONTRAST * (onePixelLineBias - ONE_PIXEL_LINE_BIAS_BASE));
#if (AA_QUALITY_MODE == 0)
  adjustedUnlikeSum -= 2.f * onePixelLineBias * 0.4f * saturate(3.f - unlikeSum);
#else
  adjustedUnlikeSum -= 2.f * onePixelLineBias * 1.9f * saturate(7.f - unlikeSum);
#endif
#endif

#if (AA_QUALITY_MODE == 0)
  float lerpFactor = saturate(tweakables.x * DELTA_SCALE * ((adjustedUnlikeSum - 1.f) / 3.f));
#else
  float lerpFactor = saturate(tweakables.x * DELTA_SCALE * ((adjustedUnlikeSum - 3.f) / 3.f));
#endif
  lerpFactor = min(lerpFactor, MAX_LERP_FACTOR);
  return lerp(baseColor, unlike, lerpFactor);
}
#endif

float4 main(float2 baseTexCoord : TEXCOORD0) : COLOR0 {
  float2 fbTexCoord = baseTexCoord * uvTransform.wz + uvTransform.xy;
  float3 outColor = SampleFramebuffer(fbTexCoord);

#if (AA_ENABLE == 1)
  outColor = PerformAA(outColor, fbTexCoord);
#endif

  float bloom_valid = IsHdrPipeline() ? RENODX_BLOOM_VALID : 1.f;
  outColor += BloomFactor.x * tex2D(BaseTextureSampler, baseTexCoord).rgb * bloom_valid;

  if (!IsHdrPipeline()) {
    return float4(PerformColorCorrection(outColor), 1.f);
  }

  float3 untonemapped = renodx::color::srgb::Decode(max(outColor, 0.f));
#if (COL_CORRECT_NUM_LOOKUPS > 0)
  float3 neutral_sdr;
  if (RENODX_TONE_MAP_TYPE == 0.f) {
    neutral_sdr = saturate(untonemapped);
  } else {
    neutral_sdr = untonemapped / max(1.f, renodx::math::Max(untonemapped));
  }
  float3 graded_sdr = renodx::color::srgb::DecodeSafe(
      PerformColorCorrection(renodx::color::srgb::Encode(neutral_sdr)));
  graded_sdr = lerp(neutral_sdr, graded_sdr, RENODX_COLOR_GRADE_STRENGTH);
  return float4(ToneMapScene(untonemapped, graded_sdr, neutral_sdr), 1.f);
#else
  return float4(ToneMapScene(untonemapped), 1.f);
#endif
}
