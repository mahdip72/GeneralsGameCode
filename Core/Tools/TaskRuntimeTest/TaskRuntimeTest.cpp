#include "Lib/TaskRuntime.h"

#include <stdio.h>

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <pthread.h>
#endif

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
extern "C" void rts_task_runtime_set_test_observer(TaskRuntimeTestObserver observer);
extern "C" void rts_task_runtime_set_test_allocation_fault(unsigned event, unsigned occurrence);

struct TaskRecord
{
	TaskRecord() : executions(0), destructions(0), order(0) {}

	unsigned executions;
	unsigned destructions;
	unsigned order;
};

class Gate
{
public:
	Gate()
#if defined(_WIN32)
		: m_entered(CreateEvent(0, TRUE, FALSE, 0)), m_open(CreateEvent(0, TRUE, FALSE, 0))
#endif
	{
#if !defined(_WIN32)
		pthread_mutex_init(&m_mutex, 0);
		pthread_cond_init(&m_condition, 0);
		m_entered = false;
		m_open = false;
#endif
	}

	~Gate()
	{
#if defined(_WIN32)
		CloseHandle(m_entered);
		CloseHandle(m_open);
#else
		pthread_cond_destroy(&m_condition);
		pthread_mutex_destroy(&m_mutex);
#endif
	}

	void waitForEntry()
	{
#if defined(_WIN32)
		WaitForSingleObject(m_entered, INFINITE);
#else
		pthread_mutex_lock(&m_mutex);
		while (!m_entered)
		{
			pthread_cond_wait(&m_condition, &m_mutex);
		}
		pthread_mutex_unlock(&m_mutex);
#endif
	}

	void waitUntilOpen()
	{
#if defined(_WIN32)
		SetEvent(m_entered);
		WaitForSingleObject(m_open, INFINITE);
#else
		pthread_mutex_lock(&m_mutex);
		m_entered = true;
		pthread_cond_broadcast(&m_condition);
		while (!m_open)
		{
			pthread_cond_wait(&m_condition, &m_mutex);
		}
		pthread_mutex_unlock(&m_mutex);
#endif
	}

	void open()
	{
#if defined(_WIN32)
		SetEvent(m_open);
#else
		pthread_mutex_lock(&m_mutex);
		m_open = true;
		pthread_cond_broadcast(&m_condition);
		pthread_mutex_unlock(&m_mutex);
#endif
	}

private:
	Gate(const Gate &);
	Gate &operator=(const Gate &);

#if defined(_WIN32)
	HANDLE m_entered;
	HANDLE m_open;
#else
	pthread_mutex_t m_mutex;
	pthread_cond_t m_condition;
	bool m_entered;
	bool m_open;
#endif
};

class Signal
{
public:
	Signal()
#if defined(_WIN32)
		: m_event(CreateEvent(0, TRUE, FALSE, 0))
#endif
	{
#if !defined(_WIN32)
		pthread_mutex_init(&m_mutex, 0);
		pthread_cond_init(&m_condition, 0);
		m_signaled = false;
#endif
	}

	~Signal()
	{
#if defined(_WIN32)
		CloseHandle(m_event);
#else
		pthread_cond_destroy(&m_condition);
		pthread_mutex_destroy(&m_mutex);
#endif
	}

	void signal()
	{
#if defined(_WIN32)
		SetEvent(m_event);
#else
		pthread_mutex_lock(&m_mutex);
		m_signaled = true;
		pthread_cond_broadcast(&m_condition);
		pthread_mutex_unlock(&m_mutex);
#endif
	}

	void wait()
	{
#if defined(_WIN32)
		WaitForSingleObject(m_event, INFINITE);
#else
		pthread_mutex_lock(&m_mutex);
		while (!m_signaled)
		{
			pthread_cond_wait(&m_condition, &m_mutex);
		}
		pthread_mutex_unlock(&m_mutex);
#endif
	}

private:
	Signal(const Signal &);
	Signal &operator=(const Signal &);

#if defined(_WIN32)
	HANDLE m_event;
#else
	pthread_mutex_t m_mutex;
	pthread_cond_t m_condition;
	bool m_signaled;
#endif
};

