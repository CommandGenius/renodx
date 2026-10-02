#include "./common.hlsl"

sampler2D RT : register(s0);

float4 main(float2 uv : TEXCOORD0) : COLOR0 {
  float4 color = tex2D(RT, uv);
  return float4(GammaSpaceOutput(color.rgb), color.a);
}
