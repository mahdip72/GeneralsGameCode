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

#include <stddef.h>
#include <Utility/stdint_adapter.h>

namespace rts
{

// The legacy and current replay lanes are deliberately separate.  Legacy is
// the XferCRC rotate/add stream used by existing Win32 replays.  Current is a
// CRC-32/ISO-HDLC stream whose committed canonical partition key is encoded
// before each payload.
enum DeterministicCrcLane
{
	DETERMINISTIC_CRC_LEGACY_XFER = 1,
	DETERMINISTIC_CRC_CURRENT_REPLAY_EPOCH = 2
};

enum DeterministicCrcCaptureLane
{
	DETERMINISTIC_CRC_CAPTURE_LEGACY = 1,
	DETERMINISTIC_CRC_CAPTURE_CURRENT = 2,
	DETERMINISTIC_CRC_CAPTURE_BOTH = 3
};

enum
{
	// Exact CanonicalSimulationChecksum key: sectionId:u32 + partitionId:u32.
	DETERMINISTIC_CRC_ENCODED_PARTITION_KEY_BYTES = 8
};

enum DeterministicCrcCaptureStatus
{
	DETERMINISTIC_CRC_CAPTURE_EMPTY = 0,
	DETERMINISTIC_CRC_CAPTURE_WRITING,
	DETERMINISTIC_CRC_CAPTURE_COMPLETE,
	DETERMINISTIC_CRC_CAPTURE_CANCELLED,
	DETERMINISTIC_CRC_CAPTURE_FAILED,
	DETERMINISTIC_CRC_CAPTURE_STALE
};

enum DeterministicCrcCaptureError
{
	DETERMINISTIC_CRC_CAPTURE_ERROR_NONE = 0,
	DETERMINISTIC_CRC_CAPTURE_ERROR_INVALID_ARGUMENT,
	DETERMINISTIC_CRC_CAPTURE_ERROR_INVALID_RANGE,
	DETERMINISTIC_CRC_CAPTURE_ERROR_STORAGE_OVERFLOW,
	DETERMINISTIC_CRC_CAPTURE_ERROR_BYTE_COUNT_OVERFLOW,
	DETERMINISTIC_CRC_CAPTURE_ERROR_RANGE_SIZE_MISMATCH,
	DETERMINISTIC_CRC_CAPTURE_ERROR_CANCELLED,
	DETERMINISTIC_CRC_CAPTURE_ERROR_STALE_GENERATION,
	DETERMINISTIC_CRC_CAPTURE_ERROR_CALLER_FAILURE
};

// sectionId and partitionId are the committed canonical checksum identity.
// byteBegin and byteEnd are end-exclusive plan validation metadata, not
// addresses and not part of the current replay checksum key.
struct DeterministicCrcRangeKey
{
	uint32_t sectionId;
	uint32_t partitionId;
	uint64_t byteBegin;
	uint64_t byteEnd;
};

// One legacy operation is exactly one XferCRC rotate/add input.  It is not a
// checksum of a partition: XferCRC cannot be combined from partition checksums
// because rotation and integer addition make its state transition dependent on
// the incoming CRC.  Workers therefore capture these bounded operations and
// the owner folds them in canonical range order.
struct DeterministicLegacyXferOperation
{
	uint32_t value;
};

struct DeterministicCrcPartitionResult
{
	DeterministicCrcRangeKey key;
	uint32_t generation;
	uint32_t physicalWorkerId;
	uint32_t capturedLanes;
	DeterministicCrcCaptureStatus status;
	DeterministicCrcCaptureError error;
	uint32_t currentReplayChecksum;
	uint64_t byteCount;
	const DeterministicLegacyXferOperation *legacyOperations;
	size_t legacyOperationCapacity;
	size_t legacyOperationCount;
};

typedef bool (*DeterministicCrcCancellationFunction)(void *context);
typedef uint32_t (*DeterministicCrcGenerationFunction)(void *context);

// Optional probes let a worker notice cancellation or owner generation changes
// between bounded writes without coupling this core to a scheduler.
struct DeterministicCrcCaptureControl
{
	DeterministicCrcCancellationFunction isCancelled;
	DeterministicCrcGenerationFunction currentGeneration;
	void *context;
};

// Caller-owned, reusable capture.  This class never allocates.  A legacy
// capture needs one operation slot per full four-byte group plus one for a
// non-empty tail in every writeXferEvent call.  Splitting a legacy Xfer event
// changes the oracle, so the title adapter must preserve call boundaries.
class DeterministicCrcPartitionCapture
{
public:
	DeterministicCrcPartitionCapture(
		DeterministicCrcPartitionResult *result,
		DeterministicLegacyXferOperation *legacyStorage,
		size_t legacyCapacity);

	bool begin(const DeterministicCrcRangeKey &key,
		uint32_t generation,
		uint32_t physicalWorkerId,
		uint32_t capturedLanes,
		const DeterministicCrcCaptureControl *control);

	// Each call is one exact legacy Xfer implementation event.  The bytes are
	// also streamed verbatim into the current replay CRC.  Do not pass object
	// memory, padding, pointers, vtables, container layout, or native wchar_t.
	bool writeXferEvent(const uint8_t *bytes, size_t byteCount);

