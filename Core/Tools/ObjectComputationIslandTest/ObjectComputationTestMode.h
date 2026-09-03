/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2026 TheSuperHackers
** SPDX-License-Identifier: GPL-3.0-or-later
*/
#pragma once

#include "Lib/ObjectComputationIsland.h"

#include <string.h>

namespace rts_test
{

enum ObjectComputationTestMode
{
	OBJECT_COMPUTATION_TEST_STRICT_SCALING = 0,
	OBJECT_COMPUTATION_TEST_ALLOW_SERIAL_FALLBACK
};

// The default lane remains strict. The explicit alias makes that contract
// visible to CTest/qualification callers, while correctness callers may opt
// into an owner fallback without changing the runtime policy.
inline bool ParseObjectComputationTestMode(int argc, char **argv,
	bool *localCapacity, ObjectComputationTestMode *mode)
{
	if (localCapacity == 0 || mode == 0 || argc < 1 || argc > 3 ||
		(argc > 1 && argv == 0))
		return false;

	bool parsedLocalCapacity = false;
	bool parsedStrictScaling = false;
	bool parsedSerialFallback = false;
	for (int index = 1; index < argc; ++index)
	{
		if (argv[index] == 0) return false;
		if (strcmp(argv[index], "--local-capacity") == 0)
		{
			if (parsedLocalCapacity) return false;
			parsedLocalCapacity = true;
		}
		else if (strcmp(argv[index], "--strict-scaling") == 0)
		{
			if (parsedStrictScaling || parsedSerialFallback) return false;
			parsedStrictScaling = true;
		}
		else if (strcmp(argv[index], "--allow-serial-fallback") == 0)
		{
			if (parsedSerialFallback || parsedStrictScaling) return false;
			parsedSerialFallback = true;
		}
		else
			return false;
	}

	*localCapacity = parsedLocalCapacity;
	*mode = parsedSerialFallback ?
		OBJECT_COMPUTATION_TEST_ALLOW_SERIAL_FALLBACK :
		OBJECT_COMPUTATION_TEST_STRICT_SCALING;
	return true;
}

inline bool ObjectComputationAllowsSerialFallback(
	ObjectComputationTestMode mode)
{
	return mode == OBJECT_COMPUTATION_TEST_ALLOW_SERIAL_FALLBACK;
}

inline const char *ObjectComputationTestModeName(
	ObjectComputationTestMode mode)
{
	return ObjectComputationAllowsSerialFallback(mode) ?
		"allow-serial-fallback" : "strict-scaling";
}

// A serial fallback may expose the attempted wave through diagnostics, but it
// must never publish a parallel command. Every admitted job must have reached
// a terminal state and been released before the owner consumes the serial
// result; failed/cancelled jobs are intentionally not counted as completed or
// assigned a worker identity, so those diagnostic counters are upper bounds.
inline bool ObjectComputationSerialFallbackIsWellAccounted(
	const rts::ObjectComputationIsland &island,
	const rts::ObjectComputationMetrics &metrics)
{
	return island.commandCount() == 0 &&
		metrics.serialFallbacks == 1 &&
		metrics.emittedCommands == 0 &&
		metrics.emittedCandidates == 0 &&
		metrics.completedJobs <= metrics.submittedJobs &&
		metrics.schedulerReleasedJobs == metrics.submittedJobs &&
		metrics.physicalWorkerJobs <= metrics.submittedJobs &&
		metrics.ownerHelpedJobs <= metrics.submittedJobs &&
		metrics.physicalWorkerJobs <=
			metrics.submittedJobs - metrics.ownerHelpedJobs;
}

inline bool ObjectComputationParallelIsWellAccounted(
	rts::ObjectComputationResult result,
	const rts::ObjectComputationMetrics &metrics,
	unsigned expectedCommandCount)
{
	return result == rts::OBJECT_COMPUTATION_PARALLEL &&
		metrics.serialFallbacks == 0 && metrics.rangeCount > 1 &&
		metrics.submittedJobs != 0 &&
		metrics.physicalWorkerJobs != 0 &&
		metrics.distinctPhysicalWorkers > 1 &&
		metrics.ownerHelpedJobs < metrics.submittedJobs &&
		metrics.completedJobs == metrics.submittedJobs &&
		metrics.schedulerReleasedJobs == metrics.submittedJobs &&
		metrics.physicalWorkerJobs <= metrics.submittedJobs &&
		metrics.ownerHelpedJobs <= metrics.submittedJobs &&
		metrics.physicalWorkerJobs ==
			metrics.submittedJobs - metrics.ownerHelpedJobs &&
		metrics.emittedCommands == expectedCommandCount;
}

inline bool ObjectComputationResultIsAccepted(
	ObjectComputationTestMode mode, rts::ObjectComputationResult result,
	const rts::ObjectComputationIsland &island,
	const rts::ObjectComputationMetrics &metrics,
	unsigned expectedCommandCount)
{
	if (ObjectComputationParallelIsWellAccounted(result, metrics,
		expectedCommandCount))
		return true;
	return ObjectComputationAllowsSerialFallback(mode) &&
		result == rts::OBJECT_COMPUTATION_SERIAL_FALLBACK &&
		ObjectComputationSerialFallbackIsWellAccounted(island, metrics);
}

} // namespace rts_test
