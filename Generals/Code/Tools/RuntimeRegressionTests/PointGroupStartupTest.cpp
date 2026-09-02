#include "pointgr.h"
#include "nativew3dbufferowner.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace
{
using namespace rts::render;

int Check(bool condition, const char *message)
{
	if (condition)
	{
		return 0;
	}
	std::fprintf(stderr, "FAILED: %s\n", message);
	return 1;
}

class PointGroupDevice;

class PointGroupContext : public IRenderContext
{
public:
	explicit PointGroupContext(PointGroupDevice *device) : m_device(device) {}
	RenderResult beginFrame() override { return RENDER_RESULT_OK; }
	RenderResult updateBuffer(GpuHandle, const void *, size_t, size_t,
		RenderBufferUpdateMode) override { return RENDER_RESULT_UNSUPPORTED; }
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
		{ return RENDER_RESULT_UNSUPPORTED; }
	RenderResult drawIndexed(unsigned int, unsigned int, int) override
		{ return RENDER_RESULT_UNSUPPORTED; }
	RenderResult endFrame() override { return RENDER_RESULT_OK; }
private:
	PointGroupDevice *m_device;
};

class PointGroupDevice : public IRenderDevice
{
public:
	struct Buffer
	{
		Buffer() : live(false), handle(), byteCount(0) {}
		bool live;
		GpuHandle handle;
		size_t byteCount;
	};

	PointGroupDevice() : m_allocator(8), m_context(this), m_operational(true),
		m_createInvocation(0), m_updateInvocation(0), m_failCreateOn(0),
		m_failUpdateOn(0), m_buffers(8) {}

	RenderBackend backend() const override { return RENDER_BACKEND_D3D11; }
	bool isOperational() const override { return m_operational; }
	RenderResult initialize(const RenderDeviceParameters &) override
		{ return RENDER_RESULT_INVALID_ARGUMENT; }
	void shutdown() override { m_operational = false; }
	IRenderContext *immediateContext() override { return &m_context; }
	RenderResult createBuffer(const BufferDescriptor &descriptor,
		const void *, size_t initialDataBytes, GpuHandle *buffer) override
	{
		if (buffer == nullptr)
		{
			return RENDER_RESULT_INVALID_ARGUMENT;
		}
		*buffer = GpuHandle();
		++m_createInvocation;
		if (m_failCreateOn != 0 && m_createInvocation == m_failCreateOn)
		{
			return RENDER_RESULT_OUT_OF_MEMORY;
		}
		if (!m_operational || descriptor.byteCount == 0 ||
			initialDataBytes != 0)
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
		slot.byteCount = descriptor.byteCount;
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
		Buffer *slot = Find(resource);
		if (slot == nullptr || !m_allocator.release(resource))
		{
			return false;
		}
		slot->live = false;
		return true;
	}
	RenderResult recoverDevice() override { return RENDER_RESULT_OK; }
	RenderResult resize(unsigned int, unsigned int) override
		{ return RENDER_RESULT_OK; }
	RenderResult present() override { return RENDER_RESULT_OK; }
	RenderResult getBackBufferInfo(RenderBackBufferInfo *) const override
		{ return RENDER_RESULT_UNSUPPORTED; }
	RenderResult captureBackBuffer(void *, size_t, size_t,
		RenderFormat *) override { return RENDER_RESULT_UNSUPPORTED; }
	RenderResult getDebugValidationErrorCount(unsigned int *count) const override
	{
		if (count == nullptr) return RENDER_RESULT_INVALID_ARGUMENT;
		*count = 0;
		return RENDER_RESULT_OK;
	}
	RenderResult reportDebugLiveObjects() override { return RENDER_RESULT_OK; }
	RenderResult updateBufferResource(GpuHandle resource, const void *data,
		size_t byteCount, size_t destinationOffset,
		RenderBufferUpdateMode) override
	{
		++m_updateInvocation;
		if (m_failUpdateOn != 0 && m_updateInvocation == m_failUpdateOn)
		{
			return RENDER_RESULT_FAILED;
		}
		Buffer *slot = Find(resource);
		return slot != nullptr && data != nullptr && byteCount != 0 &&
			destinationOffset <= slot->byteCount &&
			byteCount <= slot->byteCount - destinationOffset ?
			RENDER_RESULT_OK : RENDER_RESULT_INVALID_ARGUMENT;
	}

	void FailCreateOn(unsigned int invocation)
		{ m_failCreateOn = invocation; }
	void FailUpdateOn(unsigned int invocation)
		{ m_failUpdateOn = invocation; }
	unsigned int LiveCount() const { return m_allocator.liveCount(); }
	unsigned int CreateInvocations() const { return m_createInvocation; }
	unsigned int UpdateInvocations() const { return m_updateInvocation; }

private:
	Buffer *Find(GpuHandle handle)
	{
		if (!handle.isValid() || handle.index() >= m_buffers.size()) return nullptr;
		Buffer &slot = m_buffers[handle.index()];
		return slot.live && slot.handle == handle ? &slot : nullptr;
	}

	GpuHandleAllocator m_allocator;
	PointGroupContext m_context;
	bool m_operational;
	unsigned int m_createInvocation;
	unsigned int m_updateInvocation;
	unsigned int m_failCreateOn;
	unsigned int m_failUpdateOn;
	std::vector<Buffer> m_buffers;
};

int RunFailureCase(unsigned int failCreateOn, unsigned int failUpdateOn,
	unsigned int expectedCreates, unsigned int expectedUpdates,
	const char *message)
{
	int result = 0;
	PointGroupDevice device;
	device.FailCreateOn(failCreateOn);
	device.FailUpdateOn(failUpdateOn);
	NativeW3DResourceHost host(8);
	NativeW3DResources resources(8);
	result |= Check(host.Attach(&device, device.immediateContext()) ==
		RENDER_RESULT_OK && resources.BindHost(&host) == RENDER_RESULT_OK &&
		BindNativeW3DBufferResources(&resources) == RENDER_RESULT_OK,
		"PointGroup production failure fixture binds the native registry");
	result |= Check(!PointGroupClass::_Init() &&
		device.CreateInvocations() == expectedCreates &&
		device.UpdateInvocations() == expectedUpdates &&
		device.LiveCount() == 0, message);
	PointGroupClass::_Shutdown();
	result |= Check(UnbindNativeW3DBufferResources(&resources) ==
		RENDER_RESULT_OK && resources.Shutdown() == RENDER_RESULT_OK &&
		host.Detach() == RENDER_RESULT_OK && device.LiveCount() == 0,
		"PointGroup production failure fixture shuts down without live handles");
	return result;
}
}

int main()
{
	int result = 0;
	result |= RunFailureCase(2, 0, 2, 1,
		"actual PointGroup second allocation failure rolls back its first buffer");
	result |= RunFailureCase(0, 2, 2, 2,
		"actual PointGroup second upload failure rolls back both buffers");
	return result;
}
