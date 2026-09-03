#include "Utility/CppMacros.h"
#include "Renderer/RenderGameClient.h"
#include "Renderer/RenderGameClientNative.h"
#include "camera.h"
#include "dx8renderer.h"

#include <cfenv>
#include <cmath>
#include <cstdio>
#include <limits>
#include <windows.h>

namespace
{
int Check(bool condition, const char *message)
{
	if (condition)
		return 0;
	std::fprintf(stderr, "FAIL: %s\n", message);
	return 1;
}

const wchar_t *kWindowClassName = L"GeneralsGameCodeNativeTitleCameraAdapterTest";

LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
	return DefWindowProcW(window, message, wparam, lparam);
}

HWND CreateHiddenWindow()
{
	WNDCLASSEXW windowClass = {};
	windowClass.cbSize = sizeof(windowClass);
	windowClass.lpfnWndProc = WindowProcedure;
	windowClass.hInstance = GetModuleHandleW(0);
	windowClass.lpszClassName = kWindowClassName;
	RegisterClassExW(&windowClass);
	return CreateWindowExW(0, kWindowClassName, L"Native title camera adapter",
		WS_OVERLAPPEDWINDOW, 0, 0, 101, 79, 0, 0, windowClass.hInstance, 0);
}

// This sink observes the actual paired title adapter's complete output, not a
// copy of its conversion algorithm. Keeping malformed RED outputs here also
// prevents an invalid viewport from reaching the real GPU. The bootstrap still
// supplies the actual target-resolution query used by production.
class CameraOutputSink : public rts::render::IGameRenderClientNativeOwner
{
public:
	CameraOutputSink() : snapshots(0), viewportCount(0), commandCount(0), failures(0),
		lastFailure(rts::render::RENDER_RESULT_OK) {}
	bool IsInitialized() const override { return true; }
	bool IsOperational() const override { return true; }
	rts::render::GameRenderTargetKind ActiveRenderTargetKind() const override
	{
		return rts::render::GAME_RENDER_TARGET_BACK_BUFFER;
	}
	rts::render::RenderResult SetGameRenderCameraSnapshot(
		const rts::render::GameCameraSnapshot &value) override
	{
		++snapshots;
		snapshot = value;
		return rts::render::RENDER_RESULT_OK;
	}
	rts::render::RenderResult SetGameViewport(
		const rts::render::RenderViewport &value) override
	{
		++viewportCount;
		viewport = value;
		return rts::render::RENDER_RESULT_OK;
	}
	rts::render::RenderResult ExecuteGameRenderCommand(
		const rts::render::GameRenderCommand &command) override
	{
		// Observe the actual Apply command path without forwarding RED payloads
		// to the GPU or retaining pointers to its synchronous matrix payloads.
		if (commandCount < 2)
		{
			commandTypes[commandCount] = command.type;
			commandSlots[commandCount] = command.value0;
			commandMatrixPresent[commandCount] =
				(command.type == rts::render::GAME_RENDER_COMMAND_SET_TRANSFORM ||
				 command.type == rts::render::GAME_RENDER_COMMAND_SET_PROJECTION_WITH_Z_BIAS) &&
				command.input != 0 && command.inputBytes == sizeof(rts::render::RenderMatrix4);
			if (commandMatrixPresent[commandCount])
				commandMatrices[commandCount] =
					*static_cast<const rts::render::RenderMatrix4 *>(command.input);
		}
		++commandCount;
		return rts::render::RENDER_RESULT_OK;
	}
	void RecordGameFailure(rts::render::RenderResult value) override
	{
		++failures;
		lastFailure = value;
	}
	void ResetObservations()
	{
		snapshots = 0;
		viewportCount = 0;
		commandCount = 0;
		failures = 0;
		lastFailure = rts::render::RENDER_RESULT_OK;
	}
	unsigned int snapshots;
	unsigned int viewportCount;
	unsigned int commandCount;
	unsigned int failures;
	rts::render::RenderResult lastFailure;
	rts::render::GameCameraSnapshot snapshot;
	rts::render::RenderViewport viewport;
	rts::render::GameRenderCommandType commandTypes[2];
	unsigned int commandSlots[2];
	bool commandMatrixPresent[2];
	rts::render::RenderMatrix4 commandMatrices[2];
};

