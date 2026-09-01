#include "nativew3dtextureowner.h"
#include "Renderer/NativeW3DRenderer.h"
#include "Renderer/NativeW3DRenderState.h"

#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>

namespace rts
{
namespace render
{
class NativeW3DRecoveryTestAccess
{
public:
	static RenderResult AttachOwnedBackend(NativeW3DRenderer *renderer,
		IRenderDevice *device)
	{
		if (renderer == 0 || device == 0 || renderer->m_state != 0)
		{
			return RENDER_RESULT_INVALID_ARGUMENT;
		}
		NativeW3DRenderState *state = NativeW3DRenderState::Create(8);
		if (state == 0)
		{
			return RENDER_RESULT_OUT_OF_MEMORY;
		}
		RenderResult result = state->BindOwner();
		if (result == RENDER_RESULT_OK)
		{
			result = state->AttachBackend(device, device->immediateContext());
		}
		if (result != RENDER_RESULT_OK)
		{
			state->Release();
			return result;
		}
		renderer->m_state = state;
		renderer->m_frameOpen = false;
		renderer->m_ownsBackend = true;
		renderer->m_borrowedMode = false;
		return RENDER_RESULT_OK;
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

bool EqualTextureDescriptors(const TextureDescriptor &left,
	const TextureDescriptor &right)
{
	return left.width == right.width && left.height == right.height &&
		left.mipCount == right.mipCount &&
		left.arrayCount == right.arrayCount &&
		left.dimension == right.dimension && left.format == right.format &&
		left.binding == right.binding && left.usage == right.usage;
}

class FakeRenderContext : public IRenderContext
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

class FakeRenderDevice : public IRenderDevice
{
public:
	struct Texture
	{
		Texture() : live(false), handle(), descriptor() {}
		bool live;
		GpuHandle handle;
		TextureDescriptor descriptor;
	};

	FakeRenderDevice() : m_allocator(64), m_operational(true),
		m_failCreate(false), m_failRefresh(false), m_failDestroy(false),
		m_failRecovery(false), m_destroyCount(0),
		m_recoveryCount(0), m_textures(64)
	{
	}

	RenderBackend backend() const override { return RENDER_BACKEND_D3D11; }
	bool isOperational() const override { return m_operational; }
	RenderResult initialize(const RenderDeviceParameters &) override
		{ return RENDER_RESULT_INVALID_ARGUMENT; }
	void shutdown() override { m_operational = false; }
	IRenderContext *immediateContext() override { return &m_context; }
	RenderResult createBuffer(const BufferDescriptor &, const void *, size_t,
		GpuHandle *) override { return RENDER_RESULT_UNSUPPORTED; }
	RenderResult createTexture(const TextureDescriptor &descriptor,
		const TextureSubresourceData *, unsigned int, GpuHandle *texture) override
	{
		if (texture == 0)
		{
			return RENDER_RESULT_INVALID_ARGUMENT;
		}
		*texture = GpuHandle();
		if (m_failCreate)
		{
			return RENDER_RESULT_FAILED;
		}
		if (!m_operational)
		{
			return RENDER_RESULT_DEVICE_REMOVED;
		}
		const GpuHandle created = m_allocator.allocate();
		if (!created.isValid() || created.index() >= m_textures.size())
		{
			return RENDER_RESULT_OUT_OF_MEMORY;
		}
		Texture &slot = m_textures[created.index()];
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
		if (m_failRefresh)
		{
			return RENDER_RESULT_FAILED;
		}
		Texture *slot = Find(texture);
		if (slot == 0 || !EqualTextureDescriptors(slot->descriptor, descriptor))
		{
			return RENDER_RESULT_INVALID_ARGUMENT;
		}
		return RENDER_RESULT_OK;
	}
	RenderResult copyActiveColorTargetToTexture(GpuHandle texture) override
	{
		if (Find(texture) == 0)
		{
			return RENDER_RESULT_INVALID_ARGUMENT;
		}
		return RENDER_RESULT_OK;
	}
	bool destroyResource(GpuHandle resource) override
	{
		if (m_failDestroy)
		{
			return false;
		}
		Texture *slot = Find(resource);
		if (slot == 0 || !m_allocator.release(resource))
		{
			return false;
		}
		slot->live = false;
		++m_destroyCount;
		return true;
	}
	RenderResult recoverDevice() override
	{
		if (m_failRecovery)
		{
			m_operational = false;
			return RENDER_RESULT_DEVICE_REMOVED;
		}
		m_operational = true;
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
		if (count == 0)
		{
			return RENDER_RESULT_INVALID_ARGUMENT;
		}
		*count = 0;
		return RENDER_RESULT_OK;
	}
	RenderResult reportDebugLiveObjects() override { return RENDER_RESULT_OK; }

	void FailCreate(bool fail) { m_failCreate = fail; }
	void FailRefresh(bool fail) { m_failRefresh = fail; }
	void FailDestroy(bool fail) { m_failDestroy = fail; }
	void FailRecovery(bool fail) { m_failRecovery = fail; }
	unsigned int DestroyCount() const { return m_destroyCount; }
	unsigned int RecoveryCount() const { return m_recoveryCount; }
	unsigned int LiveCount() const { return m_allocator.liveCount(); }

private:
	Texture *Find(GpuHandle handle)
	{
		if (!handle.isValid() || handle.index() >= m_textures.size())
		{
			return 0;
		}
		Texture &slot = m_textures[handle.index()];
		return slot.live && slot.handle == handle ? &slot : 0;
	}

	GpuHandleAllocator m_allocator;
	FakeRenderContext m_context;
	bool m_operational;
	bool m_failCreate;
	bool m_failRefresh;
	bool m_failDestroy;
	bool m_failRecovery;
	unsigned int m_destroyCount;
	unsigned int m_recoveryCount;
	std::vector<Texture> m_textures;
};

TextureDescriptor MakeCpuDescriptor()
{
	TextureDescriptor descriptor;
	descriptor.width = 4;
	descriptor.height = 4;
	descriptor.mipCount = 2;
	descriptor.arrayCount = 1;
	descriptor.dimension = RENDER_TEXTURE_2D;
	descriptor.format = RENDER_FORMAT_R8G8B8A8_UNORM;
	descriptor.binding = RENDER_TEXTURE_SHADER_RESOURCE;
	descriptor.usage = RENDER_USAGE_DEFAULT;
	return descriptor;
}

TextureDescriptor MakeGpuDescriptor()
{
	TextureDescriptor descriptor;
	descriptor.width = 4;
	descriptor.height = 4;
	descriptor.mipCount = 1;
	descriptor.arrayCount = 1;
	descriptor.dimension = RENDER_TEXTURE_2D;
	descriptor.format = RENDER_FORMAT_R8G8B8A8_UNORM;
	descriptor.binding = RENDER_TEXTURE_SHADER_RESOURCE |
		RENDER_TEXTURE_RENDER_TARGET;
	descriptor.usage = RENDER_USAGE_DEFAULT;
	return descriptor;
}

void MakeCpuData(unsigned char *topBytes, unsigned char *lowerBytes,
	TextureSubresourceData *data)
{
	data[0].data = topBytes;
	data[0].rowPitch = 16;
	data[0].slicePitch = 64;
	data[1].data = lowerBytes;
	data[1].rowPitch = 8;
	data[1].slicePitch = 16;
}

bool HasNoLiveResources(const RenderResourceStatistics &statistics)
{
	return statistics.liveHandles == 0 && statistics.bufferCount == 0 &&
		statistics.textureCount == 0 && statistics.nativeResourceCount == 0 &&
		statistics.shaderResourceViewCount == 0 &&
		statistics.renderTargetViewCount == 0 &&
		statistics.depthStencilViewCount == 0 &&
		statistics.recoveryShadowBytes == 0;
}
}

int TestNeutralTextureOwnership()
{
	using namespace rts::render;
	int result = 0;
	unsigned char topBytes[64] = { 0 };
	unsigned char lowerBytes[16] = { 0 };
	TextureSubresourceData cpuData[2];
	MakeCpuData(topBytes, lowerBytes, cpuData);
	const TextureDescriptor cpuDescriptor = MakeCpuDescriptor();
	const TextureDescriptor gpuDescriptor = MakeGpuDescriptor();

	NativeW3DTextureOwner unboundOwner;
	NativeW3DTextureCandidate unboundCandidate;
	result |= Check(unboundOwner.CreateCandidate(cpuDescriptor, cpuData, 2,
		&unboundCandidate) == RENDER_RESULT_INVALID_ARGUMENT &&
		!unboundCandidate.IsValid(),
		"candidate creation fails closed before registry binding");
	result |= Check(BindNativeW3DTextureResources(0) ==
		RENDER_RESULT_INVALID_ARGUMENT,
		"the texture boundary rejects a null resource registry");

	FakeRenderDevice device;
	NativeW3DResourceHost host(32);
	NativeW3DResources resources(64);
	NativeW3DResources differentResources(2);
	result |= Check(host.Attach(&device, device.immediateContext()) ==
		RENDER_RESULT_OK && resources.BindHost(&host) == RENDER_RESULT_OK &&
		BindNativeW3DTextureResources(&resources) == RENDER_RESULT_OK &&
		BindNativeW3DTextureResources(&resources) == RENDER_RESULT_OK &&
		BindNativeW3DTextureResources(&differentResources) ==
			RENDER_RESULT_INVALID_ARGUMENT,
		"binding is idempotent only for the exact active registry");

	const unsigned int abandonedDestroys = device.DestroyCount();
	{
		NativeW3DTextureCandidate abandoned;
		result |= Check(unboundOwner.CreateCandidate(cpuDescriptor, cpuData, 2,
			&abandoned) == RENDER_RESULT_OK && abandoned.IsValid() &&
			resources.IsValid(abandoned.Handle()) &&
			!resources.DestroyTexture(abandoned.Handle()) &&
			!resources.Destroy(abandoned.Handle().resource) &&
			!resources.RetireTexture(abandoned.Handle()) &&
			abandoned.IsValid() && resources.IsValid(abandoned.Handle()),
			"raw destruction cannot bypass an owned candidate cleanup token");
	}
	result |= Check(device.DestroyCount() == abandonedDestroys + 1 &&
		device.LiveCount() == 0,
		"an abandoned owned candidate destroys its resource exactly once");

	const unsigned int rawPublishedDestroys = device.DestroyCount();
	NativeW3DTextureHandle rawPublishedHandle;
	{
		NativeW3DTextureOwner rawPublishedOwner;
		NativeW3DTextureCandidate rawPublishedCandidate;
		result |= Check(rawPublishedOwner.CreateCandidate(cpuDescriptor, cpuData,
			2, &rawPublishedCandidate) == RENDER_RESULT_OK &&
			rawPublishedOwner.PublishCandidate(&rawPublishedCandidate, 0) ==
				RENDER_RESULT_OK &&
			rawPublishedOwner.AcquireForSampling(&rawPublishedHandle) ==
				RENDER_RESULT_OK &&
			!resources.DestroyTexture(rawPublishedHandle) &&
			!resources.Destroy(rawPublishedHandle.resource) &&
			!resources.RetireTexture(rawPublishedHandle) &&
			rawPublishedOwner.PublicationGeneration() == 1 &&
			resources.IsValid(rawPublishedHandle),
			"raw destruction cannot bypass a published cleanup token");
	}
	result |= Check(device.DestroyCount() == rawPublishedDestroys + 1 &&
		!resources.IsValid(rawPublishedHandle) && device.LiveCount() == 0,
		"the published token remains intact for its owner destructor and later shutdown");

	NativeW3DTextureOwner owner;
	NativeW3DTextureCandidate first;
	result |= Check(owner.CreateCandidate(cpuDescriptor, cpuData, 2, &first) ==
		RENDER_RESULT_OK && first.IsValid() &&
		owner.PublishCandidate(&first, 0) == RENDER_RESULT_OK &&
		!first.IsValid() && owner.PublicationGeneration() == 1,
		"the first candidate is atomically consumed at publication generation one");
	NativeW3DTextureHandle firstHandle;
	result |= Check(owner.AcquireForSampling(&firstHandle) == RENDER_RESULT_OK &&
		firstHandle.isValid(),
		"CPU-authoritative content acquires its exact typed sampling handle");
	const unsigned int generationBeforeRawPublicationDestroy =
		owner.PublicationGeneration();
	result |= Check(!resources.DestroyTexture(firstHandle) &&
		!resources.Destroy(firstHandle.resource) &&
		!resources.RetireTexture(firstHandle) &&
		resources.IsValid(firstHandle) &&
		owner.PublicationGeneration() ==
			generationBeforeRawPublicationDestroy,
		"raw destruction cannot orphan a published owner cleanup token");

	const unsigned int foreignThreadDestroys = device.DestroyCount();
	{
		NativeW3DTextureCandidate foreignReplacement;
		NativeW3DTextureCandidate foreignCreated;
		NativeW3DTextureCandidate foreignBorrowed;
		result |= Check(owner.CreateCandidate(cpuDescriptor, cpuData, 2,
			&foreignReplacement) == RENDER_RESULT_OK,
			"the foreign-thread fixture prepares one owner-thread candidate");
		const NativeW3DTextureHandle foreignReplacementHandle =
			foreignReplacement.Handle();
		RenderResult foreignCreateResult = RENDER_RESULT_OK;
		RenderResult foreignBorrowResult = RENDER_RESULT_OK;
		RenderResult foreignPublishResult = RENDER_RESULT_OK;
		RenderResult foreignResetResult = RENDER_RESULT_OK;
		RenderResult foreignUnbindResult = RENDER_RESULT_OK;
		bool foreignRetireResult = true;
		bool foreignDestroyResult = true;
		std::thread foreignThread([&]() {
			foreignCreateResult = owner.CreateCandidate(cpuDescriptor, cpuData,
				2, &foreignCreated);
			foreignBorrowResult = owner.BorrowCandidate(firstHandle,
				cpuDescriptor, &foreignBorrowed);
			foreignRetireResult = resources.RetireTexture(firstHandle);
			foreignDestroyResult = resources.DestroyTexture(firstHandle);
			foreignPublishResult = owner.PublishCandidate(&foreignReplacement,
				owner.PublicationGeneration());
			foreignResetResult = owner.Reset();
			foreignUnbindResult =
				UnbindNativeW3DTextureResources(&resources);
		});
		foreignThread.join();

		NativeW3DTextureDescription firstDescription;
		NativeW3DTextureHandle stillPublished = firstHandle;
		result |= Check(foreignCreateResult == RENDER_RESULT_INVALID_ARGUMENT &&
			foreignBorrowResult == RENDER_RESULT_INVALID_ARGUMENT &&
			foreignPublishResult == RENDER_RESULT_INVALID_ARGUMENT &&
			foreignResetResult == RENDER_RESULT_INVALID_ARGUMENT &&
			foreignUnbindResult == RENDER_RESULT_INVALID_ARGUMENT &&
			!foreignRetireResult && !foreignDestroyResult &&
			!foreignCreated.IsValid() && !foreignBorrowed.IsValid() &&
			foreignReplacement.IsValid() &&
			resources.IsValid(foreignReplacementHandle) &&
			owner.PublicationGeneration() == 1 &&
			owner.AcquireForSampling(&stillPublished) == RENDER_RESULT_OK &&
			stillPublished.resource == firstHandle.resource &&
			resources.DescribeTexture(firstHandle.resource,
				&firstDescription) == RENDER_RESULT_OK &&
			firstDescription.authority == NATIVE_W3D_CONTENT_CPU &&
			device.DestroyCount() == foreignThreadDestroys,
			"foreign mutations preserve candidate, publication, authority, and slots");
	}
	result |= Check(device.DestroyCount() == foreignThreadDestroys + 1 &&
		device.LiveCount() == 1,
		"the retained foreign publication candidate abandons once on the owner");

	NativeW3DTextureCandidate *foreignAbandoned =
		new NativeW3DTextureCandidate();
	result |= Check(owner.CreateCandidate(cpuDescriptor, cpuData, 2,
		foreignAbandoned) == RENDER_RESULT_OK,
		"the foreign-abandon fixture creates an owned candidate on the owner");
	const NativeW3DTextureHandle foreignAbandonedHandle =
		foreignAbandoned->Handle();
	const unsigned int beforeForeignAbandon = device.DestroyCount();
	std::thread foreignDestructor([&]() { delete foreignAbandoned; });
	foreignDestructor.join();
	unsigned int foreignAbandonDrained = 0;
	result |= Check(device.DestroyCount() == beforeForeignAbandon &&
		resources.IsValid(foreignAbandonedHandle) &&
		host.PendingCleanup() == 1 &&
		resources.Shutdown() == RENDER_RESULT_FAILED &&
		resources.IsValid(foreignAbandonedHandle) &&
		host.DrainCleanup(0, &foreignAbandonDrained) == RENDER_RESULT_OK &&
		foreignAbandonDrained == 1 && host.PendingCleanup() == 0 &&
		!resources.IsValid(foreignAbandonedHandle) &&
		device.DestroyCount() == beforeForeignAbandon + 1,
		"a foreign candidate destructor transfers its sole token and blocks premature teardown");

	NativeW3DTextureOwner *foreignOwnedOwner = new NativeW3DTextureOwner();
	NativeW3DTextureCandidate foreignOwnedPublication;
	result |= Check(foreignOwnedOwner->CreateCandidate(cpuDescriptor, cpuData,
		2, &foreignOwnedPublication) == RENDER_RESULT_OK &&
		foreignOwnedOwner->PublishCandidate(&foreignOwnedPublication, 0) ==
			RENDER_RESULT_OK &&
		foreignOwnedOwner->PublicationGeneration() == 1,
		"the foreign owner-destructor fixture publishes one owned token");
	NativeW3DTextureHandle foreignOwnedHandle;
	result |= Check(foreignOwnedOwner->AcquireForSampling(
		&foreignOwnedHandle) == RENDER_RESULT_OK,
		"the foreign owner-destructor fixture exposes its live publication");
	const unsigned int beforeForeignOwner = device.DestroyCount();
	std::thread foreignOwnerDestructor([&]() { delete foreignOwnedOwner; });
	foreignOwnerDestructor.join();
	unsigned int foreignOwnerDrained = 0;
	result |= Check(device.DestroyCount() == beforeForeignOwner &&
		resources.IsValid(foreignOwnedHandle) && host.PendingCleanup() == 1 &&
		host.DrainCleanup(0, &foreignOwnerDrained) == RENDER_RESULT_OK &&
		foreignOwnerDrained == 1 && host.PendingCleanup() == 0 &&
		!resources.IsValid(foreignOwnedHandle) &&
		device.DestroyCount() == beforeForeignOwner + 1,
		"a foreign owner destructor transfers its owned publication exactly once");

	NativeW3DSurfaceHandle topSurface;
	NativeW3DSurfaceHandle lowerSurface;
	result |= Check(owner.AcquireSurface(0, 0, &topSurface) ==
		RENDER_RESULT_OK && topSurface.isValid() && topSurface.width == 4 &&
		topSurface.height == 4 &&
		owner.AcquireSurface(1, 0, &lowerSurface) == RENDER_RESULT_OK &&
		lowerSurface.isValid() && lowerSurface.width == 2 &&
		lowerSurface.height == 2,
		"surface acquisition returns typed tokens with exact mip dimensions");
	NativeW3DSurfaceHandle invalidSurface = topSurface;
	result |= Check(owner.AcquireSurface(2, 0, &invalidSurface) ==
		RENDER_RESULT_INVALID_ARGUMENT && !invalidSurface.isValid(),
		"out-of-range surface acquisition clears its output token");

	device.FailCreate(true);
	NativeW3DTextureCandidate failedCreate;
	result |= Check(owner.CreateCandidate(cpuDescriptor, cpuData, 2,
		&failedCreate) == RENDER_RESULT_FAILED && !failedCreate.IsValid(),
		"backend creation failure never produces a candidate token");
	NativeW3DTextureHandle preserved = firstHandle;
	result |= Check(owner.AcquireForSampling(&preserved) == RENDER_RESULT_OK &&
		preserved.resource == firstHandle.resource &&
		owner.PublicationGeneration() == 1,
		"candidate creation failure preserves the old valid image and generation");
	device.FailCreate(false);

	const unsigned int staleCandidateDestroys = device.DestroyCount();
	{
		NativeW3DTextureCandidate stalePublication;
		result |= Check(owner.CreateCandidate(cpuDescriptor, cpuData, 2,
			&stalePublication) == RENDER_RESULT_OK &&
			owner.PublishCandidate(&stalePublication, 0) ==
				RENDER_RESULT_FAILED && stalePublication.IsValid(),
			"a stale expected publication generation cannot replace the image");
		preserved = firstHandle;
		result |= Check(owner.AcquireForSampling(&preserved) ==
			RENDER_RESULT_OK && preserved.resource == firstHandle.resource,
			"stale publication failure leaves the previous sampling token current");
	}
	result |= Check(device.DestroyCount() == staleCandidateDestroys + 1,
		"a rejected owned publication remains an exactly-once candidate token");

	NativeW3DTextureCandidate gpuCandidate;
	result |= Check(owner.CreateCandidate(gpuDescriptor, 0, 0,
		&gpuCandidate) == RENDER_RESULT_OK && gpuCandidate.IsValid() &&
		owner.PublishCandidate(&gpuCandidate, owner.PublicationGeneration()) ==
			RENDER_RESULT_FAILED && gpuCandidate.IsValid(),
		"an uninitialized texture candidate cannot displace valid content");
	NativeW3DGpuContentLease creationLease;
	result |= Check(resources.CopyActiveColorTargetToTexture(
		gpuCandidate.Handle().resource, &creationLease) == RENDER_RESULT_OK &&
		creationLease.isValid() &&
		owner.PublishCandidate(&gpuCandidate, owner.PublicationGeneration()) ==
			RENDER_RESULT_OK && !gpuCandidate.IsValid() &&
		owner.PublicationGeneration() == 2 &&
		!resources.IsValid(firstHandle),
		"GPU-authoritative content publishes only after the old owned image is released");

	NativeW3DTextureHandle gpuHandle;
	result |= Check(owner.AcquireForSampling(&gpuHandle) ==
		RENDER_RESULT_FAILED && !gpuHandle.isValid(),
		"GPU content cannot be sampled without an exact lease token");
	NativeW3DGpuContentLease gpuLease;
	result |= Check(owner.AcquireForSampling(&gpuHandle, &gpuLease) ==
		RENDER_RESULT_OK && gpuHandle.isValid() && gpuLease.isValid() &&
		gpuLease.resource == gpuHandle.resource,
		"GPU sampling acquires the exact current authority lease");
	const NativeW3DGpuContentLease staleLease = gpuLease;
	NativeW3DGpuContentLease nextLease;
	result |= Check(owner.CopyActiveColorTarget(&nextLease) ==
		RENDER_RESULT_OK && nextLease.isValid() &&
		nextLease.authorityEpoch != staleLease.authorityEpoch,
		"a later GPU write advances the authority epoch");
	gpuLease = staleLease;
	NativeW3DTextureHandle staleGpuHandle = gpuHandle;
	result |= Check(owner.AcquireForSampling(&staleGpuHandle, &gpuLease) ==
		RENDER_RESULT_INVALID_ARGUMENT && !staleGpuHandle.isValid() &&
		!gpuLease.isValid(),
		"a stale GPU lease and its sampling output fail closed together");

	TextureSubresourceData gpuCpuData;
	gpuCpuData.data = topBytes;
	gpuCpuData.rowPitch = 16;
	gpuCpuData.slicePitch = 64;
	result |= Check(owner.RefreshCpuContent(gpuDescriptor,
		&gpuCpuData, 1) == RENDER_RESULT_OK,
		"the authority fixture can publish a complete CPU refresh");
	gpuLease = nextLease;
	staleGpuHandle = gpuHandle;
	result |= Check(owner.AcquireForSampling(&staleGpuHandle, &gpuLease) ==
		RENDER_RESULT_FAILED && !staleGpuHandle.isValid() &&
		!gpuLease.isValid(),
		"CPU authority rejects a caller's stale GPU-content claim");
	staleGpuHandle = NativeW3DTextureHandle();
	result |= Check(owner.AcquireForSampling(&staleGpuHandle) ==
		RENDER_RESULT_OK && staleGpuHandle.resource == gpuHandle.resource,
		"a complete CPU refresh restores lease-free sampling authority");

	NativeW3DSurfaceHandle beforeRecovery;
	NativeW3DGpuContentLease beforeRecoveryLease;
	result |= Check(owner.AcquireOutputSurface(0, 0, &beforeRecovery) ==
		RENDER_RESULT_OK && beforeRecovery.isValid() &&
		owner.PublishOutputWrite(beforeRecovery, &beforeRecoveryLease) ==
			RENDER_RESULT_OK && beforeRecoveryLease.isValid() &&
		owner.AcquireForSampling(&staleGpuHandle, &beforeRecoveryLease) ==
			RENDER_RESULT_OK &&
		device.recoverDevice() == RENDER_RESULT_OK &&
		host.ReplaceContext(device.immediateContext()) == RENDER_RESULT_OK &&
		device.RecoveryCount() == 1,
		"accepted output publication produces a lease before context replacement");
	staleGpuHandle = gpuHandle;
	result |= Check(owner.AcquireForSampling(&staleGpuHandle,
			&beforeRecoveryLease) != RENDER_RESULT_OK &&
		!staleGpuHandle.isValid() && !beforeRecoveryLease.isValid(),
		"backend recovery expires GPU output authority and its sampling lease");
	NativeW3DSurfaceHandle afterRecovery = beforeRecovery;
	result |= Check(owner.AcquireOutputSurface(0, 0, &afterRecovery) ==
		RENDER_RESULT_INVALID_ARGUMENT && !afterRecovery.isValid(),
		"a cached output surface token expires across the backend recovery epoch");
	NativeW3DGpuContentLease regeneratedLease;
	result |= Check(owner.AcquireOutputSurface(0, 0, &afterRecovery) ==
		RENDER_RESULT_OK && afterRecovery.isValid() &&
		afterRecovery.backendEpoch != beforeRecovery.backendEpoch &&
		owner.PublishOutputWrite(afterRecovery, &regeneratedLease) ==
			RENDER_RESULT_OK && regeneratedLease.isValid() &&
		regeneratedLease.backendEpoch != staleLease.backendEpoch,
		"a recovered output reacquires its typed surface and publishes regenerated GPU authority");
	staleGpuHandle = gpuHandle;
	result |= Check(owner.AcquireForSampling(&staleGpuHandle,
			&regeneratedLease) == RENDER_RESULT_OK &&
		staleGpuHandle.resource == gpuHandle.resource,
		"regenerated output is sampleable only with its new exact lease");

	device.FailRefresh(true);
	result |= Check(owner.RefreshCpuContent(gpuDescriptor,
		&gpuCpuData, 1) == RENDER_RESULT_FAILED &&
		resources.IsValid(gpuHandle),
		"a mutation fault leaves the typed resource live while invalidating content");
	NativeW3DTextureHandle authorityRejected = gpuHandle;
	const RenderResult authorityResult =
		owner.AcquireForSampling(&authorityRejected);
	result |= Check(authorityResult != RENDER_RESULT_OK &&
		!authorityRejected.isValid(),
		"sampling validates DescribeTexture authority instead of only IsValid");
	device.FailRefresh(false);
	result |= Check(owner.RefreshCpuContent(gpuDescriptor,
		&gpuCpuData, 1) == RENDER_RESULT_OK &&
		owner.AcquireForSampling(&authorityRejected) == RENDER_RESULT_OK,
		"a complete republish restores CPU sampling after authority invalidation");

	const unsigned int resetDestroys = device.DestroyCount();
	const unsigned int generationBeforeReset = owner.PublicationGeneration();
	result |= Check(owner.Reset() == RENDER_RESULT_OK &&
		device.DestroyCount() == resetDestroys + 1 &&
		owner.PublicationGeneration() == generationBeforeReset + 1 &&
		owner.Reset() == RENDER_RESULT_OK &&
		device.DestroyCount() == resetDestroys + 1,
		"owner reset destroys an owned texture exactly once and invalidates publication");

	NativeW3DTextureHandle sharedMissing;
	result |= Check(resources.CreateTexture(cpuDescriptor, cpuData, 2,
		&sharedMissing) == RENDER_RESULT_OK && sharedMissing.isValid(),
		"the missing-texture fixture creates one shared CPU-authoritative image");
	const unsigned int sharedDestroys = device.DestroyCount();
	{
		NativeW3DTextureCandidate borrowedAbandoned;
		TextureDescriptor wrongDescriptor = cpuDescriptor;
		wrongDescriptor.width = 8;
		result |= Check(owner.BorrowCandidate(sharedMissing, wrongDescriptor,
			&borrowedAbandoned) == RENDER_RESULT_FAILED &&
			!borrowedAbandoned.IsValid() &&
			owner.BorrowCandidate(sharedMissing, cpuDescriptor,
				&borrowedAbandoned) == RENDER_RESULT_OK &&
			borrowedAbandoned.IsValid(),
			"borrowed candidates require the shared handle's exact descriptor");
	}
	result |= Check(device.DestroyCount() == sharedDestroys &&
		resources.IsValid(sharedMissing),
		"abandoning a borrowed missing texture never destroys shared ownership");

	NativeW3DTextureCandidate *foreignBorrowedCandidate =
		new NativeW3DTextureCandidate();
	result |= Check(owner.BorrowCandidate(sharedMissing, cpuDescriptor,
		foreignBorrowedCandidate) == RENDER_RESULT_OK &&
		foreignBorrowedCandidate->IsValid(),
		"the foreign borrowed-candidate fixture holds a non-owning token");
	std::thread foreignBorrowedCandidateDestructor(
		[&]() { delete foreignBorrowedCandidate; });
	foreignBorrowedCandidateDestructor.join();
	result |= Check(device.DestroyCount() == sharedDestroys &&
		host.PendingCleanup() == 0 && resources.IsValid(sharedMissing),
		"a foreign borrowed-candidate destructor never queues or destroys cleanup");

	NativeW3DTextureOwner *foreignBorrowedOwner =
		new NativeW3DTextureOwner();
	NativeW3DTextureCandidate foreignBorrowedPublication;
	result |= Check(foreignBorrowedOwner->BorrowCandidate(sharedMissing,
		cpuDescriptor, &foreignBorrowedPublication) == RENDER_RESULT_OK &&
		foreignBorrowedOwner->PublishCandidate(&foreignBorrowedPublication, 0) ==
			RENDER_RESULT_OK &&
		foreignBorrowedOwner->PublicationGeneration() == 1,
		"the foreign borrowed-owner fixture publishes without cleanup ownership");
	RenderResult foreignBorrowedReset = RENDER_RESULT_OK;
	std::thread foreignBorrowedOwnerDestructor([&]() {
		foreignBorrowedReset = foreignBorrowedOwner->Reset();
		delete foreignBorrowedOwner;
	});
	foreignBorrowedOwnerDestructor.join();
	result |= Check(foreignBorrowedReset == RENDER_RESULT_INVALID_ARGUMENT &&
		device.DestroyCount() == sharedDestroys && host.PendingCleanup() == 0 &&
		resources.IsValid(sharedMissing),
		"foreign borrowed reset fails closed and its destructor never destroys shared data");

	NativeW3DTextureCandidate borrowedMissing;
	result |= Check(owner.BorrowCandidate(sharedMissing, cpuDescriptor,
		&borrowedMissing) == RENDER_RESULT_OK &&
		owner.PublishCandidate(&borrowedMissing,
			owner.PublicationGeneration()) == RENDER_RESULT_OK &&
		owner.Reset() == RENDER_RESULT_OK &&
		device.DestroyCount() == sharedDestroys &&
		resources.IsValid(sharedMissing),
		"resetting a published borrowed missing texture never destroys it");

	NativeW3DTextureCandidate borrowedAgain;
	NativeW3DTextureCandidate ownedReplacement;
	result |= Check(owner.BorrowCandidate(sharedMissing, cpuDescriptor,
		&borrowedAgain) == RENDER_RESULT_OK &&
		owner.PublishCandidate(&borrowedAgain,
			owner.PublicationGeneration()) == RENDER_RESULT_OK &&
		owner.CreateCandidate(cpuDescriptor, cpuData, 2, &ownedReplacement) ==
			RENDER_RESULT_OK &&
		owner.PublishCandidate(&ownedReplacement,
			owner.PublicationGeneration()) == RENDER_RESULT_OK &&
		resources.IsValid(sharedMissing) &&
		device.DestroyCount() == sharedDestroys,
		"replacing a borrowed missing image does not destroy its shared handle");
	result |= Check(owner.Reset() == RENDER_RESULT_OK &&
		device.DestroyCount() == sharedDestroys + 1 &&
		resources.IsValid(sharedMissing),
		"only the owned replacement is destroyed; the shared handle remains borrowed");

	// Explicit reset is bound-generation strict, but destructors retain the
	// original registry identity so unbinding or rebinding cannot consume the
	// last owned cleanup token.
	const unsigned int beforeStaleLifecycle = device.DestroyCount();
	NativeW3DTextureHandle staleOwnedPublicationHandle;
	NativeW3DTextureHandle unboundCandidateHandle;
	NativeW3DTextureHandle reboundCandidateHandle;
	{
		NativeW3DTextureOwner staleOwnedOwner;
		NativeW3DTextureCandidate staleOwnedPublication;
		NativeW3DTextureOwner staleBorrowedOwner;
		NativeW3DTextureCandidate staleBorrowedPublication;
		result |= Check(staleOwnedOwner.CreateCandidate(cpuDescriptor, cpuData,
			2, &staleOwnedPublication) == RENDER_RESULT_OK &&
			staleOwnedOwner.PublishCandidate(&staleOwnedPublication, 0) ==
				RENDER_RESULT_OK &&
			staleOwnedOwner.AcquireForSampling(
				&staleOwnedPublicationHandle) == RENDER_RESULT_OK &&
			staleBorrowedOwner.BorrowCandidate(sharedMissing, cpuDescriptor,
				&staleBorrowedPublication) == RENDER_RESULT_OK &&
			staleBorrowedOwner.PublishCandidate(&staleBorrowedPublication, 0) ==
				RENDER_RESULT_OK,
			"owned and borrowed reset fixtures publish before boundary unbind");

		NativeW3DTextureCandidate *unboundOwned =
			new NativeW3DTextureCandidate();
		NativeW3DTextureCandidate *unboundBorrowed =
			new NativeW3DTextureCandidate();
		NativeW3DTextureCandidate *reboundOwned =
			new NativeW3DTextureCandidate();
		NativeW3DTextureCandidate *reboundBorrowed =
			new NativeW3DTextureCandidate();
		result |= Check(staleOwnedOwner.CreateCandidate(cpuDescriptor, cpuData,
			2, unboundOwned) == RENDER_RESULT_OK &&
			staleOwnedOwner.BorrowCandidate(sharedMissing, cpuDescriptor,
				unboundBorrowed) == RENDER_RESULT_OK &&
			staleOwnedOwner.CreateCandidate(cpuDescriptor, cpuData, 2,
				reboundOwned) == RENDER_RESULT_OK &&
			staleOwnedOwner.BorrowCandidate(sharedMissing, cpuDescriptor,
				reboundBorrowed) == RENDER_RESULT_OK,
			"owned and borrowed candidates retain exact pre-unbind identities");
		unboundCandidateHandle = unboundOwned->Handle();
		reboundCandidateHandle = reboundOwned->Handle();

		result |= Check(UnbindNativeW3DTextureResources(&differentResources) ==
				RENDER_RESULT_INVALID_ARGUMENT &&
			UnbindNativeW3DTextureResources(&resources) == RENDER_RESULT_OK,
			"only the exact registry can remove the current texture binding");
		const unsigned int ownedGeneration =
			staleOwnedOwner.PublicationGeneration();
		const unsigned int borrowedGeneration =
			staleBorrowedOwner.PublicationGeneration();
		result |= Check(staleOwnedOwner.Reset() == RENDER_RESULT_FAILED &&
			staleBorrowedOwner.Reset() == RENDER_RESULT_FAILED &&
			staleOwnedOwner.PublicationGeneration() == ownedGeneration &&
			staleBorrowedOwner.PublicationGeneration() == borrowedGeneration &&
			resources.IsValid(staleOwnedPublicationHandle) &&
			resources.IsValid(sharedMissing),
			"unbound reset preserves owned and borrowed publications and generations");

		const unsigned int beforeUnboundDestructors = device.DestroyCount();
		delete unboundOwned;
		delete unboundBorrowed;
		result |= Check(device.DestroyCount() == beforeUnboundDestructors + 1 &&
			!resources.IsValid(unboundCandidateHandle) &&
			resources.IsValid(sharedMissing) && host.PendingCleanup() == 0,
			"unbound destructors retire owned tokens and ignore borrowed tokens");

		result |= Check(BindNativeW3DTextureResources(&resources) ==
				RENDER_RESULT_OK &&
			staleOwnedOwner.Reset() == RENDER_RESULT_FAILED &&
			staleBorrowedOwner.Reset() == RENDER_RESULT_FAILED &&
			staleOwnedOwner.PublicationGeneration() == ownedGeneration &&
			staleBorrowedOwner.PublicationGeneration() == borrowedGeneration,
			"rebind-stale reset remains fail-closed without consuming either publication");
		const unsigned int beforeReboundDestructors = device.DestroyCount();
		delete reboundOwned;
		delete reboundBorrowed;
		result |= Check(device.DestroyCount() == beforeReboundDestructors + 1 &&
			!resources.IsValid(reboundCandidateHandle) &&
			resources.IsValid(staleOwnedPublicationHandle) &&
			resources.IsValid(sharedMissing) && host.PendingCleanup() == 0,
			"rebind-stale candidate destructors use original cleanup ownership");

		NativeW3DTextureCandidate currentBindingCandidate;
		result |= Check(staleOwnedOwner.CreateCandidate(cpuDescriptor, cpuData,
			2, &currentBindingCandidate) == RENDER_RESULT_OK &&
			staleOwnedOwner.PublishCandidate(&currentBindingCandidate,
				ownedGeneration) == RENDER_RESULT_FAILED &&
			currentBindingCandidate.IsValid(),
			"a current candidate cannot publish through a stale owner token");
	}
	result |= Check(device.DestroyCount() == beforeStaleLifecycle + 4 &&
		!resources.IsValid(staleOwnedPublicationHandle) &&
		resources.IsValid(sharedMissing) && host.PendingCleanup() == 0,
		"stale owner destruction retires owned publication and never destroys borrowed data");
	result |= Check(resources.DestroyTexture(sharedMissing) &&
		device.LiveCount() == 0,
		"the explicit shared owner alone destroys the borrowed missing texture");
	result |= Check(UnbindNativeW3DTextureResources(&resources) ==
		RENDER_RESULT_OK && resources.Shutdown() == RENDER_RESULT_OK &&
		device.LiveCount() == 0 && host.Detach() == RENDER_RESULT_OK,
		"registry shutdown follows complete ticket retirement before host detach");
	device.shutdown();

	// Verify the required successful teardown order independently of the
	// stale/unbound lifecycle stress above.
	FakeRenderDevice orderedDevice;
	NativeW3DResourceHost orderedHost(8);
	NativeW3DResources orderedResources(8);
	result |= Check(orderedHost.Attach(&orderedDevice,
		orderedDevice.immediateContext()) == RENDER_RESULT_OK &&
		orderedResources.BindHost(&orderedHost) == RENDER_RESULT_OK &&
		BindNativeW3DTextureResources(&orderedResources) == RENDER_RESULT_OK,
		"the ordered teardown fixture binds a fresh registry generation");
	{
		NativeW3DTextureOwner orderedOwner;
		NativeW3DTextureCandidate orderedCandidate;
		result |= Check(orderedOwner.CreateCandidate(cpuDescriptor, cpuData, 2,
			&orderedCandidate) == RENDER_RESULT_OK &&
			orderedOwner.PublishCandidate(&orderedCandidate, 0) ==
				RENDER_RESULT_OK && orderedOwner.Reset() == RENDER_RESULT_OK &&
			orderedDevice.LiveCount() == 0,
			"the product owner resets its texture before registry teardown");
	}
	result |= Check(UnbindNativeW3DTextureResources(&orderedResources) ==
		RENDER_RESULT_OK && orderedResources.Shutdown() == RENDER_RESULT_OK &&
		orderedHost.Detach() == RENDER_RESULT_OK &&
		orderedDevice.LiveCount() == 0,
		"teardown orders owner reset, boundary unbind, registry shutdown, and host detach");
	orderedDevice.shutdown();

	return result;
}

int TestNativeD3D11RetirementFault()
{
	using namespace rts::render;
	int result = 0;
	IRenderDevice *device = CreateD3D11RenderDevice();
	result |= Check(device != 0,
		"the real destruction-fault fixture allocates a D3D11 backend");
	if (device == 0)
	{
		return result;
	}

	RenderDeviceParameters parameters;
	parameters.backend = RENDER_BACKEND_D3D11;
	parameters.width = 8;
	parameters.height = 8;
	parameters.window = 0;
	parameters.enableDebugLayer = false;
	parameters.allowSoftwareFallback = true;
	result |= Check(device->initialize(parameters) == RENDER_RESULT_OK,
		"the real destruction-fault fixture initializes headlessly");
	if (!device->isOperational())
	{
		delete device;
		return result;
	}

	unsigned char topBytes[64] = { 0 };
	unsigned char lowerBytes[16] = { 0 };
	TextureSubresourceData cpuData[2];
	MakeCpuData(topBytes, lowerBytes, cpuData);
	const TextureDescriptor descriptor = MakeCpuDescriptor();
	NativeW3DResourceHost host(8);
	NativeW3DResources resources(8);
	result |= Check(host.Attach(device, device->immediateContext()) ==
		RENDER_RESULT_OK && resources.BindHost(&host) == RENDER_RESULT_OK &&
		BindNativeW3DTextureResources(&resources) == RENDER_RESULT_OK,
		"the real destruction-fault fixture binds the native registry");

	NativeW3DTextureOwner owner;
	NativeW3DTextureCandidate firstCandidate;
	result |= Check(owner.CreateCandidate(descriptor, cpuData, 2,
		&firstCandidate) == RENDER_RESULT_OK &&
		owner.PublishCandidate(&firstCandidate, 0) == RENDER_RESULT_OK,
		"the real destruction-fault fixture publishes its original image");
	NativeW3DTextureHandle firstHandle;
	result |= Check(owner.AcquireForSampling(&firstHandle) == RENDER_RESULT_OK,
		"the original real D3D11 image is CPU-authoritative");

	NativeW3DTextureCandidate replacement;
	result |= Check(owner.CreateCandidate(descriptor, cpuData, 2,
		&replacement) == RENDER_RESULT_OK,
		"a real D3D11 replacement is ready before retirement");
	const NativeW3DTextureHandle replacementHandle = replacement.Handle();
	const unsigned int generationBefore = owner.PublicationGeneration();
	result |= Check(device->configureResourceFaultInjection(
		RENDER_RESOURCE_FAULT_TEXTURE_DESTRUCTION, 1,
		RENDER_RESULT_FAILED) == RENDER_RESULT_OK &&
		owner.PublishCandidate(&replacement, generationBefore) ==
			RENDER_RESULT_OK && !replacement.IsValid() &&
		owner.PublicationGeneration() == generationBefore + 1,
		"a real backend destroy fault defers retirement and still publishes");

	NativeW3DTextureHandle current;
	NativeW3DTextureDescription retiredDescription;
	result |= Check(owner.AcquireForSampling(&current) == RENDER_RESULT_OK &&
		current.resource == replacementHandle.resource &&
		resources.DescribeTexture(current.resource, &retiredDescription) ==
			RENDER_RESULT_OK &&
		retiredDescription.authority == NATIVE_W3D_CONTENT_CPU &&
		!resources.IsValid(firstHandle) &&
		resources.DescribeTexture(firstHandle.resource, &retiredDescription) ==
			RENDER_RESULT_INVALID_ARGUMENT,
		"retirement hides only the old slot and preserves new content authority");

	RenderResourceStatistics deferredStatistics;
	result |= Check(device->getDebugResourceStatistics(&deferredStatistics) ==
		RENDER_RESULT_OK && deferredStatistics.liveHandles == 2 &&
		deferredStatistics.textureCount == 2,
		"the real backend retains the faulted old allocation for shutdown retry");

	NativeW3DTextureCandidate *foreignFaultedCandidate =
		new NativeW3DTextureCandidate();
	result |= Check(owner.CreateCandidate(descriptor, cpuData, 2,
		foreignFaultedCandidate) == RENDER_RESULT_OK,
		"the real foreign-destructor fault fixture owns one unpublished texture");
	const NativeW3DTextureHandle foreignFaultedHandle =
		foreignFaultedCandidate->Handle();
	const unsigned int generationBeforeForeignFault =
		owner.PublicationGeneration();
	result |= Check(device->configureResourceFaultInjection(
		RENDER_RESOURCE_FAULT_TEXTURE_DESTRUCTION, 1,
		RENDER_RESULT_FAILED) == RENDER_RESULT_OK,
		"the real backend arms a foreign-destructor destruction refusal");
	std::thread foreignFaultedDestructor(
		[&]() { delete foreignFaultedCandidate; });
	foreignFaultedDestructor.join();
	unsigned int foreignFaultDrained = 0;
	RenderResourceStatistics transferredStatistics;
	result |= Check(host.PendingCleanup() == 1 &&
		resources.IsValid(foreignFaultedHandle) &&
		owner.PublicationGeneration() == generationBeforeForeignFault &&
		host.DrainCleanup(0, &foreignFaultDrained) == RENDER_RESULT_OK &&
		foreignFaultDrained == 1 && host.PendingCleanup() == 0 &&
		!resources.IsValid(foreignFaultedHandle) &&
		owner.PublicationGeneration() == generationBeforeForeignFault &&
		device->getDebugResourceStatistics(&transferredStatistics) ==
			RENDER_RESULT_OK && transferredStatistics.liveHandles == 3 &&
		transferredStatistics.textureCount == 3,
		"foreign destruction transfers the token before registry retirement absorbs a real fault");

	const RenderResult resetResult = owner.Reset();
	const RenderResult unbindResult =
		UnbindNativeW3DTextureResources(&resources);
	const RenderResult shutdownResult = resources.Shutdown();
	RenderResourceStatistics finalStatistics;
	const RenderResult statisticsResult =
		device->getDebugResourceStatistics(&finalStatistics);
	const RenderResult detachResult = host.Detach();
	result |= Check(resetResult == RENDER_RESULT_OK &&
		unbindResult == RENDER_RESULT_OK &&
		shutdownResult == RENDER_RESULT_OK &&
		statisticsResult == RENDER_RESULT_OK &&
		HasNoLiveResources(finalStatistics) &&
		detachResult == RENDER_RESULT_OK,
		"shutdown retries the retired allocation and leaves no real D3D11 resource");

	device->shutdown();
	delete device;
	return result;
}

int TestLateRegistryTicketLifetime()
{
	using namespace rts::render;
	int result = 0;
	unsigned char topBytes[64] = { 0 };
	unsigned char lowerBytes[16] = { 0 };
	TextureSubresourceData cpuData[2];
	MakeCpuData(topBytes, lowerBytes, cpuData);
	const TextureDescriptor descriptor = MakeCpuDescriptor();

	FakeRenderDevice candidateDevice;
	NativeW3DResourceHost candidateHost(8);
	NativeW3DTextureOwner candidateFactory;
	NativeW3DTextureCandidate *lateCandidate =
		new NativeW3DTextureCandidate();
	result |= Check(candidateHost.Attach(&candidateDevice,
		candidateDevice.immediateContext()) == RENDER_RESULT_OK,
		"the late-candidate fixture attaches a stable resource host");
	NativeW3DTextureHandle lateCandidateHandle;
	{
		NativeW3DResources *resources = new NativeW3DResources(8);
		result |= Check(resources->BindHost(&candidateHost) == RENDER_RESULT_OK &&
			BindNativeW3DTextureResources(resources) == RENDER_RESULT_OK &&
			candidateFactory.CreateCandidate(descriptor, cpuData, 2,
				lateCandidate) == RENDER_RESULT_OK,
			"an owned candidate retains stable table identity beyond the facade scope");
		lateCandidateHandle = lateCandidate->Handle();
		result |= Check(UnbindNativeW3DTextureResources(resources) ==
				RENDER_RESULT_OK &&
			resources->Shutdown() == RENDER_RESULT_FAILED,
			"an external candidate token blocks explicit registry shutdown");
		delete resources;
	}
	result |= Check(lateCandidateHandle.isValid() &&
		candidateDevice.LiveCount() == 1 &&
		candidateHost.BoundResourceTables() == 1 &&
		candidateHost.PendingCleanup() == 1,
		"registry scope exit transfers its table without deleting the candidate token");
	candidateDevice.FailDestroy(true);
	delete lateCandidate;
	candidateDevice.FailDestroy(false);
	result |= Check(candidateDevice.LiveCount() == 1 &&
		candidateDevice.DestroyCount() == 0 &&
		candidateHost.PendingCleanup() == 1,
		"late current-thread teardown transfers a destroy fault to table retirement");
	unsigned int candidateDrained = 0;
	result |= Check(candidateHost.DrainCleanup(0, &candidateDrained) ==
			RENDER_RESULT_OK && candidateDrained == 1 &&
		candidateHost.PendingCleanup() == 0 &&
		candidateHost.BoundResourceTables() == 0 &&
		candidateDevice.DestroyCount() == 1 &&
		candidateDevice.LiveCount() == 0 &&
		candidateHost.Detach() == RENDER_RESULT_OK,
		"table fallback retries the fault and releases its final reference exactly once");
	candidateDevice.shutdown();

	FakeRenderDevice ownerDevice;
	NativeW3DResourceHost ownerHost(8);
	NativeW3DTextureOwner *lateOwners[2] = {
		new NativeW3DTextureOwner(), new NativeW3DTextureOwner()
	};
	result |= Check(ownerHost.Attach(&ownerDevice,
		ownerDevice.immediateContext()) == RENDER_RESULT_OK,
		"the late-owner fixture attaches a stable resource host");
	{
		NativeW3DResources *resources = new NativeW3DResources(8);
		NativeW3DTextureCandidate publications[2];
		result |= Check(resources->BindHost(&ownerHost) == RENDER_RESULT_OK &&
			BindNativeW3DTextureResources(resources) == RENDER_RESULT_OK &&
			lateOwners[0]->CreateCandidate(descriptor, cpuData, 2,
				&publications[0]) == RENDER_RESULT_OK &&
			lateOwners[0]->PublishCandidate(&publications[0], 0) ==
				RENDER_RESULT_OK &&
			lateOwners[1]->CreateCandidate(descriptor, cpuData, 2,
				&publications[1]) == RENDER_RESULT_OK &&
			lateOwners[1]->PublishCandidate(&publications[1], 0) ==
				RENDER_RESULT_OK &&
			lateOwners[0]->PublicationGeneration() == 1 &&
			lateOwners[1]->PublicationGeneration() == 1 &&
			UnbindNativeW3DTextureResources(resources) == RENDER_RESULT_OK &&
			resources->Shutdown() == RENDER_RESULT_FAILED,
			"published external tickets block shutdown without losing generation");
		delete resources;
	}
	result |= Check(ownerDevice.LiveCount() == 2 &&
		ownerHost.BoundResourceTables() == 1 &&
		ownerHost.PendingCleanup() == 1,
		"registry scope exit leaves the table fallback and both owner tickets valid");
	std::atomic<unsigned int> lateReady(0);
	std::atomic<bool> releaseLateOwners(false);
	std::thread lateForeignOwnerDestructors[2] = {
		std::thread([&]() {
			lateReady.fetch_add(1, std::memory_order_release);
			while (!releaseLateOwners.load(std::memory_order_acquire))
			{
				std::this_thread::yield();
			}
			delete lateOwners[0];
		}),
		std::thread([&]() {
			lateReady.fetch_add(1, std::memory_order_release);
			while (!releaseLateOwners.load(std::memory_order_acquire))
			{
				std::this_thread::yield();
			}
			delete lateOwners[1];
		})
	};
	while (lateReady.load(std::memory_order_acquire) != 2)
	{
		std::this_thread::yield();
	}
	releaseLateOwners.store(true, std::memory_order_release);
	unsigned int ownerTableDrained = 0;
	result |= Check(ownerHost.DrainCleanup(0, &ownerTableDrained) ==
		RENDER_RESULT_OK,
		"table fallback may drain while both foreign owner destructors release");
	lateForeignOwnerDestructors[0].join();
	lateForeignOwnerDestructors[1].join();
	while (ownerHost.PendingCleanup() != 0)
	{
		unsigned int additionallyDrained = 0;
		const RenderResult drainResult = ownerHost.DrainCleanup(0,
			&additionallyDrained);
		result |= Check(drainResult == RENDER_RESULT_OK &&
			additionallyDrained != 0,
			"owner drains cleanup accepted concurrently with table retirement");
		if (drainResult != RENDER_RESULT_OK || additionallyDrained == 0)
		{
			break;
		}
		ownerTableDrained += additionallyDrained;
	}
	result |= Check(ownerTableDrained >= 1 && ownerTableDrained <= 3 &&
		ownerHost.PendingCleanup() == 0 &&
		ownerHost.BoundResourceTables() == 0 &&
		ownerDevice.DestroyCount() == 2 && ownerDevice.LiveCount() == 0,
		"table retirement and concurrent ticket release destroy each exact resource once");
	result |= Check(ownerHost.PendingCleanup() == 0 &&
		ownerHost.BoundResourceTables() == 0 && ownerDevice.DestroyCount() == 2 &&
		ownerDevice.LiveCount() == 0 &&
		ownerHost.Detach() == RENDER_RESULT_OK,
		"concurrent late teardown releases both tickets without a dangling facade");
	ownerDevice.shutdown();

	return result;
}

int TestQueuedCleanupRecoveryFailure()
{
	using namespace rts::render;
	int result = 0;
	unsigned char topBytes[64] = { 0 };
	unsigned char lowerBytes[16] = { 0 };
	TextureSubresourceData cpuData[2];
	MakeCpuData(topBytes, lowerBytes, cpuData);
	const TextureDescriptor descriptor = MakeCpuDescriptor();

	NativeW3DRenderer renderer;
	FakeRenderDevice *device = new FakeRenderDevice();
	const RenderResult attachResult =
		NativeW3DRecoveryTestAccess::AttachOwnedBackend(&renderer, device);
	result |= Check(attachResult == RENDER_RESULT_OK,
		"failed-recovery fixture attaches an owned backend");
	if (attachResult != RENDER_RESULT_OK)
	{
		delete device;
		return result;
	}
	NativeW3DRenderState *retainedState =
		NativeW3DRecoveryTestAccess::RetainState(&renderer);
	NativeW3DResources resources(8);
	NativeW3DTextureOwner *queuedOwner = new NativeW3DTextureOwner();
	NativeW3DTextureCandidate publication;
	result |= Check(retainedState != 0 &&
		resources.Bind(&renderer) == RENDER_RESULT_OK &&
		BindNativeW3DTextureResources(&resources) == RENDER_RESULT_OK &&
		queuedOwner->CreateCandidate(descriptor, cpuData, 2, &publication) ==
			RENDER_RESULT_OK &&
		queuedOwner->PublishCandidate(&publication, 0) == RENDER_RESULT_OK &&
		retainedState->BoundResourceTables() == 1,
		"prequeued recovery fixture publishes one externally owned texture");
	result |= Check(UnbindNativeW3DTextureResources(&resources) ==
		RENDER_RESULT_OK,
		"prequeued recovery fixture unpublishes the product registry");
	std::thread queuedForeignDestructor([&]() { delete queuedOwner; });
	queuedForeignDestructor.join();
	result |= Check(renderer.PendingCleanup() == 1,
		"foreign texture destruction is queued before recovery fails");

	device->FailRecovery(true);
	result |= Check(renderer.RecoverDevice() == RENDER_RESULT_DEVICE_REMOVED &&
		!renderer.IsInitialized() && retainedState->BoundResourceTables() == 1 &&
		!retainedState->IsAcceptingCleanup() &&
		retainedState->PendingCleanup() == 0,
		"failed recovery drains preaccepted texture cleanup after terminal detach");
	result |= Check(resources.Shutdown() == RENDER_RESULT_OK &&
		retainedState->BoundResourceTables() == 0,
		"terminal table shutdown releases state after prequeued ticket cleanup");
	retainedState->Release();

	NativeW3DRenderer tableRenderer;
	FakeRenderDevice *tableDevice = new FakeRenderDevice();
	const RenderResult tableAttach =
		NativeW3DRecoveryTestAccess::AttachOwnedBackend(&tableRenderer,
			tableDevice);
	result |= Check(tableAttach == RENDER_RESULT_OK,
		"prequeued table fixture attaches an owned backend");
	if (tableAttach != RENDER_RESULT_OK)
	{
		delete tableDevice;
		return result;
	}
	NativeW3DRenderState *retainedTableState =
		NativeW3DRecoveryTestAccess::RetainState(&tableRenderer);
	NativeW3DResources *queuedTable = new NativeW3DResources(8);
	GpuHandle queuedTableTexture;
	result |= Check(retainedTableState != 0 &&
		queuedTable->Bind(&tableRenderer) == RENDER_RESULT_OK &&
		queuedTable->CreateTexture(descriptor, cpuData, 2,
			&queuedTableTexture) == RENDER_RESULT_OK,
		"prequeued table fixture creates one backend allocation");
	std::thread queuedTableDestructor([&]() { delete queuedTable; });
	queuedTableDestructor.join();
	result |= Check(tableRenderer.PendingCleanup() == 1 &&
		retainedTableState->BoundResourceTables() == 1,
		"foreign table destruction is queued before recovery fails");
	tableDevice->FailRecovery(true);
	result |= Check(tableRenderer.RecoverDevice() == RENDER_RESULT_DEVICE_REMOVED &&
		!tableRenderer.IsInitialized() &&
		retainedTableState->PendingCleanup() == 0 &&
		retainedTableState->BoundResourceTables() == 0,
		"failed recovery logically completes a preaccepted terminal table callback");
	retainedTableState->Release();

	NativeW3DRenderer combinedRenderer;
	FakeRenderDevice *combinedDevice = new FakeRenderDevice();
	const RenderResult combinedAttach =
		NativeW3DRecoveryTestAccess::AttachOwnedBackend(&combinedRenderer,
			combinedDevice);
	result |= Check(combinedAttach == RENDER_RESULT_OK,
		"combined terminal queue fixture attaches an owned backend");
	if (combinedAttach != RENDER_RESULT_OK)
	{
		delete combinedDevice;
		return result;
	}
	NativeW3DRenderState *retainedCombinedState =
		NativeW3DRecoveryTestAccess::RetainState(&combinedRenderer);
	NativeW3DResources *combinedResources = new NativeW3DResources(8);
	NativeW3DTextureOwner *combinedOwner = new NativeW3DTextureOwner();
	NativeW3DTextureCandidate combinedPublication;
	result |= Check(retainedCombinedState != 0 &&
		combinedResources->Bind(&combinedRenderer) == RENDER_RESULT_OK &&
		BindNativeW3DTextureResources(combinedResources) == RENDER_RESULT_OK &&
		combinedOwner->CreateCandidate(descriptor, cpuData, 2,
			&combinedPublication) == RENDER_RESULT_OK &&
		combinedOwner->PublishCandidate(&combinedPublication, 0) ==
			RENDER_RESULT_OK &&
		UnbindNativeW3DTextureResources(combinedResources) == RENDER_RESULT_OK,
		"combined terminal queue fixture publishes one externally owned texture");
	std::thread combinedTableDestructor([&]() { delete combinedResources; });
	combinedTableDestructor.join();
	std::thread combinedTicketDestructor([&]() { delete combinedOwner; });
	combinedTicketDestructor.join();
	result |= Check(retainedCombinedState->PendingCleanup() == 2 &&
		retainedCombinedState->BoundResourceTables() == 1,
		"table-first terminal fixture queues its exact ticket behind the table");
	combinedDevice->FailRecovery(true);
	result |= Check(combinedRenderer.RecoverDevice() ==
			RENDER_RESULT_DEVICE_REMOVED && !combinedRenderer.IsInitialized() &&
		retainedCombinedState->PendingCleanup() == 0 &&
		retainedCombinedState->BoundResourceTables() == 0,
		"failed recovery drains table-first teardown then releases its final ticket");
	retainedCombinedState->Release();
	return result;
}

int TestFailedRecoveryTicketLifetime()
{
	using namespace rts::render;
	int result = 0;
	unsigned char topBytes[64] = { 0 };
	unsigned char lowerBytes[16] = { 0 };
	TextureSubresourceData cpuData[2];
	MakeCpuData(topBytes, lowerBytes, cpuData);
	const TextureDescriptor descriptor = MakeCpuDescriptor();

	NativeW3DRenderer renderer;
	FakeRenderDevice *device = new FakeRenderDevice();
	const RenderResult attachResult =
		NativeW3DRecoveryTestAccess::AttachOwnedBackend(&renderer, device);
	result |= Check(attachResult == RENDER_RESULT_OK,
		"failed-recovery fixture attaches an owned backend");
	if (attachResult != RENDER_RESULT_OK)
	{
		delete device;
		return result;
	}
	NativeW3DRenderState *retainedState =
		NativeW3DRecoveryTestAccess::RetainState(&renderer);
	NativeW3DResources resources(8);
	NativeW3DTextureOwner *lateOwner = new NativeW3DTextureOwner();
	NativeW3DTextureCandidate publication;
	NativeW3DTextureHandle staleTexture;
	NativeW3DSurfaceHandle staleSurface;
	result |= Check(retainedState != 0 &&
		resources.Bind(&renderer) == RENDER_RESULT_OK &&
		BindNativeW3DTextureResources(&resources) == RENDER_RESULT_OK &&
		lateOwner->CreateCandidate(descriptor, cpuData, 2, &publication) ==
			RENDER_RESULT_OK &&
		lateOwner->PublishCandidate(&publication, 0) == RENDER_RESULT_OK &&
		lateOwner->AcquireForSampling(&staleTexture) == RENDER_RESULT_OK &&
		lateOwner->AcquireSurface(0, 0, &staleSurface) == RENDER_RESULT_OK &&
		retainedState->BoundResourceTables() == 1,
		"an external owner preserves typed texture and surface tokens across failure");

	device->FailRecovery(true);
	result |= Check(renderer.RecoverDevice() == RENDER_RESULT_DEVICE_REMOVED &&
		!renderer.IsInitialized() && retainedState->BoundResourceTables() == 1 &&
		!retainedState->IsAcceptingCleanup(),
		"failed recovery closes and detaches the backend with the table still referenced");
	result |= Check(UnbindNativeW3DTextureResources(&resources) ==
			RENDER_RESULT_OK &&
		resources.Shutdown() == RENDER_RESULT_OK &&
		retainedState->BoundResourceTables() == 0,
		"terminal table shutdown clears backend-dead slots despite a live logical ticket");
	retainedState->Release();

	NativeW3DRenderer replacementRenderer;
	FakeRenderDevice *replacementDevice = new FakeRenderDevice();
	const RenderResult replacementAttach =
		NativeW3DRecoveryTestAccess::AttachOwnedBackend(&replacementRenderer,
			replacementDevice);
	result |= Check(replacementAttach == RENDER_RESULT_OK,
		"cross-host ABA fixture attaches a fresh backend with reset native generations");
	if (replacementAttach != RENDER_RESULT_OK)
	{
		delete replacementDevice;
		std::thread failedAttachDestructor([&]() { delete lateOwner; });
		failedAttachDestructor.join();
		return result;
	}
	result |= Check(resources.Bind(&replacementRenderer) == RENDER_RESULT_FAILED,
		"table rebind rejects every surviving ticket from its terminal attachment");
	std::thread lateForeignDestructor([&]() { delete lateOwner; });
	lateForeignDestructor.join();
	result |= Check(resources.Bind(&replacementRenderer) == RENDER_RESULT_OK &&
		BindNativeW3DTextureResources(&resources) == RENDER_RESULT_OK,
		"table rebind succeeds only after the last old logical ticket is gone");

	NativeW3DTextureOwner reboundOwner;
	NativeW3DTextureCandidate reboundPublication;
	NativeW3DTextureHandle reboundTexture;
	NativeW3DSurfaceHandle reboundSurface;
	result |= Check(reboundOwner.CreateCandidate(descriptor, cpuData, 2,
			&reboundPublication) == RENDER_RESULT_OK &&
		reboundOwner.PublishCandidate(&reboundPublication, 0) == RENDER_RESULT_OK &&
		reboundOwner.AcquireForSampling(&reboundTexture) == RENDER_RESULT_OK &&
		reboundOwner.AcquireSurface(0, 0, &reboundSurface) == RENDER_RESULT_OK &&
		reboundTexture.resource == staleTexture.resource &&
		reboundTexture.attachmentGeneration != staleTexture.attachmentGeneration &&
		!resources.IsValid(staleTexture) && !resources.IsValid(staleSurface) &&
		resources.IsValid(reboundTexture) && resources.IsValid(reboundSurface) &&
		replacementDevice->LiveCount() == 1,
		"fresh-host same-slot reuse rejects stale typed texture and surface tokens");
	result |= Check(reboundOwner.Reset() == RENDER_RESULT_OK &&
		UnbindNativeW3DTextureResources(&resources) == RENDER_RESULT_OK &&
		resources.Shutdown() == RENDER_RESULT_OK &&
		replacementDevice->LiveCount() == 0 &&
		replacementRenderer.Shutdown() == RENDER_RESULT_OK,
		"cross-host ABA fixture releases only the fresh exact resource");
	return result;
}

int TestForeignFacadeTerminalRecoveryCleanup()
{
	using namespace rts::render;
	int result = 0;
	unsigned char topBytes[64] = { 0 };
	unsigned char lowerBytes[16] = { 0 };
	TextureSubresourceData cpuData[2];
	MakeCpuData(topBytes, lowerBytes, cpuData);
	const TextureDescriptor descriptor = MakeCpuDescriptor();

	NativeW3DRenderer renderer;
	FakeRenderDevice *device = new FakeRenderDevice();
	const RenderResult attachResult =
		NativeW3DRecoveryTestAccess::AttachOwnedBackend(&renderer, device);
	result |= Check(attachResult == RENDER_RESULT_OK,
		"foreign terminal-facade fixture attaches an owned backend");
	if (attachResult != RENDER_RESULT_OK)
	{
		delete device;
		return result;
	}
	NativeW3DRenderState *retainedState =
		NativeW3DRecoveryTestAccess::RetainState(&renderer);
	NativeW3DResources *resources = new NativeW3DResources(8);
	NativeW3DTextureOwner *owner = new NativeW3DTextureOwner();
	NativeW3DTextureCandidate publication;
	result |= Check(retainedState != 0 &&
		resources->Bind(&renderer) == RENDER_RESULT_OK &&
		BindNativeW3DTextureResources(resources) == RENDER_RESULT_OK &&
		owner->CreateCandidate(descriptor, cpuData, 2, &publication) ==
			RENDER_RESULT_OK &&
		owner->PublishCandidate(&publication, 0) == RENDER_RESULT_OK &&
		UnbindNativeW3DTextureResources(resources) == RENDER_RESULT_OK &&
		retainedState->BoundResourceTables() == 1 &&
		retainedState->PendingCleanup() == 0,
		"foreign terminal-facade fixture retains one table and one exact ticket");

	device->FailRecovery(true);
	result |= Check(renderer.RecoverDevice() == RENDER_RESULT_DEVICE_REMOVED &&
		!renderer.IsInitialized() && !retainedState->IsAcceptingCleanup() &&
		retainedState->BoundResourceTables() == 1 &&
		retainedState->PendingCleanup() == 0,
		"failed recovery closes and detaches before foreign facade destruction");

	std::thread foreignTableDestructor([&]() { delete resources; });
	foreignTableDestructor.join();
	result |= Check(retainedState->BoundResourceTables() == 0 &&
		retainedState->PendingCleanup() == 0,
		"foreign terminal facade destruction unregisters the backend-dead table");

	std::thread foreignTicketDestructor([&]() { delete owner; });
	foreignTicketDestructor.join();
	result |= Check(retainedState->BoundResourceTables() == 0 &&
		retainedState->PendingCleanup() == 0,
		"the final detached ticket releases the retained table implementation");
	retainedState->Release();
	return result;
}

int main()
{
	return TestNeutralTextureOwnership() |
		TestNativeD3D11RetirementFault() |
		TestLateRegistryTicketLifetime() |
		TestQueuedCleanupRecoveryFailure() |
		TestFailedRecoveryTicketLifetime() |
		TestForeignFacadeTerminalRecoveryCleanup();
}