class RuntimeJoinObserver
{
public:
	RuntimeJoinObserver() : m_destructorEntries(0), m_workerJoins(0)
	{
#if defined(_WIN32)
		InitializeCriticalSection(&m_mutex);
#else
		pthread_mutex_init(&m_mutex, 0);
#endif
	}

	~RuntimeJoinObserver()
	{
#if defined(_WIN32)
		DeleteCriticalSection(&m_mutex);
#else
		pthread_mutex_destroy(&m_mutex);
#endif
	}

	void record(unsigned event)
	{
		lock();
		if (event == TASK_RUNTIME_TEST_DESTRUCTOR_ENTRY)
		{
			++m_destructorEntries;
		}
		else if (event == TASK_RUNTIME_TEST_WORKER_JOINED)
		{
			++m_workerJoins;
		}
		unlock();
		if (event == TASK_RUNTIME_TEST_DESTRUCTOR_ENTRY)
		{
			m_destructorEntered.signal();
		}
	}

	void waitForDestructorEntry()
	{
		m_destructorEntered.wait();
	}

	unsigned destructorEntryCount()
	{
		unsigned result;
		lock();
		result = m_destructorEntries;
		unlock();
		return result;
	}

	unsigned workerJoinCount()
	{
		unsigned result;
		lock();
		result = m_workerJoins;
		unlock();
		return result;
	}

private:
	RuntimeJoinObserver(const RuntimeJoinObserver &);
	RuntimeJoinObserver &operator=(const RuntimeJoinObserver &);

	void lock()
	{
#if defined(_WIN32)
		EnterCriticalSection(&m_mutex);
#else
		pthread_mutex_lock(&m_mutex);
#endif
	}

	void unlock()
	{
#if defined(_WIN32)
		LeaveCriticalSection(&m_mutex);
#else
		pthread_mutex_unlock(&m_mutex);
#endif
	}

	Signal m_destructorEntered;
	unsigned m_destructorEntries;
	unsigned m_workerJoins;
#if defined(_WIN32)
	CRITICAL_SECTION m_mutex;
#else
	pthread_mutex_t m_mutex;
#endif
};

static RuntimeJoinObserver *s_runtimeJoinObserver = 0;

static void recordTaskRuntimeEvent(unsigned event)
{
	if (s_runtimeJoinObserver != 0)
	{
		s_runtimeJoinObserver->record(event);
	}
}

class RecordingTask : public rts::Task
{
public:
	RecordingTask(TaskRecord *record, unsigned *nextOrder) : m_record(record), m_nextOrder(nextOrder) {}
	~RecordingTask() { ++m_record->destructions; }

	void execute()
	{
		++m_record->executions;
		if (m_nextOrder != 0)
		{
			m_record->order = *m_nextOrder;
			++*m_nextOrder;
		}
	}

private:
	RecordingTask(const RecordingTask &);
	RecordingTask &operator=(const RecordingTask &);

	TaskRecord *m_record;
	unsigned *m_nextOrder;
};

class GateTask : public rts::Task
{
public:
	GateTask(Gate *gate, TaskRecord *record) : m_gate(gate), m_record(record) {}
	~GateTask() { ++m_record->destructions; }

	void execute()
	{
		m_gate->waitUntilOpen();
		++m_record->executions;
	}

private:
	GateTask(const GateTask &);
	GateTask &operator=(const GateTask &);

	Gate *m_gate;
	TaskRecord *m_record;
};

class RuntimeDestructionThread
{
public:
	RuntimeDestructionThread(rts::TaskRuntime *runtime)
		: m_runtime(runtime)
#if defined(_WIN32)
		, m_thread(0)
#endif
	{
	}

	bool start()
	{
#if defined(_WIN32)
		unsigned threadId = 0;
		m_thread = (HANDLE)_beginthreadex(0, 0, threadEntry, this, 0, &threadId);
		return m_thread != 0;
#else
		return pthread_create(&m_thread, 0, threadEntry, this) == 0;
#endif
	}

	void waitForDestructionPermission()
	{
		m_permission.waitForEntry();
	}

	void allowDestruction()
	{
		m_permission.open();
	}

	void waitForFinish()
	{
		m_finished.waitForEntry();
	}

	void allowFinish()
	{
		m_finished.open();
	}

