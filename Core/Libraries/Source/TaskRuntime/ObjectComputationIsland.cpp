/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2026 TheSuperHackers
** SPDX-License-Identifier: GPL-3.0-or-later
*/
#include "Lib/ObjectComputationIsland.h"
#include "Lib/JobFloatingPointState.h"

#include <algorithm>
#include <math.h>
#include <new>
#include <string.h>

namespace rts
{
namespace
{
const UnsignedInt DEFAULT_MINIMUM_MODULES_PER_RANGE = 1;
const UnsignedInt DEFAULT_MINIMUM_PAIR_COUNT = 4096;
const UnsignedInt PHYSICAL_WORKER_WAIT_MILLISECONDS = 250;
const UnsignedInt MAX_CANDIDATE_PAYLOAD_BYTES = 16u * 1024u * 1024u;

bool addBytes(UnsignedInt &total, UnsignedInt count, UnsignedInt size)
{
	if (size != 0 && count > (~static_cast<UnsignedInt>(0) - total) / size)
		return false;
	total += count * size;
	return true;
}

bool broadCandidateLegacy(const SimulationReadObjectRecord &owner,
	const SimulationReadObjectRecord &candidate, float maximumDistance)
{
	if (owner.objectID == candidate.objectID) return false;
	const float deltaX = candidate.positionX - owner.positionX;
	const float deltaY = candidate.positionY - owner.positionY;
	const float deltaZ =
		(candidate.positionZ + candidate.zCenterOffset) -
		(owner.positionZ + owner.zCenterOffset);
	const float actualDistanceSquared = deltaX * deltaX + deltaY * deltaY +
		deltaZ * deltaZ;
	const float totalRadius = owner.boundingSphereRadius +
		candidate.boundingSphereRadius;
	float boundaryDistanceSquared = actualDistanceSquared;
	if (totalRadius > 0.0f)
	{
		const float actualDistance = sqrtf(actualDistanceSquared);
		const float boundaryDistance = actualDistance - totalRadius;
		boundaryDistanceSquared = boundaryDistance <= 0.0f ? 0.0f :
			boundaryDistance * boundaryDistance;
	}
	return boundaryDistanceSquared < maximumDistance * maximumDistance;
}

UnsignedInt findObjectIndex(const SimulationReadView &view,
	UnsignedInt objectID)
{
	UnsignedInt low = 0;
	UnsignedInt high = view.objectCount();
	while (low < high)
	{
		const UnsignedInt middle = low + (high - low) / 2;
		const SimulationReadObjectRecord *record = view.objectAt(middle);
		if (record->objectID < objectID) low = middle + 1;
		else high = middle;
	}
	return low < view.objectCount() &&
		view.objectAt(low)->objectID == objectID ? low :
		SIMULATION_INVALID_COMMAND_INDEX;
}

struct Producer
{
	Producer()
		: commands(0), payload(0), buffer(0), candidateIndexes(0),
		  visitStamps(0), visitStamp(0), begin(0), end(0),
		  physicalWorker(false), ownerHelped(false), jobReleased(false),
		  physicalWorkerIndex(JOB_INVALID_PHYSICAL_WORKER_INDEX),
		  emittedCandidates(0), visitedSpatialMembers(0), payloadBytes(0)
	{
	}

	~Producer()
	{
		delete buffer;
		delete[] visitStamps;
		delete[] candidateIndexes;
		delete[] payload;
		delete[] commands;
	}

