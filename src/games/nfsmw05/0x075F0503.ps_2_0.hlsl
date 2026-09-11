float4 g_avSampleOffsets[9] : register(c0);

sampler2D DIFFUSEMAP_SAMPLER : register(s0);

float4 main(float2 uv : TEXCOORD0) : COLOR0 {
  float sum = 0.f;
  [unroll]
  for (int i = 0; i < 9; ++i) {
    sum += saturate(tex2D(DIFFUSEMAP_SAMPLER, uv + g_avSampleOffsets[i].xy).a);
  }

  float4 o;
  o.rgb = sum * 0.111111112f;
  o.a = 1.f;
  return o;
}
