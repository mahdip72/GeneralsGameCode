#include "Utility/CppMacros.h"
#include "Renderer/RenderGameClient.h"
#include "Renderer/RenderGameClientNative.h"
#include "Renderer/RenderTexturePublication.h"

#include <stdio.h>

#if defined(_WIN64)
#include <atomic>
#include <chrono>
#include <thread>
#endif

#if __cplusplus >= 201103L
#include <type_traits>
#endif

namespace
{
int Check(bool condition, const char *message)
{
	if (condition)
	{
		return 0;
	}
	fprintf(stderr, "FAIL: %s\n", message);
	return 1;
}

class FakeNativeOwner : public rts::render::IGameRenderClientNativeOwner
{
public:
	FakeNativeOwner() : initialized(false), operational(false),
		target(rts::render::GAME_RENDER_TARGET_UNKNOWN), targetQueries(0),
		formatQueries(0), supportedFormat(rts::render::RENDER_FORMAT_UNKNOWN) {}

	virtual bool IsInitialized() const
	{
		return initialized;
	}

	virtual bool IsOperational() const
	{
		return operational;
	}

	virtual rts::render::GameRenderTargetKind ActiveRenderTargetKind() const
	{
		++targetQueries;
		return target;
	}

	virtual bool IsGameTextureFormatSupported(
		rts::render::RenderFormat format) const
	{
		++formatQueries;
		return format == supportedFormat;
	}

