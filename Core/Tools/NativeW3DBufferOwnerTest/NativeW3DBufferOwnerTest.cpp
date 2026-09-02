#include "nativew3dbufferowner.h"
#include "nativew3dbuffercompat.h"
#include "dx8indexbuffer.h"
#include "dx8vertexbuffer.h"
#include "Renderer/NativeW3DRenderer.h"
#include "nativew3dline.h"

#include <cstdio>
#include <cstring>
#include <new>
#include <vector>

namespace
{
using namespace rts::render;

int Check(bool condition, const char *message)
{
	if (!condition)
	{
		std::fprintf(stderr, "FAILED: %s\n", message);
		return 1;
	}
	return 0;
}

int RunNativeBufferPublicationContract()
{
	unsigned char byte = 0x5a;
	DX8VertexBufferClass *opaqueBuffer =
		reinterpret_cast<DX8VertexBufferClass *>(&byte);
	const unsigned int binding = RENDER_BUFFER_VERTEX;
	const RenderBufferUpdateMode mode = RENDER_BUFFER_UPDATE_NO_OVERWRITE;
	int result = 0;

	// NativeW3DBufferOwner::Unlock is authoritative for the upload.  This
	// compatibility notification must validate its old call shape without
	// allocating or dereferencing a native buffer.
	result |= Check(!Publish_Render_Buffer_Change(nullptr, binding, &byte,
		sizeof(byte), 0, mode, 7),
		"native publication rejects a null buffer");
	result |= Check(!Publish_Render_Buffer_Change(opaqueBuffer, binding,
		nullptr, sizeof(byte), 0, mode, 7),
		"native publication rejects null source data");
	result |= Check(!Publish_Render_Buffer_Change(opaqueBuffer, binding,
		&byte, 0, 0, mode, 7),
		"native publication rejects an empty source range");
	result |= Check(Publish_Render_Buffer_Change(opaqueBuffer, binding,
		&byte, sizeof(byte), 3, mode, 7),
		"native publication accepts a valid compatibility notification");
	return result;
}

class FakeRenderDevice;

class FakeRenderContext : public IRenderContext
{
public:
	explicit FakeRenderContext(FakeRenderDevice *device) :
		m_device(device), m_drawCount(0) {}

	RenderResult beginFrame() override { return RENDER_RESULT_OK; }
	RenderResult updateBuffer(GpuHandle buffer, const void *data,
		size_t byteCount, size_t destinationOffset,
		RenderBufferUpdateMode mode) override;
	RenderResult clear(const RenderFloat4 &, float, unsigned int) override
		{ return RENDER_RESULT_UNSUPPORTED; }
	RenderResult clearTargets(unsigned int, const RenderFloat4 &, float,
		unsigned int) override { return RENDER_RESULT_UNSUPPORTED; }
	RenderResult setRenderTargets(const RenderTargetBinding &) override
		{ return RENDER_RESULT_UNSUPPORTED; }
	RenderResult setRenderTargets(GpuHandle, GpuHandle) override
		{ return RENDER_RESULT_UNSUPPORTED; }
	RenderResult setViewport(float, float, float, float, float, float) override
		{ return RENDER_RESULT_UNSUPPORTED; }
	RenderResult setLegacyState(const LegacyLogicalState &,
		LegacyVertexFormat, unsigned int) override
		{ return RENDER_RESULT_UNSUPPORTED; }
	RenderResult setLegacyStateForLayout(const LegacyLogicalState &,
		const LegacyVertexLayout &, unsigned int) override
		{ return RENDER_RESULT_UNSUPPORTED; }
	RenderResult setVertexBuffer(GpuHandle, unsigned int, unsigned int) override
		{ return RENDER_RESULT_UNSUPPORTED; }
	RenderResult setIndexBuffer(GpuHandle, RenderFormat, unsigned int) override
		{ return RENDER_RESULT_UNSUPPORTED; }
	RenderResult setTexture(unsigned int, GpuHandle) override
		{ return RENDER_RESULT_UNSUPPORTED; }
	RenderResult setPrimitiveTopology(RenderPrimitiveTopology) override
		{ return RENDER_RESULT_UNSUPPORTED; }
	RenderResult draw(unsigned int, unsigned int) override
		{ ++m_drawCount; return RENDER_RESULT_UNSUPPORTED; }
	RenderResult drawIndexed(unsigned int, unsigned int, int) override
		{ ++m_drawCount; return RENDER_RESULT_UNSUPPORTED; }
	RenderResult endFrame() override { return RENDER_RESULT_OK; }
	unsigned int DrawCount() const { return m_drawCount; }

private:
	FakeRenderDevice *m_device;
	unsigned int m_drawCount;
};

class FakeRenderDevice : public IRenderDevice
{
public:
	struct Buffer
	{
		Buffer() : live(false), handle(), descriptor(), bytes() {}
		bool live;
		GpuHandle handle;
		BufferDescriptor descriptor;
		std::vector<unsigned char> bytes;
	};

	FakeRenderDevice() : m_allocator(16), m_context(this), m_operational(true),
		m_failCreate(false), m_failUpdate(false), m_failDestroy(false),
		m_failCreateAttempt(0),
		m_failUpdateAttempt(0), m_createAttemptCount(0), m_updateAttemptCount(0),
		m_createCount(0), m_destroyCount(0), m_updateCount(0), m_lastOffset(0),
		m_lastBytes(0), m_lastMode(RENDER_BUFFER_UPDATE_PRESERVE), m_buffers(16)
	{
	}

