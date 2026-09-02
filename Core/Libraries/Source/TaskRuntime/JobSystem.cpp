#include "Lib/JobSystem.h"
#include "Lib/PipelineExecutionPolicy.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <new>
#include <shared_mutex>
#include <thread>
#include <vector>

#if defined(_WIN32)
#if !defined(NOMINMAX)
#define NOMINMAX
#endif
#if !defined(_WIN32_WINNT)
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>
#endif

#if defined(_WIN32) && !defined(_WIN64)
#include <float.h>
#include <xmmintrin.h>
#endif

namespace rts
{
namespace
{
struct GroupRecord
{
	GroupRecord(void *ownerValue, unsigned generationValue)
		: owner(ownerValue), generation(generationValue), pending(0),
		  failed(false), cancelled(false)
	{
	}
	void removePending(unsigned count);

	void *owner;
	unsigned generation;
	std::atomic<unsigned> pending;
	std::atomic<bool> failed;
	std::atomic<bool> cancelled;
	std::mutex mutex;
	std::condition_variable completed;
};

struct JobRecord
{
	JobRecord()
		: job(0), priority(JOB_PRIORITY_NORMAL), complete(false),
		  failed(false), cancelled(false), owner(0), generation(0),
		  unresolvedDependencies(0), dependencyFailed(false), finalizing(false),
		  accepted(false), queued(false), executing(false),
		  readyPublicationFailed(false), completion(0)
	{
	}

	~JobRecord()
	{
		delete completion;
		delete job.exchange(0, std::memory_order_acq_rel);
	}

	std::atomic<Job *> job;
	JobPriority priority;
	std::shared_ptr<GroupRecord> group;
	std::atomic<bool> complete;
	std::atomic<bool> failed;
	std::atomic<bool> cancelled;
	void *owner;
	unsigned generation;
	std::atomic<unsigned> unresolvedDependencies;
	std::atomic<bool> dependencyFailed;
	std::atomic<bool> finalizing;
	std::atomic<bool> accepted;
	std::atomic<bool> queued;
	std::atomic<bool> executing;
	std::atomic<bool> readyPublicationFailed;
	OwnerCompletion *completion;
	std::chrono::steady_clock::time_point readyAt;
	std::mutex publicationMutex;
	std::mutex dependentsMutex;
	std::vector<std::shared_ptr<JobRecord> > dependents;
	std::mutex mutex;
	std::condition_variable completed;
};

thread_local void *s_currentJobSystemWorker = 0;
thread_local void *s_currentJobSystemWorkerQueue = 0;
std::atomic<unsigned> s_startupWorkerCount(0);
std::atomic<unsigned> s_startupWorkerPolicy(JOB_WORKER_POLICY_AUTO);

unsigned long long performanceClockNanoseconds()
{
	return static_cast<unsigned long long>(
		std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count());
}

void saturatingCounterAdd(unsigned long long &value,
	unsigned long long increment)
{
	const unsigned long long maximum = ~0ull;
	value = increment > maximum - value ? maximum : value + increment;
}

void saturatingCounterIncrement(unsigned long long &value)
{
	saturatingCounterAdd(value, 1);
}

void updateCounterMaximum(unsigned long long &value,
	unsigned long long sample)
{
	if (sample > value) value = sample;
}

#if defined(RTS_BUILD_CORE_EXTRAS)
std::atomic<unsigned> s_jobSystemTestFault(0);
std::atomic<unsigned> s_jobSystemTestFaultOccurrence(0);
std::atomic<unsigned> s_jobSystemTestPauseMask(0);
std::atomic<unsigned> s_jobSystemTestPauseReachedMask(0);
std::atomic<unsigned> s_jobSystemTestPauseReleasedMask(0);

bool consumeJobSystemTestFault(unsigned fault)
{
	if (s_jobSystemTestFault.load(std::memory_order_acquire) != fault)
	{
		return false;
	}
	unsigned occurrence = s_jobSystemTestFaultOccurrence.load(
		std::memory_order_acquire);
	while (occurrence != 0)
	{
		if (s_jobSystemTestFaultOccurrence.compare_exchange_weak(occurrence,
			occurrence - 1, std::memory_order_acq_rel,
			std::memory_order_acquire))
		{
			if (occurrence == 1)
			{
				s_jobSystemTestFault.store(0, std::memory_order_release);
				return true;
			}
			return false;
		}
	}
	return false;
}

void pauseJobSystemTest(unsigned pausePoint)
{
	if ((s_jobSystemTestPauseMask.load(std::memory_order_acquire) & pausePoint) == 0)
	{
		return;
	}
	s_jobSystemTestPauseReachedMask.fetch_or(pausePoint, std::memory_order_acq_rel);
	while ((s_jobSystemTestPauseMask.load(std::memory_order_acquire) & pausePoint) != 0 &&
		(s_jobSystemTestPauseReleasedMask.load(std::memory_order_acquire) &
			pausePoint) == 0)
	{
		std::this_thread::yield();
	}
}
#endif

std::unique_lock<std::mutex> lockCompletionMutex(std::mutex &mutex,
	unsigned testPausePoint)
{
	std::unique_lock<std::mutex> lock(mutex, std::defer_lock);
#if defined(RTS_BUILD_CORE_EXTRAS)
	if ((s_jobSystemTestPauseMask.load(std::memory_order_acquire) & testPausePoint) != 0 &&
		!lock.try_lock())
	{
		pauseJobSystemTest(testPausePoint);
	}
#else
	(void)testPausePoint;
#endif
	if (!lock.owns_lock()) lock.lock();
	return lock;
}

void GroupRecord::removePending(unsigned count)
{
	bool complete;
	{
		// Publish the predicate under the same mutex used by completion waiters.
		std::unique_lock<std::mutex> lock = lockCompletionMutex(mutex, 512);
		complete = pending.fetch_sub(count, std::memory_order_acq_rel) == count;
	}
	if (complete)
	{
		completed.notify_all();
#if defined(RTS_BUILD_CORE_EXTRAS)
		pauseJobSystemTest(256);
#endif
	}
}

#if defined(_WIN32) && !defined(_WIN64)
class DeterministicFloatingPointScope
{
public:
	DeterministicFloatingPointScope()
		: m_controlWord(_controlfp(0, 0)), m_mxcsr(_mm_getcsr())
	{
		_fpreset();
		_controlfp(_PC_24 | _RC_NEAR, _MCW_PC | _MCW_RC);
		_mm_setcsr(0x1f80u);
	}

	~DeterministicFloatingPointScope()
	{
		_controlfp(m_controlWord, _MCW_PC | _MCW_RC);
		_mm_setcsr(m_mxcsr);
	}

private:
	unsigned int m_controlWord;
	unsigned int m_mxcsr;
};
#endif

bool equalsAsciiNoCase(const char *left, const char *right)
{
	if (left == 0 || right == 0)
	{
		return false;
	}
	while (*left != 0 && *right != 0)
	{
		char leftValue = *left;
		char rightValue = *right;
		if (leftValue >= 'A' && leftValue <= 'Z')
		{
			leftValue = static_cast<char>(leftValue - 'A' + 'a');
		}
		if (rightValue >= 'A' && rightValue <= 'Z')
		{
			rightValue = static_cast<char>(rightValue - 'A' + 'a');
		}
		if (leftValue != rightValue)
		{
			return false;
		}
		++left;
		++right;
	}
	return *left == 0 && *right == 0;
}

#if defined(_WIN32)
bool getSelectedCpuSets(HANDLE target, bool process,
	std::vector<unsigned> &selectedIds)
{
	typedef BOOL (WINAPI *GetSelectedCpuSetsFunction)(HANDLE, PULONG,
		ULONG, PULONG);
	HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
	GetSelectedCpuSetsFunction getSelected = kernel != 0 ?
		reinterpret_cast<GetSelectedCpuSetsFunction>(GetProcAddress(kernel,
			process ? "GetProcessDefaultCpuSets" : "GetThreadSelectedCpuSets")) : 0;
	if (getSelected == 0) return false;
	ULONG count = 0;
	if (!getSelected(target, 0, 0, &count) &&
		GetLastError() != ERROR_INSUFFICIENT_BUFFER) return false;
	if (count == 0) return true;
	if (count > 65536) return false;
	selectedIds.resize(count);
	if (!getSelected(target, reinterpret_cast<PULONG>(&selectedIds[0]),
		count, &count) || count > selectedIds.size())
	{
		selectedIds.clear();
		return false;
	}
	selectedIds.resize(count);
	return true;
}

bool setCurrentThreadCpuSets(const unsigned *selectedIds, unsigned count)
{
	typedef BOOL (WINAPI *SetThreadSelectedCpuSetsFunction)(
		HANDLE, const ULONG *, ULONG);
	HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
	SetThreadSelectedCpuSetsFunction setSelected = kernel != 0 ?
		reinterpret_cast<SetThreadSelectedCpuSetsFunction>(
			GetProcAddress(kernel, "SetThreadSelectedCpuSets")) : 0;
	return setSelected != 0 && setSelected(GetCurrentThread(),
		reinterpret_cast<const ULONG *>(selectedIds), count) != 0;
}

std::vector<JobCpuSetInfo> enumerateSystemCpuSets()
{
	typedef BOOL (WINAPI *GetSystemCpuSetInformationFunction)(
		PSYSTEM_CPU_SET_INFORMATION, ULONG, PULONG, HANDLE, ULONG);
	std::vector<JobCpuSetInfo> result;
	HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
	GetSystemCpuSetInformationFunction getCpuSets = kernel != 0 ?
		reinterpret_cast<GetSystemCpuSetInformationFunction>(
			GetProcAddress(kernel, "GetSystemCpuSetInformation")) : 0;
	if (getCpuSets == 0)
	{
		return result;
	}
	ULONG byteCount = 0;
	if (getCpuSets(0, 0, &byteCount, GetCurrentProcess(), 0) == 0 &&
		GetLastError() != ERROR_INSUFFICIENT_BUFFER)
	{
		return result;
	}
	if (byteCount == 0)
	{
		return result;
	}
	std::vector<unsigned char> storage(byteCount);
	if (getCpuSets(reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(&storage[0]),
		byteCount, &byteCount, GetCurrentProcess(), 0) == 0 ||
		byteCount > storage.size())
	{
		return result;
	}
	std::vector<unsigned> processCpuSets;
	// If process constraints cannot be queried, keep workers unpinned so the
	// OS-inherited restrictions remain authoritative instead of overriding them.
	if (!getSelectedCpuSets(GetCurrentProcess(), true, processCpuSets)) return result;
	std::vector<USHORT> processGroups;
	typedef BOOL (WINAPI *GetProcessGroupAffinityFunction)(HANDLE, PUSHORT, PUSHORT);
	GetProcessGroupAffinityFunction getGroups =
		reinterpret_cast<GetProcessGroupAffinityFunction>(
			GetProcAddress(kernel, "GetProcessGroupAffinity"));
	USHORT groupCount = 0;
	if (getGroups == 0)
	{
		return result;
	}
	if (getGroups != 0)
	{
		getGroups(GetCurrentProcess(), &groupCount, 0);
		if (groupCount != 0)
		{
			processGroups.resize(groupCount);
			if (!getGroups(GetCurrentProcess(), &groupCount, &processGroups[0]))
				processGroups.clear();
			else
				processGroups.resize(groupCount);
		}
	}
	if (processGroups.empty()) return result;
	const USHORT activeGroupCount = GetActiveProcessorGroupCount();
	/* On Windows 10 a process starts associated with one processor group even
	 * when it has not been restricted to that group.  Treating that initial
	 * association as an eligibility ceiling silently caps auto/all at 64 LPs.
	 * Explicit CPU-set selections remain authoritative.  A multi-group proper
	 * subset is also an explicit association and remains authoritative. */
	const bool hasExplicitCpuSetRestriction = !processCpuSets.empty();
	const bool hasExplicitGroupRestriction = processGroups.size() > 1 &&
		activeGroupCount != 0 && processGroups.size() < activeGroupCount;
	DWORD_PTR processMask = 0;
	DWORD_PTR systemMask = 0;
	const bool hasProcessMask = processGroups.size() == 1 &&
		GetProcessAffinityMask(GetCurrentProcess(), &processMask, &systemMask) &&
		processMask != 0;
	if (processGroups.size() == 1 && !hasProcessMask) return result;
	const bool hasExplicitProcessMask = hasProcessMask &&
		processMask != systemMask;
	ULONG offset = 0;
	while (offset + sizeof(SYSTEM_CPU_SET_INFORMATION) <= byteCount)
	{
		PSYSTEM_CPU_SET_INFORMATION information =
			reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(&storage[offset]);
		if (information->Size < sizeof(SYSTEM_CPU_SET_INFORMATION) ||
			information->Size > byteCount - offset)
		{
			break;
		}
		if (information->Type == CpuSetInformation)
		{
			JobCpuSetInfo item;
			item.id = information->CpuSet.Id;
			item.efficiencyClass = information->CpuSet.EfficiencyClass;
			item.group = information->CpuSet.Group;
			item.coreIndex = information->CpuSet.CoreIndex;
			item.logicalProcessorIndex = information->CpuSet.LogicalProcessorIndex;
			item.parked = information->CpuSet.Parked != 0;
			item.allocatedToOtherProcess = information->CpuSet.Allocated != 0 &&
				information->CpuSet.AllocatedToTargetProcess == 0;
			item.availableToProcess = (!hasExplicitCpuSetRestriction ||
				std::find(processCpuSets.begin(), processCpuSets.end(), item.id) !=
					processCpuSets.end()) && (!hasExplicitGroupRestriction ||
				std::find(processGroups.begin(), processGroups.end(),
					static_cast<USHORT>(item.group)) != processGroups.end());
			if (hasExplicitProcessMask &&
				(item.group != processGroups[0] ||
				 item.logicalProcessorIndex >= sizeof(DWORD_PTR) * CHAR_BIT ||
				 (processMask & (static_cast<DWORD_PTR>(1) <<
					item.logicalProcessorIndex)) == 0))
				item.availableToProcess = false;
			result.push_back(item);
		}
		offset += information->Size;
	}
	return result;
}
#endif

unsigned processAvailableCpuCount()
{
#if defined(_WIN32)
	DWORD_PTR processMask = 0;
	DWORD_PTR systemMask = 0;
	if (GetProcessAffinityMask(GetCurrentProcess(), &processMask, &systemMask) &&
		processMask != 0)
	{
		if (processMask == systemMask)
		{
			const DWORD allGroups = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
			if (allGroups != 0) return static_cast<unsigned>(allGroups);
		}
		unsigned count = 0;
		while (processMask != 0)
		{
			count += static_cast<unsigned>(processMask & 1);
			processMask >>= 1;
		}
		return count;
	}
#endif
	return std::thread::hardware_concurrency();
}
}

#if defined(RTS_BUILD_CORE_EXTRAS)
extern "C" void rts_job_system_set_test_fault(unsigned fault,
	unsigned occurrence)
{
	s_jobSystemTestFaultOccurrence.store(occurrence, std::memory_order_release);
	s_jobSystemTestFault.store(fault, std::memory_order_release);
}

extern "C" void rts_job_system_set_test_pause_mask(unsigned pauseMask)
{
	s_jobSystemTestPauseReleasedMask.store(0, std::memory_order_release);
	s_jobSystemTestPauseReachedMask.store(0, std::memory_order_release);
	s_jobSystemTestPauseMask.store(pauseMask, std::memory_order_release);
}

extern "C" bool rts_job_system_wait_for_test_pause(unsigned pausePoint,
	unsigned timeoutMilliseconds)
{
	const std::chrono::steady_clock::time_point deadline =
		std::chrono::steady_clock::now() +
		std::chrono::milliseconds(timeoutMilliseconds);
	while ((s_jobSystemTestPauseReachedMask.load(std::memory_order_acquire) &
		pausePoint) == 0)
	{
		if (std::chrono::steady_clock::now() >= deadline)
		{
			return false;
		}
		std::this_thread::yield();
	}
	return true;
}

extern "C" void rts_job_system_release_test_pause(unsigned pausePoint)
{
	s_jobSystemTestPauseReleasedMask.fetch_or(pausePoint, std::memory_order_acq_rel);
}
#endif

struct JobContext::State
{
	State(GroupRecord *groupValue, unsigned char *scratchValue,
		unsigned scratchCapacityValue, unsigned physicalWorkerIndexValue)
		: group(groupValue), scratch(scratchValue),
		  scratchCapacity(scratchCapacityValue), scratchUsed(0), failed(false),
		  physicalWorkerIndex(physicalWorkerIndexValue)
	{
	}

