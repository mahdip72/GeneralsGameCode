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

static_assert(sizeof(ThreadClass::handle) == sizeof(HANDLE),
	"ThreadClass must retain the complete caller-owned native thread handle");

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
