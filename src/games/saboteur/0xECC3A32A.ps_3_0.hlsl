#include "./common.hlsl"

float4 c0 : register(c0);
float4 c1 : register(c1);
float4 c2 : register(c2);
float4 c3 : register(c3);
float4 c4 : register(c4);
float4 c52 : register(c52);

sampler2D s0 : register(s0);
sampler2D s1 : register(s1);
sampler2D s2 : register(s2);

float4 main(float4 v0 : COLOR0, float4 v1 : TEXCOORD0, float4 v2 : TEXCOORD1, float4 v3 : TEXCOORD2, float2 vpos : VPOS) : COLOR0 {
  static const float4 c5 = float4(2, -2, -1, 1);
  static const float4 c6 = float4(0, 0.5, 0, 0);
  float4 vPos = float4(vpos, 0.f, 0.f);
  float4 r0 = 0.f, r1 = 0.f, r2 = 0.f;
  float4 o0 = 0.f;
  r0.zw = (c6.xxxx).zw;
  r0.xy = (c52.xyzw * vPos.xyzw).xy;
  r1.xy = (c52.xyzw).xy;
  r1.xyzw = (r1.xyxx * c6.yyxx + r0.xyzw).xyzw;
  r0.xy = (r0.xyzw * c5.xyzw + c5.zwzw).xy;
  r1.xyzw = (tex2Dlod(s2, r1.xyzw)).xyzw;
  r0.z = (r1.xxxx + c0.zzzz).z;
  r0.z = (1.f / r0.zzzz).z;
  r0.z = (r0.zzzz * c0.wwww).z;
  r0.xy = (r0.xyzw * r0.zzzz).xy;
  r0.xy = (r0.xyzw * c0.xyzw).xy;
  r1.xyz = (r0.yyyy * c2.xyzw).xyz;
  r0.xyw = (c1.xyzz * r0.xxxx + r1.xyzz).xyw;
  r0.xyz = (c3.xyzw * r0.zzzz + r0.xyww).xyz;
  r0.xyz = (r0.xyzw + c4.xyzw).xyz;
  r0.xyz = (r0.xyzw + (-v3.xyzw)).xyz;
  r0.x = (dot((r0.xyzw).xyz, (r0.xyzw).xyz));
  r0.x = (rsqrt(abs(r0.xxxx))).x;
  r0.w = saturate(1.f / r0.xxxx).w;
  r1.xyzw = (tex2D(s1, (v2.xyzw).xy)).xyzw;
  r2.xyzw = (tex2D(s0, (v1.xyzw).xy)).xyzw;
  r1.xyzw = (r2.xyzw * r1.xyzw + (-r2.xyzw)).xyzw;
  r1.xyzw = (v0.wwww * r1.xyzw + r2.xyzw).xyzw;
  r0.xyz = (v0.xyzw).xyz;
  o0.xyzw = (r0.xyzw * r1.xyzw).xyzw;

  o0.xyz = ScaleToGameBrightness((RENODX_TONE_MAP_TYPE != 0) ? ClipColorKeepHeadroom(o0.xyz, 2.f) : saturate(o0.xyz));
  o0.w = saturate(o0.w + InjectionGuard());
  return o0;
}
