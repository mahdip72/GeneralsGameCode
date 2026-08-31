#include "Lib/PipelineExecutionPolicy.h"

#if defined(_MSC_VER) && _MSC_VER < 1300
#include <windows.h>
#include "Utility/interlocked_adapter.h"
#else
#include <atomic>
#endif

namespace rts
{
namespace
{
const unsigned MODE_MASK = 1;
const unsigned LOCKED_FLAG = 2;

#if defined(_MSC_VER) && _MSC_VER < 1300
volatile LONG s_policy = PIPELINE_EXECUTION_PARALLEL;

unsigned LoadPolicy()
{
	return static_cast<unsigned>(InterlockedCompareExchange(
		const_cast<LONG *>(&s_policy), 0, 0));
}

bool CompareExchangePolicy(unsigned &expected, unsigned desired)
{
	const unsigned previous = static_cast<unsigned>(InterlockedCompareExchange(
		const_cast<LONG *>(&s_policy), static_cast<LONG>(desired), static_cast<LONG>(expected)));
	if (previous == expected) return true;
	expected = previous;
	return false;
}
#else
std::atomic<unsigned> s_policy(PIPELINE_EXECUTION_PARALLEL);

unsigned LoadPolicy()
{
	return s_policy.load(std::memory_order_acquire);
}

bool CompareExchangePolicy(unsigned &expected, unsigned desired)
{
	return s_policy.compare_exchange_weak(expected, desired,
		std::memory_order_acq_rel, std::memory_order_acquire);
}
#endif

bool EqualsAsciiNoCase(const char *left, const char *right)
{
	if (left == 0 || right == 0) return false;
	while (*left != 0 && *right != 0)
	{
		char value = *left++;
		if (value >= 'A' && value <= 'Z') value = static_cast<char>(value - 'A' + 'a');
		if (value != *right++) return false;
	}
	return *left == 0 && *right == 0;
}
}

PipelineExecutionMode GetPipelineExecutionMode()
{
	return static_cast<PipelineExecutionMode>(LoadPolicy() & MODE_MASK);
}

bool SetPipelineExecutionMode(PipelineExecutionMode mode)
{
	if (mode != PIPELINE_EXECUTION_PARALLEL && mode != PIPELINE_EXECUTION_SERIAL) return false;
	unsigned expected = LoadPolicy();
	while ((expected & LOCKED_FLAG) == 0)
	{
		if (CompareExchangePolicy(expected, static_cast<unsigned>(mode))) return true;
	}
	return false;
}

bool SetPipelineExecutionMode(const char *mode)
{
	if (EqualsAsciiNoCase(mode, "parallel")) return SetPipelineExecutionMode(PIPELINE_EXECUTION_PARALLEL);
	if (EqualsAsciiNoCase(mode, "serial")) return SetPipelineExecutionMode(PIPELINE_EXECUTION_SERIAL);
	return false;
}

bool UseParallelPipelines()
{
	return GetPipelineExecutionMode() == PIPELINE_EXECUTION_PARALLEL;
}

void LockPipelineExecutionMode()
{
	unsigned expected = LoadPolicy();
	while ((expected & LOCKED_FLAG) == 0 &&
		!CompareExchangePolicy(expected, expected | LOCKED_FLAG)) {}
}

bool IsPipelineExecutionModeLocked()
{
	return (LoadPolicy() & LOCKED_FLAG) != 0;
}
}
