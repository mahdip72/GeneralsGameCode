/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2026 TheSuperHackers
** SPDX-License-Identifier: GPL-3.0-or-later
*/
#pragma once

#include "Common/GameCommon.h"
#include "Lib/ImmutableSpatialQuery.h"
#if defined(_WIN64)
#include "Lib/ImmutableSpatialQueryRuntime.h"
#endif

class Object;
class PartitionManager;
class UpdateModule;
struct Coord3D;

typedef void (*LiveImmutableSpatialObjectCommitCallback)(Object *object,
	void *context);

enum LiveImmutableSpatialConsumer
{
	LIVE_IMMUTABLE_SPATIAL_HEALING = 0,
	LIVE_IMMUTABLE_SPATIAL_POINT_DEFENSE_LASER
};

enum LiveImmutableSpatialQueryResult
{
	LIVE_IMMUTABLE_SPATIAL_QUERY_SUCCESS = 0,
	LIVE_IMMUTABLE_SPATIAL_QUERY_POLICY_FALLBACK,
	LIVE_IMMUTABLE_SPATIAL_QUERY_UNEXPECTED_FALLBACK
};

enum LiveImmutableSpatialCollectionPreflightResult
{
	LIVE_IMMUTABLE_SPATIAL_COLLECTION_ELIGIBLE = 0,
	LIVE_IMMUTABLE_SPATIAL_COLLECTION_POLICY_FALLBACK,
	LIVE_IMMUTABLE_SPATIAL_COLLECTION_FAILED
};

struct LiveImmutableSpatialResultView
{
	LiveImmutableSpatialResultView()
		: results(nullptr), count(0), queryOrdinal(0), batchEpoch(0)
	{
	}

	const rts::ImmutableSpatialResult *results;
	UnsignedInt count;
	UnsignedInt queryOrdinal;
	UnsignedInt batchEpoch;
};

// The persistent owner arena is published immediately before each exact due
// spatial consumer group. Earlier PHASE_NORMAL movers have committed first;
// touched-cell rebuilds invalidate topology because remove/re-add can change
// discovery order. Topology is reused only when no touched-cell rebuild ran.
Bool ShouldCaptureLiveImmutableSpatialArena(Bool alreadyCaptured,
	Bool dueNormalModule);
Bool CaptureLiveImmutableSpatialArena(PartitionManager *manager,
	UnsignedInt frame);
// Admission is checked before the owner captures or sorts the arena. A
// collection that cannot use at least two physical workers stays entirely on
// the unchanged scalar path and records no arena capture.
LiveImmutableSpatialCollectionPreflightResult
PreflightLiveImmutableSpatialQueryScheduler();
LiveImmutableSpatialCollectionPreflightResult
PreflightLiveImmutableSpatialQueryCollection(
	UnsignedInt queueableQueryCount);
// Uses cached radius-span and partition occupancy counters only. It never
// walks live cells or COI membership before the admission decision.
Bool MeasureLiveImmutableSpatialQueryCost(PartitionManager *manager,
	const Coord3D *position, Real maximumDistance, UnsignedInt *cellVisits,
	UnsignedInt *memberVisits);
LiveImmutableSpatialCollectionPreflightResult
PreflightLiveImmutableSpatialQueryCollectionCost(PartitionManager *manager,
	UnsignedInt queueableQueryCount, UnsignedInt queryCellVisits,
	UnsignedInt queryMemberVisits, UnsignedInt maximumRangeCost,
	UnsignedInt ownerScanCount, UnsignedInt ownerSortComparisons,
	UnsignedInt ownerLookupComparisons);
void PublishLiveImmutableSpatialNoCaptureState(
	LiveImmutableSpatialCollectionPreflightResult result,
	PartitionManager *manager, UnsignedInt frame);
Bool IsLiveImmutableSpatialConsumerQueueable(
	LiveImmutableSpatialConsumer consumer);
