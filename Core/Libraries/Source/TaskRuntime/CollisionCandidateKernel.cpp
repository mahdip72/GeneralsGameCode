/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2025 Electronic Arts Inc.
** SPDX-License-Identifier: GPL-3.0-or-later
*/
#include "Lib/CollisionCandidateKernel.h"
#include "Lib/FrameTimingDiagnostics.h"
#include <Utility/stdint_adapter.h>

#include <algorithm>
#include <new>
#if defined(_WIN64)
#include <memory>
#endif
#include <string.h>

#if !defined(_MSC_VER) || _MSC_VER >= 1300
#include <atomic>
#endif

namespace rts
{
CollisionCandidateOptions::CollisionCandidateOptions()
	: parallel(false),
	  minimumGrain(COLLISION_CANDIDATE_DEFAULT_MINIMUM_GRAIN),
	  order(COLLISION_CANDIDATE_REVERSE_DISCOVERY),
	  cancellationGroup(0)
	#if defined(_WIN64)
	  , performanceLedger(0), performanceBatch(),
	performanceReferenceLedger(0), performanceReferenceAttempt(), performanceReferenceDecisionOrdinal(0), testHooks(0), performanceReferenceBatch(0),
	  performanceReferenceOutput(0), performanceReferenceOutputCapacity(0)
#endif
{
}

CollisionCandidateMetrics::CollisionCandidateMetrics()
	: preparedPairs(0), uniqueCandidates(0), submittedJobs(0),
	  completedJobs(0), serialFallbacks(0), localSortRuns(0),
	  locallyUniqueCandidates(0), ownerMergeComparisons(0),
	  maximumRangeInputs(0), physicalWorkerJobs(0), ownerHelpedJobs(0),
	  physicalWorkerMask(0), distinctPhysicalWorkers(0),
	  physicalWorkerMaskComplete(true), peakConcurrentPhysicalWorkers(0)

{
}

CollisionCandidateRuntimeMetrics::CollisionCandidateRuntimeMetrics()
	// Owner-thread counters use JobMetricCounter so long validation sessions do
	// not wrap candidate volume at the 32-bit boundary.
	: resetEpoch(0), authoritativeCommits(0), shadowExecutions(0), shadowMismatches(0),
	  ownerFallbacks(0), unexpectedFallbacks(0), ineligibleSlices(0),
	  staleRejections(0), committedCandidates(0), shadowComparedCandidates(0),
	  preparedPairs(0), uniqueCandidates(0), submittedJobs(0),
	  completedJobs(0), localSortRuns(0), locallyUniqueCandidates(0),
	  ownerMergeComparisons(0), maximumRangeInputs(0), physicalWorkerJobs(0),
	  ownerHelpedJobs(0), physicalWorkerMask(0), distinctPhysicalWorkers(0),
	  physicalWorkerMaskComplete(true), maximumPeakConcurrentPhysicalWorkers(0)
{
}

namespace
{
#if defined(_MSC_VER) && _MSC_VER < 1300
typedef unsigned CollisionJobAtomicUnsigned;
inline unsigned incrementJobCounter(CollisionJobAtomicUnsigned &value)
{
	return ++value;
}
inline void decrementJobCounter(CollisionJobAtomicUnsigned &value)
{
	--value;
}
inline unsigned loadJobCounter(const CollisionJobAtomicUnsigned &value)
{
	return value;
}
inline void maximizeJobCounter(CollisionJobAtomicUnsigned &value,
	unsigned candidate)
{
	if (candidate > value)
		value = candidate;
}
	#else
typedef std::atomic<unsigned> CollisionJobAtomicUnsigned;
inline unsigned incrementJobCounter(CollisionJobAtomicUnsigned &value)
{
	return value.fetch_add(1, std::memory_order_acq_rel) + 1;
}
inline void decrementJobCounter(CollisionJobAtomicUnsigned &value)
{
	value.fetch_sub(1, std::memory_order_acq_rel);
}
inline unsigned loadJobCounter(const CollisionJobAtomicUnsigned &value)
{
	return value.load(std::memory_order_relaxed);
}
inline void maximizeJobCounter(CollisionJobAtomicUnsigned &value,
	unsigned candidate)
{
	unsigned observed = value.load(std::memory_order_relaxed);
	while (observed < candidate && !value.compare_exchange_weak(observed,
		candidate, std::memory_order_relaxed, std::memory_order_relaxed)) {}
}
	#endif

class CollisionPhysicalExecutionScope
{
public:
	CollisionPhysicalExecutionScope(bool physicalWorker,
		CollisionJobAtomicUnsigned *active,
		CollisionJobAtomicUnsigned *peak)
		: m_active(physicalWorker ? active : 0)
	{
		if (m_active != 0)
		{
			const unsigned current = incrementJobCounter(*m_active);
			maximizeJobCounter(*peak, current);
		}
	}

