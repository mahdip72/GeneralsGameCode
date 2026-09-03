#include "Lib/JobSystem.h"

#include <limits.h>
#include <new>
#include <stdio.h>
#include <string.h>
#if defined(_WIN32)
#if !defined(NOMINMAX)
#define NOMINMAX
#endif
#include <windows.h>
#endif
#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif

#if !defined(_MSC_VER) || _MSC_VER >= 1300
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#endif

#if defined(RTS_BUILD_CORE_EXTRAS)
enum JobSystemTestFault
{
	JOB_SYSTEM_TEST_FAIL_START = 1,
	JOB_SYSTEM_TEST_FAIL_WORKER_SCRATCH = 2,
	JOB_SYSTEM_TEST_FAIL_THREAD_CREATE = 3,
	JOB_SYSTEM_TEST_FAIL_GROUP_ALLOCATION = 4,
	JOB_SYSTEM_TEST_FAIL_JOB_ALLOCATION = 5,
	JOB_SYSTEM_TEST_FAIL_QUEUE_PUSH = 6,
	JOB_SYSTEM_TEST_FAIL_COMPLETION_PUSH = 7,
	JOB_SYSTEM_TEST_FAIL_PROMOTION_PUSH = 8,
	JOB_SYSTEM_TEST_COMPETING_FINALIZER = 9,
	JOB_SYSTEM_TEST_FAIL_AFTER_QUEUE_PUSH = 10
};

enum JobSystemTestPause
{
	JOB_SYSTEM_TEST_PAUSE_AFTER_ACCEPT = 1,
	JOB_SYSTEM_TEST_PAUSE_BEFORE_DEPENDENT_ENQUEUE = 2,
	JOB_SYSTEM_TEST_PAUSE_AFTER_QUEUE_FAILURE = 4,
	JOB_SYSTEM_TEST_PAUSE_NON_OWNER_FINALIZER = 8,
	JOB_SYSTEM_TEST_PAUSE_AFTER_DEPENDENT_ENQUEUE = 16,
	JOB_SYSTEM_TEST_PAUSE_AFTER_EXECUTION_CLAIM = 32,
	JOB_SYSTEM_TEST_PAUSE_AFTER_STALE_QUEUE_DISCARD = 64,
	JOB_SYSTEM_TEST_PAUSE_GROUP_WAIT_PREDICATE = 128,
	JOB_SYSTEM_TEST_PAUSE_AFTER_GROUP_COMPLETION = 256,
	JOB_SYSTEM_TEST_PAUSE_GROUP_COMPLETION_LOCK = 512,
	JOB_SYSTEM_TEST_PAUSE_HANDLE_WAIT_PREDICATE = 1024,
	JOB_SYSTEM_TEST_PAUSE_AFTER_HANDLE_COMPLETION = 2048,
	JOB_SYSTEM_TEST_PAUSE_HANDLE_COMPLETION_LOCK = 4096,
	JOB_SYSTEM_TEST_PAUSE_BEFORE_BATCH_READY_RECHECK = 8192,
	JOB_SYSTEM_TEST_PAUSE_READY_OWNERSHIP_RECHECK = 16384,
	JOB_SYSTEM_TEST_PAUSE_AFTER_EXECUTION_RETIREMENT = 32768
};

extern "C" void rts_job_system_set_test_fault(unsigned fault,
	unsigned occurrence);
#if !defined(_MSC_VER) || _MSC_VER >= 1300
extern "C" void rts_job_system_set_test_pause_mask(unsigned pauseMask);
extern "C" bool rts_job_system_wait_for_test_pause(unsigned pausePoint,
	unsigned timeoutMilliseconds);
extern "C" void rts_job_system_release_test_pause(unsigned pausePoint);
#endif
#endif

namespace
{
enum JobSystemTestLane
{
	JOB_SYSTEM_TEST_LANE_FULL = 0,
	JOB_SYSTEM_TEST_LANE_LOCAL_CAPACITY
};

const unsigned kLocalCapacityWorkerLimit = 12;
JobSystemTestLane g_testLane = JOB_SYSTEM_TEST_LANE_FULL;

bool parseJobSystemTestLane(int argc, const char *selector,
	JobSystemTestLane *lane)
{
	if (lane == 0 || argc < 1)
		return false;
	*lane = JOB_SYSTEM_TEST_LANE_FULL;
	if (argc == 1)
		return true;
	if (argc == 2 && selector != 0 &&
		strcmp(selector, "--local-capacity") == 0)
	{
		*lane = JOB_SYSTEM_TEST_LANE_LOCAL_CAPACITY;
		return true;
	}
	return false;
}

bool isLocalCapacityLane()
{
	return g_testLane == JOB_SYSTEM_TEST_LANE_LOCAL_CAPACITY;
}

unsigned workerCountForTest(unsigned requested, bool localCapacity)
{
	if (!localCapacity ||
		(requested != 0 && requested <= kLocalCapacityWorkerLimit))
		return requested;
	return kLocalCapacityWorkerLimit;
}

int check(bool condition, const char *message)
{
	if (!condition)
	{
		fprintf(stderr, "FAIL: %s\n", message);
		return 1;
	}
	return 0;
}

int runTest(const char *name, int (*test)())
{
	fprintf(stderr, "BEGIN: %s\n", name);
	fflush(stderr);
	const int result = test();
	fprintf(stderr, "END: %s (%s)\n", name, result == 0 ? "PASS" : "FAIL");
	fflush(stderr);
	return result;
}

int testJobSystemTestLaneSelection()
{
	int result = 0;
	JobSystemTestLane lane = JOB_SYSTEM_TEST_LANE_LOCAL_CAPACITY;
	result |= check(parseJobSystemTestLane(1, 0, &lane) &&
		lane == JOB_SYSTEM_TEST_LANE_FULL,
		"test lane defaults to the full high-core qualification lane");
	result |= check(parseJobSystemTestLane(2, "--local-capacity", &lane) &&
		lane == JOB_SYSTEM_TEST_LANE_LOCAL_CAPACITY,
		"local capacity selector chooses the bounded test lane explicitly");
	result |= check(!parseJobSystemTestLane(3, "--local-capacity", &lane),
		"unknown test lane arguments are rejected");
	result |= check(workerCountForTest(16, false) == 16 &&
		workerCountForTest(32, false) == 32,
		"full test lane retains its high-core worker requests");
	result |= check(workerCountForTest(0, true) == kLocalCapacityWorkerLimit &&
		workerCountForTest(16, true) == kLocalCapacityWorkerLimit &&
		workerCountForTest(kLocalCapacityWorkerLimit, true) ==
			kLocalCapacityWorkerLimit,
		"local capacity lane bounds automatic and oversized worker requests");
	return result;
}

class CountJob : public rts::Job
{
public:
	explicit CountJob(unsigned *count) : m_count(count) {}

	virtual void execute(rts::JobContext &)
	{
		++*m_count;
	}

private:
	unsigned *m_count;
};

class ContextFailJob : public rts::Job
{
public:
	virtual void execute(rts::JobContext &context) { context.fail(); }
};

class ExecutionIdentityJob : public rts::Job
{
public:
	ExecutionIdentityJob(bool *physicalWorker, unsigned *physicalWorkerIndex)
		: m_physicalWorker(physicalWorker),
		  m_physicalWorkerIndex(physicalWorkerIndex) {}

	virtual void execute(rts::JobContext &context)
	{
		*m_physicalWorker = context.isPhysicalWorkerExecution();
		*m_physicalWorkerIndex = context.physicalWorkerIndex();
	}

private:
	bool *m_physicalWorker;
	unsigned *m_physicalWorkerIndex;
};

class DeterministicJob : public rts::Job
{
public:
	DeterministicJob(unsigned index, unsigned *executions, unsigned *outputs)
		: m_index(index), m_executions(executions), m_outputs(outputs)
	{
	}

