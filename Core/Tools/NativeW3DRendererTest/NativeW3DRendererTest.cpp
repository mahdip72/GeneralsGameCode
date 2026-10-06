#include "Renderer/NativeW3DRenderer.h"
#include "Renderer/RigidInstancingPolicy.h"
#include <climits>
#include "Renderer/NativeW3DResources.h"
#include "Renderer/NativeW3DRenderState.h"
#include "Renderer/RenderTexturePublication.h"

#include <cstdio>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace rts { namespace render {
// Exercise the existing internal lifecycle seam with the production renderer,
// state and queue. This grants no extra access to runtime callers.
class NativeW3DRecoveryTestAccess
{
public:
	static RenderResult Attach(NativeW3DRenderer &renderer,
		NativeW3DRenderState *state, IRenderDevice *device)
	{
		const RenderResult attached = state->AttachBackend(device,
			device->immediateContext());
		return attached == RENDER_RESULT_OK ?
			renderer.AttachBorrowedState(state) : attached;
	}
	static bool BackendOperational(const NativeW3DRenderer &renderer)
	{
		return renderer.IsBackendOperational();
	}
	static IRenderDevice *Device(const NativeW3DRenderState &state)
	{
		return state.Device();
	}
	static RenderResult Detach(NativeW3DRenderer &renderer,
		NativeW3DRenderState *state)
	{
		const RenderResult detached = renderer.DetachBorrowedState();
		return detached == RENDER_RESULT_OK ? state->DetachBackend() : detached;
	}
};
} }

