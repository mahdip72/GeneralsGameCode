/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2026 TheSuperHackers
**
** Native WW3D GameEngineDevice facade.  This translation unit is deliberately
** product-neutral: title objects are handled by the paired title adapter,
** while this file validates WW3D handles and forwards synchronous POD commands
** to the one published NativeW3D2 aggregate.
*/

#include "Utility/CppMacros.h"
#include "Renderer/LegacyColorPacking.h"
#include "Renderer/RenderGameClient.h"
#include "Renderer/RenderGameClientNative.h"
#include "Renderer/WindowPresentation.h"
#include "formconv.h"
#include "nativew3d2.h"
#include "nativew3dbufferowner.h"
#include "dx8indexbuffer.h"
#include "dx8vertexbuffer.h"
#include "surfaceclass.h"
#include "texture.h"
#include "WWMath/matrix3d.h"
#include "WWMath/matrix4.h"
#include "WWMath/vector4.h"

#include <float.h>
#include <limits.h>
#include <math.h>
#include <mutex>
#include <new>
#include <string.h>
#include <atomic>

#if defined(_WIN64)

namespace
{

using namespace rts::render;

std::atomic<RenderResult> g_native_adapter_failure(RENDER_RESULT_OK);

struct NativeGameRendererState
{
	NativeGameRendererState() : aggregate(0), descriptorValid(false),
		transitionInProgress(false), window(0), width(0), height(0),
		lite(false), enableVsync(false), bitDepth(32), windowed(true),
		ownerThread(0), presentation()
	{
	}