	SimulationCommand *commands;
	unsigned char *payload;
	SimulationCommandBuffer *buffer;
	UnsignedInt *candidateIndexes;
	UnsignedInt *visitStamps;
	UnsignedInt visitStamp;
	UnsignedInt begin;
	UnsignedInt end;
	bool physicalWorker;
	bool ownerHelped;
	bool jobReleased;
	UnsignedInt physicalWorkerIndex;
	UnsignedInt emittedCandidates;
	UnsignedInt visitedSpatialMembers;
	UnsignedInt payloadBytes;
};

bool nextVisitStamp(Producer &producer, UnsignedInt objectCount)
{
	++producer.visitStamp;
	if (producer.visitStamp != 0) return true;
	memset(producer.visitStamps, 0, objectCount * sizeof(UnsignedInt));
	producer.visitStamp = 1;
	return true;
}

bool collectFullScan(const SimulationReadView &view,
	const SimulationReadObjectRecord &owner,
	const SimulationReadModuleRecord &module, Producer &producer,
	UnsignedInt *candidateCount, const JobContext *context)
{
	*candidateCount = 0;
	for (UnsignedInt objectIndex = 0; objectIndex < view.objectCount();
		++objectIndex)
	{
		if ((objectIndex & 63) == 0 && context != 0 &&
			context->isCancellationRequested())
			return false;
		if (broadCandidateLegacy(owner, *view.objectAt(objectIndex),
			module.queryRadius))
			producer.candidateIndexes[(*candidateCount)++] = objectIndex;
	}
	return true;
}

bool collectSpatialScan(const SimulationReadView &view,
	const SimulationReadObjectRecord &owner,
	const SimulationReadModuleRecord &module, Producer &producer,
	UnsignedInt *candidateCount, const JobContext *context)
{
	*candidateCount = 0;
	nextVisitStamp(producer, view.objectCount());
	const UnsignedInt centerX = module.spatialCellIndex % view.cellCountX();
	const UnsignedInt centerY = module.spatialCellIndex / view.cellCountX();
	const UnsignedInt radius = module.spatialCellRadius;
	const UnsignedInt minX = centerX > radius ? centerX - radius : 0;
	const UnsignedInt minY = centerY > radius ? centerY - radius : 0;
	const UnsignedInt maxX = radius < view.cellCountX() - 1 - centerX ?
		centerX + radius : view.cellCountX() - 1;
	const UnsignedInt maxY = radius < view.cellCountY() - 1 - centerY ?
		centerY + radius : view.cellCountY() - 1;
	UnsignedInt cancellationCounter = 0;
	for (UnsignedInt cellY = minY; cellY <= maxY; ++cellY)
	{
		for (UnsignedInt cellX = minX; cellX <= maxX; ++cellX)
		{
			const SimulationReadSpatialCellSpan *span =
				view.findSpatialCellSpan(cellY * view.cellCountX() + cellX);
			if (span == 0) continue;
			for (UnsignedInt member = 0; member < span->objectIndexCount;
				++member)
			{
				if ((cancellationCounter++ & 63) == 0 && context != 0 &&
					context->isCancellationRequested())
					return false;
				++producer.visitedSpatialMembers;
				const UnsignedInt objectIndex = view.spatialObjectIndexAt(
					span->objectIndexOffset + member);
				if (objectIndex >= view.objectCount()) return false;
				if (producer.visitStamps[objectIndex] == producer.visitStamp)
					continue;
				producer.visitStamps[objectIndex] = producer.visitStamp;
				if (broadCandidateLegacy(owner, *view.objectAt(objectIndex),
					module.queryRadius))
					producer.candidateIndexes[(*candidateCount)++] = objectIndex;
			}
		}
	}
	std::sort(producer.candidateIndexes,
		producer.candidateIndexes + *candidateCount);
	return true;
}

bool fillProducer(const SimulationReadView &view, Producer &producer,
	bool useSpatialIndex,
	ObjectComputationTestFault fault, UnsignedInt faultOrdinal,
	const JobContext *context)
{
	for (UnsignedInt scheduleIndex = producer.begin;
		scheduleIndex < producer.end; ++scheduleIndex)
	{
		if (context != 0 && context->isCancellationRequested()) return false;
		const SimulationReadScheduleEntry *schedule =
			view.scheduleAt(scheduleIndex);
		const SimulationReadModuleRecord *module =
			view.moduleAt(schedule->moduleIndex);
		const UnsignedInt ownerIndex = findObjectIndex(view, module->objectID);
		if (ownerIndex == SIMULATION_INVALID_COMMAND_INDEX)
		{
			producer.buffer->fail();
			return false;
		}

		UnsignedInt candidates = 0;
		const SimulationReadObjectRecord &owner = *view.objectAt(ownerIndex);
		if (!(useSpatialIndex ?
			collectSpatialScan(view, owner, *module, producer, &candidates,
				context) :
			collectFullScan(view, owner, *module, producer, &candidates,
				context)))
		{
			producer.buffer->fail();
			return false;
		}
		if (fault == OBJECT_COMPUTATION_TEST_SHADOW_MISMATCH &&
			scheduleIndex == faultOrdinal && view.objectCount() != 0)
		{
			UnsignedInt position = 0;
			while (position < candidates &&
				producer.candidateIndexes[position] != 0) ++position;
			if (position < candidates)
			{
				for (; position + 1 < candidates; ++position)
					producer.candidateIndexes[position] =
						producer.candidateIndexes[position + 1];
				--candidates;
			}
			else
			{
				for (position = candidates; position != 0; --position)
					producer.candidateIndexes[position] =
						producer.candidateIndexes[position - 1];
				producer.candidateIndexes[0] = 0;
				++candidates;
			}
		}
		if (candidates > ~static_cast<UnsignedInt>(0) /
			static_cast<UnsignedInt>(sizeof(UnsignedInt)))
			return false;
		const UnsignedInt candidateBytes = candidates *
			static_cast<UnsignedInt>(sizeof(UnsignedInt));
		if (candidateBytes > ~static_cast<UnsignedInt>(0) -
			static_cast<UnsignedInt>(sizeof(ObjectComputationCandidateSetHeader)))
			return false;
		const UnsignedInt payloadSize = static_cast<UnsignedInt>(
			sizeof(ObjectComputationCandidateSetHeader)) + candidateBytes;
		unsigned char *payload = 0;
		if (!producer.buffer->appendReserved(schedule->ownerOrder,
			SimulationStableHandle(module->objectID), SimulationStableHandle(),
			OBJECT_COMPUTATION_COMMAND_CANDIDATE_SET, payloadSize, &payload))
			return false;

		ObjectComputationCandidateSetHeader header;
		header.frame = view.frame();
		header.viewGeneration = view.generation();
		header.moduleIndex = schedule->moduleIndex;
		header.objectCount = view.objectCount();
		header.candidateCount = candidates;
		header.candidateByteCount = candidateBytes;
		memcpy(payload, &header, sizeof(header));
		if (candidateBytes != 0)
			memcpy(payload + sizeof(header), producer.candidateIndexes,
				candidateBytes);
		producer.emittedCandidates += candidates;
		producer.payloadBytes += payloadSize;
	}
	if (fault == OBJECT_COMPUTATION_TEST_PRODUCER_FAILURE &&
		producer.begin <= faultOrdinal && faultOrdinal < producer.end)
	{
		producer.buffer->fail();
		return false;
	}
	return producer.buffer->complete();
}

class ComputationJob : public Job
{
public:
	ComputationJob(const SimulationReadView &view, Producer &producer,
		const JobFloatingPointState &floatingPointState,
		ObjectComputationTestFault fault, UnsignedInt faultOrdinal)
		: m_view(view), m_producer(producer), m_fault(fault),
		  m_faultOrdinal(faultOrdinal),
		  m_floatingPointState(floatingPointState)
	{
	}
	virtual ~ComputationJob() { m_producer.jobReleased = true; }

