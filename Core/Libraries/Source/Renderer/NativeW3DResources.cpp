#include "Renderer/NativeW3DResources.h"
#include "Renderer/NativeW3DRenderState.h"
#include <Utility/interlocked_adapter.h>
#if defined(RTS_RENDERER_HAS_D3D11)
#include "Renderer/ThreadedRenderDevice.h"
#endif

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

#include <assert.h>
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
class ResourceTableLock
{
public:
	ResourceTableLock() : m_initialized(false)
	{
#ifdef _WIN32
		InitializeCriticalSection(&m_lock);
		m_initialized = true;
#else
		m_initialized = pthread_mutex_init(&m_lock, 0) == 0;
#endif
	}

	~ResourceTableLock()
	{
		if (!m_initialized)
		{
			return;
		}
#ifdef _WIN32
		DeleteCriticalSection(&m_lock);
#else
		pthread_mutex_destroy(&m_lock);
#endif
	}

	bool IsInitialized() const { return m_initialized; }

	void Lock()
	{
#ifdef _WIN32
		EnterCriticalSection(&m_lock);
#else
		pthread_mutex_lock(&m_lock);
#endif
	}

	void Unlock()
	{
#ifdef _WIN32
		LeaveCriticalSection(&m_lock);
#else
		pthread_mutex_unlock(&m_lock);
#endif
	}

private:
	ResourceTableLock(const ResourceTableLock &);
	ResourceTableLock &operator=(const ResourceTableLock &);

#ifdef _WIN32
	CRITICAL_SECTION m_lock;
#else
	pthread_mutex_t m_lock;
#endif
	bool m_initialized;
};

class ScopedResourceTableLock
{
public:
	explicit ScopedResourceTableLock(ResourceTableLock &lock) : m_lock(lock)
	{
		m_lock.Lock();
	}

	~ScopedResourceTableLock() { m_lock.Unlock(); }

private:
	ScopedResourceTableLock(const ScopedResourceTableLock &);
	ScopedResourceTableLock &operator=(const ScopedResourceTableLock &);

	ResourceTableLock &m_lock;
};
}

NativeW3DBufferDescription::NativeW3DBufferDescription() :
	authority(NATIVE_W3D_CONTENT_INVALID), authorityEpoch(0)
{
}

NativeW3DTextureDescription::NativeW3DTextureDescription() :
	authority(NATIVE_W3D_CONTENT_INVALID), authorityEpoch(0)
{
}

NativeW3DGpuContentLease::NativeW3DGpuContentLease() :
	attachmentGeneration(0), backendEpoch(0), authorityEpoch(0)
{
}

bool NativeW3DGpuContentLease::isValid() const
{
	return resource.isValid() && attachmentGeneration != 0 &&
		backendEpoch != 0 && authorityEpoch != 0;
}

NativeW3DTextureHandle::NativeW3DTextureHandle() :
	attachmentGeneration(0)
{
}

bool NativeW3DTextureHandle::isValid() const
{
	return resource.isValid() && attachmentGeneration != 0;
}

NativeW3DSurfaceHandle::NativeW3DSurfaceHandle() :
	backendEpoch(0), mipLevel(0), arraySlice(0), width(0), height(0),
	format(RENDER_FORMAT_UNKNOWN)
{
}

bool NativeW3DSurfaceHandle::isValid() const
{
	return texture.isValid() && backendEpoch != 0 && width != 0 &&
		height != 0 && format != RENDER_FORMAT_UNKNOWN;
}

NativeW3DResourceHost::NativeW3DResourceHost(unsigned int cleanupCapacity) :
	m_cleanupCapacity(cleanupCapacity), m_state(0), m_boundDevice(0),
	m_nextAttachmentGeneration(1)
{
}

NativeW3DResourceHost::~NativeW3DResourceHost()
{
	if (m_state != 0 && m_state->IsOwnerThread())
	{
		const RenderResult result = Detach();
		assert(result == RENDER_RESULT_OK);
		if (result != RENDER_RESULT_OK)
		{
			return;
		}
	}
	if (m_state != 0)
	{
		m_state->Release();
		m_state = 0;
	}
}

RenderResult NativeW3DResourceHost::Attach(IRenderDevice *device,
	IRenderContext *context)
{
	if (m_state != 0 || device == 0 || context == 0 ||
		(m_boundDevice != 0 && m_boundDevice != device) ||
		!device->isOperational() || device->immediateContext() != context)
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	NativeW3DRenderState *state =
		NativeW3DRenderState::Create(m_cleanupCapacity,
			m_nextAttachmentGeneration);
	if (state == 0)
	{
		return RENDER_RESULT_OUT_OF_MEMORY;
	}
	RenderResult result = state->BindOwner();
	if (result == RENDER_RESULT_OK)
	{
		result = state->AttachBackend(device, context);
	}
	if (result != RENDER_RESULT_OK)
	{
		state->Release();
		return result;
	}
	m_state = state;
	m_boundDevice = device;
	++m_nextAttachmentGeneration;
	if (m_nextAttachmentGeneration == 0)
	{
		m_nextAttachmentGeneration = 1;
	}
	return RENDER_RESULT_OK;
}

RenderResult NativeW3DResourceHost::ReplaceContext(IRenderContext *context)
{
	IRenderDevice *device = m_state == 0 ? 0 : m_state->Device();
	if (device == 0 || context == 0 || !device->isOperational() ||
		device->immediateContext() != context)
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	return m_state->ReplaceContext(context);
}

RenderResult NativeW3DResourceHost::DrainCleanup(unsigned int maxCommands,
	unsigned int *drained)
{
	return m_state == 0 ? RENDER_RESULT_INVALID_ARGUMENT :
		m_state->DrainCleanup(maxCommands, drained);
}

RenderResult NativeW3DResourceHost::Detach()
{
	if (m_state == 0)
	{
		return RENDER_RESULT_OK;
	}
	if (!m_state->IsOwnerThread())
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	if (m_state->BoundResourceTables() != 0)
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	const RenderResult closeResult = m_state->BeginShutdown();
	if (closeResult != RENDER_RESULT_OK)
	{
		return closeResult;
	}
	unsigned int drained = 0;
	const RenderResult drainResult = m_state->DrainCleanup(0, &drained);
	if (drainResult != RENDER_RESULT_OK)
	{
		return drainResult;
	}
	const RenderResult detachResult = m_state->DetachBackend();
	if (detachResult != RENDER_RESULT_OK)
	{
		return detachResult;
	}
	m_state->Release();
	m_state = 0;
	return RENDER_RESULT_OK;
}

bool NativeW3DResourceHost::IsAttached() const
{
	IRenderDevice *device = m_state == 0 ? 0 : m_state->Device();
	return m_state != 0 && m_state->IsOperational() && device != 0 &&
		device->isOperational();
}

unsigned int NativeW3DResourceHost::PendingCleanup() const
{
	return m_state == 0 ? 0 : m_state->PendingCleanup();
}

unsigned int NativeW3DResourceHost::BoundResourceTables() const
{
	return m_state == 0 ? 0 : m_state->BoundResourceTables();
}

NativeW3DRenderState *NativeW3DResourceHost::State() const
{
	return m_state;
}

struct InitializedByteRange
{
	InitializedByteRange() : begin(0), end(0) {}
	InitializedByteRange(size_t requestedBegin, size_t requestedEnd) :
		begin(requestedBegin), end(requestedEnd) {}
	size_t begin;
	size_t end;
};

struct PendingBufferPublication
{
	PendingBufferPublication() : sequence(0),
		authority(NATIVE_W3D_CONTENT_INVALID), backendEpoch(0) {}
	NativeW3DSubmissionSequence sequence;
	std::vector<InitializedByteRange> initializedBytes;
	NativeW3DContentAuthority authority;
	unsigned int backendEpoch;
};

struct NativeW3DResources::Slot
{
	Slot() : kind(0), authority(NATIVE_W3D_CONTENT_INVALID),
		authorityEpoch(0), backendEpoch(0), retired(false),
		submissionAuthority(NATIVE_W3D_CONTENT_INVALID),
		submissionBackendEpoch(0) {}
	GpuHandle handle;
	unsigned int kind;
	BufferDescriptor buffer;
	TextureDescriptor texture;
	NativeW3DContentAuthority authority;
	unsigned int authorityEpoch;
	unsigned int backendEpoch;
	bool retired;
	std::vector<InitializedByteRange> initializedBytes;
	// Confirmed bytes are published only after the matching threaded frame
	// completion. Submission bytes include accepted FIFO uploads so a draw in
	// the same or a dependent queued frame can reference them without fencing.
	std::vector<InitializedByteRange> submissionInitializedBytes;
	NativeW3DContentAuthority submissionAuthority;
	unsigned int submissionBackendEpoch;
	std::vector<PendingBufferPublication> pendingBufferPublications;
	// DEFAULT buffers retain an independent authoritative CPU-range map. It is
	// not discarded when an asynchronous GPU upload fails and is the source for
	// explicit recovery publication.
	std::vector<InitializedByteRange> recoveryInitializedBytes;
	std::vector<unsigned char> recoveryBytes;
};

// One preallocated, opaque cleanup path follows every owned texture from
// candidate creation through publication.  Its embedded fallback entry lets a
// foreign destructor transfer the last token to the render-owner queue without
// allocating or depending on the currently published bridge generation.
class NativeW3DTextureCleanupTicket
{
public:
	NativeW3DTextureCleanupTicket(void *table,
		NativeW3DTextureHandle handle) :
		m_table(table), m_handle(handle), m_fallback(), m_next(0)
	{
	}

	void *m_table;
	NativeW3DTextureHandle m_handle;
	NativeW3DOwnerFallbackEntry m_fallback;
	NativeW3DTextureCleanupTicket *m_next;

private:
	NativeW3DTextureCleanupTicket(const NativeW3DTextureCleanupTicket &);
	NativeW3DTextureCleanupTicket &operator=(
		const NativeW3DTextureCleanupTicket &);
};

