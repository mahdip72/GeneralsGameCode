#include "Utility/CppMacros.h"
#include "Renderer/RenderGameClient.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <vector>

namespace
{

const wchar_t kWindowClassName[] =
	L"GeneralsGameCodeNativeGameRenderClientLifecycleTest";

LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wparam,
	LPARAM lparam)
{
	return DefWindowProcW(window, message, wparam, lparam);
}

HWND CreateHiddenWindow()
{
	WNDCLASSEXW windowClass;
	ZeroMemory(&windowClass, sizeof(windowClass));
	windowClass.cbSize = sizeof(windowClass);
	windowClass.lpfnWndProc = WindowProcedure;
	windowClass.hInstance = GetModuleHandleW(0);
	windowClass.lpszClassName = kWindowClassName;
	RegisterClassExW(&windowClass);
	return CreateWindowExW(0, kWindowClassName,
		L"Native GameRenderClient lifecycle", WS_OVERLAPPEDWINDOW, 0, 0,
		64, 64, 0, 0, windowClass.hInstance, 0);
}

bool Check(bool condition, const char *message)
{
	if (condition)
		return true;
	std::fprintf(stderr, "Native GameRenderClient lifecycle test failed: %s\n",
		message);
	return false;
}

int TestLogicalPolicies()
{
	int failures = 0;
	failures += !Check(rts::render::GetGameTextureBitdepth() == 16,
		"texture bit depth defaults to logical 16 before device publication");
	failures += !Check(rts::render::SetGameTextureBitdepth(32) ==
		rts::render::RENDER_RESULT_OK,
		"logical texture bit depth accepts 32");
	failures += !Check(rts::render::GetGameTextureBitdepth() == 32,
		"logical texture bit depth stores 32");
	failures += !Check(rts::render::SetGameTextureBitdepth(24) ==
		rts::render::RENDER_RESULT_INVALID_ARGUMENT &&
		rts::render::GetGameTextureBitdepth() == 32,
		"logical texture bit depth rejects 24 without changing state");

	const unsigned int validModes[] = {
		rts::render::GAME_RENDER_MULTISAMPLE_NONE,
		rts::render::GAME_RENDER_MULTISAMPLE_2X,
		rts::render::GAME_RENDER_MULTISAMPLE_4X,
		rts::render::GAME_RENDER_MULTISAMPLE_8X
	};
	for (unsigned int i = 0; i < sizeof(validModes) / sizeof(validModes[0]);
		++i)
	{
		failures += !Check(rts::render::SetGameMSAAMode(validModes[i]) ==
			rts::render::RENDER_RESULT_OK &&
			rts::render::GetGameMSAAMode() ==
			rts::render::GAME_RENDER_MULTISAMPLE_NONE,
			"valid MSAA policy is accepted with effective NONE");
	}
	failures += !Check(rts::render::SetGameMSAAMode(1) ==
		rts::render::RENDER_RESULT_INVALID_ARGUMENT &&
		rts::render::GetGameMSAAMode() ==
		rts::render::GAME_RENDER_MULTISAMPLE_NONE,
		"invalid MSAA policy is rejected");
	return failures;
}

struct ReentrantCleanupHook : public rts::render::GameRenderCleanupHook
{
	explicit ReentrantCleanupHook(unsigned int shaderToDelete) :
		shaderToDelete(shaderToDelete), releaseCreatedShader(0),
		reacquireCreatedShader(0), releaseDeleteResult(
			rts::render::RENDER_RESULT_FAILED), releaseCreateResult(
			rts::render::RENDER_RESULT_FAILED), releaseCreatedDeleteResult(
			rts::render::RENDER_RESULT_FAILED), reacquireCreateResult(
			rts::render::RENDER_RESULT_FAILED), reacquireCreatedDeleteResult(
			rts::render::RENDER_RESULT_FAILED), releaseCalls(0), reacquireCalls(0),
		reentryCalls(0), releaseDeviceCount(-1), releaseDeviceIndex(-2),
		reacquireDeviceCount(-1) {}