namespace
{
int Check(bool condition, const char *message)
{
	if (condition)
	{
		return 0;
	}
	std::fprintf(stderr, "FAIL: %s\n", message);
	return 1;
}

using namespace rts::render;
// Exercise producer admission against real resource authority tables, with a
// deterministic synchronous context that records only accepted commands.
class InstancingTraceContext : public rts::render::IRenderContext
{
public:
	InstancingTraceContext() : commands(0), ordinary(0), instanced(0),
		drawResult(rts::render::RENDER_RESULT_OK), stateResult(rts::render::RENDER_RESULT_OK) {}
	unsigned int commands, ordinary, instanced;
	rts::render::RenderResult drawResult, stateResult;
	rts::render::RenderMatrix4 copied[rts::render::RENDER_RIGID_INSTANCE_MAX];
	unsigned int copiedCount;

RenderResult beginFrame() { return RENDER_RESULT_OK; }
RenderResult updateBuffer(GpuHandle, const void *, size_t, size_t, RenderBufferUpdateMode) { return RENDER_RESULT_OK; }
RenderResult clear(const RenderFloat4 &, float, unsigned int) { ++commands; return RENDER_RESULT_OK; }
RenderResult clearTargets(unsigned int, const RenderFloat4 &, float, unsigned int) { ++commands; return RENDER_RESULT_OK; }
RenderResult setRenderTargets(const RenderTargetBinding &) { ++commands; return RENDER_RESULT_OK; }
RenderResult setRenderTargets(GpuHandle, GpuHandle) { ++commands; return RENDER_RESULT_OK; }
RenderResult setViewport(float, float, float, float, float, float) { ++commands; return RENDER_RESULT_OK; }
RenderResult setLegacyState(const LegacyLogicalState &, LegacyVertexFormat, unsigned int) { ++commands; return stateResult; }
RenderResult setLegacyStateForLayout(const LegacyLogicalState &, const LegacyVertexLayout &, unsigned int) { ++commands; return stateResult; }
RenderResult setVertexBuffer(GpuHandle, unsigned int, unsigned int) { ++commands; return RENDER_RESULT_OK; }
RenderResult setIndexBuffer(GpuHandle, RenderFormat, unsigned int) { ++commands; return RENDER_RESULT_OK; }
RenderResult setTexture(unsigned int, GpuHandle) { ++commands; return RENDER_RESULT_OK; }
RenderResult setPrimitiveTopology(RenderPrimitiveTopology) { ++commands; return RENDER_RESULT_OK; }
RenderResult draw(unsigned int, unsigned int) { ++ordinary; return RENDER_RESULT_OK; }
RenderResult drawIndexed(unsigned int, unsigned int, int) { ++ordinary; return RENDER_RESULT_OK; }
RenderResult drawIndexedInstanced(unsigned int, unsigned int, int, const RenderMatrix4 *worlds, unsigned int count)
{
	++commands; ++instanced; copiedCount = count;
	for (unsigned int i = 0; i < count; ++i) copied[i] = worlds[i];
	return drawResult;
}
RenderResult endFrame() { return RENDER_RESULT_OK; }
};
class InstancingTraceDevice : public IRenderDevice
{
public:
	InstancingTraceDevice() : capability(true), handles(16) {}
	bool capability;
	GpuHandleAllocator handles;
	InstancingTraceContext context;
	RenderBackend backend() const { return RENDER_BACKEND_D3D11; }
	bool isOperational() const { return true; }
	bool supportsRigidInstancing() const { return capability; }
	RenderResult initialize(const RenderDeviceParameters &) { return RENDER_RESULT_OK; }
	void shutdown() {}
	IRenderContext *immediateContext() { return &context; }
	RenderResult createBuffer(const BufferDescriptor &, const void *, size_t, GpuHandle *buffer)
	{ *buffer = handles.allocate(); return buffer->isValid() ? RENDER_RESULT_OK : RENDER_RESULT_FAILED; }
	RenderResult createTexture(const TextureDescriptor &, const TextureSubresourceData *, unsigned int, GpuHandle *)
	{ return RENDER_RESULT_UNSUPPORTED; }
	RenderResult refreshTexture(GpuHandle, const TextureDescriptor &, const TextureSubresourceData *, unsigned int)
	{ return RENDER_RESULT_UNSUPPORTED; }
	RenderResult copyActiveColorTargetToTexture(GpuHandle) { return RENDER_RESULT_UNSUPPORTED; }
	bool destroyResource(GpuHandle buffer) { return handles.release(buffer); }
	RenderResult recoverDevice() { return RENDER_RESULT_UNSUPPORTED; }
	RenderResult resize(unsigned int, unsigned int) { return RENDER_RESULT_UNSUPPORTED; }
	RenderResult present() { return RENDER_RESULT_OK; }
	RenderResult getBackBufferInfo(RenderBackBufferInfo *) const { return RENDER_RESULT_UNSUPPORTED; }
	RenderResult captureBackBuffer(void *, size_t, size_t, RenderFormat *) { return RENDER_RESULT_UNSUPPORTED; }
	RenderResult getDebugValidationErrorCount(unsigned int *count) const { *count = 0; return RENDER_RESULT_OK; }
	RenderResult reportDebugLiveObjects() { return RENDER_RESULT_UNSUPPORTED; }
};

NativeDrawPacket RigidTestPacket()
{
	NativeDrawPacket p;
	p.indexed = true; p.vertexCount = 3; p.indexCount = 3;
	p.vertexStride = p.vertexLayout.stride = 36;
	p.vertexFormat = RENDER_VERTEX_POSITION3_NORMAL_COLOR_TEX1;
	p.vertexLayout.elementCount = 4;
	const RenderVertexSemantic semantics[] = { RENDER_VERTEX_SEMANTIC_POSITION,
		RENDER_VERTEX_SEMANTIC_NORMAL, RENDER_VERTEX_SEMANTIC_DIFFUSE, RENDER_VERTEX_SEMANTIC_TEXTURE_COORDINATE };
	const RenderVertexDataFormat formats[] = { RENDER_VERTEX_DATA_FLOAT3,
		RENDER_VERTEX_DATA_FLOAT3, RENDER_VERTEX_DATA_COLOR_BGRA8, RENDER_VERTEX_DATA_FLOAT2 };
	const unsigned int offsets[] = { 0, 12, 24, 28 };
	for (unsigned int i = 0; i < 4; ++i) {
		p.vertexLayout.elements[i].semantic = semantics[i];
		p.vertexLayout.elements[i].format = formats[i];
		p.vertexLayout.elements[i].byteOffset = offsets[i];
	}
	return p;
}

int TestInstancingAdmission()
{
	int result = 0;
	InstancingTraceDevice device;
	NativeW3DRenderer renderer;
	NativeW3DResources resources;
	NativeW3DRenderState *owner = NativeW3DRenderState::Create(8);
	if (!owner) return Check(false, "instancing creates production owner state");
	result |= Check(owner->BindOwner() == RENDER_RESULT_OK &&
		NativeW3DRecoveryTestAccess::Attach(renderer, owner, &device) == RENDER_RESULT_OK &&
		resources.Bind(&renderer) == RENDER_RESULT_OK, "instancing binds resource authority");
	BufferDescriptor vb, ib;
	vb.byteCount = 6 * 36; vb.stride = 36; vb.binding = RENDER_BUFFER_VERTEX;
	vb.usage = RENDER_USAGE_DYNAMIC;
	ib.byteCount = 6 * sizeof(unsigned short); ib.stride = sizeof(unsigned short);
	ib.binding = RENDER_BUFFER_INDEX; ib.usage = RENDER_USAGE_DYNAMIC;
	unsigned char vertices[6 * 36] = {};
	unsigned short indices[6] = { 0, 1, 2, 3, 4, 5 };
	NativeDrawPacket packet = RigidTestPacket();
	result |= Check(resources.CreateBuffer(vb, 0, 0, &packet.vertexBuffer) == RENDER_RESULT_OK &&
		resources.CreateBuffer(ib, 0, 0, &packet.indexBuffer) == RENDER_RESULT_OK &&
		resources.UpdateBuffer(packet.vertexBuffer, vertices, 3 * 36, 0) == RENDER_RESULT_OK &&
		resources.UpdateBuffer(packet.indexBuffer, indices, 3 * sizeof(unsigned short), 0) == RENDER_RESULT_OK,
		"producer fixture publishes only initialized geometry prefixes");
	LegacyLogicalState state;
	RenderMatrix4 worlds[RENDER_RIGID_INSTANCE_MAX];
	worlds[1].values[12] = 2;
	const unsigned int before = device.context.commands;
	result |= Check(!device.IRenderDevice::supportsRigidInstancing() &&
		device.context.IRenderContext::drawIndexedInstanced(3, 0, 0, worlds, 2) == RENDER_RESULT_UNSUPPORTED &&
		device.context.commands == before, "default optional seam accepts no commands");
	result |= Check(renderer.SubmitInstancedExternal(resources, state, packet, worlds, 1) == RENDER_RESULT_INVALID_ARGUMENT &&
		renderer.SubmitInstancedExternal(resources, state, packet, worlds, 33) == RENDER_RESULT_INVALID_ARGUMENT &&
		renderer.SubmitInstancedExternal(resources, state, packet, 0, 2) == RENDER_RESULT_INVALID_ARGUMENT &&
		device.context.commands == before, "invalid capacity and null arrays admit no commands");
	device.capability = false;
	result |= Check(!renderer.SupportsRigidInstancing() &&
		renderer.SubmitInstancedExternal(resources, state, packet, worlds, 2) == RENDER_RESULT_UNSUPPORTED &&
		device.context.commands == before, "unsupported capability rejects before side effects");
	device.capability = true;
	NativeDrawPacket bad = packet; bad.minimumVertexIndex = 3;
	result |= Check(renderer.SubmitInstancedExternal(resources, state, bad, worlds, 2) == RENDER_RESULT_INVALID_ARGUMENT &&
		device.context.commands == before, "uninitialized declared vertex range rejects before admission");
	bad = packet; bad.startIndex = 3;
	result |= Check(renderer.SubmitInstancedExternal(resources, state, bad, worlds, 2) == RENDER_RESULT_INVALID_ARGUMENT &&
		device.context.commands == before, "uninitialized index prefix rejects before admission");
	bad = packet; bad.baseVertex = -1;
	result |= Check(renderer.SubmitInstancedExternal(resources, state, bad, worlds, 2) == RENDER_RESULT_INVALID_ARGUMENT &&
		device.context.commands == before, "negative effective minimum vertex rejects");
	bad = packet; bad.baseVertex = INT_MAX; bad.minimumVertexIndex = UINT_MAX;
	result |= Check(renderer.SubmitInstancedExternal(resources, state, bad, worlds, 2) == RENDER_RESULT_INVALID_ARGUMENT &&
		device.context.commands == before, "overflowing base plus minimum rejects");
	bad = packet; bad.vertexBuffer = GpuHandle(packet.vertexBuffer.index(), packet.vertexBuffer.generation() + 1);
	result |= Check(renderer.SubmitInstancedExternal(resources, state, bad, worlds, 2) == RENDER_RESULT_INVALID_ARGUMENT &&
		device.context.commands == before, "stale generation rejects before admission");
	bad = packet; bad.texturePresenceMask = 1;
	result |= Check(renderer.SubmitInstancedExternal(resources, state, bad, worlds, 2) == RENDER_RESULT_INVALID_ARGUMENT &&
		device.context.commands == before, "texture mask mismatch rejects before admission");
	result |= Check(renderer.SubmitInstancedExternal(resources, state, packet, worlds, 2) == RENDER_RESULT_OK &&
		device.context.instanced == 1 && device.context.ordinary == 0 &&
		device.context.copiedCount == 2 && device.context.copied[1].values[12] == 2,
		"one admitted batch owns distinct worlds without ordinary replay");
	worlds[1].values[12] = 99;
	result |= Check(device.context.copied[1].values[12] == 2, "caller world poison does not change accepted values");
	result |= Check(renderer.SubmitInstancedExternal(resources, state, packet, worlds, 32) == RENDER_RESULT_OK &&
		device.context.copiedCount == 32, "maximum capacity accepted");
	bad = packet; bad.minimumVertexIndex = 1; bad.baseVertex = -1;
	result |= Check(renderer.SubmitInstancedExternal(resources, state, bad, worlds, 2) == RENDER_RESULT_OK,
		"indexed range uses base plus minimum rather than startVertex");
	device.context.drawResult = RENDER_RESULT_UNSUPPORTED;
	const unsigned int ordinaryBefore = device.context.ordinary;
	result |= Check(renderer.SubmitInstancedExternal(resources, state, packet, worlds, 2) == RENDER_RESULT_FAILED &&
		device.context.ordinary == ordinaryBefore, "post-admission unsupported is terminal and never replays");
	device.context.drawResult = RENDER_RESULT_FAILED;
	result |= Check(renderer.SubmitInstancedExternal(resources, state, packet, worlds, 2) == RENDER_RESULT_FAILED &&
		device.context.ordinary == ordinaryBefore, "accepted execution failure is never ordinary replay");
	device.context.stateResult = RENDER_RESULT_UNSUPPORTED;
	const unsigned int instancedBefore = device.context.instanced;
	result |= Check(renderer.SubmitInstancedExternal(resources, state, packet, worlds, 2) == RENDER_RESULT_FAILED &&
		device.context.instanced == instancedBefore, "setup failure stops before draw and cannot invite replay");
	result |= Check(resources.Shutdown() == RENDER_RESULT_OK, "fixture releases resources");
	result |= Check(owner->BeginShutdown() == RENDER_RESULT_OK &&
		NativeW3DRecoveryTestAccess::Detach(renderer, owner) == RENDER_RESULT_OK, "fixture detaches borrowed state");
	owner->Release();
	return result;
}

int TestTexturePublicationContract()
{
	int result = 0;
	int sentinel = 0;
	TextureBaseClass *texture =
		reinterpret_cast<TextureBaseClass *>(&sentinel);
	TextureClass *textureClass = reinterpret_cast<TextureClass *>(&sentinel);
	rts::render::ResetTrackedLegacyState();
	rts::render::SeedTrackedLegacyPipelineState();
	rts::render::PublishTextureStage(0, texture);
	rts::render::LegacyLogicalState state;
	result |= Check(rts::render::GetPublishedTextureStage(0) == texture,
		"native texture publication retains the typed stage source");
	result |= Check(rts::render::GetTrackedLegacyLogicalState(&state) &&
		(state.texturePresenceMask & 1U) != 0,
		"native texture publication updates neutral texture presence");
	rts::render::PublishTextureStage(
		rts::render::LEGACY_TEXTURE_STAGE_COUNT, texture);
	result |= Check(rts::render::GetPublishedTextureStage(
		rts::render::LEGACY_TEXTURE_STAGE_COUNT) == 0,
		"native texture publication rejects an out-of-range stage");
	rts::render::RecordTextureUse(textureClass);
	result |= Check(rts::render::GetTextureUseCount() == 1,
		"native texture publication records one stage use");
	rts::render::UnpublishTexture(texture);
	result |= Check(rts::render::GetPublishedTextureStage(0) == 0,
		"native texture unpublication clears the stage source");
	result |= Check(rts::render::GetTrackedLegacyLogicalState(&state) &&
		(state.texturePresenceMask & 1U) == 0,
		"native texture unpublication clears neutral texture presence");
	return result;
}

int TestTexturePublicationOperationalContract()
{
	int result = 0;
	result |= Check(
		rts::render::IsRenderTexturePublicationOperationalState(
			true, false, false, false),
		"legacy publication remains operational before a scene frame");
	result |= Check(
		!rts::render::IsRenderTexturePublicationOperationalState(
			true, true, false, false),
		"legacy publication is suppressed while the device is lost or resetting");
	result |= Check(
		!rts::render::IsRenderTexturePublicationOperationalState(
			false, false, false, false),
		"publication is suppressed after renderer shutdown");
	result |= Check(
		rts::render::IsRenderTexturePublicationOperationalState(
			true, false, true, true),
		"native publication is operational after bridge recovery");
	result |= Check(
		!rts::render::IsRenderTexturePublicationOperationalState(
			true, false, true, false),
		"native publication is suppressed while the bridge is inactive");
	return result;
}

struct TextureStageCommand
{
	unsigned int stage;
	rts::render::GpuHandle texture;
};

class TextureStageTraceContext
{
public:
	TextureStageTraceContext() : failStage(-1), failNext(false), commands()
	{
	}

