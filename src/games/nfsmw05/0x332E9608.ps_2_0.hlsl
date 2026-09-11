// Need for Speed: Most Wanted (2005) 4x4 downsample with luminance measure.
// Translated from 0x332E9608.ps_2_0.cso (55 instruction slots).
//
// Sums sixteen offset taps, then writes two render targets:
//
//   COLOR0 = (average RGB, luminance of the average)
//   COLOR1 = (average RGB scaled by 1 - average alpha, average alpha)
//
// The luminance lands in alpha so the alpha-only 3x3 downsample can reduce it
// further for eye adaptation. No clamps anywhere, so the translation is
// byte-faithful and values above 1.0 pass straight through.
//
// shared.h is not included: ps_2_0 cannot compile its constant buffer block
// and this pass needs none of the injection values.

float4 g_avSampleOffsets[16] : register(c0);
float3 LUMINANCE_VECTOR      : register(c16);

sampler2D DIFFUSEMAP_SAMPLER : register(s0);

struct PS_OUT {
  float4 color0 : COLOR0;
  float4 color1 : COLOR1;
};

PS_OUT main(float2 uv : TEXCOORD0) {
  // RenoDX: the scene target is float now and can hold inf and NaN from
  // overflowed spark blending. A NaN tap here poisons the luminance measure,
  // and with it eye adaptation, permanently. min() passes NaN through on some
  // GPUs; saturate() lowers to the NaN-suppressing clamp and matches what the
  // original 8-bit source delivered here anyway. One saturate per tap fits the
  // 64 arithmetic slots ps_2_0 allows.
  float4 sum = 0.f;
  [unroll]
  for (int i = 0; i < 16; ++i) {
    sum += saturate(tex2D(DIFFUSEMAP_SAMPLER, uv + g_avSampleOffsets[i].xy));
  }

  float4 r1;
  r1.rgb = sum.rgb;                              // mov r1.xyz, r0
  r1.a = dot(sum.rgb, LUMINANCE_VECTOR);         // dp3 r1.w, r0, c16

  PS_OUT o;
  o.color0 = r1 * 0.0625f;                       // mul r2, r1, c17.x

  float keep = 1.f - sum.a * 0.0625f;            // mad r1.w, r0.w, -c17.x, c17.y
  float4 r0;
  r0.rgb = keep * sum.rgb;                       // mul r0.xyz, r1.w, r1
  r0.a = sum.a;
  o.color1 = r0 * 0.0625f;                       // mul r0, r0, c17.x
  return o;
}