	virtual void execute(JobContext &context)
	{
		const JobFloatingPointScope floatingPointScope(m_floatingPointState);
		if (m_fault == OBJECT_COMPUTATION_TEST_OWNER_HELP_ONLY)
		{
			m_producer.physicalWorker = false;
			m_producer.ownerHelped = true;
			m_producer.physicalWorkerIndex =
				JOB_INVALID_PHYSICAL_WORKER_INDEX;
		}
		else
		{
			m_producer.physicalWorker =
				context.isPhysicalWorkerExecution();
			m_producer.ownerHelped = !m_producer.physicalWorker;
			m_producer.physicalWorkerIndex = m_producer.physicalWorker ?
				context.physicalWorkerIndex() :
				JOB_INVALID_PHYSICAL_WORKER_INDEX;
		}
		if (context.isCancellationRequested() ||
		!fillProducer(m_view, m_producer, true, m_fault, m_faultOrdinal,
				&context))
			context.fail();
	}

private:
	const SimulationReadView &m_view;
	Producer &m_producer;
	ObjectComputationTestFault m_fault;
	UnsignedInt m_faultOrdinal;
	JobFloatingPointState m_floatingPointState;
};
}

struct ObjectComputationIsland::State
{
	State()
		: producers(0), producerCount(0), merged(0), scratch(0),
		  mergedCount(0), view(0)
	{
	}

	~State() { clear(); }

	void clear()
	{
		if (producers != 0)
		{
			for (UnsignedInt index = 0; index < producerCount; ++index)
				delete producers[index];
		}
		delete[] producers;
		delete[] scratch;
		delete[] merged;
		producers = 0;
		producerCount = 0;
		merged = 0;
		scratch = 0;
		mergedCount = 0;
		view = 0;
	}