struct NativeW3DResources::Impl
{
	Impl(unsigned int capacity) : references(1), state(0), generation(0),
		stateGeneration(0),
		nextAuthorityEpoch(0), lastThreadedCompletionSequence(0), slots(capacity),
		cleanupTickets(0) {}
	~Impl()
	{
		// Each ticket owns one table reference, so the final table release can
		// occur only after every external candidate/publication token is gone.
		assert(cleanupTickets == 0);
	}
	volatile long references;
	mutable ResourceTableLock cleanupLock;
	NativeW3DRenderState *state;
	// Unlike a backend state's generation, this table-owned attachment epoch
	// never resets when the table moves to a fresh host or renderer.
	unsigned int generation;
	unsigned int stateGeneration;
	unsigned int nextAuthorityEpoch;
	NativeW3DSubmissionSequence lastThreadedCompletionSequence;
	std::vector<Slot> slots;
	NativeW3DOwnerFallbackEntry fallbackCleanup;
	NativeW3DTextureCleanupTicket *cleanupTickets;
};

namespace
{
const size_t MAX_NATIVE_TEXTURE_UPLOAD_BYTES = 256U * 1024U * 1024U;

void RecordInitializedByteRange(std::vector<InitializedByteRange> &ranges,
	size_t destinationOffset, size_t byteCount, RenderBufferUpdateMode mode)
{
	if (mode == RENDER_BUFFER_UPDATE_DISCARD)
	{
		ranges.clear();
	}
	const InitializedByteRange added(destinationOffset,
		destinationOffset + byteCount);
	std::vector<InitializedByteRange> merged;
	merged.reserve(ranges.size() + 1);
	InitializedByteRange combined = added;
	bool inserted = false;
	for (size_t index = 0; index < ranges.size(); ++index)
	{
		const InitializedByteRange &existing = ranges[index];
		if (existing.end < combined.begin)
		{
			merged.push_back(existing);
		}
		else if (combined.end < existing.begin)
		{
			if (!inserted)
			{
				merged.push_back(combined);
				inserted = true;
			}
			merged.push_back(existing);
		}
		else
		{
			if (existing.begin < combined.begin)
			{
				combined.begin = existing.begin;
			}
			if (existing.end > combined.end)
			{
				combined.end = existing.end;
			}
		}
	}
	if (!inserted)
	{
		merged.push_back(combined);
	}
	ranges.swap(merged);
}

bool ContainsInitializedByteRange(
	const std::vector<InitializedByteRange> &ranges, size_t offset,
	size_t byteCount)
{
	const size_t end = offset + byteCount;
	for (size_t index = 0; index < ranges.size(); ++index)
	{
		const InitializedByteRange &range = ranges[index];
		if (range.begin <= offset && range.end >= end)
		{
			return true;
		}
		if (range.begin > offset)
		{
			break;
		}
	}
	return false;
}

bool EqualTextureDescriptors(const TextureDescriptor &left,
	const TextureDescriptor &right)
{
	return left.width == right.width && left.height == right.height &&
		left.mipCount == right.mipCount &&
		left.arrayCount == right.arrayCount &&
		left.dimension == right.dimension && left.format == right.format &&
		left.binding == right.binding && left.usage == right.usage;
}

RenderResult WaitForMutation(IRenderDevice *device, RenderResult result)
{
	if (result != RENDER_RESULT_OK)
	{
		return result;
	}
	#if defined(RTS_RENDERER_HAS_D3D11)
	if (IsThreadedRenderDevice(device))
	{
		return DrainThreadedRenderDevice(device);
	}
	#endif
	return result;
}

RenderResult DestroyResourceAndWait(IRenderDevice *device, GpuHandle handle)
{
	#if defined(RTS_RENDERER_HAS_D3D11)
	if (IsThreadedRenderDevice(device))
	{
		return RollbackThreadedRenderResource(device, handle);
	}
	#endif
	return device != 0 && device->destroyResource(handle) ?
		WaitForMutation(device, RENDER_RESULT_OK) : RENDER_RESULT_FAILED;
}

RenderResult RollbackCreatedResource(IRenderDevice *device, GpuHandle handle)
{
	return DestroyResourceAndWait(device, handle);
}
}

void NativeW3DResources::AddImplReference(Impl *impl)
{
	if (impl == 0)
	{
		return;
	}
#ifdef _WIN32
#if defined(_MSC_VER) && _MSC_VER < 1300
	InterlockedIncrement(const_cast<long *>(&impl->references));
#else
	InterlockedIncrement(&impl->references);
#endif
#else
	__sync_add_and_fetch(&impl->references, 1);
#endif
}

void NativeW3DResources::ReleaseImplReference(Impl *impl)
{
	if (impl == 0)
	{
		return;
	}
	long references = 0;
#ifdef _WIN32
#if defined(_MSC_VER) && _MSC_VER < 1300
	references = InterlockedDecrement(const_cast<long *>(&impl->references));
#else
	references = InterlockedDecrement(&impl->references);
#endif
#else
	references = __sync_sub_and_fetch(&impl->references, 1);
#endif
	if (references == 0)
	{
		delete impl;
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
		if (!m_impl->cleanupLock.IsInitialized())
		{
			delete m_impl;
			m_impl = 0;
		}
	}
	catch (...)
	{
		m_impl = 0;
	}
}

NativeW3DResources::~NativeW3DResources()
{
	if (m_impl == 0)
	{
		return;
	}
	NativeW3DRenderState *state = 0;
	{
		ScopedResourceTableLock lock(m_impl->cleanupLock);
		state = m_impl->state;
	}
	if (state == 0)
	{
		Impl *releasedImpl = m_impl;
		m_impl = 0;
		ReleaseImplReference(releasedImpl);
		return;
	}
	bool released = false;
	if (state->IsOwnerThread())
	{
		released = Shutdown() == RENDER_RESULT_OK;
	}
	if (!released && m_impl->state != 0)
	{
		const RenderResult accepted =
			m_impl->state->EnqueueFallbackCleanup(
				DestroyDeferredResourceTable, m_impl,
				ReleaseDeferredResourceTable,
				&m_impl->fallbackCleanup);
		// The embedded fallback path allocates nothing and is bounded by the
		// registered table count. Enqueue is atomic with queue Close, so no slot
		// inspection is needed on this foreign thread.
		if (accepted == RENDER_RESULT_OK)
		{
			m_impl = 0;
			return;
		}
		if (state->IsBackendTerminal())
		{
			// Close and fallback enqueue are serialized by the queue. Recovery
			// publishes terminal backend ownership before Close, so a rejected
			// transfer can safely complete metadata-only cleanup here.
			bool terminallyReleased = false;
			{
				ScopedResourceTableLock lock(m_impl->cleanupLock);
				if (m_impl->state == state)
				{
					for (size_t index = 0;
						index < m_impl->slots.size(); ++index)
					{
						m_impl->slots[index] = Slot();
					}
					m_impl->state = 0;
					m_impl->stateGeneration = 0;
					m_impl->lastThreadedCompletionSequence = 0;
					terminallyReleased = true;
				}
			}
			if (terminallyReleased)
			{
				state->UnregisterResourceTable();
				state->Release();
				Impl *releasedImpl = m_impl;
				m_impl = 0;
				ReleaseImplReference(releasedImpl);
				return;
			}
		}
		// A non-terminal lifecycle misuse has closed the queue while native
		// allocations remain. Retain state intentionally and fail closed.
		m_impl = 0;
		return;
	}
	Impl *releasedImpl = m_impl;
	m_impl = 0;
	ReleaseImplReference(releasedImpl);
}

RenderResult NativeW3DResources::Bind(NativeW3DRenderer *renderer)
{
	if (renderer == 0 || renderer->m_state == 0 ||
		!renderer->IsInitialized() || !renderer->IsOwnerThread())
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	return BindState(renderer->m_state);
}

RenderResult NativeW3DResources::BindHost(NativeW3DResourceHost *host)
{
	return host == 0 ? RENDER_RESULT_INVALID_ARGUMENT :
		BindState(host->State());
}

