#ifndef RTS_RENDERER_RENDERERDEVICE_H
#define RTS_RENDERER_RENDERERDEVICE_H

#include <stddef.h>
#include "Renderer/LegacyRenderState.h"

namespace rts
{
namespace render
{
enum RenderBackend
{
	RENDER_BACKEND_DX8,
	RENDER_BACKEND_D3D11
};

const char *RenderBackendName(RenderBackend backend);
bool ParseRenderBackend(const char *name, RenderBackend *backend);
void SetRequestedRenderBackend(RenderBackend backend);
RenderBackend RequestedRenderBackend();

class GpuHandle
{
public:
	GpuHandle();
	GpuHandle(unsigned int index, unsigned int generation);

	bool isValid() const;
	unsigned int index() const;
	unsigned int generation() const;

	bool operator==(const GpuHandle &other) const;
	bool operator!=(const GpuHandle &other) const;

private:
	unsigned int m_index;
	unsigned int m_generation;
};

class GpuHandleAllocator
{
public:
	explicit GpuHandleAllocator(unsigned int capacity);
	~GpuHandleAllocator();

	GpuHandle allocate();
	bool release(GpuHandle handle);
	bool isLive(GpuHandle handle) const;
	unsigned int capacity() const;
	unsigned int liveCount() const;

private:
	GpuHandleAllocator(const GpuHandleAllocator &);
	GpuHandleAllocator &operator=(const GpuHandleAllocator &);

	struct Impl;
	Impl *m_impl;
};

enum RenderUsage
{
	RENDER_USAGE_IMMUTABLE,
	RENDER_USAGE_DYNAMIC,
	RENDER_USAGE_DEFAULT
};

enum BufferBinding
{
	RENDER_BUFFER_VERTEX = 1,
	RENDER_BUFFER_INDEX = 2,
	RENDER_BUFFER_CONSTANT = 4
};

enum RenderFormat
{
	RENDER_FORMAT_UNKNOWN,
	RENDER_FORMAT_R8G8B8A8_UNORM,
	RENDER_FORMAT_B8G8R8A8_UNORM,
	RENDER_FORMAT_D24_UNORM_S8_UINT,
	RENDER_FORMAT_R16_UINT,
	RENDER_FORMAT_R32_UINT,
	// Signed two-channel normalized data used by the legacy V8U8 sea bump map.
	// Keep this distinct from the packed color formats: the shader relies on
	// hardware signed-normalized sampling rather than unsigned byte decoding.
	RENDER_FORMAT_R8G8_SNORM
};

enum TextureBinding
{
	RENDER_TEXTURE_SHADER_RESOURCE = 1,
	RENDER_TEXTURE_RENDER_TARGET = 2,
	RENDER_TEXTURE_DEPTH_STENCIL = 4
};

enum RenderTextureDimension
{
	RENDER_TEXTURE_2D,
	RENDER_TEXTURE_CUBE
};

struct RenderDeviceParameters
{
	RenderDeviceParameters();

	RenderBackend backend;
	void *window;
	unsigned int width;
	unsigned int height;
	unsigned int adapterIndex;
	bool enableDebugLayer;
	bool enableVsync;
	bool allowSoftwareFallback;
};

struct BufferDescriptor
{
	BufferDescriptor();

	size_t byteCount;
	unsigned int stride;
	unsigned int binding;
	RenderUsage usage;
};

enum RenderBufferUpdateMode
{
	RENDER_BUFFER_UPDATE_PRESERVE,
	RENDER_BUFFER_UPDATE_DISCARD,
	RENDER_BUFFER_UPDATE_NO_OVERWRITE
};

struct TextureSubresourceData
{
	TextureSubresourceData();

	const void *data;
	size_t rowPitch;
	size_t slicePitch;
};

struct TextureDescriptor
{
	TextureDescriptor();

	unsigned int width;
	unsigned int height;
	unsigned int mipCount;
	unsigned int arrayCount;
	RenderTextureDimension dimension;
	RenderFormat format;
	unsigned int binding;
	RenderUsage usage;
};

// A render-target binding is explicit about whether each attachment is the
// swap-chain target or a logical texture resource.  The old two-handle API
// could not represent a custom color target using the default depth buffer,
// which is a common DX8 render-to-texture pattern.
struct RenderTargetSubresource
{
	RenderTargetSubresource();

	GpuHandle resource;
	unsigned int mip;
	unsigned int arraySlice;
};

struct RenderTargetBinding
{
	RenderTargetBinding();

