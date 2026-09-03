/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2025 Electronic Arts Inc.
**
** This program is free software: you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation, either version 3 of the License, or
** (at your option) any later version.
*/

#include "Lib/MultiplayerSimulationPolicy.h"

namespace rts
{

namespace
{

bool HasOnlyKnownKernelBits(unsigned mask)
{
	return (mask & ~static_cast<unsigned>(
		MULTIPLAYER_SIMULATION_KERNEL_KNOWN_MASK)) == 0;
}

bool IsSingleKnownKernel(MultiplayerSimulationKernel kernel)
{
	const unsigned value = static_cast<unsigned>(kernel);
	return value != 0 && HasOnlyKnownKernelBits(value) &&
		(value & (value - 1)) == 0;
}

MultiplayerSimulationPolicyStatus ValidatePeerContract(
	const MultiplayerSimulationPeerPolicy &peer)
{
	if (peer.schema != MULTIPLAYER_SIMULATION_POLICY_SCHEMA)
		return MULTIPLAYER_SIMULATION_POLICY_SERIAL_UNSUPPORTED_SCHEMA;
	if (peer.engineEpoch != MULTIPLAYER_SIMULATION_ENGINE_EPOCH)
		return MULTIPLAYER_SIMULATION_POLICY_SERIAL_UNSUPPORTED_ENGINE_EPOCH;
	if (peer.determinismEpoch !=
		MULTIPLAYER_SIMULATION_DETERMINISM_EPOCH)
	{
		return MULTIPLAYER_SIMULATION_POLICY_SERIAL_UNSUPPORTED_EPOCH;
	}
	if (!HasOnlyKnownKernelBits(peer.provenKernelMask))
		return MULTIPLAYER_SIMULATION_POLICY_SERIAL_INVALID_KERNEL_PROOF;
	return MULTIPLAYER_SIMULATION_POLICY_READY;
}

MultiplayerSimulationPolicyStatus SelectPolicyFailure(
	MultiplayerSimulationPolicyStatus current,
	MultiplayerSimulationPolicyStatus candidate)
{
	if (candidate == MULTIPLAYER_SIMULATION_POLICY_READY)
		return current;
	if (current == MULTIPLAYER_SIMULATION_POLICY_READY || candidate < current)
		return candidate;
	return current;
}

}

MultiplayerSimulationPeerPolicy::MultiplayerSimulationPeerPolicy()
	: schema(0), engineEpoch(0), determinismEpoch(0), buildCompatibilityCrc(0),
	  contentCrc(0), mapCrc(0), provenKernelMask(0)
{
}

MultiplayerSimulationSessionPolicy::MultiplayerSimulationSessionPolicy()
	: status(MULTIPLAYER_SIMULATION_POLICY_SERIAL_INVALID_ARGUMENT),
	  engineEpoch(0), determinismEpoch(0), buildCompatibilityCrc(0), contentCrc(0),
	  mapCrc(0), participantCount(0), enabledKernelMask(0)
{
}

bool ResolveMultiplayerSimulationSessionPolicy(
	const MultiplayerSimulationPeerPolicy &localPeer,
	const MultiplayerSimulationPeerPolicy *remotePeers,
	unsigned remotePeerCount,
	unsigned requestedKernelMask,
	MultiplayerSimulationSessionPolicy &output)
{
	output = MultiplayerSimulationSessionPolicy();
	if (remotePeers == 0 || remotePeerCount == 0 ||
		remotePeerCount > MULTIPLAYER_SIMULATION_MAXIMUM_REMOTE_PEERS)
	{
		return false;
	}
	if (!HasOnlyKnownKernelBits(requestedKernelMask))
	{
		output.status =
			MULTIPLAYER_SIMULATION_POLICY_SERIAL_INVALID_KERNEL_PROOF;
		return false;
	}

	MultiplayerSimulationPolicyStatus failure =
		ValidatePeerContract(localPeer);
	if (failure == MULTIPLAYER_SIMULATION_POLICY_READY)
	{
		output.engineEpoch = localPeer.engineEpoch;
		output.determinismEpoch = localPeer.determinismEpoch;
		output.buildCompatibilityCrc = localPeer.buildCompatibilityCrc;
		output.contentCrc = localPeer.contentCrc;
		output.mapCrc = localPeer.mapCrc;
		output.participantCount = remotePeerCount + 1;
	}

	unsigned enabledKernelMask = localPeer.provenKernelMask &
		requestedKernelMask;
	unsigned peerIndex;
	for (peerIndex = 0; peerIndex < remotePeerCount; ++peerIndex)
	{
		const MultiplayerSimulationPeerPolicy &peer = remotePeers[peerIndex];
		failure = SelectPolicyFailure(failure,
			ValidatePeerContract(peer));
		if (peer.engineEpoch != localPeer.engineEpoch)
			failure = SelectPolicyFailure(failure,
				MULTIPLAYER_SIMULATION_POLICY_SERIAL_UNSUPPORTED_ENGINE_EPOCH);
		if (peer.buildCompatibilityCrc != localPeer.buildCompatibilityCrc)
			failure = SelectPolicyFailure(failure,
				MULTIPLAYER_SIMULATION_POLICY_SERIAL_BUILD_MISMATCH);
		if (peer.contentCrc != localPeer.contentCrc ||
			peer.mapCrc != localPeer.mapCrc)
			failure = SelectPolicyFailure(failure,
				MULTIPLAYER_SIMULATION_POLICY_SERIAL_CONTENT_MISMATCH);
		enabledKernelMask &= peer.provenKernelMask;
	}
	if (failure != MULTIPLAYER_SIMULATION_POLICY_READY)
	{
		output.status = failure;
		return false;
	}

	output.status = MULTIPLAYER_SIMULATION_POLICY_READY;
	output.enabledKernelMask = enabledKernelMask;
	return true;
}

bool IsMultiplayerSimulationKernelEnabled(
	const MultiplayerSimulationSessionPolicy &policy,
	MultiplayerSimulationKernel kernel)
{
	if (policy.status != MULTIPLAYER_SIMULATION_POLICY_READY ||
		!IsSingleKnownKernel(kernel))
	{
		return false;
	}
	const unsigned kernelBit = static_cast<unsigned>(kernel);
	return (policy.enabledKernelMask & kernelBit) == kernelBit;
}

bool ShouldRunMultiplayerSimulationKernelOnWorkers(
	const MultiplayerSimulationSessionPolicy &policy,
	MultiplayerSimulationKernel kernel,
	SimulationExecutionMode executionMode,
	bool jobSystemReady,
	unsigned workerCount)
{
	return executionMode == SIMULATION_EXECUTION_PARALLEL &&
		jobSystemReady && workerCount > 1 && workerCount != ~0u &&
		IsMultiplayerSimulationKernelEnabled(policy, kernel);
}

bool IsMultiplayerSimulationWorkerBoundary(
	MultiplayerSimulationBoundary boundary)
{
	return boundary == MULTIPLAYER_SIMULATION_BOUNDARY_KERNEL_PREPARE;
}

} // namespace rts
