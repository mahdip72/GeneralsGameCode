#include "W3DDevice/Common/EffectPrepare.h"
#include "W3DDevice/Common/RadarTerrainPrepare.h"
#include "Lib/PipelineExecutionPolicy.h"

#include <string.h>

namespace
{
#if defined(RTS_BUILD_CORE_EXTRAS)
unsigned s_effectPrepareTestFault = 0;
bool failAllocation(unsigned fault)
{
	if (s_effectPrepareTestFault != fault)
		return false;
	s_effectPrepareTestFault = 0;
	return true;
}
#else
bool failAllocation(unsigned) { return false; }
#endif

class ParticleRenderWork : public RadarPrepareRowWork
{
public:
	ParticleRenderWork(const ParticleRenderInput *input, ParticleRenderOutput *output,
		unsigned count, const ParticleRenderBounds &bounds)
		: m_input(input), m_output(output), m_count(count), m_bounds(bounds) {}
	virtual unsigned minimumRowsPerTask() const { return 128; }
	virtual bool executeRows(unsigned begin, unsigned end)
	{
		return PrepareParticleRenderRange(m_input, m_output, m_count, m_bounds, begin, end);
	}
private:
	const ParticleRenderInput *m_input;
	ParticleRenderOutput *m_output;
	unsigned m_count;
	ParticleRenderBounds m_bounds;
};

class WaterMeshWork : public RadarPrepareRowWork
{
public:
	WaterMeshWork(const WaterMeshSnapshot &snapshot, WaterMeshVertex *output)
		: m_snapshot(snapshot), m_output(output) {}
	virtual unsigned minimumRowsPerTask() const
	{
		return m_snapshot.width >= 256 ? 1 : (256+m_snapshot.width-1)/m_snapshot.width;
	}
	virtual bool executeRows(unsigned begin, unsigned end)
	{
		return PrepareWaterMeshRows(m_snapshot, m_output, begin, end);
	}
private:
	WaterMeshSnapshot m_snapshot;
	WaterMeshVertex *m_output;
};

bool runEffect(RadarPrepareRowWork &work, unsigned rows, unsigned consumer,
	RadarTerrainPrepareService &service, bool *ranParallel)
{
	if (ranParallel != 0)
		*ranParallel = false;
	if (rows == 0)
		return true;
	rts::JobSystem &system = rts::JobSystem::instance();
	/* No owner scratch or nested wait is admitted from a compute worker. */
	if (system.isWorkerThread())
		return false;
	if (!rts::UseParallelPipelines() || rows/work.minimumRowsPerTask() < 2)
		return work.executeRows(0, rows);
	bool completed = false;
	bool acquired = false;
	if (service.tryAcquire(consumer))
	{
		acquired = true;
		completed = service.runRows(work, 0, rows, ranParallel);
		service.release(consumer);
	}
	if (completed)
		return true;
	/* runRows joins all admitted work, including failure/cancellation, before
	 * this complete reference pass replaces any partially prepared result. */
	// The row service already records a failed admission/execution fallback.
	if (!acquired)
		system.recordSerialFallback();
	if (ranParallel != 0)
		*ranParallel = false;
	return work.executeRows(0, rows);
}
}

#if defined(RTS_BUILD_CORE_EXTRAS)
extern "C" void rts_effect_prepare_set_test_fault(unsigned fault)
{
	s_effectPrepareTestFault = fault;
}
#endif

ParticleRenderBatch::ParticleRenderBatch()
	: m_input(0), m_output(0), m_count(0), m_capacity(0), m_initialized(false) {}

ParticleRenderBatch::~ParticleRenderBatch()
{
	delete [] m_output;
	delete [] m_input;
}

bool ParticleRenderBatch::initialize(unsigned count)
{
	if (rts::JobSystem::instance().isWorkerThread())
		return false;
	m_count = 0;
	m_initialized = false;
	if (count > PARTICLE_RENDER_MAX_COUNT)
		return false;
	if (count <= m_capacity)
	{
		m_count = count;
		m_initialized = true;
		return true;
	}
	ParticleRenderInput *input = 0;
	ParticleRenderOutput *output = 0;
	try
	{
		if (!failAllocation(1)) input = new ParticleRenderInput[count];
		if (input != 0 && !failAllocation(2)) output = new ParticleRenderOutput[count];
	}
	catch (...) {}
	if (input == 0 || output == 0)
	{
		delete [] output;
		delete [] input;
		return false;
	}
	delete [] m_output;
	delete [] m_input;
	m_input = input;
	m_output = output;
	m_count = count;
	m_capacity = count;
	m_initialized = true;
	return true;
}

bool ParticleRenderBatch::run(const ParticleRenderBounds &bounds,
	RadarTerrainPrepareService &service, bool *ranParallel)
{
	if (ranParallel != 0)
		*ranParallel = false;
	if (!m_initialized)
		return false;
	ParticleRenderWork work(m_input, m_output, m_count, bounds);
	return runEffect(work, m_count, 6, service, ranParallel);
}

bool ParticleRenderBatch::run(const ParticleRenderBounds &bounds)
{
	return run(bounds, GetRadarTerrainPrepareService());
}

WaterMeshBatch::WaterMeshBatch()
	: m_heights(0), m_rows(0), m_output(0), m_width(0), m_height(0)
{
	memset(&m_snapshot, 0, sizeof(m_snapshot));
}

WaterMeshBatch::~WaterMeshBatch()
{
	delete [] m_output;
	delete [] m_rows;
	delete [] m_heights;
}

bool WaterMeshBatch::initialize(unsigned width, unsigned height)
{
	if (width == 0 || height == 0 ||
		width > WATER_MESH_MAX_VERTICES/height ||
		rts::JobSystem::instance().isWorkerThread())
		return false;
	const unsigned count = width*height;
	const unsigned bytes = count*(sizeof(Real)+sizeof(WaterMeshVertex)) +
		height*sizeof(WaterMeshRowInput);
	if (bytes > 8u*1024u*1024u)
		return false;
	if (width == m_width && height == m_height)
		return true;
	Real *heights = 0;
	WaterMeshRowInput *rows = 0;
	WaterMeshVertex *output = 0;
	try
	{
		if (!failAllocation(1)) heights = new Real[count];
		if (heights != 0) rows = new WaterMeshRowInput[height];
		if (rows != 0 && !failAllocation(2)) output = new WaterMeshVertex[count];
	}
	catch (...) {}
	if (heights == 0 || rows == 0 || output == 0)
	{
		delete [] output;
		delete [] rows;
		delete [] heights;
		return false;
	}
	delete [] m_output;
	delete [] m_rows;
	delete [] m_heights;
	m_output = output;
	m_rows = rows;
	m_heights = heights;
	m_width = m_snapshot.width = width;
	m_height = m_snapshot.height = height;
	m_snapshot.heights = m_heights;
	m_snapshot.rows = m_rows;
	return true;
}

bool WaterMeshBatch::run(RadarTerrainPrepareService &service, bool *ranParallel)
{
	if (ranParallel != 0)
		*ranParallel = false;
	if (m_output == 0 || m_snapshot.width != m_width || m_snapshot.height != m_height ||
		m_snapshot.heights != m_heights || m_snapshot.rows != m_rows)
		return false;
	WaterMeshWork work(m_snapshot, m_output);
	return runEffect(work, m_height, 7, service, ranParallel);
}

bool WaterMeshBatch::run()
{
	return run(GetRadarTerrainPrepareService());
}