	void join()
	{
#if defined(_WIN32)
		WaitForSingleObject(m_thread, INFINITE);
		CloseHandle(m_thread);
		m_thread = 0;
#else
		pthread_join(m_thread, 0);
#endif
	}

private:
	RuntimeDestructionThread(const RuntimeDestructionThread &);
	RuntimeDestructionThread &operator=(const RuntimeDestructionThread &);

#if defined(_WIN32)
	static unsigned __stdcall threadEntry(void *context)
#else
	static void *threadEntry(void *context)
#endif
	{
		RuntimeDestructionThread *thread = (RuntimeDestructionThread *)context;
		thread->run();
#if defined(_WIN32)
		return 0;
#else
		return 0;
#endif
	}

	void run()
	{
		m_permission.waitUntilOpen();
		delete m_runtime;
		m_finished.waitUntilOpen();
	}

	rts::TaskRuntime *m_runtime;
	Gate m_permission;
	Gate m_finished;
#if defined(_WIN32)
	HANDLE m_thread;
#else
	pthread_t m_thread;
#endif
};

static int check(bool value, const char *testName, const char *expression)
{
	if (!value)
	{
		fprintf(stderr, "%s: %s\n", testName, expression);
		return 1;
	}
	return 0;
}

#define CHECK(testName, expression) do { if (check((expression), testName, #expression) != 0) return 1; } while (0)

static int testInvalidStartArguments()
{
	const char *testName = "testInvalidStartArguments";
	rts::TaskRuntime runtime;
	CHECK(testName, !runtime.start(0, 1));
	CHECK(testName, !runtime.start(1, 0));
	CHECK(testName, !runtime.isRunning());
	CHECK(testName, runtime.start(1, 1));
	CHECK(testName, !runtime.start(1, 1));
	runtime.shutdown();
	return 0;
}

static int testExactlyOnceAtOneAndFourWorkers()
{
	const char *testName = "testExactlyOnceAtOneAndFourWorkers";
	const unsigned workerCounts[] = { 1, 4 };
	unsigned workerIndex;

	for (workerIndex = 0; workerIndex < 2; ++workerIndex)
	{
		TaskRecord records[8];
		unsigned taskIndex;
		rts::TaskRuntime runtime;

		CHECK(testName, runtime.start(workerCounts[workerIndex], 8));
		for (taskIndex = 0; taskIndex < 8; ++taskIndex)
		{
			CHECK(testName, runtime.trySubmit(new RecordingTask(&records[taskIndex], 0)));
		}
		runtime.waitUntilIdle();
		runtime.shutdown();
		for (taskIndex = 0; taskIndex < 8; ++taskIndex)
		{
			CHECK(testName, records[taskIndex].executions == 1);
			CHECK(testName, records[taskIndex].destructions == 1);
		}
	}
	return 0;
}

static int testFifoDequeueAtOneWorker()
{
	const char *testName = "testFifoDequeueAtOneWorker";
	TaskRecord records[4];
	unsigned nextOrder = 0;
	unsigned taskIndex;
	rts::TaskRuntime runtime;

	CHECK(testName, runtime.start(1, 4));
	for (taskIndex = 0; taskIndex < 4; ++taskIndex)
	{
		CHECK(testName, runtime.trySubmit(new RecordingTask(&records[taskIndex], &nextOrder)));
	}
	runtime.waitUntilIdle();
	runtime.shutdown();
	for (taskIndex = 0; taskIndex < 4; ++taskIndex)
	{
		CHECK(testName, records[taskIndex].order == taskIndex);
	}
	return 0;
}

