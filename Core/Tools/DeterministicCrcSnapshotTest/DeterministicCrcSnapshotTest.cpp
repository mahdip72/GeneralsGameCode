/*
** Command & Conquer Generals(tm)
** Copyright 2026 TheSuperHackers
**
** This program is free software: you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation, either version 3 of the License, or
** (at your option) any later version.
*/

#include "Lib/DeterministicCrcSnapshot.h"

#include <stdio.h>
#include <string.h>

namespace
{

int Check(bool condition, const char *message)
{
	if (condition) return 0;
	fprintf(stderr, "FAIL: %s\n", message);
	return 1;
}

int TestSealedSnapshotPreservesXferBoundaries()
{
	uint8_t snapshotStorage[128];
	const uint8_t tail[3] = { 0xaaU, 0xbbU, 0xccU };
	rts::DeterministicCrcSnapshotEncoder encoder;
	const void *immutableInput = 0;
	size_t immutableInputBytes = 0U;
	rts::DeterministicCrcRangeKey range;
	rts::DeterministicLegacyXferOperation legacyStorage[3];
	rts::DeterministicCrcPartitionResult partition;
	rts::DeterministicCrcPartitionCapture capture(&partition,
		legacyStorage, 3U);
	int result = 0;

	range.sectionId = 7U;
	range.partitionId = 3U;
	range.byteBegin = 0U;
	range.byteEnd = 9U;
	result |= Check(encoder.begin(snapshotStorage, sizeof(snapshotStorage)) &&
		encoder.appendUInt32(0x11223344U) &&
		encoder.appendUInt16(0x5566U) &&
		encoder.appendXferEvent(tail, sizeof(tail)),
		"owner encodes fixed-width fields and one raw Xfer event");
	result |= Check(encoder.eventCount() == 3U &&
		encoder.payloadByteCount() == 9U &&
		encoder.requiredLegacyOperationCapacity() == 3U,
		"snapshot reports exact payload and legacy operation capacities");
	result |= Check(encoder.seal(9U, &immutableInput,
		&immutableInputBytes) && immutableInput == snapshotStorage &&
		immutableInputBytes == encoder.encodedByteCount() && encoder.isSealed(),
		"owner seals an immutable pointer-free snapshot after exact sizing");
	result |= Check(!encoder.appendUInt8(1U),
		"sealed snapshot rejects owner mutation");

	result |= Check(capture.begin(range, 1U, 0U,
		rts::DETERMINISTIC_CRC_CAPTURE_BOTH, 0) &&
		rts::CaptureDeterministicCrcSnapshot(immutableInput,
			immutableInputBytes, &capture) && capture.complete(),
		"worker validates and captures the sealed snapshot");
	result |= Check(partition.status == rts::DETERMINISTIC_CRC_CAPTURE_COMPLETE &&
		partition.byteCount == 9U && partition.legacyOperationCount == 3U,
		"worker emits exactly the sealed payload bytes and event count");
	result |= Check(legacyStorage[0].value == 0x44332211U &&
		legacyStorage[1].value == 0x00005566U &&
		legacyStorage[2].value == 0x00ccbbaaU,
		"legacy operations retain each original Xfer boundary");
	return result;
}

int TestInvalidSnapshotsFailBeforeCaptureWrites()
{
	uint8_t snapshotStorage[64];
	rts::DeterministicCrcSnapshotEncoder encoder;
	const void *immutableInput = reinterpret_cast<const void *>(1);
	size_t immutableInputBytes = 99U;
	rts::DeterministicCrcRangeKey range;
	rts::DeterministicLegacyXferOperation legacyStorage[1];
	rts::DeterministicCrcPartitionResult partition;
	rts::DeterministicCrcPartitionCapture capture(&partition,
		legacyStorage, 1U);
	int result = 0;

	memset(snapshotStorage, 0x5a, sizeof(snapshotStorage));
	result |= Check(!encoder.begin(snapshotStorage,
		rts::DETERMINISTIC_CRC_SNAPSHOT_HEADER_BYTES - 1U),
		"undersized owner storage is rejected");
	result |= Check(encoder.begin(snapshotStorage, sizeof(snapshotStorage)) &&
		encoder.appendUInt32(17U),
		"valid owner snapshot begins after rejected storage");
	result |= Check(!encoder.seal(5U, &immutableInput,
		&immutableInputBytes) && immutableInput == 0 &&
		immutableInputBytes == 0U && !encoder.isSealed(),
		"payload mismatch clears publication outputs and stays owner-mutable");

	range.sectionId = 1U;
	range.partitionId = 0U;
	range.byteBegin = 0U;
	range.byteEnd = 4U;
	result |= Check(capture.begin(range, 1U, 0U,
		rts::DETERMINISTIC_CRC_CAPTURE_BOTH, 0) &&
		!rts::CaptureDeterministicCrcSnapshot(snapshotStorage,
			encoder.encodedByteCount(), &capture) &&
		partition.byteCount == 0U && partition.legacyOperationCount == 0U,
		"unsealed snapshot fails before private capture writes");

	result |= Check(encoder.seal(4U, &immutableInput,
		&immutableInputBytes),
		"corrected payload expectation seals without rebuilding");
	// Corrupt the exact encoded-length field in private test storage.
	snapshotStorage[12] ^= 1U;
	result |= Check(!rts::CaptureDeterministicCrcSnapshot(immutableInput,
		immutableInputBytes, &capture) && partition.byteCount == 0U,
		"malformed sealed length fails before private capture writes");
	return result;
}

int TestFailedAppendIsTransactional()
{
	uint8_t snapshotStorage[
		rts::DETERMINISTIC_CRC_SNAPSHOT_HEADER_BYTES + 5U];
	const uint8_t bytes[2] = { 1U, 2U };
	rts::DeterministicCrcSnapshotEncoder encoder;
	int result = 0;

	result |= Check(encoder.begin(snapshotStorage, sizeof(snapshotStorage)) &&
		encoder.appendUInt8(7U),
		"exact-capacity first event succeeds");
	const size_t encodedBefore = encoder.encodedByteCount();
	const uint64_t payloadBefore = encoder.payloadByteCount();
	const size_t eventsBefore = encoder.eventCount();
	result |= Check(!encoder.appendXferEvent(bytes, sizeof(bytes)) &&
		encoder.encodedByteCount() == encodedBefore &&
		encoder.payloadByteCount() == payloadBefore &&
		encoder.eventCount() == eventsBefore,
		"capacity failure leaves owner encoder state unchanged");
	return result;
}

} // namespace

int main()
{
	int result = 0;
	result |= TestSealedSnapshotPreservesXferBoundaries();
	result |= TestInvalidSnapshotsFailBeforeCaptureWrites();
	result |= TestFailedAppendIsTransactional();
	if (result == 0) printf("Deterministic CRC snapshot tests passed.\n");
	return result;
}
