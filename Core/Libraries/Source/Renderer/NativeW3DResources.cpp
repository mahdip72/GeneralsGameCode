#include "Renderer/NativeW3DResources.h"

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
	Impl(unsigned int capacity) : renderer(0), slots(capacity) {}
	NativeW3DRenderer *renderer;
	std::vector<Slot> slots;
};

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
	if (m_impl != 0 && m_impl->renderer != 0 &&
		m_impl->renderer->IsOwnerThread())
	{
		Shutdown();
	}
	// Do not leave a non-owning facade pointer behind when destruction is
	// requested from the wrong thread or a release operation failed.  Backend
	// mutation remains owner-only; this only severs the C++ association.
	if (m_impl != 0 && m_impl->renderer != 0)
	{
		m_impl->renderer->m_resources = 0;
		m_impl->renderer = 0;
	}
	delete m_impl;
}

RenderResult NativeW3DResources::Bind(NativeW3DRenderer *renderer)
{
	if (m_impl == 0)
	{
		return RENDER_RESULT_OUT_OF_MEMORY;
	}
	if (renderer == 0 || m_impl->renderer != 0 || renderer->m_resources != 0 || !renderer->IsInitialized() ||
		!renderer->IsOwnerThread())
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	m_impl->renderer = renderer;
	renderer->m_resources = this;
	return RENDER_RESULT_OK;
}

RenderResult NativeW3DResources::Shutdown()
{
	if (m_impl == 0)
	{
		return RENDER_RESULT_OUT_OF_MEMORY;
	}
	if (m_impl->renderer == 0)
	{
		return RENDER_RESULT_OK;
	}
	if (!m_impl->renderer->IsOwnerThread())
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	for (size_t index = 0; index < m_impl->slots.size(); ++index)
	{
		Slot &slot = m_impl->slots[index];
		if (slot.handle.isValid())
		{
			if (!m_impl->renderer->m_device->destroyResource(slot.handle))
			{
				return RENDER_RESULT_FAILED;
			}
			slot = Slot();
		}
	}
	m_impl->renderer->m_resources = 0;
	m_impl->renderer = 0;
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
	if (m_impl->renderer == 0 || !m_impl->renderer->IsOwnerThread())
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	GpuHandle created;
	const RenderResult result = m_impl->renderer->m_device->createBuffer(
		descriptor, initialData, initialDataBytes, &created);
	if (result != RENDER_RESULT_OK)
	{
		return result;
	}
	if (created.index() >= m_impl->slots.size())
	{
		m_impl->renderer->m_device->destroyResource(created);
		return RENDER_RESULT_OUT_OF_MEMORY;
	}
	Slot &slot = m_impl->slots[created.index()];
	if (slot.handle.isValid())
	{
		m_impl->renderer->m_device->destroyResource(created);
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
	const TextureSubresourceData *initialData, unsigned int initialDataCount,
	GpuHandle *handle)
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
	if (m_impl->renderer == 0 || !m_impl->renderer->IsOwnerThread())
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	GpuHandle created;
	const RenderResult result = m_impl->renderer->m_device->createTexture(
		descriptor, initialData, initialDataCount, &created);
	if (result != RENDER_RESULT_OK)
	{
		return result;
	}
	if (created.index() >= m_impl->slots.size())
	{
		m_impl->renderer->m_device->destroyResource(created);
		return RENDER_RESULT_OUT_OF_MEMORY;
	}
	Slot &slot = m_impl->slots[created.index()];
	if (slot.handle.isValid())
	{
		m_impl->renderer->m_device->destroyResource(created);
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
	if (slot == 0 || m_impl->renderer == 0 ||
		!m_impl->renderer->IsOwnerThread())
	{
		return false;
	}
	if (!m_impl->renderer->m_device->destroyResource(handle))
	{
		return false;
	}
	*slot = Slot();
	return true;
}

bool NativeW3DResources::IsValid(GpuHandle handle) const
{
	return m_impl != 0 && m_impl->renderer != 0 &&
		m_impl->renderer->IsOwnerThread() && Find(handle) != 0;
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
	if (slot == 0 || slot->kind != 1 || (slot->binding & RENDER_BUFFER_VERTEX) == 0 ||
		stride == 0 || vertexCount == 0)
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
	const unsigned int indexSize = format == RENDER_FORMAT_R16_UINT ? 2U :
		(format == RENDER_FORMAT_R32_UINT ? 4U : 0U);
	const Slot *slot = Find(handle);
	if (slot == 0 || slot->kind != 1 || (slot->binding & RENDER_BUFFER_INDEX) == 0 ||
		indexSize == 0 || indexCount == 0)
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

bool NativeW3DResources::IsBoundTo(const NativeW3DRenderer *renderer) const
{
	return m_impl != 0 && m_impl->renderer == renderer;
}

void NativeW3DResources::InvalidateRenderer()
{
	if (m_impl == 0)
	{
		return;
	}
	for (size_t index = 0; index < m_impl->slots.size(); ++index)
	{
		m_impl->slots[index] = Slot();
	}
	m_impl->renderer = 0;
}
}
}
