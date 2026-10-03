#ifndef SRC_SOURCEENGINE_COMMON_HLSL_
#define SRC_SOURCEENGINE_COMMON_HLSL_

#include "../../shaders/tonemap/psychov/test30.hlsl"
#include "./shared.h"

bool IsHdrPipeline() {
  return RENODX_PEAK_WHITE_NITS > 0.f;
}

float3 BoundHdr(float3 color) {
  color = (color > -1e-4f) ? color : 0.f;
  color = (color < 64.f) ? color : 63.99f;
  return color;
}

float3 DisplayScene(float3 fb_color) {
  return IsHdrPipeline() ? fb_color * RENODX_SCENE_EXPOSURE : fb_color;
}

float3 FramebufferToGamma(float3 fb_color) {
  if (!IsHdrPipeline()) return fb_color;
  return renodx::color::srgb::Encode(BoundHdr(DisplayScene(fb_color)));
}

float3 FramebufferToGammaClamped(float3 fb_color) {
  return saturate(FramebufferToGamma(fb_color));
}

float3 PsychoVToneMap(float3 untonemapped) {
  float peak = RENODX_PEAK_WHITE_NITS / RENODX_DIFFUSE_WHITE_NITS;
  if (RENODX_GAMMA_CORRECTION != 0.f) {
    peak = renodx::color::correct::Gamma(peak, RENODX_GAMMA_CORRECTION > 0.f, abs(RENODX_GAMMA_CORRECTION) == 1.f ? 2.2f : 2.4f);
  }
  return renodx::tonemap::psychov::psychotm_test30(
      untonemapped,
      peak,
      RENODX_TONE_MAP_EXPOSURE,
      RENODX_TONE_MAP_HIGHLIGHTS,
      RENODX_TONE_MAP_SHADOWS,
      RENODX_TONE_MAP_CONTRAST,
      RENODX_TONE_MAP_SATURATION,
      1.f,
      100.f,
      1.f,
      1.f,
      0,
      RENODX_PSYCHOV_CONE_RESPONSE,
      0.18f,
      0.18f,
      RENODX_PSYCHOV_GAMUT_COMPRESSION,
      (int)RENODX_PSYCHOV_TARGET_GAMUT,
      1.f,
      RENODX_PSYCHOV_COMPRESSION,
      (int)RENODX_PSYCHOV_SOURCE_BOUNDARY);
}

float3 ToneMapScene(float3 untonemapped, float3 graded_sdr, float3 neutral_sdr) {
  if (RENODX_TONE_MAP_TYPE == 0.f) {
    return renodx::draw::RenderIntermediatePass(saturate(graded_sdr));
  }
  if (RENODX_TONE_MAP_TYPE == 3.f) {
    return renodx::draw::RenderIntermediatePass(PsychoVToneMap(renodx::draw::ComputeUntonemappedGraded(untonemapped, graded_sdr, neutral_sdr)));
  }
  return renodx::draw::RenderIntermediatePass(renodx::draw::ToneMapPass(untonemapped, graded_sdr, neutral_sdr));
}

float3 ToneMapScene(float3 untonemapped) {
  if (RENODX_TONE_MAP_TYPE == 0.f) {
    return renodx::draw::RenderIntermediatePass(saturate(untonemapped));
  }
  if (RENODX_TONE_MAP_TYPE == 3.f) {
    return renodx::draw::RenderIntermediatePass(PsychoVToneMap(untonemapped));
  }
  return renodx::draw::RenderIntermediatePass(renodx::draw::ToneMapPass(untonemapped));
}

float3 GammaSpaceOutput(float3 color) {
  if (RENODX_SRGB_WRITE_OFF == 1.f) return renodx::color::srgb::Decode(saturate(color));
  return color;
}

#endif  // SRC_SOURCEENGINE_COMMON_HLSL_
