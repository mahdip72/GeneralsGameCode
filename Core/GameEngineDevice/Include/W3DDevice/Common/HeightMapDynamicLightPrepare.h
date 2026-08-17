#pragma once

#include "Lib/HeightMapDynamicLightKernel.h"
#include "W3DDevice/Common/RadarTerrainPrepare.h"

#if defined(RTS_BUILD_CORE_EXTRAS)
typedef bool (*HeightMapDynamicLightPrepareTestObserver)(
	unsigned rowBegin, unsigned rowEnd);

extern "C" void rts_height_map_dynamic_light_prepare_set_test_observer(
	HeightMapDynamicLightPrepareTestObserver observer);
#endif

/* Owner-owned storage for one tile's dynamic-light CPU preparation. */
class HeightMapDynamicLightBatch
{
public:
	HeightMapDynamicLightBatch();
	~HeightMapDynamicLightBatch();

	bool initialize(unsigned width, unsigned height, unsigned lightCount);
	void reset();
	bool isAllocated() const;
	HeightMapDynamicLightSnapshot &snapshot() { return m_snapshot; }
	const HeightMapDynamicLightSnapshot &snapshot() const { return m_snapshot; }
	HeightMapDynamicLightVertex *inputVertices() { return m_inputVertices; }
	HeightMapDynamicLightVertex *outputVertices() { return m_outputVertices; }
	HeightMapDynamicLightSceneLight *lights() { return m_lights; }

	/* The caller must hold the service's consumer lease. */
	bool run(RadarTerrainPrepareService &service);
	/* Optional test evidence distinguishes the worker path from serial work. */
	bool run(RadarTerrainPrepareService &service, bool *ranParallel);

private:
	HeightMapDynamicLightBatch(const HeightMapDynamicLightBatch &);
	HeightMapDynamicLightBatch &operator=(const HeightMapDynamicLightBatch &);

	HeightMapDynamicLightSnapshot m_snapshot;
	HeightMapDynamicLightVertex *m_inputVertices;
	HeightMapDynamicLightVertex *m_outputVertices;
	HeightMapDynamicLightSceneLight *m_lights;
};
