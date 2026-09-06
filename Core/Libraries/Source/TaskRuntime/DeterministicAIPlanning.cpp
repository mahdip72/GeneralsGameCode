#include "Lib/DeterministicAIPlanning.h"
#include "Lib/JobFloatingPointState.h"
#include "Lib/JobSystem.h"

#include <atomic>
#include <memory>
#include <new>
#include <string.h>
#if defined(_WIN64)
#include <thread>
#endif

namespace rts
{
namespace
{
#if defined(_WIN64)
void ObserveAIPlanningTest(AIPlanningTestHooks *hooks, AIPlanningTestEvent event,
	uint32_t player, uint32_t begin, uint32_t end,
	AIProductionCandidateFact *verification = 0)
{
	if (hooks != 0 && hooks->observe != 0)
		hooks->observe(hooks->context, event, player, begin, end, verification);
}

bool AIPlanningTestEntryCancelled(AIPlanningTestHooks *hooks,
	uint32_t player, uint32_t begin, uint32_t end)
{
	return hooks != 0 && hooks->cancelAtEntry != 0 &&
		hooks->cancelAtEntry(hooks->context, player, begin, end);
}

void AIPlanningTestRendezvous(AIPlanningTestHooks *hooks, JobContext &context)
{
	if (hooks == 0 || hooks->rendezvous == 0 || hooks->rendezvousTarget < 2U ||
		!context.isPhysicalWorkerExecution()) return;
	hooks->rendezvous->fetch_add(1U, std::memory_order_acq_rel);
	while (hooks->rendezvous->load(std::memory_order_acquire) < hooks->rendezvousTarget &&
		!context.isCancellationRequested()) std::this_thread::yield();
}
#endif
std::atomic<uint64_t> s_capturedSnapshots(0U);
std::atomic<uint64_t> s_capturedCandidates(0U);
std::atomic<uint64_t> s_requestedBatches(0U);
std::atomic<uint64_t> s_submittedJobs(0U);
std::atomic<uint64_t> s_completedJobs(0U);
std::atomic<uint64_t> s_serialFallbacks(0U);
std::atomic<uint64_t> s_shadowMatches(0U);
std::atomic<uint64_t> s_shadowMismatches(0U);
std::atomic<uint64_t> s_validationFailures(0U);
std::atomic<uint64_t> s_canonicalValidationInvocations(0U);
std::atomic<uint64_t> s_committedBatches(0U);
std::atomic<uint64_t> s_parallelAuthoritativeCommits(0U);
std::atomic<uint64_t> s_rejectedCommits(0U);
std::atomic<uint64_t> s_physicalWorkerExecutions(0U);
std::atomic<uint64_t> s_ownerHelpedExecutions(0U);
std::atomic<uint64_t> s_observedPhysicalWorkerMask(0U);
std::atomic<uint64_t> s_maximumDistinctPhysicalWorkers(0U);
std::atomic<uint64_t> s_maximumConcurrentPhysicalWorkers(0U);

void UpdateMaximum(std::atomic<uint64_t> &maximum, uint64_t value)
{
	uint64_t observed = maximum.load(std::memory_order_relaxed);
	while (observed < value && !maximum.compare_exchange_weak(observed, value,
		std::memory_order_relaxed, std::memory_order_relaxed))
	{
	}
}

int32_t ClampInt32(int32_t value, int32_t minimum, int32_t maximum)
{
	if (value < minimum)
		return minimum;
	if (value > maximum)
		return maximum;
	return value;
}

int32_t PriorityMagnitude(int32_t priority)
{
	if (priority >= 0)
		return priority;
	if (priority == (-2147483647 - 1))
		return 2147483647;
	return -priority;
}

int32_t PriorityBandWidth(int32_t highestPriority, int32_t difficulty)
{
	const int32_t magnitude = PriorityMagnitude(highestPriority);
	if (difficulty == AI_PLANNING_DIFFICULTY_EASY)
		return 0;
	if (difficulty == AI_PLANNING_DIFFICULTY_NORMAL)
	{
		const int32_t width = magnitude / 20;
		return width > 1 ? width : 1;
	}
	const int32_t width = magnitude / 10;
	return width > 2 ? width : 2;
}

bool IsPriorityAdmitted(int32_t candidatePriority, int32_t highestPriority, int32_t difficulty)
{
	if (candidatePriority > highestPriority)
		return false;
	return (int64_t)highestPriority - (int64_t)candidatePriority <=
		(int64_t)PriorityBandWidth(highestPriority, difficulty);
}

bool IsAffordable(int32_t resources, int32_t cost, int32_t reserve)
{
	if (cost < 0)
		cost = 0;
	if (reserve < 0)
		reserve = 0;
	return (int64_t)resources >= (int64_t)cost + (int64_t)reserve;
}

int32_t EconomyScore(int32_t resources, int32_t minimumCost,
	int32_t plannedCost, int32_t reserve)
{
	if (IsAffordable(resources, plannedCost, reserve))
		return 100;
	if (IsAffordable(resources, minimumCost, reserve))
		return -150;
	return -350;
}

int32_t FactoryWaitScore(int32_t waitFrames, int32_t logicFramesPerSecond)
{
	if (waitFrames <= 0 || logicFramesPerSecond <= 0)
		return 0;
	const int64_t maximumFrames = (int64_t)30 * logicFramesPerSecond;
	if ((int64_t)waitFrames >= maximumFrames)
		return -250;
	return (int32_t)(-((int64_t)250 * waitFrames / maximumFrames));
}

int32_t RouteScore(int32_t routeClass)
{
	if (routeClass == AI_PLANNING_ROUTE_GROUND_REACHABLE)
		return 50;
	if (routeClass == AI_PLANNING_ROUTE_MIXED_UNREACHABLE)
		return -125;
	if (routeClass == AI_PLANNING_ROUTE_GROUND_UNREACHABLE)
		return -250;
	return 0;
}

int32_t TargetRouteScore(int32_t routeClass)
{
	if (routeClass == AI_PLANNING_TARGET_ROUTE_REACHABLE)
		return 150;
	if (routeClass == AI_PLANNING_TARGET_ROUTE_UNREACHABLE)
		return -150;
	return 0;
}

int32_t AddSourceCost(int32_t total, int32_t unitCost, int32_t count)
{
	if (unitCost <= 0 || count <= 0)
		return total;
	double result = (double)total + (double)unitCost * (double)count;
	return result > 2147483647.0 ? 2147483647 : (int32_t)result;
}

int32_t AddSourceFrames(int32_t total, int32_t frames)
{
	if (frames <= 0)
		return total;
	if (total > 2147483647 - frames)
		return 2147483647;
	return total + frames;
}

bool ComputeSourceProduction(const AIProductionPlanningSnapshot &snapshot,
	const AIProductionCandidateSourceFact &candidate, bool planned,
	int32_t *productionCost, int32_t *completionFrames)
{
	if (!productionCost || !completionFrames || !candidate.valid ||
		candidate.unitCount > AI_PLANNING_MAX_PRODUCTION_UNITS ||
		snapshot.sourceFacts.factoryCount > AI_PLANNING_MAX_PRODUCTION_FACTORIES)
		return false;
	int32_t projectedLoads[AI_PLANNING_MAX_PRODUCTION_FACTORIES];
	bool usedFactories[AI_PLANNING_MAX_PRODUCTION_FACTORIES];
	for (uint32_t i = 0; i < snapshot.sourceFacts.factoryCount; ++i)
	{
		if (!snapshot.sourceFacts.factories[i].valid)
			return false;
		projectedLoads[i] = snapshot.sourceFacts.factories[i].projectedFrames;
		usedFactories[i] = false;
	}
	*productionCost = 0;
	*completionFrames = 0;
	const uint32_t phaseCount = planned ? 2U : 1U;
	for (uint32_t phase = 0U; phase < phaseCount; ++phase)
	{
		for (uint32_t reverse = candidate.unitCount; reverse != 0U; --reverse)
		{
			const AIProductionUnitSourceFact &unit = candidate.units[reverse - 1U];
			int32_t unitsRemaining = phase == 0U ? unit.minUnits :
				unit.maxUnits - unit.minUnits;
			if (unitsRemaining <= 0)
				continue;
			int selectedFactory = -1;
			for (uint32_t factory = 0U;
				factory < snapshot.sourceFacts.factoryCount; ++factory)
			{
				if ((unit.compatibleFactoryMask & (1U << factory)) == 0U)
					continue;
				if (selectedFactory < 0 || projectedLoads[factory] <
					projectedLoads[selectedFactory])
					selectedFactory = (int)factory;
			}
			if (selectedFactory < 0)
				return false;
			const int32_t buildFrames = unit.buildFrames;
			const uint32_t quantity = unit.productionQuantity[selectedFactory] < 1U ?
				1U : unit.productionQuantity[selectedFactory];
			*productionCost = AddSourceCost(*productionCost, unit.cost, 1);
			projectedLoads[selectedFactory] = AddSourceFrames(
				projectedLoads[selectedFactory], buildFrames);
			usedFactories[selectedFactory] = true;
			unitsRemaining -= (int32_t)quantity;
			while (unitsRemaining > 0)
			{
				selectedFactory = -1;
				for (uint32_t factory = 0U;
					factory < snapshot.sourceFacts.factoryCount; ++factory)
				{
					if ((unit.compatibleFactoryMask & (1U << factory)) == 0U)
						continue;
					if (selectedFactory < 0 || projectedLoads[factory] <
						projectedLoads[selectedFactory])
						selectedFactory = (int)factory;
				}
				if (selectedFactory < 0)
					return false;
				const uint32_t nextQuantity = unit.productionQuantity[selectedFactory] < 1U ?
					1U : unit.productionQuantity[selectedFactory];
				*productionCost = AddSourceCost(*productionCost, unit.cost, 1);
				projectedLoads[selectedFactory] = AddSourceFrames(
					projectedLoads[selectedFactory], buildFrames);
				usedFactories[selectedFactory] = true;
				unitsRemaining -= (int32_t)nextQuantity;
			}
		}
	}
	for (uint32_t factory = 0U;
		factory < snapshot.sourceFacts.factoryCount; ++factory)
	{
		if (usedFactories[factory] && projectedLoads[factory] > *completionFrames)
			*completionFrames = projectedLoads[factory];
	}
	return true;
}

int32_t SourceCounterFit(const AIProductionPlanningSnapshot &snapshot,
	const AIProductionCandidateSourceFact &candidate)
{
	int32_t plannedValue = 0;
	int32_t antiAircraftValue = 0;
	int32_t antiVehicleValue = 0;
	int32_t antiInfantryValue = 0;
	for (uint32_t i = 0U; i < candidate.unitCount; ++i)
	{
		const AIProductionUnitSourceFact &unit = candidate.units[i];
		if (unit.maxUnits <= 0)
			continue;
		const int32_t value = AddSourceCost(0, unit.cost, unit.maxUnits);
		plannedValue = AddSourceCost(plannedValue, value, 1);
		if ((unit.flags & AI_PRODUCTION_SOURCE_ATTACKS_AIRCRAFT) != 0U)
			antiAircraftValue = AddSourceCost(antiAircraftValue, value, 1);
		if ((unit.flags & (AI_PRODUCTION_SOURCE_ATTACKS_GROUND |
			AI_PRODUCTION_SOURCE_PREFERS_VEHICLE)) != 0U)
			antiVehicleValue = AddSourceCost(antiVehicleValue, value, 1);
		if ((unit.flags & (AI_PRODUCTION_SOURCE_ATTACKS_GROUND |
			AI_PRODUCTION_SOURCE_PREFERS_INFANTRY)) != 0U)
			antiInfantryValue = AddSourceCost(antiInfantryValue, value, 1);
	}
	int32_t enemyAircraft = snapshot.sourceFacts.enemyAircraftValue;
	int32_t enemyVehicle = snapshot.sourceFacts.enemyVehicleValue;
	int32_t enemyInfantry = snapshot.sourceFacts.enemyInfantryValue;
	if (enemyAircraft < 0) enemyAircraft = 0;
	if (enemyVehicle < 0) enemyVehicle = 0;
	if (enemyInfantry < 0) enemyInfantry = 0;
	const double enemyTotal = (double)enemyAircraft + enemyVehicle + enemyInfantry;
	if (plannedValue <= 0 || enemyTotal <= 0.0)
		return 0;
	double matchedValue = 0.0;
	const int32_t antiValues[3] = {
		antiAircraftValue, antiVehicleValue, antiInfantryValue};
	const int32_t enemyValues[3] = { enemyAircraft, enemyVehicle, enemyInfantry };
	for (unsigned i = 0U; i < 3U; ++i)
	{
		double coverage = (double)antiValues[i] / (double)plannedValue;
		if (coverage > 1.0) coverage = 1.0;
		if (coverage > 0.0) matchedValue += (double)enemyValues[i] * coverage;
	}
	int32_t score = (int32_t)(300.0 * matchedValue / enemyTotal + 0.5);
	return ClampInt32(score, 0, 300);
}

bool BuildSourceCandidateFact(const AIProductionPlanningSnapshot &snapshot,
	uint32_t candidateIndex, AIProductionCandidateFact *fact)
{
	if (!fact || candidateIndex >= snapshot.candidateCount ||
		candidateIndex >= AI_PLANNING_MAX_PRODUCTION_CANDIDATES)
		return false;
	*fact = snapshot.candidates[candidateIndex];
	if (snapshot.sourceFacts.valid == 0U)
		return true;
	const AIProductionCandidateSourceFact &source =
		snapshot.sourceFacts.candidates[candidateIndex];
	if (!source.valid || source.unitCount > AI_PLANNING_MAX_PRODUCTION_UNITS)
		return false;
	if (!ComputeSourceProduction(snapshot, source, false,
		&fact->minimumCost, &fact->factoryWaitFrames) ||
		!ComputeSourceProduction(snapshot, source, true,
		&fact->plannedCost, &fact->factoryWaitFrames))
		return false;
	fact->counterFitScore = SourceCounterFit(snapshot, source);
	bool hasGround = false;
	bool hasAir = false;
	for (uint32_t i = 0U; i < source.unitCount; ++i)
	{
		const uint32_t flags = source.units[i].flags;
		if (source.units[i].maxUnits <= 0)
			continue;
		if ((flags & AI_PRODUCTION_SOURCE_AIRCRAFT) != 0U)
			hasAir = true;
		else if ((flags & (AI_PRODUCTION_SOURCE_VEHICLE |
			AI_PRODUCTION_SOURCE_INFANTRY)) != 0U)
			hasGround = true;
	}
	if (!hasGround || snapshot.sourceFacts.hasRouteTarget == 0U ||
		snapshot.sourceFacts.groundRouteKnown == 0U)
		fact->routeClass = AI_PLANNING_ROUTE_UNKNOWN;
	else if (snapshot.sourceFacts.groundRouteReachable != 0U)
		fact->routeClass = AI_PLANNING_ROUTE_GROUND_REACHABLE;
	else
		fact->routeClass = hasAir ? AI_PLANNING_ROUTE_MIXED_UNREACHABLE :
			AI_PLANNING_ROUTE_GROUND_UNREACHABLE;
	return true;
}

AIPlanningOrderKey MakeOrderKey(uint32_t frame, uint32_t playerIndex,
	uint32_t subphase, uint32_t sourceOrdinal)
{
	AIPlanningOrderKey result;
	result.frame = frame;
	result.playerIndex = playerIndex;
	result.subphase = subphase;
	result.sourceOrdinal = sourceOrdinal;
	result.emissionOrdinal = 0U;
	return result;
}

bool EqualOrderKey(const AIPlanningOrderKey &left, const AIPlanningOrderKey &right)
{
	return left.frame == right.frame &&
		left.playerIndex == right.playerIndex &&
		left.subphase == right.subphase &&
		left.sourceOrdinal == right.sourceOrdinal &&
		left.emissionOrdinal == right.emissionOrdinal;
}

AIProductionCandidateScore ScoreProductionCandidate(
	const AIProductionPlanningSnapshot &snapshot,
	const AIProductionCandidateFact &candidate,
	int32_t reserve)
{
	AIProductionCandidateScore result;
	memset(&result, 0, sizeof(result));
	result.sourceOrdinal = candidate.sourceOrdinal;
	result.candidateStableId = candidate.candidateStableId;
	result.counterFitScore = ClampInt32(candidate.counterFitScore, 0, 300);
	result.economyScore = EconomyScore(snapshot.resources,
		candidate.minimumCost, candidate.plannedCost, reserve);
	result.factoryWaitScore = FactoryWaitScore(candidate.factoryWaitFrames,
		snapshot.logicFramesPerSecond);
	result.routeScore = RouteScore(candidate.routeClass);
	result.lossScore = ClampInt32(candidate.recentLossCount, 0, 3) * -75;
	result.pathFailureScore = ClampInt32(candidate.recentPathFailureCount, 0, 3) * -100;
	result.rawContextScore = ClampInt32(result.counterFitScore + result.economyScore +
		result.factoryWaitScore + result.routeScore + result.lossScore +
		result.pathFailureScore, -1000, 1000);
	result.finalScore = (int64_t)candidate.configuredPriority * 1000 +
		(int64_t)result.rawContextScore * snapshot.contextInfluencePercent / 100;
	result.considered = 1U;
	return result;
}

bool EqualEnemyCandidateScore(const AIEnemyCandidateScore &left,
	const AIEnemyCandidateScore &right)
{
	return left.sourceOrdinal == right.sourceOrdinal &&
		left.playerIndex == right.playerIndex &&
		left.knownAssetScore == right.knownAssetScore &&
		left.retaliationScore == right.retaliationScore &&
		left.routeScore == right.routeScore &&
		left.allyTargetScore == right.allyTargetScore &&
		left.distanceScore == right.distanceScore &&
		left.crippledScore == right.crippledScore &&
		left.totalScore == right.totalScore;
}

bool EqualProductionCandidateScore(const AIProductionCandidateScore &left,
	const AIProductionCandidateScore &right)
{
	return left.sourceOrdinal == right.sourceOrdinal &&
		left.candidateStableId == right.candidateStableId &&
		left.counterFitScore == right.counterFitScore &&
		left.economyScore == right.economyScore &&
		left.factoryWaitScore == right.factoryWaitScore &&
		left.routeScore == right.routeScore &&
		left.lossScore == right.lossScore &&
		left.pathFailureScore == right.pathFailureScore &&
		left.rawContextScore == right.rawContextScore &&
		left.finalScore == right.finalScore &&
		left.considered == right.considered;
}

bool EqualProductionCandidateFact(const AIProductionCandidateFact &left,
	const AIProductionCandidateFact &right)
{
	return left.sourceOrdinal == right.sourceOrdinal &&
		left.candidateStableId == right.candidateStableId &&
		left.configuredPriority == right.configuredPriority &&
		left.counterFitScore == right.counterFitScore &&
		left.minimumCost == right.minimumCost &&
		left.plannedCost == right.plannedCost &&
		left.factoryWaitFrames == right.factoryWaitFrames &&
		left.routeClass == right.routeClass &&
		left.recentLossCount == right.recentLossCount &&
		left.recentPathFailureCount == right.recentPathFailureCount &&
		left.eligible == right.eligible;
}

void CopyPlayerResults(const AIPlayerPlanningResult *source,
	uint32_t resultCount, AIPlayerPlanningResult *destination)
{
	for (uint32_t i = 0; i < resultCount; ++i)
		destination[i] = source[i];
}

bool ArePlayerSnapshotsCommitOrdered(const AIPlayerPlanningSnapshot *snapshots,
	uint32_t snapshotCount)
{
	for (uint32_t i = 0; i < snapshotCount; ++i)
	{
		if (snapshots[i].planEnemyTarget > 1U || snapshots[i].planProduction > 1U ||
			snapshots[i].enemyTarget.candidateCount > AI_PLANNING_MAX_PLAYERS ||
			snapshots[i].production.candidateCount > AI_PLANNING_MAX_PRODUCTION_CANDIDATES)
			return false;
		if (i != 0U && snapshots[i].playerIndex <= snapshots[i - 1U].playerIndex)
			return false;
		if (snapshots[i].planEnemyTarget != 0U &&
			(snapshots[i].enemyTarget.frame != snapshots[i].frame ||
			 snapshots[i].enemyTarget.ownerPlayerIndex != snapshots[i].playerIndex))
			return false;
		for (uint32_t candidateIndex = 0;
			candidateIndex < snapshots[i].enemyTarget.candidateCount; ++candidateIndex)
		{
			const AIEnemyCandidateFact &candidate =
				snapshots[i].enemyTarget.candidates[candidateIndex];
			if (candidate.sourceOrdinal == AI_PLANNING_INVALID_ORDINAL ||
				(candidateIndex != 0U && candidate.sourceOrdinal <=
				 snapshots[i].enemyTarget.candidates[candidateIndex - 1U].sourceOrdinal))
				return false;
		}
		if (snapshots[i].planProduction != 0U &&
			(snapshots[i].production.frame != snapshots[i].frame ||
			 snapshots[i].production.ownerPlayerIndex != snapshots[i].playerIndex))
			return false;
		for (uint32_t candidateIndex = 0;
			candidateIndex < snapshots[i].production.candidateCount; ++candidateIndex)
		{
			const AIProductionCandidateFact &candidate =
				snapshots[i].production.candidates[candidateIndex];
			if (candidate.sourceOrdinal == AI_PLANNING_INVALID_ORDINAL ||
				(candidateIndex != 0U && candidate.sourceOrdinal <=
				 snapshots[i].production.candidates[candidateIndex - 1U].sourceOrdinal))
				return false;
		}
	}
	return true;
}

bool ArePlayerResultsComplete(const AIPlayerPlanningSnapshot *snapshots,
	const AIPlayerPlanningResult *results, uint32_t resultCount)
{
	for (uint32_t i = 0; i < resultCount; ++i)
	{
		if (!ValidateAIPlayerPlanningResult(snapshots[i], results[i]))
			return false;
	}
	return true;
}

class AIPlayerPlanningJob : public Job
{
public:
	struct ExecutionRecord
	{
		ExecutionRecord() : completed(false), physicalWorker(false),
			physicalWorkerIndex(JOB_INVALID_PHYSICAL_WORKER_INDEX),
			ownerHelped(false) {}

