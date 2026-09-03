#include "Renderer/NativeW3DRenderer.h"
#include "Renderer/NativeW3DResources.h"
#include "Renderer/RenderTexturePublication.h"

#include <cstdio>

#if defined(_WIN32)
#include <windows.h>
#endif

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

int TestTexturePublicationContract()
{
	int result = 0;
	int sentinel = 0;
	TextureBaseClass *texture =
		reinterpret_cast<TextureBaseClass *>(&sentinel);
	TextureClass *textureClass = reinterpret_cast<TextureClass *>(&sentinel);
	rts::render::ResetTrackedLegacyState();
	rts::render::SeedTrackedLegacyPipelineState();
	rts::render::PublishTextureStage(0, texture);
	rts::render::LegacyLogicalState state;
	result |= Check(rts::render::GetPublishedTextureStage(0) == texture,
		"native texture publication retains the typed stage source");
	result |= Check(rts::render::GetTrackedLegacyLogicalState(&state) &&
		(state.texturePresenceMask & 1U) != 0,
		"native texture publication updates neutral texture presence");
	rts::render::PublishTextureStage(
		rts::render::LEGACY_TEXTURE_STAGE_COUNT, texture);
	result |= Check(rts::render::GetPublishedTextureStage(
		rts::render::LEGACY_TEXTURE_STAGE_COUNT) == 0,
		"native texture publication rejects an out-of-range stage");
	rts::render::RecordTextureUse(textureClass);
	result |= Check(rts::render::GetTextureUseCount() == 1,
		"native texture publication records one stage use");
	rts::render::UnpublishTexture(texture);
	result |= Check(rts::render::GetPublishedTextureStage(0) == 0,
		"native texture unpublication clears the stage source");
	result |= Check(rts::render::GetTrackedLegacyLogicalState(&state) &&
		(state.texturePresenceMask & 1U) == 0,
		"native texture unpublication clears neutral texture presence");
	return result;
}

int TestTexturePublicationOperationalContract()
{
	int result = 0;
	result |= Check(
		rts::render::IsRenderTexturePublicationOperationalState(
			true, false, false, false),
		"legacy publication remains operational before a scene frame");
	result |= Check(
		!rts::render::IsRenderTexturePublicationOperationalState(
			true, true, false, false),
		"legacy publication is suppressed while the device is lost or resetting");
	result |= Check(
		!rts::render::IsRenderTexturePublicationOperationalState(
			false, false, false, false),
		"publication is suppressed after renderer shutdown");
	result |= Check(
		rts::render::IsRenderTexturePublicationOperationalState(
			true, false, true, true),
		"native publication is operational after bridge recovery");
	result |= Check(
		!rts::render::IsRenderTexturePublicationOperationalState(
			true, false, true, false),
		"native publication is suppressed while the bridge is inactive");
	return result;
}

#if defined(_WIN32) && defined(RTS_RENDERER_HAS_D3D11)
const wchar_t *kD3D11InputLayoutTestWindowClass =
	L"GeneralsGameCodeD3D11InputLayoutTestWindow";

LRESULT CALLBACK D3D11InputLayoutTestWindowProcedure(HWND window,
	UINT message, WPARAM wparam, LPARAM lparam)
{
	return DefWindowProcW(window, message, wparam, lparam);
}