	GroupRecord *group;
	unsigned char *scratch;
	unsigned scratchCapacity;
	unsigned scratchUsed;
	bool failed;
	unsigned physicalWorkerIndex;
};

struct JobHandle::State
{
	explicit State(const std::shared_ptr<JobRecord> &recordValue)
		: record(recordValue)
	{
	}

	std::shared_ptr<JobRecord> record;
};

struct JobGroup::State
{
	explicit State(const std::shared_ptr<GroupRecord> &recordValue)
		: record(recordValue)
	{
	}

	std::shared_ptr<GroupRecord> record;
};

struct JobSystem::State
{
	enum ReadyEnqueueResult
	{
		READY_ENQUEUED,
		READY_ALREADY_OWNED,
		READY_FAILED
	};

	struct CompletionItem
	{
		CompletionItem() : completion(0), succeeded(false), cancelled(false) {}
		OwnerCompletion *completion;
		bool succeeded;
		bool cancelled;
	};

	struct PerformanceRecord
	{
		PerformanceRecord()
			: busyNanoseconds(0), maximumBusyNanoseconds(0), busySampleCount(0),
			  waitNanoseconds(0), maximumWaitNanoseconds(0), waitSampleCount(0),
			  waitStartNanoseconds(0), waitGeneration(0)
		{
		}

		mutable std::mutex mutex;
		unsigned long long busyNanoseconds;
		unsigned long long maximumBusyNanoseconds;
		unsigned long long busySampleCount;
		unsigned long long waitNanoseconds;
		unsigned long long maximumWaitNanoseconds;
		unsigned long long waitSampleCount;
		unsigned long long waitStartNanoseconds;
		unsigned long long waitGeneration;
	};

	struct Worker
	{
		Worker() : performance(0), index(0)
		{
			for (unsigned priority = 0; priority < JOB_PRIORITY_COUNT; ++priority)
				stealCursor[priority] = priority;
		}

		std::mutex mutex;
		std::deque<std::shared_ptr<JobRecord> > queues[JOB_PRIORITY_COUNT];
		std::thread thread;
		std::vector<unsigned char> scratch;
		PerformanceRecord *performance;
		unsigned index;
		unsigned stealCursor[JOB_PRIORITY_COUNT];
	};

	struct OwnerThread
	{
		OwnerThread() : affinityApplied(false) {}
		std::thread::id thread;
		bool affinityApplied;
		std::vector<unsigned> previousCpuSetIds;
	};

	State()
		: pinWorkers(false), queueCapacity(0), outstanding(0), configuredWorkerCount(0),
		  generation(0), running(false), stopping(false), lazyStartupDisabled(false),
		  availableLogicalCpuCount(0), reservedOwnerCpuCount(0),
		  selectedWorkerPhysicalCoreCount(0), selectedWorkerPhysicalCoreMask(0),
		  selectedWorkerPhysicalCoreMaskComplete(true),
		  readyCount(0),
		  activeWorkers(0), submittedJobCount(0), executedJobCount(0),
		  stealCount(0), ownerHelpCount(0), waitCount(0),
		  workerWaitRejectionCount(0), failedJobCount(0),
		  cancelledJobCount(0), serialFallbackCount(0),
		  totalQueueLatencyNanoseconds(0), maximumQueueLatencyNanoseconds(0),
		  workerSleepCount(0), workerWakeCount(0),
		  performanceMetricGeneration(2), affinityFailureCount(0),
		  injectionHighWater(0), maximumActiveWorkers(0),
		  completionCapacity(0)
	{
		for (unsigned priority = 0; priority < JOB_PRIORITY_COUNT; ++priority)
			readyCounts[priority].store(0, std::memory_order_relaxed);
	}

	bool hasInjectedWorkUnlocked() const
	{
		for (unsigned priority = 0; priority < JOB_PRIORITY_COUNT; ++priority)
		{
			if (!injectionQueues[priority].empty())
			{
				return true;
			}
		}
		return false;
	}

	std::shared_ptr<JobRecord> popInjectedWorkUnlocked(unsigned priority)
	{
		if (!injectionQueues[priority].empty())
		{
			std::shared_ptr<JobRecord> record = injectionQueues[priority].front();
			injectionQueues[priority].pop_front();
			return record;
		}
		return std::shared_ptr<JobRecord>();
	}

	std::shared_ptr<JobRecord> popLocalWork(Worker &worker, unsigned priority)
	{
		std::lock_guard<std::mutex> lock(worker.mutex);
		if (!worker.queues[priority].empty())
		{
			std::shared_ptr<JobRecord> record = worker.queues[priority].front();
			worker.queues[priority].pop_front();
			return record;
		}
		return std::shared_ptr<JobRecord>();
	}

	std::shared_ptr<JobRecord> stealWork(Worker &worker, unsigned priority)
	{
		const unsigned count = static_cast<unsigned>(workers.size());
		if (count == 0) return std::shared_ptr<JobRecord>();
		const bool helper = worker.index >= count;
		const unsigned candidateCount = helper ? count : count - 1;
		const unsigned maximumProbeCount = 4;
		const unsigned probeLimit = std::min(candidateCount, maximumProbeCount);
		unsigned victimIndex = worker.stealCursor[priority] % count;
		unsigned probes = 0;
		while (probes < probeLimit)
		{
			if (!helper && victimIndex == worker.index)
			{
				victimIndex = (victimIndex + 1) % count;
				continue;
			}
			Worker &victim = *workers[victimIndex];
			{
				std::lock_guard<std::mutex> lock(victim.mutex);
				if (!victim.queues[priority].empty())
				{
					std::shared_ptr<JobRecord> record = victim.queues[priority].back();
					victim.queues[priority].pop_back();
					worker.stealCursor[priority] = (victimIndex + 1) % count;
					stealCount.fetch_add(1, std::memory_order_relaxed);
					return record;
				}
			}
			++probes;
			victimIndex = (victimIndex + 1) % count;
		}
		worker.stealCursor[priority] = victimIndex;
		return std::shared_ptr<JobRecord>();
	}

	std::shared_ptr<JobRecord> takeWork(Worker &worker)
	{
		for (;;)
		{
			std::shared_ptr<JobRecord> record;
			/* Priority is the outer selection dimension.  A worker must not run
			 * local background work while frame-critical work is already visible
			 * in the shared injection queue or on a peer worker. */
			for (unsigned priority = 0;
				priority < JOB_PRIORITY_COUNT && !record; ++priority)
			{
				record = popLocalWork(worker, priority);
				if (!record)
				{
					std::lock_guard<std::mutex> lock(mutex);
					record = popInjectedWorkUnlocked(priority);
				}
				if (!record && readyCounts[priority].load(
					std::memory_order_acquire) != 0)
				{
					record = stealWork(worker, priority);
				}
				/* A published higher-priority lane remains authoritative even
				 * when this bounded steal pass did not find its current owner. */
				if (!record && readyCounts[priority].load(
					std::memory_order_acquire) != 0)
				{
					return std::shared_ptr<JobRecord>();
				}
			}
			if (!record)
			{
				return std::shared_ptr<JobRecord>();
			}

			bool claimed = false;
			{
				std::lock_guard<std::mutex> publicationLock(
					record->publicationMutex);
				/* A physical queue entry is not itself execution ownership.  A
				 * failed publication rollback or a stale duplicate may leave an
				 * entry whose record has already been claimed or finalized. */
				if (record->queued.load(std::memory_order_acquire) &&
					!record->executing.load(std::memory_order_acquire) &&
					!record->finalizing.load(std::memory_order_acquire) &&
					!record->complete.load(std::memory_order_acquire) &&
					record->job.load(std::memory_order_acquire) != 0)
				{
					readyCounts[record->priority].fetch_sub(1,
						std::memory_order_acq_rel);
					readyCount.fetch_sub(1, std::memory_order_acq_rel);
					record->executing.store(true, std::memory_order_release);
					record->queued.store(false, std::memory_order_release);
					claimed = true;
				}
			}
			if (!claimed)
			{
#if defined(RTS_BUILD_CORE_EXTRAS)
				pauseJobSystemTest(64);
#endif
				continue;
			}
#if defined(RTS_BUILD_CORE_EXTRAS)
			pauseJobSystemTest(32);
#endif
			return record;
		}
	}

	bool helpOwnerOnce()
	{
		if (!ownerHelper || std::this_thread::get_id() != ownerThread)
		{
			return false;
		}
		std::shared_ptr<JobRecord> record = takeWork(*ownerHelper);
		if (!record)
		{
			return false;
		}
		void *previousSystem = s_currentJobSystemWorker;
		void *previousQueue = s_currentJobSystemWorkerQueue;
		s_currentJobSystemWorker = this;
		s_currentJobSystemWorkerQueue = ownerHelper.get();
		execute(record, *ownerHelper);
		s_currentJobSystemWorkerQueue = previousQueue;
		s_currentJobSystemWorker = previousSystem;
		ownerHelpCount.fetch_add(1, std::memory_order_relaxed);
		return true;
	}

	ReadyEnqueueResult enqueueReady(const std::shared_ptr<JobRecord> &record)
	{
		std::unique_lock<std::mutex> publicationLock(record->publicationMutex);
		if (record->readyPublicationFailed.load(std::memory_order_acquire) ||
			record->finalizing.load(std::memory_order_acquire) ||
			record->executing.load(std::memory_order_acquire) ||
			record->complete.load(std::memory_order_acquire))
		{
			return READY_ALREADY_OWNED;
		}
		bool expected = false;
		if (!record->queued.compare_exchange_strong(expected, true,
			std::memory_order_acq_rel, std::memory_order_acquire))
		{
			return READY_ALREADY_OWNED;
		}
		try
		{
			bool failedAfterQueuePush = false;
#if defined(RTS_BUILD_CORE_EXTRAS)
			if (consumeJobSystemTestFault(6))
			{
				throw std::bad_alloc();
			}
#endif
			record->readyAt = std::chrono::steady_clock::now();
			if (s_currentJobSystemWorker == this &&
				s_currentJobSystemWorkerQueue != 0 &&
				s_currentJobSystemWorkerQueue != ownerHelper.get())
			{
				Worker *worker = static_cast<Worker *>(s_currentJobSystemWorkerQueue);
				std::lock_guard<std::mutex> lock(worker->mutex);
				worker->queues[record->priority].push_front(record);
#if defined(RTS_BUILD_CORE_EXTRAS)
				if (consumeJobSystemTestFault(10))
				{
					failedAfterQueuePush = true;
				}
#endif
				if (!failedAfterQueuePush)
				{
					readyCounts[record->priority].fetch_add(1,
						std::memory_order_release);
					readyCount.fetch_add(1, std::memory_order_release);
				}
			}
			else
			{
				std::lock_guard<std::mutex> lock(mutex);
				injectionQueues[record->priority].push_back(record);
#if defined(RTS_BUILD_CORE_EXTRAS)
				if (consumeJobSystemTestFault(10))
				{
					failedAfterQueuePush = true;
				}
#endif
				if (!failedAfterQueuePush)
				{
					unsigned injectedCount = 0;
					for (unsigned lane = 0; lane < JOB_PRIORITY_COUNT; ++lane)
					{
						injectedCount += static_cast<unsigned>(
							injectionQueues[lane].size());
					}
					unsigned observedHighWater = injectionHighWater.load(
						std::memory_order_relaxed);
					while (injectedCount > observedHighWater &&
						!injectionHighWater.compare_exchange_weak(observedHighWater,
							injectedCount, std::memory_order_relaxed,
							std::memory_order_relaxed))
					{
					}
					readyCounts[record->priority].fetch_add(1,
						std::memory_order_release);
					readyCount.fetch_add(1, std::memory_order_release);
				}
			}
			if (failedAfterQueuePush)
			{
				record->readyPublicationFailed.store(true,
					std::memory_order_release);
				record->queued.store(false, std::memory_order_release);
				publicationLock.unlock();
#if defined(RTS_BUILD_CORE_EXTRAS)
				pauseJobSystemTest(4);
#endif
				return READY_FAILED;
			}
		}
		catch (...)
		{
			record->readyPublicationFailed.store(true, std::memory_order_release);
			record->queued.store(false, std::memory_order_release);
			publicationLock.unlock();
#if defined(RTS_BUILD_CORE_EXTRAS)
			pauseJobSystemTest(4);
#endif
			return READY_FAILED;
		}

		workAvailable.notify_one();
		return READY_ENQUEUED;
	}

