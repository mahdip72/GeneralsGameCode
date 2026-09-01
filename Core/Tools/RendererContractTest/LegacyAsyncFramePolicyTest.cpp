#include "Utility/CppMacros.h"
#include "Renderer/RendererDevice.h"
#include "Renderer/RenderSubmissionPolicy.h"
#include "Renderer/LegacyAsyncFramePolicy.h"
#include <stdio.h>

// The scene policy and result mapping are extracted unchanged from production.
// Only their device/window environment is doubled. Profiling sleeps are not
// part of this device-free acceptance test.
#ifdef EXTENDED_STATS
#undef EXTENDED_STATS
#endif

namespace
{
typedef int HRESULT;
const HRESULT D3D_OK = 0;
const HRESULT E_FAIL = static_cast<HRESULT>(0x80004005U);
const HRESULT E_INVALIDARG = static_cast<HRESULT>(0x80070057U);
const HRESULT E_NOTIMPL = static_cast<HRESULT>(0x80004001U);
const HRESULT E_OUTOFMEMORY = static_cast<HRESULT>(0x8007000eU);
const HRESULT D3DERR_DEVICELOST = static_cast<HRESULT>(0x88760868U);
const HRESULT D3DERR_DEVICENOTRESET = static_cast<HRESULT>(0x88760869U);
#define SUCCEEDED(result) ((result) >= 0)
#define DX8_THREAD_ASSERT() ((void)0)
#define DX8CALL(call) DX8Wrapper::_Get_D3D_Device8()->call
#define DX8_RECORD_DX8_CALLS() ((void)0)
#define WWPROFILE(name) ((void)0)
#define WWDEBUG_SAY(message) ((void)0)

void DX8_Assert() {}
void DX8_ErrorCode(HRESULT) {}

int check(bool condition, const char *message)
{
	if (condition) return 0;
	fprintf(stderr, "FAIL: legacy async frame policy: %s\n", message);
	return 1;
}

struct FakeDevice
{
	HRESULT presentResult;
	HRESULT cooperativeResult;
	int endCalls;
	int presentCalls;
	int cooperativeCalls;
	FakeDevice() : presentResult(D3D_OK), cooperativeResult(D3D_OK),
		endCalls(0), presentCalls(0), cooperativeCalls(0) {}
	void EndScene() { ++endCalls; }
	HRESULT Present(void *, void *, void *, void *)
	{
		++presentCalls;
		return presentResult;
	}
	HRESULT TestCooperativeLevel()
	{
		++cooperativeCalls;
		return cooperativeResult;
	}
};

struct FakeBridge
{
	rts::render::RenderFrameOutcome outcome;
	rts::render::RenderResult result;
	bool active;
	bool lastPresentRequested;
	int endCalls;
	int shutdownCalls;
	FakeBridge() : result(rts::render::RENDER_RESULT_OK), active(true),
		lastPresentRequested(false), endCalls(0), shutdownCalls(0) {}
	rts::render::RenderResult End_Frame(bool present,
		rts::render::RenderFrameOutcome *output)
	{
		++endCalls;
		lastPresentRequested = present;
		*output = outcome;
		return result;
	}
	bool Is_Active() const { return active; }
	void Shutdown() { ++shutdownCalls; active = false; }
};
FakeBridge _D3D11Bridge;
bool _UseD3D11Backend = true;
bool _D3D11FrameStarted = true;

struct FakeCaps
{
	int Get_Max_Textures_Per_Pass() const { return 2; }
};
struct DX8WebBrowser { static void Render(int) {} };
struct ThreadClass { static void Sleep_Ms(int) {} };

class DX8Wrapper
{
public:
	static FakeDevice *D3DDevice;
	static FakeCaps *CurrentCaps;
	static bool IsDeviceLost;
	static unsigned long FrameCount;
	static int releaseCalls;
	static int resetCalls;
	static FakeDevice *_Get_D3D_Device8() { return D3DDevice; }
	static void End_Scene(bool flip_frames);
	static bool Reset_Device(bool, bool *requiresReacquire)
	{
		++resetCalls;
		*requiresReacquire = false;
		return true;
	}
	static void Set_Vertex_Buffer(void *) { ++releaseCalls; }
	static void Set_Index_Buffer(void *, unsigned) { ++releaseCalls; }
	static void Set_Texture(int, void *) { ++releaseCalls; }
	static void Set_Material(void *) { ++releaseCalls; }
};
FakeDevice *DX8Wrapper::D3DDevice = nullptr;
FakeCaps *DX8Wrapper::CurrentCaps = nullptr;
bool DX8Wrapper::IsDeviceLost = false;
unsigned long DX8Wrapper::FrameCount = 0;
int DX8Wrapper::releaseCalls = 0;
int DX8Wrapper::resetCalls = 0;

#include "LegacyAsyncFrameMethods.inc"

int runScene(const rts::render::RenderFrameOutcome &outcome,
	rts::render::RenderResult frameResult, bool visible,
	bool nativeBackend, bool sceneStarted, bool bridgeActive,
	HRESULT legacyPresentResult, bool expectedLost, unsigned expectedFrames,
	unsigned expectedLegacyPresents, unsigned expectedDeviceLossChecks,
	const char *message)
{
	FakeDevice device;
	FakeCaps caps;
	device.presentResult = legacyPresentResult;
	_D3D11Bridge = FakeBridge();
	_D3D11Bridge.outcome = outcome;
	_D3D11Bridge.result = frameResult;
	_D3D11Bridge.active = bridgeActive;
	_UseD3D11Backend = nativeBackend;
	_D3D11FrameStarted = sceneStarted;
	DX8Wrapper::D3DDevice = &device;
	DX8Wrapper::CurrentCaps = &caps;
	DX8Wrapper::IsDeviceLost = !expectedLost;
	DX8Wrapper::FrameCount = 0;
	DX8Wrapper::releaseCalls = DX8Wrapper::resetCalls = 0;
	DX8Wrapper::End_Scene(visible);
	int result = check(DX8Wrapper::IsDeviceLost == expectedLost &&
		DX8Wrapper::FrameCount == expectedFrames &&
		static_cast<unsigned>(device.presentCalls) == expectedLegacyPresents &&
		static_cast<unsigned>(device.cooperativeCalls) == expectedDeviceLossChecks,
		message);
	result |= check(device.endCalls == 1 &&
		_D3D11Bridge.endCalls == (sceneStarted ? 1 : 0) &&
		(!sceneStarted || _D3D11Bridge.lastPresentRequested == visible) &&
		!_D3D11FrameStarted && DX8Wrapper::releaseCalls == 5,
		"scene teardown and owner-side resource release remain unconditional");
	result |= check(_D3D11Bridge.outcome.wasSubmitted() == outcome.wasSubmitted() &&
		_D3D11Bridge.outcome.wasPresented() == outcome.wasPresented() &&
		_D3D11Bridge.outcome.result() == outcome.result(),
		"acceptance does not mutate the completion supplied by the bridge");
	DX8Wrapper::D3DDevice = nullptr;
	DX8Wrapper::CurrentCaps = nullptr;
	return result;
}

int runNative(const rts::render::RenderFrameOutcome &outcome,
	rts::render::RenderResult frameResult, bool lost, unsigned frames,
	const char *message, bool visible = true)
{
	// Native device loss and recovery stay inside the bridge/render owner. The
	// x64 facade has no D3D8 cooperative-level device to query, including when
	// End_Frame reports device removal separately from its completion outcome.
	return runScene(outcome, frameResult, visible, true, true, true, D3D_OK,
		lost, frames, 0, 0, message);
}
}

