#ifndef RTS_RENDERER_NATIVEW3DRENDERER_H
#define RTS_RENDERER_NATIVEW3DRENDERER_H

#include "Renderer/RendererDevice.h"

class NativeW3D2;

namespace rts
{
namespace render
{
class NativeW3DResources;
class NativeW3DRenderState;
class NativeW3DRecoveryTestAccess;
// Native facade inputs are intentionally logical and handle-based.  Game code
// never receives a backend COM pointer or a legacy-adapter header.
struct NativeW3DRendererDescriptor
{
	NativeW3DRendererDescriptor();

	unsigned int width;
	unsigned int height;
	unsigned int adapterIndex;
	bool enableDebugLayer;
	bool enableVsync;
	bool allowSoftwareFallback;
};

struct NativeDrawPacket
{
	NativeDrawPacket();

	GpuHandle vertexBuffer;
	GpuHandle indexBuffer;
	GpuHandle textures[LEGACY_TEXTURE_STAGE_COUNT];
	unsigned int vertexStride;
	unsigned int vertexOffset;
	unsigned int indexOffset;
	RenderFormat indexFormat;
	RenderVertexFormat vertexFormat;
	RenderVertexLayout vertexLayout;
	RenderPrimitiveTopology topology;
	unsigned int texturePresenceMask;
	unsigned int vertexCount;
	unsigned int startVertex;
	unsigned int indexCount;
	unsigned int startIndex;
	int baseVertex;
	bool indexed;
};

class NativeW3DRenderer
{
public:
	// This facade is an owner-thread object in Stage 2.  Every method, query,
	// and destruction must run on the thread that successfully initialized it.
	// Worker threads may only submit detached cleanup packets through resource
	// destruction; Stage 4 replaces this contract with a dedicated render-owner
	// service rather than permitting concurrent facade access.
	NativeW3DRenderer();
	~NativeW3DRenderer();

	RenderResult Initialize(void *window,
		const NativeW3DRendererDescriptor &descriptor);
	// The facade and its immediate context are render-owner objects.  Shutdown,
	// recovery, resize, and submission reject calls from another thread rather
	// than releasing backend state from an arbitrary caller. In borrowed mode,
	// Shutdown releases only this facade's state reference; it never shuts down
	// or detaches the borrowed device/context.
	RenderResult Shutdown();
	RenderResult BeginFrame();
	RenderResult SetViewport(const RenderViewport &viewport);
	RenderResult Submit(const NativeW3DResources &resources,
		const LegacyLogicalState &state,
		const NativeDrawPacket &packet);
	RenderResult EndFrame(bool present);
	RenderResult RecoverDevice();
	RenderResult Resize(unsigned int width, unsigned int height);
	bool IsInitialized() const;
	bool IsFrameOpen() const;
	unsigned int PendingCleanup() const;

private:
	friend class NativeW3DResources;
	friend class NativeW3DRecoveryTestAccess;
	friend class ::NativeW3D2;
	NativeW3DRenderer(const NativeW3DRenderer &);
	NativeW3DRenderer &operator=(const NativeW3DRenderer &);

	NativeW3DRenderState *m_state;
	bool m_frameOpen;
	bool m_ownsBackend;
	bool m_borrowedMode;

	bool IsOwnerThread() const;
	RenderResult AttachBorrowedState(NativeW3DRenderState *state);
	RenderResult DetachBorrowedState();
	static RenderResult DrainFailedRecoveryCleanup(
		NativeW3DRenderState *state, unsigned int *drained);
};
}
}

#endif
