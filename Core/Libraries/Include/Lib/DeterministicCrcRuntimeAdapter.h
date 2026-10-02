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

const uint32_t DETERMINISTIC_CRC_RUNTIME_INVALID_PHYSICAL_WORKER_ID =
	UINT32_MAX;
const size_t DETERMINISTIC_CRC_RUNTIME_INVALID_PARTITION = SIZE_MAX;

// The title owns one immutable, pointer-free input per fixed logical range.
// capture must only read immutableInput and write through output. It must
// preserve legacy xferImplementation call boundaries, must not call complete,
// and must not access title globals, mutable containers, or owner-only state.
typedef bool (*DeterministicCrcRuntimeCaptureFunction)(
	const void *immutableInput,
	size_t immutableInputBytes,
	DeterministicCrcPartitionCapture *output);

typedef bool (*DeterministicCrcRuntimeIsOwnerFunction)(void *context);

struct DeterministicCrcRuntimePartitionInput
{
	const void *immutableInput;
	size_t immutableInputBytes;
	DeterministicCrcRuntimeCaptureFunction capture;
	DeterministicLegacyXferOperation *legacyStorage;
	size_t legacyOperationCapacity;
};

enum DeterministicCrcRuntimePrepareStatus
{
	DETERMINISTIC_CRC_RUNTIME_PREPARED = 0,
	DETERMINISTIC_CRC_RUNTIME_PREPARE_INVALID_ARGUMENT,
	DETERMINISTIC_CRC_RUNTIME_PREPARE_NOT_OWNER,
	DETERMINISTIC_CRC_RUNTIME_PREPARE_WAVE_ACTIVE,
	DETERMINISTIC_CRC_RUNTIME_PREPARE_STALE_GENERATION,
	DETERMINISTIC_CRC_RUNTIME_PREPARE_CANCELLED,
	DETERMINISTIC_CRC_RUNTIME_PREPARE_INVALID_PLAN,
	DETERMINISTIC_CRC_RUNTIME_PREPARE_INVALID_PARTITION,
	DETERMINISTIC_CRC_RUNTIME_PREPARE_OVERLAPPING_STORAGE
};

enum DeterministicCrcRuntimeWorkStatus
{
	DETERMINISTIC_CRC_RUNTIME_WORK_SUCCEEDED = 0,
	DETERMINISTIC_CRC_RUNTIME_WORK_NOT_PREPARED,
	DETERMINISTIC_CRC_RUNTIME_WORK_INVALID_PARTITION,
	DETERMINISTIC_CRC_RUNTIME_WORK_OWNER_EXECUTION_REJECTED,
	DETERMINISTIC_CRC_RUNTIME_WORK_CANCELLED,
	DETERMINISTIC_CRC_RUNTIME_WORK_STALE_GENERATION,
	DETERMINISTIC_CRC_RUNTIME_WORK_FAILED
};

enum DeterministicCrcRuntimeFinalizeStatus
{
	DETERMINISTIC_CRC_RUNTIME_FINALIZE_VERIFIED = 0,
	DETERMINISTIC_CRC_RUNTIME_FINALIZE_INVALID_ARGUMENT,
	DETERMINISTIC_CRC_RUNTIME_FINALIZE_NOT_PREPARED,
	DETERMINISTIC_CRC_RUNTIME_FINALIZE_NOT_OWNER,
	DETERMINISTIC_CRC_RUNTIME_FINALIZE_STALE_GENERATION,
	DETERMINISTIC_CRC_RUNTIME_FINALIZE_CANCELLED,
	DETERMINISTIC_CRC_RUNTIME_FINALIZE_PARTITION_REJECTED,
	DETERMINISTIC_CRC_RUNTIME_FINALIZE_FOLD_REJECTED,
	DETERMINISTIC_CRC_RUNTIME_FINALIZE_ORACLE_MISMATCH
};

struct DeterministicCrcRuntimeDecision
{
	DeterministicCrcRuntimeFinalizeStatus status;
	DeterministicCrcFoldStatus foldStatus;
	uint32_t generation;
	uint32_t serialOracleChecksum;
	uint32_t parallelChecksum;
	uint32_t selectedChecksum;
	size_t rejectedPartition;
	DeterministicCrcFoldedChecksum folded;
	bool parallelChecksumAvailable;
	bool parallelVerified;
};

// Allocation-free adapter for one synchronous CRC wave. The scheduler owns
// dispatch and join: after prepare, it calls executePartition exactly once per
// fixed partition, in any order, with the actual scheduler worker index. Owner
// help must pass INVALID_PHYSICAL_WORKER_ID and is rejected. After every
// accepted job is joined, the game owner calls finalize.
//
// selectedChecksum is always the already-computed serial oracle. Parallel work
// only verifies that value; this adapter never publishes simulation, replay,
// or network state. Any worker fault, cancellation, stale generation, malformed
// result, or mismatch therefore falls back before title mutation.
class DeterministicCrcRuntimeAdapter
{
public:
	DeterministicCrcRuntimeAdapter(
		DeterministicCrcRuntimeIsOwnerFunction isOwner,
		void *ownerContext);

	DeterministicCrcRuntimePrepareStatus prepare(
		const DeterministicCrcRangeKey *fixedPlan,
		const DeterministicCrcRuntimePartitionInput *partitionInputs,
		DeterministicCrcPartitionResult *partitionResults,
		size_t partitionCount,
		uint32_t generation,
		uint32_t capturedLanes,
		const DeterministicCrcCaptureControl *control,
		size_t *invalidPartition = 0);

	DeterministicCrcRuntimeWorkStatus executePartition(
		size_t partitionIndex,
		uint32_t physicalWorkerId);

	DeterministicCrcRuntimeFinalizeStatus finalize(
		DeterministicCrcLane lane,
		uint32_t serialOracleChecksum,
		DeterministicCrcRuntimeDecision *decision);

	bool isPrepared() const;
	uint32_t generation() const;
	size_t partitionCount() const;

private:
	DeterministicCrcRuntimeAdapter(
		const DeterministicCrcRuntimeAdapter &);
	DeterministicCrcRuntimeAdapter &operator=(
		const DeterministicCrcRuntimeAdapter &);

	bool isOwner() const;
	bool isCancelled() const;
	bool isCurrentGeneration() const;
	void rejectPartition(size_t partitionIndex, uint32_t physicalWorkerId);

	DeterministicCrcRuntimeIsOwnerFunction m_isOwner;
	void *m_ownerContext;
	const DeterministicCrcRangeKey *m_fixedPlan;
	const DeterministicCrcRuntimePartitionInput *m_partitionInputs;
	DeterministicCrcPartitionResult *m_partitionResults;
	size_t m_partitionCount;
	uint32_t m_generation;
	uint32_t m_lastGeneration;
	uint32_t m_capturedLanes;
	DeterministicCrcCaptureControl m_control;
	bool m_hasControl;
	bool m_prepared;
};

} // namespace rts
