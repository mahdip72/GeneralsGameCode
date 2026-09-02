#ifndef RTS_RENDERER_NATIVEW3DRESOURCES_H
#define RTS_RENDERER_NATIVEW3DRESOURCES_H

#include "Renderer/NativeW3DRenderer.h"

#include <vector>

namespace rts
{
namespace render
{
class NativeW3DRenderState;
class NativeW3DBufferOwner;
class NativeW3DTextureCandidate;
class NativeW3DTextureOwner;
class NativeW3DTextureCleanupTicket;

#if defined(_MSC_VER)
typedef unsigned __int64 NativeW3DSubmissionSequence;
#else
typedef unsigned long long NativeW3DSubmissionSequence;
#endif

enum NativeW3DContentAuthority
{
	NATIVE_W3D_CONTENT_INVALID,
	NATIVE_W3D_CONTENT_CPU,
	NATIVE_W3D_CONTENT_GPU_RENDER_TARGET
};

struct NativeW3DBufferDescription
{
	NativeW3DBufferDescription();

	BufferDescriptor descriptor;
	NativeW3DContentAuthority authority;
	unsigned int authorityEpoch;
};

struct NativeW3DTextureDescription
{
	NativeW3DTextureDescription();

	TextureDescriptor descriptor;
	NativeW3DContentAuthority authority;
	unsigned int authorityEpoch;
};

struct NativeW3DGpuContentLease
{
	NativeW3DGpuContentLease();

	bool isValid() const;

	GpuHandle resource;
	unsigned int attachmentGeneration;
	unsigned int backendEpoch;
	unsigned int authorityEpoch;
};

// Typed product-facing texture ownership.  The resource remains an opaque
// index-plus-generation handle; the attachment generation prevents a cached
// texture from crossing a resource-host lifetime.
struct NativeW3DTextureHandle
{
	NativeW3DTextureHandle();

	bool isValid() const;

	GpuHandle resource;
	unsigned int attachmentGeneration;
};

// A surface is one logical texture subresource, not a native interface.  Its
// backend epoch deliberately expires after device recovery so adapters must
// reacquire the recreated mip/face instead of caching native ownership.
struct NativeW3DSurfaceHandle
{
	NativeW3DSurfaceHandle();

	bool isValid() const;

	NativeW3DTextureHandle texture;
	unsigned int backendEpoch;
	unsigned int mipLevel;
	unsigned int arraySlice;
	unsigned int width;
	unsigned int height;
	RenderFormat format;
};

// Borrows one already initialized backend and its immediate context.  It does
// not create, recover, shut down, or delete the device.  The product owner must
// publish context replacement after recovery and detach before backend
// destruction.  One host may reattach only that same device identity; a new
// backend lifecycle requires a new host.  All lifecycle methods are
// render-owner operations.
class NativeW3DResourceHost
{
public:
	explicit NativeW3DResourceHost(unsigned int cleanupCapacity = 256);
	~NativeW3DResourceHost();

	RenderResult Attach(IRenderDevice *device, IRenderContext *context);
	RenderResult ReplaceContext(IRenderContext *context);
	RenderResult DrainCleanup(unsigned int maxCommands, unsigned int *drained);
	RenderResult Detach();
	bool IsAttached() const;
	unsigned int PendingCleanup() const;
	unsigned int BoundResourceTables() const;

private:
	friend class NativeW3DResources;
	friend class ::NativeW3D2;
	NativeW3DResourceHost(const NativeW3DResourceHost &);
	NativeW3DResourceHost &operator=(const NativeW3DResourceHost &);

	NativeW3DRenderState *State() const;

	unsigned int m_cleanupCapacity;
	NativeW3DRenderState *m_state;
	IRenderDevice *m_boundDevice;
	unsigned int m_nextAttachmentGeneration;
};

class NativeW3DResources
{
public:
	explicit NativeW3DResources(unsigned int capacity = 4096);
	~NativeW3DResources();

