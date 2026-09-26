#include "./common.hlsl"

sampler2D SceneSampler : register(s0);

float4 main(float2 uv : TEXCOORD0) : COLOR0 {
  return float4(FramebufferToGammaClamped(tex2D(SceneSampler, uv).rgb), 1.f);
}
