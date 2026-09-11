float4 g_avSampleOffsets[16] : register(c0);
float4 g_avSampleWeights[16] : register(c16);

sampler2D DIFFUSEMAP_SAMPLER : register(s0);

float4 main(float2 uv : TEXCOORD0) : COLOR0 {
  float4 sum = 0.f;
  [unroll]
  for (int i = 0; i < 16; ++i) {
    float4 t = tex2D(DIFFUSEMAP_SAMPLER, uv + g_avSampleOffsets[i].xy);
    t.rgb = saturate(t.rgb);
    sum += g_avSampleWeights[i] * t;
  }
  return sum;
}
