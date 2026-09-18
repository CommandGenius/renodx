#include "./common.hlsl"

float4 c0 : register(c0);
float4 c52 : register(c52);

sampler2D s0 : register(s0);

float4 main(float4 v0 : TEXCOORD0) : COLOR0 {
  static const float4 c1 = float4(0.600000024, 1.17999995, 0.219999999, 1);
  static const float4 c2 = float4(0.300000012, 0.589999974, 0.109999999, 0);
  static const float4 c3 = float4(1, 0, -1, 0.5);
  float4 r0 = 0.f, r1 = 0.f, r2 = 0.f, r3 = 0.f, r4 = 0.f;
  float4 o0 = 0.f;
  r0.xy = (c52.xyzw).xy;
  r0.zw = (r0.xxxx * (-c3.xyxy) + v0.xyxy).zw;
  r1.xyzw = (tex2D(s0, (r0.zwzw).xy)).xyzw;
  r0.zw = (r0.xxxx * c3.xyxy + v0.xyxy).zw;
  r2.xyzw = (tex2D(s0, (r0.zwzw).xy)).xyzw;
  r1.xyz = (r1.xyzw + r2.xyzw).xyz;
  r0.zw = (r0.xyxy * (-c3.xyyx) + v0.xyxy).zw;
  r2.xyzw = (tex2D(s0, (r0.zwzw).xy)).xyzw;
  r0.zw = (r0.xyxy * c3.xyyx + v0.xyxy).zw;
  r3.xyzw = (tex2D(s0, (r0.zwzw).xy)).xyzw;
  r2.xyz = (r2.xyzw + r3.xyzw).xyz;
  r0.z = (dot((r1.xyzw).xyz, (c2.xyzw).xyz));
  r3.xyzw = (tex2D(s0, (v0.xyzw).xy)).xyzw;
  r0.w = (dot((r3.xyzw).xyz, (c1.xyzw).xyz));
  r1.w = ((-r0.zzzz) + r0.wwww).w;
  r0.z = (dot((r2.xyzw).xyz, (c2.xyzw).xyz));
  r2.w = (r0.wwww + (-r0.zzzz)).w;
  r0.z = ((-r1.wwww) + r2.wwww).z;
  r1.xyzw = (((r0.zzzz) >= 0.f ? r2.xyzw : r1.xyzw)).xyzw;
  r2.xy = (c52.xyzw + v0.xyzw).xy;
  r2.xyzw = (tex2D(s0, (r2.xyzw).xy)).xyzw;
  r4.xy = (r0.xyzw * (-c1.wwww) + v0.xyzw).xy;
  r4.xyzw = (tex2D(s0, (r4.xyzw).xy)).xyzw;
  r2.xyz = (r2.xyzw + r4.xyzw).xyz;
  r0.z = (dot((r2.xyzw).xyz, (c2.xyzw).xyz));
  r2.w = (r0.wwww + (-r0.zzzz)).w;
  r0.z = ((-r1.wwww) + r2.wwww).z;
  r1.xyzw = (((r0.zzzz) >= 0.f ? r2.xyzw : r1.xyzw)).xyzw;
  r2.xy = (r0.xyzw * c3.xzzw + v0.xyzw).xy;
  r2.xyzw = (tex2D(s0, (r2.xyzw).xy)).xyzw;
  r0.xy = (r0.xyzw * c3.zxzw + v0.xyzw).xy;
  r4.xyzw = (tex2D(s0, (r0.xyzw).xy)).xyzw;
  r2.xyz = (r2.xyzw + r4.xyzw).xyz;
  r0.x = (dot((r2.xyzw).xyz, (c2.xyzw).xyz));
  r2.w = (r0.wwww + (-r0.xxxx)).w;
  r0.x = ((-r1.wwww) + r2.wwww).x;
  r0.xyzw = (((r0.xxxx) >= 0.f ? r2.xyzw : r1.xyzw)).xyzw;
  r0.xyz = (r0.xyzw * c3.wwww + (-r3.xyzw)).xyz;
  r1.x = (1.f / (c0.yyyy * ScaleToGameBrightness(1.f).x)).x;
  r0.w = saturate(r0.wwww * r1.xxxx).w;
  o0.xyz = (r0.wwww * r0.xyzw + r3.xyzw).xyz;
  o0.w = (c1.wwww).w;

  o0.w += InjectionGuard();
  return o0;
}
