#include "./common.hlsl"

sampler2D TexSampler : register(s0);
float4 g_vComparisonMinMaxScale : register(c0);

struct PS_OUTPUT {
  float4 color : COLOR0;
  float depth : DEPTH;
};

PS_OUTPUT main(float2 uv0 : TEXCOORD0, float2 position : VPOS) {
  const float2 pixel = floor(position);
  float luminance = 0.6f;
  if (RENODX_HISTOGRAM_MODE == 3.f) {
    const float cell = (fmod(pixel.x * 5.f + pixel.y * 19.f, 64.f) + 0.5f) / 64.f;
    luminance = cell < RENODX_HISTOGRAM_HOLD_LOW_SHARE ? RENODX_HISTOGRAM_HOLD_LOW : RENODX_HISTOGRAM_HOLD_HIGH;
  } else if (RENODX_HISTOGRAM_MODE != 1.f) {
    float3 fb = tex2D(TexSampler, uv0).rgb;
    float3 color = (RENODX_LINEAR_INPUT == 1.f ? saturate(fb) : FramebufferToGammaClamped(fb)) * g_vComparisonMinMaxScale.z;
    luminance = dot(color, float3(0.2125f, 0.7154f, 0.0721f));
    if (RENODX_HISTOGRAM_MODE == 2.f && fmod(pixel.x + 3.f * pixel.y, 10.f) >= 1.f) luminance = min(luminance, 0.55f);
  }

  PS_OUTPUT o;
  o.color = step(g_vComparisonMinMaxScale.x, luminance) * step(luminance, g_vComparisonMinMaxScale.y);
  o.depth = 0.f;
  return o;
}