	Producer **producers;
	UnsignedInt producerCount;
	SimulationMergedCommand *merged;
	SimulationMergedCommand *scratch;
	UnsignedInt mergedCount;
	const SimulationReadView *view;
};

ObjectComputationOptions::ObjectComputationOptions()
	: parallel(false),
	  minimumModulesPerRange(DEFAULT_MINIMUM_MODULES_PER_RANGE),
	  minimumPairCount(DEFAULT_MINIMUM_PAIR_COUNT),
	  testFault(OBJECT_COMPUTATION_TEST_NONE), testOrdinal(0)
{
}

ObjectComputationMetrics::ObjectComputationMetrics()
	: objectCount(0), moduleCount(0), pairCount(0), rangeCount(0),
	  submittedJobs(0), completedJobs(0), schedulerReleasedJobs(0),
	  physicalWorkerJobs(0), distinctPhysicalWorkers(0),
	  ownerHelpedJobs(0), physicalWaitTimeouts(0), emittedCommands(0),
	  emittedCandidates(0), visitedSpatialMembers(0),
	  candidatePayloadBytes(0), spatialCellSpans(0),
	  spatialMemberships(0), allocatedBytes(0),
	  arenaBudgetBytes(0), arenaAllocations(0), serialFallbacks(0)
{
}

ObjectComputationResult PreflightObjectComputationIsland(
	const ObjectComputationOptions &options, ObjectComputationMetrics *metrics)
{
	ObjectComputationMetrics localMetrics;
	if (metrics == 0) metrics = &localMetrics;
	*metrics = ObjectComputationMetrics();
	if (!options.parallel) return OBJECT_COMPUTATION_SERIAL_REFERENCE;

	JobSystem &jobs = JobSystem::instance();
	if (!jobs.isRunning() || jobs.isWorkerThread() ||
		!jobs.isCurrentThread(JOB_OWNER_GAME))
	{
		++metrics->serialFallbacks;
		jobs.recordSerialFallback();
		return OBJECT_COMPUTATION_SERIAL_FALLBACK;
	}
	if (jobs.workerCount() <= 1)
		return OBJECT_COMPUTATION_POLICY_INELIGIBLE;
	return OBJECT_COMPUTATION_PARALLEL;
}

ObjectComputationIsland::ObjectComputationIsland()
	: m_state(new (std::nothrow) State)
{
}

ObjectComputationIsland::~ObjectComputationIsland() { delete m_state; }

void ObjectComputationIsland::reset()
{
	if (m_state != 0) m_state->clear();
}

UnsignedInt ObjectComputationIsland::commandCount() const
{
	return m_state != 0 ? m_state->mergedCount : 0;
}

const SimulationMergedCommand *ObjectComputationIsland::commandAt(
	UnsignedInt index) const
{
	return m_state != 0 && index < m_state->mergedCount ?
		m_state->merged + index : 0;
}

ObjectComputationResult ObjectComputationIsland::prepare(
	const SimulationReadView &view, const ObjectComputationOptions &options,
	ObjectComputationMetrics *metrics)
{
	ObjectComputationMetrics localMetrics;
	if (metrics == 0) metrics = &localMetrics;
	*metrics = ObjectComputationMetrics();
	metrics->objectCount = view.objectCount();
	metrics->moduleCount = view.moduleCount();
	metrics->spatialCellSpans = view.spatialCellSpanCount();
	metrics->spatialMemberships = view.spatialObjectIndexCount();
	if (m_state == 0) return OBJECT_COMPUTATION_INVALID_INPUT;
	m_state->clear();
	if (!view.isValid() ||
		view.moduleCount() > ~static_cast<UnsignedInt>(0) / view.objectCount())
		return OBJECT_COMPUTATION_INVALID_INPUT;
	metrics->pairCount = view.moduleCount() * view.objectCount();
	for (UnsignedInt moduleIndex = 1; moduleIndex < view.moduleCount();
		++moduleIndex)
		if (view.moduleAt(moduleIndex)->moduleType !=
			view.moduleAt(0)->moduleType)
			return OBJECT_COMPUTATION_INVALID_INPUT;

	m_state->view = &view;
	UnsignedInt rangeCount = 1;
	JobSystem *jobs = 0;
	if (options.parallel)
	{
		if (!view.hasSpatialIndex()) return OBJECT_COMPUTATION_INVALID_INPUT;
		if (metrics->pairCount < options.minimumPairCount)
			return OBJECT_COMPUTATION_POLICY_INELIGIBLE;
		ObjectComputationMetrics preflightMetrics;
		const ObjectComputationResult preflight =
			PreflightObjectComputationIsland(options, &preflightMetrics);
		if (preflight != OBJECT_COMPUTATION_PARALLEL)
		{
			metrics->serialFallbacks = preflightMetrics.serialFallbacks;
			return preflight;
		}
		jobs = &JobSystem::instance();
		const UnsignedInt grain = options.minimumModulesPerRange != 0 ?
			options.minimumModulesPerRange : 1;
		rangeCount = JobSystem::chooseRangeCount(view.scheduleCount(), grain,
			jobs->workerCount());
		if (rangeCount <= 1) return OBJECT_COMPUTATION_POLICY_INELIGIBLE;
	}
	metrics->rangeCount = rangeCount;

	m_state->producers = new (std::nothrow) Producer *[rangeCount];
	m_state->merged = new (std::nothrow)
		SimulationMergedCommand[view.moduleCount()];
	m_state->scratch = new (std::nothrow)
		SimulationMergedCommand[view.moduleCount()];
	if (m_state->producers == 0 || m_state->merged == 0 ||
		m_state->scratch == 0)
	{
		m_state->clear();
		if (options.parallel)
		{
			++metrics->serialFallbacks;
			jobs->recordSerialFallback();
			return OBJECT_COMPUTATION_SERIAL_FALLBACK;
		}
		return OBJECT_COMPUTATION_INVALID_INPUT;
	}
	m_state->producerCount = rangeCount;
	for (UnsignedInt init = 0; init < rangeCount; ++init)
		m_state->producers[init] = 0;

	UnsignedInt worstPayloadPerSet = MAX_CANDIDATE_PAYLOAD_BYTES;
	UnsignedInt totalPayloadBudget = MAX_CANDIDATE_PAYLOAD_BYTES;
	bool exactWorstCaseBudget = false;
	if (view.objectCount() <=
		(~static_cast<UnsignedInt>(0) - static_cast<UnsignedInt>(
			sizeof(ObjectComputationCandidateSetHeader))) /
		static_cast<UnsignedInt>(sizeof(UnsignedInt)))
	{
		worstPayloadPerSet = static_cast<UnsignedInt>(
			sizeof(ObjectComputationCandidateSetHeader)) +
			view.objectCount() * static_cast<UnsignedInt>(sizeof(UnsignedInt));
		if (view.moduleCount() <= MAX_CANDIDATE_PAYLOAD_BYTES /
			worstPayloadPerSet)
		{
			totalPayloadBudget = view.moduleCount() * worstPayloadPerSet;
			exactWorstCaseBudget = true;
		}
	}
	metrics->arenaBudgetBytes = totalPayloadBudget;
	for (UnsignedInt producerIndex = 0; producerIndex < rangeCount;
		++producerIndex)
	{
		JobRange range;
		if (!JobSystem::rangeForIndex(view.scheduleCount(), rangeCount,
			producerIndex, range))
		{
			m_state->clear();
			return OBJECT_COMPUTATION_INVALID_INPUT;
		}
		Producer *producer = new (std::nothrow) Producer;
		m_state->producers[producerIndex] = producer;
		if (producer == 0)
		{
			m_state->clear();
			if (!options.parallel) return OBJECT_COMPUTATION_INVALID_INPUT;
			++metrics->serialFallbacks;
			jobs->recordSerialFallback();
			return OBJECT_COMPUTATION_SERIAL_FALLBACK;
		}
		producer->begin = range.begin;
		producer->end = range.end;
		const UnsignedInt commandCapacity = range.end - range.begin;
		if (commandCapacity > ~static_cast<UnsignedInt>(0) /
			static_cast<UnsignedInt>(sizeof(ObjectComputationCandidateSetHeader)))
		{
			m_state->clear();
			return OBJECT_COMPUTATION_INVALID_INPUT;
		}
		UnsignedInt payloadCapacity = exactWorstCaseBudget ?
			commandCapacity * worstPayloadPerSet :
			totalPayloadBudget / rangeCount;
		if (!exactWorstCaseBudget &&
			producerIndex < totalPayloadBudget % rangeCount)
			++payloadCapacity;
		const UnsignedInt minimumPayloadCapacity = commandCapacity *
			static_cast<UnsignedInt>(sizeof(ObjectComputationCandidateSetHeader));
		if (options.testFault ==
			OBJECT_COMPUTATION_TEST_PAYLOAD_BUDGET_FAILURE &&
			producerIndex == options.testOrdinal)
			payloadCapacity = minimumPayloadCapacity != 0 ?
				minimumPayloadCapacity - 1 : 0;
		if (payloadCapacity < minimumPayloadCapacity)
		{
			m_state->clear();
			if (!options.parallel) return OBJECT_COMPUTATION_INVALID_INPUT;
			++metrics->serialFallbacks;
			jobs->recordSerialFallback();
			return OBJECT_COMPUTATION_SERIAL_FALLBACK;
		}
		producer->commands = new (std::nothrow)
			SimulationCommand[commandCapacity];
		producer->payload = new (std::nothrow) unsigned char[payloadCapacity];
		producer->candidateIndexes = new (std::nothrow)
			UnsignedInt[view.objectCount()];
		producer->visitStamps = new (std::nothrow)
			UnsignedInt[view.objectCount()];
		producer->buffer = new (std::nothrow) SimulationCommandBuffer(
			producer->commands, commandCapacity, producer->payload,
			payloadCapacity, producerIndex, view.moduleAt(0)->moduleType);
		if (producer->commands == 0 || producer->payload == 0 ||
			producer->candidateIndexes == 0 || producer->visitStamps == 0 ||
			producer->buffer == 0)
		{
			m_state->clear();
			if (!options.parallel) return OBJECT_COMPUTATION_INVALID_INPUT;
			++metrics->serialFallbacks;
			jobs->recordSerialFallback();
			return OBJECT_COMPUTATION_SERIAL_FALLBACK;
		}
		UnsignedInt producerBytes = 0;
		if (!addBytes(producerBytes, commandCapacity,
			static_cast<UnsignedInt>(sizeof(SimulationCommand))) ||
			!addBytes(producerBytes, payloadCapacity, 1) ||
			!addBytes(producerBytes, view.objectCount(),
				static_cast<UnsignedInt>(2 * sizeof(UnsignedInt))))
		{
			m_state->clear();
			return OBJECT_COMPUTATION_INVALID_INPUT;
		}
		if (producerBytes > ~static_cast<UnsignedInt>(0) -
			metrics->allocatedBytes)
		{
			m_state->clear();
			return OBJECT_COMPUTATION_INVALID_INPUT;
		}
		metrics->allocatedBytes += producerBytes;
		metrics->arenaAllocations += 4;
		memset(producer->visitStamps, 0,
			view.objectCount() * sizeof(UnsignedInt));
	}
	const UnsignedInt mergeBytes = view.moduleCount() *
		static_cast<UnsignedInt>(2 * sizeof(SimulationMergedCommand));
	if (mergeBytes > ~static_cast<UnsignedInt>(0) - metrics->allocatedBytes)
	{
		m_state->clear();
		return OBJECT_COMPUTATION_INVALID_INPUT;
	}
	metrics->allocatedBytes += mergeBytes;

	if (!options.parallel)
	{
		if (!fillProducer(view, *m_state->producers[0], false, options.testFault,
			options.testOrdinal, 0))
		{
			m_state->mergedCount = 0;
			return OBJECT_COMPUTATION_INVALID_INPUT;
		}
	}
	else
	{
		JobSubmission *submissions = new (std::nothrow)
			JobSubmission[rangeCount];
		JobHandle *handles = new (std::nothrow) JobHandle[rangeCount];
		ComputationJob **work = new (std::nothrow) ComputationJob *[rangeCount];
		if (submissions == 0 || handles == 0 || work == 0)
		{
			delete[] work;
			delete[] handles;
			delete[] submissions;
			m_state->mergedCount = 0;
			++metrics->serialFallbacks;
			jobs->recordSerialFallback();
			return OBJECT_COMPUTATION_SERIAL_FALLBACK;
		}
		for (UnsignedInt workInit = 0; workInit < rangeCount; ++workInit)
			work[workInit] = 0;
		const JobFloatingPointState floatingPointState;
		bool workReady = true;
		for (UnsignedInt workIndex = 0; workIndex < rangeCount; ++workIndex)
		{
			work[workIndex] = new (std::nothrow) ComputationJob(view,
				*m_state->producers[workIndex], floatingPointState,
				options.testFault, options.testOrdinal);
			if (work[workIndex] == 0) { workReady = false; break; }
			submissions[workIndex].job = work[workIndex];
			submissions[workIndex].priority = JOB_PRIORITY_FRAME_CRITICAL;
		}
		JobGroup group;
		if (options.testFault != OBJECT_COMPUTATION_TEST_GROUP_FAILURE)
			group = jobs->createGroup();
		bool admitted = workReady && group.isValid() &&
			options.testFault != OBJECT_COMPUTATION_TEST_ADMISSION_FAILURE &&
			jobs->trySubmitBatch(submissions, rangeCount, group, handles);
		if (!admitted)
		{
			for (UnsignedInt cleanup = 0; cleanup < rangeCount; ++cleanup)
				delete work[cleanup];
			delete[] work;
			delete[] handles;
			delete[] submissions;
			m_state->mergedCount = 0;
			++metrics->serialFallbacks;
			jobs->recordSerialFallback();
			return OBJECT_COMPUTATION_SERIAL_FALLBACK;
		}
		metrics->submittedJobs = rangeCount;
		if (options.testFault == OBJECT_COMPUTATION_TEST_CANCEL_AFTER_ADMISSION)
			jobs->cancel(group);
		const bool physicalCompletion = options.testFault !=
			OBJECT_COMPUTATION_TEST_PHYSICAL_WAIT_TIMEOUT &&
			jobs->waitWithoutOwnerHelp(group,
				PHYSICAL_WORKER_WAIT_MILLISECONDS);
		if (!physicalCompletion)
		{
			++metrics->physicalWaitTimeouts;
			jobs->cancel(group);
		}
		// Timeout/cancellation cannot release immutable view or producer storage
		// until every admitted job observes cancellation and leaves the group.
		jobs->wait(group);
		bool succeeded = !group.failed() && !group.wasCancelled();
		for (UnsignedInt completion = 0; completion < rangeCount; ++completion)
		{
			if (handles[completion].succeeded()) ++metrics->completedJobs;
			else succeeded = false;
			if (m_state->producers[completion]->jobReleased)
				++metrics->schedulerReleasedJobs;
			else
				succeeded = false;
			if (m_state->producers[completion]->physicalWorker)
			{
				++metrics->physicalWorkerJobs;
				Bool firstUse = TRUE;
				for (UnsignedInt prior = 0; prior != completion; ++prior)
					if (m_state->producers[prior]->physicalWorker &&
						m_state->producers[prior]->physicalWorkerIndex ==
						m_state->producers[completion]->physicalWorkerIndex)
					{
						firstUse = FALSE;
						break;
					}
				if (firstUse) ++metrics->distinctPhysicalWorkers;
			}
			if (m_state->producers[completion]->ownerHelped)
				++metrics->ownerHelpedJobs;
		}
		// Successful admission transfers Job ownership to JobSystem. Each job
		// is destroyed before its group completion is published; work now holds
		// dangling observation-only values and must never delete them here.
		delete[] work;
		delete[] handles;
		delete[] submissions;
		const bool scalablePhysicalExecution =
			metrics->physicalWorkerJobs != 0 &&
			metrics->distinctPhysicalWorkers > 1 &&
			metrics->ownerHelpedJobs != metrics->submittedJobs;
		if (!succeeded || metrics->completedJobs != metrics->submittedJobs ||
			!scalablePhysicalExecution)
		{
			m_state->mergedCount = 0;
			++metrics->serialFallbacks;
			jobs->recordSerialFallback();
			return OBJECT_COMPUTATION_SERIAL_FALLBACK;
		}
	}

	const SimulationCommandBuffer **buffers = new (std::nothrow)
		const SimulationCommandBuffer *[rangeCount];
	if (buffers == 0)
	{
		m_state->mergedCount = 0;
		if (!options.parallel) return OBJECT_COMPUTATION_INVALID_INPUT;
		++metrics->serialFallbacks;
		jobs->recordSerialFallback();
		return OBJECT_COMPUTATION_SERIAL_FALLBACK;
	}
	for (UnsignedInt bufferIndex = 0; bufferIndex < rangeCount; ++bufferIndex)
		buffers[bufferIndex] = m_state->producers[bufferIndex]->buffer;
	const SimulationCommandMergeResult merge = MergeSimulationCommandSlots(
		buffers, rangeCount, m_state->merged, m_state->scratch,
		view.moduleCount());
	delete[] buffers;
	if (!merge.succeeded() || merge.commandCount != view.moduleCount())
	{
		m_state->mergedCount = 0;
		if (options.parallel)
		{
			++metrics->serialFallbacks;
			jobs->recordSerialFallback();
			return OBJECT_COMPUTATION_SERIAL_FALLBACK;
		}
		return OBJECT_COMPUTATION_INVALID_INPUT;
	}
	m_state->mergedCount = merge.commandCount;
	metrics->emittedCommands = merge.commandCount;
	for (UnsignedInt emitted = 0; emitted < rangeCount; ++emitted)
	{
		metrics->emittedCandidates +=
			m_state->producers[emitted]->emittedCandidates;
		metrics->visitedSpatialMembers +=
			m_state->producers[emitted]->visitedSpatialMembers;
		metrics->candidatePayloadBytes +=
			m_state->producers[emitted]->payloadBytes;
	}
	return options.parallel ? OBJECT_COMPUTATION_PARALLEL :
		OBJECT_COMPUTATION_SERIAL_REFERENCE;
}

bool DecodeObjectComputationCandidateSet(const SimulationReadView &view,
	const SimulationMergedCommand &merged,
	ObjectComputationCandidateSetHeader *header)
{
	if (header == 0 || merged.command() == 0 ||
		merged.command()->commandType() !=
			OBJECT_COMPUTATION_COMMAND_CANDIDATE_SET ||
		merged.payload() == 0 || merged.command()->payloadSize() <
			sizeof(ObjectComputationCandidateSetHeader))
		return false;
	memcpy(header, merged.payload(), sizeof(*header));
	if (header->frame != view.frame() ||
		header->viewGeneration != view.generation() ||
		header->moduleIndex >= view.moduleCount() ||
		header->objectCount != view.objectCount() ||
		header->candidateCount > view.objectCount() ||
		header->candidateCount > ~static_cast<UnsignedInt>(0) /
			static_cast<UnsignedInt>(sizeof(UnsignedInt)) ||
		header->candidateByteCount != header->candidateCount *
			static_cast<UnsignedInt>(sizeof(UnsignedInt)) ||
		merged.command()->payloadSize() != sizeof(*header) +
			header->candidateByteCount ||
		merged.command()->orderKey().target().objectID() !=
			view.moduleAt(header->moduleIndex)->objectID ||
		merged.command()->orderKey().moduleType() !=
			view.moduleAt(header->moduleIndex)->moduleType)
		return false;
	UnsignedInt prior = 0;
	for (UnsignedInt ordinal = 0; ordinal < header->candidateCount; ++ordinal)
	{
		UnsignedInt objectIndex = 0;
		if (!ObjectComputationCandidateIndexAt(view, merged, ordinal,
			&objectIndex) || (ordinal != 0 && prior >= objectIndex))
			return false;
		prior = objectIndex;
	}
	return true;
}

bool ObjectComputationCandidateAt(const SimulationReadView &view,
	const SimulationMergedCommand &merged, UnsignedInt objectIndex)
{
	if (objectIndex >= view.objectCount() || merged.command() == 0 ||
		merged.payload() == 0 || merged.command()->payloadSize() <
			sizeof(ObjectComputationCandidateSetHeader))
		return false;
	ObjectComputationCandidateSetHeader header;
	memcpy(&header, merged.payload(), sizeof(header));
	UnsignedInt low = 0;
	UnsignedInt high = header.candidateCount;
	while (low < high)
	{
		const UnsignedInt middle = low + (high - low) / 2;
		UnsignedInt candidateIndex = 0;
		if (!ObjectComputationCandidateIndexAt(view, merged, middle,
			&candidateIndex)) return false;
		if (candidateIndex < objectIndex) low = middle + 1;
		else high = middle;
	}
	UnsignedInt found = 0;
	return low < header.candidateCount &&
		ObjectComputationCandidateIndexAt(view, merged, low, &found) &&
		found == objectIndex;
}

bool ObjectComputationCandidateIndexAt(const SimulationReadView &view,
	const SimulationMergedCommand &merged, UnsignedInt candidateOrdinal,
	UnsignedInt *objectIndex)
{
	if (objectIndex == 0 || merged.command() == 0 || merged.payload() == 0 ||
		merged.command()->payloadSize() <
			sizeof(ObjectComputationCandidateSetHeader))
		return false;
	ObjectComputationCandidateSetHeader header;
	memcpy(&header, merged.payload(), sizeof(header));
	if (candidateOrdinal >= header.candidateCount ||
		header.candidateCount > ~static_cast<UnsignedInt>(0) /
			static_cast<UnsignedInt>(sizeof(UnsignedInt)) ||
		header.candidateByteCount != header.candidateCount *
			static_cast<UnsignedInt>(sizeof(UnsignedInt)) ||
		merged.command()->payloadSize() != sizeof(header) +
			header.candidateByteCount)
		return false;
	memcpy(objectIndex, merged.payload() + sizeof(header) +
		candidateOrdinal * sizeof(UnsignedInt), sizeof(UnsignedInt));
	return *objectIndex < view.objectCount();
}

bool ObjectComputationCommandsEqual(const SimulationReadView &view,
	const ObjectComputationIsland &left,
	const ObjectComputationIsland &right, UnsignedInt *firstDifference)
{
	if (firstDifference != 0) *firstDifference = 0;
	if (left.commandCount() != right.commandCount()) return false;
	for (UnsignedInt index = 0; index < left.commandCount(); ++index)
	{
		const SimulationMergedCommand *leftCommand = left.commandAt(index);
		const SimulationMergedCommand *rightCommand = right.commandAt(index);
		ObjectComputationCandidateSetHeader leftHeader;
		ObjectComputationCandidateSetHeader rightHeader;
		if (leftCommand == 0 || rightCommand == 0 ||
			!DecodeObjectComputationCandidateSet(view, *leftCommand, &leftHeader) ||
			!DecodeObjectComputationCandidateSet(view, *rightCommand,
				&rightHeader) ||
			leftCommand->command()->orderKey().phase() !=
				rightCommand->command()->orderKey().phase() ||
			leftCommand->command()->orderKey().target().objectID() !=
				rightCommand->command()->orderKey().target().objectID() ||
			leftCommand->command()->orderKey().source().objectID() !=
				rightCommand->command()->orderKey().source().objectID() ||
			leftCommand->command()->orderKey().moduleType() !=
				rightCommand->command()->orderKey().moduleType() ||
			leftCommand->command()->commandType() !=
				rightCommand->command()->commandType() ||
			leftCommand->command()->payloadSize() !=
				rightCommand->command()->payloadSize() ||
			memcmp(leftCommand->payload(), rightCommand->payload(),
				leftCommand->command()->payloadSize()) != 0)
		{
			if (firstDifference != 0) *firstDifference = index;
			return false;
		}
	}
	if (firstDifference != 0) *firstDifference = left.commandCount();
	return true;
}
}