RenderResult NativeW3DResources::BindState(NativeW3DRenderState *state)
{
	if (m_impl == 0)
	{
		return RENDER_RESULT_OUT_OF_MEMORY;
	}
	{
		ScopedResourceTableLock lock(m_impl->cleanupLock);
		if (m_impl->state != 0)
		{
			return RENDER_RESULT_INVALID_ARGUMENT;
		}
		if (m_impl->cleanupTickets != 0 || m_impl->generation == UINT_MAX)
		{
			// A prior attachment's externally held tokens must reach zero before
			// this table can publish a new backend identity. Epoch exhaustion is
			// likewise terminal so no stale typed token can become current again.
			return RENDER_RESULT_FAILED;
		}
	}
	IRenderDevice *device = state == 0 ? 0 : state->Device();
	if (state == 0 || !state->IsOperational() ||
		!state->IsOwnerThread() || device == 0 || !device->isOperational())
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	const unsigned int stateGeneration = state->Generation();
	const unsigned int backendEpoch = state->BackendEpoch();
	if (stateGeneration == 0 || backendEpoch == 0)
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	const RenderResult registerResult = state->RegisterResourceTable();
	if (registerResult != RENDER_RESULT_OK)
	{
		return registerResult;
	}
	state->AddRef();
	{
		ScopedResourceTableLock lock(m_impl->cleanupLock);
		m_impl->state = state;
		++m_impl->generation;
		m_impl->stateGeneration = stateGeneration;
		m_impl->lastThreadedCompletionSequence = 0;
	}
	return RENDER_RESULT_OK;
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
	IRenderDevice *device = m_impl->state->Device();
	const bool canDestroy = m_impl->state->IsOperational() && device != 0 &&
		device->isOperational() &&
		m_impl->stateGeneration == m_impl->state->Generation();
	const bool terminallyDetached = device == 0 &&
		m_impl->stateGeneration != m_impl->state->Generation();
	const bool terminallyUnavailable = device != 0 &&
		!device->isOperational();
	bool hasCleanupTickets = false;
	{
		ScopedResourceTableLock lock(m_impl->cleanupLock);
		hasCleanupTickets = m_impl->cleanupTickets != 0;
	}
	if (hasCleanupTickets && !terminallyDetached && !terminallyUnavailable)
	{
		// Operational tables keep exact backend ownership until every published
		// token is released. A terminal backend has already discarded its native
		// allocation table, so only the independently referenced tickets remain.
		return RENDER_RESULT_FAILED;
	}
	for (size_t index = 0; index < m_impl->slots.size(); ++index)
	{
		Slot &slot = m_impl->slots[index];
		if (slot.handle.isValid() && !canDestroy && !terminallyDetached &&
			!terminallyUnavailable)
		{
			return RENDER_RESULT_FAILED;
		}
		if (slot.handle.isValid() &&
			(terminallyDetached || terminallyUnavailable))
		{
			// A terminal detach or failed backend recovery has already destroyed
			// the native allocation table. Only fail-closed local metadata remains.
			slot = Slot();
			continue;
		}
		if (slot.handle.isValid())
		{
			const RenderResult destroyResult =
				DestroyResourceAndWait(device, slot.handle);
			if (destroyResult != RENDER_RESULT_OK)
			{
				return destroyResult;
			}
		}
		slot = Slot();
	}
	NativeW3DRenderState *releasedState = m_impl->state;
	{
		ScopedResourceTableLock lock(m_impl->cleanupLock);
		m_impl->state = 0;
		m_impl->stateGeneration = 0;
		m_impl->lastThreadedCompletionSequence = 0;
	}
	releasedState->UnregisterResourceTable();
	releasedState->Release();
	return RENDER_RESULT_OK;
}

RenderResult NativeW3DResources::PublishThreadedCompletion(
	NativeW3DSubmissionSequence submissionSequence, bool resourceFailure)
{
	if (m_impl == 0)
	{
		return RENDER_RESULT_OUT_OF_MEMORY;
	}
	if (m_impl->state == 0 || !m_impl->state->IsOwnerThread() ||
		submissionSequence == 0 ||
		submissionSequence <= m_impl->lastThreadedCompletionSequence)
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	for (size_t slotIndex = 0; slotIndex < m_impl->slots.size(); ++slotIndex)
	{
		Slot &slot = m_impl->slots[slotIndex];
		if (!slot.handle.isValid() || slot.kind != 1 || slot.retired ||
			slot.pendingBufferPublications.empty())
		{
			continue;
		}
		size_t completedCount = 0;
		while (completedCount < slot.pendingBufferPublications.size() &&
			slot.pendingBufferPublications[completedCount].sequence <=
				submissionSequence)
		{
			++completedCount;
		}
		if (completedCount == 0)
		{
			continue;
		}
		if (resourceFailure)
		{
			// Completion does not expose a per-command failure handle. Invalidate
			// only slots that had accepted writes in the failed sequence window;
			// unrelated buffer authority and DEFAULT recovery sources survive.
			slot.initializedBytes.clear();
			slot.submissionInitializedBytes.clear();
			slot.pendingBufferPublications.clear();
			slot.authority = NATIVE_W3D_CONTENT_INVALID;
			slot.submissionAuthority = NATIVE_W3D_CONTENT_INVALID;
			slot.backendEpoch = 0;
			slot.submissionBackendEpoch = 0;
			slot.authorityEpoch = NextAuthorityEpoch();
			continue;
		}

		const PendingBufferPublication &published =
			slot.pendingBufferPublications[completedCount - 1];
		std::vector<InitializedByteRange> committedRanges;
		std::vector<PendingBufferPublication> remainingPublications;
		try
		{
			committedRanges = published.initializedBytes;
			remainingPublications.assign(
				slot.pendingBufferPublications.begin() + completedCount,
				slot.pendingBufferPublications.end());
		}
		catch (...)
		{
			slot.initializedBytes.clear();
			slot.submissionInitializedBytes.clear();
			slot.pendingBufferPublications.clear();
			slot.authority = NATIVE_W3D_CONTENT_INVALID;
			slot.submissionAuthority = NATIVE_W3D_CONTENT_INVALID;
			slot.backendEpoch = 0;
			slot.submissionBackendEpoch = 0;
			slot.authorityEpoch = NextAuthorityEpoch();
			return RENDER_RESULT_OUT_OF_MEMORY;
		}
		slot.initializedBytes.swap(committedRanges);
		slot.authority = published.authority;
		slot.backendEpoch = published.backendEpoch;
		slot.authorityEpoch = NextAuthorityEpoch();
		slot.pendingBufferPublications.swap(remainingPublications);
		if (slot.pendingBufferPublications.empty())
		{
			// The submission view already contains the same final snapshot.
			slot.submissionAuthority = slot.authority;
			slot.submissionBackendEpoch = slot.backendEpoch;
		}
	}
	m_impl->lastThreadedCompletionSequence = submissionSequence;
	if (resourceFailure)
	{
		InvalidateGpuAuthorities();
	}
	return RENDER_RESULT_OK;
}

RenderResult NativeW3DResources::CreateBuffer(
	const BufferDescriptor &descriptor, const void *initialData,
	size_t initialDataBytes, GpuHandle *handle)
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
	if (device == 0 || !device->isOperational() ||
		!m_impl->state->IsOperational() ||
		m_impl->stateGeneration != m_impl->state->Generation())
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	Slot *available = 0;
	for (size_t index = 0; index < m_impl->slots.size(); ++index)
	{
		if (!m_impl->slots[index].handle.isValid())
		{
			available = &m_impl->slots[index];
			break;
		}
	}
	if (available == 0)
	{
		return RENDER_RESULT_OUT_OF_MEMORY;
	}
	GpuHandle created;
	const RenderResult result = device->createBuffer(descriptor, initialData,
		initialDataBytes, &created);
	if (result != RENDER_RESULT_OK)
	{
		return result;
	}
	if (!created.isValid())
	{
		return RENDER_RESULT_FAILED;
	}
	Slot &slot = *available;
	std::vector<InitializedByteRange> initializedBytes;
	std::vector<InitializedByteRange> submissionInitializedBytes;
	std::vector<InitializedByteRange> recoveryInitializedBytes;
	std::vector<unsigned char> recoveryBytes;
	try
	{
		if (descriptor.usage == RENDER_USAGE_DEFAULT)
		{
			recoveryBytes.resize(descriptor.byteCount);
			if (initialData != 0 && initialDataBytes == descriptor.byteCount)
			{
				memcpy(&recoveryBytes[0], initialData, descriptor.byteCount);
			}
		}
		if (initialData != 0 && initialDataBytes == descriptor.byteCount)
		{
			initializedBytes.push_back(InitializedByteRange(0,
				descriptor.byteCount));
		}
		submissionInitializedBytes = initializedBytes;
		if (descriptor.usage == RENDER_USAGE_DEFAULT)
		{
			recoveryInitializedBytes = initializedBytes;
		}
	}
	catch (...)
	{
		if (RollbackCreatedResource(device, created) != RENDER_RESULT_OK)
		{
			slot.handle = created;
			slot.kind = 1;
			slot.buffer = descriptor;
			slot.authority = NATIVE_W3D_CONTENT_INVALID;
			slot.authorityEpoch = NextAuthorityEpoch();
			slot.backendEpoch = m_impl->state->BufferEpoch();
			slot.submissionBackendEpoch = slot.backendEpoch;
		}
		return RENDER_RESULT_OUT_OF_MEMORY;
	}
	const RenderResult completionResult = CompleteBufferMutation(device, result, 0);
	if (completionResult != RENDER_RESULT_OK)
	{
		if (RollbackCreatedResource(device, created) == RENDER_RESULT_OK)
		{
			return completionResult;
		}
		// Rollback itself failed. Retain the unpublished handle so owner
		// shutdown still has a deterministic cleanup path after recovery.
		slot.handle = created;
		slot.kind = 1;
		slot.buffer = descriptor;
		slot.authority = NATIVE_W3D_CONTENT_INVALID;
		slot.authorityEpoch = NextAuthorityEpoch();
		slot.backendEpoch = m_impl->state->BufferEpoch();
		slot.initializedBytes.swap(initializedBytes);
		slot.submissionInitializedBytes.swap(submissionInitializedBytes);
		slot.submissionAuthority = slot.authority;
		slot.submissionBackendEpoch = slot.backendEpoch;
		slot.recoveryInitializedBytes.swap(recoveryInitializedBytes);
		slot.recoveryBytes.swap(recoveryBytes);
		return completionResult;
	}
	slot.handle = created;
	slot.kind = 1;
	slot.buffer = descriptor;
	slot.authority = initialData != 0 &&
		initialDataBytes == descriptor.byteCount ? NATIVE_W3D_CONTENT_CPU :
		NATIVE_W3D_CONTENT_INVALID;
	slot.authorityEpoch = NextAuthorityEpoch();
	slot.backendEpoch = m_impl->state->BufferEpoch();
	slot.initializedBytes.swap(initializedBytes);
	slot.submissionInitializedBytes.swap(submissionInitializedBytes);
	slot.submissionAuthority = slot.authority;
	slot.submissionBackendEpoch = slot.backendEpoch;
	slot.recoveryInitializedBytes.swap(recoveryInitializedBytes);
	slot.recoveryBytes.swap(recoveryBytes);
	*handle = created;
	return RENDER_RESULT_OK;
}

