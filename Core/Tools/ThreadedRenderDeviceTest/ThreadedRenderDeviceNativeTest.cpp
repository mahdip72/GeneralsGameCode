#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "Renderer/ThreadedRenderDevice.h"
#include "Lib/JobSystem.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <vector>

namespace
{
using namespace rts::render;
void require(bool value, const char *message)
{
	if (!value) throw std::runtime_error(message);
}

struct NativeFactoryState { std::atomic<bool> owner{false}; };
IRenderDevice *nativeFactory(void *context)
{
	NativeFactoryState *state = static_cast<NativeFactoryState *>(context);
	state->owner.store(rts::JobSystem::instance().isCurrentThread(rts::JOB_OWNER_RENDER));
	return CreateD3D11RenderDevice();
}

struct HiddenWindow
{
	HWND value;
	HiddenWindow() : value(CreateWindowExW(0, L"STATIC", L"", WS_OVERLAPPED,
		0, 0, 64, 64, 0, 0, GetModuleHandleW(0), 0))
	{
		require(value != 0, "create hidden native renderer window");
	}
	~HiddenWindow() { DestroyWindow(value); }
};

typedef std::vector<unsigned char> Pixels;
std::vector<Pixels> run(bool threaded, bool serial, unsigned frameSlots)
{
	HiddenWindow window;
	NativeFactoryState factory;
	ThreadedRenderOptions options;
	options.serial = serial;
	options.maxFramesInFlight = frameSlots;
	std::unique_ptr<IRenderDevice> device(threaded ?
		CreateThreadedRenderDevice(nativeFactory, &factory, options) : CreateD3D11RenderDevice());
	require(device.get() != 0, "create native device");
	RenderDeviceParameters parameters;
	parameters.backend = RENDER_BACKEND_D3D11;
	parameters.window = window.value;
	parameters.width = parameters.height = 64;
	parameters.enableDebugLayer = true;
	parameters.enableVsync = false;
	require(device->initialize(parameters) == RENDER_RESULT_OK, "initialize hidden native flip swap chain");
	if (threaded) require(factory.owner.load(), "native backend factory executes on registered render owner");
	IRenderContext *context = device->immediateContext();
	require(context != 0, "native context exists");
	struct Vertex { float x, y, z; unsigned color; };
	Vertex vertices[] = {{-0.8f, -0.8f, 0, 0xff00ff00U},
		{0.8f, -0.8f, 0, 0xff00ff00U}, {0, 0.8f, 0, 0xff00ff00U}};
	BufferDescriptor descriptor;
	descriptor.byteCount = sizeof(vertices);
	descriptor.stride = sizeof(Vertex);
	descriptor.usage = RENDER_USAGE_DYNAMIC;
	GpuHandle buffer;
	require(device->createBuffer(descriptor, vertices, sizeof(vertices), &buffer) == RENDER_RESULT_OK,
		"create real GPU buffer through queued resource preamble");
	LegacyLogicalState logical;
	logical.pipeline.rasterizer.cullMode = RENDER_CULL_NONE;
	std::vector<Pixels> captures;
	uint64_t completed = 0;
	unsigned width = 64, height = 64;
	for (unsigned frame = 0; frame < 18; ++frame)
	{
		if (frame == 6)
		{
			require(device->resize(0, 0) == RENDER_RESULT_OK, "minimize preserves native targets");
			width = 96; height = 48;
			require(device->resize(width, height) == RENDER_RESULT_OK, "resize flushes queued frames safely");
		}
		if (frame == 12)
			require(device->recoverDevice() == RENDER_RESULT_OK, "recreate native device with durable buffer handle");
		RenderBackBufferInfo info;
		require(device->getBackBufferInfo(&info) == RENDER_RESULT_OK && info.width == width && info.height == height,
			"cached native back-buffer dimensions match lifecycle transitions");
		require(context->beginFrame() == RENDER_RESULT_OK, "begin native frame");
		const unsigned color = frame % 2 ? 0xffff0000U : 0xff00ff00U;
		for (Vertex &vertex : vertices) vertex.color = color;
		require(context->updateBuffer(buffer, vertices, sizeof(vertices), 0, RENDER_BUFFER_UPDATE_DISCARD) == RENDER_RESULT_OK,
			"queue per-frame native dynamic update");
		require(context->clear(RenderFloat4(0, 0, 1, 1), 1, 0) == RENDER_RESULT_OK &&
			context->setViewport(0, 0, static_cast<float>(width), static_cast<float>(height), 0, 1) == RENDER_RESULT_OK &&
			context->setLegacyState(logical, RENDER_VERTEX_POSITION3_COLOR, 0) == RENDER_RESULT_OK &&
			context->setVertexBuffer(buffer, sizeof(Vertex), 0) == RENDER_RESULT_OK &&
			context->setPrimitiveTopology(RENDER_PRIMITIVE_TRIANGLE_LIST) == RENDER_RESULT_OK &&
			context->draw(3, 0) == RENDER_RESULT_OK && context->endFrame() == RENDER_RESULT_OK,
			"record real native clear/state/draw/end");
		// Only selected frames fence for readback; the intervening frames retain
		// normal asynchronous admission, so this also exercises FIFO ordering.
		if (frame % 3 == 2)
		{
			Pixels pixels(width * height * 4);
			RenderFormat format = RENDER_FORMAT_UNKNOWN;
			require(device->captureBackBuffer(pixels.data(), pixels.size(), width * 4, &format) == RENDER_RESULT_OK &&
				format == RENDER_FORMAT_B8G8R8A8_UNORM, "capture native pixels before flip");
			const unsigned char *center = pixels.data() + (height / 2 * width + width / 2) * 4;
			require(center[0] < 16 && (frame % 2 ? center[2] > 240 : center[1] > 240),
				"captured triangle has current frame color, not clear or stale buffer bytes");
			captures.push_back(pixels);
		}
		require(device->present() == RENDER_RESULT_OK, "submit native flip");
		if (threaded)
		{
			ThreadedRenderFrameCompletion completion;
			while (PollThreadedRenderCompletion(device.get(), &completion))
			{
				require(completion.sequence == ++completed && completion.result == RENDER_RESULT_OK &&
					completion.presented && completion.operational && !completion.resourceFailure,
					"real backend completion preserves sequence and successful resource/present outcome");
			}
		}
	}
	if (threaded)
	{
		require(DrainThreadedRenderDevice(device.get()) == RENDER_RESULT_OK, "drain real native owner");
		ThreadedRenderFrameCompletion completion;
		while (PollThreadedRenderCompletion(device.get(), &completion))
			require(completion.sequence == ++completed && completion.result == RENDER_RESULT_OK &&
				completion.presented && completion.operational && !completion.resourceFailure,
				"last native completions are retained");
		require(completed == 18, "one completion for every real native frame");
	}
	require(device->destroyResource(buffer), "destroy real native resource");
	unsigned validationErrors = 0;
	const RenderResult debug = device->getDebugValidationErrorCount(&validationErrors);
	require(debug == RENDER_RESULT_UNSUPPORTED || (debug == RENDER_RESULT_OK && validationErrors == 0),
		"native debug layer has no validation errors");
	device->shutdown();
	return captures;
}

std::vector<Pixels> runTexturePipeline(bool threaded, bool serial,
	unsigned frameSlots)
{
	HiddenWindow window;
	NativeFactoryState factory;
	ThreadedRenderOptions options;
	options.serial = serial;
	options.maxFramesInFlight = frameSlots;
	std::unique_ptr<IRenderDevice> device(threaded ?
		CreateThreadedRenderDevice(nativeFactory, &factory, options) :
		CreateD3D11RenderDevice());
	require(device.get() != 0, "create native texture-pipeline device");
	RenderDeviceParameters parameters;
	parameters.backend = RENDER_BACKEND_D3D11;
	parameters.window = window.value;
	parameters.width = parameters.height = 64;
	parameters.enableDebugLayer = true;
	parameters.enableVsync = false;
	require(device->initialize(parameters) == RENDER_RESULT_OK,
		"initialize native texture-pipeline swap chain");
	if (threaded) require(factory.owner.load(),
		"texture-pipeline backend factory executes on the render owner");
	IRenderContext *context = device->immediateContext();
	require(context != 0, "texture-pipeline native context exists");

	struct TexturedVertex
	{
		float position[3];
		float normal[3];
		unsigned int color;
		float texture[2];
	};
	const TexturedVertex vertices[3] = {
		{ { -0.8f, -0.8f, 0.0f }, { 0.0f, 0.0f, -1.0f },
			0xffffffffU, { 0.0f, 1.0f } },
		{ { 0.0f, 0.8f, 0.0f }, { 0.0f, 0.0f, -1.0f },
			0xffffffffU, { 0.5f, 0.0f } },
		{ { 0.8f, -0.8f, 0.0f }, { 0.0f, 0.0f, -1.0f },
			0xffffffffU, { 1.0f, 1.0f } }
	};
	LegacyVertexLayout layout;
	layout.stride = sizeof(TexturedVertex);
	layout.elementCount = 4;
	layout.elements[0].semantic = RENDER_VERTEX_SEMANTIC_POSITION;
	layout.elements[0].semanticIndex = 0;
	layout.elements[0].format = RENDER_VERTEX_DATA_FLOAT3;
	layout.elements[0].byteOffset = 0;
	layout.elements[1].semantic = RENDER_VERTEX_SEMANTIC_NORMAL;
	layout.elements[1].semanticIndex = 0;
	layout.elements[1].format = RENDER_VERTEX_DATA_FLOAT3;
	layout.elements[1].byteOffset = 12;
	layout.elements[2].semantic = RENDER_VERTEX_SEMANTIC_DIFFUSE;
	layout.elements[2].semanticIndex = 0;
	layout.elements[2].format = RENDER_VERTEX_DATA_COLOR_BGRA8;
	layout.elements[2].byteOffset = 24;
	layout.elements[3].semantic = RENDER_VERTEX_SEMANTIC_TEXTURE_COORDINATE;
	layout.elements[3].semanticIndex = 0;
	layout.elements[3].format = RENDER_VERTEX_DATA_FLOAT2;
	layout.elements[3].byteOffset = 28;
	BufferDescriptor vertexDescriptor;
	vertexDescriptor.byteCount = sizeof(vertices);
	vertexDescriptor.stride = sizeof(TexturedVertex);
	vertexDescriptor.usage = RENDER_USAGE_DEFAULT;
	GpuHandle vertexBuffer;
	require(device->createBuffer(vertexDescriptor, vertices, sizeof(vertices),
		&vertexBuffer) == RENDER_RESULT_OK,
		"create native textured vertex buffer");

	const unsigned int green = 0xff00ff00U;
	const unsigned int red = 0xff0000ffU;
	std::vector<unsigned int> sourceMip0(16, green);
	std::vector<unsigned int> sourceMip1(4, green);
	TextureSubresourceData sourceData[2];
	sourceData[0].data = &sourceMip0[0];
	sourceData[0].rowPitch = 4 * sizeof(unsigned int);
	sourceData[0].slicePitch = sourceMip0.size() * sizeof(unsigned int);
	sourceData[1].data = &sourceMip1[0];
	sourceData[1].rowPitch = 2 * sizeof(unsigned int);
	sourceData[1].slicePitch = sourceMip1.size() * sizeof(unsigned int);
	TextureDescriptor sourceDescriptor;
	sourceDescriptor.width = sourceDescriptor.height = 4;
	sourceDescriptor.mipCount = 2;
	sourceDescriptor.arrayCount = 1;
	sourceDescriptor.format = RENDER_FORMAT_R8G8B8A8_UNORM;
	sourceDescriptor.binding = RENDER_TEXTURE_SHADER_RESOURCE;
	sourceDescriptor.usage = RENDER_USAGE_DEFAULT;
	GpuHandle sourceTexture;
	require(device->createTexture(sourceDescriptor, sourceData, 2,
		&sourceTexture) == RENDER_RESULT_OK,
		"create native two-mip source texture");

	TextureDescriptor targetDescriptor;
	targetDescriptor.width = targetDescriptor.height = 64;
	targetDescriptor.mipCount = 1;
	targetDescriptor.arrayCount = 1;
	targetDescriptor.format = RENDER_FORMAT_R8G8B8A8_UNORM;
	targetDescriptor.binding = RENDER_TEXTURE_RENDER_TARGET |
		RENDER_TEXTURE_SHADER_RESOURCE;
	targetDescriptor.usage = RENDER_USAGE_DEFAULT;
	GpuHandle offscreenColor;
	require(device->createTexture(targetDescriptor, 0, 0, &offscreenColor) ==
		RENDER_RESULT_OK, "create native offscreen color target");
	TextureDescriptor depthDescriptor;
	depthDescriptor.width = depthDescriptor.height = 64;
	depthDescriptor.mipCount = 1;
	depthDescriptor.arrayCount = 1;
	depthDescriptor.format = RENDER_FORMAT_D24_UNORM_S8_UINT;
	depthDescriptor.binding = RENDER_TEXTURE_DEPTH_STENCIL;
	depthDescriptor.usage = RENDER_USAGE_DEFAULT;
	GpuHandle offscreenDepth;
	require(device->createTexture(depthDescriptor, 0, 0, &offscreenDepth) ==
		RENDER_RESULT_OK, "create native offscreen depth target");
	GpuHandle copiedColor;
	require(device->createTexture(targetDescriptor, 0, 0, &copiedColor) ==
		RENDER_RESULT_OK, "create native copy/sample target");

	LegacyLogicalState logical;
	logical.pipeline.rasterizer.cullMode = RENDER_CULL_NONE;
	auto setSourceColor = [&](unsigned int color)
	{
		std::fill(sourceMip0.begin(), sourceMip0.end(), color);
		std::fill(sourceMip1.begin(), sourceMip1.end(), color);
	};
	auto drawTextured = [&](GpuHandle texture, unsigned int width,
		unsigned int height)
	{
		require(context->setViewport(0.0f, 0.0f,
			static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f) ==
			RENDER_RESULT_OK, "set native textured viewport");
		require(context->setLegacyStateForLayout(logical, layout, 1) ==
			RENDER_RESULT_OK, "bind native textured legacy layout");
		require(context->setVertexBuffer(vertexBuffer, sizeof(TexturedVertex), 0) ==
			RENDER_RESULT_OK, "bind native textured vertex buffer");
		require(context->setTexture(0, texture) == RENDER_RESULT_OK,
			"bind native sampled texture");
		require(context->setPrimitiveTopology(RENDER_PRIMITIVE_TRIANGLE_LIST) ==
			RENDER_RESULT_OK && context->draw(3, 0) == RENDER_RESULT_OK,
			"draw native textured triangle");
	};
	unsigned int completed = 0;
	auto checkCompletions = [&]()
	{
		if (!threaded) return;
		ThreadedRenderFrameCompletion completion;
		while (PollThreadedRenderCompletion(device.get(), &completion))
		{
			require(completion.sequence == ++completed &&
				completion.result == RENDER_RESULT_OK && completion.presented &&
				completion.operational && !completion.resourceFailure,
				"native texture-pipeline completion has no deferred failure");
		}
	};

	unsigned int width = 64, height = 64;
	unsigned int previousColor = green;
	bool sampleReady = false;
	std::vector<Pixels> captures;
	for (unsigned int frame = 0; frame < 12; ++frame)
	{
		if (frame == 4)
		{
			require(device->resize(0, 0) == RENDER_RESULT_OK,
				"minimize native texture pipeline without destroying targets");
			width = 96; height = 48;
			require(device->resize(width, height) == RENDER_RESULT_OK,
				"resize native texture pipeline with queued frames");
			// Resize invalidates GPU-only copied pixels; reseed the copy before
			// sampling it again.
			sampleReady = false;
			checkCompletions();
		}
		const bool recoveryFrame = frame == 8;
		if (recoveryFrame)
		{
			require(device->recoverDevice() == RENDER_RESULT_OK,
				"recover native texture pipeline with live logical resources");
			require(device->updateBufferResource(vertexBuffer, vertices,
				sizeof(vertices), 0, RENDER_BUFFER_UPDATE_PRESERVE) ==
					RENDER_RESULT_OK,
				"republish descriptor-only vertex bytes after native recovery");
			// Recovery intentionally invalidates GPU-authoritative copies. The
			// source texture must instead come back from its latest refreshed
			// two-mip shadow on this frame.
			sampleReady = false;
			checkCompletions();
		}
		const unsigned int currentColor = recoveryFrame ? previousColor :
			(frame % 2 ? red : green);
		if (!recoveryFrame) setSourceColor(currentColor);
		require(context->beginFrame() == RENDER_RESULT_OK,
			"begin native texture pipeline frame");
		unsigned int visibleColor = currentColor;
		if (sampleReady)
		{
			require(context->setRenderTargets(GpuHandle(), GpuHandle()) ==
				RENDER_RESULT_OK && context->clear(RenderFloat4(0, 0, 1, 1),
					1.0f, 0) == RENDER_RESULT_OK,
				"clear native back buffer before previous-copy sample");
			drawTextured(copiedColor, width, height);
			visibleColor = previousColor;
		}
		require(context->setRenderTargets(offscreenColor, offscreenDepth) ==
			RENDER_RESULT_OK && context->clear(RenderFloat4(0, 0, 0, 1),
				1.0f, 0) == RENDER_RESULT_OK,
			"bind and clear native offscreen color/depth targets");
		if (!recoveryFrame)
			require(device->refreshTexture(sourceTexture, sourceDescriptor,
				sourceData, 2) == RENDER_RESULT_OK,
				"refresh native source texture including both mips");
		drawTextured(sourceTexture, 64, 64);
		require(device->copyActiveColorTargetToTexture(copiedColor) ==
			RENDER_RESULT_OK, "copy native offscreen color for later sampling");
		require(context->setRenderTargets(GpuHandle(), GpuHandle()) ==
			RENDER_RESULT_OK && context->setTexture(0, GpuHandle()) ==
			RENDER_RESULT_OK, "restore native back-buffer target after copy");
		if (!sampleReady)
		{
			require(context->clear(RenderFloat4(0, 0, 1, 1), 1.0f, 0) ==
				RENDER_RESULT_OK, "clear native back buffer before copy seed");
			drawTextured(copiedColor, width, height);
		}
		require(context->endFrame() == RENDER_RESULT_OK,
			"end native texture pipeline frame");
		if (frame % 3 == 2)
		{
			// Flip-model Present may discard the current back-buffer contents;
			// capture the just-rendered frame before submitting that flip.
			Pixels pixels(width * height * 4);
			RenderFormat format = RENDER_FORMAT_UNKNOWN;
			require(device->captureBackBuffer(&pixels[0], pixels.size(), width * 4,
				&format) == RENDER_RESULT_OK &&
				format == RENDER_FORMAT_B8G8R8A8_UNORM,
				"capture native textured back buffer after queued frames");
			const unsigned char *center = &pixels[(height / 2 * width + width / 2) * 4];
			const unsigned char *corner = &pixels[0];
			const bool visibleRed = visibleColor == red;
			require(visibleRed ?
				(center[0] < 16 && center[1] < 16 && center[2] > 240) :
				(center[0] < 16 && center[1] > 240 && center[2] < 16),
				"captured native texture sample has the expected current color");
			require(corner[0] > 240 && corner[1] < 16 && corner[2] < 16,
				"captured native texture frame is not an empty or stale clear");
			captures.push_back(pixels);
		}
		require(device->present() == RENDER_RESULT_OK,
			"present native texture pipeline frame");
		sampleReady = true;
		previousColor = currentColor;
		if (frame % 3 == 2) checkCompletions();
		if (frame == 5)
		{
			const GpuHandle staleCopy = copiedColor;
			require(device->destroyResource(copiedColor),
				"destroy native copy target before generation reuse");
			require(!device->destroyResource(staleCopy),
				"reject stale native copy target generation");
			GpuHandle replacement;
			require(device->createTexture(targetDescriptor, 0, 0, &replacement) ==
				RENDER_RESULT_OK && replacement != staleCopy,
				"recreate native copy target with a new logical generation");
			copiedColor = replacement;
			sampleReady = false;
		}
	}
	if (threaded)
	{
		require(DrainThreadedRenderDevice(device.get()) == RENDER_RESULT_OK,
			"drain native texture-pipeline owner");
		checkCompletions();
		require(completed == 12,
			"native texture pipeline retains one successful completion per frame");
	}
	require(device->destroyResource(sourceTexture) &&
		device->destroyResource(offscreenColor) &&
		device->destroyResource(offscreenDepth) &&
		device->destroyResource(copiedColor) &&
		device->destroyResource(vertexBuffer),
		"destroy native texture-pipeline resources");
	if (threaded)
		require(DrainThreadedRenderDevice(device.get()) == RENDER_RESULT_OK,
			"drain native texture-pipeline resource destruction");
	unsigned int validationErrors = 0;
	const RenderResult debug = device->getDebugValidationErrorCount(&validationErrors);
	require(debug == RENDER_RESULT_UNSUPPORTED ||
		(debug == RENDER_RESULT_OK && validationErrors == 0),
		"native texture pipeline has no debug validation errors");
	device->shutdown();
	return captures;
}
}

