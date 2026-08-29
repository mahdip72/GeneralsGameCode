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
	float4 MaterialSpecular;
	float4 MaterialEmissive;
	float4 MaterialSpecularPower;
	float4 GlobalAmbient;
	float4 LightDiffuse[4];
	float4 LightAmbient[4];
	float4 LightSpecular[4];
	float4 LightPositionAndType[4];
	float4 LightDirectionAndEnabled[4];
	float4 LightAttenuation[4];
	float4 LightSpotParameters[4];
	uint4 LightingParameters;
	uint4 VertexLayoutParameters;
	float4 ViewportParameters;
	uint4 ProgramParameters;
	float4 LegacyVertexConstants[34];
	float4 LegacyPixelConstants[8];
	row_major float4x4 View;
	// CPU-built inverse-transpose matrices.  Each row is padded to a full
	// constant-buffer register; vertex processing only performs the multiply.
	row_major float3x3 WorldNormalMatrix;
	row_major float3x3 WorldViewNormalMatrix;
	float4 ClipPlanes[6];
	uint4 ClipPlaneParameters;
	uint4 FogStateParameters;
};

bool UsesPreTransformedPosition()
{
	return (VertexLayoutParameters.w & 0x80000000U) != 0;
}

float4 TransformLegacyPosition(float4 position)
{
	if (!UsesPreTransformedPosition())
	{
		return mul(float4(position.xyz, 1.0f), WorldViewProjection);
	}
	const float width = max(ViewportParameters.z, 1.0f);
	const float height = max(ViewportParameters.w, 1.0f);
	// D3D8's pixel centers are on integer coordinates.  POSITIONT callers use
	// the conventional -0.5 offset; D3D11 pixel centers are half-integers.
	const float screenX = position.x + 0.5f;
	const float screenY = position.y + 0.5f;
	const float ndcX = ((screenX - ViewportParameters.x) / width) * 2.0f - 1.0f;
	const float ndcY = 1.0f - ((screenY - ViewportParameters.y) / height) * 2.0f;
	const float reciprocalHomogeneousW = abs(position.w) > 0.000001f ?
		position.w : 1.0f;
	const float clipW = 1.0f / reciprocalHomogeneousW;
	return float4(ndcX * clipW, ndcY * clipW, position.z * clipW, clipW);
}

float GetLegacyClipDistance(uint plane, float3 worldPosition,
	bool preTransformed)
{
	if (preTransformed || (ClipPlaneParameters.x & (1U << plane)) == 0U)
	{
		return 1.0f;
	}
	return dot(float4(worldPosition, 1.0f), ClipPlanes[plane]);
}

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

float CalculateFogDepth(float3 cameraPosition, bool preTransformed)
{
	if (preTransformed)
	{
		return cameraPosition.z;
	}
	return FogStateParameters.x != 0U ?
		length(cameraPosition) : cameraPosition.z;
}

float4 ApplyLegacyPixelState(float4 color, float fogDepth)
{
	if (!PassesAlphaTest(color.a))
	{
		clip(-1.0f);
	}
	const float fogFactor = CalculateFogFactor(abs(fogDepth));
	const float3 foggedRgb = saturate(lerp(FogColor.rgb, color.rgb,
		fogFactor));
	return float4(foggedRgb, color.a);
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
	float4 textureSample, bool alphaOperation)
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
	case 14:
		// D3DTOP_MODULATEALPHA_ADDCOLOR is COLOROP-only:
		// Arg1.RGB + Arg1.A * Arg2.RGB.  Preserve the separately computed
		// alpha channel when this function is used for the color operation.
		return alphaOperation ? current :
			float4(argument1.rgb + argument1.a * argument2.rgb, current.a);
	case 15:
		const float dotProduct = 4.0f * dot(argument1.rgb - 0.5f,
			argument2.rgb - 0.5f);
		// COLOROP updates RGB only; ALPHAOP receives the scalar result.
		return alphaOperation ? float4(current.rgb, dotProduct) :
			float4(dotProduct, dotProduct, dotProduct, current.a);
	case 18: return argument1 + argument2 * (1.0f - textureSample.a);
	case 19: return lerp(argument2, argument1, TextureFactor.a);
	case 20:
		// D3DTOP_PREMODULATE outputs Arg1.  Its additional effect on the
		// following stage's D3DTA_CURRENT is applied by PSTextured below.
		return argument1;
	case 21:
		// D3DTOP_MODULATECOLOR_ADDALPHA is COLOROP-only:
		// Arg1.RGB * Arg2.RGB + Arg1.A.
		return alphaOperation ? current :
			float4(argument1.rgb * argument2.rgb + argument1.a, current.a);
	case 22:
		// D3DTOP_MODULATEINVALPHA_ADDCOLOR is COLOROP-only:
		// (1 - Arg1.A) * Arg2.RGB + Arg1.RGB.
		return alphaOperation ? current :
			float4(argument1.rgb + (1.0f - argument1.a) * argument2.rgb,
			current.a);
	case 23:
		// D3DTOP_MODULATEINVCOLOR_ADDALPHA is COLOROP-only:
		// (1 - Arg1.RGB) * Arg2.RGB + Arg1.A.
		return alphaOperation ? current :
			float4((1.0f - argument1.rgb) * argument2.rgb + argument1.a,
			current.a);
	case 24: return argument1 * argument2 + argument0;
	case 25: return argument1 * argument0 + argument2 * (1.0f - argument0);
	default: return current;
	}
}

