/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2025 Electronic Arts Inc.
** SPDX-License-Identifier: GPL-3.0-or-later
*/
#include "Lib/SortingTriangleKernel.h"
#include "Lib/JobFloatingPointState.h"
#include "Lib/PipelineExecutionPolicy.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <new>

#if !defined(_MSC_VER) || _MSC_VER >= 1300
#include <chrono>
#include <thread>
#if defined(RTS_BUILD_CORE_EXTRAS)
#include <atomic>
#endif
#endif

namespace
{
const unsigned SORTING_TRIANGLE_MAX_JOBS = 256;

#if defined(RTS_BUILD_CORE_EXTRAS) && (!defined(_MSC_VER) || _MSC_VER >= 1300)
std::atomic<unsigned> sortingTriangleTestFault(0);
std::atomic<unsigned> sortingTriangleTestFaultOccurrence(0);

bool consumeSortingTriangleFault(unsigned fault)
{
	if (sortingTriangleTestFault.load(std::memory_order_acquire) != fault)
		return false;
	unsigned occurrence = sortingTriangleTestFaultOccurrence.load(
		std::memory_order_acquire);
	while (occurrence != 0)
	{
		if (sortingTriangleTestFaultOccurrence.compare_exchange_weak(occurrence,
			occurrence - 1, std::memory_order_acq_rel,
			std::memory_order_acquire))
		{
			if (occurrence == 1)
				sortingTriangleTestFault.store(0, std::memory_order_release);
			return occurrence == 1;
		}
	}
	return false;
}

extern "C" void rts_sorting_triangle_set_test_fault(unsigned fault,
	unsigned occurrence)
{
	sortingTriangleTestFaultOccurrence.store(occurrence,
		std::memory_order_release);
	sortingTriangleTestFault.store(fault, std::memory_order_release);
}
#else
bool consumeSortingTriangleFault(unsigned)
{
	return false;
}
#endif

bool addBytes(size_t count, size_t elementSize, size_t &bytes)
{
	if (count > static_cast<size_t>(-1) / elementSize)
		return false;
	const size_t addition = count * elementSize;
	if (bytes > static_cast<size_t>(-1) - addition)
		return false;
	bytes += addition;
	return true;
}
}