	bool useBackBufferColor;
	bool useBackBufferDepth;
	bool hasColor;
	bool hasDepth;
	RenderTargetSubresource color;
	RenderTargetSubresource depth;
};

enum RenderResult
{
	RENDER_RESULT_OK,
	RENDER_RESULT_INVALID_ARGUMENT,
	RENDER_RESULT_UNSUPPORTED,
	RENDER_RESULT_OUT_OF_MEMORY,
	RENDER_RESULT_DEVICE_REMOVED,
	RENDER_RESULT_FAILED
};

struct RenderBackBufferInfo
{
	RenderBackBufferInfo();

	unsigned int width;
	unsigned int height;
	RenderFormat format;
};

enum RenderCaptureKind
{
	RENDER_CAPTURE_COMPRESSED_SCREENSHOT,
	RENDER_CAPTURE_WW3D_SCREENSHOT,
	RENDER_CAPTURE_MOVIE,
	RENDER_CAPTURE_PROFILER,
	RENDER_CAPTURE_VISUAL_SMOKE
};

struct RenderCaptureHandle
{
	RenderCaptureHandle();

	RenderCaptureKind kind;
	unsigned int requestId;
	unsigned int generation;
};

typedef void (*RenderCaptureCompletedCallback)(void *consumer,
	const RenderCaptureHandle *handle, unsigned int width,
	unsigned int height, size_t rowPitch, RenderFormat format,
	const void *pixels, size_t pixelBytes);
typedef void (*RenderCaptureCancelledCallback)(void *consumer,
	const RenderCaptureHandle *handle, RenderResult reason);

struct RenderCaptureRequestDescriptor
{
	RenderCaptureRequestDescriptor();

	RenderCaptureKind kind;
	void *consumer;
	RenderCaptureCompletedCallback completed;
	RenderCaptureCancelledCallback cancelled;
};

// Owner-thread capture requests are bounded and generation-tagged. The
// renderer supplies one readback to completeVisible(), allowing multiple
// consumers to share the same visible back-buffer copy.
class RenderCaptureQueue
{
public:
	explicit RenderCaptureQueue(unsigned int capacity = 8);
	~RenderCaptureQueue();

	RenderResult enqueue(const RenderCaptureRequestDescriptor &descriptor,
		RenderCaptureHandle *handle);
	RenderResult completeVisible(unsigned int width, unsigned int height,
		size_t rowPitch, RenderFormat format, const void *pixels,
		size_t pixelBytes);
	unsigned int cancelStale(RenderResult reason);
	unsigned int cancelConsumer(void *consumer, RenderResult reason);
	unsigned int cancelCurrent(RenderResult reason);
	void shutdown(RenderResult reason);
	void reset();
	// Bind the queue to the render-owner thread before first use. All mutable
	// queue operations reject calls from other threads.
	bool bindOwnerThread();
	void advanceGeneration();
	unsigned int generation() const;
	unsigned int pendingCount() const;

private:
	RenderCaptureQueue(const RenderCaptureQueue &);
	RenderCaptureQueue &operator=(const RenderCaptureQueue &);

	struct Impl;
	Impl *m_impl;
};

// Tracks the first command-submission failure in one owner-thread frame. The
// owner is responsible for calling reset() only after a successful beginFrame.
class RenderFrameFailureLatch
{
public:
	RenderFrameFailureLatch();

	bool record(RenderResult result);
	void reset();
	bool hasFailure() const;
	bool hasDeviceRemoval() const;
	RenderResult result() const;
	RenderResult commandResult() const;

private:
	bool m_failed;
	bool m_deviceRemoved;
	RenderResult m_result;
	RenderResult m_commandResult;
};

// Separates owner-thread command failures from frame teardown and presentation
// state. Backends must not present a frame after a command failure; callers use
// presentationResult()/wasPresented() to distinguish a deliberately dropped
// frame from a presentation or device-lifecycle failure.
class RenderFrameOutcome
{
public:
	RenderFrameOutcome();

	bool recordCommandFailure(RenderResult result);
	void recordEndFrame(RenderResult result);
	void recordCapture(RenderResult result);
	void recordPresentation(RenderResult result);
	void recordRecovery(RenderResult result);
	void markFrameEnded();
	// Queue admission is not backend execution or visible presentation.
	void markSubmitted();
	void markPresented();
	void setOperational(bool operational);

	bool hasCommandFailure() const;
	bool hasLifecycleFailure() const;
	bool hasDeviceRemoval() const;
	bool wasSubmitted() const;
	bool wasPresented() const;
	bool frameEnded() const;
	bool isOperational() const;
	RenderResult commandResult() const;
	RenderResult endFrameResult() const;
	RenderResult captureResult() const;
	RenderResult presentationResult() const;
	RenderResult recoveryResult() const;
	RenderResult result() const;

private:
	RenderFrameFailureLatch m_commandFailure;
	RenderResult m_endFrameResult;
	RenderResult m_captureResult;
	RenderResult m_presentationResult;
	RenderResult m_recoveryResult;
	bool m_deviceRemoved;
	bool m_frameEnded;
	bool m_submitted;
	bool m_presented;
	bool m_operational;
};

// Keeps a diagnostic capture request alive across non-visible frames and
// transient capture failures, while bounding retries deterministically.
class RenderCaptureRequest
{
public:
	enum { MAX_FAILURES = 3 };

