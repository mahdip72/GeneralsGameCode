/*
** Command & Conquer Generals(tm)
** Copyright 2026 TheSuperHackers
**
** This program is free software: you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation, either version 3 of the License, or
** (at your option) any later version.
*/

#include "Lib/DeterministicCrcStream.h"

#include <stdio.h>
#include <string.h>

#if defined(_MSC_VER) && _MSC_VER < 1300
// VC6 reports optimizer-selected inline expansion as C4711 at /W4. It is
// informational and otherwise trips this test target's intentional /WX gate.
#pragma warning(disable : 4711)
#endif

namespace
{

int Check(bool condition, const char *message)
{
	if (condition) return 0;
	fprintf(stderr, "FAIL: %s\n", message);
	return 1;
}

rts::DeterministicCrcRangeKey MakeKey(uint32_t sectionId,
	uint32_t partitionId, uint64_t byteBegin, uint64_t byteEnd)
{
	rts::DeterministicCrcRangeKey key;
	key.sectionId = sectionId;
	key.partitionId = partitionId;
	key.byteBegin = byteBegin;
	key.byteEnd = byteEnd;
	return key;
}

uint32_t ReferenceCrc32(const uint8_t *bytes, size_t byteCount)
{
	uint32_t crc = 0xffffffffU;
	size_t index;
	for (index = 0U; index < byteCount; ++index)
	{
		unsigned bit;
		crc ^= bytes[index];
		for (bit = 0U; bit < 8U; ++bit)
		{
			const uint32_t mask = 0U - (crc & 1U);
			crc = (crc >> 1U) ^ (0xedb88320U & mask);
		}
	}
	return ~crc;
}

uint32_t ByteSwap32(uint32_t value)
{
	return ((value & 0x000000ffU) << 24U) |
		((value & 0x0000ff00U) << 8U) |
		((value & 0x00ff0000U) >> 8U) |
		((value & 0xff000000U) >> 24U);
}

void ReferenceLegacyEvent(uint32_t *state, const uint8_t *bytes,
	size_t byteCount)
{
	size_t index = 0U;
	while (byteCount - index >= 4U)
	{
		// XferCRC reads a native little-endian uint32, but addCRC applies
		// htobe() before mixing. This reference models that internal swap.
		const uint32_t value =
			(static_cast<uint32_t>(bytes[index + 0U]) << 24U) |
			(static_cast<uint32_t>(bytes[index + 1U]) << 16U) |
			(static_cast<uint32_t>(bytes[index + 2U]) << 8U) |
			static_cast<uint32_t>(bytes[index + 3U]);
		*state = (*state << 1U) + value + ((*state >> 31U) & 1U);
		index += 4U;
	}
	if (index != byteCount)
	{
		uint32_t value = 0U;
		unsigned shift = 0U;
		while (index != byteCount)
		{
			value |= static_cast<uint32_t>(bytes[index++]) << shift;
			shift += 8U;
		}
		*state = (*state << 1U) + value + ((*state >> 31U) & 1U);
	}
}

void WriteLittleEndian(uint8_t *destination, uint64_t value, unsigned width)
{
	unsigned index;
	for (index = 0U; index < width; ++index)
	{
		destination[index] = static_cast<uint8_t>(value & 0xffU);
		value >>= 8U;
	}
}

int TestLegacyOracleAndCallBoundaries()
{
	const uint8_t first[2] = {0x34U, 0x12U};
	const uint8_t second[4] = {0x78U, 0x56U, 0x34U, 0x12U};
	const uint8_t third[3] = {0xaaU, 0xbbU, 0xccU};
	const uint8_t joined[9] = {
		0x34U, 0x12U, 0x78U, 0x56U, 0x34U, 0x12U, 0xaaU, 0xbbU, 0xccU
	};
	const rts::DeterministicCrcRangeKey plan =
		MakeKey(0x01020304U, 0x05060708U, 0U, 9U);
	rts::DeterministicLegacyXferOperation operations[4];
	rts::DeterministicCrcPartitionResult partition;
	rts::DeterministicCrcPartitionCapture capture(&partition, operations, 4U);
	rts::DeterministicCrcFoldedChecksum legacy;
	rts::DeterministicCrcFoldedChecksum current;
	uint8_t currentStream[17];
	int result = 0;

	result |= Check(capture.begin(plan, 7U, 3U,
		rts::DETERMINISTIC_CRC_CAPTURE_BOTH, 0),
		"legacy oracle capture begins");
	result |= Check(capture.writeXferEvent(first, sizeof(first)) &&
		capture.writeXferEvent(second, sizeof(second)) &&
		capture.writeXferEvent(third, sizeof(third)) && capture.complete(),
		"legacy oracle preserves three Xfer event boundaries");
	result |= Check(partition.currentReplayChecksum == 0x41a7668cU &&
		partition.byteCount == 9U && partition.legacyOperationCount == 3U,
		"current payload CRC and explicit counts match the oracle bytes");
	result |= Check(rts::FoldDeterministicCrcRanges(&plan, 1U,
		&partition, 1U, 7U, rts::DETERMINISTIC_CRC_LEGACY_XFER,
		&legacy) == rts::DETERMINISTIC_CRC_FOLD_OK &&
		legacy.checksum == 0x9e6c79f1U &&
		legacy.payloadByteCount == 9U && legacy.hashedByteCount == 9U,
		"legacy fold matches the hard-coded XferCRC Windows oracle");

	WriteLittleEndian(currentStream + 0U, plan.sectionId, 4U);
	WriteLittleEndian(currentStream + 4U, plan.partitionId, 4U);
	memcpy(currentStream + 8U, joined, sizeof(joined));
	result |= Check(rts::FoldDeterministicCrcRanges(&plan, 1U,
		&partition, 1U, 7U, rts::DETERMINISTIC_CRC_CURRENT_REPLAY_EPOCH,
		&current) == rts::DETERMINISTIC_CRC_FOLD_OK &&
		current.checksum == 0x210f7ba0U &&
		current.checksum == ReferenceCrc32(currentStream, sizeof(currentStream)) &&
		current.payloadByteCount == 9U && current.hashedByteCount == 17U,
		"current replay fold hashes the committed 8-byte canonical partition key");

	// Coalescing the same bytes into one Xfer event must change only legacy.
	rts::DeterministicLegacyXferOperation joinedOperations[3];
	rts::DeterministicCrcPartitionResult joinedPartition;
	rts::DeterministicCrcPartitionCapture joinedCapture(&joinedPartition,
		joinedOperations, 3U);
	rts::DeterministicCrcFoldedChecksum joinedLegacy;
	rts::DeterministicCrcFoldedChecksum joinedCurrent;
	result |= Check(joinedCapture.begin(plan, 7U, 8U,
		rts::DETERMINISTIC_CRC_CAPTURE_BOTH, 0) &&
		joinedCapture.writeXferEvent(joined, sizeof(joined)) &&
		joinedCapture.complete(), "coalesced fixture captures");
	result |= Check(rts::FoldDeterministicCrcRanges(&plan, 1U,
		&joinedPartition, 1U, 7U, rts::DETERMINISTIC_CRC_LEGACY_XFER,
		&joinedLegacy) == rts::DETERMINISTIC_CRC_FOLD_OK &&
		joinedLegacy.checksum == 0x9b376f38U &&
		joinedLegacy.checksum != legacy.checksum,
		"legacy oracle proves Xfer call boundaries are contractual");
	result |= Check(rts::FoldDeterministicCrcRanges(&plan, 1U,
		&joinedPartition, 1U, 7U,
		rts::DETERMINISTIC_CRC_CURRENT_REPLAY_EPOCH, &joinedCurrent) ==
		rts::DETERMINISTIC_CRC_FOLD_OK &&
		joinedCurrent.checksum == current.checksum,
		"current replay CRC is a true byte stream independent of write grouping");

	rts::DeterministicCrcRangeKey changedPlan = plan;
	rts::DeterministicCrcPartitionResult changedPartition = partition;
	rts::DeterministicCrcFoldedChecksum changedLegacy;
	rts::DeterministicCrcFoldedChecksum changedCurrent;
	changedPlan.partitionId = 0x05060709U;
	changedPartition.key = changedPlan;
	result |= Check(rts::FoldDeterministicCrcRanges(&changedPlan, 1U,
		&changedPartition, 1U, 7U, rts::DETERMINISTIC_CRC_LEGACY_XFER,
		&changedLegacy) == rts::DETERMINISTIC_CRC_FOLD_OK &&
		changedLegacy.checksum == legacy.checksum,
		"legacy compatibility does not inject new range metadata");
	result |= Check(rts::FoldDeterministicCrcRanges(&changedPlan, 1U,
		&changedPartition, 1U, 7U,
		rts::DETERMINISTIC_CRC_CURRENT_REPLAY_EPOCH, &changedCurrent) ==
		rts::DETERMINISTIC_CRC_FOLD_OK &&
		changedCurrent.checksum != current.checksum,
		"current replay range identity participates in the checksum");
	return result;
}

int TestCurrentStandardVector()
{
	const uint8_t standard[9] = {
		'1', '2', '3', '4', '5', '6', '7', '8', '9'
	};
	const rts::DeterministicCrcRangeKey key = MakeKey(9U, 0U, 0U, 9U);
	rts::DeterministicCrcPartitionResult partition;
	rts::DeterministicCrcPartitionCapture capture(&partition, 0, 0U);
	return Check(capture.begin(key, 1U, 0U,
		rts::DETERMINISTIC_CRC_CAPTURE_CURRENT, 0) &&
		capture.writeXferEvent(standard, sizeof(standard)) &&
		capture.complete() && partition.currentReplayChecksum == 0xcbf43926U,
		"current partition CRC matches the CRC-32/ISO-HDLC standard vector");
}

int TestCanonicalWidthsAndEndian()
{
	const rts::DeterministicCrcRangeKey key = MakeKey(1U, 0U, 0U, 40U);
	rts::DeterministicLegacyXferOperation operations[12];
	rts::DeterministicCrcPartitionResult partition;
	rts::DeterministicCrcPartitionCapture capture(&partition, operations, 12U);
	uint8_t expected[40];
	uint32_t floatBits = 0x7fc01234U;
	uint64_t doubleBits = (static_cast<uint64_t>(0x80000000U) << 32U);
	float floatValue = 0.0F;
	double doubleValue = 0.0;
	int result = 0;

	memcpy(&floatValue, &floatBits, 4U);
	memcpy(&doubleValue, &doubleBits, 8U);
	expected[0] = 1U;
	expected[1] = 0xabU;
	WriteLittleEndian(expected + 2U, 0x1234U, 2U);
	WriteLittleEndian(expected + 4U, 0x89abcdefU, 4U);
	WriteLittleEndian(expected + 8U,
		(static_cast<uint64_t>(0x01234567U) << 32U) | 0x89abcdefU, 8U);
	WriteLittleEndian(expected + 16U, 0xfffffffeU, 4U);
	WriteLittleEndian(expected + 20U, UINT64_MAX - 2U, 8U);
	WriteLittleEndian(expected + 28U, floatBits, 4U);
	WriteLittleEndian(expected + 32U, doubleBits, 8U);

	result |= Check(capture.begin(key, 9U, 0U,
		rts::DETERMINISTIC_CRC_CAPTURE_BOTH, 0) &&
		capture.writeBool(true) && capture.writeUInt8(0xabU) &&
		capture.writeUInt16(0x1234U) && capture.writeUInt32(0x89abcdefU) &&
		capture.writeUInt64(
			(static_cast<uint64_t>(0x01234567U) << 32U) | 0x89abcdefU) &&
		capture.writeInt32(-2) && capture.writeInt64(-3) &&
		capture.writeFloat32Bits(floatValue) &&
		capture.writeFloat64Bits(doubleValue) && capture.complete(),
		"fixed-width typed fields capture without native layout");
	result |= Check(partition.byteCount == sizeof(expected) &&
		partition.currentReplayChecksum ==
			ReferenceCrc32(expected, sizeof(expected)),
		"typed fields use exact little-endian widths and IEEE bit patterns");
	return result;
}

int TestWorkerAndCompletionParity()
{
	const unsigned RANGE_COUNT = 16U;
	rts::DeterministicCrcRangeKey plan[RANGE_COUNT];
	uint8_t payload[RANGE_COUNT][8];
	size_t payloadSize[RANGE_COUNT];
	rts::DeterministicLegacyXferOperation storage[RANGE_COUNT][3];
	rts::DeterministicCrcPartitionResult slots[RANGE_COUNT];
	rts::DeterministicCrcPartitionResult completionOrder[RANGE_COUNT];
	const unsigned workerCounts[5] = {1U, 2U, 4U, 8U, 16U};
	uint32_t baselineLegacy = 0U;
	uint32_t baselineCurrent = 0U;
	uint32_t referenceLegacyState = 0U;
	uint8_t referenceCurrentStream[512];
	size_t referenceCurrentSize = 0U;
	uint64_t offset = 0U;
	unsigned index;
	unsigned run;
	int result = 0;

	for (index = 0U; index < RANGE_COUNT; ++index)
	{
		unsigned byteIndex;
		payloadSize[index] = 1U + (index % 7U);
		for (byteIndex = 0U; byteIndex < payloadSize[index]; ++byteIndex)
			payload[index][byteIndex] = static_cast<uint8_t>(
				index * 17U + byteIndex * 29U + 3U);
		plan[index] = MakeKey(77U, index, offset,
			offset + static_cast<uint64_t>(payloadSize[index]));
		offset = plan[index].byteEnd;
		ReferenceLegacyEvent(&referenceLegacyState, payload[index],
			payloadSize[index]);
		WriteLittleEndian(referenceCurrentStream + referenceCurrentSize,
			plan[index].sectionId, 4U);
		WriteLittleEndian(referenceCurrentStream + referenceCurrentSize + 4U,
			plan[index].partitionId, 4U);
		referenceCurrentSize +=
			rts::DETERMINISTIC_CRC_ENCODED_PARTITION_KEY_BYTES;
		memcpy(referenceCurrentStream + referenceCurrentSize, payload[index],
			payloadSize[index]);
		referenceCurrentSize += payloadSize[index];
	}

	for (run = 0U; run < 5U; ++run)
	{
		const unsigned workerCount = workerCounts[run];
		rts::DeterministicCrcFoldedChecksum legacy;
		rts::DeterministicCrcFoldedChecksum current;
		for (index = 0U; index < RANGE_COUNT; ++index)
		{
			rts::DeterministicCrcPartitionCapture capture(&slots[index],
				storage[index], 3U);
			const uint32_t physicalWorker =
				static_cast<uint32_t>((index * 5U + 3U) % workerCount);
			result |= Check(capture.begin(plan[index], 42U, physicalWorker,
				rts::DETERMINISTIC_CRC_CAPTURE_BOTH, 0) &&
				capture.writeXferEvent(payload[index], payloadSize[index]) &&
				capture.complete(), "worker parity partition captures");
		}
		// An odd affine permutation models a completion array unrelated to
		// logical range order.
		for (index = 0U; index < RANGE_COUNT; ++index)
			completionOrder[index] = slots[(index * 5U + 7U) % RANGE_COUNT];

		result |= Check(rts::FoldDeterministicCrcRanges(plan, RANGE_COUNT,
			completionOrder, RANGE_COUNT, 42U,
			rts::DETERMINISTIC_CRC_LEGACY_XFER, &legacy) ==
			rts::DETERMINISTIC_CRC_FOLD_OK,
			"legacy fold accepts reordered completion");
		result |= Check(rts::FoldDeterministicCrcRanges(plan, RANGE_COUNT,
			completionOrder, RANGE_COUNT, 42U,
			rts::DETERMINISTIC_CRC_CURRENT_REPLAY_EPOCH, &current) ==
			rts::DETERMINISTIC_CRC_FOLD_OK,
			"current fold accepts reordered completion");
		result |= Check(legacy.distinctPhysicalWorkerCount == workerCount &&
			legacy.physicalWorkerMask ==
				((static_cast<uint64_t>(1U) << workerCount) - 1U) &&
			legacy.physicalWorkerMaskComplete,
			"physical-worker provenance records the actual worker mapping");
		result |= Check(legacy.checksum == ByteSwap32(referenceLegacyState),
			"parallel legacy fold matches serial Xfer operation order");
		result |= Check(current.checksum == ReferenceCrc32(
			referenceCurrentStream, referenceCurrentSize),
			"partition combination matches one-shot key-framed current CRC");
		if (run == 0U)
		{
			baselineLegacy = legacy.checksum;
			baselineCurrent = current.checksum;
		}
		else
		{
			result |= Check(legacy.checksum == baselineLegacy &&
				current.checksum == baselineCurrent,
				"1/2/4/8/16 worker mappings preserve both checksum lanes");
		}
	}
	return result;
}

struct ControlFixture
{
	bool cancelled;
	uint32_t generation;
};

bool IsCancelled(void *context)
{
	return static_cast<ControlFixture *>(context)->cancelled;
}

uint32_t CurrentGeneration(void *context)
{
	return static_cast<ControlFixture *>(context)->generation;
}

int TestCancellationFailureStaleAndBounds()
{
	const rts::DeterministicCrcRangeKey key = MakeKey(2U, 0U, 0U, 4U);
	const uint8_t bytes[4] = {1U, 2U, 3U, 4U};
	ControlFixture fixture;
	rts::DeterministicCrcCaptureControl control;
	rts::DeterministicLegacyXferOperation storage[2];
	rts::DeterministicCrcPartitionResult partition;
	rts::DeterministicCrcPartitionCapture capture(&partition, storage, 2U);
	rts::DeterministicCrcFoldedChecksum output;
	int result = 0;
	fixture.cancelled = false;
	fixture.generation = 5U;
	control.isCancelled = IsCancelled;
	control.currentGeneration = CurrentGeneration;
	control.context = &fixture;

	// A second begin on a live capture must not erase already-produced data,
	// replace its identity/provenance, or change its control contract.
	rts::DeterministicLegacyXferOperation activeStorage[1];
	rts::DeterministicCrcPartitionResult activePartition;
	rts::DeterministicCrcPartitionCapture activeCapture(&activePartition,
		activeStorage, 1U);
	const rts::DeterministicCrcRangeKey replacementKey =
		MakeKey(99U, 88U, 0U, 4U);
	result |= Check(activeCapture.begin(key, 4U, 7U,
		rts::DETERMINISTIC_CRC_CAPTURE_BOTH, 0) &&
		activeCapture.writeXferEvent(bytes, sizeof(bytes)),
		"active-begin fixture captures before re-entry");
	result |= Check(!activeCapture.begin(replacementKey, 99U, 12U,
		rts::DETERMINISTIC_CRC_CAPTURE_CURRENT, &control) &&
		activePartition.key.sectionId == key.sectionId &&
		activePartition.key.partitionId == key.partitionId &&
		activePartition.key.byteBegin == key.byteBegin &&
		activePartition.key.byteEnd == key.byteEnd &&
		activePartition.generation == 4U &&
		activePartition.physicalWorkerId == 7U &&
		activePartition.capturedLanes ==
			rts::DETERMINISTIC_CRC_CAPTURE_BOTH &&
		activePartition.status == rts::DETERMINISTIC_CRC_CAPTURE_WRITING &&
		activePartition.error == rts::DETERMINISTIC_CRC_CAPTURE_ERROR_NONE &&
		activePartition.byteCount == 4U &&
		activePartition.legacyOperations == activeStorage &&
		activePartition.legacyOperationCapacity == 1U &&
		activePartition.legacyOperationCount == 1U &&
		activeStorage[0].value == 0x01020304U && activeCapture.complete(),
		"begin rejects active WRITING without mutating capture state");

	result |= Check(capture.begin(key, 5U, 1U,
		rts::DETERMINISTIC_CRC_CAPTURE_BOTH, &control),
		"controlled capture begins at the expected generation");
	fixture.cancelled = true;
	result |= Check(!capture.writeXferEvent(bytes, sizeof(bytes)) &&
		partition.status == rts::DETERMINISTIC_CRC_CAPTURE_CANCELLED &&
		rts::FoldDeterministicCrcRanges(&key, 1U, &partition, 1U, 5U,
			rts::DETERMINISTIC_CRC_LEGACY_XFER, &output) ==
			rts::DETERMINISTIC_CRC_FOLD_PARTITION_CANCELLED,
		"cancellation prevents capture and rejects owner fold");

	fixture.cancelled = false;
	fixture.generation = 6U;
	result |= Check(capture.begin(key, 6U, 1U,
		rts::DETERMINISTIC_CRC_CAPTURE_BOTH, &control),
		"capture storage is reusable in a later generation");
	fixture.generation = 7U;
	result |= Check(!capture.complete() &&
		partition.status == rts::DETERMINISTIC_CRC_CAPTURE_STALE &&
		rts::FoldDeterministicCrcRanges(&key, 1U, &partition, 1U, 6U,
			rts::DETERMINISTIC_CRC_LEGACY_XFER, &output) ==
			rts::DETERMINISTIC_CRC_FOLD_STALE_GENERATION,
		"generation changes fail stale before publication");

	fixture.generation = 8U;
	result |= Check(capture.begin(key, 8U, 2U,
		rts::DETERMINISTIC_CRC_CAPTURE_BOTH, &control),
		"caller-failure fixture begins");
	capture.fail();
	result |= Check(partition.status == rts::DETERMINISTIC_CRC_CAPTURE_FAILED &&
		partition.error == rts::DETERMINISTIC_CRC_CAPTURE_ERROR_CALLER_FAILURE &&
		rts::FoldDeterministicCrcRanges(&key, 1U, &partition, 1U, 8U,
			rts::DETERMINISTIC_CRC_LEGACY_XFER, &output) ==
			rts::DETERMINISTIC_CRC_FOLD_PARTITION_FAILED,
		"explicit worker failure rejects the whole wave");

	rts::DeterministicLegacyXferOperation oneOperation[1];
	rts::DeterministicCrcPartitionResult overflowPartition;
	rts::DeterministicCrcPartitionCapture overflow(&overflowPartition,
		oneOperation, 1U);
	const rts::DeterministicCrcRangeKey eightByteKey =
		MakeKey(3U, 0U, 0U, 8U);
	const uint8_t eightBytes[8] = {0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U};
	result |= Check(overflow.begin(eightByteKey, 9U, 0U,
		rts::DETERMINISTIC_CRC_CAPTURE_BOTH, 0) &&
		!overflow.writeXferEvent(eightBytes, sizeof(eightBytes)) &&
		overflowPartition.error ==
			rts::DETERMINISTIC_CRC_CAPTURE_ERROR_STORAGE_OVERFLOW &&
		overflowPartition.byteCount == 0U &&
		overflowPartition.legacyOperationCount == 0U,
		"bounded legacy storage fails atomically before partial capture");

	rts::DeterministicCrcPartitionResult shortPartition;
	rts::DeterministicCrcPartitionCapture shortCapture(&shortPartition,
		storage, 2U);
	result |= Check(shortCapture.begin(key, 10U, 0U,
		rts::DETERMINISTIC_CRC_CAPTURE_BOTH, 0) &&
		shortCapture.writeXferEvent(bytes, 3U) && !shortCapture.complete() &&
		shortPartition.error ==
			rts::DETERMINISTIC_CRC_CAPTURE_ERROR_RANGE_SIZE_MISMATCH,
		"declared range byte count must match captured payload exactly");
	return result;
}

int TestPlanAndResultRejections()
{
	const uint8_t firstBytes[2] = {0x10U, 0x20U};
	const uint8_t secondBytes[2] = {0x30U, 0x40U};
	rts::DeterministicCrcRangeKey plan[2];
	rts::DeterministicLegacyXferOperation storage[2][1];
	rts::DeterministicCrcPartitionResult partitions[2];
	rts::DeterministicCrcPartitionResult malformed[2];
	rts::DeterministicCrcFoldedChecksum output;
	int result = 0;
	unsigned index;

	plan[0] = MakeKey(4U, 0U, 0U, 2U);
	plan[1] = MakeKey(4U, 1U, 2U, 4U);
	for (index = 0U; index < 2U; ++index)
	{
		rts::DeterministicCrcPartitionCapture capture(&partitions[index],
			storage[index], 1U);
		const uint8_t *bytes = index == 0U ? firstBytes : secondBytes;
		result |= Check(capture.begin(plan[index], 11U, index,
			rts::DETERMINISTIC_CRC_CAPTURE_BOTH, 0) &&
			capture.writeXferEvent(bytes, 2U) && capture.complete(),
			"rejection fixture partition captures");
	}

	output.checksum = 0x11223344U;
	result |= Check(rts::FoldDeterministicCrcRanges(plan, 2U,
		partitions, 1U, 11U, rts::DETERMINISTIC_CRC_LEGACY_XFER,
		&output) == rts::DETERMINISTIC_CRC_FOLD_MISSING_PARTITION &&
		output.checksum == 0x11223344U,
		"missing partitions reject without partial output publication");

	malformed[0] = partitions[0];
	malformed[1] = partitions[0];
	result |= Check(rts::FoldDeterministicCrcRanges(plan, 2U,
		malformed, 2U, 11U, rts::DETERMINISTIC_CRC_LEGACY_XFER,
		&output) == rts::DETERMINISTIC_CRC_FOLD_DUPLICATE_PARTITION,
		"duplicate completed partitions are rejected");

	malformed[0] = partitions[0];
	malformed[1] = partitions[1];
	malformed[1].key.partitionId = 9U;
	result |= Check(rts::FoldDeterministicCrcRanges(plan, 2U,
		malformed, 2U, 11U, rts::DETERMINISTIC_CRC_LEGACY_XFER,
		&output) == rts::DETERMINISTIC_CRC_FOLD_UNEXPECTED_PARTITION,
		"unexpected range identities cannot replace expected partitions");

	rts::DeterministicCrcRangeKey invalidPlan[2];
	invalidPlan[0] = plan[0];
	invalidPlan[1] = MakeKey(4U, 1U, 1U, 4U);
	result |= Check(rts::FoldDeterministicCrcRanges(invalidPlan, 2U,
		partitions, 2U, 11U, rts::DETERMINISTIC_CRC_LEGACY_XFER,
		&output) == rts::DETERMINISTIC_CRC_FOLD_OVERLAPPING_RANGES,
		"overlapping logical ranges are rejected");
	invalidPlan[1] = MakeKey(4U, 1U, 3U, 5U);
	result |= Check(rts::FoldDeterministicCrcRanges(invalidPlan, 2U,
		partitions, 2U, 11U, rts::DETERMINISTIC_CRC_LEGACY_XFER,
		&output) == rts::DETERMINISTIC_CRC_FOLD_GAPPED_RANGES,
		"gapped logical ranges are rejected");
	invalidPlan[1] = MakeKey(4U, 0U, 2U, 4U);
	result |= Check(rts::FoldDeterministicCrcRanges(invalidPlan, 2U,
		partitions, 2U, 11U, rts::DETERMINISTIC_CRC_LEGACY_XFER,
		&output) == rts::DETERMINISTIC_CRC_FOLD_DUPLICATE_PLAN_KEY,
		"duplicate title-owned section/range keys are rejected");

	malformed[0] = partitions[0];
	malformed[1] = partitions[1];
	malformed[1].generation = 12U;
	result |= Check(rts::FoldDeterministicCrcRanges(plan, 2U,
		malformed, 2U, 11U, rts::DETERMINISTIC_CRC_LEGACY_XFER,
		&output) == rts::DETERMINISTIC_CRC_FOLD_STALE_GENERATION,
		"stale completed generations are rejected");
	malformed[1] = partitions[1];
	malformed[1].byteCount = 1U;
	result |= Check(rts::FoldDeterministicCrcRanges(plan, 2U,
		malformed, 2U, 11U, rts::DETERMINISTIC_CRC_LEGACY_XFER,
		&output) == rts::DETERMINISTIC_CRC_FOLD_BYTE_COUNT_MISMATCH,
		"completed payload byte counts must match declared ranges");

	malformed[1] = partitions[1];
	malformed[1].legacyOperationCount =
		malformed[1].legacyOperationCapacity + 1U;
	result |= Check(malformed[1].legacyOperationCapacity == 1U &&
		rts::FoldDeterministicCrcRanges(plan, 2U, malformed, 2U, 11U,
			rts::DETERMINISTIC_CRC_LEGACY_XFER, &output) ==
			rts::DETERMINISTIC_CRC_FOLD_LEGACY_STREAM_MALFORMED,
		"mutated legacy operation counts above captured capacity reject before dereference");

	malformed[1] = partitions[1];
	malformed[1].error =
		rts::DETERMINISTIC_CRC_CAPTURE_ERROR_CALLER_FAILURE;
	result |= Check(rts::FoldDeterministicCrcRanges(plan, 2U,
		malformed, 2U, 11U, rts::DETERMINISTIC_CRC_LEGACY_XFER,
		&output) == rts::DETERMINISTIC_CRC_FOLD_INVALID_CAPTURE_METADATA,
		"COMPLETE results with non-NONE errors are rejected");

	malformed[1] = partitions[1];
	malformed[1].capturedLanes =
		static_cast<uint32_t>(rts::DETERMINISTIC_CRC_CAPTURE_BOTH) | 4U;
	result |= Check(rts::FoldDeterministicCrcRanges(plan, 2U,
		malformed, 2U, 11U, rts::DETERMINISTIC_CRC_LEGACY_XFER,
		&output) == rts::DETERMINISTIC_CRC_FOLD_INVALID_CAPTURE_METADATA,
		"COMPLETE results require an exact known captured-lane mask");

	rts::DeterministicCrcPartitionResult currentOnly;
	rts::DeterministicCrcPartitionCapture currentOnlyCapture(&currentOnly, 0, 0U);
	result |= Check(currentOnlyCapture.begin(plan[0], 11U, 0U,
		rts::DETERMINISTIC_CRC_CAPTURE_CURRENT, 0) &&
		currentOnlyCapture.writeXferEvent(firstBytes, 2U) &&
		currentOnlyCapture.complete() &&
		rts::FoldDeterministicCrcRanges(plan, 1U, &currentOnly, 1U, 11U,
			rts::DETERMINISTIC_CRC_LEGACY_XFER, &output) ==
			rts::DETERMINISTIC_CRC_FOLD_CAPTURE_LANE_MISSING,
		"an explicit current-only capture cannot impersonate legacy");
	return result;
}

} // namespace

int main()
{
	int result = 0;
	result |= TestLegacyOracleAndCallBoundaries();
	result |= TestCurrentStandardVector();
	result |= TestCanonicalWidthsAndEndian();
	result |= TestWorkerAndCompletionParity();
	result |= TestCancellationFailureStaleAndBounds();
	result |= TestPlanAndResultRejections();
	if (result == 0)
		printf("Deterministic CRC stream tests passed.\n");
	return result;
}