float4 SelectLegacyMaterialSource(uint source, float4 materialColor,
	float4 color1, float4 color2)
{
	if (source == 1U)
	{
		return color1;
	}
	if (source == 2U)
	{
		return color2;
	}
	return materialColor;
}

float3 GetLegacyCameraSpaceNormal(float3 objectNormal, bool hasNormal,
	bool preTransformed)
{
	if (!hasNormal || preTransformed)
	{
		return float3(0.0f, 0.0f, 1.0f);
	}
	float3 transformed = mul(objectNormal, WorldViewNormalMatrix);
	const float lengthSquared = dot(transformed, transformed);
	if (lengthSquared <= 0.000001f)
	{
		return float3(0.0f, 0.0f, 1.0f);
	}
	if (LightingParameters.y != 0U)
	{
		transformed /= sqrt(lengthSquared);
	}
	return transformed;
}

float4 ApplyLegacyLighting(float4 vertexColor, float4 vertexSpecular,
	float3 objectPosition, float3 objectNormal,
	out float3 generatedSpecular)
{
	generatedSpecular = float3(0.0f, 0.0f, 0.0f);
	if (LightingParameters.x == 0)
	{
		return vertexColor;
	}
	const float3 worldPosition = mul(float4(objectPosition, 1.0f), World).xyz;
	float3 worldNormal = mul(objectNormal, WorldNormalMatrix);
	const float worldNormalLengthSquared = dot(worldNormal, worldNormal);
	if (worldNormalLengthSquared <= 0.000001f)
	{
		worldNormal = float3(0.0f, 0.0f, 1.0f);
	}
	else if (LightingParameters.y != 0U)
	{
		worldNormal /= sqrt(worldNormalLengthSquared);
	}
	const float4 diffuseMaterial = SelectLegacyMaterialSource(
		(ProgramParameters.z >> 2) & 3U, MaterialDiffuse,
		vertexColor, vertexSpecular);
	const float4 ambientMaterial = SelectLegacyMaterialSource(
		ProgramParameters.z & 3U, MaterialAmbient,
		vertexColor, vertexSpecular);
	const float4 emissiveMaterial = SelectLegacyMaterialSource(
		(ProgramParameters.z >> 4) & 3U, MaterialEmissive,
		vertexColor, vertexSpecular);
	const float4 specularMaterial = SelectLegacyMaterialSource(
		ProgramParameters.w & 3U, MaterialSpecular,
		vertexColor, vertexSpecular);
	float3 litColor = emissiveMaterial.rgb +
		ambientMaterial.rgb * GlobalAmbient.rgb;
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
		litColor += LightAmbient[index].rgb * ambientMaterial.rgb;
		litColor += LightDiffuse[index].rgb * diffuseMaterial.rgb *
			saturate(dot(worldNormal, directionToLight)) * attenuation;
		if (ProgramParameters.y != 0U && MaterialSpecularPower.x > 0.0f)
		{
			// D3D8 LOCALVIEWER defaults to FALSE.  In that mode the viewer is
			// the fixed +Z direction in eye space, so form the half vector in
			// view space while retaining the world-space light calculation above.
			float3 viewNormal = mul(objectNormal, WorldViewNormalMatrix);
			const float viewNormalLengthSquared = dot(viewNormal, viewNormal);
			viewNormal = viewNormalLengthSquared <= 0.000001f ?
				float3(0.0f, 0.0f, 1.0f) :
				viewNormal / sqrt(viewNormalLengthSquared);
			const float3 viewLightDirection = normalize(mul(directionToLight,
				(float3x3)View));
			const float3 halfVector = normalize(viewLightDirection +
				float3(0.0f, 0.0f, 1.0f));
			const float specularTerm = pow(saturate(dot(viewNormal,
				halfVector)), MaterialSpecularPower.x);
			generatedSpecular += LightSpecular[index].rgb *
				specularMaterial.rgb * specularTerm * attenuation;
		}
	}
	return float4(saturate(litColor), diffuseMaterial.a);
}