	bool initialized;
	bool operational;
	rts::render::GameRenderTargetKind target;
	mutable unsigned int targetQueries;
	mutable unsigned int formatQueries;
	rts::render::RenderFormat supportedFormat;
};

int TestNativeRenderTargetCapability()
{
	FakeNativeOwner owner;

	int result = 0;
	// The x64 test links the real native capability translation unit through
	// core_renderer.  VC6 validates the same owner predicate without importing
	// a native implementation into the legacy ABI target.
#if defined(_WIN64)
	rts::render::SetGameRenderClientNativeOwner(&owner);
	// Notification must be safe without a resource or an active native owner.
	// Native surface unlock is the publication transaction, not this hint.
	rts::render::NotifyTextureChanged(0);
	result |= Check(!rts::render::IsNativeD3D11PublicationActive() &&
		!rts::render::IsRenderTexturePublicationOperational(),
		"uninitialized native owner rejects texture publication");
	result |= Check(!rts::render::IsGameRenderingToTexture(),
		"uninitialized native owner does not claim a texture target");
	result |= Check(owner.targetQueries == 0,
		"uninitialized native owner is rejected before target inspection");
#else
	result |= Check(!rts::render::IsGameRenderingToTextureForOwner(&owner),
		"uninitialized native owner does not claim a texture target");
#endif

	owner.initialized = true;
	owner.operational = true;
	owner.target = rts::render::GAME_RENDER_TARGET_BACK_BUFFER;
#if defined(_WIN64)
	result |= Check(rts::render::IsNativeD3D11PublicationActive() &&
		rts::render::IsRenderTexturePublicationOperational(),
		"initialized operational native owner allows texture publication outside a scene");
	result |= Check(!rts::render::IsGameRenderingToTexture(),
		"active native back buffer is not reported as a texture");
	result |= Check(owner.targetQueries == 1,
		"operational native owner reports its target exactly once");
#else
	result |= Check(!rts::render::IsGameRenderingToTextureForOwner(&owner),
		"active native back buffer is not reported as a texture");
#endif

	owner.target = rts::render::GAME_RENDER_TARGET_TEXTURE;
#if defined(_WIN64)
	result |= Check(rts::render::IsGameRenderingToTexture(),
		"active native texture target is reported by the owner");
#else
	result |= Check(rts::render::IsGameRenderingToTextureForOwner(&owner),
		"active native texture target is reported by the owner");
#endif

	owner.target = rts::render::GAME_RENDER_TARGET_UNKNOWN;
#if defined(_WIN64)
	result |= Check(!rts::render::IsGameRenderingToTexture(),
		"unknown native target never guesses back-buffer or texture state");
#else
	result |= Check(!rts::render::IsGameRenderingToTextureForOwner(&owner),
		"unknown native target never guesses back-buffer or texture state");
#endif

	owner.operational = false;
	owner.target = rts::render::GAME_RENDER_TARGET_TEXTURE;
#if defined(_WIN64)
	result |= Check(!rts::render::IsGameRenderingToTexture(),
		"non-operational native owner cannot expose stale texture state");
	result |= Check(!rts::render::IsNativeD3D11PublicationActive() &&
		!rts::render::IsRenderTexturePublicationOperational(),
		"non-operational native owner rejects texture publication");

	rts::render::SetGameRenderClientNativeOwner(0);
	result |= Check(!rts::render::IsNativeD3D11PublicationActive() &&
		!rts::render::IsRenderTexturePublicationOperational(),
		"unpublished native owner rejects texture publication");
	result |= Check(!rts::render::IsGameRenderingToTexture(),
		"cleared native owner cannot claim a texture target");
#else
	result |= Check(!rts::render::IsGameRenderingToTextureForOwner(&owner),
		"non-operational native owner cannot expose stale texture state");
#endif
	return result;
}

int TestProjectionSeamDeclaration()
{
	// C++11 and newer can check the complete type in an unevaluated context;
	// the declaration itself keeps the C++98/VC6 source contract intact.
#if __cplusplus >= 201103L
	typedef void (*ProjectionSetter)(const Matrix4x4 &, float, float);
	static_assert(std::is_same<decltype(
		&rts::render::SetGameProjectionTransformWithZBias),
		ProjectionSetter>::value,
		"projection-with-Z-bias seam exposes the expected neutral signature");
#endif
	return 0;
}

int TestNeutralValueDefaults()
{
	rts::render::RenderBackBufferInfo backBufferInfo;
	rts::render::RenderCaptureHandle captureHandle;
	int result = 0;
	result |= Check(backBufferInfo.width == 0 && backBufferInfo.height == 0 &&
		backBufferInfo.format == rts::render::RENDER_FORMAT_UNKNOWN,
		"back-buffer info has neutral defaults without renderer linkage");
	result |= Check(captureHandle.kind ==
		rts::render::RENDER_CAPTURE_COMPRESSED_SCREENSHOT &&
		captureHandle.requestId == 0 && captureHandle.generation == 0,
		"capture handle has neutral defaults without renderer linkage");
	return result;
}

int TestTextureFormatCapability()
{
	FakeNativeOwner owner;
	int result = 0;

#if defined(_WIN64)
	rts::render::SetGameRenderClientNativeOwner(&owner);
	result |= Check(!rts::render::IsGameTextureFormatSupported(
		WW3D_FORMAT_A8R8G8B8),
		"uninitialized native owner fails texture format query closed");
	result |= Check(owner.formatQueries == 0,
		"uninitialized owner is rejected before format probe");

	owner.initialized = true;
	owner.operational = true;
	owner.supportedFormat = rts::render::RENDER_FORMAT_B8G8R8A8_UNORM;
	result |= Check(rts::render::IsGameTextureFormatSupported(
		WW3D_FORMAT_A8R8G8B8),
		"logical ARGB format uses the native BGRA resource capability");
	result |= Check(!rts::render::IsGameTextureFormatSupported(
		WW3D_FORMAT_L6V5U5),
		"explicitly legacy luminance bump format fails native query closed");
	result |= Check(!rts::render::IsGameTextureFormatSupported(
		WW3D_FORMAT_UNKNOWN),
		"unknown logical format fails native query closed");
	result |= Check(owner.formatQueries == 1,
		"only a proven native mapping reaches the device capability probe");

	owner.supportedFormat = rts::render::RENDER_FORMAT_R8G8_SNORM;
	result |= Check(rts::render::IsGameTextureFormatSupported(
		WW3D_FORMAT_U8V8),
		"U8V8 uses the native signed-normalized resource capability");
	rts::render::SetGameRenderClientNativeOwner(0);
#else
	// The VC6 command-sink target intentionally links no native adapter.  Its
	// product adapter is compiled in the paired title target; keep this test's
	// neutral owner type source-compatible without importing that backend.
	(void)owner;
#endif
	return result;
}

int TestNativeOwnerCallbackReentry()
{
	int result = 0;
#if defined(_WIN64)
	FakeNativeOwner owner;
	FakeNativeOwner replacement;
	owner.initialized = true;
	owner.operational = true;
	rts::render::SetGameRenderClientNativeOwner(&owner);
	{
		rts::render::NativeGameRenderOwnerScope command;
		result |= Check(command.Get() == &owner,
			"command pins the current native owner");
		result |= Check(rts::render::IsNativeGameRenderOwnerPinnedByCurrentThread(),
			"command reports its current-thread lifetime pin");
		rts::render::NativeGameRenderOwnerLifecycleScope rejectedLifecycle;
		result |= Check(!rejectedLifecycle.IsAcquired() &&
			rejectedLifecycle.Get() == 0,
			"callback lifecycle acquisition fails before exposing a teardown owner");
		// Resource rebuild and capture callbacks enter the neutral facade again.
		// A nested query must finish without dropping the outer lifetime pin.
		result |= Check(rts::render::IsNativeGameRendererActive(),
			"native callback can query the owner while a command pins it");
		rts::render::SetGameRenderClientNativeOwner(&replacement);
		result |= Check(rts::render::GetGameRenderClientNativeOwner() == &owner,
			"callback cannot replace a command-pinned native owner");
		rts::render::SetGameRenderClientNativeOwner(0);
		result |= Check(rts::render::GetGameRenderClientNativeOwner() == &owner,
			"callback cannot unpublish a command-pinned native owner");
	}
	rts::render::SetGameRenderClientNativeOwner(0);
	result |= Check(rts::render::GetGameRenderClientNativeOwner() == 0,
		"owner can be unpublished after the outer command returns");
#endif
	return result;
}

int TestNativeOwnerLifecycleExclusion()
{
	int result = 0;
#if defined(_WIN64)
	using namespace rts::render;
	FakeNativeOwner owner;
	owner.initialized = true;
	owner.operational = true;
	SetGameRenderClientNativeOwner(&owner);
	{
		NativeGameRenderOwnerLifecycleScope lifecycle;
		result |= Check(lifecycle.IsAcquired() && lifecycle.Get() == &owner &&
			IsNativeGameRenderOwnerPinnedByCurrentThread(),
			"top-level lifecycle acquires and pins its owner");
		NativeGameRenderOwnerLifecycleScope nested;
		result |= Check(!nested.IsAcquired(),
			"lifecycle callback cannot recursively acquire teardown authority");
		{
			NativeGameRenderOwnerScope query;
			result |= Check(query.Get() == &owner && IsNativeGameRendererActive(),
				"lifecycle callback permits nested owner queries");
			lifecycle.Publish(0);
			result |= Check(GetGameRenderClientNativeOwner() == &owner,
				"even an acquired lifecycle cannot unpublish during a nested query");
		}
		lifecycle.Publish(0);
		result |= Check(GetGameRenderClientNativeOwner() == 0,
			"lifecycle can unpublish after nested queries return");
	}
	result |= Check(!IsNativeGameRenderOwnerPinnedByCurrentThread(),
		"all current-thread pins release after lifecycle exit");

	SetGameRenderClientNativeOwner(&owner);
	std::atomic<bool> workerStarted(false);
	std::atomic<bool> workerCompleted(false);
	std::atomic<bool> workerSawPin(true);
	std::thread detach;
	{
		NativeGameRenderOwnerScope command;
		detach = std::thread([&]() {
			workerSawPin.store(IsNativeGameRenderOwnerPinnedByCurrentThread());
			workerStarted.store(true);
			SetGameRenderClientNativeOwner(0);
			workerCompleted.store(true);
		});
		const auto deadline = std::chrono::steady_clock::now() +
			std::chrono::seconds(2);
		while (!workerStarted.load() && std::chrono::steady_clock::now() < deadline)
			std::this_thread::yield();
		result |= Check(workerStarted.load() && !workerSawPin.load() &&
			!workerCompleted.load() && command.Get() == &owner,
			"other-thread lifecycle waits for the pin without inheriting its reentry state");
	}
	detach.join();
	result |= Check(workerCompleted.load() && GetGameRenderClientNativeOwner() == 0,
		"other-thread lifecycle completes after the command releases its pin");
#endif
	return result;
}
}

int main()
{
	int result = 0;
	result |= TestNativeRenderTargetCapability();
	result |= TestProjectionSeamDeclaration();
	result |= TestNeutralValueDefaults();
	result |= TestTextureFormatCapability();
	result |= TestNativeOwnerCallbackReentry();
	result |= TestNativeOwnerLifecycleExclusion();
	return result;
}
