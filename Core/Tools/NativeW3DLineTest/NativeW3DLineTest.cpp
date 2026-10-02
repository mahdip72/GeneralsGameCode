#include "Utility/CppMacros.h"
#include "nativew3dline.h"
#include "nativew3d2.h"
#include "nativeLine3dTransform.h"

#include <stdio.h>
#include <string.h>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <windows.h>

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

const wchar_t *kWindowClassName = L"GeneralsGameCodeNativeW3DLineTestWindow";

LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wparam,
	LPARAM lparam)
{
	return DefWindowProcW(window, message, wparam, lparam);
}

int RunNativeLine3DWorldTransformContract()
{
	using namespace rts::render;
	// Matrix3D's rows contain the three basis rows plus translation in column
	// three.  This 90-degree Z rotation with a non-zero translation catches both
	// the historical row-3 OOB read and an untransposed/native-layout copy.
	float matrixValues[12] = {
		0.0f, -1.0f, 0.0f, 11.0f,
		1.0f, 0.0f, 0.0f, 22.0f,
		0.0f, 0.0f, 1.0f, 33.0f
	};
	const Matrix3D transform(matrixValues);
	const float expected[16] = {
		0.0f, 1.0f, 0.0f, 0.0f,
		-1.0f, 0.0f, 0.0f, 0.0f,
		0.0f, 0.0f, 1.0f, 0.0f,
		11.0f, 22.0f, 33.0f, 1.0f
	};
	float world[16];
	int result = Check(!Build_Native_Line3D_World_Matrix(transform, 0),
		"native line transform rejects a missing destination");
	result |= Check(Build_Native_Line3D_World_Matrix(transform, world),
		"native line transform accepts an affine Matrix3D");
	for (unsigned int index = 0; index < 16; ++index)
	{
		if (world[index] != expected[index])
		{
			result |= Check(false,
				"native line transform transposes rotation and preserves translation");
			break;
		}
	}
	result |= Check(world[15] == 1.0f,
		"native line transform writes an explicit homogeneous one");
	return result;
}

int RunNativeLine3DSubmitterGateContract()
{
	using namespace rts::render;
	NativeLine3DRenderContext context(0, 0);
	Set_Native_Line3D_Submitter(&context);
	std::mutex conditionMutex;
	std::condition_variable condition;
	bool scopeEntered = false;
	bool releaseScope = false;
	bool setterStarted = false;
	bool setterFinished = false;
	std::thread renderThread([&]() {
		NativeLine3DSubmitterScope scope;
		{
			std::lock_guard<std::mutex> lock(conditionMutex);
			scopeEntered = true;
		}
		condition.notify_all();
		std::unique_lock<std::mutex> lock(conditionMutex);
		condition.wait(lock, [&]() { return releaseScope; });
	});
	{
		std::unique_lock<std::mutex> lock(conditionMutex);
		condition.wait(lock, [&]() { return scopeEntered; });
	}
	std::thread setterThread([&]() {
		{
			std::lock_guard<std::mutex> lock(conditionMutex);
			setterStarted = true;
		}
		condition.notify_all();
		Set_Native_Line3D_Submitter(0);
		{
			std::lock_guard<std::mutex> lock(conditionMutex);
			setterFinished = true;
		}
		condition.notify_all();
	});
	{
		std::unique_lock<std::mutex> lock(conditionMutex);
		condition.wait(lock, [&]() { return setterStarted; });
		const bool finishedWhilePinned = condition.wait_for(lock,
			std::chrono::milliseconds(25), [&]() { return setterFinished; });
		int result = Check(!finishedWhilePinned,
			"Line3D lifecycle publication waits for an active render scope");
		releaseScope = true;
		lock.unlock();
		condition.notify_all();
		renderThread.join();
		setterThread.join();
		result |= Check(setterFinished && Get_Native_Line3D_Submitter() == 0,
			"Line3D lifecycle publication clears only after render quiescence");
		return result;
	}
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
		L"Native Line3D lifecycle contract", WS_OVERLAPPEDWINDOW,
		0, 0, 64, 64, 0, 0, windowClass.hInstance, 0);
}