int TestLegacyAsyncFramePolicy()
{
	using namespace rts::render;
	int result = 0;
	RenderFrameOutcome queued;
	queued.recordEndFrame(RENDER_RESULT_OK);
	queued.markFrameEnded();
	queued.markSubmitted();
	result |= check(queued.wasSubmitted() && !queued.wasPresented(),
		"queue admission is distinct from physical presentation");
	result |= runNative(queued, RENDER_RESULT_OK, false, 1,
		"an admitted visible frame advances the producer without false device loss");
	result |= runNative(queued, RENDER_RESULT_OK, false, 0,
		"an admitted RTT frame does not present or advance the visible frame count", false);

	RenderFrameOutcome notSubmitted;
	notSubmitted.markFrameEnded();
	result |= runNative(notSubmitted, RENDER_RESULT_OK, true, 0,
		"an ended frame without admission or presentation is not accepted");
	RenderFrameOutcome notEnded;
	notEnded.markSubmitted();
	result |= runNative(notEnded, RENDER_RESULT_OK, true, 0,
		"an open packet cannot satisfy visible queue admission");
	RenderFrameOutcome unavailable = queued;
	unavailable.setOperational(false);
	result |= runNative(unavailable, RENDER_RESULT_OK, true, 0,
		"admission does not hide an unavailable backend");
	result |= runNative(queued, RENDER_RESULT_FAILED, true, 0,
		"admission does not hide failure returned separately by End_Frame");
	result |= runNative(queued, RENDER_RESULT_DEVICE_REMOVED, true, 0,
		"a separately returned device removal still enters device-loss handling");

	RenderFrameOutcome completed = queued;
	completed.markPresented();
	result |= runNative(completed, RENDER_RESULT_OK, false, 1,
		"completed native presentation retains its existing success path");
	RenderFrameOutcome captureWarning = completed;
	captureWarning.recordCapture(RENDER_RESULT_FAILED);
	result |= runNative(captureWarning, captureWarning.result(), false, 1,
		"successful presentation with a capture failure retains existing behavior");

	RenderFrameOutcome dropped = queued;
	dropped.recordCommandFailure(RENDER_RESULT_FAILED);
	result |= runNative(dropped, dropped.result(), false, 0,
		"a deliberate command-failed drop remains operational without frame advance");
	result |= runNative(dropped, dropped.result(), false, 0,
		"a non-device RTT command failure remains operational", false);
	result |= check(dropped.hasCommandFailure() &&
		dropped.result() == RENDER_RESULT_FAILED && !dropped.wasPresented(),
		"deliberate dropping does not erase command failure or claim presentation");

	RenderFrameOutcome latePresentFailure = queued;
	latePresentFailure.recordPresentation(RENDER_RESULT_FAILED);
	RenderFrameOutcome pendingFailure = dropped;
	if (ShouldReplaceLegacyAsyncFrameFailure(pendingFailure, latePresentFailure))
		pendingFailure = latePresentFailure;
	result |= runNative(pendingFailure, pendingFailure.result(), true, 0,
		"a batched later presentation failure is not hidden by an older command-only drop");
	pendingFailure = captureWarning;
	if (ShouldReplaceLegacyAsyncFrameFailure(pendingFailure, latePresentFailure))
		pendingFailure = latePresentFailure;
	result |= runNative(pendingFailure, pendingFailure.result(), true, 0,
		"a batched presentation failure is not hidden by an older presented capture warning");

	for (int phase = 0; phase != 4; ++phase)
	{
		for (int removed = 0; removed != 2; ++removed)
		{
			const RenderResult failure = removed ? RENDER_RESULT_DEVICE_REMOVED :
				RENDER_RESULT_FAILED;
			RenderFrameOutcome failed = queued;
			if (phase == 0) failed.recordEndFrame(failure);
			if (phase == 1) failed.recordCapture(failure);
			if (phase == 2) failed.recordPresentation(failure);
			if (phase == 3) failed.recordRecovery(failure);
			result |= runNative(failed, failed.result(), true, 0,
				"admission cannot mask a reported execution/lifecycle failure");
			result |= runNative(failed, RENDER_RESULT_OK, true, 0,
				"outcome failure rejects admission even if End_Frame returns OK");
			result |= runNative(failed, failed.result(), true, 0,
				"RTT lifecycle/device failure remains observable without presentation", false);
		}
	}
	RenderFrameOutcome commandRemoved = queued;
	commandRemoved.recordCommandFailure(RENDER_RESULT_DEVICE_REMOVED);
	result |= runNative(commandRemoved, commandRemoved.result(), true, 0,
		"device-removal command failures cannot use the deliberate-drop path");

	result |= runScene(RenderFrameOutcome(), RENDER_RESULT_OK, true,
		false, false, false, D3D_OK, false, 1, 1, 0,
		"legacy visible frames retain one legacy Present call");
	result |= runScene(RenderFrameOutcome(), RENDER_RESULT_OK, true,
		false, false, false, E_FAIL, true, 0, 1, 0,
		"legacy presentation failure still reports device loss");
	result |= runScene(queued, RENDER_RESULT_OK, true,
		true, false, true, D3D_OK, true, 0, 0, 0,
		"an active native backend that did not own the scene cannot present");
	result |= runScene(queued, RENDER_RESULT_OK, true,
		true, false, false, D3D_OK, true, 0, 0, 0,
		"an unavailable native backend never exposes the hidden legacy swap chain");
	result |= runScene(queued, RENDER_RESULT_OK, true,
		false, true, false, D3D_OK, false, 1, 0, 0,
		"latched native scene ownership prevents a legacy presentation after shutdown");
	return result;
}
