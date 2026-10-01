#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "Renderer/NativeW3DResources.h"
#include "Renderer/ThreadedRenderDevice.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <vector>

namespace
{
using namespace rts::render;
void Require(bool value, const char *message)
{
	if (!value) throw std::runtime_error(message);
}
IRenderDevice *Factory(void *) { return CreateD3D11RenderDevice(); }
struct Window
{
	HWND value = CreateWindowExW(0, L"STATIC", L"", WS_OVERLAPPED,
		0, 0, 64, 64, 0, 0, GetModuleHandleW(0), 0);
	Window() { Require(value != 0, "hidden window"); }
	~Window() { DestroyWindow(value); }
};
struct Fixture
{
	Window window;
	std::unique_ptr<IRenderDevice> device;
	NativeW3DResourceHost host;
	NativeW3DResources resources;
	IRenderContext *context = 0;
	unsigned int ownerMode;
	uint64_t sequence = 0, completedSequence = 0;
	bool threaded, open = false;
	Fixture(unsigned int mode, unsigned int samples) : device(mode ?
		CreateThreadedRenderDevice(Factory, 0, Options(mode)) : CreateD3D11RenderDevice()),
		host(8), resources(8), ownerMode(mode), threaded(mode != 0)
	{
		Require(device.get() != 0, "device allocation");
		RenderDeviceParameters parameters;
		parameters.backend = RENDER_BACKEND_D3D11;
		parameters.window = window.value;
		parameters.width = parameters.height = 64;
		parameters.multisampleCount = samples;
		parameters.enableDebugLayer = true;
		parameters.enableVsync = false;
		Require(device->initialize(parameters) == RENDER_RESULT_OK &&
			host.Attach(device.get(), device->immediateContext()) == RENDER_RESULT_OK &&
			resources.BindHost(&host) == RENDER_RESULT_OK, "actual backend/resource host");
		context = device->immediateContext();
	}
	static ThreadedRenderOptions Options(unsigned int mode)
	{
		ThreadedRenderOptions result;
		result.serial = mode == 1;
		return result;
	}
	void Begin()
	{
		Require(context->beginFrame() == RENDER_RESULT_OK, "begin frame");
		open = true;
		if (threaded)
		{
			sequence = CurrentThreadedRenderFrameSequence(device.get());
			Require(sequence == completedSequence + 1, "accepted frame sequence advances exactly once");
		}
	}
	void Finish(const char *phase, RenderResult expected = RENDER_RESULT_OK)
	{
		const bool failed = expected != RENDER_RESULT_OK;
		const RenderResult ended = context->endFrame();
		open = false;
		std::printf("COPY_RECOVERY_END mode=%u phase=%s sequence=%llu failed=%u end=%d\n",
			ownerMode, phase, static_cast<unsigned long long>(sequence), failed, ended);
		// The injected copy failure is a returned result, not physical removal.
		// Producer teardown itself remains successful in every fixture phase.
		Require(ended == RENDER_RESULT_OK, "end frame result");
		if (threaded)
		{
			const RenderResult submitted = SubmitThreadedRenderFrame(device.get(), false);
			const RenderResult drained = DrainThreadedRenderDevice(device.get());
			std::printf("COPY_RECOVERY_FENCE mode=%u phase=%s sequence=%llu end=%d submit=%d drain=%d\n",
				ownerMode, phase, static_cast<unsigned long long>(sequence), ended, submitted, drained);
			Require(LastThreadedRenderFrameSequence(device.get()) == sequence &&
				CurrentThreadedRenderFrameSequence(device.get()) == 0,
				"accepted frame is sealed with its exact sequence");
			ThreadedRenderFrameCompletion completion, duplicate;
			Require(PollThreadedRenderCompletion(device.get(), &completion),
				"each accepted frame produces a completion after its fence");
			std::printf("COPY_RECOVERY_COMPLETION mode=%u phase=%s expected_sequence=%llu sequence=%llu "
				"result=%d outcome=%d ended=%u submitted=%u presented=%u operational=%u resource_failure=%u\n",
				ownerMode, phase, static_cast<unsigned long long>(sequence),
				static_cast<unsigned long long>(completion.sequence), completion.result,
				completion.outcome.result(), completion.outcome.frameEnded(), completion.outcome.wasSubmitted(),
				completion.presented, completion.operational, completion.resourceFailure);
			Require(completion.sequence == sequence && completion.sequence == completedSequence + 1 &&
				completion.outcome.frameEnded() && completion.outcome.wasSubmitted() &&
				!completion.presented && !completion.outcome.wasPresented(),
				"one exact ordered hidden-frame completion acknowledges teardown and submission");
			completedSequence = completion.sequence;
			Require(!PollThreadedRenderCompletion(device.get(), &duplicate),
				"no duplicate or unexplained frame completion");
			Require(submitted == (ownerMode == 1 ? expected : RENDER_RESULT_OK), "hidden frame submit");
			Require(drained == expected, "actual frame fence result");
			const bool operational = expected != RENDER_RESULT_DEVICE_REMOVED;
			Require(completion.result == expected && completion.outcome.result() == expected &&
				completion.operational == operational && completion.outcome.isOperational() == operational &&
				completion.resourceFailure == failed,
				"completion preserves exact backend failure or healthy execution");
			Require(resources.PublishThreadedCompletion(completion.sequence, completion.resourceFailure) ==
				RENDER_RESULT_OK, "publish the exact owner completion into registry authority");
		}
	}
	~Fixture()
	{
		if (open)
		{
			context->endFrame();
			if (threaded) SubmitThreadedRenderFrame(device.get(), false);
			if (threaded) DrainThreadedRenderDevice(device.get());
		}
		resources.Shutdown();
		host.Detach();
		device->shutdown();
	}
};
struct ColorVertex { float x, y, z; unsigned int color; };
struct TextureVertex { float x, y, z, u, v; };

void TestRecovery(unsigned int mode, unsigned int kind)
{
	const unsigned int samples = kind == 2 ? 4 : 1;
	const unsigned int extent = kind == 1 ? 96 : 64;
	Fixture fixture(mode, samples);
	IRenderDevice *device = fixture.device.get();
	NativeW3DResources &resources = fixture.resources;
	IRenderContext *context = fixture.context;
	RenderBackBufferInfo info;
	Require(device->getBackBufferInfo(&info) == RENDER_RESULT_OK &&
		info.multisampleCount == samples, "actual source samples");
	TextureDescriptor descriptor;
	descriptor.width = descriptor.height = extent;
	descriptor.format = info.format;
	descriptor.binding = RENDER_TEXTURE_SHADER_RESOURCE | RENDER_TEXTURE_RENDER_TARGET;
	descriptor.usage = RENDER_USAGE_DEFAULT;
	std::vector<unsigned int> blue(extent * extent, 0xff0000ffU);
	TextureSubresourceData initial;
	initial.data = blue.data();
	initial.rowPitch = extent * 4;
	initial.slicePitch = blue.size() * 4;
	NativeW3DTextureHandle destination;
	Require(resources.CreateTexture(descriptor, &initial, 1, &destination) == RENDER_RESULT_OK,
		"mutable output destination with old blue CPU recovery image");
	TextureDescriptor independentDescriptor = descriptor;
	independentDescriptor.width = independentDescriptor.height = 32;
	independentDescriptor.binding = RENDER_TEXTURE_SHADER_RESOURCE;
	std::vector<unsigned int> independentPixels(32 * 32, 0xff0000ffU);
	TextureSubresourceData independentData;
	independentData.data = independentPixels.data();
	independentData.rowPitch = 32 * 4;
	independentData.slicePitch = independentPixels.size() * 4;
	NativeW3DTextureHandle independent;
	Require(resources.CreateTexture(independentDescriptor, &independentData, 1, &independent) ==
		RENDER_RESULT_OK, "independent CPU recovery image");
	RenderResourceStatistics before;
	Require(device->getDebugResourceStatistics(&before) == RENDER_RESULT_OK &&
		before.recoveryShadowBytes == initial.slicePitch + independentData.slicePitch,
		"both CPU recovery images really retained");
	if (mode == 0)
	{
		// Only the direct backend can reject this before command admission; the
		// public/typed owner preflight is covered separately by its existing fixture.
		fixture.Begin();
		Require(device->copyActiveColorTargetToTexture(independent.resource) ==
			RENDER_RESULT_UNSUPPORTED, "backend rejects too-small destination before GPU issue");
		fixture.Finish("preflight-reject");
		RenderResourceStatistics afterReject;
		NativeW3DTextureDescription rejectedDescription;
		Require(device->getDebugResourceStatistics(&afterReject) == RENDER_RESULT_OK &&
			afterReject.recoveryShadowBytes == before.recoveryShadowBytes &&
			resources.DescribeTexture(destination.resource, &rejectedDescription) == RENDER_RESULT_OK &&
			rejectedDescription.authority == NATIVE_W3D_CONTENT_CPU,
			"preflight rejection leaves destination CPU shadow and logical authority unchanged");
	}
	Require(device->configureResourceFaultInjection(RENDER_RESOURCE_FAULT_TEXTURE_COPY_AFTER_ISSUE,
		1, RENDER_RESULT_DEVICE_REMOVED) == RENDER_RESULT_OK, "post-issue removal injection");
	fixture.Begin();
	Require(context->setRenderTargets(GpuHandle(), GpuHandle()) == RENDER_RESULT_OK &&
		context->clear(RenderFloat4(0, 1, 0, 1), 1, 0) == RENDER_RESULT_OK,
		"green actual source before copy/region/resolve");
	NativeW3DGpuContentLease lease;
	const RenderResult copied = resources.CopyActiveColorTargetToTexture(destination.resource, &lease);
	std::printf("COPY_RECOVERY_ISSUE mode=%u kind=%u copy=%d lease=%u\n",
		mode, kind, copied, lease.isValid());
	Require(copied == RENDER_RESULT_DEVICE_REMOVED && !lease.isValid(),
		"issued copy failure never publishes content lease");
	fixture.Finish("issued-copy-failure", RENDER_RESULT_DEVICE_REMOVED);
	const RenderResult recoveredDevice = device->recoverDevice();
	const RenderResult replacedContext = recoveredDevice == RENDER_RESULT_OK ?
		fixture.host.ReplaceContext(device->immediateContext()) : RENDER_RESULT_FAILED;
	std::printf("COPY_RECOVERY_REBUILD mode=%u kind=%u recover=%d replace=%d operational=%u\n",
		mode, kind, recoveredDevice, replacedContext, device->isOperational());
	Require(recoveredDevice == RENDER_RESULT_OK && replacedContext == RENDER_RESULT_OK,
		"recover real native texture storage");
	fixture.context = context = device->immediateContext();
	RenderResourceStatistics recovered;
	Require(device->getDebugResourceStatistics(&recovered) == RENDER_RESULT_OK,
		"recovered backend statistics");
	const bool shadowDiscarded = recovered.recoveryShadowBytes == independentData.slicePitch;
	std::printf("COPY_RECOVERY_SHADOW mode=%u kind=%u before=%zu after=%zu independent=%zu\n",
		mode, kind, before.recoveryShadowBytes, recovered.recoveryShadowBytes, independentData.slicePitch);
	NativeW3DTextureDescription description;
	Require(resources.DescribeTexture(destination.resource, &description) == RENDER_RESULT_OK &&
		description.authority == NATIVE_W3D_CONTENT_INVALID &&
		resources.AcquireGpuContentLease(destination.resource, &lease) ==
			RENDER_RESULT_INVALID_ARGUMENT && !lease.isValid(),
		"recovered copied texture stays unleaseable before output publication");
	NativeW3DSurfaceHandle surface;
	Require(resources.AcquireTextureSurface(destination, 0, 0, &surface) == RENDER_RESULT_OK,
		"output surface legitimately reacquires without old content authority");

	// A real partial write publishes the output without clearing it again. The
	// untouched area must come from deterministic recovery clear, never old blue.
	const ColorVertex triangle[] = {
		{ -1, 1, 0.1f, 0xffff0000U }, { 0, 1, 0.1f, 0xffff0000U },
		{ -1, 0, 0.1f, 0xffff0000U }
	};
	BufferDescriptor vertexDescriptor;
	vertexDescriptor.byteCount = sizeof(triangle);
	vertexDescriptor.stride = sizeof(ColorVertex);
	vertexDescriptor.usage = RENDER_USAGE_IMMUTABLE;
	GpuHandle triangleBuffer;
	Require(resources.CreateBuffer(vertexDescriptor, triangle, sizeof(triangle), &triangleBuffer) ==
		RENDER_RESULT_OK, "post-recovery partial output geometry");
	RenderTargetBinding target;
	target.useBackBufferColor = target.useBackBufferDepth = false;
	target.hasColor = true;
	target.color.resource = destination.resource;
	LegacyLogicalState colorState;
	colorState.pipeline.rasterizer.cullMode = RENDER_CULL_NONE;
	colorState.pipeline.depthStencil.depthEnable = false;
	fixture.Begin();
	Require(context->setRenderTargets(target) == RENDER_RESULT_OK &&
		context->setViewport(0, 0, static_cast<float>(extent), static_cast<float>(extent), 0, 1) ==
			RENDER_RESULT_OK &&
		context->setLegacyState(colorState, RENDER_VERTEX_POSITION3_COLOR, 0) == RENDER_RESULT_OK &&
		context->setVertexBuffer(triangleBuffer, sizeof(ColorVertex), 0) == RENDER_RESULT_OK &&
		context->setPrimitiveTopology(RENDER_PRIMITIVE_TRIANGLE_LIST) == RENDER_RESULT_OK &&
		context->draw(3, 0) == RENDER_RESULT_OK, "actual partial draw leaves remainder untouched");
	fixture.Finish("partial-output");
	Require(resources.PublishRenderTargetWrite(surface, &lease) == RENDER_RESULT_OK &&
		lease.isValid(), "real partial output now publishes new content authority");

	const TextureVertex quad[] = {
		{ -1, 1, 0, 0, 0 }, { 1, 1, 0, 1, 0 }, { -1, -1, 0, 0, 1 },
		{ -1, -1, 0, 0, 1 }, { 1, 1, 0, 1, 0 }, { 1, -1, 0, 1, 1 }
	};
	vertexDescriptor.byteCount = sizeof(quad);
	vertexDescriptor.stride = sizeof(TextureVertex);
	GpuHandle quadBuffer;
	Require(resources.CreateBuffer(vertexDescriptor, quad, sizeof(quad), &quadBuffer) ==
		RENDER_RESULT_OK, "sample recovered output geometry");
	LegacyVertexLayout layout;
	layout.stride = sizeof(TextureVertex);
	layout.elementCount = 2;
	layout.elements[0].semantic = RENDER_VERTEX_SEMANTIC_POSITION;
	layout.elements[0].format = RENDER_VERTEX_DATA_FLOAT3;
	layout.elements[0].byteOffset = 0;
	layout.elements[1].semantic = RENDER_VERTEX_SEMANTIC_TEXTURE_COORDINATE;
	layout.elements[1].format = RENDER_VERTEX_DATA_FLOAT2;
	layout.elements[1].byteOffset = 12;
	LegacyLogicalState textureState = colorState;
	textureState.pipeline.textureStages[0].colorOperation = RENDER_TEXTURE_OP_SELECT_ARGUMENT_1;
	textureState.pipeline.textureStages[0].colorArgument1 = RENDER_TEXTURE_ARG_TEXTURE;
	fixture.Begin();
	Require(context->setRenderTargets(GpuHandle(), GpuHandle()) == RENDER_RESULT_OK &&
		context->clear(RenderFloat4(0, 1, 0, 1), 1, 0) == RENDER_RESULT_OK &&
		context->setViewport(0, 0, 64, 64, 0, 1) == RENDER_RESULT_OK &&
		context->setLegacyStateForLayout(textureState, layout, 1) == RENDER_RESULT_OK &&
		context->setVertexBuffer(quadBuffer, sizeof(TextureVertex), 0) == RENDER_RESULT_OK &&
		context->setTexture(0, destination.resource) == RENDER_RESULT_OK &&
		context->setPrimitiveTopology(RENDER_PRIMITIVE_TRIANGLE_LIST) == RENDER_RESULT_OK &&
		context->draw(6, 0) == RENDER_RESULT_OK, "sample output after real publication");
	// A partial draw does not establish the threaded owner's whole-texture
	// validity after recovery. Registry publication is not owner publication.
	fixture.Finish("sample-recovered-output", mode ? RENDER_RESULT_FAILED : RENDER_RESULT_OK);
	std::vector<unsigned char> pixels(64 * 64 * 4);
	RenderFormat captured;
	if (mode == 0)
	{
		Require(device->captureBackBuffer(pixels.data(), pixels.size(), 64 * 4, &captured) ==
			RENDER_RESULT_OK, "actual recovery pixel readback");
		const unsigned char *written = &pixels[(4 * 64 + 4) * 4];
		const unsigned char *remainder = &pixels[(56 * 64 + 56) * 4];
		const bool clearedRemainder = remainder[0] < 8 && remainder[1] < 8 && remainder[2] < 8;
		std::printf("COPY_RECOVERY mode=%u kind=%u samples=%u old_shadow=%zu recovered_shadow=%zu "
			"authority=%d remainder=%u,%u,%u written=%u,%u,%u\n", mode, kind, samples,
			before.recoveryShadowBytes, recovered.recoveryShadowBytes, description.authority,
			remainder[0], remainder[1], remainder[2], written[0], written[1], written[2]);
		Require(written[0] < 8 && written[1] < 8 && written[2] > 240,
			"partial post-recovery output really wrote red pixels");
		Require(shadowDiscarded && clearedRemainder,
			"issued failed copy cannot recover old shadow or expose an uncleared remainder");
	}
	else
	{
		Require(lease.isValid(), "partial-output lease existed before failed owner sampling");
		const RenderResult stale = resources.AcquireGpuContentLease(destination.resource, &lease);
		std::printf("COPY_RECOVERY_FAIL_CLOSED mode=%u kind=%u stale_lease=%d cleared=%u shadow_discarded=%u\n",
			mode, kind, stale, !lease.isValid(), shadowDiscarded);
		Require(stale == RENDER_RESULT_INVALID_ARGUMENT && !lease.isValid() && shadowDiscarded,
			"failed owner sampling revokes stale partial-output lease and retains no old shadow");
	}

	// Fresh copy can establish normal authority again. Region copies leave only
	// the previously defined destination remainder outside the 64-square source.
	fixture.Begin();
	Require(context->clear(RenderFloat4(0, 1, 0, 1), 1, 0) == RENDER_RESULT_OK &&
		resources.CopyActiveColorTargetToTexture(destination.resource, &lease) == RENDER_RESULT_OK &&
		lease.isValid(), "healthy later copy/resolve publishes fresh authority");
	fixture.Finish("healthy-copy");
	Require(resources.AcquireGpuContentLease(destination.resource, &lease) == RENDER_RESULT_OK,
		"healthy follow-up content remains leaseable");
	fixture.Begin();
	Require(context->setRenderTargets(GpuHandle(), GpuHandle()) == RENDER_RESULT_OK &&
		context->setViewport(0, 0, 64, 64, 0, 1) == RENDER_RESULT_OK &&
		context->setLegacyStateForLayout(textureState, layout, 1) == RENDER_RESULT_OK &&
		context->setVertexBuffer(quadBuffer, sizeof(TextureVertex), 0) == RENDER_RESULT_OK &&
		context->setPrimitiveTopology(RENDER_PRIMITIVE_TRIANGLE_LIST) == RENDER_RESULT_OK &&
		context->setTexture(0, destination.resource) == RENDER_RESULT_OK &&
		context->draw(6, 0) == RENDER_RESULT_OK, "sample healthy follow-up copy");
	fixture.Finish("sample-healthy-copy");
	Require(device->captureBackBuffer(pixels.data(), pixels.size(), 64 * 4, &captured) ==
		RENDER_RESULT_OK, "healthy follow-up readback");
	const unsigned char *healthy = &pixels[(32 * 64 + 32) * 4];
	Require(healthy[0] < 8 && healthy[1] > 240 && healthy[2] < 8,
		"healthy follow-up copy supplies current green pixels");
	if (kind == 1)
	{
		// Screen (56,56) maps to approximately (84,84) in the 96-square
		// destination, outside the 64-square source region and partial triangle.
		const unsigned char *outsideCopy = &pixels[(56 * 64 + 56) * 4];
		std::printf("COPY_RECOVERY_REGION_REMAINDER mode=%u pixels=%u,%u,%u\n", mode,
			outsideCopy[0], outsideCopy[1], outsideCopy[2]);
		Require(outsideCopy[0] < 8 && outsideCopy[1] < 8 && outsideCopy[2] < 8,
			"healthy region copy preserves defined black outside the copied source bounds");
	}
	unsigned int errors = 0;
	Require(device->getDebugValidationErrorCount(&errors) == RENDER_RESULT_OK && errors == 0,
		"copy/recovery uses valid D3D commands");
	std::printf("PASS copy-recovery mode=%u kind=%u samples=%u\n", mode, kind, samples);
}
}

int main(int argc, char **argv)
{
	try
	{
		if (argc == 1)
		{
			for (unsigned int mode = 0; mode < 3; ++mode)
				for (unsigned int kind = 0; kind < 3; ++kind) TestRecovery(mode, kind);
			return 0;
		}
		unsigned int mode = 0;
		if (argc >= 2 && std::strcmp(argv[1], "--serial") == 0) mode = 1;
		else if (argc >= 2 && std::strcmp(argv[1], "--parallel") == 0) mode = 2;
		else Require(std::strcmp(argv[1], "--direct") == 0, "unknown owner selector");
		Require(argc <= 3, "extra selectors");
		if (argc == 3)
		{
			unsigned int kind = 0;
			if (std::strcmp(argv[2], "--region") == 0) kind = 1;
			else if (std::strcmp(argv[2], "--resolve") == 0) kind = 2;
			else Require(std::strcmp(argv[2], "--full") == 0, "unknown copy selector");
			TestRecovery(mode, kind);
		}
		else for (unsigned int kind = 0; kind < 3; ++kind) TestRecovery(mode, kind);
		return 0;
	}
	catch (const std::exception &error)
	{
		std::fprintf(stderr, "FAILED: %s\n", error.what());
		return 1;
	}
}