static int testQueuedTaskCanBeTakenWithoutTakingActiveTask()
{
	const char *testName = "testQueuedTaskCanBeTakenWithoutTakingActiveTask";
	Gate gate;
	TaskRecord activeRecord;
	TaskRecord queuedRecord;
	rts::TaskRuntime runtime;
	rts::Task *activeTask = new GateTask(&gate, &activeRecord);
	rts::Task *queuedTask = new RecordingTask(&queuedRecord, 0);
	bool queuedTaken;
	bool activeTaken;
	int result = 0;

	if (!runtime.start(1, 2))
	{
		delete activeTask;
		delete queuedTask;
		return check(false, testName, "runtime starts");
	}
	if (!runtime.trySubmit(activeTask))
	{
		delete activeTask;
		delete queuedTask;
		runtime.shutdown();
		return check(false, testName, "active task is accepted");
	}
	gate.waitForEntry();
	if (!runtime.trySubmit(queuedTask))
	{
		delete queuedTask;
		gate.open();
		runtime.shutdown();
		return check(false, testName, "queued task is accepted");
	}
	queuedTaken = runtime.tryTake(queuedTask);
	activeTaken = runtime.tryTake(activeTask);
	if (queuedTaken)
	{
		delete queuedTask;
	}
	gate.open();
	runtime.shutdown();
	if (activeTaken)
	{
		delete activeTask;
	}

	result |= check(queuedTaken, testName, "queued task ownership returns to caller");
	result |= check(!activeTaken, testName, "active task remains runtime owned");
	result |= check(activeRecord.executions == 1, testName, "active task executes once");
	result |= check(activeRecord.destructions == 1, testName, "active task is destroyed once");
	result |= check(queuedRecord.executions == 0, testName, "taken task does not execute");
	result |= check(queuedRecord.destructions == 1, testName, "taken task is destroyed once");
	return result;
}

static int testBatchAdmissionIsAllOrNothing()
{
	const char *testName = "testBatchAdmissionIsAllOrNothing";
	Gate gate;
	TaskRecord gateRecord;
	TaskRecord acceptedRecords[2];
	TaskRecord rejectedRecords[3];
	rts::TaskRuntime runtime;
	rts::Task *acceptedTasks[2];
	rts::Task *rejectedTasks[3];
	bool accepted;
	bool rejected;
	int result = 0;

	CHECK(testName, runtime.start(1, 4));
	CHECK(testName, runtime.trySubmit(new GateTask(&gate, &gateRecord)));
	gate.waitForEntry();
	acceptedTasks[0] = new RecordingTask(&acceptedRecords[0], 0);
	acceptedTasks[1] = new RecordingTask(&acceptedRecords[1], 0);
	accepted = runtime.trySubmitBatch(acceptedTasks, 2);
	result |= check(accepted, testName, "runtime.trySubmitBatch(acceptedTasks, 2)");
	rejectedTasks[0] = new RecordingTask(&rejectedRecords[0], 0);
	rejectedTasks[1] = new RecordingTask(&rejectedRecords[1], 0);
	rejectedTasks[2] = new RecordingTask(&rejectedRecords[2], 0);
	rejected = runtime.trySubmitBatch(rejectedTasks, 3);
	result |= check(!rejected, testName, "!runtime.trySubmitBatch(rejectedTasks, 3)");
	gate.open();
	runtime.shutdown();
	if (!accepted)
	{
		delete acceptedTasks[0];
		delete acceptedTasks[1];
	}
	if (!rejected)
	{
		delete rejectedTasks[0];
		delete rejectedTasks[1];
		delete rejectedTasks[2];
	}
	result |= check(acceptedRecords[0].executions == 1, testName, "acceptedRecords[0].executions == 1");
	result |= check(acceptedRecords[1].executions == 1, testName, "acceptedRecords[1].executions == 1");
	result |= check(acceptedRecords[0].destructions == 1, testName, "acceptedRecords[0].destructions == 1");
	result |= check(acceptedRecords[1].destructions == 1, testName, "acceptedRecords[1].destructions == 1");
	result |= check(rejectedRecords[0].executions == 0, testName, "rejectedRecords[0].executions == 0");
	result |= check(rejectedRecords[1].executions == 0, testName, "rejectedRecords[1].executions == 0");
	result |= check(rejectedRecords[0].destructions == 1, testName, "rejectedRecords[0].destructions == 1");
	result |= check(rejectedRecords[1].destructions == 1, testName, "rejectedRecords[1].destructions == 1");
	result |= check(rejectedRecords[2].executions == 0, testName, "rejectedRecords[2].executions == 0");
	result |= check(rejectedRecords[2].destructions == 1, testName, "rejectedRecords[2].destructions == 1");
	return result;
}

