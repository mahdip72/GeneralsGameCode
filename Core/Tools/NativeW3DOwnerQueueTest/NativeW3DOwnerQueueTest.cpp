#include "Renderer/NativeW3DOwnerQueue.h"

#include <cstdio>
#include <windows.h>

namespace
{
int Check(bool condition, const char *message)
{
	if (condition)
	{
		return 0;
	}
	std::fprintf(stderr, "FAIL: %s\n", message);
	return 1;
}

struct TokenStats
{
	TokenStats() : callbackCount(0), releaseCount(0) {}

	int callbackCount;
	int releaseCount;
};

void Increment(void *context)
{
	TokenStats *stats = static_cast<TokenStats *>(context);
	++stats->callbackCount;
}

void ReleaseStats(void *context)
{
	TokenStats *stats = static_cast<TokenStats *>(context);
	++stats->releaseCount;
}

void ThrowingCommand(void *context)
{
	TokenStats *stats = static_cast<TokenStats *>(context);
	++stats->callbackCount;
	throw 1;
}

void ThrowingRelease(void *context)
{
	TokenStats *stats = static_cast<TokenStats *>(context);
	++stats->releaseCount;
	throw 1;
}

struct WrongOwnerDrain
{
	rts::render::NativeW3DOwnerQueue *queue;
	rts::render::RenderResult bindResult;
	rts::render::RenderResult result;
	unsigned int drained;
};

DWORD WINAPI DrainFromWrongOwner(void *parameter)
{
	WrongOwnerDrain *test = static_cast<WrongOwnerDrain *>(parameter);
	test->bindResult = test->queue->BindOwner();
	test->drained = 0;
	test->result = test->queue->Drain(0, &test->drained);
	return 0;
}
}

