/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2025 Electronic Arts Inc.
** SPDX-License-Identifier: GPL-3.0-or-later
*/

#include "Lib/ParallelVisibility.h"
#include "Lib/PipelineExecutionPolicy.h"
#include "Lib/JobFloatingPointState.h"

#include <new>
#include <stdlib.h>
#include <string.h>

#if !defined(_MSC_VER) || _MSC_VER >= 1300
#include <chrono>
#include <thread>
#define RTS_PARALLEL_VISIBILITY_MODERN 1
#endif

namespace rts
{
namespace
{
bool s_parallelVisibilityEnabled = true; // Only accessed by the scene owner.

#if defined(RTS_BUILD_CORE_EXTRAS)
unsigned s_fault = 0;
unsigned s_faultOccurrence = 0;
typedef void (*VisibilityTestObserver)();
VisibilityTestObserver s_testObserver = 0;

bool consumeFault(unsigned fault)
{
	if (s_fault != fault || s_faultOccurrence == 0) return false;
	if (--s_faultOccurrence != 0) return false;
	s_fault = 0;
	return true;
}
#else
bool consumeFault(unsigned) { return false; }
#endif

JobMetricCounter nowNanoseconds()
{
#if defined(RTS_PARALLEL_VISIBILITY_MODERN)
	return static_cast<JobMetricCounter>(std::chrono::duration_cast<
		std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
#else
	return 0;
#endif
}

#if defined(RTS_PARALLEL_VISIBILITY_MODERN)
struct RangeTelemetry
{
	RangeTelemetry() : completed(false) {}
	std::thread::id thread;
	bool completed;
};

class VisibilityJob : public Job
{
public:
	// JobSystem destroys completed jobs on their executor. Keep that destruction
	// independent of the game's owner-managed global allocation path.
	static void *operator new(size_t bytes, const std::nothrow_t &) throw() { return malloc(bytes); }
	static void operator delete(void *memory) throw() { free(memory); }
	static void operator delete(void *memory, const std::nothrow_t &) throw() { free(memory); }

	VisibilityJob(const VisibilityFrame &frame, const VisibilityInput *inputs,
		VisibilityResult *outputs, unsigned count, RangeTelemetry *telemetry,
		bool fail)
		: m_frame(frame), m_inputs(inputs), m_outputs(outputs), m_count(count),
		  m_telemetry(telemetry), m_fail(fail)
	{
#if defined(RTS_BUILD_CORE_EXTRAS)
		m_observer = s_testObserver;
#endif
	}

	virtual void execute(JobContext &context)
	{
		JobFloatingPointScope floatingPointScope(m_floatingPointState);
		m_telemetry->thread = std::this_thread::get_id();
#if defined(RTS_BUILD_CORE_EXTRAS)
		if (m_observer) m_observer();
#endif
		if (m_fail) { context.fail(); return; }
		if (context.isCancellationRequested()) return;
		EvaluateVisibilityReference(m_frame, m_inputs, m_outputs, m_count);
		m_telemetry->completed = true;
	}

private:
	const VisibilityFrame m_frame;
	const VisibilityInput *m_inputs;
	VisibilityResult *m_outputs;
	unsigned m_count;
	RangeTelemetry *m_telemetry;
	bool m_fail;
	const JobFloatingPointState m_floatingPointState;
#if defined(RTS_BUILD_CORE_EXTRAS)
	VisibilityTestObserver m_observer;
#endif
};
#endif
}

VisibilityFrame::VisibilityFrame()
	: reflection(false), resetHiddenDrawableFlags(false),
	  translucentOcclusion(false), behindBuildingMarkers(false)
{
	memset(planes, 0, sizeof(planes));
}

VisibilityListBudget::VisibilityListBudget()
	: translucent(0), occluders(0), occludees(0), others(0) {}

ParallelVisibilityMetrics::ParallelVisibilityMetrics()
	: objectCount(0), submittedJobs(0), completedJobs(0), workerThreadsUsed(0),
	  serialFallbacks(0), snapshotNanoseconds(0), prepareNanoseconds(0),
	  waitNanoseconds(0), publicationNanoseconds(0) {}

JobMetricCounter VisibilityClockNanoseconds() { return nowNanoseconds(); }
void SetParallelVisibilityEnabled(bool enabled) { s_parallelVisibilityEnabled = enabled; }
bool IsParallelVisibilityEnabled() { return s_parallelVisibilityEnabled && UseParallelPipelines(); }

bool VisibilitySphereInside(const VisibilityFrame &frame, const VisibilityInput &input)
{
	for (unsigned i = 0; i < 6; ++i)
	{
		const VisibilityPlane &plane = frame.planes[i];
		// Match CollisionMath::Overlap_Test(PlaneClass, SphereClass), including
		// tangent/NaN behavior and the left-to-right dot-product operation order.
		const float distance = input.x * plane.x + input.y * plane.y +
			input.z * plane.z - plane.distance;
		if (distance > input.radius) return false;
	}
	return true;
}

void EvaluateVisibilityReference(const VisibilityFrame &frame,
	const VisibilityInput *inputs, VisibilityResult *outputs, unsigned count)
{
	for (unsigned i = 0; i < count; ++i)
	{
		const VisibilityInput &input = inputs[i];
		unsigned flags = 0;
		if ((input.flags & VIS_INPUT_FORCE_VISIBLE) != 0)
			flags = VIS_RESULT_VISIBLE;
		else if (frame.reflection)
		{
			if (((input.flags & VIS_INPUT_DRAWABLE) == 0 ||
				(input.flags & VIS_INPUT_MIRROR) != 0) && VisibilitySphereInside(frame, input))
				flags = VIS_RESULT_VISIBLE;
		}
		else if ((input.flags & VIS_INPUT_HIDDEN) == 0 && VisibilitySphereInside(frame, input))
		{
			flags = VIS_RESULT_VISIBLE;
			if ((input.flags & VIS_INPUT_DRAWABLE) != 0)
			{
				if ((input.flags & (VIS_INPUT_EFFECTIVELY_HIDDEN | VIS_INPUT_SHROUDED)) != 0)
					flags = frame.resetHiddenDrawableFlags ? VIS_RESULT_RESET_DRAW_FLAGS : 0;
				else
				{
					flags |= VIS_RESULT_RESET_DRAW_FLAGS;
					if ((input.flags & VIS_INPUT_TRANSLUCENT) != 0) flags |= VIS_RESULT_TRANSLUCENT;
					if (frame.behindBuildingMarkers)
					{
						if ((input.flags & VIS_INPUT_STRUCTURE) != 0) flags |= VIS_RESULT_OCCLUDER;
						if ((input.flags & VIS_INPUT_OCCLUDEE) != 0) flags |= VIS_RESULT_OCCLUDEE;
					}
				}
			}
		}
		outputs[i].flags = flags;
	}
}

VisibilityPublication PlanVisibilityPublication(const VisibilityFrame &frame,
	const VisibilityResult &result, VisibilityListBudget &remaining)
{
	VisibilityPublication publication = { 0, 0 };
	if ((result.flags & (VIS_RESULT_VISIBLE | VIS_RESULT_RESET_DRAW_FLAGS)) !=
		(VIS_RESULT_VISIBLE | VIS_RESULT_RESET_DRAW_FLAGS)) return publication;
	if ((result.flags & VIS_RESULT_TRANSLUCENT) != 0 && remaining.translucent != 0)
	{
		--remaining.translucent;
		publication.lists = publication.drawFlags = VIS_LIST_TRANSLUCENT;
		if (!frame.translucentOcclusion) return publication;
	}
	if (!frame.behindBuildingMarkers) return publication;
	if ((result.flags & VIS_RESULT_OCCLUDER) != 0 && remaining.occluders != 0)
	{
		if ((publication.drawFlags & VIS_LIST_TRANSLUCENT) == 0)
		{
			--remaining.occluders;
			publication.lists |= VIS_LIST_OCCLUDER;
		}
		publication.drawFlags |= VIS_LIST_OCCLUDER;
	}
	else if ((result.flags & VIS_RESULT_OCCLUDEE) != 0 && remaining.occludees != 0)
	{
		--remaining.occludees;
		publication.lists |= VIS_LIST_OCCLUDEE;
		publication.drawFlags |= VIS_LIST_OCCLUDEE;
	}
	else if (publication.drawFlags == 0 && remaining.others != 0)
	{
		--remaining.others;
		publication.lists |= VIS_LIST_OTHER;
		publication.drawFlags |= VIS_LIST_OTHER;
	}
	return publication;
}

ParallelVisibilityWorkspace::ParallelVisibilityWorkspace()
	: m_inputs(0), m_results(0), m_capacity(0) {}

ParallelVisibilityWorkspace::~ParallelVisibilityWorkspace()
{
	free(m_results);
	free(m_inputs);
}

bool ParallelVisibilityWorkspace::reserve(unsigned count)
{
	if (count > MAX_OBJECTS) return false;
	if (count <= m_capacity) return true;
	unsigned capacity = 1024;
	while (capacity < count) capacity *= 2;
	VisibilityInput *inputs = consumeFault(1) ? 0 : static_cast<VisibilityInput *>(
		malloc(sizeof(VisibilityInput) * capacity));
	VisibilityResult *results = consumeFault(2) ? 0 : static_cast<VisibilityResult *>(
		malloc(sizeof(VisibilityResult) * capacity));
	if (inputs == 0 || results == 0)
	{
		free(results);
		free(inputs);
		return false;
	}
	free(m_results);
	free(m_inputs);
	m_inputs = inputs;
	m_results = results;
	m_capacity = capacity;
	return true;
}

bool ParallelVisibilityWorkspace::prepare(const VisibilityFrame &frame, unsigned count,
	bool parallel)
{
	parallel = parallel && UseParallelPipelines();
	m_metrics = ParallelVisibilityMetrics();
	m_metrics.objectCount = count;
	if (count > m_capacity) return false;
	const JobMetricCounter start = nowNanoseconds();
	bool prepared = false;
#if defined(RTS_PARALLEL_VISIBILITY_MODERN)
	JobSystem &system = JobSystem::instance();
	// Jobs must never wait for other jobs, including callers accidentally entering
	// this API from a worker. Such calls use the complete scalar reference path.
	if (parallel && count >= MIN_PARALLEL_OBJECTS && !system.isWorkerThread() &&
		system.ensureStarted() && system.workerCount() > 1)
	{
		enum { MAX_JOBS = 256, MIN_GRAIN = 512 };
		// MAX_OBJECTS / MIN_GRAIN bounds metadata, not the worker count.
		const unsigned jobCount = JobSystem::chooseRangeCount(count, MIN_GRAIN, system.workerCount());
		JobSubmission submissions[MAX_JOBS];
		JobHandle handles[MAX_JOBS];
		RangeTelemetry telemetry[MAX_JOBS];
		const JobGroup group = system.createGroup();
		unsigned created = 0;
		if (group.isValid())
		{
			for (; created < jobCount; ++created)
			{
				JobRange range;
				JobSystem::rangeForIndex(count, jobCount, created, range);
				const unsigned first = range.begin;
				const unsigned length = range.end - range.begin;
				VisibilityJob *job = consumeFault(3) ? 0 : new (std::nothrow) VisibilityJob(
					frame, m_inputs + first, m_results + first, length, telemetry + created,
					consumeFault(5));
				if (job == 0) break;
				submissions[created].job = job;
				submissions[created].priority = JOB_PRIORITY_FRAME_CRITICAL;
			}
		}
		if (created == jobCount && !consumeFault(4) &&
			system.trySubmitBatch(submissions, jobCount, group, handles))
		{
			m_metrics.submittedJobs = jobCount;
			if (consumeFault(6)) system.cancel(group);
			const JobMetricCounter waitStart = nowNanoseconds();
			system.wait(group); // Valid owner-created group; drains even on failure.
			m_metrics.waitNanoseconds = nowNanoseconds() - waitStart;
			prepared = !group.failed() && !group.wasCancelled();
			const std::thread::id owner = std::this_thread::get_id();
			for (unsigned j = 0; j < jobCount; ++j)
			{
				if (telemetry[j].completed) ++m_metrics.completedJobs;
				else prepared = false;
				if (telemetry[j].thread == owner || telemetry[j].thread == std::thread::id()) continue;
				unsigned previous = 0;
				while (previous < j && telemetry[previous].thread != telemetry[j].thread) ++previous;
				if (previous == j) ++m_metrics.workerThreadsUsed;
			}
		}
		else
		{
			// Batch rejection transfers no ownership and admits no jobs.
			for (unsigned j = 0; j < created; ++j) delete submissions[j].job;
		}
	}
#else
	(void)parallel;
#endif
	if (!prepared)
	{
		EvaluateVisibilityReference(frame, m_inputs, m_results, count);
		if (parallel && count >= MIN_PARALLEL_OBJECTS)
		{
			++m_metrics.serialFallbacks;
			JobSystem::instance().recordSerialFallback();
		}
	}
	m_metrics.prepareNanoseconds = nowNanoseconds() - start;
	return true;
}

void ParallelVisibilityWorkspace::recordSceneReference(unsigned count, bool fallback)
{
	m_metrics = ParallelVisibilityMetrics();
	m_metrics.objectCount = count;
	if (fallback)
	{
		m_metrics.serialFallbacks = 1;
		JobSystem::instance().recordSerialFallback();
	}
}

void ParallelVisibilityWorkspace::recordOwnerTimings(JobMetricCounter snapshot,
	JobMetricCounter publication)
{
	m_metrics.snapshotNanoseconds = snapshot;
	m_metrics.publicationNanoseconds = publication;
}

#if defined(RTS_BUILD_CORE_EXTRAS)
extern "C" void rts_parallel_visibility_set_test_fault(unsigned fault, unsigned occurrence)
{
	s_fault = fault;
	s_faultOccurrence = occurrence;
}
extern "C" void rts_parallel_visibility_set_test_observer(VisibilityTestObserver observer)
{
	s_testObserver = observer;
}
#endif
}