struct CameraInputs
{
	CameraInputs() : minX(0.125f), minY(0.25f), maxX(0.875f), maxY(0.75f),
		minimumDepth(0.25f), maximumDepth(0.75f), zNear(1.0f), zFar(3.0f),
		projectionType(CameraClass::PERSPECTIVE) {}
	float minX;
	float minY;
	float maxX;
	float maxY;
	float minimumDepth;
	float maximumDepth;
	float zNear;
	float zFar;
	CameraClass::ProjectionType projectionType;
};

void ConfigureCamera(CameraClass *camera, const CameraInputs &input)
{
	camera->Set_Projection_Type(input.projectionType);
	camera->Set_Viewport(Vector2(input.minX, input.minY), Vector2(input.maxX, input.maxY));
	camera->Set_Depth_Range(input.minimumDepth, input.maximumDepth);
	camera->Set_Clip_Planes(input.zNear, input.zFar);
}

int CheckRejectedCamera(CameraOutputSink *sink, CameraClass *priorCamera,
	const CameraInputs &input, const char *message)
{
	CameraClass camera;
	ConfigureCamera(&camera, input);
	TheDX8MeshRenderer.Set_Camera(priorCamera);
	sink->ResetObservations();
	std::feclearexcept(FE_ALL_EXCEPT);
	rts::render::SetGameRenderCamera(&camera);
	const int unsafeArithmetic = std::fetestexcept(FE_INVALID | FE_DIVBYZERO);
	return Check(sink->snapshots == 0 && sink->viewportCount == 0 &&
		sink->commandCount == 0 && sink->failures != 0 &&
		sink->lastFailure == rts::render::RENDER_RESULT_INVALID_ARGUMENT &&
		TheDX8MeshRenderer.Peek_Camera() == priorCamera && unsafeArithmetic == 0,
		message);
}