RenderResult NativeW3DResources::CreateTexture(
	const TextureDescriptor &descriptor,
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
	size_t uploadBytes = 0;
	const RenderResult validationResult = ValidateTextureUpload(descriptor,
		initialData, initialDataCount, false,
		MAX_NATIVE_TEXTURE_UPLOAD_BYTES, &uploadBytes);
	if (validationResult != RENDER_RESULT_OK)
	{
		return validationResult;
	}
	IRenderDevice *device = m_impl->state == 0 ? 0 : m_impl->state->Device();
	if (device == 0 || !device->isOperational() ||
		!m_impl->state->IsOperational() ||
		m_impl->stateGeneration != m_impl->state->Generation())
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	Slot *available = 0;
	for (size_t index = 0; index < m_impl->slots.size(); ++index)
	{
		if (!m_impl->slots[index].handle.isValid())
		{
			available = &m_impl->slots[index];
			break;
		}
	}
	if (available == 0)
	{
		return RENDER_RESULT_OUT_OF_MEMORY;
	}
	GpuHandle created;
	const RenderResult result = device->createTexture(descriptor, initialData,
		initialDataCount, &created);
	if (result != RENDER_RESULT_OK)
	{
		if (result == RENDER_RESULT_DEVICE_REMOVED)
		{
			InvalidateAllAuthorities();
		}
		return result;
	}
	if (!created.isValid())
	{
		return RENDER_RESULT_FAILED;
	}
	Slot &slot = *available;
	const RenderResult completionResult = CompleteMutation(device, result);
	if (completionResult != RENDER_RESULT_OK)
	{
		if (RollbackCreatedResource(device, created) == RENDER_RESULT_OK)
		{
			return completionResult;
		}
		slot.handle = created;
		slot.kind = 2;
		slot.texture = descriptor;
		slot.authority = NATIVE_W3D_CONTENT_INVALID;
		slot.authorityEpoch = NextAuthorityEpoch();
		slot.backendEpoch = m_impl->state->BackendEpoch();
		return completionResult;
	}
	slot.handle = created;
	slot.kind = 2;
	slot.texture = descriptor;
	slot.authority = initialData != 0 ? NATIVE_W3D_CONTENT_CPU :
		NATIVE_W3D_CONTENT_INVALID;
	slot.authorityEpoch = NextAuthorityEpoch();
	slot.backendEpoch = m_impl->state->BackendEpoch();
	*handle = created;
	return RENDER_RESULT_OK;
}

RenderResult NativeW3DResources::CreateTexture(
	const TextureDescriptor &descriptor,
	const TextureSubresourceData *initialData, unsigned int initialDataCount,
	NativeW3DTextureHandle *handle)
{
	if (handle == 0)
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	*handle = NativeW3DTextureHandle();
	GpuHandle resource;
	const RenderResult result = CreateTexture(descriptor, initialData,
		initialDataCount, &resource);
	if (result != RENDER_RESULT_OK)
	{
		return result;
	}
	const RenderResult acquisitionResult = AcquireTexture(resource, handle);
	if (acquisitionResult == RENDER_RESULT_OK)
	{
		return RENDER_RESULT_OK;
	}
	Destroy(resource);
	return acquisitionResult;
}

RenderResult NativeW3DResources::AcquireTexture(GpuHandle resource,
	NativeW3DTextureHandle *handle) const
{
	if (handle == 0)
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	*handle = NativeW3DTextureHandle();
	const Slot *slot = Find(resource);
	IRenderDevice *device = m_impl == 0 || m_impl->state == 0 ? 0 :
		m_impl->state->Device();
	if (slot == 0 || slot->kind != 2 || slot->retired || device == 0 ||
		!device->isOperational() || !m_impl->state->IsOperational() ||
		m_impl->stateGeneration != m_impl->state->Generation())
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	handle->resource = resource;
	handle->attachmentGeneration = m_impl->generation;
	return RENDER_RESULT_OK;
}

RenderResult NativeW3DResources::AcquireTextureSurface(
	NativeW3DTextureHandle texture, unsigned int mipLevel,
	unsigned int arraySlice, NativeW3DSurfaceHandle *surface) const
{
	if (surface == 0)
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	const NativeW3DSurfaceHandle requested = *surface;
	*surface = NativeW3DSurfaceHandle();
	const Slot *slot = Find(texture.resource);
	IRenderDevice *device = m_impl == 0 || m_impl->state == 0 ? 0 :
		m_impl->state->Device();
	if (!texture.isValid() || slot == 0 || slot->kind != 2 || slot->retired ||
		device == 0 ||
		!device->isOperational() || !m_impl->state->IsOperational() ||
		texture.attachmentGeneration != m_impl->generation ||
		m_impl->stateGeneration != m_impl->state->Generation() ||
		mipLevel >= slot->texture.mipCount ||
		arraySlice >= slot->texture.arrayCount)
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	const unsigned int backendEpoch = m_impl->state->BackendEpoch();
	const unsigned int mipWidth = slot->texture.width >> mipLevel;
	const unsigned int mipHeight = slot->texture.height >> mipLevel;
	const unsigned int width = mipWidth == 0 ? 1 : mipWidth;
	const unsigned int height = mipHeight == 0 ? 1 : mipHeight;
	if (requested.isValid() &&
		(requested.texture.resource != texture.resource ||
		 requested.texture.attachmentGeneration != texture.attachmentGeneration ||
		 requested.backendEpoch != backendEpoch ||
		 requested.mipLevel != mipLevel ||
		 requested.arraySlice != arraySlice || requested.width != width ||
		 requested.height != height || requested.format != slot->texture.format))
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	surface->texture = texture;
	surface->backendEpoch = backendEpoch;
	surface->mipLevel = mipLevel;
	surface->arraySlice = arraySlice;
	surface->width = width;
	surface->height = height;
	surface->format = slot->texture.format;
	return RENDER_RESULT_OK;
}

RenderResult NativeW3DResources::PublishRenderTargetWrite(
	NativeW3DSurfaceHandle surface, NativeW3DGpuContentLease *lease)
{
	if (lease == 0)
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	*lease = NativeW3DGpuContentLease();
	if (!surface.isValid() || !IsValid(surface) || m_impl == 0 ||
		m_impl->state == 0 || !m_impl->state->IsOwnerThread())
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	Slot *slot = Find(surface.texture.resource);
	IRenderDevice *device = m_impl->state->Device();
	if (slot == 0 || slot->kind != 2 || slot->retired || device == 0 ||
		!device->isOperational() ||
		(slot->texture.binding & (RENDER_TEXTURE_RENDER_TARGET |
			RENDER_TEXTURE_DEPTH_STENCIL)) == 0 ||
		m_impl->stateGeneration != m_impl->state->Generation())
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}

	// setRenderTargets has accepted this exact logical resource before this
	// publication. The backend owns native output binding and recovery; this
	// table owns only the content epoch consumed by generation-safe callers.
	slot->authority = NATIVE_W3D_CONTENT_GPU_RENDER_TARGET;
	slot->authorityEpoch = NextAuthorityEpoch();
	slot->backendEpoch = m_impl->state->BackendEpoch();
	PopulateLease(*slot, lease);
	return RENDER_RESULT_OK;
}

RenderResult NativeW3DResources::UpdateBuffer(GpuHandle handle,
	const void *bytes, size_t byteCount, size_t destinationOffset,
	RenderBufferUpdateMode mode)
{
	IRenderDevice *device = m_impl == 0 || m_impl->state == 0 ? 0 :
		m_impl->state->Device();
	Slot *slot = Find(handle);
	if (device == 0 || slot == 0 || slot->kind != 1 || bytes == 0 ||
		byteCount == 0 || destinationOffset > slot->buffer.byteCount ||
		byteCount > slot->buffer.byteCount - destinationOffset ||
		m_impl->stateGeneration != m_impl->state->Generation())
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	const unsigned int bufferEpoch = m_impl->state->BufferEpoch();
	NativeW3DSubmissionSequence submissionSequence = 0;
#if defined(RTS_RENDERER_HAS_D3D11)
	if (IsThreadedRenderDevice(device))
	{
		submissionSequence = static_cast<NativeW3DSubmissionSequence>(
			CurrentThreadedRenderFrameSequence(device));
	}
#endif
	const bool asynchronousPublication = submissionSequence != 0;
	if (!asynchronousPublication && !slot->pendingBufferPublications.empty())
	{
		// A resource-only update cannot be ordered after an unconsumed frame
		// completion without losing the exact publication boundary.
		return RENDER_RESULT_INVALID_ARGUMENT;
	}

	std::vector<InitializedByteRange> nextSubmissionRanges;
	std::vector<InitializedByteRange> nextCommittedRanges;
	std::vector<InitializedByteRange> nextRecoveryRanges;
	std::vector<PendingBufferPublication> nextPublications;
	NativeW3DContentAuthority nextAuthority = NATIVE_W3D_CONTENT_INVALID;
	try
	{
		if (slot->submissionBackendEpoch == bufferEpoch)
		{
			nextSubmissionRanges = slot->submissionInitializedBytes;
		}
		RecordInitializedByteRange(nextSubmissionRanges, destinationOffset,
			byteCount, mode);
		const bool fullWrite = destinationOffset == 0 &&
			byteCount == slot->buffer.byteCount;
		const bool hadWholeBufferCpuAuthority =
			slot->submissionBackendEpoch == bufferEpoch &&
			slot->submissionAuthority == NATIVE_W3D_CONTENT_CPU;
		nextAuthority = fullWrite ||
			(mode != RENDER_BUFFER_UPDATE_DISCARD &&
				hadWholeBufferCpuAuthority) ? NATIVE_W3D_CONTENT_CPU :
			NATIVE_W3D_CONTENT_INVALID;
		if (slot->buffer.usage == RENDER_USAGE_DEFAULT)
		{
			if (slot->recoveryBytes.size() != slot->buffer.byteCount)
			{
				return RENDER_RESULT_FAILED;
			}
			nextRecoveryRanges = slot->recoveryInitializedBytes;
			RecordInitializedByteRange(nextRecoveryRanges, destinationOffset,
				byteCount, mode);
		}
		if (asynchronousPublication)
		{
			nextPublications = slot->pendingBufferPublications;
			PendingBufferPublication publication;
			publication.sequence = submissionSequence;
			publication.initializedBytes = nextSubmissionRanges;
			publication.authority = nextAuthority;
			publication.backendEpoch = bufferEpoch;
			if (!nextPublications.empty() &&
				nextPublications.back().sequence == submissionSequence)
			{
				nextPublications.back() = publication;
			}
			else
			{
				nextPublications.push_back(publication);
			}
		}
		else
		{
			nextCommittedRanges = nextSubmissionRanges;
		}
	}
	catch (...)
	{
		return RENDER_RESULT_OUT_OF_MEMORY;
	}

	const RenderResult accepted = device->updateBufferResource(handle, bytes,
		byteCount, destinationOffset, mode);
	if (accepted != RENDER_RESULT_OK)
	{
		if (!asynchronousPublication)
		{
			InvalidateBufferAuthority(*slot);
		}
		return accepted;
	}
	if (asynchronousPublication)
	{
		slot->submissionInitializedBytes.swap(nextSubmissionRanges);
		slot->submissionAuthority = nextAuthority;
		slot->submissionBackendEpoch = bufferEpoch;
		slot->pendingBufferPublications.swap(nextPublications);
		if (slot->buffer.usage == RENDER_USAGE_DEFAULT)
		{
			memcpy(&slot->recoveryBytes[destinationOffset], bytes, byteCount);
			slot->recoveryInitializedBytes.swap(nextRecoveryRanges);
		}
		return RENDER_RESULT_OK;
	}

	const RenderResult completed = CompleteBufferMutation(device, accepted, slot);
	if (completed != RENDER_RESULT_OK)
	{
		return completed;
	}
	slot->initializedBytes.swap(nextCommittedRanges);
	slot->submissionInitializedBytes.swap(nextSubmissionRanges);
	slot->authority = nextAuthority;
	slot->submissionAuthority = nextAuthority;
	slot->authorityEpoch = NextAuthorityEpoch();
	slot->backendEpoch = bufferEpoch;
	slot->submissionBackendEpoch = bufferEpoch;
	if (slot->buffer.usage == RENDER_USAGE_DEFAULT)
	{
		memcpy(&slot->recoveryBytes[destinationOffset], bytes, byteCount);
		slot->recoveryInitializedBytes.swap(nextRecoveryRanges);
	}
	return RENDER_RESULT_OK;
}

