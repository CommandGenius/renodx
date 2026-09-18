#include "./common.hlsl"

float4 c0 : register(c0);
float4 c52 : register(c52);

sampler2D s0 : register(s0);

float4 main(float4 v0 : TEXCOORD0) : COLOR0 {
  static const float4 c1 = float4(0.0625, 0, 0, 0);
  static const float4 c2 = float4(5, 1, 9, 13);
  float4 r0 = 0.f, r1 = 0.f, r2 = 0.f;
  float4 o0 = 0.f;
  r0.xy = (c52.xyzw + v0.xyzw).xy;
  r0.xyzw = saturate(tex2D(s0, (r0.xyzw).xy)).xyzw;
  r1.xy = (c52.xyzw).xy;
  r1.zw = (r1.xyxy * c2.xyxy + v0.xyxy).zw;
  r2.xyzw = saturate(tex2D(s0, (r1.zwzw).xy)).xyzw;
  r0.xyz = (r0.xyzw + r2.xyzw).xyz;
  r1.zw = (r1.xyxy * c2.xyzy + v0.xyxy).zw;
  r2.xyzw = saturate(tex2D(s0, (r1.zwzw).xy)).xyzw;
  r0.xyz = (r0.xyzw + r2.xyzw).xyz;
  r1.zw = (r1.xyxy * c2.xywy + v0.xyxy).zw;
  r2.xyzw = saturate(tex2D(s0, (r1.zwzw).xy)).xyzw;
  r0.xyz = (r0.xyzw + r2.xyzw).xyz;
  r1.zw = (r1.xyxy * c2.xyyx + v0.xyxy).zw;
  r2.xyzw = saturate(tex2D(s0, (r1.zwzw).xy)).xyzw;
  r0.xyz = (r0.xyzw + r2.xyzw).xyz;
  r1.zw = (r1.xyxy * c2.xxxx + v0.xyxy).zw;
  r2.xyzw = saturate(tex2D(s0, (r1.zwzw).xy)).xyzw;
  r0.xyz = (r0.xyzw + r2.xyzw).xyz;
  r1.zw = (r1.xyxy * c2.xyzx + v0.xyxy).zw;
  r2.xyzw = saturate(tex2D(s0, (r1.zwzw).xy)).xyzw;
  r0.xyz = (r0.xyzw + r2.xyzw).xyz;
  r1.zw = (r1.xyxy * c2.xywx + v0.xyxy).zw;
  r2.xyzw = saturate(tex2D(s0, (r1.zwzw).xy)).xyzw;
  r0.xyz = (r0.xyzw + r2.xyzw).xyz;
  r1.zw = (r1.xyxy * c2.xyyz + v0.xyxy).zw;
  r2.xyzw = saturate(tex2D(s0, (r1.zwzw).xy)).xyzw;
  r0.xyz = (r0.xyzw + r2.xyzw).xyz;
  r1.zw = (r1.xyxy * c2.xyxz + v0.xyxy).zw;
  r2.xyzw = saturate(tex2D(s0, (r1.zwzw).xy)).xyzw;
  r0.xyz = (r0.xyzw + r2.xyzw).xyz;
  r1.zw = (r1.xyxy * c2.zzzz + v0.xyxy).zw;
  r2.xyzw = saturate(tex2D(s0, (r1.zwzw).xy)).xyzw;
  r0.xyz = (r0.xyzw + r2.xyzw).xyz;
  r1.zw = (r1.xyxy * c2.xywz + v0.xyxy).zw;
  r2.xyzw = saturate(tex2D(s0, (r1.zwzw).xy)).xyzw;
  r0.xyz = (r0.xyzw + r2.xyzw).xyz;
  r1.zw = (r1.xyxy * c2.xyyw + v0.xyxy).zw;
  r2.xyzw = saturate(tex2D(s0, (r1.zwzw).xy)).xyzw;
  r0.xyz = (r0.xyzw + r2.xyzw).xyz;
  r1.zw = (r1.xyxy * c2.xyxw + v0.xyxy).zw;
  r2.xyzw = saturate(tex2D(s0, (r1.zwzw).xy)).xyzw;
  r0.xyz = (r0.xyzw + r2.xyzw).xyz;
  r1.zw = (r1.xyxy * c2.xyzw + v0.xyxy).zw;
  r2.xyzw = saturate(tex2D(s0, (r1.zwzw).xy)).xyzw;
  r0.xyz = (r0.xyzw + r2.xyzw).xyz;
  r1.xy = (r1.xyzw * c2.wwww + v0.xyzw).xy;
  r1.xyzw = saturate(tex2D(s0, (r1.xyzw).xy)).xyzw;
  r0.xyz = (r0.xyzw + r1.xyzw).xyz;
  r0.yzw = (r0.xxyz * c1.xxxx).yzw;
  r1.x = (max(r0.zzzz, r0.wwww)).x;
  r0.x = (r0.xxxx * c1.xxxx + (-r1.xxxx)).x;
  r0.x = (((r0.xxxx) >= 0.f ? r0.yyyy : r1.xxxx)).x;
  r1.x = (r0.xxxx + (-c0.yyyy)).x;
  r1.y = (1.f / c0.xxxx).y;
  r1.x = saturate(r1.xxxx * r1.yyyy).x;
  r0.x = (r0.xxxx * r1.xxxx + (-c2.yyyy)).x;
  r0.yzw = (r0.xyzw * r1.xxxx).yzw;
  r1.y = (c2.yyyy).y;
  o0.w = (c0.zzzz * r0.xxxx + r1.yyyy).w;
  o0.xyz = (r0.yzww).xyz;

  o0 = saturate(o0);
  o0.w = saturate(o0.w + InjectionGuard());
  return o0;
}
