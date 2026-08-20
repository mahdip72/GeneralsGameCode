#include "Renderer/RendererDevice.h"

#include <ctype.h>
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
bool EqualIgnoringCase(const char *left, const char *right)
{
	while (*left != '\0' && *right != '\0')
	{
		const unsigned char leftCharacter = static_cast<unsigned char>(*left);
		const unsigned char rightCharacter = static_cast<unsigned char>(*right);
		if (tolower(leftCharacter) != tolower(rightCharacter))
		{
			return false;
		}
		++left;
		++right;
	}
	return *left == *right;
}
}

const char *RenderBackendName(RenderBackend backend)
{
	switch (backend)
	{
	case RENDER_BACKEND_DX8:
		return "dx8";
	case RENDER_BACKEND_D3D11:
		return "d3d11";
	default:
		return "unknown";
	}
}

bool ParseRenderBackend(const char *name, RenderBackend *backend)
{
	if (name == 0 || backend == 0)
	{
		return false;
	}
	if (EqualIgnoringCase(name, "dx8"))
	{
		*backend = RENDER_BACKEND_DX8;
		return true;
	}
	if (EqualIgnoringCase(name, "d3d11"))
	{
		*backend = RENDER_BACKEND_D3D11;
		return true;
	}
	return false;
}

GpuHandle::GpuHandle() : m_index(UINT_MAX), m_generation(0) {}

GpuHandle::GpuHandle(unsigned int index, unsigned int generation) :
	m_index(index), m_generation(generation)
{
}

bool GpuHandle::isValid() const
{
	return m_index != UINT_MAX && m_generation != 0;
}

unsigned int GpuHandle::index() const
{
	return m_index;
}

unsigned int GpuHandle::generation() const
{
	return m_generation;
}

bool GpuHandle::operator==(const GpuHandle &other) const
{
	return m_index == other.m_index && m_generation == other.m_generation;
}

bool GpuHandle::operator!=(const GpuHandle &other) const
{
	return !(*this == other);
}

struct GpuHandleAllocator::Impl
{
	explicit Impl(unsigned int requestedCapacity) :
		generations(requestedCapacity, 1), live(requestedCapacity, false),
		liveCount(0)
	{
		freeSlots.reserve(requestedCapacity);
		for (unsigned int i = requestedCapacity; i != 0; --i)
		{
			freeSlots.push_back(i - 1);
		}
	}

	std::vector<unsigned int> generations;
	std::vector<bool> live;
	std::vector<unsigned int> freeSlots;
	unsigned int liveCount;
};

GpuHandleAllocator::GpuHandleAllocator(unsigned int capacity) :
	m_impl(0)
{
	try
	{
		m_impl = new Impl(capacity);
	}
	catch (...)
	{
		m_impl = 0;
	}
}

GpuHandleAllocator::~GpuHandleAllocator()
{
	delete m_impl;
}

GpuHandle GpuHandleAllocator::allocate()
{
	if (m_impl == 0 || m_impl->freeSlots.empty())
	{
		return GpuHandle();
	}
	const unsigned int index = m_impl->freeSlots.back();
	m_impl->freeSlots.pop_back();
	m_impl->live[index] = true;
	++m_impl->liveCount;
	return GpuHandle(index, m_impl->generations[index]);
}

bool GpuHandleAllocator::release(GpuHandle handle)
{
	if (!isLive(handle))
	{
		return false;
	}
	const unsigned int index = handle.index();
	m_impl->live[index] = false;
	--m_impl->liveCount;
	++m_impl->generations[index];
	if (m_impl->generations[index] == 0)
	{
		m_impl->generations[index] = 1;
	}
	m_impl->freeSlots.push_back(index);
	return true;
}

bool GpuHandleAllocator::isLive(GpuHandle handle) const
{
	return m_impl != 0 && handle.isValid() &&
		handle.index() < m_impl->generations.size() &&
		m_impl->live[handle.index()] &&
		m_impl->generations[handle.index()] == handle.generation();
}

unsigned int GpuHandleAllocator::capacity() const
{
	return m_impl == 0 ? 0 : static_cast<unsigned int>(m_impl->generations.size());
}

unsigned int GpuHandleAllocator::liveCount() const
{
	return m_impl == 0 ? 0 : m_impl->liveCount;
}

RenderDeviceParameters::RenderDeviceParameters() :
	backend(RENDER_BACKEND_DX8), window(0), width(0), height(0),
	enableDebugLayer(false), enableVsync(true)
{
}

BufferDescriptor::BufferDescriptor() :
	byteCount(0), stride(0), binding(RENDER_BUFFER_VERTEX),
	usage(RENDER_USAGE_IMMUTABLE)
{
}

TextureSubresourceData::TextureSubresourceData() :
	data(0), rowPitch(0), slicePitch(0)
{
}

TextureDescriptor::TextureDescriptor() :
	width(0), height(0), mipCount(1), arrayCount(1), dimension(RENDER_TEXTURE_2D),
	format(RENDER_FORMAT_UNKNOWN), binding(RENDER_TEXTURE_SHADER_RESOURCE),
	usage(RENDER_USAGE_IMMUTABLE)
{
}
}
}
