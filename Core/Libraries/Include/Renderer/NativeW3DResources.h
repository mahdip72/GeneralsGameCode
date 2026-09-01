#ifndef RTS_RENDERER_NATIVEW3DRESOURCES_H
#define RTS_RENDERER_NATIVEW3DRESOURCES_H

#include "Renderer/NativeW3DRenderer.h"

#include <vector>

namespace rts
{
namespace render
{
class NativeW3DRenderState;

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
	// A successful completion preserves current authority. Aggregate resource
	// failure conservatively invalidates every authority/initialized range.
	RenderResult PublishThreadedCompletion(
		NativeW3DSubmissionSequence submissionSequence,
		bool resourceFailure);
	RenderResult CreateBuffer(const BufferDescriptor &descriptor,
		const void *initialData, size_t initialDataBytes, GpuHandle *handle);
	RenderResult CreateTexture(const TextureDescriptor &descriptor,
		const TextureSubresourceData *initialData, unsigned int initialDataCount,
		GpuHandle *handle);
	RenderResult UpdateBuffer(GpuHandle handle, const void *bytes,
		size_t byteCount, size_t destinationOffset,
		RenderBufferUpdateMode mode = RENDER_BUFFER_UPDATE_PRESERVE);
	RenderResult RefreshTexture(GpuHandle handle,
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
	bool IsValid(GpuHandle handle) const;

private:
	friend class NativeW3DRenderer;
	NativeW3DResources(const NativeW3DResources &);
	NativeW3DResources &operator=(const NativeW3DResources &);

	struct Slot;
	RenderResult BindState(NativeW3DRenderState *state);
	Slot *Find(GpuHandle handle);
	const Slot *Find(GpuHandle handle) const;
	NativeW3DContentAuthority EffectiveAuthority(const Slot &slot) const;
	RenderResult CompleteMutation(IRenderDevice *device, RenderResult result);
	void InvalidateAllAuthorities();
	void RecordBufferWrite(Slot &slot, size_t destinationOffset,
		size_t byteCount, RenderBufferUpdateMode mode);
	bool IsBufferRangeInitialized(const Slot &slot, size_t offset,
		size_t byteCount) const;
	unsigned int NextAuthorityEpoch();
	void PopulateLease(const Slot &slot, NativeW3DGpuContentLease *lease) const;
	bool IsVertexRangeValid(GpuHandle handle, unsigned int stride,
		unsigned int offset, unsigned int startVertex,
		unsigned int vertexCount) const;
	bool IsIndexRangeValid(GpuHandle handle, RenderFormat format,
		unsigned int offset, unsigned int startIndex,
		unsigned int indexCount) const;
	bool IsTextureValidOrEmpty(GpuHandle handle) const;
	bool IsBoundTo(const NativeW3DRenderer *renderer) const;
	static void DestroyDeferredResourceTable(void *context);
	static void ReleaseDeferredResourceTable(void *context);

	struct Impl;
	Impl *m_impl;
};
}
}

#endif
