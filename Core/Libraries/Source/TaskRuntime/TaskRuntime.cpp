#include "Lib/TaskRuntime.h"

#include <deque>
#include <new>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <pthread.h>
#endif

#if defined(RTS_BUILD_CORE_EXTRAS)
enum TaskRuntimeTestEvent
{
	TASK_RUNTIME_TEST_DESTRUCTOR_ENTRY = 1,
	TASK_RUNTIME_TEST_WORKER_JOINED = 2,
	TASK_RUNTIME_TEST_FAIL_STATE_ALLOCATION = 3,
	TASK_RUNTIME_TEST_FAIL_THREAD_RESERVE = 4,
	TASK_RUNTIME_TEST_FAIL_QUEUE_PUSH = 5,
	TASK_RUNTIME_TEST_FAIL_SYNC_INITIALIZATION = 6
};

typedef void (*TaskRuntimeTestObserver)(unsigned event);
static TaskRuntimeTestObserver s_taskRuntimeTestObserver = 0;
static unsigned s_taskRuntimeTestAllocationFault = 0;
static unsigned s_taskRuntimeTestAllocationFaultOccurrence = 0;

extern "C" void rts_task_runtime_set_test_observer(TaskRuntimeTestObserver observer)
{
	s_taskRuntimeTestObserver = observer;
}

extern "C" void rts_task_runtime_set_test_allocation_fault(unsigned event, unsigned occurrence)
{
	s_taskRuntimeTestAllocationFault = event;
	s_taskRuntimeTestAllocationFaultOccurrence = occurrence;
}

static void notifyTaskRuntimeTestObserver(unsigned event)
{
	if (s_taskRuntimeTestObserver != 0)
	{
		s_taskRuntimeTestObserver(event);
	}
}

static bool consumeTaskRuntimeTestAllocationFault(unsigned event)
{
	if (s_taskRuntimeTestAllocationFault != event || s_taskRuntimeTestAllocationFaultOccurrence == 0)
	{
		return false;
	}

	--s_taskRuntimeTestAllocationFaultOccurrence;
	if (s_taskRuntimeTestAllocationFaultOccurrence != 0)
	{
		return false;
	}

	s_taskRuntimeTestAllocationFault = 0;
	return true;
}
#endif

namespace rts
{
Task::Task()
{
}

Task::~Task()
{
}

struct TaskRuntime::State
{
public:
	State()
		: m_queueCapacity(0),
		  m_workerCount(0),
		  m_activeTaskCount(0),
		  m_accepting(false),
		  m_stopping(false)
#if defined(RTS_BUILD_CORE_EXTRAS)
		  , m_syncInitializationFaulted(false)
#endif
#if defined(_WIN32)
		  , m_workAvailable(0),
		  m_idle(0),
		  m_syncReady(false)
#else
		  , m_mutexInitialized(false),
		  m_workConditionInitialized(false),
		  m_idleConditionInitialized(false)
#endif
	{
#if defined(_WIN32)
		InitializeCriticalSection(&m_mutex);
		m_workAvailable = CreateEvent(0, TRUE, FALSE, 0);
		m_idle = CreateEvent(0, TRUE, TRUE, 0);
		m_syncReady = m_workAvailable != 0 && m_idle != 0;
#else
		if (pthread_mutex_init(&m_mutex, 0) == 0)
		{
			m_mutexInitialized = true;
			if (pthread_cond_init(&m_workCondition, 0) == 0)
			{
				m_workConditionInitialized = true;
				if (pthread_cond_init(&m_idleCondition, 0) == 0)
				{
					m_idleConditionInitialized = true;
				}
			}
		}
#endif
#if defined(RTS_BUILD_CORE_EXTRAS)
		if (consumeTaskRuntimeTestAllocationFault(TASK_RUNTIME_TEST_FAIL_SYNC_INITIALIZATION))
		{
			m_syncInitializationFaulted = true;
		}
#endif
	}

