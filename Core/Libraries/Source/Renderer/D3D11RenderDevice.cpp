#include "Renderer/RendererDevice.h"

#include <windows.h>
#include <d3d11.h>
#include <d3d11sdklayers.h>
#include <dxgi1_2.h>

#include "LegacyFixedFunctionPS.h"
#include "LegacyFixedFunctionVS.h"
#include "LegacyWaterFlatPS.h"
#include "LegacyWaterRiverPS.h"
#include "LegacySeaWaveVS.h"
#include "LegacySeaWavePS.h"
#include "LegacyTerrainBasePS.h"
#include "LegacyTerrainNoisePS.h"
#include "LegacyTerrainNoise2PS.h"
#include "LegacyRoadNoise2PS.h"
#include "LegacyFlatTerrainBase0PS.h"
#include "LegacyFlatTerrainBasePS.h"
#include "LegacyFlatTerrainNoisePS.h"
#include "LegacyFlatTerrainNoise2PS.h"
#include "LegacyTexturedFixed1PS.h"
#include "LegacyTexturedFixed2PS.h"
#include "LegacyTexturedPS.h"
#include "LegacyTexturedVS.h"

#include <float.h>
#include <limits.h>
#include <new>
#include <string.h>
#include <vector>

namespace rts
{
namespace render
{
namespace
{
const unsigned int RESOURCE_CAPACITY = 4096;
const unsigned int STATE_CACHE_CAPACITY = 256;
const unsigned int TRANSFORM_CONSTANT_BUFFER_COUNT = 64;
const unsigned int TERRAIN_PIXEL_PROGRAM_COUNT = 8;
const unsigned int LEGACY_VERTEX_LAYOUT_PRETRANSFORMED = 0x80000000U;
const unsigned int MAX_TEXTURE_REFRESH_SUBRESOURCES = 4096;
const size_t MAX_TEXTURE_REFRESH_BYTES = 256U * 1024U * 1024U;

bool Checked_Multiply(size_t left, size_t right, size_t *result)
{
	if (result == 0 || (left != 0 && right >
		static_cast<size_t>(-1) / left))
	{
		return false;
	}
	*result = left * right;
	return true;
}

enum ResourceKind
{
	RESOURCE_NONE,
	RESOURCE_BUFFER,
	RESOURCE_TEXTURE
};

RenderResult TranslateResult(HRESULT result)
{
	if (SUCCEEDED(result))
	{
		return RENDER_RESULT_OK;
	}
	if (result == E_INVALIDARG)
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	if (result == E_OUTOFMEMORY)
	{
		return RENDER_RESULT_OUT_OF_MEMORY;
	}
	if (result == DXGI_ERROR_UNSUPPORTED)
	{
		return RENDER_RESULT_UNSUPPORTED;
	}
	if (result == DXGI_ERROR_DEVICE_HUNG ||
		result == DXGI_ERROR_DEVICE_REMOVED ||
		result == DXGI_ERROR_DEVICE_RESET)
	{
		return RENDER_RESULT_DEVICE_REMOVED;
	}
	return RENDER_RESULT_FAILED;
}

DXGI_FORMAT TranslateFormat(RenderFormat format)
{
	switch (format)
	{
	case RENDER_FORMAT_R8G8B8A8_UNORM:
		return DXGI_FORMAT_R8G8B8A8_UNORM;
	case RENDER_FORMAT_B8G8R8A8_UNORM:
		return DXGI_FORMAT_B8G8R8A8_UNORM;
	case RENDER_FORMAT_D24_UNORM_S8_UINT:
		return DXGI_FORMAT_D24_UNORM_S8_UINT;
	case RENDER_FORMAT_R16_UINT:
		return DXGI_FORMAT_R16_UINT;
	case RENDER_FORMAT_R32_UINT:
		return DXGI_FORMAT_R32_UINT;
	case RENDER_FORMAT_R8G8_SNORM:
		return DXGI_FORMAT_R8G8_SNORM;
	default:
		return DXGI_FORMAT_UNKNOWN;
	}
}

unsigned int TextureBytesPerPixel(RenderFormat format)
{
	switch (format)
	{
	case RENDER_FORMAT_R8G8B8A8_UNORM:
	case RENDER_FORMAT_B8G8R8A8_UNORM:
	case RENDER_FORMAT_D24_UNORM_S8_UINT:
	case RENDER_FORMAT_R32_UINT:
		return 4;
	case RENDER_FORMAT_R16_UINT:
	case RENDER_FORMAT_R8G8_SNORM:
		return 2;
	default:
		return 0;
	}
}

bool EqualTextureDescriptors(const TextureDescriptor &left,
	const TextureDescriptor &right)
{
	return left.width == right.width && left.height == right.height &&
		left.mipCount == right.mipCount && left.arrayCount == right.arrayCount &&
		left.dimension == right.dimension && left.format == right.format &&
		left.binding == right.binding && left.usage == right.usage;
}

RenderFormat TranslateBackBufferFormat(DXGI_FORMAT format)
{
	switch (format)
	{
	case DXGI_FORMAT_R8G8B8A8_UNORM:
		return RENDER_FORMAT_R8G8B8A8_UNORM;
	case DXGI_FORMAT_B8G8R8A8_UNORM:
		return RENDER_FORMAT_B8G8R8A8_UNORM;
	default:
		return RENDER_FORMAT_UNKNOWN;
	}
}

D3D11_COMPARISON_FUNC TranslateComparison(RenderCompareFunction function)
{
	static const D3D11_COMPARISON_FUNC values[] = {
		D3D11_COMPARISON_NEVER, D3D11_COMPARISON_LESS,
		D3D11_COMPARISON_EQUAL, D3D11_COMPARISON_LESS_EQUAL,
		D3D11_COMPARISON_GREATER, D3D11_COMPARISON_NOT_EQUAL,
		D3D11_COMPARISON_GREATER_EQUAL, D3D11_COMPARISON_ALWAYS
	};
	return values[static_cast<unsigned int>(function) < 8 ? function : 0];
}

D3D11_BLEND TranslateBlend(RenderBlendFactor factor)
{
	static const D3D11_BLEND values[] = {
		D3D11_BLEND_ZERO, D3D11_BLEND_ONE, D3D11_BLEND_SRC_COLOR,
		D3D11_BLEND_INV_SRC_COLOR, D3D11_BLEND_SRC_ALPHA,
		D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_DEST_ALPHA,
		D3D11_BLEND_INV_DEST_ALPHA, D3D11_BLEND_DEST_COLOR,
		D3D11_BLEND_INV_DEST_COLOR
	};
	return values[static_cast<unsigned int>(factor) < 10 ? factor : 0];
}

D3D11_BLEND TranslateAlphaBlend(RenderBlendFactor factor)
{
	switch (factor)
	{
	case RENDER_BLEND_SOURCE_COLOR:
		return D3D11_BLEND_SRC_ALPHA;
	case RENDER_BLEND_INVERSE_SOURCE_COLOR:
		return D3D11_BLEND_INV_SRC_ALPHA;
	case RENDER_BLEND_DESTINATION_COLOR:
		return D3D11_BLEND_DEST_ALPHA;
	case RENDER_BLEND_INVERSE_DESTINATION_COLOR:
		return D3D11_BLEND_INV_DEST_ALPHA;
	default:
		return TranslateBlend(factor);
	}
}

D3D11_BLEND_OP TranslateBlendOperation(RenderBlendOperation operation)
{
	static const D3D11_BLEND_OP values[] = {
		D3D11_BLEND_OP_ADD, D3D11_BLEND_OP_SUBTRACT,
		D3D11_BLEND_OP_REV_SUBTRACT, D3D11_BLEND_OP_MIN,
		D3D11_BLEND_OP_MAX
	};
	return values[static_cast<unsigned int>(operation) < 5 ? operation : 0];
}

D3D11_STENCIL_OP TranslateStencilOperation(RenderStencilOperation operation)
{
	static const D3D11_STENCIL_OP values[] = {
		D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_ZERO,
		D3D11_STENCIL_OP_REPLACE, D3D11_STENCIL_OP_INCR_SAT,
		D3D11_STENCIL_OP_DECR_SAT, D3D11_STENCIL_OP_INVERT,
		D3D11_STENCIL_OP_INCR, D3D11_STENCIL_OP_DECR
	};
	return values[static_cast<unsigned int>(operation) < 8 ? operation : 0];
}

D3D11_TEXTURE_ADDRESS_MODE TranslateAddressMode(RenderTextureAddressMode mode)
{
	static const D3D11_TEXTURE_ADDRESS_MODE values[] = {
		D3D11_TEXTURE_ADDRESS_WRAP, D3D11_TEXTURE_ADDRESS_MIRROR,
		D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_TEXTURE_ADDRESS_BORDER
	};
	return values[static_cast<unsigned int>(mode) < 4 ? mode : 0];
}

D3D11_FILTER TranslateFilter(const LegacySamplerState &sampler)
{
	if (sampler.minification == RENDER_TEXTURE_FILTER_ANISOTROPIC ||
		sampler.magnification == RENDER_TEXTURE_FILTER_ANISOTROPIC ||
		sampler.mipmapping == RENDER_TEXTURE_FILTER_ANISOTROPIC)
	{
		return D3D11_FILTER_ANISOTROPIC;
	}
	const unsigned int bits =
		(sampler.minification == RENDER_TEXTURE_FILTER_LINEAR ? 4U : 0U) |
		(sampler.magnification == RENDER_TEXTURE_FILTER_LINEAR ? 2U : 0U) |
		(sampler.mipmapping == RENDER_TEXTURE_FILTER_LINEAR ? 1U : 0U);
	static const D3D11_FILTER values[] = {
		D3D11_FILTER_MIN_MAG_MIP_POINT,
		D3D11_FILTER_MIN_MAG_POINT_MIP_LINEAR,
		D3D11_FILTER_MIN_POINT_MAG_LINEAR_MIP_POINT,
		D3D11_FILTER_MIN_POINT_MAG_MIP_LINEAR,
		D3D11_FILTER_MIN_LINEAR_MAG_MIP_POINT,
		D3D11_FILTER_MIN_LINEAR_MAG_POINT_MIP_LINEAR,
		D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT,
		D3D11_FILTER_MIN_MAG_MIP_LINEAR
	};
	return values[bits];
}

struct BlendStateEntry
{
	D3D11_BLEND_DESC descriptor;
	ID3D11BlendState *state;
	unsigned int lastUsedSerial;
};

struct DepthStencilStateEntry
{
	D3D11_DEPTH_STENCIL_DESC descriptor;
	ID3D11DepthStencilState *state;
	unsigned int lastUsedSerial;
};

struct RasterizerStateEntry
{
	D3D11_RASTERIZER_DESC descriptor;
	ID3D11RasterizerState *state;
	unsigned int lastUsedSerial;
};

struct SamplerStateEntry
{
	D3D11_SAMPLER_DESC descriptor;
	ID3D11SamplerState *state;
	unsigned int lastUsedSerial;
};

struct InputLayoutEntry
{
	LegacyVertexLayout descriptor;
	ID3D11InputLayout *layout;
	unsigned int lastUsedSerial;
};

DXGI_FORMAT TranslateVertexDataFormat(LegacyVertexDataFormat format)
{
	switch (format)
	{
	case RENDER_VERTEX_DATA_FLOAT1: return DXGI_FORMAT_R32_FLOAT;
	case RENDER_VERTEX_DATA_FLOAT2: return DXGI_FORMAT_R32G32_FLOAT;
	case RENDER_VERTEX_DATA_FLOAT3: return DXGI_FORMAT_R32G32B32_FLOAT;
	case RENDER_VERTEX_DATA_FLOAT4: return DXGI_FORMAT_R32G32B32A32_FLOAT;
	case RENDER_VERTEX_DATA_COLOR_BGRA8: return DXGI_FORMAT_B8G8R8A8_UNORM;
	default: return DXGI_FORMAT_UNKNOWN;
	}
}

unsigned int VertexDataByteCount(LegacyVertexDataFormat format)
{
	static const unsigned int sizes[] = { 4, 8, 12, 16, 4 };
	return static_cast<unsigned int>(format) <
		static_cast<unsigned int>(sizeof(sizes) / sizeof(sizes[0])) ?
		sizes[format] : 0;
}

bool EqualVertexLayouts(const LegacyVertexLayout &left,
	const LegacyVertexLayout &right)
{
	if (left.stride != right.stride ||
		left.elementCount != right.elementCount ||
		left.preTransformed != right.preTransformed)
	{
		return false;
	}
	for (unsigned int index = 0; index < left.elementCount; ++index)
	{
		const LegacyVertexElement &a = left.elements[index];
		const LegacyVertexElement &b = right.elements[index];
		if (a.semantic != b.semantic || a.semanticIndex != b.semanticIndex ||
			a.format != b.format || a.byteOffset != b.byteOffset)
		{
			return false;
		}
	}
	return true;
}

struct ResourceSlot
{
	ResourceSlot() : resource(0), view(0), renderTarget(0), depthStencil(0),
		kind(RESOURCE_NONE),
		usage(RENDER_USAGE_DEFAULT), binding(0), byteCount(0),
		gpuAuthoritative(false)
	{
	}

	ID3D11Resource *resource;
	ID3D11ShaderResourceView *view;
	ID3D11RenderTargetView *renderTarget;
	ID3D11DepthStencilView *depthStencil;
	ResourceKind kind;
	RenderUsage usage;
	unsigned int binding;
	size_t byteCount;
	// A resource populated by a GPU copy has no trustworthy CPU shadow.  Keep
	// that distinction explicit so device recovery cannot silently restore the
	// texture contents from data that predates the copy.
	bool gpuAuthoritative;
	BufferDescriptor bufferDescriptor;
	TextureDescriptor textureDescriptor;
	std::vector<unsigned char> shadow;
	std::vector<size_t> subresourceOffsets;
	std::vector<size_t> subresourceRowPitches;
	std::vector<size_t> subresourceSlicePitches;
};

struct LegacyTransformConstants
{
	float worldViewProjection[16];
	float worldView[16];
	float fogColor[4];
	float fogParameters[4];
	unsigned int alphaTestParameters[4];
	unsigned int textureColorParameters[LEGACY_TEXTURE_STAGE_COUNT][4];
	unsigned int textureAlphaParameters[LEGACY_TEXTURE_STAGE_COUNT][4];
	unsigned int textureModifierParameters[LEGACY_TEXTURE_STAGE_COUNT][4];
	float textureBumpParameters0[LEGACY_TEXTURE_STAGE_COUNT][4];
	float textureBumpParameters1[LEGACY_TEXTURE_STAGE_COUNT][4];
	float textureFactor[4];
	float world[16];
	float textureTransforms[LEGACY_TEXTURE_STAGE_COUNT][16];
	float materialDiffuse[4];
	float materialAmbient[4];
	float materialSpecular[4];
	float materialEmissive[4];
	float materialSpecularPower[4];
	float globalAmbient[4];
	float lightDiffuse[LEGACY_LIGHT_COUNT][4];
	float lightAmbient[LEGACY_LIGHT_COUNT][4];
	float lightSpecular[LEGACY_LIGHT_COUNT][4];
	float lightPositionAndType[LEGACY_LIGHT_COUNT][4];
	float lightDirectionAndEnabled[LEGACY_LIGHT_COUNT][4];
	float lightAttenuation[LEGACY_LIGHT_COUNT][4];
	float lightSpotParameters[LEGACY_LIGHT_COUNT][4];
	unsigned int lightingParameters[4];
	unsigned int vertexLayoutParameters[4];
	float viewportParameters[4];
	unsigned int programParameters[4];
	float vertexShaderConstants[LEGACY_VERTEX_CONSTANT_COUNT][4];
	float pixelShaderConstants[LEGACY_PIXEL_CONSTANT_COUNT][4];
	float view[16];
	// Each row of a row-major float3x3 occupies one 16-byte constant-buffer
	// register. Keep the fourth component of each row as explicit padding.
	float worldNormalMatrix[12];
	float worldViewNormalMatrix[12];
	float clipPlanes[LEGACY_CLIP_PLANE_COUNT][4];
	unsigned int clipPlaneParameters[4];
	unsigned int fogStateParameters[4];
};

void MultiplyMatrices(const float *left, const float *right, float *product)
{
	for (unsigned int row = 0; row < 4; ++row)
	{
		for (unsigned int column = 0; column < 4; ++column)
		{
			float value = 0.0f;
			for (unsigned int component = 0; component < 4; ++component)
			{
				value += left[row * 4 + component] *
					right[component * 4 + column];
			}
			product[row * 4 + column] = value;
		}
	}
}

bool IsSeaWaveVertexLayout(const LegacyVertexLayout &layout)
{
	if (layout.stride != 24 || layout.elementCount != 3 ||
		layout.preTransformed)
	{
		return false;
	}
	const LegacyVertexElement &position = layout.elements[0];
	const LegacyVertexElement &diffuse = layout.elements[1];
	const LegacyVertexElement &texture = layout.elements[2];
	return position.semantic == RENDER_VERTEX_SEMANTIC_POSITION &&
		position.semanticIndex == 0 &&
		position.format == RENDER_VERTEX_DATA_FLOAT3 &&
		position.byteOffset == 0 &&
		diffuse.semantic == RENDER_VERTEX_SEMANTIC_DIFFUSE &&
		diffuse.semanticIndex == 0 &&
		diffuse.format == RENDER_VERTEX_DATA_COLOR_BGRA8 &&
		diffuse.byteOffset == 12 &&
		texture.semantic == RENDER_VERTEX_SEMANTIC_TEXTURE_COORDINATE &&
		texture.semanticIndex == 0 &&
		texture.format == RENDER_VERTEX_DATA_FLOAT2 &&
		texture.byteOffset == 16;
}

unsigned int CountActiveFixedFunctionStages(
	const LegacyPipelineState &pipeline)
{
	unsigned int count = 0;
	while (count < LEGACY_TEXTURE_STAGE_COUNT &&
		pipeline.textureStages[count].colorOperation !=
			RENDER_TEXTURE_OP_DISABLE)
	{
		++count;
	}
	return count;
}

bool CanDisableLegacyPixelShader(const LegacyPipelineState &pipeline)
{
	if ((pipeline.blend.colorWriteMask & 0x0fU) != 0U ||
		pipeline.alphaTestEnable)
	{
		return false;
	}
	// These shaders only produce color and share the optional alpha-test
	// discard. They do not write depth/coverage or have UAV side effects;
	// clipping is still performed by the vertex shader's SV_ClipDistance.
	// Keep an explicit allowlist so a future program is not silently skipped.
	switch (pipeline.pixelProgram)
	{
	case RENDER_LEGACY_PIXEL_FIXED_FUNCTION:
	case RENDER_LEGACY_PIXEL_WATER_FLAT:
	case RENDER_LEGACY_PIXEL_WATER_RIVER:
	case RENDER_LEGACY_PIXEL_TERRAIN_BASE:
	case RENDER_LEGACY_PIXEL_TERRAIN_NOISE:
	case RENDER_LEGACY_PIXEL_TERRAIN_NOISE2:
	case RENDER_LEGACY_PIXEL_ROAD_NOISE2:
	case RENDER_LEGACY_PIXEL_FLAT_TERRAIN_BASE0:
	case RENDER_LEGACY_PIXEL_FLAT_TERRAIN_BASE:
	case RENDER_LEGACY_PIXEL_FLAT_TERRAIN_NOISE:
	case RENDER_LEGACY_PIXEL_FLAT_TERRAIN_NOISE2:
	case RENDER_LEGACY_PIXEL_MONOCHROME:
	case RENDER_LEGACY_PIXEL_WATER_SEA:
	case RENDER_LEGACY_PIXEL_PROFILER_SWIZZLE:
		return true;
	default:
		return false;
	}
}

class D3D11RenderDevice : public IRenderDevice, public IRenderContext
{
public:
	D3D11RenderDevice() : m_device(0), m_context(0), m_debugLayer(0),
		m_debugLayerActive(false), m_swapChain(0),
		m_renderTarget(0), m_renderTargetResource(0), m_depthTexture(0), m_depthStencil(0),
		m_activeRenderTarget(0), m_activeDepthStencil(0),
		m_activeColorResource(0), m_activeDepthResource(0),
		m_vertexShader(0), m_pixelShader(0), m_positionColorLayout(0),
		m_texturedVertexShader(0), m_texturedPixelShader(0),
		m_texturedFixed1PixelShader(0), m_texturedFixed2PixelShader(0),
		m_waterFlatPixelShader(0), m_waterRiverPixelShader(0),
		m_seaWaveVertexShader(0), m_seaWavePixelShader(0),
		m_seaWaveLayout(0),
		m_handles(0), m_stateUseSerial(0), m_ownerThread(0), m_initialized(false),
		m_frameOpen(false), m_pipelineBound(false), m_vertexBufferBound(false),
		m_indexBufferBound(false), m_topologyBound(false),
		m_enableVsync(true), m_transformConstantCursor(0), m_width(0), m_height(0),
		m_viewportX(0.0f), m_viewportY(0.0f), m_viewportWidth(0.0f),
		m_viewportHeight(0.0f), m_hasVertexLayoutFlagsOverride(false),
		m_vertexLayoutFlagsOverride(0), m_parameters(),
		m_pipelineStateValid(false), m_boundStencilReference(0),
		m_boundBlendState(0), m_boundDepthState(0),
		m_boundRasterizerState(0), m_boundInputLayout(0),
		m_boundVertexShader(0), m_boundPixelShader(0),
		m_pipelineHasTextures(false), m_cachedLegacyStateValid(false),
		m_cachedLegacyPipelineValid(false),
		m_cachedLegacyVertexFormat(RENDER_VERTEX_POSITION3_COLOR),
		m_cachedLegacyTexturePresenceMask(0), m_cachedLegacyCubeTextureMask(0),
		m_cachedLegacySignedTextureMask(0),
		m_cachedLegacyVertexLayoutFlags(0), m_cachedLegacyInputLayout(0),
		m_transformConstantsValid(false),
		m_transformConstantsChanged(false), m_hasInputLayoutOverride(false),
		m_inputLayoutOverride(0), m_renderTargetsBound(false),
		m_textureBindingsValid(false),
		m_boundCubeTextureMask(0), m_boundSignedTextureMask(0),
		m_boundVertexBuffer(), m_boundVertexStride(0), m_boundVertexOffset(0),
		m_boundIndexBuffer(), m_boundIndexFormat(RENDER_FORMAT_UNKNOWN),
		m_boundIndexOffset(0), m_boundTopology(RENDER_PRIMITIVE_TRIANGLE_LIST),
		m_viewportBound(false), m_viewportMinimumDepth(0.0f),
		m_viewportMaximumDepth(1.0f)
	{
		memset(m_transformConstants, 0, sizeof(m_transformConstants));
		memset(m_terrainPixelShaders, 0, sizeof(m_terrainPixelShaders));
		memset(&m_boundBlendDescriptor, 0, sizeof(m_boundBlendDescriptor));
		memset(&m_boundDepthDescriptor, 0, sizeof(m_boundDepthDescriptor));
		memset(&m_boundRasterizerDescriptor, 0,
			sizeof(m_boundRasterizerDescriptor));
		memset(m_boundSamplerStates, 0, sizeof(m_boundSamplerStates));
		memset(m_cachedLegacyState, 0, sizeof(m_cachedLegacyState));
		memset(m_cachedLegacyPipeline, 0, sizeof(m_cachedLegacyPipeline));
		memset(m_boundTextures, 0, sizeof(m_boundTextures));
		memset(&m_lastTransformConstants, 0, sizeof(m_lastTransformConstants));
	}

	virtual ~D3D11RenderDevice()
	{
		shutdown();
	}

	virtual RenderBackend backend() const
	{
		return RENDER_BACKEND_D3D11;
	}

	virtual bool isOperational() const
	{
		return m_initialized && m_device != 0 && m_context != 0;
	}

