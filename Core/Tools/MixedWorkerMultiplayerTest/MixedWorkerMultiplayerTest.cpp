#include "Lib/JobSystem.h"
#include "Lib/MultiplayerSimulationPolicy.h"
#include "MixedWorkerMultiplayerLoopbackContract.h"
#include "../TestSupport/LocalCapacityTestLane.h"

#include <stdio.h>
#include <string.h>

namespace
{

enum
{
	SOAK_FRAME_COUNT = 32,
	SOAK_STATE_COUNT = 257,
	SOAK_KERNEL_COUNT = 6,
	SOAK_REPEAT_COUNT = 2,
	SOAK_MINIMUM_GRAIN = 16
};

const rts::MultiplayerSimulationKernel SOAK_KERNELS[SOAK_KERNEL_COUNT] =
{
	rts::MULTIPLAYER_SIMULATION_KERNEL_PHYSICS,
	rts::MULTIPLAYER_SIMULATION_KERNEL_STATUS,
	rts::MULTIPLAYER_SIMULATION_KERNEL_COLLISION,
	rts::MULTIPLAYER_SIMULATION_KERNEL_AI_PLANNING,
	rts::MULTIPLAYER_SIMULATION_KERNEL_SPATIAL,
	rts::MULTIPLAYER_SIMULATION_KERNEL_PATH
};

struct CanonicalTrace
{
	unsigned frameDigest[SOAK_FRAME_COUNT];
	unsigned networkCommitDigest[SOAK_FRAME_COUNT];
	unsigned recorderPublicationDigest[SOAK_FRAME_COUNT];
	unsigned parallelKernelMask;
	unsigned physicalJobCount;
	unsigned physicalWorkerMask;
};

struct SoakRangeEvidence
{
	SoakRangeEvidence()
		: physicalExecution(false),
		  physicalWorkerIndex(rts::JOB_INVALID_PHYSICAL_WORKER_INDEX)
	{
	}

	bool physicalExecution;
	unsigned physicalWorkerIndex;
};

int Check(bool condition, const char *message)
{
	if (!condition)
	{
		fprintf(stderr, "FAIL: %s\n", message);
		return 1;
	}
	return 0;
}

unsigned RotateLeft(unsigned value, unsigned count)
{
	const unsigned width = static_cast<unsigned>(sizeof(unsigned) * 8);
	count %= width;
	if (count == 0) return value;
	return (value << count) | (value >> (width - count));
}

unsigned ComputeStateValue(const unsigned *snapshot, unsigned index,
	unsigned frame, unsigned kernelOrdinal)
{
	const unsigned nextIndex = (index + kernelOrdinal + 1) % SOAK_STATE_COUNT;
	const unsigned salt = 0x9e3779b9u ^
		(frame * 0x85ebca6bu) ^
		((kernelOrdinal + 1) * 0xc2b2ae35u) ^ index;
	return RotateLeft(snapshot[index] ^ salt,
		(index + frame + kernelOrdinal) % 31 + 1) + snapshot[nextIndex];
}

void AppendDigest(unsigned &digest, unsigned value)
{
	unsigned byteIndex;
	for (byteIndex = 0; byteIndex < sizeof(value); ++byteIndex)
	{
		digest ^= value & 0xffu;
		digest *= 16777619u;
		value >>= 8;
	}
}

unsigned DigestState(const unsigned *state, unsigned frame)
{
	unsigned digest = 2166136261u;
	AppendDigest(digest, frame);
	unsigned index;
	for (index = 0; index < SOAK_STATE_COUNT; ++index)
		AppendDigest(digest, state[index]);
	return digest;
}

class SoakRangeJob : public rts::Job
{
public:
	SoakRangeJob(const rts::JobRange &range, const unsigned *snapshot,
		unsigned *output, unsigned frame, unsigned kernelOrdinal,
		SoakRangeEvidence *evidence)
		: m_range(range), m_snapshot(snapshot), m_output(output),
		  m_frame(frame), m_kernelOrdinal(kernelOrdinal),
		  m_evidence(evidence)
	{
	}

