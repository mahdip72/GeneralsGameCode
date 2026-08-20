cbuffer LegacyTransformConstants : register(b0)
{
	row_major float4x4 WorldViewProjection;
};

struct VertexInput
{
	float3 position : POSITION;
	float4 color : COLOR0;
};

struct VertexOutput
{
	float4 position : SV_POSITION;
	float4 color : COLOR0;
};

VertexOutput VSMain(VertexInput input)
{
	VertexOutput output;
	output.position = mul(float4(input.position, 1.0f), WorldViewProjection);
	output.color = input.color;
	return output;
}

float4 PSMain(VertexOutput input) : SV_TARGET
{
	return saturate(input.color);
}

struct TexturedVertexInput
{
	float3 position : POSITION;
	float3 normal : NORMAL;
	float4 color : COLOR0;
	float2 textureCoordinate : TEXCOORD0;
};

struct TexturedVertexOutput
{
	float4 position : SV_POSITION;
	float4 color : COLOR0;
	float2 textureCoordinate : TEXCOORD0;
};

TexturedVertexOutput VSTextured(TexturedVertexInput input)
{
	TexturedVertexOutput output;
	output.position = mul(float4(input.position, 1.0f), WorldViewProjection);
	output.color = input.color;
	output.textureCoordinate = input.textureCoordinate;
	return output;
}

Texture2D LegacyTexture0 : register(t0);
SamplerState LegacySampler0 : register(s0);

float4 PSTextured(TexturedVertexOutput input) : SV_TARGET
{
	return saturate(input.color *
		LegacyTexture0.Sample(LegacySampler0, input.textureCoordinate));
}
