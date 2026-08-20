#include "Renderer/RendererDevice.h"

#include <windows.h>
#include <d3d11.h>
#include <d3d11sdklayers.h>
#include <dxgi1_2.h>

#include "LegacyFixedFunctionPS.h"
#include "LegacyFixedFunctionVS.h"
#include "LegacyTexturedPS.h"
#include "LegacyTexturedVS.h"

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
const unsigned int TRANSFORM_CONSTANT_BUFFER_COUNT = 64;

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
	default:
		return DXGI_FORMAT_UNKNOWN;
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
};

struct DepthStencilStateEntry
{
	D3D11_DEPTH_STENCIL_DESC descriptor;
	ID3D11DepthStencilState *state;
};

struct RasterizerStateEntry
{
	D3D11_RASTERIZER_DESC descriptor;
	ID3D11RasterizerState *state;
};

struct SamplerStateEntry
{
	D3D11_SAMPLER_DESC descriptor;
	ID3D11SamplerState *state;
};

struct InputLayoutEntry
{
	LegacyVertexLayout descriptor;
	ID3D11InputLayout *layout;
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
		left.elementCount != right.elementCount)
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
		usage(RENDER_USAGE_DEFAULT), binding(0), byteCount(0)
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
	float materialEmissive[4];
	float globalAmbient[4];
	float lightDiffuse[LEGACY_LIGHT_COUNT][4];
	float lightAmbient[LEGACY_LIGHT_COUNT][4];
	float lightPositionAndType[LEGACY_LIGHT_COUNT][4];
	float lightDirectionAndEnabled[LEGACY_LIGHT_COUNT][4];
	float lightAttenuation[LEGACY_LIGHT_COUNT][4];
	float lightSpotParameters[LEGACY_LIGHT_COUNT][4];
	unsigned int lightingParameters[4];
	unsigned int vertexLayoutParameters[4];
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

class D3D11RenderDevice : public IRenderDevice, public IRenderContext
{
public:
	D3D11RenderDevice() : m_device(0), m_context(0), m_swapChain(0),
		m_renderTarget(0), m_depthTexture(0), m_depthStencil(0),
		m_activeRenderTarget(0), m_activeDepthStencil(0),
		m_vertexShader(0), m_pixelShader(0), m_positionColorLayout(0),
		m_texturedVertexShader(0), m_texturedPixelShader(0),
		m_texturedLayout(0),
		m_handles(0), m_ownerThread(0), m_initialized(false),
		m_frameOpen(false), m_pipelineBound(false), m_vertexBufferBound(false),
		m_indexBufferBound(false), m_topologyBound(false),
		m_enableVsync(true), m_transformConstantCursor(0), m_width(0), m_height(0)
	{
		memset(m_transformConstants, 0, sizeof(m_transformConstants));
	}

	virtual ~D3D11RenderDevice()
	{
		shutdown();
	}