RenderResult NativeW3DResources::RepublishStaticBuffersAfterResize()
{
	return RepublishStaticBuffers(false);
}

RenderResult NativeW3DResources::RestoreStaticBuffersAfterRecovery()
{
	return RepublishStaticBuffers(true);
}

RenderResult NativeW3DResources::RepublishStaticBuffers(
	bool advanceBufferEpoch)
{
	IRenderDevice *device = m_impl == 0 || m_impl->state == 0 ? 0 :
		m_impl->state->Device();
	if (device == 0 || !device->isOperational() ||
		!m_impl->state->IsOperational() ||
		m_impl->stateGeneration != m_impl->state->Generation())
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	if (advanceBufferEpoch)
	{
		const RenderResult epochResult = m_impl->state->AdvanceBufferEpoch();
		if (epochResult != RENDER_RESULT_OK)
		{
			return epochResult;
		}
	}
	const unsigned int bufferEpoch = m_impl->state->BufferEpoch();
	for (size_t slotIndex = 0; slotIndex < m_impl->slots.size(); ++slotIndex)
	{
		Slot &slot = m_impl->slots[slotIndex];
		if (!slot.handle.isValid() || slot.kind != 1 || slot.retired)
		{
			continue;
		}
		if (advanceBufferEpoch)
		{
			slot.pendingBufferPublications.clear();
		}
		if (slot.buffer.usage == RENDER_USAGE_IMMUTABLE)
		{
			if (!advanceBufferEpoch)
			{
				continue;
			}
			// The backend recreates immutable buffers from their full creation
			// image. Publish only that exact contract; immutable buffers cannot be
			// repaired through updateBufferResource.
			if (slot.buffer.byteCount == 0 ||
				!ContainsInitializedByteRange(slot.initializedBytes, 0,
					slot.buffer.byteCount))
			{
				InvalidateBufferAuthority(slot);
				return RENDER_RESULT_FAILED;
			}
			std::vector<InitializedByteRange> immutableSubmissionRanges;
			try
			{
				immutableSubmissionRanges = slot.initializedBytes;
			}
			catch (...)
			{
				InvalidateBufferAuthority(slot);
				return RENDER_RESULT_OUT_OF_MEMORY;
			}
			slot.backendEpoch = bufferEpoch;
			slot.submissionBackendEpoch = bufferEpoch;
			slot.submissionInitializedBytes.swap(immutableSubmissionRanges);
			slot.submissionAuthority = slot.authority;
			continue;
		}
		if (slot.buffer.usage != RENDER_USAGE_DEFAULT)
		{
			if (advanceBufferEpoch)
			{
				InvalidateBufferAuthority(slot);
			}
			continue;
		}
		if (slot.recoveryInitializedBytes.empty())
		{
			if (advanceBufferEpoch)
			{
				InvalidateBufferAuthority(slot);
			}
			continue;
		}
		if (slot.recoveryBytes.size() != slot.buffer.byteCount)
		{
			InvalidateBufferAuthority(slot);
			return RENDER_RESULT_FAILED;
		}
		std::vector<InitializedByteRange> restoredRanges;
		std::vector<InitializedByteRange> submissionRanges;
		try
		{
			restoredRanges = slot.recoveryInitializedBytes;
			submissionRanges = slot.recoveryInitializedBytes;
		}
		catch (...)
		{
			InvalidateBufferAuthority(slot);
			return RENDER_RESULT_OUT_OF_MEMORY;
		}
		for (size_t rangeIndex = 0;
			rangeIndex < slot.recoveryInitializedBytes.size(); ++rangeIndex)
		{
			const InitializedByteRange &range =
				slot.recoveryInitializedBytes[rangeIndex];
			if (range.begin >= range.end || range.end > slot.buffer.byteCount)
			{
				InvalidateBufferAuthority(slot);
				return RENDER_RESULT_FAILED;
			}
			const RenderResult result = CompleteBufferMutation(device,
				device->updateBufferResource(slot.handle,
					&slot.recoveryBytes[range.begin], range.end - range.begin,
					range.begin, RENDER_BUFFER_UPDATE_PRESERVE), &slot);
			if (result != RENDER_RESULT_OK)
			{
				return result;
			}
		}
		slot.initializedBytes.swap(restoredRanges);
		slot.submissionInitializedBytes.swap(submissionRanges);
		slot.authority = ContainsInitializedByteRange(slot.initializedBytes, 0,
			slot.buffer.byteCount) ? NATIVE_W3D_CONTENT_CPU :
			NATIVE_W3D_CONTENT_INVALID;
		slot.submissionAuthority = slot.authority;
		slot.backendEpoch = bufferEpoch;
		slot.submissionBackendEpoch = bufferEpoch;
	}
	return RENDER_RESULT_OK;
}

RenderResult NativeW3DResources::RefreshTexture(GpuHandle handle,
	const TextureDescriptor &descriptor,
	const TextureSubresourceData *subresources,
	unsigned int subresourceCount)
{
	IRenderDevice *device = m_impl == 0 || m_impl->state == 0 ? 0 :
		m_impl->state->Device();
	Slot *slot = Find(handle);
	if (device == 0 || !device->isOperational() || slot == 0 ||
		slot->kind != 2 || slot->retired ||
		m_impl->stateGeneration != m_impl->state->Generation())
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	if (!EqualTextureDescriptors(slot->texture, descriptor) ||
		descriptor.usage == RENDER_USAGE_IMMUTABLE)
	{
		return RENDER_RESULT_UNSUPPORTED;
	}
	if ((descriptor.binding & RENDER_TEXTURE_DEPTH_STENCIL) != 0)
	{
		// D3D11 depth-stencil resources are not legal UpdateSubresource
		// destinations. Preserve the prior authority metadata and native state.
		return RENDER_RESULT_UNSUPPORTED;
	}
	size_t uploadBytes = 0;
	const RenderResult validationResult = ValidateTextureUpload(descriptor,
		subresources, subresourceCount, true,
		MAX_NATIVE_TEXTURE_UPLOAD_BYTES, &uploadBytes);
	if (validationResult != RENDER_RESULT_OK)
	{
		return validationResult;
	}
	const RenderResult result = CompleteMutation(device,
		device->refreshTexture(handle, descriptor, subresources,
			subresourceCount));
	if (result == RENDER_RESULT_OK)
	{
		slot->authority = NATIVE_W3D_CONTENT_CPU;
		slot->authorityEpoch = NextAuthorityEpoch();
		slot->backendEpoch = m_impl->state->BackendEpoch();
	}
	return result;
}

RenderResult NativeW3DResources::RefreshTexture(
	NativeW3DTextureHandle handle, const TextureDescriptor &descriptor,
	const TextureSubresourceData *subresources,
	unsigned int subresourceCount)
{
	return IsValid(handle) ? RefreshTexture(handle.resource, descriptor,
		subresources, subresourceCount) : RENDER_RESULT_INVALID_ARGUMENT;
}

RenderResult NativeW3DResources::DescribeBuffer(GpuHandle handle,
	NativeW3DBufferDescription *description) const
{
	if (description == 0)
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	*description = NativeW3DBufferDescription();
	const Slot *slot = Find(handle);
	IRenderDevice *device = m_impl == 0 || m_impl->state == 0 ? 0 :
		m_impl->state->Device();
	if (slot == 0 || slot->kind != 1 || device == 0 ||
		!device->isOperational() ||
		m_impl->stateGeneration != m_impl->state->Generation())
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	description->descriptor = slot->buffer;
	description->authority = EffectiveAuthority(*slot);
	description->authorityEpoch = slot->authorityEpoch;
	return RENDER_RESULT_OK;
}

