#include "Utility/CppMacros.h"
#include "nativew3d2.h"
#include "nativew3dline.h"
#include "line3d.h"
#include "camera.h"
#include "rinfo.h"
#include "static_sort_list.h"
#include "ww3d.h"

#include <cstdio>
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
	std::fprintf(stderr, "FAIL: %s\n", message);
	return 1;
}

const wchar_t *kWindowClassName =
	L"GeneralsGameCodeNativeW3DLineProductionTestWindow";

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
		L"Native Line3D production contract", WS_OVERLAPPEDWINDOW,
		0, 0, 64, 64, 0, 0, windowClass.hInstance, 0);
}

class RecordingSubmitter : public rts::render::NativeLine3DSubmitter
{
public:
	RecordingSubmitter() : calls(0), result(rts::render::RENDER_RESULT_OK),
		lastGeometry(), lastState()
	{
	}

	virtual rts::render::RenderResult SubmitLine3D(
		const rts::render::NativeLine3DGeometry &geometry,
		const rts::render::LegacyLogicalState &state,
		rts::render::NativeLine3DBufferSet *buffers) override
	{
		if (buffers == 0)
		{
			return rts::render::RENDER_RESULT_INVALID_ARGUMENT;
		}
		++calls;
		lastGeometry = geometry;
		lastState = state;
		return result;
	}

	virtual bool ReleaseLine3D(
		rts::render::NativeLine3DBufferSet *buffers) override
	{
		return buffers != 0;
	}

	int calls;
	rts::render::RenderResult result;
	rts::render::NativeLine3DGeometry lastGeometry;
	rts::render::LegacyLogicalState lastState;
};

class RecordingStaticSortList : public StaticSortListClass
{
public:
	RecordingStaticSortList() : calls(0), lastObject(0), lastLevel(0) {}

	virtual void Add_To_List(RenderObjClass *object,
		unsigned int sortLevel) override
	{
		++calls;
		lastObject = object;
		lastLevel = sortLevel;
	}

	virtual void Render_And_Clear(RenderInfoClass &rinfo) override
	{
		(void)rinfo;
	}

	int calls;
	RenderObjClass *lastObject;
	unsigned int lastLevel;
};

