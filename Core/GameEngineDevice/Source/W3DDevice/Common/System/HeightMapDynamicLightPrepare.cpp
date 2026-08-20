#include "W3DDevice/Common/HeightMapDynamicLightPrepare.h"

#include <limits.h>
#include <string.h>

namespace
{

#if defined(RTS_BUILD_CORE_EXTRAS)
static HeightMapDynamicLightPrepareTestObserver
	s_heightMapDynamicLightPrepareTestObserver = 0;
#endif

static bool checkedMultiply(unsigned left, unsigned right, unsigned *result)
{
	if (result == 0 || (right != 0 && left > UINT_MAX / right))
		return false;
	*result = left * right;
	return true;
}

static bool checkedAdd(unsigned left, unsigned right, unsigned *result)
{
	if (result == 0 || left > UINT_MAX - right)
		return false;
	*result = left + right;
	return true;
}

static HeightMapDynamicLightVertex *allocateVertices(unsigned count)
{
	HeightMapDynamicLightVertex *vertices = 0;
	try
	{
		vertices = new HeightMapDynamicLightVertex[count];
	}
	catch (...)
	{
		vertices = 0;
	}
	return vertices;
}

static HeightMapDynamicLightSceneLight *allocateLights(unsigned count)
{
	HeightMapDynamicLightSceneLight *lights = 0;
	try
	{
		lights = new HeightMapDynamicLightSceneLight[count];
	}
	catch (...)
	{
		lights = 0;
	}
	return lights;
}

class HeightMapDynamicLightRowWork : public RadarPrepareRowWork
{
public:
	HeightMapDynamicLightRowWork(HeightMapDynamicLightSnapshot *snapshot,
		HeightMapDynamicLightVertex *output)
		: m_snapshot(snapshot), m_output(output)
	{
	}

	virtual bool executeRows(unsigned rowBegin, unsigned rowEnd)
	{
		if (m_snapshot == 0 || m_output == 0)
			return false;
#if defined(RTS_BUILD_CORE_EXTRAS)
		if (s_heightMapDynamicLightPrepareTestObserver != 0 &&
			!s_heightMapDynamicLightPrepareTestObserver(rowBegin, rowEnd))
			return false;
#endif
		return PrepareHeightMapDynamicLightRows(*m_snapshot, m_output, rowBegin,
			rowEnd);
	}

private:
	HeightMapDynamicLightSnapshot *m_snapshot;
	HeightMapDynamicLightVertex *m_output;
};

} // namespace

#if defined(RTS_BUILD_CORE_EXTRAS)
extern "C" void rts_height_map_dynamic_light_prepare_set_test_observer(
	HeightMapDynamicLightPrepareTestObserver observer)
{
	s_heightMapDynamicLightPrepareTestObserver = observer;
}
#endif

HeightMapDynamicLightBatch::HeightMapDynamicLightBatch()
	: m_inputVertices(0), m_outputVertices(0), m_lights(0)
{
	memset(&m_snapshot, 0, sizeof(m_snapshot));
}

HeightMapDynamicLightBatch::~HeightMapDynamicLightBatch()
{
	reset();
}