		bool completed;
		bool physicalWorker;
		unsigned physicalWorkerIndex;
		bool ownerHelped;
#if defined(_WIN64)
		bool traceSource = false;
		performance::KernelPerformanceRangePlan range = {};
		performance::KernelPerformanceCheckpointProbe checkpoint;
#endif
	};

	AIPlayerPlanningJob(const AIPlayerPlanningSnapshot *snapshot,
		AIPlayerPlanningResult *result, ExecutionRecord *execution,
		std::atomic<uint32_t> *activePhysicalWorkers,
		std::atomic<uint32_t> *peakPhysicalWorkers,
		const JobFloatingPointState &floatingPointState
#if defined(_WIN64)
		, AIPlanningTestHooks *testHooks = 0, uint32_t playerOrdinal = 0
#endif
		) :
		m_snapshot(snapshot), m_result(result), m_execution(execution),
		m_activePhysicalWorkers(activePhysicalWorkers),
		m_peakPhysicalWorkers(peakPhysicalWorkers),
		m_floatingPointState(floatingPointState)
#if defined(_WIN64)
		, m_testHooks(testHooks), m_playerOrdinal(playerOrdinal)
#endif
		{}

	void execute(JobContext &context) override
	{
		const JobFloatingPointScope floatingPointScope(m_floatingPointState);
#if defined(_WIN64)
		if (m_execution->traceSource) m_execution->checkpoint.beginRecord();
#endif
		if (!enterBody(context.isCancellationRequested()))
		{
			context.fail();
			return;
		}
		m_execution->physicalWorker = context.isPhysicalWorkerExecution();
		m_execution->ownerHelped = !m_execution->physicalWorker;
		uint32_t activePhysicalWorkers = 0U;
		if (m_execution->physicalWorker)
		{
			m_execution->physicalWorkerIndex = context.physicalWorkerIndex();
			activePhysicalWorkers = m_activePhysicalWorkers->fetch_add(1U,
				std::memory_order_acq_rel) + 1U;
			uint32_t observed = m_peakPhysicalWorkers->load(
				std::memory_order_relaxed);
			while (observed < activePhysicalWorkers &&
				!m_peakPhysicalWorkers->compare_exchange_weak(observed,
					activePhysicalWorkers, std::memory_order_relaxed,
					std::memory_order_relaxed)) {}
		}
#if defined(_WIN64)
		AIPlanningTestRendezvous(m_testHooks, context);
#endif
		const bool planned = planBody();
		if (m_execution->physicalWorker)
			m_activePhysicalWorkers->fetch_sub(1U, std::memory_order_acq_rel);
		if (!planned) context.fail();
		else s_completedJobs.fetch_add(1U, std::memory_order_relaxed);
	}

#if defined(_WIN64)
	// Called only after the reference ledger authenticates this exact range.
	// No JobContext, submission, physical-worker identity, or rendezvous exists
	// on this path; both modes execute the same entry and planning body below.
	bool executeInline()
	{
		const JobFloatingPointScope floatingPointScope(m_floatingPointState);
		return enterBody(false) && planBody();
	}
#endif

private:
	bool enterBody(bool cancellationRequested)
	{
#if defined(_WIN64)
		ObserveAIPlanningTest(m_testHooks, AI_PLANNING_TEST_RANGE_ENTRY, m_playerOrdinal, 0, 1);
#endif
		bool cancelled = cancellationRequested
#if defined(_WIN64)
			|| AIPlanningTestEntryCancelled(m_testHooks, m_playerOrdinal, 0, 1)
#endif
			;
#if defined(_WIN64)
		const performance::KernelPerformanceCheckpoint entry = {1, m_playerOrdinal, 0};
		if (m_execution->traceSource)
			cancelled = m_execution->checkpoint.cancelled(entry, cancelled);
#endif
		if (cancelled)
		{
#if defined(_WIN64)
			if (m_execution->traceSource)
				m_execution->checkpoint.finish(entry, 0, performance::KERNEL_RANGE_CANCELLED);
#endif
			return false;
		}
		return true;
	}
	bool planBody()
	{
#if defined(_WIN64)
		ObserveAIPlanningTest(m_testHooks, AI_PLANNING_TEST_PLAYER_BODY, m_playerOrdinal, 0, 1);
#endif
		const bool planned = PlanAIPlayer(*m_snapshot, m_result);
#if defined(_WIN64)
		if (m_execution->traceSource)
		{
			const performance::KernelPerformanceCheckpoint end = {2, m_playerOrdinal, 1};
			m_execution->checkpoint.finish(end, planned ? 1 : 0,
				planned ? performance::KERNEL_RANGE_COMPLETED : performance::KERNEL_RANGE_FAILED);
		}
#endif
		m_execution->completed = planned;
		return planned;
	}

private:
	const AIPlayerPlanningSnapshot *m_snapshot;
	AIPlayerPlanningResult *m_result;
	ExecutionRecord *m_execution;
	std::atomic<uint32_t> *m_activePhysicalWorkers;
	std::atomic<uint32_t> *m_peakPhysicalWorkers;
	const JobFloatingPointState m_floatingPointState;
#if defined(_WIN64)
	AIPlanningTestHooks *m_testHooks;
	uint32_t m_playerOrdinal;
#endif
};

// Large current-epoch production snapshots are split into deterministic,
// contiguous candidate ranges. Each worker writes only its range in the
// caller-owned POD buffer; no live product object is reachable from this job.
class AIProductionCandidateShardJob : public Job
{
public:
	AIProductionCandidateShardJob(
		const AIProductionPlanningSnapshot *snapshot,
		AIProductionCandidateFact *facts,
		AIProductionCandidateFact *verificationFacts,
		uint32_t begin, uint32_t end,
		AIPlayerPlanningJob::ExecutionRecord *execution,
		std::atomic<uint32_t> *activePhysicalWorkers,
		std::atomic<uint32_t> *peakPhysicalWorkers,
		const JobFloatingPointState &floatingPointState
#if defined(_WIN64)
		, AIPlanningTestHooks *testHooks = 0, uint32_t playerOrdinal = 0
#endif
		) :
		m_snapshot(snapshot), m_facts(facts),
		m_verificationFacts(verificationFacts), m_begin(begin), m_end(end),
		m_execution(execution), m_activePhysicalWorkers(activePhysicalWorkers),
		m_peakPhysicalWorkers(peakPhysicalWorkers),
		m_floatingPointState(floatingPointState)
#if defined(_WIN64)
		, m_testHooks(testHooks), m_playerOrdinal(playerOrdinal)
#endif
		{}

