#include "./common.hlsl"

sampler2D SceneSampler : register(s0);

float4 main(float2 uv : TEXCOORD0) : COLOR0 {
  float4 scene = tex2D(SceneSampler, uv);
  return float4(saturate(renodx::draw::InvertIntermediatePass(scene.rgb)), scene.a);
}
