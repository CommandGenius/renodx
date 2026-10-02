// Garry's Mod HTML panel blit: oC0 = tex2D(RT, uv). The texture is premultiplied (blended
// ONE / INVSRCALPHA), so the gamma decode has to run on the straight colour.
#include "./common.hlsl"

sampler2D RT : register(s0);

float4 main(float2 uv : TEXCOORD0) : COLOR0 {
  float4 color = tex2D(RT, uv);
  if (color.a > 0.f) color.rgb = GammaSpaceOutput(color.rgb / color.a) * color.a;
  return color;
}
