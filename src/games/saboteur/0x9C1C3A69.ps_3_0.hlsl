#include "./common.hlsl"

float4 c52 : register(c52);

sampler2D s0 : register(s0);
sampler2D s1 : register(s1);

float4 main(float4 v0 : COLOR0, float4 v1 : TEXCOORD0, float2 vpos : VPOS) : COLOR0 {
  static const float4 c0 = float4(1, 0.699999988, 0.200000003, 0);
  static const float4 c1 = float4(0.600000024, 0.300000012, 0, 0);
  float4 vPos = float4(vpos, 0.f, 0.f);
  float4 r0 = 0.f, r1 = 0.f, r2 = 0.f;
  float4 o0 = 0.f;
  r0.xy = (c52.xyzw * vPos.xyzw).xy;
  r0.xyzw = (tex2D(s1, (r0.xyzw).xy)).xyzw;
  r0.x = ((-r0.wwww) + c0.xxxx).x;
  r0.y = (r0.wwww * c1.xxxx + c1.yyyy).y;
  r0.x = (r0.xxxx * c0.yyyy + c0.zzzz).x;
  r1.x = (lerp(r0.xxxx, c0.xxxx, v0.xxxx)).x;
  r2.xyzw = (tex2D(s0, (v1.xyzw).xy)).xyzw;
  r2.xyzw = (r2.xyzw * v0.yyyw).xyzw;
  o0.xyz = (r1.xxxx * r2.xyzw).xyz;
  r1.x = (lerp(r0.yyyy, c0.xxxx, v0.xxxx)).x;
  o0.w = (r2.wwww * r1.xxxx).w;

  o0.xyz = ScaleToGameBrightness(o0.xyz);
  o0.w = saturate(o0.w + InjectionGuard());
  return o0;
}