	void releaseDependents(const std::shared_ptr<JobRecord> &record,
		bool dependencyDidFail)
	{
		std::vector<std::shared_ptr<JobRecord> > dependents;
		{
			std::lock_guard<std::mutex> lock(record->dependentsMutex);
			dependents.swap(record->dependents);
		}
		for (const std::shared_ptr<JobRecord> &dependent : dependents)
		{
			if (!dependent)
			{
				continue;
			}
			if (dependencyDidFail)
			{
				dependent->dependencyFailed.store(true, std::memory_order_release);
			}
			if (dependent->unresolvedDependencies.fetch_sub(1,
				std::memory_order_acq_rel) == 1 &&
				dependent->accepted.load(std::memory_order_acquire))
			{
				#if defined(RTS_BUILD_CORE_EXTRAS)
				pauseJobSystemTest(2);
				#endif
				const ReadyEnqueueResult enqueueResult = enqueueReady(dependent);
				#if defined(RTS_BUILD_CORE_EXTRAS)
				pauseJobSystemTest(16);
				#endif
				if (enqueueResult == READY_FAILED)
				{
					delete dependent->job.exchange(0,
						std::memory_order_acq_rel);
					finish(dependent, true, false);
				}
			}
		}
	}

	void finish(const std::shared_ptr<JobRecord> &record, bool failed,
		bool cancelled, bool executionOwner = false)
	{
		bool expected = false;
		while (!record->finalizing.compare_exchange_strong(expected, true,
			std::memory_order_acq_rel, std::memory_order_acquire))
		{
			if (!executionOwner || record->complete.load(std::memory_order_acquire))
			{
				return;
			}
			expected = false;
			std::this_thread::yield();
		}
#if defined(RTS_BUILD_CORE_EXTRAS)
		if (!executionOwner)
		{
			pauseJobSystemTest(8);
		}
#endif
		/* A non-executing caller yields to a queued or active execution.  The
		 * execution owner must always finish: enqueueReady can transiently claim
		 * queued while it observes finalizing/executing and rejects a duplicate. */
		if (!executionOwner &&
			(record->queued.load(std::memory_order_acquire) ||
			 record->executing.load(std::memory_order_acquire)))
		{
			record->finalizing.store(false, std::memory_order_release);
			return;
		}
		if (executionOwner)
		{
			record->executing.store(false, std::memory_order_release);
		}
		if (record->completion != 0)
		{
			bool queuedCompletion = false;
			try
			{
#if defined(RTS_BUILD_CORE_EXTRAS)
				if (consumeJobSystemTestFault(7))
				{
					throw std::bad_alloc();
				}
#endif
				std::lock_guard<std::mutex> lock(completionMutex);
				if (ownerCompletions.size() < completionCapacity)
				{
					CompletionItem item;
					item.completion = record->completion;
					item.succeeded = !failed && !cancelled;
					item.cancelled = cancelled;
					ownerCompletions.push_back(item);
					queuedCompletion = true;
				}
			}
			catch (...)
			{
				queuedCompletion = false;
			}
			if (queuedCompletion)
			{
				record->completion = 0;
			}
			else
			{
				delete record->completion;
				record->completion = 0;
				failed = true;
			}
		}
		if (failed)
		{
			failedJobCount.fetch_add(1, std::memory_order_relaxed);
		}
		if (cancelled)
		{
			cancelledJobCount.fetch_add(1, std::memory_order_relaxed);
		}
		record->failed.store(failed, std::memory_order_release);
		record->cancelled.store(cancelled, std::memory_order_release);
		{
			std::unique_lock<std::mutex> lock = lockCompletionMutex(record->mutex, 4096);
			record->complete.store(true, std::memory_order_release);
		}
		record->completed.notify_all();
#if defined(RTS_BUILD_CORE_EXTRAS)
		pauseJobSystemTest(2048);
#endif
		releaseDependents(record, failed || cancelled);

		const std::shared_ptr<GroupRecord> group = record->group;
		if (failed)
		{
			group->failed.store(true, std::memory_order_release);
		}
		if (cancelled)
		{
			group->cancelled.store(true, std::memory_order_release);
		}
		{
			std::lock_guard<std::mutex> lock(mutex);
			if (outstanding != 0)
			{
				--outstanding;
			}
		}
		capacityAvailable.notify_all();
		group->removePending(1);
	}

	void execute(const std::shared_ptr<JobRecord> &record, Worker &worker)
	{
		const unsigned long long queueLatency = static_cast<unsigned long long>(
			std::chrono::duration_cast<std::chrono::nanoseconds>(
				std::chrono::steady_clock::now() - record->readyAt).count());
		totalQueueLatencyNanoseconds.fetch_add(queueLatency,
			std::memory_order_relaxed);
		unsigned long long observedLatency = maximumQueueLatencyNanoseconds.load(
			std::memory_order_relaxed);
		while (queueLatency > observedLatency &&
			!maximumQueueLatencyNanoseconds.compare_exchange_weak(observedLatency,
				queueLatency, std::memory_order_relaxed,
				std::memory_order_relaxed))
		{
		}
		const bool poolWorker = worker.index < configuredWorkerCount;
		bool callbackExecuted = false;
		unsigned long long busyStartNanoseconds = 0;
		unsigned long long busyGeneration = 0;
		bool cancelled = record->group->cancelled.load(
			std::memory_order_acquire);
		bool failed = record->dependencyFailed.load(std::memory_order_acquire);
		Job *executionJob = record->job.load(std::memory_order_acquire);
		if (executionJob == 0)
		{
			failed = true;
		}
		if (!cancelled && !failed)
		{
			callbackExecuted = true;
			if (poolWorker)
			{
				busyGeneration = performanceMetricGeneration.load(
					std::memory_order_acquire);
				busyStartNanoseconds = performanceClockNanoseconds();
				const unsigned active = activeWorkers.fetch_add(1,
					std::memory_order_acq_rel) + 1;
				unsigned observedMaximum = maximumActiveWorkers.load(
					std::memory_order_relaxed);
				while (active > observedMaximum &&
					!maximumActiveWorkers.compare_exchange_weak(observedMaximum,
						active, std::memory_order_relaxed,
						std::memory_order_relaxed))
				{
				}
			}
#if defined(_WIN32) && !defined(_WIN64)
			DeterministicFloatingPointScope floatingPointScope;
#endif
			JobContext::State contextState(record->group.get(),
				worker.scratch.empty() ? 0 : &worker.scratch[0],
				static_cast<unsigned>(worker.scratch.size()),
				poolWorker ? worker.index : JOB_INVALID_PHYSICAL_WORKER_INDEX);
			JobContext context(&contextState);
			try
			{
				executionJob->execute(context);
				failed = contextState.failed;
			}
			catch (...)
			{
				failed = true;
			}
		}
		cancelled = cancelled || record->group->cancelled.load(
			std::memory_order_acquire);

		delete record->job.exchange(0, std::memory_order_acq_rel);
		if (callbackExecuted)
		{
			executedJobCount.fetch_add(1, std::memory_order_relaxed);
		}
		if (poolWorker && callbackExecuted)
		{
			const unsigned long long busyEndNanoseconds =
				performanceClockNanoseconds();
			recordWorkerBusy(worker, busyGeneration, busyStartNanoseconds,
				busyEndNanoseconds);
			activeWorkers.fetch_sub(1, std::memory_order_acq_rel);
		}
#if defined(RTS_BUILD_CORE_EXTRAS)
		std::unique_ptr<std::thread> competingFinalizer;
		if (consumeJobSystemTestFault(9))
		{
			try
			{
				competingFinalizer.reset(new std::thread([this, record]() {
					finish(record, true, false);
				}));
			}
			catch (...)
			{
				competingFinalizer.reset();
			}
			if (competingFinalizer.get() != 0 &&
				(s_jobSystemTestPauseMask.load(std::memory_order_acquire) & 8) != 0)
			{
				const std::chrono::steady_clock::time_point pauseDeadline =
					std::chrono::steady_clock::now() + std::chrono::seconds(5);
				while ((s_jobSystemTestPauseReachedMask.load(
					std::memory_order_acquire) & 8) == 0 &&
					std::chrono::steady_clock::now() < pauseDeadline)
				{
					std::this_thread::yield();
				}
			}
		}
#endif
		finish(record, failed, cancelled, true);
#if defined(RTS_BUILD_CORE_EXTRAS)
		if (competingFinalizer.get() != 0 && competingFinalizer->joinable())
		{
			competingFinalizer->join();
		}
#endif
	}

	void recordWorkerBusy(Worker &worker, unsigned long long generation,
		unsigned long long startNanoseconds, unsigned long long endNanoseconds)
	{
		PerformanceRecord *performance = worker.performance;
		if (performance == 0 || (generation & 1) != 0) return;
		std::lock_guard<std::mutex> lock(performance->mutex);
		if (generation != performanceMetricGeneration.load(
			std::memory_order_acquire)) return;
		const unsigned long long elapsed = endNanoseconds >= startNanoseconds ?
			endNanoseconds - startNanoseconds : 0;
		saturatingCounterAdd(performance->busyNanoseconds, elapsed);
		updateCounterMaximum(performance->maximumBusyNanoseconds, elapsed);
		saturatingCounterIncrement(performance->busySampleCount);
	}

	void beginWorkerWait(Worker &worker)
	{
		PerformanceRecord *performance = worker.performance;
		if (performance == 0) return;
		for (;;)
		{
			const unsigned long long generation = performanceMetricGeneration.load(
				std::memory_order_acquire);
			if ((generation & 1) != 0)
			{
				std::this_thread::yield();
				continue;
			}
			const unsigned long long startNanoseconds =
				performanceClockNanoseconds();
			std::lock_guard<std::mutex> lock(performance->mutex);
			if (generation != performanceMetricGeneration.load(
				std::memory_order_acquire))
			{
				continue;
			}
			performance->waitGeneration = generation;
			performance->waitStartNanoseconds = startNanoseconds;
			return;
		}
	}

	void endWorkerWait(Worker &worker)
	{
		PerformanceRecord *performance = worker.performance;
		if (performance == 0) return;
		const unsigned long long endNanoseconds = performanceClockNanoseconds();
		std::lock_guard<std::mutex> lock(performance->mutex);
		const unsigned long long generation = performanceMetricGeneration.load(
			std::memory_order_acquire);
		if ((generation & 1) == 0 &&
			performance->waitStartNanoseconds != 0 &&
			performance->waitGeneration == generation)
		{
			const unsigned long long elapsed = endNanoseconds >=
				performance->waitStartNanoseconds ? endNanoseconds -
				performance->waitStartNanoseconds : 0;
			saturatingCounterAdd(performance->waitNanoseconds, elapsed);
			updateCounterMaximum(performance->maximumWaitNanoseconds, elapsed);
			saturatingCounterIncrement(performance->waitSampleCount);
		}
		performance->waitStartNanoseconds = 0;
		performance->waitGeneration = 0;
	}

	void resetPerformanceRecords()
	{
		unsigned long long stableGeneration = performanceMetricGeneration.load(
			std::memory_order_acquire);
		for (;;)
		{
			if ((stableGeneration & 1) != 0)
			{
				std::this_thread::yield();
				stableGeneration = performanceMetricGeneration.load(
					std::memory_order_acquire);
				continue;
			}
			const unsigned long long resettingGeneration = stableGeneration + 1;
			if (performanceMetricGeneration.compare_exchange_weak(stableGeneration,
				resettingGeneration, std::memory_order_acq_rel,
				std::memory_order_acquire)) break;
		}
		const unsigned long long nextGeneration = stableGeneration >= ~0ull - 1 ?
			2 : stableGeneration + 2;
		const unsigned long long resetNanoseconds = performanceClockNanoseconds();
		for (std::unique_ptr<PerformanceRecord> &record : performanceRecords)
		{
			std::lock_guard<std::mutex> lock(record->mutex);
			const bool remainsWaiting = record->waitStartNanoseconds != 0 &&
				record->waitGeneration == stableGeneration;
			record->busyNanoseconds = 0;
			record->maximumBusyNanoseconds = 0;
			record->busySampleCount = 0;
			record->waitNanoseconds = 0;
			record->maximumWaitNanoseconds = 0;
			record->waitSampleCount = 0;
			record->waitStartNanoseconds = remainsWaiting ? resetNanoseconds : 0;
			record->waitGeneration = remainsWaiting ? nextGeneration : 0;
		}
		performanceMetricGeneration.store(nextGeneration,
			std::memory_order_release);
	}