int TestD3D11TexturedInputLayoutSafety()
{
	using namespace rts::render;
	WNDCLASSEXW windowClass;
	ZeroMemory(&windowClass, sizeof(windowClass));
	windowClass.cbSize = sizeof(windowClass);
	windowClass.lpfnWndProc = D3D11InputLayoutTestWindowProcedure;
	windowClass.hInstance = GetModuleHandleW(0);
	windowClass.lpszClassName = kD3D11InputLayoutTestWindowClass;
	const ATOM classAtom = RegisterClassExW(&windowClass);
	if (classAtom == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
	{
		return Check(false, "D3D11 input-layout test registers its window class");
	}
	HWND window = CreateWindowExW(0, kD3D11InputLayoutTestWindowClass,
		L"D3D11 input-layout safety", WS_OVERLAPPED, 0, 0, 64, 64, 0, 0,
		windowClass.hInstance, 0);
	if (window == 0)
	{
		return Check(false, "D3D11 input-layout test creates a hidden window");
	}

	IRenderDevice *device = CreateD3D11RenderDevice();
	int result = Check(device != 0,
		"D3D11 input-layout test creates a native device");
	if (device == 0)
	{
		DestroyWindow(window);
		return result;
	}
	RenderDeviceParameters parameters;
	parameters.backend = RENDER_BACKEND_D3D11;
	parameters.window = window;
	parameters.width = 64;
	parameters.height = 64;
	parameters.enableVsync = false;
	parameters.allowSoftwareFallback = true;
	const RenderResult initializeResult = device->initialize(parameters);
	result |= Check(initializeResult == RENDER_RESULT_OK,
		"D3D11 input-layout test initializes its native device");
	if (initializeResult != RENDER_RESULT_OK)
	{
		device->shutdown();
		delete device;
		DestroyWindow(window);
		return result;
	}

	struct XYZVertex
	{
		float x;
		float y;
		float z;
	};
	const XYZVertex xyzVertices[3] = {
		{ -0.5f, -0.5f, 0.0f },
		{ 0.0f, 0.5f, 0.0f },
		{ 0.5f, -0.5f, 0.0f }
	};
	BufferDescriptor xyzDescriptor;
	xyzDescriptor.byteCount = sizeof(xyzVertices);
	xyzDescriptor.stride = sizeof(XYZVertex);
	xyzDescriptor.binding = RENDER_BUFFER_VERTEX;
	xyzDescriptor.usage = RENDER_USAGE_IMMUTABLE;
	GpuHandle xyzBuffer;
	result |= Check(device->createBuffer(xyzDescriptor, xyzVertices,
		sizeof(xyzVertices), &xyzBuffer) == RENDER_RESULT_OK,
		"plain XYZ input fixture creates a 12-byte vertex stream");

	LegacyVertexLayout xyzLayout;
	xyzLayout.stride = sizeof(XYZVertex);
	xyzLayout.elementCount = 1;
	xyzLayout.elements[0].semantic = RENDER_VERTEX_SEMANTIC_POSITION;
	xyzLayout.elements[0].semanticIndex = 0;
	xyzLayout.elements[0].format = RENDER_VERTEX_DATA_FLOAT3;
	xyzLayout.elements[0].byteOffset = 0;
	LegacyLogicalState state;
	IRenderContext *context = device->immediateContext();
	bool frameStarted = context != 0 &&
		context->beginFrame() == RENDER_RESULT_OK;
	result |= Check(frameStarted,
		"plain XYZ input fixture begins a D3D11 frame");
	if (frameStarted)
	{
		const RenderResult layoutResult = context->setLegacyStateForLayout(
			state, xyzLayout, 0);
		result |= Check(layoutResult == RENDER_RESULT_OK,
			"plain XYZ selects the unweighted shader and bounded layout");
		result |= Check(context->setVertexBuffer(xyzBuffer,
			sizeof(XYZVertex), 0) == RENDER_RESULT_OK,
			"plain XYZ binds its exact 12-byte source stride");
		result |= Check(context->setPrimitiveTopology(
			RENDER_PRIMITIVE_TRIANGLE_LIST) == RENDER_RESULT_OK &&
			context->draw(3, 0) == RENDER_RESULT_OK,
			"plain XYZ draws without a synthesized 16-byte read");
		result |= Check(context->endFrame() == RENDER_RESULT_OK,
			"plain XYZ input fixture ends its D3D11 frame");
	}

	struct WeightedVertex
	{
		float x;
		float y;
		float z;
		float weight0[4];
	};
	const WeightedVertex weightedVertices[3] = {
		{ -0.5f, -0.5f, 0.0f, { 1.0f, 0.0f, 0.0f, 0.0f } },
		{ 0.0f, 0.5f, 0.0f, { 1.0f, 0.0f, 0.0f, 0.0f } },
		{ 0.5f, -0.5f, 0.0f, { 1.0f, 0.0f, 0.0f, 0.0f } }
	};
	BufferDescriptor weightedDescriptor;
	weightedDescriptor.byteCount = sizeof(weightedVertices);
	weightedDescriptor.stride = sizeof(WeightedVertex);
	weightedDescriptor.binding = RENDER_BUFFER_VERTEX;
	weightedDescriptor.usage = RENDER_USAGE_IMMUTABLE;
	GpuHandle weightedBuffer;
	result |= Check(device->createBuffer(weightedDescriptor, weightedVertices,
		sizeof(weightedVertices), &weightedBuffer) == RENDER_RESULT_OK,
		"weighted input fixture creates its source stream");
	LegacyVertexLayout weightedLayout = xyzLayout;
	weightedLayout.stride = sizeof(WeightedVertex);
	weightedLayout.elementCount = 2;
	weightedLayout.elements[1].semantic = RENDER_VERTEX_SEMANTIC_BLEND_WEIGHT;
	weightedLayout.elements[1].semanticIndex = 0;
	weightedLayout.elements[1].format = RENDER_VERTEX_DATA_FLOAT4;
	weightedLayout.elements[1].byteOffset = 12;
	frameStarted = context->beginFrame() == RENDER_RESULT_OK;
	result |= Check(frameStarted,
		"weighted input fixture begins a D3D11 frame");
	if (frameStarted)
	{
		const RenderResult layoutResult = context->setLegacyStateForLayout(
			state, weightedLayout, 0);
		result |= Check(layoutResult == RENDER_RESULT_OK,
			"weighted XYZ selects the weighted shader and layout");
		result |= Check(context->setVertexBuffer(weightedBuffer,
			sizeof(WeightedVertex), 0) == RENDER_RESULT_OK &&
			context->setPrimitiveTopology(
				RENDER_PRIMITIVE_TRIANGLE_LIST) == RENDER_RESULT_OK &&
			context->draw(3, 0) == RENDER_RESULT_OK,
			"weighted XYZ draws through its explicit blend declaration");
		result |= Check(context->endFrame() == RENDER_RESULT_OK,
			"weighted input fixture ends its D3D11 frame");
	}

	LegacyVertexLayout shortLayout = xyzLayout;
	shortLayout.stride = 8;
	frameStarted = context->beginFrame() == RENDER_RESULT_OK;
	result |= Check(frameStarted,
		"short XYZ input fixture begins a D3D11 frame");
	if (frameStarted)
	{
		result |= Check(context->setLegacyStateForLayout(state, shortLayout, 0) ==
			RENDER_RESULT_INVALID_ARGUMENT,
			"short XYZ rejects a position declaration outside its source stride");
		result |= Check(context->endFrame() == RENDER_RESULT_OK,
			"short XYZ input fixture ends its D3D11 frame");
	}

	if (xyzBuffer.isValid())
	{
		result |= Check(device->destroyResource(xyzBuffer),
			"plain XYZ input fixture releases its buffer");
	}
	if (weightedBuffer.isValid())
	{
		result |= Check(device->destroyResource(weightedBuffer),
			"weighted input fixture releases its buffer");
	}
	device->shutdown();
	delete device;
	DestroyWindow(window);
	return result;
}
#endif
}