	virtual void ReleaseResources()
	{
		++releaseCalls;
		++reentryCalls;
		releaseDeviceCount = rts::render::GetGameRenderDeviceCount();
		releaseDeviceIndex = rts::render::GetGameRenderDeviceIndex();
		releaseDeleteResult = rts::render::DeleteGameVertexShader(
			shaderToDelete) ? rts::render::RENDER_RESULT_OK :
			rts::render::RENDER_RESULT_FAILED;
		releaseCreateResult = rts::render::CreateGameShaderFromAsset(
			"shaders\\trees.vso", true, 0, 0, 0, &releaseCreatedShader);
		if (releaseCreateResult == rts::render::RENDER_RESULT_OK)
		{
			releaseCreatedDeleteResult =
				rts::render::DeleteGameVertexShader(releaseCreatedShader) ?
				rts::render::RENDER_RESULT_OK :
				rts::render::RENDER_RESULT_FAILED;
		}
	}

	virtual void ReAcquireResources()
	{
		++reacquireCalls;
		++reentryCalls;
		reacquireDeviceCount = rts::render::GetGameRenderDeviceCount();
		reacquireCreateResult = rts::render::CreateGameShaderFromAsset(
			"shaders\\monochrome.pso", false, 0, 0, 0,
			&reacquireCreatedShader);
		if (reacquireCreateResult == rts::render::RENDER_RESULT_OK)
		{
			reacquireCreatedDeleteResult =
				rts::render::DeleteGamePixelShader(reacquireCreatedShader) ?
				rts::render::RENDER_RESULT_OK :
				rts::render::RENDER_RESULT_FAILED;
		}
	}

	unsigned int shaderToDelete;
	unsigned int releaseCreatedShader;
	unsigned int reacquireCreatedShader;
	rts::render::RenderResult releaseDeleteResult;
	rts::render::RenderResult releaseCreateResult;
	rts::render::RenderResult releaseCreatedDeleteResult;
	rts::render::RenderResult reacquireCreateResult;
	rts::render::RenderResult reacquireCreatedDeleteResult;
	unsigned int releaseCalls;
	unsigned int reacquireCalls;
	unsigned int reentryCalls;
	int releaseDeviceCount;
	int releaseDeviceIndex;
	int reacquireDeviceCount;
};