	void resetAllMetrics()
	{
		submittedJobCount.store(0, std::memory_order_relaxed);
		executedJobCount.store(0, std::memory_order_relaxed);
		stealCount.store(0, std::memory_order_relaxed);
		ownerHelpCount.store(0, std::memory_order_relaxed);
		waitCount.store(0, std::memory_order_relaxed);
		workerWaitRejectionCount.store(0, std::memory_order_relaxed);
		failedJobCount.store(0, std::memory_order_relaxed);
		cancelledJobCount.store(0, std::memory_order_relaxed);
		serialFallbackCount.store(0, std::memory_order_relaxed);
		totalQueueLatencyNanoseconds.store(0, std::memory_order_relaxed);
		maximumQueueLatencyNanoseconds.store(0, std::memory_order_relaxed);
		workerSleepCount.store(0, std::memory_order_relaxed);
		workerWakeCount.store(0, std::memory_order_relaxed);
		affinityFailureCount.store(0, std::memory_order_relaxed);
		injectionHighWater.store(0, std::memory_order_relaxed);
		maximumActiveWorkers.store(0, std::memory_order_relaxed);
		resetPerformanceRecords();
	}

	void collectPerformanceMetrics(JobSystemMetrics &result)
	{
		for (;;)
		{
			const unsigned long long generation = performanceMetricGeneration.load(
				std::memory_order_acquire);
			if ((generation & 1) != 0)
			{
				std::this_thread::yield();
				continue;
			}
			result.workerBusyNanoseconds = 0;
			result.maximumWorkerBusyNanoseconds = 0;
			result.workerBusySampleCount = 0;
			result.workerWaitNanoseconds = 0;
			result.maximumWorkerWaitNanoseconds = 0;
			result.workerWaitSampleCount = 0;
			const unsigned long long nowNanoseconds = performanceClockNanoseconds();
			for (std::unique_ptr<PerformanceRecord> &record : performanceRecords)
			{
				std::lock_guard<std::mutex> lock(record->mutex);
				saturatingCounterAdd(result.workerBusyNanoseconds,
					record->busyNanoseconds);
				updateCounterMaximum(result.maximumWorkerBusyNanoseconds,
					record->maximumBusyNanoseconds);
				saturatingCounterAdd(result.workerBusySampleCount,
					record->busySampleCount);
				saturatingCounterAdd(result.workerWaitNanoseconds,
					record->waitNanoseconds);
				updateCounterMaximum(result.maximumWorkerWaitNanoseconds,
					record->maximumWaitNanoseconds);
				saturatingCounterAdd(result.workerWaitSampleCount,
					record->waitSampleCount);
				if (record->waitStartNanoseconds != 0 &&
					record->waitGeneration == generation)
				{
					const unsigned long long elapsed = nowNanoseconds >=
						record->waitStartNanoseconds ? nowNanoseconds -
						record->waitStartNanoseconds : 0;
					saturatingCounterAdd(result.workerWaitNanoseconds, elapsed);
					updateCounterMaximum(result.maximumWorkerWaitNanoseconds,
						elapsed);
					saturatingCounterIncrement(result.workerWaitSampleCount);
				}
			}
			if (generation == performanceMetricGeneration.load(
				std::memory_order_acquire)) return;
		}
	}

	void workerLoop(Worker *worker)
	{
#if defined(_WIN32)
		if (pinWorkers && !selectedCpuSetIds.empty())
		{
			const unsigned cpuSetId = selectedCpuSetIds[
				worker->index % selectedCpuSetIds.size()];
			if (!setCurrentThreadCpuSets(&cpuSetId, 1))
				affinityFailureCount.fetch_add(1, std::memory_order_relaxed);
		}
#endif
#if defined(_WIN32) && !defined(_WIN64)
		_fpreset();
		_controlfp(_PC_24 | _RC_NEAR, _MCW_PC | _MCW_RC);
#endif
		s_currentJobSystemWorker = this;
		s_currentJobSystemWorkerQueue = worker;
		for (;;)
		{
			std::shared_ptr<JobRecord> record = takeWork(*worker);
			if (record)
			{
				execute(record, *worker);
				continue;
			}
			{
				std::unique_lock<std::mutex> lock(mutex);
				if (readyCount.load(std::memory_order_acquire) != 0)
				{
					lock.unlock();
					std::this_thread::yield();
					continue;
				}
				workerSleepCount.fetch_add(1, std::memory_order_relaxed);
				beginWorkerWait(*worker);
				workAvailable.wait(lock, [this]() {
					return stopping || readyCount.load(std::memory_order_acquire) != 0;
				});
				endWorkerWait(*worker);
				workerWakeCount.fetch_add(1, std::memory_order_relaxed);
				if (stopping && readyCount.load(std::memory_order_acquire) == 0)
				{
					break;
				}
			}
		}
		s_currentJobSystemWorkerQueue = 0;
		s_currentJobSystemWorker = 0;
	}

