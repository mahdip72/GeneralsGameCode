#include "Utility/CppMacros.h"
#include "Renderer/LegacyAsyncFramePolicy.h"
#include <stdio.h>
#include <vector>
#if defined(_MSC_VER) && _MSC_VER < 1300
typedef unsigned __int64 uint64_t;
#else
#include <stdint.h>
#endif

// Compile the real bridge's cache-rebuild methods against retained-surface
// doubles. The production failure ranking is shared directly, not restated in
// a test implementation of the completion algorithm.
namespace
{
using namespace rts::render;
typedef int HRESULT;
const HRESULT S_OK = 0;
const HRESULT E_FAIL = -1;
const int D3DRTYPE_TEXTURE = 3;
#define FAILED(result) ((result) < 0)

int check(bool condition, const char *message)
{
	if (condition) return 0;
	fprintf(stderr, "FAIL: legacy async bridge completion: %s\n", message);
	return 1;
}

struct IDirect3DSurface8
{
	IDirect3DSurface8() : references(0), releases(0) {}
	unsigned references, releases;
	void Release() { --references; ++releases; }
};

struct IDirect3DBaseTexture8
{
	virtual int GetType() const = 0;
};

struct IDirect3DTexture8 : IDirect3DBaseTexture8
{
	IDirect3DTexture8() : kind(D3DRTYPE_TEXTURE), fail(false),
		returnFailedSurface(false), requestedMip(99) {}
	int kind;
	bool fail, returnFailedSurface;
	unsigned requestedMip;
	IDirect3DSurface8 surface;
	int GetType() const { return kind; }
	HRESULT GetSurfaceLevel(unsigned mip, IDirect3DSurface8 **output)
	{
		requestedMip = mip;
		*output = nullptr;
		if (!fail || returnFailedSurface)
		{
			++surface.references;
			*output = &surface;
		}
		return fail ? E_FAIL : S_OK;
	}
};

struct TargetBridge
{
	struct TextureEntry
	{
		TextureEntry(GpuHandle resource, IDirect3DBaseTexture8 *texture) :
			handle(resource), source(texture) {}
		GpuHandle handle;
		IDirect3DBaseTexture8 *source;
	};

