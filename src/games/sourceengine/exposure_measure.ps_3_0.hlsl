sampler2D SceneSampler : register(s0);
float4 Bin : register(c0);

float4 main(float2 uv : TEXCOORD0) : COLOR0 {
  const float luminance = dot(tex2D(SceneSampler, uv).rgb, float3(0.2125f, 0.7154f, 0.0721f)) * Bin.z;
  clip(luminance - Bin.x);
  clip(Bin.y - luminance);
  return 0.f;
}
