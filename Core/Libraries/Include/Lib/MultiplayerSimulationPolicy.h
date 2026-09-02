/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2025 Electronic Arts Inc.
**
** This program is free software: you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation, either version 3 of the License, or
** (at your option) any later version.
*/

#pragma once

#include "Lib/SimulationExecutionPolicy.h"

namespace rts
{

enum
{
	MULTIPLAYER_SIMULATION_POLICY_SCHEMA = 1,
	MULTIPLAYER_SIMULATION_ENGINE_EPOCH = 1,
	MULTIPLAYER_SIMULATION_DETERMINISM_EPOCH = 1,
	MULTIPLAYER_SIMULATION_MAXIMUM_REMOTE_PEERS = 7
};

enum MultiplayerSimulationKernel
{
	MULTIPLAYER_SIMULATION_KERNEL_NONE = 0,
	MULTIPLAYER_SIMULATION_KERNEL_PHYSICS = 1 << 0,
	MULTIPLAYER_SIMULATION_KERNEL_STATUS = 1 << 1,
	MULTIPLAYER_SIMULATION_KERNEL_COLLISION = 1 << 2,
	MULTIPLAYER_SIMULATION_KERNEL_AI_PLANNING = 1 << 3,
	MULTIPLAYER_SIMULATION_KERNEL_SPATIAL = 1 << 4,
	MULTIPLAYER_SIMULATION_KERNEL_PATH = 1 << 5,
	MULTIPLAYER_SIMULATION_KERNEL_KNOWN_MASK =
		MULTIPLAYER_SIMULATION_KERNEL_PHYSICS |
		MULTIPLAYER_SIMULATION_KERNEL_STATUS |
		MULTIPLAYER_SIMULATION_KERNEL_COLLISION |
		MULTIPLAYER_SIMULATION_KERNEL_AI_PLANNING |
		MULTIPLAYER_SIMULATION_KERNEL_SPATIAL |
		MULTIPLAYER_SIMULATION_KERNEL_PATH,
	// A live integration is only a diagnostic candidate. Product peers advertise
	// none of these bits: the InstalledNet3Validation v1 evidence and its
	// external proof are deliberately unable to authorize live lockstep work.
	// A separately reviewed lockstep-v2 contract is required before promotion.
	MULTIPLAYER_SIMULATION_KERNEL_LIVE_INTEGRATED_MASK =
		MULTIPLAYER_SIMULATION_KERNEL_KNOWN_MASK,
	MULTIPLAYER_SIMULATION_KERNEL_RELEASE_PROVEN_DEFAULT_MASK =
		MULTIPLAYER_SIMULATION_KERNEL_NONE
};

enum
{
	MULTIPLAYER_SIMULATION_RELEASE_PROOF_SCHEMA = 1,
	MULTIPLAYER_SIMULATION_SOURCE_REVISION_HEX_LENGTH = 40,
	MULTIPLAYER_SIMULATION_EVIDENCE_SHA256_HEX_LENGTH = 64
};

// Legacy-compatible policy fixture retained for device-free contract tests.
// Product startup never loads this object or any generated source header; it
// resolves authority only from the exact external installed-runtime bundle in
// MultiplayerSimulationRuntimeProof.h. Missing or malformed fixture data still
// resolves to zero.
struct MultiplayerSimulationGeneratedReleaseProof
{
	unsigned schema;
	const char *sourceRevision;
	const char *generalsExecutableSha256;
	const char *zeroHourExecutableSha256;
	const char *artifactSetSha256;
	const char *evidenceManifestSha256;
	unsigned provenKernelMask;
};

inline bool IsCanonicalMultiplayerSimulationHexDigest(
	const char *value, unsigned expectedLength)
{
	if (value == 0 || expectedLength == 0)
		return false;
	bool hasNonZeroDigit = false;
	unsigned index;
	for (index = 0; index < expectedLength; ++index)
	{
		const char digit = value[index];
		if (!((digit >= '0' && digit <= '9') ||
			(digit >= 'a' && digit <= 'f')))
		{
			return false;
		}
		if (digit != '0')
			hasNonZeroDigit = true;
	}
	return hasNonZeroDigit && value[expectedLength] == '\0';
}

inline bool AreExactMultiplayerSimulationHexDigestsEqual(
	const char *left, const char *right, unsigned expectedLength)
{
	if (!IsCanonicalMultiplayerSimulationHexDigest(left, expectedLength) ||
		!IsCanonicalMultiplayerSimulationHexDigest(right, expectedLength))
	{
		return false;
	}
	unsigned index;
	for (index = 0; index < expectedLength; ++index)
	{
		if (left[index] != right[index])
			return false;
	}
	return true;
}

inline unsigned ResolveMultiplayerSimulationGeneratedReleaseProofMask(
	const MultiplayerSimulationGeneratedReleaseProof &proof,
	unsigned liveIntegratedKernelMask)
{
	const unsigned knownMask = static_cast<unsigned>(
		MULTIPLAYER_SIMULATION_KERNEL_KNOWN_MASK);
	if (proof.schema != MULTIPLAYER_SIMULATION_RELEASE_PROOF_SCHEMA ||
		liveIntegratedKernelMask == 0 ||
		(liveIntegratedKernelMask & ~knownMask) != 0 ||
		!IsCanonicalMultiplayerSimulationHexDigest(proof.sourceRevision,
			MULTIPLAYER_SIMULATION_SOURCE_REVISION_HEX_LENGTH) ||
		!IsCanonicalMultiplayerSimulationHexDigest(
			proof.generalsExecutableSha256,
			MULTIPLAYER_SIMULATION_EVIDENCE_SHA256_HEX_LENGTH) ||
		!IsCanonicalMultiplayerSimulationHexDigest(
			proof.zeroHourExecutableSha256,
			MULTIPLAYER_SIMULATION_EVIDENCE_SHA256_HEX_LENGTH) ||
		!IsCanonicalMultiplayerSimulationHexDigest(proof.artifactSetSha256,
			MULTIPLAYER_SIMULATION_EVIDENCE_SHA256_HEX_LENGTH) ||
		!IsCanonicalMultiplayerSimulationHexDigest(
			proof.evidenceManifestSha256,
			MULTIPLAYER_SIMULATION_EVIDENCE_SHA256_HEX_LENGTH) ||
		proof.provenKernelMask == 0 ||
		(proof.provenKernelMask & ~liveIntegratedKernelMask) != 0)
	{
		return MULTIPLAYER_SIMULATION_KERNEL_RELEASE_PROVEN_DEFAULT_MASK;
	}
	return proof.provenKernelMask;
}

// Focused policy/transport tests may exercise a positive negotiation without
// forging product evidence. Product code must use the external runtime-proof
// resolver and must not call either test-only override helper.
inline unsigned SelectMultiplayerSimulationNonProductTestOverrideMask(
	unsigned liveIntegratedKernelMask, unsigned explicitOverrideMask)
{
	const unsigned knownMask = static_cast<unsigned>(
		MULTIPLAYER_SIMULATION_KERNEL_KNOWN_MASK);
	if (liveIntegratedKernelMask == 0 || explicitOverrideMask == 0 ||
		(liveIntegratedKernelMask & ~knownMask) != 0 ||
		(explicitOverrideMask & ~liveIntegratedKernelMask) != 0)
	{
		return MULTIPLAYER_SIMULATION_KERNEL_RELEASE_PROVEN_DEFAULT_MASK;
	}
	return explicitOverrideMask;
}

enum MultiplayerSimulationPolicyStatus
{
	MULTIPLAYER_SIMULATION_POLICY_READY = 0,
	MULTIPLAYER_SIMULATION_POLICY_SERIAL_INVALID_ARGUMENT,
	MULTIPLAYER_SIMULATION_POLICY_SERIAL_UNSUPPORTED_SCHEMA,
	MULTIPLAYER_SIMULATION_POLICY_SERIAL_UNSUPPORTED_ENGINE_EPOCH,
	MULTIPLAYER_SIMULATION_POLICY_SERIAL_UNSUPPORTED_EPOCH,
	MULTIPLAYER_SIMULATION_POLICY_SERIAL_BUILD_MISMATCH,
	MULTIPLAYER_SIMULATION_POLICY_SERIAL_CONTENT_MISMATCH,
	MULTIPLAYER_SIMULATION_POLICY_SERIAL_INVALID_KERNEL_PROOF,
	MULTIPLAYER_SIMULATION_POLICY_SERIAL_NETWORK_UNAVAILABLE
};

inline bool IsMultiplayerSimulationPolicyLifecycleUsable(
	bool policyResolved,
	bool policyReady,
	bool networkHelloReady,
	bool networkHelloFailed,
	unsigned rosterMask,
	unsigned expectedRemoteMask,
	unsigned presentRemoteMask,
	unsigned quittingRemoteMask,
	unsigned localSlot,
	unsigned maximumSlots)
{
	if (!policyResolved || !policyReady || !networkHelloReady ||
		networkHelloFailed ||
		maximumSlots == 0 || maximumSlots > 32 || localSlot >= maximumSlots)
	{
		return false;
	}
	const unsigned validSlotMask = maximumSlots == 32 ? ~0u :
		((1u << maximumSlots) - 1u);
	const unsigned localSlotMask = 1u << localSlot;
	if ((expectedRemoteMask & ~validSlotMask) != 0 ||
		(expectedRemoteMask & localSlotMask) != 0 ||
		rosterMask != (expectedRemoteMask | localSlotMask))
	{
		return false;
	}
	return presentRemoteMask == expectedRemoteMask &&
		quittingRemoteMask == 0;
}

// Worker count is intentionally absent. It is a local scheduling choice, not
// part of the lockstep identity. Every participant must still advertise the
// exact deterministic epoch and content identity before any kernel is enabled.
struct MultiplayerSimulationPeerPolicy
{
	MultiplayerSimulationPeerPolicy();