	virtual RenderResult initialize(const RenderDeviceParameters &parameters)
	{
		if (m_initialized || parameters.backend != RENDER_BACKEND_D3D11 ||
			parameters.width == 0 || parameters.height == 0)
		{
			return RENDER_RESULT_INVALID_ARGUMENT;
		}

		HRESULT result = createNativeDevice(parameters);
		if (FAILED(result))
		{
			releaseDeviceObjects();
			return TranslateResult(result);
		}

		try
		{
			m_handles = new GpuHandleAllocator(RESOURCE_CAPACITY);
			m_resources.resize(RESOURCE_CAPACITY);
		}
		catch (...)
		{
			delete m_handles;
			m_handles = 0;
			releaseDeviceObjects();
			return RENDER_RESULT_OUT_OF_MEMORY;
		}
		if (m_handles == 0 || m_handles->capacity() != RESOURCE_CAPACITY)
		{
			delete m_handles;
			m_handles = 0;
			releaseDeviceObjects();
			return RENDER_RESULT_OUT_OF_MEMORY;
		}

		m_ownerThread = GetCurrentThreadId();
		m_enableVsync = parameters.enableVsync;
		m_width = parameters.width;
		m_height = parameters.height;
		if (parameters.window != 0)
		{
			result = createSwapChain(static_cast<HWND>(parameters.window));
			if (FAILED(result))
			{
				shutdownInternal();
				return TranslateResult(result);
			}
			result = createBackBufferTargets(m_width, m_height);
			if (FAILED(result))
			{
				shutdownInternal();
				return TranslateResult(result);
			}
		}
		result = createPipelineResources();
		if (FAILED(result))
		{
			shutdownInternal();
			return TranslateResult(result);
		}
		m_initialized = true;
		m_parameters = parameters;
		return RENDER_RESULT_OK;
	}

	virtual void shutdown()
	{
		if (m_ownerThread != 0 && GetCurrentThreadId() != m_ownerThread)
		{
			return;
		}
		shutdownInternal();
	}

	virtual IRenderContext *immediateContext()
	{
		return isOwner() ? this : 0;
	}

	virtual RenderResult createBuffer(const BufferDescriptor &descriptor,
		const void *initialData, size_t initialDataBytes, GpuHandle *buffer)
	{
		if (!isOwner() || buffer == 0 || descriptor.byteCount == 0 ||
			descriptor.byteCount > UINT_MAX || descriptor.binding == 0 ||
			(initialData == 0 && initialDataBytes != 0) ||
			(initialData != 0 && initialDataBytes != descriptor.byteCount) ||
			(descriptor.usage == RENDER_USAGE_IMMUTABLE && initialData == 0) ||
			((descriptor.binding & RENDER_BUFFER_CONSTANT) != 0 &&
				(descriptor.byteCount % 16) != 0))
		{
			return RENDER_RESULT_INVALID_ARGUMENT;
		}
		*buffer = GpuHandle();

		D3D11_BUFFER_DESC nativeDescriptor;
		memset(&nativeDescriptor, 0, sizeof(nativeDescriptor));
		nativeDescriptor.ByteWidth = static_cast<UINT>(descriptor.byteCount);
		if ((descriptor.binding & RENDER_BUFFER_VERTEX) != 0)
		{
			nativeDescriptor.BindFlags |= D3D11_BIND_VERTEX_BUFFER;
		}
		if ((descriptor.binding & RENDER_BUFFER_INDEX) != 0)
		{
			nativeDescriptor.BindFlags |= D3D11_BIND_INDEX_BUFFER;
		}
		if ((descriptor.binding & RENDER_BUFFER_CONSTANT) != 0)
		{
			nativeDescriptor.BindFlags |= D3D11_BIND_CONSTANT_BUFFER;
		}
		setNativeUsage(descriptor.usage, &nativeDescriptor.Usage,
			&nativeDescriptor.CPUAccessFlags);

		D3D11_SUBRESOURCE_DATA nativeInitialData;
		memset(&nativeInitialData, 0, sizeof(nativeInitialData));
		nativeInitialData.pSysMem = initialData;
		ID3D11Buffer *nativeBuffer = 0;
		const HRESULT result = m_device->CreateBuffer(&nativeDescriptor,
			initialData == 0 ? 0 : &nativeInitialData, &nativeBuffer);
		if (FAILED(result))
		{
			return TranslateResult(result);
		}

		GpuHandle handle = m_handles->allocate();
		if (!handle.isValid())
		{
			nativeBuffer->Release();
			return RENDER_RESULT_OUT_OF_MEMORY;
		}
		ResourceSlot &slot = m_resources[handle.index()];
		slot.resource = nativeBuffer;
		slot.kind = RESOURCE_BUFFER;
		slot.usage = descriptor.usage;
		slot.binding = descriptor.binding;
		slot.byteCount = descriptor.byteCount;
		slot.bufferDescriptor = descriptor;
		try
		{
			slot.shadow.assign(descriptor.byteCount, 0);
			if (initialData != 0)
			{
				memcpy(&slot.shadow[0], initialData, descriptor.byteCount);
			}
		}
		catch (...)
		{
			releaseSlot(handle, slot);
			return RENDER_RESULT_OUT_OF_MEMORY;
		}
		*buffer = handle;
		return RENDER_RESULT_OK;
	}

	virtual RenderResult createTexture(const TextureDescriptor &descriptor,
		const TextureSubresourceData *initialData,
		unsigned int initialDataCount, GpuHandle *texture)
	{
		const DXGI_FORMAT format = TranslateFormat(descriptor.format);
		if (descriptor.arrayCount != 0 &&
			descriptor.mipCount > UINT_MAX / descriptor.arrayCount)
		{
			return RENDER_RESULT_INVALID_ARGUMENT;
		}
		const unsigned int subresourceCount = descriptor.mipCount * descriptor.arrayCount;
		if (!isOwner() || texture == 0 || descriptor.width == 0 ||
			descriptor.height == 0 || descriptor.mipCount == 0 ||
			descriptor.arrayCount == 0 || format == DXGI_FORMAT_UNKNOWN ||
			(descriptor.dimension != RENDER_TEXTURE_2D &&
				descriptor.dimension != RENDER_TEXTURE_CUBE) ||
			// The fixed-function shader contract exposes Texture2D and TextureCube
			// resources only.  Do not create native array resources that would bind
			// successfully but fail validation (or sample as black) at draw time.
			(descriptor.dimension == RENDER_TEXTURE_2D &&
				descriptor.arrayCount != 1) ||
			(descriptor.dimension == RENDER_TEXTURE_CUBE &&
				(descriptor.width != descriptor.height ||
				descriptor.arrayCount != 6 ||
				(descriptor.binding & (RENDER_TEXTURE_RENDER_TARGET |
					RENDER_TEXTURE_DEPTH_STENCIL)) != 0)) ||
			descriptor.binding == 0 ||
			(initialData == 0 && initialDataCount != 0) ||
			(initialData != 0 && initialDataCount != subresourceCount) ||
			(descriptor.usage == RENDER_USAGE_IMMUTABLE && initialData == 0))
		{
			return RENDER_RESULT_INVALID_ARGUMENT;
		}
		*texture = GpuHandle();

		D3D11_TEXTURE2D_DESC nativeDescriptor;
		memset(&nativeDescriptor, 0, sizeof(nativeDescriptor));
		nativeDescriptor.Width = descriptor.width;
		nativeDescriptor.Height = descriptor.height;
		nativeDescriptor.MipLevels = descriptor.mipCount;
		nativeDescriptor.ArraySize = descriptor.arrayCount;
		nativeDescriptor.Format = format;
		nativeDescriptor.SampleDesc.Count = 1;
		if (descriptor.dimension == RENDER_TEXTURE_CUBE)
		{
			nativeDescriptor.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;
		}
		if ((descriptor.binding & RENDER_TEXTURE_SHADER_RESOURCE) != 0)
		{
			nativeDescriptor.BindFlags |= D3D11_BIND_SHADER_RESOURCE;
		}
		if ((descriptor.binding & RENDER_TEXTURE_RENDER_TARGET) != 0)
		{
			nativeDescriptor.BindFlags |= D3D11_BIND_RENDER_TARGET;
		}
		if ((descriptor.binding & RENDER_TEXTURE_DEPTH_STENCIL) != 0)
		{
			nativeDescriptor.BindFlags |= D3D11_BIND_DEPTH_STENCIL;
		}
		setNativeUsage(descriptor.usage, &nativeDescriptor.Usage,
			&nativeDescriptor.CPUAccessFlags);

		std::vector<D3D11_SUBRESOURCE_DATA> nativeInitialData;
		if (initialData != 0)
		{
			try
			{
				nativeInitialData.resize(initialDataCount);
			}
			catch (...)
			{
				return RENDER_RESULT_OUT_OF_MEMORY;
			}
			for (unsigned int i = 0; i < initialDataCount; ++i)
			{
				if (initialData[i].data == 0 || initialData[i].rowPitch == 0 ||
					initialData[i].rowPitch > UINT_MAX ||
					initialData[i].slicePitch > UINT_MAX)
				{
					return RENDER_RESULT_INVALID_ARGUMENT;
				}
				const unsigned int mip = i % descriptor.mipCount;
				const unsigned int mipWidth = mip < 32 ?
					descriptor.width >> mip : 0;
				const unsigned int mipHeight = mip < 32 ?
					descriptor.height >> mip : 0;
				const size_t width = mipWidth == 0 ? 1 : mipWidth;
				const size_t height = mipHeight == 0 ? 1 : mipHeight;
				const unsigned int bytesPerPixel = TextureBytesPerPixel(
					descriptor.format);
				if (bytesPerPixel == 0 || width > static_cast<size_t>(-1) /
					bytesPerPixel || initialData[i].rowPitch < width *
					bytesPerPixel || height > static_cast<size_t>(-1) /
					initialData[i].rowPitch)
				{
					return RENDER_RESULT_INVALID_ARGUMENT;
				}
				const size_t minimumSlicePitch = initialData[i].rowPitch * height;
				if (initialData[i].slicePitch != 0 &&
					initialData[i].slicePitch < minimumSlicePitch)
				{
					return RENDER_RESULT_INVALID_ARGUMENT;
				}
				nativeInitialData[i].pSysMem = initialData[i].data;
				nativeInitialData[i].SysMemPitch =
					static_cast<UINT>(initialData[i].rowPitch);
				nativeInitialData[i].SysMemSlicePitch =
					static_cast<UINT>(initialData[i].slicePitch);
			}
		}
		ID3D11Texture2D *nativeTexture = 0;
		const HRESULT result = m_device->CreateTexture2D(&nativeDescriptor,
			nativeInitialData.empty() ? 0 : &nativeInitialData[0], &nativeTexture);
		if (FAILED(result))
		{
			return TranslateResult(result);
		}
		GpuHandle handle = m_handles->allocate();
		if (!handle.isValid())
		{
			nativeTexture->Release();
			return RENDER_RESULT_OUT_OF_MEMORY;
		}
		ResourceSlot &slot = m_resources[handle.index()];
		slot.resource = nativeTexture;
		slot.kind = RESOURCE_TEXTURE;
		slot.usage = descriptor.usage;
		slot.binding = descriptor.binding;
		slot.byteCount = 0;
		slot.textureDescriptor = descriptor;
		if ((descriptor.binding & RENDER_TEXTURE_SHADER_RESOURCE) != 0)
		{
			D3D11_SHADER_RESOURCE_VIEW_DESC viewDescriptor;
			D3D11_SHADER_RESOURCE_VIEW_DESC *viewDescriptorPointer = 0;
			if (descriptor.dimension == RENDER_TEXTURE_CUBE)
			{
				memset(&viewDescriptor, 0, sizeof(viewDescriptor));
				viewDescriptor.Format = format;
				if (descriptor.arrayCount == 6)
				{
					viewDescriptor.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
					viewDescriptor.TextureCube.MostDetailedMip = 0;
					viewDescriptor.TextureCube.MipLevels = descriptor.mipCount;
				}
				else
				{
					viewDescriptor.ViewDimension =
						D3D11_SRV_DIMENSION_TEXTURECUBEARRAY;
					viewDescriptor.TextureCubeArray.MostDetailedMip = 0;
					viewDescriptor.TextureCubeArray.MipLevels = descriptor.mipCount;
					viewDescriptor.TextureCubeArray.First2DArrayFace = 0;
					viewDescriptor.TextureCubeArray.NumCubes = descriptor.arrayCount / 6;
				}
				viewDescriptorPointer = &viewDescriptor;
			}
			const HRESULT viewResult = m_device->CreateShaderResourceView(
				nativeTexture, viewDescriptorPointer, &slot.view);
			if (FAILED(viewResult))
			{
				releaseSlot(handle, slot);
				return TranslateResult(viewResult);
			}
		}
		if ((descriptor.binding & RENDER_TEXTURE_RENDER_TARGET) != 0)
		{
			const HRESULT viewResult = m_device->CreateRenderTargetView(
				nativeTexture, 0, &slot.renderTarget);
			if (FAILED(viewResult))
			{
				releaseSlot(handle, slot);
				return TranslateResult(viewResult);
			}
		}
		if ((descriptor.binding & RENDER_TEXTURE_DEPTH_STENCIL) != 0)
		{
			const HRESULT viewResult = m_device->CreateDepthStencilView(
				nativeTexture, 0, &slot.depthStencil);
			if (FAILED(viewResult))
			{
				releaseSlot(handle, slot);
				return TranslateResult(viewResult);
			}
		}
		if (initialData != 0)
		{
			try
			{
				size_t totalBytes = 0;
				slot.subresourceOffsets.resize(initialDataCount);
				slot.subresourceRowPitches.resize(initialDataCount);
				slot.subresourceSlicePitches.resize(initialDataCount);
				for (unsigned int i = 0; i < initialDataCount; ++i)
				{
					const unsigned int mip = i % descriptor.mipCount;
					const unsigned int mipHeight = mip < 32 ?
						descriptor.height >> mip : 0;
					const size_t rowCount = mipHeight == 0 ? 1 : mipHeight;
					const size_t minimumBytes = initialData[i].rowPitch * rowCount;
					const size_t copyBytes = initialData[i].slicePitch > minimumBytes ?
						initialData[i].slicePitch : minimumBytes;
					if (copyBytes > static_cast<size_t>(-1) - totalBytes)
					{
						releaseSlot(handle, slot);
						return RENDER_RESULT_INVALID_ARGUMENT;
					}
					slot.subresourceOffsets[i] = totalBytes;
					slot.subresourceRowPitches[i] = initialData[i].rowPitch;
					slot.subresourceSlicePitches[i] = initialData[i].slicePitch;
					totalBytes += copyBytes;
				}
				slot.shadow.resize(totalBytes);
				for (unsigned int i = 0; i < initialDataCount; ++i)
				{
					const size_t nextOffset = i + 1 < initialDataCount ?
						slot.subresourceOffsets[i + 1] : totalBytes;
					memcpy(&slot.shadow[slot.subresourceOffsets[i]],
						initialData[i].data, nextOffset - slot.subresourceOffsets[i]);
				}
			}
			catch (...)
			{
				releaseSlot(handle, slot);
				return RENDER_RESULT_OUT_OF_MEMORY;
			}
		}
		*texture = handle;
		return RENDER_RESULT_OK;
	}

	virtual RenderResult refreshTexture(GpuHandle texture,
		const TextureDescriptor &descriptor,
		const TextureSubresourceData *data, unsigned int dataCount)
	{
		if (!isOwner() || !m_handles->isLive(texture) || data == 0)
		{
			return RENDER_RESULT_INVALID_ARGUMENT;
		}
		ResourceSlot &slot = m_resources[texture.index()];
		if (slot.kind != RESOURCE_TEXTURE || slot.resource == 0)
		{
			return RENDER_RESULT_INVALID_ARGUMENT;
		}
		if (!EqualTextureDescriptors(slot.textureDescriptor, descriptor))
		{
			return RENDER_RESULT_UNSUPPORTED;
		}
		// Immutable allocations deliberately retain their creation-time data.
		// The bridge treats this capability result as the narrow recreation path.
		if (slot.usage == RENDER_USAGE_IMMUTABLE)
		{
			return RENDER_RESULT_UNSUPPORTED;
		}
		if (slot.resource == m_activeColorResource ||
			slot.resource == m_activeDepthResource)
		{
			// An active output must be released by the target owner first.  Do not
			// silently replace it underneath the current render-target binding.
			return RENDER_RESULT_INVALID_ARGUMENT;
		}
		if (m_context == 0)
		{
			return RENDER_RESULT_FAILED;
		}

		// Keep the recovery shadow attached to the logical resource.  The movie
		// path refreshes one fixed-size BGRA texture for every decoded frame; using
		// temporary vectors here would allocate and release the complete 4K image
		// on every refresh even though the existing shadow has the exact capacity
		// needed for recovery.  VideoBuffer allocations use decoder dimensions and
		// display scaling happens at draw time, so a 4K display does not imply a 4K
		// shadow unless the movie source itself is 4K.
		const RenderResult preparationResult = prepareTextureRefreshData(
			descriptor, data, dataCount, &slot);
		if (preparationResult != RENDER_RESULT_OK)
		{
			return preparationResult;
		}

		// D3D11 forbids updating a resource while it is exposed as an SRV.  Clear
		// only the stages that reference this texture so unrelated legacy stages
		// remain intact across the refresh.
		const unsigned int reboundStages = unbindTextureResource(texture);

		const unsigned int subresourceCount = descriptor.mipCount *
			descriptor.arrayCount;
		const unsigned int bytesPerPixel = TextureBytesPerPixel(
			descriptor.format);
		for (unsigned int index = 0; index < subresourceCount; ++index)
		{
			const unsigned int mip = index % descriptor.mipCount;
			const unsigned int arraySlice = index / descriptor.mipCount;
			const UINT nativeSubresource = D3D11CalcSubresource(mip, arraySlice,
				descriptor.mipCount);
			if (slot.usage == RENDER_USAGE_DYNAMIC)
			{
				D3D11_MAPPED_SUBRESOURCE mapped;
				memset(&mapped, 0, sizeof(mapped));
				const HRESULT mapResult = m_context->Map(slot.resource,
					nativeSubresource, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
				if (FAILED(mapResult))
				{
					// Earlier subresources may already have been written.  The old
					// CPU shadow is therefore no longer a complete description of
					// the native resource; do not let a later device recovery
					// restore a mixed/obsolete image.
					markTextureRecoverySourceUnavailable(slot);
					return TranslateResult(mapResult);
				}
				if (mapped.pData == 0 || mapped.RowPitch == 0)
				{
					m_context->Unmap(slot.resource, nativeSubresource);
					markTextureRecoverySourceUnavailable(slot);
					return RENDER_RESULT_FAILED;
				}
				const unsigned int mipWidth = mip < 32 ?
					descriptor.width >> mip : 0;
				const unsigned int mipHeight = mip < 32 ?
					descriptor.height >> mip : 0;
				const size_t rowBytes = static_cast<size_t>(mipWidth == 0 ? 1 :
					mipWidth) * bytesPerPixel;
				const unsigned int rowCount = mipHeight == 0 ? 1 : mipHeight;
				if (mapped.RowPitch < rowBytes)
				{
					m_context->Unmap(slot.resource, nativeSubresource);
					markTextureRecoverySourceUnavailable(slot);
					return RENDER_RESULT_INVALID_ARGUMENT;
				}
				const unsigned char *source = static_cast<const unsigned char *>(
					data[index].data);
				unsigned char *destination = static_cast<unsigned char *>(
					mapped.pData);
				for (unsigned int row = 0; row < rowCount; ++row)
				{
					memcpy(destination + static_cast<size_t>(row) * mapped.RowPitch,
						source + static_cast<size_t>(row) * data[index].rowPitch,
						rowBytes);
				}
				m_context->Unmap(slot.resource, nativeSubresource);
			}
			else
			{
				m_context->UpdateSubresource(slot.resource, nativeSubresource, 0,
					data[index].data,
					static_cast<UINT>(data[index].rowPitch),
					static_cast<UINT>(data[index].slicePitch));
				const HRESULT deviceResult = m_device->GetDeviceRemovedReason();
				if (FAILED(deviceResult))
				{
					markTextureRecoverySourceUnavailable(slot);
					return TranslateResult(deviceResult);
				}
			}
		}

		// The native upload above consumes the caller's owner-thread memory
		// synchronously.  Commit the same bytes to the reusable recovery shadow
		// only after all subresources have uploaded successfully; a failed upload
		// therefore retains no partial image and is invalidated by the paths above.
		for (unsigned int index = 0; index < subresourceCount; ++index)
		{
			const unsigned int mip = index % descriptor.mipCount;
			const unsigned int mipHeight = mip < 32 ?
				descriptor.height >> mip : 0;
			const size_t height = mipHeight == 0 ? 1 : mipHeight;
			const size_t minimumSlicePitch =
				slot.subresourceRowPitches[index] * height;
			const size_t copyBytes = slot.subresourceSlicePitches[index] >
				minimumSlicePitch ? slot.subresourceSlicePitches[index] :
				minimumSlicePitch;
			const size_t offset = slot.subresourceOffsets[index];
			if (offset > slot.shadow.size() ||
				copyBytes > slot.shadow.size() - offset)
			{
				markTextureRecoverySourceUnavailable(slot);
				return RENDER_RESULT_FAILED;
			}
			memcpy(&slot.shadow[offset], data[index].data, copyBytes);
		}
		// A successful CPU upload establishes a new recovery source.  A later
		// device loss must preserve this upload rather than treating the resource
		// as GPU-only merely because it was copied earlier in its lifetime.
		slot.gpuAuthoritative = false;
		return rebindTextureResource(texture, reboundStages);
	}

	virtual RenderResult copyActiveColorTargetToTexture(GpuHandle texture)
	{
		if (!isOwner() || !m_frameOpen || !m_activeColorResource ||
			!m_handles->isLive(texture))
		{
			return RENDER_RESULT_INVALID_ARGUMENT;
		}
		ResourceSlot &destination = m_resources[texture.index()];
		if (destination.kind != RESOURCE_TEXTURE || destination.resource == 0 ||
			(destination.binding & RENDER_TEXTURE_SHADER_RESOURCE) == 0 ||
			(destination.binding & RENDER_TEXTURE_RENDER_TARGET) == 0 ||
			destination.textureDescriptor.mipCount != 1 ||
			destination.textureDescriptor.arrayCount != 1 ||
			destination.textureDescriptor.usage == RENDER_USAGE_IMMUTABLE)
		{
			return RENDER_RESULT_UNSUPPORTED;
		}
		if (destination.resource == m_activeColorResource ||
			destination.resource == m_activeDepthResource)
		{
			// Copying a target into itself, or into the active depth resource, is
			// never a valid synchronization operation.
			return RENDER_RESULT_INVALID_ARGUMENT;
		}

		D3D11_RESOURCE_DIMENSION sourceType;
		D3D11_RESOURCE_DIMENSION destinationType;
		m_activeColorResource->GetType(&sourceType);
		destination.resource->GetType(&destinationType);
		if (sourceType != D3D11_RESOURCE_DIMENSION_TEXTURE2D ||
			destinationType != D3D11_RESOURCE_DIMENSION_TEXTURE2D)
		{
			return RENDER_RESULT_UNSUPPORTED;
		}
		ID3D11Texture2D *sourceTexture = 0;
		ID3D11Texture2D *destinationTexture = 0;
		HRESULT result = m_activeColorResource->QueryInterface(
			__uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&sourceTexture));
		if (SUCCEEDED(result))
		{
			result = destination.resource->QueryInterface(__uuidof(ID3D11Texture2D),
				reinterpret_cast<void **>(&destinationTexture));
		}
		if (FAILED(result) || sourceTexture == 0 || destinationTexture == 0)
		{
			if (destinationTexture != 0) destinationTexture->Release();
			if (sourceTexture != 0) sourceTexture->Release();
			return TranslateResult(FAILED(result) ? result : E_NOINTERFACE);
		}

		D3D11_TEXTURE2D_DESC sourceDescriptor;
		D3D11_TEXTURE2D_DESC destinationDescriptor;
		sourceTexture->GetDesc(&sourceDescriptor);
		destinationTexture->GetDesc(&destinationDescriptor);
		const bool compatible = sourceDescriptor.Width == destinationDescriptor.Width &&
			sourceDescriptor.Height == destinationDescriptor.Height &&
			sourceDescriptor.MipLevels == 1 && destinationDescriptor.MipLevels == 1 &&
			sourceDescriptor.ArraySize == 1 && destinationDescriptor.ArraySize == 1 &&
			sourceDescriptor.Format == destinationDescriptor.Format &&
			sourceDescriptor.SampleDesc.Count == destinationDescriptor.SampleDesc.Count &&
			sourceDescriptor.SampleDesc.Quality == destinationDescriptor.SampleDesc.Quality;
		if (!compatible)
		{
			destinationTexture->Release();
			sourceTexture->Release();
			return RENDER_RESULT_UNSUPPORTED;
		}

		// CopyResource rejects a destination that is still exposed as an SRV.
		// Preserve every unrelated stage and restore the destination stages after
		// the copy completes.
		const unsigned int reboundStages = unbindTextureResource(texture);
		m_context->CopyResource(destinationTexture, sourceTexture);
		const HRESULT deviceResult = m_device->GetDeviceRemovedReason();
		destinationTexture->Release();
		sourceTexture->Release();
		if (FAILED(deviceResult))
		{
			return TranslateResult(deviceResult);
		}

		// CopyResource makes the destination GPU-authoritative.  Any creation or
		// refresh bytes retained before this point describe an older CPU upload and
		// would restore stale pixels after device recovery.  Clear them now and
		// let recreateTexture establish a deterministic cleared resource; the
		// producer will repopulate it on the next render pass.
		destination.gpuAuthoritative = true;
		destination.shadow.clear();
		destination.subresourceOffsets.clear();
		destination.subresourceRowPitches.clear();
		destination.subresourceSlicePitches.clear();
		return rebindTextureResource(texture, reboundStages);
	}

