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

namespace rts
{
#if defined(_MSC_VER) && _MSC_VER < 1300
typedef unsigned __int64 JobMetricCounter;
#else
typedef unsigned long long JobMetricCounter;
#endif

enum JobPriority
{
	JOB_PRIORITY_FRAME_CRITICAL = 0,
	JOB_PRIORITY_NORMAL = 1,
	JOB_PRIORITY_STREAMING = 2,
	JOB_PRIORITY_BACKGROUND = 3,
	JOB_PRIORITY_COUNT = 4
};

enum JobWorkerPolicy
{
	JOB_WORKER_POLICY_AUTO = 0,
	JOB_WORKER_POLICY_ALL = 1
};

enum JobOwnerRole
{
	JOB_OWNER_GAME = 0,
	JOB_OWNER_RENDER,
	JOB_OWNER_AUDIO,
	JOB_OWNER_NETWORK,
	JOB_OWNER_IO,
	JOB_OWNER_COUNT
};

struct JobRange
{
	unsigned begin;
	unsigned end;
};

struct JobCpuSetInfo
{
	JobCpuSetInfo();

	unsigned id;
	unsigned efficiencyClass;
	unsigned group;
	unsigned coreIndex;
	unsigned logicalProcessorIndex;
	bool parked;
	bool allocatedToOtherProcess;
	bool availableToProcess;
};

struct JobSystemConfig
{
	JobSystemConfig();

	unsigned workerCount;
	unsigned queueCapacity;
	unsigned scratchBytesPerWorker;
	bool pinWorkers;
	JobWorkerPolicy workerPolicy;
};

struct JobSystemMetrics
{
	JobSystemMetrics();

	JobMetricCounter submittedJobCount;
	JobMetricCounter executedJobCount;
	JobMetricCounter stealCount;
	JobMetricCounter ownerHelpCount;
	JobMetricCounter waitCount;
	JobMetricCounter workerWaitRejectionCount;
	JobMetricCounter failedJobCount;
	JobMetricCounter cancelledJobCount;
	JobMetricCounter serialFallbackCount;
	JobMetricCounter totalQueueLatencyNanoseconds;
	JobMetricCounter maximumQueueLatencyNanoseconds;
	JobMetricCounter workerSleepCount;
	JobMetricCounter workerWakeCount;
	JobMetricCounter affinityFailureCount;
	unsigned injectionHighWater;
	unsigned maximumActiveWorkers;
	unsigned availableLogicalCpuCount;
	unsigned reservedOwnerCpuCount;
	unsigned selectedWorkerCpuCount;
};

class JobContext
{
public:
	bool isCancellationRequested() const;
	void fail();
	void *allocateScratch(unsigned byteCount, unsigned alignment);

private:
	friend class JobSystem;
	struct State;
	explicit JobContext(State *state);
	JobContext(const JobContext &);
	JobContext &operator=(const JobContext &);

	State *m_state;
};

class Job
{
public:
	Job();
	virtual ~Job();
	virtual void execute(JobContext &context) = 0;

private:
	Job(const Job &);
	Job &operator=(const Job &);
};

class OwnerCompletion
{
public:
	OwnerCompletion();
	virtual ~OwnerCompletion();
	virtual void complete(bool succeeded, bool cancelled) = 0;

private:
	OwnerCompletion(const OwnerCompletion &);
	OwnerCompletion &operator=(const OwnerCompletion &);
};

class JobHandle;

struct JobSubmission
{
	JobSubmission();

	Job *job;
	JobPriority priority;
	OwnerCompletion *completion;
	const JobHandle *dependencies;
	unsigned dependencyCount;
};

class JobHandle
{
public:
	JobHandle();
	JobHandle(const JobHandle &other);
	~JobHandle();
	JobHandle &operator=(const JobHandle &other);

