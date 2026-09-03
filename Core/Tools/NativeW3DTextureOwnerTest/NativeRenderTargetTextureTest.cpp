#include "Utility/CppMacros.h"
#include "texture.h"
#include "nativew3dtextureowner.h"

#include <cstdio>
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

class TextureTestContext : public IRenderContext
{
public:
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
};

class TextureTestDevice : public IRenderDevice
{
public:
	struct TextureSlot
	{
		TextureSlot() : live(false), handle(), descriptor() {}
		bool live;
		GpuHandle handle;
		TextureDescriptor descriptor;
	};

	TextureTestDevice() : m_allocator(16), m_context(), m_operational(true),
		m_failCreation(false), m_recoveryCount(0), m_textures(16) {}

	RenderBackend backend() const override { return RENDER_BACKEND_D3D11; }
	bool isOperational() const override { return m_operational; }
	RenderResult initialize(const RenderDeviceParameters &) override
		{ return RENDER_RESULT_INVALID_ARGUMENT; }
	void shutdown() override { m_operational = false; }
	IRenderContext *immediateContext() override { return &m_context; }
	RenderResult createBuffer(const BufferDescriptor &, const void *, size_t,
		GpuHandle *) override { return RENDER_RESULT_UNSUPPORTED; }
	RenderResult createTexture(const TextureDescriptor &descriptor,
		const TextureSubresourceData *initialData,
		unsigned int initialDataCount, GpuHandle *texture) override
	{
		if (texture == nullptr)
		{
			return RENDER_RESULT_INVALID_ARGUMENT;
		}
		*texture = GpuHandle();
		if (m_failCreation)
		{
			return RENDER_RESULT_OUT_OF_MEMORY;
		}
		if (!m_operational || descriptor.width == 0 || descriptor.height == 0 ||
			descriptor.mipCount != 1 || descriptor.arrayCount != 1 ||
			descriptor.dimension != RENDER_TEXTURE_2D ||
			descriptor.usage != RENDER_USAGE_DEFAULT ||
			(descriptor.binding & RENDER_TEXTURE_SHADER_RESOURCE) == 0 ||
			(descriptor.binding & RENDER_TEXTURE_RENDER_TARGET) == 0 ||
			initialData == nullptr || initialDataCount != 1)
		{
			return RENDER_RESULT_INVALID_ARGUMENT;
		}
		const GpuHandle created = m_allocator.allocate();
		if (!created.isValid() || created.index() >= m_textures.size())
		{
			return RENDER_RESULT_OUT_OF_MEMORY;
		}
		TextureSlot &slot = m_textures[created.index()];
		slot.live = true;
		slot.handle = created;
		slot.descriptor = descriptor;
		*texture = created;
		return RENDER_RESULT_OK;
	}
	RenderResult refreshTexture(GpuHandle texture,
		const TextureDescriptor &descriptor, const TextureSubresourceData *,
		unsigned int) override
	{
		TextureSlot *slot = Find(texture);
		if (slot == nullptr)
		{
			return RENDER_RESULT_INVALID_ARGUMENT;
		}
		slot->descriptor = descriptor;
		return RENDER_RESULT_OK;
	}
	RenderResult copyActiveColorTargetToTexture(GpuHandle) override
		{ return RENDER_RESULT_UNSUPPORTED; }
	bool destroyResource(GpuHandle resource) override
	{
		TextureSlot *slot = Find(resource);
		if (slot == nullptr || !m_allocator.release(resource))
		{
			return false;
		}
		slot->live = false;
		return true;
	}
	RenderResult recoverDevice() override
	{
		if (!m_operational)
		{
			return RENDER_RESULT_DEVICE_REMOVED;
		}
		++m_recoveryCount;
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
	RenderResult reportDebugLiveObjects() override
		{ return RENDER_RESULT_OK; }

	void FailCreation(bool fail) { m_failCreation = fail; }
	unsigned int LiveCount() const { return m_allocator.liveCount(); }
	unsigned int RecoveryCount() const { return m_recoveryCount; }

private:
	TextureSlot *Find(GpuHandle handle)
	{
		if (!handle.isValid() || handle.index() >= m_textures.size())
		{
			return nullptr;
		}
		TextureSlot &slot = m_textures[handle.index()];
		return slot.live && slot.handle == handle ? &slot : nullptr;
	}

	GpuHandleAllocator m_allocator;
	TextureTestContext m_context;
	bool m_operational;
	bool m_failCreation;
	unsigned int m_recoveryCount;
	std::vector<TextureSlot> m_textures;
};
}

int main()
{
	using namespace rts::render;
	int result = 0;
	TextureTestDevice device;
	NativeW3DResourceHost host(8);
	NativeW3DResources resources(8);
	result |= Check(host.Attach(&device, device.immediateContext()) ==
		RENDER_RESULT_OK && resources.BindHost(&host) == RENDER_RESULT_OK &&
		BindNativeW3DTextureResources(&resources) == RENDER_RESULT_OK,
		"the title texture fixture binds one native owner-thread registry");

	TextureClass *target = new TextureClass(64, 32, WW3D_FORMAT_A8R8G8B8,
		MIP_LEVELS_1, TextureClass::POOL_DEFAULT, true);
	NativeW3DSurfaceHandle beforeRecovery;
	result |= Check(target != nullptr && target->Is_Initialized() &&
		target->Acquire_Native_Surface(0, 0, true, &beforeRecovery) &&
		beforeRecovery.isValid() && beforeRecovery.width == 64 &&
		beforeRecovery.height == 32 &&
		beforeRecovery.format == RENDER_FORMAT_B8G8R8A8_UNORM &&
		device.LiveCount() == 1,
		"a title TextureClass creates an initialized typed native color target");

	device.FailCreation(true);
	TextureClass *failedTarget = new TextureClass(32, 32,
		WW3D_FORMAT_A8R8G8B8, MIP_LEVELS_1,
		TextureClass::POOL_DEFAULT, true);
	NativeW3DSurfaceHandle failedSurface;
	result |= Check(failedTarget != nullptr &&
		!failedTarget->Is_Initialized() &&
		!failedTarget->Acquire_Native_Surface(0, 0, true, &failedSurface) &&
		!failedSurface.isValid() && device.LiveCount() == 1,
		"backend allocation failure leaves no initialized texture or surface token");
	failedTarget->Release_Ref();
	device.FailCreation(false);

	const unsigned int priorEpoch = beforeRecovery.backendEpoch;
	result |= Check(device.recoverDevice() == RENDER_RESULT_OK &&
		host.ReplaceContext(device.immediateContext()) == RENDER_RESULT_OK &&
		device.RecoveryCount() == 1,
		"the native resource host publishes one owner-thread recovery epoch");
	NativeW3DSurfaceHandle afterRecovery = beforeRecovery;
	result |= Check(target->Is_Initialized() &&
		target->Acquire_Native_Surface(0, 0, true, &afterRecovery) &&
		afterRecovery.isValid() && afterRecovery.backendEpoch != priorEpoch &&
		afterRecovery.width == 64 && afterRecovery.height == 32,
		"TextureClass reacquires its typed output surface after device recovery");

	target->Release_Ref();
	result |= Check(device.LiveCount() == 0 &&
		UnbindNativeW3DTextureResources(&resources) == RENDER_RESULT_OK &&
		resources.Shutdown() == RENDER_RESULT_OK &&
		host.Detach() == RENDER_RESULT_OK,
		"title texture teardown retires native ownership before host shutdown");
	device.shutdown();
	return result;
}
