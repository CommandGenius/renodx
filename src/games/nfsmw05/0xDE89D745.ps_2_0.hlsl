// Need for Speed: Most Wanted (2005) 16-tap weighted blur (bloom).
// Translated from 0xDE89D745.ps_2_0.cso (49 instruction slots).
//
// Weighted sum of sixteen offset taps. As with the 13-tap variant the bytecode
// shuffles the first two taps, but each tap pairs with its own weight, so the
// order does not change the result. No clamps, so it is translated
// byte-faithfully; values above 1.0 pass straight through.
//
// shared.h is not included: ps_2_0 cannot compile its constant buffer block
// and this pass needs none of the injection values.

float4 g_avSampleOffsets[16] : register(c0);
float4 g_avSampleWeights[16] : register(c16);

sampler2D DIFFUSEMAP_SAMPLER : register(s0);

float4 main(float2 uv : TEXCOORD0) : COLOR0 {
  float4 sum = 0.f;
  [unroll]
  for (int i = 0; i < 16; ++i) {
    sum += g_avSampleWeights[i] * tex2D(DIFFUSEMAP_SAMPLER, uv + g_avSampleOffsets[i].xy);
  }
  return sum;  // mov oC0, r0
}