static int testDuplicateBatchIsRejectedWithoutPublishing()
{
	const char *testName = "testDuplicateBatchIsRejectedWithoutPublishing";
	Gate *gate = new Gate;
	TaskRecord *gateRecord = new TaskRecord;
	TaskRecord *duplicateRecord = new TaskRecord;
	TaskRecord *recoveryRecord = new TaskRecord;
	rts::TaskRuntime *runtime = new rts::TaskRuntime;
	rts::Task *gateTask = new GateTask(gate, gateRecord);
	rts::Task *duplicateTask = new RecordingTask(duplicateRecord, 0);
	rts::Task *duplicateTasks[2];
	rts::Task *recoveryTask;
	bool started;
	bool gateAccepted;
	bool duplicateAccepted;
	unsigned pendingAfterDuplicate;
	bool recoveryAccepted;
	int result = 0;

	duplicateTasks[0] = duplicateTask;
	duplicateTasks[1] = duplicateTask;
	started = runtime->start(1, 2);
	gateAccepted = started && runtime->trySubmit(gateTask);
	if (!started || !gateAccepted)
	{
		if (!gateAccepted)
		{
			delete gateTask;
		}
		runtime->shutdown();
		delete runtime;
		delete duplicateTask;
		delete gate;
		delete gateRecord;
		delete duplicateRecord;
		delete recoveryRecord;
		return check(false, testName, "runtime start and gate submission");
	}

	gate->waitForEntry();
	duplicateAccepted = runtime->trySubmitBatch(duplicateTasks, 2);
	pendingAfterDuplicate = runtime->pendingTaskCount();
	if (duplicateAccepted || pendingAfterDuplicate != 0)
	{
		/*
		 * Leave the held worker and duplicate task allocated on this failure
		 * path.  Releasing the gate or destroying the runtime would make a
		 * broken implementation delete the same Task twice.  The test process
		 * owns these allocations until process teardown, so no duplicate delete
		 * is deliberately invoked just to demonstrate the regression.
		 */
		return check(false, testName, "duplicate batch is rejected without publishing tasks");
	}

	delete duplicateTask;
	recoveryTask = new RecordingTask(recoveryRecord, 0);
	recoveryAccepted = runtime->trySubmit(recoveryTask);
	if (!recoveryAccepted)
	{
		delete recoveryTask;
	}
	gate->open();
	runtime->shutdown();
	result |= check(recoveryAccepted, testName, "runtime.trySubmit(recoveryTask) after duplicate rejection");
	result |= check(gateRecord->executions == 1, testName, "gateRecord->executions == 1");
	result |= check(gateRecord->destructions == 1, testName, "gateRecord->destructions == 1");
	result |= check(duplicateRecord->executions == 0, testName, "duplicateRecord->executions == 0");
	result |= check(duplicateRecord->destructions == 1, testName, "duplicateRecord->destructions == 1");
	result |= check(recoveryRecord->executions == 1, testName, "recoveryRecord->executions == 1");
	result |= check(recoveryRecord->destructions == 1, testName, "recoveryRecord->destructions == 1");
	delete runtime;
	delete gate;
	delete gateRecord;
	delete duplicateRecord;
	delete recoveryRecord;
	return result;
}

static int testQueueBackpressure()
{
	const char *testName = "testQueueBackpressure";
	Gate gate;
	TaskRecord gateRecord;
	TaskRecord acceptedRecord;
	TaskRecord rejectedRecord;
	rts::TaskRuntime runtime;
	rts::Task *rejectedTask;

	CHECK(testName, runtime.start(1, 1));
	CHECK(testName, runtime.trySubmit(new GateTask(&gate, &gateRecord)));
	gate.waitForEntry();
	CHECK(testName, runtime.trySubmit(new RecordingTask(&acceptedRecord, 0)));
	rejectedTask = new RecordingTask(&rejectedRecord, 0);
	CHECK(testName, !runtime.trySubmit(rejectedTask));
	gate.open();
	runtime.shutdown();
	delete rejectedTask;
	CHECK(testName, acceptedRecord.executions == 1);
	CHECK(testName, rejectedRecord.executions == 0);
	CHECK(testName, rejectedRecord.destructions == 1);
	return 0;
}

static int testShutdownDrainsAcceptedTasks()
{
	const char *testName = "testShutdownDrainsAcceptedTasks";
	Gate gate;
	TaskRecord gateRecord;
	TaskRecord queuedRecords[2];
	rts::TaskRuntime runtime;

	CHECK(testName, runtime.start(1, 2));
	CHECK(testName, runtime.trySubmit(new GateTask(&gate, &gateRecord)));
	gate.waitForEntry();
	CHECK(testName, runtime.trySubmit(new RecordingTask(&queuedRecords[0], 0)));
	CHECK(testName, runtime.trySubmit(new RecordingTask(&queuedRecords[1], 0)));
	gate.open();
	runtime.shutdown();
	CHECK(testName, gateRecord.executions == 1);
	CHECK(testName, queuedRecords[0].executions == 1);
	CHECK(testName, queuedRecords[1].executions == 1);
	CHECK(testName, !runtime.isRunning());
	return 0;
}

