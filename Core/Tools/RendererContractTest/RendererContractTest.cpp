#include "Renderer/RendererDevice.h"

#include <stdio.h>

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
		result |= check(device->initialize(parameters) ==
			rts::render::RENDER_RESULT_OK,
			"flip-model D3D11 swap chain initializes while hidden");
		result |= check(device->present() == rts::render::RENDER_RESULT_OK &&
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
