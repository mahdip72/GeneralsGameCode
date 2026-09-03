#pragma once

#include "GameNetwork/NetworkInterface.h"
#include "Lib/SimulationExecutionPolicy.h"

namespace rts
{

// Non-network games retain the existing serial/shadow/parallel product policy.
// A live lockstep session may prepare one of the proven pointer-free kernels
// only after NET3 has persisted an exact-roster READY policy containing that
// bit. Network shadow never becomes authoritative worker permission.
inline Bool ShouldPrepareLiveSimulationKernelOffThread(
	MultiplayerSimulationKernel kernel)
{
	if (!PrepareSimulationCommandsOffThread())
		return FALSE;
	if (TheNetwork == nullptr)
		return TRUE;
	return GetSimulationExecutionMode() == SIMULATION_EXECUTION_PARALLEL &&
		TheNetwork->isMultiplayerSimulationKernelEnabled(kernel);
}

inline Bool IsLiveMultiplayerSimulationKernelEnabled(
	MultiplayerSimulationKernel kernel)
{
	return TheNetwork != nullptr &&
		TheNetwork->isMultiplayerSimulationKernelEnabled(kernel);
}

} // namespace rts