	virtual bool destroyResource(GpuHandle resource)
	{
		if (!isOwner() || !m_handles->isLive(resource))
		{
			return false;
		}
		ResourceSlot &slot = m_resources[resource.index()];
		if (slot.kind == RESOURCE_TEXTURE)
		{
			unbindTextureResource(resource);
			if (slot.renderTarget == m_activeRenderTarget ||
				slot.depthStencil == m_activeDepthStencil)
			{
				if (m_renderTarget != 0)
				{
					m_context->OMSetRenderTargets(1, &m_renderTarget,
						m_depthStencil);
				}
				else
				{
					m_context->OMSetRenderTargets(0, 0, 0);
				}
				m_activeRenderTarget = m_renderTarget;
				m_activeDepthStencil = m_depthStencil;
				m_activeColorResource = m_renderTargetResource;
				m_activeDepthResource = m_depthTexture;
				m_renderTargetsBound = true;
			}
		}
		else if (slot.kind == RESOURCE_BUFFER)
		{
			if ((slot.binding & RENDER_BUFFER_VERTEX) != 0)
			{
				ID3D11Buffer *emptyBuffer = 0;
				const UINT zero = 0;
				m_context->IASetVertexBuffers(0, 1, &emptyBuffer, &zero, &zero);
				m_vertexBufferBound = false;
				m_boundVertexBuffer = GpuHandle();
			}
			if ((slot.binding & RENDER_BUFFER_INDEX) != 0)
			{
				m_context->IASetIndexBuffer(0, DXGI_FORMAT_UNKNOWN, 0);
				m_indexBufferBound = false;
				m_boundIndexBuffer = GpuHandle();
			}
		}
		return releaseSlot(resource, slot);
	}

	virtual RenderResult recoverDevice()
	{
		if (!isOwner() || m_frameOpen)
		{
			return RENDER_RESULT_INVALID_ARGUMENT;
		}
		m_parameters.width = m_width;
		m_parameters.height = m_height;
		m_activeRenderTarget = 0;
		m_activeDepthStencil = 0;
		m_activeColorResource = 0;
		m_activeDepthResource = 0;
		invalidateContextBindings();
		m_context->ClearState();
		m_context->Flush();
		for (unsigned int i = 0; i < m_resources.size(); ++i)
		{
			releaseNativeSlot(m_resources[i]);
		}
		releasePipelineResources();
		releaseBackBufferTargets();
		releaseSwapChain();
		releaseImmediateContext();
		// Report after every logical/native resource has been released, but while
		// the device and debug interface are still alive. Retail devices simply
		// return UNSUPPORTED here because the optional SDK layer is not present.
		reportLiveObjects();
		releaseDeviceObjects();
		HRESULT result = createNativeDevice(m_parameters);
		if (SUCCEEDED(result) && m_parameters.window != 0)
		{
			result = createSwapChain(static_cast<HWND>(m_parameters.window));
			if (SUCCEEDED(result))
			{
				result = createBackBufferTargets(m_width, m_height);
			}
		}
		if (SUCCEEDED(result))
		{
			result = createPipelineResources();
		}
		for (unsigned int i = 0; SUCCEEDED(result) &&
			i < m_resources.size(); ++i)
		{
			ResourceSlot &slot = m_resources[i];
			if (slot.kind == RESOURCE_BUFFER)
			{
				result = recreateBuffer(slot);
			}
			else if (slot.kind == RESOURCE_TEXTURE)
			{
				result = recreateTexture(slot);
			}
		}
		if (FAILED(result))
		{
			for (unsigned int i = 0; i < m_resources.size(); ++i)
			{
				releaseNativeSlot(m_resources[i]);
			}
			releasePipelineResources();
			releaseBackBufferTargets();
			releaseDeviceObjects();
			// A failed recovery cannot leave logical slots, allocator state, or
			// active aliases attached to a dead native device. Return to the same
			// clean state as shutdown so a later initialize is safe.
			m_activeRenderTarget = 0;
			m_activeDepthStencil = 0;
			shutdownInternal();
			return TranslateResult(result);
		}
		m_activeRenderTarget = m_renderTarget;
		m_activeDepthStencil = m_depthStencil;
		m_activeColorResource = m_renderTargetResource;
		m_activeDepthResource = m_depthTexture;
		if (m_renderTarget != 0)
		{
			m_context->OMSetRenderTargets(1, &m_renderTarget, m_depthStencil);
		}
		else
		{
			// A headless device can still render to logical targets.  Ensure the
			// recreated context starts with no output when there is no swap-chain
			// target to restore.
			m_context->OMSetRenderTargets(0, 0, 0);
		}
		m_pipelineBound = false;
		m_vertexBufferBound = false;
		m_indexBufferBound = false;
		m_topologyBound = false;
		m_renderTargetsBound = m_renderTarget != 0;
		markTextureBindingsEmpty();
		m_viewportBound = m_renderTarget != 0;
		return RENDER_RESULT_OK;
	}

	RenderResult resizeInternal(unsigned int width, unsigned int height,
		bool recoverOnDeviceRemoval)
	{
		if (!isOwner() || m_frameOpen)
		{
			return RENDER_RESULT_INVALID_ARGUMENT;
		}
		if (width == 0 || height == 0 ||
			(width == m_width && height == m_height))
		{
			// DXGI rejects zero-sized ResizeBuffers calls while a window is
			// minimized.  Keep the last valid targets alive and defer the actual
			// resize until a non-zero client area is reported.
			return RENDER_RESULT_OK;
		}
		if (m_swapChain != 0)
		{
			const unsigned int previousWidth = m_width;
			const unsigned int previousHeight = m_height;
			m_activeRenderTarget = 0;
			m_activeDepthStencil = 0;
			m_activeColorResource = 0;
			m_activeDepthResource = 0;
			m_context->OMSetRenderTargets(0, 0, 0);
			unbindTextureResources();
			releaseBackBufferTargets();
			HRESULT result = m_swapChain->ResizeBuffers(0, width, height,
				DXGI_FORMAT_UNKNOWN, 0);
			if (FAILED(result))
			{
				// ResizeBuffers is one of the DXGI calls that can first surface a
				// removed, reset, or hung device.  The old swap chain is no longer a
				// valid recovery target in that case: attempting to recreate its
				// back-buffer views merely preserves the failure and can leave the
				// bridge rendering into a black frame.  recoverDevice() releases and
				// recreates native resources in place while retaining every logical
				// handle, so retry the requested size on the fresh swap chain exactly
				// once.
				const RenderResult translatedResult = TranslateResult(result);
				if (translatedResult == RENDER_RESULT_DEVICE_REMOVED &&
					recoverOnDeviceRemoval)
				{
					const RenderResult recoveryResult = recoverDevice();
					if (recoveryResult != RENDER_RESULT_OK)
					{
						return recoveryResult;
					}
					return resizeInternal(width, height, false);
				}
				if (SUCCEEDED(createBackBufferTargets(previousWidth,
					previousHeight)))
				{
					m_activeRenderTarget = m_renderTarget;
					m_activeDepthStencil = m_depthStencil;
					m_activeColorResource = m_renderTargetResource;
					m_activeDepthResource = m_depthTexture;
					m_renderTargetsBound = true;
					m_context->OMSetRenderTargets(1, &m_renderTarget,
						m_depthStencil);
				}
				else
				{
					shutdownInternal();
				}
				return translatedResult;
			}
			result = createBackBufferTargets(width, height);
			if (FAILED(result))
			{
				m_activeRenderTarget = 0;
				m_activeDepthStencil = 0;
				m_activeColorResource = 0;
				m_activeDepthResource = 0;
				releaseBackBufferTargets();
				if (SUCCEEDED(m_swapChain->ResizeBuffers(0, previousWidth,
					previousHeight, DXGI_FORMAT_UNKNOWN, 0)) &&
					SUCCEEDED(createBackBufferTargets(previousWidth,
						previousHeight)))
				{
					m_width = previousWidth;
					m_height = previousHeight;
					m_parameters.width = previousWidth;
					m_parameters.height = previousHeight;
					m_activeRenderTarget = m_renderTarget;
					m_activeDepthStencil = m_depthStencil;
					m_activeColorResource = m_renderTargetResource;
					m_activeDepthResource = m_depthTexture;
					m_renderTargetsBound = true;
					m_context->OMSetRenderTargets(1, &m_renderTarget,
						m_depthStencil);
				}
				else
				{
					shutdownInternal();
				}
				return TranslateResult(result);
			}
			m_width = width;
			m_height = height;
			m_activeRenderTarget = m_renderTarget;
			m_activeDepthStencil = m_depthStencil;
			m_activeColorResource = m_renderTargetResource;
			m_activeDepthResource = m_depthTexture;
			m_renderTargetsBound = true;
			m_context->OMSetRenderTargets(1, &m_renderTarget, m_depthStencil);
			m_parameters.width = width;
			m_parameters.height = height;
		}
		else
		{
			m_width = width;
			m_height = height;
			m_parameters.width = width;
			m_parameters.height = height;
		}
		return RENDER_RESULT_OK;
	}

	virtual RenderResult resize(unsigned int width, unsigned int height)
	{
		return resizeInternal(width, height, true);
	}

	virtual RenderResult present()
	{
		if (!isOwner() || m_frameOpen)
		{
			return RENDER_RESULT_INVALID_ARGUMENT;
		}
		if (m_swapChain == 0)
		{
			// Headless passes have no Present call to surface asynchronous device
			// removal.  Query the device at this lifecycle boundary instead of
			// silently reporting success after a lost GPU.
			return TranslateResult(m_device->GetDeviceRemovedReason());
		}
		const HRESULT presentResult = m_swapChain->Present(
			m_enableVsync ? 1 : 0, 0);
		if (FAILED(presentResult))
		{
			return TranslateResult(presentResult);
		}
		return TranslateResult(m_device->GetDeviceRemovedReason());
	}

	virtual RenderResult getBackBufferInfo(RenderBackBufferInfo *info) const
	{
		if (!isOwner() || info == 0 || m_swapChain == 0)
		{
			return RENDER_RESULT_INVALID_ARGUMENT;
		}
		ID3D11Texture2D *backBuffer = 0;
		const HRESULT result = m_swapChain->GetBuffer(0,
			__uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&backBuffer));
		if (FAILED(result))
		{
			return TranslateResult(result);
		}
		D3D11_TEXTURE2D_DESC descriptor;
		backBuffer->GetDesc(&descriptor);
		backBuffer->Release();
		const RenderFormat format = TranslateBackBufferFormat(descriptor.Format);
		if (format == RENDER_FORMAT_UNKNOWN || descriptor.Width == 0 ||
			descriptor.Height == 0 || descriptor.SampleDesc.Count != 1)
		{
			return RENDER_RESULT_UNSUPPORTED;
		}
		info->width = descriptor.Width;
		info->height = descriptor.Height;
		info->format = format;
		return RENDER_RESULT_OK;
	}

	virtual RenderResult beginFrame()
	{
		if (!isOwner() || m_frameOpen)
		{
			return RENDER_RESULT_INVALID_ARGUMENT;
		}
		if (m_renderTarget != 0)
		{
			m_context->OMSetRenderTargets(1, &m_renderTarget, m_depthStencil);
		}
		else
		{
			// A headless device may have used a logical target in the previous
			// frame.  Do not leave that native RTV bound when the neutral frame
			// begins without a swap-chain output.
			m_context->OMSetRenderTargets(0, 0, 0);
		}
		m_activeRenderTarget = m_renderTarget;
		m_activeDepthStencil = m_depthStencil;
		m_activeColorResource = m_renderTargetResource;
		m_activeDepthResource = m_depthTexture;
		m_renderTargetsBound = m_renderTarget != 0;
		bool needsTextureReset = !m_textureBindingsValid;
		for (unsigned int stage = 0; !needsTextureReset &&
			stage < LEGACY_TEXTURE_STAGE_COUNT; ++stage)
		{
			needsTextureReset = m_boundTextures[stage].isValid();
		}
		if (needsTextureReset)
		{
			unbindTextureResources();
		}
		m_pipelineStateValid = false;
		m_transformConstantsValid = false;
		m_pipelineBound = false;
		m_vertexBufferBound = false;
		m_indexBufferBound = false;
		m_topologyBound = false;
		m_frameOpen = true;
		return RENDER_RESULT_OK;
	}

	virtual RenderResult updateBuffer(GpuHandle buffer, const void *data,
		size_t byteCount, size_t destinationOffset,
		RenderBufferUpdateMode mode)
	{
		if (!m_frameOpen)
		{
			return RENDER_RESULT_INVALID_ARGUMENT;
		}
		return updateBufferResource(buffer, data, byteCount,
			destinationOffset, mode);
	}

	virtual RenderResult updateBufferResource(GpuHandle buffer,
		const void *data, size_t byteCount, size_t destinationOffset,
		RenderBufferUpdateMode mode)
	{
		if (!isOwner() || data == 0 || byteCount == 0 ||
			!m_handles->isLive(buffer))
		{
			return RENDER_RESULT_INVALID_ARGUMENT;
		}
		ResourceSlot &slot = m_resources[buffer.index()];
		if (slot.kind != RESOURCE_BUFFER || destinationOffset > slot.byteCount ||
			byteCount > slot.byteCount - destinationOffset)
		{
			return RENDER_RESULT_INVALID_ARGUMENT;
		}
		if (slot.usage == RENDER_USAGE_IMMUTABLE)
		{
			return RENDER_RESULT_UNSUPPORTED;
		}
		if (mode != RENDER_BUFFER_UPDATE_PRESERVE &&
			mode != RENDER_BUFFER_UPDATE_DISCARD &&
			mode != RENDER_BUFFER_UPDATE_NO_OVERWRITE)
		{
			return RENDER_RESULT_INVALID_ARGUMENT;
		}
		const bool rangeUpdate = mode != RENDER_BUFFER_UPDATE_PRESERVE;
		if (rangeUpdate && (slot.usage != RENDER_USAGE_DYNAMIC ||
			(slot.binding != RENDER_BUFFER_VERTEX &&
			 slot.binding != RENDER_BUFFER_INDEX) ||
			(mode == RENDER_BUFFER_UPDATE_DISCARD && destinationOffset != 0)))
		{
			return RENDER_RESULT_INVALID_ARGUMENT;
		}
		memcpy(&slot.shadow[destinationOffset], data, byteCount);
		if (slot.usage == RENDER_USAGE_DYNAMIC)
		{
			D3D11_MAPPED_SUBRESOURCE mapped;
			const D3D11_MAP mapMode = mode == RENDER_BUFFER_UPDATE_NO_OVERWRITE ?
				D3D11_MAP_WRITE_NO_OVERWRITE : D3D11_MAP_WRITE_DISCARD;
			const HRESULT result = m_context->Map(slot.resource, 0,
				mapMode, 0, &mapped);
			if (FAILED(result))
			{
				return TranslateResult(result);
			}
			if (mapped.pData == 0)
			{
				m_context->Unmap(slot.resource, 0);
				return RENDER_RESULT_FAILED;
			}
			if (mode == RENDER_BUFFER_UPDATE_PRESERVE)
			{
				memcpy(mapped.pData, &slot.shadow[0], slot.byteCount);
			}
			else
			{
				memcpy(static_cast<unsigned char *>(mapped.pData) +
					destinationOffset, data, byteCount);
			}
			m_context->Unmap(slot.resource, 0);
			return RENDER_RESULT_OK;
		}

		D3D11_BOX destination;
		destination.left = static_cast<UINT>(destinationOffset);
		destination.right = static_cast<UINT>(destinationOffset + byteCount);
		destination.top = 0;
		destination.bottom = 1;
		destination.front = 0;
		destination.back = 1;
		m_context->UpdateSubresource(slot.resource, 0, &destination, data, 0, 0);
		return RENDER_RESULT_OK;
	}

	virtual RenderResult clear(const RenderFloat4 &color, float depth,
		unsigned int stencil)
	{
		unsigned int clearFlags = 0;
		if (m_activeRenderTarget != 0)
		{
			clearFlags |= RENDER_CLEAR_COLOR;
		}
		if (m_activeDepthStencil != 0)
		{
			clearFlags |= RENDER_CLEAR_DEPTH | RENDER_CLEAR_STENCIL;
		}
		return clearTargets(clearFlags, color, depth, stencil);
	}

	virtual RenderResult clearTargets(unsigned int clearFlags,
		const RenderFloat4 &color, float depth, unsigned int stencil)
	{
		const unsigned int validFlags = RENDER_CLEAR_COLOR |
			RENDER_CLEAR_DEPTH | RENDER_CLEAR_STENCIL;
		const bool clearColor = (clearFlags & RENDER_CLEAR_COLOR) != 0;
		const bool clearDepthStencil =
			(clearFlags & (RENDER_CLEAR_DEPTH | RENDER_CLEAR_STENCIL)) != 0;
		if (!isOwner() || !m_frameOpen || clearFlags == 0 ||
			(clearFlags & ~validFlags) != 0 ||
			(clearColor && m_activeRenderTarget == 0) ||
			(clearDepthStencil && m_activeDepthStencil == 0) ||
			((clearFlags & RENDER_CLEAR_DEPTH) != 0 &&
				(depth < 0.0f || depth > 1.0f)))
		{
			return RENDER_RESULT_INVALID_ARGUMENT;
		}
		const float nativeColor[4] = { color.x, color.y, color.z, color.w };
		if (clearColor)
		{
			m_context->ClearRenderTargetView(m_activeRenderTarget, nativeColor);
		}
		if (clearDepthStencil)
		{
			UINT nativeFlags = 0;
			if ((clearFlags & RENDER_CLEAR_DEPTH) != 0)
			{
				nativeFlags |= D3D11_CLEAR_DEPTH;
			}
			if ((clearFlags & RENDER_CLEAR_STENCIL) != 0)
			{
				nativeFlags |= D3D11_CLEAR_STENCIL;
			}
			m_context->ClearDepthStencilView(m_activeDepthStencil,
				nativeFlags, depth,
				static_cast<UINT8>(stencil));
		}
		return RENDER_RESULT_OK;
	}

	virtual RenderResult setRenderTargets(const RenderTargetBinding &binding)
	{
		if (!isOwner() || !m_frameOpen)
		{
			return RENDER_RESULT_INVALID_ARGUMENT;
		}
		if (binding.useBackBufferColor && binding.hasColor)
		{
			return RENDER_RESULT_INVALID_ARGUMENT;
		}
		if (binding.useBackBufferDepth && binding.hasDepth)
		{
			return RENDER_RESULT_INVALID_ARGUMENT;
		}
		if (binding.hasColor && !binding.color.resource.isValid())
		{
			return RENDER_RESULT_INVALID_ARGUMENT;
		}
		if (binding.hasDepth && !binding.depth.resource.isValid())
		{
			return RENDER_RESULT_INVALID_ARGUMENT;
		}
		if (binding.hasColor && (binding.color.mip != 0 ||
			binding.color.arraySlice != 0))
		{
			return RENDER_RESULT_UNSUPPORTED;
		}
		if (binding.hasDepth && (binding.depth.mip != 0 ||
			binding.depth.arraySlice != 0))
		{
			return RENDER_RESULT_UNSUPPORTED;
		}

		if (!binding.useBackBufferColor && !binding.hasColor &&
			!binding.useBackBufferDepth && !binding.hasDepth)
		{
			if (m_renderTargetsBound && m_activeRenderTarget == 0 &&
				m_activeDepthStencil == 0 && m_textureBindingsValid)
			{
				return RENDER_RESULT_OK;
			}
			if (!m_textureBindingsValid)
			{
				unbindTextureResources();
			}
			if (!m_renderTargetsBound || m_activeRenderTarget != 0 ||
				m_activeDepthStencil != 0)
			{
				m_context->OMSetRenderTargets(0, 0, 0);
			}
			m_activeRenderTarget = 0;
			m_activeDepthStencil = 0;
			m_activeColorResource = 0;
			m_activeDepthResource = 0;
			m_renderTargetsBound = true;
			return RENDER_RESULT_OK;
		}

		ID3D11RenderTargetView *colorView = 0;
		ID3D11Resource *colorResource = 0;
		if (binding.useBackBufferColor)
		{
			if (m_renderTarget == 0)
			{
				return RENDER_RESULT_UNSUPPORTED;
			}
			colorView = m_renderTarget;
			colorResource = m_renderTargetResource;
		}
		else if (binding.hasColor)
		{
			const GpuHandle colorTarget = binding.color.resource;
			if (!m_handles->isLive(colorTarget))
			{
				return RENDER_RESULT_INVALID_ARGUMENT;
			}
			ResourceSlot &colorSlot = m_resources[colorTarget.index()];
			if (colorSlot.kind != RESOURCE_TEXTURE ||
				colorSlot.renderTarget == 0)
			{
				return RENDER_RESULT_INVALID_ARGUMENT;
			}
			colorView = colorSlot.renderTarget;
			colorResource = colorSlot.resource;
		}

		ID3D11DepthStencilView *depthView = 0;
		ID3D11Resource *depthResource = 0;
		if (binding.useBackBufferDepth)
		{
			if (m_depthStencil == 0)
			{
				return RENDER_RESULT_UNSUPPORTED;
			}
			depthView = m_depthStencil;
			depthResource = m_depthTexture;
		}
		else if (binding.hasDepth)
		{
			const GpuHandle depthTarget = binding.depth.resource;
			if (!m_handles->isLive(depthTarget))
			{
				return RENDER_RESULT_INVALID_ARGUMENT;
			}
			ResourceSlot &depthSlot = m_resources[depthTarget.index()];
			if (depthSlot.kind != RESOURCE_TEXTURE ||
				depthSlot.depthStencil == 0)
			{
				return RENDER_RESULT_INVALID_ARGUMENT;
			}
			depthView = depthSlot.depthStencil;
			depthResource = depthSlot.resource;
		}
		if (colorResource != 0 && colorResource == depthResource)
		{
			return RENDER_RESULT_INVALID_ARGUMENT;
		}
		if (colorResource != 0 && depthResource != 0)
		{
			D3D11_RESOURCE_DIMENSION colorDimension;
			D3D11_RESOURCE_DIMENSION depthDimension;
			colorResource->GetType(&colorDimension);
			depthResource->GetType(&depthDimension);
			if (colorDimension != D3D11_RESOURCE_DIMENSION_TEXTURE2D ||
				depthDimension != D3D11_RESOURCE_DIMENSION_TEXTURE2D)
			{
				return RENDER_RESULT_UNSUPPORTED;
			}

			ID3D11Texture2D *colorTexture = 0;
			ID3D11Texture2D *depthTexture = 0;
			HRESULT result = colorResource->QueryInterface(__uuidof(ID3D11Texture2D),
				reinterpret_cast<void **>(&colorTexture));
			if (SUCCEEDED(result))
			{
				result = depthResource->QueryInterface(__uuidof(ID3D11Texture2D),
					reinterpret_cast<void **>(&depthTexture));
			}
			if (FAILED(result) || colorTexture == 0 || depthTexture == 0)
			{
				if (depthTexture != 0)
				{
					depthTexture->Release();
				}
				if (colorTexture != 0)
				{
					colorTexture->Release();
				}
				return TranslateResult(FAILED(result) ? result : E_NOINTERFACE);
			}

			D3D11_TEXTURE2D_DESC colorDescriptor;
			D3D11_TEXTURE2D_DESC depthDescriptor;
			colorTexture->GetDesc(&colorDescriptor);
			depthTexture->GetDesc(&depthDescriptor);
			const bool compatible =
				colorDescriptor.Width == depthDescriptor.Width &&
				colorDescriptor.Height == depthDescriptor.Height &&
				colorDescriptor.SampleDesc.Count ==
					depthDescriptor.SampleDesc.Count &&
				colorDescriptor.SampleDesc.Quality ==
					depthDescriptor.SampleDesc.Quality;
			depthTexture->Release();
			colorTexture->Release();
			if (!compatible)
			{
				return RENDER_RESULT_INVALID_ARGUMENT;
			}
		}
		// Once a logical resource is bound as an output, its CPU shadow is no
		// longer authoritative: a later draw or clear may modify it entirely on
		// the GPU.  Record that fact before the fast path below so device
		// recovery never restores a custom color/depth target from stale upload
		// bytes.  The replacement is cleared by recreateTexture() when no valid
		// CPU image remains.
		if (binding.hasColor)
		{
			ResourceSlot &colorSlot = m_resources[
				binding.color.resource.index()];
			colorSlot.gpuAuthoritative = true;
		}
		if (binding.hasDepth)
		{
			ResourceSlot &depthSlot = m_resources[
				binding.depth.resource.index()];
			depthSlot.gpuAuthoritative = true;
		}
		const bool sameTargets = m_renderTargetsBound &&
			m_activeRenderTarget == colorView &&
			m_activeDepthStencil == depthView &&
			m_activeColorResource == colorResource &&
			m_activeDepthResource == depthResource;
		if (sameTargets && m_textureBindingsValid)
		{
			return RENDER_RESULT_OK;
		}
		// OMSetRenderTargets implicitly unbinds any SRV that aliases a new
		// output resource.  Invalidate those cached stages before changing
		// targets so a later setTexture call cannot incorrectly suppress the
		// required rebind, while preserving unrelated SRVs across the transition.
		if (!sameTargets)
		{
			invalidateTextureBindingsForTargets(colorResource, depthResource);
		}
		else if (!m_textureBindingsValid)
		{
			unbindTextureResources();
		}
		if (!sameTargets)
		{
			m_context->OMSetRenderTargets(colorView == 0 ? 0 : 1,
				colorView == 0 ? 0 : &colorView, depthView);
		}
		m_activeRenderTarget = colorView;
		m_activeDepthStencil = depthView;
		m_activeColorResource = colorResource;
		m_activeDepthResource = depthResource;
		m_renderTargetsBound = true;
		return RENDER_RESULT_OK;
	}

