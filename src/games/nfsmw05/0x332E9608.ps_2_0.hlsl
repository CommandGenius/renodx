float4 g_avSampleOffsets[16] : register(c0);
float3 LUMINANCE_VECTOR      : register(c16);

sampler2D DIFFUSEMAP_SAMPLER : register(s0);

struct PS_OUT {
  float4 color0 : COLOR0;
  float4 color1 : COLOR1;
};

PS_OUT main(float2 uv : TEXCOORD0) {
  float4 sum = 0.f;
  [unroll]
  for (int i = 0; i < 16; ++i) {
    sum += saturate(tex2D(DIFFUSEMAP_SAMPLER, uv + g_avSampleOffsets[i].xy));
  }

  float4 r1;
  r1.rgb = sum.rgb;
  r1.a = dot(sum.rgb, LUMINANCE_VECTOR);

  PS_OUT o;
  o.color0 = r1 * 0.0625f;

  float keep = 1.f - sum.a * 0.0625f;
  float4 r0;
  r0.rgb = keep * sum.rgb;
  r0.a = sum.a;
  o.color1 = r0 * 0.0625f;
  return o;
}