int TestCameraOutputs(CameraOutputSink *sink)
{
	int result = 0;
	CameraClass priorCamera;
	CameraClass camera;
	CameraInputs input;
	ConfigureCamera(&camera, input);
	sink->ResetObservations();
	rts::render::SetGameRenderCamera(&camera);
	result |= Check(sink->snapshots == 1 && sink->failures == 0 &&
		sink->snapshot.viewport.x == 12.0f && sink->snapshot.viewport.y == 19.0f &&
		sink->snapshot.viewport.width == 75.0f && sink->snapshot.viewport.height == 39.0f &&
		sink->snapshot.viewport.minimumDepth == 0.25f &&
		sink->snapshot.viewport.maximumDepth == 0.75f &&
		sink->snapshot.zNear == 1.0f && sink->snapshot.zFar == 3.0f &&
		TheDX8MeshRenderer.Peek_Camera() == &camera,
		"real title camera adapter preserves fractional pixel truncation and camera association");

	input.minX = input.minY = 0.0f;
	input.maxX = input.maxY = 1.0f;
	input.minimumDepth = input.maximumDepth = 0.5f;
	ConfigureCamera(&camera, input);
	sink->ResetObservations();
	rts::render::SetGameRenderCamera(&camera);
	result |= Check(sink->snapshots == 1 && sink->failures == 0 &&
		sink->snapshot.viewport.x == 0.0f && sink->snapshot.viewport.y == 0.0f &&
		sink->snapshot.viewport.width == 101.0f && sink->snapshot.viewport.height == 79.0f &&
		sink->snapshot.viewport.minimumDepth == 0.5f &&
		sink->snapshot.viewport.maximumDepth == 0.5f,
		"real title camera adapter accepts normalized boundaries and equal depth endpoints");

	// Physical clipping planes feed divisions inside CameraClass. Unlike the
	// viewport depth interval above, they cannot have equal endpoints.
	input = CameraInputs();
	input.zNear = input.zFar = 3.0f;
	result |= CheckRejectedCamera(sink, &priorCamera, input,
		"camera adapter rejects equal perspective clip planes before arithmetic or output");

	const float invalidNormalizedValues[] = {
		std::numeric_limits<float>::quiet_NaN(),
		std::numeric_limits<float>::infinity(),
		-std::numeric_limits<float>::infinity(), -0.001f, 1.001f
	};
	for (unsigned int field = 0; field != 4; ++field)
	{
		for (unsigned int value = 0; value != 5; ++value)
		{
			input = CameraInputs();
			float *fields[] = { &input.minX, &input.minY, &input.maxX, &input.maxY };
			*fields[field] = invalidNormalizedValues[value];
			result |= CheckRejectedCamera(sink, &priorCamera, input,
				"invalid normalized camera coordinates reject before conversion and snapshot publication");
		}
	}
	const float invalidRectangles[][4] = {
		{ 0.875f, 0.25f, 0.125f, 0.75f },
		{ 0.125f, 0.75f, 0.875f, 0.25f },
		{ 0.5f, 0.25f, 0.5f, 0.75f },
		{ 0.125f, 0.5f, 0.875f, 0.5f },
		{ 0.5f, 0.25f, 0.5001f, 0.75f },
		{ 0.125f, 0.5f, 0.875f, 0.5001f }
	};
	for (unsigned int rectangle = 0; rectangle != 6; ++rectangle)
	{
		input = CameraInputs();
		input.minX = invalidRectangles[rectangle][0];
		input.minY = invalidRectangles[rectangle][1];
		input.maxX = invalidRectangles[rectangle][2];
		input.maxY = invalidRectangles[rectangle][3];
		result |= CheckRejectedCamera(sink, &priorCamera, input,
			"reversed or zero-pixel-area camera rectangles reject without replacing the prior camera");
	}

	for (unsigned int field = 0; field != 4; ++field)
	{
		for (unsigned int value = 0; value != 4; ++value)
		{
			input = CameraInputs();
			float *fields[] = { &input.minimumDepth, &input.maximumDepth,
				&input.zNear, &input.zFar };
			*fields[field] = invalidNormalizedValues[value];
			result |= CheckRejectedCamera(sink, &priorCamera, input,
				"invalid camera depth metadata rejects before arithmetic and snapshot publication");
		}
	}
	input = CameraInputs();
	input.minimumDepth = 1.001f;
	result |= CheckRejectedCamera(sink, &priorCamera, input,
		"camera minimum depth above one rejects before snapshot publication");
	input = CameraInputs();
	input.maximumDepth = 1.001f;
	result |= CheckRejectedCamera(sink, &priorCamera, input,
		"camera maximum depth above one rejects before snapshot publication");
	input = CameraInputs();
	input.minimumDepth = 0.75f;
	input.maximumDepth = 0.25f;
	result |= CheckRejectedCamera(sink, &priorCamera, input,
		"reversed camera depth range rejects before snapshot publication");
	input = CameraInputs();
	input.zNear = 4.0f;
	result |= CheckRejectedCamera(sink, &priorCamera, input,
		"reversed camera clip planes reject before snapshot publication");
	TheDX8MeshRenderer.Set_Camera(0);
	return result;
}

int CheckRejectedCameraApply(CameraOutputSink *sink, CameraClass *priorCamera,
	const CameraInputs &input, const char *message)
{
	CameraClass camera;
	ConfigureCamera(&camera, input);
	TheDX8MeshRenderer.Set_Camera(priorCamera);
	sink->ResetObservations();
	std::feclearexcept(FE_ALL_EXCEPT);
	camera.Apply();
	const int unsafeArithmetic = std::fetestexcept(FE_INVALID | FE_DIVBYZERO);
	int result = Check(sink->viewportCount == 0 && sink->commandCount == 0 &&
		sink->snapshots == 0 && sink->failures != 0 &&
		sink->lastFailure == rts::render::RENDER_RESULT_INVALID_ARGUMENT &&
		TheDX8MeshRenderer.Peek_Camera() == priorCamera && unsafeArithmetic == 0,
		message);

	// Ordinary WW3D::Render calls this adapter after Apply. Check Apply above
	// first so this later, already-guarded boundary cannot mask earlier writes.
	sink->ResetObservations();
	std::feclearexcept(FE_ALL_EXCEPT);
	rts::render::SetGameRenderCamera(&camera);
	result |= Check(sink->viewportCount == 0 && sink->commandCount == 0 &&
		sink->snapshots == 0 && sink->failures != 0 &&
		sink->lastFailure == rts::render::RENDER_RESULT_INVALID_ARGUMENT &&
		TheDX8MeshRenderer.Peek_Camera() == priorCamera &&
		std::fetestexcept(FE_INVALID | FE_DIVBYZERO) == 0,
		"guarded camera adapter still rejects after the actual Apply call");
	return result;
}

