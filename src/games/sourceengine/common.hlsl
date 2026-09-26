#ifndef SRC_SOURCEENGINE_COMMON_HLSL_
#define SRC_SOURCEENGINE_COMMON_HLSL_

#include "./shared.h"

bool IsHdrPipeline() {
  return RENODX_PEAK_WHITE_NITS > 0.f;
}

float3 BoundHdr(float3 color) {
  color = (color > -1e-4f) ? color : 0.f;
  color = (color < 64.f) ? color : 63.99f;
  return color;
}

float3 FramebufferToGamma(float3 fb_color) {
  if (!IsHdrPipeline()) return fb_color;
  return renodx::color::srgb::Encode(BoundHdr(fb_color));
}

float3 FramebufferToGammaClamped(float3 fb_color) {
  return saturate(FramebufferToGamma(fb_color));
}

float3 ToneMapScene(float3 untonemapped, float3 graded_sdr, float3 neutral_sdr) {
  untonemapped *= RENODX_SCENE_EXPOSURE;
  if (RENODX_TONE_MAP_TYPE == 0.f) {
    return renodx::draw::RenderIntermediatePass(saturate(graded_sdr * RENODX_SCENE_EXPOSURE));
  }
  return renodx::draw::RenderIntermediatePass(renodx::draw::ToneMapPass(untonemapped, graded_sdr, neutral_sdr));
}

float3 ToneMapScene(float3 untonemapped) {
  untonemapped *= RENODX_SCENE_EXPOSURE;
  if (RENODX_TONE_MAP_TYPE == 0.f) {
    return renodx::draw::RenderIntermediatePass(saturate(untonemapped));
  }
  return renodx::draw::RenderIntermediatePass(renodx::draw::ToneMapPass(untonemapped));
}

float3 GammaSpaceOutput(float3 color) {
  if (RENODX_SRGB_WRITE_OFF == 1.f) return renodx::color::srgb::Decode(saturate(color));
  return color;
}

#endif  // SRC_SOURCEENGINE_COMMON_HLSL_
