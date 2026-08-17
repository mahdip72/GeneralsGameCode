/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2025 Electronic Arts Inc.
**
** This program is free software: you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation, either version 3 of the License, or
** (at your option) any later version.
*/

#include "W3DDevice/Common/RadarTerrainPrepare.h"

#include <limits.h>
#include <string.h>

RadarPrepareRowWork::RadarPrepareRowWork()
{
}

RadarPrepareRowWork::~RadarPrepareRowWork()
{
}

static bool radarTerrainCheckedMultiply(unsigned left, unsigned right,
	unsigned *result)
{
	if (result == 0 || (right != 0 && left > UINT_MAX / right))
	{
		return false;
	}
	*result = left * right;
	return true;
}

static bool radarTerrainCheckedAdd(unsigned left, unsigned right,
	unsigned *result)
{
	if (result == 0 || left > UINT_MAX - right)
	{
		return false;
	}
	*result = left + right;
	return true;
}

static RadarTerrainCellInput *radarTerrainAllocateCells(unsigned count)
{
	RadarTerrainCellInput *cells = 0;
	try
	{
		cells = new RadarTerrainCellInput[count];
	}
	catch (...)
	{
		cells = 0;
	}
	return cells;
}

static unsigned char *radarTerrainAllocateOutput(unsigned count)
{
	unsigned char *output = 0;
	try
	{
		output = new unsigned char[count];
	}
	catch (...)
	{
		output = 0;
	}
	return output;
}

RadarTerrainBatch::RadarTerrainBatch() : m_cells(0), m_output(0),
	m_complete(false)
{
	memset(&m_snapshot, 0, sizeof(m_snapshot));
}

RadarTerrainBatch::~RadarTerrainBatch()
{
	reset();
}

bool RadarTerrainBatch::initialize(unsigned width, unsigned height,
	unsigned formatCode)
{
	const unsigned bytesPerPixel = RadarTerrainBytesPerPixel(formatCode);
	unsigned cellCount;
	unsigned cellBytes;
	unsigned rowBytes;
	unsigned outputBytes;
	unsigned totalBytes;

	/* Refuse to replace live owner storage. */
	if (isAllocated() || width == 0 || height == 0 || bytesPerPixel == 0)
	{
		return false;
	}

	if (!radarTerrainCheckedMultiply(width, height, &cellCount) ||
		!radarTerrainCheckedMultiply(cellCount,
			static_cast<unsigned>(sizeof(RadarTerrainCellInput)), &cellBytes) ||
		!radarTerrainCheckedMultiply(width, bytesPerPixel, &rowBytes) ||
		!radarTerrainCheckedMultiply(rowBytes, height, &outputBytes) ||
		!radarTerrainCheckedAdd(cellBytes, outputBytes, &totalBytes) ||
		totalBytes > MAX_BYTES)
	{
		return false;
	}

	m_cells = radarTerrainAllocateCells(cellCount);
	if (m_cells == 0)
	{
		return false;
	}
	memset(m_cells, 0, cellBytes);

	m_output = radarTerrainAllocateOutput(outputBytes);
	if (m_output == 0)
	{
		reset();
		return false;
	}
	memset(m_output, 0, outputBytes);

	memset(&m_snapshot, 0, sizeof(m_snapshot));
	m_snapshot.width = width;
	m_snapshot.height = height;
	m_snapshot.bytesPerPixel = bytesPerPixel;
	m_snapshot.formatCode = formatCode;
	m_snapshot.rowBytes = rowBytes;
	m_snapshot.cells = m_cells;
	m_complete = false;
	return true;
}

void RadarTerrainBatch::reset()
{
	delete [] m_output;
	delete [] m_cells;
	m_output = 0;
	m_cells = 0;
	m_complete = false;
	memset(&m_snapshot, 0, sizeof(m_snapshot));
}

bool RadarTerrainBatch::isAllocated() const
{
	return m_cells != 0 && m_output != 0;
}

bool RadarTerrainBatchCapturePreflight(const RadarTerrainBatch &batch,
	unsigned expectedWidth, unsigned expectedHeight, Real xSample, Real ySample)
{
	const RadarTerrainSnapshot &snapshot = batch.snapshot();
	return batch.isAllocated() && expectedWidth != 0 && expectedHeight != 0 &&
		xSample != 0.0f && ySample != 0.0f &&
		snapshot.width == expectedWidth && snapshot.height == expectedHeight &&
		snapshot.cells == batch.cells();
}

