#pragma once

#include <stdio.h>
#include <string.h>

namespace rts_test
{

const unsigned kLocalCapacityWorkerLimit = 12;

inline bool ParseTestCapacityLane(int argc, char **argv, bool *localCapacity,
	bool *serialPipelines = 0,
	const char *serialSelector = "--serial-pipelines")
{
	if (localCapacity == 0 || argc < 1 || argc > 3 ||
		(argc > 1 && argv == 0) ||
		(serialPipelines != 0 && serialSelector == 0))
		return false;

	bool parsedLocalCapacity = false;
	bool parsedSerialPipelines = false;
	for (int index = 1; index < argc; ++index)
	{
		if (argv[index] == 0)
			return false;
		if (strcmp(argv[index], "--local-capacity") == 0)
		{
			if (parsedLocalCapacity)
				return false;
			parsedLocalCapacity = true;
		}
		else if (serialPipelines != 0 &&
			serialSelector != 0 &&
			strcmp(argv[index], serialSelector) == 0)
		{
			if (parsedSerialPipelines)
				return false;
			parsedSerialPipelines = true;
		}
		else
			return false;
	}

	*localCapacity = parsedLocalCapacity;
	if (serialPipelines != 0)
		*serialPipelines = parsedSerialPipelines;
	return true;
}

inline unsigned ResolveActualWorkerCount(unsigned canonicalWorkerCount,
	bool localCapacity)
{
	if (!localCapacity ||
		(canonicalWorkerCount != 0 &&
			canonicalWorkerCount <= kLocalCapacityWorkerLimit))
		return canonicalWorkerCount;
	return kLocalCapacityWorkerLimit;
}

inline void PrintWorkerCountSubstitution(const char *label,
	unsigned requestedWorkerCount, unsigned effectiveWorkerCount,
	bool localCapacity)
{
	if (!localCapacity || requestedWorkerCount == effectiveWorkerCount)
		return;
	fprintf(stdout,
		"%s: local-capacity substituted worker count %u -> %u; "
		"external qualification excluded.\n",
		label != 0 ? label : "test",
		requestedWorkerCount, effectiveWorkerCount);
}

inline void PrintTestCapacityLane(bool localCapacity)
{
	if (localCapacity)
		printf("Test lane: local-capacity diagnostic (maximum workers=%u; "
			"external qualification excluded).\n",
			kLocalCapacityWorkerLimit);
}

} // namespace rts_test
