#include "./common.hlsl"

sampler2D TexSampler : register(s0);
float4 params : register(c0);

float4 Shape(float2 uv) {
  float4 pixel = tex2D(TexSampler, uv);
  pixel.rgb = FramebufferToGammaClamped(pixel.rgb);
  float lum = dot(pixel.xyz, params.xyz);
  pixel.xyz = pow(pixel.xyz, params.w) * lum;
  return pixel;
}

float4 main(
    float2 coordTap0 : TEXCOORD0,
    float2 coordTap1 : TEXCOORD1,
    float2 coordTap2 : TEXCOORD2,
    float2 coordTap3 : TEXCOORD3) : COLOR0 {
  float4 s0 = Shape(coordTap0);
  float4 s1 = Shape(coordTap1);
  float4 s2 = Shape(coordTap2);
  float4 s3 = Shape(coordTap3);
  return (s0 + s1 + s2 + s3) * 0.25f;
}