	~State()
	{
#if defined(_WIN32)
		if (m_workAvailable != 0)
		{
			CloseHandle(m_workAvailable);
		}
		if (m_idle != 0)
		{
			CloseHandle(m_idle);
		}
		DeleteCriticalSection(&m_mutex);
#else
		if (m_idleConditionInitialized)
		{
			pthread_cond_destroy(&m_idleCondition);
		}
		if (m_workConditionInitialized)
		{
			pthread_cond_destroy(&m_workCondition);
		}
		if (m_mutexInitialized)
		{
			pthread_mutex_destroy(&m_mutex);
		}
#endif
	}

	bool start(unsigned workerCount, unsigned queueCapacity)
	{
		unsigned workerIndex;

		if (workerCount == 0 || queueCapacity == 0 || !syncReady())
		{
			return false;
		}

		lock();
		if (m_accepting || m_stopping || !m_threads.empty())
		{
			unlock();
			return false;
		}

		try
		{
#if defined(RTS_BUILD_CORE_EXTRAS)
			if (consumeTaskRuntimeTestAllocationFault(TASK_RUNTIME_TEST_FAIL_THREAD_RESERVE))
			{
				throw std::bad_alloc();
			}
#endif
			m_threads.reserve(workerCount);
#if defined(_WIN32)
			m_threadIds.reserve(workerCount);
		}
		catch (...)
		{
			unlock();
			return false;
		}
		ResetEvent(m_workAvailable);
		SetEvent(m_idle);
#else
		}
		catch (...)
		{
			unlock();
			return false;
		}
#endif
		m_queueCapacity = queueCapacity;
		m_workerCount = workerCount;
		m_activeTaskCount = 0;
		m_accepting = false;
		m_stopping = false;

		for (workerIndex = 0; workerIndex < workerCount; ++workerIndex)
		{
			if (!hasThreadStorageForOneMoreUnlocked())
			{
				break;
			}
#if defined(_WIN32)
			unsigned threadId = 0;
			HANDLE thread = (HANDLE)_beginthreadex(0, 0, workerEntry, this, 0, &threadId);
			if (thread == 0)
			{
				break;
			}
			m_threads.push_back(thread);
			m_threadIds.push_back(threadId);
#else
			pthread_t thread;
			if (pthread_create(&thread, 0, workerEntry, this) != 0)
			{
				break;
			}
			m_threads.push_back(thread);
#endif
		}

		if (workerIndex == workerCount)
		{
			m_accepting = true;
			unlock();
			return true;
		}

		m_accepting = false;
		m_stopping = true;
		signalWorkersUnlocked();
		unlock();
		joinWorkers();
		lock();
		resetAfterJoinUnlocked();
		unlock();
		return false;
	}

	bool trySubmitBatch(Task *const *tasks, unsigned taskCount, bool submitFront)
	{
		unsigned taskIndex;
		unsigned previousTaskIndex;
		bool accepted = false;

		if (!syncReady())
		{
			return false;
		}

		lock();
		if (m_accepting && !m_stopping && tasks != 0 && taskCount != 0 &&
			taskCount <= m_queueCapacity - (unsigned)m_tasks.size())
		{
			accepted = true;
			for (taskIndex = 0; taskIndex < taskCount; ++taskIndex)
			{
				if (tasks[taskIndex] == 0)
				{
					accepted = false;
					break;
				}
				for (previousTaskIndex = 0; previousTaskIndex < taskIndex; ++previousTaskIndex)
				{
					if (tasks[previousTaskIndex] == tasks[taskIndex])
					{
						accepted = false;
						break;
					}
				}
				if (!accepted)
				{
					break;
				}
			}

			if (accepted)
			{
				unsigned appendedTaskCount = 0;
				try
				{
					for (taskIndex = 0; taskIndex < taskCount; ++taskIndex)
					{
#if defined(RTS_BUILD_CORE_EXTRAS)
						if (consumeTaskRuntimeTestAllocationFault(TASK_RUNTIME_TEST_FAIL_QUEUE_PUSH))
						{
							throw std::bad_alloc();
						}
#endif
						if (submitFront)
						{
							m_tasks.push_front(tasks[taskIndex]);
						}
						else
						{
							m_tasks.push_back(tasks[taskIndex]);
						}
						++appendedTaskCount;
					}
				}
				catch (...)
				{
					while (appendedTaskCount != 0)
					{
						if (submitFront)
						{
							m_tasks.pop_front();
						}
						else
						{
							m_tasks.pop_back();
						}
						--appendedTaskCount;
					}
					unlock();
					return false;
				}
#if defined(_WIN32)
				ResetEvent(m_idle);
#endif
				signalWorkersUnlocked();
			}
		}
		unlock();
		return accepted;
	}