	~CollisionPhysicalExecutionScope()
	{
		if (m_active != 0)
			decrementJobCounter(*m_active);
	}

private:
	CollisionJobAtomicUnsigned *m_active;
};

#if defined(_MSC_VER) && _MSC_VER < 1300
typedef JobMetricCounter CollisionMetricAtomic;
inline JobMetricCounter loadMetric(const CollisionMetricAtomic &value)
{
	return value;
}
inline void resetMetric(CollisionMetricAtomic &value)
{
	value = 0;
}
inline void addMetric(CollisionMetricAtomic &value, JobMetricCounter amount)
{
	value += amount;
}
inline void orMetric(CollisionMetricAtomic &value, JobMetricCounter bits)
{
	value |= bits;
}
inline void maximizeMetric(CollisionMetricAtomic &value,
	JobMetricCounter candidate)
{
	if (candidate > value)
		value = candidate;
}
#else
typedef std::atomic<JobMetricCounter> CollisionMetricAtomic;
inline JobMetricCounter loadMetric(const CollisionMetricAtomic &value)
{
	return value.load(std::memory_order_relaxed);
}
inline void resetMetric(CollisionMetricAtomic &value)
{
	value.store(0, std::memory_order_relaxed);
}
inline void addMetric(CollisionMetricAtomic &value, JobMetricCounter amount)
{
	value.fetch_add(amount, std::memory_order_relaxed);
}
inline void orMetric(CollisionMetricAtomic &value, JobMetricCounter bits)
{
	value.fetch_or(bits, std::memory_order_relaxed);
}
inline void maximizeMetric(CollisionMetricAtomic &value,
	JobMetricCounter candidate)
{
	JobMetricCounter observed = value.load(std::memory_order_relaxed);
	while (observed < candidate && !value.compare_exchange_weak(observed,
		candidate, std::memory_order_relaxed, std::memory_order_relaxed)) {}
}
#endif

struct CollisionCandidateAddressSpan
{
	uintptr_t begin;
	uintptr_t end;
};

bool makeCollisionCandidateAddressSpan(const void *data, unsigned count,
	unsigned itemSize, CollisionCandidateAddressSpan &span)
{
	span.begin = 0;
	span.end = 0;
	if (count == 0)
		return true;
	if (data == 0 || itemSize == 0 ||
		static_cast<uintptr_t>(count) >
			UINTPTR_MAX / static_cast<uintptr_t>(itemSize))
	{
		return false;
	}
	const uintptr_t byteCount = static_cast<uintptr_t>(count) *
		static_cast<uintptr_t>(itemSize);
	const uintptr_t begin = reinterpret_cast<uintptr_t>(data);
	if (begin > UINTPTR_MAX - byteCount)
		return false;
	span.begin = begin;
	span.end = begin + byteCount;
	return true;
}

bool collisionCandidateAddressSpansOverlap(
	const CollisionCandidateAddressSpan &left,
	const CollisionCandidateAddressSpan &right)
{
	return left.begin != left.end && right.begin != right.end &&
		left.begin < right.end && right.begin < left.end;
}

bool collisionCandidateAddressSpansAreDisjoint(
	const CollisionCandidateAddressSpan *spans, unsigned count)
{
	for (unsigned left = 0; left != count; ++left)
	{
		for (unsigned right = left + 1; right != count; ++right)
		{
			if (collisionCandidateAddressSpansOverlap(spans[left], spans[right]))
				return false;
		}
	}
	return true;
}

unsigned collisionAdmissionHash(unsigned value)
{
	// Fixed unsigned mixing makes reservoir replacement deterministic without
	// consuming gameplay RNG state.
	value ^= value >> 16;
	value *= 0x7feb352du;
	value ^= value >> 15;
	value *= 0x846ca68bu;
	value ^= value >> 16;
	return value;
}
}

CollisionAdmissionSampler::CollisionAdmissionSampler()
	: m_encounterCount(0), m_sampleCount(0)
{
	memset(m_sampleIDs, 0, sizeof(m_sampleIDs));
}

void CollisionAdmissionSampler::observe(unsigned objectID)
{
	if (objectID == 0)
		return;
	++m_encounterCount;
	if (m_sampleCount < COLLISION_ADMISSION_SAMPLE_CAPACITY)
	{
		m_sampleIDs[m_sampleCount++] = objectID;
		return;
	}
	const unsigned replacement = collisionAdmissionHash(m_encounterCount) %
		m_encounterCount;
	if (replacement < COLLISION_ADMISSION_SAMPLE_CAPACITY)
		m_sampleIDs[replacement] = objectID;
}

unsigned CollisionAdmissionSampler::encounterCount() const
{
	return m_encounterCount;
}

unsigned CollisionAdmissionSampler::sampleCount() const
{
	return m_sampleCount;
}

unsigned CollisionAdmissionSampler::uniqueSampleCount() const
{
	unsigned uniqueCount = 0;
	for (unsigned sampleIndex = 0; sampleIndex != m_sampleCount; ++sampleIndex)
	{
		bool firstSample = true;
		for (unsigned previousIndex = 0; previousIndex != sampleIndex;
			++previousIndex)
		{
			if (m_sampleIDs[previousIndex] == m_sampleIDs[sampleIndex])
			{
				firstSample = false;
				break;
			}
		}
		if (firstSample)
			++uniqueCount;
	}
	return uniqueCount;
}

bool CollisionAdmissionSampler::hasUsefulSpread() const
{
	return m_sampleCount == COLLISION_ADMISSION_SAMPLE_CAPACITY &&
		uniqueSampleCount() * 4 >= m_sampleCount * 3;
}

bool MakeCollisionCandidateKey(unsigned firstID, unsigned secondID,
	CollisionCandidateKey &key)
{
	key.lowID = 0;
	key.highID = 0;
	if (firstID == 0 || secondID == 0 || firstID == secondID)
		return false;
	if (firstID < secondID)
	{
		key.lowID = firstID;
		key.highID = secondID;
	}
	else
	{
		key.lowID = secondID;
		key.highID = firstID;
	}
	return true;
}

unsigned HashCollisionCandidateKey(const CollisionCandidateKey &key)
{
	// Unsigned arithmetic is intentional: wraparound and shifts are defined.
	unsigned hash = key.lowID * 0x9e3779b1u;
	hash ^= key.highID + 0x85ebca6bu + (hash << 6) + (hash >> 2);
	return hash;
}

namespace
{
bool cancelled(const CollisionCandidateOptions &options)
{
	return options.cancellationGroup != 0 &&
		options.cancellationGroup->wasCancelled();
}

#if defined(_WIN64)
struct CollisionSourceRange
{
	CollisionSourceRange() : plan(), replay(false) {}
	performance::KernelPerformanceRangePlan plan;
	performance::KernelPerformanceCheckpointProbe checkpoint;
	bool replay;
};

class CollisionSourceBodyScope
{
public:
	CollisionSourceBodyScope(CollisionSourceRange *range, const unsigned &units,
		const bool &completed) : m_range(range), m_units(units), m_completed(completed)
	{ if (m_range != 0 && !m_range->replay) m_range->checkpoint.beginRecord(); }
	~CollisionSourceBodyScope()
	{
		if (m_range == 0) return;
		const performance::KernelPerformanceCheckpoint at = {5, m_units, m_range->plan.end};
		m_range->checkpoint.finish(at, m_units, m_completed ? performance::KERNEL_RANGE_COMPLETED :
			m_range->checkpoint.snapshot().firstTruePoll != 0 ? performance::KERNEL_RANGE_CANCELLED :
			performance::KERNEL_RANGE_FAILED);
	}
private:
	CollisionSourceRange *m_range;
	const unsigned &m_units;
	const bool &m_completed;
};

bool collisionSourceCheckpoint(CollisionSourceRange *range,
	unsigned site, unsigned units, bool actual)
{
	if (range == 0) return actual;
	const performance::KernelPerformanceCheckpoint at = {site, units, range->plan.end};
	return range->checkpoint.cancelled(at, actual);
}

struct GenericCollisionReferenceInput
{
	const CollisionCandidateInput *inputs;
	unsigned count;
	CollisionCandidateOrder order;
};

bool writeGenericCollisionReferenceInput(performance::KernelPerformanceCanonicalWriter &writer,
	const void *context)
{
	const GenericCollisionReferenceInput &input = *static_cast<const GenericCollisionReferenceInput *>(context);
	if (input.inputs == 0 || !writer.sequence(1, input.count)) return false;
	for (unsigned i = 0; i != input.count; ++i)
	{
		const CollisionCandidateInput &value = input.inputs[i];
		if (!writer.u32(2, value.firstID) || !writer.u32(3, value.secondID) ||
			!writer.u32(4, value.firstGeneration) || !writer.u32(5, value.secondGeneration) ||
			!writer.u32(6, value.discoveryOrder)) return false;
	}
	return writer.u32(7, static_cast<unsigned>(input.order));
}

class CollisionSourceAttempt
{
public:
	CollisionSourceAttempt(const CollisionCandidateOptions &options, unsigned count,
		unsigned grain, unsigned rangeCount, unsigned bodyKind, JobSystem &jobs,
		performance::KernelPerformanceCanonicalCallback writeInput, const void *input) :
		m_options(options), m_grain(grain), m_bodyKind(bodyKind), m_workers(jobs.workerCount()),
		m_pending(jobs.pendingOwnerCompletionCount()), m_outstanding(jobs.outstandingJobCount()),
		m_ranges(0), m_enabled(false), m_released(false)
	{
		if (options.performanceReferenceLedger == 0 || !options.performanceReferenceAttempt.valid()) return;
		const performance::KernelPerformanceReferenceMode mode = options.performanceReferenceLedger->mode();
		if (mode != performance::KERNEL_REFERENCE_THROUGHPUT_BINDING &&
			mode != performance::KERNEL_REFERENCE_SERIAL_ORACLE) return;
		m_ranges = new (std::nothrow) CollisionSourceRange[rangeCount];
		if (m_ranges == 0) return;
		m_enabled = options.performanceReferenceLedger->bindCapturedInput(
			options.performanceReferenceAttempt, 1, count, writeInput, input);
		performance::KernelPerformanceCanonicalWriter facts;
		if (m_enabled && facts.begin(1) && facts.u32(1, count) && facts.u32(2, grain) &&
			facts.u32(3, bodyKind) && facts.u32(4, static_cast<unsigned>(options.order)))
			m_facts = facts.finish();
	}
	~CollisionSourceAttempt()
	{
		release(0, false, false);
		delete[] m_ranges;
	}
	CollisionSourceRange *plan(unsigned ordinal, unsigned begin, unsigned end)
	{
		if (!m_enabled) return 0;
		m_ranges[ordinal].plan = {1, ordinal, m_bodyKind, begin, end, end - begin};
		return m_ranges + ordinal;
	}
	void release(unsigned submitted, bool published, bool cancelled)
	{
		if (!m_enabled || m_released) return;
		m_released = true;
		performance::KernelPerformanceReferenceLedger &ledger = *m_options.performanceReferenceLedger;
		const performance::KernelPerformanceAttempt attempt = m_options.performanceReferenceAttempt;
		performance::KernelPerformanceAttemptDecision decision = {};
		decision.decisionOrdinal = m_options.performanceReferenceDecisionOrdinal;
		decision.site = 1; decision.reasonSchema = 1; decision.reason = submitted == 0 ? 2 : cancelled ? 3 : 1;
		decision.deterministicEligible = true; decision.deterministicFacts = m_facts;
		decision.admission = submitted != 0 ? performance::KERNEL_ADMISSION_ACCEPTED : performance::KERNEL_ADMISSION_REFUSED;
		decision.sourceConfiguredWorkers = m_workers; decision.dynamicFactsKnownMask = 3;
		decision.pendingJobs = m_pending; decision.outstandingJobs = m_outstanding;
		ledger.observeDecision(attempt, decision);
		if (submitted == 0) return;
		performance::KernelPerformanceDispatchPlan dispatch = {1, 1, 1, submitted,
			0, m_grain, COLLISION_CANDIDATE_MAXIMUM_INPUTS};
		for (unsigned i = 0; i != submitted; ++i) dispatch.operationCount += m_ranges[i].plan.operationCount;
		ledger.observeDispatch(attempt, dispatch);
		for (unsigned i = 0; i != submitted; ++i) ledger.observeRangePlan(attempt, m_ranges[i].plan);
		for (unsigned i = 0; i != submitted; ++i)
		{
			performance::KernelPerformanceRangeProgress progress = {};
			progress.checkpoint = m_ranges[i].checkpoint.snapshot();
			progress.publication = !progress.checkpoint.entered ? performance::KERNEL_PUBLICATION_NOT_APPLICABLE :
				published ? performance::KERNEL_PUBLICATION_PUBLISHED : cancelled ||
				progress.checkpoint.terminal == performance::KERNEL_RANGE_CANCELLED ?
				performance::KERNEL_PUBLICATION_DISCARDED_AFTER_CANCEL : performance::KERNEL_PUBLICATION_REJECTED;
			ledger.observeReleasedRange(attempt, m_ranges[i].plan, progress);
		}
	}
	void validated(CollisionCandidate *output, unsigned count)
	{
		if (!m_enabled || m_options.performanceReferenceBatch == 0) return;
		const PartitionCollisionReferenceOutput production = {output, 0, count, count};
		*m_options.performanceReferenceBatch = m_options.performanceReferenceLedger->observeValidatedAttempt(
			m_options.performanceReferenceAttempt, WritePartitionCollisionReferenceOutput, &production);
	}
private:
	const CollisionCandidateOptions &m_options;
	unsigned m_grain, m_bodyKind, m_workers;
	JobMetricCounter m_pending, m_outstanding;
	CollisionSourceRange *m_ranges;
	bool m_enabled, m_released;
	performance::KernelPerformanceDigest m_facts;
};

void observeCollisionTest(const CollisionCandidateTestHooks *hooks,
	CollisionCandidateTestEvent event, unsigned rangeIndex, unsigned begin,
	unsigned end, unsigned workUnits = 0, bool completed = false)
{
	if (hooks != 0 && hooks->observe != 0)
		hooks->observe(hooks->context, event, rangeIndex, begin, end, workUnits, completed, 0);
}

bool collisionTestCheckpoint(const CollisionCandidateTestHooks *hooks,
	unsigned rangeIndex, CollisionCandidateTestCheckpoint site,
	unsigned workUnits, bool actual)
{
	return hooks != 0 && hooks->checkpoint != 0 ?
		hooks->checkpoint(hooks->context, rangeIndex, site, workUnits, actual) : actual;
}

void waitForCollisionTestWorkers(const CollisionCandidateOptions &options,
	JobSystem &jobs, const JobGroup &group)
{
	if (options.testHooks != 0 && options.testHooks->physicalWaitMilliseconds != 0)
	{
		const unsigned milliseconds = options.testHooks->physicalWaitMilliseconds < 1000 ?
			options.testHooks->physicalWaitMilliseconds : 1000;
		if (!jobs.waitWithoutOwnerHelp(group, milliseconds)) jobs.cancel(group);
	}
}

void observeCollisionTestRelease(const CollisionCandidateOptions &options,
	unsigned count, unsigned rangeCount)
{
	if (options.testHooks == 0) return;
	for (unsigned released = 0; released != rangeCount; ++released)
	{
		JobRange range;
		JobSystem::rangeForIndex(count, rangeCount, released, range);
		observeCollisionTest(options.testHooks, COLLISION_CANDIDATE_TEST_RANGE_RELEASED,
			released, range.begin, range.end);
	}
}
#endif

void normalizeRange(const CollisionCandidateInput *inputs,
	CollisionCandidate *scratch, unsigned begin, unsigned end,
	JobContext *context
#if defined(_WIN64)
	, const CollisionCandidateTestHooks *testHooks = 0, unsigned rangeIndex = 0,
	unsigned *completedUnits = 0, bool *testCancelled = 0, CollisionSourceRange *sourceRange = 0
#endif
	)
{
	for (unsigned index = begin; index != end; ++index)
	{
		if ((index - begin) % 256 == 0 && (context != 0
#if defined(_WIN64)
			|| sourceRange != 0
#endif
			))
		{
			bool cancelled = context != 0 && context->isCancellationRequested();
#if defined(_WIN64)
			cancelled = collisionTestCheckpoint(testHooks, rangeIndex,
				COLLISION_CANDIDATE_TEST_CHECKPOINT_BLOCK, index - begin, cancelled);
			cancelled = collisionSourceCheckpoint(sourceRange, 2, index - begin, cancelled);
			if (cancelled && testCancelled != 0) *testCancelled = true;
#endif
			if (cancelled) return;
		}
#if defined(_WIN64)
		observeCollisionTest(testHooks, COLLISION_CANDIDATE_TEST_GENERIC_NORMALIZED,
			rangeIndex, begin, end, index - begin);
#endif
		CollisionCandidate &candidate = scratch[index];
		candidate.firstID = inputs[index].firstID;
		candidate.secondID = inputs[index].secondID;
		candidate.firstGeneration = inputs[index].firstGeneration;
		candidate.secondGeneration = inputs[index].secondGeneration;
		candidate.discoveryOrder = inputs[index].discoveryOrder;
		MakeCollisionCandidateKey(candidate.firstID, candidate.secondID,
			candidate.key);
#if defined(_WIN64)
		if (completedUnits != 0) *completedUnits = index - begin + 1;
#endif
	}
}

struct KeyOrder
{
	bool operator()(const CollisionCandidate &left,
		const CollisionCandidate &right) const
	{
		if (left.key.lowID != right.key.lowID)
			return left.key.lowID < right.key.lowID;
		if (left.key.highID != right.key.highID)
			return left.key.highID < right.key.highID;
		if (left.discoveryOrder != right.discoveryOrder)
			return left.discoveryOrder < right.discoveryOrder;
		if (left.firstID != right.firstID)
			return left.firstID < right.firstID;
		if (left.secondID != right.secondID)
			return left.secondID < right.secondID;
		if (left.firstGeneration != right.firstGeneration)
			return left.firstGeneration < right.firstGeneration;
		return left.secondGeneration < right.secondGeneration;
	}
};

struct ReverseDiscoveryOrder
{
	bool operator()(const CollisionCandidate &left,
		const CollisionCandidate &right) const
	{
		if (left.discoveryOrder != right.discoveryOrder)
			return left.discoveryOrder > right.discoveryOrder;
		return KeyOrder()(left, right);
	}
};

bool sameKey(const CollisionCandidate &left,
	const CollisionCandidate &right)
{
	return left.key.lowID == right.key.lowID &&
		left.key.highID == right.key.highID;
}

struct CollisionCandidateRangeState
{
	unsigned begin;
	unsigned end;
	unsigned uniqueCount;
	unsigned cursor;
	unsigned heapRange;
	bool completed;
	bool physicalWorker;
	unsigned physicalWorkerIndex;
#if defined(_WIN64)
	CollisionSourceRange *sourceRange;
#endif
};

#if defined(_WIN64)
class CollisionTestBodyScope
{
public:
	CollisionTestBodyScope(const CollisionCandidateTestHooks *hooks, unsigned rangeIndex,
		const CollisionCandidateRangeState &range, const unsigned &units)
		: m_hooks(hooks), m_rangeIndex(rangeIndex), m_range(range), m_units(units)
	{ observeCollisionTest(m_hooks, COLLISION_CANDIDATE_TEST_RANGE_ENTERED, m_rangeIndex, m_range.begin, m_range.end); }
	~CollisionTestBodyScope()
	{ observeCollisionTest(m_hooks, COLLISION_CANDIDATE_TEST_RANGE_FINISHED, m_rangeIndex, m_range.begin, m_range.end, m_units, m_range.completed); }
private:
	const CollisionCandidateTestHooks *m_hooks;
	unsigned m_rangeIndex;
	const CollisionCandidateRangeState &m_range;
	const unsigned &m_units;
};
#endif

unsigned sortAndDeduplicateRange(CollisionCandidate *scratch,
	unsigned begin, unsigned end)
{
	unsigned validCount = 0;
	for (unsigned index = begin; index != end; ++index)
	{
		if (scratch[index].key.lowID != 0)
			scratch[begin + validCount++] = scratch[index];
	}
	std::sort(scratch + begin, scratch + begin + validCount, KeyOrder());

	unsigned uniqueCount = 0;
	for (unsigned uniqueIndex = 0; uniqueIndex != validCount; ++uniqueIndex)
	{
		const CollisionCandidate &candidate = scratch[begin + uniqueIndex];
		if (uniqueCount == 0 || !sameKey(scratch[begin + uniqueCount - 1],
			candidate))
			scratch[begin + uniqueCount++] = candidate;
	}
	return uniqueCount;
}

void collectRangeMetrics(CollisionCandidateRangeState *ranges,
	unsigned rangeCount, CollisionCandidateMetrics *metrics)
{
	for (unsigned rangeIndex = 0; rangeIndex != rangeCount; ++rangeIndex)
	{
		CollisionCandidateRangeState &range = ranges[rangeIndex];
		if (!range.completed)
			continue;
		++metrics->completedJobs;
		++metrics->localSortRuns;
		metrics->locallyUniqueCandidates += range.uniqueCount;
		const unsigned rangeInputs = range.end - range.begin;
		if (rangeInputs > metrics->maximumRangeInputs)
			metrics->maximumRangeInputs = rangeInputs;
		if (range.physicalWorker)
		{
			++metrics->physicalWorkerJobs;
			bool firstRangeForWorker = true;
			for (unsigned prior = 0; prior != rangeIndex; ++prior)
			{
				if (ranges[prior].completed && ranges[prior].physicalWorker &&
					ranges[prior].physicalWorkerIndex == range.physicalWorkerIndex)
				{
					firstRangeForWorker = false;
					break;
				}
			}
			if (firstRangeForWorker)
				++metrics->distinctPhysicalWorkers;
			if (range.physicalWorkerIndex <
				sizeof(JobMetricCounter) * 8)
			{
				metrics->physicalWorkerMask |=
					static_cast<JobMetricCounter>(1) <<
					range.physicalWorkerIndex;
			}
			else
			{
				metrics->physicalWorkerMaskComplete = false;
			}
		}
		else
		{
			++metrics->ownerHelpedJobs;
		}
	}
}

unsigned mergeSortedRanges(CollisionCandidate *scratch,
	CollisionCandidateRangeState *ranges, unsigned rangeCount,
	CollisionCandidate *output, CollisionCandidateMetrics *metrics)
{
	for (unsigned rangeIndex = 0; rangeIndex != rangeCount; ++rangeIndex)
		ranges[rangeIndex].cursor = 0;

	// heapRange is independent owner-only storage embedded in the state array;
	// it avoids a second allocation while retaining every range's worker result.
	unsigned heapSize = 0;
	for (unsigned heapRangeIndex = 0; heapRangeIndex != rangeCount;
		++heapRangeIndex)
	{
		if (ranges[heapRangeIndex].uniqueCount == 0)
			continue;
		unsigned heapPosition = heapSize++;
		ranges[heapPosition].heapRange = heapRangeIndex;
		while (heapPosition != 0)
		{
			const unsigned parentPosition = (heapPosition - 1) / 2;
			const unsigned childRange = ranges[heapPosition].heapRange;
			const unsigned parentRange = ranges[parentPosition].heapRange;
			const CollisionCandidateRangeState &child = ranges[childRange];
			const CollisionCandidateRangeState &parent = ranges[parentRange];
			const CollisionCandidate &childCandidate =
				scratch[child.begin + child.cursor];
			const CollisionCandidate &parentCandidate =
				scratch[parent.begin + parent.cursor];
			++metrics->ownerMergeComparisons;
			const bool childFirst = KeyOrder()(childCandidate,
				parentCandidate) ||
				(!KeyOrder()(parentCandidate, childCandidate) &&
				 childRange < parentRange);
			if (!childFirst)
				break;
			const unsigned temporary = ranges[parentPosition].heapRange;
			ranges[parentPosition].heapRange = childRange;
			ranges[heapPosition].heapRange = temporary;
			heapPosition = parentPosition;
		}
	}

	unsigned outputCount = 0;
	while (heapSize != 0)
	{
		const unsigned selectedRange = ranges[0].heapRange;
		CollisionCandidateRangeState &selected = ranges[selectedRange];
		const CollisionCandidate candidate =
			scratch[selected.begin + selected.cursor];
		++selected.cursor;
		if (outputCount == 0 || !sameKey(output[outputCount - 1], candidate))
			output[outputCount++] = candidate;

		if (selected.cursor == selected.uniqueCount)
		{
			--heapSize;
			if (heapSize != 0)
				ranges[0].heapRange = ranges[heapSize].heapRange;
		}

		unsigned heapPosition = 0;
		for (;;)
		{
			const unsigned leftPosition = heapPosition * 2 + 1;
			if (leftPosition >= heapSize)
				break;
			const unsigned rightPosition = leftPosition + 1;
			unsigned firstPosition = leftPosition;
			if (rightPosition < heapSize)
			{
				const unsigned leftRange = ranges[leftPosition].heapRange;
				const unsigned rightRange = ranges[rightPosition].heapRange;
				const CollisionCandidateRangeState &left = ranges[leftRange];
				const CollisionCandidateRangeState &right = ranges[rightRange];
				const CollisionCandidate &leftCandidate =
					scratch[left.begin + left.cursor];
				const CollisionCandidate &rightCandidate =
					scratch[right.begin + right.cursor];
				++metrics->ownerMergeComparisons;
				if (KeyOrder()(rightCandidate, leftCandidate) ||
					(!KeyOrder()(leftCandidate, rightCandidate) &&
					 rightRange < leftRange))
					firstPosition = rightPosition;
			}

			const unsigned parentRange = ranges[heapPosition].heapRange;
			const unsigned childRange = ranges[firstPosition].heapRange;
			const CollisionCandidateRangeState &parent = ranges[parentRange];
			const CollisionCandidateRangeState &child = ranges[childRange];
			const CollisionCandidate &parentCandidate =
				scratch[parent.begin + parent.cursor];
			const CollisionCandidate &childCandidate =
				scratch[child.begin + child.cursor];
			++metrics->ownerMergeComparisons;
			const bool childFirst = KeyOrder()(childCandidate,
				parentCandidate) ||
				(!KeyOrder()(parentCandidate, childCandidate) &&
				 childRange < parentRange);
			if (!childFirst)
				break;
			ranges[heapPosition].heapRange = childRange;
			ranges[firstPosition].heapRange = parentRange;
			heapPosition = firstPosition;
		}
	}
	return outputCount;
}

void orderMergedCandidates(CollisionCandidate *output, unsigned outputCount,
	CollisionCandidate *scratch, unsigned inputCount,
	CollisionCandidateOrder order)
{
	if (order != COLLISION_CANDIDATE_REVERSE_DISCOVERY || outputCount < 2)
		return;

	// Live partition discovery ordinals are a unique bounded input index. That
	// common path can be restored in reverse legacy order linearly. The generic
	// kernel retains the total comparator fallback for callers with repeated or
	// externally assigned ordinals.
	for (unsigned index = 0; index != inputCount; ++index)
		scratch[index].key.lowID = 0;
	bool boundedUniqueOrders = true;
	for (unsigned outputIndex = 0; outputIndex != outputCount; ++outputIndex)
	{
		const unsigned discoveryOrder = output[outputIndex].discoveryOrder;
		if (discoveryOrder >= inputCount ||
			scratch[discoveryOrder].key.lowID != 0)
		{
			boundedUniqueOrders = false;
			break;
		}
		scratch[discoveryOrder] = output[outputIndex];
	}
	if (!boundedUniqueOrders)
	{
		std::sort(output, output + outputCount, ReverseDiscoveryOrder());
		return;
	}

	unsigned orderedCount = 0;
	for (unsigned discoveryOrder = inputCount; discoveryOrder != 0;
		--discoveryOrder)
	{
		if (scratch[discoveryOrder - 1].key.lowID != 0)
			output[orderedCount++] = scratch[discoveryOrder - 1];
	}
}

unsigned finalizeCandidates(CollisionCandidate *scratch, unsigned inputCount,
	CollisionCandidateOrder order)
{
	unsigned validCount = 0;
	for (unsigned index = 0; index != inputCount; ++index)
	{
		if (scratch[index].key.lowID != 0)
			scratch[validCount++] = scratch[index];
	}
	std::sort(scratch, scratch + validCount, KeyOrder());

	unsigned uniqueCount = 0;
	for (unsigned uniqueIndex = 0; uniqueIndex != validCount; ++uniqueIndex)
	{
		if (uniqueCount == 0 || !sameKey(scratch[uniqueCount - 1],
			scratch[uniqueIndex]))
			scratch[uniqueCount++] = scratch[uniqueIndex];
	}

	if (order == COLLISION_CANDIDATE_REVERSE_DISCOVERY)
		std::sort(scratch, scratch + uniqueCount, ReverseDiscoveryOrder());
	// KeyOrder already produced canonical key order.
	return uniqueCount;
}

class CollisionCandidateJob : public Job
{
public:
	CollisionCandidateJob(const CollisionCandidateInput *inputs,
		CollisionCandidate *scratch, CollisionCandidateRangeState *range,
		CollisionJobAtomicUnsigned *activePhysicalWorkers,
		CollisionJobAtomicUnsigned *peakPhysicalWorkers
#if defined(_WIN64)
		, const CollisionCandidateTestHooks *testHooks, unsigned rangeIndex
#endif
		)
		: m_inputs(inputs), m_scratch(scratch), m_range(range),
		  m_activePhysicalWorkers(activePhysicalWorkers),
		  m_peakPhysicalWorkers(peakPhysicalWorkers)
#if defined(_WIN64)
		, m_testHooks(testHooks), m_rangeIndex(rangeIndex)
#endif
	{
	}