bool SplitRadarTerrainRowRanges(unsigned rowBegin, unsigned rowEnd,
	RadarTerrainRowRange ranges[2])
{
	if (ranges == 0 || rowBegin > rowEnd)
	{
		return false;
	}

	const unsigned rowCount = rowEnd - rowBegin;
	const unsigned split = rowBegin + rowCount / 2 + rowCount % 2;
	ranges[0].begin = rowBegin;
	ranges[0].end = split;
	ranges[1].begin = split;
	ranges[1].end = rowEnd;
	return true;
}

/*
 * The terrain adapter is owner-stack state.  RadarTerrainPrepareService
 * joins both tasks before this object can leave scope.
 */
class RadarTerrainRowWorkAdapter : public RadarPrepareRowWork
{
public:
	RadarTerrainRowWorkAdapter(RadarTerrainSnapshot *snapshot,
		unsigned char *output)
		: m_snapshot(snapshot), m_output(output)
	{
	}

	virtual bool executeRows(unsigned rowBegin, unsigned rowEnd)
	{
		return m_snapshot != 0 && m_output != 0 &&
			ShadeRadarRows(*m_snapshot, m_output, rowBegin, rowEnd);
	}

private:
	RadarTerrainSnapshot *m_snapshot;
	unsigned char *m_output;
};

namespace
{

#if defined(RTS_BUILD_CORE_EXTRAS)
unsigned s_radarTerrainPrepareTestFault = 0;
unsigned s_radarTerrainPrepareTestFaultOccurrence = 0;

static bool consumeRadarTerrainPrepareTestFault(unsigned fault)
{
	if (s_radarTerrainPrepareTestFault != fault ||
		s_radarTerrainPrepareTestFaultOccurrence == 0)
	{
		return false;
	}

	--s_radarTerrainPrepareTestFaultOccurrence;
	if (s_radarTerrainPrepareTestFaultOccurrence != 0)
	{
		return false;
	}
	s_radarTerrainPrepareTestFault = 0;
	return true;
}
#endif

static bool radarTerrainPrepareCheckedMultiply(unsigned left, unsigned right,
	unsigned *result)
{
	if (result == 0 || (right != 0 && left > UINT_MAX / right))
	{
		return false;
	}
	*result = left * right;
	return true;
}

static bool radarTerrainPrepareCheckedAdd(unsigned left, unsigned right,
	unsigned *result)
{
	if (result == 0 || left > UINT_MAX - right)
	{
		return false;
	}
	*result = left + right;
	return true;
}

static bool radarTerrainPrepareRowsAreValid(
	const RadarTerrainSnapshot *snapshot, const unsigned char *output,
	unsigned rowBegin, unsigned rowEnd)
{
	unsigned requiredRowBytes;
	unsigned cellCount;
	unsigned cellBytes;
	unsigned stagedBytes;
	unsigned outputBytes;
	const unsigned taskAndBookkeepingBudget = 1024;
	if (snapshot == 0 || output == 0 || snapshot->cells == 0 ||
		snapshot->width == 0 || snapshot->height == 0 || rowBegin > rowEnd ||
		rowEnd > snapshot->height ||
		RadarTerrainBytesPerPixel(snapshot->formatCode) == 0 ||
		snapshot->bytesPerPixel !=
			RadarTerrainBytesPerPixel(snapshot->formatCode))
	{
		return false;
	}

	if (!radarTerrainPrepareCheckedMultiply(snapshot->width,
		snapshot->bytesPerPixel, &requiredRowBytes) ||
		snapshot->rowBytes < requiredRowBytes ||
		!radarTerrainPrepareCheckedMultiply(snapshot->width,
			snapshot->height, &cellCount) ||
		!radarTerrainPrepareCheckedMultiply(cellCount,
			static_cast<unsigned>(sizeof(RadarTerrainCellInput)), &cellBytes) ||
		!radarTerrainPrepareCheckedMultiply(snapshot->rowBytes,
			snapshot->height, &outputBytes) ||
		!radarTerrainPrepareCheckedAdd(cellBytes, outputBytes, &stagedBytes) ||
		stagedBytes > RadarTerrainBatch::MAX_BYTES -
			taskAndBookkeepingBudget)
	{
		return false;
	}

	return true;
}

/*
 * A worker borrows this owner-stack view only through the joined call.  The
 * view contains no live engine pointer; its two result bytes are disjoint.
 */
struct RadarTerrainTaskBatch
{
	RadarPrepareRowWork *work;
	unsigned char results[2];
};

/* A task owns only this batch view pointer and its exclusive row range. */
class RadarPrepareRowTask : public rts::Task
{
public:
	RadarPrepareRowTask(RadarTerrainTaskBatch *batch, unsigned rowBegin,
		unsigned rowEnd, unsigned resultIndex)
		: m_batch(batch), m_rowBegin(rowBegin), m_rowEnd(rowEnd),
		  m_resultIndex(resultIndex)
	{
		if (m_batch != 0 && m_resultIndex < 2)
		{
			m_batch->results[m_resultIndex] = 0;
		}
	}

