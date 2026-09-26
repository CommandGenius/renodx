#include "./common.hlsl"

sampler2D TexSampler : register(s0);
float4 g_vComparisonMinMaxScale : register(c0);

struct PS_OUTPUT {
  float4 color : COLOR0;
  float depth : DEPTH;
};

PS_OUTPUT main(float2 uv0 : TEXCOORD0) {
  float3 color = FramebufferToGammaClamped(tex2D(TexSampler, uv0).rgb) * g_vComparisonMinMaxScale.z;
  float flLuminance = dot(color, float3(0.2125f, 0.7154f, 0.0721f));

  PS_OUTPUT o;
  o.color = step(g_vComparisonMinMaxScale.x, flLuminance) * step(flLuminance, g_vComparisonMinMaxScale.y);
  o.depth = 0.f;
  return o;
}
