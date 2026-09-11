#include "./shared.h"

sampler2D InputTex : register(s0);

static const float LUT[256] = {
    0.0000000f, 0.0039100f, 0.0068400f, 0.0097800f, 0.0117300f, 0.0146600f, 0.0166200f, 0.0195500f,
    0.0224800f, 0.0254200f, 0.0283500f, 0.0312800f, 0.0351900f, 0.0381200f, 0.0420300f, 0.0459400f,
    0.0488800f, 0.0537600f, 0.0576700f, 0.0615800f, 0.0664700f, 0.0703800f, 0.0752700f, 0.0801600f,
    0.0860200f, 0.0909100f, 0.0958000f, 0.1006800f, 0.1055700f, 0.1104600f, 0.1143700f, 0.1192600f,
    0.1241400f, 0.1290300f, 0.1339200f, 0.1378300f, 0.1427200f, 0.1476100f, 0.1524900f, 0.1564000f,
    0.1612900f, 0.1661800f, 0.1700900f, 0.1749800f, 0.1788900f, 0.1837700f, 0.1876800f, 0.1925700f,
    0.1964800f, 0.2023500f, 0.2062600f, 0.2111400f, 0.2150500f, 0.2189600f, 0.2238500f, 0.2277600f,
    0.2316700f, 0.2365600f, 0.2414500f, 0.2453600f, 0.2502400f, 0.2541500f, 0.2580600f, 0.2619700f,
    0.2668600f, 0.2717500f, 0.2756600f, 0.2795700f, 0.2844600f, 0.2883700f, 0.2922800f, 0.2971700f,
    0.3010800f, 0.3049900f, 0.3089000f, 0.3137800f, 0.3186700f, 0.3225800f, 0.3264900f, 0.3304000f,
    0.3343100f, 0.3392000f, 0.3431100f, 0.3470200f, 0.3509300f, 0.3548400f, 0.3597300f, 0.3636400f,
    0.3675500f, 0.3714600f, 0.3763400f, 0.3802500f, 0.3841600f, 0.3880700f, 0.3919800f, 0.3968700f,
    0.4007800f, 0.4046900f, 0.4076200f, 0.4125100f, 0.4164200f, 0.4203300f, 0.4242400f, 0.4291300f,
    0.4330400f, 0.4369500f, 0.4398800f, 0.4447700f, 0.4486800f, 0.4525900f, 0.4565000f, 0.4613900f,
    0.4643200f, 0.4682300f, 0.4731200f, 0.4770300f, 0.4809400f, 0.4838700f, 0.4887600f, 0.4926700f,
    0.4965800f, 0.4995100f, 0.5044000f, 0.5083100f, 0.5122200f, 0.5161300f, 0.5200400f, 0.5239500f,
    0.5268800f, 0.5317700f, 0.5356800f, 0.5395900f, 0.5435000f, 0.5474100f, 0.5513200f, 0.5552300f,
    0.5591400f, 0.5630500f, 0.5669600f, 0.5708700f, 0.5747800f, 0.5777100f, 0.5826000f, 0.5855300f,
    0.5894400f, 0.5943300f, 0.5972600f, 0.6011700f, 0.6060600f, 0.6089900f, 0.6129000f, 0.6168100f,
    0.6207200f, 0.6246300f, 0.6285400f, 0.6324500f, 0.6353900f, 0.6402700f, 0.6432100f, 0.6471200f,
    0.6520000f, 0.6549400f, 0.6588500f, 0.6627600f, 0.6666700f, 0.6696000f, 0.6744900f, 0.6774200f,
    0.6813300f, 0.6852400f, 0.6891500f, 0.6920800f, 0.6969700f, 0.6999000f, 0.7038100f, 0.7077200f,
    0.7116300f, 0.7145600f, 0.7194500f, 0.7223900f, 0.7263000f, 0.7302100f, 0.7341200f, 0.7370500f,
    0.7419400f, 0.7448700f, 0.7497600f, 0.7526900f, 0.7556200f, 0.7605100f, 0.7634400f, 0.7673500f,
    0.7712600f, 0.7751700f, 0.7781000f, 0.7829900f, 0.7859200f, 0.7898300f, 0.7937400f, 0.7966800f,
    0.8015600f, 0.8045000f, 0.8074300f, 0.8123200f, 0.8152500f, 0.8201400f, 0.8230700f, 0.8260000f,
    0.8308900f, 0.8338200f, 0.8367500f, 0.8416400f, 0.8445700f, 0.8494600f, 0.8523900f, 0.8553300f,
    0.8602200f, 0.8631500f, 0.8660800f, 0.8709700f, 0.8739000f, 0.8778100f, 0.8817200f, 0.8846500f,
    0.8885600f, 0.8924700f, 0.8963800f, 0.8993200f, 0.9032300f, 0.9071400f, 0.9100700f, 0.9149600f,
    0.9178900f, 0.9208200f, 0.9257100f, 0.9286400f, 0.9315700f, 0.9364600f, 0.9393900f, 0.9433000f,
    0.9462400f, 0.9501500f, 0.9540600f, 0.9569900f, 0.9618800f, 0.9648100f, 0.9677400f, 0.9716500f,
    0.9755600f, 0.9794700f, 0.9824000f, 0.9853400f, 0.9902200f, 0.9931600f, 0.9970700f, 1.0000000f
};

