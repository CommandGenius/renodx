// Need for Speed: Most Wanted (2005) bloom bright pass.
// Translated from 0xE2825F6E.ps_2_0.cso (8 instruction slots).
//
// Scales the scene by middle grey over adapted luminance, subtracts the
// threshold, and floors at zero. There is no saturate, so anything above 1.0
// already survives as long as the target format can hold it. Nothing to
// unclip; kept byte-faithful so the pass behaves identically on an upgraded
// float target.
//
// shared.h is not included: ps_2_0 cannot compile its constant buffer block
// and this pass needs none of the injection values.

float g_fMiddleGray           : register(c0);
float g_fAdaptedLum           : register(c1);
float g_fBrightPassThreshold  : register(c2);

sampler2D DIFFUSEMAP_SAMPLER : register(s0);

float4 main(float2 uv : TEXCOORD0) : COLOR0 {
  float4 r0 = tex2D(DIFFUSEMAP_SAMPLER, uv);                 // texld r0, t0, s0

  // RenoDX: bound the tap on read. The float scene target can hold inf and
  // NaN from overflowed spark blending; min() passes NaN through on some GPUs,
  // while saturate() lowers to the NaN-suppressing clamp and matches what the
  // original 8-bit source delivered here anyway.
  r0.rgb = saturate(r0.rgb);

  float scale = g_fMiddleGray / (g_fAdaptedLum + 0.001f);    // add / rcp / mul
  r0.rgb = max(r0.rgb * scale - g_fBrightPassThreshold, 0.f); // mad / max

  return r0;                                                 // mov oC0, r0
}
