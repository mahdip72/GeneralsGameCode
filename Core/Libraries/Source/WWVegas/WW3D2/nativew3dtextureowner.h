#ifndef RTS_WW3D2_NATIVEW3DTEXTUREOWNER_H
#define RTS_WW3D2_NATIVEW3DTEXTUREOWNER_H

// The native texture owner is an x64 migration boundary.  Keep the complete
// neutral-renderer type graph out of the VC6 translation unit while allowing
// legacy sources to include this header during an incremental cutover.
#if !defined(_MSC_VER) || _MSC_VER >= 1900

#include "Renderer/NativeW3DResources.h"

namespace rts
{
namespace render
{

// Borrows exactly one initialized registry generation.  The bridge must bind
// before creating candidates and unbind only after every owner is reset.
RenderResult BindNativeW3DTextureResources(NativeW3DResources *resources);
RenderResult UnbindNativeW3DTextureResources(NativeW3DResources *resources);

// An explicit unpublished ownership token.  A successfully created candidate
// destroys its typed handle if it leaves scope without being published.  A
// borrowed candidate never destroys the shared handle.
class NativeW3DTextureCandidate
{
public:
	NativeW3DTextureCandidate();
	~NativeW3DTextureCandidate();

	bool IsValid() const;
	NativeW3DTextureHandle Handle() const;

private:
	friend class NativeW3DTextureOwner;
	NativeW3DTextureCandidate(const NativeW3DTextureCandidate &);
	NativeW3DTextureCandidate &operator=(const NativeW3DTextureCandidate &);

	void Abandon();
	void Clear();

	NativeW3DTextureHandle m_handle;
	TextureDescriptor m_descriptor;
	NativeW3DTextureCleanupTicket *m_cleanupTicket;
	unsigned int m_bindingGeneration;
	bool m_owned;
};

class NativeW3DTextureOwner
{
public:
	NativeW3DTextureOwner();
	~NativeW3DTextureOwner();

	RenderResult CreateCandidate(const TextureDescriptor &descriptor,
		const TextureSubresourceData *subresources,
		unsigned int subresourceCount,
		NativeW3DTextureCandidate *candidate) const;
	RenderResult BorrowCandidate(NativeW3DTextureHandle handle,
		const TextureDescriptor &descriptor,
		NativeW3DTextureCandidate *candidate) const;
	RenderResult PublishCandidate(NativeW3DTextureCandidate *candidate,
		unsigned int expectedPublicationGeneration);

	// CPU content requires current CPU authority.  GPU content additionally
	// requires a lease token: an invalid token acquires the exact current lease,
	// while a valid token is revalidated and cleared when stale.
	RenderResult AcquireForSampling(NativeW3DTextureHandle *handle,
		NativeW3DGpuContentLease *gpuLease = 0) const;
	RenderResult AcquireSurface(unsigned int mipLevel,
		unsigned int arraySlice, NativeW3DSurfaceHandle *surface,
		NativeW3DGpuContentLease *gpuLease = 0) const;
	// Output acquisition deliberately does not require current sampled-content
	// authority: a recovered target must be rebound before its producer can
	// regenerate pixels. Publication occurs only after the render owner accepts
	// the exact typed surface as an output.
	RenderResult AcquireOutputSurface(unsigned int mipLevel,
		unsigned int arraySlice, NativeW3DSurfaceHandle *surface) const;
	RenderResult PublishOutputWrite(NativeW3DSurfaceHandle surface,
		NativeW3DGpuContentLease *gpuLease) const;
	// Copies the render owner's current color output directly into this exact
	// texture generation and publishes the resulting GPU authority lease.
	RenderResult CopyActiveColorTarget(
		NativeW3DGpuContentLease *gpuLease) const;
	RenderResult RefreshCpuContent(const TextureDescriptor &descriptor,
		const TextureSubresourceData *subresources,
		unsigned int subresourceCount) const;
	RenderResult Reset();

	unsigned int PublicationGeneration() const;

private:
	NativeW3DTextureOwner(const NativeW3DTextureOwner &);
	NativeW3DTextureOwner &operator=(const NativeW3DTextureOwner &);

	NativeW3DResources *ActiveResources() const;
	void AbandonPublication();
	void ClearPublication();
	void AdvancePublicationGeneration();

	NativeW3DTextureHandle m_handle;
	TextureDescriptor m_descriptor;
	NativeW3DTextureCleanupTicket *m_cleanupTicket;
	unsigned int m_bindingGeneration;
	unsigned int m_publicationGeneration;
	bool m_owned;
};

}
}

#endif

#endif