int TestCameraApplyOutputs(CameraOutputSink *sink)
{
	using namespace rts::render;
	int result = 0;
	CameraClass priorCamera;
	CameraClass camera;
	CameraInputs input;
	ConfigureCamera(&camera, input);
	TheDX8MeshRenderer.Set_Camera(&priorCamera);
	sink->ResetObservations();
	camera.Apply();
#if defined(RTS_GENERALS)
	const bool projectionCommand = sink->commandCount == 2 &&
		sink->commandTypes[0] == GAME_RENDER_COMMAND_SET_TRANSFORM &&
		sink->commandSlots[0] == GAME_TRANSFORM_PROJECTION;
#elif defined(RTS_ZEROHOUR)
	const bool projectionCommand = sink->commandCount == 2 &&
		sink->commandTypes[0] == GAME_RENDER_COMMAND_SET_PROJECTION_WITH_Z_BIAS;
#else
#error The actual camera Apply fixture requires a paired title interface.
#endif
	result |= Check(sink->viewportCount == 1 && sink->commandCount == 2 &&
		sink->snapshots == 0 && sink->failures == 0 && projectionCommand &&
		sink->commandTypes[1] == GAME_RENDER_COMMAND_SET_TRANSFORM &&
		sink->commandSlots[1] == GAME_TRANSFORM_VIEW &&
		sink->viewport.x == 12.0f && sink->viewport.y == 19.0f &&
		sink->viewport.width == 75.0f && sink->viewport.height == 39.0f &&
		sink->viewport.minimumDepth == 0.25f && sink->viewport.maximumDepth == 0.75f &&
		TheDX8MeshRenderer.Peek_Camera() == &priorCamera,
		"actual CameraClass::Apply preserves literal pixels, title commands and prior mesh camera");

	// This finite counterexample also appears in the full corpus below. Keep
	// it separately labelled: its failure does not depend on NaN conversion.
	input = CameraInputs();
	input.minX = -0.001f;
	result |= CheckRejectedCameraApply(sink, &priorCamera, input,
		"actual Apply rejects finite negative normalized X before it truncates to pixel zero");

	// Keep this Apply corpus separate from the previously verified adapter
	// test so failure accounting identifies which actual boundary emitted.
	const float invalidNormalizedValues[] = {
		std::numeric_limits<float>::quiet_NaN(),
		std::numeric_limits<float>::infinity(),
		-std::numeric_limits<float>::infinity(), -0.001f, 1.001f
	};
	for (unsigned int field = 0; field != 4; ++field)
	{
		for (unsigned int value = 0; value != 5; ++value)
		{
			input = CameraInputs();
			float *fields[] = { &input.minX, &input.minY, &input.maxX, &input.maxY };
			*fields[field] = invalidNormalizedValues[value];
			result |= CheckRejectedCameraApply(sink, &priorCamera, input,
				"actual Apply rejects invalid normalized coordinates before viewport or transform emission");
		}
	}
	const float invalidRectangles[][4] = {
		{ 0.875f, 0.25f, 0.125f, 0.75f },
		{ 0.125f, 0.75f, 0.875f, 0.25f },
		{ 0.5f, 0.25f, 0.5f, 0.75f },
		{ 0.125f, 0.5f, 0.875f, 0.5f },
		{ 0.5f, 0.25f, 0.5001f, 0.75f },
		{ 0.125f, 0.5f, 0.875f, 0.5001f }
	};
	for (unsigned int rectangle = 0; rectangle != 6; ++rectangle)
	{
		input = CameraInputs();
		input.minX = invalidRectangles[rectangle][0];
		input.minY = invalidRectangles[rectangle][1];
		input.maxX = invalidRectangles[rectangle][2];
		input.maxY = invalidRectangles[rectangle][3];
		result |= CheckRejectedCameraApply(sink, &priorCamera, input,
			"actual Apply rejects reversed or zero-pixel rectangles before viewport or transform emission");
	}
	for (unsigned int field = 0; field != 4; ++field)
	{
		for (unsigned int value = 0; value != 4; ++value)
		{
			input = CameraInputs();
			float *fields[] = { &input.minimumDepth, &input.maximumDepth,
				&input.zNear, &input.zFar };
			*fields[field] = invalidNormalizedValues[value];
			result |= CheckRejectedCameraApply(sink, &priorCamera, input,
				"actual Apply rejects invalid depth metadata before arithmetic or state emission");
		}
	}
	input = CameraInputs();
	input.minimumDepth = 1.001f;
	result |= CheckRejectedCameraApply(sink, &priorCamera, input,
		"actual Apply rejects minimum depth above one before state emission");
	input = CameraInputs();
	input.maximumDepth = 1.001f;
	result |= CheckRejectedCameraApply(sink, &priorCamera, input,
		"actual Apply rejects maximum depth above one before state emission");
	input = CameraInputs();
	input.minimumDepth = 0.75f;
	input.maximumDepth = 0.25f;
	result |= CheckRejectedCameraApply(sink, &priorCamera, input,
		"actual Apply rejects reversed depth range before state emission");
	input = CameraInputs();
	input.zNear = 4.0f;
	result |= CheckRejectedCameraApply(sink, &priorCamera, input,
		"actual Apply rejects reversed clip planes before arithmetic or state emission");
	TheDX8MeshRenderer.Set_Camera(0);
	return result;
}

