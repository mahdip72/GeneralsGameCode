#include "Utility/CppMacros.h"
#include "Renderer/NativeW3DResources.h"
#include "Renderer/ThreadedRenderDevice.h"
#include "nativew3dbufferowner.h"
#include "nativew3d2.h"

#include <cstdio>
#include <cstring>
#include <new>
#include <vector>
#include <windows.h>

namespace
{
using namespace rts::render;

int Check(bool condition, const char *message)
{
	if (condition)
	{
		return 0;
	}
	std::fprintf(stderr, "FAIL: %s\n", message);
	return 1;
}

class FakeRenderDevice;

struct FakeRenderControl
{
	FakeRenderControl() : failCreate(0), failUpdate(0), failRefresh(0),
		failCopy(0), createCalls(0), updateCalls(0), refreshCalls(0),
		copyCalls(0) {}
	volatile long failCreate;
	volatile long failUpdate;
	volatile long failRefresh;
	volatile long failCopy;
	volatile long createCalls;
	volatile long updateCalls;
	volatile long refreshCalls;
	volatile long copyCalls;
};

bool IsSet(volatile long *value)
{
	return InterlockedCompareExchange(value, 0, 0) != 0;
}

long ReadCount(volatile long *value)
{
	return InterlockedCompareExchange(value, 0, 0);
}

class FakeRenderContext : public IRenderContext
{
public:
	explicit FakeRenderContext(FakeRenderDevice *device) :
		m_device(device), m_frameOpen(false) {}

	RenderResult beginFrame() override;
	RenderResult updateBuffer(GpuHandle buffer, const void *data,
		size_t byteCount, size_t destinationOffset,
		RenderBufferUpdateMode mode) override;
	RenderResult clear(const RenderFloat4 &, float, unsigned int) override
	{
		return m_frameOpen ? RENDER_RESULT_OK : RENDER_RESULT_INVALID_ARGUMENT;
	}
	RenderResult clearTargets(unsigned int, const RenderFloat4 &, float,
		unsigned int) override
	{
		return m_frameOpen ? RENDER_RESULT_OK : RENDER_RESULT_INVALID_ARGUMENT;
	}
	RenderResult setRenderTargets(const RenderTargetBinding &) override
	{
		return m_frameOpen ? RENDER_RESULT_OK : RENDER_RESULT_INVALID_ARGUMENT;
	}
	RenderResult setRenderTargets(GpuHandle, GpuHandle) override
	{
		return m_frameOpen ? RENDER_RESULT_OK : RENDER_RESULT_INVALID_ARGUMENT;
	}
	RenderResult setViewport(float, float, float, float, float, float) override
	{
		return m_frameOpen ? RENDER_RESULT_OK : RENDER_RESULT_INVALID_ARGUMENT;
	}
	RenderResult setLegacyState(const LegacyLogicalState &, LegacyVertexFormat,
		unsigned int) override
	{
		return m_frameOpen ? RENDER_RESULT_OK : RENDER_RESULT_INVALID_ARGUMENT;
	}
	RenderResult setLegacyStateForLayout(const LegacyLogicalState &,
		const LegacyVertexLayout &, unsigned int) override
	{
		return m_frameOpen ? RENDER_RESULT_OK : RENDER_RESULT_INVALID_ARGUMENT;
	}
	RenderResult setVertexBuffer(GpuHandle, unsigned int,
		unsigned int) override
	{
		return m_frameOpen ? RENDER_RESULT_OK : RENDER_RESULT_INVALID_ARGUMENT;
	}
	RenderResult setIndexBuffer(GpuHandle, RenderFormat,
		unsigned int) override
	{
		return m_frameOpen ? RENDER_RESULT_OK : RENDER_RESULT_INVALID_ARGUMENT;
	}
	RenderResult setTexture(unsigned int, GpuHandle) override
	{
		return m_frameOpen ? RENDER_RESULT_OK : RENDER_RESULT_INVALID_ARGUMENT;
	}
	RenderResult setPrimitiveTopology(RenderPrimitiveTopology) override
	{
		return m_frameOpen ? RENDER_RESULT_OK : RENDER_RESULT_INVALID_ARGUMENT;
	}
	RenderResult draw(unsigned int, unsigned int) override
	{
		return m_frameOpen ? RENDER_RESULT_OK : RENDER_RESULT_INVALID_ARGUMENT;
	}
	RenderResult drawIndexed(unsigned int, unsigned int, int) override
	{
		return m_frameOpen ? RENDER_RESULT_OK : RENDER_RESULT_INVALID_ARGUMENT;
	}
	RenderResult endFrame() override;

	bool IsFrameOpen() const { return m_frameOpen; }

private:
	FakeRenderDevice *m_device;
	bool m_frameOpen;
};

struct FakeResource
{
	FakeResource() : texture(false), gpuAuthority(false) {}

	GpuHandle handle;
	bool texture;
	bool gpuAuthority;
	BufferDescriptor buffer;
	TextureDescriptor textureDescriptor;
	std::vector<unsigned char> liveBytes;
	std::vector<unsigned char> recoveryBytes;
};

class FakeRenderDevice : public IRenderDevice
{
public:
	explicit FakeRenderDevice(bool operational = true,
		FakeRenderControl *control = 0) : m_handles(16), m_resources(16),
		m_context(this), m_operational(operational), m_failDestroy(false),
		m_destroyCount(0),
		m_refreshCount(0), m_control(control) {}

	RenderBackend backend() const override { return RENDER_BACKEND_D3D11; }
	bool isOperational() const override { return m_operational; }
	RenderResult initialize(const RenderDeviceParameters &) override
	{
		if (m_operational)
		{
			return RENDER_RESULT_INVALID_ARGUMENT;
		}
		m_operational = true;
		return RENDER_RESULT_OK;
	}
	void shutdown() override { m_operational = false; }
	IRenderContext *immediateContext() override
	{
		return m_operational ? &m_context : 0;
	}

	RenderResult createBuffer(const BufferDescriptor &descriptor,
		const void *initialData, size_t initialDataBytes,
		GpuHandle *buffer) override
	{
		if (m_control != 0)
		{
			InterlockedIncrement(&m_control->createCalls);
			if (IsSet(&m_control->failCreate))
			{
				return RENDER_RESULT_FAILED;
			}
		}
		if (!m_operational || buffer == 0 || descriptor.byteCount == 0 ||
			descriptor.binding == 0 ||
			(initialData == 0 && initialDataBytes != 0) ||
			(initialData != 0 && initialDataBytes != descriptor.byteCount) ||
			(descriptor.usage == RENDER_USAGE_IMMUTABLE && initialData == 0))
		{
			return RENDER_RESULT_INVALID_ARGUMENT;
		}
		*buffer = GpuHandle();
		GpuHandle handle = m_handles.allocate();
		if (!handle.isValid())
		{
			return RENDER_RESULT_OUT_OF_MEMORY;
		}
		FakeResource &resource = m_resources[handle.index()];
		resource = FakeResource();
		resource.handle = handle;
		resource.buffer = descriptor;
		resource.liveBytes.assign(descriptor.byteCount, 0);
		if (initialData != 0)
		{
			std::memcpy(&resource.liveBytes[0], initialData,
				descriptor.byteCount);
		}
		resource.recoveryBytes = resource.liveBytes;
		*buffer = handle;
		return RENDER_RESULT_OK;
	}

	RenderResult createTexture(const TextureDescriptor &descriptor,
		const TextureSubresourceData *initialData, unsigned int initialDataCount,
		GpuHandle *texture) override
	{
		if (!m_operational || texture == 0 || descriptor.width == 0 ||
			descriptor.height == 0 || descriptor.mipCount == 0 ||
			descriptor.arrayCount == 0 || descriptor.binding == 0 ||
			(initialData == 0 && initialDataCount != 0) ||
			(initialData != 0 && initialDataCount !=
				descriptor.mipCount * descriptor.arrayCount) ||
			(descriptor.usage == RENDER_USAGE_IMMUTABLE && initialData == 0))
		{
			return RENDER_RESULT_INVALID_ARGUMENT;
		}
		*texture = GpuHandle();
		GpuHandle handle = m_handles.allocate();
		if (!handle.isValid())
		{
			return RENDER_RESULT_OUT_OF_MEMORY;
		}
		FakeResource &resource = m_resources[handle.index()];
		resource = FakeResource();
		resource.handle = handle;
		resource.texture = true;
		resource.textureDescriptor = descriptor;
		*texture = handle;
		return RENDER_RESULT_OK;
	}

	RenderResult updateBufferResource(GpuHandle buffer, const void *data,
		size_t byteCount, size_t destinationOffset,
		RenderBufferUpdateMode mode) override
	{
		return UpdateBuffer(buffer, data, byteCount, destinationOffset, mode);
	}

	RenderResult refreshTexture(GpuHandle texture,
		const TextureDescriptor &descriptor,
		const TextureSubresourceData *data, unsigned int dataCount) override
	{
		if (m_control != 0)
		{
			InterlockedIncrement(&m_control->refreshCalls);
			if (IsSet(&m_control->failRefresh))
			{
				return RENDER_RESULT_FAILED;
			}
		}
		FakeResource *resource = Find(texture);
		if (resource == 0 || !resource->texture || data == 0 ||
			dataCount != descriptor.mipCount * descriptor.arrayCount)
		{
			return RENDER_RESULT_INVALID_ARGUMENT;
		}
		if (!SameTexture(resource->textureDescriptor, descriptor) ||
			descriptor.usage == RENDER_USAGE_IMMUTABLE)
		{
			return RENDER_RESULT_UNSUPPORTED;
		}
		++m_refreshCount;
		resource->gpuAuthority = false;
		return RENDER_RESULT_OK;
	}

