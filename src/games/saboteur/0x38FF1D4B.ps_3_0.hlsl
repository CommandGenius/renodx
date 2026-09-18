#include "./common.hlsl"

float4 g_vFadeValues         : register(c3);
float4 g_vMisc               : register(c4);
float4 g_vGodRays            : register(c5);
float4 g_vAdaptiveParams     : register(c6);
float4 g_vHazeBloom          : register(c7);
float4 g_vSkyBloomColor      : register(c8);
float4 g_vAntialiasingFilter : register(c9);
float4 g_vGodRays2           : register(c10);

sampler2D BackBufferSampler            : register(s0);
sampler2D DepthStencilSampler          : register(s1);
sampler2D DownsampledBackBufferSampler : register(s2);
sampler2D SkyBloomTextureSampler       : register(s4);
sampler2D BloomTextureSampler          : register(s5);

float3 SampleHdr(sampler2D s, float2 uv) {
  return BoundHdr(tex2D(s, uv).rgb);
}

float4 main(float2 uv : TEXCOORD0) : COLOR0 {
  float4 o;

  float4 scene = tex2D(BackBufferSampler, uv);
  float3 vFinalColor = SampleHdr(BackBufferSampler, uv);
  o.w = dot(saturate(scene.rgb), float3(0.300000012f, 0.600000024f, 0.200000003f));

  float fProjectedDepth = tex2D(DepthStencilSampler, uv).x;
  float fOpacity = saturate(g_vFadeValues.x * fProjectedDepth + g_vFadeValues.y);
  float fNearOpacity = saturate(g_vFadeValues.z * fProjectedDepth + g_vFadeValues.w);
  float fBlur = saturate(saturate(fOpacity + fNearOpacity) * g_vMisc.x);

  float3 vBlurColor = SampleHdr(DownsampledBackBufferSampler, uv);
  vFinalColor = lerp(vFinalColor, vBlurColor, fBlur);

  float3 vBloom = SampleHdr(BloomTextureSampler, uv);
  vFinalColor = vFinalColor * 4.f + vBloom * g_vAdaptiveParams.y;

  float fLinearDepth = g_vAdaptiveParams.w * (1.f / fProjectedDepth) + g_vAdaptiveParams.z;
  float fHazeBloomGradient = 1.f - saturate(fLinearDepth * (1.f / g_vAdaptiveParams.x));
  float fFinalHazeBloomGradient = g_vAntialiasingFilter.x * (1.f - fHazeBloomGradient) + fHazeBloomGradient;

  float4 vLowResSkyBloom = tex2D(SkyBloomTextureSampler, uv);
  vLowResSkyBloom = saturate(vLowResSkyBloom);
  float fHazeBloom = max(vLowResSkyBloom.y - g_vHazeBloom.w, 0.f);
  vFinalColor = (fHazeBloom * g_vHazeBloom.xyz) * fFinalHazeBloomGradient + vFinalColor;

  float fGodRayBlend = saturate(vLowResSkyBloom.w * g_vGodRays2.w);
  float3 vGodRayColor = fGodRayBlend * (g_vGodRays.xyz - g_vGodRays2.xyz) + g_vGodRays2.xyz;
  vFinalColor = vGodRayColor * vLowResSkyBloom.w + vFinalColor;

  float fSkyBloomIntensity = saturate(vLowResSkyBloom.x * g_vSkyBloomColor.w * g_vGodRays.w) * g_vGodRays.w;
  vFinalColor = lerp(vFinalColor, g_vSkyBloomColor.xyz, fSkyBloomIntensity);

  o.xyz = ToneMapScene(vFinalColor);

  o.w = saturate(o.w + InjectionGuard());

  return o;
}
