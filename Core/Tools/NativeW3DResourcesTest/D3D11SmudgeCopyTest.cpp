#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "Renderer/NativeW3DResources.h"
#include "Renderer/RenderGameClientNative.h"
#include "Renderer/ThreadedRenderDevice.h"
#include "nativew3dtextureowner.h"
#include "W3DDevice/GameClient/W3DSmudgeUVMapping.h"

#include <cmath>
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

// The production texture owner pins this logical-target query before admitting
// its real backend copy. The fixture selects the same 256-square reflection
// output as renderMirror without needing a game/profile or a visible window.
class TargetOwner : public IGameRenderClientNativeOwner
{
public:
	RenderBackBufferInfo target;
	bool IsInitialized() const override { return true; }
	bool IsOperational() const override { return true; }
	GameRenderTargetKind ActiveRenderTargetKind() const override
		{ return GAME_RENDER_TARGET_TEXTURE; }
	RenderResult GetGameRenderTargetInfo(RenderBackBufferInfo *info) const override
		{ if (!info) return RENDER_RESULT_INVALID_ARGUMENT; *info = target; return RENDER_RESULT_OK; }
};

struct HiddenWindow
{
	HWND value;
	HiddenWindow() : value(CreateWindowExW(0, L"STATIC", L"", WS_OVERLAPPED,
		0, 0, 64, 64, 0, 0, GetModuleHandleW(0), 0))
		{ Require(value != 0, "hidden window creation"); }
	~HiddenWindow() { DestroyWindow(value); }
};

struct OwnerBinding
{
	NativeW3DResources &resources;
	OwnerBinding(TargetOwner &owner, NativeW3DResources &value) : resources(value)
		{ SetGameRenderClientNativeOwner(&owner); }
	~OwnerBinding()
	{
		SetGameRenderClientNativeOwner(0);
		UnbindNativeW3DTextureResources(&resources);
	}
};

struct OpenFrameCleanup
{
	IRenderDevice *device;
	IRenderContext *context;
	bool threaded, open;
	OpenFrameCleanup(IRenderDevice *value, IRenderContext *commands, bool queued) :
		device(value), context(commands), threaded(queued), open(false) {}
	~OpenFrameCleanup()
	{
		if (open)
		{
			try
			{
				const RenderResult ended = context->endFrame();
				if (threaded && ended == RENDER_RESULT_OK)
					SubmitThreadedRenderFrame(device, false);
				if (threaded) DrainThreadedRenderDevice(device);
			}
			catch (...) {}
		}
	}
};

bool Near(float value, float expected) { return std::fabs(value - expected) < 0.000001f; }

void TestMapping()
{
	const W3DSmudgeUVAxis reflection(0, 1, 256, 3840);
	Require(Near(reflection.Project(0), 128.0f / 3840) &&
		Near(reflection.Constrain(reflection.Project(1)), 255.5f / 3840) &&
		Near(reflection.Constrain(reflection.Project(-1)), 0.5f / 3840),
		"production reflection mapping uses copied pixel centers");
	const W3DSmudgeUVAxis viewport(0.25f, 0.75f, 256, 1920);
	Require(Near(viewport.Project(-1), 64.0f / 1920) &&
		Near(viewport.Project(0), 128.0f / 1920) &&
		Near(viewport.Project(1), 192.0f / 1920),
		"production mapping preserves viewport origin and aspect span");
	const W3DSmudgeUVAxis main(0, 0.8f, 2160, 2160);
	Require(Near(main.Project(1), 0.8f) && main.Constrain(-0.1f) == -0.1f,
		"main tactical viewport retains full-texture hardware clamp behavior");
}

struct Vertex { float x, y, z, nx, ny, nz; unsigned int color; float u, v; };

