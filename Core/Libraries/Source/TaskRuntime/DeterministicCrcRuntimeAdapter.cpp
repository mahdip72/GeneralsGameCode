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

namespace rts
{
namespace
{

struct AddressSpan
{
	uintptr_t begin;
	uintptr_t end;
};

bool MakeAddressSpan(const void *data, size_t count, size_t itemSize,
	AddressSpan *span)
{
	size_t byteCount;
	uintptr_t begin;
	if (span == 0 || itemSize == 0U) return false;
	span->begin = 0U;
	span->end = 0U;
	if (count == 0U) return true;
	if (data == 0 || count > SIZE_MAX / itemSize) return false;
	byteCount = count * itemSize;
	if (byteCount > static_cast<size_t>(UINTPTR_MAX)) return false;
	begin = reinterpret_cast<uintptr_t>(data);
	if (begin > UINTPTR_MAX - static_cast<uintptr_t>(byteCount)) return false;
	span->begin = begin;
	span->end = begin + static_cast<uintptr_t>(byteCount);
	return true;
}

bool SpansOverlap(const AddressSpan &left, const AddressSpan &right)
{
	if (left.begin == left.end || right.begin == right.end) return false;
	return left.begin < right.end && right.begin < left.end;
}

bool ValidCapturedLanes(uint32_t capturedLanes)
{
	return capturedLanes == DETERMINISTIC_CRC_CAPTURE_LEGACY ||
		capturedLanes == DETERMINISTIC_CRC_CAPTURE_CURRENT ||
		capturedLanes == DETERMINISTIC_CRC_CAPTURE_BOTH;
}

bool ValidateFixedPlan(const DeterministicCrcRangeKey *plan,
	size_t partitionCount, size_t *invalidPartition)
{
	size_t index;
	for (index = 0U; index < partitionCount; ++index)
	{
		const DeterministicCrcRangeKey &range = plan[index];
		if (range.byteEnd <= range.byteBegin)
		{
			*invalidPartition = index;
			return false;
		}
		if (index == 0U)
		{
			if (range.byteBegin != 0U)
			{
				*invalidPartition = index;
				return false;
			}
			continue;
		}

		const DeterministicCrcRangeKey &previous = plan[index - 1U];
		if (range.sectionId < previous.sectionId ||
			(range.sectionId == previous.sectionId &&
			 range.partitionId <= previous.partitionId) ||
			(range.sectionId == previous.sectionId &&
			 range.byteBegin != previous.byteEnd) ||
			(range.sectionId != previous.sectionId &&
			 range.byteBegin != 0U))
		{
			*invalidPartition = index;
			return false;
		}
	}
	return true;
}

void ClearFoldedChecksum(DeterministicCrcFoldedChecksum *folded,
	DeterministicCrcLane lane, uint32_t generation)
{
	folded->lane = lane;
	folded->generation = generation;
	folded->checksum = 0U;
	folded->payloadByteCount = 0U;
	folded->hashedByteCount = 0U;
	folded->distinctPhysicalWorkerCount = 0U;
	folded->physicalWorkerMask = 0U;
	folded->physicalWorkerMaskComplete = true;
}

void InitializeDecision(DeterministicCrcRuntimeDecision *decision,
	DeterministicCrcRuntimeFinalizeStatus status,
	DeterministicCrcLane lane, uint32_t generation,
	uint32_t serialOracleChecksum)
{
	decision->status = status;
	decision->foldStatus = DETERMINISTIC_CRC_FOLD_OK;
	decision->generation = generation;
	decision->serialOracleChecksum = serialOracleChecksum;
	decision->parallelChecksum = 0U;
	decision->selectedChecksum = serialOracleChecksum;
	decision->rejectedPartition = DETERMINISTIC_CRC_RUNTIME_INVALID_PARTITION;
	ClearFoldedChecksum(&decision->folded, lane, generation);
	decision->parallelChecksumAvailable = false;
	decision->parallelVerified = false;
}

DeterministicCrcRuntimeWorkStatus WorkStatusForResult(
	const DeterministicCrcPartitionResult &result)
{
	if (result.status == DETERMINISTIC_CRC_CAPTURE_COMPLETE)
		return DETERMINISTIC_CRC_RUNTIME_WORK_SUCCEEDED;
	if (result.status == DETERMINISTIC_CRC_CAPTURE_CANCELLED)
		return DETERMINISTIC_CRC_RUNTIME_WORK_CANCELLED;
	if (result.status == DETERMINISTIC_CRC_CAPTURE_STALE)
		return DETERMINISTIC_CRC_RUNTIME_WORK_STALE_GENERATION;
	return DETERMINISTIC_CRC_RUNTIME_WORK_FAILED;
}

DeterministicCrcRuntimeFinalizeStatus FinalizeStatusForFold(
	DeterministicCrcFoldStatus status)
{
	if (status == DETERMINISTIC_CRC_FOLD_STALE_GENERATION)
		return DETERMINISTIC_CRC_RUNTIME_FINALIZE_STALE_GENERATION;
	if (status == DETERMINISTIC_CRC_FOLD_PARTITION_CANCELLED)
		return DETERMINISTIC_CRC_RUNTIME_FINALIZE_CANCELLED;
	if (status == DETERMINISTIC_CRC_FOLD_PARTITION_FAILED ||
		status == DETERMINISTIC_CRC_FOLD_PARTITION_INCOMPLETE)
	{
		return DETERMINISTIC_CRC_RUNTIME_FINALIZE_PARTITION_REJECTED;
	}
	return DETERMINISTIC_CRC_RUNTIME_FINALIZE_FOLD_REJECTED;
}

size_t FindRejectedPartition(const DeterministicCrcPartitionResult *results,
	size_t partitionCount, uint32_t generation)
{
	size_t index;
	for (index = 0U; index < partitionCount; ++index)
	{
		if (results[index].generation != generation ||
			results[index].status != DETERMINISTIC_CRC_CAPTURE_COMPLETE ||
			results[index].error != DETERMINISTIC_CRC_CAPTURE_ERROR_NONE)
		{
			return index;
		}
	}
	return DETERMINISTIC_CRC_RUNTIME_INVALID_PARTITION;
}

} // namespace

DeterministicCrcRuntimeAdapter::DeterministicCrcRuntimeAdapter(
	DeterministicCrcRuntimeIsOwnerFunction isOwnerFunction,
	void *ownerContext)
	: m_isOwner(isOwnerFunction),
	  m_ownerContext(ownerContext),
	  m_fixedPlan(0),
	  m_partitionInputs(0),
	  m_partitionResults(0),
	  m_partitionCount(0U),
	  m_generation(0U),
	  m_lastGeneration(0U),
	  m_capturedLanes(0U),
	  m_hasControl(false),
	  m_prepared(false)
{
	m_control.isCancelled = 0;
	m_control.currentGeneration = 0;
	m_control.context = 0;
}

bool DeterministicCrcRuntimeAdapter::isOwner() const
{
	return m_isOwner != 0 && m_isOwner(m_ownerContext);
}

bool DeterministicCrcRuntimeAdapter::isCancelled() const
{
	return m_hasControl && m_control.isCancelled != 0 &&
		m_control.isCancelled(m_control.context);
}

bool DeterministicCrcRuntimeAdapter::isCurrentGeneration() const
{
	return !m_hasControl || m_control.currentGeneration == 0 ||
		m_control.currentGeneration(m_control.context) == m_generation;
}

DeterministicCrcRuntimePrepareStatus
DeterministicCrcRuntimeAdapter::prepare(
	const DeterministicCrcRangeKey *fixedPlan,
	const DeterministicCrcRuntimePartitionInput *partitionInputs,
	DeterministicCrcPartitionResult *partitionResults,
	size_t partitionCountValue,
	uint32_t generationValue,
	uint32_t capturedLanes,
	const DeterministicCrcCaptureControl *control,
	size_t *invalidPartition)
{
	AddressSpan resultSpan;
	AddressSpan planSpan;
	AddressSpan inputSpan;
	size_t failureIndex = DETERMINISTIC_CRC_RUNTIME_INVALID_PARTITION;
	size_t index;

	if (invalidPartition != 0)
		*invalidPartition = DETERMINISTIC_CRC_RUNTIME_INVALID_PARTITION;
	if (m_prepared) return DETERMINISTIC_CRC_RUNTIME_PREPARE_WAVE_ACTIVE;
	if (m_isOwner == 0 || fixedPlan == 0 || partitionInputs == 0 ||
		partitionResults == 0 || partitionCountValue == 0U ||
		generationValue == 0U || !ValidCapturedLanes(capturedLanes))
	{
		return DETERMINISTIC_CRC_RUNTIME_PREPARE_INVALID_ARGUMENT;
	}
	if (!isOwner()) return DETERMINISTIC_CRC_RUNTIME_PREPARE_NOT_OWNER;
	if (generationValue <= m_lastGeneration)
		return DETERMINISTIC_CRC_RUNTIME_PREPARE_STALE_GENERATION;
	if (control != 0 && control->currentGeneration != 0 &&
		control->currentGeneration(control->context) != generationValue)
	{
		return DETERMINISTIC_CRC_RUNTIME_PREPARE_STALE_GENERATION;
	}
	if (control != 0 && control->isCancelled != 0 &&
		control->isCancelled(control->context))
	{
		return DETERMINISTIC_CRC_RUNTIME_PREPARE_CANCELLED;
	}
	if (!MakeAddressSpan(partitionResults, partitionCountValue,
		sizeof(DeterministicCrcPartitionResult), &resultSpan) ||
		!MakeAddressSpan(fixedPlan, partitionCountValue,
		sizeof(DeterministicCrcRangeKey), &planSpan) ||
		!MakeAddressSpan(partitionInputs, partitionCountValue,
		sizeof(DeterministicCrcRuntimePartitionInput), &inputSpan))
	{
		return DETERMINISTIC_CRC_RUNTIME_PREPARE_INVALID_ARGUMENT;
	}
	if (SpansOverlap(resultSpan, planSpan) ||
		SpansOverlap(resultSpan, inputSpan))
	{
		return DETERMINISTIC_CRC_RUNTIME_PREPARE_OVERLAPPING_STORAGE;
	}
	if (!ValidateFixedPlan(fixedPlan, partitionCountValue, &failureIndex))
	{
		if (invalidPartition != 0) *invalidPartition = failureIndex;
		return DETERMINISTIC_CRC_RUNTIME_PREPARE_INVALID_PLAN;
	}

	for (index = 0U; index < partitionCountValue; ++index)
	{
		const DeterministicCrcRuntimePartitionInput &input =
			partitionInputs[index];
		AddressSpan immutableSpan;
		AddressSpan legacySpan;
		size_t other;
		if (input.capture == 0 ||
			(input.immutableInputBytes != 0U && input.immutableInput == 0) ||
			((capturedLanes & DETERMINISTIC_CRC_CAPTURE_LEGACY) != 0U &&
			 (input.legacyStorage == 0 || input.legacyOperationCapacity == 0U)) ||
			!MakeAddressSpan(input.immutableInput, input.immutableInputBytes,
				1U, &immutableSpan) ||
			!MakeAddressSpan(input.legacyStorage,
				input.legacyOperationCapacity,
				sizeof(DeterministicLegacyXferOperation), &legacySpan))
		{
			if (invalidPartition != 0) *invalidPartition = index;
			return DETERMINISTIC_CRC_RUNTIME_PREPARE_INVALID_PARTITION;
		}
		if (SpansOverlap(resultSpan, immutableSpan) ||
			SpansOverlap(legacySpan, resultSpan) ||
			SpansOverlap(legacySpan, planSpan) ||
			SpansOverlap(legacySpan, inputSpan) ||
			SpansOverlap(legacySpan, immutableSpan))
		{
			if (invalidPartition != 0) *invalidPartition = index;
			return DETERMINISTIC_CRC_RUNTIME_PREPARE_OVERLAPPING_STORAGE;
		}

		for (other = 0U; other < index; ++other)
		{
			AddressSpan otherImmutableSpan;
			AddressSpan otherLegacySpan;
			const DeterministicCrcRuntimePartitionInput &otherInput =
				partitionInputs[other];
			if (!MakeAddressSpan(otherInput.immutableInput,
					otherInput.immutableInputBytes, 1U,
					&otherImmutableSpan) ||
				!MakeAddressSpan(otherInput.legacyStorage,
					otherInput.legacyOperationCapacity,
					sizeof(DeterministicLegacyXferOperation),
					&otherLegacySpan))
			{
				if (invalidPartition != 0) *invalidPartition = other;
				return DETERMINISTIC_CRC_RUNTIME_PREPARE_INVALID_PARTITION;
			}
			if (SpansOverlap(legacySpan, otherLegacySpan) ||
				SpansOverlap(legacySpan, otherImmutableSpan) ||
				SpansOverlap(otherLegacySpan, immutableSpan))
			{
				if (invalidPartition != 0) *invalidPartition = index;
				return DETERMINISTIC_CRC_RUNTIME_PREPARE_OVERLAPPING_STORAGE;
			}
		}
	}

	m_fixedPlan = fixedPlan;
	m_partitionInputs = partitionInputs;
	m_partitionResults = partitionResults;
	m_partitionCount = partitionCountValue;
	m_generation = generationValue;
	m_lastGeneration = generationValue;
	m_capturedLanes = capturedLanes;
	m_hasControl = control != 0;
	if (control != 0) m_control = *control;
	else
	{
		m_control.isCancelled = 0;
		m_control.currentGeneration = 0;
		m_control.context = 0;
	}

	for (index = 0U; index < m_partitionCount; ++index)
	{
		DeterministicCrcPartitionCapture clearResult(
			&m_partitionResults[index],
			m_partitionInputs[index].legacyStorage,
			m_partitionInputs[index].legacyOperationCapacity);
		m_partitionResults[index].key = m_fixedPlan[index];
		m_partitionResults[index].generation = m_generation;
		m_partitionResults[index].physicalWorkerId =
			DETERMINISTIC_CRC_RUNTIME_INVALID_PHYSICAL_WORKER_ID;
		m_partitionResults[index].capturedLanes = m_capturedLanes;
	}
	m_prepared = true;
	return DETERMINISTIC_CRC_RUNTIME_PREPARED;
}

void DeterministicCrcRuntimeAdapter::rejectPartition(
	size_t partitionIndex, uint32_t physicalWorkerId)
{
	DeterministicCrcPartitionCapture capture(
		&m_partitionResults[partitionIndex],
		m_partitionInputs[partitionIndex].legacyStorage,
		m_partitionInputs[partitionIndex].legacyOperationCapacity);
	if (capture.begin(m_fixedPlan[partitionIndex], m_generation,
		physicalWorkerId, m_capturedLanes,
		m_hasControl ? &m_control : 0))
	{
		capture.fail();
	}
}

DeterministicCrcRuntimeWorkStatus
DeterministicCrcRuntimeAdapter::executePartition(
	size_t partitionIndex, uint32_t physicalWorkerId)
{
	if (!m_prepared) return DETERMINISTIC_CRC_RUNTIME_WORK_NOT_PREPARED;
	if (partitionIndex >= m_partitionCount)
		return DETERMINISTIC_CRC_RUNTIME_WORK_INVALID_PARTITION;
	if (physicalWorkerId ==
		DETERMINISTIC_CRC_RUNTIME_INVALID_PHYSICAL_WORKER_ID || isOwner())
	{
		rejectPartition(partitionIndex, physicalWorkerId);
		return DETERMINISTIC_CRC_RUNTIME_WORK_OWNER_EXECUTION_REJECTED;
	}

	DeterministicCrcPartitionCapture capture(
		&m_partitionResults[partitionIndex],
		m_partitionInputs[partitionIndex].legacyStorage,
		m_partitionInputs[partitionIndex].legacyOperationCapacity);
	if (!capture.begin(m_fixedPlan[partitionIndex], m_generation,
		physicalWorkerId, m_capturedLanes,
		m_hasControl ? &m_control : 0))
	{
		return WorkStatusForResult(m_partitionResults[partitionIndex]);
	}

	const bool captured = m_partitionInputs[partitionIndex].capture(
		m_partitionInputs[partitionIndex].immutableInput,
		m_partitionInputs[partitionIndex].immutableInputBytes,
		&capture);
	// The adapter alone owns the terminal transition. A callback that calls
	// complete cannot smuggle a terminal slot past a false return or bypass the
	// adapter's final control probe.
	if (m_partitionResults[partitionIndex].status ==
		DETERMINISTIC_CRC_CAPTURE_COMPLETE)
	{
		m_partitionResults[partitionIndex].status =
			DETERMINISTIC_CRC_CAPTURE_FAILED;
		m_partitionResults[partitionIndex].error =
			DETERMINISTIC_CRC_CAPTURE_ERROR_CALLER_FAILURE;
		return DETERMINISTIC_CRC_RUNTIME_WORK_FAILED;
	}
	if (!captured)
	{
		capture.fail();
		return WorkStatusForResult(m_partitionResults[partitionIndex]);
	}
	if (!capture.complete())
		return WorkStatusForResult(m_partitionResults[partitionIndex]);
	return DETERMINISTIC_CRC_RUNTIME_WORK_SUCCEEDED;
}

DeterministicCrcRuntimeFinalizeStatus
DeterministicCrcRuntimeAdapter::finalize(
	DeterministicCrcLane lane,
	uint32_t serialOracleChecksum,
	DeterministicCrcRuntimeDecision *decision)
{
	DeterministicCrcRuntimeDecision local;
	DeterministicCrcFoldedChecksum folded;
	DeterministicCrcFoldStatus foldStatus;
	DeterministicCrcRuntimeFinalizeStatus status;

	if (decision == 0 ||
		(lane != DETERMINISTIC_CRC_LEGACY_XFER &&
		 lane != DETERMINISTIC_CRC_CURRENT_REPLAY_EPOCH))
	{
		return DETERMINISTIC_CRC_RUNTIME_FINALIZE_INVALID_ARGUMENT;
	}
	InitializeDecision(&local,
		DETERMINISTIC_CRC_RUNTIME_FINALIZE_NOT_PREPARED,
		lane, m_generation, serialOracleChecksum);
	if (!m_prepared)
	{
		*decision = local;
		return local.status;
	}
	if (!isOwner())
	{
		local.status = DETERMINISTIC_CRC_RUNTIME_FINALIZE_NOT_OWNER;
		*decision = local;
		return local.status;
	}
	if (!isCurrentGeneration())
	{
		local.status =
			DETERMINISTIC_CRC_RUNTIME_FINALIZE_STALE_GENERATION;
		m_prepared = false;
		*decision = local;
		return local.status;
	}
	if (isCancelled())
	{
		local.status = DETERMINISTIC_CRC_RUNTIME_FINALIZE_CANCELLED;
		m_prepared = false;
		*decision = local;
		return local.status;
	}

	foldStatus = FoldDeterministicCrcRanges(m_fixedPlan, m_partitionCount,
		m_partitionResults, m_partitionCount, m_generation, lane, &folded);
	local.foldStatus = foldStatus;
	if (foldStatus != DETERMINISTIC_CRC_FOLD_OK)
	{
		status = FinalizeStatusForFold(foldStatus);
		local.status = status;
		local.rejectedPartition = FindRejectedPartition(
			m_partitionResults, m_partitionCount, m_generation);
		m_prepared = false;
		*decision = local;
		return status;
	}

	local.folded = folded;
	local.parallelChecksum = folded.checksum;
	local.parallelChecksumAvailable = true;
	if (folded.checksum != serialOracleChecksum)
	{
		local.status = DETERMINISTIC_CRC_RUNTIME_FINALIZE_ORACLE_MISMATCH;
		m_prepared = false;
		*decision = local;
		return local.status;
	}

	local.status = DETERMINISTIC_CRC_RUNTIME_FINALIZE_VERIFIED;
	local.parallelVerified = true;
	m_prepared = false;
	*decision = local;
	return local.status;
}

bool DeterministicCrcRuntimeAdapter::isPrepared() const
{
	return m_prepared;
}

uint32_t DeterministicCrcRuntimeAdapter::generation() const
{
	return m_generation;
}

size_t DeterministicCrcRuntimeAdapter::partitionCount() const
{
	return m_partitionCount;
}

} // namespace rts