	virtual ~RadarPrepareRowTask()
	{
	}

	virtual void execute()
	{
		const bool completed = m_batch != 0 && m_batch->work != 0 &&
			m_batch->work->executeRows(m_rowBegin, m_rowEnd);
		if (m_batch != 0 && m_resultIndex < 2)
		{
			m_batch->results[m_resultIndex] = completed ? 1 : 0;
		}
	}

private:
	RadarPrepareRowTask(const RadarPrepareRowTask &);
	RadarPrepareRowTask &operator=(const RadarPrepareRowTask &);

	RadarTerrainTaskBatch *m_batch;
	unsigned m_rowBegin;
	unsigned m_rowEnd;
	unsigned m_resultIndex;
};

static RadarPrepareRowTask *radarPrepareAllocateRowTask(
	RadarTerrainTaskBatch *batch, unsigned rowBegin, unsigned rowEnd,
	unsigned resultIndex)
{
#if defined(RTS_BUILD_CORE_EXTRAS)
	if (consumeRadarTerrainPrepareTestFault(
		RADAR_TERRAIN_PREPARE_TEST_FAIL_TASK_ALLOCATION))
	{
		return 0;
	}
#endif

	RadarPrepareRowTask *task = 0;
	try
	{
		task = new RadarPrepareRowTask(batch, rowBegin, rowEnd, resultIndex);
	}
	catch (...)
	{
		task = 0;
	}
	return task;
}

} // namespace

#if defined(RTS_BUILD_CORE_EXTRAS)
extern "C" void rts_radar_terrain_prepare_set_test_fault(
	unsigned fault, unsigned occurrence)
{
	s_radarTerrainPrepareTestFault = fault;
	s_radarTerrainPrepareTestFaultOccurrence = occurrence;
}
#endif

RadarTerrainPrepareService::RadarTerrainPrepareService()
	: m_requestedWorkers(0), m_queueCapacity(0), m_activeConsumer(0),
	  m_initialized(false), m_stopping(false), m_leaseActive(false)
{
}

RadarTerrainPrepareService::~RadarTerrainPrepareService()
{
	shutdown();
}

bool RadarTerrainPrepareService::initialize(unsigned workerCount,
	unsigned queueCapacity)
{
	if (workerCount == 0 || queueCapacity == 0)
	{
		return false;
	}

	if (workerCount > 2)
	{
		workerCount = 2;
	}

	if (m_initialized && !m_stopping)
	{
		/* Do not silently replace an active display's private runtime. */
		return m_requestedWorkers == workerCount &&
			m_queueCapacity == queueCapacity;
	}

	if (m_leaseActive)
	{
		return false;
	}

	/* shutdown() leaves the private TaskRuntime restartable and idle. */
	m_runtime.shutdown();
	m_requestedWorkers = workerCount;
	m_queueCapacity = queueCapacity;
	m_activeConsumer = 0;
	m_initialized = true;
	m_stopping = false;
	m_leaseActive = false;
	return true;
}

bool RadarTerrainPrepareService::tryAcquire(unsigned consumerId)
{
	if (!m_initialized || m_stopping || consumerId == 0 || m_leaseActive)
	{
		return false;
	}

	m_activeConsumer = consumerId;
	m_leaseActive = true;
	return true;
}

bool RadarTerrainPrepareService::warmup()
{
	return m_initialized && !m_stopping && !m_leaseActive &&
		startRuntime(m_requestedWorkers);
}

bool RadarTerrainPrepareService::startRuntime(unsigned workerCount)
{
	if (workerCount == 0 || workerCount > 2 || m_queueCapacity == 0)
	{
		return false;
	}

	if (m_runtime.isRunning())
	{
		if (m_runtime.workerCount() == workerCount)
		{
			return true;
		}
		stopIdleRuntime();
	}

	return m_runtime.start(workerCount, m_queueCapacity);
}