	NativeW3D2 *aggregate;
	bool descriptorValid;
	bool transitionInProgress;
	void *window;
	unsigned int width;
	unsigned int height;
	bool lite;
	bool enableVsync;
	int bitDepth;
	bool windowed;
	unsigned long ownerThread;
	WindowPresentationState presentation;
};

std::mutex g_bootstrap_mutex;
NativeGameRendererState g_renderer_state;

// The native D3D11 product currently exposes one logical renderer.  The
// bootstrap may use WARP when hardware creation fails, so do not publish
// hardware or feature-level claims that this facade cannot query. Display
// modes below come from the primary display's OS mode list; they are not a
// claim that a particular physical adapter owns that monitor.
const char kNativeDeviceName[] = "Native D3D11 renderer";
const char kNativeDriverName[] = "d3d11.dll";
const char kNativeDriverVersion[] = "Unknown";
const char kNativeDevicePlatform[] = "Native x64";
const char kNativeHardwareName[] = "Unknown";
const int kNativeFallbackResolutionWidth = 640;
const int kNativeFallbackResolutionHeight = 480;
const int kNativeFallbackResolutionBitDepth = 32;

void CopyNativeDeviceString(char *destination, const char *source)
{
	if (destination == 0)
		return;
	if (source == 0)
		source = "";
	size_t length = strlen(source);
	if (length >= GAME_RENDER_DEVICE_STRING_CAPACITY)
		length = GAME_RENDER_DEVICE_STRING_CAPACITY - 1;
	memcpy(destination, source, length);
	destination[length] = '\0';
}

void FillNativeDeviceDescription(GameRenderDeviceDesc *description)
{
	if (description == 0)
		return;
	memset(description, 0, sizeof(*description));
	CopyNativeDeviceString(description->deviceName, kNativeDeviceName);
	CopyNativeDeviceString(description->devicePlatform,
		kNativeDevicePlatform);
	CopyNativeDeviceString(description->driverName, kNativeDriverName);
	CopyNativeDeviceString(description->driverVersion,
		kNativeDriverVersion);
	CopyNativeDeviceString(description->hardwareName, kNativeHardwareName);
	description->adapterIndex = 0;
}

unsigned int EnumerateNativeDisplayModes(GameRenderResolutionDesc *resolutions,
	unsigned int resolutionCapacity)
{
	unsigned int availableCount = 0;
	DEVMODE mode;
	ZeroMemory(&mode, sizeof(mode));
	mode.dmSize = sizeof(mode);
	for (DWORD modeIndex = 0;
		EnumDisplaySettings(0, modeIndex, &mode) != FALSE; ++modeIndex)
	{
		if (mode.dmPelsWidth != 0 && mode.dmPelsHeight != 0 &&
			mode.dmBitsPerPel >= 32)
		{
			if (resolutions != 0 && availableCount < resolutionCapacity)
			{
				GameRenderResolutionDesc &resolution =
					resolutions[availableCount];
				resolution.width = static_cast<int>(mode.dmPelsWidth);
				resolution.height = static_cast<int>(mode.dmPelsHeight);
				resolution.bitDepth = 32;
				resolution.refreshRate = (mode.dmFields & DM_DISPLAYFREQUENCY) != 0 ?
					static_cast<int>(mode.dmDisplayFrequency) : 0;
			}
			++availableCount;
		}
		ZeroMemory(&mode, sizeof(mode));
		mode.dmSize = sizeof(mode);
	}
	if (availableCount == 0)
	{
		if (resolutions != 0 && resolutionCapacity != 0)
		{
			resolutions[0].width = kNativeFallbackResolutionWidth;
			resolutions[0].height = kNativeFallbackResolutionHeight;
			resolutions[0].bitDepth = kNativeFallbackResolutionBitDepth;
			resolutions[0].refreshRate = 0;
		}
		availableCount = 1;
	}
	return availableCount;
}

bool IsFiniteFloat(float value)
{
	return value == value && value <= FLT_MAX && value >= -FLT_MAX;
}

bool IsFiniteColor(const RenderFloat4 &color)
{
	return IsFiniteFloat(color.x) && IsFiniteFloat(color.y) &&
		IsFiniteFloat(color.z) && IsFiniteFloat(color.w);
}

bool IsReady(IGameRenderClientNativeOwner *owner)
{
	return owner != 0 && owner->IsInitialized() && owner->IsOperational();
}

RenderResult Fail(IGameRenderClientNativeOwner *owner, RenderResult result)
{
	if (result != RENDER_RESULT_OK)
	{
		g_native_adapter_failure.store(result, std::memory_order_release);
		if (owner != 0)
			owner->RecordGameFailure(result);
	}
	return result;
}

void InitializeCommand(GameRenderCommand *command,
	GameRenderCommandType type)
{
	memset(command, 0, sizeof(*command));
	command->type = type;
}

GameRenderHandle ToGameHandle(const NativeW3DTextureHandle &handle)
{
	GameRenderHandle result = { 0, 0 };
	if (handle.isValid())
	{
		result.index = handle.resource.index();
		result.generation = handle.resource.generation();
	}
	return result;
}

GameRenderHandle ToGameHandle(const GpuHandle &handle)
{
	GameRenderHandle result = { 0, 0 };
	if (handle.isValid())
	{
		result.index = handle.index();
		result.generation = handle.generation();
	}
	return result;
}

bool CheckedMultiply(size_t left, size_t right, size_t *result)
{
	if (result == 0 || (left != 0 && right > static_cast<size_t>(-1) / left))
		return false;
	*result = left * right;
	return true;
}

bool ValidTransformSlot(GameRenderTransformSlot slot)
{
	return slot >= GAME_TRANSFORM_WORLD && slot <= GAME_TRANSFORM_TEXTURE7;
}

void CopyMatrix(const Matrix3D &source, RenderMatrix4 *destination)
{
	for (unsigned int row = 0; row < 3; ++row)
	{
		for (unsigned int column = 0; column < 4; ++column)
			destination->values[row * 4 + column] = source[row][column];
	}
	destination->values[12] = 0.0f;
	destination->values[13] = 0.0f;
	destination->values[14] = 0.0f;
	destination->values[15] = 1.0f;
}

void CopyMatrix(const Matrix4x4 &source, RenderMatrix4 *destination)
{
	for (unsigned int row = 0; row < 4; ++row)
	{
		for (unsigned int column = 0; column < 4; ++column)
			destination->values[row * 4 + column] = source[row][column];
	}
}

void CopyMatrixToLegacy(const RenderMatrix4 &source, void *destination)
{
	memcpy(destination, source.values, sizeof(source.values));
}

bool IsFiniteMatrix(const RenderMatrix4 &matrix)
{
	for (unsigned int index = 0; index < 16; ++index)
	{
		if (!IsFiniteFloat(matrix.values[index]))
			return false;
	}
	return true;
}

RenderResult DispatchCommand(IGameRenderClientNativeOwner *owner,
	const GameRenderCommand &command)
{
	if (!IsReady(owner))
		return Fail(owner, RENDER_RESULT_INVALID_ARGUMENT);
	return Fail(owner, owner->ExecuteGameRenderCommand(command));
}

RenderResult Dispatch(const GameRenderCommand &command)
{
	NativeGameRenderOwnerScope scope;
	return DispatchCommand(scope.Get(), command);
}

IGameRenderClientNativeOwner *PinnedOwner(
	NativeGameRenderOwnerScope *scope)
{
	return scope == 0 ? 0 : scope->Get();
}

bool IsValidGameColor(const GameRenderColor &color)
{
	return IsFiniteFloat(color.red) && IsFiniteFloat(color.green) &&
		IsFiniteFloat(color.blue) && IsFiniteFloat(color.alpha);
}

void SetClearCommandFields(GameRenderCommand *command, bool clear,
	bool clearz, const GameRenderColor &color, float destinationAlpha)
{
	command->value0 = (clear ? RENDER_CLEAR_COLOR : 0U) |
		(clearz ? RENDER_CLEAR_DEPTH : 0U);
	command->value1 = 0;
	command->float0 = color.red;
	command->float1 = color.green;
	command->float2 = color.blue;
	command->float3 = destinationAlpha;
	command->float4 = 1.0f;
}

bool ToRenderTopology(GamePrimitiveTopology topology,
	RenderPrimitiveTopology *translated)
{
	if (translated == 0)
		return false;
	switch (topology)
	{
	case GAME_PRIMITIVE_TRIANGLE_LIST:
		*translated = RENDER_PRIMITIVE_TRIANGLE_LIST;
		return true;
	case GAME_PRIMITIVE_TRIANGLE_STRIP:
		*translated = RENDER_PRIMITIVE_TRIANGLE_STRIP;
		return true;
	case GAME_PRIMITIVE_LINE_LIST:
		*translated = RENDER_PRIMITIVE_LINE_LIST;
		return true;
	case GAME_PRIMITIVE_LINE_STRIP:
		*translated = RENDER_PRIMITIVE_LINE_STRIP;
		return true;
	case GAME_PRIMITIVE_POINT_LIST:
	default:
		return false;
	}
}

bool GetMonitorRect(HWND window, RECT *rect)
{
	if (window == 0 || rect == 0)
		return false;
	HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
	MONITORINFO info;
	ZeroMemory(&info, sizeof(info));
	info.cbSize = sizeof(info);
	return monitor != 0 && GetMonitorInfo(monitor, &info) != FALSE &&
		IsValidWindowPresentationRect(info.rcMonitor) &&
		(*rect = info.rcMonitor, true);
}

void ResetRendererStateLocked()
{
	g_renderer_state.aggregate = 0;
	g_renderer_state.descriptorValid = false;
	g_renderer_state.transitionInProgress = false;
	g_renderer_state.window = 0;
	g_renderer_state.width = 0;
	g_renderer_state.height = 0;
	g_renderer_state.lite = false;
	g_renderer_state.enableVsync = false;
	g_renderer_state.bitDepth = 32;
	g_renderer_state.windowed = true;
	g_renderer_state.ownerThread = 0;
	g_renderer_state.presentation.clear();
	rts::render::GameRenderer_IsWindowed = true;
}

bool IsRendererOwnerThreadLocked()
{
	return g_renderer_state.ownerThread != 0 &&
		g_renderer_state.ownerThread ==
			static_cast<unsigned long>(GetCurrentThreadId());
}

RenderResult ReadRendererState(int *width, int *height, int *bitDepth,
	bool *windowed)
{
	if (width == 0 || height == 0 || bitDepth == 0 || windowed == 0)
		return RENDER_RESULT_INVALID_ARGUMENT;
	std::lock_guard<std::mutex> lock(g_bootstrap_mutex);
	if (g_renderer_state.aggregate == 0 ||
		!IsRendererOwnerThreadLocked())
		return Fail(0, RENDER_RESULT_INVALID_ARGUMENT);
	*width = static_cast<int>(g_renderer_state.width);
	*height = static_cast<int>(g_renderer_state.height);
	*bitDepth = g_renderer_state.bitDepth;
	*windowed = g_renderer_state.windowed;
	return RENDER_RESULT_OK;
}

RenderResult ApplyPresentation(HWND window, bool windowed,
	WindowPresentationState *state)
{
	if (window == 0 || state == 0)
		return RENDER_RESULT_INVALID_ARGUMENT;
	if (windowed)
	{
		if (!state->valid)
			return RENDER_RESULT_OK;
		return RestoreWindowPresentationSnapshot(window, state, true) ?
			RENDER_RESULT_OK : RENDER_RESULT_FAILED;
	}
	RECT monitorRect;
	if (!GetMonitorRect(window, &monitorRect))
		return RENDER_RESULT_FAILED;
	return ApplyBorderlessWindow(window, monitorRect, state) ?
		RENDER_RESULT_OK : RENDER_RESULT_FAILED;
}

} // anonymous namespace

