#pragma once

#include "Lib/ParticleRenderKernel.h"
#include "Lib/WaterMeshKernel.h"

class RadarTerrainPrepareService;

/* Owner-only scratch, reusable across systems in one frame. Allocation and
 * resize finish before admission. run() joins before returning; no worker
 * retains a pointer when this storage is reused or destroyed. */
class ParticleRenderBatch
{
public:
	ParticleRenderBatch();
	~ParticleRenderBatch();
	bool initialize(unsigned count);
	ParticleRenderInput *input() { return m_input; }
	const ParticleRenderInput *input() const { return m_input; }
	const ParticleRenderOutput *output() const { return m_output; }
	unsigned count() const { return m_count; }
	bool run(const ParticleRenderBounds &bounds,
		RadarTerrainPrepareService &service, bool *ranParallel = 0);
	bool run(const ParticleRenderBounds &bounds);

private:
	ParticleRenderBatch(const ParticleRenderBatch &);
	ParticleRenderBatch &operator=(const ParticleRenderBatch &);
	ParticleRenderInput *m_input;
	ParticleRenderOutput *m_output;
	unsigned m_count, m_capacity;
	bool m_initialized;
};

/* At most 8 MiB per snapshot including heights, rows and CPU vertices.
 * Transactional owner-only resize can briefly retain two snapshots (16 MiB). */
class WaterMeshBatch
{
public:
	WaterMeshBatch();
	~WaterMeshBatch();
	bool initialize(unsigned width, unsigned height);
	WaterMeshSnapshot &snapshot() { return m_snapshot; }
	Real *heights() { return m_heights; }
	WaterMeshRowInput *rows() { return m_rows; }
	const WaterMeshVertex *output() const { return m_output; }
	bool run(RadarTerrainPrepareService &service, bool *ranParallel = 0);
	bool run();

private:
	WaterMeshBatch(const WaterMeshBatch &);
	WaterMeshBatch &operator=(const WaterMeshBatch &);
	WaterMeshSnapshot m_snapshot;
	Real *m_heights;
	WaterMeshRowInput *m_rows;
	WaterMeshVertex *m_output;
	unsigned m_width, m_height;
};

#if defined(RTS_BUILD_CORE_EXTRAS)
enum EffectPrepareTestFault
{
	EFFECT_PREPARE_TEST_FAIL_INPUT_ALLOCATION = 1,
	EFFECT_PREPARE_TEST_FAIL_OUTPUT_ALLOCATION = 2
};
extern "C" void rts_effect_prepare_set_test_fault(unsigned fault);
#endif