	virtual void execute(JobContext &context) { executeBody(&context); }
#if defined(_WIN64)
	void executeInline() { executeBody(0); }
#endif

	void executeBody(JobContext *context)
	{
#if defined(_WIN64)
		unsigned completedUnits = 0;
		bool testCancelled = false;
		CollisionSourceBodyScope sourceScope(m_range->sourceRange, completedUnits, m_range->completed);
		CollisionTestBodyScope testScope(m_testHooks, m_rangeIndex, *m_range, completedUnits);
#endif
		bool cancelled = context != 0 && context->isCancellationRequested();
#if defined(_WIN64)
		cancelled = collisionTestCheckpoint(m_testHooks, m_rangeIndex,
			COLLISION_CANDIDATE_TEST_CHECKPOINT_ENTRY, 0, cancelled);
		cancelled = collisionSourceCheckpoint(m_range->sourceRange, 1, 0, cancelled);
#endif
		if (cancelled) return;
		const bool physicalWorker = context != 0 && context->isPhysicalWorkerExecution();
		CollisionPhysicalExecutionScope physicalScope(physicalWorker,
			m_activePhysicalWorkers, m_peakPhysicalWorkers);
		normalizeRange(m_inputs, m_scratch, m_range->begin, m_range->end,
			context
#if defined(_WIN64)
			, m_testHooks, m_rangeIndex, &completedUnits,
			(m_testHooks != 0 || m_range->sourceRange != 0) ? &testCancelled : 0, m_range->sourceRange
#endif
			);
#if defined(_WIN64)
		if (testCancelled) return;
#endif
		cancelled = context != 0 && context->isCancellationRequested();
#if defined(_WIN64)
		cancelled = collisionTestCheckpoint(m_testHooks, m_rangeIndex,
			COLLISION_CANDIDATE_TEST_CHECKPOINT_POST_NORMALIZE, completedUnits, cancelled);
		cancelled = collisionSourceCheckpoint(m_range->sourceRange, 3, completedUnits, cancelled);
#endif
		if (cancelled) return;
#if defined(_WIN64)
		observeCollisionTest(m_testHooks, COLLISION_CANDIDATE_TEST_LOCAL_SORT,
			m_rangeIndex, m_range->begin, m_range->end, completedUnits);
#endif
		m_range->uniqueCount = sortAndDeduplicateRange(m_scratch,
			m_range->begin, m_range->end);
		cancelled = context != 0 && context->isCancellationRequested();
#if defined(_WIN64)
		cancelled = collisionTestCheckpoint(m_testHooks, m_rangeIndex,
			COLLISION_CANDIDATE_TEST_CHECKPOINT_POST_SORT, completedUnits, cancelled);
		cancelled = collisionSourceCheckpoint(m_range->sourceRange, 4, completedUnits, cancelled);
#endif
		if (cancelled) return;
		m_range->physicalWorker = physicalWorker;
		m_range->physicalWorkerIndex = context != 0 ? context->physicalWorkerIndex() : JOB_INVALID_PHYSICAL_WORKER_INDEX;
		m_range->completed = true;
	}

private:
	const CollisionCandidateInput *m_inputs;
	CollisionCandidate *m_scratch;
	CollisionCandidateRangeState *m_range;
	CollisionJobAtomicUnsigned *m_activePhysicalWorkers;
	CollisionJobAtomicUnsigned *m_peakPhysicalWorkers;
#if defined(_WIN64)
	const CollisionCandidateTestHooks *m_testHooks;
	unsigned m_rangeIndex;
#endif
};

void normalizePartitionRange(
	const PartitionCollisionObjectSnapshot *owner,
	const PartitionCollisionOccupantSnapshot *occupants,
	CollisionCandidate *scratch, unsigned begin, unsigned end,
	JobContext *context
#if defined(_WIN64)
	, const CollisionCandidateTestHooks *testHooks = 0, unsigned rangeIndex = 0,
	unsigned *completedUnits = 0, bool *testCancelled = 0, CollisionSourceRange *sourceRange = 0
#endif
	)
{
	for (unsigned index = begin; index != end; ++index)
	{
		if ((index - begin) % 256 == 0 && (context != 0
#if defined(_WIN64)
			|| sourceRange != 0
#endif
			))
		{
			bool cancelled = context != 0 && context->isCancellationRequested();
#if defined(_WIN64)
			cancelled = collisionTestCheckpoint(testHooks, rangeIndex,
				COLLISION_CANDIDATE_TEST_CHECKPOINT_BLOCK, index - begin, cancelled);
			cancelled = collisionSourceCheckpoint(sourceRange, 2, index - begin, cancelled);
			if (cancelled && testCancelled != 0) *testCancelled = true;
#endif
			if (cancelled) return;
		}
#if defined(_WIN64)
		observeCollisionTest(testHooks, COLLISION_CANDIDATE_TEST_PARTITION_NORMALIZED,
			rangeIndex, begin, end, index - begin);
#endif
		CollisionCandidate &candidate = scratch[index];
		candidate.firstID = owner->objectID;
		candidate.secondID = occupants[index].objectID;
		candidate.firstGeneration = owner->generation;
		candidate.secondGeneration = occupants[index].generation;
		candidate.discoveryOrder = index;
		MakeCollisionCandidateKey(candidate.firstID, candidate.secondID,
			candidate.key);
#if defined(_WIN64)
		if (completedUnits != 0) *completedUnits = index - begin + 1;
#endif
	}
}

class PartitionCollisionCandidateJob : public Job
{
public:
	PartitionCollisionCandidateJob(
		const PartitionCollisionObjectSnapshot *owner,
		const PartitionCollisionOccupantSnapshot *occupants,
		CollisionCandidate *scratch, CollisionCandidateRangeState *range,
		CollisionJobAtomicUnsigned *activePhysicalWorkers,
		CollisionJobAtomicUnsigned *peakPhysicalWorkers
#if defined(_WIN64)
		, const CollisionCandidateTestHooks *testHooks, unsigned rangeIndex
#endif
		)
		: m_owner(owner), m_occupants(occupants), m_scratch(scratch),
		  m_range(range), m_activePhysicalWorkers(activePhysicalWorkers),
		  m_peakPhysicalWorkers(peakPhysicalWorkers)
#if defined(_WIN64)
		, m_testHooks(testHooks), m_rangeIndex(rangeIndex)
#endif
	{
	}

