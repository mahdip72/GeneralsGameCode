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
RenderBackend g_requestedBackend = RENDER_BACKEND_DX8;

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

void SetRequestedRenderBackend(RenderBackend backend)
{
	g_requestedBackend = backend;
}

RenderBackend RequestedRenderBackend()
{
	return g_requestedBackend;
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

RenderFrameFailureLatch::RenderFrameFailureLatch() :
	m_failed(false), m_deviceRemoved(false), m_result(RENDER_RESULT_OK),
	m_commandResult(RENDER_RESULT_OK)
{
}

bool RenderFrameFailureLatch::record(RenderResult result)
{
	if (result == RENDER_RESULT_OK)
	{
		return false;
	}
	bool newlyRecorded = false;
	if (!m_failed)
	{
		m_failed = true;
		m_result = result;
		newlyRecorded = true;
	}
	if (result == RENDER_RESULT_DEVICE_REMOVED)
	{
		if (!m_deviceRemoved)
		{
			m_deviceRemoved = true;
			newlyRecorded = true;
		}
	}
	else if (m_commandResult == RENDER_RESULT_OK)
	{
		m_commandResult = result;
	}
	return newlyRecorded;
}

void RenderFrameFailureLatch::reset()
{
	m_failed = false;
	m_deviceRemoved = false;
	m_result = RENDER_RESULT_OK;
	m_commandResult = RENDER_RESULT_OK;
}

bool RenderFrameFailureLatch::hasFailure() const
{
	return m_failed;
}

bool RenderFrameFailureLatch::hasDeviceRemoval() const
{
	return m_deviceRemoved;
}

RenderResult RenderFrameFailureLatch::result() const
{
	return m_deviceRemoved ? RENDER_RESULT_DEVICE_REMOVED : m_result;
}

RenderResult RenderFrameFailureLatch::commandResult() const
{
	return m_commandResult != RENDER_RESULT_OK ? m_commandResult : m_result;
}

RenderFrameOutcome::RenderFrameOutcome() :
	m_commandFailure(), m_endFrameResult(RENDER_RESULT_OK),
	m_presentationResult(RENDER_RESULT_OK),
	m_recoveryResult(RENDER_RESULT_OK), m_deviceRemoved(false),
	m_frameEnded(false), m_presented(false), m_operational(true)
{
}

bool RenderFrameOutcome::recordCommandFailure(RenderResult result)
{
	if (result == RENDER_RESULT_DEVICE_REMOVED)
	{
		m_deviceRemoved = true;
	}
	return m_commandFailure.record(result);
}

void RenderFrameOutcome::recordEndFrame(RenderResult result)
{
	m_endFrameResult = result;
	if (result == RENDER_RESULT_DEVICE_REMOVED)
	{
		m_deviceRemoved = true;
	}
}

void RenderFrameOutcome::recordPresentation(RenderResult result)
{
	m_presentationResult = result;
	if (result == RENDER_RESULT_DEVICE_REMOVED)
	{
		m_deviceRemoved = true;
	}
}

void RenderFrameOutcome::recordRecovery(RenderResult result)
{
	m_recoveryResult = result;
	if (result == RENDER_RESULT_DEVICE_REMOVED)
	{
		m_deviceRemoved = true;
	}
}

void RenderFrameOutcome::markFrameEnded()
{
	m_frameEnded = true;
}

void RenderFrameOutcome::markPresented()
{
	m_presented = true;
}

void RenderFrameOutcome::setOperational(bool operational)
{
	m_operational = operational;
}

bool RenderFrameOutcome::hasCommandFailure() const
{
	return m_commandFailure.hasFailure();
}

bool RenderFrameOutcome::hasLifecycleFailure() const
{
	return m_endFrameResult != RENDER_RESULT_OK ||
		m_presentationResult != RENDER_RESULT_OK ||
		m_recoveryResult != RENDER_RESULT_OK;
}

bool RenderFrameOutcome::hasDeviceRemoval() const
{
	return m_deviceRemoved || m_commandFailure.hasDeviceRemoval();
}

bool RenderFrameOutcome::wasPresented() const
{
	return m_presented;
}

bool RenderFrameOutcome::frameEnded() const
{
	return m_frameEnded;
}

bool RenderFrameOutcome::isOperational() const
{
	return m_operational;
}

RenderResult RenderFrameOutcome::commandResult() const
{
	return m_commandFailure.hasDeviceRemoval() ?
		m_commandFailure.commandResult() : m_commandFailure.result();
}

RenderResult RenderFrameOutcome::endFrameResult() const
{
	return m_endFrameResult;
}

RenderResult RenderFrameOutcome::presentationResult() const
{
	return m_presentationResult;
}

RenderResult RenderFrameOutcome::recoveryResult() const
{
	return m_recoveryResult;
}

RenderResult RenderFrameOutcome::result() const
{
	if (hasDeviceRemoval())
	{
		return RENDER_RESULT_DEVICE_REMOVED;
	}
	if (m_recoveryResult != RENDER_RESULT_OK)
	{
		return m_recoveryResult;
	}
	if (m_presentationResult != RENDER_RESULT_OK)
	{
		return m_presentationResult;
	}
	if (m_endFrameResult != RENDER_RESULT_OK)
	{
		return m_endFrameResult;
	}
	return m_commandFailure.result();
}

RenderCaptureRequest::RenderCaptureRequest() :
	m_requested(false), m_failureCount(0)
{
}

void RenderCaptureRequest::request()
{
	m_requested = true;
	m_failureCount = 0;
}

void RenderCaptureRequest::clear()
{
	m_requested = false;
	m_failureCount = 0;
}

bool RenderCaptureRequest::isRequested() const
{
	return m_requested;
}

bool RenderCaptureRequest::shouldAttempt(bool visibleFrame) const
{
	return m_requested && visibleFrame;
}

void RenderCaptureRequest::recordSuccess()
{
	clear();
}

void RenderCaptureRequest::recordFailure()
{
	if (!m_requested)
	{
		return;
	}
	if (m_failureCount < MAX_FAILURES)
	{
		++m_failureCount;
	}
	if (m_failureCount >= MAX_FAILURES)
	{
		m_requested = false;
	}
}

unsigned int RenderCaptureRequest::failureCount() const
{
	return m_failureCount;
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
	adapterIndex(UINT_MAX), enableDebugLayer(false), enableVsync(true),
	allowSoftwareFallback(true)
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

LegacyVertexElement::LegacyVertexElement() :
	semantic(RENDER_VERTEX_SEMANTIC_POSITION), semanticIndex(0),
	format(RENDER_VERTEX_DATA_FLOAT3), byteOffset(0)
{
}

LegacyVertexLayout::LegacyVertexLayout() : stride(0), elementCount(0)
{
}
}
}