	RenderCaptureRequest();

	void request();
	void clear();
	bool isRequested() const;
	bool shouldAttempt(bool visibleFrame) const;
	void recordSuccess();
	void recordFailure();
	unsigned int failureCount() const;

private:
	bool m_requested;
	unsigned int m_failureCount;
};

enum RenderVertexFormat
{
	RENDER_VERTEX_POSITION3_COLOR = 1,
	RENDER_VERTEX_POSITION3_NORMAL_COLOR_TEX1 = 2
};

// Temporary source-compatible spellings for the legacy backend.  Remove them
// after that adapter consumes the neutral descriptor types directly.
typedef RenderVertexFormat LegacyVertexFormat;

enum RenderVertexSemantic
{
	RENDER_VERTEX_SEMANTIC_POSITION,
	RENDER_VERTEX_SEMANTIC_NORMAL,
	RENDER_VERTEX_SEMANTIC_DIFFUSE,
	RENDER_VERTEX_SEMANTIC_SPECULAR,
	RENDER_VERTEX_SEMANTIC_TEXTURE_COORDINATE
};

typedef RenderVertexSemantic LegacyVertexSemantic;

enum RenderVertexDataFormat
{
	RENDER_VERTEX_DATA_FLOAT1,
	RENDER_VERTEX_DATA_FLOAT2,
	RENDER_VERTEX_DATA_FLOAT3,
	RENDER_VERTEX_DATA_FLOAT4,
	RENDER_VERTEX_DATA_COLOR_BGRA8
};

// Compatibility spelling for the existing backend adapter.  The native API
// above remains the source of truth and carries no API-specific FVF value.
typedef RenderVertexDataFormat LegacyVertexDataFormat;

struct LegacyVertexElement
{
	LegacyVertexElement();

	LegacyVertexSemantic semantic;
	unsigned int semanticIndex;
	LegacyVertexDataFormat format;
	unsigned int byteOffset;
};

struct LegacyVertexLayout
{
	enum { MAX_ELEMENT_COUNT = 12 };

	LegacyVertexLayout();

	unsigned int stride;
	unsigned int elementCount;
	// Legacy XYZRHW/POSITIONT data is already in viewport space and must not be
	// transformed by the world/view/projection matrices.
	bool preTransformed;
	LegacyVertexElement elements[MAX_ELEMENT_COUNT];
};

// Backend-neutral description of one vertex stream.  Keep this independent of
// LegacyVertexLayout so native consumers can be compiled without the legacy
// FVF adapter.
struct RenderVertexElement
{
	RenderVertexElement() : semantic(RENDER_VERTEX_SEMANTIC_POSITION),
		semanticIndex(0), format(RENDER_VERTEX_DATA_FLOAT3), byteOffset(0) {}

	RenderVertexSemantic semantic;
	unsigned int semanticIndex;
	RenderVertexDataFormat format;
	unsigned int byteOffset;
};

struct RenderVertexLayout
{
	enum { MAX_ELEMENT_COUNT = 12 };

	RenderVertexLayout() : stride(0), elementCount(0), preTransformed(false) {}

	unsigned int stride;
	unsigned int elementCount;
	// Pre-transformed positions are already in viewport space.
	bool preTransformed;
	RenderVertexElement elements[MAX_ELEMENT_COUNT];
};

enum RenderPrimitiveTopology
{
	RENDER_PRIMITIVE_TRIANGLE_LIST,
	RENDER_PRIMITIVE_TRIANGLE_STRIP,
	RENDER_PRIMITIVE_LINE_LIST,
	RENDER_PRIMITIVE_LINE_STRIP
};

// Coordinates and dimensions are pixels; depth remains in the neutral [0, 1]
// renderer range.  The descriptor deliberately uses floats so conversion from
// integer legacy viewports preserves the existing backend call exactly.
struct RenderViewport
{
	RenderViewport() : x(0.0f), y(0.0f), width(0.0f), height(0.0f),
		minimumDepth(0.0f), maximumDepth(1.0f) {}
	RenderViewport(float xValue, float yValue, float widthValue,
		float heightValue, float minimumDepthValue, float maximumDepthValue) :
		x(xValue), y(yValue), width(widthValue), height(heightValue),
		minimumDepth(minimumDepthValue), maximumDepth(maximumDepthValue) {}

