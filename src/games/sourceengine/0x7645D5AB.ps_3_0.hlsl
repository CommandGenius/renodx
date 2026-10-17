#include "./common.hlsl"

sampler2D MaskSampler : register(s0);
float4 Weights : register(c0);

float4 main(float2 uv : TEXCOORD0) : COLOR0 {
  const float factor = dot(tex2D(MaskSampler, uv), Weights);
  if (!IsHdrPipeline()) return float4(factor.xxx, 1.f);
  return float4(pow(max(factor, 0.f), 2.2f).xxx, 1.f);
}