	virtual void execute(JobContext &context) { executeBody(&context); }
#if defined(_WIN64)
	void executeInline() { executeBody(0); }
#endif

	void executeBody(JobContext *context)
	{
#if defined(_WIN64)
		unsigned completedUnits = 0;
		bool testCancelled = false;
		CollisionSourceBodyScope sourceScope(m_range->sourceRange, completedUnits, m_range->completed);
		CollisionTestBodyScope testScope(m_testHooks, m_rangeIndex, *m_range, completedUnits);
#endif
		bool cancelled = context != 0 && context->isCancellationRequested();
#if defined(_WIN64)
		cancelled = collisionTestCheckpoint(m_testHooks, m_rangeIndex,
			COLLISION_CANDIDATE_TEST_CHECKPOINT_ENTRY, 0, cancelled);
		cancelled = collisionSourceCheckpoint(m_range->sourceRange, 1, 0, cancelled);
#endif
		if (cancelled) return;
		const bool physicalWorker = context != 0 && context->isPhysicalWorkerExecution();
		CollisionPhysicalExecutionScope physicalScope(physicalWorker,
			m_activePhysicalWorkers, m_peakPhysicalWorkers);
		normalizePartitionRange(m_owner, m_occupants, m_scratch,
			m_range->begin, m_range->end, context
#if defined(_WIN64)
			, m_testHooks, m_rangeIndex, &completedUnits,
			(m_testHooks != 0 || m_range->sourceRange != 0) ? &testCancelled : 0, m_range->sourceRange
#endif
			);
#if defined(_WIN64)
		if (testCancelled) return;
#endif
		cancelled = context != 0 && context->isCancellationRequested();
#if defined(_WIN64)
		cancelled = collisionTestCheckpoint(m_testHooks, m_rangeIndex,
			COLLISION_CANDIDATE_TEST_CHECKPOINT_POST_NORMALIZE, completedUnits, cancelled);
		cancelled = collisionSourceCheckpoint(m_range->sourceRange, 3, completedUnits, cancelled);
#endif
		if (cancelled) return;
#if defined(_WIN64)
		observeCollisionTest(m_testHooks, COLLISION_CANDIDATE_TEST_LOCAL_SORT,
			m_rangeIndex, m_range->begin, m_range->end, completedUnits);
#endif
		m_range->uniqueCount = sortAndDeduplicateRange(m_scratch,
			m_range->begin, m_range->end);
		cancelled = context != 0 && context->isCancellationRequested();
#if defined(_WIN64)
		cancelled = collisionTestCheckpoint(m_testHooks, m_rangeIndex,
			COLLISION_CANDIDATE_TEST_CHECKPOINT_POST_SORT, completedUnits, cancelled);
		cancelled = collisionSourceCheckpoint(m_range->sourceRange, 4, completedUnits, cancelled);
#endif
		if (cancelled) return;
		m_range->physicalWorker = physicalWorker;
		m_range->physicalWorkerIndex = context != 0 ? context->physicalWorkerIndex() : JOB_INVALID_PHYSICAL_WORKER_INDEX;
		m_range->completed = true;
	}

private:
	const PartitionCollisionObjectSnapshot *m_owner;
	const PartitionCollisionOccupantSnapshot *m_occupants;
	CollisionCandidate *m_scratch;
	CollisionCandidateRangeState *m_range;
	CollisionJobAtomicUnsigned *m_activePhysicalWorkers;
	CollisionJobAtomicUnsigned *m_peakPhysicalWorkers;
#if defined(_WIN64)
	const CollisionCandidateTestHooks *m_testHooks;
	unsigned m_rangeIndex;
#endif
};


#if defined(_WIN64)
CollisionCandidateResult consumeCollisionCandidates(const CollisionCandidateInput *inputs,
	const PartitionCollisionObjectSnapshot *owner, const PartitionCollisionOccupantSnapshot *occupants,
	unsigned count, CollisionCandidate *output, CollisionCandidate *scratch,
	const CollisionCandidateOptions &options, unsigned *outputCount, CollisionCandidateMetrics &metrics,
	unsigned bodyKind, performance::KernelPerformanceCanonicalCallback writeInput, const void *input)
{
	using namespace performance;
	KernelPerformanceReferenceLedger &ledger = *options.performanceReferenceLedger;
	const KernelPerformanceAttempt attempt = options.performanceReferenceAttempt;
	JobSystem &jobs = JobSystem::instance();
	if (!attempt.valid() || ledger.mode() != KERNEL_REFERENCE_PHASE_BASELINE_BINDING ||
		options.performanceLedger == 0 || !options.performanceBatch.valid() ||
		options.performanceReferenceBatch == 0 ||
		jobs.isWorkerThread() || !jobs.isRunning() || !jobs.isCurrentThread(JOB_OWNER_GAME))
		return COLLISION_CANDIDATE_SERIAL_FALLBACK;
	const unsigned grain = options.minimumGrain != 0 ? options.minimumGrain : COLLISION_CANDIDATE_DEFAULT_MINIMUM_GRAIN;
	KernelPerformanceCanonicalWriter facts;
	KernelPerformanceAttemptDecision decision = {};
	if (!facts.begin(1) || !facts.u32(1, count) || !facts.u32(2, grain) ||
		!facts.u32(3, bodyKind) || !facts.u32(4, static_cast<unsigned>(options.order)) ||
		!ledger.bindCapturedInput(attempt, 1, count, writeInput, input) ||
		!ledger.replayDecision(attempt, 1, options.parallel && count >= COLLISION_CANDIDATE_MINIMUM_PARALLEL_INPUTS,
			facts.finish(), decision) || decision.decisionOrdinal != options.performanceReferenceDecisionOrdinal ||
			decision.admission != KERNEL_ADMISSION_ACCEPTED ||
		decision.reasonSchema != 1 || (decision.reason != 1 && decision.reason != 3))
		return COLLISION_CANDIDATE_SERIAL_FALLBACK;
	KernelPerformanceDispatchPlan dispatch = {};
	KernelPerformanceAttemptFinish sourceFinish = {};
	const unsigned planned = JobSystem::chooseRangeCount(count, grain, decision.sourceConfiguredWorkers);
	if (!ledger.readSourceDispatch(attempt, 1, dispatch) || !ledger.readSourceFinish(attempt, sourceFinish) ||
		dispatch.bodySchema != 1 || dispatch.checkpointSchema != 1 || dispatch.sourceGrain != grain ||
		dispatch.sourceLimit != COLLISION_CANDIDATE_MAXIMUM_INPUTS || dispatch.rangeCount == 0 ||
		dispatch.rangeCount > planned || planned < 2 ||
		(decision.reason == 3 && sourceFinish.disposition == KERNEL_PERFORMANCE_COMMITTED) ||
		(sourceFinish.validationObserved &&
		 sourceFinish.disposition != KERNEL_PERFORMANCE_COMMITTED &&
		 sourceFinish.disposition != KERNEL_PERFORMANCE_ABORTED_AFTER_ADMISSION))
		return COLLISION_CANDIDATE_SERIAL_FALLBACK;
	std::unique_ptr<CollisionCandidateRangeState[]> ranges(new (std::nothrow) CollisionCandidateRangeState[dispatch.rangeCount]);
	std::unique_ptr<CollisionSourceRange[]> sourceRanges(new (std::nothrow) CollisionSourceRange[dispatch.rangeCount]);
	std::unique_ptr<CollisionCandidate[]> discardedOutput;
	if (sourceFinish.validationObserved &&
		sourceFinish.disposition == KERNEL_PERFORMANCE_ABORTED_AFTER_ADMISSION)
		discardedOutput.reset(new (std::nothrow) CollisionCandidate[count]);
	{
		// Real owner plan setup ends before any authenticated inline body.
		KernelPerformanceScope schedule(options.performanceLedger, options.performanceBatch, KERNEL_PERFORMANCE_SCHEDULE);
		if (!ranges || !sourceRanges ||
			(sourceFinish.validationObserved &&
			 sourceFinish.disposition == KERNEL_PERFORMANCE_ABORTED_AFTER_ADMISSION &&
			 !discardedOutput) || !ledger.observeDispatch(attempt, dispatch))
			return COLLISION_CANDIDATE_SERIAL_FALLBACK;
		memset(ranges.get(), 0, sizeof(CollisionCandidateRangeState) * dispatch.rangeCount);
		JobMetricCounter operations = 0;
		for (unsigned i = 0; i != dispatch.rangeCount; ++i)
		{
			JobRange range;
			KernelPerformanceRangePlan source = {};
			if (!JobSystem::rangeForIndex(count, planned, i, range) ||
				!ledger.readSourceRange(attempt, 1, i, source) || source.bodyKind != bodyKind ||
				source.begin != range.begin || source.end != range.end || source.operationCount != range.end - range.begin ||
				!ledger.observeRangePlan(attempt, source)) return COLLISION_CANDIDATE_SERIAL_FALLBACK;
			sourceRanges[i].plan = source; sourceRanges[i].replay = true; operations += source.operationCount;
			ranges[i].begin = range.begin; ranges[i].end = range.end;
			ranges[i].physicalWorkerIndex = JOB_INVALID_PHYSICAL_WORKER_INDEX;
			ranges[i].sourceRange = &sourceRanges[i];
		}
		if (operations != dispatch.operationCount) return COLLISION_CANDIDATE_SERIAL_FALLBACK;
	}
	bool complete = dispatch.rangeCount == planned;
	for (unsigned i = 0; i != dispatch.rangeCount; ++i)
	{
		CollisionSourceRange &record = sourceRanges[i];
		KernelPerformanceInlineBody body;
		const KernelPerformanceInlineAction action = ledger.beginInlineBody(attempt,
			record.plan, *options.performanceLedger, body, record.checkpoint);
		if (action == KERNEL_INLINE_INVALID) return COLLISION_CANDIDATE_SERIAL_FALLBACK;
		if (action == KERNEL_INLINE_EXECUTE)
		{
			if (bodyKind == 1)
			{
				CollisionCandidateJob job(inputs, scratch, &ranges[i], 0, 0, options.testHooks, i);
				job.executeInline();
			}
			else
			{
				PartitionCollisionCandidateJob job(owner, occupants, scratch, &ranges[i], 0, 0, options.testHooks, i);
				job.executeInline();
			}
		}
		KernelPerformanceRangeProgress progress = {};
		progress.checkpoint = record.checkpoint.snapshot();
		progress.publication = !progress.checkpoint.entered ? KERNEL_PUBLICATION_NOT_APPLICABLE :
			decision.reason == 3 || progress.checkpoint.terminal == KERNEL_RANGE_CANCELLED ?
			KERNEL_PUBLICATION_DISCARDED_AFTER_CANCEL : sourceFinish.validationObserved ?
			KERNEL_PUBLICATION_PUBLISHED : KERNEL_PUBLICATION_REJECTED;
		if ((action == KERNEL_INLINE_EXECUTE && !ledger.finishInlineBody(body, progress)) ||
			!ledger.observeReleasedRange(attempt, record.plan, progress)) return COLLISION_CANDIDATE_SERIAL_FALLBACK;
		complete = ranges[i].completed && complete;
	}
	bool published = false;
	if (complete)
	{
		observeCollisionTest(options.testHooks, COLLISION_CANDIDATE_TEST_OWNER_VALIDATION, 0, 0, count, count, true);
		if (decision.reason != 3 && sourceFinish.validationObserved)
		{
			observeCollisionTest(options.testHooks, COLLISION_CANDIDATE_TEST_OWNER_REDUCTION, 0, 0, count, count, true);
			CollisionCandidate *validatedOutput =
				sourceFinish.disposition == KERNEL_PERFORMANCE_COMMITTED ?
					output : discardedOutput.get();
			const unsigned prepared = mergeSortedRanges(scratch, ranges.get(), planned,
				validatedOutput, &metrics);
			orderMergedCandidates(validatedOutput, prepared, scratch, count,
				options.order);
			metrics.preparedPairs = count; metrics.uniqueCandidates = prepared;
			const PartitionCollisionReferenceOutput production = {
				validatedOutput, 0, prepared, prepared};
			*options.performanceReferenceBatch = ledger.observeValidatedAttempt(attempt,
				WritePartitionCollisionReferenceOutput, &production);
			if (!options.performanceReferenceBatch->valid())
				return COLLISION_CANDIDATE_SERIAL_FALLBACK;
			if (sourceFinish.disposition == KERNEL_PERFORMANCE_COMMITTED)
			{
				*outputCount = prepared;
				observeCollisionTest(options.testHooks,
					COLLISION_CANDIDATE_TEST_PUBLICATION, 0, 0, count,
					prepared, true);
				published = true;
			}
		}
	}
	ranges.reset(); sourceRanges.reset();
	for (unsigned i = 0; i != dispatch.rangeCount; ++i)
	{
		JobRange range; JobSystem::rangeForIndex(count, planned, i, range);
		observeCollisionTest(options.testHooks, COLLISION_CANDIDATE_TEST_RANGE_RELEASED, i, range.begin, range.end);
	}
	// A partial admission cancels its cleanup group but retains native fallback.
	return published ? COLLISION_CANDIDATE_PARALLEL : decision.reason == 3 && dispatch.rangeCount == planned ?
		COLLISION_CANDIDATE_CANCELLED : COLLISION_CANDIDATE_SERIAL_FALLBACK;
}
#endif

CollisionMetricAtomic s_resetEpoch(0);
CollisionMetricAtomic s_authoritativeCommits(0);
CollisionMetricAtomic s_shadowExecutions(0);
CollisionMetricAtomic s_shadowMismatches(0);
CollisionMetricAtomic s_ownerFallbacks(0);
CollisionMetricAtomic s_unexpectedFallbacks(0);
CollisionMetricAtomic s_ineligibleSlices(0);
CollisionMetricAtomic s_staleRejections(0);
CollisionMetricAtomic s_committedCandidates(0);
CollisionMetricAtomic s_shadowComparedCandidates(0);
CollisionMetricAtomic s_preparedPairs(0);
CollisionMetricAtomic s_uniqueCandidates(0);
CollisionMetricAtomic s_submittedJobs(0);
CollisionMetricAtomic s_completedJobs(0);
CollisionMetricAtomic s_localSortRuns(0);
CollisionMetricAtomic s_locallyUniqueCandidates(0);
CollisionMetricAtomic s_ownerMergeComparisons(0);
CollisionMetricAtomic s_maximumRangeInputs(0);
CollisionMetricAtomic s_physicalWorkerJobs(0);
CollisionMetricAtomic s_ownerHelpedJobs(0);
CollisionMetricAtomic s_physicalWorkerMask(0);
CollisionMetricAtomic s_distinctPhysicalWorkers(0);
CollisionMetricAtomic s_physicalWorkerMaskIncomplete(0);
CollisionMetricAtomic s_maximumPeakConcurrentPhysicalWorkers(0);

void recordParallelWorkMetrics(const CollisionCandidateMetrics &metrics)
{
	addMetric(s_preparedPairs, metrics.preparedPairs);
	addMetric(s_uniqueCandidates, metrics.uniqueCandidates);
	addMetric(s_submittedJobs, metrics.submittedJobs);
	addMetric(s_completedJobs, metrics.completedJobs);
	addMetric(s_localSortRuns, metrics.localSortRuns);
	addMetric(s_locallyUniqueCandidates, metrics.locallyUniqueCandidates);
	addMetric(s_ownerMergeComparisons, metrics.ownerMergeComparisons);
	maximizeMetric(s_maximumRangeInputs, metrics.maximumRangeInputs);
	addMetric(s_physicalWorkerJobs, metrics.physicalWorkerJobs);
	addMetric(s_ownerHelpedJobs, metrics.ownerHelpedJobs);
	orMetric(s_physicalWorkerMask, metrics.physicalWorkerMask);
	maximizeMetric(s_distinctPhysicalWorkers,
		metrics.distinctPhysicalWorkers);
	maximizeMetric(s_maximumPeakConcurrentPhysicalWorkers,
		metrics.peakConcurrentPhysicalWorkers);
	if (!metrics.physicalWorkerMaskComplete)
		addMetric(s_physicalWorkerMaskIncomplete, 1);
}

#if defined(RTS_BUILD_CORE_EXTRAS) && \
	(!defined(_MSC_VER) || _MSC_VER >= 1300)
unsigned s_testAllocationFailureOccurrence = 0;
unsigned s_testAllocationAttempt = 0;

bool shouldFailTestAllocation()
{
	if (s_testAllocationFailureOccurrence == 0)
		return false;
	++s_testAllocationAttempt;
	return s_testAllocationAttempt == s_testAllocationFailureOccurrence;
}
#else
bool shouldFailTestAllocation()
{
	return false;
}
#endif
}

#if defined(RTS_BUILD_CORE_EXTRAS) && \
	(!defined(_MSC_VER) || _MSC_VER >= 1300)
extern "C" void rts_collision_candidate_set_test_allocation_failure(
	unsigned occurrence)
{
	s_testAllocationFailureOccurrence = occurrence;
	s_testAllocationAttempt = 0;
}
#endif

CollisionCandidateResult PrepareCollisionCandidates(
	const CollisionCandidateInput *inputs,
	unsigned inputCount,
	CollisionCandidate *output,
	unsigned outputCapacity,
	CollisionCandidate *scratch,
	unsigned scratchCapacity,
	const CollisionCandidateOptions &options,
	unsigned *outputCount,
	CollisionCandidateMetrics *metrics)
{
	CollisionCandidateMetrics localMetrics;
	if (metrics == 0)
		metrics = &localMetrics;
	*metrics = CollisionCandidateMetrics();
	if (outputCount == 0 ||
		(inputCount != 0 && (inputs == 0 || output == 0 || scratch == 0)) ||
		outputCapacity < inputCount ||
		scratchCapacity < inputCount ||
		inputCount > COLLISION_CANDIDATE_MAXIMUM_INPUTS ||
		(options.order != COLLISION_CANDIDATE_REVERSE_DISCOVERY &&
		 options.order != COLLISION_CANDIDATE_CANONICAL_KEY))
		return COLLISION_CANDIDATE_INVALID_INPUT;
	CollisionCandidateAddressSpan spans[3];
	if (!makeCollisionCandidateAddressSpan(inputs, inputCount,
			sizeof(CollisionCandidateInput), spans[0]) ||
		!makeCollisionCandidateAddressSpan(output, inputCount,
			sizeof(CollisionCandidate), spans[1]) ||
		!makeCollisionCandidateAddressSpan(scratch, inputCount,
			sizeof(CollisionCandidate), spans[2]) ||
		!collisionCandidateAddressSpansAreDisjoint(spans, 3))
	{
		return COLLISION_CANDIDATE_INVALID_INPUT;
	}
	for (unsigned index = 0; index != inputCount; ++index)
	{
		if (inputs[index].firstID != 0 && inputs[index].secondID != 0 &&
			inputs[index].firstID != inputs[index].secondID &&
			(inputs[index].firstGeneration == 0 ||
			 inputs[index].secondGeneration == 0))
			return COLLISION_CANDIDATE_INVALID_INPUT;
	}
#if defined(_WIN64)
	if (options.performanceReferenceLedger != 0 &&
		options.performanceReferenceLedger->runMode() == performance::KERNEL_REFERENCE_PHASE_BASELINE_BINDING)
	{
		const GenericCollisionReferenceInput input = {inputs, inputCount, options.order};
		return consumeCollisionCandidates(inputs, 0, 0, inputCount, output, scratch,
			options, outputCount, *metrics, 1, writeGenericCollisionReferenceInput, &input);
	}
#endif
	if (cancelled(options))
		return COLLISION_CANDIDATE_CANCELLED;
	if (inputCount == 0)
	{
		*outputCount = 0;
		return COLLISION_CANDIDATE_SERIAL;
	}

	const unsigned minimumGrain = options.minimumGrain != 0 ?
		options.minimumGrain : COLLISION_CANDIDATE_DEFAULT_MINIMUM_GRAIN;
	if (!options.parallel ||
		inputCount < COLLISION_CANDIDATE_MINIMUM_PARALLEL_INPUTS)
	{
		normalizeRange(inputs, scratch, 0, inputCount, 0);
		const unsigned preparedCount = finalizeCandidates(scratch, inputCount,
			options.order);
		if (cancelled(options))
			return COLLISION_CANDIDATE_CANCELLED;
		memcpy(output, scratch, sizeof(CollisionCandidate) * preparedCount);
		*outputCount = preparedCount;
		metrics->preparedPairs = inputCount;
		metrics->uniqueCandidates = preparedCount;
		return COLLISION_CANDIDATE_SERIAL;
	}

	JobSystem &jobs = JobSystem::instance();
	// Scheduler startup and owner registration are engine lifecycle operations.
	// A simulation kernel must never lazily create workers from inside a tick.
	if (jobs.isWorkerThread() || !jobs.isRunning() ||
		!jobs.isCurrentThread(JOB_OWNER_GAME) || jobs.workerCount() <= 1)
	{
		++metrics->serialFallbacks;
		jobs.recordSerialFallback();
		return COLLISION_CANDIDATE_SERIAL_FALLBACK;
	}
	unsigned jobCount = JobSystem::chooseRangeCount(inputCount, minimumGrain,
		jobs.workerCount());
	if (jobCount < 2)
	{
		++metrics->serialFallbacks;
		jobs.recordSerialFallback();
		return COLLISION_CANDIDATE_SERIAL_FALLBACK;
	}

#if defined(_WIN64)
	const GenericCollisionReferenceInput sourceInput = {inputs, inputCount, options.order};
	CollisionSourceAttempt sourceAttempt(options, inputCount, minimumGrain, jobCount,
		1, jobs, writeGenericCollisionReferenceInput, &sourceInput);
#endif
	JobGroup group = jobs.createGroup();
	if (!group.isValid())
	{
		++metrics->serialFallbacks;
		jobs.recordSerialFallback();
		return COLLISION_CANDIDATE_SERIAL_FALLBACK;
	}

	CollisionCandidateRangeState *ranges = shouldFailTestAllocation() ? 0 :
		new (std::nothrow) CollisionCandidateRangeState[jobCount];
	if (ranges == 0)
	{
		++metrics->serialFallbacks;
		jobs.recordSerialFallback();
		return COLLISION_CANDIDATE_SERIAL_FALLBACK;
	}
	memset(ranges, 0, sizeof(CollisionCandidateRangeState) * jobCount);
	CollisionJobAtomicUnsigned activePhysicalWorkers(0);
	CollisionJobAtomicUnsigned peakPhysicalWorkers(0);
	unsigned submitted = 0;
	bool accepted = true;
	#if defined(_WIN64)
	{
		performance::KernelPerformanceScope scheduleTiming(
			options.performanceLedger, options.performanceBatch,
			performance::KERNEL_PERFORMANCE_SCHEDULE);
	#endif
	for (; submitted != jobCount; ++submitted)
	{
		JobRange range;
		if (!JobSystem::rangeForIndex(inputCount, jobCount, submitted, range))
		{
			accepted = false;
			break;
		}
		ranges[submitted].begin = range.begin;
		ranges[submitted].end = range.end;
		ranges[submitted].physicalWorkerIndex =
			JOB_INVALID_PHYSICAL_WORKER_INDEX;
#if defined(_WIN64)
		ranges[submitted].sourceRange = sourceAttempt.plan(submitted, range.begin, range.end);
#endif
		CollisionCandidateJob *job = new (std::nothrow)
			CollisionCandidateJob(inputs, scratch, ranges + submitted,
				&activePhysicalWorkers, &peakPhysicalWorkers
#if defined(_WIN64)
				, options.testHooks, submitted
#endif
				);
		JobHandle handle = job != 0 ? jobs.trySubmit(job,
			JOB_PRIORITY_FRAME_CRITICAL, group) : JobHandle();
		if (!handle.isValid())
		{
			delete job;
			accepted = false;
			break;
		}
		++metrics->submittedJobs;
	}
#if defined(_WIN64)
	}
#endif
	if (!accepted)
	{
		jobs.cancel(group);
	#if defined(_WIN64)
		{
			performance::KernelPerformanceScope waitTiming(
				options.performanceLedger, options.performanceBatch,
				performance::KERNEL_PERFORMANCE_WAIT);
			jobs.wait(group);
		}
	#else
		jobs.wait(group);
	#endif
#if defined(_WIN64)
		sourceAttempt.release(submitted, false, true);
#endif
		delete[] ranges;
#if defined(_WIN64)
		observeCollisionTestRelease(options, inputCount, jobCount);
#endif
		++metrics->serialFallbacks;
		jobs.recordSerialFallback();
		return COLLISION_CANDIDATE_SERIAL_FALLBACK;
	}

#if defined(_WIN64)
	{
		performance::KernelPerformanceScope waitTiming(
			options.performanceLedger, options.performanceBatch,
			performance::KERNEL_PERFORMANCE_WAIT);
		waitForCollisionTestWorkers(options, jobs, group);
		jobs.wait(group);
	}
#else
	jobs.wait(group);
#endif
	collectRangeMetrics(ranges, jobCount, metrics);
	metrics->peakConcurrentPhysicalWorkers =
		loadJobCounter(peakPhysicalWorkers);
#if defined(_WIN64)
	if (metrics->completedJobs == metrics->submittedJobs)
		observeCollisionTest(options.testHooks, COLLISION_CANDIDATE_TEST_OWNER_VALIDATION,
			0, 0, inputCount, inputCount, true);
#endif
	if (cancelled(options) || group.wasCancelled())
	{
#if defined(_WIN64)
		sourceAttempt.release(submitted, false, true);
#endif
		delete[] ranges;
#if defined(_WIN64)
		observeCollisionTestRelease(options, inputCount, jobCount);
#endif
		return COLLISION_CANDIDATE_CANCELLED;
	}
	if (group.failed() || metrics->completedJobs != metrics->submittedJobs)
	{
#if defined(_WIN64)
		sourceAttempt.release(submitted, false, false);
#endif
		delete[] ranges;
#if defined(_WIN64)
		observeCollisionTestRelease(options, inputCount, jobCount);
#endif
		++metrics->serialFallbacks;
		jobs.recordSerialFallback();
		return COLLISION_CANDIDATE_SERIAL_FALLBACK;
	}

#if defined(_WIN64)
	observeCollisionTest(options.testHooks, COLLISION_CANDIDATE_TEST_OWNER_REDUCTION,
		0, 0, inputCount, inputCount, true);
#endif
	const unsigned preparedCount = mergeSortedRanges(scratch, ranges, jobCount,
		output, metrics);
	orderMergedCandidates(output, preparedCount, scratch, inputCount,
		options.order);
	*outputCount = preparedCount;
	metrics->preparedPairs = inputCount;
	metrics->uniqueCandidates = preparedCount;
#if defined(_WIN64)
	observeCollisionTest(options.testHooks, COLLISION_CANDIDATE_TEST_PUBLICATION,
		0, 0, inputCount, preparedCount, true);
	sourceAttempt.release(submitted, true, false);
	sourceAttempt.validated(output, preparedCount);
#endif
	delete[] ranges;
#if defined(_WIN64)
	observeCollisionTestRelease(options, inputCount, jobCount);
#endif
	return COLLISION_CANDIDATE_PARALLEL;
}

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
	CollisionCandidateMetrics *metrics)
{
	CollisionCandidateMetrics localMetrics;
	if (metrics == 0)
		metrics = &localMetrics;
	*metrics = CollisionCandidateMetrics();
	if (outputCount == 0 || owner.objectID == 0 || owner.generation == 0 ||
		(cellCount != 0 && cells == 0) ||
		(occupantCount != 0 &&
		 (occupants == 0 || output == 0 || scratch == 0)) ||
		outputCapacity < occupantCount ||
		scratchCapacity < occupantCount ||
		occupantCount > COLLISION_CANDIDATE_MAXIMUM_INPUTS ||
		(options.order != COLLISION_CANDIDATE_REVERSE_DISCOVERY &&
		 options.order != COLLISION_CANDIDATE_CANONICAL_KEY))
		return COLLISION_CANDIDATE_INVALID_INPUT;
	CollisionCandidateAddressSpan spans[4];
	if (!makeCollisionCandidateAddressSpan(cells, cellCount,
			sizeof(PartitionCollisionCellSnapshot), spans[0]) ||
		!makeCollisionCandidateAddressSpan(occupants, occupantCount,
			sizeof(PartitionCollisionOccupantSnapshot), spans[1]) ||
		!makeCollisionCandidateAddressSpan(output, occupantCount,
			sizeof(CollisionCandidate), spans[2]) ||
		!makeCollisionCandidateAddressSpan(scratch, occupantCount,
			sizeof(CollisionCandidate), spans[3]) ||
		!collisionCandidateAddressSpansAreDisjoint(spans, 4))
	{
		return COLLISION_CANDIDATE_INVALID_INPUT;
	}

	unsigned expectedBegin = 0;
	for (unsigned cell = 0; cell != cellCount; ++cell)
	{
		const PartitionCollisionCellSnapshot &snapshot = cells[cell];
		if (snapshot.occupantBegin != expectedBegin ||
			snapshot.discoveryBase != expectedBegin ||
			snapshot.occupantCount > occupantCount - expectedBegin)
			return COLLISION_CANDIDATE_INVALID_INPUT;
		expectedBegin += snapshot.occupantCount;
	}
	if (expectedBegin != occupantCount)
		return COLLISION_CANDIDATE_INVALID_INPUT;
	for (unsigned index = 0; index != occupantCount; ++index)
	{
		if (occupants[index].objectID != 0 &&
			occupants[index].generation == 0)
			return COLLISION_CANDIDATE_INVALID_INPUT;
	}
#if defined(_WIN64)
	if (options.performanceReferenceLedger != 0 &&
		options.performanceReferenceLedger->runMode() == performance::KERNEL_REFERENCE_PHASE_BASELINE_BINDING)
	{
		PartitionCollisionReferenceInput input;
		input.owner = owner; input.cells = cells; input.cellCount = cellCount;
		input.occupants = occupants; input.occupantCount = occupantCount; input.order = options.order;
		return consumeCollisionCandidates(0, &owner, occupants, occupantCount, output, scratch,
			options, outputCount, *metrics, 2, WritePartitionCollisionReferenceInput, &input);
	}
#endif
	if (cancelled(options))
		return COLLISION_CANDIDATE_CANCELLED;
	if (occupantCount == 0)
	{
		*outputCount = 0;
		return COLLISION_CANDIDATE_SERIAL;
	}

	const unsigned minimumGrain = options.minimumGrain != 0 ?
		options.minimumGrain : COLLISION_CANDIDATE_DEFAULT_MINIMUM_GRAIN;
	if (!options.parallel ||
		occupantCount < COLLISION_CANDIDATE_MINIMUM_PARALLEL_INPUTS)
	{
		normalizePartitionRange(&owner, occupants, scratch, 0,
			occupantCount, 0);
		const unsigned preparedCount = finalizeCandidates(scratch,
			occupantCount, options.order);
		if (cancelled(options))
			return COLLISION_CANDIDATE_CANCELLED;
		memcpy(output, scratch, sizeof(CollisionCandidate) * preparedCount);
		*outputCount = preparedCount;
		metrics->preparedPairs = occupantCount;
		metrics->uniqueCandidates = preparedCount;
		return COLLISION_CANDIDATE_SERIAL;
	}

	JobSystem &jobs = JobSystem::instance();
	if (jobs.isWorkerThread() || !jobs.isRunning() ||
		!jobs.isCurrentThread(JOB_OWNER_GAME) || jobs.workerCount() <= 1)
	{
		++metrics->serialFallbacks;
		jobs.recordSerialFallback();
		return COLLISION_CANDIDATE_SERIAL_FALLBACK;
	}
	const unsigned jobCount = JobSystem::chooseRangeCount(occupantCount,
		minimumGrain, jobs.workerCount());
	if (jobCount < 2)
	{
		++metrics->serialFallbacks;
		jobs.recordSerialFallback();
		return COLLISION_CANDIDATE_SERIAL_FALLBACK;
	}

#if defined(_WIN64)
	PartitionCollisionReferenceInput sourceInput;
	sourceInput.owner = owner; sourceInput.cells = cells; sourceInput.cellCount = cellCount;
	sourceInput.occupants = occupants; sourceInput.occupantCount = occupantCount; sourceInput.order = options.order;
	CollisionSourceAttempt sourceAttempt(options, occupantCount, minimumGrain, jobCount,
		2, jobs, WritePartitionCollisionReferenceInput, &sourceInput);
#endif
	JobGroup group = jobs.createGroup();
	if (!group.isValid())
	{
		++metrics->serialFallbacks;
		jobs.recordSerialFallback();
		return COLLISION_CANDIDATE_SERIAL_FALLBACK;
	}
	CollisionCandidateRangeState *ranges = shouldFailTestAllocation() ? 0 :
		new (std::nothrow) CollisionCandidateRangeState[jobCount];
	if (ranges == 0)
	{
		++metrics->serialFallbacks;
		jobs.recordSerialFallback();
		return COLLISION_CANDIDATE_SERIAL_FALLBACK;
	}
	memset(ranges, 0, sizeof(CollisionCandidateRangeState) * jobCount);
	CollisionJobAtomicUnsigned activePhysicalWorkers(0);
	CollisionJobAtomicUnsigned peakPhysicalWorkers(0);
	unsigned submitted = 0;
	bool accepted = true;
#if defined(_WIN64)
	{
		performance::KernelPerformanceScope scheduleTiming(
			options.performanceLedger, options.performanceBatch,
			performance::KERNEL_PERFORMANCE_SCHEDULE);
#endif
	for (; submitted != jobCount; ++submitted)
	{
		JobRange range;
		if (!JobSystem::rangeForIndex(occupantCount, jobCount, submitted,
			range))
		{
			accepted = false;
			break;
		}
		ranges[submitted].begin = range.begin;
		ranges[submitted].end = range.end;
		ranges[submitted].physicalWorkerIndex =
			JOB_INVALID_PHYSICAL_WORKER_INDEX;
#if defined(_WIN64)
		ranges[submitted].sourceRange = sourceAttempt.plan(submitted, range.begin, range.end);
#endif
		PartitionCollisionCandidateJob *job = new (std::nothrow)
			PartitionCollisionCandidateJob(&owner, occupants, scratch,
				ranges + submitted, &activePhysicalWorkers,
				&peakPhysicalWorkers
#if defined(_WIN64)
				, options.testHooks, submitted
#endif
				);
		JobHandle handle = job != 0 ? jobs.trySubmit(job,
			JOB_PRIORITY_FRAME_CRITICAL, group) : JobHandle();
		if (!handle.isValid())
		{
			delete job;
			accepted = false;
			break;
		}
		++metrics->submittedJobs;
	}
#if defined(_WIN64)
	}
#endif
	if (!accepted)
	{
		jobs.cancel(group);
	#if defined(_WIN64)
		{
			performance::KernelPerformanceScope waitTiming(
				options.performanceLedger, options.performanceBatch,
				performance::KERNEL_PERFORMANCE_WAIT);
			jobs.wait(group);
		}
	#else
		jobs.wait(group);
	#endif
#if defined(_WIN64)
		sourceAttempt.release(submitted, false, true);
#endif
		delete[] ranges;
#if defined(_WIN64)
		observeCollisionTestRelease(options, occupantCount, jobCount);
#endif
		++metrics->serialFallbacks;
		jobs.recordSerialFallback();
		return COLLISION_CANDIDATE_SERIAL_FALLBACK;
	}

	{
		rts::frame_timing::Scope partitionWaitTiming(
			rts::frame_timing::SimulationWait);
	#if defined(_WIN64)
		performance::KernelPerformanceScope waitTiming(
			options.performanceLedger, options.performanceBatch,
			performance::KERNEL_PERFORMANCE_WAIT);
	#endif
#if defined(_WIN64)
		waitForCollisionTestWorkers(options, jobs, group);
#endif
		jobs.wait(group);
	}
	collectRangeMetrics(ranges, jobCount, metrics);
	metrics->peakConcurrentPhysicalWorkers =
		loadJobCounter(peakPhysicalWorkers);
#if defined(_WIN64)
	if (metrics->completedJobs == metrics->submittedJobs)
		observeCollisionTest(options.testHooks, COLLISION_CANDIDATE_TEST_OWNER_VALIDATION,
			0, 0, occupantCount, occupantCount, true);
#endif
	if (cancelled(options) || group.wasCancelled())
	{
#if defined(_WIN64)
		sourceAttempt.release(submitted, false, true);
#endif
		delete[] ranges;
#if defined(_WIN64)
		observeCollisionTestRelease(options, occupantCount, jobCount);
#endif
		return COLLISION_CANDIDATE_CANCELLED;
	}
	if (group.failed() || metrics->completedJobs != metrics->submittedJobs)
	{
#if defined(_WIN64)
		sourceAttempt.release(submitted, false, false);
#endif
		delete[] ranges;
#if defined(_WIN64)
		observeCollisionTestRelease(options, occupantCount, jobCount);
#endif
		++metrics->serialFallbacks;
		jobs.recordSerialFallback();
		return COLLISION_CANDIDATE_SERIAL_FALLBACK;
	}

	unsigned preparedCount = 0;
	{
		rts::frame_timing::Scope partitionReduceTiming(
			rts::frame_timing::SimulationReduce);
#if defined(_WIN64)
	observeCollisionTest(options.testHooks, COLLISION_CANDIDATE_TEST_OWNER_REDUCTION,
		0, 0, occupantCount, occupantCount, true);
#endif
		preparedCount = mergeSortedRanges(scratch, ranges, jobCount, output,
			metrics);
		orderMergedCandidates(output, preparedCount, scratch, occupantCount,
			options.order);
	}
	*outputCount = preparedCount;
	metrics->preparedPairs = occupantCount;
	metrics->uniqueCandidates = preparedCount;
#if defined(_WIN64)
	observeCollisionTest(options.testHooks, COLLISION_CANDIDATE_TEST_PUBLICATION,
		0, 0, occupantCount, preparedCount, true);
	sourceAttempt.release(submitted, true, false);
	sourceAttempt.validated(output, preparedCount);
#endif
	delete[] ranges;
#if defined(_WIN64)
	observeCollisionTestRelease(options, occupantCount, jobCount);
#endif
	return COLLISION_CANDIDATE_PARALLEL;
}

