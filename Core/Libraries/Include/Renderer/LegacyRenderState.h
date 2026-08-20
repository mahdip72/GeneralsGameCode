#ifndef RTS_RENDERER_LEGACYRENDERSTATE_H
#define RTS_RENDERER_LEGACYRENDERSTATE_H

namespace rts
{
namespace render
{
const unsigned int LEGACY_TEXTURE_STAGE_COUNT = 8;
const unsigned int LEGACY_LIGHT_COUNT = 4;

struct RenderFloat4
{
	RenderFloat4();
	RenderFloat4(float xValue, float yValue, float zValue, float wValue);

	float x;
	float y;
	float z;
	float w;
};

struct RenderMatrix4
{
	RenderMatrix4();
	void setIdentity();

	float values[16];
};

enum RenderCompareFunction
{
	RENDER_COMPARE_NEVER,
	RENDER_COMPARE_LESS,
	RENDER_COMPARE_EQUAL,
	RENDER_COMPARE_LESS_EQUAL,
	RENDER_COMPARE_GREATER,
	RENDER_COMPARE_NOT_EQUAL,
	RENDER_COMPARE_GREATER_EQUAL,
	RENDER_COMPARE_ALWAYS
};

enum RenderBlendFactor
{
	RENDER_BLEND_ZERO,
	RENDER_BLEND_ONE,
	RENDER_BLEND_SOURCE_COLOR,
	RENDER_BLEND_INVERSE_SOURCE_COLOR,
	RENDER_BLEND_SOURCE_ALPHA,
	RENDER_BLEND_INVERSE_SOURCE_ALPHA,
	RENDER_BLEND_DESTINATION_ALPHA,
	RENDER_BLEND_INVERSE_DESTINATION_ALPHA,
	RENDER_BLEND_DESTINATION_COLOR,
	RENDER_BLEND_INVERSE_DESTINATION_COLOR
};

enum RenderBlendOperation
{
	RENDER_BLEND_ADD,
	RENDER_BLEND_SUBTRACT,
	RENDER_BLEND_REVERSE_SUBTRACT,
	RENDER_BLEND_MINIMUM,
	RENDER_BLEND_MAXIMUM
};

enum RenderStencilOperation
{
	RENDER_STENCIL_KEEP,
	RENDER_STENCIL_ZERO,
	RENDER_STENCIL_REPLACE,
	RENDER_STENCIL_INCREMENT_SATURATE,
	RENDER_STENCIL_DECREMENT_SATURATE,
	RENDER_STENCIL_INVERT,
	RENDER_STENCIL_INCREMENT,
	RENDER_STENCIL_DECREMENT
};

enum RenderCullMode
{
	RENDER_CULL_NONE,
	RENDER_CULL_FRONT,
	RENDER_CULL_BACK
};

enum RenderFillMode
{
	RENDER_FILL_WIREFRAME,
	RENDER_FILL_SOLID
};

enum RenderFogMode
{
	RENDER_FOG_DISABLED,
	RENDER_FOG_LINEAR,
	RENDER_FOG_EXPONENTIAL,
	RENDER_FOG_EXPONENTIAL_SQUARED,
	RENDER_FOG_SCALE_FRAGMENT,
	RENDER_FOG_WHITE
};

enum RenderTextureOperation
{
	RENDER_TEXTURE_OP_DISABLE,
	RENDER_TEXTURE_OP_SELECT_ARGUMENT_1,
	RENDER_TEXTURE_OP_SELECT_ARGUMENT_2,
	RENDER_TEXTURE_OP_MODULATE,
	RENDER_TEXTURE_OP_MODULATE_2X,
	RENDER_TEXTURE_OP_MODULATE_4X,
	RENDER_TEXTURE_OP_ADD,
	RENDER_TEXTURE_OP_ADD_SIGNED,
	RENDER_TEXTURE_OP_ADD_SIGNED_2X,
	RENDER_TEXTURE_OP_SUBTRACT,
	RENDER_TEXTURE_OP_ADD_SMOOTH,
	RENDER_TEXTURE_OP_BLEND_DIFFUSE_ALPHA,
	RENDER_TEXTURE_OP_BLEND_TEXTURE_ALPHA,
	RENDER_TEXTURE_OP_BLEND_CURRENT_ALPHA,
	RENDER_TEXTURE_OP_MODULATE_ALPHA_ADD_COLOR,
	RENDER_TEXTURE_OP_DOT_PRODUCT_3,
	RENDER_TEXTURE_OP_BUMP_ENVIRONMENT,
	RENDER_TEXTURE_OP_BUMP_ENVIRONMENT_LUMINANCE
};

enum RenderTextureArgument
{
	RENDER_TEXTURE_ARG_CURRENT,
	RENDER_TEXTURE_ARG_DIFFUSE,
	RENDER_TEXTURE_ARG_TEXTURE,
	RENDER_TEXTURE_ARG_TEXTURE_FACTOR,
	RENDER_TEXTURE_ARG_SPECULAR,
	RENDER_TEXTURE_ARG_TEMP
};

enum RenderTextureAddressMode
{
	RENDER_TEXTURE_ADDRESS_WRAP,
	RENDER_TEXTURE_ADDRESS_MIRROR,
	RENDER_TEXTURE_ADDRESS_CLAMP,
	RENDER_TEXTURE_ADDRESS_BORDER
};

enum RenderTextureFilter
{
	RENDER_TEXTURE_FILTER_POINT,
	RENDER_TEXTURE_FILTER_LINEAR,
	RENDER_TEXTURE_FILTER_ANISOTROPIC
};

enum RenderLightType
{
	RENDER_LIGHT_DIRECTIONAL,
	RENDER_LIGHT_POINT,
	RENDER_LIGHT_SPOT
};

struct LegacyBlendState
{
	LegacyBlendState();