struct VertexInput
{
	float4 position : POSITION;
	float4 color : COLOR0;
};

struct VertexOutput
{
	float4 position : SV_POSITION;
	float4 color : COLOR0;
	float fogDepth : TEXCOORD0;
	float4 clipDistance0 : SV_ClipDistance0;
	float4 clipDistance1 : SV_ClipDistance1;
};

VertexOutput VSMain(VertexInput input)
{
	VertexOutput output;
	const float4 objectPosition = float4(input.position.xyz, 1.0f);
	output.position = TransformLegacyPosition(input.position);
	output.color = input.color;
	const float3 cameraPosition = UsesPreTransformedPosition() ?
		input.position.xyz : mul(objectPosition, WorldView).xyz;
	output.fogDepth = CalculateFogDepth(cameraPosition,
		UsesPreTransformedPosition());
	const float3 worldPosition = mul(objectPosition, World).xyz;
	output.clipDistance0 = float4(
		GetLegacyClipDistance(0, worldPosition, UsesPreTransformedPosition()),
		GetLegacyClipDistance(1, worldPosition, UsesPreTransformedPosition()),
		GetLegacyClipDistance(2, worldPosition, UsesPreTransformedPosition()),
		GetLegacyClipDistance(3, worldPosition, UsesPreTransformedPosition()));
	output.clipDistance1 = float4(
		GetLegacyClipDistance(4, worldPosition, UsesPreTransformedPosition()),
		GetLegacyClipDistance(5, worldPosition, UsesPreTransformedPosition()),
		1.0f, 1.0f);
	return output;
}

float4 PSMain(VertexOutput input) : SV_TARGET
{
	return ApplyLegacyPixelState(input.color, input.fogDepth);
}

struct TexturedVertexInput
{
	float4 position : POSITION;
	float3 normal : NORMAL;
	float4 color : COLOR0;
	float4 specular : COLOR1;
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
	float4 specular : COLOR1;
	float4 textureCoordinate0 : TEXCOORD0;
	float4 textureCoordinate1 : TEXCOORD1;
	float4 textureCoordinate2 : TEXCOORD2;
	float4 textureCoordinate3 : TEXCOORD3;
	float4 textureCoordinate4 : TEXCOORD4;
	float4 textureCoordinate5 : TEXCOORD5;
	float4 textureCoordinate6 : TEXCOORD6;
	float4 textureCoordinate7 : TEXCOORD7;
	float fogDepth : TEXCOORD8;
	float3 cameraPosition : TEXCOORD9;
	float3 cameraNormal : TEXCOORD10;
	float4 clipDistance0 : SV_ClipDistance0;
	float4 clipDistance1 : SV_ClipDistance1;
};