	mutable std::shared_mutex lifecycleMutex;
	mutable std::mutex mutex;
	std::condition_variable workAvailable;
	std::condition_variable capacityAvailable;
	std::deque<std::shared_ptr<JobRecord> > injectionQueues[JOB_PRIORITY_COUNT];
	std::vector<std::unique_ptr<Worker> > workers;
	std::vector<JobCpuSetInfo> cpuSets;
	std::vector<unsigned> selectedCpuSetIds;
	unsigned ownerCpuSetIds[2];
	OwnerThread ownerThreads[JOB_OWNER_COUNT];
	bool pinWorkers;
	std::unique_ptr<Worker> ownerHelper;
	std::thread::id ownerThread;
	mutable std::mutex completionMutex;
	std::deque<CompletionItem> ownerCompletions;
	unsigned queueCapacity;
	unsigned outstanding;
	unsigned configuredWorkerCount;
	unsigned generation;
	bool running;
	bool stopping;
	bool lazyStartupDisabled;
	unsigned availableLogicalCpuCount;
	unsigned reservedOwnerCpuCount;
	unsigned selectedWorkerPhysicalCoreCount;
	JobMetricCounter selectedWorkerPhysicalCoreMask;
	bool selectedWorkerPhysicalCoreMaskComplete;
	std::atomic<unsigned> readyCount;
	std::atomic<unsigned> readyCounts[JOB_PRIORITY_COUNT];
	std::atomic<unsigned> activeWorkers;
	std::atomic<unsigned long long> submittedJobCount;
	std::atomic<unsigned long long> executedJobCount;
	std::atomic<unsigned long long> stealCount;
	std::atomic<unsigned long long> ownerHelpCount;
	std::atomic<unsigned long long> waitCount;
	std::atomic<unsigned long long> workerWaitRejectionCount;
	std::atomic<unsigned long long> failedJobCount;
	std::atomic<unsigned long long> cancelledJobCount;
	std::atomic<unsigned long long> serialFallbackCount;
	std::atomic<unsigned long long> totalQueueLatencyNanoseconds;
	std::atomic<unsigned long long> maximumQueueLatencyNanoseconds;
	std::atomic<unsigned long long> workerSleepCount;
	std::atomic<unsigned long long> workerWakeCount;
	std::atomic<unsigned long long> performanceMetricGeneration;
	std::atomic<unsigned long long> affinityFailureCount;
	std::atomic<unsigned> injectionHighWater;
	std::atomic<unsigned> maximumActiveWorkers;
	unsigned completionCapacity;
	std::vector<std::unique_ptr<PerformanceRecord> > performanceRecords;
};

JobCpuSetInfo::JobCpuSetInfo()
	: id(0), efficiencyClass(0), group(0), coreIndex(UINT_MAX),
	  logicalProcessorIndex(0), parked(false),
	  allocatedToOtherProcess(false), availableToProcess(true)
{
}

JobSystemConfig::JobSystemConfig()
	: workerCount(0), queueCapacity(0), scratchBytesPerWorker(0),
	  pinWorkers(true), workerPolicy(JOB_WORKER_POLICY_AUTO)
{
}

JobSystemMetrics::JobSystemMetrics()
	: submittedJobCount(0), executedJobCount(0), stealCount(0),
	  ownerHelpCount(0), waitCount(0), workerWaitRejectionCount(0),
	  failedJobCount(0), cancelledJobCount(0), serialFallbackCount(0),
	  totalQueueLatencyNanoseconds(0), maximumQueueLatencyNanoseconds(0),
	  workerSleepCount(0), workerWakeCount(0),
	  workerBusyNanoseconds(0), maximumWorkerBusyNanoseconds(0),
	  workerBusySampleCount(0), workerWaitNanoseconds(0),
	  maximumWorkerWaitNanoseconds(0), workerWaitSampleCount(0),
	  affinityFailureCount(0),
	  injectionHighWater(0), maximumActiveWorkers(0), availableLogicalCpuCount(0),
	  reservedOwnerCpuCount(0), selectedWorkerCpuCount(0),
	  selectedWorkerPhysicalCoreCount(0), selectedWorkerPhysicalCoreMask(0),
	  selectedWorkerPhysicalCoreMaskComplete(true)
{
}

JobContext::JobContext(State *state) : m_state(state)
{
}

bool JobContext::isCancellationRequested() const
{
	return m_state != 0 && m_state->group != 0 &&
		m_state->group->cancelled.load(std::memory_order_acquire);
}

bool JobContext::isPhysicalWorkerExecution() const
{
	return physicalWorkerIndex() != JOB_INVALID_PHYSICAL_WORKER_INDEX;
}

unsigned JobContext::physicalWorkerIndex() const
{
	return m_state != 0 ? m_state->physicalWorkerIndex :
		JOB_INVALID_PHYSICAL_WORKER_INDEX;
}

void JobContext::fail()
{
	if (m_state != 0)
	{
		m_state->failed = true;
	}
}

void *JobContext::allocateScratch(unsigned byteCount, unsigned alignment)
{
	if (m_state == 0 || m_state->scratch == 0 || byteCount == 0 ||
		alignment == 0 || (alignment & (alignment - 1)) != 0)
	{
		return 0;
	}
	const unsigned aligned = (m_state->scratchUsed + alignment - 1) &
		~(alignment - 1);
	if (aligned > m_state->scratchCapacity ||
		byteCount > m_state->scratchCapacity - aligned)
	{
		return 0;
	}
	m_state->scratchUsed = aligned + byteCount;
	return m_state->scratch + aligned;
}

Job::Job()
{
}

Job::~Job()
{
}

OwnerCompletion::OwnerCompletion()
{
}

OwnerCompletion::~OwnerCompletion()
{
}

JobSubmission::JobSubmission()
	: job(0), priority(JOB_PRIORITY_NORMAL), completion(0),
	  dependencies(0), dependencyCount(0)
{
}

JobHandle::JobHandle() : m_state(0)
{
}

JobHandle::JobHandle(State *state) : m_state(state)
{
}

JobHandle::JobHandle(const JobHandle &other) : m_state(0)
{
	if (other.m_state != 0)
	{
		m_state = new State(other.m_state->record);
	}
}

JobHandle::~JobHandle()
{
	delete m_state;
}

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

bool JobHandle::isValid() const
{
	return m_state != 0 && m_state->record.get() != 0;
}

bool JobHandle::isComplete() const
{
	return isValid() && m_state->record->complete.load(std::memory_order_acquire);
}

bool JobHandle::succeeded() const
{
	return isComplete() && !failed() && !wasCancelled();
}

bool JobHandle::failed() const
{
	return isValid() && m_state->record->failed.load(std::memory_order_acquire);
}

bool JobHandle::wasCancelled() const
{
	return isValid() && m_state->record->cancelled.load(std::memory_order_acquire);
}

JobGroup::JobGroup() : m_state(0)
{
}

JobGroup::JobGroup(State *state) : m_state(state)
{
}

JobGroup::JobGroup(const JobGroup &other) : m_state(0)
{
	if (other.m_state != 0)
	{
		m_state = new State(other.m_state->record);
	}
}

JobGroup::~JobGroup()
{
	delete m_state;
}

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

bool JobGroup::isValid() const
{
	return m_state != 0 && m_state->record.get() != 0;
}

bool JobGroup::isComplete() const
{
	return isValid() && m_state->record->pending.load(std::memory_order_acquire) == 0;
}

bool JobGroup::failed() const
{
	return isValid() && m_state->record->failed.load(std::memory_order_acquire);
}

bool JobGroup::wasCancelled() const
{
	return isValid() && m_state->record->cancelled.load(std::memory_order_acquire);
}

JobSystem::JobSystem() : m_state(new (std::nothrow) State)
{
}

JobSystem::~JobSystem()
{
	shutdown();
	delete m_state;
}

JobSystem &JobSystem::instance()
{
	static JobSystem system;
	return system;
}

unsigned JobSystem::chooseRangeCount(unsigned itemCount,
	unsigned minimumItemsPerRange, unsigned workerCount)
{
	if (itemCount == 0) return 0;
	if (workerCount <= 1) return 1;
	if (minimumItemsPerRange == 0) minimumItemsPerRange = 1;
	const unsigned usefulRanges = itemCount / minimumItemsPerRange;
	if (usefulRanges <= 1) return 1;
	const unsigned parallelRanges = workerCount > UINT_MAX / 4 ?
		UINT_MAX : workerCount * 4;
	return usefulRanges < parallelRanges ? usefulRanges : parallelRanges;
}

bool JobSystem::rangeForIndex(unsigned itemCount, unsigned rangeCount,
	unsigned rangeIndex, JobRange &range)
{
	range.begin = 0;
	range.end = 0;
	if (rangeCount == 0 || rangeCount > itemCount || rangeIndex >= rangeCount)
		return false;
	const unsigned width = itemCount / rangeCount;
	const unsigned remainder = itemCount % rangeCount;
	range.begin = rangeIndex * width +
		(rangeIndex < remainder ? rangeIndex : remainder);
	range.end = range.begin + width + (rangeIndex < remainder ? 1 : 0);
	return true;
}

unsigned JobSystem::chooseWorkerCount(unsigned eligibleLogicalCpuCount,
	JobWorkerPolicy policy, unsigned explicitWorkerCount)
{
	if (explicitWorkerCount != 0)
	{
		return explicitWorkerCount;
	}
	if (eligibleLogicalCpuCount == 0)
	{
		return 1;
	}
	if (policy == JOB_WORKER_POLICY_ALL)
	{
		return eligibleLogicalCpuCount;
	}
	const unsigned reserved = eligibleLogicalCpuCount >= 8 ? 2 : 1;
	return eligibleLogicalCpuCount > reserved ?
		eligibleLogicalCpuCount - reserved : 1;
}

unsigned JobSystem::selectOwnerCpuSets(const JobCpuSetInfo *cpuSets,
	unsigned cpuSetCount, JobWorkerPolicy policy,
	unsigned explicitWorkerCount, unsigned *selectedIds,
	unsigned selectedIdCapacity)
{
	if (cpuSets == 0 || selectedIds == 0 || selectedIdCapacity == 0 ||
		policy != JOB_WORKER_POLICY_AUTO || explicitWorkerCount != 0) return 0;
	unsigned eligibleCount = 0;
	unsigned first = cpuSetCount;
	for (unsigned index = 0; index < cpuSetCount; ++index)
	{
		const JobCpuSetInfo &item = cpuSets[index];
		if (item.parked || item.allocatedToOtherProcess || !item.availableToProcess)
			continue;
		++eligibleCount;
		if (first == cpuSetCount || item.efficiencyClass > cpuSets[first].efficiencyClass ||
			(item.efficiencyClass == cpuSets[first].efficiencyClass && item.id < cpuSets[first].id))
			first = index;
	}
	if (eligibleCount <= 1) return 0;
	selectedIds[0] = cpuSets[first].id;
	if (eligibleCount < 8 || selectedIdCapacity < 2) return 1;
	unsigned second = cpuSetCount;
	bool secondOnDifferentCore = false;
	for (unsigned index = 0; index < cpuSetCount; ++index)
	{
		const JobCpuSetInfo &item = cpuSets[index];
		if (index == first || item.parked || item.allocatedToOtherProcess ||
			!item.availableToProcess) continue;
		const bool differentCore = item.coreIndex == UINT_MAX ||
			cpuSets[first].coreIndex == UINT_MAX || item.group != cpuSets[first].group ||
			item.coreIndex != cpuSets[first].coreIndex;
		if (second == cpuSetCount || (differentCore && !secondOnDifferentCore) ||
			(differentCore == secondOnDifferentCore &&
			 (item.efficiencyClass > cpuSets[second].efficiencyClass ||
			  (item.efficiencyClass == cpuSets[second].efficiencyClass && item.id < cpuSets[second].id))))
		{
			second = index;
			secondOnDifferentCore = differentCore;
		}
	}
	selectedIds[1] = cpuSets[second].id;
	return 2;
}

unsigned JobSystem::selectWorkerCpuSets(const JobCpuSetInfo *cpuSets,
	unsigned cpuSetCount, JobWorkerPolicy policy,
	unsigned explicitWorkerCount, unsigned *selectedIds,
	unsigned selectedIdCapacity)
{
	if (cpuSets == 0 || selectedIds == 0 || selectedIdCapacity == 0)
	{
		return 0;
	}
	std::vector<JobCpuSetInfo> eligible;
	try
	{
		eligible.reserve(cpuSetCount);
		for (unsigned index = 0; index < cpuSetCount; ++index)
		{
			if (cpuSets[index].availableToProcess && !cpuSets[index].parked &&
				!cpuSets[index].allocatedToOtherProcess)
			{
				eligible.push_back(cpuSets[index]);
			}
		}
		std::stable_sort(eligible.begin(), eligible.end(),
			[](const JobCpuSetInfo &left, const JobCpuSetInfo &right) {
				if (left.efficiencyClass != right.efficiencyClass)
				{
					return left.efficiencyClass > right.efficiencyClass;
				}
				return left.id < right.id;
			});
	}
	catch (...)
	{
		return 0;
	}

	const unsigned eligibleCount = static_cast<unsigned>(eligible.size());
	const unsigned requested = chooseWorkerCount(eligibleCount, policy,
		explicitWorkerCount);
	const unsigned selectedCount = requested < eligibleCount ? requested :
		eligibleCount;
	unsigned ownerIds[2];
	const unsigned ownerCount = selectOwnerCpuSets(cpuSets, cpuSetCount,
		policy, explicitWorkerCount, ownerIds, 2);
	const unsigned writable = selectedCount < selectedIdCapacity ?
		selectedCount : selectedIdCapacity;
	unsigned written = 0;
	// Prefer one logical processor from each physical core before consuming SMT
	// siblings.  Efficiency-class ordering remains authoritative within each
	// pass, so heterogeneous systems still prefer their faster cores.
	for (unsigned index = 0; index < eligibleCount && written < writable; ++index)
	{
		if ((ownerCount > 0 && eligible[index].id == ownerIds[0]) ||
			(ownerCount > 1 && eligible[index].id == ownerIds[1])) continue;
		bool sharesSelectedCore = false;
		for (unsigned selectedIndex = 0; selectedIndex < written; ++selectedIndex)
		{
			for (unsigned eligibleIndex = 0; eligibleIndex < eligibleCount;
				++eligibleIndex)
			{
				if (eligible[eligibleIndex].id != selectedIds[selectedIndex]) continue;
				sharesSelectedCore = eligible[index].coreIndex != UINT_MAX &&
					eligible[eligibleIndex].coreIndex != UINT_MAX &&
					eligible[index].group == eligible[eligibleIndex].group &&
					eligible[index].coreIndex == eligible[eligibleIndex].coreIndex;
				break;
			}
			if (sharesSelectedCore) break;
		}
		if (sharesSelectedCore) continue;
		selectedIds[written++] = eligible[index].id;
	}
	for (unsigned index = 0; index < eligibleCount && written < writable; ++index)
	{
		if ((ownerCount > 0 && eligible[index].id == ownerIds[0]) ||
			(ownerCount > 1 && eligible[index].id == ownerIds[1])) continue;
		bool alreadySelected = false;
		for (unsigned selectedIndex = 0; selectedIndex < written; ++selectedIndex)
		{
			if (selectedIds[selectedIndex] == eligible[index].id)
			{
				alreadySelected = true;
				break;
			}
		}
		if (!alreadySelected) selectedIds[written++] = eligible[index].id;
	}
	return written;
}

unsigned JobSystem::summarizeSelectedPhysicalCores(
	const JobCpuSetInfo *cpuSets, unsigned cpuSetCount,
	const unsigned *selectedIds, unsigned selectedIdCount,
	JobMetricCounter *selectedCoreMask, bool *selectedCoreMaskComplete)
{
	if (selectedCoreMask != 0) *selectedCoreMask = 0;
	if (selectedCoreMaskComplete != 0) *selectedCoreMaskComplete = true;
	if (cpuSets == 0 || selectedIds == 0)
	{
		if (selectedCoreMaskComplete != 0 && selectedIdCount != 0)
			*selectedCoreMaskComplete = false;
		return 0;
	}
	unsigned distinctCount = 0;
	const unsigned maskBits = static_cast<unsigned>(sizeof(JobMetricCounter) * 8);
	for (unsigned selectedIndex = 0; selectedIndex < selectedIdCount; ++selectedIndex)
	{
		unsigned cpuIndex = cpuSetCount;
		for (unsigned index = 0; index < cpuSetCount; ++index)
		{
			if (cpuSets[index].id == selectedIds[selectedIndex])
			{
				cpuIndex = index;
				break;
			}
		}
		if (cpuIndex == cpuSetCount)
		{
			if (selectedCoreMaskComplete != 0)
				*selectedCoreMaskComplete = false;
			continue;
		}
		bool alreadyCounted = false;
		for (unsigned previous = 0; previous < selectedIndex; ++previous)
		{
			unsigned previousCpuIndex = cpuSetCount;
			for (unsigned index = 0; index < cpuSetCount; ++index)
			{
				if (cpuSets[index].id == selectedIds[previous])
				{
					previousCpuIndex = index;
					break;
				}
			}
			if (previousCpuIndex == cpuSetCount) continue;
			const bool sameKnownCore = cpuSets[cpuIndex].coreIndex != UINT_MAX &&
				cpuSets[previousCpuIndex].coreIndex != UINT_MAX &&
				cpuSets[cpuIndex].group == cpuSets[previousCpuIndex].group &&
				cpuSets[cpuIndex].coreIndex == cpuSets[previousCpuIndex].coreIndex;
			if (sameKnownCore || cpuSets[cpuIndex].id == cpuSets[previousCpuIndex].id)
			{
				alreadyCounted = true;
				break;
			}
		}
		if (alreadyCounted) continue;
		unsigned representativeIndex = cpuIndex;
		for (unsigned index = 0; index < cpuIndex; ++index)
		{
			const bool sameKnownCore = cpuSets[cpuIndex].coreIndex != UINT_MAX &&
				cpuSets[index].coreIndex != UINT_MAX &&
				cpuSets[cpuIndex].group == cpuSets[index].group &&
				cpuSets[cpuIndex].coreIndex == cpuSets[index].coreIndex;
			if (sameKnownCore || cpuSets[cpuIndex].id == cpuSets[index].id)
			{
				representativeIndex = index;
				break;
			}
		}
		unsigned topologyOrdinal = 0;
		for (unsigned index = 0; index < representativeIndex; ++index)
		{
			bool earlierSameCore = false;
			for (unsigned earlier = 0; earlier < index; ++earlier)
			{
				const bool sameKnownCore = cpuSets[index].coreIndex != UINT_MAX &&
					cpuSets[earlier].coreIndex != UINT_MAX &&
					cpuSets[index].group == cpuSets[earlier].group &&
					cpuSets[index].coreIndex == cpuSets[earlier].coreIndex;
				if (sameKnownCore || cpuSets[index].id == cpuSets[earlier].id)
				{
					earlierSameCore = true;
					break;
				}
			}
			if (!earlierSameCore) ++topologyOrdinal;
		}
		if (topologyOrdinal < maskBits)
		{
			if (selectedCoreMask != 0)
				*selectedCoreMask |= static_cast<JobMetricCounter>(1) << topologyOrdinal;
		}
		else if (selectedCoreMaskComplete != 0)
		{
			*selectedCoreMaskComplete = false;
		}
		++distinctCount;
	}
	return distinctCount;
}

bool JobSystem::setStartupWorkerCount(unsigned workerCount)
{
	s_startupWorkerCount.store(workerCount, std::memory_order_release);
	return true;
}

bool JobSystem::setStartupWorkerPolicy(const char *policy)
{
	if (equalsAsciiNoCase(policy, "auto"))
	{
		s_startupWorkerPolicy.store(JOB_WORKER_POLICY_AUTO,
			std::memory_order_release);
		return true;
	}
	if (equalsAsciiNoCase(policy, "all"))
	{
		s_startupWorkerPolicy.store(JOB_WORKER_POLICY_ALL,
			std::memory_order_release);
		return true;
	}
	return false;
}

JobSystemConfig JobSystem::startupConfig()
{
	JobSystemConfig config;
	config.workerCount = s_startupWorkerCount.load(std::memory_order_acquire);
	config.workerPolicy = static_cast<JobWorkerPolicy>(
		s_startupWorkerPolicy.load(std::memory_order_acquire));
	config.queueCapacity = 4096;
	config.scratchBytesPerWorker = 256 * 1024;
	config.pinWorkers = true;
	return config;
}

bool JobSystem::ensureStarted()
{
	if (isRunning())
	{
		return true;
	}
	return startInternal(startupConfig(), false);
}

bool JobSystem::start(const JobSystemConfig &config)
{
	return startInternal(config, true);
}

bool JobSystem::startInternal(const JobSystemConfig &config, bool allowRestart)
{
#if defined(RTS_BUILD_CORE_EXTRAS)
	if (consumeJobSystemTestFault(1))
	{
		return false;
	}
#endif
	if (config.queueCapacity == 0 || config.scratchBytesPerWorker == 0 ||
		(config.workerPolicy != JOB_WORKER_POLICY_AUTO &&
		 config.workerPolicy != JOB_WORKER_POLICY_ALL))
	{
		return false;
	}
	if (m_state == 0)
	{
		return false;
	}
	std::unique_lock<std::shared_mutex> lifecycleLock(m_state->lifecycleMutex);
	{
		std::lock_guard<std::mutex> lock(m_state->mutex);
		if (m_state->running) return !allowRestart;
		if (m_state->stopping || (!allowRestart && m_state->lazyStartupDisabled))
			return false;
		const std::thread::id gameOwner = m_state->ownerThreads[JOB_OWNER_GAME].thread;
		if (gameOwner != std::thread::id() && gameOwner != std::this_thread::get_id())
			return false;
	}
	std::vector<JobCpuSetInfo> cpuSets;
#if defined(_WIN32)
	try
	{
		cpuSets = enumerateSystemCpuSets();
	}
	catch (...)
	{
		cpuSets.clear();
	}
#endif
	unsigned eligibleCpuCount = 0;
	for (const JobCpuSetInfo &cpuSet : cpuSets)
	{
		if (cpuSet.availableToProcess && !cpuSet.parked && !cpuSet.allocatedToOtherProcess)
		{
			++eligibleCpuCount;
		}
	}
	if (eligibleCpuCount == 0)
	{
		if (!cpuSets.empty()) return false;
		eligibleCpuCount = processAvailableCpuCount();
	}
	if (eligibleCpuCount == 0)
	{
		eligibleCpuCount = 1;
	}
	const unsigned effectiveWorkerCount = chooseWorkerCount(eligibleCpuCount,
		config.workerPolicy, config.workerCount);
	if (effectiveWorkerCount == 0)
	{
		return false;
	}
	const unsigned minimumExplicitLimit = 32;
	const unsigned topologyMultiplier = 4;
	const unsigned topologyLimit = eligibleCpuCount > UINT_MAX / topologyMultiplier ?
		UINT_MAX : eligibleCpuCount * topologyMultiplier;
	const unsigned explicitLimit = std::max(minimumExplicitLimit, topologyLimit);
	if (config.workerCount != 0 && effectiveWorkerCount > explicitLimit)
	{
		return false;
	}
	std::vector<unsigned> selectedCpuSetIds;
	unsigned ownerCpuSetIds[2] = { 0, 0 };
	unsigned ownerCpuSetCount = 0;
	if (config.pinWorkers && !cpuSets.empty())
	{
		try
		{
			selectedCpuSetIds.resize(cpuSets.size());
			const unsigned selectedCount = selectWorkerCpuSets(&cpuSets[0],
				static_cast<unsigned>(cpuSets.size()), config.workerPolicy,
				config.workerCount, &selectedCpuSetIds[0],
				static_cast<unsigned>(selectedCpuSetIds.size()));
			selectedCpuSetIds.resize(selectedCount);
			ownerCpuSetCount = selectOwnerCpuSets(&cpuSets[0],
				static_cast<unsigned>(cpuSets.size()), config.workerPolicy,
				config.workerCount, ownerCpuSetIds, 2);
		}
		catch (...)
		{
			selectedCpuSetIds.clear();
			ownerCpuSetCount = 0;
		}
	}
	JobMetricCounter selectedPhysicalCoreMask = 0;
	bool selectedPhysicalCoreMaskComplete = true;
	const unsigned selectedPhysicalCoreCount = selectedCpuSetIds.empty() ? 0 :
		summarizeSelectedPhysicalCores(&cpuSets[0],
			static_cast<unsigned>(cpuSets.size()), &selectedCpuSetIds[0],
			static_cast<unsigned>(selectedCpuSetIds.size()),
			&selectedPhysicalCoreMask, &selectedPhysicalCoreMaskComplete);
	{
		std::lock_guard<std::mutex> lock(m_state->mutex);
		if (m_state->running || m_state->stopping || !m_state->workers.empty())
		{
			return false;
		}
		m_state->queueCapacity = config.queueCapacity;
		m_state->outstanding = 0;
		m_state->configuredWorkerCount = effectiveWorkerCount;
		m_state->completionCapacity = config.queueCapacity;
		m_state->ownerThread = std::this_thread::get_id();
		m_state->cpuSets.swap(cpuSets);
		m_state->selectedCpuSetIds.swap(selectedCpuSetIds);
		m_state->pinWorkers = config.pinWorkers &&
			!m_state->selectedCpuSetIds.empty();
		m_state->availableLogicalCpuCount = eligibleCpuCount;
		m_state->reservedOwnerCpuCount = ownerCpuSetCount;
		m_state->selectedWorkerPhysicalCoreCount = selectedPhysicalCoreCount;
		m_state->selectedWorkerPhysicalCoreMask = selectedPhysicalCoreMask;
		m_state->selectedWorkerPhysicalCoreMaskComplete =
			selectedPhysicalCoreMaskComplete;
		m_state->ownerCpuSetIds[0] = ownerCpuSetIds[0];
		m_state->ownerCpuSetIds[1] = ownerCpuSetIds[1];
		++m_state->generation;
		m_state->stopping = false;
		m_state->readyCount.store(0, std::memory_order_release);
		for (unsigned priority = 0; priority < JOB_PRIORITY_COUNT; ++priority)
			m_state->readyCounts[priority].store(0, std::memory_order_release);
		m_state->activeWorkers.store(0, std::memory_order_release);
	}
	m_state->resetAllMetrics();

	try
	{
		m_state->performanceRecords.clear();
		m_state->performanceRecords.reserve(effectiveWorkerCount);
		m_state->ownerHelper.reset(new State::Worker);
		m_state->ownerHelper->index = effectiveWorkerCount;
		m_state->ownerHelper->scratch.resize(config.scratchBytesPerWorker);
		m_state->workers.reserve(effectiveWorkerCount);
		for (unsigned index = 0; index < effectiveWorkerCount; ++index)
		{
			std::unique_ptr<State::Worker> worker(new State::Worker);
			std::unique_ptr<State::PerformanceRecord> performance(
				new State::PerformanceRecord);
			worker->index = index;
			worker->performance = performance.get();
#if defined(RTS_BUILD_CORE_EXTRAS)
			if (consumeJobSystemTestFault(2))
			{
				throw std::bad_alloc();
			}
#endif
			worker->scratch.resize(config.scratchBytesPerWorker);
			m_state->performanceRecords.push_back(std::move(performance));
			m_state->workers.push_back(std::move(worker));
		}
		LockPipelineExecutionMode();
		for (unsigned index = 0; index < effectiveWorkerCount; ++index)
		{
			State::Worker *workerPointer = m_state->workers[index].get();
#if defined(RTS_BUILD_CORE_EXTRAS)
			if (consumeJobSystemTestFault(3))
			{
				throw std::bad_alloc();
			}
#endif
			workerPointer->thread = std::thread([this, workerPointer]() {
				m_state->workerLoop(workerPointer);
			});
		}
	}
	catch (...)
	{
		{
			std::lock_guard<std::mutex> lock(m_state->mutex);
			m_state->stopping = true;
		}
		m_state->workAvailable.notify_all();
		for (std::unique_ptr<State::Worker> &worker : m_state->workers)
		{
			if (worker->thread.joinable())
			{
				worker->thread.join();
			}
		}
		m_state->workers.clear();
		m_state->performanceRecords.clear();
		m_state->ownerHelper.reset();
		{
			std::lock_guard<std::mutex> lock(m_state->mutex);
			m_state->stopping = false;
			m_state->configuredWorkerCount = 0;
			m_state->completionCapacity = 0;
			m_state->cpuSets.clear();
			m_state->selectedCpuSetIds.clear();
			m_state->pinWorkers = false;
			m_state->reservedOwnerCpuCount = 0;
			m_state->selectedWorkerPhysicalCoreCount = 0;
			m_state->selectedWorkerPhysicalCoreMask = 0;
			m_state->selectedWorkerPhysicalCoreMaskComplete = true;
		}
		return false;
	}

	{
		std::lock_guard<std::mutex> lock(m_state->mutex);
		m_state->running = true;
		m_state->lazyStartupDisabled = false;
	}
	return true;
}

void JobSystem::shutdown()
{
	if (m_state == 0)
	{
		return;
	}
	if (s_currentJobSystemWorker == m_state)
	{
		return;
	}
	std::unique_lock<std::shared_mutex> lifecycleLock(m_state->lifecycleMutex);
	{
		std::lock_guard<std::mutex> lock(m_state->mutex);
		if ((m_state->running || m_state->stopping) &&
			std::this_thread::get_id() != m_state->ownerThread)
		{
			return;
		}
		m_state->running = false;
		m_state->stopping = true;
		m_state->lazyStartupDisabled = true;
	}
	lifecycleLock.unlock();
	m_state->workAvailable.notify_all();
	for (std::unique_ptr<State::Worker> &worker : m_state->workers)
	{
		if (worker->thread.joinable())
		{
			worker->thread.join();
		}
	}
	m_state->workers.clear();
	if (std::this_thread::get_id() == m_state->ownerThread)
	{
		for (;;)
		{
			State::CompletionItem item;
			{
				std::lock_guard<std::mutex> lock(m_state->completionMutex);
				if (m_state->ownerCompletions.empty())
				{
					break;
				}
				item = m_state->ownerCompletions.front();
				m_state->ownerCompletions.pop_front();
			}
			try
			{
				item.completion->complete(item.succeeded, item.cancelled);
			}
			catch (...)
			{
			}
			delete item.completion;
		}
	}
	{
		std::lock_guard<std::mutex> completionLock(m_state->completionMutex);
		while (!m_state->ownerCompletions.empty())
		{
			delete m_state->ownerCompletions.front().completion;
			m_state->ownerCompletions.pop_front();
		}
	}
	m_state->ownerHelper.reset();
	// Keep immutable affinity metadata until the next explicit start: service
	// owners outlive compute draining and must still unregister on their thread.
	lifecycleLock.lock();
	{
		std::lock_guard<std::mutex> lock(m_state->mutex);
		for (unsigned priority = 0; priority < JOB_PRIORITY_COUNT; ++priority)
		{
			m_state->injectionQueues[priority].clear();
		}
		m_state->outstanding = 0;
		m_state->readyCount.store(0, std::memory_order_release);
		for (unsigned priority = 0; priority < JOB_PRIORITY_COUNT; ++priority)
			m_state->readyCounts[priority].store(0, std::memory_order_release);
		m_state->configuredWorkerCount = 0;
		m_state->queueCapacity = 0;
		m_state->completionCapacity = 0;
		m_state->stopping = false;
	}
}

bool JobSystem::registerCurrentThread(JobOwnerRole role)
{
	if (m_state == 0 || role < JOB_OWNER_GAME || role >= JOB_OWNER_COUNT ||
		isWorkerThread()) return false;
	std::lock_guard<std::mutex> lock(m_state->mutex);
	State::OwnerThread &owner = m_state->ownerThreads[role];
	const std::thread::id current = std::this_thread::get_id();
	if (owner.thread != std::thread::id()) return owner.thread == current;
	// A serial backend remains owned by GAME; it must not repin that thread
	// as another service, or restoring one role would corrupt the other's mask.
	for (unsigned index = 0; index < JOB_OWNER_COUNT; ++index)
		if (m_state->ownerThreads[index].thread == current) return false;
	if (role == JOB_OWNER_GAME && m_state->running && current != m_state->ownerThread)
		return false;
	LockPipelineExecutionMode();
	owner.thread = current;
#if defined(_WIN32)
	if (m_state->pinWorkers && !m_state->selectedCpuSetIds.empty())
	{
		try
		{
			const bool saved = getSelectedCpuSets(GetCurrentThread(), false,
				owner.previousCpuSetIds);
			const unsigned roleIndex = static_cast<unsigned>(role);
			const bool dedicated = roleIndex < m_state->reservedOwnerCpuCount;
			const unsigned *ids = dedicated ? &m_state->ownerCpuSetIds[roleIndex] :
				&m_state->selectedCpuSetIds[0];
			const unsigned count = dedicated ? 1 :
				static_cast<unsigned>(m_state->selectedCpuSetIds.size());
			owner.affinityApplied = saved && setCurrentThreadCpuSets(ids, count);
		}
		catch (...) { owner.affinityApplied = false; }
		if (!owner.affinityApplied)
			m_state->affinityFailureCount.fetch_add(1, std::memory_order_relaxed);
	}
#endif
	return true;
}

bool JobSystem::unregisterCurrentThread(JobOwnerRole role)
{
	if (m_state == 0 || role < JOB_OWNER_GAME || role >= JOB_OWNER_COUNT) return false;
	std::lock_guard<std::mutex> lock(m_state->mutex);
	State::OwnerThread &owner = m_state->ownerThreads[role];
	if (owner.thread != std::this_thread::get_id()) return false;
#if defined(_WIN32)
	if (owner.affinityApplied && !setCurrentThreadCpuSets(
		owner.previousCpuSetIds.empty() ? 0 : &owner.previousCpuSetIds[0],
		static_cast<unsigned>(owner.previousCpuSetIds.size())))
		m_state->affinityFailureCount.fetch_add(1, std::memory_order_relaxed);
#endif
	owner.thread = std::thread::id();
	owner.affinityApplied = false;
	owner.previousCpuSetIds.clear();
	return true;
}

bool JobSystem::isCurrentThread(JobOwnerRole role) const
{
	if (m_state == 0 || role < JOB_OWNER_GAME || role >= JOB_OWNER_COUNT) return false;
	std::lock_guard<std::mutex> lock(m_state->mutex);
	return m_state->ownerThreads[role].thread == std::this_thread::get_id();
}

bool JobSystem::isRunning() const
{
	if (m_state == 0)
	{
		return false;
	}
	std::lock_guard<std::mutex> lock(m_state->mutex);
	return m_state->running;
}

bool JobSystem::isWorkerThread() const
{
	return m_state != 0 && s_currentJobSystemWorker == m_state;
}

unsigned JobSystem::workerCount() const
{
	if (m_state == 0)
	{
		return 0;
	}
	std::lock_guard<std::mutex> lock(m_state->mutex);
	return m_state->configuredWorkerCount;
}

unsigned JobSystem::outstandingJobCount() const
{
	if (m_state == 0)
	{
		return 0;
	}
	std::lock_guard<std::mutex> lock(m_state->mutex);
	return m_state->outstanding;
}

unsigned JobSystem::cpuSetCount() const
{
	if (m_state == 0)
		return 0;
	std::lock_guard<std::mutex> lock(m_state->mutex);
	return static_cast<unsigned>(m_state->cpuSets.size());
}

bool JobSystem::cpuSetAt(unsigned index, JobCpuSetInfo &result) const
{
	if (m_state == 0)
		return false;
	std::lock_guard<std::mutex> lock(m_state->mutex);
	if (index >= m_state->cpuSets.size())
		return false;
	result = m_state->cpuSets[index];
	return true;
}

unsigned JobSystem::selectedWorkerCpuSetCount() const
{
	if (m_state == 0)
		return 0;
	std::lock_guard<std::mutex> lock(m_state->mutex);
	return static_cast<unsigned>(m_state->selectedCpuSetIds.size());
}

bool JobSystem::selectedWorkerCpuSetIdAt(unsigned index, unsigned &result) const
{
	if (m_state == 0)
		return false;
	std::lock_guard<std::mutex> lock(m_state->mutex);
	if (index >= m_state->selectedCpuSetIds.size())
		return false;
	result = m_state->selectedCpuSetIds[index];
	return true;
}

unsigned JobSystem::ownerCpuSetCount() const
{
	if (m_state == 0)
		return 0;
	std::lock_guard<std::mutex> lock(m_state->mutex);
	return m_state->reservedOwnerCpuCount;
}

bool JobSystem::ownerCpuSetIdAt(unsigned index, unsigned &result) const
{
	if (m_state == 0)
		return false;
	std::lock_guard<std::mutex> lock(m_state->mutex);
	if (index >= m_state->reservedOwnerCpuCount || index >= 2)
		return false;
	result = m_state->ownerCpuSetIds[index];
	return true;
}

JobSystemMetrics JobSystem::metrics() const
{
	JobSystemMetrics result;
	if (m_state == 0)
	{
		return result;
	}
	std::shared_lock<std::shared_mutex> lifecycleLock(m_state->lifecycleMutex);
	result.submittedJobCount = m_state->submittedJobCount.load(std::memory_order_relaxed);
	result.executedJobCount = m_state->executedJobCount.load(std::memory_order_relaxed);
	result.stealCount = m_state->stealCount.load(std::memory_order_relaxed);
	result.ownerHelpCount = m_state->ownerHelpCount.load(std::memory_order_relaxed);
	result.waitCount = m_state->waitCount.load(std::memory_order_relaxed);
	result.workerWaitRejectionCount = m_state->workerWaitRejectionCount.load(std::memory_order_relaxed);
	result.failedJobCount = m_state->failedJobCount.load(std::memory_order_relaxed);
	result.cancelledJobCount = m_state->cancelledJobCount.load(std::memory_order_relaxed);
	result.serialFallbackCount = m_state->serialFallbackCount.load(std::memory_order_relaxed);
	result.totalQueueLatencyNanoseconds = m_state->totalQueueLatencyNanoseconds.load(std::memory_order_relaxed);
	result.maximumQueueLatencyNanoseconds = m_state->maximumQueueLatencyNanoseconds.load(std::memory_order_relaxed);
	result.workerSleepCount = m_state->workerSleepCount.load(std::memory_order_relaxed);
	result.workerWakeCount = m_state->workerWakeCount.load(std::memory_order_relaxed);
	m_state->collectPerformanceMetrics(result);
	result.affinityFailureCount = m_state->affinityFailureCount.load(std::memory_order_relaxed);
	result.injectionHighWater = m_state->injectionHighWater.load(std::memory_order_relaxed);
	result.maximumActiveWorkers = m_state->maximumActiveWorkers.load(std::memory_order_relaxed);
	{
		std::lock_guard<std::mutex> lock(m_state->mutex);
		result.availableLogicalCpuCount = m_state->availableLogicalCpuCount;
		result.reservedOwnerCpuCount = m_state->reservedOwnerCpuCount;
		result.selectedWorkerCpuCount = static_cast<unsigned>(m_state->selectedCpuSetIds.size());
		result.selectedWorkerPhysicalCoreCount =
			m_state->selectedWorkerPhysicalCoreCount;
		result.selectedWorkerPhysicalCoreMask =
			m_state->selectedWorkerPhysicalCoreMask;
		result.selectedWorkerPhysicalCoreMaskComplete =
			m_state->selectedWorkerPhysicalCoreMaskComplete;
	}
	return result;
}

void JobSystem::resetMetrics()
{
	if (m_state == 0)
	{
		return;
	}
	std::shared_lock<std::shared_mutex> lifecycleLock(m_state->lifecycleMutex);
	m_state->resetAllMetrics();
}

void JobSystem::resetPerformanceMetrics()
{
	if (m_state == 0)
		return;
	std::shared_lock<std::shared_mutex> lifecycleLock(m_state->lifecycleMutex);
	m_state->resetPerformanceRecords();
}

void JobSystem::recordSerialFallback()
{
	if (m_state != 0)
	{
		m_state->serialFallbackCount.fetch_add(1, std::memory_order_relaxed);
	}
}

JobGroup JobSystem::createGroup()
{
	if (m_state == 0)
	{
		return JobGroup();
	}
	std::shared_lock<std::shared_mutex> lifecycleLock(m_state->lifecycleMutex);
	std::lock_guard<std::mutex> lock(m_state->mutex);
	if (!m_state->running || m_state->stopping)
	{
		return JobGroup();
	}
	try
	{
#if defined(RTS_BUILD_CORE_EXTRAS)
		if (consumeJobSystemTestFault(4))
		{
			throw std::bad_alloc();
		}
#endif
		std::shared_ptr<GroupRecord> record = std::make_shared<GroupRecord>(
			m_state, m_state->generation);
		return JobGroup(new JobGroup::State(record));
	}
	catch (...)
	{
		return JobGroup();
	}
}

JobHandle JobSystem::trySubmit(Job *job, JobPriority priority,
	const JobGroup &group)
{
	return trySubmit(job, priority, group, 0);
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
		priority >= JOB_PRIORITY_COUNT || m_state == 0 || !group.isValid() ||
		(dependencyCount != 0 && dependencies == 0))
	{
		return JobHandle();
	}
	std::shared_lock<std::shared_mutex> lifecycleLock(m_state->lifecycleMutex);
	const std::shared_ptr<GroupRecord> groupRecord = group.m_state->record;
	if (groupRecord->owner != m_state ||
		groupRecord->generation != m_state->generation ||
		groupRecord->cancelled.load(std::memory_order_acquire))
	{
		return JobHandle();
	}
	for (unsigned dependencyIndex = 0; dependencyIndex < dependencyCount;
		++dependencyIndex)
	{
		if (!dependencies[dependencyIndex].isValid())
		{
			return JobHandle();
		}
		const std::shared_ptr<JobRecord> dependency =
			dependencies[dependencyIndex].m_state->record;
		if (dependency->owner != m_state ||
			dependency->generation != m_state->generation)
		{
			return JobHandle();
		}
		for (unsigned previousIndex = 0; previousIndex < dependencyIndex;
			++previousIndex)
		{
			if (dependencies[previousIndex].m_state->record == dependency)
			{
				return JobHandle();
			}
		}
	}

