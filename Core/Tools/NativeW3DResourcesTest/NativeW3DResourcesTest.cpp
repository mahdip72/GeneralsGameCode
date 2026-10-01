#include "Utility/CppMacros.h"
#include "Renderer/NativeW3DResources.h"
#include "Renderer/ThreadedRenderDevice.h"
#include "nativew3dbufferowner.h"
#include "nativew3d2.h"

#include <climits>
#include <chrono>
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
	FakeRenderControl() : failCreate(0), failUpdate(0), failUpdateOnCall(0),
		failUpdateResult(RENDER_RESULT_FAILED),
		failRefresh(0),
		failRefreshOnCall(0),
		failCopy(0), createCalls(0), updateCalls(0), refreshCalls(0),
		copyCalls(0), refreshPixelSequence(0) {}
	volatile long failCreate;
	volatile long failUpdate;
	volatile long failUpdateOnCall;
	volatile long failUpdateResult;
	volatile long failRefresh;
	volatile long failRefreshOnCall;
	volatile long failCopy;
	volatile long createCalls;
	volatile long updateCalls;
	volatile long refreshCalls;
	volatile long copyCalls;
	volatile long refreshPixelSequence;
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
			const long invocation =
				InterlockedIncrement(&m_control->refreshCalls);
			if (IsSet(&m_control->failRefresh) ||
				invocation == ReadCount(&m_control->failRefreshOnCall))
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
		if (m_control != 0 && dataCount != 0 && data[0].data != 0)
		{
			const long pixel = *static_cast<const unsigned char *>(data[0].data);
			const long previous = ReadCount(&m_control->refreshPixelSequence);
			InterlockedExchange(&m_control->refreshPixelSequence,
				previous * 257 + pixel);
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
		if ((resource->textureDescriptor.binding &
			RENDER_TEXTURE_SHADER_RESOURCE) == 0 ||
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
			const long invocation =
				InterlockedIncrement(&m_control->updateCalls);
			if (IsSet(&m_control->failUpdate) ||
				invocation == ReadCount(&m_control->failUpdateOnCall))
			{
				return static_cast<RenderResult>(
					ReadCount(&m_control->failUpdateResult));
			}
		}
		FakeResource *resource = Find(handle);
		if (resource == 0 || resource->texture || data == 0 || byteCount == 0 ||
			destinationOffset > resource->buffer.byteCount ||
			byteCount > resource->buffer.byteCount - destinationOffset)
		{
			return RENDER_RESULT_INVALID_ARGUMENT;
		}
		// Match native/threaded pre-mutation admission rather than allowing the
		// fake to hide a resource-table request that both actual backends reject.
		if (resource->buffer.usage == RENDER_USAGE_IMMUTABLE)
		{
			return RENDER_RESULT_UNSUPPORTED;
		}
		if ((mode != RENDER_BUFFER_UPDATE_PRESERVE &&
			 mode != RENDER_BUFFER_UPDATE_DISCARD &&
			 mode != RENDER_BUFFER_UPDATE_NO_OVERWRITE) ||
			(mode != RENDER_BUFFER_UPDATE_PRESERVE &&
			 (resource->buffer.usage != RENDER_USAGE_DYNAMIC ||
			  (resource->buffer.binding != RENDER_BUFFER_VERTEX &&
			   resource->buffer.binding != RENDER_BUFFER_INDEX) ||
			  (mode == RENDER_BUFFER_UPDATE_DISCARD && destinationOffset != 0))))
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

int TestResourceLookupHints()
{
	int result = 0;
	FakeRenderDevice device;
	NativeW3DResourceHost host(8);
	NativeW3DResources resources(8);
	if (host.Attach(&device, device.immediateContext()) != RENDER_RESULT_OK ||
		resources.BindHost(&host) != RENDER_RESULT_OK)
	{
		return Check(false, "lookup fixture attaches a resource table");
	}

	const unsigned int value = 7;
	BufferDescriptor descriptor;
	descriptor.byteCount = sizeof(value);
	descriptor.stride = sizeof(value);
	descriptor.binding = RENDER_BUFFER_VERTEX;
	descriptor.usage = RENDER_USAGE_DYNAMIC;
	GpuHandle aligned;
	const bool alignedCreated = resources.CreateBuffer(descriptor, &value,
		sizeof(value), &aligned) == RENDER_RESULT_OK;
	result |= Check(alignedCreated && aligned.index() == 0,
		"resource and backend first-free indices align initially");
	if (alignedCreated)
	{
		const NativeW3DResources &readOnly = resources;
		for (unsigned int repeat = 0; repeat < 8; ++repeat)
			result |= Check(readOnly.IsValid(aligned),
				"aligned lookup preserves resource identity");
		const GpuHandle stale(aligned.index(), aligned.generation() + 1);
		result |= Check(!readOnly.IsValid(stale),
			"lookup hint rejects a different handle generation");
		result |= Check(resources.Destroy(aligned),
			"aligned lookup still destroys the exact resource");
	}

	GpuHandle external;
	const bool externalCreated = device.createBuffer(descriptor, &value,
		sizeof(value), &external) == RENDER_RESULT_OK;
	GpuHandle misaligned;
	const bool misalignedCreated = externalCreated &&
		resources.CreateBuffer(descriptor, &value, sizeof(value),
			&misaligned) == RENDER_RESULT_OK;
	result |= Check(misalignedCreated && external.index() == 0 &&
		misaligned.index() == 1,
		"out-of-band allocation makes handle index differ from table slot");
	if (misalignedCreated)
	{
		const NativeW3DResources &readOnly = resources;
		for (unsigned int repeat = 0; repeat < 8; ++repeat)
			result |= Check(readOnly.IsValid(misaligned),
				"fallback and repeated cache hits retain resource identity");
		result |= Check(!readOnly.IsValid(external),
			"out-of-band backend handle is not a table resource");
		result |= Check(resources.Destroy(misaligned),
			"cached fallback lookup destroys the exact resource");
	}
	if (externalCreated)
		result |= Check(device.destroyResource(external),
			"fixture removes only its out-of-band backend allocation");
	GpuHandle externalReuse;
	const bool externalRecreated = device.createBuffer(descriptor, &value,
		sizeof(value), &externalReuse) == RENDER_RESULT_OK;
	GpuHandle reused;
	const bool reusedCreated = externalRecreated &&
		resources.CreateBuffer(descriptor, &value, sizeof(value),
			&reused) == RENDER_RESULT_OK;
	result |= Check(reusedCreated && reused.index() == misaligned.index() &&
		reused.generation() != misaligned.generation(),
		"backend slot reuse advances the generation without moving table slot");
	if (reusedCreated)
	{
		result |= Check(resources.IsValid(reused),
			"stale cached generation falls back to the newly published resource");
		result |= Check(resources.Destroy(reused),
			"reused generation remains destructible");
	}
	if (externalRecreated)
		result |= Check(device.destroyResource(externalReuse),
			"fixture removes the reused out-of-band allocation");
	result |= Check(resources.Shutdown() == RENDER_RESULT_OK &&
		host.Detach() == RENDER_RESULT_OK && device.LiveCount() == 0,
		"lookup fixture closes without retained resources");
	return result;
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
	options.resourceCapacity = 3;
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
	NativeW3DResources resources(3);
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
		resources.DescribeBuffer(buffer, &bufferDescription) == RENDER_RESULT_OK &&
		bufferDescription.authority == NATIVE_W3D_CONTENT_CPU &&
		resources.AcquireVertexBufferRange(buffer, sizeof(unsigned int), 0,
			0, 4, &validated) == RENDER_RESULT_OK && validated == buffer,
		"matching completion publishes the exact accepted in-frame range");
	const unsigned int bufferEpochBeforeQueuedFailure =
		bufferDescription.authorityEpoch;
	const long updateCallsBeforeQueuedFailure = ReadCount(&control.updateCalls);
	InterlockedExchange(&control.failUpdateOnCall,
		updateCallsBeforeQueuedFailure + 1);
	result |= Check(context->beginFrame() == RENDER_RESULT_OK &&
		resources.UpdateBuffer(buffer, bytes, sizeof(bytes) / 2, 0,
			RENDER_BUFFER_UPDATE_DISCARD) == RENDER_RESULT_OK &&
		context->endFrame() == RENDER_RESULT_OK &&
		SubmitThreadedRenderFrame(device, false) == RENDER_RESULT_OK &&
		context->beginFrame() == RENDER_RESULT_OK &&
		resources.UpdateBuffer(buffer, bytes, sizeof(bytes), 0,
			RENDER_BUFFER_UPDATE_DISCARD) == RENDER_RESULT_OK &&
		context->endFrame() == RENDER_RESULT_OK &&
		SubmitThreadedRenderFrame(device, false) == RENDER_RESULT_OK &&
		DrainThreadedRenderDevice(device) == RENDER_RESULT_FAILED &&
		ReadCount(&control.updateCalls) == updateCallsBeforeQueuedFailure + 2,
		"queued full buffer upload executes after an earlier frame failure");
	ThreadedRenderFrameCompletion failedQueuedBufferCompletion;
	ThreadedRenderFrameCompletion restoredQueuedBufferCompletion;
	result |= Check(PollThreadedRenderCompletion(device,
		&failedQueuedBufferCompletion) &&
		PollThreadedRenderCompletion(device,
			&restoredQueuedBufferCompletion) &&
		failedQueuedBufferCompletion.resourceFailure &&
		!restoredQueuedBufferCompletion.resourceFailure &&
		failedQueuedBufferCompletion.sequence <
			restoredQueuedBufferCompletion.sequence &&
		resources.PublishThreadedCompletion(
			failedQueuedBufferCompletion.sequence, true) == RENDER_RESULT_OK &&
		resources.DescribeBuffer(buffer, &bufferDescription) == RENDER_RESULT_OK &&
		bufferDescription.authority == NATIVE_W3D_CONTENT_INVALID,
		"failed buffer frame invalidates authority but retains its later full upload");
	validated = GpuHandle();
	result |= Check(resources.PublishThreadedCompletion(
		restoredQueuedBufferCompletion.sequence, false) == RENDER_RESULT_OK &&
		resources.DescribeBuffer(buffer, &bufferDescription) == RENDER_RESULT_OK &&
		bufferDescription.authority == NATIVE_W3D_CONTENT_CPU &&
		bufferDescription.authorityEpoch > bufferEpochBeforeQueuedFailure &&
		resources.AcquireVertexBufferRange(buffer, sizeof(unsigned int), 0,
			0, 4, &validated) == RENDER_RESULT_OK && validated == buffer,
		"later full buffer upload republishes CPU authority after failed B");
	const long updateCallsBeforePartialRecovery = ReadCount(&control.updateCalls);
	InterlockedExchange(&control.failUpdateOnCall,
		updateCallsBeforePartialRecovery + 1);
	result |= Check(context->beginFrame() == RENDER_RESULT_OK &&
		resources.UpdateBuffer(buffer, bytes, sizeof(bytes) / 2, 0,
			RENDER_BUFFER_UPDATE_DISCARD) == RENDER_RESULT_OK &&
		context->endFrame() == RENDER_RESULT_OK &&
		SubmitThreadedRenderFrame(device, false) == RENDER_RESULT_OK &&
		context->beginFrame() == RENDER_RESULT_OK &&
		resources.UpdateBuffer(buffer, bytes, sizeof(bytes) / 2, 0,
			RENDER_BUFFER_UPDATE_DISCARD) == RENDER_RESULT_OK &&
		context->endFrame() == RENDER_RESULT_OK &&
		SubmitThreadedRenderFrame(device, false) == RENDER_RESULT_OK &&
		DrainThreadedRenderDevice(device) == RENDER_RESULT_FAILED &&
		ReadCount(&control.updateCalls) == updateCallsBeforePartialRecovery + 2,
		"queued partial DISCARD executes after an earlier upload failure");
	ThreadedRenderFrameCompletion failedBeforePartialCompletion;
	ThreadedRenderFrameCompletion partialRecoveryCompletion;
	result |= Check(PollThreadedRenderCompletion(device,
		&failedBeforePartialCompletion) &&
		PollThreadedRenderCompletion(device, &partialRecoveryCompletion) &&
		failedBeforePartialCompletion.resourceFailure &&
		partialRecoveryCompletion.result == RENDER_RESULT_OK &&
		!partialRecoveryCompletion.resourceFailure &&
		failedBeforePartialCompletion.sequence < partialRecoveryCompletion.sequence &&
		resources.PublishThreadedCompletion(
			failedBeforePartialCompletion.sequence, true) == RENDER_RESULT_OK &&
		resources.HasBufferAuthorityFailure(buffer) &&
		resources.DescribeBuffer(buffer, &bufferDescription) == RENDER_RESULT_OK,
		"failed buffer completion invalidates authority before partial recovery");
	const unsigned int failedPartialAuthorityEpoch = bufferDescription.authorityEpoch;
	result |= Check(resources.PublishThreadedCompletion(
			partialRecoveryCompletion.sequence, false) == RENDER_RESULT_OK &&
		resources.DescribeBuffer(buffer, &bufferDescription) == RENDER_RESULT_OK &&
		bufferDescription.authority == NATIVE_W3D_CONTENT_INVALID &&
		bufferDescription.authorityEpoch > failedPartialAuthorityEpoch &&
		!resources.HasBufferAuthorityFailure(buffer),
		"successful partial recovery advances its epoch without claiming whole authority");
	validated = GpuHandle();
	result |= Check(resources.AcquireVertexBufferRange(buffer,
		sizeof(unsigned int), 0, 0, 2, &validated) == RENDER_RESULT_OK &&
		validated == buffer,
		"later partial DISCARD republishes only its independent initialized prefix");
	validated = buffer;
	result |= Check(resources.AcquireVertexBufferRange(buffer,
		sizeof(unsigned int), 0, 2, 1, &validated) ==
		RENDER_RESULT_INVALID_ARGUMENT && !validated.isValid(),
		"partial recovery rejects adjacent discarded bytes");
	const long updateCallsBeforeDependentRecovery = ReadCount(&control.updateCalls);
	InterlockedExchange(&control.failUpdateOnCall,
		updateCallsBeforeDependentRecovery + 1);
	result |= Check(context->beginFrame() == RENDER_RESULT_OK &&
		resources.UpdateBuffer(buffer, bytes, sizeof(bytes) / 2, 0,
			RENDER_BUFFER_UPDATE_DISCARD) == RENDER_RESULT_OK &&
		context->endFrame() == RENDER_RESULT_OK &&
		SubmitThreadedRenderFrame(device, false) == RENDER_RESULT_OK &&
		context->beginFrame() == RENDER_RESULT_OK &&
		resources.UpdateBuffer(buffer, bytes + 2, sizeof(unsigned int),
			sizeof(bytes) / 2, RENDER_BUFFER_UPDATE_NO_OVERWRITE) ==
			RENDER_RESULT_OK &&
		context->endFrame() == RENDER_RESULT_OK &&
		SubmitThreadedRenderFrame(device, false) == RENDER_RESULT_OK &&
		DrainThreadedRenderDevice(device) == RENDER_RESULT_FAILED &&
		ReadCount(&control.updateCalls) == updateCallsBeforeDependentRecovery + 2,
		"queued NO_OVERWRITE executes without independently restoring failed bytes");
	ThreadedRenderFrameCompletion failedBeforeDependentCompletion;
	ThreadedRenderFrameCompletion dependentRecoveryCompletion;
	result |= Check(PollThreadedRenderCompletion(device,
		&failedBeforeDependentCompletion) &&
		PollThreadedRenderCompletion(device, &dependentRecoveryCompletion) &&
		failedBeforeDependentCompletion.resourceFailure &&
		dependentRecoveryCompletion.result == RENDER_RESULT_OK &&
		!dependentRecoveryCompletion.resourceFailure &&
		resources.PublishThreadedCompletion(
			failedBeforeDependentCompletion.sequence, true) == RENDER_RESULT_OK &&
		resources.PublishThreadedCompletion(dependentRecoveryCompletion.sequence,
			false) == RENDER_RESULT_OK &&
		resources.HasBufferAuthorityFailure(buffer),
		"dependent NO_OVERWRITE cannot publish a snapshot inherited from failed DISCARD");
	validated = buffer;
	result |= Check(resources.AcquireVertexBufferRange(buffer,
		sizeof(unsigned int), 0, 0, 2, &validated) ==
		RENDER_RESULT_INVALID_ARGUMENT && !validated.isValid(),
		"dependent recovery remains fail-closed for the failed prefix");
	validated = buffer;
	result |= Check(resources.AcquireVertexBufferRange(buffer,
		sizeof(unsigned int), 0, 2, 1, &validated) ==
		RENDER_RESULT_INVALID_ARGUMENT && !validated.isValid(),
		"dependent snapshot cannot repair authority with its appended range");
	InterlockedExchange(&control.failUpdateOnCall, 0);
	const long updateCallsBeforePersistentFailure =
		ReadCount(&control.updateCalls);
	InterlockedExchange(&control.failUpdate, 1);
	result |= Check(context->beginFrame() == RENDER_RESULT_OK &&
		resources.UpdateBuffer(buffer, bytes, sizeof(bytes) / 2, 0,
			RENDER_BUFFER_UPDATE_DISCARD) == RENDER_RESULT_OK &&
		ReadCount(&control.updateCalls) ==
			updateCallsBeforePersistentFailure &&
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

	const long updateCallsBeforePreFrameFailure =
		ReadCount(&control.updateCalls);
	InterlockedExchange(&control.failUpdate, 1);
	result |= Check(resources.UpdateBuffer(buffer, bytes, sizeof(bytes), 0,
		RENDER_BUFFER_UPDATE_DISCARD) == RENDER_RESULT_FAILED &&
		ReadCount(&control.updateCalls) ==
			updateCallsBeforePreFrameFailure + 1 &&
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
		resources.Destroy(replacementBuffer) &&
		resources.UpdateBuffer(buffer, bytes, sizeof(bytes), 0,
			RENDER_BUFFER_UPDATE_DISCARD) == RENDER_RESULT_OK,
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
	TextureDescriptor streamTextureDescriptor = textureDescriptor;
	streamTextureDescriptor.binding = RENDER_TEXTURE_SHADER_RESOURCE;
	GpuHandle streamTexture;
	result |= Check(resources.CreateTexture(streamTextureDescriptor, &textureData,
		1, &streamTexture) == RENDER_RESULT_OK,
		"threaded streaming texture completes before publication");
	NativeW3DTextureDescription textureDescription;
	result |= Check(resources.DescribeTexture(streamTexture,
		&textureDescription) ==
		RENDER_RESULT_OK &&
		textureDescription.authority == NATIVE_W3D_CONTENT_CPU,
		"threaded texture starts with confirmed CPU authority");
	const unsigned int textureCreateEpoch = textureDescription.authorityEpoch;
	unsigned int firstRefreshPixels[16];
	unsigned int secondRefreshPixels[16];
	unsigned int thirdRefreshPixels[16];
	std::memset(firstRefreshPixels, 0x11, sizeof(firstRefreshPixels));
	std::memset(secondRefreshPixels, 0x22, sizeof(secondRefreshPixels));
	std::memset(thirdRefreshPixels, 0x33, sizeof(thirdRefreshPixels));
	TextureSubresourceData firstRefreshData = textureData;
	TextureSubresourceData secondRefreshData = textureData;
	TextureSubresourceData thirdRefreshData = textureData;
	firstRefreshData.data = firstRefreshPixels;
	secondRefreshData.data = secondRefreshPixels;
	thirdRefreshData.data = thirdRefreshPixels;
	const long refreshCallsBeforeFrame = ReadCount(&control.refreshCalls);
	InterlockedExchange(&control.refreshPixelSequence, 0);
	result |= Check(context->beginFrame() == RENDER_RESULT_OK &&
		resources.RefreshTexture(streamTexture, streamTextureDescriptor,
			&firstRefreshData, 1) == RENDER_RESULT_OK &&
		resources.RefreshTexture(streamTexture, streamTextureDescriptor,
			&secondRefreshData, 1) == RENDER_RESULT_OK &&
		ReadCount(&control.refreshCalls) == refreshCallsBeforeFrame &&
		context->setLegacyStateForLayout(drawState, drawLayout, 0) ==
			RENDER_RESULT_OK &&
		context->setVertexBuffer(buffer, sizeof(unsigned int), 0) ==
			RENDER_RESULT_OK &&
		context->setTexture(0, streamTexture) == RENDER_RESULT_OK &&
		context->setPrimitiveTopology(RENDER_PRIMITIVE_TRIANGLE_LIST) ==
			RENDER_RESULT_OK && context->draw(3, 0) == RENDER_RESULT_OK &&
		context->endFrame() == RENDER_RESULT_OK &&
		SubmitThreadedRenderFrame(device, false) == RENDER_RESULT_OK,
		"in-frame texture refreshes and dependent draw enqueue without a fence");
	result |= Check(resources.DescribeTexture(streamTexture,
		&textureDescription) ==
		RENDER_RESULT_OK &&
		textureDescription.authorityEpoch == textureCreateEpoch &&
		DrainThreadedRenderDevice(device) == RENDER_RESULT_OK &&
		ReadCount(&control.refreshCalls) == refreshCallsBeforeFrame + 2 &&
		ReadCount(&control.refreshPixelSequence) == 0x11 * 257 + 0x22,
		"threaded owner consumes distinct refresh pixels in FIFO order");
	ThreadedRenderFrameCompletion textureRefreshCompletion;
	result |= Check(PollThreadedRenderCompletion(device,
		&textureRefreshCompletion) &&
		!textureRefreshCompletion.resourceFailure &&
		resources.PublishThreadedCompletion(textureRefreshCompletion.sequence,
			false) == RENDER_RESULT_OK &&
		resources.DescribeTexture(streamTexture, &textureDescription) ==
			RENDER_RESULT_OK &&
		textureDescription.authority == NATIVE_W3D_CONTENT_CPU &&
		textureDescription.authorityEpoch > textureCreateEpoch,
		"matching completion publishes the coalesced texture refresh epoch");
	const unsigned int cpuEpochBeforeCopy = textureDescription.authorityEpoch;
	NativeW3DGpuContentLease refreshThenCopyLease;
	result |= Check(context->beginFrame() == RENDER_RESULT_OK &&
		resources.RefreshTexture(streamTexture, streamTextureDescriptor,
			&firstRefreshData, 1) == RENDER_RESULT_OK &&
		resources.CopyActiveColorTargetToTexture(streamTexture,
			&refreshThenCopyLease) == RENDER_RESULT_OK &&
		refreshThenCopyLease.isValid() &&
		resources.DescribeTexture(streamTexture, &textureDescription) ==
			RENDER_RESULT_OK &&
		textureDescription.authority ==
			NATIVE_W3D_CONTENT_GPU_RENDER_TARGET &&
		textureDescription.authorityEpoch ==
			refreshThenCopyLease.authorityEpoch &&
		textureDescription.authorityEpoch > cpuEpochBeforeCopy &&
		context->endFrame() == RENDER_RESULT_OK &&
		SubmitThreadedRenderFrame(device, false) == RENDER_RESULT_OK &&
		DrainThreadedRenderDevice(device) == RENDER_RESULT_OK,
		"same-frame GPU copy supersedes its earlier deferred CPU refresh");
	ThreadedRenderFrameCompletion refreshThenCopyCompletion;
	NativeW3DGpuContentLease validatedRefreshThenCopyLease =
		refreshThenCopyLease;
	result |= Check(PollThreadedRenderCompletion(device,
		&refreshThenCopyCompletion) &&
		!refreshThenCopyCompletion.resourceFailure &&
		resources.PublishThreadedCompletion(
			refreshThenCopyCompletion.sequence, false) == RENDER_RESULT_OK &&
		resources.AcquireGpuContentLease(streamTexture,
			&validatedRefreshThenCopyLease) == RENDER_RESULT_OK &&
		validatedRefreshThenCopyLease.authorityEpoch ==
			refreshThenCopyLease.authorityEpoch &&
		resources.DescribeTexture(streamTexture, &textureDescription) ==
			RENDER_RESULT_OK &&
		textureDescription.authority ==
			NATIVE_W3D_CONTENT_GPU_RENDER_TARGET &&
		textureDescription.authorityEpoch ==
			refreshThenCopyLease.authorityEpoch,
		"refresh completion preserves the newer GPU-copy lease");
	result |= Check(resources.RefreshTexture(streamTexture,
		streamTextureDescriptor, &firstRefreshData, 1) == RENDER_RESULT_OK &&
		resources.DescribeTexture(streamTexture, &textureDescription) ==
			RENDER_RESULT_OK &&
		textureDescription.authority == NATIVE_W3D_CONTENT_CPU &&
		textureDescription.authorityEpoch >
			refreshThenCopyLease.authorityEpoch,
		"synchronous CPU refresh restores the streaming fixture after GPU copy");
	const long refreshCallsBeforeGlobalFailure =
		ReadCount(&control.refreshCalls);
	InterlockedExchange(&control.failRefreshOnCall,
		refreshCallsBeforeGlobalFailure + 2);
	result |= Check(context->beginFrame() == RENDER_RESULT_OK &&
		resources.RefreshTexture(streamTexture, streamTextureDescriptor,
			&firstRefreshData, 1) == RENDER_RESULT_OK &&
		context->endFrame() == RENDER_RESULT_OK &&
		SubmitThreadedRenderFrame(device, false) == RENDER_RESULT_OK &&
		resources.RefreshTexture(texture, textureDescriptor, &textureData, 1) ==
			RENDER_RESULT_FAILED,
		"synchronous owner failure drains an older accepted texture refresh");
	ThreadedRenderFrameCompletion preFailureTextureCompletion;
	result |= Check(PollThreadedRenderCompletion(device,
		&preFailureTextureCompletion) &&
		!preFailureTextureCompletion.resourceFailure &&
		resources.PublishThreadedCompletion(
			preFailureTextureCompletion.sequence, false) == RENDER_RESULT_OK &&
		resources.DescribeTexture(streamTexture, &textureDescription) ==
			RENDER_RESULT_OK &&
		textureDescription.authority == NATIVE_W3D_CONTENT_INVALID,
		"global mutation failure clears pending texture publication before completion");
	InterlockedExchange(&control.failRefreshOnCall, 0);
	result |= Check(device->recoverDevice() == RENDER_RESULT_OK &&
		host.ReplaceContext(device->immediateContext()) == RENDER_RESULT_OK &&
		resources.RestoreStaticBuffersAfterRecovery() == RENDER_RESULT_OK &&
		resources.UpdateBuffer(buffer, bytes, sizeof(bytes), 0,
			RENDER_BUFFER_UPDATE_DISCARD) == RENDER_RESULT_OK &&
		resources.RefreshTexture(texture, textureDescriptor,
			&textureData, 1) == RENDER_RESULT_OK &&
		resources.RefreshTexture(streamTexture, streamTextureDescriptor,
			&firstRefreshData, 1) == RENDER_RESULT_OK &&
		resources.DescribeTexture(streamTexture, &textureDescription) ==
			RENDER_RESULT_OK &&
		textureDescription.authority == NATIVE_W3D_CONTENT_CPU,
		"texture fixture republishes CPU authority after global failure");
	const unsigned int firstPublishedTextureEpoch =
		textureDescription.authorityEpoch;
	const long refreshCallsBeforeQueuedFrames = ReadCount(&control.refreshCalls);
	InterlockedExchange(&control.refreshPixelSequence, 0);
	InterlockedExchange(&control.failRefreshOnCall,
		refreshCallsBeforeQueuedFrames + 2);
	result |= Check(context->beginFrame() == RENDER_RESULT_OK &&
		resources.RefreshTexture(streamTexture, streamTextureDescriptor,
			&firstRefreshData, 1) == RENDER_RESULT_OK &&
		context->setLegacyStateForLayout(drawState, drawLayout, 0) ==
			RENDER_RESULT_OK &&
		context->setVertexBuffer(buffer, sizeof(unsigned int), 0) ==
			RENDER_RESULT_OK &&
		context->setTexture(0, streamTexture) == RENDER_RESULT_OK &&
		context->setPrimitiveTopology(RENDER_PRIMITIVE_TRIANGLE_LIST) ==
			RENDER_RESULT_OK && context->draw(3, 0) == RENDER_RESULT_OK &&
		context->endFrame() == RENDER_RESULT_OK &&
		SubmitThreadedRenderFrame(device, false) == RENDER_RESULT_OK &&
		context->beginFrame() == RENDER_RESULT_OK &&
		resources.RefreshTexture(streamTexture, streamTextureDescriptor,
			&secondRefreshData, 1) == RENDER_RESULT_OK &&
		context->setLegacyStateForLayout(drawState, drawLayout, 0) ==
			RENDER_RESULT_OK &&
		context->setVertexBuffer(buffer, sizeof(unsigned int), 0) ==
			RENDER_RESULT_OK &&
		context->setTexture(0, streamTexture) == RENDER_RESULT_OK &&
		context->setPrimitiveTopology(RENDER_PRIMITIVE_TRIANGLE_LIST) ==
			RENDER_RESULT_OK && context->draw(3, 0) == RENDER_RESULT_OK &&
		context->endFrame() == RENDER_RESULT_OK &&
		SubmitThreadedRenderFrame(device, false) == RENDER_RESULT_OK &&
		context->beginFrame() == RENDER_RESULT_OK &&
		resources.RefreshTexture(streamTexture, streamTextureDescriptor,
			&thirdRefreshData, 1) == RENDER_RESULT_OK &&
		context->setLegacyStateForLayout(drawState, drawLayout, 0) ==
			RENDER_RESULT_OK &&
		context->setVertexBuffer(buffer, sizeof(unsigned int), 0) ==
			RENDER_RESULT_OK &&
		context->setTexture(0, streamTexture) == RENDER_RESULT_OK &&
		context->setPrimitiveTopology(RENDER_PRIMITIVE_TRIANGLE_LIST) ==
			RENDER_RESULT_OK && context->draw(3, 0) == RENDER_RESULT_OK &&
		context->endFrame() == RENDER_RESULT_OK &&
		SubmitThreadedRenderFrame(device, false) == RENDER_RESULT_OK,
		"three texture frames enqueue A, failed B, and restoring C packets");
	result |= Check(resources.DescribeTexture(streamTexture,
		&textureDescription) == RENDER_RESULT_OK &&
		textureDescription.authority == NATIVE_W3D_CONTENT_CPU &&
		textureDescription.authorityEpoch == firstPublishedTextureEpoch &&
		DrainThreadedRenderDevice(device) == RENDER_RESULT_FAILED &&
		ReadCount(&control.refreshCalls) == refreshCallsBeforeQueuedFrames + 3 &&
		ReadCount(&control.refreshPixelSequence) == 0x11 * 257 + 0x33,
		"queued texture epochs remain unpublished while the second owner refresh fails");
	ThreadedRenderFrameCompletion firstQueuedTextureCompletion;
	ThreadedRenderFrameCompletion failedQueuedTextureCompletion;
	ThreadedRenderFrameCompletion restoredQueuedTextureCompletion;
	result |= Check(PollThreadedRenderCompletion(device,
		&firstQueuedTextureCompletion) &&
		PollThreadedRenderCompletion(device, &failedQueuedTextureCompletion) &&
		PollThreadedRenderCompletion(device, &restoredQueuedTextureCompletion) &&
		!firstQueuedTextureCompletion.resourceFailure &&
		failedQueuedTextureCompletion.resourceFailure &&
		!restoredQueuedTextureCompletion.resourceFailure &&
		firstQueuedTextureCompletion.sequence <
			failedQueuedTextureCompletion.sequence &&
		failedQueuedTextureCompletion.sequence <
			restoredQueuedTextureCompletion.sequence &&
		resources.PublishThreadedCompletion(
			firstQueuedTextureCompletion.sequence, false) == RENDER_RESULT_OK &&
		resources.DescribeTexture(streamTexture, &textureDescription) ==
			RENDER_RESULT_OK &&
		textureDescription.authority == NATIVE_W3D_CONTENT_CPU &&
		textureDescription.authorityEpoch > firstPublishedTextureEpoch,
		"first completion publishes A while failed B remains pending");
	result |= Check(resources.PublishThreadedCompletion(
		failedQueuedTextureCompletion.sequence, true) == RENDER_RESULT_OK &&
		resources.DescribeTexture(streamTexture, &textureDescription) ==
			RENDER_RESULT_OK &&
		textureDescription.authority == NATIVE_W3D_CONTENT_INVALID,
		"second completion invalidates the failed pending texture epoch");
	const unsigned int failedQueuedTextureEpoch =
		textureDescription.authorityEpoch;
	result |= Check(resources.PublishThreadedCompletion(
		restoredQueuedTextureCompletion.sequence, false) == RENDER_RESULT_OK &&
		resources.DescribeTexture(streamTexture, &textureDescription) ==
			RENDER_RESULT_OK &&
		textureDescription.authority == NATIVE_W3D_CONTENT_CPU &&
		textureDescription.authorityEpoch > failedQueuedTextureEpoch,
		"successful C completion republishes CPU authority after failed B");
	InterlockedExchange(&control.failRefreshOnCall, 0);
	result |= Check(device->recoverDevice() == RENDER_RESULT_OK &&
		host.ReplaceContext(device->immediateContext()) == RENDER_RESULT_OK &&
		resources.RestoreStaticBuffersAfterRecovery() == RENDER_RESULT_OK &&
		resources.UpdateBuffer(buffer, bytes, sizeof(bytes), 0,
			RENDER_BUFFER_UPDATE_DISCARD) == RENDER_RESULT_OK &&
		resources.RefreshTexture(streamTexture, streamTextureDescriptor,
			&firstRefreshData, 1) == RENDER_RESULT_OK,
		"texture publication fixture recovers after a queued refresh failure");

	InterlockedExchange(&control.failRefresh, 1);
	const long refreshCallsBeforeFailure = ReadCount(&control.refreshCalls);
	result |= Check(resources.RefreshTexture(texture, textureDescriptor,
		&textureData, 1) == RENDER_RESULT_FAILED &&
		ReadCount(&control.refreshCalls) == refreshCallsBeforeFailure + 1 &&
		resources.DescribeTexture(texture, &textureDescription) ==
		RENDER_RESULT_OK &&
		textureDescription.authority == NATIVE_W3D_CONTENT_INVALID,
		"threaded owner-side refresh failure invalidates optimistic authority");
	InterlockedExchange(&control.failRefresh, 0);
	result |= Check(device->recoverDevice() == RENDER_RESULT_OK &&
		host.ReplaceContext(device->immediateContext()) == RENDER_RESULT_OK &&
		resources.RestoreStaticBuffersAfterRecovery() == RENDER_RESULT_OK &&
		resources.UpdateBuffer(buffer, bytes, sizeof(bytes), 0,
			RENDER_BUFFER_UPDATE_DISCARD) == RENDER_RESULT_OK &&
		resources.RefreshTexture(texture, textureDescriptor,
			&firstRefreshData, 1) == RENDER_RESULT_OK &&
		resources.RefreshTexture(streamTexture, streamTextureDescriptor,
			&firstRefreshData, 1) == RENDER_RESULT_OK,
		"threaded owner recovers after an asynchronous refresh failure");

	InterlockedExchange(&control.failRefresh, 1);
	const long refreshCallsBeforeAsyncFailure = ReadCount(&control.refreshCalls);
	result |= Check(context->beginFrame() == RENDER_RESULT_OK &&
		resources.RefreshTexture(streamTexture, streamTextureDescriptor,
			&secondRefreshData, 1) == RENDER_RESULT_OK &&
		ReadCount(&control.refreshCalls) == refreshCallsBeforeAsyncFailure &&
		context->setLegacyStateForLayout(drawState, drawLayout, 0) ==
			RENDER_RESULT_OK &&
		context->setVertexBuffer(buffer, sizeof(unsigned int), 0) ==
			RENDER_RESULT_OK &&
		context->setTexture(0, streamTexture) == RENDER_RESULT_OK &&
		context->setPrimitiveTopology(RENDER_PRIMITIVE_TRIANGLE_LIST) ==
			RENDER_RESULT_OK && context->draw(3, 0) == RENDER_RESULT_OK &&
		context->endFrame() == RENDER_RESULT_OK &&
		SubmitThreadedRenderFrame(device, false) == RENDER_RESULT_OK &&
		DrainThreadedRenderDevice(device) == RENDER_RESULT_FAILED,
		"owner-side refresh failure remains deferred to frame completion");
	ThreadedRenderFrameCompletion failedTextureRefreshCompletion;
	result |= Check(PollThreadedRenderCompletion(device,
		&failedTextureRefreshCompletion) &&
		failedTextureRefreshCompletion.resourceFailure &&
		resources.PublishThreadedCompletion(
			failedTextureRefreshCompletion.sequence, true) == RENDER_RESULT_OK &&
		resources.DescribeTexture(streamTexture, &textureDescription) ==
			RENDER_RESULT_OK &&
		textureDescription.authority == NATIVE_W3D_CONTENT_INVALID,
		"failed completion invalidates its pending texture publication");
	InterlockedExchange(&control.failRefresh, 0);
	result |= Check(device->recoverDevice() == RENDER_RESULT_OK &&
		host.ReplaceContext(device->immediateContext()) == RENDER_RESULT_OK &&
		resources.RestoreStaticBuffersAfterRecovery() == RENDER_RESULT_OK &&
		resources.RefreshTexture(texture, textureDescriptor,
			&textureData, 1) == RENDER_RESULT_OK,
		"synchronous refresh republishes CPU pixels after recovery");

	const long copyCallsBeforeFailure = ReadCount(&control.copyCalls);
	InterlockedExchange(&control.failCopy, 1);
	NativeW3DGpuContentLease lease;
	result |= Check(context->beginFrame() == RENDER_RESULT_OK &&
		resources.CopyActiveColorTargetToTexture(texture, &lease) ==
		RENDER_RESULT_FAILED && !lease.isValid() &&
		ReadCount(&control.copyCalls) == copyCallsBeforeFailure + 1 &&
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

int TestThreadedAdjacentBufferPublication()
{
	int result = 0;
	FakeRenderControl control;
	ThreadedRenderOptions options;
	options.serial = false;
	options.maxFramesInFlight = 3;
	options.maxPacketBytes = 1024 * 1024;
	options.maxPacketCommands = 128;
	options.resourceCapacity = 2;
	IRenderDevice *device = CreateThreadedRenderDevice(
		CreateThreadedFakeRenderDevice, &control, options);
	if (device == 0) return Check(false, "adjacent publication device allocates");
	RenderDeviceParameters parameters;
	parameters.backend = RENDER_BACKEND_D3D11;
	parameters.window = reinterpret_cast<void *>(1);
	parameters.width = 4;
	parameters.height = 4;
	result |= Check(device->initialize(parameters) == RENDER_RESULT_OK,
		"adjacent publication device initializes");
	NativeW3DResourceHost host(2);
	NativeW3DResources resources(2);
	result |= Check(host.Attach(device, device->immediateContext()) ==
		RENDER_RESULT_OK && resources.BindHost(&host) == RENDER_RESULT_OK,
		"adjacent publication fixture binds resource host");
	unsigned int words[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
	BufferDescriptor descriptor;
	descriptor.byteCount = sizeof(words);
	descriptor.stride = sizeof(words[0]);
	descriptor.binding = RENDER_BUFFER_VERTEX;
	descriptor.usage = RENDER_USAGE_DYNAMIC;
	GpuHandle buffer;
	result |= Check(resources.CreateBuffer(descriptor, 0, 0, &buffer) ==
		RENDER_RESULT_OK, "adjacent publication buffer creates");
	IRenderContext *context = device->immediateContext();
	GpuHandle validated;
	result |= Check(context->beginFrame() == RENDER_RESULT_OK &&
		resources.UpdateBuffer(buffer, words, 4, 0,
			RENDER_BUFFER_UPDATE_DISCARD) == RENDER_RESULT_OK &&
		resources.UpdateBuffer(buffer, words + 1, 4, 4,
			RENDER_BUFFER_UPDATE_NO_OVERWRITE) == RENDER_RESULT_OK &&
		resources.UpdateBuffer(buffer, words + 2, 4, 8,
			RENDER_BUFFER_UPDATE_NO_OVERWRITE) == RENDER_RESULT_OK,
		"same-sequence adjacent writes extend the accepted submission prefix");
	result |= Check(resources.UpdateBuffer(buffer, words + 1, 4, 4,
			RENDER_BUFFER_UPDATE_NO_OVERWRITE) == RENDER_RESULT_OK &&
		resources.UpdateBuffer(buffer, words + 5, 4, 20,
			RENDER_BUFFER_UPDATE_NO_OVERWRITE) == RENDER_RESULT_OK &&
		resources.UpdateBuffer(buffer, words + 6, 4, 24,
			RENDER_BUFFER_UPDATE_NO_OVERWRITE) == RENDER_RESULT_OK,
		"overlap, gap, and multiple-range writes keep their general path");
	result |= Check(context->endFrame() == RENDER_RESULT_OK &&
		SubmitThreadedRenderFrame(device, false) == RENDER_RESULT_OK &&
		context->beginFrame() == RENDER_RESULT_OK &&
		resources.UpdateBuffer(buffer, words + 3, 4, 12,
			RENDER_BUFFER_UPDATE_NO_OVERWRITE) == RENDER_RESULT_OK &&
		context->endFrame() == RENDER_RESULT_OK &&
		SubmitThreadedRenderFrame(device, false) == RENDER_RESULT_OK,
		"later queued sequence cannot use the same-sequence fast path");
	ThreadedRenderFrameCompletion first;
	ThreadedRenderFrameCompletion second;
	result |= Check(DrainThreadedRenderDevice(device) == RENDER_RESULT_OK &&
		PollThreadedRenderCompletion(device, &first) &&
		PollThreadedRenderCompletion(device, &second) &&
		first.sequence < second.sequence &&
		resources.PublishThreadedCompletion(first.sequence, false) ==
			RENDER_RESULT_OK &&
		resources.AcquireVertexBufferRange(buffer, 4, 0, 0, 3,
			&validated) == RENDER_RESULT_OK && validated == buffer,
		"first completion preserves its exact prefix while another frame is pending");
	validated = buffer;
	result |= Check(resources.AcquireVertexBufferRange(buffer, 4, 0, 3, 1,
			&validated) == RENDER_RESULT_INVALID_ARGUMENT &&
		!validated.isValid() &&
		resources.PublishThreadedCompletion(second.sequence, false) ==
			RENDER_RESULT_OK &&
		resources.AcquireVertexBufferRange(buffer, 4, 0, 0, 4,
			&validated) == RENDER_RESULT_OK && validated == buffer,
		"second completion publishes only its own dependent extension");
	const long failureCall = ReadCount(&control.updateCalls) + 1;
	InterlockedExchange(&control.failUpdateOnCall, failureCall);
	result |= Check(context->beginFrame() == RENDER_RESULT_OK &&
		resources.UpdateBuffer(buffer, words, 4, 0,
			RENDER_BUFFER_UPDATE_DISCARD) == RENDER_RESULT_OK &&
		context->endFrame() == RENDER_RESULT_OK &&
		SubmitThreadedRenderFrame(device, false) == RENDER_RESULT_OK &&
		context->beginFrame() == RENDER_RESULT_OK &&
		resources.UpdateBuffer(buffer, words, 4, 0,
			RENDER_BUFFER_UPDATE_DISCARD) == RENDER_RESULT_OK &&
		resources.UpdateBuffer(buffer, words + 1, 4, 4,
			RENDER_BUFFER_UPDATE_NO_OVERWRITE) == RENDER_RESULT_OK &&
		context->endFrame() == RENDER_RESULT_OK &&
		SubmitThreadedRenderFrame(device, false) == RENDER_RESULT_OK &&
		DrainThreadedRenderDevice(device) == RENDER_RESULT_FAILED,
		"later DISCARD plus adjacent append remains queued after owner failure");
	ThreadedRenderFrameCompletion failed;
	ThreadedRenderFrameCompletion recovered;
	result |= Check(PollThreadedRenderCompletion(device, &failed) &&
		PollThreadedRenderCompletion(device, &recovered) &&
		failed.resourceFailure && !recovered.resourceFailure &&
		resources.PublishThreadedCompletion(failed.sequence, true) ==
			RENDER_RESULT_OK && resources.HasBufferAuthorityFailure(buffer) &&
		resources.PublishThreadedCompletion(recovered.sequence, false) ==
			RENDER_RESULT_OK && !resources.HasBufferAuthorityFailure(buffer) &&
		resources.AcquireVertexBufferRange(buffer, 4, 0, 0, 2,
			&validated) == RENDER_RESULT_OK && validated == buffer,
		"same-sequence append preserves DISCARD restore-after-failure boundary");
	InterlockedExchange(&control.failUpdateOnCall, 0);
	result |= Check(device->recoverDevice() == RENDER_RESULT_OK &&
		host.ReplaceContext(device->immediateContext()) == RENDER_RESULT_OK &&
		resources.RestoreStaticBuffersAfterRecovery() == RENDER_RESULT_OK,
		"backend epoch advances after adjacent publication");
	result |= Check(resources.AcquireVertexBufferRange(buffer, 4, 0, 0, 1,
			&validated) == RENDER_RESULT_INVALID_ARGUMENT &&
		!validated.isValid() &&
		resources.UpdateBuffer(buffer, words, 4, 0,
			RENDER_BUFFER_UPDATE_DISCARD) == RENDER_RESULT_OK &&
		resources.Shutdown() == RENDER_RESULT_OK &&
		host.Detach() == RENDER_RESULT_OK,
		"epoch change rejects stale ranges before a fresh synchronous DISCARD");
	device->shutdown();
	delete device;
	return result;
}

int TestRejectedAdjacentBufferPublication()
{
	int result = 0;
	FakeRenderControl control;
	ThreadedRenderOptions options;
	options.serial = false;
	options.maxFramesInFlight = 2;
	options.maxPacketBytes = sizeof(BufferDescriptor);
	options.maxPacketCommands = 16;
	options.resourceCapacity = 1;
	IRenderDevice *device = CreateThreadedRenderDevice(
		CreateThreadedFakeRenderDevice, &control, options);
	if (device == 0) return Check(false, "rejected adjacent device allocates");
	RenderDeviceParameters parameters;
	parameters.backend = RENDER_BACKEND_D3D11;
	parameters.window = reinterpret_cast<void *>(1);
	parameters.width = 4;
	parameters.height = 4;
	result |= Check(device->initialize(parameters) == RENDER_RESULT_OK,
		"rejected adjacent device initializes");
	NativeW3DResourceHost host(1);
	NativeW3DResources resources(1);
	result |= Check(host.Attach(device, device->immediateContext()) ==
		RENDER_RESULT_OK && resources.BindHost(&host) == RENDER_RESULT_OK,
		"rejected adjacent fixture binds");
	unsigned int words[32] = { 1, 2, 3, 4 };
	BufferDescriptor descriptor;
	descriptor.byteCount = sizeof(words);
	descriptor.stride = sizeof(words[0]);
	descriptor.binding = RENDER_BUFFER_VERTEX;
	descriptor.usage = RENDER_USAGE_DYNAMIC;
	GpuHandle buffer;
	result |= Check(resources.CreateBuffer(descriptor, 0, 0, &buffer) ==
		RENDER_RESULT_OK, "rejected adjacent fixture creates buffer");
	IRenderContext *context = device->immediateContext();
	NativeW3DBufferDescription before;
	NativeW3DBufferDescription after;
	result |= Check(context->beginFrame() == RENDER_RESULT_OK &&
		resources.UpdateBuffer(buffer, words, 4, 0,
			RENDER_BUFFER_UPDATE_DISCARD) == RENDER_RESULT_OK &&
		resources.UpdateBuffer(buffer, words + 1, 4, 4,
			RENDER_BUFFER_UPDATE_NO_OVERWRITE) == RENDER_RESULT_OK &&
		resources.DescribeBuffer(buffer, &before) == RENDER_RESULT_OK &&
		resources.UpdateBuffer(buffer, words + 2, 64, 8,
			RENDER_BUFFER_UPDATE_NO_OVERWRITE) == RENDER_RESULT_OUT_OF_MEMORY &&
		resources.DescribeBuffer(buffer, &after) == RENDER_RESULT_OK &&
		before.authority == after.authority &&
		before.authorityEpoch == after.authorityEpoch,
		"rejected adjacent packet cannot extend an accepted prefix");
	result |= Check(resources.UpdateBuffer(buffer, words + 2, 4, 8,
			RENDER_BUFFER_UPDATE_NO_OVERWRITE) == RENDER_RESULT_OK &&
		resources.DescribeBuffer(buffer, &after) == RENDER_RESULT_OK &&
		after.authority == before.authority,
		"rejected adjacent tail stays absent until a later accepted append");
	result |= Check(context->endFrame() == RENDER_RESULT_OUT_OF_MEMORY &&
		SubmitThreadedRenderFrame(device, false) == RENDER_RESULT_OK,
		"producer failure remains latched after enqueue rejection");
	DrainThreadedRenderDevice(device);
	result |= Check(ReadCount(&control.updateCalls) == 3,
		"owner executes the three accepted writes but not the rejected append");
	ThreadedRenderFrameCompletion completion;
	result |= Check(PollThreadedRenderCompletion(device, &completion) &&
		!completion.resourceFailure &&
		resources.PublishThreadedCompletion(completion.sequence,
			completion.resourceFailure) == RENDER_RESULT_OK,
		"accepted writes publish after a completed producer-rejected frame");
	GpuHandle validated;
	result |= Check(resources.AcquireVertexBufferRange(buffer, 4, 0, 0, 3,
			&validated) == RENDER_RESULT_OK && validated == buffer,
		"accepted prefix remains exactly initialized through byte 12");
	validated = buffer;
	result |= Check(resources.AcquireVertexBufferRange(buffer, 4, 0, 3, 15,
			&validated) == RENDER_RESULT_INVALID_ARGUMENT &&
		!validated.isValid(),
		"rejected in-bounds tail bytes 12 through 72 remain unavailable");
	validated = buffer;
	result |= Check(resources.AcquireVertexBufferRange(buffer, 4, 0, 3, 1,
			&validated) == RENDER_RESULT_INVALID_ARGUMENT &&
		!validated.isValid(),
		"first rejected tail word remains unavailable");
	validated = buffer;
	result |= Check(resources.AcquireVertexBufferRange(buffer, 4, 0, 17, 1,
			&validated) == RENDER_RESULT_INVALID_ARGUMENT &&
		!validated.isValid(),
		"last rejected word remains unavailable and clears its output handle");
	result |= Check(resources.Shutdown() == RENDER_RESULT_OK &&
		host.Detach() == RENDER_RESULT_OK,
		"rejected adjacent fixture releases resources");
	device->shutdown();
	delete device;
	return result;
}

int BenchmarkThreadedAdjacentBufferPublication()
{
	int result = 0;
	FakeRenderControl control;
	ThreadedRenderOptions options;
	options.serial = false;
	options.maxFramesInFlight = 2;
	options.maxPacketBytes = 65536;
	options.maxPacketCommands = 2048;
	options.resourceCapacity = 1;
	IRenderDevice *device = CreateThreadedRenderDevice(
		CreateThreadedFakeRenderDevice, &control, options);
	if (device == 0) return Check(false, "adjacent benchmark device allocates");
	RenderDeviceParameters parameters;
	parameters.backend = RENDER_BACKEND_D3D11;
	parameters.window = reinterpret_cast<void *>(1);
	parameters.width = 4;
	parameters.height = 4;
	result |= Check(device->initialize(parameters) == RENDER_RESULT_OK,
		"adjacent benchmark device initializes");
	NativeW3DResourceHost host(1);
	NativeW3DResources resources(1);
	result |= Check(host.Attach(device, device->immediateContext()) ==
		RENDER_RESULT_OK && resources.BindHost(&host) == RENDER_RESULT_OK,
		"adjacent benchmark fixture binds");
	unsigned int words[1024];
	for (unsigned int i = 0; i < 1024; ++i) words[i] = i;
	BufferDescriptor descriptor;
	descriptor.byteCount = sizeof(words);
	descriptor.stride = sizeof(words[0]);
	descriptor.binding = RENDER_BUFFER_VERTEX;
	descriptor.usage = RENDER_USAGE_DYNAMIC;
	GpuHandle buffer;
	result |= Check(resources.CreateBuffer(descriptor, 0, 0, &buffer) ==
		RENDER_RESULT_OK, "adjacent benchmark buffer creates");
	IRenderContext *context = device->immediateContext();
	for (unsigned int pass = 0; pass < 8 && result == 0; ++pass)
	{
		result |= Check(context->beginFrame() == RENDER_RESULT_OK &&
			resources.UpdateBuffer(buffer, words, 4, 0,
				RENDER_BUFFER_UPDATE_DISCARD) == RENDER_RESULT_OK,
			"adjacent benchmark starts a fresh frame and DISCARD");
		const std::chrono::steady_clock::time_point start =
			std::chrono::steady_clock::now();
		for (unsigned int i = 1; i < 1024 && result == 0; ++i)
			result |= Check(resources.UpdateBuffer(buffer, words + i, 4, i * 4,
				RENDER_BUFFER_UPDATE_NO_OVERWRITE) == RENDER_RESULT_OK,
				"adjacent benchmark accepts the production resource update");
		const std::chrono::steady_clock::time_point end =
			std::chrono::steady_clock::now();
		const double milliseconds =
			std::chrono::duration<double, std::milli>(end - start).count();
		std::printf("adjacent_producer_pass=%u updates=1023 ms=%.3f\n",
			pass + 1, milliseconds);
		result |= Check(context->endFrame() == RENDER_RESULT_OK &&
			SubmitThreadedRenderFrame(device, false) == RENDER_RESULT_OK &&
			DrainThreadedRenderDevice(device) == RENDER_RESULT_OK,
			"adjacent benchmark frame completes successfully");
		ThreadedRenderFrameCompletion completion;
		GpuHandle validated;
		result |= Check(PollThreadedRenderCompletion(device, &completion) &&
			!completion.resourceFailure &&
			resources.PublishThreadedCompletion(completion.sequence, false) ==
				RENDER_RESULT_OK &&
			resources.AcquireVertexBufferRange(buffer, 4, 0, 0, 1024,
				&validated) == RENDER_RESULT_OK && validated == buffer,
			"adjacent benchmark publishes the same full buffer each pass");
	}
	result |= Check(resources.Shutdown() == RENDER_RESULT_OK &&
		host.Detach() == RENDER_RESULT_OK,
		"adjacent benchmark fixture releases resources");
	device->shutdown();
	delete device;
	return result;
}

int TestThreadedBetweenFrameBufferUpdates()
{
	int result = 0;
	FakeRenderControl control;
	ThreadedRenderOptions options;
	IRenderDevice *device = CreateThreadedRenderDevice(
		CreateThreadedFakeRenderDevice, &control, options);
	if (device == 0)
		return Check(false, "between-frame fixture allocates its device");
	RenderDeviceParameters parameters;
	parameters.backend = RENDER_BACKEND_D3D11;
	parameters.window = reinterpret_cast<void *>(1);
	parameters.width = 4;
	parameters.height = 4;
	result |= Check(device->initialize(parameters) == RENDER_RESULT_OK,
		"between-frame fixture initializes the render owner");
	NativeW3D2 product;
	result |= Check(product.AttachBackend(device, device->immediateContext()) ==
		RENDER_RESULT_OK, "product binds its buffer publication fence");
	BufferDescriptor descriptor;
	descriptor.byteCount = 16;
	descriptor.stride = 4;
	descriptor.binding = RENDER_BUFFER_VERTEX;
	descriptor.usage = RENDER_USAGE_DEFAULT;
	NativeW3DBufferOwner owner;
	result |= Check(owner.Create(descriptor) == RENDER_RESULT_OK,
		"terrain-shaped DEFAULT buffer is created");
	void *bytes = 0;
	result |= Check(product.Renderer().BeginFrame() == RENDER_RESULT_OK &&
		owner.Lock(0, 0, RENDER_BUFFER_UPDATE_PRESERVE, &bytes) ==
			RENDER_RESULT_OK && bytes != 0,
		"terrain upload begins inside the preceding frame");
	if (bytes != 0) std::memset(bytes, 0x31, descriptor.byteCount);
	result |= Check(owner.Unlock() == RENDER_RESULT_OK &&
		product.Renderer().EndFrame(false) == RENDER_RESULT_OK &&
		SubmitThreadedRenderFrame(device, false) == RENDER_RESULT_OK,
		"terrain upload submits without consuming its frame completion");
	// Deliberately do not poll completion or begin another frame. Dynamic
	// lighting runs at this same point in the animated shell update.
	result |= Check(owner.Lock(4, 4, RENDER_BUFFER_UPDATE_PRESERVE, &bytes) ==
		RENDER_RESULT_OK && bytes != 0 &&
		static_cast<unsigned char *>(bytes)[0] == 0x31,
		"between-frame lighting starts with the accepted terrain image");
	if (bytes != 0) std::memset(bytes, 0x42, 4);
	GpuHandle handle;
	NativeW3DBufferDescription description;
	result |= Check(owner.Unlock() == RENDER_RESULT_OK &&
		!owner.HasFailedMutation() && ReadCount(&control.updateCalls) == 2 &&
		owner.AcquireVertexRange(4, 0, 0, 4, &handle) == RENDER_RESULT_OK &&
		product.Resources().DescribeBuffer(handle, &description) ==
			RENDER_RESULT_OK && description.authority == NATIVE_W3D_CONTENT_CPU,
		"completion fence orders the lighting write without poisoning terrain");
	ThreadedRenderFrameCompletion consumed;
	result |= Check(!PollThreadedRenderCompletion(device, &consumed),
		"product completion owner consumed the preceding frame exactly once");
	result |= Check(owner.Lock(0, 0, RENDER_BUFFER_UPDATE_PRESERVE, &bytes) ==
		RENDER_RESULT_OK && bytes != 0 &&
		static_cast<unsigned char *>(bytes)[0] == 0x31 &&
		static_cast<unsigned char *>(bytes)[4] == 0x42 &&
		static_cast<unsigned char *>(bytes)[8] == 0x31 &&
		owner.Unlock() == RENDER_RESULT_OK,
		"later terrain locks retain changed and untouched bytes");

	InterlockedExchange(&control.failUpdate, 1);
	result |= Check(product.Renderer().BeginFrame() == RENDER_RESULT_OK &&
		owner.Lock(0, 0, RENDER_BUFFER_UPDATE_PRESERVE, &bytes) ==
			RENDER_RESULT_OK && owner.Unlock() == RENDER_RESULT_OK &&
		product.Renderer().EndFrame(false) == RENDER_RESULT_OK &&
		SubmitThreadedRenderFrame(device, false) == RENDER_RESULT_OK &&
		owner.Lock(4, 4, RENDER_BUFFER_UPDATE_PRESERVE, &bytes) ==
			RENDER_RESULT_OK,
		"failed preceding upload is still pending when lighting acquires staging");
	result |= Check(owner.Unlock() == RENDER_RESULT_FAILED &&
		owner.HasFailedMutation() &&
		owner.Lock(0, 0, RENDER_BUFFER_UPDATE_PRESERVE, &bytes) ==
			RENDER_RESULT_FAILED && bytes == 0,
		"fence publishes real upload failure and continues rejecting stale preserve");
	InterlockedExchange(&control.failUpdate, 0);
	result |= Check(owner.Reset() == RENDER_RESULT_OK &&
		product.Shutdown() == RENDER_RESULT_OK,
		"between-frame fixture releases its product binding");
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

int TestUnrelatedFrameFailureDoesNotInvalidateResourceMutation()
{
	int result = 0;
	FakeRenderControl control;
	ThreadedRenderOptions options;
	options.serial = false;
	options.resourceCapacity = 4;
	IRenderDevice *device = CreateThreadedRenderDevice(
		CreateThreadedFakeRenderDevice, &control, options);
	if (device == 0)
		return Check(false, "resource fence fixture allocates");
	RenderDeviceParameters parameters;
	parameters.backend = RENDER_BACKEND_D3D11;
	parameters.window = reinterpret_cast<void *>(1);
	parameters.width = 4;
	parameters.height = 4;
	if (device->initialize(parameters) != RENDER_RESULT_OK)
	{
		delete device;
		return Check(false, "resource fence fixture initializes");
	}
	NativeW3DResourceHost host(4);
	NativeW3DResources resources(4);
	result |= Check(host.Attach(device, device->immediateContext()) ==
		RENDER_RESULT_OK && resources.BindHost(&host) == RENDER_RESULT_OK,
		"resource fence fixture binds a threaded host");
	unsigned int bytes[4] = { 1, 2, 3, 4 };
	BufferDescriptor bufferDescriptor;
	bufferDescriptor.byteCount = sizeof(bytes);
	bufferDescriptor.stride = sizeof(unsigned int);
	bufferDescriptor.binding = RENDER_BUFFER_VERTEX;
	bufferDescriptor.usage = RENDER_USAGE_DEFAULT;
	GpuHandle buffer;
	result |= Check(resources.CreateBuffer(bufferDescriptor, bytes,
		sizeof(bytes), &buffer) == RENDER_RESULT_OK,
		"resource fence fixture creates an unrelated vertex buffer");
	unsigned int pixels[16] = { 0 };
	TextureDescriptor textureDescriptor;
	textureDescriptor.width = 4;
	textureDescriptor.height = 4;
	textureDescriptor.mipCount = 1;
	textureDescriptor.arrayCount = 1;
	textureDescriptor.dimension = RENDER_TEXTURE_2D;
	textureDescriptor.format = RENDER_FORMAT_R8G8B8A8_UNORM;
	textureDescriptor.binding = RENDER_TEXTURE_SHADER_RESOURCE;
	textureDescriptor.usage = RENDER_USAGE_DEFAULT;
	TextureSubresourceData textureData;
	textureData.data = pixels;
	textureData.rowPitch = sizeof(unsigned int) * 4;
	textureData.slicePitch = sizeof(pixels);
	GpuHandle texture;
	result |= Check(resources.CreateTexture(textureDescriptor, &textureData,
		1, &texture) == RENDER_RESULT_OK,
		"resource fence fixture creates a CPU texture");
	GpuHandle validated;
	NativeW3DTextureDescription textureDescription;
	result |= Check(resources.AcquireVertexBufferRange(buffer,
		sizeof(unsigned int), 0, 0, 4, &validated) == RENDER_RESULT_OK &&
		validated == buffer &&
		resources.DescribeTexture(texture, &textureDescription) ==
			RENDER_RESULT_OK &&
		textureDescription.authority == NATIVE_W3D_CONTENT_CPU,
		"both resources have authority before the unrelated frame fails");
	IRenderContext *context = device->immediateContext();
	result |= Check(context->beginFrame() == RENDER_RESULT_OK &&
		CancelThreadedRenderFrame(device, RENDER_RESULT_FAILED) ==
			RENDER_RESULT_OK,
		"unrelated failed frame is accepted before the texture refresh");
	const RenderResult refresh = resources.RefreshTexture(texture,
		textureDescriptor, &textureData, 1);
	result |= Check(ReadCount(&control.refreshCalls) == 1,
		"render owner executed the texture refresh despite the failed frame");
	validated = GpuHandle();
	result |= Check(refresh == RENDER_RESULT_OK &&
		resources.DescribeTexture(texture, &textureDescription) ==
			RENDER_RESULT_OK &&
		textureDescription.authority == NATIVE_W3D_CONTENT_CPU &&
		resources.AcquireVertexBufferRange(buffer, sizeof(unsigned int), 0,
			0, 4, &validated) == RENDER_RESULT_OK && validated == buffer,
		"successful refresh preserves texture and unrelated buffer authority");
	ThreadedRenderFrameCompletion completion;
	result |= Check(DrainThreadedRenderDevice(device) == RENDER_RESULT_FAILED &&
		PollThreadedRenderCompletion(device, &completion) &&
		completion.result == RENDER_RESULT_FAILED &&
		!completion.resourceFailure &&
		resources.PublishThreadedCompletion(completion.sequence, false) ==
			RENDER_RESULT_OK,
		"unrelated failed frame remains observable after resource completion");
	result |= Check(resources.Shutdown() == RENDER_RESULT_OK &&
		host.Detach() == RENDER_RESULT_OK,
		"resource fence fixture releases only its resources");
	device->shutdown();
	delete device;
	return result;
}

int TestSplitPacketResourceFenceReportsOwnerFailure()
{
	int result = 0;
	FakeRenderControl control;
	ThreadedRenderOptions options;
	options.serial = false;
	options.maxPacketCommands = 1;
	options.resourceCapacity = 2;
	IRenderDevice *device = CreateThreadedRenderDevice(
		CreateThreadedFakeRenderDevice, &control, options);
	if (device == 0)
		return Check(false, "split resource fence fixture allocates");
	RenderDeviceParameters parameters;
	parameters.backend = RENDER_BACKEND_D3D11;
	parameters.window = reinterpret_cast<void *>(1);
	parameters.width = 4;
	parameters.height = 4;
	if (device->initialize(parameters) != RENDER_RESULT_OK)
	{
		delete device;
		return Check(false, "split resource fence fixture initializes");
	}
	unsigned int pixels[16] = { 0 };
	TextureDescriptor descriptor;
	descriptor.width = 4;
	descriptor.height = 4;
	descriptor.mipCount = 1;
	descriptor.arrayCount = 1;
	descriptor.dimension = RENDER_TEXTURE_2D;
	descriptor.format = RENDER_FORMAT_R8G8B8A8_UNORM;
	descriptor.binding = RENDER_TEXTURE_SHADER_RESOURCE;
	descriptor.usage = RENDER_USAGE_DEFAULT;
	TextureSubresourceData data;
	data.data = pixels;
	data.rowPitch = sizeof(unsigned int) * 4;
	data.slicePitch = sizeof(pixels);
	GpuHandle texture;
	result |= Check(device->createTexture(descriptor, &data, 1,
		&texture) == RENDER_RESULT_OK &&
		FenceThreadedRenderResourceMutation(device) == RENDER_RESULT_OK,
		"split resource fence fixture creates its native texture");
	InterlockedExchange(&control.failRefresh, 1);
	IRenderContext *context = device->immediateContext();
	result |= Check(context->beginFrame() == RENDER_RESULT_OK &&
		device->refreshTexture(texture, descriptor, &data, 1) ==
			RENDER_RESULT_OK &&
		FenceThreadedRenderResourceMutation(device) == RENDER_RESULT_FAILED &&
		ReadCount(&control.refreshCalls) == 1,
		"resource fence sees failed refresh after Begin forces a packet split");
	result |= Check(context->endFrame() == RENDER_RESULT_OK &&
		SubmitThreadedRenderFrame(device, false) == RENDER_RESULT_OK &&
		DrainThreadedRenderDevice(device) == RENDER_RESULT_FAILED,
		"split resource failure remains in the aggregate frame result");
	device->shutdown();
	delete device;
	return result;
}

int TestRejectedBufferUpdate(const char *caseName, RenderUsage usage,
	unsigned int binding, RenderBufferUpdateMode mode, size_t offset,
	RenderResult expected)
{
	int result = 0;
	FakeRenderControl control;
	FakeRenderDevice device(true, &control);
	NativeW3DResourceHost host(2);
	NativeW3DResources resources(2);
	const unsigned int original[] = { 1, 2, 3, 4 };
	const unsigned int replacement = 17;
	BufferDescriptor descriptor;
	descriptor.byteCount = sizeof(original);
	descriptor.stride = sizeof(original[0]);
	descriptor.binding = binding;
	descriptor.usage = usage;
	GpuHandle buffer;
	NativeW3DBufferDescription before;
	result |= Check(host.Attach(&device, device.immediateContext()) ==
		RENDER_RESULT_OK && resources.BindHost(&host) == RENDER_RESULT_OK &&
		resources.CreateBuffer(descriptor, original, sizeof(original), &buffer) ==
			RENDER_RESULT_OK &&
		resources.DescribeBuffer(buffer, &before) == RENDER_RESULT_OK &&
		before.authority == NATIVE_W3D_CONTENT_CPU,
		"preflight fixture establishes original whole-buffer authority");
	result |= Check(resources.UpdateBuffer(buffer, &replacement,
		sizeof(replacement), offset, mode) == expected &&
		ReadCount(&control.updateCalls) == 0,
		"descriptor/mode rejection occurs before any backend update call");
	NativeW3DBufferDescription after;
	result |= Check(resources.DescribeBuffer(buffer, &after) ==
		RENDER_RESULT_OK && after.authority == before.authority &&
		after.authorityEpoch == before.authorityEpoch &&
		!resources.HasBufferAuthorityFailure(buffer) &&
		device.BufferEquals(buffer, original, sizeof(original)),
		"preflight rejection preserves unchanged bytes, authority, and epoch");
	GpuHandle validated;
	if ((binding & RENDER_BUFFER_VERTEX) != 0)
	{
		result |= Check(resources.AcquireVertexBufferRange(buffer,
			descriptor.stride, 0, 0, 4, &validated) == RENDER_RESULT_OK &&
			validated == buffer,
			"preflight rejection preserves the exact committed vertex range");
	}
	if ((binding & RENDER_BUFFER_INDEX) != 0)
	{
		result |= Check(resources.AcquireIndexBufferRange(buffer,
			RENDER_FORMAT_R32_UINT, 0, 0, 4, &validated) == RENDER_RESULT_OK &&
			validated == buffer,
			"preflight rejection preserves the exact committed index range");
	}
	if (usage == RENDER_USAGE_IMMUTABLE)
	{
		result |= Check(device.recoverDevice() == RENDER_RESULT_OK &&
			host.ReplaceContext(device.immediateContext()) == RENDER_RESULT_OK &&
			resources.RestoreStaticBuffersAfterRecovery() == RENDER_RESULT_OK &&
			resources.AcquireVertexBufferRange(buffer, descriptor.stride, 0, 0, 4,
				&validated) == RENDER_RESULT_OK && validated == buffer &&
			device.BufferEquals(buffer, original, sizeof(original)) &&
			ReadCount(&control.updateCalls) == 0,
			"rejected immutable update retains original creation proof through recovery");
	}
	result |= Check(resources.Shutdown() == RENDER_RESULT_OK &&
		host.Detach() == RENDER_RESULT_OK && device.LiveCount() == 0,
		"preflight fixture releases its exact resource and borrowed host");
	if (result != 0)
	{
		std::fprintf(stderr, "Preflight case: %s\n", caseName);
	}
	return result;
}

int TestBufferUpdatePreflight()
{
	int result = 0;
	result |= TestRejectedBufferUpdate("immutable", RENDER_USAGE_IMMUTABLE,
		RENDER_BUFFER_VERTEX, RENDER_BUFFER_UPDATE_PRESERVE, 0,
		RENDER_RESULT_UNSUPPORTED);
	result |= TestRejectedBufferUpdate("invalid-mode", RENDER_USAGE_DYNAMIC,
		RENDER_BUFFER_VERTEX, static_cast<RenderBufferUpdateMode>(
			RENDER_BUFFER_UPDATE_NO_OVERWRITE + 1), 0,
		RENDER_RESULT_INVALID_ARGUMENT);
	result |= TestRejectedBufferUpdate("discard-offset", RENDER_USAGE_DYNAMIC,
		RENDER_BUFFER_VERTEX, RENDER_BUFFER_UPDATE_DISCARD, sizeof(unsigned int),
		RENDER_RESULT_INVALID_ARGUMENT);
	result |= TestRejectedBufferUpdate("default-discard", RENDER_USAGE_DEFAULT,
		RENDER_BUFFER_VERTEX, RENDER_BUFFER_UPDATE_DISCARD, 0,
		RENDER_RESULT_INVALID_ARGUMENT);
	result |= TestRejectedBufferUpdate("default-no-overwrite", RENDER_USAGE_DEFAULT,
		RENDER_BUFFER_INDEX, RENDER_BUFFER_UPDATE_NO_OVERWRITE, 0,
		RENDER_RESULT_INVALID_ARGUMENT);
	result |= TestRejectedBufferUpdate("constant-discard", RENDER_USAGE_DYNAMIC,
		RENDER_BUFFER_CONSTANT, RENDER_BUFFER_UPDATE_DISCARD, 0,
		RENDER_RESULT_INVALID_ARGUMENT);
	result |= TestRejectedBufferUpdate("constant-no-overwrite", RENDER_USAGE_DYNAMIC,
		RENDER_BUFFER_CONSTANT, RENDER_BUFFER_UPDATE_NO_OVERWRITE, 0,
		RENDER_RESULT_INVALID_ARGUMENT);
	result |= TestRejectedBufferUpdate("combined-binding", RENDER_USAGE_DYNAMIC,
		RENDER_BUFFER_VERTEX | RENDER_BUFFER_INDEX,
		RENDER_BUFFER_UPDATE_NO_OVERWRITE, 0, RENDER_RESULT_INVALID_ARGUMENT);

	// A valid request's generic backend rejection must remain fail closed, even
	// for an error code also used by the preflight checks above. Error codes alone
	// are not a guarantee that a backend operation left its content untouched.
	const RenderResult failures[] = { RENDER_RESULT_FAILED,
		RENDER_RESULT_INVALID_ARGUMENT, RENDER_RESULT_UNSUPPORTED };
	for (unsigned int index = 0; index < sizeof(failures) / sizeof(failures[0]); ++index)
	{
		FakeRenderControl control;
		FakeRenderDevice device(true, &control);
		NativeW3DResourceHost host(2);
		NativeW3DResources resources(2);
		const unsigned int original[] = { 1, 2, 3, 4 };
		const unsigned int replacement = 17;
		BufferDescriptor descriptor;
		descriptor.byteCount = sizeof(original);
		descriptor.stride = sizeof(original[0]);
		descriptor.binding = RENDER_BUFFER_VERTEX;
		descriptor.usage = RENDER_USAGE_DYNAMIC;
		GpuHandle buffer;
		NativeW3DBufferDescription before;
		result |= Check(host.Attach(&device, device.immediateContext()) ==
			RENDER_RESULT_OK && resources.BindHost(&host) == RENDER_RESULT_OK &&
			resources.CreateBuffer(descriptor, original, sizeof(original), &buffer) ==
				RENDER_RESULT_OK &&
			resources.DescribeBuffer(buffer, &before) == RENDER_RESULT_OK,
			"valid backend-failure fixture initializes its buffer");
		InterlockedExchange(&control.failUpdate, 1);
		InterlockedExchange(&control.failUpdateResult, failures[index]);
		NativeW3DBufferDescription after;
		GpuHandle validated = buffer;
		result |= Check(resources.UpdateBuffer(buffer, &replacement,
			sizeof(replacement), 0, RENDER_BUFFER_UPDATE_PRESERVE) ==
				failures[index] && ReadCount(&control.updateCalls) == 1 &&
			resources.HasBufferAuthorityFailure(buffer) &&
			resources.DescribeBuffer(buffer, &after) == RENDER_RESULT_OK &&
			after.authority == NATIVE_W3D_CONTENT_INVALID &&
			after.authorityEpoch > before.authorityEpoch &&
			resources.AcquireVertexBufferRange(buffer, descriptor.stride, 0, 0, 4,
				&validated) == RENDER_RESULT_INVALID_ARGUMENT &&
			!validated.isValid(),
			"a valid request's generic backend failure still revokes exact authority");
		result |= Check(resources.Shutdown() == RENDER_RESULT_OK &&
			host.Detach() == RENDER_RESULT_OK && device.LiveCount() == 0,
			"valid backend-failure fixture retains exact cleanup ownership");
	}
	return result;
}

int TestRetiredBufferPublication()
{
	int result = 0;
	FakeRenderControl control;
	FakeRenderDevice device(true, &control);
	NativeW3D2 product;
	result |= Check(product.AttachBackend(&device, device.immediateContext()) ==
		RENDER_RESULT_OK, "retirement fixture attaches the real product resource table");
	NativeW3DResources &resources = product.Resources();
	const unsigned int vertices[] = { 1, 2, 3, 4 };
	const unsigned short indices[] = { 0, 1, 2 };
	BufferDescriptor vertexDescriptor;
	vertexDescriptor.byteCount = sizeof(vertices);
	vertexDescriptor.stride = sizeof(vertices[0]);
	vertexDescriptor.binding = RENDER_BUFFER_VERTEX;
	vertexDescriptor.usage = RENDER_USAGE_DYNAMIC;
	BufferDescriptor indexDescriptor;
	indexDescriptor.byteCount = sizeof(indices);
	indexDescriptor.stride = sizeof(indices[0]);
	indexDescriptor.binding = RENDER_BUFFER_INDEX;
	indexDescriptor.usage = RENDER_USAGE_DYNAMIC;
	GpuHandle retiredVertex;
	GpuHandle retiredIndex;
	GpuHandle liveVertex;
	GpuHandle liveIndex;
	result |= Check(resources.CreateBuffer(vertexDescriptor, vertices,
		sizeof(vertices), &retiredVertex) == RENDER_RESULT_OK &&
		resources.CreateBuffer(indexDescriptor, indices, sizeof(indices),
			&retiredIndex) == RENDER_RESULT_OK &&
		resources.CreateBuffer(vertexDescriptor, vertices, sizeof(vertices),
			&liveVertex) == RENDER_RESULT_OK &&
		resources.CreateBuffer(indexDescriptor, indices, sizeof(indices),
			&liveIndex) == RENDER_RESULT_OK,
		"retirement fixture initializes independent vertex and index slots");
	NativeDrawPacket vertexPacket;
	vertexPacket.vertexBuffer = retiredVertex;
	vertexPacket.vertexStride = vertexDescriptor.stride;
	vertexPacket.vertexLayout.stride = vertexDescriptor.stride;
	vertexPacket.vertexCount = 3;
	vertexPacket.indexed = true;
	vertexPacket.indexBuffer = liveIndex;
	vertexPacket.indexFormat = RENDER_FORMAT_R16_UINT;
	vertexPacket.indexCount = 3;
	NativeDrawPacket indexPacket = vertexPacket;
	indexPacket.vertexBuffer = liveVertex;
	indexPacket.indexBuffer = retiredIndex;
	LegacyLogicalState state;
	result |= Check(product.Renderer().BeginFrame() == RENDER_RESULT_OK &&
		product.Renderer().Submit(resources, state, vertexPacket) ==
			RENDER_RESULT_OK &&
		product.Renderer().Submit(resources, state, indexPacket) ==
			RENDER_RESULT_OK &&
		product.Renderer().EndFrame(false) == RENDER_RESULT_OK,
		"retirement fixture admits both exact submission ranges before retirement");
	NativeW3DBufferDescription vertexBefore;
	NativeW3DBufferDescription indexBefore;
	result |= Check(resources.DescribeBuffer(retiredVertex, &vertexBefore) ==
		RENDER_RESULT_OK &&
		resources.DescribeBuffer(retiredIndex, &indexBefore) == RENDER_RESULT_OK,
		"retirement fixture records the original authority epochs");
	const unsigned int destroysBefore = device.DestroyCount();
	device.FailDestroy(true);
	result |= Check(resources.RetireBuffer(retiredVertex) &&
		resources.RetireBuffer(retiredIndex) && !resources.IsValid(retiredVertex) &&
		!resources.IsValid(retiredIndex) && resources.IsValid(liveVertex) &&
		resources.IsValid(liveIndex) && device.LiveCount() == 4 &&
		device.DestroyCount() == destroysBefore,
		"backend refusal transfers only the exact retired slots to shutdown cleanup");
	NativeW3DBufferDescription retiredVertexDescription;
	NativeW3DBufferDescription retiredIndexDescription;
	result |= Check(resources.DescribeBuffer(retiredVertex,
		&retiredVertexDescription) == RENDER_RESULT_OK &&
		resources.DescribeBuffer(retiredIndex, &retiredIndexDescription) ==
			RENDER_RESULT_OK &&
		retiredVertexDescription.authority == NATIVE_W3D_CONTENT_INVALID &&
		retiredIndexDescription.authority == NATIVE_W3D_CONTENT_INVALID &&
		retiredVertexDescription.authorityEpoch > vertexBefore.authorityEpoch &&
		retiredIndexDescription.authorityEpoch > indexBefore.authorityEpoch,
		"retirement invalidates content and advances each exact authority epoch");
	const long updatesBefore = ReadCount(&control.updateCalls);
	const unsigned int replacementVertices[] = { 11, 12, 13, 14 };
	const unsigned short replacementIndices[] = { 2, 1, 0 };
	result |= Check(resources.UpdateBuffer(retiredVertex, replacementVertices,
		sizeof(replacementVertices), 0, RENDER_BUFFER_UPDATE_DISCARD) ==
			RENDER_RESULT_INVALID_ARGUMENT &&
		ReadCount(&control.updateCalls) == updatesBefore,
		"a retired vertex slot rejects resurrection before backend upload");
	result |= Check(resources.UpdateBuffer(retiredIndex, replacementIndices,
		sizeof(replacementIndices), 0, RENDER_BUFFER_UPDATE_DISCARD) ==
			RENDER_RESULT_INVALID_ARGUMENT &&
		ReadCount(&control.updateCalls) == updatesBefore,
		"a retired index slot rejects resurrection before backend upload");
	NativeW3DBufferDescription vertexAfter;
	NativeW3DBufferDescription indexAfter;
	result |= Check(resources.DescribeBuffer(retiredVertex, &vertexAfter) ==
		RENDER_RESULT_OK &&
		resources.DescribeBuffer(retiredIndex, &indexAfter) == RENDER_RESULT_OK &&
		vertexAfter.authority == NATIVE_W3D_CONTENT_INVALID &&
		indexAfter.authority == NATIVE_W3D_CONTENT_INVALID &&
		vertexAfter.authorityEpoch == retiredVertexDescription.authorityEpoch &&
		indexAfter.authorityEpoch == retiredIndexDescription.authorityEpoch &&
		device.BufferEquals(retiredVertex, vertices, sizeof(vertices)) &&
		device.BufferEquals(retiredIndex, indices, sizeof(indices)) &&
		device.BufferEquals(liveVertex, vertices, sizeof(vertices)) &&
		device.BufferEquals(liveIndex, indices, sizeof(indices)),
		"rejected retired uploads preserve epochs, backend bytes, and unrelated authority");
	GpuHandle validated = liveVertex;
	result |= Check(resources.AcquireVertexBufferRange(retiredVertex,
		vertexDescriptor.stride, 0, 0, 3, &validated) ==
			RENDER_RESULT_INVALID_ARGUMENT && !validated.isValid(),
		"retired vertex exact acquisition clears its output after resurrection rejection");
	validated = liveIndex;
	result |= Check(resources.AcquireIndexBufferRange(retiredIndex,
		RENDER_FORMAT_R16_UINT, 0, 0, 3, &validated) ==
			RENDER_RESULT_INVALID_ARGUMENT && !validated.isValid(),
		"retired index exact acquisition clears its output after resurrection rejection");
	result |= Check(product.Renderer().BeginFrame() == RENDER_RESULT_OK,
		"retirement fixture opens a submission-validation frame");
	result |= Check(product.Renderer().Submit(resources, state, vertexPacket) ==
		RENDER_RESULT_INVALID_ARGUMENT,
		"submission range validation rejects a retired vertex with a live index");
	result |= Check(product.Renderer().Submit(resources, state, indexPacket) ==
		RENDER_RESULT_INVALID_ARGUMENT,
		"submission range validation rejects a retired index with a live vertex");
	NativeDrawPacket livePacket = vertexPacket;
	livePacket.vertexBuffer = liveVertex;
	result |= Check(product.Renderer().Submit(resources, state, livePacket) ==
		RENDER_RESULT_OK &&
		product.Renderer().EndFrame(false) == RENDER_RESULT_OK,
		"retirement keeps unrelated initialized submission ranges usable");
	result |= Check(product.Shutdown() == RENDER_RESULT_FAILED &&
		device.LiveCount() == 4 && device.DestroyCount() == destroysBefore,
		"shutdown refusal retains all native allocations for a later exact retry");
	device.FailDestroy(false);
	result |= Check(product.Shutdown() == RENDER_RESULT_OK &&
		device.LiveCount() == 0 && device.DestroyCount() == destroysBefore + 4 &&
		device.isOperational() && !resources.IsValid(retiredVertex) &&
		!resources.IsValid(retiredIndex),
		"later shutdown destroys retired allocations without shutting down the borrowed backend");
	return result;
}

int TestRawHandlesAcrossFreshBackends()
{
	int result = 0;
	NativeW3DResources resources(2);
	FakeRenderDevice firstDevice;
	NativeW3DResourceHost firstHost(2);
	BufferDescriptor bufferDescriptor;
	bufferDescriptor.byteCount = sizeof(unsigned int);
	bufferDescriptor.stride = sizeof(unsigned int);
	bufferDescriptor.binding = RENDER_BUFFER_VERTEX;
	bufferDescriptor.usage = RENDER_USAGE_DYNAMIC;
	TextureDescriptor textureDescriptor;
	textureDescriptor.width = 1;
	textureDescriptor.height = 1;
	textureDescriptor.format = RENDER_FORMAT_R8G8B8A8_UNORM;
	textureDescriptor.binding = RENDER_TEXTURE_SHADER_RESOURCE;
	textureDescriptor.usage = RENDER_USAGE_DEFAULT;
	const unsigned int firstValue = 7;
	GpuHandle staleBuffer;
	GpuHandle staleTexture;
	result |= Check(firstHost.Attach(&firstDevice,
		firstDevice.immediateContext()) == RENDER_RESULT_OK &&
		resources.BindHost(&firstHost) == RENDER_RESULT_OK &&
		resources.CreateBuffer(bufferDescriptor, &firstValue,
			sizeof(firstValue), &staleBuffer) == RENDER_RESULT_OK &&
		resources.CreateTexture(textureDescriptor, 0, 0,
			&staleTexture) == RENDER_RESULT_OK,
		"first backend publishes raw buffer and texture handles");
	result |= Check(resources.Shutdown() == RENDER_RESULT_OK &&
		firstHost.Detach() == RENDER_RESULT_OK &&
		firstDevice.LiveCount() == 0,
		"first backend drains before resource table rebind");

	FakeRenderDevice secondDevice;
	NativeW3DResourceHost secondHost(2);
	const unsigned int secondValue = 19;
	GpuHandle currentBuffer;
	GpuHandle currentTexture;
	result |= Check(secondHost.Attach(&secondDevice,
		secondDevice.immediateContext()) == RENDER_RESULT_OK &&
		resources.BindHost(&secondHost) == RENDER_RESULT_OK &&
		resources.CreateBuffer(bufferDescriptor, &secondValue,
			sizeof(secondValue), &currentBuffer) == RENDER_RESULT_OK &&
		resources.CreateTexture(textureDescriptor, 0, 0,
			&currentTexture) == RENDER_RESULT_OK,
		"same resource table publishes handles from a fresh backend");
	result |= Check(staleBuffer.index() == currentBuffer.index() &&
		staleTexture.index() == currentTexture.index() &&
		staleBuffer != currentBuffer && staleTexture != currentTexture &&
		!resources.IsValid(staleBuffer) &&
		!resources.IsValid(staleTexture) &&
		!resources.Destroy(staleBuffer) && !resources.Destroy(staleTexture) &&
		resources.UpdateBuffer(staleBuffer, &firstValue,
			sizeof(firstValue), 0, RENDER_BUFFER_UPDATE_PRESERVE) ==
			RENDER_RESULT_INVALID_ARGUMENT &&
		secondDevice.BufferEquals(currentBuffer, &secondValue,
			sizeof(secondValue)) &&
		resources.IsValid(currentBuffer) && resources.IsValid(currentTexture) &&
		secondDevice.LiveCount() == 2,
		"stale raw handles cannot inspect or mutate same-index resources after rebind");
	result |= Check(resources.Shutdown() == RENDER_RESULT_OK &&
		secondHost.Detach() == RENDER_RESULT_OK &&
		secondDevice.LiveCount() == 0,
		"fresh backend resources drain normally");
	return result;
}
}

int main()
{
	char benchmark[2] = {0};
	if (GetEnvironmentVariableA("RTS_BUFFER_PUBLICATION_BENCH", benchmark,
		sizeof(benchmark)) == 1 && benchmark[0] == '1')
		return BenchmarkThreadedAdjacentBufferPublication();
	int result = 0;
	result |= TestBufferUpdatePreflight();
	result |= TestRetiredBufferPublication();
	result |= TestRawHandlesAcrossFreshBackends();
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
	const unsigned short indexedValues[3] = { 0, 0, 0 };
	const unsigned short negativeIndexedValues[3] = { 1, 1, 1 };
	BufferDescriptor indexedIndexDescriptor;
	indexedIndexDescriptor.byteCount = sizeof(indexedValues);
	indexedIndexDescriptor.stride = sizeof(unsigned short);
	indexedIndexDescriptor.binding = RENDER_BUFFER_INDEX;
	indexedIndexDescriptor.usage = RENDER_USAGE_IMMUTABLE;
	GpuHandle indexedIndexBuffer;
	GpuHandle negativeIndexedIndexBuffer;
	NativeDrawPacket indexedPacket = partialPacket;
	indexedPacket.indexed = true;
	indexedPacket.indexCount = 3;
	indexedPacket.indexFormat = RENDER_FORMAT_R16_UINT;
	indexedPacket.indexBuffer = indexedIndexBuffer;
	result |= Check(productResources.Resources().CreateBuffer(
		indexedIndexDescriptor, indexedValues, sizeof(indexedValues),
		&indexedIndexBuffer) == RENDER_RESULT_OK,
		"product seam creates an initialized R16 index fixture");
	result |= Check(productResources.Resources().CreateBuffer(
		indexedIndexDescriptor, negativeIndexedValues,
		sizeof(negativeIndexedValues), &negativeIndexedIndexBuffer) ==
		RENDER_RESULT_OK,
		"product seam creates the negative-base R16 index fixture");
	indexedPacket.indexBuffer = indexedIndexBuffer;
	LegacyLogicalState indexedState;
	const auto SubmitIndexedPacket = [&](const NativeDrawPacket &packet,
		RenderResult expected, const char *message) {
		const RenderResult begin = productResources.Renderer().BeginFrame();
		const RenderResult submit = begin == RENDER_RESULT_OK ?
			productResources.Renderer().Submit(productResources.Resources(),
				indexedState, packet) : RENDER_RESULT_INVALID_ARGUMENT;
		const RenderResult end = begin == RENDER_RESULT_OK ?
			productResources.Renderer().EndFrame(false) :
			RENDER_RESULT_INVALID_ARGUMENT;
		result |= Check(begin == RENDER_RESULT_OK && submit == expected &&
			end == RENDER_RESULT_OK, message);
	};
	indexedPacket.baseVertex = 0;
	indexedPacket.minimumVertexIndex = 0;
	SubmitIndexedPacket(indexedPacket, RENDER_RESULT_OK,
		"indexed range accepts initialized vertex zero with zero base");
	indexedPacket.baseVertex = 1;
	SubmitIndexedPacket(indexedPacket, RENDER_RESULT_INVALID_ARGUMENT,
		"indexed range rejects a base before the second vertex is initialized");
	result |= Check(productResources.Resources().UpdateBuffer(
		partialProductBuffer, latestBytes + 1, sizeof(latestBytes[0]),
		sizeof(latestBytes[0]), RENDER_BUFFER_UPDATE_PRESERVE) == RENDER_RESULT_OK,
		"product seam initializes the second vertex slot");
	indexedPacket.baseVertex = 1;
	SubmitIndexedPacket(indexedPacket, RENDER_RESULT_OK,
		"indexed range accepts the initialized positive base");
	indexedPacket.baseVertex = -1;
	indexedPacket.minimumVertexIndex = 1;
	indexedPacket.indexBuffer = negativeIndexedIndexBuffer;
	SubmitIndexedPacket(indexedPacket, RENDER_RESULT_OK,
		"indexed range accepts a legal negative base cancellation");
	indexedPacket.baseVertex = -2;
	indexedPacket.minimumVertexIndex = 1;
	SubmitIndexedPacket(indexedPacket, RENDER_RESULT_INVALID_ARGUMENT,
		"indexed range rejects a negative effective start");
	indexedPacket.baseVertex = INT_MAX;
	indexedPacket.minimumVertexIndex = UINT_MAX;
	SubmitIndexedPacket(indexedPacket, RENDER_RESULT_INVALID_ARGUMENT,
		"indexed range rejects signed base and minimum overflow");
	if (indexedIndexBuffer.isValid())
		result |= Check(productResources.Resources().Destroy(indexedIndexBuffer),
			"product seam destroys the indexed range index fixture");
	if (negativeIndexedIndexBuffer.isValid())
		result |= Check(productResources.Resources().Destroy(
			negativeIndexedIndexBuffer),
			"product seam destroys the negative-base index fixture");
	result |= Check(productResources.Renderer().Shutdown() == RENDER_RESULT_OK &&
		productResources.Shutdown() == RENDER_RESULT_OK &&
		device.LiveCount() == 0 && device.isOperational(),
		"public borrowed shutdown succeeds after EndFrame and product cleanup preserves backend ownership");
	result |= TestThreadedResourceCompletion();
	result |= TestThreadedAdjacentBufferPublication();
	result |= TestRejectedAdjacentBufferPublication();
	result |= TestResourceLookupHints();
	result |= TestThreadedNativeBufferOwnerFailureRecovery();
	result |= TestThreadedBetweenFrameBufferUpdates();
	result |= TestUnrelatedFrameFailureDoesNotInvalidateResourceMutation();
	result |= TestSplitPacketResourceFenceReportsOwnerFailure();
	return result;
}