static int testRuntimeCanRestartAfterShutdown()
{
	const char *testName = "testRuntimeCanRestartAfterShutdown";
	TaskRecord firstRecord;
	TaskRecord secondRecord;
	rts::TaskRuntime runtime;

	CHECK(testName, runtime.start(1, 1));
	CHECK(testName, runtime.trySubmit(new RecordingTask(&firstRecord, 0)));
	runtime.shutdown();
	CHECK(testName, firstRecord.executions == 1);
	CHECK(testName, runtime.start(1, 1));
	CHECK(testName, runtime.trySubmit(new RecordingTask(&secondRecord, 0)));
	runtime.shutdown();
	CHECK(testName, secondRecord.executions == 1);
	return 0;
}

static int testStateAllocationFailureLeavesRuntimeUsable()
{
	const char *testName = "testStateAllocationFailureLeavesRuntimeUsable";
	TaskRecord rejectedRecord;
	TaskRecord acceptedRecord;
	rts::TaskRuntime runtime;
	rts::Task *rejectedTask;
	rts::Task *acceptedTask;
	bool failedStart;
	bool rejectedSubmission;
	bool started;
	bool acceptedSubmission;
	int result = 0;

	rts_task_runtime_set_test_allocation_fault(TASK_RUNTIME_TEST_FAIL_STATE_ALLOCATION, 1);
	failedStart = runtime.start(1, 1);
	rts_task_runtime_set_test_allocation_fault(0, 0);
	result |= check(!failedStart, testName, "!runtime.start(1, 1) after state allocation fault");
	result |= check(!runtime.isRunning(), testName, "!runtime.isRunning() after state allocation fault");
	result |= check(runtime.workerCount() == 0, testName, "runtime.workerCount() == 0 after state allocation fault");
	result |= check(runtime.pendingTaskCount() == 0, testName, "runtime.pendingTaskCount() == 0 after state allocation fault");
	runtime.waitUntilIdle();

	rejectedTask = new RecordingTask(&rejectedRecord, 0);
	rejectedSubmission = runtime.trySubmit(rejectedTask);
	if (!rejectedSubmission)
	{
		delete rejectedTask;
	}
	result |= check(!rejectedSubmission, testName, "!runtime.trySubmit(rejectedTask) without state");
	result |= check(rejectedRecord.executions == 0, testName, "rejectedRecord.executions == 0");
	result |= check(rejectedRecord.destructions == 1, testName, "rejectedRecord.destructions == 1");

	started = runtime.start(1, 1);
	acceptedTask = new RecordingTask(&acceptedRecord, 0);
	acceptedSubmission = started && runtime.trySubmit(acceptedTask);
	if (!acceptedSubmission)
	{
		delete acceptedTask;
	}
	runtime.shutdown();
	result |= check(started, testName, "runtime.start(1, 1) after state allocation fault");
	result |= check(acceptedSubmission, testName, "runtime.trySubmit(acceptedTask) after state allocation fault");
	result |= check(acceptedRecord.executions == 1, testName, "acceptedRecord.executions == 1");
	result |= check(acceptedRecord.destructions == 1, testName, "acceptedRecord.destructions == 1");
	return result;
}

