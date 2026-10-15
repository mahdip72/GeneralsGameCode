#include "Renderer/GameRigidDrawBatch.h"
#include "Renderer/LegacyFvfLayout.h"

#include <stdio.h>
#include <vector>

using namespace rts::render;

namespace {
int failures = 0;
#define CHECK(x) do { if (!(x)) { ++failures; fprintf(stderr, "line %d: %s\n", __LINE__, #x); } } while (0)

struct Event
{
	unsigned int geometry;
	unsigned int count;
	bool instanced;
	float ambient;
	float material;
	float worlds[RENDER_RIGID_INSTANCE_MAX];
};

class RecordingSink : public GameRigidDrawSink
{
public:
	RecordingSink() : instanceResult(RENDER_RESULT_OK),
		ordinaryResult(RENDER_RESULT_OK), instanceCalls(0), ordinaryCalls(0) {}
	virtual RenderResult Ordinary(const LegacyLogicalState &state,
		const NativeDrawPacket &packet)
	{
		++ordinaryCalls;
		Record(state, packet, &state.constants.world, 1, false);
		return ordinaryResult;
	}
	virtual RenderResult Instanced(const LegacyLogicalState &state,
		const NativeDrawPacket &packet, const RenderMatrix4 *worlds,
		unsigned int count)
	{
		++instanceCalls;
		if (instanceResult != RENDER_RESULT_UNSUPPORTED)
			Record(state, packet, worlds, count, true);
		return instanceResult;
	}
	void Record(const LegacyLogicalState &state, const NativeDrawPacket &packet,
		const RenderMatrix4 *worlds, unsigned int count, bool instanced)
	{
		Event event;
		event.geometry = packet.vertexBuffer.index();
		event.count = count;
		event.instanced = instanced;
		event.ambient = state.constants.globalAmbient.x;
		event.material = state.constants.material.diffuse.x;
		for (unsigned int i = 0; i < count; ++i)
			event.worlds[i] = worlds[i].values[12];
		events.push_back(event);
	}
	RenderResult instanceResult;
	RenderResult ordinaryResult;
	unsigned int instanceCalls;
	unsigned int ordinaryCalls;
	std::vector<Event> events;
};

NativeDrawPacket Packet(unsigned int id)
{
	NativeDrawPacket p;
	p.vertexBuffer = GpuHandle(id, 1);
	p.indexBuffer = GpuHandle(100, 1);
	p.indexed = true;
	p.vertexCount = 3;
	p.indexCount = 3;
	p.vertexStride = 36;
	p.vertexFormat = RENDER_VERTEX_POSITION3_NORMAL_COLOR_TEX1;
	p.topology = RENDER_PRIMITIVE_TRIANGLE_LIST;
	p.vertexLayout.stride = 36;
	p.vertexLayout.elementCount = 4;
	const RenderVertexSemantic semantic[4] = { RENDER_VERTEX_SEMANTIC_POSITION,
		RENDER_VERTEX_SEMANTIC_NORMAL, RENDER_VERTEX_SEMANTIC_DIFFUSE,
		RENDER_VERTEX_SEMANTIC_TEXTURE_COORDINATE };
	const RenderVertexDataFormat format[4] = { RENDER_VERTEX_DATA_FLOAT3,
		RENDER_VERTEX_DATA_FLOAT3, RENDER_VERTEX_DATA_COLOR_BGRA8,
		RENDER_VERTEX_DATA_FLOAT2 };
	const unsigned int offset[4] = { 0, 12, 24, 28 };
	for (unsigned int i = 0; i < 4; ++i)
	{
		p.vertexLayout.elements[i].semantic = semantic[i];
		p.vertexLayout.elements[i].semanticIndex = 0;
		p.vertexLayout.elements[i].format = format[i];
		p.vertexLayout.elements[i].byteOffset = offset[i];
	}
	return p;
}

LegacyLogicalState State(float x)
{
	LegacyLogicalState s;
	s.constants.world.values[12] = x;
	s.constants.globalAmbient.x = 0.25f;
	s.constants.material.diffuse.x = 0.75f;
	return s;
}

GameRigidCaptureResult Capture(GameRigidDrawBatch &batch, RecordingSink &sink,
	const NativeDrawPacket &packet, const LegacyLogicalState &state,
	uint64_t epoch = 1, uint64_t geometry = 1, uint64_t category = 1,
	bool enabled = true)
{
	return batch.Capture(sink, packet, state, epoch, geometry, category, enabled);
}

NativeDrawPacket Packet32(unsigned int id)
{
	NativeDrawPacket packet = Packet(id);
	const unsigned int fvf = LEGACY_FVF_XYZ | LEGACY_FVF_NORMAL | (1U << LEGACY_FVF_TEXCOUNT_SHIFT);
	packet.vertexStride = LegacyFvfVertexSize(fvf);
	CHECK(packet.vertexStride == 32);
	CHECK(DecodeLegacyFvfVertexLayout(fvf, packet.vertexStride, &packet.vertexLayout));
	CHECK(packet.vertexLayout.elementCount == 3 &&
		packet.vertexLayout.elements[2].byteOffset == 24);
	return packet;
}

void Exact32AdjacentBarrierOrderAndCapacity()
{
	GameRigidDrawBatch batch;
	RecordingSink sink;
	const NativeDrawPacket a = Packet32(1), b = Packet32(2);
	CHECK(IsRigidInstancingLayout(a.vertexLayout));
	CHECK(Capture(batch, sink, a, State(10)) == GAME_RIGID_PENDING_OWNED);
	CHECK(Capture(batch, sink, a, State(20)) == GAME_RIGID_PENDING_OWNED);
	CHECK(batch.Flush(sink) == RENDER_RESULT_OK);
	CHECK(sink.events.size() == 1 && sink.events[0].instanced && sink.events[0].count == 2);
	CHECK(sink.events[0].worlds[0] == 10 && sink.events[0].worlds[1] == 20);
	// Real producer declaration, explicit barrier, then A,B,A in encounter order.
	CHECK(Capture(batch, sink, a, State(30), 2) == GAME_RIGID_PENDING_OWNED);
	CHECK(Capture(batch, sink, b, State(40), 2) == GAME_RIGID_PENDING_OWNED);
	CHECK(Capture(batch, sink, a, State(50), 2) == GAME_RIGID_PENDING_OWNED);
	CHECK(batch.Flush(sink) == RENDER_RESULT_OK);
	CHECK(sink.events.size() == 4 && sink.instanceCalls == 1);
	CHECK(sink.events[1].geometry == 1 && sink.events[2].geometry == 2 && sink.events[3].geometry == 1);
	for (unsigned int i = 0; i < 33; ++i)
		CHECK(Capture(batch, sink, a, State(static_cast<float>(i)), 3) == GAME_RIGID_PENDING_OWNED);
	CHECK(batch.Count() == 1 && sink.events.back().count == RENDER_RIGID_INSTANCE_MAX);
	CHECK(batch.Flush(sink) == RENDER_RESULT_OK && sink.events.back().worlds[0] == 32);
	CHECK(batch.Diagnostics().accepted[0] == 38 && batch.Diagnostics().accepted[1] == 0);
	CHECK(batch.Diagnostics().declarations[0] == 38 && batch.Metrics().rejectedDraws == 0);
}

void AdmissionReasonCountsAndSaturation()
{
	GameRigidDrawBatch batch;
	RecordingSink sink;
	NativeDrawPacket packet = Packet32(1);
	LegacyLogicalState state = State(1);
	CHECK(Capture(batch, sink, packet, state, 1, 0) == GAME_RIGID_ORDINARY_REQUIRED);
	packet.indexed = false;
	CHECK(Capture(batch, sink, packet, state) == GAME_RIGID_ORDINARY_REQUIRED);
	packet = Packet32(1); packet.vertexStride = 40;
	CHECK(Capture(batch, sink, packet, state) == GAME_RIGID_ORDINARY_REQUIRED);
	packet = Packet32(1); packet.vertexLayout.elements[2].byteOffset = 28;
	CHECK(Capture(batch, sink, packet, state) == GAME_RIGID_ORDINARY_REQUIRED);
	packet = Packet32(1); packet.topology = RENDER_PRIMITIVE_TRIANGLE_STRIP;
	CHECK(Capture(batch, sink, packet, state) == GAME_RIGID_ORDINARY_REQUIRED);
	packet = Packet32(1);
	state.pipeline.vertexProgram = RENDER_LEGACY_VERTEX_TREES;
	CHECK(Capture(batch, sink, packet, state) == GAME_RIGID_ORDINARY_REQUIRED);
	state = State(1); state.pipeline.pixelProgram = RENDER_LEGACY_PIXEL_WATER_FLAT;
	CHECK(Capture(batch, sink, packet, state) == GAME_RIGID_ORDINARY_REQUIRED);
	state = State(1); state.pipeline.blend.blendEnable = true;
	CHECK(Capture(batch, sink, packet, state) == GAME_RIGID_ORDINARY_REQUIRED);
	state = State(1); state.pipeline.nPatchEnable = true;
	CHECK(Capture(batch, sink, packet, state) == GAME_RIGID_ORDINARY_REQUIRED);
	state = State(1); state.pipeline.alphaTestEnable = true;
	CHECK(Capture(batch, sink, packet, state) == GAME_RIGID_ORDINARY_REQUIRED);
	state = State(1); state.constants.world.values[0] = 0;
	CHECK(Capture(batch, sink, packet, state) == GAME_RIGID_ORDINARY_REQUIRED);
	// Multiple bad gates classify once at the first group, disabled hints count nothing.
	packet.indexed = false; packet.vertexStride = 40;
	CHECK(Capture(batch, sink, packet, state, 1, 0) == GAME_RIGID_ORDINARY_REQUIRED);
	CHECK(Capture(batch, sink, packet, state, 1, 0, 0, false) == GAME_RIGID_ORDINARY_REQUIRED);
	const GameRigidDrawBatch::AdmissionDiagnostics &d = batch.Diagnostics();
	CHECK(d.rejected[0] == 2);
	CHECK(d.rejected[1] == 1 && d.rejected[2] == 2 && d.rejected[3] == 1);
	CHECK(d.rejected[4] == 2 && d.rejected[5] == 3 && d.rejected[6] == 1);
	CHECK(batch.Metrics().rejectedDraws == 12 && batch.Metrics().capturedDraws == 0);
	CHECK(d.declarations[0] == 10 && d.declarations[1] == 0 && d.declarations[2] == 2);
	CHECK(sink.events.empty());
	CHECK(Capture(batch, sink, Packet(1), State(2)) == GAME_RIGID_PENDING_OWNED);
	CHECK(batch.Diagnostics().accepted[1] == 1 && batch.Diagnostics().declarations[1] == 1);
	uint64_t value = ~static_cast<uint64_t>(0) - 1;
	GameRigidDrawBatch::AdmissionDiagnostics::Increment(value);
	CHECK(value == ~static_cast<uint64_t>(0));
	GameRigidDrawBatch::AdmissionDiagnostics::Increment(value);
	CHECK(value == ~static_cast<uint64_t>(0));
	CHECK(batch.Flush(sink) == RENDER_RESULT_OK);
}

void OwnedAdjacentAndSingleton()
{
	GameRigidDrawBatch batch;
	RecordingSink sink;
	NativeDrawPacket packet = Packet(1);
	LegacyLogicalState state = State(10);
	CHECK(Capture(batch, sink, packet, state) == GAME_RIGID_PENDING_OWNED);
	state.constants.world.values[12] = 20;
	CHECK(Capture(batch, sink, packet, state) == GAME_RIGID_PENDING_OWNED);
	CHECK(sink.events.empty());
	packet = Packet(99);
	state = State(99);
	state.constants.globalAmbient.x = 99;
	CHECK(batch.Flush(sink) == RENDER_RESULT_OK);
	CHECK(sink.events.size() == 1 && sink.instanceCalls == 1 && sink.ordinaryCalls == 0);
	CHECK(sink.events[0].geometry == 1 && sink.events[0].count == 2);
	CHECK(sink.events[0].worlds[0] == 10 && sink.events[0].worlds[1] == 20);
	CHECK(sink.events[0].ambient == 0.25f && sink.events[0].material == 0.75f);
	CHECK(batch.Metrics().capturedDraws == 2 && batch.Metrics().instancedBatches == 1);
	CHECK(batch.Metrics().instancedInstances == 2);
	CHECK(batch.Flush(sink) == RENDER_RESULT_OK && sink.events.size() == 1);
	CHECK(Capture(batch, sink, Packet(2), State(30)) == GAME_RIGID_PENDING_OWNED);
	CHECK(batch.Flush(sink) == RENDER_RESULT_OK);
	CHECK(sink.ordinaryCalls == 1 && sink.events[1].geometry == 2);
	CHECK(batch.Metrics().singletonOrdinary == 1);
}

void EncounterOrderAndResolvedLight()
{
	GameRigidDrawBatch batch;
	RecordingSink sink;
	CHECK(Capture(batch, sink, Packet(1), State(1)) == GAME_RIGID_PENDING_OWNED);
	CHECK(Capture(batch, sink, Packet(2), State(2)) == GAME_RIGID_PENDING_OWNED);
	CHECK(Capture(batch, sink, Packet(1), State(3)) == GAME_RIGID_PENDING_OWNED);
	CHECK(batch.Flush(sink) == RENDER_RESULT_OK);
	CHECK(sink.events.size() == 3 && sink.instanceCalls == 0);
	CHECK(sink.events[0].geometry == 1 && sink.events[1].geometry == 2 && sink.events[2].geometry == 1);
	LegacyLogicalState lightA = State(4), lightB = State(5);
	lightB.constants.lights[0].enabled = true;
	lightB.constants.lights[0].diffuse.x = 0.8f;
	CHECK(Capture(batch, sink, Packet(3), lightA) == GAME_RIGID_PENDING_OWNED);
	CHECK(Capture(batch, sink, Packet(3), lightB) == GAME_RIGID_PENDING_OWNED);
	// A null light environment leaves the actual tracked light values intact.
	// Producer supplies that resolved snapshot, not a null/default light key.
	lightB.constants.world.values[12] = 6;
	CHECK(Capture(batch, sink, Packet(3), lightB) == GAME_RIGID_PENDING_OWNED);
	CHECK(batch.Flush(sink) == RENDER_RESULT_OK);
	CHECK(sink.events.size() == 5 && !sink.events[3].instanced && sink.events[4].count == 2);
}

void CapacityAndBarrier()
{
	GameRigidDrawBatch batch;
	RecordingSink sink;
	for (unsigned int i = 0; i < 33; ++i)
		CHECK(Capture(batch, sink, Packet(1), State(static_cast<float>(i))) == GAME_RIGID_PENDING_OWNED);
	CHECK(sink.events.size() == 1 && sink.events[0].count == 32 && batch.Count() == 1);
	CHECK(batch.Flush(sink) == RENDER_RESULT_OK && sink.events.size() == 2);
	CHECK(sink.events[1].worlds[0] == 32);
	// Target/resource/pass mutation is permitted only after this flush returns.
	const unsigned int prior = static_cast<unsigned int>(sink.events.size());
	CHECK(Capture(batch, sink, Packet(2), State(40)) == GAME_RIGID_PENDING_OWNED);
	CHECK(batch.Flush(sink) == RENDER_RESULT_OK && sink.events.size() == prior + 1);
	CHECK(Capture(batch, sink, Packet(2), State(41), 2) == GAME_RIGID_PENDING_OWNED);
	CHECK(Capture(batch, sink, Packet(2), State(42), 3) == GAME_RIGID_PENDING_OWNED);
	CHECK(batch.Flush(sink) == RENDER_RESULT_OK && sink.events.size() == prior + 3);
}

void FallbackAndAcceptedFailure()
{
	GameRigidDrawBatch batch;
	RecordingSink sink;
	sink.instanceResult = RENDER_RESULT_UNSUPPORTED;
	Capture(batch, sink, Packet(1), State(10));
	Capture(batch, sink, Packet(1), State(20));
	CHECK(batch.Flush(sink) == RENDER_RESULT_OK);
	CHECK(sink.instanceCalls == 1 && sink.ordinaryCalls == 2 && sink.events.size() == 2);
	CHECK(sink.events[0].worlds[0] == 10 && sink.events[1].worlds[0] == 20);
	CHECK(batch.Metrics().unsupportedFallbacks == 1 && batch.Metrics().ordinaryFallbackDraws == 2);
	sink.instanceResult = RENDER_RESULT_FAILED;
	Capture(batch, sink, Packet(1), State(30));
	Capture(batch, sink, Packet(1), State(40));
	CHECK(Capture(batch, sink, Packet(2), State(50)) == GAME_RIGID_FAILED);
	CHECK(batch.Count() == 0 && sink.instanceCalls == 2 && sink.ordinaryCalls == 2);
	CHECK(batch.Flush(sink) == RENDER_RESULT_FAILED && sink.instanceCalls == 2);
	CHECK(Capture(batch, sink, Packet(3), State(60)) == GAME_RIGID_FAILED);
}

void RejectionAndIdentityBoundaries()
{
	GameRigidDrawBatch batch;
	RecordingSink sink;
	Capture(batch, sink, Packet(1), State(1));
	LegacyLogicalState alpha = State(2);
	alpha.pipeline.alphaTestEnable = true;
	CHECK(Capture(batch, sink, Packet(1), alpha) == GAME_RIGID_ORDINARY_REQUIRED);
	CHECK(sink.ordinaryCalls == 1 && sink.events[0].worlds[0] == 1);
	// Current rejected draw belongs to the original caller exactly once.
	CHECK(sink.Ordinary(alpha, Packet(1)) == RENDER_RESULT_OK);
	NativeDrawPacket skinned = Packet(1);
	skinned.vertexLayout.elements[0].semantic = RENDER_VERTEX_SEMANTIC_BLEND_WEIGHT;
	CHECK(Capture(batch, sink, skinned, State(3)) == GAME_RIGID_ORDINARY_REQUIRED);
	CHECK(Capture(batch, sink, Packet(1), State(4), 1, 1, 1, false) == GAME_RIGID_ORDINARY_REQUIRED);
	LegacyLogicalState singular = State(5);
	singular.constants.world.values[0] = 0;
	CHECK(Capture(batch, sink, Packet(1), singular) == GAME_RIGID_ORDINARY_REQUIRED);
	Capture(batch, sink, Packet(1), State(6), 1, 1, 1);
	Capture(batch, sink, Packet(1), State(7), 1, 2, 1);
	Capture(batch, sink, Packet(1), State(8), 1, 2, 2);
	CHECK(batch.Flush(sink) == RENDER_RESULT_OK && sink.instanceCalls == 0);
}

class ReentrantSink : public RecordingSink
{
public:
	explicit ReentrantSink(GameRigidDrawBatch &batch) : m_batch(batch) {}
	virtual RenderResult Instanced(const LegacyLogicalState &state,
		const NativeDrawPacket &packet, const RenderMatrix4 *worlds, unsigned int count)
	{
		CHECK(Capture(m_batch, *this, Packet(99), State(99)) == GAME_RIGID_FAILED);
		// The failed reentry cannot replace the prefix currently being emitted.
		CHECK(worlds[0].values[12] == 10 && worlds[1].values[12] == 20);
		return RecordingSink::Instanced(state, packet, worlds, count);
	}
private:
	GameRigidDrawBatch &m_batch;
};

void ReentrantCaptureDoesNotOverwrite()
{
	GameRigidDrawBatch batch;
	ReentrantSink sink(batch);
	Capture(batch, sink, Packet(1), State(10));
	Capture(batch, sink, Packet(1), State(20));
	CHECK(batch.Flush(sink) == RENDER_RESULT_FAILED);
	CHECK(batch.Count() == 0 && sink.ordinaryCalls == 0 && sink.instanceCalls == 1);
	CHECK(batch.Metrics().capturedDraws == 2);
	CHECK(batch.Flush(sink) == RENDER_RESULT_FAILED && sink.instanceCalls == 1);
}
} // namespace

int main()
{
	Exact32AdjacentBarrierOrderAndCapacity();
	AdmissionReasonCountsAndSaturation();
	OwnedAdjacentAndSingleton();
	EncounterOrderAndResolvedLight();
	CapacityAndBarrier();
	FallbackAndAcceptedFailure();
	RejectionAndIdentityBoundaries();
	ReentrantCaptureDoesNotOverwrite();
	if (failures) return 1;
	puts("GameRigidDrawBatch contract passed");
	return 0;
}