	bool blendEnable;
	RenderBlendFactor sourceColor;
	RenderBlendFactor destinationColor;
	RenderBlendOperation colorOperation;
	RenderBlendFactor sourceAlpha;
	RenderBlendFactor destinationAlpha;
	RenderBlendOperation alphaOperation;
	unsigned int colorWriteMask;
};

struct LegacyDepthStencilState
{
	LegacyDepthStencilState();

	bool depthEnable;
	bool depthWrite;
	RenderCompareFunction depthFunction;
	bool stencilEnable;
	unsigned int stencilReadMask;
	unsigned int stencilWriteMask;
	unsigned int stencilReference;
	RenderCompareFunction stencilFunction;
	RenderStencilOperation stencilFail;
	RenderStencilOperation stencilDepthFail;
	RenderStencilOperation stencilPass;
};

struct LegacyRasterizerState
{
	LegacyRasterizerState();

	RenderFillMode fillMode;
	RenderCullMode cullMode;
	bool frontCounterClockwise;
	bool scissorEnable;
	int depthBias;
	float slopeScaledDepthBias;
};

struct LegacySamplerState
{
	LegacySamplerState();

	RenderTextureAddressMode addressU;
	RenderTextureAddressMode addressV;
	RenderTextureAddressMode addressW;
	RenderTextureFilter minification;
	RenderTextureFilter magnification;
	RenderTextureFilter mipmapping;
	unsigned int maximumAnisotropy;
	float mipLodBias;
	RenderFloat4 borderColor;
};

struct LegacyTextureStageState
{
	LegacyTextureStageState();

	RenderTextureOperation colorOperation;
	RenderTextureArgument colorArgument1;
	RenderTextureArgument colorArgument2;
	RenderTextureOperation alphaOperation;
	RenderTextureArgument alphaArgument1;
	RenderTextureArgument alphaArgument2;
	unsigned int textureCoordinateIndex;
	bool projectedCoordinates;
	LegacySamplerState sampler;
};

struct LegacyPipelineState
{
	LegacyPipelineState();

	unsigned int shaderBits;
	LegacyBlendState blend;
	LegacyDepthStencilState depthStencil;
	LegacyRasterizerState rasterizer;
	LegacyTextureStageState textureStages[LEGACY_TEXTURE_STAGE_COUNT];
	RenderFogMode fogMode;
	bool secondaryGradientEnable;
	bool nPatchEnable;
	bool lightingEnable;
	bool normalizeNormals;
	bool alphaTestEnable;
	RenderCompareFunction alphaFunction;
	unsigned int alphaReference;
	unsigned int textureFactor;
};

struct LegacyMaterialState
{
	LegacyMaterialState();

	RenderFloat4 diffuse;
	RenderFloat4 ambient;
	RenderFloat4 specular;
	RenderFloat4 emissive;
	float specularPower;
};

struct LegacyLightState
{
	LegacyLightState();

	bool enabled;
	RenderLightType type;
	RenderFloat4 diffuse;
	RenderFloat4 specular;
	RenderFloat4 ambient;
	RenderFloat4 position;
	RenderFloat4 direction;
	float range;
	float falloff;
	float attenuation0;
	float attenuation1;
	float attenuation2;
	float theta;
	float phi;
};

struct LegacyFogConstants
{
	LegacyFogConstants();

	bool enabled;
	RenderFloat4 color;
	float start;
	float end;
	float density;
};

struct LegacyFixedFunctionConstants
{
	LegacyFixedFunctionConstants();

	RenderMatrix4 world;
	RenderMatrix4 view;
	RenderMatrix4 projection;
	RenderMatrix4 textureTransforms[LEGACY_TEXTURE_STAGE_COUNT];
	LegacyMaterialState material;
	LegacyLightState lights[LEGACY_LIGHT_COUNT];
	LegacyFogConstants fog;
	RenderFloat4 globalAmbient;
};

struct LegacyLogicalState
{
	LegacyLogicalState();

	LegacyPipelineState pipeline;
	LegacyFixedFunctionConstants constants;
};

enum LegacyTransformSlot
{
	LEGACY_TRANSFORM_WORLD,
	LEGACY_TRANSFORM_VIEW,
	LEGACY_TRANSFORM_PROJECTION,
	LEGACY_TRANSFORM_TEXTURE0,
	LEGACY_TRANSFORM_TEXTURE1,
	LEGACY_TRANSFORM_TEXTURE2,
	LEGACY_TRANSFORM_TEXTURE3,
	LEGACY_TRANSFORM_TEXTURE4,
	LEGACY_TRANSFORM_TEXTURE5,
	LEGACY_TRANSFORM_TEXTURE6,
	LEGACY_TRANSFORM_TEXTURE7,
	LEGACY_TRANSFORM_COUNT
};

struct LegacyShaderKey
{
	enum { WORD_COUNT = 11 };

	LegacyShaderKey();

	bool operator==(const LegacyShaderKey &other) const;
	bool operator!=(const LegacyShaderKey &other) const;

	unsigned int words[WORD_COUNT];
};

LegacyShaderKey BuildLegacyShaderKey(const LegacyPipelineState &state,
	unsigned int vertexFormat, unsigned int texturePresenceMask);
bool DecodeLegacyShaderBits(unsigned int shaderBits,
	LegacyPipelineState *state);
void TrackLegacyShaderBits(unsigned int shaderBits);
bool GetTrackedLegacyPipelineState(LegacyPipelineState *state);
bool TrackLegacyTransform(LegacyTransformSlot slot, const float *values);
bool GetTrackedLegacyLogicalState(LegacyLogicalState *state);
}
}

#endif
