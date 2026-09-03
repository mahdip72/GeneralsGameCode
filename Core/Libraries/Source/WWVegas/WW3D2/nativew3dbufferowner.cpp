#include "nativew3dbufferowner.h"

#include <cstring>
#include <limits.h>
#include <new>

namespace rts
{
namespace render
{
namespace
{
NativeW3DResources *g_nativeW3DBufferResources = 0;
unsigned int g_nativeW3DBufferBindingGeneration = 0;
bool g_nativeW3DBufferBindingExhausted = false;

bool IsSupportedUpdateMode(RenderBufferUpdateMode mode)
{
	return mode == RENDER_BUFFER_UPDATE_PRESERVE ||
		mode == RENDER_BUFFER_UPDATE_DISCARD ||
		mode == RENDER_BUFFER_UPDATE_NO_OVERWRITE;
}

}

RenderResult BindNativeW3DBufferResources(NativeW3DResources *resources)
{
	if (resources == 0 || !resources->IsOwnerThread() ||
		(g_nativeW3DBufferResources != 0 &&
		 g_nativeW3DBufferResources != resources))
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	if (g_nativeW3DBufferResources == resources)
	{
		return RENDER_RESULT_OK;
	}
	if (g_nativeW3DBufferBindingExhausted ||
		g_nativeW3DBufferBindingGeneration == UINT_MAX)
	{
		g_nativeW3DBufferBindingExhausted = true;
		return RENDER_RESULT_FAILED;
	}
	++g_nativeW3DBufferBindingGeneration;
	g_nativeW3DBufferResources = resources;
	return RENDER_RESULT_OK;
}

RenderResult UnbindNativeW3DBufferResources(NativeW3DResources *resources)
{
	if (g_nativeW3DBufferResources == 0)
	{
		return RENDER_RESULT_OK;
	}
	if (resources == 0 || g_nativeW3DBufferResources != resources ||
		!resources->IsOwnerThread())
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	g_nativeW3DBufferResources = 0;
	return RENDER_RESULT_OK;
}

bool IsNativeW3DBufferOwnerThread()
{
	return g_nativeW3DBufferResources != 0 &&
		g_nativeW3DBufferResources->IsOwnerThread();
}

NativeW3DBufferOwner::NativeW3DBufferOwner() :
	m_resources(0), m_bindingGeneration(0), m_descriptor(), m_handle(),
	m_cleanupTicket(0), m_deferredHandle(), m_deferredCleanupTicket(0),
	m_authoritative(0), m_authoritativeBytes(0), m_staging(0),
	m_lockOffset(0), m_lockBytes(0),
	m_lockMode(RENDER_BUFFER_UPDATE_PRESERVE), m_locked(false),
	m_failedMutation(false)
{
}

NativeW3DBufferOwner::~NativeW3DBufferOwner()
{
	// Reset transfers foreign-thread destruction to the resource table's
	// allocation-free fallback ticket.  A failed transfer deliberately leaves
	// the ticket linked, keeping the table alive and making shutdown fail closed;
	// this destructor must never allocate or discard a live backend handle.
	(void)Reset();
	ReleaseStaging();
	delete[] m_authoritative;
	m_authoritative = 0;
	m_authoritativeBytes = 0;
}

RenderResult NativeW3DBufferOwner::Create(
	const BufferDescriptor &descriptor)
{
	if (m_resources != 0 || m_handle.isValid() || m_locked ||
		descriptor.byteCount == 0 || descriptor.stride == 0 ||
		(descriptor.binding != RENDER_BUFFER_VERTEX &&
		 descriptor.binding != RENDER_BUFFER_INDEX) ||
		(descriptor.usage != RENDER_USAGE_DEFAULT &&
		 descriptor.usage != RENDER_USAGE_DYNAMIC) ||
		g_nativeW3DBufferResources == 0)
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}

