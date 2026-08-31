/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2026 TheSuperHackers
** SPDX-License-Identifier: GPL-3.0-or-later
*/
#include "Lib/GeometryTriangleDecode.h"
#include "Lib/JobFloatingPointState.h"
#include "Lib/PipelineExecutionPolicy.h"

#include <limits.h>
#include <new>
#include <stdlib.h>
#include <string.h>

#if !defined(_MSC_VER) || _MSC_VER >= 1300
#include <chrono>
#include <thread>
#endif

namespace
{
const unsigned MAX_GEOMETRY_TRIANGLE_JOBS = 64;
typedef char GeometryUnsignedMustBe32Bits[sizeof(unsigned) == 4 ? 1 : -1];
typedef char GeometryShortMustBe16Bits[sizeof(unsigned short) == 2 ? 1 : -1];
typedef char GeometryFloatMustBe32Bits[sizeof(float) == 4 ? 1 : -1];

#if defined(RTS_BUILD_CORE_EXTRAS)
// Fault selection is consumed exclusively by the calling owner, including
// the execution-failure bit captured before submitting each worker job.
unsigned triangleFault = 0;
unsigned triangleFaultOccurrence = 0;
typedef void (*TriangleTestObserver)();
TriangleTestObserver triangleTestObserver = 0;
bool consumeFault(unsigned fault)
{
	if (triangleFault != fault || triangleFaultOccurrence == 0) return false;
	if (--triangleFaultOccurrence != 0) return false;
	triangleFault = 0;
	return true;
}
#else
bool consumeFault(unsigned) { return false; }
#endif
}

#if defined(RTS_BUILD_CORE_EXTRAS)
extern "C" void rts_geometry_triangle_set_test_fault(unsigned fault,
	unsigned occurrence)
{
	triangleFault = fault;
	triangleFaultOccurrence = occurrence;
}
extern "C" void rts_geometry_triangle_set_test_observer(TriangleTestObserver observer)
{
	triangleTestObserver = observer;
}
#endif

