struct VertexInput {
  float3 position : POSITION;
  float2 texcoord : TEXCOORD0;
};

struct VertexOutput {
  float4 position : SV_POSITION;
  float2 texcoord : TEXCOORD0;
};

cbuffer TransformConstants : register(b0) {
  row_major float4x4 world_view_projection;
};

Texture2D<float4> cube_texture : register(t0);
SamplerState cube_sampler : register(s0);

VertexOutput VSMain(VertexInput input) {
  VertexOutput output;
  output.position = mul(float4(input.position, 1.0f), world_view_projection);
  output.texcoord = input.texcoord;
  return output;
}

float4 PSMain(VertexOutput input) : SV_TARGET {
  return cube_texture.Sample(cube_sampler, input.texcoord);
}
