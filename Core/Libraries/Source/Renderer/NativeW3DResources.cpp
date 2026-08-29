#include "Renderer/NativeW3DResources.h"
#include "Renderer/NativeW3DRenderState.h"

#include <new>
#include <vector>

namespace rts
{
namespace render
{
struct NativeW3DResources::Slot
{
	Slot() : kind(0), byteCount(0), stride(0), binding(0) {}
	GpuHandle handle;
	unsigned int kind;
	size_t byteCount;
	unsigned int stride;
	unsigned int binding;
};

struct NativeW3DResources::Impl
{
	Impl(unsigned int capacity) : state(0), generation(0), slots(capacity) {}
	NativeW3DRenderState *state;
	unsigned int generation;
	std::vector<Slot> slots;
};

namespace
{
struct DeferredDestroyRequest
{
	// The resource table retains the state until EnqueueCleanup atomically
	// accepts or rejects this request.  Once accepted, NativeW3DRenderer keeps
	// its owner reference until it closes admission and drains the queue, so a
	// request intentionally does not retain its own containing state.
	DeferredDestroyRequest(NativeW3DRenderState *requestedState,
		const std::vector<GpuHandle> &requestedHandles) :
		state(requestedState), handles(requestedHandles)
	{
	}

	NativeW3DRenderState *state;
	std::vector<GpuHandle> handles;
};

void ReleaseDeferredResources(void *context)
{
	DeferredDestroyRequest *request = static_cast<DeferredDestroyRequest *>(context);
	delete request;
}

}

NativeW3DResources::NativeW3DResources(unsigned int capacity) : m_impl(0)
{
	if (capacity == 0)
	{
		return;
	}
	try
	{
		m_impl = new Impl(capacity);
	}
	catch (...)
	{
		m_impl = 0;
	}
}

NativeW3DResources::~NativeW3DResources()
{
	if (m_impl == 0 || m_impl->state == 0)
	{
		delete m_impl;
		return;
	}
	bool released = false;
	if (m_impl->state->IsOwnerThread() && m_impl->state->IsOperational())
	{
		released = Shutdown() == RENDER_RESULT_OK;
	}
	if (!released && m_impl->state != 0)
	{
		std::vector<GpuHandle> handles;
		try
		{
			for (size_t index = 0; index < m_impl->slots.size(); ++index)
			{
				if (m_impl->slots[index].handle.isValid())
				{
					handles.push_back(m_impl->slots[index].handle);
				}
			}
			EnqueueDeferredResources(m_impl->state, handles);
		}
		catch (...)
		{
			// Terminal device teardown owns the bounded allocation-failure fallback.
		}
		if (m_impl->state != 0)
		{
			m_impl->state->Release();
			m_impl->state = 0;
			m_impl->generation = 0;
		}
	}
	delete m_impl;
}

RenderResult NativeW3DResources::Bind(NativeW3DRenderer *renderer)
{
	if (m_impl == 0)
	{
		return RENDER_RESULT_OUT_OF_MEMORY;
	}
	if (renderer == 0 || m_impl->state != 0 || renderer->m_state == 0 ||
		!renderer->IsInitialized() || !renderer->IsOwnerThread())
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	m_impl->state = renderer->m_state;
	m_impl->state->AddRef();
	m_impl->generation = m_impl->state->Generation();
	return m_impl->generation == 0 ? RENDER_RESULT_FAILED : RENDER_RESULT_OK;
}

RenderResult NativeW3DResources::Shutdown()
{
	if (m_impl == 0)
	{
		return RENDER_RESULT_OUT_OF_MEMORY;
	}
	if (m_impl->state == 0)
	{
		return RENDER_RESULT_OK;
	}
	if (!m_impl->state->IsOwnerThread())
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	if (m_impl->state->IsOperational() && m_impl->generation == m_impl->state->Generation())
	{
		IRenderDevice *device = m_impl->state->Device();
		for (size_t index = 0; index < m_impl->slots.size(); ++index)
		{
			Slot &slot = m_impl->slots[index];
			if (slot.handle.isValid())
			{
				if (device == 0 || !device->destroyResource(slot.handle))
				{
					return RENDER_RESULT_FAILED;
				}
				slot = Slot();
			}
		}
	}
	m_impl->state->Release();
	m_impl->state = 0;
	m_impl->generation = 0;
	return RENDER_RESULT_OK;
}

RenderResult NativeW3DResources::CreateBuffer(const BufferDescriptor &descriptor,
	const void *initialData, size_t initialDataBytes, GpuHandle *handle)
{
	if (handle == 0)
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	*handle = GpuHandle();
	if (m_impl == 0)
	{
		return RENDER_RESULT_OUT_OF_MEMORY;
	}
	IRenderDevice *device = m_impl->state == 0 ? 0 : m_impl->state->Device();
	if (device == 0 || !m_impl->state->IsOperational() || m_impl->generation != m_impl->state->Generation())
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	GpuHandle created;
	const RenderResult result = device->createBuffer(descriptor, initialData, initialDataBytes, &created);
	if (result != RENDER_RESULT_OK)
	{
		return result;
	}
	if (created.index() >= m_impl->slots.size())
	{
		device->destroyResource(created);
		return RENDER_RESULT_OUT_OF_MEMORY;
	}
	Slot &slot = m_impl->slots[created.index()];
	if (slot.handle.isValid())
	{
		device->destroyResource(created);
		return RENDER_RESULT_FAILED;
	}
	slot.handle = created;
	slot.kind = 1;
	slot.byteCount = descriptor.byteCount;
	slot.stride = descriptor.stride;
	slot.binding = descriptor.binding;
	*handle = created;
	return RENDER_RESULT_OK;
}

RenderResult NativeW3DResources::CreateTexture(const TextureDescriptor &descriptor,
	const TextureSubresourceData *initialData, unsigned int initialDataCount, GpuHandle *handle)
{
	if (handle == 0)
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	*handle = GpuHandle();
	if (m_impl == 0)
	{
		return RENDER_RESULT_OUT_OF_MEMORY;
	}
	IRenderDevice *device = m_impl->state == 0 ? 0 : m_impl->state->Device();
	if (device == 0 || !m_impl->state->IsOperational() || m_impl->generation != m_impl->state->Generation())
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	GpuHandle created;
	const RenderResult result = device->createTexture(descriptor, initialData, initialDataCount, &created);
	if (result != RENDER_RESULT_OK)
	{
		return result;
	}
	if (created.index() >= m_impl->slots.size())
	{
		device->destroyResource(created);
		return RENDER_RESULT_OUT_OF_MEMORY;
	}
	Slot &slot = m_impl->slots[created.index()];
	if (slot.handle.isValid())
	{
		device->destroyResource(created);
		return RENDER_RESULT_FAILED;
	}
	slot.handle = created;
	slot.kind = 2;
	*handle = created;
	return RENDER_RESULT_OK;
}

bool NativeW3DResources::Destroy(GpuHandle handle)
{
	Slot *slot = Find(handle);
	IRenderDevice *device = m_impl == 0 || m_impl->state == 0 ? 0 : m_impl->state->Device();
	if (slot == 0 || device == 0 || !m_impl->state->IsOperational() ||
		m_impl->generation != m_impl->state->Generation())
	{
		return false;
	}
	if (!device->destroyResource(handle))
	{
		return false;
	}
	*slot = Slot();
	return true;
}

bool NativeW3DResources::IsValid(GpuHandle handle) const
{
	return m_impl != 0 && m_impl->state != 0 && m_impl->state->IsOperational() &&
		m_impl->generation == m_impl->state->Generation() && Find(handle) != 0;
}

NativeW3DResources::Slot *NativeW3DResources::Find(GpuHandle handle)
{
	if (m_impl == 0 || !handle.isValid() || handle.index() >= m_impl->slots.size())
	{
		return 0;
	}
	Slot &slot = m_impl->slots[handle.index()];
	return slot.handle == handle ? &slot : 0;
}

const NativeW3DResources::Slot *NativeW3DResources::Find(GpuHandle handle) const
{
	if (m_impl == 0 || !handle.isValid() || handle.index() >= m_impl->slots.size())
	{
		return 0;
	}
	const Slot &slot = m_impl->slots[handle.index()];
	return slot.handle == handle ? &slot : 0;
}

bool NativeW3DResources::IsVertexRangeValid(GpuHandle handle, unsigned int stride,
	unsigned int offset, unsigned int startVertex, unsigned int vertexCount) const
{
	const Slot *slot = Find(handle);
	if (slot == 0 || slot->kind != 1 || (slot->binding & RENDER_BUFFER_VERTEX) == 0 || stride == 0 || vertexCount == 0)
	{
		return false;
	}
	const size_t entries = static_cast<size_t>(startVertex) + vertexCount;
	return slot->stride == stride && entries >= startVertex && entries <= (static_cast<size_t>(-1) - offset) / stride &&
		static_cast<size_t>(offset) + entries * stride <= slot->byteCount;
}

bool NativeW3DResources::IsIndexRangeValid(GpuHandle handle, RenderFormat format,
	unsigned int offset, unsigned int startIndex, unsigned int indexCount) const
{
	const unsigned int indexSize = format == RENDER_FORMAT_R16_UINT ? 2U : (format == RENDER_FORMAT_R32_UINT ? 4U : 0U);
	const Slot *slot = Find(handle);
	if (slot == 0 || slot->kind != 1 || (slot->binding & RENDER_BUFFER_INDEX) == 0 || indexSize == 0 || indexCount == 0)
	{
		return false;
	}
	const size_t entries = static_cast<size_t>(startIndex) + indexCount;
	return entries >= startIndex && entries <= (static_cast<size_t>(-1) - offset) / indexSize &&
		static_cast<size_t>(offset) + entries * indexSize <= slot->byteCount;
}

bool NativeW3DResources::IsTextureValidOrEmpty(GpuHandle handle) const
{
	return !handle.isValid() || (Find(handle) != 0 && Find(handle)->kind == 2);
}

bool NativeW3DResources::EnqueueDeferredResources(NativeW3DRenderState *state,
	const std::vector<GpuHandle> &handles)
{
	if (state == 0 || handles.empty() || !state->IsAcceptingCleanup())
	{
		return false;
	}
	DeferredDestroyRequest *request = 0;
	try
	{
		request = new DeferredDestroyRequest(state, handles);
	}
	catch (...)
	{
		return false;
	}
	NativeW3DOwnerToken *token = NativeW3DOwnerToken::Create(request,
		ReleaseDeferredResources);
	if (token == 0)
	{
		delete request;
		return false;
	}
	const RenderResult result = state->EnqueueCleanup(DestroyDeferredResources,
		token);
	token->Release();
	return result == RENDER_RESULT_OK;
}

void NativeW3DResources::DestroyDeferredResources(void *context)
{
	DeferredDestroyRequest *request = static_cast<DeferredDestroyRequest *>(context);
	IRenderDevice *device = request->state->Device();
	if (device == 0)
	{
		return;
	}
	for (size_t index = 0; index < request->handles.size(); ++index)
	{
		device->destroyResource(request->handles[index]);
	}
}

bool NativeW3DResources::IsBoundTo(const NativeW3DRenderer *renderer) const
{
	return m_impl != 0 && m_impl->state != 0 && renderer != 0 && m_impl->state == renderer->m_state &&
		m_impl->state->IsOperational() && m_impl->generation == m_impl->state->Generation();
}
}
}
