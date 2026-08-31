#include "Lib/JobSystem.h"

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
		byteCount, &byteCount, GetCurrentProcess(), 0) == 0)
	{
		return result;
	}
	ULONG offset = 0;
	while (offset + sizeof(SYSTEM_CPU_SET_INFORMATION) <= byteCount)
	{
		PSYSTEM_CPU_SET_INFORMATION information =
			reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(&storage[offset]);
		if (information->Size == 0 || offset + information->Size > byteCount)
		{
			break;
		}
		if (information->Type == CpuSetInformation)
		{
			JobCpuSetInfo item;
			item.id = information->CpuSet.Id;
			item.efficiencyClass = information->CpuSet.EfficiencyClass;
			item.parked = information->CpuSet.Parked != 0;
			item.allocatedToOtherProcess = information->CpuSet.Allocated != 0 &&
				information->CpuSet.AllocatedToTargetProcess == 0;
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
		unsigned scratchCapacityValue)
		: group(groupValue), scratch(scratchValue),
		  scratchCapacity(scratchCapacityValue), scratchUsed(0), failed(false)
	{
	}

	GroupRecord *group;
	unsigned char *scratch;
	unsigned scratchCapacity;
	unsigned scratchUsed;
	bool failed;
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

	struct Worker
	{
		Worker() : index(0) {}

		std::mutex mutex;
		std::deque<std::shared_ptr<JobRecord> > queues[JOB_PRIORITY_COUNT];
		std::thread thread;
		std::vector<unsigned char> scratch;
		unsigned index;
	};

	State()
		: pinWorkers(false), queueCapacity(0), outstanding(0), configuredWorkerCount(0),
		  generation(0), running(false), stopping(false), readyCount(0),
		  activeWorkers(0), submittedJobCount(0), executedJobCount(0),
		  stealCount(0), ownerHelpCount(0), waitCount(0),
		  workerWaitRejectionCount(0), failedJobCount(0),
		  cancelledJobCount(0), serialFallbackCount(0),
		  totalQueueLatencyNanoseconds(0), maximumQueueLatencyNanoseconds(0),
		  workerSleepCount(0), workerWakeCount(0),
		  injectionHighWater(0), maximumActiveWorkers(0),
		  completionCapacity(0)
	{
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

	std::shared_ptr<JobRecord> popInjectedWorkUnlocked()
	{
		for (unsigned priority = 0; priority < JOB_PRIORITY_COUNT; ++priority)
		{
			if (!injectionQueues[priority].empty())
			{
				std::shared_ptr<JobRecord> record = injectionQueues[priority].front();
				injectionQueues[priority].pop_front();
				return record;
			}
		}
		return std::shared_ptr<JobRecord>();
	}

	std::shared_ptr<JobRecord> popLocalWork(Worker &worker)
	{
		std::lock_guard<std::mutex> lock(worker.mutex);
		for (unsigned priority = 0; priority < JOB_PRIORITY_COUNT; ++priority)
		{
			if (!worker.queues[priority].empty())
			{
				std::shared_ptr<JobRecord> record = worker.queues[priority].front();
				worker.queues[priority].pop_front();
				return record;
			}
		}
		return std::shared_ptr<JobRecord>();
	}

	std::shared_ptr<JobRecord> stealWork(Worker &worker)
	{
		const unsigned count = static_cast<unsigned>(workers.size());
		const bool helper = worker.index >= count;
		const unsigned firstOffset = helper ? 0 : 1;
		const unsigned endOffset = helper ? count : count;
		for (unsigned offset = firstOffset; offset < endOffset; ++offset)
		{
			const unsigned victimIndex = helper ? offset :
				(worker.index + offset) % count;
			Worker &victim = *workers[victimIndex];
			std::lock_guard<std::mutex> lock(victim.mutex);
			for (unsigned priority = 0; priority < JOB_PRIORITY_COUNT; ++priority)
			{
				if (!victim.queues[priority].empty())
				{
					std::shared_ptr<JobRecord> record = victim.queues[priority].back();
					victim.queues[priority].pop_back();
					stealCount.fetch_add(1, std::memory_order_relaxed);
					return record;
				}
			}
		}
		return std::shared_ptr<JobRecord>();
	}

	std::shared_ptr<JobRecord> takeWork(Worker &worker)
	{
		for (;;)
		{
			std::shared_ptr<JobRecord> record = popLocalWork(worker);
			if (!record)
			{
				std::lock_guard<std::mutex> lock(mutex);
				record = popInjectedWorkUnlocked();
			}
			if (!record)
			{
				record = stealWork(worker);
			}
			if (!record)
			{
				return std::shared_ptr<JobRecord>();
			}

			unsigned observedReadyCount = readyCount.load(std::memory_order_acquire);
			while (observedReadyCount != 0 &&
				!readyCount.compare_exchange_weak(observedReadyCount,
					observedReadyCount - 1, std::memory_order_acq_rel,
					std::memory_order_acquire))
			{
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
					throw std::bad_alloc();
				}
#endif
				readyCount.fetch_add(1, std::memory_order_release);
			}
			else
			{
				std::lock_guard<std::mutex> lock(mutex);
				injectionQueues[record->priority].push_back(record);
#if defined(RTS_BUILD_CORE_EXTRAS)
				if (consumeJobSystemTestFault(10))
				{
					throw std::bad_alloc();
				}
#endif
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
				readyCount.fetch_add(1, std::memory_order_release);
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
		executedJobCount.fetch_add(1, std::memory_order_relaxed);
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
		record->complete.store(true, std::memory_order_release);
		record->completed.notify_all();
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
		if (group->pending.fetch_sub(1, std::memory_order_acq_rel) == 1)
		{
			group->completed.notify_all();
		}
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
		unsigned active = 0;
		if (poolWorker)
		{
			active = activeWorkers.fetch_add(1, std::memory_order_acq_rel) + 1;
			unsigned observedMaximum = maximumActiveWorkers.load(
				std::memory_order_relaxed);
			while (active > observedMaximum &&
				!maximumActiveWorkers.compare_exchange_weak(observedMaximum, active,
					std::memory_order_relaxed, std::memory_order_relaxed))
			{
			}
		}
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
#if defined(_WIN32) && !defined(_WIN64)
			DeterministicFloatingPointScope floatingPointScope;
#endif
			JobContext::State contextState(record->group.get(),
				worker.scratch.empty() ? 0 : &worker.scratch[0],
				static_cast<unsigned>(worker.scratch.size()));
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
		if (poolWorker)
		{
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

	void workerLoop(Worker *worker)
	{
#if defined(_WIN32)
		if (pinWorkers && !selectedCpuSetIds.empty())
		{
			typedef BOOL (WINAPI *SetThreadSelectedCpuSetsFunction)(
				HANDLE, const ULONG *, ULONG);
			HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
			SetThreadSelectedCpuSetsFunction setCpuSets = kernel != 0 ?
				reinterpret_cast<SetThreadSelectedCpuSetsFunction>(
					GetProcAddress(kernel, "SetThreadSelectedCpuSets")) : 0;
			if (setCpuSets != 0)
			{
				const ULONG cpuSetId = selectedCpuSetIds[
					worker->index % selectedCpuSetIds.size()];
				setCpuSets(GetCurrentThread(), &cpuSetId, 1);
			}
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
				workerSleepCount.fetch_add(1, std::memory_order_relaxed);
				workAvailable.wait(lock, [this]() {
					return stopping || readyCount.load(std::memory_order_acquire) != 0;
				});
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
	std::vector<unsigned> selectedCpuSetIds;
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
	std::atomic<unsigned> readyCount;
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
	std::atomic<unsigned> injectionHighWater;
	std::atomic<unsigned> maximumActiveWorkers;
	unsigned completionCapacity;
};

JobCpuSetInfo::JobCpuSetInfo()
	: id(0), efficiencyClass(0), parked(false),
	  allocatedToOtherProcess(false)
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
	  injectionHighWater(0), maximumActiveWorkers(0)
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
			if (!cpuSets[index].parked &&
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
	unsigned firstSelected = 0;
	if (explicitWorkerCount == 0 && policy == JOB_WORKER_POLICY_AUTO &&
		eligibleCount > selectedCount)
	{
		firstSelected = eligibleCount - selectedCount;
	}
	const unsigned writable = selectedCount < selectedIdCapacity ?
		selectedCount : selectedIdCapacity;
	for (unsigned index = 0; index < writable; ++index)
	{
		selectedIds[index] = eligible[firstSelected + index].id;
	}
	return writable;
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
	return start(startupConfig());
}

bool JobSystem::start(const JobSystemConfig &config)
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
		if (!cpuSet.parked && !cpuSet.allocatedToOtherProcess)
		{
			++eligibleCpuCount;
		}
	}
	if (eligibleCpuCount == 0)
	{
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
		}
		catch (...)
		{
			selectedCpuSetIds.clear();
		}
	}
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
		m_state->selectedCpuSetIds.swap(selectedCpuSetIds);
		m_state->pinWorkers = config.pinWorkers &&
			!m_state->selectedCpuSetIds.empty();
		++m_state->generation;
		m_state->stopping = false;
		m_state->readyCount.store(0, std::memory_order_release);
		m_state->activeWorkers.store(0, std::memory_order_release);
	}
	resetMetrics();

	try
	{
		m_state->ownerHelper.reset(new State::Worker);
		m_state->ownerHelper->index = effectiveWorkerCount;
		m_state->ownerHelper->scratch.resize(config.scratchBytesPerWorker);
		m_state->workers.reserve(effectiveWorkerCount);
		for (unsigned index = 0; index < effectiveWorkerCount; ++index)
		{
			std::unique_ptr<State::Worker> worker(new State::Worker);
			worker->index = index;
#if defined(RTS_BUILD_CORE_EXTRAS)
			if (consumeJobSystemTestFault(2))
			{
				throw std::bad_alloc();
			}
#endif
			worker->scratch.resize(config.scratchBytesPerWorker);
			m_state->workers.push_back(std::move(worker));
		}
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
		m_state->ownerHelper.reset();
		m_state->stopping = false;
		m_state->configuredWorkerCount = 0;
		m_state->completionCapacity = 0;
		m_state->selectedCpuSetIds.clear();
		m_state->pinWorkers = false;
		return false;
	}

	{
		std::lock_guard<std::mutex> lock(m_state->mutex);
		m_state->running = true;
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
	m_state->selectedCpuSetIds.clear();
	m_state->pinWorkers = false;
	lifecycleLock.lock();
	{
		std::lock_guard<std::mutex> lock(m_state->mutex);
		for (unsigned priority = 0; priority < JOB_PRIORITY_COUNT; ++priority)
		{
			m_state->injectionQueues[priority].clear();
		}
		m_state->outstanding = 0;
		m_state->readyCount.store(0, std::memory_order_release);
		m_state->configuredWorkerCount = 0;
		m_state->queueCapacity = 0;
		m_state->completionCapacity = 0;
		m_state->stopping = false;
	}
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

JobSystemMetrics JobSystem::metrics() const
{
	JobSystemMetrics result;
	if (m_state == 0)
	{
		return result;
	}
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
	result.injectionHighWater = m_state->injectionHighWater.load(std::memory_order_relaxed);
	result.maximumActiveWorkers = m_state->maximumActiveWorkers.load(std::memory_order_relaxed);
	return result;
}

void JobSystem::resetMetrics()
{
	if (m_state == 0)
	{
		return;
	}
	m_state->submittedJobCount.store(0, std::memory_order_relaxed);
	m_state->executedJobCount.store(0, std::memory_order_relaxed);
	m_state->stealCount.store(0, std::memory_order_relaxed);
	m_state->ownerHelpCount.store(0, std::memory_order_relaxed);
	m_state->waitCount.store(0, std::memory_order_relaxed);
	m_state->workerWaitRejectionCount.store(0, std::memory_order_relaxed);
	m_state->failedJobCount.store(0, std::memory_order_relaxed);
	m_state->cancelledJobCount.store(0, std::memory_order_relaxed);
	m_state->serialFallbackCount.store(0, std::memory_order_relaxed);
	m_state->totalQueueLatencyNanoseconds.store(0, std::memory_order_relaxed);
	m_state->maximumQueueLatencyNanoseconds.store(0, std::memory_order_relaxed);
	m_state->workerSleepCount.store(0, std::memory_order_relaxed);
	m_state->workerWakeCount.store(0, std::memory_order_relaxed);
	m_state->injectionHighWater.store(0, std::memory_order_relaxed);
	m_state->maximumActiveWorkers.store(0, std::memory_order_relaxed);
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
		if (groupRecord->pending.fetch_sub(1, std::memory_order_acq_rel) == 1)
		{
			groupRecord->completed.notify_all();
		}
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
		if (groupRecord->pending.fetch_sub(1, std::memory_order_acq_rel) == 1)
		{
			groupRecord->completed.notify_all();
		}
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
		if (groupRecord->pending.fetch_sub(submissionCount,
			std::memory_order_acq_rel) == submissionCount)
		{
			groupRecord->completed.notify_all();
		}
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
	auto promoteInQueues = [job, priority](
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
