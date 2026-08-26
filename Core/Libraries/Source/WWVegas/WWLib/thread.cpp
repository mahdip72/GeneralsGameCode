/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/


#include "thread.h"
#include "Except.h"
#include "WWDebug/wwdebug.h"
#pragma warning ( push )
#pragma warning ( disable : 4201 )
#include "systimer.h"
#pragma warning ( pop )

#ifdef _WIN32
#include <process.h>
#include <Utility/interlocked_adapter.h>
#include <windows.h>
#endif

ThreadClass::StartupHookType ThreadClass::s_testStartupHook = nullptr;

ThreadClass::ThreadClass(const char *thread_name, ExceptionHandlerType exception_handler) : handle(0), running(false), thread_priority(0)
{
	if (thread_name) {
		size_t nameLen = strlcpy(ThreadName, thread_name, ARRAY_SIZE(ThreadName));
		(void)nameLen; assert(nameLen < ARRAY_SIZE(ThreadName));
	} else {
		strcpy(ThreadName, "No name");
	}

	ExceptionHandler = exception_handler;
}

ThreadClass::~ThreadClass()
{
	Stop();
}

#ifdef _WIN32
unsigned __stdcall ThreadClass::Internal_Thread_Function(void* params)
#else
void __cdecl ThreadClass::Internal_Thread_Function(void* params)
#endif
{
	ThreadClass* tc=reinterpret_cast<ThreadClass*>(params);
	if (s_testStartupHook != nullptr)
		s_testStartupHook(tc);
	#ifndef _WIN32
	tc->running = true;
	#else
	// Execute publishes the initial running state before creating the worker.
	// Do not reassert it here: Stop() may have cleared it before this entry point.
	#endif

#ifdef _WIN32
	const unsigned current_thread_id = GetCurrentThreadId();
	Register_Thread_ID(current_thread_id, tc->ThreadName);

#if defined(_MSC_VER)
	// MSVC supports structured exception handling (__try/__except)
	if (tc->ExceptionHandler != nullptr) {
		__try {
			tc->Thread_Function();
		} __except(tc->ExceptionHandler(GetExceptionCode(), GetExceptionInformation())) {};
	} else {
		tc->Thread_Function();
	}
#elif defined(__GNUC__) && defined(_WIN32)
	// GCC/MinGW-w64 doesn't support MSVC's __try/__except syntax
	// Call Thread_Function directly without SEH support
	tc->Thread_Function();
#else
	#error "ThreadClass::Internal_Thread_Function: Unsupported compiler. This code requires MSVC or GCC/MinGW-w64 targeting Windows."
#endif

#else //_WIN32
	tc->Thread_Function();
#endif //_WIN32

#ifdef _WIN32
	Unregister_Thread_ID(current_thread_id, tc->ThreadName);
#endif // _WIN32
	#ifdef _WIN32
	InterlockedExchange(&tc->running, 0);
	#else
	tc->running = false;
	#endif
	#ifdef _WIN32
	return 0;
	#endif
}

void ThreadClass::Set_Test_Startup_Hook(StartupHookType hook)
{
	s_testStartupHook = hook;
}

void ThreadClass::Execute()
{
	#ifdef _UNIX
		// assert(0);
		return;
	#else
	if (handle)
	{
		const DWORD wait_result = WaitForSingleObject(reinterpret_cast<HANDLE>(handle), 0);
		if (wait_result == WAIT_OBJECT_0)
		{
			CloseHandle(reinterpret_cast<HANDLE>(handle));
			handle = 0;
			ThreadID = 0;
		}
		else
		{
			WWASSERT(wait_result != WAIT_FAILED);
			WWASSERT(!"ThreadClass::Execute called before the previous thread exited");
			return;
		}
	}
	WWASSERT(!handle);	// Only one thread at a time!
		WWASSERT(!running);
		InterlockedExchange(&running, 1);
		unsigned thread_id = 0;
		const size_t new_handle = _beginthreadex(0, 0,
			&Internal_Thread_Function, this, 0, &thread_id);
		if (new_handle == 0)
		{
			InterlockedExchange(&running, 0);
			WWDEBUG_SAY(("ThreadClass::Execute: Failed to start thread %s", ThreadName));
			return;
		}
		handle = new_handle;
		ThreadID = thread_id;
		::SetThreadPriority(reinterpret_cast<HANDLE>(handle),
			THREAD_PRIORITY_NORMAL+thread_priority);
		WWDEBUG_SAY(("ThreadClass::Execute: Started thread %s, thread ID is %X", ThreadName, ThreadID));
	#endif
}

void ThreadClass::Set_Priority(int priority)
{
	#ifdef _UNIX
		// assert(0);
		return;
	#else
		thread_priority=priority;
		if (handle) ::SetThreadPriority(reinterpret_cast<HANDLE>(handle),THREAD_PRIORITY_NORMAL+thread_priority);
	#endif
}

void ThreadClass::Stop(unsigned ms)
{
	#ifdef _UNIX
		// assert(0);
		return;
	#else
		if (!handle) return;
		InterlockedExchange(&running, 0);
		HANDLE native_handle = reinterpret_cast<HANDLE>(handle);
		DWORD wait_result = WaitForSingleObject(native_handle, ms);
		if (wait_result == WAIT_TIMEOUT) {
			int res=TerminateThread(native_handle,0);
			res;	// just to silence compiler warnings
			WWASSERT(res);	// Thread still not killed!
			wait_result = WaitForSingleObject(native_handle, INFINITE);
		}
		if (wait_result == WAIT_OBJECT_0) {
			CloseHandle(native_handle);
			handle=0;
			ThreadID = 0;
		}
	#endif
}

void ThreadClass::Sleep_Ms(unsigned ms)
{
	Sleep(ms);
}

#ifndef _UNIX
HANDLE test_event = ::CreateEvent (nullptr, FALSE, FALSE, "");
#endif

void ThreadClass::Switch_Thread()
{
	#ifdef _UNIX
		return;
	#else
		//	::SwitchToThread ();
		::WaitForSingleObject (test_event, 1);
		//	Sleep(1);	// Note! Parameter can not be 0 (or the thread switch doesn't occur)
	#endif
}

// Return calling thread's unique thread id
unsigned ThreadClass::_Get_Current_Thread_ID()
{
	#ifdef _UNIX
		return 0;
	#else
		return GetCurrentThreadId();
	#endif
}

bool ThreadClass::Is_Running()
{
	#ifdef _WIN32
	const HANDLE native_handle = reinterpret_cast<HANDLE>(handle);
	return native_handle != nullptr && WaitForSingleObject(native_handle, 0) != WAIT_OBJECT_0;
	#else
	return running != 0;
	#endif
}
