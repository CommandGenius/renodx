float g_fMiddleGray           : register(c0);
float g_fAdaptedLum           : register(c1);
float g_fBrightPassThreshold  : register(c2);

sampler2D DIFFUSEMAP_SAMPLER : register(s0);

float4 main(float2 uv : TEXCOORD0) : COLOR0 {
  float4 r0 = tex2D(DIFFUSEMAP_SAMPLER, uv);

  r0.rgb = saturate(r0.rgb);

  float scale = g_fMiddleGray / (g_fAdaptedLum + 0.001f);
  r0.rgb = max(r0.rgb * scale - g_fBrightPassThreshold, 0.f);

  return r0;
}