	RenderResult copyActiveColorTargetToTexture(GpuHandle texture) override
	{
		if (m_control != 0)
		{
			InterlockedIncrement(&m_control->copyCalls);
			if (IsSet(&m_control->failCopy))
			{
				return RENDER_RESULT_FAILED;
			}
		}
		FakeResource *resource = Find(texture);
		if (!m_context.IsFrameOpen() || resource == 0 || !resource->texture)
		{
			return RENDER_RESULT_INVALID_ARGUMENT;
		}
		const unsigned int required = RENDER_TEXTURE_SHADER_RESOURCE |
			RENDER_TEXTURE_RENDER_TARGET;
		if ((resource->textureDescriptor.binding & required) != required ||
			resource->textureDescriptor.mipCount != 1 ||
			resource->textureDescriptor.arrayCount != 1 ||
			resource->textureDescriptor.usage == RENDER_USAGE_IMMUTABLE)
		{
			return RENDER_RESULT_UNSUPPORTED;
		}
		resource->gpuAuthority = true;
		return RENDER_RESULT_OK;
	}

	bool destroyResource(GpuHandle handle) override
	{
		if (m_failDestroy)
		{
			return false;
		}
		FakeResource *resource = Find(handle);
		if (resource == 0 || !m_handles.release(handle))
		{
			return false;
		}
		*resource = FakeResource();
		++m_destroyCount;
		return true;
	}

	RenderResult recoverDevice() override
	{
		if (!m_operational)
		{
			return RENDER_RESULT_INVALID_ARGUMENT;
		}
		for (size_t index = 0; index < m_resources.size(); ++index)
		{
			FakeResource &resource = m_resources[index];
			if (!resource.handle.isValid())
			{
				continue;
			}
			if (!resource.texture)
			{
				if (resource.buffer.usage == RENDER_USAGE_IMMUTABLE)
				{
					resource.liveBytes = resource.recoveryBytes;
				}
				else if (!resource.liveBytes.empty())
				{
					std::memset(&resource.liveBytes[0], 0,
						resource.liveBytes.size());
				}
			}
			else if (resource.gpuAuthority)
			{
				resource.gpuAuthority = false;
			}
		}
		return RENDER_RESULT_OK;
	}

	RenderResult resize(unsigned int width, unsigned int height) override
	{
		return width != 0 && height != 0 ? RENDER_RESULT_OK :
			RENDER_RESULT_INVALID_ARGUMENT;
	}
	RenderResult present() override { return RENDER_RESULT_OK; }
	RenderResult getBackBufferInfo(RenderBackBufferInfo *info) const override
	{
		if (info == 0)
		{
			return RENDER_RESULT_INVALID_ARGUMENT;
		}
		info->width = 4;
		info->height = 4;
		info->format = RENDER_FORMAT_R8G8B8A8_UNORM;
		return RENDER_RESULT_OK;
	}
	RenderResult captureBackBuffer(void *, size_t, size_t,
		RenderFormat *) override { return RENDER_RESULT_UNSUPPORTED; }
	RenderResult getDebugValidationErrorCount(unsigned int *count) const override
	{
		if (count == 0)
		{
			return RENDER_RESULT_INVALID_ARGUMENT;
		}
		*count = 0;
		return RENDER_RESULT_OK;
	}
	RenderResult reportDebugLiveObjects() override
	{
		return RENDER_RESULT_UNSUPPORTED;
	}

	RenderResult UpdateBuffer(GpuHandle handle, const void *data,
		size_t byteCount, size_t destinationOffset,
		RenderBufferUpdateMode mode)
	{
		if (m_control != 0)
		{
			InterlockedIncrement(&m_control->updateCalls);
			if (IsSet(&m_control->failUpdate))
			{
				return RENDER_RESULT_FAILED;
			}
		}
		FakeResource *resource = Find(handle);
		if (resource == 0 || resource->texture || data == 0 || byteCount == 0 ||
			destinationOffset > resource->buffer.byteCount ||
			byteCount > resource->buffer.byteCount - destinationOffset ||
			resource->buffer.usage == RENDER_USAGE_IMMUTABLE ||
			mode < RENDER_BUFFER_UPDATE_PRESERVE ||
			mode > RENDER_BUFFER_UPDATE_NO_OVERWRITE)
		{
			return RENDER_RESULT_INVALID_ARGUMENT;
		}
		std::memcpy(&resource->liveBytes[destinationOffset], data, byteCount);
		std::memcpy(&resource->recoveryBytes[destinationOffset], data,
			byteCount);
		return RENDER_RESULT_OK;
	}

	bool BufferEquals(GpuHandle handle, const void *data, size_t byteCount) const
	{
		const FakeResource *resource = Find(handle);
		return resource != 0 && !resource->texture &&
			resource->liveBytes.size() == byteCount &&
			std::memcmp(&resource->liveBytes[0], data, byteCount) == 0;
	}

	unsigned int DestroyCount() const { return m_destroyCount; }
	unsigned int RefreshCount() const { return m_refreshCount; }
	unsigned int LiveCount() const { return m_handles.liveCount(); }
	void FailDestroy(bool fail) { m_failDestroy = fail; }

private:
	static bool SameTexture(const TextureDescriptor &left,
		const TextureDescriptor &right)
	{
		return left.width == right.width && left.height == right.height &&
			left.mipCount == right.mipCount &&
			left.arrayCount == right.arrayCount &&
			left.dimension == right.dimension && left.format == right.format &&
			left.binding == right.binding && left.usage == right.usage;
	}

	FakeResource *Find(GpuHandle handle)
	{
		if (!m_handles.isLive(handle) || handle.index() >= m_resources.size())
		{
			return 0;
		}
		FakeResource &resource = m_resources[handle.index()];
		return resource.handle == handle ? &resource : 0;
	}

	const FakeResource *Find(GpuHandle handle) const
	{
		if (!m_handles.isLive(handle) || handle.index() >= m_resources.size())
		{
			return 0;
		}
		const FakeResource &resource = m_resources[handle.index()];
		return resource.handle == handle ? &resource : 0;
	}