	virtual void execute(rts::JobContext &)
	{
		++m_executions[m_index];
		m_outputs[m_index] = (m_index * 2654435761u) ^ (m_index >> 3);
	}

private:
	unsigned m_index;
	unsigned *m_executions;
	unsigned *m_outputs;
};

int testBasicStartSubmitWaitShutdown()
{
	int result = 0;
	result |= check(rts::JOB_SYSTEM_PERFORMANCE_SCHEMA_VERSION == 1,
		"scheduler performance schema marker remains explicit");
	unsigned executions = 0;
	rts::JobSystemConfig config;
	config.workerCount = 1;
	config.queueCapacity = 8;
	config.scratchBytesPerWorker = 4096;
	config.pinWorkers = false;

	rts::JobSystem &system = rts::JobSystem::instance();
	result |= check(system.start(config), "one-worker job system starts");
	rts::JobGroup group = system.createGroup();
	result |= check(group.isValid(), "group creation succeeds");

	rts::Job *job = new CountJob(&executions);
	rts::JobHandle handle = system.trySubmit(job, rts::JOB_PRIORITY_NORMAL, group);
	if (!handle.isValid())
	{
		delete job;
	}
	result |= check(handle.isValid(), "job submission succeeds");
	result |= check(system.wait(group), "owner waits for group");
	result |= check(handle.isComplete(), "submitted handle completes");
	result |= check(handle.succeeded(), "submitted handle succeeds");
	result |= check(executions == 1, "submitted job executes exactly once");

#if defined(_MSC_VER) && _MSC_VER < 1300
	bool physicalWorker = true;
	unsigned physicalWorkerIndex = 0;
	rts::JobGroup identityGroup = system.createGroup();
	rts::Job *identityJob = new ExecutionIdentityJob(&physicalWorker,
		&physicalWorkerIndex);
	rts::JobHandle identityHandle = system.trySubmit(identityJob,
		rts::JOB_PRIORITY_NORMAL, identityGroup);
	if (!identityHandle.isValid()) delete identityJob;
	result |= check(identityHandle.isValid() && system.wait(identityGroup) &&
		!physicalWorker &&
		physicalWorkerIndex == rts::JOB_INVALID_PHYSICAL_WORKER_INDEX,
		"legacy inline execution has no physical-worker identity");
	system.resetMetrics();
	rts::JobGroup skippedGroup = system.createGroup();
	rts::JobHandle failedPrerequisite = system.trySubmit(new ContextFailJob,
		rts::JOB_PRIORITY_NORMAL, skippedGroup);
	unsigned skippedExecutions = 0;
	rts::Job *skippedJob = new CountJob(&skippedExecutions);
	rts::JobHandle skippedHandle = system.trySubmitAfter(skippedJob,
		rts::JOB_PRIORITY_NORMAL, skippedGroup, &failedPrerequisite, 1);
	if (!skippedHandle.isValid()) delete skippedJob;
	const rts::JobSystemMetrics skippedMetrics = system.metrics();
	result |= check(failedPrerequisite.isValid() && skippedHandle.isValid() &&
		failedPrerequisite.failed() && skippedHandle.failed() &&
		skippedExecutions == 0 && skippedMetrics.executedJobCount == 1 &&
		skippedMetrics.failedJobCount == 2 &&
		skippedMetrics.maximumActiveWorkers == 1,
		"legacy dependency skip is finalized without false callback execution");
	result |= check(system.waitWithoutOwnerHelp(skippedGroup, 0) &&
		system.metrics().waitCount == skippedMetrics.waitCount + 1,
		"legacy no-owner-help fence observes immediate inline completion");
	system.resetPerformanceMetrics();
	result |= check(system.metrics().workerBusyNanoseconds == 0 &&
		system.metrics().workerWaitNanoseconds == 0,
		"legacy direct lane exposes zero physical-worker timing");
#endif

	system.shutdown();
	result |= check(!system.isRunning(), "shutdown stops the job system");
	result |= check(system.start(config), "job system restarts for dependency validation");
	result |= check(system.metrics().submittedJobCount == 0 &&
		system.metrics().executedJobCount == 0,
		"restart resets scheduler-generation metrics");
	rts::JobGroup restartedGroup = system.createGroup();
	rts::Job *staleJob = new CountJob(&executions);
	rts::JobHandle staleDependent = system.trySubmitAfter(staleJob,
		rts::JOB_PRIORITY_NORMAL, restartedGroup, &handle, 1);
	if (!staleDependent.isValid())
	{
		delete staleJob;
	}
	result |= check(!staleDependent.isValid(),
		"dependency handles from a previous generation are rejected");
	rts::Job *freshJob = new CountJob(&executions);
	rts::JobHandle fresh = system.trySubmit(freshJob,
		rts::JOB_PRIORITY_NORMAL, restartedGroup);
	if (!fresh.isValid())
	{
		delete freshJob;
	}
	rts::JobHandle duplicates[2] = { fresh, fresh };
	rts::Job *duplicateJob = new CountJob(&executions);
	rts::JobHandle duplicateDependent = system.trySubmitAfter(duplicateJob,
		rts::JOB_PRIORITY_NORMAL, restartedGroup, duplicates, 2);
	if (!duplicateDependent.isValid())
	{
		delete duplicateJob;
	}
	result |= check(fresh.isValid() && !duplicateDependent.isValid(),
		"duplicate dependency handles are rejected");
	result |= check(system.wait(restartedGroup),
		"dependency validation group remains drainable");
	system.shutdown();
	rts::JobSystemConfig invalidConfig = config;
	invalidConfig.scratchBytesPerWorker = 0;
	result |= check(!system.start(invalidConfig),
		"zero scratch capacity is rejected consistently");
	invalidConfig = config;
	invalidConfig.workerPolicy = static_cast<rts::JobWorkerPolicy>(99);
	result |= check(!system.start(invalidConfig),
		"unknown worker policy is rejected consistently");
	return result;
}

int testDeterministicWorkerCounts()
{
	// The local lane keeps the worker-count matrix visible while replacing
	// direct starts above the local capacity with an explicit bounded request.
	const unsigned workerCounts[] = { 1, 2, 4, 8, 16, 0 };
	const unsigned jobCount = 256;
	unsigned reference[jobCount];
	unsigned index;
	unsigned workerIndex;
	for (index = 0; index < jobCount; ++index)
	{
		reference[index] = (index * 2654435761u) ^ (index >> 3);
	}

	int result = 0;
	for (workerIndex = 0;
		workerIndex < sizeof(workerCounts) / sizeof(workerCounts[0]);
		++workerIndex)
	{
		unsigned executions[jobCount] = { 0 };
		unsigned outputs[jobCount] = { 0 };
		rts::JobHandle handles[jobCount];
		rts::JobSystemConfig config;
		config.workerCount = workerCountForTest(workerCounts[workerIndex],
			isLocalCapacityLane());
		config.queueCapacity = jobCount;
		config.scratchBytesPerWorker = 4096;
		config.pinWorkers = false;

		rts::JobSystem &system = rts::JobSystem::instance();
		result |= check(system.start(config), "configured worker count starts");
#if defined(_MSC_VER) && _MSC_VER < 1300
		result |= check(system.workerCount() == 1,
			"VC6 reference adapter reports its single execution lane");
#else
		const unsigned expectedWorkerCount = workerCountForTest(
			workerCounts[workerIndex], isLocalCapacityLane());
		const bool workerCountMatches = isLocalCapacityLane() ?
			system.workerCount() == expectedWorkerCount :
			(workerCounts[workerIndex] == 0 ? system.workerCount() > 0 :
				system.workerCount() == expectedWorkerCount);
		result |= check(workerCountMatches,
			isLocalCapacityLane() ?
				"local lane uses its bounded worker request" :
				"configured worker count has no product cap");
#endif
		rts::JobGroup group = system.createGroup();
		for (index = 0; index < jobCount; ++index)
		{
			rts::Job *job = new DeterministicJob(index, executions, outputs);
			handles[index] = system.trySubmit(job, rts::JOB_PRIORITY_NORMAL, group);
			if (!handles[index].isValid())
			{
				delete job;
			}
			result |= check(handles[index].isValid(), "deterministic job accepted");
		}
		result |= check(system.wait(group), "deterministic group completes");
		result |= check(system.outstandingJobCount() == 0,
			"completed jobs release bounded capacity");
		for (index = 0; index < jobCount; ++index)
		{
			result |= check(handles[index].succeeded(), "deterministic handle succeeds");
			result |= check(executions[index] == 1, "job executes exactly once");
			result |= check(outputs[index] == reference[index],
				"worker count preserves deterministic output");
		}
		system.shutdown();
	}
	return result;
}

int testFlatRangePartitions()
{
	int result = 0;
	const unsigned sizes[] = { 0, 1, 7, 64, 257, 4097, 131071, UINT_MAX };
	const unsigned grains[] = { 0, 1, 32, 512, UINT_MAX };
	// These values exercise pure range arithmetic and do not start workers.
	const unsigned workers[] = { 0, 1, 2, 4, 8, 16, UINT_MAX };
	unsigned sizeIndex;
	unsigned grainIndex;
	unsigned workerIndex;
	for (sizeIndex = 0; sizeIndex < sizeof(sizes) / sizeof(sizes[0]); ++sizeIndex)
	for (grainIndex = 0; grainIndex < sizeof(grains) / sizeof(grains[0]); ++grainIndex)
	for (workerIndex = 0; workerIndex < sizeof(workers) / sizeof(workers[0]); ++workerIndex)
	{
		const unsigned size = sizes[sizeIndex];
		const unsigned count = rts::JobSystem::chooseRangeCount(size,
			grains[grainIndex], workers[workerIndex]);
		result |= check(size == 0 ? count == 0 : count > 0 && count <= size,
			"range count is bounded for empty, small, large and extreme inputs");
		if (size == 0) continue;
		result |= check(workers[workerIndex] > 1 || count == 1,
			"zero/one worker uses the serial reference partition");
		const unsigned sampleCount = count < 1024 ? count : 3;
		unsigned previousEnd = 0;
		for (unsigned sample = 0; sample < sampleCount; ++sample)
		{
			const unsigned index = count < 1024 ? sample :
				(sample == 0 ? 0 : (sample == 1 ? count / 2 : count - 1));
			rts::JobRange range;
			result |= check(rts::JobSystem::rangeForIndex(size, count, index, range),
				"every valid partition can be materialized without allocation");
			result |= check(range.begin < range.end && range.end <= size,
				"ranges remain nonempty and inside immutable snapshot bounds");
			if (count < 1024 || index == 0)
				result |= check(range.begin == previousEnd,
					"ordered ranges have no gaps or overlap");
			previousEnd = range.end;
		}
		result |= check(previousEnd == size, "last range covers the exact tail");
	}
	rts::JobRange invalid;
	result |= check(!rts::JobSystem::rangeForIndex(0, 0, 0, invalid) &&
		invalid.begin == 0 && invalid.end == 0 &&
		!rts::JobSystem::rangeForIndex(3, 4, 0, invalid) &&
		!rts::JobSystem::rangeForIndex(3, 3, 3, invalid),
		"invalid partition metadata fails closed with an empty output");
	result |= check(rts::JobSystem::chooseRangeCount(4096, 32, 16) == 64 &&
		rts::JobSystem::chooseRangeCount(UINT_MAX, 1, UINT_MAX) == UINT_MAX,
		"adaptive ranges scale beyond two workers without unsigned overflow");
	return result;
}

class FlatRangeJob : public rts::Job
{
public:
	FlatRangeJob(const rts::JobRange &range, unsigned *executions, unsigned *outputs)
		: m_range(range), m_executions(executions), m_outputs(outputs) {}
	virtual void execute(rts::JobContext &)
	{
		for (unsigned index = m_range.begin; index < m_range.end; ++index)
		{
			++m_executions[index];
			m_outputs[index] = (index * 2654435761u) ^ (index >> 3);
		}
	}
private:
	rts::JobRange m_range;
	unsigned *m_executions;
	unsigned *m_outputs;
};

int testFlatRangeKernelAndSaturationFallback()
{
	int result = 0;
	const unsigned itemCount = 4097;
	const unsigned maximumRanges = itemCount / 32;
	// The 16-worker case remains in the full matrix; local direct starts are
	// reduced to the explicit 12-worker capacity by workerCountForTest.
	const unsigned workers[] = { 1, 2, 4, 8, 16, 0 };
	for (unsigned workerIndex = 0; workerIndex < sizeof(workers) / sizeof(workers[0]); ++workerIndex)
	for (unsigned saturated = 0; saturated < 2; ++saturated)
	{
		rts::JobSystem &system = rts::JobSystem::instance();
		rts::JobSystemConfig config;
		config.workerCount = workerCountForTest(workers[workerIndex],
			isLocalCapacityLane());
		config.queueCapacity = saturated ? 1 : maximumRanges;
		config.scratchBytesPerWorker = 4096;
		config.pinWorkers = false;
		result |= check(system.start(config), "flat-range fixture starts");
		if (isLocalCapacityLane())
			result |= check(system.workerCount() <= kLocalCapacityWorkerLimit,
				"local flat-range fixture stays within worker capacity");
		unsigned executions[itemCount] = { 0 };
		unsigned outputs[itemCount] = { 0 };
		const unsigned rangeCount = rts::JobSystem::chooseRangeCount(itemCount, 32,
			system.workerCount());
		rts::JobGroup group = system.createGroup();
		rts::JobSubmission submissions[maximumRanges];
		rts::JobHandle handles[maximumRanges];
		unsigned allocated = 0;
		for (unsigned index = 0; index < rangeCount; ++index)
		{
			rts::JobRange range;
			if (!rts::JobSystem::rangeForIndex(itemCount, rangeCount, index, range)) break;
			submissions[index].job = new (std::nothrow) FlatRangeJob(range, executions, outputs);
			submissions[index].priority = rts::JOB_PRIORITY_FRAME_CRITICAL;
			if (submissions[index].job == 0) break;
			++allocated;
		}
		const bool accepted = allocated == rangeCount &&
			system.trySubmitBatch(submissions, rangeCount, group, handles);
		if (accepted)
		{
			result |= check(system.wait(group) && !group.failed(),
				"flat-range batch drains before snapshot storage is released");
		}
		else
		{
			for (unsigned release = 0; release < allocated; ++release)
				delete submissions[release].job;
			system.recordSerialFallback();
			for (unsigned serial = 0; serial < itemCount; ++serial)
			{
				++executions[serial];
				outputs[serial] = (serial * 2654435761u) ^ (serial >> 3);
			}
		}
		result |= check(accepted == (rangeCount <= config.queueCapacity),
			"bounded admission either accepts every range or leaves caller ownership");
		for (unsigned verify = 0; verify < itemCount; ++verify)
			result |= check(executions[verify] == 1 &&
				outputs[verify] == ((verify * 2654435761u) ^ (verify >> 3)),
				"flat-range and saturation fallback match serial output exactly once");
		result |= check(system.outstandingJobCount() == 0,
			"flat-range fixture retains no task after publication");
		system.shutdown();
	}
	return result;
}

int testAvailableCpuSetsAndOwnerReservations()
{
	int result = 0;
	rts::JobCpuSetInfo cpuSets[12];
	unsigned index;
	for (index = 0; index < 12; ++index)
	{
		cpuSets[index].id = 100 + index;
		cpuSets[index].efficiencyClass = index < 4 ? 3 : 1;
		cpuSets[index].group = 0;
		cpuSets[index].coreIndex = index / 2;
		cpuSets[index].logicalProcessorIndex = index;
	}
	cpuSets[8].parked = true;
	cpuSets[9].allocatedToOtherProcess = true;
	cpuSets[10].availableToProcess = false;
	cpuSets[11].group = 1;
	cpuSets[11].coreIndex = 0;
	unsigned owners[2] = { 0, 0 };
	unsigned workers[12] = { 0 };
	result |= check(rts::JobSystem::selectOwnerCpuSets(cpuSets, 12,
		rts::JOB_WORKER_POLICY_AUTO, 0, owners, 2) == 2 &&
		owners[0] == 100 && owners[1] == 102,
		"auto reserves separate high-performance physical cores for game and render");
	const unsigned count = rts::JobSystem::selectWorkerCpuSets(cpuSets, 12,
		rts::JOB_WORKER_POLICY_AUTO, 0, workers, 12);
	result |= check(count == 7, "auto derives workers from process-available CPU sets");
	for (index = 0; index < count; ++index)
		result |= check(workers[index] != 100 && workers[index] != 102 &&
			workers[index] != 108 && workers[index] != 109 && workers[index] != 110,
			"workers avoid owner reservations and unavailable, parked or allocated CPUs");
	result |= check(rts::JobSystem::selectWorkerCpuSets(cpuSets, 12,
		rts::JOB_WORKER_POLICY_ALL, 0, workers, 12) == 9 &&
		rts::JobSystem::selectOwnerCpuSets(cpuSets, 12,
			rts::JOB_WORKER_POLICY_ALL, 0, owners, 2) == 0,
		"all retains every eligible CPU without bypassing process availability");
	result |= check(rts::JobSystem::selectWorkerCpuSets(cpuSets, 12,
		rts::JOB_WORKER_POLICY_AUTO, 16, workers, 12) == 9 &&
		rts::JobSystem::chooseWorkerCount(9, rts::JOB_WORKER_POLICY_AUTO, 16) == 16,
		"explicit oversubscription preserves count but pins only to available CPUs");
	rts::JobCpuSetInfo pairedSmtCpuSets[16];
	unsigned pairedWorkers[8] = { 0 };
	for (index = 0; index < 16; ++index)
	{
		pairedSmtCpuSets[index].id = 200 + index;
		pairedSmtCpuSets[index].efficiencyClass = 1;
		pairedSmtCpuSets[index].group = 0;
		pairedSmtCpuSets[index].coreIndex = index / 2;
		pairedSmtCpuSets[index].logicalProcessorIndex = index;
	}
	const unsigned pairedWorkerCount = rts::JobSystem::selectWorkerCpuSets(
		pairedSmtCpuSets, 16, rts::JOB_WORKER_POLICY_AUTO, 8,
		pairedWorkers, 8);
	rts::JobMetricCounter pairedPhysicalMask = 0;
	bool pairedPhysicalMaskComplete = false;
	const unsigned pairedPhysicalCount =
		rts::JobSystem::summarizeSelectedPhysicalCores(pairedSmtCpuSets, 16,
			pairedWorkers, pairedWorkerCount, &pairedPhysicalMask,
			&pairedPhysicalMaskComplete);
	result |= check(pairedWorkerCount == 8 && pairedPhysicalCount == 8 &&
		pairedPhysicalMask == static_cast<rts::JobMetricCounter>(0xff) &&
		pairedPhysicalMaskComplete,
		"eight workers spread across eight paired-SMT physical cores");
	rts::JobCpuSetInfo wideCpuSets[128];
	unsigned wideWorkers[128] = { 0 };
	for (index = 0; index < 128; ++index)
	{
		wideCpuSets[index].id = 1000 + index;
		wideCpuSets[index].efficiencyClass = 1;
		wideCpuSets[index].group = index / 64;
		wideCpuSets[index].coreIndex = index % 64;
		wideCpuSets[index].logicalProcessorIndex = index % 64;
	}
	result |= check(rts::JobSystem::selectWorkerCpuSets(wideCpuSets, 96,
		rts::JOB_WORKER_POLICY_AUTO, 0, wideWorkers, 128) == 94 &&
		rts::JobSystem::chooseWorkerCount(96, rts::JOB_WORKER_POLICY_AUTO, 0) == 94,
		"96-LP auto topology reserves owners without a processor-group ceiling");
	const unsigned wideWorkerCount = rts::JobSystem::selectWorkerCpuSets(
		wideCpuSets, 128, rts::JOB_WORKER_POLICY_ALL, 0, wideWorkers, 128);
	rts::JobMetricCounter widePhysicalMask = 0;
	bool widePhysicalMaskComplete = true;
	const unsigned widePhysicalCount =
		rts::JobSystem::summarizeSelectedPhysicalCores(wideCpuSets, 128,
			wideWorkers, wideWorkerCount, &widePhysicalMask,
			&widePhysicalMaskComplete);
	result |= check(wideWorkerCount == 128 && widePhysicalCount == 128 &&
		widePhysicalMask == ~static_cast<rts::JobMetricCounter>(0) &&
		!widePhysicalMaskComplete,
		"128-LP topology reports an explicitly incomplete 64-bit core mask");
	for (index = 0; index < 12; ++index) cpuSets[index].availableToProcess = index < 2;
	result |= check(rts::JobSystem::selectOwnerCpuSets(cpuSets, 12,
		rts::JOB_WORKER_POLICY_AUTO, 0, owners, 2) == 1 &&
		rts::JobSystem::selectWorkerCpuSets(cpuSets, 12,
			rts::JOB_WORKER_POLICY_AUTO, 0, workers, 12) == 1 && workers[0] == 101,
		"two-CPU process affinity keeps one owner and one worker");
	cpuSets[0].availableToProcess = false;
	result |= check(rts::JobSystem::selectOwnerCpuSets(cpuSets, 12,
		rts::JOB_WORKER_POLICY_AUTO, 0, owners, 2) == 0 &&
		rts::JobSystem::selectWorkerCpuSets(cpuSets, 12,
			rts::JOB_WORKER_POLICY_AUTO, 0, workers, 12) == 1,
		"single eligible CPU shares the serial/one-worker fallback without reservation");
	cpuSets[1].availableToProcess = false;
	result |= check(rts::JobSystem::selectWorkerCpuSets(cpuSets, 12,
		rts::JOB_WORKER_POLICY_ALL, 16, workers, 12) == 0,
		"no available CPU never selects an invalid affinity target");
	return result;
}

int testOwnerRoleAndLazyRestartBasics()
{
	int result = 0;
	rts::JobSystem &system = rts::JobSystem::instance();
	rts::JobSystemConfig config;
	config.workerCount = 1;
	config.queueCapacity = 8;
	config.scratchBytesPerWorker = 4096;
	config.pinWorkers = false;
	result |= check(system.start(config), "owner-role fixture starts");
	result |= check(system.registerCurrentThread(rts::JOB_OWNER_GAME) &&
		system.registerCurrentThread(rts::JOB_OWNER_GAME) &&
		system.isCurrentThread(rts::JOB_OWNER_GAME),
		"game owner registration is explicit and idempotent");
	result |= check(!system.registerCurrentThread(rts::JOB_OWNER_RENDER) &&
		!system.registerCurrentThread(static_cast<rts::JobOwnerRole>(-1)),
		"one thread cannot claim conflicting owner roles or invalid roles");
	system.shutdown();
	result |= check(!system.ensureStarted() && !system.isRunning() &&
		system.isCurrentThread(rts::JOB_OWNER_GAME),
		"shutdown disables lazy restart without destroying owner identities");
	result |= check(system.unregisterCurrentThread(rts::JOB_OWNER_GAME) &&
		!system.isCurrentThread(rts::JOB_OWNER_GAME) &&
		!system.unregisterCurrentThread(rts::JOB_OWNER_GAME),
		"same-thread owner release remains available after compute shutdown");
	result |= check(system.registerCurrentThread(rts::JOB_OWNER_IO) &&
		!system.isRunning() && system.unregisterCurrentThread(rts::JOB_OWNER_IO),
		"service ownership alone never resurrects compute workers");
	result |= check(system.start(config) && system.ensureStarted(),
		"explicit startup alone starts a new scheduler generation");
	system.shutdown();
	return result;
}

#if !defined(_MSC_VER) || _MSC_VER >= 1300
class Gate
{
public:
	Gate() : m_entered(false), m_open(false) {}