	RenderBackend backend() const override { return RENDER_BACKEND_D3D11; }
	bool isOperational() const override { return m_operational; }
	RenderResult initialize(const RenderDeviceParameters &) override
		{ return RENDER_RESULT_INVALID_ARGUMENT; }
	void shutdown() override { m_operational = false; }
	IRenderContext *immediateContext() override { return &m_context; }
	RenderResult createBuffer(const BufferDescriptor &descriptor,
		const void *initialData, size_t initialDataBytes, GpuHandle *buffer) override
	{
		if (buffer == nullptr)
		{
			return RENDER_RESULT_INVALID_ARGUMENT;
		}
		*buffer = GpuHandle();
		++m_createAttemptCount;
		if (m_failCreate || m_createAttemptCount == m_failCreateAttempt)
		{
			m_failCreateAttempt = 0;
			return RENDER_RESULT_FAILED;
		}
		if (!m_operational || descriptor.byteCount == 0 ||
			(initialDataBytes != 0 && (initialData == nullptr ||
			 initialDataBytes != descriptor.byteCount)))
		{
			return RENDER_RESULT_INVALID_ARGUMENT;
		}
		const GpuHandle created = m_allocator.allocate();
		if (!created.isValid() || created.index() >= m_buffers.size())
		{
			return RENDER_RESULT_OUT_OF_MEMORY;
		}
		Buffer &slot = m_buffers[created.index()];
		slot.live = true;
		slot.handle = created;
		slot.descriptor = descriptor;
		// Do not let a fake's zero-fill hide an owner that fails to establish the
		// initial image.  Real D3D11 dynamic buffers have undefined bytes when
		// created without initial data, so partial PRESERVE must be safe even
		// against a nonzero backend allocation.
		slot.bytes.assign(descriptor.byteCount, initialData == nullptr ? 0xA5 : 0);
		if (initialData != nullptr)
		{
			std::memcpy(slot.bytes.data(), initialData, initialDataBytes);
		}
		++m_createCount;
		*buffer = created;
		return RENDER_RESULT_OK;
	}
	RenderResult createTexture(const TextureDescriptor &,
		const TextureSubresourceData *, unsigned int, GpuHandle *) override
		{ return RENDER_RESULT_UNSUPPORTED; }
	RenderResult refreshTexture(GpuHandle, const TextureDescriptor &,
		const TextureSubresourceData *, unsigned int) override
		{ return RENDER_RESULT_UNSUPPORTED; }
	RenderResult copyActiveColorTargetToTexture(GpuHandle) override
		{ return RENDER_RESULT_UNSUPPORTED; }
	bool destroyResource(GpuHandle resource) override
	{
		if (m_failDestroy)
		{
			return false;
		}
		Buffer *slot = Find(resource);
		if (slot == nullptr || !m_allocator.release(resource))
		{
			return false;
		}
		slot->live = false;
		slot->bytes.clear();
		++m_destroyCount;
		return true;
	}
	RenderResult recoverDevice() override
	{
		m_operational = true;
		for (size_t index = 0; index < m_buffers.size(); ++index)
		{
			Buffer &slot = m_buffers[index];
			if (slot.live && !slot.bytes.empty())
			{
				std::memset(&slot.bytes[0], 0, slot.bytes.size());
			}
		}
		return RENDER_RESULT_OK;
	}
	RenderResult resize(unsigned int, unsigned int) override
		{ return RENDER_RESULT_OK; }
	RenderResult present() override { return RENDER_RESULT_OK; }
	RenderResult getBackBufferInfo(RenderBackBufferInfo *) const override
		{ return RENDER_RESULT_UNSUPPORTED; }
	RenderResult captureBackBuffer(void *, size_t, size_t,
		RenderFormat *) override { return RENDER_RESULT_UNSUPPORTED; }
	RenderResult getDebugValidationErrorCount(unsigned int *count) const override
	{
		if (count == nullptr)
		{
			return RENDER_RESULT_INVALID_ARGUMENT;
		}
		*count = 0;
		return RENDER_RESULT_OK;
	}
	RenderResult reportDebugLiveObjects() override { return RENDER_RESULT_OK; }

	RenderResult Update(GpuHandle handle, const void *data, size_t byteCount,
		size_t destinationOffset, RenderBufferUpdateMode mode)
	{
		++m_updateAttemptCount;
		if (m_failUpdate || m_updateAttemptCount == m_failUpdateAttempt)
		{
			m_failUpdateAttempt = 0;
			return RENDER_RESULT_FAILED;
		}
		Buffer *slot = Find(handle);
		if (slot == nullptr || data == nullptr || byteCount == 0 ||
			destinationOffset > slot->bytes.size() ||
			byteCount > slot->bytes.size() - destinationOffset ||
			(mode == RENDER_BUFFER_UPDATE_DISCARD && destinationOffset != 0))
		{
			return RENDER_RESULT_INVALID_ARGUMENT;
		}
		std::memcpy(slot->bytes.data() + destinationOffset, data, byteCount);
		++m_updateCount;
		m_lastOffset = destinationOffset;
		m_lastBytes = byteCount;
		m_lastMode = mode;
		return RENDER_RESULT_OK;
	}

	void FailCreate(bool fail) { m_failCreate = fail; }
	void FailUpdate(bool fail) { m_failUpdate = fail; }
	void FailDestroy(bool fail) { m_failDestroy = fail; }
	void FailCreateOnAttempt(unsigned int attempt)
		{ m_failCreateAttempt = attempt; }
	void FailUpdateOnAttempt(unsigned int attempt)
		{ m_failUpdateAttempt = attempt; }
	unsigned int CreateAttemptCount() const { return m_createAttemptCount; }
	unsigned int UpdateAttemptCount() const { return m_updateAttemptCount; }
	unsigned int CreateCount() const { return m_createCount; }
	unsigned int DestroyCount() const { return m_destroyCount; }
	unsigned int UpdateCount() const { return m_updateCount; }
	unsigned int DrawCount() const { return m_context.DrawCount(); }
	size_t LastOffset() const { return m_lastOffset; }
	size_t LastBytes() const { return m_lastBytes; }
	RenderBufferUpdateMode LastMode() const { return m_lastMode; }
	unsigned int LiveCount() const { return m_allocator.liveCount(); }
	bool BufferEquals(GpuHandle handle, const void *bytes,
		size_t byteCount) const
	{
		const Buffer *slot = Find(handle);
		return slot != nullptr && bytes != nullptr &&
			slot->bytes.size() == byteCount &&
			std::memcmp(&slot->bytes[0], bytes, byteCount) == 0;
	}

private:
	Buffer *Find(GpuHandle handle)
	{
		if (!handle.isValid() || handle.index() >= m_buffers.size())
		{
			return nullptr;
		}
		Buffer &slot = m_buffers[handle.index()];
		return slot.live && slot.handle == handle ? &slot : nullptr;
	}
	const Buffer *Find(GpuHandle handle) const
	{
		if (!handle.isValid() || handle.index() >= m_buffers.size())
		{
			return nullptr;
		}
		const Buffer &slot = m_buffers[handle.index()];
		return slot.live && slot.handle == handle ? &slot : nullptr;
	}

