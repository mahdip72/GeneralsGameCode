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
#include <string.h>

namespace rts
{
CollisionCandidateOptions::CollisionCandidateOptions()
	: parallel(false),
	  minimumGrain(COLLISION_CANDIDATE_DEFAULT_MINIMUM_GRAIN),
	  order(COLLISION_CANDIDATE_REVERSE_DISCOVERY),
	  cancellationGroup(0)
{
}

CollisionCandidateMetrics::CollisionCandidateMetrics()
	: preparedPairs(0), uniqueCandidates(0), submittedJobs(0),
	  completedJobs(0), serialFallbacks(0), localSortRuns(0),
	  locallyUniqueCandidates(0), ownerMergeComparisons(0),
	  maximumRangeInputs(0), physicalWorkerJobs(0), ownerHelpedJobs(0),
	  physicalWorkerMask(0), distinctPhysicalWorkers(0),
	  physicalWorkerMaskComplete(true)

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
	  physicalWorkerMaskComplete(true)
{
}

namespace
{
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

void normalizeRange(const CollisionCandidateInput *inputs,
	CollisionCandidate *scratch, unsigned begin, unsigned end,
	JobContext *context)
{
	for (unsigned index = begin; index != end; ++index)
	{
		if (context != 0 && (index - begin) % 256 == 0 &&
			context->isCancellationRequested())
			return;
		CollisionCandidate &candidate = scratch[index];
		candidate.firstID = inputs[index].firstID;
		candidate.secondID = inputs[index].secondID;
		candidate.firstGeneration = inputs[index].firstGeneration;
		candidate.secondGeneration = inputs[index].secondGeneration;
		candidate.discoveryOrder = inputs[index].discoveryOrder;
		MakeCollisionCandidateKey(candidate.firstID, candidate.secondID,
			candidate.key);
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
};

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
	for (unsigned rangeIndex = 0; rangeIndex != rangeCount; ++rangeIndex)
	{
		if (ranges[rangeIndex].uniqueCount == 0)
			continue;
		unsigned heapPosition = heapSize++;
		ranges[heapPosition].heapRange = rangeIndex;
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
	for (unsigned index = 0; index != outputCount; ++index)
	{
		const unsigned discoveryOrder = output[index].discoveryOrder;
		if (discoveryOrder >= inputCount ||
			scratch[discoveryOrder].key.lowID != 0)
		{
			boundedUniqueOrders = false;
			break;
		}
		scratch[discoveryOrder] = output[index];
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
		CollisionCandidate *scratch, CollisionCandidateRangeState *range)
		: m_inputs(inputs), m_scratch(scratch), m_range(range)
	{
	}

	virtual void execute(JobContext &context)
	{
		if (context.isCancellationRequested())
			return;
		normalizeRange(m_inputs, m_scratch, m_range->begin, m_range->end,
			&context);
		if (context.isCancellationRequested())
			return;
		m_range->uniqueCount = sortAndDeduplicateRange(m_scratch,
			m_range->begin, m_range->end);
		if (context.isCancellationRequested())
			return;
		m_range->physicalWorker = context.isPhysicalWorkerExecution();
		m_range->physicalWorkerIndex = context.physicalWorkerIndex();
		m_range->completed = true;
	}

private:
	const CollisionCandidateInput *m_inputs;
	CollisionCandidate *m_scratch;
	CollisionCandidateRangeState *m_range;
};

void normalizePartitionRange(
	const PartitionCollisionObjectSnapshot *owner,
	const PartitionCollisionOccupantSnapshot *occupants,
	CollisionCandidate *scratch, unsigned begin, unsigned end,
	JobContext *context)
{
	for (unsigned index = begin; index != end; ++index)
	{
		if (context != 0 && (index - begin) % 256 == 0 &&
			context->isCancellationRequested())
			return;
		CollisionCandidate &candidate = scratch[index];
		candidate.firstID = owner->objectID;
		candidate.secondID = occupants[index].objectID;
		candidate.firstGeneration = owner->generation;
		candidate.secondGeneration = occupants[index].generation;
		candidate.discoveryOrder = index;
		MakeCollisionCandidateKey(candidate.firstID, candidate.secondID,
			candidate.key);
	}
}

class PartitionCollisionCandidateJob : public Job
{
public:
	PartitionCollisionCandidateJob(
		const PartitionCollisionObjectSnapshot *owner,
		const PartitionCollisionOccupantSnapshot *occupants,
		CollisionCandidate *scratch, CollisionCandidateRangeState *range)
		: m_owner(owner), m_occupants(occupants), m_scratch(scratch),
		  m_range(range)
	{
	}

	virtual void execute(JobContext &context)
	{
		if (context.isCancellationRequested())
			return;
		normalizePartitionRange(m_owner, m_occupants, m_scratch,
			m_range->begin, m_range->end, &context);
		if (context.isCancellationRequested())
			return;
		m_range->uniqueCount = sortAndDeduplicateRange(m_scratch,
			m_range->begin, m_range->end);
		if (context.isCancellationRequested())
			return;
		m_range->physicalWorker = context.isPhysicalWorkerExecution();
		m_range->physicalWorkerIndex = context.physicalWorkerIndex();
		m_range->completed = true;
	}

private:
	const PartitionCollisionObjectSnapshot *m_owner;
	const PartitionCollisionOccupantSnapshot *m_occupants;
	CollisionCandidate *m_scratch;
	CollisionCandidateRangeState *m_range;
};

CollisionCandidateRuntimeMetrics s_runtimeMetrics;

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
	unsigned submitted = 0;
	bool accepted = true;
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
		CollisionCandidateJob *job = new (std::nothrow)
			CollisionCandidateJob(inputs, scratch, ranges + submitted);
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
	if (!accepted)
	{
		jobs.cancel(group);
		jobs.wait(group);
		delete[] ranges;
		++metrics->serialFallbacks;
		jobs.recordSerialFallback();
		return COLLISION_CANDIDATE_SERIAL_FALLBACK;
	}

	jobs.wait(group);
	collectRangeMetrics(ranges, jobCount, metrics);
	if (cancelled(options) || group.wasCancelled())
	{
		delete[] ranges;
		return COLLISION_CANDIDATE_CANCELLED;
	}
	if (group.failed() || metrics->completedJobs != metrics->submittedJobs)
	{
		delete[] ranges;
		++metrics->serialFallbacks;
		jobs.recordSerialFallback();
		return COLLISION_CANDIDATE_SERIAL_FALLBACK;
	}

	const unsigned preparedCount = mergeSortedRanges(scratch, ranges, jobCount,
		output, metrics);
	orderMergedCandidates(output, preparedCount, scratch, inputCount,
		options.order);
	*outputCount = preparedCount;
	metrics->preparedPairs = inputCount;
	metrics->uniqueCandidates = preparedCount;
	delete[] ranges;
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
	unsigned submitted = 0;
	bool accepted = true;
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
		PartitionCollisionCandidateJob *job = new (std::nothrow)
			PartitionCollisionCandidateJob(&owner, occupants, scratch,
				ranges + submitted);
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
	if (!accepted)
	{
		jobs.cancel(group);
		jobs.wait(group);
		delete[] ranges;
		++metrics->serialFallbacks;
		jobs.recordSerialFallback();
		return COLLISION_CANDIDATE_SERIAL_FALLBACK;
	}

	{
		rts::frame_timing::Scope partitionWaitTiming(
			rts::frame_timing::SimulationWait);
		jobs.wait(group);
	}
	collectRangeMetrics(ranges, jobCount, metrics);
	if (cancelled(options) || group.wasCancelled())
	{
		delete[] ranges;
		return COLLISION_CANDIDATE_CANCELLED;
	}
	if (group.failed() || metrics->completedJobs != metrics->submittedJobs)
	{
		delete[] ranges;
		++metrics->serialFallbacks;
		jobs.recordSerialFallback();
		return COLLISION_CANDIDATE_SERIAL_FALLBACK;
	}

	unsigned preparedCount = 0;
	{
		rts::frame_timing::Scope partitionReduceTiming(
			rts::frame_timing::SimulationReduce);
		preparedCount = mergeSortedRanges(scratch, ranges, jobCount, output,
			metrics);
		orderMergedCandidates(output, preparedCount, scratch, occupantCount,
			options.order);
	}
	*outputCount = preparedCount;
	metrics->preparedPairs = occupantCount;
	metrics->uniqueCandidates = preparedCount;
	delete[] ranges;
	return COLLISION_CANDIDATE_PARALLEL;
}

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
	JobMetricCounter nextEpoch = s_runtimeMetrics.resetEpoch + 1;
	if (nextEpoch == 0) nextEpoch = 1;
	s_runtimeMetrics = CollisionCandidateRuntimeMetrics();
	s_runtimeMetrics.resetEpoch = nextEpoch;
}

CollisionCandidateRuntimeMetrics GetCollisionCandidateRuntimeMetrics()
{
	return s_runtimeMetrics;
}

void RecordCollisionCandidateOwnerCommit(bool authoritative, bool shadow,
	unsigned insertedCandidateCount)
{
	if (authoritative)
	{
		++s_runtimeMetrics.authoritativeCommits;
		s_runtimeMetrics.committedCandidates += insertedCandidateCount;
	}
	if (shadow)
	{
		++s_runtimeMetrics.shadowExecutions;
		s_runtimeMetrics.shadowComparedCandidates += insertedCandidateCount;
	}
}

void RecordCollisionCandidateShadowMismatch()
{
	++s_runtimeMetrics.shadowMismatches;
}

void RecordCollisionCandidateParallelWork(
	const CollisionCandidateMetrics &metrics)
{
	s_runtimeMetrics.preparedPairs += metrics.preparedPairs;
	s_runtimeMetrics.uniqueCandidates += metrics.uniqueCandidates;
	s_runtimeMetrics.submittedJobs += metrics.submittedJobs;
	s_runtimeMetrics.completedJobs += metrics.completedJobs;
	s_runtimeMetrics.localSortRuns += metrics.localSortRuns;
	s_runtimeMetrics.locallyUniqueCandidates +=
		metrics.locallyUniqueCandidates;
	s_runtimeMetrics.ownerMergeComparisons += metrics.ownerMergeComparisons;
	if (metrics.maximumRangeInputs > s_runtimeMetrics.maximumRangeInputs)
		s_runtimeMetrics.maximumRangeInputs = metrics.maximumRangeInputs;
	s_runtimeMetrics.physicalWorkerJobs += metrics.physicalWorkerJobs;
	s_runtimeMetrics.ownerHelpedJobs += metrics.ownerHelpedJobs;
	s_runtimeMetrics.physicalWorkerMask |= metrics.physicalWorkerMask;
	if (metrics.distinctPhysicalWorkers >
		s_runtimeMetrics.distinctPhysicalWorkers)
	{
		s_runtimeMetrics.distinctPhysicalWorkers =
			metrics.distinctPhysicalWorkers;
	}
	if (!metrics.physicalWorkerMaskComplete)
		s_runtimeMetrics.physicalWorkerMaskComplete = false;
}

void RecordCollisionCandidateIneligibleSlice()
{
	++s_runtimeMetrics.ineligibleSlices;
}

void RecordCollisionCandidateOwnerFallback(bool stale, bool unexpected)
{
	++s_runtimeMetrics.ownerFallbacks;
	if (unexpected)
		++s_runtimeMetrics.unexpectedFallbacks;
	if (stale)
		++s_runtimeMetrics.staleRejections;
}
}