int RunNativeW3D2LifecycleContract()
{
	using namespace rts::render;
	HWND window = CreateHiddenWindow();
	if (window == 0)
	{
		return Check(false, "native line lifecycle test creates a hidden window");
	}
	IRenderDevice *device = CreateD3D11RenderDevice();
	if (device == 0)
	{
		DestroyWindow(window);
		return Check(false, "native line lifecycle test creates a D3D11 device");
	}
	RenderDeviceParameters parameters;
	parameters.backend = RENDER_BACKEND_D3D11;
	parameters.window = window;
	parameters.width = 64;
	parameters.height = 64;
	parameters.enableVsync = false;
	parameters.allowSoftwareFallback = true;
	const RenderResult initializeResult = device->initialize(parameters);
	if (initializeResult == RENDER_RESULT_UNSUPPORTED)
	{
		device->shutdown();
		delete device;
		DestroyWindow(window);
		return 77;
	}
	int result = Check(initializeResult == RENDER_RESULT_OK,
		"native line lifecycle test initializes a borrowed D3D11 device");
	if (result != 0)
	{
		device->shutdown();
		delete device;
		DestroyWindow(window);
		return result;
	}

	IRenderContext *context = device->immediateContext();
	NativeW3D2 nativeW3D;
	result |= Check(nativeW3D.AttachBackend(device, context) ==
		RENDER_RESULT_OK,
		"NativeW3D2 publishes the Line3D owner when attaching the product backend");
	NativeLine3DSubmitter *submitter = Get_Native_Line3D_Submitter();
	result |= Check(submitter != 0 && nativeW3D.IsAttachedToBorrowedBackend(),
		"attached NativeW3D2 exposes a live Line3D submitter");

	float positions[NATIVE_LINE3D_VERTEX_COUNT * 3];
	for (unsigned int vertex = 0; vertex < NATIVE_LINE3D_VERTEX_COUNT;
		++vertex)
	{
		positions[vertex * 3] = static_cast<float>(vertex);
		positions[vertex * 3 + 1] = -static_cast<float>(vertex);
		positions[vertex * 3 + 2] = 0.0f;
	}
	NativeLine3DGeometry geometry;
	LegacyLogicalState state;
	float world[16];
	for (unsigned int index = 0; index < 16; ++index)
	{
		world[index] = (index % 5 == 0) ? 1.0f : 0.0f;
	}
	result |= Check(Build_Native_Line3D_Geometry(positions,
		NATIVE_LINE3D_VERTEX_COUNT, RenderFloat4(1.0f, 1.0f, 1.0f, 1.0f),
		&geometry) && Build_Native_Line3D_State(0, 0, world, &state),
		"native line lifecycle test prepares neutral geometry and state");

	NativeLine3DBufferSet buffers;
	if (result == 0)
	{
		result |= Check(nativeW3D.Renderer().BeginFrame() == RENDER_RESULT_OK,
			"attached NativeW3D2 begins a Line3D frame");
		result |= Check(nativeW3D.Renderer().SetViewport(
			RenderViewport(0.0f, 0.0f, 64.0f, 64.0f, 0.0f, 1.0f)) ==
			RENDER_RESULT_OK,
			"attached NativeW3D2 accepts the Line3D viewport");
		result |= Check(submitter->SubmitLine3D(geometry, state, &buffers) ==
			RENDER_RESULT_OK && buffers.vertexBuffer.isValid() &&
			buffers.indexBuffer.isValid(),
			"the published Line3D submitter uploads and submits geometry");
		result |= Check(nativeW3D.Renderer().EndFrame(false) == RENDER_RESULT_OK,
			"attached NativeW3D2 closes the Line3D frame");
		result |= Check(nativeW3D.ReplaceBackendContext(context) ==
			RENDER_RESULT_OK && Get_Native_Line3D_Submitter() == submitter,
			"backend-context reset preserves the published Line3D owner");
		result |= Check(nativeW3D.Renderer().BeginFrame() == RENDER_RESULT_OK,
			"the rebound backend begins another Line3D frame");
		result |= Check(submitter->SubmitLine3D(geometry, state, &buffers) ==
			RENDER_RESULT_OK,
			"Line3D reuses recoverable buffers after backend-context reset");
		result |= Check(nativeW3D.Renderer().EndFrame(false) == RENDER_RESULT_OK,
			"the rebound backend closes the second Line3D frame");
	}
	if (result == 0)
	{
		result |= Check(nativeW3D.Renderer().BeginFrame() == RENDER_RESULT_OK,
			"a Line3D submission failure still opens a controlled frame");
		result |= Check(submitter->SubmitLine3D(geometry, state, 0) ==
			RENDER_RESULT_INVALID_ARGUMENT,
			"the Line3D owner reports a rejected submission");
		result |= Check(nativeW3D.Renderer().EndFrame(false) ==
			RENDER_RESULT_INVALID_ARGUMENT,
			"a rejected Line3D submission is visible through frame completion");
	}

	result |= Check(nativeW3D.Shutdown() == RENDER_RESULT_OK &&
		Get_Native_Line3D_Submitter() == 0 &&
		!buffers.vertexBuffer.isValid() && !buffers.indexBuffer.isValid(),
		"NativeW3D2 drains Line3D caches before clearing the owner and releasing the backend");
	if (submitter != 0)
	{
		result |= Check(submitter->SubmitLine3D(geometry, state, &buffers) ==
			RENDER_RESULT_INVALID_ARGUMENT,
			"a cleared Line3D owner cannot submit after NativeW3D2 shutdown");
	}
	result |= Check(nativeW3D.AttachBackend(device, context) ==
		RENDER_RESULT_OK && Get_Native_Line3D_Submitter() != 0,
		"NativeW3D2 rebind republishes the Line3D owner");
	NativeLine3DSubmitter *reboundSubmitter = Get_Native_Line3D_Submitter();
	result |= Check(reboundSubmitter == submitter,
		"Line3D owner identity remains stable across backend rebind");
	result |= Check(nativeW3D.Shutdown() == RENDER_RESULT_OK &&
		Get_Native_Line3D_Submitter() == 0,
		"rebound NativeW3D2 teardown clears the Line3D owner deterministically");

	device->shutdown();
	delete device;
	DestroyWindow(window);
	return result;
}
}

