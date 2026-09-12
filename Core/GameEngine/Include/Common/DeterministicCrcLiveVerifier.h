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

#include "Common/XferCRCSnapshot.h"
#include "Lib/DeterministicCrcJobSystemAdapter.h"
#include "Lib/SimulationExecutionPolicy.h"

namespace rts
{

enum DeterministicCrcLivePartition
{
	DETERMINISTIC_CRC_LIVE_OBJECTS = 0,
	DETERMINISTIC_CRC_LIVE_RANDOM_SEED,
	DETERMINISTIC_CRC_LIVE_PARTITION_MANAGER,
	DETERMINISTIC_CRC_LIVE_PLAYER_LIST,
	DETERMINISTIC_CRC_LIVE_AI,
	DETERMINISTIC_CRC_LIVE_PARTITION_COUNT
};

enum DeterministicCrcLiveStatus
{
	DETERMINISTIC_CRC_LIVE_DISABLED = 0,
	DETERMINISTIC_CRC_LIVE_VERIFIED,
	DETERMINISTIC_CRC_LIVE_SNAPSHOT_FALLBACK,
	DETERMINISTIC_CRC_LIVE_ALLOCATION_FALLBACK,
	DETERMINISTIC_CRC_LIVE_JOB_FALLBACK,
	DETERMINISTIC_CRC_LIVE_ORACLE_MISMATCH
};

struct DeterministicCrcLiveResult
{
	DeterministicCrcLiveResult();

	DeterministicCrcLiveStatus status;
	SimulationExecutionMode mode;
	uint32_t generation;
	uint32_t serialOracleChecksum;
	uint32_t parallelChecksum;
	uint32_t selectedChecksum;
	size_t capturedPartitionCount;
	size_t completedPartitionCount;
	uint32_t distinctPhysicalWorkerCount;
	uint64_t physicalWorkerMask;
	bool physicalWorkerMaskComplete;
	bool workerExecutionObserved;
	bool oracleMatched;
	DeterministicCrcJobSystemResult jobSystem;
};

struct DeterministicCrcLiveMetrics
{
	DeterministicCrcLiveMetrics();

	JobMetricCounter resetEpoch;
	JobMetricCounter attempts;
	JobMetricCounter verifiedWaves;
	JobMetricCounter fallbackWaves;
	JobMetricCounter snapshotFallbacks;
	JobMetricCounter allocationFallbacks;
	JobMetricCounter jobFallbacks;
	JobMetricCounter oracleMismatches;
	JobMetricCounter capturedPartitions;
	JobMetricCounter completedPartitions;
	JobMetricCounter parallelAttempts;
	JobMetricCounter parallelVerified;
	JobMetricCounter parallelMismatches;
	JobMetricCounter shadowAttempts;
	JobMetricCounter shadowMatches;
	JobMetricCounter shadowMismatches;
	JobMetricCounter epochSkips;
	JobMetricCounter executedWaves;
	JobMetricCounter oracleMatches;
	uint32_t maximumDistinctPhysicalWorkerCount;
	uint64_t physicalWorkerMask;
	bool physicalWorkerMaskComplete;
};

void ResetDeterministicCrcLiveMetrics();
DeterministicCrcLiveMetrics GetDeterministicCrcLiveMetrics();

// Fresh games use the current executable epoch. Playback is eligible only
// after the title has positively identified its current replay markers;
// legacy and unmarked recordings stay on the unchanged serial CRC path.
bool IsDeterministicCrcLiveEpochEligible(bool playbackMode,
	bool skirmishAiUsesCurrentEpoch,
	bool pathfindQueueUsesCurrentEpoch);

// One owner-scoped light-CRC verification wave. The caller writes the exact
// legacy stream through xfer(), bracketing the five fixed canonical sections
// in enum order. verify() synchronously drains all worker work and always
// retains serialOracleChecksum as the selected/public checksum.
class DeterministicCrcLiveVerifier
{
public:
	DeterministicCrcLiveVerifier();
	~DeterministicCrcLiveVerifier();

	bool begin(SimulationExecutionMode mode);
	XferCRCSnapshot *xfer();
	bool beginPartition(DeterministicCrcLivePartition partition);
	bool endPartition(DeterministicCrcLivePartition partition);
	DeterministicCrcLiveStatus verify(uint32_t serialOracleChecksum,
		DeterministicCrcLiveResult *result);
	bool isActive() const;
	uint32_t generation() const;

private:
	DeterministicCrcLiveVerifier(const DeterministicCrcLiveVerifier &);
	DeterministicCrcLiveVerifier &operator=(
		const DeterministicCrcLiveVerifier &);

	struct PartitionStorage;

	static bool GrowSnapshotStorage(
		DeterministicCrcSnapshotEncoder *encoder,
		size_t requiredStorageCapacity,
		void *context);
	static bool IsOwner(void *context);
	static bool IsCancelled(void *context);
	static uint32_t CurrentGeneration(void *context);

	bool growSnapshotStorage(PartitionStorage *partition,
		size_t requiredStorageCapacity);
	void recordStatus(DeterministicCrcLiveStatus status,
		size_t capturedPartitionCount, size_t completedPartitionCount);
	void releaseStorage();

	XferCRCSnapshot m_xfer;
	PartitionStorage *m_partitions;
	SimulationExecutionMode m_mode;
	uint32_t m_generation;
	size_t m_nextPartition;
	size_t m_activePartition;
	size_t m_totalSnapshotCapacity;
	bool m_active;
	bool m_captureFailed;
	bool m_allocationFailed;
};

} // namespace rts