	GpuHandleAllocator m_allocator;
	FakeRenderContext m_context;
	bool m_operational;
	bool m_failCreate;
	bool m_failUpdate;
	bool m_failDestroy;
	unsigned int m_failCreateAttempt;
	unsigned int m_failUpdateAttempt;
	unsigned int m_createAttemptCount;
	unsigned int m_updateAttemptCount;
	unsigned int m_createCount;
	unsigned int m_destroyCount;
	unsigned int m_updateCount;
	size_t m_lastOffset;
	size_t m_lastBytes;
	RenderBufferUpdateMode m_lastMode;
	std::vector<Buffer> m_buffers;
};

RenderResult FakeRenderContext::updateBuffer(GpuHandle buffer,
	const void *data, size_t byteCount, size_t destinationOffset,
	RenderBufferUpdateMode mode)
{
	return m_device->Update(buffer, data, byteCount, destinationOffset, mode);
}

void Fill(void *data, size_t byteCount, unsigned char value)
{
	std::memset(data, value, byteCount);
}

int RunNativeLine3DDestroyFailureContract()
{
	using namespace rts::render;
	FakeRenderDevice device;
	NativeW3DResourceHost host(8);
	NativeW3DResources resources(8);
	NativeW3DRenderer renderer;
	int result = Check(host.Attach(&device, device.immediateContext()) ==
		RENDER_RESULT_OK && resources.BindHost(&host) == RENDER_RESULT_OK,
		"Line3D failure fixture binds its owner resource table");
	NativeLine3DRenderContext context(&renderer, &resources);
	NativeLine3DBufferSet *buffers =
		new (std::nothrow) NativeLine3DBufferSet;
	result |= Check(buffers != 0,
		"Line3D failure fixture allocates its buffer sidecar");
	if (buffers == 0)
	{
		result |= Check(resources.Shutdown() == RENDER_RESULT_OK &&
			host.Detach() == RENDER_RESULT_OK,
			"Line3D failure fixture unwinds after sidecar allocation failure");
		return result;
	}
	buffers->lineOwned = true;
	unsigned char vertexBytes[sizeof(NativeLine3DVertex) *
		NATIVE_LINE3D_VERTEX_COUNT] = {};
	unsigned char indexBytes[sizeof(unsigned short) *
		NATIVE_LINE3D_INDEX_COUNT] = {};
	BufferDescriptor vertexDescriptor;
	vertexDescriptor.byteCount = sizeof(vertexBytes);
	vertexDescriptor.stride = sizeof(NativeLine3DVertex);
	vertexDescriptor.binding = RENDER_BUFFER_VERTEX;
	vertexDescriptor.usage = RENDER_USAGE_DEFAULT;
	BufferDescriptor indexDescriptor;
	indexDescriptor.byteCount = sizeof(indexBytes);
	indexDescriptor.stride = sizeof(unsigned short);
	indexDescriptor.binding = RENDER_BUFFER_INDEX;
	indexDescriptor.usage = RENDER_USAGE_DEFAULT;
	const RenderResult createResult = resources.CreateBuffer(vertexDescriptor,
		vertexBytes, sizeof(vertexBytes), &buffers->vertexBuffer);
	const RenderResult indexResult = createResult == RENDER_RESULT_OK ?
		resources.CreateBuffer(indexDescriptor, indexBytes, sizeof(indexBytes),
			&buffers->indexBuffer) : RENDER_RESULT_FAILED;
	result |= Check(createResult == RENDER_RESULT_OK &&
		indexResult == RENDER_RESULT_OK,
		"Line3D failure fixture creates both native buffer handles");
	if (createResult != RENDER_RESULT_OK || indexResult != RENDER_RESULT_OK)
	{
		device.FailDestroy(false);
		if (buffers->vertexBuffer.isValid())
		{
			(void)resources.Destroy(buffers->vertexBuffer);
		}
		if (buffers->indexBuffer.isValid())
		{
			(void)resources.Destroy(buffers->indexBuffer);
		}
		delete buffers;
		result |= Check(resources.Shutdown() == RENDER_RESULT_OK &&
			host.Detach() == RENDER_RESULT_OK && device.LiveCount() == 0,
			"Line3D failure fixture unwinds partial buffer creation");
		return result;
	}
	const GpuHandle vertexHandle = buffers->vertexBuffer;
	const GpuHandle indexHandle = buffers->indexBuffer;
	NativeLine3DGeometry geometry = {};
	LegacyLogicalState state;
	result |= Check(context.SubmitLine3D(geometry, state, buffers) ==
		RENDER_RESULT_INVALID_ARGUMENT && context.PendingLine3DCount() == 1,
		"Line3D failure fixture registers its sidecar before draw rejection");
	if (context.PendingLine3DCount() == 1)
	{
		device.FailDestroy(true);
		context.DrainLine3D();
		result |= Check(context.PendingLine3DCount() == 1 &&
			buffers->vertexBuffer == vertexHandle &&
			buffers->indexBuffer == indexHandle &&
			resources.IsValid(vertexHandle) && resources.IsValid(indexHandle) &&
			device.LiveCount() == 2,
			"failed Line3D destruction retains exact handles for owner retry");
		device.FailDestroy(false);
		context.DrainLine3D();
		result |= Check(context.PendingLine3DCount() == 0 &&
			!resources.IsValid(vertexHandle) && !resources.IsValid(indexHandle) &&
			device.LiveCount() == 0,
			"Line3D owner retry drains retained handles after backend recovery");
		// The successful owner drain deletes line-owned sidecars once both handles
		// have been released.
		buffers = 0;
	}
	else
	{
		device.FailDestroy(false);
		if (buffers->vertexBuffer.isValid())
		{
			(void)resources.Destroy(buffers->vertexBuffer);
		}
		if (buffers->indexBuffer.isValid())
		{
			(void)resources.Destroy(buffers->indexBuffer);
		}
		delete buffers;
		buffers = 0;
	}
	result |= Check(resources.Shutdown() == RENDER_RESULT_OK &&
		host.Detach() == RENDER_RESULT_OK && device.LiveCount() == 0,
		"Line3D failure fixture shuts down after retry completion");
	return result;
}

}

