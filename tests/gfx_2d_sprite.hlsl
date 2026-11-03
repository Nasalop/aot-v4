struct SpriteInput {
  float3 position : POSITION;
  float2 texcoord : TEXCOORD0;
  float4 color : COLOR0;
};

struct SpriteOutput {
  float4 position : SV_POSITION;
  float2 texcoord : TEXCOORD0;
  float4 color : COLOR0;
};

cbuffer SpriteConstants : register(b0) {
  float4 motion;
};

Texture2D<float4> sprite_texture : register(t0);
SamplerState sprite_sampler : register(s0);

SpriteOutput VSMain(SpriteInput input) {
  SpriteOutput output;
  output.position = float4(input.position.xy + motion.xy, input.position.z, 1.0f);
  output.texcoord = input.texcoord;
  output.color = input.color;
  return output;
}

float4 PSMain(SpriteOutput input) : SV_TARGET {
  return sprite_texture.Sample(sprite_sampler, input.texcoord) * input.color;
}