// The owner queues every due exact consumer against the same arena, then
// publishes one all-or-nothing multi-query batch. Collections smaller than two
// queries are deliberately left on the unchanged scalar path; no one-range
// worker submission is performed.
Bool BeginLiveImmutableSpatialQueryCollection(UnsignedInt maximumQueryCount);
Bool QueueLiveImmutableSpatialQuery(UpdateModule *owner,
	PartitionManager *manager, const Coord3D *position, Real maximumDistance,
	UnsignedInt frame, LiveImmutableSpatialConsumer consumer);
void ExecuteLiveImmutableSpatialQueryCollection();
// End the owner's collection lifetime by aborting any unconsumed diagnostic
// batch. This is idempotent and leaves arena/query state and gameplay intact.
void EndLiveImmutableSpatialQueryCollection();
void ResetLiveImmutableSpatialRuntime();
void InvalidateLiveImmutableSpatialLifecycle();
void InvalidateLiveImmutableSpatialTopology();
void InvalidateLiveImmutableSpatialFacts();

LiveImmutableSpatialQueryResult QueryLiveImmutableSpatialCandidates(
	UpdateModule *owner,
	PartitionManager *manager,
	const Coord3D *position,
	Real maximumDistance,
	UnsignedInt frame,
	LiveImmutableSpatialConsumer consumer,
	LiveImmutableSpatialResultView *view);

// Returned candidates are re-resolved on the owner and generation-checked.
// Both buffers have capacity for every real object captured in the arena and
// are reused transactionally for the live-filtered worker and local-oracle ID
// sequences.
Object *ResolveLiveImmutableSpatialResult(
	const rts::ImmutableSpatialResult &result);
Object *ResolveLiveImmutableSpatialObjectID(ObjectID objectID);
Bool ValidateLiveImmutableSpatialResultView(
	const LiveImmutableSpatialResultView &view);
Bool GetLiveImmutableSpatialIDBuffers(ObjectID **first, ObjectID **second,
	UnsignedInt *capacity);
// Callers pre-resolve the exact mutation sequence into this bounded persistent
// pointer buffer before the first callback. The commit walk never re-resolves
// IDs or consults a generation that an earlier callback may invalidate.
Bool GetLiveImmutableSpatialCommitBuffer(Object ***objects,
	UnsignedInt *capacity);
void CommitLiveImmutableSpatialObjectSequence(Object *const *objects,
	UnsignedInt count, LiveImmutableSpatialObjectCommitCallback callback,
	void *context);

// Performance instrumentation is owner-thread only and intentionally has no
// gameplay meaning.  Commit intervals bracket the exact state publication in
// each consumer; completion closes the shared collection token only after all
// queued consumers have returned (including fallback/abort paths).
#if defined(_WIN64)
rts::ImmutableSpatialConsumerCompletionToken CaptureLiveImmutableSpatialCompletion(
	UpdateModule *owner, LiveImmutableSpatialConsumer consumer);
void BeginLiveImmutableSpatialCommit(LiveImmutableSpatialConsumer consumer,
	const rts::ImmutableSpatialConsumerCompletionToken &token);
void EndLiveImmutableSpatialCommit(LiveImmutableSpatialConsumer consumer,
	const rts::ImmutableSpatialConsumerCompletionToken &token);
void CompleteLiveImmutableSpatialConsumer(LiveImmutableSpatialConsumer consumer,
	const rts::ImmutableSpatialConsumerCompletionToken &token, Bool committed);
#endif

void RecordLiveImmutableSpatialAuthoritativeQuery(
	LiveImmutableSpatialConsumer consumer, UnsignedInt candidateCount);
void RecordLiveImmutableSpatialShadowQuery(
	LiveImmutableSpatialConsumer consumer, Bool matched);
void DisableLiveImmutableSpatialConsumer(
	LiveImmutableSpatialConsumer consumer);
void RecordLiveImmutableSpatialUnexpectedFallback(
	LiveImmutableSpatialConsumer consumer, Bool stale, Bool validationFailure);
