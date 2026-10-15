#ifndef RTS_RENDERER_GAMERIGIDDRAWBATCH_H
#define RTS_RENDERER_GAMERIGIDDRAWBATCH_H

#include "Renderer/RigidInstancingPolicy.h"
#include "Renderer/RenderGameClientNative.h"

namespace rts { namespace render {

// Synchronous adapter: no sink, owner, model, or caller pointer is retained.
class GameRigidDrawSink
{
public:
	virtual ~GameRigidDrawSink() {}
	virtual RenderResult Ordinary(const LegacyLogicalState &state,
		const NativeDrawPacket &packet) = 0;
	// UNSUPPORTED means clean rejection before any command admission.
	virtual RenderResult Instanced(const LegacyLogicalState &state,
		const NativeDrawPacket &packet, const RenderMatrix4 *worlds,
		unsigned int count) = 0;
};

enum GameRigidCaptureResult
{
	GAME_RIGID_ORDINARY_REQUIRED,
	GAME_RIGID_PENDING_OWNED,
	GAME_RIGID_FAILED
};

// Original traversal order is the only order. The pending prefix owns a full
// ordinary snapshot and each world, including the inherited resolved lights.
class GameRigidDrawBatch
{
public:
	// Private publisher data, separate from the seven-field gameplay metrics.
	struct AdmissionDiagnostics
	{
		uint64_t rejected[7];
		uint64_t declarations[3]; // enabled stride32 / stride36 / other
		uint64_t accepted[2];
		AdmissionDiagnostics()
		{
			for (unsigned int i = 0; i < 7; ++i) rejected[i] = 0;
			for (unsigned int i = 0; i < 3; ++i) declarations[i] = 0;
			accepted[0] = accepted[1] = 0;
		}
		static void Increment(uint64_t &value)
		{
			if (value != ~static_cast<uint64_t>(0)) ++value;
		}
	};
	const AdmissionDiagnostics &Diagnostics() const { return m_diagnostics; }

	GameRigidDrawBatch() : m_count(0), m_epoch(0), m_geometry(0),
		m_category(0), m_failure(RENDER_RESULT_OK), m_emitting(false) {}

	unsigned int Count() const { return m_count; }
	RenderResult Failure() const { return m_failure; }
	bool Emitting() const { return m_emitting; }
	const GameRigidDrawMetrics &Metrics() const { return m_metrics; }

	GameRigidCaptureResult Capture(GameRigidDrawSink &sink,
		const NativeDrawPacket &packet, const LegacyLogicalState &state,
		uint64_t epoch, uint64_t geometry, uint64_t category,
		bool enabled)
	{
		if (m_emitting)
		{
			m_failure = RENDER_RESULT_FAILED;
			return GAME_RIGID_FAILED;
		}
		if (m_failure != RENDER_RESULT_OK) return GAME_RIGID_FAILED;
		if (enabled) AdmissionDiagnostics::Increment(m_diagnostics.declarations[
			packet.vertexStride == 32 ? 0 : packet.vertexStride == 36 ? 1 : 2]);
		const bool eligible = enabled && geometry != 0 && category != 0 &&
			RigidInstancingDrawEligible(packet, state);
		if (!eligible || (m_count != 0 &&
			(m_count == RENDER_RIGID_INSTANCE_MAX || m_geometry != geometry ||
			 m_category != category || !RigidInstancingCompatible(m_packet,
			 m_state, m_epoch, packet, state, epoch))))
		{
			if (Flush(sink) != RENDER_RESULT_OK) return GAME_RIGID_FAILED;
		}
		if (!eligible)
		{
			if (enabled)
			{
				++m_metrics.rejectedDraws;
				RecordRejection(packet, state, geometry, category);
			}
			return GAME_RIGID_ORDINARY_REQUIRED;
		}
		if (m_count == 0)
		{
			m_packet = packet;
			m_state = state;
			m_epoch = epoch;
			m_geometry = geometry;
			m_category = category;
		}
		m_worlds[m_count++] = state.constants.world;
		++m_metrics.capturedDraws;
		AdmissionDiagnostics::Increment(m_diagnostics.accepted[
			packet.vertexStride == 32 ? 0 : 1]);
		return GAME_RIGID_PENDING_OWNED;
	}