	bool waitForEntry()
	{
		std::unique_lock<std::mutex> lock(m_mutex);
		return m_condition.wait_for(lock, std::chrono::seconds(5),
			[this]() { return m_entered; });
	}

	void waitUntilOpen()
	{
		std::unique_lock<std::mutex> lock(m_mutex);
		m_entered = true;
		m_condition.notify_all();
		m_condition.wait(lock, [this]() { return m_open; });
	}

	void open()
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_open = true;
		m_condition.notify_all();
	}

private:
	std::mutex m_mutex;
	std::condition_variable m_condition;
	bool m_entered;
	bool m_open;
};

class GateJob : public rts::Job
{
public:
	explicit GateJob(Gate *gate) : m_gate(gate) {}
	virtual void execute(rts::JobContext &) { m_gate->waitUntilOpen(); }
private:
	Gate *m_gate;
};

class SignalledExecutionIdentityJob : public rts::Job
{
public:
	SignalledExecutionIdentityJob(std::atomic<bool> *executed,
		bool *physicalWorker, unsigned *physicalWorkerIndex)
		: m_executed(executed), m_physicalWorker(physicalWorker),
		  m_physicalWorkerIndex(physicalWorkerIndex) {}

	virtual void execute(rts::JobContext &context)
	{
		*m_physicalWorker = context.isPhysicalWorkerExecution();
		*m_physicalWorkerIndex = context.physicalWorkerIndex();
		m_executed->store(true, std::memory_order_release);
	}

private:
	std::atomic<bool> *m_executed;
	bool *m_physicalWorker;
	unsigned *m_physicalWorkerIndex;
};

class BlockingExecutionIdentityJob : public rts::Job
{
public:
	BlockingExecutionIdentityJob(Gate *gate, bool *physicalWorker,
		unsigned *physicalWorkerIndex, std::thread::id *threadId)
		: m_gate(gate), m_physicalWorker(physicalWorker),
		  m_physicalWorkerIndex(physicalWorkerIndex), m_threadId(threadId) {}

	virtual void execute(rts::JobContext &context)
	{
		*m_physicalWorker = context.isPhysicalWorkerExecution();
		*m_physicalWorkerIndex = context.physicalWorkerIndex();
		*m_threadId = std::this_thread::get_id();
		m_gate->waitUntilOpen();
	}

private:
	Gate *m_gate;
	bool *m_physicalWorker;
	unsigned *m_physicalWorkerIndex;
	std::thread::id *m_threadId;
};

bool runPhysicalWorkerIdentityWave(rts::JobSystem &system,
	bool physicalWorkers[4], unsigned physicalWorkerIndices[4],
	std::thread::id threadIds[4])
{
	Gate gates[4];
	rts::JobHandle handles[4];
	rts::JobGroup group = system.createGroup();
	bool submitted = true;
	bool anySubmitted = false;
	bool entered = true;
	unsigned index;

	for (index = 0; index != 4; ++index)
	{
		physicalWorkers[index] = false;
		physicalWorkerIndices[index] = rts::JOB_INVALID_PHYSICAL_WORKER_INDEX;
		threadIds[index] = std::thread::id();
		rts::Job *job = new BlockingExecutionIdentityJob(&gates[index],
			&physicalWorkers[index], &physicalWorkerIndices[index],
			&threadIds[index]);
		handles[index] = system.trySubmit(job, rts::JOB_PRIORITY_NORMAL, group);
		if (!handles[index].isValid())
		{
			delete job;
			submitted = false;
		}
		else
		{
			anySubmitted = true;
		}
	}

	if (submitted)
	{
		for (index = 0; index != 4; ++index)
			entered = gates[index].waitForEntry() && entered;
	}
	for (index = 0; index != 4; ++index) gates[index].open();
	if (anySubmitted) entered = system.wait(group) && entered;
	return submitted && entered;
}

int testExecutionScopedPhysicalWorkerIdentity()
{
	int result = 0;
	rts::JobSystemConfig config;
	config.workerCount = 1;
	config.queueCapacity = 8;
	config.scratchBytesPerWorker = 4096;
	config.pinWorkers = false;

	rts::JobSystem &system = rts::JobSystem::instance();
	const bool started = system.start(config);
	result |= check(started, "physical-worker identity job system starts");
	if (!started) return result;

	bool physicalWorker = false;
	unsigned physicalWorkerIndex = rts::JOB_INVALID_PHYSICAL_WORKER_INDEX;
	std::atomic<bool> executed(false);
	rts::JobGroup physicalGroup = system.createGroup();
	rts::Job *physicalJob = new SignalledExecutionIdentityJob(&executed,
		&physicalWorker, &physicalWorkerIndex);
	rts::JobHandle physicalHandle = system.trySubmit(physicalJob,
		rts::JOB_PRIORITY_NORMAL, physicalGroup);
	if (!physicalHandle.isValid()) delete physicalJob;
	const std::chrono::steady_clock::time_point physicalDeadline =
		std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (!executed.load(std::memory_order_acquire) &&
		std::chrono::steady_clock::now() < physicalDeadline)
		std::this_thread::yield();
	const bool workerExecuted = executed.load(std::memory_order_acquire);
	result |= check(physicalHandle.isValid() && workerExecuted,
		"physical worker executes identity probe without owner help");
	result |= check(workerExecuted && physicalWorker && physicalWorkerIndex == 0,
		"physical worker exposes its stable zero-based scheduler index");
	if (physicalHandle.isValid())
		result |= check(system.wait(physicalGroup),
			"physical-worker identity probe drains");

	Gate blocker;
	rts::JobGroup blockerGroup = system.createGroup();
	rts::Job *blockerJob = new GateJob(&blocker);
	rts::JobHandle blockerHandle = system.trySubmit(blockerJob,
		rts::JOB_PRIORITY_FRAME_CRITICAL, blockerGroup);
	if (!blockerHandle.isValid()) delete blockerJob;
	const bool workerBlocked = blockerHandle.isValid() && blocker.waitForEntry();
	result |= check(workerBlocked,
		"physical worker is held before the owner-help identity probe");

	bool ownerPhysicalWorker = true;
	unsigned ownerPhysicalWorkerIndex = 0;
	rts::JobGroup ownerGroup = system.createGroup();
	rts::Job *ownerJob = new ExecutionIdentityJob(&ownerPhysicalWorker,
		&ownerPhysicalWorkerIndex);
	rts::JobHandle ownerHandle = system.trySubmit(ownerJob,
		rts::JOB_PRIORITY_NORMAL, ownerGroup);
	if (!ownerHandle.isValid()) delete ownerJob;
	const bool ownerWaited = ownerHandle.isValid() && system.wait(ownerGroup);
	result |= check(ownerWaited,
		"registered scheduler owner executes queued identity probe while worker is held");
	result |= check(ownerWaited && !ownerPhysicalWorker &&
		ownerPhysicalWorkerIndex == rts::JOB_INVALID_PHYSICAL_WORKER_INDEX,
		"owner help cannot impersonate a physical scheduler worker");

	if (blockerHandle.isValid())
	{
		blocker.open();
		result |= check(system.wait(blockerGroup),
			"held physical worker drains after identity probe");
	}
	system.shutdown();

	config.workerCount = 4;
	config.queueCapacity = 32;
	const bool multiWorkerStarted = system.start(config);
	result |= check(multiWorkerStarted && system.workerCount() == 4,
		"multi-worker identity fixture starts four physical workers");
	if (multiWorkerStarted && system.workerCount() == 4)
	{
		bool firstPhysical[4];
		bool secondPhysical[4];
		unsigned firstIndices[4];
		unsigned secondIndices[4];
		std::thread::id firstThreads[4];
		std::thread::id secondThreads[4];
		const bool firstWave = runPhysicalWorkerIdentityWave(system,
			firstPhysical, firstIndices, firstThreads);
		const bool secondWave = runPhysicalWorkerIdentityWave(system,
			secondPhysical, secondIndices, secondThreads);
		result |= check(firstWave && secondWave,
			"all physical workers execute both identity waves without owner help");

		bool validRange = firstWave && secondWave;
		bool distinct = firstWave && secondWave;
		bool stableByThread = firstWave && secondWave;
		for (unsigned first = 0; first != 4; ++first)
		{
			validRange = validRange && firstPhysical[first] &&
				secondPhysical[first] && firstIndices[first] < 4 &&
				secondIndices[first] < 4;
			for (unsigned other = first + 1; other != 4; ++other)
			{
				distinct = distinct && firstIndices[first] != firstIndices[other] &&
					secondIndices[first] != secondIndices[other];
			}

			bool foundThread = false;
			for (unsigned second = 0; second != 4; ++second)
			{
				if (firstThreads[first] == secondThreads[second])
				{
					foundThread = true;
					stableByThread = stableByThread &&
						firstIndices[first] == secondIndices[second];
					break;
				}
			}
			stableByThread = stableByThread && foundThread;
		}
		result |= check(validRange,
			"physical worker indices remain within the configured worker range");
		result |= check(distinct,
			"simultaneously occupied physical workers expose distinct indices");
		result |= check(stableByThread,
			"each physical scheduler thread retains its index across jobs");
	}
	system.shutdown();
	return result;
}

class OrderedJob : public rts::Job
{
public:
	OrderedJob(std::atomic<unsigned> *sequence, unsigned *order)
		: m_sequence(sequence), m_order(order) {}
	virtual void execute(rts::JobContext &)
	{
		*m_order = m_sequence->fetch_add(1, std::memory_order_relaxed) + 1;
	}
private:
	std::atomic<unsigned> *m_sequence;
	unsigned *m_order;
};

class LocalPriorityPublisherJob : public rts::Job
{
public:
	LocalPriorityPublisherJob(rts::JobSystem *system,
		const rts::JobGroup &group, Gate *gate,
		std::atomic<unsigned> *sequence, unsigned *backgroundOrder,
		bool *accepted)
		: m_system(system), m_group(group), m_gate(gate),
		  m_sequence(sequence), m_backgroundOrder(backgroundOrder),
		  m_accepted(accepted) {}

	virtual void execute(rts::JobContext &context)
	{
		rts::Job *job = new OrderedJob(m_sequence, m_backgroundOrder);
		const rts::JobHandle handle = m_system->trySubmit(job,
			rts::JOB_PRIORITY_BACKGROUND, m_group);
		*m_accepted = handle.isValid();
		if (!handle.isValid())
		{
			delete job;
			context.fail();
		}
		m_gate->waitUntilOpen();
	}

private:
	rts::JobSystem *m_system;
	rts::JobGroup m_group;
	Gate *m_gate;
	std::atomic<unsigned> *m_sequence;
	unsigned *m_backgroundOrder;
	bool *m_accepted;
};

class BusyJob : public rts::Job
{
public:
	explicit BusyJob(std::atomic<unsigned> *executions)
		: m_executions(executions) {}
	virtual void execute(rts::JobContext &)
	{
		const std::chrono::steady_clock::time_point until =
			std::chrono::steady_clock::now() + std::chrono::milliseconds(2);
		while (std::chrono::steady_clock::now() < until)
		{
			std::this_thread::yield();
		}
		m_executions->fetch_add(1, std::memory_order_relaxed);
	}
private:
	std::atomic<unsigned> *m_executions;
};

class PriorityCountJob : public rts::Job
{
public:
	PriorityCountJob(std::atomic<unsigned> *counts, unsigned priority)
		: m_counts(counts), m_priority(priority) {}
	virtual void execute(rts::JobContext &)
	{
		m_counts[m_priority].fetch_add(1, std::memory_order_relaxed);
	}
private:
	std::atomic<unsigned> *m_counts;
	unsigned m_priority;
};

class FanOutJob : public rts::Job
{
public:
	FanOutJob(rts::JobSystem *system, const rts::JobGroup &group,
		std::atomic<unsigned> *executions, unsigned childCount, bool *submitted,
		Gate *startGate)
		: m_system(system), m_group(group), m_executions(executions),
		  m_childCount(childCount), m_submitted(submitted), m_startGate(startGate) {}

	virtual void execute(rts::JobContext &context)
	{
		m_startGate->waitUntilOpen();
		for (unsigned index = 0; index < m_childCount; ++index)
		{
			rts::Job *job = new BusyJob(m_executions);
			rts::JobHandle handle = m_system->trySubmit(job,
				rts::JOB_PRIORITY_NORMAL, m_group);
			if (!handle.isValid())
			{
				delete job;
				context.fail();
				return;
			}
		}
		*m_submitted = true;
	}

private:
	rts::JobSystem *m_system;
	rts::JobGroup m_group;
	std::atomic<unsigned> *m_executions;
	unsigned m_childCount;
	bool *m_submitted;
	Gate *m_startGate;
};

