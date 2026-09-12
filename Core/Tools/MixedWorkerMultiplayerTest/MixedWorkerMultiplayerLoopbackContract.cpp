#include "MixedWorkerMultiplayerLoopbackContract.h"

namespace rts
{

namespace
{

void SetExplicit(MixedWorkerLoopbackPeerConfiguration &peer,
	unsigned workerCount)
{
	peer.workerPolicy = MIXED_WORKER_LOOPBACK_EXPLICIT;
	peer.workerCount = workerCount;
}

void SetAuto(MixedWorkerLoopbackPeerConfiguration &peer)
{
	peer.workerPolicy = MIXED_WORKER_LOOPBACK_AUTO;
	peer.workerCount = 0;
}

const MultiplayerSimulationKernel LOOPBACK_KERNELS[
	MIXED_WORKER_LOOPBACK_KERNEL_COUNT] =
{
	MULTIPLAYER_SIMULATION_KERNEL_PHYSICS,
	MULTIPLAYER_SIMULATION_KERNEL_STATUS,
	MULTIPLAYER_SIMULATION_KERNEL_COLLISION,
	MULTIPLAYER_SIMULATION_KERNEL_AI_PLANNING,
	MULTIPLAYER_SIMULATION_KERNEL_SPATIAL,
	MULTIPLAYER_SIMULATION_KERNEL_PATH
};

unsigned CountBits(unsigned value)
{
	unsigned count = 0;
	while (value != 0)
	{
		value &= value - 1;
		++count;
	}
	return count;
}

bool IsKernelEvidenceZero(const MixedWorkerLoopbackPeerEvidence &record,
	unsigned kernelIndex)
{
	return record.kernelSubmittedJobCount[kernelIndex] == 0 &&
		record.kernelCompletedJobCount[kernelIndex] == 0 &&
		record.kernelPhysicalJobCount[kernelIndex] == 0 &&
		record.kernelPhysicalWorkerMask[kernelIndex] == 0 &&
		record.kernelDistinctPhysicalWorkerCount[kernelIndex] == 0;
}

bool IsValidPhysicalEvidence(const MixedWorkerLoopbackPeerEvidence &record,
	unsigned expectedKernelMask)
{
	unsigned expectedPhysicalKernelMask = 0;
	unsigned kernelIndex;
	for (kernelIndex = 0;
		kernelIndex < MIXED_WORKER_LOOPBACK_KERNEL_COUNT; ++kernelIndex)
	{
		const unsigned kernelBit = static_cast<unsigned>(
			LOOPBACK_KERNELS[kernelIndex]);
		const bool enabled = (expectedKernelMask & kernelBit) != 0;
		if (record.effectiveWorkerCount <= 1 || !enabled)
		{
			if (!IsKernelEvidenceZero(record, kernelIndex))
				return false;
			continue;
		}

		expectedPhysicalKernelMask |= kernelBit;
		const unsigned submitted =
			record.kernelSubmittedJobCount[kernelIndex];
		const unsigned completed =
			record.kernelCompletedJobCount[kernelIndex];
		const unsigned physical =
			record.kernelPhysicalJobCount[kernelIndex];
		const unsigned workerMask =
			record.kernelPhysicalWorkerMask[kernelIndex];
		const unsigned distinctWorkers =
			record.kernelDistinctPhysicalWorkerCount[kernelIndex];
		if (submitted == 0 || completed != submitted ||
			physical != submitted || distinctWorkers <= 1 ||
			distinctWorkers > record.effectiveWorkerCount ||
			CountBits(workerMask) != distinctWorkers)
		{
			return false;
		}
		if (record.effectiveWorkerCount <
			static_cast<unsigned>(sizeof(unsigned) * 8))
		{
			const unsigned validWorkerMask =
				(1u << record.effectiveWorkerCount) - 1u;
			if ((workerMask & ~validWorkerMask) != 0)
				return false;
		}
	}
	return record.physicalKernelMask == expectedPhysicalKernelMask;
}

}

unsigned GetMixedWorkerLoopbackSeed(unsigned seedIndex)
{
	const unsigned seeds[MIXED_WORKER_LOOPBACK_SEED_COUNT] =
	{
		0x00005a17u,
		0x0000c0deu
	};
	return seedIndex < MIXED_WORKER_LOOPBACK_SEED_COUNT ?
		seeds[seedIndex] : 0;
}

bool GetMixedWorkerLoopbackCase(unsigned caseIndex,
	MixedWorkerLoopbackCase &output)
{
	output.name = 0;
	output.peerCount = 0;
	unsigned peerIndex;
	for (peerIndex = 0;
		peerIndex < MIXED_WORKER_LOOPBACK_MAXIMUM_PEERS; ++peerIndex)
	{
		SetExplicit(output.peers[peerIndex], 0);
	}
	if (caseIndex >= MIXED_WORKER_LOOPBACK_CASE_COUNT)
		return false;

	if (caseIndex == 0)
	{
		output.name = "two-peer-1-v-16";
		output.peerCount = 2;
		SetExplicit(output.peers[0], 1);
		SetExplicit(output.peers[1], 16);
	}
	else if (caseIndex == 1)
	{
		output.name = "two-peer-2-v-auto";
		output.peerCount = 2;
		SetExplicit(output.peers[0], 2);
		SetAuto(output.peers[1]);
	}
	else if (caseIndex == 2)
	{
		output.name = "two-peer-4-v-8";
		output.peerCount = 2;
		SetExplicit(output.peers[0], 4);
		SetExplicit(output.peers[1], 8);
	}
	else
	{
		output.name = "four-peer-mixed-workers";
		output.peerCount = 4;
		SetExplicit(output.peers[0], 1);
		SetExplicit(output.peers[1], 2);
		SetExplicit(output.peers[2], 8);
		SetAuto(output.peers[3]);
	}
	return true;
}

unsigned GetMixedWorkerLoopbackEvidenceRecordCount()
{
	unsigned count = 0;
	unsigned caseIndex;
	for (caseIndex = 0; caseIndex < MIXED_WORKER_LOOPBACK_CASE_COUNT;
		++caseIndex)
	{
		MixedWorkerLoopbackCase loopbackCase;
		if (!GetMixedWorkerLoopbackCase(caseIndex, loopbackCase))
			return 0;
		count += loopbackCase.peerCount * MIXED_WORKER_LOOPBACK_SEED_COUNT;
	}
	return count;
}

bool ValidateMixedWorkerLoopbackEvidence(
	const MixedWorkerLoopbackPeerEvidence *records,
	unsigned recordCount,
	MixedWorkerLoopbackTitle expectedTitle,
	const char *expectedSourceRevision,
	const char *expectedExecutableSha256,
	const char *expectedArtifactSetSha256,
	unsigned expectedBuildCompatibilityCrc,
	unsigned expectedContentCrc,
	unsigned expectedMapCrc,
	unsigned expectedKernelMask)
{
	if (records == 0 ||
		(expectedTitle != MIXED_WORKER_LOOPBACK_TITLE_GENERALS &&
		 expectedTitle != MIXED_WORKER_LOOPBACK_TITLE_ZERO_HOUR) ||
		!IsCanonicalMultiplayerSimulationHexDigest(expectedSourceRevision,
			MULTIPLAYER_SIMULATION_SOURCE_REVISION_HEX_LENGTH) ||
		!IsCanonicalMultiplayerSimulationHexDigest(expectedExecutableSha256,
			MULTIPLAYER_SIMULATION_EVIDENCE_SHA256_HEX_LENGTH) ||
		!IsCanonicalMultiplayerSimulationHexDigest(expectedArtifactSetSha256,
			MULTIPLAYER_SIMULATION_EVIDENCE_SHA256_HEX_LENGTH) ||
		expectedBuildCompatibilityCrc == 0 ||
		expectedContentCrc == 0 || expectedMapCrc == 0 ||
		expectedKernelMask == 0 ||
		(expectedKernelMask & ~static_cast<unsigned>(
			MULTIPLAYER_SIMULATION_KERNEL_KNOWN_MASK)) != 0 ||
		recordCount != GetMixedWorkerLoopbackEvidenceRecordCount())
	{
		return false;
	}

	unsigned recordIndex = 0;
	unsigned caseIndex;
	for (caseIndex = 0; caseIndex < MIXED_WORKER_LOOPBACK_CASE_COUNT;
		++caseIndex)
	{
		MixedWorkerLoopbackCase loopbackCase;
		if (!GetMixedWorkerLoopbackCase(caseIndex, loopbackCase))
			return false;
		const unsigned rosterMask = (1u << loopbackCase.peerCount) - 1;
		unsigned seedIndex;
		for (seedIndex = 0; seedIndex < MIXED_WORKER_LOOPBACK_SEED_COUNT;
			++seedIndex)
		{
			const unsigned seedValue =
				GetMixedWorkerLoopbackSeed(seedIndex);
			if (seedValue == 0)
				return false;
			unsigned canonicalTraceCrc = 0;
			unsigned peerIndex;
			for (peerIndex = 0; peerIndex < loopbackCase.peerCount;
				++peerIndex, ++recordIndex)
			{
				const MixedWorkerLoopbackPeerEvidence &record =
					records[recordIndex];
				const MixedWorkerLoopbackPeerConfiguration &configuration =
					loopbackCase.peers[peerIndex];
				if (record.caseIndex != caseIndex ||
					record.seedIndex != seedIndex ||
					record.seedValue != seedValue ||
					record.peerIndex != peerIndex ||
					record.title != expectedTitle ||
					!AreExactMultiplayerSimulationHexDigestsEqual(
						record.sourceRevision, expectedSourceRevision,
						MULTIPLAYER_SIMULATION_SOURCE_REVISION_HEX_LENGTH) ||
					!AreExactMultiplayerSimulationHexDigestsEqual(
						record.executableSha256, expectedExecutableSha256,
						MULTIPLAYER_SIMULATION_EVIDENCE_SHA256_HEX_LENGTH) ||
					!AreExactMultiplayerSimulationHexDigestsEqual(
						record.artifactSetSha256,
						expectedArtifactSetSha256,
						MULTIPLAYER_SIMULATION_EVIDENCE_SHA256_HEX_LENGTH) ||
					record.engineEpoch != MULTIPLAYER_SIMULATION_ENGINE_EPOCH ||
					record.determinismEpoch !=
						MULTIPLAYER_SIMULATION_DETERMINISM_EPOCH ||
					record.buildCompatibilityCrc !=
						expectedBuildCompatibilityCrc ||
					record.contentCrc != expectedContentCrc ||
					record.mapCrc != expectedMapCrc ||
					record.rosterMask != rosterMask ||
					record.resolvedKernelMask != expectedKernelMask ||
					record.requestedWorkerCount != configuration.workerCount ||
					record.effectiveWorkerCount == 0 ||
					record.effectiveWorkerCount == ~0u ||
					(configuration.workerPolicy ==
						MIXED_WORKER_LOOPBACK_EXPLICIT &&
					 record.effectiveWorkerCount != configuration.workerCount) ||
					!IsValidPhysicalEvidence(record, expectedKernelMask) ||
					record.canonicalTraceCrc == 0 ||
					record.processExitCode != 0 || !record.handshakeReady ||
					!record.cleanShutdown)
				{
					return false;
				}
				if (peerIndex == 0)
					canonicalTraceCrc = record.canonicalTraceCrc;
				else if (record.canonicalTraceCrc != canonicalTraceCrc)
					return false;
			}
		}
	}
	return recordIndex == recordCount;
}

} // namespace rts
