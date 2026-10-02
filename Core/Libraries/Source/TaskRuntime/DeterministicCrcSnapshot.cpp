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

#include <limits.h>
#include <string.h>

namespace rts
{
namespace
{

const uint32_t DETERMINISTIC_CRC_SNAPSHOT_MAGIC = 0x43524353U;
const uint32_t DETERMINISTIC_CRC_SNAPSHOT_VERSION = 1U;

enum SnapshotHeaderOffset
{
	SNAPSHOT_MAGIC_OFFSET = 0,
	SNAPSHOT_VERSION_OFFSET = 4,
	SNAPSHOT_EVENT_COUNT_OFFSET = 8,
	SNAPSHOT_ENCODED_BYTES_OFFSET = 12,
	SNAPSHOT_PAYLOAD_BYTES_OFFSET = 16
};

void WriteUInt32(uint8_t *destination, uint32_t value)
{
	destination[0] = static_cast<uint8_t>(value & 0xffU);
	destination[1] = static_cast<uint8_t>((value >> 8U) & 0xffU);
	destination[2] = static_cast<uint8_t>((value >> 16U) & 0xffU);
	destination[3] = static_cast<uint8_t>((value >> 24U) & 0xffU);
}

uint32_t ReadUInt32(const uint8_t *source)
{
	return static_cast<uint32_t>(source[0]) |
		(static_cast<uint32_t>(source[1]) << 8U) |
		(static_cast<uint32_t>(source[2]) << 16U) |
		(static_cast<uint32_t>(source[3]) << 24U);
}

} // namespace

DeterministicCrcSnapshotEncoder::DeterministicCrcSnapshotEncoder()
	: m_storage(0),
	  m_storageCapacity(0U),
	  m_encodedByteCount(0U),
	  m_payloadByteCount(0U),
	  m_eventCount(0U),
	  m_requiredLegacyOperationCapacity(0U),
	  m_begun(false),
	  m_sealed(false)
{
}

bool DeterministicCrcSnapshotEncoder::begin(
	void *storage, size_t storageCapacity)
{
	uint8_t *bytes;
	if (storage == 0 ||
		storageCapacity < DETERMINISTIC_CRC_SNAPSHOT_HEADER_BYTES)
	{
		return false;
	}

	bytes = static_cast<uint8_t *>(storage);
	WriteUInt32(bytes + SNAPSHOT_MAGIC_OFFSET,
		DETERMINISTIC_CRC_SNAPSHOT_MAGIC);
	// Version zero is deliberately not a publishable worker snapshot.
	WriteUInt32(bytes + SNAPSHOT_VERSION_OFFSET, 0U);
	WriteUInt32(bytes + SNAPSHOT_EVENT_COUNT_OFFSET, 0U);
	WriteUInt32(bytes + SNAPSHOT_ENCODED_BYTES_OFFSET,
		DETERMINISTIC_CRC_SNAPSHOT_HEADER_BYTES);
	WriteUInt32(bytes + SNAPSHOT_PAYLOAD_BYTES_OFFSET, 0U);

	m_storage = bytes;
	m_storageCapacity = storageCapacity;
	m_encodedByteCount = DETERMINISTIC_CRC_SNAPSHOT_HEADER_BYTES;
	m_payloadByteCount = 0U;
	m_eventCount = 0U;
	m_requiredLegacyOperationCapacity = 0U;
	m_begun = true;
	m_sealed = false;
	return true;
}

bool DeterministicCrcSnapshotEncoder::requiredStorageCapacityForXferEvent(
	size_t byteCount, size_t *requiredStorageCapacity) const
{
	size_t recordBytes;
	if (requiredStorageCapacity != 0) *requiredStorageCapacity = 0U;
	if (!m_begun || m_sealed || requiredStorageCapacity == 0 ||
		byteCount > UINT_MAX || m_eventCount == UINT_MAX ||
		m_payloadByteCount > UINT_MAX - byteCount ||
		byteCount > SIZE_MAX - DETERMINISTIC_CRC_SNAPSHOT_EVENT_HEADER_BYTES)
	{
		return false;
	}
	recordBytes = DETERMINISTIC_CRC_SNAPSHOT_EVENT_HEADER_BYTES + byteCount;
	if (recordBytes > UINT_MAX ||
		m_encodedByteCount > UINT_MAX - recordBytes ||
		m_encodedByteCount > SIZE_MAX - recordBytes)
	{
		return false;
	}
	*requiredStorageCapacity = m_encodedByteCount + recordBytes;
	return true;
}

bool DeterministicCrcSnapshotEncoder::rebindStorage(
	void *storage, size_t storageCapacity)
{
	uint8_t *bytes = static_cast<uint8_t *>(storage);
	if (!m_begun || m_sealed || bytes == 0 ||
		storageCapacity < m_encodedByteCount)
	{
		return false;
	}
	if (bytes != m_storage)
		memmove(bytes, m_storage, m_encodedByteCount);
	m_storage = bytes;
	m_storageCapacity = storageCapacity;
	return true;
}

bool DeterministicCrcSnapshotEncoder::appendXferEvent(
	const void *bytes, size_t byteCount)
{
	const size_t eventOperations =
		RequiredLegacyXferOperationCapacity(byteCount);
	size_t recordBytes;
	uint8_t *record;

	if (!m_begun || m_sealed || (bytes == 0 && byteCount != 0U) ||
		byteCount > UINT_MAX || m_eventCount == UINT_MAX ||
		m_payloadByteCount > UINT_MAX - byteCount ||
		m_requiredLegacyOperationCapacity > SIZE_MAX - eventOperations ||
		byteCount > SIZE_MAX - DETERMINISTIC_CRC_SNAPSHOT_EVENT_HEADER_BYTES)
	{
		return false;
	}
	recordBytes = DETERMINISTIC_CRC_SNAPSHOT_EVENT_HEADER_BYTES + byteCount;
	if (recordBytes > UINT_MAX ||
		m_encodedByteCount > UINT_MAX - recordBytes ||
		m_encodedByteCount > m_storageCapacity ||
		recordBytes > m_storageCapacity - m_encodedByteCount)
	{
		return false;
	}

	record = m_storage + m_encodedByteCount;
	WriteUInt32(record, static_cast<uint32_t>(byteCount));
	if (byteCount != 0U)
		memcpy(record + DETERMINISTIC_CRC_SNAPSHOT_EVENT_HEADER_BYTES,
			bytes, byteCount);
	m_encodedByteCount += recordBytes;
	m_payloadByteCount += byteCount;
	++m_eventCount;
	m_requiredLegacyOperationCapacity += eventOperations;
	return true;
}

bool DeterministicCrcSnapshotEncoder::appendLittleEndian(
	uint64_t value, unsigned byteWidth)
{
	uint8_t bytes[8];
	unsigned index;
	if (byteWidth == 0U || byteWidth > sizeof(bytes)) return false;
	for (index = 0U; index < byteWidth; ++index)
	{
		bytes[index] = static_cast<uint8_t>(value & 0xffU);
		value >>= 8U;
	}
	return appendXferEvent(bytes, byteWidth);
}

bool DeterministicCrcSnapshotEncoder::appendBool(bool value)
{
	return appendUInt8(static_cast<uint8_t>(value ? 1U : 0U));
}

bool DeterministicCrcSnapshotEncoder::appendUInt8(uint8_t value)
{
	return appendLittleEndian(value, 1U);
}

bool DeterministicCrcSnapshotEncoder::appendUInt16(uint16_t value)
{
	return appendLittleEndian(value, 2U);
}

bool DeterministicCrcSnapshotEncoder::appendUInt32(uint32_t value)
{
	return appendLittleEndian(value, 4U);
}

bool DeterministicCrcSnapshotEncoder::appendUInt64(uint64_t value)
{
	return appendLittleEndian(value, 8U);
}

bool DeterministicCrcSnapshotEncoder::appendInt32(int32_t value)
{
	return appendUInt32(static_cast<uint32_t>(value));
}

bool DeterministicCrcSnapshotEncoder::appendInt64(int64_t value)
{
	return appendUInt64(static_cast<uint64_t>(value));
}

bool DeterministicCrcSnapshotEncoder::appendFloat32Bits(float value)
{
	uint32_t bits = 0U;
	memcpy(&bits, &value, sizeof(bits));
	return appendUInt32(bits);
}

bool DeterministicCrcSnapshotEncoder::appendFloat64Bits(double value)
{
	uint64_t bits = 0U;
	memcpy(&bits, &value, sizeof(bits));
	return appendUInt64(bits);
}

bool DeterministicCrcSnapshotEncoder::seal(
	uint64_t expectedPayloadByteCount,
	const void **immutableInput, size_t *immutableInputBytes)
{
	if (immutableInput != 0) *immutableInput = 0;
	if (immutableInputBytes != 0) *immutableInputBytes = 0U;
	if (!m_begun || m_sealed || immutableInput == 0 ||
		immutableInputBytes == 0 ||
		expectedPayloadByteCount != m_payloadByteCount ||
		m_eventCount > UINT_MAX || m_encodedByteCount > UINT_MAX ||
		m_payloadByteCount > UINT_MAX)
	{
		return false;
	}

	WriteUInt32(m_storage + SNAPSHOT_EVENT_COUNT_OFFSET,
		static_cast<uint32_t>(m_eventCount));
	WriteUInt32(m_storage + SNAPSHOT_ENCODED_BYTES_OFFSET,
		static_cast<uint32_t>(m_encodedByteCount));
	WriteUInt32(m_storage + SNAPSHOT_PAYLOAD_BYTES_OFFSET,
		static_cast<uint32_t>(m_payloadByteCount));
	// Publish the valid version only after every other header field is final.
	WriteUInt32(m_storage + SNAPSHOT_VERSION_OFFSET,
		DETERMINISTIC_CRC_SNAPSHOT_VERSION);
	m_sealed = true;
	*immutableInput = m_storage;
	*immutableInputBytes = m_encodedByteCount;
	return true;
}

bool DeterministicCrcSnapshotEncoder::isBegun() const { return m_begun; }
bool DeterministicCrcSnapshotEncoder::isSealed() const { return m_sealed; }
size_t DeterministicCrcSnapshotEncoder::encodedByteCount() const
{
	return m_encodedByteCount;
}
size_t DeterministicCrcSnapshotEncoder::storageCapacity() const
{
	return m_storageCapacity;
}
uint64_t DeterministicCrcSnapshotEncoder::payloadByteCount() const
{
	return m_payloadByteCount;
}
size_t DeterministicCrcSnapshotEncoder::eventCount() const
{
	return m_eventCount;
}
size_t DeterministicCrcSnapshotEncoder::requiredLegacyOperationCapacity() const
{
	return m_requiredLegacyOperationCapacity;
}

bool CaptureDeterministicCrcSnapshot(
	const void *immutableInput,
	size_t immutableInputBytes,
	DeterministicCrcPartitionCapture *output)
{
	const uint8_t *snapshot =
		static_cast<const uint8_t *>(immutableInput);
	uint32_t eventCount;
	uint32_t payloadByteCount;
	size_t cursor;
	uint64_t observedPayloadByteCount = 0U;
	uint32_t index;

	if (snapshot == 0 || output == 0 ||
		immutableInputBytes < DETERMINISTIC_CRC_SNAPSHOT_HEADER_BYTES ||
		immutableInputBytes > UINT_MAX ||
		ReadUInt32(snapshot + SNAPSHOT_MAGIC_OFFSET) !=
			DETERMINISTIC_CRC_SNAPSHOT_MAGIC ||
		ReadUInt32(snapshot + SNAPSHOT_VERSION_OFFSET) !=
			DETERMINISTIC_CRC_SNAPSHOT_VERSION ||
		ReadUInt32(snapshot + SNAPSHOT_ENCODED_BYTES_OFFSET) !=
			immutableInputBytes)
	{
		return false;
	}

	eventCount = ReadUInt32(snapshot + SNAPSHOT_EVENT_COUNT_OFFSET);
	payloadByteCount = ReadUInt32(snapshot + SNAPSHOT_PAYLOAD_BYTES_OFFSET);
	cursor = DETERMINISTIC_CRC_SNAPSHOT_HEADER_BYTES;
	for (index = 0U; index < eventCount; ++index)
	{
		uint32_t eventByteCount;
		if (cursor > immutableInputBytes ||
			immutableInputBytes - cursor <
				DETERMINISTIC_CRC_SNAPSHOT_EVENT_HEADER_BYTES)
		{
			return false;
		}
		eventByteCount = ReadUInt32(snapshot + cursor);
		cursor += DETERMINISTIC_CRC_SNAPSHOT_EVENT_HEADER_BYTES;
		if (eventByteCount > immutableInputBytes - cursor ||
			observedPayloadByteCount > UINT_MAX - eventByteCount)
		{
			return false;
		}
		cursor += eventByteCount;
		observedPayloadByteCount += eventByteCount;
	}
	if (cursor != immutableInputBytes ||
		observedPayloadByteCount != payloadByteCount)
	{
		return false;
	}

	cursor = DETERMINISTIC_CRC_SNAPSHOT_HEADER_BYTES;
	for (index = 0U; index < eventCount; ++index)
	{
		const uint32_t eventByteCount = ReadUInt32(snapshot + cursor);
		cursor += DETERMINISTIC_CRC_SNAPSHOT_EVENT_HEADER_BYTES;
		if (!output->writeXferEvent(snapshot + cursor, eventByteCount))
			return false;
		cursor += eventByteCount;
	}
	return true;
}

} // namespace rts
