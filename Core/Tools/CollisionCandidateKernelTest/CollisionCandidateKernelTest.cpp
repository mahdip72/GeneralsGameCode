/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2025 Electronic Arts Inc.
** SPDX-License-Identifier: GPL-3.0-or-later
*/
#include "Lib/CollisionCandidateKernel.h"

#include <limits.h>
#include <stdio.h>

#if !defined(_MSC_VER) || _MSC_VER >= 1300
#include <atomic>
#include <chrono>
#include <thread>
#endif

#if defined(RTS_BUILD_CORE_EXTRAS) && \
	(!defined(_MSC_VER) || _MSC_VER >= 1300)
extern "C" void rts_job_system_set_test_fault(unsigned fault,
	unsigned occurrence);
extern "C" void rts_collision_candidate_set_test_allocation_failure(
	unsigned occurrence);
#endif

namespace
{
int failures = 0;

enum
{
	PARITY_INPUT_COUNT = 512,
	PARITY_UNIQUE_COUNT = 300
};

void expect(bool condition, const char *message)
{
	if (condition)
		return;
	printf("FAIL: %s\n", message);
	++failures;
}

void setInput(rts::CollisionCandidateInput &input, unsigned firstID,
	unsigned secondID, unsigned discoveryOrder)
{
	input.firstID = firstID;
	input.secondID = secondID;
	input.firstGeneration = firstID == 0 ? 0 : 1;
	input.secondGeneration = secondID == 0 ? 0 : 2;
	input.discoveryOrder = discoveryOrder;
}

void setSentinel(rts::CollisionCandidate *output, unsigned count)
{
	for (unsigned index = 0; index != count; ++index)
	{
		output[index].key.lowID = 0x12345678u;
		output[index].key.highID = 0x9abcdef0u;
		output[index].firstID = 0x13579bdfu;
		output[index].secondID = 0x2468ace0u;
		output[index].firstGeneration = 0x10203040u;
		output[index].secondGeneration = 0x50607080u;
		output[index].discoveryOrder = UINT_MAX;
	}
}

bool isSentinel(const rts::CollisionCandidate &candidate)
{
	return candidate.key.lowID == 0x12345678u &&
		candidate.key.highID == 0x9abcdef0u &&
		candidate.firstID == 0x13579bdfu &&
		candidate.secondID == 0x2468ace0u &&
		candidate.firstGeneration == 0x10203040u &&
		candidate.secondGeneration == 0x50607080u &&
		candidate.discoveryOrder == UINT_MAX;
}

bool sameCandidates(const rts::CollisionCandidate *left,
	const rts::CollisionCandidate *right, unsigned count)
{
	for (unsigned index = 0; index != count; ++index)
	{
		if (left[index].key.lowID != right[index].key.lowID ||
			left[index].key.highID != right[index].key.highID ||
			left[index].firstID != right[index].firstID ||
			left[index].secondID != right[index].secondID ||
			left[index].firstGeneration != right[index].firstGeneration ||
			left[index].secondGeneration != right[index].secondGeneration ||
			left[index].discoveryOrder != right[index].discoveryOrder)
			return false;
	}
	return true;
}

void makeParityInputs(rts::CollisionCandidateInput *inputs)
{
	for (unsigned index = 0; index != PARITY_INPUT_COUNT; ++index)
	{
		const unsigned pairIndex = index % PARITY_UNIQUE_COUNT;
		const unsigned firstID = pairIndex + 1;
		const unsigned secondID = pairIndex + 1001;
		if (index < PARITY_UNIQUE_COUNT)
			setInput(inputs[index], firstID, secondID, index);
		else
			setInput(inputs[index], secondID, firstID, index);
	}
}

bool startJobSystem(unsigned workerCount)
{
	rts::JobSystem &jobs = rts::JobSystem::instance();
	jobs.shutdown();
	rts::JobSystemConfig config;
	config.workerCount = workerCount;
	config.queueCapacity = 128;
	config.scratchBytesPerWorker = 4096;
	config.pinWorkers = false;
	if (!jobs.start(config))
	{
		expect(false, "job system starts for collision candidate test");
		return false;
	}
	if (!jobs.registerCurrentThread(rts::JOB_OWNER_GAME))
	{
		expect(false, "collision candidate test registers the game owner");
		jobs.shutdown();
		return false;
	}
	return true;
}

void stopJobSystem()
{
	rts::JobSystem &jobs = rts::JobSystem::instance();
	jobs.shutdown();
	expect(jobs.unregisterCurrentThread(rts::JOB_OWNER_GAME),
		"collision candidate test unregisters the game owner");
}

void testUnsignedKey()
{
	rts::CollisionCandidateKey forward;
	rts::CollisionCandidateKey reverse;
	expect(rts::MakeCollisionCandidateKey(UINT_MAX, 40000u, forward),
		"large unsigned IDs normalize");
	expect(rts::MakeCollisionCandidateKey(40000u, UINT_MAX, reverse),
		"reversed large unsigned IDs normalize");
	expect(forward.lowID == 40000u && forward.highID == UINT_MAX,
		"large key is ordered without a signed comparison");
	expect(forward.lowID == reverse.lowID &&
		forward.highID == reverse.highID,
		"pair orientation does not change the normalized key");
	expect(rts::HashCollisionCandidateKey(forward) ==
		rts::HashCollisionCandidateKey(reverse),
		"normalized pair orientations hash identically");
	expect(!rts::MakeCollisionCandidateKey(0, 4, forward),
		"invalid zero ID is rejected");
	expect(!rts::MakeCollisionCandidateKey(4, 4, forward),
		"self pair is rejected");
}

void testAdmissionSamplesWholeEncounterSpan()
{
	rts::CollisionAdmissionSampler useful;
	for (unsigned usefulIndex = 1; usefulIndex <= 512; ++usefulIndex)
		useful.observe(usefulIndex);
	expect(useful.encounterCount() == 512 &&
		useful.sampleCount() == rts::COLLISION_ADMISSION_SAMPLE_CAPACITY &&
		useful.hasUsefulSpread(),
		"distributed admission sample accepts unique work across the span");

	rts::CollisionAdmissionSampler duplicateTail;
	for (unsigned prefixIndex = 1;
		prefixIndex <= rts::COLLISION_ADMISSION_SAMPLE_CAPACITY; ++prefixIndex)
		duplicateTail.observe(prefixIndex);
	for (unsigned tailIndex = 0; tailIndex != 4096; ++tailIndex)
		duplicateTail.observe(7);
	expect(duplicateTail.encounterCount() == 4160 &&
		!duplicateTail.hasUsefulSpread(),
		"duplicate-heavy later cells cannot hide behind a unique 64-item prefix");
}

void testOrderAndDedup()
{
	rts::CollisionCandidateInput inputs[6];
	setInput(inputs[0], 9, 2, 0);
	setInput(inputs[1], 2, 9, 1);
	setInput(inputs[2], UINT_MAX, 40000u, 2);
	setInput(inputs[3], 7, 4, 3);
	setInput(inputs[4], 5, 5, 4);
	setInput(inputs[5], 0, 8, 5);
	rts::CollisionCandidate output[6];
	rts::CollisionCandidate scratch[6];
	rts::CollisionCandidateOptions options;
	unsigned outputCount = 99;
	rts::CollisionCandidateResult result = rts::PrepareCollisionCandidates(
		inputs, 6, output, 6, scratch, 6, options, &outputCount);
	expect(result == rts::COLLISION_CANDIDATE_SERIAL,
		"small preparation uses the serial pure kernel");
	expect(outputCount == 3, "invalid and duplicate pairs are removed");
	expect(output[0].firstID == 7 && output[0].secondID == 4 &&
		output[0].discoveryOrder == 3,
		"legacy-compatible output begins with latest unique discovery");
	expect(output[1].key.lowID == 40000u &&
		output[1].key.highID == UINT_MAX,
		"large unsigned pair survives sort and dedup");
	expect(output[2].firstID == 9 && output[2].secondID == 2 &&
		output[2].discoveryOrder == 0,
		"dedup keeps first-discovery callback orientation");

	options.order = rts::COLLISION_CANDIDATE_CANONICAL_KEY;
	outputCount = 99;
	result = rts::PrepareCollisionCandidates(inputs, 6, output, 6, scratch,
		6, options, &outputCount);
	expect(result == rts::COLLISION_CANDIDATE_SERIAL && outputCount == 3,
		"canonical preparation succeeds");
	expect(output[0].key.lowID == 2 && output[0].key.highID == 9 &&
		output[1].key.lowID == 4 && output[1].key.highID == 7 &&
		output[2].key.lowID == 40000u &&
		output[2].key.highID == UINT_MAX,
		"canonical output is lexicographically ordered");
}

void testTransactionalFailure()
{
	rts::CollisionCandidateInput input;
	setInput(input, 1, 2, 0);
	rts::CollisionCandidate output;
	output.key.lowID = 123;
	output.key.highID = 456;
	unsigned outputCount = 77;
	rts::CollisionCandidateOptions options;
	const rts::CollisionCandidateResult result =
		rts::PrepareCollisionCandidates(&input, 1, &output, 1, &output, 1,
			options, &outputCount);
	expect(result == rts::COLLISION_CANDIDATE_INVALID_INPUT,
		"overlapping output and scratch are rejected");
	expect(output.key.lowID == 123 && output.key.highID == 456 &&
		outputCount == 77,
		"failed preparation leaves publication untouched");
}

void makePartitionOwner(rts::PartitionCollisionObjectSnapshot &owner);

void expectGenericOverlapRejected(
	const rts::CollisionCandidateInput *inputs,
	rts::CollisionCandidate *output, rts::CollisionCandidate *scratch,
	bool parallel, const char *message)
{
	rts::CollisionCandidateOptions options;
	options.parallel = parallel;
	options.minimumGrain = 32;
	output[0].key.lowID = 0x12345678u;
	output[0].key.highID = 0x9abcdef0u;
	unsigned outputCount = 73;
	const rts::CollisionCandidateResult result =
		rts::PrepareCollisionCandidates(inputs, PARITY_INPUT_COUNT, output,
			PARITY_INPUT_COUNT, scratch, PARITY_INPUT_COUNT, options,
			&outputCount);
	expect(result == rts::COLLISION_CANDIDATE_INVALID_INPUT &&
		outputCount == 73 && output[0].key.lowID == 0x12345678u &&
		output[0].key.highID == 0x9abcdef0u, message);
}

void expectPartitionOverlapRejected(
	const rts::PartitionCollisionObjectSnapshot &owner,
	const rts::PartitionCollisionCellSnapshot *cells,
	const rts::PartitionCollisionOccupantSnapshot *occupants,
	rts::CollisionCandidate *output, rts::CollisionCandidate *scratch,
	bool parallel, const char *message)
{
	rts::CollisionCandidateOptions options;
	options.parallel = parallel;
	options.minimumGrain = 32;
	output[0].key.lowID = 0x23456789u;
	output[0].key.highID = 0xabcdef01u;
	unsigned outputCount = 74;
	const rts::CollisionCandidateResult result =
		rts::PreparePartitionCollisionCandidates(owner, cells, 1, occupants,
			PARITY_INPUT_COUNT, output, PARITY_INPUT_COUNT, scratch,
			PARITY_INPUT_COUNT, options, &outputCount);
	expect(result == rts::COLLISION_CANDIDATE_INVALID_INPUT &&
		outputCount == 74 && output[0].key.lowID == 0x23456789u &&
		output[0].key.highID == 0xabcdef01u, message);
}

void testPartialOverlapRejectedInSerialAndParallelModes()
{
	union GenericInputCandidateStorage
	{
		rts::CollisionCandidateInput inputs[PARITY_INPUT_COUNT];
		rts::CollisionCandidate candidates[PARITY_INPUT_COUNT + 1];
	};
	union PartitionInputCandidateStorage
	{
		rts::PartitionCollisionOccupantSnapshot occupants[PARITY_INPUT_COUNT];
		rts::CollisionCandidate candidates[PARITY_INPUT_COUNT + 1];
	};
	union PartitionCellCandidateStorage
	{
		rts::PartitionCollisionCellSnapshot cells[2];
		rts::CollisionCandidate candidates[PARITY_INPUT_COUNT];
	};
	union PartitionCellOccupantStorage
	{
		rts::PartitionCollisionCellSnapshot cells[2];
		rts::PartitionCollisionOccupantSnapshot occupants[PARITY_INPUT_COUNT];
	};
	static GenericInputCandidateStorage genericInputStorage;
	static PartitionInputCandidateStorage partitionInputStorage;
	static PartitionCellCandidateStorage partitionCellStorage;
	static PartitionCellOccupantStorage partitionInputsStorage;
	static rts::CollisionCandidateInput genericInputs[PARITY_INPUT_COUNT];
	static rts::PartitionCollisionOccupantSnapshot
		partitionOccupants[PARITY_INPUT_COUNT];
	static rts::CollisionCandidate output[PARITY_INPUT_COUNT];
	static rts::CollisionCandidate scratch[PARITY_INPUT_COUNT];
	static rts::CollisionCandidate shared[PARITY_INPUT_COUNT + 1];

	makeParityInputs(genericInputs);
	makeParityInputs(genericInputStorage.inputs);
	for (unsigned index = 0; index != PARITY_INPUT_COUNT; ++index)
	{
		partitionOccupants[index].objectID = index + 20;
		partitionOccupants[index].generation = index + 2;
		partitionInputStorage.occupants[index] = partitionOccupants[index];
	}
	rts::PartitionCollisionCellSnapshot cell;
	cell.occupantBegin = 0;
	cell.occupantCount = PARITY_INPUT_COUNT;
	cell.discoveryBase = 0;
	rts::PartitionCollisionObjectSnapshot owner;
	makePartitionOwner(owner);

	for (unsigned mode = 0; mode != 2; ++mode)
	{
		const bool parallel = mode != 0;
		expectGenericOverlapRejected(genericInputStorage.inputs,
			genericInputStorage.candidates + 1, scratch, parallel,
			"generic input/output partial overlap is rejected transactionally");
		expectGenericOverlapRejected(genericInputStorage.inputs, output,
			genericInputStorage.candidates + 1, parallel,
			"generic input/scratch partial overlap is rejected transactionally");
		expectGenericOverlapRejected(genericInputs, shared, shared + 1,
			parallel,
			"generic output/scratch partial overlap is rejected transactionally");

		partitionInputsStorage.cells[1] = cell;
		expectPartitionOverlapRejected(owner, partitionInputsStorage.cells + 1,
			partitionInputsStorage.occupants, output, scratch, parallel,
			"partition cell/occupant partial overlap is rejected transactionally");
		partitionCellStorage.cells[1] = cell;
		expectPartitionOverlapRejected(owner, partitionCellStorage.cells + 1,
			partitionOccupants, partitionCellStorage.candidates, scratch,
			parallel,
			"partition cell/output partial overlap is rejected transactionally");
		partitionCellStorage.cells[1] = cell;
		expectPartitionOverlapRejected(owner, partitionCellStorage.cells + 1,
			partitionOccupants, output, partitionCellStorage.candidates,
			parallel,
			"partition cell/scratch partial overlap is rejected transactionally");
		expectPartitionOverlapRejected(owner, &cell,
			partitionInputStorage.occupants,
			partitionInputStorage.candidates + 1, scratch, parallel,
			"partition occupant/output partial overlap is rejected transactionally");
		expectPartitionOverlapRejected(owner, &cell,
			partitionInputStorage.occupants, output,
			partitionInputStorage.candidates + 1, parallel,
			"partition occupant/scratch partial overlap is rejected transactionally");
		expectPartitionOverlapRejected(owner, &cell, partitionOccupants,
			shared, shared + 1, parallel,
			"partition output/scratch partial overlap is rejected transactionally");
	}
}

void testEmptyInputAcceptsNullAndAliasedBuffers()
{
	rts::CollisionCandidateOptions options;
	unsigned outputCount = 17;
	rts::CollisionCandidateResult result = rts::PrepareCollisionCandidates(
		0, 0, 0, 0, 0, 0, options, &outputCount);
	expect(result == rts::COLLISION_CANDIDATE_SERIAL && outputCount == 0,
		"empty generic input accepts null aliased buffers transactionally");

	rts::CollisionCandidate shared;
	outputCount = 18;
	result = rts::PrepareCollisionCandidates(0, 0, &shared, 0, &shared, 0,
		options, &outputCount);
	expect(result == rts::COLLISION_CANDIDATE_SERIAL && outputCount == 0,
		"empty generic input permits an unused non-null output/scratch alias");

	rts::PartitionCollisionObjectSnapshot owner;
	makePartitionOwner(owner);
	outputCount = 19;
	result = rts::PreparePartitionCollisionCandidates(owner, 0, 0, 0, 0,
		0, 0, 0, 0, options, &outputCount);
	expect(result == rts::COLLISION_CANDIDATE_SERIAL && outputCount == 0,
		"empty partition input accepts null aliased buffers transactionally");

	outputCount = 20;
	result = rts::PreparePartitionCollisionCandidates(owner, 0, 0, 0, 0,
		&shared, 0, &shared, 0, options, &outputCount);
	expect(result == rts::COLLISION_CANDIDATE_SERIAL && outputCount == 0,
		"empty partition input permits an unused non-null output/scratch alias");
}

void testColdParallelRequestDoesNotStartWorkers()
{
	rts::JobSystem &jobs = rts::JobSystem::instance();
	jobs.shutdown();
	rts::CollisionCandidateInput inputs[PARITY_INPUT_COUNT];
	rts::CollisionCandidate output[PARITY_INPUT_COUNT];
	rts::CollisionCandidate scratch[PARITY_INPUT_COUNT];
	makeParityInputs(inputs);
	setSentinel(output, PARITY_INPUT_COUNT);
	rts::CollisionCandidateOptions options;
	options.parallel = true;
	options.minimumGrain = 64;
	unsigned outputCount = 77;
	const rts::CollisionCandidateResult result =
		rts::PrepareCollisionCandidates(inputs, PARITY_INPUT_COUNT, output,
			PARITY_INPUT_COUNT, scratch, PARITY_INPUT_COUNT, options,
			&outputCount);
	expect(result == rts::COLLISION_CANDIDATE_SERIAL_FALLBACK,
		"cold parallel request returns an explicit serial fallback");
	expect(!jobs.isRunning(),
		"collision candidate kernel never starts engine workers lazily");
	expect(outputCount == 77 && isSentinel(output[0]),
		"cold fallback leaves publication untouched");
}

void testOneWorkerFallbackAndCancellation()
{
	if (!startJobSystem(1))
		return;
	rts::JobSystem &jobs = rts::JobSystem::instance();
	expect(jobs.workerCount() == 1,
		"one-worker collision fixture owns exactly one worker lane");
	rts::CollisionCandidateInput inputs[PARITY_INPUT_COUNT];
	rts::CollisionCandidate output[PARITY_INPUT_COUNT];
	rts::CollisionCandidate scratch[PARITY_INPUT_COUNT];
	makeParityInputs(inputs);
	setSentinel(output, PARITY_INPUT_COUNT);
	rts::CollisionCandidateOptions options;
	options.parallel = true;
	options.minimumGrain = 64;
	unsigned outputCount = 91;
	rts::CollisionCandidateMetrics metrics;
	rts::CollisionCandidateResult result = rts::PrepareCollisionCandidates(
		inputs, PARITY_INPUT_COUNT, output, PARITY_INPUT_COUNT, scratch,
		PARITY_INPUT_COUNT, options, &outputCount, &metrics);
	expect(result == rts::COLLISION_CANDIDATE_SERIAL_FALLBACK,
		"one-worker parallel request returns the expected serial fallback");
	expect(metrics.submittedJobs == 0 && metrics.completedJobs == 0 &&
		metrics.serialFallbacks == 1,
		"one-worker fallback records no false parallel work");
	expect(outputCount == 91 && isSentinel(output[0]),
		"one-worker fallback leaves publication untouched");

	rts::JobGroup cancellation = jobs.createGroup();
	expect(cancellation.isValid() && jobs.cancel(cancellation),
		"cancellation fixture creates a cancelled group");
	options.cancellationGroup = &cancellation;
	outputCount = 92;
	result = rts::PrepareCollisionCandidates(inputs, PARITY_INPUT_COUNT,
		output, PARITY_INPUT_COUNT, scratch, PARITY_INPUT_COUNT, options,
		&outputCount);
	expect(result == rts::COLLISION_CANDIDATE_CANCELLED,
		"cancelled collision candidate request is reported");
	expect(outputCount == 92 && isSentinel(output[0]),
		"cancellation leaves publication untouched");
	stopJobSystem();
}

struct GenerationState
{
	unsigned ids[8];
	unsigned generations[8];
	bool alive[8];
	unsigned count;
};

bool resolveGeneration(unsigned objectID, unsigned generation, void *context)
{
	GenerationState *state = static_cast<GenerationState *>(context);
	for (unsigned index = 0; index != state->count; ++index)
	{
		if (state->ids[index] == objectID)
			return state->alive[index] &&
				state->generations[index] == generation;
	}
	return false;
}

void makePartitionOwner(rts::PartitionCollisionObjectSnapshot &owner)
{
	owner.objectID = 10;
	owner.generation = 7;
	owner.dirtyOrder = 3;
	owner.positionX = 12.0f;
	owner.positionY = 15.0f;
	owner.positionZ = 2.0f;
	owner.orientation = 0.5f;
	owner.majorRadius = 8.0f;
	owner.minorRadius = 4.0f;
	owner.geometryType = 2;
	owner.smallGeometry = false;
}

void testPartitionOrderingGenerationAndCallbackDestruction()
{
	rts::PartitionCollisionObjectSnapshot owner;
	makePartitionOwner(owner);
	rts::PartitionCollisionCellSnapshot cells[2];
	cells[0].occupantBegin = 0;
	cells[0].occupantCount = 3;
	cells[0].discoveryBase = 0;
	cells[1].occupantBegin = 3;
	cells[1].occupantCount = 3;
	cells[1].discoveryBase = 3;
	rts::PartitionCollisionOccupantSnapshot occupants[6];
	const unsigned ids[6] = { 10, 20, 30, 20, 40, 30 };
	const unsigned generations[6] = { 7, 2, 3, 4, 5, 6 };
	for (unsigned index = 0; index != 6; ++index)
	{
		occupants[index].objectID = ids[index];
		occupants[index].generation = generations[index];
	}
	rts::CollisionCandidate output[6];
	rts::CollisionCandidate scratch[6];
	rts::CollisionCandidateOptions options;
	unsigned outputCount = 0;
	rts::CollisionCandidateMetrics metrics;
	const rts::CollisionCandidateResult result =
		rts::PreparePartitionCollisionCandidates(owner, cells, 2, occupants,
			6, output, 6, scratch, 6, options, &outputCount, &metrics);
	expect(result == rts::COLLISION_CANDIDATE_SERIAL && outputCount == 3,
		"partition snapshot removes self pairs and cross-cell duplicates");
	expect(output[0].secondID == 40 && output[0].discoveryOrder == 4 &&
		output[1].secondID == 30 && output[1].discoveryOrder == 2 &&
		output[2].secondID == 20 && output[2].discoveryOrder == 1,
		"partition reduction preserves legacy reverse discovery order");
	expect(output[1].secondGeneration == 3 &&
		output[2].secondGeneration == 2,
		"partition dedup retains the first discovered snapshot generation");
	expect(metrics.preparedPairs == 6 && metrics.uniqueCandidates == 3,
		"partition preparation reports candidate-volume metrics");

	// Independent contact-list model: pair 10/30 exists before this owner is
	// visited, and each successful legacy encounter prepends one new contact.
	rts::CollisionCandidate expectedInserted[6];
	unsigned expectedInsertedCount = 0;
	for (unsigned expectedIndex = 0; expectedIndex != outputCount;
		++expectedIndex)
	{
		if (output[expectedIndex].secondID != 30)
			expectedInserted[expectedInsertedCount++] = output[expectedIndex];
	}
	rts::CollisionCandidate legacyInserted[6];
	unsigned legacyInsertedCount = 0;
	bool seen20 = false;
	bool seen30 = true;
	bool seen40 = false;
	for (unsigned discovery = 0; discovery != 6; ++discovery)
	{
		const unsigned otherID = ids[discovery];
		bool *seen = otherID == 20 ? &seen20 :
			(otherID == 30 ? &seen30 : (otherID == 40 ? &seen40 : 0));
		if (otherID == owner.objectID || seen == 0 || *seen)
			continue;
		*seen = true;
		rts::CollisionCandidate &inserted =
			legacyInserted[legacyInsertedCount++];
		inserted.firstID = owner.objectID;
		inserted.secondID = otherID;
		inserted.firstGeneration = owner.generation;
		inserted.secondGeneration = generations[discovery];
		inserted.discoveryOrder = discovery;
		rts::MakeCollisionCandidateKey(inserted.firstID, inserted.secondID,
			inserted.key);
	}
	for (unsigned first = 0, last = legacyInsertedCount;
		first < last && first < --last; ++first)
	{
		rts::CollisionCandidate temporary = legacyInserted[first];
		legacyInserted[first] = legacyInserted[last];
		legacyInserted[last] = temporary;
	}
	unsigned legacyDifference = 99;
	expect(rts::CollisionCandidatesEqual(expectedInserted,
		expectedInsertedCount, legacyInserted, legacyInsertedCount,
		&legacyDifference),
		"independent legacy prepend oracle preserves exact orientation/order and existing-pair filtering");

	GenerationState state;
	state.count = 4;
	state.ids[0] = 10; state.generations[0] = 7; state.alive[0] = true;
	state.ids[1] = 20; state.generations[1] = 2; state.alive[1] = true;
	state.ids[2] = 30; state.generations[2] = 3; state.alive[2] = true;
	state.ids[3] = 40; state.generations[3] = 5; state.alive[3] = true;
	unsigned staleIndex = 99;
	expect(rts::ValidateCollisionCandidateGenerations(output, outputCount,
		resolveGeneration, &state, &staleIndex),
		"owner generation validation accepts an unchanged snapshot");
	state.generations[2] = 99;
	expect(!rts::ValidateCollisionCandidateGenerations(output, outputCount,
		resolveGeneration, &state, &staleIndex) && staleIndex == 1,
		"stale generation rejects the complete publication");
	state.generations[2] = 3;

	unsigned firstDifference = 99;
	expect(rts::CollisionCandidatesEqual(output, outputCount, output,
		outputCount, &firstDifference) && firstDifference == outputCount,
		"shadow comparison accepts every field of identical output");
	rts::CollisionCandidate changed[6];
	for (unsigned index = 0; index != outputCount; ++index)
		changed[index] = output[index];
	++changed[1].secondGeneration;
	expect(!rts::CollisionCandidatesEqual(output, outputCount, changed,
		outputCount, &firstDifference) && firstDifference == 1,
		"shadow comparison rejects a differing candidate field");
	expect(!rts::CollisionCandidatesEqual(output, outputCount, output,
		outputCount - 1, &firstDifference) &&
		firstDifference == outputCount - 1,
		"shadow comparison rejects a differing candidate count");

	unsigned callbacks = 0;
	for (unsigned index = 0; index != outputCount; ++index)
	{
		if (!resolveGeneration(output[index].firstID,
				output[index].firstGeneration, &state) ||
			!resolveGeneration(output[index].secondID,
				output[index].secondGeneration, &state))
			continue;
		++callbacks;
		// Mirrors the live contact-list contract: detaching during onCollide
		// invalidates every later contact containing the destroyed object.
		state.alive[0] = false;
	}
	expect(callbacks == 1,
		"callback destruction prevents later stale contact invocation");

	rts::ResetCollisionCandidateRuntimeMetrics();
	rts::RecordCollisionCandidateOwnerCommit(true, false, outputCount);
	rts::RecordCollisionCandidateOwnerCommit(false, true, 2);
	rts::RecordCollisionCandidateShadowMismatch();
	rts::CollisionCandidateMetrics parallelWork;
	parallelWork.preparedPairs = 6;
	parallelWork.uniqueCandidates = 3;
	parallelWork.submittedJobs = 4;
	parallelWork.completedJobs = 4;
	parallelWork.localSortRuns = 4;
	parallelWork.locallyUniqueCandidates = 5;
	parallelWork.ownerMergeComparisons = 12;
	parallelWork.maximumRangeInputs = 2;
	parallelWork.physicalWorkerJobs = 3;
	parallelWork.ownerHelpedJobs = 1;
	parallelWork.physicalWorkerMask = 5;
	parallelWork.distinctPhysicalWorkers = 2;
	rts::RecordCollisionCandidateParallelWork(parallelWork);
	rts::RecordCollisionCandidateIneligibleSlice();
	rts::RecordCollisionCandidateOwnerFallback(true, true);
	const rts::CollisionCandidateRuntimeMetrics runtime =
		rts::GetCollisionCandidateRuntimeMetrics();
	expect(runtime.resetEpoch != 0 && runtime.authoritativeCommits == 1 &&
		runtime.shadowExecutions == 1 && runtime.shadowMismatches == 1 &&
		runtime.ownerFallbacks == 1 && runtime.unexpectedFallbacks == 1 &&
		runtime.ineligibleSlices == 1 && runtime.staleRejections == 1 &&
		runtime.committedCandidates == 3 &&
		runtime.shadowComparedCandidates == 2 && runtime.preparedPairs == 6 &&
		runtime.uniqueCandidates == 3 && runtime.submittedJobs == 4 &&
		runtime.completedJobs == 4 && runtime.localSortRuns == 4 &&
		runtime.locallyUniqueCandidates == 5 &&
		runtime.ownerMergeComparisons == 12 &&
		runtime.maximumRangeInputs == 2 &&
		runtime.physicalWorkerJobs == 3 && runtime.ownerHelpedJobs == 1 &&
		runtime.physicalWorkerMask == 5,
		"runtime metrics separate commits, shadows, failures, useful work, and jobs");
	rts::ResetCollisionCandidateRuntimeMetrics();
	const rts::CollisionCandidateRuntimeMetrics resetRuntime =
		rts::GetCollisionCandidateRuntimeMetrics();
	expect(resetRuntime.resetEpoch == runtime.resetEpoch + 1 &&
		resetRuntime.authoritativeCommits == 0 &&
		resetRuntime.shadowExecutions == 0 &&
		resetRuntime.shadowMismatches == 0 &&
		resetRuntime.ownerFallbacks == 0 &&
		resetRuntime.unexpectedFallbacks == 0 &&
		resetRuntime.ineligibleSlices == 0 &&
		resetRuntime.staleRejections == 0 &&
		resetRuntime.committedCandidates == 0 &&
		resetRuntime.shadowComparedCandidates == 0 &&
		resetRuntime.preparedPairs == 0 &&
		resetRuntime.uniqueCandidates == 0 &&
		resetRuntime.submittedJobs == 0 &&
		resetRuntime.completedJobs == 0 && resetRuntime.localSortRuns == 0 &&
		resetRuntime.locallyUniqueCandidates == 0 &&
		resetRuntime.ownerMergeComparisons == 0 &&
		resetRuntime.maximumRangeInputs == 0 &&
		resetRuntime.physicalWorkerJobs == 0 &&
		resetRuntime.ownerHelpedJobs == 0 &&
		resetRuntime.physicalWorkerMask == 0,
		"runtime metric lifecycle reset advances its epoch and clears every collision counter");
}

#if !defined(_MSC_VER) || _MSC_VER >= 1300
void testActualWorkerMatrixParity()
{
	enum { OCCUPANT_COUNT = 65536, UNIQUE_COUNT = 24000, CELL_COUNT = 16 };
	static rts::PartitionCollisionOccupantSnapshot occupants[OCCUPANT_COUNT];
	static rts::CollisionCandidate serialOutput[OCCUPANT_COUNT];
	static rts::CollisionCandidate parallelOutput[OCCUPANT_COUNT];
	static rts::CollisionCandidate serialScratch[OCCUPANT_COUNT];
	static rts::CollisionCandidate parallelScratch[OCCUPANT_COUNT];
	rts::PartitionCollisionObjectSnapshot owner;
	makePartitionOwner(owner);
	rts::PartitionCollisionCellSnapshot cells[CELL_COUNT];
	for (unsigned cell = 0; cell != CELL_COUNT; ++cell)
	{
		cells[cell].occupantBegin = cell * (OCCUPANT_COUNT / CELL_COUNT);
		cells[cell].occupantCount = OCCUPANT_COUNT / CELL_COUNT;
		cells[cell].discoveryBase = cells[cell].occupantBegin;
	}
	for (unsigned index = 0; index != OCCUPANT_COUNT; ++index)
	{
		occupants[index].objectID = index % UNIQUE_COUNT + 20;
		occupants[index].generation = index + 2;
	}
	rts::CollisionCandidateOptions options;
	options.minimumGrain = 32;
	unsigned serialCount = 0;
	rts::CollisionCandidateResult result =
		rts::PreparePartitionCollisionCandidates(owner, cells, CELL_COUNT,
			occupants, OCCUPANT_COUNT, serialOutput, OCCUPANT_COUNT,
			serialScratch, OCCUPANT_COUNT, options, &serialCount);
	expect(result == rts::COLLISION_CANDIDATE_SERIAL,
		"partition parity oracle uses the serial candidate kernel");
	expect(serialCount == UNIQUE_COUNT,
		"partition serial oracle retains each pair exactly once");

	rts::CollisionCandidateInput genericInputs[PARITY_INPUT_COUNT];
	rts::CollisionCandidate genericSerial[PARITY_INPUT_COUNT];
	rts::CollisionCandidate genericParallel[PARITY_INPUT_COUNT];
	rts::CollisionCandidate genericSerialScratch[PARITY_INPUT_COUNT];
	rts::CollisionCandidate genericParallelScratch[PARITY_INPUT_COUNT];
	makeParityInputs(genericInputs);
	unsigned genericSerialCount = 0;
	result = rts::PrepareCollisionCandidates(genericInputs,
		PARITY_INPUT_COUNT, genericSerial, PARITY_INPUT_COUNT,
		genericSerialScratch, PARITY_INPUT_COUNT, options,
		&genericSerialCount);
	expect(result == rts::COLLISION_CANDIDATE_SERIAL &&
		genericSerialCount == PARITY_UNIQUE_COUNT,
		"reverse-oriented duplicate oracle is exact before worker matrix");

	options.parallel = true;
	const unsigned workerCounts[] = { 1, 2, 4, 8, 16 };
	for (unsigned worker = 0;
		worker != sizeof(workerCounts) / sizeof(workerCounts[0]); ++worker)
	{
		if (!startJobSystem(workerCounts[worker]))
			continue;
		rts::JobSystem &jobs = rts::JobSystem::instance();
		expect(jobs.workerCount() == workerCounts[worker],
			"worker-matrix fixture starts the exact requested lane count");
		unsigned parallelCount = 71;
		setSentinel(parallelOutput, OCCUPANT_COUNT);
		jobs.resetMetrics();
		rts::CollisionCandidateMetrics metrics;
		result = rts::PreparePartitionCollisionCandidates(owner, cells,
			CELL_COUNT, occupants, OCCUPANT_COUNT, parallelOutput,
			OCCUPANT_COUNT, parallelScratch, OCCUPANT_COUNT, options,
			&parallelCount, &metrics);
		if (workerCounts[worker] == 1)
		{
			expect(result == rts::COLLISION_CANDIDATE_SERIAL_FALLBACK &&
				parallelCount == 71 && isSentinel(parallelOutput[0]),
				"one-worker partition request preserves exact legacy fallback");
		}
		else
		{
			const unsigned expectedRanges = rts::JobSystem::chooseRangeCount(
				OCCUPANT_COUNT, options.minimumGrain, workerCounts[worker]);
			const unsigned expectedMaximumRange =
				(OCCUPANT_COUNT + expectedRanges - 1) / expectedRanges;
			expect(result == rts::COLLISION_CANDIDATE_PARALLEL,
				"2/4/8/16-worker partition fixture uses actual ranged jobs");
			expect(metrics.submittedJobs >= workerCounts[worker] &&
				metrics.completedJobs == metrics.submittedJobs &&
				metrics.serialFallbacks == 0,
				"large partition wave queues at least one range per configured lane");
			expect(metrics.localSortRuns == metrics.completedJobs &&
				metrics.locallyUniqueCandidates >= serialCount &&
				metrics.ownerMergeComparisons != 0 &&
				metrics.ownerMergeComparisons <
					static_cast<rts::JobMetricCounter>(OCCUPANT_COUNT) * 20 &&
				metrics.maximumRangeInputs == expectedMaximumRange,
				"workers own local sort/dedup and owner reports bounded merge cost");
			expect(metrics.physicalWorkerJobs + metrics.ownerHelpedJobs ==
				metrics.completedJobs && metrics.physicalWorkerJobs != 0 &&
				metrics.distinctPhysicalWorkers != 0 &&
				(metrics.physicalWorkerMask >> workerCounts[worker]) == 0,
				"range identities distinguish physical workers from owner help");
			expect(parallelCount == serialCount && sameCandidates(
				serialOutput, parallelOutput, serialCount),
				"all worker counts preserve exact serial partition ordering");
			const rts::JobSystemMetrics schedulerMetrics = jobs.metrics();
			expect(schedulerMetrics.maximumActiveWorkers <=
				workerCounts[worker] && schedulerMetrics.maximumActiveWorkers != 0 &&
				(workerCounts[worker] < 4 ||
				 schedulerMetrics.maximumActiveWorkers >= 2),
				"large collision wave records bounded physical-worker peak occupancy");
		}

		unsigned genericParallelCount = 72;
		setSentinel(genericParallel, PARITY_INPUT_COUNT);
		rts::CollisionCandidateMetrics genericMetrics;
		result = rts::PrepareCollisionCandidates(genericInputs,
			PARITY_INPUT_COUNT, genericParallel, PARITY_INPUT_COUNT,
			genericParallelScratch, PARITY_INPUT_COUNT, options,
			&genericParallelCount, &genericMetrics);
		if (workerCounts[worker] == 1)
		{
			expect(result == rts::COLLISION_CANDIDATE_SERIAL_FALLBACK &&
				genericParallelCount == 72 && isSentinel(genericParallel[0]),
				"one-worker reverse-pair request remains transactional");
		}
		else
		{
			expect(result == rts::COLLISION_CANDIDATE_PARALLEL &&
				genericParallelCount == genericSerialCount &&
				sameCandidates(genericSerial, genericParallel,
					genericSerialCount),
				"2/4/8/16 workers preserve reversed-pair duplicate parity");
		}
		expect(jobs.outstandingJobCount() == 0,
			"partition worker matrix drains every submitted job");
		stopJobSystem();
	}
}

class WorkerBlockerJob : public rts::Job
{
public:
	WorkerBlockerJob(std::atomic<unsigned> *started,
		std::atomic<bool> *release)
		: m_started(started), m_release(release)
	{
	}