	TargetBridge() : pending_target_change(false), target_transition_failed(false),
		failColor(false), failDepth(false), invalidated(false), released(false),
		fenced(false), ordered(true), retainsAtRelease(0), colorCalls(0), depthCalls(0),
		colorSurface(nullptr), depthSurface(nullptr) {}
	std::vector<TextureEntry> textures;
	RenderTargetBinding active_target, pending_target;
	bool pending_target_change, target_transition_failed;
	bool failColor, failDepth, invalidated, released, fenced, ordered;
	unsigned retainsAtRelease, colorCalls, depthCalls;
	IDirect3DSurface8 *colorSurface, *depthSurface;
	void Invalidate_GPU_Copy_Content() { invalidated = true; }
	void Release_Caches()
	{
		ordered = ordered && invalidated;
		for (unsigned index = 0; index < textures.size(); ++index)
			retainsAtRelease += static_cast<IDirect3DTexture8 *>(textures[index].source)->surface.references;
		textures.clear();
		released = true;
	}
	RenderResult Fence_Render()
	{
		ordered = ordered && released;
		fenced = true;
		return RENDER_RESULT_OK;
	}
	bool Ensure_Render_Target(IDirect3DSurface8 *surface, GpuHandle *handle, RenderResult *result)
	{
		ordered = ordered && fenced && surface->references != 0;
		++colorCalls;
		colorSurface = surface;
		*handle = GpuHandle(42, 2);
		*result = failColor ? RENDER_RESULT_FAILED : RENDER_RESULT_OK;
		return !failColor;
	}
	bool Ensure_Depth_Target(IDirect3DSurface8 *surface, GpuHandle *handle, RenderResult *result)
	{
		ordered = ordered && fenced && surface->references != 0;
		++depthCalls;
		depthSurface = surface;
		*handle = GpuHandle(43, 2);
		*result = failDepth ? RENDER_RESULT_FAILED : RENDER_RESULT_OK;
		return !failDepth;
	}

#include "LegacyAsyncTargetMethods.inc"
};

struct ServiceDevice
{
	ServiceDevice() : operational(true) {}
	bool operational;
	bool isOperational() const { return operational; }
};
struct ServiceCaptureQueue
{
	ServiceCaptureQueue() : cancellations(0) {}
	unsigned cancellations;
	void cancelCurrent(RenderResult) { ++cancellations; }
};

struct ServiceBridge
{
	ServiceBridge() : device(&ownedDevice), deferred_failure_sequence(0),
		recovered_failure_sequence(0), async_resource_failure(false), frame_open(false),
		fenceResult(RENDER_RESULT_OK), recoverResult(RENDER_RESULT_OK),
		fences(0), failAtFence(0), recoveries(0), rebuilds(0), cancellations(0) {}
	ServiceDevice ownedDevice;
	ServiceDevice *device;
	ServiceCaptureQueue capture_queue;
	RenderFrameOutcome deferred_failure;
	uint64_t deferred_failure_sequence, recovered_failure_sequence;
	bool async_resource_failure, frame_open;
	RenderResult fenceResult, recoverResult;
	unsigned fences, failAtFence, recoveries, rebuilds, cancellations;
	bool Is_Threaded() const { return true; }
	void Require_Owner_Thread(const char *) {}
	void Poll_Render_Completions() {} // Preamble fixtures deliberately have none.
	void Log_Result(const char *, RenderResult) {}
	void Log(const char *) {}
	void Cancel_Threaded_Frame(RenderResult) { ++cancellations; }
	RenderResult Fence_Render()
	{
		++fences;
		if (fences == failAtFence)
		{
			device->operational = false;
			fenceResult = RENDER_RESULT_DEVICE_REMOVED;
		}
		return fenceResult;
	}
	RenderResult Recover_Device()
	{
		++recoveries;
		device->operational = recoverResult == RENDER_RESULT_OK;
		if (device->operational) fenceResult = RENDER_RESULT_OK;
		return recoverResult;
	}
	void Rebuild_Resource_Caches() { ++rebuilds; }

#include "LegacyAsyncServiceMethods.inc"
};

RenderTargetBinding customBinding(bool color, bool depth)
{
	RenderTargetBinding binding;
	binding.hasColor = color;
	binding.useBackBufferColor = !color;
	binding.hasDepth = depth;
	binding.useBackBufferDepth = !depth;
	if (color) binding.color.resource = GpuHandle(1, 1);
	if (depth) binding.depth.resource = GpuHandle(2, 1);
	return binding;
}

int testTargetRebuild()
{
	int result = 0;
	for (unsigned custom = 0; custom != 4; ++custom)
	{
		IDirect3DTexture8 color, depth;
		TargetBridge bridge;
		bridge.textures.push_back(TargetBridge::TextureEntry(GpuHandle(1, 1), &color));
		bridge.textures.push_back(TargetBridge::TextureEntry(GpuHandle(2, 1), &depth));
		bridge.pending_target = customBinding((custom & 1) != 0, (custom & 2) != 0);
		bridge.pending_target_change = true;
		bridge.Rebuild_Resource_Caches();
		result |= check(bridge.ordered && bridge.pending_target_change &&
			!bridge.target_transition_failed && bridge.active_target.useBackBufferColor,
			"cache replacement fences old handles and defers the requested binding");
		result |= check(bridge.pending_target.hasColor == ((custom & 1) != 0) &&
			bridge.pending_target.hasDepth == ((custom & 2) != 0) &&
			bridge.pending_target.useBackBufferColor == ((custom & 1) == 0) &&
			bridge.pending_target.useBackBufferDepth == ((custom & 2) == 0),
			"custom/default color and depth ownership survive cache invalidation");
		result |= check(bridge.colorCalls == ((custom & 1) != 0 ? 1U : 0U) &&
			bridge.depthCalls == ((custom & 2) != 0 ? 1U : 0U) &&
			bridge.retainsAtRelease == bridge.colorCalls + bridge.depthCalls &&
			color.surface.references == 0 && depth.surface.references == 0,
			"source surfaces survive release and are released after handle recreation");
		if (custom & 1) result |= check(bridge.pending_target.color.resource == GpuHandle(42, 2),
			"custom color receives a replacement generation, not an old handle");
		if (custom & 2) result |= check(bridge.pending_target.depth.resource == GpuHandle(43, 2),
			"custom depth receives a replacement generation, not an old handle");
	}

	{
		IDirect3DTexture8 color;
		TargetBridge bridge;
		bridge.textures.push_back(TargetBridge::TextureEntry(GpuHandle(1, 1), &color));
		bridge.active_target = customBinding(true, false);
		bridge.active_target.color.mip = 2;
		bridge.Rebuild_Resource_Caches();
		result |= check(bridge.colorSurface == &color.surface && color.requestedMip == 2 &&
			bridge.pending_target.color.mip == 2 && !bridge.target_transition_failed,
			"the active target is restored when there is no newer pending request");
	}
	{
		IDirect3DTexture8 color;
		TargetBridge bridge;
		bridge.textures.push_back(TargetBridge::TextureEntry(GpuHandle(1, 1), &color));
		bridge.active_target = customBinding(true, false);
		bridge.pending_target = RenderTargetBinding();
		bridge.pending_target_change = true;
		bridge.Rebuild_Resource_Caches();
		result |= check(bridge.colorCalls == 0 && bridge.pending_target.useBackBufferColor,
			"a newer default-target request overrides the previous custom target");
	}

	for (unsigned failure = 0; failure != 5; ++failure)
	{
		IDirect3DTexture8 color, depth;
		TargetBridge bridge;
		bridge.textures.push_back(TargetBridge::TextureEntry(GpuHandle(1, 1), &color));
		bridge.textures.push_back(TargetBridge::TextureEntry(GpuHandle(2, 1), &depth));
		bridge.pending_target = customBinding(true, true);
		bridge.pending_target_change = true;
		bridge.failColor = failure == 0;
		bridge.failDepth = failure == 1;
		color.fail = failure == 2 || failure == 3;
		color.returnFailedSurface = failure == 3;
		if (failure == 4) color.kind = 0;
		bridge.Rebuild_Resource_Caches();
		result |= check(bridge.target_transition_failed && bridge.pending_target_change &&
			bridge.pending_target.hasColor && !bridge.pending_target.useBackBufferColor &&
			bridge.pending_target.hasDepth && !bridge.pending_target.useBackBufferDepth,
			"failed target recreation cannot silently redirect an RTT pass to the backbuffer");
		result |= check(!(failure == 1 ? bridge.pending_target.depth.resource :
			bridge.pending_target.color.resource).isValid() &&
			color.surface.references == 0 && depth.surface.references == 0 && bridge.ordered,
			"failed recreation retains an invalid custom binding and releases every retained surface");
	}
	return result;
}

int testFailurePriority()
{
	RenderFrameOutcome outcomes[10];
	const unsigned priorities[10] = { 0, 1, 2, 3, 3, 3, 4, 4, 4, 4 };
	outcomes[1].markPresented();
	outcomes[1].recordCapture(RENDER_RESULT_FAILED);
	outcomes[2].recordCommandFailure(RENDER_RESULT_FAILED);
	outcomes[3].recordEndFrame(RENDER_RESULT_FAILED);
	outcomes[4].recordPresentation(RENDER_RESULT_FAILED);
	outcomes[5].recordRecovery(RENDER_RESULT_FAILED);
	outcomes[6].recordCommandFailure(RENDER_RESULT_DEVICE_REMOVED);
	outcomes[7].recordEndFrame(RENDER_RESULT_DEVICE_REMOVED);
	outcomes[8].recordPresentation(RENDER_RESULT_DEVICE_REMOVED);
	outcomes[9].recordCapture(RENDER_RESULT_DEVICE_REMOVED);
	int result = 0;
	for (unsigned pending = 0; pending != 10; ++pending)
	{
		result |= check(LegacyAsyncFrameFailurePriority(outcomes[pending]) == priorities[pending],
			"failure priority preserves device/lifecycle/command/capture severity");
		for (unsigned completed = 0; completed != 10; ++completed)
		{
			result |= check(ShouldReplaceLegacyAsyncFrameFailure(outcomes[pending], outcomes[completed]) ==
				(priorities[completed] > priorities[pending]),
				"a later lifecycle error supersedes a harmless command drop or capture warning");
			result |= check(ShouldReplaceLegacyAsyncFrameFailure(outcomes[pending], outcomes[completed], true) ==
				(priorities[completed] > priorities[pending] ||
					(priorities[pending] == 4 && priorities[completed] == 4)),
				"a fresh device removal rearms recovery after the pending failure was recovered");
		}
	}
	return result;
}

int testPreambleRecovery()
{
	int result = 0;
	{
		ServiceBridge bridge;
		bridge.device->operational = false;
		bridge.fenceResult = RENDER_RESULT_DEVICE_REMOVED;
		bridge.Service_Render_Completions();
		result |= check(bridge.recoveries == 1 && bridge.rebuilds == 1 &&
			bridge.device->isOperational() && bridge.deferred_failure.hasDeviceRemoval() &&
			bridge.deferred_failure_sequence == ~static_cast<uint64_t>(0) &&
			bridge.recovered_failure_sequence == bridge.deferred_failure_sequence,
			"a preamble removal without any frame completion is recovered and retained for reporting");
		bridge.Service_Render_Completions();
		result |= check(bridge.recoveries == 1 && bridge.deferred_failure.hasDeviceRemoval(),
			"Begin_Display then Begin_Frame neither recover twice nor erase the pending error");
		bridge.device->operational = false;
		bridge.fenceResult = RENDER_RESULT_DEVICE_REMOVED;
		bridge.Service_Render_Completions();
		result |= check(bridge.recoveries == 2 && bridge.rebuilds == 2,
			"a fresh preamble removal rearms recovery before the previous one was reported");
		bridge.deferred_failure = RenderFrameOutcome();
		bridge.deferred_failure_sequence = 0;
		bridge.device->operational = false;
		bridge.fenceResult = RENDER_RESULT_DEVICE_REMOVED;
		bridge.Service_Render_Completions();
		result |= check(bridge.recoveries == 3 && bridge.rebuilds == 3,
			"reusing the reserved non-frame identity cannot suppress a later recovery");
	}
	{
		ServiceBridge bridge;
		bridge.device->operational = false;
		bridge.fenceResult = RENDER_RESULT_DEVICE_REMOVED;
		bridge.recoverResult = RENDER_RESULT_FAILED;
		bridge.Service_Render_Completions();
		const unsigned fences = bridge.fences;
		bridge.Service_Render_Completions();
		result |= check(bridge.recoveries == 1 && bridge.fences == fences &&
			bridge.rebuilds == 0 && bridge.async_resource_failure &&
			bridge.deferred_failure.recoveryResult() == RENDER_RESULT_FAILED,
			"failed recovery retains cache ownership and does not retry indefinitely");
	}
	{
		ServiceBridge bridge;
		bridge.async_resource_failure = true;
		bridge.failAtFence = 1;
		bridge.Service_Render_Completions();
		result |= check(bridge.rebuilds == 0 && bridge.async_resource_failure &&
			!bridge.device->isOperational(),
			"device removal during a cache drain prevents releasing its live ownership");
		bridge.Service_Render_Completions();
		result |= check(bridge.recoveries == 1 && bridge.rebuilds == 1 &&
			bridge.device->isOperational() && !bridge.async_resource_failure,
			"cache rebuilding resumes only after the drained failure is recovered");
	}
	{
		ServiceBridge bridge;
		bridge.frame_open = true;
		bridge.device->operational = false;
		bridge.deferred_failure.recordPresentation(RENDER_RESULT_DEVICE_REMOVED);
		bridge.deferred_failure_sequence = 7;
		bridge.Service_Render_Completions();
		bridge.Service_Render_Completions();
		result |= check(bridge.cancellations == 1 && !bridge.frame_open &&
			bridge.recoveries == 1 && bridge.deferred_failure_sequence == 7 &&
			bridge.recovered_failure_sequence == 7,
			"a delayed presented-frame removal cancels the current recording and remains reportable once");
	}
	return result;
}
}

int TestLegacyAsyncBridgeCompletion()
{
	return testFailurePriority() | testTargetRebuild() | testPreambleRecovery();
}
