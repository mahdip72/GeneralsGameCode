/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2026 TheSuperHackers
** SPDX-License-Identifier: GPL-3.0-or-later
*/
#include "Lib/ImmutableSpatialQueryRuntime.h"
#include "Lib/JobFloatingPointState.h"

#include <atomic>
#include <new>

namespace rts
{
namespace
{
enum
{
	IMMUTABLE_SPATIAL_PHYSICAL_WAIT_MILLISECONDS = 250
};

enum SpatialExecutionIdentity
{
	SPATIAL_NOT_EXECUTED = 0,
	SPATIAL_PHYSICAL_WORKER,
	SPATIAL_OWNER_HELP
};

typedef std::atomic<unsigned> SpatialJobAtomicUnsigned;

inline unsigned incrementJobCounter(SpatialJobAtomicUnsigned &value)
{
	return value.fetch_add(1, std::memory_order_acq_rel) + 1;
}

inline void decrementJobCounter(SpatialJobAtomicUnsigned &value)
{
	value.fetch_sub(1, std::memory_order_acq_rel);
}

inline unsigned loadJobCounter(const SpatialJobAtomicUnsigned &value)
{
	return value.load(std::memory_order_relaxed);
}

inline void maximizeJobCounter(SpatialJobAtomicUnsigned &value,
	unsigned candidate)
{
	unsigned observed = value.load(std::memory_order_relaxed);
	while (observed < candidate && !value.compare_exchange_weak(observed,
		candidate, std::memory_order_relaxed, std::memory_order_relaxed)) {}
}

class SpatialPhysicalExecutionScope
{
public:
	SpatialPhysicalExecutionScope(bool physicalWorker,
		SpatialJobAtomicUnsigned *active,
		SpatialJobAtomicUnsigned *peak)
		: m_active(physicalWorker ? active : 0)
	{
		if (m_active != 0)
		{
			const unsigned current = incrementJobCounter(*m_active);
			maximizeJobCounter(*peak, current);
		}
	}

	~SpatialPhysicalExecutionScope()
	{
		if (m_active != 0)
			decrementJobCounter(*m_active);
	}

private:
	SpatialJobAtomicUnsigned *m_active;
};

struct SpatialRangeJob : public Job
{
	SpatialRangeJob(ImmutableSpatialRangeFunction rangeFunction,
		void *rangeContext, unsigned ordinal,
		SpatialExecutionIdentity *executionIdentity,
		unsigned *executionPhysicalWorkerIndex, unsigned spinIterations,
		bool fail,
		const JobFloatingPointState &floatingPointState,
		SpatialJobAtomicUnsigned *activePhysicalWorkers,
		SpatialJobAtomicUnsigned *peakPhysicalWorkers)
		: function(rangeFunction), context(rangeContext), rangeIndex(ordinal),
		  identity(executionIdentity),
		  physicalWorkerIndex(executionPhysicalWorkerIndex),
		  testSpinIterations(spinIterations), forceFailure(fail),
		  floatingPointState(floatingPointState),
		  activePhysicalWorkers(activePhysicalWorkers),
		  peakPhysicalWorkers(peakPhysicalWorkers)
	{
	}

	virtual void execute(JobContext &jobContext)
	{
		const JobFloatingPointScope floatingPointScope(floatingPointState);
		const unsigned workerIndex = jobContext.physicalWorkerIndex();
		const bool physicalWorker =
			jobContext.isPhysicalWorkerExecution() &&
			workerIndex != JOB_INVALID_PHYSICAL_WORKER_INDEX;
		if (identity != 0)
		{
			if (physicalWorker)
			{
				*identity = SPATIAL_PHYSICAL_WORKER;
				if (physicalWorkerIndex != 0)
					*physicalWorkerIndex = workerIndex;
			}
			else
			{
				*identity = SPATIAL_OWNER_HELP;
			}
		}
		SpatialPhysicalExecutionScope physicalScope(physicalWorker,
			activePhysicalWorkers, peakPhysicalWorkers);
		volatile unsigned spinValue = rangeIndex;
		for (unsigned spin = 0; spin != testSpinIterations; ++spin)
			spinValue = spinValue * 1664525u + 1013904223u;
		if (forceFailure || jobContext.isCancellationRequested() ||
			function == 0 || !function(context, rangeIndex))
			jobContext.fail();
	}

	ImmutableSpatialRangeFunction function;
	void *context;
	unsigned rangeIndex;
	SpatialExecutionIdentity *identity;
	unsigned *physicalWorkerIndex;
	unsigned testSpinIterations;
	bool forceFailure;
	const JobFloatingPointState floatingPointState;
	SpatialJobAtomicUnsigned *activePhysicalWorkers;
	SpatialJobAtomicUnsigned *peakPhysicalWorkers;
};

struct SpatialDispatchContext
{
	SpatialDispatchContext()
		: options(0), metrics(0), dispatchOrdinal(0), cancelled(false),
		  activePhysicalWorkers(0), peakPhysicalWorkers(0),
		  observedPhysicalWorkerIndices(0), observedPhysicalWorkerCount(0),
		  observedPhysicalWorkerCapacity(0),
		  floatingPointState()
	{
	}

