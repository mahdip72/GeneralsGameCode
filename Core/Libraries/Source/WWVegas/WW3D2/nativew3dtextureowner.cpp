#include "nativew3dtextureowner.h"

#if !defined(_MSC_VER) || _MSC_VER >= 1900

#include <limits.h>

namespace rts
{
namespace render
{
namespace
{
NativeW3DResources *g_nativeW3DTextureResources = 0;
unsigned int g_nativeW3DTextureBindingGeneration = 0;
bool g_nativeW3DTextureBindingExhausted = false;

bool EqualTextureDescriptors(const TextureDescriptor &left,
	const TextureDescriptor &right)
{
	return left.width == right.width && left.height == right.height &&
		left.mipCount == right.mipCount &&
		left.arrayCount == right.arrayCount &&
		left.dimension == right.dimension && left.format == right.format &&
		left.binding == right.binding && left.usage == right.usage;
}

NativeW3DResources *ResourcesForGeneration(unsigned int generation)
{
	return generation != 0 &&
		generation == g_nativeW3DTextureBindingGeneration ?
		g_nativeW3DTextureResources : 0;
}

RenderResult ValidateCandidate(NativeW3DResources *resources,
	NativeW3DTextureHandle handle, const TextureDescriptor &descriptor,
	NativeW3DTextureDescription *description)
{
	if (resources == 0 || !handle.isValid() || description == 0)
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	const RenderResult result = resources->DescribeTexture(handle.resource,
		description);
	if (result != RENDER_RESULT_OK || !resources->IsValid(handle) ||
		!EqualTextureDescriptors(description->descriptor, descriptor))
	{
		return result == RENDER_RESULT_OK ? RENDER_RESULT_FAILED : result;
	}
	return RENDER_RESULT_OK;
}
}

RenderResult BindNativeW3DTextureResources(NativeW3DResources *resources)
{
	if (resources == 0 || !resources->IsOwnerThread() ||
		(g_nativeW3DTextureResources != 0 &&
		 g_nativeW3DTextureResources != resources))
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	if (g_nativeW3DTextureResources == resources)
	{
		return RENDER_RESULT_OK;
	}
	if (g_nativeW3DTextureBindingExhausted ||
		g_nativeW3DTextureBindingGeneration == UINT_MAX)
	{
		g_nativeW3DTextureBindingExhausted = true;
		return RENDER_RESULT_FAILED;
	}
	++g_nativeW3DTextureBindingGeneration;
	g_nativeW3DTextureResources = resources;
	return RENDER_RESULT_OK;
}

RenderResult UnbindNativeW3DTextureResources(NativeW3DResources *resources)
{
	if (g_nativeW3DTextureResources == 0)
	{
		return RENDER_RESULT_OK;
	}
	if (resources == 0 || g_nativeW3DTextureResources != resources ||
		!resources->IsOwnerThread())
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	g_nativeW3DTextureResources = 0;
	return RENDER_RESULT_OK;
}

NativeW3DTextureCandidate::NativeW3DTextureCandidate() :
	m_handle(), m_descriptor(), m_cleanupTicket(0), m_bindingGeneration(0),
	m_owned(false)
{
}

NativeW3DTextureCandidate::~NativeW3DTextureCandidate()
{
	Abandon();
}

bool NativeW3DTextureCandidate::IsValid() const
{
	return m_handle.isValid() && m_bindingGeneration != 0;
}

NativeW3DTextureHandle NativeW3DTextureCandidate::Handle() const
{
	return m_handle;
}

void NativeW3DTextureCandidate::Abandon()
{
	if (!m_owned)
	{
		Clear();
		return;
	}
	if (m_cleanupTicket != 0 &&
		NativeW3DResources::ReleaseTextureCleanupTicket(m_cleanupTicket) ==
			RENDER_RESULT_OK)
	{
		m_cleanupTicket = 0;
		Clear();
	}
}

void NativeW3DTextureCandidate::Clear()
{
	m_handle = NativeW3DTextureHandle();
	m_descriptor = TextureDescriptor();
	m_cleanupTicket = 0;
	m_bindingGeneration = 0;
	m_owned = false;
}

NativeW3DTextureOwner::NativeW3DTextureOwner() :
	m_handle(), m_descriptor(), m_cleanupTicket(0), m_bindingGeneration(0),
	m_publicationGeneration(0), m_owned(false)
{
}

NativeW3DTextureOwner::~NativeW3DTextureOwner()
{
	AbandonPublication();
}

RenderResult NativeW3DTextureOwner::CreateCandidate(
	const TextureDescriptor &descriptor,
	const TextureSubresourceData *subresources,
	unsigned int subresourceCount,
	NativeW3DTextureCandidate *candidate) const
{
	if (candidate == 0 || candidate->IsValid())
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	NativeW3DResources *resources = g_nativeW3DTextureResources;
	const unsigned int bindingGeneration =
		g_nativeW3DTextureBindingGeneration;
	if (resources == 0 || bindingGeneration == 0 ||
		!resources->IsOwnerThread())
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}

