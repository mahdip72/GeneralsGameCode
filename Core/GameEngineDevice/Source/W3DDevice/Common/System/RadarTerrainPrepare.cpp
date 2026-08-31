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
#include "Lib/JobFloatingPointState.h"
#include "Lib/PipelineExecutionPolicy.h"

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

unsigned BuildRadarTerrainRowRanges(unsigned rowBegin, unsigned rowEnd,
	unsigned desiredRangeCount, RadarTerrainRowRange *ranges,
	unsigned rangeCapacity)
{
	if (ranges == 0 || rowBegin >= rowEnd || desiredRangeCount == 0 ||
		rangeCapacity == 0)
	{
		return 0;
	}

	const unsigned rowCount = rowEnd - rowBegin;
	unsigned rangeCount = desiredRangeCount;
	if (rangeCount > rowCount)
	{
		rangeCount = rowCount;
	}
	if (rangeCount > rangeCapacity)
	{
		rangeCount = rangeCapacity;
	}

	const unsigned rowsPerRange = rowCount / rangeCount;
	const unsigned extraRows = rowCount % rangeCount;
	unsigned nextRow = rowBegin;
	unsigned index;
	for (index = 0; index < rangeCount; ++index)
	{
		const unsigned rows = rowsPerRange + (index < extraRows ? 1 : 0);
		ranges[index].begin = nextRow;
		ranges[index].end = nextRow + rows;
		nextRow += rows;
	}
	return rangeCount;
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
	unsigned char *results;
	unsigned resultCount;
};

/* A task owns only this batch view pointer and its exclusive row range. */
class RadarPrepareRowTask : public rts::Job
{
public:
	RadarPrepareRowTask(RadarTerrainTaskBatch *batch, unsigned rowBegin,
		unsigned rowEnd, unsigned resultIndex)
		: m_batch(batch), m_rowBegin(rowBegin), m_rowEnd(rowEnd),
		  m_resultIndex(resultIndex)
	{
		if (m_batch != 0 && m_resultIndex < m_batch->resultCount)
		{
			m_batch->results[m_resultIndex] = 0;
		}
	}

	virtual ~RadarPrepareRowTask()
	{
	}

	virtual void execute(rts::JobContext &context)
	{
		rts::JobFloatingPointScope floatScope(m_floatState);
		const bool completed = m_batch != 0 && m_batch->work != 0 &&
			m_batch->work->executeRows(m_rowBegin, m_rowEnd);
		if (m_batch != 0 && m_resultIndex < m_batch->resultCount)
		{
			m_batch->results[m_resultIndex] = completed ? 1 : 0;
		}
		if (!completed)
		{
			context.fail();
		}
	}

private:
	RadarPrepareRowTask(const RadarPrepareRowTask &);
	RadarPrepareRowTask &operator=(const RadarPrepareRowTask &);

	RadarTerrainTaskBatch *m_batch;
	unsigned m_rowBegin;
	unsigned m_rowEnd;
	unsigned m_resultIndex;
	rts::JobFloatingPointState m_floatState;
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

	if (m_initialized && !m_stopping)
	{
		/* Do not silently replace an active display's configuration. */
		return m_requestedWorkers == workerCount &&
			m_queueCapacity == queueCapacity;
	}

	if (m_leaseActive)
	{
		return false;
	}

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
		(!rts::UseParallelPipelines() || rts::JobSystem::instance().ensureStarted());
}