static int testSyncInitializationFailureLeavesRuntimeUsable()
{
	const char *testName = "testSyncInitializationFailureLeavesRuntimeUsable";
	TaskRecord rejectedRecord;
	TaskRecord acceptedRecord;
	rts::TaskRuntime runtime;
	rts::Task *rejectedTask;
	rts::Task *acceptedTask;
	bool failedStart;
	bool rejectedSubmission;
	bool started;
	bool acceptedSubmission;
	int result = 0;

	rts_task_runtime_set_test_allocation_fault(TASK_RUNTIME_TEST_FAIL_SYNC_INITIALIZATION, 1);
	failedStart = runtime.start(1, 1);
	rts_task_runtime_set_test_allocation_fault(0, 0);
	result |= check(!failedStart, testName, "!runtime.start(1, 1) after sync initialization fault");
	result |= check(!runtime.isRunning(), testName, "!runtime.isRunning() after sync initialization fault");
	result |= check(runtime.workerCount() == 0, testName, "runtime.workerCount() == 0 after sync initialization fault");
	result |= check(runtime.pendingTaskCount() == 0, testName, "runtime.pendingTaskCount() == 0 after sync initialization fault");
	runtime.waitUntilIdle();

	rejectedTask = new RecordingTask(&rejectedRecord, 0);
	rejectedSubmission = runtime.trySubmit(rejectedTask);
	if (!rejectedSubmission)
	{
		delete rejectedTask;
	}
	result |= check(!rejectedSubmission, testName, "!runtime.trySubmit(rejectedTask) without state");
	result |= check(rejectedRecord.executions == 0, testName, "rejectedRecord.executions == 0");
	result |= check(rejectedRecord.destructions == 1, testName, "rejectedRecord.destructions == 1");

	started = runtime.start(1, 1);
	acceptedTask = new RecordingTask(&acceptedRecord, 0);
	acceptedSubmission = started && runtime.trySubmit(acceptedTask);
	if (!acceptedSubmission)
	{
		delete acceptedTask;
	}
	runtime.shutdown();
	result |= check(started, testName, "runtime.start(1, 1) after sync initialization fault");
	result |= check(acceptedSubmission, testName, "runtime.trySubmit(acceptedTask) after sync initialization fault");
	result |= check(acceptedRecord.executions == 1, testName, "acceptedRecord.executions == 1");
	result |= check(acceptedRecord.destructions == 1, testName, "acceptedRecord.destructions == 1");
	return result;
}

static int testThreadReserveFailureLeavesRuntimeRestartable()
{
	const char *testName = "testThreadReserveFailureLeavesRuntimeRestartable";
	TaskRecord record;
	rts::TaskRuntime runtime;
	rts::Task *task;
	bool failedStart;
	bool started;
	bool submitted;
	int result = 0;

	rts_task_runtime_set_test_allocation_fault(TASK_RUNTIME_TEST_FAIL_THREAD_RESERVE, 1);
	failedStart = runtime.start(1, 1);
	rts_task_runtime_set_test_allocation_fault(0, 0);
	result |= check(!failedStart, testName, "!runtime.start(1, 1) after thread reserve fault");
	result |= check(!runtime.isRunning(), testName, "!runtime.isRunning() after thread reserve fault");
	result |= check(runtime.workerCount() == 0, testName, "runtime.workerCount() == 0 after thread reserve fault");
	result |= check(runtime.pendingTaskCount() == 0, testName, "runtime.pendingTaskCount() == 0 after thread reserve fault");
	runtime.waitUntilIdle();

	started = runtime.start(1, 1);
	task = new RecordingTask(&record, 0);
	submitted = started && runtime.trySubmit(task);
	if (!submitted)
	{
		delete task;
	}
	runtime.shutdown();
	result |= check(started, testName, "runtime.start(1, 1) after thread reserve fault");
	result |= check(submitted, testName, "runtime.trySubmit(task) after thread reserve fault");
	result |= check(record.executions == 1, testName, "record.executions == 1");
	result |= check(record.destructions == 1, testName, "record.destructions == 1");
	return result;
}