	bool tryTake(Task *task)
	{
		std::deque<Task *>::iterator taskIterator;
		bool taken = false;

		if (!syncReady() || task == 0)
		{
			return false;
		}

		lock();
		if (m_accepting && !m_stopping)
		{
			for (taskIterator = m_tasks.begin(); taskIterator != m_tasks.end(); ++taskIterator)
			{
				if (*taskIterator == task)
				{
					m_tasks.erase(taskIterator);
					taken = true;
					break;
				}
			}
		}

		if (taken && m_tasks.empty())
		{
#if defined(_WIN32)
			ResetEvent(m_workAvailable);
			if (m_activeTaskCount == 0)
			{
				SetEvent(m_idle);
			}
#else
			if (m_activeTaskCount == 0)
			{
				pthread_cond_broadcast(&m_idleCondition);
			}
#endif
		}
		unlock();
		return taken;
	}

	void waitUntilIdle()
	{
		if (!syncReady())
		{
			return;
		}

		lock();
		if (isCurrentWorkerUnlocked())
		{
			unlock();
			return;
		}
#if defined(_WIN32)
		if (m_tasks.empty() && m_activeTaskCount == 0)
		{
			unlock();
			return;
		}
		unlock();
		WaitForSingleObject(m_idle, INFINITE);
#else
		while (!m_tasks.empty() || m_activeTaskCount != 0)
		{
			pthread_cond_wait(&m_idleCondition, &m_mutex);
		}
		unlock();
#endif
	}

	void shutdown()
	{
		if (!syncReady())
		{
			return;
		}

		lock();
		if (isCurrentWorkerUnlocked())
		{
			unlock();
			return;
		}
		if (m_threads.empty())
		{
			m_accepting = false;
			m_stopping = false;
			m_queueCapacity = 0;
			m_workerCount = 0;
			unlock();
			return;
		}

		m_accepting = false;
		m_stopping = true;
		signalWorkersUnlocked();
		unlock();

		joinWorkers();

		lock();
		resetAfterJoinUnlocked();
		unlock();
	}

	bool isRunning() const
	{
		bool result;
		if (!syncReady())
		{
			return false;
		}
		lock();
		result = m_accepting;
		unlock();
		return result;
	}

	unsigned workerCount() const
	{
		unsigned result;
		if (!syncReady())
		{
			return 0;
		}
		lock();
		result = m_workerCount;
		unlock();
		return result;
	}

	unsigned pendingTaskCount() const
	{
		unsigned result;
		if (!syncReady())
		{
			return 0;
		}
		lock();
		result = (unsigned)m_tasks.size();
		unlock();
		return result;
	}

private:
	friend class TaskRuntime;

	State(const State &);
	State &operator=(const State &);

	bool syncReady() const
	{
#if defined(RTS_BUILD_CORE_EXTRAS)
		if (m_syncInitializationFaulted)
		{
			return false;
		}
#endif
#if defined(_WIN32)
		return m_syncReady;
#else
		return m_mutexInitialized && m_workConditionInitialized && m_idleConditionInitialized;
#endif
	}