	virtual RenderResult setRenderTargets(GpuHandle colorTarget,
		GpuHandle depthTarget)
	{
		RenderTargetBinding binding;
		// Preserve the original two-handle contract: two invalid handles restore
		// the swap-chain targets, while one valid handle binds only that custom
		// attachment.  Callers that need a custom color plus default depth use the
		// explicit RenderTargetBinding overload.
		if (!colorTarget.isValid() && !depthTarget.isValid())
		{
			return setRenderTargets(binding);
		}
		binding.useBackBufferColor = false;
		binding.useBackBufferDepth = false;
		if (colorTarget.isValid())
		{
			binding.hasColor = true;
			binding.color.resource = colorTarget;
		}
		if (depthTarget.isValid())
		{
			binding.hasDepth = true;
			binding.depth.resource = depthTarget;
		}
		return setRenderTargets(binding);
	}

	virtual RenderResult setViewport(float x, float y, float width,
		float height, float minimumDepth, float maximumDepth)
	{
		if (!isOwner() || !m_frameOpen || width <= 0.0f || height <= 0.0f ||
			minimumDepth < 0.0f || maximumDepth > 1.0f ||
			minimumDepth > maximumDepth)
		{
			return RENDER_RESULT_INVALID_ARGUMENT;
		}
		if (m_viewportBound && m_viewportX == x && m_viewportY == y &&
			m_viewportWidth == width && m_viewportHeight == height &&
			m_viewportMinimumDepth == minimumDepth &&
			m_viewportMaximumDepth == maximumDepth)
		{
			return RENDER_RESULT_OK;
		}
		D3D11_VIEWPORT viewport;
		viewport.TopLeftX = x;
		viewport.TopLeftY = y;
		viewport.Width = width;
		viewport.Height = height;
		viewport.MinDepth = minimumDepth;
		viewport.MaxDepth = maximumDepth;
		m_context->RSSetViewports(1, &viewport);
		m_viewportX = x;
		m_viewportY = y;
		m_viewportWidth = width;
		m_viewportHeight = height;
		m_viewportMinimumDepth = minimumDepth;
		m_viewportMaximumDepth = maximumDepth;
		m_viewportBound = true;
		m_cachedLegacyStateValid = false;
		return RENDER_RESULT_OK;
	}

	virtual RenderResult setLegacyState(const LegacyLogicalState &state,
		LegacyVertexFormat vertexFormat, unsigned int texturePresenceMask)
	{
		if (!isOwner() || !m_frameOpen)
		{
			return RENDER_RESULT_INVALID_ARGUMENT;
		}
		const bool hasTextures = texturePresenceMask != 0;
		const bool useFullPipeline = hasTextures || vertexFormat ==
			RENDER_VERTEX_POSITION3_NORMAL_COLOR_TEX1;
		// The textured shader has inputs for every legacy material/texture
		// channel.  There is no safe canonical vertex declaration for the
		// compact LegacyVertexFormat enum: fabricating COLOR1 or TEXCOORD1..7
		// offsets would read unrelated bytes (or beyond the caller's stride).
		// The bridge always supplies the actual declaration through
		// setLegacyStateForLayout; reject the ambiguous fallback instead of
		// silently binding an inferred declaration.
		if (useFullPipeline && !m_hasInputLayoutOverride)
		{
			return RENDER_RESULT_UNSUPPORTED;
		}
		const bool useSeaWavePipeline = state.pipeline.pixelProgram ==
			RENDER_LEGACY_PIXEL_WATER_SEA || state.pipeline.vertexProgram ==
			RENDER_LEGACY_VERTEX_WATER_SEA;
		if (useSeaWavePipeline &&
			(state.pipeline.pixelProgram != RENDER_LEGACY_PIXEL_WATER_SEA ||
			state.pipeline.vertexProgram != RENDER_LEGACY_VERTEX_WATER_SEA ||
			(texturePresenceMask & 0x03U) != 0x03U))
		{
			return RENDER_RESULT_INVALID_ARGUMENT;
		}
		if ((!useFullPipeline && vertexFormat != RENDER_VERTEX_POSITION3_COLOR) ||
			(useFullPipeline && vertexFormat !=
				RENDER_VERTEX_POSITION3_NORMAL_COLOR_TEX1))
		{
			return RENDER_RESULT_UNSUPPORTED;
		}
		const unsigned int vertexLayoutFlags = m_hasVertexLayoutFlagsOverride ?
			m_vertexLayoutFlagsOverride : (useFullPipeline ? 0x10bU : 0x02U);
		ID3D11InputLayout *inputLayout = m_hasInputLayoutOverride ?
			m_inputLayoutOverride : (useSeaWavePipeline ? m_seaWaveLayout :
			m_positionColorLayout);
		if (m_pipelineStateValid && m_pipelineBound &&
			m_cachedLegacyStateValid &&
			m_cachedLegacyVertexFormat == vertexFormat &&
			m_cachedLegacyTexturePresenceMask == texturePresenceMask &&
			m_cachedLegacyCubeTextureMask == m_boundCubeTextureMask &&
			m_cachedLegacySignedTextureMask == m_boundSignedTextureMask &&
			m_cachedLegacyVertexLayoutFlags == vertexLayoutFlags &&
			m_cachedLegacyInputLayout == inputLayout &&
			memcmp(m_cachedLegacyState, &state, sizeof(state)) == 0)
		{
			return RENDER_RESULT_OK;
		}
		if (m_pipelineStateValid && m_pipelineBound &&
			m_cachedLegacyPipelineValid &&
			m_cachedLegacyVertexFormat == vertexFormat &&
			m_cachedLegacyTexturePresenceMask == texturePresenceMask &&
			m_cachedLegacyCubeTextureMask == m_boundCubeTextureMask &&
			m_cachedLegacySignedTextureMask == m_boundSignedTextureMask &&
			m_cachedLegacyVertexLayoutFlags == vertexLayoutFlags &&
			m_cachedLegacyInputLayout == inputLayout &&
			memcmp(m_cachedLegacyPipeline, &state.pipeline,
				sizeof(state.pipeline)) == 0)
		{
			const HRESULT transformResult = updateTransformConstants(state,
				vertexLayoutFlags, texturePresenceMask, m_boundCubeTextureMask);
			if (FAILED(transformResult))
			{
				return TranslateResult(transformResult);
			}
			m_transformConstantsChanged = false;
			cacheLegacyState(state, vertexFormat, texturePresenceMask,
				vertexLayoutFlags, inputLayout);
			return RENDER_RESULT_OK;
		}
		if (state.pipeline.ambientMaterialSource >
			RENDER_MATERIAL_SOURCE_COLOR2 ||
			state.pipeline.diffuseMaterialSource >
				RENDER_MATERIAL_SOURCE_COLOR2 ||
			state.pipeline.emissiveMaterialSource >
				RENDER_MATERIAL_SOURCE_COLOR2 ||
			state.pipeline.specularMaterialSource >
				RENDER_MATERIAL_SOURCE_COLOR2)
		{
			return RENDER_RESULT_INVALID_ARGUMENT;
		}
		if ((state.pipeline.clipPlaneEnableMask &
				~((1U << LEGACY_CLIP_PLANE_COUNT) - 1U)) != 0U)
		{
			return RENDER_RESULT_INVALID_ARGUMENT;
		}
		if (!useFullPipeline && state.pipeline.lightingEnable)
		{
			return RENDER_RESULT_UNSUPPORTED;
		}
		for (unsigned int stage = 0; useFullPipeline &&
			stage < LEGACY_TEXTURE_STAGE_COUNT; ++stage)
		{
			if (state.pipeline.textureStages[stage].colorOperation >
					RENDER_TEXTURE_OP_LINEAR_INTERPOLATE ||
				state.pipeline.textureStages[stage].alphaOperation >
					RENDER_TEXTURE_OP_LINEAR_INTERPOLATE)
			{
				return RENDER_RESULT_UNSUPPORTED;
			}
		}
		// Clip equations are published in world space.  The fixed-function
		// shader deliberately does not reinterpret them for POSITIONT data;
		// reject that combination instead of silently drawing with clipping
		// disabled.
		if (state.pipeline.clipPlaneEnableMask != 0U &&
			(vertexLayoutFlags & LEGACY_VERTEX_LAYOUT_PRETRANSFORMED) != 0U)
		{
			return RENDER_RESULT_UNSUPPORTED;
		}
		const HRESULT transformResult = updateTransformConstants(state,
			vertexLayoutFlags, texturePresenceMask, m_boundCubeTextureMask);
		if (FAILED(transformResult))
		{
			return TranslateResult(transformResult);
		}

		D3D11_BLEND_DESC blendDescriptor;
		memset(&blendDescriptor, 0, sizeof(blendDescriptor));
		D3D11_RENDER_TARGET_BLEND_DESC &target = blendDescriptor.RenderTarget[0];
		target.BlendEnable = state.pipeline.blend.blendEnable;
		target.SrcBlend = TranslateBlend(state.pipeline.blend.sourceColor);
		target.DestBlend = TranslateBlend(state.pipeline.blend.destinationColor);
		target.BlendOp = TranslateBlendOperation(state.pipeline.blend.colorOperation);
		target.SrcBlendAlpha = TranslateAlphaBlend(
			state.pipeline.blend.sourceAlpha);
		target.DestBlendAlpha = TranslateAlphaBlend(
			state.pipeline.blend.destinationAlpha);
		target.BlendOpAlpha = TranslateBlendOperation(
			state.pipeline.blend.alphaOperation);
		target.RenderTargetWriteMask =
			static_cast<UINT8>(state.pipeline.blend.colorWriteMask & 0x0fU);
		ID3D11BlendState *blendState = 0;
		HRESULT result = findOrCreateBlendState(blendDescriptor, &blendState);
		if (FAILED(result))
		{
			return TranslateResult(result);
		}

		D3D11_DEPTH_STENCIL_DESC depthDescriptor;
		memset(&depthDescriptor, 0, sizeof(depthDescriptor));
		depthDescriptor.DepthEnable = state.pipeline.depthStencil.depthEnable;
		depthDescriptor.DepthWriteMask = state.pipeline.depthStencil.depthWrite ?
			D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
		depthDescriptor.DepthFunc = TranslateComparison(
			state.pipeline.depthStencil.depthFunction);
		depthDescriptor.StencilEnable = state.pipeline.depthStencil.stencilEnable;
		depthDescriptor.StencilReadMask = static_cast<UINT8>(
			state.pipeline.depthStencil.stencilReadMask);
		depthDescriptor.StencilWriteMask = static_cast<UINT8>(
			state.pipeline.depthStencil.stencilWriteMask);
		depthDescriptor.FrontFace.StencilFunc = TranslateComparison(
			state.pipeline.depthStencil.stencilFunction);
		depthDescriptor.FrontFace.StencilFailOp = TranslateStencilOperation(
			state.pipeline.depthStencil.stencilFail);
		depthDescriptor.FrontFace.StencilDepthFailOp = TranslateStencilOperation(
			state.pipeline.depthStencil.stencilDepthFail);
		depthDescriptor.FrontFace.StencilPassOp = TranslateStencilOperation(
			state.pipeline.depthStencil.stencilPass);
		depthDescriptor.BackFace = depthDescriptor.FrontFace;
		ID3D11DepthStencilState *depthState = 0;
		result = findOrCreateDepthState(depthDescriptor, &depthState);
		if (FAILED(result))
		{
			return TranslateResult(result);
		}

		D3D11_RASTERIZER_DESC rasterizerDescriptor;
		memset(&rasterizerDescriptor, 0, sizeof(rasterizerDescriptor));
		rasterizerDescriptor.FillMode = state.pipeline.rasterizer.fillMode ==
			RENDER_FILL_WIREFRAME ? D3D11_FILL_WIREFRAME : D3D11_FILL_SOLID;
		switch (state.pipeline.rasterizer.cullMode)
		{
		case RENDER_CULL_NONE:
			rasterizerDescriptor.CullMode = D3D11_CULL_NONE;
			break;
		case RENDER_CULL_FRONT:
			rasterizerDescriptor.CullMode = D3D11_CULL_FRONT;
			break;
		default:
			rasterizerDescriptor.CullMode = D3D11_CULL_BACK;
			break;
		}
		rasterizerDescriptor.FrontCounterClockwise =
			state.pipeline.rasterizer.frontCounterClockwise;
		rasterizerDescriptor.DepthBias = state.pipeline.rasterizer.depthBias;
		rasterizerDescriptor.SlopeScaledDepthBias =
			state.pipeline.rasterizer.slopeScaledDepthBias;
		rasterizerDescriptor.DepthClipEnable = true;
		rasterizerDescriptor.ScissorEnable =
			state.pipeline.rasterizer.scissorEnable;
		ID3D11RasterizerState *rasterizerState = 0;
		result = findOrCreateRasterizerState(rasterizerDescriptor, &rasterizerState);
		if (FAILED(result))
		{
			return TranslateResult(result);
		}
		ID3D11SamplerState *samplerStates[LEGACY_TEXTURE_STAGE_COUNT] = { 0 };
		if (hasTextures)
		{
			for (unsigned int stage = 0; stage < LEGACY_TEXTURE_STAGE_COUNT;
				++stage)
			{
				const LegacySamplerState &sampler =
					state.pipeline.textureStages[stage].sampler;
				D3D11_SAMPLER_DESC samplerDescriptor;
				memset(&samplerDescriptor, 0, sizeof(samplerDescriptor));
				samplerDescriptor.Filter = TranslateFilter(sampler);
				samplerDescriptor.AddressU = TranslateAddressMode(sampler.addressU);
				samplerDescriptor.AddressV = TranslateAddressMode(sampler.addressV);
				samplerDescriptor.AddressW = TranslateAddressMode(sampler.addressW);
				samplerDescriptor.MipLODBias = sampler.mipLodBias;
				samplerDescriptor.MaxAnisotropy =
					sampler.maximumAnisotropy == 0 ? 1 : sampler.maximumAnisotropy;
				if (samplerDescriptor.MaxAnisotropy > 16)
				{
					samplerDescriptor.MaxAnisotropy = 16;
				}
				samplerDescriptor.ComparisonFunc = D3D11_COMPARISON_NEVER;
				samplerDescriptor.BorderColor[0] = sampler.borderColor.x;
				samplerDescriptor.BorderColor[1] = sampler.borderColor.y;
				samplerDescriptor.BorderColor[2] = sampler.borderColor.z;
				samplerDescriptor.BorderColor[3] = sampler.borderColor.w;
				// D3DTEXF_NONE disables mip selection altogether.  D3D11 has no
				// separate "no mip" filter bit; pinning both LOD bounds to the base
				// level is the equivalent operation.  Treating NONE as POINT alone
				// still selects lower mip levels under minification and makes smudge
				// and UI surfaces shimmer.
				if (sampler.mipmapping == RENDER_TEXTURE_FILTER_NONE)
				{
					samplerDescriptor.MinLOD = 0.0f;
					samplerDescriptor.MaxLOD = 0.0f;
				}
				else
				{
					// D3DTSS_MAXMIPLEVEL excludes higher-detail levels.  D3D11's
					// equivalent is a minimum LOD, not a maximum LOD.
					samplerDescriptor.MinLOD =
						static_cast<float>(sampler.maximumMipLevel);
					samplerDescriptor.MaxLOD = FLT_MAX;
				}
				result = findOrCreateSamplerState(samplerDescriptor,
					&samplerStates[stage]);
				if (FAILED(result))
				{
					return TranslateResult(result);
				}
			}
		}

		ID3D11VertexShader *vertexShader = useSeaWavePipeline ?
			m_seaWaveVertexShader : (useFullPipeline ?
			m_texturedVertexShader : m_vertexShader);
		ID3D11PixelShader *pixelShader = useSeaWavePipeline ?
			m_seaWavePixelShader : (useFullPipeline ?
			m_texturedPixelShader : m_pixelShader);
		if (useFullPipeline && state.pipeline.pixelProgram ==
			RENDER_LEGACY_PIXEL_FIXED_FUNCTION)
		{
			const unsigned int activeStages =
				CountActiveFixedFunctionStages(state.pipeline);
			if (activeStages <= 1)
			{
				pixelShader = m_texturedFixed1PixelShader;
			}
			else if (activeStages == 2)
			{
				pixelShader = m_texturedFixed2PixelShader;
			}
		}
		if (useFullPipeline && state.pipeline.pixelProgram ==
			RENDER_LEGACY_PIXEL_WATER_FLAT)
		{
			pixelShader = m_waterFlatPixelShader;
		}
		else if (useFullPipeline && state.pipeline.pixelProgram ==
			RENDER_LEGACY_PIXEL_WATER_RIVER)
		{
			pixelShader = m_waterRiverPixelShader;
		}
		else if (useFullPipeline && state.pipeline.pixelProgram >=
			RENDER_LEGACY_PIXEL_TERRAIN_BASE &&
			state.pipeline.pixelProgram <=
			RENDER_LEGACY_PIXEL_FLAT_TERRAIN_NOISE2)
		{
			pixelShader = m_terrainPixelShaders[
				state.pipeline.pixelProgram - RENDER_LEGACY_PIXEL_TERRAIN_BASE];
		}
		if (CanDisableLegacyPixelShader(state.pipeline))
		{
			pixelShader = 0;
		}
		const bool pipelineMatches = m_pipelineStateValid &&
			!m_transformConstantsChanged &&
			memcmp(&m_boundBlendDescriptor, &blendDescriptor,
				sizeof(blendDescriptor)) == 0 &&
			memcmp(&m_boundDepthDescriptor, &depthDescriptor,
				sizeof(depthDescriptor)) == 0 &&
			memcmp(&m_boundRasterizerDescriptor, &rasterizerDescriptor,
				sizeof(rasterizerDescriptor)) == 0 &&
			m_boundStencilReference ==
				state.pipeline.depthStencil.stencilReference &&
			m_boundBlendState == blendState &&
			m_boundDepthState == depthState &&
			m_boundRasterizerState == rasterizerState &&
			m_boundInputLayout == inputLayout &&
			m_boundVertexShader == vertexShader &&
			m_boundPixelShader == pixelShader &&
			m_pipelineHasTextures == hasTextures &&
			(!hasTextures || memcmp(m_boundSamplerStates, samplerStates,
				sizeof(samplerStates)) == 0);
		if (pipelineMatches)
		{
			m_pipelineBound = true;
			cacheLegacyState(state, vertexFormat, texturePresenceMask,
				vertexLayoutFlags, inputLayout);
			return RENDER_RESULT_OK;
		}

		const bool forcePipeline = !m_pipelineStateValid;
		const float blendFactor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
		if (forcePipeline || memcmp(&m_boundBlendDescriptor, &blendDescriptor,
			sizeof(blendDescriptor)) != 0 || m_boundBlendState != blendState)
		{
			m_context->OMSetBlendState(blendState, blendFactor, 0xffffffffU);
		}
		if (forcePipeline || memcmp(&m_boundDepthDescriptor, &depthDescriptor,
			sizeof(depthDescriptor)) != 0 || m_boundDepthState != depthState ||
			m_boundStencilReference !=
				state.pipeline.depthStencil.stencilReference)
		{
			m_context->OMSetDepthStencilState(depthState,
				state.pipeline.depthStencil.stencilReference);
		}
		if (forcePipeline || memcmp(&m_boundRasterizerDescriptor,
			&rasterizerDescriptor, sizeof(rasterizerDescriptor)) != 0 ||
			m_boundRasterizerState != rasterizerState)
		{
			m_context->RSSetState(rasterizerState);
		}
		if (forcePipeline || m_boundInputLayout != inputLayout)
		{
			m_context->IASetInputLayout(inputLayout);
		}
		if (forcePipeline || m_boundVertexShader != vertexShader)
		{
			m_context->VSSetShader(vertexShader, 0, 0);
		}
		if (forcePipeline || m_boundPixelShader != pixelShader)
		{
			m_context->PSSetShader(pixelShader, 0, 0);
		}
		if (hasTextures && (forcePipeline || !m_pipelineHasTextures ||
			memcmp(m_boundSamplerStates, samplerStates,
				sizeof(samplerStates)) != 0))
		{
			m_context->PSSetSamplers(0, LEGACY_TEXTURE_STAGE_COUNT,
				samplerStates);
		}
		m_boundBlendDescriptor = blendDescriptor;
		m_boundDepthDescriptor = depthDescriptor;
		m_boundRasterizerDescriptor = rasterizerDescriptor;
		m_boundStencilReference =
			state.pipeline.depthStencil.stencilReference;
		m_boundBlendState = blendState;
		m_boundDepthState = depthState;
		m_boundRasterizerState = rasterizerState;
		m_boundInputLayout = inputLayout;
		m_boundVertexShader = vertexShader;
		m_boundPixelShader = pixelShader;
		m_pipelineHasTextures = hasTextures;
		if (hasTextures)
		{
			memcpy(m_boundSamplerStates, samplerStates,
				sizeof(m_boundSamplerStates));
		}
		m_pipelineBound = true;
		m_pipelineStateValid = true;
		m_transformConstantsChanged = false;
		cacheLegacyState(state, vertexFormat, texturePresenceMask,
			vertexLayoutFlags, inputLayout);
		return RENDER_RESULT_OK;
	}

