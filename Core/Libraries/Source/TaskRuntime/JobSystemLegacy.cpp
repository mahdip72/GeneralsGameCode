#include "Lib/JobSystem.h"

#include <deque>
#include <limits.h>
#include <new>
#include <string.h>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace rts
{
namespace
{
struct LegacyGroupRecord
{
	LegacyGroupRecord(void *ownerValue, unsigned generationValue)
		: references(0), owner(ownerValue), generation(generationValue),
		  pending(0), failed(false), cancelled(false) {}

	unsigned references;
	void *owner;
	unsigned generation;
	unsigned pending;
	bool failed;
	bool cancelled;
};

void retainGroup(LegacyGroupRecord *record)
{
	if (record != 0) ++record->references;
}

void releaseGroup(LegacyGroupRecord *record)
{
	if (record != 0 && --record->references == 0) delete record;
}

struct LegacyJobRecord
{
	LegacyJobRecord()
		: references(0), group(0), complete(false), failed(false),
		  cancelled(false) {}
	~LegacyJobRecord() { releaseGroup(group); }

	unsigned references;
	LegacyGroupRecord *group;
	bool complete;
	bool failed;
	bool cancelled;
};

void retainJob(LegacyJobRecord *record)
{
	if (record != 0) ++record->references;
}

void releaseJob(LegacyJobRecord *record)
{
	if (record != 0 && --record->references == 0) delete record;
}

bool equalsAsciiNoCaseLegacy(const char *left, const char *right)
{
	if (left == 0 || right == 0) return false;
	while (*left != 0 && *right != 0)
	{
		char a = *left++;
		char b = *right++;
		if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
		if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
		if (a != b) return false;
	}
	return *left == 0 && *right == 0;
}

unsigned s_startupWorkerCountLegacy = 0;
JobWorkerPolicy s_startupWorkerPolicyLegacy = JOB_WORKER_POLICY_AUTO;

#if defined(RTS_BUILD_CORE_EXTRAS)
unsigned s_jobSystemTestFaultLegacy = 0;
unsigned s_jobSystemTestFaultOccurrenceLegacy = 0;

bool consumeJobSystemTestFaultLegacy(unsigned fault)
{
	if (s_jobSystemTestFaultLegacy != fault ||
		s_jobSystemTestFaultOccurrenceLegacy == 0) return false;
	--s_jobSystemTestFaultOccurrenceLegacy;
	if (s_jobSystemTestFaultOccurrenceLegacy != 0) return false;
	s_jobSystemTestFaultLegacy = 0;
	return true;
}
#endif

unsigned legacyExplicitWorkerLimit()
{
	unsigned cpuCount = 1;
#if defined(_WIN32)
	SYSTEM_INFO information;
	GetSystemInfo(&information);
	if (information.dwNumberOfProcessors != 0)
		cpuCount = information.dwNumberOfProcessors;
#endif
	const unsigned topologyLimit = cpuCount > UINT_MAX / 4 ?
		UINT_MAX : cpuCount * 4;
	return topologyLimit > 32 ? topologyLimit : 32;
}
}

struct JobContext::State
{
	State(LegacyGroupRecord *groupValue, unsigned char *scratchValue,
		unsigned scratchCapacityValue)
		: group(groupValue), scratch(scratchValue),
		  scratchCapacity(scratchCapacityValue), scratchUsed(0), failed(false) {}

	LegacyGroupRecord *group;
	unsigned char *scratch;
	unsigned scratchCapacity;
	unsigned scratchUsed;
	bool failed;
};

struct JobHandle::State
{
	explicit State(LegacyJobRecord *recordValue) : record(recordValue)
	{
		retainJob(record);
	}
	~State() { releaseJob(record); }
	LegacyJobRecord *record;
};

struct JobGroup::State
{
	explicit State(LegacyGroupRecord *recordValue) : record(recordValue)
	{
		retainGroup(record);
	}
	~State() { releaseGroup(record); }
	LegacyGroupRecord *record;
};

struct LegacySystemState
{
	struct CompletionItem
	{
		CompletionItem() : completion(0), succeeded(false), cancelled(false) {}
		OwnerCompletion *completion;
		bool succeeded;
		bool cancelled;
	};

	LegacySystemState()
		: running(false), stopping(false), configuredWorkerCount(0),
		  queueCapacity(0), generation(0) {}

	bool running;
	bool stopping;
	unsigned configuredWorkerCount;
	unsigned queueCapacity;
	unsigned generation;
	std::vector<unsigned char> scratch;
	std::deque<CompletionItem> completions;
	JobSystemMetrics metrics;
};

struct JobSystem::State : public LegacySystemState {};

JobCpuSetInfo::JobCpuSetInfo()
	: id(0), efficiencyClass(0), parked(false), allocatedToOtherProcess(false) {}

JobSystemConfig::JobSystemConfig()
	: workerCount(0), queueCapacity(0), scratchBytesPerWorker(0),
	  pinWorkers(true), workerPolicy(JOB_WORKER_POLICY_AUTO) {}

JobSystemMetrics::JobSystemMetrics()
	: submittedJobCount(0), executedJobCount(0), stealCount(0),
	  ownerHelpCount(0), waitCount(0), workerWaitRejectionCount(0),
	  failedJobCount(0), cancelledJobCount(0), serialFallbackCount(0),
	  totalQueueLatencyNanoseconds(0), maximumQueueLatencyNanoseconds(0),
	  workerSleepCount(0), workerWakeCount(0), injectionHighWater(0),
	  maximumActiveWorkers(0) {}

JobContext::JobContext(State *state) : m_state(state) {}

bool JobContext::isCancellationRequested() const
{
	return m_state != 0 && m_state->group != 0 && m_state->group->cancelled;
}

void JobContext::fail()
{
	if (m_state != 0) m_state->failed = true;
}

void *JobContext::allocateScratch(unsigned byteCount, unsigned alignment)
{
	if (m_state == 0 || m_state->scratch == 0 || byteCount == 0 ||
		alignment == 0 || (alignment & (alignment - 1)) != 0) return 0;
	const unsigned aligned = (m_state->scratchUsed + alignment - 1) &
		~(alignment - 1);
	if (aligned > m_state->scratchCapacity ||
		byteCount > m_state->scratchCapacity - aligned) return 0;
	m_state->scratchUsed = aligned + byteCount;
	return m_state->scratch + aligned;
}

Job::Job() {}
Job::~Job() {}
OwnerCompletion::OwnerCompletion() {}
OwnerCompletion::~OwnerCompletion() {}

JobSubmission::JobSubmission()
	: job(0), priority(JOB_PRIORITY_NORMAL), completion(0), dependencies(0),
	  dependencyCount(0) {}

JobHandle::JobHandle() : m_state(0) {}
JobHandle::JobHandle(State *state) : m_state(state) {}
JobHandle::JobHandle(const JobHandle &other) : m_state(0)
{
	if (other.m_state != 0) m_state = new State(other.m_state->record);
}
JobHandle::~JobHandle() { delete m_state; }
JobHandle &JobHandle::operator=(const JobHandle &other)
{
	if (this != &other)
	{
		State *replacement = other.m_state != 0 ?
			new State(other.m_state->record) : 0;
		delete m_state;
		m_state = replacement;
	}
	return *this;
}
bool JobHandle::isValid() const { return m_state != 0 && m_state->record != 0; }
bool JobHandle::isComplete() const { return isValid() && m_state->record->complete; }
bool JobHandle::succeeded() const { return isComplete() && !failed() && !wasCancelled(); }
bool JobHandle::failed() const { return isValid() && m_state->record->failed; }
bool JobHandle::wasCancelled() const { return isValid() && m_state->record->cancelled; }

JobGroup::JobGroup() : m_state(0) {}
JobGroup::JobGroup(State *state) : m_state(state) {}
JobGroup::JobGroup(const JobGroup &other) : m_state(0)
{
	if (other.m_state != 0) m_state = new State(other.m_state->record);
}
JobGroup::~JobGroup() { delete m_state; }
JobGroup &JobGroup::operator=(const JobGroup &other)
{
	if (this != &other)
	{
		State *replacement = other.m_state != 0 ?
			new State(other.m_state->record) : 0;
		delete m_state;
		m_state = replacement;
	}
	return *this;
}
bool JobGroup::isValid() const { return m_state != 0 && m_state->record != 0; }
bool JobGroup::isComplete() const { return isValid() && m_state->record->pending == 0; }
bool JobGroup::failed() const { return isValid() && m_state->record->failed; }
bool JobGroup::wasCancelled() const { return isValid() && m_state->record->cancelled; }

JobSystem::JobSystem() : m_state(new State) {}
JobSystem::~JobSystem() { shutdown(); delete m_state; }
JobSystem &JobSystem::instance()
{
	/* VC6 cannot register a private destructor for a function-local static. */
	static JobSystem *system = new JobSystem;
	return *system;
}

unsigned JobSystem::chooseWorkerCount(unsigned eligibleLogicalCpuCount,
	JobWorkerPolicy policy, unsigned explicitWorkerCount)
{
	if (explicitWorkerCount != 0) return explicitWorkerCount;
	if (eligibleLogicalCpuCount == 0) return 1;
	if (policy == JOB_WORKER_POLICY_ALL) return eligibleLogicalCpuCount;
	const unsigned reserved = eligibleLogicalCpuCount >= 8 ? 2 : 1;
	return eligibleLogicalCpuCount > reserved ?
		eligibleLogicalCpuCount - reserved : 1;
}

unsigned JobSystem::selectWorkerCpuSets(const JobCpuSetInfo *cpuSets,
	unsigned cpuSetCount, JobWorkerPolicy policy,
	unsigned explicitWorkerCount, unsigned *selectedIds,
	unsigned selectedIdCapacity)
{
	if (cpuSets == 0 || selectedIds == 0 || selectedIdCapacity == 0) return 0;
	std::vector<JobCpuSetInfo> eligible;
	try
	{
		unsigned index;
		for (index = 0; index < cpuSetCount; ++index)
		{
			if (!cpuSets[index].parked && !cpuSets[index].allocatedToOtherProcess)
				eligible.push_back(cpuSets[index]);
		}
		for (index = 1; index < eligible.size(); ++index)
		{
			JobCpuSetInfo value = eligible[index];
			unsigned position = index;
			while (position != 0 &&
				(eligible[position - 1].efficiencyClass < value.efficiencyClass ||
				 (eligible[position - 1].efficiencyClass == value.efficiencyClass &&
				  eligible[position - 1].id > value.id)))
			{
				eligible[position] = eligible[position - 1];
				--position;
			}
			eligible[position] = value;
		}
	}
	catch (...) { return 0; }
	const unsigned eligibleCount = (unsigned)eligible.size();
	const unsigned requested = chooseWorkerCount(eligibleCount, policy,
		explicitWorkerCount);
	const unsigned selectedCount = requested < eligibleCount ? requested : eligibleCount;
	unsigned first = 0;
	if (explicitWorkerCount == 0 && policy == JOB_WORKER_POLICY_AUTO &&
		eligibleCount > selectedCount) first = eligibleCount - selectedCount;
	const unsigned writable = selectedCount < selectedIdCapacity ?
		selectedCount : selectedIdCapacity;
	unsigned index;
	for (index = 0; index < writable; ++index) selectedIds[index] = eligible[first + index].id;
	return writable;
}

bool JobSystem::setStartupWorkerCount(unsigned workerCount)
{
	s_startupWorkerCountLegacy = workerCount;
	return true;
}
bool JobSystem::setStartupWorkerPolicy(const char *policy)
{
	if (equalsAsciiNoCaseLegacy(policy, "auto"))
	{
		s_startupWorkerPolicyLegacy = JOB_WORKER_POLICY_AUTO;
		return true;
	}
	if (equalsAsciiNoCaseLegacy(policy, "all"))
	{
		s_startupWorkerPolicyLegacy = JOB_WORKER_POLICY_ALL;
		return true;
	}
	return false;
}
JobSystemConfig JobSystem::startupConfig()
{
	JobSystemConfig config;
	config.workerCount = s_startupWorkerCountLegacy;
	config.workerPolicy = s_startupWorkerPolicyLegacy;
	config.queueCapacity = 4096;
	config.scratchBytesPerWorker = 64u * 1024u;
	config.pinWorkers = false;
	return config;
}

bool JobSystem::start(const JobSystemConfig &config)
{
#if defined(RTS_BUILD_CORE_EXTRAS)
	if (consumeJobSystemTestFaultLegacy(1)) return false;
#endif
	if (m_state == 0 || m_state->running || config.queueCapacity == 0 ||
		config.scratchBytesPerWorker == 0 ||
		(config.workerPolicy != JOB_WORKER_POLICY_AUTO &&
		 config.workerPolicy != JOB_WORKER_POLICY_ALL) ||
		(config.workerCount != 0 &&
		 config.workerCount > legacyExplicitWorkerLimit())) return false;
	/* This adapter is the VC6 differential oracle: it executes inline and
	 * must report the one lane it actually owns, not the requested count. */
	const unsigned count = 1;
	try
	{
		m_state->scratch.resize(config.scratchBytesPerWorker);
	}
	catch (...) { return false; }
	++m_state->generation;
	m_state->configuredWorkerCount = count;
	m_state->queueCapacity = config.queueCapacity;
	m_state->metrics = JobSystemMetrics();
	m_state->running = true;
	m_state->stopping = false;
	return true;
}
bool JobSystem::ensureStarted() { return isRunning() || start(startupConfig()); }
void JobSystem::shutdown()
{
	if (m_state == 0) return;
	m_state->stopping = true;
	pumpOwnerCompletions((unsigned)-1);
	m_state->completions.clear();
	m_state->scratch.clear();
	m_state->configuredWorkerCount = 0;
	m_state->queueCapacity = 0;
	m_state->running = false;
	m_state->stopping = false;
}
bool JobSystem::isRunning() const { return m_state != 0 && m_state->running; }
unsigned JobSystem::workerCount() const { return isRunning() ? m_state->configuredWorkerCount : 0; }
bool JobSystem::isWorkerThread() const { return false; }
unsigned JobSystem::outstandingJobCount() const { return 0; }
JobSystemMetrics JobSystem::metrics() const { return m_state != 0 ? m_state->metrics : JobSystemMetrics(); }
void JobSystem::resetMetrics() { if (m_state != 0) m_state->metrics = JobSystemMetrics(); }
void JobSystem::recordSerialFallback() { if (m_state != 0) ++m_state->metrics.serialFallbackCount; }

JobGroup JobSystem::createGroup()
{
	if (!isRunning() || m_state->stopping) return JobGroup();
	try
	{
		LegacyGroupRecord *record = new LegacyGroupRecord(m_state,
			m_state->generation);
		return JobGroup(new JobGroup::State(record));
	}
	catch (...) { return JobGroup(); }
}

JobHandle JobSystem::trySubmit(Job *job, JobPriority priority,
	const JobGroup &group)
{
	return trySubmitAfter(job, priority, group, 0, 0, 0);
}
JobHandle JobSystem::trySubmit(Job *job, JobPriority priority,
	const JobGroup &group, OwnerCompletion *completion)
{
	return trySubmitAfter(job, priority, group, 0, 0, completion);
}
JobHandle JobSystem::trySubmitAfter(Job *job, JobPriority priority,
	const JobGroup &group, const JobHandle *dependencies,
	unsigned dependencyCount)
{
	return trySubmitAfter(job, priority, group, dependencies,
		dependencyCount, 0);
}

JobHandle JobSystem::trySubmitAfter(Job *job, JobPriority priority,
	const JobGroup &group, const JobHandle *dependencies,
	unsigned dependencyCount, OwnerCompletion *completion)
{
	if (job == 0 || priority < JOB_PRIORITY_FRAME_CRITICAL ||
		priority >= JOB_PRIORITY_COUNT || !isRunning() || m_state->stopping ||
		!group.isValid() || (dependencyCount != 0 && dependencies == 0))
		return JobHandle();
	LegacyGroupRecord *groupRecord = group.m_state->record;
	if (groupRecord->owner != m_state || groupRecord->generation != m_state->generation ||
		groupRecord->cancelled) return JobHandle();
	bool dependencyFailed = false;
	unsigned index;
	for (index = 0; index < dependencyCount; ++index)
	{
		if (!dependencies[index].isValid() || !dependencies[index].isComplete() ||
			dependencies[index].m_state->record->group == 0 ||
			dependencies[index].m_state->record->group->owner != m_state ||
			dependencies[index].m_state->record->group->generation != m_state->generation)
			return JobHandle();
		unsigned previous;
		for (previous = 0; previous < index; ++previous)
		{
			if (dependencies[previous].m_state->record ==
				dependencies[index].m_state->record) return JobHandle();
		}
		if (dependencies[index].failed() || dependencies[index].wasCancelled())
			dependencyFailed = true;
	}
	LegacyJobRecord *record = 0;
	JobHandle::State *handleState = 0;
	try
	{
		record = new LegacyJobRecord;
		record->group = groupRecord;
		retainGroup(groupRecord);
		handleState = new JobHandle::State(record);
	}
	catch (...)
	{
		delete handleState;
		if (record != 0 && record->references == 0) delete record;
		return JobHandle();
	}
	++groupRecord->pending;
	++m_state->metrics.submittedJobCount;
	bool cancelled = groupRecord->cancelled;
	bool failed = dependencyFailed;
	if (!cancelled && !failed)
	{
		JobContext::State contextState(groupRecord,
			m_state->scratch.empty() ? 0 : &m_state->scratch[0],
			(unsigned)m_state->scratch.size());
		JobContext context(&contextState);
		try { job->execute(context); failed = contextState.failed; }
		catch (...) { failed = true; }
	}
	delete job;
	if (completion != 0)
	{
		try
		{
			LegacySystemState::CompletionItem item;
			item.completion = completion;
			item.succeeded = !failed && !cancelled;
			item.cancelled = cancelled;
			m_state->completions.push_back(item);
		}
		catch (...) { delete completion; failed = true; }
	}
	record->failed = failed;
	record->cancelled = cancelled;
	record->complete = true;
	groupRecord->failed = groupRecord->failed || failed;
	groupRecord->cancelled = groupRecord->cancelled || cancelled;
	--groupRecord->pending;
	++m_state->metrics.executedJobCount;
	if (failed) ++m_state->metrics.failedJobCount;
	if (cancelled) ++m_state->metrics.cancelledJobCount;
	m_state->metrics.maximumActiveWorkers = 1;
	return JobHandle(handleState);
}

JobHandle JobSystem::then(const JobHandle &prerequisite, Job *job,
	JobPriority priority, const JobGroup &group)
{
	return trySubmitAfter(job, priority, group, &prerequisite, 1, 0);
}

bool JobSystem::trySubmitBatch(const JobSubmission *submissions,
	unsigned submissionCount, const JobGroup &group, JobHandle *handles)
{
	if (submissions == 0 || submissionCount == 0 || handles == 0 ||
		!group.isValid() || !isRunning() || submissionCount > m_state->queueCapacity)
		return false;
	LegacyGroupRecord *groupRecord = group.m_state->record;
	if (groupRecord->owner != m_state ||
		groupRecord->generation != m_state->generation || groupRecord->cancelled)
		return false;
	unsigned index;
	for (index = 0; index < submissionCount; ++index)
	{
#if defined(RTS_BUILD_CORE_EXTRAS)
		if (consumeJobSystemTestFaultLegacy(6)) return false;
#endif
		if (submissions[index].job == 0 ||
			submissions[index].priority < JOB_PRIORITY_FRAME_CRITICAL ||
			submissions[index].priority >= JOB_PRIORITY_COUNT ||
			(submissions[index].dependencyCount != 0 &&
			 submissions[index].dependencies == 0)) return false;
		unsigned previous;
		for (previous = 0; previous < index; ++previous)
		{
			if (submissions[previous].job == submissions[index].job) return false;
		}
		unsigned dependencyIndex;
		for (dependencyIndex = 0;
			dependencyIndex < submissions[index].dependencyCount;
			++dependencyIndex)
		{
			const JobHandle &dependency =
				submissions[index].dependencies[dependencyIndex];
			if (!dependency.isValid() || !dependency.isComplete() ||
				dependency.m_state->record->group == 0 ||
				dependency.m_state->record->group->owner != m_state ||
				dependency.m_state->record->group->generation != m_state->generation)
				return false;
			unsigned previousDependency;
			for (previousDependency = 0;
				previousDependency < dependencyIndex; ++previousDependency)
			{
				if (submissions[index].dependencies[previousDependency].m_state->record ==
					dependency.m_state->record) return false;
			}
		}
	}

	LegacyJobRecord **records = 0;
	JobHandle::State **handleStates = 0;
	try
	{
		records = new LegacyJobRecord *[submissionCount];
		handleStates = new JobHandle::State *[submissionCount];
		for (index = 0; index < submissionCount; ++index)
		{
			records[index] = 0;
			handleStates[index] = 0;
		}
		for (index = 0; index < submissionCount; ++index)
		{
			records[index] = new LegacyJobRecord;
			records[index]->group = groupRecord;
			retainGroup(groupRecord);
			handleStates[index] = new JobHandle::State(records[index]);
		}
	}
	catch (...)
	{
		if (records != 0 && handleStates != 0)
		{
			for (index = 0; index < submissionCount; ++index)
			{
				if (handleStates[index] != 0) delete handleStates[index];
				else if (records[index] != 0) delete records[index];
			}
		}
		delete [] handleStates;
		delete [] records;
		return false;
	}

	groupRecord->pending += submissionCount;
	for (index = 0; index < submissionCount; ++index)
	{
		bool dependencyFailed = false;
		unsigned dependencyIndex;
		for (dependencyIndex = 0;
			dependencyIndex < submissions[index].dependencyCount;
			++dependencyIndex)
		{
			if (submissions[index].dependencies[dependencyIndex].failed() ||
				submissions[index].dependencies[dependencyIndex].wasCancelled())
				dependencyFailed = true;
		}
		bool cancelled = groupRecord->cancelled;
		bool failed = dependencyFailed;
		if (!cancelled && !failed)
		{
			JobContext::State contextState(groupRecord,
				m_state->scratch.empty() ? 0 : &m_state->scratch[0],
				(unsigned)m_state->scratch.size());
			JobContext context(&contextState);
			try
			{
				submissions[index].job->execute(context);
				failed = contextState.failed;
			}
			catch (...) { failed = true; }
		}
		delete submissions[index].job;
		if (submissions[index].completion != 0)
		{
			try
			{
				LegacySystemState::CompletionItem item;
				item.completion = submissions[index].completion;
				item.succeeded = !failed && !cancelled;
				item.cancelled = cancelled;
				m_state->completions.push_back(item);
			}
			catch (...)
			{
				delete submissions[index].completion;
				failed = true;
			}
		}
		records[index]->failed = failed;
		records[index]->cancelled = cancelled;
		records[index]->complete = true;
		groupRecord->failed = groupRecord->failed || failed;
		groupRecord->cancelled = groupRecord->cancelled || cancelled;
		--groupRecord->pending;
		++m_state->metrics.submittedJobCount;
		++m_state->metrics.executedJobCount;
		if (failed) ++m_state->metrics.failedJobCount;
		if (cancelled) ++m_state->metrics.cancelledJobCount;
		m_state->metrics.maximumActiveWorkers = 1;
		delete handles[index].m_state;
		handles[index].m_state = handleStates[index];
		handleStates[index] = 0;
	}
	delete [] handleStates;
	delete [] records;
	return true;
}

bool JobSystem::tryPromote(Job *, JobPriority)
{
	return false;
}

bool JobSystem::wait(const JobHandle &handle)
{
	if (!handle.isValid()) return false;
	++m_state->metrics.waitCount;
	return handle.isComplete();
}
bool JobSystem::wait(const JobGroup &group)
{
	if (!group.isValid()) return false;
	++m_state->metrics.waitCount;
	return group.isComplete();
}
bool JobSystem::cancel(const JobGroup &group)
{
	if (!group.isValid()) return false;
	group.m_state->record->cancelled = true;
	return true;
}
unsigned JobSystem::pumpOwnerCompletions(unsigned maximumCount)
{
	if (m_state == 0 || maximumCount == 0) return 0;
	unsigned count = 0;
	while (count < maximumCount && !m_state->completions.empty())
	{
		LegacySystemState::CompletionItem item = m_state->completions.front();
		m_state->completions.pop_front();
		try { item.completion->complete(item.succeeded, item.cancelled); }
		catch (...) {}
		delete item.completion;
		++count;
	}
	return count;
}
unsigned JobSystem::pendingOwnerCompletionCount() const
{
	return m_state != 0 ? (unsigned)m_state->completions.size() : 0;
}

#if defined(RTS_BUILD_CORE_EXTRAS)
extern "C" void rts_job_system_set_test_fault(unsigned fault,
	unsigned occurrence)
{
	s_jobSystemTestFaultOccurrenceLegacy = occurrence;
	s_jobSystemTestFaultLegacy = fault;
}
#endif
}
