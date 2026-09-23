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
			validModes[i],
			"valid MSAA policy is retained before device publication");
	}
	failures += !Check(rts::render::SetGameMSAAMode(1) ==
		rts::render::RENDER_RESULT_INVALID_ARGUMENT &&
		rts::render::GetGameMSAAMode() ==
		rts::render::GAME_RENDER_MULTISAMPLE_8X,
		"invalid MSAA policy is rejected without changing state");
	failures += !Check(rts::render::SetGameMSAAMode(
		rts::render::GAME_RENDER_MULTISAMPLE_4X) ==
		rts::render::RENDER_RESULT_OK,
		"logical MSAA policy selects 4x for native initialization");
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
	// Mirror W3DDisplay startup: the saved policy is published before Init,
	// then Set_Render_Device selects the final dimensions without recreating
	// the already initialized native device.
	MONITORINFO selectedMonitor = { sizeof(MONITORINFO) };
	failures += !Check(GetMonitorInfo(MonitorFromWindow(window,
		MONITOR_DEFAULTTOPRIMARY), &selectedMonitor) != FALSE,
		"read the window's selected monitor before startup resizing");
	const rts::render::RenderResult startupSelectionResult =
		rts::render::SetGameRenderDeviceByIndex(0, 640, 480, 32, 1, true,
		false, true);
	failures += !Check(startupSelectionResult ==
		rts::render::RENDER_RESULT_OK &&
		rts::render::GetGameMSAAMode() ==
		rts::render::GAME_RENDER_MULTISAMPLE_4X,
		"product startup ordering preserves the effective 4x D3D11 scene target");
	RECT clientRect = { 0 };
	failures += !Check(GetClientRect(window, &clientRect) &&
		clientRect.right == 640 && clientRect.bottom == 480,
		"windowed startup sizes the physical client to the selected render resolution");
	RECT positionedWindow = { 0 };
	failures += !Check(GetWindowRect(window, &positionedWindow) &&
		positionedWindow.left == (selectedMonitor.rcWork.left + selectedMonitor.rcWork.right -
			(positionedWindow.right - positionedWindow.left)) / 2 &&
		positionedWindow.top == (selectedMonitor.rcWork.top + selectedMonitor.rcWork.bottom -
			(positionedWindow.bottom - positionedWindow.top)) / 2,
		"resized window is centered in its selected monitor work area");

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
	failures += !Check(GetClientRect(window, &clientRect) &&
		clientRect.right == 800 && clientRect.bottom == 600,
		"same-mode windowed selection honors resizeWindow for the physical client");
	rts::render::RenderBackBufferInfo backBuffer;
	failures += !Check(rts::render::GetGameBackBufferInfo(&backBuffer) ==
		rts::render::RENDER_RESULT_OK && backBuffer.width == 800 &&
		backBuffer.height == 600,
		"windowed client and actual backbuffer agree after resizing");
	RECT originalWindowRect = { 0 };
	GetWindowRect(window, &originalWindowRect);
	const rts::render::GameRenderColor clearColor = { 0, 0, 0, 1 };
	failures += !Check(rts::render::BeginGameRender(true, true, clearColor, 1.0f) ==
		rts::render::RENDER_RESULT_OK, "open frame establishes a resize rejection boundary");
	failures += !Check(rts::render::SetGameRendererResolution(960, 720, 32, 1, true) ==
		rts::render::RENDER_RESULT_INVALID_ARGUMENT,
		"resolution transaction rejects resizing an open render frame");
	RECT rolledBackWindowRect = { 0 };
	failures += !Check(GetWindowRect(window, &rolledBackWindowRect) &&
		EqualRect(&originalWindowRect, &rolledBackWindowRect),
		"failed render resize restores the complete prior window rectangle");
	failures += !Check(rts::render::GetGameRendererResolution(&width, &height,
		&bitDepth, &windowed) == rts::render::RENDER_RESULT_OK &&
		width == 800 && height == 600 && windowed,
		"failed resize does not publish new renderer dimensions");
	failures += !Check(rts::render::GetGameBackBufferInfo(&backBuffer) ==
		rts::render::RENDER_RESULT_OK && backBuffer.width == 800 &&
		backBuffer.height == 600,
		"rejected open-frame resize preserves actual backbuffer dimensions");
	(void)rts::render::EndGameRender(false);
	failures += !Check(rts::render::SetGameRendererResolution(640, 480, 32, 1, false) ==
		rts::render::RENDER_RESULT_OK && GetClientRect(window, &clientRect) &&
		clientRect.right == 800 && clientRect.bottom == 600 &&
		rts::render::GetGameBackBufferInfo(&backBuffer) == rts::render::RENDER_RESULT_OK &&
		backBuffer.width == 640 && backBuffer.height == 480,
		"explicit nonresizing selection retains independent client and render sizes");
	failures += !Check(rts::render::SetGameRendererResolution(800, 600, 32, 1, true) ==
		rts::render::RENDER_RESULT_OK,
		"resizing selection restores the matched client and renderer");
	const int monitorWidth = selectedMonitor.rcMonitor.right - selectedMonitor.rcMonitor.left;
	const int monitorHeight = selectedMonitor.rcMonitor.bottom - selectedMonitor.rcMonitor.top;
	failures += !Check(rts::render::SetGameRendererResolution(monitorWidth,
		monitorHeight, 32, 1, true) == rts::render::RENDER_RESULT_OK,
		"windowed monitor-sized request preserves exact requested resolution");
	POINT fittedClientOrigin = { 0, 0 };
	failures += !Check(GetClientRect(window, &clientRect) &&
		clientRect.right == monitorWidth && clientRect.bottom == monitorHeight &&
		ClientToScreen(window, &fittedClientOrigin) &&
		fittedClientOrigin.x == selectedMonitor.rcMonitor.left &&
		fittedClientOrigin.y == selectedMonitor.rcMonitor.top,
		"monitor-sized client fits monitor bounds without clipping from centered decorations");
	failures += !Check(rts::render::SetGameRendererResolution(800, 600, 32, 1, true) ==
		rts::render::RENDER_RESULT_OK,
		"ordinary window resolution is restored after monitor-sized placement");
	failures += !Check(rts::render::GetGameMSAAMode() ==
		rts::render::GAME_RENDER_MULTISAMPLE_4X,
		"native resize preserves the effective 4x D3D11 scene target");

	const int fullscreenClientWidth = selectedMonitor.rcMonitor.right -
		selectedMonitor.rcMonitor.left;
	const int fullscreenClientHeight = selectedMonitor.rcMonitor.bottom -
		selectedMonitor.rcMonitor.top;
	failures += !Check(rts::render::SetGameRenderDeviceByName(deviceName,
		640, 480, 16, 0, true) == rts::render::RENDER_RESULT_OK,
		"valid logical device name routes through bootstrap presentation");
	POINT fullscreenClientOrigin = { 0, 0 };
	failures += !Check(GetClientRect(window, &clientRect) &&
		clientRect.right - clientRect.left == fullscreenClientWidth &&
		clientRect.bottom - clientRect.top == fullscreenClientHeight &&
		ClientToScreen(window, &fullscreenClientOrigin) &&
		fullscreenClientOrigin.x == selectedMonitor.rcMonitor.left &&
		fullscreenClientOrigin.y == selectedMonitor.rcMonitor.top,
		"borderless client exactly covers the selected monitor");
	failures += !Check(rts::render::GetGameBackBufferInfo(&backBuffer) ==
		rts::render::RENDER_RESULT_OK &&
		backBuffer.width == static_cast<unsigned int>(fullscreenClientWidth) &&
		backBuffer.height == static_cast<unsigned int>(fullscreenClientHeight),
		"fullscreen backbuffer matches the monitor client without stretching a mismatched request");
	failures += !Check(rts::render::GetGameRendererResolution(&width, &height,
		&bitDepth, &windowed) == rts::render::RENDER_RESULT_OK &&
		width == fullscreenClientWidth && height == fullscreenClientHeight &&
		bitDepth == 32 && !windowed,
		"native fullscreen publishes client-sized dimensions for display and mouse input");
	failures += !Check(rts::render::SetGameRenderDeviceByName(deviceName,
		640, 480, 16, 1, true) == rts::render::RENDER_RESULT_OK,
		"valid logical device name restores windowed presentation");
	failures += !Check(GetClientRect(window, &clientRect) &&
		clientRect.right - clientRect.left == 640 &&
		clientRect.bottom - clientRect.top == 480 &&
		rts::render::GetGameBackBufferInfo(&backBuffer) ==
			rts::render::RENDER_RESULT_OK && backBuffer.width == 640 &&
		backBuffer.height == 480 &&
		rts::render::GetGameRendererResolution(&width, &height, &bitDepth,
			&windowed) == rts::render::RENDER_RESULT_OK &&
		width == 640 && height == 480 && windowed,
		"windowed resolution restores the exact requested client and backbuffer dimensions");
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
	// Match the native product's per-monitor awareness so client measurements
	// exercise physical pixels on scaled desktops rather than DPI virtualization.
	typedef HANDLE (WINAPI *SetThreadDpiAwarenessContextProc)(HANDLE);
	const SetThreadDpiAwarenessContextProc setThreadDpiAwarenessContext =
		reinterpret_cast<SetThreadDpiAwarenessContextProc>(GetProcAddress(
			GetModuleHandleW(L"user32.dll"), "SetThreadDpiAwarenessContext"));
	const HANDLE previousDpiContext = setThreadDpiAwarenessContext ?
		setThreadDpiAwarenessContext(reinterpret_cast<HANDLE>(-4)) : 0;
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
	if (previousDpiContext)
		setThreadDpiAwarenessContext(previousDpiContext);
	return result;
}
