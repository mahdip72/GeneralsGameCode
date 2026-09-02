/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2025 Electronic Arts Inc.
** SPDX-License-Identifier: GPL-3.0-or-later
*/
#pragma once

#include "Lib/JobSystem.h"

namespace rts
{
struct CollisionCandidateKey
{
	unsigned lowID;
	unsigned highID;
};

// Pointer-free owner snapshot. discoveryOrder is assigned by the owner at the
// first observation of a pair and is never derived from worker completion.
struct CollisionCandidateInput
{
	unsigned firstID;
	unsigned secondID;
	unsigned firstGeneration;
	unsigned secondGeneration;
	unsigned discoveryOrder;
};

struct CollisionCandidate
{
	CollisionCandidateKey key;
	unsigned firstID;
	unsigned secondID;
	unsigned firstGeneration;
	unsigned secondGeneration;
	unsigned discoveryOrder;
};

// Complete pointer-free owner state used by the live partition prefix.  The
// geometry fields are intentionally retained even though candidate enumeration
// only consumes the identity fields: the owner revalidates the complete state
// before publishing any prepared contact.
struct PartitionCollisionObjectSnapshot
{
	unsigned objectID;
	unsigned generation;
	unsigned dirtyOrder;
	float positionX;
	float positionY;
	float positionZ;
	float orientation;
	float majorRadius;
	float minorRadius;
	unsigned geometryType;
	bool smallGeometry;
};

struct PartitionCollisionCellSnapshot
{
	unsigned occupantBegin;
	unsigned occupantCount;
	unsigned discoveryBase;
};

struct PartitionCollisionOccupantSnapshot
{
	unsigned objectID;
	unsigned generation;
};

enum CollisionCandidateOrder
{
	// Reconstructs the contact list's historical prepend order after dedup.
	COLLISION_CANDIDATE_REVERSE_DISCOVERY = 0,
	// Reserved for an explicitly versioned deterministic-simulation epoch.
	COLLISION_CANDIDATE_CANONICAL_KEY = 1
};

enum CollisionCandidateResult
{
	COLLISION_CANDIDATE_SERIAL,
	COLLISION_CANDIDATE_PARALLEL,
	COLLISION_CANDIDATE_SERIAL_FALLBACK,
	COLLISION_CANDIDATE_CANCELLED,
	COLLISION_CANDIDATE_INVALID_INPUT
};

enum
{
	COLLISION_ADMISSION_SAMPLE_CAPACITY = 64,
	COLLISION_CANDIDATE_MINIMUM_PARALLEL_INPUTS = 256,
	COLLISION_CANDIDATE_DEFAULT_MINIMUM_GRAIN = 128,
	COLLISION_CANDIDATE_MAXIMUM_INPUTS = 65536
};

// Deterministic bounded reservoir used by the owner during admission. Unlike
// a prefix sample, later cells and encounters can replace earlier slots, so a
// duplicate-heavy tail cannot be hidden by an initially unique cell.
class CollisionAdmissionSampler
{
public:
	CollisionAdmissionSampler();
	void observe(unsigned objectID);
	unsigned encounterCount() const;
	unsigned sampleCount() const;
	unsigned uniqueSampleCount() const;
	bool hasUsefulSpread() const;

private:
	unsigned m_encounterCount;
	unsigned m_sampleCount;
	unsigned m_sampleIDs[COLLISION_ADMISSION_SAMPLE_CAPACITY];
};

struct CollisionCandidateOptions
{
	CollisionCandidateOptions();
	bool parallel;
	unsigned minimumGrain;
	CollisionCandidateOrder order;
	const JobGroup *cancellationGroup;
};

struct CollisionCandidateMetrics
{
	CollisionCandidateMetrics();
	unsigned preparedPairs;
	unsigned uniqueCandidates;
	unsigned submittedJobs;
	unsigned completedJobs;
	unsigned serialFallbacks;
	unsigned localSortRuns;
	unsigned locallyUniqueCandidates;
	JobMetricCounter ownerMergeComparisons;
	unsigned maximumRangeInputs;
	unsigned physicalWorkerJobs;
	unsigned ownerHelpedJobs;
	JobMetricCounter physicalWorkerMask;
	unsigned distinctPhysicalWorkers;
	bool physicalWorkerMaskComplete;
	// Exact maximum number of physical workers executing this candidate
	// preparation at the same time. This is kernel-local and non-authoritative;
	// it must not be inferred from the scheduler-wide worker high-water mark.
	unsigned peakConcurrentPhysicalWorkers;
};

struct CollisionCandidateRuntimeMetrics
{
	CollisionCandidateRuntimeMetrics();
	// Process-local lifecycle marker. It is never serialized and advances on
	// each explicit metrics reset so pre-match baselines cannot leak.
	JobMetricCounter resetEpoch;
	JobMetricCounter authoritativeCommits;
	JobMetricCounter shadowExecutions;
	JobMetricCounter shadowMismatches;
	JobMetricCounter ownerFallbacks;
	JobMetricCounter unexpectedFallbacks;
	JobMetricCounter ineligibleSlices;
	JobMetricCounter staleRejections;
	// Successful owner contact-list insertions, excluding existing pairs.
	JobMetricCounter committedCandidates;
	// Successful legacy insertions covered by a matching shadow comparison.
	JobMetricCounter shadowComparedCandidates;
	JobMetricCounter preparedPairs;
	JobMetricCounter uniqueCandidates;
	JobMetricCounter submittedJobs;
	JobMetricCounter completedJobs;
	JobMetricCounter localSortRuns;
	JobMetricCounter locallyUniqueCandidates;
	JobMetricCounter ownerMergeComparisons;
	unsigned maximumRangeInputs;
	JobMetricCounter physicalWorkerJobs;
	JobMetricCounter ownerHelpedJobs;
	JobMetricCounter physicalWorkerMask;
	// Maximum exact distinct-worker count observed in one accepted batch.  The
	// fixed-width mask is diagnostic only and may be incomplete on large hosts.
	unsigned distinctPhysicalWorkers;
	bool physicalWorkerMaskComplete;
	// Maximum kernel-local physical-worker overlap observed by an accepted
	// candidate preparation. This is independent from distinct worker count.
	unsigned maximumPeakConcurrentPhysicalWorkers;
};

typedef bool (*CollisionCandidateGenerationResolver)(unsigned objectID,
	unsigned generation, void *context);

// Object IDs are treated as unsigned values before comparison or hashing. The
// key is independent of callback orientation and contains no signed shifts.
bool MakeCollisionCandidateKey(unsigned firstID, unsigned secondID,
	CollisionCandidateKey &key);
unsigned HashCollisionCandidateKey(const CollisionCandidateKey &key);

// Inputs, output and scratch remain owner-owned for the complete synchronous
// call. Workers only normalize their input range into the same disjoint scratch
// slots. Each worker sorts and deduplicates its own range. The calling owner
// performs a deterministic fixed-range-order k-way merge after the fence and
// publishes transactionally. Failure leaves output and outputCount untouched
// so the owner can execute its legacy reference path. For a nonempty call, the
// active input, output, and scratch byte ranges must be pairwise disjoint.
CollisionCandidateResult PrepareCollisionCandidates(
	const CollisionCandidateInput *inputs,
	unsigned inputCount,
	CollisionCandidate *output,
	unsigned outputCapacity,
	CollisionCandidate *scratch,
	unsigned scratchCapacity,
	const CollisionCandidateOptions &options,
	unsigned *outputCount,
	CollisionCandidateMetrics *metrics = 0);

// The cell table defines the owner's immutable legacy discovery order.  Worker
// ranges read only the pointer-free owner/occupant snapshots and write disjoint
// scratch slots. Each range is locally sorted/deduplicated; deterministic k-way
// reduction and transactional publication happen on the owner after the fence.
// The active cell, occupant, output, and scratch byte ranges must be pairwise
// disjoint for nonempty calls. Any failure leaves output/outputCount untouched.
CollisionCandidateResult PreparePartitionCollisionCandidates(
	const PartitionCollisionObjectSnapshot &owner,
	const PartitionCollisionCellSnapshot *cells,
	unsigned cellCount,
	const PartitionCollisionOccupantSnapshot *occupants,
	unsigned occupantCount,
	CollisionCandidate *output,
	unsigned outputCapacity,
	CollisionCandidate *scratch,
	unsigned scratchCapacity,
	const CollisionCandidateOptions &options,
	unsigned *outputCount,
	CollisionCandidateMetrics *metrics = 0);

// Compares the complete prepared representation, including callback
// orientation, snapshot generations, and discovery order.  On success,
// firstDifference is the common count; on mismatch it is the first differing
// element or the shorter count when only the counts differ.
bool CollisionCandidatesEqual(
	const CollisionCandidate *left,
	unsigned leftCount,
	const CollisionCandidate *right,
	unsigned rightCount,
	unsigned *firstDifference = 0);

bool ValidateCollisionCandidateGenerations(
	const CollisionCandidate *candidates,
	unsigned candidateCount,
	CollisionCandidateGenerationResolver resolver,
	void *context,
	unsigned *firstStaleCandidate = 0);

void ResetCollisionCandidateRuntimeMetrics();
CollisionCandidateRuntimeMetrics GetCollisionCandidateRuntimeMetrics();
void RecordCollisionCandidateOwnerCommit(bool authoritative, bool shadow,
	unsigned insertedCandidateCount);
void RecordCollisionCandidateShadowMismatch();
void RecordCollisionCandidateParallelWork(
	const CollisionCandidateMetrics &metrics);
// Records a preparation only after the owner has validated and published its
// authoritative candidate result. The non-accepted entry point above remains
// available for attempted-work diagnostics and must not be used as authority
// evidence by itself.
void RecordCollisionCandidateAcceptedParallelWork(
	const CollisionCandidateMetrics &metrics);
void RecordCollisionCandidateIneligibleSlice();
void RecordCollisionCandidateOwnerFallback(bool stale,
	bool unexpected = false);
}
