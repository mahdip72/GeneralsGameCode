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

void Increment(void *context)
{
	int *value = static_cast<int *>(context);
	++*value;
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
	int callbackCount = 0;

	result |= Check(queue.Capacity() == 2,
		"owner queue preserves its fixed capacity");
	result |= Check(!queue.IsBound() && !queue.IsOwnerThread(),
		"owner queue starts without an owner");
	result |= Check(queue.Enqueue(Increment, &callbackCount) ==
		rts::render::RENDER_RESULT_INVALID_ARGUMENT,
		"owner queue rejects producers before owner binding");
	result |= Check(queue.BindOwner() == rts::render::RENDER_RESULT_OK,
		"owner queue binds the current thread");
	result |= Check(queue.BindOwner() == rts::render::RENDER_RESULT_OK,
		"owner binding is idempotent on the owner thread");
	result |= Check(queue.IsBound() && queue.IsOwnerThread(),
		"owner queue reports its bound owner");

	result |= Check(queue.Enqueue(Increment, &callbackCount) ==
		rts::render::RENDER_RESULT_OK,
		"owner queue accepts a command after binding");
	result |= Check(queue.Enqueue(Increment, &callbackCount) ==
		rts::render::RENDER_RESULT_OK,
		"owner queue accepts commands up to capacity");
	result |= Check(queue.Enqueue(Increment, &callbackCount) ==
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
		drained == 1 && callbackCount == 1,
		"owner drains a bounded number of commands");
	result |= Check(queue.Size() == 1,
		"bounded drain leaves the remaining command queued");
	result |= Check(queue.Drain(0, &drained) == rts::render::RENDER_RESULT_OK &&
		drained == 1 && callbackCount == 2,
		"owner drains the remaining commands");
	result |= Check(queue.Size() == 0,
		"owner drain empties the queue");
	result |= Check(queue.Drain(0, &drained) == rts::render::RENDER_RESULT_OK &&
		drained == 0 && callbackCount == 2,
		"repeated empty drains are deterministic and harmless");
	result |= Check(queue.Enqueue(Increment, &callbackCount) ==
		rts::render::RENDER_RESULT_OK,
		"owner queue can be reused after a complete drain");
	result |= Check(queue.Drain(0, &drained) == rts::render::RENDER_RESULT_OK &&
		drained == 1 && callbackCount == 3,
		"reused owner queue drains a later command");

	return result;
}