int TestNativeLifecycle(HWND window)
{
	int failures = 0;
	const rts::render::RenderResult initializeResult =
		rts::render::InitializeGameRenderer(window, 640, 480, false, false);
	if (initializeResult == rts::render::RENDER_RESULT_UNSUPPORTED)
		return 77;
	if (!Check(initializeResult == rts::render::RENDER_RESULT_OK,
		"native bootstrap initializes the hidden D3D11 target"))
		return 1;

	const long intervals[] = { 0, 1, 3 };
	for (unsigned int i = 0; i != sizeof(intervals) / sizeof(intervals[0]); ++i)
	{
		failures += !Check(rts::render::SetGameRendererSwapInterval(intervals[i]) ==
			rts::render::RENDER_RESULT_OK &&
			rts::render::GetGameRendererSwapInterval() == intervals[i],
			"native presentation interval reaches the render owner");
	}
	failures += !Check(rts::render::SetGameRendererSwapInterval(-1) ==
		rts::render::RENDER_RESULT_INVALID_ARGUMENT &&
		rts::render::SetGameRendererSwapInterval(4) ==
		rts::render::RENDER_RESULT_INVALID_ARGUMENT &&
		rts::render::GetGameRendererSwapInterval() == 3,
		"invalid presentation intervals preserve the prior device policy");
	failures += !Check(rts::render::SetGameGamma(2.0f, 0.1f, 1.25f, true) ==
		rts::render::RENDER_RESULT_OK &&
		rts::render::SetGameGamma(1.0f, 0.0f, 1.0f, false) ==
		rts::render::RENDER_RESULT_OK,
		"native gamma settings reach the renderer-local presentation pass");

	char deviceName[rts::render::GAME_RENDER_DEVICE_STRING_CAPACITY];
	std::memset(deviceName, 0, sizeof(deviceName));
	failures += !Check(rts::render::GetGameRenderDeviceCount() == 1 &&
		rts::render::GetGameRenderDeviceIndex() == 0,
		"bootstrap publishes exactly logical device index 0");
	failures += !Check(rts::render::GetGameRenderDeviceName(0, deviceName,
		sizeof(deviceName)) == rts::render::RENDER_RESULT_OK &&
		std::strcmp(deviceName, "Native D3D11 renderer") == 0,
		"device name is the truthful logical native renderer name");

	char invalidName[rts::render::GAME_RENDER_DEVICE_STRING_CAPACITY];
	std::memset(invalidName, 0, sizeof(invalidName));
	failures += !Check(rts::render::GetGameRenderDeviceName(1, invalidName,
		sizeof(invalidName)) == rts::render::RENDER_RESULT_INVALID_ARGUMENT,
		"device name rejects index 1");

	rts::render::GameRenderDeviceDesc description = {};
	unsigned int resolutionCount = 0;
	failures += !Check(rts::render::GetGameRenderDeviceDesc(0, &description,
		0, 0, &resolutionCount) == rts::render::RENDER_RESULT_OK &&
		resolutionCount != 0,
		"device description reports operating-system display modes");
	failures += !Check(std::strcmp(description.deviceName,
		"Native D3D11 renderer") == 0 &&
		std::strcmp(description.driverName, "d3d11.dll") == 0 &&
		std::strcmp(description.driverVersion, "Unknown") == 0 &&
		std::strcmp(description.hardwareName, "Unknown") == 0 &&
		description.adapterIndex == 0,
		"device description contains logical and unknown metadata accurately");
	if (resolutionCount > 4096U)
	{
		failures += !Check(false, "display mode count is bounded");
	}
	else if (resolutionCount != 0U)
	{
		std::vector<rts::render::GameRenderResolutionDesc> resolutions(
			resolutionCount);
		unsigned int copiedCount = resolutionCount;
		const rts::render::RenderResult descResult =
			rts::render::GetGameRenderDeviceDesc(0, &description,
			&resolutions[0], static_cast<unsigned int>(resolutions.size()),
			&copiedCount);
		failures += !Check(descResult == rts::render::RENDER_RESULT_OK &&
			copiedCount != 0 && copiedCount <= resolutionCount,
			"device description copies its reported display modes");
		for (unsigned int i = 0; i < copiedCount; ++i)
		{
			failures += !Check(resolutions[i].width > 0 &&
				resolutions[i].height > 0 && resolutions[i].bitDepth == 32 &&
				resolutions[i].refreshRate >= 0,
				"display mode has valid dimensions and native bit depth");
		}
	}

	rts::render::GameRenderDeviceDesc invalidDescription = {};
	unsigned int invalidResolutionCount = 42;
	failures += !Check(rts::render::GetGameRenderDeviceDesc(1,
		&invalidDescription, 0, 0, &invalidResolutionCount) ==
		rts::render::RENDER_RESULT_INVALID_ARGUMENT &&
		invalidResolutionCount == 0,
		"device description rejects index 1");

	failures += !Check(rts::render::SetGameRenderDeviceByName(
		"not a native renderer", 640, 480, 32, 1, true) ==
		rts::render::RENDER_RESULT_INVALID_ARGUMENT,
		"device selection rejects an unknown name");
	failures += !Check(rts::render::SetGameRenderDeviceByIndex(1, 640, 480,
		32, 1, true, false, true) ==
		rts::render::RENDER_RESULT_INVALID_ARGUMENT,
		"device selection rejects index 1");
	failures += !Check(rts::render::SetGameRenderDeviceByIndex(-1, 640, 480,
		32, 1, true, false, true) ==
		rts::render::RENDER_RESULT_INVALID_ARGUMENT,
		"device selection rejects a negative index");
	failures += !Check(rts::render::SetGameRenderDeviceByIndex(0, 640, 480,
		24, 1, true, false, true) ==
		rts::render::RENDER_RESULT_INVALID_ARGUMENT,
		"device selection rejects unsupported display bit depth");
	failures += !Check(rts::render::SetGameRenderDeviceByIndex(0, 640, 480,
		32, 2, true, false, true) ==
		rts::render::RENDER_RESULT_INVALID_ARGUMENT,
		"device selection rejects invalid window mode");

	failures += !Check(rts::render::SetGameRenderDeviceByIndex(0, 800, 600,
		16, 1, true, false, true) == rts::render::RENDER_RESULT_OK,
		"index zero selection routes through the native resize transaction");
	int width = 0;
	int height = 0;
	int bitDepth = 0;
	bool windowed = false;
	failures += !Check(rts::render::GetGameRendererResolution(&width, &height,
		&bitDepth, &windowed) == rts::render::RENDER_RESULT_OK &&
		width == 800 && height == 600 && bitDepth == 32 && windowed,
		"native selection reports effective 32-bit resized state");

	failures += !Check(rts::render::SetGameRenderDeviceByName(deviceName,
		640, 480, 16, 0, true) == rts::render::RENDER_RESULT_OK,
		"valid logical device name routes through bootstrap presentation");
	failures += !Check(rts::render::GetGameRendererResolution(&width, &height,
		&bitDepth, &windowed) == rts::render::RENDER_RESULT_OK &&
		width == 640 && height == 480 && bitDepth == 32 && !windowed,
		"native name selection reports borderless presentation state");
	failures += !Check(rts::render::SetGameRenderDeviceByName(deviceName,
		640, 480, 16, 1, true) == rts::render::RENDER_RESULT_OK,
		"valid logical device name restores windowed presentation");
	failures += !Check(rts::render::SetAnyGameRenderDevice() ==
		rts::render::RENDER_RESULT_OK &&
		rts::render::SetNextGameRenderDevice() ==
		rts::render::RENDER_RESULT_OK &&
		rts::render::GetGameRenderDeviceIndex() == 0,
		"sole logical renderer accepts any and next without fake switching");

	unsigned int shaderToDelete = 0;
	const rts::render::RenderResult shaderCreateResult =
		rts::render::CreateGameShaderFromAsset(
			"shaders\\trees.vso", true, 0, 0, 0, &shaderToDelete);
	failures += !Check(shaderCreateResult == rts::render::RENDER_RESULT_OK &&
		shaderToDelete != 0,
		"native shader creation publishes a logical generation-safe handle");
	ReentrantCleanupHook cleanupHook(shaderToDelete);
	rts::render::SetGameCleanupHook(&cleanupHook);
	const rts::render::RenderResult reentrantResize =
		rts::render::SetGameRenderDeviceByIndex(0, 1024, 768, 16, 1, true,
		true, true);
	failures += !Check(reentrantResize == rts::render::RENDER_RESULT_OK &&
		cleanupHook.releaseCalls == 1 && cleanupHook.reacquireCalls == 1 &&
		cleanupHook.reentryCalls == 2 && cleanupHook.releaseDeviceCount == 0 &&
		cleanupHook.releaseDeviceIndex == -1 &&
		cleanupHook.reacquireDeviceCount == 0 &&
		cleanupHook.releaseDeleteResult == rts::render::RENDER_RESULT_OK &&
		cleanupHook.releaseCreateResult == rts::render::RENDER_RESULT_OK &&
		cleanupHook.releaseCreatedDeleteResult ==
			rts::render::RENDER_RESULT_OK &&
		cleanupHook.reacquireCreateResult == rts::render::RENDER_RESULT_OK &&
		cleanupHook.reacquireCreatedDeleteResult ==
			rts::render::RENDER_RESULT_OK,
		"resize cleanup callbacks can re-enter the native facade for shader rebuilds");

	rts::render::SetGameCleanupHook(0);
	failures += !Check(rts::render::ShutdownGameRenderer() ==
		rts::render::RENDER_RESULT_OK,
		"native bootstrap shuts down after lifecycle coverage");
	return failures == 0 ? 0 : 1;
}

} // namespace

int main()
{
	int result = TestLogicalPolicies();
	HWND window = CreateHiddenWindow();
	if (window == 0)
	{
		std::fprintf(stderr, "Native GameRenderClient lifecycle test could not "
			"create a hidden window\n");
		return 1;
	}
	const int lifecycleResult = TestNativeLifecycle(window);
	if (lifecycleResult == 77)
		result = result == 0 ? 77 : result;
	else
		result |= lifecycleResult;
	DestroyWindow(window);
	return result;
}