	RenderResult Bind(NativeW3DRenderer *renderer);
	RenderResult BindHost(NativeW3DResourceHost *host);
	RenderResult Shutdown();
	// Publish render-owner completion in strictly increasing submission order.
	// A successful completion publishes accepted in-frame buffer ranges.
	// Aggregate render failure invalidates only buffer slots with pending writes
	// through that sequence, plus GPU-produced texture authority. Resource
	// commands outside a frame retain the synchronous lifecycle path.
	RenderResult PublishThreadedCompletion(
		NativeW3DSubmissionSequence submissionSequence,
		bool resourceFailure);
	RenderResult CreateBuffer(const BufferDescriptor &descriptor,
		const void *initialData, size_t initialDataBytes, GpuHandle *handle);
	RenderResult CreateTexture(const TextureDescriptor &descriptor,
		const TextureSubresourceData *initialData, unsigned int initialDataCount,
		GpuHandle *handle);
	RenderResult CreateTexture(const TextureDescriptor &descriptor,
		const TextureSubresourceData *initialData, unsigned int initialDataCount,
		NativeW3DTextureHandle *handle);
	RenderResult AcquireTexture(GpuHandle resource,
		NativeW3DTextureHandle *handle) const;
	// Passing an invalid surface acquires the requested subresource. Passing a
	// valid cached surface validates its exact resource, attachment, backend,
	// mip, slice, dimensions, and format; stale inputs are cleared on failure.
	RenderResult AcquireTextureSurface(NativeW3DTextureHandle texture,
		unsigned int mipLevel, unsigned int arraySlice,
		NativeW3DSurfaceHandle *surface) const;
	// Publish accepted render-owner output authority for an exact typed
	// subresource. Threaded aggregate failure invalidates the resulting lease
	// through PublishThreadedCompletion; no native view or COM object escapes.
	RenderResult PublishRenderTargetWrite(NativeW3DSurfaceHandle surface,
		NativeW3DGpuContentLease *lease);
	RenderResult UpdateBuffer(GpuHandle handle, const void *bytes,
		size_t byteCount, size_t destinationOffset,
		RenderBufferUpdateMode mode = RENDER_BUFFER_UPDATE_PRESERVE);
	// Ordinary resize preserves the buffer epoch but republishes authoritative
	// static bytes in case ResizeBuffers recovered the backend internally.
	RenderResult RepublishStaticBuffersAfterResize();
	// Explicit recovery advances the buffer epoch, then restores only static
	// ranges backed by an authoritative CPU image.
	RenderResult RestoreStaticBuffersAfterRecovery();
	RenderResult RefreshTexture(GpuHandle handle,
		const TextureDescriptor &descriptor,
		const TextureSubresourceData *subresources,
		unsigned int subresourceCount);
	RenderResult RefreshTexture(NativeW3DTextureHandle handle,
		const TextureDescriptor &descriptor,
		const TextureSubresourceData *subresources,
		unsigned int subresourceCount);
	RenderResult DescribeBuffer(GpuHandle handle,
		NativeW3DBufferDescription *description) const;
	RenderResult DescribeTexture(GpuHandle handle,
		NativeW3DTextureDescription *description) const;
	RenderResult CopyActiveColorTargetToTexture(GpuHandle handle,
		NativeW3DGpuContentLease *lease);
	// An invalid input lease acquires the current GPU epoch.  A valid input
	// lease is accepted only while that exact attachment/backend/authority epoch
	// remains current, allowing consumers to reject stale GPU content tokens.
	RenderResult AcquireGpuContentLease(GpuHandle handle,
		NativeW3DGpuContentLease *lease) const;
	// Acquires only the exact initialized draw range from the current attachment.
	// The output is cleared on every failure, including generation replacement or
	// aggregate threaded resource invalidation. No backend pointer is exposed.
	RenderResult AcquireVertexBufferRange(GpuHandle resource,
		unsigned int stride, unsigned int offset, unsigned int startVertex,
		unsigned int vertexCount, GpuHandle *validated) const;
	RenderResult AcquireIndexBufferRange(GpuHandle resource,
		RenderFormat format, unsigned int offset, unsigned int startIndex,
		unsigned int indexCount, GpuHandle *validated) const;
	bool Destroy(GpuHandle handle);
	bool DestroyTexture(NativeW3DTextureHandle handle);
	// Transfers an owned texture to registry cleanup.  A backend refusal hides
	// only this texture and defers physical destruction until Shutdown.
	bool RetireTexture(NativeW3DTextureHandle handle);
	bool IsOwnerThread() const;
	bool IsValid(GpuHandle handle) const;
	// A resource handle can remain structurally live after an asynchronous
	// upload failure.  This predicate distinguishes that failed authority from
	// an intentionally range-authoritative dynamic buffer.
	bool HasBufferAuthorityFailure(GpuHandle handle) const;
	bool IsValid(NativeW3DTextureHandle handle) const;
	bool IsValid(NativeW3DSurfaceHandle handle) const;

private:
	friend class NativeW3DRenderer;
	friend class NativeW3DBufferOwner;
	friend class NativeW3DTextureCandidate;
	friend class NativeW3DTextureOwner;
	NativeW3DResources(const NativeW3DResources &);
	NativeW3DResources &operator=(const NativeW3DResources &);

