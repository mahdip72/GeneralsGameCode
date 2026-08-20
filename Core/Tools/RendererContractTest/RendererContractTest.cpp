#include "Renderer/RendererDevice.h"
#include "Renderer/LegacyRenderState.h"

#include <stdio.h>
#include <vector>

#if defined(RTS_RENDERER_HAS_D3D11)
#include <windows.h>
#endif

namespace
{
int check(bool condition, const char *message)
{
	if (!condition)
	{
		fprintf(stderr, "FAIL: %s\n", message);
		return 1;
	}
	return 0;
}

int testBackendNames()
{
	int result = 0;
	rts::render::RenderBackend backend = rts::render::RENDER_BACKEND_D3D11;
	result |= check(rts::render::ParseRenderBackend("dx8", &backend) &&
		backend == rts::render::RENDER_BACKEND_DX8,
		"dx8 backend name parses");
	result |= check(rts::render::ParseRenderBackend("D3D11", &backend) &&
		backend == rts::render::RENDER_BACKEND_D3D11,
		"d3d11 backend name is case insensitive");
	result |= check(!rts::render::ParseRenderBackend("dx12", &backend),
		"unsupported backend name is rejected");
	result |= check(!rts::render::ParseRenderBackend(0, &backend) &&
		!rts::render::ParseRenderBackend("dx8", 0),
		"backend parser rejects null inputs");
	result |= check(rts::render::RenderBackendName(
		rts::render::RENDER_BACKEND_DX8)[0] == 'd' &&
		rts::render::RenderBackendName(
		rts::render::RENDER_BACKEND_D3D11)[3] == '1',
		"backend names are stable command-line values");
	return result;
}

int testGenerationSafeHandles()
{
	int result = 0;
	rts::render::GpuHandleAllocator allocator(2);
	const rts::render::GpuHandle first = allocator.allocate();
	const rts::render::GpuHandle second = allocator.allocate();
	result |= check(first.isValid() && second.isValid() && first != second,
		"bounded allocator returns unique live handles");
	result |= check(allocator.liveCount() == 2 &&
		allocator.allocate() == rts::render::GpuHandle(),
		"allocator rejects capacity overflow with an invalid handle");
	result |= check(allocator.isLive(first) && allocator.release(first) &&
		!allocator.isLive(first),
		"released handles immediately become stale");
	const rts::render::GpuHandle replacement = allocator.allocate();
	result |= check(replacement.isValid() &&
		replacement.index() == first.index() &&
		replacement.generation() != first.generation(),
		"reused slots advance their generation");
	result |= check(!allocator.release(first) && allocator.isLive(replacement),
		"stale release cannot destroy a replacement resource");
	result |= check(allocator.release(second) && allocator.release(replacement) &&
		allocator.liveCount() == 0,
		"every live handle can be drained deterministically");
	return result;
}

int testNeutralDescriptorDefaults()
{
	int result = 0;
	rts::render::RenderDeviceParameters device;
	rts::render::BufferDescriptor buffer;
	rts::render::TextureDescriptor texture;
	result |= check(device.backend == rts::render::RENDER_BACKEND_DX8 &&
		device.width == 0 && device.height == 0 && device.window == 0 &&
		!device.enableDebugLayer && device.enableVsync,
		"device parameters preserve the legacy backend by default");
	result |= check(buffer.byteCount == 0 && buffer.stride == 0 &&
		buffer.binding == rts::render::RENDER_BUFFER_VERTEX &&
		buffer.usage == rts::render::RENDER_USAGE_IMMUTABLE,
		"buffer descriptor has deterministic defaults");
	result |= check(texture.width == 0 && texture.height == 0 &&
		texture.mipCount == 1 && texture.arrayCount == 1 &&
		texture.format == rts::render::RENDER_FORMAT_UNKNOWN &&
		texture.binding == rts::render::RENDER_TEXTURE_SHADER_RESOURCE,
		"texture descriptor has deterministic defaults");
	return result;
}

int testLegacyLogicalState()
{
	int result = 0;
	rts::render::LegacyLogicalState state;
	result |= check(state.pipeline.depthStencil.depthEnable &&
		state.pipeline.depthStencil.depthWrite &&
		state.pipeline.depthStencil.depthFunction ==
			rts::render::RENDER_COMPARE_LESS_EQUAL &&
		!state.pipeline.blend.blendEnable &&
		state.pipeline.rasterizer.cullMode == rts::render::RENDER_CULL_BACK,
		"legacy logical state starts with deterministic depth and raster defaults");
	result |= check(state.constants.world.values[0] == 1.0f &&
		state.constants.world.values[5] == 1.0f &&
		state.constants.world.values[10] == 1.0f &&
		state.constants.world.values[15] == 1.0f &&
		state.constants.material.diffuse.w == 1.0f,
		"legacy transforms and material use identity/opaque defaults");
	result |= check(!state.constants.lights[0].enabled &&
		!state.constants.fog.enabled &&
		state.pipeline.textureStages[0].colorOperation ==
			rts::render::RENDER_TEXTURE_OP_MODULATE &&
		state.pipeline.textureStages[1].colorOperation ==
			rts::render::RENDER_TEXTURE_OP_DISABLE,
		"legacy lights, fog, and texture stages have stable defaults");

	const rts::render::LegacyShaderKey baseline =
		rts::render::BuildLegacyShaderKey(state.pipeline, 0x11223344U, 1U);
	const rts::render::LegacyShaderKey repeated =
		rts::render::BuildLegacyShaderKey(state.pipeline, 0x11223344U, 1U);
	if (baseline != repeated)
	{
		for (unsigned int word = 0;
			word < rts::render::LegacyShaderKey::WORD_COUNT; ++word)
		{
			if (baseline.words[word] != repeated.words[word])
			{
				fprintf(stderr, "Shader key word %u differs: %08x != %08x\n",
					word, baseline.words[word], repeated.words[word]);
			}
		}
	}
	result |= check(baseline == repeated,
		"identical logical states produce identical shader keys");
	state.pipeline.textureStages[0].colorOperation =
		rts::render::RENDER_TEXTURE_OP_ADD_SIGNED_2X;
	const rts::render::LegacyShaderKey combinerKey =
		rts::render::BuildLegacyShaderKey(state.pipeline, 0x11223344U, 1U);
	state.pipeline.fogMode = rts::render::RENDER_FOG_LINEAR;
	const rts::render::LegacyShaderKey fogKey =
		rts::render::BuildLegacyShaderKey(state.pipeline, 0x11223344U, 1U);
	result |= check(combinerKey != baseline && fogKey != combinerKey,
		"shader key distinguishes texture combiner and fog variants");
	return result;
}

#if defined(RTS_RENDERER_HAS_D3D11)
struct CrossThreadProbe
{
	rts::render::IRenderDevice *device;
	bool contextRejected;
	bool presentRejected;
};

DWORD WINAPI probeD3D11FromWrongThread(void *parameter)
{
	CrossThreadProbe *probe = static_cast<CrossThreadProbe *>(parameter);
	probe->contextRejected = probe->device->immediateContext() == 0;
	probe->presentRejected = probe->device->present() ==
		rts::render::RENDER_RESULT_INVALID_ARGUMENT;
	return 0;
}

int testD3D11HiddenSwapChain()
{
	int result = 0;
	const char *className = "GeneralsRendererContractWindow";
	WNDCLASSEXA windowClass;
	ZeroMemory(&windowClass, sizeof(windowClass));
	windowClass.cbSize = sizeof(windowClass);
	windowClass.lpfnWndProc = DefWindowProcA;
	windowClass.hInstance = GetModuleHandleA(0);
	windowClass.lpszClassName = className;
	const ATOM classAtom = RegisterClassExA(&windowClass);
	result |= check(classAtom != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS,
		"hidden D3D11 test window class registers");
	HWND window = CreateWindowExA(0, className, "", WS_OVERLAPPED,
		0, 0, 64, 64, 0, 0, windowClass.hInstance, 0);
	result |= check(window != 0, "hidden D3D11 test window is created");
	if (window == 0)
	{
		return result;
	}

	rts::render::IRenderDevice *device =
		rts::render::CreateD3D11RenderDevice();
	result |= check(device != 0, "swap-chain D3D11 factory returns a device");
	if (device != 0)
	{
		rts::render::RenderDeviceParameters parameters;
		parameters.backend = rts::render::RENDER_BACKEND_D3D11;
		parameters.window = window;
		parameters.width = 64;
		parameters.height = 64;
		parameters.enableVsync = false;
		parameters.enableDebugLayer = true;
		result |= check(device->initialize(parameters) ==
			rts::render::RENDER_RESULT_OK,
			"flip-model D3D11 swap chain initializes while hidden");

		struct TestVertex
		{
			float x;
			float y;
			float z;
			unsigned int color;
		};
		const TestVertex vertices[3] = {
			{ -0.8f, -0.8f, 0.0f, 0xff0000ffU },
			{ 0.0f, 0.8f, 0.0f, 0xff0000ffU },
			{ 0.8f, -0.8f, 0.0f, 0xff0000ffU }
		};
		rts::render::BufferDescriptor vertexDescriptor;
		vertexDescriptor.byteCount = sizeof(vertices);
		vertexDescriptor.stride = sizeof(TestVertex);
		rts::render::GpuHandle vertexBuffer;
		result |= check(device->createBuffer(vertexDescriptor, vertices,
			sizeof(vertices), &vertexBuffer) == rts::render::RENDER_RESULT_OK,
			"D3D11 parity probe creates an immutable vertex buffer");

		rts::render::IRenderContext *context = device->immediateContext();
		rts::render::LegacyLogicalState logicalState;
		const rts::render::RenderFloat4 clearColor(0.0f, 0.0f, 1.0f, 1.0f);
		result |= check(context != 0 &&
			context->beginFrame() == rts::render::RENDER_RESULT_OK &&
			context->clear(clearColor, 1.0f, 0) == rts::render::RENDER_RESULT_OK &&
			context->setViewport(0.0f, 0.0f, 64.0f, 64.0f, 0.0f, 1.0f) ==
				rts::render::RENDER_RESULT_OK &&
			context->setLegacyState(logicalState,
				rts::render::RENDER_VERTEX_POSITION3_COLOR, 0) ==
				rts::render::RENDER_RESULT_OK &&
			context->setVertexBuffer(vertexBuffer, sizeof(TestVertex), 0) ==
				rts::render::RENDER_RESULT_OK &&
			context->setPrimitiveTopology(
				rts::render::RENDER_PRIMITIVE_TRIANGLE_LIST) ==
				rts::render::RENDER_RESULT_OK &&
			context->draw(3, 0) == rts::render::RENDER_RESULT_OK &&
			context->endFrame() == rts::render::RENDER_RESULT_OK,
			"D3D11 parity pipeline clears and draws through logical state");

		std::vector<unsigned char> pixels(64 * 64 * 4);
		rts::render::RenderFormat captureFormat =
			rts::render::RENDER_FORMAT_UNKNOWN;
		result |= check(device->captureBackBuffer(&pixels[0], pixels.size(),
			64 * 4, &captureFormat) == rts::render::RENDER_RESULT_OK &&
			captureFormat == rts::render::RENDER_FORMAT_B8G8R8A8_UNORM,
			"D3D11 back buffer is available for deterministic capture");
		const unsigned char *corner = &pixels[4 * (2 * 64 + 2)];
		const unsigned char *center = &pixels[4 * (32 * 64 + 32)];
		result |= check(corner[0] > 240 && corner[2] < 16 &&
			center[2] > 240 && center[0] < 16,
			"captured D3D11 triangle preserves clear and vertex colors");

		struct TexturedVertex
		{
			float position[3];
			float normal[3];
			unsigned int color;
			float texture[2];
		};
		const TexturedVertex texturedVertices[3] = {
			{ { -0.8f, -0.8f, 0.0f }, { 0.0f, 0.0f, -1.0f },
				0xffffffffU, { 0.0f, 1.0f } },
			{ { 0.0f, 0.8f, 0.0f }, { 0.0f, 0.0f, -1.0f },
				0xffffffffU, { 0.5f, 0.0f } },
			{ { 0.8f, -0.8f, 0.0f }, { 0.0f, 0.0f, -1.0f },
				0xffffffffU, { 1.0f, 1.0f } }
		};
		rts::render::BufferDescriptor texturedVertexDescriptor;
		texturedVertexDescriptor.byteCount = sizeof(texturedVertices);
		texturedVertexDescriptor.stride = sizeof(TexturedVertex);
		rts::render::GpuHandle texturedVertexBuffer;
		result |= check(device->createBuffer(texturedVertexDescriptor,
			texturedVertices, sizeof(texturedVertices), &texturedVertexBuffer) ==
			rts::render::RENDER_RESULT_OK,
			"D3D11 parity probe creates a textured vertex buffer");
		const unsigned int greenPixels[4] = {
			0xff00ff00U, 0xff00ff00U, 0xff00ff00U, 0xff00ff00U
		};
		rts::render::TextureDescriptor textureDescriptor;
		textureDescriptor.width = 2;
		textureDescriptor.height = 2;
		textureDescriptor.format = rts::render::RENDER_FORMAT_R8G8B8A8_UNORM;
		rts::render::TextureSubresourceData textureData;
		textureData.data = greenPixels;
		textureData.rowPitch = 2 * sizeof(unsigned int);
		textureData.slicePitch = sizeof(greenPixels);
		rts::render::GpuHandle texture;
		result |= check(device->createTexture(textureDescriptor, &textureData, 1,
			&texture) == rts::render::RENDER_RESULT_OK,
			"D3D11 parity probe creates an immutable shader texture");
		logicalState.pipeline.textureStages[0].colorOperation =
			rts::render::RENDER_TEXTURE_OP_MODULATE;
		result |= check(context->beginFrame() == rts::render::RENDER_RESULT_OK &&
			context->clear(clearColor, 1.0f, 0) == rts::render::RENDER_RESULT_OK &&
			context->setViewport(0.0f, 0.0f, 64.0f, 64.0f, 0.0f, 1.0f) ==
				rts::render::RENDER_RESULT_OK &&
			context->setLegacyState(logicalState,
				rts::render::RENDER_VERTEX_POSITION3_NORMAL_COLOR_TEX1, 1) ==
				rts::render::RENDER_RESULT_OK &&
			context->setVertexBuffer(texturedVertexBuffer,
				sizeof(TexturedVertex), 0) == rts::render::RENDER_RESULT_OK &&
			context->setTexture(0, texture) == rts::render::RENDER_RESULT_OK &&
			context->setPrimitiveTopology(
				rts::render::RENDER_PRIMITIVE_TRIANGLE_LIST) ==
				rts::render::RENDER_RESULT_OK &&
			context->draw(3, 0) == rts::render::RENDER_RESULT_OK &&
			context->endFrame() == rts::render::RENDER_RESULT_OK &&
			device->captureBackBuffer(&pixels[0], pixels.size(), 64 * 4,
				&captureFormat) == rts::render::RENDER_RESULT_OK,
			"D3D11 logical texture state draws through a shader-resource handle");
		center = &pixels[4 * (32 * 64 + 32)];
		result |= check(center[1] > 240 && center[0] < 16 && center[2] < 16,
			"captured D3D11 textured triangle preserves sampled color");
		unsigned int debugErrorCount = 0xffffffffU;
		result |= check(device->getDebugValidationErrorCount(&debugErrorCount) ==
			rts::render::RENDER_RESULT_OK && debugErrorCount == 0,
			"D3D11 debug layer reports no validation errors");
		result |= check(device->destroyResource(vertexBuffer) &&
			device->destroyResource(texturedVertexBuffer) &&
			device->destroyResource(texture) &&
			device->present() == rts::render::RENDER_RESULT_OK &&
			device->resize(96, 80) == rts::render::RENDER_RESULT_OK &&
			device->present() == rts::render::RENDER_RESULT_OK,
			"hidden flip-model swap chain presents and resizes");
		device->shutdown();
		delete device;
	}
	DestroyWindow(window);
	return result;
}

int testD3D11HeadlessDevice()
{
	int result = 0;
	rts::render::IRenderDevice *device =
		rts::render::CreateD3D11RenderDevice();
	result |= check(device != 0, "D3D11 factory returns a device");
	if (device == 0)
	{
		return result;
	}

	rts::render::RenderDeviceParameters parameters;
	parameters.backend = rts::render::RENDER_BACKEND_D3D11;
	parameters.width = 64;
	parameters.height = 64;
	parameters.enableVsync = false;
	result |= check(device->backend() == rts::render::RENDER_BACKEND_D3D11 &&
		device->initialize(parameters) == rts::render::RENDER_RESULT_OK,
		"headless D3D11 feature-level-11 device initializes");
	result |= check(device->immediateContext() != 0,
		"initialized D3D11 device exposes its owner context");
	CrossThreadProbe probe = { device, false, false };
	HANDLE thread = CreateThread(0, 0, probeD3D11FromWrongThread, &probe, 0, 0);
	result |= check(thread != 0, "D3D11 ownership probe thread starts");
	if (thread != 0)
	{
		const DWORD waitResult = WaitForSingleObject(thread, 5000);
		CloseHandle(thread);
		result |= check(waitResult == WAIT_OBJECT_0 &&
			probe.contextRejected && probe.presentRejected,
			"D3D11 immediate context rejects non-owner access");
	}

	rts::render::BufferDescriptor descriptor;
	descriptor.byteCount = 64;
	descriptor.stride = 16;
	descriptor.usage = rts::render::RENDER_USAGE_DYNAMIC;
	rts::render::GpuHandle buffer;
	result |= check(device->createBuffer(descriptor, 0, 0, &buffer) ==
		rts::render::RENDER_RESULT_OK && buffer.isValid(),
		"D3D11 dynamic buffer receives a logical handle");
	unsigned int values[4] = { 1, 2, 3, 4 };
	result |= check(device->immediateContext()->beginFrame() ==
		rts::render::RENDER_RESULT_OK &&
		device->immediateContext()->updateBuffer(buffer, values,
			sizeof(values), 0) == rts::render::RENDER_RESULT_OK &&
		device->immediateContext()->endFrame() ==
			rts::render::RENDER_RESULT_OK,
		"owner context maps and updates dynamic buffers");
	result |= check(device->destroyResource(buffer) &&
		!device->destroyResource(buffer),
		"D3D11 resource destruction rejects stale handles");
	rts::render::BufferDescriptor immutableDescriptor;
	immutableDescriptor.byteCount = 16;
	rts::render::GpuHandle invalidBuffer;
	result |= check(device->createBuffer(immutableDescriptor, 0, 0,
		&invalidBuffer) == rts::render::RENDER_RESULT_INVALID_ARGUMENT &&
		!invalidBuffer.isValid(),
		"immutable D3D11 resources require complete initial data");
	result |= check(device->present() == rts::render::RENDER_RESULT_OK,
		"headless presentation is a successful no-op");
	device->shutdown();
	device->shutdown();
	delete device;
	return result;
}
#endif
}

int main()
{
	int result = 0;
	result |= testBackendNames();
	result |= testGenerationSafeHandles();
	result |= testNeutralDescriptorDefaults();
	result |= testLegacyLogicalState();
#if defined(RTS_RENDERER_HAS_D3D11)
	result |= testD3D11HeadlessDevice();
	result |= testD3D11HiddenSwapChain();
#endif
	if (result == 0)
	{
		printf("Renderer contract tests passed.\n");
	}
	return result;
}
