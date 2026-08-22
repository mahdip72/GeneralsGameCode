#include "Renderer/NativeW3DRenderState.h"

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

struct ReleaseStats
{
	ReleaseStats() : commandCount(0), releaseCount(0) {}
	int commandCount;
	int releaseCount;
};

void RunCommand(void *context)
{
	++static_cast<ReleaseStats *>(context)->commandCount;
}

void ReleaseCommand(void *context)
{
	++static_cast<ReleaseStats *>(context)->releaseCount;
}

struct WrongOwnerRequest
{
	rts::render::NativeW3DRenderState *state;
	rts::render::RenderResult drainResult;
	unsigned int drained;
};

DWORD WINAPI DrainFromWrongOwner(void *parameter)
{
	WrongOwnerRequest *request = static_cast<WrongOwnerRequest *>(parameter);
	request->drained = 0;
	request->drainResult = request->state->DrainCleanup(0, &request->drained);
	return 0;
}
}

int main()
{
	int result = 0;
	rts::render::NativeW3DRenderState *state =
		rts::render::NativeW3DRenderState::Create(2);
	result |= Check(state != 0, "render state can be allocated");
	if (state == 0)
	{
		return result == 0 ? 1 : result;
	}
	result |= Check(state->BindOwner() == rts::render::RENDER_RESULT_OK &&
		state->IsOwnerThread() && state->IsAcceptingCleanup(),
		"render state binds one cleanup owner before accepting producer work");

	ReleaseStats stats;
	rts::render::NativeW3DOwnerToken *token =
		rts::render::NativeW3DOwnerToken::Create(&stats, ReleaseCommand);
	result |= Check(token != 0, "cleanup payload receives an opaque lifetime token");
	if (token != 0)
	{
		result |= Check(state->EnqueueCleanup(RunCommand, token) ==
			rts::render::RENDER_RESULT_OK,
			"render state accepts cleanup while operational");
		token->Release();
	}

	WrongOwnerRequest wrongOwner;
	wrongOwner.state = state;
	wrongOwner.drainResult = rts::render::RENDER_RESULT_OK;
	wrongOwner.drained = 0;
	HANDLE thread = CreateThread(0, 0, DrainFromWrongOwner, &wrongOwner, 0, 0);
	result |= Check(thread != 0, "wrong-owner cleanup request starts");
	if (thread != 0)
	{
		WaitForSingleObject(thread, INFINITE);
		CloseHandle(thread);
		result |= Check(wrongOwner.drainResult == rts::render::RENDER_RESULT_INVALID_ARGUMENT &&
			wrongOwner.drained == 0,
			"only the state owner may drain backend cleanup");
	}

	unsigned int drained = 0;
	result |= Check(state->BeginShutdown() == rts::render::RENDER_RESULT_OK &&
		!state->IsAcceptingCleanup(),
		"shutdown closes cleanup admission before backend destruction");
	result |= Check(state->DrainCleanup(0, &drained) == rts::render::RENDER_RESULT_OK &&
		drained == 1 && stats.commandCount == 1 && stats.releaseCount == 1,
		"owner drains accepted cleanup after shutdown admission closes");
	rts::render::NativeW3DOwnerToken *lateToken =
		rts::render::NativeW3DOwnerToken::Create(&stats, ReleaseCommand);
	result |= Check(lateToken != 0, "late cleanup payload can be allocated");
	if (lateToken != 0)
	{
		result |= Check(state->EnqueueCleanup(RunCommand, lateToken) ==
			rts::render::RENDER_RESULT_INVALID_ARGUMENT,
			"a closed state rejects late cleanup without touching backend lifetime");
		lateToken->Release();
	}
	state->Release();
	return result;
}
