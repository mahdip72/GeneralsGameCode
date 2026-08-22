#ifndef RTS_RENDERER_LEGACYRENDERSTATE_H
#define RTS_RENDERER_LEGACYRENDERSTATE_H

namespace rts
{
namespace render
{
const unsigned int LEGACY_TEXTURE_STAGE_COUNT = 8;
const unsigned int LEGACY_LIGHT_COUNT = 4;
const unsigned int LEGACY_CLIP_PLANE_COUNT = 6;
const unsigned int LEGACY_VERTEX_CONSTANT_COUNT = 34;
const unsigned int LEGACY_PIXEL_CONSTANT_COUNT = 8;

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

// D3DMCS_MATERIAL/COLOR1/COLOR2 are deliberately represented by the same
// ordinal values.  The neutral renderer does not include the D3D8 headers,
// but the DX8 bridge can therefore publish the values without lossy remapping.
enum RenderMaterialSource
{
	RENDER_MATERIAL_SOURCE_MATERIAL,
	RENDER_MATERIAL_SOURCE_COLOR1,
	RENDER_MATERIAL_SOURCE_COLOR2
};

enum RenderLegacyPixelProgram
{
	RENDER_LEGACY_PIXEL_FIXED_FUNCTION,
	RENDER_LEGACY_PIXEL_WATER_FLAT,
	RENDER_LEGACY_PIXEL_WATER_RIVER,
	RENDER_LEGACY_PIXEL_TERRAIN_BASE,
	RENDER_LEGACY_PIXEL_TERRAIN_NOISE,
	RENDER_LEGACY_PIXEL_TERRAIN_NOISE2,
	RENDER_LEGACY_PIXEL_ROAD_NOISE2,
	RENDER_LEGACY_PIXEL_FLAT_TERRAIN_BASE0,
	RENDER_LEGACY_PIXEL_FLAT_TERRAIN_BASE,
	RENDER_LEGACY_PIXEL_FLAT_TERRAIN_NOISE,
	RENDER_LEGACY_PIXEL_FLAT_TERRAIN_NOISE2,
	RENDER_LEGACY_PIXEL_MONOCHROME,
	// The legacy GeForce3 sea path uses a dedicated wave vertex/pixel pair. Keep
	// this appended so the existing program values embedded in shader code do
	// not change while the migration is in progress.
	RENDER_LEGACY_PIXEL_WATER_SEA,
	// The profiler's D3D8 ps_1_4 shader swaps the sampled B/R channels. Keep
	// this appended as well so all existing program values remain stable.
	RENDER_LEGACY_PIXEL_PROFILER_SWIZZLE
};

enum RenderLegacyVertexProgram
{
	RENDER_LEGACY_VERTEX_FIXED_FUNCTION,
	RENDER_LEGACY_VERTEX_TREES,
	RENDER_LEGACY_VERTEX_WATER_SEA
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
	RENDER_TEXTURE_OP_BUMP_ENVIRONMENT_LUMINANCE,
	RENDER_TEXTURE_OP_BLEND_TEXTURE_ALPHA_PREMULTIPLIED,
	RENDER_TEXTURE_OP_BLEND_TEXTURE_FACTOR_ALPHA,
	RENDER_TEXTURE_OP_PREMODULATE,
	RENDER_TEXTURE_OP_MODULATE_COLOR_ADD_ALPHA,
	RENDER_TEXTURE_OP_MODULATE_INVERSE_ALPHA_ADD_COLOR,
	RENDER_TEXTURE_OP_MODULATE_INVERSE_COLOR_ADD_ALPHA,
	RENDER_TEXTURE_OP_MULTIPLY_ADD,
	RENDER_TEXTURE_OP_LINEAR_INTERPOLATE
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
	RENDER_TEXTURE_FILTER_NONE,
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
	unsigned int maximumMipLevel;
	float mipLodBias;
	RenderFloat4 borderColor;
};

struct LegacyTextureStageState
{
	LegacyTextureStageState();

	RenderTextureOperation colorOperation;
	RenderTextureArgument colorArgument0;
	RenderTextureArgument colorArgument1;
	RenderTextureArgument colorArgument2;
	RenderTextureOperation alphaOperation;
	RenderTextureArgument alphaArgument0;
	RenderTextureArgument alphaArgument1;
	RenderTextureArgument alphaArgument2;
	bool colorArgument0Complement;
	bool colorArgument0AlphaReplicate;
	bool colorArgument1Complement;
	bool colorArgument1AlphaReplicate;
	bool colorArgument2Complement;
	bool colorArgument2AlphaReplicate;
	bool alphaArgument0Complement;
	bool alphaArgument0AlphaReplicate;
	bool alphaArgument1Complement;
	bool alphaArgument1AlphaReplicate;
	bool alphaArgument2Complement;
	bool alphaArgument2AlphaReplicate;
	RenderTextureArgument resultArgument;
	unsigned int textureCoordinateIndex;
	bool cameraSpacePosition;
	bool cameraSpaceNormal;
	bool cameraSpaceReflectionVector;
	bool textureTransformEnable;
	bool projectedCoordinates;
	// D3DTTFF_COUNT1..COUNT4 are values, not independent flags.  Keep the
	// selected component count in the neutral state so the D3D11 sampler can
	// reproduce D3D8's projected-coordinate denominator (Y, Z, or W).
	unsigned int textureTransformCount;
	float bumpEnvironmentMatrix00;
	float bumpEnvironmentMatrix01;
	float bumpEnvironmentMatrix10;
	float bumpEnvironmentMatrix11;
	float bumpEnvironmentLuminanceScale;
	float bumpEnvironmentLuminanceOffset;
	LegacySamplerState sampler;
};

struct LegacyPipelineState
{
	LegacyPipelineState();

