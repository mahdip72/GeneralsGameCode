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

#include <string.h>

#if defined(_MSC_VER) && _MSC_VER < 1300
#pragma warning(disable : 4786)
#endif

namespace rts
{
namespace
{

const uint32_t CRC32_ISO_HDLC_REVERSED_POLYNOMIAL = 0xedb88320U;
typedef char FloatWidthMustBeFourBytes[(sizeof(float) == 4U) ? 1 : -1];
typedef char DoubleWidthMustBeEightBytes[(sizeof(double) == 8U) ? 1 : -1];

void ClearRangeKey(DeterministicCrcRangeKey *key)
{
	key->sectionId = 0U;
	key->partitionId = 0U;
	key->byteBegin = 0U;
	key->byteEnd = 0U;
}

bool EqualRangeIdentity(const DeterministicCrcRangeKey &left,
	const DeterministicCrcRangeKey &right)
{
	return left.sectionId == right.sectionId &&
		left.partitionId == right.partitionId;
}

bool EqualRangeKey(const DeterministicCrcRangeKey &left,
	const DeterministicCrcRangeKey &right)
{
	return EqualRangeIdentity(left, right) &&
		left.byteBegin == right.byteBegin && left.byteEnd == right.byteEnd;
}

void UpdateCrc32(uint32_t *crc, const uint8_t *bytes, size_t byteCount)
{
	size_t index;
	for (index = 0U; index < byteCount; ++index)
	{
		unsigned bit;
		*crc ^= bytes[index];
		for (bit = 0U; bit < 8U; ++bit)
		{
			const uint32_t mask = 0U - (*crc & 1U);
			*crc = (*crc >> 1U) ^
				(CRC32_ISO_HDLC_REVERSED_POLYNOMIAL & mask);
		}
	}
}

uint32_t CalculateCrc32(const uint8_t *bytes, size_t byteCount)
{
	uint32_t crc = 0xffffffffU;
	UpdateCrc32(&crc, bytes, byteCount);
	return ~crc;
}

uint32_t Gf2MatrixTimes(const uint32_t *matrix, uint32_t vector)
{
	uint32_t sum = 0U;
	while (vector != 0U)
	{
		if ((vector & 1U) != 0U) sum ^= *matrix;
		vector >>= 1U;
		++matrix;
	}
	return sum;
}

void Gf2MatrixSquare(uint32_t *square, const uint32_t *matrix)
{
	unsigned index;
	for (index = 0U; index < 32U; ++index)
		square[index] = Gf2MatrixTimes(matrix, matrix[index]);
}

uint32_t CombineCrc32(uint32_t prefixChecksum, uint32_t suffixChecksum,
	uint64_t suffixByteCount)
{
	uint32_t odd[32];
	uint32_t even[32];
	uint32_t row;
	unsigned index;

	if (suffixByteCount == 0U) return prefixChecksum;
	for (index = 0U; index < 32U; ++index)
	{
		odd[index] = 0U;
		even[index] = 0U;
	}
	odd[0] = CRC32_ISO_HDLC_REVERSED_POLYNOMIAL;
	row = 1U;
	for (index = 1U; index < 32U; ++index)
	{
		odd[index] = row;
		row <<= 1U;
	}

	Gf2MatrixSquare(even, odd);
	Gf2MatrixSquare(odd, even);
	do
	{
		Gf2MatrixSquare(even, odd);
		if ((suffixByteCount & 1U) != 0U)
			prefixChecksum = Gf2MatrixTimes(even, prefixChecksum);
		suffixByteCount >>= 1U;
		if (suffixByteCount == 0U) break;
		Gf2MatrixSquare(odd, even);
		if ((suffixByteCount & 1U) != 0U)
			prefixChecksum = Gf2MatrixTimes(odd, prefixChecksum);
		suffixByteCount >>= 1U;
	}
	while (suffixByteCount != 0U);

	return prefixChecksum ^ suffixChecksum;
}

void WriteLittleEndian(uint8_t *destination, uint64_t value,
	unsigned byteWidth)
{
	unsigned index;
	for (index = 0U; index < byteWidth; ++index)
	{
		destination[index] = static_cast<uint8_t>(value & 0xffU);
		value >>= 8U;
	}
}

void EncodePartitionKey(const DeterministicCrcRangeKey &key,
	uint8_t *encoded)
{
	WriteLittleEndian(encoded + 0U, key.sectionId, 4U);
	WriteLittleEndian(encoded + 4U, key.partitionId, 4U);
}

bool ValidCapturedLanes(uint32_t capturedLanes)
{
	return capturedLanes == DETERMINISTIC_CRC_CAPTURE_LEGACY ||
		capturedLanes == DETERMINISTIC_CRC_CAPTURE_CURRENT ||
		capturedLanes == DETERMINISTIC_CRC_CAPTURE_BOTH;
}

uint32_t ByteSwap32(uint32_t value)
{
	return ((value & 0x000000ffU) << 24U) |
		((value & 0x0000ff00U) << 8U) |
		((value & 0x00ff0000U) >> 8U) |
		((value & 0xff000000U) >> 24U);
}

DeterministicCrcFoldStatus ValidatePlan(
	const DeterministicCrcRangeKey *ranges, size_t rangeCount)
{
	size_t index;
	for (index = 0U; index < rangeCount; ++index)
	{
		const DeterministicCrcRangeKey &range = ranges[index];
		if (range.byteEnd <= range.byteBegin)
			return DETERMINISTIC_CRC_FOLD_INVALID_RANGE;
		if (index == 0U)
		{
			if (range.byteBegin != 0U)
				return DETERMINISTIC_CRC_FOLD_GAPPED_RANGES;
			continue;
		}

		const DeterministicCrcRangeKey &previous = ranges[index - 1U];
		if (range.sectionId < previous.sectionId)
			return DETERMINISTIC_CRC_FOLD_NONCANONICAL_PLAN;
		if (range.sectionId != previous.sectionId)
		{
			if (range.byteBegin != 0U)
				return DETERMINISTIC_CRC_FOLD_GAPPED_RANGES;
			continue;
		}

		if (range.partitionId == previous.partitionId)
			return DETERMINISTIC_CRC_FOLD_DUPLICATE_PLAN_KEY;
		if (range.partitionId < previous.partitionId)
			return DETERMINISTIC_CRC_FOLD_NONCANONICAL_PLAN;
		if (range.byteBegin < previous.byteEnd)
			return DETERMINISTIC_CRC_FOLD_OVERLAPPING_RANGES;
		if (range.byteBegin > previous.byteEnd)
			return DETERMINISTIC_CRC_FOLD_GAPPED_RANGES;
	}
	return DETERMINISTIC_CRC_FOLD_OK;
}

size_t FindExactResult(const DeterministicCrcRangeKey &key,
	const DeterministicCrcPartitionResult *results, size_t resultCount)
{
	size_t index;
	for (index = 0U; index < resultCount; ++index)
	{
		if (EqualRangeKey(key, results[index].key)) return index;
	}
	return resultCount;
}

bool PhysicalWorkerSeenEarlier(
	const DeterministicCrcRangeKey *expectedRanges, size_t expectedIndex,
	const DeterministicCrcPartitionResult *results, size_t resultCount,
	uint32_t physicalWorkerId)
{
	size_t index;
	for (index = 0U; index < expectedIndex; ++index)
	{
		const size_t resultIndex = FindExactResult(expectedRanges[index],
			results, resultCount);
		if (resultIndex != resultCount &&
			results[resultIndex].physicalWorkerId == physicalWorkerId)
		{
			return true;
		}
	}
	return false;
}

} // namespace

size_t RequiredLegacyXferOperationCapacity(size_t xferEventByteCount)
{
	return xferEventByteCount / 4U +
		((xferEventByteCount & 3U) != 0U ? 1U : 0U);
}

DeterministicCrcPartitionCapture::DeterministicCrcPartitionCapture(
	DeterministicCrcPartitionResult *result,
	DeterministicLegacyXferOperation *legacyStorage,
	size_t legacyCapacity)
	: m_result(result),
	  m_legacyStorage(legacyStorage),
	  m_legacyCapacity(legacyCapacity),
	  m_currentCrc(0xffffffffU),
	  m_control(0),
	  m_isWriting(false)
{
	if (m_result != 0)
	{
		ClearRangeKey(&m_result->key);
		m_result->generation = 0U;
		m_result->physicalWorkerId = 0U;
		m_result->capturedLanes = 0U;
		m_result->status = DETERMINISTIC_CRC_CAPTURE_EMPTY;
		m_result->error = DETERMINISTIC_CRC_CAPTURE_ERROR_NONE;
		m_result->currentReplayChecksum = 0U;
		m_result->byteCount = 0U;
		m_result->legacyOperations = legacyStorage;
		m_result->legacyOperationCapacity = legacyCapacity;
		m_result->legacyOperationCount = 0U;
	}
}

void DeterministicCrcPartitionCapture::setTerminal(
	DeterministicCrcCaptureStatus status,
	DeterministicCrcCaptureError error)
{
	m_isWriting = false;
	if (m_result == 0) return;
	m_result->status = status;
	m_result->error = error;
}

bool DeterministicCrcPartitionCapture::begin(
	const DeterministicCrcRangeKey &key,
	uint32_t generation,
	uint32_t physicalWorkerId,
	uint32_t capturedLanes,
	const DeterministicCrcCaptureControl *control)
{
	if (m_result == 0) return false;
	if (m_isWriting ||
		m_result->status == DETERMINISTIC_CRC_CAPTURE_WRITING)
		return false;
	m_isWriting = true;

	m_result->key = key;
	m_result->generation = generation;
	m_result->physicalWorkerId = physicalWorkerId;
	m_result->capturedLanes = capturedLanes;
	m_result->status = DETERMINISTIC_CRC_CAPTURE_WRITING;
	m_result->error = DETERMINISTIC_CRC_CAPTURE_ERROR_NONE;
	m_result->currentReplayChecksum = 0U;
	m_result->byteCount = 0U;
	m_result->legacyOperations = m_legacyStorage;
	m_result->legacyOperationCapacity = m_legacyCapacity;
	m_result->legacyOperationCount = 0U;
	m_currentCrc = 0xffffffffU;
	m_control = control;

	if (generation == 0U || key.byteEnd <= key.byteBegin ||
		!ValidCapturedLanes(capturedLanes))
	{
		setTerminal(DETERMINISTIC_CRC_CAPTURE_FAILED,
			key.byteEnd <= key.byteBegin ?
			DETERMINISTIC_CRC_CAPTURE_ERROR_INVALID_RANGE :
			DETERMINISTIC_CRC_CAPTURE_ERROR_INVALID_ARGUMENT);
		return false;
	}
	if ((capturedLanes & DETERMINISTIC_CRC_CAPTURE_LEGACY) != 0U &&
		(m_legacyStorage == 0 || m_legacyCapacity == 0U))
	{
		setTerminal(DETERMINISTIC_CRC_CAPTURE_FAILED,
			DETERMINISTIC_CRC_CAPTURE_ERROR_INVALID_ARGUMENT);
		return false;
	}
	return checkControl();
}

bool DeterministicCrcPartitionCapture::checkControl()
{
	if (m_result == 0 ||
		m_result->status != DETERMINISTIC_CRC_CAPTURE_WRITING)
	{
		return false;
	}
	if (m_control != 0 && m_control->currentGeneration != 0 &&
		m_control->currentGeneration(m_control->context) !=
			m_result->generation)
	{
		setTerminal(DETERMINISTIC_CRC_CAPTURE_STALE,
			DETERMINISTIC_CRC_CAPTURE_ERROR_STALE_GENERATION);
		return false;
	}
	if (m_control != 0 && m_control->isCancelled != 0 &&
		m_control->isCancelled(m_control->context))
	{
		setTerminal(DETERMINISTIC_CRC_CAPTURE_CANCELLED,
			DETERMINISTIC_CRC_CAPTURE_ERROR_CANCELLED);
		return false;
	}
	return true;
}

bool DeterministicCrcPartitionCapture::writeXferEvent(
	const uint8_t *bytes, size_t byteCount)
{
	const uint64_t expectedByteCount =
		m_result != 0 && m_result->key.byteEnd > m_result->key.byteBegin ?
		m_result->key.byteEnd - m_result->key.byteBegin : 0U;
	const size_t operationCount =
		RequiredLegacyXferOperationCapacity(byteCount);
	size_t sourceIndex = 0U;

	if (!checkControl()) return false;
	if (bytes == 0 && byteCount != 0U)
	{
		setTerminal(DETERMINISTIC_CRC_CAPTURE_FAILED,
			DETERMINISTIC_CRC_CAPTURE_ERROR_INVALID_ARGUMENT);
		return false;
	}
	if (m_result->byteCount > UINT64_MAX - static_cast<uint64_t>(byteCount))
	{
		setTerminal(DETERMINISTIC_CRC_CAPTURE_FAILED,
			DETERMINISTIC_CRC_CAPTURE_ERROR_BYTE_COUNT_OVERFLOW);
		return false;
	}
	if (m_result->byteCount + static_cast<uint64_t>(byteCount) >
		expectedByteCount)
	{
		setTerminal(DETERMINISTIC_CRC_CAPTURE_FAILED,
			DETERMINISTIC_CRC_CAPTURE_ERROR_RANGE_SIZE_MISMATCH);
		return false;
	}
	if ((m_result->capturedLanes & DETERMINISTIC_CRC_CAPTURE_LEGACY) != 0U &&
		(operationCount > m_legacyCapacity -
			m_result->legacyOperationCount))
	{
		setTerminal(DETERMINISTIC_CRC_CAPTURE_FAILED,
			DETERMINISTIC_CRC_CAPTURE_ERROR_STORAGE_OVERFLOW);
		return false;
	}

	if ((m_result->capturedLanes & DETERMINISTIC_CRC_CAPTURE_LEGACY) != 0U)
	{
		while (byteCount - sourceIndex >= 4U)
		{
			DeterministicLegacyXferOperation &operation =
				m_legacyStorage[m_result->legacyOperationCount++];
			operation.value =
				(static_cast<uint32_t>(bytes[sourceIndex + 0U]) << 24U) |
				(static_cast<uint32_t>(bytes[sourceIndex + 1U]) << 16U) |
				(static_cast<uint32_t>(bytes[sourceIndex + 2U]) << 8U) |
				static_cast<uint32_t>(bytes[sourceIndex + 3U]);
			sourceIndex += 4U;
		}
		if (sourceIndex != byteCount)
		{
			uint32_t value = 0U;
			unsigned shift = 0U;
			while (sourceIndex != byteCount)
			{
				value |= static_cast<uint32_t>(bytes[sourceIndex++]) << shift;
				shift += 8U;
			}
			m_legacyStorage[m_result->legacyOperationCount++].value = value;
		}
	}

	if ((m_result->capturedLanes & DETERMINISTIC_CRC_CAPTURE_CURRENT) != 0U)
		UpdateCrc32(&m_currentCrc, bytes, byteCount);
	m_result->byteCount += static_cast<uint64_t>(byteCount);
	return true;
}

bool DeterministicCrcPartitionCapture::writeLittleEndian(uint64_t value,
	unsigned byteWidth)
{
	uint8_t bytes[8];
	WriteLittleEndian(bytes, value, byteWidth);
	return writeXferEvent(bytes, byteWidth);
}

bool DeterministicCrcPartitionCapture::writeBool(bool value)
{
	return writeUInt8(static_cast<uint8_t>(value ? 1U : 0U));
}

bool DeterministicCrcPartitionCapture::writeUInt8(uint8_t value)
{
	return writeLittleEndian(value, 1U);
}

bool DeterministicCrcPartitionCapture::writeUInt16(uint16_t value)
{
	return writeLittleEndian(value, 2U);
}

bool DeterministicCrcPartitionCapture::writeUInt32(uint32_t value)
{
	return writeLittleEndian(value, 4U);
}

bool DeterministicCrcPartitionCapture::writeUInt64(uint64_t value)
{
	return writeLittleEndian(value, 8U);
}

bool DeterministicCrcPartitionCapture::writeInt32(int32_t value)
{
	return writeUInt32(static_cast<uint32_t>(value));
}

bool DeterministicCrcPartitionCapture::writeInt64(int64_t value)
{
	return writeUInt64(static_cast<uint64_t>(value));
}

bool DeterministicCrcPartitionCapture::writeFloat32Bits(float value)
{
	uint32_t bits = 0U;
	memcpy(&bits, &value, 4U);
	return writeUInt32(bits);
}

bool DeterministicCrcPartitionCapture::writeFloat64Bits(double value)
{
	uint64_t bits = 0U;
	memcpy(&bits, &value, 8U);
	return writeUInt64(bits);
}

bool DeterministicCrcPartitionCapture::complete()
{
	if (!checkControl()) return false;
	if (m_result->byteCount != m_result->key.byteEnd - m_result->key.byteBegin)
	{
		setTerminal(DETERMINISTIC_CRC_CAPTURE_FAILED,
			DETERMINISTIC_CRC_CAPTURE_ERROR_RANGE_SIZE_MISMATCH);
		return false;
	}
	if ((m_result->capturedLanes & DETERMINISTIC_CRC_CAPTURE_CURRENT) != 0U)
		m_result->currentReplayChecksum = ~m_currentCrc;
	m_result->status = DETERMINISTIC_CRC_CAPTURE_COMPLETE;
	m_result->error = DETERMINISTIC_CRC_CAPTURE_ERROR_NONE;
	m_isWriting = false;
	return true;
}

void DeterministicCrcPartitionCapture::cancel()
{
	if (m_result != 0 &&
		m_result->status == DETERMINISTIC_CRC_CAPTURE_WRITING)
	{
		setTerminal(DETERMINISTIC_CRC_CAPTURE_CANCELLED,
			DETERMINISTIC_CRC_CAPTURE_ERROR_CANCELLED);
	}
}

void DeterministicCrcPartitionCapture::fail()
{
	if (m_result != 0 &&
		m_result->status == DETERMINISTIC_CRC_CAPTURE_WRITING)
	{
		setTerminal(DETERMINISTIC_CRC_CAPTURE_FAILED,
			DETERMINISTIC_CRC_CAPTURE_ERROR_CALLER_FAILURE);
	}
}

const DeterministicCrcPartitionResult *
DeterministicCrcPartitionCapture::result() const
{
	return m_result;
}

DeterministicCrcFoldStatus FoldDeterministicCrcRanges(
	const DeterministicCrcRangeKey *expectedRanges,
	size_t expectedRangeCount,
	const DeterministicCrcPartitionResult *results,
	size_t resultCount,
	uint32_t expectedGeneration,
	DeterministicCrcLane lane,
	DeterministicCrcFoldedChecksum *output)
{
	DeterministicCrcFoldedChecksum folded;
	uint32_t legacyState = 0U;
	uint32_t currentChecksum = 0U;
	size_t index;
	DeterministicCrcFoldStatus status;

	if (output == 0 || expectedGeneration == 0U ||
		(expectedRangeCount != 0U && expectedRanges == 0) ||
		(resultCount != 0U && results == 0))
	{
		return DETERMINISTIC_CRC_FOLD_INVALID_ARGUMENT;
	}
	if (lane != DETERMINISTIC_CRC_LEGACY_XFER &&
		lane != DETERMINISTIC_CRC_CURRENT_REPLAY_EPOCH)
	{
		return DETERMINISTIC_CRC_FOLD_INVALID_LANE;
	}
	status = ValidatePlan(expectedRanges, expectedRangeCount);
	if (status != DETERMINISTIC_CRC_FOLD_OK) return status;

	// Reject duplicate result identities before matching bounds so a duplicate
	// cannot masquerade as an unexpected replacement range.
	for (index = 0U; index < resultCount; ++index)
	{
		size_t previous;
		bool expected = false;
		for (previous = 0U; previous < index; ++previous)
		{
			if (EqualRangeIdentity(results[index].key, results[previous].key))
				return DETERMINISTIC_CRC_FOLD_DUPLICATE_PARTITION;
		}
		for (previous = 0U; previous < expectedRangeCount; ++previous)
		{
			if (EqualRangeKey(results[index].key, expectedRanges[previous]))
			{
				expected = true;
				break;
			}
		}
		if (!expected) return DETERMINISTIC_CRC_FOLD_UNEXPECTED_PARTITION;
	}
	for (index = 0U; index < expectedRangeCount; ++index)
	{
		if (FindExactResult(expectedRanges[index], results, resultCount) ==
			resultCount)
		{
			return DETERMINISTIC_CRC_FOLD_MISSING_PARTITION;
		}
	}

	folded.lane = lane;
	folded.generation = expectedGeneration;
	folded.checksum = 0U;
	folded.payloadByteCount = 0U;
	folded.hashedByteCount = 0U;
	folded.distinctPhysicalWorkerCount = 0U;
	folded.physicalWorkerMask = 0U;
	folded.physicalWorkerMaskComplete = true;

	for (index = 0U; index < expectedRangeCount; ++index)
	{
		const size_t resultIndex = FindExactResult(expectedRanges[index],
			results, resultCount);
		const DeterministicCrcPartitionResult &partition = results[resultIndex];
		const uint64_t expectedBytes =
			expectedRanges[index].byteEnd - expectedRanges[index].byteBegin;

		if (partition.status == DETERMINISTIC_CRC_CAPTURE_CANCELLED)
			return DETERMINISTIC_CRC_FOLD_PARTITION_CANCELLED;
		if (partition.status == DETERMINISTIC_CRC_CAPTURE_STALE ||
			partition.generation != expectedGeneration)
		{
			return DETERMINISTIC_CRC_FOLD_STALE_GENERATION;
		}
		if (partition.status == DETERMINISTIC_CRC_CAPTURE_FAILED)
			return DETERMINISTIC_CRC_FOLD_PARTITION_FAILED;
		if (partition.status != DETERMINISTIC_CRC_CAPTURE_COMPLETE)
			return DETERMINISTIC_CRC_FOLD_PARTITION_INCOMPLETE;
		if (partition.error != DETERMINISTIC_CRC_CAPTURE_ERROR_NONE ||
			!ValidCapturedLanes(partition.capturedLanes))
		{
			return DETERMINISTIC_CRC_FOLD_INVALID_CAPTURE_METADATA;
		}
		if ((lane == DETERMINISTIC_CRC_LEGACY_XFER &&
			(partition.capturedLanes & DETERMINISTIC_CRC_CAPTURE_LEGACY) == 0U) ||
			(lane == DETERMINISTIC_CRC_CURRENT_REPLAY_EPOCH &&
			(partition.capturedLanes & DETERMINISTIC_CRC_CAPTURE_CURRENT) == 0U))
		{
			return DETERMINISTIC_CRC_FOLD_CAPTURE_LANE_MISSING;
		}
		if (partition.byteCount != expectedBytes)
			return DETERMINISTIC_CRC_FOLD_BYTE_COUNT_MISMATCH;
		if (folded.payloadByteCount > UINT64_MAX - partition.byteCount)
			return DETERMINISTIC_CRC_FOLD_BYTE_COUNT_OVERFLOW;

		if (!PhysicalWorkerSeenEarlier(expectedRanges, index, results,
			resultCount, partition.physicalWorkerId))
		{
			++folded.distinctPhysicalWorkerCount;
			if (partition.physicalWorkerId < 64U)
			{
				folded.physicalWorkerMask |=
					static_cast<uint64_t>(1U) << partition.physicalWorkerId;
			}
			else
			{
				folded.physicalWorkerMaskComplete = false;
			}
		}

		if (lane == DETERMINISTIC_CRC_LEGACY_XFER)
		{
			size_t operationIndex;
			size_t minimumOperations;
			if (expectedBytes > static_cast<uint64_t>(SIZE_MAX))
				return DETERMINISTIC_CRC_FOLD_LEGACY_STREAM_MALFORMED;
			minimumOperations = RequiredLegacyXferOperationCapacity(
				static_cast<size_t>(expectedBytes));
			if (
				partition.legacyOperations == 0 ||
				partition.legacyOperationCount >
					partition.legacyOperationCapacity ||
				partition.legacyOperationCount < minimumOperations ||
				static_cast<uint64_t>(partition.legacyOperationCount) >
					expectedBytes)
			{
				return DETERMINISTIC_CRC_FOLD_LEGACY_STREAM_MALFORMED;
			}
			for (operationIndex = 0U;
				operationIndex < partition.legacyOperationCount;
				++operationIndex)
			{
				legacyState = (legacyState << 1U) +
					partition.legacyOperations[operationIndex].value +
					((legacyState >> 31U) & 1U);
			}
		}
		else
		{
			uint8_t encodedKey[
				DETERMINISTIC_CRC_ENCODED_PARTITION_KEY_BYTES];
			uint32_t keyChecksum;
			EncodePartitionKey(expectedRanges[index], encodedKey);
			keyChecksum = CalculateCrc32(encodedKey,
				DETERMINISTIC_CRC_ENCODED_PARTITION_KEY_BYTES);
			if (folded.hashedByteCount >
				UINT64_MAX - static_cast<uint64_t>(
					DETERMINISTIC_CRC_ENCODED_PARTITION_KEY_BYTES) ||
				partition.byteCount > UINT64_MAX -
					(folded.hashedByteCount +
						DETERMINISTIC_CRC_ENCODED_PARTITION_KEY_BYTES))
			{
				return DETERMINISTIC_CRC_FOLD_BYTE_COUNT_OVERFLOW;
			}
			currentChecksum = CombineCrc32(currentChecksum, keyChecksum,
				DETERMINISTIC_CRC_ENCODED_PARTITION_KEY_BYTES);
			folded.hashedByteCount +=
				DETERMINISTIC_CRC_ENCODED_PARTITION_KEY_BYTES;
			currentChecksum = CombineCrc32(currentChecksum,
				partition.currentReplayChecksum, partition.byteCount);
			folded.hashedByteCount += partition.byteCount;
		}
		folded.payloadByteCount += partition.byteCount;
	}

	if (lane == DETERMINISTIC_CRC_LEGACY_XFER)
	{
		// XferCRC::getCRC returns htobe(m_crc).  Supported legacy and native
		// Windows lanes are little-endian, so this stable number is byte-swapped.
		folded.checksum = ByteSwap32(legacyState);
		folded.hashedByteCount = folded.payloadByteCount;
	}
	else
	{
		folded.checksum = currentChecksum;
	}
	*output = folded;
	return DETERMINISTIC_CRC_FOLD_OK;
}

} // namespace rts