	virtual RenderResult setLegacyStateForLayout(const LegacyLogicalState &state,
		const LegacyVertexLayout &vertexLayout,
		unsigned int texturePresenceMask)
	{
		if (!isOwner() || !m_frameOpen)
		{
			return RENDER_RESULT_INVALID_ARGUMENT;
		}
		ID3D11InputLayout *inputLayout = 0;
		HRESULT layoutResult = S_OK;
		if (state.pipeline.vertexProgram == RENDER_LEGACY_VERTEX_WATER_SEA)
		{
			if (state.pipeline.pixelProgram != RENDER_LEGACY_PIXEL_WATER_SEA ||
			!IsSeaWaveVertexLayout(vertexLayout) || m_seaWaveLayout == 0)
			{
				return RENDER_RESULT_INVALID_ARGUMENT;
			}
			inputLayout = m_seaWaveLayout;
		}
		else
		{
			layoutResult = findOrCreateInputLayout(vertexLayout,
				&inputLayout);
		}
		if (FAILED(layoutResult))
		{
			return TranslateResult(layoutResult);
		}
		unsigned int layoutFlags = 0;
		for (unsigned int index = 0; index < vertexLayout.elementCount; ++index)
		{
			switch (vertexLayout.elements[index].semantic)
			{
			case RENDER_VERTEX_SEMANTIC_NORMAL: layoutFlags |= 1U; break;
			case RENDER_VERTEX_SEMANTIC_DIFFUSE: layoutFlags |= 2U; break;
			case RENDER_VERTEX_SEMANTIC_SPECULAR: layoutFlags |= 4U; break;
			case RENDER_VERTEX_SEMANTIC_TEXTURE_COORDINATE:
				layoutFlags |= 1U << (8 +
					vertexLayout.elements[index].semanticIndex);
				break;
			default: break;
			}
		}
		if (vertexLayout.preTransformed)
		{
			layoutFlags |= LEGACY_VERTEX_LAYOUT_PRETRANSFORMED;
		}
		m_hasVertexLayoutFlagsOverride = true;
		m_vertexLayoutFlagsOverride = layoutFlags;
		m_hasInputLayoutOverride = true;
		m_inputLayoutOverride = inputLayout;
		const RenderResult stateResult = setLegacyState(state,
			RENDER_VERTEX_POSITION3_NORMAL_COLOR_TEX1, texturePresenceMask);
		m_hasVertexLayoutFlagsOverride = false;
		m_vertexLayoutFlagsOverride = 0;
		m_hasInputLayoutOverride = false;
		m_inputLayoutOverride = 0;
		return stateResult;
	}

	virtual RenderResult setVertexBuffer(GpuHandle buffer, unsigned int stride,
		unsigned int offset)
	{
		if (!isOwner() || !m_frameOpen || stride == 0 ||
			!m_handles->isLive(buffer))
		{
			return RENDER_RESULT_INVALID_ARGUMENT;
		}
		ResourceSlot &slot = m_resources[buffer.index()];
		if (slot.kind != RESOURCE_BUFFER ||
			(slot.binding & RENDER_BUFFER_VERTEX) == 0 || offset >= slot.byteCount)
		{
			return RENDER_RESULT_INVALID_ARGUMENT;
		}
		if (m_vertexBufferBound && m_boundVertexBuffer == buffer &&
			m_boundVertexStride == stride && m_boundVertexOffset == offset)
		{
			return RENDER_RESULT_OK;
		}
		ID3D11Buffer *nativeBuffer = static_cast<ID3D11Buffer *>(slot.resource);
		const UINT nativeStride = stride;
		const UINT nativeOffset = offset;
		m_context->IASetVertexBuffers(0, 1, &nativeBuffer, &nativeStride,
			&nativeOffset);
		m_vertexBufferBound = true;
		m_boundVertexBuffer = buffer;
		m_boundVertexStride = stride;
		m_boundVertexOffset = offset;
		return RENDER_RESULT_OK;
	}

	virtual RenderResult setIndexBuffer(GpuHandle buffer, RenderFormat format,
		unsigned int offset)
	{
		const DXGI_FORMAT nativeFormat = TranslateFormat(format);
		const unsigned int indexSize = format == RENDER_FORMAT_R16_UINT ? 2U :
			(format == RENDER_FORMAT_R32_UINT ? 4U : 0U);
		if (!isOwner() || !m_frameOpen || indexSize == 0 ||
			nativeFormat == DXGI_FORMAT_UNKNOWN || !m_handles->isLive(buffer))
		{
			return RENDER_RESULT_INVALID_ARGUMENT;
		}
		ResourceSlot &slot = m_resources[buffer.index()];
		if (slot.kind != RESOURCE_BUFFER ||
			(slot.binding & RENDER_BUFFER_INDEX) == 0 || offset >= slot.byteCount ||
			(offset % indexSize) != 0)
		{
			return RENDER_RESULT_INVALID_ARGUMENT;
		}
		if (m_indexBufferBound && m_boundIndexBuffer == buffer &&
			m_boundIndexFormat == format && m_boundIndexOffset == offset)
		{
			return RENDER_RESULT_OK;
		}
		ID3D11Buffer *nativeBuffer = static_cast<ID3D11Buffer *>(slot.resource);
		m_context->IASetIndexBuffer(nativeBuffer, nativeFormat, offset);
		m_indexBufferBound = true;
		m_boundIndexBuffer = buffer;
		m_boundIndexFormat = format;
		m_boundIndexOffset = offset;
		return RENDER_RESULT_OK;
	}

	virtual RenderResult setTexture(unsigned int stage, GpuHandle texture)
	{
		if (!isOwner() || !m_frameOpen || stage >= LEGACY_TEXTURE_STAGE_COUNT)
		{
			return RENDER_RESULT_INVALID_ARGUMENT;
		}
		if (!texture.isValid())
		{
			if (m_textureBindingsValid && !m_boundTextures[stage].isValid() &&
				(m_boundCubeTextureMask & (1U << stage)) == 0U &&
				(m_boundSignedTextureMask & (1U << stage)) == 0U)
			{
				return RENDER_RESULT_OK;
			}
			ID3D11ShaderResourceView *emptyView = 0;
			m_context->PSSetShaderResources(stage, 1, &emptyView);
			m_context->PSSetShaderResources(8 + stage, 1, &emptyView);
			m_boundTextures[stage] = GpuHandle();
			m_boundCubeTextureMask &= ~(1U << stage);
			m_boundSignedTextureMask &= ~(1U << stage);
			m_transformConstantsChanged = true;
			m_textureBindingsValid = true;
			return RENDER_RESULT_OK;
		}
		if (!m_handles->isLive(texture))
		{
			return RENDER_RESULT_INVALID_ARGUMENT;
		}
		ResourceSlot &slot = m_resources[texture.index()];
		if (slot.kind != RESOURCE_TEXTURE || slot.view == 0)
		{
			return RENDER_RESULT_INVALID_ARGUMENT;
		}
		// D3D11 forbids binding a resource as an SRV while any subresource of
		// the same resource is still bound as an output.  Compare resources,
		// rather than view pointers, so future mip/array views remain safe too.
		if (slot.resource == m_activeColorResource ||
			slot.resource == m_activeDepthResource)
		{
			return RENDER_RESULT_INVALID_ARGUMENT;
		}
		const bool isCube = slot.textureDescriptor.dimension ==
			RENDER_TEXTURE_CUBE;
		const bool isSigned = slot.textureDescriptor.format ==
			RENDER_FORMAT_R8G8_SNORM;
		const bool wasCube = (m_boundCubeTextureMask & (1U << stage)) != 0U;
		const bool wasSigned = (m_boundSignedTextureMask & (1U << stage)) != 0U;
		if (m_textureBindingsValid && m_boundTextures[stage] == texture &&
			wasCube == isCube && wasSigned == isSigned)
		{
			return RENDER_RESULT_OK;
		}
		ID3D11ShaderResourceView *emptyView = 0;
		if (isCube)
		{
			m_context->PSSetShaderResources(stage, 1, &emptyView);
			m_context->PSSetShaderResources(8 + stage, 1, &slot.view);
			m_boundCubeTextureMask |= 1U << stage;
		}
		else
		{
			m_context->PSSetShaderResources(stage, 1, &slot.view);
			m_context->PSSetShaderResources(8 + stage, 1, &emptyView);
			m_boundCubeTextureMask &= ~(1U << stage);
		}
		if (isSigned)
		{
			m_boundSignedTextureMask |= 1U << stage;
		}
		else
		{
			m_boundSignedTextureMask &= ~(1U << stage);
		}
		m_boundTextures[stage] = texture;
		m_textureBindingsValid = true;
		m_transformConstantsChanged = true;
		return RENDER_RESULT_OK;
	}

	virtual RenderResult setPrimitiveTopology(RenderPrimitiveTopology topology)
	{
		if (!isOwner() || !m_frameOpen)
		{
			return RENDER_RESULT_INVALID_ARGUMENT;
		}
		D3D11_PRIMITIVE_TOPOLOGY nativeTopology;
		switch (topology)
		{
		case RENDER_PRIMITIVE_TRIANGLE_LIST:
			nativeTopology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
			break;
		case RENDER_PRIMITIVE_TRIANGLE_STRIP:
			nativeTopology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
			break;
		case RENDER_PRIMITIVE_LINE_LIST:
			nativeTopology = D3D11_PRIMITIVE_TOPOLOGY_LINELIST;
			break;
		case RENDER_PRIMITIVE_LINE_STRIP:
			nativeTopology = D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP;
			break;
		default:
			return RENDER_RESULT_INVALID_ARGUMENT;
		}
		if (m_topologyBound && m_boundTopology == topology)
		{
			return RENDER_RESULT_OK;
		}
		m_context->IASetPrimitiveTopology(nativeTopology);
		m_topologyBound = true;
		m_boundTopology = topology;
		return RENDER_RESULT_OK;
	}

	virtual RenderResult draw(unsigned int vertexCount, unsigned int startVertex)
	{
		if (!isOwner() || !m_frameOpen || !m_pipelineBound || !m_topologyBound ||
			!m_vertexBufferBound || !m_handles->isLive(m_boundVertexBuffer))
		{
			return RENDER_RESULT_INVALID_ARGUMENT;
		}
		const ResourceSlot &vertexSlot =
			m_resources[m_boundVertexBuffer.index()];
		if (!isElementRangeWithinBuffer(vertexSlot.byteCount,
			m_boundVertexOffset, m_boundVertexStride, startVertex, vertexCount))
		{
			return RENDER_RESULT_INVALID_ARGUMENT;
		}
		m_context->Draw(vertexCount, startVertex);
		return RENDER_RESULT_OK;
	}

	virtual RenderResult drawIndexed(unsigned int indexCount,
		unsigned int startIndex, int baseVertex)
	{
		if (!isOwner() || !m_frameOpen || !m_pipelineBound || !m_topologyBound ||
			!m_vertexBufferBound || !m_indexBufferBound ||
			!m_handles->isLive(m_boundIndexBuffer))
		{
			return RENDER_RESULT_INVALID_ARGUMENT;
		}
		const ResourceSlot &indexSlot = m_resources[m_boundIndexBuffer.index()];
		const unsigned int indexSize = m_boundIndexFormat ==
			RENDER_FORMAT_R16_UINT ? 2U : (m_boundIndexFormat ==
			RENDER_FORMAT_R32_UINT ? 4U : 0U);
		if (!isElementRangeWithinBuffer(indexSlot.byteCount,
			m_boundIndexOffset, indexSize, startIndex, indexCount))
		{
			return RENDER_RESULT_INVALID_ARGUMENT;
		}
		m_context->DrawIndexed(indexCount, startIndex, baseVertex);
		return RENDER_RESULT_OK;
	}

	virtual RenderResult endFrame()
	{
		if (!isOwner() || !m_frameOpen)
		{
			return RENDER_RESULT_INVALID_ARGUMENT;
		}
		m_frameOpen = false;
		return TranslateResult(m_device->GetDeviceRemovedReason());
	}

	virtual RenderResult captureBackBuffer(void *destination,
		size_t destinationBytes, size_t destinationRowPitch,
		RenderFormat *format)
	{
		size_t requiredRowBytes = 0;
		size_t requiredBytes = 0;
		const bool validSize = Checked_Multiply(static_cast<size_t>(m_width),
			4, &requiredRowBytes) &&
			Checked_Multiply(destinationRowPitch, static_cast<size_t>(m_height),
				&requiredBytes);
		if (!isOwner() || m_frameOpen || m_swapChain == 0 || destination == 0 ||
			format == 0 || m_width == 0 || m_height == 0 || !validSize ||
			destinationRowPitch < requiredRowBytes ||
			destinationBytes < requiredBytes)
		{
			return RENDER_RESULT_INVALID_ARGUMENT;
		}
		ID3D11Texture2D *backBuffer = 0;
		HRESULT result = m_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D),
			reinterpret_cast<void **>(&backBuffer));
		if (FAILED(result))
		{
			return TranslateResult(result);
		}
		D3D11_TEXTURE2D_DESC descriptor;
		backBuffer->GetDesc(&descriptor);
		const RenderFormat captureFormat =
			TranslateBackBufferFormat(descriptor.Format);
		if (descriptor.Width != m_width || descriptor.Height != m_height ||
			descriptor.SampleDesc.Count != 1 || captureFormat ==
			RENDER_FORMAT_UNKNOWN)
		{
			backBuffer->Release();
			return RENDER_RESULT_FAILED;
		}
		descriptor.BindFlags = 0;
		descriptor.MiscFlags = 0;
		descriptor.Usage = D3D11_USAGE_STAGING;
		descriptor.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		ID3D11Texture2D *staging = 0;
		result = m_device->CreateTexture2D(&descriptor, 0, &staging);
		if (SUCCEEDED(result))
		{
			m_context->CopyResource(staging, backBuffer);
			D3D11_MAPPED_SUBRESOURCE mapped;
			result = m_context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
			if (SUCCEEDED(result))
			{
				if (mapped.pData == 0 || mapped.RowPitch < requiredRowBytes)
				{
					// A successful Map must still provide a complete readable row;
					// reject malformed mapping metadata before touching the destination.
					result = E_FAIL;
				}
				else
				{
					unsigned char *output = static_cast<unsigned char *>(destination);
					const unsigned char *input =
						static_cast<const unsigned char *>(mapped.pData);
					for (unsigned int row = 0; row < m_height; ++row)
					{
						memcpy(output + row * destinationRowPitch,
							input + row * mapped.RowPitch, requiredRowBytes);
					}
				}
				m_context->Unmap(staging, 0);
				if (SUCCEEDED(result))
				{
					*format = captureFormat;
				}
			}
		}
		if (staging != 0)
		{
			staging->Release();
		}
		backBuffer->Release();
		return TranslateResult(result);
	}

	virtual RenderResult getDebugValidationErrorCount(unsigned int *count) const
	{
		if (!isOwner() || count == 0)
		{
			return RENDER_RESULT_INVALID_ARGUMENT;
		}
		ID3D11InfoQueue *queue = 0;
		const HRESULT queryResult = m_device->QueryInterface(
			__uuidof(ID3D11InfoQueue), reinterpret_cast<void **>(&queue));
		if (FAILED(queryResult))
		{
			return RENDER_RESULT_UNSUPPORTED;
		}
		*count = 0;
		const UINT64 messageCount = queue->GetNumStoredMessagesAllowedByRetrievalFilter();
		for (UINT64 index = 0; index < messageCount; ++index)
		{
			SIZE_T messageBytes = 0;
			if (FAILED(queue->GetMessage(index, 0, &messageBytes)))
			{
				queue->Release();
				return RENDER_RESULT_FAILED;
			}
			std::vector<unsigned char> storage;
			try
			{
				storage.resize(messageBytes);
			}
			catch (...)
			{
				queue->Release();
				return RENDER_RESULT_OUT_OF_MEMORY;
			}
			D3D11_MESSAGE *message =
				reinterpret_cast<D3D11_MESSAGE *>(&storage[0]);
			if (FAILED(queue->GetMessage(index, message, &messageBytes)))
			{
				queue->Release();
				return RENDER_RESULT_FAILED;
			}
			if (message->Severity == D3D11_MESSAGE_SEVERITY_CORRUPTION ||
				message->Severity == D3D11_MESSAGE_SEVERITY_ERROR)
			{
				++*count;
			}
		}
		queue->Release();
		return RENDER_RESULT_OK;
	}

	virtual RenderResult reportDebugLiveObjects()
	{
		if (!isOwner() || m_frameOpen)
		{
			return RENDER_RESULT_INVALID_ARGUMENT;
		}
		return reportLiveObjects();
	}

private:
	RenderResult reportLiveObjects()
	{
		if (!m_debugLayerActive || m_debugLayer == 0)
		{
			return RENDER_RESULT_UNSUPPORTED;
		}
		// Context references are themselves reported as live objects. Clear and
		// flush them before asking the SDK layer for its summary so this check is
		// useful both at an explicit test point and during final device teardown.
		if (m_context != 0)
		{
			m_context->ClearState();
			m_context->Flush();
			invalidateContextBindings();
		}
		return TranslateResult(m_debugLayer->ReportLiveDeviceObjects(
			D3D11_RLDO_DETAIL));
	}

	bool isOwner() const
	{
		return m_initialized && GetCurrentThreadId() == m_ownerThread;
	}

	void cacheLegacyState(const LegacyLogicalState &state,
		LegacyVertexFormat vertexFormat, unsigned int texturePresenceMask,
		unsigned int vertexLayoutFlags, ID3D11InputLayout *inputLayout)
	{
		memcpy(m_cachedLegacyState, &state, sizeof(state));
		memcpy(m_cachedLegacyPipeline, &state.pipeline,
			sizeof(state.pipeline));
		m_cachedLegacyVertexFormat = vertexFormat;
		m_cachedLegacyTexturePresenceMask = texturePresenceMask;
		m_cachedLegacyCubeTextureMask = m_boundCubeTextureMask;
		m_cachedLegacySignedTextureMask = m_boundSignedTextureMask;
		m_cachedLegacyVertexLayoutFlags = vertexLayoutFlags;
		m_cachedLegacyInputLayout = inputLayout;
		m_cachedLegacyStateValid = true;
		m_cachedLegacyPipelineValid = true;
	}

	void invalidatePipelineBindings()
	{
		m_pipelineStateValid = false;
		m_pipelineBound = false;
		m_cachedLegacyStateValid = false;
		m_cachedLegacyPipelineValid = false;
		m_transformConstantsValid = false;
		m_transformConstantsChanged = true;
	}

	void invalidateResourceBindings()
	{
		m_renderTargetsBound = false;
		m_textureBindingsValid = false;
		m_vertexBufferBound = false;
		m_indexBufferBound = false;
		m_topologyBound = false;
		m_viewportBound = false;
	}

	void invalidateContextBindings()
	{
		invalidatePipelineBindings();
		invalidateResourceBindings();
	}

	void markTextureBindingsEmpty()
	{
		m_textureBindingsValid = true;
		m_boundCubeTextureMask = 0;
		m_boundSignedTextureMask = 0;
		m_transformConstantsChanged = true;
		for (unsigned int stage = 0; stage < LEGACY_TEXTURE_STAGE_COUNT;
			++stage)
		{
			m_boundTextures[stage] = GpuHandle();
		}
	}

	void markTextureRecoverySourceUnavailable(ResourceSlot &slot)
	{
		// A refresh can fail after one or more subresources have already been
		// written.  The native resource is then neither the previous complete
		// image nor the newly requested complete image.  Retaining the previous
		// shadow would make device recovery restore stale bytes over that partial
		// update, so force deterministic clear/repopulation instead.
		slot.gpuAuthoritative = true;
		slot.shadow.clear();
		slot.subresourceOffsets.clear();
		slot.subresourceRowPitches.clear();
		slot.subresourceSlicePitches.clear();
	}

	void unbindTextureResources()
	{
		ID3D11ShaderResourceView *emptyViews[LEGACY_TEXTURE_STAGE_COUNT * 2] = { 0 };
		m_context->PSSetShaderResources(0, LEGACY_TEXTURE_STAGE_COUNT * 2,
			emptyViews);
		markTextureBindingsEmpty();
	}

	void clearTextureStage(unsigned int stage)
	{
		ID3D11ShaderResourceView *emptyView = 0;
		m_context->PSSetShaderResources(stage, 1, &emptyView);
		m_context->PSSetShaderResources(8 + stage, 1, &emptyView);
		m_boundTextures[stage] = GpuHandle();
		m_boundCubeTextureMask &= ~(1U << stage);
		m_boundSignedTextureMask &= ~(1U << stage);
		m_transformConstantsChanged = true;
	}

	unsigned int unbindTextureResource(GpuHandle texture)
	{
		if (!m_textureBindingsValid)
		{
			unbindTextureResources();
			return 0;
		}
		unsigned int affectedStages = 0;
		for (unsigned int stage = 0; stage < LEGACY_TEXTURE_STAGE_COUNT;
			++stage)
		{
			if (m_boundTextures[stage] != texture)
			{
				continue;
			}
			clearTextureStage(stage);
			affectedStages |= 1U << stage;
		}
		return affectedStages;
	}

	RenderResult rebindTextureResource(GpuHandle texture,
		unsigned int affectedStages)
	{
		if (affectedStages == 0 || !m_frameOpen)
		{
			return RENDER_RESULT_OK;
		}
		for (unsigned int stage = 0; stage < LEGACY_TEXTURE_STAGE_COUNT;
			++stage)
		{
			if ((affectedStages & (1U << stage)) == 0)
			{
				continue;
			}
			const RenderResult bindingResult = setTexture(stage, texture);
			if (bindingResult != RENDER_RESULT_OK)
			{
				return bindingResult;
			}
		}
		return RENDER_RESULT_OK;
	}

	static bool isElementRangeWithinBuffer(size_t byteCapacity,
		size_t byteOffset, size_t elementSize, unsigned int start,
		unsigned int count)
	{
		if (elementSize == 0 || count == 0 || byteOffset > byteCapacity)
		{
			return false;
		}
		const size_t availableElements =
			(byteCapacity - byteOffset) / elementSize;
		const size_t firstElement = static_cast<size_t>(start);
		return firstElement <= availableElements &&
			static_cast<size_t>(count) <= availableElements - firstElement;
	}

	void invalidateTextureBindingsForTargets(ID3D11Resource *colorResource,
		ID3D11Resource *depthResource)
	{
		if (!m_textureBindingsValid)
		{
			unbindTextureResources();
			return;
		}
		for (unsigned int stage = 0; stage < LEGACY_TEXTURE_STAGE_COUNT;
			++stage)
		{
			const GpuHandle handle = m_boundTextures[stage];
			if (!m_handles->isLive(handle))
			{
				continue;
			}
			const ResourceSlot &slot = m_resources[handle.index()];
			if (slot.resource != colorResource && slot.resource != depthResource)
			{
				continue;
			}
			clearTextureStage(stage);
		}
	}

	static void setNativeUsage(RenderUsage usage, D3D11_USAGE *nativeUsage,
		UINT *cpuAccessFlags)
	{
		*cpuAccessFlags = 0;
		switch (usage)
		{
		case RENDER_USAGE_IMMUTABLE:
			*nativeUsage = D3D11_USAGE_IMMUTABLE;
			break;
		case RENDER_USAGE_DYNAMIC:
			*nativeUsage = D3D11_USAGE_DYNAMIC;
			*cpuAccessFlags = D3D11_CPU_ACCESS_WRITE;
			break;
		default:
			*nativeUsage = D3D11_USAGE_DEFAULT;
			break;
		}
	}

