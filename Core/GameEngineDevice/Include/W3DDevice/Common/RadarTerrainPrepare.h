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

#include "Lib/JobSystem.h"
#include "Lib/RadarTerrainKernel.h"

class W3DRadar;

/*
 * Owner-stack work adapter for one synchronous render-preparation batch.  A
 * worker may call only this immutable-range operation; the owner keeps the
 * adapter and every buffer it references alive until the service returns.
 */
class RadarPrepareRowWork
{
	public:
	virtual ~RadarPrepareRowWork();
	virtual bool executeRows(unsigned rowBegin, unsigned rowEnd) = 0;

protected:
	RadarPrepareRowWork();

	private:
	RadarPrepareRowWork(const RadarPrepareRowWork &);
	RadarPrepareRowWork &operator=(const RadarPrepareRowWork &);
};

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

struct RadarTerrainRowRange
{
	unsigned begin;
	unsigned end;
};

/* Pure balanced split used by both owner-created row tasks. */
bool SplitRadarTerrainRowRanges(unsigned rowBegin, unsigned rowEnd,
	RadarTerrainRowRange ranges[2]);

/* Build a balanced, gap-free split without imposing a subsystem worker cap. */
unsigned BuildRadarTerrainRowRanges(unsigned rowBegin, unsigned rowEnd,
	unsigned desiredRangeCount, RadarTerrainRowRange *ranges,
	unsigned rangeCapacity);

/*
 * A synchronous client of the process-wide compute scheduler. The service owns no
 * raster data: a caller-owned snapshot and output buffer remain valid until
 * runRows returns.  Every lifecycle, lease, and run method is owner-thread
 * only and calls must never overlap.  A consumer must acquire before calling
 * runRows and release only after it has consumed the completed output.
 */
class RadarTerrainPrepareService
{
public:
	RadarTerrainPrepareService();
	~RadarTerrainPrepareService();

	/*
	 * The worker count is retained only as a compatibility/configuration hint;
	 * execution uses the shared scheduler's current topology-aware worker count.
	 */
	bool initialize(unsigned workerCount, unsigned queueCapacity);
	/* Start the shared scheduler during interactive display initialization to
	 * avoid a first-use hitch; headless replay may defer this until first use. */
	bool warmup();
	bool tryAcquire(unsigned consumerId);

	/* Returns true only when both row tasks completed successfully. */
	bool runRows(RadarTerrainSnapshot *snapshot, unsigned char *output,
		unsigned rowBegin, unsigned rowEnd, bool *ranParallel = 0);
	/* Generic row operation shared by later render-preparation consumers. */
	bool runRows(RadarPrepareRowWork *work, unsigned rowBegin,
		unsigned rowEnd, bool *ranParallel = 0);
	bool runRows(RadarPrepareRowWork &work, unsigned rowBegin,
		unsigned rowEnd, bool *ranParallel = 0)
	{
		return runRows(&work, rowBegin, rowEnd, ranParallel);
	}

	void release(unsigned consumerId);
	void shutdown();

	bool isInitialized() const { return m_initialized; }
	bool hasLease() const { return m_leaseActive; }
	unsigned activeConsumer() const { return m_activeConsumer; }
#if defined(RTS_BUILD_CORE_EXTRAS)
	unsigned pendingTaskCount() const;
#endif

private:
	RadarTerrainPrepareService(const RadarTerrainPrepareService &);
	RadarTerrainPrepareService &operator=(const RadarTerrainPrepareService &);

	bool runAttempt(RadarPrepareRowWork *work, unsigned rowBegin,
		unsigned rowEnd,
		unsigned desiredRangeCount, bool *ranParallel);

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