	virtual RenderBackend backend() const
	{
		return RENDER_BACKEND_D3D11;
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
			result = createBackBufferTargets();
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
			(descriptor.dimension == RENDER_TEXTURE_CUBE &&
				(descriptor.width != descriptor.height ||
				descriptor.arrayCount < 6 || (descriptor.arrayCount % 6) != 0 ||
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
					const unsigned int mipHeight = descriptor.height >> mip;
					const size_t rowCount = mipHeight == 0 ? 1 : mipHeight;
					if (initialData[i].rowPitch > static_cast<size_t>(-1) / rowCount)
					{
						releaseSlot(handle, slot);
						return RENDER_RESULT_INVALID_ARGUMENT;
					}
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

	virtual bool destroyResource(GpuHandle resource)
	{
		if (!isOwner() || !m_handles->isLive(resource))
		{
			return false;
		}
		ResourceSlot &slot = m_resources[resource.index()];
		if (slot.kind == RESOURCE_TEXTURE)
		{
			ID3D11ShaderResourceView *emptyViews[LEGACY_TEXTURE_STAGE_COUNT] = { 0 };
			m_context->PSSetShaderResources(0, LEGACY_TEXTURE_STAGE_COUNT,
				emptyViews);
			if (slot.renderTarget == m_activeRenderTarget ||
				slot.depthStencil == m_activeDepthStencil)
			{
				m_context->OMSetRenderTargets(1, &m_renderTarget, m_depthStencil);
				m_activeRenderTarget = m_renderTarget;
				m_activeDepthStencil = m_depthStencil;
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
			}
			if ((slot.binding & RENDER_BUFFER_INDEX) != 0)
			{
				m_context->IASetIndexBuffer(0, DXGI_FORMAT_UNKNOWN, 0);
				m_indexBufferBound = false;
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
		m_context->ClearState();
		m_context->Flush();
		for (unsigned int i = 0; i < m_resources.size(); ++i)
		{
			releaseNativeSlot(m_resources[i]);
		}
		releasePipelineResources();
		releaseBackBufferTargets();
		releaseDeviceObjects();
		HRESULT result = createNativeDevice(m_parameters);
		if (SUCCEEDED(result) && m_parameters.window != 0)
		{
			result = createSwapChain(static_cast<HWND>(m_parameters.window));
			if (SUCCEEDED(result))
			{
				result = createBackBufferTargets();
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
			m_initialized = false;
			return TranslateResult(result);
		}
		m_activeRenderTarget = m_renderTarget;
		m_activeDepthStencil = m_depthStencil;
		m_pipelineBound = false;
		m_vertexBufferBound = false;
		m_indexBufferBound = false;
		m_topologyBound = false;
		return RENDER_RESULT_OK;
	}

	virtual RenderResult resize(unsigned int width, unsigned int height)
	{
		if (!isOwner() || width == 0 || height == 0 || m_frameOpen)
		{
			return RENDER_RESULT_INVALID_ARGUMENT;
		}
		if (m_swapChain != 0)
		{
			m_context->OMSetRenderTargets(0, 0, 0);
			releaseBackBufferTargets();
			HRESULT result = m_swapChain->ResizeBuffers(0, width, height,
				DXGI_FORMAT_UNKNOWN, 0);
			if (FAILED(result))
			{
				createBackBufferTargets();
				return TranslateResult(result);
			}
			m_width = width;
			m_height = height;
			result = createBackBufferTargets();
			if (FAILED(result))
			{
				return TranslateResult(result);
			}
		}
		m_width = width;
		m_height = height;
		return RENDER_RESULT_OK;
	}

	virtual RenderResult present()
	{
		if (!isOwner() || m_frameOpen)
		{
			return RENDER_RESULT_INVALID_ARGUMENT;
		}
		return m_swapChain == 0 ? RENDER_RESULT_OK :
			TranslateResult(m_swapChain->Present(m_enableVsync ? 1 : 0, 0));
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
		m_activeRenderTarget = m_renderTarget;
		m_activeDepthStencil = m_depthStencil;
		m_pipelineBound = false;
		m_vertexBufferBound = false;
		m_indexBufferBound = false;
		m_topologyBound = false;
		m_frameOpen = true;
		return RENDER_RESULT_OK;
	}

	virtual RenderResult updateBuffer(GpuHandle buffer, const void *data,
		size_t byteCount, size_t destinationOffset)
	{
		if (!isOwner() || !m_frameOpen || data == 0 || byteCount == 0 ||
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
		memcpy(&slot.shadow[destinationOffset], data, byteCount);
		if (slot.usage == RENDER_USAGE_DYNAMIC)
		{
			D3D11_MAPPED_SUBRESOURCE mapped;
			const HRESULT result = m_context->Map(slot.resource, 0,
				D3D11_MAP_WRITE_DISCARD, 0, &mapped);
			if (FAILED(result))
			{
				return TranslateResult(result);
			}
			memcpy(mapped.pData, &slot.shadow[0], slot.byteCount);
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

	virtual RenderResult setRenderTargets(GpuHandle colorTarget,
		GpuHandle depthTarget)
	{
		if (!isOwner() || !m_frameOpen)
		{
			return RENDER_RESULT_INVALID_ARGUMENT;
		}
		if (!colorTarget.isValid() && !depthTarget.isValid())
		{
			if (m_renderTarget == 0)
			{
				return RENDER_RESULT_UNSUPPORTED;
			}
			m_context->OMSetRenderTargets(1, &m_renderTarget, m_depthStencil);
			m_activeRenderTarget = m_renderTarget;
			m_activeDepthStencil = m_depthStencil;
			return RENDER_RESULT_OK;
		}
		if ((colorTarget.isValid() && !m_handles->isLive(colorTarget)) ||
			(depthTarget.isValid() && !m_handles->isLive(depthTarget)))
		{
			return RENDER_RESULT_INVALID_ARGUMENT;
		}
		ID3D11RenderTargetView *colorView = 0;
		if (colorTarget.isValid())
		{
			ResourceSlot &colorSlot = m_resources[colorTarget.index()];
			if (colorSlot.kind != RESOURCE_TEXTURE || colorSlot.renderTarget == 0)
			{
				return RENDER_RESULT_INVALID_ARGUMENT;
			}
			colorView = colorSlot.renderTarget;
		}
		ID3D11DepthStencilView *depthView = 0;
		if (depthTarget.isValid())
		{
			ResourceSlot &depthSlot = m_resources[depthTarget.index()];
			if (depthSlot.kind != RESOURCE_TEXTURE || depthSlot.depthStencil == 0)
			{
				return RENDER_RESULT_INVALID_ARGUMENT;
			}
			depthView = depthSlot.depthStencil;
		}
		ID3D11ShaderResourceView *emptyViews[LEGACY_TEXTURE_STAGE_COUNT] = { 0 };
		m_context->PSSetShaderResources(0, LEGACY_TEXTURE_STAGE_COUNT, emptyViews);
		m_context->OMSetRenderTargets(colorView == 0 ? 0 : 1,
			colorView == 0 ? 0 : &colorView, depthView);
		m_activeRenderTarget = colorView;
		m_activeDepthStencil = depthView;
		return RENDER_RESULT_OK;
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
		D3D11_VIEWPORT viewport;
		viewport.TopLeftX = x;
		viewport.TopLeftY = y;
		viewport.Width = width;
		viewport.Height = height;
		viewport.MinDepth = minimumDepth;
		viewport.MaxDepth = maximumDepth;
		m_context->RSSetViewports(1, &viewport);
		return RENDER_RESULT_OK;
	}

	virtual RenderResult setLegacyState(const LegacyLogicalState &state,
		LegacyVertexFormat vertexFormat, unsigned int texturePresenceMask)
	{
		if (!isOwner() || !m_frameOpen)
		{
			return RENDER_RESULT_INVALID_ARGUMENT;
		}
		const bool textured = texturePresenceMask != 0;
		if ((!textured && vertexFormat != RENDER_VERTEX_POSITION3_COLOR) ||
			(textured && vertexFormat !=
				RENDER_VERTEX_POSITION3_NORMAL_COLOR_TEX1))
		{
			return RENDER_RESULT_UNSUPPORTED;
		}
		if (!textured && state.pipeline.lightingEnable)
		{
			return RENDER_RESULT_UNSUPPORTED;
		}
		for (unsigned int stage = 0; textured &&
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
		const HRESULT transformResult = updateTransformConstants(state,
			textured ? 0x0bU : 0x02U);
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
		target.SrcBlendAlpha = TranslateBlend(state.pipeline.blend.sourceAlpha);
		target.DestBlendAlpha = TranslateBlend(state.pipeline.blend.destinationAlpha);
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
		if (textured)
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
				samplerDescriptor.MinLOD =
					static_cast<float>(sampler.maximumMipLevel);
				samplerDescriptor.MaxLOD = D3D11_FLOAT32_MAX;
				result = findOrCreateSamplerState(samplerDescriptor,
					&samplerStates[stage]);
				if (FAILED(result))
				{
					return TranslateResult(result);
				}
			}
		}

		const float blendFactor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
		m_context->OMSetBlendState(blendState, blendFactor, 0xffffffffU);
		m_context->OMSetDepthStencilState(depthState,
			state.pipeline.depthStencil.stencilReference);
		m_context->RSSetState(rasterizerState);
		m_context->IASetInputLayout(textured ? m_texturedLayout :
			m_positionColorLayout);
		m_context->VSSetShader(textured ? m_texturedVertexShader : m_vertexShader,
			0, 0);
		m_context->PSSetShader(textured ? m_texturedPixelShader : m_pixelShader,
			0, 0);
		if (textured)
		{
			m_context->PSSetSamplers(0, LEGACY_TEXTURE_STAGE_COUNT, samplerStates);
		}
		m_pipelineBound = true;
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
		const HRESULT layoutResult = findOrCreateInputLayout(vertexLayout,
			&inputLayout);
		if (FAILED(layoutResult))
		{
			return TranslateResult(layoutResult);
		}
		const RenderResult stateResult = setLegacyState(state,
			RENDER_VERTEX_POSITION3_NORMAL_COLOR_TEX1, texturePresenceMask);
		if (stateResult == RENDER_RESULT_OK)
		{
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
			const HRESULT constantsResult = updateTransformConstants(state,
				layoutFlags);
			if (FAILED(constantsResult))
			{
				return TranslateResult(constantsResult);
			}
			m_context->IASetInputLayout(inputLayout);
		}
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
		ID3D11Buffer *nativeBuffer = static_cast<ID3D11Buffer *>(slot.resource);
		const UINT nativeStride = stride;
		const UINT nativeOffset = offset;
		m_context->IASetVertexBuffers(0, 1, &nativeBuffer, &nativeStride,
			&nativeOffset);
		m_vertexBufferBound = true;
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
		ID3D11Buffer *nativeBuffer = static_cast<ID3D11Buffer *>(slot.resource);
		m_context->IASetIndexBuffer(nativeBuffer, nativeFormat, offset);
		m_indexBufferBound = true;
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
			ID3D11ShaderResourceView *emptyView = 0;
			m_context->PSSetShaderResources(stage, 1, &emptyView);
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
		m_context->PSSetShaderResources(stage, 1, &slot.view);
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
		m_context->IASetPrimitiveTopology(nativeTopology);
		m_topologyBound = true;
		return RENDER_RESULT_OK;
	}

	virtual RenderResult draw(unsigned int vertexCount, unsigned int startVertex)
	{
		if (!isOwner() || !m_frameOpen || !m_pipelineBound || !m_topologyBound ||
			!m_vertexBufferBound || vertexCount == 0)
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
			!m_vertexBufferBound || !m_indexBufferBound || indexCount == 0)
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
		return RENDER_RESULT_OK;
	}

	virtual RenderResult captureBackBuffer(void *destination,
		size_t destinationBytes, size_t destinationRowPitch,
		RenderFormat *format)
	{
		const size_t requiredRowBytes = static_cast<size_t>(m_width) * 4;
		const size_t maximumSize = static_cast<size_t>(-1);
		if (!isOwner() || m_frameOpen || m_swapChain == 0 || destination == 0 ||
			format == 0 || destinationRowPitch < requiredRowBytes ||
			(m_height != 0 && destinationRowPitch > maximumSize / m_height) ||
			destinationBytes < destinationRowPitch * m_height)
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
				unsigned char *output = static_cast<unsigned char *>(destination);
				const unsigned char *input =
					static_cast<const unsigned char *>(mapped.pData);
				for (unsigned int row = 0; row < m_height; ++row)
				{
					memcpy(output + row * destinationRowPitch,
						input + row * mapped.RowPitch, requiredRowBytes);
				}
				m_context->Unmap(staging, 0);
				*format = RENDER_FORMAT_B8G8R8A8_UNORM;
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

private:
	bool isOwner() const
	{
		return m_initialized && GetCurrentThreadId() == m_ownerThread;
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
			{ "COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 12,
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
		const D3D11_INPUT_ELEMENT_DESC texturedElements[] = {
			{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
				D3D11_INPUT_PER_VERTEX_DATA, 0 },
			{ "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12,
				D3D11_INPUT_PER_VERTEX_DATA, 0 },
			{ "COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 24,
				D3D11_INPUT_PER_VERTEX_DATA, 0 },
			{ "COLOR", 1, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
				D3D11_INPUT_PER_VERTEX_DATA, 0 },
			{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 28,
				D3D11_INPUT_PER_VERTEX_DATA, 0 },
			{ "TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 0, 28,
				D3D11_INPUT_PER_VERTEX_DATA, 0 },
			{ "TEXCOORD", 2, DXGI_FORMAT_R32G32_FLOAT, 0, 28,
				D3D11_INPUT_PER_VERTEX_DATA, 0 },
			{ "TEXCOORD", 3, DXGI_FORMAT_R32G32_FLOAT, 0, 28,
				D3D11_INPUT_PER_VERTEX_DATA, 0 },
			{ "TEXCOORD", 4, DXGI_FORMAT_R32G32_FLOAT, 0, 28,
				D3D11_INPUT_PER_VERTEX_DATA, 0 },
			{ "TEXCOORD", 5, DXGI_FORMAT_R32G32_FLOAT, 0, 28,
				D3D11_INPUT_PER_VERTEX_DATA, 0 },
			{ "TEXCOORD", 6, DXGI_FORMAT_R32G32_FLOAT, 0, 28,
				D3D11_INPUT_PER_VERTEX_DATA, 0 },
			{ "TEXCOORD", 7, DXGI_FORMAT_R32G32_FLOAT, 0, 28,
				D3D11_INPUT_PER_VERTEX_DATA, 0 }
		};
		return m_device->CreateInputLayout(texturedElements,
			static_cast<UINT>(sizeof(texturedElements) / sizeof(texturedElements[0])),
			g_LegacyTexturedVS, sizeof(g_LegacyTexturedVS), &m_texturedLayout);
	}

	HRESULT updateTransformConstants(const LegacyLogicalState &state,
		unsigned int vertexLayoutFlags)
	{
		LegacyTransformConstants shaderConstants;
		MultiplyMatrices(state.constants.world.values, state.constants.view.values,
			shaderConstants.worldView);
		MultiplyMatrices(shaderConstants.worldView,
			state.constants.projection.values,
			shaderConstants.worldViewProjection);
		const RenderFloat4 fogColor = state.pipeline.fogMode == RENDER_FOG_WHITE ?
			RenderFloat4(1.0f, 1.0f, 1.0f, 1.0f) : state.constants.fog.color;
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
				textureStage.projectedCoordinates ? 1U : 0U;
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
		}
		const RenderFloat4 materialValues[] = {
			state.constants.material.diffuse, state.constants.material.ambient,
			state.constants.material.emissive, state.constants.globalAmbient
		};
		float *materialTargets[] = {
			shaderConstants.materialDiffuse, shaderConstants.materialAmbient,
			shaderConstants.materialEmissive, shaderConstants.globalAmbient
		};
		for (unsigned int valueIndex = 0; valueIndex < 4; ++valueIndex)
		{
			materialTargets[valueIndex][0] = materialValues[valueIndex].x;
			materialTargets[valueIndex][1] = materialValues[valueIndex].y;
			materialTargets[valueIndex][2] = materialValues[valueIndex].z;
			materialTargets[valueIndex][3] = materialValues[valueIndex].w;
		}
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
		shaderConstants.lightingParameters[2] = 0;
		shaderConstants.lightingParameters[3] = 0;
		shaderConstants.vertexLayoutParameters[0] =
			(vertexLayoutFlags & 1U) != 0 ? 1U : 0U;
		shaderConstants.vertexLayoutParameters[1] =
			(vertexLayoutFlags & 2U) != 0 ? 1U : 0U;
		shaderConstants.vertexLayoutParameters[2] =
			(vertexLayoutFlags & 4U) != 0 ? 1U : 0U;
		shaderConstants.vertexLayoutParameters[3] = vertexLayoutFlags >> 8;
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
		memcpy(mapped.pData, &shaderConstants, sizeof(shaderConstants));
		m_context->Unmap(constantBuffer, 0);
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
		if (!slot.subresourceOffsets.empty())
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
		if (FAILED(result))
		{
			releaseNativeSlot(slot);
		}
		return result;
	}

	HRESULT createBackBufferTargets()
	{
		if (m_swapChain == 0)
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
		depthDescriptor.Width = m_width;
		depthDescriptor.Height = m_height;
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
		return result;
	}

	void releaseBackBufferTargets()
	{
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
	}

	HRESULT findOrCreateBlendState(const D3D11_BLEND_DESC &descriptor,
		ID3D11BlendState **state)
	{
		for (unsigned int index = 0; index < m_blendStates.size(); ++index)
		{
			if (memcmp(&m_blendStates[index].descriptor, &descriptor,
				sizeof(descriptor)) == 0)
			{
				*state = m_blendStates[index].state;
				return S_OK;
			}
		}
		if (m_blendStates.size() >= 256)
		{
			return E_OUTOFMEMORY;
		}
		BlendStateEntry entry;
		entry.descriptor = descriptor;
		entry.state = 0;
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
				*state = m_depthStates[index].state;
				return S_OK;
			}
		}
		if (m_depthStates.size() >= 256)
		{
			return E_OUTOFMEMORY;
		}
		DepthStencilStateEntry entry;
		entry.descriptor = descriptor;
		entry.state = 0;
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
				*state = m_rasterizerStates[index].state;
				return S_OK;
			}
		}
		if (m_rasterizerStates.size() >= 256)
		{
			return E_OUTOFMEMORY;
		}
		RasterizerStateEntry entry;
		entry.descriptor = descriptor;
		entry.state = 0;
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
				*state = m_samplerStates[index].state;
				return S_OK;
			}
		}
		if (m_samplerStates.size() >= 256)
		{
			return E_OUTOFMEMORY;
		}
		SamplerStateEntry entry;
		entry.descriptor = descriptor;
		entry.state = 0;
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
				*layout = m_inputLayouts[cached].layout;
				return S_OK;
			}
		}
		if (m_inputLayouts.size() >= 256)
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
		}
		if (SUCCEEDED(result) && obtainedLevel != D3D_FEATURE_LEVEL_11_0)
		{
			releaseDeviceObjects();
			return DXGI_ERROR_UNSUPPORTED;
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
		slot.shadow.clear();
		slot.subresourceOffsets.clear();
		slot.subresourceRowPitches.clear();
		slot.subresourceSlicePitches.clear();
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
		}
		releasePipelineResources();
		releaseBackBufferTargets();
		releaseDeviceObjects();
		m_ownerThread = 0;
		m_activeRenderTarget = 0;
		m_activeDepthStencil = 0;
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
		if (m_texturedLayout != 0)
		{
			m_texturedLayout->Release();
			m_texturedLayout = 0;
		}
		if (m_texturedPixelShader != 0)
		{
			m_texturedPixelShader->Release();
			m_texturedPixelShader = 0;
		}
		if (m_texturedVertexShader != 0)
		{
			m_texturedVertexShader->Release();
			m_texturedVertexShader = 0;
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

	void releaseDeviceObjects()
	{
		if (m_swapChain != 0)
		{
			m_swapChain->Release();
			m_swapChain = 0;
		}
		if (m_context != 0)
		{
			m_context->Release();
			m_context = 0;
		}
		if (m_device != 0)
		{
			m_device->Release();
			m_device = 0;
		}
	}

	ID3D11Device *m_device;
	ID3D11DeviceContext *m_context;
	IDXGISwapChain1 *m_swapChain;
	ID3D11RenderTargetView *m_renderTarget;
	ID3D11Texture2D *m_depthTexture;
	ID3D11DepthStencilView *m_depthStencil;
	ID3D11RenderTargetView *m_activeRenderTarget;
	ID3D11DepthStencilView *m_activeDepthStencil;
	ID3D11Buffer *m_transformConstants[TRANSFORM_CONSTANT_BUFFER_COUNT];
	ID3D11VertexShader *m_vertexShader;
	ID3D11PixelShader *m_pixelShader;
	ID3D11InputLayout *m_positionColorLayout;
	ID3D11VertexShader *m_texturedVertexShader;
	ID3D11PixelShader *m_texturedPixelShader;
	ID3D11InputLayout *m_texturedLayout;
	GpuHandleAllocator *m_handles;
	std::vector<ResourceSlot> m_resources;
	std::vector<BlendStateEntry> m_blendStates;
	std::vector<DepthStencilStateEntry> m_depthStates;
	std::vector<RasterizerStateEntry> m_rasterizerStates;
	std::vector<SamplerStateEntry> m_samplerStates;
	std::vector<InputLayoutEntry> m_inputLayouts;
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
	RenderDeviceParameters m_parameters;
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
