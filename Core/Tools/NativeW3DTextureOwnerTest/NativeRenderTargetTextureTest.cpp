#include "Utility/CppMacros.h"
#include "texture.h"
#include "surfaceclass.h"
#include "nativew3dtextureowner.h"

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
	RenderResult setRenderTargets(const RenderTargetBinding &binding) override
		{ return binding.useBackBufferColor || (binding.hasColor &&
			binding.color.resource.isValid()) ? RENDER_RESULT_OK : RENDER_RESULT_INVALID_ARGUMENT; }
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
		m_failCreation(false), m_failRefresh(false), m_recoveryCount(0), m_textures(16) {}

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
		const bool sampledUpload = descriptor.usage == RENDER_USAGE_DEFAULT &&
			descriptor.binding == RENDER_TEXTURE_SHADER_RESOURCE;
		const bool renderTarget = descriptor.usage == RENDER_USAGE_DEFAULT &&
			(descriptor.binding & RENDER_TEXTURE_RENDER_TARGET) != 0;
		if (!m_operational || descriptor.width == 0 || descriptor.height == 0 ||
			descriptor.mipCount != 1 || descriptor.arrayCount != 1 ||
			descriptor.dimension != RENDER_TEXTURE_2D ||
			(!sampledUpload && !renderTarget) ||
			(descriptor.binding & RENDER_TEXTURE_SHADER_RESOURCE) == 0 ||
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
		if (m_failRefresh) return RENDER_RESULT_FAILED;
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
	void FailRefresh(bool fail) { m_failRefresh = fail; }
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
	bool m_failRefresh;
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
	{
		SurfaceClass *source = new SurfaceClass(2, 2, WW3D_FORMAT_A8R8G8B8);
		const unsigned char fog[16] = {
			0, 0, 0, 255, 64, 64, 64, 255,
			128, 128, 128, 255, 255, 255, 255, 255 };
		int sourcePitch = 0;
		unsigned char *sourceBytes = static_cast<unsigned char *>(source->Lock(&sourcePitch));
		result |= Check(sourceBytes != nullptr && sourcePitch >= 8,
			"CPU shroud source exposes its two-row image");
		if (sourceBytes != nullptr && sourcePitch >= 8)
		{
			std::memcpy(sourceBytes, fog, 8);
			std::memcpy(sourceBytes + sourcePitch, fog + 8, 8);
		}
		result |= Check(source->Unlock_Native_Surface(),
			"standalone CPU surface unlock succeeds without a GPU texture owner");
		for (unsigned int reset = 0; reset < 2; ++reset)
		{
			TextureClass *destination = new TextureClass(4, 4, WW3D_FORMAT_R5G6B5,
				MIP_LEVELS_1, TextureBaseClass::POOL_DEFAULT);
			SurfaceClass *surface = destination->Get_Surface_Level(0);
			result |= Check(surface != nullptr, "shroud destination owns a writable native surface");
			if (surface != nullptr)
			{
				int destinationPitch = 0;
				unsigned char *destinationBytes = static_cast<unsigned char *>(surface->Lock(&destinationPitch));
				const unsigned char border = static_cast<unsigned char>(17 + reset);
				if (destinationBytes != nullptr && destinationPitch >= 16)
					for (unsigned int y = 0; y < 4; ++y)
						for (unsigned int x = 0; x < 4; ++x)
						{
							unsigned char *pixel = destinationBytes + y * destinationPitch + x * 4;
							pixel[0] = pixel[1] = pixel[2] = border; pixel[3] = 255;
						}
				result |= Check(destinationBytes != nullptr && destinationPitch >= 16 &&
					surface->Unlock_Native_Surface() && surface->Copy_Native(1, 1, 0, 0, 2, 2, source),
					"CPU shroud data publishes into the bordered destination after creation or reset");
				const unsigned char *published = nullptr;
				size_t rowPitch = 0, slicePitch = 0;
				result |= Check(destination->Get_Native_Subresource_Data(0, 0, &published, &rowPitch, &slicePitch) &&
					published != nullptr && rowPitch >= 16 && slicePitch >= rowPitch * 4 &&
					published[0] == border && published[3 * rowPitch + 12] == border &&
					std::memcmp(published + rowPitch + 4, fog, 8) == 0 &&
					std::memcmp(published + 2 * rowPitch + 4, fog + 8, 8) == 0,
					"shroud publication preserves black, partial and visible cells plus the reset border");
				destinationBytes = static_cast<unsigned char *>(surface->Lock(&destinationPitch));
				if (destinationBytes != nullptr) destinationBytes[0] = 99;
				device.FailRefresh(true);
				result |= Check(destinationBytes != nullptr && !surface->Unlock_Native_Surface(),
					"attached surface unlock still reports a failed GPU publication");
				device.FailRefresh(false);
				result |= Check(surface->Publish_Native_Changes(),
					"failed attached publication can retry its retained bytes");
				surface->Release_Ref();
			}
			destination->Release_Ref();
		}
		source->Release_Ref();
	}
	{
		// Font atlases are CPU A4R4G4B4 surfaces, while native sampled texture
		// storage is BGRA8. Exercise the sentence upload constructor with transparent,
		// partially covered, and opaque white glyph pixels.
		SurfaceClass *atlas = new SurfaceClass(3, 1, WW3D_FORMAT_A4R4G4B4);
		int pitch = 0;
		unsigned short *pixels = static_cast<unsigned short *>(atlas->Lock(&pitch));
		result |= Check(pixels != nullptr && pitch >= 6, "font atlas exposes pitched A4R4G4B4 pixels");
		if (pixels != nullptr && pitch >= 6)
		{
			pixels[0] = 0x0fff;
			pixels[1] = 0x8fff;
			pixels[2] = 0xffff;
			atlas->Unlock();
			TextureClass *font = new TextureClass(atlas, MIP_LEVELS_1);
			const unsigned char *uploaded = nullptr;
			size_t rowPitch = 0, slicePitch = 0;
			result |= Check(font->Is_Initialized() &&
				font->Get_Native_Subresource_Data(0, 0, &uploaded, &rowPitch, &slicePitch) &&
				uploaded != nullptr && rowPitch >= 12 && slicePitch >= 12 &&
				uploaded[0] == 255 && uploaded[3] == 0 &&
				uploaded[4] == 255 && uploaded[7] == 136 &&
				uploaded[8] == 255 && uploaded[11] == 255,
				"sentence surface upload preserves transparent, antialiased, and opaque glyph alpha");
			font->Release_Ref();
		}
		atlas->Release_Ref();
	}

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
	RenderTargetBinding outputBinding;
	outputBinding.useBackBufferColor = false;
	outputBinding.hasColor = true;
	outputBinding.color.resource = beforeRecovery.texture.resource;
	result |= Check(device.immediateContext()->beginFrame() == RENDER_RESULT_OK &&
		device.immediateContext()->setRenderTargets(outputBinding) == RENDER_RESULT_OK &&
		target->Publish_Native_Output(beforeRecovery) &&
		device.immediateContext()->endFrame() == RENDER_RESULT_OK,
		"accepted output binding publishes GPU authority into the texture cache");
	NativeW3DTextureHandle rendered;
	NativeW3DGpuContentLease renderedLease;
	const unsigned char *cpuPixels = nullptr;
	size_t cpuPitch = 0, cpuBytes = 0;
	result |= Check(target->Acquire_Native_Texture(&rendered, &renderedLease) &&
		renderedLease.isValid() &&
		!target->Get_Native_Subresource_Data(0, 0, &cpuPixels, &cpuPitch, &cpuBytes),
		"GPU-authored target samples through a lease and hides stale creation pixels");

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
	NativeW3DTextureHandle invalidated;
	result |= Check(!target->Acquire_Native_Texture(&invalidated) &&
		!invalidated.isValid() &&
		!target->Get_Native_Subresource_Data(0, 0, &cpuPixels, &cpuPitch, &cpuBytes),
		"recovery cannot silently replace GPU output with its initial CPU-zero image");
	NativeW3DSurfaceHandle afterRecovery = beforeRecovery;
	result |= Check(target->Is_Initialized() &&
		target->Acquire_Native_Surface(0, 0, true, &afterRecovery) &&
		afterRecovery.isValid() && afterRecovery.backendEpoch != priorEpoch &&
		afterRecovery.width == 64 && afterRecovery.height == 32,
		"TextureClass reacquires its typed output surface after device recovery");
	outputBinding.color.resource = afterRecovery.texture.resource;
	result |= Check(device.immediateContext()->beginFrame() == RENDER_RESULT_OK &&
		device.immediateContext()->setRenderTargets(outputBinding) == RENDER_RESULT_OK &&
		target->Publish_Native_Output(afterRecovery) &&
		device.immediateContext()->endFrame() == RENDER_RESULT_OK &&
		target->Acquire_Native_Texture(&invalidated),
		"explicit post-recovery output publication restores current GPU sampling");
	NativeW3DTextureHandle staleHandle = rendered;
	result |= Check(!target->Acquire_Native_Texture(&staleHandle, &renderedLease),
		"an explicitly requested pre-recovery GPU lease remains invalid");

	target->Release_Ref();
	result |= Check(device.LiveCount() == 0 &&
		UnbindNativeW3DTextureResources(&resources) == RENDER_RESULT_OK &&
		resources.Shutdown() == RENDER_RESULT_OK &&
		host.Detach() == RENDER_RESULT_OK,
		"title texture teardown retires native ownership before host shutdown");
	device.shutdown();
	return result;
}