	void execute(JobContext &context) override
	{
		const JobFloatingPointScope floatingPointScope(m_floatingPointState);
		m_execution->physicalWorker = context.isPhysicalWorkerExecution();
		m_execution->ownerHelped = !m_execution->physicalWorker;
		if (m_execution->physicalWorker)
		{
			m_execution->physicalWorkerIndex = context.physicalWorkerIndex();
			const uint32_t active = m_activePhysicalWorkers->fetch_add(1U,
				std::memory_order_acq_rel) + 1U;
			uint32_t observed = m_peakPhysicalWorkers->load(
				std::memory_order_relaxed);
			while (observed < active &&
				!m_peakPhysicalWorkers->compare_exchange_weak(observed, active,
					std::memory_order_relaxed, std::memory_order_relaxed)) {}
		}
#if defined(_WIN64)
		AIPlanningTestRendezvous(m_testHooks, context);
		if (m_execution->traceSource) m_execution->checkpoint.beginRecord();
#endif
		const bool planned = planBody(context.isCancellationRequested());
		if (m_execution->physicalWorker)
			m_activePhysicalWorkers->fetch_sub(1U, std::memory_order_acq_rel);
		if (!planned) context.fail();
		else s_completedJobs.fetch_add(1U, std::memory_order_relaxed);
	}

#if defined(_WIN64)
	bool executeInline()
	{
		const JobFloatingPointScope floatingPointScope(m_floatingPointState);
		return planBody(false);
	}
#endif

private:
	bool planBody(bool cancellationRequested)
	{
#if defined(_WIN64)
		ObserveAIPlanningTest(m_testHooks, AI_PLANNING_TEST_RANGE_ENTRY,
			m_playerOrdinal, m_begin, m_end);
#endif
		bool cancelled = cancellationRequested
#if defined(_WIN64)
			|| AIPlanningTestEntryCancelled(m_testHooks, m_playerOrdinal, m_begin, m_end)
#endif
			;
#if defined(_WIN64)
		const performance::KernelPerformanceCheckpoint entry = {1, m_playerOrdinal, m_begin};
		if (m_execution->traceSource)
			cancelled = m_execution->checkpoint.cancelled(entry, cancelled);
		uint32_t completedCandidates = 0;
#endif
		bool planned = !cancelled;
		for (uint32_t i = m_begin; planned && i < m_end; ++i)
		{
			// Derive two disjoint outputs from the immutable owner snapshot. The
			// owner compares them before accepting either one, so a corrupted
			// worker-prepared fact cannot redefine its own validation oracle.
			planned = BuildSourceCandidateFact(*m_snapshot, i, &m_facts[i]);
#if defined(_WIN64)
			if (planned) ObserveAIPlanningTest(m_testHooks, AI_PLANNING_TEST_PREPARED_FACT,
				m_playerOrdinal, i, i + 1);
#endif
			if (planned) planned = BuildSourceCandidateFact(*m_snapshot, i, &m_verificationFacts[i]);
#if defined(_WIN64)
			if (planned) ObserveAIPlanningTest(m_testHooks, AI_PLANNING_TEST_VERIFICATION_FACT,
				m_playerOrdinal, i, i + 1, &m_verificationFacts[i]);
			if (planned) ++completedCandidates;
#endif
		}
#if defined(_WIN64)
		if (m_execution->traceSource)
		{
			const performance::KernelPerformanceCheckpoint end = {2, m_playerOrdinal,
				m_begin + completedCandidates};
			m_execution->checkpoint.finish(cancelled ? entry : end, completedCandidates,
				cancelled ? performance::KERNEL_RANGE_CANCELLED :
				planned ? performance::KERNEL_RANGE_COMPLETED : performance::KERNEL_RANGE_FAILED);
		}
#endif
		m_execution->completed = planned;
		return planned;
	}

private:
	const AIProductionPlanningSnapshot *m_snapshot;
	AIProductionCandidateFact *m_facts;
	AIProductionCandidateFact *m_verificationFacts;
	uint32_t m_begin;
	uint32_t m_end;
	AIPlayerPlanningJob::ExecutionRecord *m_execution;
	std::atomic<uint32_t> *m_activePhysicalWorkers;
	std::atomic<uint32_t> *m_peakPhysicalWorkers;
	const JobFloatingPointState m_floatingPointState;
#if defined(_WIN64)
	AIPlanningTestHooks *m_testHooks;
	uint32_t m_playerOrdinal;
#endif
};

struct AIPlanningJobSystemEvidence
{
	AIPlanningJobSystemEvidence() : physicalWorkerMask(0U),
		distinctPhysicalWorkers(0U), peakConcurrentPhysicalWorkers(0U),
		physicalWorkerExecutions(0U), ownerHelpedJobs(0U),
		resultsValidated(false), nativeAdmissionAccepted(false)
#if defined(_WIN64)
		, performanceBatch(0), referenceBatch(0), sourceBoundInline(false),
		sourceOwnerCommitAllowed(false)
#endif
	{}

	void collect(const AIPlayerPlanningJob::ExecutionRecord *execution,
		uint32_t executionCount, uint32_t peak)
	{
		physicalWorkerMask = 0U;
		distinctPhysicalWorkers = 0U;
		peakConcurrentPhysicalWorkers = peak;
		physicalWorkerExecutions = 0U;
		ownerHelpedJobs = 0U;
		for (uint32_t index = 0; index < executionCount; ++index)
		{
			if (execution[index].ownerHelped) ++ownerHelpedJobs;
			if (!execution[index].physicalWorker) continue;
			++physicalWorkerExecutions;
			const unsigned workerIndex = execution[index].physicalWorkerIndex;
			if (workerIndex < 64U)
				physicalWorkerMask |= (uint64_t(1U) << workerIndex);
			bool firstExecutionOnWorker = true;
			for (uint32_t previous = 0; previous < index; ++previous)
			{
				if (execution[previous].physicalWorker &&
					execution[previous].physicalWorkerIndex == workerIndex)
				{
					firstExecutionOnWorker = false;
					break;
				}
			}
			if (firstExecutionOnWorker) ++distinctPhysicalWorkers;
		}
	}