	unsigned int shaderBits;
	RenderLegacyPixelProgram pixelProgram;
	RenderLegacyVertexProgram vertexProgram;
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
	unsigned int clipPlaneEnableMask;
	RenderMaterialSource ambientMaterialSource;
	RenderMaterialSource diffuseMaterialSource;
	RenderMaterialSource emissiveMaterialSource;
	RenderMaterialSource specularMaterialSource;
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

// The vertex-material portion of the old fixed-function API is kept as a
// renderer-neutral value object.  The compatibility wrapper owns conversion
// to the legacy backend ABI, so game/runtime code does not need to mention
// backend descriptor types or state constants.
struct LegacyVertexMaterialState
{
	LegacyVertexMaterialState();

	LegacyMaterialState material;
	bool lightingEnable;
	RenderMaterialSource ambientMaterialSource;
	RenderMaterialSource diffuseMaterialSource;
	RenderMaterialSource emissiveMaterialSource;
	unsigned int textureCoordinateIndex[LEGACY_TEXTURE_STAGE_COUNT];
	// Bit i requests the default coordinate/transform state for stage i.
	// Mapper implementations overwrite their own stage after this baseline.
	unsigned int textureStageResetMask;
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
	RenderFloat4 clipPlanes[LEGACY_CLIP_PLANE_COUNT];
	RenderFloat4 vertexShaderConstants[LEGACY_VERTEX_CONSTANT_COUNT];
	RenderFloat4 pixelShaderConstants[LEGACY_PIXEL_CONSTANT_COUNT];
};

struct LegacyLogicalState
{
	LegacyLogicalState();

	LegacyPipelineState pipeline;
	LegacyFixedFunctionConstants constants;
	unsigned int texturePresenceMask;
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
	enum { WORD_COUNT = 20 };

	LegacyShaderKey();

	bool operator==(const LegacyShaderKey &other) const;
	bool operator!=(const LegacyShaderKey &other) const;

	unsigned int words[WORD_COUNT];
};

LegacyShaderKey BuildLegacyShaderKey(const LegacyPipelineState &state,
	unsigned int vertexFormat, unsigned int texturePresenceMask);
bool DecodeLegacyShaderBits(unsigned int shaderBits,
	LegacyPipelineState *state);
void ResetTrackedLegacyState();
// Re-enable the neutral pipeline after a deliberate state reset.  Callers
// use this before publishing ordinary render states that precede the first
// shader in a frame.
void SeedTrackedLegacyPipelineState();
// A failed legacy-to-neutral state conversion poisons the current frame.  The
// D3D11 bridge must reject the next draw instead of reusing the prior state;
// the owner clears this latch at the start of the next frame.
void ResetLegacyStatePublicationFailure();
void MarkLegacyStatePublicationFailure();
bool HasLegacyStatePublicationFailure();
void TrackLegacyShaderBits(unsigned int shaderBits);
void TrackLegacyPixelProgram(RenderLegacyPixelProgram program);
void TrackLegacyVertexProgram(RenderLegacyVertexProgram program);
void TrackLegacyCullState(bool enabled, bool frontCounterClockwise);
void TrackLegacyPipelineState(const LegacyPipelineState &state);
bool GetTrackedLegacyPipelineState(LegacyPipelineState *state);
bool TrackLegacyTransform(LegacyTransformSlot slot, const float *values);
bool TrackLegacyVertexShaderConstants(unsigned int startRegister,
	const float *values, unsigned int registerCount);
bool TrackLegacyPixelShaderConstants(unsigned int startRegister,
	const float *values, unsigned int registerCount);
void TrackLegacyMaterial(const LegacyMaterialState &material);
bool TrackLegacyLight(unsigned int index, const LegacyLightState &light);
bool TrackLegacyTextureStage(unsigned int index,
	const LegacyTextureStageState &textureStage);
bool GetTrackedLegacyTextureStage(unsigned int index,
	LegacyTextureStageState *textureStage);
bool TrackLegacyTexturePresence(unsigned int index, bool present);
void TrackLegacyFog(const LegacyFogConstants &fog);
bool TrackLegacyClipPlane(unsigned int index, const float *plane);
RenderFloat4 DecodeLegacyD3D8Ambient(unsigned int color);
void TrackLegacyGlobalAmbient(const RenderFloat4 &ambient);
bool GetTrackedLegacyLogicalState(LegacyLogicalState *state);

// Shared contract helpers for the fixed-function texture-transform and
// camera-space-normal paths.  These remain C++98-compatible and are also used
// by the offscreen renderer contract tests.
bool IsLegacyTextureTransformCountValid(unsigned int count);
bool IsLegacyProjectedTextureTransformValid(unsigned int count,
	bool projected);
float LegacyProjectedTextureDenominator(unsigned int count,
	const RenderFloat4 &coordinate);
// Writes a row-major 3x3 inverse-transpose matrix into three float4 rows.
// Singular transforms are zero-filled and return false so shader consumers
// can retain the safe default-normal fallback without doing per-vertex matrix
// inversion.
bool BuildLegacyInverseTransposeNormalMatrix(const RenderMatrix4 &transform,
	float *normalMatrix);
RenderFloat4 TransformLegacyCameraNormal(const RenderMatrix4 &worldView,
	const RenderFloat4 &objectNormal, bool hasNormal, bool preTransformed,
	bool normalizeNormal);
}
}

#endif
