/*
** Command & Conquer Generals(tm)
** Copyright 2026 TheSuperHackers
**
** This program is free software: you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation, either version 3 of the License, or
** (at your option) any later version.
*/

#include "Lib/DeterministicCrcJobSystemAdapter.h"
#include "Lib/DeterministicCrcSnapshot.h"

#include <stdio.h>

namespace
{

enum
{
	PARTITION_COUNT = 16,
	SNAPSHOT_CAPACITY = 32
};

int Check(bool condition, const char *message)
{
	if (condition) return 0;
	fprintf(stderr, "FAIL: %s\n", message);
	return 1;
}

bool IsGameOwner(void *)
{
	return rts::JobSystem::instance().isCurrentThread(rts::JOB_OWNER_GAME);
}

uint32_t ByteSwap32(uint32_t value)
{
	return ((value & 0x000000ffU) << 24U) |
		((value & 0x0000ff00U) << 8U) |
		((value & 0x00ff0000U) >> 8U) |
		((value & 0xff000000U) >> 24U);
}

uint32_t ReferenceLegacyChecksum(const uint32_t *values, size_t valueCount)
{
	uint32_t state = 0U;
	for (size_t index = 0U; index < valueCount; ++index)
	{
		const uint32_t operation = ByteSwap32(values[index]);
		state = (state << 1U) + operation + ((state >> 31U) & 1U);
	}
	return ByteSwap32(state);
}

#if !defined(_MSC_VER) || _MSC_VER >= 1300
int TestFixedWorkerCountsPreserveOracle()
{
	const unsigned workerCounts[] = { 1U, 2U, 4U, 8U, 16U };
	uint8_t snapshotStorage[PARTITION_COUNT][SNAPSHOT_CAPACITY];
	rts::DeterministicCrcSnapshotEncoder encoders[PARTITION_COUNT];
	rts::DeterministicCrcRangeKey plan[PARTITION_COUNT];
	rts::DeterministicCrcRuntimePartitionInput inputs[PARTITION_COUNT];
	rts::DeterministicLegacyXferOperation legacyStorage[PARTITION_COUNT][1];
	rts::DeterministicCrcPartitionResult results[PARTITION_COUNT];
	uint32_t values[PARTITION_COUNT];
	int result = 0;

	for (size_t index = 0U; index < PARTITION_COUNT; ++index)
	{
		const void *immutableInput = 0;
		size_t immutableInputBytes = 0U;
		values[index] = 0x10203040U + static_cast<uint32_t>(index * 0x01010101U);
		plan[index].sectionId = 1U;
		plan[index].partitionId = static_cast<uint32_t>(index);
		plan[index].byteBegin = index * 4U;
		plan[index].byteEnd = plan[index].byteBegin + 4U;
		result |= Check(encoders[index].begin(snapshotStorage[index],
			SNAPSHOT_CAPACITY) && encoders[index].appendUInt32(values[index]) &&
			encoders[index].seal(4U, &immutableInput, &immutableInputBytes),
			"owner builds every fixed immutable partition snapshot");
		inputs[index].immutableInput = immutableInput;
		inputs[index].immutableInputBytes = immutableInputBytes;
		inputs[index].capture = rts::CaptureDeterministicCrcSnapshot;
		inputs[index].legacyStorage = legacyStorage[index];
		inputs[index].legacyOperationCapacity = 1U;
	}

	const uint32_t serialOracle = ReferenceLegacyChecksum(values,
		PARTITION_COUNT);
	rts::DeterministicCrcRuntimeAdapter runtime(IsGameOwner, 0);
	rts::DeterministicCrcJobSystemOptions options;
	options.physicalWaitMilliseconds = 5000U;
	for (size_t run = 0U;
		run < sizeof(workerCounts) / sizeof(workerCounts[0]); ++run)
	{
		rts::JobSystemConfig config;
		config.workerCount = workerCounts[run];
		config.queueCapacity = PARTITION_COUNT;
		config.scratchBytesPerWorker = 4096U;
		config.pinWorkers = false;
		rts::JobSystem &jobs = rts::JobSystem::instance();
		result |= Check(jobs.start(config) &&
			jobs.registerCurrentThread(rts::JOB_OWNER_GAME),
			"configured job system and game owner start");

		rts::DeterministicCrcJobSystemResult verification;
		const rts::DeterministicCrcJobSystemStatus status =
			rts::VerifyDeterministicCrcWithJobSystem(&runtime, plan, inputs,
				results, PARTITION_COUNT, static_cast<uint32_t>(run + 1U),
				rts::DETERMINISTIC_CRC_CAPTURE_BOTH, 0,
				rts::DETERMINISTIC_CRC_LEGACY_XFER, serialOracle,
				options, &verification);
		result |= Check(status == rts::DETERMINISTIC_CRC_JOB_SYSTEM_VERIFIED &&
			verification.decision.parallelVerified &&
			verification.decision.parallelChecksum == serialOracle &&
			verification.decision.selectedChecksum == serialOracle,
			"1/2/4/8/16 physical workers preserve the serial CRC oracle");
		for (size_t index = 0U; index < PARTITION_COUNT; ++index)
		{
			result |= Check(results[index].physicalWorkerId < workerCounts[run],
				"every accepted partition records a real scheduler worker index");
		}
		result |= Check(jobs.unregisterCurrentThread(rts::JOB_OWNER_GAME),
			"game owner unregisters after verification");
		jobs.shutdown();
	}
	return result;
}
#endif

} // namespace

int main()
{
	int result = 0;
#if defined(_MSC_VER) && _MSC_VER < 1300
	// The production adapter is deliberately gated off from the synchronous
	// reference runtime; JobSystemTest covers its invalid identity directly.
	printf("Deterministic CRC JobSystem adapter unavailable on VC6 as expected.\n");
#else
	result |= TestFixedWorkerCountsPreserveOracle();
	if (result == 0)
		printf("Deterministic CRC JobSystem adapter tests passed.\n");
#endif
	return result;
}
