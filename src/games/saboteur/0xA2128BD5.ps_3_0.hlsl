#include "./common.hlsl"

float4 c52 : register(c52);

sampler2D s0 : register(s0);
sampler2D s2 : register(s2);

float4 main(float4 v0 : TEXCOORD0, float4 v1 : TEXCOORD2, float4 v2 : COLOR0, float2 vpos : VPOS) : COLOR0 {
  static const float4 c0 = float4(1, 0, 0, 0);
  float4 vPos = float4(vpos, 0.f, 0.f);
  float4 r0 = 0.f, r1 = 0.f;
  float4 o0 = 0.f;
  r0.xy = (c52.xyzw * vPos.xyzw).xy;
  r0.xyzw = (tex2D(s2, (r0.xyzw).xy)).xyzw;
  r0.x = saturate((-r0.wwww) + c0.xxxx).x;
  r1.xyzw = (tex2D(s0, (v0.xyzw).xy)).xyzw;
  r1.xyzw = (r1.xyzw * v2.xyzw).xyzw;
  r1.xyzw = (r1.xyzw * v1.xxxx + r1.xyzw).xyzw;
  o0.w = (r0.xxxx * r1.wwww).w;
  o0.xyz = (r1.xyzw).xyz;

  o0.xyz = ScaleToGameBrightness((RENODX_TONE_MAP_TYPE != 0) ? ClipColorKeepHeadroom(o0.xyz, 2.f) : saturate(o0.xyz));
  o0.w = saturate(o0.w + InjectionGuard());
  return o0;
}