	m_resources = g_nativeW3DBufferResources;
	m_bindingGeneration = g_nativeW3DBufferBindingGeneration;
	m_descriptor = descriptor;
	unsigned char *authoritative =
		new(std::nothrow) unsigned char[descriptor.byteCount];
	if (authoritative == 0)
	{
		m_resources = 0;
		m_bindingGeneration = 0;
		m_descriptor = BufferDescriptor();
		m_failedMutation = true;
		return RENDER_RESULT_OUT_OF_MEMORY;
	}
	memset(authoritative, 0, descriptor.byteCount);
	GpuHandle created;
	// Establish a deterministic backend image before any partial PRESERVE lock.
	// A dynamic D3D11 buffer created with null initial data has undefined bytes;
	// the owner shadow alone cannot make untouched GPU bytes recoverable.  Seed
	// the native allocation from the same zeroed authoritative image.
	const RenderResult result = m_resources->CreateBuffer(m_descriptor,
		authoritative, descriptor.byteCount, &created);
	if (result != RENDER_RESULT_OK || !created.isValid())
	{
		delete[] authoritative;
		m_resources = 0;
		m_bindingGeneration = 0;
		m_descriptor = BufferDescriptor();
		m_failedMutation = true;
		return result == RENDER_RESULT_OK ? RENDER_RESULT_FAILED : result;
	}
	NativeW3DBufferCleanupTicket *cleanupTicket = 0;
	const RenderResult ticketResult =
		m_resources->CreateBufferCleanupTicket(created, &cleanupTicket);
	if (ticketResult != RENDER_RESULT_OK || cleanupTicket == 0)
	{
		if (!m_resources->Destroy(created))
		{
			(void)m_resources->RetireBuffer(created);
		}
		delete[] authoritative;
		m_resources = 0;
		m_bindingGeneration = 0;
		m_descriptor = BufferDescriptor();
		m_failedMutation = true;
		return ticketResult == RENDER_RESULT_OK ? RENDER_RESULT_FAILED :
			ticketResult;
	}
	m_handle = created;
	m_cleanupTicket = cleanupTicket;
	m_authoritative = authoritative;
	m_authoritativeBytes = descriptor.byteCount;
	m_failedMutation = false;
	return RENDER_RESULT_OK;
}

RenderResult NativeW3DBufferOwner::Reset()
{
	ReleaseStaging();
	RenderResult result = RENDER_RESULT_OK;
	if (m_deferredCleanupTicket != 0)
	{
		const RenderResult releaseResult =
			NativeW3DResources::ReleaseBufferCleanupTicket(
				m_deferredCleanupTicket);
		if (releaseResult == RENDER_RESULT_OK)
		{
			m_deferredCleanupTicket = 0;
			m_deferredHandle = GpuHandle();
		}
		else
		{
			result = releaseResult;
		}
	}
	if (m_cleanupTicket != 0)
	{
		const RenderResult releaseResult =
			NativeW3DResources::ReleaseBufferCleanupTicket(m_cleanupTicket);
		if (releaseResult == RENDER_RESULT_OK)
		{
			m_cleanupTicket = 0;
			m_handle = GpuHandle();
		}
		else
		{
			result = releaseResult;
		}
	}
	if (result != RENDER_RESULT_OK)
	{
		// A closed operational queue keeps its intrusive ticket linked.  The
		// table reference prevents resource destruction until the owner can drain
		// or explicitly resolve that ticket.
		m_failedMutation = true;
		return result;
	}
	m_deferredHandle = GpuHandle();
	m_handle = GpuHandle();
	m_deferredCleanupTicket = 0;
	m_cleanupTicket = 0;
	m_resources = 0;
	m_bindingGeneration = 0;
	m_descriptor = BufferDescriptor();
	delete[] m_authoritative;
	m_authoritative = 0;
	m_authoritativeBytes = 0;
	m_failedMutation = false;
	return result;
}

RenderResult NativeW3DBufferOwner::RecreateForDiscard()
{
	NativeW3DResources *resources = ActiveResources();
	if (resources == 0 || !m_handle.isValid() || m_cleanupTicket == 0 ||
		!resources->IsValid(m_handle))
	{
		return RENDER_RESULT_FAILED;
	}
	if (m_deferredHandle.isValid())
	{
		if (m_deferredCleanupTicket == 0)
		{
			return RENDER_RESULT_FAILED;
		}
		const RenderResult deferredResult =
			NativeW3DResources::ReleaseBufferCleanupTicket(
				m_deferredCleanupTicket);
		if (deferredResult != RENDER_RESULT_OK)
		{
			return deferredResult;
		}
		m_deferredCleanupTicket = 0;
		m_deferredHandle = GpuHandle();
	}
	const GpuHandle previous = m_handle;
	unsigned char *zeroData = new(std::nothrow) unsigned char[
		m_descriptor.byteCount];
	if (zeroData == 0)
	{
		return RENDER_RESULT_OUT_OF_MEMORY;
	}
	memset(zeroData, 0, m_descriptor.byteCount);
	GpuHandle replacement;
	const RenderResult result = resources->CreateBuffer(m_descriptor, zeroData,
		m_descriptor.byteCount, &replacement);
	delete[] zeroData;
	if (result != RENDER_RESULT_OK || !replacement.isValid())
	{
		return result == RENDER_RESULT_OK ? RENDER_RESULT_FAILED : result;
	}
	NativeW3DBufferCleanupTicket *replacementTicket = 0;
	const RenderResult ticketResult = resources->CreateBufferCleanupTicket(
		replacement, &replacementTicket);
	if (ticketResult != RENDER_RESULT_OK || replacementTicket == 0)
	{
		if (!resources->Destroy(replacement))
		{
			(void)resources->RetireBuffer(replacement);
		}
		return ticketResult == RENDER_RESULT_OK ? RENDER_RESULT_FAILED :
			ticketResult;
	}
	const RenderResult previousResult =
		NativeW3DResources::ReleaseBufferCleanupTicket(m_cleanupTicket);
	if (previousResult != RENDER_RESULT_OK)
	{
		// Keep the replacement ticket owner-reachable when a closed queue or
		// generation mismatch prevents retiring the previous handle.  The next
		// DISCARD first releases this replacement before trying again.
		m_deferredHandle = replacement;
		m_deferredCleanupTicket = replacementTicket;
		return previousResult;
	}
	m_cleanupTicket = 0;
	m_handle = GpuHandle();
	m_handle = replacement;
	m_cleanupTicket = replacementTicket;
	if (m_authoritative != 0 && m_authoritativeBytes != 0)
	{
		memset(m_authoritative, 0, m_authoritativeBytes);
	}
	m_failedMutation = false;
	return RENDER_RESULT_OK;
}

RenderResult NativeW3DBufferOwner::Lock(size_t destinationOffset,
	size_t byteCount, RenderBufferUpdateMode mode, void **data)
{
	if (data == 0)
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	*data = 0;
	NativeW3DResources *resources = ActiveResources();
	ObserveAuthorityFailure(resources);
	if (resources == 0 || !m_handle.isValid() || m_locked ||
		!resources->IsValid(m_handle) || !IsSupportedUpdateMode(mode) ||
		destinationOffset > m_descriptor.byteCount)
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	if (byteCount == 0)
	{
		byteCount = m_descriptor.byteCount - destinationOffset;
	}
	if (byteCount == 0 || byteCount > m_descriptor.byteCount - destinationOffset ||
		(mode == RENDER_BUFFER_UPDATE_DISCARD && destinationOffset != 0) ||
		(m_descriptor.usage != RENDER_USAGE_DYNAMIC &&
		 mode != RENDER_BUFFER_UPDATE_PRESERVE))
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}