bool RadarTerrainPrepareService::runAttempt(RadarPrepareRowWork *work,
	unsigned rowBegin, unsigned rowEnd,
	unsigned desiredRangeCount, bool *ranParallel)
{
	if (ranParallel != 0)
	{
		*ranParallel = false;
	}
	if (work == 0 || rowBegin >= rowEnd || desiredRangeCount == 0)
	{
		return false;
	}

	rts::JobSystem &system = rts::JobSystem::instance();
	if (!system.ensureStarted())
	{
		return false;
	}

	const unsigned rowCount = rowEnd - rowBegin;
	unsigned rangeCapacity = desiredRangeCount;
	if (rangeCapacity > rowCount)
	{
		rangeCapacity = rowCount;
	}
	RadarTerrainRowRange *ranges = 0;
	RadarPrepareRowTask **tasks = 0;
	rts::JobSubmission *submissions = 0;
	rts::JobHandle *handles = 0;
	unsigned char *results = 0;
	RadarTerrainTaskBatch taskBatch;
	taskBatch.work = work;
	taskBatch.results = 0;
	taskBatch.resultCount = 0;
	try
	{
		ranges = new RadarTerrainRowRange[rangeCapacity];
		tasks = new RadarPrepareRowTask *[rangeCapacity];
		submissions = new rts::JobSubmission[rangeCapacity];
		handles = new rts::JobHandle[rangeCapacity];
		results = new unsigned char[rangeCapacity];
	}
	catch (...)
	{
		delete [] results;
		delete [] handles;
		delete [] submissions;
		delete [] tasks;
		delete [] ranges;
		return false;
	}

	const unsigned rangeCount = BuildRadarTerrainRowRanges(rowBegin, rowEnd,
		desiredRangeCount, ranges, rangeCapacity);
	if (rangeCount == 0)
	{
		delete [] results;
		delete [] handles;
		delete [] submissions;
		delete [] tasks;
		delete [] ranges;
		return false;
	}
	taskBatch.results = results;
	taskBatch.resultCount = rangeCount;
	memset(results, 0, rangeCount);
	unsigned index;
	for (index = 0; index < rangeCount; ++index)
	{
		tasks[index] = 0;
	}
	for (index = 0; index < rangeCount; ++index)
	{
		tasks[index] = radarPrepareAllocateRowTask(&taskBatch,
			ranges[index].begin, ranges[index].end, index);
		if (tasks[index] == 0)
		{
			unsigned cleanupIndex;
			for (cleanupIndex = 0; cleanupIndex < index; ++cleanupIndex)
			{
				delete tasks[cleanupIndex];
			}
			delete [] results;
			delete [] handles;
			delete [] submissions;
			delete [] tasks;
			delete [] ranges;
			return false;
		}
		submissions[index].job = tasks[index];
		submissions[index].priority = rts::JOB_PRIORITY_FRAME_CRITICAL;
	}

	rts::JobGroup group = system.createGroup();
	bool completed = group.isValid() && system.trySubmitBatch(submissions,
		rangeCount, group, handles);
	if (!completed)
	{
		for (index = 0; index < rangeCount; ++index)
		{
			delete tasks[index];
		}
	}
	else
	{
		completed = system.wait(group) && !group.failed() &&
			!group.wasCancelled();
		for (index = 0; index < rangeCount; ++index)
		{
			completed = completed && handles[index].succeeded() &&
				results[index] != 0;
		}
	}

	delete [] results;
	delete [] handles;
	delete [] submissions;
	delete [] tasks;
	delete [] ranges;
	if (completed && ranParallel != 0)
	{
		*ranParallel = rangeCount > 1;
	}
	return completed;
}

bool RadarTerrainPrepareService::runRows(RadarPrepareRowWork *work,
	unsigned rowBegin, unsigned rowEnd, bool *ranParallel)
{
	if (ranParallel != 0)
	{
		*ranParallel = false;
	}
	if (!m_initialized || m_stopping || !m_leaseActive ||
		work == 0 || rowBegin == rowEnd || rowBegin > rowEnd)
	{
		return false;
	}

	rts::JobSystem &system = rts::JobSystem::instance();
	if (system.isWorkerThread())
	{
		system.recordSerialFallback();
		return false;
	}
	if (!rts::UseParallelPipelines())
		return work->executeRows(rowBegin, rowEnd);
	const unsigned workerCount = system.ensureStarted() ? system.workerCount() : 0;
	const unsigned desiredRangeCount = workerCount == 0 ? 0 :
		rts::JobSystem::chooseRangeCount(rowEnd-rowBegin,
			work->minimumRowsPerTask(), workerCount);
	if (runAttempt(work, rowBegin, rowEnd, desiredRangeCount, ranParallel))
	{
		return true;
	}

	/* Preserve a one-range reference/recovery path after admission failures. */
	if (desiredRangeCount > 1 && runAttempt(work, rowBegin, rowEnd, 1,
		ranParallel))
	{
		system.recordSerialFallback();
		return true;
	}

	system.recordSerialFallback();
	return false;
}

bool RadarTerrainPrepareService::runRows(RadarTerrainSnapshot *snapshot,
	unsigned char *output, unsigned rowBegin, unsigned rowEnd,
	bool *ranParallel)
{
	if (!radarTerrainPrepareRowsAreValid(snapshot, output, rowBegin, rowEnd) ||
		rowBegin == rowEnd)
	{
		return false;
	}

	RadarTerrainRowWorkAdapter work(snapshot, output);
	return runRows(&work, rowBegin, rowEnd, ranParallel);
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
		return;
	}

	/* Calls are synchronous, so stopping admission is sufficient here. */
	m_stopping = true;
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
	/* runRows joins its group before returning; this service retains no jobs. */
	return 0;
}
#endif

RadarTerrainPrepareService &GetRadarTerrainPrepareService()
{
	static RadarTerrainPrepareService service;
	return service;
}