float3 SampleCurve(float3 c) {
  float3 s = c * 255.f;

  float3 fl = floor(s + 1e-4f);
  float3 f = saturate(s - fl);
  int3 i = (int3)fl;
  int3 j = min(i + 1, 255);

  float3 lo = float3(LUT[i.x], LUT[i.y], LUT[i.z]);
  float3 hi = float3(LUT[j.x], LUT[j.y], LUT[j.z]);
  return lerp(lo, hi, f);
}

float4 main(float2 uv : TEXCOORD0) : COLOR0 {
  float4 c = tex2D(InputTex, uv);

  const float encoding = RENODX_INTERMEDIATE_ENCODING;
  const float correction = RENODX_GAMMA_CORRECTION;
  const float scaling = RENODX_DIFFUSE_WHITE_NITS / RENODX_GRAPHICS_WHITE_NITS;

  float3 encoded = c.rgb;
  encoded = (encoded > -1e-4f) ? encoded : 0.f;
  encoded = (encoded < 64.f) ? encoded : 63.99f;
  float3 decoded =
      (encoding == 1.f) ? renodx::color::srgb::Decode(encoded)
      : (encoding == 2.f) ? renodx::color::gamma::Decode(encoded, 2.2f)
      : (encoding == 3.f) ? renodx::color::gamma::Decode(encoded, 2.4f)
      : encoded;
  decoded /= scaling;
  float3 linear_color =
      (correction == 1.f) ? renodx::color::correct::GammaSafe(decoded, true, 2.2f)
      : (correction == 2.f) ? renodx::color::correct::GammaSafe(decoded, true, 2.4f)
      : decoded;
  float3 game_gamma = renodx::color::gamma::Encode(max(linear_color, 0.f), 2.2f);

  float3 sdr = saturate(game_gamma);
  float3 excess = game_gamma - sdr;
  float3 curved = SampleCurve(sdr) + excess;

  float3 out_linear = renodx::color::gamma::Decode(curved, 2.2f);
  out_linear =
      (correction == 1.f) ? renodx::color::correct::GammaSafe(out_linear, false, 2.2f)
      : (correction == 2.f) ? renodx::color::correct::GammaSafe(out_linear, false, 2.4f)
      : out_linear;
  out_linear *= scaling;
  float3 out_encoded =
      (encoding == 1.f) ? renodx::color::srgb::Encode(out_linear)
      : (encoding == 2.f) ? renodx::color::gamma::Encode(out_linear, 2.2f)
      : (encoding == 3.f) ? renodx::color::gamma::Encode(out_linear, 2.4f)
      : out_linear;

  float guard = 0.f;
  [unroll]
  for (int k = 0; k < 9; ++k) {
    guard += shader_injection[k].x;
  }

  float4 o;
  o.rgb = out_encoded;
  o.a = saturate(1.f + guard * 1e-30f);
  return o;
}