class GateOrderedJob : public rts::Job
{
public:
	GateOrderedJob(Gate *gate, std::atomic<unsigned> *sequence, unsigned *order)
		: m_gate(gate), m_sequence(sequence), m_order(order) {}
	virtual void execute(rts::JobContext &)
	{
		m_gate->waitUntilOpen();
		*m_order = m_sequence->fetch_add(1, std::memory_order_relaxed) + 1;
	}
private:
	Gate *m_gate;
	std::atomic<unsigned> *m_sequence;
	unsigned *m_order;
};

class FailJob : public rts::Job
{
public:
	virtual void execute(rts::JobContext &context) { context.fail(); }
};

class MarkJob : public rts::Job
{
public:
	explicit MarkJob(bool *executed) : m_executed(executed) {}
	virtual void execute(rts::JobContext &) { *m_executed = true; }
private:
	bool *m_executed;
};

class LifetimeJob : public rts::Job
{
public:
	explicit LifetimeJob(std::atomic<unsigned> *destructions) : m_destructions(destructions) {}
	virtual ~LifetimeJob() { m_destructions->fetch_add(1, std::memory_order_relaxed); }
	virtual void execute(rts::JobContext &) {}
private:
	std::atomic<unsigned> *m_destructions;
};

class BlockingLifetimeJob : public rts::Job
{
public:
	BlockingLifetimeJob(Gate *gate, std::atomic<unsigned> *executions,
		std::atomic<unsigned> *destructions)
		: m_gate(gate), m_executions(executions), m_destructions(destructions) {}
	virtual ~BlockingLifetimeJob()
	{
		m_destructions->fetch_add(1, std::memory_order_relaxed);
	}
	virtual void execute(rts::JobContext &)
	{
		m_executions->fetch_add(1, std::memory_order_relaxed);
		m_gate->waitUntilOpen();
	}
private:
	Gate *m_gate;
	std::atomic<unsigned> *m_executions;
	std::atomic<unsigned> *m_destructions;
};

class WorkerWaitJob : public rts::Job
{
public:
	WorkerWaitJob(rts::JobSystem *system, const rts::JobGroup &group,
		bool *waitResult, bool *childExecuted)
		: m_system(system), m_group(group), m_waitResult(waitResult),
		  m_childExecuted(childExecuted) {}
	virtual void execute(rts::JobContext &context)
	{
		rts::Job *child = new MarkJob(m_childExecuted);
		rts::JobHandle handle = m_system->trySubmit(child,
			rts::JOB_PRIORITY_NORMAL, m_group);
		if (!handle.isValid())
		{
			delete child;
			context.fail();
			return;
		}
		*m_waitResult = m_system->wait(handle);
	}
private:
	rts::JobSystem *m_system;
	rts::JobGroup m_group;
	bool *m_waitResult;
	bool *m_childExecuted;
};

struct CompletionRecord
{
	CompletionRecord() : calls(0), succeeded(false), cancelled(false), ownerThread() {}
	unsigned calls;
	bool succeeded;
	bool cancelled;
	std::thread::id ownerThread;
};

class CompletionProbe : public rts::OwnerCompletion
{
public:
	explicit CompletionProbe(CompletionRecord *record) : m_record(record) {}
	virtual void complete(bool succeeded, bool cancelled)
	{
		++m_record->calls;
		m_record->succeeded = succeeded;
		m_record->cancelled = cancelled;
		m_record->ownerThread = std::this_thread::get_id();
	}
private:
	CompletionRecord *m_record;
};

int testPrioritiesAndWorkStealing()
{
	int result = 0;
	rts::JobSystem &system = rts::JobSystem::instance();
	{
		rts::JobSystemConfig config;
		config.workerCount = 1;
		config.queueCapacity = 8;
		config.scratchBytesPerWorker = 4096;
		config.pinWorkers = false;
		result |= check(system.start(config), "priority test starts");
		rts::JobGroup group = system.createGroup();
		Gate gate;
		std::atomic<unsigned> sequence(0);
		unsigned backgroundOrder = 0;
		unsigned criticalOrder = 0;
		rts::JobHandle blocker = system.trySubmit(new GateJob(&gate),
			rts::JOB_PRIORITY_NORMAL, group);
		result |= check(blocker.isValid(), "priority blocker accepted");
		result |= check(gate.waitForEntry(), "priority blocker enters before timeout");
		OrderedJob *backgroundJob = new OrderedJob(&sequence, &backgroundOrder);
		rts::JobHandle background = system.trySubmit(
			backgroundJob,
			rts::JOB_PRIORITY_BACKGROUND, group);
		rts::JobHandle critical = system.trySubmit(
			new OrderedJob(&sequence, &criticalOrder),
			rts::JOB_PRIORITY_FRAME_CRITICAL, group);
		result |= check(background.isValid() && critical.isValid(),
			"priority jobs accepted");
		result |= check(system.tryPromote(backgroundJob,
			rts::JOB_PRIORITY_FRAME_CRITICAL),
			"queued job can be promoted to a frame-critical lane");
		gate.open();
		/*
		 * This ordering contract is for the single physical worker.  The
		 * owner-thread fallback used by wait(group) is a second consumer: it
		 * can claim the promoted record and be preempted before execute(),
		 * allowing the physical worker to start the older critical record.
		 */
		result |= check(system.waitWithoutOwnerHelp(group, 5000),
			"priority group completes");
		result |= check(backgroundOrder == 1 && criticalOrder == 2,
			"promoted job runs before existing frame-critical work");
		system.shutdown();
	}

	{
		rts::JobSystemConfig config;
		config.workerCount = 1;
		config.queueCapacity = 8;
		config.scratchBytesPerWorker = 4096;
		config.pinWorkers = false;
		result |= check(system.start(config),
			"cross-source priority test starts");
		rts::JobGroup group = system.createGroup();
		Gate publisherGate;
		std::atomic<unsigned> sequence(0);
		unsigned backgroundOrder = 0;
		unsigned criticalOrder = 0;
		bool localBackgroundAccepted = false;
		rts::JobHandle publisher = system.trySubmit(
			new LocalPriorityPublisherJob(&system, group, &publisherGate,
				&sequence, &backgroundOrder, &localBackgroundAccepted),
			rts::JOB_PRIORITY_NORMAL, group);
		result |= check(publisher.isValid() && publisherGate.waitForEntry() &&
			localBackgroundAccepted,
			"worker publishes local background work before release");
		rts::JobHandle critical = system.trySubmit(
			new OrderedJob(&sequence, &criticalOrder),
			rts::JOB_PRIORITY_FRAME_CRITICAL, group);
		result |= check(critical.isValid(),
			"owner publishes injected frame-critical work before release");
		publisherGate.open();
		const std::chrono::steady_clock::time_point completionDeadline =
			std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (!group.isComplete() &&
			std::chrono::steady_clock::now() < completionDeadline)
		{
			std::this_thread::yield();
		}
		const bool completedWithoutOwnerHelp = group.isComplete();
		result |= check(completedWithoutOwnerHelp,
			"physical worker completes cross-source priority group");
		if (!completedWithoutOwnerHelp)
			system.wait(group);
		result |= check(criticalOrder == 1 && backgroundOrder == 2,
			"injected frame-critical work outranks worker-local background work");
		system.shutdown();
	}

	{
		const unsigned childCount = 96;
		rts::JobSystemConfig config;
		config.workerCount = 4;
		config.queueCapacity = childCount + 8;
		config.scratchBytesPerWorker = 4096;
		config.pinWorkers = false;
		result |= check(system.start(config), "stealing test starts");
		rts::JobGroup group = system.createGroup();
		std::atomic<unsigned> executions(0);
		bool submitted = false;
		Gate startGate;
		rts::JobHandle fanOut = system.trySubmit(
			new FanOutJob(&system, group, &executions, childCount, &submitted,
				&startGate),
			rts::JOB_PRIORITY_NORMAL, group);
		result |= check(fanOut.isValid(), "worker fan-out accepted");
		result |= check(startGate.waitForEntry(), "fan-out job enters before timeout");
		startGate.open();
		result |= check(system.wait(group), "fan-out group completes");
		const rts::JobSystemMetrics metrics = system.metrics();
		result |= check(submitted, "worker submitted every child");
		result |= check(executions.load(std::memory_order_relaxed) == childCount,
			"fan-out children execute exactly once");
		result |= check(metrics.stealCount > 0,
			"worker-local fan-out is stolen by peer workers");
		result |= check(metrics.executedJobCount >= childCount + 1,
			"execution telemetry includes fan-out work");
		result |= check(metrics.maximumActiveWorkers >= 3,
			"fan-out workload executes concurrently beyond two workers");
		system.shutdown();
	}

	{
		rts::JobSystemConfig config;
		config.workerCount = 1;
		config.queueCapacity = 8;
		config.scratchBytesPerWorker = 4096;
		config.pinWorkers = false;
		result |= check(system.start(config), "promotion-fault test starts");
		rts::JobGroup group = system.createGroup();
		Gate gate;
		bool queuedJobExecuted = false;
		rts::JobHandle blocker = system.trySubmit(new GateJob(&gate),
			rts::JOB_PRIORITY_NORMAL, group);
		result |= check(gate.waitForEntry(), "promotion-fault blocker enters before timeout");
		rts::Job *queuedJob = new MarkJob(&queuedJobExecuted);
		rts::JobHandle queued = system.trySubmit(queuedJob,
			rts::JOB_PRIORITY_BACKGROUND, group);
		result |= check(blocker.isValid() && queued.isValid(),
			"promotion-fault jobs accepted");
#if defined(RTS_BUILD_CORE_EXTRAS)
		rts_job_system_set_test_fault(JOB_SYSTEM_TEST_FAIL_PROMOTION_PUSH, 1);
#endif
		result |= check(!system.tryPromote(queuedJob,
			rts::JOB_PRIORITY_FRAME_CRITICAL),
			"promotion allocation failure preserves the source queue");
		gate.open();
		result |= check(system.wait(group),
			"promotion allocation failure leaves the group drainable");
		result |= check(queued.succeeded() && queuedJobExecuted,
			"promotion allocation failure does not strand the job");
		system.shutdown();
	}
	return result;
}

int testDependenciesContinuationsAndFailurePropagation()
{
	int result = 0;
	rts::JobSystem &system = rts::JobSystem::instance();
	rts::JobSystemConfig config;
	config.workerCount = 4;
	config.queueCapacity = 32;
	config.scratchBytesPerWorker = 4096;
	config.pinWorkers = false;
	result |= check(system.start(config), "dependency test starts");

	{
		rts::JobGroup group = system.createGroup();
		Gate gate;
		std::atomic<unsigned> sequence(0);
		unsigned prerequisiteOrder = 0;
		unsigned dependentOrder = 0;
		unsigned continuationOrder = 0;
		rts::JobHandle prerequisite = system.trySubmit(
			new GateOrderedJob(&gate, &sequence, &prerequisiteOrder),
			rts::JOB_PRIORITY_NORMAL, group);
		result |= check(prerequisite.isValid(), "prerequisite accepted");
		result |= check(gate.waitForEntry(), "dependency prerequisite enters before timeout");
		rts::Job *dependentJob = new OrderedJob(&sequence, &dependentOrder);
		rts::JobHandle dependent = system.trySubmitAfter(dependentJob,
			rts::JOB_PRIORITY_FRAME_CRITICAL, group, &prerequisite, 1);
		if (!dependent.isValid())
		{
			delete dependentJob;
		}
		result |= check(dependent.isValid(), "dependent accepted");
		result |= check(!dependent.isComplete(), "dependency blocks execution");
		rts::Job *continuationJob = new OrderedJob(&sequence, &continuationOrder);
		rts::JobHandle continuation = system.then(dependent, continuationJob,
			rts::JOB_PRIORITY_NORMAL, group);
		if (!continuation.isValid())
		{
			delete continuationJob;
		}
		result |= check(continuation.isValid(), "continuation accepted");
		gate.open();
		result |= check(system.wait(group), "dependency chain completes");
		result |= check(prerequisiteOrder == 1 && dependentOrder == 2 &&
			continuationOrder == 3, "dependencies release in chain order");
		result |= check(prerequisite.succeeded() && dependent.succeeded() &&
			continuation.succeeded(), "dependency chain succeeds");
	}

	{
		rts::JobGroup group = system.createGroup();
		bool dependentExecuted = false;
		rts::JobHandle failure = system.trySubmit(new FailJob,
			rts::JOB_PRIORITY_NORMAL, group);
		rts::Job *dependentJob = new MarkJob(&dependentExecuted);
		rts::JobHandle dependent = system.trySubmitAfter(dependentJob,
			rts::JOB_PRIORITY_NORMAL, group, &failure, 1);
		if (!dependent.isValid())
		{
			delete dependentJob;
		}
		result |= check(dependent.isValid(), "failure dependent accepted");
		result |= check(system.wait(group), "failed dependency group completes");
		result |= check(failure.failed(), "explicit job failure is recorded");
		result |= check(dependent.failed() && !dependentExecuted,
			"dependency failure propagates without executing child");
		result |= check(group.failed(), "group records propagated failure");
	}

	{
		rts::JobGroup group = system.createGroup();
		Gate gate;
		bool continuationExecuted = false;
		rts::JobHandle prerequisite = system.trySubmit(new GateJob(&gate),
			rts::JOB_PRIORITY_NORMAL, group);
		result |= check(gate.waitForEntry(), "continuation prerequisite enters before timeout");
		{
			rts::Job *continuationJob = new MarkJob(&continuationExecuted);
			rts::JobHandle discarded = system.then(prerequisite,
				continuationJob, rts::JOB_PRIORITY_NORMAL, group);
			if (!discarded.isValid())
			{
				delete continuationJob;
			}
			result |= check(discarded.isValid(),
				"fire-and-forget continuation accepted");
		}
		gate.open();
		result |= check(system.wait(group),
			"discarded continuation handle does not strand its group");
		result |= check(continuationExecuted,
			"fire-and-forget continuation executes");
	}

	system.shutdown();
	return result;
}