	if (m_failedMutation)
	{
		if (mode != RENDER_BUFFER_UPDATE_DISCARD || destinationOffset != 0)
		{
			return RENDER_RESULT_FAILED;
		}
		const RenderResult recreateResult = RecreateForDiscard();
		if (recreateResult != RENDER_RESULT_OK)
		{
			return recreateResult;
		}
	}
	unsigned char *staging = new(std::nothrow) unsigned char[byteCount];
	if (staging == 0)
	{
		return RENDER_RESULT_OUT_OF_MEMORY;
	}
	if (mode == RENDER_BUFFER_UPDATE_DISCARD &&
		m_authoritative != 0 && m_authoritativeBytes != 0)
	{
		// D3D discard invalidates the previous contents of the whole resource,
		// not merely the caller's range. Seed the replacement image only after
		// staging allocation succeeds; an allocation failure must not mutate the
		// authoritative shadow before a write has been accepted.
		memset(m_authoritative, 0, m_authoritativeBytes);
	}
	memset(staging, 0, byteCount);
	if (mode != RENDER_BUFFER_UPDATE_DISCARD && m_authoritative != 0 &&
		m_authoritativeBytes == m_descriptor.byteCount)
	{
		memcpy(staging, m_authoritative + destinationOffset, byteCount);
	}
	m_staging = staging;
	m_lockOffset = destinationOffset;
	m_lockBytes = byteCount;
	m_lockMode = mode;
	m_locked = true;
	*data = m_staging;
	return RENDER_RESULT_OK;
}