namespace rts
{

SortingTriangleScratchLease::SortingTriangleScratchLease()
	: m_storage(0), m_capacityBytes(0), m_descriptorOffset(0),
	  m_outputOffset(0), m_descriptorCount(0), m_outputCount(0),
	  m_allocationCount(0)
{
}

SortingTriangleScratchLease::~SortingTriangleScratchLease()
{
	free(m_storage);
	m_storage = 0;
	m_capacityBytes = 0;
}

bool SortingTriangleScratchLease::prepare(unsigned descriptorCount,
	unsigned outputCount, unsigned maximumBytes)
{
	m_descriptorCount = 0;
	m_outputCount = 0;
	if (descriptorCount > SORTING_TRIANGLE_MAX_DESCRIPTORS ||
		maximumBytes == 0 || maximumBytes > SORTING_TRIANGLE_MAXIMUM_SCRATCH_BYTES)
		return false;

	size_t descriptorBytes = 0;
	size_t outputBytes = 0;
	if (!addBytes(descriptorCount, sizeof(SortingTriangleDescriptor),
		descriptorBytes) ||
		!addBytes(outputCount, sizeof(SortingTriangleOutput), outputBytes))
		return false;
	if (descriptorBytes > static_cast<size_t>(maximumBytes) ||
		outputBytes > static_cast<size_t>(maximumBytes) - descriptorBytes)
		return false;
	const size_t requestedBytes = descriptorBytes + outputBytes;
	if (requestedBytes > SORTING_TRIANGLE_MAXIMUM_SCRATCH_BYTES)
		return false;

	if (requestedBytes != 0 && m_capacityBytes < requestedBytes)
	{
		if (consumeSortingTriangleFault(1))
			return false;
		size_t grown = m_capacityBytes != 0 ? m_capacityBytes : 4096;
		while (grown < requestedBytes)
		{
			if (grown > static_cast<size_t>(SORTING_TRIANGLE_MAXIMUM_SCRATCH_BYTES) / 2)
			{
				grown = requestedBytes;
				break;
			}
			grown *= 2;
		}
		if (grown > maximumBytes)
			grown = requestedBytes;
		void *replacement = malloc(grown);
		if (!replacement)
			return false;
		free(m_storage);
		m_storage = replacement;
		m_capacityBytes = static_cast<unsigned>(grown);
		++m_allocationCount;
	}

	m_descriptorOffset = 0;
	m_outputOffset = static_cast<unsigned>(descriptorBytes);
	m_descriptorCount = descriptorCount;
	m_outputCount = outputCount;
	return true;
}

SortingTriangleDescriptor *SortingTriangleScratchLease::descriptors()
{
	return m_storage != 0 && m_descriptorCount != 0 ?
		static_cast<SortingTriangleDescriptor *>(m_storage) : 0;
}

SortingTriangleOutput *SortingTriangleScratchLease::outputs()
{
	return m_storage != 0 && m_outputCount != 0 ?
		reinterpret_cast<SortingTriangleOutput *>(
			static_cast<unsigned char *>(m_storage) + m_outputOffset) : 0;
}

unsigned SortingTriangleScratchLease::descriptorCount() const
{
	return m_descriptorCount;
}

unsigned SortingTriangleScratchLease::outputCount() const
{
	return m_outputCount;
}

unsigned SortingTriangleScratchLease::capacityBytes() const
{
	return m_capacityBytes;
}

unsigned SortingTriangleScratchLease::allocationCount() const
{
	return m_allocationCount;
}

SortingTriangleOptions::SortingTriangleOptions()
	: parallel(true), minimumGrain(SORTING_TRIANGLE_DEFAULT_MINIMUM_GRAIN),
	  maximumScratchBytes(SORTING_TRIANGLE_DEFAULT_MAXIMUM_SCRATCH_BYTES),
	  cancellationGroup(0)
{
}

SortingTriangleMetrics::SortingTriangleMetrics()
	: submittedJobs(0), completedJobs(0), workerThreadsUsed(0),
	  serialFallbacks(0), waitNanoseconds(0)
{
}

bool SortingTriangleCompleted(SortingTriangleResult result)
{
	return result == SORTING_TRIANGLE_SERIAL ||
		result == SORTING_TRIANGLE_PARALLEL ||
		result == SORTING_TRIANGLE_SERIAL_FALLBACK;
}

namespace
{
bool cancelled(const SortingTriangleOptions &options)
{
	return options.cancellationGroup != 0 &&
		options.cancellationGroup->wasCancelled();
}

bool validateDescriptors(const SortingTriangleDescriptor *descriptors,
	unsigned descriptorCount, unsigned polygonCount,
	SortingTriangleOutput *output, SortingTriangleOutput *scratchOutput)
{
	if (descriptorCount > SORTING_TRIANGLE_MAX_DESCRIPTORS ||
		(descriptorCount != 0 && descriptors == 0) ||
		(polygonCount != 0 && (output == 0 || scratchOutput == 0 ||
			output == scratchOutput)))
		return false;
	if (polygonCount != 0)
	{
		const size_t maximumAddress = static_cast<size_t>(-1);
		if (static_cast<size_t>(polygonCount) > maximumAddress /
			sizeof(SortingTriangleOutput))
			return false;
		const size_t outputBytes = static_cast<size_t>(polygonCount) *
			sizeof(SortingTriangleOutput);
		const size_t outputBegin = reinterpret_cast<size_t>(output);
		const size_t scratchBegin = reinterpret_cast<size_t>(scratchOutput);
		if (outputBegin > maximumAddress - outputBytes ||
			scratchBegin > maximumAddress - outputBytes)
			return false;
		const size_t outputEnd = outputBegin + outputBytes;
		const size_t scratchEnd = scratchBegin + outputBytes;
		if (outputBegin < scratchEnd && scratchBegin < outputEnd)
			return false;
	}

	unsigned expectedOffset = 0;
	for (unsigned index = 0; index != descriptorCount; ++index)
	{
		const SortingTriangleDescriptor &descriptor = descriptors[index];
		if (descriptor.outputOffset != expectedOffset ||
			descriptor.vertexStrideBytes < sizeof(float) * 3 ||
			descriptor.polygonCount > polygonCount - expectedOffset)
			return false;
		if (descriptor.polygonCount != 0 &&
			(descriptor.vertices == 0 || descriptor.indices == 0 ||
				descriptor.vertexCount == 0))
			return false;
		expectedOffset += descriptor.polygonCount;
	}
	return expectedOffset == polygonCount;
}

void prepareOne(const SortingTriangleDescriptor &descriptor,
	unsigned polygonIndex, SortingTriangleOutput &output)
{
	const unsigned short *indices = descriptor.indices;
	unsigned short idx1 = indices[polygonIndex * 3] -
		descriptor.minVertexIndex;
	unsigned short idx2 = indices[polygonIndex * 3 + 1] -
		descriptor.minVertexIndex;
	unsigned short idx3 = indices[polygonIndex * 3 + 2] -
		descriptor.minVertexIndex;
	const unsigned char *vertexBytes = descriptor.vertices;
	const float *v1 = reinterpret_cast<const float *>(vertexBytes +
		static_cast<size_t>(idx1) * descriptor.vertexStrideBytes);
	const float *v2 = reinterpret_cast<const float *>(vertexBytes +
		static_cast<size_t>(idx2) * descriptor.vertexStrideBytes);
	const float *v3 = reinterpret_cast<const float *>(vertexBytes +
		static_cast<size_t>(idx3) * descriptor.vertexStrideBytes);

	output.tri.i = idx1 + descriptor.vertexOffset;
	output.tri.j = idx2 + descriptor.vertexOffset;
	output.tri.k = idx3 + descriptor.vertexOffset;
	output.idx = descriptor.nodeIndex;
	if (descriptor.commonZ != 0)
	{
		// Keep the legacy left-to-right single-precision expression intact.
		output.z = (v1[2] + v2[2] + v3[2]) / 3.0f;
	}
	else
	{
		output.z = (descriptor.zX * (v1[0] + v2[0] + v3[0]) +
			descriptor.zY * (v1[1] + v2[1] + v3[1]) +
			descriptor.zZ * (v1[2] + v2[2] + v3[2])) / 3.0f +
			descriptor.zTranslation;
	}
}

void prepareRange(const SortingTriangleDescriptor *descriptors,
	unsigned descriptorCount, SortingTriangleOutput *output,
	unsigned begin, unsigned end, JobContext *context)
{
	unsigned descriptorIndex = 0;
	while (descriptorIndex != descriptorCount &&
		descriptors[descriptorIndex].outputOffset +
			descriptors[descriptorIndex].polygonCount <= begin)
		++descriptorIndex;

	for (unsigned outputIndex = begin; outputIndex != end; ++outputIndex)
	{
		if (context != 0 && (outputIndex - begin) % 256 == 0 &&
			context->isCancellationRequested())
			return;
		while (descriptorIndex != descriptorCount &&
			outputIndex >= descriptors[descriptorIndex].outputOffset +
				descriptors[descriptorIndex].polygonCount)
			++descriptorIndex;
		if (descriptorIndex == descriptorCount)
			return;
		const SortingTriangleDescriptor &descriptor = descriptors[descriptorIndex];
		prepareOne(descriptor, outputIndex - descriptor.outputOffset,
			output[outputIndex]);
	}
}

void prepareSerial(const SortingTriangleDescriptor *descriptors,
	unsigned descriptorCount, SortingTriangleOutput *output)
{
	prepareRange(descriptors, descriptorCount, output, 0,
		descriptorCount == 0 ? 0 : descriptors[descriptorCount - 1].outputOffset +
		descriptors[descriptorCount - 1].polygonCount, 0);
}

#if !defined(_MSC_VER) || _MSC_VER >= 1300
struct RangeTelemetry
{
	RangeTelemetry() : completed(false) {}
	std::thread::id thread;
	bool completed;
};

class SortingTriangleJob : public Job
{
public:
	static void *operator new(size_t bytes, const std::nothrow_t &) throw()
	{
		return malloc(bytes);
	}
	static void operator delete(void *memory) throw()
	{
		free(memory);
	}
	static void operator delete(void *memory, const std::nothrow_t &) throw()
	{
		free(memory);
	}