	const ImmutableSpatialJobSystemOptions *options;
	ImmutableSpatialJobSystemMetrics *metrics;
	unsigned dispatchOrdinal;
	std::atomic<bool> cancelled;
	SpatialJobAtomicUnsigned activePhysicalWorkers;
	SpatialJobAtomicUnsigned peakPhysicalWorkers;
	unsigned *observedPhysicalWorkerIndices;
	unsigned observedPhysicalWorkerCount;
	unsigned observedPhysicalWorkerCapacity;
	const JobFloatingPointState floatingPointState;
};

void observePhysicalWorker(SpatialDispatchContext *dispatch,
	unsigned workerIndex)
{
	for (unsigned index = 0;
		index != dispatch->observedPhysicalWorkerCount; ++index)
	{
		if (dispatch->observedPhysicalWorkerIndices[index] == workerIndex)
			return;
	}
	if (dispatch->observedPhysicalWorkerCount <
		dispatch->observedPhysicalWorkerCapacity)
	{
		dispatch->observedPhysicalWorkerIndices[
			dispatch->observedPhysicalWorkerCount++] = workerIndex;
	}
	else
	{
		// The scheduler normally assigns IDs in [0, workerCount), which is the
		// capacity allocated by the wrapper. Keep the evidence fail-closed if a
		// future scheduler violates that contract.
		dispatch->metrics->physicalWorkerMaskComplete = false;
	}
}

void publishPhysicalWorkerPeak(SpatialDispatchContext *dispatch)
{
	const unsigned peak = loadJobCounter(dispatch->peakPhysicalWorkers);
	if (peak > dispatch->metrics->peakConcurrentPhysicalWorkers)
		dispatch->metrics->peakConcurrentPhysicalWorkers = peak;
}

bool spatialCancelled(void *context)
{
	SpatialDispatchContext *dispatch =
		static_cast<SpatialDispatchContext *>(context);
	return dispatch == 0 || dispatch->cancelled.load(std::memory_order_acquire);
}

bool spatialDispatch(void *context, ImmutableSpatialUInt32 rangeCount,
	ImmutableSpatialRangeFunction rangeFunction, void *rangeContext)
{
	SpatialDispatchContext *dispatch =
		static_cast<SpatialDispatchContext *>(context);
	if (dispatch == 0 || dispatch->options == 0 || dispatch->metrics == 0 ||
		rangeFunction == 0 || rangeCount == 0)
		return false;

	++dispatch->dispatchOrdinal;
	++dispatch->metrics->dispatches;
	const ImmutableSpatialJobSystemOptions &options = *dispatch->options;
	if (options.testFault == IMMUTABLE_SPATIAL_JOB_SYSTEM_TEST_GROUP_FAILURE &&
		dispatch->dispatchOrdinal == options.testDispatchOrdinal)
		return false;

	JobSystem &jobs = JobSystem::instance();
	JobGroup group = jobs.createGroup();
	if (!group.isValid())
		return false;

	SpatialRangeJob **rangeJobs = new (std::nothrow) SpatialRangeJob *[rangeCount];
	SpatialExecutionIdentity *identities = new (std::nothrow)
		SpatialExecutionIdentity[rangeCount];
	unsigned *physicalWorkerIndices = new (std::nothrow) unsigned[rangeCount];
	JobSubmission *submissions = new (std::nothrow) JobSubmission[rangeCount];
	JobHandle *handles = new (std::nothrow) JobHandle[rangeCount];
	if (rangeJobs == 0 || identities == 0 || physicalWorkerIndices == 0 ||
		submissions == 0 || handles == 0)
	{
		delete[] rangeJobs;
		delete[] identities;
		delete[] physicalWorkerIndices;
		delete[] submissions;
		delete[] handles;
		return false;
	}
	unsigned allocated = 0;
	for (; allocated != rangeCount; ++allocated)
	{
		rangeJobs[allocated] = 0;
		identities[allocated] = SPATIAL_NOT_EXECUTED;
		physicalWorkerIndices[allocated] = JOB_INVALID_PHYSICAL_WORKER_INDEX;
		const bool forceFailure =
			options.testFault == IMMUTABLE_SPATIAL_JOB_SYSTEM_TEST_RANGE_FAILURE &&
			dispatch->dispatchOrdinal == options.testDispatchOrdinal &&
			allocated == options.testRangeOrdinal;
		SpatialRangeJob *job = new (std::nothrow) SpatialRangeJob(
			rangeFunction, rangeContext, allocated, identities + allocated,
			physicalWorkerIndices + allocated, options.testSpinIterations,
			forceFailure, dispatch->floatingPointState,
			&dispatch->activePhysicalWorkers, &dispatch->peakPhysicalWorkers);
		if (job == 0)
			break;
		rangeJobs[allocated] = job;
		submissions[allocated].job = job;
		submissions[allocated].priority = JOB_PRIORITY_FRAME_CRITICAL;
	}
	if (allocated != rangeCount)
	{
		for (unsigned index = 0; index != allocated; ++index)
			delete rangeJobs[index];
		delete[] rangeJobs;
		delete[] identities;
		delete[] physicalWorkerIndices;
		delete[] submissions;
		delete[] handles;
		return false;
	}
	if (options.testFault == IMMUTABLE_SPATIAL_JOB_SYSTEM_TEST_ADMISSION_FAILURE &&
		dispatch->dispatchOrdinal == options.testDispatchOrdinal)
	{
		for (unsigned index = 0; index != allocated; ++index)
			delete rangeJobs[index];
		delete[] rangeJobs;
		delete[] identities;
		delete[] physicalWorkerIndices;
		delete[] submissions;
		delete[] handles;
		return false;
	}
	if (!jobs.trySubmitBatch(submissions, rangeCount, group, handles))
	{
		for (unsigned index = 0; index != allocated; ++index)
			delete rangeJobs[index];
		delete[] rangeJobs;
		delete[] identities;
		delete[] physicalWorkerIndices;
		delete[] submissions;
		delete[] handles;
		return false;
	}
	dispatch->metrics->submittedJobs += rangeCount;
	dispatch->metrics->ranges += rangeCount;

	if (options.testFault ==
		IMMUTABLE_SPATIAL_JOB_SYSTEM_TEST_CANCEL_AFTER_ADMISSION &&
		dispatch->dispatchOrdinal == options.testDispatchOrdinal)
	{
		dispatch->cancelled.store(true, std::memory_order_release);
		jobs.cancel(group);
	}

	const bool forceTimeout = options.testFault ==
		IMMUTABLE_SPATIAL_JOB_SYSTEM_TEST_TIMEOUT &&
		dispatch->dispatchOrdinal == options.testDispatchOrdinal;
	if (forceTimeout || !jobs.waitWithoutOwnerHelp(group,
		IMMUTABLE_SPATIAL_PHYSICAL_WAIT_MILLISECONDS))
	{
		dispatch->cancelled.store(true, std::memory_order_release);
		jobs.cancel(group);
		// Cancellation makes queued jobs terminal. Any already executing range
		// owns only immutable input and private scratch, so drain before the
		// dispatch-owned metadata leaves scope.
		jobs.wait(group);
		publishPhysicalWorkerPeak(dispatch);
		delete[] rangeJobs;
		delete[] identities;
		delete[] physicalWorkerIndices;
		delete[] submissions;
		delete[] handles;
		return false;
	}
	if (!jobs.wait(group))
	{
		publishPhysicalWorkerPeak(dispatch);
		delete[] rangeJobs;
		delete[] identities;
		delete[] physicalWorkerIndices;
		delete[] submissions;
		delete[] handles;
		return false;
	}
	publishPhysicalWorkerPeak(dispatch);

	bool succeeded = !group.wasCancelled() && !group.failed();
	for (unsigned completionIndex = 0; completionIndex != rangeCount;
		++completionIndex)
	{
		if (handles[completionIndex].succeeded())
			++dispatch->metrics->completedJobs;
		else
			succeeded = false;
		if (identities[completionIndex] == SPATIAL_PHYSICAL_WORKER)
		{
			++dispatch->metrics->physicalWorkerJobs;
			const unsigned workerIndex = physicalWorkerIndices[completionIndex];
			if (workerIndex < sizeof(JobMetricCounter) * 8)
				dispatch->metrics->physicalWorkerMask |=
					static_cast<JobMetricCounter>(1) << workerIndex;
			else
				dispatch->metrics->physicalWorkerMaskComplete = false;
			observePhysicalWorker(dispatch, workerIndex);
		}
		else if (identities[completionIndex] == SPATIAL_OWNER_HELP)
		{
			++dispatch->metrics->ownerHelpedJobs;
			succeeded = false;
		}
	}
	if (dispatch->observedPhysicalWorkerCount >
		dispatch->metrics->distinctPhysicalWorkers)
		dispatch->metrics->distinctPhysicalWorkers =
		dispatch->observedPhysicalWorkerCount;
	delete[] rangeJobs;
	delete[] identities;
	delete[] physicalWorkerIndices;
	delete[] submissions;
	delete[] handles;
	return succeeded;
}

typedef std::atomic<ImmutableSpatialMetricCounter> SpatialMetricAtomic;

struct SpatialConsumerMetricAtomics
{
	SpatialConsumerMetricAtomics()
		: eligibleQueries(0), authoritativeQueries(0),
		  authoritativeCandidates(0), shadowQueries(0), shadowMatches(0),
		  shadowMismatches(0), submittedJobs(0), completedJobs(0),
		  physicalWorkerJobs(0), ownerHelpedJobs(0), expectedFallbacks(0),
		  unexpectedFallbacks(0), staleRejections(0), validationFailures(0),
		  circuitBreakerTrips(0)
	{
	}