	virtual void execute(rts::JobContext &context)
	{
		m_evidence->physicalExecution = context.isPhysicalWorkerExecution();
		m_evidence->physicalWorkerIndex = context.physicalWorkerIndex();
		unsigned index;
		for (index = m_range.begin; index < m_range.end; ++index)
		{
			m_output[index] = ComputeStateValue(m_snapshot, index,
				m_frame, m_kernelOrdinal);
		}
	}

private:
	rts::JobRange m_range;
	const unsigned *m_snapshot;
	unsigned *m_output;
	unsigned m_frame;
	unsigned m_kernelOrdinal;
	SoakRangeEvidence *m_evidence;
};

rts::MultiplayerSimulationPeerPolicy MakePeerPolicy(unsigned provenKernelMask)
{
	rts::MultiplayerSimulationPeerPolicy peer;
	peer.schema = rts::MULTIPLAYER_SIMULATION_POLICY_SCHEMA;
	peer.engineEpoch = rts::MULTIPLAYER_SIMULATION_ENGINE_EPOCH;
	peer.determinismEpoch =
		rts::MULTIPLAYER_SIMULATION_DETERMINISM_EPOCH;
	peer.buildCompatibilityCrc = 0x13579bdfu;
	peer.contentCrc = 0x2468ace0u;
	peer.mapCrc = 0x10203040u;
	peer.provenKernelMask = provenKernelMask;
	return peer;
}

unsigned GetNonProductHarnessKernelMask()
{
	return rts::SelectMultiplayerSimulationNonProductTestOverrideMask(
		static_cast<unsigned>(
			rts::MULTIPLAYER_SIMULATION_KERNEL_LIVE_INTEGRATED_MASK),
		static_cast<unsigned>(
			rts::MULTIPLAYER_SIMULATION_KERNEL_KNOWN_MASK));
}

bool PrepareParallel(rts::JobSystem &jobs, const unsigned *snapshot,
	unsigned *output, unsigned frame, unsigned kernelOrdinal,
	CanonicalTrace &trace)
{
	const unsigned rangeCount = rts::JobSystem::chooseRangeCount(
		SOAK_STATE_COUNT, SOAK_MINIMUM_GRAIN, jobs.workerCount());
	if (rangeCount == 0) return false;
	rts::JobGroup group = jobs.createGroup();
	if (!group.isValid()) return false;
	rts::JobHandle *handles = new rts::JobHandle[rangeCount];
	SoakRangeEvidence *evidence = new SoakRangeEvidence[rangeCount];
	bool submitted = true;
	unsigned rangeIndex;
	for (rangeIndex = 0; rangeIndex < rangeCount; ++rangeIndex)
	{
		rts::JobRange range;
		if (!rts::JobSystem::rangeForIndex(SOAK_STATE_COUNT, rangeCount,
			rangeIndex, range))
		{
			submitted = false;
			break;
		}
		rts::Job *job = new SoakRangeJob(range, snapshot, output,
			frame, kernelOrdinal, &evidence[rangeIndex]);
		handles[rangeIndex] = jobs.trySubmit(job,
			rts::JOB_PRIORITY_FRAME_CRITICAL, group);
		if (!handles[rangeIndex].isValid())
		{
			delete job;
			submitted = false;
			break;
		}
	}
	if (!submitted)
		jobs.cancel(group);
	const bool completed = jobs.wait(group);
	bool succeeded = submitted && completed;
	if (succeeded)
	{
		for (rangeIndex = 0; rangeIndex < rangeCount; ++rangeIndex)
		{
			if (!handles[rangeIndex].succeeded())
			{
				succeeded = false;
				break;
			}
		}
	}
	if (succeeded)
	{
		for (rangeIndex = 0; rangeIndex < rangeCount; ++rangeIndex)
		{
			if (evidence[rangeIndex].physicalExecution)
			{
				++trace.physicalJobCount;
				if (evidence[rangeIndex].physicalWorkerIndex < 32)
				{
					trace.physicalWorkerMask |=
						1u << evidence[rangeIndex].physicalWorkerIndex;
				}
			}
		}
	}
	delete[] evidence;
	delete[] handles;
	return succeeded;
}

void PrepareSerial(const unsigned *snapshot, unsigned *output,
	unsigned frame, unsigned kernelOrdinal)
{
	unsigned index;
	for (index = 0; index < SOAK_STATE_COUNT; ++index)
	{
		output[index] = ComputeStateValue(snapshot, index, frame,
			kernelOrdinal);
	}
}

bool RunSoakOnStartedScheduler(rts::JobSystem &jobs,
	const rts::MultiplayerSimulationSessionPolicy &policy,
	CanonicalTrace &trace)
{
	unsigned state[SOAK_STATE_COUNT];
	unsigned snapshot[SOAK_STATE_COUNT];
	unsigned output[SOAK_STATE_COUNT];
	unsigned index;
	for (index = 0; index < SOAK_STATE_COUNT; ++index)
		state[index] = 0xa5a5a5a5u ^ (index * 0x45d9f3bu);
	memset(&trace, 0, sizeof(trace));

	bool succeeded = true;
	unsigned frame;
	for (frame = 0; frame < SOAK_FRAME_COUNT && succeeded; ++frame)
	{
		if (!jobs.isCurrentThread(rts::JOB_OWNER_GAME) ||
			rts::IsMultiplayerSimulationWorkerBoundary(
				rts::MULTIPLAYER_SIMULATION_BOUNDARY_OWNER_INTAKE))
		{
			succeeded = false;
			break;
		}
		// Owner intake applies the already ordered lockstep command stream.
		const unsigned intakeIndex = (frame * 17 + 3) % SOAK_STATE_COUNT;
		state[intakeIndex] ^= 0x7f4a7c15u + frame;

		unsigned kernelOrdinal;
		for (kernelOrdinal = 0; kernelOrdinal < SOAK_KERNEL_COUNT;
			++kernelOrdinal)
		{
			memcpy(snapshot, state, sizeof(snapshot));
			const bool useWorkers =
				rts::ShouldRunMultiplayerSimulationKernelOnWorkers(
					policy, SOAK_KERNELS[kernelOrdinal],
					rts::SIMULATION_EXECUTION_PARALLEL,
					jobs.isRunning(), jobs.workerCount());
			if (useWorkers)
			{
				trace.parallelKernelMask |=
					static_cast<unsigned>(SOAK_KERNELS[kernelOrdinal]);
				succeeded = PrepareParallel(jobs, snapshot, output, frame,
					kernelOrdinal, trace);
			}
			else
			{
				PrepareSerial(snapshot, output, frame, kernelOrdinal);
			}
			if (!succeeded) break;
			// The owner validates and publishes the complete output in canonical
			// item order. Workers never mutate the live state array.
			if (!jobs.isCurrentThread(rts::JOB_OWNER_GAME) ||
				rts::IsMultiplayerSimulationWorkerBoundary(
					rts::MULTIPLAYER_SIMULATION_BOUNDARY_OWNER_KERNEL_COMMIT))
			{
				succeeded = false;
				break;
			}
			memcpy(state, output, sizeof(state));
		}

		if (succeeded)
		{
			if (!jobs.isCurrentThread(rts::JOB_OWNER_GAME) ||
				rts::IsMultiplayerSimulationWorkerBoundary(
					rts::MULTIPLAYER_SIMULATION_BOUNDARY_OWNER_NETWORK_COMMIT))
			{
				succeeded = false;
				break;
			}
			trace.frameDigest[frame] = DigestState(state, frame);
			unsigned networkDigest = 2166136261u;
			AppendDigest(networkDigest, frame);
			AppendDigest(networkDigest, policy.determinismEpoch);
			AppendDigest(networkDigest, policy.contentCrc);
			AppendDigest(networkDigest, policy.mapCrc);
			AppendDigest(networkDigest, trace.frameDigest[frame]);
			trace.networkCommitDigest[frame] = networkDigest;

			if (!jobs.isCurrentThread(rts::JOB_OWNER_GAME) ||
				rts::IsMultiplayerSimulationWorkerBoundary(
					rts::MULTIPLAYER_SIMULATION_BOUNDARY_OWNER_RECORDER_PUBLICATION))
			{
				succeeded = false;
				break;
			}
			unsigned recorderDigest = 2166136261u;
			AppendDigest(recorderDigest, frame);
			AppendDigest(recorderDigest, trace.frameDigest[frame]);
			trace.recorderPublicationDigest[frame] = recorderDigest;
		}
	}

	return succeeded;
}

bool StartSoakScheduler(rts::JobSystem &jobs, unsigned requestedWorkerCount)
{
	rts::JobSystemConfig config;
	config.workerCount = requestedWorkerCount;
	config.queueCapacity = 256;
	config.scratchBytesPerWorker = 4096;
	config.pinWorkers = false;
	if (!jobs.start(config)) return false;
	if (!jobs.registerCurrentThread(rts::JOB_OWNER_GAME))
	{
		jobs.shutdown();
		return false;
	}
#if defined(_MSC_VER) && _MSC_VER < 1300
	const bool countMatches = jobs.workerCount() == 1;
#else
	const bool countMatches = jobs.workerCount() == requestedWorkerCount;
#endif
	if (!countMatches)
	{
		jobs.shutdown();
		jobs.unregisterCurrentThread(rts::JOB_OWNER_GAME);
	}
	return countMatches;
}

bool StopSoakScheduler(rts::JobSystem &jobs)
{
	jobs.shutdown();
	return jobs.unregisterCurrentThread(rts::JOB_OWNER_GAME);
}

unsigned CountSetBits(unsigned value)
{
	unsigned count = 0;
	while (value != 0)
	{
		value &= value - 1;
		++count;
	}
	return count;
}

bool EqualTrace(const CanonicalTrace &left, const CanonicalTrace &right)
{
	return memcmp(left.frameDigest, right.frameDigest,
		sizeof(left.frameDigest)) == 0 &&
		memcmp(left.networkCommitDigest, right.networkCommitDigest,
			sizeof(left.networkCommitDigest)) == 0 &&
		memcmp(left.recorderPublicationDigest,
			right.recorderPublicationDigest,
			sizeof(left.recorderPublicationDigest)) == 0;
}

int TestReleaseEvidenceGate()
{
	int result = 0;
	const unsigned liveIntegratedMask = static_cast<unsigned>(
		rts::MULTIPLAYER_SIMULATION_KERNEL_LIVE_INTEGRATED_MASK);
	rts::MultiplayerSimulationGeneratedReleaseProof proof =
	{
		rts::MULTIPLAYER_SIMULATION_RELEASE_PROOF_SCHEMA,
		"0123456789abcdef0123456789abcdef01234567",
		"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
		"123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0",
		"23456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef01",
		"3456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef012",
		liveIntegratedMask
	};
	result |= Check(
		rts::MULTIPLAYER_SIMULATION_KERNEL_RELEASE_PROVEN_DEFAULT_MASK == 0,
		"default product release-proven mask is serial");
	proof.schema = 0;
	result |= Check(
		rts::ResolveMultiplayerSimulationGeneratedReleaseProofMask(
			proof, liveIntegratedMask) == 0,
		"wrong generated release-proof schema is rejected");
	proof.schema = rts::MULTIPLAYER_SIMULATION_RELEASE_PROOF_SCHEMA;
	const char *savedSourceRevision = proof.sourceRevision;
	proof.sourceRevision = "";
	result |= Check(
		rts::ResolveMultiplayerSimulationGeneratedReleaseProofMask(
			proof, liveIntegratedMask) == 0,
		"missing generated source revision is rejected");
	proof.sourceRevision = savedSourceRevision;
	const char *savedGeneralsSha256 = proof.generalsExecutableSha256;
	proof.generalsExecutableSha256 = "";
	result |= Check(
		rts::ResolveMultiplayerSimulationGeneratedReleaseProofMask(
			proof, liveIntegratedMask) == 0,
		"missing generated title executable digest is rejected");
	proof.generalsExecutableSha256 = savedGeneralsSha256;
	const char *savedZeroHourSha256 = proof.zeroHourExecutableSha256;
	proof.zeroHourExecutableSha256 = "";
	result |= Check(
		rts::ResolveMultiplayerSimulationGeneratedReleaseProofMask(
			proof, liveIntegratedMask) == 0,
		"missing generated second-title executable digest is rejected");
	proof.zeroHourExecutableSha256 = savedZeroHourSha256;
	const char *savedArtifactSetSha256 = proof.artifactSetSha256;
	proof.artifactSetSha256 = "";
	result |= Check(
		rts::ResolveMultiplayerSimulationGeneratedReleaseProofMask(
			proof, liveIntegratedMask) == 0,
		"missing generated artifact-set digest is rejected");
	proof.artifactSetSha256 = savedArtifactSetSha256;
	const char *savedManifestSha256 = proof.evidenceManifestSha256;
	proof.evidenceManifestSha256 =
		"0000000000000000000000000000000000000000000000000000000000000000";
	result |= Check(
		rts::ResolveMultiplayerSimulationGeneratedReleaseProofMask(
			proof, liveIntegratedMask) == 0,
		"all-zero generated evidence-manifest digest is rejected");
	proof.evidenceManifestSha256 = savedManifestSha256;
	proof.provenKernelMask = 1u << 31;
	result |= Check(
		rts::ResolveMultiplayerSimulationGeneratedReleaseProofMask(
			proof, liveIntegratedMask) == 0,
		"unknown generated release-proof kernel bits are rejected");
	proof.provenKernelMask = 0;
	result |= Check(
		rts::ResolveMultiplayerSimulationGeneratedReleaseProofMask(
			proof, liveIntegratedMask) == 0,
		"empty generated release-proof kernel mask stays serial");
	proof.provenKernelMask = liveIntegratedMask;
	result |= Check(
		rts::ResolveMultiplayerSimulationGeneratedReleaseProofMask(
			proof, liveIntegratedMask) == liveIntegratedMask,
		"complete generated release proof unlocks only its proven bits");
	proof.provenKernelMask =
		rts::MULTIPLAYER_SIMULATION_KERNEL_PHYSICS;
	result |= Check(
		rts::ResolveMultiplayerSimulationGeneratedReleaseProofMask(
			proof, liveIntegratedMask) ==
		static_cast<unsigned>(rts::MULTIPLAYER_SIMULATION_KERNEL_PHYSICS),
		"a partial generated proof unlocks only its separately proven kernel");
	result |= Check(GetNonProductHarnessKernelMask() == liveIntegratedMask,
		"policy soak uses an explicit non-product positive override");
	result |= Check(
		rts::SelectMultiplayerSimulationNonProductTestOverrideMask(
			liveIntegratedMask, 1u << 31) == 0,
		"non-product override cannot admit unknown kernels");
	return result;
}

int TestNetworkPolicyLifecycle()
{
	int result = 0;
	const unsigned rosterMask = 0x0fu;
	const unsigned expectedRemoteMask = 0x0eu;
	result |= Check(rts::IsMultiplayerSimulationPolicyLifecycleUsable(
		true, true, true, false, rosterMask, expectedRemoteMask,
		expectedRemoteMask, 0, 0, 8),
		"resolved policy is usable only with the exact live roster");
	result |= Check(!rts::IsMultiplayerSimulationPolicyLifecycleUsable(
		false, true, true, false, rosterMask, expectedRemoteMask,
		expectedRemoteMask, 0, 0, 8),
		"reset or revoked policy is unavailable");
	result |= Check(!rts::IsMultiplayerSimulationPolicyLifecycleUsable(
		true, true, false, false, rosterMask, expectedRemoteMask,
		expectedRemoteMask, 0, 0, 8),
		"incomplete network hello keeps policy unavailable");
	result |= Check(!rts::IsMultiplayerSimulationPolicyLifecycleUsable(
		true, true, true, true, rosterMask, expectedRemoteMask,
		expectedRemoteMask, 0, 0, 8),
		"network hello failure revokes policy usability");
	result |= Check(!rts::IsMultiplayerSimulationPolicyLifecycleUsable(
		true, false, true, false, rosterMask, expectedRemoteMask,
		expectedRemoteMask, 0, 0, 8),
		"rejected session policy is unavailable");
	result |= Check(!rts::IsMultiplayerSimulationPolicyLifecycleUsable(
		true, true, true, false, rosterMask, expectedRemoteMask,
		0x06u, 0, 0, 8),
		"first missing peer revokes policy usability");
	result |= Check(!rts::IsMultiplayerSimulationPolicyLifecycleUsable(
		true, true, true, false, rosterMask, expectedRemoteMask,
		expectedRemoteMask, 0x04u, 0, 8),
		"first quitting peer revokes policy usability");
	result |= Check(!rts::IsMultiplayerSimulationPolicyLifecycleUsable(
		true, true, true, false, 0x07u, expectedRemoteMask,
		expectedRemoteMask, 0, 0, 8),
		"policy cannot renegotiate a reduced roster after resolution");
	return result;
}

int TestPolicyNegotiation()
{
	int result = 0;
	const rts::MultiplayerSimulationPeerPolicy local = MakePeerPolicy(
		GetNonProductHarnessKernelMask());
	rts::MultiplayerSimulationPeerPolicy remote = local;
	rts::MultiplayerSimulationSessionPolicy policy;
	result |= Check(rts::ResolveMultiplayerSimulationSessionPolicy(local,
		&remote, 1, rts::MULTIPLAYER_SIMULATION_KERNEL_KNOWN_MASK,
		policy), "matching peer contracts enable mixed-worker policy");
	result |= Check(policy.status ==
		rts::MULTIPLAYER_SIMULATION_POLICY_READY &&
		policy.participantCount == 2 && policy.enabledKernelMask ==
		static_cast<unsigned>(
			rts::MULTIPLAYER_SIMULATION_KERNEL_KNOWN_MASK),
		"matching peers publish the exact proven-kernel intersection");
	result |= Check(rts::ShouldRunMultiplayerSimulationKernelOnWorkers(
		policy, rts::MULTIPLAYER_SIMULATION_KERNEL_PHYSICS,
		rts::SIMULATION_EXECUTION_PARALLEL, true, 16),
		"proven parallel kernel is worker eligible");
	result |= Check(!rts::ShouldRunMultiplayerSimulationKernelOnWorkers(
		policy, rts::MULTIPLAYER_SIMULATION_KERNEL_PHYSICS,
		rts::SIMULATION_EXECUTION_SHADOW, true, 16) &&
		!rts::ShouldRunMultiplayerSimulationKernelOnWorkers(
			policy, rts::MULTIPLAYER_SIMULATION_KERNEL_PHYSICS,
			rts::SIMULATION_EXECUTION_PARALLEL, true, 1) &&
		!rts::ShouldRunMultiplayerSimulationKernelOnWorkers(
			policy, rts::MULTIPLAYER_SIMULATION_KERNEL_PHYSICS,
			rts::SIMULATION_EXECUTION_PARALLEL, false, 16) &&
		!rts::ShouldRunMultiplayerSimulationKernelOnWorkers(
			policy, rts::MULTIPLAYER_SIMULATION_KERNEL_PHYSICS,
			rts::SIMULATION_EXECUTION_PARALLEL, true, 0) &&
		!rts::ShouldRunMultiplayerSimulationKernelOnWorkers(
			policy, rts::MULTIPLAYER_SIMULATION_KERNEL_PHYSICS,
			rts::SIMULATION_EXECUTION_PARALLEL, true, ~0u),
		"shadow, unavailable scheduler, and invalid worker counts remain serial");

	result |= Check(!rts::ResolveMultiplayerSimulationSessionPolicy(local,
		0, 1, rts::MULTIPLAYER_SIMULATION_KERNEL_KNOWN_MASK, policy) &&
		policy.status ==
			rts::MULTIPLAYER_SIMULATION_POLICY_SERIAL_INVALID_ARGUMENT,
		"null remote roster fails closed");
	result |= Check(!rts::ResolveMultiplayerSimulationSessionPolicy(local,
		&remote, 0, rts::MULTIPLAYER_SIMULATION_KERNEL_KNOWN_MASK, policy) &&
		policy.status ==
			rts::MULTIPLAYER_SIMULATION_POLICY_SERIAL_INVALID_ARGUMENT,
		"zero-peer roster fails closed");
	rts::MultiplayerSimulationPeerPolicy maximumRemotes[
		rts::MULTIPLAYER_SIMULATION_MAXIMUM_REMOTE_PEERS + 1];
	unsigned remoteIndex;
	for (remoteIndex = 0;
		remoteIndex < rts::MULTIPLAYER_SIMULATION_MAXIMUM_REMOTE_PEERS + 1;
		++remoteIndex)
	{
		maximumRemotes[remoteIndex] = local;
	}
	result |= Check(rts::ResolveMultiplayerSimulationSessionPolicy(local,
		maximumRemotes, rts::MULTIPLAYER_SIMULATION_MAXIMUM_REMOTE_PEERS,
		rts::MULTIPLAYER_SIMULATION_KERNEL_KNOWN_MASK, policy) &&
		policy.participantCount == 8,
		"seven remotes fill the authoritative eight-slot roster");
	result |= Check(!rts::ResolveMultiplayerSimulationSessionPolicy(local,
		maximumRemotes, rts::MULTIPLAYER_SIMULATION_MAXIMUM_REMOTE_PEERS + 1,
		rts::MULTIPLAYER_SIMULATION_KERNEL_KNOWN_MASK, policy),
		"an eighth remote exceeds the fixed game roster and fails closed");

	remote = local;
	remote.schema = rts::MULTIPLAYER_SIMULATION_POLICY_SCHEMA + 1;
	result |= Check(!rts::ResolveMultiplayerSimulationSessionPolicy(local,
		&remote, 1, rts::MULTIPLAYER_SIMULATION_KERNEL_KNOWN_MASK,
		policy) && policy.status ==
		rts::MULTIPLAYER_SIMULATION_POLICY_SERIAL_UNSUPPORTED_SCHEMA,
		"unsupported policy schema rejects every worker kernel");

	remote = local;
	remote.engineEpoch = rts::MULTIPLAYER_SIMULATION_ENGINE_EPOCH + 1;
	result |= Check(!rts::ResolveMultiplayerSimulationSessionPolicy(local,
		&remote, 1, rts::MULTIPLAYER_SIMULATION_KERNEL_KNOWN_MASK,
		policy) && policy.status ==
		rts::MULTIPLAYER_SIMULATION_POLICY_SERIAL_UNSUPPORTED_ENGINE_EPOCH,
		"unsupported engine epoch rejects every worker kernel");

	remote = local;
	remote.buildCompatibilityCrc ^= 1;
	result |= Check(!rts::ResolveMultiplayerSimulationSessionPolicy(local,
		&remote, 1, rts::MULTIPLAYER_SIMULATION_KERNEL_KNOWN_MASK,
		policy) && policy.status ==
		rts::MULTIPLAYER_SIMULATION_POLICY_SERIAL_BUILD_MISMATCH,
		"build mismatch rejects every worker kernel");

	remote = local;
	remote.provenKernelMask &=
		~static_cast<unsigned>(rts::MULTIPLAYER_SIMULATION_KERNEL_PATH);
	result |= Check(rts::ResolveMultiplayerSimulationSessionPolicy(local,
		&remote, 1, rts::MULTIPLAYER_SIMULATION_KERNEL_KNOWN_MASK,
		policy) && !rts::IsMultiplayerSimulationKernelEnabled(policy,
			rts::MULTIPLAYER_SIMULATION_KERNEL_PATH) &&
		rts::IsMultiplayerSimulationKernelEnabled(policy,
			rts::MULTIPLAYER_SIMULATION_KERNEL_SPATIAL),
		"missing proof keeps only the unproven subsystem serial");

	remote = local;
	remote.determinismEpoch =
		rts::MULTIPLAYER_SIMULATION_DETERMINISM_EPOCH + 1;
	result |= Check(!rts::ResolveMultiplayerSimulationSessionPolicy(local,
		&remote, 1, rts::MULTIPLAYER_SIMULATION_KERNEL_KNOWN_MASK,
		policy) && policy.status ==
		rts::MULTIPLAYER_SIMULATION_POLICY_SERIAL_UNSUPPORTED_EPOCH &&
		policy.enabledKernelMask == 0,
		"unsupported epoch rejects every worker kernel");

	remote = local;
	remote.contentCrc ^= 1;
	result |= Check(!rts::ResolveMultiplayerSimulationSessionPolicy(local,
		&remote, 1, rts::MULTIPLAYER_SIMULATION_KERNEL_KNOWN_MASK,
		policy) && policy.status ==
		rts::MULTIPLAYER_SIMULATION_POLICY_SERIAL_CONTENT_MISMATCH &&
		policy.enabledKernelMask == 0,
		"content mismatch rejects every worker kernel");

	remote = local;
	remote.provenKernelMask |= 1u << 31;
	result |= Check(!rts::ResolveMultiplayerSimulationSessionPolicy(local,
		&remote, 1, rts::MULTIPLAYER_SIMULATION_KERNEL_KNOWN_MASK,
		policy) && policy.status ==
		rts::MULTIPLAYER_SIMULATION_POLICY_SERIAL_INVALID_KERNEL_PROOF,
		"unknown proof bits fail closed instead of enabling a subsystem");

	rts::MultiplayerSimulationPeerPolicy orderedRemotes[2];
	orderedRemotes[0] = local;
	orderedRemotes[1] = local;
	orderedRemotes[0].contentCrc ^= 1;
	orderedRemotes[1].schema =
		rts::MULTIPLAYER_SIMULATION_POLICY_SCHEMA + 1;
	rts::MultiplayerSimulationSessionPolicy forwardPolicy;
	rts::MultiplayerSimulationSessionPolicy reversePolicy;
	result |= Check(!rts::ResolveMultiplayerSimulationSessionPolicy(local,
		orderedRemotes, 2, rts::MULTIPLAYER_SIMULATION_KERNEL_KNOWN_MASK,
		forwardPolicy), "forward failure-order permutation is rejected");
	rts::MultiplayerSimulationPeerPolicy swap = orderedRemotes[0];
	orderedRemotes[0] = orderedRemotes[1];
	orderedRemotes[1] = swap;
	result |= Check(!rts::ResolveMultiplayerSimulationSessionPolicy(local,
		orderedRemotes, 2, rts::MULTIPLAYER_SIMULATION_KERNEL_KNOWN_MASK,
		reversePolicy) && forwardPolicy.status == reversePolicy.status &&
		forwardPolicy.enabledKernelMask == 0 &&
		reversePolicy.enabledKernelMask == 0,
		"roster order cannot change fail-closed policy resolution");

	result |= Check(!rts::IsMultiplayerSimulationWorkerBoundary(
		rts::MULTIPLAYER_SIMULATION_BOUNDARY_OWNER_INTAKE) &&
		rts::IsMultiplayerSimulationWorkerBoundary(
			rts::MULTIPLAYER_SIMULATION_BOUNDARY_KERNEL_PREPARE) &&
		!rts::IsMultiplayerSimulationWorkerBoundary(
			rts::MULTIPLAYER_SIMULATION_BOUNDARY_OWNER_KERNEL_COMMIT) &&
		!rts::IsMultiplayerSimulationWorkerBoundary(
			rts::MULTIPLAYER_SIMULATION_BOUNDARY_OWNER_NETWORK_COMMIT) &&
		!rts::IsMultiplayerSimulationWorkerBoundary(
			rts::MULTIPLAYER_SIMULATION_BOUNDARY_OWNER_RECORDER_PUBLICATION),
		"owner intake, commit, network, and recorder boundaries never move to workers");
	return result;
}

int TestMixedWorkerSoak(bool localCapacity)
{
	const unsigned workerCounts[] = { 1, 2, 4, 8, 16 };
	const rts::MultiplayerSimulationPeerPolicy local = MakePeerPolicy(
		GetNonProductHarnessKernelMask());
	rts::MultiplayerSimulationPeerPolicy remotePeers[4];
	unsigned remoteIndex;
	for (remoteIndex = 0; remoteIndex < 4; ++remoteIndex)
		remotePeers[remoteIndex] = local;
	rts::MultiplayerSimulationSessionPolicy fullPolicy;
	int result = 0;
	result |= Check(rts::ResolveMultiplayerSimulationSessionPolicy(local,
		remotePeers, 4, rts::MULTIPLAYER_SIMULATION_KERNEL_KNOWN_MASK,
		fullPolicy), "soak policy negotiation succeeds");
	result |= Check(fullPolicy.participantCount == 5,
		"soak policy represents all five mixed-worker logical peers");
	remotePeers[2].provenKernelMask &=
		~static_cast<unsigned>(rts::MULTIPLAYER_SIMULATION_KERNEL_PATH);
	rts::MultiplayerSimulationSessionPolicy partialPolicy;
	result |= Check(rts::ResolveMultiplayerSimulationSessionPolicy(local,
		remotePeers, 4, rts::MULTIPLAYER_SIMULATION_KERNEL_KNOWN_MASK,
		partialPolicy), "partial-proof soak policy resolves");

	for (remoteIndex = 0; remoteIndex < 4; ++remoteIndex)
		remotePeers[remoteIndex] = local;
	remotePeers[1].determinismEpoch =
		rts::MULTIPLAYER_SIMULATION_DETERMINISM_EPOCH + 1;
	rts::MultiplayerSimulationSessionPolicy rejectedEpochPolicy;
	result |= Check(!rts::ResolveMultiplayerSimulationSessionPolicy(local,
		remotePeers, 4, rts::MULTIPLAYER_SIMULATION_KERNEL_KNOWN_MASK,
		rejectedEpochPolicy), "unsupported-epoch soak policy is rejected");

	for (remoteIndex = 0; remoteIndex < 4; ++remoteIndex)
		remotePeers[remoteIndex] = local;
	remotePeers[3].mapCrc ^= 1;
	rts::MultiplayerSimulationSessionPolicy rejectedContentPolicy;
	result |= Check(!rts::ResolveMultiplayerSimulationSessionPolicy(local,
		remotePeers, 4, rts::MULTIPLAYER_SIMULATION_KERNEL_KNOWN_MASK,
		rejectedContentPolicy), "content-identity soak policy is rejected");
	if (result != 0) return result;

	rts::JobSystem &jobs = rts::JobSystem::instance();
	CanonicalTrace reference;
	bool haveReference = false;
	unsigned workerIndex;
	for (workerIndex = 0;
		workerIndex < sizeof(workerCounts) / sizeof(workerCounts[0]);
		++workerIndex)
	{
		const unsigned requestedWorkerCount = workerCounts[workerIndex];
		const unsigned workerCount = rts_test::ResolveActualWorkerCount(
			requestedWorkerCount, localCapacity);
		rts_test::PrintWorkerCountSubstitution("Mixed-worker multiplayer",
			requestedWorkerCount, workerCount, localCapacity);
		const bool started = StartSoakScheduler(jobs,
			workerCount);
		result |= Check(started,
			"one scheduler startup succeeds for a logical worker configuration");
		if (!started) return result;

		unsigned repeat;
		for (repeat = 0; repeat < SOAK_REPEAT_COUNT; ++repeat)
		{
			CanonicalTrace actual;
			const bool ran = RunSoakOnStartedScheduler(jobs, fullPolicy,
				actual);
			result |= Check(ran,
				"mixed-worker repeated policy soak execution completes");
			if (!ran)
			{
				StopSoakScheduler(jobs);
				return result;
			}
			if (!haveReference)
			{
				reference = actual;
				haveReference = true;
			}
			result |= Check(EqualTrace(reference, actual),
				"every repeated worker configuration preserves the canonical trace");
#if defined(_MSC_VER) && _MSC_VER < 1300
			result |= Check(actual.parallelKernelMask == 0 &&
				actual.physicalJobCount == 0 &&
				actual.physicalWorkerMask == 0,
				"VC6 oracle keeps logical worker configurations serial");
#else
			if (workerCounts[workerIndex] == 1)
			{
				result |= Check(actual.parallelKernelMask == 0 &&
					actual.physicalJobCount == 0,
					"one-worker reference remains physically serial");
			}
			else
			{
				result |= Check(actual.parallelKernelMask ==
					static_cast<unsigned>(
						rts::MULTIPLAYER_SIMULATION_KERNEL_KNOWN_MASK),
					"policy soak routes exactly the proven kernels to workers");
				result |= Check(actual.physicalJobCount != 0 &&
					CountSetBits(actual.physicalWorkerMask) >= 2,
					"JobContext evidence proves physical execution on distinct workers");
			}
#endif
		}

		if (requestedWorkerCount == 16)
		{
			CanonicalTrace partialProof;
			result |= Check(RunSoakOnStartedScheduler(jobs, partialPolicy,
				partialProof) && EqualTrace(reference, partialProof),
				"unproven subsystem serial fallback preserves the canonical trace");
#if !defined(_MSC_VER) || _MSC_VER >= 1300
			result |= Check((partialProof.parallelKernelMask &
				static_cast<unsigned>(
					rts::MULTIPLAYER_SIMULATION_KERNEL_PATH)) == 0 &&
				(partialProof.parallelKernelMask & static_cast<unsigned>(
					rts::MULTIPLAYER_SIMULATION_KERNEL_SPATIAL)) != 0 &&
				partialProof.physicalJobCount != 0,
				"partial proof executes proven kernels and serializes only path");
#endif
			CanonicalTrace rejectedEpoch;
			result |= Check(RunSoakOnStartedScheduler(jobs,
				rejectedEpochPolicy, rejectedEpoch) &&
				EqualTrace(reference, rejectedEpoch) &&
				rejectedEpoch.parallelKernelMask == 0 &&
				rejectedEpoch.physicalJobCount == 0,
				"unsupported epoch stays physically serial with canonical output");
			CanonicalTrace rejectedContent;
			result |= Check(RunSoakOnStartedScheduler(jobs,
				rejectedContentPolicy, rejectedContent) &&
				EqualTrace(reference, rejectedContent) &&
				rejectedContent.parallelKernelMask == 0 &&
				rejectedContent.physicalJobCount == 0,
				"content mismatch stays physically serial with canonical output");
		}

		result |= Check(StopSoakScheduler(jobs),
			"scheduler stops cleanly after all repeats for the configuration");
		if (result != 0) return result;
	}
	return result;
}

int TestInstalledLoopbackContract()
{
	int result = 0;
	const char sourceRevision[] =
		"0123456789abcdef0123456789abcdef01234567";
	const char executableSha256[] =
		"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
	const char artifactSetSha256[] =
		"123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0";
	const rts::MixedWorkerLoopbackTitle title =
		rts::MIXED_WORKER_LOOPBACK_TITLE_ZERO_HOUR;
	result |= Check(rts::GetMixedWorkerLoopbackEvidenceRecordCount() == 20,
		"installed loopback contract covers four topologies and two seeds");
	rts::MixedWorkerLoopbackCase loopbackCase;
	result |= Check(rts::GetMixedWorkerLoopbackCase(0, loopbackCase) &&
		loopbackCase.peerCount == 2 &&
		loopbackCase.peers[0].workerCount == 1 &&
		loopbackCase.peers[1].workerCount == 16,
		"installed loopback contract includes 1-v-16");
	result |= Check(rts::GetMixedWorkerLoopbackCase(1, loopbackCase) &&
		loopbackCase.peerCount == 2 &&
		loopbackCase.peers[0].workerCount == 2 &&
		loopbackCase.peers[1].workerPolicy ==
			rts::MIXED_WORKER_LOOPBACK_AUTO,
		"installed loopback contract includes 2-v-auto");
	result |= Check(rts::GetMixedWorkerLoopbackCase(2, loopbackCase) &&
		loopbackCase.peers[0].workerCount == 4 &&
		loopbackCase.peers[1].workerCount == 8,
		"installed loopback contract includes 4-v-8");
	result |= Check(rts::GetMixedWorkerLoopbackCase(3, loopbackCase) &&
		loopbackCase.peerCount == 4 &&
		loopbackCase.peers[0].workerCount == 1 &&
		loopbackCase.peers[1].workerCount == 2 &&
		loopbackCase.peers[2].workerCount == 8 &&
		loopbackCase.peers[3].workerPolicy ==
			rts::MIXED_WORKER_LOOPBACK_AUTO &&
		rts::GetMixedWorkerLoopbackSeed(0) != 0 &&
		rts::GetMixedWorkerLoopbackSeed(1) != 0 &&
		rts::GetMixedWorkerLoopbackSeed(0) !=
			rts::GetMixedWorkerLoopbackSeed(1),
		"installed loopback contract includes four peers and two seeds");

	rts::MixedWorkerLoopbackPeerEvidence records[20];
	unsigned recordIndex = 0;
	unsigned caseIndex;
	for (caseIndex = 0;
		caseIndex < rts::MIXED_WORKER_LOOPBACK_CASE_COUNT; ++caseIndex)
	{
		rts::GetMixedWorkerLoopbackCase(caseIndex, loopbackCase);
		const unsigned rosterMask = (1u << loopbackCase.peerCount) - 1;
		unsigned seedIndex;
		for (seedIndex = 0;
			seedIndex < rts::MIXED_WORKER_LOOPBACK_SEED_COUNT; ++seedIndex)
		{
			unsigned peerIndex;
			for (peerIndex = 0; peerIndex < loopbackCase.peerCount;
				++peerIndex, ++recordIndex)
			{
				rts::MixedWorkerLoopbackPeerEvidence &record =
					records[recordIndex];
				record.caseIndex = caseIndex;
				record.seedIndex = seedIndex;
				record.seedValue =
					rts::GetMixedWorkerLoopbackSeed(seedIndex);
				record.peerIndex = peerIndex;
				record.title = title;
				strcpy(record.sourceRevision, sourceRevision);
				strcpy(record.executableSha256, executableSha256);
				strcpy(record.artifactSetSha256, artifactSetSha256);
				record.engineEpoch =
					rts::MULTIPLAYER_SIMULATION_ENGINE_EPOCH;
				record.determinismEpoch =
					rts::MULTIPLAYER_SIMULATION_DETERMINISM_EPOCH;
				record.buildCompatibilityCrc = 0x13579bdfu;
				record.contentCrc = 0x2468ace0u;
				record.mapCrc = 0x10203040u;
				record.rosterMask = rosterMask;
				record.resolvedKernelMask =
					GetNonProductHarnessKernelMask();
				record.requestedWorkerCount =
					loopbackCase.peers[peerIndex].workerCount;
				record.effectiveWorkerCount =
					loopbackCase.peers[peerIndex].workerPolicy ==
						rts::MIXED_WORKER_LOOPBACK_AUTO ? 4 :
						loopbackCase.peers[peerIndex].workerCount;
				record.physicalKernelMask =
					record.effectiveWorkerCount > 1 ?
					GetNonProductHarnessKernelMask() : 0;
				unsigned kernelIndex;
				for (kernelIndex = 0;
					kernelIndex < rts::MIXED_WORKER_LOOPBACK_KERNEL_COUNT;
					++kernelIndex)
				{
					const bool physical =
						record.effectiveWorkerCount > 1;
					record.kernelSubmittedJobCount[kernelIndex] =
						physical ? 8 : 0;
					record.kernelCompletedJobCount[kernelIndex] =
						physical ? 8 : 0;
					record.kernelPhysicalJobCount[kernelIndex] =
						physical ? 8 : 0;
					record.kernelPhysicalWorkerMask[kernelIndex] =
						physical ? 3 : 0;
					record.kernelDistinctPhysicalWorkerCount[kernelIndex] =
						physical ? 2 : 0;
				}
				record.canonicalTraceCrc = 0x80000000u |
					(caseIndex << 8) | seedIndex;
				record.processExitCode = 0;
				record.handshakeReady = true;
				record.cleanShutdown = true;
			}
		}
	}
	result |= Check(rts::ValidateMixedWorkerLoopbackEvidence(records, 20,
		title, sourceRevision, executableSha256, artifactSetSha256,
		0x13579bdfu, 0x2468ace0u, 0x10203040u,
		GetNonProductHarnessKernelMask()),
		"complete installed-loopback evidence matrix is accepted");
	records[1].canonicalTraceCrc ^= 1;
	result |= Check(!rts::ValidateMixedWorkerLoopbackEvidence(records, 20,
		title, sourceRevision, executableSha256, artifactSetSha256,
		0x13579bdfu, 0x2468ace0u, 0x10203040u,
		GetNonProductHarnessKernelMask()),
		"cross-peer canonical trace mismatch is rejected");
	records[1].canonicalTraceCrc ^= 1;
	records[4].rosterMask ^= 1;
	result |= Check(!rts::ValidateMixedWorkerLoopbackEvidence(records, 20,
		title, sourceRevision, executableSha256, artifactSetSha256,
		0x13579bdfu, 0x2468ace0u, 0x10203040u,
		GetNonProductHarnessKernelMask()),
		"loopback evidence not bound to the exact roster is rejected");
	records[4].rosterMask ^= 1;
	records[1].seedValue ^= 1;
	result |= Check(!rts::ValidateMixedWorkerLoopbackEvidence(records, 20,
		title, sourceRevision, executableSha256, artifactSetSha256,
		0x13579bdfu, 0x2468ace0u, 0x10203040u,
		GetNonProductHarnessKernelMask()),
		"loopback evidence not bound to the exact seed value is rejected");
	records[1].seedValue ^= 1;
	records[1].title = rts::MIXED_WORKER_LOOPBACK_TITLE_GENERALS;
	result |= Check(!rts::ValidateMixedWorkerLoopbackEvidence(records, 20,
		title, sourceRevision, executableSha256, artifactSetSha256,
		0x13579bdfu, 0x2468ace0u, 0x10203040u,
		GetNonProductHarnessKernelMask()),
		"loopback evidence not bound to the exact title is rejected");
	records[1].title = title;
	records[1].sourceRevision[0] ^= 1;
	result |= Check(!rts::ValidateMixedWorkerLoopbackEvidence(records, 20,
		title, sourceRevision, executableSha256, artifactSetSha256,
		0x13579bdfu, 0x2468ace0u, 0x10203040u,
		GetNonProductHarnessKernelMask()),
		"loopback evidence not bound to the exact source revision is rejected");
	records[1].sourceRevision[0] ^= 1;
	records[1].executableSha256[0] ^= 1;
	result |= Check(!rts::ValidateMixedWorkerLoopbackEvidence(records, 20,
		title, sourceRevision, executableSha256, artifactSetSha256,
		0x13579bdfu, 0x2468ace0u, 0x10203040u,
		GetNonProductHarnessKernelMask()),
		"loopback evidence not bound to the exact executable is rejected");
	records[1].executableSha256[0] ^= 1;
	records[1].artifactSetSha256[0] ^= 1;
	result |= Check(!rts::ValidateMixedWorkerLoopbackEvidence(records, 20,
		title, sourceRevision, executableSha256, artifactSetSha256,
		0x13579bdfu, 0x2468ace0u, 0x10203040u,
		GetNonProductHarnessKernelMask()),
		"loopback evidence not bound to the exact artifact set is rejected");
	records[1].artifactSetSha256[0] ^= 1;
	records[1].physicalKernelMask &=
		~static_cast<unsigned>(rts::MULTIPLAYER_SIMULATION_KERNEL_PHYSICS);
	result |= Check(!rts::ValidateMixedWorkerLoopbackEvidence(records, 20,
		title, sourceRevision, executableSha256, artifactSetSha256,
		0x13579bdfu, 0x2468ace0u, 0x10203040u,
		GetNonProductHarnessKernelMask()),
		"missing per-kernel physical mask evidence is rejected");
	records[1].physicalKernelMask |=
		static_cast<unsigned>(rts::MULTIPLAYER_SIMULATION_KERNEL_PHYSICS);
	records[1].kernelSubmittedJobCount[0] = 0;
	result |= Check(!rts::ValidateMixedWorkerLoopbackEvidence(records, 20,
		title, sourceRevision, executableSha256, artifactSetSha256,
		0x13579bdfu, 0x2468ace0u, 0x10203040u,
		GetNonProductHarnessKernelMask()),
		"zero per-kernel submitted work is rejected");
	records[1].kernelSubmittedJobCount[0] = 8;
	--records[1].kernelCompletedJobCount[0];
	result |= Check(!rts::ValidateMixedWorkerLoopbackEvidence(records, 20,
		title, sourceRevision, executableSha256, artifactSetSha256,
		0x13579bdfu, 0x2468ace0u, 0x10203040u,
		GetNonProductHarnessKernelMask()),
		"unbalanced per-kernel completion evidence is rejected");
	++records[1].kernelCompletedJobCount[0];
	--records[1].kernelPhysicalJobCount[0];
	result |= Check(!rts::ValidateMixedWorkerLoopbackEvidence(records, 20,
		title, sourceRevision, executableSha256, artifactSetSha256,
		0x13579bdfu, 0x2468ace0u, 0x10203040u,
		GetNonProductHarnessKernelMask()),
		"unbalanced per-kernel physical work evidence is rejected");
	++records[1].kernelPhysicalJobCount[0];
	records[1].kernelPhysicalWorkerMask[0] = 1;
	records[1].kernelDistinctPhysicalWorkerCount[0] = 1;
	result |= Check(!rts::ValidateMixedWorkerLoopbackEvidence(records, 20,
		title, sourceRevision, executableSha256, artifactSetSha256,
		0x13579bdfu, 0x2468ace0u, 0x10203040u,
		GetNonProductHarnessKernelMask()),
		"single-worker per-kernel evidence is rejected for a multi-worker peer");
	records[1].kernelPhysicalWorkerMask[0] = 3;
	records[1].kernelDistinctPhysicalWorkerCount[0] = 2;
	records[0].kernelSubmittedJobCount[0] = 1;
	result |= Check(!rts::ValidateMixedWorkerLoopbackEvidence(records, 20,
		title, sourceRevision, executableSha256, artifactSetSha256,
		0x13579bdfu, 0x2468ace0u, 0x10203040u,
		GetNonProductHarnessKernelMask()),
		"forced-one-worker peer must report zero physical kernel work");
	return result;
}

}

int main(int argc, char **argv)
{
	bool localCapacity = false;
	if (!rts_test::ParseTestCapacityLane(argc, argv, &localCapacity))
	{
		fprintf(stderr,
			"Usage: core_mixed_worker_multiplayer_policy_tests "
			"[--local-capacity]\n");
		return 2;
	}
	rts_test::PrintTestCapacityLane(localCapacity);
	int result = 0;
	result |= TestReleaseEvidenceGate();
	result |= TestNetworkPolicyLifecycle();
	result |= TestPolicyNegotiation();
	result |= TestMixedWorkerSoak(localCapacity);
	result |= TestInstalledLoopbackContract();
	if (result == 0)
		printf("Mixed-worker multiplayer policy and soak tests passed.\n");
	return result;
}