	RenderResult prepareTextureRefreshData(const TextureDescriptor &descriptor,
		const TextureSubresourceData *data, unsigned int dataCount,
		ResourceSlot *slot) const
	{
		if (data == 0 || slot == 0 || descriptor.arrayCount == 0 ||
			descriptor.mipCount == 0 ||
			descriptor.mipCount > UINT_MAX / descriptor.arrayCount)
		{
			return RENDER_RESULT_INVALID_ARGUMENT;
		}
		const unsigned int subresourceCount = descriptor.mipCount *
			descriptor.arrayCount;
		if (dataCount != subresourceCount)
		{
			return RENDER_RESULT_INVALID_ARGUMENT;
		}
		if (subresourceCount > MAX_TEXTURE_REFRESH_SUBRESOURCES)
		{
			return RENDER_RESULT_UNSUPPORTED;
		}
		const unsigned int bytesPerPixel = TextureBytesPerPixel(
			descriptor.format);
		if (bytesPerPixel == 0)
		{
			return RENDER_RESULT_UNSUPPORTED;
		}

		size_t totalBytes = 0;
		for (unsigned int index = 0; index < subresourceCount; ++index)
		{
			if (data[index].data == 0 || data[index].rowPitch == 0 ||
				data[index].rowPitch > UINT_MAX ||
				data[index].slicePitch > UINT_MAX)
			{
				return RENDER_RESULT_INVALID_ARGUMENT;
			}
			const unsigned int mip = index % descriptor.mipCount;
			const unsigned int mipWidth = mip < 32 ? descriptor.width >> mip : 0;
			const unsigned int mipHeight = mip < 32 ? descriptor.height >> mip : 0;
			const size_t width = mipWidth == 0 ? 1 : mipWidth;
			const size_t height = mipHeight == 0 ? 1 : mipHeight;
			if (width > static_cast<size_t>(-1) / bytesPerPixel)
			{
				return RENDER_RESULT_INVALID_ARGUMENT;
			}
			const size_t minimumRowPitch = width * bytesPerPixel;
			if (data[index].rowPitch < minimumRowPitch ||
				height > static_cast<size_t>(-1) / data[index].rowPitch)
			{
				return RENDER_RESULT_INVALID_ARGUMENT;
			}
			const size_t minimumSlicePitch = data[index].rowPitch * height;
			if (data[index].slicePitch != 0 &&
				data[index].slicePitch < minimumSlicePitch)
			{
				return RENDER_RESULT_INVALID_ARGUMENT;
			}
			const size_t copyBytes = data[index].slicePitch > minimumSlicePitch ?
				data[index].slicePitch : minimumSlicePitch;
			if (copyBytes > MAX_TEXTURE_REFRESH_BYTES ||
				totalBytes > MAX_TEXTURE_REFRESH_BYTES - copyBytes)
			{
				return RENDER_RESULT_UNSUPPORTED;
			}
			totalBytes += copyBytes;
		}

		// Reserve before changing any values so a failed allocation leaves the
		// previous complete recovery description intact.  Once capacity is
		// established, the steady-state movie refresh only resizes to the same
		// lengths and writes the new frame in place.
		const size_t oldShadowSize = slot->shadow.size();
		const size_t oldOffsetCount = slot->subresourceOffsets.size();
		const size_t oldRowPitchCount = slot->subresourceRowPitches.size();
		const size_t oldSlicePitchCount = slot->subresourceSlicePitches.size();
		try
		{
			slot->shadow.reserve(totalBytes);
			slot->subresourceOffsets.reserve(subresourceCount);
			slot->subresourceRowPitches.reserve(subresourceCount);
			slot->subresourceSlicePitches.reserve(subresourceCount);
			slot->shadow.resize(totalBytes);
			slot->subresourceOffsets.resize(subresourceCount);
			slot->subresourceRowPitches.resize(subresourceCount);
			slot->subresourceSlicePitches.resize(subresourceCount);
		}
		catch (...)
		{
			// reserve() never shrinks storage, so restoring the old sizes cannot
			// throw and leaves the old shadow bytes available for recovery.
			slot->shadow.resize(oldShadowSize);
			slot->subresourceOffsets.resize(oldOffsetCount);
			slot->subresourceRowPitches.resize(oldRowPitchCount);
			slot->subresourceSlicePitches.resize(oldSlicePitchCount);
			return RENDER_RESULT_OUT_OF_MEMORY;
		}

		size_t offset = 0;
		for (unsigned int index = 0; index < subresourceCount; ++index)
		{
			const unsigned int mip = index % descriptor.mipCount;
			const unsigned int mipHeight = mip < 32 ?
				descriptor.height >> mip : 0;
			const size_t height = mipHeight == 0 ? 1 : mipHeight;
			const size_t minimumSlicePitch = data[index].rowPitch * height;
			const size_t copyBytes = data[index].slicePitch > minimumSlicePitch ?
				data[index].slicePitch : minimumSlicePitch;
			slot->subresourceOffsets[index] = offset;
			slot->subresourceRowPitches[index] = data[index].rowPitch;
			slot->subresourceSlicePitches[index] = data[index].slicePitch;
			offset += copyBytes;
		}
		if (offset != totalBytes)
		{
			// Keep this invariant explicit: the commit loop relies on every
			// subresource range being inside the reserved shadow.
			slot->shadow.resize(oldShadowSize);
			slot->subresourceOffsets.resize(oldOffsetCount);
			slot->subresourceRowPitches.resize(oldRowPitchCount);
			slot->subresourceSlicePitches.resize(oldSlicePitchCount);
			return RENDER_RESULT_FAILED;
		}
		return RENDER_RESULT_OK;
	}

