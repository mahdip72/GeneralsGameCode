#include "Utility/CppMacros.h"
#include "nativew3d2.h"
#include "Renderer/LegacyRenderState.h"
#include "Renderer/NativeW3DRenderState.h"
#include "Renderer/ThreadedRenderDevice.h"

#include <cstdio>
#include <climits>
#include <new>
#include <windows.h>

namespace rts
{
namespace render
{
// The production renderer intentionally keeps its backend device private. A
// test-only friend exposes the existing deterministic fault hook without
// widening the game-facing owner ABI or returning a backend pointer to game
// code.
class NativeW3DRecoveryTestAccess
{
public:
	static void RecordFrameFailure(NativeW3DRenderer *renderer, RenderResult result)
	{
		renderer->RecordFrameFailure(result);
	}

	static RenderResult ConfigureResourceFault(NativeW3DRenderer *renderer,
		RenderResourceFaultPoint point, unsigned int failOnInvocation,
		RenderResult result)
	{
		if (renderer == 0 || renderer->m_state == 0)
			return RENDER_RESULT_INVALID_ARGUMENT;
		IRenderDevice *device = renderer->m_state->Device();
		return device == 0 ? RENDER_RESULT_INVALID_ARGUMENT :
			device->configureResourceFaultInjection(point, failOnInvocation,
				result);
	}

	static RenderResult GetResourceStatistics(NativeW3DRenderer *renderer,
		RenderResourceStatistics *statistics)
	{
		if (renderer == 0 || renderer->m_state == 0)
			return RENDER_RESULT_INVALID_ARGUMENT;
		IRenderDevice *device = renderer->m_state->Device();
		return device == 0 ? RENDER_RESULT_INVALID_ARGUMENT :
			device->getDebugResourceStatistics(statistics);
	}

	static bool IsThreaded(NativeW3DRenderer *renderer)
	{
		return renderer != 0 && renderer->m_state != 0 &&
			IsThreadedRenderDevice(renderer->m_state->Device());
	}
};
}
}

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
	std::fprintf(stderr, "FAIL: %s\n", message);
	return 1;
}

struct NativeVertex
{
	float x;
	float y;
	float z;
	unsigned int color;
};

struct DestroyResourcesRequest
{
	rts::render::NativeW3DResources *resources;
};

struct ShutdownRequest
{
	NativeW3D2 *owner;
	rts::render::RenderResult result;
};

struct CaptureProbe
{
	NativeW3D2 *owner;
	unsigned int completed;
	unsigned int cancelled;
	rts::render::RenderResult cancellationReason;
	bool attemptShutdown;
	rts::render::RenderResult shutdownResult;
	bool frameWasOpen;
	unsigned int *presentCalls;
	unsigned int presentCallsAtCompletion;
};

struct ThreadedCaptureFactoryContext
{
	ThreadedCaptureFactoryContext() : failCapture(false), failClear(false),
		presentCalls(0), captureCalls(0) {}

	bool failCapture;
	bool failClear;
	unsigned int presentCalls;
	unsigned int captureCalls;
};

struct ThrowingCleanupHook : public rts::render::GameRenderCleanupHook
{
	ThrowingCleanupHook() : owner(0), probeReentry(false), releaseCalls(0),
		reacquireCalls(0), releaseShutdownResult(rts::render::RENDER_RESULT_OK),
		reacquireShutdownResult(rts::render::RENDER_RESULT_OK) {}

	virtual void ReleaseResources()
	{
		++releaseCalls;
		if (probeReentry && owner != 0)
		{
			(void)rts::render::IsNativeGameRendererActive();
			releaseShutdownResult = owner->Shutdown();
		}
	}

	virtual void ReAcquireResources()
	{
		++reacquireCalls;
		if (probeReentry && owner != 0)
		{
			(void)rts::render::IsNativeGameRendererActive();
			reacquireShutdownResult = owner->Shutdown();
		}
		throw 1;
	}

	NativeW3D2 *owner;
	bool probeReentry;
	unsigned int releaseCalls;
	unsigned int reacquireCalls;
	rts::render::RenderResult releaseShutdownResult;
	rts::render::RenderResult reacquireShutdownResult;
};

