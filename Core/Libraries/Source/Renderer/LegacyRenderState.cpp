#include "Renderer/LegacyRenderState.h"

namespace rts
{
namespace render
{
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
	mipLodBias(0.0f), borderColor()
{
}

LegacyTextureStageState::LegacyTextureStageState() :
	colorOperation(RENDER_TEXTURE_OP_DISABLE),
	colorArgument1(RENDER_TEXTURE_ARG_TEXTURE),
	colorArgument2(RENDER_TEXTURE_ARG_DIFFUSE),
	alphaOperation(RENDER_TEXTURE_OP_DISABLE),
	alphaArgument1(RENDER_TEXTURE_ARG_TEXTURE),
	alphaArgument2(RENDER_TEXTURE_ARG_DIFFUSE), textureCoordinateIndex(0),
	projectedCoordinates(false), sampler()
{
}

LegacyPipelineState::LegacyPipelineState() :
	shaderBits(0), blend(), depthStencil(), rasterizer(),
	fogMode(RENDER_FOG_DISABLED), lightingEnable(false),
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

LegacyLogicalState::LegacyLogicalState() : pipeline(), constants() {}

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
		key.words[3 + stage] =
			static_cast<unsigned int>(textureStage.colorOperation) |
			(static_cast<unsigned int>(textureStage.colorArgument1) << 4) |
			(static_cast<unsigned int>(textureStage.colorArgument2) << 7) |
			(static_cast<unsigned int>(textureStage.alphaOperation) << 10) |
			(static_cast<unsigned int>(textureStage.alphaArgument1) << 14) |
			(static_cast<unsigned int>(textureStage.alphaArgument2) << 17) |
			((textureStage.textureCoordinateIndex & 0x0fU) << 20) |
			(static_cast<unsigned int>(textureStage.projectedCoordinates) << 24);
	}
	return key;
}
}
}
