#include "Utility/CppMacros.h"
#include "WWLib/always.h"
#include "WWLib/STLUtils.h"
#include "WWLib/win.h"
#include "WWLib/WWCommon.h"
#include "WWLib/wwstring.h"

#define private public
#include "WWLib/thread.h"
#undef private

#include <windows.h>

#include <cstdio>
#include <thread>

static_assert(sizeof(ThreadClass::handle) == sizeof(HANDLE),
	"ThreadClass must retain the complete caller-owned native thread handle");

static HANDLE g_startEntered = nullptr;
static HANDLE g_releaseStart = nullptr;

static void blockBeforeThreadEntry(ThreadClass *)
{
	SetEvent(g_startEntered);
	WaitForSingleObject(g_releaseStart, INFINITE);
}

class ImmediateStopThread final : public ThreadClass
{
public:
	ImmediateStopThread() : ThreadClass("immediate stop"), cooperativelyStopped(CreateEvent(nullptr, TRUE, FALSE, nullptr)) {}
	~ImmediateStopThread() override
	{
		Stop();
		CloseHandle(cooperativelyStopped);
	}

	HANDLE cooperativelyStopped;
	bool stopRequested() const { return running == 0; }
	static void setStartupHook(void (*hook)(ThreadClass *)) { Set_Test_Startup_Hook(hook); }

protected:
	void Thread_Function() override
	{
		while (running)
			Switch_Thread();
		SetEvent(cooperativelyStopped);
	}
};

class RestartBarrierThread final : public ThreadClass
{
public:
	RestartBarrierThread() : ThreadClass("restart barrier"), entered(CreateEvent(nullptr, TRUE, FALSE, nullptr)),
		release(CreateEvent(nullptr, TRUE, FALSE, nullptr)) {}

	~RestartBarrierThread() override
	{
		SetEvent(release);
		Stop();
		CloseHandle(release);
		CloseHandle(entered);
	}

	HANDLE entered;
	HANDLE release;
	unsigned threadId() const { return ThreadID; }

protected:
	void Thread_Function() override
	{
		InterlockedExchange(&running, 0);
		SetEvent(entered);
		WaitForSingleObject(release, INFINITE);
	}
};

int main()
{
	g_startEntered = CreateEvent(nullptr, TRUE, FALSE, nullptr);
	g_releaseStart = CreateEvent(nullptr, TRUE, FALSE, nullptr);
	ImmediateStopThread::setStartupHook(&blockBeforeThreadEntry);
	ImmediateStopThread immediateStop;
	immediateStop.Execute();
	if (WaitForSingleObject(g_startEntered, 5000) != WAIT_OBJECT_0)
	{
		std::puts("Thread did not reach the pre-entry stop barrier.");
		ImmediateStopThread::setStartupHook(nullptr);
		SetEvent(g_releaseStart);
		CloseHandle(g_releaseStart);
		CloseHandle(g_startEntered);
		return 1;
	}

	std::thread stopper([&immediateStop]()
	{
		immediateStop.Stop(500);
	});
	while (!immediateStop.stopRequested())
		SwitchToThread();
	SetEvent(g_releaseStart);
	stopper.join();
	ImmediateStopThread::setStartupHook(nullptr);
	const bool cooperativelyStopped = WaitForSingleObject(immediateStop.cooperativelyStopped, 0) == WAIT_OBJECT_0;
	CloseHandle(g_releaseStart);
	CloseHandle(g_startEntered);
	if (!cooperativelyStopped)
	{
		std::puts("Stop before worker entry did not produce a cooperative exit.");
		return 1;
	}

	RestartBarrierThread thread;
	thread.Execute();
	if (WaitForSingleObject(thread.entered, 5000) != WAIT_OBJECT_0)
	{
		std::puts("Thread did not reach the restart barrier.");
		return 1;
	}

	const bool reportedRunning = thread.Is_Running();
	const unsigned firstThreadId = thread.threadId();
#ifdef NDEBUG
	thread.Execute();
	const bool preservedThreadId = thread.threadId() == firstThreadId;
#else
	const bool preservedThreadId = true;
#endif
	if (!preservedThreadId)
	{
		std::puts("Execute replaced an unsignaled native thread in a release build.");
		std::fflush(stdout);
		ExitProcess(1);
	}
	SetEvent(thread.release);
	thread.Stop();
	if (!reportedRunning)
	{
		std::puts("Thread reported stopped before its native handle was signaled.");
		return 1;
	}
	return 0;
}
