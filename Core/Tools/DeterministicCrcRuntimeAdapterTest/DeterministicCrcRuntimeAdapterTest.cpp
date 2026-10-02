/*
** Command & Conquer Generals(tm)
** Copyright 2026 TheSuperHackers
**
** This program is free software: you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation, either version 3 of the License, or
** (at your option) any later version.
*/

#include "Lib/DeterministicCrcRuntimeAdapter.h"

#include <stdio.h>
#include <string.h>

namespace
{

enum
{
	PARTITION_COUNT = 16,
	PARTITIONS_PER_SECTION = 4,
	PARTITION_BYTES = 9,
	LEGACY_OPERATIONS_PER_PARTITION = 3
};

struct OwnerFixture
{
	bool owner;
	bool cancelled;
	uint32_t generation;
};

struct PartitionPayload
{
	uint32_t first;
	uint16_t second;
	uint8_t tail[3];
};

OwnerFixture *g_faultControl = 0;

int Check(bool condition, const char *message)
{
	if (condition) return 0;
	fprintf(stderr, "FAIL: %s\n", message);
	return 1;
}
bool IsOwner(void *context)
{
	return static_cast<OwnerFixture *>(context)->owner;
}

bool IsCancelled(void *context)
{
	return static_cast<OwnerFixture *>(context)->cancelled;
}

uint32_t CurrentGeneration(void *context)
{
	return static_cast<OwnerFixture *>(context)->generation;
}

bool CapturePartition(const void *immutableInput,
	size_t immutableInputBytes,
	rts::DeterministicCrcPartitionCapture *output)
{
	const PartitionPayload *payload =
		static_cast<const PartitionPayload *>(immutableInput);
	return immutableInputBytes == sizeof(PartitionPayload) &&
		payload != 0 && output != 0 &&
		output->writeUInt32(payload->first) &&
		output->writeUInt16(payload->second) &&
		output->writeXferEvent(payload->tail, sizeof(payload->tail));
}

bool CaptureFailure(const void *, size_t,
	rts::DeterministicCrcPartitionCapture *)
{
	return false;
}

bool CapturePrematureComplete(const void *immutableInput,
	size_t immutableInputBytes,
	rts::DeterministicCrcPartitionCapture *output)
{
	return CapturePartition(immutableInput, immutableInputBytes, output) &&
		output->complete();
}

bool CaptureCancellation(const void *immutableInput,
	size_t immutableInputBytes,
	rts::DeterministicCrcPartitionCapture *output)
{
	const PartitionPayload *payload =
		static_cast<const PartitionPayload *>(immutableInput);
	if (immutableInputBytes != sizeof(PartitionPayload) ||
		payload == 0 || output == 0 || g_faultControl == 0 ||
		!output->writeUInt32(payload->first))
	{
		return false;
	}
	g_faultControl->cancelled = true;
	return output->writeUInt16(payload->second) &&
		output->writeXferEvent(payload->tail, sizeof(payload->tail));
}

bool CaptureStaleGeneration(const void *immutableInput,
	size_t immutableInputBytes,
	rts::DeterministicCrcPartitionCapture *output)
{
	const PartitionPayload *payload =
		static_cast<const PartitionPayload *>(immutableInput);
	if (immutableInputBytes != sizeof(PartitionPayload) ||
		payload == 0 || output == 0 || g_faultControl == 0 ||
		!output->writeUInt32(payload->first))
	{
		return false;
	}
	++g_faultControl->generation;
	return output->writeUInt16(payload->second) &&
		output->writeXferEvent(payload->tail, sizeof(payload->tail));
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

void ReferenceLegacyEvent(uint32_t *state, const uint8_t *bytes,
	size_t byteCount)
{
	size_t index = 0U;
	while (byteCount - index >= 4U)
	{
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

uint32_t ByteSwap32(uint32_t value)
{
	return ((value & 0x000000ffU) << 24U) |
		((value & 0x0000ff00U) << 8U) |
		((value & 0x00ff0000U) >> 8U) |
		((value & 0xff000000U) >> 24U);
}

uint32_t ReferenceLegacyChecksum(const PartitionPayload *payloads,
	size_t payloadCount)
{
	uint32_t state = 0U;
	size_t index;
	for (index = 0U; index < payloadCount; ++index)
	{
		uint8_t first[4];
		uint8_t second[2];
		WriteLittleEndian(first, payloads[index].first, 4U);
		WriteLittleEndian(second, payloads[index].second, 2U);
		ReferenceLegacyEvent(&state, first, sizeof(first));
		ReferenceLegacyEvent(&state, second, sizeof(second));
		ReferenceLegacyEvent(&state, payloads[index].tail,
			sizeof(payloads[index].tail));
	}
	return ByteSwap32(state);
}

void UpdateReferenceCrc32(uint32_t *crc, const uint8_t *bytes,
	size_t byteCount)
{
	size_t index;
	for (index = 0U; index < byteCount; ++index)
	{
		unsigned bit;
		*crc ^= bytes[index];
		for (bit = 0U; bit < 8U; ++bit)
		{
			const uint32_t mask = 0U - (*crc & 1U);
			*crc = (*crc >> 1U) ^ (0xedb88320U & mask);
		}
	}
}

uint32_t ReferenceCurrentChecksum(
	const rts::DeterministicCrcRangeKey *plan,
	const PartitionPayload *payloads, size_t payloadCount)
{
	uint32_t crc = 0xffffffffU;
	size_t index;
	for (index = 0U; index < payloadCount; ++index)
	{
		uint8_t key[8];
		uint8_t first[4];
		uint8_t second[2];
		WriteLittleEndian(key + 0U, plan[index].sectionId, 4U);
		WriteLittleEndian(key + 4U, plan[index].partitionId, 4U);
		WriteLittleEndian(first, payloads[index].first, 4U);
		WriteLittleEndian(second, payloads[index].second, 2U);
		UpdateReferenceCrc32(&crc, key, sizeof(key));
		UpdateReferenceCrc32(&crc, first, sizeof(first));
		UpdateReferenceCrc32(&crc, second, sizeof(second));
		UpdateReferenceCrc32(&crc, payloads[index].tail,
			sizeof(payloads[index].tail));
	}
	return ~crc;
}

void InitializeFixture(
	rts::DeterministicCrcRangeKey *plan,
	PartitionPayload *payloads,
	rts::DeterministicCrcRuntimePartitionInput *inputs,
	rts::DeterministicLegacyXferOperation
		legacyStorage[PARTITION_COUNT][LEGACY_OPERATIONS_PER_PARTITION])
{
	unsigned index;
	for (index = 0U; index < PARTITION_COUNT; ++index)
	{
		const uint32_t section = 100U + index / PARTITIONS_PER_SECTION;
		const uint32_t partition = index % PARTITIONS_PER_SECTION;
		const uint64_t begin =
			static_cast<uint64_t>(partition * PARTITION_BYTES);
		plan[index].sectionId = section;
		plan[index].partitionId = partition;
		plan[index].byteBegin = begin;
		plan[index].byteEnd = begin + PARTITION_BYTES;
		payloads[index].first = 0x01020300U + index * 0x101U;
		payloads[index].second = static_cast<uint16_t>(0x1200U + index);
		payloads[index].tail[0] = static_cast<uint8_t>(index * 7U + 1U);
		payloads[index].tail[1] = static_cast<uint8_t>(index * 7U + 2U);
		payloads[index].tail[2] = static_cast<uint8_t>(index * 7U + 3U);
		inputs[index].immutableInput = &payloads[index];
		inputs[index].immutableInputBytes = sizeof(payloads[index]);
		inputs[index].capture = CapturePartition;
		inputs[index].legacyStorage = legacyStorage[index];
		inputs[index].legacyOperationCapacity =
			LEGACY_OPERATIONS_PER_PARTITION;
	}
}

int TestWorkerCountAndCompletionParity()
{
	const unsigned workerCounts[5] = {1U, 2U, 4U, 8U, 16U};
	rts::DeterministicCrcRangeKey plan[PARTITION_COUNT];
	PartitionPayload payloads[PARTITION_COUNT];
	rts::DeterministicCrcRuntimePartitionInput inputs[PARTITION_COUNT];
	rts::DeterministicLegacyXferOperation
		legacyStorage[PARTITION_COUNT][LEGACY_OPERATIONS_PER_PARTITION];
	rts::DeterministicCrcPartitionResult results[PARTITION_COUNT];
	OwnerFixture fixture;
	rts::DeterministicCrcCaptureControl control;
	rts::DeterministicCrcRuntimeAdapter adapter(IsOwner, &fixture);
	const uint32_t serialOracle = ReferenceLegacyChecksum(payloads, 0U);
	uint32_t expectedOracle;
	uint32_t currentOracle;
	unsigned run;
	int result = 0;

	InitializeFixture(plan, payloads, inputs, legacyStorage);
	expectedOracle = ReferenceLegacyChecksum(payloads, PARTITION_COUNT);
	currentOracle = ReferenceCurrentChecksum(plan, payloads, PARTITION_COUNT);
	fixture.owner = true;
	fixture.cancelled = false;
	fixture.generation = 1U;
	control.isCancelled = IsCancelled;
	control.currentGeneration = CurrentGeneration;
	control.context = &fixture;
	result |= Check(serialOracle == 0U,
		"empty serial oracle starts at the legacy zero state");

	for (run = 0U; run < 5U; ++run)
	{
		const unsigned workerCount = workerCounts[run];
		rts::DeterministicCrcRuntimeDecision decision;
		unsigned completionOrdinal;
		fixture.generation = run * 2U + 1U;
		fixture.owner = true;
		result |= Check(adapter.prepare(plan, inputs, results,
			PARTITION_COUNT, fixture.generation,
			rts::DETERMINISTIC_CRC_CAPTURE_BOTH, &control) ==
			rts::DETERMINISTIC_CRC_RUNTIME_PREPARED,
			"fixed CRC wave prepares on the game owner");

		fixture.owner = false;
		for (completionOrdinal = 0U;
			completionOrdinal < PARTITION_COUNT; ++completionOrdinal)
		{
			const unsigned partition =
				(completionOrdinal * 5U + 7U) % PARTITION_COUNT;
			const unsigned physicalWorker =
				(partition * 5U + 3U) % workerCount;
			result |= Check(adapter.executePartition(partition,
				physicalWorker) ==
				rts::DETERMINISTIC_CRC_RUNTIME_WORK_SUCCEEDED,
				"completion-order-independent partition succeeds");
		}

		fixture.owner = true;
		result |= Check(adapter.finalize(
			rts::DETERMINISTIC_CRC_LEGACY_XFER, expectedOracle,
			&decision) ==
			rts::DETERMINISTIC_CRC_RUNTIME_FINALIZE_VERIFIED,
			"owner verifies parallel CRC against the serial oracle");
		result |= Check(decision.parallelVerified &&
			decision.parallelChecksumAvailable &&
			decision.parallelChecksum == expectedOracle &&
			decision.selectedChecksum == expectedOracle &&
			decision.folded.distinctPhysicalWorkerCount == workerCount,
			"1/2/4/8/16 mappings preserve legacy checksum and provenance");

		++fixture.generation;
		fixture.owner = true;
		result |= Check(adapter.prepare(plan, inputs, results,
			PARTITION_COUNT, fixture.generation,
			rts::DETERMINISTIC_CRC_CAPTURE_BOTH, &control) ==
			rts::DETERMINISTIC_CRC_RUNTIME_PREPARED,
			"current replay-epoch verification wave prepares");
		fixture.owner = false;
		for (completionOrdinal = 0U;
			completionOrdinal < PARTITION_COUNT; ++completionOrdinal)
		{
			const unsigned partition =
				(completionOrdinal * 5U + 7U) % PARTITION_COUNT;
			const unsigned physicalWorker =
				(partition * 5U + 3U) % workerCount;
			result |= Check(adapter.executePartition(partition,
				physicalWorker) ==
				rts::DETERMINISTIC_CRC_RUNTIME_WORK_SUCCEEDED,
				"current replay-epoch partition succeeds");
		}
		fixture.owner = true;
		result |= Check(adapter.finalize(
			rts::DETERMINISTIC_CRC_CURRENT_REPLAY_EPOCH,
			currentOracle, &decision) ==
			rts::DETERMINISTIC_CRC_RUNTIME_FINALIZE_VERIFIED &&
			decision.parallelChecksum == currentOracle &&
			decision.selectedChecksum == currentOracle &&
			decision.folded.distinctPhysicalWorkerCount == workerCount,
			"1/2/4/8/16 current folds match an independent serial stream");
	}
	return result;
}

int TestPrepareFailuresDoNotTouchPrivateResults()
{
	rts::DeterministicCrcRangeKey plan[1];
	PartitionPayload payload;
	rts::DeterministicCrcRuntimePartitionInput input;
	rts::DeterministicLegacyXferOperation storage[3];
	rts::DeterministicCrcPartitionResult resultSlot;
	rts::DeterministicCrcPartitionResult before;
	OwnerFixture fixture;
	rts::DeterministicCrcCaptureControl control;
	size_t invalidPartition = 99U;
	int result = 0;

	plan[0].sectionId = 1U;
	plan[0].partitionId = 0U;
	plan[0].byteBegin = 0U;
	plan[0].byteEnd = PARTITION_BYTES;
	payload.first = 1U;
	payload.second = 2U;
	payload.tail[0] = 3U;
	payload.tail[1] = 4U;
	payload.tail[2] = 5U;
	input.immutableInput = &payload;
	input.immutableInputBytes = sizeof(payload);
	input.capture = CapturePartition;
	input.legacyStorage = storage;
	input.legacyOperationCapacity = 3U;
	memset(&resultSlot, 0x5a, sizeof(resultSlot));
	before = resultSlot;
	fixture.owner = true;
	fixture.cancelled = true;
	fixture.generation = 1U;
	control.isCancelled = IsCancelled;
	control.currentGeneration = CurrentGeneration;
	control.context = &fixture;

	{
		rts::DeterministicCrcRuntimeAdapter adapter(IsOwner, &fixture);
		result |= Check(adapter.prepare(plan, &input, &resultSlot, 1U, 1U,
			rts::DETERMINISTIC_CRC_CAPTURE_BOTH, &control) ==
			rts::DETERMINISTIC_CRC_RUNTIME_PREPARE_CANCELLED &&
			memcmp(&resultSlot, &before, sizeof(resultSlot)) == 0,
			"cancel-before-prepare leaves result storage untouched");
	}

	fixture.cancelled = false;
	fixture.generation = 2U;
	{
		rts::DeterministicCrcRuntimeAdapter adapter(IsOwner, &fixture);
		result |= Check(adapter.prepare(plan, &input, &resultSlot, 1U, 1U,
			rts::DETERMINISTIC_CRC_CAPTURE_BOTH, &control) ==
			rts::DETERMINISTIC_CRC_RUNTIME_PREPARE_STALE_GENERATION &&
			memcmp(&resultSlot, &before, sizeof(resultSlot)) == 0,
			"stale prepare leaves result storage untouched");
	}

	fixture.generation = 1U;
	plan[0].byteBegin = 1U;
	{
		rts::DeterministicCrcRuntimeAdapter adapter(IsOwner, &fixture);
		result |= Check(adapter.prepare(plan, &input, &resultSlot, 1U, 1U,
			rts::DETERMINISTIC_CRC_CAPTURE_BOTH, &control,
			&invalidPartition) ==
			rts::DETERMINISTIC_CRC_RUNTIME_PREPARE_INVALID_PLAN &&
			invalidPartition == 0U &&
			memcmp(&resultSlot, &before, sizeof(resultSlot)) == 0,
			"invalid fixed range fails before result mutation");
	}

	plan[0].byteBegin = 0U;
	input.immutableInput = &resultSlot;
	input.immutableInputBytes = sizeof(payload);
	{
		rts::DeterministicCrcRuntimeAdapter adapter(IsOwner, &fixture);
		result |= Check(adapter.prepare(plan, &input, &resultSlot, 1U, 1U,
			rts::DETERMINISTIC_CRC_CAPTURE_BOTH, &control,
			&invalidPartition) ==
			rts::DETERMINISTIC_CRC_RUNTIME_PREPARE_OVERLAPPING_STORAGE &&
			memcmp(&resultSlot, &before, sizeof(resultSlot)) == 0,
			"mutable result and immutable input overlap fails closed");
	}

	input.immutableInput = &payload;
	input.immutableInputBytes = sizeof(payload);
	{
		const size_t overflowingCount =
			SIZE_MAX / sizeof(rts::DeterministicCrcPartitionResult) + 1U;
		rts::DeterministicCrcRuntimeAdapter adapter(IsOwner, &fixture);
		result |= Check(adapter.prepare(plan, &input, &resultSlot,
			overflowingCount, 1U,
			rts::DETERMINISTIC_CRC_CAPTURE_BOTH, &control) ==
			rts::DETERMINISTIC_CRC_RUNTIME_PREPARE_INVALID_ARGUMENT &&
			memcmp(&resultSlot, &before, sizeof(resultSlot)) == 0,
			"overflowing partition count rejects before plan traversal");
	}
	return result;
}

int TestFaultCancellationStaleAndOwnerFallback()
{
	rts::DeterministicCrcRangeKey plan;
	PartitionPayload payload;
	rts::DeterministicCrcRuntimePartitionInput input;
	rts::DeterministicLegacyXferOperation storage[3];
	rts::DeterministicCrcPartitionResult resultSlot;
	OwnerFixture fixture;
	rts::DeterministicCrcCaptureControl control;
	const uint32_t serialOracle = 0x12345678U;
	int result = 0;

	plan.sectionId = 5U;
	plan.partitionId = 0U;
	plan.byteBegin = 0U;
	plan.byteEnd = PARTITION_BYTES;
	payload.first = 0x10203040U;
	payload.second = 0x5060U;
	payload.tail[0] = 0x70U;
	payload.tail[1] = 0x80U;
	payload.tail[2] = 0x90U;
	input.immutableInput = &payload;
	input.immutableInputBytes = sizeof(payload);
	input.capture = CaptureFailure;
	input.legacyStorage = storage;
	input.legacyOperationCapacity = 3U;
	fixture.owner = true;
	fixture.cancelled = false;
	fixture.generation = 1U;
	control.isCancelled = IsCancelled;
	control.currentGeneration = CurrentGeneration;
	control.context = &fixture;

	{
		rts::DeterministicCrcRuntimeAdapter adapter(IsOwner, &fixture);
		rts::DeterministicCrcRuntimeDecision decision;
		result |= Check(adapter.prepare(&plan, &input, &resultSlot, 1U, 1U,
			rts::DETERMINISTIC_CRC_CAPTURE_BOTH, &control) ==
			rts::DETERMINISTIC_CRC_RUNTIME_PREPARED,
			"worker-fault wave prepares");
		fixture.owner = false;
		result |= Check(adapter.executePartition(0U, 0U) ==
			rts::DETERMINISTIC_CRC_RUNTIME_WORK_FAILED,
			"worker callback failure rejects its private slot");
		fixture.owner = true;
		result |= Check(adapter.finalize(
			rts::DETERMINISTIC_CRC_LEGACY_XFER, serialOracle,
			&decision) ==
			rts::DETERMINISTIC_CRC_RUNTIME_FINALIZE_PARTITION_REJECTED &&
			decision.selectedChecksum == serialOracle,
			"worker fault preserves the selected serial oracle");
	}

	fixture.generation = 2U;
	fixture.owner = true;
	fixture.cancelled = false;
	input.capture = CapturePrematureComplete;
	{
		rts::DeterministicCrcRuntimeAdapter adapter(IsOwner, &fixture);
		rts::DeterministicCrcRuntimeDecision decision;
		result |= Check(adapter.prepare(&plan, &input, &resultSlot, 1U, 2U,
			rts::DETERMINISTIC_CRC_CAPTURE_BOTH, &control) ==
			rts::DETERMINISTIC_CRC_RUNTIME_PREPARED,
			"premature-complete wave prepares");
		fixture.owner = false;
		result |= Check(adapter.executePartition(0U, 0U) ==
			rts::DETERMINISTIC_CRC_RUNTIME_WORK_FAILED,
			"capture callback cannot own the terminal transition");
		fixture.owner = true;
		result |= Check(adapter.finalize(
			rts::DETERMINISTIC_CRC_LEGACY_XFER, serialOracle,
			&decision) ==
			rts::DETERMINISTIC_CRC_RUNTIME_FINALIZE_PARTITION_REJECTED &&
			decision.selectedChecksum == serialOracle,
			"premature-complete callback falls back to serial");
	}

	fixture.generation = 3U;
	fixture.owner = true;
	input.capture = CapturePartition;
	{
		rts::DeterministicCrcRuntimeAdapter adapter(IsOwner, &fixture);
		rts::DeterministicCrcRuntimeDecision decision;
		result |= Check(adapter.prepare(&plan, &input, &resultSlot, 1U, 3U,
			rts::DETERMINISTIC_CRC_CAPTURE_BOTH, &control) ==
			rts::DETERMINISTIC_CRC_RUNTIME_PREPARED &&
			adapter.executePartition(0U,
				rts::DETERMINISTIC_CRC_RUNTIME_INVALID_PHYSICAL_WORKER_ID) ==
			rts::DETERMINISTIC_CRC_RUNTIME_WORK_OWNER_EXECUTION_REJECTED &&
			adapter.finalize(rts::DETERMINISTIC_CRC_LEGACY_XFER,
				serialOracle, &decision) ==
			rts::DETERMINISTIC_CRC_RUNTIME_FINALIZE_PARTITION_REJECTED &&
			decision.selectedChecksum == serialOracle,
			"owner-helped CRC work cannot become authoritative");
	}

	fixture.generation = 4U;
	fixture.owner = true;
	fixture.cancelled = false;
	input.capture = CaptureCancellation;
	g_faultControl = &fixture;
	{
		rts::DeterministicCrcRuntimeAdapter adapter(IsOwner, &fixture);
		rts::DeterministicCrcRuntimeDecision decision;
		result |= Check(adapter.prepare(&plan, &input, &resultSlot, 1U, 4U,
			rts::DETERMINISTIC_CRC_CAPTURE_BOTH, &control) ==
			rts::DETERMINISTIC_CRC_RUNTIME_PREPARED,
			"mid-capture cancellation wave prepares");
		fixture.owner = false;
		result |= Check(adapter.executePartition(0U, 0U) ==
			rts::DETERMINISTIC_CRC_RUNTIME_WORK_CANCELLED,
			"cancellation is observed between bounded writes");
		fixture.owner = true;
		result |= Check(adapter.finalize(
			rts::DETERMINISTIC_CRC_LEGACY_XFER, serialOracle,
			&decision) ==
			rts::DETERMINISTIC_CRC_RUNTIME_FINALIZE_CANCELLED &&
			decision.selectedChecksum == serialOracle,
			"cancelled wave selects the serial oracle");
	}

	fixture.generation = 5U;
	fixture.owner = true;
	fixture.cancelled = false;
	input.capture = CaptureStaleGeneration;
	{
		rts::DeterministicCrcRuntimeAdapter adapter(IsOwner, &fixture);
		rts::DeterministicCrcRuntimeDecision decision;
		result |= Check(adapter.prepare(&plan, &input, &resultSlot, 1U, 5U,
			rts::DETERMINISTIC_CRC_CAPTURE_BOTH, &control) ==
			rts::DETERMINISTIC_CRC_RUNTIME_PREPARED,
			"mid-capture stale wave prepares");
		fixture.owner = false;
		result |= Check(adapter.executePartition(0U, 0U) ==
			rts::DETERMINISTIC_CRC_RUNTIME_WORK_STALE_GENERATION,
			"generation change is observed between bounded writes");
		fixture.owner = true;
		result |= Check(adapter.finalize(
			rts::DETERMINISTIC_CRC_LEGACY_XFER, serialOracle,
			&decision) ==
			rts::DETERMINISTIC_CRC_RUNTIME_FINALIZE_STALE_GENERATION &&
			decision.selectedChecksum == serialOracle,
			"stale wave selects the serial oracle");
	}

	fixture.generation = 7U;
	fixture.owner = true;
	fixture.cancelled = false;
	input.capture = CapturePartition;
	{
		rts::DeterministicCrcRuntimeAdapter adapter(IsOwner, &fixture);
		rts::DeterministicCrcRuntimeDecision decision;
		result |= Check(adapter.prepare(&plan, &input, &resultSlot, 1U, 7U,
			rts::DETERMINISTIC_CRC_CAPTURE_BOTH, &control) ==
			rts::DETERMINISTIC_CRC_RUNTIME_PREPARED &&
			adapter.finalize(rts::DETERMINISTIC_CRC_LEGACY_XFER,
				serialOracle, &decision) ==
			rts::DETERMINISTIC_CRC_RUNTIME_FINALIZE_PARTITION_REJECTED &&
			decision.rejectedPartition == 0U &&
			decision.selectedChecksum == serialOracle,
			"unsubmitted partition falls back before publication");
	}
	g_faultControl = 0;
	return result;
}

int TestFinalizeStaleAndOracleMismatch()
{
	rts::DeterministicCrcRangeKey plan;
	PartitionPayload payload;
	rts::DeterministicCrcRuntimePartitionInput input;
	rts::DeterministicLegacyXferOperation storage[3];
	rts::DeterministicCrcPartitionResult resultSlot;
	OwnerFixture fixture;
	rts::DeterministicCrcCaptureControl control;
	uint32_t serialOracle;
	int result = 0;

	plan.sectionId = 8U;
	plan.partitionId = 0U;
	plan.byteBegin = 0U;
	plan.byteEnd = PARTITION_BYTES;
	payload.first = 10U;
	payload.second = 20U;
	payload.tail[0] = 30U;
	payload.tail[1] = 40U;
	payload.tail[2] = 50U;
	input.immutableInput = &payload;
	input.immutableInputBytes = sizeof(payload);
	input.capture = CapturePartition;
	input.legacyStorage = storage;
	input.legacyOperationCapacity = 3U;
	serialOracle = ReferenceLegacyChecksum(&payload, 1U);
	fixture.owner = true;
	fixture.cancelled = false;
	fixture.generation = 1U;
	control.isCancelled = IsCancelled;
	control.currentGeneration = CurrentGeneration;
	control.context = &fixture;

	{
		rts::DeterministicCrcRuntimeAdapter adapter(IsOwner, &fixture);
		rts::DeterministicCrcRuntimeDecision decision;
		result |= Check(adapter.prepare(&plan, &input, &resultSlot, 1U, 1U,
			rts::DETERMINISTIC_CRC_CAPTURE_BOTH, &control) ==
			rts::DETERMINISTIC_CRC_RUNTIME_PREPARED,
			"pre-finalize stale wave prepares");
		fixture.owner = false;
		result |= Check(adapter.executePartition(0U, 0U) ==
			rts::DETERMINISTIC_CRC_RUNTIME_WORK_SUCCEEDED,
			"pre-finalize stale wave captures");
		fixture.owner = true;
		++fixture.generation;
		result |= Check(adapter.finalize(
			rts::DETERMINISTIC_CRC_LEGACY_XFER, serialOracle,
			&decision) ==
			rts::DETERMINISTIC_CRC_RUNTIME_FINALIZE_STALE_GENERATION &&
			decision.selectedChecksum == serialOracle,
			"generation changes after join reject before publication");
	}

	fixture.generation = 3U;
	fixture.owner = true;
	{
		rts::DeterministicCrcRuntimeAdapter adapter(IsOwner, &fixture);
		rts::DeterministicCrcRuntimeDecision decision;
		result |= Check(adapter.prepare(&plan, &input, &resultSlot, 1U, 3U,
			rts::DETERMINISTIC_CRC_CAPTURE_BOTH, &control) ==
			rts::DETERMINISTIC_CRC_RUNTIME_PREPARED,
			"oracle mismatch wave prepares");
		fixture.owner = false;
		result |= Check(adapter.executePartition(0U, 0U) ==
			rts::DETERMINISTIC_CRC_RUNTIME_WORK_SUCCEEDED,
			"oracle mismatch wave captures");
		fixture.owner = true;
		result |= Check(adapter.finalize(
			rts::DETERMINISTIC_CRC_LEGACY_XFER,
			serialOracle ^ 1U, &decision) ==
			rts::DETERMINISTIC_CRC_RUNTIME_FINALIZE_ORACLE_MISMATCH &&
			decision.parallelChecksumAvailable &&
			!decision.parallelVerified &&
			decision.parallelChecksum == serialOracle &&
			decision.selectedChecksum == (serialOracle ^ 1U),
			"parallel mismatch preserves the supplied serial oracle");
	}
	return result;
}

} // namespace

int main()
{
	int result = 0;
	result |= TestWorkerCountAndCompletionParity();
	result |= TestPrepareFailuresDoNotTouchPrivateResults();
	result |= TestFaultCancellationStaleAndOwnerFallback();
	result |= TestFinalizeStaleAndOracleMismatch();
	if (result == 0)
		printf("Deterministic CRC runtime adapter tests passed.\n");
	return result;
}