	SortingTriangleJob(const SortingTriangleDescriptor *descriptors,
		unsigned descriptorCount, SortingTriangleOutput *output,
		unsigned begin, unsigned end, RangeTelemetry *telemetry)
		: m_descriptors(descriptors), m_descriptorCount(descriptorCount),
		  m_output(output), m_begin(begin), m_end(end), m_telemetry(telemetry)
	{
	}

	virtual void execute(JobContext &context)
	{
		JobFloatingPointScope floatingPointScope(m_floatingPointState);
		m_telemetry->thread = std::this_thread::get_id();
#if defined(RTS_BUILD_CORE_EXTRAS)
		if (consumeSortingTriangleFault(3))
		{
			context.fail();
			return;
		}
#endif
		if (context.isCancellationRequested())
			return;
		prepareRange(m_descriptors, m_descriptorCount, m_output,
			m_begin, m_end, &context);
		if (context.isCancellationRequested())
			return;
		m_telemetry->completed = true;
	}

private:
	const SortingTriangleDescriptor *m_descriptors;
	unsigned m_descriptorCount;
	SortingTriangleOutput *m_output;
	unsigned m_begin;
	unsigned m_end;
	RangeTelemetry *m_telemetry;
	JobFloatingPointState m_floatingPointState;
};
#endif
}

SortingTriangleResult PrepareSortingTriangles(
	const SortingTriangleDescriptor *descriptors,
	unsigned descriptorCount,
	unsigned polygonCount,
	SortingTriangleOutput *output,
	SortingTriangleOutput *scratchOutput,
	const SortingTriangleOptions &options,
	SortingTriangleMetrics *metrics)
{
	SortingTriangleMetrics localMetrics;
	if (metrics == 0)
		metrics = &localMetrics;
	*metrics = SortingTriangleMetrics();
	if (!validateDescriptors(descriptors, descriptorCount, polygonCount,
		output, scratchOutput))
		return SORTING_TRIANGLE_INVALID_INPUT;
	if (cancelled(options))
		return SORTING_TRIANGLE_CANCELLED;
	if (polygonCount == 0)
		return SORTING_TRIANGLE_SERIAL;

	if (!options.parallel || polygonCount < SORTING_TRIANGLE_MIN_PARALLEL_POLYGONS)
	{
		prepareSerial(descriptors, descriptorCount, scratchOutput);
		if (cancelled(options))
			return SORTING_TRIANGLE_CANCELLED;
		memcpy(output, scratchOutput,
			sizeof(SortingTriangleOutput) * polygonCount);
		return SORTING_TRIANGLE_SERIAL;
	}

#if !defined(_MSC_VER) || _MSC_VER >= 1300
	JobSystem &jobs = JobSystem::instance();
	const unsigned minimumGrain = options.minimumGrain != 0 ?
		options.minimumGrain : SORTING_TRIANGLE_DEFAULT_MINIMUM_GRAIN;
	if (!UseParallelPipelines() || jobs.isWorkerThread() ||
		!jobs.ensureStarted() || jobs.workerCount() <= 1)
	{
		++metrics->serialFallbacks;
		jobs.recordSerialFallback();
		return SORTING_TRIANGLE_SERIAL_FALLBACK;
	}

	unsigned jobCount = JobSystem::chooseRangeCount(polygonCount,
		minimumGrain, jobs.workerCount());
	if (jobCount < 2)
	{
		++metrics->serialFallbacks;
		jobs.recordSerialFallback();
		return SORTING_TRIANGLE_SERIAL_FALLBACK;
	}
	if (jobCount > SORTING_TRIANGLE_MAX_JOBS)
		jobCount = SORTING_TRIANGLE_MAX_JOBS;

	JobGroup group = jobs.createGroup();
	if (!group.isValid())
	{
		++metrics->serialFallbacks;
		jobs.recordSerialFallback();
		return SORTING_TRIANGLE_SERIAL_FALLBACK;
	}

	RangeTelemetry telemetry[SORTING_TRIANGLE_MAX_JOBS];
	unsigned created = 0;
	bool accepted = true;
	for (; created != jobCount; ++created)
	{
		JobRange range;
		if (!JobSystem::rangeForIndex(polygonCount, jobCount, created, range))
		{
			accepted = false;
			break;
		}
		SortingTriangleJob *job = consumeSortingTriangleFault(2) ? 0 :
			new (std::nothrow) SortingTriangleJob(descriptors, descriptorCount,
			scratchOutput, range.begin, range.end, telemetry + created);
		JobHandle handle = job ? jobs.trySubmit(job,
			JOB_PRIORITY_FRAME_CRITICAL, group) : JobHandle();
		if (!handle.isValid())
		{
			delete job;
			accepted = false;
			break;
		}
		++metrics->submittedJobs;
	}

	if (!accepted)
	{
		jobs.cancel(group);
		jobs.wait(group);
		++metrics->serialFallbacks;
		jobs.recordSerialFallback();
		return SORTING_TRIANGLE_SERIAL_FALLBACK;
	}

	const std::chrono::steady_clock::time_point waitStart =
		std::chrono::steady_clock::now();
	jobs.wait(group);
	metrics->waitNanoseconds = static_cast<JobMetricCounter>(
		std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::steady_clock::now() - waitStart).count());
	const std::thread::id owner = std::this_thread::get_id();
	for (unsigned index = 0; index != jobCount; ++index)
	{
		if (telemetry[index].completed)
			++metrics->completedJobs;
		if (telemetry[index].thread == owner ||
			telemetry[index].thread == std::thread::id())
			continue;
		unsigned previous = 0;
		while (previous < index && telemetry[previous].thread !=
			telemetry[index].thread)
			++previous;
		if (previous == index)
			++metrics->workerThreadsUsed;
	}
	if (cancelled(options))
		return SORTING_TRIANGLE_CANCELLED;
	if (group.wasCancelled() || group.failed() ||
		metrics->completedJobs != metrics->submittedJobs)
	{
		++metrics->serialFallbacks;
		jobs.recordSerialFallback();
		return group.wasCancelled() ? SORTING_TRIANGLE_CANCELLED :
			SORTING_TRIANGLE_SERIAL_FALLBACK;
	}
	memcpy(output, scratchOutput,
		sizeof(SortingTriangleOutput) * polygonCount);
	return SORTING_TRIANGLE_PARALLEL;
#else
	(void)metrics;
	return SORTING_TRIANGLE_SERIAL_FALLBACK;
#endif
}
}