void TestCopy(bool threaded, bool serial, unsigned int samples,
	unsigned int width, unsigned int height, bool reflectionOnly)
{
	HiddenWindow window;
	ThreadedRenderOptions options;
	options.serial = serial;
	std::unique_ptr<IRenderDevice> device(threaded ?
		CreateThreadedRenderDevice(Factory, 0, options) : CreateD3D11RenderDevice());
	Require(device.get() != 0, "device allocation");
	RenderDeviceParameters parameters;
	parameters.backend = RENDER_BACKEND_D3D11;
	parameters.window = window.value;
	parameters.width = width;
	parameters.height = height;
	parameters.enableDebugLayer = true;
	parameters.enableVsync = false;
	parameters.multisampleCount = samples;
	Require(device->initialize(parameters) == RENDER_RESULT_OK, "native device initialization");
	RenderBackBufferInfo info;
	Require(device->getBackBufferInfo(&info) == RENDER_RESULT_OK &&
		info.multisampleCount == samples, "fixture uses requested AA mode");
	NativeW3DResourceHost host(16);
	NativeW3DResources resources(16);
	Require(host.Attach(device.get(), device->immediateContext()) == RENDER_RESULT_OK &&
		resources.BindHost(&host) == RENDER_RESULT_OK &&
		BindNativeW3DTextureResources(&resources) == RENDER_RESULT_OK, "typed resource binding");
	TargetOwner gameOwner;
	gameOwner.target.width = gameOwner.target.height = 256;
	gameOwner.target.format = info.format;
	OwnerBinding ownerBinding(gameOwner, resources);
	IRenderContext *context = device->immediateContext();
	OpenFrameCleanup frameCleanup(device.get(), context, threaded);
	TextureDescriptor backgroundDescriptor;
	backgroundDescriptor.width = width;
	backgroundDescriptor.height = height;
	backgroundDescriptor.format = info.format;
	backgroundDescriptor.binding = RENDER_TEXTURE_SHADER_RESOURCE;
	backgroundDescriptor.usage = RENDER_USAGE_DEFAULT;
	std::vector<unsigned int> blue(width * height, 0xffff0000U);
	TextureSubresourceData blueData;
	blueData.data = blue.data();
	blueData.rowPitch = width * 4;
	blueData.slicePitch = blue.size() * 4;
	NativeW3DTextureOwner background;
	NativeW3DTextureCandidate candidate;
	Require(background.CreateCandidate(backgroundDescriptor, &blueData, 1,
		&candidate) == RENDER_RESULT_OK &&
		background.PublishCandidate(&candidate, false) == RENDER_RESULT_OK,
		"production texture owner creates display-sized background");
	TextureDescriptor reflectionDescriptor = backgroundDescriptor;
	reflectionDescriptor.width = reflectionDescriptor.height = 256;
	reflectionDescriptor.binding |= RENDER_TEXTURE_RENDER_TARGET;
	NativeW3DTextureHandle reflection;
	Require(resources.CreateTexture(reflectionDescriptor, 0, 0, &reflection) ==
		RENDER_RESULT_OK, "real 256-square reflection output");
	NativeW3DTextureHandle independent;
	Require(resources.CreateTexture(backgroundDescriptor, &blueData, 1, &independent) ==
		RENDER_RESULT_OK, "independent CPU texture");
	TextureDescriptor immutableDescriptor = backgroundDescriptor;
	immutableDescriptor.usage = RENDER_USAGE_IMMUTABLE;
	NativeW3DTextureHandle immutable;
	Require(resources.CreateTexture(immutableDescriptor, &blueData, 1, &immutable) ==
		RENDER_RESULT_OK, "immutable negative destination");
	const W3DSmudgeUVAxis uvX(0, 1, 256, width), uvY(0, 1, 256, height);
	const float u0 = uvX.Constrain(uvX.Project(-1)), u1 = uvX.Constrain(uvX.Project(1));
	const float v0 = uvY.Constrain(uvY.Project(-1)), v1 = uvY.Constrain(uvY.Project(1));
	Vertex vertices[] = {
		{ -1, 1, 0, 0, 0, -1, 0xffffffffU, u0, v0 },
		{ 1, 1, 0, 0, 0, -1, 0xffffffffU, u1, v0 },
		{ -1, -1, 0, 0, 0, -1, 0xffffffffU, u0, v1 },
		{ 1, -1, 0, 0, 0, -1, 0xffffffffU, u1, v1 }
	};
	const unsigned short indices[] = { 0, 1, 2, 2, 1, 3 };
	LegacyVertexLayout layout;
	layout.stride = sizeof(Vertex);
	layout.elementCount = 4;
	layout.elements[0].semantic = RENDER_VERTEX_SEMANTIC_POSITION;
	layout.elements[0].format = RENDER_VERTEX_DATA_FLOAT3;
	layout.elements[0].byteOffset = 0;
	layout.elements[1].semantic = RENDER_VERTEX_SEMANTIC_NORMAL;
	layout.elements[1].format = RENDER_VERTEX_DATA_FLOAT3;
	layout.elements[1].byteOffset = 12;
	layout.elements[2].semantic = RENDER_VERTEX_SEMANTIC_DIFFUSE;
	layout.elements[2].format = RENDER_VERTEX_DATA_COLOR_BGRA8;
	layout.elements[2].byteOffset = 24;
	layout.elements[3].semantic = RENDER_VERTEX_SEMANTIC_TEXTURE_COORDINATE;
	layout.elements[3].format = RENDER_VERTEX_DATA_FLOAT2;
	layout.elements[3].byteOffset = 28;
	BufferDescriptor vertexDescriptor;
	vertexDescriptor.byteCount = sizeof(vertices);
	vertexDescriptor.stride = sizeof(Vertex);
	vertexDescriptor.usage = RENDER_USAGE_DEFAULT;
	BufferDescriptor indexDescriptor;
	indexDescriptor.byteCount = sizeof(indices);
	indexDescriptor.stride = sizeof(unsigned short);
	indexDescriptor.binding = RENDER_BUFFER_INDEX;
	indexDescriptor.usage = RENDER_USAGE_DEFAULT;
	GpuHandle vertexBuffer, indexBuffer;
	Require(resources.CreateBuffer(vertexDescriptor, vertices, sizeof(vertices), &vertexBuffer) ==
		RENDER_RESULT_OK && resources.CreateBuffer(indexDescriptor, indices, sizeof(indices),
		&indexBuffer) == RENDER_RESULT_OK, "independent initialized static vertex/index buffers");
	struct ColorVertex { float x, y, z; unsigned int color; };
	const ColorVertex sourcePattern[] = {
		{ -1, 1, 0, 0xffff0000U }, { 0, 1, 0, 0xffff0000U },
		{ -1, 0, 0, 0xffff0000U }
	};
	BufferDescriptor patternDescriptor;
	patternDescriptor.byteCount = sizeof(sourcePattern);
	patternDescriptor.stride = sizeof(ColorVertex);
	patternDescriptor.usage = RENDER_USAGE_DEFAULT;
	GpuHandle patternBuffer;
	Require(resources.CreateBuffer(patternDescriptor, sourcePattern, sizeof(sourcePattern), &patternBuffer) ==
		RENDER_RESULT_OK, "asymmetric reflection color pattern buffer");
	NativeW3DTextureDescription independentBefore;
	Require(resources.DescribeTexture(independent.resource, &independentBefore) ==
		RENDER_RESULT_OK, "independent texture authority snapshot");
	auto unchanged = [&]()
	{
		NativeW3DTextureDescription after;
		GpuHandle vertex, index;
		Require(resources.DescribeTexture(independent.resource, &after) == RENDER_RESULT_OK &&
			after.authority == independentBefore.authority &&
			after.authorityEpoch == independentBefore.authorityEpoch &&
			resources.AcquireVertexBufferRange(vertexBuffer, sizeof(Vertex), 0, 0, 4, &vertex) ==
				RENDER_RESULT_OK &&
			resources.AcquireIndexBufferRange(indexBuffer, RENDER_FORMAT_R16_UINT, 0, 0, 6, &index) ==
				RENDER_RESULT_OK,
			"rejected prevalidation preserves unrelated texture and static/index authorities");
	};
	NativeW3DGpuContentLease lease;
	Require(context->beginFrame() == RENDER_RESULT_OK, "begin reflection frame");
	frameCleanup.open = true;
	if (!reflectionOnly)
	{
		Require(resources.CopyActiveColorTargetToTexture(immutable.resource, &lease) ==
			RENDER_RESULT_UNSUPPORTED && !lease.isValid(), "immutable rejection before command admission");
		unchanged();
		gameOwner.target.width = width + 1;
		Require(background.CopyActiveColorTarget(&lease) == RENDER_RESULT_UNSUPPORTED,
			"actual texture owner rejects too-small destination before copy");
		unchanged();
		gameOwner.target.width = 256;
		gameOwner.target.format = RENDER_FORMAT_R8G8B8A8_UNORM;
		Require(background.CopyActiveColorTarget(&lease) == RENDER_RESULT_UNSUPPORTED,
			"actual texture owner rejects format mismatch before copy");
		unchanged();
		gameOwner.target.format = info.format;
		gameOwner.target.multisampleCount = 4;
		Require(background.CopyActiveColorTarget(&lease) == RENDER_RESULT_UNSUPPORTED,
			"actual texture owner rejects differently-sized MSAA source before copy");
		unchanged();
		gameOwner.target.multisampleCount = 1;
	}
	RenderTargetBinding binding;
	binding.useBackBufferColor = binding.useBackBufferDepth = false;
	binding.hasColor = true;
	binding.color.resource = reflection.resource;
	LegacyLogicalState patternState;
	patternState.pipeline.rasterizer.cullMode = RENDER_CULL_NONE;
	patternState.pipeline.depthStencil.depthEnable = false;
	Require(context->setRenderTargets(binding) == RENDER_RESULT_OK &&
		context->clearTargets(RENDER_CLEAR_COLOR, RenderFloat4(0, 1, 0, 1), 1, 0) ==
			RENDER_RESULT_OK &&
		context->setViewport(0, 0, 256, 256, 0, 1) == RENDER_RESULT_OK &&
		context->setLegacyState(patternState, RENDER_VERTEX_POSITION3_COLOR, 0) == RENDER_RESULT_OK &&
		context->setVertexBuffer(patternBuffer, sizeof(ColorVertex), 0) == RENDER_RESULT_OK &&
		context->setPrimitiveTopology(RENDER_PRIMITIVE_TRIANGLE_LIST) == RENDER_RESULT_OK &&
		context->draw(3, 0) == RENDER_RESULT_OK, "draw asymmetric actual reflection pattern");
	const RenderResult reflectionCopy = background.CopyActiveColorTarget(&lease);
	if (reflectionCopy != RENDER_RESULT_OK)
	{
		NativeW3DTextureDescription after;
		GpuHandle vertex, index;
		const RenderResult describe = resources.DescribeTexture(independent.resource, &after);
		const RenderResult acquireVertex = resources.AcquireVertexBufferRange(vertexBuffer,
			sizeof(Vertex), 0, 0, 4, &vertex);
		const RenderResult acquireIndex = resources.AcquireIndexBufferRange(indexBuffer,
			RENDER_FORMAT_R16_UINT, 0, 0, 6, &index);
		std::fprintf(stderr,
			"COPY_FAILURE result=%d source=256x256 destination=%ux%u lease=%u describe=%d texture_authority=%d->%d texture_epoch=%u->%u vertex_acquire=%d index_acquire=%d\n",
			reflectionCopy, width, height, lease.isValid(), describe,
			independentBefore.authority, after.authority,
			independentBefore.authorityEpoch, after.authorityEpoch, acquireVertex, acquireIndex);
	}
	Require(reflectionCopy == RENDER_RESULT_OK && lease.isValid(),
		"production texture owner copies actual 256 reflection into larger background");
	unchanged();
	NativeW3DTextureHandle sampled;
	Require(background.AcquireForSampling(&sampled, &lease) == RENDER_RESULT_OK,
		"copied content has current GPU sampling lease");
	LegacyLogicalState logical;
	logical.pipeline.rasterizer.cullMode = RENDER_CULL_NONE;
	logical.pipeline.depthStencil.depthEnable = false;
	logical.pipeline.textureStages[0].colorOperation = RENDER_TEXTURE_OP_SELECT_ARGUMENT_1;
	logical.pipeline.textureStages[0].colorArgument1 = RENDER_TEXTURE_ARG_TEXTURE;
	Require(context->setRenderTargets(GpuHandle(), GpuHandle()) == RENDER_RESULT_OK &&
		context->clear(RenderFloat4(1, 0, 0, 1), 1, 0) == RENDER_RESULT_OK &&
		context->setViewport(0, 0, (float)width, (float)height, 0, 1) == RENDER_RESULT_OK &&
		context->setLegacyStateForLayout(logical, layout, 1) ==
			RENDER_RESULT_OK && context->setVertexBuffer(vertexBuffer, sizeof(Vertex), 0) ==
			RENDER_RESULT_OK && context->setIndexBuffer(indexBuffer, RENDER_FORMAT_R16_UINT, 0) ==
			RENDER_RESULT_OK && context->setTexture(0, sampled.resource) == RENDER_RESULT_OK &&
		context->setPrimitiveTopology(RENDER_PRIMITIVE_TRIANGLE_LIST) == RENDER_RESULT_OK &&
		context->drawIndexed(6, 0, 0) == RENDER_RESULT_OK && context->endFrame() == RENDER_RESULT_OK,
		"restore main output and sample production-mapped reflection region");
	frameCleanup.open = false;
	std::vector<unsigned char> pixels(width * height * 4);
	RenderFormat capturedFormat;
	Require(device->captureBackBuffer(pixels.data(), pixels.size(), width * 4, &capturedFormat) ==
		RENDER_RESULT_OK, "reflection regression readback");
	for (unsigned int y : { 4U, height / 2, height - 5 })
		for (unsigned int x : { 4U, width / 2, width - 5 })
		{
			const unsigned char *pixel = &pixels[(y * width + x) * 4];
			const bool redCorner = x == 4 && y == 4;
			Require(pixel[0] < 16 && (redCorner ?
				(pixel[1] < 16 && pixel[2] > 240) : (pixel[1] > 240 && pixel[2] < 16)),
				"copied region pixels and UV coverage survive aspect ratio and AA");
		}
	Require(device->present() == RENDER_RESULT_OK, "visible frame remains presentable after reflection");
	if (threaded) Require(DrainThreadedRenderDevice(device.get()) == RENDER_RESULT_OK,
		"owner-mode completion remains successful");
	// A nonzero viewport origin samples the right side of the asymmetric
	// reflection, where every pixel is green. Ignoring the origin would expose
	// the red upper-left triangle and fail this real-pixel check.
	const W3DSmudgeUVAxis offsetX(0.6f, 0.9f, 256, width);
	vertices[0].u = vertices[2].u = offsetX.Constrain(offsetX.Project(-1));
	vertices[1].u = vertices[3].u = offsetX.Constrain(offsetX.Project(1));
	Require(resources.UpdateBuffer(vertexBuffer, vertices, sizeof(vertices), 0,
		RENDER_BUFFER_UPDATE_PRESERVE) == RENDER_RESULT_OK &&
		context->beginFrame() == RENDER_RESULT_OK, "begin nonzero camera viewport sample");
	frameCleanup.open = true;
	Require(context->setRenderTargets(GpuHandle(), GpuHandle()) == RENDER_RESULT_OK &&
		context->clear(RenderFloat4(0, 0, 1, 1), 1, 0) == RENDER_RESULT_OK &&
		context->setViewport(0, 0, (float)width, (float)height, 0, 1) == RENDER_RESULT_OK &&
		context->setLegacyStateForLayout(logical, layout, 1) ==
			RENDER_RESULT_OK && context->setVertexBuffer(vertexBuffer, sizeof(Vertex), 0) ==
			RENDER_RESULT_OK && context->setIndexBuffer(indexBuffer, RENDER_FORMAT_R16_UINT, 0) ==
			RENDER_RESULT_OK && context->setTexture(0, sampled.resource) == RENDER_RESULT_OK &&
		context->setPrimitiveTopology(RENDER_PRIMITIVE_TRIANGLE_LIST) == RENDER_RESULT_OK &&
		context->drawIndexed(6, 0, 0) == RENDER_RESULT_OK && context->endFrame() == RENDER_RESULT_OK &&
		device->captureBackBuffer(pixels.data(), pixels.size(), width * 4, &capturedFormat) ==
			RENDER_RESULT_OK, "sample nonzero camera viewport with actual reflection pixels");
	frameCleanup.open = false;
	for (unsigned int y : { 4U, height / 2, height - 5 })
		for (unsigned int x : { 4U, width / 2, width - 5 })
		{
			const unsigned char *pixel = &pixels[(y * width + x) * 4];
			Require(pixel[0] < 16 && pixel[1] > 240 && pixel[2] < 16,
				"nonzero viewport origin never mirrors or samples the excluded red corner");
		}
	Require(device->present() == RENDER_RESULT_OK, "nonzero viewport sample remains presentable");
	if (threaded) Require(DrainThreadedRenderDevice(device.get()) == RENDER_RESULT_OK,
		"nonzero viewport owner completion succeeds");
	// The next pass restores the main output. Its exact-size single-sample
	// copy / AA resolve must keep the original full-target semantics.
	gameOwner.target = info;
	Require(context->beginFrame() == RENDER_RESULT_OK, "begin main target copy frame");
	frameCleanup.open = true;
	Require(
		context->setRenderTargets(GpuHandle(), GpuHandle()) == RENDER_RESULT_OK &&
		context->clear(RenderFloat4(1, 0, 1, 1), 1, 0) == RENDER_RESULT_OK &&
		background.CopyActiveColorTarget(&lease) == RENDER_RESULT_OK &&
		background.AcquireForSampling(&sampled, &lease) == RENDER_RESULT_OK &&
		context->clear(RenderFloat4(0, 0, 1, 1), 1, 0) == RENDER_RESULT_OK &&
		context->setViewport(0, 0, (float)width, (float)height, 0, 1) == RENDER_RESULT_OK &&
		context->setLegacyStateForLayout(logical, layout, 1) ==
			RENDER_RESULT_OK && context->setVertexBuffer(vertexBuffer, sizeof(Vertex), 0) ==
			RENDER_RESULT_OK && context->setIndexBuffer(indexBuffer, RENDER_FORMAT_R16_UINT, 0) ==
			RENDER_RESULT_OK && context->setPrimitiveTopology(RENDER_PRIMITIVE_TRIANGLE_LIST) ==
			RENDER_RESULT_OK &&
		context->setTexture(0, sampled.resource) == RENDER_RESULT_OK &&
		context->drawIndexed(6, 0, 0) == RENDER_RESULT_OK && context->endFrame() == RENDER_RESULT_OK &&
		device->captureBackBuffer(pixels.data(), pixels.size(), width * 4, &capturedFormat) ==
			RENDER_RESULT_OK, "main target exact-size copy/AA resolve after reflection restore");
	frameCleanup.open = false;
	const unsigned char *mainPixel = &pixels[(height / 2 * width + width / 2) * 4];
	Require(mainPixel[0] > 240 && mainPixel[1] < 16 && mainPixel[2] > 240,
		"exact-size main copy/resolve supplies current pixels");
	unchanged();
	Require(device->present() == RENDER_RESULT_OK, "main pass present after restored copy");
	if (threaded) Require(DrainThreadedRenderDevice(device.get()) == RENDER_RESULT_OK,
		"main pass owner completion succeeds");
	unsigned int errors = 0;
	Require(device->getDebugValidationErrorCount(&errors) == RENDER_RESULT_OK && errors == 0,
		"reflection copy has no D3D debug-layer errors");
	Require(background.Reset() == RENDER_RESULT_OK, "background teardown");
	SetGameRenderClientNativeOwner(0);
	Require(UnbindNativeW3DTextureResources(&resources) == RENDER_RESULT_OK &&
		resources.Shutdown() == RENDER_RESULT_OK && host.Detach() == RENDER_RESULT_OK,
		"isolated owner/resource teardown");
	device->shutdown();
	std::printf("PASS smudge-copy threaded=%u serial=%u samples=%u %ux%u\n",
		threaded, serial, samples, width, height);
}
}

int main(int argc, char **argv)
{
	try
	{
		TestMapping();
		const bool reflectionOnly = argc == 2 && std::strcmp(argv[1], "--reflection-only") == 0;
		for (unsigned int samples : { 1U, 4U })
			for (unsigned int mode = 0; mode < 3; ++mode)
				TestCopy(mode != 0, mode == 1, samples, samples == 1 ? 360 : 512,
					samples == 1 ? 288 : 320, reflectionOnly);
		return 0;
	}
	catch (const std::exception &error)
	{
		SetGameRenderClientNativeOwner(0);
		std::fprintf(stderr, "FAILED: %s\n", error.what());
		return 1;
	}
}