int main()
{
	using namespace rts::render;
	int result = 0;
	float positions[NATIVE_LINE3D_VERTEX_COUNT * 3];
	for (unsigned int vertex = 0; vertex < NATIVE_LINE3D_VERTEX_COUNT;
		++vertex)
	{
		positions[vertex * 3] = static_cast<float>(vertex);
		positions[vertex * 3 + 1] = -static_cast<float>(vertex);
		positions[vertex * 3 + 2] = 0.5f;
	}
	const RenderFloat4 color(1.0f, 0.5f, 0.0f, 0.25f);
	NativeLine3DGeometry geometry;
	result |= Check(!Build_Native_Line3D_Geometry(0,
		NATIVE_LINE3D_VERTEX_COUNT, color, &geometry),
		"native line geometry rejects a null position stream");
	result |= Check(!Build_Native_Line3D_Geometry(positions,
		NATIVE_LINE3D_VERTEX_COUNT - 1, color, &geometry),
		"native line geometry rejects a short vertex cohort");
	result |= Check(Build_Native_Line3D_Geometry(positions,
		NATIVE_LINE3D_VERTEX_COUNT, color, &geometry),
		"native line geometry accepts the complete eight-vertex box");
	const unsigned int expectedColor = 0x3fff7f00U;
	result |= Check(geometry.vertices[0].color == expectedColor &&
		geometry.vertices[7].color == expectedColor &&
		geometry.vertices[0].x == 0.0f &&
		geometry.vertices[7].y == -7.0f,
		"native line geometry preserves positions and ARGB-to-BGRA8 packing");
	const unsigned short expectedFirstTriangle[3] = { 3, 5, 1 };
	result |= Check(memcmp(geometry.indices, expectedFirstTriangle,
		sizeof(expectedFirstTriangle)) == 0 &&
		geometry.indices[NATIVE_LINE3D_INDEX_COUNT - 1] == 0,
		"native line geometry preserves the legacy indexed box winding");

	RenderVertexLayout layout;
	Build_Native_Line3D_Layout(&layout);
	result |= Check(layout.stride == sizeof(NativeLine3DVertex) &&
		layout.elementCount == 2 && !layout.preTransformed &&
		layout.elements[0].semantic == RENDER_VERTEX_SEMANTIC_POSITION &&
		layout.elements[0].format == RENDER_VERTEX_DATA_FLOAT3 &&
		layout.elements[0].byteOffset == 0 &&
		layout.elements[1].semantic == RENDER_VERTEX_SEMANTIC_DIFFUSE &&
		layout.elements[1].format == RENDER_VERTEX_DATA_COLOR_BGRA8 &&
		layout.elements[1].byteOffset == 12,
		"native line layout describes neutral position/color input elements");

	float world[16];
	for (unsigned int index = 0; index < 16; ++index)
	{
		world[index] = static_cast<float>(index + 1);
	}
	LegacyLogicalState baseState;
	baseState.constants.view.values[0] = 42.0f;
	baseState.constants.projection.values[5] = 43.0f;
	baseState.pipeline.shaderBits = 0xfeedU;
	baseState.pipeline.pixelProgram = RENDER_LEGACY_PIXEL_WATER_RIVER;
	baseState.pipeline.vertexProgram = RENDER_LEGACY_VERTEX_TREES;
	baseState.pipeline.blend.blendEnable = true;
	baseState.pipeline.blend.sourceColor = RENDER_BLEND_SOURCE_ALPHA;
	baseState.pipeline.blend.destinationColor = RENDER_BLEND_INVERSE_SOURCE_ALPHA;
	baseState.pipeline.blend.colorOperation = RENDER_BLEND_MAXIMUM;
	baseState.pipeline.blend.sourceAlpha = RENDER_BLEND_DESTINATION_ALPHA;
	baseState.pipeline.blend.destinationAlpha = RENDER_BLEND_INVERSE_DESTINATION_ALPHA;
	baseState.pipeline.blend.alphaOperation = RENDER_BLEND_REVERSE_SUBTRACT;
	baseState.pipeline.blend.colorWriteMask = 0x05U;
	baseState.pipeline.depthStencil.depthEnable = false;
	baseState.pipeline.depthStencil.depthWrite = false;
	baseState.pipeline.depthStencil.depthFunction = RENDER_COMPARE_GREATER;
	baseState.pipeline.depthStencil.stencilEnable = true;
	baseState.pipeline.depthStencil.stencilReadMask = 0x12U;
	baseState.pipeline.depthStencil.stencilWriteMask = 0x34U;
	baseState.pipeline.depthStencil.stencilReference = 17U;
	baseState.pipeline.depthStencil.stencilFunction = RENDER_COMPARE_GREATER;
	baseState.pipeline.depthStencil.stencilFail = RENDER_STENCIL_REPLACE;
	baseState.pipeline.depthStencil.stencilDepthFail = RENDER_STENCIL_INCREMENT;
	baseState.pipeline.depthStencil.stencilPass = RENDER_STENCIL_DECREMENT;
	baseState.pipeline.rasterizer.fillMode = RENDER_FILL_WIREFRAME;
	baseState.pipeline.rasterizer.cullMode = RENDER_CULL_FRONT;
	baseState.pipeline.rasterizer.frontCounterClockwise = true;
	baseState.pipeline.rasterizer.scissorEnable = true;
	baseState.pipeline.rasterizer.depthBias = 13;
	baseState.pipeline.rasterizer.slopeScaledDepthBias = 2.5f;
	baseState.pipeline.rangeFogEnable = true;
	baseState.pipeline.lightingEnable = true;
	baseState.pipeline.normalizeNormals = true;
	baseState.pipeline.ambientMaterialSource = RENDER_MATERIAL_SOURCE_COLOR2;
	baseState.pipeline.diffuseMaterialSource = RENDER_MATERIAL_SOURCE_MATERIAL;
	baseState.pipeline.emissiveMaterialSource = RENDER_MATERIAL_SOURCE_COLOR2;
	baseState.pipeline.textureFactor = 0x12345678U;
	baseState.pipeline.alphaTestEnable = true;
	baseState.pipeline.alphaFunction = RENDER_COMPARE_NOT_EQUAL;
	baseState.pipeline.alphaReference = 0x11U;
	baseState.constants.material.diffuse = RenderFloat4(0.2f, 0.3f, 0.4f, 0.5f);
	baseState.constants.material.ambient = RenderFloat4(0.4f, 0.3f, 0.2f, 0.5f);
	baseState.constants.material.specular = RenderFloat4(0.9f, 0.8f, 0.7f, 0.6f);
	baseState.constants.material.emissive = RenderFloat4(0.6f, 0.7f, 0.8f, 0.9f);
	baseState.constants.material.specularPower = 32.0f;
	baseState.texturePresenceMask = 0x03U;
	LegacyLogicalState state;
	const unsigned int opaqueShaderBits = (1U << 3) | (1U << 4) |
		(1U << 14) | (1U << 19);
	result |= Check(Build_Native_Line3D_State(opaqueShaderBits, &baseState,
		world, &state),
		"native line state accepts a neutral shader bit set");
	result |= Check(state.constants.world.values[0] == 1.0f &&
		state.constants.world.values[15] == 16.0f &&
		state.constants.view.values[0] == 42.0f &&
		state.constants.projection.values[5] == 43.0f &&
		state.texturePresenceMask == 0 &&
		state.pipeline.shaderBits == opaqueShaderBits &&
		state.pipeline.pixelProgram == RENDER_LEGACY_PIXEL_WATER_RIVER &&
		state.pipeline.vertexProgram == RENDER_LEGACY_VERTEX_TREES &&
		!state.pipeline.blend.blendEnable &&
		state.pipeline.blend.sourceColor == RENDER_BLEND_SOURCE_ALPHA &&
		state.pipeline.blend.destinationColor == RENDER_BLEND_INVERSE_SOURCE_ALPHA &&
		state.pipeline.blend.colorOperation == RENDER_BLEND_MAXIMUM &&
		state.pipeline.blend.sourceAlpha == RENDER_BLEND_DESTINATION_ALPHA &&
		state.pipeline.blend.destinationAlpha == RENDER_BLEND_INVERSE_DESTINATION_ALPHA &&
		state.pipeline.blend.alphaOperation == RENDER_BLEND_REVERSE_SUBTRACT &&
		state.pipeline.blend.colorWriteMask == 0x05U &&
		!state.pipeline.depthStencil.depthEnable &&
		state.pipeline.depthStencil.depthWrite &&
		state.pipeline.depthStencil.depthFunction == RENDER_COMPARE_NEVER &&
		state.pipeline.depthStencil.stencilEnable &&
		state.pipeline.depthStencil.stencilReadMask == 0x12U &&
		state.pipeline.depthStencil.stencilWriteMask == 0x34U &&
		state.pipeline.depthStencil.stencilReference == 17U &&
		state.pipeline.depthStencil.stencilFunction == RENDER_COMPARE_GREATER &&
		state.pipeline.depthStencil.stencilFail == RENDER_STENCIL_REPLACE &&
		state.pipeline.depthStencil.stencilDepthFail == RENDER_STENCIL_INCREMENT &&
		state.pipeline.depthStencil.stencilPass == RENDER_STENCIL_DECREMENT &&
		state.pipeline.rasterizer.fillMode == RENDER_FILL_WIREFRAME &&
		state.pipeline.rasterizer.cullMode == RENDER_CULL_BACK &&
		state.pipeline.rasterizer.frontCounterClockwise &&
		state.pipeline.rasterizer.scissorEnable &&
		state.pipeline.rasterizer.depthBias == 13 &&
		state.pipeline.rasterizer.slopeScaledDepthBias == 2.5f &&
		state.pipeline.rangeFogEnable &&
		!state.pipeline.lightingEnable &&
		state.pipeline.ambientMaterialSource == RENDER_MATERIAL_SOURCE_MATERIAL &&
		state.pipeline.diffuseMaterialSource == RENDER_MATERIAL_SOURCE_COLOR1 &&
		state.pipeline.emissiveMaterialSource == RENDER_MATERIAL_SOURCE_MATERIAL &&
		state.pipeline.textureFactor == 0x12345678U &&
		!state.pipeline.alphaTestEnable &&
		state.pipeline.alphaFunction == RENDER_COMPARE_NOT_EQUAL &&
		state.pipeline.alphaReference == 0x11U &&
		state.constants.material.diffuse.x == 1.0f &&
		state.constants.material.diffuse.y == 1.0f &&
		state.constants.material.diffuse.z == 1.0f &&
		state.constants.material.diffuse.w == 1.0f &&
		state.constants.material.ambient.x == 1.0f &&
		state.constants.material.ambient.y == 1.0f &&
		state.constants.material.ambient.z == 1.0f &&
		state.constants.material.ambient.w == 1.0f &&
		state.constants.material.specular.x == 0.0f &&
		state.constants.material.specular.y == 0.0f &&
		state.constants.material.specular.z == 0.0f &&
		state.constants.material.specular.w == 0.0f &&
		state.constants.material.emissive.x == 0.0f &&
		state.constants.material.emissive.y == 0.0f &&
		state.constants.material.emissive.z == 0.0f &&
		state.constants.material.emissive.w == 0.0f &&
		state.constants.material.specularPower == 0.0f,
		"native line state preserves non-shader pipeline/material state while forcing PRELIT_DIFFUSE");
	result |= Check(!Build_Native_Line3D_State(0, 0, 0, &state),
		"native line state rejects a missing world transform");

	NativeLine3DBufferSet buffers;
	result |= Check(!buffers.vertexBuffer.isValid() &&
		!buffers.indexBuffer.isValid(),
		"native line buffer state starts with no resource handles");
	result |= Check(Upload_Native_Line3D_Geometry(0, &geometry, &buffers) ==
		RENDER_RESULT_INVALID_ARGUMENT,
		"native line upload rejects an absent resource table");
	result |= Check(Submit_Native_Line3D(0, 0, &state, &geometry, &buffers) ==
		RENDER_RESULT_INVALID_ARGUMENT,
		"native line submission rejects an absent renderer without side effects");
	NativeLine3DRenderContext context(0, 0);
	result |= Check(context.SubmitLine3D(geometry, state, &buffers) ==
		RENDER_RESULT_INVALID_ARGUMENT,
		"native line context rejects an unbound renderer/resource pair");
	Set_Native_Line3D_Submitter(&context);
	result |= Check(Get_Native_Line3D_Submitter() == &context,
		"native line submitter publication is explicit");
	Set_Native_Line3D_Submitter(0);
	result |= Check(Get_Native_Line3D_Submitter() == 0,
		"native line submitter can be cleared at lifecycle teardown");
	result |= RunNativeLine3DWorldTransformContract();
	result |= RunNativeLine3DSubmitterGateContract();
	if (result == 0)
	{
		const int lifecycleResult = RunNativeW3D2LifecycleContract();
		if (lifecycleResult == 77)
		{
			return 77;
		}
		result |= lifecycleResult;
	}
	return result;
}