int main()
{
	using namespace rts::render;
	int result = 0;
	result |= RunNativeBufferPublicationContract();

	BufferDescriptor staticDescriptor;
	staticDescriptor.byteCount = 16;
	staticDescriptor.stride = 4;
	staticDescriptor.binding = RENDER_BUFFER_VERTEX;
	staticDescriptor.usage = RENDER_USAGE_DEFAULT;
	NativeW3DBufferOwner unbound;
	result |= Check(unbound.Create(staticDescriptor) ==
		RENDER_RESULT_INVALID_ARGUMENT,
		"an unbound native buffer owner fails closed");
	DX8IndexBufferClass *unboundIndex = NEW_REF(DX8IndexBufferClass,(3));
	DX8VertexBufferClass *unboundVertex = NEW_REF(DX8VertexBufferClass,(
		DX8_FVF_XYZ, 3));
	result |= Check(unboundIndex != nullptr && unboundVertex != nullptr &&
		!unboundIndex->Is_Valid() && !unboundVertex->Is_Valid() &&
		!unboundIndex->Lock_Buffer(0, 0, 0, nullptr) &&
		!unboundVertex->Lock_Buffer(0, 0, 0, nullptr),
		"native compatibility-shaped buffers fail closed without a resource table");
	{
		IndexBufferClass::WriteLockClass indexLock(unboundIndex);
		VertexBufferClass::WriteLockClass vertexLock(unboundVertex);
		result |= Check(!indexLock.Is_Locked() &&
			indexLock.Get_Index_Array() == nullptr && !indexLock.Commit() &&
			!vertexLock.Is_Locked() &&
			vertexLock.Get_Vertex_Array() == nullptr && !vertexLock.Commit(),
			"legacy lock wrappers expose an unavailable native allocation");
	}
	unboundIndex->Release_Ref();
	unboundVertex->Release_Ref();
	{
		DynamicIBAccessClass dynamicIndex(BUFFER_TYPE_DYNAMIC_DX8, 3);
		DynamicVBAccessClass dynamicVertex(BUFFER_TYPE_DYNAMIC_DX8,
			dynamic_fvf_type, 3);
		DynamicIBAccessClass::WriteLockClass indexLock(&dynamicIndex);
		DynamicVBAccessClass::WriteLockClass vertexLock(&dynamicVertex);
		result |= Check(!dynamicIndex.Is_Valid() && !dynamicVertex.Is_Valid() &&
			!indexLock.Is_Locked() && indexLock.Get_Index_Array() == nullptr &&
			!indexLock.Commit() && !vertexLock.Is_Locked() &&
			vertexLock.Get_Formatted_Vertex_Array() == nullptr &&
			!vertexLock.Commit(),
			"dynamic write wrappers fail closed without a native resource table");
	}
	DynamicIBAccessClass::_Deinit();
	DynamicVBAccessClass::_Deinit();

	FakeRenderDevice device;
	NativeW3DResourceHost host(8);
	NativeW3DResources resources(16);
	NativeW3DResources differentResources(2);
	result |= Check(host.Attach(&device, device.immediateContext()) ==
		RENDER_RESULT_OK && resources.BindHost(&host) == RENDER_RESULT_OK &&
		BindNativeW3DBufferResources(&resources) == RENDER_RESULT_OK &&
		BindNativeW3DBufferResources(&differentResources) ==
			RENDER_RESULT_INVALID_ARGUMENT,
		"the native buffer boundary borrows exactly one resource registry");
	device.FailCreate(true);
	DX8IndexBufferClass *failedIndex = NEW_REF(DX8IndexBufferClass,(3));
	DX8VertexBufferClass *failedVertex = NEW_REF(DX8VertexBufferClass,(
		DX8_FVF_XYZ, 3));
	result |= Check(failedIndex != nullptr && failedVertex != nullptr &&
		!failedIndex->Is_Valid() && !failedVertex->Is_Valid() &&
		!failedIndex->Lock_Buffer(0, 0, 0, nullptr) &&
		!failedVertex->Lock_Buffer(0, 0, 0, nullptr),
		"device allocation failure leaves native buffers unavailable");
	failedIndex->Release_Ref();
	failedVertex->Release_Ref();
	{
		DynamicIBAccessClass failedDynamicIndex(BUFFER_TYPE_DYNAMIC_DX8, 3);
		DynamicVBAccessClass failedDynamicVertex(BUFFER_TYPE_DYNAMIC_DX8,
			dynamic_fvf_type, 3);
		result |= Check(!failedDynamicIndex.Is_Valid() &&
			!failedDynamicVertex.Is_Valid(),
			"failed dynamic creation is rejected by both native buffer boundaries");
		result |= Check(device.DrawCount() == 0,
			"a failed-create dynamic draw stops before backend submission");
	}
	DynamicIBAccessClass::_Deinit();
	DynamicVBAccessClass::_Deinit();
	device.FailCreate(false);
	{
		DynamicIBAccessClass failedUpdateIndex(BUFFER_TYPE_DYNAMIC_DX8, 3);
		DynamicVBAccessClass failedUpdateVertex(BUFFER_TYPE_DYNAMIC_DX8,
			dynamic_fvf_type, 3);
		DynamicIBAccessClass::WriteLockClass indexLock(&failedUpdateIndex);
		DynamicVBAccessClass::WriteLockClass vertexLock(&failedUpdateVertex);
		unsigned short *dynamicIndices = indexLock.Get_Index_Array();
		VertexFormatXYZNDUV2 *dynamicVertices =
			vertexLock.Get_Formatted_Vertex_Array();
		result |= Check(indexLock.Is_Locked() && dynamicIndices != nullptr &&
			vertexLock.Is_Locked() && dynamicVertices != nullptr,
			"dynamic failure fixture locks both allocated write ranges");
		if (dynamicIndices != nullptr && dynamicVertices != nullptr)
		{
			dynamicIndices[0] = 0;
			dynamicIndices[1] = 1;
			dynamicIndices[2] = 2;
			std::memset(dynamicVertices, 0,
				3 * failedUpdateVertex.FVF_Info().Get_FVF_Size());
		}
		device.FailUpdate(true);
		const bool indexPublished = indexLock.Commit();
		const bool vertexPublished = vertexLock.Commit();
		result |= Check(!indexPublished && !vertexPublished &&
			!failedUpdateIndex.Is_Valid() && !failedUpdateVertex.Is_Valid(),
			"failed dynamic publication invalidates both native buffers");
		result |= Check(device.DrawCount() == 0,
			"a failed-publication dynamic draw stops before backend submission");
		device.FailUpdate(false);
		DynamicIBAccessClass::WriteLockClass retryIndexLock(&failedUpdateIndex);
		DynamicVBAccessClass::WriteLockClass retryVertexLock(&failedUpdateVertex);
		unsigned short *retryIndices = retryIndexLock.Get_Index_Array();
		VertexFormatXYZNDUV2 *retryVertices =
			retryVertexLock.Get_Formatted_Vertex_Array();
		result |= Check(retryIndexLock.Is_Locked() && retryIndices != nullptr &&
			retryVertexLock.Is_Locked() && retryVertices != nullptr &&
			failedUpdateIndex.Is_Valid() && failedUpdateVertex.Is_Valid(),
			"failed dynamic publication can re-enter through a zero-offset discard");
		if (retryIndices != nullptr && retryVertices != nullptr)
		{
			retryIndices[0] = 2;
			retryIndices[1] = 1;
			retryIndices[2] = 0;
			std::memset(retryVertices, 0,
				3 * failedUpdateVertex.FVF_Info().Get_FVF_Size());
		}
		result |= Check(retryIndexLock.Commit() && retryVertexLock.Commit() &&
			failedUpdateIndex.Is_Valid() && failedUpdateVertex.Is_Valid(),
			"discard recovery clears the failed-mutation state after publication");
	}
	DynamicIBAccessClass::_Deinit();
	DynamicVBAccessClass::_Deinit();
	DX8IndexBufferClass *nativeIndex = NEW_REF(DX8IndexBufferClass,(3));
	DX8VertexBufferClass *nativeVertex = NEW_REF(DX8VertexBufferClass,(
		DX8_FVF_XYZ, 3));
	result |= Check(nativeIndex != nullptr && nativeVertex != nullptr &&
		nativeIndex->Is_Valid() && nativeVertex->Is_Valid(),
		"successful native buffer construction publishes valid owners");
	unsigned char indexBytes[3 * sizeof(unsigned short)] = {};
	unsigned char vertexBytes[3 * 3 * sizeof(float)] = {};
	void *lockedIndexBytes = nullptr;
	void *lockedVertexBytes = nullptr;
	result |= Check(nativeIndex->Lock_Buffer(0, sizeof(indexBytes), 0,
		&lockedIndexBytes) && lockedIndexBytes != nullptr &&
		nativeVertex->Lock_Buffer(0, sizeof(vertexBytes), 0,
		&lockedVertexBytes) && lockedVertexBytes != nullptr,
		"native compatibility-shaped locks expose owner storage");
	if (lockedIndexBytes != nullptr)
	{
		std::memcpy(lockedIndexBytes, indexBytes, sizeof(indexBytes));
	}
	if (lockedVertexBytes != nullptr)
	{
		std::memcpy(lockedVertexBytes, vertexBytes, sizeof(vertexBytes));
	}
	result |= Check(nativeIndex->Unlock_Buffer() &&
		nativeVertex->Unlock_Buffer(),
		"native compatibility-shaped unlocks publish through the owner");
	nativeIndex->Release_Ref();
	nativeVertex->Release_Ref();
	result |= Check(device.LiveCount() == 0,
		"native buffer fixture releases every successful native allocation");

	NativeW3DBufferOwner staticBuffer;
	result |= Check(staticBuffer.Create(staticDescriptor) == RENDER_RESULT_OK,
		"static vertex buffer creation publishes a neutral handle");
	void *bytes = nullptr;
	result |= Check(staticBuffer.Lock(0, 0, RENDER_BUFFER_UPDATE_PRESERVE,
		&bytes) == RENDER_RESULT_OK && bytes != nullptr &&
		staticBuffer.IsLocked(),
		"a zero-sized full lock spans the remaining static buffer");
	void *nested = reinterpret_cast<void *>(1);
	result |= Check(staticBuffer.Lock(0, 4, RENDER_BUFFER_UPDATE_PRESERVE,
		&nested) == RENDER_RESULT_INVALID_ARGUMENT && nested == nullptr,
		"overlapping native locks are rejected and clear their output");
	Fill(bytes, 16, 0x11);
	result |= Check(staticBuffer.Unlock() == RENDER_RESULT_OK &&
		!staticBuffer.IsLocked() && device.LastOffset() == 0 &&
		device.LastBytes() == 16 &&
		device.LastMode() == RENDER_BUFFER_UPDATE_PRESERVE,
		"static unlock publishes the exact full preserve update");
	GpuHandle staticHandle;
	GpuHandle rejectedHandle(1, 1);
	NativeW3DBufferDescription description;
	result |= Check(staticBuffer.AcquireVertexRange(4, 0, 0, 4,
		&staticHandle) == RENDER_RESULT_OK &&
		staticHandle.isValid() &&
		resources.DescribeBuffer(staticHandle, &description) == RENDER_RESULT_OK &&
		description.authority == NATIVE_W3D_CONTENT_CPU,
		"a successful full update acquires its exact CPU-authoritative range");
	unsigned int staticAuthorityEpoch = description.authorityEpoch;
	unsigned char staticBytes[16];
	std::memset(staticBytes, 0x11, sizeof(staticBytes));
	result |= Check(staticBuffer.Lock(4, 4, RENDER_BUFFER_UPDATE_PRESERVE,
		&bytes) == RENDER_RESULT_OK && bytes != nullptr &&
		std::memcmp(bytes, staticBytes + 4, 4) == 0,
		"static partial preserve staging starts from the authoritative image");
	Fill(bytes, 4, 0x22);
	staticBytes[4] = 0x22;
	staticBytes[5] = 0x22;
	staticBytes[6] = 0x22;
	staticBytes[7] = 0x22;
	result |= Check(staticBuffer.Unlock() == RENDER_RESULT_OK &&
		device.BufferEquals(staticHandle, staticBytes, sizeof(staticBytes)),
		"static partial preserve publishes only the requested vertex bytes");
	result |= Check(resources.DescribeBuffer(staticHandle, &description) ==
		RENDER_RESULT_OK && description.authority == NATIVE_W3D_CONTENT_CPU,
		"static partial preserve retains whole-buffer CPU authority");
	staticAuthorityEpoch = description.authorityEpoch;
	result |= Check(device.resize(800, 600) == RENDER_RESULT_OK &&
		host.ReplaceContext(device.immediateContext()) == RENDER_RESULT_OK &&
		resources.RepublishStaticBuffersAfterResize() == RENDER_RESULT_OK &&
		device.BufferEquals(staticHandle, staticBytes, sizeof(staticBytes)) &&
		resources.DescribeBuffer(staticHandle, &description) == RENDER_RESULT_OK &&
		description.authority == NATIVE_W3D_CONTENT_CPU &&
		description.authorityEpoch == staticAuthorityEpoch &&
		staticBuffer.AcquireVertexRange(4, 0, 0, 4, &rejectedHandle) ==
			RENDER_RESULT_OK && rejectedHandle == staticHandle,
		"ordinary resize preserves the static geometry epoch and exact draw range");
	result |= Check(staticBuffer.AcquireVertexRange(4, 0, 0, 0,
		&rejectedHandle) == RENDER_RESULT_INVALID_ARGUMENT &&
		!rejectedHandle.isValid() &&
		staticBuffer.AcquireVertexRange(8, 0, 0, 2,
			&rejectedHandle) == RENDER_RESULT_INVALID_ARGUMENT &&
		!rejectedHandle.isValid() &&
		staticBuffer.AcquireIndexRange(RENDER_FORMAT_R16_UINT, 0, 0, 1,
			&rejectedHandle) == RENDER_RESULT_INVALID_ARGUMENT &&
		!rejectedHandle.isValid(),
		"typed acquisition rejects empty, mismatched-stride, and wrong-binding ranges");
	result |= Check(staticBuffer.Lock(0, 16, RENDER_BUFFER_UPDATE_DISCARD,
		&bytes) == RENDER_RESULT_INVALID_ARGUMENT && bytes == nullptr &&
		staticBuffer.Lock(0, 16, RENDER_BUFFER_UPDATE_NO_OVERWRITE,
			&bytes) == RENDER_RESULT_INVALID_ARGUMENT && bytes == nullptr,
		"static buffers reject dynamic discard and no-overwrite modes");

	BufferDescriptor dynamicDescriptor = staticDescriptor;
	dynamicDescriptor.binding = RENDER_BUFFER_INDEX;
	dynamicDescriptor.stride = 2;
	dynamicDescriptor.usage = RENDER_USAGE_DYNAMIC;
	NativeW3DBufferOwner dynamicBuffer;
	result |= Check(dynamicBuffer.Create(dynamicDescriptor) == RENDER_RESULT_OK,
		"dynamic index buffer creation publishes a neutral handle");
	result |= Check(dynamicBuffer.Lock(0, 8, RENDER_BUFFER_UPDATE_DISCARD,
		&bytes) == RENDER_RESULT_OK && bytes != nullptr,
		"dynamic buffer accepts a discard-at-zero range");
	Fill(bytes, 8, 0x22);
	result |= Check(dynamicBuffer.Unlock() == RENDER_RESULT_OK &&
		device.LastOffset() == 0 && device.LastBytes() == 8 &&
		device.LastMode() == RENDER_BUFFER_UPDATE_DISCARD,
		"discard publishes its exact initialized prefix");
	GpuHandle dynamicHandle;
	unsigned char dynamicBytes[16];
	std::memset(dynamicBytes, 0, sizeof(dynamicBytes));
	std::memset(dynamicBytes, 0x22, 8);
	result |= Check(dynamicBuffer.AcquireIndexRange(RENDER_FORMAT_R16_UINT,
		0, 0, 4, &dynamicHandle) == RENDER_RESULT_OK &&
		resources.DescribeBuffer(dynamicHandle, &description) == RENDER_RESULT_OK &&
		description.authority == NATIVE_W3D_CONTENT_INVALID,
		"partial discard acquires only its initialized index prefix");
	result |= Check(dynamicBuffer.AcquireIndexRange(RENDER_FORMAT_R16_UINT,
		0, 4, 1, &rejectedHandle) == RENDER_RESULT_INVALID_ARGUMENT &&
		!rejectedHandle.isValid() &&
		dynamicBuffer.AcquireIndexRange(RENDER_FORMAT_R32_UINT,
			0, 0, 2, &rejectedHandle) == RENDER_RESULT_INVALID_ARGUMENT &&
		!rejectedHandle.isValid(),
		"index acquisition clears adjacent unwritten and wrong-format ranges");
	result |= Check(dynamicBuffer.Lock(8, 8,
		RENDER_BUFFER_UPDATE_NO_OVERWRITE, &bytes) == RENDER_RESULT_OK,
		"dynamic buffer accepts a disjoint no-overwrite tail");
	Fill(bytes, 8, 0x33);
	std::memset(dynamicBytes + 8, 0x33, 8);
	result |= Check(dynamicBuffer.Unlock() == RENDER_RESULT_OK &&
		device.LastOffset() == 8 && device.LastBytes() == 8 &&
		device.LastMode() == RENDER_BUFFER_UPDATE_NO_OVERWRITE &&
		resources.DescribeBuffer(dynamicHandle, &description) == RENDER_RESULT_OK &&
		description.authority == NATIVE_W3D_CONTENT_INVALID,
		"disjoint no-overwrite remains range-authoritative, not whole-buffer authoritative");
	result |= Check(dynamicBuffer.Lock(4, 4,
		RENDER_BUFFER_UPDATE_PRESERVE, &bytes) == RENDER_RESULT_OK &&
		bytes != nullptr && std::memcmp(bytes, dynamicBytes + 4, 4) == 0,
		"index partial preserve staging starts from the authoritative image");
	Fill(bytes, 4, 0x44);
	std::memset(dynamicBytes + 4, 0x44, 4);
	result |= Check(dynamicBuffer.Unlock() == RENDER_RESULT_OK &&
		device.BufferEquals(dynamicHandle, dynamicBytes, sizeof(dynamicBytes)),
		"index partial preserve publishes only the requested index bytes");
	result |= Check(dynamicBuffer.AcquireIndexRange(RENDER_FORMAT_R16_UINT,
		0, 0, 8, &rejectedHandle) == RENDER_RESULT_OK &&
		rejectedHandle == dynamicHandle,
		"adjacent discard and no-overwrite writes acquire as one exact draw range");
	result |= Check(dynamicBuffer.Lock(4, 4, RENDER_BUFFER_UPDATE_DISCARD,
		&bytes) == RENDER_RESULT_INVALID_ARGUMENT && bytes == nullptr,
		"discard with a nonzero destination fails closed");

	device.FailUpdate(true);
	result |= Check(dynamicBuffer.Lock(8, 4,
		RENDER_BUFFER_UPDATE_NO_OVERWRITE, &bytes) == RENDER_RESULT_OK,
		"failure fixture obtains one bounded transient range");
	Fill(bytes, 4, 0x44);
	rejectedHandle = GpuHandle(1, 1);
	result |= Check(dynamicBuffer.Unlock() == RENDER_RESULT_FAILED &&
		dynamicBuffer.HasFailedMutation() &&
		dynamicBuffer.AcquireIndexRange(RENDER_FORMAT_R16_UINT, 0, 0, 1,
			&rejectedHandle) == RENDER_RESULT_FAILED &&
		!rejectedHandle.isValid(),
		"failed publication suppresses exact range acquisition and clears staging");
	rejectedHandle = GpuHandle(1, 1);
	result |= Check(staticBuffer.AcquireVertexRange(4, 0, 0, 4,
		&rejectedHandle) == RENDER_RESULT_OK &&
		rejectedHandle == staticHandle,
		"one buffer mutation failure preserves unrelated static authority");
	result |= Check(dynamicBuffer.Lock(0, 16,
		RENDER_BUFFER_UPDATE_PRESERVE, &bytes) == RENDER_RESULT_FAILED &&
		bytes == nullptr,
		"a failed owner cannot expose stale bytes through preserve");
	device.FailUpdate(false);
	const unsigned int destroysBeforeRecovery = device.DestroyCount();
	device.FailCreate(true);
	result |= Check(dynamicBuffer.Lock(0, 16, RENDER_BUFFER_UPDATE_DISCARD,
		&bytes) == RENDER_RESULT_FAILED && bytes == nullptr &&
		device.DestroyCount() == destroysBeforeRecovery &&
		resources.IsValid(dynamicHandle),
		"failed discard recreation retains the retryable previous generation");
	device.FailCreate(false);
	result |= Check(dynamicBuffer.Lock(0, 16, RENDER_BUFFER_UPDATE_DISCARD,
		&bytes) == RENDER_RESULT_OK,
		"discard recreation retries after a transient allocation failure");
	Fill(bytes, 16, 0x55);
	GpuHandle recoveredHandle;
	result |= Check(dynamicBuffer.Unlock() == RENDER_RESULT_OK &&
		dynamicBuffer.AcquireIndexRange(RENDER_FORMAT_R16_UINT, 0, 0, 8,
			&recoveredHandle) == RENDER_RESULT_OK &&
		recoveredHandle != dynamicHandle &&
		device.DestroyCount() == destroysBeforeRecovery + 1 &&
		!resources.IsValid(dynamicHandle),
		"successful recovery publishes a replacement before retiring the stale generation");

	// A replacement allocation is already live when destruction of the old
	// handle refuses the transaction.  Retire only the exact old registry slot;
	// keep its native allocation for Shutdown while publishing the replacement.
	NativeW3DBufferOwner deferredDestroyBuffer;
	result |= Check(deferredDestroyBuffer.Create(dynamicDescriptor) ==
		RENDER_RESULT_OK, "discard cleanup fixture creates its owner");
	void *deferredBytes = nullptr;
	result |= Check(deferredDestroyBuffer.Lock(4, 4,
		RENDER_BUFFER_UPDATE_NO_OVERWRITE, &deferredBytes) ==
		RENDER_RESULT_OK && deferredBytes != nullptr,
		"discard cleanup fixture accepts an initial mutation");
	if (deferredBytes != nullptr)
	{
		Fill(deferredBytes, 4, 0x19);
	}
	GpuHandle deferredOldHandle;
	result |= Check(deferredDestroyBuffer.Unlock() == RENDER_RESULT_OK &&
		deferredDestroyBuffer.AcquireIndexRange(RENDER_FORMAT_R16_UINT, 0, 2, 2,
			&deferredOldHandle) == RENDER_RESULT_OK,
		"discard cleanup fixture publishes its initial mutation and binding");
	device.FailUpdate(true);
	result |= Check(deferredDestroyBuffer.Lock(4, 4,
		RENDER_BUFFER_UPDATE_NO_OVERWRITE, &deferredBytes) ==
		RENDER_RESULT_OK && deferredBytes != nullptr,
		"discard cleanup fixture obtains a failure-injection range");
	if (deferredBytes != nullptr)
	{
		Fill(deferredBytes, 4, 0x29);
	}
	result |= Check(deferredDestroyBuffer.Unlock() == RENDER_RESULT_FAILED &&
		deferredDestroyBuffer.HasFailedMutation(),
		"discard cleanup fixture enters failed-mutation state");
	device.FailUpdate(false);
	const unsigned int deferredCreatesBeforeRecovery = device.CreateCount();
	const unsigned int deferredDestroysBeforeRecovery = device.DestroyCount();
	const unsigned int deferredLiveBeforeRecovery = device.LiveCount();
	device.FailDestroy(true);
	deferredBytes = nullptr;
	result |= Check(deferredDestroyBuffer.Lock(0, 16,
		RENDER_BUFFER_UPDATE_DISCARD, &deferredBytes) ==
		RENDER_RESULT_OK && deferredBytes != nullptr &&
		device.LiveCount() == deferredLiveBeforeRecovery + 1 &&
		device.CreateCount() == deferredCreatesBeforeRecovery + 1 &&
		!resources.IsValid(deferredOldHandle),
		"old-handle destruction failure retires only the old slot");
	if (deferredBytes != nullptr)
	{
		Fill(deferredBytes, 16, 0x39);
	}
	GpuHandle deferredReplacementHandle;
	result |= Check(deferredDestroyBuffer.Unlock() == RENDER_RESULT_OK &&
		!deferredDestroyBuffer.HasFailedMutation() &&
		device.CreateCount() == deferredCreatesBeforeRecovery + 1 &&
		device.DestroyCount() == deferredDestroysBeforeRecovery &&
		deferredDestroyBuffer.AcquireIndexRange(RENDER_FORMAT_R16_UINT, 0, 0, 8,
			&deferredReplacementHandle) == RENDER_RESULT_OK &&
		deferredReplacementHandle != deferredOldHandle,
		"replacement publication leaves the retired allocation hidden");
	device.FailDestroy(false);
	result |= Check(deferredDestroyBuffer.Reset() == RENDER_RESULT_OK &&
		device.LiveCount() == deferredLiveBeforeRecovery &&
		device.DestroyCount() == deferredDestroysBeforeRecovery + 1 &&
		!resources.IsValid(deferredOldHandle) &&
		!resources.IsValid(deferredReplacementHandle),
		"discard cleanup fixture releases the replacement and retains the old slot for shutdown");

	rejectedHandle = GpuHandle(1, 1);
	result |= Check(device.recoverDevice() == RENDER_RESULT_OK &&
		!device.BufferEquals(staticHandle, staticBytes, sizeof(staticBytes)) &&
		host.ReplaceContext(device.immediateContext()) == RENDER_RESULT_OK &&
		resources.RestoreStaticBuffersAfterRecovery() == RENDER_RESULT_OK &&
		device.BufferEquals(staticHandle, staticBytes, sizeof(staticBytes)) &&
		resources.DescribeBuffer(staticHandle, &description) == RENDER_RESULT_OK &&
		description.authority == NATIVE_W3D_CONTENT_CPU &&
		description.authorityEpoch == staticAuthorityEpoch &&
		staticBuffer.AcquireVertexRange(4, 0, 0, 4, &rejectedHandle) ==
			RENDER_RESULT_OK && rejectedHandle == staticHandle,
		"forced recovery republishes persistent static geometry before acquisition");
	device.FailUpdate(true);
	rejectedHandle = GpuHandle(1, 1);
	result |= Check(device.recoverDevice() == RENDER_RESULT_OK &&
		host.ReplaceContext(device.immediateContext()) == RENDER_RESULT_OK &&
		resources.RestoreStaticBuffersAfterRecovery() == RENDER_RESULT_FAILED &&
		staticBuffer.AcquireVertexRange(4, 0, 0, 4, &rejectedHandle) ==
			RENDER_RESULT_FAILED && !rejectedHandle.isValid(),
		"failed static recovery clears the draw handle instead of exposing stale geometry");
	device.FailUpdate(false);

	device.FailCreate(true);
	NativeW3DBufferOwner failedCreate;
	result |= Check(failedCreate.Create(dynamicDescriptor) ==
		RENDER_RESULT_FAILED && failedCreate.HasFailedMutation(),
		"backend creation failure is retained as a closed owner state");
	device.FailCreate(false);

	result |= Check(staticBuffer.Reset() == RENDER_RESULT_OK &&
		dynamicBuffer.Reset() == RENDER_RESULT_OK &&
		failedCreate.Reset() == RENDER_RESULT_OK &&
		!resources.IsValid(staticHandle) &&
		!resources.IsValid(recoveredHandle),
		"owner reset invalidates every exported handle generation");
	const unsigned int retainedRetiredLiveCount = device.LiveCount();

	const unsigned int pointGroupTriIndices = 3U * (2048U / 3U);
	const unsigned int pointGroupQuadIndices = 6U * (2048U / 4U);
	BufferDescriptor pointGroupTrisDescriptor = dynamicDescriptor;
	pointGroupTrisDescriptor.usage = RENDER_USAGE_DEFAULT;
	pointGroupTrisDescriptor.byteCount =
		pointGroupTriIndices * sizeof(unsigned short);
	BufferDescriptor pointGroupQuadsDescriptor = pointGroupTrisDescriptor;
	pointGroupQuadsDescriptor.byteCount =
		pointGroupQuadIndices * sizeof(unsigned short);
	NativeW3DBufferOwner pointGroupTris;
	NativeW3DBufferOwner pointGroupQuads;
	result |= Check(pointGroupTris.Create(pointGroupTrisDescriptor) ==
		RENDER_RESULT_OK &&
		pointGroupQuads.Create(pointGroupQuadsDescriptor) == RENDER_RESULT_OK &&
		pointGroupTris.Lock(0, 0, RENDER_BUFFER_UPDATE_PRESERVE, &bytes) ==
			RENDER_RESULT_OK,
		"PointGroup-shaped startup creates both static index owners");
	Fill(bytes, pointGroupTrisDescriptor.byteCount, 0x77);
	result |= Check(pointGroupTris.Unlock() == RENDER_RESULT_OK &&
		pointGroupQuads.Lock(0, 0, RENDER_BUFFER_UPDATE_PRESERVE, &bytes) ==
			RENDER_RESULT_OK,
		"PointGroup-shaped startup publishes the full triangle index range");
	Fill(bytes, pointGroupQuadsDescriptor.byteCount, 0x88);
	GpuHandle pointGroupTrisHandle;
	GpuHandle pointGroupQuadsHandle;
	result |= Check(pointGroupQuads.Unlock() == RENDER_RESULT_OK &&
		pointGroupTris.AcquireIndexRange(RENDER_FORMAT_R16_UINT, 0, 0,
			pointGroupTriIndices, &pointGroupTrisHandle) == RENDER_RESULT_OK &&
		pointGroupQuads.AcquireIndexRange(RENDER_FORMAT_R16_UINT, 0, 0,
			pointGroupQuadIndices, &pointGroupQuadsHandle) == RENDER_RESULT_OK &&
		device.LiveCount() == retainedRetiredLiveCount + 2,
		"PointGroup-shaped startup exposes both exact initialized draw ranges");
	result |= Check(pointGroupQuads.Reset() == RENDER_RESULT_OK &&
		pointGroupTris.Reset() == RENDER_RESULT_OK &&
		device.LiveCount() == retainedRetiredLiveCount,
		"PointGroup-shaped owner deinitialization releases both live handles");

	NativeW3DBufferOwner partialPointGroupTris;
	NativeW3DBufferOwner partialPointGroupQuads;
	result |= Check(partialPointGroupTris.Create(pointGroupTrisDescriptor) ==
		RENDER_RESULT_OK, "partial PointGroup startup creates its first owner");
	device.FailCreate(true);
	result |= Check(partialPointGroupQuads.Create(pointGroupQuadsDescriptor) ==
		RENDER_RESULT_FAILED && partialPointGroupQuads.HasFailedMutation(),
		"injected second PointGroup owner creation fails closed");
	device.FailCreate(false);
	result |= Check(partialPointGroupQuads.Reset() == RENDER_RESULT_OK,
		"partial PointGroup startup resets the failed second owner");
	result |= Check(partialPointGroupTris.Reset() == RENDER_RESULT_OK,
		"partial PointGroup startup resets the first owner");
	result |= Check(device.LiveCount() == retainedRetiredLiveCount,
		"partial PointGroup startup unwind leaves only the retired recovery handle");

	NativeW3DBufferOwner staleBindingBuffer;
	result |= Check(staleBindingBuffer.Create(staticDescriptor) ==
		RENDER_RESULT_OK &&
		staleBindingBuffer.Lock(0, 16, RENDER_BUFFER_UPDATE_PRESERVE,
			&bytes) == RENDER_RESULT_OK,
		"a lifecycle fixture publishes one initialized native buffer");
	Fill(bytes, 16, 0x66);
	GpuHandle staleBindingHandle;
	result |= Check(staleBindingBuffer.Unlock() == RENDER_RESULT_OK &&
		staleBindingBuffer.AcquireVertexRange(4, 0, 0, 4,
			&staleBindingHandle) == RENDER_RESULT_OK,
		"the lifecycle fixture exposes its current binding generation");
	result |= Check(UnbindNativeW3DBufferResources(&differentResources) ==
		RENDER_RESULT_INVALID_ARGUMENT &&
		UnbindNativeW3DBufferResources(&resources) == RENDER_RESULT_OK &&
		BindNativeW3DBufferResources(&resources) == RENDER_RESULT_OK &&
		staleBindingBuffer.AcquireVertexRange(4, 0, 0, 4,
			&rejectedHandle) == RENDER_RESULT_FAILED &&
		!rejectedHandle.isValid() && staleBindingBuffer.Reset() == RENDER_RESULT_OK &&
		!resources.IsValid(staleBindingHandle) &&
		UnbindNativeW3DBufferResources(&resources) == RENDER_RESULT_OK,
		"only the borrowed registry can unbind the native buffer boundary");
	NativeW3DBufferOwner afterUnbind;
	result |= Check(afterUnbind.Create(staticDescriptor) ==
		RENDER_RESULT_INVALID_ARGUMENT,
		"new native owners fail closed after registry unbind");
	result |= Check(resources.Shutdown() == RENDER_RESULT_OK &&
		host.Detach() == RENDER_RESULT_OK && device.LiveCount() == 0,
		"registry shutdown leaves no native buffer resource alive");
	result |= RunNativeLine3DDestroyFailureContract();

	return result;
}
