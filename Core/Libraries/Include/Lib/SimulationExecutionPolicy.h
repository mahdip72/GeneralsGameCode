#pragma once

namespace rts
{
enum SimulationExecutionMode
{
	SIMULATION_EXECUTION_SERIAL = 0,
	SIMULATION_EXECUTION_PARALLEL = 1,
	SIMULATION_EXECUTION_SHADOW = 2
};

typedef bool (*SimulationExecutionEnsureStartedFunction)(void *context);

enum SimulationExecutionStartupResult
{
	SIMULATION_EXECUTION_STARTUP_READY = 0,
	SIMULATION_EXECUTION_STARTUP_SERIAL_FALLBACK,
	SIMULATION_EXECUTION_STARTUP_POLICY_FAILURE
};

// Process-wide startup policy. Serial remains the safe default until a title
// explicitly selects and locks a different mode before simulation starts.
SimulationExecutionMode GetSimulationExecutionMode();
bool SetSimulationExecutionMode(SimulationExecutionMode mode);
bool SetSimulationExecutionMode(const char *mode);
bool UseParallelSimulation();
bool UseSimulationShadowOracle();
bool PrepareSimulationCommandsOffThread();
void LockSimulationExecutionMode();
bool IsSimulationExecutionModeLocked();

// Starts the scheduler before locking the selected mode.  A failed start is
// downgraded to a verified serial policy before that policy is locked.  Owner
// registration remains with the caller so it can occur after a successful
// return and lock.
SimulationExecutionStartupResult PrepareSimulationExecutionStartup(
	SimulationExecutionEnsureStartedFunction ensureStarted, void *context);
}