int main()
{
	int result = 0;
	result |= TestTexturePublicationContract();
	result |= TestTexturePublicationOperationalContract();
#if defined(_WIN32) && defined(RTS_RENDERER_HAS_D3D11)
	result |= TestD3D11TexturedInputLayoutSafety();
#endif
	rts::render::NativeW3DRenderer renderer;
	rts::render::NativeW3DResources resources;
	rts::render::NativeW3DRendererDescriptor descriptor;
	rts::render::NativeDrawPacket packet;
	rts::render::LegacyLogicalState state;
	rts::render::RenderViewport viewport(0.0f, 0.0f, 640.0f, 480.0f,
		0.0f, 1.0f);
	rts::render::RenderVertexLayout layout;
	rts::render::RenderMatrix4 matrix;
	result |= Check(layout.elements[0].format ==
		rts::render::RENDER_VERTEX_DATA_FLOAT3,
		"neutral position elements preserve the legacy FLOAT3 default");

	layout.stride = 16;
	layout.elementCount = 1;
	layout.elements[0].semantic = rts::render::RENDER_VERTEX_SEMANTIC_POSITION;
	layout.elements[0].format = rts::render::RENDER_VERTEX_DATA_FLOAT3;
	result |= Check(layout.elements[0].semantic ==
		rts::render::RENDER_VERTEX_SEMANTIC_POSITION &&
		viewport.width == 640.0f && viewport.maximumDepth == 1.0f &&
		matrix.values[0] == 1.0f && matrix.values[15] == 1.0f &&
		packet.indexFormat == rts::render::RENDER_FORMAT_R16_UINT &&
		packet.topology == rts::render::RENDER_PRIMITIVE_TRIANGLE_LIST,
		"native vocabulary has stable neutral defaults");

	result |= Check(renderer.BeginFrame() == rts::render::RENDER_RESULT_INVALID_ARGUMENT,
		"cannot begin a native frame before initialization");
	result |= Check(renderer.SetViewport(viewport) ==
		rts::render::RENDER_RESULT_INVALID_ARGUMENT,
		"cannot set a native viewport before initialization");
	result |= Check(renderer.EndFrame(false) == rts::render::RENDER_RESULT_INVALID_ARGUMENT,
		"cannot end a native frame before initialization");
	result |= Check(renderer.Submit(resources, state, packet) == rts::render::RENDER_RESULT_INVALID_ARGUMENT,
		"cannot submit a native draw before initialization");
	descriptor.width = 640;
	descriptor.height = 480;
	result |= Check(renderer.Initialize(0, descriptor) == rts::render::RENDER_RESULT_INVALID_ARGUMENT,
		"a native facade rejects a null window without creating a legacy device");
	result |= Check(!renderer.IsInitialized() && !renderer.IsFrameOpen(),
		"a failed initialization leaves no native renderer state behind");
	result |= Check(renderer.RecoverDevice() == rts::render::RENDER_RESULT_INVALID_ARGUMENT &&
		renderer.Resize(640, 480) == rts::render::RENDER_RESULT_INVALID_ARGUMENT,
		"recovery and resize reject an uninitialized native facade");
	result |= Check(renderer.Shutdown() == rts::render::RENDER_RESULT_OK,
		"shutdown is idempotent before native initialization");
	result |= Check(resources.Bind(0) == rts::render::RENDER_RESULT_INVALID_ARGUMENT &&
		!resources.Destroy(packet.vertexBuffer),
		"native resource tables reject an unbound renderer and stale handles");
	return result;
}
