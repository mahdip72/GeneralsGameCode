#include "Renderer/RendererDevice.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>

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

struct ResourceSlot
{
	ResourceSlot() : resource(0), kind(RESOURCE_NONE),
		usage(RENDER_USAGE_DEFAULT), byteCount(0)
	{
	}

	ID3D11Resource *resource;
	ResourceKind kind;
	RenderUsage usage;
	size_t byteCount;
	std::vector<unsigned char> shadow;
};

class D3D11RenderDevice : public IRenderDevice, public IRenderContext
{
public:
	D3D11RenderDevice() : m_device(0), m_context(0), m_swapChain(0),
		m_handles(0), m_ownerThread(0), m_initialized(false),
		m_frameOpen(false), m_enableVsync(true), m_width(0), m_height(0)
	{
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

		UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
		if (parameters.enableDebugLayer)
		{
			flags |= D3D11_CREATE_DEVICE_DEBUG;
		}
		const D3D_FEATURE_LEVEL requestedLevel = D3D_FEATURE_LEVEL_11_0;
		D3D_FEATURE_LEVEL obtainedLevel = D3D_FEATURE_LEVEL_9_1;
		HRESULT result = D3D11CreateDevice(0, D3D_DRIVER_TYPE_HARDWARE, 0,
			flags, &requestedLevel, 1, D3D11_SDK_VERSION, &m_device,
			&obtainedLevel, &m_context);
		if (FAILED(result))
		{
			result = D3D11CreateDevice(0, D3D_DRIVER_TYPE_WARP, 0,
				flags, &requestedLevel, 1, D3D11_SDK_VERSION, &m_device,
				&obtainedLevel, &m_context);
		}
		if (FAILED(result))
		{
			releaseDeviceObjects();
			return TranslateResult(result);
		}
		if (obtainedLevel != D3D_FEATURE_LEVEL_11_0)
		{
			releaseDeviceObjects();
			return RENDER_RESULT_UNSUPPORTED;
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
		}
		m_initialized = true;
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
		slot.byteCount = descriptor.byteCount;
		if (descriptor.usage == RENDER_USAGE_DYNAMIC)
		{
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
		slot.byteCount = 0;
		*texture = handle;
		return RENDER_RESULT_OK;
	}

	virtual bool destroyResource(GpuHandle resource)
	{
		if (!isOwner() || !m_handles->isLive(resource))
		{
			return false;
		}
		return releaseSlot(resource, m_resources[resource.index()]);
	}

	virtual RenderResult resize(unsigned int width, unsigned int height)
	{
		if (!isOwner() || width == 0 || height == 0 || m_frameOpen)
		{
			return RENDER_RESULT_INVALID_ARGUMENT;
		}
		if (m_swapChain != 0)
		{
			const HRESULT result = m_swapChain->ResizeBuffers(0, width, height,
				DXGI_FORMAT_UNKNOWN, 0);
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
		if (slot.usage == RENDER_USAGE_DYNAMIC)
		{
			memcpy(&slot.shadow[destinationOffset], data, byteCount);
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

	virtual RenderResult endFrame()
	{
		if (!isOwner() || !m_frameOpen)
		{
			return RENDER_RESULT_INVALID_ARGUMENT;
		}
		m_frameOpen = false;
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

	bool releaseSlot(GpuHandle handle, ResourceSlot &slot)
	{
		if (slot.resource != 0)
		{
			slot.resource->Release();
		}
		slot.resource = 0;
		slot.kind = RESOURCE_NONE;
		slot.usage = RENDER_USAGE_DEFAULT;
		slot.byteCount = 0;
		slot.shadow.clear();
		return m_handles->release(handle);
	}

	void shutdownInternal()
	{
		m_frameOpen = false;
		if (m_handles != 0)
		{
			for (unsigned int i = 0; i < m_resources.size(); ++i)
			{
				ResourceSlot &slot = m_resources[i];
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
		releaseDeviceObjects();
		m_ownerThread = 0;
		m_initialized = false;
		m_width = 0;
		m_height = 0;
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
	GpuHandleAllocator *m_handles;
	std::vector<ResourceSlot> m_resources;
	DWORD m_ownerThread;
	bool m_initialized;
	bool m_frameOpen;
	bool m_enableVsync;
	unsigned int m_width;
	unsigned int m_height;
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