// The borrowed-threaded capture test keeps the real D3D11 device while
// injecting only the capture result. The wrapper is created on the threaded
// owner, so every forwarded backend call retains the same owner affinity as
// production without adding a test-only backend to the product graph.
class ThreadedCaptureBackend final : public rts::render::IRenderDevice,
	public rts::render::IRenderContext
{
public:
	explicit ThreadedCaptureBackend(ThreadedCaptureFactoryContext *context) :
		m_contextState(context), m_backend(rts::render::CreateD3D11RenderDevice()),
		m_backendContext(0) {}

	~ThreadedCaptureBackend() override
	{
		delete m_backend;
		m_backend = 0;
		m_backendContext = 0;
	}

	rts::render::RenderBackend backend() const override
	{
		return rts::render::RENDER_BACKEND_D3D11;
	}

	bool isOperational() const override
	{
		return m_backend != 0 && m_backend->isOperational();
	}

	rts::render::RenderResult initialize(
		const rts::render::RenderDeviceParameters &parameters) override
	{
		if (m_backend == 0)
			return rts::render::RENDER_RESULT_OUT_OF_MEMORY;
		const rts::render::RenderResult result = m_backend->initialize(
			parameters);
		if (result == rts::render::RENDER_RESULT_OK)
			m_backendContext = m_backend->immediateContext();
		return result;
	}

	void shutdown() override
	{
		if (m_backend != 0)
			m_backend->shutdown();
		m_backendContext = 0;
	}

	rts::render::IRenderContext *immediateContext() override
	{
		return m_backendContext == 0 ? 0 : this;
	}

	rts::render::RenderResult createBuffer(
		const rts::render::BufferDescriptor &descriptor,
		const void *initialData, size_t initialDataBytes,
		rts::render::GpuHandle *buffer) override
	{
		return m_backend == 0 ? rts::render::RENDER_RESULT_INVALID_ARGUMENT :
			m_backend->createBuffer(descriptor, initialData, initialDataBytes,
				buffer);
	}

	rts::render::RenderResult createTexture(
		const rts::render::TextureDescriptor &descriptor,
		const rts::render::TextureSubresourceData *initialData,
		unsigned int initialDataCount, rts::render::GpuHandle *texture) override
	{
		return m_backend == 0 ? rts::render::RENDER_RESULT_INVALID_ARGUMENT :
			m_backend->createTexture(descriptor, initialData, initialDataCount,
				texture);
	}

	rts::render::RenderResult refreshTexture(
		rts::render::GpuHandle texture,
		const rts::render::TextureDescriptor &descriptor,
		const rts::render::TextureSubresourceData *data,
		unsigned int dataCount) override
	{
		return m_backend == 0 ? rts::render::RENDER_RESULT_INVALID_ARGUMENT :
			m_backend->refreshTexture(texture, descriptor, data, dataCount);
	}

	rts::render::RenderResult copyActiveColorTargetToTexture(
		rts::render::GpuHandle texture) override
	{
		return m_backend == 0 ? rts::render::RENDER_RESULT_INVALID_ARGUMENT :
			m_backend->copyActiveColorTargetToTexture(texture);
	}

	bool destroyResource(rts::render::GpuHandle resource) override
	{
		return m_backend != 0 && m_backend->destroyResource(resource);
	}

	rts::render::RenderResult recoverDevice() override
	{
		return m_backend == 0 ? rts::render::RENDER_RESULT_INVALID_ARGUMENT :
			m_backend->recoverDevice();
	}

	rts::render::RenderResult resize(unsigned int width,
		unsigned int height) override
	{
		return m_backend == 0 ? rts::render::RENDER_RESULT_INVALID_ARGUMENT :
			m_backend->resize(width, height);
	}

	rts::render::RenderResult present() override
	{
		if (m_contextState != 0)
			++m_contextState->presentCalls;
		return m_backend == 0 ? rts::render::RENDER_RESULT_INVALID_ARGUMENT :
			m_backend->present();
	}

	rts::render::RenderResult getBackBufferInfo(
		rts::render::RenderBackBufferInfo *info) const override
	{
		return m_backend == 0 ? rts::render::RENDER_RESULT_INVALID_ARGUMENT :
			m_backend->getBackBufferInfo(info);
	}

	rts::render::RenderResult getTextureFilterCapabilities(
		rts::render::RenderTextureFilterCapabilities *capabilities) const override
	{
		return m_backend == 0 ? rts::render::RENDER_RESULT_INVALID_ARGUMENT :
			m_backend->getTextureFilterCapabilities(capabilities);
	}

	rts::render::RenderResult captureBackBuffer(void *destination,
		size_t destinationBytes, size_t destinationRowPitch,
		rts::render::RenderFormat *format) override
	{
		if (m_contextState != 0)
			++m_contextState->captureCalls;
		if (m_contextState != 0 && m_contextState->failCapture)
			return rts::render::RENDER_RESULT_FAILED;
		return m_backend == 0 ? rts::render::RENDER_RESULT_INVALID_ARGUMENT :
			m_backend->captureBackBuffer(destination, destinationBytes,
				destinationRowPitch, format);
	}

	rts::render::RenderResult getDebugValidationErrorCount(
		unsigned int *count) const override
	{
		return m_backend == 0 ? rts::render::RENDER_RESULT_INVALID_ARGUMENT :
			m_backend->getDebugValidationErrorCount(count);
	}

	rts::render::RenderResult configureResourceFaultInjection(
		rts::render::RenderResourceFaultPoint point,
		unsigned int failOnInvocation, rts::render::RenderResult result) override
	{
		return m_backend == 0 ? rts::render::RENDER_RESULT_INVALID_ARGUMENT :
			m_backend->configureResourceFaultInjection(point, failOnInvocation,
				result);
	}

	rts::render::RenderResult getDebugResourceStatistics(
		rts::render::RenderResourceStatistics *statistics) const override
	{
		return m_backend == 0 ? rts::render::RENDER_RESULT_INVALID_ARGUMENT :
			m_backend->getDebugResourceStatistics(statistics);
	}

	rts::render::RenderResult reportDebugLiveObjects() override
	{
		return m_backend == 0 ? rts::render::RENDER_RESULT_INVALID_ARGUMENT :
			m_backend->reportDebugLiveObjects();
	}

	rts::render::RenderResult beginFrame() override
	{
		return m_backendContext == 0 ?
			rts::render::RENDER_RESULT_INVALID_ARGUMENT :
			m_backendContext->beginFrame();
	}

	rts::render::RenderResult updateBuffer(rts::render::GpuHandle buffer,
		const void *data, size_t byteCount, size_t destinationOffset,
		rts::render::RenderBufferUpdateMode mode) override
	{
		return m_backendContext == 0 ?
			rts::render::RENDER_RESULT_INVALID_ARGUMENT :
			m_backendContext->updateBuffer(buffer, data, byteCount,
				destinationOffset, mode);
	}

	rts::render::RenderResult clear(const rts::render::RenderFloat4 &color,
		float depth, unsigned int stencil) override
	{
		return m_backendContext == 0 ?
			rts::render::RENDER_RESULT_INVALID_ARGUMENT :
			m_backendContext->clear(color, depth, stencil);
	}

	rts::render::RenderResult clearTargets(unsigned int clearFlags,
		const rts::render::RenderFloat4 &color, float depth,
		unsigned int stencil) override
	{
		if (m_contextState != 0 && m_contextState->failClear)
			return rts::render::RENDER_RESULT_FAILED;
		return m_backendContext == 0 ?
			rts::render::RENDER_RESULT_INVALID_ARGUMENT :
			m_backendContext->clearTargets(clearFlags, color, depth, stencil);
	}

	rts::render::RenderResult setRenderTargets(
		const rts::render::RenderTargetBinding &binding) override
	{
		return m_backendContext == 0 ?
			rts::render::RENDER_RESULT_INVALID_ARGUMENT :
			m_backendContext->setRenderTargets(binding);
	}

	rts::render::RenderResult setRenderTargets(
		rts::render::GpuHandle colorTarget,
		rts::render::GpuHandle depthTarget) override
	{
		return m_backendContext == 0 ?
			rts::render::RENDER_RESULT_INVALID_ARGUMENT :
			m_backendContext->setRenderTargets(colorTarget, depthTarget);
	}

	rts::render::RenderResult setViewport(float x, float y, float width,
		float height, float minimumDepth, float maximumDepth) override
	{
		return m_backendContext == 0 ?
			rts::render::RENDER_RESULT_INVALID_ARGUMENT :
			m_backendContext->setViewport(x, y, width, height, minimumDepth,
				maximumDepth);
	}

	rts::render::RenderResult setLegacyState(
		const rts::render::LegacyLogicalState &state,
		rts::render::LegacyVertexFormat vertexFormat,
		unsigned int texturePresenceMask) override
	{
		return m_backendContext == 0 ?
			rts::render::RENDER_RESULT_INVALID_ARGUMENT :
			m_backendContext->setLegacyState(state, vertexFormat,
				texturePresenceMask);
	}

	rts::render::RenderResult setLegacyStateForLayout(
		const rts::render::LegacyLogicalState &state,
		const rts::render::LegacyVertexLayout &vertexLayout,
		unsigned int texturePresenceMask) override
	{
		return m_backendContext == 0 ?
			rts::render::RENDER_RESULT_INVALID_ARGUMENT :
			m_backendContext->setLegacyStateForLayout(state, vertexLayout,
				texturePresenceMask);
	}

	rts::render::RenderResult setVertexBuffer(
		rts::render::GpuHandle buffer, unsigned int stride,
		unsigned int offset) override
	{
		return m_backendContext == 0 ?
			rts::render::RENDER_RESULT_INVALID_ARGUMENT :
			m_backendContext->setVertexBuffer(buffer, stride, offset);
	}

	rts::render::RenderResult setIndexBuffer(
		rts::render::GpuHandle buffer, rts::render::RenderFormat format,
		unsigned int offset) override
	{
		return m_backendContext == 0 ?
			rts::render::RENDER_RESULT_INVALID_ARGUMENT :
			m_backendContext->setIndexBuffer(buffer, format, offset);
	}

	rts::render::RenderResult setTexture(unsigned int stage,
		rts::render::GpuHandle texture) override
	{
		return m_backendContext == 0 ?
			rts::render::RENDER_RESULT_INVALID_ARGUMENT :
			m_backendContext->setTexture(stage, texture);
	}

	rts::render::RenderResult setPrimitiveTopology(
		rts::render::RenderPrimitiveTopology topology) override
	{
		return m_backendContext == 0 ?
			rts::render::RENDER_RESULT_INVALID_ARGUMENT :
			m_backendContext->setPrimitiveTopology(topology);
	}

	rts::render::RenderResult draw(unsigned int vertexCount,
		unsigned int startVertex) override
	{
		return m_backendContext == 0 ?
			rts::render::RENDER_RESULT_INVALID_ARGUMENT :
			m_backendContext->draw(vertexCount, startVertex);
	}

	rts::render::RenderResult drawIndexed(unsigned int indexCount,
		unsigned int startIndex, int baseVertex) override
	{
		return m_backendContext == 0 ?
			rts::render::RENDER_RESULT_INVALID_ARGUMENT :
			m_backendContext->drawIndexed(indexCount, startIndex, baseVertex);
	}

	rts::render::RenderResult endFrame() override
	{
		return m_backendContext == 0 ?
			rts::render::RENDER_RESULT_INVALID_ARGUMENT :
			m_backendContext->endFrame();
	}

private:
	ThreadedCaptureFactoryContext *m_contextState;
	rts::render::IRenderDevice *m_backend;
	rts::render::IRenderContext *m_backendContext;
};

rts::render::IRenderDevice *CreateThreadedCaptureBackend(void *context)
{
	return new (std::nothrow) ThreadedCaptureBackend(
		static_cast<ThreadedCaptureFactoryContext *>(context));
}

DWORD WINAPI DestroyResourcesFromWorker(void *parameter)
{
	DestroyResourcesRequest *request = static_cast<DestroyResourcesRequest *>(parameter);
	delete request->resources;
	request->resources = 0;
	return 0;
}

DWORD WINAPI ShutdownFromWorker(void *parameter)
{
	ShutdownRequest *request = static_cast<ShutdownRequest *>(parameter);
	request->result = request->owner->Shutdown();
	return 0;
}

void CaptureCompleted(void *consumer, const rts::render::RenderCaptureHandle *,
	unsigned int, unsigned int, size_t, rts::render::RenderFormat,
	const void *, size_t)
{
	CaptureProbe *probe = static_cast<CaptureProbe *>(consumer);
	++probe->completed;
	probe->frameWasOpen = probe->owner->Renderer().IsFrameOpen();
	if (probe->presentCalls != 0)
		probe->presentCallsAtCompletion = *probe->presentCalls;
}

void CaptureCancelled(void *consumer, const rts::render::RenderCaptureHandle *,
	rts::render::RenderResult reason)
{
	CaptureProbe *probe = static_cast<CaptureProbe *>(consumer);
	++probe->cancelled;
	probe->cancellationReason = reason;
	if (probe->attemptShutdown)
	{
		(void)rts::render::IsNativeGameRendererActive();
		probe->shutdownResult = probe->owner->Shutdown();
	}
}

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