void RadarTerrainPrepareService::stopIdleRuntime()
{
	/* Only the owner calls this, and every accepted task is joined first. */
	m_runtime.waitUntilIdle();
	m_runtime.shutdown();
}

bool RadarTerrainPrepareService::runAttempt(RadarPrepareRowWork *work,
	unsigned rowBegin, unsigned rowEnd,
	unsigned workerCount)
{
	RadarTerrainRowRange ranges[2];
	RadarPrepareRowTask *tasks[2] = { 0, 0 };
	RadarTerrainTaskBatch taskBatch;
	bool submitted;
	taskBatch.work = work;
	taskBatch.results[0] = 0;
	taskBatch.results[1] = 0;
	if (!SplitRadarTerrainRowRanges(rowBegin, rowEnd, ranges))
	{
		return false;
	}

	if (!startRuntime(workerCount))
	{
		return false;
	}

	/* Always construct exactly two disjoint wrappers for one batch. */
	tasks[0] = radarPrepareAllocateRowTask(&taskBatch, ranges[0].begin,
		ranges[0].end, 0);
	if (tasks[0] == 0)
	{
		stopIdleRuntime();
		return false;
	}

	tasks[1] = radarPrepareAllocateRowTask(&taskBatch, ranges[1].begin,
		ranges[1].end, 1);
	if (tasks[1] == 0)
	{
		delete tasks[0];
		stopIdleRuntime();
		return false;
	}

	{
		rts::Task *submittedTasks[2];
		submittedTasks[0] = tasks[0];
		submittedTasks[1] = tasks[1];
		submitted = m_runtime.trySubmitBatch(submittedTasks, 2);
	}
	if (!submitted)
	{
		/* Rejected wrappers remain caller-owned by TaskRuntime contract. */
		delete tasks[0];
		delete tasks[1];
		stopIdleRuntime();
		return false;
	}

	/* Ownership transferred; wait only on this private runtime. */
	m_runtime.waitUntilIdle();
	return taskBatch.results[0] != 0 && taskBatch.results[1] != 0;
}

bool RadarTerrainPrepareService::runRows(RadarPrepareRowWork *work,
	unsigned rowBegin, unsigned rowEnd)
{
	if (!m_initialized || m_stopping || !m_leaseActive ||
		work == 0 || rowBegin == rowEnd || rowBegin > rowEnd)
	{
		return false;
	}

	/* A recovery worker count is not retained across owner calls. */
	if (m_runtime.isRunning() &&
		m_runtime.workerCount() != m_requestedWorkers)
	{
		stopIdleRuntime();
	}

	if (runAttempt(work, rowBegin, rowEnd, m_requestedWorkers))
	{
		return true;
	}

	/* A failed two-worker attempt must be idle before the one-worker retry. */
	stopIdleRuntime();
	if (m_requestedWorkers > 1 &&
		runAttempt(work, rowBegin, rowEnd, 1))
	{
		return true;
	}

	stopIdleRuntime();
	return false;
}

bool RadarTerrainPrepareService::runRows(RadarTerrainSnapshot *snapshot,
	unsigned char *output, unsigned rowBegin, unsigned rowEnd)
{
	if (!radarTerrainPrepareRowsAreValid(snapshot, output, rowBegin, rowEnd) ||
		rowBegin == rowEnd)
	{
		return false;
	}

	RadarTerrainRowWorkAdapter work(snapshot, output);
	return runRows(&work, rowBegin, rowEnd);
}

void RadarTerrainPrepareService::release(unsigned consumerId)
{
	if (m_leaseActive && m_activeConsumer == consumerId)
	{
		m_leaseActive = false;
		m_activeConsumer = 0;
	}
}

void RadarTerrainPrepareService::shutdown()
{
	if (!m_initialized && !m_leaseActive)
	{
		m_runtime.shutdown();
		return;
	}

	/* Stop admission first, then synchronously drain and join accepted work. */
	m_stopping = true;
	m_runtime.waitUntilIdle();
	m_runtime.shutdown();
	m_leaseActive = false;
	m_activeConsumer = 0;
	m_initialized = false;
	m_requestedWorkers = 0;
	m_queueCapacity = 0;
	m_stopping = false;
}

#if defined(RTS_BUILD_CORE_EXTRAS)
unsigned RadarTerrainPrepareService::pendingTaskCount() const
{
	return m_runtime.pendingTaskCount();
}
#endif

RadarTerrainPrepareService &GetRadarTerrainPrepareService()
{
	static RadarTerrainPrepareService service;
	return service;
}
