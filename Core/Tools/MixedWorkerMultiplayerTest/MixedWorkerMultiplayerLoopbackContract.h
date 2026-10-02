#pragma once

#include "Lib/MultiplayerSimulationPolicy.h"

namespace rts
{

enum MixedWorkerLoopbackWorkerPolicy
{
	MIXED_WORKER_LOOPBACK_EXPLICIT = 0,
	MIXED_WORKER_LOOPBACK_AUTO
};

enum
{
	MIXED_WORKER_LOOPBACK_CASE_COUNT = 4,
	MIXED_WORKER_LOOPBACK_SEED_COUNT = 2,
	MIXED_WORKER_LOOPBACK_MAXIMUM_PEERS = 4,
	MIXED_WORKER_LOOPBACK_KERNEL_COUNT = 6
};

enum MixedWorkerLoopbackTitle
{
	MIXED_WORKER_LOOPBACK_TITLE_INVALID = 0,
	MIXED_WORKER_LOOPBACK_TITLE_GENERALS,
	MIXED_WORKER_LOOPBACK_TITLE_ZERO_HOUR
};

struct MixedWorkerLoopbackPeerConfiguration
{
	MixedWorkerLoopbackWorkerPolicy workerPolicy;
	unsigned workerCount;
};

struct MixedWorkerLoopbackCase
{
	const char *name;
	unsigned peerCount;
	MixedWorkerLoopbackPeerConfiguration peers[
		MIXED_WORKER_LOOPBACK_MAXIMUM_PEERS];
};

// An installed-runtime launcher owns process creation and loopback session
// setup. It must publish one record per peer for every case/seed pair. The
// verifier accepts no inferred identities: every record repeats the exact
// fixed-width handshake identity, bound roster, resolved policy, canonical
// trace, physical JobContext evidence, and clean process outcome.
struct MixedWorkerLoopbackPeerEvidence
{
	unsigned caseIndex;
	unsigned seedIndex;
	unsigned seedValue;
	unsigned peerIndex;
	MixedWorkerLoopbackTitle title;
	char sourceRevision[
		MULTIPLAYER_SIMULATION_SOURCE_REVISION_HEX_LENGTH + 1];
	char executableSha256[
		MULTIPLAYER_SIMULATION_EVIDENCE_SHA256_HEX_LENGTH + 1];
	char artifactSetSha256[
		MULTIPLAYER_SIMULATION_EVIDENCE_SHA256_HEX_LENGTH + 1];
	unsigned engineEpoch;
	unsigned determinismEpoch;
	unsigned buildCompatibilityCrc;
	unsigned contentCrc;
	unsigned mapCrc;
	unsigned rosterMask;
	unsigned resolvedKernelMask;
	unsigned requestedWorkerCount;
	unsigned effectiveWorkerCount;
	unsigned physicalKernelMask;
	unsigned kernelSubmittedJobCount[MIXED_WORKER_LOOPBACK_KERNEL_COUNT];
	unsigned kernelCompletedJobCount[MIXED_WORKER_LOOPBACK_KERNEL_COUNT];
	unsigned kernelPhysicalJobCount[MIXED_WORKER_LOOPBACK_KERNEL_COUNT];
	unsigned kernelPhysicalWorkerMask[MIXED_WORKER_LOOPBACK_KERNEL_COUNT];
	unsigned kernelDistinctPhysicalWorkerCount[
		MIXED_WORKER_LOOPBACK_KERNEL_COUNT];
	unsigned canonicalTraceCrc;
	int processExitCode;
	bool handshakeReady;
	bool cleanShutdown;
};

unsigned GetMixedWorkerLoopbackSeed(unsigned seedIndex);
bool GetMixedWorkerLoopbackCase(unsigned caseIndex,
	MixedWorkerLoopbackCase &output);
unsigned GetMixedWorkerLoopbackEvidenceRecordCount();

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
	unsigned expectedKernelMask);

} // namespace rts
