float4 g_avSampleOffsets[13] : register(c0);
float4 g_avSampleWeights[13] : register(c13);

sampler2D DIFFUSEMAP_SAMPLER : register(s0);

float4 main(float2 uv : TEXCOORD0) : COLOR0 {
  float4 sum = 0.f;
  [unroll]
  for (int i = 0; i < 13; ++i) {
    float4 t = tex2D(DIFFUSEMAP_SAMPLER, uv + g_avSampleOffsets[i].xy);
    t.rgb = saturate(t.rgb);
    sum += g_avSampleWeights[i] * t;
  }
  return sum;
}