TexturedVertexOutput VSTextured(TexturedVertexInput input)
{
	TexturedVertexOutput output;
	if (ProgramParameters.x == 1U)
	{
		const uint swayIndex = min((uint)max(round(input.normal.x), 0.0f) + 8U,
			33U);
		const float3 sway = LegacyVertexConstants[swayIndex].xyz;
		const float heightAboveBase = input.position.z - input.normal.z;
		const float3 animatedPosition = input.position.xyz +
			heightAboveBase * sway;
		const float4 animatedObjectPosition = float4(animatedPosition, 1.0f);
		output.position = mul(animatedObjectPosition, WorldViewProjection);
		const float4 diffuse = VertexLayoutParameters.y != 0 ?
			input.color : float4(1.0f, 1.0f, 1.0f, 1.0f);
		output.color = float4(diffuse.rgb * input.normal.y, diffuse.a);
		output.specular = 0.0f;
		output.textureCoordinate0 = input.textureCoordinate0;
		output.textureCoordinate1 =
			(float4(input.position.xyz, 1.0f) + LegacyVertexConstants[32]) *
			LegacyVertexConstants[33];
		output.textureCoordinate2 = 0.0f;
		output.textureCoordinate3 = 0.0f;
		output.textureCoordinate4 = 0.0f;
		output.textureCoordinate5 = 0.0f;
		output.textureCoordinate6 = 0.0f;
		output.textureCoordinate7 = 0.0f;
		const float4 cameraPosition = mul(animatedObjectPosition, WorldView);
		output.fogDepth = CalculateFogDepth(cameraPosition.xyz, false);
		output.cameraPosition = cameraPosition.xyz;
		output.cameraNormal = float3(0.0f, 0.0f, 1.0f);
		const float3 worldPosition = mul(animatedObjectPosition, World).xyz;
		output.clipDistance0 = float4(
			GetLegacyClipDistance(0, worldPosition, false),
			GetLegacyClipDistance(1, worldPosition, false),
			GetLegacyClipDistance(2, worldPosition, false),
			GetLegacyClipDistance(3, worldPosition, false));
		output.clipDistance1 = float4(
			GetLegacyClipDistance(4, worldPosition, false),
			GetLegacyClipDistance(5, worldPosition, false), 1.0f, 1.0f);
		return output;
	}
	const float4 objectPosition = float4(input.position.xyz, 1.0f);
	output.position = TransformLegacyPosition(input.position);
	const float4 diffuse = VertexLayoutParameters.y != 0 ?
		input.color : float4(1.0f, 1.0f, 1.0f, 1.0f);
	const float3 normal = VertexLayoutParameters.x != 0 ?
		input.normal : float3(0.0f, 0.0f, 1.0f);
	const float4 specular = VertexLayoutParameters.z != 0 ?
		input.specular : float4(1.0f, 1.0f, 1.0f, 1.0f);
	float3 generatedSpecular = float3(0.0f, 0.0f, 0.0f);
	output.color = UsesPreTransformedPosition() ? diffuse :
		ApplyLegacyLighting(diffuse, specular, input.position.xyz, normal,
			generatedSpecular);
	// COLOR2 is an input to the material-source selector, not a prerequisite
	// for the secondary color generated by fixed-function lighting.  In
	// particular, ordinary position/normal/diffuse meshes have no COLOR2 but
	// must still expose D3DTA_SPECULAR when material specular is enabled.
	output.specular = VertexLayoutParameters.z != 0 &&
		((ProgramParameters.w & 3U) == 2U) ? input.specular : 0.0f;
	if (!UsesPreTransformedPosition() && LightingParameters.x != 0 &&
		ProgramParameters.y != 0U)
	{
		output.specular = saturate(output.specular +
			float4(generatedSpecular, 0.0f));
	}
	output.textureCoordinate0 = input.textureCoordinate0;
	output.textureCoordinate1 = input.textureCoordinate1;
	output.textureCoordinate2 = input.textureCoordinate2;
	output.textureCoordinate3 = input.textureCoordinate3;
	output.textureCoordinate4 = input.textureCoordinate4;
	output.textureCoordinate5 = input.textureCoordinate5;
	output.textureCoordinate6 = input.textureCoordinate6;
	output.textureCoordinate7 = input.textureCoordinate7;
	const float4 cameraPosition = mul(objectPosition, WorldView);
	output.fogDepth = CalculateFogDepth(UsesPreTransformedPosition() ?
		input.position.xyz : cameraPosition.xyz, UsesPreTransformedPosition());
	output.cameraPosition = cameraPosition.xyz;
	output.cameraNormal = GetLegacyCameraSpaceNormal(normal,
		VertexLayoutParameters.x != 0U, UsesPreTransformedPosition());
	const float3 worldPosition = mul(objectPosition, World).xyz;
	output.clipDistance0 = float4(
		GetLegacyClipDistance(0, worldPosition, UsesPreTransformedPosition()),
		GetLegacyClipDistance(1, worldPosition, UsesPreTransformedPosition()),
		GetLegacyClipDistance(2, worldPosition, UsesPreTransformedPosition()),
		GetLegacyClipDistance(3, worldPosition, UsesPreTransformedPosition()));
	output.clipDistance1 = float4(
		GetLegacyClipDistance(4, worldPosition, UsesPreTransformedPosition()),
		GetLegacyClipDistance(5, worldPosition, UsesPreTransformedPosition()),
		1.0f, 1.0f);
	return output;
}