	// Fixed-width current-epoch fields use little-endian two's-complement or
	// exact IEEE bit encodings.  Each method is also one legacy Xfer event.
	bool writeBool(bool value);
	bool writeUInt8(uint8_t value);
	bool writeUInt16(uint16_t value);
	bool writeUInt32(uint32_t value);
	bool writeUInt64(uint64_t value);
	bool writeInt32(int32_t value);
	bool writeInt64(int64_t value);
	bool writeFloat32Bits(float value);
	bool writeFloat64Bits(double value);

	bool complete();
	void cancel();
	void fail();

	const DeterministicCrcPartitionResult *result() const;

private:
	bool checkControl();
	bool writeLittleEndian(uint64_t value, unsigned byteWidth);
	void setTerminal(DeterministicCrcCaptureStatus status,
		DeterministicCrcCaptureError error);

	DeterministicCrcPartitionResult *m_result;
	DeterministicLegacyXferOperation *m_legacyStorage;
	size_t m_legacyCapacity;
	uint32_t m_currentCrc;
	const DeterministicCrcCaptureControl *m_control;
	bool m_isWriting;
};

enum DeterministicCrcFoldStatus
{
	DETERMINISTIC_CRC_FOLD_OK = 0,
	DETERMINISTIC_CRC_FOLD_INVALID_ARGUMENT,
	DETERMINISTIC_CRC_FOLD_INVALID_LANE,
	DETERMINISTIC_CRC_FOLD_INVALID_RANGE,
	DETERMINISTIC_CRC_FOLD_NONCANONICAL_PLAN,
	DETERMINISTIC_CRC_FOLD_DUPLICATE_PLAN_KEY,
	DETERMINISTIC_CRC_FOLD_OVERLAPPING_RANGES,
	DETERMINISTIC_CRC_FOLD_GAPPED_RANGES,
	DETERMINISTIC_CRC_FOLD_UNEXPECTED_PARTITION,
	DETERMINISTIC_CRC_FOLD_DUPLICATE_PARTITION,
	DETERMINISTIC_CRC_FOLD_MISSING_PARTITION,
	DETERMINISTIC_CRC_FOLD_PARTITION_INCOMPLETE,
	DETERMINISTIC_CRC_FOLD_PARTITION_CANCELLED,
	DETERMINISTIC_CRC_FOLD_PARTITION_FAILED,
	DETERMINISTIC_CRC_FOLD_STALE_GENERATION,
	DETERMINISTIC_CRC_FOLD_INVALID_CAPTURE_METADATA,
	DETERMINISTIC_CRC_FOLD_CAPTURE_LANE_MISSING,
	DETERMINISTIC_CRC_FOLD_BYTE_COUNT_MISMATCH,
	DETERMINISTIC_CRC_FOLD_LEGACY_STREAM_MALFORMED,
	DETERMINISTIC_CRC_FOLD_BYTE_COUNT_OVERFLOW
};

struct DeterministicCrcFoldedChecksum
{
	DeterministicCrcLane lane;
	uint32_t generation;
	// Legacy is the network-order numeric value returned by XferCRC::getCRC
	// on supported little-endian Windows builds. Current is finalized
	// CRC-32/ISO-HDLC over key-framed payload bytes.
	uint32_t checksum;
	uint64_t payloadByteCount;
	uint64_t hashedByteCount;
	// Provenance is diagnostic only and is never included in checksum.
	uint32_t distinctPhysicalWorkerCount;
	uint64_t physicalWorkerMask;
	bool physicalWorkerMaskComplete;
};

// expectedRanges is the fixed title-owned plan in section/range order.  Ranges
// within each section must start at zero and be contiguous.  results may be in
// any completion order.  The output is published only after the entire plan is
// validated and folded.
DeterministicCrcFoldStatus FoldDeterministicCrcRanges(
	const DeterministicCrcRangeKey *expectedRanges,
	size_t expectedRangeCount,
	const DeterministicCrcPartitionResult *results,
	size_t resultCount,
	uint32_t expectedGeneration,
	DeterministicCrcLane lane,
	DeterministicCrcFoldedChecksum *output);

size_t RequiredLegacyXferOperationCapacity(size_t xferEventByteCount);

// Title-adapter seam (this core does not grant runtime authority):
//  1. The owner snapshots immutable, pointer-free values and fixes this plan
//     independently of worker count or completion order. Workers never call a
//     title Snapshot, XferCRC, The* global, or mutable title container.
//  2. A title adapter encodes only those immutable values. A legacy adapter
//     maps every old xferImplementation call one-for-one to writeXferEvent
//     (typed helpers count as one call). Range boundaries may occur only
//     between old calls, and range order must equal old owner order.
//  3. physicalWorkerId comes from the executing scheduler worker, not a logical
//     range index.  It is retained only as provenance.
//  4. The owner joins, folds with the captured generation, then publishes.  Any
//     missing, duplicate, cancelled, failed, or stale result fails the wave.
//  5. The current lane is an explicit replay-epoch opt-in.  Existing replay and
//     network peers stay on the legacy lane until a title schema gate changes.

} // namespace rts
