cbuffer LegacyTransformConstants : register(b0)
{
	row_major float4x4 WorldViewProjection;
	row_major float4x4 WorldView;
	float4 FogColor;
	float4 FogParameters;
	uint4 AlphaTestParameters;
	uint4 TextureColorParameters;
	uint4 TextureAlphaParameters;
	float4 TextureFactor;
	row_major float4x4 World;
	float4 MaterialDiffuse;
	float4 MaterialAmbient;
	float4 MaterialEmissive;
	float4 GlobalAmbient;
	float4 LightDiffuse[4];
	float4 LightAmbient[4];
	float4 LightPositionAndType[4];
	float4 LightDirectionAndEnabled[4];
	float4 LightAttenuation[4];
	float4 LightSpotParameters[4];
	uint4 LightingParameters;
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

float4 SelectTextureArgument(uint argument, float4 current, float4 diffuse,
	float4 textureSample)
{
	switch (argument)
	{
	case 0: return current;
	case 1: return diffuse;
	case 2: return textureSample;
	case 3: return TextureFactor;
	case 4: return 0.0f;
	case 5: return 0.0f;
	default: return current;
	}
}

float4 ApplyTextureOperation(uint operation, float4 argument1,
	float4 argument2, float4 current, float4 diffuse, float4 textureSample)
{
	switch (operation)
	{
	case 0: return current;
	case 1: return argument1;
	case 2: return argument2;
	case 3: return argument1 * argument2;
	case 4: return 2.0f * argument1 * argument2;
	case 5: return 4.0f * argument1 * argument2;
	case 6: return argument1 + argument2;
	case 7: return argument1 + argument2 - 0.5f;
	case 8: return 2.0f * (argument1 + argument2 - 0.5f);
	case 9: return argument1 - argument2;
	case 10: return argument1 + argument2 * (1.0f - argument1);
	case 11: return lerp(argument2, argument1, diffuse.a);
	case 12: return lerp(argument2, argument1, textureSample.a);
	case 13: return lerp(argument2, argument1, current.a);
	case 14: return argument1 * argument1.a + argument2;
	case 15:
		const float dotProduct = 4.0f * dot(argument1.rgb - 0.5f,
			argument2.rgb - 0.5f);
		return float4(dotProduct, dotProduct, dotProduct, dotProduct);
	default: return current;
	}
}

float4 ApplyLegacyLighting(float4 vertexColor, float3 objectPosition,
	float3 objectNormal)
{
	if (LightingParameters.x == 0)
	{
		return vertexColor;
	}
	const float3 worldPosition = mul(float4(objectPosition, 1.0f), World).xyz;
	float3 worldNormal = mul(objectNormal, (float3x3)World);
	if (LightingParameters.y != 0)
	{
		worldNormal = normalize(worldNormal);
	}
	float3 litColor = MaterialEmissive.rgb +
		MaterialAmbient.rgb * GlobalAmbient.rgb;
	[unroll]
	for (uint index = 0; index < 4; ++index)
	{
		if (LightDirectionAndEnabled[index].w == 0.0f)
		{
			continue;
		}
		float3 directionToLight = float3(0.0f, 0.0f, 1.0f);
		float attenuation = 1.0f;
		if ((uint)LightPositionAndType[index].w == 0)
		{
			directionToLight = normalize(-LightDirectionAndEnabled[index].xyz);
		}
		else
		{
			const float3 offset = LightPositionAndType[index].xyz - worldPosition;
			const float distanceToLight = length(offset);
			if (distanceToLight > LightAttenuation[index].x)
			{
				continue;
			}
			directionToLight = distanceToLight > 0.000001f ?
				offset / distanceToLight : float3(0.0f, 0.0f, 1.0f);
			const float denominator = LightAttenuation[index].y +
				LightAttenuation[index].z * distanceToLight +
				LightAttenuation[index].w * distanceToLight * distanceToLight;
			attenuation = denominator > 0.000001f ? 1.0f / denominator : 1.0f;
			if ((uint)LightPositionAndType[index].w == 2)
			{
				const float cosine = dot(normalize(LightDirectionAndEnabled[index].xyz),
					-directionToLight);
				const float innerCosine = cos(0.5f * LightSpotParameters[index].x);
				const float outerCosine = cos(0.5f * LightSpotParameters[index].y);
				if (cosine <= outerCosine)
				{
					continue;
				}
				const float coneRange = max(innerCosine - outerCosine, 0.000001f);
				const float cone = saturate((cosine - outerCosine) / coneRange);
				attenuation *= pow(cone, max(LightSpotParameters[index].z, 0.0f));
			}
		}
		litColor += LightAmbient[index].rgb * MaterialAmbient.rgb;
		litColor += LightDiffuse[index].rgb * MaterialDiffuse.rgb *
			saturate(dot(worldNormal, directionToLight)) * attenuation;
	}
	return float4(saturate(litColor) * vertexColor.rgb,
		MaterialDiffuse.a * vertexColor.a);
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
	output.color = ApplyLegacyLighting(input.color, input.position, input.normal);
	output.textureCoordinate = input.textureCoordinate;
	output.fogDepth = mul(objectPosition, WorldView).z;
	return output;
}

Texture2D LegacyTexture0 : register(t0);
SamplerState LegacySampler0 : register(s0);

float4 PSTextured(TexturedVertexOutput input) : SV_TARGET
{
	const float4 textureSample =
		LegacyTexture0.Sample(LegacySampler0, input.textureCoordinate);
	const float4 current = input.color;
	const float4 colorArgument1 = SelectTextureArgument(TextureColorParameters.y,
		current, input.color, textureSample);
	const float4 colorArgument2 = SelectTextureArgument(TextureColorParameters.z,
		current, input.color, textureSample);
	float4 color = ApplyTextureOperation(TextureColorParameters.x,
		colorArgument1, colorArgument2, current, input.color, textureSample);
	if (TextureColorParameters.w != 0)
	{
		const float4 alphaArgument1 = SelectTextureArgument(
			TextureAlphaParameters.x, current, input.color, textureSample);
		const float4 alphaArgument2 = SelectTextureArgument(
			TextureAlphaParameters.y, current, input.color, textureSample);
		color.a = ApplyTextureOperation(TextureColorParameters.w, alphaArgument1,
			alphaArgument2, current, input.color, textureSample).a;
	}
	return ApplyLegacyPixelState(color, input.fogDepth);
}
