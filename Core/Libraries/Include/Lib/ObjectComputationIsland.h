/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2026 TheSuperHackers
** SPDX-License-Identifier: GPL-3.0-or-later
*/
#pragma once

#include "Lib/JobSystem.h"
#include "Lib/SimulationCommandBuffer.h"
#include "Lib/SimulationReadView.h"

namespace rts
{
enum ObjectComputationStableModuleType
{
	OBJECT_COMPUTATION_MODULE_HORDE = 1
};

enum ObjectComputationCommandType
{
	OBJECT_COMPUTATION_COMMAND_CANDIDATE_SET = 1
};

enum ObjectComputationResult
{
	OBJECT_COMPUTATION_SERIAL_REFERENCE = 0,
	OBJECT_COMPUTATION_PARALLEL,
	OBJECT_COMPUTATION_POLICY_INELIGIBLE,
	OBJECT_COMPUTATION_SERIAL_FALLBACK,
	OBJECT_COMPUTATION_INVALID_INPUT
};

enum ObjectComputationTestFault
{
	OBJECT_COMPUTATION_TEST_NONE = 0,
	OBJECT_COMPUTATION_TEST_GROUP_FAILURE,
	OBJECT_COMPUTATION_TEST_ADMISSION_FAILURE,
	OBJECT_COMPUTATION_TEST_CANCEL_AFTER_ADMISSION,
	OBJECT_COMPUTATION_TEST_PRODUCER_FAILURE,
	OBJECT_COMPUTATION_TEST_OWNER_HELP_ONLY,
	OBJECT_COMPUTATION_TEST_PHYSICAL_WAIT_TIMEOUT,
	OBJECT_COMPUTATION_TEST_SHADOW_MISMATCH,
	OBJECT_COMPUTATION_TEST_PAYLOAD_BUDGET_FAILURE
};

struct ObjectComputationOptions
{
	ObjectComputationOptions();

	bool parallel;
	UnsignedInt minimumModulesPerRange;
	UnsignedInt minimumPairCount;
	ObjectComputationTestFault testFault;
	UnsignedInt testOrdinal;
};

struct ObjectComputationMetrics
{
	ObjectComputationMetrics();

	UnsignedInt objectCount;
	UnsignedInt moduleCount;
	UnsignedInt pairCount;
	UnsignedInt rangeCount;
	UnsignedInt submittedJobs;
	UnsignedInt completedJobs;
	UnsignedInt schedulerReleasedJobs;
	UnsignedInt physicalWorkerJobs;
	UnsignedInt distinctPhysicalWorkers;
	UnsignedInt ownerHelpedJobs;
	UnsignedInt physicalWaitTimeouts;
	UnsignedInt emittedCommands;
	UnsignedInt emittedCandidates;
	UnsignedInt visitedSpatialMembers;
	UnsignedInt candidatePayloadBytes;
	UnsignedInt spatialCellSpans;
	UnsignedInt spatialMemberships;
	UnsignedInt allocatedBytes;
	UnsignedInt arenaBudgetBytes;
	UnsignedInt arenaAllocations;
	UnsignedInt serialFallbacks;
};

// Allocation-free owner/scheduler admission used before a title scans live
// modules or objects. Serial policy and one-worker policy rejection perform no
// island capture; scheduler safety rejection records one serial fallback.
ObjectComputationResult PreflightObjectComputationIsland(
	const ObjectComputationOptions &options,
	ObjectComputationMetrics *metrics = 0);

struct ObjectComputationCandidateSetHeader
{
	UnsignedInt frame;
	UnsignedInt viewGeneration;
	UnsignedInt moduleIndex;
	UnsignedInt objectCount;
	UnsignedInt candidateCount;
	UnsignedInt candidateByteCount;
};

// Owns one bounded preparation wave. All worker-visible state is reachable
// only through the pointer-free immutable SimulationReadView. Merged commands
// remain valid until reset(), another prepare(), or destruction.
class ObjectComputationIsland
{
public:
	ObjectComputationIsland();
	~ObjectComputationIsland();

	ObjectComputationResult prepare(const SimulationReadView &view,
		const ObjectComputationOptions &options,
		ObjectComputationMetrics *metrics = 0);
	void reset();
	UnsignedInt commandCount() const;
	const SimulationMergedCommand *commandAt(UnsignedInt index) const;

private:
	ObjectComputationIsland(const ObjectComputationIsland &);
	ObjectComputationIsland &operator=(const ObjectComputationIsland &);
	struct State;
	State *m_state;
};

bool DecodeObjectComputationCandidateSet(const SimulationReadView &view,
	const SimulationMergedCommand &merged,
	ObjectComputationCandidateSetHeader *header);
bool ObjectComputationCandidateAt(const SimulationReadView &view,
	const SimulationMergedCommand &merged, UnsignedInt objectIndex);
bool ObjectComputationCandidateIndexAt(const SimulationReadView &view,
	const SimulationMergedCommand &merged, UnsignedInt candidateOrdinal,
	UnsignedInt *objectIndex);
bool ObjectComputationCommandsEqual(const SimulationReadView &view,
	const ObjectComputationIsland &left,
	const ObjectComputationIsland &right, UnsignedInt *firstDifference = 0);
}