#if defined(_WIN64)
CollisionCandidateReferenceBatchTransport::CollisionCandidateReferenceBatchTransport()
	: referenceLedger(0), referenceBatch(0), writeInput(0), immutableInput(0),
	  writeOutput(0), productionOutput(0), serialCompute(0),
	  detachedSerialOutput(0), operationCount(0),
	  fieldSchema(COLLISION_CANDIDATE_REFERENCE_FIELD_SCHEMA)
{
}

bool ObserveCollisionCandidateReferenceBatch(
	performance::KernelPerformanceLedger *timingLedger,
	const performance::KernelPerformanceBatch *timingBatch,
	CollisionCandidateReferenceBatchTransport *transport) noexcept
{
	if (timingLedger == 0 || timingBatch == 0 || transport == 0 ||
		transport->referenceLedger == 0 || transport->referenceBatch == 0 ||
		transport->referenceBatch->valid() || transport->writeInput == 0 ||
		transport->immutableInput == 0 || transport->writeOutput == 0 ||
		transport->productionOutput == 0 || transport->operationCount == 0 ||
		transport->fieldSchema == 0 ||
		transport->referenceLedger->mode() ==
			performance::KERNEL_REFERENCE_DISABLED)
		return false;
	performance::KernelPerformanceBatchIdentity identity;
	if (!timingLedger->describeBatch(*timingBatch, identity) ||
		identity.kernel != performance::KERNEL_PERFORMANCE_COLLISION ||
		identity.subtype != 0)
		return false;
	const performance::KernelPerformanceReferenceBatch observed =
		transport->referenceLedger->observeValidatedBatch(identity.kernel,
		identity.subtype, identity.frame, identity.ordinal,
		transport->fieldSchema, transport->operationCount,
		transport->writeInput, transport->immutableInput,
		transport->writeOutput, transport->productionOutput,
		transport->serialCompute, transport->detachedSerialOutput);
	if (!observed.valid())
		return false;
	*transport->referenceBatch = observed;
	return true;
}