	unsigned schema;
	unsigned engineEpoch;
	unsigned determinismEpoch;
	unsigned buildCompatibilityCrc;
	unsigned contentCrc;
	unsigned mapCrc;
	unsigned provenKernelMask;
};

// A rejected policy is a fully initialized serial policy. When the local peer
// contract itself is valid, its identity remains available for owner-side
// diagnostics/publication even if a remote peer rejects worker execution.
// Missing proof for a known kernel removes only that kernel from the enabled
// intersection; an unknown proof bit rejects the complete policy as an
// incompatible contract.
struct MultiplayerSimulationSessionPolicy
{
	MultiplayerSimulationSessionPolicy();

	MultiplayerSimulationPolicyStatus status;
	unsigned engineEpoch;
	unsigned determinismEpoch;
	unsigned buildCompatibilityCrc;
	unsigned contentCrc;
	unsigned mapCrc;
	unsigned participantCount;
	unsigned enabledKernelMask;
};

// remotePeers excludes localPeer. Resolution is order independent. A false
// return always leaves output in a serial, query-safe state with a precise
// rejection status.
bool ResolveMultiplayerSimulationSessionPolicy(
	const MultiplayerSimulationPeerPolicy &localPeer,
	const MultiplayerSimulationPeerPolicy *remotePeers,
	unsigned remotePeerCount,
	unsigned requestedKernelMask,
	MultiplayerSimulationSessionPolicy &output);

bool IsMultiplayerSimulationKernelEnabled(
	const MultiplayerSimulationSessionPolicy &policy,
	MultiplayerSimulationKernel kernel);

// Only the explicit parallel product policy may publish a proven worker
// result. Shadow, unavailable scheduler, one-worker, rejected epoch/content,
// and missing-proof paths all remain serial.
bool ShouldRunMultiplayerSimulationKernelOnWorkers(
	const MultiplayerSimulationSessionPolicy &policy,
	MultiplayerSimulationKernel kernel,
	SimulationExecutionMode executionMode,
	bool jobSystemReady,
	unsigned workerCount);

enum MultiplayerSimulationBoundary
{
	MULTIPLAYER_SIMULATION_BOUNDARY_OWNER_INTAKE = 0,
	MULTIPLAYER_SIMULATION_BOUNDARY_KERNEL_PREPARE,
	MULTIPLAYER_SIMULATION_BOUNDARY_OWNER_KERNEL_COMMIT,
	MULTIPLAYER_SIMULATION_BOUNDARY_OWNER_NETWORK_COMMIT,
	MULTIPLAYER_SIMULATION_BOUNDARY_OWNER_RECORDER_PUBLICATION
};

// This is deliberately true for one boundary only. Intake, all live mutation,
// network command commit, and recorder publication remain owner operations.
bool IsMultiplayerSimulationWorkerBoundary(
	MultiplayerSimulationBoundary boundary);

} // namespace rts