	HRESULT createPipelineResources()
	{
		D3D11_BUFFER_DESC constantDescriptor;
		memset(&constantDescriptor, 0, sizeof(constantDescriptor));
		constantDescriptor.ByteWidth = sizeof(LegacyTransformConstants);
		constantDescriptor.Usage = D3D11_USAGE_DYNAMIC;
		constantDescriptor.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		constantDescriptor.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		HRESULT result = S_OK;
		for (unsigned int index = 0; index < TRANSFORM_CONSTANT_BUFFER_COUNT;
			++index)
		{
			result = m_device->CreateBuffer(&constantDescriptor, 0,
				&m_transformConstants[index]);
			if (FAILED(result))
			{
				return result;
			}
		}
		m_transformConstantCursor = 0;
		result = m_device->CreateVertexShader(g_LegacyFixedFunctionVS,
			sizeof(g_LegacyFixedFunctionVS), 0, &m_vertexShader);
		if (FAILED(result))
		{
			return result;
		}
		result = m_device->CreatePixelShader(g_LegacyFixedFunctionPS,
			sizeof(g_LegacyFixedFunctionPS), 0, &m_pixelShader);
		if (FAILED(result))
		{
			return result;
		}
		const D3D11_INPUT_ELEMENT_DESC elements[] = {
			{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
				D3D11_INPUT_PER_VERTEX_DATA, 0 },
			{ "COLOR", 0, DXGI_FORMAT_B8G8R8A8_UNORM, 0, 12,
				D3D11_INPUT_PER_VERTEX_DATA, 0 }
		};
		result = m_device->CreateInputLayout(elements,
			static_cast<UINT>(sizeof(elements) / sizeof(elements[0])),
			g_LegacyFixedFunctionVS, sizeof(g_LegacyFixedFunctionVS),
			&m_positionColorLayout);
		if (FAILED(result))
		{
			return result;
		}
		result = m_device->CreateVertexShader(g_LegacyTexturedVS,
			sizeof(g_LegacyTexturedVS), 0, &m_texturedVertexShader);
		if (FAILED(result))
		{
			return result;
		}
	result = m_device->CreatePixelShader(g_LegacyTexturedPS,
		 sizeof(g_LegacyTexturedPS), 0, &m_texturedPixelShader);
	if (FAILED(result))
	{
		return result;
	}
	result = m_device->CreatePixelShader(g_LegacyTexturedFixed1PS,
		sizeof(g_LegacyTexturedFixed1PS), 0, &m_texturedFixed1PixelShader);
	if (FAILED(result))
	{
		return result;
	}
	result = m_device->CreatePixelShader(g_LegacyTexturedFixed2PS,
		sizeof(g_LegacyTexturedFixed2PS), 0, &m_texturedFixed2PixelShader);
	if (FAILED(result))
	{
		return result;
	}
	struct TerrainPixelShaderBlob
	{
		const void *data;
		size_t byteCount;
	};
	const TerrainPixelShaderBlob terrainPixelShaders[
		TERRAIN_PIXEL_PROGRAM_COUNT] = {
		{ g_LegacyTerrainBasePS, sizeof(g_LegacyTerrainBasePS) },
		{ g_LegacyTerrainNoisePS, sizeof(g_LegacyTerrainNoisePS) },
		{ g_LegacyTerrainNoise2PS, sizeof(g_LegacyTerrainNoise2PS) },
		{ g_LegacyRoadNoise2PS, sizeof(g_LegacyRoadNoise2PS) },
		{ g_LegacyFlatTerrainBase0PS, sizeof(g_LegacyFlatTerrainBase0PS) },
		{ g_LegacyFlatTerrainBasePS, sizeof(g_LegacyFlatTerrainBasePS) },
		{ g_LegacyFlatTerrainNoisePS, sizeof(g_LegacyFlatTerrainNoisePS) },
		{ g_LegacyFlatTerrainNoise2PS, sizeof(g_LegacyFlatTerrainNoise2PS) }
	};
	for (unsigned int programIndex = 0;
		programIndex < TERRAIN_PIXEL_PROGRAM_COUNT; ++programIndex)
	{
		result = m_device->CreatePixelShader(terrainPixelShaders[programIndex].data,
			terrainPixelShaders[programIndex].byteCount, 0,
			&m_terrainPixelShaders[programIndex]);
		if (FAILED(result))
		{
			return result;
		}
	}
	result = m_device->CreatePixelShader(g_LegacyWaterFlatPS,
		sizeof(g_LegacyWaterFlatPS), 0, &m_waterFlatPixelShader);
	if (FAILED(result))
	{
		return result;
	}
	result = m_device->CreatePixelShader(g_LegacyWaterRiverPS,
		sizeof(g_LegacyWaterRiverPS), 0, &m_waterRiverPixelShader);
	if (FAILED(result))
	{
		return result;
	}
	result = m_device->CreateVertexShader(g_LegacySeaWaveVS,
		sizeof(g_LegacySeaWaveVS), 0, &m_seaWaveVertexShader);
	if (FAILED(result))
	{
		return result;
	}
	result = m_device->CreatePixelShader(g_LegacySeaWavePS,
		sizeof(g_LegacySeaWavePS), 0, &m_seaWavePixelShader);
	if (FAILED(result))
	{
		return result;
	}
	const D3D11_INPUT_ELEMENT_DESC seaWaveElements[] = {
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
			D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "COLOR", 0, DXGI_FORMAT_B8G8R8A8_UNORM, 0, 12,
			D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 16,
			D3D11_INPUT_PER_VERTEX_DATA, 0 }
	};
	result = m_device->CreateInputLayout(seaWaveElements,
		static_cast<UINT>(sizeof(seaWaveElements) /
			sizeof(seaWaveElements[0])), g_LegacySeaWaveVS,
		sizeof(g_LegacySeaWaveVS), &m_seaWaveLayout);
	if (FAILED(result))
	{
		return result;
	}
		return S_OK;
	}

	HRESULT updateTransformConstants(const LegacyLogicalState &state,
		unsigned int vertexLayoutFlags, unsigned int texturePresenceMask,
		unsigned int cubeTextureMask)
	{
		LegacyTransformConstants shaderConstants;
		RenderMatrix4 worldViewMatrix;
		MultiplyMatrices(state.constants.world.values, state.constants.view.values,
			worldViewMatrix.values);
		for (unsigned int component = 0; component < 16; ++component)
		{
			shaderConstants.worldView[component] = worldViewMatrix.values[component];
		}
		MultiplyMatrices(worldViewMatrix.values,
			state.constants.projection.values,
			shaderConstants.worldViewProjection);
		// Invert once while publishing the logical state.  The vertex shader only
		// multiplies by these matrices; it must not perform a cofactor/determinant
		// calculation for every vertex.
		BuildLegacyInverseTransposeNormalMatrix(state.constants.world,
			shaderConstants.worldNormalMatrix);
		BuildLegacyInverseTransposeNormalMatrix(worldViewMatrix,
			shaderConstants.worldViewNormalMatrix);
		// The legacy shader has three distinct fog equations.  The scale-fragment
		// variant deliberately sets the fixed-function fog color to black so that
		// the visibility factor multiplies the fragment instead of blending it
		// toward the scene fog color.  Keeping the configured fog color here makes
		// D3D11 output visibly different from the legacy FOG_SCALE_FRAGMENT path.
		const RenderFloat4 fogColor = state.pipeline.fogMode == RENDER_FOG_WHITE ?
			RenderFloat4(1.0f, 1.0f, 1.0f, 1.0f) :
			(state.pipeline.fogMode == RENDER_FOG_SCALE_FRAGMENT ?
				RenderFloat4(0.0f, 0.0f, 0.0f, 0.0f) : state.constants.fog.color);
		shaderConstants.fogColor[0] = fogColor.x;
		shaderConstants.fogColor[1] = fogColor.y;
		shaderConstants.fogColor[2] = fogColor.z;
		shaderConstants.fogColor[3] = fogColor.w;
		shaderConstants.fogParameters[0] = state.constants.fog.start;
		shaderConstants.fogParameters[1] = state.constants.fog.end;
		shaderConstants.fogParameters[2] = state.constants.fog.density;
		shaderConstants.fogParameters[3] = state.constants.fog.enabled ?
			static_cast<float>(state.pipeline.fogMode) : 0.0f;
		shaderConstants.alphaTestParameters[0] =
			state.pipeline.alphaTestEnable ? 1U : 0U;
		shaderConstants.alphaTestParameters[1] =
			static_cast<unsigned int>(state.pipeline.alphaFunction);
		shaderConstants.alphaTestParameters[2] =
			state.pipeline.alphaReference > 255 ? 255 :
			state.pipeline.alphaReference;
		shaderConstants.alphaTestParameters[3] = 0;
		shaderConstants.fogStateParameters[0] =
			state.pipeline.rangeFogEnable ? 1U : 0U;
		shaderConstants.fogStateParameters[1] = 0;
		shaderConstants.fogStateParameters[2] = 0;
		shaderConstants.fogStateParameters[3] = 0;
		for (unsigned int stage = 0; stage < LEGACY_TEXTURE_STAGE_COUNT; ++stage)
		{
			const LegacyTextureStageState &textureStage =
				state.pipeline.textureStages[stage];
			shaderConstants.textureColorParameters[stage][0] =
				static_cast<unsigned int>(textureStage.colorOperation);
			shaderConstants.textureColorParameters[stage][1] =
				static_cast<unsigned int>(textureStage.colorArgument0);
			shaderConstants.textureColorParameters[stage][2] =
				static_cast<unsigned int>(textureStage.colorArgument1);
			shaderConstants.textureColorParameters[stage][3] =
				static_cast<unsigned int>(textureStage.colorArgument2);
			shaderConstants.textureAlphaParameters[stage][0] =
				static_cast<unsigned int>(textureStage.alphaOperation);
			shaderConstants.textureAlphaParameters[stage][1] =
				static_cast<unsigned int>(textureStage.alphaArgument0);
			shaderConstants.textureAlphaParameters[stage][2] =
				static_cast<unsigned int>(textureStage.alphaArgument1);
			shaderConstants.textureAlphaParameters[stage][3] =
				static_cast<unsigned int>(textureStage.alphaArgument2);
			shaderConstants.textureModifierParameters[stage][0] =
				(static_cast<unsigned int>(textureStage.colorArgument0Complement) << 0) |
				(static_cast<unsigned int>(textureStage.colorArgument0AlphaReplicate) << 1) |
				(static_cast<unsigned int>(textureStage.colorArgument1Complement) << 2) |
				(static_cast<unsigned int>(textureStage.colorArgument1AlphaReplicate) << 3) |
				(static_cast<unsigned int>(textureStage.colorArgument2Complement) << 4) |
				(static_cast<unsigned int>(textureStage.colorArgument2AlphaReplicate) << 5);
			shaderConstants.textureModifierParameters[stage][1] =
				(static_cast<unsigned int>(textureStage.alphaArgument0Complement) << 0) |
				(static_cast<unsigned int>(textureStage.alphaArgument0AlphaReplicate) << 1) |
				(static_cast<unsigned int>(textureStage.alphaArgument1Complement) << 2) |
				(static_cast<unsigned int>(textureStage.alphaArgument1AlphaReplicate) << 3) |
				(static_cast<unsigned int>(textureStage.alphaArgument2Complement) << 4) |
				(static_cast<unsigned int>(textureStage.alphaArgument2AlphaReplicate) << 5);
			shaderConstants.textureModifierParameters[stage][2] =
				(textureStage.textureCoordinateIndex & 7U) |
				(textureStage.resultArgument == RENDER_TEXTURE_ARG_TEMP ? 0x100U : 0U);
			shaderConstants.textureModifierParameters[stage][3] =
				(textureStage.projectedCoordinates ? 1U : 0U) |
				(textureStage.cameraSpacePosition ? 2U : 0U) |
				(textureStage.textureTransformEnable ? 4U : 0U) |
				(textureStage.cameraSpaceNormal ? 8U : 0U) |
				(textureStage.cameraSpaceReflectionVector ? 16U : 0U) |
				((textureStage.textureTransformCount & 7U) << 5);
			shaderConstants.textureBumpParameters0[stage][0] =
				textureStage.bumpEnvironmentMatrix00;
			shaderConstants.textureBumpParameters0[stage][1] =
				textureStage.bumpEnvironmentMatrix01;
			shaderConstants.textureBumpParameters0[stage][2] =
				textureStage.bumpEnvironmentMatrix10;
			shaderConstants.textureBumpParameters0[stage][3] =
				textureStage.bumpEnvironmentMatrix11;
			shaderConstants.textureBumpParameters1[stage][0] =
				textureStage.bumpEnvironmentLuminanceScale;
			shaderConstants.textureBumpParameters1[stage][1] =
				textureStage.bumpEnvironmentLuminanceOffset;
			shaderConstants.textureBumpParameters1[stage][2] = 0.0f;
			shaderConstants.textureBumpParameters1[stage][3] = 0.0f;
			for (unsigned int component = 0; component < 16; ++component)
			{
				shaderConstants.textureTransforms[stage][component] =
					state.constants.textureTransforms[stage].values[component];
			}
		}
		const unsigned int textureFactor = state.pipeline.textureFactor;
		shaderConstants.textureFactor[0] =
			static_cast<float>((textureFactor >> 16) & 0xffU) / 255.0f;
		shaderConstants.textureFactor[1] =
			static_cast<float>((textureFactor >> 8) & 0xffU) / 255.0f;
		shaderConstants.textureFactor[2] =
			static_cast<float>(textureFactor & 0xffU) / 255.0f;
		shaderConstants.textureFactor[3] =
			static_cast<float>((textureFactor >> 24) & 0xffU) / 255.0f;
		for (unsigned int component = 0; component < 16; ++component)
		{
			shaderConstants.world[component] = state.constants.world.values[component];
			shaderConstants.view[component] = state.constants.view.values[component];
		}
		shaderConstants.materialDiffuse[0] = state.constants.material.diffuse.x;
		shaderConstants.materialDiffuse[1] = state.constants.material.diffuse.y;
		shaderConstants.materialDiffuse[2] = state.constants.material.diffuse.z;
		shaderConstants.materialDiffuse[3] = state.constants.material.diffuse.w;
		shaderConstants.materialAmbient[0] = state.constants.material.ambient.x;
		shaderConstants.materialAmbient[1] = state.constants.material.ambient.y;
		shaderConstants.materialAmbient[2] = state.constants.material.ambient.z;
		shaderConstants.materialAmbient[3] = state.constants.material.ambient.w;
		shaderConstants.materialSpecular[0] = state.constants.material.specular.x;
		shaderConstants.materialSpecular[1] = state.constants.material.specular.y;
		shaderConstants.materialSpecular[2] = state.constants.material.specular.z;
		shaderConstants.materialSpecular[3] = state.constants.material.specular.w;
		shaderConstants.materialEmissive[0] = state.constants.material.emissive.x;
		shaderConstants.materialEmissive[1] = state.constants.material.emissive.y;
		shaderConstants.materialEmissive[2] = state.constants.material.emissive.z;
		shaderConstants.materialEmissive[3] = state.constants.material.emissive.w;
		shaderConstants.materialSpecularPower[0] =
			state.constants.material.specularPower;
		shaderConstants.materialSpecularPower[1] = 0.0f;
		shaderConstants.materialSpecularPower[2] = 0.0f;
		shaderConstants.materialSpecularPower[3] = 0.0f;
		shaderConstants.globalAmbient[0] = state.constants.globalAmbient.x;
		shaderConstants.globalAmbient[1] = state.constants.globalAmbient.y;
		shaderConstants.globalAmbient[2] = state.constants.globalAmbient.z;
		shaderConstants.globalAmbient[3] = state.constants.globalAmbient.w;
		for (unsigned int lightIndex = 0; lightIndex < LEGACY_LIGHT_COUNT;
			++lightIndex)
		{
			const LegacyLightState &light = state.constants.lights[lightIndex];
			shaderConstants.lightDiffuse[lightIndex][0] = light.diffuse.x;
			shaderConstants.lightDiffuse[lightIndex][1] = light.diffuse.y;
			shaderConstants.lightDiffuse[lightIndex][2] = light.diffuse.z;
			shaderConstants.lightDiffuse[lightIndex][3] = light.diffuse.w;
			shaderConstants.lightAmbient[lightIndex][0] = light.ambient.x;
			shaderConstants.lightAmbient[lightIndex][1] = light.ambient.y;
			shaderConstants.lightAmbient[lightIndex][2] = light.ambient.z;
			shaderConstants.lightAmbient[lightIndex][3] = light.ambient.w;
			shaderConstants.lightSpecular[lightIndex][0] = light.specular.x;
			shaderConstants.lightSpecular[lightIndex][1] = light.specular.y;
			shaderConstants.lightSpecular[lightIndex][2] = light.specular.z;
			shaderConstants.lightSpecular[lightIndex][3] = light.specular.w;
			shaderConstants.lightPositionAndType[lightIndex][0] = light.position.x;
			shaderConstants.lightPositionAndType[lightIndex][1] = light.position.y;
			shaderConstants.lightPositionAndType[lightIndex][2] = light.position.z;
			shaderConstants.lightPositionAndType[lightIndex][3] =
				static_cast<float>(light.type);
			shaderConstants.lightDirectionAndEnabled[lightIndex][0] = light.direction.x;
			shaderConstants.lightDirectionAndEnabled[lightIndex][1] = light.direction.y;
			shaderConstants.lightDirectionAndEnabled[lightIndex][2] = light.direction.z;
			shaderConstants.lightDirectionAndEnabled[lightIndex][3] =
				light.enabled ? 1.0f : 0.0f;
			shaderConstants.lightAttenuation[lightIndex][0] = light.range;
			shaderConstants.lightAttenuation[lightIndex][1] = light.attenuation0;
			shaderConstants.lightAttenuation[lightIndex][2] = light.attenuation1;
			shaderConstants.lightAttenuation[lightIndex][3] = light.attenuation2;
			shaderConstants.lightSpotParameters[lightIndex][0] = light.theta;
			shaderConstants.lightSpotParameters[lightIndex][1] = light.phi;
			shaderConstants.lightSpotParameters[lightIndex][2] = light.falloff;
			shaderConstants.lightSpotParameters[lightIndex][3] = 0.0f;
		}
		shaderConstants.lightingParameters[0] =
			state.pipeline.lightingEnable ? 1U : 0U;
		shaderConstants.lightingParameters[1] =
			state.pipeline.normalizeNormals ? 1U : 0U;
		shaderConstants.lightingParameters[2] =
			static_cast<unsigned int>(state.pipeline.pixelProgram);
		shaderConstants.lightingParameters[3] = texturePresenceMask & 0xffU;
		shaderConstants.lightingParameters[3] |=
			(cubeTextureMask & 0xffU) << 8;
		shaderConstants.lightingParameters[3] |=
			(m_boundSignedTextureMask & 0xffU) << 16;
		shaderConstants.vertexLayoutParameters[0] =
			(vertexLayoutFlags & 1U) != 0 ? 1U : 0U;
		shaderConstants.vertexLayoutParameters[1] =
			(vertexLayoutFlags & 2U) != 0 ? 1U : 0U;
		shaderConstants.vertexLayoutParameters[2] =
			(vertexLayoutFlags & 4U) != 0 ? 1U : 0U;
		shaderConstants.vertexLayoutParameters[3] = vertexLayoutFlags >> 8;
		shaderConstants.vertexLayoutParameters[3] |=
			(vertexLayoutFlags & LEGACY_VERTEX_LAYOUT_PRETRANSFORMED) != 0 ?
				0x80000000U : 0U;
		shaderConstants.viewportParameters[0] = m_viewportX;
		shaderConstants.viewportParameters[1] = m_viewportY;
		shaderConstants.viewportParameters[2] = m_viewportWidth;
		shaderConstants.viewportParameters[3] = m_viewportHeight;
		shaderConstants.programParameters[0] =
			static_cast<unsigned int>(state.pipeline.vertexProgram);
		shaderConstants.programParameters[1] =
			state.pipeline.secondaryGradientEnable ? 1U : 0U;
		// Keep the four D3DMCS selectors in the existing spare program words.
		// Two bits are sufficient for MATERIAL/COLOR1/COLOR2 and leaving the
		// unused values rejected above prevents an accidental HLSL fallback.
		shaderConstants.programParameters[2] =
			static_cast<unsigned int>(state.pipeline.ambientMaterialSource) |
			(static_cast<unsigned int>(state.pipeline.diffuseMaterialSource) << 2) |
			(static_cast<unsigned int>(state.pipeline.emissiveMaterialSource) << 4);
		shaderConstants.programParameters[3] =
			static_cast<unsigned int>(state.pipeline.specularMaterialSource);
		for (unsigned int constant = 0;
			constant < LEGACY_VERTEX_CONSTANT_COUNT; ++constant)
		{
			const RenderFloat4 &source =
				state.constants.vertexShaderConstants[constant];
			shaderConstants.vertexShaderConstants[constant][0] = source.x;
			shaderConstants.vertexShaderConstants[constant][1] = source.y;
			shaderConstants.vertexShaderConstants[constant][2] = source.z;
			shaderConstants.vertexShaderConstants[constant][3] = source.w;
		}
		for (unsigned int constant = 0;
			constant < LEGACY_PIXEL_CONSTANT_COUNT; ++constant)
		{
			const RenderFloat4 &source =
				state.constants.pixelShaderConstants[constant];
			shaderConstants.pixelShaderConstants[constant][0] = source.x;
			shaderConstants.pixelShaderConstants[constant][1] = source.y;
			shaderConstants.pixelShaderConstants[constant][2] = source.z;
			shaderConstants.pixelShaderConstants[constant][3] = source.w;
		}
		for (unsigned int plane = 0; plane < LEGACY_CLIP_PLANE_COUNT; ++plane)
		{
			const RenderFloat4 &source = state.constants.clipPlanes[plane];
			shaderConstants.clipPlanes[plane][0] = source.x;
			shaderConstants.clipPlanes[plane][1] = source.y;
			shaderConstants.clipPlanes[plane][2] = source.z;
			shaderConstants.clipPlanes[plane][3] = source.w;
		}
		shaderConstants.clipPlaneParameters[0] =
			state.pipeline.clipPlaneEnableMask;
		shaderConstants.clipPlaneParameters[1] = 0;
		shaderConstants.clipPlaneParameters[2] = 0;
		shaderConstants.clipPlaneParameters[3] = 0;
		if (m_transformConstantsValid && memcmp(&m_lastTransformConstants,
			&shaderConstants, sizeof(shaderConstants)) == 0)
		{
			m_transformConstantsChanged = false;
			return S_OK;
		}
		ID3D11Buffer *constantBuffer =
			m_transformConstants[m_transformConstantCursor];
		m_transformConstantCursor = (m_transformConstantCursor + 1) %
			TRANSFORM_CONSTANT_BUFFER_COUNT;
		D3D11_MAPPED_SUBRESOURCE mapped;
		const HRESULT result = m_context->Map(constantBuffer, 0,
			D3D11_MAP_WRITE_DISCARD, 0, &mapped);
		if (FAILED(result))
		{
			return result;
		}
		if (mapped.pData == 0)
		{
			m_context->Unmap(constantBuffer, 0);
			return E_FAIL;
		}
		memcpy(mapped.pData, &shaderConstants, sizeof(shaderConstants));
		m_context->Unmap(constantBuffer, 0);
		m_lastTransformConstants = shaderConstants;
		m_transformConstantsValid = true;
		m_transformConstantsChanged = true;
		m_context->VSSetConstantBuffers(0, 1, &constantBuffer);
		m_context->PSSetConstantBuffers(0, 1, &constantBuffer);
		return S_OK;
	}

	void releaseNativeSlot(ResourceSlot &slot)
	{
		if (slot.depthStencil != 0)
		{
			slot.depthStencil->Release();
			slot.depthStencil = 0;
		}
		if (slot.renderTarget != 0)
		{
			slot.renderTarget->Release();
			slot.renderTarget = 0;
		}
		if (slot.view != 0)
		{
			slot.view->Release();
			slot.view = 0;
		}
		if (slot.resource != 0)
		{
			slot.resource->Release();
			slot.resource = 0;
		}
	}

	HRESULT recreateBuffer(ResourceSlot &slot)
	{
		const BufferDescriptor &descriptor = slot.bufferDescriptor;
		D3D11_BUFFER_DESC nativeDescriptor;
		memset(&nativeDescriptor, 0, sizeof(nativeDescriptor));
		nativeDescriptor.ByteWidth = static_cast<UINT>(descriptor.byteCount);
		if ((descriptor.binding & RENDER_BUFFER_VERTEX) != 0)
		{
			nativeDescriptor.BindFlags |= D3D11_BIND_VERTEX_BUFFER;
		}
		if ((descriptor.binding & RENDER_BUFFER_INDEX) != 0)
		{
			nativeDescriptor.BindFlags |= D3D11_BIND_INDEX_BUFFER;
		}
		if ((descriptor.binding & RENDER_BUFFER_CONSTANT) != 0)
		{
			nativeDescriptor.BindFlags |= D3D11_BIND_CONSTANT_BUFFER;
		}
		setNativeUsage(descriptor.usage, &nativeDescriptor.Usage,
			&nativeDescriptor.CPUAccessFlags);
		D3D11_SUBRESOURCE_DATA initialData;
		memset(&initialData, 0, sizeof(initialData));
		initialData.pSysMem = slot.shadow.empty() ? 0 : &slot.shadow[0];
		return m_device->CreateBuffer(&nativeDescriptor,
			initialData.pSysMem == 0 ? 0 : &initialData,
			reinterpret_cast<ID3D11Buffer **>(&slot.resource));
	}

	HRESULT recreateTexture(ResourceSlot &slot)
	{
		const TextureDescriptor &descriptor = slot.textureDescriptor;
		if ((descriptor.dimension != RENDER_TEXTURE_2D &&
			descriptor.dimension != RENDER_TEXTURE_CUBE) ||
			(descriptor.dimension == RENDER_TEXTURE_2D &&
				descriptor.arrayCount != 1) ||
			(descriptor.dimension == RENDER_TEXTURE_CUBE &&
				(descriptor.width != descriptor.height ||
				descriptor.arrayCount != 6 ||
				(descriptor.binding & (RENDER_TEXTURE_RENDER_TARGET |
					RENDER_TEXTURE_DEPTH_STENCIL)) != 0)))
		{
			return E_INVALIDARG;
		}
		const DXGI_FORMAT format = TranslateFormat(descriptor.format);
		D3D11_TEXTURE2D_DESC nativeDescriptor;
		memset(&nativeDescriptor, 0, sizeof(nativeDescriptor));
		nativeDescriptor.Width = descriptor.width;
		nativeDescriptor.Height = descriptor.height;
		nativeDescriptor.MipLevels = descriptor.mipCount;
		nativeDescriptor.ArraySize = descriptor.arrayCount;
		nativeDescriptor.Format = format;
		nativeDescriptor.SampleDesc.Count = 1;
		if (descriptor.dimension == RENDER_TEXTURE_CUBE)
		{
			nativeDescriptor.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;
		}
		if ((descriptor.binding & RENDER_TEXTURE_SHADER_RESOURCE) != 0)
		{
			nativeDescriptor.BindFlags |= D3D11_BIND_SHADER_RESOURCE;
		}
		if ((descriptor.binding & RENDER_TEXTURE_RENDER_TARGET) != 0)
		{
			nativeDescriptor.BindFlags |= D3D11_BIND_RENDER_TARGET;
		}
		if ((descriptor.binding & RENDER_TEXTURE_DEPTH_STENCIL) != 0)
		{
			nativeDescriptor.BindFlags |= D3D11_BIND_DEPTH_STENCIL;
		}
		setNativeUsage(descriptor.usage, &nativeDescriptor.Usage,
			&nativeDescriptor.CPUAccessFlags);
		std::vector<D3D11_SUBRESOURCE_DATA> initialData;
		if (!slot.gpuAuthoritative && !slot.subresourceOffsets.empty())
		{
			try
			{
				initialData.resize(slot.subresourceOffsets.size());
			}
			catch (...)
			{
				return E_OUTOFMEMORY;
			}
			for (unsigned int i = 0; i < initialData.size(); ++i)
			{
				initialData[i].pSysMem = &slot.shadow[slot.subresourceOffsets[i]];
				initialData[i].SysMemPitch =
					static_cast<UINT>(slot.subresourceRowPitches[i]);
				initialData[i].SysMemSlicePitch =
					static_cast<UINT>(slot.subresourceSlicePitches[i]);
			}
		}
		ID3D11Texture2D *nativeTexture = 0;
		HRESULT result = m_device->CreateTexture2D(&nativeDescriptor,
			initialData.empty() ? 0 : &initialData[0], &nativeTexture);
		if (FAILED(result))
		{
			return result;
		}
		slot.resource = nativeTexture;
		if ((descriptor.binding & RENDER_TEXTURE_SHADER_RESOURCE) != 0)
		{
			D3D11_SHADER_RESOURCE_VIEW_DESC viewDescriptor;
			D3D11_SHADER_RESOURCE_VIEW_DESC *viewDescriptorPointer = 0;
			if (descriptor.dimension == RENDER_TEXTURE_CUBE)
			{
				memset(&viewDescriptor, 0, sizeof(viewDescriptor));
				viewDescriptor.Format = format;
				if (descriptor.arrayCount == 6)
				{
					viewDescriptor.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
					viewDescriptor.TextureCube.MostDetailedMip = 0;
					viewDescriptor.TextureCube.MipLevels = descriptor.mipCount;
				}
				else
				{
					viewDescriptor.ViewDimension =
						D3D11_SRV_DIMENSION_TEXTURECUBEARRAY;
					viewDescriptor.TextureCubeArray.MostDetailedMip = 0;
					viewDescriptor.TextureCubeArray.MipLevels = descriptor.mipCount;
					viewDescriptor.TextureCubeArray.First2DArrayFace = 0;
					viewDescriptor.TextureCubeArray.NumCubes =
						descriptor.arrayCount / 6;
				}
				viewDescriptorPointer = &viewDescriptor;
			}
			result = m_device->CreateShaderResourceView(nativeTexture,
				viewDescriptorPointer, &slot.view);
		}
		if (SUCCEEDED(result) &&
			(descriptor.binding & RENDER_TEXTURE_RENDER_TARGET) != 0)
		{
			result = m_device->CreateRenderTargetView(nativeTexture, 0,
				&slot.renderTarget);
		}
		if (SUCCEEDED(result) &&
			(descriptor.binding & RENDER_TEXTURE_DEPTH_STENCIL) != 0)
		{
			result = m_device->CreateDepthStencilView(nativeTexture, 0,
				&slot.depthStencil);
		}
		if (SUCCEEDED(result) && slot.gpuAuthoritative && m_context != 0)
		{
			// GPU-authoritative render targets have no valid CPU recovery image.
			// Start the replacement from a known value so a producer that has not
			// rendered yet cannot expose undefined memory after recovery.
			const float clearColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
			if (slot.renderTarget != 0)
			{
				m_context->ClearRenderTargetView(slot.renderTarget, clearColor);
			}
			if (slot.depthStencil != 0)
			{
				m_context->ClearDepthStencilView(slot.depthStencil,
					D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
			}
		}
		if (FAILED(result))
		{
			releaseNativeSlot(slot);
		}
		return result;
	}

	HRESULT createBackBufferTargets(unsigned int width, unsigned int height)
	{
		if (m_swapChain == 0 || width == 0 || height == 0)
		{
			return E_INVALIDARG;
		}
		ID3D11Texture2D *backBuffer = 0;
		HRESULT result = m_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D),
			reinterpret_cast<void **>(&backBuffer));
		if (SUCCEEDED(result))
		{
			result = m_device->CreateRenderTargetView(backBuffer, 0,
				&m_renderTarget);
			if (SUCCEEDED(result))
			{
				backBuffer->AddRef();
				m_renderTargetResource = backBuffer;
			}
		}
		if (backBuffer != 0)
		{
			backBuffer->Release();
		}
		if (FAILED(result))
		{
			return result;
		}

		D3D11_TEXTURE2D_DESC depthDescriptor;
		memset(&depthDescriptor, 0, sizeof(depthDescriptor));
		depthDescriptor.Width = width;
		depthDescriptor.Height = height;
		depthDescriptor.MipLevels = 1;
		depthDescriptor.ArraySize = 1;
		depthDescriptor.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
		depthDescriptor.SampleDesc.Count = 1;
		depthDescriptor.Usage = D3D11_USAGE_DEFAULT;
		depthDescriptor.BindFlags = D3D11_BIND_DEPTH_STENCIL;
		result = m_device->CreateTexture2D(&depthDescriptor, 0, &m_depthTexture);
		if (SUCCEEDED(result))
		{
			result = m_device->CreateDepthStencilView(m_depthTexture, 0,
				&m_depthStencil);
		}
		if (FAILED(result))
		{
			releaseBackBufferTargets();
		}
		else
		{
			// The legacy API publishes a full-back-buffer viewport when a device or reset
			// completes. Mirror that default here because the legacy state cache
			// may suppress its first SetViewport call after this backend starts.
			D3D11_VIEWPORT viewport;
			viewport.TopLeftX = 0.0f;
			viewport.TopLeftY = 0.0f;
			viewport.Width = static_cast<float>(width);
			viewport.Height = static_cast<float>(height);
			viewport.MinDepth = 0.0f;
			viewport.MaxDepth = 1.0f;
			m_context->RSSetViewports(1, &viewport);
			m_viewportX = 0.0f;
			m_viewportY = 0.0f;
			m_viewportWidth = static_cast<float>(width);
			m_viewportHeight = static_cast<float>(height);
			m_viewportMinimumDepth = 0.0f;
			m_viewportMaximumDepth = 1.0f;
			m_viewportBound = true;
		}
		return result;
	}

	void releaseBackBufferTargets()
	{
		m_activeRenderTarget = 0;
		m_activeDepthStencil = 0;
		m_activeColorResource = 0;
		m_activeDepthResource = 0;
		m_renderTargetsBound = false;
		m_viewportBound = false;
		if (m_depthStencil != 0)
		{
			m_depthStencil->Release();
			m_depthStencil = 0;
		}
		if (m_depthTexture != 0)
		{
			m_depthTexture->Release();
			m_depthTexture = 0;
		}
		if (m_renderTarget != 0)
		{
			m_renderTarget->Release();
			m_renderTarget = 0;
		}
		if (m_renderTargetResource != 0)
		{
			m_renderTargetResource->Release();
			m_renderTargetResource = 0;
		}
	}

	unsigned int nextStateUseSerial()
	{
		++m_stateUseSerial;
		if (m_stateUseSerial == 0)
		{
			// Keep the ordering deterministic across the (very long) serial
			// lifetime. Existing entries become older than the first post-wrap
			// access without requiring a signed comparison.
			m_stateUseSerial = 1;
			for (unsigned int index = 0; index < m_blendStates.size(); ++index)
			{
				m_blendStates[index].lastUsedSerial = 0;
			}
			for (unsigned int index = 0; index < m_depthStates.size(); ++index)
			{
				m_depthStates[index].lastUsedSerial = 0;
			}
			for (unsigned int index = 0; index < m_rasterizerStates.size(); ++index)
			{
				m_rasterizerStates[index].lastUsedSerial = 0;
			}
			for (unsigned int index = 0; index < m_samplerStates.size(); ++index)
			{
				m_samplerStates[index].lastUsedSerial = 0;
			}
			for (unsigned int index = 0; index < m_inputLayouts.size(); ++index)
			{
				m_inputLayouts[index].lastUsedSerial = 0;
			}
		}
		return m_stateUseSerial;
	}

	bool evictBlendState()
	{
		if (m_blendStates.size() < STATE_CACHE_CAPACITY)
		{
			return true;
		}
		unsigned int oldest = static_cast<unsigned int>(m_blendStates.size());
		for (unsigned int index = 0; index < m_blendStates.size(); ++index)
		{
			if (m_blendStates[index].state == m_boundBlendState)
			{
				continue;
			}
			if (oldest == m_blendStates.size() ||
				m_blendStates[index].lastUsedSerial <
				m_blendStates[oldest].lastUsedSerial)
			{
				oldest = index;
			}
		}
		if (oldest == m_blendStates.size())
		{
			return false;
		}
		m_blendStates[oldest].state->Release();
		m_blendStates.erase(m_blendStates.begin() + oldest);
		return true;
	}

	bool evictDepthState()
	{
		if (m_depthStates.size() < STATE_CACHE_CAPACITY)
		{
			return true;
		}
		unsigned int oldest = static_cast<unsigned int>(m_depthStates.size());
		for (unsigned int index = 0; index < m_depthStates.size(); ++index)
		{
			if (m_depthStates[index].state == m_boundDepthState)
			{
				continue;
			}
			if (oldest == m_depthStates.size() ||
				m_depthStates[index].lastUsedSerial <
				m_depthStates[oldest].lastUsedSerial)
			{
				oldest = index;
			}
		}
		if (oldest == m_depthStates.size())
		{
			return false;
		}
		m_depthStates[oldest].state->Release();
		m_depthStates.erase(m_depthStates.begin() + oldest);
		return true;
	}

	bool evictRasterizerState()
	{
		if (m_rasterizerStates.size() < STATE_CACHE_CAPACITY)
		{
			return true;
		}
		unsigned int oldest = static_cast<unsigned int>(m_rasterizerStates.size());
		for (unsigned int index = 0; index < m_rasterizerStates.size(); ++index)
		{
			if (m_rasterizerStates[index].state == m_boundRasterizerState)
			{
				continue;
			}
			if (oldest == m_rasterizerStates.size() ||
				m_rasterizerStates[index].lastUsedSerial <
				m_rasterizerStates[oldest].lastUsedSerial)
			{
				oldest = index;
			}
		}
		if (oldest == m_rasterizerStates.size())
		{
			return false;
		}
		m_rasterizerStates[oldest].state->Release();
		m_rasterizerStates.erase(m_rasterizerStates.begin() + oldest);
		return true;
	}

	bool evictSamplerState()
	{
		if (m_samplerStates.size() < STATE_CACHE_CAPACITY)
		{
			return true;
		}
		unsigned int oldest = static_cast<unsigned int>(m_samplerStates.size());
		for (unsigned int index = 0; index < m_samplerStates.size(); ++index)
		{
			bool bound = false;
			for (unsigned int stage = 0; stage < LEGACY_TEXTURE_STAGE_COUNT;
				++stage)
			{
				if (m_boundSamplerStates[stage] == m_samplerStates[index].state)
				{
					bound = true;
					break;
				}
			}
			if (bound)
			{
				continue;
			}
			if (oldest == m_samplerStates.size() ||
				m_samplerStates[index].lastUsedSerial <
				m_samplerStates[oldest].lastUsedSerial)
			{
				oldest = index;
			}
		}
		if (oldest == m_samplerStates.size())
		{
			return false;
		}
		m_samplerStates[oldest].state->Release();
		m_samplerStates.erase(m_samplerStates.begin() + oldest);
		return true;
	}

	bool evictInputLayout()
	{
		if (m_inputLayouts.size() < STATE_CACHE_CAPACITY)
		{
			return true;
		}
		unsigned int oldest = static_cast<unsigned int>(m_inputLayouts.size());
		for (unsigned int index = 0; index < m_inputLayouts.size(); ++index)
		{
			if (m_inputLayouts[index].layout == m_boundInputLayout)
			{
				continue;
			}
			if (oldest == m_inputLayouts.size() ||
				m_inputLayouts[index].lastUsedSerial <
				m_inputLayouts[oldest].lastUsedSerial)
			{
				oldest = index;
			}
		}
		if (oldest == m_inputLayouts.size())
		{
			return false;
		}
		m_inputLayouts[oldest].layout->Release();
		m_inputLayouts.erase(m_inputLayouts.begin() + oldest);
		return true;
	}

	HRESULT findOrCreateBlendState(const D3D11_BLEND_DESC &descriptor,
		ID3D11BlendState **state)
	{
		for (unsigned int index = 0; index < m_blendStates.size(); ++index)
		{
			if (memcmp(&m_blendStates[index].descriptor, &descriptor,
				sizeof(descriptor)) == 0)
			{
				m_blendStates[index].lastUsedSerial = nextStateUseSerial();
				*state = m_blendStates[index].state;
				return S_OK;
			}
		}
		if (!evictBlendState())
		{
			return E_OUTOFMEMORY;
		}
		BlendStateEntry entry;
		entry.descriptor = descriptor;
		entry.state = 0;
		entry.lastUsedSerial = nextStateUseSerial();
		HRESULT result = m_device->CreateBlendState(&descriptor, &entry.state);
		if (FAILED(result))
		{
			return result;
		}
		try
		{
			m_blendStates.push_back(entry);
		}
		catch (...)
		{
			entry.state->Release();
			return E_OUTOFMEMORY;
		}
		*state = entry.state;
		return S_OK;
	}

	HRESULT findOrCreateDepthState(const D3D11_DEPTH_STENCIL_DESC &descriptor,
		ID3D11DepthStencilState **state)
	{
		for (unsigned int index = 0; index < m_depthStates.size(); ++index)
		{
			if (memcmp(&m_depthStates[index].descriptor, &descriptor,
				sizeof(descriptor)) == 0)
			{
				m_depthStates[index].lastUsedSerial = nextStateUseSerial();
				*state = m_depthStates[index].state;
				return S_OK;
			}
		}
		if (!evictDepthState())
		{
			return E_OUTOFMEMORY;
		}
		DepthStencilStateEntry entry;
		entry.descriptor = descriptor;
		entry.state = 0;
		entry.lastUsedSerial = nextStateUseSerial();
		HRESULT result = m_device->CreateDepthStencilState(&descriptor, &entry.state);
		if (FAILED(result))
		{
			return result;
		}
		try
		{
			m_depthStates.push_back(entry);
		}
		catch (...)
		{
			entry.state->Release();
			return E_OUTOFMEMORY;
		}
		*state = entry.state;
		return S_OK;
	}

	HRESULT findOrCreateRasterizerState(const D3D11_RASTERIZER_DESC &descriptor,
		ID3D11RasterizerState **state)
	{
		for (unsigned int index = 0; index < m_rasterizerStates.size(); ++index)
		{
			if (memcmp(&m_rasterizerStates[index].descriptor, &descriptor,
				sizeof(descriptor)) == 0)
			{
				m_rasterizerStates[index].lastUsedSerial = nextStateUseSerial();
				*state = m_rasterizerStates[index].state;
				return S_OK;
			}
		}
		if (!evictRasterizerState())
		{
			return E_OUTOFMEMORY;
		}
		RasterizerStateEntry entry;
		entry.descriptor = descriptor;
		entry.state = 0;
		entry.lastUsedSerial = nextStateUseSerial();
		HRESULT result = m_device->CreateRasterizerState(&descriptor, &entry.state);
		if (FAILED(result))
		{
			return result;
		}
		try
		{
			m_rasterizerStates.push_back(entry);
		}
		catch (...)
		{
			entry.state->Release();
			return E_OUTOFMEMORY;
		}
		*state = entry.state;
		return S_OK;
	}

	HRESULT findOrCreateSamplerState(const D3D11_SAMPLER_DESC &descriptor,
		ID3D11SamplerState **state)
	{
		for (unsigned int index = 0; index < m_samplerStates.size(); ++index)
		{
			if (memcmp(&m_samplerStates[index].descriptor, &descriptor,
				sizeof(descriptor)) == 0)
			{
				m_samplerStates[index].lastUsedSerial = nextStateUseSerial();
				*state = m_samplerStates[index].state;
				return S_OK;
			}
		}
		if (!evictSamplerState())
		{
			return E_OUTOFMEMORY;
		}
		SamplerStateEntry entry;
		entry.descriptor = descriptor;
		entry.state = 0;
		entry.lastUsedSerial = nextStateUseSerial();
		HRESULT result = m_device->CreateSamplerState(&descriptor, &entry.state);
		if (FAILED(result))
		{
			return result;
		}
		try
		{
			m_samplerStates.push_back(entry);
		}
		catch (...)
		{
			entry.state->Release();
			return E_OUTOFMEMORY;
		}
		*state = entry.state;
		return S_OK;
	}

	HRESULT findOrCreateInputLayout(const LegacyVertexLayout &descriptor,
		ID3D11InputLayout **layout)
	{
		if (layout == 0 || descriptor.stride == 0 ||
			descriptor.elementCount == 0 ||
			descriptor.elementCount > LegacyVertexLayout::MAX_ELEMENT_COUNT)
		{
			return E_INVALIDARG;
		}
		for (unsigned int cached = 0; cached < m_inputLayouts.size(); ++cached)
		{
			if (EqualVertexLayouts(m_inputLayouts[cached].descriptor, descriptor))
			{
				m_inputLayouts[cached].lastUsedSerial = nextStateUseSerial();
				*layout = m_inputLayouts[cached].layout;
				return S_OK;
			}
		}
		if (!evictInputLayout())
		{
			return E_OUTOFMEMORY;
		}
		D3D11_INPUT_ELEMENT_DESC nativeElements[
			LegacyVertexLayout::MAX_ELEMENT_COUNT + LEGACY_TEXTURE_STAGE_COUNT + 3];
		bool hasPosition = false;
		bool hasNormal = false;
		bool hasDiffuse = false;
		bool hasSpecular = false;
		bool hasTextureCoordinate[LEGACY_TEXTURE_STAGE_COUNT] = { false };
		unsigned int positionOffset = 0;
		for (unsigned int index = 0; index < descriptor.elementCount; ++index)
		{
			const LegacyVertexElement &element = descriptor.elements[index];
			const unsigned int byteCount = VertexDataByteCount(element.format);
			if (byteCount == 0 || element.byteOffset > descriptor.stride ||
				byteCount > descriptor.stride - element.byteOffset)
			{
				return E_INVALIDARG;
			}
			const char *semanticName = 0;
			unsigned int semanticIndex = element.semanticIndex;
			switch (element.semantic)
			{
			case RENDER_VERTEX_SEMANTIC_POSITION:
				if (hasPosition || semanticIndex != 0 ||
					(element.format != RENDER_VERTEX_DATA_FLOAT3 &&
					 element.format != RENDER_VERTEX_DATA_FLOAT4))
				{
					return E_INVALIDARG;
				}
				hasPosition = true;
				positionOffset = element.byteOffset;
				semanticName = "POSITION";
				break;
			case RENDER_VERTEX_SEMANTIC_NORMAL:
				if (hasNormal || semanticIndex != 0 ||
					element.format != RENDER_VERTEX_DATA_FLOAT3)
				{
					return E_INVALIDARG;
				}
				hasNormal = true;
				semanticName = "NORMAL";
				break;
			case RENDER_VERTEX_SEMANTIC_DIFFUSE:
				if (hasDiffuse || semanticIndex != 0 ||
					element.format != RENDER_VERTEX_DATA_COLOR_BGRA8)
				{
					return E_INVALIDARG;
				}
				hasDiffuse = true;
				semanticName = "COLOR";
				semanticIndex = 0;
				break;
			case RENDER_VERTEX_SEMANTIC_SPECULAR:
				if (hasSpecular || semanticIndex != 0 ||
					element.format != RENDER_VERTEX_DATA_COLOR_BGRA8)
				{
					return E_INVALIDARG;
				}
				hasSpecular = true;
				semanticName = "COLOR";
				semanticIndex = 1;
				break;
			case RENDER_VERTEX_SEMANTIC_TEXTURE_COORDINATE:
				if (semanticIndex >= LEGACY_TEXTURE_STAGE_COUNT ||
					hasTextureCoordinate[semanticIndex] ||
					element.format == RENDER_VERTEX_DATA_COLOR_BGRA8)
				{
					return E_INVALIDARG;
				}
				hasTextureCoordinate[semanticIndex] = true;
				semanticName = "TEXCOORD";
				break;
			default:
				return E_INVALIDARG;
			}
			for (unsigned int previous = 0; previous < index; ++previous)
			{
				if (nativeElements[previous].SemanticIndex == semanticIndex &&
					strcmp(nativeElements[previous].SemanticName, semanticName) == 0)
				{
					return E_INVALIDARG;
				}
			}
			nativeElements[index].SemanticName = semanticName;
			nativeElements[index].SemanticIndex = semanticIndex;
			nativeElements[index].Format = TranslateVertexDataFormat(element.format);
			nativeElements[index].InputSlot = 0;
			nativeElements[index].AlignedByteOffset = element.byteOffset;
			nativeElements[index].InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
			nativeElements[index].InstanceDataStepRate = 0;
		}
		if (!hasPosition)
		{
			return E_INVALIDARG;
		}
		unsigned int nativeElementCount = descriptor.elementCount;
		if (!hasNormal)
		{
			D3D11_INPUT_ELEMENT_DESC &element =
				nativeElements[nativeElementCount++];
			element.SemanticName = "NORMAL";
			element.SemanticIndex = 0;
			element.Format = DXGI_FORMAT_R32G32B32_FLOAT;
			element.InputSlot = 0;
			element.AlignedByteOffset = positionOffset;
			element.InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
			element.InstanceDataStepRate = 0;
		}
		if (!hasDiffuse)
		{
			D3D11_INPUT_ELEMENT_DESC &element =
				nativeElements[nativeElementCount++];
			element.SemanticName = "COLOR";
			element.SemanticIndex = 0;
			element.Format = DXGI_FORMAT_R32G32B32_FLOAT;
			element.InputSlot = 0;
			element.AlignedByteOffset = positionOffset;
			element.InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
			element.InstanceDataStepRate = 0;
		}
		if (!hasSpecular)
		{
			D3D11_INPUT_ELEMENT_DESC &element =
				nativeElements[nativeElementCount++];
			element.SemanticName = "COLOR";
			element.SemanticIndex = 1;
			element.Format = DXGI_FORMAT_R32G32B32_FLOAT;
			element.InputSlot = 0;
			element.AlignedByteOffset = positionOffset;
			element.InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
			element.InstanceDataStepRate = 0;
		}
		for (unsigned int coordinate = 0;
			coordinate < LEGACY_TEXTURE_STAGE_COUNT; ++coordinate)
		{
			if (!hasTextureCoordinate[coordinate])
			{
				D3D11_INPUT_ELEMENT_DESC &element =
					nativeElements[nativeElementCount++];
				element.SemanticName = "TEXCOORD";
				element.SemanticIndex = coordinate;
				element.Format = DXGI_FORMAT_R32G32B32_FLOAT;
				element.InputSlot = 0;
				element.AlignedByteOffset = positionOffset;
				element.InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
				element.InstanceDataStepRate = 0;
			}
		}
		InputLayoutEntry entry;
		entry.descriptor = descriptor;
		entry.layout = 0;
		entry.lastUsedSerial = nextStateUseSerial();
		HRESULT result = m_device->CreateInputLayout(nativeElements,
			nativeElementCount, g_LegacyTexturedVS,
			sizeof(g_LegacyTexturedVS), &entry.layout);
		if (FAILED(result))
		{
			return result;
		}
		try
		{
			m_inputLayouts.push_back(entry);
		}
		catch (...)
		{
			entry.layout->Release();
			return E_OUTOFMEMORY;
		}
		*layout = entry.layout;
		return S_OK;
	}

	HRESULT createSwapChain(HWND window)
	{
		IDXGIDevice *dxgiDevice = 0;
		IDXGIAdapter *adapter = 0;
		IDXGIFactory2 *factory = 0;
		HRESULT result = m_device->QueryInterface(__uuidof(IDXGIDevice),
			reinterpret_cast<void **>(&dxgiDevice));
		if (SUCCEEDED(result))
		{
			result = dxgiDevice->GetAdapter(&adapter);
		}
		if (SUCCEEDED(result))
		{
			result = adapter->GetParent(__uuidof(IDXGIFactory2),
				reinterpret_cast<void **>(&factory));
		}
		if (SUCCEEDED(result))
		{
			DXGI_SWAP_CHAIN_DESC1 descriptor;
			memset(&descriptor, 0, sizeof(descriptor));
			descriptor.Width = m_width;
			descriptor.Height = m_height;
			descriptor.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
			descriptor.SampleDesc.Count = 1;
			descriptor.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
			descriptor.BufferCount = 2;
			descriptor.Scaling = DXGI_SCALING_STRETCH;
			descriptor.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
			descriptor.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
			result = factory->CreateSwapChainForHwnd(m_device, window,
				&descriptor, 0, 0, &m_swapChain);
			if (SUCCEEDED(result))
			{
				factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);
			}
		}
		if (factory != 0)
		{
			factory->Release();
		}
		if (adapter != 0)
		{
			adapter->Release();
		}
		if (dxgiDevice != 0)
		{
			dxgiDevice->Release();
		}
		return result;
	}

	HRESULT createNativeDevice(const RenderDeviceParameters &parameters)
	{
		UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
		if (parameters.enableDebugLayer)
		{
			flags |= D3D11_CREATE_DEVICE_DEBUG;
		}
		const D3D_FEATURE_LEVEL requestedLevel = D3D_FEATURE_LEVEL_11_0;
		D3D_FEATURE_LEVEL obtainedLevel = D3D_FEATURE_LEVEL_9_1;
		IDXGIAdapter1 *selectedAdapter = 0;
		if (parameters.adapterIndex != UINT_MAX)
		{
			IDXGIFactory1 *factory = 0;
			HRESULT adapterResult = CreateDXGIFactory1(__uuidof(IDXGIFactory1),
				reinterpret_cast<void **>(&factory));
			if (SUCCEEDED(adapterResult))
			{
				adapterResult = factory->EnumAdapters1(parameters.adapterIndex,
					&selectedAdapter);
				factory->Release();
			}
			if (FAILED(adapterResult) || selectedAdapter == 0)
			{
				if (selectedAdapter != 0)
				{
					selectedAdapter->Release();
				}
				return E_INVALIDARG;
			}
		}
		const D3D_DRIVER_TYPE driverType = selectedAdapter == 0 ?
			D3D_DRIVER_TYPE_HARDWARE : D3D_DRIVER_TYPE_UNKNOWN;
		HRESULT result = D3D11CreateDevice(selectedAdapter, driverType, 0,
			flags, &requestedLevel, 1, D3D11_SDK_VERSION, &m_device,
			&obtainedLevel, &m_context);
		// The debug layer is an optional Windows SDK component. A request for
		// diagnostics must never make the retail renderer fail to initialize when
		// that component is absent; retry the same adapter without the flag only
		// for the documented missing-component result.
		if (FAILED(result) && parameters.enableDebugLayer &&
			result == DXGI_ERROR_SDK_COMPONENT_MISSING)
		{
			flags &= ~static_cast<UINT>(D3D11_CREATE_DEVICE_DEBUG);
			releaseDeviceObjects();
			result = D3D11CreateDevice(selectedAdapter, driverType, 0,
				flags, &requestedLevel, 1, D3D11_SDK_VERSION, &m_device,
				&obtainedLevel, &m_context);
		}
		if (selectedAdapter != 0)
		{
			selectedAdapter->Release();
		}
		if (FAILED(result) && parameters.adapterIndex == UINT_MAX &&
			parameters.allowSoftwareFallback)
		{
			result = D3D11CreateDevice(0, D3D_DRIVER_TYPE_WARP, 0,
				flags, &requestedLevel, 1, D3D11_SDK_VERSION, &m_device,
				&obtainedLevel, &m_context);
			if (FAILED(result) && parameters.enableDebugLayer &&
				result == DXGI_ERROR_SDK_COMPONENT_MISSING)
			{
				flags &= ~static_cast<UINT>(D3D11_CREATE_DEVICE_DEBUG);
				releaseDeviceObjects();
				result = D3D11CreateDevice(0, D3D_DRIVER_TYPE_WARP, 0,
					flags, &requestedLevel, 1, D3D11_SDK_VERSION, &m_device,
					&obtainedLevel, &m_context);
			}
		}
		if (SUCCEEDED(result) && obtainedLevel != D3D_FEATURE_LEVEL_11_0)
		{
			releaseDeviceObjects();
			return DXGI_ERROR_UNSUPPORTED;
		}
		if (SUCCEEDED(result))
		{
			m_debugLayerActive = false;
			if ((flags & D3D11_CREATE_DEVICE_DEBUG) != 0)
			{
				ID3D11Debug *debugLayer = 0;
				if (SUCCEEDED(m_device->QueryInterface(__uuidof(ID3D11Debug),
					reinterpret_cast<void **>(&debugLayer))))
				{
					m_debugLayer = debugLayer;
					m_debugLayerActive = true;
				}
			}
		}
		return result;
	}

	bool releaseSlot(GpuHandle handle, ResourceSlot &slot)
	{
		if (slot.depthStencil != 0)
		{
			slot.depthStencil->Release();
		}
		if (slot.renderTarget != 0)
		{
			slot.renderTarget->Release();
		}
		if (slot.view != 0)
		{
			slot.view->Release();
		}
		if (slot.resource != 0)
		{
			slot.resource->Release();
		}
		slot.resource = 0;
		slot.view = 0;
		slot.renderTarget = 0;
		slot.depthStencil = 0;
		slot.kind = RESOURCE_NONE;
		slot.usage = RENDER_USAGE_DEFAULT;
		slot.binding = 0;
		slot.byteCount = 0;
		slot.gpuAuthoritative = false;
		// This logical resource is gone, unlike an in-place refresh or device
		// recovery. Do not retain its largest CPU shadow in the reusable slot.
		std::vector<unsigned char>().swap(slot.shadow);
		std::vector<size_t>().swap(slot.subresourceOffsets);
		std::vector<size_t>().swap(slot.subresourceRowPitches);
		std::vector<size_t>().swap(slot.subresourceSlicePitches);
		return m_handles->release(handle);
	}

	void shutdownInternal()
	{
		m_frameOpen = false;
		m_pipelineBound = false;
		m_vertexBufferBound = false;
		m_indexBufferBound = false;
		m_topologyBound = false;
		if (m_handles != 0)
		{
			for (unsigned int i = 0; i < m_resources.size(); ++i)
			{
				ResourceSlot &slot = m_resources[i];
				if (slot.depthStencil != 0)
				{
					slot.depthStencil->Release();
					slot.depthStencil = 0;
				}
				if (slot.renderTarget != 0)
				{
					slot.renderTarget->Release();
					slot.renderTarget = 0;
				}
				if (slot.view != 0)
				{
					slot.view->Release();
					slot.view = 0;
				}
				if (slot.resource != 0)
				{
					slot.resource->Release();
					slot.resource = 0;
				}
				slot.shadow.clear();
			}
		}
		m_resources.clear();
		delete m_handles;
		m_handles = 0;
		if (m_context != 0)
		{
			m_context->ClearState();
			m_context->Flush();
			invalidateContextBindings();
		}
		releasePipelineResources();
		releaseBackBufferTargets();
		releaseSwapChain();
		releaseImmediateContext();
		reportLiveObjects();
		releaseDeviceObjects();
		m_ownerThread = 0;
		m_activeRenderTarget = 0;
		m_activeDepthStencil = 0;
		m_activeColorResource = 0;
		m_activeDepthResource = 0;
		m_initialized = false;
		m_width = 0;
		m_height = 0;
	}

	void releasePipelineResources()
	{
		for (unsigned int index = 0; index < m_inputLayouts.size(); ++index)
		{
			m_inputLayouts[index].layout->Release();
		}
		m_inputLayouts.clear();
		for (unsigned int index = 0; index < m_blendStates.size(); ++index)
		{
			m_blendStates[index].state->Release();
		}
		m_blendStates.clear();
		for (unsigned int index = 0; index < m_depthStates.size(); ++index)
		{
			m_depthStates[index].state->Release();
		}
		m_depthStates.clear();
		for (unsigned int index = 0; index < m_rasterizerStates.size(); ++index)
		{
			m_rasterizerStates[index].state->Release();
		}
		m_rasterizerStates.clear();
		for (unsigned int index = 0; index < m_samplerStates.size(); ++index)
		{
			m_samplerStates[index].state->Release();
		}
		m_samplerStates.clear();
		for (unsigned int index = 0; index < TRANSFORM_CONSTANT_BUFFER_COUNT;
			++index)
		{
			if (m_transformConstants[index] != 0)
			{
				m_transformConstants[index]->Release();
				m_transformConstants[index] = 0;
			}
		}
		m_transformConstantCursor = 0;
		if (m_texturedPixelShader != 0)
		{
			m_texturedPixelShader->Release();
			m_texturedPixelShader = 0;
		}
		for (unsigned int programIndex = 0;
			programIndex < TERRAIN_PIXEL_PROGRAM_COUNT; ++programIndex)
		{
			if (m_terrainPixelShaders[programIndex] != 0)
			{
				m_terrainPixelShaders[programIndex]->Release();
				m_terrainPixelShaders[programIndex] = 0;
			}
		}
		if (m_texturedFixed2PixelShader != 0)
		{
			m_texturedFixed2PixelShader->Release();
			m_texturedFixed2PixelShader = 0;
		}
		if (m_texturedFixed1PixelShader != 0)
		{
			m_texturedFixed1PixelShader->Release();
			m_texturedFixed1PixelShader = 0;
		}
		if (m_texturedVertexShader != 0)
		{
			m_texturedVertexShader->Release();
			m_texturedVertexShader = 0;
		}
		if (m_waterRiverPixelShader != 0)
		{
			m_waterRiverPixelShader->Release();
			m_waterRiverPixelShader = 0;
		}
		if (m_waterFlatPixelShader != 0)
		{
			m_waterFlatPixelShader->Release();
			m_waterFlatPixelShader = 0;
		}
		if (m_seaWaveLayout != 0)
		{
			m_seaWaveLayout->Release();
			m_seaWaveLayout = 0;
		}
		if (m_seaWavePixelShader != 0)
		{
			m_seaWavePixelShader->Release();
			m_seaWavePixelShader = 0;
		}
		if (m_seaWaveVertexShader != 0)
		{
			m_seaWaveVertexShader->Release();
			m_seaWaveVertexShader = 0;
		}
		if (m_positionColorLayout != 0)
		{
			m_positionColorLayout->Release();
			m_positionColorLayout = 0;
		}
		if (m_pixelShader != 0)
		{
			m_pixelShader->Release();
			m_pixelShader = 0;
		}
		if (m_vertexShader != 0)
		{
			m_vertexShader->Release();
			m_vertexShader = 0;
		}
	}

	void releaseSwapChain()
	{
		if (m_swapChain != 0)
		{
			m_swapChain->Release();
			m_swapChain = 0;
		}
	}

	void releaseImmediateContext()
	{
		if (m_context != 0)
		{
			m_context->Release();
			m_context = 0;
		}
	}

	void releaseDeviceObjects()
	{
		invalidateContextBindings();
		releaseSwapChain();
		releaseImmediateContext();
		if (m_debugLayer != 0)
		{
			m_debugLayer->Release();
			m_debugLayer = 0;
		}
		m_debugLayerActive = false;
		if (m_device != 0)
		{
			m_device->Release();
			m_device = 0;
		}
	}

	ID3D11Device *m_device;
	ID3D11DeviceContext *m_context;
	ID3D11Debug *m_debugLayer;
	bool m_debugLayerActive;
	IDXGISwapChain1 *m_swapChain;
	ID3D11RenderTargetView *m_renderTarget;
	ID3D11Resource *m_renderTargetResource;
	ID3D11Texture2D *m_depthTexture;
	ID3D11DepthStencilView *m_depthStencil;
	ID3D11RenderTargetView *m_activeRenderTarget;
	ID3D11DepthStencilView *m_activeDepthStencil;
	ID3D11Resource *m_activeColorResource;
	ID3D11Resource *m_activeDepthResource;
	ID3D11Buffer *m_transformConstants[TRANSFORM_CONSTANT_BUFFER_COUNT];
	ID3D11VertexShader *m_vertexShader;
	ID3D11PixelShader *m_pixelShader;
	ID3D11InputLayout *m_positionColorLayout;
	ID3D11VertexShader *m_texturedVertexShader;
	ID3D11PixelShader *m_texturedPixelShader;
	ID3D11PixelShader *m_texturedFixed1PixelShader;
	ID3D11PixelShader *m_texturedFixed2PixelShader;
	ID3D11PixelShader *m_terrainPixelShaders[TERRAIN_PIXEL_PROGRAM_COUNT];
	ID3D11PixelShader *m_waterFlatPixelShader;
	ID3D11PixelShader *m_waterRiverPixelShader;
	ID3D11VertexShader *m_seaWaveVertexShader;
	ID3D11PixelShader *m_seaWavePixelShader;
	ID3D11InputLayout *m_seaWaveLayout;
	GpuHandleAllocator *m_handles;
	std::vector<ResourceSlot> m_resources;
	std::vector<BlendStateEntry> m_blendStates;
	std::vector<DepthStencilStateEntry> m_depthStates;
	std::vector<RasterizerStateEntry> m_rasterizerStates;
	std::vector<SamplerStateEntry> m_samplerStates;
	std::vector<InputLayoutEntry> m_inputLayouts;
	unsigned int m_stateUseSerial;
	DWORD m_ownerThread;
	bool m_initialized;
	bool m_frameOpen;
	bool m_pipelineBound;
	bool m_vertexBufferBound;
	bool m_indexBufferBound;
	bool m_topologyBound;
	bool m_enableVsync;
	unsigned int m_transformConstantCursor;
	unsigned int m_width;
	unsigned int m_height;
	float m_viewportX;
	float m_viewportY;
	float m_viewportWidth;
	float m_viewportHeight;
	bool m_hasVertexLayoutFlagsOverride;
	unsigned int m_vertexLayoutFlagsOverride;
	RenderDeviceParameters m_parameters;
	bool m_pipelineStateValid;
	D3D11_BLEND_DESC m_boundBlendDescriptor;
	D3D11_DEPTH_STENCIL_DESC m_boundDepthDescriptor;
	D3D11_RASTERIZER_DESC m_boundRasterizerDescriptor;
	unsigned int m_boundStencilReference;
	ID3D11BlendState *m_boundBlendState;
	ID3D11DepthStencilState *m_boundDepthState;
	ID3D11RasterizerState *m_boundRasterizerState;
	ID3D11InputLayout *m_boundInputLayout;
	ID3D11VertexShader *m_boundVertexShader;
	ID3D11PixelShader *m_boundPixelShader;
	ID3D11SamplerState *m_boundSamplerStates[LEGACY_TEXTURE_STAGE_COUNT];
	bool m_pipelineHasTextures;
	unsigned char m_cachedLegacyState[sizeof(LegacyLogicalState)];
	bool m_cachedLegacyStateValid;
	unsigned char m_cachedLegacyPipeline[sizeof(LegacyPipelineState)];
	bool m_cachedLegacyPipelineValid;
	LegacyVertexFormat m_cachedLegacyVertexFormat;
	unsigned int m_cachedLegacyTexturePresenceMask;
	unsigned int m_cachedLegacyCubeTextureMask;
	unsigned int m_cachedLegacySignedTextureMask;
	unsigned int m_cachedLegacyVertexLayoutFlags;
	ID3D11InputLayout *m_cachedLegacyInputLayout;
	LegacyTransformConstants m_lastTransformConstants;
	bool m_transformConstantsValid;
	bool m_transformConstantsChanged;
	bool m_hasInputLayoutOverride;
	ID3D11InputLayout *m_inputLayoutOverride;
	bool m_renderTargetsBound;
	bool m_textureBindingsValid;
	unsigned int m_boundCubeTextureMask;
	unsigned int m_boundSignedTextureMask;
	GpuHandle m_boundTextures[LEGACY_TEXTURE_STAGE_COUNT];
	GpuHandle m_boundVertexBuffer;
	unsigned int m_boundVertexStride;
	unsigned int m_boundVertexOffset;
	GpuHandle m_boundIndexBuffer;
	RenderFormat m_boundIndexFormat;
	unsigned int m_boundIndexOffset;
	RenderPrimitiveTopology m_boundTopology;
	bool m_viewportBound;
	float m_viewportMinimumDepth;
	float m_viewportMaximumDepth;
};
}

IRenderDevice *CreateD3D11RenderDevice()
{
	try
	{
		return new (std::nothrow) D3D11RenderDevice();
	}
	catch (...)
	{
		return 0;
	}
}
}
}
