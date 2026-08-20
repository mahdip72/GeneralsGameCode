cbuffer LegacyTransformConstants : register(b0)
{
	row_major float4x4 WorldViewProjection;
	row_major float4x4 WorldView;
	float4 FogColor;
	float4 FogParameters;
	uint4 AlphaTestParameters;
	uint4 TextureColorParameters[8];
	uint4 TextureAlphaParameters[8];
	uint4 TextureModifierParameters[8];
	float4 TextureBumpParameters0[8];
	float4 TextureBumpParameters1[8];
	float4 TextureFactor;
	row_major float4x4 World;
	row_major float4x4 TextureTransforms[8];
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
	float4 specular, float4 textureSample, float4 temporary)
{
	switch (argument)
	{
	case 0: return current;
	case 1: return diffuse;
	case 2: return textureSample;
	case 3: return TextureFactor;
	case 4: return specular;
	case 5: return temporary;
	default: return current;
	}
}

float4 ApplyTextureArgumentModifiers(float4 value, uint modifiers,
	uint argumentIndex)
{
	const uint shift = argumentIndex * 2;
	if ((modifiers & (2U << shift)) != 0)
	{
		value = value.aaaa;
	}
	if ((modifiers & (1U << shift)) != 0)
	{
		value = 1.0f - value;
	}
	return value;
}

float4 ApplyTextureOperation(uint operation, float4 argument1,
	float4 argument2, float4 argument0, float4 current, float4 diffuse,
	float4 textureSample)
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
	case 18: return argument1 + argument2 * (1.0f - textureSample.a);
	case 19: return lerp(argument2, argument1, TextureFactor.a);
	case 20: return argument1 * argument2;
	case 21: return argument1 + argument1.a * argument2;
	case 22: return argument1 + (1.0f - argument1.a) * argument2;
	case 23: return argument1.a + (1.0f - argument1) * argument2;
	case 24: return argument1 * argument2 + argument0;
	case 25: return argument1 * argument0 + argument2 * (1.0f - argument0);
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
	float4 textureCoordinate0 : TEXCOORD0;
	float4 textureCoordinate1 : TEXCOORD1;
	float4 textureCoordinate2 : TEXCOORD2;
	float4 textureCoordinate3 : TEXCOORD3;
	float4 textureCoordinate4 : TEXCOORD4;
	float4 textureCoordinate5 : TEXCOORD5;
	float4 textureCoordinate6 : TEXCOORD6;
	float4 textureCoordinate7 : TEXCOORD7;
};

struct TexturedVertexOutput
{
	float4 position : SV_POSITION;
	float4 color : COLOR0;
	float4 textureCoordinate0 : TEXCOORD0;
	float4 textureCoordinate1 : TEXCOORD1;
	float4 textureCoordinate2 : TEXCOORD2;
	float4 textureCoordinate3 : TEXCOORD3;
	float4 textureCoordinate4 : TEXCOORD4;
	float4 textureCoordinate5 : TEXCOORD5;
	float4 textureCoordinate6 : TEXCOORD6;
	float4 textureCoordinate7 : TEXCOORD7;
	float fogDepth : TEXCOORD8;
};

TexturedVertexOutput VSTextured(TexturedVertexInput input)
{
	TexturedVertexOutput output;
	const float4 objectPosition = float4(input.position, 1.0f);
	output.position = mul(objectPosition, WorldViewProjection);
	output.color = ApplyLegacyLighting(input.color, input.position, input.normal);
	output.textureCoordinate0 = input.textureCoordinate0;
	output.textureCoordinate1 = input.textureCoordinate1;
	output.textureCoordinate2 = input.textureCoordinate2;
	output.textureCoordinate3 = input.textureCoordinate3;
	output.textureCoordinate4 = input.textureCoordinate4;
	output.textureCoordinate5 = input.textureCoordinate5;
	output.textureCoordinate6 = input.textureCoordinate6;
	output.textureCoordinate7 = input.textureCoordinate7;
	output.fogDepth = mul(objectPosition, WorldView).z;
	return output;
}

Texture2D LegacyTexture0 : register(t0);
Texture2D LegacyTexture1 : register(t1);
Texture2D LegacyTexture2 : register(t2);
Texture2D LegacyTexture3 : register(t3);
Texture2D LegacyTexture4 : register(t4);
Texture2D LegacyTexture5 : register(t5);
Texture2D LegacyTexture6 : register(t6);
Texture2D LegacyTexture7 : register(t7);
SamplerState LegacySampler0 : register(s0);
SamplerState LegacySampler1 : register(s1);
SamplerState LegacySampler2 : register(s2);
SamplerState LegacySampler3 : register(s3);
SamplerState LegacySampler4 : register(s4);
SamplerState LegacySampler5 : register(s5);
SamplerState LegacySampler6 : register(s6);
SamplerState LegacySampler7 : register(s7);

float4 GetTextureCoordinate(TexturedVertexOutput input, uint index)
{
	switch (index)
	{
	case 1: return input.textureCoordinate1;
	case 2: return input.textureCoordinate2;
	case 3: return input.textureCoordinate3;
	case 4: return input.textureCoordinate4;
	case 5: return input.textureCoordinate5;
	case 6: return input.textureCoordinate6;
	case 7: return input.textureCoordinate7;
	default: return input.textureCoordinate0;
	}
}