	struct Slot;
	struct Impl;
	RenderResult BindState(NativeW3DRenderState *state);
	Slot *Find(GpuHandle handle);
	const Slot *Find(GpuHandle handle) const;
	NativeW3DContentAuthority EffectiveAuthority(const Slot &slot) const;
	RenderResult CompleteMutation(IRenderDevice *device, RenderResult result);
	RenderResult CompleteBufferMutation(IRenderDevice *device,
		RenderResult result, Slot *affected);
	void InvalidateAllAuthorities();
	void InvalidateGpuAuthorities();
	void InvalidateBufferAuthority(Slot &slot);
	void InvalidateTextureAuthority(Slot &slot);
	RenderResult RepublishStaticBuffers(bool advanceBufferEpoch);
	bool IsBufferRangeInitialized(const Slot &slot, size_t offset,
		size_t byteCount) const;
	bool IsBufferRangeInitializedForSubmission(const Slot &slot, size_t offset,
		size_t byteCount) const;
	unsigned int NextAuthorityEpoch();
	void PopulateLease(const Slot &slot, NativeW3DGpuContentLease *lease) const;
	bool IsVertexRangeValid(GpuHandle handle, unsigned int stride,
		unsigned int offset, unsigned int startVertex,
		unsigned int vertexCount) const;
	bool IsIndexRangeValid(GpuHandle handle, RenderFormat format,
		unsigned int offset, unsigned int startIndex,
		unsigned int indexCount) const;
	bool IsVertexRangeValidForSubmission(GpuHandle handle,
		unsigned int stride, unsigned int offset, unsigned int startVertex,
		unsigned int vertexCount) const;
	bool IsIndexRangeValidForSubmission(GpuHandle handle, RenderFormat format,
		unsigned int offset, unsigned int startIndex,
		unsigned int indexCount) const;
	RenderResult AcquireVertexBufferRangeForSubmission(GpuHandle resource,
		unsigned int stride, unsigned int offset, unsigned int startVertex,
		unsigned int vertexCount, GpuHandle *validated) const;
	RenderResult AcquireIndexBufferRangeForSubmission(GpuHandle resource,
		RenderFormat format, unsigned int offset, unsigned int startIndex,
		unsigned int indexCount, GpuHandle *validated) const;
	bool IsTextureValidOrEmpty(GpuHandle handle) const;
	bool IsBoundTo(const NativeW3DRenderer *renderer) const;
	RenderResult CreateTextureCleanupTicket(NativeW3DTextureHandle handle,
		NativeW3DTextureCleanupTicket **ticket);
	static RenderResult ReleaseTextureCleanupTicket(
		NativeW3DTextureCleanupTicket *ticket);
	static bool RetireTextureImpl(Impl *impl,
		NativeW3DTextureHandle handle);
	static bool HasTextureCleanupTicket(const Impl *impl, GpuHandle handle);
	static void AddImplReference(Impl *impl);
	static void ReleaseImplReference(Impl *impl);
	static void ForgetTextureCleanupTicket(Impl *impl,
		NativeW3DTextureCleanupTicket *ticket);
	static void DestroyTransferredTexture(void *context);
	static void ReleaseTransferredTexture(void *context);
	static void DestroyDeferredResourceTable(void *context);
	static void ReleaseDeferredResourceTable(void *context);

	Impl *m_impl;
};
}
}

#endif