// The GeForce3 sea path uses a small, dedicated vertex declaration rather
// than the regular W3D textured declaration.  Keep its register contract
// explicit: c2-c5 is the per-patch world/view/projection matrix and c6 is the
// four-component screen-space reflection scale/offset.
struct SeaWaveVertexInput
{
	float3 position : POSITION;
	float4 diffuse : COLOR0;
	float2 bumpCoordinate : TEXCOORD0;
};

struct SeaWaveVertexOutput
{
	float4 position : SV_POSITION;
	float4 diffuse : COLOR0;
	float2 bumpCoordinate : TEXCOORD0;
	float2 reflectionCoordinate : TEXCOORD1;
	float fogDepth : TEXCOORD2;
	float4 clipDistance0 : SV_ClipDistance0;
	float4 clipDistance1 : SV_ClipDistance1;
};

SeaWaveVertexOutput VSSeaWave(SeaWaveVertexInput input)
{
	SeaWaveVertexOutput output;
	const float4 objectPosition = float4(input.position, 1.0f);
	float4 clipPosition;
	clipPosition.x = dot(objectPosition, LegacyVertexConstants[2]);
	clipPosition.y = dot(objectPosition, LegacyVertexConstants[3]);
	clipPosition.z = dot(objectPosition, LegacyVertexConstants[4]);
	clipPosition.w = dot(objectPosition, LegacyVertexConstants[5]);
	output.position = clipPosition;
	const float reciprocalW = abs(clipPosition.w) > 0.000001f ?
		1.0f / clipPosition.w : 0.0f;
	const float2 projectedPosition = clipPosition.xy * reciprocalW;
	output.reflectionCoordinate = projectedPosition *
		LegacyVertexConstants[6].xy + LegacyVertexConstants[6].zw;
	output.bumpCoordinate = input.bumpCoordinate;
	output.diffuse = input.diffuse;
	// c7-c10 is the transposed per-patch world/view matrix uploaded alongside
	// the c2-c5 WVP. Reconstruct the view-space position so range fog can use
	// viewer distance while z fog retains the legacy third-row result.
	const float3 cameraPosition = float3(
		dot(objectPosition, LegacyVertexConstants[7]),
		dot(objectPosition, LegacyVertexConstants[8]),
		dot(objectPosition, LegacyVertexConstants[9]));
	output.fogDepth = CalculateFogDepth(cameraPosition, false);
	const float3 worldPosition = mul(objectPosition, World).xyz;
	output.clipDistance0 = float4(
		GetLegacyClipDistance(0, worldPosition, false),
		GetLegacyClipDistance(1, worldPosition, false),
		GetLegacyClipDistance(2, worldPosition, false),
		GetLegacyClipDistance(3, worldPosition, false));
	output.clipDistance1 = float4(
		GetLegacyClipDistance(4, worldPosition, false),
		GetLegacyClipDistance(5, worldPosition, false), 1.0f, 1.0f);
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
// Cube SRVs use a disjoint register range so a cube can be sampled by its
// reflection vector without changing or invalidating the ordinary 2D paths.
TextureCube LegacyTextureCube0 : register(t8);
TextureCube LegacyTextureCube1 : register(t9);
TextureCube LegacyTextureCube2 : register(t10);
TextureCube LegacyTextureCube3 : register(t11);
TextureCube LegacyTextureCube4 : register(t12);
TextureCube LegacyTextureCube5 : register(t13);
TextureCube LegacyTextureCube6 : register(t14);
TextureCube LegacyTextureCube7 : register(t15);
SamplerState LegacySampler0 : register(s0);
SamplerState LegacySampler1 : register(s1);
SamplerState LegacySampler2 : register(s2);
SamplerState LegacySampler3 : register(s3);
SamplerState LegacySampler4 : register(s4);
SamplerState LegacySampler5 : register(s5);
SamplerState LegacySampler6 : register(s6);
SamplerState LegacySampler7 : register(s7);

bool HasTextureCoordinate(uint index)
{
	return index < 8U && (VertexLayoutParameters.w & (1U << index)) != 0U;
}

float4 GetTextureCoordinate(TexturedVertexOutput input, uint index)
{
	if (!HasTextureCoordinate(index))
	{
		return float4(0.0f, 0.0f, 0.0f, 1.0f);
	}
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

float4 GetLegacyTextureCoordinate(TexturedVertexOutput input, uint stage)
{
	const uint modifiers = TextureModifierParameters[stage].w;
	if ((modifiers & 16U) != 0)
	{
		const float3 incident = length(input.cameraPosition) > 0.000001f ?
			normalize(input.cameraPosition) : float3(0.0f, 0.0f, 1.0f);
		return float4(reflect(incident, input.cameraNormal), 1.0f);
	}
	if ((modifiers & 8U) != 0)
	{
		return float4(input.cameraNormal, 1.0f);
	}
	if ((modifiers & 2U) != 0)
	{
		return float4(input.cameraPosition, 1.0f);
	}
	return GetTextureCoordinate(input,
		TextureModifierParameters[stage].z & 7U);
}

float4 SampleLegacyTexture(uint stage, float4 coordinate)
{
	if ((LightingParameters.w & (1U << stage)) == 0)
	{
		return float4(1.0f, 1.0f, 1.0f, 1.0f);
	}
	const uint cubeMask = LightingParameters.w >> 8;
	if ((cubeMask & (1U << stage)) != 0U)
	{
		const float3 direction = coordinate.xyz;
		if (dot(direction, direction) <= 0.000001f)
		{
			return float4(1.0f, 1.0f, 1.0f, 1.0f);
		}
		switch (stage)
		{
		case 0: return LegacyTextureCube0.Sample(LegacySampler0, direction);
		case 1: return LegacyTextureCube1.Sample(LegacySampler1, direction);
		case 2: return LegacyTextureCube2.Sample(LegacySampler2, direction);
		case 3: return LegacyTextureCube3.Sample(LegacySampler3, direction);
		case 4: return LegacyTextureCube4.Sample(LegacySampler4, direction);
		case 5: return LegacyTextureCube5.Sample(LegacySampler5, direction);
		case 6: return LegacyTextureCube6.Sample(LegacySampler6, direction);
		default: return LegacyTextureCube7.Sample(LegacySampler7, direction);
		}
	}
	const uint transformCount = (TextureModifierParameters[stage].w >> 5) & 7U;
	const bool projected = (TextureModifierParameters[stage].w & 1U) != 0U;
	const float selectedDenominator = !projected ? 1.0f :
		(transformCount == 2U ? coordinate.y :
		(transformCount == 3U ? coordinate.z :
		(transformCount == 4U ? coordinate.w : 1.0f)));
	const float denominator = abs(selectedDenominator) > 0.000001f ?
		selectedDenominator : 1.0f;
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

float4 SampleProgramTexture(TexturedVertexOutput input, uint stage)
{
	float4 coordinate = GetLegacyTextureCoordinate(input, stage);
	if ((TextureModifierParameters[stage].w & 4U) != 0)
	{
		coordinate = mul(coordinate, TextureTransforms[stage]);
	}
	if ((TextureModifierParameters[stage].w & 1U) == 0)
	{
		coordinate.w = 1.0f;
	}
	return SampleLegacyTexture(stage, coordinate);
}

float4 ApplyLegacyPixelProgram(TexturedVertexOutput input)
{
	const uint program = LightingParameters.z;
	const float4 texture0 = SampleProgramTexture(input, 0);
	const float4 texture1 = SampleProgramTexture(input, 1);
	if (program == 3U)
	{
		return lerp(texture0, texture1, input.color.a) * input.color;
	}
	if (program == 4U)
	{
		return lerp(texture0, texture1, input.color.a) * input.color *
			SampleProgramTexture(input, 2);
	}
	if (program == 5U)
	{
		return lerp(texture0, texture1, input.color.a) * input.color *
			SampleProgramTexture(input, 2) * SampleProgramTexture(input, 3);
	}
	if (program == 6U)
	{
		return texture0 * texture1 * SampleProgramTexture(input, 2) * input.color;
	}
	if (program == 7U)
	{
		return texture1 * input.color;
	}
	if (program == 8U)
	{
		return texture1 * texture0 * input.color;
	}
	if (program == 9U)
	{
		return texture1 * texture0 * input.color * SampleProgramTexture(input, 2);
	}
	if (program == 10U)
	{
		return texture1 * texture0 * input.color * SampleProgramTexture(input, 2) *
			SampleProgramTexture(input, 3);
	}
	if (program == 11U)
	{
		const float luminance = dot(texture0.rgb, LegacyPixelConstants[0].rgb);
		const float4 filtered = luminance * LegacyPixelConstants[1];
		return lerp(texture0, filtered, LegacyPixelConstants[2]);
	}
	// Keep this value aligned with the appended
	// RENDER_LEGACY_PIXEL_PROFILER_SWIZZLE enum.  The legacy D3D8 ps_1_4
	// profiler shader writes B*c0 + G*c1 + R*c2 to RGB and preserves alpha.
	if (program == 13U)
	{
		const float3 swizzled = texture0.b * LegacyPixelConstants[0].rgb +
			texture0.g * LegacyPixelConstants[1].rgb +
			texture0.r * LegacyPixelConstants[2].rgb;
		return float4(swizzled, texture0.a);
	}
	return input.color;
}

float4 PSTextured(TexturedVertexOutput input) : SV_TARGET
{
	if (LightingParameters.z >= 3U)
	{
		return ApplyLegacyPixelState(saturate(ApplyLegacyPixelProgram(input)),
			input.fogDepth);
	}
	float4 current = input.color;
	float4 temporary = 0.0f;
	bool premodulateCurrent = false;
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
	[loop]
	for (uint stage = 0; stage < 8; ++stage)
	{
		const uint colorOperation = TextureColorParameters[stage].x;
		if (colorOperation == 0)
		{
			break;
		}
		float4 coordinate = GetLegacyTextureCoordinate(input, stage) +
			coordinateOffsets[stage];
		if ((TextureModifierParameters[stage].w & 4U) != 0)
		{
			coordinate = mul(coordinate, TextureTransforms[stage]);
		}
		if ((TextureModifierParameters[stage].w & 1U) == 0)
		{
			coordinate.w = 1.0f;
		}
		const float4 textureSample = SampleLegacyTexture(stage, coordinate);
		const float4 currentArgument = premodulateCurrent ?
			current * textureSample : current;
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
			SelectTextureArgument(TextureColorParameters[stage].y, currentArgument,
				input.color, input.specular, textureSample, temporary), colorModifiers, 0);
		const float4 colorArgument1 = ApplyTextureArgumentModifiers(
			SelectTextureArgument(TextureColorParameters[stage].z, currentArgument,
				input.color, input.specular, textureSample, temporary), colorModifiers, 1);
		const float4 colorArgument2 = ApplyTextureArgumentModifiers(
			SelectTextureArgument(TextureColorParameters[stage].w, currentArgument,
				input.color, input.specular, textureSample, temporary), colorModifiers, 2);
		float4 stageResult = ApplyTextureOperation(colorOperation,
			colorArgument1, colorArgument2, colorArgument0, current, input.color,
			textureSample, false);
		const uint alphaOperation = TextureAlphaParameters[stage].x;
		if (alphaOperation != 0)
		{
			const uint alphaModifiers = TextureModifierParameters[stage].y;
			const float4 alphaArgument0 = ApplyTextureArgumentModifiers(
				SelectTextureArgument(TextureAlphaParameters[stage].y, currentArgument,
					input.color, input.specular, textureSample, temporary), alphaModifiers, 0);
			const float4 alphaArgument1 = ApplyTextureArgumentModifiers(
				SelectTextureArgument(TextureAlphaParameters[stage].z, currentArgument,
					input.color, input.specular, textureSample, temporary), alphaModifiers, 1);
			const float4 alphaArgument2 = ApplyTextureArgumentModifiers(
				SelectTextureArgument(TextureAlphaParameters[stage].w, currentArgument,
					input.color, input.specular, textureSample, temporary), alphaModifiers, 2);
			stageResult.a = ApplyTextureOperation(alphaOperation, alphaArgument1,
				alphaArgument2, alphaArgument0, current, input.color,
				textureSample, true).a;
		}
		else
		{
			// COLOROP and ALPHAOP are independent D3D texture-stage state.
			// Keep the prior alpha when no alpha operation is supplied.
			stageResult.a = current.a;
		}
		stageResult = saturate(stageResult);
		premodulateCurrent = colorOperation == 20 || alphaOperation == 20;
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

float4 SampleWaterTexture0(float2 uv)
{
	return LegacyTexture0.Sample(LegacySampler0, uv);
}

float4 SampleWaterTexture1(float2 uv)
{
	return LegacyTexture1.Sample(LegacySampler1, uv);
}

float4 SampleWaterTexture2(float2 uv)
{
	return LegacyTexture2.Sample(LegacySampler2, uv);
}

float4 SampleWaterTexture3(float2 uv)
{
	return LegacyTexture3.Sample(LegacySampler3, uv);
}

float4 GetWaterTextureCoordinate(TexturedVertexOutput input, uint stage)
{
	float4 coordinate = GetLegacyTextureCoordinate(input, stage);
	if ((TextureModifierParameters[stage].w & 4U) != 0)
	{
		coordinate = mul(coordinate, TextureTransforms[stage]);
	}
	if ((TextureModifierParameters[stage].w & 1U) == 0)
	{
		coordinate.w = 1.0f;
	}
	return coordinate;
}

float4 PSWaterFlat(TexturedVertexOutput input) : SV_TARGET
{
	const float2 waterUV = GetWaterTextureCoordinate(input, 0).xy;
	const float2 sparkleUV = GetWaterTextureCoordinate(input, 1).xy;
	const float2 noiseUV = GetWaterTextureCoordinate(input, 2).xy;
	const float2 shroudUV = GetWaterTextureCoordinate(input, 3).xy;
	float4 result = input.color * SampleWaterTexture0(waterUV);
	const float4 sparkle = SampleWaterTexture1(sparkleUV);
	const float4 noise = SampleWaterTexture2(noiseUV);
	const float4 shroud = SampleWaterTexture3(shroudUV);
	result.rgb = result.rgb + sparkle.rgb * noise.rgb;
	result.rgb = result.rgb * shroud.rgb;
	return ApplyLegacyPixelState(result, input.fogDepth);
}

float4 PSWaterRiver(TexturedVertexOutput input) : SV_TARGET
{
	const float2 waterUV = GetWaterTextureCoordinate(input, 0).xy;
	const float2 sparkleUV = GetWaterTextureCoordinate(input, 1).xy;
	const float2 noiseUV = GetWaterTextureCoordinate(input, 2).xy;
	const float2 edgeUV = GetWaterTextureCoordinate(input, 3).xy;
	const float4 baseTexture = SampleWaterTexture0(waterUV);
	const float4 base = input.color * baseTexture;
	const float4 sparkle = SampleWaterTexture1(sparkleUV);
	const float4 noise = SampleWaterTexture2(noiseUV);
	const float4 edge = SampleWaterTexture3(edgeUV);
	float4 result = base;
	result.rgb = result.rgb + sparkle.rgb * noise.rgb * input.color.a;
	result.rgb = result.rgb + edge.rgb * input.color.a;
	// The D3D8 river program overwrites r0.a with texture0.a before applying
	// the edge alpha; vertex diffuse alpha only scales the added RGB detail.
	result.a = baseTexture.a * edge.a;
	return ApplyLegacyPixelState(result, input.fogDepth);
}

float4 PSSeaWave(SeaWaveVertexOutput input) : SV_TARGET
{
	// R8G8_SNORM is the D3D11 representation of D3DFMT_V8U8.  Sampling it
	// already returns the signed [-1, 1] bump vector; applying an additional
	// unsigned-byte remap would invert the legacy wave amplitude.
	const float2 signedBump = LegacyTexture0.Sample(LegacySampler0,
		input.bumpCoordinate).rg;
	const float2 matrixOffset = float2(
		dot(signedBump, TextureBumpParameters0[1].xy),
		dot(signedBump, TextureBumpParameters0[1].zw));
	const float2 reflectionOffset =
		matrixOffset * TextureBumpParameters1[1].x +
		TextureBumpParameters1[1].y;
	const float4 reflection = LegacyTexture1.Sample(LegacySampler1,
		input.reflectionCoordinate + reflectionOffset);
	return ApplyLegacyPixelState(reflection * input.diffuse, input.fogDepth);
}
