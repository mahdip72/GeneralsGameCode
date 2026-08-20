#include "Renderer/LegacyRenderState.h"

namespace rts
{
namespace render
{
namespace
{
LegacyLogicalState g_trackedLogicalState;
bool g_trackedPipelineStateValid = false;
}

RenderFloat4::RenderFloat4() : x(0.0f), y(0.0f), z(0.0f), w(0.0f) {}

RenderFloat4::RenderFloat4(float xValue, float yValue, float zValue,
	float wValue) : x(xValue), y(yValue), z(zValue), w(wValue)
{
}

RenderMatrix4::RenderMatrix4()
{
	setIdentity();
}

void RenderMatrix4::setIdentity()
{
	for (unsigned int index = 0; index < 16; ++index)
	{
		values[index] = 0.0f;
	}
	values[0] = 1.0f;
	values[5] = 1.0f;
	values[10] = 1.0f;
	values[15] = 1.0f;
}

LegacyBlendState::LegacyBlendState() :
	blendEnable(false), sourceColor(RENDER_BLEND_ONE),
	destinationColor(RENDER_BLEND_ZERO), colorOperation(RENDER_BLEND_ADD),
	sourceAlpha(RENDER_BLEND_ONE), destinationAlpha(RENDER_BLEND_ZERO),
	alphaOperation(RENDER_BLEND_ADD), colorWriteMask(0x0fU)
{
}

LegacyDepthStencilState::LegacyDepthStencilState() :
	depthEnable(true), depthWrite(true), depthFunction(RENDER_COMPARE_LESS_EQUAL),
	stencilEnable(false), stencilReadMask(0xffU), stencilWriteMask(0xffU),
	stencilReference(0), stencilFunction(RENDER_COMPARE_ALWAYS),
	stencilFail(RENDER_STENCIL_KEEP), stencilDepthFail(RENDER_STENCIL_KEEP),
	stencilPass(RENDER_STENCIL_KEEP)
{
}

LegacyRasterizerState::LegacyRasterizerState() :
	fillMode(RENDER_FILL_SOLID), cullMode(RENDER_CULL_BACK),
	frontCounterClockwise(false), scissorEnable(false), depthBias(0),
	slopeScaledDepthBias(0.0f)
{
}

LegacySamplerState::LegacySamplerState() :
	addressU(RENDER_TEXTURE_ADDRESS_WRAP),
	addressV(RENDER_TEXTURE_ADDRESS_WRAP),
	addressW(RENDER_TEXTURE_ADDRESS_WRAP),
	minification(RENDER_TEXTURE_FILTER_LINEAR),
	magnification(RENDER_TEXTURE_FILTER_LINEAR),
	mipmapping(RENDER_TEXTURE_FILTER_LINEAR), maximumAnisotropy(1),
	maximumMipLevel(0), mipLodBias(0.0f), borderColor()
{
}

LegacyTextureStageState::LegacyTextureStageState() :
	colorOperation(RENDER_TEXTURE_OP_DISABLE),
	colorArgument0(RENDER_TEXTURE_ARG_CURRENT),
	colorArgument1(RENDER_TEXTURE_ARG_TEXTURE),
	colorArgument2(RENDER_TEXTURE_ARG_DIFFUSE),
	alphaOperation(RENDER_TEXTURE_OP_DISABLE),
	alphaArgument0(RENDER_TEXTURE_ARG_CURRENT),
	alphaArgument1(RENDER_TEXTURE_ARG_TEXTURE),
	alphaArgument2(RENDER_TEXTURE_ARG_DIFFUSE),
	colorArgument0Complement(false), colorArgument0AlphaReplicate(false),
	colorArgument1Complement(false), colorArgument1AlphaReplicate(false),
	colorArgument2Complement(false), colorArgument2AlphaReplicate(false),
	alphaArgument0Complement(false), alphaArgument0AlphaReplicate(false),
	alphaArgument1Complement(false), alphaArgument1AlphaReplicate(false),
	alphaArgument2Complement(false), alphaArgument2AlphaReplicate(false),
	resultArgument(RENDER_TEXTURE_ARG_CURRENT), textureCoordinateIndex(0),
	projectedCoordinates(false), bumpEnvironmentMatrix00(1.0f),
	bumpEnvironmentMatrix01(0.0f), bumpEnvironmentMatrix10(0.0f),
	bumpEnvironmentMatrix11(1.0f), bumpEnvironmentLuminanceScale(1.0f),
	bumpEnvironmentLuminanceOffset(0.0f), sampler()
{
}

LegacyPipelineState::LegacyPipelineState() :
	shaderBits(0), blend(), depthStencil(), rasterizer(),
	fogMode(RENDER_FOG_DISABLED), secondaryGradientEnable(false),
	nPatchEnable(false), lightingEnable(false),
	normalizeNormals(false), alphaTestEnable(false),
	alphaFunction(RENDER_COMPARE_ALWAYS), alphaReference(0), textureFactor(0)
{
	textureStages[0].colorOperation = RENDER_TEXTURE_OP_MODULATE;
	textureStages[0].alphaOperation = RENDER_TEXTURE_OP_MODULATE;
	for (unsigned int stage = 0; stage < LEGACY_TEXTURE_STAGE_COUNT; ++stage)
	{
		textureStages[stage].textureCoordinateIndex = stage;
	}
}

LegacyMaterialState::LegacyMaterialState() :
	diffuse(1.0f, 1.0f, 1.0f, 1.0f),
	ambient(1.0f, 1.0f, 1.0f, 1.0f), specular(), emissive(),
	specularPower(0.0f)
{
}

LegacyLightState::LegacyLightState() :
	enabled(false), type(RENDER_LIGHT_DIRECTIONAL), diffuse(), specular(),
	ambient(), position(0.0f, 0.0f, 0.0f, 1.0f),
	direction(0.0f, 0.0f, 1.0f, 0.0f), range(0.0f), falloff(1.0f),
	attenuation0(1.0f), attenuation1(0.0f), attenuation2(0.0f),
	theta(0.0f), phi(0.0f)
{
}

LegacyFogConstants::LegacyFogConstants() :
	enabled(false), color(), start(0.0f), end(1.0f), density(1.0f)
{
}

LegacyFixedFunctionConstants::LegacyFixedFunctionConstants() :
	world(), view(), projection(), material(), fog(),
	globalAmbient(0.0f, 0.0f, 0.0f, 1.0f)
{
}

LegacyLogicalState::LegacyLogicalState() :
	pipeline(), constants(), texturePresenceMask(0) {}

LegacyShaderKey::LegacyShaderKey()
{
	for (unsigned int index = 0; index < WORD_COUNT; ++index)
	{
		words[index] = 0;
	}
}

bool LegacyShaderKey::operator==(const LegacyShaderKey &other) const
{
	for (unsigned int index = 0; index < WORD_COUNT; ++index)
	{
		if (words[index] != other.words[index])
		{
			return false;
		}
	}
	return true;
}

bool LegacyShaderKey::operator!=(const LegacyShaderKey &other) const
{
	return !(*this == other);
}

LegacyShaderKey BuildLegacyShaderKey(const LegacyPipelineState &state,
	unsigned int vertexFormat, unsigned int texturePresenceMask)
{
	LegacyShaderKey key;
	key.words[0] = state.shaderBits;
	key.words[1] = vertexFormat;
	key.words[2] = (texturePresenceMask & 0xffU) |
		(static_cast<unsigned int>(state.fogMode) << 8) |
		(static_cast<unsigned int>(state.lightingEnable) << 12) |
		(static_cast<unsigned int>(state.normalizeNormals) << 13) |
		(static_cast<unsigned int>(state.alphaTestEnable) << 14) |
		(static_cast<unsigned int>(state.alphaFunction) << 15);

	for (unsigned int stage = 0; stage < LEGACY_TEXTURE_STAGE_COUNT; ++stage)
	{
		const LegacyTextureStageState &textureStage = state.textureStages[stage];
		const unsigned int firstWord = 3 + (stage * 2);
		key.words[firstWord] =
			static_cast<unsigned int>(textureStage.colorOperation) |
			(static_cast<unsigned int>(textureStage.colorArgument0) << 5) |
			(static_cast<unsigned int>(textureStage.colorArgument1) << 8) |
			(static_cast<unsigned int>(textureStage.colorArgument2) << 11) |
			(static_cast<unsigned int>(textureStage.alphaOperation) << 14) |
			(static_cast<unsigned int>(textureStage.alphaArgument0) << 19) |
			(static_cast<unsigned int>(textureStage.alphaArgument1) << 22) |
			(static_cast<unsigned int>(textureStage.alphaArgument2) << 25) |
			((textureStage.textureCoordinateIndex & 0x0fU) << 28);
		key.words[firstWord + 1] =
			static_cast<unsigned int>(textureStage.projectedCoordinates) |
			(static_cast<unsigned int>(textureStage.colorArgument0Complement) << 1) |
			(static_cast<unsigned int>(textureStage.colorArgument0AlphaReplicate) << 2) |
			(static_cast<unsigned int>(textureStage.colorArgument1Complement) << 3) |
			(static_cast<unsigned int>(textureStage.colorArgument1AlphaReplicate) << 4) |
			(static_cast<unsigned int>(textureStage.colorArgument2Complement) << 5) |
			(static_cast<unsigned int>(textureStage.colorArgument2AlphaReplicate) << 6) |
			(static_cast<unsigned int>(textureStage.alphaArgument0Complement) << 7) |
			(static_cast<unsigned int>(textureStage.alphaArgument0AlphaReplicate) << 8) |
			(static_cast<unsigned int>(textureStage.alphaArgument1Complement) << 9) |
			(static_cast<unsigned int>(textureStage.alphaArgument1AlphaReplicate) << 10) |
			(static_cast<unsigned int>(textureStage.alphaArgument2Complement) << 11) |
			(static_cast<unsigned int>(textureStage.alphaArgument2AlphaReplicate) << 12) |
			(static_cast<unsigned int>(textureStage.resultArgument) << 13);
	}
	key.words[2] |= static_cast<unsigned int>(state.secondaryGradientEnable) << 22;
	key.words[2] |= static_cast<unsigned int>(state.nPatchEnable) << 23;
	return key;
}

namespace
{
unsigned int legacyField(unsigned int bits, unsigned int shift,
	unsigned int mask)
{
	return (bits >> shift) & mask;
}

void setTextureOperation(LegacyTextureStageState *stage,
	RenderTextureOperation colorOperation, RenderTextureArgument colorArgument1,
	RenderTextureArgument colorArgument2,
	RenderTextureOperation alphaOperation, RenderTextureArgument alphaArgument1,
	RenderTextureArgument alphaArgument2)
{
	stage->colorOperation = colorOperation;
	stage->colorArgument1 = colorArgument1;
	stage->colorArgument2 = colorArgument2;
	stage->alphaOperation = alphaOperation;
	stage->alphaArgument1 = alphaArgument1;
	stage->alphaArgument2 = alphaArgument2;
}
}

bool DecodeLegacyShaderBits(unsigned int shaderBits,
	LegacyPipelineState *state)
{
	if (state == 0)
	{
		return false;
	}

	const unsigned int depthCompare = legacyField(shaderBits, 0, 7);
	const unsigned int depthWrite = legacyField(shaderBits, 3, 1);
	const unsigned int colorWrite = legacyField(shaderBits, 4, 1);
	const unsigned int destinationBlend = legacyField(shaderBits, 5, 7);
	const unsigned int fog = legacyField(shaderBits, 8, 3);
	const unsigned int primaryGradient = legacyField(shaderBits, 10, 7);
	const unsigned int secondaryGradient = legacyField(shaderBits, 13, 1);
	const unsigned int sourceBlend = legacyField(shaderBits, 14, 3);
	const unsigned int texturing = legacyField(shaderBits, 16, 1);
	const unsigned int nPatch = legacyField(shaderBits, 17, 1);
	const unsigned int alphaTest = legacyField(shaderBits, 18, 1);
	const unsigned int cull = legacyField(shaderBits, 19, 1);
	const unsigned int detailColor = legacyField(shaderBits, 20, 15);
	const unsigned int detailAlpha = legacyField(shaderBits, 24, 7);
	if (destinationBlend > 5 || primaryGradient > 5 ||
		detailColor > 12 || detailAlpha > 3)
	{
		*state = LegacyPipelineState();
		return false;
	}

	*state = LegacyPipelineState();
	state->shaderBits = shaderBits;
	state->depthStencil.depthFunction =
		static_cast<RenderCompareFunction>(depthCompare);
	state->depthStencil.depthWrite = depthWrite != 0;
	state->rasterizer.cullMode = cull != 0 ? RENDER_CULL_BACK : RENDER_CULL_NONE;
	state->secondaryGradientEnable = secondaryGradient != 0;
	state->nPatchEnable = nPatch != 0;

	static const RenderBlendFactor sourceBlendFactors[4] = {
		RENDER_BLEND_ZERO, RENDER_BLEND_ONE, RENDER_BLEND_SOURCE_ALPHA,
		RENDER_BLEND_DESTINATION_COLOR
	};
	static const RenderBlendFactor destinationBlendFactors[6] = {
		RENDER_BLEND_ZERO, RENDER_BLEND_ONE, RENDER_BLEND_SOURCE_COLOR,
		RENDER_BLEND_INVERSE_SOURCE_COLOR, RENDER_BLEND_SOURCE_ALPHA,
		RENDER_BLEND_INVERSE_SOURCE_ALPHA
	};
	state->blend.sourceColor = sourceBlendFactors[sourceBlend];
	state->blend.destinationColor = destinationBlendFactors[destinationBlend];
	if (colorWrite == 0)
	{
		state->blend.sourceColor = RENDER_BLEND_ZERO;
		state->blend.destinationColor = RENDER_BLEND_ONE;
		state->blend.colorWriteMask = 0;
	}
	state->blend.sourceAlpha = state->blend.sourceColor;
	state->blend.destinationAlpha = state->blend.destinationColor;
	state->blend.blendEnable = state->blend.sourceColor != RENDER_BLEND_ONE ||
		state->blend.destinationColor != RENDER_BLEND_ZERO;

	state->alphaTestEnable = alphaTest != 0;
	if (state->alphaTestEnable)
	{
		state->alphaReference = 0x60U;
		state->alphaFunction = RENDER_COMPARE_GREATER_EQUAL;
	}

	switch (fog)
	{
	case 1: state->fogMode = RENDER_FOG_LINEAR; break;
	case 2: state->fogMode = RENDER_FOG_SCALE_FRAGMENT; break;
	case 3: state->fogMode = RENDER_FOG_WHITE; break;
	default: state->fogMode = RENDER_FOG_DISABLED; break;
	}

	LegacyTextureStageState *primary = &state->textureStages[0];
	if (texturing != 0)
	{
		switch (primaryGradient)
		{
		case 0:
			setTextureOperation(primary, RENDER_TEXTURE_OP_SELECT_ARGUMENT_1,
				RENDER_TEXTURE_ARG_TEXTURE, RENDER_TEXTURE_ARG_CURRENT,
				RENDER_TEXTURE_OP_SELECT_ARGUMENT_1,
				RENDER_TEXTURE_ARG_TEXTURE, RENDER_TEXTURE_ARG_CURRENT);
			break;
		case 2:
			setTextureOperation(primary, RENDER_TEXTURE_OP_ADD,
				RENDER_TEXTURE_ARG_TEXTURE, RENDER_TEXTURE_ARG_DIFFUSE,
				RENDER_TEXTURE_OP_MODULATE,
				RENDER_TEXTURE_ARG_TEXTURE, RENDER_TEXTURE_ARG_DIFFUSE);
			break;
		case 3:
			setTextureOperation(primary, RENDER_TEXTURE_OP_BUMP_ENVIRONMENT,
				RENDER_TEXTURE_ARG_TEXTURE, RENDER_TEXTURE_ARG_DIFFUSE,
				RENDER_TEXTURE_OP_DISABLE,
				RENDER_TEXTURE_ARG_TEXTURE, RENDER_TEXTURE_ARG_CURRENT);
			break;
		case 4:
			setTextureOperation(primary,
				RENDER_TEXTURE_OP_BUMP_ENVIRONMENT_LUMINANCE,
				RENDER_TEXTURE_ARG_TEXTURE, RENDER_TEXTURE_ARG_DIFFUSE,
				RENDER_TEXTURE_OP_DISABLE,
				RENDER_TEXTURE_ARG_TEXTURE, RENDER_TEXTURE_ARG_CURRENT);
			break;
		case 5:
			setTextureOperation(primary, RENDER_TEXTURE_OP_MODULATE_2X,
				RENDER_TEXTURE_ARG_TEXTURE, RENDER_TEXTURE_ARG_DIFFUSE,
				RENDER_TEXTURE_OP_MODULATE,
				RENDER_TEXTURE_ARG_TEXTURE, RENDER_TEXTURE_ARG_DIFFUSE);
			break;
		default:
			setTextureOperation(primary, RENDER_TEXTURE_OP_MODULATE,
				RENDER_TEXTURE_ARG_TEXTURE, RENDER_TEXTURE_ARG_DIFFUSE,
				RENDER_TEXTURE_OP_MODULATE,
				RENDER_TEXTURE_ARG_TEXTURE, RENDER_TEXTURE_ARG_DIFFUSE);
			break;
		}
	}
	else if (primaryGradient == 0)
	{
		setTextureOperation(primary, RENDER_TEXTURE_OP_DISABLE,
			RENDER_TEXTURE_ARG_TEXTURE, RENDER_TEXTURE_ARG_CURRENT,
			RENDER_TEXTURE_OP_DISABLE,
			RENDER_TEXTURE_ARG_TEXTURE, RENDER_TEXTURE_ARG_CURRENT);
	}
	else
	{
		setTextureOperation(primary, RENDER_TEXTURE_OP_SELECT_ARGUMENT_2,
			RENDER_TEXTURE_ARG_TEXTURE, RENDER_TEXTURE_ARG_DIFFUSE,
			RENDER_TEXTURE_OP_SELECT_ARGUMENT_2,
			RENDER_TEXTURE_ARG_TEXTURE, RENDER_TEXTURE_ARG_DIFFUSE);
	}

	LegacyTextureStageState *detail = &state->textureStages[1];
	if (texturing != 0)
	{
		static const RenderTextureOperation detailColorOperations[13] = {
			RENDER_TEXTURE_OP_DISABLE, RENDER_TEXTURE_OP_SELECT_ARGUMENT_1,
			RENDER_TEXTURE_OP_MODULATE, RENDER_TEXTURE_OP_ADD_SMOOTH,
			RENDER_TEXTURE_OP_ADD, RENDER_TEXTURE_OP_SUBTRACT,
			RENDER_TEXTURE_OP_SUBTRACT, RENDER_TEXTURE_OP_BLEND_TEXTURE_ALPHA,
			RENDER_TEXTURE_OP_BLEND_CURRENT_ALPHA, RENDER_TEXTURE_OP_ADD_SIGNED,
			RENDER_TEXTURE_OP_ADD_SIGNED_2X, RENDER_TEXTURE_OP_MODULATE_2X,
			RENDER_TEXTURE_OP_MODULATE_ALPHA_ADD_COLOR
		};
		static const RenderTextureOperation detailAlphaOperations[4] = {
			RENDER_TEXTURE_OP_DISABLE, RENDER_TEXTURE_OP_SELECT_ARGUMENT_1,
			RENDER_TEXTURE_OP_MODULATE, RENDER_TEXTURE_OP_ADD_SMOOTH
		};
		detail->colorOperation = detailColorOperations[detailColor];
		detail->colorArgument1 = detailColor == 6 ?
			RENDER_TEXTURE_ARG_CURRENT : RENDER_TEXTURE_ARG_TEXTURE;
		detail->colorArgument2 = detailColor == 6 ?
			RENDER_TEXTURE_ARG_TEXTURE : RENDER_TEXTURE_ARG_CURRENT;
		if (detailColor == 12)
		{
			detail->colorArgument1 = RENDER_TEXTURE_ARG_CURRENT;
			detail->colorArgument2 = RENDER_TEXTURE_ARG_TEXTURE;
		}
		detail->alphaOperation = detailAlphaOperations[detailAlpha];
		detail->alphaArgument1 = RENDER_TEXTURE_ARG_TEXTURE;
		detail->alphaArgument2 = RENDER_TEXTURE_ARG_CURRENT;
		if (detail->colorOperation != RENDER_TEXTURE_OP_DISABLE &&
			detail->alphaOperation == RENDER_TEXTURE_OP_DISABLE)
		{
			detail->alphaOperation = RENDER_TEXTURE_OP_SELECT_ARGUMENT_2;
		}
		else if (detail->colorOperation == RENDER_TEXTURE_OP_DISABLE &&
			detail->alphaOperation != RENDER_TEXTURE_OP_DISABLE)
		{
			detail->colorOperation = RENDER_TEXTURE_OP_SELECT_ARGUMENT_2;
		}
	}
	return true;
}

void TrackLegacyShaderBits(unsigned int shaderBits)
{
	LegacyPipelineState decoded;
	g_trackedPipelineStateValid = DecodeLegacyShaderBits(shaderBits, &decoded);
	if (!g_trackedPipelineStateValid)
	{
		return;
	}
	for (unsigned int index = 0; index < LEGACY_TEXTURE_STAGE_COUNT; ++index)
	{
		decoded.textureStages[index] =
			g_trackedLogicalState.pipeline.textureStages[index];
	}
	g_trackedLogicalState.pipeline = decoded;
}

bool GetTrackedLegacyPipelineState(LegacyPipelineState *state)
{
	if (state == 0 || !g_trackedPipelineStateValid)
	{
		return false;
	}
	*state = g_trackedLogicalState.pipeline;
	return true;
}

bool TrackLegacyTransform(LegacyTransformSlot slot, const float *values)
{
	if (values == 0 || slot < LEGACY_TRANSFORM_WORLD ||
		slot >= LEGACY_TRANSFORM_COUNT)
	{
		return false;
	}
	RenderMatrix4 *target = 0;
	if (slot == LEGACY_TRANSFORM_WORLD)
	{
		target = &g_trackedLogicalState.constants.world;
	}
	else if (slot == LEGACY_TRANSFORM_VIEW)
	{
		target = &g_trackedLogicalState.constants.view;
	}
	else if (slot == LEGACY_TRANSFORM_PROJECTION)
	{
		target = &g_trackedLogicalState.constants.projection;
	}
	else
	{
		target = &g_trackedLogicalState.constants.textureTransforms[
			static_cast<unsigned int>(slot) - LEGACY_TRANSFORM_TEXTURE0];
	}
	for (unsigned int index = 0; index < 16; ++index)
	{
		target->values[index] = values[index];
	}
	return true;
}

void TrackLegacyMaterial(const LegacyMaterialState &material)
{
	g_trackedLogicalState.constants.material = material;
}

bool TrackLegacyLight(unsigned int index, const LegacyLightState &light)
{
	if (index >= LEGACY_LIGHT_COUNT)
	{
		return false;
	}
	g_trackedLogicalState.constants.lights[index] = light;
	return true;
}

bool TrackLegacyTextureStage(unsigned int index,
	const LegacyTextureStageState &textureStage)
{
	if (index >= LEGACY_TEXTURE_STAGE_COUNT)
	{
		return false;
	}
	g_trackedLogicalState.pipeline.textureStages[index] = textureStage;
	return true;
}

bool GetTrackedLegacyTextureStage(unsigned int index,
	LegacyTextureStageState *textureStage)
{
	if (index >= LEGACY_TEXTURE_STAGE_COUNT || textureStage == 0)
	{
		return false;
	}
	*textureStage = g_trackedLogicalState.pipeline.textureStages[index];
	return true;
}

bool TrackLegacyTexturePresence(unsigned int index, bool present)
{
	if (index >= LEGACY_TEXTURE_STAGE_COUNT)
	{
		return false;
	}
	const unsigned int bit = 1U << index;
	if (present)
	{
		g_trackedLogicalState.texturePresenceMask |= bit;
	}
	else
	{
		g_trackedLogicalState.texturePresenceMask &= ~bit;
	}
	return true;
}

void TrackLegacyFog(const LegacyFogConstants &fog)
{
	g_trackedLogicalState.constants.fog = fog;
}

void TrackLegacyGlobalAmbient(const RenderFloat4 &ambient)
{
	g_trackedLogicalState.constants.globalAmbient = ambient;
}

bool GetTrackedLegacyLogicalState(LegacyLogicalState *state)
{
	if (state == 0 || !g_trackedPipelineStateValid)
	{
		return false;
	}
	*state = g_trackedLogicalState;
	return true;
}
}
}