bool HeightMapDynamicLightBatch::initialize(unsigned width, unsigned height,
	unsigned lightCount)
{
	unsigned cellCount;
	unsigned vertexCount;
	unsigned rowBytes;
	unsigned vertexBytes;
	unsigned lightBytes;
	unsigned batchBytes;

	if (width == 0 || height == 0 || lightCount >
		HEIGHTMAP_DYNAMIC_LIGHT_MAX_LIGHTS ||
		!checkedMultiply(width, height, &cellCount) ||
		!checkedMultiply(cellCount, HEIGHTMAP_DYNAMIC_LIGHT_VERTEX_COUNT,
			&vertexCount) || !checkedMultiply(vertexCount,
			static_cast<unsigned>(sizeof(HeightMapDynamicLightVertex)),
			&vertexBytes) || !checkedMultiply(width,
			HEIGHTMAP_DYNAMIC_LIGHT_VERTEX_COUNT *
				static_cast<unsigned>(sizeof(HeightMapDynamicLightVertex)),
			&rowBytes) || !checkedMultiply(lightCount,
			static_cast<unsigned>(sizeof(HeightMapDynamicLightSceneLight)),
			&lightBytes) || !checkedAdd(vertexBytes, vertexBytes, &batchBytes) ||
		!checkedAdd(batchBytes, lightBytes, &batchBytes) ||
		batchBytes > HEIGHTMAP_DYNAMIC_LIGHT_MAX_BATCH_BYTES)
		return false;

	if (isAllocated())
	{
		if (m_snapshot.width != width || m_snapshot.height != height ||
			m_snapshot.lightCount != lightCount)
		{
			reset();
		}
		else
		{
			/* Reuse the owner allocation for adjacent affected tiles. */
			memset(m_outputVertices, 0xFF, vertexBytes);
			memset(&m_snapshot, 0, sizeof(m_snapshot));
			m_snapshot.width = width;
			m_snapshot.height = height;
			m_snapshot.inputStrideBytes = rowBytes;
			m_snapshot.outputStrideBytes = rowBytes;
			m_snapshot.vertexCount = vertexCount;
			m_snapshot.vertices = m_inputVertices;
			m_snapshot.lights = m_lights;
			m_snapshot.lightCount = lightCount;
			m_snapshot.outputCapacityBytes = vertexBytes;
			m_snapshot.batchBytes = batchBytes;
			return true;
		}
	}

	m_inputVertices = allocateVertices(vertexCount);
	if (m_inputVertices == 0)
		return false;
	m_outputVertices = allocateVertices(vertexCount);
	if (m_outputVertices == 0)
	{
		reset();
		return false;
	}
	if (lightCount != 0)
	{
		m_lights = allocateLights(lightCount);
		if (m_lights == 0)
		{
			reset();
			return false;
		}
	}

	memset(m_outputVertices, 0xFF, vertexBytes);
	memset(&m_snapshot, 0, sizeof(m_snapshot));
	m_snapshot.width = width;
	m_snapshot.height = height;
	m_snapshot.inputStrideBytes = rowBytes;
	m_snapshot.outputStrideBytes = rowBytes;
	m_snapshot.vertexCount = vertexCount;
	m_snapshot.vertices = m_inputVertices;
	m_snapshot.lights = m_lights;
	m_snapshot.lightCount = lightCount;
	m_snapshot.outputCapacityBytes = vertexBytes;
	m_snapshot.batchBytes = batchBytes;
	return true;
}

void HeightMapDynamicLightBatch::reset()
{
	delete [] m_lights;
	delete [] m_outputVertices;
	delete [] m_inputVertices;
	m_lights = 0;
	m_outputVertices = 0;
	m_inputVertices = 0;
	memset(&m_snapshot, 0, sizeof(m_snapshot));
}

bool HeightMapDynamicLightBatch::isAllocated() const
{
	return m_inputVertices != 0 && m_outputVertices != 0 &&
		(m_snapshot.lightCount == 0 || m_lights != 0);
}

bool HeightMapDynamicLightBatch::run(RadarTerrainPrepareService &service)
{
	return run(service, 0);
}

bool HeightMapDynamicLightBatch::run(RadarTerrainPrepareService &service,
	bool *ranParallel)
{
	HeightMapDynamicLightRowWork work(&m_snapshot, m_outputVertices);
	if (ranParallel != 0)
		*ranParallel = false;
	if (!isAllocated())
		return false;
	/* Avoid two task/event handoffs for tiny edge updates. */
	if (m_snapshot.height < 2 || m_snapshot.width * m_snapshot.height < 8)
		return PrepareHeightMapDynamicLightRows(m_snapshot, m_outputVertices,
			0, m_snapshot.height) &&
			ValidatePreparedHeightMapDynamicLightStructure(m_snapshot,
				m_outputVertices);
	if (!service.runRows(work, 0, m_snapshot.height, ranParallel))
		return false;
	return ValidatePreparedHeightMapDynamicLightStructure(m_snapshot,
		m_outputVertices);
}