bool FinishCollisionCandidateReferenceBatch(
	CollisionCandidateReferenceBatchTransport *transport,
	bool committed) noexcept
{
	if (transport == 0 || transport->referenceLedger == 0 ||
		transport->referenceBatch == 0 ||
		!transport->referenceBatch->valid())
		return false;
	if (!transport->referenceLedger->finishBatch(
			*transport->referenceBatch, committed))
		return false;
	*transport->referenceBatch = performance::KernelPerformanceReferenceBatch();
	return true;
}

bool WritePartitionCollisionReferenceInput(
	performance::KernelPerformanceCanonicalWriter &writer,
	const void *context)
{
	const PartitionCollisionReferenceInput *input =
		static_cast<const PartitionCollisionReferenceInput *>(context);
	if (input == 0 ||
		(input->cellCount != 0 && input->cells == 0) ||
		(input->occupantCount != 0 && input->occupants == 0))
		return false;
	const PartitionCollisionObjectSnapshot &owner = input->owner;
	if (!writer.u32(1, owner.objectID) || !writer.u32(2, owner.generation) ||
		!writer.u32(3, owner.dirtyOrder) ||
		!writer.f32(4, owner.positionX) || !writer.f32(5, owner.positionY) ||
		!writer.f32(6, owner.positionZ) ||
		!writer.f32(7, owner.orientation) ||
		!writer.f32(8, owner.majorRadius) ||
		!writer.f32(9, owner.minorRadius) ||
		!writer.u32(10, owner.geometryType) ||
		!writer.boolean(11, owner.smallGeometry) ||
		!writer.sequence(20, input->cellCount))
		return false;
	for (unsigned cellIndex = 0; cellIndex != input->cellCount;
		++cellIndex)
	{
		const PartitionCollisionCellSnapshot &cell =
			input->cells[cellIndex];
		if (!writer.u32(21, cell.occupantBegin) ||
			!writer.u32(22, cell.occupantCount) ||
			!writer.u32(23, cell.discoveryBase))
			return false;
	}
	if (!writer.sequence(30, input->occupantCount))
		return false;
	for (unsigned occupantIndex = 0;
		occupantIndex != input->occupantCount; ++occupantIndex)
	{
		const PartitionCollisionOccupantSnapshot &occupant =
			input->occupants[occupantIndex];
		if (!writer.u32(31, occupant.objectID) ||
			!writer.u32(32, occupant.generation))
			return false;
	}
	return writer.u32(40, static_cast<unsigned>(input->order));
}