int RunLiveLine3DRecordingContract()
{
	using namespace rts::render;
	CameraClass camera;
	RenderInfoClass renderInfo(camera);
	RecordingSubmitter submitter;
	Set_Native_Line3D_Submitter(&submitter);

	LegacyPipelineState seededPipeline;
	seededPipeline.lightingEnable = true;
	seededPipeline.ambientMaterialSource = RENDER_MATERIAL_SOURCE_COLOR2;
	seededPipeline.diffuseMaterialSource = RENDER_MATERIAL_SOURCE_MATERIAL;
	seededPipeline.emissiveMaterialSource = RENDER_MATERIAL_SOURCE_COLOR2;
	seededPipeline.blend.colorOperation = RENDER_BLEND_MAXIMUM;
	seededPipeline.blend.colorWriteMask = 0x05U;
	seededPipeline.depthStencil.depthEnable = false;
	seededPipeline.depthStencil.stencilEnable = true;
	seededPipeline.rasterizer.fillMode = RENDER_FILL_WIREFRAME;
	seededPipeline.rasterizer.frontCounterClockwise = true;
	seededPipeline.rasterizer.depthBias = 13;
	seededPipeline.rangeFogEnable = true;
	TrackLegacyPipelineState(seededPipeline);
	TrackLegacyMaterial(LegacyMaterialState());

	Line3DClass line(Vector3(0.0f, 0.0f, 0.0f),
		Vector3(1.0f, 0.0f, 0.0f), 0.25f, 0.2f, 0.4f, 0.8f, 1.0f);
	float matrixValues[12] = {
		0.0f, -1.0f, 0.0f, 11.0f,
		1.0f, 0.0f, 0.0f, 22.0f,
		0.0f, 0.0f, 1.0f, 33.0f
	};
	line.Set_Transform(Matrix3D(matrixValues));
	line.Render(renderInfo);

	int result = Check(submitter.calls == 1,
		"live Line3D::Render reaches the published native submitter");
	const float expectedWorld[16] = {
		0.0f, 1.0f, 0.0f, 0.0f,
		-1.0f, 0.0f, 0.0f, 0.0f,
		0.0f, 0.0f, 1.0f, 0.0f,
		11.0f, 22.0f, 33.0f, 1.0f
	};
	for (unsigned int index = 0; index < 16; ++index)
	{
		if (submitter.lastState.constants.world.values[index] !=
			expectedWorld[index])
		{
			result |= Check(false,
				"live Line3D::Render maps a non-identity Matrix3D to native world layout");
			break;
		}
	}
	result |= Check(!submitter.lastState.pipeline.lightingEnable &&
		submitter.lastState.pipeline.ambientMaterialSource ==
			RENDER_MATERIAL_SOURCE_MATERIAL &&
		submitter.lastState.pipeline.diffuseMaterialSource ==
			RENDER_MATERIAL_SOURCE_COLOR1 &&
		submitter.lastState.pipeline.emissiveMaterialSource ==
			RENDER_MATERIAL_SOURCE_MATERIAL,
		"live Line3D::Render forces the PRELIT_DIFFUSE unlit material contract");
	result |= Check(submitter.lastState.pipeline.blend.colorOperation ==
			RENDER_BLEND_MAXIMUM &&
		submitter.lastState.pipeline.blend.colorWriteMask == 0x05U &&
		!submitter.lastState.pipeline.depthStencil.depthEnable &&
		submitter.lastState.pipeline.depthStencil.stencilEnable &&
		submitter.lastState.pipeline.rasterizer.fillMode ==
			RENDER_FILL_WIREFRAME &&
		submitter.lastState.pipeline.rasterizer.frontCounterClockwise &&
		submitter.lastState.pipeline.rasterizer.depthBias == 13 &&
		submitter.lastState.pipeline.rangeFogEnable,
		"live Line3D::Render retains non-shader state from the lit base");

	RecordingStaticSortList sortList;
	WW3D::Override_Current_Static_Sort_Lists(&sortList);
	WW3D::Enable_Static_Sort_Lists(true);
	Line3DClass sortedLine(Vector3(0.0f, 0.0f, 0.0f),
		Vector3(1.0f, 0.0f, 0.0f), 0.25f, 1.0f, 1.0f, 1.0f, 0.5f);
	sortedLine.Render(renderInfo);
	result |= Check(sortList.calls == 1 && sortList.lastObject == &sortedLine &&
		sortList.lastLevel == 1 && submitter.calls == 1,
		"live Line3D::Render preserves static-sort deferral before submission");
	Line3DClass sortedCopy(sortedLine);
	result |= Check(sortedCopy.Get_Sort_Level() == sortedLine.Get_Sort_Level(),
		"Line3D copy construction preserves static sort level");
	Line3DClass sortedAssignment(Vector3(0.0f, 0.0f, 0.0f),
		Vector3(1.0f, 0.0f, 0.0f), 0.25f, 1.0f, 1.0f, 1.0f, 1.0f);
	sortedAssignment = sortedLine;
	result |= Check(sortedAssignment.Get_Sort_Level() ==
		sortedLine.Get_Sort_Level(),
		"Line3D assignment preserves static sort level");
	RenderObjClass *sortedClone = sortedLine.Clone();
	result |= Check(sortedClone != 0 && sortedClone->Get_Sort_Level() ==
		sortedLine.Get_Sort_Level(),
		"Line3D cloning preserves static sort level");
	if (sortedClone != 0)
	{
		sortedClone->Release_Ref();
	}
	WW3D::Enable_Static_Sort_Lists(false);
	WW3D::Reset_Current_Static_Sort_Lists_To_Default();
	ResetTrackedLegacyState();
	Set_Native_Line3D_Submitter(0);
	return result;
}