static int testQueueAppendFailureRollsBackBatch()
{
	const char *testName = "testQueueAppendFailureRollsBackBatch";
	Gate gate;
	TaskRecord gateRecord;
	TaskRecord rejectedRecords[3];
	TaskRecord acceptedRecord;
	rts::TaskRuntime runtime;
	rts::Task *rejectedTasks[3];
	rts::Task *acceptedTask;
	bool rejected;
	bool accepted;
	unsigned pendingAfterFailure;
	unsigned taskIndex;
	int result = 0;

	CHECK(testName, runtime.start(1, 3));
	CHECK(testName, runtime.trySubmit(new GateTask(&gate, &gateRecord)));
	gate.waitForEntry();
	for (taskIndex = 0; taskIndex < 3; ++taskIndex)
	{
		rejectedTasks[taskIndex] = new RecordingTask(&rejectedRecords[taskIndex], 0);
	}
	rts_task_runtime_set_test_allocation_fault(TASK_RUNTIME_TEST_FAIL_QUEUE_PUSH, 2);
	rejected = runtime.trySubmitBatch(rejectedTasks, 3);
	rts_task_runtime_set_test_allocation_fault(0, 0);
	pendingAfterFailure = runtime.pendingTaskCount();
	acceptedTask = new RecordingTask(&acceptedRecord, 0);
	accepted = runtime.trySubmit(acceptedTask);
	gate.open();
	runtime.shutdown();

	if (!rejected && pendingAfterFailure == 0)
	{
		for (taskIndex = 0; taskIndex < 3; ++taskIndex)
		{
			delete rejectedTasks[taskIndex];
		}
	}
	if (!accepted)
	{
		delete acceptedTask;
	}
	result |= check(!rejected, testName, "!runtime.trySubmitBatch(rejectedTasks, 3) after second queue push fault");
	result |= check(pendingAfterFailure == 0, testName, "runtime.pendingTaskCount() == 0 after queue push fault");
	result |= check(accepted, testName, "runtime.trySubmit(acceptedTask) after queue push fault");
	result |= check(gateRecord.executions == 1, testName, "gateRecord.executions == 1");
	result |= check(acceptedRecord.executions == 1, testName, "acceptedRecord.executions == 1");
	result |= check(acceptedRecord.destructions == 1, testName, "acceptedRecord.destructions == 1");
	for (taskIndex = 0; taskIndex < 3; ++taskIndex)
	{
		result |= check(rejectedRecords[taskIndex].executions == 0, testName, "rejectedRecords[taskIndex].executions == 0");
		result |= check(rejectedRecords[taskIndex].destructions == 1, testName, "rejectedRecords[taskIndex].destructions == 1");
	}
	return result;
}

static int testDestructorDrainsAndJoins()
{
	const char *testName = "testDestructorDrainsAndJoins";
	Gate gate;
	TaskRecord record;
	rts::TaskRuntime *runtime = new rts::TaskRuntime;
	RuntimeDestructionThread destruction(runtime);
	RuntimeJoinObserver observer;
	int result = 0;

	CHECK(testName, runtime->start(1, 1));
	CHECK(testName, runtime->trySubmit(new GateTask(&gate, &record)));
	gate.waitForEntry();
	if (!destruction.start())
	{
		gate.open();
		runtime->shutdown();
		delete runtime;
		return check(false, testName, "destruction.start()");
	}
	destruction.waitForDestructionPermission();
	s_runtimeJoinObserver = &observer;
	rts_task_runtime_set_test_observer(recordTaskRuntimeEvent);
	destruction.allowDestruction();
	observer.waitForDestructorEntry();
	result |= check(observer.destructorEntryCount() == 1, testName, "observer.destructorEntryCount() == 1");
	result |= check(observer.workerJoinCount() == 0, testName, "observer.workerJoinCount() == 0");
	gate.open();
	destruction.waitForFinish();
	result |= check(record.executions == 1, testName, "record.executions == 1");
	result |= check(record.destructions == 1, testName, "record.destructions == 1");
	result |= check(observer.workerJoinCount() == 1, testName, "observer.workerJoinCount() == 1");
	destruction.allowFinish();
	destruction.join();
	rts_task_runtime_set_test_observer(0);
	s_runtimeJoinObserver = 0;
	return result;
}

int main()
{
	int result = 0;
	result |= testInvalidStartArguments();
	result |= testExactlyOnceAtOneAndFourWorkers();
	result |= testFifoDequeueAtOneWorker();
	result |= testQueuedTaskCanBeTakenWithoutTakingActiveTask();
	result |= testBatchAdmissionIsAllOrNothing();
	result |= testDuplicateBatchIsRejectedWithoutPublishing();
	result |= testQueueBackpressure();
	result |= testShutdownDrainsAcceptedTasks();
	result |= testRuntimeCanRestartAfterShutdown();
	result |= testStateAllocationFailureLeavesRuntimeUsable();
	result |= testSyncInitializationFailureLeavesRuntimeUsable();
	result |= testThreadReserveFailureLeavesRuntimeRestartable();
	result |= testQueueAppendFailureRollsBackBatch();
	result |= testDestructorDrainsAndJoins();
	return result;
}
