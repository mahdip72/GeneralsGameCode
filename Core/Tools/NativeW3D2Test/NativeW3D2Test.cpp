#include "nativew3d2.h"

#include <cstdio>
#include <windows.h>

namespace
{
const wchar_t *WINDOW_CLASS_NAME = L"GeneralsGameCodeNativeW3D2ContractWindow";

LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
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
	windowClass.lpszClassName = WINDOW_CLASS_NAME;
	RegisterClassExW(&windowClass);
	return CreateWindowExW(0, WINDOW_CLASS_NAME, L"Native W3D2 contract",
		WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, 0, 0, windowClass.hInstance, 0);
}

int Check(bool condition, const char *message)
{
	if (condition)
	{
		return 0;
	}
	std::fprintf(stderr, "FAIL: %s\\n", message);
	return 1;
}

struct NativeVertex
{
	float x;
	float y;
	float z;
	unsigned int color;
};

void ConfigurePacket(rts::render::NativeDrawPacket *packet,
	rts::render::GpuHandle vertexBuffer)
{
	packet->vertexBuffer = vertexBuffer;
	packet->vertexStride = sizeof(NativeVertex);
	packet->vertexLayout.stride = sizeof(NativeVertex);
	packet->vertexLayout.elementCount = 2;
	packet->vertexLayout.elements[0].semantic = rts::render::RENDER_VERTEX_SEMANTIC_POSITION;
	packet->vertexLayout.elements[0].semanticIndex = 0;
	packet->vertexLayout.elements[0].format = rts::render::RENDER_VERTEX_DATA_FLOAT3;
	packet->vertexLayout.elements[0].byteOffset = 0;
	packet->vertexLayout.elements[1].semantic = rts::render::RENDER_VERTEX_SEMANTIC_DIFFUSE;
	packet->vertexLayout.elements[1].semanticIndex = 0;
	packet->vertexLayout.elements[1].format = rts::render::RENDER_VERTEX_DATA_COLOR_BGRA8;
	packet->vertexLayout.elements[1].byteOffset = 12;
	packet->vertexCount = 3;
}
}

int main()
{
	int result = 0;
	NativeW3D2 w3d;
	rts::render::NativeW3DRendererDescriptor descriptor;
	descriptor.width = 64;
	descriptor.height = 64;
	descriptor.enableVsync = false;
	descriptor.allowSoftwareFallback = true;
	if (w3d.Initialize(0, descriptor) != rts::render::RENDER_RESULT_INVALID_ARGUMENT)
	{
		std::fprintf(stderr, "FAIL: native WW3D2 accepted an invalid window\n");
		return 1;
	}
	HWND window = CreateHiddenWindow();
	if (window == 0)
	{
		std::fprintf(stderr, "FAIL: could not create hidden native window\\n");
		return 1;
	}
	const rts::render::RenderResult initializeResult = w3d.Initialize(window, descriptor);
	if (initializeResult == rts::render::RENDER_RESULT_UNSUPPORTED)
	{
		DestroyWindow(window);
		return 77;
	}
	result |= Check(initializeResult == rts::render::RENDER_RESULT_OK,
		"native WW3D2 initializes a hidden D3D11 swap chain");
	if (result == 0)
	{
		NativeVertex vertices[3] = {
			{ -0.5f, -0.5f, 0.0f, 0xffffffffU },
			{  0.0f,  0.5f, 0.0f, 0xffffffffU },
			{  0.5f, -0.5f, 0.0f, 0xffffffffU }
		};
		rts::render::BufferDescriptor bufferDescriptor;
		bufferDescriptor.byteCount = sizeof(vertices);
		bufferDescriptor.stride = sizeof(NativeVertex);
		bufferDescriptor.binding = rts::render::RENDER_BUFFER_VERTEX;
		bufferDescriptor.usage = rts::render::RENDER_USAGE_IMMUTABLE;
		rts::render::GpuHandle vertexBuffer;
		result |= Check(w3d.Resources().CreateBuffer(bufferDescriptor, vertices,
			sizeof(vertices), &vertexBuffer) == rts::render::RENDER_RESULT_OK,
			"native WW3D2 creates a logical vertex buffer");
		rts::render::NativeDrawPacket packet;
		ConfigurePacket(&packet, vertexBuffer);
		rts::render::LegacyLogicalState state;
		result |= Check(w3d.Renderer().BeginFrame() == rts::render::RENDER_RESULT_OK,
			"native WW3D2 begins a hidden frame");
		result |= Check(w3d.Renderer().Submit(w3d.Resources(), state, packet) ==
			rts::render::RENDER_RESULT_OK,
			"native WW3D2 submits a triangle through the logical facade");
		result |= Check(w3d.Renderer().EndFrame(true) == rts::render::RENDER_RESULT_OK,
			"native WW3D2 presents a hidden D3D11 frame");
		result |= Check(w3d.Renderer().RecoverDevice() == rts::render::RENDER_RESULT_OK,
			"native WW3D2 recovers a hidden D3D11 device");
		result |= Check(w3d.Renderer().BeginFrame() == rts::render::RENDER_RESULT_OK,
			"native WW3D2 begins a frame after device recovery");
		result |= Check(w3d.Renderer().Submit(w3d.Resources(), state, packet) ==
			rts::render::RENDER_RESULT_OK,
			"native WW3D2 preserves logical resources through device recovery");
		result |= Check(w3d.Renderer().EndFrame(true) == rts::render::RENDER_RESULT_OK,
			"native WW3D2 presents after device recovery");
	}
	result |= Check(w3d.Shutdown() == rts::render::RENDER_RESULT_OK,
		"native WW3D2 shuts down after a presented frame");
	DestroyWindow(window);
	if (result != 0)
	{
		return result;
	}
	if (w3d.Shutdown() != rts::render::RENDER_RESULT_OK)
	{
		std::fprintf(stderr, "FAIL: native WW3D2 shutdown was not deterministic\n");
		return 1;
	}
	return 0;
}