int main()
{
	try
	{
		require(rts::JobSystem::instance().registerCurrentThread(rts::JOB_OWNER_GAME), "register producer owner");
		const std::vector<Pixels> reference = run(false, false, 2);
		require(run(true, true, 2) == reference, "serial native owner pixels equal direct D3D11 reference");
		require(run(true, false, 2) == reference, "two-slot native owner pixels equal direct D3D11 reference");
		require(run(true, false, 3) == reference, "three-slot native owner pixels equal direct D3D11 reference");
		const std::vector<Pixels> textureReference = runTexturePipeline(false, false, 2);
		require(runTexturePipeline(true, true, 2) == textureReference,
			"serial native texture/copy pixels equal direct D3D11 reference");
		require(runTexturePipeline(true, false, 2) == textureReference,
			"two-slot native texture/copy pixels equal direct D3D11 reference");
		require(runTexturePipeline(true, false, 3) == textureReference,
			"three-slot native texture/copy pixels equal direct D3D11 reference");
		require(rts::JobSystem::instance().unregisterCurrentThread(rts::JOB_OWNER_GAME), "release producer owner");
		std::puts("Real D3D11 threaded pixel/lifecycle contracts passed");
		return 0;
	}
	catch (const std::exception &error)
	{
		std::fprintf(stderr, "Native threaded renderer failure: %s\n", error.what());
		return 1;
	}
}