	std::shared_ptr<JobRecord> record;
	JobHandle::State *handleState = 0;
	try
	{
#if defined(RTS_BUILD_CORE_EXTRAS)
		if (consumeJobSystemTestFault(5))
		{
			throw std::bad_alloc();
		}
#endif
		record = std::make_shared<JobRecord>();
		handleState = new JobHandle::State(record);
	}
	catch (...)
	{
		delete handleState;
		return JobHandle();
	}

	record->priority = priority;
	record->group = groupRecord;
	record->owner = m_state;
	record->generation = m_state->generation;
	{
		std::lock_guard<std::mutex> lock(m_state->mutex);
		if (!m_state->running || m_state->stopping ||
			m_state->outstanding >= m_state->queueCapacity)
		{
			delete handleState;
			return JobHandle();
		}
		record->job.store(job, std::memory_order_release);
		record->completion = completion;
		++m_state->outstanding;
		groupRecord->pending.fetch_add(1, std::memory_order_acq_rel);
	}

	bool wired = true;
	try
	{
		for (unsigned dependencyIndex = 0; dependencyIndex < dependencyCount;
			++dependencyIndex)
		{
			const std::shared_ptr<JobRecord> dependency =
				dependencies[dependencyIndex].m_state->record;
			std::lock_guard<std::mutex> lock(dependency->dependentsMutex);
			if (dependency->complete.load(std::memory_order_acquire))
			{
				if (dependency->failed.load(std::memory_order_acquire) ||
					dependency->cancelled.load(std::memory_order_acquire))
				{
					record->dependencyFailed.store(true, std::memory_order_release);
				}
			}
			else
			{
				record->unresolvedDependencies.fetch_add(1,
					std::memory_order_acq_rel);
				dependency->dependents.push_back(record);
			}
		}
	}
	catch (...)
	{
		wired = false;
	}

