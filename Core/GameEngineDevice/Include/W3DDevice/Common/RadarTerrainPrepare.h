/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2025 Electronic Arts Inc.
**
** This program is free software: you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation, either version 3 of the License, or
** (at your option) any later version.
*/

#pragma once

#include "Lib/TaskRuntime.h"
#include "Lib/RadarTerrainKernel.h"

class W3DRadar;

/*
 * Owner-side storage for one complete radar terrain preparation batch.
 *
 * The owner allocates and fills the cells, then keeps this object alive until
 * the staged raster has been consumed by the owner-side surface upload.  The
 * kernel only borrows snapshot().cells and output() during a synchronous call;
 * no render, terrain, bridge, object, or other engine pointer is retained.
 *
 * This class is deliberately not copyable.  A shallow copy would give two
 * owners the same arrays and would make a later worker/runtime stage unsafe.
 */
class RadarTerrainBatch
{
public:
	/* Includes cells, tight output, and conservative per-batch bookkeeping. */
	enum { MAX_BYTES = 8u * 1024u * 1024u };

	RadarTerrainBatch();
	~RadarTerrainBatch();

	/* Allocate one complete, tightly packed batch or leave it unallocated. */
	bool initialize(unsigned width, unsigned height, unsigned formatCode);
	void reset();

	bool isAllocated() const;
	bool isComplete() const { return m_complete; }

	const RadarTerrainSnapshot &snapshot() const { return m_snapshot; }
	const RadarTerrainCellInput *cells() const { return m_cells; }
	unsigned char *output() { return m_output; }
	const unsigned char *output() const { return m_output; }

private:
	friend class W3DRadar;

	RadarTerrainSnapshot &mutableSnapshot() { return m_snapshot; }
	RadarTerrainCellInput *mutableCells() { return m_cells; }
	void markComplete() { m_complete = true; }

	RadarTerrainBatch(const RadarTerrainBatch &);
	RadarTerrainBatch &operator=(const RadarTerrainBatch &);

	RadarTerrainSnapshot m_snapshot;
	RadarTerrainCellInput *m_cells;
	unsigned char *m_output;
	bool m_complete;
};

/* Pure owner-capture validation; it does not inspect engine or D3D state. */
bool RadarTerrainBatchCapturePreflight(const RadarTerrainBatch &batch,
	unsigned expectedWidth, unsigned expectedHeight, Real xSample, Real ySample);

/*
 * One private, synchronous render-preparation runtime.  The service owns no
 * raster data: a caller-owned snapshot and output buffer remain valid until
 * runRows returns.  A consumer must acquire before calling runRows and release
 * only after it has consumed the completed output on the owner thread.
 */
class RadarTerrainPrepareService
{
public:
	RadarTerrainPrepareService();
	~RadarTerrainPrepareService();

	/* Worker count is capped at two; capacity below two forces serial fallback. */
	bool initialize(unsigned workerCount, unsigned queueCapacity);
	bool tryAcquire(unsigned consumerId);

	/* Returns true only when both row tasks completed successfully. */
	bool runRows(RadarTerrainSnapshot *snapshot, unsigned char *output,
		unsigned rowBegin, unsigned rowEnd);

	void release(unsigned consumerId);
	void shutdown();

	bool isInitialized() const { return m_initialized; }
	bool hasLease() const { return m_leaseActive; }
	unsigned activeConsumer() const { return m_activeConsumer; }

private:
	RadarTerrainPrepareService(const RadarTerrainPrepareService &);
	RadarTerrainPrepareService &operator=(const RadarTerrainPrepareService &);

	bool startRuntime(unsigned workerCount);
	bool runAttempt(RadarTerrainSnapshot *snapshot,
		unsigned char *output, unsigned rowBegin, unsigned rowEnd,
		unsigned workerCount);
	void stopIdleRuntime();

	rts::TaskRuntime m_runtime;
	unsigned m_requestedWorkers;
	unsigned m_queueCapacity;
	unsigned m_activeConsumer;
	bool m_initialized;
	bool m_stopping;
	bool m_leaseActive;
};

/* Shared service accessor for later display lifecycle integration. */
RadarTerrainPrepareService &GetRadarTerrainPrepareService();

#if defined(RTS_BUILD_CORE_EXTRAS)
enum RadarTerrainPrepareTestFault
{
	RADAR_TERRAIN_PREPARE_TEST_FAIL_TASK_ALLOCATION = 1
};

extern "C" void rts_radar_terrain_prepare_set_test_fault(
	unsigned fault, unsigned occurrence);
#endif