RenderResult NativeW3DResources::DescribeTexture(GpuHandle handle,
	NativeW3DTextureDescription *description) const
{
	if (description == 0)
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	*description = NativeW3DTextureDescription();
	const Slot *slot = Find(handle);
	IRenderDevice *device = m_impl == 0 || m_impl->state == 0 ? 0 :
		m_impl->state->Device();
	if (slot == 0 || slot->kind != 2 || slot->retired || device == 0 ||
		!device->isOperational() ||
		m_impl->stateGeneration != m_impl->state->Generation())
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	description->descriptor = slot->texture;
	description->authority = EffectiveAuthority(*slot);
	description->authorityEpoch = slot->authorityEpoch;
	return RENDER_RESULT_OK;
}

RenderResult NativeW3DResources::CopyActiveColorTargetToTexture(
	GpuHandle handle, NativeW3DGpuContentLease *lease)
{
	if (lease == 0)
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	*lease = NativeW3DGpuContentLease();
	IRenderDevice *device = m_impl == 0 || m_impl->state == 0 ? 0 :
		m_impl->state->Device();
	Slot *slot = Find(handle);
	if (device == 0 || !device->isOperational() || slot == 0 ||
		slot->kind != 2 || slot->retired ||
		m_impl->stateGeneration != m_impl->state->Generation())
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	const RenderResult result = CompleteMutation(device,
		device->copyActiveColorTargetToTexture(handle));
	if (result == RENDER_RESULT_OK)
	{
		slot->authority = NATIVE_W3D_CONTENT_GPU_RENDER_TARGET;
		slot->authorityEpoch = NextAuthorityEpoch();
		slot->backendEpoch = m_impl->state->BackendEpoch();
		PopulateLease(*slot, lease);
	}
	return result;
}

RenderResult NativeW3DResources::AcquireGpuContentLease(GpuHandle handle,
	NativeW3DGpuContentLease *lease) const
{
	if (lease == 0)
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	const NativeW3DGpuContentLease requested = *lease;
	*lease = NativeW3DGpuContentLease();
	const Slot *slot = Find(handle);
	IRenderDevice *device = m_impl == 0 || m_impl->state == 0 ? 0 :
		m_impl->state->Device();
	if (slot == 0 || slot->kind != 2 || slot->retired || device == 0 ||
		!device->isOperational() ||
		m_impl->stateGeneration != m_impl->state->Generation() ||
		EffectiveAuthority(*slot) != NATIVE_W3D_CONTENT_GPU_RENDER_TARGET)
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	if (requested.isValid() &&
		(requested.resource != slot->handle ||
		requested.attachmentGeneration != m_impl->generation ||
		requested.backendEpoch != slot->backendEpoch ||
		requested.authorityEpoch != slot->authorityEpoch))
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	PopulateLease(*slot, lease);
	return RENDER_RESULT_OK;
}

RenderResult NativeW3DResources::AcquireVertexBufferRange(
	GpuHandle resource, unsigned int stride, unsigned int offset,
	unsigned int startVertex, unsigned int vertexCount,
	GpuHandle *validated) const
{
	if (validated == 0)
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	*validated = GpuHandle();
	IRenderDevice *device = m_impl == 0 || m_impl->state == 0 ? 0 :
		m_impl->state->Device();
	if (m_impl == 0 || m_impl->state == 0 ||
		!m_impl->state->IsOperational() ||
		device == 0 || !device->isOperational() ||
		m_impl->stateGeneration != m_impl->state->Generation() ||
		!IsVertexRangeValid(resource, stride, offset, startVertex, vertexCount))
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	*validated = resource;
	return RENDER_RESULT_OK;
}

RenderResult NativeW3DResources::AcquireIndexBufferRange(
	GpuHandle resource, RenderFormat format, unsigned int offset,
	unsigned int startIndex, unsigned int indexCount,
	GpuHandle *validated) const
{
	if (validated == 0)
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	*validated = GpuHandle();
	IRenderDevice *device = m_impl == 0 || m_impl->state == 0 ? 0 :
		m_impl->state->Device();
	if (m_impl == 0 || m_impl->state == 0 ||
		!m_impl->state->IsOperational() ||
		device == 0 || !device->isOperational() ||
		m_impl->stateGeneration != m_impl->state->Generation() ||
		!IsIndexRangeValid(resource, format, offset, startIndex, indexCount))
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	*validated = resource;
	return RENDER_RESULT_OK;
}

RenderResult NativeW3DResources::AcquireVertexBufferRangeForSubmission(
	GpuHandle resource, unsigned int stride, unsigned int offset,
	unsigned int startVertex, unsigned int vertexCount,
	GpuHandle *validated) const
{
	if (validated == 0)
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	*validated = GpuHandle();
	IRenderDevice *device = m_impl == 0 || m_impl->state == 0 ? 0 :
		m_impl->state->Device();
	if (m_impl == 0 || m_impl->state == 0 ||
		!m_impl->state->IsOperational() || device == 0 ||
		!device->isOperational() ||
		m_impl->stateGeneration != m_impl->state->Generation() ||
		!IsVertexRangeValidForSubmission(resource, stride, offset,
			startVertex, vertexCount))
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	*validated = resource;
	return RENDER_RESULT_OK;
}

RenderResult NativeW3DResources::AcquireIndexBufferRangeForSubmission(
	GpuHandle resource, RenderFormat format, unsigned int offset,
	unsigned int startIndex, unsigned int indexCount,
	GpuHandle *validated) const
{
	if (validated == 0)
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	*validated = GpuHandle();
	IRenderDevice *device = m_impl == 0 || m_impl->state == 0 ? 0 :
		m_impl->state->Device();
	if (m_impl == 0 || m_impl->state == 0 ||
		!m_impl->state->IsOperational() || device == 0 ||
		!device->isOperational() ||
		m_impl->stateGeneration != m_impl->state->Generation() ||
		!IsIndexRangeValidForSubmission(resource, format, offset,
			startIndex, indexCount))
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	*validated = resource;
	return RENDER_RESULT_OK;
}

bool NativeW3DResources::Destroy(GpuHandle handle)
{
	if (!IsOwnerThread())
	{
		return false;
	}
	Slot *slot = Find(handle);
	IRenderDevice *device = m_impl == 0 || m_impl->state == 0 ? 0 :
		m_impl->state->Device();
	if (slot == 0 || HasTextureCleanupTicket(m_impl, handle) ||
		device == 0 || !device->isOperational() ||
		!m_impl->state->IsOperational() ||
		m_impl->stateGeneration != m_impl->state->Generation())
	{
		return false;
	}
	if (DestroyResourceAndWait(device, handle) != RENDER_RESULT_OK)
	{
		if (slot->kind == 1)
		{
			InvalidateBufferAuthority(*slot);
		}
		else
		{
			InvalidateTextureAuthority(*slot);
		}
		return false;
	}
	*slot = Slot();
	return true;
}

bool NativeW3DResources::DestroyTexture(NativeW3DTextureHandle handle)
{
	const Slot *slot = Find(handle.resource);
	return IsValid(handle) && slot != 0 && slot->kind == 2 ?
		Destroy(handle.resource) : false;
}

bool NativeW3DResources::RetireTexture(NativeW3DTextureHandle handle)
{
	if (!IsOwnerThread() ||
		HasTextureCleanupTicket(m_impl, handle.resource))
	{
		return false;
	}
	return RetireTextureImpl(m_impl, handle);
}

bool NativeW3DResources::RetireTextureImpl(Impl *impl,
	NativeW3DTextureHandle handle)
{
	Slot *slot = 0;
	if (impl != 0)
	{
		for (size_t index = 0; index < impl->slots.size(); ++index)
		{
			if (impl->slots[index].handle == handle.resource)
			{
				slot = &impl->slots[index];
				break;
			}
		}
	}
	IRenderDevice *device = impl == 0 || impl->state == 0 ? 0 :
		impl->state->Device();
	if (!handle.isValid() || slot == 0 || slot->kind != 2 || slot->retired ||
		device == 0 || !device->isOperational() ||
		!impl->state->IsOperational() ||
		handle.attachmentGeneration != impl->generation ||
		impl->stateGeneration != impl->state->Generation())
	{
		return false;
	}
	if (DestroyResourceAndWait(device, handle.resource) == RENDER_RESULT_OK)
	{
		*slot = Slot();
		return true;
	}

	// Ownership has transferred to this registry even though the backend
	// refused the immediate release.  Hide the exact retired slot from all
	// consumers and let Shutdown retry physical destruction.
	slot->retired = true;
	slot->authority = NATIVE_W3D_CONTENT_INVALID;
	++impl->nextAuthorityEpoch;
	if (impl->nextAuthorityEpoch == 0)
	{
		++impl->nextAuthorityEpoch;
	}
	slot->authorityEpoch = impl->nextAuthorityEpoch;
	return true;
}

bool NativeW3DResources::HasTextureCleanupTicket(const Impl *impl,
	GpuHandle handle)
{
	if (impl == 0 || !handle.isValid())
	{
		return false;
	}
	ScopedResourceTableLock lock(impl->cleanupLock);
	for (const NativeW3DTextureCleanupTicket *ticket =
		impl->cleanupTickets; ticket != 0; ticket = ticket->m_next)
	{
		if (ticket->m_handle.resource == handle)
		{
			return true;
		}
	}
	return false;
}

RenderResult NativeW3DResources::CreateTextureCleanupTicket(
	NativeW3DTextureHandle handle, NativeW3DTextureCleanupTicket **ticket)
{
	if (ticket == 0)
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	*ticket = 0;
	if (!IsOwnerThread() || !IsValid(handle))
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	NativeW3DTextureCleanupTicket *created =
		new (std::nothrow) NativeW3DTextureCleanupTicket(m_impl, handle);
	if (created == 0)
	{
		return RENDER_RESULT_OUT_OF_MEMORY;
	}
	AddImplReference(m_impl);
	{
		ScopedResourceTableLock lock(m_impl->cleanupLock);
		created->m_next = m_impl->cleanupTickets;
		m_impl->cleanupTickets = created;
	}
	*ticket = created;
	return RENDER_RESULT_OK;
}

