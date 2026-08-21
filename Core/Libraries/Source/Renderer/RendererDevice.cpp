#include "Renderer/RendererDevice.h"

#ifdef _WIN32
#include <windows.h>
#endif
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

bool Checked_Multiply(size_t left, size_t right, size_t *result)
{
	if (result == 0 || (left != 0 && right > (size_t)-1 / left))
	{
		return false;
	}
	*result = left * right;
	return true;
}

unsigned long Current_Render_Thread_Id()
{
#ifdef _WIN32
	return static_cast<unsigned long>(GetCurrentThreadId());
#else
	return 1;
#endif
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

RenderBackBufferInfo::RenderBackBufferInfo() :
	width(0), height(0), format(RENDER_FORMAT_UNKNOWN)
{
}

RenderCaptureHandle::RenderCaptureHandle() :
	kind(RENDER_CAPTURE_COMPRESSED_SCREENSHOT), requestId(0), generation(0)
{
}

RenderCaptureRequestDescriptor::RenderCaptureRequestDescriptor() :
	kind(RENDER_CAPTURE_COMPRESSED_SCREENSHOT), consumer(0), completed(0),
	cancelled(0)
{
}

struct RenderCaptureQueue::Impl
{
	struct Entry
	{
		RenderCaptureRequestDescriptor descriptor;
		RenderCaptureHandle handle;
	};

	Impl(unsigned int requestCapacity) :
		capacity(requestCapacity == 0 ? 1 : requestCapacity), generation(1),
		nextRequestId(1), active(true), ownerThread(0), resetting(false),
		shuttingDown(false)
	{
	}

	bool isOwnerThread() const
	{
		return ownerThread != 0 &&
			ownerThread == Current_Render_Thread_Id();
	}

	unsigned int capacity;
	unsigned int generation;
	unsigned int nextRequestId;
	bool active;
	unsigned long ownerThread;
	bool resetting;
	bool shuttingDown;
	std::vector<Entry> entries;
};

RenderCaptureQueue::RenderCaptureQueue(unsigned int capacity) :
	m_impl(new(std::nothrow) Impl(capacity))
{
}

RenderCaptureQueue::~RenderCaptureQueue()
{
	// Destruction must not call into consumer objects whose lifetime is already
	// ending. The owner calls shutdown() while consumers are still alive.
	delete m_impl;
	m_impl = 0;
}

bool RenderCaptureQueue::bindOwnerThread()
{
	if (m_impl == 0)
	{
		return false;
	}
	const unsigned long currentThread = Current_Render_Thread_Id();
	if (m_impl->ownerThread == 0)
	{
		m_impl->ownerThread = currentThread;
	}
	return m_impl->ownerThread == currentThread;
}

RenderResult RenderCaptureQueue::enqueue(
	const RenderCaptureRequestDescriptor &descriptor,
	RenderCaptureHandle *handle)
{
	if (m_impl == 0 || !m_impl->isOwnerThread() || !m_impl->active ||
		handle == 0 ||
		descriptor.completed == 0 || descriptor.cancelled == 0)
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	if (m_impl->entries.size() >= m_impl->capacity)
	{
		return RENDER_RESULT_OUT_OF_MEMORY;
	}
	RenderCaptureQueue::Impl::Entry entry;
	entry.descriptor = descriptor;
	entry.handle.kind = descriptor.kind;
	entry.handle.requestId = m_impl->nextRequestId++;
	if (entry.handle.requestId == 0)
	{
		entry.handle.requestId = m_impl->nextRequestId++;
	}
	entry.handle.generation = m_impl->generation;
	try
	{
		m_impl->entries.push_back(entry);
	}
	catch (...)
	{
		return RENDER_RESULT_OUT_OF_MEMORY;
	}
	*handle = entry.handle;
	return RENDER_RESULT_OK;
}

RenderResult RenderCaptureQueue::completeVisible(unsigned int width,
	unsigned int height, size_t rowPitch, RenderFormat format,
	const void *pixels, size_t pixelBytes)
{
	size_t requiredRowBytes = 0;
	size_t requiredBytes = 0;
	if (m_impl == 0 || !m_impl->isOwnerThread() || !m_impl->active ||
		width == 0 || height == 0 ||
		rowPitch == 0 || pixels == 0 || pixelBytes == 0 ||
		(format != RENDER_FORMAT_R8G8B8A8_UNORM &&
			format != RENDER_FORMAT_B8G8R8A8_UNORM) ||
		!Checked_Multiply(static_cast<size_t>(width), 4, &requiredRowBytes) ||
		rowPitch < requiredRowBytes ||
		!Checked_Multiply(rowPitch, static_cast<size_t>(height),
			&requiredBytes) || pixelBytes < requiredBytes)
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	std::vector<RenderCaptureQueue::Impl::Entry> completed;
	completed.swap(m_impl->entries);
	for (size_t index = 0; index < completed.size(); ++index)
	{
		RenderCaptureQueue::Impl::Entry &entry = completed[index];
		if (entry.handle.generation != m_impl->generation)
		{
			try
			{
				entry.descriptor.cancelled(entry.descriptor.consumer,
					&entry.handle, RENDER_RESULT_FAILED);
			}
			catch (...)
			{
			}
			continue;
		}
		try
		{
			entry.descriptor.completed(entry.descriptor.consumer, &entry.handle,
				width, height, rowPitch, format, pixels, pixelBytes);
		}
		catch (...)
		{
		}
	}
	return RENDER_RESULT_OK;
}

unsigned int RenderCaptureQueue::cancelStale(RenderResult reason)
{
	if (m_impl == 0 || !m_impl->isOwnerThread() || !m_impl->active)
	{
		return 0;
	}
	std::vector<RenderCaptureQueue::Impl::Entry> pending;
	pending.swap(m_impl->entries);
	size_t cancelledCount = 0;
	// Stable-partition the detached entries without another allocation. This
	// keeps callbacks out of the queue until every original entry is detached,
	// while preserving FIFO order for both retained and cancelled requests.
	for (size_t index = 0; index < pending.size(); ++index)
	{
		if (pending[index].handle.generation != m_impl->generation)
		{
			RenderCaptureQueue::Impl::Entry cancelledEntry = pending[index];
			for (size_t move = index; move > cancelledCount; --move)
			{
				pending[move] = pending[move - 1];
			}
			pending[cancelledCount] = cancelledEntry;
			++cancelledCount;
		}
	}
	try
	{
		m_impl->entries.reserve(pending.capacity());
		for (size_t retainedIndex = cancelledCount;
			retainedIndex < pending.size(); ++retainedIndex)
		{
			m_impl->entries.push_back(pending[retainedIndex]);
		}
	}
	catch (...)
	{
		m_impl->entries.clear();
		for (size_t failedIndex = 0; failedIndex < pending.size();
			++failedIndex)
		{
			try
			{
				pending[failedIndex].descriptor.cancelled(
					pending[failedIndex].descriptor.consumer,
					&pending[failedIndex].handle,
					RENDER_RESULT_OUT_OF_MEMORY);
			}
			catch (...)
			{
			}
		}
		return static_cast<unsigned int>(pending.size());
	}
	for (size_t cancelledIndex = 0; cancelledIndex < cancelledCount;
		++cancelledIndex)
	{
		RenderCaptureQueue::Impl::Entry &entry = pending[cancelledIndex];
		try
		{
			entry.descriptor.cancelled(entry.descriptor.consumer,
				&entry.handle, reason);
		}
		catch (...)
		{
		}
	}
	return static_cast<unsigned int>(cancelledCount);
}

unsigned int RenderCaptureQueue::cancelConsumer(void *consumer,
	RenderResult reason)
{
	if (m_impl == 0 || !m_impl->isOwnerThread() || !m_impl->active)
	{
		return 0;
	}
	std::vector<RenderCaptureQueue::Impl::Entry> pending;
	pending.swap(m_impl->entries);
	size_t cancelledCount = 0;
	// Keep the matching entries in stable FIFO order at the detached prefix;
	// callbacks are invoked only after all source entries have been removed.
	for (size_t index = 0; index < pending.size(); ++index)
	{
		if (pending[index].descriptor.consumer == consumer)
		{
			RenderCaptureQueue::Impl::Entry cancelledEntry = pending[index];
			for (size_t move = index; move > cancelledCount; --move)
			{
				pending[move] = pending[move - 1];
			}
			pending[cancelledCount] = cancelledEntry;
			++cancelledCount;
		}
	}
	try
	{
		m_impl->entries.reserve(pending.capacity());
		for (size_t retainedIndex = cancelledCount;
			retainedIndex < pending.size(); ++retainedIndex)
		{
			m_impl->entries.push_back(pending[retainedIndex]);
		}
	}
	catch (...)
	{
		m_impl->entries.clear();
		for (size_t failedIndex = 0; failedIndex < pending.size();
			++failedIndex)
		{
			try
			{
				pending[failedIndex].descriptor.cancelled(
					pending[failedIndex].descriptor.consumer,
					&pending[failedIndex].handle,
					RENDER_RESULT_OUT_OF_MEMORY);
			}
			catch (...)
			{
			}
		}
		return static_cast<unsigned int>(pending.size());
	}
	for (size_t cancelledIndex = 0; cancelledIndex < cancelledCount;
		++cancelledIndex)
	{
		RenderCaptureQueue::Impl::Entry &entry = pending[cancelledIndex];
		try
		{
			entry.descriptor.cancelled(entry.descriptor.consumer,
				&entry.handle, reason);
		}
		catch (...)
		{
		}
	}
	return static_cast<unsigned int>(cancelledCount);
}

unsigned int RenderCaptureQueue::cancelCurrent(RenderResult reason)
{
	if (m_impl == 0 || !m_impl->isOwnerThread() || !m_impl->active)
	{
		return 0;
	}
	std::vector<RenderCaptureQueue::Impl::Entry> pending;
	pending.swap(m_impl->entries);
	const unsigned int cancelledCount = static_cast<unsigned int>(pending.size());
	for (size_t index = 0; index < pending.size(); ++index)
	{
		RenderCaptureQueue::Impl::Entry &entry = pending[index];
		try
		{
			entry.descriptor.cancelled(entry.descriptor.consumer,
				&entry.handle, reason);
		}
		catch (...)
		{
		}
	}
	return cancelledCount;
}

void RenderCaptureQueue::shutdown(RenderResult reason)
{
	// An already-shut-down queue must remain ownerless. In particular, a
	// repeated shutdown from a different thread must not claim ownership and
	// change the thread allowed to reset the queue during the next initialize.
	if (m_impl == 0 || !m_impl->active || !m_impl->isOwnerThread())
	{
		return;
	}
	m_impl->active = false;
	m_impl->shuttingDown = true;
	std::vector<RenderCaptureQueue::Impl::Entry> pending;
	pending.swap(m_impl->entries);
	for (size_t index = 0; index < pending.size(); ++index)
	{
		RenderCaptureQueue::Impl::Entry &entry = pending[index];
		try
		{
			entry.descriptor.cancelled(entry.descriptor.consumer,
				&entry.handle, reason);
		}
		catch (...)
		{
		}
	}
	m_impl->shuttingDown = false;
	m_impl->resetting = false;
	m_impl->ownerThread = 0;
}

void RenderCaptureQueue::reset()
{
	if (m_impl == 0 || !m_impl->isOwnerThread() || m_impl->resetting ||
		m_impl->shuttingDown)
	{
		return;
	}
	const bool wasActive = m_impl->active;
	m_impl->resetting = true;
	if (wasActive && !m_impl->entries.empty())
	{
		cancelCurrent(RENDER_RESULT_FAILED);
	}
	if (wasActive && !m_impl->active)
	{
		m_impl->resetting = false;
		return;
	}
	m_impl->entries.clear();
	m_impl->generation = 1;
	m_impl->nextRequestId = 1;
	m_impl->active = true;
	m_impl->resetting = false;
}

void RenderCaptureQueue::advanceGeneration()
{
	if (m_impl == 0 || !m_impl->isOwnerThread() || !m_impl->active)
	{
		return;
	}
	++m_impl->generation;
	if (m_impl->generation == 0)
	{
		m_impl->generation = 1;
	}
}

unsigned int RenderCaptureQueue::generation() const
{
	return m_impl == 0 || (m_impl->ownerThread != 0 &&
		m_impl->ownerThread != Current_Render_Thread_Id()) ? 0 :
		m_impl->generation;
}

unsigned int RenderCaptureQueue::pendingCount() const
{
	return m_impl == 0 || (m_impl->ownerThread != 0 &&
		m_impl->ownerThread != Current_Render_Thread_Id()) ? 0 :
		static_cast<unsigned int>(m_impl->entries.size());
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
