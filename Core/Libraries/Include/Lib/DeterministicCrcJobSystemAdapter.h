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

#include "Lib/DeterministicCrcRuntimeAdapter.h"
#include "Lib/JobSystem.h"

namespace rts
{

enum
{
	DETERMINISTIC_CRC_JOB_SYSTEM_MAX_PARTITIONS = 64,
	DETERMINISTIC_CRC_JOB_SYSTEM_DEFAULT_WAIT_MILLISECONDS = 250
};

enum DeterministicCrcJobSystemStatus
{
	DETERMINISTIC_CRC_JOB_SYSTEM_VERIFIED = 0,
	DETERMINISTIC_CRC_JOB_SYSTEM_INVALID_ARGUMENT,
	DETERMINISTIC_CRC_JOB_SYSTEM_UNAVAILABLE,
	DETERMINISTIC_CRC_JOB_SYSTEM_PREPARE_FALLBACK,
	DETERMINISTIC_CRC_JOB_SYSTEM_GROUP_FALLBACK,
	DETERMINISTIC_CRC_JOB_SYSTEM_ALLOCATION_FALLBACK,
	DETERMINISTIC_CRC_JOB_SYSTEM_ADMISSION_FALLBACK,
	DETERMINISTIC_CRC_JOB_SYSTEM_TIMEOUT_FALLBACK,
	DETERMINISTIC_CRC_JOB_SYSTEM_WORK_FALLBACK,
	DETERMINISTIC_CRC_JOB_SYSTEM_FINALIZE_FALLBACK
};

struct DeterministicCrcJobSystemOptions
{
	DeterministicCrcJobSystemOptions();

	JobPriority priority;
	unsigned physicalWaitMilliseconds;
};

struct DeterministicCrcJobSystemResult
{
	DeterministicCrcJobSystemResult();

	DeterministicCrcJobSystemStatus status;
	DeterministicCrcRuntimePrepareStatus prepareStatus;
	DeterministicCrcRuntimeFinalizeStatus finalizeStatus;
	DeterministicCrcRuntimeDecision decision;
	size_t failedPartition;
	bool timedOut;
};

// Synchronous owner adapter. It submits one fixed job per canonical partition,
// passively waits for physical workers, joins all accepted work, and finalizes
// on the owner. It never owner-helps successful work. Every failure keeps the
// serial oracle selected and drains accepted jobs before caller storage may be
// reused. The VC6 synchronous runtime reports UNAVAILABLE without preparation.
DeterministicCrcJobSystemStatus VerifyDeterministicCrcWithJobSystem(
	DeterministicCrcRuntimeAdapter *runtime,
	const DeterministicCrcRangeKey *fixedPlan,
	const DeterministicCrcRuntimePartitionInput *partitionInputs,
	DeterministicCrcPartitionResult *partitionResults,
	size_t partitionCount,
	uint32_t generation,
	uint32_t capturedLanes,
	const DeterministicCrcCaptureControl *control,
	DeterministicCrcLane lane,
	uint32_t serialOracleChecksum,
	const DeterministicCrcJobSystemOptions &options,
	DeterministicCrcJobSystemResult *result);

} // namespace rts