	uint64_t physicalWorkerMask;
	uint32_t distinctPhysicalWorkers;
	uint32_t peakConcurrentPhysicalWorkers;
	uint32_t physicalWorkerExecutions;
	uint32_t ownerHelpedJobs;
	// True only after worker facts have passed their independent comparison and
	// every result is either owner-produced from those facts or canonically
	// validated against its original immutable snapshot.
	bool resultsValidated;
	bool nativeAdmissionAccepted;
#if defined(_WIN64)
	performance::KernelPerformanceBatch *performanceBatch;
	AIPlanningReferenceBatchTransport *referenceBatch;
	bool sourceBoundInline;
	bool sourceOwnerCommitAllowed;
#endif
};

uint32_t CountAIPlanningJobs(const AIPlayerPlanningSnapshot *snapshots,
	uint32_t snapshotCount)
{
	uint32_t jobCount = 0U;
	for (uint32_t player = 0U; player < snapshotCount; ++player)
	{
		const AIProductionPlanningSnapshot &production = snapshots[player].production;
		if (snapshots[player].planProduction != 0U &&
			production.sourceFacts.valid != 0U)
		{
			jobCount += (production.candidateCount +
				AI_PLANNING_PRODUCTION_SHARD_SIZE - 1U) /
				AI_PLANNING_PRODUCTION_SHARD_SIZE;
		}
		else
		{
			++jobCount;
		}
	}
	return jobCount;
}

#if defined(_WIN64)
performance::KernelPerformanceDigest AIPlanningDecisionFacts(uint32_t playerCount,
	uint32_t requestedJobs)
{
	performance::KernelPerformanceCanonicalWriter facts;
	if (!facts.begin(1) || !facts.u32(1, playerCount) || !facts.u32(2, requestedJobs) ||
		!facts.u32(3, AI_PLANNING_PRODUCTION_SHARD_SIZE))
		return performance::KernelPerformanceDigest();
	return facts.finish();
}

// This owner-side recorder never dispatches, reruns, cancels, or publishes work.
// Its range POD is imported only after the existing native group fence.
class AIPlanningSourceAttempt
{
public:
	AIPlanningSourceAttempt(AIPlanningReferenceBatchTransport *transport,
		AIPlayerPlanningJob::ExecutionRecord *execution, uint32_t playerCount,
		uint32_t requestedJobs, JobSystem &jobs) : m_transport(transport),
		m_execution(execution), m_requestedJobs(requestedJobs), m_workers(0),
		m_pending(0), m_outstanding(0),
		m_enabled(false), m_released(false)
	{
		if (transport == 0 || transport->referenceLedger == 0 || !transport->referenceAttempt.valid()) return;
		const performance::KernelPerformanceReferenceMode mode = transport->referenceLedger->mode();
		if (mode != performance::KERNEL_REFERENCE_THROUGHPUT_BINDING &&
			mode != performance::KERNEL_REFERENCE_SERIAL_ORACLE) return;
		m_workers = jobs.workerCount(); m_pending = jobs.pendingOwnerCompletionCount();
		m_outstanding = jobs.outstandingJobCount();
		m_enabled = transport->referenceLedger->bindCapturedInput(transport->referenceAttempt,
			transport->fieldSchema, transport->operationCount, transport->writeInput, transport->immutableInput);
		if (m_enabled) m_facts = AIPlanningDecisionFacts(playerCount, requestedJobs);
	}
	~AIPlanningSourceAttempt() { release(0, false); }
	void plan(uint32_t ordinal, unsigned kind, uint32_t begin, uint32_t end)
	{
		m_execution[ordinal].traceSource = m_enabled;
		m_execution[ordinal].range = {1, ordinal, kind, begin, end, end - begin};
	}
	void release(uint32_t submitted, bool cancelled)
	{
		if (!m_enabled || m_released) return;
		m_released = true;
		performance::KernelPerformanceReferenceLedger &ledger = *m_transport->referenceLedger;
		const performance::KernelPerformanceAttempt attempt = m_transport->referenceAttempt;
		performance::KernelPerformanceAttemptDecision decision = {};
		decision.site = 1; decision.reasonSchema = 1;
		decision.reason = submitted != 0 ? (cancelled ? 3 : 1) : 2;
		decision.deterministicEligible = m_requestedJobs >= 2;
		decision.deterministicFacts = m_facts;
		decision.admission = submitted != 0 ? performance::KERNEL_ADMISSION_ACCEPTED :
			performance::KERNEL_ADMISSION_REFUSED;
		decision.sourceConfiguredWorkers = m_workers;
		decision.dynamicFactsKnownMask = 3; decision.pendingJobs = m_pending;
		decision.outstandingJobs = m_outstanding;
		AIPlanningTestHooks *hooks = m_transport->testHooks;
		if (hooks != 0 && hooks->releasedGroup != 0)
		{
			unsigned completed = 0;
			for (uint32_t i = 0; i != submitted; ++i) if (m_execution[i].completed) ++completed;
			hooks->releasedGroup(hooks->context, cancelled, completed, submitted, decision.reason);
		}
		ledger.observeDecision(attempt, decision);
		if (submitted == 0) return;
		performance::KernelPerformanceDispatchPlan dispatch = {1, 1, 1, submitted, 0,
			AI_PLANNING_PRODUCTION_SHARD_SIZE, AI_PLANNING_MAX_JOB_EXECUTIONS};
		for (uint32_t i = 0; i != submitted; ++i) dispatch.operationCount += m_execution[i].range.operationCount;
		ledger.observeDispatch(attempt, dispatch);
		for (uint32_t i = 0; i != submitted; ++i) ledger.observeRangePlan(attempt, m_execution[i].range);
		for (uint32_t i = 0; i != submitted; ++i)
		{
			performance::KernelPerformanceRangeProgress progress = {};
			progress.checkpoint = m_execution[i].checkpoint.snapshot();
			progress.publication = !progress.checkpoint.entered ? performance::KERNEL_PUBLICATION_NOT_APPLICABLE :
				cancelled || progress.checkpoint.terminal == performance::KERNEL_RANGE_CANCELLED ?
				performance::KERNEL_PUBLICATION_DISCARDED_AFTER_CANCEL : m_execution[i].completed ?
				performance::KERNEL_PUBLICATION_PUBLISHED : performance::KERNEL_PUBLICATION_REJECTED;
			ledger.observeReleasedRange(attempt, m_execution[i].range, progress);
		}
	}
private:
	AIPlanningReferenceBatchTransport *m_transport;
	AIPlayerPlanningJob::ExecutionRecord *m_execution;
	uint32_t m_requestedJobs;
	unsigned m_workers;
	JobMetricCounter m_pending, m_outstanding;
	bool m_enabled, m_released;
	performance::KernelPerformanceDigest m_facts;
};

bool ConsumeAIPlanningRanges(AIPlanningReferenceBatchTransport *transport,
	const AIPlayerPlanningSnapshot *snapshots, uint32_t snapshotCount,
	AIPlayerPlanningResult *results,
	AIProductionCandidateFact (*shardedFacts)[AI_PLANNING_MAX_PRODUCTION_CANDIDATES],
	AIProductionCandidateFact (*verificationFacts)[AI_PLANNING_MAX_PRODUCTION_CANDIDATES],
	AIPlayerPlanningJob::ExecutionRecord *execution,
	const JobFloatingPointState &floatingPointState, bool *admissionAccepted)
{
	if (admissionAccepted != 0) *admissionAccepted = false;
	if (transport == 0 || transport->referenceLedger == 0 || !transport->referenceAttempt.valid())
		return false;
	performance::KernelPerformanceReferenceLedger &ledger = *transport->referenceLedger;
	const performance::KernelPerformanceAttempt attempt = transport->referenceAttempt;
	const uint32_t requested = CountAIPlanningJobs(snapshots, snapshotCount);
	performance::KernelPerformanceAttemptDecision decision = {};
	if (!ledger.bindCapturedInput(attempt, transport->fieldSchema, transport->operationCount,
		transport->writeInput, transport->immutableInput) ||
		!ledger.replayDecision(attempt, 1, requested >= 2,
			AIPlanningDecisionFacts(snapshotCount, requested), decision) ||
		decision.admission != performance::KERNEL_ADMISSION_ACCEPTED)
		return false;
	if (admissionAccepted != 0) *admissionAccepted = true;
	if (decision.reasonSchema != 1 || (decision.reason != 1 && decision.reason != 3)) return false;
	const bool sourceCancelled = decision.reason == 3;
	performance::KernelPerformanceDispatchPlan source = {};
	if (!ledger.readSourceDispatch(attempt, 1, source) || source.rangeCount == 0 ||
		source.rangeCount > requested || requested > AI_PLANNING_MAX_JOB_EXECUTIONS)
		return false;
	uint32_t ordinal = 0;
	for (uint32_t player = 0; player != snapshotCount; ++player)
	{
		const AIProductionPlanningSnapshot &production = snapshots[player].production;
		if (snapshots[player].planProduction != 0 && production.sourceFacts.valid != 0)
		{
			for (uint32_t begin = 0; begin < production.candidateCount;
				begin += AI_PLANNING_PRODUCTION_SHARD_SIZE, ++ordinal)
			{
				const uint32_t end = begin + AI_PLANNING_PRODUCTION_SHARD_SIZE < production.candidateCount ?
					begin + AI_PLANNING_PRODUCTION_SHARD_SIZE : production.candidateCount;
				execution[ordinal].range = {1, ordinal, 2,
					player * AI_PLANNING_MAX_PRODUCTION_CANDIDATES + begin,
					player * AI_PLANNING_MAX_PRODUCTION_CANDIDATES + end, end - begin};
			}
		}
		else
		{
			execution[ordinal].range = {1, ordinal, 1, player, player + 1, 1};
			++ordinal;
		}
	}
	performance::KernelPerformanceDispatchPlan dispatch = {1, 1, 1, source.rangeCount, 0,
		AI_PLANNING_PRODUCTION_SHARD_SIZE, AI_PLANNING_MAX_JOB_EXECUTIONS};
	for (uint32_t i = 0; i != dispatch.rangeCount; ++i)
		dispatch.operationCount += execution[i].range.operationCount;
	if (!ledger.observeDispatch(attempt, dispatch)) return false;
	for (uint32_t i = 0; i != dispatch.rangeCount; ++i)
		if (!ledger.observeRangePlan(attempt, execution[i].range)) return false;
	bool completed = dispatch.rangeCount == requested && !sourceCancelled;
	for (uint32_t i = 0; i != dispatch.rangeCount; ++i)
	{
		AIPlayerPlanningJob::ExecutionRecord &record = execution[i];
		performance::KernelPerformanceInlineBody body;
		const performance::KernelPerformanceInlineAction action = ledger.beginInlineBody(attempt,
			record.range, performance::KernelPerformanceLedger::instance(), body, record.checkpoint);
		if (action == performance::KERNEL_INLINE_INVALID) return false;
		if (action == performance::KERNEL_INLINE_EXECUTE)
		{
			record.traceSource = true;
			if (record.range.bodyKind == 1)
			{
				const uint32_t player = static_cast<uint32_t>(record.range.begin);
				AIPlayerPlanningJob job(snapshots + player, results + player, &record,
					0, 0, floatingPointState, transport->testHooks, player);
				job.executeInline();
			}
			else
			{
				const uint32_t player = static_cast<uint32_t>(record.range.begin /
					AI_PLANNING_MAX_PRODUCTION_CANDIDATES);
				const uint32_t begin = static_cast<uint32_t>(record.range.begin %
					AI_PLANNING_MAX_PRODUCTION_CANDIDATES);
				AIProductionCandidateShardJob job(&snapshots[player].production,
					shardedFacts[player], verificationFacts[player], begin,
					begin + static_cast<uint32_t>(record.range.operationCount), &record,
					0, 0, floatingPointState, transport->testHooks, player);
				job.executeInline();
			}
		}
		performance::KernelPerformanceRangeProgress progress = {};
		progress.checkpoint = record.checkpoint.snapshot();
		progress.publication = !progress.checkpoint.entered ? performance::KERNEL_PUBLICATION_NOT_APPLICABLE :
			sourceCancelled || progress.checkpoint.terminal == performance::KERNEL_RANGE_CANCELLED ?
			performance::KERNEL_PUBLICATION_DISCARDED_AFTER_CANCEL : record.completed ?
			performance::KERNEL_PUBLICATION_PUBLISHED : performance::KERNEL_PUBLICATION_REJECTED;
		if (action == performance::KERNEL_INLINE_EXECUTE && !ledger.finishInlineBody(body, progress))
			return false;
		if (!ledger.observeReleasedRange(attempt, record.range, progress)) return false;
		completed = record.completed && completed;
	}
	return completed;
}
#endif

bool RunAIPlanningJobs(const AIPlayerPlanningSnapshot *snapshots,
	uint32_t snapshotCount, AIPlayerPlanningResult *results, void *userData)
{
	AIPlanningJobSystemEvidence *evidence =
		static_cast<AIPlanningJobSystemEvidence *>(userData);
	#if defined(_WIN64)
	performance::KernelPerformanceBatch *performanceBatch =
		evidence != 0 ? evidence->performanceBatch : 0;
	AIPlanningReferenceBatchTransport *referenceBatch =
		evidence != 0 ? evidence->referenceBatch : 0;
	AIPlanningTestHooks *testHooks = referenceBatch != 0 ? referenceBatch->testHooks : 0;
	const bool sourceBoundInline = referenceBatch != 0 && referenceBatch->referenceLedger != 0 &&
		referenceBatch->referenceLedger->runMode() == performance::KERNEL_REFERENCE_PHASE_BASELINE_BINDING;
	#endif
	if (evidence != 0) *evidence = AIPlanningJobSystemEvidence();
	#if defined(_WIN64)
	if (evidence != 0) evidence->performanceBatch = performanceBatch;
	if (evidence != 0) evidence->referenceBatch = referenceBatch;
	#endif
	// Admit only a shape with at least two meaningful jobs. A single due player
	// can still qualify when its immutable production source view spans multiple
	// candidate shards; one small player remains on the serial oracle.
	if (CountAIPlanningJobs(snapshots, snapshotCount) < 2U)
		return false;
	JobSystem &jobs = JobSystem::instance();
#if defined(_WIN64)
	AIPlayerPlanningJob::ExecutionRecord execution[
		AI_PLANNING_MAX_JOB_EXECUTIONS];
	AIPlanningSourceAttempt source(referenceBatch, execution, snapshotCount,
		CountAIPlanningJobs(snapshots, snapshotCount), jobs);
#endif
	if (
#if defined(_WIN64)
		!sourceBoundInline &&
#endif
		(!jobs.isRunning() || jobs.isWorkerThread() ||
		!jobs.isCurrentThread(JOB_OWNER_GAME) || jobs.workerCount() == 0U)
		)
		return false;

#if defined(_WIN64)
	AIPlanningPerformanceInterval schedule(
		evidence != 0 ? evidence->performanceBatch : 0,
		performance::KERNEL_PERFORMANCE_SCHEDULE);
#endif
#if !defined(_WIN64)
	AIPlayerPlanningJob::ExecutionRecord execution[
		AI_PLANNING_MAX_JOB_EXECUTIONS];
#endif
	JobGroup group =
#if defined(_WIN64)
		sourceBoundInline ? JobGroup() :
#endif
		jobs.createGroup();
	if (
#if defined(_WIN64)
		!sourceBoundInline &&
#endif
		!group.isValid())
		return false;
	std::atomic<uint32_t> activePhysicalWorkers(0U);
	std::atomic<uint32_t> peakPhysicalWorkers(0U);
	const JobFloatingPointState floatingPointState;
	typedef AIProductionCandidateFact AIProductionCandidateFactTable[
		AI_PLANNING_MAX_PRODUCTION_CANDIDATES];
	std::unique_ptr<AIProductionCandidateFactTable[]> shardedFacts;
	std::unique_ptr<AIProductionCandidateFactTable[]> verificationFacts;
	for (uint32_t player = 0U; player < snapshotCount; ++player)
	{
		if (snapshots[player].planProduction != 0U &&
			snapshots[player].production.sourceFacts.valid != 0U)
		{
			shardedFacts.reset(new (std::nothrow)
				AIProductionCandidateFactTable[AI_PLANNING_MAX_PLAYERS]);
			verificationFacts.reset(new (std::nothrow)
				AIProductionCandidateFactTable[AI_PLANNING_MAX_PLAYERS]);
			if (!shardedFacts || !verificationFacts)
				return false;
			break;
		}
	}
	bool sourceSharded[AI_PLANNING_MAX_PLAYERS] = { false };
	uint32_t submitted = 0U;
	bool submissionFailed = false;
#if defined(_WIN64)
	if (sourceBoundInline)
	{
		schedule.end();
		bool admissionAccepted = false;
		if (!ConsumeAIPlanningRanges(referenceBatch, snapshots, snapshotCount, results,
			shardedFacts.get(), verificationFacts.get(), execution,
			floatingPointState, &admissionAccepted))
		{
			if (evidence != 0)
				evidence->nativeAdmissionAccepted = admissionAccepted;
			return false;
		}
		submitted = CountAIPlanningJobs(snapshots, snapshotCount);
		for (uint32_t player = 0; player != snapshotCount; ++player)
			sourceSharded[player] = snapshots[player].planProduction != 0 &&
				snapshots[player].production.sourceFacts.valid != 0;
		if (evidence != 0)
		{
			evidence->sourceBoundInline = true;
			evidence->nativeAdmissionAccepted = admissionAccepted;
		}
	}
	else
	{
#endif
	for (uint32_t player = 0U; player < snapshotCount && !submissionFailed;
		++player)
	{
		const AIProductionPlanningSnapshot &production = snapshots[player].production;
		if (snapshots[player].planProduction != 0U &&
			production.sourceFacts.valid != 0U)
		{
			sourceSharded[player] = true;
			for (uint32_t begin = 0U; begin < production.candidateCount;
				begin += AI_PLANNING_PRODUCTION_SHARD_SIZE)
			{
				if (submitted >= AI_PLANNING_MAX_JOB_EXECUTIONS)
				{
					submissionFailed = true;
					break;
				}
				const uint32_t end = begin + AI_PLANNING_PRODUCTION_SHARD_SIZE <
					production.candidateCount ? begin +
					AI_PLANNING_PRODUCTION_SHARD_SIZE : production.candidateCount;
#if defined(_WIN64)
				source.plan(submitted, 2, player * AI_PLANNING_MAX_PRODUCTION_CANDIDATES + begin,
					player * AI_PLANNING_MAX_PRODUCTION_CANDIDATES + end);
#endif
				AIProductionCandidateShardJob *job = new (std::nothrow)
					AIProductionCandidateShardJob(&production,
						shardedFacts[player], verificationFacts[player], begin,
						end, execution + submitted,
						&activePhysicalWorkers, &peakPhysicalWorkers,
						floatingPointState
#if defined(_WIN64)
						, testHooks, player
#endif
						);
				JobHandle handle = job != 0 ? jobs.trySubmit(job,
					JOB_PRIORITY_FRAME_CRITICAL, group) : JobHandle();
				if (!handle.isValid())
				{
					delete job;
					submissionFailed = true;
					break;
				}
				++submitted;
				if (evidence != 0) evidence->nativeAdmissionAccepted = true;
				s_submittedJobs.fetch_add(1U, std::memory_order_relaxed);
			}
		}
		else
		{
			if (submitted >= AI_PLANNING_MAX_JOB_EXECUTIONS)
			{
				submissionFailed = true;
				break;
			}
#if defined(_WIN64)
			source.plan(submitted, 1, player, player + 1);
#endif
			AIPlayerPlanningJob *job = new (std::nothrow) AIPlayerPlanningJob(
				snapshots + player, results + player, execution + submitted,
				&activePhysicalWorkers, &peakPhysicalWorkers,
				floatingPointState
#if defined(_WIN64)
				, testHooks, player
#endif
				);
			JobHandle handle = job != 0 ? jobs.trySubmit(job,
				JOB_PRIORITY_FRAME_CRITICAL, group) : JobHandle();
			if (!handle.isValid())
			{
				delete job;
				submissionFailed = true;
				break;
			}
			++submitted;
			if (evidence != 0) evidence->nativeAdmissionAccepted = true;
			s_submittedJobs.fetch_add(1U, std::memory_order_relaxed);
		}
	}

#if defined(_WIN64)
	schedule.end();
#endif
	if (submissionFailed)
	{
		jobs.cancel(group);
		jobs.wait(group);
#if defined(_WIN64)
		source.release(submitted, group.wasCancelled());
#endif
		if (evidence != 0) evidence->collect(execution, submitted,
			peakPhysicalWorkers.load(std::memory_order_relaxed));
		return false;
	}
	// Do not manufacture a parallel result by letting the owner execute every
	// job. A short passive fence either proves physical-worker progress or
	// cancels and falls back to the serial oracle.
	const unsigned physicalCompletionTimeoutMilliseconds =
#if defined(_WIN64)
		// This bounded rendezvous is a focused-test control, not evidence of
		// ordinary eight-millisecond admission or installed qualification.
		testHooks != 0 && testHooks->rendezvous != 0 ? 1000U :
#endif
		8U;
#if defined(_WIN64)
	if (testHooks != 0 && testHooks->beforeWait != 0) testHooks->beforeWait(testHooks->context);
	AIPlanningPerformanceInterval wait(
		evidence != 0 ? evidence->performanceBatch : 0,
		performance::KERNEL_PERFORMANCE_WAIT);
#endif
	const bool passiveWaitCompleted = jobs.waitWithoutOwnerHelp(group,
		physicalCompletionTimeoutMilliseconds);
#if defined(_WIN64)
	wait.end();
#endif
	if (!passiveWaitCompleted)
	{
		jobs.cancel(group);
#if defined(_WIN64)
		if (testHooks != 0 && testHooks->afterCancel != 0) testHooks->afterCancel(testHooks->context);
#endif
		jobs.wait(group);
#if defined(_WIN64)
		source.release(submitted, group.wasCancelled());
#endif
		if (evidence != 0) evidence->collect(execution, submitted,
			peakPhysicalWorkers.load(std::memory_order_relaxed));
		return false;
	}
#if defined(_WIN64)
	source.release(submitted, group.wasCancelled());
#endif
	if (group.failed() || group.wasCancelled())
	{
		if (evidence != 0) evidence->collect(execution, submitted,
			peakPhysicalWorkers.load(std::memory_order_relaxed));
		return false;
	}
#if defined(_WIN64)
	}
#endif
	for (uint32_t i = 0U; i < submitted; ++i)
	{
		if (!execution[i].completed)
		{
			if (evidence != 0) evidence->collect(execution, submitted,
				peakPhysicalWorkers.load(std::memory_order_relaxed));
			return false;
		}
	}
	// Candidate shards have completed on physical workers. Compare the two
	// independently derived POD outputs before either can become authoritative,
	// then run the small deterministic winner reduction exactly once.
#if defined(_WIN64)
	AIPlanningPerformanceInterval validate(
		evidence != 0 ? evidence->performanceBatch : 0,
		performance::KERNEL_PERFORMANCE_VALIDATE);
#endif
	for (uint32_t player = 0U; player < snapshotCount; ++player)
	{
		if (sourceSharded[player])
		{
			AIPlayerPlanningSnapshot prepared = snapshots[player];
			prepared.production.sourceFacts.valid = 0U;
#if defined(_WIN64)
			ObserveAIPlanningTest(testHooks, AI_PLANNING_TEST_OWNER_FACT_COMPARISON,
				player, 0, prepared.production.candidateCount);
#endif
			if (!ValidateAIProductionPreparedFacts(shardedFacts[player],
				verificationFacts[player], prepared.production.candidateCount))
				return false;
			for (uint32_t i = 0U; i < prepared.production.candidateCount; ++i)
			{
				prepared.production.candidates[i] = shardedFacts[player][i];
			}
#if defined(_WIN64)
			ObserveAIPlanningTest(testHooks, AI_PLANNING_TEST_OWNER_WINNER,
				player, 0, prepared.production.candidateCount);
#endif
			if (!PlanAIPlayer(prepared, &results[player]))
				return false;
		}
		else
		{
#if defined(_WIN64)
			ObserveAIPlanningTest(testHooks, AI_PLANNING_TEST_OWNER_VALIDATION, player, 0, 1);
#endif
			if (!ValidateAIPlayerPlanningResult(snapshots[player], results[player])) return false;
		}
	}
#if defined(_WIN64)
	const bool referenceObserved = ObserveAIPlanningReferenceBatch(
		performanceBatch, referenceBatch);
	validate.end();
	if (referenceBatch != 0 && referenceBatch->referenceAttempt.valid() &&
		!referenceObserved)
		return false;
	if (sourceBoundInline)
	{
		performance::KernelPerformanceAttemptFinish sourceFinish = {};
		if (!referenceObserved || referenceBatch == 0 ||
			referenceBatch->referenceLedger == 0 ||
			!referenceBatch->referenceLedger->readSourceFinish(
				referenceBatch->referenceAttempt, sourceFinish) ||
			!sourceFinish.validationObserved ||
			(sourceFinish.disposition != performance::KERNEL_PERFORMANCE_COMMITTED &&
			 sourceFinish.disposition != performance::KERNEL_PERFORMANCE_ABORTED_AFTER_ADMISSION))
			return false;
		evidence->sourceOwnerCommitAllowed = sourceFinish.disposition ==
			performance::KERNEL_PERFORMANCE_COMMITTED;
	}
#endif
	if (evidence != 0) evidence->collect(execution, submitted,
		peakPhysicalWorkers.load(std::memory_order_relaxed));
	if (evidence != 0) evidence->resultsValidated = true;
	return evidence != 0 &&
#if defined(_WIN64)
		(evidence->sourceBoundInline ||
#endif
		(evidence->ownerHelpedJobs == 0U &&
		evidence->distinctPhysicalWorkers > 1U &&
		evidence->peakConcurrentPhysicalWorkers > 1U)
#if defined(_WIN64)
		)
#endif
		;
}
}

uint32_t CountAIPlanningBatchJobs(const AIPlayerPlanningSnapshot *snapshots,
	uint32_t snapshotCount)
{
	return snapshots != 0 ? CountAIPlanningJobs(snapshots, snapshotCount) : 0U;
}

#if defined(_WIN64)
AIPlanningPerformanceInterval::AIPlanningPerformanceInterval(
	performance::KernelPerformanceBatch *batch,
	performance::KernelPerformanceStage stage) : m_ledger(0), m_interval()
{
	if (batch != 0 && batch->valid())
	{
		m_ledger = &performance::KernelPerformanceLedger::instance();
		m_interval = m_ledger->beginInterval(*batch, stage);
	}
}

AIPlanningPerformanceInterval::~AIPlanningPerformanceInterval()
{
	end();
}

bool AIPlanningPerformanceInterval::end()
{
	if (m_ledger == 0 || !m_interval.valid())
		return false;
	const bool ended = m_ledger->endInterval(m_interval);
	m_interval = performance::KernelPerformanceInterval();
	return ended;
}

bool AIPlanningPerformanceInterval::valid() const
{
	return m_ledger != 0 && m_interval.valid();
}

AIPlanningPerformanceBatchScope::AIPlanningPerformanceBatchScope(bool enabled,
	performance::KernelPerformanceKernel kernel, unsigned subtype,
	unsigned frame, JobMetricCounter ordinal) : m_ledger(0), m_batch(),
	m_interval(), m_closed(false)
{
	// Do not touch the ledger while disabled. In particular, this keeps a
	// disabled title path from reading its clock or changing diagnostic state.
	if (!enabled)
		return;
	m_ledger = &performance::KernelPerformanceLedger::instance();
	m_batch = m_ledger->beginBatch(kernel, subtype, frame, ordinal);
}

AIPlanningPerformanceBatchScope::~AIPlanningPerformanceBatchScope()
{
	abort();
}

performance::KernelPerformanceBatch *AIPlanningPerformanceBatchScope::token()
{
	return m_ledger != 0 && m_batch.valid() && !m_closed ? &m_batch : 0;
}

void AIPlanningPerformanceBatchScope::begin(
	performance::KernelPerformanceStage stage)
{
	if (m_closed || m_ledger == 0 || !m_batch.valid())
		return;
	end();
	m_interval = m_ledger->beginInterval(m_batch, stage);
}

bool AIPlanningPerformanceBatchScope::end()
{
	if (m_ledger == 0 || !m_interval.valid())
		return false;
	const bool ended = m_ledger->endInterval(m_interval);
	m_interval = performance::KernelPerformanceInterval();
	return ended;
}

void AIPlanningPerformanceBatchScope::finish(
	performance::KernelPerformanceDisposition disposition)
{
	if (m_closed)
		return;
	end();
	if (m_ledger != 0 && m_batch.valid())
		m_ledger->endBatch(m_batch, disposition);
	m_closed = true;
}

void AIPlanningPerformanceBatchScope::notAdmitted()
{
	finish(performance::KERNEL_PERFORMANCE_NOT_ADMITTED);
}

void AIPlanningPerformanceBatchScope::abort()
{
	finish(performance::KERNEL_PERFORMANCE_ABORTED_AFTER_ADMISSION);
}

void AIPlanningPerformanceBatchScope::commit()
{
	finish(performance::KERNEL_PERFORMANCE_COMMITTED);
}

AIPlanningReferenceBatchTransport::AIPlanningReferenceBatchTransport() :
	referenceLedger(0), referenceBatch(0), writeInput(0), immutableInput(0),
	writeOutput(0), productionOutput(0), serialCompute(0),
		detachedSerialOutput(0), operationCount(0), fieldSchema(1U),
	referenceAttempt(), testHooks(0)
{
}

namespace
{
bool WriteAIPlanningReferenceOrderKey(
	performance::KernelPerformanceCanonicalWriter &writer,
	const AIPlanningOrderKey &key, unsigned tag)
{
	return writer.u32(tag + 0U, key.frame) &&
		writer.u32(tag + 1U, key.playerIndex) &&
		writer.u32(tag + 2U, key.subphase) &&
		writer.u32(tag + 3U, key.sourceOrdinal) &&
		writer.u32(tag + 4U, key.emissionOrdinal);
}

bool WriteAIPlanningReferenceCounterKey(
	performance::KernelPerformanceCanonicalWriter &writer,
	const AICounterRngKey &key, unsigned tag)
{
	return writer.u32(tag + 0U, key.simulationEpoch) &&
		writer.u32(tag + 1U, key.matchSeed) &&
		writer.u32(tag + 2U, key.frame) &&
		writer.u32(tag + 3U, key.domain) &&
		writer.u32(tag + 4U, key.playerIndex) &&
		writer.u32(tag + 5U, key.ownerStableId) &&
		writer.u32(tag + 6U, key.sourceStableId) &&
		writer.u32(tag + 7U, key.eventKind) &&
		writer.u32(tag + 8U, key.eventOrdinal) &&
		writer.u32(tag + 9U, key.drawOrdinal);
}

bool WriteAIPlanningReferenceCounterLedger(
	performance::KernelPerformanceCanonicalWriter &writer,
	const AICounterRngLedgerRecord &record, unsigned tag)
{
	return WriteAIPlanningReferenceCounterKey(writer, record.key, tag) &&
		writer.u32(tag + 10U, record.algorithm) &&
		writer.u32(tag + 11U, record.rawValue) &&
		writer.u32(tag + 12U, record.exclusiveUpperBound) &&
		writer.u32(tag + 13U, record.selectedIndex) &&
		writer.u32(tag + 14U, record.valid);
}

bool WriteAIPlanningReferenceEnemySnapshot(
	performance::KernelPerformanceCanonicalWriter &writer,
	const AIEnemyPlanningSnapshot &snapshot, unsigned tag)
{
	if (snapshot.candidateCount > AI_PLANNING_MAX_PLAYERS ||
		!writer.u32(tag + 0U, snapshot.frame) ||
		!writer.u32(tag + 1U, snapshot.ownerPlayerIndex) ||
		!writer.i32(tag + 2U, snapshot.currentEnemyPlayerIndex) ||
		!writer.i32(tag + 3U, snapshot.switchScoreThreshold) ||
		!writer.sequence(tag + 4U, snapshot.candidateCount))
		return false;
	for (uint32_t i = 0U; i < snapshot.candidateCount; ++i)
	{
		const AIEnemyCandidateFact &candidate = snapshot.candidates[i];
		if (!writer.u32(tag + 5U, candidate.sourceOrdinal) ||
			!writer.i32(tag + 6U, candidate.playerIndex) ||
			!writer.i32(tag + 7U, candidate.knownAssetValue) ||
			!writer.i32(tag + 8U, candidate.distance) ||
			!writer.i32(tag + 9U, candidate.alliedAIsTargeting) ||
			!writer.i32(tag + 10U, candidate.routeClass) ||
			!writer.u32(tag + 11U, candidate.hasKnownPosition) ||
			!writer.u32(tag + 12U, candidate.targetingThisAI) ||
			!writer.u32(tag + 13U, candidate.hasKnownObject) ||
			!writer.u32(tag + 14U, candidate.hasKnownUnit) ||
			!writer.u32(tag + 15U, candidate.hasKnownBuildFacility))
			return false;
	}
	return true;
}

bool WriteAIPlanningReferenceProductionSnapshot(
	performance::KernelPerformanceCanonicalWriter &writer,
	const AIProductionPlanningSnapshot &snapshot, unsigned tag)
{
	const AIProductionPlanningSourceFacts &facts = snapshot.sourceFacts;
	if (snapshot.candidateCount > AI_PLANNING_MAX_PRODUCTION_CANDIDATES ||
		facts.factoryCount > AI_PLANNING_MAX_PRODUCTION_FACTORIES ||
		!writer.u32(tag + 0U, snapshot.frame) ||
		!writer.u32(tag + 1U, snapshot.ownerPlayerIndex) ||
		!writer.i32(tag + 2U, snapshot.resources) ||
		!writer.i32(tag + 3U, snapshot.logicFramesPerSecond) ||
		!writer.i32(tag + 4U, snapshot.initialReserve) ||
		!writer.i32(tag + 5U, snapshot.retryReserve) ||
		!writer.i32(tag + 6U, snapshot.difficulty) ||
		!writer.i32(tag + 7U, snapshot.contextInfluencePercent) ||
		!writer.u32(tag + 8U, snapshot.retryWithoutInitialReserve) ||
		!writer.sequence(tag + 9U, snapshot.candidateCount) ||
		!WriteAIPlanningReferenceCounterKey(writer, snapshot.tieBreakKey,
			tag + 10U))
		return false;
	for (uint32_t i = 0U; i < snapshot.candidateCount; ++i)
	{
		const AIProductionCandidateFact &candidate = snapshot.candidates[i];
		if (!writer.u32(tag + 20U, candidate.sourceOrdinal) ||
			!writer.u32(tag + 21U, candidate.candidateStableId) ||
			!writer.i32(tag + 22U, candidate.configuredPriority) ||
			!writer.i32(tag + 23U, candidate.counterFitScore) ||
			!writer.i32(tag + 24U, candidate.minimumCost) ||
			!writer.i32(tag + 25U, candidate.plannedCost) ||
			!writer.i32(tag + 26U, candidate.factoryWaitFrames) ||
			!writer.i32(tag + 27U, candidate.routeClass) ||
			!writer.i32(tag + 28U, candidate.recentLossCount) ||
			!writer.i32(tag + 29U, candidate.recentPathFailureCount) ||
			!writer.u32(tag + 30U, candidate.eligible))
			return false;
	}

	if (!writer.u32(tag + 40U, facts.valid) ||
		!writer.u32(tag + 41U, facts.factoryCount) ||
		!writer.i32(tag + 42U, facts.enemyAircraftValue) ||
		!writer.i32(tag + 43U, facts.enemyVehicleValue) ||
		!writer.i32(tag + 44U, facts.enemyInfantryValue) ||
		!writer.u32(tag + 45U, facts.hasRouteTarget) ||
		!writer.u32(tag + 46U, facts.groundRouteKnown) ||
		!writer.u32(tag + 47U, facts.groundRouteReachable) ||
		!writer.sequence(tag + 48U, facts.factoryCount) ||
		!writer.sequence(tag + 49U, snapshot.candidateCount))
		return false;
	for (uint32_t i = 0U; i < facts.factoryCount; ++i)
	{
		const AIProductionFactorySourceFact &factory = facts.factories[i];
		if (!writer.i32(tag + 50U, factory.projectedFrames) ||
			!writer.u32(tag + 51U, factory.valid) ||
			!writer.u32(tag + 52U, factory.idle))
			return false;
	}
	for (uint32_t i = 0U; i < snapshot.candidateCount; ++i)
	{
		const AIProductionCandidateSourceFact &candidate = facts.candidates[i];
		if (candidate.unitCount > AI_PLANNING_MAX_PRODUCTION_UNITS ||
			!writer.u32(tag + 60U, candidate.valid) ||
			!writer.u32(tag + 61U, candidate.unitCount) ||
			!writer.sequence(tag + 62U, candidate.unitCount))
			return false;
		for (uint32_t unitIndex = 0U; unitIndex < candidate.unitCount;
			++unitIndex)
		{
			const AIProductionUnitSourceFact &unit = candidate.units[unitIndex];
			if (!writer.i32(tag + 63U, unit.cost) ||
				!writer.i32(tag + 64U, unit.buildFrames) ||
				!writer.i32(tag + 65U, unit.minUnits) ||
				!writer.i32(tag + 66U, unit.maxUnits) ||
				!writer.u32(tag + 67U, unit.flags) ||
				!writer.u32(tag + 68U, unit.compatibleFactoryMask) ||
				!writer.sequence(tag + 69U, facts.factoryCount))
				return false;
			for (uint32_t factory = 0U; factory < facts.factoryCount; ++factory)
				if (!writer.u32(tag + 70U,
					unit.productionQuantity[factory]))
					return false;
		}
	}
	return true;
}

bool WriteAIPlanningReferenceEnemyResult(
	performance::KernelPerformanceCanonicalWriter &writer,
	const AIEnemyPlanningResult &result, unsigned tag)
{
	if (result.candidateScoreCount > AI_PLANNING_MAX_PLAYERS ||
		!writer.u32(tag + 0U, result.valid) ||
		!writer.i32(tag + 1U, result.selectedPlayerIndex) ||
		!writer.i32(tag + 2U, result.selectedScore) ||
		!writer.i32(tag + 3U, result.bestPlayerIndex) ||
		!writer.i32(tag + 4U, result.bestScore) ||
		!writer.u32(tag + 5U, result.candidateScoreCount) ||
		!WriteAIPlanningReferenceOrderKey(writer, result.orderKey, tag + 6U) ||
		!writer.sequence(tag + 11U, result.candidateScoreCount))
		return false;
	for (uint32_t i = 0U; i < result.candidateScoreCount; ++i)
	{
		const AIEnemyCandidateScore &score = result.candidateScores[i];
		if (!writer.u32(tag + 12U, score.sourceOrdinal) ||
			!writer.i32(tag + 13U, score.playerIndex) ||
			!writer.i32(tag + 14U, score.knownAssetScore) ||
			!writer.i32(tag + 15U, score.retaliationScore) ||
			!writer.i32(tag + 16U, score.routeScore) ||
			!writer.i32(tag + 17U, score.allyTargetScore) ||
			!writer.i32(tag + 18U, score.distanceScore) ||
			!writer.i32(tag + 19U, score.crippledScore) ||
			!writer.i32(tag + 20U, score.totalScore))
			return false;
	}
	return true;
}

bool WriteAIPlanningReferenceProductionResult(
	performance::KernelPerformanceCanonicalWriter &writer,
	const AIProductionPlanningResult &result, unsigned tag)
{
	if (result.candidateScoreCount > AI_PLANNING_MAX_PRODUCTION_CANDIDATES ||
		!writer.u32(tag + 0U, result.valid) ||
		!writer.u32(tag + 1U, result.hasSelection) ||
		!writer.u32(tag + 2U, result.selectedSourceOrdinal) ||
		!writer.u32(tag + 3U, result.selectedStableId) ||
		!writer.u64(tag + 4U,
			static_cast<JobMetricCounter>(result.selectedScore)) ||
		!writer.i32(tag + 5U, result.highestPriority) ||
		!writer.i32(tag + 6U, result.committedReserve) ||
		!writer.u32(tag + 7U, result.tieCount) ||
		!writer.u32(tag + 8U, result.usedRetryReserve) ||
		!writer.u32(tag + 9U, result.candidateScoreCount) ||
		!WriteAIPlanningReferenceOrderKey(writer, result.orderKey, tag + 10U) ||
		!WriteAIPlanningReferenceCounterLedger(writer, result.randomLedger,
			tag + 15U) ||
		!writer.sequence(tag + 30U, result.candidateScoreCount))
		return false;
	for (uint32_t i = 0U; i < result.candidateScoreCount; ++i)
	{
		const AIProductionCandidateScore &score = result.candidateScores[i];
		if (!writer.u32(tag + 31U, score.sourceOrdinal) ||
			!writer.u32(tag + 32U, score.candidateStableId) ||
			!writer.i32(tag + 33U, score.counterFitScore) ||
			!writer.i32(tag + 34U, score.economyScore) ||
			!writer.i32(tag + 35U, score.factoryWaitScore) ||
			!writer.i32(tag + 36U, score.routeScore) ||
			!writer.i32(tag + 37U, score.lossScore) ||
			!writer.i32(tag + 38U, score.pathFailureScore) ||
			!writer.i32(tag + 39U, score.rawContextScore) ||
			!writer.u64(tag + 40U,
				static_cast<JobMetricCounter>(score.finalScore)) ||
			!writer.u32(tag + 41U, score.considered))
			return false;
	}
	return true;
}

bool WriteAIPlanningReferenceInputFields(
	performance::KernelPerformanceCanonicalWriter &writer,
	const AIPlanningReferencePlayerInputView &view)
{
	if (!view.snapshots || view.count == 0U ||
		view.count > AI_PLANNING_MAX_PLAYERS || view.subtype > 1U ||
		!writer.u32(1U, view.subtype) || !writer.sequence(2U, view.count))
		return false;
	for (uint32_t i = 0U; i < view.count; ++i)
	{
		const AIPlayerPlanningSnapshot &snapshot = view.snapshots[i];
		if (!writer.u32(3U, snapshot.frame) ||
			!writer.u32(4U, snapshot.playerIndex) ||
			!writer.u32(5U, snapshot.planEnemyTarget) ||
			!writer.u32(6U, snapshot.planProduction))
			return false;
		if (view.subtype == AI_PLANNING_SUBPHASE_ENEMY_TARGET)
		{
			if (snapshot.planEnemyTarget == 0U ||
				!WriteAIPlanningReferenceEnemySnapshot(writer,
					snapshot.enemyTarget, 10U))
				return false;
		}
		else if (snapshot.planProduction == 0U ||
			!WriteAIPlanningReferenceProductionSnapshot(writer,
				snapshot.production, 100U))
			return false;
	}
	return true;
}

bool WriteAIPlanningReferenceOutputFields(
	performance::KernelPerformanceCanonicalWriter &writer,
	const AIPlanningReferencePlayerOutputView &view)
{
	if (!view.results || view.count == 0U ||
		view.count > AI_PLANNING_MAX_PLAYERS || view.subtype > 1U ||
		!writer.u32(1U, view.subtype) || !writer.sequence(2U, view.count))
		return false;
	for (uint32_t i = 0U; i < view.count; ++i)
	{
		const AIPlayerPlanningResult &result = view.results[i];
		if (!writer.u32(3U, result.valid) ||
			!writer.u32(4U, result.playerIndex))
			return false;
		if (view.subtype == AI_PLANNING_SUBPHASE_ENEMY_TARGET)
		{
			if (!WriteAIPlanningReferenceEnemyResult(writer,
				result.enemyTarget, 10U))
				return false;
		}
		else if (!WriteAIPlanningReferenceProductionResult(writer,
			result.production, 100U))
			return false;
	}
	return true;
}
}

bool WriteAIPlanningReferenceInput(
	performance::KernelPerformanceCanonicalWriter &writer,
	const void *context)
{
	const AIPlanningReferencePlayerInputView *view =
		static_cast<const AIPlanningReferencePlayerInputView *>(context);
	return view != 0 && WriteAIPlanningReferenceInputFields(writer, *view);
}

bool WriteAIPlanningReferenceOutput(
	performance::KernelPerformanceCanonicalWriter &writer,
	const void *context)
{
	const AIPlanningReferencePlayerOutputView *view =
		static_cast<const AIPlanningReferencePlayerOutputView *>(context);
	return view != 0 && WriteAIPlanningReferenceOutputFields(writer, *view);
}

bool ComputeAIPlanningReferenceSerial(const void *immutableInput,
	void *detachedOutput)
{
	const AIPlanningReferencePlayerInputView *inputView =
		static_cast<const AIPlanningReferencePlayerInputView *>(immutableInput);
	AIPlanningReferencePlayerOutputView *outputView =
		static_cast<AIPlanningReferencePlayerOutputView *>(detachedOutput);
	return inputView != 0 && outputView != 0 && inputView->snapshots != 0 &&
		outputView->results != 0 && inputView->count != 0U &&
		inputView->count <= AI_PLANNING_MAX_PLAYERS &&
		outputView->count == inputView->count &&
		outputView->subtype == inputView->subtype &&
		PlanAIPlayerBatchSerial(inputView->snapshots, inputView->count,
			outputView->results);
}

bool ObserveAIPlanningReferenceBatch(
	performance::KernelPerformanceBatch *timingBatch,
	AIPlanningReferenceBatchTransport *transport) noexcept
{
	if (transport == 0 || transport->referenceLedger == 0 ||
		transport->referenceBatch == 0 || timingBatch == 0)
		return false;
	*transport->referenceBatch = performance::KernelPerformanceReferenceBatch();
	if (transport->referenceLedger->mode() ==
		performance::KERNEL_REFERENCE_DISABLED)
		return false;
	performance::KernelPerformanceBatchIdentity identity;
	if (!performance::KernelPerformanceLedger::instance().describeBatch(
		*timingBatch, identity))
		return false;
	const performance::KernelPerformanceReferenceBatch observed = transport->referenceAttempt.valid() ?
		transport->referenceLedger->observeValidatedAttempt(transport->referenceAttempt,
			transport->writeOutput, transport->productionOutput) :
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

bool FinishAIPlanningReferenceBatch(
	AIPlanningReferenceBatchTransport *transport, bool committed) noexcept
{
	if (transport == 0 || transport->referenceLedger == 0 ||
		transport->referenceBatch == 0 ||
		!transport->referenceBatch->valid())
		return false;
	const bool finished = transport->referenceLedger->finishBatch(
		*transport->referenceBatch, committed);
	if (finished)
		*transport->referenceBatch = performance::KernelPerformanceReferenceBatch();
	return finished;
}
#endif

void ClearAIEnemyPlanningSnapshot(AIEnemyPlanningSnapshot *snapshot)
{
	if (snapshot != 0)
	{
		memset(snapshot, 0, sizeof(*snapshot));
		snapshot->currentEnemyPlayerIndex = -1;
		snapshot->switchScoreThreshold = 200;
	}
}

bool ValidateAIProductionPreparedFacts(
	const AIProductionCandidateFact *prepared,
	const AIProductionCandidateFact *verification, uint32_t candidateCount)
{
	if (candidateCount > AI_PLANNING_MAX_PRODUCTION_CANDIDATES ||
		(candidateCount != 0U && (prepared == 0 || verification == 0)))
		return false;
	for (uint32_t i = 0U; i < candidateCount; ++i)
	{
		if (!EqualProductionCandidateFact(prepared[i], verification[i]))
			return false;
	}
	return true;
}

void ClearAIProductionPlanningSnapshot(AIProductionPlanningSnapshot *snapshot)
{
	if (snapshot != 0)
		memset(snapshot, 0, sizeof(*snapshot));
}

void ClearAIPlayerPlanningSnapshot(AIPlayerPlanningSnapshot *snapshot)
{
	if (snapshot != 0)
	{
		memset(snapshot, 0, sizeof(*snapshot));
		ClearAIEnemyPlanningSnapshot(&snapshot->enemyTarget);
		ClearAIProductionPlanningSnapshot(&snapshot->production);
	}
}

void ClearAIPlayerPlanningResult(AIPlayerPlanningResult *result)
{
	if (result != 0)
	{
		memset(result, 0, sizeof(*result));
		result->enemyTarget.selectedPlayerIndex = -1;
		result->enemyTarget.bestPlayerIndex = -1;
		result->enemyTarget.orderKey.sourceOrdinal = AI_PLANNING_INVALID_ORDINAL;
		result->production.selectedSourceOrdinal = AI_PLANNING_INVALID_ORDINAL;
		result->production.orderKey.sourceOrdinal = AI_PLANNING_INVALID_ORDINAL;
	}
}

bool PlanAIEnemyTarget(const AIEnemyPlanningSnapshot &snapshot,
	AIEnemyPlanningResult *result)
{
	if (result == 0 || snapshot.candidateCount > AI_PLANNING_MAX_PLAYERS)
		return false;

	memset(result, 0, sizeof(*result));
	result->selectedPlayerIndex = -1;
	result->bestPlayerIndex = -1;
	result->candidateScoreCount = snapshot.candidateCount;
	result->orderKey = MakeOrderKey(snapshot.frame, snapshot.ownerPlayerIndex,
		AI_PLANNING_SUBPHASE_ENEMY_TARGET, AI_PLANNING_INVALID_ORDINAL);

	int32_t maximumKnownAssetValue = 0;
	int32_t minimumDistance = 2147483647;
	int32_t maximumDistance = 0;
	bool hasKnownDistance = false;
	for (uint32_t i = 0; i < snapshot.candidateCount; ++i)
	{
		const AIEnemyCandidateFact &candidate = snapshot.candidates[i];
		if (candidate.knownAssetValue > maximumKnownAssetValue)
			maximumKnownAssetValue = candidate.knownAssetValue;
		if (candidate.hasKnownPosition != 0U)
		{
			if (candidate.distance < minimumDistance)
				minimumDistance = candidate.distance;
			if (candidate.distance > maximumDistance)
				maximumDistance = candidate.distance;
			hasKnownDistance = true;
		}
	}

	int32_t bestScore = 0;
	int32_t bestPlayerIndex = -1;
	uint32_t bestSourceOrdinal = AI_PLANNING_INVALID_ORDINAL;
	int32_t currentScore = 0;
	bool hasBest = false;
	bool hasCurrent = false;
	for (uint32_t i = 0; i < snapshot.candidateCount; ++i)
	{
		const AIEnemyCandidateFact &candidate = snapshot.candidates[i];
		AIEnemyCandidateScore &score = result->candidateScores[i];
		score.sourceOrdinal = candidate.sourceOrdinal;
		score.playerIndex = candidate.playerIndex;
		if (candidate.knownAssetValue > 0 && maximumKnownAssetValue > 0)
		{
			if (candidate.knownAssetValue >= maximumKnownAssetValue)
				score.knownAssetScore = 300;
			else
				score.knownAssetScore = ClampInt32((int32_t)(300.0 *
					(double)candidate.knownAssetValue / (double)maximumKnownAssetValue + 0.5), 0, 300);
		}
		score.retaliationScore = candidate.targetingThisAI != 0U ? 250 : 0;
		score.routeScore = TargetRouteScore(candidate.routeClass);
		score.allyTargetScore = -150 * ClampInt32(candidate.alliedAIsTargeting, 0, 3);
		if (candidate.hasKnownPosition != 0U && hasKnownDistance &&
			maximumDistance > minimumDistance && candidate.distance > minimumDistance)
		{
			if (candidate.distance >= maximumDistance)
				score.distanceScore = -300;
			else
				score.distanceScore = -ClampInt32((int32_t)(300.0 *
					(double)(candidate.distance - minimumDistance) /
					(double)(maximumDistance - minimumDistance) + 0.5), 0, 300);
		}
		score.crippledScore = candidate.hasKnownObject != 0U &&
			candidate.hasKnownUnit == 0U && candidate.hasKnownBuildFacility == 0U ? -600 : 0;
		score.totalScore = score.knownAssetScore + score.retaliationScore +
			score.routeScore + score.allyTargetScore + score.distanceScore + score.crippledScore;

		if (candidate.playerIndex == snapshot.currentEnemyPlayerIndex)
		{
			hasCurrent = true;
			currentScore = score.totalScore;
		}
		if (!hasBest || score.totalScore > bestScore ||
			(score.totalScore == bestScore && candidate.playerIndex < bestPlayerIndex))
		{
			hasBest = true;
			bestScore = score.totalScore;
			bestPlayerIndex = candidate.playerIndex;
			bestSourceOrdinal = candidate.sourceOrdinal;
		}
	}

	result->bestPlayerIndex = bestPlayerIndex;
	result->bestScore = bestScore;
	if (!hasCurrent)
	{
		result->selectedPlayerIndex = hasBest ? bestPlayerIndex : -1;
		result->selectedScore = hasBest ? bestScore : 0;
		result->orderKey.sourceOrdinal = hasBest ? bestSourceOrdinal : AI_PLANNING_INVALID_ORDINAL;
	}
	else if (hasBest && bestPlayerIndex != snapshot.currentEnemyPlayerIndex &&
		(int64_t)bestScore - (int64_t)currentScore >= (int64_t)snapshot.switchScoreThreshold)
	{
		result->selectedPlayerIndex = bestPlayerIndex;
		result->selectedScore = bestScore;
		result->orderKey.sourceOrdinal = bestSourceOrdinal;
	}
	else
	{
		result->selectedPlayerIndex = snapshot.currentEnemyPlayerIndex;
		result->selectedScore = currentScore;
		for (uint32_t i = 0; i < snapshot.candidateCount; ++i)
		{
			if (snapshot.candidates[i].playerIndex == snapshot.currentEnemyPlayerIndex)
			{
				result->orderKey.sourceOrdinal = snapshot.candidates[i].sourceOrdinal;
				break;
			}
		}
	}
	result->valid = 1U;
	return true;
}

bool PlanAIProduction(const AIProductionPlanningSnapshot &snapshot,
	AIProductionPlanningResult *result)
{
	if (result == 0 || snapshot.candidateCount > AI_PLANNING_MAX_PRODUCTION_CANDIDATES)
		return false;

	memset(result, 0, sizeof(*result));
	result->selectedSourceOrdinal = AI_PLANNING_INVALID_ORDINAL;
	result->orderKey = MakeOrderKey(snapshot.frame, snapshot.ownerPlayerIndex,
		AI_PLANNING_SUBPHASE_TEAM_PRODUCTION, AI_PLANNING_INVALID_ORDINAL);
	result->candidateScoreCount = snapshot.candidateCount;
	AIProductionCandidateFact candidates[AI_PLANNING_MAX_PRODUCTION_CANDIDATES];
	for (uint32_t i = 0; i < snapshot.candidateCount; ++i)
	{
		if (!BuildSourceCandidateFact(snapshot, i, &candidates[i]))
			return false;
	}

	bool hasEligibleCandidate = false;
	int32_t highestPriority = (-2147483647 - 1);
	for (uint32_t i = 0; i < snapshot.candidateCount; ++i)
	{
		result->candidateScores[i].sourceOrdinal = candidates[i].sourceOrdinal;
		result->candidateScores[i].candidateStableId = candidates[i].candidateStableId;
		if (candidates[i].eligible == 0U)
			continue;
		if (!hasEligibleCandidate || candidates[i].configuredPriority > highestPriority)
			highestPriority = candidates[i].configuredPriority;
		hasEligibleCandidate = true;
	}
	result->highestPriority = highestPriority;

	if (!hasEligibleCandidate)
	{
		result->valid = 1U;
		return true;
	}

	uint32_t tieCandidateIndices[AI_PLANNING_MAX_PRODUCTION_CANDIDATES];
	uint32_t tieCount = 0U;
	int64_t bestScore = 0;
	int32_t reserve = snapshot.initialReserve;
	for (uint32_t pass = 0; pass < 2U; ++pass)
	{
		memset(result->candidateScores, 0, sizeof(result->candidateScores));
		tieCount = 0U;
		for (uint32_t i = 0; i < snapshot.candidateCount; ++i)
		{
			const AIProductionCandidateFact &candidate = candidates[i];
			AIProductionCandidateScore &score = result->candidateScores[i];
			score.sourceOrdinal = candidate.sourceOrdinal;
			score.candidateStableId = candidate.candidateStableId;
			if (candidate.eligible == 0U ||
				!IsPriorityAdmitted(candidate.configuredPriority, highestPriority, snapshot.difficulty) ||
				!IsAffordable(snapshot.resources, candidate.minimumCost, reserve))
			{
				continue;
			}

			score = ScoreProductionCandidate(snapshot, candidate, reserve);
			if (tieCount == 0U || score.finalScore > bestScore)
			{
				bestScore = score.finalScore;
				tieCount = 1U;
				tieCandidateIndices[0] = i;
			}
			else if (score.finalScore == bestScore)
			{
				tieCandidateIndices[tieCount++] = i;
			}
		}

		if (tieCount != 0U || pass != 0U || snapshot.retryWithoutInitialReserve == 0U)
			break;
		reserve = snapshot.retryReserve;
		result->usedRetryReserve = 1U;
	}

	result->committedReserve = reserve;
	result->tieCount = tieCount;
	if (tieCount != 0U)
	{
		uint32_t tieSelection = 0U;
		if (tieCount > 1U && !GenerateAICounterRngIndex(snapshot.tieBreakKey,
			tieCount, &tieSelection, &result->randomLedger))
		{
			return false;
		}
		const uint32_t selectedCandidateIndex = tieCandidateIndices[tieSelection];
		const AIProductionCandidateFact &selected = candidates[selectedCandidateIndex];
		result->hasSelection = 1U;
		result->selectedSourceOrdinal = selected.sourceOrdinal;
		result->selectedStableId = selected.candidateStableId;
		result->selectedScore = bestScore;
		result->orderKey.sourceOrdinal = selected.sourceOrdinal;
	}
	result->valid = 1U;
	return true;
}

bool PlanAIProductionSelectionOwnerSerial(
	const AIProductionPlanningSnapshot &context,
	const AIProductionCandidateFact *candidates, uint32_t candidateCount,
	AIProductionSelectionResult *result)
{
	if (result == 0 || (candidateCount != 0U && candidates == 0))
		return false;

	memset(result, 0, sizeof(*result));
	result->selectedSourceOrdinal = AI_PLANNING_INVALID_ORDINAL;
	bool hasEligibleCandidate = false;
	int32_t highestPriority = (-2147483647 - 1);
	for (uint32_t i = 0; i < candidateCount; ++i)
	{
		if (candidates[i].eligible == 0U)
			continue;
		if (!hasEligibleCandidate || candidates[i].configuredPriority > highestPriority)
			highestPriority = candidates[i].configuredPriority;
		hasEligibleCandidate = true;
	}
	result->highestPriority = highestPriority;
	if (!hasEligibleCandidate)
	{
		result->valid = 1U;
		return true;
	}

	uint32_t *tieCandidateIndices = new (std::nothrow) uint32_t[candidateCount];
	if (tieCandidateIndices == 0)
		return false;
	uint32_t tieCount = 0U;
	int64_t bestScore = 0;
	int32_t reserve = context.initialReserve;
	for (uint32_t pass = 0; pass < 2U; ++pass)
	{
		tieCount = 0U;
		for (uint32_t i = 0; i < candidateCount; ++i)
		{
			const AIProductionCandidateFact &candidate = candidates[i];
			if (candidate.eligible == 0U ||
				!IsPriorityAdmitted(candidate.configuredPriority, highestPriority, context.difficulty) ||
				!IsAffordable(context.resources, candidate.minimumCost, reserve))
			{
				continue;
			}

			const AIProductionCandidateScore score =
				ScoreProductionCandidate(context, candidate, reserve);
			if (tieCount == 0U || score.finalScore > bestScore)
			{
				bestScore = score.finalScore;
				tieCount = 1U;
				tieCandidateIndices[0] = i;
			}
			else if (score.finalScore == bestScore)
			{
				tieCandidateIndices[tieCount++] = i;
			}
		}

		if (tieCount != 0U || pass != 0U || context.retryWithoutInitialReserve == 0U)
			break;
		reserve = context.retryReserve;
		result->usedRetryReserve = 1U;
	}

	result->committedReserve = reserve;
	result->tieCount = tieCount;
	if (tieCount != 0U)
	{
		uint32_t tieSelection = 0U;
		if (tieCount > 1U && !GenerateAICounterRngIndex(context.tieBreakKey,
			tieCount, &tieSelection, &result->randomLedger))
		{
			delete[] tieCandidateIndices;
			return false;
		}
		const AIProductionCandidateFact &selected =
			candidates[tieCandidateIndices[tieSelection]];
		result->hasSelection = 1U;
		result->selectedSourceOrdinal = selected.sourceOrdinal;
		result->selectedStableId = selected.candidateStableId;
		result->selectedScore = bestScore;
	}
	delete[] tieCandidateIndices;
	result->valid = 1U;
	return true;
}

bool RequiresAIProductionOwnerSerialFallback(uint32_t candidateCount)
{
	return candidateCount > AI_PLANNING_MAX_PRODUCTION_CANDIDATES;
}

bool PlanAIPlayer(const AIPlayerPlanningSnapshot &snapshot,
	AIPlayerPlanningResult *result)
{
	if (result == 0)
		return false;
	ClearAIPlayerPlanningResult(result);
	result->playerIndex = snapshot.playerIndex;
	if (snapshot.planEnemyTarget != 0U &&
		!PlanAIEnemyTarget(snapshot.enemyTarget, &result->enemyTarget))
		return false;
	if (snapshot.planProduction != 0U &&
		!PlanAIProduction(snapshot.production, &result->production))
		return false;
	result->valid = 1U;
	return true;
}

bool PlanAIPlayerBatchSerial(const AIPlayerPlanningSnapshot *snapshots,
	uint32_t snapshotCount, AIPlayerPlanningResult *results)
{
	if ((snapshotCount != 0U && (snapshots == 0 || results == 0)) ||
		snapshotCount > AI_PLANNING_MAX_PLAYERS ||
		!ArePlayerSnapshotsCommitOrdered(snapshots, snapshotCount))
		return false;
	for (uint32_t i = 0; i < snapshotCount; ++i)
	{
		if (!PlanAIPlayer(snapshots[i], &results[i]))
			return false;
	}
	return true;
}

bool EqualAIEnemyPlanningResult(const AIEnemyPlanningResult &left,
	const AIEnemyPlanningResult &right)
{
	if (left.candidateScoreCount > AI_PLANNING_MAX_PLAYERS ||
		right.candidateScoreCount > AI_PLANNING_MAX_PLAYERS)
		return false;
	if (left.valid != right.valid ||
		left.selectedPlayerIndex != right.selectedPlayerIndex ||
		left.selectedScore != right.selectedScore ||
		left.bestPlayerIndex != right.bestPlayerIndex ||
		left.bestScore != right.bestScore ||
		left.candidateScoreCount != right.candidateScoreCount ||
		!EqualOrderKey(left.orderKey, right.orderKey))
		return false;
	for (uint32_t i = 0; i < left.candidateScoreCount; ++i)
	{
		if (!EqualEnemyCandidateScore(left.candidateScores[i], right.candidateScores[i]))
			return false;
	}
	return true;
}

bool EqualAIProductionPlanningResult(const AIProductionPlanningResult &left,
	const AIProductionPlanningResult &right)
{
	if (left.candidateScoreCount > AI_PLANNING_MAX_PRODUCTION_CANDIDATES ||
		right.candidateScoreCount > AI_PLANNING_MAX_PRODUCTION_CANDIDATES)
		return false;
	if (left.valid != right.valid ||
		left.hasSelection != right.hasSelection ||
		left.selectedSourceOrdinal != right.selectedSourceOrdinal ||
		left.selectedStableId != right.selectedStableId ||
		left.selectedScore != right.selectedScore ||
		left.highestPriority != right.highestPriority ||
		left.committedReserve != right.committedReserve ||
		left.tieCount != right.tieCount ||
		left.usedRetryReserve != right.usedRetryReserve ||
		left.candidateScoreCount != right.candidateScoreCount ||
		!EqualOrderKey(left.orderKey, right.orderKey) ||
		!EqualAICounterRngLedgerRecord(left.randomLedger, right.randomLedger))
		return false;
	for (uint32_t i = 0; i < left.candidateScoreCount; ++i)
	{
		if (!EqualProductionCandidateScore(left.candidateScores[i], right.candidateScores[i]))
			return false;
	}
	return true;
}

bool EqualAIPlayerPlanningResult(const AIPlayerPlanningResult &left,
	const AIPlayerPlanningResult &right)
{
	return left.valid == right.valid &&
		left.playerIndex == right.playerIndex &&
		EqualAIEnemyPlanningResult(left.enemyTarget, right.enemyTarget) &&
		EqualAIProductionPlanningResult(left.production, right.production);
}

bool ValidateAIProductionPlanningResult(
	const AIProductionPlanningSnapshot &snapshot,
	const AIProductionPlanningResult &result)
{
	s_canonicalValidationInvocations.fetch_add(1U, std::memory_order_relaxed);
	if (snapshot.candidateCount > AI_PLANNING_MAX_PRODUCTION_CANDIDATES ||
		result.candidateScoreCount > AI_PLANNING_MAX_PRODUCTION_CANDIDATES)
		return false;
	AIProductionPlanningResult canonical;
	return PlanAIProduction(snapshot, &canonical) &&
		EqualAIProductionPlanningResult(canonical, result);
}

bool ValidateAIPlayerPlanningResult(const AIPlayerPlanningSnapshot &snapshot,
	const AIPlayerPlanningResult &result)
{
	if (result.valid != 1U || result.playerIndex != snapshot.playerIndex ||
		snapshot.planEnemyTarget > 1U || snapshot.planProduction > 1U ||
		snapshot.enemyTarget.candidateCount > AI_PLANNING_MAX_PLAYERS ||
		snapshot.production.candidateCount > AI_PLANNING_MAX_PRODUCTION_CANDIDATES ||
		result.enemyTarget.candidateScoreCount > AI_PLANNING_MAX_PLAYERS ||
		result.production.candidateScoreCount > AI_PLANNING_MAX_PRODUCTION_CANDIDATES)
		return false;

	if (snapshot.planEnemyTarget != 0U)
	{
		const AIEnemyPlanningResult &enemy = result.enemyTarget;
		if (enemy.valid != 1U || enemy.candidateScoreCount != snapshot.enemyTarget.candidateCount ||
			enemy.orderKey.frame != snapshot.frame ||
			enemy.orderKey.playerIndex != snapshot.playerIndex ||
			enemy.orderKey.subphase != AI_PLANNING_SUBPHASE_ENEMY_TARGET ||
			enemy.orderKey.emissionOrdinal != 0U)
			return false;
		AIEnemyPlanningResult canonicalEnemy;
		if (!PlanAIEnemyTarget(snapshot.enemyTarget, &canonicalEnemy) ||
			!EqualAIEnemyPlanningResult(canonicalEnemy, enemy))
			return false;
		for (uint32_t i = 0; i < snapshot.enemyTarget.candidateCount; ++i)
		{
			if (enemy.candidateScores[i].sourceOrdinal !=
				snapshot.enemyTarget.candidates[i].sourceOrdinal ||
				enemy.candidateScores[i].playerIndex !=
				snapshot.enemyTarget.candidates[i].playerIndex)
				return false;
		}
		if (enemy.bestPlayerIndex < 0)
		{
			if (enemy.bestPlayerIndex != -1 || snapshot.enemyTarget.candidateCount != 0U)
				return false;
		}
		else
		{
			bool bestFound = false;
			for (uint32_t i = 0; i < snapshot.enemyTarget.candidateCount; ++i)
			{
				if (snapshot.enemyTarget.candidates[i].playerIndex == enemy.bestPlayerIndex)
				{
					bestFound = true;
					break;
				}
			}
			if (!bestFound)
				return false;
		}
		if (enemy.selectedPlayerIndex < 0)
		{
			if (enemy.selectedPlayerIndex != -1 ||
				enemy.orderKey.sourceOrdinal != AI_PLANNING_INVALID_ORDINAL)
				return false;
		}
		else
		{
			bool found = false;
			for (uint32_t i = 0; i < snapshot.enemyTarget.candidateCount; ++i)
			{
				const AIEnemyCandidateFact &candidate = snapshot.enemyTarget.candidates[i];
				if (candidate.playerIndex == enemy.selectedPlayerIndex &&
					candidate.sourceOrdinal == enemy.orderKey.sourceOrdinal)
				{
					found = true;
					break;
				}
			}
			if (!found)
				return false;
		}
	}

	if (snapshot.planProduction != 0U)
	{
		const AIProductionPlanningResult &production = result.production;
		if (!ValidateAIProductionPlanningResult(snapshot.production, production) ||
			production.valid != 1U || production.hasSelection > 1U ||
			production.usedRetryReserve > 1U ||
			production.tieCount > snapshot.production.candidateCount ||
			production.candidateScoreCount != snapshot.production.candidateCount ||
			production.orderKey.frame != snapshot.frame ||
			production.orderKey.playerIndex != snapshot.playerIndex ||
			production.orderKey.subphase != AI_PLANNING_SUBPHASE_TEAM_PRODUCTION ||
			production.orderKey.emissionOrdinal != 0U)
			return false;
		for (uint32_t i = 0; i < snapshot.production.candidateCount; ++i)
		{
			if (production.candidateScores[i].sourceOrdinal !=
				snapshot.production.candidates[i].sourceOrdinal ||
				production.candidateScores[i].candidateStableId !=
				snapshot.production.candidates[i].candidateStableId)
				return false;
		}
		if (production.hasSelection == 0U)
		{
			if (production.selectedSourceOrdinal != AI_PLANNING_INVALID_ORDINAL ||
				production.orderKey.sourceOrdinal != AI_PLANNING_INVALID_ORDINAL ||
				production.selectedStableId != 0U || production.tieCount != 0U)
				return false;
		}
		else
		{
			if (production.tieCount == 0U)
				return false;
			bool found = false;
			for (uint32_t i = 0; i < snapshot.production.candidateCount; ++i)
			{
				const AIProductionCandidateFact &candidate = snapshot.production.candidates[i];
				if (candidate.eligible != 0U &&
					candidate.sourceOrdinal == production.selectedSourceOrdinal &&
					candidate.sourceOrdinal == production.orderKey.sourceOrdinal &&
					candidate.candidateStableId == production.selectedStableId)
				{
					found = true;
					break;
				}
			}
			if (!found)
				return false;
		}
		if (production.tieCount > 1U)
		{
			if (production.randomLedger.valid != 1U ||
				production.randomLedger.exclusiveUpperBound != production.tieCount ||
				production.randomLedger.selectedIndex >= production.tieCount ||
				!EqualAICounterRngKey(production.randomLedger.key,
					snapshot.production.tieBreakKey))
				return false;
		}
		else if (production.randomLedger.valid != 0U)
		{
			return false;
		}
	}
	return true;
}

bool ValidateAIPlanningBatchResults(const AIPlayerPlanningSnapshot *snapshots,
	const AIPlayerPlanningResult *results, uint32_t resultCount)
{
	if ((resultCount != 0U && (snapshots == 0 || results == 0)) ||
		resultCount > AI_PLANNING_MAX_PLAYERS ||
		!ArePlayerSnapshotsCommitOrdered(snapshots, resultCount))
		return false;
	return ArePlayerResultsComplete(snapshots, results, resultCount);
}

bool ExecuteAIPlanningBatch(AIPlanningExecutionMode mode,
	const AIPlayerPlanningSnapshot *snapshots, uint32_t snapshotCount,
	AIPlayerPlanningResult *committedResults,
	AIPlayerPlanningResult *serialScratch,
	AIPlayerPlanningResult *parallelScratch,
	AIPlanningParallelBatchRunner parallelRunner,
	void *parallelUserData,
	AIPlanningBatchStatus *status)
{
	if (status != 0)
	{
		memset(status, 0, sizeof(*status));
		status->requestedMode = (uint32_t)mode;
		status->committedMode = AI_PLANNING_EXECUTION_SERIAL;
		status->mismatchPlayerOrdinal = AI_PLANNING_INVALID_ORDINAL;
	}
	if ((snapshotCount != 0U && (snapshots == 0 || committedResults == 0)) ||
		snapshotCount > AI_PLANNING_MAX_PLAYERS ||
		!ArePlayerSnapshotsCommitOrdered(snapshots, snapshotCount))
		return false;

	if (mode == AI_PLANNING_EXECUTION_SERIAL)
	{
		return PlanAIPlayerBatchSerial(snapshots, snapshotCount, committedResults);
	}

	if (parallelRunner != 0 && parallelScratch != 0)
	{
		AIPlanningJobSystemEvidence *jobEvidence =
			parallelRunner == RunAIPlanningJobs ?
			static_cast<AIPlanningJobSystemEvidence *>(parallelUserData) : 0;
		const bool parallelRan = parallelRunner(snapshots, snapshotCount,
			parallelScratch, parallelUserData);
		const bool complete = parallelRan &&
			((jobEvidence != 0 && jobEvidence->resultsValidated) ||
			 ArePlayerResultsComplete(snapshots, parallelScratch, snapshotCount));
		if (complete)
		{
#if defined(_WIN64)
			// An authenticated owner-inline result follows the same single output
			// copy, but can never claim physical-worker or parallel authority.
			if (jobEvidence != 0 && jobEvidence->sourceBoundInline &&
				mode == AI_PLANNING_EXECUTION_PARALLEL)
			{
				if (!jobEvidence->sourceOwnerCommitAllowed)
					return false;
				CopyPlayerResults(parallelScratch, snapshotCount, committedResults);
				return true;
			}
#endif
			if (status != 0)
				status->parallelSucceeded =
#if defined(_WIN64)
					jobEvidence != 0 && jobEvidence->sourceBoundInline ? 0U :
#endif
					1U;
			if (mode == AI_PLANNING_EXECUTION_PARALLEL)
			{
				CopyPlayerResults(parallelScratch, snapshotCount, committedResults);
				if (status != 0)
					status->committedMode = AI_PLANNING_EXECUTION_PARALLEL;
				return true;
			}

			if (mode == AI_PLANNING_EXECUTION_SHADOW && serialScratch != 0 &&
				PlanAIPlayerBatchSerial(snapshots, snapshotCount, serialScratch))
			{
				uint32_t mismatch = AI_PLANNING_INVALID_ORDINAL;
				for (uint32_t i = 0; i < snapshotCount; ++i)
				{
					if (!EqualAIPlayerPlanningResult(serialScratch[i], parallelScratch[i]))
					{
						mismatch = i;
						break;
					}
				}
				if (mismatch == AI_PLANNING_INVALID_ORDINAL)
				{
					CopyPlayerResults(parallelScratch, snapshotCount, committedResults);
					if (status != 0)
					{
						status->committedMode =
#if defined(_WIN64)
							jobEvidence != 0 && jobEvidence->sourceBoundInline ? AI_PLANNING_EXECUTION_SERIAL :
#endif
							AI_PLANNING_EXECUTION_PARALLEL;
						status->shadowMatched = 1U;
					}
					return true;
				}

				CopyPlayerResults(serialScratch, snapshotCount, committedResults);
				if (status != 0)
				{
					status->usedSerialFallback = 1U;
					status->mismatchPlayerOrdinal = mismatch;
				}
				return true;
			}
		}
	}

	if (serialScratch == 0 ||
		!PlanAIPlayerBatchSerial(snapshots, snapshotCount, serialScratch))
		return false;
	CopyPlayerResults(serialScratch, snapshotCount, committedResults);
	if (status != 0)
		status->usedSerialFallback = 1U;
	return true;
}

bool ExecuteAIPlanningBatchOnJobSystem(AIPlanningExecutionMode mode,
	const AIPlayerPlanningSnapshot *snapshots, uint32_t snapshotCount,
	AIPlayerPlanningResult *committedResults,
	AIPlayerPlanningResult *serialScratch,
	AIPlayerPlanningResult *parallelScratch,
	AIPlanningBatchStatus *status
#if defined(_WIN64)
	, performance::KernelPerformanceBatch *performanceBatch,
	AIPlanningReferenceBatchTransport *referenceBatch
#endif
	)
{
	s_requestedBatches.fetch_add(1U, std::memory_order_relaxed);
	AIPlanningJobSystemEvidence evidence;
#if defined(_WIN64)
	evidence.performanceBatch = performanceBatch;
	evidence.referenceBatch = referenceBatch;
#endif
	const bool admitJobSystem = snapshots != 0 &&
		CountAIPlanningJobs(snapshots, snapshotCount) >= 2U;
	const bool executed = ExecuteAIPlanningBatch(mode, snapshots, snapshotCount,
		committedResults, serialScratch, parallelScratch,
		admitJobSystem ? static_cast<AIPlanningParallelBatchRunner>(RunAIPlanningJobs) : 0,
		&evidence, status);
	if (status != 0)
	{
		status->physicalWorkerMask = evidence.physicalWorkerMask;
		status->distinctPhysicalWorkers = evidence.distinctPhysicalWorkers;
		status->peakConcurrentPhysicalWorkers =
			evidence.peakConcurrentPhysicalWorkers;
		status->ownerHelpedJobs = evidence.ownerHelpedJobs;
		status->nativeAdmissionAccepted =
			evidence.nativeAdmissionAccepted ? 1U : 0U;
	}
	s_physicalWorkerExecutions.fetch_add(evidence.physicalWorkerExecutions,
		std::memory_order_relaxed);
	s_ownerHelpedExecutions.fetch_add(evidence.ownerHelpedJobs,
		std::memory_order_relaxed);
	s_observedPhysicalWorkerMask.fetch_or(evidence.physicalWorkerMask,
		std::memory_order_relaxed);
	UpdateMaximum(s_maximumDistinctPhysicalWorkers,
		evidence.distinctPhysicalWorkers);
	UpdateMaximum(s_maximumConcurrentPhysicalWorkers,
		evidence.peakConcurrentPhysicalWorkers);
	if (!executed)
	{
		s_validationFailures.fetch_add(1U, std::memory_order_relaxed);
		return false;
	}
	// ExecuteAIPlanningBatch validates an accepted worker result before it is
	// published.  Serial and fallback paths are produced by the same canonical
	// planner.  Revalidating this complete batch here duplicated the dominant
	// production recomputation (and, for enemy planning, the live commit oracle).
	if (status != 0)
	{
		if (status->usedSerialFallback != 0U)
		{
#if defined(_WIN64)
			ObserveAIPlanningTest(referenceBatch != 0 ? referenceBatch->testHooks : 0,
				AI_PLANNING_TEST_SERIAL_FALLBACK, 0, 0, snapshotCount);
#endif
			s_serialFallbacks.fetch_add(1U, std::memory_order_relaxed);
			JobSystem::instance().recordSerialFallback();
		}
		if (status->shadowMatched != 0U)
			s_shadowMatches.fetch_add(1U, std::memory_order_relaxed);
		if (mode == AI_PLANNING_EXECUTION_SHADOW &&
			status->mismatchPlayerOrdinal != AI_PLANNING_INVALID_ORDINAL)
			s_shadowMismatches.fetch_add(1U, std::memory_order_relaxed);
	}
	return true;
}

void ResetAIPlanningRuntimeMetrics()
{
	s_capturedSnapshots.store(0U, std::memory_order_relaxed);
	s_capturedCandidates.store(0U, std::memory_order_relaxed);
	s_requestedBatches.store(0U, std::memory_order_relaxed);
	s_submittedJobs.store(0U, std::memory_order_relaxed);
	s_completedJobs.store(0U, std::memory_order_relaxed);
	s_serialFallbacks.store(0U, std::memory_order_relaxed);
	s_shadowMatches.store(0U, std::memory_order_relaxed);
	s_shadowMismatches.store(0U, std::memory_order_relaxed);
	s_validationFailures.store(0U, std::memory_order_relaxed);
	s_canonicalValidationInvocations.store(0U, std::memory_order_relaxed);
	s_committedBatches.store(0U, std::memory_order_relaxed);
	s_parallelAuthoritativeCommits.store(0U, std::memory_order_relaxed);
	s_rejectedCommits.store(0U, std::memory_order_relaxed);
	s_physicalWorkerExecutions.store(0U, std::memory_order_relaxed);
	s_ownerHelpedExecutions.store(0U, std::memory_order_relaxed);
	s_observedPhysicalWorkerMask.store(0U, std::memory_order_relaxed);
	s_maximumDistinctPhysicalWorkers.store(0U, std::memory_order_relaxed);
	s_maximumConcurrentPhysicalWorkers.store(0U, std::memory_order_relaxed);
}

AIPlanningRuntimeMetrics GetAIPlanningRuntimeMetrics()
{
	AIPlanningRuntimeMetrics result;
	result.capturedSnapshots = s_capturedSnapshots.load(std::memory_order_relaxed);
	result.capturedCandidates = s_capturedCandidates.load(std::memory_order_relaxed);
	result.requestedBatches = s_requestedBatches.load(std::memory_order_relaxed);
	result.submittedJobs = s_submittedJobs.load(std::memory_order_relaxed);
	result.completedJobs = s_completedJobs.load(std::memory_order_relaxed);
	result.serialFallbacks = s_serialFallbacks.load(std::memory_order_relaxed);
	result.shadowMatches = s_shadowMatches.load(std::memory_order_relaxed);
	result.shadowMismatches = s_shadowMismatches.load(std::memory_order_relaxed);
	result.validationFailures = s_validationFailures.load(std::memory_order_relaxed);
	result.canonicalValidationInvocations =
		s_canonicalValidationInvocations.load(std::memory_order_relaxed);
	result.committedBatches = s_committedBatches.load(std::memory_order_relaxed);
	result.parallelAuthoritativeCommits =
		s_parallelAuthoritativeCommits.load(std::memory_order_relaxed);
	result.rejectedCommits = s_rejectedCommits.load(std::memory_order_relaxed);
	result.physicalWorkerExecutions =
		s_physicalWorkerExecutions.load(std::memory_order_relaxed);
	result.ownerHelpedExecutions =
		s_ownerHelpedExecutions.load(std::memory_order_relaxed);
	result.observedPhysicalWorkerMask =
		s_observedPhysicalWorkerMask.load(std::memory_order_relaxed);
	result.maximumDistinctPhysicalWorkers =
		s_maximumDistinctPhysicalWorkers.load(std::memory_order_relaxed);
	result.maximumConcurrentPhysicalWorkers =
		s_maximumConcurrentPhysicalWorkers.load(std::memory_order_relaxed);
	return result;
}

void RecordAIPlanningOwnerCapture(uint32_t candidateCount)
{
	s_capturedSnapshots.fetch_add(1U, std::memory_order_relaxed);
	s_capturedCandidates.fetch_add(candidateCount, std::memory_order_relaxed);
}

void RecordAIPlanningOwnerCommit(bool accepted,
	const AIPlanningBatchStatus *status)
{
	if (accepted)
	{
		s_committedBatches.fetch_add(1U, std::memory_order_relaxed);
		if (status != 0 &&
			status->requestedMode == AI_PLANNING_EXECUTION_PARALLEL &&
			status->committedMode == AI_PLANNING_EXECUTION_PARALLEL &&
			status->parallelSucceeded != 0U &&
			status->usedSerialFallback == 0U &&
			status->ownerHelpedJobs == 0U &&
			status->distinctPhysicalWorkers > 1U &&
			status->peakConcurrentPhysicalWorkers > 1U)
		{
			s_parallelAuthoritativeCommits.fetch_add(
				1U, std::memory_order_relaxed);
		}
	}
	else
		s_rejectedCommits.fetch_add(1U, std::memory_order_relaxed);
}
}