	if (!wired)
	{
		record->job.store(0, std::memory_order_release);
		record->completion = 0;
		groupRecord->removePending(1);
		try
		{
			std::lock_guard<std::mutex> lock(m_state->mutex);
			--m_state->outstanding;
		}
		catch (...)
		{
		}
		delete handleState;
		return JobHandle();
	}

	record->accepted.store(true, std::memory_order_release);
#if defined(RTS_BUILD_CORE_EXTRAS)
	pauseJobSystemTest(1);
#endif
	if (record->unresolvedDependencies.load(std::memory_order_acquire) == 0 &&
		m_state->enqueueReady(record) == State::READY_FAILED)
	{
		record->accepted.store(false, std::memory_order_release);
		record->job.store(0, std::memory_order_release);
		record->completion = 0;
		groupRecord->removePending(1);
		{
			std::lock_guard<std::mutex> lock(m_state->mutex);
			--m_state->outstanding;
		}
		delete handleState;
		return JobHandle();
	}
	m_state->submittedJobCount.fetch_add(1, std::memory_order_relaxed);
	return JobHandle(handleState);
}

JobHandle JobSystem::then(const JobHandle &prerequisite, Job *job,
	JobPriority priority, const JobGroup &group)
{
	return trySubmitAfter(job, priority, group, &prerequisite, 1);
}