RenderResult NativeW3DResources::ReleaseTextureCleanupTicket(
	NativeW3DTextureCleanupTicket *ticket)
{
	Impl *impl = ticket == 0 ? 0 : static_cast<Impl *>(ticket->m_table);
	if (impl == 0)
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	NativeW3DRenderState *state = 0;
	{
		ScopedResourceTableLock lock(impl->cleanupLock);
		state = impl->state;
		if (state != 0)
		{
			state->AddRef();
		}
	}
	if (state == 0)
	{
		// The table fallback already destroyed every exact backend slot. Only
		// this externally held logical token remains.
		ForgetTextureCleanupTicket(impl, ticket);
		return RENDER_RESULT_OK;
	}
	if (state->IsOwnerThread() &&
		RetireTextureImpl(impl, ticket->m_handle))
	{
		ForgetTextureCleanupTicket(impl, ticket);
		state->Release();
		return RENDER_RESULT_OK;
	}
	RenderResult result = state->EnqueueFallbackCleanup(
		DestroyTransferredTexture, ticket, ReleaseTransferredTexture,
		&ticket->m_fallback);
	if (result != RENDER_RESULT_OK && !state->IsAcceptingCleanup())
	{
		// A closed state with a bound table is the failed-recovery terminal path.
		// Its native allocations are already gone, so do not strand the last
		// externally held logical ticket after the owner object disappears.
		ForgetTextureCleanupTicket(impl, ticket);
		result = RENDER_RESULT_OK;
	}
	state->Release();
	return result;
}

void NativeW3DResources::ForgetTextureCleanupTicket(
	Impl *impl, NativeW3DTextureCleanupTicket *ticket)
{
	if (ticket == 0 || impl == 0)
	{
		return;
	}
	bool forgotten = false;
	{
		ScopedResourceTableLock lock(impl->cleanupLock);
		NativeW3DTextureCleanupTicket **link = &impl->cleanupTickets;
		while (*link != 0 && *link != ticket)
		{
			link = &(*link)->m_next;
		}
		if (*link == ticket)
		{
			*link = ticket->m_next;
			ticket->m_next = 0;
			ticket->m_table = 0;
			forgotten = true;
		}
	}
	if (forgotten)
	{
		delete ticket;
		ReleaseImplReference(impl);
	}
}

void NativeW3DResources::DestroyTransferredTexture(void *context)
{
	NativeW3DTextureCleanupTicket *ticket =
		static_cast<NativeW3DTextureCleanupTicket *>(context);
	Impl *impl = ticket == 0 ? 0 : static_cast<Impl *>(ticket->m_table);
	if (impl == 0)
	{
		throw 1;
	}
	NativeW3DRenderState *state = 0;
	unsigned int stateGeneration = 0;
	{
		ScopedResourceTableLock lock(impl->cleanupLock);
		state = impl->state;
		stateGeneration = impl->stateGeneration;
		if (state != 0)
		{
			state->AddRef();
		}
	}
	if (state == 0)
	{
		return;
	}
	IRenderDevice *device = state->Device();
	const bool terminallyDetached = device == 0 &&
		stateGeneration != state->Generation();
	const bool terminallyUnavailable = device != 0 &&
		!device->isOperational();
	if (terminallyDetached || terminallyUnavailable)
	{
		// Recovery has already discarded the backend allocation table. Clear only
		// the exact old typed slot; the queue's release callback unlinks its ticket.
		{
			ScopedResourceTableLock lock(impl->cleanupLock);
			if (ticket->m_handle.attachmentGeneration == impl->generation)
			{
				for (size_t index = 0; index < impl->slots.size(); ++index)
				{
					if (impl->slots[index].handle == ticket->m_handle.resource)
					{
						impl->slots[index] = Slot();
						break;
					}
				}
			}
		}
		state->Release();
		return;
	}
	const bool retired = RetireTextureImpl(impl, ticket->m_handle);
	state->Release();
	if (!retired)
	{
		throw 1;
	}
}

void NativeW3DResources::ReleaseTransferredTexture(void *context)
{
	NativeW3DTextureCleanupTicket *ticket =
		static_cast<NativeW3DTextureCleanupTicket *>(context);
	Impl *impl = ticket == 0 ? 0 : static_cast<Impl *>(ticket->m_table);
	if (impl != 0)
	{
		ForgetTextureCleanupTicket(impl, ticket);
	}
}

bool NativeW3DResources::IsOwnerThread() const
{
	return m_impl != 0 && m_impl->state != 0 &&
		m_impl->state->IsOwnerThread();
}

bool NativeW3DResources::IsValid(GpuHandle handle) const
{
	IRenderDevice *device = m_impl == 0 || m_impl->state == 0 ? 0 :
		m_impl->state->Device();
	const Slot *slot = Find(handle);
	return m_impl != 0 && m_impl->state != 0 &&
		m_impl->state->IsOperational() && device != 0 &&
		device->isOperational() &&
		m_impl->stateGeneration == m_impl->state->Generation() &&
		slot != 0 && !slot->retired;
}

bool NativeW3DResources::IsValid(NativeW3DTextureHandle handle) const
{
	const Slot *slot = Find(handle.resource);
	return handle.isValid() && m_impl != 0 && m_impl->state != 0 &&
		handle.attachmentGeneration == m_impl->generation &&
		IsValid(handle.resource) && slot != 0 && slot->kind == 2;
}

bool NativeW3DResources::IsValid(NativeW3DSurfaceHandle handle) const
{
	if (!handle.isValid())
	{
		return false;
	}
	NativeW3DSurfaceHandle validated = handle;
	return AcquireTextureSurface(handle.texture, handle.mipLevel,
		handle.arraySlice, &validated) == RENDER_RESULT_OK;
}

NativeW3DResources::Slot *NativeW3DResources::Find(GpuHandle handle)
{
	if (m_impl == 0 || !handle.isValid())
	{
		return 0;
	}
	for (size_t index = 0; index < m_impl->slots.size(); ++index)
	{
		if (m_impl->slots[index].handle == handle)
		{
			return &m_impl->slots[index];
		}
	}
	return 0;
}

const NativeW3DResources::Slot *NativeW3DResources::Find(
	GpuHandle handle) const
{
	if (m_impl == 0 || !handle.isValid())
	{
		return 0;
	}
	for (size_t index = 0; index < m_impl->slots.size(); ++index)
	{
		if (m_impl->slots[index].handle == handle)
		{
			return &m_impl->slots[index];
		}
	}
	return 0;
}

NativeW3DContentAuthority NativeW3DResources::EffectiveAuthority(
	const Slot &slot) const
{
	if (slot.retired)
	{
		return NATIVE_W3D_CONTENT_INVALID;
	}
	if (slot.kind == 1 && (m_impl == 0 || m_impl->state == 0 ||
		slot.backendEpoch != m_impl->state->BufferEpoch()))
	{
		return NATIVE_W3D_CONTENT_INVALID;
	}
	if (slot.authority == NATIVE_W3D_CONTENT_GPU_RENDER_TARGET &&
		(m_impl == 0 || m_impl->state == 0 ||
		slot.backendEpoch != m_impl->state->BackendEpoch()))
	{
		return NATIVE_W3D_CONTENT_INVALID;
	}
	return slot.authority;
}

RenderResult NativeW3DResources::CompleteMutation(IRenderDevice *device,
	RenderResult result)
{
	result = WaitForMutation(device, result);
	if (result != RENDER_RESULT_OK)
	{
		InvalidateAllAuthorities();
	}
	return result;
}

RenderResult NativeW3DResources::CompleteBufferMutation(IRenderDevice *device,
	RenderResult result, Slot *affected)
{
	result = WaitForMutation(device, result);
	if (result != RENDER_RESULT_OK && affected != 0)
	{
		InvalidateBufferAuthority(*affected);
	}
	return result;
}

void NativeW3DResources::InvalidateBufferAuthority(Slot &slot)
{
	if (slot.kind != 1 || !slot.handle.isValid())
	{
		return;
	}
	slot.authority = NATIVE_W3D_CONTENT_INVALID;
	slot.initializedBytes.clear();
	slot.submissionInitializedBytes.clear();
	slot.pendingBufferPublications.clear();
	slot.submissionAuthority = NATIVE_W3D_CONTENT_INVALID;
	slot.backendEpoch = 0;
	slot.submissionBackendEpoch = 0;
	if (slot.buffer.usage != RENDER_USAGE_DEFAULT)
	{
		slot.recoveryInitializedBytes.clear();
		slot.recoveryBytes.clear();
	}
	slot.authorityEpoch = NextAuthorityEpoch();
}

void NativeW3DResources::InvalidateTextureAuthority(Slot &slot)
{
	if (slot.kind != 2 || !slot.handle.isValid())
	{
		return;
	}
	slot.authority = NATIVE_W3D_CONTENT_INVALID;
	slot.authorityEpoch = NextAuthorityEpoch();
}

void NativeW3DResources::InvalidateGpuAuthorities()
{
	if (m_impl == 0)
	{
		return;
	}
	for (size_t index = 0; index < m_impl->slots.size(); ++index)
	{
		Slot &slot = m_impl->slots[index];
		if (slot.handle.isValid() && slot.kind == 2 &&
			slot.authority == NATIVE_W3D_CONTENT_GPU_RENDER_TARGET)
		{
			slot.authority = NATIVE_W3D_CONTENT_INVALID;
			slot.authorityEpoch = NextAuthorityEpoch();
		}
	}
}