bool WritePartitionCollisionReferenceOutput(
	performance::KernelPerformanceCanonicalWriter &writer,
	const void *context)
{
	const PartitionCollisionReferenceOutput *output =
		static_cast<const PartitionCollisionReferenceOutput *>(context);
	if (output == 0 || output->count > output->capacity ||
		(output->count != 0 && output->candidates == 0))
		return false;
	if (!writer.sequence(1, output->count))
		return false;
	for (unsigned index = 0; index != output->count; ++index)
	{
		const CollisionCandidate &candidate = output->candidates[index];
		if (!writer.u32(2, candidate.key.lowID) ||
			!writer.u32(3, candidate.key.highID) ||
			!writer.u32(4, candidate.firstID) ||
			!writer.u32(5, candidate.secondID) ||
			!writer.u32(6, candidate.firstGeneration) ||
			!writer.u32(7, candidate.secondGeneration) ||
			!writer.u32(8, candidate.discoveryOrder))
			return false;
	}
	return true;
}

bool ComputePartitionCollisionCandidatesSerialReference(
	const void *immutableInput, void *detachedOutput)
{
	const PartitionCollisionReferenceInput *input =
		static_cast<const PartitionCollisionReferenceInput *>(immutableInput);
	PartitionCollisionReferenceOutput *output =
		static_cast<PartitionCollisionReferenceOutput *>(detachedOutput);
	if (input == 0 || output == 0 || input->owner.objectID == 0 ||
		input->owner.generation == 0 ||
		(input->cellCount != 0 && input->cells == 0) ||
		(input->occupantCount != 0 && input->occupants == 0) ||
		(input->occupantCount != 0 &&
			(output->candidates == 0 || output->scratch == 0)) ||
		output->capacity < input->occupantCount ||
		(input->order != COLLISION_CANDIDATE_REVERSE_DISCOVERY &&
			input->order != COLLISION_CANDIDATE_CANONICAL_KEY))
		return false;
	CollisionCandidateAddressSpan detachedSpans[2];
	if (!makeCollisionCandidateAddressSpan(output->candidates,
			output->capacity, sizeof(CollisionCandidate), detachedSpans[0]) ||
		!makeCollisionCandidateAddressSpan(output->scratch, output->capacity,
			sizeof(CollisionCandidate), detachedSpans[1]) ||
		!collisionCandidateAddressSpansAreDisjoint(detachedSpans, 2))
		return false;
	unsigned expectedBegin = 0;
	for (unsigned cell = 0; cell != input->cellCount; ++cell)
	{
		const PartitionCollisionCellSnapshot &snapshot = input->cells[cell];
		if (snapshot.occupantBegin != expectedBegin ||
			snapshot.discoveryBase != expectedBegin ||
			snapshot.occupantCount > input->occupantCount - expectedBegin)
			return false;
		expectedBegin += snapshot.occupantCount;
	}
	if (expectedBegin != input->occupantCount)
		return false;
	for (unsigned index = 0; index != input->occupantCount; ++index)
	{
		if (input->occupants[index].objectID != 0 &&
			input->occupants[index].generation == 0)
			return false;
	}
	if (input->occupantCount == 0)
	{
		output->count = 0;
		return true;
	}
	normalizePartitionRange(&input->owner, input->occupants, output->scratch,
		0, input->occupantCount, 0);
	const unsigned count = finalizeCandidates(output->scratch,
		input->occupantCount, input->order);
	if (count > output->capacity)
		return false;
	memcpy(output->candidates, output->scratch,
		sizeof(CollisionCandidate) * count);
	output->count = count;
	return true;
}
#endif

