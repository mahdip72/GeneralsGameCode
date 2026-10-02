/*
** Command & Conquer Generals(tm)
** Copyright 2026 TheSuperHackers
**
** This program is free software: you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation, either version 3 of the License, or
** (at your option) any later version.
*/

#include "Utility/CppMacros.h"

#include "Common/DeterministicCrcLiveVerifier.h"
#include "XferCrcSnapshotTest.h"

#include <stdio.h>

namespace
{

int Check(bool condition, const char *message)
{
	if (condition) return 0;
	fprintf(stderr, "FAIL: %s\n", message);
	return 1;
}

struct FixedGrowthStorage
{
	uint8_t replacement[256];
	bool used;
};

bool GrowIntoFixedStorage(rts::DeterministicCrcSnapshotEncoder *encoder,
	size_t requiredStorageCapacity, void *context)
{
	FixedGrowthStorage *storage = static_cast<FixedGrowthStorage *>(context);
	if (encoder == 0 || storage == 0 ||
		requiredStorageCapacity > sizeof(storage->replacement))
	{
		return false;
	}
	storage->used = encoder->rebindStorage(storage->replacement,
		sizeof(storage->replacement));
	return storage->used;
}

int TestSerialOracleAndSnapshotStayIdentical()
{
	uint8_t snapshotStorage[128];
	rts::DeterministicCrcSnapshotEncoder encoder;
	XferCRCSnapshot xfer;
	const void *immutableInput = 0;
	size_t immutableInputBytes = 0U;
	UnsignedInt first = 0x11223344U;
	UnsignedShort second = 0x5566U;
	const uint8_t tail[3] = { 0xaaU, 0xbbU, 0xccU };
	rts::DeterministicCrcRangeKey range;
	rts::DeterministicLegacyXferOperation legacyStorage[3];
	rts::DeterministicCrcPartitionResult partition;
	rts::DeterministicCrcPartitionCapture capture(&partition,
		legacyStorage, 3U);
	rts::DeterministicCrcFoldedChecksum folded;
	int result = 0;

	xfer.open("snapshot-test");
	result |= Check(xfer.beginSnapshotPartition(&encoder, snapshotStorage,
		sizeof(snapshotStorage)), "owner begins one Xfer CRC snapshot partition");
	xfer.xferUnsignedInt(&first);
	xfer.xferUnsignedShort(&second);
	xfer.xferUser(const_cast<uint8_t *>(tail), sizeof(tail));
	result |= Check(xfer.endSnapshotPartition(9U, &immutableInput,
		&immutableInputBytes), "owner seals the exact Xfer event stream");
	xfer.close();
	const UnsignedInt serialOracle = xfer.getCRC();

	range.sectionId = 1U;
	range.partitionId = 0U;
	range.byteBegin = 0U;
	range.byteEnd = 9U;
	result |= Check(capture.begin(range, 1U, 0U,
		rts::DETERMINISTIC_CRC_CAPTURE_LEGACY, 0) &&
		rts::CaptureDeterministicCrcSnapshot(immutableInput,
			immutableInputBytes, &capture) && capture.complete(),
		"worker capture replays the sealed partition");
	result |= Check(rts::FoldDeterministicCrcRanges(&range, 1U, &partition,
		1U, 1U, rts::DETERMINISTIC_CRC_LEGACY_XFER, &folded) ==
		rts::DETERMINISTIC_CRC_FOLD_OK && folded.checksum == serialOracle,
		"parallel fold is byte-identical to the unchanged XferCRC oracle");
	return result;
}

int TestSnapshotOverflowDoesNotChangeSerialOracle()
{
	uint8_t tinyStorage[
		rts::DETERMINISTIC_CRC_SNAPSHOT_HEADER_BYTES + 4U];
	rts::DeterministicCrcSnapshotEncoder encoder;
	XferCRCSnapshot tee;
	XferCRC oracle;
	UnsignedInt value = 0xaabbccddU;
	const void *immutableInput = reinterpret_cast<const void *>(1);
	size_t immutableInputBytes = 99U;
	int result = 0;

	tee.open("tee");
	oracle.open("oracle");
	result |= Check(tee.beginSnapshotPartition(&encoder, tinyStorage,
		sizeof(tinyStorage)), "undersized snapshot partition still begins");
	tee.xferUnsignedInt(&value);
	oracle.xferUnsignedInt(&value);
	result |= Check(tee.snapshotPartitionFailed() &&
		!tee.endSnapshotPartition(4U, &immutableInput,
			&immutableInputBytes) && immutableInput == 0 &&
		immutableInputBytes == 0U,
		"snapshot overflow fails closed without publication");
	tee.close();
	oracle.close();
	result |= Check(tee.getCRC() == oracle.getCRC(),
		"snapshot overflow leaves the serial XferCRC oracle unchanged");
	return result;
}

int TestOwnerGrowthPreservesSerialOracleAndSnapshot()
{
	uint8_t tinyStorage[
		rts::DETERMINISTIC_CRC_SNAPSHOT_HEADER_BYTES + 4U];
	FixedGrowthStorage growth = { { 0 }, false };
	rts::DeterministicCrcSnapshotEncoder encoder;
	XferCRCSnapshot tee;
	XferCRC oracle;
	UnsignedInt first = 0x11223344U;
	UnsignedInt second = 0x55667788U;
	const void *immutableInput = 0;
	size_t immutableInputBytes = 0U;
	rts::DeterministicLegacyXferOperation legacyStorage[2];
	rts::DeterministicCrcPartitionResult partition;
	rts::DeterministicCrcPartitionCapture capture(&partition,
		legacyStorage, 2U);
	rts::DeterministicCrcRangeKey range;
	rts::DeterministicCrcFoldedChecksum folded;
	int result = 0;

	tee.open("growth-tee");
	oracle.open("growth-oracle");
	result |= Check(tee.beginSnapshotPartition(&encoder, tinyStorage,
		sizeof(tinyStorage), &GrowIntoFixedStorage, &growth),
		"owner begins a growable snapshot partition");
	tee.xferUnsignedInt(&first);
	tee.xferUnsignedInt(&second);
	oracle.xferUnsignedInt(&first);
	oracle.xferUnsignedInt(&second);
	result |= Check(growth.used && tee.endSnapshotPartition(8U,
		&immutableInput, &immutableInputBytes),
		"owner growth seals the complete immutable event stream");
	tee.close();
	oracle.close();

	range.sectionId = 1U;
	range.partitionId = 0U;
	range.byteBegin = 0U;
	range.byteEnd = 8U;
	result |= Check(capture.begin(range, 1U, 0U,
		rts::DETERMINISTIC_CRC_CAPTURE_LEGACY, 0) &&
		rts::CaptureDeterministicCrcSnapshot(immutableInput,
			immutableInputBytes, &capture) && capture.complete() &&
		rts::FoldDeterministicCrcRanges(&range, 1U, &partition, 1U,
			1U, rts::DETERMINISTIC_CRC_LEGACY_XFER, &folded) ==
			rts::DETERMINISTIC_CRC_FOLD_OK &&
		folded.checksum == oracle.getCRC() && tee.getCRC() == oracle.getCRC(),
		"grown snapshot fold remains the exact serial XferCRC oracle");
	return result;
}

int TestCanonicalPartitionsFoldAsOneLegacyStream()
{
	enum { PARTITIONS = rts::DETERMINISTIC_CRC_LIVE_PARTITION_COUNT };
	uint8_t snapshotStorage[PARTITIONS][256];
	rts::DeterministicCrcSnapshotEncoder encoders[PARTITIONS];
	const void *immutableInput[PARTITIONS] = { 0 };
	size_t immutableInputBytes[PARTITIONS] = { 0 };
	rts::DeterministicLegacyXferOperation legacyStorage[PARTITIONS][64];
	rts::DeterministicCrcPartitionResult partitions[PARTITIONS];
	rts::DeterministicCrcRangeKey plan[PARTITIONS];
	rts::DeterministicCrcFoldedChecksum folded;
	XferCRCSnapshot xfer;
	AsciiString marker;
	UnsignedInt objects[2] = { 0x01020304U, 0x11121314U };
	UnsignedInt seed = 0x21222324U;
	UnsignedInt partitionState = 0x31323334U;
	UnsignedInt playerState = 0x41424344U;
	UnsignedInt aiState = 0x51525354U;
	int result = 0;

	xfer.open("canonical-live-crc");
	result |= Check(xfer.beginSnapshotPartition(
		&encoders[rts::DETERMINISTIC_CRC_LIVE_OBJECTS],
		snapshotStorage[rts::DETERMINISTIC_CRC_LIVE_OBJECTS], 256U),
		"objects partition begins in canonical position");
	marker = "MARKER:Objects";
	xfer.xferAsciiString(&marker);
	xfer.xferUnsignedInt(&objects[0]);
	xfer.xferUnsignedInt(&objects[1]);
	result |= Check(xfer.endSnapshotPartition(
		encoders[rts::DETERMINISTIC_CRC_LIVE_OBJECTS].payloadByteCount(),
		&immutableInput[rts::DETERMINISTIC_CRC_LIVE_OBJECTS],
		&immutableInputBytes[rts::DETERMINISTIC_CRC_LIVE_OBJECTS]),
		"objects partition seals without changing event boundaries");

	result |= Check(xfer.beginSnapshotPartition(
		&encoders[rts::DETERMINISTIC_CRC_LIVE_RANDOM_SEED],
		snapshotStorage[rts::DETERMINISTIC_CRC_LIVE_RANDOM_SEED], 256U),
		"random-seed partition begins after objects");
	xfer.xferUnsignedInt(&seed);
	result |= Check(xfer.endSnapshotPartition(
		encoders[rts::DETERMINISTIC_CRC_LIVE_RANDOM_SEED].payloadByteCount(),
		&immutableInput[rts::DETERMINISTIC_CRC_LIVE_RANDOM_SEED],
		&immutableInputBytes[rts::DETERMINISTIC_CRC_LIVE_RANDOM_SEED]),
		"random-seed partition seals");

	result |= Check(xfer.beginSnapshotPartition(
		&encoders[rts::DETERMINISTIC_CRC_LIVE_PARTITION_MANAGER],
		snapshotStorage[rts::DETERMINISTIC_CRC_LIVE_PARTITION_MANAGER], 256U),
		"partition-manager partition begins after seed");
	marker = "MARKER:ThePartitionManager";
	xfer.xferAsciiString(&marker);
	xfer.xferUnsignedInt(&partitionState);
	result |= Check(xfer.endSnapshotPartition(
		encoders[rts::DETERMINISTIC_CRC_LIVE_PARTITION_MANAGER].payloadByteCount(),
		&immutableInput[rts::DETERMINISTIC_CRC_LIVE_PARTITION_MANAGER],
		&immutableInputBytes[rts::DETERMINISTIC_CRC_LIVE_PARTITION_MANAGER]),
		"partition-manager partition seals");

	result |= Check(xfer.beginSnapshotPartition(
		&encoders[rts::DETERMINISTIC_CRC_LIVE_PLAYER_LIST],
		snapshotStorage[rts::DETERMINISTIC_CRC_LIVE_PLAYER_LIST], 256U),
		"player-list partition begins after partition manager");
	marker = "MARKER:ThePlayerList";
	xfer.xferAsciiString(&marker);
	xfer.xferUnsignedInt(&playerState);
	result |= Check(xfer.endSnapshotPartition(
		encoders[rts::DETERMINISTIC_CRC_LIVE_PLAYER_LIST].payloadByteCount(),
		&immutableInput[rts::DETERMINISTIC_CRC_LIVE_PLAYER_LIST],
		&immutableInputBytes[rts::DETERMINISTIC_CRC_LIVE_PLAYER_LIST]),
		"player-list partition seals");

	result |= Check(xfer.beginSnapshotPartition(
		&encoders[rts::DETERMINISTIC_CRC_LIVE_AI],
		snapshotStorage[rts::DETERMINISTIC_CRC_LIVE_AI], 256U),
		"AI partition begins after player list");
	marker = "MARKER:TheAI";
	xfer.xferAsciiString(&marker);
	xfer.xferUnsignedInt(&aiState);
	result |= Check(xfer.endSnapshotPartition(
		encoders[rts::DETERMINISTIC_CRC_LIVE_AI].payloadByteCount(),
		&immutableInput[rts::DETERMINISTIC_CRC_LIVE_AI],
		&immutableInputBytes[rts::DETERMINISTIC_CRC_LIVE_AI]),
		"AI partition seals last");
	xfer.close();
	const UnsignedInt serialOracle = xfer.getCRC();

	for (size_t index = 0U; index < PARTITIONS; ++index)
	{
		plan[index].sectionId = static_cast<uint32_t>(index + 1U);
		plan[index].partitionId = 0U;
		plan[index].byteBegin = 0U;
		plan[index].byteEnd = encoders[index].payloadByteCount();
		rts::DeterministicCrcPartitionCapture capture(&partitions[index],
			legacyStorage[index], 64U);
		result |= Check(capture.begin(plan[index], 7U,
			static_cast<uint32_t>(index),
			rts::DETERMINISTIC_CRC_CAPTURE_LEGACY, 0) &&
			rts::CaptureDeterministicCrcSnapshot(immutableInput[index],
				immutableInputBytes[index], &capture) && capture.complete(),
			"worker captures one exact canonical section stream");
	}
	result |= Check(rts::FoldDeterministicCrcRanges(plan, PARTITIONS,
		partitions, PARTITIONS, 7U, rts::DETERMINISTIC_CRC_LEGACY_XFER,
		&folded) == rts::DETERMINISTIC_CRC_FOLD_OK &&
		folded.checksum == serialOracle,
		"fixed-order fold equals the one concatenated legacy XferCRC stream");
	return result;
}

int TestSerialModeLeavesLiveVerificationDormant()
{
	rts::ResetDeterministicCrcLiveMetrics();
	rts::DeterministicCrcLiveVerifier verifier;
	const bool began = verifier.begin(rts::SIMULATION_EXECUTION_SERIAL);
	const rts::DeterministicCrcLiveMetrics metrics =
		rts::GetDeterministicCrcLiveMetrics();
	return Check(!began && !verifier.isActive() && metrics.resetEpoch != 0 &&
		metrics.attempts == 0 && metrics.verifiedWaves == 0 &&
		metrics.fallbackWaves == 0 && metrics.executedWaves == 0 &&
		metrics.oracleMatches == 0 &&
		metrics.maximumDistinctPhysicalWorkerCount == 0U &&
		metrics.physicalWorkerMask == 0U &&
		metrics.physicalWorkerMaskComplete,
		"serial mode leaves the exact legacy CRC path and metrics dormant");
}

int TestLiveVerificationRequiresCurrentReplayEpoch()
{
#if defined(RTS_GENERALS) && RTS_GENERALS
	return Check(
		rts::IsDeterministicCrcLiveEpochEligible(false, false, false) &&
		!rts::IsDeterministicCrcLiveEpochEligible(true, false, true) &&
		rts::IsDeterministicCrcLiveEpochEligible(true, true, false) &&
		rts::IsDeterministicCrcLiveEpochEligible(true, true, true),
		"Generals admits fresh games and only deterministic-planning replays");
#else
	return Check(
		rts::IsDeterministicCrcLiveEpochEligible(false, false, false) &&
		!rts::IsDeterministicCrcLiveEpochEligible(true, false, false) &&
		!rts::IsDeterministicCrcLiveEpochEligible(true, true, false) &&
		!rts::IsDeterministicCrcLiveEpochEligible(true, false, true) &&
		rts::IsDeterministicCrcLiveEpochEligible(true, true, true),
		"Zero Hour requires both counter-RNG and path-queue replay epochs");
#endif
}

} // namespace

int RunXferCrcSnapshotTests()
{
	int result = 0;
	result |= TestSerialOracleAndSnapshotStayIdentical();
	result |= TestSnapshotOverflowDoesNotChangeSerialOracle();
	result |= TestOwnerGrowthPreservesSerialOracleAndSnapshot();
	result |= TestCanonicalPartitionsFoldAsOneLegacyStream();
	result |= TestSerialModeLeavesLiveVerificationDormant();
	result |= TestLiveVerificationRequiresCurrentReplayEpoch();
	if (result == 0) printf("Xfer CRC snapshot tests passed.\n");
	return result;
}