void NativeW3DResources::InvalidateAllAuthorities()
{
	if (m_impl == 0)
	{
		return;
	}
	bool invalidateBufferEpoch = false;
	for (size_t index = 0; index < m_impl->slots.size(); ++index)
	{
		Slot &slot = m_impl->slots[index];
		if (!slot.handle.isValid())
		{
			continue;
		}
		if (slot.kind == 1)
		{
			// Keep the CPU-owned recovery image and exact initialized ranges.
			// Advancing the buffer epoch below hides them until recovery has
			// synchronously republished every recoverable static range.
			slot.pendingBufferPublications.clear();
			slot.submissionInitializedBytes.clear();
			slot.submissionAuthority = NATIVE_W3D_CONTENT_INVALID;
			slot.submissionBackendEpoch = 0;
			if (slot.buffer.usage == RENDER_USAGE_DYNAMIC)
			{
				slot.initializedBytes.clear();
				slot.authority = NATIVE_W3D_CONTENT_INVALID;
				slot.authorityEpoch = NextAuthorityEpoch();
			}
			invalidateBufferEpoch = true;
		}
		else
		{
			InvalidateTextureAuthority(slot);
		}
	}
	if (invalidateBufferEpoch && m_impl->state != 0)
	{
		m_impl->state->AdvanceBufferEpoch();
	}
}

bool NativeW3DResources::IsBufferRangeInitialized(const Slot &slot,
	size_t offset, size_t byteCount) const
{
	if (m_impl == 0 || m_impl->state == 0 ||
		slot.backendEpoch != m_impl->state->BufferEpoch() ||
		byteCount == 0 || offset > slot.buffer.byteCount ||
		byteCount > slot.buffer.byteCount - offset)
	{
		return false;
	}
	return ContainsInitializedByteRange(slot.initializedBytes, offset,
		byteCount);
}

bool NativeW3DResources::IsBufferRangeInitializedForSubmission(
	const Slot &slot, size_t offset, size_t byteCount) const
{
	if (m_impl == 0 || m_impl->state == 0 ||
		slot.submissionBackendEpoch != m_impl->state->BufferEpoch() ||
		byteCount == 0 || offset > slot.buffer.byteCount ||
		byteCount > slot.buffer.byteCount - offset)
	{
		return false;
	}
	return ContainsInitializedByteRange(slot.submissionInitializedBytes,
		offset, byteCount);
}

unsigned int NativeW3DResources::NextAuthorityEpoch()
{
	++m_impl->nextAuthorityEpoch;
	if (m_impl->nextAuthorityEpoch == 0)
	{
		++m_impl->nextAuthorityEpoch;
	}
	return m_impl->nextAuthorityEpoch;
}

void NativeW3DResources::PopulateLease(const Slot &slot,
	NativeW3DGpuContentLease *lease) const
{
	lease->resource = slot.handle;
	lease->attachmentGeneration = m_impl->generation;
	lease->backendEpoch = slot.backendEpoch;
	lease->authorityEpoch = slot.authorityEpoch;
}

bool NativeW3DResources::IsVertexRangeValid(GpuHandle handle,
	unsigned int stride, unsigned int offset, unsigned int startVertex,
	unsigned int vertexCount) const
{
	const Slot *slot = Find(handle);
	if (slot == 0 || slot->kind != 1 ||
		(slot->buffer.binding & RENDER_BUFFER_VERTEX) == 0 || stride == 0 ||
		vertexCount == 0)
	{
		return false;
	}
	const size_t entries = static_cast<size_t>(startVertex) + vertexCount;
	if (slot->buffer.stride != stride || entries < startVertex ||
		entries > (static_cast<size_t>(-1) - offset) / stride)
	{
		return false;
	}
	const size_t rangeBegin = static_cast<size_t>(offset) +
		static_cast<size_t>(startVertex) * stride;
	const size_t rangeBytes = static_cast<size_t>(vertexCount) * stride;
	return
		static_cast<size_t>(offset) + entries * stride <= slot->buffer.byteCount &&
		IsBufferRangeInitialized(*slot, rangeBegin, rangeBytes);
}

bool NativeW3DResources::IsIndexRangeValid(GpuHandle handle,
	RenderFormat format, unsigned int offset, unsigned int startIndex,
	unsigned int indexCount) const
{
	const unsigned int indexSize = format == RENDER_FORMAT_R16_UINT ? 2U :
		(format == RENDER_FORMAT_R32_UINT ? 4U : 0U);
	const Slot *slot = Find(handle);
	if (slot == 0 || slot->kind != 1 ||
		(slot->buffer.binding & RENDER_BUFFER_INDEX) == 0 || indexSize == 0 ||
		indexCount == 0)
	{
		return false;
	}
	const size_t entries = static_cast<size_t>(startIndex) + indexCount;
	if (entries < startIndex ||
		entries > (static_cast<size_t>(-1) - offset) / indexSize)
	{
		return false;
	}
	const size_t rangeBegin = static_cast<size_t>(offset) +
		static_cast<size_t>(startIndex) * indexSize;
	const size_t rangeBytes = static_cast<size_t>(indexCount) * indexSize;
	return static_cast<size_t>(offset) + entries * indexSize <=
		slot->buffer.byteCount &&
		IsBufferRangeInitialized(*slot, rangeBegin, rangeBytes);
}

bool NativeW3DResources::IsVertexRangeValidForSubmission(GpuHandle handle,
	unsigned int stride, unsigned int offset, unsigned int startVertex,
	unsigned int vertexCount) const
{
	const Slot *slot = Find(handle);
	if (slot == 0 || slot->kind != 1 ||
		(slot->buffer.binding & RENDER_BUFFER_VERTEX) == 0 || stride == 0 ||
		vertexCount == 0)
	{
		return false;
	}
	const size_t entries = static_cast<size_t>(startVertex) + vertexCount;
	if (slot->buffer.stride != stride || entries < startVertex ||
		entries > (static_cast<size_t>(-1) - offset) / stride)
	{
		return false;
	}
	const size_t rangeBegin = static_cast<size_t>(offset) +
		static_cast<size_t>(startVertex) * stride;
	const size_t rangeBytes = static_cast<size_t>(vertexCount) * stride;
	return static_cast<size_t>(offset) + entries * stride <=
		slot->buffer.byteCount &&
		IsBufferRangeInitializedForSubmission(*slot, rangeBegin, rangeBytes);
}

bool NativeW3DResources::IsIndexRangeValidForSubmission(GpuHandle handle,
	RenderFormat format, unsigned int offset, unsigned int startIndex,
	unsigned int indexCount) const
{
	const unsigned int indexSize = format == RENDER_FORMAT_R16_UINT ? 2U :
		(format == RENDER_FORMAT_R32_UINT ? 4U : 0U);
	const Slot *slot = Find(handle);
	if (slot == 0 || slot->kind != 1 ||
		(slot->buffer.binding & RENDER_BUFFER_INDEX) == 0 || indexSize == 0 ||
		indexCount == 0)
	{
		return false;
	}
	const size_t entries = static_cast<size_t>(startIndex) + indexCount;
	if (entries < startIndex ||
		entries > (static_cast<size_t>(-1) - offset) / indexSize)
	{
		return false;
	}
	const size_t rangeBegin = static_cast<size_t>(offset) +
		static_cast<size_t>(startIndex) * indexSize;
	const size_t rangeBytes = static_cast<size_t>(indexCount) * indexSize;
	return static_cast<size_t>(offset) + entries * indexSize <=
		slot->buffer.byteCount &&
		IsBufferRangeInitializedForSubmission(*slot, rangeBegin, rangeBytes);
}

bool NativeW3DResources::IsTextureValidOrEmpty(GpuHandle handle) const
{
	if (!handle.isValid())
	{
		return true;
	}
	const Slot *slot = Find(handle);
	return slot != 0 && slot->kind == 2 && !slot->retired &&
		EffectiveAuthority(*slot) != NATIVE_W3D_CONTENT_INVALID;
}

void NativeW3DResources::DestroyDeferredResourceTable(void *context)
{
	Impl *impl = static_cast<Impl *>(context);
	NativeW3DRenderState *state = 0;
	if (impl != 0)
	{
		ScopedResourceTableLock lock(impl->cleanupLock);
		state = impl->state;
	}
	if (impl == 0)
	{
		throw 1;
	}
	if (state == 0)
	{
		return;
	}
	IRenderDevice *device = state->Device();
	const bool terminallyDetached = device == 0 &&
		impl->stateGeneration != state->Generation();
	const bool terminallyUnavailable = device != 0 &&
		!device->isOperational();
	if (!terminallyDetached && !terminallyUnavailable &&
		(device == 0 || impl->stateGeneration != state->Generation()))
	{
		throw 1;
	}
	for (size_t index = 0; index < impl->slots.size(); ++index)
	{
		Slot &slot = impl->slots[index];
		if (!slot.handle.isValid())
		{
			continue;
		}
		if (!terminallyDetached && !terminallyUnavailable &&
			DestroyResourceAndWait(device, slot.handle) != RENDER_RESULT_OK)
		{
			throw 1;
		}
		slot = Slot();
	}
	{
		ScopedResourceTableLock lock(impl->cleanupLock);
		if (impl->state != state)
		{
			throw 1;
		}
		impl->state = 0;
		impl->stateGeneration = 0;
	}
	state->UnregisterResourceTable();
	state->Release();
}

void NativeW3DResources::ReleaseDeferredResourceTable(void *context)
{
	ReleaseImplReference(static_cast<Impl *>(context));
}

bool NativeW3DResources::IsBoundTo(const NativeW3DRenderer *renderer) const
{
	IRenderDevice *device = m_impl == 0 || m_impl->state == 0 ? 0 :
		m_impl->state->Device();
	return m_impl != 0 && m_impl->state != 0 && renderer != 0 &&
		m_impl->state == renderer->m_state && m_impl->state->IsOperational() &&
		device != 0 && device->isOperational() &&
		m_impl->stateGeneration == m_impl->state->Generation();
}
}
}