	virtual void execute(rts::JobContext &context)
	{
		m_started->fetch_add(1, std::memory_order_release);
		while (!m_release->load(std::memory_order_acquire) &&
			!context.isCancellationRequested())
			std::this_thread::yield();
	}

private:
	std::atomic<unsigned> *m_started;
	std::atomic<bool> *m_release;
};

void testOwnerHelpIdentityAndParity()
{
	if (!startJobSystem(4))
		return;
	rts::JobSystem &jobs = rts::JobSystem::instance();
	std::atomic<unsigned> started(0);
	std::atomic<bool> release(false);
	rts::JobGroup blockers = jobs.createGroup();
	bool submitted = blockers.isValid();
	for (unsigned worker = 0; worker != jobs.workerCount() && submitted;
		++worker)
	{
		WorkerBlockerJob *job = new WorkerBlockerJob(&started, &release);
		if (!jobs.trySubmit(job, rts::JOB_PRIORITY_FRAME_CRITICAL,
			blockers).isValid())
		{
			delete job;
			submitted = false;
		}
	}
	const std::chrono::steady_clock::time_point deadline =
		std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (submitted && started.load(std::memory_order_acquire) !=
		jobs.workerCount() && std::chrono::steady_clock::now() < deadline)
		std::this_thread::yield();
	const bool allBlocked = submitted &&
		started.load(std::memory_order_acquire) == jobs.workerCount();
	expect(allBlocked, "owner-help fixture occupies every physical worker");

	if (allBlocked)
	{
		rts::CollisionCandidateInput inputs[PARITY_INPUT_COUNT];
		rts::CollisionCandidate serialOutput[PARITY_INPUT_COUNT];
		rts::CollisionCandidate ownerHelpOutput[PARITY_INPUT_COUNT];
		rts::CollisionCandidate serialScratch[PARITY_INPUT_COUNT];
		rts::CollisionCandidate ownerHelpScratch[PARITY_INPUT_COUNT];
		makeParityInputs(inputs);
		rts::CollisionCandidateOptions options;
		options.minimumGrain = 32;
		unsigned serialCount = 0;
		rts::CollisionCandidateResult result =
			rts::PrepareCollisionCandidates(inputs, PARITY_INPUT_COUNT,
				serialOutput, PARITY_INPUT_COUNT, serialScratch,
				PARITY_INPUT_COUNT, options, &serialCount);
		expect(result == rts::COLLISION_CANDIDATE_SERIAL,
			"owner-help fixture creates serial oracle");
		options.parallel = true;
		unsigned ownerHelpCount = 0;
		rts::CollisionCandidateMetrics metrics;
		result = rts::PrepareCollisionCandidates(inputs, PARITY_INPUT_COUNT,
			ownerHelpOutput, PARITY_INPUT_COUNT, ownerHelpScratch,
			PARITY_INPUT_COUNT, options, &ownerHelpCount, &metrics);
		expect(result == rts::COLLISION_CANDIDATE_PARALLEL &&
			ownerHelpCount == serialCount && sameCandidates(serialOutput,
				ownerHelpOutput, serialCount),
			"owner-help execution preserves exact reverse-pair parity");
		expect(metrics.ownerHelpedJobs == metrics.completedJobs &&
			metrics.physicalWorkerJobs == 0 &&
			metrics.physicalWorkerMask == 0,
			"owner-help identity cannot masquerade as a physical worker");
	}

	release.store(true, std::memory_order_release);
	if (!submitted)
		jobs.cancel(blockers);
	jobs.wait(blockers);
	expect(jobs.outstandingJobCount() == 0,
		"owner-help fixture releases and drains every blocker");
	stopJobSystem();
}

void testPartitionInputLimitTransactional()
{
	enum
	{
		MAXIMUM = rts::COLLISION_CANDIDATE_MAXIMUM_INPUTS,
		TOO_MANY = MAXIMUM + 1,
		UNIQUE_COUNT = 1000
	};
	static rts::PartitionCollisionOccupantSnapshot occupants[TOO_MANY];
	static rts::CollisionCandidate output[TOO_MANY];
	static rts::CollisionCandidate scratch[TOO_MANY];
	rts::PartitionCollisionObjectSnapshot owner;
	makePartitionOwner(owner);
	for (unsigned index = 0; index != TOO_MANY; ++index)
	{
		occupants[index].objectID = index % UNIQUE_COUNT + 20;
		occupants[index].generation = index + 2;
	}
	rts::PartitionCollisionCellSnapshot cell;
	cell.occupantBegin = 0;
	cell.occupantCount = MAXIMUM;
	cell.discoveryBase = 0;
	rts::CollisionCandidateOptions options;
	unsigned outputCount = UINT_MAX;
	rts::CollisionCandidateMetrics metrics;
	rts::CollisionCandidateResult result =
		rts::PreparePartitionCollisionCandidates(owner, &cell, 1, occupants,
			MAXIMUM, output, MAXIMUM, scratch, MAXIMUM, options,
			&outputCount, &metrics);
	expect(result == rts::COLLISION_CANDIDATE_SERIAL &&
		outputCount == UNIQUE_COUNT && metrics.preparedPairs == MAXIMUM,
		"partition preparation accepts exactly 65,536 occupants");

	setSentinel(output, TOO_MANY);
	outputCount = 95;
	cell.occupantCount = TOO_MANY;
	result = rts::PreparePartitionCollisionCandidates(owner, &cell, 1,
		occupants, TOO_MANY, output, TOO_MANY, scratch, TOO_MANY, options,
		&outputCount, &metrics);
	expect(result == rts::COLLISION_CANDIDATE_INVALID_INPUT,
		"partition preparation rejects exactly 65,537 occupants");
	expect(outputCount == 95 && isSentinel(output[0]) &&
		isSentinel(output[TOO_MANY - 1]),
		"over-limit partition rejection leaves output and count transactional");
}
#endif

#if defined(RTS_BUILD_CORE_EXTRAS) && \
	(!defined(_MSC_VER) || _MSC_VER >= 1300)
void testInjectedSubmissionFailure()
{
	if (!startJobSystem(4))
		return;
	rts::JobSystem &jobs = rts::JobSystem::instance();
	if (jobs.workerCount() <= 1)
	{
		expect(false, "failure fixture has multiple actual worker lanes");
		stopJobSystem();
		return;
	}
	rts::PartitionCollisionObjectSnapshot owner;
	makePartitionOwner(owner);
	rts::PartitionCollisionCellSnapshot cell;
	cell.occupantBegin = 0;
	cell.occupantCount = PARITY_INPUT_COUNT;
	cell.discoveryBase = 0;
	rts::PartitionCollisionOccupantSnapshot occupants[PARITY_INPUT_COUNT];
	rts::CollisionCandidate output[PARITY_INPUT_COUNT];
	rts::CollisionCandidate scratch[PARITY_INPUT_COUNT];
	for (unsigned index = 0; index != PARITY_INPUT_COUNT; ++index)
	{
		occupants[index].objectID = index % PARITY_UNIQUE_COUNT + 20;
		occupants[index].generation = index + 2;
	}
	setSentinel(output, PARITY_INPUT_COUNT);
	rts::CollisionCandidateOptions options;
	options.parallel = true;
	options.minimumGrain = 64;
	unsigned outputCount = 93;
	rts::CollisionCandidateMetrics metrics;
	// Reject the second queue publication after one accepted job. The kernel
	// must cancel/fence that partial wave before returning its serial fallback.
	rts_job_system_set_test_fault(6, 2);
	const rts::CollisionCandidateResult result =
		rts::PreparePartitionCollisionCandidates(owner, &cell, 1, occupants,
			PARITY_INPUT_COUNT, output, PARITY_INPUT_COUNT, scratch,
			PARITY_INPUT_COUNT, options, &outputCount, &metrics);
	rts_job_system_set_test_fault(0, 0);
	expect(result == rts::COLLISION_CANDIDATE_SERIAL_FALLBACK,
		"injected JobSystem submission failure returns serial fallback");
	expect(metrics.serialFallbacks == 1,
		"injected failure records the fallback");
	expect(outputCount == 93 && isSentinel(output[0]) &&
		isSentinel(output[PARITY_INPUT_COUNT - 1]),
		"injected partition submission failure leaves output and count transactional");
	expect(jobs.outstandingJobCount() == 0,
		"injected collision failure drains accepted work");
	stopJobSystem();
}

void testInjectedAllocationFailure()
{
	if (!startJobSystem(4))
		return;
	rts::PartitionCollisionObjectSnapshot owner;
	makePartitionOwner(owner);
	rts::PartitionCollisionCellSnapshot cell;
	cell.occupantBegin = 0;
	cell.occupantCount = PARITY_INPUT_COUNT;
	cell.discoveryBase = 0;
	rts::PartitionCollisionOccupantSnapshot occupants[PARITY_INPUT_COUNT];
	rts::CollisionCandidate output[PARITY_INPUT_COUNT];
	rts::CollisionCandidate scratch[PARITY_INPUT_COUNT];
	for (unsigned index = 0; index != PARITY_INPUT_COUNT; ++index)
	{
		occupants[index].objectID = index % PARITY_UNIQUE_COUNT + 20;
		occupants[index].generation = index + 2;
	}
	setSentinel(output, PARITY_INPUT_COUNT);
	rts::CollisionCandidateOptions options;
	options.parallel = true;
	options.minimumGrain = 64;
	unsigned outputCount = 94;
	rts::CollisionCandidateMetrics metrics;
	rts_collision_candidate_set_test_allocation_failure(1);
	const rts::CollisionCandidateResult result =
		rts::PreparePartitionCollisionCandidates(owner, &cell, 1, occupants,
			PARITY_INPUT_COUNT, output, PARITY_INPUT_COUNT, scratch,
			PARITY_INPUT_COUNT, options, &outputCount, &metrics);
	rts_collision_candidate_set_test_allocation_failure(0);
	expect(result == rts::COLLISION_CANDIDATE_SERIAL_FALLBACK &&
		metrics.serialFallbacks == 1,
		"injected kernel allocation failure returns an explicit fallback");
	expect(outputCount == 94 && isSentinel(output[0]) &&
		isSentinel(output[PARITY_INPUT_COUNT - 1]),
		"partition allocation failure leaves output and count transactional");
	expect(rts::JobSystem::instance().outstandingJobCount() == 0,
		"allocation failure publishes no jobs");
	stopJobSystem();
}
#endif
}

int main()
{
	testUnsignedKey();
	testAdmissionSamplesWholeEncounterSpan();
	testOrderAndDedup();
	testTransactionalFailure();
	testPartialOverlapRejectedInSerialAndParallelModes();
	testEmptyInputAcceptsNullAndAliasedBuffers();
	testColdParallelRequestDoesNotStartWorkers();
	testOneWorkerFallbackAndCancellation();
	testPartitionOrderingGenerationAndCallbackDestruction();
#if !defined(_MSC_VER) || _MSC_VER >= 1300
	testActualWorkerMatrixParity();
	testOwnerHelpIdentityAndParity();
	testPartitionInputLimitTransactional();
#endif
#if defined(RTS_BUILD_CORE_EXTRAS) && \
	(!defined(_MSC_VER) || _MSC_VER >= 1300)
	testInjectedSubmissionFailure();
	testInjectedAllocationFailure();
#endif
	if (failures != 0)
		return 1;
	printf("Collision candidate kernel tests passed.\n");
	return 0;
}