	float x;
	float y;
	float width;
	float height;
	float minimumDepth;
	float maximumDepth;
};

enum RenderClearFlags
{
	RENDER_CLEAR_COLOR = 1,
	RENDER_CLEAR_DEPTH = 2,
	RENDER_CLEAR_STENCIL = 4
};

class IRenderContext
{
public:
	virtual ~IRenderContext() {}
	virtual RenderResult beginFrame() = 0;
	virtual RenderResult updateBuffer(GpuHandle buffer, const void *data,
		size_t byteCount, size_t destinationOffset,
		RenderBufferUpdateMode mode = RENDER_BUFFER_UPDATE_PRESERVE) = 0;
	virtual RenderResult clear(const RenderFloat4 &color, float depth,
		unsigned int stencil) = 0;
	virtual RenderResult clearTargets(unsigned int clearFlags,
		const RenderFloat4 &color, float depth, unsigned int stencil) = 0;
	virtual RenderResult setRenderTargets(
		const RenderTargetBinding &binding) = 0;
	virtual RenderResult setRenderTargets(GpuHandle colorTarget,
		GpuHandle depthTarget) = 0;
	virtual RenderResult setViewport(float x, float y, float width,
		float height, float minimumDepth, float maximumDepth) = 0;
	virtual RenderResult setLegacyState(const LegacyLogicalState &state,
		LegacyVertexFormat vertexFormat, unsigned int texturePresenceMask) = 0;
	virtual RenderResult setLegacyStateForLayout(const LegacyLogicalState &state,
		const LegacyVertexLayout &vertexLayout,
		unsigned int texturePresenceMask) = 0;
	virtual RenderResult setVertexBuffer(GpuHandle buffer, unsigned int stride,
		unsigned int offset) = 0;
	virtual RenderResult setIndexBuffer(GpuHandle buffer, RenderFormat format,
		unsigned int offset) = 0;
	virtual RenderResult setTexture(unsigned int stage, GpuHandle texture) = 0;
	virtual RenderResult setPrimitiveTopology(RenderPrimitiveTopology topology) = 0;
	virtual RenderResult draw(unsigned int vertexCount,
		unsigned int startVertex) = 0;
	virtual RenderResult drawIndexed(unsigned int indexCount,
		unsigned int startIndex, int baseVertex) = 0;
	virtual RenderResult endFrame() = 0;
};

class IRenderDevice
{
public:
	virtual ~IRenderDevice() {}
	virtual RenderBackend backend() const = 0;
	virtual bool isOperational() const = 0;
	virtual RenderResult initialize(const RenderDeviceParameters &parameters) = 0;
	virtual void shutdown() = 0;
	virtual IRenderContext *immediateContext() = 0;
	virtual RenderResult createBuffer(const BufferDescriptor &descriptor,
		const void *initialData, size_t initialDataBytes, GpuHandle *buffer) = 0;
	virtual RenderResult createTexture(const TextureDescriptor &descriptor,
		const TextureSubresourceData *initialData,
		unsigned int initialDataCount, GpuHandle *texture) = 0;
	// Refreshes every texture subresource in place when the existing resource
	// has compatible shape, format, binding, and update capability.  A
	// RENDER_RESULT_UNSUPPORTED result means callers may recreate the resource;
	// stale handles and active output hazards remain invalid arguments.
	virtual RenderResult refreshTexture(GpuHandle texture,
		const TextureDescriptor &descriptor,
		const TextureSubresourceData *data,
		unsigned int dataCount) = 0;
	// Copies the currently bound color target into a compatible texture while a
	// frame is open.  This is an owner-thread GPU copy: it never reads through
	// the legacy device or exposes native backend resources to game code.
	virtual RenderResult copyActiveColorTargetToTexture(GpuHandle texture) = 0;
	virtual bool destroyResource(GpuHandle resource) = 0;
	virtual RenderResult recoverDevice() = 0;
	// A zero dimension represents a minimized window.  Keep the last valid
	// swap-chain targets and treat this notification as a successful no-op;
	// callers can submit the next non-zero size when the window is restored.
	virtual RenderResult resize(unsigned int width, unsigned int height) = 0;
	virtual RenderResult present() = 0;
	virtual RenderResult getBackBufferInfo(RenderBackBufferInfo *info) const = 0;
	virtual RenderResult captureBackBuffer(void *destination,
		size_t destinationBytes, size_t destinationRowPitch,
		RenderFormat *format) = 0;
	virtual RenderResult getDebugValidationErrorCount(unsigned int *count) const = 0;
	// Requests the D3D11 debug-layer live-object report while the device is
	// still alive. The report is emitted through the normal graphics-debug
	// output channel; retail devices without the optional SDK layer return
	// RENDER_RESULT_UNSUPPORTED and remain fully operational.
	virtual RenderResult reportDebugLiveObjects() = 0;
};

IRenderDevice *CreateD3D11RenderDevice();
}
}

#endif