int testWideWorkerPriorityThroughput()
{
	int result = 0;
	rts::JobSystem &system = rts::JobSystem::instance();
	const unsigned fullWorkerCounts[2] = { 16, 32 };
	const unsigned localWorkerCounts[3] = { 4, 8, 12 };
	const unsigned *workerCounts = isLocalCapacityLane() ?
		localWorkerCounts : fullWorkerCounts;
	const unsigned configurationCount = isLocalCapacityLane() ? 3 : 2;
	const unsigned jobsPerPriority = 256;
	for (unsigned configuration = 0; configuration < configurationCount;
		++configuration)
	{
		rts::JobSystemConfig config;
		config.workerCount = workerCounts[configuration];
		config.queueCapacity = jobsPerPriority * rts::JOB_PRIORITY_COUNT + 16;
		config.scratchBytesPerWorker = 4096;
		config.pinWorkers = false;
		result |= check(system.start(config),
			"wide priority-throughput scheduler starts");
		if (isLocalCapacityLane())
			result |= check(system.workerCount() <= kLocalCapacityWorkerLimit,
				"local priority-throughput fixture stays within worker capacity");
		rts::JobGroup group = system.createGroup();
		std::atomic<unsigned> counts[rts::JOB_PRIORITY_COUNT];
		for (unsigned priority = 0; priority < rts::JOB_PRIORITY_COUNT; ++priority)
			counts[priority].store(0, std::memory_order_relaxed);
		bool accepted = true;
		for (unsigned index = 0; index < jobsPerPriority && accepted; ++index)
		{
			for (unsigned priority = 0; priority < rts::JOB_PRIORITY_COUNT; ++priority)
			{
				rts::Job *job = new PriorityCountJob(counts, priority);
				const rts::JobHandle handle = system.trySubmit(job,
					static_cast<rts::JobPriority>(priority), group);
				if (!handle.isValid())
				{
					delete job;
					accepted = false;
					break;
				}
			}
		}
		result |= check(accepted,
			"wide priority-throughput workload admits every priority lane");
		const bool completed = accepted && system.waitWithoutOwnerHelp(group, 5000);
		result |= check(completed,
			"wide priority-throughput workload completes on physical workers");
		for (unsigned priority = 0; priority < rts::JOB_PRIORITY_COUNT; ++priority)
		{
			result |= check(counts[priority].load(std::memory_order_relaxed) ==
				jobsPerPriority,
				"wide priority-throughput lane executes every job exactly once");
		}
		const rts::JobSystemMetrics metrics = system.metrics();
		result |= check(metrics.ownerHelpCount == 0 &&
			metrics.executedJobCount == jobsPerPriority * rts::JOB_PRIORITY_COUNT,
			"wide priority-throughput telemetry excludes owner execution");
		if (!completed) system.cancel(group);
		system.wait(group);
		system.shutdown();
	}
	return result;
}

int testCurrentWorkerWaitAccounting()
{
	int result = 0;
	rts::JobSystem &system = rts::JobSystem::instance();
	rts::JobSystemConfig config;
	config.workerCount = 1;
	config.queueCapacity = 8;
	config.scratchBytesPerWorker = 4096;
	config.pinWorkers = false;
	result |= check(system.start(config),
		"current-wait telemetry scheduler starts");
	const std::chrono::steady_clock::time_point deadline =
		std::chrono::steady_clock::now() + std::chrono::seconds(5);
	rts::JobSystemMetrics beforeReset;
	do
	{
		beforeReset = system.metrics();
		if (beforeReset.workerWaitSampleCount != 0 &&
			beforeReset.workerWaitNanoseconds != 0) break;
		std::this_thread::yield();
	} while (std::chrono::steady_clock::now() < deadline);
	result |= check(beforeReset.workerWaitSampleCount == 1 &&
		beforeReset.workerWaitNanoseconds >=
			beforeReset.maximumWorkerWaitNanoseconds,
		"snapshot includes a currently sleeping physical worker");
	system.resetPerformanceMetrics();
	rts::JobSystemMetrics afterReset = system.metrics();
	result |= check(afterReset.workerBusySampleCount == 0 &&
		afterReset.workerBusyNanoseconds == 0 &&
		afterReset.workerWaitSampleCount == 1 &&
		afterReset.workerWaitNanoseconds >=
			afterReset.maximumWorkerWaitNanoseconds,
		"linearizable reset rebases rather than erases an open worker wait");
	system.shutdown();
	const rts::JobSystemMetrics afterShutdown = system.metrics();
	result |= check(afterShutdown.workerWaitSampleCount >= 1 &&
		afterShutdown.workerWaitNanoseconds >=
			afterShutdown.maximumWorkerWaitNanoseconds,
		"per-worker telemetry remains observable after worker shutdown");
	return result;
}

int testCancellationOwnerHelpingAndCompletions()
{
	int result = 0;
	rts::JobSystem &system = rts::JobSystem::instance();
	rts::JobSystemConfig config;
	config.workerCount = 1;
	config.queueCapacity = 32;
	config.scratchBytesPerWorker = 4096;
	config.pinWorkers = false;
	result |= check(system.start(config), "owner-help test starts");
	const std::thread::id ownerThread = std::this_thread::get_id();

	{
		system.resetMetrics();
		rts::JobGroup group = system.createGroup();
		Gate gate;
		bool queuedJobExecuted = false;
		rts::JobHandle blocker = system.trySubmit(new GateJob(&gate),
			rts::JOB_PRIORITY_NORMAL, group);
		result |= check(blocker.isValid() && gate.waitForEntry(),
			"no-owner-help blocker enters before timeout");
		rts::JobHandle queued = system.trySubmit(
			new MarkJob(&queuedJobExecuted),
			rts::JOB_PRIORITY_FRAME_CRITICAL, group);
		const rts::JobMetricCounter ownerHelpBefore =
			system.metrics().ownerHelpCount;
		result |= check(queued.isValid() &&
			!system.waitWithoutOwnerHelp(group, 20),
			"bounded no-owner-help fence times out while worker is blocked");
		result |= check(!queuedJobExecuted &&
			system.metrics().ownerHelpCount == ownerHelpBefore,
			"bounded fence never executes queued work on the owner");
		gate.open();
		result |= check(system.waitWithoutOwnerHelp(group, 5000) &&
			queuedJobExecuted,
			"bounded no-owner-help fence observes physical completion");
		const rts::JobSystemMetrics performanceMetrics = system.metrics();
		result |= check(performanceMetrics.workerBusySampleCount == 2 &&
			performanceMetrics.workerBusyNanoseconds >=
				performanceMetrics.maximumWorkerBusyNanoseconds,
			"physical callback timing records two overflow-safe busy samples");
		system.resetPerformanceMetrics();
		const rts::JobSystemMetrics resetPerformanceMetrics = system.metrics();
		result |= check(resetPerformanceMetrics.workerBusySampleCount == 0 &&
			resetPerformanceMetrics.workerBusyNanoseconds == 0 &&
			resetPerformanceMetrics.maximumWorkerBusyNanoseconds == 0 &&
			resetPerformanceMetrics.workerWaitSampleCount <= 1 &&
			resetPerformanceMetrics.workerWaitNanoseconds >=
				resetPerformanceMetrics.maximumWorkerWaitNanoseconds,
			"match performance reset clears completed timing and may expose only the current wait");
	}

	{
		rts::JobGroup blockedGroup = system.createGroup();
		rts::JobGroup helpedGroup = system.createGroup();
		Gate gate;
		bool helpedJobExecuted = false;
		rts::JobHandle blocker = system.trySubmit(new GateJob(&gate),
			rts::JOB_PRIORITY_NORMAL, blockedGroup);
		result |= check(blocker.isValid(), "owner-help blocker accepted");
		result |= check(gate.waitForEntry(), "owner-help blocker enters before timeout");
		rts::JobHandle helped = system.trySubmit(new MarkJob(&helpedJobExecuted),
			rts::JOB_PRIORITY_FRAME_CRITICAL, helpedGroup);
		result |= check(helped.isValid(), "owner-help job accepted");
		std::thread delayedOpen([&gate]() {
			std::this_thread::sleep_for(std::chrono::milliseconds(250));
			gate.open();
		});
		const std::chrono::steady_clock::time_point start =
			std::chrono::steady_clock::now();
		result |= check(system.wait(helpedGroup), "owner wait completes");
		const long long elapsedMilliseconds =
			std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now() - start).count();
		result |= check(helpedJobExecuted && elapsedMilliseconds < 150,
			"owner wait helps execute eligible work");
		delayedOpen.join();
		result |= check(system.wait(blockedGroup), "blocked worker completes");
		result |= check(system.metrics().ownerHelpCount > 0,
			"owner-help telemetry is recorded");
	}

	{
		system.resetMetrics();
		rts::JobGroup group = system.createGroup();
		Gate gate;
		bool queuedJobExecuted = false;
		rts::JobHandle active = system.trySubmit(new GateJob(&gate),
			rts::JOB_PRIORITY_NORMAL, group);
		result |= check(gate.waitForEntry(), "cancellation blocker enters before timeout");
		rts::JobHandle queued = system.trySubmit(new MarkJob(&queuedJobExecuted),
			rts::JOB_PRIORITY_NORMAL, group);
		result |= check(active.isValid() && queued.isValid(),
			"cancellation jobs accepted");
		result |= check(system.cancel(group), "group cancellation accepted");
		gate.open();
		result |= check(system.wait(group), "cancelled group completes");
		result |= check(active.wasCancelled() && queued.wasCancelled(),
			"active and queued jobs publish cancellation");
		result |= check(!queuedJobExecuted,
			"queued cancelled work does not execute");
		const rts::JobSystemMetrics cancellationMetrics = system.metrics();
		result |= check(cancellationMetrics.executedJobCount == 1 &&
			cancellationMetrics.cancelledJobCount == 2 &&
			cancellationMetrics.maximumActiveWorkers == 1,
			"cancelled claims count finalization but not callback or physical execution");
	}

	{
		rts::JobGroup group = system.createGroup();
		bool executed = false;
		CompletionRecord completionRecord;
		rts::Job *job = new MarkJob(&executed);
		rts::OwnerCompletion *completion = new CompletionProbe(&completionRecord);
		rts::JobHandle handle = system.trySubmit(job, rts::JOB_PRIORITY_NORMAL,
			group, completion);
		if (!handle.isValid())
		{
			delete completion;
			delete job;
		}
		result |= check(handle.isValid(), "owner completion job accepted");
		result |= check(system.wait(group), "owner completion job finishes");
		result |= check(completionRecord.calls == 0,
			"worker does not invoke owner completion");
		result |= check(system.pumpOwnerCompletions(1) == 1,
			"owner pumps one completion");
		result |= check(completionRecord.calls == 1 &&
			completionRecord.succeeded && !completionRecord.cancelled &&
			completionRecord.ownerThread == ownerThread,
			"completion runs once on the owner thread");
	}

	system.shutdown();
	return result;
}

int testTopologyPoliciesAndStartupOptions()
{
	int result = 0;
	result |= check(rts::JobSystem::chooseWorkerCount(1,
		rts::JOB_WORKER_POLICY_AUTO, 0) == 1,
		"auto keeps one worker on a one-CPU topology");
	result |= check(rts::JobSystem::chooseWorkerCount(4,
		rts::JOB_WORKER_POLICY_AUTO, 0) == 3,
		"auto reserves one CPU below eight");
	result |= check(rts::JobSystem::chooseWorkerCount(8,
		rts::JOB_WORKER_POLICY_AUTO, 0) == 6,
		"auto reserves two CPUs at eight");
	result |= check(rts::JobSystem::chooseWorkerCount(32,
		rts::JOB_WORKER_POLICY_AUTO, 0) == 30,
		"auto scales through a synthetic 32-CPU topology");
	result |= check(rts::JobSystem::chooseWorkerCount(32,
		rts::JOB_WORKER_POLICY_ALL, 0) == 32,
		"all policy uses every eligible CPU");
	result |= check(rts::JobSystem::chooseWorkerCount(8,
		rts::JOB_WORKER_POLICY_AUTO, 16) == 16,
		"explicit worker count has no product hard cap");
	// The wide topology values below exercise selection/mask arithmetic only;
	// this test does not start a scheduler for those synthetic topologies.

	rts::JobCpuSetInfo cpuSets[34];
	for (unsigned index = 0; index < 32; ++index)
	{
		cpuSets[index].id = 100 + index;
		cpuSets[index].efficiencyClass = index >= 24 ? 2 : 1;
		cpuSets[index].parked = false;
		cpuSets[index].allocatedToOtherProcess = false;
	}
	cpuSets[32].id = 500;
	cpuSets[32].efficiencyClass = 3;
	cpuSets[32].parked = true;
	cpuSets[32].allocatedToOtherProcess = false;
	cpuSets[33].id = 501;
	cpuSets[33].efficiencyClass = 3;
	cpuSets[33].parked = false;
	cpuSets[33].allocatedToOtherProcess = true;
	unsigned selected[34] = { 0 };
	const unsigned selectedCount = rts::JobSystem::selectWorkerCpuSets(
		cpuSets, 34, rts::JOB_WORKER_POLICY_AUTO, 0, selected, 34);
	result |= check(selectedCount == 30,
		"synthetic topology excludes parked, allocated, and two reserved CPUs");
	bool containsReserved = false;
	for (unsigned index = 0; index < selectedCount; ++index)
	{
		if (selected[index] == 124 || selected[index] == 125)
		{
			containsReserved = true;
		}
	}
	result |= check(!containsReserved,
		"auto reserves the two stable highest-performance CPU sets");

	// This verifies command-line state storage only; it does not start workers.
	result |= check(rts::JobSystem::setStartupWorkerCount(16),
		"startup worker override accepts 16");
	result |= check(rts::JobSystem::setStartupWorkerPolicy("all"),
		"startup worker policy accepts all");
	result |= check(!rts::JobSystem::setStartupWorkerPolicy("invalid"),
		"startup worker policy rejects invalid text");
	const rts::JobSystemConfig startup = rts::JobSystem::startupConfig();
	result |= check(startup.workerCount == 16 &&
		startup.workerPolicy == rts::JOB_WORKER_POLICY_ALL,
		"startup configuration retains command-line choices");
	result |= check(rts::JobSystem::setStartupWorkerCount(0) &&
		rts::JobSystem::setStartupWorkerPolicy("auto"),
		"startup options reset to automatic policy");
	return result;
}

