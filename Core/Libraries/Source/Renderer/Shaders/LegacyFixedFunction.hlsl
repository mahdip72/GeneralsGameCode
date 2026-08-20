cbuffer LegacyTransformConstants : register(b0)
{
	row_major float4x4 WorldViewProjection;
	row_major float4x4 WorldView;
	float4 FogColor;
	float4 FogParameters;
	uint4 AlphaTestParameters;
};

bool PassesAlphaTest(float alpha)
{
	if (AlphaTestParameters.x == 0)
	{
		return true;
	}
	const uint value = (uint)round(saturate(alpha) * 255.0f);
	const uint reference = AlphaTestParameters.z;
	switch (AlphaTestParameters.y)
	{
	case 0: return false;
	case 1: return value < reference;
	case 2: return value == reference;
	case 3: return value <= reference;
	case 4: return value > reference;
	case 5: return value != reference;
	case 6: return value >= reference;
	default: return true;
	}
}

float CalculateFogFactor(float depth)
{
	const uint mode = (uint)FogParameters.w;
	if (mode == 0)
	{
		return 1.0f;
	}
	if (mode == 2)
	{
		return saturate(exp(-FogParameters.z * depth));
	}
	if (mode == 3)
	{
		const float scaledDepth = FogParameters.z * depth;
		return saturate(exp(-(scaledDepth * scaledDepth)));
	}
	const float range = FogParameters.y - FogParameters.x;
	return abs(range) < 0.000001f ? 0.0f :
		saturate((FogParameters.y - depth) / range);
}

float4 ApplyLegacyPixelState(float4 color, float fogDepth)
{
	if (!PassesAlphaTest(color.a))
	{
		clip(-1.0f);
	}
	const float fogFactor = CalculateFogFactor(abs(fogDepth));
	return saturate(lerp(FogColor, color, fogFactor));
}

struct VertexInput
{
	float3 position : POSITION;
	float4 color : COLOR0;
};

struct VertexOutput
{
	float4 position : SV_POSITION;
	float4 color : COLOR0;
	float fogDepth : TEXCOORD0;
};

VertexOutput VSMain(VertexInput input)
{
	VertexOutput output;
	const float4 objectPosition = float4(input.position, 1.0f);
	output.position = mul(objectPosition, WorldViewProjection);
	output.color = input.color;
	output.fogDepth = mul(objectPosition, WorldView).z;
	return output;
}

float4 PSMain(VertexOutput input) : SV_TARGET
{
	return ApplyLegacyPixelState(input.color, input.fogDepth);
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
	float fogDepth : TEXCOORD1;
};

TexturedVertexOutput VSTextured(TexturedVertexInput input)
{
	TexturedVertexOutput output;
	const float4 objectPosition = float4(input.position, 1.0f);
	output.position = mul(objectPosition, WorldViewProjection);
	output.color = input.color;
	output.textureCoordinate = input.textureCoordinate;
	output.fogDepth = mul(objectPosition, WorldView).z;
	return output;
}

Texture2D LegacyTexture0 : register(t0);
SamplerState LegacySampler0 : register(s0);

float4 PSTextured(TexturedVertexOutput input) : SV_TARGET
{
	const float4 color = input.color *
		LegacyTexture0.Sample(LegacySampler0, input.textureCoordinate);
	return ApplyLegacyPixelState(color, input.fogDepth);
}
