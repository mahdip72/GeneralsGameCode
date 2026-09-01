#include "Renderer/NativeW3DResources.h"
#include "Renderer/NativeW3DRenderState.h"
#if defined(RTS_RENDERER_HAS_D3D11)
#include "Renderer/ThreadedRenderDevice.h"
#endif

#include <assert.h>
#include <limits.h>
#include <new>
#include <vector>

namespace rts
{
namespace render
{
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

struct NativeW3DResources::Slot
{
	Slot() : kind(0), authority(NATIVE_W3D_CONTENT_INVALID),
		authorityEpoch(0), backendEpoch(0) {}
	GpuHandle handle;
	unsigned int kind;
	BufferDescriptor buffer;
	TextureDescriptor texture;
	NativeW3DContentAuthority authority;
	unsigned int authorityEpoch;
	unsigned int backendEpoch;
	std::vector<InitializedByteRange> initializedBytes;
};

struct NativeW3DResources::Impl
{
	Impl(unsigned int capacity) : state(0), generation(0),
		nextAuthorityEpoch(0), lastThreadedCompletionSequence(0), slots(capacity) {}
	NativeW3DRenderState *state;
	unsigned int generation;
	unsigned int nextAuthorityEpoch;
	NativeW3DSubmissionSequence lastThreadedCompletionSequence;
	std::vector<Slot> slots;
	NativeW3DOwnerFallbackEntry fallbackCleanup;
};

namespace
{
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
		bool hasHandles = false;
		for (size_t index = 0; index < m_impl->slots.size(); ++index)
		{
			if (m_impl->slots[index].handle.isValid())
			{
				hasHandles = true;
				break;
			}
		}
		if (hasHandles)
		{
			const RenderResult accepted =
				m_impl->state->EnqueueFallbackCleanup(
					DestroyDeferredResourceTable, m_impl,
					ReleaseDeferredResourceTable,
					&m_impl->fallbackCleanup);
			// The embedded fallback path allocates nothing and is bounded by the
			// registered table count.  If lifecycle misuse has already closed it,
			// retain the table/state intentionally so Detach stays fail-closed.
			if (accepted == RENDER_RESULT_OK)
			{
				m_impl = 0;
				return;
			}
			m_impl = 0;
			return;
		}
		m_impl->state->UnregisterResourceTable();
		m_impl->state->Release();
		m_impl->state = 0;
		m_impl->generation = 0;
	}
	delete m_impl;
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
	IRenderDevice *device = state == 0 ? 0 : state->Device();
	if (m_impl->state != 0 || state == 0 || !state->IsOperational() ||
		!state->IsOwnerThread() || device == 0 || !device->isOperational())
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	const unsigned int generation = state->Generation();
	const unsigned int backendEpoch = state->BackendEpoch();
	if (generation == 0 || backendEpoch == 0)
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	const RenderResult registerResult = state->RegisterResourceTable();
	if (registerResult != RENDER_RESULT_OK)
	{
		return registerResult;
	}
	m_impl->state = state;
	m_impl->state->AddRef();
	m_impl->generation = generation;
	m_impl->lastThreadedCompletionSequence = 0;
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
		m_impl->generation == m_impl->state->Generation();
	const bool terminallyDetached = device == 0 &&
		m_impl->generation != m_impl->state->Generation();
	for (size_t index = 0; index < m_impl->slots.size(); ++index)
	{
		Slot &slot = m_impl->slots[index];
		if (slot.handle.isValid() && !canDestroy && !terminallyDetached)
		{
			return RENDER_RESULT_FAILED;
		}
		if (slot.handle.isValid() && terminallyDetached)
		{
			// The owned backend has already completed terminal teardown and
			// destroyed its native allocation table. Only local metadata remains.
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
	m_impl->state->UnregisterResourceTable();
	m_impl->state->Release();
	m_impl->state = 0;
	m_impl->generation = 0;
	m_impl->lastThreadedCompletionSequence = 0;
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
	m_impl->lastThreadedCompletionSequence = submissionSequence;
	if (resourceFailure)
	{
		InvalidateAllAuthorities();
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
		m_impl->generation != m_impl->state->Generation())
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
	if (initialData != 0 && initialDataBytes == descriptor.byteCount)
	{
		try
		{
			initializedBytes.push_back(InitializedByteRange(0,
				descriptor.byteCount));
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
				slot.backendEpoch = m_impl->state->BackendEpoch();
			}
			return RENDER_RESULT_OUT_OF_MEMORY;
		}
	}
	const RenderResult completionResult = CompleteMutation(device, result);
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
		slot.backendEpoch = m_impl->state->BackendEpoch();
		slot.initializedBytes.swap(initializedBytes);
		return completionResult;
	}
	slot.handle = created;
	slot.kind = 1;
	slot.buffer = descriptor;
	slot.authority = initialData != 0 &&
		initialDataBytes == descriptor.byteCount ? NATIVE_W3D_CONTENT_CPU :
		NATIVE_W3D_CONTENT_INVALID;
	slot.authorityEpoch = NextAuthorityEpoch();
	slot.backendEpoch = m_impl->state->BackendEpoch();
	slot.initializedBytes.swap(initializedBytes);
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
	IRenderDevice *device = m_impl->state == 0 ? 0 : m_impl->state->Device();
	if (device == 0 || !device->isOperational() ||
		!m_impl->state->IsOperational() ||
		m_impl->generation != m_impl->state->Generation())
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
		m_impl->generation != m_impl->state->Generation())
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	const RenderResult result = CompleteMutation(device,
		device->updateBufferResource(handle, bytes, byteCount,
			destinationOffset, mode));
	if (result == RENDER_RESULT_OK)
	{
		const bool hadWholeBufferCpuAuthority =
			slot->authority == NATIVE_W3D_CONTENT_CPU;
		try
		{
			RecordBufferWrite(*slot, destinationOffset, byteCount, mode);
		}
		catch (...)
		{
			slot->initializedBytes.clear();
			slot->authority = NATIVE_W3D_CONTENT_INVALID;
			slot->authorityEpoch = NextAuthorityEpoch();
			return RENDER_RESULT_OUT_OF_MEMORY;
		}
		const bool fullWrite = destinationOffset == 0 &&
			byteCount == slot->buffer.byteCount;
		slot->authority = fullWrite || (mode != RENDER_BUFFER_UPDATE_DISCARD &&
			hadWholeBufferCpuAuthority) ? NATIVE_W3D_CONTENT_CPU :
			NATIVE_W3D_CONTENT_INVALID;
		slot->authorityEpoch = NextAuthorityEpoch();
		slot->backendEpoch = m_impl->state->BackendEpoch();
	}
	return result;
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
		slot->kind != 2 || subresources == 0 || descriptor.mipCount == 0 ||
		descriptor.arrayCount == 0 ||
		descriptor.mipCount > UINT_MAX / descriptor.arrayCount ||
		subresourceCount != descriptor.mipCount * descriptor.arrayCount ||
		m_impl->generation != m_impl->state->Generation())
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	if (!EqualTextureDescriptors(slot->texture, descriptor) ||
		descriptor.usage == RENDER_USAGE_IMMUTABLE)
	{
		return RENDER_RESULT_UNSUPPORTED;
	}
	for (unsigned int index = 0; index < subresourceCount; ++index)
	{
		if (subresources[index].data == 0 ||
			subresources[index].rowPitch == 0)
		{
			return RENDER_RESULT_INVALID_ARGUMENT;
		}
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
		m_impl->generation != m_impl->state->Generation())
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
	if (slot == 0 || slot->kind != 2 || device == 0 ||
		!device->isOperational() ||
		m_impl->generation != m_impl->state->Generation())
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
		slot->kind != 2 || m_impl->generation != m_impl->state->Generation())
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
	if (slot == 0 || slot->kind != 2 || device == 0 ||
		!device->isOperational() ||
		m_impl->generation != m_impl->state->Generation() ||
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
		m_impl->generation != m_impl->state->Generation() ||
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
		m_impl->generation != m_impl->state->Generation() ||
		!IsIndexRangeValid(resource, format, offset, startIndex, indexCount))
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	*validated = resource;
	return RENDER_RESULT_OK;
}