namespace rts
{
GeometryTriangleDecodeScratch::GeometryTriangleDecodeScratch()
	: m_storage(0), m_count(0), m_inputBytes(0), m_capacityBytes(0),
	  m_indexWidthBytes(0), m_planeOffset(0), m_surfaceOffset(0),
	  m_prepared(false), m_ready(false), m_attributesInRange(false) {}

GeometryTriangleDecodeScratch::~GeometryTriangleDecodeScratch()
{
	free(m_storage);
}

bool GeometryTriangleDecodeScratch::prepare(unsigned count, unsigned indexWidthBytes,
	unsigned maximumBytes)
{
	m_count = m_inputBytes = 0;
	m_prepared = m_ready = m_attributesInRange = false;
	if (indexWidthBytes != GEOMETRY_TRIANGLE_INDEX16_BYTES &&
		indexWidthBytes != GEOMETRY_TRIANGLE_INDEX32_BYTES) return false;
	const unsigned bytesPerRecord = GEOMETRY_TRIANGLE_RECORD_BYTES +
		3 * indexWidthBytes + 4 * sizeof(float) + sizeof(unsigned char);
	if (maximumBytes == 0 || maximumBytes > GEOMETRY_TRIANGLE_MAXIMUM_SCRATCH_BYTES ||
		count > maximumBytes / bytesPerRecord ||
		count > UINT_MAX / GEOMETRY_TRIANGLE_RECORD_BYTES)
		return false;
	// The 16-bit index array can end on a two-byte boundary. Account for the
	// plane alignment padding inside the same aggregate input/output budget.
	const unsigned indexBytes = count * 3 * indexWidthBytes;
	const unsigned indexPadding = (4 - indexBytes % 4) % 4;
	const unsigned bytes = count * bytesPerRecord + indexPadding;
	if (bytes > maximumBytes) return false;
	if (bytes > m_capacityBytes || m_capacityBytes > maximumBytes)
	{
		if (consumeFault(1)) return false;
		// prepare invalidated the previous snapshot. Release it before growing,
		// so even the transient allocation peak respects the same byte cap.
		free(m_storage);
		m_storage = 0;
		m_capacityBytes = 0;
		m_storage = bytes != 0 ? malloc(bytes) : 0;
		if (bytes != 0 && m_storage == 0) return false;
		m_capacityBytes = bytes;
	}
	m_count = count;
	m_inputBytes = count * GEOMETRY_TRIANGLE_RECORD_BYTES;
	m_indexWidthBytes = indexWidthBytes;
	m_planeOffset = m_inputBytes + indexBytes + indexPadding;
	m_surfaceOffset = m_planeOffset + count * 4 * sizeof(float);
	m_prepared = true;
	return true;
}

unsigned char *GeometryTriangleDecodeScratch::records()
{
	return m_count != 0 ? static_cast<unsigned char *>(m_storage) : 0;
}

const void *GeometryTriangleDecodeScratch::indices() const
{
	return m_ready && m_count != 0 ?
		static_cast<const unsigned char *>(m_storage) + m_inputBytes : 0;
}

const float *GeometryTriangleDecodeScratch::planes() const
{
	return m_ready && m_count != 0 ? reinterpret_cast<const float *>(
		static_cast<const unsigned char *>(m_storage) + m_planeOffset) : 0;
}

const unsigned char *GeometryTriangleDecodeScratch::surfaces() const
{
	return m_ready && m_count != 0 ?
		static_cast<const unsigned char *>(m_storage) + m_surfaceOffset : 0;
}

bool GeometryTriangleDecodeScratch::attributesInRange() const
{
	return m_ready && m_attributesInRange;
}

unsigned GeometryTriangleDecodeScratch::count() const { return m_count; }
unsigned GeometryTriangleDecodeScratch::inputBytes() const { return m_inputBytes; }
unsigned GeometryTriangleDecodeScratch::indexWidthBytes() const { return m_indexWidthBytes; }
unsigned GeometryTriangleDecodeScratch::capacityBytes() const { return m_capacityBytes; }

GeometryTriangleDecodeOptions::GeometryTriangleDecodeOptions()
	: parallel(true), minimumGrain(256), cancellationGroup(0) {}

GeometryTriangleDecodeMetrics::GeometryTriangleDecodeMetrics()
	: submittedJobs(0), completedJobs(0), workerThreadsUsed(0), serialFallbacks(0),
	  waitNanoseconds(0) {}

bool GeometryTriangleDecodeCompleted(GeometryTriangleDecodeResult result)
{
	return result == GEOMETRY_TRIANGLE_SERIAL || result == GEOMETRY_TRIANGLE_PARALLEL ||
		result == GEOMETRY_TRIANGLE_SERIAL_FALLBACK;
}

namespace
{
bool cancelled(const GeometryTriangleDecodeOptions &options)
{
	return options.cancellationGroup != 0 && options.cancellationGroup->wasCancelled();
}

bool decodeOne(const unsigned char *record, void *indices, unsigned indexWidth,
	float *planes, unsigned char *surfaces, unsigned index)
{
	// W3dTriStruct is three uint32 indices, one uint32 attribute, three float32
	// normal components and a float32 distance. memcpy avoids aliasing/alignment
	// assumptions while preserving all source bits. Only distance is transformed.
	if (indexWidth == GEOMETRY_TRIANGLE_INDEX32_BYTES)
	{
		// The legacy unsigned-to-signed 32-bit assignment preserves these bits
		// on the supported platforms. Do not narrow Generals' indices to 16-bit.
		memcpy(static_cast<unsigned *>(indices) + index * 3, record, 12);
	}
	else
	{
		unsigned source[3];
		memcpy(source, record, sizeof(source));
		unsigned short *destination = static_cast<unsigned short *>(indices) + index * 3;
		destination[0] = static_cast<unsigned short>(source[0]);
		destination[1] = static_cast<unsigned short>(source[1]);
		destination[2] = static_cast<unsigned short>(source[2]);
	}
	unsigned attributes;
	memcpy(&attributes, record + 12, sizeof(attributes));
	surfaces[index] = static_cast<unsigned char>(attributes);
	memcpy(planes + index * 4, record + 16, sizeof(float) * 3);
	float distance;
	memcpy(&distance, record + 28, sizeof(distance));
	planes[index * 4 + 3] = -distance;
	return attributes < 256;
}

bool decodeRange(const unsigned char *records, void *indices, unsigned indexWidth,
	float *planes, unsigned char *surfaces, unsigned begin, unsigned end, JobContext *context)
{
	bool attributesInRange = true;
	for (unsigned index = begin; index != end; ++index)
	{
		if (context != 0 && (index - begin) % 256 == 0 &&
			context->isCancellationRequested()) return false;
		if (!decodeOne(records + static_cast<size_t>(index) * GEOMETRY_TRIANGLE_RECORD_BYTES,
			indices, indexWidth, planes, surfaces, index)) attributesInRange = false;
	}
	return attributesInRange;
}

#if !defined(_MSC_VER) || _MSC_VER >= 1300
struct RangeTelemetry
{
	RangeTelemetry() : completed(false), attributesInRange(false) {}
	std::thread::id thread;
	bool completed, attributesInRange;
};

class GeometryTriangleJob : public Job
{
public:
	static void *operator new(size_t bytes, const std::nothrow_t &) throw() { return malloc(bytes); }
	static void operator delete(void *memory) throw() { free(memory); }
	static void operator delete(void *memory, const std::nothrow_t &) throw() { free(memory); }
	GeometryTriangleJob(const unsigned char *records, void *indices, unsigned indexWidth,
		float *planes, unsigned char *surfaces,
		unsigned begin, unsigned end, RangeTelemetry *telemetry, bool fail)
		: m_records(records), m_indices(indices), m_indexWidth(indexWidth),
		  m_planes(planes), m_surfaces(surfaces), m_begin(begin), m_end(end),
		  m_telemetry(telemetry), m_fail(fail)
	{
#if defined(RTS_BUILD_CORE_EXTRAS)
		m_observer = triangleTestObserver;
#endif
	}
	virtual void execute(JobContext &context)
	{
		JobFloatingPointScope floatingPointScope(m_floatingPointState);
		m_telemetry->thread = std::this_thread::get_id();
#if defined(RTS_BUILD_CORE_EXTRAS)
		if (m_observer != 0) m_observer();
#endif
		if (m_fail) { context.fail(); return; }
		if (context.isCancellationRequested()) return;
		m_telemetry->attributesInRange = decodeRange(m_records, m_indices, m_indexWidth,
			m_planes, m_surfaces, m_begin, m_end, &context);
		if (!context.isCancellationRequested()) m_telemetry->completed = true;
	}
private:
	const unsigned char *m_records;
	void *m_indices;
	unsigned m_indexWidth;
	float *m_planes;
	unsigned char *m_surfaces;
	unsigned m_begin, m_end;
	RangeTelemetry *m_telemetry;
	bool m_fail;
	const JobFloatingPointState m_floatingPointState;
#if defined(RTS_BUILD_CORE_EXTRAS)
	TriangleTestObserver m_observer;
#endif
};
#endif
}

GeometryTriangleDecodeResult DecodeGeometryTriangles(GeometryTriangleDecodeScratch &scratch,
	const GeometryTriangleDecodeOptions &options, GeometryTriangleDecodeMetrics *metrics)
{
	GeometryTriangleDecodeMetrics localMetrics;
	if (metrics == 0) metrics = &localMetrics;
	*metrics = GeometryTriangleDecodeMetrics();
	scratch.m_ready = scratch.m_attributesInRange = false;
	if (!scratch.m_prepared) return GEOMETRY_TRIANGLE_INVALID_INPUT;
	if (cancelled(options)) return GEOMETRY_TRIANGLE_CANCELLED;
	if (scratch.m_count == 0)
	{ scratch.m_ready = scratch.m_attributesInRange = true; return GEOMETRY_TRIANGLE_SERIAL; }
	if (scratch.m_storage == 0 || scratch.m_inputBytes / GEOMETRY_TRIANGLE_RECORD_BYTES !=
		scratch.m_count) return GEOMETRY_TRIANGLE_INVALID_INPUT;
	const unsigned char *records = static_cast<const unsigned char *>(scratch.m_storage);
	void *indices = static_cast<unsigned char *>(scratch.m_storage) + scratch.m_inputBytes;
	float *planes = reinterpret_cast<float *>(
		static_cast<unsigned char *>(scratch.m_storage) + scratch.m_planeOffset);
	unsigned char *surfaces = static_cast<unsigned char *>(scratch.m_storage) + scratch.m_surfaceOffset;
	bool parallel = false;
	bool fallback = false;
	bool attributesInRange = true;
	if (options.parallel && scratch.m_count >= GEOMETRY_TRIANGLE_MIN_PARALLEL_RECORDS)
	{
		fallback = true;
#if !defined(_MSC_VER) || _MSC_VER >= 1300
		JobSystem &jobs = JobSystem::instance();
		if (UseParallelPipelines() && !jobs.isWorkerThread() && jobs.ensureStarted() &&
			jobs.workerCount() > 1)
		{
			unsigned jobCount = JobSystem::chooseRangeCount(scratch.m_count,
				options.minimumGrain != 0 ? options.minimumGrain : 256, jobs.workerCount());
			if (jobCount > MAX_GEOMETRY_TRIANGLE_JOBS) jobCount = MAX_GEOMETRY_TRIANGLE_JOBS;
			JobGroup group = jobCount > 1 ? jobs.createGroup() : JobGroup();
			if (group.isValid())
			{
				RangeTelemetry telemetry[MAX_GEOMETRY_TRIANGLE_JOBS];
				bool accepted = true;
				for (unsigned index = 0; index != jobCount; ++index)
				{
					JobRange range;
					if (!JobSystem::rangeForIndex(scratch.m_count, jobCount, index, range))
					{ accepted = false; break; }
					const bool failExecution = consumeFault(3);
					GeometryTriangleJob *job = consumeFault(2) ? 0 : new (std::nothrow)
						GeometryTriangleJob(records, indices, scratch.m_indexWidthBytes, planes, surfaces,
							range.begin, range.end,
							telemetry + index, failExecution);
					JobHandle handle = job != 0 ? jobs.trySubmit(job, JOB_PRIORITY_STREAMING, group) : JobHandle();
					if (!handle.isValid()) { delete job; accepted = false; break; }
					++metrics->submittedJobs;
				}
				if (!accepted) jobs.cancel(group);
				const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
				jobs.wait(group);
				metrics->waitNanoseconds = static_cast<JobMetricCounter>(
					std::chrono::duration_cast<std::chrono::nanoseconds>(
						std::chrono::steady_clock::now() - start).count());
				const std::thread::id owner = std::this_thread::get_id();
				for (unsigned index = 0; index != metrics->submittedJobs; ++index)
				{
					if (telemetry[index].completed) ++metrics->completedJobs;
					if (!telemetry[index].attributesInRange) attributesInRange = false;
					if (telemetry[index].thread == owner || telemetry[index].thread == std::thread::id()) continue;
					unsigned previous = 0;
					while (previous < index && telemetry[previous].thread != telemetry[index].thread) ++previous;
					if (previous == index) ++metrics->workerThreadsUsed;
				}
				parallel = accepted && !group.failed() && !group.wasCancelled() &&
					metrics->completedJobs == metrics->submittedJobs;
				fallback = !parallel;
			}
		}
#endif
	}
	if (cancelled(options)) return GEOMETRY_TRIANGLE_CANCELLED;
	if (!parallel) attributesInRange = decodeRange(records, indices, scratch.m_indexWidthBytes,
		planes, surfaces, 0, scratch.m_count, 0);
	if (cancelled(options)) return GEOMETRY_TRIANGLE_CANCELLED;
	scratch.m_ready = true;
	scratch.m_attributesInRange = attributesInRange;
	if (fallback)
	{
		++metrics->serialFallbacks;
		JobSystem::instance().recordSerialFallback();
		return GEOMETRY_TRIANGLE_SERIAL_FALLBACK;
	}
	return parallel ? GEOMETRY_TRIANGLE_PARALLEL : GEOMETRY_TRIANGLE_SERIAL;
}
}
