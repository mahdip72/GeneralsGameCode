#include "Utility/CppMacros.h"
#include "nativew3d2.h"
#include "nativew3dclearcommand.h"
#include "nativew3dbufferowner.h"
#include "nativew3dtextureowner.h"
#include "Renderer/LegacyRenderState.h"
#include "Renderer/NativeW3DRenderState.h"
#include "Renderer/ThreadedRenderDevice.h"
#include "Lib/PipelineExecutionPolicy.h"

#include <cstdio>
#include <climits>
#include <cstring>
#include <limits>
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

	static bool PopulateGpuOnlyTexture(NativeW3DRenderer *renderer)
	{
		if (renderer == 0 || renderer->m_state == 0)
			return false;
		IRenderDevice *device = renderer->m_state->Device();
		RenderBackBufferInfo info;
		if (device == 0 || device->getBackBufferInfo(&info) != RENDER_RESULT_OK)
			return false;
		TextureDescriptor descriptor;
		descriptor.width = info.width;
		descriptor.height = info.height;
		descriptor.mipCount = 1;
		descriptor.arrayCount = 1;
		descriptor.dimension = RENDER_TEXTURE_2D;
		descriptor.format = info.format;
		descriptor.binding = RENDER_TEXTURE_SHADER_RESOURCE;
		descriptor.usage = RENDER_USAGE_DEFAULT;
		GpuHandle texture;
		if (device->createTexture(descriptor, 0, 0, &texture) != RENDER_RESULT_OK ||
			!texture.isValid() || renderer->BeginFrame() != RENDER_RESULT_OK)
			return false;
		const RenderResult copy = device->copyActiveColorTargetToTexture(texture);
		const RenderResult end = renderer->EndFrame(false);
		const RenderResult finalize = renderer->FinalizeEndedFrame(false);
		return copy == RENDER_RESULT_OK && end == RENDER_RESULT_OK &&
			finalize == RENDER_RESULT_OK &&
			renderer->DrainThreaded() == RENDER_RESULT_OK;
	}

	static NativeW3DRenderState *RetainState(NativeW3DRenderer *renderer)
	{
		NativeW3DRenderState *state = renderer == 0 ? 0 : renderer->m_state;
		if (state != 0)
		{
			state->AddRef();
		}
		return state;
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

struct ResizeRequest
{
	rts::render::IRenderDevice *device;
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
	bool inspectFirstPixel;
	bool firstPixelIsOpaqueRed;
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

struct CountingResizeHook : public rts::render::GameRenderCleanupHook
{
	CountingResizeHook() : releaseCalls(0), reacquireCalls(0) {}
	virtual void ReleaseResources() { ++releaseCalls; }
	virtual void ReAcquireResources() { ++reacquireCalls; }
	unsigned int releaseCalls;
	unsigned int reacquireCalls;
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

struct DestroyAggregateRequest
{
	NativeW3D2 *owner;
};

DWORD WINAPI DestroyAggregateFromWorker(void *parameter)
{
	DestroyAggregateRequest *request =
		static_cast<DestroyAggregateRequest *>(parameter);
	delete request->owner;
	request->owner = 0;
	return 0;
}

DWORD WINAPI ShutdownFromWorker(void *parameter)
{
	ShutdownRequest *request = static_cast<ShutdownRequest *>(parameter);
	request->result = request->owner->Shutdown();
	return 0;
}

DWORD WINAPI ResizeFromWorker(void *parameter)
{
	ResizeRequest *request = static_cast<ResizeRequest *>(parameter);
	request->result = request->device->resize(80, 72);
	return 0;
}

void CaptureCompleted(void *consumer, const rts::render::RenderCaptureHandle *,
	unsigned int width, unsigned int height, size_t rowPitch,
	rts::render::RenderFormat format, const void *pixels, size_t bytes)
{
	CaptureProbe *probe = static_cast<CaptureProbe *>(consumer);
	++probe->completed;
	probe->frameWasOpen = probe->owner->Renderer().IsFrameOpen();
	if (probe->presentCalls != 0)
		probe->presentCallsAtCompletion = *probe->presentCalls;
	if (probe->inspectFirstPixel)
	{
		const unsigned char *pixel = static_cast<const unsigned char *>(pixels);
		probe->firstPixelIsOpaqueRed = width != 0 && height != 0 &&
			rowPitch >= 4 && bytes >= 4 && pixel != 0 &&
			format == rts::render::RENDER_FORMAT_B8G8R8A8_UNORM &&
			pixel[0] == 0 && pixel[1] == 0 && pixel[2] == 255 &&
			pixel[3] == 255;
	}
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

bool HasNativeCaptureTga(unsigned int width, unsigned int height)
{
	std::FILE *file = std::fopen("D3D11RendererCapture.tga", "rb");
	if (file == 0)
		return false;
	unsigned char header[18] = {};
	const bool readHeader = std::fread(header, 1, sizeof(header), file) ==
		sizeof(header);
	std::fclose(file);
	return readHeader && header[2] == 2 && header[16] == 24 &&
		((static_cast<unsigned int>(header[12]) |
			(static_cast<unsigned int>(header[13]) << 8)) == width) &&
		((static_cast<unsigned int>(header[14]) |
			(static_cast<unsigned int>(header[15]) << 8)) == height);
}

int TestNativeOneShotCapture(NativeW3D2 *owner)
{
	int result = 0;
	rts::render::GameRenderCommand clearCommand = {};
	clearCommand.type = rts::render::GAME_RENDER_COMMAND_CLEAR_RENDER_TARGETS;
	clearCommand.value0 = rts::render::RENDER_CLEAR_COLOR;
	clearCommand.float3 = 1.0f;
	clearCommand.float4 = 1.0f;
	rts::render::GameRenderCommand endCommand = {};
	endCommand.type = rts::render::GAME_RENDER_COMMAND_END_RENDER;
	endCommand.value0 = 1;
	const rts::render::GameRenderCommand invalidCommand = {};

	std::remove("D3D11RendererCapture.tga");
	owner->RequestGameBackBufferCapture();
	const rts::render::RenderResult successBegin = owner->Renderer().BeginFrame();
	const rts::render::RenderResult successEnd = successBegin ==
		rts::render::RENDER_RESULT_OK &&
		owner->ExecuteGameRenderCommand(clearCommand) ==
			rts::render::RENDER_RESULT_OK ?
		owner->ExecuteGameRenderCommand(endCommand) :
		rts::render::RENDER_RESULT_INVALID_ARGUMENT;
	const bool successAck = owner->ConsumeGameBackBufferCaptureSuccess();
	result |= Check(successBegin ==
		rts::render::RENDER_RESULT_OK,
		"native one-shot capture begins its owner frame");
	result |= Check(successEnd == rts::render::RENDER_RESULT_OK && successAck &&
		!owner->ConsumeGameBackBufferCaptureSuccess() &&
		HasNativeCaptureTga(64, 64),
		"native one-shot capture writes and acknowledges a deterministic TGA once");

	std::remove("D3D11RendererCapture.tga");
	owner->RequestGameBackBufferCapture();
	const rts::render::RenderResult failedBegin = owner->Renderer().BeginFrame();
	const rts::render::RenderResult failedCommand = failedBegin ==
		rts::render::RENDER_RESULT_OK ?
		owner->ExecuteGameRenderCommand(invalidCommand) :
		rts::render::RENDER_RESULT_INVALID_ARGUMENT;
	const rts::render::RenderResult failedEnd = failedBegin ==
		rts::render::RENDER_RESULT_OK ?
		owner->ExecuteGameRenderCommand(endCommand) :
		rts::render::RENDER_RESULT_INVALID_ARGUMENT;
	const bool failedAck = owner->ConsumeGameBackBufferCaptureSuccess();
	result |= Check(failedCommand == rts::render::RENDER_RESULT_INVALID_ARGUMENT &&
		failedEnd == rts::render::RENDER_RESULT_INVALID_ARGUMENT && !failedAck &&
		!HasNativeCaptureTga(64, 64),
		"native one-shot capture does not acknowledge a failed frame or stale file");

	std::remove("D3D11RendererCapture.tga");
	owner->RequestGameBackBufferCapture();
	const rts::render::RenderResult retryBegin = owner->Renderer().BeginFrame();
	const rts::render::RenderResult retryEnd = retryBegin ==
		rts::render::RENDER_RESULT_OK &&
		owner->ExecuteGameRenderCommand(clearCommand) ==
			rts::render::RENDER_RESULT_OK ?
		owner->ExecuteGameRenderCommand(endCommand) :
		rts::render::RENDER_RESULT_INVALID_ARGUMENT;
	const bool retryAck = owner->ConsumeGameBackBufferCaptureSuccess();
	result |= Check(retryEnd == rts::render::RENDER_RESULT_OK && retryAck &&
		HasNativeCaptureTga(64, 64),
		"native one-shot capture retries after a failed frame");
	std::remove("D3D11RendererCapture.tga");
	return result;
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

bool HasNativeCapturePixel(unsigned int x, unsigned int y,
	unsigned char blue, unsigned char green, unsigned char red)
{
	std::FILE *file = std::fopen("D3D11RendererCapture.tga", "rb");
	if (file == 0)
		return false;
	unsigned char header[18] = {};
	const bool validHeader = std::fread(header, 1, sizeof(header), file) ==
		sizeof(header) && header[2] == 2 && header[16] == 24;
	const unsigned int width = static_cast<unsigned int>(header[12]) |
		(static_cast<unsigned int>(header[13]) << 8);
	const unsigned int height = static_cast<unsigned int>(header[14]) |
		(static_cast<unsigned int>(header[15]) << 8);
	unsigned char pixel[3] = {};
	const long offset = static_cast<long>(sizeof(header) +
		(static_cast<size_t>(y) * width + x) * sizeof(pixel));
	const bool readPixel = validHeader && x < width && y < height &&
		std::fseek(file, offset, SEEK_SET) == 0 &&
		std::fread(pixel, 1, sizeof(pixel), file) == sizeof(pixel);
	std::fclose(file);
	return readPixel && pixel[0] == blue && pixel[1] == green &&
		pixel[2] == red;
}

int TestSortedScratchRendering(NativeW3D2 *owner)
{
	using namespace rts::render;
	int result = 0;
	NativeSortedDraw draw;
	draw.packet.vertexStride = sizeof(NativeVertex);
	draw.packet.vertexLayout.stride = sizeof(NativeVertex);
	draw.packet.vertexLayout.elementCount = 2;
	draw.packet.vertexLayout.elements[0].semantic =
		RENDER_VERTEX_SEMANTIC_POSITION;
	draw.packet.vertexLayout.elements[0].format = RENDER_VERTEX_DATA_FLOAT3;
	draw.packet.vertexLayout.elements[1].semantic =
		RENDER_VERTEX_SEMANTIC_DIFFUSE;
	draw.packet.vertexLayout.elements[1].format = RENDER_VERTEX_DATA_COLOR_BGRA8;
	draw.packet.vertexLayout.elements[1].byteOffset = 12;
	draw.packet.vertexCount = 3;
	draw.packet.indexCount = 3;
	draw.packet.indexed = true;
	draw.state.pipeline.rasterizer.cullMode = RENDER_CULL_NONE;
	const unsigned short indices[3] = { 0, 1, 2 };
	const NativeVertex queuedVertices[3] = {
		{ -0.8f, -0.8f, 0.0f, 0xff0000ffU },
		{  0.0f,  0.8f, 0.0f, 0xff0000ffU },
		{  0.8f, -0.8f, 0.0f, 0xff0000ffU }
	};
	unsigned int queuedDraws = 0;
	const RenderResult queuedBegin = owner->Renderer().BeginFrame();
	const RenderResult queuedSubmit = queuedBegin == RENDER_RESULT_OK ?
		owner->SubmitNativeSortedBatch(&draw, 1, queuedVertices,
			sizeof(queuedVertices), indices, sizeof(indices), &queuedDraws) :
		RENDER_RESULT_INVALID_ARGUMENT;
	const RenderResult queuedEnd = queuedBegin == RENDER_RESULT_OK ?
		owner->Renderer().EndFrame(false) : RENDER_RESULT_INVALID_ARGUMENT;
	const RenderResult queuedFinalize = queuedEnd == RENDER_RESULT_OK ?
		owner->Renderer().FinalizeEndedFrame(false) : queuedEnd;
	result |= Check(queuedSubmit == RENDER_RESULT_OK && queuedDraws == 1 &&
		queuedFinalize == RENDER_RESULT_OK,
		"native sorted scratch queues a prior frame before reuse");

	const NativeVertex leftVertices[3] = {
		{ -0.9f, -0.6f, 0.0f, 0xffff0000U },
		{ -0.5f,  0.6f, 0.0f, 0xffff0000U },
		{ -0.1f, -0.6f, 0.0f, 0xffff0000U }
	};
	const NativeVertex rightVertices[3] = {
		{ 0.1f, -0.6f, 0.0f, 0xff00ff00U },
		{ 0.5f,  0.6f, 0.0f, 0xff00ff00U },
		{ 0.9f, -0.6f, 0.0f, 0xff00ff00U }
	};
	std::remove("D3D11RendererCapture.tga");
	owner->RequestGameBackBufferCapture();
	unsigned int leftDraws = 0;
	unsigned int rightDraws = 0;
	const RenderResult visibleBegin = owner->Renderer().BeginFrame();
	const RenderResult viewportResult = visibleBegin == RENDER_RESULT_OK ?
		owner->Renderer().SetViewport(RenderViewport(0.0f, 0.0f, 64.0f,
			64.0f, 0.0f, 1.0f)) : RENDER_RESULT_INVALID_ARGUMENT;
	const RenderResult clearResult = viewportResult == RENDER_RESULT_OK ?
		owner->Renderer().ClearExternal(RENDER_CLEAR_COLOR | RENDER_CLEAR_DEPTH,
			RenderFloat4(), 1.0f, 0) : RENDER_RESULT_INVALID_ARGUMENT;
	const RenderResult leftResult = clearResult == RENDER_RESULT_OK ?
		owner->SubmitNativeSortedBatch(&draw, 1, leftVertices,
			sizeof(leftVertices), indices, sizeof(indices), &leftDraws) :
		RENDER_RESULT_INVALID_ARGUMENT;
	const RenderResult rightResult = leftResult == RENDER_RESULT_OK ?
		owner->SubmitNativeSortedBatch(&draw, 1, rightVertices,
			sizeof(rightVertices), indices, sizeof(indices), &rightDraws) :
		RENDER_RESULT_INVALID_ARGUMENT;
	GameRenderCommand endCommand = {};
	endCommand.type = GAME_RENDER_COMMAND_END_RENDER;
	endCommand.value0 = 1;
	const RenderResult endResult = rightResult == RENDER_RESULT_OK ?
		owner->ExecuteGameRenderCommand(endCommand) :
		RENDER_RESULT_INVALID_ARGUMENT;
	const bool captureSucceeded = owner->ConsumeGameBackBufferCaptureSuccess();
	result |= Check(leftDraws == 1 && rightDraws == 1 &&
		endResult == RENDER_RESULT_OK && captureSucceeded &&
		HasNativeCapturePixel(16, 32, 0, 0, 255) &&
		HasNativeCapturePixel(48, 32, 0, 255, 0),
		"successive sorted scratch updates preserve both rendered batches");
	std::remove("D3D11RendererCapture.tga");
	return result;
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
	result |= Check(rts::render::NativeGameClearFlags(false, false) == 0U &&
		rts::render::NativeGameClearFlags(true, false) ==
			rts::render::RENDER_CLEAR_COLOR &&
		rts::render::NativeGameClearFlags(false, true) ==
			(rts::render::RENDER_CLEAR_DEPTH | rts::render::RENDER_CLEAR_STENCIL) &&
		rts::render::NativeGameClearFlags(true, true) ==
			(rts::render::RENDER_CLEAR_COLOR | rts::render::RENDER_CLEAR_DEPTH |
				rts::render::RENDER_CLEAR_STENCIL),
		"game depth clears include stencil for volumetric shadow frames");

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
	capture.inspectFirstPixel = false;
	capture.firstPixelIsOpaqueRed = false;
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
	failedCapture.inspectFirstPixel = false;
	failedCapture.firstPixelIsOpaqueRed = false;
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

int TestExplicitRecoveryWithOpenFrame(HWND window)
{
	using namespace rts::render;
	int result = 0;
	NativeW3DRendererDescriptor descriptor;
	descriptor.width = 64;
	descriptor.height = 64;
	descriptor.enableVsync = false;
	descriptor.allowSoftwareFallback = true;
	NativeW3D2 owner;
	CountingResizeHook hook;
	const RenderResult initialize = owner.Initialize(window, descriptor);
	if (initialize == RENDER_RESULT_UNSUPPORTED) return 77;
	result |= Check(initialize == RENDER_RESULT_OK,
		"explicit open-frame recovery initializes the actual native owner");
	if (initialize != RENDER_RESULT_OK) return result;
	owner.SetGameCleanupHook(&hook);
	result |= Check(owner.Renderer().IsThreaded(),
		"explicit open-frame recovery uses the production threaded backend");

	const NativeVertex vertices[3] = {
		{ -0.8f, -0.8f, 0.0f, 0xff0000ffU },
		{  0.0f,  0.8f, 0.0f, 0xff0000ffU },
		{  0.8f, -0.8f, 0.0f, 0xff0000ffU }
	};
	BufferDescriptor buffer;
	buffer.byteCount = sizeof(vertices);
	buffer.stride = sizeof(vertices[0]);
	buffer.binding = RENDER_BUFFER_VERTEX;
	buffer.usage = RENDER_USAGE_IMMUTABLE;
	GpuHandle staticBuffer;
	GpuHandle dynamicBuffer;
	result |= Check(owner.Resources().CreateBuffer(buffer, vertices,
		sizeof(vertices), &staticBuffer) == RENDER_RESULT_OK,
		"explicit recovery retains immutable CPU-backed bytes");
	buffer.usage = RENDER_USAGE_DYNAMIC;
	result |= Check(owner.Resources().CreateBuffer(buffer, vertices,
		sizeof(vertices), &dynamicBuffer) == RENDER_RESULT_OK,
		"explicit recovery creates authoritative dynamic bytes before cancellation");
	const unsigned int pixel = 0xffffffffU;
	TextureDescriptor texture;
	texture.width = texture.height = texture.mipCount = texture.arrayCount = 1;
	texture.dimension = RENDER_TEXTURE_2D;
	texture.format = RENDER_FORMAT_B8G8R8A8_UNORM;
	texture.binding = RENDER_TEXTURE_SHADER_RESOURCE;
	texture.usage = RENDER_USAGE_IMMUTABLE;
	TextureSubresourceData data;
	data.data = &pixel;
	data.rowPitch = sizeof(pixel);
	data.slicePitch = sizeof(pixel);
	GpuHandle textureResource;
	NativeW3DTextureHandle retainedTexture;
	NativeW3DSurfaceHandle oldSurface;
	result |= Check(owner.Resources().CreateTexture(texture, &data, 1,
		&textureResource) == RENDER_RESULT_OK &&
		owner.Resources().AcquireTexture(textureResource, &retainedTexture) ==
			RENDER_RESULT_OK &&
		owner.Resources().AcquireTextureSurface(retainedTexture, 0, 0,
			&oldSurface) == RENDER_RESULT_OK,
		"explicit recovery retains a typed surface from the old backend epoch");
	ThreadedRenderMetrics before;
	result |= Check(owner.Renderer().GetThreadedMetrics(&before) &&
		owner.Renderer().BeginFrame() == RENDER_RESULT_OK &&
		owner.Renderer().IsFrameOpen(),
		"explicit recovery starts with a real open producer frame");
	const RenderResult recover = owner.RecoverDevice();
	ThreadedRenderMetrics after;
	result |= Check(recover == RENDER_RESULT_OK && owner.IsOperational() &&
		!owner.Renderer().IsFrameOpen() && hook.releaseCalls == 1 &&
		hook.reacquireCalls == 1,
		"expected frame cancellation does not bypass lifecycle cleanup and recovery");
	result |= Check(owner.Renderer().GetThreadedMetrics(&after) &&
		after.submittedFrames == before.submittedFrames + 1 &&
		after.completedFrames == before.completedFrames + 1 &&
		after.failedFrames == before.failedFrames + 1,
		"recovery fences exactly one canceled frame before rebuilding the backend");
	if (recover == RENDER_RESULT_OK)
	{
		GpuHandle validated;
		result |= Check(owner.Resources().AcquireVertexBufferRange(staticBuffer,
			sizeof(vertices[0]), 0, 0, 3, &validated) == RENDER_RESULT_OK &&
			owner.Resources().AcquireVertexBufferRange(dynamicBuffer,
				sizeof(vertices[0]), 0, 0, 3, &validated) ==
				RENDER_RESULT_INVALID_ARGUMENT,
			"recovery restores immutable bytes but invalidates old dynamic authority");
		const unsigned int oldEpoch = oldSurface.backendEpoch;
		result |= Check(owner.Resources().AcquireTextureSurface(retainedTexture,
			0, 0, &oldSurface) == RENDER_RESULT_INVALID_ARGUMENT &&
			!oldSurface.isValid(),
			"recovery rejects and clears a cached old-backend surface");
		NativeW3DSurfaceHandle newSurface;
		result |= Check(owner.Resources().AcquireTexture(textureResource,
			&retainedTexture) == RENDER_RESULT_OK &&
			owner.Resources().AcquireTextureSurface(retainedTexture, 0, 0,
				&newSurface) == RENDER_RESULT_OK && newSurface.backendEpoch != oldEpoch,
			"recovery reacquires CPU-backed texture authority in the new backend epoch");
		// Service through the real next owner boundaries, not the renderer-only
		// shortcut: the retained cancellation must report once, without a second
		// cleanup/reacquire transaction after explicit recovery already completed.
		const RenderResult reset = owner.ResetGameRenderFrameResources(true);
		const RenderResult failedBoundary = owner.BeginGameDisplayIteration();
		const RenderResult healthyBoundary = owner.BeginGameDisplayIteration();
		result |= Check(reset == RENDER_RESULT_OK &&
			failedBoundary == RENDER_RESULT_DEVICE_REMOVED &&
			healthyBoundary == RENDER_RESULT_OK && owner.IsOperational() &&
			hook.releaseCalls == 1 && hook.reacquireCalls == 1,
			"next owner boundaries report cancellation once without repeating explicit recovery");
		GameRenderCommand begin = {};
		begin.type = GAME_RENDER_COMMAND_BEGIN_RENDER;
		begin.value0 = RENDER_CLEAR_COLOR | RENDER_CLEAR_DEPTH;
		begin.float3 = begin.float4 = 1.0f;
		GameRenderCommand end = {};
		end.type = GAME_RENDER_COMMAND_END_RENDER;
		result |= Check(healthyBoundary == RENDER_RESULT_OK &&
			owner.ExecuteGameRenderCommand(begin) == RENDER_RESULT_OK &&
			owner.ExecuteGameRenderCommand(end) == RENDER_RESULT_OK &&
			owner.Renderer().DrainThreaded() == RENDER_RESULT_OK &&
			owner.ResetGameRenderFrameResources(true) == RENDER_RESULT_OK &&
			hook.releaseCalls == 1 && hook.reacquireCalls == 1,
			"normal commands complete the recovered frame with exactly one lifecycle transaction");
	}
	owner.SetGameCleanupHook(0);
	result |= Check(owner.Shutdown() == RENDER_RESULT_OK,
		"explicit open-frame recovery shuts down without a stranded frame reservation");

	// A successful cancellation must not hide an actual recovery callback failure.
	NativeW3D2 failingOwner;
	ThrowingCleanupHook failingHook;
	result |= Check(failingOwner.Initialize(window, descriptor) == RENDER_RESULT_OK,
		"open-frame reacquire failure fixture initializes");
	if (failingOwner.IsOperational())
	{
		failingOwner.SetGameCleanupHook(&failingHook);
		failingHook.owner = &failingOwner;
		failingHook.probeReentry = true;
		result |= Check(failingOwner.Renderer().BeginFrame() == RENDER_RESULT_OK &&
			failingOwner.RecoverDevice() == RENDER_RESULT_FAILED &&
			!failingOwner.IsOperational() && !failingOwner.Renderer().IsFrameOpen() &&
			failingHook.releaseCalls == 1 && failingHook.reacquireCalls == 1,
			"open-frame cancellation preserves real reacquire failure and fails closed");
		result |= Check(failingHook.releaseShutdownResult ==
			RENDER_RESULT_INVALID_ARGUMENT && failingHook.reacquireShutdownResult ==
			RENDER_RESULT_INVALID_ARGUMENT,
			"open-frame recovery retains lifecycle authority across cleanup callbacks");
		result |= Check(failingOwner.ResetGameRenderFrameResources(true) ==
			RENDER_RESULT_INVALID_ARGUMENT &&
			failingOwner.BeginGameDisplayIteration() == RENDER_RESULT_INVALID_ARGUMENT &&
			failingHook.releaseCalls == 1 && failingHook.reacquireCalls == 1,
			"later owner boundaries do not repeat the failed explicit reacquire attempt");
		failingOwner.SetGameCleanupHook(0);
		result |= Check(failingOwner.Shutdown() == RENDER_RESULT_OK,
			"failed open-frame recovery still releases owner resources");
	}
	return result;
}

int TestPublicFrameResetRecoversRemovedDevice(HWND window)
{
	using namespace rts::render;
	int result = 0;
	NativeW3DRendererDescriptor descriptor;
	descriptor.width = 64;
	descriptor.height = 64;
	descriptor.enableVsync = false;
	descriptor.allowSoftwareFallback = true;
	NativeW3D2 owner;
	result |= Check(owner.Initialize(window, descriptor) == RENDER_RESULT_OK,
		"public frame recovery fixture initializes");
	if (!owner.IsInitialized()) return result;
	result |= Check(owner.Renderer().IsThreaded(),
		"public frame recovery exercises asynchronous rendering");
	result |= Check(owner.Renderer().SetGamma(1.1f, 0.0f, 1.0f, false, true) ==
		RENDER_RESULT_OK, "public frame recovery enables the presentation pass");
	NativeDrawPacket sortedPacket;
	sortedPacket.vertexStride = sizeof(NativeVertex);
	sortedPacket.vertexLayout.stride = sizeof(NativeVertex);
	sortedPacket.vertexLayout.elementCount = 2;
	sortedPacket.vertexLayout.elements[0].semantic =
		RENDER_VERTEX_SEMANTIC_POSITION;
	sortedPacket.vertexLayout.elements[0].format = RENDER_VERTEX_DATA_FLOAT3;
	sortedPacket.vertexLayout.elements[1].semantic =
		RENDER_VERTEX_SEMANTIC_DIFFUSE;
	sortedPacket.vertexLayout.elements[1].format = RENDER_VERTEX_DATA_COLOR_BGRA8;
	sortedPacket.vertexLayout.elements[1].byteOffset = 12;
	sortedPacket.vertexCount = 3;
	sortedPacket.indexCount = 3;
	sortedPacket.indexed = true;
	const NativeVertex sortedVertices[3] = {
		{ -0.8f, -0.8f, 0.0f, 0xff0000ffU },
		{  0.0f,  0.8f, 0.0f, 0xff0000ffU },
		{  0.8f, -0.8f, 0.0f, 0xff0000ffU }
	};
	const unsigned short sortedIndices[3] = { 0, 1, 2 };
	LegacyLogicalState staleState;
	staleState.constants.world.values[0] =
		std::numeric_limits<float>::quiet_NaN();
	result |= Check(owner.QueueGameSortedTriangles(staleState, sortedPacket,
		sortedVertices, sizeof(sortedVertices), sortedIndices,
		sizeof(sortedIndices), 0) == RENDER_RESULT_OK &&
		owner.FlushGameSortedTriangles() == RENDER_RESULT_INVALID_ARGUMENT &&
		owner.FlushGameSortedTriangles() == RENDER_RESULT_INVALID_ARGUMENT,
		"failed sorted flush retains its copied packet before recovery");
	result |= Check(NativeW3DRecoveryTestAccess::ConfigureResourceFault(
		&owner.Renderer(), RENDER_RESOURCE_FAULT_PRESENTATION_PASS, 1,
		RENDER_RESULT_DEVICE_REMOVED) == RENDER_RESULT_OK,
		"public frame recovery arms device removal during presentation");
	result |= Check(BeginGameDisplayIteration() == RENDER_RESULT_OK,
		"public frame recovery enters the first display boundary");
	GameRenderCommand begin = {};
	begin.type = GAME_RENDER_COMMAND_BEGIN_RENDER;
	begin.value0 = RENDER_CLEAR_COLOR | RENDER_CLEAR_DEPTH;
	begin.float3 = 1.0f;
	begin.float4 = 1.0f;
	GameRenderCommand end = {};
	end.type = GAME_RENDER_COMMAND_END_RENDER;
	end.value0 = 1;
	result |= Check(owner.ExecuteGameRenderCommand(begin) == RENDER_RESULT_OK &&
		owner.ExecuteGameRenderCommand(end) == RENDER_RESULT_OK,
		"public frame recovery admits the frame before asynchronous removal");
	(void)owner.Renderer().DrainThreaded();
	result |= Check(!owner.IsInitialized() && !owner.IsOperational(),
		"asynchronous removal is published before the next public frame entry");
	// WW3D::Begin_Render resets resources before BeginGameDisplayIteration.
	// Calling the concrete owner's display method first hides this regression.
	const RenderResult reset = ResetGameRenderFrameResources(true);
	result |= Check(reset == RENDER_RESULT_OK && owner.IsOperational(),
		"public frame resource reset reaches recovery while the device is removed");
	if (reset == RENDER_RESULT_OK)
	{
		// Completion is deferred until the public display boundary.  The caller
		// must not issue BEGIN_RENDER after that error; a later healthy boundary
		// is allowed to start the recovered frame.
		const RenderResult failedBoundary = BeginGameDisplayIteration();
		const RenderResult rejectedBegin = failedBoundary == RENDER_RESULT_OK ?
			owner.ExecuteGameRenderCommand(begin) : failedBoundary;
		result |= Check(failedBoundary == RENDER_RESULT_DEVICE_REMOVED &&
			rejectedBegin == RENDER_RESULT_DEVICE_REMOVED &&
			!owner.Renderer().IsFrameOpen(),
			"public boundary propagates deferred worker failure without opening another frame");
		const RenderResult healthyBoundary = BeginGameDisplayIteration();
		result |= Check(healthyBoundary == RENDER_RESULT_OK,
			"public display boundary clears the deferred failure for the following frame");
		result |= Check(owner.FlushGameSortedTriangles() == RENDER_RESULT_OK,
			"device recovery discards the stale sorted packet");
		result |= Check(healthyBoundary == RENDER_RESULT_OK &&
			owner.ExecuteGameRenderCommand(begin) == RENDER_RESULT_OK &&
			owner.ExecuteGameRenderCommand(end) == RENDER_RESULT_OK &&
			owner.Renderer().DrainThreaded() == RENDER_RESULT_OK,
			"public frame rendering resumes after device recovery");
	}
	result |= Check(owner.Shutdown() == RENDER_RESULT_OK,
		"public frame recovery fixture shuts down");
	return result;
}

int TestResizeRollback(HWND window)
{
	using namespace rts::render;
	int result = 0;
	NativeW3DRendererDescriptor descriptor;
	descriptor.width = 64;
	descriptor.height = 64;
	descriptor.enableVsync = false;
	descriptor.allowSoftwareFallback = true;
	GameRenderCommand resizeCommand = {};
	resizeCommand.type = GAME_RENDER_COMMAND_SET_RESOLUTION;
	resizeCommand.value0 = 80;
	resizeCommand.value1 = 72;
	resizeCommand.value2 = 1;
	const RenderResourceFaultPoint faults[] = {
		RENDER_RESOURCE_FAULT_RESIZE_TARGETS,
		RENDER_RESOURCE_FAULT_RESIZE_TARGETS_AND_ROLLBACK,
		RENDER_RESOURCE_FAULT_RESIZE_TARGETS,
		RENDER_RESOURCE_FAULT_RESIZE_RECOVERY_RETRY_TARGETS,
		RENDER_RESOURCE_FAULT_RESIZE_TARGETS_RECOVERY_FAILURE,
		RENDER_RESOURCE_FAULT_NONE
	};
	const RenderResult injectedResults[] = {
		RENDER_RESULT_OUT_OF_MEMORY,
		RENDER_RESULT_OUT_OF_MEMORY,
		RENDER_RESULT_DEVICE_REMOVED,
		RENDER_RESULT_OUT_OF_MEMORY,
		RENDER_RESULT_OUT_OF_MEMORY,
		RENDER_RESULT_OK
	};
	for (unsigned int scenario = 0; scenario < 6; ++scenario)
	{
		NativeW3D2 owner;
		CountingResizeHook hook;
		result |= Check(owner.Initialize(window, descriptor) == RENDER_RESULT_OK,
			"resize rollback fixture initializes native owner");
		if (!owner.IsOperational()) continue;
		owner.SetGameCleanupHook(&hook);
		GpuHandle retainedDynamicBuffer;
		GpuHandle retainedGpuTexture;
		NativeW3DGpuContentLease retainedGpuLease;
		if (scenario == 2 || scenario == 5)
		{
			const unsigned int vertices[3] = { 1U, 2U, 3U };
			BufferDescriptor bufferDescriptor;
			bufferDescriptor.byteCount = sizeof(vertices);
			bufferDescriptor.stride = sizeof(vertices[0]);
			bufferDescriptor.binding = RENDER_BUFFER_VERTEX;
			bufferDescriptor.usage = RENDER_USAGE_DYNAMIC;
			GpuHandle validated;
			result |= Check(owner.Resources().CreateBuffer(bufferDescriptor,
				vertices, sizeof(vertices), &retainedDynamicBuffer) ==
				RENDER_RESULT_OK &&
				owner.Resources().AcquireVertexBufferRange(retainedDynamicBuffer,
					sizeof(vertices[0]), 0, 0, 3, &validated) ==
					RENDER_RESULT_OK,
				"resize fixture retains an initialized dynamic buffer");
			TextureDescriptor textureDescriptor;
			textureDescriptor.width = 64;
			textureDescriptor.height = 64;
			textureDescriptor.mipCount = 1;
			textureDescriptor.arrayCount = 1;
			textureDescriptor.dimension = RENDER_TEXTURE_2D;
			textureDescriptor.format = RENDER_FORMAT_B8G8R8A8_UNORM;
			textureDescriptor.binding = RENDER_TEXTURE_SHADER_RESOURCE;
			textureDescriptor.usage = RENDER_USAGE_DEFAULT;
			result |= Check(owner.Resources().CreateTexture(textureDescriptor,
				0, 0, &retainedGpuTexture) == RENDER_RESULT_OK,
				"resize fixture creates a GPU-copy texture");
			const RenderResult beginResult = owner.Renderer().BeginFrame();
			const RenderResult copyResult = beginResult == RENDER_RESULT_OK ?
				owner.Resources().CopyActiveColorTargetToTexture(
					retainedGpuTexture, &retainedGpuLease) :
				RENDER_RESULT_INVALID_ARGUMENT;
			const RenderResult endResult = beginResult == RENDER_RESULT_OK ?
				owner.Renderer().EndFrame(false) :
				RENDER_RESULT_INVALID_ARGUMENT;
			const RenderResult fenceResult = endResult == RENDER_RESULT_OK ?
				owner.Renderer().FinalizeEndedFrame(false) :
				RENDER_RESULT_INVALID_ARGUMENT;
			result |= Check(copyResult == RENDER_RESULT_OK &&
				retainedGpuLease.isValid() && endResult == RENDER_RESULT_OK &&
				fenceResult == RENDER_RESULT_OK &&
				owner.Renderer().DrainThreaded() == RENDER_RESULT_OK,
				"resize fixture publishes GPU-authored texture content");
		}
		if (scenario == 3 || scenario == 4)
			result |= Check(NativeW3DRecoveryTestAccess::PopulateGpuOnlyTexture(
				&owner.Renderer()),
				"resize recovery fixture populates a shader-only texture on the GPU");
		if (scenario != 5)
			result |= Check(NativeW3DRecoveryTestAccess::ConfigureResourceFault(
				&owner.Renderer(), faults[scenario], 1,
				injectedResults[scenario]) == RENDER_RESULT_OK,
				"resize rollback fixture arms target failure");
		const RenderResult resizeResult =
			owner.ExecuteGameRenderCommand(resizeCommand);
		RenderBackBufferInfo info;
		if (scenario == 0)
		{
			result |= Check(resizeResult == RENDER_RESULT_OUT_OF_MEMORY &&
				hook.releaseCalls == 1 && hook.reacquireCalls == 1 &&
				owner.IsOperational() && owner.Renderer().GetBackBufferInfo(&info) ==
					RENDER_RESULT_OK && info.width == 64 && info.height == 64,
				"failed target creation restores old targets and title resources");
			GameRenderCommand begin = {};
			begin.type = GAME_RENDER_COMMAND_BEGIN_RENDER;
			begin.value0 = RENDER_CLEAR_COLOR | RENDER_CLEAR_DEPTH;
			begin.float3 = 1.0f;
			begin.float4 = 1.0f;
			GameRenderCommand end = {};
			end.type = GAME_RENDER_COMMAND_END_RENDER;
			end.value0 = 1;
			GameRenderCommand retryOldSize = resizeCommand;
			retryOldSize.value0 = 64;
			retryOldSize.value1 = 64;
			result |= Check(owner.ExecuteGameRenderCommand(retryOldSize) ==
				RENDER_RESULT_OK && hook.releaseCalls == 2 &&
				hook.reacquireCalls == 2 && owner.IsOperational(),
				"retrying the old resolution leaves title resources operational");
			result |= Check(owner.Renderer().DrainThreaded() ==
				RENDER_RESULT_OUT_OF_MEMORY,
				"resize failure remains visible at the next threaded fence");
			const RenderResult beginResult = owner.ExecuteGameRenderCommand(begin);
			const RenderResult endResult = owner.ExecuteGameRenderCommand(end);
			const RenderResult drainResult = owner.Renderer().DrainThreaded();
			result |= Check(beginResult == RENDER_RESULT_OK &&
				endResult == RENDER_RESULT_OK && drainResult == RENDER_RESULT_OK,
				"old-size rendering and presentation continue after resize rollback");
		}
		else if (scenario == 2 || scenario == 5)
		{
			result |= Check(resizeResult == RENDER_RESULT_OK &&
				hook.releaseCalls == 1 && hook.reacquireCalls == 1 &&
				owner.IsOperational() && owner.Renderer().GetBackBufferInfo(&info) ==
					RENDER_RESULT_OK && info.width == 80 && info.height == 72,
				"removed-target recovery and ordinary resize publish new targets");
			GameRenderCommand begin = {};
			begin.type = GAME_RENDER_COMMAND_BEGIN_RENDER;
			begin.value0 = RENDER_CLEAR_COLOR | RENDER_CLEAR_DEPTH;
			begin.float3 = 1.0f;
			begin.float4 = 1.0f;
			GameRenderCommand end = {};
			end.type = GAME_RENDER_COMMAND_END_RENDER;
			end.value0 = 1;
			result |= Check(owner.ExecuteGameRenderCommand(begin) ==
				RENDER_RESULT_OK && owner.ExecuteGameRenderCommand(end) ==
				RENDER_RESULT_OK && owner.Renderer().DrainThreaded() ==
				RENDER_RESULT_OK,
				"new-size rendering and presentation use valid recovered targets");
			NativeW3DBufferDescription bufferDescription;
			NativeW3DTextureDescription textureDescription;
			GpuHandle validated;
			NativeW3DGpuContentLease acquiredLease = retainedGpuLease;
			const RenderResult rangeResult =
				owner.Resources().AcquireVertexBufferRange(
					retainedDynamicBuffer, sizeof(unsigned int), 0, 0, 3,
					&validated);
			const RenderResult leaseResult =
				owner.Resources().AcquireGpuContentLease(retainedGpuTexture,
					&acquiredLease);
			result |= Check(owner.Resources().DescribeBuffer(retainedDynamicBuffer,
				&bufferDescription) == RENDER_RESULT_OK &&
				owner.Resources().DescribeTexture(retainedGpuTexture,
					&textureDescription) == RENDER_RESULT_OK &&
				(scenario == 2 ?
					(bufferDescription.authority == NATIVE_W3D_CONTENT_INVALID &&
						textureDescription.authority ==
						NATIVE_W3D_CONTENT_INVALID &&
						rangeResult == RENDER_RESULT_INVALID_ARGUMENT &&
						leaseResult == RENDER_RESULT_INVALID_ARGUMENT &&
						!acquiredLease.isValid()) :
					(bufferDescription.authority == NATIVE_W3D_CONTENT_CPU &&
						textureDescription.authority ==
						NATIVE_W3D_CONTENT_GPU_RENDER_TARGET &&
						rangeResult == RENDER_RESULT_OK &&
						leaseResult == RENDER_RESULT_OK &&
						acquiredLease.isValid())),
				"recovered resize invalidates old content; ordinary resize retains it");
		}
		else
		{
			result |= Check(resizeResult == injectedResults[scenario] &&
				hook.releaseCalls == 1 && hook.reacquireCalls == 0 &&
				!owner.IsOperational() && !owner.Renderer().IsInitialized(),
				"failed rollback or recovery retry terminates the native facade");
		}
		if (retainedDynamicBuffer.isValid() && owner.IsOperational())
			result |= Check(owner.Resources().Destroy(retainedDynamicBuffer),
				"resize fixture releases retained dynamic buffer");
		if (retainedGpuTexture.isValid() && owner.IsOperational())
			result |= Check(owner.Resources().Destroy(retainedGpuTexture),
				"resize fixture releases retained GPU texture");
		result |= Check(owner.Shutdown() == RENDER_RESULT_OK,
			"resize rollback fixture shuts down");
	}
	return result;
}

int TestResizeOwnerThread(HWND window)
{
	using namespace rts::render;
	int result = 0;
	IRenderDevice *device = CreateD3D11RenderDevice();
	result |= Check(device != 0, "resize owner fixture allocates D3D11 device");
	if (device == 0) return result;
	RenderDeviceParameters parameters;
	parameters.backend = RENDER_BACKEND_D3D11;
	parameters.window = window;
	parameters.width = 64;
	parameters.height = 64;
	parameters.enableVsync = false;
	parameters.allowSoftwareFallback = true;
	const RenderResult initialized = device->initialize(parameters);
	result |= Check(initialized == RENDER_RESULT_OK,
		"resize owner fixture initializes D3D11");
	if (initialized == RENDER_RESULT_OK)
	{
		ResizeRequest request = { device, RENDER_RESULT_OK };
		HANDLE worker = CreateThread(0, 0, ResizeFromWorker, &request, 0, 0);
		result |= Check(worker != 0, "resize owner fixture starts worker");
		if (worker != 0)
		{
			WaitForSingleObject(worker, INFINITE);
			CloseHandle(worker);
			RenderBackBufferInfo info;
			result |= Check(request.result == RENDER_RESULT_INVALID_ARGUMENT &&
				device->isOperational() &&
				device->getBackBufferInfo(&info) == RENDER_RESULT_OK &&
				info.width == 64 && info.height == 64 &&
				device->resize(80, 72) == RENDER_RESULT_OK,
				"off-owner resize is refused and owner resize remains usable");
		}
	}
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
			rts::render::ResetGameRenderFrameResources(true) ==
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
			rts::render::ResetGameRenderFrameResources(true) ==
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
	{
		using namespace rts::render;
		static_assert(GAME_TEXTURE_STAGE_MAX_ANISOTROPY == 16,
			"existing texture-stage command ordinals remain stable");
		static_assert(GAME_TEXTURE_STAGE_MAX_MIP_LEVEL == 17,
			"native mip-LOD command is appended");
		ResetTrackedLegacyState();
		SeedTrackedLegacyPipelineState();
		GameRenderCommand command = {};
		command.type = GAME_RENDER_COMMAND_SET_TEXTURE_STAGE_STATE;
		command.value0 = 2;
		command.value1 = GAME_TEXTURE_STAGE_MAX_MIP_LEVEL;
		command.value2 = GAME_TEXTURE_MAX_MIP_LEVEL_INDEX;
		LegacyLogicalState state;
		result |= Check(owner->ExecuteGameRenderCommand(command) ==
			RENDER_RESULT_OK && GetTrackedLegacyLogicalState(&state) &&
			state.pipeline.textureStages[2].sampler.maximumMipLevel ==
			GAME_TEXTURE_MAX_MIP_LEVEL_INDEX,
			"native stage conversion accepts the maximum supported mip index");
		command.value2 = GAME_TEXTURE_MAX_MIP_LEVEL_INDEX + 1U;
		result |= Check(owner->ExecuteGameRenderCommand(command) ==
			RENDER_RESULT_INVALID_ARGUMENT &&
			GetTrackedLegacyLogicalState(&state) &&
			state.pipeline.textureStages[2].sampler.maximumMipLevel ==
			GAME_TEXTURE_MAX_MIP_LEVEL_INDEX,
			"native stage conversion rejects an invalid mip payload without mutation");
		result |= Check(owner->BeginGameDisplayIteration() == RENDER_RESULT_OK,
			"the rejected sampler payload does not poison the next native iteration");
	}
	return result;
}

int TestTextureStageGetterValidity(NativeW3D2 *owner)
{
	using namespace rts::render;
	int result = 0;
	if (owner == 0 || !owner->IsOperational())
		return Check(false, "texture-stage validity fixture has an operational owner");

	LegacyLogicalState savedState;
	RenderMatrix4 savedView;
	if (!GetTrackedLegacyLogicalState(&savedState) ||
		!GetTrackedLegacyTransform(LEGACY_TRANSFORM_VIEW, &savedView))
		return Check(false, "texture-stage validity fixture starts from valid tracked state");

	const unsigned int laterStage = LEGACY_TEXTURE_STAGE_COUNT - 1U;
	LegacyPipelineState stalePipeline = savedState.pipeline;
	LegacyTextureStageState staleStage = stalePipeline.textureStages[0];
	staleStage.colorOperation = RENDER_TEXTURE_OP_ADD;
	staleStage.textureCoordinateIndex = 3U;
	staleStage.sampler.addressU = RENDER_TEXTURE_ADDRESS_BORDER;
	staleStage.sampler.maximumMipLevel = GAME_TEXTURE_MAX_MIP_LEVEL_INDEX;
	stalePipeline.textureStages[0] = staleStage;
	staleStage = stalePipeline.textureStages[laterStage];
	staleStage.colorOperation = RENDER_TEXTURE_OP_ADD;
	staleStage.textureCoordinateIndex = 1U;
	staleStage.sampler.addressU = RENDER_TEXTURE_ADDRESS_BORDER;
	staleStage.sampler.maximumMipLevel = GAME_TEXTURE_MAX_MIP_LEVEL_INDEX;
	stalePipeline.textureStages[laterStage] = staleStage;
	TrackLegacyPipelineState(stalePipeline);

	RenderMatrix4 retainedView;
	for (unsigned int index = 0; index != 16U; ++index)
		retainedView.values[index] = static_cast<float>(index + 1U);
	result |= Check(TrackLegacyTransform(LEGACY_TRANSFORM_VIEW,
		retainedView.values),
		"texture-stage validity fixture tracks an unrelated view constant");

	LegacyTextureStageState stage;
	const bool seededStageZeroRead = GetTrackedLegacyTextureStageIfValid(0, &stage);
	result |= Check(seededStageZeroRead && stage.colorOperation == RENDER_TEXTURE_OP_ADD,
		"valid texture-stage getter reads stage zero");
	const bool seededLaterStageRead =
		GetTrackedLegacyTextureStageIfValid(laterStage, &stage);
	result |= Check(seededLaterStageRead && stage.colorOperation == RENDER_TEXTURE_OP_ADD,
		"valid texture-stage getter reads a later stage");
	result |= Check(!GetTrackedLegacyTextureStageIfValid(
		LEGACY_TEXTURE_STAGE_COUNT, &stage),
		"valid texture-stage getter rejects an invalid index");
	result |= Check(!GetTrackedLegacyTextureStageIfValid(0, 0),
		"valid texture-stage getter rejects a null output");

	TrackLegacyShaderBits(0xffffffffU);
	LegacyTextureStageState rawStage;
	LegacyTextureStageState invalidOutput;
	invalidOutput.sampler.maximumMipLevel = 77U;
	LegacyPipelineState invalidPipeline;
	LegacyLogicalState invalidLogical;
	const bool rawInvalidStageRead = GetTrackedLegacyTextureStage(0, &rawStage);
	result |= Check(rawInvalidStageRead &&
		rawStage.colorOperation == RENDER_TEXTURE_OP_ADD &&
		rawStage.sampler.maximumMipLevel == GAME_TEXTURE_MAX_MIP_LEVEL_INDEX,
		"legacy texture-stage getter still reads stored stage data while invalid");
	const bool rejectedInvalidStageRead =
		!GetTrackedLegacyTextureStageIfValid(0, &invalidOutput);
	result |= Check(rejectedInvalidStageRead &&
		invalidOutput.sampler.maximumMipLevel == 77U &&
		!GetTrackedLegacyPipelineState(&invalidPipeline) &&
		!GetTrackedLegacyLogicalState(&invalidLogical),
		"validity-gated getter refuses stale stage data without validating the pipeline");

	LegacyPipelineState defaults;
	GameRenderCommand command = {};
	command.type = GAME_RENDER_COMMAND_SET_TEXTURE_STAGE_STATE;
	command.value0 = 0;
	command.value1 = GAME_TEXTURE_STAGE_MAX_MIP_LEVEL;
	command.value2 = 3U;
	const RenderResult invalidStageZeroResult =
		owner->ExecuteGameRenderCommand(command);
	const bool invalidStageZeroRead = GetTrackedLegacyTextureStage(0, &stage);
	result |= Check(invalidStageZeroResult == RENDER_RESULT_OK &&
		invalidStageZeroRead &&
		stage.colorOperation == defaults.textureStages[0].colorOperation &&
		stage.alphaOperation == defaults.textureStages[0].alphaOperation &&
		stage.textureCoordinateIndex == defaults.textureStages[0].textureCoordinateIndex &&
		stage.sampler.addressU == defaults.textureStages[0].sampler.addressU &&
		stage.sampler.maximumMipLevel == 3U,
		"invalid pipeline uses stage-zero defaults for the production setter");
	const bool invalidStageZeroStillInvalid =
		!GetTrackedLegacyTextureStageIfValid(0, &invalidOutput);
	result |= Check(invalidStageZeroStillInvalid &&
		!GetTrackedLegacyPipelineState(&invalidPipeline) &&
		!GetTrackedLegacyLogicalState(&invalidLogical),
		"stage-zero setter does not seed shared pipeline validity");
	command.value0 = laterStage;
	command.value2 = 5U;
	const RenderResult invalidLaterStageResult =
		owner->ExecuteGameRenderCommand(command);
	const bool invalidLaterStageRead = GetTrackedLegacyTextureStage(laterStage, &stage);
	result |= Check(invalidLaterStageResult == RENDER_RESULT_OK &&
		invalidLaterStageRead &&
		stage.colorOperation == defaults.textureStages[laterStage].colorOperation &&
		stage.alphaOperation == defaults.textureStages[laterStage].alphaOperation &&
		stage.textureCoordinateIndex ==
			defaults.textureStages[laterStage].textureCoordinateIndex &&
		stage.sampler.addressU == defaults.textureStages[laterStage].sampler.addressU &&
		stage.sampler.maximumMipLevel == 5U,
		"invalid pipeline uses later-stage defaults for the production setter");
	const bool invalidLaterStageStillInvalid =
		!GetTrackedLegacyTextureStageIfValid(laterStage, &invalidOutput);
	result |= Check(invalidLaterStageStillInvalid &&
		!GetTrackedLegacyPipelineState(&invalidPipeline) &&
		!GetTrackedLegacyLogicalState(&invalidLogical),
		"later-stage setter does not seed shared pipeline validity");
	RenderMatrix4 actualView;
	result |= Check(GetTrackedLegacyTransform(LEGACY_TRANSFORM_VIEW, &actualView) &&
		std::memcmp(actualView.values, retainedView.values,
			sizeof(retainedView.values)) == 0,
		"texture-stage setters preserve unrelated tracked constants while invalid");

	LegacyPipelineState validPipeline;
	TrackLegacyPipelineState(validPipeline);
	LegacyTextureStageState validStage = validPipeline.textureStages[0];
	validStage.colorOperation = RENDER_TEXTURE_OP_ADD;
	validStage.textureCoordinateIndex = 2U;
	validStage.sampler.addressU = RENDER_TEXTURE_ADDRESS_BORDER;
	validStage.sampler.maximumMipLevel = 8U;
	result |= Check(TrackLegacyTextureStage(0, validStage),
		"texture-stage validity fixture seeds a nondefault valid stage");
	validStage = validPipeline.textureStages[laterStage];
	validStage.colorOperation = RENDER_TEXTURE_OP_ADD;
	validStage.textureCoordinateIndex = 2U;
	validStage.sampler.addressU = RENDER_TEXTURE_ADDRESS_BORDER;
	validStage.sampler.maximumMipLevel = 6U;
	result |= Check(TrackLegacyTextureStage(laterStage, validStage),
		"texture-stage validity fixture seeds a nondefault later stage");
	command.value0 = 0;
	command.value2 = 4U;
	const RenderResult validStageZeroResult =
		owner->ExecuteGameRenderCommand(command);
	const bool validStageZeroRead = GetTrackedLegacyTextureStageIfValid(0, &stage);
	result |= Check(validStageZeroResult == RENDER_RESULT_OK && validStageZeroRead &&
		stage.colorOperation == RENDER_TEXTURE_OP_ADD &&
		stage.textureCoordinateIndex == 2U &&
		stage.sampler.addressU == RENDER_TEXTURE_ADDRESS_BORDER &&
		stage.sampler.maximumMipLevel == 4U,
		"valid stage-zero setter preserves unrelated nondefault stage fields");
	command.value0 = laterStage;
	command.value2 = 5U;
	const RenderResult validLaterStageResult =
		owner->ExecuteGameRenderCommand(command);
	const bool validLaterStageRead =
		GetTrackedLegacyTextureStageIfValid(laterStage, &stage);
	result |= Check(validLaterStageResult == RENDER_RESULT_OK && validLaterStageRead &&
		stage.colorOperation == RENDER_TEXTURE_OP_ADD &&
		stage.textureCoordinateIndex == 2U &&
		stage.sampler.addressU == RENDER_TEXTURE_ADDRESS_BORDER &&
		stage.sampler.maximumMipLevel == 5U,
		"valid later-stage setter preserves unrelated nondefault stage fields");
	result |= Check(GetTrackedLegacyTransform(LEGACY_TRANSFORM_VIEW, &actualView) &&
		std::memcmp(actualView.values, retainedView.values,
			sizeof(retainedView.values)) == 0,
		"valid texture-stage setters preserve unrelated tracked constants");

	TrackLegacyPipelineState(savedState.pipeline);
	TrackLegacyTransform(LEGACY_TRANSFORM_VIEW, savedView.values);
	return result;
}

int TestGetTransformWithInvalidPipeline(NativeW3D2 *owner)
{
	int result = 0;
	if (owner == 0 || !owner->IsOperational())
		return Check(false, "transform read fixture has an operational owner");

	using namespace rts::render;
	ResetTrackedLegacyState();
	LegacyLogicalState logical;
	GameRenderCommand command = {};
	command.type = GAME_RENDER_COMMAND_GET_TRANSFORM;
	command.value0 = LEGACY_TRANSFORM_VIEW;
	RenderMatrix4 actual;
	command.output = &actual;
	command.outputBytes = sizeof(actual);
	RenderMatrix4 identity;
	result |= Check(!GetTrackedLegacyLogicalState(&logical) &&
		owner->ExecuteGameRenderCommand(command) == RENDER_RESULT_OK &&
		std::memcmp(actual.values, identity.values, sizeof(identity.values)) == 0,
		"transform read returns identity after reset while pipeline state is invalid");
	result |= Check(!GetTrackedLegacyTransform(
		static_cast<LegacyTransformSlot>(LEGACY_TRANSFORM_COUNT), &actual) &&
		!GetTrackedLegacyTransform(LEGACY_TRANSFORM_VIEW, 0),
		"transform tracker rejects invalid slots and null outputs");

	RenderMatrix4 expected;
	for (unsigned int index = 0; index != 16; ++index)
		expected.values[index] = static_cast<float>(index + 1U);
	command = GameRenderCommand();
	command.type = GAME_RENDER_COMMAND_SET_TRANSFORM;
	command.value0 = LEGACY_TRANSFORM_VIEW;
	command.input = &expected;
	command.inputBytes = sizeof(expected);
	result |= Check(owner->ExecuteGameRenderCommand(command) == RENDER_RESULT_OK,
		"transform read fixture publishes a known view matrix");

	TrackLegacyShaderBits(0xffffffffU);
	result |= Check(!GetTrackedLegacyLogicalState(&logical),
		"invalid shader bits keep the logical pipeline unavailable");

	command = GameRenderCommand();
	command.type = GAME_RENDER_COMMAND_GET_TRANSFORM;
	command.value0 = LEGACY_TRANSFORM_VIEW;
	command.output = &actual;
	command.outputBytes = sizeof(actual);
	const RenderResult getResult = owner->ExecuteGameRenderCommand(command);
	result |= Check(getResult == RENDER_RESULT_OK &&
		std::memcmp(actual.values, expected.values, sizeof(expected.values)) == 0 &&
		!GetTrackedLegacyLogicalState(&logical),
		"transform read preserves the exact view matrix without revalidating invalid pipeline state");

	const float vertices[9] = {
		0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f
	};
	command = GameRenderCommand();
	command.type = GAME_RENDER_COMMAND_DRAW_PRIMITIVE_UP;
	command.value0 = GAME_PRIMITIVE_TRIANGLE_LIST;
	command.value1 = 1U;
	command.value2 = 3U * sizeof(float);
	command.value3 = GAME_VERTEX_XYZ;
	command.input = vertices;
	command.inputBytes = sizeof(vertices);
	result |= Check(owner->ExecuteGameRenderCommand(command) ==
		RENDER_RESULT_INVALID_ARGUMENT && !GetTrackedLegacyLogicalState(&logical),
		"draw submission remains rejected while the tracked pipeline is invalid");

	owner->BeginGameDisplayIteration();
	ResetTrackedLegacyState();
	SeedTrackedLegacyPipelineState();
	return result;
}

int TestPlainTransformRejectsNonfiniteValues(NativeW3D2 *owner)
{
	int result = 0;
	if (owner == 0 || !owner->IsOperational())
		return Check(false, "nonfinite transform fixture has an operational owner");
	const float invalidValues[] = {
		std::numeric_limits<float>::quiet_NaN(),
		std::numeric_limits<float>::infinity(),
		-std::numeric_limits<float>::infinity()
	};
	rts::render::RenderMatrix4 priorTransform;
	for (unsigned int element = 0; element != 16; ++element)
		priorTransform.values[element] = static_cast<float>(element + 1);

	for (unsigned int published = 0; published != 2; ++published)
	{
		const rts::render::LegacyTransformSlot slot = published == 0 ?
			rts::render::LEGACY_TRANSFORM_PROJECTION :
			rts::render::LEGACY_TRANSFORM_VIEW;
		for (unsigned int invalid = 0; invalid != 3; ++invalid)
		{
			for (unsigned int element = 0; element != 16; ++element)
			{
				rts::render::ResetTrackedLegacyState();
				result |= Check(rts::render::TrackLegacyTransform(slot,
					priorTransform.values), "nonfinite fixture seeds a prior transform");
				if (published != 0)
				{
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
						"nonfinite fixture publishes nondefault pipeline state");
				}
				if (owner->BeginGameDisplayIteration() != rts::render::RENDER_RESULT_OK ||
					owner->Renderer().BeginFrame() != rts::render::RENDER_RESULT_OK)
					return result | Check(false, "nonfinite fixture starts a real frame");

				rts::render::RenderMatrix4 malformed = priorTransform;
				malformed.values[element] = invalidValues[invalid];
				rts::render::GameRenderCommand command = {};
				command.type = rts::render::GAME_RENDER_COMMAND_SET_TRANSFORM;
				command.value0 = slot;
				command.input = &malformed;
				command.inputBytes = sizeof(malformed);
				const rts::render::RenderResult commandResult =
					owner->ExecuteGameRenderCommand(command);
				rts::render::LegacyPipelineState pipeline;
				const bool pipelinePublished =
					rts::render::GetTrackedLegacyPipelineState(&pipeline);
				// Publish only after observing the rejection so the ordinary getter
				// can inspect the retained transform of the unpublished fixture.
				if (!pipelinePublished)
					rts::render::SeedTrackedLegacyPipelineState();
				rts::render::LegacyLogicalState after;
				const bool hasState = rts::render::GetTrackedLegacyLogicalState(&after);
				const rts::render::RenderMatrix4 &actualTransform = published == 0 ?
					after.constants.projection : after.constants.view;
				const bool pipelinePreserved = published == 0 ? !pipelinePublished :
					pipelinePublished && pipeline.rasterizer.depthBias == -8 &&
					pipeline.depthStencil.stencilReference == 0x3fU &&
					pipeline.blend.blendEnable;
				const rts::render::RenderResult endResult = owner->Renderer().EndFrame(false);
				if (endResult == rts::render::RENDER_RESULT_OK)
					result |= Check(owner->Renderer().FinalizeEndedFrame(false) ==
						rts::render::RENDER_RESULT_OK,
						"nonfinite RED fixture finalizes an incorrectly accepted command");
				const rts::render::RenderResult drainResult = owner->Renderer().DrainThreaded();
				const bool rejectedAtomically = commandResult ==
					rts::render::RENDER_RESULT_INVALID_ARGUMENT && hasState &&
					pipelinePreserved && std::memcmp(actualTransform.values,
					priorTransform.values, sizeof(priorTransform.values)) == 0 &&
					endResult == rts::render::RENDER_RESULT_INVALID_ARGUMENT &&
					drainResult == rts::render::RENDER_RESULT_OK;
				if (!rejectedAtomically)
					std::fprintf(stderr, "Transform case: published=%u invalid=%u element=%u command=%d end=%d\n",
						published, invalid, element, commandResult, endResult);
				result |= Check(rejectedAtomically,
					"plain transform rejects nonfinite values before state publication and reports frame failure");
			}
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

int TestOffOwnerAggregatePublication(HWND window)
{
	int result = 0;
	rts::render::NativeW3DRendererDescriptor descriptor;
	descriptor.width = 64;
	descriptor.height = 64;
	descriptor.enableVsync = false;
	descriptor.allowSoftwareFallback = true;

	NativeW3D2 *aggregate = new (std::nothrow) NativeW3D2();
	result |= Check(aggregate != 0,
		"off-owner aggregate fixture allocates its owner");
	if (aggregate == 0)
	{
		return result;
	}
	const rts::render::RenderResult initializeResult = aggregate->Initialize(
		window, descriptor);
	result |= Check(initializeResult == rts::render::RENDER_RESULT_OK,
		"off-owner aggregate fixture initializes its renderer");
	if (initializeResult != rts::render::RENDER_RESULT_OK)
	{
		delete aggregate;
		return result;
	}
	rts::render::NativeW3DRenderState *state =
		rts::render::NativeW3DRecoveryTestAccess::RetainState(
		&aggregate->Renderer());
	result |= Check(state != 0,
		"off-owner aggregate fixture retains its owner state for drain");

	{
		rts::render::BufferDescriptor bufferDescriptor;
		bufferDescriptor.byteCount = 16;
		bufferDescriptor.stride = 4;
		bufferDescriptor.binding = rts::render::RENDER_BUFFER_VERTEX;
		bufferDescriptor.usage = rts::render::RENDER_USAGE_DEFAULT;
		rts::render::NativeW3DBufferOwner buffer;
		result |= Check(buffer.Create(bufferDescriptor) ==
			rts::render::RENDER_RESULT_OK,
			"off-owner fixture creates a surviving native buffer");

		rts::render::TextureDescriptor textureDescriptor;
		textureDescriptor.width = 2;
		textureDescriptor.height = 2;
		textureDescriptor.mipCount = 1;
		textureDescriptor.arrayCount = 1;
		textureDescriptor.dimension = rts::render::RENDER_TEXTURE_2D;
		textureDescriptor.format = rts::render::RENDER_FORMAT_B8G8R8A8_UNORM;
		textureDescriptor.binding = rts::render::RENDER_TEXTURE_SHADER_RESOURCE;
		textureDescriptor.usage = rts::render::RENDER_USAGE_DEFAULT;
		static const unsigned char texturePixels[16] = {
			0xff, 0x00, 0xff, 0xff, 0x00, 0x00, 0x00, 0xff,
			0x00, 0x00, 0x00, 0xff, 0xff, 0x00, 0xff, 0xff };
		rts::render::TextureSubresourceData subresource;
		subresource.data = texturePixels;
		subresource.rowPitch = 8;
		subresource.slicePitch = sizeof(texturePixels);
		rts::render::NativeW3DTextureOwner texture;
		rts::render::NativeW3DTextureCandidate candidate;
		result |= Check(texture.CreateCandidate(textureDescriptor, &subresource,
			1, &candidate) == rts::render::RENDER_RESULT_OK &&
			texture.PublishCandidate(&candidate, texture.PublicationGeneration()) ==
				rts::render::RENDER_RESULT_OK,
			"off-owner fixture creates a surviving native texture");

		DestroyAggregateRequest request;
		request.owner = aggregate;
		HANDLE destroyThread = CreateThread(0, 0, DestroyAggregateFromWorker,
			&request, 0, 0);
		result |= Check(destroyThread != 0,
			"off-owner aggregate fixture starts worker destruction");
		if (destroyThread != 0)
		{
			WaitForSingleObject(destroyThread, INFINITE);
			CloseHandle(destroyThread);
		}
		result |= Check(request.owner == 0,
			"off-owner aggregate fixture destroys on the worker");

		void *lockedBytes = 0;
		const rts::render::RenderResult lockResult = buffer.Lock(0, 4,
			rts::render::RENDER_BUFFER_UPDATE_PRESERVE, &lockedBytes);
		rts::render::NativeW3DTextureHandle sampledTexture;
		const rts::render::RenderResult sampleResult =
			texture.AcquireForSampling(&sampledTexture);
		result |= Check(lockResult == rts::render::RENDER_RESULT_INVALID_ARGUMENT &&
			lockedBytes == 0 &&
			sampleResult != rts::render::RENDER_RESULT_OK &&
			!sampledTexture.isValid(),
			"surviving native owners fail closed after aggregate destruction");
		result |= Check(buffer.Reset() == rts::render::RENDER_RESULT_OK,
			"surviving native buffer releases its transferred ticket");
	}

	if (state != 0)
	{
		unsigned int drained = 0;
		result |= Check(state->DrainCleanup(0, &drained) ==
			rts::render::RENDER_RESULT_OK && drained != 0,
			"off-owner aggregate fallback cleanup drains on its owner");
		state->Release();
	}

	NativeW3D2 replacement;
	const rts::render::RenderResult replacementResult = replacement.Initialize(
		window, descriptor);
	result |= Check(replacementResult == rts::render::RENDER_RESULT_OK,
		"native resource publications rebind after off-owner destruction");
	if (replacementResult == rts::render::RENDER_RESULT_OK)
	{
		result |= Check(replacement.Shutdown() == rts::render::RENDER_RESULT_OK,
			"replacement aggregate shuts down after publication rebind");
	}
	return result;
}

int TestCpuSortingImmediateTriangles(HWND window)
{
	using namespace rts::render;
	NativeW3D2 owner;
	NativeW3DRendererDescriptor descriptor;
	descriptor.width = descriptor.height = 64;
	descriptor.enableVsync = false;
	descriptor.allowSoftwareFallback = true;
	const RenderResult initialized = owner.Initialize(window, descriptor);
	if (initialized == RENDER_RESULT_UNSUPPORTED) return 77;
	int result = Check(initialized == RENDER_RESULT_OK,
		"CPU sorting immediate fixture initializes production owner");
	if (result) return result;
	struct Vertex
	{
		float x, y, z, nx, ny, nz;
		unsigned int diffuse;
		float u0, v0, u1, v1;
	};
	Vertex vertices[8] = {
		{ -0.9f, -0.6f, 0, 0, 0, 1, 0xffff0000U },
		{ -0.9f,  0.6f, 0, 0, 0, 1, 0xffff0000U },
		{ -0.1f,  0.6f, 0, 0, 0, 1, 0xffff0000U },
		{ -0.1f, -0.6f, 0, 0, 0, 1, 0xffff0000U },
		{  0.1f, -0.6f, 0, 0, 0, 1, 0xff00ff00U },
		{  0.1f,  0.6f, 0, 0, 0, 1, 0xff00ff00U },
		{  0.9f,  0.6f, 0, 0, 0, 1, 0xff00ff00U },
		{  0.9f, -0.6f, 0, 0, 0, 1, 0xff00ff00U }
	};
	const unsigned short indices[12] = { 0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7 };
	result |= Check(sizeof(Vertex) == LegacyFvfVertexSize(GAME_VERTEX_XYZNDUV2),
		"CPU sorting immediate fixture keeps production FVF stride");
	auto begin = [&]() {
		ResetTrackedLegacyState();
		LegacyPipelineState pipeline;
		pipeline.rasterizer.cullMode = RENDER_CULL_NONE;
		pipeline.depthStencil.depthEnable = false;
		pipeline.depthStencil.depthWrite = false;
		TrackLegacyPipelineState(pipeline);
		return owner.Renderer().BeginFrame() == RENDER_RESULT_OK &&
			owner.Renderer().SetViewport(RenderViewport(0, 0, 64, 64, 0, 1)) ==
				RENDER_RESULT_OK && owner.Renderer().ClearExternal(
					RENDER_CLEAR_COLOR | RENDER_CLEAR_DEPTH, RenderFloat4(), 1, 0) ==
					RENDER_RESULT_OK;
	};
	auto bind = [&](unsigned int minimum, unsigned int indexStart,
		int baseVertex, const unsigned short *sourceIndices, unsigned int indexCount) {
		GameRenderCommand command = {};
		command.type = GAME_RENDER_COMMAND_SET_VERTEX_BUFFER;
		command.value0 = GAME_VERTEX_XYZNDUV2;
		command.value1 = sizeof(Vertex);
		command.value2 = minimum;
		command.value3 = 8;
		command.value4 = minimum * sizeof(Vertex);
		command.input = vertices;
		command.inputBytes = sizeof(vertices);
		const RenderResult vb = owner.ExecuteGameRenderCommand(command);
		command = {};
		command.type = GAME_RENDER_COMMAND_SET_INDEX_BUFFER;
		command.value0 = RENDER_FORMAT_R16_UINT;
		command.value1 = indexStart;
		command.value2 = indexCount;
		command.signedValue0 = baseVertex;
		command.input = sourceIndices;
		command.inputBytes = indexCount * sizeof(unsigned short);
		return vb == RENDER_RESULT_OK &&
			owner.ExecuteGameRenderCommand(command) == RENDER_RESULT_OK;
	};
	auto draw = [&](unsigned int start, unsigned int polygons,
		unsigned int minimum, unsigned int count) {
		GameRenderCommand command = {};
		command.type = GAME_RENDER_COMMAND_DRAW_TRIANGLES;
		command.value0 = start; command.value1 = polygons;
		command.value2 = minimum; command.value3 = count;
		return owner.ExecuteGameRenderCommand(command);
	};
	auto capture = [&]() {
		owner.RequestGameBackBufferCapture();
		GameRenderCommand command = {};
		command.type = GAME_RENDER_COMMAND_END_RENDER;
		command.value0 = 1;
		const RenderResult ended = owner.ExecuteGameRenderCommand(command);
		return ended == RENDER_RESULT_OK && owner.ConsumeGameBackBufferCaptureSuccess();
	};
	std::remove("D3D11RendererCapture.tga");
	result |= Check(begin() && bind(0, 0, 0, indices, 12),
		"generic quad draw binds paired retained CPU sorting images");
	result |= Check(draw(0, 4, 0, 8) == RENDER_RESULT_OK,
		"generic 0/4/0/8 triangle draw accepts valid CPU sorting geometry");
	if (result != 0)
	{
		(void)owner.Renderer().EndFrame(false);
		(void)owner.Renderer().DrainThreaded();
		(void)owner.Shutdown();
		return result;
	}
	for (unsigned int vertex = 0; vertex < 4; ++vertex)
		vertices[vertex].diffuse = 0xff0000ffU;
	result |= Check(bind(0, 0, 0, indices, 12) &&
		draw(0, 2, 0, 4) == RENDER_RESULT_OK && capture() &&
		HasNativeCapturePixel(16, 32, 255, 0, 0) &&
		HasNativeCapturePixel(48, 32, 0, 255, 0),
		"generic CPU sorting draws execute immediately in order without sorter flush");
	// The retained source begins at physical vertex 2, while indices remain
	// relative to the mesh and the index binding has a nonzero selected start.
	result |= Check(begin() && bind(2, 5, 2, indices, 12) &&
		draw(5, 4, 0, 8) == RENDER_RESULT_OK && capture() &&
		HasNativeCapturePixel(16, 32, 255, 0, 0) &&
		HasNativeCapturePixel(48, 32, 0, 255, 0),
		"generic CPU sorting draw honors retained origins and nonzero base vertex");
	const unsigned short subrange[6] = { 2, 3, 4, 2, 4, 5 };
	result |= Check(begin() && bind(2, 5, 0, subrange, 6) &&
		draw(5, 2, 2, 4) == RENDER_RESULT_OK && capture() &&
		HasNativeCapturePixel(16, 32, 255, 0, 0),
		"generic CPU sorting draw rebases a nonzero minimum vertex window");

	BufferDescriptor gpuDescriptor;
	gpuDescriptor.byteCount = sizeof(vertices);
	gpuDescriptor.stride = sizeof(Vertex);
	gpuDescriptor.binding = RENDER_BUFFER_VERTEX;
	gpuDescriptor.usage = RENDER_USAGE_DEFAULT;
	GpuHandle gpuVertex, gpuIndex;
	result |= Check(owner.Resources().CreateBuffer(gpuDescriptor, vertices,
		sizeof(vertices), &gpuVertex) == RENDER_RESULT_OK,
		"mixed-mode negative fixture creates an actual GPU vertex resource");
	gpuDescriptor.byteCount = sizeof(indices);
	gpuDescriptor.stride = sizeof(unsigned short);
	gpuDescriptor.binding = RENDER_BUFFER_INDEX;
	result |= Check(owner.Resources().CreateBuffer(gpuDescriptor, indices,
		sizeof(indices), &gpuIndex) == RENDER_RESULT_OK,
		"mixed-mode negative fixture creates an actual GPU index resource");
	for (unsigned int invalid = 0; invalid < 8; ++invalid)
	{
		unsigned short badIndices[12];
		std::memcpy(badIndices, indices, sizeof(indices));
		if (invalid == 2) badIndices[1] = 8;
		if (invalid == 3) badIndices[1] = 1;
		result |= Check(begin() && bind(0, 0, 0, badIndices, 12),
			"generic CPU sorting negative case starts with valid retained bindings");
		if (invalid >= 4)
		{
			GameRenderCommand command = {};
			command.type = invalid == 5 ? GAME_RENDER_COMMAND_SET_INDEX_BUFFER :
				GAME_RENDER_COMMAND_SET_VERTEX_BUFFER;
			if (invalid == 4)
			{
				command.resource0.index = gpuVertex.index();
				command.resource0.generation = gpuVertex.generation();
				command.value0 = GAME_VERTEX_XYZNDUV2;
				command.value1 = sizeof(Vertex);
			}
			else if (invalid == 5)
			{
				command.resource0.index = gpuIndex.index();
				command.resource0.generation = gpuIndex.generation();
				command.value0 = RENDER_FORMAT_R16_UINT;
			}
			result |= Check(owner.ExecuteGameRenderCommand(command) == RENDER_RESULT_OK,
				"generic negative fixture selects mixed or unbound vertex/index mode");
			if (invalid == 7)
			{
				command = {};
				command.type = GAME_RENDER_COMMAND_SET_INDEX_BUFFER;
				result |= Check(owner.ExecuteGameRenderCommand(command) == RENDER_RESULT_OK,
					"generic negative fixture explicitly unbinds both images");
			}
		}
		const RenderResult rejected = draw(invalid == 0 ? 12 : 0,
			invalid == 1 ? 5 : (invalid == 3 ? 2 : 4),
			invalid == 3 ? 2 : 0, invalid == 3 ? 4 : 8);
		result |= Check(rejected == RENDER_RESULT_INVALID_ARGUMENT &&
			owner.Renderer().EndFrame(false) == RENDER_RESULT_INVALID_ARGUMENT,
			"generic CPU sorting rejects invalid ranges/references and mixed/unbound modes");
		(void)owner.Renderer().DrainThreaded();
	}
	result |= Check(owner.Resources().Destroy(gpuVertex) &&
		owner.Resources().Destroy(gpuIndex), "mixed-mode fixture releases GPU resources");
	std::remove("D3D11RendererCapture.tga");
	result |= Check(owner.Shutdown() == RENDER_RESULT_OK,
		"CPU sorting immediate fixture shuts down its production owner");
	if (result == 0) std::printf("CPU sorting immediate triangle tests passed.\n");
	return result;
}

constexpr int NativeFixtureExitCode(int failures, bool skipped)
{
	return failures != 0 ? failures : (skipped ? 77 : 0);
}

static_assert(NativeFixtureExitCode(0, false) == 0, "completed tests pass");
static_assert(NativeFixtureExitCode(0, true) == 77, "unsupported tests skip");
static_assert(NativeFixtureExitCode(1, false) == 1, "failed tests fail");
static_assert(NativeFixtureExitCode(1, true) == 1, "failures dominate skips");

int TestPublicCopyPreflight(HWND window, bool typedOnly = false)
{
	using namespace rts::render;
	int result = 0;
	NativeW3D2 owner;
	NativeW3DRendererDescriptor descriptor;
	descriptor.width = descriptor.height = 64;
	descriptor.multisampleCount = 4;
	descriptor.enableVsync = false;
	const RenderResult initialized = owner.Initialize(window, descriptor);
	if (initialized == RENDER_RESULT_UNSUPPORTED) return 77;
	if (initialized != RENDER_RESULT_OK)
		return Check(false, "public copy fixture initializes production owner");
	RenderBackBufferInfo mainInfo;
	result |= Check(owner.GetGameRenderTargetInfo(&mainInfo) == RENDER_RESULT_OK &&
		mainInfo.width == 64 && mainInfo.height == 64 && mainInfo.multisampleCount == 4,
		"public copy fixture has actual AA4 main layer");
	RenderBackBufferInfo colorInfo;
	GpuHandle colorResource(1, 1);
	result |= Check(owner.GetGameActiveColorTargetInfo(&colorInfo, &colorResource) ==
		RENDER_RESULT_OK && colorInfo.width == mainInfo.width &&
		colorInfo.height == mainInfo.height && colorInfo.multisampleCount == 4 &&
		!colorResource.isValid(), "active color query identifies actual AA4 backbuffer");
	TextureDescriptor textureDescriptor;
	textureDescriptor.width = textureDescriptor.height = 64;
	textureDescriptor.format = mainInfo.format;
	textureDescriptor.binding = RENDER_TEXTURE_SHADER_RESOURCE;
	textureDescriptor.usage = RENDER_USAGE_DEFAULT;
	GpuHandle mainCopy, layer, layerCopy, smallCopy, wrongFormat, largeCopy, independent, depth;
	bool created = owner.Resources().CreateTexture(textureDescriptor, 0, 0, &mainCopy) ==
		RENDER_RESULT_OK;
	textureDescriptor.width = textureDescriptor.height = 16;
	textureDescriptor.binding |= RENDER_TEXTURE_RENDER_TARGET;
	// A valid CPU-backed image also permits the typed owner to borrow this
	// exact selected color resource for the self-copy admission regression.
	const unsigned int layerPixels[16 * 16] = {};
	TextureSubresourceData layerData;
	layerData.data = layerPixels;
	layerData.rowPitch = 16 * sizeof(unsigned int);
	layerData.slicePitch = sizeof(layerPixels);
	created = created && owner.Resources().CreateTexture(textureDescriptor, &layerData, 1, &layer) ==
		RENDER_RESULT_OK;
	textureDescriptor.binding = RENDER_TEXTURE_SHADER_RESOURCE;
	textureDescriptor.width = textureDescriptor.height = 32;
	created = created && owner.Resources().CreateTexture(textureDescriptor, 0, 0, &layerCopy) ==
		RENDER_RESULT_OK;
	textureDescriptor.width = textureDescriptor.height = 8;
	created = created && owner.Resources().CreateTexture(textureDescriptor, 0, 0, &smallCopy) ==
		RENDER_RESULT_OK;
	textureDescriptor.width = textureDescriptor.height = 32;
	textureDescriptor.format = mainInfo.format == RENDER_FORMAT_B8G8R8A8_UNORM ?
		RENDER_FORMAT_R8G8B8A8_UNORM : RENDER_FORMAT_B8G8R8A8_UNORM;
	created = created && owner.Resources().CreateTexture(textureDescriptor, 0, 0, &wrongFormat) ==
		RENDER_RESULT_OK;
	textureDescriptor.format = mainInfo.format;
	textureDescriptor.width = textureDescriptor.height = 128;
	created = created && owner.Resources().CreateTexture(textureDescriptor, 0, 0, &largeCopy) ==
		RENDER_RESULT_OK;
	textureDescriptor.width = textureDescriptor.height = 1;
	const unsigned int pixel = 0xff00ff00U;
	TextureSubresourceData data;
	data.data = &pixel;
	data.rowPitch = data.slicePitch = sizeof(pixel);
	created = created && owner.Resources().CreateTexture(textureDescriptor, &data, 1, &independent) ==
		RENDER_RESULT_OK;
	textureDescriptor.width = textureDescriptor.height = 16;
	textureDescriptor.format = RENDER_FORMAT_D24_UNORM_S8_UINT;
	textureDescriptor.binding = RENDER_TEXTURE_DEPTH_STENCIL;
	created = created && owner.Resources().CreateTexture(textureDescriptor, 0, 0, &depth) ==
		RENDER_RESULT_OK;
	const NativeVertex vertices[3] = {};
	const unsigned short indices[3] = { 0, 1, 2 };
	BufferDescriptor bufferDescriptor;
	bufferDescriptor.byteCount = sizeof(vertices);
	bufferDescriptor.stride = sizeof(NativeVertex);
	bufferDescriptor.usage = RENDER_USAGE_DEFAULT;
	GpuHandle vertexBuffer, indexBuffer;
	created = created && owner.Resources().CreateBuffer(bufferDescriptor, vertices,
		sizeof(vertices), &vertexBuffer) == RENDER_RESULT_OK;
	bufferDescriptor.byteCount = sizeof(indices);
	bufferDescriptor.stride = sizeof(unsigned short);
	bufferDescriptor.binding = RENDER_BUFFER_INDEX;
	created = created && owner.Resources().CreateBuffer(bufferDescriptor, indices,
		sizeof(indices), &indexBuffer) == RENDER_RESULT_OK;
	if (!created)
	{
		owner.Shutdown();
		return result | Check(false, "public copy fixture creates independent resources");
	}
	NativeW3DTextureDescription before;
	result |= Check(owner.Resources().DescribeTexture(independent, &before) == RENDER_RESULT_OK,
		"public copy fixture snapshots independent CPU authority");
	auto preserved = [&]()
	{
		NativeW3DTextureDescription after;
		GpuHandle vertex, index;
		return owner.Resources().DescribeTexture(independent, &after) == RENDER_RESULT_OK &&
			after.authority == before.authority && after.authorityEpoch == before.authorityEpoch &&
			owner.Resources().AcquireVertexBufferRange(vertexBuffer, sizeof(NativeVertex),
				0, 0, 3, &vertex) == RENDER_RESULT_OK &&
			owner.Resources().AcquireIndexBufferRange(indexBuffer, RENDER_FORMAT_R16_UINT,
				0, 0, 3, &index) == RENDER_RESULT_OK;
	};
	NativeW3DGpuContentLease lease;
	auto copy = [&](GpuHandle destination, GameRenderCommandType type)
	{
		GameRenderCommand command = {};
		command.type = type;
		command.resource0.index = destination.index();
		command.resource0.generation = destination.generation();
		command.output = &lease;
		command.outputBytes = sizeof(lease);
		return owner.ExecuteGameRenderCommand(command);
	};
	RenderTargetBinding selected;
	selected.useBackBufferColor = selected.useBackBufferDepth = false;
	selected.hasColor = true;
	selected.color.resource = layer;
	auto select = [&](bool custom)
	{
		const RenderTargetBinding binding = custom ? selected : RenderTargetBinding();
		GameRenderCommand command = {};
		command.type = GAME_RENDER_COMMAND_SET_RENDER_TARGET;
		command.input = &binding;
		command.inputBytes = sizeof(binding);
		return owner.ExecuteGameRenderCommand(command);
	};
	GameRenderCommand begin = {};
	begin.type = GAME_RENDER_COMMAND_BEGIN_RENDER;
	begin.value0 = RENDER_CLEAR_COLOR;
	begin.float1 = begin.float3 = begin.float4 = 1.0f;
	GameRenderCommand end = {};
	end.type = GAME_RENDER_COMMAND_END_RENDER;
	result |= Check(owner.ExecuteGameRenderCommand(begin) == RENDER_RESULT_OK &&
		copy(mainCopy, GAME_RENDER_COMMAND_COPY_ACTIVE_TARGET_TO_TEXTURE) == RENDER_RESULT_OK &&
		lease.isValid() && owner.ExecuteGameRenderCommand(end) == RENDER_RESULT_OK &&
		owner.Renderer().DrainThreaded() == RENDER_RESULT_OK,
		"public command performs exact-size AA4 main copy/resolve");
	result |= Check(select(true) == RENDER_RESULT_OK &&
		owner.ExecuteGameRenderCommand(begin) == RENDER_RESULT_OK &&
		copy(layerCopy, GAME_RENDER_COMMAND_COPY_ACTIVE_TARGET_TO_TEXTURE) == RENDER_RESULT_OK &&
		lease.isValid() && lease.resource == layerCopy &&
		copy(layerCopy, GAME_RENDER_COMMAND_ACQUIRE_COPIED_TEXTURE_CONTENT) == RENDER_RESULT_OK &&
		lease.isValid() && owner.ExecuteGameRenderCommand(end) == RENDER_RESULT_OK &&
		owner.Renderer().DrainThreaded() == RENDER_RESULT_OK,
		"public copy/acquire uses selected 16-square layer, not 64-square AA4 main");
	const NativeW3DGpuContentLease savedLease = lease;
	NativeW3DTextureOwner typedLayer, typedMainCopy, typedLayerCopy;
	auto borrow = [&](GpuHandle resource, NativeW3DTextureOwner &destination)
	{
		NativeW3DTextureHandle handle;
		NativeW3DTextureDescription description;
		NativeW3DTextureCandidate candidate;
		return owner.Resources().AcquireTexture(resource, &handle) == RENDER_RESULT_OK &&
			owner.Resources().DescribeTexture(resource, &description) == RENDER_RESULT_OK &&
			destination.BorrowCandidate(handle, description.descriptor, &candidate) == RENDER_RESULT_OK &&
			destination.PublishCandidate(&candidate, 0) == RENDER_RESULT_OK;
	};
	if (typedOnly && !(borrow(layer, typedLayer) && borrow(mainCopy, typedMainCopy) &&
		borrow(layerCopy, typedLayerCopy)))
	{
		(void)typedLayer.Reset();
		(void)typedMainCopy.Reset();
		(void)typedLayerCopy.Reset();
		owner.Shutdown();
		return result | Check(false, "typed copy fixture borrows actual current texture identities");
	}
	const GpuHandle rejected[] = { smallCopy, wrongFormat, largeCopy, GpuHandle(),
		layer, mainCopy, layerCopy };
	for (unsigned int scenario = typedOnly ? 4 : 0; scenario < 7; ++scenario)
	{
		const RenderResult expected = scenario >= 3 ? RENDER_RESULT_INVALID_ARGUMENT :
			RENDER_RESULT_UNSUPPORTED;
		RenderTargetBinding noColor;
		noColor.useBackBufferColor = noColor.useBackBufferDepth = false;
		noColor.hasDepth = scenario == 6;
		noColor.depth.resource = scenario == 6 ? depth : GpuHandle();
		GameRenderCommand noColorCommand = {};
		noColorCommand.type = GAME_RENDER_COMMAND_SET_RENDER_TARGET;
		noColorCommand.input = &noColor;
		noColorCommand.inputBytes = sizeof(noColor);
		begin.value0 = scenario >= 5 ? 0 : RENDER_CLEAR_COLOR;
		result |= Check(owner.BeginGameDisplayIteration() == RENDER_RESULT_OK,
			"copy rejection begins at a healthy owner boundary");
		result |= Check((scenario >= 5 ? owner.ExecuteGameRenderCommand(noColorCommand) :
			select(scenario != 2)) == RENDER_RESULT_OK &&
			owner.ExecuteGameRenderCommand(begin) == RENDER_RESULT_OK,
			"public copy rejection begins with valid selected output");
		if (scenario >= 4)
		{
			RenderBackBufferInfo source;
			GpuHandle sourceResource = layer;
			const RenderResult queried = owner.GetGameActiveColorTargetInfo(&source, &sourceResource);
			result |= Check(scenario == 4 ? (queried == RENDER_RESULT_OK && sourceResource == layer) :
				(queried == RENDER_RESULT_INVALID_ARGUMENT && !sourceResource.isValid() &&
				 source.width == 0 && source.height == 0),
				"active color query reports exact alias identity or clears absent-color output");
		}
		lease = savedLease;
		NativeW3DTextureOwner *typedDestination = scenario == 4 ? &typedLayer :
			(scenario == 5 ? &typedMainCopy : &typedLayerCopy);
		const RenderResult rejectedResult = typedOnly ? typedDestination->CopyActiveColorTarget(&lease) :
			copy(rejected[scenario],
			GAME_RENDER_COMMAND_COPY_ACTIVE_TARGET_TO_TEXTURE);
		result |= Check(rejectedResult == expected && !lease.isValid() && preserved(),
			"public copy preflight clears stale lease and preserves CPU texture/static VB/IB");
		// The direct typed owner returns its raw failure; the production title
		// facade maps a false TextureClass copy to FAILED and records it. Model
		// only that caller latch here without changing the typed-owner contract.
		if (typedOnly) owner.RecordGameFailure(RENDER_RESULT_FAILED);
		const RenderResult frameExpected = typedOnly ? RENDER_RESULT_FAILED : expected;
		const RenderResult ended = owner.ExecuteGameRenderCommand(end);
		const RenderResult drained = owner.Renderer().DrainThreaded();
		// The producer rejects before admitting a copy: command/frame failure
		// remains visible, while the backend fence completes its valid work.
		result |= Check(ended == frameExpected && drained == RENDER_RESULT_OK && preserved(),
			"rejected public command reports frame failure with successful fence and retained authority");
		// Consume the failed completion without hiding it; the following display
		// iteration may then render again against the still-valid resource table.
		const RenderResult boundary = owner.BeginGameDisplayIteration();
		result |= Check((boundary == frameExpected || boundary == RENDER_RESULT_OK) &&
			owner.BeginGameDisplayIteration() == RENDER_RESULT_OK && preserved(),
			"public copy rejection does not require device recovery to retain geometry");
		std::printf("PUBLIC_COPY_PREFLIGHT typed=%u case=%u copy=%d end=%d drain=%d lease=%u authorities=%u\n",
			typedOnly, scenario, rejectedResult, ended, drained, lease.isValid(), preserved());
	}
	begin.value0 = RENDER_CLEAR_COLOR;
	result |= Check(select(true) == RENDER_RESULT_OK &&
		owner.ExecuteGameRenderCommand(begin) == RENDER_RESULT_OK &&
		(typedOnly ? typedLayerCopy.CopyActiveColorTarget(&lease) :
		 copy(layerCopy, GAME_RENDER_COMMAND_COPY_ACTIVE_TARGET_TO_TEXTURE)) == RENDER_RESULT_OK &&
		lease.isValid() && owner.ExecuteGameRenderCommand(end) == RENDER_RESULT_OK &&
		owner.Renderer().DrainThreaded() == RENDER_RESULT_OK && preserved(),
		"valid public layer copy resumes after rejection without resource/device recovery");
	result |= Check(typedLayer.Reset() == RENDER_RESULT_OK &&
		typedMainCopy.Reset() == RENDER_RESULT_OK && typedLayerCopy.Reset() == RENDER_RESULT_OK,
		"typed borrowed copy owners release before production owner shutdown");
	result |= Check(owner.Shutdown() == RENDER_RESULT_OK,
		"public copy fixture releases owner and resources");
	return result;
}

int main(int argc, char **argv)
{
	const bool explicitSerial = argc == 2 &&
		std::strcmp(argv[1], "--explicit-open-recovery-serial") == 0;
	const bool explicitParallel = argc == 2 &&
		std::strcmp(argv[1], "--explicit-open-recovery-parallel") == 0;
	const bool copySerial = argc == 2 &&
		std::strcmp(argv[1], "--copy-active-preflight-serial") == 0;
	const bool copyParallel = argc == 2 &&
		std::strcmp(argv[1], "--copy-active-preflight-parallel") == 0;
	// Startup policy is immutable after any execution owner starts. Run each
	// production mode in its own process, before even the invalid-window probe.
	if ((explicitSerial || explicitParallel || copySerial || copyParallel) &&
		!rts::SetPipelineExecutionMode((explicitSerial || copySerial) ?
			rts::PIPELINE_EXECUTION_SERIAL : rts::PIPELINE_EXECUTION_PARALLEL))
		return 1;
	int result = 0;
	bool skipped = false;
	NativeW3D2 w3d;
	rts::render::NativeW3DRendererDescriptor descriptor;
	descriptor.width = 64;
	descriptor.height = 64;
	descriptor.enableVsync = false;
	descriptor.allowSoftwareFallback = true;
	descriptor.multisampleCount = 4;
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
	if (explicitSerial || explicitParallel)
	{
		const int recoveryResult = TestExplicitRecoveryWithOpenFrame(window);
		DestroyWindow(window);
		return recoveryResult;
	}
	if (copySerial || copyParallel)
	{
		int copyResult = TestPublicCopyPreflight(window);
		if (copyResult != 77) copyResult |= TestPublicCopyPreflight(window, true);
		DestroyWindow(window);
		return copyResult;
	}
	if (argc == 2 && std::strcmp(argv[1], "--cpu-sorting-immediate") == 0)
	{
		const int immediateResult = TestCpuSortingImmediateTriangles(window);
		DestroyWindow(window);
		return immediateResult;
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
	result |= TestPublicFrameResetRecoversRemovedDevice(window);
	result |= TestResizeRollback(window);
	result |= TestResizeOwnerThread(window);
	result |= TestReacquireFailureFailClosed(window);
	const int cpuSortingResult = TestCpuSortingImmediateTriangles(window);
	if (cpuSortingResult == 77)
		skipped = true;
	else
		result |= cpuSortingResult;
	const rts::render::RenderResult initializeResult = w3d.Initialize(window, descriptor);
	if (initializeResult == rts::render::RENDER_RESULT_UNSUPPORTED)
	{
		DestroyWindow(window);
		return NativeFixtureExitCode(result, true);
	}
	result |= Check(initializeResult == rts::render::RENDER_RESULT_OK,
		"native WW3D2 initializes a hidden D3D11 swap chain");
	if (initializeResult == rts::render::RENDER_RESULT_OK)
	{
		rts::render::RenderBackBufferInfo multisampleInfo;
		result |= Check(w3d.Renderer().GetBackBufferInfo(&multisampleInfo) ==
			rts::render::RENDER_RESULT_OK &&
			multisampleInfo.multisampleCount == 4,
			"native WW3D2 publishes its effective 4x scene sample count");
		result |= Check(w3d.Renderer().Resize(80, 72) ==
			rts::render::RENDER_RESULT_OK &&
			w3d.Renderer().GetBackBufferInfo(&multisampleInfo) ==
				rts::render::RENDER_RESULT_OK &&
			multisampleInfo.width == 80 && multisampleInfo.height == 72 &&
			multisampleInfo.multisampleCount == 4,
			"native WW3D2 preserves 4x scene targets across resize");
		result |= Check(w3d.Renderer().Resize(64, 64) ==
			rts::render::RENDER_RESULT_OK,
			"native WW3D2 restores the fixture resolution after MSAA resize");
		result |= TestNativeHardwareZBias(&w3d);
		result |= TestNativeCameraBiasSequences(&w3d);
		result |= TestNativeCommandsPreservePipelineState(&w3d);
		result |= TestTextureStageGetterValidity(&w3d);
		result |= TestGetTransformWithInvalidPipeline(&w3d);
		{
			using namespace rts::render;
			LegacyPipelineState saved;
			GetTrackedLegacyPipelineState(&saved);
			LegacyPipelineState actual;
			result |= Check(w3d.SetGameRenderState(GAME_RENDER_STATE_CULL_MODE,
				GAME_RENDER_CULL_CLOCKWISE) == RENDER_RESULT_OK &&
				GetTrackedLegacyPipelineState(&actual) &&
				actual.rasterizer.cullMode == RENDER_CULL_BACK &&
				actual.rasterizer.frontCounterClockwise,
				"legacy clockwise culling preserves counter-clockwise front faces");
			result |= Check(w3d.SetGameRenderState(GAME_RENDER_STATE_CULL_MODE,
				GAME_RENDER_CULL_COUNTER_CLOCKWISE) == RENDER_RESULT_OK &&
				GetTrackedLegacyPipelineState(&actual) &&
				actual.rasterizer.cullMode == RENDER_CULL_BACK &&
				!actual.rasterizer.frontCounterClockwise,
				"legacy counter-clockwise culling preserves clockwise front faces");
			result |= Check(w3d.SetGameRenderState(GAME_RENDER_STATE_CULL_MODE,
				GAME_RENDER_CULL_NONE) == RENDER_RESULT_OK &&
				GetTrackedLegacyPipelineState(&actual) &&
				actual.rasterizer.cullMode == RENDER_CULL_NONE,
				"legacy no-cull mode preserves both windings");
			TrackLegacyPipelineState(saved);
		}
		{
			using namespace rts::render;
			LegacyPipelineState saved;
			GetTrackedLegacyPipelineState(&saved);
			LegacyPipelineState scene = saved;
			scene.textureStages[0].colorOperation = RENDER_TEXTURE_OP_ADD;
			scene.textureStages[0].alphaOperation = RENDER_TEXTURE_OP_SELECT_ARGUMENT_2;
			scene.textureStages[0].colorArgument1Complement = true;
			scene.textureStages[1].colorOperation = RENDER_TEXTURE_OP_MODULATE;
			scene.textureStages[1].alphaOperation = RENDER_TEXTURE_OP_MODULATE;
			scene.textureStages[0].sampler.addressU = RENDER_TEXTURE_ADDRESS_CLAMP;
			scene.textureStages[0].textureCoordinateIndex = 1;
			scene.textureStages[0].textureTransformEnable = true;
			TrackLegacyPipelineState(scene);
			// Texturing + diffuse modulation, with no detail stage.
			result |= Check(w3d.ApplyGameShaderBits((1U << 16) | (1U << 10)) ==
				RENDER_RESULT_OK, "native UI shader publication succeeds");
			LegacyPipelineState ui;
			result |= Check(GetTrackedLegacyPipelineState(&ui) &&
				ui.textureStages[0].colorOperation == RENDER_TEXTURE_OP_MODULATE &&
				ui.textureStages[0].alphaOperation == RENDER_TEXTURE_OP_MODULATE &&
				!ui.textureStages[0].colorArgument1Complement &&
				ui.textureStages[1].colorOperation == RENDER_TEXTURE_OP_DISABLE &&
				ui.textureStages[1].alphaOperation == RENDER_TEXTURE_OP_DISABLE &&
				ui.textureStages[0].sampler.addressU == RENDER_TEXTURE_ADDRESS_CLAMP &&
				ui.textureStages[0].textureCoordinateIndex == 1 &&
				ui.textureStages[0].textureTransformEnable,
				"native UI shader replaces scene combiners while preserving sampler and UV mapping");
			TrackLegacyPipelineState(saved);
		}
		{
			using namespace rts::render;
			LegacyPipelineState saved;
			GetTrackedLegacyPipelineState(&saved);
			const RenderLegacyVertexProgram programs[] = {
				RENDER_LEGACY_VERTEX_TREES, RENDER_LEGACY_VERTEX_WATER_SEA };
			for (unsigned int pass = 0; pass < 2; ++pass)
			{
				w3d.SetGameLegacyVertexProgram(programs[pass]);
				LegacyPipelineState active;
				result |= Check(GetTrackedLegacyPipelineState(&active) &&
					active.vertexProgram == programs[pass],
					"native scene pass can deliberately select its vertex program");
				w3d.SetGameVertexShader(GAME_VERTEX_XYZDUV1);
				result |= Check(GetTrackedLegacyPipelineState(&active) &&
					active.vertexProgram == RENDER_LEGACY_VERTEX_FIXED_FUNCTION,
					"FVF restore disables the prior water or tree vertex program");
			}
			TrackLegacyPipelineState(saved);
		}
		{
			using namespace rts::render;
			LegacyPipelineState savedPipeline;
			GetTrackedLegacyPipelineState(&savedPipeline);
			LegacyPipelineState uiPipeline = savedPipeline;
			LegacyTextureStageState &stage = uiPipeline.textureStages[0];
			stage.colorOperation = RENDER_TEXTURE_OP_MODULATE;
			stage.alphaOperation = RENDER_TEXTURE_OP_MODULATE;
			stage.cameraSpacePosition = true;
			stage.textureTransformEnable = true;
			stage.projectedCoordinates = true;
			stage.textureTransformCount = 3;
			TrackLegacyPipelineState(uiPipeline);
			LegacyVertexMaterialState material;
			material.textureStageResetMask = 1U;
			material.textureCoordinateIndex[0] = 1U;
			GameRenderCommand command;
			memset(&command, 0, sizeof(command));
			command.type = GAME_RENDER_COMMAND_SET_MATERIAL;
			command.input = &material;
			command.inputBytes = sizeof(material);
			// Render2D submits its prelit material for each batch, while an
			// unchanged ShaderClass may skip reapplying its combiners.
			for (unsigned int batch = 0; batch < 2; ++batch)
			{
				result |= Check(w3d.ExecuteGameRenderCommand(command) ==
					RENDER_RESULT_OK, "native UI material command succeeds");
				LegacyPipelineState actual;
				result |= Check(GetTrackedLegacyPipelineState(&actual) &&
					actual.textureStages[0].colorOperation == RENDER_TEXTURE_OP_MODULATE &&
					actual.textureStages[0].alphaOperation == RENDER_TEXTURE_OP_MODULATE &&
					actual.textureStages[0].textureCoordinateIndex == 1U &&
					!actual.textureStages[0].cameraSpacePosition &&
					!actual.textureStages[0].textureTransformEnable &&
					!actual.textureStages[0].projectedCoordinates &&
					actual.textureStages[0].textureTransformCount == 0U,
					"repeated UI material preserves shader combiners and resets mapping");
			}
			TrackLegacyPipelineState(savedPipeline);
		}
		{
			using namespace rts::render;
			LegacyPipelineState saved;
			GetTrackedLegacyPipelineState(&saved);
			const unsigned int modes[] = {
				GAME_TEXTURE_COORDINATE_CAMERA_REFLECTION,
				GAME_TEXTURE_COORDINATE_CAMERA_POSITION,
				GAME_TEXTURE_COORDINATE_CAMERA_NORMAL,
				GAME_TEXTURE_COORDINATE_PASSTHROUGH };
			const bool normal[] = { false, false, true, false };
			const bool position[] = { false, true, false, false };
			const bool reflection[] = { true, false, false, false };
			for (unsigned int mode = 0; mode < 4; ++mode)
			{
				GameRenderCommand command;
				memset(&command, 0, sizeof(command));
				command.type = GAME_RENDER_COMMAND_SET_TEXTURE_STAGE_STATE;
				command.value0 = 0;
				command.value1 = GAME_TEXTURE_STAGE_COORDINATE_INDEX;
				command.value2 = modes[mode] | 3U;
				LegacyPipelineState actual;
				result |= Check(w3d.ExecuteGameRenderCommand(command) ==
					RENDER_RESULT_OK && GetTrackedLegacyPipelineState(&actual) &&
					actual.textureStages[0].textureCoordinateIndex == 3U &&
					actual.textureStages[0].cameraSpaceNormal == normal[mode] &&
					actual.textureStages[0].cameraSpacePosition == position[mode] &&
					actual.textureStages[0].cameraSpaceReflectionVector ==
						reflection[mode],
					"coordinate-generation mode decodes as one exclusive legacy mode");
			}
			TrackLegacyPipelineState(saved);
		}
		result |= TestPlainTransformRejectsNonfiniteValues(&w3d);
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
			// A changed vertex shape grows the reusable scratch allocation. Its old
			// handle retirement remains synchronous; refusal must fail the batch
			// while the new candidate is rolled back.
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
				rts::render::RENDER_RESULT_FAILED && submittedFaultDraws == 0 &&
				faultEnd == rts::render::RENDER_RESULT_FAILED &&
				faultSubmit == rts::render::RENDER_RESULT_FAILED &&
				faultFence == rts::render::RENDER_RESULT_OK,
				"native sorted scratch retirement refusal rolls back growth and fails frame end");
			result |= Check(rts::render::NativeW3DRecoveryTestAccess::
				GetResourceStatistics(&w3d.Renderer(), &afterCleanupFault) ==
					rts::render::RENDER_RESULT_OK &&
					afterCleanupFault.liveHandles == beforeCleanupFault.liveHandles &&
					afterCleanupFault.bufferCount == beforeCleanupFault.bufferCount,
				"native sorted scratch growth rollback leaves no candidate allocation");
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
		result |= TestSortedScratchRendering(&w3d);
		result |= Check(w3d.RecoverDevice() == rts::render::RENDER_RESULT_OK,
			"native WW3D2 recovers a hidden D3D11 device");
		result |= Check(w3d.Renderer().GetBackBufferInfo(&multisampleInfo) ==
			rts::render::RENDER_RESULT_OK &&
			multisampleInfo.multisampleCount == 4,
			"native WW3D2 preserves 4x scene targets through device recovery");
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
	result |= TestNativeOneShotCapture(&w3d);
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
	completedCapture.inspectFirstPixel = true;
	completedCapture.firstPixelIsOpaqueRed = false;
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
	rts::render::GameRenderCommand clearCaptureCommand = {};
	clearCaptureCommand.type =
		rts::render::GAME_RENDER_COMMAND_CLEAR_RENDER_TARGETS;
	clearCaptureCommand.value0 = rts::render::RENDER_CLEAR_COLOR;
	clearCaptureCommand.float0 = 1.0f;
	clearCaptureCommand.float1 = 0.0f;
	clearCaptureCommand.float2 = 0.0f;
	clearCaptureCommand.float3 = 1.0f;
	clearCaptureCommand.float4 = 1.0f;
	result |= Check(w3d.ExecuteGameRenderCommand(clearCaptureCommand) ==
		rts::render::RENDER_RESULT_OK,
		"native WW3D2 clears the multisampled scene for capture");
	rts::render::GameRenderCommand endRenderCommand = {};
	endRenderCommand.type = rts::render::GAME_RENDER_COMMAND_END_RENDER;
	endRenderCommand.value0 = 1;
	result |= Check(w3d.ExecuteGameRenderCommand(endRenderCommand) ==
		rts::render::RENDER_RESULT_OK && completedCapture.completed == 1 &&
		completedCapture.cancelled == 0 && !completedCapture.frameWasOpen &&
		completedCapture.firstPixelIsOpaqueRed,
		"END_RENDER resolves MSAA before capture and presents after readback");

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
	cancelledCapture.inspectFirstPixel = false;
	cancelledCapture.firstPixelIsOpaqueRed = false;
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
	flippedCapture.inspectFirstPixel = false;
	flippedCapture.firstPixelIsOpaqueRed = false;
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
	result |= TestOffOwnerAggregatePublication(window);
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
	return NativeFixtureExitCode(result, skipped);
}
