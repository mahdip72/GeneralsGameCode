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
	RENDER_FORMAT_R32_UINT
};

enum TextureBinding
{
	RENDER_TEXTURE_SHADER_RESOURCE = 1,
	RENDER_TEXTURE_RENDER_TARGET = 2,
	RENDER_TEXTURE_DEPTH_STENCIL = 4
};

struct RenderDeviceParameters
{
	RenderDeviceParameters();

	RenderBackend backend;
	void *window;
	unsigned int width;
	unsigned int height;
	bool enableDebugLayer;
	bool enableVsync;
};

struct BufferDescriptor
{
	BufferDescriptor();

	size_t byteCount;
	unsigned int stride;
	unsigned int binding;
	RenderUsage usage;
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
	RenderFormat format;
	unsigned int binding;
	RenderUsage usage;
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

enum LegacyVertexFormat
{
	RENDER_VERTEX_POSITION3_COLOR = 1,
	RENDER_VERTEX_POSITION3_NORMAL_COLOR_TEX1 = 2
};

enum RenderPrimitiveTopology
{
	RENDER_PRIMITIVE_TRIANGLE_LIST,
	RENDER_PRIMITIVE_TRIANGLE_STRIP,
	RENDER_PRIMITIVE_LINE_LIST,
	RENDER_PRIMITIVE_LINE_STRIP
};

class IRenderContext
{
public:
	virtual ~IRenderContext() {}
	virtual RenderResult beginFrame() = 0;
	virtual RenderResult updateBuffer(GpuHandle buffer, const void *data,
		size_t byteCount, size_t destinationOffset) = 0;
	virtual RenderResult clear(const RenderFloat4 &color, float depth,
		unsigned int stencil) = 0;
	virtual RenderResult setViewport(float x, float y, float width,
		float height, float minimumDepth, float maximumDepth) = 0;
	virtual RenderResult setLegacyState(const LegacyLogicalState &state,
		LegacyVertexFormat vertexFormat, unsigned int texturePresenceMask) = 0;
	virtual RenderResult setVertexBuffer(GpuHandle buffer, unsigned int stride,
		unsigned int offset) = 0;
	virtual RenderResult setTexture(unsigned int stage, GpuHandle texture) = 0;
	virtual RenderResult setPrimitiveTopology(RenderPrimitiveTopology topology) = 0;
	virtual RenderResult draw(unsigned int vertexCount,
		unsigned int startVertex) = 0;
	virtual RenderResult endFrame() = 0;
};

class IRenderDevice
{
public:
	virtual ~IRenderDevice() {}
	virtual RenderBackend backend() const = 0;
	virtual RenderResult initialize(const RenderDeviceParameters &parameters) = 0;
	virtual void shutdown() = 0;
	virtual IRenderContext *immediateContext() = 0;
	virtual RenderResult createBuffer(const BufferDescriptor &descriptor,
		const void *initialData, size_t initialDataBytes, GpuHandle *buffer) = 0;
	virtual RenderResult createTexture(const TextureDescriptor &descriptor,
		const TextureSubresourceData *initialData,
		unsigned int initialDataCount, GpuHandle *texture) = 0;
	virtual bool destroyResource(GpuHandle resource) = 0;
	virtual RenderResult resize(unsigned int width, unsigned int height) = 0;
	virtual RenderResult present() = 0;
	virtual RenderResult captureBackBuffer(void *destination,
		size_t destinationBytes, size_t destinationRowPitch,
		RenderFormat *format) = 0;
	virtual RenderResult getDebugValidationErrorCount(unsigned int *count) const = 0;
};

IRenderDevice *CreateD3D11RenderDevice();
}
}

#endif