bool HasFiniteMatrix(const rts::render::RenderMatrix4 &matrix)
{
	for (unsigned int index = 0; index < 16; ++index)
	{
		if (!std::isfinite(matrix.values[index]))
			return false;
	}
	return true;
}

int TestCameraClipConstruction(CameraOutputSink *sink)
{
	using namespace rts::render;
	int result = 0;
	CameraClass priorCamera;
	CameraInputs input;
	input.zNear = input.zFar = 3.0f;
	result |= CheckRejectedCameraApply(sink, &priorCamera, input,
		"actual Apply rejects equal perspective clip planes before arithmetic or output");
	input.projectionType = CameraClass::ORTHO;
	result |= CheckRejectedCamera(sink, &priorCamera, input,
		"camera adapter rejects equal orthographic clip planes before arithmetic or output");
	result |= CheckRejectedCameraApply(sink, &priorCamera, input,
		"actual Apply rejects equal orthographic clip planes before arithmetic or output");

	input = CameraInputs();
	input.zNear = 0.0f;
	result |= CheckRejectedCamera(sink, &priorCamera, input,
		"camera adapter rejects zero perspective near plane before arithmetic or output");
	result |= CheckRejectedCameraApply(sink, &priorCamera, input,
		"actual Apply rejects zero perspective near plane before arithmetic or output");

	// The real orthographic matrix implementation permits near=0 when far>near.
	// Require finite outputs from both real call paths so preserving that valid
	// allowance cannot repeat the former metadata-only equal-clip test mistake.
	input.projectionType = CameraClass::ORTHO;
	CameraClass camera;
	ConfigureCamera(&camera, input);
	TheDX8MeshRenderer.Set_Camera(&priorCamera);
	sink->ResetObservations();
	std::feclearexcept(FE_ALL_EXCEPT);
	SetGameRenderCamera(&camera);
	result |= Check(sink->snapshots == 1 && sink->viewportCount == 0 &&
		sink->commandCount == 0 && sink->failures == 0 &&
		sink->snapshot.zNear == 0.0f && sink->snapshot.zFar == 3.0f &&
		HasFiniteMatrix(sink->snapshot.view) && HasFiniteMatrix(sink->snapshot.projection) &&
		sink->snapshot.viewport.x == 12.0f && sink->snapshot.viewport.y == 19.0f &&
		sink->snapshot.viewport.width == 75.0f && sink->snapshot.viewport.height == 39.0f &&
		TheDX8MeshRenderer.Peek_Camera() == &camera &&
		std::fetestexcept(FE_INVALID | FE_DIVBYZERO) == 0,
		"camera adapter accepts zero orthographic near plane with finite projection and no unsafe arithmetic");

	ConfigureCamera(&camera, input);
	TheDX8MeshRenderer.Set_Camera(&priorCamera);
	sink->ResetObservations();
	std::feclearexcept(FE_ALL_EXCEPT);
	camera.Apply();
#if defined(RTS_GENERALS)
	const bool projectionCommand = sink->commandCount == 2 &&
		sink->commandTypes[0] == GAME_RENDER_COMMAND_SET_TRANSFORM &&
		sink->commandSlots[0] == GAME_TRANSFORM_PROJECTION;
#elif defined(RTS_ZEROHOUR)
	const bool projectionCommand = sink->commandCount == 2 &&
		sink->commandTypes[0] == GAME_RENDER_COMMAND_SET_PROJECTION_WITH_Z_BIAS;
#else
#error The actual camera clip fixture requires a paired title interface.
#endif
	result |= Check(sink->viewportCount == 1 && sink->commandCount == 2 &&
		sink->snapshots == 0 && sink->failures == 0 && projectionCommand &&
		sink->commandTypes[1] == GAME_RENDER_COMMAND_SET_TRANSFORM &&
		sink->commandSlots[1] == GAME_TRANSFORM_VIEW &&
		sink->commandMatrixPresent[0] && sink->commandMatrixPresent[1] &&
		HasFiniteMatrix(sink->commandMatrices[0]) && HasFiniteMatrix(sink->commandMatrices[1]) &&
		sink->viewport.x == 12.0f && sink->viewport.y == 19.0f &&
		sink->viewport.width == 75.0f && sink->viewport.height == 39.0f &&
		TheDX8MeshRenderer.Peek_Camera() == &priorCamera &&
		std::fetestexcept(FE_INVALID | FE_DIVBYZERO) == 0,
		"actual Apply accepts zero orthographic near plane with finite transforms and unchanged mesh camera");
	TheDX8MeshRenderer.Set_Camera(0);
	return result;
}
}

