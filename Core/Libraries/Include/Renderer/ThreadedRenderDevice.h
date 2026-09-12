#ifndef RTS_RENDERER_THREADEDRENDERDEVICE_H
#define RTS_RENDERER_THREADEDRENDERDEVICE_H

#include "Renderer/RendererDevice.h"
#include <Utility/stdint_adapter.h>

namespace rts
{
namespace render
{
// This interface is native-only. The unwrapped device remains the legacy
// reference. A serial packet mode retains owner affinity but disables overlap.
struct ThreadedRenderOptions
{
	ThreadedRenderOptions();
	bool serial;
	unsigned int maxFramesInFlight; // 2 or 3, including the producer's packet.
	size_t maxPacketBytes; // Owned payload bound; allocated lazily and reused.
	unsigned int maxPacketCommands; // Separate fixed-size command-storage bound.
	unsigned int resourceCapacity;
};

struct ThreadedRenderFrameCompletion
{
	ThreadedRenderFrameCompletion();
	uint64_t sequence;
	RenderResult result;
	RenderFrameOutcome outcome;
	// An accepted resource upload or dependency failed. The producer must
	// invalidate its optimistic resource/revision caches before retrying.
	bool resourceFailure;
	bool presented;
	bool operational;
};

struct ThreadedRenderMetrics
{
	ThreadedRenderMetrics();
	uint64_t submittedFrames;
	uint64_t completedFrames;
	uint64_t failedFrames;
	uint64_t producerOverlapFrames; // Begin calls while the owner is executing.
	uint64_t backpressureWaits;
	uint64_t producerWaitNanoseconds;
	uint64_t ownerExecutionNanoseconds;
	uint64_t rejectedPackets;
	unsigned int pendingPackets;
	unsigned int peakPendingPackets;
	size_t peakPacketBytes;
};

// The callback is invoked on the dedicated owner, as are initialize, shutdown,
// every backend call, and delete. The context must survive initialize(). No
// backend or native resource pointer is exposed to the producer.
typedef IRenderDevice *(*ThreadedRenderBackendFactory)(void *context);
IRenderDevice *CreateThreadedRenderDevice(ThreadedRenderBackendFactory factory,
	void *context, const ThreadedRenderOptions &options = ThreadedRenderOptions());
IRenderDevice *CreateThreadedD3D11RenderDevice(
	const ThreadedRenderOptions &options = ThreadedRenderOptions());
bool IsThreadedRenderDevice(const IRenderDevice *device);

// endFrame records teardown; present seals a visible packet. Non-visible
// render-to-texture frames MUST be sealed explicitly with presentFrame=false.
// A successful enqueue is NOT execution success. Poll every accepted frame's
// completion on the producer and publish cache/capture outcomes there. The
// bounded completion mailbox rejects admission if the consumer stops polling.
RenderResult SubmitThreadedRenderFrame(IRenderDevice *device, bool presentFrame);
uint64_t LastThreadedRenderFrameSequence(const IRenderDevice *device);
// Returns the producer's currently open frame sequence, or zero when resource
// commands are being recorded outside a frame. A nonzero sequence lets a
// resource registry defer publication to the matching completion without a
// per-upload render-owner fence.
uint64_t CurrentThreadedRenderFrameSequence(const IRenderDevice *device);
bool PollThreadedRenderCompletion(IRenderDevice *device,
	ThreadedRenderFrameCompletion *completion);

// A CPU owner fence: previously submitted commands have executed, not a GPU
// idle fence. Readback supplies its own GPU synchronization. Drain also flushes
// pending resource commands, but never implicitly presents an open frame.
RenderResult DrainThreadedRenderDevice(IRenderDevice *device);
// Roll back one unpublished logical resource transaction. The render owner
// destroys any native allocation first; only then is the producer handle
// released for reuse. This reports the rollback itself, independently of an
// earlier aggregate resource failure retained by Drain.
RenderResult RollbackThreadedRenderResource(IRenderDevice *device,
	GpuHandle handle);
bool GetThreadedRenderMetrics(const IRenderDevice *device,
	ThreadedRenderMetrics *metrics);

// Cancel the current recording only (already accepted packets still complete).
// Necessary lifecycle/resource commands are retained and executed in FIFO order;
// drawing/presentation is suppressed and a failed completion is published.
RenderResult CancelThreadedRenderFrame(IRenderDevice *device,
	RenderResult reason = RENDER_RESULT_FAILED);
}
}

#endif