int TestBorrowedThreadedCapture(HWND window)
{
	int result = 0;
	ThreadedCaptureFactoryContext factoryContext;
	rts::render::ThreadedRenderOptions options;
	options.serial = true;
	options.maxFramesInFlight = 2;
	options.maxPacketBytes = 1024 * 1024;
	options.maxPacketCommands = 256;
	options.resourceCapacity = 16;
	rts::render::IRenderDevice *device =
		rts::render::CreateThreadedRenderDevice(
		CreateThreadedCaptureBackend, &factoryContext, options);
	result |= Check(device != 0,
		"borrowed threaded capture fixture allocates a device");
	if (device == 0)
		return result;
	rts::render::RenderDeviceParameters parameters;
	parameters.backend = rts::render::RENDER_BACKEND_D3D11;
	parameters.window = window;
	parameters.width = 64;
	parameters.height = 64;
	parameters.enableVsync = false;
	parameters.allowSoftwareFallback = true;
	const rts::render::RenderResult initializeResult =
		device->initialize(parameters);
	if (initializeResult == rts::render::RENDER_RESULT_UNSUPPORTED)
	{
		device->shutdown();
		delete device;
		return 77;
	}
	result |= Check(initializeResult == rts::render::RENDER_RESULT_OK,
		"borrowed threaded capture fixture initializes D3D11");
	if (initializeResult != rts::render::RENDER_RESULT_OK)
	{
		device->shutdown();
		delete device;
		return result;
	}
	NativeW3D2 owner;
	result |= Check(owner.AttachBackend(device, device->immediateContext()) ==
		rts::render::RENDER_RESULT_OK,
		"NativeW3D2 attaches the threaded capture owner without a second device");
	if (result != 0)
	{
		device->shutdown();
		delete device;
		return result;
	}

	rts::render::RenderCaptureRequestDescriptor captureDescriptor;
	CaptureProbe capture;
	capture.owner = &owner;
	capture.completed = 0;
	capture.cancelled = 0;
	capture.cancellationReason = rts::render::RENDER_RESULT_OK;
	capture.attemptShutdown = false;
	capture.shutdownResult = rts::render::RENDER_RESULT_OK;
	capture.frameWasOpen = true;
	capture.presentCalls = &factoryContext.presentCalls;
	capture.presentCallsAtCompletion = 0;
	captureDescriptor.kind = rts::render::RENDER_CAPTURE_WW3D_SCREENSHOT;
	captureDescriptor.consumer = &capture;
	captureDescriptor.completed = CaptureCompleted;
	captureDescriptor.cancelled = CaptureCancelled;
	rts::render::RenderCaptureHandle handle;
	result |= Check(owner.QueueGameBackBufferCapture(captureDescriptor, &handle) ==
		rts::render::RENDER_RESULT_OK &&
		owner.Renderer().BeginFrame() == rts::render::RENDER_RESULT_OK,
		"borrowed threaded owner opens a frame with a pending capture");
	rts::render::GameRenderCommand endCommand = {};
	endCommand.type = rts::render::GAME_RENDER_COMMAND_END_RENDER;
	endCommand.value0 = 1;
	result |= Check(owner.ExecuteGameRenderCommand(endCommand) ==
		rts::render::RENDER_RESULT_OK && capture.completed == 1 &&
		capture.cancelled == 0 && !capture.frameWasOpen &&
		factoryContext.captureCalls == 1 && factoryContext.presentCalls == 1 &&
		capture.presentCallsAtCompletion == 1,
		"borrowed threaded capture finalizes after readback and presents once");

	endCommand.value0 = 0;
	result |= Check(owner.Renderer().BeginFrame() == rts::render::RENDER_RESULT_OK &&
		owner.ExecuteGameRenderCommand(endCommand) ==
			rts::render::RENDER_RESULT_OK && factoryContext.presentCalls == 1,
		"borrowed threaded non-present frame finalizes without presenting");
	// The next frame proves the explicit non-present finalizer cleared the
	// ThreadedRenderDevice recording/ended state rather than leaving it stuck.
	result |= Check(owner.Renderer().BeginFrame() == rts::render::RENDER_RESULT_OK &&
		owner.ExecuteGameRenderCommand(endCommand) ==
			rts::render::RENDER_RESULT_OK,
		"borrowed threaded owner accepts the frame after non-present finalization");

	CaptureProbe failedCapture;
	failedCapture.owner = &owner;
	failedCapture.completed = 0;
	failedCapture.cancelled = 0;
	failedCapture.cancellationReason = rts::render::RENDER_RESULT_OK;
	failedCapture.attemptShutdown = true;
	failedCapture.shutdownResult = rts::render::RENDER_RESULT_OK;
	failedCapture.frameWasOpen = true;
	failedCapture.presentCalls = 0;
	failedCapture.presentCallsAtCompletion = 0;
	captureDescriptor.consumer = &failedCapture;
	factoryContext.failCapture = true;
	endCommand.value0 = 1;
	result |= Check(owner.QueueGameBackBufferCapture(captureDescriptor, &handle) ==
		rts::render::RENDER_RESULT_OK &&
		owner.Renderer().BeginFrame() == rts::render::RENDER_RESULT_OK,
		"borrowed threaded owner opens a frame for capture failure");
	rts::render::RenderResult captureFailure =
		rts::render::RENDER_RESULT_INVALID_ARGUMENT;
	{
		// Production command dispatch pins the aggregate for the entire virtual
		// call. Reproduce that boundary so a cancellation callback can query the
		// owner but cannot destroy it reentrantly.
		rts::render::NativeGameRenderOwnerScope ownerScope;
		captureFailure = owner.ExecuteGameRenderCommand(endCommand);
	}
	result |= Check(captureFailure == rts::render::RENDER_RESULT_FAILED &&
		failedCapture.completed == 0 && failedCapture.cancelled == 1 &&
		failedCapture.cancellationReason ==
			rts::render::RENDER_RESULT_FAILED &&
		failedCapture.shutdownResult ==
			rts::render::RENDER_RESULT_INVALID_ARGUMENT &&
		factoryContext.captureCalls == 2 && factoryContext.presentCalls == 1,
		"borrowed threaded capture failure cancels without presenting or reentrant shutdown");
	factoryContext.failCapture = false;
	result |= Check(owner.Renderer().BeginFrame() == rts::render::RENDER_RESULT_OK &&
		owner.ExecuteGameRenderCommand(endCommand) ==
			rts::render::RENDER_RESULT_OK,
		"borrowed threaded owner accepts a frame after failed capture finalization");

	factoryContext.failClear = true;
	// Even the serial-reference threaded device queues clear commands until
	// submission. Keep the backend fault active through END_RENDER and verify
	// failure at that execution boundary, not at producer queue admission.
	rts::render::GameRenderCommand beginCommand = {};
	beginCommand.type = rts::render::GAME_RENDER_COMMAND_BEGIN_RENDER;
	beginCommand.value0 = rts::render::RENDER_CLEAR_COLOR;
	beginCommand.float0 = 0.0f;
	beginCommand.float1 = 0.0f;
	beginCommand.float2 = 0.0f;
	beginCommand.float3 = 1.0f;
	beginCommand.float4 = 1.0f;
	const rts::render::RenderResult clearAdmission =
		owner.ExecuteGameRenderCommand(beginCommand);
	endCommand.value0 = 0;
	const rts::render::RenderResult clearFailure =
		owner.ExecuteGameRenderCommand(endCommand);
	factoryContext.failClear = false;
	result |= Check(clearAdmission == rts::render::RENDER_RESULT_OK &&
		clearFailure == rts::render::RENDER_RESULT_FAILED &&
		!owner.Renderer().IsFrameOpen(),
		"borrowed threaded clear failure is reported by sealed submission");
	result |= Check(
		owner.Renderer().BeginFrame() == rts::render::RENDER_RESULT_OK &&
		owner.ExecuteGameRenderCommand(endCommand) ==
			rts::render::RENDER_RESULT_OK,
		"borrowed threaded owner accepts a frame after backend clear failure");

	const rts::render::RenderResult failedFrameBegin = owner.Renderer().BeginFrame();
	rts::render::NativeW3DRecoveryTestAccess::RecordFrameFailure(
		&owner.Renderer(), rts::render::RENDER_RESULT_FAILED);
	const rts::render::RenderResult producerFailure =
		owner.ExecuteGameRenderCommand(endCommand);
	result |= Check(failedFrameBegin == rts::render::RENDER_RESULT_OK &&
		producerFailure == rts::render::RENDER_RESULT_FAILED &&
		!owner.Renderer().IsFrameOpen(),
		"borrowed threaded producer frame failure seals the packet");
	result |= Check(owner.Renderer().BeginFrame() == rts::render::RENDER_RESULT_OK &&
		owner.ExecuteGameRenderCommand(endCommand) == rts::render::RENDER_RESULT_OK,
		"borrowed threaded owner accepts a frame after producer failure");

	result |= Check(owner.Shutdown() == rts::render::RENDER_RESULT_OK,
		"borrowed threaded NativeW3D2 shutdown leaves backend ownership external");
	device->shutdown();
	delete device;
	return result;
}