bool NativeW3DResources::Destroy(GpuHandle handle)
{
	Slot *slot = Find(handle);
	IRenderDevice *device = m_impl == 0 || m_impl->state == 0 ? 0 :
		m_impl->state->Device();
	if (slot == 0 || device == 0 || !device->isOperational() ||
		!m_impl->state->IsOperational() ||
		m_impl->generation != m_impl->state->Generation())
	{
		return false;
	}
	if (DestroyResourceAndWait(device, handle) != RENDER_RESULT_OK)
	{
		InvalidateAllAuthorities();
		return false;
	}
	*slot = Slot();
	return true;
}

bool NativeW3DResources::IsValid(GpuHandle handle) const
{
	IRenderDevice *device = m_impl == 0 || m_impl->state == 0 ? 0 :
		m_impl->state->Device();
	return m_impl != 0 && m_impl->state != 0 &&
		m_impl->state->IsOperational() && device != 0 &&
		device->isOperational() &&
		m_impl->generation == m_impl->state->Generation() &&
		Find(handle) != 0;
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

void NativeW3DResources::InvalidateAllAuthorities()
{
	if (m_impl == 0)
	{
		return;
	}
	for (size_t index = 0; index < m_impl->slots.size(); ++index)
	{
		Slot &slot = m_impl->slots[index];
		if (!slot.handle.isValid())
		{
			continue;
		}
		slot.authority = NATIVE_W3D_CONTENT_INVALID;
		slot.initializedBytes.clear();
		slot.authorityEpoch = NextAuthorityEpoch();
	}
}

void NativeW3DResources::RecordBufferWrite(Slot &slot,
	size_t destinationOffset, size_t byteCount, RenderBufferUpdateMode mode)
{
	if (mode == RENDER_BUFFER_UPDATE_DISCARD)
	{
		slot.initializedBytes.clear();
	}
	const InitializedByteRange added(destinationOffset,
		destinationOffset + byteCount);
	std::vector<InitializedByteRange> merged;
	merged.reserve(slot.initializedBytes.size() + 1);
	InitializedByteRange combined = added;
	bool inserted = false;
	for (size_t index = 0; index < slot.initializedBytes.size(); ++index)
	{
		const InitializedByteRange &existing = slot.initializedBytes[index];
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
	slot.initializedBytes.swap(merged);
}

bool NativeW3DResources::IsBufferRangeInitialized(const Slot &slot,
	size_t offset, size_t byteCount) const
{
	if (byteCount == 0 || offset > slot.buffer.byteCount ||
		byteCount > slot.buffer.byteCount - offset)
	{
		return false;
	}
	const size_t end = offset + byteCount;
	for (size_t index = 0; index < slot.initializedBytes.size(); ++index)
	{
		const InitializedByteRange &range = slot.initializedBytes[index];
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

bool NativeW3DResources::IsTextureValidOrEmpty(GpuHandle handle) const
{
	if (!handle.isValid())
	{
		return true;
	}
	const Slot *slot = Find(handle);
	return slot != 0 && slot->kind == 2 &&
		EffectiveAuthority(*slot) != NATIVE_W3D_CONTENT_INVALID;
}

void NativeW3DResources::DestroyDeferredResourceTable(void *context)
{
	Impl *impl = static_cast<Impl *>(context);
	if (impl == 0 || impl->state == 0)
	{
		throw 1;
	}
	IRenderDevice *device = impl->state->Device();
	if (device == 0 || !device->isOperational() ||
		impl->generation != impl->state->Generation())
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
		if (DestroyResourceAndWait(device, slot.handle) != RENDER_RESULT_OK)
		{
			throw 1;
		}
		slot = Slot();
	}
	impl->state->UnregisterResourceTable();
	impl->state->Release();
	impl->state = 0;
	impl->generation = 0;
}

void NativeW3DResources::ReleaseDeferredResourceTable(void *context)
{
	delete static_cast<Impl *>(context);
}

bool NativeW3DResources::IsBoundTo(const NativeW3DRenderer *renderer) const
{
	IRenderDevice *device = m_impl == 0 || m_impl->state == 0 ? 0 :
		m_impl->state->Device();
	return m_impl != 0 && m_impl->state != 0 && renderer != 0 &&
		m_impl->state == renderer->m_state && m_impl->state->IsOperational() &&
		device != 0 && device->isOperational() &&
		m_impl->generation == m_impl->state->Generation();
}
}
}
