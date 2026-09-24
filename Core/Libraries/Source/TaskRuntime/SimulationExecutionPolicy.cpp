#include "Lib/SimulationExecutionPolicy.h"

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
const unsigned MODE_MASK = 3;
const unsigned LOCKED_FLAG = 4;

#if defined(_MSC_VER) && _MSC_VER < 1300
volatile LONG s_policy = SIMULATION_EXECUTION_SERIAL;

unsigned LoadPolicy()
{
	return static_cast<unsigned>(InterlockedCompareExchange(
		const_cast<LONG *>(&s_policy), 0, 0));
}

bool CompareExchangePolicy(unsigned &expected, unsigned desired)
{
	const unsigned previous = static_cast<unsigned>(InterlockedCompareExchange(
		const_cast<LONG *>(&s_policy), static_cast<LONG>(desired),
		static_cast<LONG>(expected)));
	if (previous == expected) return true;
	expected = previous;
	return false;
}
#else
std::atomic<unsigned> s_policy(SIMULATION_EXECUTION_SERIAL);

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
		if (value >= 'A' && value <= 'Z')
			value = static_cast<char>(value - 'A' + 'a');
		if (value != *right++) return false;
	}
	return *left == 0 && *right == 0;
}
}

SimulationExecutionMode GetSimulationExecutionMode()
{
	return static_cast<SimulationExecutionMode>(LoadPolicy() & MODE_MASK);
}

bool SetSimulationExecutionMode(SimulationExecutionMode mode)
{
	if (mode != SIMULATION_EXECUTION_SERIAL &&
		mode != SIMULATION_EXECUTION_PARALLEL &&
		mode != SIMULATION_EXECUTION_SHADOW)
	{
		return false;
	}

	unsigned expected = LoadPolicy();
	while ((expected & LOCKED_FLAG) == 0)
	{
		if (CompareExchangePolicy(expected, static_cast<unsigned>(mode)))
			return true;
	}
	return false;
}

bool SetSimulationExecutionMode(const char *mode)
{
	if (EqualsAsciiNoCase(mode, "serial"))
		return SetSimulationExecutionMode(SIMULATION_EXECUTION_SERIAL);
	if (EqualsAsciiNoCase(mode, "parallel"))
		return SetSimulationExecutionMode(SIMULATION_EXECUTION_PARALLEL);
	if (EqualsAsciiNoCase(mode, "shadow"))
		return SetSimulationExecutionMode(SIMULATION_EXECUTION_SHADOW);
	return false;
}

bool UseParallelSimulation()
{
	return GetSimulationExecutionMode() == SIMULATION_EXECUTION_PARALLEL;
}

bool UseSimulationShadowOracle()
{
	return GetSimulationExecutionMode() == SIMULATION_EXECUTION_SHADOW;
}

bool PrepareSimulationCommandsOffThread()
{
	return GetSimulationExecutionMode() != SIMULATION_EXECUTION_SERIAL;
}

void LockSimulationExecutionMode()
{
	unsigned expected = LoadPolicy();
	while ((expected & LOCKED_FLAG) == 0 &&
		!CompareExchangePolicy(expected, expected | LOCKED_FLAG)) {}
}

bool IsSimulationExecutionModeLocked()
{
	return (LoadPolicy() & LOCKED_FLAG) != 0;
}

SimulationExecutionStartupResult PrepareSimulationExecutionStartup(
	SimulationExecutionEnsureStartedFunction ensureStarted, void *context)
{
	if (ensureStarted == 0)
	{
		return SIMULATION_EXECUTION_STARTUP_POLICY_FAILURE;
	}

	if (ensureStarted(context))
	{
		LockSimulationExecutionMode();
		return SIMULATION_EXECUTION_STARTUP_READY;
	}

	if (!SetSimulationExecutionMode(SIMULATION_EXECUTION_SERIAL) ||
		GetSimulationExecutionMode() != SIMULATION_EXECUTION_SERIAL)
	{
		return SIMULATION_EXECUTION_STARTUP_POLICY_FAILURE;
	}
	LockSimulationExecutionMode();
	return SIMULATION_EXECUTION_STARTUP_SERIAL_FALLBACK;
}
}
