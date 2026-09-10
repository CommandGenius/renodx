// Need for Speed: Most Wanted (2005) 13-tap weighted blur (bloom).
// Translated from 0x2E70CDF8.ps_2_0.cso (40 instruction slots).
//
// Weighted sum of thirteen offset taps. The bytecode issues the taps in a
// slightly shuffled order (offset c1 first, then c0, c2, ...) but every tap
// pairs with its own weight, so the sum is the same in any order. No clamps,
// so it is translated byte-faithfully; values above 1.0 pass straight through.
// The arrays are declared at the thirteen entries actually used so the weight
// array keeps the c13 base the game's constant table assigns it.
//
// shared.h is not included: ps_2_0 cannot compile its constant buffer block
// and this pass needs none of the injection values.

float4 g_avSampleOffsets[13] : register(c0);
float4 g_avSampleWeights[13] : register(c13);

sampler2D DIFFUSEMAP_SAMPLER : register(s0);

float4 main(float2 uv : TEXCOORD0) : COLOR0 {
  float4 sum = 0.f;
  [unroll]
  for (int i = 0; i < 13; ++i) {
    sum += g_avSampleWeights[i] * tex2D(DIFFUSEMAP_SAMPLER, uv + g_avSampleOffsets[i].xy);
  }
  return sum;  // mov oC0, r0
}