float4 SampleLegacyTexture(uint stage, float4 coordinate)
{
	const float denominator = abs(coordinate.w) > 0.000001f ? coordinate.w : 1.0f;
	const float2 uv = coordinate.xy / denominator;
	switch (stage)
	{
	case 0: return LegacyTexture0.Sample(LegacySampler0, uv);
	case 1: return LegacyTexture1.Sample(LegacySampler1, uv);
	case 2: return LegacyTexture2.Sample(LegacySampler2, uv);
	case 3: return LegacyTexture3.Sample(LegacySampler3, uv);
	case 4: return LegacyTexture4.Sample(LegacySampler4, uv);
	case 5: return LegacyTexture5.Sample(LegacySampler5, uv);
	case 6: return LegacyTexture6.Sample(LegacySampler6, uv);
	default: return LegacyTexture7.Sample(LegacySampler7, uv);
	}
}

float4 PSTextured(TexturedVertexOutput input) : SV_TARGET
{
	float4 current = input.color;
	float4 temporary = 0.0f;
	float4 coordinateOffsets[8] = {
		float4(0.0f, 0.0f, 0.0f, 0.0f),
		float4(0.0f, 0.0f, 0.0f, 0.0f),
		float4(0.0f, 0.0f, 0.0f, 0.0f),
		float4(0.0f, 0.0f, 0.0f, 0.0f),
		float4(0.0f, 0.0f, 0.0f, 0.0f),
		float4(0.0f, 0.0f, 0.0f, 0.0f),
		float4(0.0f, 0.0f, 0.0f, 0.0f),
		float4(0.0f, 0.0f, 0.0f, 0.0f)
	};
	[unroll]
	for (uint stage = 0; stage < 8; ++stage)
	{
		const uint colorOperation = TextureColorParameters[stage].x;
		if (colorOperation == 0)
		{
			break;
		}
		const uint coordinateIndex = TextureModifierParameters[stage].z & 7U;
		float4 coordinate = GetTextureCoordinate(input, coordinateIndex) +
			coordinateOffsets[stage];
		coordinate = mul(coordinate, TextureTransforms[stage]);
		if (TextureModifierParameters[stage].w == 0)
		{
			coordinate.w = 1.0f;
		}
		const float4 textureSample = SampleLegacyTexture(stage, coordinate);
		if (colorOperation == 16 || colorOperation == 17)
		{
			if (stage + 1 < 8)
			{
				const float2 bump = textureSample.rg * 2.0f - 1.0f;
				coordinateOffsets[stage + 1].xy += float2(
					dot(bump, TextureBumpParameters0[stage].xy),
					dot(bump, TextureBumpParameters0[stage].zw));
			}
			if (colorOperation == 17)
			{
				current.rgb *= saturate(textureSample.b *
					TextureBumpParameters1[stage].x +
					TextureBumpParameters1[stage].y);
			}
			continue;
		}
		const uint colorModifiers = TextureModifierParameters[stage].x;
		const float4 colorArgument0 = ApplyTextureArgumentModifiers(
			SelectTextureArgument(TextureColorParameters[stage].y, current,
				input.color, 0.0f, textureSample, temporary), colorModifiers, 0);
		const float4 colorArgument1 = ApplyTextureArgumentModifiers(
			SelectTextureArgument(TextureColorParameters[stage].z, current,
				input.color, 0.0f, textureSample, temporary), colorModifiers, 1);
		const float4 colorArgument2 = ApplyTextureArgumentModifiers(
			SelectTextureArgument(TextureColorParameters[stage].w, current,
				input.color, 0.0f, textureSample, temporary), colorModifiers, 2);
		float4 stageResult = ApplyTextureOperation(colorOperation,
			colorArgument1, colorArgument2, colorArgument0, current, input.color,
			textureSample);
		const uint alphaOperation = TextureAlphaParameters[stage].x;
		if (alphaOperation != 0)
		{
			const uint alphaModifiers = TextureModifierParameters[stage].y;
			const float4 alphaArgument0 = ApplyTextureArgumentModifiers(
				SelectTextureArgument(TextureAlphaParameters[stage].y, current,
					input.color, 0.0f, textureSample, temporary), alphaModifiers, 0);
			const float4 alphaArgument1 = ApplyTextureArgumentModifiers(
				SelectTextureArgument(TextureAlphaParameters[stage].z, current,
					input.color, 0.0f, textureSample, temporary), alphaModifiers, 1);
			const float4 alphaArgument2 = ApplyTextureArgumentModifiers(
				SelectTextureArgument(TextureAlphaParameters[stage].w, current,
					input.color, 0.0f, textureSample, temporary), alphaModifiers, 2);
			stageResult.a = ApplyTextureOperation(alphaOperation, alphaArgument1,
				alphaArgument2, alphaArgument0, current, input.color,
				textureSample).a;
		}
		stageResult = saturate(stageResult);
		if ((TextureModifierParameters[stage].z & 0x100U) != 0)
		{
			temporary = stageResult;
		}
		else
		{
			current = stageResult;
		}
	}
	return ApplyLegacyPixelState(current, input.fogDepth);
}