int main()
{
	HWND window = CreateHiddenWindow();
	if (window == 0)
		return Check(false, "camera adapter fixture creates a hidden window");
	const rts::render::RenderResult initialized = rts::render::InitializeGameRenderer(
		window, 101, 79, false, false);
	if (initialized != rts::render::RENDER_RESULT_OK)
	{
		DestroyWindow(window);
		return initialized == rts::render::RENDER_RESULT_UNSUPPORTED ? 77 :
			Check(false, "camera adapter fixture initializes the real native bootstrap");
	}

	int result = 0;
	CameraOutputSink sink;
	rts::render::IGameRenderClientNativeOwner *actualOwner = 0;
	{
		rts::render::NativeGameRenderOwnerLifecycleScope lifecycle;
		if (lifecycle.IsAcquired())
		{
			actualOwner = lifecycle.Get();
			lifecycle.Publish(&sink);
		}
	}
	if (actualOwner != 0)
	{
		result |= TestCameraOutputs(&sink);
		result |= TestCameraApplyOutputs(&sink);
		result |= TestCameraClipConstruction(&sink);
		rts::render::NativeGameRenderOwnerLifecycleScope lifecycle;
		result |= Check(lifecycle.IsAcquired(), "camera fixture reacquires its owner publication gate");
		if (lifecycle.IsAcquired())
			lifecycle.Publish(actualOwner);
	}
	else
		result |= Check(false, "camera fixture observes the real native owner publication");
	result |= Check(rts::render::ShutdownGameRenderer() == rts::render::RENDER_RESULT_OK,
		"camera fixture restores and shuts down the real native owner");
	DestroyWindow(window);
	return result;
}
