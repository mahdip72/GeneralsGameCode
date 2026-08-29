#ifndef RTS_RENDERER_NATIVEW3DRENDERSTATE_H
#define RTS_RENDERER_NATIVEW3DRENDERSTATE_H

#include "Renderer/NativeW3DOwnerQueue.h"

namespace rts
{
namespace render
{
class NativeW3DRenderer;
class NativeW3DResources;
// This is the shared lifetime authority for the native facade and resource
// tables.  It deliberately owns no game object and exposes no backend COM
// pointer.  Producers may queue an opaque cleanup token only while the render
// owner accepts cleanup; shutdown closes admission before device teardown and
// drains already accepted work on the owner.  The renderer owns the final
// state reference until that drain completes, preventing cleanup packets from
// retaining their own containing queue.
class NativeW3DRenderState
{
public:
	static NativeW3DRenderState *Create(unsigned int cleanupCapacity = 256);

	void AddRef();
	void Release();

	RenderResult BindOwner();
	RenderResult BeginShutdown();
	RenderResult EnqueueCleanup(NativeW3DOwnerCommand command,
		NativeW3DOwnerToken *token);
	RenderResult DrainCleanup(unsigned int maxCommands, unsigned int *drained);

	bool IsOwnerThread() const;
	bool IsAcceptingCleanup() const;
	unsigned int PendingCleanup() const;

private:
	friend class NativeW3DRenderer;
	friend class NativeW3DResources;

	explicit NativeW3DRenderState(unsigned int cleanupCapacity);
	~NativeW3DRenderState();
	NativeW3DRenderState(const NativeW3DRenderState &);
	NativeW3DRenderState &operator=(const NativeW3DRenderState &);

	RenderResult AttachBackend(IRenderDevice *device, IRenderContext *context);
	RenderResult ReplaceContext(IRenderContext *context);
	RenderResult DetachBackend();
	bool IsOperational() const;
	unsigned int Generation() const;
	IRenderDevice *Device() const;
	IRenderContext *Context() const;

	volatile long m_references;
	NativeW3DOwnerQueue m_cleanup;
	IRenderDevice *m_device;
	IRenderContext *m_context;
	unsigned int m_generation;
};
}
}

#endif
