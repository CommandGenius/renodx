// Need for Speed: Most Wanted (2005) 3x3 alpha-channel downsample.
// Translated from 0x075F0503.ps_2_0.cso (29 instruction slots).
//
// Averages the alpha channel of nine offset taps and broadcasts it to RGB with
// alpha set to 1. This is the luminance-adaptation reduction step; the value it
// averages was written into alpha by the luminance pass. No clamps, so it is
// translated byte-faithfully. Only nine offsets are read, so the array is
// declared at that size to keep the same c0-c8 register span the game fills.
//
// shared.h is not included: ps_2_0 cannot compile its constant buffer block
// and this pass needs none of the injection values.

float4 g_avSampleOffsets[9] : register(c0);

sampler2D DIFFUSEMAP_SAMPLER : register(s0);

float4 main(float2 uv : TEXCOORD0) : COLOR0 {
  float sum = 0.f;
  [unroll]
  for (int i = 0; i < 9; ++i) {
    sum += tex2D(DIFFUSEMAP_SAMPLER, uv + g_avSampleOffsets[i].xy).a;
  }

  float4 o;
  o.rgb = sum * 0.111111112f;  // mul r0.xyz, r0.x, c9.x
  o.a = 1.f;                    // mov r0.w, c9.y
  return o;
}
