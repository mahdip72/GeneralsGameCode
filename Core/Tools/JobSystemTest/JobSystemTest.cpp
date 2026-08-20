#include "Lib/JobSystem.h"

#include <limits.h>
#include <stdio.h>

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
	JOB_SYSTEM_TEST_FAIL_PROMOTION_PUSH = 8
};

extern "C" void rts_job_system_set_test_fault(unsigned fault,
	unsigned occurrence);
#endif

namespace
{
int check(bool condition, const char *message)
{
	if (!condition)
	{
		fprintf(stderr, "FAIL: %s\n", message);
		return 1;
	}
	return 0;
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
	const unsigned workerCounts[] = { 1, 2, 4, 8, 16 };
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
		config.workerCount = workerCounts[workerIndex];
		config.queueCapacity = jobCount;
		config.scratchBytesPerWorker = 4096;
		config.pinWorkers = false;

		rts::JobSystem &system = rts::JobSystem::instance();
		result |= check(system.start(config), "configured worker count starts");
#if defined(_MSC_VER) && _MSC_VER < 1300
		result |= check(system.workerCount() == 1,
			"VC6 reference adapter reports its single execution lane");
#else
		result |= check(system.workerCount() == workerCounts[workerIndex],
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
		result |= check(system.wait(group), "priority group completes");
		result |= check(backgroundOrder == 1 && criticalOrder == 2,
			"promoted job runs before existing frame-critical work");
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

	config.workerCount = UINT_MAX;
	result |= check(!system.start(config) && !system.isRunning(),
		"topology-derived resource limit rejects pathological worker counts");
	return result;
}
#endif
}

int main()
{
	int result = 0;
	result |= testBasicStartSubmitWaitShutdown();
	result |= testDeterministicWorkerCounts();
#if !defined(_MSC_VER) || _MSC_VER >= 1300
	result |= testPrioritiesAndWorkStealing();
	result |= testDependenciesContinuationsAndFailurePropagation();
	result |= testCancellationOwnerHelpingAndCompletions();
	result |= testTopologyPoliciesAndStartupOptions();
#if defined(RTS_BUILD_CORE_EXTRAS)
	result |= testFaultInjectionAndRecovery();
#endif
	result |= testBatchAdmissionAndWorkerWaitRejection();
	result |= testLifecycleOwnershipAndResourceLimits();
#endif
	if (result == 0)
	{
		printf("JobSystem tests passed.\n");
	}
	return result;
}