	RenderResult Flush(GameRigidDrawSink &sink)
	{
		if (m_emitting) return RENDER_RESULT_OK;
		if (m_failure != RENDER_RESULT_OK) return m_failure;
		const unsigned int count = m_count;
		m_count = 0; // Never replay after admission, failure, or recursive entry.
		if (count == 0) return RENDER_RESULT_OK;
		m_emitting = true;
		RenderResult result = RENDER_RESULT_UNSUPPORTED;
		try
		{
			if (count > 1)
			{
				result = sink.Instanced(m_state, m_packet, m_worlds, count);
				if (result == RENDER_RESULT_OK)
				{
					++m_metrics.instancedBatches;
					m_metrics.instancedInstances += count;
				}
				else if (result == RENDER_RESULT_UNSUPPORTED) ++m_metrics.unsupportedFallbacks;
			}
			else ++m_metrics.singletonOrdinary;
			if (m_failure != RENDER_RESULT_OK) result = m_failure;
			if (result == RENDER_RESULT_UNSUPPORTED)
			{
				for (unsigned int i = 0; i < count; ++i)
				{
					LegacyLogicalState state = m_state;
					state.constants.world = m_worlds[i];
					result = sink.Ordinary(state, m_packet);
					if (count > 1) ++m_metrics.ordinaryFallbackDraws;
					if (m_failure != RENDER_RESULT_OK) result = m_failure;
					if (result != RENDER_RESULT_OK) break;
				}
			}
		}
		catch (...) { result = RENDER_RESULT_FAILED; }
		m_emitting = false;
		if (result != RENDER_RESULT_OK) m_failure = result;
		return result;
	}

	// Device/frame failure has already cancelled admission. Do not manufacture
	// ordinary output against a lost target or a replacement resource table.
	void Abandon() { m_count = 0; m_failure = RENDER_RESULT_OK; }

private:
	void RecordRejection(const NativeDrawPacket &packet,
		const LegacyLogicalState &state, uint64_t geometry, uint64_t category)
	{
		unsigned int reason = 6; // world is the last eligibility gate
		if (geometry == 0 || category == 0) reason = 0;
		else if (!packet.indexed || packet.indexCount == 0 || packet.vertexCount == 0) reason = 1;
		else if ((packet.vertexStride != 32 && packet.vertexStride != 36) ||
			packet.vertexStride != packet.vertexLayout.stride ||
			packet.vertexFormat != RENDER_VERTEX_POSITION3_NORMAL_COLOR_TEX1 ||
			!IsRigidInstancingLayout(packet.vertexLayout)) reason = 2;
		else if (packet.topology != RENDER_PRIMITIVE_TRIANGLE_LIST) reason = 3;
		else if (state.pipeline.vertexProgram != RENDER_LEGACY_VERTEX_FIXED_FUNCTION ||
			state.pipeline.pixelProgram != RENDER_LEGACY_PIXEL_FIXED_FUNCTION) reason = 4;
		else if (state.pipeline.blend.blendEnable || state.pipeline.alphaTestEnable ||
			state.pipeline.nPatchEnable) reason = 5;
		AdmissionDiagnostics::Increment(m_diagnostics.rejected[reason]);
	}
	GameRigidDrawBatch(const GameRigidDrawBatch &);
	GameRigidDrawBatch &operator=(const GameRigidDrawBatch &);
	NativeDrawPacket m_packet;
	LegacyLogicalState m_state;
	RenderMatrix4 m_worlds[RENDER_RIGID_INSTANCE_MAX];
	unsigned int m_count;
	uint64_t m_epoch;
	uint64_t m_geometry;
	uint64_t m_category;
	RenderResult m_failure;
	bool m_emitting;
	GameRigidDrawMetrics m_metrics;
	AdmissionDiagnostics m_diagnostics;
};

} }
#endif