	GpuHandleAllocator m_handles;
	std::vector<FakeResource> m_resources;
	FakeRenderContext m_context;
	bool m_operational;
	bool m_failDestroy;
	unsigned int m_destroyCount;
	unsigned int m_refreshCount;
	FakeRenderControl *m_control;
};

IRenderDevice *CreateThreadedFakeRenderDevice(void *context)
{
	return new (std::nothrow) FakeRenderDevice(false,
		static_cast<FakeRenderControl *>(context));
}

RenderResult FakeRenderContext::beginFrame()
{
	if (m_frameOpen)
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	m_frameOpen = true;
	return RENDER_RESULT_OK;
}

RenderResult FakeRenderContext::updateBuffer(GpuHandle buffer,
	const void *data, size_t byteCount, size_t destinationOffset,
	RenderBufferUpdateMode mode)
{
	return !m_frameOpen ? RENDER_RESULT_INVALID_ARGUMENT :
		m_device->UpdateBuffer(buffer, data, byteCount, destinationOffset,
			mode);
}

RenderResult FakeRenderContext::endFrame()
{
	if (!m_frameOpen)
	{
		return RENDER_RESULT_INVALID_ARGUMENT;
	}
	m_frameOpen = false;
	return RENDER_RESULT_OK;
}

struct WrongOwnerUpdate
{
	NativeW3DResources *resources;
	GpuHandle handle;
	unsigned int value;
	RenderResult result;
};

DWORD WINAPI UpdateFromWrongOwner(void *parameter)
{
	WrongOwnerUpdate *request = static_cast<WrongOwnerUpdate *>(parameter);
	request->result = request->resources->UpdateBuffer(request->handle,
		&request->value, sizeof(request->value), 0,
		RENDER_BUFFER_UPDATE_PRESERVE);
	return 0;
}

struct WorkerDestroy
{
	NativeW3DResources *resources;
};

DWORD WINAPI DestroyFromWorker(void *parameter)
{
	WorkerDestroy *request = static_cast<WorkerDestroy *>(parameter);
	delete request->resources;
	request->resources = 0;
	return 0;
}

int TestThreadedResourceCompletion()
{
	int result = 0;
	FakeRenderControl control;
	ThreadedRenderOptions options;
	options.serial = false;
	options.maxFramesInFlight = 2;
	options.maxPacketBytes = 1024 * 1024;
	options.maxPacketCommands = 128;
	options.resourceCapacity = 2;
	IRenderDevice *device = CreateThreadedRenderDevice(
		CreateThreadedFakeRenderDevice, &control, options);
	result |= Check(device != 0, "threaded resource fixture allocates");
	if (device == 0)
	{
		return result;
	}
	RenderDeviceParameters parameters;
	parameters.backend = RENDER_BACKEND_D3D11;
	parameters.window = reinterpret_cast<void *>(1);
	parameters.width = 4;
	parameters.height = 4;
	result |= Check(device->initialize(parameters) == RENDER_RESULT_OK,
		"threaded resource fixture initializes its render owner");
	if (!device->isOperational())
	{
		delete device;
		return result;
	}

	NativeW3DResourceHost host(8);
	NativeW3DResources resources(2);
	result |= Check(host.Attach(device, device->immediateContext()) ==
		RENDER_RESULT_OK && resources.BindHost(&host) == RENDER_RESULT_OK,
		"resource host borrows the threaded producer facade");

	unsigned int bytes[4] = { 1, 2, 3, 4 };
	BufferDescriptor bufferDescriptor;
	bufferDescriptor.byteCount = sizeof(bytes);
	bufferDescriptor.stride = sizeof(unsigned int);
	bufferDescriptor.binding = RENDER_BUFFER_VERTEX;
	bufferDescriptor.usage = RENDER_USAGE_DYNAMIC;
	GpuHandle buffer;
	NativeW3DBufferDescription bufferDescription;
	result |= Check(resources.CreateBuffer(bufferDescriptor, 0,
		0, &buffer) == RENDER_RESULT_OK &&
		ReadCount(&control.createCalls) == 1 &&
		resources.DescribeBuffer(buffer, &bufferDescription) == RENDER_RESULT_OK &&
		bufferDescription.authority == NATIVE_W3D_CONTENT_INVALID &&
		resources.UpdateBuffer(buffer, bytes, sizeof(bytes) / 2, 0,
			RENDER_BUFFER_UPDATE_DISCARD) == RENDER_RESULT_OK &&
		ReadCount(&control.updateCalls) == 1 &&
		resources.DescribeBuffer(buffer, &bufferDescription) == RENDER_RESULT_OK &&
		bufferDescription.authority == NATIVE_W3D_CONTENT_INVALID,
		"pre-frame threaded partial upload completes without publishing whole authority");
	GpuHandle validated = GpuHandle(1, 1);
	result |= Check(resources.AcquireVertexBufferRange(buffer,
		sizeof(unsigned int), 0, 0, 2, &validated) == RENDER_RESULT_OK &&
		validated == buffer,
		"pre-frame partial DISCARD publishes its exact initialized range");
	validated = GpuHandle(1, 1);
	result |= Check(resources.AcquireVertexBufferRange(buffer,
		sizeof(unsigned int), 0, 2, 1, &validated) ==
		RENDER_RESULT_INVALID_ARGUMENT && !validated.isValid(),
		"pre-frame partial DISCARD rejects adjacent unwritten bytes");
	IRenderContext *context = device->immediateContext();
	result |= Check(context->beginFrame() == RENDER_RESULT_OK &&
		CurrentThreadedRenderFrameSequence(device) != 0 &&
		resources.UpdateBuffer(buffer, bytes, sizeof(bytes), 0,
			RENDER_BUFFER_UPDATE_DISCARD) == RENDER_RESULT_OK &&
		ReadCount(&control.updateCalls) == 1,
		"in-frame upload returns without a per-unlock render-owner fence");
	validated = GpuHandle(1, 1);
	result |= Check(resources.AcquireVertexBufferRange(buffer,
		sizeof(unsigned int), 0, 0, 4, &validated) ==
		RENDER_RESULT_INVALID_ARGUMENT && !validated.isValid() &&
		context->endFrame() == RENDER_RESULT_OK &&
		SubmitThreadedRenderFrame(device, false) == RENDER_RESULT_OK &&
		DrainThreadedRenderDevice(device) == RENDER_RESULT_OK &&
		ReadCount(&control.updateCalls) == 2,
		"accepted in-frame bytes remain fail-closed until owner completion");
	ThreadedRenderFrameCompletion asynchronousUploadCompletion;
	result |= Check(PollThreadedRenderCompletion(device,
		&asynchronousUploadCompletion) &&
		asynchronousUploadCompletion.result == RENDER_RESULT_OK &&
		!asynchronousUploadCompletion.resourceFailure &&
		resources.PublishThreadedCompletion(
			asynchronousUploadCompletion.sequence, false) == RENDER_RESULT_OK &&
		resources.AcquireVertexBufferRange(buffer, sizeof(unsigned int), 0,
			0, 4, &validated) == RENDER_RESULT_OK && validated == buffer,
		"matching completion publishes the exact accepted in-frame range");
	InterlockedExchange(&control.failUpdate, 1);
	result |= Check(context->beginFrame() == RENDER_RESULT_OK &&
		resources.UpdateBuffer(buffer, bytes, sizeof(bytes) / 2, 0,
			RENDER_BUFFER_UPDATE_DISCARD) == RENDER_RESULT_OK &&
		ReadCount(&control.updateCalls) == 2 &&
		context->endFrame() == RENDER_RESULT_OK &&
		SubmitThreadedRenderFrame(device, false) == RENDER_RESULT_OK &&
		DrainThreadedRenderDevice(device) == RENDER_RESULT_FAILED,
		"owner-side asynchronous upload failure remains observable at completion");
	ThreadedRenderFrameCompletion failedUploadCompletion;
	validated = buffer;
	result |= Check(PollThreadedRenderCompletion(device,
		&failedUploadCompletion) && failedUploadCompletion.resourceFailure &&
		resources.PublishThreadedCompletion(failedUploadCompletion.sequence,
			true) == RENDER_RESULT_OK &&
		resources.AcquireVertexBufferRange(buffer, sizeof(unsigned int), 0,
			0, 2, &validated) == RENDER_RESULT_INVALID_ARGUMENT &&
		!validated.isValid(),
		"failed completion invalidates only its pending buffer publication");
	InterlockedExchange(&control.failUpdate, 0);
	result |= Check(device->recoverDevice() == RENDER_RESULT_OK &&
		host.ReplaceContext(device->immediateContext()) == RENDER_RESULT_OK &&
		resources.RestoreStaticBuffersAfterRecovery() == RENDER_RESULT_OK &&
		resources.UpdateBuffer(buffer, bytes, sizeof(bytes), 0,
			RENDER_BUFFER_UPDATE_DISCARD) == RENDER_RESULT_OK,
		"dynamic owner recovers explicitly after asynchronous upload failure");
	unsigned short indices[3] = { 0, 1, 0 };
	BufferDescriptor indexDescriptor;
	indexDescriptor.byteCount = 4 * sizeof(unsigned short);
	indexDescriptor.stride = sizeof(unsigned short);
	indexDescriptor.binding = RENDER_BUFFER_INDEX;
	indexDescriptor.usage = RENDER_USAGE_DYNAMIC;
	GpuHandle indexBuffer;
	result |= Check(resources.CreateBuffer(indexDescriptor, 0, 0,
		&indexBuffer) == RENDER_RESULT_OK &&
		resources.UpdateBuffer(indexBuffer, indices, sizeof(indices), 0,
			RENDER_BUFFER_UPDATE_DISCARD) == RENDER_RESULT_OK,
		"threaded partial index DISCARD completes before exact acquisition");
	validated = buffer;
	result |= Check(resources.AcquireIndexBufferRange(indexBuffer,
		RENDER_FORMAT_R16_UINT, 0, 0, 3, &validated) == RENDER_RESULT_OK &&
		validated == indexBuffer,
		"threaded exact index acquisition accepts the written prefix");
	validated = buffer;
	result |= Check(resources.AcquireIndexBufferRange(indexBuffer,
		RENDER_FORMAT_R16_UINT, 0, 3, 1, &validated) ==
		RENDER_RESULT_INVALID_ARGUMENT && !validated.isValid(),
		"threaded exact index acquisition rejects adjacent unwritten bytes");
	LegacyLogicalState drawState;
	LegacyVertexLayout drawLayout;
	drawLayout.stride = sizeof(unsigned int);
	result |= Check(context->beginFrame() == RENDER_RESULT_OK &&
		context->setLegacyStateForLayout(drawState, drawLayout, 0) ==
			RENDER_RESULT_OK &&
		context->setVertexBuffer(buffer, sizeof(unsigned int), 0) ==
			RENDER_RESULT_OK &&
		context->setIndexBuffer(indexBuffer, RENDER_FORMAT_R16_UINT, 0) ==
			RENDER_RESULT_OK &&
		context->setPrimitiveTopology(RENDER_PRIMITIVE_TRIANGLE_LIST) ==
			RENDER_RESULT_OK &&
		context->draw(2, 0) == RENDER_RESULT_OK &&
		context->drawIndexed(3, 0, 0) == RENDER_RESULT_OK &&
		context->endFrame() == RENDER_RESULT_OK &&
		SubmitThreadedRenderFrame(device, false) == RENDER_RESULT_OK &&
		DrainThreadedRenderDevice(device) == RENDER_RESULT_OK,
		"a pre-frame partial upload remains bindable by an exact later threaded draw");
	ThreadedRenderFrameCompletion uploadCompletion;
	result |= Check(PollThreadedRenderCompletion(device, &uploadCompletion) &&
		uploadCompletion.result == RENDER_RESULT_OK &&
		!uploadCompletion.resourceFailure &&
		resources.PublishThreadedCompletion(uploadCompletion.sequence, false) ==
			RENDER_RESULT_OK,
		"later draw completion preserves pre-frame upload authority");

	result |= Check(context->beginFrame() == RENDER_RESULT_OK &&
		context->setLegacyStateForLayout(drawState, drawLayout, 0) ==
			RENDER_RESULT_OK &&
		context->setVertexBuffer(buffer, sizeof(unsigned int), 0) ==
			RENDER_RESULT_OK &&
		context->setIndexBuffer(indexBuffer, RENDER_FORMAT_R16_UINT, 0) ==
			RENDER_RESULT_OK &&
		context->setPrimitiveTopology(RENDER_PRIMITIVE_TRIANGLE_LIST) ==
			RENDER_RESULT_OK &&
		context->drawIndexed(1, 3, 0) == RENDER_RESULT_OK &&
		context->endFrame() == RENDER_RESULT_OK &&
		SubmitThreadedRenderFrame(device, false) == RENDER_RESULT_OK &&
		DrainThreadedRenderDevice(device) == RENDER_RESULT_FAILED,
		"threaded owner rejects a draw that reaches adjacent unwritten bytes");
	ThreadedRenderFrameCompletion adjacentCompletion;
	result |= Check(PollThreadedRenderCompletion(device, &adjacentCompletion) &&
		adjacentCompletion.result == RENDER_RESULT_FAILED &&
		adjacentCompletion.resourceFailure &&
		resources.PublishThreadedCompletion(adjacentCompletion.sequence, true) ==
			RENDER_RESULT_OK,
		"adjacent range failure publishes aggregate resource invalidation");
	validated = buffer;
	result |= Check(resources.AcquireVertexBufferRange(buffer,
		sizeof(unsigned int), 0, 0, 2, &validated) ==
		RENDER_RESULT_OK && validated == buffer &&
		device->recoverDevice() == RENDER_RESULT_OK &&
		host.ReplaceContext(device->immediateContext()) == RENDER_RESULT_OK &&
		resources.RestoreStaticBuffersAfterRecovery() == RENDER_RESULT_OK,
		"failed indexed draw preserves an unrelated initialized vertex range before recovery");
	result |= Check(resources.Destroy(indexBuffer),
		"exact owner destruction releases the invalidated threaded index slot");

	InterlockedExchange(&control.failUpdate, 1);
	result |= Check(resources.UpdateBuffer(buffer, bytes, sizeof(bytes), 0,
		RENDER_BUFFER_UPDATE_DISCARD) == RENDER_RESULT_FAILED &&
		ReadCount(&control.updateCalls) == 6 &&
		resources.DescribeBuffer(buffer, &bufferDescription) == RENDER_RESULT_OK &&
		bufferDescription.authority == NATIVE_W3D_CONTENT_INVALID,
		"failed pre-frame threaded upload invalidates publication authority");
	InterlockedExchange(&control.failUpdate, 0);
	result |= Check(device->recoverDevice() == RENDER_RESULT_OK &&
		host.ReplaceContext(device->immediateContext()) == RENDER_RESULT_OK &&
		resources.RestoreStaticBuffersAfterRecovery() == RENDER_RESULT_OK &&
		resources.UpdateBuffer(buffer, bytes, sizeof(bytes), 0,
			RENDER_BUFFER_UPDATE_DISCARD) == RENDER_RESULT_OK &&
		resources.DescribeBuffer(buffer, &bufferDescription) == RENDER_RESULT_OK &&
		bufferDescription.authority == NATIVE_W3D_CONTENT_CPU,
		"recovery restores the synchronous pre-frame upload path");
	result |= Check(context->beginFrame() == RENDER_RESULT_OK &&
		context->setLegacyStateForLayout(drawState, drawLayout, 0) ==
			RENDER_RESULT_OK &&
		context->setVertexBuffer(buffer, sizeof(unsigned int), 0) ==
			RENDER_RESULT_OK &&
		context->setPrimitiveTopology(RENDER_PRIMITIVE_TRIANGLE_LIST) ==
			RENDER_RESULT_OK && context->draw(3, 0) == RENDER_RESULT_OK &&
		context->endFrame() == RENDER_RESULT_OK &&
		SubmitThreadedRenderFrame(device, false) == RENDER_RESULT_OK &&
		DrainThreadedRenderDevice(device) == RENDER_RESULT_OK,
		"recovered out-of-frame upload feeds a successful threaded frame");
	ThreadedRenderFrameCompletion recoveredUploadCompletion;
	validated = GpuHandle();
	result |= Check(PollThreadedRenderCompletion(device,
		&recoveredUploadCompletion) &&
		recoveredUploadCompletion.result == RENDER_RESULT_OK &&
		!recoveredUploadCompletion.resourceFailure &&
		resources.PublishThreadedCompletion(
			recoveredUploadCompletion.sequence, false) == RENDER_RESULT_OK &&
		resources.AcquireVertexBufferRange(buffer, sizeof(unsigned int), 0,
			0, 3, &validated) == RENDER_RESULT_OK && validated == buffer,
		"successful recovery clears resource-failure latches and preserves the republished range");

	for (unsigned int failedCreate = 0; failedCreate < 3; ++failedCreate)
	{
		InterlockedExchange(&control.failCreate, 1);
		GpuHandle rejectedBuffer;
		result |= Check(resources.CreateBuffer(bufferDescriptor, bytes,
			sizeof(bytes), &rejectedBuffer) == RENDER_RESULT_FAILED &&
			!rejectedBuffer.isValid() &&
			ReadCount(&control.createCalls) ==
				static_cast<long>(3 + failedCreate),
			"threaded owner-side create failure never publishes a logical handle");
		InterlockedExchange(&control.failCreate, 0);
		result |= Check(device->recoverDevice() == RENDER_RESULT_OK &&
			host.ReplaceContext(device->immediateContext()) == RENDER_RESULT_OK &&
			resources.RestoreStaticBuffersAfterRecovery() == RENDER_RESULT_OK,
			"threaded owner recovers after an asynchronous create failure");
	}
	GpuHandle replacementBuffer;
	result |= Check(resources.CreateBuffer(bufferDescriptor, bytes,
		sizeof(bytes), &replacementBuffer) == RENDER_RESULT_OK &&
		resources.Destroy(replacementBuffer),
		"failed threaded create rolls back its logical slot before reuse");

	unsigned int texturePixels[16] = { 0 };
	TextureDescriptor textureDescriptor;
	textureDescriptor.width = 4;
	textureDescriptor.height = 4;
	textureDescriptor.mipCount = 1;
	textureDescriptor.arrayCount = 1;
	textureDescriptor.dimension = RENDER_TEXTURE_2D;
	textureDescriptor.format = RENDER_FORMAT_R8G8B8A8_UNORM;
	textureDescriptor.binding = RENDER_TEXTURE_SHADER_RESOURCE |
		RENDER_TEXTURE_RENDER_TARGET;
	textureDescriptor.usage = RENDER_USAGE_DEFAULT;
	TextureSubresourceData textureData;
	textureData.data = texturePixels;
	textureData.rowPitch = sizeof(unsigned int) * 4;
	textureData.slicePitch = sizeof(texturePixels);
	GpuHandle texture;
	result |= Check(resources.CreateTexture(textureDescriptor, &textureData, 1,
		&texture) == RENDER_RESULT_OK,
		"threaded texture create completes before publication");

	InterlockedExchange(&control.failRefresh, 1);
	NativeW3DTextureDescription textureDescription;
	result |= Check(resources.RefreshTexture(texture, textureDescriptor,
		&textureData, 1) == RENDER_RESULT_FAILED &&
		ReadCount(&control.refreshCalls) == 1 &&
		resources.DescribeTexture(texture, &textureDescription) ==
		RENDER_RESULT_OK &&
		textureDescription.authority == NATIVE_W3D_CONTENT_INVALID,
		"threaded owner-side refresh failure invalidates optimistic authority");
	InterlockedExchange(&control.failRefresh, 0);
	result |= Check(device->recoverDevice() == RENDER_RESULT_OK &&
		host.ReplaceContext(device->immediateContext()) == RENDER_RESULT_OK &&
		resources.RestoreStaticBuffersAfterRecovery() == RENDER_RESULT_OK,
		"threaded owner recovers after an asynchronous refresh failure");

	InterlockedExchange(&control.failCopy, 1);
	NativeW3DGpuContentLease lease;
	result |= Check(context->beginFrame() == RENDER_RESULT_OK &&
		resources.CopyActiveColorTargetToTexture(texture, &lease) ==
		RENDER_RESULT_FAILED && !lease.isValid() &&
		ReadCount(&control.copyCalls) == 1 &&
		context->endFrame() == RENDER_RESULT_OK &&
		SubmitThreadedRenderFrame(device, false) == RENDER_RESULT_OK,
		"threaded copy failure never publishes a GPU lease");
	const RenderResult failedFrameDrain = DrainThreadedRenderDevice(device);
	ThreadedRenderFrameCompletion completion;
	result |= Check(failedFrameDrain == RENDER_RESULT_FAILED &&
		PollThreadedRenderCompletion(device, &completion) &&
		completion.resourceFailure && completion.result == RENDER_RESULT_FAILED,
		"threaded frame completion reports the owner resource failure");
	InterlockedExchange(&control.failCopy, 0);
	result |= Check(device->recoverDevice() == RENDER_RESULT_OK &&
		host.ReplaceContext(device->immediateContext()) == RENDER_RESULT_OK &&
		resources.RestoreStaticBuffersAfterRecovery() == RENDER_RESULT_OK &&
		resources.Shutdown() == RENDER_RESULT_OK &&
		host.Detach() == RENDER_RESULT_OK,
		"threaded resource shutdown destroys owner handles before host detach");
	device->shutdown();
	delete device;
	return result;
}

int TestThreadedNativeBufferOwnerFailureRecovery()
{
	int result = 0;
	FakeRenderControl control;
	ThreadedRenderOptions options;
	options.serial = false;
	options.maxFramesInFlight = 2;
	options.maxPacketBytes = 1024 * 1024;
	options.maxPacketCommands = 128;
	options.resourceCapacity = 4;
	IRenderDevice *device = CreateThreadedRenderDevice(
		CreateThreadedFakeRenderDevice, &control, options);
	result |= Check(device != 0,
		"threaded native owner fixture allocates its device");
	if (device == 0)
	{
		return result;
	}
	RenderDeviceParameters parameters;
	parameters.backend = RENDER_BACKEND_D3D11;
	parameters.window = reinterpret_cast<void *>(1);
	parameters.width = 4;
	parameters.height = 4;
	result |= Check(device->initialize(parameters) == RENDER_RESULT_OK,
		"threaded native owner fixture initializes its render owner");
	if (!device->isOperational())
	{
		delete device;
		return result;
	}

	NativeW3DResourceHost host(8);
	NativeW3DResources resources(4);
	result |= Check(host.Attach(device, device->immediateContext()) ==
		RENDER_RESULT_OK && resources.BindHost(&host) == RENDER_RESULT_OK &&
		BindNativeW3DBufferResources(&resources) == RENDER_RESULT_OK,
		"threaded native owner fixture binds one resource host");

	BufferDescriptor descriptor;
	descriptor.byteCount = 16;
	descriptor.stride = 4;
	descriptor.binding = RENDER_BUFFER_VERTEX;
	descriptor.usage = RENDER_USAGE_DYNAMIC;
	NativeW3DBufferOwner owner;
	void *bytes = 0;
	result |= Check(owner.Create(descriptor) == RENDER_RESULT_OK,
		"threaded native owner publishes its dynamic buffer");
	IRenderContext *context = device->immediateContext();
	InterlockedExchange(&control.failUpdate, 1);
	unsigned char failedImage[16];
	std::memset(failedImage, 0x51, sizeof(failedImage));
	result |= Check(context->beginFrame() == RENDER_RESULT_OK &&
		owner.Lock(0, sizeof(failedImage), RENDER_BUFFER_UPDATE_DISCARD,
			&bytes) == RENDER_RESULT_OK && bytes != 0,
		"threaded owner accepts an in-frame discard before completion");
	if (bytes != 0)
	{
		std::memcpy(bytes, failedImage, sizeof(failedImage));
	}
	result |= Check(owner.Unlock() == RENDER_RESULT_OK &&
		context->endFrame() == RENDER_RESULT_OK &&
		SubmitThreadedRenderFrame(device, false) == RENDER_RESULT_OK &&
		DrainThreadedRenderDevice(device) == RENDER_RESULT_FAILED,
		"threaded owner exposes an asynchronous upload failure at completion");
	ThreadedRenderFrameCompletion completion;
	result |= Check(PollThreadedRenderCompletion(device, &completion) &&
		completion.resourceFailure &&
		resources.PublishThreadedCompletion(completion.sequence, true) ==
			RENDER_RESULT_OK && owner.HasFailedMutation() &&
		!owner.IsLocked(),
		"failed threaded publication latches the direct owner authority");
	bytes = reinterpret_cast<void *>(1);
	result |= Check(owner.Lock(0, sizeof(failedImage),
		RENDER_BUFFER_UPDATE_PRESERVE, &bytes) == RENDER_RESULT_FAILED &&
		bytes == 0,
		"failed threaded authority rejects stale preserve bytes");

	InterlockedExchange(&control.failUpdate, 0);
	result |= Check(device->recoverDevice() == RENDER_RESULT_OK &&
		host.ReplaceContext(device->immediateContext()) == RENDER_RESULT_OK &&
		resources.RestoreStaticBuffersAfterRecovery() == RENDER_RESULT_OK,
		"threaded native owner fixture restores the backend before discard");
	bytes = 0;
	result |= Check(owner.Lock(0, sizeof(failedImage),
		RENDER_BUFFER_UPDATE_DISCARD, &bytes) == RENDER_RESULT_OK &&
		bytes != 0 && !owner.HasFailedMutation(),
		"explicit discard recreates the failed threaded owner");
	if (bytes != 0)
	{
		std::memset(bytes, 0x61, sizeof(failedImage));
	}
	GpuHandle validated;
	result |= Check(owner.Unlock() == RENDER_RESULT_OK &&
		owner.AcquireVertexRange(4, 0, 0, 4, &validated) ==
			RENDER_RESULT_OK && validated.isValid() &&
		!owner.HasFailedMutation(),
		"discard recovery republishes a valid direct owner range");
	result |= Check(owner.Reset() == RENDER_RESULT_OK &&
		UnbindNativeW3DBufferResources(&resources) == RENDER_RESULT_OK &&
		resources.Shutdown() == RENDER_RESULT_OK &&
		host.Detach() == RENDER_RESULT_OK,
		"threaded native owner fixture releases its recovered allocation");
	device->shutdown();
	delete device;
	return result;
}
}

