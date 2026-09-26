#include "./shared.h"

Texture2D t0 : register(t0);
SamplerState s0 : register(s0);
float4 main(float4 vpos: SV_POSITION, float2 uv: TEXCOORD0)
    : SV_TARGET {
  float4 color = t0.Sample(s0, uv);
  if (RENODX_SCENE_TONE_MAPPED == 0.f) color.rgb = saturate(color.rgb);
  return renodx::draw::SwapChainPass(color);
}
