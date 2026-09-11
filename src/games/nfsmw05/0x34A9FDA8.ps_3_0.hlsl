#include "./shared.h"

// Need for Speed: Most Wanted (2005) visual treatment pass.
// Translated instruction-for-instruction from 0x34A9FDA8.ps_3_0.cso
// (Microsoft HLSL Shader Compiler 9.29.952.3111, 42 instruction slots).

float3 LUMINANCE_VECTOR     : register( c0  );
float  VisualEffectVignette : register( c1  );
float  Desaturation         : register( c2  );
float  g_fVignetteScale     : register( c3  );
float  g_fBloomScale        : register( c4  );
float4 BlurParams           : register( c5  );
float4 Coeffs0              : register( c6  );
float4 Coeffs1              : register( c7  );
float4 Coeffs2              : register( c8  );
float4 Coeffs3              : register( c9  );
float  CombinedBrightness   : register( c10 );

sampler2D DIFFUSEMAP_SAMPLER : register( s0 );
sampler2D MISCMAP1_SAMPLER   : register( s1 );
sampler2D MISCMAP2_SAMPLER   : register( s2 );
sampler2D MISCMAP3_SAMPLER   : register( s3 );
sampler2D HEIGHTMAP_SAMPLER  : register( s4 );

struct PS_IN
{
	float4 texcoord  : TEXCOORD0;
	float4 texcoord1 : TEXCOORD1;
	float4 texcoord2 : TEXCOORD2;
	float4 texcoord3 : TEXCOORD3;
};

// RenoDX: the scene targets are float now. Additive spark sprites in a crash
// overflow half precision to infinity, and the game's own blend modes then
// turn that into NaN in the target itself (a black sprite texel screen-blended
// over an infinite pixel is 0 * inf). Those NaNs render as black holes inside
// the brightest glow. min() does not help: the SPIR-V it lowers to is
// undefined for NaN and passes it through on some GPUs. saturate() lowers to
// the NaN-suppressing clamp, flushes NaN to 0, and matches exactly what the
// original 8-bit targets did to every tap anyway. HDR headroom still comes
// from the visual treatment's own brightness and bloom math below.
float4 SampleScene(sampler2D s, float2 uv) {
	float4 t = tex2D(s, uv);
	t.rgb = saturate(t.rgb);
	return t;
}