int RunLiveLine3DLifecycleContract()
{
	using namespace rts::render;
	HWND window = CreateHiddenWindow();
	if (window == 0)
	{
		return Check(false, "Line3D production test creates a hidden window");
	}
	IRenderDevice *device = CreateD3D11RenderDevice();
	if (device == 0)
	{
		DestroyWindow(window);
		return Check(false, "Line3D production test creates a D3D11 device");
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
		"Line3D production test initializes the borrowed D3D11 device");
	if (result != 0)
	{
		device->shutdown();
		delete device;
		DestroyWindow(window);
		return result;
	}

	IRenderContext *context = device->immediateContext();
	NativeW3D2 nativeW3D;
	CameraClass camera;
	RenderInfoClass renderInfo(camera);
	Line3DClass line(Vector3(0.0f, 0.0f, 0.0f),
		Vector3(1.0f, 0.0f, 0.0f), 0.25f, 1.0f, 1.0f, 1.0f, 1.0f);
	float matrixValues[12] = {
		0.0f, -1.0f, 0.0f, 11.0f,
		1.0f, 0.0f, 0.0f, 22.0f,
		0.0f, 0.0f, 1.0f, 33.0f
	};
	line.Set_Transform(Matrix3D(matrixValues));
	result |= Check(nativeW3D.AttachBackend(device, context) ==
		RENDER_RESULT_OK,
		"NativeW3D2 publishes the production Line3D owner");
	if (result == 0)
	{
		result |= Check(nativeW3D.Renderer().BeginFrame() == RENDER_RESULT_OK,
			"production Line3D frame opens before live Render");
		result |= Check(nativeW3D.Renderer().SetViewport(RenderViewport(
			0.0f, 0.0f, 64.0f, 64.0f, 0.0f, 1.0f)) == RENDER_RESULT_OK,
			"production Line3D frame accepts the viewport");
		line.Render(renderInfo);
		result |= Check(nativeW3D.Renderer().EndFrame(false) == RENDER_RESULT_OK,
			"live Line3D Render submits through the D3D11 owner");
		NativeLine3DSubmitter *submitter = Get_Native_Line3D_Submitter();
		NativeLine3DGeometry invalidGeometry;
		LegacyLogicalState invalidState;
		result |= Check(nativeW3D.Renderer().BeginFrame() == RENDER_RESULT_OK &&
			submitter != 0 && submitter->SubmitLine3D(invalidGeometry,
			invalidState, 0) == RENDER_RESULT_INVALID_ARGUMENT &&
			nativeW3D.Renderer().EndFrame(false) ==
			RENDER_RESULT_INVALID_ARGUMENT,
			"a rejected live Line3D submission latches through frame completion");
		result |= Check(nativeW3D.ReplaceBackendContext(context) ==
			RENDER_RESULT_OK,
			"Line3D backend-context replacement holds the lifecycle gate");
		result |= Check(nativeW3D.Renderer().BeginFrame() == RENDER_RESULT_OK,
			"production Line3D frame reopens after rebind");
		line.Render(renderInfo);
		result |= Check(nativeW3D.Renderer().EndFrame(false) == RENDER_RESULT_OK,
			"live Line3D Render reuses buffers after rebind");
	}
	if (result == 0)
	{
		NativeLine3DRenderContext *lineContext =
			static_cast<NativeLine3DRenderContext *>(
				Get_Native_Line3D_Submitter());
		const unsigned int pendingBeforeWorkerDestroy =
			lineContext->PendingLine3DCount();
		Line3DClass *workerOwnedLine = new Line3DClass(
			Vector3(0.0f, 0.0f, 0.0f), Vector3(1.0f, 0.0f, 0.0f),
			0.25f, 0.7f, 0.8f, 0.9f, 1.0f);
		workerOwnedLine->Set_Transform(Matrix3D(matrixValues));
		result |= Check(nativeW3D.Renderer().BeginFrame() == RENDER_RESULT_OK,
			"worker-owned Line3D frame opens before retention test");
		workerOwnedLine->Render(renderInfo);
		result |= Check(nativeW3D.Renderer().EndFrame(false) == RENDER_RESULT_OK,
			"worker-owned Line3D creates a retryable native sidecar");
		std::thread workerDestroy([workerOwnedLine]() {
			delete workerOwnedLine;
		});
		workerDestroy.join();
		result |= Check(lineContext->PendingLine3DCount() >
			pendingBeforeWorkerDestroy,
			"off-owner Line3D destruction retains its sidecar for the owner retry");
		const RenderResult retainedShutdownResult = nativeW3D.Shutdown();
		result |= Check(retainedShutdownResult == RENDER_RESULT_OK &&
			lineContext->PendingLine3DCount() == 0,
			"owner-thread shutdown retries and drains off-owner Line3D destruction");
	}
	if (result == 0)
	{
		const RenderResult shutdownResult = nativeW3D.Shutdown();
		result |= Check(shutdownResult == RENDER_RESULT_OK &&
			Get_Native_Line3D_Submitter() == 0,
			"NativeW3D2 drains live Line3D resources before owner clear");
		// The line remains live across shutdown. Its destructor must reclaim the
		// now-empty sidecar without calling a stale backend owner.
		result |= Check(nativeW3D.AttachBackend(device, context) ==
			RENDER_RESULT_OK,
			"NativeW3D2 republishes the stable Line3D owner after shutdown");
		result |= Check(nativeW3D.Renderer().BeginFrame() == RENDER_RESULT_OK,
			"production Line3D frame opens after shutdown/rebind");
		line.Render(renderInfo);
		result |= Check(nativeW3D.Renderer().EndFrame(false) == RENDER_RESULT_OK,
			"live Line3D Render recreates buffers after shutdown/rebind");
		result |= Check(nativeW3D.Shutdown() == RENDER_RESULT_OK &&
			Get_Native_Line3D_Submitter() == 0,
			"rebound NativeW3D2 drains the live Line3D sidecar deterministically");
	}

	device->shutdown();
	delete device;
	DestroyWindow(window);
	return result;
}
}

int main()
{
	int result = RunLiveLine3DRecordingContract();
	if (result == 0)
	{
		const int lifecycleResult = RunLiveLine3DLifecycleContract();
		if (lifecycleResult == 77)
		{
			return 77;
		}
		result |= lifecycleResult;
	}
	return result;
}