RenderResult NativeW3DBufferOwner::Unlock()
{
	if (!m_locked)
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	NativeW3DResources *resources = ActiveResources();
	ObserveAuthorityFailure(resources);
	if (resources == 0 || m_staging == 0 ||
		!m_handle.isValid() || !resources->IsValid(m_handle))
	{
		ReleaseStaging();
		m_failedMutation = true;
		return RENDER_RESULT_FAILED;
	}
	const RenderResult result = resources->UpdateBuffer(m_handle, m_staging,
		m_lockBytes, m_lockOffset, m_lockMode);
	if (result == RENDER_RESULT_OK && m_authoritative != 0 &&
		m_authoritativeBytes == m_descriptor.byteCount)
	{
		memcpy(m_authoritative + m_lockOffset, m_staging, m_lockBytes);
	}
	ReleaseStaging();
	m_failedMutation = result != RENDER_RESULT_OK;
	return result;
}

RenderResult NativeW3DBufferOwner::AcquireVertexRange(unsigned int stride,
	unsigned int offset, unsigned int startVertex, unsigned int vertexCount,
	GpuHandle *validated) const
{
	if (validated == 0)
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	*validated = GpuHandle();
	NativeW3DResources *resources = ActiveResources();
	ObserveAuthorityFailure(resources);
	if (resources == 0 || m_locked || m_failedMutation ||
		!m_handle.isValid() || !resources->IsValid(m_handle))
	{
		return RENDER_RESULT_FAILED;
	}
	if (m_descriptor.binding != RENDER_BUFFER_VERTEX ||
		stride != m_descriptor.stride)
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	return resources->AcquireVertexBufferRangeForSubmission(m_handle, stride, offset,
		startVertex, vertexCount, validated);
}

RenderResult NativeW3DBufferOwner::AcquireIndexRange(RenderFormat format,
	unsigned int offset, unsigned int startIndex, unsigned int indexCount,
	GpuHandle *validated) const
{
	if (validated == 0)
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	*validated = GpuHandle();
	NativeW3DResources *resources = ActiveResources();
	ObserveAuthorityFailure(resources);
	if (resources == 0 || m_locked || m_failedMutation ||
		!m_handle.isValid() || !resources->IsValid(m_handle))
	{
		return RENDER_RESULT_FAILED;
	}
	const unsigned int indexStride = format == RENDER_FORMAT_R16_UINT ? 2U :
		(format == RENDER_FORMAT_R32_UINT ? 4U : 0U);
	if (m_descriptor.binding != RENDER_BUFFER_INDEX || indexStride == 0 ||
		m_descriptor.stride != indexStride)
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	return resources->AcquireIndexBufferRangeForSubmission(m_handle, format, offset,
		startIndex, indexCount, validated);
}

bool NativeW3DBufferOwner::IsLocked() const
{
	return m_locked;
}

bool NativeW3DBufferOwner::HasFailedMutation() const
{
	ObserveAuthorityFailure(ActiveResources());
	return m_failedMutation;
}

void NativeW3DBufferOwner::ObserveAuthorityFailure(
	NativeW3DResources *resources) const
{
	if (!m_failedMutation && resources != 0 && m_handle.isValid() &&
		resources->HasBufferAuthorityFailure(m_handle))
	{
		m_failedMutation = true;
	}
}

NativeW3DResources *NativeW3DBufferOwner::ActiveResources() const
{
	return m_resources != 0 && m_resources == g_nativeW3DBufferResources &&
		m_bindingGeneration != 0 &&
		m_bindingGeneration == g_nativeW3DBufferBindingGeneration &&
		m_resources->IsOwnerThread() ?
		m_resources : 0;
}

void NativeW3DBufferOwner::ReleaseStaging()
{
	delete[] m_staging;
	m_staging = 0;
	m_lockOffset = 0;
	m_lockBytes = 0;
	m_lockMode = RENDER_BUFFER_UPDATE_PRESERVE;
	m_locked = false;
}

}
}