#if defined(RTS_BUILD_CORE_EXTRAS)
int testCompletionPublicationCannotPassWaitingPredicate(bool waitOnHandle)
{
	int result = 0;
	rts::JobSystem &system = rts::JobSystem::instance();
	rts::JobSystemConfig config;
	config.workerCount = 1;
	config.queueCapacity = 8;
	config.scratchBytesPerWorker = 4096;
	config.pinWorkers = false;
	const bool started = system.start(config);
	result |= check(started, "completion-publication fixture starts");
	if (!started) return result;

	rts::JobGroup group = system.createGroup();
	Gate gate;
	rts::Job *job = new GateJob(&gate);
	rts::JobHandle handle = system.trySubmit(job, rts::JOB_PRIORITY_NORMAL, group);
	if (!handle.isValid()) delete job;
	const bool entered = handle.isValid() && gate.waitForEntry();
	result |= check(entered, "completion-publication job enters before waiter starts");
	if (entered)
	{
		const unsigned predicatePause = waitOnHandle ?
			JOB_SYSTEM_TEST_PAUSE_HANDLE_WAIT_PREDICATE : JOB_SYSTEM_TEST_PAUSE_GROUP_WAIT_PREDICATE;
		const unsigned publicationPause = waitOnHandle ?
			JOB_SYSTEM_TEST_PAUSE_AFTER_HANDLE_COMPLETION : JOB_SYSTEM_TEST_PAUSE_AFTER_GROUP_COMPLETION;
		const unsigned contentionPause = waitOnHandle ?
			JOB_SYSTEM_TEST_PAUSE_HANDLE_COMPLETION_LOCK : JOB_SYSTEM_TEST_PAUSE_GROUP_COMPLETION_LOCK;
		rts_job_system_set_test_pause_mask(predicatePause | publicationPause | contentionPause);
		bool predicateHeld = false;
		bool publisherReached = false;
		bool publisherBlocked = false;
		bool completedWhileHeld = true;
		std::thread controller([&]() {
			predicateHeld = rts_job_system_wait_for_test_pause(predicatePause, 5000);
			gate.open();
			publisherReached = rts_job_system_wait_for_test_pause(
				publicationPause | contentionPause, 5000);
			publisherBlocked = rts_job_system_wait_for_test_pause(contentionPause, 0);
			completedWhileHeld = waitOnHandle ? handle.isComplete() : group.isComplete();
			// Release every pause before joining, including on a failed assertion.
			rts_job_system_set_test_pause_mask(0);
		});
		// The owner handle fence polls at one millisecond and the group fence is
		// bounded, so the deliberately broken missed-notify path cannot strand a join.
		const bool waited = waitOnHandle ? system.wait(handle) :
			system.waitWithoutOwnerHelp(group, 5000);
		controller.join();
		result |= check(predicateHeld, "completion waiter holds its mutex after a false predicate");
		result |= check(publisherReached, "completion publisher reaches the controlled interleaving");
		result |= check(predicateHeld && publisherReached && publisherBlocked && !completedWhileHeld,
			"completion publication cannot bypass the waiter's predicate mutex");
		result |= check(waited && handle.succeeded(),
			"completion waiter drains after the publisher/waiter handshake");
	}
	gate.open();
	if (handle.isValid()) system.wait(group);
	system.shutdown();
	return result;
}

int testHandleCompletionPublication()
{
	return testCompletionPublicationCannotPassWaitingPredicate(true);
}

int testGroupCompletionPublication()
{
	return testCompletionPublicationCannotPassWaitingPredicate(false);
}

int testBatchRecheckCannotRepublishRetiringExecution()
{
	int result = 0;
	rts::JobSystem &system = rts::JobSystem::instance();
	rts::JobSystemConfig config;
	config.workerCount = 1;
	config.queueCapacity = 8;
	config.scratchBytesPerWorker = 4096;
	config.pinWorkers = false;
	const bool started = system.start(config);
	result |= check(started, "batch-retirement fixture starts");
	if (!started) return result;

	rts::JobGroup group = system.createGroup();
	Gate gate;
	std::atomic<unsigned> executions(0);
	std::atomic<unsigned> destructions(0);
	rts::JobSubmission submission;
	submission.job = new BlockingLifetimeJob(&gate, &executions, &destructions);
	rts::JobHandle handle;
	std::atomic<bool> batchReturned(false);
	bool recheckPaused = false;
	bool executionEntered = false;
	bool ownershipChecked = false;
	bool retirementPaused = false;
	bool admissionFinished = false;
	rts_job_system_set_test_pause_mask(
		JOB_SYSTEM_TEST_PAUSE_BEFORE_BATCH_READY_RECHECK |
		JOB_SYSTEM_TEST_PAUSE_READY_OWNERSHIP_RECHECK |
		JOB_SYSTEM_TEST_PAUSE_AFTER_EXECUTION_RETIREMENT);
	std::thread controller([&]() {
		recheckPaused = rts_job_system_wait_for_test_pause(
			JOB_SYSTEM_TEST_PAUSE_BEFORE_BATCH_READY_RECHECK, 5000);
		executionEntered = recheckPaused && gate.waitForEntry();
		if (executionEntered)
		{
			// A rejected duplicate must not attempt another queue publication.
			// If the bug does attempt one, the fault prevents a phantom ready
			// counter from stranding shutdown while preserving observable proof.
			rts_job_system_set_test_fault(JOB_SYSTEM_TEST_FAIL_QUEUE_PUSH, 1);
			rts_job_system_release_test_pause(
				JOB_SYSTEM_TEST_PAUSE_BEFORE_BATCH_READY_RECHECK);
			const std::chrono::steady_clock::time_point deadline =
				std::chrono::steady_clock::now() + std::chrono::seconds(5);
			while (!batchReturned.load(std::memory_order_acquire) &&
				!rts_job_system_wait_for_test_pause(
					JOB_SYSTEM_TEST_PAUSE_READY_OWNERSHIP_RECHECK, 0) &&
				std::chrono::steady_clock::now() < deadline)
			{
				std::this_thread::yield();
			}
			ownershipChecked = batchReturned.load(std::memory_order_acquire) ||
				rts_job_system_wait_for_test_pause(
					JOB_SYSTEM_TEST_PAUSE_READY_OWNERSHIP_RECHECK, 0);
			gate.open();
			retirementPaused = rts_job_system_wait_for_test_pause(
				JOB_SYSTEM_TEST_PAUSE_AFTER_EXECUTION_RETIREMENT, 5000);
			rts_job_system_release_test_pause(
				JOB_SYSTEM_TEST_PAUSE_READY_OWNERSHIP_RECHECK);
			const std::chrono::steady_clock::time_point admissionDeadline =
				std::chrono::steady_clock::now() + std::chrono::seconds(5);
			while (!batchReturned.load(std::memory_order_acquire) &&
				std::chrono::steady_clock::now() < admissionDeadline)
			{
				std::this_thread::yield();
			}
			admissionFinished = batchReturned.load(std::memory_order_acquire);
		}
		// Every error path releases both the worker and the admission thread.
		gate.open();
		rts_job_system_set_test_pause_mask(0);
	});
	const bool accepted = system.trySubmitBatch(&submission, 1, group, &handle);
	batchReturned.store(true, std::memory_order_release);
	controller.join();
	if (!accepted) delete submission.job;
	result |= check(accepted && recheckPaused && executionEntered &&
		ownershipChecked && retirementPaused && admissionFinished,
		"batch recheck overlaps a real execution's controlled retirement");
	if (accepted)
	{
		result |= check(system.wait(group) && handle.succeeded() &&
			executions.load() == 1 && destructions.load() == 1,
			"retiring batch job completes and destroys exactly once");
	}

	rts::Job *probe = new LifetimeJob(&destructions);
	rts::JobHandle probeHandle = system.trySubmit(probe,
		rts::JOB_PRIORITY_NORMAL, group);
	result |= check(!probeHandle.isValid(),
		"retiring execution rejects duplicate publication before consuming the queue fault");
	if (!probeHandle.isValid()) delete probe;
	else system.wait(group);
	rts_job_system_set_test_fault(0, 0);
	result |= check(group.isComplete() && system.outstandingJobCount() == 0,
		"batch-retirement regression drains without phantom ready work");
	system.shutdown();
	return result;
}

