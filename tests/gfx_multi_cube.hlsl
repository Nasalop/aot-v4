struct VertexInput {
  float3 position : POSITION;
  float2 texcoord : TEXCOORD0;
};

struct VertexOutput {
  float4 position : SV_POSITION;
  float2 texcoord : TEXCOORD0;
  float texture_index : TEXCOORD1;
  float4 tint : COLOR0;
};

cbuffer WorldConstants : register(b0) {
  row_major float4x4 world;
};

cbuffer ViewConstants : register(b1) {
  row_major float4x4 view;
};

cbuffer ProjectionConstants : register(b2) {
  row_major float4x4 projection;
};

cbuffer MaterialConstants : register(b3) {
  float4 tint;
  float4 texture_choice;
};

Texture2D<float4> texture0 : register(t0);
Texture2D<float4> texture1 : register(t1);
Texture2D<float4> texture2 : register(t2);
Texture2D<float4> texture3 : register(t3);
SamplerState sampler0 : register(s0);
SamplerState sampler1 : register(s1);
SamplerState sampler2 : register(s2);
SamplerState sampler3 : register(s3);

VertexOutput VSMain(VertexInput input) {
  VertexOutput output;
  float4 position = mul(float4(input.position, 1.0f), world);
  position = mul(position, view);
  output.position = mul(position, projection);
  output.texcoord = input.texcoord;
  output.texture_index = texture_choice.x;
  output.tint = tint;
  return output;
}

float4 PSMain(VertexOutput input) : SV_TARGET {
  if (input.texture_index < 0.5f)
    return texture0.Sample(sampler0, input.texcoord) * input.tint;
  if (input.texture_index < 1.5f)
    return texture1.Sample(sampler1, input.texcoord) * input.tint;
  if (input.texture_index < 2.5f)
    return texture2.Sample(sampler2, input.texcoord) * input.tint;
  return texture3.Sample(sampler3, input.texcoord) * input.tint;
}