float4 main(PS_IN i) : COLOR0
{
	float4 o;

	float4 r0;
	float4 r1;
	float4 r2;
	float3 r3;
	float4 r4;

	r0     = SampleScene(DIFFUSEMAP_SAMPLER,i.texcoord1.xy);              // texld r0, v1, s0
	r1     = SampleScene(DIFFUSEMAP_SAMPLER,i.texcoord.zw);               // texld r1, v0.zwzw, s0
	r1.xyz = r1.xyz * 0.1875;                                        // mul r1.xyz, r1, c11.y
	r2     = SampleScene(DIFFUSEMAP_SAMPLER,i.texcoord.xy);               // texld r2, v0, s0
	r1.xyz = r2.xyz * 0.1875 + r1.xyz;                               // mad r1.xyz, r2, c11.y, r1
	r0.xyz = r0.xyz * 0.125 + r1.xyz;                                // mad r0.xyz, r0, c11.z, r1
	r1     = SampleScene(DIFFUSEMAP_SAMPLER,i.texcoord1.zw);              // texld r1, v1.zwzw, s0
	r0.xyz = r1.xyz * 0.125 + r0.xyz;                                // mad r0.xyz, r1, c11.z, r0
	r1     = SampleScene(DIFFUSEMAP_SAMPLER,i.texcoord2.xy);              // texld r1, v2, s0
	r0.xyz = r1.xyz * 0.125 + r0.xyz;                                // mad r0.xyz, r1, c11.z, r0
	r1     = SampleScene(DIFFUSEMAP_SAMPLER,i.texcoord2.zw);              // texld r1, v2.zwzw, s0
	r0.xyz = r1.xyz * 0.09375 + r0.xyz;                              // mad r0.xyz, r1, c11.w, r0
	r1     = SampleScene(DIFFUSEMAP_SAMPLER,i.texcoord3.xy);              // texld r1, v3, s0
	r0.xyz = r1.xyz * 0.09375 + r0.xyz;                              // mad r0.xyz, r1, c11.w, r0
	r1     = SampleScene(DIFFUSEMAP_SAMPLER,i.texcoord3.zw);              // texld r1, v3.zwzw, s0
	r0.xyz = r1.xyz * 0.0625 + r0.xyz;                               // mad r0.xyz, r1, c12.x, r0
	r1     = tex2D(HEIGHTMAP_SAMPLER, i.texcoord.xy);                // texld r1, v0, s4
	r0.w   = 1.0 - r1.x;                                             // add r0.w, -r1.x, c11.x
	r0.w   = 1.0 / r0.w;                                             // rcp r0.w, r0.w
	r0.w   = saturate(r0.w * -0.00333333341 + 1.20000005);           // mad_sat r0.w, r0.w, c12.y, c12.z
	r1.y   = g_fVignetteScale * i.texcoord.y;                        // mul r1.y, c3.x, v0.y
	r1.x   = i.texcoord.x;                                           // mov r1.x, v0.x
	r1     = tex2D(MISCMAP2_SAMPLER, r1.xy);                         // texld r1, r1, s2
	r1.x   = r1.x + BlurParams.x;                                    // add r1.x, r1.x, c5.x
	r1.w   = r1.w * BlurParams.y;                                    // mul r1.w, r1.w, c5.y
	r0.w   = saturate(r1.x * r0.w + r1.w);                           // mad_sat r0.w, r1.x, r0.w, r1.w
	r3.xyz = lerp(r2.xyz, r0.xyz, r0.w);                             // lrp r3.xyz, r0.w, r0, r2
	o.w    = r2.w;                                                   // mov oC0.w, r2.w
	r0.x   = dot(r3.xyz, LUMINANCE_VECTOR);                          // dp3 r0.x, r3, c0
	r2.xyz = lerp(r0.xxx, r3.xyz, Desaturation);                     // lrp r2.xyz, c2.x, r3, r0.x
	r0     = tex2D(MISCMAP1_SAMPLER, i.texcoord.xy);                 // texld r0, v0, s1
	r0.w   = saturate(r0.w);                                         // adaptation value: bound like a scene tap
	r4     = Coeffs3;                                                // mov r4, c9
	r4     = r4 * r0.w + Coeffs2;                                    // mad r4, r4, r0.w, c8
	r4     = r4 * r0.w + Coeffs1;                                    // mad r4, r4, r0.w, c7
	r0     = r4 * r0.w + Coeffs0;                                    // mad r0, r4, r0.w, c6
	r0.yzw = r3.xyz * r0.yzw;                                        // mul r0.yzw, r3.xxyz, r0
	r0.xyz = r2.xyz * r0.x + r0.yzw;                                 // mad r0.xyz, r2, r0.x, r0.yzww
	r0.xyz = r1.y * VisualEffectVignette + r0.xyz;                   // mad r0.xyz, r1.y, c1.x, r0
	r0.w   = r1.z * CombinedBrightness;                              // mul r0.w, r1.z, c10.x
	r1     = SampleScene(MISCMAP3_SAMPLER, i.texcoord.xy);           // texld r1, v0, s3
	r1.xyz = r1.xyz * g_fBloomScale;                                 // mul r1.xyz, r1, c4.x
	o.xyz  = r0.xyz * r0.w + r1.xyz;                                 // mad oC0.xyz, r0, r0.w, r1

	// RenoDX: the original wrote o.xyz straight to an 8-bit target, clipping
	// anything above 1.0. Keep the unclipped value as HDR headroom, decode the
	// game's gamma space to linear, tone map, and write the intermediate
	// encoding the swap chain proxy pass expects.
	float3 untonemapped = renodx::color::gamma::Decode(max(0.f, o.xyz), 2.2f);
    if (RENODX_TONE_MAP_TYPE != 0)
	{
      float3 tonemapped = renodx::draw::ToneMapPass(untonemapped);
      o.xyz = renodx::draw::RenderIntermediatePass(tonemapped);
    }
    else
	{
      float3 tonemapped = saturate(untonemapped);
      o.xyz = renodx::draw::RenderIntermediatePass(tonemapped);
	}

	// fxc places its own literal constants in any register it thinks is free,
	// including unread members of the injection array. RenoDX writes the
	// injection block into c50-c58 at draw time, which then corrupts those
	// literals (this build had the 2.2 / 2.4 gamma exponents in c57). Touching
	// one component of every injection register keeps the whole range reserved.
	// The guard is folded into alpha in a way that leaves it unchanged for any
	// finite values and cannot be constant-folded away.
	float guard = 0.f;
	[unroll]
	for (int k = 0; k < 9; ++k) {
		guard += shader_injection[k].x;
	}
	o.w = saturate(o.w + guard * 1e-30f);

	return o;
}