namespace rts
{
namespace render
{

bool GameRenderer_IsWindowed = true;
int GameRenderer_PreserveFPU = 0;

RenderResult InitializeGameRenderer(void *window, unsigned int width,
	unsigned int height, bool lite, bool enableVsync)
{
	if (window == 0 || width == 0 || height == 0)
		return Fail(0, RENDER_RESULT_INVALID_ARGUMENT);

	NativeW3D2 *candidate = 0;
	{
		std::lock_guard<std::mutex> lock(g_bootstrap_mutex);
		if (g_renderer_state.transitionInProgress)
			return Fail(0, RENDER_RESULT_INVALID_ARGUMENT);
		if (g_renderer_state.aggregate != 0)
		{
			if (!IsRendererOwnerThreadLocked() ||
				!g_renderer_state.aggregate->IsInitialized() ||
				!g_renderer_state.aggregate->IsOperational() ||
				GetGameRenderClientNativeOwner() != g_renderer_state.aggregate)
				return Fail(0, RENDER_RESULT_INVALID_ARGUMENT);
			const bool same = g_renderer_state.window == window &&
				g_renderer_state.width == width &&
				g_renderer_state.height == height &&
				g_renderer_state.lite == lite &&
				g_renderer_state.enableVsync == enableVsync;
			return same ? RENDER_RESULT_OK :
				Fail(0, RENDER_RESULT_INVALID_ARGUMENT);
		}
		if (GetGameRenderClientNativeOwner() != 0)
			return Fail(0, RENDER_RESULT_INVALID_ARGUMENT);
		try
		{
			candidate = new NativeW3D2();
		}
		catch (const std::bad_alloc &)
		{
			return Fail(0, RENDER_RESULT_OUT_OF_MEMORY);
		}
		catch (...)
		{
			return Fail(0, RENDER_RESULT_FAILED);
		}
		g_renderer_state.transitionInProgress = true;
	}

	NativeW3DRendererDescriptor descriptor;
	descriptor.width = width;
	descriptor.height = height;
	descriptor.adapterIndex = UINT_MAX;
	descriptor.enableDebugLayer = false;
	descriptor.enableVsync = enableVsync;
	descriptor.allowSoftwareFallback = true;
	const RenderResult result = candidate->Initialize(window, descriptor);
	if (result != RENDER_RESULT_OK ||
		GetGameRenderClientNativeOwner() != candidate)
	{
		if (result == RENDER_RESULT_OK)
			candidate->Shutdown();
		delete candidate;
		std::lock_guard<std::mutex> lock(g_bootstrap_mutex);
		g_renderer_state.transitionInProgress = false;
		return Fail(0, result == RENDER_RESULT_OK ?
			RENDER_RESULT_FAILED : result);
	}

	{
		std::lock_guard<std::mutex> lock(g_bootstrap_mutex);
		g_renderer_state.aggregate = candidate;
		g_renderer_state.descriptorValid = true;
		g_renderer_state.transitionInProgress = false;
		g_renderer_state.window = window;
		g_renderer_state.width = width;
		g_renderer_state.height = height;
		g_renderer_state.lite = lite;
		g_renderer_state.enableVsync = enableVsync;
		g_renderer_state.bitDepth = 32;
		g_renderer_state.windowed = true;
		g_renderer_state.ownerThread =
			static_cast<unsigned long>(GetCurrentThreadId());
		g_renderer_state.presentation.clear();
		GameRenderer_IsWindowed = true;
	}
	return RENDER_RESULT_OK;
}

RenderResult ShutdownGameRenderer()
{
	NativeW3D2 *aggregate = 0;
	{
		std::lock_guard<std::mutex> lock(g_bootstrap_mutex);
		if (g_renderer_state.aggregate == 0)
			return RENDER_RESULT_OK;
		if (g_renderer_state.transitionInProgress ||
			!IsRendererOwnerThreadLocked())
			return Fail(0, RENDER_RESULT_INVALID_ARGUMENT);
		g_renderer_state.transitionInProgress = true;
		aggregate = g_renderer_state.aggregate;
	}

	// Shutdown owns publication removal internally.  Do not pin the owner or
	// hold the bootstrap mutex while it acquires its lifecycle gate.
	const RenderResult result = aggregate->Shutdown();
	if (result != RENDER_RESULT_OK)
	{
		std::lock_guard<std::mutex> lock(g_bootstrap_mutex);
		g_renderer_state.transitionInProgress = false;
		return Fail(aggregate, result);
	}
	{
		std::lock_guard<std::mutex> lock(g_bootstrap_mutex);
		ResetRendererStateLocked();
	}
	delete aggregate;
	return RENDER_RESULT_OK;
}

void SetGameCleanupHook(GameRenderCleanupHook *hook)
{
	NativeGameRenderOwnerScope scope;
	IGameRenderClientNativeOwner *owner = scope.Get();
	if (!IsReady(owner))
	{
		Fail(owner, RENDER_RESULT_INVALID_ARGUMENT);
		return;
	}
	owner->SetGameCleanupHook(hook);
}

void SetGameCursorVisible(bool visible)
{
	// Native presentation deliberately uses the polygon cursor path.  The
	// compatibility facade therefore consumes these calls without retaining a
	// surface or inventing a second hardware-cursor resource.
	(void)visible;
}

void SetGameCursorProperties(int hotspotX, int hotspotY,
	SurfaceClass *surface)
{
	(void)hotspotX;
	(void)hotspotY;
	// Native presentation deliberately uses the polygon cursor path.  A null
	// surface is still an invalid caller contract; a non-null surface needs no
	// native retention.
	if (surface == 0)
		Fail(0, RENDER_RESULT_INVALID_ARGUMENT);
}

void SetGameCursorPosition(int x, int y)
{
	(void)x;
	(void)y;
}

RenderResult SetGameRenderDeviceByName(const char *name, int width,
	int height, int bitDepth, int windowed, bool resizeWindow)
{
	if (name == 0 || name[0] == '\0')
		return Fail(0, RENDER_RESULT_INVALID_ARGUMENT);
	unsigned int nameLength = 0;
	while (nameLength < 256U && name[nameLength] != '\0')
		++nameLength;
	if (nameLength == 256U)
		return Fail(0, RENDER_RESULT_INVALID_ARGUMENT);
	if (windowed != -1 && windowed != 0 && windowed != 1)
		return Fail(0, RENDER_RESULT_INVALID_ARGUMENT);
	if (strcmp(name, kNativeDeviceName) != 0)
		return Fail(0, RENDER_RESULT_INVALID_ARGUMENT);
	return SetGameRenderDeviceByIndex(0, width, height, bitDepth,
		windowed, resizeWindow, true, true);
}

RenderResult SetGameRenderDeviceByIndex(int device, int width, int height,
	int bitDepth, int windowed, bool resizeWindow, bool resetDevice,
	bool restoreAssets)
{
	if (device < 0 || (windowed != -1 && windowed != 0 && windowed != 1))
		return Fail(0, RENDER_RESULT_INVALID_ARGUMENT);
	if (device != 0 || (bitDepth > 0 && bitDepth != 16 && bitDepth != 32))
		return Fail(0, RENDER_RESULT_INVALID_ARGUMENT);
	// The native owner already owns the selected D3D11 device.  Its existing
	// resolution transaction performs the required resource release/resize/
	// reacquire work, so do not submit a device-selection command that the
	// aggregate cannot truthfully execute.  D3D11 presents a 32-bit format even
	// when the legacy caller requested the 16-bit fallback.
	(void)resetDevice;
	(void)restoreAssets;
	const int effectiveBitDepth = bitDepth > 0 ? 32 : bitDepth;
	return SetGameRendererResolution(width, height, effectiveBitDepth,
		windowed, resizeWindow);
}

RenderResult SetAnyGameRenderDevice()
{
	NativeGameRenderOwnerScope scope;
	IGameRenderClientNativeOwner *owner = scope.Get();
	if (!IsReady(owner))
		return Fail(owner, RENDER_RESULT_INVALID_ARGUMENT);
	// There is exactly one published logical native renderer. Selecting any
	// renderer is therefore a successful no-op, not an invitation to invent a
	// second physical adapter.
	return RENDER_RESULT_OK;
}

RenderResult SetNextGameRenderDevice()
{
	NativeGameRenderOwnerScope scope;
	IGameRenderClientNativeOwner *owner = scope.Get();
	if (!IsReady(owner))
		return Fail(owner, RENDER_RESULT_INVALID_ARGUMENT);
	// The single logical renderer wraps to itself. Keep this a successful
	// no-op while refusing to claim that an alternate device was selected.
	return RENDER_RESULT_OK;
}

int GetGameRenderDeviceIndex()
{
	NativeGameRenderOwnerScope scope;
	IGameRenderClientNativeOwner *owner = scope.Get();
	return IsReady(owner) ? 0 : -1;
}

RenderResult GetGameRenderDeviceName(int device, char *name,
	unsigned int nameCapacity)
{
	if (name == 0 || nameCapacity == 0U || device < 0)
		return Fail(0, RENDER_RESULT_INVALID_ARGUMENT);
	name[0] = '\0';
	NativeGameRenderOwnerScope scope;
	IGameRenderClientNativeOwner *owner = scope.Get();
	if (!IsReady(owner) || device != 0)
		return Fail(owner, RENDER_RESULT_INVALID_ARGUMENT);
	const size_t sourceLength = strlen(kNativeDeviceName);
	const size_t copyLength = sourceLength < nameCapacity - 1U ?
		sourceLength : nameCapacity - 1U;
	memcpy(name, kNativeDeviceName, copyLength);
	name[copyLength] = '\0';
	return RENDER_RESULT_OK;
}

int GetGameRenderDeviceCount()
{
	NativeGameRenderOwnerScope scope;
	IGameRenderClientNativeOwner *owner = scope.Get();
	return IsReady(owner) ? 1 : 0;
}

RenderResult GetGameRenderDeviceDesc(int device, GameRenderDeviceDesc *desc,
	GameRenderResolutionDesc *resolutions, unsigned int resolutionCapacity,
	unsigned int *resolutionCount)
{
	if (resolutionCount != 0)
		*resolutionCount = 0;
	if (device < 0 || desc == 0 || (resolutionCapacity != 0U &&
		resolutions == 0) || resolutionCount == 0)
		return Fail(0, RENDER_RESULT_INVALID_ARGUMENT);
	memset(desc, 0, sizeof(*desc));
	NativeGameRenderOwnerScope scope;
	IGameRenderClientNativeOwner *owner = scope.Get();
	if (!IsReady(owner) || device != 0)
		return Fail(owner, RENDER_RESULT_INVALID_ARGUMENT);
	FillNativeDeviceDescription(desc);
	const unsigned int availableCount = EnumerateNativeDisplayModes(
		resolutions, resolutionCapacity);
	*resolutionCount = resolutionCapacity == 0U ||
		availableCount < resolutionCapacity ? availableCount :
		resolutionCapacity;
	return RENDER_RESULT_OK;
}

RenderResult ToggleGameRendererWindowed()
{
	int width = 0;
	int height = 0;
	int bitDepth = 0;
	bool windowed = false;
	const RenderResult stateResult = ReadRendererState(&width, &height,
		&bitDepth, &windowed);
	if (stateResult != RENDER_RESULT_OK)
		return stateResult;
	return SetGameRendererResolution(width, height, bitDepth,
		windowed ? 0 : 1, true);
}

RenderResult GetGameRendererResolution(int *width, int *height,
	int *bitDepth, bool *windowed)
{
	return ReadRendererState(width, height, bitDepth, windowed);
}

RenderResult GetGameRendererTargetResolution(int *width, int *height,
	int *bitDepth, bool *windowed)
{
	return ReadRendererState(width, height, bitDepth, windowed);
}

RenderResult SetGameRendererResolution(int width, int height, int bitDepth,
	int windowed, bool resizeWindow)
{
	if (windowed < -1 || windowed > 1)
		return Fail(0, RENDER_RESULT_INVALID_ARGUMENT);

	NativeW3D2 *aggregate = 0;
	NativeGameRendererState previous;
	bool targetWindowed = true;
	{
		std::lock_guard<std::mutex> lock(g_bootstrap_mutex);
		if (g_renderer_state.aggregate == 0 ||
			g_renderer_state.transitionInProgress ||
			!IsRendererOwnerThreadLocked())
			return Fail(0, RENDER_RESULT_INVALID_ARGUMENT);
		aggregate = g_renderer_state.aggregate;
		previous = g_renderer_state;
		if (width <= 0)
			width = static_cast<int>(previous.width);
		if (height <= 0)
			height = static_cast<int>(previous.height);
		if (bitDepth <= 0)
			bitDepth = previous.bitDepth;
		if (width <= 0 || height <= 0 ||
			(bitDepth != 16 && bitDepth != 32))
			return Fail(0, RENDER_RESULT_INVALID_ARGUMENT);
		// The native swap chain is an always-32-bit D3D11 target.  Accept the
		// historical 16-bit request as a fallback input, but publish the actual
		// format rather than leaving a false display depth in bootstrap state.
		bitDepth = 32;
		targetWindowed = windowed < 0 ? previous.windowed : windowed != 0;
		g_renderer_state.transitionInProgress = true;
	}

	const HWND nativeWindow = static_cast<HWND>(previous.window);
	WindowPresentationState nextPresentation = previous.presentation;
	const bool transitionPresentation = previous.windowed != targetWindowed;
	RenderResult result = RENDER_RESULT_OK;
	if (transitionPresentation)
	{
		result = ApplyPresentation(nativeWindow, targetWindowed,
			&nextPresentation);
	}
	if (result == RENDER_RESULT_OK)
	{
		GameRenderCommand command;
		InitializeCommand(&command, GAME_RENDER_COMMAND_SET_RESOLUTION);
		command.value0 = static_cast<unsigned int>(width);
		command.value1 = static_cast<unsigned int>(height);
		command.signedValue0 = bitDepth;
		command.value2 = targetWindowed ? 1U : 0U;
		command.value3 = resizeWindow ? 1U : 0U;
		NativeGameRenderOwnerScope scope;
		IGameRenderClientNativeOwner *owner = scope.Get();
		if (owner != aggregate)
			result = RENDER_RESULT_INVALID_ARGUMENT;
		else
			// The owner is already pinned for this transaction. Calling the
			// public dispatch helper here would attempt to acquire the same
			// non-recursive lifecycle mutex a second time.
			result = Fail(owner, owner->ExecuteGameRenderCommand(command));
	}

	if (result != RENDER_RESULT_OK && transitionPresentation)
	{
		WindowPresentationState rollback = previous.presentation;
		RenderResult rollbackResult;
		if (previous.windowed)
		{
			rollbackResult = ApplyPresentation(nativeWindow, true, &rollback);
		}
		else
		{
			rollbackResult = ApplyPresentation(nativeWindow, false, &rollback);
		}
		if (rollbackResult != RENDER_RESULT_OK)
			result = RENDER_RESULT_FAILED;
	}

	{
		std::lock_guard<std::mutex> lock(g_bootstrap_mutex);
		if (g_renderer_state.aggregate == aggregate)
		{
			g_renderer_state.transitionInProgress = false;
			if (result == RENDER_RESULT_OK)
			{
				g_renderer_state.width = static_cast<unsigned int>(width);
				g_renderer_state.height = static_cast<unsigned int>(height);
				g_renderer_state.bitDepth = bitDepth;
				g_renderer_state.windowed = targetWindowed;
				g_renderer_state.presentation = nextPresentation;
				GameRenderer_IsWindowed = targetWindowed;
			}
		}
	}
	// The owner pin has ended before this point. The bootstrap transition gate
	// also permits a concurrent shutdown once the state lock is released, so do
	// not dereference the aggregate here when reporting the final result.
	return Fail(0, result);
}

RenderResult QueueGameBackBufferCapture(
	const RenderCaptureRequestDescriptor &descriptor,
	RenderCaptureHandle *handle)
{
	NativeGameRenderOwnerScope scope;
	IGameRenderClientNativeOwner *owner = scope.Get();
	if (!IsReady(owner))
		return Fail(owner, RENDER_RESULT_INVALID_ARGUMENT);
	return Fail(owner, owner->QueueGameBackBufferCapture(descriptor, handle));
}

unsigned int CancelGameBackBufferCaptures(void *consumer, RenderResult reason)
{
	NativeGameRenderOwnerScope scope;
	IGameRenderClientNativeOwner *owner = scope.Get();
	if (!IsReady(owner))
	{
		Fail(owner, RENDER_RESULT_INVALID_ARGUMENT);
		return 0;
	}
	if (reason == RENDER_RESULT_OK)
	{
		Fail(owner, RENDER_RESULT_INVALID_ARGUMENT);
		return 0;
	}
	return owner->CancelGameBackBufferCaptures(consumer, reason);
}

void RequestGameBackBufferCapture()
{
	NativeGameRenderOwnerScope scope;
	IGameRenderClientNativeOwner *owner = scope.Get();
	if (!IsReady(owner))
	{
		Fail(owner, RENDER_RESULT_INVALID_ARGUMENT);
		return;
	}
	owner->RequestGameBackBufferCapture();
}

} // namespace render
} // namespace rts

#endif // defined(_WIN64)