int main()
{
	int result = 0;
	rts::render::NativeW3DOwnerQueue queue(2);
	TokenStats stats;
	rts::render::NativeW3DOwnerToken *token =
		rts::render::NativeW3DOwnerToken::Create(&stats, ReleaseStats);
	result |= Check(token != 0,
		"owner token can be allocated");
	if (token == 0)
	{
		return result == 0 ? 1 : result;
	}

	result |= Check(queue.Capacity() == 2,
		"owner queue preserves its fixed capacity");
	result |= Check(!queue.IsBound() && !queue.IsOwnerThread(),
		"owner queue starts without an owner");
	result |= Check(queue.Enqueue(Increment, 0) ==
		rts::render::RENDER_RESULT_INVALID_ARGUMENT,
		"owner queue rejects a missing opaque token");
	result |= Check(queue.Enqueue(Increment, token) ==
		rts::render::RENDER_RESULT_INVALID_ARGUMENT,
		"owner queue rejects producers before owner binding");
	result |= Check(queue.BindOwner() == rts::render::RENDER_RESULT_OK,
		"owner queue binds the current thread");
	result |= Check(queue.BindOwner() == rts::render::RENDER_RESULT_OK,
		"owner binding is idempotent on the owner thread");
	result |= Check(queue.IsBound() && queue.IsOwnerThread(),
		"owner queue reports its bound owner");

	result |= Check(queue.Enqueue(Increment, token) ==
		rts::render::RENDER_RESULT_OK,
		"owner queue accepts a command after binding");
	result |= Check(queue.Enqueue(Increment, token) ==
		rts::render::RENDER_RESULT_OK,
		"owner queue accepts commands up to capacity");
	result |= Check(queue.Enqueue(Increment, token) ==
		rts::render::RENDER_RESULT_OUT_OF_MEMORY,
		"owner queue rejects commands beyond capacity");
	result |= Check(queue.Size() == 2,
		"owner queue reports the number of pending commands");

	WrongOwnerDrain wrongOwner;
	wrongOwner.queue = &queue;
	wrongOwner.bindResult = rts::render::RENDER_RESULT_OK;
	wrongOwner.result = rts::render::RENDER_RESULT_OK;
	wrongOwner.drained = 0;
	HANDLE thread = CreateThread(0, 0, DrainFromWrongOwner, &wrongOwner, 0, 0);
	result |= Check(thread != 0,
		"wrong-owner test thread starts");
	if (thread != 0)
	{
		WaitForSingleObject(thread, INFINITE);
		CloseHandle(thread);
		result |= Check(wrongOwner.bindResult ==
			rts::render::RENDER_RESULT_INVALID_ARGUMENT,
			"wrong owner cannot replace the bound owner");
		result |= Check(wrongOwner.result ==
			rts::render::RENDER_RESULT_INVALID_ARGUMENT,
			"wrong owner cannot drain the queue");
		result |= Check(wrongOwner.drained == 0,
			"wrong-owner rejection leaves commands pending");
	}

	unsigned int drained = 0;
	result |= Check(queue.Drain(1, &drained) == rts::render::RENDER_RESULT_OK &&
		drained == 1 && stats.callbackCount == 1 && stats.releaseCount == 0,
		"owner drains a bounded number of commands");
	result |= Check(queue.Size() == 1,
		"bounded drain leaves the remaining command queued");
	result |= Check(queue.Drain(0, &drained) == rts::render::RENDER_RESULT_OK &&
		drained == 1 && stats.callbackCount == 2 && stats.releaseCount == 0,
		"owner drains the remaining commands");
	result |= Check(queue.Size() == 0,
		"owner drain empties the queue");
	result |= Check(queue.Drain(0, &drained) == rts::render::RENDER_RESULT_OK &&
		drained == 0 && stats.callbackCount == 2,
		"repeated empty drains are deterministic and harmless");
	result |= Check(queue.Enqueue(Increment, token) ==
		rts::render::RENDER_RESULT_OK,
		"owner queue can be reused after a complete drain");
	result |= Check(queue.Drain(0, &drained) == rts::render::RENDER_RESULT_OK &&
		drained == 1 && stats.callbackCount == 3 && stats.releaseCount == 0,
		"reused owner queue drains a later command");

	// The caller may release its reference immediately after enqueue.  The
	// queue's reference keeps the opaque context alive until execution.
	token->Release();
	result |= Check(stats.releaseCount == 1,
		"final caller release runs the context release callback exactly once");

	TokenStats exceptionStats;
	{
		rts::render::NativeW3DOwnerQueue exceptionQueue(1);
		result |= Check(exceptionQueue.BindOwner() == rts::render::RENDER_RESULT_OK,
			"exception queue binds its owner");
		rts::render::NativeW3DOwnerToken *exceptionToken =
			rts::render::NativeW3DOwnerToken::Create(&exceptionStats,
				ReleaseStats);
		result |= Check(exceptionToken != 0,
			"exception command token can be allocated");
		if (exceptionToken != 0)
		{
			result |= Check(exceptionQueue.Enqueue(ThrowingCommand,
				exceptionToken) == rts::render::RENDER_RESULT_OK,
				"exception command can be queued");
			exceptionToken->Release();
			unsigned int exceptionDrained = 0;
			result |= Check(exceptionQueue.Drain(0, &exceptionDrained) ==
				rts::render::RENDER_RESULT_FAILED && exceptionDrained == 1 &&
				exceptionStats.callbackCount == 1 &&
				exceptionStats.releaseCount == 1,
				"callback exceptions still release the queue token");
		}
	}

	TokenStats discardedStats;
	{
		rts::render::NativeW3DOwnerQueue discardedQueue(1);
		result |= Check(discardedQueue.BindOwner() == rts::render::RENDER_RESULT_OK,
			"discard queue binds its owner");
		rts::render::NativeW3DOwnerToken *discardedToken =
			rts::render::NativeW3DOwnerToken::Create(&discardedStats,
				ReleaseStats);
		result |= Check(discardedToken != 0,
			"discard token can be allocated");
		if (discardedToken != 0)
		{
			result |= Check(discardedQueue.Enqueue(Increment,
				discardedToken) == rts::render::RENDER_RESULT_OK,
				"discard command can be queued");
			discardedToken->Release();
		}
	}
	result |= Check(discardedStats.callbackCount == 0 &&
		discardedStats.releaseCount == 1,
		"discarding pending entries releases but never executes their tokens");

	TokenStats explicitReferenceStats;
	rts::render::NativeW3DOwnerToken *explicitToken =
		rts::render::NativeW3DOwnerToken::Create(&explicitReferenceStats,
			ThrowingRelease);
	result |= Check(explicitToken != 0,
		"explicit reference token can be allocated");
	if (explicitToken != 0)
	{
		explicitToken->AddRef();
		explicitToken->Release();
		result |= Check(explicitReferenceStats.releaseCount == 0,
			"an additional token reference defers release");
		explicitToken->Release();
		result |= Check(explicitReferenceStats.releaseCount == 1,
			"last token reference invokes release even when it throws");
	}

	return result;
}