int TestReacquireFailureFailClosed(HWND window)
{
	int result = 0;
	rts::render::NativeW3DRendererDescriptor descriptor;
	descriptor.width = 64;
	descriptor.height = 64;
	descriptor.enableVsync = false;
	descriptor.allowSoftwareFallback = true;
	rts::render::GameRenderCommand resizeCommand = {};
	resizeCommand.type = rts::render::GAME_RENDER_COMMAND_SET_RESOLUTION;
	resizeCommand.value0 = 64;
	resizeCommand.value1 = 64;
	resizeCommand.value2 = 1;

	NativeW3D2 resizeOwner;
	ThrowingCleanupHook resizeHook;
	result |= Check(resizeOwner.Initialize(window, descriptor) ==
		rts::render::RENDER_RESULT_OK,
		"resize failure fixture initializes the native owner");
	if (resizeOwner.IsInitialized())
	{
		resizeOwner.SetGameCleanupHook(&resizeHook);
		resizeHook.owner = &resizeOwner;
		resizeHook.probeReentry = true;
		const unsigned int resizeEpoch = resizeOwner.DisplayIterationEpoch();
		rts::render::RenderResult resizeResult =
			rts::render::RENDER_RESULT_INVALID_ARGUMENT;
		{
			rts::render::NativeGameRenderOwnerScope ownerScope;
			resizeResult = resizeOwner.ExecuteGameRenderCommand(resizeCommand);
		}
		result |= Check(resizeResult == rts::render::RENDER_RESULT_FAILED &&
			resizeHook.releaseCalls == 1 &&
			resizeHook.reacquireCalls == 1 && !resizeOwner.IsOperational() &&
			resizeHook.releaseShutdownResult ==
				rts::render::RENDER_RESULT_INVALID_ARGUMENT &&
			resizeHook.reacquireShutdownResult ==
				rts::render::RENDER_RESULT_INVALID_ARGUMENT &&
			resizeOwner.ActiveRenderTargetKind() ==
				rts::render::GAME_RENDER_TARGET_UNKNOWN &&
			rts::render::GetGameRenderClientNativeOwner() == &resizeOwner,
			"resize ReAcquire exception leaves the native owner published but unavailable");
		result |= Check(resizeOwner.BeginGameDisplayIteration() ==
			rts::render::RENDER_RESULT_INVALID_ARGUMENT &&
			resizeOwner.DisplayIterationEpoch() == resizeEpoch &&
			resizeOwner.SetGameViewport(rts::render::RenderViewport(
				0.0f, 0.0f, 64.0f, 64.0f, 0.0f, 1.0f)) ==
				rts::render::RENDER_RESULT_INVALID_ARGUMENT &&
			resizeOwner.ExecuteGameRenderCommand(resizeCommand) ==
				rts::render::RENDER_RESULT_INVALID_ARGUMENT,
			"resize failure refuses display reset and all later rendering wrappers");
		result |= Check(resizeOwner.Shutdown() ==
			rts::render::RENDER_RESULT_OK,
			"resize failure fixture shuts down after fail-closed publication");
	}

	NativeW3D2 recoverOwner;
	ThrowingCleanupHook recoverHook;
	result |= Check(recoverOwner.Initialize(window, descriptor) ==
		rts::render::RENDER_RESULT_OK,
		"recovery failure fixture initializes the native owner");
	if (recoverOwner.IsInitialized())
	{
		recoverOwner.SetGameCleanupHook(&recoverHook);
		recoverHook.owner = &recoverOwner;
		recoverHook.probeReentry = true;
		const unsigned int recoverEpoch = recoverOwner.DisplayIterationEpoch();
		result |= Check(recoverOwner.RecoverDevice() ==
			rts::render::RENDER_RESULT_FAILED && recoverHook.releaseCalls == 1 &&
			recoverHook.reacquireCalls == 1 && !recoverOwner.IsOperational() &&
			recoverHook.releaseShutdownResult ==
				rts::render::RENDER_RESULT_INVALID_ARGUMENT &&
			recoverHook.reacquireShutdownResult ==
				rts::render::RENDER_RESULT_INVALID_ARGUMENT &&
			recoverOwner.ActiveRenderTargetKind() ==
				rts::render::GAME_RENDER_TARGET_UNKNOWN &&
			rts::render::GetGameRenderClientNativeOwner() == &recoverOwner,
			"device-recovery ReAcquire exception leaves the native owner unavailable");
		result |= Check(recoverOwner.BeginGameDisplayIteration() ==
			rts::render::RENDER_RESULT_INVALID_ARGUMENT &&
			recoverOwner.DisplayIterationEpoch() == recoverEpoch &&
			recoverOwner.SetGameViewport(rts::render::RenderViewport(
				0.0f, 0.0f, 64.0f, 64.0f, 0.0f, 1.0f)) ==
				rts::render::RENDER_RESULT_INVALID_ARGUMENT &&
			recoverOwner.ExecuteGameRenderCommand(
			resizeCommand) == rts::render::RENDER_RESULT_INVALID_ARGUMENT,
			"recovery failure refuses display reset and all later rendering wrappers");
		result |= Check(recoverOwner.Shutdown() ==
			rts::render::RENDER_RESULT_OK,
			"recovery failure fixture shuts down after fail-closed publication");
	}
	return result;
}

int TestNativeHardwareZBias(NativeW3D2 *owner)
{
	int result = 0;
	if (owner == 0 || !owner->IsOperational())
		return Check(false, "Z-bias fixture has an operational native owner");

	// D3D8 positive ZBIAS brings coplanar geometry forward. D3D11 adds its
	// signed rasterizer bias to depth, so the native game seam must negate the
	// logical value for the game's LESS/LESSEQUAL depth convention. This tests
	// the existing unit scale, not equality with a particular D3D8 GPU's pixels.
	rts::render::ResetTrackedLegacyState();
	rts::render::SeedTrackedLegacyPipelineState();
	struct BiasCase
	{
		unsigned int logicalBias;
		int rasterizerBias;
	};
	const BiasCase cases[] = {
		{ 0U, 0 }, { 1U, -1 }, { 8U, -8 }, { 16U, -16 },
		{ static_cast<unsigned int>(INT_MAX), -INT_MAX }
	};
	rts::render::LegacyLogicalState state;
	for (unsigned int index = 0; index != sizeof(cases) / sizeof(cases[0]); ++index)
	{
		result |= Check(owner->SetGameRenderState(
			rts::render::GAME_RENDER_STATE_Z_BIAS, cases[index].logicalBias) ==
			rts::render::RENDER_RESULT_OK &&
			rts::render::GetTrackedLegacyLogicalState(&state) &&
			state.pipeline.rasterizer.depthBias == cases[index].rasterizerBias,
			"native positive logical Z-bias maps toward the viewer without signed overflow");
	}
	result |= Check(owner->SupportsZBias(),
		"native hardware bias suppresses the title's extra physical decal offset");

	result |= Check(owner->SetGameRenderState(
		rts::render::GAME_RENDER_STATE_Z_BIAS, 16U) ==
		rts::render::RENDER_RESULT_OK,
		"Z-bias boundary fixture restores a normal prior value");
	const unsigned int invalidValues[] = {
		static_cast<unsigned int>(INT_MAX) + 1U, UINT_MAX
	};
	for (unsigned int index = 0; index != sizeof(invalidValues) / sizeof(invalidValues[0]); ++index)
	{
		result |= Check(owner->SetGameRenderState(
			rts::render::GAME_RENDER_STATE_Z_BIAS, invalidValues[index]) ==
			rts::render::RENDER_RESULT_INVALID_ARGUMENT &&
			rts::render::GetTrackedLegacyLogicalState(&state) &&
			state.pipeline.rasterizer.depthBias == -16,
			"out-of-range logical Z-bias preserves the prior signed rasterizer bias");
	}
	result |= Check(owner->SetGameRenderState(
		rts::render::GAME_RENDER_STATE_Z_BIAS, 0U) ==
		rts::render::RENDER_RESULT_OK &&
		rts::render::GetTrackedLegacyLogicalState(&state) &&
		state.pipeline.rasterizer.depthBias == 0,
		"clearing native logical Z-bias restores unbiased rasterization");
	return result;
}

bool HasUnmodifiedCameraProjection(const rts::render::LegacyLogicalState &state)
{
	// Hand-written perspective fixture: near=1, far=3, unit X/Y scale.
	const float expected[16] = {
		1.0f, 0.0f, 0.0f, 0.0f,
		0.0f, 1.0f, 0.0f, 0.0f,
		0.0f, 0.0f, -1.5f, -1.0f,
		0.0f, 0.0f, -1.5f, 0.0f
	};
	for (unsigned int index = 0; index != 16; ++index)
		if (state.constants.projection.values[index] != expected[index])
			return false;
	return true;
}

int TestNativeCameraBiasSequences(NativeW3D2 *owner)
{
	int result = 0;
	if (owner == 0 || !owner->IsOperational())
		return Check(false, "camera bias fixture has an operational native owner");
	// Snapshots publish a viewport to the real threaded context. That command
	// belongs to an open frame, just as it does in the title's render loop.
	result |= Check(owner->BeginGameDisplayIteration() ==
		rts::render::RENDER_RESULT_OK && owner->Renderer().BeginFrame() ==
		rts::render::RENDER_RESULT_OK,
		"camera bias fixture begins a real native frame");
	if (result != 0)
		return result;
	rts::render::ResetTrackedLegacyState();
	rts::render::SeedTrackedLegacyPipelineState();
	rts::render::GameCameraSnapshot snapshot;
	snapshot.projection.values[10] = -1.5f;
	snapshot.projection.values[11] = -1.0f;
	snapshot.projection.values[14] = -1.5f;
	snapshot.projection.values[15] = 0.0f;
	snapshot.viewport = rts::render::RenderViewport(
		0.0f, 0.0f, 64.0f, 64.0f, 0.0f, 1.0f);
	snapshot.zNear = 1.0f;
	snapshot.zFar = 3.0f;

	for (unsigned int title = 0; title != 2; ++title)
	{
		// Exercise each title's real native command sequence: Generals applies
		// a plain projection; Zero Hour uses the bias-aware projection command.
		// Both subsequently publish the same raw camera snapshot for meshes.
		rts::render::GameRenderCommand command = {};
		command.type = title == 0 ?
			rts::render::GAME_RENDER_COMMAND_SET_TRANSFORM :
			rts::render::GAME_RENDER_COMMAND_SET_PROJECTION_WITH_Z_BIAS;
		command.value0 = rts::render::LEGACY_TRANSFORM_PROJECTION;
		command.input = &snapshot.projection;
		command.inputBytes = sizeof(snapshot.projection);
		command.float0 = snapshot.zNear;
		command.float1 = snapshot.zFar;
		result |= Check(owner->SetGameRenderState(
			rts::render::GAME_RENDER_STATE_Z_BIAS, 8U) ==
			rts::render::RENDER_RESULT_OK,
			"camera fixture enables the game's decal bias");
		rts::render::LegacyLogicalState state;
		result |= Check(owner->ExecuteGameRenderCommand(command) ==
			rts::render::RENDER_RESULT_OK &&
			rts::render::GetTrackedLegacyLogicalState(&state) &&
			HasUnmodifiedCameraProjection(state),
			title == 0 ? "Generals camera leaves bias exclusively in the rasterizer" :
			"Zero Hour camera does not also fold hardware bias into its projection");
		for (unsigned int repeat = 0; repeat != 3; ++repeat)
		{
			result |= Check(owner->SetGameRenderCameraSnapshot(snapshot) ==
				rts::render::RENDER_RESULT_OK &&
				rts::render::GetTrackedLegacyLogicalState(&state) &&
				HasUnmodifiedCameraProjection(state) &&
				state.pipeline.rasterizer.depthBias == -8,
				"repeated title camera snapshots preserve hardware bias without accumulating projection bias");
		}
		result |= Check(owner->SetGameRenderState(
			rts::render::GAME_RENDER_STATE_Z_BIAS, 16U) ==
			rts::render::RENDER_RESULT_OK &&
			owner->ExecuteGameRenderCommand(command) ==
			rts::render::RENDER_RESULT_OK &&
			rts::render::GetTrackedLegacyLogicalState(&state) &&
			HasUnmodifiedCameraProjection(state) &&
			state.pipeline.rasterizer.depthBias == -16,
			"changing bias and reapplying either title camera does not alter projection");
		command.float0 = command.float1 = 2.0f;
		result |= Check(owner->ExecuteGameRenderCommand(command) ==
			rts::render::RENDER_RESULT_OK &&
			rts::render::GetTrackedLegacyLogicalState(&state) &&
			HasUnmodifiedCameraProjection(state),
			"equal clip planes do not introduce a projection fallback or division");
		result |= Check(owner->SetGameRenderState(
			rts::render::GAME_RENDER_STATE_Z_BIAS, 0U) ==
			rts::render::RENDER_RESULT_OK &&
			owner->SetGameRenderCameraSnapshot(snapshot) ==
			rts::render::RENDER_RESULT_OK &&
			rts::render::GetTrackedLegacyLogicalState(&state) &&
			HasUnmodifiedCameraProjection(state) &&
			state.pipeline.rasterizer.depthBias == 0,
			"ending either title's biased pass restores the unmodified camera and zero bias");
	}
	const rts::render::RenderResult endResult = owner->Renderer().EndFrame(false);
	const rts::render::RenderResult finalizeResult = endResult ==
		rts::render::RENDER_RESULT_OK ? owner->Renderer().FinalizeEndedFrame(false) :
		endResult;
	const rts::render::RenderResult drainResult = owner->Renderer().DrainThreaded();
	result |= Check(endResult == rts::render::RENDER_RESULT_OK &&
		finalizeResult == rts::render::RENDER_RESULT_OK &&
		drainResult == rts::render::RENDER_RESULT_OK,
		"camera bias fixture completes its native frame without presentation");
	return result;
}

