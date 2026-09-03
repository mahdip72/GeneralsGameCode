/*
** Command & Conquer Generals(tm)
** Copyright 2026 TheSuperHackers
**
** This program is free software: you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation, either version 3 of the License, or
** (at your option) any later version.
*/

#include "PreRTS.h" // This must go first in EVERY cpp file in the GameEngine

#include "Common/DeterministicCrcLiveVerifier.h"

#include "Common/GameThreadOwnership.h"
#include "Common/Recorder.h"

#include <new>
#include <string.h>

namespace rts
{
namespace
{

const size_t INITIAL_SNAPSHOT_CAPACITY = 4096U;
const size_t MAXIMUM_SNAPSHOT_CAPACITY = 64U * 1024U * 1024U;

DeterministicCrcLiveMetrics s_metrics;
uint32_t s_nextGeneration = 0U;

uint32_t NextGeneration()
{
	++s_nextGeneration;
	if (s_nextGeneration == 0U) ++s_nextGeneration;
	return s_nextGeneration;
}

bool ProductReplayUsesCurrentDeterministicCrcEpoch()
{
	if (TheRecorder == 0 || !TheRecorder->isPlaybackMode()) return true;
#if defined(RTS_GENERALS) && RTS_GENERALS
	return IsDeterministicCrcLiveEpochEligible(true,
		TheRecorder->replayUsesSkirmishAIDeterministicPlanning(), true);
#else
	return IsDeterministicCrcLiveEpochEligible(true,
		TheRecorder->replayUsesSkirmishAICounterRng(),
		TheRecorder->replayUsesPathfindQueueCapacity());
#endif
}

} // namespace

struct DeterministicCrcLiveVerifier::PartitionStorage
{
	PartitionStorage()
		: owner(0), snapshotStorage(0), snapshotCapacity(0U),
		  immutableInput(0), immutableInputBytes(0U),
		  legacyStorage(0), legacyCapacity(0U), sealed(false)
	{
		plan.sectionId = 0U;
		plan.partitionId = 0U;
		plan.byteBegin = 0U;
		plan.byteEnd = 0U;
	}

