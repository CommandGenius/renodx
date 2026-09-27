#include "./common.hlsl"

float4 g_vComparisonMinMaxScale : register(c0);

struct PS_OUTPUT {
  float4 color : COLOR0;
  float depth : DEPTH;
};

PS_OUTPUT main(float2 uv0 : TEXCOORD0) {
  PS_OUTPUT o;
  o.color = step(g_vComparisonMinMaxScale.x, 0.f) * step(0.f, g_vComparisonMinMaxScale.y);
  o.depth = 0.f;
  return o;
}
