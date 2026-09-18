#include "./common.hlsl"

sampler2D g_Sampler0 : register(s0);

float4 main(float2 uv : TEXCOORD0) : COLOR0 {
  float4 inColor = tex2D(g_Sampler0, uv);

  float tmp = inColor.w * 0.75f + 0.5f;
  float fractional = frac(tmp);
  float whole = tmp - fractional;
  float round_up = (-fractional >= 0.f) ? 0.f : 1.f;
  float negative = (tmp >= 0.f) ? 0.f : 1.f;
  tmp = negative * round_up + whole;

  float3 color = inColor.rgb * 0.25f;
  color = (color > -1e-4f) ? color : 0.f;
  color = (color < 64.f) ? color : 63.99f;


  float4 o;
  o.xyz = color;
  o.w = saturate(tmp * 0.333333343f + InjectionGuard());
  return o;
}
