/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2025 Electronic Arts Inc.
** SPDX-License-Identifier: GPL-3.0-or-later
*/
#pragma once

#include "Lib/JobSystem.h"
#if defined(_WIN64)
#include "Lib/KernelPerformanceDiagnostics.h"
#include "Lib/KernelPerformanceReference.h"
#endif

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

#if defined(_WIN64)
enum
{
	// Bump when the typed collision reference field layout changes. The
	// production/reference callbacks bind this schema to both digests.
	COLLISION_CANDIDATE_REFERENCE_FIELD_SCHEMA = 1
};

// Canonical reference input is an immutable owner snapshot plus the exact
// cell/occupant request order. Pointer values are transport only; callbacks
// emit every pointed-to scalar field in order.
struct PartitionCollisionReferenceInput
{
	PartitionCollisionObjectSnapshot owner;
	const PartitionCollisionCellSnapshot *cells;
	unsigned cellCount;
	const PartitionCollisionOccupantSnapshot *occupants;
	unsigned occupantCount;
	CollisionCandidateOrder order;
};

// Production output and detached serial-oracle output share this descriptor.
// The serial callback writes candidates/scratch only in its detached instance;
// the production descriptor has scratch == 0 and is read-only by callbacks.
struct PartitionCollisionReferenceOutput
{
	CollisionCandidate *candidates;
	CollisionCandidate *scratch;
	unsigned count;
	unsigned capacity;
};

// Owner-only transport for one already-live-validated collision batch. The
// shared kernel never calls reference callbacks on worker threads; the title
// supplies immutable input/production views and, for SerialOracle mode, the
// detached output descriptor.
struct CollisionCandidateReferenceBatchTransport
{
	CollisionCandidateReferenceBatchTransport();
	performance::KernelPerformanceReferenceLedger *referenceLedger;
	performance::KernelPerformanceReferenceBatch *referenceBatch;
	performance::KernelPerformanceCanonicalCallback writeInput;
	const void *immutableInput;
	performance::KernelPerformanceCanonicalCallback writeOutput;
	const void *productionOutput;
	performance::KernelPerformanceSerialCallback serialCompute;
	void *detachedSerialOutput;
	JobMetricCounter operationCount;
	unsigned fieldSchema;
};

bool WritePartitionCollisionReferenceInput(
	performance::KernelPerformanceCanonicalWriter &writer,
	const void *context);
bool WritePartitionCollisionReferenceOutput(
	performance::KernelPerformanceCanonicalWriter &writer,
	const void *context);
#endif

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
#if defined(_WIN64)
	// Optional owner-created diagnostic token.  The shared kernel only consumes
	// this transport on native x64; an invalid token is inert and never changes
	// candidate preparation or its fallback behavior.
	performance::KernelPerformanceLedger *performanceLedger;
	performance::KernelPerformanceBatch performanceBatch;
	// Optional owner-created canonical-reference transport. The shared kernel
	// never invokes callbacks on worker threads; the owner fills this output
	// token only after live validation and closes it after authoritative
	// publication (or fallback). Null pointers are inert.
	performance::KernelPerformanceReferenceLedger *performanceReferenceLedger;
	performance::KernelPerformanceReferenceBatch *performanceReferenceBatch;
	// Dedicated detached serial-oracle output. It is never an alias for the
	// authoritative output or worker scratch; capacity zero keeps the optional
	// transport inert in disabled/throughput modes.
	CollisionCandidate *performanceReferenceOutput;
	unsigned performanceReferenceOutputCapacity;
#endif
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

#if defined(_WIN64)
// Replays the exact owner-side serial normalization/sort/dedup path into
// detached storage for KernelPerformanceReferenceLedger's serial oracle.
// It performs no contact publication and never touches authoritative output.
bool ComputePartitionCollisionCandidatesSerialReference(
	const void *immutableInput, void *detachedOutput);

bool ObserveCollisionCandidateReferenceBatch(
	performance::KernelPerformanceLedger *timingLedger,
	const performance::KernelPerformanceBatch *timingBatch,
	CollisionCandidateReferenceBatchTransport *transport) noexcept;
bool FinishCollisionCandidateReferenceBatch(
	CollisionCandidateReferenceBatchTransport *transport,
	bool committed) noexcept;
#endif

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