int TestNativeCommandsPreservePipelineState(NativeW3D2 *owner)
{
	int result = 0;
	if (owner == 0 || !owner->IsOperational())
		return Check(false, "pipeline preservation fixture has an operational owner");
	rts::render::RenderMatrix4 view;
	view.values[12] = 5.0f;
	const rts::render::RenderFloat4 constants(1.0f, 2.0f, 3.0f, 4.0f);
	const rts::render::GameRenderCommandType types[] = {
		rts::render::GAME_RENDER_COMMAND_SET_TRANSFORM,
		rts::render::GAME_RENDER_COMMAND_APPLY_RENDER_STATE_CHANGES,
		rts::render::GAME_RENDER_COMMAND_SET_VERTEX_SHADER_CONSTANTS,
		rts::render::GAME_RENDER_COMMAND_SET_PIXEL_SHADER_CONSTANTS
	};
	const char *messages[] = {
		"plain transforms preserve live bias, stencil reference, and alpha blending",
		"applying render-state changes preserves live bias, stencil reference, and alpha blending",
		"vertex constants preserve live bias, stencil reference, and alpha blending",
		"pixel constants preserve live bias, stencil reference, and alpha blending"
	};
	for (unsigned int index = 0; index != sizeof(types) / sizeof(types[0]); ++index)
	{
		rts::render::ResetTrackedLegacyState();
		rts::render::SeedTrackedLegacyPipelineState();
		result |= Check(owner->SetGameRenderState(
			rts::render::GAME_RENDER_STATE_Z_BIAS, 8U) ==
			rts::render::RENDER_RESULT_OK &&
			owner->SetGameRenderState(
			rts::render::GAME_RENDER_STATE_STENCIL_REFERENCE, 0x3fU) ==
			rts::render::RENDER_RESULT_OK &&
			owner->SetGameRenderState(
			rts::render::GAME_RENDER_STATE_ALPHA_BLEND_ENABLE, 1U) ==
			rts::render::RENDER_RESULT_OK,
			"pipeline preservation fixture publishes nondefault render state");
		rts::render::GameRenderCommand command = {};
		command.type = types[index];
		if (index == 0)
		{
			command.value0 = rts::render::LEGACY_TRANSFORM_VIEW;
			command.input = &view;
			command.inputBytes = sizeof(view);
		}
		else if (index >= 2)
		{
			command.value1 = 1U;
			command.input = &constants;
			command.inputBytes = sizeof(constants);
		}
		rts::render::LegacyLogicalState state;
		result |= Check(owner->ExecuteGameRenderCommand(command) ==
			rts::render::RENDER_RESULT_OK &&
			rts::render::GetTrackedLegacyLogicalState(&state) &&
			state.pipeline.rasterizer.depthBias == -8 &&
			state.pipeline.depthStencil.stencilReference == 0x3fU &&
			state.pipeline.blend.blendEnable, messages[index]);
		if (index == 0)
			result |= Check(state.constants.view.values[12] == 5.0f,
				"preserving pipeline state still publishes the requested view transform");
		else if (index >= 2)
		{
			const rts::render::RenderFloat4 &published = index == 2 ?
				state.constants.vertexShaderConstants[0] :
				state.constants.pixelShaderConstants[0];
			result |= Check(published.x == 1.0f && published.y == 2.0f &&
				published.z == 3.0f && published.w == 4.0f,
				"preserving pipeline state still publishes the requested shader constants");
		}
	}
	return result;
}

