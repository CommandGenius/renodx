sampler2D SceneSampler : register(s0);
float4 SourceRect : register(c0);

float4 main(float2 uv : TEXCOORD0) : COLOR0 {
  return tex2D(SceneSampler, SourceRect.xy + uv * SourceRect.zw);
}