	DeterministicCrcLiveVerifier *owner;
	DeterministicCrcSnapshotEncoder encoder;
	unsigned char *snapshotStorage;
	size_t snapshotCapacity;
	const void *immutableInput;
	size_t immutableInputBytes;
	DeterministicLegacyXferOperation *legacyStorage;
	size_t legacyCapacity;
	DeterministicCrcRangeKey plan;
	bool sealed;
};

DeterministicCrcLiveResult::DeterministicCrcLiveResult()
	: status(DETERMINISTIC_CRC_LIVE_DISABLED),
	  mode(SIMULATION_EXECUTION_SERIAL),
	  generation(0U),
	  serialOracleChecksum(0U),
	  parallelChecksum(0U),
	  selectedChecksum(0U),
	  capturedPartitionCount(0U),
	  completedPartitionCount(0U),
	  distinctPhysicalWorkerCount(0U),
	  physicalWorkerMask(0U),
	  physicalWorkerMaskComplete(true),
	  workerExecutionObserved(false),
	  oracleMatched(false)
{
}

DeterministicCrcLiveMetrics::DeterministicCrcLiveMetrics()
	: resetEpoch(0), attempts(0), verifiedWaves(0), fallbackWaves(0),
	  snapshotFallbacks(0), allocationFallbacks(0), jobFallbacks(0),
	  oracleMismatches(0), capturedPartitions(0), completedPartitions(0),
	  parallelAttempts(0), parallelVerified(0), parallelMismatches(0),
	  shadowAttempts(0), shadowMatches(0), shadowMismatches(0),
	  epochSkips(0), executedWaves(0), oracleMatches(0),
	  maximumDistinctPhysicalWorkerCount(0U), physicalWorkerMask(0U),
	  physicalWorkerMaskComplete(true)
{
}

bool IsDeterministicCrcLiveEpochEligible(bool playbackMode,
	bool skirmishAiUsesCurrentEpoch,
	bool pathfindQueueUsesCurrentEpoch)
{
	if (!playbackMode) return true;
#if defined(RTS_GENERALS) && RTS_GENERALS
	(void)pathfindQueueUsesCurrentEpoch;
	return skirmishAiUsesCurrentEpoch;
#else
	return skirmishAiUsesCurrentEpoch && pathfindQueueUsesCurrentEpoch;
#endif
}

void ResetDeterministicCrcLiveMetrics()
{
	JobMetricCounter nextEpoch = s_metrics.resetEpoch + 1;
	if (nextEpoch == 0) nextEpoch = 1;
	s_metrics = DeterministicCrcLiveMetrics();
	s_metrics.resetEpoch = nextEpoch;
}

DeterministicCrcLiveMetrics GetDeterministicCrcLiveMetrics()
{
	return s_metrics;
}

DeterministicCrcLiveVerifier::DeterministicCrcLiveVerifier()
	: m_partitions(0), m_mode(SIMULATION_EXECUTION_SERIAL),
	  m_generation(0U), m_nextPartition(0U),
	  m_activePartition(DETERMINISTIC_CRC_LIVE_PARTITION_COUNT),
	  m_totalSnapshotCapacity(0U), m_active(false),
	  m_captureFailed(false), m_allocationFailed(false)
{
}

DeterministicCrcLiveVerifier::~DeterministicCrcLiveVerifier()
{
	releaseStorage();
	delete[] m_partitions;
}

bool DeterministicCrcLiveVerifier::begin(SimulationExecutionMode mode)
{
	if (m_active || (mode != SIMULATION_EXECUTION_PARALLEL &&
		mode != SIMULATION_EXECUTION_SHADOW))
	{
		return false;
	}
	if (!ProductReplayUsesCurrentDeterministicCrcEpoch())
	{
		++s_metrics.epochSkips;
		return false;
	}

	m_mode = mode;
	++s_metrics.attempts;
	if (mode == SIMULATION_EXECUTION_PARALLEL) ++s_metrics.parallelAttempts;
	else ++s_metrics.shadowAttempts;

#if defined(_MSC_VER) && _MSC_VER < 1300
	recordStatus(DETERMINISTIC_CRC_LIVE_JOB_FALLBACK, 0U, 0U);
	return false;
#else
	JobSystem &jobs = JobSystem::instance();
	if (!GameThreadOwnership::IsAttached() ||
		!GameThreadOwnership::IsCurrentThread() || !jobs.isRunning() ||
		jobs.workerCount() == 0U || jobs.isWorkerThread() ||
		!jobs.isCurrentThread(JOB_OWNER_GAME))
	{
		recordStatus(DETERMINISTIC_CRC_LIVE_JOB_FALLBACK, 0U, 0U);
		return false;
	}

	m_partitions = new (std::nothrow)
		PartitionStorage[DETERMINISTIC_CRC_LIVE_PARTITION_COUNT];
	if (m_partitions == 0)
	{
		m_allocationFailed = true;
		recordStatus(DETERMINISTIC_CRC_LIVE_ALLOCATION_FALLBACK, 0U, 0U);
		return false;
	}
	for (size_t index = 0U;
		index < DETERMINISTIC_CRC_LIVE_PARTITION_COUNT; ++index)
	{
		m_partitions[index].owner = this;
	}
	m_generation = NextGeneration();
	m_active = true;
	return true;
#endif
}

XferCRCSnapshot *DeterministicCrcLiveVerifier::xfer()
{
	return m_active ? &m_xfer : 0;
}

bool DeterministicCrcLiveVerifier::beginPartition(
	DeterministicCrcLivePartition partition)
{
	const size_t partitionIndex = static_cast<size_t>(partition);
	if (!m_active || m_captureFailed || m_partitions == 0 ||
		partitionIndex != m_nextPartition ||
		m_activePartition != DETERMINISTIC_CRC_LIVE_PARTITION_COUNT)
	{
		m_captureFailed = true;
		return false;
	}

	PartitionStorage &storage = m_partitions[partitionIndex];
	if (storage.snapshotStorage == 0 &&
		!growSnapshotStorage(&storage, INITIAL_SNAPSHOT_CAPACITY))
	{
		m_captureFailed = true;
		m_allocationFailed = true;
		return false;
	}
	if (!m_xfer.beginSnapshotPartition(&storage.encoder,
		storage.snapshotStorage, storage.snapshotCapacity,
		&GrowSnapshotStorage, &storage))
	{
		m_captureFailed = true;
		return false;
	}
	m_activePartition = partitionIndex;
	return true;
}

bool DeterministicCrcLiveVerifier::endPartition(
	DeterministicCrcLivePartition partition)
{
	const size_t partitionIndex = static_cast<size_t>(partition);
	if (!m_active || m_partitions == 0 ||
		partitionIndex != m_activePartition ||
		partitionIndex != m_nextPartition)
	{
		m_captureFailed = true;
		return false;
	}

	PartitionStorage &storage = m_partitions[partitionIndex];
	const uint64_t payloadByteCount = storage.encoder.payloadByteCount();
	const bool sealed = m_xfer.endSnapshotPartition(payloadByteCount,
		&storage.immutableInput, &storage.immutableInputBytes);
	m_activePartition = DETERMINISTIC_CRC_LIVE_PARTITION_COUNT;
	if (!sealed)
	{
		m_captureFailed = true;
		return false;
	}

	storage.plan.sectionId = static_cast<uint32_t>(partitionIndex + 1U);
	storage.plan.partitionId = 0U;
	storage.plan.byteBegin = 0U;
	storage.plan.byteEnd = payloadByteCount;
	storage.sealed = true;
	++m_nextPartition;
	return true;
}

bool DeterministicCrcLiveVerifier::GrowSnapshotStorage(
	DeterministicCrcSnapshotEncoder *encoder,
	size_t requiredStorageCapacity, void *context)
{
	PartitionStorage *partition = static_cast<PartitionStorage *>(context);
	return encoder != 0 && partition != 0 && partition->owner != 0 &&
		partition->owner->growSnapshotStorage(partition,
			requiredStorageCapacity);
}

bool DeterministicCrcLiveVerifier::growSnapshotStorage(
	PartitionStorage *partition, size_t requiredStorageCapacity)
{
	size_t newCapacity;
	unsigned char *newStorage;
	if (partition == 0 || requiredStorageCapacity == 0U ||
		requiredStorageCapacity > MAXIMUM_SNAPSHOT_CAPACITY)
	{
		m_allocationFailed = true;
		return false;
	}

	newCapacity = partition->snapshotCapacity != 0U ?
		partition->snapshotCapacity : INITIAL_SNAPSHOT_CAPACITY;
	while (newCapacity < requiredStorageCapacity)
	{
		if (newCapacity > MAXIMUM_SNAPSHOT_CAPACITY / 2U)
		{
			newCapacity = MAXIMUM_SNAPSHOT_CAPACITY;
			break;
		}
		newCapacity *= 2U;
	}
	if (newCapacity < requiredStorageCapacity ||
		m_totalSnapshotCapacity - partition->snapshotCapacity >
			MAXIMUM_SNAPSHOT_CAPACITY - newCapacity)
	{
		m_allocationFailed = true;
		return false;
	}

#if defined(_MSC_VER) && _MSC_VER < 1300
	// VC6's STLPort does not provide the array form of nothrow new. Its
	// compiler runtime retains the legacy null-returning allocation contract.
	newStorage = new unsigned char[newCapacity];
#else
	newStorage = new (std::nothrow) unsigned char[newCapacity];
#endif
	if (newStorage == 0)
	{
		m_allocationFailed = true;
		return false;
	}
	if (partition->snapshotStorage != 0 &&
		!partition->encoder.rebindStorage(newStorage, newCapacity))
	{
		delete[] newStorage;
		m_allocationFailed = true;
		return false;
	}

	m_totalSnapshotCapacity = m_totalSnapshotCapacity -
		partition->snapshotCapacity + newCapacity;
	delete[] partition->snapshotStorage;
	partition->snapshotStorage = newStorage;
	partition->snapshotCapacity = newCapacity;
	return true;
}

bool DeterministicCrcLiveVerifier::IsOwner(void *context)
{
	DeterministicCrcLiveVerifier *verifier =
		static_cast<DeterministicCrcLiveVerifier *>(context);
	return verifier != 0 && GameThreadOwnership::IsAttached() &&
		GameThreadOwnership::IsCurrentThread() &&
		JobSystem::instance().isCurrentThread(JOB_OWNER_GAME);
}

bool DeterministicCrcLiveVerifier::IsCancelled(void *context)
{
	(void)context;
	return false;
}

uint32_t DeterministicCrcLiveVerifier::CurrentGeneration(void *context)
{
	DeterministicCrcLiveVerifier *verifier =
		static_cast<DeterministicCrcLiveVerifier *>(context);
	return verifier != 0 ? verifier->m_generation : 0U;
}

DeterministicCrcLiveStatus DeterministicCrcLiveVerifier::verify(
	uint32_t serialOracleChecksum, DeterministicCrcLiveResult *result)
{
	DeterministicCrcRangeKey plan[DETERMINISTIC_CRC_LIVE_PARTITION_COUNT];
	DeterministicCrcRuntimePartitionInput inputs[
		DETERMINISTIC_CRC_LIVE_PARTITION_COUNT];
	DeterministicCrcPartitionResult partitionResults[
		DETERMINISTIC_CRC_LIVE_PARTITION_COUNT];
	DeterministicCrcCaptureControl control;
	DeterministicCrcRuntimeAdapter runtime(&IsOwner, this);
	DeterministicCrcJobSystemOptions options;
	size_t capturedPartitionCount = 0U;
	size_t completedPartitionCount = 0U;

	if (result == 0) return DETERMINISTIC_CRC_LIVE_JOB_FALLBACK;
	*result = DeterministicCrcLiveResult();
	result->mode = m_mode;
	result->generation = m_generation;
	result->serialOracleChecksum = serialOracleChecksum;
	result->selectedChecksum = serialOracleChecksum;

	if (!m_active || m_partitions == 0 || m_captureFailed ||
		m_activePartition != DETERMINISTIC_CRC_LIVE_PARTITION_COUNT ||
		m_nextPartition != DETERMINISTIC_CRC_LIVE_PARTITION_COUNT)
	{
		const DeterministicCrcLiveStatus status = m_allocationFailed ?
			DETERMINISTIC_CRC_LIVE_ALLOCATION_FALLBACK :
			DETERMINISTIC_CRC_LIVE_SNAPSHOT_FALLBACK;
		for (size_t index = 0U; m_partitions != 0 &&
			index < DETERMINISTIC_CRC_LIVE_PARTITION_COUNT; ++index)
		{
			if (m_partitions[index].sealed) ++capturedPartitionCount;
		}
		result->status = status;
		result->capturedPartitionCount = capturedPartitionCount;
		recordStatus(status, capturedPartitionCount, 0U);
		m_active = false;
		return status;
	}

	for (size_t index = 0U;
		index < DETERMINISTIC_CRC_LIVE_PARTITION_COUNT; ++index)
	{
		PartitionStorage &storage = m_partitions[index];
		storage.legacyCapacity =
			storage.encoder.requiredLegacyOperationCapacity();
		if (storage.legacyCapacity == 0U ||
			storage.legacyCapacity > SIZE_MAX /
				sizeof(DeterministicLegacyXferOperation))
		{
			m_allocationFailed = true;
			break;
		}
#if defined(_MSC_VER) && _MSC_VER < 1300
		storage.legacyStorage =
			new DeterministicLegacyXferOperation[storage.legacyCapacity];
#else
		storage.legacyStorage = new (std::nothrow)
			DeterministicLegacyXferOperation[storage.legacyCapacity];
#endif
		if (storage.legacyStorage == 0)
		{
			m_allocationFailed = true;
			break;
		}

		plan[index] = storage.plan;
		inputs[index].immutableInput = storage.immutableInput;
		inputs[index].immutableInputBytes = storage.immutableInputBytes;
		inputs[index].capture = &CaptureDeterministicCrcSnapshot;
		inputs[index].legacyStorage = storage.legacyStorage;
		inputs[index].legacyOperationCapacity = storage.legacyCapacity;
	}
	capturedPartitionCount = DETERMINISTIC_CRC_LIVE_PARTITION_COUNT;
	if (m_allocationFailed ||
		m_partitions[DETERMINISTIC_CRC_LIVE_PARTITION_COUNT - 1U].legacyStorage == 0)
	{
		result->status = DETERMINISTIC_CRC_LIVE_ALLOCATION_FALLBACK;
		result->capturedPartitionCount = capturedPartitionCount;
		recordStatus(result->status, capturedPartitionCount, 0U);
		m_active = false;
		return result->status;
	}

	control.isCancelled = &IsCancelled;
	control.currentGeneration = &CurrentGeneration;
	control.context = this;
	memset(partitionResults, 0, sizeof(partitionResults));
	VerifyDeterministicCrcWithJobSystem(&runtime, plan, inputs,
		partitionResults, DETERMINISTIC_CRC_LIVE_PARTITION_COUNT,
		m_generation, DETERMINISTIC_CRC_CAPTURE_LEGACY, &control,
		DETERMINISTIC_CRC_LEGACY_XFER, serialOracleChecksum, options,
		&result->jobSystem);
	for (size_t resultIndex = 0U;
		resultIndex < DETERMINISTIC_CRC_LIVE_PARTITION_COUNT; ++resultIndex)
	{
		if (partitionResults[resultIndex].status ==
			DETERMINISTIC_CRC_CAPTURE_COMPLETE)
			++completedPartitionCount;
	}

	result->capturedPartitionCount = capturedPartitionCount;
	result->completedPartitionCount = completedPartitionCount;
	result->parallelChecksum = result->jobSystem.decision.parallelChecksum;
	result->distinctPhysicalWorkerCount =
		result->jobSystem.decision.folded.distinctPhysicalWorkerCount;
	result->physicalWorkerMask =
		result->jobSystem.decision.folded.physicalWorkerMask;
	result->physicalWorkerMaskComplete =
		result->jobSystem.decision.folded.physicalWorkerMaskComplete;
	result->workerExecutionObserved =
		result->jobSystem.decision.parallelChecksumAvailable &&
		completedPartitionCount == DETERMINISTIC_CRC_LIVE_PARTITION_COUNT &&
		result->distinctPhysicalWorkerCount != 0U;
	result->oracleMatched = result->workerExecutionObserved &&
		result->jobSystem.decision.parallelVerified;
	if (result->jobSystem.status == DETERMINISTIC_CRC_JOB_SYSTEM_VERIFIED &&
		result->oracleMatched)
		result->status = DETERMINISTIC_CRC_LIVE_VERIFIED;
	else if (result->jobSystem.finalizeStatus ==
		DETERMINISTIC_CRC_RUNTIME_FINALIZE_ORACLE_MISMATCH)
		result->status = DETERMINISTIC_CRC_LIVE_ORACLE_MISMATCH;
	else
		result->status = DETERMINISTIC_CRC_LIVE_JOB_FALLBACK;

	// Publication remains the original XferCRC result in every mode/status.
	result->jobSystem.decision.selectedChecksum = serialOracleChecksum;
	result->selectedChecksum = serialOracleChecksum;
	if (result->workerExecutionObserved)
	{
		++s_metrics.executedWaves;
		if (result->oracleMatched) ++s_metrics.oracleMatches;
		if (s_metrics.maximumDistinctPhysicalWorkerCount <
			result->distinctPhysicalWorkerCount)
		{
			s_metrics.maximumDistinctPhysicalWorkerCount =
				result->distinctPhysicalWorkerCount;
		}
		s_metrics.physicalWorkerMask |= result->physicalWorkerMask;
		if (!result->physicalWorkerMaskComplete)
			s_metrics.physicalWorkerMaskComplete = false;
	}
	recordStatus(result->status, capturedPartitionCount,
		completedPartitionCount);
	m_active = false;
	return result->status;
}

bool DeterministicCrcLiveVerifier::isActive() const
{
	return m_active;
}

uint32_t DeterministicCrcLiveVerifier::generation() const
{
	return m_generation;
}

void DeterministicCrcLiveVerifier::recordStatus(
	DeterministicCrcLiveStatus status, size_t capturedPartitionCount,
	size_t completedPartitionCount)
{
	s_metrics.capturedPartitions += capturedPartitionCount;
	s_metrics.completedPartitions += completedPartitionCount;
	if (status == DETERMINISTIC_CRC_LIVE_VERIFIED)
	{
		++s_metrics.verifiedWaves;
		if (m_mode == SIMULATION_EXECUTION_PARALLEL)
			++s_metrics.parallelVerified;
		else if (m_mode == SIMULATION_EXECUTION_SHADOW)
			++s_metrics.shadowMatches;
		return;
	}

	if (status == DETERMINISTIC_CRC_LIVE_ORACLE_MISMATCH)
	{
		++s_metrics.oracleMismatches;
		if (m_mode == SIMULATION_EXECUTION_PARALLEL)
			++s_metrics.parallelMismatches;
		else if (m_mode == SIMULATION_EXECUTION_SHADOW)
			++s_metrics.shadowMismatches;
		return;
	}

	++s_metrics.fallbackWaves;
	if (status == DETERMINISTIC_CRC_LIVE_SNAPSHOT_FALLBACK)
		++s_metrics.snapshotFallbacks;
	else if (status == DETERMINISTIC_CRC_LIVE_ALLOCATION_FALLBACK)
		++s_metrics.allocationFallbacks;
	else
		++s_metrics.jobFallbacks;
}

void DeterministicCrcLiveVerifier::releaseStorage()
{
	if (m_partitions == 0) return;
	for (size_t index = 0U;
		index < DETERMINISTIC_CRC_LIVE_PARTITION_COUNT; ++index)
	{
		delete[] m_partitions[index].legacyStorage;
		m_partitions[index].legacyStorage = 0;
		delete[] m_partitions[index].snapshotStorage;
		m_partitions[index].snapshotStorage = 0;
	}
	m_totalSnapshotCapacity = 0U;
}

} // namespace rts
