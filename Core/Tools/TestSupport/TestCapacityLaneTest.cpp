/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2026 TheSuperHackers
** SPDX-License-Identifier: GPL-3.0-or-later
*/

#include "LocalCapacityTestLane.h"

#include <stdio.h>

namespace
{

int Check(bool condition, const char *message)
{
	if (condition)
		return 0;
	fprintf(stderr, "FAIL: %s\n", message);
	return 1;
}

int TestFailSafeDefaultResolution()
{
	char program[] = "capacity-lane-test";
	char *arguments[] = { program };
	bool localCapacity = false;
	bool serialPipelines = true;
	int result = 0;

	result |= Check(rts_test::ParseTestCapacityLane(1, arguments,
		&localCapacity, &serialPipelines) && localCapacity &&
		!serialPipelines, "no selector chooses the fail-safe local lane");
	result |= Check(rts_test::ResolveActualWorkerCount(0, localCapacity) ==
			rts_test::kLocalCapacityWorkerLimit &&
		rts_test::ResolveActualWorkerCount(16, localCapacity) ==
			rts_test::kLocalCapacityWorkerLimit &&
		rts_test::ResolveActualWorkerCount(96, localCapacity) ==
			rts_test::kLocalCapacityWorkerLimit,
		"default resolution bounds automatic and high-core requests");
	return result;
}

int TestExternalQualificationResolution()
{
	char program[] = "capacity-lane-test";
	char externalSelector[] = "--external-qualification";
	char *arguments[] = { program, externalSelector };
	bool localCapacity = true;
	bool serialPipelines = true;
	int result = 0;

	result |= Check(rts_test::ParseTestCapacityLane(2, arguments,
		&localCapacity, &serialPipelines) && !localCapacity &&
		!serialPipelines,
		"external qualification requires its explicit selector");
	result |= Check(rts_test::ResolveActualWorkerCount(0, localCapacity) == 0 &&
		rts_test::ResolveActualWorkerCount(16, localCapacity) == 16 &&
		rts_test::ResolveActualWorkerCount(96, localCapacity) == 96,
		"external qualification preserves automatic and high-core requests");
	return result;
}

int TestLocalResolution()
{
	char program[] = "capacity-lane-test";
	char localSelector[] = "--local-capacity";
	char *arguments[] = { program, localSelector };
	bool localCapacity = false;
	bool serialPipelines = true;
	int result = 0;

	result |= Check(rts_test::ParseTestCapacityLane(2, arguments,
		&localCapacity, &serialPipelines) && localCapacity &&
		!serialPipelines, "local selector chooses the diagnostic lane");
	result |= Check(rts_test::ResolveActualWorkerCount(1, true) == 1 &&
		rts_test::ResolveActualWorkerCount(12, true) == 12 &&
		rts_test::ResolveActualWorkerCount(0, true) ==
			rts_test::kLocalCapacityWorkerLimit &&
		rts_test::ResolveActualWorkerCount(16, true) ==
			rts_test::kLocalCapacityWorkerLimit &&
		rts_test::ResolveActualWorkerCount(96, true) ==
			rts_test::kLocalCapacityWorkerLimit,
		"local resolution bounds only automatic and oversized starts");
	return result;
}

int TestSerialSelectorAndRejection()
{
	char program[] = "capacity-lane-test";
	char localSelector[] = "--local-capacity";
	char externalSelector[] = "--external-qualification";
	char serialSelector[] = "--serial-pipelines";
	char customSerialSelector[] = "--serial-policy";
	char unknownSelector[] = "--unknown";
	char *serialArguments[] = { program, serialSelector };
	char *customSerialArguments[] = { program, customSerialSelector };
	char *localSerialArguments[] = { program, localSelector, serialSelector };
	char *serialLocalArguments[] = { program, serialSelector, localSelector };
	char *externalSerialArguments[] = { program, externalSelector,
		serialSelector };
	char *unknownArguments[] = { program, unknownSelector };
	char *duplicateLocalArguments[] = { program, localSelector,
		localSelector };
	char *conflictingCapacityArguments[] = { program, localSelector,
		externalSelector };
	char *duplicateExternalArguments[] = { program, externalSelector,
		externalSelector };
	char *duplicateSerialArguments[] = { program, serialSelector,
		serialSelector };
	bool localCapacity = false;
	bool serialPipelines = false;
	int result = 0;

	result |= Check(!rts_test::ParseTestCapacityLane(2, serialArguments,
		&localCapacity), "serial selector requires caller opt-in");
	result |= Check(rts_test::ParseTestCapacityLane(2, serialArguments,
		&localCapacity, &serialPipelines) && localCapacity &&
		serialPipelines, "caller opt-in accepts serial selector");
	result |= Check(rts_test::ParseTestCapacityLane(2, customSerialArguments,
		&localCapacity, &serialPipelines, customSerialSelector) &&
		localCapacity && serialPipelines &&
		!rts_test::ParseTestCapacityLane(2, serialArguments, &localCapacity,
			&serialPipelines, customSerialSelector),
		"caller opt-in accepts only its exact custom selector");
	result |= Check(rts_test::ParseTestCapacityLane(3,
		localSerialArguments, &localCapacity, &serialPipelines) &&
		localCapacity && serialPipelines &&
		rts_test::ParseTestCapacityLane(3, serialLocalArguments,
		&localCapacity, &serialPipelines) && localCapacity && serialPipelines,
		"local and caller selectors are order-independent");
	result |= Check(rts_test::ParseTestCapacityLane(3,
		externalSerialArguments, &localCapacity, &serialPipelines) &&
		!localCapacity && serialPipelines,
		"external qualification may combine with the caller selector");
	result |= Check(!rts_test::ParseTestCapacityLane(2, unknownArguments,
		&localCapacity, &serialPipelines),
		"unknown selector is rejected");
	result |= Check(!rts_test::ParseTestCapacityLane(3,
		duplicateLocalArguments, &localCapacity, &serialPipelines) &&
		!rts_test::ParseTestCapacityLane(3, conflictingCapacityArguments,
			&localCapacity, &serialPipelines) &&
		!rts_test::ParseTestCapacityLane(3, duplicateExternalArguments,
			&localCapacity, &serialPipelines) &&
		!rts_test::ParseTestCapacityLane(3, duplicateSerialArguments,
		&localCapacity, &serialPipelines),
		"duplicate selectors are rejected");
	result |= Check(!rts_test::ParseTestCapacityLane(1, serialArguments,
		0, &serialPipelines), "null lane output is rejected");
	return result;
}

} // namespace

int main()
{
	int result = 0;
	result |= TestFailSafeDefaultResolution();
	result |= TestExternalQualificationResolution();
	result |= TestLocalResolution();
	result |= TestSerialSelectorAndRejection();
	if (result == 0)
		printf("Test capacity lane helper tests passed.\n");
	return result;
}
