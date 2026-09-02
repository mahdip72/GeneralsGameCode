#ifndef RTS_RENDERER_NATIVEW3DRENDERER_H
#define RTS_RENDERER_NATIVEW3DRENDERER_H

#include "Renderer/RendererDevice.h"

#include <Utility/stdint_adapter.h>

class NativeW3D2;

namespace rts
{
namespace render
{
class NativeW3DResources;
class NativeW3DRenderState;
class NativeW3DRecoveryTestAccess;
class NativeLine3DRenderContext;
struct ThreadedRenderFrameCompletion;
struct ThreadedRenderMetrics;
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
	// For deferred legacy sorting, source indices may retain their original
	// absolute vertex range while vertexData is copied from that range.  This
	// neutral value carries the old minVertexIndex without coupling the packet
	// to a retired rendering API or a title buffer class. Immediate submissions
	// leave it zero.
	unsigned int minimumVertexIndex;
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
	// Borrowed bridge frames are opened directly on the shared context.  These
	// owner-thread entry points use the same validation/state translation as
	// ordinary submissions without requiring this facade's private frame flag.
	RenderResult SetViewportExternal(const RenderViewport &viewport);
	// These owner-thread entry points are also valid while a borrowed bridge
	// frame is open.  They deliberately expose only neutral state and never
	// return the backend context to a caller.
	RenderResult SetRenderTargetsExternal(const RenderTargetBinding &binding);
	RenderResult ClearExternal(unsigned int clearFlags,
		const RenderFloat4 &color, float depth, unsigned int stencil);
	RenderResult CaptureBackBuffer(void *destination, size_t destinationBytes,
		size_t destinationRowPitch, RenderFormat *format);
	RenderResult GetTextureFilterCapabilities(
		RenderTextureFilterCapabilities *capabilities) const;
	RenderResult Submit(const NativeW3DResources &resources,
		const LegacyLogicalState &state,
		const NativeDrawPacket &packet);
	RenderResult SubmitExternal(const NativeW3DResources &resources,
		const LegacyLogicalState &state,
		const NativeDrawPacket &packet);
	RenderResult GetBackBufferInfo(RenderBackBufferInfo *info) const;
	RenderResult EndFrame(bool present);
	// Finalize an already-ended owner-thread frame.  Capture/readback must run
	// between EndFrame(false) and this operation because D3D11 rejects back-buffer
	// mapping while the immediate frame is open. Borrowed threaded devices use
	// this operation to submit the ended producer packet; immediate devices only
	// present when requested. EndFrame(false) intentionally leaves a borrowed
	// threaded packet in this ended state until this call.
	RenderResult FinalizeEndedFrame(bool present);
	// Present is retained as the compatibility spelling for a visible frame.
	RenderResult Present();
	RenderResult RecoverDevice();
	RenderResult Resize(unsigned int width, unsigned int height);
	RenderResult SetSwapInterval(unsigned int interval);
	RenderResult GetSwapInterval(unsigned int *interval) const;
	RenderResult SetGamma(float gamma, float brightness, float contrast,
		bool calibrate, bool useLimit);
	// These owner-thread probes keep the threaded backend lifecycle behind the
	// facade.  They publish no device/context pointer and are used by the
	// NativeW3D2 aggregate to service completion and recovery boundaries.
	bool HasBackendState() const;
	bool IsThreaded() const;
	bool IsBackendOperational() const;
	bool CanRecoverDevice() const;
	uint64_t LastThreadedSubmissionSequence() const;
	bool PollThreadedCompletion(ThreadedRenderFrameCompletion *completion);
	RenderResult DrainThreaded();
	RenderResult CancelThreadedFrame(RenderResult reason);
	bool GetThreadedMetrics(ThreadedRenderMetrics *metrics) const;
	bool IsInitialized() const;
	bool IsFrameOpen() const;
	unsigned int PendingCleanup() const;

private:
	friend class NativeW3DResources;
	friend class NativeW3DRecoveryTestAccess;
	friend class NativeLine3DRenderContext;
	friend class ::NativeW3D2;
	NativeW3DRenderer(const NativeW3DRenderer &);
	NativeW3DRenderer &operator=(const NativeW3DRenderer &);

	NativeW3DRenderState *m_state;
	// Set only by the owning NativeW3D2 aggregate. Borrowed product recovery is
	// orchestrated by the bridge that owns the backend.
	NativeW3DResources *m_recoveryResources;
	bool m_frameOpen;
	// A render-owner service can submit through this facade while retaining the
	// void Render() ABI.  EndFrame reports the first such failure instead of
	// silently presenting a frame whose line draw was rejected.
	RenderResult m_frameFailure;
	bool m_ownsBackend;
	bool m_borrowedMode;

	bool IsOwnerThread() const;
	void RecordFrameFailure(RenderResult result);
	RenderResult SetViewportInternal(const RenderViewport &viewport,
		bool requireFacadeFrame);
	RenderResult SubmitInternal(const NativeW3DResources &resources,
		const LegacyLogicalState &state, const NativeDrawPacket &packet,
		bool requireFacadeFrame);
	RenderResult AttachBorrowedState(NativeW3DRenderState *state);
	RenderResult DetachBorrowedState();
	static RenderResult DrainFailedRecoveryCleanup(
		NativeW3DRenderState *state, unsigned int *drained);
};
}
}

#endif