	void lock() const
	{
#if defined(_WIN32)
		EnterCriticalSection(&m_mutex);
#else
		pthread_mutex_lock(&m_mutex);
#endif
	}

	void unlock() const
	{
#if defined(_WIN32)
		LeaveCriticalSection(&m_mutex);
#else
		pthread_mutex_unlock(&m_mutex);
#endif
	}

	void signalWorkersUnlocked()
	{
#if defined(_WIN32)
		SetEvent(m_workAvailable);
#else
		pthread_cond_broadcast(&m_workCondition);
#endif
	}

	bool isCurrentWorkerUnlocked() const
	{
		unsigned workerIndex;
#if defined(_WIN32)
		unsigned currentThreadId = (unsigned)GetCurrentThreadId();
		for (workerIndex = 0; workerIndex < m_threadIds.size(); ++workerIndex)
		{
			if (m_threadIds[workerIndex] == currentThreadId)
			{
				return true;
			}
		}
#else
		pthread_t currentThread = pthread_self();
		for (workerIndex = 0; workerIndex < m_threads.size(); ++workerIndex)
		{
			if (pthread_equal(m_threads[workerIndex], currentThread) != 0)
			{
				return true;
			}
		}
#endif
		return false;
	}

	bool hasThreadStorageForOneMoreUnlocked() const
	{
		if (m_threads.size() >= m_threads.capacity())
		{
			return false;
		}
#if defined(_WIN32)
		if (m_threadIds.size() >= m_threadIds.capacity())
		{
			return false;
		}
#endif
		return true;
	}

	void joinWorkers()
	{
		unsigned workerIndex;
		for (workerIndex = 0; workerIndex < m_threads.size(); ++workerIndex)
		{
#if defined(_WIN32)
			bool joined = WaitForSingleObject(m_threads[workerIndex], INFINITE) == WAIT_OBJECT_0;
			bool closed = CloseHandle(m_threads[workerIndex]) != FALSE;
			if (joined && closed)
#else
			if (pthread_join(m_threads[workerIndex], 0) == 0)
#endif
			{
#if defined(RTS_BUILD_CORE_EXTRAS)
				notifyTaskRuntimeTestObserver(TASK_RUNTIME_TEST_WORKER_JOINED);
#endif
			}
		}
		m_threads.clear();
#if defined(_WIN32)
		m_threadIds.clear();
#endif
	}

	void resetAfterJoinUnlocked()
	{
		m_accepting = false;
		m_stopping = false;
		m_queueCapacity = 0;
		m_workerCount = 0;
		m_activeTaskCount = 0;
		m_tasks.clear();
#if defined(_WIN32)
		ResetEvent(m_workAvailable);
		SetEvent(m_idle);
#else
		pthread_cond_broadcast(&m_idleCondition);
#endif
	}

	void workerLoop()
	{
		for (;;)
		{
			Task *task;
#if defined(_WIN32)
			WaitForSingleObject(m_workAvailable, INFINITE);
			lock();
			if (m_tasks.empty())
			{
				if (m_stopping)
				{
					unlock();
					return;
				}
				ResetEvent(m_workAvailable);
				unlock();
				continue;
			}
#else
			lock();
			while (m_tasks.empty() && !m_stopping)
			{
				pthread_cond_wait(&m_workCondition, &m_mutex);
			}
			if (m_tasks.empty() && m_stopping)
			{
				unlock();
				return;
			}
#endif
			task = m_tasks.front();
			m_tasks.pop_front();
			++m_activeTaskCount;
#if defined(_WIN32)
			if (m_tasks.empty() && !m_stopping)
			{
				ResetEvent(m_workAvailable);
			}
#endif
			unlock();

			task->execute();
			delete task;

			lock();
			--m_activeTaskCount;
			if (m_tasks.empty() && m_activeTaskCount == 0)
			{
#if defined(_WIN32)
				SetEvent(m_idle);
#else
				pthread_cond_broadcast(&m_idleCondition);
#endif
			}
			unlock();
		}
	}

#if defined(_WIN32)
	static unsigned __stdcall workerEntry(void *context)
	{
		State *state = (State *)context;
		state->workerLoop();
		return 0;
	}
#else
	static void *workerEntry(void *context)
	{
		State *state = (State *)context;
		state->workerLoop();
		return 0;
	}
#endif

