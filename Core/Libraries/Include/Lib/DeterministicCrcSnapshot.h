/*
** Command & Conquer Generals(tm)
** Copyright 2026 TheSuperHackers
**
** This program is free software: you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation, either version 3 of the License, or
** (at your option) any later version.
*/

#pragma once

#include "Lib/DeterministicCrcStream.h"

namespace rts
{

enum
{
	DETERMINISTIC_CRC_SNAPSHOT_HEADER_BYTES = 20,
	DETERMINISTIC_CRC_SNAPSHOT_EVENT_HEADER_BYTES = 4
};

// Owner-only, allocation-free encoder for one immutable CRC partition.
// The sealed snapshot contains no pointers. Each appended event retains one
// exact legacy xferImplementation boundary while its copied bytes also feed
// the current replay CRC lane verbatim.
class DeterministicCrcSnapshotEncoder
{
public:
	DeterministicCrcSnapshotEncoder();

	bool begin(void *storage, size_t storageCapacity);
	// The owner may rebind an active, unsealed encoder before appending an
	// event. The old storage must remain valid until rebindStorage returns.
	// Existing bytes are copied and no worker-visible version is published.
	bool requiredStorageCapacityForXferEvent(size_t byteCount,
		size_t *requiredStorageCapacity) const;
	bool rebindStorage(void *storage, size_t storageCapacity);
	bool appendXferEvent(const void *bytes, size_t byteCount);
	bool appendBool(bool value);
	bool appendUInt8(uint8_t value);
	bool appendUInt16(uint16_t value);
	bool appendUInt32(uint32_t value);
	bool appendUInt64(uint64_t value);
	bool appendInt32(int32_t value);
	bool appendInt64(int64_t value);
	bool appendFloat32Bits(float value);
	bool appendFloat64Bits(double value);

	// Sealing publishes the immutable range only after its exact payload byte
	// count is known. On failure, output pointers are cleared and the encoder
	// remains owner-mutable so the caller may append the missing events.
	bool seal(uint64_t expectedPayloadByteCount,
		const void **immutableInput, size_t *immutableInputBytes);

	bool isBegun() const;
	bool isSealed() const;
	size_t encodedByteCount() const;
	size_t storageCapacity() const;
	uint64_t payloadByteCount() const;
	size_t eventCount() const;
	size_t requiredLegacyOperationCapacity() const;

private:
	DeterministicCrcSnapshotEncoder(
		const DeterministicCrcSnapshotEncoder &);
	DeterministicCrcSnapshotEncoder &operator=(
		const DeterministicCrcSnapshotEncoder &);

	bool appendLittleEndian(uint64_t value, unsigned byteWidth);

	uint8_t *m_storage;
	size_t m_storageCapacity;
	size_t m_encodedByteCount;
	uint64_t m_payloadByteCount;
	size_t m_eventCount;
	size_t m_requiredLegacyOperationCapacity;
	bool m_begun;
	bool m_sealed;
};

// DeterministicCrcRuntimeCaptureFunction-compatible worker decoder. It first
// validates the complete sealed snapshot and only then emits its copied events
// to the caller-owned private partition capture.
bool CaptureDeterministicCrcSnapshot(
	const void *immutableInput,
	size_t immutableInputBytes,
	DeterministicCrcPartitionCapture *output);

} // namespace rts