int TestStencilStateEncoding(NativeW3D2 *owner)
{
	int result = 0;
	if (owner == 0 || !owner->IsOperational())
		return Check(false, "stencil compatibility fixture has an operational owner");

	// The Generals title still publishes several D3D8-era DWORD encodings to
	// the native seam even though D3D11 consumes an 8-bit stencil reference and
	// masks.  Seed a clean logical state so each assertion observes only this
	// setter's result rather than a previous test's publication.
	rts::render::ResetTrackedLegacyState();
	rts::render::SeedTrackedLegacyPipelineState();
	rts::render::LegacyLogicalState state;
	result |= Check(owner->SetGameRenderState(
		rts::render::GAME_RENDER_STATE_STENCIL_REFERENCE, 0x80808080U) ==
		rts::render::RENDER_RESULT_OK &&
		rts::render::GetTrackedLegacyLogicalState(&state) &&
		state.pipeline.depthStencil.stencilReference == 0x80U,
		"stencil reference accepts the historical repeated-byte encoding");
	result |= Check(owner->SetGameRenderState(
		rts::render::GAME_RENDER_STATE_STENCIL_READ_MASK, 0xffffffffU) ==
		rts::render::RENDER_RESULT_OK &&
		rts::render::GetTrackedLegacyLogicalState(&state) &&
		state.pipeline.depthStencil.stencilReadMask == 0xffU,
		"stencil read mask accepts the historical full-byte encoding");
	result |= Check(owner->SetGameRenderState(
		rts::render::GAME_RENDER_STATE_STENCIL_WRITE_MASK, 0x80808080U) ==
		rts::render::RENDER_RESULT_OK &&
		rts::render::GetTrackedLegacyLogicalState(&state) &&
		state.pipeline.depthStencil.stencilWriteMask == 0x80U,
		"stencil write mask accepts the historical repeated-byte encoding");
	result |= Check(owner->SetGameRenderState(
		rts::render::GAME_RENDER_STATE_STENCIL_READ_MASK,
		static_cast<unsigned int>(~0xc0U)) ==
		rts::render::RENDER_RESULT_OK &&
		rts::render::GetTrackedLegacyLogicalState(&state) &&
		state.pipeline.depthStencil.stencilReadMask == 0x3fU,
		"stencil read mask preserves the low-byte semantics of a complemented mask");

	// An unrelated wide value remains an error and must not overwrite any of the
	// already-published 8-bit fields.  Exercise every stencil field because the
	// title uses all three setter paths.
	result |= Check(owner->SetGameRenderState(
		rts::render::GAME_RENDER_STATE_STENCIL_REFERENCE, 0x12345678U) ==
		rts::render::RENDER_RESULT_INVALID_ARGUMENT &&
		rts::render::GetTrackedLegacyLogicalState(&state) &&
		state.pipeline.depthStencil.stencilReference == 0x80U,
		"invalid wide stencil reference preserves the prior value and reports an error");
	result |= Check(owner->SetGameRenderState(
		rts::render::GAME_RENDER_STATE_STENCIL_READ_MASK, 0x12345678U) ==
		rts::render::RENDER_RESULT_INVALID_ARGUMENT &&
		rts::render::GetTrackedLegacyLogicalState(&state) &&
		state.pipeline.depthStencil.stencilReadMask == 0x3fU,
		"invalid wide stencil read mask preserves the prior value and reports an error");
	result |= Check(owner->SetGameRenderState(
		rts::render::GAME_RENDER_STATE_STENCIL_WRITE_MASK, 0x12345678U) ==
		rts::render::RENDER_RESULT_INVALID_ARGUMENT &&
		rts::render::GetTrackedLegacyLogicalState(&state) &&
		state.pipeline.depthStencil.stencilWriteMask == 0x80U,
		"invalid wide stencil write mask preserves the prior value and reports an error");

	return result;
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
		std::fprintf(stderr, "FAIL: could not create hidden native window\n");
		return 1;
	}
	// Keep the owned/borrowed threaded lifecycle fixture independent from the
	// longer native contract sequence below. Earlier negative assertions in the
	// latter must not suppress coverage for capture ordering, failed clear
	// sealing, or the serial borrowed-backend teardown path.
	const int threadedResult = TestBorrowedThreadedCapture(window);
	if (threadedResult == 77)
	{
		DestroyWindow(window);
		return 77;
	}
	result |= threadedResult;
	result |= TestReacquireFailureFailClosed(window);
	const rts::render::RenderResult initializeResult = w3d.Initialize(window, descriptor);
	if (initializeResult == rts::render::RENDER_RESULT_UNSUPPORTED)
	{
		DestroyWindow(window);
		return 77;
	}
	result |= Check(initializeResult == rts::render::RENDER_RESULT_OK,
		"native WW3D2 initializes a hidden D3D11 swap chain");
	if (initializeResult == rts::render::RENDER_RESULT_OK)
	{
		result |= TestNativeHardwareZBias(&w3d);
		result |= TestNativeCameraBiasSequences(&w3d);
		result |= TestNativeCommandsPreservePipelineState(&w3d);
		result |= TestStencilStateEncoding(&w3d);
		const bool usesDedicatedThreadedOwner =
			rts::render::NativeW3DRecoveryTestAccess::IsThreaded(
				&w3d.Renderer());
		result |= Check(usesDedicatedThreadedOwner,
			"native WW3D2 uses the dedicated threaded render owner");
		if (usesDedicatedThreadedOwner)
		{
			bool overlapFramesSucceeded = true;
			for (unsigned int frame = 0; frame != 72 && overlapFramesSucceeded;
				++frame)
			{
				if (w3d.BeginGameDisplayIteration() !=
					rts::render::RENDER_RESULT_OK ||
					w3d.Renderer().BeginFrame() !=
						rts::render::RENDER_RESULT_OK ||
					w3d.Renderer().EndFrame(true) !=
						rts::render::RENDER_RESULT_OK)
					overlapFramesSucceeded = false;
			}
			result |= Check(overlapFramesSucceeded,
				"native WW3D2 services completed frames before the mailbox fills");
		}

		rts::render::RenderResourceStatistics beforeOffOwnerShutdown;
		result |= Check(rts::render::NativeW3DRecoveryTestAccess::
			GetResourceStatistics(&w3d.Renderer(), &beforeOffOwnerShutdown) ==
				rts::render::RENDER_RESULT_OK,
			"native WW3D2 reads resource state before off-owner shutdown");
		ShutdownRequest shutdownRequest;
		shutdownRequest.owner = &w3d;
		shutdownRequest.result = rts::render::RENDER_RESULT_OK;
		HANDLE shutdownThread = CreateThread(0, 0, ShutdownFromWorker,
			&shutdownRequest, 0, 0);
		result |= Check(shutdownThread != 0,
			"native WW3D2 starts an off-owner shutdown probe");
		if (shutdownThread != 0)
		{
			WaitForSingleObject(shutdownThread, INFINITE);
			CloseHandle(shutdownThread);
		}
		rts::render::RenderResourceStatistics afterOffOwnerShutdown;
		result |= Check(shutdownRequest.result ==
			rts::render::RENDER_RESULT_INVALID_ARGUMENT &&
			w3d.IsInitialized() &&
			rts::render::GetGameRenderClientNativeOwner() == &w3d &&
			rts::render::NativeW3DRecoveryTestAccess::GetResourceStatistics(
				&w3d.Renderer(), &afterOffOwnerShutdown) ==
				rts::render::RENDER_RESULT_OK &&
			afterOffOwnerShutdown.liveHandles == beforeOffOwnerShutdown.liveHandles &&
			afterOffOwnerShutdown.bufferCount == beforeOffOwnerShutdown.bufferCount &&
			afterOffOwnerShutdown.textureCount == beforeOffOwnerShutdown.textureCount &&
			afterOffOwnerShutdown.nativeResourceCount ==
				beforeOffOwnerShutdown.nativeResourceCount,
			"off-owner shutdown leaves publication and resources untouched");

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
		rts::render::BufferDescriptor staticDescriptor = bufferDescriptor;
		staticDescriptor.usage = rts::render::RENDER_USAGE_DEFAULT;
		rts::render::GpuHandle staticVertexBuffer;
		result |= Check(w3d.Resources().CreateBuffer(staticDescriptor, vertices,
			sizeof(vertices), &staticVertexBuffer) ==
				rts::render::RENDER_RESULT_OK,
			"native WW3D2 creates recoverable static geometry");
		rts::render::BufferDescriptor dynamicDescriptor = bufferDescriptor;
		dynamicDescriptor.usage = rts::render::RENDER_USAGE_DYNAMIC;
		rts::render::GpuHandle dynamicVertexBuffer;
	result |= Check(w3d.Resources().CreateBuffer(dynamicDescriptor, vertices,
		sizeof(vertices), &dynamicVertexBuffer) ==
				rts::render::RENDER_RESULT_OK,
		"native WW3D2 creates explicitly unrestorable dynamic geometry");
	// A buffer created without an initial image has no committed draw range.
	// Its first full write is intentionally made inside a threaded frame so the
	// range must remain unpublished until the aggregate observes completion.
	rts::render::GpuHandle completionBuffer;
	result |= Check(w3d.Resources().CreateBuffer(staticDescriptor, 0, 0,
		&completionBuffer) == rts::render::RENDER_RESULT_OK,
		"native WW3D2 creates a buffer for completion publication");
	if (completionBuffer.isValid())
	{
		rts::render::GpuHandle unpublishedRange;
		const rts::render::RenderResult completionBegin =
			w3d.Renderer().BeginFrame();
		const rts::render::RenderResult updateResult = completionBegin ==
			rts::render::RENDER_RESULT_OK ? w3d.Resources().UpdateBuffer(
				completionBuffer, vertices, sizeof(vertices), 0,
				rts::render::RENDER_BUFFER_UPDATE_PRESERVE) :
			rts::render::RENDER_RESULT_INVALID_ARGUMENT;
		const rts::render::RenderResult beforeCompletion = updateResult ==
			rts::render::RENDER_RESULT_OK ?
			w3d.Resources().AcquireVertexBufferRange(completionBuffer,
				sizeof(NativeVertex), 0, 0, 3, &unpublishedRange) :
			rts::render::RENDER_RESULT_INVALID_ARGUMENT;
		const rts::render::RenderResult completionEnd = completionBegin ==
			rts::render::RENDER_RESULT_OK ? w3d.Renderer().EndFrame(true) :
			rts::render::RENDER_RESULT_INVALID_ARGUMENT;
		const rts::render::RenderResult completionFence = completionEnd ==
			rts::render::RENDER_RESULT_OK ? w3d.Renderer().DrainThreaded() :
			rts::render::RENDER_RESULT_INVALID_ARGUMENT;
		const rts::render::RenderResult completionService = completionFence ==
			rts::render::RENDER_RESULT_OK ? w3d.BeginGameDisplayIteration() :
			rts::render::RENDER_RESULT_INVALID_ARGUMENT;
		rts::render::GpuHandle publishedRange;
		const rts::render::RenderResult afterCompletion = completionService ==
			rts::render::RENDER_RESULT_OK ?
			w3d.Resources().AcquireVertexBufferRange(completionBuffer,
				sizeof(NativeVertex), 0, 0, 3, &publishedRange) :
			rts::render::RENDER_RESULT_INVALID_ARGUMENT;
		result |= Check(updateResult == rts::render::RENDER_RESULT_OK &&
			beforeCompletion == rts::render::RENDER_RESULT_INVALID_ARGUMENT &&
			!unpublishedRange.isValid() && completionEnd ==
			rts::render::RENDER_RESULT_OK && completionFence ==
			rts::render::RENDER_RESULT_OK && completionService ==
			rts::render::RENDER_RESULT_OK && afterCompletion ==
			rts::render::RENDER_RESULT_OK && publishedRange.isValid(),
			"native WW3D2 publishes an in-frame buffer range after completion");
		result |= Check(w3d.Resources().Destroy(completionBuffer),
			"native WW3D2 destroys the completion publication buffer");
	}
	rts::render::NativeDrawPacket packet;
		ConfigurePacket(&packet, vertexBuffer);
		rts::render::LegacyLogicalState state;
		const rts::render::RenderViewport viewport(0.0f, 0.0f, 64.0f,
			64.0f, 0.0f, 1.0f);
		result |= Check(w3d.Renderer().BeginFrame() == rts::render::RENDER_RESULT_OK,
			"native WW3D2 begins a hidden frame");
		result |= Check(w3d.Renderer().SetViewport(viewport) ==
			rts::render::RENDER_RESULT_OK,
			"native WW3D2 applies a neutral viewport descriptor");
		result |= Check(w3d.Renderer().Submit(w3d.Resources(), state, packet) ==
			rts::render::RENDER_RESULT_OK,
			"native WW3D2 submits a triangle through the logical facade");
		// DynamicMesh binds the complete sorting allocation once, then emits
		// per-material runs with narrower index/minimum-vertex ranges.  The
		// command sink must retain the source offsets while validating and
		// copying only the requested sub-range for the deferred sorter.
		const unsigned int sortedStride = rts::render::LegacyFvfVertexSize(
			rts::render::GAME_VERTEX_XYZNDUV2);
		unsigned char sortedVertices[4 * 44] = {};
		const unsigned short sortedIndices[6] = { 2, 3, 4, 3, 4, 5 };
		result |= Check(sortedStride == 44U,
			"dynamic sorting FVF keeps its historical 44-byte stride");
		if (sortedStride == 44U)
		{
			rts::render::GameRenderCommand command = {};
			command.type = rts::render::GAME_RENDER_COMMAND_APPLY_RENDER_STATE_CHANGES;
			result |= Check(w3d.ExecuteGameRenderCommand(command) ==
				rts::render::RENDER_RESULT_OK,
				"native command sink seeds sorted draw state");

			command = {};
			command.type = rts::render::GAME_RENDER_COMMAND_SET_VERTEX_BUFFER;
			command.value0 = rts::render::GAME_VERTEX_XYZNDUV2;
			command.value1 = sortedStride;
			command.value2 = 2U;
			command.value3 = 4U;
			command.value4 = 2U * sortedStride;
			command.input = sortedVertices;
			command.inputBytes = sizeof(sortedVertices);
			result |= Check(w3d.ExecuteGameRenderCommand(command) ==
				rts::render::RENDER_RESULT_OK,
				"native command sink binds the full sorted vertex allocation");

			command = {};
			command.type = rts::render::GAME_RENDER_COMMAND_SET_INDEX_BUFFER;
			command.value0 = rts::render::RENDER_FORMAT_R16_UINT;
			command.value1 = 0U;
			command.value2 = 6U;
			command.input = sortedIndices;
			command.inputBytes = sizeof(sortedIndices);
			result |= Check(w3d.ExecuteGameRenderCommand(command) ==
				rts::render::RENDER_RESULT_OK,
				"native command sink binds the full sorted index allocation");

			command = {};
			command.type = rts::render::GAME_RENDER_COMMAND_DRAW_SORTED_TRIANGLES;
			command.value0 = 0U;
			command.value1 = 1U;
			command.value2 = 1U;
			command.value3 = 1U;
			result |= Check(w3d.ExecuteGameRenderCommand(command) ==
				rts::render::RENDER_RESULT_INVALID_ARGUMENT,
				"native sorted draw rejects a minimum vertex range below its binding");

			command.value0 = 6U;
			command.value2 = 3U;
			command.value3 = 3U;
			result |= Check(w3d.ExecuteGameRenderCommand(command) ==
				rts::render::RENDER_RESULT_INVALID_ARGUMENT,
				"native sorted draw rejects an index range beyond its binding");
			// Invalid commands deliberately latch the active frame failure. Close
			// that negative-test frame and start a clean one before exercising the
			// accepted sub-range; this also verifies failure propagation rather than
			// allowing the negative cases to hide a later success.
			result |= Check(w3d.Renderer().EndFrame(false) ==
				rts::render::RENDER_RESULT_INVALID_ARGUMENT,
				"native sorted command failures propagate at frame end");
			result |= Check(w3d.Renderer().BeginFrame() ==
				rts::render::RENDER_RESULT_OK,
				"native sorted command test starts a clean frame");

			command.value0 = 3U;
			command.value2 = 3U;
			command.value3 = 3U;
			result |= Check(w3d.ExecuteGameRenderCommand(command) ==
				rts::render::RENDER_RESULT_OK,
				"native sorted draw accepts a DynamicMesh sub-range");
			result |= Check(w3d.FlushGameSortedTriangles() ==
				rts::render::RENDER_RESULT_OK,
				"native sorted sub-range copies the selected vertex/index data");
			const rts::render::RenderResult sortedEndResult =
				w3d.Renderer().EndFrame(false);
			const rts::render::RenderResult sortedFinalizeResult =
				sortedEndResult == rts::render::RENDER_RESULT_OK ?
				w3d.Renderer().FinalizeEndedFrame(false) :
				rts::render::RENDER_RESULT_INVALID_ARGUMENT;
			result |= Check(sortedEndResult == rts::render::RENDER_RESULT_OK &&
				sortedFinalizeResult == rts::render::RENDER_RESULT_OK,
				"native sorted success frame closes before cleanup fault injection");
			result |= Check(rts::render::NativeW3DRecoveryTestAccess::
				ConfigureResourceFault(&w3d.Renderer(),
					rts::render::RENDER_RESOURCE_FAULT_BUFFER_DESTRUCTION, 1,
					rts::render::RENDER_RESULT_FAILED) ==
					rts::render::RENDER_RESULT_OK,
				"native sorted cleanup fault injection arms one buffer refusal");
			rts::render::RenderResourceStatistics beforeCleanupFault;
			result |= Check(rts::render::NativeW3DRecoveryTestAccess::
				GetResourceStatistics(&w3d.Renderer(), &beforeCleanupFault) ==
					rts::render::RENDER_RESULT_OK,
				"native sorted cleanup test reads the pre-fault resource count");
			result |= Check(w3d.Renderer().BeginFrame() ==
				rts::render::RENDER_RESULT_OK,
				"native sorted cleanup test starts a faulted frame");
			rts::render::NativeSortedDraw faultDraw;
			faultDraw.state = state;
			faultDraw.packet.vertexStride = sizeof(NativeVertex);
			faultDraw.packet.vertexLayout.stride = sizeof(NativeVertex);
			faultDraw.packet.vertexLayout.elementCount = 2;
			faultDraw.packet.vertexLayout.elements[0].semantic =
				rts::render::RENDER_VERTEX_SEMANTIC_POSITION;
			faultDraw.packet.vertexLayout.elements[0].semanticIndex = 0;
			faultDraw.packet.vertexLayout.elements[0].format =
				rts::render::RENDER_VERTEX_DATA_FLOAT3;
			faultDraw.packet.vertexLayout.elements[0].byteOffset = 0;
			faultDraw.packet.vertexLayout.elements[1].semantic =
				rts::render::RENDER_VERTEX_SEMANTIC_DIFFUSE;
			faultDraw.packet.vertexLayout.elements[1].semanticIndex = 0;
			faultDraw.packet.vertexLayout.elements[1].format =
				rts::render::RENDER_VERTEX_DATA_COLOR_BGRA8;
			faultDraw.packet.vertexLayout.elements[1].byteOffset = 12;
			faultDraw.packet.vertexCount = 3;
			faultDraw.packet.indexCount = 3;
			faultDraw.packet.indexed = true;
			const unsigned short faultIndices[3] = { 0, 1, 2 };
			unsigned int submittedFaultDraws = 0;
			const rts::render::RenderResult cleanupFaultResult =
				w3d.SubmitNativeSortedBatch(&faultDraw, 1, vertices,
					sizeof(vertices), faultIndices, sizeof(faultIndices),
					&submittedFaultDraws);
			// Temporary resource rollback is a synchronous owner transaction.
			// Its refusal is returned by the batch and latched through frame end.
			// Always close the packet, even if an earlier assertion fails.
			const rts::render::RenderResult faultEnd = w3d.Renderer().EndFrame(false);
			const rts::render::RenderResult faultSubmit = faultEnd ==
				rts::render::RENDER_RESULT_OK ?
				w3d.Renderer().FinalizeEndedFrame(false) : faultEnd;
			const rts::render::RenderResult faultFence = w3d.Renderer().DrainThreaded();
			if (cleanupFaultResult != rts::render::RENDER_RESULT_FAILED ||
				faultEnd != rts::render::RENDER_RESULT_FAILED ||
				faultSubmit != rts::render::RENDER_RESULT_FAILED ||
				faultFence != rts::render::RENDER_RESULT_OK)
				std::fprintf(stderr, "Sorted cleanup: admission=%d draws=%u end=%d submit=%d fence=%d\n",
					cleanupFaultResult, submittedFaultDraws, faultEnd, faultSubmit, faultFence);
			rts::render::RenderResourceStatistics afterCleanupFault;
			result |= Check(cleanupFaultResult ==
				rts::render::RENDER_RESULT_FAILED && submittedFaultDraws == 1 &&
				faultEnd == rts::render::RENDER_RESULT_FAILED &&
				faultSubmit == rts::render::RENDER_RESULT_FAILED &&
				faultFence == rts::render::RENDER_RESULT_OK,
				"native sorted cleanup refusal preserves submitted draw count and fails frame end");
			result |= Check(rts::render::NativeW3DRecoveryTestAccess::
				GetResourceStatistics(&w3d.Renderer(), &afterCleanupFault) ==
					rts::render::RENDER_RESULT_OK &&
					afterCleanupFault.liveHandles == beforeCleanupFault.liveHandles + 1U &&
					afterCleanupFault.bufferCount == beforeCleanupFault.bufferCount + 1U,
				"native sorted cleanup attempts both temporary destroys");
			const rts::render::RenderResult faultBoundary = w3d.BeginGameDisplayIteration();
			if (faultBoundary != rts::render::RENDER_RESULT_OK)
				std::fprintf(stderr, "Sorted cleanup: display boundary=%d\n", faultBoundary);
			result |= Check(faultBoundary ==
				rts::render::RENDER_RESULT_OK,
				"native sorted cleanup frame leaves no unreported asynchronous failure");
			result |= Check(w3d.Renderer().BeginFrame() ==
				rts::render::RENDER_RESULT_OK,
				"native sorted cleanup test starts a clean recovery frame");
		}
		rts::render::NativeDrawPacket invalidLayoutPacket = packet;
		invalidLayoutPacket.vertexLayout.elementCount =
			rts::render::RenderVertexLayout::MAX_ELEMENT_COUNT + 1;
		result |= Check(w3d.Renderer().Submit(w3d.Resources(), state,
			invalidLayoutPacket) == rts::render::RENDER_RESULT_INVALID_ARGUMENT,
			"native WW3D2 bounds neutral vertex layout descriptors");
		result |= Check(w3d.Renderer().EndFrame(true) == rts::render::RENDER_RESULT_OK,
			"native WW3D2 presents a hidden D3D11 frame");
		result |= Check(w3d.RecoverDevice() == rts::render::RENDER_RESULT_OK,
			"native WW3D2 recovers a hidden D3D11 device");
		result |= Check(w3d.Renderer().BeginFrame() == rts::render::RENDER_RESULT_OK,
			"native WW3D2 begins a frame after device recovery");
		result |= Check(w3d.Renderer().Submit(w3d.Resources(), state, packet) ==
			rts::render::RENDER_RESULT_OK,
			"native WW3D2 republishes immutable creation bytes through recovery");
		rts::render::NativeDrawPacket staticPacket;
		ConfigurePacket(&staticPacket, staticVertexBuffer);
		result |= Check(w3d.Renderer().Submit(w3d.Resources(), state,
			staticPacket) == rts::render::RENDER_RESULT_OK,
			"native WW3D2 republishes DEFAULT static bytes through recovery");
		rts::render::NativeDrawPacket dynamicPacket;
		ConfigurePacket(&dynamicPacket, dynamicVertexBuffer);
		result |= Check(w3d.Renderer().Submit(w3d.Resources(), state,
			dynamicPacket) == rts::render::RENDER_RESULT_INVALID_ARGUMENT,
			"native WW3D2 fails closed for dynamic bytes after recovery");
		result |= Check(w3d.Renderer().EndFrame(true) == rts::render::RENDER_RESULT_OK,
			"native WW3D2 presents after device recovery");

		rts::render::NativeW3DResources *deferredResources =
			new rts::render::NativeW3DResources(2);
		result |= Check(deferredResources != 0 &&
			deferredResources->Bind(&w3d.Renderer()) == rts::render::RENDER_RESULT_OK,
			"a detached resource table binds to the shared native render state");
		if (deferredResources != 0)
		{
			rts::render::GpuHandle deferredBuffer;
			result |= Check(deferredResources->CreateBuffer(bufferDescriptor, vertices,
				sizeof(vertices), &deferredBuffer) == rts::render::RENDER_RESULT_OK,
				"detached resources create owner-thread handles before worker teardown");
			DestroyResourcesRequest destroyRequest;
			destroyRequest.resources = deferredResources;
			HANDLE destroyThread = CreateThread(0, 0, DestroyResourcesFromWorker,
				&destroyRequest, 0, 0);
			result |= Check(destroyThread != 0,
				"resource teardown worker starts");
			if (destroyThread != 0)
			{
				WaitForSingleObject(destroyThread, INFINITE);
				CloseHandle(destroyThread);
				result |= Check(destroyRequest.resources == 0 &&
					w3d.Renderer().PendingCleanup() == 1,
					"off-owner resource teardown queues one owner cleanup packet");
				const rts::render::RenderResult cleanupFrameBegin =
					w3d.Renderer().BeginFrame();
				const rts::render::RenderResult cleanupFrameEnd =
					cleanupFrameBegin == rts::render::RENDER_RESULT_OK ?
					w3d.Renderer().EndFrame(false) :
					rts::render::RENDER_RESULT_INVALID_ARGUMENT;
				const rts::render::RenderResult cleanupFrameFinalize =
					cleanupFrameEnd == rts::render::RENDER_RESULT_OK ?
					w3d.Renderer().FinalizeEndedFrame(false) :
					rts::render::RENDER_RESULT_INVALID_ARGUMENT;
				result |= Check(cleanupFrameBegin == rts::render::RENDER_RESULT_OK &&
					cleanupFrameEnd == rts::render::RENDER_RESULT_OK &&
					cleanupFrameFinalize == rts::render::RENDER_RESULT_OK &&
					w3d.Renderer().PendingCleanup() == 0,
					"the next render frame drains detached resource cleanup on its owner");
			}
		}
	}
	CaptureProbe completedCapture;
	completedCapture.owner = &w3d;
	completedCapture.completed = 0;
	completedCapture.cancelled = 0;
	completedCapture.cancellationReason = rts::render::RENDER_RESULT_OK;
	completedCapture.attemptShutdown = false;
	completedCapture.shutdownResult = rts::render::RENDER_RESULT_OK;
	completedCapture.frameWasOpen = true;
	completedCapture.presentCalls = 0;
	completedCapture.presentCallsAtCompletion = 0;
	rts::render::RenderCaptureRequestDescriptor captureDescriptor;
	captureDescriptor.kind = rts::render::RENDER_CAPTURE_WW3D_SCREENSHOT;
	captureDescriptor.consumer = &completedCapture;
	captureDescriptor.completed = CaptureCompleted;
	captureDescriptor.cancelled = CaptureCancelled;
	rts::render::RenderCaptureHandle captureHandle;
	result |= Check(w3d.QueueGameBackBufferCapture(captureDescriptor,
		&captureHandle) == rts::render::RENDER_RESULT_OK,
		"native WW3D2 queues a capture before frame teardown");
	result |= Check(w3d.Renderer().BeginFrame() ==
		rts::render::RENDER_RESULT_OK,
		"native WW3D2 begins a frame for ordered capture");
	rts::render::GameRenderCommand endRenderCommand = {};
	endRenderCommand.type = rts::render::GAME_RENDER_COMMAND_END_RENDER;
	endRenderCommand.value0 = 1;
	result |= Check(w3d.ExecuteGameRenderCommand(endRenderCommand) ==
		rts::render::RENDER_RESULT_OK && completedCapture.completed == 1 &&
		completedCapture.cancelled == 0 && !completedCapture.frameWasOpen,
		"END_RENDER ends before capture and presents after readback");

	CaptureProbe cancelledCapture;
	cancelledCapture.owner = &w3d;
	cancelledCapture.completed = 0;
	cancelledCapture.cancelled = 0;
	cancelledCapture.cancellationReason = rts::render::RENDER_RESULT_OK;
	cancelledCapture.attemptShutdown = false;
	cancelledCapture.shutdownResult = rts::render::RENDER_RESULT_OK;
	cancelledCapture.frameWasOpen = true;
	cancelledCapture.presentCalls = 0;
	cancelledCapture.presentCallsAtCompletion = 0;
	captureDescriptor.consumer = &cancelledCapture;
	result |= Check(w3d.QueueGameBackBufferCapture(captureDescriptor,
		&captureHandle) == rts::render::RENDER_RESULT_OK,
		"native WW3D2 queues a capture for failure cancellation");
	result |= Check(w3d.Renderer().BeginFrame() ==
		rts::render::RENDER_RESULT_OK,
		"native WW3D2 begins a frame for capture cancellation");
	rts::render::GameRenderCommand invalidFrameCommand = {};
	invalidFrameCommand.type = rts::render::GAME_RENDER_COMMAND_SET_TEXTURE;
	invalidFrameCommand.value0 = rts::render::LEGACY_TEXTURE_STAGE_COUNT;
	result |= Check(w3d.ExecuteGameRenderCommand(invalidFrameCommand) ==
		rts::render::RENDER_RESULT_INVALID_ARGUMENT,
		"native WW3D2 latches a frame command failure before capture");
	result |= Check(w3d.ExecuteGameRenderCommand(endRenderCommand) ==
		rts::render::RENDER_RESULT_INVALID_ARGUMENT &&
		cancelledCapture.completed == 0 && cancelledCapture.cancelled == 1 &&
		cancelledCapture.cancellationReason ==
			rts::render::RENDER_RESULT_INVALID_ARGUMENT,
		"END_RENDER cancels capture when frame teardown fails");

	CaptureProbe flippedCapture;
	flippedCapture.owner = &w3d;
	flippedCapture.completed = 0;
	flippedCapture.cancelled = 0;
	flippedCapture.cancellationReason = rts::render::RENDER_RESULT_OK;
	flippedCapture.attemptShutdown = false;
	flippedCapture.shutdownResult = rts::render::RENDER_RESULT_OK;
	flippedCapture.frameWasOpen = true;
	flippedCapture.presentCalls = 0;
	flippedCapture.presentCallsAtCompletion = 0;
	captureDescriptor.consumer = &flippedCapture;
	result |= Check(w3d.QueueGameBackBufferCapture(captureDescriptor,
		&captureHandle) == rts::render::RENDER_RESULT_OK &&
		w3d.Renderer().BeginFrame() == rts::render::RENDER_RESULT_OK,
		"native WW3D2 begins a frame for FLIP capture");
	rts::render::GameRenderCommand flipCommand = {};
	flipCommand.type = rts::render::GAME_RENDER_COMMAND_FLIP_RENDERER;
	result |= Check(w3d.ExecuteGameRenderCommand(flipCommand) ==
		rts::render::RENDER_RESULT_OK && flippedCapture.completed == 1 &&
		flippedCapture.cancelled == 0 && !flippedCapture.frameWasOpen,
		"FLIP ends before capture and presents through the native seam");

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