bool CollisionCandidatesEqual(
	const CollisionCandidate *left,
	unsigned leftCount,
	const CollisionCandidate *right,
	unsigned rightCount,
	unsigned *firstDifference)
{
	if (firstDifference != 0)
		*firstDifference = 0;
	if ((leftCount != 0 && left == 0) ||
		(rightCount != 0 && right == 0))
		return false;
	const unsigned commonCount = leftCount < rightCount ?
		leftCount : rightCount;
	for (unsigned index = 0; index != commonCount; ++index)
	{
		if (left[index].key.lowID != right[index].key.lowID ||
			left[index].key.highID != right[index].key.highID ||
			left[index].firstID != right[index].firstID ||
			left[index].secondID != right[index].secondID ||
			left[index].firstGeneration != right[index].firstGeneration ||
			left[index].secondGeneration != right[index].secondGeneration ||
			left[index].discoveryOrder != right[index].discoveryOrder)
		{
			if (firstDifference != 0)
				*firstDifference = index;
			return false;
		}
	}
	if (firstDifference != 0)
		*firstDifference = commonCount;
	return leftCount == rightCount;
}

bool ValidateCollisionCandidateGenerations(
	const CollisionCandidate *candidates,
	unsigned candidateCount,
	CollisionCandidateGenerationResolver resolver,
	void *context,
	unsigned *firstStaleCandidate)
{
	if (firstStaleCandidate != 0)
		*firstStaleCandidate = 0;
	if ((candidateCount != 0 && candidates == 0) || resolver == 0)
		return false;
	for (unsigned index = 0; index != candidateCount; ++index)
	{
		if (!resolver(candidates[index].firstID,
				candidates[index].firstGeneration, context) ||
			!resolver(candidates[index].secondID,
				candidates[index].secondGeneration, context))
		{
			if (firstStaleCandidate != 0)
				*firstStaleCandidate = index;
			return false;
		}
	}
	return true;
}

void ResetCollisionCandidateRuntimeMetrics()
{
	JobMetricCounter nextEpoch = loadMetric(s_resetEpoch) + 1;
	if (nextEpoch == 0) nextEpoch = 1;
	resetMetric(s_authoritativeCommits);
	resetMetric(s_shadowExecutions);
	resetMetric(s_shadowMismatches);
	resetMetric(s_ownerFallbacks);
	resetMetric(s_unexpectedFallbacks);
	resetMetric(s_ineligibleSlices);
	resetMetric(s_staleRejections);
	resetMetric(s_committedCandidates);
	resetMetric(s_shadowComparedCandidates);
	resetMetric(s_preparedPairs);
	resetMetric(s_uniqueCandidates);
	resetMetric(s_submittedJobs);
	resetMetric(s_completedJobs);
	resetMetric(s_localSortRuns);
	resetMetric(s_locallyUniqueCandidates);
	resetMetric(s_ownerMergeComparisons);
	resetMetric(s_maximumRangeInputs);
	resetMetric(s_physicalWorkerJobs);
	resetMetric(s_ownerHelpedJobs);
	resetMetric(s_physicalWorkerMask);
	resetMetric(s_distinctPhysicalWorkers);
	resetMetric(s_physicalWorkerMaskIncomplete);
	resetMetric(s_maximumPeakConcurrentPhysicalWorkers);
	// The owner resets only at a lifecycle boundary after all kernel groups
	// have drained. Publish the new epoch last so diagnostic readers can use it
	// as the boundary marker for the cleared counters.
#if defined(_MSC_VER) && _MSC_VER < 1300
	s_resetEpoch = nextEpoch;
#else
	s_resetEpoch.store(nextEpoch, std::memory_order_release);
#endif
}

CollisionCandidateRuntimeMetrics GetCollisionCandidateRuntimeMetrics()
{
	CollisionCandidateRuntimeMetrics result;
	result.resetEpoch = loadMetric(s_resetEpoch);
	result.authoritativeCommits = loadMetric(s_authoritativeCommits);
	result.shadowExecutions = loadMetric(s_shadowExecutions);
	result.shadowMismatches = loadMetric(s_shadowMismatches);
	result.ownerFallbacks = loadMetric(s_ownerFallbacks);
	result.unexpectedFallbacks = loadMetric(s_unexpectedFallbacks);
	result.ineligibleSlices = loadMetric(s_ineligibleSlices);
	result.staleRejections = loadMetric(s_staleRejections);
	result.committedCandidates = loadMetric(s_committedCandidates);
	result.shadowComparedCandidates = loadMetric(s_shadowComparedCandidates);
	result.preparedPairs = loadMetric(s_preparedPairs);
	result.uniqueCandidates = loadMetric(s_uniqueCandidates);
	result.submittedJobs = loadMetric(s_submittedJobs);
	result.completedJobs = loadMetric(s_completedJobs);
	result.localSortRuns = loadMetric(s_localSortRuns);
	result.locallyUniqueCandidates = loadMetric(s_locallyUniqueCandidates);
	result.ownerMergeComparisons = loadMetric(s_ownerMergeComparisons);
	result.maximumRangeInputs = static_cast<unsigned>(
		loadMetric(s_maximumRangeInputs));
	result.physicalWorkerJobs = loadMetric(s_physicalWorkerJobs);
	result.ownerHelpedJobs = loadMetric(s_ownerHelpedJobs);
	result.physicalWorkerMask = loadMetric(s_physicalWorkerMask);
	result.distinctPhysicalWorkers = static_cast<unsigned>(
		loadMetric(s_distinctPhysicalWorkers));
	result.physicalWorkerMaskComplete =
		loadMetric(s_physicalWorkerMaskIncomplete) == 0;
	result.maximumPeakConcurrentPhysicalWorkers = static_cast<unsigned>(
		loadMetric(s_maximumPeakConcurrentPhysicalWorkers));
	return result;
}

void RecordCollisionCandidateOwnerCommit(bool authoritative, bool shadow,
	unsigned insertedCandidateCount)
{
	if (authoritative)
	{
		addMetric(s_authoritativeCommits, 1);
		addMetric(s_committedCandidates, insertedCandidateCount);
	}
	if (shadow)
	{
		addMetric(s_shadowExecutions, 1);
		addMetric(s_shadowComparedCandidates, insertedCandidateCount);
	}
}

void RecordCollisionCandidateShadowMismatch()
{
	addMetric(s_shadowMismatches, 1);
}

void RecordCollisionCandidateParallelWork(
	const CollisionCandidateMetrics &metrics)
{
	recordParallelWorkMetrics(metrics);
}

void RecordCollisionCandidateAcceptedParallelWork(
	const CollisionCandidateMetrics &metrics)
{
	// The owner calls this only after generation/contact validation and
	// authoritative publication. Keep the recording primitive shared so
	// accepted telemetry has exactly the same fields and atomic semantics as
	// attempted-work diagnostics.
	recordParallelWorkMetrics(metrics);
}

void RecordCollisionCandidateIneligibleSlice()
{
	addMetric(s_ineligibleSlices, 1);
}

void RecordCollisionCandidateOwnerFallback(bool stale, bool unexpected)
{
	addMetric(s_ownerFallbacks, 1);
	if (unexpected)
		addMetric(s_unexpectedFallbacks, 1);
	if (stale)
		addMetric(s_staleRejections, 1);
}
}
