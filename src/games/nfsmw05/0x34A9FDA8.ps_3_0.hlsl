#include "./shared.h"

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

	r0     = SampleScene(DIFFUSEMAP_SAMPLER,i.texcoord1.xy);
	r1     = SampleScene(DIFFUSEMAP_SAMPLER,i.texcoord.zw);
	r1.xyz = r1.xyz * 0.1875;
	r2     = SampleScene(DIFFUSEMAP_SAMPLER,i.texcoord.xy);
	r1.xyz = r2.xyz * 0.1875 + r1.xyz;
	r0.xyz = r0.xyz * 0.125 + r1.xyz;
	r1     = SampleScene(DIFFUSEMAP_SAMPLER,i.texcoord1.zw);
	r0.xyz = r1.xyz * 0.125 + r0.xyz;
	r1     = SampleScene(DIFFUSEMAP_SAMPLER,i.texcoord2.xy);
	r0.xyz = r1.xyz * 0.125 + r0.xyz;
	r1     = SampleScene(DIFFUSEMAP_SAMPLER,i.texcoord2.zw);
	r0.xyz = r1.xyz * 0.09375 + r0.xyz;
	r1     = SampleScene(DIFFUSEMAP_SAMPLER,i.texcoord3.xy);
	r0.xyz = r1.xyz * 0.09375 + r0.xyz;
	r1     = SampleScene(DIFFUSEMAP_SAMPLER,i.texcoord3.zw);
	r0.xyz = r1.xyz * 0.0625 + r0.xyz;
	r1     = tex2D(HEIGHTMAP_SAMPLER, i.texcoord.xy);
	r0.w   = 1.0 - r1.x;
	r0.w   = 1.0 / r0.w;
	r0.w   = saturate(r0.w * -0.00333333341 + 1.20000005);
	r1.y   = g_fVignetteScale * i.texcoord.y;
	r1.x   = i.texcoord.x;
	r1     = tex2D(MISCMAP2_SAMPLER, r1.xy);
	r1.x   = r1.x + BlurParams.x;
	r1.w   = r1.w * BlurParams.y;
	r0.w   = saturate(r1.x * r0.w + r1.w);
	r3.xyz = lerp(r2.xyz, r0.xyz, r0.w);
	o.w    = r2.w;
	r0.x   = dot(r3.xyz, LUMINANCE_VECTOR);
	r2.xyz = lerp(r0.xxx, r3.xyz, Desaturation);
	r0     = tex2D(MISCMAP1_SAMPLER, i.texcoord.xy);
	r0.w   = saturate(r0.w);
	r4     = Coeffs3;
	r4     = r4 * r0.w + Coeffs2;
	r4     = r4 * r0.w + Coeffs1;
	r0     = r4 * r0.w + Coeffs0;
	r0.yzw = r3.xyz * r0.yzw;
	r0.xyz = r2.xyz * r0.x + r0.yzw;
	r0.xyz = r1.y * VisualEffectVignette + r0.xyz;
	r0.w   = r1.z * CombinedBrightness;
	r1     = SampleScene(MISCMAP3_SAMPLER, i.texcoord.xy);
	r1.xyz = r1.xyz * g_fBloomScale;
	o.xyz  = r0.xyz * r0.w + r1.xyz;

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

	float guard = 0.f;
	[unroll]
	for (int k = 0; k < 9; ++k) {
		guard += shader_injection[k].x;
	}
	o.w = saturate(o.w + guard * 1e-30f);

	return o;
}