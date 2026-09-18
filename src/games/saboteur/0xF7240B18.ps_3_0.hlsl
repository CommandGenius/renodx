#include "./common.hlsl"

float4 g_vZConvert  : register(c31);
float4 g_Resolution : register(c52);

sampler2D DiffuseSampler : register(s0);
sampler2D DepthSampler   : register(s1);

float4 main(float2 uv : TEXCOORD0, float4 proj : TEXCOORD1, float boost : TEXCOORD2, float4 vertex_color : COLOR0) : COLOR0 {
  float inv_w = 1.f / proj.w;
  float2 screen = proj.xy * inv_w + 1.f;
  screen = float2(screen.x * 0.5f, screen.y * -0.5f + 1.f) + g_Resolution.xy * 0.5f;
  float pixel_z = proj.z * -inv_w + g_vZConvert.x;
  float scene_z = abs(tex2D(DepthSampler, screen).x);
  float zalpha = saturate((g_vZConvert.y * -(1.f / pixel_z) + scene_z) * 0.5f);

  float4 color = tex2D(DiffuseSampler, uv) * vertex_color;
  color = color * boost + color;

  float4 o;
  o.xyz = ScaleToGameBrightness((RENODX_TONE_MAP_TYPE != 0) ? ClipColorKeepHeadroom(color.xyz, 2.f) : saturate(color.xyz));
  o.w = (-abs(g_vZConvert.z - 1.f) >= 0.f) ? zalpha * color.w : color.w;
  o.w = saturate(o.w + InjectionGuard());
  return o;
}