	rts::render::RenderResult setTexture(unsigned int stage,
		rts::render::GpuHandle texture)
	{
		TextureStageCommand command;
		command.stage = stage;
		command.texture = texture;
		commands.push_back(command);
		if (failNext && static_cast<int>(stage) == failStage)
		{
			failNext = false;
			return rts::render::RENDER_RESULT_FAILED;
		}
		return rts::render::RENDER_RESULT_OK;
	}

	int failStage;
	bool failNext;
	std::vector<TextureStageCommand> commands;
};

int TestTextureBindingCacheCommandTrace()
{
	using namespace rts::render;
	int result = 0;
	GpuHandle textures[LEGACY_TEXTURE_STAGE_COUNT];
	textures[1] = GpuHandle(4, 7);
	textures[5] = GpuHandle(9, 2);
	NativeW3DTextureBindingCache cache;
	TextureStageTraceContext trace;
	const unsigned int stageCount = LEGACY_TEXTURE_STAGE_COUNT;

	result |= Check(cache.Bind(&trace, textures) == RENDER_RESULT_OK &&
		trace.commands.size() == stageCount,
		"first sorted packet submits one texture command for each unknown stage");
	bool orderedStages = trace.commands.size() == stageCount;
	for (unsigned int stage = 0; orderedStages && stage < stageCount; ++stage)
	{
		orderedStages = trace.commands[stage].stage == stage &&
			trace.commands[stage].texture == textures[stage];
	}
	result |= Check(orderedStages,
		"first texture command trace preserves stage order and full handles");
	result |= Check(cache.Bind(&trace, textures) == RENDER_RESULT_OK &&
		trace.commands.size() == stageCount,
		"identical consecutive texture bindings emit no extra commands");

	textures[1] = GpuHandle(4, 8);
	result |= Check(cache.Bind(&trace, textures) == RENDER_RESULT_OK &&
		trace.commands.size() == stageCount + 1 &&
		trace.commands.back().stage == 1 &&
		trace.commands.back().texture == textures[1],
		"a reused texture slot with a new generation is submitted");

	const GpuHandle textureA = textures[1];
	textures[1] = GpuHandle();
	result |= Check(cache.Bind(&trace, textures) == RENDER_RESULT_OK &&
		trace.commands.size() == stageCount + 2 &&
		trace.commands.back().stage == 1 &&
		!trace.commands.back().texture.isValid(),
		"A-to-null transition submits the required unbind");
	textures[1] = textureA;
	result |= Check(cache.Bind(&trace, textures) == RENDER_RESULT_OK &&
		trace.commands.size() == stageCount + 3 &&
		trace.commands.back().stage == 1 &&
		trace.commands.back().texture == textureA,
		"null-to-A transition submits the required rebind");

	NativeW3DTextureBindingCache failureCache;
	TextureStageTraceContext failureTrace;
	failureTrace.failStage = 3;
	failureTrace.failNext = true;
	GpuHandle failureTextures[LEGACY_TEXTURE_STAGE_COUNT];
	failureTextures[0] = GpuHandle(20, 1);
	failureTextures[1] = GpuHandle(21, 1);
	failureTextures[2] = GpuHandle(22, 1);
	failureTextures[3] = GpuHandle(23, 1);
	result |= Check(failureCache.Bind(&failureTrace, failureTextures) ==
		RENDER_RESULT_FAILED && failureTrace.commands.size() == 4,
		"a failed texture command stops the current batch immediately");
	result |= Check(failureCache.Bind(&failureTrace, failureTextures) ==
		RENDER_RESULT_OK && failureTrace.commands.size() == 9 &&
		failureTrace.commands[4].stage == 3,
		"a failed stage is retried while earlier successful stages stay cached");

	NativeW3DTextureBindingCache nextBatchCache;
	const size_t beforeNextBatch = trace.commands.size();
	result |= Check(nextBatchCache.Bind(&trace, textures) == RENDER_RESULT_OK &&
		trace.commands.size() == beforeNextBatch + stageCount,
		"each sorted batch starts unknown and rebinds all stages");
	std::fprintf(stdout,
		"texture binding trace counts: first=%u repeat=0 generation=1 "
		"A-null-A=2 failed-attempt=4 retry=5 batch-reset=%u\n",
		stageCount, stageCount);
	return result;
}

enum SortedBatchCommandType
{
	SORTED_BATCH_TOPOLOGY_COMMAND,
	SORTED_BATCH_INDEX_BUFFER_COMMAND
};

struct SortedBatchCommand
{
	SortedBatchCommandType type;
	rts::render::GpuHandle buffer;
	rts::render::RenderFormat format;
	unsigned int offset;
	rts::render::RenderPrimitiveTopology topology;
};

class SortedBatchTraceContext
{
public:
	SortedBatchTraceContext() : failNextTopology(false), failNextIndexBuffer(false),
		commands()
	{
	}