	std::deque<Task *> m_tasks;
	unsigned m_queueCapacity;
	unsigned m_workerCount;
	unsigned m_activeTaskCount;
	bool m_accepting;
	bool m_stopping;
#if defined(RTS_BUILD_CORE_EXTRAS)
	bool m_syncInitializationFaulted;
#endif
#if defined(_WIN32)
	mutable CRITICAL_SECTION m_mutex;
	HANDLE m_workAvailable;
	HANDLE m_idle;
	std::vector<HANDLE> m_threads;
	std::vector<unsigned> m_threadIds;
	bool m_syncReady;
#else
	mutable pthread_mutex_t m_mutex;
	pthread_cond_t m_workCondition;
	pthread_cond_t m_idleCondition;
	std::vector<pthread_t> m_threads;
	bool m_mutexInitialized;
	bool m_workConditionInitialized;
	bool m_idleConditionInitialized;
#endif
};

TaskRuntime::TaskRuntime() : m_state(0)
{
}

TaskRuntime::~TaskRuntime()
{
#if defined(RTS_BUILD_CORE_EXTRAS)
	notifyTaskRuntimeTestObserver(TASK_RUNTIME_TEST_DESTRUCTOR_ENTRY);
#endif
	shutdown();
	delete m_state;
}

bool TaskRuntime::start(unsigned workerCount, unsigned queueCapacity)
{
	if (m_state == 0)
	{
		State *state = 0;
		try
		{
#if defined(RTS_BUILD_CORE_EXTRAS)
			if (consumeTaskRuntimeTestAllocationFault(TASK_RUNTIME_TEST_FAIL_STATE_ALLOCATION))
			{
				throw std::bad_alloc();
			}
#endif
			state = new (std::nothrow) State;
		}
		catch (...)
		{
			return false;
		}
		if (state == 0)
		{
			return false;
		}
		if (!state->syncReady())
		{
			delete state;
			return false;
		}
		m_state = state;
	}
	return m_state->start(workerCount, queueCapacity);
}

bool TaskRuntime::trySubmit(Task *task)
{
	if (m_state == 0)
	{
		return false;
	}
	return m_state->trySubmitBatch(&task, 1, false);
}

bool TaskRuntime::trySubmitFront(Task *task)
{
	if (m_state == 0)
	{
		return false;
	}
	return m_state->trySubmitBatch(&task, 1, true);
}

bool TaskRuntime::trySubmitBatch(Task *const *tasks, unsigned taskCount)
{
	if (m_state == 0)
	{
		return false;
	}
	return m_state->trySubmitBatch(tasks, taskCount, false);
}

bool TaskRuntime::tryTake(Task *task)
{
	if (m_state == 0)
	{
		return false;
	}
	return m_state->tryTake(task);
}

void TaskRuntime::waitUntilIdle()
{
	if (m_state != 0)
	{
		m_state->waitUntilIdle();
	}
}

void TaskRuntime::shutdown()
{
	if (m_state != 0)
	{
		m_state->shutdown();
	}
}

bool TaskRuntime::isRunning() const
{
	return m_state != 0 && m_state->isRunning();
}

unsigned TaskRuntime::workerCount() const
{
	return m_state != 0 ? m_state->workerCount() : 0;
}

unsigned TaskRuntime::pendingTaskCount() const
{
	return m_state != 0 ? m_state->pendingTaskCount() : 0;
}
}
