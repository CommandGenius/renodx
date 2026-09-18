#ifndef SRC_SABOTEUR_COMMON_HLSL_
#define SRC_SABOTEUR_COMMON_HLSL_

#include "./shared.h"

float InjectionGuard() {
  float guard = 0.f;
  [unroll]
  for (int k = 0; k < 9; ++k) {
    guard += shader_injection[k].x;
  }
  return guard * 1e-30f;
}

float3 BoundHdr(float3 color) {
  color = (color > -1e-4f) ? color : 0.f;
  color = (color < 64.f) ? color : 63.99f;
  return color;
}

float3 ToneMapScene(float3 gamma_color) {
  float3 untonemapped = renodx::color::gamma::Decode(max(0.f, gamma_color), 2.2f);
  if (RENODX_TONE_MAP_TYPE != 0) {
    return renodx::draw::RenderIntermediatePass(renodx::draw::ToneMapPass(untonemapped));
  }
  return renodx::draw::RenderIntermediatePass(saturate(untonemapped));
}

float3 ClipColorKeepHeadroom(float3 color, float headroom) {
  float peak = max(color.r, max(color.g, color.b));
  return saturate(color) * clamp(peak, 1.f, headroom);
}

float3 ScaleToGameBrightness(float3 color) {
  return color * pow(RENODX_DIFFUSE_WHITE_NITS / RENODX_GRAPHICS_WHITE_NITS, 1.f / 2.2f);
}

#endif