int testFaultInjectionAndRecovery()
{
	int result = 0;
	rts::JobSystem &system = rts::JobSystem::instance();
	rts::JobSystemConfig config;
	config.workerCount = 4;
	config.queueCapacity = 16;
	config.scratchBytesPerWorker = 4096;
	config.pinWorkers = false;

	rts_job_system_set_test_fault(JOB_SYSTEM_TEST_FAIL_START, 1);
	result |= check(!system.start(config) && !system.isRunning() &&
		system.workerCount() == 0, "start fault leaves no running workers");
	rts_job_system_set_test_fault(JOB_SYSTEM_TEST_FAIL_WORKER_SCRATCH, 2);
	result |= check(!system.start(config) && !system.isRunning() &&
		system.workerCount() == 0, "scratch fault joins partially created workers");
	rts_job_system_set_test_fault(JOB_SYSTEM_TEST_FAIL_THREAD_CREATE, 2);
	result |= check(!system.start(config) && !system.isRunning() &&
		system.workerCount() == 0, "thread fault joins partially created workers");
	result |= check(system.start(config), "system restarts after start faults");

	rts_job_system_set_test_fault(JOB_SYSTEM_TEST_FAIL_GROUP_ALLOCATION, 1);
	result |= check(!system.createGroup().isValid(),
		"group allocation fault returns an invalid group");
	rts::JobGroup group = system.createGroup();
	result |= check(group.isValid(), "group creation recovers after fault");

	std::atomic<unsigned> destructions(0);
	rts::Job *job = new LifetimeJob(&destructions);
	rts_job_system_set_test_fault(JOB_SYSTEM_TEST_FAIL_JOB_ALLOCATION, 1);
	rts::JobHandle handle = system.trySubmit(job, rts::JOB_PRIORITY_NORMAL, group);
	result |= check(!handle.isValid() && destructions.load() == 0,
		"job allocation rejection preserves caller ownership");
	delete job;
	result |= check(destructions.load() == 1, "caller destroys rejected job once");

	job = new LifetimeJob(&destructions);
	rts_job_system_set_test_fault(JOB_SYSTEM_TEST_FAIL_QUEUE_PUSH, 1);
	handle = system.trySubmit(job, rts::JOB_PRIORITY_NORMAL, group);
	result |= check(!handle.isValid() && destructions.load() == 1,
		"queue rejection preserves caller ownership");
	delete job;
	result |= check(destructions.load() == 2, "caller owns queue-rejected job");

#if !defined(_MSC_VER) || _MSC_VER >= 1300
	{
		rts::JobGroup staleGroup = system.createGroup();
		std::atomic<unsigned> staleDestructions(0);
		rts_job_system_set_test_pause_mask(
			JOB_SYSTEM_TEST_PAUSE_AFTER_EXECUTION_CLAIM |
			JOB_SYSTEM_TEST_PAUSE_AFTER_STALE_QUEUE_DISCARD);
		rts_job_system_set_test_fault(
			JOB_SYSTEM_TEST_FAIL_AFTER_QUEUE_PUSH, 1);
		rts::Job *staleJob = new LifetimeJob(&staleDestructions);
		rts::JobHandle staleHandle = system.trySubmit(staleJob,
			rts::JOB_PRIORITY_NORMAL, staleGroup);
		result |= check(!staleHandle.isValid() && staleGroup.isComplete() &&
			system.outstandingJobCount() == 0 &&
			staleDestructions.load(std::memory_order_acquire) == 0,
			"post-push failure rolls admission back to the caller");
		delete staleJob;
		result |= check(staleDestructions.load(std::memory_order_acquire) == 1,
			"caller destroys the post-push rejected job once");
		rts::JobGroup recoveryGroup = system.createGroup();
		Gate recoveryGate;
		std::atomic<unsigned> recoveryExecutions(0);
		std::atomic<unsigned> recoveryDestructions(0);
		BlockingLifetimeJob *recoveryJob = new BlockingLifetimeJob(
			&recoveryGate, &recoveryExecutions, &recoveryDestructions);
		rts::JobHandle recoveryHandle = system.trySubmit(recoveryJob,
			rts::JOB_PRIORITY_NORMAL, recoveryGroup);
		if (!recoveryHandle.isValid())
		{
			delete recoveryJob;
		}
		result |= check(rts_job_system_wait_for_test_pause(
			JOB_SYSTEM_TEST_PAUSE_AFTER_STALE_QUEUE_DISCARD, 5000),
			"worker discards the uncounted failed-publication entry");
		rts_job_system_release_test_pause(
			JOB_SYSTEM_TEST_PAUSE_AFTER_STALE_QUEUE_DISCARD);
		result |= check(recoveryHandle.isValid() &&
			rts_job_system_wait_for_test_pause(
				JOB_SYSTEM_TEST_PAUSE_AFTER_EXECUTION_CLAIM, 5000),
			"scheduler claims valid work after stale-entry recovery");
		rts_job_system_release_test_pause(
			JOB_SYSTEM_TEST_PAUSE_AFTER_EXECUTION_CLAIM);
		result |= check(recoveryGate.waitForEntry(),
			"recovery job starts");
		recoveryGate.open();
		result |= check(system.wait(recoveryGroup),
			"recovery group drains");
		result |= check(recoveryHandle.isValid() &&
			recoveryHandle.succeeded() &&
			recoveryExecutions.load(std::memory_order_acquire) == 1 &&
			recoveryDestructions.load(std::memory_order_acquire) == 1 &&
			system.outstandingJobCount() == 0,
			"uncounted stale entry cannot corrupt later execution or shutdown");
		rts_job_system_set_test_pause_mask(0);

		system.shutdown();
		config.workerCount = 1;
		config.queueCapacity = 8;
		result |= check(system.start(config),
			"admission-claim race test starts");
		rts::JobGroup raceGroup = system.createGroup();
		Gate prerequisiteGate;
		Gate dependentGate;
		std::atomic<unsigned> executions(0);
		std::atomic<unsigned> raceDestructions(0);
		GateJob *prerequisiteJob = new GateJob(&prerequisiteGate);
		rts::JobHandle prerequisite = system.trySubmit(prerequisiteJob,
			rts::JOB_PRIORITY_NORMAL, raceGroup);
		if (!prerequisite.isValid())
		{
			delete prerequisiteJob;
		}
		result |= check(prerequisite.isValid() && prerequisiteGate.waitForEntry(),
			"admission-claim prerequisite enters");

		rts::JobHandle dependent;
		BlockingLifetimeJob *raceJob = new BlockingLifetimeJob(&dependentGate,
			&executions, &raceDestructions);
		rts_job_system_set_test_pause_mask(JOB_SYSTEM_TEST_PAUSE_AFTER_ACCEPT);
		std::thread submitter([&]() {
			dependent = system.trySubmitAfter(raceJob,
				rts::JOB_PRIORITY_NORMAL, raceGroup, &prerequisite, 1);
		});
		const bool admissionPaused = rts_job_system_wait_for_test_pause(
			JOB_SYSTEM_TEST_PAUSE_AFTER_ACCEPT, 5000);
		result |= check(admissionPaused,
			"dependent admission reaches the deterministic pause");
		prerequisiteGate.open();
		result |= check(dependentGate.waitForEntry(),
			"dependency release transfers execution ownership");
		rts_job_system_release_test_pause(JOB_SYSTEM_TEST_PAUSE_AFTER_ACCEPT);
		submitter.join();
		result |= check(dependent.isValid(),
			"execution ownership prevents admission rollback");
		dependentGate.open();
		const std::chrono::steady_clock::time_point raceDeadline =
			std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (!raceGroup.isComplete() &&
			std::chrono::steady_clock::now() < raceDeadline)
		{
			std::this_thread::yield();
		}
		result |= check(raceGroup.isComplete(),
			"admission-claim race group drains");
		result |= check(dependent.isValid() && dependent.succeeded() &&
			executions.load(std::memory_order_relaxed) == 1 &&
			raceDestructions.load(std::memory_order_relaxed) == 1 &&
			system.outstandingJobCount() == 0,
			"claimed dependent executes and is finalized exactly once");
		if (!dependent.isValid() &&
			executions.load(std::memory_order_acquire) == 0)
		{
			delete raceJob;
		}
		rts_job_system_set_test_pause_mask(0);

		system.shutdown();
		result |= check(system.start(config),
			"dependency-publication failure test starts");
		rts::JobGroup failureGroup = system.createGroup();
		Gate failurePrerequisiteGate;
		std::atomic<unsigned> failedExecutions(0);
		std::atomic<unsigned> failedDestructions(0);
		Gate failedJobGate;
		GateJob *failurePrerequisiteJob = new GateJob(&failurePrerequisiteGate);
		rts::JobHandle failurePrerequisite = system.trySubmit(
			failurePrerequisiteJob, rts::JOB_PRIORITY_NORMAL, failureGroup);
		if (!failurePrerequisite.isValid())
		{
			delete failurePrerequisiteJob;
		}
		result |= check(failurePrerequisite.isValid() &&
			failurePrerequisiteGate.waitForEntry(),
			"publication-failure prerequisite enters");
		rts::JobHandle failedDependent;
		BlockingLifetimeJob *failedJob = new BlockingLifetimeJob(&failedJobGate,
			&failedExecutions, &failedDestructions);
		const unsigned publicationPauseMask =
			JOB_SYSTEM_TEST_PAUSE_AFTER_ACCEPT |
			JOB_SYSTEM_TEST_PAUSE_BEFORE_DEPENDENT_ENQUEUE |
			JOB_SYSTEM_TEST_PAUSE_AFTER_QUEUE_FAILURE |
			JOB_SYSTEM_TEST_PAUSE_AFTER_DEPENDENT_ENQUEUE;
		rts_job_system_set_test_pause_mask(publicationPauseMask);
		std::thread failureSubmitter([&]() {
			failedDependent = system.trySubmitAfter(failedJob,
				rts::JOB_PRIORITY_NORMAL, failureGroup, &failurePrerequisite, 1);
		});
		const bool failureAdmissionPaused =
			rts_job_system_wait_for_test_pause(
				JOB_SYSTEM_TEST_PAUSE_AFTER_ACCEPT, 5000);
		result |= check(failureAdmissionPaused,
			"publication-failure admission reaches the deterministic pause");
		failurePrerequisiteGate.open();
		const bool dependencyReleasePaused =
			rts_job_system_wait_for_test_pause(
				JOB_SYSTEM_TEST_PAUSE_BEFORE_DEPENDENT_ENQUEUE, 5000);
		result |= check(dependencyReleasePaused,
			"dependency release pauses before competing publication");
		rts_job_system_set_test_fault(JOB_SYSTEM_TEST_FAIL_QUEUE_PUSH, 1);
		rts_job_system_release_test_pause(JOB_SYSTEM_TEST_PAUSE_AFTER_ACCEPT);
		const bool queueFailurePaused = rts_job_system_wait_for_test_pause(
			JOB_SYSTEM_TEST_PAUSE_AFTER_QUEUE_FAILURE, 5000);
		result |= check(queueFailurePaused,
			"submitter publication failure is held before rollback");
		rts_job_system_release_test_pause(
			JOB_SYSTEM_TEST_PAUSE_BEFORE_DEPENDENT_ENQUEUE);
		const bool dependencyEnqueueCompleted =
			rts_job_system_wait_for_test_pause(
				JOB_SYSTEM_TEST_PAUSE_AFTER_DEPENDENT_ENQUEUE, 5000);
		result |= check(dependencyEnqueueCompleted,
			"dependency release observes failed publication ownership");
		rts_job_system_release_test_pause(
			JOB_SYSTEM_TEST_PAUSE_AFTER_DEPENDENT_ENQUEUE);
		rts_job_system_release_test_pause(
			JOB_SYSTEM_TEST_PAUSE_AFTER_QUEUE_FAILURE);
		failureSubmitter.join();
		result |= check(!failedDependent.isValid(),
			"submitter publication failure returns caller ownership");
		rts_job_system_set_test_pause_mask(0);
		failedJobGate.open();
		const std::chrono::steady_clock::time_point failureDeadline =
			std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (!failureGroup.isComplete() &&
			std::chrono::steady_clock::now() < failureDeadline)
		{
			std::this_thread::yield();
		}
		const bool failureGroupCompleted = failureGroup.isComplete();
		const unsigned failedExecutionCount = failedExecutions.load(
			std::memory_order_acquire);
		const unsigned failureOutstandingCount = system.outstandingJobCount();
		system.shutdown();
		if (failedDestructions.load(std::memory_order_acquire) == 0)
		{
			delete failedJob;
		}
		result |= check(failureGroupCompleted,
			"dependency-publication failure group drains");
		result |= check(failedExecutionCount == 0 &&
			failedDestructions.load(std::memory_order_relaxed) == 1 &&
			failureOutstandingCount == 0,
			"failed publication cannot resurrect or double-finalize work");

		result |= check(system.start(config),
			"dependency-first publication failure test starts");
		rts::JobGroup dependencyFirstGroup = system.createGroup();
		Gate dependencyFirstPrerequisiteGate;
		Gate dependencyFirstJobGate;
		std::atomic<unsigned> dependencyFirstExecutions(0);
		std::atomic<unsigned> dependencyFirstDestructions(0);
		GateJob *dependencyFirstPrerequisiteJob =
			new GateJob(&dependencyFirstPrerequisiteGate);
		rts::JobHandle dependencyFirstPrerequisite = system.trySubmit(
			dependencyFirstPrerequisiteJob, rts::JOB_PRIORITY_NORMAL,
			dependencyFirstGroup);
		if (!dependencyFirstPrerequisite.isValid())
		{
			delete dependencyFirstPrerequisiteJob;
		}
		result |= check(dependencyFirstPrerequisite.isValid() &&
			dependencyFirstPrerequisiteGate.waitForEntry(),
			"dependency-first prerequisite enters");
		BlockingLifetimeJob *dependencyFirstJob = new BlockingLifetimeJob(
			&dependencyFirstJobGate, &dependencyFirstExecutions,
			&dependencyFirstDestructions);
		rts::JobHandle dependencyFirstHandle;
		rts_job_system_set_test_pause_mask(publicationPauseMask);
		std::thread dependencyFirstSubmitter([&]() {
			dependencyFirstHandle = system.trySubmitAfter(dependencyFirstJob,
				rts::JOB_PRIORITY_NORMAL, dependencyFirstGroup,
				&dependencyFirstPrerequisite, 1);
		});
		result |= check(rts_job_system_wait_for_test_pause(
			JOB_SYSTEM_TEST_PAUSE_AFTER_ACCEPT, 5000),
			"dependency-first admission reaches the deterministic pause");
		dependencyFirstPrerequisiteGate.open();
		result |= check(rts_job_system_wait_for_test_pause(
			JOB_SYSTEM_TEST_PAUSE_BEFORE_DEPENDENT_ENQUEUE, 5000),
			"dependency-first publisher pauses before queue publication");
		rts_job_system_set_test_fault(JOB_SYSTEM_TEST_FAIL_QUEUE_PUSH, 1);
		rts_job_system_release_test_pause(
			JOB_SYSTEM_TEST_PAUSE_BEFORE_DEPENDENT_ENQUEUE);
		result |= check(rts_job_system_wait_for_test_pause(
			JOB_SYSTEM_TEST_PAUSE_AFTER_QUEUE_FAILURE, 5000),
			"dependency-first publisher records queue failure");
		rts_job_system_release_test_pause(JOB_SYSTEM_TEST_PAUSE_AFTER_ACCEPT);
		dependencyFirstSubmitter.join();
		result |= check(dependencyFirstHandle.isValid(),
			"competing submitter preserves scheduler failure ownership");
		rts_job_system_release_test_pause(
			JOB_SYSTEM_TEST_PAUSE_AFTER_QUEUE_FAILURE);
		result |= check(rts_job_system_wait_for_test_pause(
			JOB_SYSTEM_TEST_PAUSE_AFTER_DEPENDENT_ENQUEUE, 5000),
			"dependency-first publisher reaches finalization handoff");
		rts_job_system_release_test_pause(
			JOB_SYSTEM_TEST_PAUSE_AFTER_DEPENDENT_ENQUEUE);
		rts_job_system_set_test_pause_mask(0);
		dependencyFirstJobGate.open();
		const std::chrono::steady_clock::time_point dependencyFirstDeadline =
			std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (!dependencyFirstGroup.isComplete() &&
			std::chrono::steady_clock::now() < dependencyFirstDeadline)
		{
			std::this_thread::yield();
		}
		const bool dependencyFirstCompleted = dependencyFirstGroup.isComplete();
		const unsigned dependencyFirstOutstanding = system.outstandingJobCount();
		system.shutdown();
		if (!dependencyFirstHandle.isValid() &&
			dependencyFirstDestructions.load(std::memory_order_acquire) == 0)
		{
			delete dependencyFirstJob;
		}
		result |= check(dependencyFirstCompleted &&
			dependencyFirstHandle.isValid() && dependencyFirstHandle.failed() &&
			dependencyFirstExecutions.load(std::memory_order_relaxed) == 0 &&
			dependencyFirstDestructions.load(std::memory_order_relaxed) == 1 &&
			dependencyFirstOutstanding == 0,
			"dependency-first queue failure finalizes exactly once without a strand");

		result |= check(system.start(config),
			"execution-owner finalization race test starts");
		rts::JobGroup finalizationGroup = system.createGroup();
		rts_job_system_set_test_pause_mask(
			JOB_SYSTEM_TEST_PAUSE_NON_OWNER_FINALIZER);
		rts_job_system_set_test_fault(JOB_SYSTEM_TEST_COMPETING_FINALIZER, 1);
		FailJob *finalizationJob = new FailJob;
		rts::JobHandle finalizationHandle = system.trySubmit(finalizationJob,
			rts::JOB_PRIORITY_NORMAL, finalizationGroup);
		if (!finalizationHandle.isValid())
		{
			delete finalizationJob;
		}
		result |= check(finalizationHandle.isValid(),
			"finalization-race job is admitted");
		const bool nonOwnerPaused = rts_job_system_wait_for_test_pause(
			JOB_SYSTEM_TEST_PAUSE_NON_OWNER_FINALIZER, 5000);
		result |= check(nonOwnerPaused,
			"non-owner finalizer holds the finalization claim");
		rts_job_system_release_test_pause(
			JOB_SYSTEM_TEST_PAUSE_NON_OWNER_FINALIZER);
		const std::chrono::steady_clock::time_point finalizationDeadline =
			std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (!finalizationGroup.isComplete() &&
			std::chrono::steady_clock::now() < finalizationDeadline)
		{
			std::this_thread::yield();
		}
		const rts::JobSystemMetrics finalizationMetrics = system.metrics();
		result |= check(finalizationGroup.isComplete() &&
			finalizationHandle.failed() && finalizationGroup.failed() &&
			finalizationMetrics.executedJobCount == 1 &&
			finalizationMetrics.failedJobCount == 1 &&
			system.outstandingJobCount() == 0,
			"execution owner retries and finalizes exactly once");
		rts_job_system_set_test_pause_mask(0);
	}
#endif

	group = system.createGroup();
	result |= check(group.isValid(),
		"completion-fault group is recreated after race tests");
	CompletionRecord completionRecord;
	bool executed = false;
	rts_job_system_set_test_fault(JOB_SYSTEM_TEST_FAIL_COMPLETION_PUSH, 1);
	handle = system.trySubmit(new MarkJob(&executed),
		rts::JOB_PRIORITY_NORMAL, group,
		new CompletionProbe(&completionRecord));
	result |= check(handle.isValid(), "completion-fault job is admitted");
	result |= check(system.wait(group), "completion-fault group drains");
	result |= check(executed && handle.failed() && group.failed(),
		"completion queue failure is explicit job/group failure");
	result |= check(completionRecord.calls == 0,
		"failed completion publication is not called from a worker");

	system.shutdown();
	rts_job_system_set_test_fault(0, 0);
	return result;
}
#endif