bool JobSystem::trySubmitBatch(const JobSubmission *submissions,
	unsigned submissionCount, const JobGroup &group, JobHandle *handles)
{
	if (submissions == 0 || submissionCount == 0 || handles == 0 ||
		m_state == 0 || !group.isValid())
	{
		return false;
	}
	std::shared_lock<std::shared_mutex> lifecycleLock(m_state->lifecycleMutex);
	const std::shared_ptr<GroupRecord> groupRecord = group.m_state->record;
	if (groupRecord->owner != m_state ||
		groupRecord->generation != m_state->generation ||
		groupRecord->cancelled.load(std::memory_order_acquire))
	{
		return false;
	}
	for (unsigned index = 0; index < submissionCount; ++index)
	{
		if (submissions[index].job == 0 ||
			submissions[index].priority < JOB_PRIORITY_FRAME_CRITICAL ||
			submissions[index].priority >= JOB_PRIORITY_COUNT ||
			(submissions[index].dependencyCount != 0 &&
			 submissions[index].dependencies == 0))
		{
			return false;
		}
		for (unsigned previous = 0; previous < index; ++previous)
		{
			if (submissions[previous].job == submissions[index].job)
			{
				return false;
			}
		}
		for (unsigned dependencyIndex = 0;
			dependencyIndex < submissions[index].dependencyCount;
			++dependencyIndex)
		{
			const JobHandle &dependencyHandle =
				submissions[index].dependencies[dependencyIndex];
			if (!dependencyHandle.isValid() ||
				dependencyHandle.m_state->record->owner != m_state ||
				dependencyHandle.m_state->record->generation != m_state->generation)
			{
				return false;
			}
			for (unsigned previousDependency = 0;
				previousDependency < dependencyIndex; ++previousDependency)
			{
				if (submissions[index].dependencies[previousDependency].m_state->record ==
					dependencyHandle.m_state->record)
				{
					return false;
				}
			}
		}
	}

	std::vector<std::shared_ptr<JobRecord> > records;
	std::vector<JobHandle::State *> handleStates;
	try
	{
		records.reserve(submissionCount);
		handleStates.reserve(submissionCount);
		for (unsigned index = 0; index < submissionCount; ++index)
		{
#if defined(RTS_BUILD_CORE_EXTRAS)
			if (consumeJobSystemTestFault(5))
			{
				throw std::bad_alloc();
			}
#endif
			std::shared_ptr<JobRecord> record = std::make_shared<JobRecord>();
			record->priority = submissions[index].priority;
			record->group = groupRecord;
			record->owner = m_state;
			record->generation = m_state->generation;
			records.push_back(record);
			handleStates.push_back(new JobHandle::State(record));
		}
	}
	catch (...)
	{
		for (JobHandle::State *state : handleStates)
		{
			delete state;
		}
		return false;
	}

	{
		std::lock_guard<std::mutex> lock(m_state->mutex);
		if (!m_state->running || m_state->stopping ||
			submissionCount > m_state->queueCapacity - m_state->outstanding)
		{
			for (JobHandle::State *state : handleStates)
			{
				delete state;
			}
			return false;
		}
		m_state->outstanding += submissionCount;
		groupRecord->pending.fetch_add(submissionCount, std::memory_order_acq_rel);
	}
	for (unsigned index = 0; index < submissionCount; ++index)
	{
		records[index]->job.store(submissions[index].job,
			std::memory_order_release);
		records[index]->completion = submissions[index].completion;
	}

	bool wired = true;
	try
	{
		for (unsigned index = 0; index < submissionCount && wired; ++index)
		{
			for (unsigned dependencyIndex = 0;
				dependencyIndex < submissions[index].dependencyCount;
				++dependencyIndex)
			{
				const std::shared_ptr<JobRecord> dependency =
					submissions[index].dependencies[dependencyIndex].m_state->record;
				std::lock_guard<std::mutex> lock(dependency->dependentsMutex);
				if (dependency->complete.load(std::memory_order_acquire))
				{
					if (dependency->failed.load(std::memory_order_acquire) ||
						dependency->cancelled.load(std::memory_order_acquire))
					{
						records[index]->dependencyFailed.store(true,
							std::memory_order_release);
					}
				}
				else
				{
					records[index]->unresolvedDependencies.fetch_add(1,
						std::memory_order_acq_rel);
					dependency->dependents.push_back(records[index]);
				}
			}
		}
	}
	catch (...)
	{
		wired = false;
	}

	auto rollbackAdmission = [&]() {
		for (unsigned index = 0; index < submissionCount; ++index)
		{
			records[index]->job.store(0, std::memory_order_release);
			records[index]->completion = 0;
			records[index]->accepted.store(false, std::memory_order_release);
			records[index]->queued.store(false, std::memory_order_release);
		}
		groupRecord->removePending(submissionCount);
		{
			std::lock_guard<std::mutex> lock(m_state->mutex);
			m_state->outstanding -= submissionCount;
		}
		for (JobHandle::State *state : handleStates)
		{
			delete state;
		}
	};

	if (!wired)
	{
		rollbackAdmission();
		return false;
	}

	std::vector<std::shared_ptr<JobRecord> > readyRecords;
	try
	{
		readyRecords.reserve(submissionCount);
		for (const std::shared_ptr<JobRecord> &record : records)
		{
			if (record->unresolvedDependencies.load(std::memory_order_acquire) == 0)
			{
				readyRecords.push_back(record);
			}
		}
	}
	catch (...)
	{
		rollbackAdmission();
		return false;
	}

	bool queued = true;
	State::Worker *localWorker = s_currentJobSystemWorker == m_state ?
		static_cast<State::Worker *>(s_currentJobSystemWorkerQueue) : 0;
	if (localWorker == m_state->ownerHelper.get())
	{
		localWorker = 0;
	}
	auto eraseQueuedRecords = [&]() {
		if (localWorker != 0)
		{
			for (unsigned lane = 0; lane < JOB_PRIORITY_COUNT; ++lane)
			{
				std::deque<std::shared_ptr<JobRecord> > &queue =
					localWorker->queues[lane];
				for (std::deque<std::shared_ptr<JobRecord> >::iterator it = queue.begin();
					it != queue.end();)
				{
					if (std::find(readyRecords.begin(), readyRecords.end(), *it) !=
						readyRecords.end())
					{
						it = queue.erase(it);
					}
					else
					{
						++it;
					}
				}
			}
		}
		else
		{
			for (unsigned lane = 0; lane < JOB_PRIORITY_COUNT; ++lane)
			{
				std::deque<std::shared_ptr<JobRecord> > &queue =
					m_state->injectionQueues[lane];
				for (std::deque<std::shared_ptr<JobRecord> >::iterator it = queue.begin();
					it != queue.end();)
				{
					if (std::find(readyRecords.begin(), readyRecords.end(), *it) !=
						readyRecords.end())
					{
						it = queue.erase(it);
					}
					else
					{
						++it;
					}
				}
			}
		}
	};

	if (localWorker != 0)
	{
		std::lock_guard<std::mutex> lock(localWorker->mutex);
		try
		{
			for (const std::shared_ptr<JobRecord> &record : readyRecords)
			{
#if defined(RTS_BUILD_CORE_EXTRAS)
				if (consumeJobSystemTestFault(6))
				{
					throw std::bad_alloc();
				}
#endif
				record->readyAt = std::chrono::steady_clock::now();
				record->queued.store(true, std::memory_order_release);
				localWorker->queues[record->priority].push_front(record);
			}
			for (const std::shared_ptr<JobRecord> &record : records)
			{
				record->accepted.store(true, std::memory_order_release);
			}
			for (const std::shared_ptr<JobRecord> &record : readyRecords)
			{
				m_state->readyCounts[record->priority].fetch_add(1,
					std::memory_order_release);
			}
			m_state->readyCount.fetch_add(
				static_cast<unsigned>(readyRecords.size()),
				std::memory_order_release);
		}
		catch (...)
		{
			queued = false;
			eraseQueuedRecords();
		}
	}
	else
	{
		std::lock_guard<std::mutex> lock(m_state->mutex);
		try
		{
			for (const std::shared_ptr<JobRecord> &record : readyRecords)
			{
#if defined(RTS_BUILD_CORE_EXTRAS)
				if (consumeJobSystemTestFault(6))
				{
					throw std::bad_alloc();
				}
#endif
				record->readyAt = std::chrono::steady_clock::now();
				record->queued.store(true, std::memory_order_release);
				m_state->injectionQueues[record->priority].push_back(record);
			}
			for (const std::shared_ptr<JobRecord> &record : records)
			{
				record->accepted.store(true, std::memory_order_release);
			}
			unsigned injectedCount = 0;
			for (unsigned lane = 0; lane < JOB_PRIORITY_COUNT; ++lane)
			{
				injectedCount += static_cast<unsigned>(
					m_state->injectionQueues[lane].size());
			}
			unsigned observedHighWater = m_state->injectionHighWater.load(
				std::memory_order_relaxed);
			while (injectedCount > observedHighWater &&
				!m_state->injectionHighWater.compare_exchange_weak(
					observedHighWater, injectedCount, std::memory_order_relaxed,
					std::memory_order_relaxed))
			{
			}
			for (const std::shared_ptr<JobRecord> &record : readyRecords)
			{
				m_state->readyCounts[record->priority].fetch_add(1,
					std::memory_order_release);
			}
			m_state->readyCount.fetch_add(
				static_cast<unsigned>(readyRecords.size()),
				std::memory_order_release);
		}
		catch (...)
		{
			queued = false;
			eraseQueuedRecords();
		}
	}

	if (!queued)
	{
		rollbackAdmission();
		return false;
	}
	m_state->submittedJobCount.fetch_add(submissionCount,
		std::memory_order_relaxed);
	for (unsigned index = 0; index < submissionCount; ++index)
	{
		delete handles[index].m_state;
		handles[index].m_state = handleStates[index];
		handleStates[index] = 0;
	}
	const unsigned wakeCount = std::min(static_cast<unsigned>(readyRecords.size()),
		m_state->configuredWorkerCount);
	for (unsigned wakeIndex = 0; wakeIndex < wakeCount; ++wakeIndex)
	{
		m_state->workAvailable.notify_one();
	}

	for (const std::shared_ptr<JobRecord> &record : records)
	{
		if (!record->complete.load(std::memory_order_acquire) &&
			!record->queued.load(std::memory_order_acquire) &&
			record->unresolvedDependencies.load(std::memory_order_acquire) == 0)
		{
			if (m_state->enqueueReady(record) == State::READY_FAILED)
			{
				delete record->job.exchange(0, std::memory_order_acq_rel);
				m_state->finish(record, true, false);
			}
		}
	}
	return true;
}

bool JobSystem::tryPromote(Job *job, JobPriority priority)
{
	if (job == 0 || priority < JOB_PRIORITY_FRAME_CRITICAL ||
		priority >= JOB_PRIORITY_COUNT || m_state == 0)
	{
		return false;
	}
	std::shared_lock<std::shared_mutex> lifecycleLock(m_state->lifecycleMutex);
	{
		std::lock_guard<std::mutex> lock(m_state->mutex);
		if (!m_state->running || m_state->stopping)
		{
			return false;
		}
	}
	auto promoteInQueues = [this, job, priority](
		std::deque<std::shared_ptr<JobRecord> > *queues) {
		for (unsigned lane = 0; lane < JOB_PRIORITY_COUNT; ++lane)
		{
			for (auto it = queues[lane].begin(); it != queues[lane].end(); ++it)
			{
				if ((*it)->job.load(std::memory_order_acquire) == job)
				{
					std::shared_ptr<JobRecord> record = *it;
					if (lane == static_cast<unsigned>(priority))
					{
						return true;
					}
					try
					{
#if defined(RTS_BUILD_CORE_EXTRAS)
						if (consumeJobSystemTestFault(8))
						{
							throw std::bad_alloc();
						}
#endif
						queues[priority].push_front(record);
					}
					catch (...)
					{
						return false;
					}
					record->priority = priority;
					queues[lane].erase(it);
					m_state->readyCounts[priority].fetch_add(1,
						std::memory_order_release);
					m_state->readyCounts[lane].fetch_sub(1,
						std::memory_order_acq_rel);
					return true;
				}
			}
		}
		return false;
	};
	{
		std::lock_guard<std::mutex> lock(m_state->mutex);
		if (promoteInQueues(m_state->injectionQueues))
		{
			m_state->workAvailable.notify_one();
			return true;
		}
	}
	for (const std::unique_ptr<State::Worker> &worker : m_state->workers)
	{
		std::lock_guard<std::mutex> lock(worker->mutex);
		if (promoteInQueues(worker->queues))
		{
			m_state->workAvailable.notify_one();
			return true;
		}
	}
	return false;
}

bool JobSystem::wait(const JobHandle &handle)
{
	if (!handle.isValid() || m_state == 0)
	{
		return false;
	}
	if (s_currentJobSystemWorker == m_state)
	{
		m_state->workerWaitRejectionCount.fetch_add(1, std::memory_order_relaxed);
		return false;
	}
	m_state->waitCount.fetch_add(1, std::memory_order_relaxed);
	const std::shared_ptr<JobRecord> record = handle.m_state->record;
	if (std::this_thread::get_id() == m_state->ownerThread)
	{
		while (!record->complete.load(std::memory_order_acquire))
		{
			if (m_state->helpOwnerOnce())
			{
				continue;
			}
			std::unique_lock<std::mutex> lock(record->mutex);
#if defined(RTS_BUILD_CORE_EXTRAS)
			pauseJobSystemTest(1024);
#endif
			record->completed.wait_for(lock, std::chrono::milliseconds(1));
		}
		return true;
	}
	std::unique_lock<std::mutex> lock(record->mutex);
	record->completed.wait(lock, [record]() {
		return record->complete.load(std::memory_order_acquire);
	});
	return record->complete.load(std::memory_order_acquire);
}

bool JobSystem::wait(const JobGroup &group)
{
	if (!group.isValid() || m_state == 0 ||
		group.m_state->record->owner != m_state)
	{
		return false;
	}
	if (s_currentJobSystemWorker == m_state)
	{
		m_state->workerWaitRejectionCount.fetch_add(1, std::memory_order_relaxed);
		return false;
	}
	m_state->waitCount.fetch_add(1, std::memory_order_relaxed);
	const std::shared_ptr<GroupRecord> record = group.m_state->record;
	if (std::this_thread::get_id() == m_state->ownerThread)
	{
		while (record->pending.load(std::memory_order_acquire) != 0)
		{
			if (m_state->helpOwnerOnce())
			{
				continue;
			}
			std::unique_lock<std::mutex> lock(record->mutex);
			record->completed.wait_for(lock, std::chrono::milliseconds(1));
		}
		return true;
	}
	std::unique_lock<std::mutex> lock(record->mutex);
	record->completed.wait(lock, [record]() {
		return record->pending.load(std::memory_order_acquire) == 0;
	});
	return true;
}

bool JobSystem::waitWithoutOwnerHelp(const JobGroup &group,
	unsigned timeoutMilliseconds)
{
	if (!group.isValid() || m_state == 0 ||
		group.m_state->record->owner != m_state)
	{
		return false;
	}
	if (s_currentJobSystemWorker == m_state)
	{
		m_state->workerWaitRejectionCount.fetch_add(1,
			std::memory_order_relaxed);
		return false;
	}
	m_state->waitCount.fetch_add(1, std::memory_order_relaxed);
	const std::shared_ptr<GroupRecord> record = group.m_state->record;
	std::unique_lock<std::mutex> lock(record->mutex);
	return record->completed.wait_for(lock,
		std::chrono::milliseconds(timeoutMilliseconds), [record]() {
			const bool complete = record->pending.load(std::memory_order_acquire) == 0;
#if defined(RTS_BUILD_CORE_EXTRAS)
			if (!complete) pauseJobSystemTest(128);
#endif
			return complete;
		});
}

bool JobSystem::cancel(const JobGroup &group)
{
	if (!group.isValid() || m_state == 0 ||
		group.m_state->record->owner != m_state)
	{
		return false;
	}
	std::shared_lock<std::shared_mutex> lifecycleLock(m_state->lifecycleMutex);
	group.m_state->record->cancelled.store(true, std::memory_order_release);
	m_state->workAvailable.notify_all();
	return true;
}

unsigned JobSystem::pumpOwnerCompletions(unsigned maximumCount)
{
	if (m_state == 0 || maximumCount == 0 ||
		std::this_thread::get_id() != m_state->ownerThread)
	{
		return 0;
	}
	unsigned completedCount = 0;
	while (completedCount < maximumCount)
	{
		State::CompletionItem item;
		{
			std::lock_guard<std::mutex> lock(m_state->completionMutex);
			if (m_state->ownerCompletions.empty())
			{
				break;
			}
			item = m_state->ownerCompletions.front();
			m_state->ownerCompletions.pop_front();
		}
		try
		{
			item.completion->complete(item.succeeded, item.cancelled);
		}
		catch (...)
		{
		}
		delete item.completion;
		++completedCount;
	}
	return completedCount;
}

unsigned JobSystem::pendingOwnerCompletionCount() const
{
	if (m_state == 0)
	{
		return 0;
	}
	std::lock_guard<std::mutex> lock(m_state->completionMutex);
	return static_cast<unsigned>(m_state->ownerCompletions.size());
}
}