	NativeW3DTextureHandle created;
	const RenderResult result = resources->CreateTexture(descriptor,
		subresources, subresourceCount, &created);
	if (result != RENDER_RESULT_OK || !created.isValid())
	{
		return result == RENDER_RESULT_OK ? RENDER_RESULT_FAILED : result;
	}
	NativeW3DTextureDescription description;
	const RenderResult validation = ValidateCandidate(resources, created,
		descriptor, &description);
	if (validation != RENDER_RESULT_OK)
	{
		resources->RetireTexture(created);
		return validation;
	}
	NativeW3DTextureCleanupTicket *cleanupTicket = 0;
	const RenderResult ticketResult = resources->CreateTextureCleanupTicket(
		created, &cleanupTicket);
	if (ticketResult != RENDER_RESULT_OK || cleanupTicket == 0)
	{
		resources->RetireTexture(created);
		return ticketResult == RENDER_RESULT_OK ? RENDER_RESULT_FAILED :
			ticketResult;
	}

	candidate->m_handle = created;
	candidate->m_descriptor = descriptor;
	candidate->m_cleanupTicket = cleanupTicket;
	candidate->m_bindingGeneration = bindingGeneration;
	candidate->m_owned = true;
	return RENDER_RESULT_OK;
}

RenderResult NativeW3DTextureOwner::BorrowCandidate(
	NativeW3DTextureHandle handle, const TextureDescriptor &descriptor,
	NativeW3DTextureCandidate *candidate) const
{
	if (candidate == 0 || candidate->IsValid())
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	NativeW3DResources *resources = g_nativeW3DTextureResources;
	const unsigned int bindingGeneration =
		g_nativeW3DTextureBindingGeneration;
	if (resources == 0 || bindingGeneration == 0 ||
		!resources->IsOwnerThread())
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	NativeW3DTextureDescription description;
	const RenderResult validation = ValidateCandidate(resources, handle,
		descriptor, &description);
	if (validation != RENDER_RESULT_OK ||
		description.authority == NATIVE_W3D_CONTENT_INVALID)
	{
		return validation == RENDER_RESULT_OK ? RENDER_RESULT_FAILED :
			validation;
	}

	candidate->m_handle = handle;
	candidate->m_descriptor = descriptor;
	candidate->m_cleanupTicket = 0;
	candidate->m_bindingGeneration = bindingGeneration;
	candidate->m_owned = false;
	return RENDER_RESULT_OK;
}

RenderResult NativeW3DTextureOwner::PublishCandidate(
	NativeW3DTextureCandidate *candidate,
	unsigned int expectedPublicationGeneration)
{
	if (candidate == 0 || !candidate->IsValid())
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	NativeW3DResources *resources =
		ResourcesForGeneration(candidate->m_bindingGeneration);
	if (resources != 0 && !resources->IsOwnerThread())
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	if (resources == 0 ||
		candidate->m_bindingGeneration !=
			g_nativeW3DTextureBindingGeneration ||
		expectedPublicationGeneration != m_publicationGeneration ||
		m_publicationGeneration == UINT_MAX ||
		(m_handle.isValid() && ActiveResources() != resources) ||
		(m_handle.isValid() && candidate->m_handle.resource == m_handle.resource &&
		 candidate->m_handle.attachmentGeneration ==
			m_handle.attachmentGeneration))
	{
		return RENDER_RESULT_FAILED;
	}

	NativeW3DTextureDescription description;
	const RenderResult validation = ValidateCandidate(resources,
		candidate->m_handle, candidate->m_descriptor, &description);
	if (validation != RENDER_RESULT_OK ||
		description.authority == NATIVE_W3D_CONTENT_INVALID ||
		description.authorityEpoch == 0)
	{
		return validation == RENDER_RESULT_OK ? RENDER_RESULT_FAILED :
			validation;
	}

	// Once both publications are validated, retirement transfers cleanup to the
	// registry. A backend destroy refusal defers only the old physical resource;
	// it cannot invalidate the candidate that is about to become current.
	if (m_handle.isValid() && m_owned &&
		(ActiveResources() == 0 || m_cleanupTicket == 0 ||
		 NativeW3DResources::ReleaseTextureCleanupTicket(m_cleanupTicket) !=
			RENDER_RESULT_OK))
	{
		return RENDER_RESULT_FAILED;
	}
	if (m_owned)
	{
		m_cleanupTicket = 0;
	}

	m_handle = candidate->m_handle;
	m_descriptor = candidate->m_descriptor;
	m_cleanupTicket = candidate->m_cleanupTicket;
	m_bindingGeneration = candidate->m_bindingGeneration;
	m_owned = candidate->m_owned;
	candidate->m_cleanupTicket = 0;
	candidate->Clear();
	AdvancePublicationGeneration();
	return RENDER_RESULT_OK;
}

RenderResult NativeW3DTextureOwner::AcquireForSampling(
	NativeW3DTextureHandle *handle,
	NativeW3DGpuContentLease *gpuLease) const
{
	if (handle == 0)
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	const NativeW3DTextureHandle requestedHandle = *handle;
	const NativeW3DGpuContentLease requestedLease = gpuLease == 0 ?
		NativeW3DGpuContentLease() : *gpuLease;
	*handle = NativeW3DTextureHandle();
	if (gpuLease != 0)
	{
		*gpuLease = NativeW3DGpuContentLease();
	}

	NativeW3DResources *resources = ActiveResources();
	if (resources == 0 || !m_handle.isValid() ||
		(requestedHandle.isValid() &&
		 (requestedHandle.resource != m_handle.resource ||
		  requestedHandle.attachmentGeneration !=
			m_handle.attachmentGeneration)))
	{
		return RENDER_RESULT_FAILED;
	}

	NativeW3DTextureDescription description;
	const RenderResult validation = ValidateCandidate(resources, m_handle,
		m_descriptor, &description);
	if (validation != RENDER_RESULT_OK || description.authorityEpoch == 0)
	{
		return validation == RENDER_RESULT_OK ? RENDER_RESULT_FAILED :
			validation;
	}
	if (description.authority == NATIVE_W3D_CONTENT_CPU)
	{
		if (requestedLease.isValid())
		{
			return RENDER_RESULT_FAILED;
		}
		*handle = m_handle;
		return RENDER_RESULT_OK;
	}
	if (description.authority != NATIVE_W3D_CONTENT_GPU_RENDER_TARGET ||
		gpuLease == 0)
	{
		return RENDER_RESULT_FAILED;
	}

	NativeW3DGpuContentLease validatedLease = requestedLease;
	const RenderResult leaseResult = resources->AcquireGpuContentLease(
		m_handle.resource, &validatedLease);
	if (leaseResult != RENDER_RESULT_OK || !validatedLease.isValid() ||
		validatedLease.resource != m_handle.resource ||
		validatedLease.attachmentGeneration != m_handle.attachmentGeneration)
	{
		return leaseResult == RENDER_RESULT_OK ? RENDER_RESULT_FAILED :
			leaseResult;
	}
	*gpuLease = validatedLease;
	*handle = m_handle;
	return RENDER_RESULT_OK;
}

RenderResult NativeW3DTextureOwner::AcquireSurface(unsigned int mipLevel,
	unsigned int arraySlice, NativeW3DSurfaceHandle *surface,
	NativeW3DGpuContentLease *gpuLease) const
{
	if (surface == 0)
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	NativeW3DTextureHandle texture = surface->isValid() ?
		surface->texture : NativeW3DTextureHandle();
	const RenderResult samplingResult = AcquireForSampling(&texture, gpuLease);
	if (samplingResult != RENDER_RESULT_OK)
	{
		*surface = NativeW3DSurfaceHandle();
		return samplingResult;
	}
	NativeW3DResources *resources = ActiveResources();
	if (resources == 0)
	{
		*surface = NativeW3DSurfaceHandle();
		return RENDER_RESULT_FAILED;
	}
	return resources->AcquireTextureSurface(texture, mipLevel, arraySlice,
		surface);
}

RenderResult NativeW3DTextureOwner::AcquireOutputSurface(
	unsigned int mipLevel, unsigned int arraySlice,
	NativeW3DSurfaceHandle *surface) const
{
	if (surface == 0)
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	const NativeW3DSurfaceHandle requested = *surface;
	*surface = NativeW3DSurfaceHandle();
	NativeW3DResources *resources = ActiveResources();
	if (resources == 0 || !m_handle.isValid() ||
		(m_descriptor.binding & (RENDER_TEXTURE_RENDER_TARGET |
			RENDER_TEXTURE_DEPTH_STENCIL)) == 0)
	{
		return RENDER_RESULT_FAILED;
	}
	*surface = requested;
	return resources->AcquireTextureSurface(m_handle, mipLevel, arraySlice,
		surface);
}

RenderResult NativeW3DTextureOwner::PublishOutputWrite(
	NativeW3DSurfaceHandle surface, NativeW3DGpuContentLease *gpuLease) const
{
	if (gpuLease == 0)
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	*gpuLease = NativeW3DGpuContentLease();
	NativeW3DResources *resources = ActiveResources();
	if (resources == 0 || !m_handle.isValid() || !surface.isValid() ||
		surface.texture.resource != m_handle.resource ||
		surface.texture.attachmentGeneration != m_handle.attachmentGeneration)
	{
		return RENDER_RESULT_FAILED;
	}
	return resources->PublishRenderTargetWrite(surface, gpuLease);
}

RenderResult NativeW3DTextureOwner::CopyActiveColorTarget(
	NativeW3DGpuContentLease *gpuLease) const
{
	if (gpuLease == 0)
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	*gpuLease = NativeW3DGpuContentLease();
	NativeW3DResources *resources = ActiveResources();
	if (resources == 0 || !m_handle.isValid())
	{
		return RENDER_RESULT_FAILED;
	}
	return resources->CopyActiveColorTargetToTexture(m_handle.resource,
		gpuLease);
}

RenderResult NativeW3DTextureOwner::RefreshCpuContent(
	const TextureDescriptor &descriptor,
	const TextureSubresourceData *subresources,
	unsigned int subresourceCount) const
{
	NativeW3DResources *resources = ActiveResources();
	if (resources == 0 || !m_handle.isValid() ||
		!EqualTextureDescriptors(descriptor, m_descriptor) ||
		subresources == 0 ||
		subresourceCount != descriptor.mipCount * descriptor.arrayCount)
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	return resources->RefreshTexture(m_handle, descriptor, subresources,
		subresourceCount);
}

RenderResult NativeW3DTextureOwner::Reset()
{
	if (!m_handle.isValid())
	{
		return RENDER_RESULT_OK;
	}
	NativeW3DResources *resources = ActiveResources();
	if (resources == 0)
	{
		return RENDER_RESULT_FAILED;
	}
	if (!resources->IsOwnerThread())
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	if (m_owned && (m_cleanupTicket == 0 ||
		NativeW3DResources::ReleaseTextureCleanupTicket(m_cleanupTicket) !=
			RENDER_RESULT_OK))
	{
		return RENDER_RESULT_FAILED;
	}
	if (m_owned)
	{
		m_cleanupTicket = 0;
	}
	ClearPublication();
	AdvancePublicationGeneration();
	return RENDER_RESULT_OK;
}

unsigned int NativeW3DTextureOwner::PublicationGeneration() const
{
	return m_publicationGeneration;
}

NativeW3DResources *NativeW3DTextureOwner::ActiveResources() const
{
	return ResourcesForGeneration(m_bindingGeneration);
}

void NativeW3DTextureOwner::AbandonPublication()
{
	if (!m_handle.isValid())
	{
		return;
	}
	if (!m_owned)
	{
		ClearPublication();
		return;
	}
	if (m_cleanupTicket != 0 &&
		NativeW3DResources::ReleaseTextureCleanupTicket(m_cleanupTicket) ==
			RENDER_RESULT_OK)
	{
		m_cleanupTicket = 0;
		ClearPublication();
	}
}

void NativeW3DTextureOwner::ClearPublication()
{
	m_handle = NativeW3DTextureHandle();
	m_descriptor = TextureDescriptor();
	m_cleanupTicket = 0;
	m_bindingGeneration = 0;
	m_owned = false;
}

void NativeW3DTextureOwner::AdvancePublicationGeneration()
{
	if (m_publicationGeneration != UINT_MAX)
	{
		++m_publicationGeneration;
	}
}

}
}

#endif