int testBatchAdmissionAndWorkerWaitRejection()
{
	int result = 0;
	rts::JobSystem &system = rts::JobSystem::instance();
	rts::JobSystemConfig config;
	config.workerCount = 2;
	config.queueCapacity = 8;
	config.scratchBytesPerWorker = 4096;
	config.pinWorkers = false;
	result |= check(system.start(config), "batch test starts");

	{
		rts::JobGroup group = system.createGroup();
		std::atomic<unsigned> destructions(0);
		rts::JobSubmission submissions[3];
		rts::JobHandle handles[3];
		for (unsigned index = 0; index < 3; ++index)
		{
			submissions[index].job = new LifetimeJob(&destructions);
			submissions[index].priority = rts::JOB_PRIORITY_NORMAL;
		}
#if defined(RTS_BUILD_CORE_EXTRAS)
		rts_job_system_set_test_fault(JOB_SYSTEM_TEST_FAIL_QUEUE_PUSH, 2);
#endif
		result |= check(!system.trySubmitBatch(submissions, 3, group, handles),
			"batch queue fault rejects the entire batch");
		result |= check(destructions.load() == 0,
			"rejected batch preserves caller-owned jobs");
		result |= check(system.outstandingJobCount() == 0,
			"rejected batch rolls back bounded capacity");
		result |= check(group.isComplete(),
			"rejected batch rolls back group membership");
		for (unsigned index = 0; index < 3; ++index)
		{
			result |= check(!handles[index].isValid(),
				"rejected batch publishes no handles");
			delete submissions[index].job;
		}
		result |= check(destructions.load() == 3,
			"caller destroys every rejected batch job once");

		for (unsigned index = 0; index < 3; ++index)
		{
			submissions[index].job = new LifetimeJob(&destructions);
		}
		result |= check(system.trySubmitBatch(submissions, 3, group, handles),
			"valid batch is admitted atomically");
		result |= check(system.wait(group), "accepted batch completes");
		result |= check(destructions.load() == 6,
			"accepted batch jobs execute and destroy exactly once");
		result |= check(system.metrics().injectionHighWater >= 3,
			"accepted batch contributes to injection high-water telemetry");
		for (unsigned index = 0; index < 3; ++index)
		{
			result |= check(handles[index].succeeded(),
				"accepted batch handle succeeds");
		}
	}

	/* Tiny failing jobs can complete while batch admission publishes handles.
	 * Repeat the race window so an execution owner can never be stranded by a
	 * transient duplicate-enqueue claim. */
	for (unsigned iteration = 0; iteration < 128; ++iteration)
	{
		rts::JobGroup group = system.createGroup();
		rts::JobSubmission submissions[8];
		rts::JobHandle handles[8];
		for (unsigned index = 0; index < 8; ++index)
		{
			submissions[index].job = new FailJob;
			submissions[index].priority = rts::JOB_PRIORITY_FRAME_CRITICAL;
		}
		const bool accepted = system.trySubmitBatch(submissions, 8, group,
			handles);
		result |= check(accepted,
			"fast-failure race batch is admitted");
		if (!accepted)
		{
			for (unsigned index = 0; index < 8; ++index)
			{
				delete submissions[index].job;
			}
			break;
		}
		result |= check(system.wait(group) && group.failed(),
			"fast-failure race batch drains with explicit failure");
		for (unsigned index = 0; index < 8; ++index)
		{
			result |= check(handles[index].failed(),
				"fast-failure race handle reaches terminal failure");
		}
	}

	{
		rts::JobGroup group = system.createGroup();
		bool workerWaitResult = true;
		bool childExecuted = false;
		rts::JobHandle parent = system.trySubmit(
			new WorkerWaitJob(&system, group, &workerWaitResult, &childExecuted),
			rts::JOB_PRIORITY_NORMAL, group);
		result |= check(parent.isValid(), "worker-wait parent accepted");
		result |= check(system.wait(group), "worker-wait group completes");
		result |= check(!workerWaitResult,
			"compute worker cannot block waiting for a child");
		result |= check(childExecuted,
			"worker-wait child completes before test storage is released");
		result |= check(system.metrics().workerWaitRejectionCount > 0,
			"worker-wait rejection telemetry is recorded");
	}

	system.recordSerialFallback();
	const rts::JobSystemMetrics metrics = system.metrics();
	result |= check(metrics.serialFallbackCount == 1,
		"consumer serial fallback telemetry is recorded");
	result |= check(metrics.totalQueueLatencyNanoseconds > 0 &&
		metrics.maximumQueueLatencyNanoseconds > 0,
		"queue latency telemetry is recorded");

	system.shutdown();
	return result;
}

class OwnerRoleProbeJob : public rts::Job
{
public:
	OwnerRoleProbeJob(std::atomic<bool> *entered, bool *registration)
		: m_entered(entered), m_registration(registration) {}
	virtual void execute(rts::JobContext &)
	{
		*m_registration = rts::JobSystem::instance().registerCurrentThread(rts::JOB_OWNER_IO);
		m_entered->store(true, std::memory_order_release);
	}
private:
	std::atomic<bool> *m_entered;
	bool *m_registration;
};

int testServiceOwnerLifetimeAndWorkerRoleRejection()
{
	int result = 0;
	rts::JobSystem &system = rts::JobSystem::instance();
	rts::JobSystemConfig config;
	config.workerCount = 2;
	config.queueCapacity = 8;
	config.scratchBytesPerWorker = 4096;
	config.pinWorkers = false;
	result |= check(system.start(config) && system.registerCurrentThread(rts::JOB_OWNER_GAME),
		"service-owner fixture establishes the game owner");
	Gate serviceLifetime;
	bool serviceRegistered = false;
	bool serviceUnregistered = false;
	bool serviceSurvivedComputeDrain = false;
	bool serviceWrongGameOwner = true;
	std::thread service([&]() {
		serviceWrongGameOwner = system.registerCurrentThread(rts::JOB_OWNER_GAME);
		serviceRegistered = system.registerCurrentThread(rts::JOB_OWNER_AUDIO);
		serviceLifetime.waitUntilOpen();
		serviceSurvivedComputeDrain = system.isCurrentThread(rts::JOB_OWNER_AUDIO) &&
			!system.isRunning() && !system.ensureStarted() && !system.start(config);
		serviceUnregistered = system.unregisterCurrentThread(rts::JOB_OWNER_AUDIO);
	});
	result |= check(serviceLifetime.waitForEntry(), "service execution owner enters");
	result |= check(!system.unregisterCurrentThread(rts::JOB_OWNER_AUDIO),
		"game owner cannot unregister another service execution owner");
	bool conflictingRegistration = true;
	std::thread competing([&]() {
		conflictingRegistration = system.registerCurrentThread(rts::JOB_OWNER_AUDIO);
	});
	competing.join();
	result |= check(!conflictingRegistration, "one role cannot hide a second private service thread");
	std::atomic<bool> entered(false);
	bool workerRegistration = true;
	rts::JobGroup group = system.createGroup();
	OwnerRoleProbeJob *probe = new OwnerRoleProbeJob(&entered, &workerRegistration);
	rts::JobHandle handle = system.trySubmit(probe, rts::JOB_PRIORITY_NORMAL, group);
	if (!handle.isValid()) delete probe;
	const std::chrono::steady_clock::time_point deadline =
		std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (!entered.load(std::memory_order_acquire) &&
		std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
	const bool workerEntered = entered.load(std::memory_order_acquire);
	system.shutdown();
	serviceLifetime.open();
	service.join();
	result |= check(handle.isValid() && workerEntered && !workerRegistration && handle.succeeded(),
		"compute workers cannot claim native service resource ownership");
	result |= check(serviceRegistered && serviceUnregistered &&
		serviceSurvivedComputeDrain && !serviceWrongGameOwner,
		"service ownership remains valid through compute drain and releases on its owner");
	result |= check(system.unregisterCurrentThread(rts::JOB_OWNER_GAME),
		"game owner releases after all services have exited");
	return result;
}

int testProcessAffinityLimitsTopology()
{
#if defined(_WIN32)
	DWORD_PTR previousMask = 0;
	DWORD_PTR systemMask = 0;
	if (!GetProcessAffinityMask(GetCurrentProcess(), &previousMask, &systemMask) ||
		previousMask == 0)
	{
		printf("Process-affinity topology fixture unavailable on this processor-group layout.\n");
		return 0;
	}
	DWORD_PTR restrictedMask = previousMask & (~previousMask + 1);
	typedef DWORD (WINAPI *GetCurrentProcessorNumberFunction)();
	GetCurrentProcessorNumberFunction currentProcessor =
		reinterpret_cast<GetCurrentProcessorNumberFunction>(
			GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetCurrentProcessorNumber"));
	if (currentProcessor != 0)
	{
		const DWORD current = currentProcessor();
		if (current < sizeof(DWORD_PTR) * CHAR_BIT &&
			(previousMask & (static_cast<DWORD_PTR>(1) << current)) != 0)
			restrictedMask = static_cast<DWORD_PTR>(1) << current;
	}
	if (!SetProcessAffinityMask(GetCurrentProcess(), restrictedMask))
	{
		printf("Process-affinity topology fixture could not restrict its test process.\n");
		return 0;
	}
	int result = 0;
	rts::JobSystem &system = rts::JobSystem::instance();
	rts::JobSystemConfig config;
	config.workerCount = 0;
	config.workerPolicy = rts::JOB_WORKER_POLICY_ALL;
	config.queueCapacity = 8;
	config.scratchBytesPerWorker = 4096;
	config.pinWorkers = true;
	const bool started = system.start(config);
	result |= check(started && system.workerCount() == 1 &&
		system.metrics().availableLogicalCpuCount == 1,
		"live CPU-set enumeration respects this process's single-CPU hard affinity");
	if (started)
	{
		result |= check(system.registerCurrentThread(rts::JOB_OWNER_GAME),
			"game role registration tolerates a single available logical CPU");
		rts::JobGroup group = system.createGroup();
		unsigned executions = 0;
		CountJob *job = new CountJob(&executions);
		rts::JobHandle handle = system.trySubmit(job, rts::JOB_PRIORITY_NORMAL, group);
		if (!handle.isValid()) delete job;
		result |= check(handle.isValid() && system.wait(group) && executions == 1,
			"one-CPU worker/owner helping path remains live under real affinity");
		system.shutdown();
		result |= check(system.unregisterCurrentThread(rts::JOB_OWNER_GAME),
			"owner restores its prior CPU-set selection after shutdown");
	}
	result |= check(SetProcessAffinityMask(GetCurrentProcess(), previousMask) != 0,
		"topology fixture restores the test process's original affinity");
	return result;
#else
	return 0;
#endif
}

int testLifecycleOwnershipAndResourceLimits()
{
	int result = 0;
	rts::JobSystem &system = rts::JobSystem::instance();
	rts::JobSystemConfig config;
	config.workerCount = 2;
	config.queueCapacity = 16;
	config.scratchBytesPerWorker = 4096;
	config.pinWorkers = false;
	result |= check(system.start(config), "lifecycle ownership test starts");
	std::thread nonOwnerShutdown([&system]() { system.shutdown(); });
	nonOwnerShutdown.join();
	result |= check(system.isRunning() && system.workerCount() == 2,
		"non-owner shutdown cannot tear down owner wait state");
	rts::JobGroup group = system.createGroup();
	bool executed = false;
	rts::JobHandle handle = system.trySubmit(new MarkJob(&executed),
		rts::JOB_PRIORITY_NORMAL, group);
	result |= check(handle.isValid() && system.wait(group) &&
		executed,
		"scheduler remains usable after rejected non-owner shutdown");
	system.shutdown();

	// The pathological request is rejected by JobSystem before allocation and
	// intentionally remains outside the local worker-start matrix.
	config.workerCount = UINT_MAX;
	result |= check(!system.start(config) && !system.isRunning(),
		"topology-derived resource limit rejects pathological worker counts");
	return result;
}
#endif
}

int main(int argc, char **argv)
{
#if defined(_WIN32)
	SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX |
		SEM_NOOPENFILEERRORBOX);
#endif
#if defined(_MSC_VER) && defined(_DEBUG)
#if _MSC_VER >= 1400
	_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
	_CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
	_CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif
	JobSystemTestLane selectedLane = JOB_SYSTEM_TEST_LANE_FULL;
	const char *selector = argv != 0 && argc > 1 ? argv[1] : 0;
	if (!parseJobSystemTestLane(argc, selector, &selectedLane))
	{
		fprintf(stderr, "Usage: %s [--local-capacity]\n",
			argv != 0 && argc > 0 && argv[0] != 0 ? argv[0] :
				"core_job_system_tests");
		return 2;
	}
	g_testLane = selectedLane;
	if (isLocalCapacityLane())
	{
		printf("JobSystem test lane: local-capacity (maximum test-created "
			"workers=%u).\n", kLocalCapacityWorkerLimit);
		printf("External high-core throughput lane explicitly excluded: "
			"worker counts 16 and 32; the no-argument lane retains it.\n");
		printf("Local priority-throughput worker counts: 4, 8 and 12.\n");
		printf("Local deterministic and flat-range starts above 12, including "
			"automatic requests, use explicit 12-worker requests.\n");
		printf("Synthetic topology counts above 12 and the UINT_MAX rejection "
			"probe do not create worker threads.\n");
	}
	else
	{
		printf("JobSystem test lane: full (includes high-core throughput "
			"worker counts 16 and 32).\n");
	}
	int result = 0;
	result |= runTest("testJobSystemTestLaneSelection", testJobSystemTestLaneSelection);
	// Match headless startup before any subsystem or test has started workers.
	// Lazy model/pose preparation must not resurrect a compute pool here.
	rts::JobSystem &coldSystem = rts::JobSystem::instance();
	result |= check(!coldSystem.isRunning(), "cold scheduler has no compute workers");
	coldSystem.shutdown();
	result |= check(!coldSystem.ensureStarted() && !coldSystem.isRunning() &&
		coldSystem.workerCount() == 0 && coldSystem.outstandingJobCount() == 0,
		"headless cold shutdown blocks lazy compute startup");
	result |= runTest("testBasicStartSubmitWaitShutdown", testBasicStartSubmitWaitShutdown);
	result |= runTest("testDeterministicWorkerCounts", testDeterministicWorkerCounts);
	result |= runTest("testFlatRangePartitions", testFlatRangePartitions);
	result |= runTest("testFlatRangeKernelAndSaturationFallback", testFlatRangeKernelAndSaturationFallback);
	result |= runTest("testAvailableCpuSetsAndOwnerReservations", testAvailableCpuSetsAndOwnerReservations);
	result |= runTest("testOwnerRoleAndLazyRestartBasics", testOwnerRoleAndLazyRestartBasics);
#if !defined(_MSC_VER) || _MSC_VER >= 1300
	result |= runTest("testExecutionScopedPhysicalWorkerIdentity", testExecutionScopedPhysicalWorkerIdentity);
	result |= runTest("testPrioritiesAndWorkStealing", testPrioritiesAndWorkStealing);
	result |= runTest("testWideWorkerPriorityThroughput", testWideWorkerPriorityThroughput);
	result |= runTest("testCurrentWorkerWaitAccounting", testCurrentWorkerWaitAccounting);
	result |= runTest("testDependenciesContinuationsAndFailurePropagation", testDependenciesContinuationsAndFailurePropagation);
	result |= runTest("testCancellationOwnerHelpingAndCompletions", testCancellationOwnerHelpingAndCompletions);
	result |= runTest("testTopologyPoliciesAndStartupOptions", testTopologyPoliciesAndStartupOptions);
#if defined(RTS_BUILD_CORE_EXTRAS)
	result |= runTest("testHandleCompletionPublication", testHandleCompletionPublication);
	result |= runTest("testGroupCompletionPublication", testGroupCompletionPublication);
	result |= runTest("testBatchRecheckCannotRepublishRetiringExecution", testBatchRecheckCannotRepublishRetiringExecution);
	result |= runTest("testFaultInjectionAndRecovery", testFaultInjectionAndRecovery);
#endif
	result |= runTest("testBatchAdmissionAndWorkerWaitRejection", testBatchAdmissionAndWorkerWaitRejection);
	result |= runTest("testServiceOwnerLifetimeAndWorkerRoleRejection", testServiceOwnerLifetimeAndWorkerRoleRejection);
	result |= runTest("testProcessAffinityLimitsTopology", testProcessAffinityLimitsTopology);
	result |= runTest("testLifecycleOwnershipAndResourceLimits", testLifecycleOwnershipAndResourceLimits);
#endif
	if (result == 0)
	{
		printf("JobSystem tests passed.\n");
	}
	return result;
}