	SpatialMetricAtomic eligibleQueries;
	SpatialMetricAtomic authoritativeQueries;
	SpatialMetricAtomic authoritativeCandidates;
	SpatialMetricAtomic shadowQueries;
	SpatialMetricAtomic shadowMatches;
	SpatialMetricAtomic shadowMismatches;
	SpatialMetricAtomic submittedJobs;
	SpatialMetricAtomic completedJobs;
	SpatialMetricAtomic physicalWorkerJobs;
	SpatialMetricAtomic ownerHelpedJobs;
	SpatialMetricAtomic expectedFallbacks;
	SpatialMetricAtomic unexpectedFallbacks;
	SpatialMetricAtomic staleRejections;
	SpatialMetricAtomic validationFailures;
	SpatialMetricAtomic circuitBreakerTrips;
};

SpatialMetricAtomic s_resetEpoch(0);
SpatialMetricAtomic s_capturedArenas(0);
SpatialMetricAtomic s_captureFailures(0);
SpatialMetricAtomic s_successfulCollections(0);
SpatialMetricAtomic s_successfulCollectionQueries(0);
SpatialMetricAtomic s_successfulCollectionRanges(0);
SpatialMetricAtomic s_multiRangeCollections(0);
SpatialMetricAtomic s_collectionSubmittedJobs(0);
SpatialMetricAtomic s_collectionCompletedJobs(0);
SpatialMetricAtomic s_collectionPhysicalWorkerJobs(0);
SpatialMetricAtomic s_collectionOwnerHelpedJobs(0);
SpatialMetricAtomic s_collectionPhysicalWorkerMask(0);
SpatialMetricAtomic s_collectionPhysicalWorkerMaskIncomplete(0);
SpatialMetricAtomic s_maximumCollectionQueries(0);
SpatialMetricAtomic s_maximumCollectionRanges(0);
SpatialMetricAtomic s_maximumCollectionDistinctPhysicalWorkers(0);
SpatialMetricAtomic s_maximumCollectionPeakConcurrentPhysicalWorkers(0);
SpatialConsumerMetricAtomics s_consumers[IMMUTABLE_SPATIAL_CONSUMER_COUNT];

void resetMetric(SpatialMetricAtomic &metric)
{
	metric.store(0, std::memory_order_relaxed);
}

void addMetric(SpatialMetricAtomic &metric, ImmutableSpatialMetricCounter amount)
{
	metric.fetch_add(amount, std::memory_order_relaxed);
}

void orMetric(SpatialMetricAtomic &metric, ImmutableSpatialMetricCounter bits)
{
	metric.fetch_or(bits, std::memory_order_relaxed);
}

unsigned countMetricBits(ImmutableSpatialMetricCounter bits)
{
	unsigned count = 0;
	while (bits != 0)
	{
		bits &= bits - 1;
		++count;
	}
	return count;
}

void maximizeMetric(SpatialMetricAtomic &metric,
	ImmutableSpatialMetricCounter candidate)
{
	ImmutableSpatialMetricCounter current = metric.load(std::memory_order_relaxed);
	while (current < candidate && !metric.compare_exchange_weak(current,
		candidate, std::memory_order_relaxed, std::memory_order_relaxed))
	{
	}
}

ImmutableSpatialMetricCounter loadMetric(const SpatialMetricAtomic &metric)
{
	return metric.load(std::memory_order_relaxed);
}

SpatialConsumerMetricAtomics *consumerAtomics(ImmutableSpatialConsumer consumer)
{
	if (consumer < IMMUTABLE_SPATIAL_CONSUMER_HEALING ||
		consumer >= IMMUTABLE_SPATIAL_CONSUMER_COUNT)
		return 0;
	return &s_consumers[consumer];
}

void resetConsumer(SpatialConsumerMetricAtomics &metrics)
{
	resetMetric(metrics.eligibleQueries);
	resetMetric(metrics.authoritativeQueries);
	resetMetric(metrics.authoritativeCandidates);
	resetMetric(metrics.shadowQueries);
	resetMetric(metrics.shadowMatches);
	resetMetric(metrics.shadowMismatches);
	resetMetric(metrics.submittedJobs);
	resetMetric(metrics.completedJobs);
	resetMetric(metrics.physicalWorkerJobs);
	resetMetric(metrics.ownerHelpedJobs);
	resetMetric(metrics.expectedFallbacks);
	resetMetric(metrics.unexpectedFallbacks);
	resetMetric(metrics.staleRejections);
	resetMetric(metrics.validationFailures);
	resetMetric(metrics.circuitBreakerTrips);
}

ImmutableSpatialConsumerRuntimeMetrics loadConsumer(
	const SpatialConsumerMetricAtomics &source)
{
	ImmutableSpatialConsumerRuntimeMetrics result;
	result.eligibleQueries = loadMetric(source.eligibleQueries);
	result.authoritativeQueries = loadMetric(source.authoritativeQueries);
	result.authoritativeCandidates = loadMetric(source.authoritativeCandidates);
	result.shadowQueries = loadMetric(source.shadowQueries);
	result.shadowMatches = loadMetric(source.shadowMatches);
	result.shadowMismatches = loadMetric(source.shadowMismatches);
	result.submittedJobs = loadMetric(source.submittedJobs);
	result.completedJobs = loadMetric(source.completedJobs);
	result.physicalWorkerJobs = loadMetric(source.physicalWorkerJobs);
	result.ownerHelpedJobs = loadMetric(source.ownerHelpedJobs);
	result.expectedFallbacks = loadMetric(source.expectedFallbacks);
	result.unexpectedFallbacks = loadMetric(source.unexpectedFallbacks);
	result.staleRejections = loadMetric(source.staleRejections);
	result.validationFailures = loadMetric(source.validationFailures);
	result.circuitBreakerTrips = loadMetric(source.circuitBreakerTrips);
	return result;
}

void recordJobMetrics(SpatialConsumerMetricAtomics &target,
	const ImmutableSpatialJobSystemMetrics &metrics)
{
	addMetric(target.submittedJobs, metrics.submittedJobs);
	addMetric(target.completedJobs, metrics.completedJobs);
	addMetric(target.physicalWorkerJobs, metrics.physicalWorkerJobs);
	addMetric(target.ownerHelpedJobs, metrics.ownerHelpedJobs);
}
}

bool ShouldDispatchImmutableSpatialQueryCollection(unsigned queryCount,
	unsigned workerCount)
{
	return queryCount >= 2 && workerCount >= 2;
}

ImmutableSpatialAdmissionCost::ImmutableSpatialAdmissionCost()
	: queryCount(0), workerCount(0), queryCellVisits(0),
	  queryMemberVisits(0), objectCount(0), cellCount(0), memberCount(0),
	  radiusOffsetCount(0), maximumRangeCost(0), ownerScanCount(0),
	  ownerSortComparisons(0), ownerLookupComparisons(0),
	  rebuildTopology(false), refreshFacts(false)
{
}

namespace
{
bool addAdmissionCost(ImmutableSpatialMetricCounter value,
	ImmutableSpatialMetricCounter &total)
{
	const ImmutableSpatialMetricCounter maximum =
		~static_cast<ImmutableSpatialMetricCounter>(0);
	if (value > maximum - total)
		return false;
	total += value;
	return true;
}

bool addScaledAdmissionCost(ImmutableSpatialMetricCounter value,
	ImmutableSpatialMetricCounter scale,
	ImmutableSpatialMetricCounter &total)
{
	const ImmutableSpatialMetricCounter maximum =
		~static_cast<ImmutableSpatialMetricCounter>(0);
	if (scale != 0 && value > maximum / scale)
		return false;
	return addAdmissionCost(value * scale, total);
}
}

ImmutableSpatialAdmissionResult EvaluateImmutableSpatialQueryAdmission(
	const ImmutableSpatialAdmissionCost &cost,
	ImmutableSpatialMetricCounter *legacyCost,
	ImmutableSpatialMetricCounter *parallelCost)
{
	if (legacyCost != 0)
		*legacyCost = 0;
	if (parallelCost != 0)
		*parallelCost = 0;
	if (!ShouldDispatchImmutableSpatialQueryCollection(cost.queryCount,
		cost.workerCount))
		return IMMUTABLE_SPATIAL_ADMISSION_POLICY_INELIGIBLE;
	if (cost.queryCellVisits == 0 || cost.queryMemberVisits == 0 ||
		cost.cellCount == 0 || cost.radiusOffsetCount == 0 ||
		(cost.rebuildTopology && cost.refreshFacts))
		return IMMUTABLE_SPATIAL_ADMISSION_POLICY_INELIGIBLE;

	// These weights are benchmark-calibrated operation units, never wall-clock
	// time. Every peer therefore makes the same decision while the model still
	// accounts for the measured complete owner transaction.
	ImmutableSpatialMetricCounter serial = 0;
	if (!addScaledAdmissionCost(cost.queryCellVisits, 8, serial) ||
		!addScaledAdmissionCost(cost.queryMemberVisits, 24, serial))
		return IMMUTABLE_SPATIAL_ADMISSION_INVALID;

	const unsigned rangeCount = cost.queryCount < cost.workerCount ?
		cost.queryCount : cost.workerCount;
	ImmutableSpatialMetricCounter parallel = 4096; // passive wait/fence
	if (!addScaledAdmissionCost(cost.queryCount, 64, parallel) ||
		!addScaledAdmissionCost(rangeCount, 2048, parallel) ||
		!addScaledAdmissionCost(cost.queryMemberVisits, 4, parallel) ||
		!addScaledAdmissionCost(cost.queryCount, 32, parallel) ||
		!addScaledAdmissionCost(cost.ownerScanCount, 8, parallel) ||
		!addScaledAdmissionCost(cost.ownerSortComparisons, 16, parallel) ||
		!addScaledAdmissionCost(cost.ownerLookupComparisons, 12, parallel))
		return IMMUTABLE_SPATIAL_ADMISSION_INVALID;
	const ImmutableSpatialMetricCounter maximumRangeCost =
		cost.maximumRangeCost != 0 ? cost.maximumRangeCost :
		serial / rangeCount + (serial % rangeCount != 0 ? 1 : 0);
	if (!addAdmissionCost(maximumRangeCost, parallel))
		return IMMUTABLE_SPATIAL_ADMISSION_INVALID;

	if (cost.rebuildTopology)
	{
		if (!addScaledAdmissionCost(cost.objectCount, 20, parallel) ||
			!addScaledAdmissionCost(cost.cellCount, 4, parallel) ||
			!addScaledAdmissionCost(cost.memberCount, 8, parallel) ||
			!addScaledAdmissionCost(cost.radiusOffsetCount, 4, parallel))
			return IMMUTABLE_SPATIAL_ADMISSION_INVALID;
	}
	else if (cost.refreshFacts &&
		!addScaledAdmissionCost(cost.objectCount, 16, parallel))
	{
		return IMMUTABLE_SPATIAL_ADMISSION_INVALID;
	}

	if (legacyCost != 0)
		*legacyCost = serial;
	if (parallelCost != 0)
		*parallelCost = parallel;
	return parallel < serial ? IMMUTABLE_SPATIAL_ADMISSION_ELIGIBLE :
		IMMUTABLE_SPATIAL_ADMISSION_POLICY_INELIGIBLE;
}

ImmutableSpatialJobSystemOptions::ImmutableSpatialJobSystemOptions()
	: testFault(IMMUTABLE_SPATIAL_JOB_SYSTEM_TEST_NONE),
	  testDispatchOrdinal(1), testRangeOrdinal(0), testSpinIterations(0)
{
}

ImmutableSpatialJobSystemMetrics::ImmutableSpatialJobSystemMetrics()
	: dispatches(0), ranges(0), submittedJobs(0), completedJobs(0),
	  physicalWorkerJobs(0), ownerHelpedJobs(0), physicalWorkerMask(0),
	  distinctPhysicalWorkers(0), physicalWorkerMaskComplete(true),
	  peakConcurrentPhysicalWorkers(0)
{
}

ImmutableSpatialConsumerRuntimeMetrics::ImmutableSpatialConsumerRuntimeMetrics()
	: eligibleQueries(0), authoritativeQueries(0), authoritativeCandidates(0),
	  shadowQueries(0), shadowMatches(0), shadowMismatches(0), submittedJobs(0),
	  completedJobs(0), physicalWorkerJobs(0), ownerHelpedJobs(0),
	  expectedFallbacks(0), unexpectedFallbacks(0), staleRejections(0),
	  validationFailures(0), circuitBreakerTrips(0)
{
}

ImmutableSpatialRuntimeMetrics::ImmutableSpatialRuntimeMetrics()
	: resetEpoch(0), capturedArenas(0), captureFailures(0),
	  successfulCollections(0), successfulCollectionQueries(0),
	  successfulCollectionRanges(0), multiRangeCollections(0),
	  collectionSubmittedJobs(0), collectionCompletedJobs(0),
	  collectionPhysicalWorkerJobs(0), collectionOwnerHelpedJobs(0),
	  collectionPhysicalWorkerMask(0), collectionPhysicalWorkerMaskComplete(true),
	  maximumCollectionQueries(0), maximumCollectionRanges(0),
	  maximumCollectionDistinctPhysicalWorkers(0),
	  maximumCollectionPeakConcurrentPhysicalWorkers(0)
{
}

ImmutableSpatialJobSystemResult ExecuteImmutableSpatialQueryBatchOnJobSystem(
	const void *arena, ImmutableSpatialUInt32 arenaCapacity,
	const ImmutableSpatialQuery *queries, ImmutableSpatialUInt32 queryCount,
	ImmutableSpatialArenaGenerationResolver arenaResolver,
	ImmutableSpatialObjectGenerationResolver objectResolver,
	void *generationContext, const ImmutableSpatialBatchScratch &scratch,
	ImmutableSpatialResult *output, ImmutableSpatialUInt32 outputCapacity,
	ImmutableSpatialResultSpan *outputSpans,
	ImmutableSpatialUInt32 outputSpanCapacity,
	ImmutableSpatialUInt32 *outputCount,
	const ImmutableSpatialJobSystemOptions &options,
	ImmutableSpatialJobSystemMetrics *jobMetrics,
	ImmutableSpatialExecutionMetrics *executionMetrics,
	ImmutableSpatialStatus *kernelStatus)
{
	ImmutableSpatialJobSystemMetrics localMetrics;
	if (jobMetrics == 0)
		jobMetrics = &localMetrics;
	*jobMetrics = ImmutableSpatialJobSystemMetrics();
	if (kernelStatus != 0)
		*kernelStatus = IMMUTABLE_SPATIAL_INVALID_ARGUMENT;

	JobSystem &jobs = JobSystem::instance();
	if (!jobs.isRunning() || jobs.isWorkerThread() ||
		!jobs.isCurrentThread(JOB_OWNER_GAME) || jobs.workerCount() <= 1)
		return IMMUTABLE_SPATIAL_JOB_SYSTEM_INELIGIBLE;
	SpatialDispatchContext dispatch;
	dispatch.options = &options;
	dispatch.metrics = jobMetrics;
	dispatch.observedPhysicalWorkerCapacity = jobs.workerCount();
	dispatch.observedPhysicalWorkerIndices = new (std::nothrow) unsigned[
		dispatch.observedPhysicalWorkerCapacity];
	if (dispatch.observedPhysicalWorkerIndices == 0)
		return IMMUTABLE_SPATIAL_JOB_SYSTEM_FAILED;
	ImmutableSpatialExecutionOptions executionOptions;
	executionOptions.workerCount = jobs.workerCount();
	executionOptions.dispatch = spatialDispatch;
	executionOptions.dispatchContext = &dispatch;
	executionOptions.isCancelled = spatialCancelled;
	executionOptions.cancellationContext = &dispatch;
	executionOptions.resolveArenaGeneration = arenaResolver;
	executionOptions.resolveObjectGeneration = objectResolver;
	executionOptions.generationContext = generationContext;

	const ImmutableSpatialStatus status = ExecuteImmutableSpatialQueryBatch(
		arena, arenaCapacity, queries, queryCount, executionOptions, scratch,
		output, outputCapacity, outputSpans, outputSpanCapacity, outputCount,
		executionMetrics);
	if (kernelStatus != 0)
		*kernelStatus = status;
	ImmutableSpatialJobSystemResult result = IMMUTABLE_SPATIAL_JOB_SYSTEM_FAILED;
	if (status == IMMUTABLE_SPATIAL_SUCCESS &&
		jobMetrics->submittedJobs == jobMetrics->completedJobs &&
		jobMetrics->completedJobs == jobMetrics->physicalWorkerJobs &&
		jobMetrics->ownerHelpedJobs == 0)
		result = IMMUTABLE_SPATIAL_JOB_SYSTEM_SUCCESS;
	else if (status == IMMUTABLE_SPATIAL_CANCELLED || dispatch.cancelled.load(
		std::memory_order_acquire))
		result = IMMUTABLE_SPATIAL_JOB_SYSTEM_CANCELLED;
	delete[] dispatch.observedPhysicalWorkerIndices;
	return result;
}

void ResetImmutableSpatialRuntimeMetrics()
{
	addMetric(s_resetEpoch, 1);
	resetMetric(s_capturedArenas);
	resetMetric(s_captureFailures);
	resetMetric(s_successfulCollections);
	resetMetric(s_successfulCollectionQueries);
	resetMetric(s_successfulCollectionRanges);
	resetMetric(s_multiRangeCollections);
	resetMetric(s_collectionSubmittedJobs);
	resetMetric(s_collectionCompletedJobs);
	resetMetric(s_collectionPhysicalWorkerJobs);
	resetMetric(s_collectionOwnerHelpedJobs);
	resetMetric(s_collectionPhysicalWorkerMask);
	resetMetric(s_collectionPhysicalWorkerMaskIncomplete);
	resetMetric(s_maximumCollectionQueries);
	resetMetric(s_maximumCollectionRanges);
	resetMetric(s_maximumCollectionDistinctPhysicalWorkers);
	resetMetric(s_maximumCollectionPeakConcurrentPhysicalWorkers);
	for (unsigned index = 0; index != IMMUTABLE_SPATIAL_CONSUMER_COUNT; ++index)
		resetConsumer(s_consumers[index]);
}

ImmutableSpatialRuntimeMetrics GetImmutableSpatialRuntimeMetrics()
{
	ImmutableSpatialRuntimeMetrics result;
	result.resetEpoch = loadMetric(s_resetEpoch);
	result.capturedArenas = loadMetric(s_capturedArenas);
	result.captureFailures = loadMetric(s_captureFailures);
	result.successfulCollections = loadMetric(s_successfulCollections);
	result.successfulCollectionQueries = loadMetric(
		s_successfulCollectionQueries);
	result.successfulCollectionRanges = loadMetric(
		s_successfulCollectionRanges);
	result.multiRangeCollections = loadMetric(s_multiRangeCollections);
	result.collectionSubmittedJobs = loadMetric(s_collectionSubmittedJobs);
	result.collectionCompletedJobs = loadMetric(s_collectionCompletedJobs);
	result.collectionPhysicalWorkerJobs = loadMetric(
		s_collectionPhysicalWorkerJobs);
	result.collectionOwnerHelpedJobs = loadMetric(
		s_collectionOwnerHelpedJobs);
	result.collectionPhysicalWorkerMask = loadMetric(
		s_collectionPhysicalWorkerMask);
	result.collectionPhysicalWorkerMaskComplete =
		loadMetric(s_collectionPhysicalWorkerMaskIncomplete) == 0;
	result.maximumCollectionQueries = loadMetric(s_maximumCollectionQueries);
	result.maximumCollectionRanges = loadMetric(s_maximumCollectionRanges);
	result.maximumCollectionDistinctPhysicalWorkers = loadMetric(
		s_maximumCollectionDistinctPhysicalWorkers);
	result.maximumCollectionPeakConcurrentPhysicalWorkers = loadMetric(
		s_maximumCollectionPeakConcurrentPhysicalWorkers);
	result.healing = loadConsumer(s_consumers[
		IMMUTABLE_SPATIAL_CONSUMER_HEALING]);
	result.pointDefenseLaser = loadConsumer(s_consumers[
		IMMUTABLE_SPATIAL_CONSUMER_POINT_DEFENSE_LASER]);
	return result;
}

void RecordImmutableSpatialArenaCapture(bool succeeded)
{
	addMetric(succeeded ? s_capturedArenas : s_captureFailures, 1);
}

void RecordImmutableSpatialSuccessfulCollection(unsigned queryCount,
	unsigned rangeCount, const ImmutableSpatialJobSystemMetrics &metrics)
{
	if (queryCount < 2 || rangeCount < 2 ||
		metrics.submittedJobs != metrics.completedJobs ||
		metrics.completedJobs != metrics.physicalWorkerJobs ||
		metrics.ownerHelpedJobs != 0 || metrics.distinctPhysicalWorkers == 0 ||
		metrics.peakConcurrentPhysicalWorkers == 0 ||
		metrics.peakConcurrentPhysicalWorkers >
			metrics.distinctPhysicalWorkers ||
		(metrics.physicalWorkerMaskComplete &&
			(metrics.physicalWorkerMask == 0 ||
				countMetricBits(metrics.physicalWorkerMask) !=
					metrics.distinctPhysicalWorkers)))
		return;
	addMetric(s_successfulCollections, 1);
	addMetric(s_successfulCollectionQueries, queryCount);
	addMetric(s_successfulCollectionRanges, rangeCount);
	addMetric(s_multiRangeCollections, 1);
	addMetric(s_collectionSubmittedJobs, metrics.submittedJobs);
	addMetric(s_collectionCompletedJobs, metrics.completedJobs);
	addMetric(s_collectionPhysicalWorkerJobs, metrics.physicalWorkerJobs);
	addMetric(s_collectionOwnerHelpedJobs, metrics.ownerHelpedJobs);
	orMetric(s_collectionPhysicalWorkerMask, metrics.physicalWorkerMask);
	if (!metrics.physicalWorkerMaskComplete)
		addMetric(s_collectionPhysicalWorkerMaskIncomplete, 1);
	maximizeMetric(s_maximumCollectionQueries, queryCount);
	maximizeMetric(s_maximumCollectionRanges, rangeCount);
	maximizeMetric(s_maximumCollectionDistinctPhysicalWorkers,
		metrics.distinctPhysicalWorkers);
	maximizeMetric(s_maximumCollectionPeakConcurrentPhysicalWorkers,
		metrics.peakConcurrentPhysicalWorkers);
}

void RecordImmutableSpatialEligibleQuery(ImmutableSpatialConsumer consumer)
{
	SpatialConsumerMetricAtomics *metrics = consumerAtomics(consumer);
	if (metrics != 0)
		addMetric(metrics->eligibleQueries, 1);
}

void RecordImmutableSpatialAuthoritativeQuery(ImmutableSpatialConsumer consumer,
	unsigned candidateCount, const ImmutableSpatialJobSystemMetrics &jobMetrics)
{
	SpatialConsumerMetricAtomics *metrics = consumerAtomics(consumer);
	if (metrics == 0)
		return;
	addMetric(metrics->authoritativeQueries, 1);
	addMetric(metrics->authoritativeCandidates, candidateCount);
	recordJobMetrics(*metrics, jobMetrics);
}

void RecordImmutableSpatialShadowQuery(ImmutableSpatialConsumer consumer,
	bool matched, const ImmutableSpatialJobSystemMetrics &jobMetrics)
{
	SpatialConsumerMetricAtomics *metrics = consumerAtomics(consumer);
	if (metrics == 0)
		return;
	addMetric(metrics->shadowQueries, 1);
	addMetric(matched ? metrics->shadowMatches : metrics->shadowMismatches, 1);
	recordJobMetrics(*metrics, jobMetrics);
}

void RecordImmutableSpatialExpectedFallback(ImmutableSpatialConsumer consumer)
{
	SpatialConsumerMetricAtomics *metrics = consumerAtomics(consumer);
	if (metrics != 0)
		addMetric(metrics->expectedFallbacks, 1);
}

void RecordImmutableSpatialUnexpectedFallback(ImmutableSpatialConsumer consumer,
	bool stale, bool validationFailure,
	const ImmutableSpatialJobSystemMetrics *jobMetrics)
{
	SpatialConsumerMetricAtomics *metrics = consumerAtomics(consumer);
	if (metrics == 0)
		return;
	addMetric(metrics->unexpectedFallbacks, 1);
	if (stale)
		addMetric(metrics->staleRejections, 1);
	if (validationFailure)
		addMetric(metrics->validationFailures, 1);
	if (jobMetrics != 0)
		recordJobMetrics(*metrics, *jobMetrics);
}

void RecordImmutableSpatialCircuitBreakerTrip(ImmutableSpatialConsumer consumer)
{
	SpatialConsumerMetricAtomics *metrics = consumerAtomics(consumer);
	if (metrics != 0)
		addMetric(metrics->circuitBreakerTrips, 1);
}
}