int main()
{
	int result = 0;
	NativeW3DResources unbound(2);
	GpuHandle invalid;
	BufferDescriptor emptyDescriptor;
	result |= Check(unbound.BindHost(0) ==
		RENDER_RESULT_INVALID_ARGUMENT,
		"resource table refuses a null borrowed-backend host");
	result |= Check(!unbound.IsValid(invalid) && !unbound.Destroy(invalid),
		"default and stale handles are never live resources");
	result |= Check(unbound.CreateBuffer(emptyDescriptor, 0, 0, 0) ==
		RENDER_RESULT_INVALID_ARGUMENT,
		"resource creation validates its output before touching a backend");
	result |= Check(unbound.Shutdown() == RENDER_RESULT_OK,
		"an unbound resource table shuts down deterministically");

	FakeRenderDevice device;
	NativeW3DResourceHost host(8);
	result |= Check(host.Attach(0, device.immediateContext()) ==
		RENDER_RESULT_INVALID_ARGUMENT &&
		host.Attach(&device, device.immediateContext()) == RENDER_RESULT_OK &&
		host.IsAttached(),
		"resource host borrows exactly one initialized backend and context");

	NativeW3DResources resources(16);
	result |= Check(resources.BindHost(&host) == RENDER_RESULT_OK,
		"resource table binds to the borrowed backend lifecycle");
	result |= Check(host.BoundResourceTables() == 1 &&
		host.Detach() == RENDER_RESULT_INVALID_ARGUMENT && host.IsAttached(),
		"host detach rejects a bound resource table before invalidating its generation");

	unsigned int originalBytes[4] = { 1, 2, 3, 4 };
	unsigned int latestBytes[4] = { 5, 6, 7, 8 };
	BufferDescriptor bufferDescriptor;
	bufferDescriptor.byteCount = sizeof(originalBytes);
	bufferDescriptor.stride = sizeof(unsigned int);
	bufferDescriptor.binding = RENDER_BUFFER_VERTEX;
	bufferDescriptor.usage = RENDER_USAGE_DYNAMIC;
	GpuHandle buffer;
	result |= Check(resources.CreateBuffer(bufferDescriptor, originalBytes,
		sizeof(originalBytes), &buffer) == RENDER_RESULT_OK,
		"resource table creates a CPU-authoritative buffer");
	NativeW3DBufferDescription bufferDescription;
	result |= Check(resources.DescribeBuffer(buffer, &bufferDescription) ==
		RENDER_RESULT_OK &&
		bufferDescription.descriptor.byteCount == sizeof(originalBytes) &&
		bufferDescription.authority == NATIVE_W3D_CONTENT_CPU &&
		bufferDescription.authorityEpoch != 0,
		"buffer description exposes descriptor and CPU authority epoch");
	const unsigned int createEpoch = bufferDescription.authorityEpoch;
	result |= Check(resources.UpdateBuffer(buffer, latestBytes,
		sizeof(latestBytes[0]), 0, RENDER_BUFFER_UPDATE_PRESERVE) ==
		RENDER_RESULT_OK &&
		resources.UpdateBuffer(buffer, latestBytes + 1,
			sizeof(latestBytes[1]), sizeof(latestBytes[0]),
			RENDER_BUFFER_UPDATE_NO_OVERWRITE) == RENDER_RESULT_OK &&
		resources.DescribeBuffer(buffer, &bufferDescription) ==
		RENDER_RESULT_OK &&
		bufferDescription.authority == NATIVE_W3D_CONTENT_CPU,
		"partial preserve and no-overwrite updates retain existing whole-buffer authority");
	GpuHandle partiallyInitialized;
	result |= Check(resources.CreateBuffer(bufferDescriptor, 0, 0,
		&partiallyInitialized) == RENDER_RESULT_OK &&
		resources.DescribeBuffer(partiallyInitialized, &bufferDescription) ==
		RENDER_RESULT_OK &&
		bufferDescription.authority == NATIVE_W3D_CONTENT_INVALID,
		"buffer creation without bytes starts with no whole-buffer authority");
	result |= Check(resources.UpdateBuffer(partiallyInitialized, originalBytes,
		sizeof(originalBytes[0]), 0, RENDER_BUFFER_UPDATE_PRESERVE) ==
		RENDER_RESULT_OK &&
		resources.DescribeBuffer(partiallyInitialized, &bufferDescription) ==
		RENDER_RESULT_OK &&
		bufferDescription.authority == NATIVE_W3D_CONTENT_INVALID,
		"a partial preserve write does not grant whole-buffer CPU authority");
	result |= Check(resources.UpdateBuffer(partiallyInitialized,
		originalBytes + 1, sizeof(originalBytes) - sizeof(originalBytes[0]),
		sizeof(originalBytes[0]), RENDER_BUFFER_UPDATE_NO_OVERWRITE) ==
		RENDER_RESULT_OK &&
		resources.DescribeBuffer(partiallyInitialized, &bufferDescription) ==
		RENDER_RESULT_OK &&
		bufferDescription.authority == NATIVE_W3D_CONTENT_INVALID,
		"disjoint partial ranges remain range-valid without granting whole-buffer authority");
	result |= Check(resources.UpdateBuffer(partiallyInitialized, latestBytes,
		sizeof(latestBytes[0]), 0, RENDER_BUFFER_UPDATE_DISCARD) ==
		RENDER_RESULT_OK &&
		resources.DescribeBuffer(partiallyInitialized, &bufferDescription) ==
		RENDER_RESULT_OK &&
		bufferDescription.authority == NATIVE_W3D_CONTENT_INVALID,
		"partial DISCARD invalidates the untouched remainder without whole authority");
	GpuHandle validatedRange = buffer;
	result |= Check(resources.AcquireVertexBufferRange(partiallyInitialized,
		sizeof(unsigned int), 0, 0, 1, &validatedRange) == RENDER_RESULT_OK &&
		validatedRange == partiallyInitialized,
		"partial DISCARD publishes its exact initialized vertex range");
	validatedRange = buffer;
	result |= Check(resources.AcquireVertexBufferRange(partiallyInitialized,
		sizeof(unsigned int), 0, 1, 1, &validatedRange) ==
		RENDER_RESULT_INVALID_ARGUMENT && !validatedRange.isValid() &&
		resources.Destroy(partiallyInitialized),
		"exact range acquisition rejects and clears adjacent unwritten bytes");
	bufferDescriptor.binding = RENDER_BUFFER_INDEX;
	GpuHandle partiallyInitializedIndices;
	result |= Check(resources.CreateBuffer(bufferDescriptor, 0, 0,
		&partiallyInitializedIndices) == RENDER_RESULT_OK &&
		resources.UpdateBuffer(partiallyInitializedIndices, latestBytes,
			2 * sizeof(unsigned int), 0, RENDER_BUFFER_UPDATE_DISCARD) ==
			RENDER_RESULT_OK,
		"partial index DISCARD publishes owner bytes without whole authority");
	validatedRange = buffer;
	result |= Check(resources.AcquireIndexBufferRange(partiallyInitializedIndices,
		RENDER_FORMAT_R32_UINT, 0, 0, 2, &validatedRange) == RENDER_RESULT_OK &&
		validatedRange == partiallyInitializedIndices,
		"exact index acquisition accepts the initialized prefix");
	validatedRange = buffer;
	result |= Check(resources.AcquireIndexBufferRange(partiallyInitializedIndices,
		RENDER_FORMAT_R32_UINT, 0, 2, 1, &validatedRange) ==
		RENDER_RESULT_INVALID_ARGUMENT && !validatedRange.isValid() &&
		resources.Destroy(partiallyInitializedIndices),
		"exact index acquisition rejects and clears the adjacent unwritten range");
	bufferDescriptor.binding = RENDER_BUFFER_VERTEX;

	WrongOwnerUpdate wrongOwner;
	wrongOwner.resources = &resources;
	wrongOwner.handle = buffer;
	wrongOwner.value = 99;
	wrongOwner.result = RENDER_RESULT_OK;
	HANDLE wrongOwnerThread = CreateThread(0, 0, UpdateFromWrongOwner,
		&wrongOwner, 0, 0);
	result |= Check(wrongOwnerThread != 0,
		"wrong-owner resource update worker starts");
	if (wrongOwnerThread != 0)
	{
		WaitForSingleObject(wrongOwnerThread, INFINITE);
		CloseHandle(wrongOwnerThread);
		result |= Check(wrongOwner.result == RENDER_RESULT_INVALID_ARGUMENT,
			"resource mutation rejects the wrong owner thread");
	}

	result |= Check(resources.UpdateBuffer(buffer, latestBytes,
		sizeof(latestBytes), sizeof(originalBytes),
		RENDER_BUFFER_UPDATE_PRESERVE) == RENDER_RESULT_INVALID_ARGUMENT,
		"buffer update rejects an out-of-range destination");
	result |= Check(resources.UpdateBuffer(buffer, latestBytes,
		sizeof(latestBytes), 0, RENDER_BUFFER_UPDATE_DISCARD) ==
		RENDER_RESULT_OK &&
		resources.DescribeBuffer(buffer, &bufferDescription) ==
		RENDER_RESULT_OK && bufferDescription.authorityEpoch > createEpoch,
		"accepted buffer update advances CPU authority");
	BufferDescriptor staticBufferDescriptor = bufferDescriptor;
	staticBufferDescriptor.usage = RENDER_USAGE_DEFAULT;
	GpuHandle persistentStaticBuffer;
	result |= Check(resources.CreateBuffer(staticBufferDescriptor, latestBytes,
		sizeof(latestBytes), &persistentStaticBuffer) == RENDER_RESULT_OK &&
		resources.DescribeBuffer(persistentStaticBuffer, &bufferDescription) ==
			RENDER_RESULT_OK,
		"persistent static geometry records one authoritative CPU image");
	const unsigned int persistentStaticEpoch = bufferDescription.authorityEpoch;
	result |= Check(device.resize(800, 600) == RENDER_RESULT_OK &&
		host.ReplaceContext(device.immediateContext()) == RENDER_RESULT_OK &&
		resources.RepublishStaticBuffersAfterResize() == RENDER_RESULT_OK &&
		device.BufferEquals(persistentStaticBuffer, latestBytes,
			sizeof(latestBytes)) &&
		resources.DescribeBuffer(persistentStaticBuffer, &bufferDescription) ==
			RENDER_RESULT_OK &&
		bufferDescription.authority == NATIVE_W3D_CONTENT_CPU &&
		bufferDescription.authorityEpoch == persistentStaticEpoch &&
		resources.AcquireVertexBufferRange(persistentStaticBuffer,
			sizeof(unsigned int), 0, 0, 4, &validatedRange) ==
			RENDER_RESULT_OK && validatedRange == persistentStaticBuffer,
		"ordinary resize republishes static geometry without advancing its invalidation epoch");
	result |= Check(device.recoverDevice() == RENDER_RESULT_OK &&
		host.ReplaceContext(device.immediateContext()) == RENDER_RESULT_OK &&
		resources.RestoreStaticBuffersAfterRecovery() == RENDER_RESULT_OK &&
		device.BufferEquals(persistentStaticBuffer, latestBytes,
			sizeof(latestBytes)) &&
		resources.DescribeBuffer(persistentStaticBuffer, &bufferDescription) ==
			RENDER_RESULT_OK &&
		bufferDescription.authority == NATIVE_W3D_CONTENT_CPU &&
		bufferDescription.authorityEpoch == persistentStaticEpoch &&
		resources.AcquireVertexBufferRange(persistentStaticBuffer,
			sizeof(unsigned int), 0, 0, 4, &validatedRange) ==
			RENDER_RESULT_OK && validatedRange == persistentStaticBuffer &&
		!device.BufferEquals(buffer, latestBytes, sizeof(latestBytes)) &&
		resources.DescribeBuffer(buffer, &bufferDescription) ==
		RENDER_RESULT_OK &&
		bufferDescription.authority == NATIVE_W3D_CONTENT_INVALID &&
		resources.AcquireVertexBufferRange(buffer, sizeof(unsigned int), 0,
			0, 1, &validatedRange) == RENDER_RESULT_INVALID_ARGUMENT &&
		!validatedRange.isValid() &&
		resources.UpdateBuffer(buffer, latestBytes, sizeof(latestBytes), 0,
			RENDER_BUFFER_UPDATE_DISCARD) == RENDER_RESULT_OK,
		"device recovery restores static geometry and invalidates unrestorable dynamic bytes");
	result |= Check(resources.Destroy(persistentStaticBuffer),
		"persistent static recovery fixture releases its exact resource");
	result |= Check(resources.PublishThreadedCompletion(1, false) ==
		RENDER_RESULT_OK &&
		resources.DescribeBuffer(buffer, &bufferDescription) ==
		RENDER_RESULT_OK &&
		bufferDescription.authority == NATIVE_W3D_CONTENT_CPU &&
		resources.PublishThreadedCompletion(1, false) ==
		RENDER_RESULT_INVALID_ARGUMENT,
		"successful ordered completion preserves authority and duplicate publication is rejected");
	result |= Check(resources.PublishThreadedCompletion(2, true) ==
		RENDER_RESULT_OK &&
		resources.DescribeBuffer(buffer, &bufferDescription) ==
		RENDER_RESULT_OK &&
		bufferDescription.authority == NATIVE_W3D_CONTENT_CPU &&
		resources.AcquireVertexBufferRange(buffer, sizeof(unsigned int), 0,
			0, 1, &validatedRange) == RENDER_RESULT_OK &&
		validatedRange == buffer,
		"aggregate threaded failure preserves synchronously fenced buffer authority");

	unsigned int texturePixels[16] = { 0 };
	TextureDescriptor textureDescriptor;
	textureDescriptor.width = 4;
	textureDescriptor.height = 4;
	textureDescriptor.mipCount = 1;
	textureDescriptor.arrayCount = 1;
	textureDescriptor.dimension = RENDER_TEXTURE_2D;
	textureDescriptor.format = RENDER_FORMAT_R8G8B8A8_UNORM;
	textureDescriptor.binding = RENDER_TEXTURE_SHADER_RESOURCE |
		RENDER_TEXTURE_RENDER_TARGET;
	textureDescriptor.usage = RENDER_USAGE_DEFAULT;
	TextureSubresourceData textureData;
	textureData.data = texturePixels;
	textureData.rowPitch = sizeof(unsigned int) * 4;
	textureData.slicePitch = sizeof(texturePixels);
	GpuHandle texture;
	result |= Check(resources.CreateTexture(textureDescriptor, &textureData, 1,
		&texture) == RENDER_RESULT_OK,
		"resource table creates a CPU-authoritative refreshable texture");
	NativeW3DTextureDescription textureDescription;
	result |= Check(resources.DescribeTexture(texture, &textureDescription) ==
		RENDER_RESULT_OK &&
		textureDescription.authority == NATIVE_W3D_CONTENT_CPU,
		"texture description exposes its initial CPU authority");
	const unsigned int textureCreateEpoch = textureDescription.authorityEpoch;
	result |= Check(
		resources.DescribeBuffer(texture, &bufferDescription) ==
		RENDER_RESULT_INVALID_ARGUMENT &&
		resources.DescribeTexture(buffer, &textureDescription) ==
		RENDER_RESULT_INVALID_ARGUMENT,
		"descriptions reject the wrong resource kind");
	NativeW3DGpuContentLease lease;
	result |= Check(resources.UpdateBuffer(texture, latestBytes,
		sizeof(latestBytes), 0, RENDER_BUFFER_UPDATE_PRESERVE) ==
		RENDER_RESULT_INVALID_ARGUMENT &&
		resources.AcquireGpuContentLease(buffer, &lease) ==
		RENDER_RESULT_INVALID_ARGUMENT &&
		resources.AcquireGpuContentLease(texture, &lease) ==
		RENDER_RESULT_INVALID_ARGUMENT,
		"updates and GPU leases reject wrong-kind or CPU authority");

	const unsigned int refreshes = device.RefreshCount();
	result |= Check(resources.RefreshTexture(texture, textureDescriptor,
		&textureData, 0) == RENDER_RESULT_INVALID_ARGUMENT &&
		device.RefreshCount() == refreshes,
		"texture refresh rejects a wrong subresource count before the backend");
	TextureDescriptor incompatibleTexture = textureDescriptor;
	incompatibleTexture.width = 8;
	result |= Check(resources.RefreshTexture(texture, incompatibleTexture,
		&textureData, 1) == RENDER_RESULT_UNSUPPORTED,
		"texture refresh rejects an incompatible descriptor");
	const GpuHandle stableTexture = texture;
	result |= Check(resources.RefreshTexture(texture, textureDescriptor,
		&textureData, 1) == RENDER_RESULT_OK && texture == stableTexture &&
		resources.DescribeTexture(texture, &textureDescription) ==
		RENDER_RESULT_OK && textureDescription.authorityEpoch >
		textureCreateEpoch,
		"compatible refresh preserves the handle and advances CPU authority");

	FakeRenderContext *context =
		static_cast<FakeRenderContext *>(device.immediateContext());
	result |= Check(context->beginFrame() == RENDER_RESULT_OK &&
		resources.CopyActiveColorTargetToTexture(buffer, &lease) ==
		RENDER_RESULT_INVALID_ARGUMENT &&
		resources.CopyActiveColorTargetToTexture(texture, &lease) ==
		RENDER_RESULT_OK && lease.isValid() && lease.resource == texture,
		"active-target copy rejects buffers and publishes a GPU lease");
	NativeW3DGpuContentLease acquiredLease;
	result |= Check(resources.AcquireGpuContentLease(texture, &acquiredLease) ==
		RENDER_RESULT_OK && acquiredLease.resource == lease.resource &&
		acquiredLease.authorityEpoch == lease.authorityEpoch &&
		context->endFrame() == RENDER_RESULT_OK,
		"GPU-authoritative texture content can be acquired in its epoch");
	const unsigned int firstAttachmentGeneration =
		acquiredLease.attachmentGeneration;
	NativeW3DGpuContentLease staleLease = acquiredLease;
	result |= Check(context->beginFrame() == RENDER_RESULT_OK &&
		resources.CopyActiveColorTargetToTexture(texture, &lease) ==
		RENDER_RESULT_OK && context->endFrame() == RENDER_RESULT_OK &&
		resources.AcquireGpuContentLease(texture, &staleLease) ==
		RENDER_RESULT_INVALID_ARGUMENT && !staleLease.isValid(),
		"a later GPU write rejects a stale authority-epoch lease");
	result |= Check(resources.RefreshTexture(texture, textureDescriptor,
		&textureData, 1) == RENDER_RESULT_OK &&
		resources.AcquireGpuContentLease(texture, &acquiredLease) ==
		RENDER_RESULT_INVALID_ARGUMENT,
		"a later CPU refresh invalidates GPU authority");
	result |= Check(context->beginFrame() == RENDER_RESULT_OK &&
		resources.CopyActiveColorTargetToTexture(texture, &lease) ==
		RENDER_RESULT_OK && context->endFrame() == RENDER_RESULT_OK &&
		device.recoverDevice() == RENDER_RESULT_OK &&
		host.ReplaceContext(device.immediateContext()) == RENDER_RESULT_OK &&
		resources.RestoreStaticBuffersAfterRecovery() == RENDER_RESULT_OK &&
		resources.DescribeTexture(texture, &textureDescription) ==
		RENDER_RESULT_OK &&
		textureDescription.authority == NATIVE_W3D_CONTENT_INVALID &&
		resources.AcquireGpuContentLease(texture, &acquiredLease) ==
		RENDER_RESULT_INVALID_ARGUMENT,
		"context recovery invalidates GPU-only render-target authority");

	const GpuHandle stale = buffer;
	result |= Check(resources.Destroy(buffer) && !resources.IsValid(stale) &&
		resources.UpdateBuffer(stale, latestBytes, sizeof(latestBytes), 0,
			RENDER_BUFFER_UPDATE_PRESERVE) == RENDER_RESULT_INVALID_ARGUMENT,
		"destroyed handles are stale for every table operation");
	GpuHandle recreated;
	result |= Check(resources.CreateBuffer(bufferDescriptor, originalBytes,
		sizeof(originalBytes), &recreated) == RENDER_RESULT_OK &&
		recreated != stale,
		"destroy and recreate advances the backend handle generation");

	NativeW3DResources *deferred = new NativeW3DResources(16);
	result |= Check(deferred != 0 && deferred->BindHost(&host) == RENDER_RESULT_OK &&
		host.BoundResourceTables() == 2,
		"detached owner table binds to the same borrowed backend");
	GpuHandle deferredBuffer;
	if (deferred != 0)
	{
		result |= Check(deferred->CreateBuffer(bufferDescriptor, originalBytes,
			sizeof(originalBytes), &deferredBuffer) == RENDER_RESULT_OK,
			"deferred-destruction fixture creates a live buffer");
		const unsigned int destroyBefore = device.DestroyCount();
		WorkerDestroy request;
		request.resources = deferred;
		HANDLE destroyThread = CreateThread(0, 0, DestroyFromWorker, &request,
			0, 0);
		result |= Check(destroyThread != 0,
			"worker-side resource-table destruction starts");
		if (destroyThread != 0)
		{
			WaitForSingleObject(destroyThread, INFINITE);
			CloseHandle(destroyThread);
			result |= Check(request.resources == 0 &&
				host.PendingCleanup() == 1 &&
				host.BoundResourceTables() == 2 &&
				device.DestroyCount() == destroyBefore,
				"worker destruction defers backend release to the owner");
			unsigned int drained = 0;
			result |= Check(host.DrainCleanup(0, &drained) == RENDER_RESULT_OK &&
				drained == 1 && host.PendingCleanup() == 0 &&
				host.BoundResourceTables() == 1 &&
				device.DestroyCount() == destroyBefore + 1,
				"borrowed host drains deferred destruction on its owner");
		}
	}

	const GpuHandle beforeDetach = recreated;
	result |= Check(resources.Shutdown() == RENDER_RESULT_OK &&
		host.BoundResourceTables() == 0 &&
		host.Detach() == RENDER_RESULT_OK && !host.IsAttached() &&
		!resources.IsValid(beforeDetach),
		"owner shutdown invalidates the old attachment generation before detach");
	result |= Check(host.Attach(&device, device.immediateContext()) ==
		RENDER_RESULT_OK && resources.BindHost(&host) == RENDER_RESULT_OK,
		"borrowed host can bind a fresh attachment generation");
	GpuHandle rebound;
	result |= Check(resources.CreateBuffer(bufferDescriptor, latestBytes,
		sizeof(latestBytes), &rebound) == RENDER_RESULT_OK &&
		rebound != beforeDetach && !resources.IsValid(beforeDetach),
		"rebind keeps handles from the detached generation stale");
	GpuHandle reboundTexture;
	NativeW3DGpuContentLease reboundLease;
	result |= Check(resources.CreateTexture(textureDescriptor, &textureData, 1,
		&reboundTexture) == RENDER_RESULT_OK &&
		context->beginFrame() == RENDER_RESULT_OK &&
		resources.CopyActiveColorTargetToTexture(reboundTexture, &reboundLease) ==
		RENDER_RESULT_OK && context->endFrame() == RENDER_RESULT_OK &&
		reboundLease.attachmentGeneration > firstAttachmentGeneration,
		"reattach advances the host attachment generation monotonically");
	result |= Check(resources.Shutdown() == RENDER_RESULT_OK &&
		host.Detach() == RENDER_RESULT_OK && device.LiveCount() == 0,
		"resource owner destroys every allocation before borrowed backend detach");
	FakeRenderDevice differentDevice;
	result |= Check(host.Attach(&differentDevice,
		differentDevice.immediateContext()) == RENDER_RESULT_INVALID_ARGUMENT,
		"one resource host cannot be rebound to a second backend identity");
	FakeRenderDevice bridgeDeviceA;
	FakeRenderDevice bridgeDeviceB;
	NativeW3D2 *bridgeNative = new (std::nothrow) NativeW3D2;
	result |= Check(bridgeNative != 0 && bridgeNative->AttachBackend(
		&bridgeDeviceA, bridgeDeviceA.immediateContext()) == RENDER_RESULT_OK &&
		bridgeNative->Shutdown() == RENDER_RESULT_OK &&
		bridgeDeviceA.isOperational(),
		"first bridge-native host detaches without shutting down its caller-owned device");
	delete bridgeNative;
	bridgeNative = new (std::nothrow) NativeW3D2;
	result |= Check(bridgeNative != 0 && bridgeNative->AttachBackend(
		&bridgeDeviceB, bridgeDeviceB.immediateContext()) == RENDER_RESULT_OK &&
		bridgeNative->Shutdown() == RENDER_RESULT_OK &&
		bridgeDeviceA.isOperational() && bridgeDeviceB.isOperational(),
		"fresh bridge-native allocation accepts a distinct second device lifetime");
	delete bridgeNative;

	FakeRenderDevice saturatedDevice;
	NativeW3DResourceHost saturatedHost(1);
	result |= Check(saturatedHost.Attach(&saturatedDevice,
		saturatedDevice.immediateContext()) == RENDER_RESULT_OK,
		"bounded cleanup fixture attaches");
	NativeW3DResources *deferredTables[2] = {
		new NativeW3DResources(2), new NativeW3DResources(2)
	};
	for (unsigned int tableIndex = 0; tableIndex < 2; ++tableIndex)
	{
		GpuHandle deferredHandle;
		result |= Check(deferredTables[tableIndex] != 0 &&
			deferredTables[tableIndex]->BindHost(&saturatedHost) ==
				RENDER_RESULT_OK &&
			deferredTables[tableIndex]->CreateBuffer(bufferDescriptor,
				originalBytes, sizeof(originalBytes), &deferredHandle) ==
				RENDER_RESULT_OK,
			"bounded cleanup table creates a live owner resource");
		if (deferredTables[tableIndex] != 0)
		{
			WorkerDestroy request;
			request.resources = deferredTables[tableIndex];
			HANDLE destroyThread = CreateThread(0, 0, DestroyFromWorker,
				&request, 0, 0);
			result |= Check(destroyThread != 0,
				"bounded cleanup worker starts");
			if (destroyThread != 0)
			{
				WaitForSingleObject(destroyThread, INFINITE);
				CloseHandle(destroyThread);
				deferredTables[tableIndex] = request.resources;
			}
		}
	}
	result |= Check(saturatedHost.PendingCleanup() == 2 &&
		saturatedHost.BoundResourceTables() == 2 &&
		saturatedDevice.LiveCount() == 2 &&
		saturatedHost.Detach() == RENDER_RESULT_INVALID_ARGUMENT,
		"terminal cleanup survives nominal queue saturation and blocks detach");
	unsigned int saturatedDrained = 0;
	saturatedDevice.FailDestroy(true);
	result |= Check(saturatedHost.DrainCleanup(0, &saturatedDrained) ==
		RENDER_RESULT_FAILED && saturatedDrained == 0 &&
		saturatedHost.PendingCleanup() == 2 &&
		saturatedHost.BoundResourceTables() == 2 &&
		saturatedDevice.LiveCount() == 2 &&
		saturatedHost.Detach() == RENDER_RESULT_INVALID_ARGUMENT,
		"failed owner cleanup remains registered and retryable");
	saturatedDevice.FailDestroy(false);
	result |= Check(saturatedHost.DrainCleanup(0, &saturatedDrained) ==
		RENDER_RESULT_OK && saturatedDrained == 2 &&
		saturatedHost.PendingCleanup() == 0 &&
		saturatedHost.BoundResourceTables() == 0 &&
		saturatedDevice.LiveCount() == 0 &&
		saturatedHost.Detach() == RENDER_RESULT_OK,
		"allocation-free owner cleanup drains every saturated table and handle");

	NativeW3D2 productResources;
	result |= Check(productResources.AttachBackend(&device,
		device.immediateContext()) == RENDER_RESULT_OK &&
		productResources.IsAttachedToBorrowedBackend(),
		"native WW3D product seam attaches without allocating another device");
	NativeW3DRendererDescriptor rejectedDescriptor;
	rejectedDescriptor.width = 4;
	rejectedDescriptor.height = 4;
	rejectedDescriptor.width = 4;
	rejectedDescriptor.height = 4;
	result |= Check(productResources.Renderer().Initialize(
		reinterpret_cast<void *>(1), rejectedDescriptor) ==
		RENDER_RESULT_INVALID_ARGUMENT &&
		productResources.Renderer().Shutdown() == RENDER_RESULT_OK &&
		!productResources.Renderer().IsInitialized() &&
		productResources.Renderer().Initialize(reinterpret_cast<void *>(1),
			rejectedDescriptor) == RENDER_RESULT_INVALID_ARGUMENT &&
		productResources.IsAttachedToBorrowedBackend() && device.isOperational() &&
		productResources.Shutdown() == RENDER_RESULT_OK &&
		!productResources.IsAttachedToBorrowedBackend() && device.isOperational() &&
		productResources.AttachBackend(&device, device.immediateContext()) ==
			RENDER_RESULT_OK,
		"borrowed renderer shutdown releases only its state reference and permits safe product reattach");
	BufferDescriptor productStaticDescriptor = bufferDescriptor;
	productStaticDescriptor.usage = RENDER_USAGE_DEFAULT;
	BufferDescriptor productImmutableDescriptor = bufferDescriptor;
	productImmutableDescriptor.usage = RENDER_USAGE_IMMUTABLE;
	GpuHandle productBuffer;
	GpuHandle productImmutableBuffer;
	GpuHandle productValidated;
	result |= Check(productResources.Resources().CreateBuffer(
		productStaticDescriptor,
		latestBytes, sizeof(latestBytes), &productBuffer) == RENDER_RESULT_OK &&
		productResources.Resources().CreateBuffer(productImmutableDescriptor,
			latestBytes, sizeof(latestBytes), &productImmutableBuffer) ==
			RENDER_RESULT_OK &&
		device.recoverDevice() == RENDER_RESULT_OK &&
		productResources.ReplaceBackendContext(device.immediateContext()) ==
		RENDER_RESULT_OK && productResources.Resources().
			RestoreStaticBuffersAfterRecovery() == RENDER_RESULT_OK &&
		device.BufferEquals(productBuffer, latestBytes,
			sizeof(latestBytes)) &&
		device.BufferEquals(productImmutableBuffer, latestBytes,
			sizeof(latestBytes)) &&
		productResources.Resources().AcquireVertexBufferRange(
			productImmutableBuffer, sizeof(unsigned int), 0, 0, 4,
			&productValidated) == RENDER_RESULT_OK &&
		productValidated == productImmutableBuffer,
		"product seam republishes DEFAULT and immutable bytes after recovery");
	GpuHandle partialProductBuffer;
	result |= Check(productResources.Resources().CreateBuffer(bufferDescriptor,
		0, 0, &partialProductBuffer) == RENDER_RESULT_OK &&
		productResources.Resources().UpdateBuffer(partialProductBuffer,
			latestBytes, sizeof(latestBytes[0]), 0,
			RENDER_BUFFER_UPDATE_PRESERVE) == RENDER_RESULT_OK,
		"product seam records a partial buffer write without whole authority");
	NativeDrawPacket partialPacket;
	partialPacket.vertexBuffer = partialProductBuffer;
	partialPacket.vertexStride = sizeof(unsigned int);
	partialPacket.vertexLayout.stride = sizeof(unsigned int);
	partialPacket.vertexCount = 1;
	LegacyLogicalState partialState;
	const unsigned int liveBeforeOpenShutdown = device.LiveCount();
	result |= Check(productResources.Renderer().BeginFrame() ==
		RENDER_RESULT_OK &&
		productResources.Renderer().Submit(productResources.Resources(),
			partialState, partialPacket) == RENDER_RESULT_OK &&
		productResources.Renderer().Shutdown() ==
			RENDER_RESULT_INVALID_ARGUMENT &&
		productResources.Shutdown() == RENDER_RESULT_INVALID_ARGUMENT &&
		productResources.IsAttachedToBorrowedBackend() &&
		productResources.Resources().IsValid(productBuffer) &&
		productResources.Resources().IsValid(partialProductBuffer) &&
		device.LiveCount() == liveBeforeOpenShutdown &&
		productResources.Renderer().EndFrame(false) == RENDER_RESULT_OK,
		"public and product borrowed shutdown reject an open frame without detaching state");
	result |= Check(productResources.Renderer().Shutdown() == RENDER_RESULT_OK &&
		productResources.Shutdown() == RENDER_RESULT_OK &&
		device.LiveCount() == 0 && device.isOperational(),
		"public borrowed shutdown succeeds after EndFrame and product cleanup preserves backend ownership");
	result |= TestThreadedResourceCompletion();
	result |= TestThreadedNativeBufferOwnerFailureRecovery();
	return result;
}