	rts::render::RenderResult setPrimitiveTopology(
		rts::render::RenderPrimitiveTopology topology)
	{
		SortedBatchCommand command;
		command.type = SORTED_BATCH_TOPOLOGY_COMMAND;
		command.buffer = rts::render::GpuHandle();
		command.format = rts::render::RENDER_FORMAT_UNKNOWN;
		command.offset = 0;
		command.topology = topology;
		commands.push_back(command);
		if (failNextTopology)
		{
			failNextTopology = false;
			return rts::render::RENDER_RESULT_FAILED;
		}
		return rts::render::RENDER_RESULT_OK;
	}

	rts::render::RenderResult setIndexBuffer(rts::render::GpuHandle buffer,
		rts::render::RenderFormat format, unsigned int offset)
	{
		SortedBatchCommand command;
		command.type = SORTED_BATCH_INDEX_BUFFER_COMMAND;
		command.buffer = buffer;
		command.format = format;
		command.offset = offset;
		command.topology = rts::render::RENDER_PRIMITIVE_TRIANGLE_LIST;
		commands.push_back(command);
		if (failNextIndexBuffer)
		{
			failNextIndexBuffer = false;
			return rts::render::RENDER_RESULT_FAILED;
		}
		return rts::render::RENDER_RESULT_OK;
	}