	bool isValid() const;
	bool isComplete() const;
	bool succeeded() const;
	bool failed() const;
	bool wasCancelled() const;

private:
	friend class JobSystem;
	struct State;
	explicit JobHandle(State *state);
	State *m_state;
};

class JobGroup
{
public:
	JobGroup();
	JobGroup(const JobGroup &other);
	~JobGroup();
	JobGroup &operator=(const JobGroup &other);

	bool isValid() const;
	bool isComplete() const;
	bool failed() const;
	bool wasCancelled() const;

private:
	friend class JobSystem;
	struct State;
	explicit JobGroup(State *state);
	State *m_state;
};

class JobSystem
{
public:
	static JobSystem &instance();
	// Allocation-free partitions of immutable input and disjoint output storage.
	static unsigned chooseRangeCount(unsigned itemCount,
		unsigned minimumItemsPerRange, unsigned workerCount);
	static bool rangeForIndex(unsigned itemCount, unsigned rangeCount,
		unsigned rangeIndex, JobRange &range);
	static unsigned chooseWorkerCount(unsigned eligibleLogicalCpuCount,
		JobWorkerPolicy policy, unsigned explicitWorkerCount);
	static unsigned selectOwnerCpuSets(const JobCpuSetInfo *cpuSets,
		unsigned cpuSetCount, JobWorkerPolicy policy,
		unsigned explicitWorkerCount, unsigned *selectedIds,
		unsigned selectedIdCapacity);
	static unsigned selectWorkerCpuSets(const JobCpuSetInfo *cpuSets,
		unsigned cpuSetCount, JobWorkerPolicy policy,
		unsigned explicitWorkerCount, unsigned *selectedIds,
		unsigned selectedIdCapacity);
	static bool setStartupWorkerCount(unsigned workerCount);
	static bool setStartupWorkerPolicy(const char *policy);
	static JobSystemConfig startupConfig();

	bool start(const JobSystemConfig &config);
	// Lazy startup is disabled after shutdown; only explicit start can restart.
	bool ensureStarted();
	void shutdown();
	// One execution thread per role and one role per thread. Registration never
	// starts compute workers; optional affinity failure is reported in metrics.
	// Unregister on that thread after native destruction, even after shutdown.
	bool registerCurrentThread(JobOwnerRole role);
	bool unregisterCurrentThread(JobOwnerRole role);
	bool isCurrentThread(JobOwnerRole role) const;
	bool isRunning() const;
	bool isWorkerThread() const;
	unsigned workerCount() const;
	unsigned outstandingJobCount() const;
	JobSystemMetrics metrics() const;
	void resetMetrics();
	void recordSerialFallback();

	JobGroup createGroup();
	JobHandle trySubmit(Job *job, JobPriority priority,
		const JobGroup &group);
	JobHandle trySubmit(Job *job, JobPriority priority,
		const JobGroup &group, OwnerCompletion *completion);
	JobHandle trySubmitAfter(Job *job, JobPriority priority,
		const JobGroup &group, const JobHandle *dependencies,
		unsigned dependencyCount);
	JobHandle trySubmitAfter(Job *job, JobPriority priority,
		const JobGroup &group, const JobHandle *dependencies,
		unsigned dependencyCount, OwnerCompletion *completion);
	JobHandle then(const JobHandle &prerequisite, Job *job,
		JobPriority priority, const JobGroup &group);
	bool trySubmitBatch(const JobSubmission *submissions,
		unsigned submissionCount, const JobGroup &group,
		JobHandle *handles);
	bool tryPromote(Job *job, JobPriority priority);
	bool wait(const JobHandle &handle);
	bool wait(const JobGroup &group);
	bool cancel(const JobGroup &group);
	unsigned pumpOwnerCompletions(unsigned maximumCount);
	unsigned pendingOwnerCompletionCount() const;

private:
	JobSystem();
	~JobSystem();
	JobSystem(const JobSystem &);
	JobSystem &operator=(const JobSystem &);
	bool startInternal(const JobSystemConfig &config, bool allowRestart);

	struct State;
	State *m_state;
};
}