	bool failNextTopology;
	bool failNextIndexBuffer;
	std::vector<SortedBatchCommand> commands;
};

int TestCompactRecordBindingAcknowledgements()
{
	using namespace rts::render;
	int result = 0;
	NativeW3DTextureBindingCache textures;
	TextureStageTraceContext textureTrace;
	GpuHandle slots[LEGACY_TEXTURE_STAGE_COUNT];
	slots[0] = GpuHandle(8, 2);
	textures.Acknowledge(slots);
	result |= Check(textures.Bind(&textureTrace, slots) == RENDER_RESULT_OK &&
		textureTrace.commands.empty(), "admitted compact draw acknowledges every texture slot");
	textures.Reset();
	result |= Check(textures.Bind(&textureTrace, slots) == RENDER_RESULT_OK &&
		textureTrace.commands.size() == LEGACY_TEXTURE_STAGE_COUNT,
		"reset forgets compact texture acknowledgements");
	NativeW3DSortedBatchBindingCache sorted;
	SortedBatchTraceContext sortedTrace;
	GpuHandle index(12, 3);
	result |= Check(!sorted.IsIndexBufferKnown(index, RENDER_FORMAT_R16_UINT, 0),
		"fresh batch requires the compact index binding");
	sorted.Acknowledge(RENDER_PRIMITIVE_TRIANGLE_LIST, index, RENDER_FORMAT_R16_UINT, 0);
	result |= Check(sorted.IsIndexBufferKnown(index, RENDER_FORMAT_R16_UINT, 0) &&
		!sorted.IsIndexBufferKnown(GpuHandle(12, 4), RENDER_FORMAT_R16_UINT, 0) &&
		!sorted.IsIndexBufferKnown(index, RENDER_FORMAT_R32_UINT, 0) &&
		!sorted.IsIndexBufferKnown(index, RENDER_FORMAT_R16_UINT, 2),
		"compact acknowledgement retains the complete generation and index tuple");
	result |= Check(sorted.BindTopology(&sortedTrace, RENDER_PRIMITIVE_TRIANGLE_LIST) == RENDER_RESULT_OK &&
		sorted.BindIndexBuffer(&sortedTrace, index, RENDER_FORMAT_R16_UINT, 0) == RENDER_RESULT_OK &&
		sortedTrace.commands.empty(), "fallback submissions reuse acknowledged compact bindings");
	sorted.Reset();
	result |= Check(!sorted.IsIndexBufferKnown(index, RENDER_FORMAT_R16_UINT, 0),
		"batch reset forgets the compact index acknowledgement");
	return result;
}

int TestSortedBatchBindingCacheCommandTrace()
{
	using namespace rts::render;
	int result = 0;
	NativeW3DSortedBatchBindingCache cache;
	SortedBatchTraceContext trace;
	const GpuHandle indexA(30, 4);
	const GpuHandle indexANextGeneration(30, 5);

	result |= Check(cache.BindTopology(&trace, RENDER_PRIMITIVE_TRIANGLE_LIST) ==
		RENDER_RESULT_OK && trace.commands.size() == 1 &&
		trace.commands[0].type == SORTED_BATCH_TOPOLOGY_COMMAND,
		"first sorted packet records its topology command");
	result |= Check(cache.BindIndexBuffer(&trace, indexA, RENDER_FORMAT_R16_UINT,
		0) == RENDER_RESULT_OK && trace.commands.size() == 2 &&
		trace.commands[1].type == SORTED_BATCH_INDEX_BUFFER_COMMAND &&
		trace.commands[1].buffer == indexA &&
		trace.commands[1].format == RENDER_FORMAT_R16_UINT &&
		trace.commands[1].offset == 0,
		"first sorted packet records the full index binding tuple");
	result |= Check(cache.BindTopology(&trace, RENDER_PRIMITIVE_TRIANGLE_LIST) ==
		RENDER_RESULT_OK && cache.BindIndexBuffer(&trace, indexA,
		RENDER_FORMAT_R16_UINT, 0) == RENDER_RESULT_OK &&
		trace.commands.size() == 2,
		"repeated topology and exact index bindings emit no commands");

	result |= Check(cache.BindIndexBuffer(&trace, indexANextGeneration,
		RENDER_FORMAT_R16_UINT, 0) == RENDER_RESULT_OK &&
		trace.commands.size() == 3 &&
		trace.commands.back().buffer == indexANextGeneration,
		"recycled index slots with a new generation are rebound");
	result |= Check(cache.BindIndexBuffer(&trace, indexANextGeneration,
		RENDER_FORMAT_R32_UINT, 0) == RENDER_RESULT_OK &&
		trace.commands.size() == 4 &&
		trace.commands.back().format == RENDER_FORMAT_R32_UINT,
		"an index-format change is rebound");
	result |= Check(cache.BindIndexBuffer(&trace, indexANextGeneration,
		RENDER_FORMAT_R32_UINT, 4) == RENDER_RESULT_OK &&
		trace.commands.size() == 5 && trace.commands.back().offset == 4,
		"an index-offset change is rebound");
	result |= Check(cache.BindTopology(&trace, RENDER_PRIMITIVE_LINE_LIST) ==
		RENDER_RESULT_OK && trace.commands.size() == 6 &&
		trace.commands.back().topology == RENDER_PRIMITIVE_LINE_LIST,
		"a topology change is rebound");

	trace.failNextIndexBuffer = true;
	result |= Check(cache.BindIndexBuffer(&trace, indexA, RENDER_FORMAT_R16_UINT,
		0) == RENDER_RESULT_FAILED && trace.commands.size() == 7,
		"a failed index bind is surfaced and not cached");
	result |= Check(cache.BindIndexBuffer(&trace, indexA, RENDER_FORMAT_R16_UINT,
		0) == RENDER_RESULT_OK && trace.commands.size() == 8 &&
		trace.commands.back().buffer == indexA &&
		cache.BindIndexBuffer(&trace, indexA, RENDER_FORMAT_R16_UINT, 0) ==
			RENDER_RESULT_OK && trace.commands.size() == 8,
		"the failed index bind is retried, then the successful tuple is cached");

	trace.failNextTopology = true;
	result |= Check(cache.BindTopology(&trace, RENDER_PRIMITIVE_TRIANGLE_STRIP) ==
		RENDER_RESULT_FAILED && trace.commands.size() == 9,
		"a failed topology bind is surfaced and not cached");
	result |= Check(cache.BindTopology(&trace, RENDER_PRIMITIVE_TRIANGLE_STRIP) ==
		RENDER_RESULT_OK && trace.commands.size() == 10 &&
		cache.BindTopology(&trace, RENDER_PRIMITIVE_TRIANGLE_STRIP) ==
			RENDER_RESULT_OK && trace.commands.size() == 10,
		"the failed topology bind is retried, then the successful value is cached");

	NativeW3DSortedBatchBindingCache nextBatchCache;
	result |= Check(nextBatchCache.BindTopology(&trace,
		RENDER_PRIMITIVE_TRIANGLE_STRIP) == RENDER_RESULT_OK &&
		nextBatchCache.BindIndexBuffer(&trace, indexA, RENDER_FORMAT_R16_UINT,
			0) == RENDER_RESULT_OK && trace.commands.size() == 12,
		"a new sorted batch starts unknown and reissues both bindings");
	std::fprintf(stdout,
		"sorted batch binding trace counts: first=2 repeated=0 tuple-changes=4 "
		"failed-bind-retries=2 batch-reset=2\n");
	return result;
}

#if defined(_WIN32) && defined(RTS_RENDERER_HAS_D3D11)
void CountReadinessCleanup(void *context)
{
	++*static_cast<unsigned int *>(context);
}

struct ForeignReadinessRequest
{
	rts::render::NativeW3DRenderer *renderer;
	rts::render::NativeW3DRenderState *state;
	rts::render::NativeW3DOwnerToken *token;
	bool initialized;
	bool backendOperational;
	rts::render::RenderResult enqueued;
};

DWORD WINAPI ProbeAndEnqueueReadinessCleanup(void *parameter)
{
	ForeignReadinessRequest *request = static_cast<ForeignReadinessRequest *>(parameter);
	request->initialized = request->renderer->IsInitialized();
	request->backendOperational =
		rts::render::NativeW3DRecoveryTestAccess::BackendOperational(*request->renderer);
	request->enqueued = request->state->EnqueueCleanup(
		CountReadinessCleanup, request->token);
	return 0;
}

// Called after the existing native device fixture has finished its draws. It
// owns shutdown of that device and tests readiness against its real transition.
int TestRendererReadinessLifecycle(rts::render::IRenderDevice *device)
{
	using namespace rts::render;
	int result = 0;
	NativeW3DRenderer renderer;
	NativeW3DRenderState *state = NativeW3DRenderState::Create(2);
	if (state == 0) return Check(false, "readiness fixture creates its production state");
	const RenderResult bound = state->BindOwner();
	const RenderResult attached = bound == RENDER_RESULT_OK ?
		NativeW3DRecoveryTestAccess::Attach(renderer, state, device) : bound;
	result |= Check(attached == RENDER_RESULT_OK,
		"readiness fixture attaches the initialized device to the real renderer");
	if (attached != RENDER_RESULT_OK)
	{
		state->BeginShutdown();
		NativeW3DRecoveryTestAccess::Detach(renderer, state);
		state->Release();
		return result;
	}
	result |= Check(renderer.IsInitialized() &&
		NativeW3DRecoveryTestAccess::BackendOperational(renderer),
		"attached owner observes live backend readiness");
	unsigned int callbacks = 0;
	NativeW3DOwnerToken *token = NativeW3DOwnerToken::Create(&callbacks, 0);
	result |= Check(token != 0, "readiness fixture creates an opaque cleanup token");
	if (token != 0)
	{
		ForeignReadinessRequest request = { &renderer, state, token, true, true,
			RENDER_RESULT_FAILED };
		HANDLE worker = CreateThread(0, 0, ProbeAndEnqueueReadinessCleanup, &request, 0, 0);
		result |= Check(worker != 0, "readiness fixture starts its joined foreign producer");
		if (worker != 0)
		{
			WaitForSingleObject(worker, INFINITE);
			CloseHandle(worker);
			result |= Check(!request.initialized && !request.backendOperational &&
				request.enqueued == RENDER_RESULT_OK && state->PendingCleanup() == 1,
				"foreign readiness is rejected while foreign cleanup is accepted");
			result |= Check(renderer.IsInitialized() &&
				NativeW3DRecoveryTestAccess::BackendOperational(renderer) && callbacks == 0,
				"owner readiness neither hides nor executes pending foreign cleanup");
		}
		token->Release();
	}
	device->shutdown();
	result |= Check(!renderer.IsInitialized() &&
		!NativeW3DRecoveryTestAccess::BackendOperational(renderer) && state->IsOwnerThread(),
		"backend unavailability is observed live without losing cleanup ownership");
	result |= Check(state->BeginShutdown() == RENDER_RESULT_OK &&
		!renderer.IsInitialized() && !NativeW3DRecoveryTestAccess::BackendOperational(renderer) &&
		NativeW3DRecoveryTestAccess::Device(*state) == device,
		"closed owner rejects readiness but retains attached device access for cleanup");
	const unsigned int pending = state->PendingCleanup();
	unsigned int drained = 0;
	result |= Check(state->DrainCleanup(0, &drained) == RENDER_RESULT_OK &&
		drained == pending && callbacks == pending && state->PendingCleanup() == 0,
		"closed owner drains exactly the accepted foreign cleanup");
	result |= Check(NativeW3DRecoveryTestAccess::Detach(renderer, state) == RENDER_RESULT_OK &&
		!renderer.IsInitialized() && !NativeW3DRecoveryTestAccess::BackendOperational(renderer),
		"terminal detachment leaves no stale renderer readiness");
	state->Release();
	return result;
}

const wchar_t *kD3D11InputLayoutTestWindowClass =
	L"GeneralsGameCodeD3D11InputLayoutTestWindow";

LRESULT CALLBACK D3D11InputLayoutTestWindowProcedure(HWND window,
	UINT message, WPARAM wparam, LPARAM lparam)
{
	return DefWindowProcW(window, message, wparam, lparam);
}

int TestD3D11TexturedInputLayoutSafety()
{
	using namespace rts::render;
	WNDCLASSEXW windowClass;
	ZeroMemory(&windowClass, sizeof(windowClass));
	windowClass.cbSize = sizeof(windowClass);
	windowClass.lpfnWndProc = D3D11InputLayoutTestWindowProcedure;
	windowClass.hInstance = GetModuleHandleW(0);
	windowClass.lpszClassName = kD3D11InputLayoutTestWindowClass;
	const ATOM classAtom = RegisterClassExW(&windowClass);
	if (classAtom == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
	{
		return Check(false, "D3D11 input-layout test registers its window class");
	}
	HWND window = CreateWindowExW(0, kD3D11InputLayoutTestWindowClass,
		L"D3D11 input-layout safety", WS_OVERLAPPED, 0, 0, 64, 64, 0, 0,
		windowClass.hInstance, 0);
	if (window == 0)
	{
		return Check(false, "D3D11 input-layout test creates a hidden window");
	}

	IRenderDevice *device = CreateD3D11RenderDevice();
	int result = Check(device != 0,
		"D3D11 input-layout test creates a native device");
	if (device == 0)
	{
		DestroyWindow(window);
		return result;
	}
	RenderDeviceParameters parameters;
	parameters.backend = RENDER_BACKEND_D3D11;
	parameters.window = window;
	parameters.width = 64;
	parameters.height = 64;
	parameters.enableVsync = false;
	parameters.allowSoftwareFallback = true;
	const RenderResult initializeResult = device->initialize(parameters);
	result |= Check(initializeResult == RENDER_RESULT_OK,
		"D3D11 input-layout test initializes its native device");
	if (initializeResult != RENDER_RESULT_OK)
	{
		device->shutdown();
		delete device;
		DestroyWindow(window);
		return result;
	}

	struct XYZVertex
	{
		float x;
		float y;
		float z;
	};
	const XYZVertex xyzVertices[3] = {
		{ -0.5f, -0.5f, 0.0f },
		{ 0.0f, 0.5f, 0.0f },
		{ 0.5f, -0.5f, 0.0f }
	};
	BufferDescriptor xyzDescriptor;
	xyzDescriptor.byteCount = sizeof(xyzVertices);
	xyzDescriptor.stride = sizeof(XYZVertex);
	xyzDescriptor.binding = RENDER_BUFFER_VERTEX;
	xyzDescriptor.usage = RENDER_USAGE_IMMUTABLE;
	GpuHandle xyzBuffer;
	result |= Check(device->createBuffer(xyzDescriptor, xyzVertices,
		sizeof(xyzVertices), &xyzBuffer) == RENDER_RESULT_OK,
		"plain XYZ input fixture creates a 12-byte vertex stream");

	LegacyVertexLayout xyzLayout;
	xyzLayout.stride = sizeof(XYZVertex);
	xyzLayout.elementCount = 1;
	xyzLayout.elements[0].semantic = RENDER_VERTEX_SEMANTIC_POSITION;
	xyzLayout.elements[0].semanticIndex = 0;
	xyzLayout.elements[0].format = RENDER_VERTEX_DATA_FLOAT3;
	xyzLayout.elements[0].byteOffset = 0;
	LegacyLogicalState state;
	IRenderContext *context = device->immediateContext();
	bool frameStarted = context != 0 &&
		context->beginFrame() == RENDER_RESULT_OK;
	result |= Check(frameStarted,
		"plain XYZ input fixture begins a D3D11 frame");
	if (frameStarted)
	{
		const RenderResult layoutResult = context->setLegacyStateForLayout(
			state, xyzLayout, 0);
		result |= Check(layoutResult == RENDER_RESULT_OK,
			"plain XYZ selects the unweighted shader and bounded layout");
		result |= Check(context->setVertexBuffer(xyzBuffer,
			sizeof(XYZVertex), 0) == RENDER_RESULT_OK,
			"plain XYZ binds its exact 12-byte source stride");
		result |= Check(context->setPrimitiveTopology(
			RENDER_PRIMITIVE_TRIANGLE_LIST) == RENDER_RESULT_OK &&
			context->draw(3, 0) == RENDER_RESULT_OK,
			"plain XYZ draws without a synthesized 16-byte read");
		result |= Check(context->endFrame() == RENDER_RESULT_OK,
			"plain XYZ input fixture ends its D3D11 frame");
	}

	struct WeightedVertex
	{
		float x;
		float y;
		float z;
		float weight0[4];
	};
	const WeightedVertex weightedVertices[3] = {
		{ -0.5f, -0.5f, 0.0f, { 1.0f, 0.0f, 0.0f, 0.0f } },
		{ 0.0f, 0.5f, 0.0f, { 1.0f, 0.0f, 0.0f, 0.0f } },
		{ 0.5f, -0.5f, 0.0f, { 1.0f, 0.0f, 0.0f, 0.0f } }
	};
	BufferDescriptor weightedDescriptor;
	weightedDescriptor.byteCount = sizeof(weightedVertices);
	weightedDescriptor.stride = sizeof(WeightedVertex);
	weightedDescriptor.binding = RENDER_BUFFER_VERTEX;
	weightedDescriptor.usage = RENDER_USAGE_IMMUTABLE;
	GpuHandle weightedBuffer;
	result |= Check(device->createBuffer(weightedDescriptor, weightedVertices,
		sizeof(weightedVertices), &weightedBuffer) == RENDER_RESULT_OK,
		"weighted input fixture creates its source stream");
	LegacyVertexLayout weightedLayout = xyzLayout;
	weightedLayout.stride = sizeof(WeightedVertex);
	weightedLayout.elementCount = 2;
	weightedLayout.elements[1].semantic = RENDER_VERTEX_SEMANTIC_BLEND_WEIGHT;
	weightedLayout.elements[1].semanticIndex = 0;
	weightedLayout.elements[1].format = RENDER_VERTEX_DATA_FLOAT4;
	weightedLayout.elements[1].byteOffset = 12;
	frameStarted = context->beginFrame() == RENDER_RESULT_OK;
	result |= Check(frameStarted,
		"weighted input fixture begins a D3D11 frame");
	if (frameStarted)
	{
		const RenderResult layoutResult = context->setLegacyStateForLayout(
			state, weightedLayout, 0);
		result |= Check(layoutResult == RENDER_RESULT_OK,
			"weighted XYZ selects the weighted shader and layout");
		result |= Check(context->setVertexBuffer(weightedBuffer,
			sizeof(WeightedVertex), 0) == RENDER_RESULT_OK &&
			context->setPrimitiveTopology(
				RENDER_PRIMITIVE_TRIANGLE_LIST) == RENDER_RESULT_OK &&
			context->draw(3, 0) == RENDER_RESULT_OK,
			"weighted XYZ draws through its explicit blend declaration");
		result |= Check(context->endFrame() == RENDER_RESULT_OK,
			"weighted input fixture ends its D3D11 frame");
	}

	LegacyVertexLayout shortLayout = xyzLayout;
	shortLayout.stride = 8;
	frameStarted = context->beginFrame() == RENDER_RESULT_OK;
	result |= Check(frameStarted,
		"short XYZ input fixture begins a D3D11 frame");
	if (frameStarted)
	{
		result |= Check(context->setLegacyStateForLayout(state, shortLayout, 0) ==
			RENDER_RESULT_INVALID_ARGUMENT,
			"short XYZ rejects a position declaration outside its source stride");
		result |= Check(context->endFrame() == RENDER_RESULT_OK,
			"short XYZ input fixture ends its D3D11 frame");
	}

	if (xyzBuffer.isValid())
	{
		result |= Check(device->destroyResource(xyzBuffer),
			"plain XYZ input fixture releases its buffer");
	}
	if (weightedBuffer.isValid())
	{
		result |= Check(device->destroyResource(weightedBuffer),
			"weighted input fixture releases its buffer");
	}
	result |= TestRendererReadinessLifecycle(device);
	device->shutdown();
	delete device;
	DestroyWindow(window);
	return result;
}
#endif
}

int main()
{
	int result = 0;
	result |= TestInstancingAdmission();
	result |= TestTexturePublicationContract();
	result |= TestTexturePublicationOperationalContract();
	result |= TestTextureBindingCacheCommandTrace();
	result |= TestSortedBatchBindingCacheCommandTrace();
	result |= TestCompactRecordBindingAcknowledgements();
#if defined(_WIN32) && defined(RTS_RENDERER_HAS_D3D11)
	result |= TestD3D11TexturedInputLayoutSafety();
#endif
	rts::render::NativeW3DRenderer renderer;
	rts::render::NativeW3DResources resources;
	rts::render::NativeW3DRendererDescriptor descriptor;
	rts::render::NativeDrawPacket packet;
	rts::render::LegacyLogicalState state;
	rts::render::RenderViewport viewport(0.0f, 0.0f, 640.0f, 480.0f,
		0.0f, 1.0f);
	rts::render::RenderVertexLayout layout;
	rts::render::RenderMatrix4 matrix;
	result |= Check(layout.elements[0].format ==
		rts::render::RENDER_VERTEX_DATA_FLOAT3,
		"neutral position elements preserve the legacy FLOAT3 default");

	layout.stride = 16;
	layout.elementCount = 1;
	layout.elements[0].semantic = rts::render::RENDER_VERTEX_SEMANTIC_POSITION;
	layout.elements[0].format = rts::render::RENDER_VERTEX_DATA_FLOAT3;
	result |= Check(layout.elements[0].semantic ==
		rts::render::RENDER_VERTEX_SEMANTIC_POSITION &&
		viewport.width == 640.0f && viewport.maximumDepth == 1.0f &&
		matrix.values[0] == 1.0f && matrix.values[15] == 1.0f &&
		packet.indexFormat == rts::render::RENDER_FORMAT_R16_UINT &&
		packet.topology == rts::render::RENDER_PRIMITIVE_TRIANGLE_LIST,
		"native vocabulary has stable neutral defaults");

	result |= Check(renderer.BeginFrame() == rts::render::RENDER_RESULT_INVALID_ARGUMENT,
		"cannot begin a native frame before initialization");
	result |= Check(renderer.SetViewport(viewport) ==
		rts::render::RENDER_RESULT_INVALID_ARGUMENT,
		"cannot set a native viewport before initialization");
	result |= Check(renderer.EndFrame(false) == rts::render::RENDER_RESULT_INVALID_ARGUMENT,
		"cannot end a native frame before initialization");
	result |= Check(renderer.Submit(resources, state, packet) == rts::render::RENDER_RESULT_INVALID_ARGUMENT,
		"cannot submit a native draw before initialization");
	descriptor.width = 640;
	descriptor.height = 480;
	result |= Check(renderer.Initialize(0, descriptor) == rts::render::RENDER_RESULT_INVALID_ARGUMENT,
		"a native facade rejects a null window without creating a legacy device");
	result |= Check(!renderer.IsInitialized() && !renderer.IsFrameOpen(),
		"a failed initialization leaves no native renderer state behind");
	result |= Check(renderer.RecoverDevice() == rts::render::RENDER_RESULT_INVALID_ARGUMENT &&
		renderer.Resize(640, 480) == rts::render::RENDER_RESULT_INVALID_ARGUMENT,
		"recovery and resize reject an uninitialized native facade");
	result |= Check(renderer.Shutdown() == rts::render::RENDER_RESULT_OK,
		"shutdown is idempotent before native initialization");
	result |= Check(resources.Bind(0) == rts::render::RENDER_RESULT_INVALID_ARGUMENT &&
		!resources.Destroy(packet.vertexBuffer),
		"native resource tables reject an unbound renderer and stale handles");
	return result;
}
