/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2026 TheSuperHackers
**
** This program is free software: you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation, either version 3 of the License, or
** (at your option) any later version.
*/

#include "PreRTS.h"

#include "Common/GameEngine.h"
#include "Common/GlobalData.h"
#include "Common/BuildAssistant.h"
#include "Common/GameState.h"
#include "Common/MessageStream.h"
#include "Common/NameKeyGenerator.h"
#include "Common/Player.h"
#include "Common/PlayerList.h"
#include "Common/PlayerTemplate.h"
#include "Common/RandomValue.h"
#include "Common/Recorder.h"
#include "Common/SkirmishAITestRunner.h"
#include "Common/ThingFactory.h"
#include "Common/ThingTemplate.h"
#include "Common/Team.h"
#if RTS_ZEROHOUR
#include "GameLogic/AI.h"
#include "GameLogic/AISkirmishPlayer.h"
#endif
#include "GameLogic/AIPathfind.h"
#include "GameClient/MapUtil.h"
#include "GameLogic/GameLogic.h"
#include "GameLogic/Object.h"
#include "GameLogic/Damage.h"
#include "GameLogic/Weapon.h"
#include "GameLogic/Module/BodyModule.h"
#include "GameLogic/TerrainLogic.h"
#include "GameLogic/Module/AIUpdate.h"
#include "GameLogic/Module/DozerAIUpdate.h"
#include "GameLogic/Module/HackInternetAIUpdate.h"
#include "GameLogic/Module/ProductionUpdate.h"
#include "GameLogic/Module/RebuildHoleBehavior.h"
#include "GameLogic/PartitionManager.h"
#include "GameLogic/SidesList.h"
#include "GameLogic/VictoryConditions.h"
#include "Lib/JobSystem.h"
#include "Lib/PipelineExecutionPolicy.h"
#include "Lib/SimulationExecutionPolicy.h"
#include "Lib/SimulationPhaseGraphOwnerAdapter.h"
#if defined(_WIN64)
#include "Common/FileSystem.h"
#include "Common/Stage5MapResolution.h"
#include "Common/PerformanceReceiptRuntime.h"
#include "Common/crc.h"
#include "Lib/CollisionCandidateKernel.h"
#include "Lib/DeterministicAIPlanning.h"
#include "Lib/ImmutableSpatialQueryRuntime.h"
#include "Lib/ObjectStatusTimerKernel.h"
#include "Lib/PhysicsIntegrationKernel.h"
#include <memory>
#include <new>
#include <vector>
#endif

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#if defined(_WIN32)
#include <io.h>
#endif

namespace
{
const char *const s_alliedCaseNames[] = {
	"transfer_command", "coordination_live", "save_load", "support_lifecycle", "aid_lifecycle"
};
Int s_alliedRequestedCase = -1;
struct AlliedMovementProbe
{
	ObjectID objectID;
	UnsignedInt teamID;
	Int participant;
	Coord3D position;
};
struct AlliedFixtureState
{
	Bool active;
	Int fixtureCase;
	UnsignedInt startFrame;
	UnsignedInt checks;
	Bool saveLoaded;
	Bool sawFortifyDecline;
	Bool sawCoordination;
	UnsignedInt nextEvaluation;
	Int supportDonorSlot;
	Int supportRecipientIndex;
	Bool aidFaultApplied;
	Bool sawFirstStarvationEvaluation;
	Bool sawAid;
	Int aidDonorSlot;
	Int aidRecipientSlot;
	UnsignedInt aidCooldownUntil;
	UnsignedInt aidEvaluation;
	UnsignedInt firstStarvationFrame;
	Bool sawSupportReturning;
	UnsignedInt coordinatedReleaseFrame;
	UnsignedInt postLoadEvaluation;
	Int postLoadBlockedEvaluations;
	Bool withdrawalFaultIssued;
	Bool withdrawalCanceled;
	Bool withdrawalComplete;
	Bool withdrawalOrdinary[2];
	ObjectID withdrawalMembers[2];
	UnsignedInt withdrawalTeams[2];
	Bool assaultRetained;
	Bool assaultLaunched;
	UnsignedInt assaultLastIntactFrame;
	Bool positiveAbortActive;
	Bool positiveAbortCanceled;
	UnsignedInt positiveAbortFrame;
	Int positiveAbortCount;
	ObjectID positiveAbortMembers[2];
	UnsignedInt positiveAbortTeams[2];
	Bool positiveAbortOrdinary[2];
	Bool returnFireIssued;
	UnsignedInt returnFireFrame;
	UnsignedInt returnFireEvaluation;
	ObjectID returnFireMember;
	ObjectID returnFireSource;
	UnsignedInt returnFireTeam;
	Int returnFireParticipant;
	UnsignedInt assaultAdmissionFrames[2];
	Int assaultSlots[2];
	Int assaultLeader;
	Int assaultEnemy;
	ObjectID assaultTarget;
	UnsignedInt assaultRelease;
	UnsignedInt assaultExpiry;
	Coord3D assaultTargetPosition;
	Bool assaultMoved[2];
	ObjectID assaultMovedMembers[2];
	UnsignedInt assaultMovedTeams[2];
	std::vector<AlliedMovementProbe> assaultProbes;
	UnsignedInt supportTeamID;
	ObjectID supportMemberID;
	Coord3D supportInitialPosition;
	Coord3D supportAwayPosition;
	Bool supportFaultIssued;
	Bool supportAssignedScriptChecked;
	Bool supportReturningScriptChecked;
	Bool cancellationIssued;
	Bool cancellationSaved;
	Bool postLoadProtectionVerified;
	Int cancellationSlots[2];
	ObjectID cancellationMembers[2];
	UnsignedInt cancellationTeams[2];
	Coord3D cancellationPositions[2];
	Bool cancellationOrdinaryQualified[2];
	Bool cancellationPendingAtSave[2];
	Bool cancellationPendingAfterLoad[2];
	ObjectID cancellationTarget;
	UnsignedInt cancellationRelease;
	AlliedFixtureState() : active(FALSE), fixtureCase(-1), startFrame(0),
		checks(0), saveLoaded(FALSE), sawFortifyDecline(FALSE),
		sawCoordination(FALSE), nextEvaluation(0), supportDonorSlot(-1),
		supportRecipientIndex(-1), aidFaultApplied(FALSE),
		sawFirstStarvationEvaluation(FALSE), sawAid(FALSE), aidDonorSlot(-1),
		aidRecipientSlot(-1), aidCooldownUntil(0), aidEvaluation(0),
		firstStarvationFrame(0), sawSupportReturning(FALSE),
		coordinatedReleaseFrame(0), postLoadEvaluation(0), postLoadBlockedEvaluations(0),
		withdrawalFaultIssued(FALSE), withdrawalCanceled(FALSE), withdrawalComplete(FALSE),
		assaultRetained(FALSE), assaultLaunched(FALSE), assaultLastIntactFrame(0), positiveAbortActive(FALSE),
		positiveAbortCanceled(FALSE), positiveAbortFrame(0), positiveAbortCount(0), returnFireIssued(FALSE), returnFireFrame(0),
		returnFireEvaluation(0), returnFireMember(INVALID_ID), returnFireSource(INVALID_ID),
		returnFireTeam(0), returnFireParticipant(-1), assaultLeader(-1), assaultEnemy(-1),
		assaultTarget(INVALID_ID), assaultRelease(0), assaultExpiry(0),
		supportTeamID(0), supportMemberID(INVALID_ID), supportFaultIssued(FALSE),
		supportAssignedScriptChecked(FALSE), supportReturningScriptChecked(FALSE),
		cancellationIssued(FALSE), cancellationSaved(FALSE), postLoadProtectionVerified(FALSE),
		cancellationTarget(INVALID_ID), cancellationRelease(0)
	{
		withdrawalOrdinary[0] = withdrawalOrdinary[1] = FALSE;
		withdrawalMembers[0] = withdrawalMembers[1] = INVALID_ID;
		withdrawalTeams[0] = withdrawalTeams[1] = 0;
		assaultSlots[0] = assaultSlots[1] = -1;
		positiveAbortMembers[0] = positiveAbortMembers[1] = INVALID_ID;
		positiveAbortTeams[0] = positiveAbortTeams[1] = 0;
		positiveAbortOrdinary[0] = positiveAbortOrdinary[1] = FALSE;
		assaultAdmissionFrames[0] = assaultAdmissionFrames[1] = 0;
		assaultMoved[0] = assaultMoved[1] = FALSE;
		assaultMovedMembers[0] = assaultMovedMembers[1] = INVALID_ID;
		assaultMovedTeams[0] = assaultMovedTeams[1] = 0;
		for (Int participant = 0; participant < 2; ++participant)
		{
			cancellationSlots[participant] = -1;
			cancellationMembers[participant] = INVALID_ID;
			cancellationTeams[participant] = 0;
			cancellationOrdinaryQualified[participant] = FALSE;
			cancellationPendingAtSave[participant] = FALSE;
			cancellationPendingAfterLoad[participant] = FALSE;
		}
	}
};
AlliedFixtureState s_allied;
void UpdateSkirmishAIAlliedFixture();
#if defined(_WIN64)
rts::ai_fixture::MapRequest s_reviewedMapRequest;
rts::fixture::ResolvedMapIdentity s_reviewedMapIdentity;
char s_reviewedMapSha256[65] = {};
#endif
struct SkirmishAITestRunnerState
{
	Bool armed;
	Bool started;
	Bool ending;
	Bool finished;
	Bool failed;
	Int seed;
	Int winnerTeam;
	UnsignedInt endFrame;
	UnsignedInt startupStartMilliseconds;
	UnsignedInt lastObservedFrame;
	UnsignedInt stalledStartMilliseconds;
	UnsignedInt shutdownStartMilliseconds;
	char replayFileName[_MAX_PATH + 1];
	const char *failureReason;
	UnsignedInt expectedMapCRC;
	UnsignedInt expectedMapSize;
	Bool loadedStateValidated;
	char loadedMapName[_MAX_PATH + 1];
	UnsignedInt loadedMapCRC;
	UnsignedInt loadedMapSize;
	Int loadedSeed;
	SkirmishAITestScenario scenario;
	Int actualAiCount;
	Int actualTeamCounts[2];
	Int replayEpoch;
	char retainedReplayPath[SKIRMISH_AI_TEST_RECEIPT_PATH_LENGTH];
	char replaySha256[SKIRMISH_AI_TEST_RECEIPT_SHA256_LENGTH + 1];
	char runNonce[SKIRMISH_AI_TEST_RECEIPT_NONCE_LENGTH + 1];
	unsigned requestedWorkerCount;
	rts::JobWorkerPolicy workerPolicy;
	unsigned effectiveWorkerCount;
	rts::PipelineExecutionMode requestedPipelineMode;
	rts::SimulationExecutionMode requestedSimulationMode;
};

enum SkirmishAIRecoveryFixturePhase
{
	SKIRMISH_AI_RECOVERY_PHASE_WAIT_BASELINE,
	SKIRMISH_AI_RECOVERY_PHASE_APPLY_FAULT,
	SKIRMISH_AI_RECOVERY_PHASE_WAIT_RECOVERY,
	SKIRMISH_AI_RECOVERY_PHASE_WAIT_LOW_CASH,
	SKIRMISH_AI_RECOVERY_PHASE_VERIFY_NO_PATH,
	SKIRMISH_AI_RECOVERY_PHASE_WAIT_SECOND_RECOVERY,
	SKIRMISH_AI_RECOVERY_PHASE_WAIT_INFRASTRUCTURE,
	SKIRMISH_AI_RECOVERY_PHASE_WAIT_POWER_REBUILD,
	SKIRMISH_AI_RECOVERY_PHASE_VERIFY_GLA_HOLE,
	SKIRMISH_AI_RECOVERY_PHASE_SAVE_LOAD_PENDING,
	SKIRMISH_AI_RECOVERY_PHASE_SAVE_LOAD_REBOUND,
	SKIRMISH_AI_RECOVERY_PHASE_COMPLETE
};

enum
{
	SKIRMISH_AI_RECOVERY_BASELINE_TIMEOUT_FRAMES = 1200,
	SKIRMISH_AI_RECOVERY_PHASE_TIMEOUT_FRAMES = 18000,
	// Natural objective acquisition is a stock-AI precondition for this case;
	// reuse the full recovery bound without changing the post-fault assertion.
	SKIRMISH_AI_RECOVERY_NO_PATH_BASELINE_TIMEOUT_FRAMES =
		SKIRMISH_AI_RECOVERY_PHASE_TIMEOUT_FRAMES,
	SKIRMISH_AI_RECOVERY_LOW_CASH_WAIT_FRAMES = 120,
	SKIRMISH_AI_RECOVERY_DISABLED_FACTORY_VERIFY_FRAMES =
		5 * LOGICFRAMES_PER_SECOND,
	SKIRMISH_AI_RECOVERY_NO_PATH_VERIFY_FRAMES = 600,
	SKIRMISH_AI_RECOVERY_REPEAT_SETTLE_FRAMES = 30,
	SKIRMISH_AI_RECOVERY_MAX_CONSTRUCTION_ATTEMPTS = 24,
	// A one-worker rebuild must allow the full advanced-tech and sixth-market
	// construction cycle after the replacement command center completes.
	SKIRMISH_AI_RECOVERY_INFRASTRUCTURE_TIMEOUT_FRAMES = 24000,
	SKIRMISH_AI_RECOVERY_DIAGNOSTIC_MAX_IDS = 32
};

const Real SKIRMISH_AI_RECOVERY_SAVE_LOAD_PROGRESS_TOLERANCE = 0.01f;
const Real SKIRMISH_AI_RECOVERY_SAVE_LOAD_MIN_PROGRESS = 1.0f;

struct SkirmishAIRecoveryFixtureState
{
	Bool active;
	Int fixtureCase;
	Int faction;
	Int templateIndex;
	Int phase;
	UnsignedInt phaseStartFrame;
	UnsignedInt nextActionFrame;
	Bool baselineCaptured;
	Bool faultApplied;
	Bool moneyReleased;
	Bool obstructionPlaced;
	Bool sawAlternatePlacement;
	Bool sawBuilderQueue;
	Bool sawBuilderQueuePayment;
	Bool sawConstruction;
	Bool sawConstructionProgress;
	Bool sawConstructionOwnership;
	Bool sawCompletedRecovery;
	Bool infrastructureFaulted;
	Int depletedSupplySources;
	Bool sawLastStand;
	Bool secondFaultIssued;
	Bool secondBuilderLossIssued;
	Bool secondBuilderLossObserved;
	Bool secondBuilderLossSkipped;
	Bool secondBuilderReplacementObserved;
	Bool secondBuilderRoutePaid;
	Bool holeObserved;
	Bool holeLineageObserved;
	Bool saveLoadIssued;
	Bool saveLoadRebound;
	Bool saveLoadProgressPreserved;
	Bool saveLoadSawProgress;
	Bool factoryDisabled;
	Bool factoryBlockVerified;
	Bool factoryRestored;
	Bool factoryWorkerObserved;
	Bool factoryReserveHeldObserved;
	Bool factoryReserveReleasedObserved;
	Bool sawNoDuplicateCommandCenter;
	Bool lastStandBaselinePrepared;
	Bool obstructionOriginalLocationBlocked;
	Bool obstructionControlLocationLegal;
	ObjectID initialCenterID;
	ObjectID currentConstructionID;
	ObjectID lastConstructionBuilderID;
	ObjectID lastCompletedCenterID;
	ObjectID builderFactoryID;
	ObjectID obstructionID;
	ObjectID lastStandBaselineEvidenceUnitID;
	ObjectID holeID;
	ObjectID holeReconstructionID;
	ObjectID saveLoadConstructionID;
	ObjectID spawnedBuilderID;
	ObjectID secondBuilderLossID;
	ObjectID secondBuilderLossConstructionID;
	ObjectID secondBuilderReplacementID;
	ObjectID secondBuilderRouteFactoryID;
	ObjectID disabledFactoryBuilderID;
	ProductionID recoveryBuilderProductionID;
	ProductionID secondBuilderRouteProductionID;
	Int initialBuilderCount;
	Int initialFactoryBuilderCount;
	Int preFaultBuilderQueueCount;
	Int preFaultFactoryBuilderQueueCount;
	Bool preFaultBuilderWork;
	Bool spawnConsumed;
	ObjectID baselineBuilderIDs[SKIRMISH_AI_RECOVERY_DIAGNOSTIC_MAX_IDS];
	Int baselineBuilderIDCount;
	ObjectID baselineCombatIDs[SKIRMISH_AI_RECOVERY_DIAGNOSTIC_MAX_IDS];
	Int baselineCombatIDCount;
	ProductionID baselineQueueIDs[SKIRMISH_AI_RECOVERY_DIAGNOSTIC_MAX_IDS];
	Int baselineQueueIDCount;
	Int lastDiagnosticBuilderCount;
	Int lastDiagnosticQueueCount;
	Int initialCombatCount;
	Int initialFactoryCount;
	Int ccCost;
	Int builderCost;
	Int destructionCount;
	Int recoveryCompletionCount;
	Int infrastructureBaselineBuildings;
	Int infrastructureBaselinePower;
	Int infrastructureBaselineProduction;
	const ThingTemplate *infrastructureBaselineTemplates[
		SKIRMISH_AI_RECOVERY_DIAGNOSTIC_MAX_IDS];
	Int infrastructureBaselineTemplateCount;
	// Counts visible under-construction center scaffolds; failed pre-scaffold
	// placement calls are intentionally outside this fixture's observation API.
	Int constructionScaffoldCount;
	Int disabledFactoryBuilderCount;
	UnsignedInt lastConstructionAttemptFrame;
	Bool hasConstructionAttempt;
	Bool lastStandBaselineObjectiveObserved;
	Bool repeatedBaselineRouteReady;
	Real lastConstructionPercent;
	UnsignedInt initialCash;
	UnsignedInt lastObservedCash;
	UnsignedInt lowCash;
	UnsignedInt cashBeforeQueue;
	UnsignedInt cashAfterQueue;
	UnsignedInt cashBeforeConstruction;
	UnsignedInt cashAfterConstruction;
	UnsignedInt secondBuilderRouteCashBefore;
	UnsignedInt secondBuilderRouteCashAfter;
	UnsignedInt disabledFactoryBlockUntilFrame;
	UnsignedInt disabledFactoryCash;
	Real disabledFactoryProductionPercent;
	Real saveLoadConstructionPercent;
	AsciiString saveLoadFilename;
	const ThingTemplate *primaryTemplate;
	const ThingTemplate *builderTemplate;
	Coord3D originalCenterPosition;
	Coord3D originalBuildPosition;
	Coord3D obstructionOriginalPosition;
	Coord3D obstructionBlockedPosition;
	Coord3D lastStandBaselineAttackTarget;

	SkirmishAIRecoveryFixtureState()
	{
		reset();
	}

	void reset()
	{
		active = FALSE;
		fixtureCase = SKIRMISH_AI_RECOVERY_SURVIVING_BUILDER;
		faction = SKIRMISH_AI_RECOVERY_FACTION_AMERICA;
		templateIndex = -1;
		phase = SKIRMISH_AI_RECOVERY_PHASE_WAIT_BASELINE;
		phaseStartFrame = 0;
		nextActionFrame = 0;
		baselineCaptured = FALSE;
		faultApplied = FALSE;
		moneyReleased = FALSE;
		obstructionPlaced = FALSE;
		sawAlternatePlacement = FALSE;
		sawBuilderQueue = FALSE;
		sawBuilderQueuePayment = FALSE;
		sawConstruction = FALSE;
		sawConstructionProgress = FALSE;
		sawConstructionOwnership = FALSE;
		sawCompletedRecovery = FALSE;
		infrastructureFaulted = FALSE;
		depletedSupplySources = 0;
		sawLastStand = FALSE;
		secondFaultIssued = FALSE;
		secondBuilderLossIssued = FALSE;
		secondBuilderLossObserved = FALSE;
		secondBuilderLossSkipped = FALSE;
		secondBuilderReplacementObserved = FALSE;
		secondBuilderRoutePaid = FALSE;
		holeObserved = FALSE;
		holeLineageObserved = FALSE;
		saveLoadIssued = FALSE;
		saveLoadRebound = FALSE;
		saveLoadProgressPreserved = FALSE;
		saveLoadSawProgress = FALSE;
		factoryDisabled = FALSE;
		factoryBlockVerified = FALSE;
		factoryRestored = FALSE;
		factoryWorkerObserved = FALSE;
		factoryReserveHeldObserved = FALSE;
		factoryReserveReleasedObserved = FALSE;
		sawNoDuplicateCommandCenter = TRUE;
		lastStandBaselinePrepared = FALSE;
		obstructionOriginalLocationBlocked = FALSE;
		obstructionControlLocationLegal = FALSE;
		lastStandBaselineObjectiveObserved = FALSE;
		repeatedBaselineRouteReady = FALSE;
		initialCenterID = INVALID_ID;
		currentConstructionID = INVALID_ID;
		lastConstructionBuilderID = INVALID_ID;
		lastCompletedCenterID = INVALID_ID;
		builderFactoryID = INVALID_ID;
		obstructionID = INVALID_ID;
		lastStandBaselineEvidenceUnitID = INVALID_ID;
		holeID = INVALID_ID;
		holeReconstructionID = INVALID_ID;
		saveLoadConstructionID = INVALID_ID;
		spawnedBuilderID = INVALID_ID;
		secondBuilderLossID = INVALID_ID;
		secondBuilderLossConstructionID = INVALID_ID;
		secondBuilderReplacementID = INVALID_ID;
		secondBuilderRouteFactoryID = INVALID_ID;
		disabledFactoryBuilderID = INVALID_ID;
		recoveryBuilderProductionID = PRODUCTIONID_INVALID;
		secondBuilderRouteProductionID = PRODUCTIONID_INVALID;
		initialBuilderCount = 0;
		initialFactoryBuilderCount = 0;
		preFaultBuilderQueueCount = 0;
		preFaultFactoryBuilderQueueCount = 0;
		preFaultBuilderWork = FALSE;
		spawnConsumed = FALSE;
		baselineBuilderIDCount = 0;
		baselineCombatIDCount = 0;
		baselineQueueIDCount = 0;
		lastDiagnosticBuilderCount = -1;
		lastDiagnosticQueueCount = -1;
		initialCombatCount = 0;
		for (Int i = 0; i < SKIRMISH_AI_RECOVERY_DIAGNOSTIC_MAX_IDS; ++i)
		{
			baselineBuilderIDs[i] = INVALID_ID;
			baselineCombatIDs[i] = INVALID_ID;
			baselineQueueIDs[i] = PRODUCTIONID_INVALID;
		}
		initialFactoryCount = 0;
		ccCost = 0;
		builderCost = 0;
		destructionCount = 0;
		recoveryCompletionCount = 0;
		infrastructureBaselineBuildings = 0;
		infrastructureBaselinePower = 0;
		infrastructureBaselineProduction = 0;
		infrastructureBaselineTemplateCount = 0;
		for (Int templateIndex = 0;
			templateIndex < SKIRMISH_AI_RECOVERY_DIAGNOSTIC_MAX_IDS;
			++templateIndex)
			infrastructureBaselineTemplates[templateIndex] = nullptr;
		constructionScaffoldCount = 0;
		disabledFactoryBuilderCount = 0;
		lastConstructionAttemptFrame = 0;
		hasConstructionAttempt = FALSE;
		lastConstructionPercent = 0.0f;
		initialCash = 0;
		lastObservedCash = 0;
		lowCash = 0;
		cashBeforeQueue = 0;
		cashAfterQueue = 0;
		cashBeforeConstruction = 0;
		cashAfterConstruction = 0;
		secondBuilderRouteCashBefore = 0;
		secondBuilderRouteCashAfter = 0;
		disabledFactoryBlockUntilFrame = 0;
		disabledFactoryCash = 0;
		disabledFactoryProductionPercent = 0.0f;
		saveLoadConstructionPercent = 0.0f;
		saveLoadFilename.clear();
		primaryTemplate = nullptr;
		builderTemplate = nullptr;
		originalCenterPosition.zero();
		originalBuildPosition.zero();
		obstructionOriginalPosition.zero();
		obstructionBlockedPosition.zero();
		lastStandBaselineAttackTarget.zero();
	}
};

static const char *const g_skirmishAIRecoveryFixtureCaseNames[] =
{
	"surviving_builder",
	"factory_only",
	"no_path_laststand",
	"repeated_cc",
	"obstructed",
	"low_cash",
	"gla_hole",
	"save_load",
	"disabled_factory",
	"infrastructure_collapse"
};

static const char *const g_skirmishAIRecoveryFactionNames[] =
{
	"FactionAmerica",
	"FactionAmericaSuperWeaponGeneral",
	"FactionAmericaLaserGeneral",
	"FactionAmericaAirForceGeneral",
	"FactionChina",
	"FactionChinaTankGeneral",
	"FactionChinaInfantryGeneral",
	"FactionChinaNukeGeneral",
	"FactionGLA",
	"FactionGLAToxinGeneral",
	"FactionGLADemolitionGeneral",
	"FactionGLAStealthGeneral"
};

SkirmishAITestRunnerState s_runner = {
	FALSE, FALSE, FALSE, FALSE, FALSE, 0, -1, 0, 0, UINT_MAX, 0, 0, { 0 }, nullptr
};
rts::JobSystemMetrics s_jobMetricsAtStart;
rts::LiveSimulationPhaseRuntimeMetrics s_phaseMetricsAtStart;
rts::LiveSimulationPhaseRuntimeMetrics s_phaseMetricsLast;
DirectPathRuntimeMetrics s_directPathMetricsAtStart;
DirectPathRuntimeMetrics s_directPathMetricsFrozen;
Bool s_directPathMetricsHaveFrozenActivity = FALSE;
Bool s_directPathMetricsAwaitingInitialReset = FALSE;
OrdinaryPathRuntimeMetrics s_ordinaryPathMetricsAtStart;
OrdinaryPathRuntimeMetrics s_ordinaryPathMetricsFrozen;
Bool s_ordinaryPathMetricsAwaitingInitialReset = FALSE;
#if defined(_WIN64)
std::unique_ptr<PerformanceReceiptRuntime> s_performanceReceipt;
GameLogic *s_performanceReceiptOwner = 0;
bool s_performanceReceiptAttempted = false;
rts::AIPlanningRuntimeMetrics s_aiPlanningMetricsAtStart;
rts::CollisionCandidateRuntimeMetrics s_collisionMetricsAtStart;
rts::CollisionCandidateRuntimeMetrics s_collisionMetricsFrozen;
Bool s_collisionMetricsAwaitingInitialReset = FALSE;
rts::PhysicsIntegrationRuntimeMetrics s_physicsMetricsAtStart;
rts::PhysicsIntegrationRuntimeMetrics s_physicsMetricsFrozen;
Bool s_physicsMetricsAwaitingInitialReset = FALSE;
rts::ObjectStatusTimerRuntimeMetrics s_statusMetricsAtStart;
rts::ObjectStatusTimerRuntimeMetrics s_statusMetricsFrozen;
Bool s_statusMetricsAwaitingInitialReset = FALSE;
rts::ImmutableSpatialRuntimeMetrics s_spatialMetricsAtStart;
rts::ImmutableSpatialRuntimeMetrics s_spatialMetricsFrozen;
Bool s_spatialMetricsAwaitingInitialReset = FALSE;
#endif
char s_executableHashInput[65] = "unavailable";
char s_executableHashObserved[SKIRMISH_AI_TEST_RECEIPT_SHA256_LENGTH + 1] =
	"unavailable";
char s_simulationModeInput[16] = "unknown";
UnsignedInt s_finalDigest = 0;
Bool s_finalDigestAvailable = FALSE;
UnsignedInt s_runnerNonceCounter = 0;

void CaptureSkirmishAITestSliceMetrics();
Bool IsHexDigit(char value);

SkirmishAIRecoveryFixtureState s_recovery;

UnsignedInt ElapsedMilliseconds(UnsignedInt startMilliseconds, UnsignedInt nowMilliseconds)
{
	// Unsigned subtraction keeps short deadlines correct across the 32-bit
	// GetTickCount wrap and remains compatible with the VC6 reference lane.
	return nowMilliseconds - startMilliseconds;
}

class SkirmishAITestSha256
{
public:
	SkirmishAITestSha256() : m_dataLength(0), m_bitLengthLow(0),
		m_bitLengthHigh(0)
	{
		m_state[0] = 0x6a09e667U;
		m_state[1] = 0xbb67ae85U;
		m_state[2] = 0x3c6ef372U;
		m_state[3] = 0xa54ff53aU;
		m_state[4] = 0x510e527fU;
		m_state[5] = 0x9b05688cU;
		m_state[6] = 0x1f83d9abU;
		m_state[7] = 0x5be0cd19U;
	}

	void update(const unsigned char *bytes, size_t byteCount)
	{
		if (bytes == nullptr || byteCount == 0)
			return;
		const UnsignedInt lowBits = static_cast<UnsignedInt>(byteCount << 3);
		const UnsignedInt highBits = static_cast<UnsignedInt>(byteCount >> 29);
		const UnsignedInt previousLowBits = m_bitLengthLow;
		m_bitLengthLow += lowBits;
		if (m_bitLengthLow < previousLowBits)
			++m_bitLengthHigh;
		m_bitLengthHigh += highBits;

		size_t index = 0;
		while (index < byteCount)
		{
			m_data[m_dataLength++] = bytes[index++];
			if (m_dataLength == sizeof(m_data))
			{
				transform(m_data);
				m_dataLength = 0;
			}
		}
	}

	void finish(char digest[SKIRMISH_AI_TEST_RECEIPT_SHA256_LENGTH + 1])
	{
		if (digest == nullptr)
			return;
		UnsignedInt index = m_dataLength;
		m_data[index++] = 0x80;
		if (index > 56)
		{
			while (index < sizeof(m_data))
				m_data[index++] = 0;
			transform(m_data);
			index = 0;
		}
		while (index < 56)
			m_data[index++] = 0;
		m_data[56] = static_cast<unsigned char>(m_bitLengthHigh >> 24);
		m_data[57] = static_cast<unsigned char>(m_bitLengthHigh >> 16);
		m_data[58] = static_cast<unsigned char>(m_bitLengthHigh >> 8);
		m_data[59] = static_cast<unsigned char>(m_bitLengthHigh);
		m_data[60] = static_cast<unsigned char>(m_bitLengthLow >> 24);
		m_data[61] = static_cast<unsigned char>(m_bitLengthLow >> 16);
		m_data[62] = static_cast<unsigned char>(m_bitLengthLow >> 8);
		m_data[63] = static_cast<unsigned char>(m_bitLengthLow);
		transform(m_data);

		static const char hex[] = "0123456789ABCDEF";
		for (UnsignedInt stateIndex = 0; stateIndex < 8; ++stateIndex)
		{
			const UnsignedInt value = m_state[stateIndex];
			const UnsignedInt offset = stateIndex * 8;
			digest[offset] = hex[(value >> 28) & 0x0f];
			digest[offset + 1] = hex[(value >> 24) & 0x0f];
			digest[offset + 2] = hex[(value >> 20) & 0x0f];
			digest[offset + 3] = hex[(value >> 16) & 0x0f];
			digest[offset + 4] = hex[(value >> 12) & 0x0f];
			digest[offset + 5] = hex[(value >> 8) & 0x0f];
			digest[offset + 6] = hex[(value >> 4) & 0x0f];
			digest[offset + 7] = hex[value & 0x0f];
		}
		digest[SKIRMISH_AI_TEST_RECEIPT_SHA256_LENGTH] = '\0';
	}

private:
	static UnsignedInt rotateRight(UnsignedInt value, UnsignedInt count)
	{
		return (value >> count) | (value << (32 - count));
	}

	void transform(const unsigned char block[64])
	{
		static const UnsignedInt k[64] = {
			0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
			0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
			0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
			0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
			0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
			0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
			0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
			0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
			0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
			0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
			0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
			0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
			0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
			0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
			0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
			0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U
		};
		UnsignedInt words[64];
		for (UnsignedInt index = 0; index < 16; ++index)
		{
			const UnsignedInt offset = index * 4;
			words[index] = (static_cast<UnsignedInt>(block[offset]) << 24) |
				(static_cast<UnsignedInt>(block[offset + 1]) << 16) |
				(static_cast<UnsignedInt>(block[offset + 2]) << 8) |
				static_cast<UnsignedInt>(block[offset + 3]);
		}
		for (UnsignedInt wordIndex = 16; wordIndex < 64; ++wordIndex)
		{
			const UnsignedInt s0 = rotateRight(words[wordIndex - 15], 7) ^
				rotateRight(words[wordIndex - 15], 18) ^ (words[wordIndex - 15] >> 3);
			const UnsignedInt s1 = rotateRight(words[wordIndex - 2], 17) ^
				rotateRight(words[wordIndex - 2], 19) ^ (words[wordIndex - 2] >> 10);
			words[wordIndex] = words[wordIndex - 16] + s0 + words[wordIndex - 7] + s1;
		}

		UnsignedInt a = m_state[0];
		UnsignedInt b = m_state[1];
		UnsignedInt c = m_state[2];
		UnsignedInt d = m_state[3];
		UnsignedInt e = m_state[4];
		UnsignedInt f = m_state[5];
		UnsignedInt g = m_state[6];
		UnsignedInt h = m_state[7];
		for (UnsignedInt roundIndex = 0; roundIndex < 64; ++roundIndex)
		{
			const UnsignedInt sum1 = rotateRight(e, 6) ^ rotateRight(e, 11) ^
				rotateRight(e, 25);
			const UnsignedInt choice = (e & f) ^ ((~e) & g);
			const UnsignedInt temp1 = h + sum1 + choice + k[roundIndex] + words[roundIndex];
			const UnsignedInt sum0 = rotateRight(a, 2) ^ rotateRight(a, 13) ^
				rotateRight(a, 22);
			const UnsignedInt majority = (a & b) ^ (a & c) ^ (b & c);
			const UnsignedInt temp2 = sum0 + majority;
			h = g;
			g = f;
			f = e;
			e = d + temp1;
			d = c;
			c = b;
			b = a;
			a = temp1 + temp2;
		}
		m_state[0] += a;
		m_state[1] += b;
		m_state[2] += c;
		m_state[3] += d;
		m_state[4] += e;
		m_state[5] += f;
		m_state[6] += g;
		m_state[7] += h;
	}

	unsigned char m_data[64];
	UnsignedInt m_dataLength;
	UnsignedInt m_bitLengthLow;
	UnsignedInt m_bitLengthHigh;
	UnsignedInt m_state[8];
};

Bool HasBoundedString(const char *value, size_t capacity)
{
	if (value == nullptr || capacity == 0)
		return FALSE;
	for (size_t index = 0; index < capacity; ++index)
	{
		if (value[index] == '\0')
			return TRUE;
	}
	return FALSE;
}

Bool IsHexString(const char *value, size_t length)
{
	if (value == nullptr || !HasBoundedString(value, length + 1) ||
		strlen(value) != length)
		return FALSE;
	for (size_t index = 0; index < length; ++index)
	{
		if (!IsHexDigit(value[index]))
			return FALSE;
	}
	return TRUE;
}

Bool HashSkirmishAITestFile(const char *path,
	char digest[SKIRMISH_AI_TEST_RECEIPT_SHA256_LENGTH + 1])
{
	if (path == nullptr || digest == nullptr)
		return FALSE;
	FILE *file = fopen(path, "rb");
	if (file == nullptr)
		return FALSE;
	SkirmishAITestSha256 sha256;
	unsigned char bytes[32768];
	Bool success = TRUE;
	while (!feof(file))
	{
		const size_t readCount = fread(bytes, 1, sizeof(bytes), file);
		if (readCount != 0)
			sha256.update(bytes, readCount);
		if (ferror(file))
		{
			success = FALSE;
			break;
		}
	}
	if (fclose(file) != 0)
		success = FALSE;
	if (success)
		sha256.finish(digest);
	return success;
}

Bool HashSkirmishAITestHandle(void *opaqueHandle,
	char digest[SKIRMISH_AI_TEST_RECEIPT_SHA256_LENGTH + 1])
{
#if defined(_WIN32)
	if (opaqueHandle == nullptr || digest == nullptr)
		return FALSE;
	digest[0] = '\0';
	HANDLE handle = static_cast<HANDLE>(opaqueHandle);
	if (handle == INVALID_HANDLE_VALUE || handle == nullptr)
		return FALSE;
	LARGE_INTEGER origin = {};
	LARGE_INTEGER position = {};
	LARGE_INTEGER extent = {};
	if (!SetFilePointerEx(handle, origin, &position, FILE_CURRENT) ||
		!GetFileSizeEx(handle, &extent) || extent.QuadPart < 0 ||
		extent.QuadPart > static_cast<LONGLONG>(SKIRMISH_AI_TEST_MAX_REPLAY_BYTES) ||
		!SetFilePointerEx(handle, origin, nullptr, FILE_BEGIN))
		return FALSE;
	SkirmishAITestSha256 sha256;
	unsigned char bytes[32768];
	Bool success = TRUE;
	unsigned long long remaining = static_cast<unsigned long long>(extent.QuadPart);
	while (remaining != 0)
	{
		const DWORD requested = static_cast<DWORD>(remaining < sizeof(bytes) ?
			remaining : sizeof(bytes));
		DWORD readCount = 0;
		if (!ReadFile(handle, bytes, requested, &readCount, nullptr) ||
			readCount != requested)
		{
			success = FALSE;
			break;
		}
		sha256.update(bytes, readCount);
		remaining -= readCount;
	}
	// Reading exactly the admitted extent is not enough if the underlying
	// object changed length. Confirm EOF and the same extent before accepting
	// the digest; the replay owner performs this again at checked closure.
	unsigned char extra = 0;
	DWORD extraCount = 0;
	LARGE_INTEGER closedExtent = {};
	if (success && (!ReadFile(handle, &extra, 1, &extraCount, nullptr) ||
		extraCount != 0 || !GetFileSizeEx(handle, &closedExtent) ||
		closedExtent.QuadPart != extent.QuadPart))
		success = FALSE;
	// The caller retains ownership of the identity-bound handle. Restoring its
	// position also prevents this validation pass from changing later users.
	if (!SetFilePointerEx(handle, position, nullptr, FILE_BEGIN))
		success = FALSE;
	if (success)
		sha256.finish(digest);
	return success;
#else
	(void)opaqueHandle;
	(void)digest;
	return FALSE;
#endif
}

#if defined(_WIN64)
struct SkirmishAITestReplayIdentity
{
	DWORD volume, high, low;
	unsigned long long size;
};

Bool QuerySkirmishAITestReplayIdentity(HANDLE handle, const char *expectedPath,
	SkirmishAITestReplayIdentity &identity)
{
	if (handle == INVALID_HANDLE_VALUE || handle == nullptr || expectedPath == nullptr)
		return FALSE;
	BY_HANDLE_FILE_INFORMATION information = {};
	char nativePath[SKIRMISH_AI_TEST_RECEIPT_PATH_LENGTH + 4] = {};
	char fullPath[SKIRMISH_AI_TEST_RECEIPT_PATH_LENGTH] = {};
	const DWORD nativeLength = GetFinalPathNameByHandleA(handle, nativePath,
		static_cast<DWORD>(sizeof(nativePath)), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
	const DWORD fullLength = GetFullPathNameA(expectedPath,
		static_cast<DWORD>(sizeof(fullPath)), fullPath, nullptr);
	if (!GetFileInformationByHandle(handle, &information) ||
		nativeLength < 7 || nativeLength >= sizeof(nativePath) ||
		strncmp(nativePath, "\\\\?\\", 4) != 0 ||
		fullLength == 0 || fullLength >= sizeof(fullPath) ||
		_stricmp(nativePath + 4, fullPath) != 0 ||
		(information.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0 ||
		information.nNumberOfLinks != 1)
		return FALSE;
	identity.volume = information.dwVolumeSerialNumber;
	identity.high = information.nFileIndexHigh;
	identity.low = information.nFileIndexLow;
	identity.size = (static_cast<unsigned long long>(information.nFileSizeHigh) << 32) |
		information.nFileSizeLow;
	return TRUE;
}

Bool SameSkirmishAITestReplayIdentity(const SkirmishAITestReplayIdentity &left,
	const SkirmishAITestReplayIdentity &right)
{
	return left.volume == right.volume && left.high == right.high &&
		left.low == right.low && left.size == right.size;
}

void DeleteSkirmishAITestReplayHandle(HANDLE handle)
{
	if (handle == INVALID_HANDLE_VALUE || handle == nullptr) return;
	FILE_DISPOSITION_INFO disposition = { TRUE };
	SetFileInformationByHandle(handle, FileDispositionInfo, &disposition, sizeof(disposition));
}

#endif

Bool CommitSkirmishAITestReplay(const char *temporaryPath,
	const char *destinationPath,
	SkirmishAITestDetail::ReplayCommitCallback commitCallback,
	void *commitContext)
{
#if defined(_WIN32)
	const UnsignedInt maxAttempts = 8;
	for (UnsignedInt attempt = 0; attempt < maxAttempts; ++attempt)
	{
		Bool committed = FALSE;
		if (commitCallback != nullptr)
			committed = commitCallback(temporaryPath, destinationPath,
				commitContext);
		else
			committed = MoveFileExA(temporaryPath, destinationPath,
				MOVEFILE_WRITE_THROUGH) ? TRUE : FALSE;
		if (committed)
			return TRUE;
		const DWORD error = GetLastError();
		if (error != ERROR_SHARING_VIOLATION || attempt + 1 >= maxAttempts)
			return FALSE;
		Sleep(10);
	}
	return FALSE;
#else
	if (commitCallback != nullptr)
		return commitCallback(temporaryPath, destinationPath, commitContext);
	return rename(temporaryPath, destinationPath) == 0;
#endif
}

Bool RetainSkirmishAITestReplayAtomicallyInternal(const char *sourcePath,
	const char *destinationPath,
	char sha256[SKIRMISH_AI_TEST_RECEIPT_SHA256_LENGTH + 1],
	SkirmishAITestDetail::ReplayCommitCallback commitCallback,
	void *commitContext,
	SkirmishAITestDetail::ReplayFinalHandleCloseCallback finalCloseCallback,
	void *finalCloseContext)
{
	if (sourcePath == nullptr || destinationPath == nullptr || sha256 == nullptr ||
		!HasBoundedString(sourcePath, SKIRMISH_AI_TEST_RECEIPT_PATH_LENGTH) ||
		!HasBoundedString(destinationPath, SKIRMISH_AI_TEST_RECEIPT_PATH_LENGTH) ||
		strlen(sourcePath) == 0 || strlen(destinationPath) == 0)
	{
		return FALSE;
	}
#if defined(_WIN32)
	if (_stricmp(sourcePath, destinationPath) == 0)
#else
	if (strcmp(sourcePath, destinationPath) == 0)
#endif
		return FALSE;
	char temporaryPath[SKIRMISH_AI_TEST_RECEIPT_PATH_LENGTH];
	if (strlen(destinationPath) + 5 >= sizeof(temporaryPath))
		return FALSE;
	_snprintf(temporaryPath, sizeof(temporaryPath), "%s.tmp", destinationPath);
	temporaryPath[sizeof(temporaryPath) - 1] = '\0';
#if defined(_WIN64)
	HANDLE source = CreateFileA(sourcePath, GENERIC_READ, FILE_SHARE_READ, nullptr,
		OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT |
		FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
	if (source == INVALID_HANDLE_VALUE) return FALSE;
	HANDLE temporary = CreateFileA(temporaryPath,
		GENERIC_READ | GENERIC_WRITE | DELETE, FILE_SHARE_READ | FILE_SHARE_DELETE,
		nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT |
		FILE_FLAG_WRITE_THROUGH, nullptr);
	if (temporary == INVALID_HANDLE_VALUE)
	{
		CloseHandle(source);
		return FALSE;
	}
	SkirmishAITestReplayIdentity sourceIdentity = {}, temporaryIdentity = {};
	Bool success = QuerySkirmishAITestReplayIdentity(source, sourcePath, sourceIdentity) &&
		sourceIdentity.size <= SKIRMISH_AI_TEST_MAX_REPLAY_BYTES &&
		QuerySkirmishAITestReplayIdentity(temporary, temporaryPath, temporaryIdentity) &&
		temporaryIdentity.size == 0;
	SkirmishAITestSha256 hasher;
	unsigned char bytes[32768];
	unsigned long long remaining = sourceIdentity.size;
	while (success && remaining != 0)
	{
		const DWORD requested = static_cast<DWORD>(remaining < sizeof(bytes) ?
			remaining : sizeof(bytes));
		DWORD readCount = 0, written = 0;
		if (!ReadFile(source, bytes, requested, &readCount, nullptr) ||
			readCount != requested ||
			!WriteFile(temporary, bytes, readCount, &written, nullptr) ||
			written != readCount)
		{
			success = FALSE;
			break;
		}
		hasher.update(bytes, readCount);
		remaining -= readCount;
	}
	unsigned char extra = 0;
	DWORD extraCount = 0;
	SkirmishAITestReplayIdentity closedSourceIdentity = {}, writtenIdentity = {};
	if (success && (!ReadFile(source, &extra, 1, &extraCount, nullptr) ||
		extraCount != 0 ||
		!QuerySkirmishAITestReplayIdentity(source, sourcePath, closedSourceIdentity) ||
		!SameSkirmishAITestReplayIdentity(sourceIdentity, closedSourceIdentity) ||
		!FlushFileBuffers(temporary) ||
		!QuerySkirmishAITestReplayIdentity(temporary, temporaryPath, writtenIdentity) ||
		writtenIdentity.volume != temporaryIdentity.volume ||
		writtenIdentity.high != temporaryIdentity.high ||
		writtenIdentity.low != temporaryIdentity.low ||
		writtenIdentity.size != sourceIdentity.size)) success = FALSE;
	char sourceDigest[SKIRMISH_AI_TEST_RECEIPT_SHA256_LENGTH + 1] = {};
	char temporaryDigest[SKIRMISH_AI_TEST_RECEIPT_SHA256_LENGTH + 1] = {};
	if (success)
	{
		hasher.finish(sourceDigest);
		success = HashSkirmishAITestHandle(temporary, temporaryDigest) &&
			strcmp(sourceDigest, temporaryDigest) == 0;
	}
	if (!CloseHandle(source)) success = FALSE;
	Bool committed = FALSE;
	if (success)
		committed = CommitSkirmishAITestReplay(temporaryPath, destinationPath,
			commitCallback, commitContext);
	SkirmishAITestReplayIdentity committedIdentity = {};
	if (committed && (!QuerySkirmishAITestReplayIdentity(temporary,
		destinationPath, committedIdentity) ||
		!SameSkirmishAITestReplayIdentity(writtenIdentity, committedIdentity))) success = FALSE;
	HANDLE sharedDestination = INVALID_HANDLE_VALUE;
	if (success && committed)
	{
		// The writer remains live while this exact-identity bridge is admitted.
		// Share its existing write access, but retain DELETE authority so failure
		// cleanup never depends on a later pathname reopen.
		sharedDestination = CreateFileA(destinationPath, GENERIC_READ | DELETE,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT |
			FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
		SkirmishAITestReplayIdentity sharedIdentity = {};
		if (sharedDestination == INVALID_HANDLE_VALUE ||
			!QuerySkirmishAITestReplayIdentity(sharedDestination, destinationPath, sharedIdentity) ||
			!SameSkirmishAITestReplayIdentity(committedIdentity, sharedIdentity)) success = FALSE;
	}
	if (!success || !committed) DeleteSkirmishAITestReplayHandle(temporary);
	if (!CloseHandle(temporary)) success = FALSE;
	HANDLE lockedDestination = INVALID_HANDLE_VALUE;
	HANDLE cleanupDestination = INVALID_HANDLE_VALUE;
	Bool lockedMatchesCommit = FALSE;
	if (success && committed)
	{
		// This DELETE-capable exact-object handle denies later writers while its
		// cleanup twin survives the checked final close. Rename sharing is required
		// for exact-handle cleanup; the repeated canonical-path checks reject any
		// displacement or pathname substitution before acceptance.
		lockedDestination = ReOpenFile(sharedDestination, GENERIC_READ | DELETE,
			FILE_SHARE_READ | FILE_SHARE_DELETE,
			FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN);
		SkirmishAITestReplayIdentity lockedIdentity = {}, closedLockedIdentity = {};
		char lockedDigest[SKIRMISH_AI_TEST_RECEIPT_SHA256_LENGTH + 1] = {};
		lockedMatchesCommit = lockedDestination != INVALID_HANDLE_VALUE &&
			QuerySkirmishAITestReplayIdentity(lockedDestination, destinationPath, lockedIdentity) &&
			SameSkirmishAITestReplayIdentity(committedIdentity, lockedIdentity);
		if (!lockedMatchesCommit ||
			!DuplicateHandle(GetCurrentProcess(), lockedDestination, GetCurrentProcess(),
				&cleanupDestination, 0, FALSE, DUPLICATE_SAME_ACCESS) ||
			!SetHandleInformation(cleanupDestination, HANDLE_FLAG_INHERIT, 0) ||
			!HashSkirmishAITestHandle(lockedDestination, lockedDigest) ||
			strcmp(sourceDigest, lockedDigest) != 0 ||
			!QuerySkirmishAITestReplayIdentity(lockedDestination, destinationPath,
				closedLockedIdentity) ||
			!SameSkirmishAITestReplayIdentity(lockedIdentity, closedLockedIdentity)) success = FALSE;
	}
	if ((!success || !committed) && cleanupDestination == INVALID_HANDLE_VALUE)
	{
		if (lockedMatchesCommit) DeleteSkirmishAITestReplayHandle(lockedDestination);
		else DeleteSkirmishAITestReplayHandle(sharedDestination);
	}
	if (sharedDestination != INVALID_HANDLE_VALUE && !CloseHandle(sharedDestination)) success = FALSE;
	if (lockedDestination != INVALID_HANDLE_VALUE)
	{
		const Bool closed = finalCloseCallback != nullptr ?
			finalCloseCallback(lockedDestination, finalCloseContext) :
			(CloseHandle(lockedDestination) ? TRUE : FALSE);
		if (!closed) success = FALSE;
	}
	if (!success || !committed)
		DeleteSkirmishAITestReplayHandle(cleanupDestination);
	if (cleanupDestination != INVALID_HANDLE_VALUE && !CloseHandle(cleanupDestination))
		success = FALSE;
	if (!success || !committed) return FALSE;
	strlcpy(sha256, sourceDigest, SKIRMISH_AI_TEST_RECEIPT_SHA256_LENGTH + 1);
	return TRUE;
#else
	(void)finalCloseCallback;
	(void)finalCloseContext;
	FILE *existingTemporary = fopen(temporaryPath, "rb");
	if (existingTemporary != nullptr)
	{
		fclose(existingTemporary);
		return FALSE;
	}
	FILE *source = fopen(sourcePath, "rb");
	if (source == nullptr)
		return FALSE;
	FILE *temporary = fopen(temporaryPath, "wb");
	if (temporary == nullptr)
	{
		fclose(source);
		return FALSE;
	}
	SkirmishAITestSha256 hasher;
	unsigned char bytes[32768];
	Bool success = TRUE;
	while (!feof(source))
	{
		const size_t readCount = fread(bytes, 1, sizeof(bytes), source);
		if (readCount != 0)
		{
			hasher.update(bytes, readCount);
			if (fwrite(bytes, 1, readCount, temporary) != readCount)
			{
				success = FALSE;
				break;
			}
		}
		if (ferror(source))
		{
			success = FALSE;
			break;
		}
	}
	if (fflush(temporary) != 0)
		success = FALSE;
#if defined(_WIN32)
	if (success && _commit(_fileno(temporary)) != 0)
		success = FALSE;
#endif
	if (fclose(source) != 0)
		success = FALSE;
	if (fclose(temporary) != 0)
		success = FALSE;
	Bool committed = FALSE;
	if (success)
		committed = CommitSkirmishAITestReplay(temporaryPath, destinationPath,
			commitCallback, commitContext);
	if (!success || !committed)
	{
		remove(temporaryPath);
		return FALSE;
	}
	hasher.finish(sha256);
	return TRUE;
#endif
}

Bool CaptureSkirmishAITestExecutableHash(
	char digest[SKIRMISH_AI_TEST_RECEIPT_SHA256_LENGTH + 1])
{
#if defined(_WIN32)
	char modulePath[SKIRMISH_AI_TEST_RECEIPT_PATH_LENGTH];
	const DWORD pathLength = GetModuleFileNameA(nullptr, modulePath,
		static_cast<DWORD>(sizeof(modulePath)));
	if (pathLength == 0 || pathLength >= sizeof(modulePath))
		return FALSE;
	modulePath[pathLength] = '\0';
	return HashSkirmishAITestFile(modulePath, digest);
#else
	(void)digest;
	return FALSE;
#endif
}

Bool IsHexDigit(char value)
{
	return (value >= '0' && value <= '9') ||
		(value >= 'a' && value <= 'f') ||
		(value >= 'A' && value <= 'F');
}

const char *SkirmishAITestPipelineModeName(rts::PipelineExecutionMode mode)
{
	return mode == rts::PIPELINE_EXECUTION_SERIAL ? "serial" : "parallel";
}

const char *SkirmishAITestSimulationModeName(rts::SimulationExecutionMode mode)
{
	if (mode == rts::SIMULATION_EXECUTION_PARALLEL) return "parallel";
	if (mode == rts::SIMULATION_EXECUTION_SHADOW) return "shadow";
	return "serial";
}

void CaptureSkirmishAITestRuntimeState()
{
	const unsigned workerCount = rts::JobSystem::instance().workerCount();
	if (workerCount > s_runner.effectiveWorkerCount)
		s_runner.effectiveWorkerCount = workerCount;
	if (TheGameLogic != nullptr && !s_runner.ending)
		s_phaseMetricsLast = TheGameLogic->getStage5PhaseRuntimeMetrics();
	CaptureSkirmishAITestSliceMetrics();
}

#if defined(_WIN64)
struct ClosePerformanceMapFile
{
	void operator()(File *file) const { if (file != 0) file->close(); }
};

Bool VerifyReviewedSkirmishMapBytes()
{
	if (!s_reviewedMapRequest.requested || !TheFileSystem) return FALSE;
	std::unique_ptr<File, ClosePerformanceMapFile> file(TheFileSystem->openFile(
		s_reviewedMapIdentity.runtimePath, File::READ | File::BINARY | File::STREAMING));
	const Int length = file ? file->size() : 0;
	if (length <= 0 || static_cast<unsigned>(length) != s_reviewedMapRequest.byteCount ||
		length > 64 * 1024 * 1024) return FALSE;
	try
	{
		std::vector<unsigned char> bytes(static_cast<size_t>(length));
		Int offset = 0;
		while (offset < length)
		{
			const Int count = file->read(&bytes[static_cast<size_t>(offset)], length - offset);
			if (count <= 0 || count > length - offset) return FALSE;
			offset += count;
		}
		unsigned char extra = 0;
		if (file->read(&extra, 1) != 0) return FALSE;
		CRC crc;
		crc.computeCRC(&bytes[0], length);
		char sha256[65];
		if (crc.get() != s_reviewedMapRequest.crc ||
			!HashSkirmishAITestBytes(&bytes[0], bytes.size(), sha256) ||
			strcmp(sha256, s_reviewedMapRequest.sha256) != 0) return FALSE;
		memcpy(s_reviewedMapSha256, sha256, sizeof(s_reviewedMapSha256));
		return TRUE;
	}
	catch (const std::bad_alloc &) { return FALSE; }
}

void BindSkirmishAITestPerformanceMap()
{
	if (!s_performanceReceipt || !s_performanceReceipt->active()) return;
	if (TheFileSystem == 0)
	{
		s_performanceReceipt->invalidate("loaded map filesystem was unavailable");
		return;
	}
	std::unique_ptr<File, ClosePerformanceMapFile> file(TheFileSystem->openFile(
		s_runner.loadedMapName, File::READ | File::BINARY | File::STREAMING));
	const Int length = file ? file->size() : 0;
	if (length <= 0 || length > 64 * 1024 * 1024 ||
		static_cast<unsigned>(length) != s_runner.loadedMapSize)
	{
		s_performanceReceipt->invalidate("loaded map content size could not be verified");
		return;
	}
	try
	{
		std::vector<unsigned char> bytes(static_cast<size_t>(length));
		Int offset = 0;
		while (offset < length)
		{
			const Int count = file->read(&bytes[static_cast<size_t>(offset)], length - offset);
			if (count <= 0 || count > length - offset) break;
			offset += count;
		}
		unsigned char extra = 0;
		if (offset != length || file->read(&extra, 1) != 0)
		{
			s_performanceReceipt->invalidate("loaded map content was not read exactly");
			return;
		}
		CRC crc;
		crc.computeCRC(&bytes[0], length);
		char sha256[65];
		if (crc.get() != s_runner.loadedMapCRC ||
			!HashSkirmishAITestBytes(&bytes[0], bytes.size(), sha256))
		{
			s_performanceReceipt->invalidate("loaded map content disagrees with the live map");
			return;
		}
		s_performanceReceipt->bindFixture("fresh-ai-map", s_reviewedMapRequest.requested ?
			s_reviewedMapIdentity.logicalKey : s_runner.loadedMapName,
			sha256, static_cast<unsigned>(s_runner.loadedSeed));
	}
	catch (const std::bad_alloc &)
	{
		s_performanceReceipt->invalidate("loaded map hashing storage was unavailable");
	}
}
#endif

rts::JobMetricCounter JobMetricDelta(rts::JobMetricCounter finalValue,
	rts::JobMetricCounter initialValue)
{
	return finalValue >= initialValue ? finalValue - initialValue : finalValue;
}

} // namespace

Bool RetainSkirmishAITestReplayAtomically(const char *sourcePath,
	const char *destinationPath,
	char sha256[SKIRMISH_AI_TEST_RECEIPT_SHA256_LENGTH + 1])
{
	return SkirmishAITestDetail::RetainSkirmishAITestReplayAtomically(
		sourcePath, destinationPath, sha256, nullptr, nullptr, nullptr, nullptr);
}

namespace SkirmishAITestDetail
{
Bool RetainSkirmishAITestReplayAtomically(const char *sourcePath,
	const char *destinationPath,
	char sha256[SKIRMISH_AI_TEST_RECEIPT_SHA256_LENGTH + 1],
	ReplayCommitCallback commitCallback, void *commitContext,
	ReplayFinalHandleCloseCallback finalCloseCallback, void *finalCloseContext)
{
	if (commitCallback == nullptr)
		return RetainSkirmishAITestReplayAtomicallyInternal(sourcePath,
			destinationPath, sha256, nullptr, nullptr,
			finalCloseCallback, finalCloseContext);
	return RetainSkirmishAITestReplayAtomicallyInternal(sourcePath,
		destinationPath, sha256, commitCallback, commitContext,
		finalCloseCallback, finalCloseContext);
}
}

Bool HashSkirmishAITestBytes(const void *bytes, size_t byteCount,
	char sha256[SKIRMISH_AI_TEST_RECEIPT_SHA256_LENGTH + 1])
{
	if ((!bytes && byteCount != 0) || !sha256) return FALSE;
	SkirmishAITestSha256 hash;
	if (byteCount != 0)
		hash.update(static_cast<const unsigned char *>(bytes), byteCount);
	hash.finish(sha256);
	return TRUE;
}

Bool CaptureSkirmishAITestValidatedExecutableHash(
	char sha256[SKIRMISH_AI_TEST_RECEIPT_SHA256_LENGTH + 1])
{
	return CaptureSkirmishAITestExecutableHash(sha256) &&
		(strcmp(s_executableHashInput, "unavailable") == 0 ||
			_stricmp(s_executableHashInput, sha256) == 0);
}

Bool HashSkirmishAITestContentFile(const char *path,
	char sha256[SKIRMISH_AI_TEST_RECEIPT_SHA256_LENGTH + 1])
{
	return HashSkirmishAITestFile(path, sha256);
}

Bool HashSkirmishAITestContentHandle(void *handle,
	char sha256[SKIRMISH_AI_TEST_RECEIPT_SHA256_LENGTH + 1])
{
	return HashSkirmishAITestHandle(handle, sha256);
}

void AccumulateSkirmishAITestDirectPathMetrics(
	DirectPathRuntimeMetrics *baseline,
	const DirectPathRuntimeMetrics &current,
	DirectPathRuntimeMetrics *frozen,
	Bool *hasFrozenActivity,
	Bool *awaitingInitialReset)
{
	if (baseline == nullptr || frozen == nullptr || hasFrozenActivity == nullptr ||
		awaitingInitialReset == nullptr)
		return;
	if (*awaitingInitialReset)
	{
		// Arm occurs in the shell before MSG_NEW_GAME resets Pathfinder. Ignore
		// shell diagnostics until that first epoch transition, then treat the
		// new epoch as the match-local zero baseline.
		if (current.resetEpoch == baseline->resetEpoch)
			return;
		memset(baseline, 0, sizeof(*baseline));
		baseline->resetEpoch = current.resetEpoch;
		*awaitingInitialReset = FALSE;
	}
	else if (current.resetEpoch != baseline->resetEpoch)
	{
		// A later transition is teardown. Frozen match evidence must survive it.
		return;
	}
#define CAPTURE_PATH_COUNTER(member) \
	do { const UnsignedInt captured = current.member >= baseline->member ? \
		current.member - baseline->member : current.member; \
		if (captured > frozen->member) frozen->member = captured; } while (0)
	CAPTURE_PATH_COUNTER(eligibleRequests);
	CAPTURE_PATH_COUNTER(submittedJobs);
	CAPTURE_PATH_COUNTER(executedJobs);
	CAPTURE_PATH_COUNTER(workerExecutedJobs);
	CAPTURE_PATH_COUNTER(ownerHelpedJobs);
	CAPTURE_PATH_COUNTER(authoritativeCommits);
	CAPTURE_PATH_COUNTER(authoritativeMultiWorkerCommits);
	CAPTURE_PATH_COUNTER(staleRejections);
	CAPTURE_PATH_COUNTER(validationFailures);
	CAPTURE_PATH_COUNTER(serialFallbacks);
	CAPTURE_PATH_COUNTER(unsupportedAuthoritativeCommits);
	CAPTURE_PATH_COUNTER(shadowAuthoritativeCommits);
	CAPTURE_PATH_COUNTER(staleAuthoritativeCommits);
	CAPTURE_PATH_COUNTER(malformedAuthoritativeCommits);
	CAPTURE_PATH_COUNTER(shadowOnlyExecutions);
	CAPTURE_PATH_COUNTER(timeoutCancellations);
	CAPTURE_PATH_COUNTER(lateDrainExecutions);
#undef CAPTURE_PATH_COUNTER
	if (current.peakActiveWorkers > frozen->peakActiveWorkers)
		frozen->peakActiveWorkers = current.peakActiveWorkers;
	if (current.minimumCallbackCount != 0 &&
		(frozen->minimumCallbackCount == 0 ||
		 current.minimumCallbackCount < frozen->minimumCallbackCount))
		frozen->minimumCallbackCount = current.minimumCallbackCount;
	if (current.maximumCallbackCount > frozen->maximumCallbackCount)
		frozen->maximumCallbackCount = current.maximumCallbackCount;
	if (frozen->eligibleRequests != 0 || frozen->submittedJobs != 0 ||
		frozen->executedJobs != 0 || frozen->workerExecutedJobs != 0 ||
		frozen->ownerHelpedJobs != 0 || frozen->authoritativeCommits != 0 ||
		frozen->authoritativeMultiWorkerCommits != 0 ||
		frozen->staleRejections != 0 || frozen->validationFailures != 0 ||
		frozen->serialFallbacks != 0 ||
		frozen->unsupportedAuthoritativeCommits != 0 ||
		frozen->shadowAuthoritativeCommits != 0 ||
		frozen->staleAuthoritativeCommits != 0 ||
		frozen->malformedAuthoritativeCommits != 0 ||
		frozen->shadowOnlyExecutions != 0 ||
		frozen->timeoutCancellations != 0 ||
		frozen->lateDrainExecutions != 0 ||
		frozen->peakActiveWorkers != 0 ||
		frozen->minimumCallbackCount != 0 ||
		frozen->maximumCallbackCount != 0)
		*hasFrozenActivity = TRUE;
}

void AccumulateSkirmishAITestOrdinaryPathMetrics(
	OrdinaryPathRuntimeMetrics *baseline,
	const OrdinaryPathRuntimeMetrics &current,
	OrdinaryPathRuntimeMetrics *frozen,
	Bool *awaitingInitialReset)
{
	if (baseline == nullptr || frozen == nullptr ||
		awaitingInitialReset == nullptr)
	{
		return;
	}
	if (*awaitingInitialReset)
	{
		if (current.resetEpoch == baseline->resetEpoch)
			return;
		memset(baseline, 0, sizeof(*baseline));
		baseline->resetEpoch = current.resetEpoch;
		frozen->physicalWorkerMaskComplete = TRUE;
		*awaitingInitialReset = FALSE;
	}
	else if (current.resetEpoch != baseline->resetEpoch)
	{
		// MSG_CLEAR_GAME_DATA starts a new epoch after the match. Preserve the
		// last same-epoch worker and owner-commit evidence for the manifest.
		return;
	}
#define CAPTURE_ORDINARY_PATH_COUNTER(member) \
	do { const UnsignedInt captured = current.member >= baseline->member ? \
		current.member - baseline->member : current.member; \
		if (captured > frozen->member) frozen->member = captured; } while (0)
	CAPTURE_ORDINARY_PATH_COUNTER(eligibleRequests);
	CAPTURE_ORDINARY_PATH_COUNTER(submittedRequests);
	CAPTURE_ORDINARY_PATH_COUNTER(submittedRangeJobs);
	CAPTURE_ORDINARY_PATH_COUNTER(workerExecutedRequests);
	CAPTURE_ORDINARY_PATH_COUNTER(workerExecutedRangeJobs);
	CAPTURE_ORDINARY_PATH_COUNTER(ownerHelpedRangeJobs);
	CAPTURE_ORDINARY_PATH_COUNTER(failedRangeJobs);
	CAPTURE_ORDINARY_PATH_COUNTER(authoritativeCommits);
	CAPTURE_ORDINARY_PATH_COUNTER(authoritativeMultiWorkerCommits);
	CAPTURE_ORDINARY_PATH_COUNTER(staleRejections);
	CAPTURE_ORDINARY_PATH_COUNTER(validationFailures);
	CAPTURE_ORDINARY_PATH_COUNTER(serialFallbacks);
	CAPTURE_ORDINARY_PATH_COUNTER(shadowComparisons);
	CAPTURE_ORDINARY_PATH_COUNTER(shadowMismatches);
	CAPTURE_ORDINARY_PATH_COUNTER(timeoutCancellations);
	CAPTURE_ORDINARY_PATH_COUNTER(lateDrainExecutions);
#undef CAPTURE_ORDINARY_PATH_COUNTER
	frozen->physicalWorkerMask |= current.physicalWorkerMask;
	if (current.distinctPhysicalWorkers > frozen->distinctPhysicalWorkers)
		frozen->distinctPhysicalWorkers = current.distinctPhysicalWorkers;
	if (!current.physicalWorkerMaskComplete)
		frozen->physicalWorkerMaskComplete = FALSE;
	if (current.peakActiveWorkers > frozen->peakActiveWorkers)
		frozen->peakActiveWorkers = current.peakActiveWorkers;
	if (current.maximumBatchRequests > frozen->maximumBatchRequests)
		frozen->maximumBatchRequests = current.maximumBatchRequests;
	if (current.maximumRangeCount > frozen->maximumRangeCount)
		frozen->maximumRangeCount = current.maximumRangeCount;
	if (current.maximumGrainSize > frozen->maximumGrainSize)
		frozen->maximumGrainSize = current.maximumGrainSize;
	frozen->resetEpoch = baseline->resetEpoch;
}

#if defined(_WIN64)
void AccumulateSkirmishAITestCollisionMetrics(
	rts::CollisionCandidateRuntimeMetrics *baseline,
	const rts::CollisionCandidateRuntimeMetrics &current,
	rts::CollisionCandidateRuntimeMetrics *frozen,
	Bool *awaitingInitialReset)
{
	if (baseline == nullptr || frozen == nullptr || awaitingInitialReset == nullptr)
		return;
	if (*awaitingInitialReset)
	{
		if (current.resetEpoch == baseline->resetEpoch)
			return;
		*baseline = rts::CollisionCandidateRuntimeMetrics();
		baseline->resetEpoch = current.resetEpoch;
		*awaitingInitialReset = FALSE;
	}
	else if (current.resetEpoch != baseline->resetEpoch)
	{
		return;
	}
#define CAPTURE_COLLISION_COUNTER(member) \
	frozen->member = current.member - baseline->member
	CAPTURE_COLLISION_COUNTER(authoritativeCommits);
	CAPTURE_COLLISION_COUNTER(shadowExecutions);
	CAPTURE_COLLISION_COUNTER(shadowMismatches);
	CAPTURE_COLLISION_COUNTER(ownerFallbacks);
	CAPTURE_COLLISION_COUNTER(unexpectedFallbacks);
	CAPTURE_COLLISION_COUNTER(ineligibleSlices);
	CAPTURE_COLLISION_COUNTER(staleRejections);
	CAPTURE_COLLISION_COUNTER(committedCandidates);
	CAPTURE_COLLISION_COUNTER(shadowComparedCandidates);
	CAPTURE_COLLISION_COUNTER(preparedPairs);
	CAPTURE_COLLISION_COUNTER(uniqueCandidates);
	CAPTURE_COLLISION_COUNTER(submittedJobs);
	CAPTURE_COLLISION_COUNTER(completedJobs);
	CAPTURE_COLLISION_COUNTER(physicalWorkerJobs);
	CAPTURE_COLLISION_COUNTER(ownerHelpedJobs);
#undef CAPTURE_COLLISION_COUNTER
	// A worker identity mask is an accumulated set, not a monotonic counter.
	// The first match reset establishes an empty epoch-local baseline.
	frozen->physicalWorkerMask = current.physicalWorkerMask;
	frozen->distinctPhysicalWorkers = current.distinctPhysicalWorkers;
	frozen->physicalWorkerMaskComplete = current.physicalWorkerMaskComplete;
	frozen->resetEpoch = baseline->resetEpoch;
}

void AccumulateSkirmishAITestPhysicsMetrics(
	rts::PhysicsIntegrationRuntimeMetrics *baseline,
	const rts::PhysicsIntegrationRuntimeMetrics &current,
	rts::PhysicsIntegrationRuntimeMetrics *frozen,
	Bool *awaitingInitialReset)
{
	if (baseline == nullptr || frozen == nullptr || awaitingInitialReset == nullptr)
		return;
	if (*awaitingInitialReset)
	{
		if (current.resetEpoch == baseline->resetEpoch)
			return;
		*baseline = rts::PhysicsIntegrationRuntimeMetrics();
		baseline->resetEpoch = current.resetEpoch;
		*awaitingInitialReset = FALSE;
	}
	else if (current.resetEpoch != baseline->resetEpoch)
	{
		return;
	}
#define CAPTURE_PHYSICS_COUNTER(member) \
	frozen->member = current.member - baseline->member
	CAPTURE_PHYSICS_COUNTER(acceptedBatches);
	CAPTURE_PHYSICS_COUNTER(acceptedPrefixes);
	CAPTURE_PHYSICS_COUNTER(acceptedRanges);
	CAPTURE_PHYSICS_COUNTER(acceptedSubmittedJobs);
	CAPTURE_PHYSICS_COUNTER(acceptedCompletedJobs);
	CAPTURE_PHYSICS_COUNTER(acceptedPhysicalWorkerJobs);
	CAPTURE_PHYSICS_COUNTER(acceptedOwnerHelpedJobs);
	frozen->acceptedPhysicalWorkerMask = current.acceptedPhysicalWorkerMask;
	if (!current.acceptedPhysicalWorkerMaskComplete)
		frozen->acceptedPhysicalWorkerMaskComplete = false;
	if (current.maximumAcceptedDistinctPhysicalWorkers >
		frozen->maximumAcceptedDistinctPhysicalWorkers)
		frozen->maximumAcceptedDistinctPhysicalWorkers =
			current.maximumAcceptedDistinctPhysicalWorkers;
	if (current.maximumAcceptedPeakConcurrentPhysicalWorkers >
		frozen->maximumAcceptedPeakConcurrentPhysicalWorkers)
		frozen->maximumAcceptedPeakConcurrentPhysicalWorkers =
			current.maximumAcceptedPeakConcurrentPhysicalWorkers;
	CAPTURE_PHYSICS_COUNTER(acceptedAllocatedBytes);
	CAPTURE_PHYSICS_COUNTER(acceptedCaptureNanoseconds);
	CAPTURE_PHYSICS_COUNTER(acceptedPrepareNanoseconds);
	CAPTURE_PHYSICS_COUNTER(acceptedWaitNanoseconds);
	CAPTURE_PHYSICS_COUNTER(acceptedCommitNanoseconds);
	CAPTURE_PHYSICS_COUNTER(acceptedStorageBytes);
	CAPTURE_PHYSICS_COUNTER(acceptedStorageCapacityBytes);
	CAPTURE_PHYSICS_COUNTER(acceptedStorageAllocations);
	CAPTURE_PHYSICS_COUNTER(shadowBatches);
	CAPTURE_PHYSICS_COUNTER(shadowPrefixes);
	CAPTURE_PHYSICS_COUNTER(shadowRanges);
	CAPTURE_PHYSICS_COUNTER(shadowSubmittedJobs);
	CAPTURE_PHYSICS_COUNTER(shadowCompletedJobs);
	CAPTURE_PHYSICS_COUNTER(shadowMatches);
	CAPTURE_PHYSICS_COUNTER(shadowMismatches);
	CAPTURE_PHYSICS_COUNTER(ownerFallbacks);
	CAPTURE_PHYSICS_COUNTER(ineligibleSlices);
	CAPTURE_PHYSICS_COUNTER(unexpectedFallbacks);
	CAPTURE_PHYSICS_COUNTER(staleRejections);
	CAPTURE_PHYSICS_COUNTER(circuitBreakerTrips);
#undef CAPTURE_PHYSICS_COUNTER
	frozen->resetEpoch = baseline->resetEpoch;
}

void AccumulateSkirmishAITestObjectStatusTimerMetrics(
	rts::ObjectStatusTimerRuntimeMetrics *baseline,
	const rts::ObjectStatusTimerRuntimeMetrics &current,
	rts::ObjectStatusTimerRuntimeMetrics *frozen,
	Bool *awaitingInitialReset)
{
	if (baseline == nullptr || frozen == nullptr || awaitingInitialReset == nullptr)
		return;
	if (*awaitingInitialReset)
	{
		if (current.resetEpoch == baseline->resetEpoch)
			return;
		*baseline = rts::ObjectStatusTimerRuntimeMetrics();
		baseline->resetEpoch = current.resetEpoch;
		*awaitingInitialReset = FALSE;
	}
	else if (current.resetEpoch != baseline->resetEpoch)
	{
		return;
	}
#define CAPTURE_STATUS_COUNTER(member) \
	frozen->member = current.member - baseline->member
	CAPTURE_STATUS_COUNTER(authoritativeBatches);
	CAPTURE_STATUS_COUNTER(committedCommands);
	CAPTURE_STATUS_COUNTER(submittedJobs);
	CAPTURE_STATUS_COUNTER(completedJobs);
	CAPTURE_STATUS_COUNTER(physicalWorkerJobs);
	CAPTURE_STATUS_COUNTER(ownerHelpedJobs);
	frozen->physicalWorkerMask = current.physicalWorkerMask;
	if (!current.physicalWorkerMaskComplete)
		frozen->physicalWorkerMaskComplete = false;
	if (current.maximumDistinctPhysicalWorkers >
		frozen->maximumDistinctPhysicalWorkers)
		frozen->maximumDistinctPhysicalWorkers =
			current.maximumDistinctPhysicalWorkers;
	if (current.maximumPeakConcurrentPhysicalWorkers >
		frozen->maximumPeakConcurrentPhysicalWorkers)
		frozen->maximumPeakConcurrentPhysicalWorkers =
			current.maximumPeakConcurrentPhysicalWorkers;
	CAPTURE_STATUS_COUNTER(shadowExecutions);
	CAPTURE_STATUS_COUNTER(shadowCommands);
	CAPTURE_STATUS_COUNTER(shadowMatches);
	CAPTURE_STATUS_COUNTER(shadowMismatches);
	CAPTURE_STATUS_COUNTER(ownerFallbacks);
	CAPTURE_STATUS_COUNTER(staleRejections);
#undef CAPTURE_STATUS_COUNTER
	frozen->resetEpoch = baseline->resetEpoch;
}

void AccumulateSkirmishAITestImmutableSpatialMetrics(
	rts::ImmutableSpatialRuntimeMetrics *baseline,
	const rts::ImmutableSpatialRuntimeMetrics &current,
	rts::ImmutableSpatialRuntimeMetrics *frozen,
	Bool *awaitingInitialReset)
{
	if (baseline == nullptr || frozen == nullptr || awaitingInitialReset == nullptr)
		return;
	if (*awaitingInitialReset)
	{
		if (current.resetEpoch == baseline->resetEpoch)
			return;
		*baseline = rts::ImmutableSpatialRuntimeMetrics();
		baseline->resetEpoch = current.resetEpoch;
		*awaitingInitialReset = FALSE;
	}
	else if (current.resetEpoch != baseline->resetEpoch)
	{
		return;
	}
#define CAPTURE_SPATIAL_COUNTER(member) \
	frozen->member = current.member - baseline->member
#define CAPTURE_SPATIAL_CONSUMER(consumer, member) \
	frozen->consumer.member = current.consumer.member - baseline->consumer.member
	CAPTURE_SPATIAL_COUNTER(capturedArenas);
	CAPTURE_SPATIAL_COUNTER(captureFailures);
	CAPTURE_SPATIAL_COUNTER(successfulCollections);
	CAPTURE_SPATIAL_COUNTER(successfulCollectionQueries);
	CAPTURE_SPATIAL_COUNTER(successfulCollectionRanges);
	CAPTURE_SPATIAL_COUNTER(multiRangeCollections);
	CAPTURE_SPATIAL_COUNTER(collectionSubmittedJobs);
	CAPTURE_SPATIAL_COUNTER(collectionCompletedJobs);
	CAPTURE_SPATIAL_COUNTER(collectionPhysicalWorkerJobs);
	CAPTURE_SPATIAL_COUNTER(collectionOwnerHelpedJobs);
	frozen->collectionPhysicalWorkerMask |=
		current.collectionPhysicalWorkerMask &
		~baseline->collectionPhysicalWorkerMask;
	if (current.maximumCollectionQueries > frozen->maximumCollectionQueries)
		frozen->maximumCollectionQueries = current.maximumCollectionQueries;
	if (current.maximumCollectionRanges > frozen->maximumCollectionRanges)
		frozen->maximumCollectionRanges = current.maximumCollectionRanges;
	if (current.maximumCollectionDistinctPhysicalWorkers >
		frozen->maximumCollectionDistinctPhysicalWorkers)
	{
		frozen->maximumCollectionDistinctPhysicalWorkers =
			current.maximumCollectionDistinctPhysicalWorkers;
	}
#define CAPTURE_SPATIAL_CONSUMER_COUNTERS(consumer) \
	CAPTURE_SPATIAL_CONSUMER(consumer, eligibleQueries); \
	CAPTURE_SPATIAL_CONSUMER(consumer, authoritativeQueries); \
	CAPTURE_SPATIAL_CONSUMER(consumer, authoritativeCandidates); \
	CAPTURE_SPATIAL_CONSUMER(consumer, shadowQueries); \
	CAPTURE_SPATIAL_CONSUMER(consumer, shadowMatches); \
	CAPTURE_SPATIAL_CONSUMER(consumer, shadowMismatches); \
	CAPTURE_SPATIAL_CONSUMER(consumer, submittedJobs); \
	CAPTURE_SPATIAL_CONSUMER(consumer, completedJobs); \
	CAPTURE_SPATIAL_CONSUMER(consumer, physicalWorkerJobs); \
	CAPTURE_SPATIAL_CONSUMER(consumer, ownerHelpedJobs); \
	CAPTURE_SPATIAL_CONSUMER(consumer, expectedFallbacks); \
	CAPTURE_SPATIAL_CONSUMER(consumer, unexpectedFallbacks); \
	CAPTURE_SPATIAL_CONSUMER(consumer, staleRejections); \
	CAPTURE_SPATIAL_CONSUMER(consumer, validationFailures); \
	CAPTURE_SPATIAL_CONSUMER(consumer, circuitBreakerTrips)
	CAPTURE_SPATIAL_CONSUMER_COUNTERS(healing);
	CAPTURE_SPATIAL_CONSUMER_COUNTERS(pointDefenseLaser);
#undef CAPTURE_SPATIAL_CONSUMER_COUNTERS
#undef CAPTURE_SPATIAL_CONSUMER
#undef CAPTURE_SPATIAL_COUNTER
	frozen->resetEpoch = baseline->resetEpoch;
}
#endif

namespace
{

void CaptureSkirmishAITestSliceMetrics()
{
	const DirectPathRuntimeMetrics currentPath = GetDirectPathRuntimeMetrics();
	AccumulateSkirmishAITestDirectPathMetrics(&s_directPathMetricsAtStart,
		currentPath, &s_directPathMetricsFrozen,
		&s_directPathMetricsHaveFrozenActivity,
		&s_directPathMetricsAwaitingInitialReset);
	const OrdinaryPathRuntimeMetrics currentOrdinaryPath =
		GetOrdinaryPathRuntimeMetrics();
	AccumulateSkirmishAITestOrdinaryPathMetrics(
		&s_ordinaryPathMetricsAtStart, currentOrdinaryPath,
		&s_ordinaryPathMetricsFrozen,
		&s_ordinaryPathMetricsAwaitingInitialReset);
#if defined(_WIN64)
	const rts::CollisionCandidateRuntimeMetrics currentCollision =
		rts::GetCollisionCandidateRuntimeMetrics();
	AccumulateSkirmishAITestCollisionMetrics(&s_collisionMetricsAtStart,
		currentCollision, &s_collisionMetricsFrozen,
		&s_collisionMetricsAwaitingInitialReset);
	const rts::PhysicsIntegrationRuntimeMetrics currentPhysics =
		rts::GetPhysicsIntegrationRuntimeMetrics();
	AccumulateSkirmishAITestPhysicsMetrics(&s_physicsMetricsAtStart,
		currentPhysics, &s_physicsMetricsFrozen,
		&s_physicsMetricsAwaitingInitialReset);
	const rts::ObjectStatusTimerRuntimeMetrics currentStatus =
		rts::GetObjectStatusTimerRuntimeMetrics();
	AccumulateSkirmishAITestObjectStatusTimerMetrics(&s_statusMetricsAtStart,
		currentStatus, &s_statusMetricsFrozen,
		&s_statusMetricsAwaitingInitialReset);
	const rts::ImmutableSpatialRuntimeMetrics currentSpatial =
		rts::GetImmutableSpatialRuntimeMetrics();
	AccumulateSkirmishAITestImmutableSpatialMetrics(&s_spatialMetricsAtStart,
		currentSpatial, &s_spatialMetricsFrozen,
		&s_spatialMetricsAwaitingInitialReset);
#endif
}

void PrintJobMetric(const char *name, rts::JobMetricCounter value)
{
#if defined(_MSC_VER) && _MSC_VER < 1300
	printf(" %s=%I64u", name, static_cast<unsigned __int64>(value));
#else
	printf(" %s=%llu", name, static_cast<unsigned long long>(value));
#endif
}

void PrintSkirmishAITestManifest()
{
	CaptureSkirmishAITestRuntimeState();
	const rts::JobSystemMetrics metrics = rts::JobSystem::instance().metrics();
	rts::LiveSimulationPhaseRuntimeMetrics phase;
	phase.attemptedFrames = JobMetricDelta(
		s_phaseMetricsLast.attemptedFrames, s_phaseMetricsAtStart.attemptedFrames);
	phase.completedFrames = JobMetricDelta(
		s_phaseMetricsLast.completedFrames, s_phaseMetricsAtStart.completedFrames);
	phase.stableSequenceFrames = JobMetricDelta(
		s_phaseMetricsLast.stableSequenceFrames,
		s_phaseMetricsAtStart.stableSequenceFrames);
	phase.stoppedByOwnerFrames = JobMetricDelta(
		s_phaseMetricsLast.stoppedByOwnerFrames,
		s_phaseMetricsAtStart.stoppedByOwnerFrames);
	phase.fallbackBeforeMutationFrames = JobMetricDelta(
		s_phaseMetricsLast.fallbackBeforeMutationFrames,
		s_phaseMetricsAtStart.fallbackBeforeMutationFrames);
	phase.failedAfterMutationFrames = JobMetricDelta(
		s_phaseMetricsLast.failedAfterMutationFrames,
		s_phaseMetricsAtStart.failedAfterMutationFrames);
	phase.committedPhases = JobMetricDelta(
		s_phaseMetricsLast.committedPhases, s_phaseMetricsAtStart.committedPhases);
	phase.sequenceViolationFrames = JobMetricDelta(
		s_phaseMetricsLast.sequenceViolationFrames,
		s_phaseMetricsAtStart.sequenceViolationFrames);
	phase.lastFrame = s_phaseMetricsLast.lastFrame;
	phase.lastGeneration = s_phaseMetricsLast.lastGeneration;
	phase.lastCommittedPhaseCount =
		s_phaseMetricsLast.lastCommittedPhaseCount;
	phase.lastSequenceSignature = s_phaseMetricsLast.lastSequenceSignature;
	UnsignedInt phaseOrdinal;
	for (phaseOrdinal = 0;
		phaseOrdinal < rts::LIVE_SIMULATION_PHASE_COUNT - 1; ++phaseOrdinal)
	{
		phase.ownerPhaseTotalNanoseconds[phaseOrdinal] = JobMetricDelta(
			s_phaseMetricsLast.ownerPhaseTotalNanoseconds[phaseOrdinal],
			s_phaseMetricsAtStart.ownerPhaseTotalNanoseconds[phaseOrdinal]);
		phase.ownerPhaseMaximumNanoseconds[phaseOrdinal] =
			s_phaseMetricsLast.ownerPhaseMaximumNanoseconds[phaseOrdinal];
		phase.ownerPhaseSampleCount[phaseOrdinal] = JobMetricDelta(
			s_phaseMetricsLast.ownerPhaseSampleCount[phaseOrdinal],
			s_phaseMetricsAtStart.ownerPhaseSampleCount[phaseOrdinal]);
	}
	phase.frameSimulationTotalNanoseconds = JobMetricDelta(
		s_phaseMetricsLast.frameSimulationTotalNanoseconds,
		s_phaseMetricsAtStart.frameSimulationTotalNanoseconds);
	phase.frameSimulationMaximumNanoseconds =
		s_phaseMetricsLast.frameSimulationMaximumNanoseconds;
	phase.frameSimulationSampleCount = JobMetricDelta(
		s_phaseMetricsLast.frameSimulationSampleCount,
		s_phaseMetricsAtStart.frameSimulationSampleCount);
	phase.serialIslandTotalNanoseconds = JobMetricDelta(
		s_phaseMetricsLast.serialIslandTotalNanoseconds,
		s_phaseMetricsAtStart.serialIslandTotalNanoseconds);
	phase.serialIslandMaximumNanoseconds =
		s_phaseMetricsLast.serialIslandMaximumNanoseconds;
	phase.serialIslandSampleCount = JobMetricDelta(
		s_phaseMetricsLast.serialIslandSampleCount,
		s_phaseMetricsAtStart.serialIslandSampleCount);
	const Bool stablePhaseEvidence =
		rts::HasStableLiveSimulationPhaseEvidence(phase) ? TRUE : FALSE;
	// The recorder publishes end_frame as the winning frame index and validates
	// an inclusive frame count of end_frame + 1. Use the same contract here.
	const UnsignedInt expectedPhaseFrames = s_runner.endFrame < UINT_MAX ?
		s_runner.endFrame + 1U : 0;
	const Bool completePhaseCoverage = stablePhaseEvidence &&
		phase.attemptedFrames == expectedPhaseFrames;
	const Bool releasePhaseWorkerCount =
		rts::IsLiveSimulationPhaseReleaseWorkerCount(
			s_runner.effectiveWorkerCount) ? TRUE : FALSE;
	const Bool amdahlOneWorkerEvidence = completePhaseCoverage &&
		s_runner.effectiveWorkerCount == 1 &&
		phase.frameSimulationTotalNanoseconds != 0 &&
		phase.serialIslandTotalNanoseconds <=
			phase.frameSimulationTotalNanoseconds;
	rts::JobMetricCounter aiCapturedSnapshots = 0;
	rts::JobMetricCounter aiCapturedCandidates = 0;
	rts::JobMetricCounter aiRequestedBatches = 0;
	rts::JobMetricCounter aiSubmittedJobs = 0;
	rts::JobMetricCounter aiCompletedJobs = 0;
	rts::JobMetricCounter aiSerialFallbacks = 0;
	rts::JobMetricCounter aiShadowMatches = 0;
	rts::JobMetricCounter aiShadowMismatches = 0;
	rts::JobMetricCounter aiValidationFailures = 0;
	rts::JobMetricCounter aiCommittedBatches = 0;
	rts::JobMetricCounter aiParallelAuthoritativeCommits = 0;
	rts::JobMetricCounter aiRejectedCommits = 0;
	rts::JobMetricCounter collisionAuthoritativeCommits = 0;
	rts::JobMetricCounter collisionShadowExecutions = 0;
	rts::JobMetricCounter collisionShadowComparedCandidates = 0;
	rts::JobMetricCounter collisionShadowMismatches = 0;
	rts::JobMetricCounter collisionOwnerFallbacks = 0;
	rts::JobMetricCounter collisionUnexpectedFallbacks = 0;
	rts::JobMetricCounter collisionIneligibleSlices = 0;
	rts::JobMetricCounter collisionStaleRejections = 0;
	rts::JobMetricCounter collisionCommittedCandidates = 0;
	rts::JobMetricCounter collisionPreparedPairs = 0;
	rts::JobMetricCounter collisionUniqueCandidates = 0;
	rts::JobMetricCounter collisionSubmittedJobs = 0;
	rts::JobMetricCounter collisionCompletedJobs = 0;
	rts::JobMetricCounter collisionPhysicalWorkerJobs = 0;
	rts::JobMetricCounter collisionOwnerHelpedJobs = 0;
	rts::JobMetricCounter collisionPhysicalWorkerMask = 0;
	UnsignedInt collisionDistinctPhysicalWorkers = 0;
	const DirectPathRuntimeMetrics &path = s_directPathMetricsFrozen;
	const OrdinaryPathRuntimeMetrics &ordinaryPath =
		s_ordinaryPathMetricsFrozen;
#if defined(_WIN64)
	const rts::AIPlanningRuntimeMetrics ai = rts::GetAIPlanningRuntimeMetrics();
	aiCapturedSnapshots = JobMetricDelta(ai.capturedSnapshots,
		s_aiPlanningMetricsAtStart.capturedSnapshots);
	aiCapturedCandidates = JobMetricDelta(ai.capturedCandidates,
		s_aiPlanningMetricsAtStart.capturedCandidates);
	aiRequestedBatches = JobMetricDelta(ai.requestedBatches,
		s_aiPlanningMetricsAtStart.requestedBatches);
	aiSubmittedJobs = JobMetricDelta(ai.submittedJobs,
		s_aiPlanningMetricsAtStart.submittedJobs);
	aiCompletedJobs = JobMetricDelta(ai.completedJobs,
		s_aiPlanningMetricsAtStart.completedJobs);
	aiSerialFallbacks = JobMetricDelta(ai.serialFallbacks,
		s_aiPlanningMetricsAtStart.serialFallbacks);
	aiShadowMatches = JobMetricDelta(ai.shadowMatches,
		s_aiPlanningMetricsAtStart.shadowMatches);
	aiShadowMismatches = JobMetricDelta(ai.shadowMismatches,
		s_aiPlanningMetricsAtStart.shadowMismatches);
	aiValidationFailures = JobMetricDelta(ai.validationFailures,
		s_aiPlanningMetricsAtStart.validationFailures);
	aiCommittedBatches = JobMetricDelta(ai.committedBatches,
		s_aiPlanningMetricsAtStart.committedBatches);
	aiParallelAuthoritativeCommits = JobMetricDelta(
		ai.parallelAuthoritativeCommits,
		s_aiPlanningMetricsAtStart.parallelAuthoritativeCommits);
	aiRejectedCommits = JobMetricDelta(ai.rejectedCommits,
		s_aiPlanningMetricsAtStart.rejectedCommits);
	const rts::CollisionCandidateRuntimeMetrics &collision =
		s_collisionMetricsFrozen;
	collisionAuthoritativeCommits = collision.authoritativeCommits;
	collisionShadowExecutions = collision.shadowExecutions;
	collisionShadowComparedCandidates = collision.shadowComparedCandidates;
	collisionShadowMismatches = collision.shadowMismatches;
	collisionOwnerFallbacks = collision.ownerFallbacks;
	collisionUnexpectedFallbacks = collision.unexpectedFallbacks;
	collisionIneligibleSlices = collision.ineligibleSlices;
	collisionStaleRejections = collision.staleRejections;
	collisionCommittedCandidates = collision.committedCandidates;
	collisionPreparedPairs = collision.preparedPairs;
	collisionUniqueCandidates = collision.uniqueCandidates;
	collisionSubmittedJobs = collision.submittedJobs;
	collisionCompletedJobs = collision.completedJobs;
	collisionPhysicalWorkerJobs = collision.physicalWorkerJobs;
	collisionOwnerHelpedJobs = collision.ownerHelpedJobs;
	collisionPhysicalWorkerMask = collision.physicalWorkerMask;
	collisionDistinctPhysicalWorkers = collision.distinctPhysicalWorkers;
#endif
	char requestedWorkers[16];
	if (s_runner.requestedWorkerCount == 0)
		strlcpy(requestedWorkers, "auto", ARRAY_SIZE(requestedWorkers));
	else
		_snprintf(requestedWorkers, ARRAY_SIZE(requestedWorkers), "%u", s_runner.requestedWorkerCount);
	requestedWorkers[ARRAY_SIZE(requestedWorkers) - 1] = '\0';
	char finalDigest[16];
	if (s_finalDigestAvailable)
		_snprintf(finalDigest, ARRAY_SIZE(finalDigest), "%08X", s_finalDigest);
	else
		strlcpy(finalDigest, "unavailable", ARRAY_SIZE(finalDigest));
	finalDigest[ARRAY_SIZE(finalDigest) - 1] = '\0';
	const char *executableHash = s_executableHashObserved;
	const char *executableHashOrigin = "module";
	if (strcmp(executableHash, "unavailable") == 0)
	{
		executableHash = s_executableHashInput;
		executableHashOrigin = strcmp(s_executableHashInput, "unavailable") == 0
			? "unavailable" : "input";
	}

	printf(" executable_sha256=%s simulation_mode=%s requested_pipeline=%s effective_pipeline=%s "
		"requested_simulation=%s effective_simulation=%s requested_workers=%s effective_workers=%u "
		"worker_policy=%s final_digest=%s wall_ms=%u run_nonce=%s replay_epoch=%d "
		"replay_sha256=%s replay_retained=\"%s\" outcome=winner_team_%d "
		"executable_sha256_origin=%s",
		executableHash, s_simulationModeInput,
		SkirmishAITestPipelineModeName(s_runner.requestedPipelineMode),
		SkirmishAITestPipelineModeName(rts::GetPipelineExecutionMode()),
		SkirmishAITestSimulationModeName(s_runner.requestedSimulationMode),
		SkirmishAITestSimulationModeName(rts::GetSimulationExecutionMode()), requestedWorkers,
		s_runner.effectiveWorkerCount,
		s_runner.workerPolicy == rts::JOB_WORKER_POLICY_ALL ? "all" : "auto",
		finalDigest, ElapsedMilliseconds(s_runner.startupStartMilliseconds, GetTickCount()),
		s_runner.runNonce, s_runner.replayEpoch,
		s_runner.replaySha256[0] != '\0' ? s_runner.replaySha256 : "unavailable",
		s_runner.retainedReplayPath[0] != '\0' ? s_runner.retainedReplayPath : "unavailable",
		s_runner.winnerTeam, executableHashOrigin);
	PrintJobMetric("job_submitted", JobMetricDelta(metrics.submittedJobCount,
		s_jobMetricsAtStart.submittedJobCount));
	PrintJobMetric("job_executed", JobMetricDelta(metrics.executedJobCount,
		s_jobMetricsAtStart.executedJobCount));
	PrintJobMetric("job_steals", JobMetricDelta(metrics.stealCount, s_jobMetricsAtStart.stealCount));
	PrintJobMetric("job_owner_help", JobMetricDelta(metrics.ownerHelpCount,
		s_jobMetricsAtStart.ownerHelpCount));
	PrintJobMetric("job_waits", JobMetricDelta(metrics.waitCount, s_jobMetricsAtStart.waitCount));
	PrintJobMetric("job_worker_wait_reject", JobMetricDelta(metrics.workerWaitRejectionCount,
		s_jobMetricsAtStart.workerWaitRejectionCount));
	PrintJobMetric("job_failed", JobMetricDelta(metrics.failedJobCount, s_jobMetricsAtStart.failedJobCount));
	PrintJobMetric("job_cancelled", JobMetricDelta(metrics.cancelledJobCount,
		s_jobMetricsAtStart.cancelledJobCount));
	PrintJobMetric("job_fallback", JobMetricDelta(metrics.serialFallbackCount,
		s_jobMetricsAtStart.serialFallbackCount));
	PrintJobMetric("job_queue_latency_ns", JobMetricDelta(metrics.totalQueueLatencyNanoseconds,
		s_jobMetricsAtStart.totalQueueLatencyNanoseconds));
	PrintJobMetric("job_max_queue_latency_ns", metrics.maximumQueueLatencyNanoseconds);
	PrintJobMetric("job_sleeps", JobMetricDelta(metrics.workerSleepCount,
		s_jobMetricsAtStart.workerSleepCount));
	PrintJobMetric("job_wakes", JobMetricDelta(metrics.workerWakeCount,
		s_jobMetricsAtStart.workerWakeCount));
	printf(" scheduler_perf_schema=%u scheduler_perf_unit=ns",
		rts::JOB_SYSTEM_PERFORMANCE_SCHEMA_VERSION);
	PrintJobMetric("job_worker_busy_total_ns", JobMetricDelta(
		metrics.workerBusyNanoseconds,
		s_jobMetricsAtStart.workerBusyNanoseconds));
	PrintJobMetric("job_worker_busy_max_ns",
		metrics.maximumWorkerBusyNanoseconds);
	PrintJobMetric("job_worker_busy_samples", JobMetricDelta(
		metrics.workerBusySampleCount,
		s_jobMetricsAtStart.workerBusySampleCount));
	PrintJobMetric("job_worker_wait_total_ns", JobMetricDelta(
		metrics.workerWaitNanoseconds,
		s_jobMetricsAtStart.workerWaitNanoseconds));
	PrintJobMetric("job_worker_wait_max_ns",
		metrics.maximumWorkerWaitNanoseconds);
	PrintJobMetric("job_worker_wait_samples", JobMetricDelta(
		metrics.workerWaitSampleCount,
		s_jobMetricsAtStart.workerWaitSampleCount));
	PrintJobMetric("job_affinity_failures", JobMetricDelta(metrics.affinityFailureCount,
		s_jobMetricsAtStart.affinityFailureCount));
	PrintJobMetric("phase_attempted_frames", phase.attemptedFrames);
	PrintJobMetric("phase_completed_frames", phase.completedFrames);
	PrintJobMetric("phase_stable_sequence_frames", phase.stableSequenceFrames);
	PrintJobMetric("phase_stopped_frames", phase.stoppedByOwnerFrames);
	PrintJobMetric("phase_fallback_before_mutation",
		phase.fallbackBeforeMutationFrames);
	PrintJobMetric("phase_failed_after_mutation",
		phase.failedAfterMutationFrames);
	PrintJobMetric("phase_committed_phases", phase.committedPhases);
	PrintJobMetric("phase_sequence_violations", phase.sequenceViolationFrames);
	printf(" phase_perf_schema=%u phase_perf_unit=ns phase_perf_crc_excluded=1",
		rts::LIVE_SIMULATION_PHASE_PERFORMANCE_SCHEMA_VERSION);
	static const char *phasePerformanceNames[
		rts::LIVE_SIMULATION_PHASE_COUNT - 1] = {
		"owner_intake", "legacy_mutable_island", "spatial", "owner_tail",
		"verification_publication"
	};
	for (phaseOrdinal = 0;
		phaseOrdinal < rts::LIVE_SIMULATION_PHASE_COUNT - 1; ++phaseOrdinal)
	{
		char fieldName[96];
		_snprintf(fieldName, ARRAY_SIZE(fieldName), "phase_%s_total_ns",
			phasePerformanceNames[phaseOrdinal]);
		fieldName[ARRAY_SIZE(fieldName) - 1] = '\0';
		PrintJobMetric(fieldName,
			phase.ownerPhaseTotalNanoseconds[phaseOrdinal]);
		_snprintf(fieldName, ARRAY_SIZE(fieldName), "phase_%s_max_ns",
			phasePerformanceNames[phaseOrdinal]);
		fieldName[ARRAY_SIZE(fieldName) - 1] = '\0';
		PrintJobMetric(fieldName,
			phase.ownerPhaseMaximumNanoseconds[phaseOrdinal]);
		_snprintf(fieldName, ARRAY_SIZE(fieldName), "phase_%s_samples",
			phasePerformanceNames[phaseOrdinal]);
		fieldName[ARRAY_SIZE(fieldName) - 1] = '\0';
		PrintJobMetric(fieldName,
			phase.ownerPhaseSampleCount[phaseOrdinal]);
	}
	PrintJobMetric("simulation_frame_total_ns",
		phase.frameSimulationTotalNanoseconds);
	PrintJobMetric("simulation_frame_max_ns",
		phase.frameSimulationMaximumNanoseconds);
	PrintJobMetric("simulation_frame_samples",
		phase.frameSimulationSampleCount);
	PrintJobMetric("simulation_serial_island_total_ns",
		phase.serialIslandTotalNanoseconds);
	PrintJobMetric("simulation_serial_island_max_ns",
		phase.serialIslandMaximumNanoseconds);
	PrintJobMetric("simulation_serial_island_samples",
		phase.serialIslandSampleCount);
	printf(" amdahl_one_worker_eligible=%u", amdahlOneWorkerEvidence);
	PrintJobMetric("amdahl_one_worker_frame_total_ns",
		amdahlOneWorkerEvidence ? phase.frameSimulationTotalNanoseconds : 0);
	PrintJobMetric("amdahl_one_worker_serial_total_ns",
		amdahlOneWorkerEvidence ? phase.serialIslandTotalNanoseconds : 0);
	PrintJobMetric("amdahl_one_worker_frame_samples",
		amdahlOneWorkerEvidence ? phase.frameSimulationSampleCount : 0);
	printf(" phase_expected_frames=%u phase_last_frame=%u phase_last_generation=%u "
		"phase_last_committed_phases=%u phase_last_sequence=%u "
		"phase_evidence_stable=%u phase_matches_end_frame=%u "
		"phase_release_worker_count_supported=%u",
		expectedPhaseFrames, phase.lastFrame, phase.lastGeneration,
		phase.lastCommittedPhaseCount,
		phase.lastSequenceSignature, stablePhaseEvidence,
		completePhaseCoverage, releasePhaseWorkerCount);
	// These slice-specific counters prove that authoritative AI planning work,
	// rather than an unrelated JobSystem consumer or duplicate shadow path job,
	// reached the owner-thread commit boundary during the live match.
	PrintJobMetric("authoritative_commits", aiParallelAuthoritativeCommits);
	PrintJobMetric("shadow_executions", aiShadowMatches + aiShadowMismatches);
	PrintJobMetric("owner_fallbacks", aiSerialFallbacks);
	PrintJobMetric("ai_captured_snapshots", aiCapturedSnapshots);
	PrintJobMetric("ai_captured_candidates", aiCapturedCandidates);
	PrintJobMetric("ai_requested_batches", aiRequestedBatches);
	PrintJobMetric("ai_submitted_jobs", aiSubmittedJobs);
	PrintJobMetric("ai_completed_jobs", aiCompletedJobs);
	PrintJobMetric("ai_serial_fallbacks", aiSerialFallbacks);
	PrintJobMetric("ai_shadow_matches", aiShadowMatches);
	PrintJobMetric("ai_shadow_mismatches", aiShadowMismatches);
	PrintJobMetric("ai_validation_failures", aiValidationFailures);
	PrintJobMetric("ai_committed_batches", aiCommittedBatches);
	PrintJobMetric("ai_parallel_authoritative_commits",
		aiParallelAuthoritativeCommits);
	PrintJobMetric("ai_rejected_commits", aiRejectedCommits);
	PrintJobMetric("direct_eligible", path.eligibleRequests);
	PrintJobMetric("direct_submitted", path.submittedJobs);
	PrintJobMetric("direct_executed", path.executedJobs);
	PrintJobMetric("direct_worker_executed", path.workerExecutedJobs);
	PrintJobMetric("direct_owner_helped", path.ownerHelpedJobs);
	PrintJobMetric("direct_authoritative_commits", path.authoritativeCommits);
	PrintJobMetric("direct_authoritative_multiworker_commits",
		path.authoritativeMultiWorkerCommits);
	PrintJobMetric("direct_stale_rejections", path.staleRejections);
	PrintJobMetric("direct_validation_failures", path.validationFailures);
	PrintJobMetric("direct_serial_fallbacks", path.serialFallbacks);
	PrintJobMetric("direct_unsupported_authority",
		path.unsupportedAuthoritativeCommits);
	PrintJobMetric("direct_shadow_authority", path.shadowAuthoritativeCommits);
	PrintJobMetric("direct_stale_acceptance", path.staleAuthoritativeCommits);
	PrintJobMetric("direct_malformed_acceptance",
		path.malformedAuthoritativeCommits);
	PrintJobMetric("direct_shadow_only", path.shadowOnlyExecutions);
	PrintJobMetric("direct_timeouts", path.timeoutCancellations);
	PrintJobMetric("direct_late_drains", path.lateDrainExecutions);
	PrintJobMetric("direct_peak_active_workers", path.peakActiveWorkers);
	PrintJobMetric("direct_callback_min", path.minimumCallbackCount);
	PrintJobMetric("direct_callback_max", path.maximumCallbackCount);
	PrintJobMetric("ordinary_path_eligible", ordinaryPath.eligibleRequests);
	PrintJobMetric("ordinary_path_submitted_requests",
		ordinaryPath.submittedRequests);
	PrintJobMetric("ordinary_path_submitted_ranges",
		ordinaryPath.submittedRangeJobs);
	PrintJobMetric("ordinary_path_worker_executed_requests",
		ordinaryPath.workerExecutedRequests);
	PrintJobMetric("ordinary_path_worker_executed_range_jobs",
		ordinaryPath.workerExecutedRangeJobs);
	PrintJobMetric("ordinary_path_owner_helped_range_jobs",
		ordinaryPath.ownerHelpedRangeJobs);
	PrintJobMetric("ordinary_path_failed_range_jobs",
		ordinaryPath.failedRangeJobs);
	PrintJobMetric("ordinary_path_physical_worker_mask",
		ordinaryPath.physicalWorkerMask);
	PrintJobMetric("ordinary_path_distinct_physical_workers",
		ordinaryPath.distinctPhysicalWorkers);
	PrintJobMetric("ordinary_path_physical_worker_mask_complete",
		ordinaryPath.physicalWorkerMaskComplete ? 1 : 0);
	PrintJobMetric("ordinary_path_authoritative_commits",
		ordinaryPath.authoritativeCommits);
	PrintJobMetric("ordinary_path_authoritative_multiworker_commits",
		ordinaryPath.authoritativeMultiWorkerCommits);
	PrintJobMetric("ordinary_path_stale_rejections",
		ordinaryPath.staleRejections);
	PrintJobMetric("ordinary_path_validation_failures",
		ordinaryPath.validationFailures);
	PrintJobMetric("ordinary_path_serial_fallbacks",
		ordinaryPath.serialFallbacks);
	PrintJobMetric("ordinary_path_shadow_comparisons",
		ordinaryPath.shadowComparisons);
	PrintJobMetric("ordinary_path_shadow_mismatches",
		ordinaryPath.shadowMismatches);
	PrintJobMetric("ordinary_path_timeouts",
		ordinaryPath.timeoutCancellations);
	PrintJobMetric("ordinary_path_late_drains",
		ordinaryPath.lateDrainExecutions);
	PrintJobMetric("ordinary_path_peak_active_workers",
		ordinaryPath.peakActiveWorkers);
	PrintJobMetric("ordinary_path_max_batch_requests",
		ordinaryPath.maximumBatchRequests);
	PrintJobMetric("ordinary_path_max_range_count",
		ordinaryPath.maximumRangeCount);
	PrintJobMetric("ordinary_path_max_grain_size",
		ordinaryPath.maximumGrainSize);
	PrintJobMetric("collision_authoritative_commits",
		collisionAuthoritativeCommits);
	PrintJobMetric("collision_shadow_executions", collisionShadowExecutions);
	PrintJobMetric("collision_shadow_compared_candidates",
		collisionShadowComparedCandidates);
	PrintJobMetric("collision_shadow_mismatches", collisionShadowMismatches);
	PrintJobMetric("collision_owner_fallbacks", collisionOwnerFallbacks);
	PrintJobMetric("collision_unexpected_fallbacks",
		collisionUnexpectedFallbacks);
	PrintJobMetric("collision_ineligible_slices", collisionIneligibleSlices);
	PrintJobMetric("collision_stale_rejections", collisionStaleRejections);
	PrintJobMetric("collision_committed_candidates",
		collisionCommittedCandidates);
	PrintJobMetric("collision_prepared_pairs", collisionPreparedPairs);
	PrintJobMetric("collision_unique_candidates", collisionUniqueCandidates);
	PrintJobMetric("collision_submitted_jobs", collisionSubmittedJobs);
	PrintJobMetric("collision_completed_jobs", collisionCompletedJobs);
	PrintJobMetric("collision_physical_worker_jobs",
		collisionPhysicalWorkerJobs);
	PrintJobMetric("collision_owner_helped_jobs", collisionOwnerHelpedJobs);
	PrintJobMetric("collision_physical_worker_mask",
		collisionPhysicalWorkerMask);
	PrintJobMetric("collision_distinct_physical_workers",
		collisionDistinctPhysicalWorkers);
	#if defined(_WIN64)
	PrintJobMetric("collision_physical_worker_mask_complete",
		collision.physicalWorkerMaskComplete ? 1 : 0);
	#else
	PrintJobMetric("collision_physical_worker_mask_complete", 0);
	#endif
#if defined(_WIN64)
	PrintJobMetric("physics_authoritative_batches",
		s_physicsMetricsFrozen.acceptedBatches);
	PrintJobMetric("physics_committed_prefixes",
		s_physicsMetricsFrozen.acceptedPrefixes);
	PrintJobMetric("physics_ranges", s_physicsMetricsFrozen.acceptedRanges);
	PrintJobMetric("physics_submitted_jobs",
		s_physicsMetricsFrozen.acceptedSubmittedJobs);
	PrintJobMetric("physics_completed_jobs",
		s_physicsMetricsFrozen.acceptedCompletedJobs);
	PrintJobMetric("physics_physical_worker_jobs",
		s_physicsMetricsFrozen.acceptedPhysicalWorkerJobs);
	PrintJobMetric("physics_owner_helped_jobs",
		s_physicsMetricsFrozen.acceptedOwnerHelpedJobs);
	PrintJobMetric("physics_physical_worker_mask",
		s_physicsMetricsFrozen.acceptedPhysicalWorkerMask);
	PrintJobMetric("physics_distinct_physical_workers",
		s_physicsMetricsFrozen.maximumAcceptedDistinctPhysicalWorkers);
	PrintJobMetric("physics_physical_worker_mask_complete",
		s_physicsMetricsFrozen.acceptedPhysicalWorkerMaskComplete ? 1 : 0);
	PrintJobMetric("physics_peak_concurrent_physical_workers",
		s_physicsMetricsFrozen.maximumAcceptedPeakConcurrentPhysicalWorkers);
	PrintJobMetric("physics_allocated_bytes",
		s_physicsMetricsFrozen.acceptedAllocatedBytes);
	PrintJobMetric("physics_capture_ns",
		s_physicsMetricsFrozen.acceptedCaptureNanoseconds);
	PrintJobMetric("physics_prepare_ns",
		s_physicsMetricsFrozen.acceptedPrepareNanoseconds);
	PrintJobMetric("physics_wait_ns",
		s_physicsMetricsFrozen.acceptedWaitNanoseconds);
	PrintJobMetric("physics_commit_ns",
		s_physicsMetricsFrozen.acceptedCommitNanoseconds);
	PrintJobMetric("physics_storage_bytes",
		s_physicsMetricsFrozen.acceptedStorageBytes);
	PrintJobMetric("physics_storage_capacity_bytes",
		s_physicsMetricsFrozen.acceptedStorageCapacityBytes);
	PrintJobMetric("physics_storage_allocations",
		s_physicsMetricsFrozen.acceptedStorageAllocations);
	PrintJobMetric("physics_shadow_executions", s_physicsMetricsFrozen.shadowBatches);
	PrintJobMetric("physics_shadow_prefixes", s_physicsMetricsFrozen.shadowPrefixes);
	PrintJobMetric("physics_shadow_ranges", s_physicsMetricsFrozen.shadowRanges);
	PrintJobMetric("physics_shadow_submitted_jobs",
		s_physicsMetricsFrozen.shadowSubmittedJobs);
	PrintJobMetric("physics_shadow_completed_jobs",
		s_physicsMetricsFrozen.shadowCompletedJobs);
	PrintJobMetric("physics_shadow_matches", s_physicsMetricsFrozen.shadowMatches);
	PrintJobMetric("physics_shadow_mismatches", s_physicsMetricsFrozen.shadowMismatches);
	PrintJobMetric("physics_owner_fallbacks", s_physicsMetricsFrozen.ownerFallbacks);
	PrintJobMetric("physics_ineligible_slices", s_physicsMetricsFrozen.ineligibleSlices);
	PrintJobMetric("physics_unexpected_fallbacks",
		s_physicsMetricsFrozen.unexpectedFallbacks);
	PrintJobMetric("physics_stale_rejections", s_physicsMetricsFrozen.staleRejections);
	PrintJobMetric("physics_circuit_breaker_trips",
		s_physicsMetricsFrozen.circuitBreakerTrips);
	PrintJobMetric("status_authoritative_batches",
		s_statusMetricsFrozen.authoritativeBatches);
	PrintJobMetric("status_committed_commands",
		s_statusMetricsFrozen.committedCommands);
	PrintJobMetric("status_submitted_jobs", s_statusMetricsFrozen.submittedJobs);
	PrintJobMetric("status_completed_jobs", s_statusMetricsFrozen.completedJobs);
	PrintJobMetric("status_physical_worker_jobs",
		s_statusMetricsFrozen.physicalWorkerJobs);
	PrintJobMetric("status_owner_helped_jobs",
		s_statusMetricsFrozen.ownerHelpedJobs);
	PrintJobMetric("status_physical_worker_mask",
		s_statusMetricsFrozen.physicalWorkerMask);
	PrintJobMetric("status_distinct_physical_workers",
		s_statusMetricsFrozen.maximumDistinctPhysicalWorkers);
	PrintJobMetric("status_physical_worker_mask_complete",
		s_statusMetricsFrozen.physicalWorkerMaskComplete ? 1 : 0);
	PrintJobMetric("status_peak_concurrent_physical_workers",
		s_statusMetricsFrozen.maximumPeakConcurrentPhysicalWorkers);
	PrintJobMetric("status_shadow_executions",
		s_statusMetricsFrozen.shadowExecutions);
	PrintJobMetric("status_shadow_commands", s_statusMetricsFrozen.shadowCommands);
	PrintJobMetric("status_shadow_matches", s_statusMetricsFrozen.shadowMatches);
	PrintJobMetric("status_shadow_mismatches",
		s_statusMetricsFrozen.shadowMismatches);
	PrintJobMetric("status_owner_fallbacks", s_statusMetricsFrozen.ownerFallbacks);
	PrintJobMetric("status_stale_rejections", s_statusMetricsFrozen.staleRejections);
	const rts::ImmutableSpatialRuntimeMetrics &spatial = s_spatialMetricsFrozen;
	PrintJobMetric("spatial_captured_arenas", spatial.capturedArenas);
	PrintJobMetric("spatial_capture_failures", spatial.captureFailures);
	PrintJobMetric("spatial_successful_collections", spatial.successfulCollections);
	PrintJobMetric("spatial_successful_collection_queries",
		spatial.successfulCollectionQueries);
	PrintJobMetric("spatial_successful_collection_ranges",
		spatial.successfulCollectionRanges);
	PrintJobMetric("spatial_multi_range_collections",
		spatial.multiRangeCollections);
	PrintJobMetric("spatial_collection_submitted_jobs",
		spatial.collectionSubmittedJobs);
	PrintJobMetric("spatial_collection_completed_jobs",
		spatial.collectionCompletedJobs);
	PrintJobMetric("spatial_collection_physical_worker_jobs",
		spatial.collectionPhysicalWorkerJobs);
	PrintJobMetric("spatial_collection_owner_helped_jobs",
		spatial.collectionOwnerHelpedJobs);
	PrintJobMetric("spatial_collection_physical_worker_mask",
		spatial.collectionPhysicalWorkerMask);
	PrintJobMetric("spatial_maximum_collection_queries",
		spatial.maximumCollectionQueries);
	PrintJobMetric("spatial_maximum_collection_ranges",
		spatial.maximumCollectionRanges);
	PrintJobMetric("spatial_maximum_collection_distinct_physical_workers",
		spatial.maximumCollectionDistinctPhysicalWorkers);
#define PRINT_SPATIAL_CONSUMER(prefix, consumer) \
	PrintJobMetric(prefix "_eligible_queries", consumer.eligibleQueries); \
	PrintJobMetric(prefix "_authoritative_queries", consumer.authoritativeQueries); \
	PrintJobMetric(prefix "_authoritative_candidates", consumer.authoritativeCandidates); \
	PrintJobMetric(prefix "_shadow_queries", consumer.shadowQueries); \
	PrintJobMetric(prefix "_shadow_matches", consumer.shadowMatches); \
	PrintJobMetric(prefix "_shadow_mismatches", consumer.shadowMismatches); \
	PrintJobMetric(prefix "_submitted_jobs", consumer.submittedJobs); \
	PrintJobMetric(prefix "_completed_jobs", consumer.completedJobs); \
	PrintJobMetric(prefix "_physical_worker_jobs", consumer.physicalWorkerJobs); \
	PrintJobMetric(prefix "_owner_helped_jobs", consumer.ownerHelpedJobs); \
	PrintJobMetric(prefix "_expected_fallbacks", consumer.expectedFallbacks); \
	PrintJobMetric(prefix "_unexpected_fallbacks", consumer.unexpectedFallbacks); \
	PrintJobMetric(prefix "_stale_rejections", consumer.staleRejections); \
	PrintJobMetric(prefix "_validation_failures", consumer.validationFailures); \
	PrintJobMetric(prefix "_circuit_breaker_trips", consumer.circuitBreakerTrips)
	PRINT_SPATIAL_CONSUMER("spatial_healing", spatial.healing);
	PRINT_SPATIAL_CONSUMER("spatial_pdl", spatial.pointDefenseLaser);
#undef PRINT_SPATIAL_CONSUMER
#endif
	printf(" job_queue_high_water=%u job_peak_active_workers=%u available_cpus=%u "
		"reserved_owner_cpus=%u selected_worker_cpus=%u\n",
		metrics.injectionHighWater, metrics.maximumActiveWorkers,
		metrics.availableLogicalCpuCount, metrics.reservedOwnerCpuCount,
		metrics.selectedWorkerCpuCount);
}
} // namespace
void FailSkirmishAITest(const char *reason)
{
	s_runner.failed = TRUE;
	s_runner.failureReason = reason;
}

void RequestSkirmishAITestStop()
{
	CaptureSkirmishAITestRuntimeState();
	if (TheGameLogic && TheGameLogic->isInGame())
	{
		if (!s_runner.ending)
		{
			TheGameLogic->exitGame();
			s_runner.ending = TRUE;
			s_runner.shutdownStartMilliseconds = GetTickCount();
		}
	}
	else if (TheGameEngine)
	{
		TheGameEngine->setQuitting(TRUE);
	}
}

Bool IsSkirmishAITest4v2(SkirmishAITestScenario scenario)
{
	return scenario == SKIRMISH_AI_TEST_SCENARIO_4V2;
}

Bool IsSkirmishAITestHardAI2v6(SkirmishAITestScenario scenario)
{
	return scenario == SKIRMISH_AI_TEST_SCENARIO_HARD_AI_2V6;
}

Int ExpectedSkirmishAITestAiCount(SkirmishAITestScenario scenario)
{
	if (IsSkirmishAITestHardAI2v6(scenario))
		return SKIRMISH_AI_TEST_SLOT_COUNT;
	return IsSkirmishAITest4v2(scenario) ? 6 : 7;
}

const char *SkirmishAITestScenarioName(SkirmishAITestScenario scenario)
{
	if (IsSkirmishAITest4v2(scenario))
		return "4v2";
	if (scenario == SKIRMISH_AI_TEST_SCENARIO_PRACTICAL_1V7)
		return "practical-1v7";
	if (IsSkirmishAITestHardAI2v6(scenario))
		return "hard-ai-2v6";
	return "4v3";
}

Bool IsSkirmishAITestPracticalControllerScenario(
	SkirmishAITestScenario scenario)
{
	return scenario == SKIRMISH_AI_TEST_SCENARIO_PRACTICAL_1V7;
}

namespace
{
Bool IsSkirmishAIRecoveryFactoryFixture()
{
	return s_recovery.fixtureCase == SKIRMISH_AI_RECOVERY_FACTORY_ONLY ||
		s_recovery.fixtureCase == SKIRMISH_AI_RECOVERY_DISABLED_FACTORY;
}

Bool IsLiveSkirmishAIRecoveryObject(const Object *object)
{
	return object != nullptr && !object->isDestroyed() && !object->isEffectivelyDead();
}

Int FindPlayerTemplateIndex(const char *templateName)
{
	if (!templateName || !ThePlayerTemplateStore)
		return -1;

	for (Int i = 0; i < ThePlayerTemplateStore->getPlayerTemplateCount(); ++i)
	{
		const PlayerTemplate *playerTemplate = ThePlayerTemplateStore->getNthPlayerTemplate(i);
		if (playerTemplate && playerTemplate->getName().compareNoCase(templateName) == 0 &&
			playerTemplate->isPlayableSide() && playerTemplate->getStartingBuilding().isNotEmpty())
			return i;
	}
	return -1;
}

Player *GetSkirmishAIRecoveryFixturePlayer()
{
	return ThePlayerList ? ThePlayerList->getPlayerFromSlotIndex(1) : nullptr;
}

Object *FindSkirmishAIRecoveryCommandCenter(
	Player *player, const ThingTemplate *primaryTemplate, Int *count, Bool *underConstruction)
{
	if (count)
		*count = 0;
	if (underConstruction)
		*underConstruction = FALSE;
	if (!player || !primaryTemplate || !TheGameLogic)
		return nullptr;

	Object *best = nullptr;
	for (Object *object = TheGameLogic->getFirstObject(); object;
		object = object->getNextObject())
	{
		if (!IsLiveSkirmishAIRecoveryObject(object) ||
			object->getControllingPlayer() != player ||
			!object->isKindOf(KINDOF_COMMANDCENTER) ||
			object->testStatus(OBJECT_STATUS_SOLD) ||
			!object->getTemplate()->isEquivalentTo(primaryTemplate))
			continue;

		if (count)
			++*count;
		if (object->testStatus(OBJECT_STATUS_UNDER_CONSTRUCTION) && underConstruction)
			*underConstruction = TRUE;
		if (!best || object->getID() < best->getID())
			best = object;
	}
	return best;
}

void PrintSkirmishAIRecoveryDuplicateCommandCenterDiagnostics(
	Player *player, const ThingTemplate *primaryTemplate)
{
	if (!player || !primaryTemplate || !TheGameLogic)
		return;
	printf("SKIRMISH_AI_RECOVERY_DIAGNOSTIC phase=duplicate_command_centers "
		"frame=%u cash=%u\n", TheGameLogic->getFrame(),
		player->getMoney() ? player->getMoney()->countMoney() : 0);
	for (Object *object = TheGameLogic->getFirstObject(); object;
		object = object->getNextObject())
	{
		if (object->getControllingPlayer() != player ||
			!object->isKindOf(KINDOF_COMMANDCENTER) || !object->getTemplate() ||
			!object->getTemplate()->isEquivalentTo(primaryTemplate))
			continue;
		const Coord3D *position = object->getPosition();
		printf("SKIRMISH_AI_RECOVERY_DIAGNOSTIC duplicate_center id=%u "
			"under_construction=%d reconstructing=%d sold=%d destroyed=%d "
			"effectively_dead=%d producer=%u builder=%u percent=%g "
			"pos=(%g,%g,%g)\n",
			object->getID(), object->testStatus(OBJECT_STATUS_UNDER_CONSTRUCTION),
			object->testStatus(OBJECT_STATUS_RECONSTRUCTING),
			object->testStatus(OBJECT_STATUS_SOLD), object->isDestroyed(),
			object->isEffectivelyDead(), object->getProducerID(),
			object->getBuilderID(), object->getConstructionPercent(),
			position ? position->x : 0.0f, position ? position->y : 0.0f,
			position ? position->z : 0.0f);
	}
	fflush(stdout);
}

BuildListInfo *FindSkirmishAIRecoveryCommandCenterBuildInfo(
	Player *player, const ThingTemplate *primaryTemplate)
{
	if (!player || !primaryTemplate || !TheThingFactory)
		return nullptr;

	for (BuildListInfo *info = player->getBuildList(); info; info = info->getNext())
	{
		const ThingTemplate *plan = TheThingFactory->findTemplate(info->getTemplateName());
		if (plan && plan->isEquivalentTo(primaryTemplate))
			return info;
	}
	return nullptr;
}

Int CountSkirmishAIRecoveryBuilders(Player *player, const ThingTemplate **builderTemplate)
{
	if (builderTemplate)
		*builderTemplate = nullptr;
	if (!player || !TheGameLogic)
		return 0;

	Int count = 0;
	for (Object *object = TheGameLogic->getFirstObject(); object;
		object = object->getNextObject())
	{
		if (!IsLiveSkirmishAIRecoveryObject(object) ||
			object->isContained() ||
			object->getControllingPlayer() != player ||
			!object->isKindOf(KINDOF_DOZER) || !object->getAIUpdateInterface() ||
			!object->getAIUpdateInterface()->getDozerAIInterface())
			continue;

		++count;
		if (builderTemplate && *builderTemplate == nullptr)
			*builderTemplate = object->getTemplate();
	}
	return count;
}

Int CountSkirmishAIRecoveryBuildersForTemplate(
	Player *player, const ThingTemplate *builderTemplate)
{
	if (!player || !builderTemplate || !TheGameLogic)
		return 0;

	Int count = 0;
	for (Object *object = TheGameLogic->getFirstObject(); object;
		object = object->getNextObject())
	{
		if (!IsLiveSkirmishAIRecoveryObject(object) ||
			object->isContained() ||
			object->getControllingPlayer() != player ||
			!object->isKindOf(KINDOF_DOZER) || !object->getAIUpdateInterface() ||
			!object->getAIUpdateInterface()->getDozerAIInterface() ||
			!object->getTemplate()->isEquivalentTo(builderTemplate))
			continue;
		++count;
	}
	return count;
}

Bool HasSkirmishAIRecoveryBuilderTemplate(Player *player, const ThingTemplate *builderTemplate)
{
	if (!player || !builderTemplate || !TheGameLogic)
		return FALSE;

	for (Object *object = TheGameLogic->getFirstObject(); object;
		object = object->getNextObject())
	{
		if (!IsLiveSkirmishAIRecoveryObject(object) ||
			object->isContained() ||
			object->getControllingPlayer() != player ||
			!object->isKindOf(KINDOF_DOZER) || !object->getAIUpdateInterface() ||
			!object->getAIUpdateInterface()->getDozerAIInterface() ||
			!object->getTemplate()->isEquivalentTo(builderTemplate))
			continue;
		return TRUE;
	}
	return FALSE;
}

Bool HasSkirmishAIRecoveryPendingBuilderWork(Player *player)
{
	if (!player || !TheGameLogic)
		return FALSE;

	for (Object *object = TheGameLogic->getFirstObject(); object;
		object = object->getNextObject())
	{
		if (!IsLiveSkirmishAIRecoveryObject(object) ||
			object->isContained() ||
			object->getControllingPlayer() != player ||
			!object->isKindOf(KINDOF_DOZER) || !object->getAIUpdateInterface())
			continue;
		DozerAIInterface *dozerAI =
			object->getAIUpdateInterface()->getDozerAIInterface();
		if (dozerAI && dozerAI->isTaskPending(DOZER_TASK_BUILD))
			return TRUE;
	}
	return FALSE;
}

const ThingTemplate *ResolveSkirmishAIRecoveryBuilderTemplate(
	Player *player, const ThingTemplate *primaryTemplate)
{
	if (!player || !primaryTemplate || !TheGameLogic || !TheBuildAssistant ||
		!TheThingFactory)
		return nullptr;

	// Start from the subject's actual PlayerTemplate starting unit. This keeps
	// USA/China dozers and GLA workers tied to the selected general instead of
	// whichever global DOZER template happens to appear first.
	const PlayerTemplate *playerTemplate = player->getPlayerTemplate();
	if (playerTemplate)
	{
		const AsciiString startingUnit = playerTemplate->getStartingUnit(0);
		if (startingUnit.isNotEmpty())
		{
			const ThingTemplate *candidate = TheThingFactory->findTemplate(startingUnit);
			if (candidate && candidate->isKindOf(KINDOF_DOZER))
			{
				for (Object *object = TheGameLogic->getFirstObject(); object;
					object = object->getNextObject())
				{
					if (!IsLiveSkirmishAIRecoveryObject(object) ||
						object->isContained() ||
						object->getControllingPlayer() != player ||
						!object->isKindOf(KINDOF_DOZER) ||
						!object->getAIUpdateInterface() ||
						!object->getAIUpdateInterface()->getDozerAIInterface() ||
						!object->getTemplate()->isEquivalentTo(candidate))
						continue;
					if (TheBuildAssistant->isPossibleToMakeUnit(object, primaryTemplate))
						return candidate;
				}
			}
		}
	}

	// A map/general may have a nonstandard starting-unit slot. Resolve the
	// first live subject-owned builder whose command set can construct the
	// subject's primary command center.
	for (Object *object = TheGameLogic->getFirstObject(); object;
		object = object->getNextObject())
	{
		if (!IsLiveSkirmishAIRecoveryObject(object) ||
			object->isContained() ||
			object->getControllingPlayer() != player ||
			!object->isKindOf(KINDOF_DOZER) || !object->getAIUpdateInterface() ||
			!object->getAIUpdateInterface()->getDozerAIInterface())
			continue;
		if (TheBuildAssistant->isPossibleToMakeUnit(object, primaryTemplate))
			return object->getTemplate();
	}
	return nullptr;
}

Int CountSkirmishAIRecoveryBuilderFactories(
	Player *player, const ThingTemplate *builderTemplate, Object *excluded,
	Object **firstFactory)
{
	if (firstFactory)
		*firstFactory = nullptr;
	if (!player || !builderTemplate || !TheGameLogic || !TheBuildAssistant)
		return 0;

	Int count = 0;
	for (Object *object = TheGameLogic->getFirstObject(); object;
		object = object->getNextObject())
	{
		if (!IsLiveSkirmishAIRecoveryObject(object) || object == excluded ||
			object->getControllingPlayer() != player || !object->isStructure() ||
			object->testStatus(OBJECT_STATUS_UNDER_CONSTRUCTION) ||
			object->testStatus(OBJECT_STATUS_SOLD) ||
			!object->getProductionUpdateInterface() ||
			!TheBuildAssistant->isPossibleToMakeUnit(object, builderTemplate))
			continue;

		++count;
		if (firstFactory && (*firstFactory == nullptr ||
			object->getID() < (*firstFactory)->getID()))
			*firstFactory = object;
	}
	return count;
}

Int CountSkirmishAIRecoveryBuilderQueueEntries(
	Player *player, const ThingTemplate *builderTemplate, ObjectID *firstFactoryID)
{
	if (firstFactoryID)
		*firstFactoryID = INVALID_ID;
	if (!player || !builderTemplate || !TheGameLogic)
		return 0;

	Int count = 0;
	for (Object *object = TheGameLogic->getFirstObject(); object;
		object = object->getNextObject())
	{
		if (!IsLiveSkirmishAIRecoveryObject(object) ||
			object->getControllingPlayer() != player ||
			!object->getProductionUpdateInterface())
			continue;

		ProductionUpdateInterface *production = object->getProductionUpdateInterface();
		for (const ProductionEntry *entry = production->firstProduction(); entry;
			entry = production->nextProduction(entry))
		{
			if (entry->getProductionType() != PRODUCTION_UNIT ||
				!entry->getProductionObject() ||
				!entry->getProductionObject()->isEquivalentTo(builderTemplate))
				continue;

			++count;
			if (firstFactoryID && *firstFactoryID == INVALID_ID)
				*firstFactoryID = object->getID();
		}
	}
	return count;
}

Int CountSkirmishAIRecoveryBuilderQueueEntriesOnFactory(
	Player *player, const ThingTemplate *builderTemplate, ObjectID factoryID)
{
	if (!player || !builderTemplate || factoryID == INVALID_ID || !TheGameLogic)
		return 0;

	Object *factory = TheGameLogic->findObjectByID(factoryID);
	if (!IsLiveSkirmishAIRecoveryObject(factory) ||
		factory->getControllingPlayer() != player ||
		!factory->getProductionUpdateInterface())
		return 0;

	Int count = 0;
	ProductionUpdateInterface *production = factory->getProductionUpdateInterface();
	for (const ProductionEntry *entry = production->firstProduction(); entry;
		entry = production->nextProduction(entry))
	{
		if (entry->getProductionType() == PRODUCTION_UNIT &&
			entry->getProductionObject() &&
			entry->getProductionObject()->isEquivalentTo(builderTemplate))
			++count;
	}
	return count;
}

Object *FindSkirmishAIRecoverySecondBuilderFactory(Player *player)
{
	if (!player || !s_recovery.builderTemplate || !TheGameLogic ||
		!TheBuildAssistant)
		return nullptr;

	Object *bestFactory = nullptr;
	for (Object *object = TheGameLogic->getFirstObject(); object;
		object = object->getNextObject())
	{
		if (!IsLiveSkirmishAIRecoveryObject(object) ||
			object->getControllingPlayer() != player || !object->isStructure() ||
			object->testStatus(OBJECT_STATUS_UNDER_CONSTRUCTION) ||
			object->testStatus(OBJECT_STATUS_SOLD) ||
			!object->getProductionUpdateInterface() ||
			!TheBuildAssistant->isPossibleToMakeUnit(
				object, s_recovery.builderTemplate) ||
			CountSkirmishAIRecoveryBuilderQueueEntriesOnFactory(
				player, s_recovery.builderTemplate, object->getID()) != 0)
			continue;
		if (!bestFactory || object->getID() < bestFactory->getID())
			bestFactory = object;
	}
	return bestFactory;
}

ProductionID FindSkirmishAIRecoveryBuilderQueueIDOnFactory(
	Player *player, const ThingTemplate *builderTemplate, ObjectID factoryID)
{
	if (!player || !builderTemplate || factoryID == INVALID_ID || !TheGameLogic)
		return PRODUCTIONID_INVALID;

	Object *factory = TheGameLogic->findObjectByID(factoryID);
	if (!IsLiveSkirmishAIRecoveryObject(factory) ||
		factory->getControllingPlayer() != player ||
		!factory->getProductionUpdateInterface())
		return PRODUCTIONID_INVALID;

	ProductionUpdateInterface *production = factory->getProductionUpdateInterface();
	for (const ProductionEntry *entry = production->firstProduction(); entry;
		entry = production->nextProduction(entry))
	{
		if (entry->getProductionType() == PRODUCTION_UNIT &&
			entry->getProductionObject() &&
			entry->getProductionObject()->isEquivalentTo(builderTemplate))
			return entry->getProductionID();
	}
	return PRODUCTIONID_INVALID;
}

const ProductionEntry *FindSkirmishAIRecoveryProductionEntryOnFactory(
	Player *player, const ThingTemplate *builderTemplate, ObjectID factoryID,
	ProductionID productionID)
{
	if (!player || !builderTemplate || factoryID == INVALID_ID ||
		productionID == PRODUCTIONID_INVALID || !TheGameLogic)
		return nullptr;

	Object *factory = TheGameLogic->findObjectByID(factoryID);
	if (!IsLiveSkirmishAIRecoveryObject(factory) ||
		factory->getControllingPlayer() != player ||
		!factory->getProductionUpdateInterface())
		return nullptr;

	ProductionUpdateInterface *production = factory->getProductionUpdateInterface();
	for (const ProductionEntry *entry = production->firstProduction(); entry;
		entry = production->nextProduction(entry))
	{
		if (entry->getProductionID() == productionID &&
			entry->getProductionType() == PRODUCTION_UNIT &&
			entry->getProductionObject() &&
			entry->getProductionObject()->isEquivalentTo(builderTemplate))
			return entry;
	}
	return nullptr;
}

Int CountSkirmishAIRecoveryBuilderQueueEntriesOnFactoryExcept(
	Player *player, const ThingTemplate *builderTemplate, ObjectID factoryID,
	ProductionID excludedProductionID)
{
	if (!player || !builderTemplate || factoryID == INVALID_ID || !TheGameLogic)
		return 0;

	Object *factory = TheGameLogic->findObjectByID(factoryID);
	if (!IsLiveSkirmishAIRecoveryObject(factory) ||
		factory->getControllingPlayer() != player ||
		!factory->getProductionUpdateInterface())
		return 0;

	Int count = 0;
	ProductionUpdateInterface *production = factory->getProductionUpdateInterface();
	for (const ProductionEntry *entry = production->firstProduction(); entry;
		entry = production->nextProduction(entry))
	{
		if (entry->getProductionType() == PRODUCTION_UNIT &&
			entry->getProductionObject() &&
			entry->getProductionObject()->isEquivalentTo(builderTemplate) &&
			entry->getProductionID() != excludedProductionID)
			++count;
	}
	return count;
}

Int CancelSkirmishAIRecoveryBuilderQueueEntriesOnFactory(
	Player *player, const ThingTemplate *builderTemplate, ObjectID factoryID)
{
	if (!player || !builderTemplate || factoryID == INVALID_ID || !TheGameLogic)
		return 0;

	Object *factory = TheGameLogic->findObjectByID(factoryID);
	if (!IsLiveSkirmishAIRecoveryObject(factory) ||
		factory->getControllingPlayer() != player ||
		!factory->getProductionUpdateInterface())
		return 0;

	ProductionUpdateInterface *production = factory->getProductionUpdateInterface();
	Int canceled = 0;
	while (true)
	{
		ProductionID productionID = PRODUCTIONID_INVALID;
		for (const ProductionEntry *entry = production->firstProduction(); entry;
			entry = production->nextProduction(entry))
		{
			if (entry->getProductionType() == PRODUCTION_UNIT &&
				entry->getProductionObject() &&
				entry->getProductionObject()->isEquivalentTo(builderTemplate))
			{
				productionID = entry->getProductionID();
				break;
			}
		}
		if (productionID == PRODUCTIONID_INVALID)
			break;
		production->cancelUnitCreate(productionID);
		++canceled;
	}
	return canceled;
}

Int CountSkirmishAIRecoveryBuildersProducedByFactory(
	Player *player, const ThingTemplate *builderTemplate, ObjectID factoryID)
{
	if (!player || !builderTemplate || factoryID == INVALID_ID || !TheGameLogic)
		return 0;

	Int count = 0;
	for (Object *object = TheGameLogic->getFirstObject(); object;
		object = object->getNextObject())
	{
		if (IsLiveSkirmishAIRecoveryObject(object) &&
			!object->isContained() &&
			object->getControllingPlayer() == player &&
			object->isKindOf(KINDOF_DOZER) &&
			object->getTemplate()->isEquivalentTo(builderTemplate) &&
			object->getProducerID() == factoryID)
			++count;
	}
	return count;
}

ObjectID FindSkirmishAIRecoveryBuilderProducedByFactory(
	Player *player, const ThingTemplate *builderTemplate, ObjectID factoryID)
{
	if (!player || !builderTemplate || factoryID == INVALID_ID || !TheGameLogic)
		return INVALID_ID;

	ObjectID firstID = INVALID_ID;
	for (Object *object = TheGameLogic->getFirstObject(); object;
		object = object->getNextObject())
	{
		if (IsLiveSkirmishAIRecoveryObject(object) &&
			!object->isContained() &&
			object->getControllingPlayer() == player &&
			object->isKindOf(KINDOF_DOZER) &&
			object->getTemplate()->isEquivalentTo(builderTemplate) &&
			object->getProducerID() == factoryID &&
			(firstID == INVALID_ID || object->getID() < firstID))
			firstID = object->getID();
	}
	return firstID;
}

Bool IsSkirmishAIRecoveryIDInList(
	ObjectID objectID, const ObjectID *ids, Int idCount)
{
	if (objectID == INVALID_ID || !ids)
		return FALSE;
	for (Int i = 0; i < idCount; ++i)
	{
		if (ids[i] == objectID)
			return TRUE;
	}
	return FALSE;
}

Bool IsSkirmishAIRecoveryBaselineBuilderID(ObjectID objectID)
{
	return IsSkirmishAIRecoveryIDInList(
		objectID, s_recovery.baselineBuilderIDs, s_recovery.baselineBuilderIDCount);
}

Bool IsSkirmishAIRecoveryBaselineCombatID(ObjectID objectID)
{
	return IsSkirmishAIRecoveryIDInList(
		objectID, s_recovery.baselineCombatIDs, s_recovery.baselineCombatIDCount);
}

Object *FindSkirmishAIRecoveryBaselineBuilder(Player *player)
{
	if (!player || !TheGameLogic)
		return nullptr;

	for (Int i = 0; i < s_recovery.baselineBuilderIDCount; ++i)
	{
		Object *object = TheGameLogic->findObjectByID(s_recovery.baselineBuilderIDs[i]);
		if (IsLiveSkirmishAIRecoveryObject(object) &&
			!object->isContained() &&
			object->getControllingPlayer() == player &&
			object->isKindOf(KINDOF_DOZER) && object->getAIUpdateInterface() &&
			object->getAIUpdateInterface()->getDozerAIInterface())
			return object;
	}
	return nullptr;
}

Bool HasUnexpectedSkirmishAIRecoveryBuilder(Player *player)
{
	if (!player || !TheGameLogic || !s_recovery.builderTemplate)
		return FALSE;

	for (Object *object = TheGameLogic->getFirstObject(); object;
		object = object->getNextObject())
	{
		if (IsLiveSkirmishAIRecoveryObject(object) &&
			!object->isContained() &&
			object->getControllingPlayer() == player &&
			object->isKindOf(KINDOF_DOZER) &&
			object->getTemplate()->isEquivalentTo(s_recovery.builderTemplate) &&
			!IsSkirmishAIRecoveryBaselineBuilderID(object->getID()))
			return TRUE;
	}
	return FALSE;
}

Object *FindSkirmishAIRecoverySpareBaselineBuilder(
	Player *player, ObjectID excludedBuilderID)
{
	if (!player || !TheGameLogic || !s_recovery.builderTemplate)
		return nullptr;

	for (Int i = 0; i < s_recovery.baselineBuilderIDCount; ++i)
	{
		Object *object = TheGameLogic->findObjectByID(s_recovery.baselineBuilderIDs[i]);
		if (!IsLiveSkirmishAIRecoveryObject(object) ||
			object->isContained() ||
			object->getID() == excludedBuilderID ||
			object->getControllingPlayer() != player ||
			!object->isKindOf(KINDOF_DOZER) ||
			object->testStatus(OBJECT_STATUS_SOLD) ||
			object->isDisabledByType(DISABLED_UNMANNED) ||
			!object->getTemplate()->isEquivalentTo(s_recovery.builderTemplate) ||
			!object->getAIUpdateInterface())
			continue;
		if (object->getAIUpdateInterface()->getDozerAIInterface())
			return object;
	}
	return nullptr;
}

Bool IsSkirmishAIRecoveryCombatUnit(const Object *object, Player *player)
{
	return IsLiveSkirmishAIRecoveryObject(object) &&
		object->getControllingPlayer() == player &&
		!object->isContained() &&
		!object->isKindOf(KINDOF_STRUCTURE) &&
		!object->isKindOf(KINDOF_IMMOBILE) &&
		!object->isKindOf(KINDOF_DOZER) &&
		!object->isKindOf(KINDOF_HARVESTER) &&
		!object->isKindOf(KINDOF_PROJECTILE) &&
		!object->isKindOf(KINDOF_MINE) && object->isAbleToAttack();
}

Int CountSkirmishAIRecoveryAttackCapableUnits(Player *player)
{
	if (!player || !TheGameLogic)
		return 0;

	Int count = 0;
	for (Object *object = TheGameLogic->getFirstObject(); object;
		object = object->getNextObject())
	{
		if (IsSkirmishAIRecoveryCombatUnit(object, player))
			++count;
	}
	return count;
}

Object *FindSkirmishAIRecoveryVictoryBuilding(Player *player)
{
	if (!player || !TheGameLogic)
		return nullptr;

	Object *best = nullptr;
	for (Object *object = TheGameLogic->getFirstObject(); object;
		object = object->getNextObject())
	{
		if (!IsLiveSkirmishAIRecoveryObject(object) ||
			object->getControllingPlayer() != player ||
			!object->isKindOf(KINDOF_STRUCTURE) ||
			!object->isKindOf(KINDOF_MP_COUNT_FOR_VICTORY) ||
			object->isKindOf(KINDOF_COMMANDCENTER) ||
			object->testStatus(OBJECT_STATUS_UNDER_CONSTRUCTION) ||
			object->testStatus(OBJECT_STATUS_SOLD))
			continue;
		if (!best || object->getID() < best->getID())
			best = object;
	}
	return best;
}

Bool FindSkirmishAIRecoveryNaturalAttackMove(
	Player *player, ObjectID *unitID, Coord3D *victimPosition)
{
	if (unitID)
		*unitID = INVALID_ID;
	if (victimPosition)
		victimPosition->zero();
	if (!player || !TheGameLogic)
		return FALSE;

	for (Object *object = TheGameLogic->getFirstObject(); object;
		object = object->getNextObject())
	{
		if (!IsSkirmishAIRecoveryCombatUnit(object, player))
			continue;
		AIUpdateInterface *ai = object->getAIUpdateInterface();
		const Coord3D *currentVictimPosition = ai
			? ai->getCurrentVictimPos() : nullptr;
		if (!ai || ai->getLastCommandSource() != CMD_FROM_AI ||
			!ai->isAttackPath() || !currentVictimPosition)
			continue;
		if (unitID)
			*unitID = object->getID();
		if (victimPosition)
			*victimPosition = *currentVictimPosition;
		return TRUE;
	}
	return FALSE;
}

Bool PrepareSkirmishAIRecoveryLastStandBaseline(Player *player)
{
	if (!player || !TheGameLogic)
		return FALSE;

	s_recovery.baselineCombatIDCount = 0;
	for (Int i = 0; i < SKIRMISH_AI_RECOVERY_DIAGNOSTIC_MAX_IDS; ++i)
		s_recovery.baselineCombatIDs[i] = INVALID_ID;

	Object *best = nullptr;
	for (Object *object = TheGameLogic->getFirstObject(); object;
		object = object->getNextObject())
	{
		if (!IsSkirmishAIRecoveryCombatUnit(object, player))
			continue;
		AIUpdateInterface *ai = object->getAIUpdateInterface();
		if (!ai || ai->isAttacking() || ai->isAttackPath())
			continue;
		if (!best || object->getID() < best->getID())
			best = object;
	}
	if (!best)
		return FALSE;
	s_recovery.baselineCombatIDs[0] = best->getID();
	s_recovery.baselineCombatIDCount = 1;
	s_recovery.lastStandBaselineEvidenceUnitID = best->getID();
	return TRUE;
}

void CaptureSkirmishAIRecoveryBaselineIDs(
	Player *player, const ThingTemplate *builderTemplate, ObjectID factoryID)
{
	if (!player || !builderTemplate || !TheGameLogic)
		return;

	s_recovery.baselineBuilderIDCount = 0;
	s_recovery.baselineQueueIDCount = 0;
	for (Int i = 0; i < SKIRMISH_AI_RECOVERY_DIAGNOSTIC_MAX_IDS; ++i)
	{
		s_recovery.baselineBuilderIDs[i] = INVALID_ID;
		s_recovery.baselineQueueIDs[i] = PRODUCTIONID_INVALID;
	}

	for (Object *object = TheGameLogic->getFirstObject(); object;
		object = object->getNextObject())
	{
		if (s_recovery.baselineBuilderIDCount >= SKIRMISH_AI_RECOVERY_DIAGNOSTIC_MAX_IDS)
			break;
		if (IsLiveSkirmishAIRecoveryObject(object) &&
			!object->isContained() &&
			object->getControllingPlayer() == player &&
			object->isKindOf(KINDOF_DOZER) &&
			object->getAIUpdateInterface() &&
			object->getAIUpdateInterface()->getDozerAIInterface() &&
			object->getTemplate()->isEquivalentTo(builderTemplate))
			s_recovery.baselineBuilderIDs[s_recovery.baselineBuilderIDCount++] = object->getID();
	}

	Object *factory = factoryID != INVALID_ID
		? TheGameLogic->findObjectByID(factoryID) : nullptr;
	if (!IsLiveSkirmishAIRecoveryObject(factory) ||
		factory->getControllingPlayer() != player ||
		!factory->getProductionUpdateInterface())
		return;

	ProductionUpdateInterface *production = factory->getProductionUpdateInterface();
	for (const ProductionEntry *entry = production->firstProduction(); entry;
		entry = production->nextProduction(entry))
	{
		if (s_recovery.baselineQueueIDCount >= SKIRMISH_AI_RECOVERY_DIAGNOSTIC_MAX_IDS)
			break;
		if (entry->getProductionType() == PRODUCTION_UNIT &&
			entry->getProductionObject() &&
			entry->getProductionObject()->isEquivalentTo(builderTemplate))
			s_recovery.baselineQueueIDs[s_recovery.baselineQueueIDCount++] =
				entry->getProductionID();
	}
}

void PrintSkirmishAIRecoveryFactoryDiagnostics(Player *player, const char *reason)
{
	if (!player || !TheGameLogic)
		return;

	const Int workerCount = CountSkirmishAIRecoveryBuilders(player, nullptr);
	const Int queueCount = CountSkirmishAIRecoveryBuilderQueueEntriesOnFactory(
		player, s_recovery.builderTemplate, s_recovery.builderFactoryID);
	printf("SKIRMISH_AI_RECOVERY_DIAGNOSTIC reason=%s frame=%u factory=%u "
		"recovery_production_id=%d queue_count=%d worker_count=%d initial_factory_workers=%d "
		"pre_fault_queue=%d pre_fault_factory_queue=%d\n",
		reason ? reason : "unknown", TheGameLogic->getFrame(),
		s_recovery.builderFactoryID,
		static_cast<Int>(s_recovery.recoveryBuilderProductionID), queueCount, workerCount,
		s_recovery.initialFactoryBuilderCount, s_recovery.preFaultBuilderQueueCount,
		s_recovery.preFaultFactoryBuilderQueueCount);

	printf("SKIRMISH_AI_RECOVERY_DIAGNOSTIC baseline_builder_ids=");
	for (Int i = 0; i < s_recovery.baselineBuilderIDCount; ++i)
		printf("%s%u", i == 0 ? "" : ",", s_recovery.baselineBuilderIDs[i]);
	printf(" baseline_queue_ids=");
	Int diagnosticIndex;
	for (diagnosticIndex = 0; diagnosticIndex < s_recovery.baselineQueueIDCount; ++diagnosticIndex)
		printf("%s%d", diagnosticIndex == 0 ? "" : ",",
			static_cast<Int>(s_recovery.baselineQueueIDs[diagnosticIndex]));
	printf("\n");

	Object *factory = s_recovery.builderFactoryID != INVALID_ID
		? TheGameLogic->findObjectByID(s_recovery.builderFactoryID) : nullptr;
	if (IsLiveSkirmishAIRecoveryObject(factory) &&
		factory->getControllingPlayer() == player &&
		factory->getProductionUpdateInterface())
	{
		ProductionUpdateInterface *production = factory->getProductionUpdateInterface();
		for (const ProductionEntry *entry = production->firstProduction(); entry;
			entry = production->nextProduction(entry))
		{
			const ThingTemplate *unit = entry->getProductionType() == PRODUCTION_UNIT
				? entry->getProductionObject() : nullptr;
			printf("SKIRMISH_AI_RECOVERY_DIAGNOSTIC queue_entry type=%d production_id=%d "
				"unit=%s quantity_remaining=%d percent=%g\n",
				entry->getProductionType(), static_cast<Int>(entry->getProductionID()),
				unit ? unit->getName().str() : "<upgrade>",
				entry->getProductionQuantityRemaining(), entry->getPercentComplete());
		}
	}

	for (Object *object = TheGameLogic->getFirstObject(); object;
		object = object->getNextObject())
	{
		if (!IsLiveSkirmishAIRecoveryObject(object) ||
			object->isContained() ||
			object->getControllingPlayer() != player ||
			!object->isKindOf(KINDOF_DOZER) ||
			!s_recovery.builderTemplate ||
			!object->getTemplate()->isEquivalentTo(s_recovery.builderTemplate))
			continue;
		AIUpdateInterface *ai = object->getAIUpdateInterface();
		DozerAIInterface *dozerAI = ai ? ai->getDozerAIInterface() : nullptr;
		printf("SKIRMISH_AI_RECOVERY_DIAGNOSTIC worker id=%u template=%s producer=%u "
			"current_task=%d recent_task=%d pending=%d attacking=%d attack_path=%d\n",
			object->getID(), object->getTemplate()->getName().str(), object->getProducerID(),
			dozerAI ? dozerAI->getCurrentTask() : DOZER_TASK_INVALID,
			dozerAI ? dozerAI->getMostRecentCommand() : DOZER_TASK_INVALID,
			dozerAI ? dozerAI->isAnyTaskPending() : FALSE,
			ai ? ai->isAttacking() : FALSE, ai ? ai->isAttackPath() : FALSE);
	}
}

void PrintSkirmishAIRecoveryScaffoldDiagnostics(
	Player *player, Object *commandCenter, const char *phase)
{
	ObjectID centerID = commandCenter
		? commandCenter->getID() : s_recovery.currentConstructionID;
	Real constructionPercent = commandCenter
		? commandCenter->getConstructionPercent() : s_recovery.lastConstructionPercent;
	ObjectID builderID = commandCenter
		? commandCenter->getBuilderID() : s_recovery.lastConstructionBuilderID;
	Object *builder = builderID != INVALID_ID && TheGameLogic
		? TheGameLogic->findObjectByID(builderID) : nullptr;
	const Bool builderLive = IsLiveSkirmishAIRecoveryObject(builder) &&
		!builder->isContained();
	const Bool builderOwner = builderLive && player &&
		builder->getControllingPlayer() == player;
	AIUpdateInterface *ai = builderLive ? builder->getAIUpdateInterface() : nullptr;
	DozerAIInterface *dozerAI = ai ? ai->getDozerAIInterface() : nullptr;
	const DozerTask currentTask = dozerAI
		? dozerAI->getCurrentTask() : DOZER_TASK_INVALID;
	const DozerTask recentCommand = dozerAI
		? dozerAI->getMostRecentCommand() : DOZER_TASK_INVALID;
	const Bool buildPending = dozerAI
		? dozerAI->isTaskPending(DOZER_TASK_BUILD) : FALSE;
	const ObjectID buildTarget = dozerAI
		? dozerAI->getTaskTarget(DOZER_TASK_BUILD) : INVALID_ID;
	Coord3D builderPosition;
	builderPosition.zero();
	if (builderLive && builder->getPosition())
		builderPosition = *builder->getPosition();
	Coord3D goalPosition;
	goalPosition.zero();
	const Coord3D *goal = ai ? ai->getGoalPosition() : nullptr;
	if (goal)
		goalPosition = *goal;
	printf("SKIRMISH_AI_RECOVERY_DIAGNOSTIC phase=%s frame=%u center=%u "
		"construction_percent=%g center_builder=%u builder_live=%d "
		"builder_owner=%d current_task=%d recent_command=%d build_pending=%d "
		"build_target=%u builder_pos=(%g,%g,%g) goal=(%g,%g,%g) cash=%u\n",
		phase ? phase : "unknown", TheGameLogic ? TheGameLogic->getFrame() : 0,
		centerID, constructionPercent, builderID, builderLive, builderOwner,
		static_cast<Int>(currentTask), static_cast<Int>(recentCommand),
		buildPending, buildTarget, builderPosition.x, builderPosition.y,
		builderPosition.z, goalPosition.x, goalPosition.y, goalPosition.z,
		player && player->getMoney() ? player->getMoney()->countMoney() : 0);
	fflush(stdout);
}

void MaybePrintSkirmishAIRecoveryFactoryDiagnostics(Player *player)
{
	if (!player || !IsSkirmishAIRecoveryFactoryFixture())
		return;
	const Int workerCount = CountSkirmishAIRecoveryBuilders(player, nullptr);
	const Int queueCount = CountSkirmishAIRecoveryBuilderQueueEntriesOnFactory(
		player, s_recovery.builderTemplate, s_recovery.builderFactoryID);
	if (workerCount == s_recovery.lastDiagnosticBuilderCount &&
		queueCount == s_recovery.lastDiagnosticQueueCount)
		return;
	s_recovery.lastDiagnosticBuilderCount = workerCount;
	s_recovery.lastDiagnosticQueueCount = queueCount;
	PrintSkirmishAIRecoveryFactoryDiagnostics(player, "count_change");
}

Int CountSkirmishAIRecoveryLastStandUnits(Player *player, Bool *huntEvidence)
{
	if (huntEvidence)
		*huntEvidence = FALSE;
	if (!player || !TheGameLogic)
		return 0;

	Int count = 0;
	for (Object *object = TheGameLogic->getFirstObject(); object;
		object = object->getNextObject())
	{
		if (!IsSkirmishAIRecoveryCombatUnit(object, player))
			continue;
		if (s_recovery.baselineCaptured &&
			s_recovery.fixtureCase == SKIRMISH_AI_RECOVERY_NO_PATH_LASTSTAND &&
			!IsSkirmishAIRecoveryBaselineCombatID(object->getID()))
			continue;

		++count;
		AIUpdateInterface *ai = object->getAIUpdateInterface();
		const Coord3D *goalPosition = ai ? ai->getGoalPosition() : nullptr;
		const Bool attackMoveOrder = ai &&
			ai->getLastCommandSource() == CMD_FROM_AI &&
			ai->getCurrentStateID() == AI_ATTACK_MOVE_TO && goalPosition != nullptr;
		// Product recovery issues aiAttackMoveToPosition, which enters the
		// AI_ATTACK_MOVE_TO state and sets a goal position. Generic attack flags
		// and current-victim state are not sufficient evidence here.
		if (huntEvidence && attackMoveOrder)
			*huntEvidence = TRUE;
	}
	return count;
}

void PrintSkirmishAIRecoveryLastStandEvidenceDiagnostics(Player *player)
{
	if (!player || !TheGameLogic)
		return;
	printf("SKIRMISH_AI_RECOVERY_DIAGNOSTIC phase=last_stand_evidence "
		"frame=%u baseline_units=%d\n", TheGameLogic->getFrame(),
		s_recovery.baselineCombatIDCount);
	for (Int i = 0; i < s_recovery.baselineCombatIDCount; ++i)
	{
		Object *object = TheGameLogic->findObjectByID(s_recovery.baselineCombatIDs[i]);
		const Bool live = IsLiveSkirmishAIRecoveryObject(object);
		AIUpdateInterface *ai = live ? object->getAIUpdateInterface() : nullptr;
		const Coord3D *goalPosition = ai ? ai->getGoalPosition() : nullptr;
		printf("SKIRMISH_AI_RECOVERY_DIAGNOSTIC last_stand_unit id=%u live=%d "
			"source=%d state=%d goal=(%g,%g,%g) goal_present=%d "
			"attacking=%d attack_path=%d victim_pos=%d contained=%d\n",
			s_recovery.baselineCombatIDs[i], live,
			ai ? static_cast<Int>(ai->getLastCommandSource()) : -1,
			ai ? static_cast<Int>(ai->getCurrentStateID()) : -1,
			goalPosition ? goalPosition->x : 0.0f,
			goalPosition ? goalPosition->y : 0.0f,
			goalPosition ? goalPosition->z : 0.0f, goalPosition != nullptr,
			ai ? ai->isAttacking() : FALSE, ai ? ai->isAttackPath() : FALSE,
			ai ? ai->getCurrentVictimPos() != nullptr : FALSE,
			object ? object->isContained() : FALSE);
	}
	fflush(stdout);
}

Object *FindSkirmishAIRecoveryHole(
	Player *player, ObjectID spawnerID, Int *holeCount)
{
	if (holeCount)
		*holeCount = 0;
	if (!player || !TheGameLogic)
		return nullptr;

	Object *firstHole = nullptr;
	for (Object *object = TheGameLogic->getFirstObject(); object;
		object = object->getNextObject())
	{
		if (!IsLiveSkirmishAIRecoveryObject(object) ||
			object->getControllingPlayer() != player ||
			!object->isKindOf(KINDOF_REBUILD_HOLE))
			continue;

		RebuildHoleBehaviorInterface *holeBehavior =
			RebuildHoleBehavior::getRebuildHoleBehaviorInterfaceFromObject(object);
		if (!holeBehavior || holeBehavior->getSpawnerID() != spawnerID)
			continue;
		if (holeCount)
			++*holeCount;
		if (!firstHole)
			firstHole = object;
	}
	return firstHole;
}

Bool ObserveSkirmishAIRecoveryHoleLineage(Object *commandCenter)
{
	if (s_recovery.fixtureCase != SKIRMISH_AI_RECOVERY_GLA_HOLE ||
		!commandCenter || commandCenter->getID() == s_recovery.initialCenterID)
		return TRUE;
	if (!s_recovery.holeObserved || s_recovery.holeID == INVALID_ID)
	{
		FailSkirmishAITest("fixture_gla_hole_lineage_mismatch");
		return FALSE;
	}
	if (s_recovery.holeLineageObserved)
	{
		if (commandCenter->getID() != s_recovery.holeReconstructionID)
		{
			FailSkirmishAITest("fixture_gla_hole_reconstruction_replaced");
			return FALSE;
		}
		return TRUE;
	}
	Object *hole = TheGameLogic->findObjectByID(s_recovery.holeID);
	RebuildHoleBehaviorInterface *holeBehavior =
		IsLiveSkirmishAIRecoveryObject(hole)
			? RebuildHoleBehavior::getRebuildHoleBehaviorInterfaceFromObject(hole)
			: nullptr;
	if (!holeBehavior || hole->getControllingPlayer() !=
			commandCenter->getControllingPlayer() ||
		holeBehavior->getSpawnerID() != s_recovery.initialCenterID ||
		holeBehavior->getReconstructedBuildingID() != commandCenter->getID() ||
		!holeBehavior->getRebuildTemplate() || !s_recovery.primaryTemplate ||
		!holeBehavior->getRebuildTemplate()->isEquivalentTo(s_recovery.primaryTemplate))
		return TRUE;

	s_recovery.holeLineageObserved = TRUE;
	s_recovery.holeReconstructionID = commandCenter->getID();
	printf("SKIRMISH_AI_RECOVERY_HOLE_PHASE phase=hole_lineage_verified frame=%u "
		"spawner=%u hole=%u reconstruction=%u producer=%u\n",
		TheGameLogic->getFrame(), s_recovery.initialCenterID, s_recovery.holeID,
		s_recovery.holeReconstructionID, commandCenter->getProducerID());
	fflush(stdout);
	return TRUE;
}

Object *FindSkirmishAIRecoveryObstruction(Player *player, Object *center)
{
	if (!player || !TheGameLogic)
		return nullptr;

	for (Object *object = TheGameLogic->getFirstObject(); object;
		object = object->getNextObject())
	{
		if (IsLiveSkirmishAIRecoveryObject(object) && object != center &&
			object->getControllingPlayer() == player && object->isStructure() &&
			object->isKindOf(KINDOF_IMMOBILE) &&
			!object->isKindOf(KINDOF_COMMANDCENTER) &&
			!object->testStatus(OBJECT_STATUS_UNDER_CONSTRUCTION) &&
			!object->testStatus(OBJECT_STATUS_SOLD))
			return object;
	}
	return nullptr;
}

void SetSkirmishAIRecoveryCash(Player *player, UnsignedInt amount)
{
	if (!player || !player->getMoney())
		return;
	Money *money = player->getMoney();
	money->withdraw(money->countMoney(), FALSE);
	if (amount > 0)
		money->deposit(amount, FALSE, FALSE);
}

void MoveSkirmishAIRecoveryObject(Object *object, const Coord3D *position)
{
	if (!object || !position)
		return;

#if RTS_ZEROHOUR
	if (TheAI && TheAI->pathfinder())
		TheAI->pathfinder()->removeObjectFromPathfindMap(object);
#endif
	object->setPosition(position);
#if RTS_ZEROHOUR
	if (TheAI && TheAI->pathfinder())
		TheAI->pathfinder()->addObjectToPathfindMap(object);
#endif
	object->handlePartitionCellMaintenance();
	if (ThePartitionManager)
		ThePartitionManager->update();
}

void DestroySkirmishAIRecoveryObject(Object *object)
{
	if (object && IsLiveSkirmishAIRecoveryObject(object) && TheGameLogic)
		TheGameLogic->destroyObject(object);
}

void DestroySkirmishAIRecoveryBuilders(Player *player)
{
	if (!player || !TheGameLogic)
		return;
	for (Object *object = TheGameLogic->getFirstObject(); object; )
	{
		Object *next = object->getNextObject();
		if (IsLiveSkirmishAIRecoveryObject(object) &&
			object->getControllingPlayer() == player && object->isKindOf(KINDOF_DOZER))
			DestroySkirmishAIRecoveryObject(object);
		object = next;
	}
}

void DestroySkirmishAIRecoveryBuilderFactories(
	Player *player, const ThingTemplate *builderTemplate)
{
	if (!player || !builderTemplate || !TheGameLogic || !TheBuildAssistant)
		return;
	for (Object *object = TheGameLogic->getFirstObject(); object; )
	{
		Object *next = object->getNextObject();
		if (IsLiveSkirmishAIRecoveryObject(object) &&
			object->getControllingPlayer() == player && object->isStructure() &&
			object->getProductionUpdateInterface() &&
			TheBuildAssistant->isPossibleToMakeUnit(object, builderTemplate))
			DestroySkirmishAIRecoveryObject(object);
		object = next;
	}
}

Bool IsSkirmishAIRecoveryFixtureSetupValid()
{
	if (!TheGameLogic || !TheGameInfo || !ThePlayerList ||
		!TheGameLogic->isInSkirmishGame())
		return FALSE;

	const GameSlot *observerSlot = TheGameInfo->getConstSlot(0);
	Player *observer = ThePlayerList->getPlayerFromSlotIndex(0);
	Player *fixturePlayer = GetSkirmishAIRecoveryFixturePlayer();
	const GameSlot *fixtureSlot = TheGameInfo->getConstSlot(1);
	const PlayerTemplate *selectedTemplate = ThePlayerTemplateStore
		? ThePlayerTemplateStore->getNthPlayerTemplate(s_recovery.templateIndex) : nullptr;
	return observerSlot && observerSlot->isHuman() &&
		observerSlot->getOriginalPlayerTemplate() == PLAYERTEMPLATE_OBSERVER &&
		observer && observer->isPlayerObserver() && fixtureSlot && fixtureSlot->isAI() &&
		fixturePlayer && fixturePlayer->getPlayerType() == PLAYER_COMPUTER &&
		fixturePlayer->isSkirmishAIPlayer() && fixturePlayer->getPlayerTemplate() &&
		selectedTemplate && fixturePlayer->getSide().compareNoCase(
			selectedTemplate->getSide().str()) == 0 &&
		fixtureSlot->getOriginalPlayerTemplate() == s_recovery.templateIndex &&
		selectedTemplate->getName().compareNoCase(
			GetSkirmishAIRecoveryFactionTemplateName(s_recovery.faction)) == 0;
}

Bool IsDifferentSkirmishAIRecoveryPosition(const Coord3D &first, const Coord3D &second)
{
	Real dx = first.x - second.x;
	Real dy = first.y - second.y;
	Real dz = first.z - second.z;
	return dx * dx + dy * dy + dz * dz > 1.0f;
}

Bool IsSkirmishAIRecoveryExpectedMapLeaf(const char *actual, const char *expected)
{
	if (!actual || !expected)
		return FALSE;
	const char *actualLeaf = strrchr(actual, '\\');
	const char *slashLeaf = strrchr(actual, '/');
	if (!actualLeaf || (slashLeaf && slashLeaf > actualLeaf))
		actualLeaf = slashLeaf;
	if (actualLeaf)
		++actualLeaf;
	else
		actualLeaf = actual;
	const char *expectedLeaf = strrchr(expected, '\\');
	if (!expectedLeaf)
		expectedLeaf = strrchr(expected, '/');
	if (expectedLeaf)
		++expectedLeaf;
	else
		expectedLeaf = expected;
	return _stricmp(actualLeaf, expectedLeaf) == 0;
}

Bool IsExpectedSkirmishAIRecoveryLoadedState(
	const SkirmishAITestPlan &plan, UnsignedInt expectedMapCRC,
	UnsignedInt expectedMapSize, const SkirmishAITestLoadedState *loadedState)
{
	if (IsExpectedSkirmishAITestLoadedState(
			plan, expectedMapCRC, expectedMapSize, loadedState))
		return TRUE;
	if ((!s_recovery.saveLoadIssued && !s_allied.saveLoaded) || !loadedState || plan.mapName == nullptr ||
		loadedState->gameInfoMapName == nullptr || loadedState->globalMapName == nullptr ||
		loadedState->terrainMapName == nullptr || loadedState->mapCRC != expectedMapCRC ||
		loadedState->mapSize != expectedMapSize || loadedState->seed != plan.seed)
		return FALSE;
	return IsSkirmishAIRecoveryExpectedMapLeaf(loadedState->gameInfoMapName, plan.mapName) &&
		IsSkirmishAIRecoveryExpectedMapLeaf(loadedState->globalMapName, plan.mapName) &&
		IsSkirmishAIRecoveryExpectedMapLeaf(loadedState->terrainMapName, plan.mapName);
}


void UpdateSkirmishAIRecoveryFixture();
}

static Bool TryParseSkirmishAIRecoveryNamedValue(
	const char *text, Int *value, const char *const *names, Int nameCount)
{
	if (!text || !value || !names)
		return FALSE;
	for (Int i = 0; i < nameCount; ++i)
	{
		if (_stricmp(text, names[i]) == 0)
		{
			*value = i;
			return TRUE;
		}
	}
	return FALSE;
}

Bool TryParseSkirmishAIAlliedFixtureCase(const char *text, Int *fixtureCase)
{
	return TryParseSkirmishAIRecoveryNamedValue(text, fixtureCase,
		s_alliedCaseNames, ARRAY_SIZE(s_alliedCaseNames));
}

Bool ConfigureSkirmishAIAlliedFixture(Int fixtureCase)
{
#if !RTS_ZEROHOUR || !defined(_WIN64)
	return FALSE;
#else
	if (s_alliedRequestedCase >= 0 || fixtureCase < 0 ||
		fixtureCase >= SKIRMISH_AI_ALLIED_FIXTURE_CASE_COUNT)
		return FALSE;
	s_alliedRequestedCase = fixtureCase;
	return TRUE;
#endif
}

Bool IsSkirmishAIAlliedFixtureActive()
{
#if RTS_ZEROHOUR && defined(_WIN64)
	return s_allied.active;
#else
	return FALSE;
#endif
}

Bool TryParseSkirmishAIRecoveryFixtureCase(const char *text, Int *fixtureCase)
{
	return TryParseSkirmishAIRecoveryNamedValue(
		text, fixtureCase, g_skirmishAIRecoveryFixtureCaseNames,
		ARRAY_SIZE(g_skirmishAIRecoveryFixtureCaseNames));
}

Bool TryParseSkirmishAIRecoveryFaction(const char *text, Int *faction)
{
	return TryParseSkirmishAIRecoveryNamedValue(
		text, faction, g_skirmishAIRecoveryFactionNames,
		ARRAY_SIZE(g_skirmishAIRecoveryFactionNames));
}

Bool IsSupportedSkirmishAIRecoveryFixtureCombination(Int fixtureCase, Int faction)
{
	if (fixtureCase < 0 || fixtureCase >= SKIRMISH_AI_RECOVERY_FIXTURE_CASE_COUNT ||
		faction < 0 || faction >= SKIRMISH_AI_RECOVERY_FACTION_COUNT)
		return FALSE;

	// Stock USA/China do not expose an alternate dozer-producing factory. The
	// factory-only and die-module hole fixtures therefore require a GLA side.
	if ((fixtureCase == SKIRMISH_AI_RECOVERY_FACTORY_ONLY ||
			fixtureCase == SKIRMISH_AI_RECOVERY_DISABLED_FACTORY ||
			fixtureCase == SKIRMISH_AI_RECOVERY_GLA_HOLE) &&
		faction < SKIRMISH_AI_RECOVERY_FACTION_GLA)
		return FALSE;

	return TRUE;
}

const char *GetSkirmishAIRecoveryFixtureCaseName(Int fixtureCase)
{
	if (fixtureCase < 0 || fixtureCase >= SKIRMISH_AI_RECOVERY_FIXTURE_CASE_COUNT)
		return "invalid";
	return g_skirmishAIRecoveryFixtureCaseNames[fixtureCase];
}

const char *GetSkirmishAIRecoveryFactionName(Int faction)
{
	if (faction < 0 || faction >= SKIRMISH_AI_RECOVERY_FACTION_COUNT)
		return "invalid";
	return g_skirmishAIRecoveryFactionNames[faction];
}

const char *GetSkirmishAIRecoveryFactionTemplateName(Int faction)
{
	return GetSkirmishAIRecoveryFactionName(faction);
}



Bool IsValidSkirmishAITestPracticalControllerPlan(
	const SkirmishAITestPlan &plan)
{
	const SkirmishAITestSlotPlan &controller = plan.slots[0];
	if (controller.state != SLOT_PLAYER ||
		controller.playerTemplate == PLAYERTEMPLATE_OBSERVER ||
		!controller.isController || controller.color != 0 ||
		controller.startPosition != 0 || controller.teamNumber != 0)
	{
		return FALSE;
	}
	Int aiCount = 0;
	Int teamCounts[2] = { 0, 0 };
	for (Int slotIndex = 1; slotIndex < SKIRMISH_AI_TEST_SLOT_COUNT; ++slotIndex)
	{
		const SkirmishAITestSlotPlan &slot = plan.slots[slotIndex];
		if (slot.state != SLOT_BRUTAL_AI || slot.isController ||
			slot.playerTemplate != PLAYERTEMPLATE_RANDOM ||
			slot.color != slotIndex || slot.startPosition != slotIndex ||
			slot.teamNumber != (slotIndex <= 3 ? 0 : 1))
		{
			return FALSE;
		}
		++aiCount;
		++teamCounts[slot.teamNumber];
	}
	return aiCount == 7 && controller.teamNumber == 0 &&
		teamCounts[0] == 3 && teamCounts[1] == 4;
}

Bool IsValidSkirmishAITestReplayReceipt(
	const SkirmishAITestReplayReceipt &receipt, Int expectedReplayEpoch)
{
	if (expectedReplayEpoch <= 0 || receipt.seed <= 0 ||
		(receipt.winnerTeam != 0 && receipt.winnerTeam != 1) ||
		receipt.endFrame == 0 || receipt.replayEpoch != expectedReplayEpoch ||
		!HasBoundedString(receipt.scenario,
			SKIRMISH_AI_TEST_RECEIPT_SCENARIO_LENGTH) ||
		!HasBoundedString(receipt.executableSha256,
			SKIRMISH_AI_TEST_RECEIPT_SHA256_LENGTH + 1) ||
		!HasBoundedString(receipt.replaySha256,
			SKIRMISH_AI_TEST_RECEIPT_SHA256_LENGTH + 1) ||
		!HasBoundedString(receipt.runNonce,
			SKIRMISH_AI_TEST_RECEIPT_NONCE_LENGTH + 1) ||
		!HasBoundedString(receipt.replayPath,
			SKIRMISH_AI_TEST_RECEIPT_PATH_LENGTH))
	{
		return FALSE;
	}
	if (strcmp(receipt.scenario, "4v3") != 0 &&
		strcmp(receipt.scenario, "4v2") != 0 &&
		strcmp(receipt.scenario, "practical-1v7") != 0 &&
		strcmp(receipt.scenario, "hard-ai-2v6") != 0)
	{
		return FALSE;
	}
	if (!IsHexString(receipt.executableSha256,
			SKIRMISH_AI_TEST_RECEIPT_SHA256_LENGTH) ||
		!IsHexString(receipt.replaySha256,
			SKIRMISH_AI_TEST_RECEIPT_SHA256_LENGTH) ||
		strlen(receipt.runNonce) == 0)
	{
		return FALSE;
	}
	for (size_t index = 0; receipt.runNonce[index] != '\0'; ++index)
	{
		const char value = receipt.runNonce[index];
		if (!((value >= '0' && value <= '9') ||
			(value >= 'A' && value <= 'F') ||
			(value >= 'a' && value <= 'f') || value == '-'))
		{
			return FALSE;
		}
	}
	return receipt.replayPath[0] != '\0';
}

Bool TryParseSkirmishAITestSeed(const char *text, Int *seed)
{
	if (text == nullptr || text[0] == '\0' || seed == nullptr)
		return FALSE;

	errno = 0;
	char *end = nullptr;
	const long value = strtol(text, &end, 10);
	if (errno == ERANGE || end == text || *end != '\0' || value <= 0 || value > INT_MAX)
		return FALSE;

	*seed = static_cast<Int>(value);
	return TRUE;
}

Bool SetSkirmishAITestExecutableHashInput(const char *sha256)
{
	if (sha256 == nullptr || strlen(sha256) != 64)
		return FALSE;
	for (Int index = 0; index < 64; ++index)
	{
		if (!IsHexDigit(sha256[index]))
			return FALSE;
	}
	strlcpy(s_executableHashInput, sha256, ARRAY_SIZE(s_executableHashInput));
	return TRUE;
}

Bool SetSkirmishAITestSimulationModeInput(const char *mode)
{
	if (mode == nullptr ||
		(strcmp(mode, "serial") != 0 && strcmp(mode, "parallel") != 0 &&
			strcmp(mode, "shadow") != 0))
	{
		return FALSE;
	}
	strlcpy(s_simulationModeInput, mode, ARRAY_SIZE(s_simulationModeInput));
	return TRUE;
}

void SetSkirmishAITestFinalDigest(UnsignedInt digest)
{
	s_finalDigest = digest;
	s_finalDigestAvailable = TRUE;
}

Bool ShouldBypassFramePacingForSkirmishAITest(Bool runnerArmed)
{
	if (!runnerArmed)
		return FALSE;
	// Automated headless gates run without pacing. The practical controller
	// lane keeps normal presentation/input cadence so it remains playable.
	return !s_runner.armed ||
		!IsSkirmishAITestPracticalControllerScenario(s_runner.scenario);
}

void BuildSkirmishAITestPlan(Int seed, SkirmishAITestPlan *plan)
{
	BuildSkirmishAITestPlan(seed, SKIRMISH_AI_TEST_SCENARIO_4V3, plan);
}

void BuildSkirmishAITestPlan(Int seed, SkirmishAITestScenario scenario,
	SkirmishAITestPlan *plan)
{
	if (plan == nullptr)
		return;
	if (scenario != SKIRMISH_AI_TEST_SCENARIO_4V2 &&
		scenario != SKIRMISH_AI_TEST_SCENARIO_PRACTICAL_1V7 &&
		!IsSkirmishAITestHardAI2v6(scenario))
		scenario = SKIRMISH_AI_TEST_SCENARIO_4V3;

	plan->seed = seed;
	plan->mapName = "Maps\\Twilight Flame\\Twilight Flame.map";

	const Bool hardAI2v6 = IsSkirmishAITestHardAI2v6(scenario);
	SkirmishAITestSlotPlan &localSlot = plan->slots[0];
	localSlot.state = hardAI2v6 ? SLOT_BRUTAL_AI : SLOT_PLAYER;
	localSlot.isController = IsSkirmishAITestPracticalControllerScenario(scenario);
	if (hardAI2v6)
	{
		localSlot.playerTemplate = PLAYERTEMPLATE_RANDOM;
		localSlot.color = 0;
		localSlot.startPosition = 0;
		localSlot.teamNumber = 0;
	}
	else if (localSlot.isController)
	{
		localSlot.playerTemplate = PLAYERTEMPLATE_RANDOM;
		localSlot.color = 0;
		localSlot.startPosition = 0;
		localSlot.teamNumber = 0;
	}
	else
	{
		localSlot.playerTemplate = PLAYERTEMPLATE_OBSERVER;
		localSlot.color = -1;
		localSlot.startPosition = -1;
		localSlot.teamNumber = -1;
	}

	for (Int i = 1; i < SKIRMISH_AI_TEST_SLOT_COUNT; ++i)
	{
		SkirmishAITestSlotPlan &slot = plan->slots[i];
		slot.isController = FALSE;
		if (IsSkirmishAITest4v2(scenario) && i == 7)
		{
			slot.state = SLOT_CLOSED;
			slot.playerTemplate = -1;
			slot.color = -1;
			slot.startPosition = -1;
			slot.teamNumber = -1;
			continue;
		}
		slot.state = SLOT_BRUTAL_AI;
		slot.playerTemplate = PLAYERTEMPLATE_RANDOM;
		if (hardAI2v6)
		{
			slot.color = i;
			slot.startPosition = i;
			slot.teamNumber = i < 2 ? 0 : 1;
		}
		else if (IsSkirmishAITestPracticalControllerScenario(scenario))
		{
			slot.color = i;
			slot.startPosition = i;
			slot.teamNumber = i <= 3 ? 0 : 1;
		}
		else
		{
			slot.color = i - 1;
			slot.startPosition = i - 1;
			slot.teamNumber = i <= 4 ? 0 : 1;
		}
	}
}

Bool IsExpectedSkirmishAITestLoadedState(const SkirmishAITestPlan &plan,
	UnsignedInt expectedMapCRC, UnsignedInt expectedMapSize,
	const SkirmishAITestLoadedState *loadedState)
{
	if (plan.mapName == nullptr || loadedState == nullptr ||
		loadedState->gameInfoMapName == nullptr || loadedState->globalMapName == nullptr ||
		loadedState->terrainMapName == nullptr)
	{
		return FALSE;
	}

	return _stricmp(loadedState->gameInfoMapName, plan.mapName) == 0 &&
		_stricmp(loadedState->globalMapName, plan.mapName) == 0 &&
		_stricmp(loadedState->terrainMapName, plan.mapName) == 0 &&
		loadedState->mapCRC == expectedMapCRC &&
		loadedState->mapSize == expectedMapSize &&
		loadedState->seed == plan.seed;
}

Bool IsValidSkirmishAITestReplayResult(UnsignedInt expectedFrameCount,
	UnsignedInt actualFrameCount, Bool desyncGame, Bool quitEarly,
	time_t startTime, time_t endTime)
{
	// VictoryConditions records the winning frame before GameLogic advances to
	// the frame written by RecorderClass::logGameEnd().
	return expectedFrameCount != 0 && expectedFrameCount < UINT_MAX &&
		actualFrameCount == expectedFrameCount + 1U &&
		!desyncGame && !quitEarly && startTime > 0 && endTime >= startTime;
}

SkirmishAITestProgress EvaluateSkirmishAITestProgress(UnsignedInt endFrame, UnsignedInt currentFrame)
{
	if (endFrame != 0)
		return SKIRMISH_AI_TEST_COMPLETE;
	if (currentFrame >= SKIRMISH_AI_TEST_MAX_FRAME)
		return SKIRMISH_AI_TEST_TIMED_OUT;
	return SKIRMISH_AI_TEST_RUNNING;
}

Bool IsSkirmishAITestShutdownTimedOut(UnsignedInt elapsedMilliseconds)
{
	return elapsedMilliseconds >= SKIRMISH_AI_TEST_MAX_SHUTDOWN_MILLISECONDS;
}

Bool IsSkirmishAITestStartupTimedOut(UnsignedInt elapsedMilliseconds)
{
	return elapsedMilliseconds >= SKIRMISH_AI_TEST_MAX_STARTUP_MILLISECONDS;
}

Bool IsSkirmishAITestProgressStalled(UnsignedInt elapsedMilliseconds)
{
	return elapsedMilliseconds >= SKIRMISH_AI_TEST_MAX_STALLED_MILLISECONDS;
}

void ArmSkirmishAITestRunner(Int seed, SkirmishAITestScenario scenario)
{
	s_recovery.reset();
	s_allied = AlliedFixtureState();
	s_allied.active = s_alliedRequestedCase >= 0;
	s_allied.fixtureCase = s_alliedRequestedCase;
	if (s_allied.active && s_allied.fixtureCase == SKIRMISH_AI_ALLIED_TRANSFER_COMMAND)
		scenario = SKIRMISH_AI_TEST_SCENARIO_PRACTICAL_1V7;
	if (scenario != SKIRMISH_AI_TEST_SCENARIO_4V2 &&
		scenario != SKIRMISH_AI_TEST_SCENARIO_PRACTICAL_1V7 &&
		!IsSkirmishAITestHardAI2v6(scenario))
		scenario = SKIRMISH_AI_TEST_SCENARIO_4V3;
	s_runner.armed = TRUE;
	s_runner.started = FALSE;
	s_runner.ending = FALSE;
	s_runner.finished = FALSE;
	s_runner.failed = FALSE;
	s_runner.seed = seed;
	s_runner.winnerTeam = -1;
	s_runner.endFrame = 0;
	s_runner.startupStartMilliseconds = 0;
	s_runner.lastObservedFrame = UINT_MAX;
	s_runner.stalledStartMilliseconds = 0;
	s_runner.shutdownStartMilliseconds = 0;
	s_runner.replayFileName[0] = '\0';
	s_runner.failureReason = nullptr;
	s_runner.expectedMapCRC = 0;
	s_runner.expectedMapSize = 0;
	s_runner.loadedStateValidated = FALSE;
	s_runner.loadedMapName[0] = '\0';
	s_runner.loadedMapCRC = 0;
	s_runner.loadedMapSize = 0;
	s_runner.loadedSeed = 0;
	s_runner.scenario = scenario;
	s_runner.actualAiCount = 0;
	s_runner.actualTeamCounts[0] = 0;
	s_runner.actualTeamCounts[1] = 0;
	s_runner.replayEpoch = SKIRMISH_AI_REPLAY_EPOCH_LEGACY;
	s_runner.retainedReplayPath[0] = '\0';
	s_runner.replaySha256[0] = '\0';
	++s_runnerNonceCounter;
	if (s_runnerNonceCounter == 0)
		++s_runnerNonceCounter;
	_snprintf(s_runner.runNonce, ARRAY_SIZE(s_runner.runNonce),
		"%08X-%08X-%08X", GetTickCount(),
		static_cast<UnsignedInt>(seed), s_runnerNonceCounter);
	s_runner.runNonce[ARRAY_SIZE(s_runner.runNonce) - 1] = '\0';
	strlcpy(s_executableHashObserved, "unavailable",
		ARRAY_SIZE(s_executableHashObserved));
	CaptureSkirmishAITestExecutableHash(s_executableHashObserved);
	const rts::JobSystemConfig jobConfig = rts::JobSystem::startupConfig();
	s_runner.requestedWorkerCount = jobConfig.workerCount;
	s_runner.workerPolicy = jobConfig.workerPolicy;
	s_runner.effectiveWorkerCount = 0;
	s_runner.requestedPipelineMode = rts::GetPipelineExecutionMode();
	s_runner.requestedSimulationMode = rts::GetSimulationExecutionMode();
	s_jobMetricsAtStart = rts::JobSystem::instance().metrics();
	s_phaseMetricsAtStart = TheGameLogic != nullptr ?
		TheGameLogic->getStage5PhaseRuntimeMetrics() :
		rts::LiveSimulationPhaseRuntimeMetrics();
	s_phaseMetricsLast = s_phaseMetricsAtStart;
	s_directPathMetricsAtStart = GetDirectPathRuntimeMetrics();
	memset(&s_directPathMetricsFrozen, 0, sizeof(s_directPathMetricsFrozen));
	s_directPathMetricsHaveFrozenActivity = FALSE;
	s_directPathMetricsAwaitingInitialReset = TRUE;
	s_ordinaryPathMetricsAtStart = GetOrdinaryPathRuntimeMetrics();
	memset(&s_ordinaryPathMetricsFrozen, 0,
		sizeof(s_ordinaryPathMetricsFrozen));
	s_ordinaryPathMetricsAwaitingInitialReset = TRUE;
#if defined(_WIN64)
	s_aiPlanningMetricsAtStart = rts::GetAIPlanningRuntimeMetrics();
	s_collisionMetricsAtStart = rts::GetCollisionCandidateRuntimeMetrics();
	s_collisionMetricsFrozen = rts::CollisionCandidateRuntimeMetrics();
	s_collisionMetricsAwaitingInitialReset = TRUE;
	s_physicsMetricsAtStart = rts::GetPhysicsIntegrationRuntimeMetrics();
	s_physicsMetricsFrozen = rts::PhysicsIntegrationRuntimeMetrics();
	s_physicsMetricsAwaitingInitialReset = TRUE;
	s_statusMetricsAtStart = rts::GetObjectStatusTimerRuntimeMetrics();
	s_statusMetricsFrozen = rts::ObjectStatusTimerRuntimeMetrics();
	s_statusMetricsAwaitingInitialReset = TRUE;
	s_spatialMetricsAtStart = rts::GetImmutableSpatialRuntimeMetrics();
	s_spatialMetricsFrozen = rts::ImmutableSpatialRuntimeMetrics();
	s_spatialMetricsAwaitingInitialReset = TRUE;
#endif
	s_finalDigest = 0;
	s_finalDigestAvailable = FALSE;
}

void ArmSkirmishAIRecoveryFixtureRunner(Int seed, Int fixtureCase, Int faction)
{
	ArmSkirmishAITestRunner(seed, SKIRMISH_AI_TEST_SCENARIO_4V3);
	s_recovery.active = TRUE;
	s_recovery.fixtureCase = fixtureCase;
	s_recovery.faction = faction;
	s_recovery.phase = SKIRMISH_AI_RECOVERY_PHASE_WAIT_BASELINE;
	s_recovery.phaseStartFrame = 0;
}



Bool IsSkirmishAITestRunnerArmed()
{
	return s_runner.armed;
}

#if defined(_WIN64)
Bool ConfigureSkirmishAITestReviewedMap(const rts::ai_fixture::MapRequest &request)
{
	if (!request.requested || s_reviewedMapRequest.requested) return FALSE;
	s_reviewedMapRequest = request;
	return TRUE;
}
#endif

Bool StartSkirmishAITestRunner()
{
	if (!s_runner.armed)
		return TRUE;
#if defined(_WIN64)
	// Explicit native trace intent is a caller contract, not a game-start
	// fallback. Reject it before observing or mutating any game, global, lobby,
	// RNG, or owner state so an unsupported fresh shape is side-effect free.
	if (PerformanceReceiptRuntime::explicitTraceRequestedFromEnvironment())
	{
		FailSkirmishAITest(IsSkirmishAITestPracticalControllerScenario(s_runner.scenario) ?
			"explicit trace admission is unsupported for practical controller" :
			"explicit trace admission is unsupported for fresh matches");
		return FALSE;
	}
#endif
	DEBUG_LOG(("SkirmishAITestRunner::start phase=entry seed=%d", s_runner.seed));
	s_runner.startupStartMilliseconds = GetTickCount();
	CaptureSkirmishAITestRuntimeState();
	if (!TheGlobalData->m_simulateReplays.empty())
	{
		FailSkirmishAITest("conflicting_replay_mode");
		return FALSE;
	}
	if (!TheMapCache || !TheMessageStream || !TheRecorder || !TheWritableGlobalData)
	{
		FailSkirmishAITest("engine_not_ready");
		return FALSE;
	}
	DEBUG_LOG(("SkirmishAITestRunner::start phase=dependencies_ready"));

	SkirmishAITestPlan plan;
	BuildSkirmishAITestPlan(s_runner.seed, s_runner.scenario, &plan);
	if (s_recovery.active)
	{
#if !RTS_ZEROHOUR
		FailSkirmishAITest("zero_hour_only");
		return FALSE;
#endif
		if (!IsSupportedSkirmishAIRecoveryFixtureCombination(s_recovery.fixtureCase, s_recovery.faction))
		{
			FailSkirmishAITest("fixture_unsupported_case_faction");
			return FALSE;
		}
		s_recovery.templateIndex = FindPlayerTemplateIndex(
			GetSkirmishAIRecoveryFactionTemplateName(s_recovery.faction));
		if (s_recovery.templateIndex < 0)
		{
			FailSkirmishAITest("fixture_faction_unavailable");
			return FALSE;
		}
		plan.slots[1].playerTemplate = s_recovery.templateIndex;
	}
#if defined(_WIN64)
	if (s_reviewedMapRequest.requested)
	{
		if (!IsSkirmishAITest4v2(s_runner.scenario) ||
			!ResolveStage5MapIdentity(s_reviewedMapRequest.mapKey, &s_reviewedMapIdentity) ||
			!s_reviewedMapIdentity.profileMap ||
			strlen(s_reviewedMapIdentity.runtimePath) >= sizeof(s_runner.loadedMapName) ||
			!VerifyReviewedSkirmishMapBytes())
		{
			FailSkirmishAITest("reviewed_map_binding_failed");
			return FALSE;
		}
		plan.mapName = s_reviewedMapIdentity.runtimePath;
	}
#endif
	const MapMetaData *map = TheMapCache->findMap(plan.mapName);
	const Int expectedMapPlayers = ExpectedSkirmishAITestAiCount(s_runner.scenario) +
		(IsSkirmishAITestHardAI2v6(s_runner.scenario) ? 0 : 1);
	if (!map || !map->m_doesExist || !map->m_isMultiplayer ||
		map->m_numPlayers < expectedMapPlayers)
	{
		FailSkirmishAITest("twilight_flame_unavailable");
		return FALSE;
	}
	DEBUG_LOG(("SkirmishAITestRunner::start phase=map_ready"));
	s_runner.expectedMapCRC = map->m_CRC;
	s_runner.expectedMapSize = map->m_filesize;
#if defined(_WIN64)
	if (s_reviewedMapRequest.requested && (map->m_CRC != s_reviewedMapRequest.crc ||
		map->m_filesize != s_reviewedMapRequest.byteCount))
	{
		FailSkirmishAITest("reviewed_map_cache_identity_mismatch");
		return FALSE;
	}
#endif

	delete TheSkirmishGameInfo;
	TheSkirmishGameInfo = NEW SkirmishGameInfo;
	TheSkirmishGameInfo->init();
	TheSkirmishGameInfo->clearSlotList();
	TheSkirmishGameInfo->reset();
	TheSkirmishGameInfo->setLocalIP(0);
	TheSkirmishGameInfo->enterGame();
	DEBUG_LOG(("SkirmishAITestRunner::start phase=game_info_ready"));

	for (Int i = 0; i < SKIRMISH_AI_TEST_SLOT_COUNT; ++i)
	{
		const SkirmishAITestSlotPlan &slotPlan = plan.slots[i];
		GameSlot *slot = TheSkirmishGameInfo->getSlot(i);
		UnicodeString localName;
		if (i == 0)
			localName.set(slotPlan.isController ? L"Practical Controller" :
				L"Automated Observer");
		slot->setState(slotPlan.state, localName, 0);
		slot->setPlayerTemplate(slotPlan.playerTemplate);
		slot->setColor(slotPlan.color);
		slot->setStartPos(slotPlan.startPosition);
		slot->setTeamNumber(slotPlan.teamNumber);
		if (i == 0)
		{
			slot->setAccept();
			slot->setMapAvailability(TRUE);
		}
	}
	DEBUG_LOG(("SkirmishAITestRunner::start phase=slots_ready"));

	TheSkirmishGameInfo->setMap(plan.mapName);
	TheSkirmishGameInfo->setMapCRC(map->m_CRC);
	TheSkirmishGameInfo->setMapSize(map->m_filesize);
	TheSkirmishGameInfo->setSeed(plan.seed);
	TheSkirmishGameInfo->startGame(0);
	DEBUG_LOG(("SkirmishAITestRunner::start phase=start_game_complete"));

	TheWritableGlobalData->m_mapName = plan.mapName;
	// The practical controller lane is intentionally interactive.  Automated
	// lanes remain headless and continue to be the replay-gate scenarios.
	TheWritableGlobalData->m_headless =
		!s_allied.active && IsSkirmishAITestPracticalControllerScenario(s_runner.scenario) ? FALSE : TRUE;
	TheWritableGlobalData->m_shellMapOn = FALSE;
	if (s_allied.active || !IsSkirmishAITestPracticalControllerScenario(s_runner.scenario))
		TheWritableGlobalData->m_useFpsLimit = FALSE;
	// Automated lanes keep logical and local retaliation modes disabled so the
	// recorder does not capture an irrelevant frame-zero preference
	// synchronization command.
	if (s_allied.active || !IsSkirmishAITestPracticalControllerScenario(s_runner.scenario))
		TheWritableGlobalData->m_clientRetaliationModeEnabled = FALSE;
	TheRecorder->setArchiveEnabled(FALSE);
	InitRandom(static_cast<UnsignedInt>(plan.seed));

#if defined(_WIN64)
	if (!s_recovery.active &&
		!IsSkirmishAITestPracticalControllerScenario(s_runner.scenario) &&
		!s_allied.active && !s_performanceReceiptAttempted)
	{
		s_performanceReceiptAttempted = true;
		s_performanceReceipt.reset(new PerformanceReceiptRuntime);
		if (!s_performanceReceipt->begin("fresh-ai-map", ""))
		{
			const bool traceRequested = s_performanceReceipt->traceRequested();
			s_performanceReceipt.reset();
			if (traceRequested)
			{
				FailSkirmishAITest("explicit trace admission is unsupported for fresh matches");
				return FALSE;
			}
		}
		else if (TheGameLogic != 0 &&
			TheGameLogic->attachPerformanceReceiptRuntime(s_performanceReceipt.get()))
			s_performanceReceiptOwner = TheGameLogic;
		else
			s_performanceReceipt->invalidate("fresh owner rejected the receipt runtime borrow");
	}
#endif
	GameMessage *message = TheMessageStream->appendMessage(GameMessage::MSG_NEW_GAME);
	message->appendIntegerArgument(GAME_SKIRMISH);
	message->appendIntegerArgument(DIFFICULTY_NORMAL);
	message->appendIntegerArgument(0);

	s_runner.started = TRUE;
	const char *reportedMapName = plan.mapName;
#if defined(_WIN64)
	if (s_reviewedMapRequest.requested) reportedMapName = s_reviewedMapIdentity.logicalKey;
#endif
	if (s_recovery.active)
	{
		printf("SKIRMISH_AI_RECOVERY_START seed=%d case=%s faction=%s template=%s map=\"%s\" "
			"expected_mode=zero_hour_skirmish expected_subject_slot=1\n",
			plan.seed, GetSkirmishAIRecoveryFixtureCaseName(s_recovery.fixtureCase),
			GetSkirmishAIRecoveryFactionName(s_recovery.faction),
			GetSkirmishAIRecoveryFactionTemplateName(s_recovery.faction), reportedMapName);
	}
	else if (s_allied.active)
	{
		printf("SKIRMISH_AI_ALLIED_FIXTURE_START seed=%d case=%s map=\"%s\"\n",
			plan.seed, s_alliedCaseNames[s_allied.fixtureCase], reportedMapName);
	}
	else if (IsSkirmishAITest4v2(s_runner.scenario))
	{
		printf("SKIRMISH_AI_TEST_START seed=%d scenario=%s map=\"%s\" expected_ai=6 expected_teams=4v2\n",
			plan.seed, SkirmishAITestScenarioName(s_runner.scenario), reportedMapName);
	}
	else if (IsSkirmishAITestPracticalControllerScenario(s_runner.scenario))
	{
		printf("SKIRMISH_AI_TEST_START seed=%d scenario=%s map=\"%s\" expected_ai=7 expected_teams=1-controller+3v4-ai\n",
			plan.seed, SkirmishAITestScenarioName(s_runner.scenario), reportedMapName);
	}
	else if (IsSkirmishAITestHardAI2v6(s_runner.scenario))
	{
		printf("SKIRMISH_AI_TEST_START seed=%d scenario=%s map=\"%s\" expected_ai=8 expected_teams=2v6\n",
			plan.seed, SkirmishAITestScenarioName(s_runner.scenario), reportedMapName);
	}
	else
	{
		printf("SKIRMISH_AI_TEST_START seed=%d scenario=%s map=\"%s\" expected_ai=7 expected_teams=4v3\n",
			plan.seed, SkirmishAITestScenarioName(s_runner.scenario), reportedMapName);
	}
	fflush(stdout);
	return TRUE;
}

void UpdateSkirmishAITestRunner()
{
	if (s_runner.armed)
		CaptureSkirmishAITestRuntimeState();
	static UnsignedInt lastDiagnosticMilliseconds = 0;
	const UnsignedInt diagnosticMilliseconds = GetTickCount();
	if (s_runner.armed &&
		(lastDiagnosticMilliseconds == 0 ||
			ElapsedMilliseconds(lastDiagnosticMilliseconds, diagnosticMilliseconds) >= 10000))
	{
		lastDiagnosticMilliseconds = diagnosticMilliseconds;
		DEBUG_LOG(("SkirmishAITestRunner::update armed=%d started=%d ending=%d finished=%d failed=%d frame=%u",
			s_runner.armed, s_runner.started, s_runner.ending, s_runner.finished, s_runner.failed,
			TheGameLogic ? TheGameLogic->getFrame() : 0));
	}

	if (!s_runner.armed || !s_runner.started || s_runner.finished)
		return;
	if (!TheGameLogic)
	{
		FailSkirmishAITest("runtime_state_unavailable");
		RequestSkirmishAITestStop();
		return;
	}
	if (s_runner.ending)
	{
		const UnsignedInt shutdownElapsed =
			ElapsedMilliseconds(s_runner.shutdownStartMilliseconds, GetTickCount());
		if (!TheGameLogic->isInGame())
		{
			s_runner.finished = !s_runner.failed;
			TheGameEngine->setQuitting(TRUE);
		}
		else if (IsSkirmishAITestShutdownTimedOut(shutdownElapsed))
		{
			FailSkirmishAITest("shutdown_timeout");
			if (TheRecorder && TheRecorder->getMode() == RECORDERMODETYPE_RECORD)
				TheRecorder->stopRecording();
			TheGameLogic->clearGameData(FALSE);
			TheGameEngine->setQuitting(TRUE);
		}
		return;
	}
	if (!TheGameLogic->isInGame() || TheGameLogic->isLoadingMap() || !TheGameInfo)
	{
		const UnsignedInt startupElapsed =
			ElapsedMilliseconds(s_runner.startupStartMilliseconds, GetTickCount());
		if (IsSkirmishAITestStartupTimedOut(startupElapsed))
		{
			FailSkirmishAITest("startup_timeout");
			RequestSkirmishAITestStop();
		}
		return;
	}
	if (!TheVictoryConditions || !ThePlayerList || !TheRecorder)
	{
		FailSkirmishAITest("runtime_state_unavailable");
		RequestSkirmishAITestStop();
		return;
	}

	SkirmishAITestPlan expectedPlan;
	BuildSkirmishAITestPlan(s_runner.seed, s_runner.scenario, &expectedPlan);
	if (s_recovery.active)
		expectedPlan.slots[1].playerTemplate = s_recovery.templateIndex;
#if defined(_WIN64)
	if (s_reviewedMapRequest.requested) expectedPlan.mapName = s_reviewedMapIdentity.runtimePath;
#endif
	const AsciiString gameInfoMap = TheGameInfo->getMap();
	const AsciiString globalMap = TheGlobalData->m_mapName;
	const AsciiString terrainMap = TheTerrainLogic
		? TheTerrainLogic->getSourceFilename()
		: AsciiString::TheEmptyString;
	SkirmishAITestLoadedState loadedState = {
		gameInfoMap.str(), globalMap.str(), terrainMap.str(),
		TheGameInfo->getMapCRC(), TheGameInfo->getMapSize(), TheGameInfo->getSeed()
	};
	if (!(s_recovery.active || s_allied.saveLoaded ?
		IsExpectedSkirmishAIRecoveryLoadedState(expectedPlan, s_runner.expectedMapCRC,
			s_runner.expectedMapSize, &loadedState) :
		IsExpectedSkirmishAITestLoadedState(expectedPlan, s_runner.expectedMapCRC,
			s_runner.expectedMapSize, &loadedState)))
	{
		FailSkirmishAITest("loaded_state_mismatch");
		RequestSkirmishAITestStop();
		return;
	}
	if (!s_runner.loadedStateValidated)
	{
#if defined(_WIN64)
		if (s_reviewedMapRequest.requested && !VerifyReviewedSkirmishMapBytes())
		{
			FailSkirmishAITest("reviewed_loaded_map_identity_mismatch");
			RequestSkirmishAITestStop();
			return;
		}
#endif
		s_runner.loadedStateValidated = TRUE;
		strlcpy(s_runner.loadedMapName, loadedState.gameInfoMapName, ARRAY_SIZE(s_runner.loadedMapName));
		s_runner.loadedMapCRC = loadedState.mapCRC;
		s_runner.loadedMapSize = loadedState.mapSize;
		s_runner.loadedSeed = loadedState.seed;
#if defined(_WIN64)
		BindSkirmishAITestPerformanceMap();
#endif
	}

	if (s_allied.active)
	{
		TheWritableGlobalData->m_useFpsLimit = FALSE;
		UpdateSkirmishAIAlliedFixture();
		return;
	}
	if (s_recovery.active)
	{
		TheWritableGlobalData->m_useFpsLimit = FALSE;
		UpdateSkirmishAIRecoveryFixture();
		return;
	}
	if (!IsSkirmishAITestPracticalControllerScenario(s_runner.scenario))
		TheWritableGlobalData->m_useFpsLimit = FALSE;
	// RecorderClass::startRecording() resets this preference after the runner's
	// startup hook. Reassert it while recording so LastReplay remains at the
	// exact path reported and validated by this test.
	TheRecorder->setArchiveEnabled(FALSE);
	if (TheRecorder->getMode() == RECORDERMODETYPE_RECORD && !TheRecorder->hasOpenRecordingFile())
	{
		FailSkirmishAITest("recorder_file_unavailable");
		RequestSkirmishAITestStop();
		return;
	}
	if (s_runner.replayFileName[0] == '\0' && TheRecorder->getMode() == RECORDERMODETYPE_RECORD)
	{
		const AsciiString recordingFileName = TheRecorder->getRecordingFileName();
		strlcpy(s_runner.replayFileName, recordingFileName.str(), ARRAY_SIZE(s_runner.replayFileName));
	}
	const UnsignedInt endFrame = TheVictoryConditions->getEndFrame();
	const UnsignedInt currentFrame = TheGameLogic->getFrame();
	const SkirmishAITestProgress progress =
		EvaluateSkirmishAITestProgress(endFrame, currentFrame);
	if (progress == SKIRMISH_AI_TEST_RUNNING)
	{
		const UnsignedInt nowMilliseconds = GetTickCount();
		if (currentFrame == s_runner.lastObservedFrame)
		{
			const UnsignedInt stalledElapsed =
				ElapsedMilliseconds(s_runner.stalledStartMilliseconds, nowMilliseconds);
			if (IsSkirmishAITestProgressStalled(stalledElapsed))
			{
				FailSkirmishAITest("frame_stalled");
				RequestSkirmishAITestStop();
			}
		}
		else
		{
			s_runner.lastObservedFrame = currentFrame;
			s_runner.stalledStartMilliseconds = nowMilliseconds;
		}
		return;
	}
	if (progress == SKIRMISH_AI_TEST_TIMED_OUT)
	{
		FailSkirmishAITest("frame_limit");
		RequestSkirmishAITestStop();
		return;
	}

	const GameSlot *localSlot = TheGameInfo->getConstSlot(0);
	const Bool hardAI2v6 = IsSkirmishAITestHardAI2v6(s_runner.scenario);
	Player *localPlayer = hardAI2v6
		? ThePlayerList->findPlayerWithNameKey(NAMEKEY("ReplayObserver"))
		: ThePlayerList->findPlayerWithNameKey(NAMEKEY("player0"));
	Bool validMatch = FALSE;
	if (hardAI2v6)
	{
		// An all-AI run has no GameInfo human slot. GameLogic creates the
		// permanent ReplayObserver player outside those eight occupied slots.
		validMatch = localSlot && localSlot->isAI() && localPlayer &&
			localPlayer->isPlayerObserver() &&
			ThePlayerList->getLocalPlayer() == localPlayer &&
			TheGameInfo->getLocalSlotNum() == -1 &&
			TheGameInfo->getNumPlayers() == SKIRMISH_AI_TEST_SLOT_COUNT &&
			TheGameInfo->getNumNonObserverPlayers() == SKIRMISH_AI_TEST_SLOT_COUNT;
	}
	else if (IsSkirmishAITestPracticalControllerScenario(s_runner.scenario))
	{
		// A practical run must own a normal player slot.  An observer is not a
		// substitute for controller coverage and is rejected explicitly here.
		validMatch = localSlot && localSlot->isHuman() &&
			localSlot->getOriginalPlayerTemplate() != PLAYERTEMPLATE_OBSERVER &&
			localPlayer && !localPlayer->isPlayerObserver();
	}
	else
	{
		validMatch = localSlot && localSlot->isHuman() &&
			localSlot->getOriginalPlayerTemplate() == PLAYERTEMPLATE_OBSERVER &&
			localPlayer && localPlayer->isPlayerObserver();
	}
	Int expectedAiCount = 0;
	Int expectedTeamCounts[2] = { 0, 0 };
	const Int firstAiSlot = hardAI2v6 ? 0 : 1;
	for (Int i = firstAiSlot; i < SKIRMISH_AI_TEST_SLOT_COUNT; ++i)
	{
		const SkirmishAITestSlotPlan &slotPlan = expectedPlan.slots[i];
		if (slotPlan.state == SLOT_BRUTAL_AI)
		{
			++expectedAiCount;
			if (slotPlan.teamNumber == 0 || slotPlan.teamNumber == 1)
				++expectedTeamCounts[slotPlan.teamNumber];
		}
	}
	Int actualAiCount = 0;
	Int actualTeamCounts[2] = { 0, 0 };
	for (Int slotIndex = firstAiSlot; slotIndex < SKIRMISH_AI_TEST_SLOT_COUNT; ++slotIndex)
	{
		const SkirmishAITestSlotPlan &expectedSlot = expectedPlan.slots[slotIndex];
		const GameSlot *slot = TheGameInfo->getConstSlot(slotIndex);
		AsciiString playerName;
		playerName.format("player%d", slotIndex);
		Player *player = ThePlayerList->findPlayerWithNameKey(NAMEKEY(playerName));
		if (slot && slot->getState() == SLOT_BRUTAL_AI)
		{
			++actualAiCount;
			const Int actualTeam = slot->getTeamNumber();
			if (actualTeam == 0 || actualTeam == 1)
				++actualTeamCounts[actualTeam];
			else
				validMatch = FALSE;
		}

		if (expectedSlot.state == SLOT_CLOSED)
		{
			if (!slot || slot->getState() != SLOT_CLOSED || player != nullptr)
				validMatch = FALSE;
			continue;
		}

		if (!slot || slot->getState() != SLOT_BRUTAL_AI ||
			slot->getOriginalPlayerTemplate() != expectedSlot.playerTemplate ||
			slot->getOriginalColor() != expectedSlot.color ||
			slot->getOriginalStartPos() != expectedSlot.startPosition ||
			!player || player->getPlayerType() != PLAYER_COMPUTER ||
			slot->getTeamNumber() != expectedSlot.teamNumber)
		{
			validMatch = FALSE;
		}
	}
	if (IsSkirmishAITestPracticalControllerScenario(s_runner.scenario) &&
		(!localSlot || localSlot->getTeamNumber() != 0 ||
		localSlot->getOriginalPlayerTemplate() == PLAYERTEMPLATE_OBSERVER))
	{
		validMatch = FALSE;
	}
	if (!validMatch || actualAiCount != expectedAiCount ||
		actualTeamCounts[0] != expectedTeamCounts[0] ||
		actualTeamCounts[1] != expectedTeamCounts[1])
	{
		const char *failureReason = "invalid_4v3_setup";
		if (IsSkirmishAITest4v2(s_runner.scenario))
			failureReason = "invalid_4v2_setup";
		else if (IsSkirmishAITestPracticalControllerScenario(s_runner.scenario))
			failureReason = "invalid_practical_1v7_setup";
		else if (hardAI2v6)
			failureReason = "invalid_hard_ai_2v6_setup";
		FailSkirmishAITest(failureReason);
		RequestSkirmishAITestStop();
		return;
	}
	s_runner.actualAiCount = actualAiCount;
	s_runner.actualTeamCounts[0] = actualTeamCounts[0];
	s_runner.actualTeamCounts[1] = actualTeamCounts[1];

	Int winnerTeam = -1;
	Bool conflictingWinners = FALSE;
	for (Int winnerIndex = 0; winnerIndex < SKIRMISH_AI_TEST_SLOT_COUNT; ++winnerIndex)
	{
		AsciiString playerName;
		playerName.format("player%d", winnerIndex);
		Player *player = ThePlayerList->findPlayerWithNameKey(NAMEKEY(playerName));
		if (player && TheVictoryConditions->hasAchievedVictory(player))
		{
			const GameSlot *slot = TheGameInfo->getConstSlot(winnerIndex);
			if (!slot)
			{
				conflictingWinners = TRUE;
				continue;
			}
			const Int playerTeam = slot->getTeamNumber();
			if (winnerTeam == -1)
				winnerTeam = playerTeam;
			else if (winnerTeam != playerTeam)
				conflictingWinners = TRUE;
		}
	}
	if (conflictingWinners || (winnerTeam != 0 && winnerTeam != 1))
	{
		FailSkirmishAITest("winner_unavailable");
		RequestSkirmishAITestStop();
		return;
	}
	if (TheRecorder->getMode() != RECORDERMODETYPE_RECORD || !TheRecorder->hasOpenRecordingFile())
	{
		FailSkirmishAITest("recorder_file_unavailable");
		RequestSkirmishAITestStop();
		return;
	}
	if (s_runner.replayFileName[0] == '\0')
	{
		FailSkirmishAITest("replay_filename_unavailable");
		RequestSkirmishAITestStop();
		return;
	}

	s_runner.winnerTeam = winnerTeam;
	s_runner.endFrame = endFrame;
	// Capture the authoritative owner-thread state digest before exitGame tears
	// down the live simulation. Validation compares this value across worker
	// counts and execution modes; it must never be derived from worker order.
	const UnsignedInt finalCRC = TheGameLogic->getCRC(CRC_RECALC);
	SetSkirmishAITestFinalDigest(finalCRC);
#if defined(_WIN64)
	// The victory event frame remains unchanged in the replay contract. Its
	// authoritative digest belongs to the actual current owner frame instead.
	if (s_performanceReceipt)
		s_performanceReceipt->captureTerminalResult(currentFrame, finalCRC, true, true);
#endif
	RequestSkirmishAITestStop();
}

#if defined(_WIN64)
void ObserveSkirmishAITestCompletedFrame(unsigned previousFrame)
{
	if (!s_performanceReceipt || !s_performanceReceipt->active() ||
		!s_runner.loadedStateValidated || s_runner.failed)
		return;
	// ending may already be true: exitGame queued teardown, but this frame
	// still contains the terminal owner state and must be observed after EndFrame.
	s_performanceReceipt->captureCompletedFrame(previousFrame,
		s_collisionMetricsFrozen, s_physicsMetricsFrozen, s_statusMetricsFrozen,
		s_spatialMetricsFrozen, s_ordinaryPathMetricsFrozen);
}

void ReleaseSkirmishAITestPerformanceReceiptOwner()
{
	if (s_performanceReceiptOwner == 0) return;
	// Check identity before dereferencing the retained address. Ordinary attach
	// rejection only disables evidence; an impossible detach cannot leave a
	// live GameLogic referring to runtime storage which will be released.
	if (!s_performanceReceipt || TheGameLogic != s_performanceReceiptOwner ||
		!s_performanceReceiptOwner->detachPerformanceReceiptRuntime(s_performanceReceipt.get()))
	{
		RELEASE_CRASH(("Fresh receipt runtime borrow could not be released before owner destruction."));
		abort();
	}
	s_performanceReceiptOwner = 0;
}

void FinalizeSkirmishAITestPerformanceReceipt(Int engineExitCode)
{
	if (s_performanceReceiptOwner != 0)
	{
		RELEASE_CRASH(("Fresh receipt runtime cannot be freed while its owner borrow remains live."));
		abort();
	}
	if (s_performanceReceipt)
	{
		s_performanceReceipt->finish(engineExitCode, "GameMain:engine-destroyed-before-owner-detach");
		s_performanceReceipt.reset();
	}
}
#endif


namespace
{

Bool ObserveSkirmishAIRecoverySecondBuilderRoutePayment(
	Player *player, UnsignedInt cash);

Bool ObserveSkirmishAIRecoveryFixture(Player *player)
{
	if (!player || !s_recovery.primaryTemplate || !s_recovery.builderTemplate)
		return FALSE;

	Int commandCenterCount = 0;
	Bool commandCenterUnderConstruction = FALSE;
	Object *commandCenter = FindSkirmishAIRecoveryCommandCenter(
		player, s_recovery.primaryTemplate, &commandCenterCount,
		&commandCenterUnderConstruction);
	MaybePrintSkirmishAIRecoveryFactoryDiagnostics(player);
	if (!commandCenter && s_recovery.sawConstruction &&
		!s_recovery.sawCompletedRecovery &&
		(s_recovery.phase == SKIRMISH_AI_RECOVERY_PHASE_WAIT_RECOVERY ||
		 s_recovery.phase == SKIRMISH_AI_RECOVERY_PHASE_WAIT_SECOND_RECOVERY))
	{
		Object *trackedConstruction = s_recovery.currentConstructionID != INVALID_ID
			? TheGameLogic->findObjectByID(s_recovery.currentConstructionID) : nullptr;
		if (trackedConstruction &&
			trackedConstruction->testStatus(OBJECT_STATUS_SOLD))
		{
			printf("SKIRMISH_AI_RECOVERY_PHASE phase=scaffold_sold_retry "
				"frame=%u center=%u percent=%g cash=%u\n",
				TheGameLogic->getFrame(), trackedConstruction->getID(),
				trackedConstruction->getConstructionPercent(),
				player->getMoney() ? player->getMoney()->countMoney() : 0);
			fflush(stdout);
			// Keep the accumulated scaffold/payment/recovery counters, but clear
			// only this attempt so a replacement scaffold can be observed.
			s_recovery.currentConstructionID = INVALID_ID;
			s_recovery.lastConstructionBuilderID = INVALID_ID;
			s_recovery.lastConstructionPercent = 0.0f;
			s_recovery.sawConstruction = FALSE;
			s_recovery.sawConstructionProgress = FALSE;
			s_recovery.sawConstructionOwnership = FALSE;
			s_recovery.hasConstructionAttempt = FALSE;
			return TRUE;
		}
		PrintSkirmishAIRecoveryScaffoldDiagnostics(
			player, nullptr, "recovery_scaffold_disappeared");
		FailSkirmishAITest("fixture_recovery_scaffold_disappeared");
		return FALSE;
	}
	if (s_recovery.fixtureCase == SKIRMISH_AI_RECOVERY_OBSTRUCTED &&
		s_recovery.obstructionPlaced)
	{
		Object *obstruction = s_recovery.obstructionID != INVALID_ID
			? TheGameLogic->findObjectByID(s_recovery.obstructionID) : nullptr;
		const Bool obstructionLive = IsLiveSkirmishAIRecoveryObject(obstruction);
		const Bool obstructionStructure = obstruction && obstruction->isStructure();
		const Bool obstructionImmobile = obstruction &&
			obstruction->isKindOf(KINDOF_IMMOBILE);
		const Bool obstructionOwner = obstruction &&
			obstruction->getControllingPlayer() == player;
		const Bool obstructionUnderConstruction = obstruction &&
			obstruction->testStatus(OBJECT_STATUS_UNDER_CONSTRUCTION);
		const Bool obstructionSold = obstruction &&
			obstruction->testStatus(OBJECT_STATUS_SOLD);
		const Bool obstructionAtExpectedPosition = obstruction &&
			obstruction->getPosition() &&
			!IsDifferentSkirmishAIRecoveryPosition(
				s_recovery.obstructionBlockedPosition, *obstruction->getPosition());
		if (!obstructionLive || !obstructionStructure || !obstructionImmobile ||
			!obstructionOwner || obstructionUnderConstruction || obstructionSold ||
			!obstructionAtExpectedPosition)
		{
			Coord3D actualPosition;
			actualPosition.zero();
			if (obstruction && obstruction->getPosition())
				actualPosition = *obstruction->getPosition();
			printf("SKIRMISH_AI_RECOVERY_DIAGNOSTIC phase=obstruction_check "
				"frame=%u obstruction=%u live=%d structure=%d immobile=%d owner=%d "
				"under_construction=%d sold=%d expected=(%g,%g,%g) actual=(%g,%g,%g)\n",
				TheGameLogic->getFrame(), s_recovery.obstructionID,
				obstructionLive, obstructionStructure, obstructionImmobile,
				obstructionOwner, obstructionUnderConstruction, obstructionSold,
				s_recovery.obstructionBlockedPosition.x,
				s_recovery.obstructionBlockedPosition.y,
				s_recovery.obstructionBlockedPosition.z,
				actualPosition.x, actualPosition.y, actualPosition.z);
			fflush(stdout);
			FailSkirmishAITest(obstructionAtExpectedPosition
				? "fixture_obstruction_lost" : "fixture_obstruction_moved");
			return FALSE;
		}
	}
	if (commandCenterCount > 1)
	{
		PrintSkirmishAIRecoveryDuplicateCommandCenterDiagnostics(
			player, s_recovery.primaryTemplate);
		s_recovery.sawNoDuplicateCommandCenter = FALSE;
		FailSkirmishAITest("fixture_duplicate_command_center");
		return FALSE;
	}
	if (!ObserveSkirmishAIRecoveryHoleLineage(commandCenter))
		return FALSE;
	if (s_recovery.fixtureCase == SKIRMISH_AI_RECOVERY_REPEATED_COMMAND_CENTER &&
		s_recovery.secondBuilderLossIssued && commandCenter &&
		commandCenter->getID() != s_recovery.secondBuilderLossConstructionID)
	{
		FailSkirmishAITest("fixture_second_builder_scaffold_replaced");
		return FALSE;
	}

	Int builderCount = CountSkirmishAIRecoveryBuilders(player, nullptr);
	if (s_recovery.fixtureCase == SKIRMISH_AI_RECOVERY_SURVIVING_BUILDER &&
		s_recovery.baselineCaptured && !s_recovery.sawConstruction &&
		HasUnexpectedSkirmishAIRecoveryBuilder(player))
	{
		FailSkirmishAITest("fixture_surviving_builder_replaced");
		return FALSE;
	}
	if (s_recovery.baselineCaptured &&
		!IsSkirmishAIRecoveryFactoryFixture() &&
		s_recovery.fixtureCase != SKIRMISH_AI_RECOVERY_INFRASTRUCTURE_COLLAPSE &&
		builderCount > s_recovery.initialBuilderCount + 2)
	{
		FailSkirmishAITest("fixture_duplicate_builder");
		return FALSE;
	}
	if (IsSkirmishAIRecoveryFactoryFixture() &&
		!s_recovery.sawConstruction &&
		CountSkirmishAIRecoveryBuildersProducedByFactory(
			player, s_recovery.builderTemplate, s_recovery.builderFactoryID) >
			s_recovery.initialFactoryBuilderCount + 1)
	{
		PrintSkirmishAIRecoveryFactoryDiagnostics(player, "duplicate_recovery_builder");
		FailSkirmishAITest("fixture_duplicate_recovery_builder");
		return FALSE;
	}

	const Int recoveryQueueCount = IsSkirmishAIRecoveryFactoryFixture()
		? CountSkirmishAIRecoveryBuilderQueueEntriesOnFactory(
			player, s_recovery.builderTemplate, s_recovery.builderFactoryID) : 0;
	const ProductionID firstQueueID = recoveryQueueCount > 0
		? FindSkirmishAIRecoveryBuilderQueueIDOnFactory(
			player, s_recovery.builderTemplate, s_recovery.builderFactoryID)
		: PRODUCTIONID_INVALID;
	if (recoveryQueueCount > 1)
	{
		const Int otherQueueCount = CountSkirmishAIRecoveryBuilderQueueEntriesOnFactoryExcept(
			player, s_recovery.builderTemplate, s_recovery.builderFactoryID, firstQueueID);
		if (otherQueueCount > 0 &&
			(!s_recovery.sawConstruction || !s_recovery.sawConstructionOwnership))
		{
			PrintSkirmishAIRecoveryFactoryDiagnostics(player, "duplicate_builder_queue");
			FailSkirmishAITest("fixture_duplicate_builder_queue");
			return FALSE;
		}
	}

	const UnsignedInt cash = player->getMoney()->countMoney();
	if (s_recovery.fixtureCase == SKIRMISH_AI_RECOVERY_REPEATED_COMMAND_CENTER &&
		!ObserveSkirmishAIRecoverySecondBuilderRoutePayment(player, cash))
		return FALSE;
	if (recoveryQueueCount > 0 && !s_recovery.sawBuilderQueue)
	{
		s_recovery.sawBuilderQueue = TRUE;
		s_recovery.recoveryBuilderProductionID = firstQueueID;
		s_recovery.cashBeforeQueue = s_recovery.lastObservedCash;
		s_recovery.cashAfterQueue = cash;
		if (s_recovery.cashBeforeQueue <= s_recovery.cashAfterQueue ||
			s_recovery.cashBeforeQueue - s_recovery.cashAfterQueue <
			s_recovery.builderCost)
		{
			FailSkirmishAITest("fixture_builder_queue_unpaid");
			return FALSE;
		}
		s_recovery.sawBuilderQueuePayment = TRUE;
		printf("SKIRMISH_AI_RECOVERY_PHASE phase=builder_queue_paid frame=%u factory=%u cash_before=%u cash_after=%u\n",
			TheGameLogic->getFrame(), s_recovery.builderFactoryID,
			s_recovery.cashBeforeQueue, s_recovery.cashAfterQueue);
		fflush(stdout);
	}

	if (commandCenter && commandCenterUnderConstruction)
	{
		s_recovery.sawConstruction = TRUE;
		if (commandCenter->getID() != s_recovery.currentConstructionID)
		{
			if (s_recovery.hasConstructionAttempt &&
				TheGameLogic->getFrame() <= s_recovery.lastConstructionAttemptFrame + 1U)
			{
				FailSkirmishAITest("fixture_placement_retry_loop");
				return FALSE;
			}
			s_recovery.currentConstructionID = commandCenter->getID();
			s_recovery.lastConstructionBuilderID = INVALID_ID;
			s_recovery.lastConstructionPercent = 0.0f;
			++s_recovery.constructionScaffoldCount;
			s_recovery.hasConstructionAttempt = TRUE;
			s_recovery.lastConstructionAttemptFrame = TheGameLogic->getFrame();
			s_recovery.cashBeforeConstruction = s_recovery.lastObservedCash;
			s_recovery.cashAfterConstruction = cash;
			if (s_recovery.constructionScaffoldCount >
				SKIRMISH_AI_RECOVERY_MAX_CONSTRUCTION_ATTEMPTS)
			{
				FailSkirmishAITest("fixture_placement_retry_bound");
				return FALSE;
			}
			if (s_recovery.fixtureCase != SKIRMISH_AI_RECOVERY_GLA_HOLE &&
				(s_recovery.cashBeforeConstruction <= s_recovery.cashAfterConstruction ||
				 s_recovery.cashBeforeConstruction - s_recovery.cashAfterConstruction <
				 s_recovery.ccCost))
			{
				FailSkirmishAITest("fixture_command_center_unpaid");
				return FALSE;
			}
			printf("SKIRMISH_AI_RECOVERY_PHASE phase=%s frame=%u construction=%u "
				"cash_before=%u cash_after=%u scaffold=%d\n",
				s_recovery.fixtureCase == SKIRMISH_AI_RECOVERY_GLA_HOLE
					? "command_center_reconstruction_started" : "command_center_paid",
				TheGameLogic->getFrame(), commandCenter->getID(),
				s_recovery.cashBeforeConstruction, s_recovery.cashAfterConstruction,
				s_recovery.constructionScaffoldCount);
			fflush(stdout);
		}
		s_recovery.lastConstructionPercent = commandCenter->getConstructionPercent();
		if (commandCenter->getBuilderID() != INVALID_ID)
			s_recovery.lastConstructionBuilderID = commandCenter->getBuilderID();

		Object *builder = TheGameLogic->findObjectByID(commandCenter->getBuilderID());
		if (builder && IsLiveSkirmishAIRecoveryObject(builder) &&
			!builder->isContained() &&
			builder->getControllingPlayer() == player)
		{
			s_recovery.sawConstructionOwnership = TRUE;
			if (s_recovery.fixtureCase == SKIRMISH_AI_RECOVERY_REPEATED_COMMAND_CENTER &&
				s_recovery.secondBuilderLossIssued &&
				!s_recovery.secondBuilderReplacementObserved)
			{
				if (builder->getID() == s_recovery.secondBuilderLossID ||
					(s_recovery.secondBuilderRouteFactoryID == INVALID_ID &&
						!IsSkirmishAIRecoveryBaselineBuilderID(builder->getID())) ||
					(s_recovery.secondBuilderRouteFactoryID != INVALID_ID &&
						(!s_recovery.secondBuilderRoutePaid ||
						 builder->getProducerID() !=
							s_recovery.secondBuilderRouteFactoryID)))
				{
					FailSkirmishAITest("fixture_second_builder_unattributed");
					return FALSE;
				}
				s_recovery.secondBuilderReplacementID = builder->getID();
				s_recovery.secondBuilderReplacementObserved = TRUE;
				printf("SKIRMISH_AI_RECOVERY_PHASE phase=second_builder_rebound "
					"frame=%u builder=%u producer=%u route=%s\n",
					TheGameLogic->getFrame(), builder->getID(), builder->getProducerID(),
					s_recovery.secondBuilderRouteFactoryID == INVALID_ID
						? "baseline" : "factory");
				fflush(stdout);
			}
			if (IsSkirmishAIRecoveryFactoryFixture() &&
				(builder->getProducerID() != s_recovery.builderFactoryID ||
				 s_recovery.builderFactoryID == INVALID_ID))
			{
				FailSkirmishAITest("fixture_recovery_builder_wrong_factory");
				return FALSE;
			}
			if (s_recovery.fixtureCase ==
					SKIRMISH_AI_RECOVERY_DISABLED_FACTORY &&
				!s_recovery.factoryWorkerObserved)
			{
				// Observation runs before the disabled-factory phase updater. When
				// production and construction begin in the same frame, validate and
				// record the exact resumed worker here. The original paid entry must
				// be consumed, while a later ordinary resource-worker ID remains a
				// valid post-ownership economy order.
				if (FindSkirmishAIRecoveryProductionEntryOnFactory(
						player, s_recovery.builderTemplate,
						s_recovery.builderFactoryID,
						s_recovery.recoveryBuilderProductionID))
				{
					FailSkirmishAITest("fixture_disabled_factory_queue_not_consumed");
					return FALSE;
				}
				s_recovery.disabledFactoryBuilderID = builder->getID();
				s_recovery.factoryWorkerObserved = TRUE;
				printf("SKIRMISH_AI_RECOVERY_PHASE phase=builder_production_resumed "
					"frame=%u factory=%u production_id=%d builder=%u\n",
					TheGameLogic->getFrame(), s_recovery.builderFactoryID,
					static_cast<Int>(s_recovery.recoveryBuilderProductionID),
					builder->getID());
				fflush(stdout);
			}
			if (s_recovery.fixtureCase ==
					SKIRMISH_AI_RECOVERY_DISABLED_FACTORY &&
				builder->getID() != s_recovery.disabledFactoryBuilderID)
			{
				FailSkirmishAITest("fixture_disabled_factory_builder_not_reused");
				return FALSE;
			}
			if (s_recovery.fixtureCase == SKIRMISH_AI_RECOVERY_SURVIVING_BUILDER &&
				!IsSkirmishAIRecoveryBaselineBuilderID(builder->getID()))
			{
				FailSkirmishAITest("fixture_surviving_builder_not_reused");
				return FALSE;
			}
		}
		if (commandCenter->getConstructionPercent() > 0.0f)
		{
			if (!s_recovery.sawConstructionProgress)
				PrintSkirmishAIRecoveryScaffoldDiagnostics(
					player, commandCenter, "first_scaffold_progress");
			s_recovery.sawConstructionProgress = TRUE;
		}
		if (s_recovery.fixtureCase == SKIRMISH_AI_RECOVERY_OBSTRUCTED &&
			IsDifferentSkirmishAIRecoveryPosition(
				s_recovery.originalBuildPosition,
				*commandCenter->getPosition()))
		{
			// The scaffold itself occupies the candidate footprint now. The
			// BuildAssistant enemy-only query must therefore be separate from
			// terrain checks; its collision helper treats combined flags as the
			// full friendly-overlap policy. The observed owned scaffold already
			// proves that the recovery builder reached this location.
			// The setup separately proves the original footprint was blocked by
			// the moved immobile obstruction under NO_OBJECT_OVERLAP.
			const LegalBuildCode alternateOverlapCode = builder && TheBuildAssistant
				? TheBuildAssistant->isLocationLegalToBuild(
					commandCenter->getPosition(), s_recovery.primaryTemplate,
					commandCenter->getOrientation(),
					BuildAssistant::NO_ENEMY_OBJECT_OVERLAP,
					builder, player)
				: LBC_GENERIC_FAILURE;
			const LegalBuildCode alternateTerrainCode = builder && TheBuildAssistant
				? TheBuildAssistant->isLocationLegalToBuild(
					commandCenter->getPosition(), s_recovery.primaryTemplate,
					commandCenter->getOrientation(),
					BuildAssistant::TERRAIN_RESTRICTIONS,
					builder, player)
				: LBC_GENERIC_FAILURE;
			if (alternateOverlapCode == LBC_OK && alternateTerrainCode == LBC_OK)
				s_recovery.sawAlternatePlacement = TRUE;
		}
	}

	if (commandCenter && !commandCenterUnderConstruction &&
		commandCenter->getID() != s_recovery.initialCenterID &&
		commandCenter->getID() != s_recovery.lastCompletedCenterID)
	{
		s_recovery.lastCompletedCenterID = commandCenter->getID();
		++s_recovery.recoveryCompletionCount;
		s_recovery.sawCompletedRecovery = TRUE;
		if (s_recovery.fixtureCase == SKIRMISH_AI_RECOVERY_OBSTRUCTED &&
			IsDifferentSkirmishAIRecoveryPosition(
				s_recovery.originalBuildPosition,
				*commandCenter->getPosition()))
			s_recovery.sawAlternatePlacement = TRUE;
		if (s_recovery.fixtureCase == SKIRMISH_AI_RECOVERY_OBSTRUCTED)
		{
			Coord3D obstructionPosition;
			obstructionPosition.zero();
			Object *obstruction = s_recovery.obstructionID != INVALID_ID
				? TheGameLogic->findObjectByID(s_recovery.obstructionID) : nullptr;
			if (obstruction && obstruction->getPosition())
				obstructionPosition = *obstruction->getPosition();
			printf("SKIRMISH_AI_RECOVERY_PHASE phase=command_center_complete "
				"frame=%u center=%u completions=%d builders=%d "
				"original=(%g,%g,%g) rebuilt=(%g,%g,%g) "
				"obstruction=(%g,%g,%g) expected_obstruction=(%g,%g,%g)\n",
				TheGameLogic->getFrame(), commandCenter->getID(),
				s_recovery.recoveryCompletionCount, builderCount,
				s_recovery.originalBuildPosition.x,
				s_recovery.originalBuildPosition.y,
				s_recovery.originalBuildPosition.z,
				commandCenter->getPosition()->x, commandCenter->getPosition()->y,
				commandCenter->getPosition()->z, obstructionPosition.x,
				obstructionPosition.y, obstructionPosition.z,
				s_recovery.obstructionBlockedPosition.x,
				s_recovery.obstructionBlockedPosition.y,
				s_recovery.obstructionBlockedPosition.z);
		}
		else
			printf("SKIRMISH_AI_RECOVERY_PHASE phase=command_center_complete frame=%u center=%u "
				"completions=%d builders=%d\n",
				TheGameLogic->getFrame(), commandCenter->getID(),
				s_recovery.recoveryCompletionCount, builderCount);
		fflush(stdout);
	}

	if (s_recovery.fixtureCase == SKIRMISH_AI_RECOVERY_NO_PATH_LASTSTAND &&
		TheGameLogic->getFrame() > s_recovery.phaseStartFrame)
	{
		Bool huntEvidence = FALSE;
		if (CountSkirmishAIRecoveryLastStandUnits(player, &huntEvidence) > 0 &&
			huntEvidence)
		{
			if (!s_recovery.sawLastStand)
			{
				printf("SKIRMISH_AI_RECOVERY_PHASE phase=last_stand_response frame=%u "
					"baseline_combat_units=%d\n",
					TheGameLogic->getFrame(), s_recovery.baselineCombatIDCount);
				fflush(stdout);
			}
			s_recovery.sawLastStand = TRUE;
		}
	}

	s_recovery.lastObservedCash = cash;
	return TRUE;
}

Bool UpdateSkirmishAIRecoveryDisabledFactoryFixture(Player *player)
{
	if (!player || s_recovery.fixtureCase !=
		SKIRMISH_AI_RECOVERY_DISABLED_FACTORY || !TheGameLogic)
		return TRUE;

	Object *factory = s_recovery.builderFactoryID != INVALID_ID
		? TheGameLogic->findObjectByID(s_recovery.builderFactoryID) : nullptr;
	if (!IsLiveSkirmishAIRecoveryObject(factory) ||
		factory->getControllingPlayer() != player ||
		!factory->getProductionUpdateInterface())
	{
		FailSkirmishAITest("fixture_disabled_factory_lost");
		return FALSE;
	}

	const UnsignedInt frame = TheGameLogic->getFrame();
	if (!s_recovery.factoryDisabled)
	{
		if (!s_recovery.sawBuilderQueuePayment)
			return TRUE;
		const ProductionEntry *entry =
			FindSkirmishAIRecoveryProductionEntryOnFactory(
				player, s_recovery.builderTemplate,
				s_recovery.builderFactoryID,
				s_recovery.recoveryBuilderProductionID);
		if (!entry || CountSkirmishAIRecoveryBuilderQueueEntries(
				player, s_recovery.builderTemplate, nullptr) != 1)
		{
			FailSkirmishAITest("fixture_disabled_factory_paid_queue_missing");
			return FALSE;
		}
		s_recovery.disabledFactoryProductionPercent =
			entry->getPercentComplete();
		s_recovery.disabledFactoryCash = player->getMoney()->countMoney();
		s_recovery.disabledFactoryBuilderCount =
			CountSkirmishAIRecoveryBuildersProducedByFactory(
				player, s_recovery.builderTemplate,
				s_recovery.builderFactoryID);
		s_recovery.disabledFactoryBlockUntilFrame = frame +
			SKIRMISH_AI_RECOVERY_DISABLED_FACTORY_VERIFY_FRAMES;
		factory->setScriptStatus(OBJECT_STATUS_SCRIPT_DISABLED, TRUE);
		if (!factory->testScriptStatusBit(OBJECT_STATUS_SCRIPT_DISABLED) ||
			!factory->isDisabled())
		{
			FailSkirmishAITest("fixture_disabled_factory_status_not_applied");
			return FALSE;
		}
		s_recovery.factoryDisabled = TRUE;
		printf("SKIRMISH_AI_RECOVERY_PHASE phase=factory_disabled frame=%u "
			"factory=%u production_id=%d percent=%g cash=%u verify_until=%u\n",
			frame, s_recovery.builderFactoryID,
			static_cast<Int>(s_recovery.recoveryBuilderProductionID),
			s_recovery.disabledFactoryProductionPercent,
			s_recovery.disabledFactoryCash,
			s_recovery.disabledFactoryBlockUntilFrame);
		fflush(stdout);
		return TRUE;
	}

	if (!s_recovery.factoryBlockVerified)
	{
		const ProductionEntry *entry =
			FindSkirmishAIRecoveryProductionEntryOnFactory(
				player, s_recovery.builderTemplate,
				s_recovery.builderFactoryID,
				s_recovery.recoveryBuilderProductionID);
		const Int factoryQueueCount =
			CountSkirmishAIRecoveryBuilderQueueEntriesOnFactory(
				player, s_recovery.builderTemplate,
				s_recovery.builderFactoryID);
		const Int globalQueueCount =
			CountSkirmishAIRecoveryBuilderQueueEntries(
				player, s_recovery.builderTemplate, nullptr);
		const Int producedBuilderCount =
			CountSkirmishAIRecoveryBuildersProducedByFactory(
				player, s_recovery.builderTemplate,
				s_recovery.builderFactoryID);
		Int commandCenterCount = 0;
		Bool commandCenterUnderConstruction = FALSE;
		FindSkirmishAIRecoveryCommandCenter(
			player, s_recovery.primaryTemplate, &commandCenterCount,
			&commandCenterUnderConstruction);
		const UnsignedInt cash = player->getMoney()->countMoney();
		if (!factory->testScriptStatusBit(OBJECT_STATUS_SCRIPT_DISABLED) ||
			!factory->isDisabled() || !entry || factoryQueueCount != 1 ||
			globalQueueCount != 1 ||
			entry->getPercentComplete() !=
				s_recovery.disabledFactoryProductionPercent ||
			producedBuilderCount != s_recovery.disabledFactoryBuilderCount ||
			CountSkirmishAIRecoveryBuilders(player, nullptr) != 0 ||
			commandCenterCount != 0 || commandCenterUnderConstruction ||
			s_recovery.constructionScaffoldCount != 0 ||
			cash < s_recovery.disabledFactoryCash)
		{
			PrintSkirmishAIRecoveryFactoryDiagnostics(
				player, "disabled_factory_block_changed");
			FailSkirmishAITest("fixture_disabled_factory_block_changed");
			return FALSE;
		}

		// Neutralize deterministic resource income after proving no debit. This
		// keeps the restored producer at the exact command-center reserve.
		if (cash != s_recovery.disabledFactoryCash)
			SetSkirmishAIRecoveryCash(
				player, s_recovery.disabledFactoryCash);
		const UnsignedInt recoverySpendProbeCash =
			player->getMoney()->countMoney();
		if (recoverySpendProbeCash > static_cast<UnsignedInt>(INT_MAX))
		{
			FailSkirmishAITest("fixture_disabled_factory_cash_out_of_probe_range");
			return FALSE;
		}
		const Int recoverySpendProbeCost =
			static_cast<Int>(recoverySpendProbeCash);
#if RTS_ZEROHOUR
		const Bool recoverySpendAllowed =
			player->canSpendForSkirmishAIRecovery(
				recoverySpendProbeCost, nullptr, FALSE);
#else
		// The disabled-factory fixture exercises Zero Hour-only recovery state.
		// Keep the shared runner buildable for Generals without inventing a
		// product-side reserve policy for that title.
		const Bool recoverySpendAllowed = FALSE;
#endif
		if (!recoverySpendAllowed)
			s_recovery.factoryReserveHeldObserved = TRUE;
		if (frame < s_recovery.disabledFactoryBlockUntilFrame)
			return TRUE;
		if (!s_recovery.factoryReserveHeldObserved || !recoverySpendAllowed)
		{
			PrintSkirmishAIRecoveryFactoryDiagnostics(
				player, "disabled_factory_reserve_not_released");
			FailSkirmishAITest("fixture_disabled_factory_reserve_not_released");
			return FALSE;
		}
		s_recovery.factoryReserveReleasedObserved = TRUE;
		printf("SKIRMISH_AI_RECOVERY_PHASE "
			"phase=post_grace_reserve_released frame=%u factory=%u "
			"production_id=%d reserve_held=1 spend_probe_cost=%d "
			"spend_probe_allowed=1 queue=1 percent=%g scaffolds=0\n",
			frame, s_recovery.builderFactoryID,
			static_cast<Int>(s_recovery.recoveryBuilderProductionID),
			recoverySpendProbeCost,
			s_recovery.disabledFactoryProductionPercent);

		s_recovery.factoryBlockVerified = TRUE;
		printf("SKIRMISH_AI_RECOVERY_PHASE phase=factory_block_verified "
			"frame=%u factory=%u production_id=%d percent=%g queue=1 "
			"workers=%d scaffolds=0 additional_payment=0 blocked_frames=%u "
			"post_grace_reserve_release=verified\n",
			frame, s_recovery.builderFactoryID,
			static_cast<Int>(s_recovery.recoveryBuilderProductionID),
			s_recovery.disabledFactoryProductionPercent,
			s_recovery.disabledFactoryBuilderCount,
			SKIRMISH_AI_RECOVERY_DISABLED_FACTORY_VERIFY_FRAMES);
		factory->clearScriptStatus(OBJECT_STATUS_SCRIPT_DISABLED);
		if (factory->testScriptStatusBit(OBJECT_STATUS_SCRIPT_DISABLED) ||
			factory->isDisabledByType(DISABLED_SCRIPT_DISABLED))
		{
			FailSkirmishAITest("fixture_disabled_factory_status_not_cleared");
			return FALSE;
		}
		s_recovery.factoryRestored = TRUE;
		printf("SKIRMISH_AI_RECOVERY_PHASE phase=factory_restored frame=%u "
			"factory=%u production_id=%d cash=%u\n", frame,
			s_recovery.builderFactoryID,
			static_cast<Int>(s_recovery.recoveryBuilderProductionID),
			player->getMoney()->countMoney());
		fflush(stdout);
		return TRUE;
	}

	if (!s_recovery.factoryWorkerObserved)
	{
		const ObjectID builderID = FindSkirmishAIRecoveryBuilderProducedByFactory(
			player, s_recovery.builderTemplate, s_recovery.builderFactoryID);
		if (builderID == INVALID_ID)
			return TRUE;
		if (FindSkirmishAIRecoveryProductionEntryOnFactory(
				player, s_recovery.builderTemplate,
				s_recovery.builderFactoryID,
				s_recovery.recoveryBuilderProductionID))
		{
			FailSkirmishAITest("fixture_disabled_factory_queue_not_consumed");
			return FALSE;
		}
		s_recovery.disabledFactoryBuilderID = builderID;
		s_recovery.factoryWorkerObserved = TRUE;
		printf("SKIRMISH_AI_RECOVERY_PHASE phase=builder_production_resumed "
			"frame=%u factory=%u production_id=%d builder=%u\n", frame,
			s_recovery.builderFactoryID,
			static_cast<Int>(s_recovery.recoveryBuilderProductionID), builderID);
		fflush(stdout);
	}
	return TRUE;
}

Bool FinishSkirmishAIRecoveryFixture();

Bool FinishSkirmishAIRecoveryHoleFixture()
{
	if (!s_recovery.holeObserved || !s_recovery.holeLineageObserved ||
		s_recovery.holeReconstructionID == INVALID_ID ||
		s_recovery.lastCompletedCenterID != s_recovery.holeReconstructionID)
	{
		FailSkirmishAITest(s_recovery.holeObserved
			? "fixture_gla_hole_lineage_not_observed"
			: "fixture_gla_hole_not_observed");
		return FALSE;
	}

	return FinishSkirmishAIRecoveryFixture();
}

Bool SaveAndLoadSkirmishAIRecoveryFixture(Player *player, Object *construction)
{
	if (!player || !construction || !TheGameState)
	{
		FailSkirmishAITest("fixture_save_load_api_unavailable");
		return FALSE;
	}

	AsciiString filename;
	filename.format("SkirmishAIRecovery_%d.sav", s_runner.seed);
	const AsciiString primaryTemplateName = s_recovery.primaryTemplate
		? s_recovery.primaryTemplate->getName() : AsciiString::TheEmptyString;
	const AsciiString builderTemplateName = s_recovery.builderTemplate
		? s_recovery.builderTemplate->getName() : AsciiString::TheEmptyString;
	UnicodeString description;
	description.set(L"Stage 1 recovery pending construction");
	SaveResult saveResult = TheGameState->saveGame(
		filename, description, SAVE_FILE_TYPE_NORMAL);
	if (saveResult.saveCode != SC_OK || saveResult.filename.isEmpty())
	{
		FailSkirmishAITest("fixture_save_failed");
		return FALSE;
	}

	AvailableGameInfo gameInfo;
	gameInfo.filename = saveResult.filename;
	gameInfo.next = nullptr;
	gameInfo.prev = nullptr;
	gameInfo.saveGameInfo = *TheGameState->getSaveGameInfo();
	gameInfo.saveGameInfo.saveFileType = SAVE_FILE_TYPE_NORMAL;
	const ObjectID constructionID = construction->getID();
	const Real constructionPercent = construction->getConstructionPercent();
	if (constructionPercent < SKIRMISH_AI_RECOVERY_SAVE_LOAD_MIN_PROGRESS)
	{
		FailSkirmishAITest("fixture_save_load_progress_not_ready");
		return FALSE;
	}
	const SaveCode loadResult = TheGameState->loadGame(gameInfo);
	if (loadResult != SC_OK)
	{
		FailSkirmishAITest("fixture_load_failed");
		return FALSE;
	}

	// GameState::loadGame() resets and reconstructs runtime objects. Rebind
	// every subject pointer through the canonical template database and reacquire
	// the player before inspecting cash or construction state.
	Player *reboundPlayer = GetSkirmishAIRecoveryFixturePlayer();
	if (!reboundPlayer || !TheThingFactory || primaryTemplateName.isEmpty() ||
		builderTemplateName.isEmpty())
	{
		FailSkirmishAITest("fixture_save_load_rebind_state_unavailable");
		return FALSE;
	}
	const ThingTemplate *reboundPrimaryTemplate =
		TheThingFactory->findTemplate(primaryTemplateName);
	const ThingTemplate *reboundBuilderTemplate =
		TheThingFactory->findTemplate(builderTemplateName);
	if (!reboundPrimaryTemplate || !reboundBuilderTemplate ||
		!reboundPrimaryTemplate->isKindOf(KINDOF_COMMANDCENTER) ||
		!reboundBuilderTemplate->isKindOf(KINDOF_DOZER) ||
		!HasSkirmishAIRecoveryBuilderTemplate(reboundPlayer, reboundBuilderTemplate))
	{
		FailSkirmishAITest("fixture_save_load_template_rebind_failed");
		return FALSE;
	}
	Int reboundCenterCount = 0;
	Bool reboundUnderConstruction = FALSE;
	Object *reboundConstruction = FindSkirmishAIRecoveryCommandCenter(
		reboundPlayer, reboundPrimaryTemplate, &reboundCenterCount,
		&reboundUnderConstruction);
	if (!reboundConstruction || reboundCenterCount != 1 || !reboundUnderConstruction ||
		reboundConstruction->getID() != constructionID ||
		reboundConstruction->getConstructionPercent() +
		SKIRMISH_AI_RECOVERY_SAVE_LOAD_PROGRESS_TOLERANCE < constructionPercent)
	{
		FailSkirmishAITest("fixture_save_load_progress_reset");
		return FALSE;
	}
	s_recovery.primaryTemplate = reboundPrimaryTemplate;
	s_recovery.builderTemplate = reboundBuilderTemplate;
	s_recovery.saveLoadFilename = saveResult.filename;
	s_recovery.saveLoadConstructionID = constructionID;
	s_recovery.saveLoadConstructionPercent = constructionPercent;
	s_recovery.saveLoadIssued = TRUE;
	s_recovery.saveLoadProgressPreserved = TRUE;
	s_recovery.phase = SKIRMISH_AI_RECOVERY_PHASE_SAVE_LOAD_REBOUND;
	s_recovery.phaseStartFrame = TheGameLogic->getFrame();
	s_recovery.lastObservedCash = reboundPlayer->getMoney()->countMoney();
	printf("SKIRMISH_AI_RECOVERY_PHASE phase=save_load_complete frame=%u file=%s "
		"construction=%u saved_percent=%g reloaded_percent=%g cash=%u\n",
		TheGameLogic->getFrame(), s_recovery.saveLoadFilename.str(), constructionID,
		constructionPercent, reboundConstruction->getConstructionPercent(),
		s_recovery.lastObservedCash);
	fflush(stdout);
	return TRUE;
}

Bool TryInjectSkirmishAIRecoverySecondBuilderLoss(
	Player *player, Object *commandCenter)
{
	if (!player || !commandCenter || !TheGameLogic)
	{
		FailSkirmishAITest("fixture_second_builder_loss_target_missing");
		return FALSE;
	}

	Object *builder = TheGameLogic->findObjectByID(commandCenter->getBuilderID());
	if (!builder || !IsLiveSkirmishAIRecoveryObject(builder) ||
		builder->isContained() ||
		builder->getControllingPlayer() != player)
	{
		FailSkirmishAITest("fixture_second_builder_loss_target_missing");
		return FALSE;
	}

	Object *spareBuilder = FindSkirmishAIRecoverySpareBaselineBuilder(
		player, builder->getID());
	Object *routeFactory = spareBuilder ? nullptr
		: FindSkirmishAIRecoverySecondBuilderFactory(player);
	if (!spareBuilder && !routeFactory)
	{
		FailSkirmishAITest("fixture_second_builder_loss_route_missing");
		return FALSE;
	}

	s_recovery.secondBuilderLossID = builder->getID();
	s_recovery.secondBuilderLossConstructionID = commandCenter->getID();
	s_recovery.secondBuilderRouteFactoryID = routeFactory
		? routeFactory->getID() : INVALID_ID;
	s_recovery.secondBuilderRouteProductionID = PRODUCTIONID_INVALID;
	s_recovery.secondBuilderRouteCashBefore = 0;
	s_recovery.secondBuilderRouteCashAfter = 0;
	s_recovery.secondBuilderRoutePaid = spareBuilder != nullptr;
	DestroySkirmishAIRecoveryObject(builder);
	if (IsLiveSkirmishAIRecoveryObject(
		TheGameLogic->findObjectByID(s_recovery.secondBuilderLossID)))
	{
		FailSkirmishAITest("fixture_second_builder_loss_not_observed");
		return FALSE;
	}

	s_recovery.secondBuilderLossIssued = TRUE;
	s_recovery.secondBuilderLossObserved = TRUE;
	printf("SKIRMISH_AI_RECOVERY_PHASE phase=second_builder_loss_applied frame=%u "
		"builder=%u route=%s factory=%u\n",
		TheGameLogic->getFrame(), s_recovery.secondBuilderLossID,
		routeFactory ? "factory" : "baseline",
		s_recovery.secondBuilderRouteFactoryID);
	fflush(stdout);
	return TRUE;
}

Bool ObserveSkirmishAIRecoverySecondBuilderRoutePayment(
	Player *player, UnsignedInt cash)
{
	if (!player || !s_recovery.secondBuilderLossIssued ||
		s_recovery.secondBuilderRouteFactoryID == INVALID_ID ||
		s_recovery.secondBuilderRoutePaid)
		return TRUE;

	const Int queueCount = CountSkirmishAIRecoveryBuilderQueueEntriesOnFactory(
		player, s_recovery.builderTemplate,
		s_recovery.secondBuilderRouteFactoryID);
	if (queueCount == 0)
		return TRUE;
	if (queueCount > 1)
	{
		PrintSkirmishAIRecoveryFactoryDiagnostics(player,
			"second_builder_duplicate_queue");
		FailSkirmishAITest("fixture_second_builder_duplicate_queue");
		return FALSE;
	}

	const ProductionID productionID = FindSkirmishAIRecoveryBuilderQueueIDOnFactory(
		player, s_recovery.builderTemplate,
		s_recovery.secondBuilderRouteFactoryID);
	if (productionID == PRODUCTIONID_INVALID)
		return TRUE;
	s_recovery.secondBuilderRouteProductionID = productionID;
	s_recovery.secondBuilderRouteCashBefore = s_recovery.lastObservedCash;
	s_recovery.secondBuilderRouteCashAfter = cash;
	if (s_recovery.secondBuilderRouteCashBefore <=
		s_recovery.secondBuilderRouteCashAfter ||
		s_recovery.secondBuilderRouteCashBefore -
		s_recovery.secondBuilderRouteCashAfter <
		s_recovery.builderCost)
	{
		FailSkirmishAITest("fixture_second_builder_unpaid");
		return FALSE;
	}
	s_recovery.secondBuilderRoutePaid = TRUE;
	printf("SKIRMISH_AI_RECOVERY_PHASE phase=second_builder_paid frame=%u "
		"factory=%u production_id=%d cash_before=%u cash_after=%u\n",
		TheGameLogic->getFrame(), s_recovery.secondBuilderRouteFactoryID,
		static_cast<Int>(productionID),
		s_recovery.secondBuilderRouteCashBefore,
		s_recovery.secondBuilderRouteCashAfter);
	fflush(stdout);
	return TRUE;
}

void UpdateSkirmishAIRecoveryHoleFixture(Player *player)
{
	if (TheGameLogic->getFrame() - s_recovery.phaseStartFrame >
		SKIRMISH_AI_RECOVERY_PHASE_TIMEOUT_FRAMES)
	{
		FailSkirmishAITest("fixture_gla_hole_timeout");
		RequestSkirmishAITestStop();
		return;
	}

	Int holeCount = 0;
	Object *hole = FindSkirmishAIRecoveryHole(
		player, s_recovery.initialCenterID, &holeCount);
	if (holeCount > 1)
	{
		FailSkirmishAITest("fixture_duplicate_gla_hole");
		RequestSkirmishAITestStop();
		return;
	}
	if (hole && !s_recovery.holeObserved)
	{
		s_recovery.holeObserved = TRUE;
		s_recovery.holeID = hole->getID();
		printf("SKIRMISH_AI_RECOVERY_HOLE_PHASE phase=hole_verified frame=%u hole=%u "
			"spawner=%u\n",
			TheGameLogic->getFrame(), s_recovery.holeID, s_recovery.initialCenterID);
		fflush(stdout);
	}

	// Keep running the ordinary observation path after the real hole appears.
	// This checks duplicate centers every frame and records the native hole
	// worker's construction/progress before allowing the fixture to finish.
	if (!ObserveSkirmishAIRecoveryFixture(player))
	{
		RequestSkirmishAITestStop();
		return;
	}
	if (s_recovery.holeObserved && s_recovery.sawCompletedRecovery &&
		!FinishSkirmishAIRecoveryHoleFixture())
		RequestSkirmishAITestStop();
}

Bool FinishSkirmishAIRecoveryFixture()
{
	if (!s_recovery.sawConstruction || !s_recovery.sawConstructionProgress ||
		!s_recovery.sawConstructionOwnership || !s_recovery.sawCompletedRecovery)
	{
		FailSkirmishAITest("fixture_incomplete_construction_observation");
		return FALSE;
	}
	if (IsSkirmishAIRecoveryFactoryFixture() &&
		!s_recovery.sawBuilderQueuePayment)
	{
		FailSkirmishAITest("fixture_builder_queue_not_observed");
		return FALSE;
	}
	if (s_recovery.fixtureCase == SKIRMISH_AI_RECOVERY_DISABLED_FACTORY &&
		(!s_recovery.factoryDisabled || !s_recovery.factoryBlockVerified ||
		 !s_recovery.factoryRestored || !s_recovery.factoryWorkerObserved ||
		 !s_recovery.factoryReserveHeldObserved ||
		 !s_recovery.factoryReserveReleasedObserved ||
		 s_recovery.disabledFactoryBuilderID == INVALID_ID ||
		 s_recovery.lastConstructionBuilderID !=
			s_recovery.disabledFactoryBuilderID))
	{
		FailSkirmishAITest("fixture_disabled_factory_incomplete");
		return FALSE;
	}
	if (s_recovery.fixtureCase == SKIRMISH_AI_RECOVERY_OBSTRUCTED &&
		(!s_recovery.obstructionOriginalLocationBlocked ||
			!s_recovery.obstructionControlLocationLegal ||
			!s_recovery.sawAlternatePlacement))
	{
		FailSkirmishAITest("fixture_obstruction_not_bypassed");
		return FALSE;
	}
	if (s_recovery.fixtureCase == SKIRMISH_AI_RECOVERY_LOW_CASH &&
		!s_recovery.moneyReleased)
	{
		FailSkirmishAITest("fixture_low_cash_release_missing");
		return FALSE;
	}
	if (s_recovery.fixtureCase == SKIRMISH_AI_RECOVERY_SAVE_LOAD &&
		(!s_recovery.saveLoadRebound || !s_recovery.saveLoadProgressPreserved ||
			!s_recovery.saveLoadSawProgress))
	{
		FailSkirmishAITest("fixture_save_load_rebind_incomplete");
		return FALSE;
	}
	if (s_recovery.fixtureCase == SKIRMISH_AI_RECOVERY_REPEATED_COMMAND_CENTER &&
		s_recovery.recoveryCompletionCount < 2)
	{
		FailSkirmishAITest("fixture_repeated_recovery_incomplete");
		return FALSE;
	}
	if (s_recovery.fixtureCase == SKIRMISH_AI_RECOVERY_REPEATED_COMMAND_CENTER &&
		s_recovery.secondBuilderLossSkipped)
	{
		FailSkirmishAITest("fixture_repeated_builder_loss_skipped");
		return FALSE;
	}
	if (s_recovery.fixtureCase == SKIRMISH_AI_RECOVERY_REPEATED_COMMAND_CENTER &&
		s_recovery.secondBuilderLossIssued &&
		(!s_recovery.secondBuilderLossObserved ||
			!s_recovery.secondBuilderReplacementObserved ||
			(s_recovery.secondBuilderRouteFactoryID != INVALID_ID &&
				!s_recovery.secondBuilderRoutePaid)))
	{
		FailSkirmishAITest("fixture_second_builder_loss_incomplete");
		return FALSE;
	}

	s_recovery.phase = SKIRMISH_AI_RECOVERY_PHASE_COMPLETE;
	printf("SKIRMISH_AI_RECOVERY_PHASE phase=assertions_passed frame=%u construction_scaffolds=%d "
		"rebuilds=%d duplicate_cc=0\n",
		TheGameLogic->getFrame(), s_recovery.constructionScaffoldCount,
		s_recovery.recoveryCompletionCount);
	fflush(stdout);
	s_runner.endFrame = TheGameLogic->getFrame();
	RequestSkirmishAITestStop();
	return TRUE;
}

void CountSkirmishAIInfrastructure(Player *player, Int *buildings,
	Int *power, Int *production, Int *renewableIncome)
{
	*buildings = *power = *production = *renewableIncome = 0;
	for (Object *object = TheGameLogic->getFirstObject(); object;
		object = object->getNextObject())
	{
		if (object->getControllingPlayer() != player ||
			!IsLiveSkirmishAIRecoveryObject(object) ||
			object->testStatus(OBJECT_STATUS_UNDER_CONSTRUCTION))
			continue;
		if (object->isKindOf(KINDOF_STRUCTURE) &&
			!object->isKindOf(KINDOF_COMMANDCENTER) &&
			!object->isKindOf(KINDOF_SUPPLY_SOURCE))
			++*buildings;
		if (object->isKindOf(KINDOF_FS_POWER) &&
			!object->isKindOf(KINDOF_CASH_GENERATOR))
			++*power;
		if (object->isKindOf(KINDOF_FS_BARRACKS) ||
			object->isKindOf(KINDOF_FS_WARFACTORY) ||
			object->isKindOf(KINDOF_FS_AIRFIELD) ||
			object->isKindOf(KINDOF_FS_FACTORY))
			++*production;
		if (object->isKindOf(KINDOF_FS_SUPPLY_DROPZONE) ||
			object->isKindOf(KINDOF_FS_BLACK_MARKET) ||
			(object->isKindOf(KINDOF_INFANTRY) &&
			 object->isKindOf(KINDOF_MONEY_HACKER) &&
			 object->getAIUpdateInterface() &&
			 object->getAIUpdateInterface()->getHackInternetAIInterface() &&
			 object->getAIUpdateInterface()->getHackInternetAIInterface()
				->isHacking()))
			++*renewableIncome;
	}
}

Bool HasRestoredSkirmishAIInfrastructureBaseline(Player *player)
{
	for (Int i = 0; i < s_recovery.infrastructureBaselineTemplateCount; ++i)
	{
		const ThingTemplate *plan = s_recovery.infrastructureBaselineTemplates[i];
		Int needed = 0;
		for (Int prior = 0; prior <= i; ++prior)
			if (s_recovery.infrastructureBaselineTemplates[prior] == plan)
				++needed;
		Int restored = 0;
		for (Object *object = TheGameLogic->getFirstObject(); object;
			object = object->getNextObject())
		{
			if (object->getControllingPlayer() == player &&
				IsLiveSkirmishAIRecoveryObject(object) &&
				!object->testStatus(OBJECT_STATUS_UNDER_CONSTRUCTION) &&
				object->getTemplate() == plan)
				++restored;
		}
		if (restored < needed)
			return FALSE;
	}
	return TRUE;
}

void PrintSkirmishAIInfrastructureIncomePlans(Player *player)
{
	Object *builder = nullptr;
	for (Object *object = TheGameLogic->getFirstObject(); object;
		object = object->getNextObject())
		if (object->getControllingPlayer() == player &&
			object->isKindOf(KINDOF_DOZER) &&
			IsLiveSkirmishAIRecoveryObject(object) && !object->isContained())
		{
			builder = object;
			break;
		}
	Int printed = 0;
	for (BuildListInfo *info = player->getBuildList(); info && printed < 12;
		info = info->getNext())
	{
		const ThingTemplate *plan = TheThingFactory->findTemplate(
			info->getTemplateName());
		if (!plan || (!plan->isKindOf(KINDOF_FS_SUPPLY_DROPZONE) &&
			!plan->isKindOf(KINDOF_FS_BLACK_MARKET)))
			continue;
		Object *built = TheGameLogic->findObjectByID(info->getObjectID());
		printf("SKIRMISH_AI_INCOME_PLAN template=%s priority=%d automatic=%d "
			"rebuilds=%d object=%u built=%d constructing=%d timestamp=%u "
			"can_make=%d site=(%g,%g)\n",
			plan->getName().str(), info->isPriorityBuild(),
			info->isAutomaticBuild(), info->getNumRebuilds(),
			info->getObjectID(), built != nullptr,
			built && built->testStatus(OBJECT_STATUS_UNDER_CONSTRUCTION),
			info->getObjectTimestamp(), builder
				? TheBuildAssistant->canMakeUnit(builder, plan) : -1,
			info->getLocation()->x, info->getLocation()->y);
		++printed;
	}
	fflush(stdout);
}

void ApplySkirmishAIRecoveryFixtureFault(Player *player)
{
	if (!player || !s_recovery.primaryTemplate || !s_recovery.builderTemplate)
	{
		FailSkirmishAITest("fixture_fault_state_unavailable");
		RequestSkirmishAITestStop();
		return;
	}

	const UnsignedInt frame = TheGameLogic->getFrame();
	Object *commandCenter = nullptr;
	if (!s_recovery.faultApplied)
	{
		Int commandCenterCount = 0;
		Bool commandCenterUnderConstruction = FALSE;
		commandCenter = FindSkirmishAIRecoveryCommandCenter(
			player, s_recovery.primaryTemplate, &commandCenterCount,
			&commandCenterUnderConstruction);
		if (!commandCenter || commandCenterUnderConstruction || commandCenterCount != 1)
		{
			FailSkirmishAITest("fixture_baseline_command_center_lost");
			RequestSkirmishAITestStop();
			return;
		}

		if (IsSkirmishAIRecoveryFactoryFixture())
		{
			Object *factory = nullptr;
			if (CountSkirmishAIRecoveryBuilderFactories(
					player, s_recovery.builderTemplate, commandCenter, &factory) == 0 ||
				!factory)
			{
				FailSkirmishAITest("fixture_builder_factory_unavailable");
				RequestSkirmishAITestStop();
				return;
			}
			s_recovery.builderFactoryID = factory->getID();
			s_recovery.initialFactoryBuilderCount =
				CountSkirmishAIRecoveryBuildersProducedByFactory(
					player, s_recovery.builderTemplate, s_recovery.builderFactoryID);
		}

		if (s_recovery.fixtureCase == SKIRMISH_AI_RECOVERY_OBSTRUCTED)
		{
			Object *obstruction = FindSkirmishAIRecoveryObstruction(player, commandCenter);
			if (!obstruction || !obstruction->getPosition())
			{
				FailSkirmishAITest("fixture_obstruction_unavailable");
				RequestSkirmishAITestStop();
				return;
			}
			s_recovery.obstructionID = obstruction->getID();
			s_recovery.obstructionOriginalPosition = *obstruction->getPosition();
		}
		if (s_recovery.fixtureCase == SKIRMISH_AI_RECOVERY_SURVIVING_BUILDER ||
			s_recovery.fixtureCase == SKIRMISH_AI_RECOVERY_INFRASTRUCTURE_COLLAPSE)
		{
			// Isolate surviving_builder's baseline-builder reuse assertion without
			// adding a free faction building that changes power, production, or
			// placement. The low_cash fixture separately covers recovery under the
			// stock victory rule with a real non-command-center victory structure.
			const Int priorVictoryConditions =
				TheVictoryConditions->getVictoryConditions();
			if (priorVictoryConditions != VICTORY_NOBUILDINGS)
			{
				printf("SKIRMISH_AI_RECOVERY_DIAGNOSTIC "
					"reason=unexpected_victory_conditions frame=%u conditions=%d\n",
					frame, priorVictoryConditions);
				fflush(stdout);
				FailSkirmishAITest("fixture_unexpected_victory_conditions");
				RequestSkirmishAITestStop();
				return;
			}
			const Int fixtureVictoryConditions =
				VICTORY_NOBUILDINGS | VICTORY_NOUNITS;
			TheVictoryConditions->setVictoryConditions(fixtureVictoryConditions);
			printf("SKIRMISH_AI_RECOVERY_PHASE phase=victory_conditions_overridden "
				"frame=%u prior_conditions=%d conditions=%d\n", frame,
				priorVictoryConditions, fixtureVictoryConditions);
			fflush(stdout);
		}
		if (s_recovery.fixtureCase == SKIRMISH_AI_RECOVERY_INFRASTRUCTURE_COLLAPSE)
		{
			ObjectID survivingBuilderID = INVALID_ID;
			for (Object *baselineBuilder = TheGameLogic->getFirstObject(); baselineBuilder;
				baselineBuilder = baselineBuilder->getNextObject())
			{
				if (baselineBuilder->getControllingPlayer() == player &&
					baselineBuilder->isKindOf(KINDOF_DOZER) &&
					IsLiveSkirmishAIRecoveryObject(baselineBuilder) &&
					!baselineBuilder->isContained() &&
					(survivingBuilderID == INVALID_ID ||
					 baselineBuilder->getID() < survivingBuilderID))
					survivingBuilderID = baselineBuilder->getID();
			}
			if (survivingBuilderID == INVALID_ID)
			{
				FailSkirmishAITest("fixture_single_builder_unavailable");
				RequestSkirmishAITestStop();
				return;
			}
			for (Object *extraBuilder = TheGameLogic->getFirstObject(); extraBuilder; )
			{
				Object *next = extraBuilder->getNextObject();
				if (extraBuilder->getControllingPlayer() == player &&
					extraBuilder->isKindOf(KINDOF_DOZER) &&
					extraBuilder->getID() != survivingBuilderID &&
					IsLiveSkirmishAIRecoveryObject(extraBuilder))
					DestroySkirmishAIRecoveryObject(extraBuilder);
				extraBuilder = next;
			}
			// Exhaust the starting supply field too, so restored income must
			// come from hackers, drop zones, or markets rather than collectors.
			for (Object *supplyObject = TheGameLogic->getFirstObject(); supplyObject; )
			{
				Object *next = supplyObject->getNextObject();
				if (supplyObject->isKindOf(KINDOF_SUPPLY_SOURCE) &&
					IsLiveSkirmishAIRecoveryObject(supplyObject) &&
					supplyObject->getPosition())
				{
					const Real dx = supplyObject->getPosition()->x -
						s_recovery.originalCenterPosition.x;
					const Real dy = supplyObject->getPosition()->y -
						s_recovery.originalCenterPosition.y;
					if (dx * dx + dy * dy < 700.0f * 700.0f)
					{
						DestroySkirmishAIRecoveryObject(supplyObject);
						++s_recovery.depletedSupplySources;
					}
				}
				supplyObject = next;
			}
			if (s_recovery.depletedSupplySources == 0)
			{
				FailSkirmishAITest("fixture_supply_field_unavailable");
				RequestSkirmishAITestStop();
				return;
			}
			// Remove every completed base building while keeping the surviving
			// builders. Exhaust their ordinary build-list allowances as well.
			for (Object *baseStructure = TheGameLogic->getFirstObject(); baseStructure; )
			{
				Object *next = baseStructure->getNextObject();
				if (baseStructure != commandCenter &&
					baseStructure->getControllingPlayer() == player &&
					baseStructure->isKindOf(KINDOF_STRUCTURE) &&
					!baseStructure->isKindOf(KINDOF_SUPPLY_SOURCE) &&
					IsLiveSkirmishAIRecoveryObject(baseStructure))
				{
					if (!baseStructure->testStatus(OBJECT_STATUS_UNDER_CONSTRUCTION) &&
						!baseStructure->isKindOf(KINDOF_FS_SUPPLY_CENTER) &&
						baseStructure->getTemplate() &&
						s_recovery.infrastructureBaselineTemplateCount <
							SKIRMISH_AI_RECOVERY_DIAGNOSTIC_MAX_IDS)
						s_recovery.infrastructureBaselineTemplates[
							s_recovery.infrastructureBaselineTemplateCount++] =
							baseStructure->getTemplate();
					DestroySkirmishAIRecoveryObject(baseStructure);
					++s_recovery.destructionCount;
				}
				baseStructure = next;
			}
			for (BuildListInfo *buildInfo = player->getBuildList();
				buildInfo; buildInfo = buildInfo->getNext())
			{
				const ThingTemplate *plan = TheThingFactory->findTemplate(
					buildInfo->getTemplateName());
				if (plan && !plan->isKindOf(KINDOF_COMMANDCENTER) &&
					(plan->isKindOf(KINDOF_FS_POWER) ||
					 plan->isKindOf(KINDOF_FS_BARRACKS) ||
					 plan->isKindOf(KINDOF_FS_WARFACTORY) ||
					 plan->isKindOf(KINDOF_FS_FACTORY) ||
					 plan->isKindOf(KINDOF_FS_SUPPLY_DROPZONE) ||
					 plan->isKindOf(KINDOF_FS_BLACK_MARKET)))
					buildInfo->setNumRebuilds(0);
			}
			SetSkirmishAIRecoveryCash(player, 30000);
			s_recovery.infrastructureFaulted = TRUE;
			printf("SKIRMISH_AI_INFRASTRUCTURE_FAULT frame=%u "
				"single_builder=%u depleted_supply_sources=%d\n",
				frame, survivingBuilderID,
				s_recovery.depletedSupplySources);
			fflush(stdout);
		}

#if RTS_ZEROHOUR
		if (s_recovery.fixtureCase == SKIRMISH_AI_RECOVERY_GLA_HOLE)
			commandCenter->kill(DAMAGE_UNRESISTABLE, DEATH_NORMAL);
		else
#endif
			DestroySkirmishAIRecoveryObject(commandCenter);
		++s_recovery.destructionCount;
		s_recovery.faultApplied = TRUE;
		if (s_recovery.fixtureCase == SKIRMISH_AI_RECOVERY_OBSTRUCTED)
		{
			s_recovery.phaseStartFrame = frame;
			printf("SKIRMISH_AI_RECOVERY_PHASE phase=obstruction_cleanup_pending "
				"frame=%u center=%u obstruction=%u\n",
				frame, s_recovery.initialCenterID, s_recovery.obstructionID);
			fflush(stdout);
			return;
		}
	}

	if (s_recovery.fixtureCase == SKIRMISH_AI_RECOVERY_OBSTRUCTED &&
		!s_recovery.obstructionPlaced)
	{
		if (TheGameLogic->findObjectByID(s_recovery.initialCenterID) != nullptr)
		{
			if (frame - s_recovery.phaseStartFrame >
				SKIRMISH_AI_RECOVERY_PHASE_TIMEOUT_FRAMES)
			{
				FailSkirmishAITest("fixture_obstruction_cleanup_timeout");
				RequestSkirmishAITestStop();
			}
			return;
		}

		Object *obstruction = TheGameLogic->findObjectByID(
			s_recovery.obstructionID);
		if (!IsLiveSkirmishAIRecoveryObject(obstruction) ||
			!obstruction->isStructure() || !obstruction->isKindOf(KINDOF_IMMOBILE) ||
			obstruction->getControllingPlayer() != player)
		{
			FailSkirmishAITest("fixture_obstruction_lost");
			RequestSkirmishAITestStop();
			return;
		}
		BuildListInfo *info = FindSkirmishAIRecoveryCommandCenterBuildInfo(
			player, s_recovery.primaryTemplate);
		Object *builder = FindSkirmishAIRecoveryBaselineBuilder(player);
		if (!info || !builder || !TheBuildAssistant || !TheTerrainLogic)
		{
			FailSkirmishAITest("fixture_obstruction_build_info_missing");
			RequestSkirmishAITestStop();
			return;
		}

		Coord3D originalObstructionPosition = s_recovery.obstructionOriginalPosition;
		originalObstructionPosition.z = TheTerrainLogic->getGroundHeight(
			originalObstructionPosition.x, originalObstructionPosition.y);
		Coord3D blockedPosition = *info->getLocation();
		blockedPosition.z = TheTerrainLogic->getGroundHeight(
			blockedPosition.x, blockedPosition.y);
		// The production placement path projects the candidate onto terrain
		// before collision and construction. Keep the fixture's intended footprint
		// in that same plane so z-only differences cannot hide a same-footprint
		// rebuild or move the real blocker below the collision volume.
		s_recovery.originalBuildPosition = blockedPosition;
		MoveSkirmishAIRecoveryObject(obstruction, &originalObstructionPosition);
		const LegalBuildCode controlCode =
			TheBuildAssistant->isLocationLegalToBuild(
				&blockedPosition, s_recovery.primaryTemplate, info->getAngle(),
				BuildAssistant::CLEAR_PATH |
				BuildAssistant::TERRAIN_RESTRICTIONS |
				BuildAssistant::NO_OBJECT_OVERLAP,
				builder, player);
		MoveSkirmishAIRecoveryObject(obstruction, &blockedPosition);
		const LegalBuildCode blockedCode =
			TheBuildAssistant->isLocationLegalToBuild(
				&blockedPosition, s_recovery.primaryTemplate, info->getAngle(),
				BuildAssistant::CLEAR_PATH |
				BuildAssistant::TERRAIN_RESTRICTIONS |
				BuildAssistant::NO_OBJECT_OVERLAP,
				builder, player);
		if (blockedCode == LBC_OK || controlCode != LBC_OK)
		{
			FailSkirmishAITest(blockedCode == LBC_OK
				? "fixture_obstruction_not_effective"
				: "fixture_obstruction_control_illegal");
			RequestSkirmishAITestStop();
			return;
		}
		s_recovery.obstructionOriginalPosition = originalObstructionPosition;
		s_recovery.obstructionBlockedPosition = blockedPosition;
		s_recovery.obstructionOriginalLocationBlocked = TRUE;
		s_recovery.obstructionControlLocationLegal = TRUE;
		s_recovery.obstructionPlaced = TRUE;
		printf("SKIRMISH_AI_RECOVERY_PHASE phase=obstruction_placed frame=%u "
			"obstruction=%u original=(%g,%g,%g) blocked=(%g,%g,%g) "
			"blocked_code=%d control_code=%d\n",
			frame, s_recovery.obstructionID, originalObstructionPosition.x,
			originalObstructionPosition.y, originalObstructionPosition.z,
			blockedPosition.x, blockedPosition.y, blockedPosition.z,
			static_cast<Int>(blockedCode), static_cast<Int>(controlCode));
		fflush(stdout);
	}
	if (IsSkirmishAIRecoveryFactoryFixture() ||
		s_recovery.fixtureCase == SKIRMISH_AI_RECOVERY_NO_PATH_LASTSTAND)
		DestroySkirmishAIRecoveryBuilders(player);
	if (s_recovery.fixtureCase == SKIRMISH_AI_RECOVERY_NO_PATH_LASTSTAND)
		DestroySkirmishAIRecoveryBuilderFactories(player, s_recovery.builderTemplate);
	if (IsSkirmishAIRecoveryFactoryFixture())
	{
		// Give the legitimate factory exactly one paid builder plus the
		// subsequent paid command-center construction. The protected reserve
		// then leaves no credits for optional worker replenishment.
		SetSkirmishAIRecoveryCash(player, static_cast<UnsignedInt>(
			s_recovery.ccCost + s_recovery.builderCost));
	}

	if (s_recovery.fixtureCase == SKIRMISH_AI_RECOVERY_LOW_CASH)
	{
		s_recovery.lowCash = s_recovery.ccCost > 0
			? static_cast<UnsignedInt>(s_recovery.ccCost - 1) : 0;
		SetSkirmishAIRecoveryCash(player, s_recovery.lowCash);
		s_recovery.nextActionFrame = TheGameLogic->getFrame() +
			SKIRMISH_AI_RECOVERY_LOW_CASH_WAIT_FRAMES;
		s_recovery.phase = SKIRMISH_AI_RECOVERY_PHASE_WAIT_LOW_CASH;
	}
	else if (s_recovery.fixtureCase == SKIRMISH_AI_RECOVERY_NO_PATH_LASTSTAND)
	{
		s_recovery.phase = SKIRMISH_AI_RECOVERY_PHASE_VERIFY_NO_PATH;
	}
	else if (s_recovery.fixtureCase == SKIRMISH_AI_RECOVERY_GLA_HOLE)
	{
		s_recovery.phase = SKIRMISH_AI_RECOVERY_PHASE_VERIFY_GLA_HOLE;
	}
	else if (s_recovery.fixtureCase == SKIRMISH_AI_RECOVERY_SAVE_LOAD)
	{
		s_recovery.phase = SKIRMISH_AI_RECOVERY_PHASE_SAVE_LOAD_PENDING;
	}
	else
	{
		s_recovery.phase = SKIRMISH_AI_RECOVERY_PHASE_WAIT_RECOVERY;
	}
	s_recovery.phaseStartFrame = frame;
	s_recovery.lastObservedCash = player->getMoney()->countMoney();
	printf("SKIRMISH_AI_RECOVERY_PHASE phase=fault_applied frame=%u case=%s destruction_count=%d "
		"builders_remaining=%d builder_factories_remaining=%d\n",
		frame, GetSkirmishAIRecoveryFixtureCaseName(s_recovery.fixtureCase),
		s_recovery.destructionCount,
		CountSkirmishAIRecoveryBuilders(player, nullptr),
		CountSkirmishAIRecoveryBuilderFactories(
			player, s_recovery.builderTemplate, nullptr, nullptr));
	fflush(stdout);
}

void UpdateSkirmishAIRecoveryFixture()
{
	if (!s_recovery.active || s_recovery.phase == SKIRMISH_AI_RECOVERY_PHASE_COMPLETE)
		return;

	if (!IsSkirmishAIRecoveryFixtureSetupValid())
	{
		FailSkirmishAITest("fixture_skirmish_setup_mismatch");
		RequestSkirmishAITestStop();
		return;
	}

	Player *player = GetSkirmishAIRecoveryFixturePlayer();
	const UnsignedInt frame = TheGameLogic->getFrame();
	if (frame >= SKIRMISH_AI_TEST_MAX_FRAME)
	{
		FailSkirmishAITest("fixture_frame_limit");
		RequestSkirmishAITestStop();
		return;
	}

	if (s_recovery.phase == SKIRMISH_AI_RECOVERY_PHASE_WAIT_BASELINE)
	{
		const UnsignedInt baselineTimeout =
			s_recovery.fixtureCase == SKIRMISH_AI_RECOVERY_NO_PATH_LASTSTAND
				? SKIRMISH_AI_RECOVERY_NO_PATH_BASELINE_TIMEOUT_FRAMES
				: (s_recovery.fixtureCase == SKIRMISH_AI_RECOVERY_INFRASTRUCTURE_COLLAPSE
					? 3600 : SKIRMISH_AI_RECOVERY_BASELINE_TIMEOUT_FRAMES);
		if (frame > baselineTimeout)
		{
			printf("SKIRMISH_AI_RECOVERY_DIAGNOSTIC reason=baseline_timeout frame=%u "
				"case=%s builders=%d combat_units=%d factories=%d builder_queue=%d "
				"baseline_timeout=%u\n",
				frame, GetSkirmishAIRecoveryFixtureCaseName(s_recovery.fixtureCase),
				CountSkirmishAIRecoveryBuilders(player, nullptr),
				CountSkirmishAIRecoveryAttackCapableUnits(player),
				CountSkirmishAIRecoveryBuilderFactories(
					player, s_recovery.builderTemplate, nullptr, nullptr),
				CountSkirmishAIRecoveryBuilderQueueEntries(
					player, s_recovery.builderTemplate, nullptr), baselineTimeout);
			fflush(stdout);
			FailSkirmishAITest(
				s_recovery.fixtureCase == SKIRMISH_AI_RECOVERY_REPEATED_COMMAND_CENTER &&
				!s_recovery.repeatedBaselineRouteReady
					? "fixture_repeated_baseline_route_timeout"
					: "fixture_baseline_timeout");
			RequestSkirmishAITestStop();
			return;
		}

		Object *commandCenter = player->findNaturalCommandCenter();
		if (!commandCenter || !IsLiveSkirmishAIRecoveryObject(commandCenter) ||
			commandCenter->getControllingPlayer() != player ||
			commandCenter->testStatus(OBJECT_STATUS_UNDER_CONSTRUCTION))
			return;

		s_recovery.primaryTemplate = commandCenter->getTemplate();
		const ThingTemplate *builderTemplate = ResolveSkirmishAIRecoveryBuilderTemplate(
			player, s_recovery.primaryTemplate);
		const Int builderCount = CountSkirmishAIRecoveryBuilders(player, nullptr);
		const Int builderQueueCount = builderTemplate
			? CountSkirmishAIRecoveryBuilderQueueEntries(player, builderTemplate, nullptr) : 0;
		s_recovery.preFaultBuilderQueueCount = builderQueueCount;
		s_recovery.preFaultBuilderWork = HasSkirmishAIRecoveryPendingBuilderWork(player);
		Object *builderFactory = nullptr;
		const Int builderFactoryCount = builderTemplate
			? CountSkirmishAIRecoveryBuilderFactories(
				player, builderTemplate, commandCenter, &builderFactory) : 0;
		if (builderCount <= 0 || !builderTemplate ||
			!HasSkirmishAIRecoveryBuilderTemplate(player, builderTemplate))
			return;
		if (IsSkirmishAIRecoveryFactoryFixture() &&
			(builderFactoryCount == 0 || !builderFactory))
			return;
		if (s_recovery.fixtureCase == SKIRMISH_AI_RECOVERY_INFRASTRUCTURE_COLLAPSE)
		{
			Int baselineIncome = 0;
			CountSkirmishAIInfrastructure(player,
				&s_recovery.infrastructureBaselineBuildings,
				&s_recovery.infrastructureBaselinePower,
				&s_recovery.infrastructureBaselineProduction, &baselineIncome);
			const Bool gla = strncmp(player->getSide().str(), "GLA", 3) == 0;
			if (s_recovery.infrastructureBaselineBuildings < 2 ||
				s_recovery.infrastructureBaselineProduction < 1 ||
				(!gla && s_recovery.infrastructureBaselinePower < 1))
				return;
		}
	if (s_recovery.fixtureCase == SKIRMISH_AI_RECOVERY_OBSTRUCTED &&
		!FindSkirmishAIRecoveryObstruction(player, commandCenter))
		return;
	if (s_recovery.fixtureCase == SKIRMISH_AI_RECOVERY_LOW_CASH)
	{
		Object *victoryBuilding = FindSkirmishAIRecoveryVictoryBuilding(player);
		if (!victoryBuilding)
			return;
		printf("SKIRMISH_AI_RECOVERY_PHASE phase=low_cash_baseline_building "
			"frame=%u building=%u template=%s\n",
			frame, victoryBuilding->getID(),
			victoryBuilding->getTemplate()->getName().str());
		fflush(stdout);
	}
	if (s_recovery.fixtureCase == SKIRMISH_AI_RECOVERY_REPEATED_COMMAND_CENTER)
	{
		const Int compatibleBuilderCount = CountSkirmishAIRecoveryBuildersForTemplate(
			player, builderTemplate);
		// Only a second live compatible builder proves that the healthy
		// insurance route has materialized. Factory and queue counts remain
		// diagnostic context; unpaid WorkOrders cannot satisfy this gate.
		const Bool viableInsuranceRoute = compatibleBuilderCount >= 2;
		if (!viableInsuranceRoute)
		{
			s_recovery.repeatedBaselineRouteReady = FALSE;
			s_recovery.nextActionFrame = 0;
			return;
		}
		if (!s_recovery.repeatedBaselineRouteReady)
		{
			s_recovery.repeatedBaselineRouteReady = TRUE;
			s_recovery.nextActionFrame = frame +
				SKIRMISH_AI_RECOVERY_REPEAT_SETTLE_FRAMES;
			printf("SKIRMISH_AI_RECOVERY_PHASE phase=repeated_baseline_route_ready "
				"frame=%u builders=%d factories=%d builder_queue=%d settle_until=%u\n",
				frame, compatibleBuilderCount, builderFactoryCount,
				builderQueueCount, s_recovery.nextActionFrame);
			fflush(stdout);
			return;
		}
		if (frame < s_recovery.nextActionFrame)
			return;
	}

	BuildListInfo *info = FindSkirmishAIRecoveryCommandCenterBuildInfo(
			player, s_recovery.primaryTemplate);
		if (!info)
		{
			FailSkirmishAITest("fixture_command_center_build_info_missing");
			RequestSkirmishAITestStop();
			return;
		}

		if (IsSkirmishAIRecoveryFactoryFixture())
		{
			s_recovery.builderFactoryID = builderFactory->getID();
			s_recovery.preFaultFactoryBuilderQueueCount =
				CountSkirmishAIRecoveryBuilderQueueEntriesOnFactory(
					player, builderTemplate, s_recovery.builderFactoryID);
			if (s_recovery.preFaultFactoryBuilderQueueCount > 0)
				CancelSkirmishAIRecoveryBuilderQueueEntriesOnFactory(
					player, builderTemplate, s_recovery.builderFactoryID);
			if (CountSkirmishAIRecoveryBuilderQueueEntriesOnFactory(
					player, builderTemplate, s_recovery.builderFactoryID) != 0)
				return;
			s_recovery.spawnedBuilderID = FindSkirmishAIRecoveryBuilderProducedByFactory(
				player, builderTemplate, s_recovery.builderFactoryID);
			if (s_recovery.spawnedBuilderID == INVALID_ID)
				return;
			s_recovery.spawnConsumed = TRUE;
		}

		s_recovery.builderTemplate = builderTemplate;
		CaptureSkirmishAIRecoveryBaselineIDs(
			player, s_recovery.builderTemplate, s_recovery.builderFactoryID);
		if (s_recovery.fixtureCase == SKIRMISH_AI_RECOVERY_SURVIVING_BUILDER &&
			(s_recovery.baselineBuilderIDCount <= 0 ||
				!FindSkirmishAIRecoveryBaselineBuilder(player)))
			return;
		s_recovery.initialCenterID = commandCenter->getID();
		s_recovery.lastCompletedCenterID = commandCenter->getID();
		s_recovery.originalCenterPosition = *commandCenter->getPosition();
		s_recovery.originalBuildPosition = *info->getLocation();
		s_recovery.initialBuilderCount = builderCount;
		s_recovery.initialFactoryCount = builderFactoryCount;
		s_recovery.ccCost = commandCenter->getTemplate()->calcCostToBuild(player);
		s_recovery.builderCost = builderTemplate->calcCostToBuild(player);
		s_recovery.initialCash = player->getMoney()->countMoney();
		s_recovery.initialCombatCount =
			CountSkirmishAIRecoveryAttackCapableUnits(player);
		if (s_recovery.fixtureCase == SKIRMISH_AI_RECOVERY_NO_PATH_LASTSTAND &&
			s_recovery.initialCombatCount <= 0)
			return;
		if (s_recovery.fixtureCase == SKIRMISH_AI_RECOVERY_NO_PATH_LASTSTAND)
		{
			if (!s_recovery.lastStandBaselineObjectiveObserved)
			{
				ObjectID naturalAttackUnitID = INVALID_ID;
				Coord3D naturalAttackTarget;
				naturalAttackTarget.zero();
				if (!FindSkirmishAIRecoveryNaturalAttackMove(
						player, &naturalAttackUnitID, &naturalAttackTarget))
					return;
				s_recovery.lastStandBaselineAttackTarget = naturalAttackTarget;
				printf("SKIRMISH_AI_RECOVERY_PHASE phase=last_stand_baseline_target "
					"frame=%u unit=%u target=(%g,%g,%g)\n",
					frame, naturalAttackUnitID, naturalAttackTarget.x,
					naturalAttackTarget.y, naturalAttackTarget.z);
				s_recovery.lastStandBaselineObjectiveObserved = TRUE;
				fflush(stdout);
			}
			if (!s_recovery.lastStandBaselinePrepared)
			{
				if (!PrepareSkirmishAIRecoveryLastStandBaseline(player) ||
					!IsSkirmishAIRecoveryBaselineCombatID(
						s_recovery.lastStandBaselineEvidenceUnitID))
					return;
				s_recovery.lastStandBaselinePrepared = TRUE;
				Object *baselineEvidenceUnit = TheGameLogic->findObjectByID(
					s_recovery.lastStandBaselineEvidenceUnitID);
				const char *baselineEvidenceTemplate = baselineEvidenceUnit &&
					baselineEvidenceUnit->getTemplate()
					? baselineEvidenceUnit->getTemplate()->getName().str() : "<missing>";
				const Bool baselineEvidenceContained = baselineEvidenceUnit &&
					baselineEvidenceUnit->isContained();
				printf("SKIRMISH_AI_RECOVERY_PHASE phase=last_stand_baseline_prepared "
					"frame=%u combat_units=%d baseline_unit=%u template=%s "
					"contained=%d baseline_combat_ids=",
					frame, s_recovery.baselineCombatIDCount,
					s_recovery.lastStandBaselineEvidenceUnitID,
					baselineEvidenceTemplate, baselineEvidenceContained);
				Int combatDiagnosticIndex;
				for (combatDiagnosticIndex = 0;
					combatDiagnosticIndex < s_recovery.baselineCombatIDCount;
					++combatDiagnosticIndex)
					printf("%s%u", combatDiagnosticIndex == 0 ? "" : ",",
						s_recovery.baselineCombatIDs[combatDiagnosticIndex]);
				printf("\n");
				fflush(stdout);
			}
			s_recovery.initialCombatCount = s_recovery.baselineCombatIDCount;
		}
		if (s_recovery.ccCost <= 0 || s_recovery.builderCost <= 0)
		{
			FailSkirmishAITest("fixture_invalid_recovery_cost");
			RequestSkirmishAITestStop();
			return;
		}

		// Exhaust the ordinary build-list allowance.  The selected recovery path
		// must still rebuild through normal construction and payment APIs.
		info->setNumRebuilds(0);
		s_recovery.baselineCaptured = TRUE;
		s_recovery.phase = SKIRMISH_AI_RECOVERY_PHASE_APPLY_FAULT;
		s_recovery.phaseStartFrame = frame;
		s_recovery.lastObservedCash = s_recovery.initialCash;
		printf("SKIRMISH_AI_RECOVERY_PHASE phase=baseline_ready frame=%u center=%u "
			"rebuilds=%d builders=%d pre_fault_builder_queue=%d "
			"pre_fault_factory_builder_queue=%d pre_fault_builder_work=%d "
			"spawn_consumed=%d spawned_builder=%u initial_combat=%d factories=%d cash=%u "
			"cc_cost=%d builder_cost=%d\n",
			frame, s_recovery.initialCenterID, info->getNumRebuilds(),
			s_recovery.initialBuilderCount, s_recovery.preFaultBuilderQueueCount,
			s_recovery.preFaultFactoryBuilderQueueCount, s_recovery.preFaultBuilderWork,
			s_recovery.spawnConsumed, s_recovery.spawnedBuilderID,
			s_recovery.initialCombatCount, s_recovery.initialFactoryCount,
			s_recovery.initialCash, s_recovery.ccCost, s_recovery.builderCost);
		fflush(stdout);
		if (IsSkirmishAIRecoveryFactoryFixture() ||
			s_recovery.fixtureCase == SKIRMISH_AI_RECOVERY_NO_PATH_LASTSTAND)
			ApplySkirmishAIRecoveryFixtureFault(player);
		return;
	}

	if (s_recovery.phase == SKIRMISH_AI_RECOVERY_PHASE_APPLY_FAULT)
	{
		ApplySkirmishAIRecoveryFixtureFault(player);
		return;
	}

	if (s_recovery.phase == SKIRMISH_AI_RECOVERY_PHASE_VERIFY_GLA_HOLE)
	{
		UpdateSkirmishAIRecoveryHoleFixture(player);
		return;
	}

	if (!ObserveSkirmishAIRecoveryFixture(player))
	{
		RequestSkirmishAITestStop();
		return;
	}
	if (!UpdateSkirmishAIRecoveryDisabledFactoryFixture(player))
	{
		RequestSkirmishAITestStop();
		return;
	}

	if (s_recovery.fixtureCase == SKIRMISH_AI_RECOVERY_SAVE_LOAD &&
		s_recovery.phase == SKIRMISH_AI_RECOVERY_PHASE_SAVE_LOAD_PENDING &&
		!s_recovery.saveLoadIssued)
	{
		Int commandCenterCount = 0;
		Bool underConstruction = FALSE;
		Object *commandCenter = FindSkirmishAIRecoveryCommandCenter(
			player, s_recovery.primaryTemplate, &commandCenterCount,
			&underConstruction);
		if (commandCenter && commandCenterCount == 1 && underConstruction &&
			commandCenter->getConstructionPercent() >=
				SKIRMISH_AI_RECOVERY_SAVE_LOAD_MIN_PROGRESS)
		{
			if (!SaveAndLoadSkirmishAIRecoveryFixture(player, commandCenter))
			{
				RequestSkirmishAITestStop();
				return;
			}
			return;
		}
	}

	if (s_recovery.fixtureCase == SKIRMISH_AI_RECOVERY_SAVE_LOAD &&
		s_recovery.phase == SKIRMISH_AI_RECOVERY_PHASE_SAVE_LOAD_PENDING &&
		frame - s_recovery.phaseStartFrame > SKIRMISH_AI_RECOVERY_PHASE_TIMEOUT_FRAMES)
	{
		FailSkirmishAITest("fixture_save_load_pending_timeout");
		RequestSkirmishAITestStop();
		return;
	}

	if (s_recovery.fixtureCase == SKIRMISH_AI_RECOVERY_SAVE_LOAD &&
		s_recovery.saveLoadIssued && !s_recovery.saveLoadRebound)
	{
		Int commandCenterCount = 0;
		Bool underConstruction = FALSE;
		Object *commandCenter = FindSkirmishAIRecoveryCommandCenter(
			player, s_recovery.primaryTemplate, &commandCenterCount,
			&underConstruction);
		if (commandCenter && commandCenterCount == 1 && underConstruction &&
			commandCenter->getID() == s_recovery.saveLoadConstructionID)
		{
			s_recovery.saveLoadRebound = TRUE;
			printf("SKIRMISH_AI_RECOVERY_PHASE phase=save_load_rebound frame=%u "
				"construction=%u percent=%g\n",
				frame, commandCenter->getID(), commandCenter->getConstructionPercent());
			fflush(stdout);
		}
		else if (frame - s_recovery.phaseStartFrame >
			SKIRMISH_AI_RECOVERY_PHASE_TIMEOUT_FRAMES)
		{
			FailSkirmishAITest("fixture_save_load_rebind_missing");
			RequestSkirmishAITestStop();
			return;
		}
	}

	if (s_recovery.fixtureCase == SKIRMISH_AI_RECOVERY_SAVE_LOAD &&
		s_recovery.saveLoadRebound &&
		FindSkirmishAIRecoveryCommandCenter(
			player, s_recovery.primaryTemplate, nullptr, nullptr) &&
		FindSkirmishAIRecoveryCommandCenter(
			player, s_recovery.primaryTemplate, nullptr, nullptr)->getConstructionPercent() >
			s_recovery.saveLoadConstructionPercent)
		s_recovery.saveLoadSawProgress = TRUE;

	if (s_recovery.phase == SKIRMISH_AI_RECOVERY_PHASE_WAIT_LOW_CASH)
	{
		Int commandCenterCount = 0;
		Bool underConstruction = FALSE;
		FindSkirmishAIRecoveryCommandCenter(
			player, s_recovery.primaryTemplate, &commandCenterCount, &underConstruction);
		if (!s_recovery.moneyReleased)
		{
			// Keep resource income from accidentally crossing the deliberate
			// below-cost boundary before the later affordability event.
			if (frame < s_recovery.nextActionFrame)
				SetSkirmishAIRecoveryCash(player, s_recovery.lowCash);
			if (commandCenterCount != 0 || underConstruction ||
				CountSkirmishAIRecoveryBuilderQueueEntries(
					player, s_recovery.builderTemplate, nullptr) != 0)
			{
				FailSkirmishAITest("fixture_low_cash_started_early");
				RequestSkirmishAITestStop();
				return;
			}
			// This callback runs after Player::update(); release one frame
			// before the deadline so the next full engine update sees the funds.
			if (frame + 1U >= s_recovery.nextActionFrame)
			{
				SetSkirmishAIRecoveryCash(
					player, static_cast<UnsignedInt>(s_recovery.ccCost));
				s_recovery.moneyReleased = TRUE;
				s_recovery.phase = SKIRMISH_AI_RECOVERY_PHASE_WAIT_RECOVERY;
				s_recovery.phaseStartFrame = frame;
				s_recovery.lastObservedCash = player->getMoney()->countMoney();
				printf("SKIRMISH_AI_RECOVERY_PHASE phase=money_released frame=%u cash=%u\n",
					frame, s_recovery.lastObservedCash);
				fflush(stdout);
			}
		}
		return;
	}

	if (s_recovery.phase == SKIRMISH_AI_RECOVERY_PHASE_VERIFY_NO_PATH)
	{
		if (CountSkirmishAIRecoveryBuilders(player, nullptr) != 0 ||
			CountSkirmishAIRecoveryBuilderFactories(
				player, s_recovery.builderTemplate, nullptr, nullptr) != 0 ||
			CountSkirmishAIRecoveryBuilderQueueEntries(
				player, s_recovery.builderTemplate, nullptr) != 0)
		{
			FailSkirmishAITest("fixture_recovery_path_reappeared");
			RequestSkirmishAITestStop();
			return;
		}
		if (frame - s_recovery.phaseStartFrame >= SKIRMISH_AI_RECOVERY_NO_PATH_VERIFY_FRAMES)
		{
			Bool postFaultAttackEvidence = FALSE;
			const Int liveBaselineCombatCount = CountSkirmishAIRecoveryLastStandUnits(
				player, &postFaultAttackEvidence);
			Object *baselineEvidenceUnit = TheGameLogic->findObjectByID(
				s_recovery.lastStandBaselineEvidenceUnitID);
			const Bool baselineEvidenceLive =
				IsLiveSkirmishAIRecoveryObject(baselineEvidenceUnit);
			if (liveBaselineCombatCount <= 0 || !s_recovery.sawLastStand)
			{
				PrintSkirmishAIRecoveryLastStandEvidenceDiagnostics(player);
				printf("SKIRMISH_AI_RECOVERY_DIAGNOSTIC reason=last_stand_forces_missing "
					"frame=%u baseline_units=%d live_baseline_units=%d "
					"attack_path_evidence=%d\n", frame,
					s_recovery.baselineCombatIDCount, liveBaselineCombatCount,
					postFaultAttackEvidence);
				fflush(stdout);
				FailSkirmishAITest(baselineEvidenceLive
					? "fixture_last_stand_forces_missing"
					: "fixture_last_stand_baseline_unit_lost");
				RequestSkirmishAITestStop();
				return;
			}
			s_recovery.phase = SKIRMISH_AI_RECOVERY_PHASE_COMPLETE;
			printf("SKIRMISH_AI_RECOVERY_PHASE phase=last_stand_assertions_passed frame=%u "
				"recovery_paths=0 construction_scaffolds=%d\n",
				frame, s_recovery.constructionScaffoldCount);
			fflush(stdout);
			s_runner.endFrame = frame;
			RequestSkirmishAITestStop();
		}
		return;
	}

	if (s_recovery.phase == SKIRMISH_AI_RECOVERY_PHASE_WAIT_RECOVERY)
	{
		if (frame - s_recovery.phaseStartFrame > SKIRMISH_AI_RECOVERY_PHASE_TIMEOUT_FRAMES)
		{
			Int timeoutCenterCount = 0;
			Bool timeoutUnderConstruction = FALSE;
			Object *timeoutCenter = FindSkirmishAIRecoveryCommandCenter(
				player, s_recovery.primaryTemplate, &timeoutCenterCount,
				&timeoutUnderConstruction);
			PrintSkirmishAIRecoveryScaffoldDiagnostics(
				player, timeoutCenter, "recovery_timeout_scaffold");
			printf("SKIRMISH_AI_RECOVERY_DIAGNOSTIC reason=recovery_timeout frame=%u "
				"construction=%d progress=%d ownership=%d complete=%d alternate=%d "
				"obstruction=%d obstruction_blocked=%d obstruction_control=%d "
				"destructions=%d recoveries=%d\n",
				frame, s_recovery.sawConstruction, s_recovery.sawConstructionProgress,
				s_recovery.sawConstructionOwnership, s_recovery.sawCompletedRecovery,
				s_recovery.sawAlternatePlacement, s_recovery.obstructionPlaced,
				s_recovery.obstructionOriginalLocationBlocked,
				s_recovery.obstructionControlLocationLegal,
				s_recovery.destructionCount, s_recovery.recoveryCompletionCount);
			fflush(stdout);
			FailSkirmishAITest("fixture_recovery_timeout");
			RequestSkirmishAITestStop();
			return;
		}
		if (s_recovery.sawCompletedRecovery)
		{
			if (s_recovery.fixtureCase == SKIRMISH_AI_RECOVERY_INFRASTRUCTURE_COLLAPSE)
			{
				s_recovery.phase = SKIRMISH_AI_RECOVERY_PHASE_WAIT_INFRASTRUCTURE;
				s_recovery.phaseStartFrame = frame;
				return;
			}
			if (s_recovery.fixtureCase == SKIRMISH_AI_RECOVERY_REPEATED_COMMAND_CENTER &&
				!s_recovery.secondFaultIssued)
			{
				if (frame - s_recovery.phaseStartFrame <
					SKIRMISH_AI_RECOVERY_REPEAT_SETTLE_FRAMES)
					return;
				Int commandCenterCount = 0;
				Bool underConstruction = FALSE;
				Object *commandCenter = FindSkirmishAIRecoveryCommandCenter(
					player, s_recovery.primaryTemplate, &commandCenterCount,
					&underConstruction);
				if (!commandCenter || commandCenterCount != 1 || underConstruction)
					return;
				BuildListInfo *info = FindSkirmishAIRecoveryCommandCenterBuildInfo(
					player, s_recovery.primaryTemplate);
				if (!info)
				{
					FailSkirmishAITest("fixture_repeated_build_info_missing");
					RequestSkirmishAITestStop();
					return;
				}
				info->setNumRebuilds(0);
				DestroySkirmishAIRecoveryObject(commandCenter);
				++s_recovery.destructionCount;
				s_recovery.secondFaultIssued = TRUE;
				s_recovery.sawConstruction = FALSE;
				s_recovery.sawConstructionProgress = FALSE;
				s_recovery.sawConstructionOwnership = FALSE;
				s_recovery.currentConstructionID = INVALID_ID;
				s_recovery.lastConstructionBuilderID = INVALID_ID;
				s_recovery.lastConstructionPercent = 0.0f;
				s_recovery.phase = SKIRMISH_AI_RECOVERY_PHASE_WAIT_SECOND_RECOVERY;
				s_recovery.phaseStartFrame = frame;
				printf("SKIRMISH_AI_RECOVERY_PHASE phase=second_fault_applied frame=%u destruction_count=%d\n",
					frame, s_recovery.destructionCount);
				fflush(stdout);
				return;
			}
			if (s_recovery.fixtureCase != SKIRMISH_AI_RECOVERY_REPEATED_COMMAND_CENTER &&
				!FinishSkirmishAIRecoveryFixture())
			{
				RequestSkirmishAITestStop();
				return;
			}
		}
		return;
	}
	if (s_recovery.phase == SKIRMISH_AI_RECOVERY_PHASE_WAIT_INFRASTRUCTURE)
	{
		if (frame % LOGICFRAMES_PER_SECOND != 0)
			return;
		Int buildings = 0;
		Int power = 0;
		Int production = 0;
		Int income = 0;
		CountSkirmishAIInfrastructure(player, &buildings, &power,
			&production, &income);
		const UnsignedInt infrastructureElapsed =
			frame - s_recovery.phaseStartFrame;
		if ((infrastructureElapsed >= 9000 && infrastructureElapsed <
				9000 + LOGICFRAMES_PER_SECOND) ||
			(infrastructureElapsed >= 15000 && infrastructureElapsed <
				15000 + LOGICFRAMES_PER_SECOND))
		{
			printf("SKIRMISH_AI_INFRASTRUCTURE_PROGRESS frame=%u "
				"buildings=%d power=%d production=%d renewable_income=%d "
				"cash=%u\n", frame, buildings, power, production,
				income, player->getMoney()->countMoney());
			PrintSkirmishAIInfrastructureIncomePlans(player);
		}
		const Bool gla = strncmp(player->getSide().str(), "GLA", 3) == 0;
		if (power >= (gla ? 0 : max(2,
			s_recovery.infrastructureBaselinePower)) &&
			production >= s_recovery.infrastructureBaselineProduction &&
			income >= 6 &&
			buildings >= s_recovery.infrastructureBaselineBuildings &&
			HasRestoredSkirmishAIInfrastructureBaseline(player) &&
			s_recovery.infrastructureFaulted)
		{
			printf("SKIRMISH_AI_INFRASTRUCTURE_RESTORED frame=%u buildings=%d "
				"power=%d production=%d renewable_income=%d cash=%u "
				"depleted_supply_sources=%d\n",
				frame, buildings, power, production, income,
				player->getMoney()->countMoney(),
				s_recovery.depletedSupplySources);
			fflush(stdout);
			if (!gla)
			{
				Int destroyedPower = 0;
				for (Object *object = TheGameLogic->getFirstObject(); object; )
				{
					Object *next = object->getNextObject();
					if (object->getControllingPlayer() == player &&
						object->isKindOf(KINDOF_FS_POWER) &&
						!object->isKindOf(KINDOF_CASH_GENERATOR) &&
						IsLiveSkirmishAIRecoveryObject(object))
					{
						DestroySkirmishAIRecoveryObject(object);
						++destroyedPower;
					}
					object = next;
				}
				for (BuildListInfo *info = player->getBuildList(); info;
					info = info->getNext())
				{
					const ThingTemplate *plan = TheThingFactory->findTemplate(
						info->getTemplateName());
					if (plan && plan->isKindOf(KINDOF_FS_POWER) &&
						!plan->isKindOf(KINDOF_CASH_GENERATOR))
						info->setNumRebuilds(0);
				}
				s_recovery.phase = SKIRMISH_AI_RECOVERY_PHASE_WAIT_POWER_REBUILD;
				s_recovery.phaseStartFrame = frame;
				printf("SKIRMISH_AI_POWER_FAULT frame=%u destroyed=%d\n",
					frame, destroyedPower);
				fflush(stdout);
				return;
			}
			if (!FinishSkirmishAIRecoveryFixture())
				RequestSkirmishAITestStop();
			return;
		}
		if (frame - s_recovery.phaseStartFrame >
			SKIRMISH_AI_RECOVERY_INFRASTRUCTURE_TIMEOUT_FRAMES)
		{
			printf("SKIRMISH_AI_INFRASTRUCTURE_TIMEOUT frame=%u buildings=%d "
				"power=%d production=%d renewable_income=%d cash=%u "
				"baseline_templates_restored=%d\n",
				frame, buildings, power, production, income,
				player->getMoney()->countMoney(),
				HasRestoredSkirmishAIInfrastructureBaseline(player));
			fflush(stdout);
			PrintSkirmishAIInfrastructureIncomePlans(player);
			FailSkirmishAITest("fixture_infrastructure_not_restored");
			RequestSkirmishAITestStop();
		}
		return;
	}
	if (s_recovery.phase == SKIRMISH_AI_RECOVERY_PHASE_WAIT_POWER_REBUILD)
	{
		if (frame % LOGICFRAMES_PER_SECOND != 0)
			return;
		Int buildings = 0;
		Int power = 0;
		Int production = 0;
		Int income = 0;
		CountSkirmishAIInfrastructure(player, &buildings, &power,
			&production, &income);
		if (power >= 2 && player->getEnergy()->hasSufficientPower() &&
			production >= 1 && income >= 3)
		{
			printf("SKIRMISH_AI_POWER_RESTORED frame=%u power=%d "
				"production=%d renewable_income=%d cash=%u\n",
				frame, power, production, income,
				player->getMoney()->countMoney());
			fflush(stdout);
			if (!FinishSkirmishAIRecoveryFixture())
				RequestSkirmishAITestStop();
			return;
		}
		if (frame - s_recovery.phaseStartFrame >
			SKIRMISH_AI_RECOVERY_INFRASTRUCTURE_TIMEOUT_FRAMES)
		{
			printf("SKIRMISH_AI_POWER_TIMEOUT frame=%u power=%d "
				"production=%d renewable_income=%d cash=%u\n",
				frame, power, production, income,
				player->getMoney()->countMoney());
			fflush(stdout);
			FailSkirmishAITest("fixture_power_not_restored");
			RequestSkirmishAITestStop();
		}
		return;
	}

	if (s_recovery.phase == SKIRMISH_AI_RECOVERY_PHASE_SAVE_LOAD_REBOUND)
	{
		if (frame - s_recovery.phaseStartFrame > SKIRMISH_AI_RECOVERY_PHASE_TIMEOUT_FRAMES)
		{
			FailSkirmishAITest("fixture_save_load_recovery_timeout");
			RequestSkirmishAITestStop();
			return;
		}
		if (s_recovery.sawCompletedRecovery &&
			!FinishSkirmishAIRecoveryFixture())
		{
			RequestSkirmishAITestStop();
			return;
		}
		return;
	}

	if (s_recovery.phase == SKIRMISH_AI_RECOVERY_PHASE_WAIT_SECOND_RECOVERY)
	{
		if (frame - s_recovery.phaseStartFrame > SKIRMISH_AI_RECOVERY_PHASE_TIMEOUT_FRAMES)
		{
			FailSkirmishAITest("fixture_second_recovery_timeout");
			RequestSkirmishAITestStop();
			return;
		}
		if (!s_recovery.secondBuilderLossIssued &&
			!s_recovery.secondBuilderLossSkipped &&
			s_recovery.sawConstruction && s_recovery.sawConstructionProgress)
		{
			Int commandCenterCount = 0;
			Bool underConstruction = FALSE;
			Object *commandCenter = FindSkirmishAIRecoveryCommandCenter(
				player, s_recovery.primaryTemplate, &commandCenterCount,
				&underConstruction);
			if (commandCenter && commandCenterCount == 1 && underConstruction)
			{
				if (!TryInjectSkirmishAIRecoverySecondBuilderLoss(
						player, commandCenter))
				{
					RequestSkirmishAITestStop();
					return;
				}
			}
		}
		if (s_recovery.secondBuilderLossIssued &&
			!s_recovery.secondBuilderReplacementObserved)
			return;
		if (s_recovery.recoveryCompletionCount >= 2 &&
			(s_recovery.secondBuilderLossIssued ||
			 s_recovery.secondBuilderLossSkipped) &&
			!FinishSkirmishAIRecoveryFixture())
		{
			RequestSkirmishAITestStop();
			return;
		}
	}
}

}


namespace
{
#if RTS_ZEROHOUR && defined(_WIN64)
Bool CheckAlliedTransferCommand(Player *donor, Player *recipient, Int amount,
	Bool accepted, Int argumentShape = 0)
{
	const UnsignedInt frame = TheGameLogic->getFrame();
	const UnsignedInt donorBefore = donor->getMoney()->countMoney();
	const UnsignedInt recipientBefore = recipient->getMoney()->countMoney();
	AIPlayer *recipientAI = recipient->getAIPlayerForPlanning();
	AISkirmishPlayer *skirmishAI = recipientAI && recipientAI->isSkirmishAI()
		? static_cast<AISkirmishPlayer *>(recipientAI) : nullptr;
	const UnsignedInt receiptBefore = skirmishAI ?
		skirmishAI->getAlliedCoordinationDiagnostics().mayDonateFrame : 0;
	CommandList commands;
	GameMessage *message = newInstance(GameMessage)(GameMessage::MSG_TRANSFER_MONEY_TO_ALLY);
	message->friend_setPlayerIndex(donor->getPlayerIndex());
	if (argumentShape == 1)
		message->appendRealArgument(static_cast<Real>(recipient->getPlayerIndex()));
	else
		message->appendIntegerArgument(recipient->getPlayerIndex());
	if (argumentShape == 2)
		message->appendRealArgument(static_cast<Real>(amount));
	else if (argumentShape != 3)
		message->appendIntegerArgument(amount);
	if (argumentShape == 4)
		message->appendIntegerArgument(0);
	commands.appendMessage(message);
	TheGameLogic->processCommandList(&commands);
	const UnsignedInt delta = accepted ? static_cast<UnsignedInt>(amount) : 0;
	const Bool receiptValid = !skirmishAI || (accepted ?
		(skirmishAI->getAlliedCoordinationDiagnostics().receiptCooldownActive &&
		 skirmishAI->getAlliedCoordinationDiagnostics().mayDonateFrame ==
		 frame + 120 * LOGICFRAMES_PER_SECOND) :
		 skirmishAI->getAlliedCoordinationDiagnostics().mayDonateFrame == receiptBefore);
	const Bool valid = receiptValid && TheGameLogic->getFrame() == frame &&
		donor->getMoney()->countMoney() == donorBefore - delta &&
		recipient->getMoney()->countMoney() == recipientBefore + delta;
	printf("SKIRMISH_AI_ALLIED_TRANSFER_ASSERT frame=%u donor=%d recipient=%d "
		"amount=%d shape=%d accepted=%d donor_before=%u donor_after=%u "
		"recipient_before=%u recipient_after=%u valid=%d\n", frame,
		donor->getPlayerIndex(), recipient->getPlayerIndex(), amount, argumentShape,
		accepted, donorBefore, donor->getMoney()->countMoney(), recipientBefore,
		recipient->getMoney()->countMoney(), valid);
	fflush(stdout);
	if (valid) ++s_allied.checks;
	return valid;
}

Bool RunAlliedTransferCommands()
{
	Player *donor = ThePlayerList->getPlayerFromSlotIndex(0);
	Player *recipient = ThePlayerList->getPlayerFromSlotIndex(1);
	Player *enemy = ThePlayerList->getPlayerFromSlotIndex(4);
	Player *inactive = ThePlayerList->getPlayerFromSlotIndex(2);
	if (!donor || !recipient || !enemy || !inactive ||
		donor->getPlayerType() != PLAYER_HUMAN || !donor->isPlayerActive() ||
		!recipient->isPlayerActive()) return FALSE;
	SetSkirmishAIRecoveryCash(donor, 50000);
	SetSkirmishAIRecoveryCash(recipient, 1000);
	if (!CheckAlliedTransferCommand(donor, recipient, 1800, TRUE) ||
		!CheckAlliedTransferCommand(donor, recipient, 100, TRUE) ||
		!CheckAlliedTransferCommand(donor, recipient, 10000, TRUE) ||
		!CheckAlliedTransferCommand(donor, donor, 100, FALSE) ||
		!CheckAlliedTransferCommand(donor, enemy, 100, FALSE) ||
		!CheckAlliedTransferCommand(donor, recipient, -100, FALSE) ||
		!CheckAlliedTransferCommand(donor, recipient, 0, FALSE) ||
		!CheckAlliedTransferCommand(donor, recipient, 50, FALSE) ||
		!CheckAlliedTransferCommand(donor, recipient, 150, FALSE) ||
		!CheckAlliedTransferCommand(donor, recipient, 10100, FALSE)) return FALSE;
	for (Int shape = 1; shape <= 4; ++shape)
		if (!CheckAlliedTransferCommand(donor, recipient, 100, FALSE, shape)) return FALSE;
	SetSkirmishAIRecoveryCash(recipient, UINT_MAX - 50);
	if (!CheckAlliedTransferCommand(donor, recipient, 100, FALSE)) return FALSE;
	SetSkirmishAIRecoveryCash(recipient, 1000);
	SetSkirmishAIRecoveryCash(donor, 50);
	if (!CheckAlliedTransferCommand(donor, recipient, 100, FALSE)) return FALSE;
	SetSkirmishAIRecoveryCash(donor, 20000);
	inactive->killPlayer();
	if (inactive->isPlayerActive() ||
		!CheckAlliedTransferCommand(donor, inactive, 100, FALSE)) return FALSE;
	return TRUE;
}

AISkirmishPlayer *GetAlliedFixtureAI(Int slot)
{
	Player *player = ThePlayerList->getPlayerFromSlotIndex(slot);
	AIPlayer *ai = player ? player->getAIPlayerForPlanning() : nullptr;
	return ai && ai->isSkirmishAI() ? static_cast<AISkirmishPlayer *>(ai) : nullptr;
}

Player *FindAlliedFixturePlayer(Int playerIndex)
{
	for (Int ordinal = 0; ordinal < ThePlayerList->getPlayerCount(); ++ordinal)
	{
		Player *player = ThePlayerList->getNthPlayer(ordinal);
		if (player && player->getPlayerIndex() == playerIndex) return player;
	}
	return nullptr;
}

Bool SameAlliedDiagnostics(const AISkirmishPlayer::AlliedCoordinationDiagnostics &a,
	const AISkirmishPlayer::AlliedCoordinationDiagnostics &b)
{
	return a.assaultActive == b.assaultActive && a.assaultLaunched == b.assaultLaunched &&
		a.strategyResumePending == b.strategyResumePending &&
		a.assaultTeamCount == b.assaultTeamCount &&
		a.holdAdmissionValid == b.holdAdmissionValid && a.holdAdmissionFrame == b.holdAdmissionFrame &&
		a.homeDamageValid == b.homeDamageValid && a.homeDamageFrame == b.homeDamageFrame &&
		a.heldDamageValid == b.heldDamageValid && a.heldDamageFrame == b.heldDamageFrame &&
		a.leaderIndex == b.leaderIndex && a.enemyIndex == b.enemyIndex &&
		a.targetID == b.targetID && a.assaultFrame == b.assaultFrame &&
		a.assaultExpiryFrame == b.assaultExpiryFrame &&
		a.supportRecipientIndex == b.supportRecipientIndex &&
		a.supportTeamCount == b.supportTeamCount && a.supportReturning == b.supportReturning &&
		a.donationCooldownActive == b.donationCooldownActive &&
		a.nextDonationFrame == b.nextDonationFrame &&
		a.receiptCooldownActive == b.receiptCooldownActive && a.mayDonateFrame == b.mayDonateFrame;
}

Bool CaptureAlliedFixtureAssaultRoster(AISkirmishPlayer *ai, std::vector<UnsignedInt> *ids)
{
	const Int count = ai->getAlliedCoordinationDiagnostics().assaultTeamCount;
	if (count < 0 || count > 16) return FALSE;
	ids->clear();
	for (Int index = 0; index < count; ++index)
	{
		const UnsignedInt id = ai->getAlliedAssaultTeamID(index);
		if (id == 0 || (index > 0 && id <= ids->back()) || !ai->isAlliedAssaultTeam(id)) return FALSE;
		ids->push_back(id);
	}
	return TRUE;
}

Bool AlliedFixtureAssaultRosterContains(const std::vector<UnsignedInt> &ids, UnsignedInt teamID)
{
	for (size_t index = 0; index < ids.size(); ++index) if (ids[index] == teamID) return TRUE;
	return FALSE;
}

void PrintAlliedFixtureAssaultRoster(const char *record, UnsignedInt frame, Int slot,
	const std::vector<UnsignedInt> &ids)
{
	printf("%s frame=%u slot=%d count=%u team_ids=", record, frame, slot, static_cast<UnsignedInt>(ids.size()));
	for (size_t index = 0; index < ids.size(); ++index) printf("%s%u", index == 0 ? "" : ",", ids[index]);
	printf("\n");
}

Bool RoundTripAlliedFixture()
{
	if (!TheAI || !TheGameState) return FALSE;
	// Leave enough protected time for two ordinary five-second evaluations
	// after load. A nonzero expired timestamp is not cooldown evidence.
	if (!s_allied.sawAid || s_allied.aidCooldownUntil <= TheGameLogic->getFrame() ||
		s_allied.aidCooldownUntil - TheGameLogic->getFrame() < 20 * LOGICFRAMES_PER_SECOND)
		return FALSE;
	AISkirmishPlayer::AlliedCoordinationDiagnostics before[7];
	Int playerIndices[7];
	Int starvation[7];
	UnsignedInt relief[7];
	std::vector<UnsignedInt> supportIDs[7];
	std::vector<UnsignedInt> assaultIDs[7];
	const UnsignedInt frame = TheGameLogic->getFrame();
	const Bool evaluated = TheAI->hasAlliedEvaluation();
	const UnsignedInt nextEvaluation = TheAI->getNextAlliedEvaluationFrame();
	for (Int slot = 1; slot <= 7; ++slot)
	{
		AISkirmishPlayer *ai = GetAlliedFixtureAI(slot);
		Player *player = ThePlayerList->getPlayerFromSlotIndex(slot);
		if (!ai || !player) return FALSE;
		before[slot - 1] = ai->getAlliedCoordinationDiagnostics();
		if (!CaptureAlliedFixtureAssaultRoster(ai, &assaultIDs[slot - 1])) return FALSE;
		playerIndices[slot - 1] = player->getPlayerIndex();
		starvation[slot - 1] = TheAI->getAlliedStarvationStreak(playerIndices[slot - 1]);
		relief[slot - 1] = TheAI->getAlliedRecipientReliefUntil(playerIndices[slot - 1]);
		for (Int team = 0; team < before[slot - 1].supportTeamCount; ++team)
			supportIDs[slot - 1].push_back(ai->getAlliedSupportTeamID(team));
	}
	if (!s_allied.cancellationIssued ||
		(!before[s_allied.cancellationSlots[0] - 1].strategyResumePending &&
		 !before[s_allied.cancellationSlots[1] - 1].strategyResumePending)) return FALSE;
	for (Int participant = 0; participant < 2; ++participant)
	{
		const AISkirmishPlayer::AlliedCoordinationDiagnostics &state =
			before[s_allied.cancellationSlots[participant] - 1];
		s_allied.cancellationPendingAtSave[participant] =
			state.strategyResumePending;
		if (state.strategyResumePending &&
			(!state.holdAdmissionValid || state.holdAdmissionFrame > frame || state.assaultTeamCount <= 0 ||
			 !AlliedFixtureAssaultRosterContains(assaultIDs[s_allied.cancellationSlots[participant] - 1],
				s_allied.cancellationTeams[participant]))) return FALSE;
	}
	AsciiString filename;
	filename.format("SkirmishAIAllied_%s.sav", s_runner.runNonce);
	UnicodeString description;
	description.set(L"Stage 5 allied coordination fixture");
	const SaveResult saved = TheGameState->saveGame(filename, description, SAVE_FILE_TYPE_NORMAL);
	if (saved.saveCode != SC_OK || saved.filename.isEmpty()) return FALSE;
	AvailableGameInfo gameInfo;
	gameInfo.filename = saved.filename;
	gameInfo.next = nullptr;
	gameInfo.prev = nullptr;
	gameInfo.saveGameInfo = *TheGameState->getSaveGameInfo();
	gameInfo.saveGameInfo.saveFileType = SAVE_FILE_TYPE_NORMAL;
	if (TheGameState->loadGame(gameInfo) != SC_OK) return FALSE;
	s_allied.saveLoaded = TRUE;
	// No borrowed Player/AI/Object pointers survive loadGame. Reacquire each
	// slot and validate the serialized ID-bearing state and central cadence.
	if (!TheAI || !ThePlayerList || TheGameLogic->getFrame() != frame ||
		TheAI->hasAlliedEvaluation() != evaluated ||
		TheAI->getNextAlliedEvaluationFrame() != nextEvaluation) return FALSE;
	for (Int slot = 1; slot <= 7; ++slot)
	{
		AISkirmishPlayer *ai = GetAlliedFixtureAI(slot);
		Player *player = ThePlayerList->getPlayerFromSlotIndex(slot);
		if (!ai || !player || player->getPlayerIndex() != playerIndices[slot - 1] ||
			!SameAlliedDiagnostics(before[slot - 1], ai->getAlliedCoordinationDiagnostics()) ||
			TheAI->getAlliedStarvationStreak(playerIndices[slot - 1]) != starvation[slot - 1] ||
			TheAI->getAlliedRecipientReliefUntil(playerIndices[slot - 1]) != relief[slot - 1]) return FALSE;
		for (Int team = 0; team < before[slot - 1].supportTeamCount; ++team)
			if (ai->getAlliedSupportTeamID(team) != supportIDs[slot - 1][team]) return FALSE;
		std::vector<UnsignedInt> reboundAssaultIDs;
		if (!CaptureAlliedFixtureAssaultRoster(ai, &reboundAssaultIDs) || reboundAssaultIDs != assaultIDs[slot - 1]) return FALSE;
		if (before[slot - 1].assaultActive &&
			!TheGameLogic->findObjectByID(before[slot - 1].targetID)) return FALSE;
	}
	for (Int participant = 0; participant < 2; ++participant)
	{
		AISkirmishPlayer *rebound = GetAlliedFixtureAI(s_allied.cancellationSlots[participant]);
		if (!rebound) return FALSE;
		s_allied.cancellationPendingAfterLoad[participant] =
			rebound->getAlliedCoordinationDiagnostics().strategyResumePending;
		if (s_allied.cancellationPendingAfterLoad[participant] !=
			s_allied.cancellationPendingAtSave[participant]) return FALSE;
	}
	++s_allied.checks;
	for (Int slot = 1; slot <= 7; ++slot)
		if (!assaultIDs[slot - 1].empty())
			PrintAlliedFixtureAssaultRoster("SKIRMISH_AI_ALLIED_ROSTER_SAVE_LOAD_ASSERT", frame, slot, assaultIDs[slot - 1]);
	s_allied.postLoadEvaluation = TheAI->getNextAlliedEvaluationFrame();
	s_allied.postLoadBlockedEvaluations = 0;
	printf("SKIRMISH_AI_ALLIED_SAVE_LOAD_ASSERT frame=%u file=%s "
		"player_slots=7 central_cadence=preserved commitments=preserved support_ids=preserved damage_latches=preserved "
		"admitted_rosters=preserved_nonzero "
		"cooldown_until=%u remaining_frames=%u strategy_resume_pending=preserved_nonzero "
		"pending_at_save=%d,%d pending_after_load=%d,%d hold_admission=preserved "
		"hold_admission_valid=%d,%d hold_admission_frame=%u,%u post_load_protection=pending\n",
		frame, saved.filename.str(), s_allied.aidCooldownUntil, s_allied.aidCooldownUntil - frame,
		s_allied.cancellationPendingAtSave[0], s_allied.cancellationPendingAtSave[1],
		s_allied.cancellationPendingAfterLoad[0], s_allied.cancellationPendingAfterLoad[1],
		before[s_allied.cancellationSlots[0] - 1].holdAdmissionValid,
		before[s_allied.cancellationSlots[1] - 1].holdAdmissionValid,
		before[s_allied.cancellationSlots[0] - 1].holdAdmissionFrame,
		before[s_allied.cancellationSlots[1] - 1].holdAdmissionFrame);
	fflush(stdout);
	return TRUE;
}

void BeginAlliedCancellation(Int firstSlot, Int secondSlot, ObjectID targetID,
	UnsignedInt releaseFrame, UnsignedInt frame)
{
	if (s_allied.cancellationIssued || releaseFrame <= frame) return;
	const Int slots[2] = { firstSlot, secondSlot };
	Object *held[2] = { nullptr, nullptr };
	std::vector<UnsignedInt> rosters[2];
	for (Int participant = 0; participant < 2; ++participant)
	{
		Player *player = ThePlayerList->getPlayerFromSlotIndex(slots[participant]);
		AISkirmishPlayer *skirmish = GetAlliedFixtureAI(slots[participant]);
		Coord3D home;
		if (!player || !skirmish || !skirmish->getBaseCenter(&home)) return;
		if (!CaptureAlliedFixtureAssaultRoster(skirmish, &rosters[participant]) || rosters[participant].empty()) return;
		for (Object *object = TheGameLogic->getFirstObject(); object; object = object->getNextObject())
		{
			AIUpdateInterface *ai = object->getAIUpdateInterface();
			const Coord3D *guard = ai ? ai->getGuardLocation() : nullptr;
			if (IsSkirmishAIRecoveryCombatUnit(object, player) && object->getTeam() && ai && guard &&
				AlliedFixtureAssaultRosterContains(rosters[participant], object->getTeam()->getID()) &&
				skirmish->isAlliedAssaultTeam(object->getTeam()->getID()) &&
				skirmish->isAlliedAssaultHoldingTeam(object->getTeam()->getID()) &&
				ai->getLastCommandSource() == CMD_FROM_AI && ai->getGuardTargetType() == GUARDTARGET_LOCATION)
			{
				const Real dx = guard->x - home.x;
				const Real dy = guard->y - home.y;
				if (dx * dx + dy * dy <= 50.0f * 50.0f) { held[participant] = object; break; }
			}
		}
		if (!held[participant]) return;
	}
	Object *target = TheGameLogic->findObjectByID(targetID);
	if (!IsLiveSkirmishAIRecoveryObject(target)) return;
	for (Int participant = 0; participant < 2; ++participant)
	{
		s_allied.cancellationSlots[participant] = slots[participant];
		s_allied.cancellationMembers[participant] = held[participant]->getID();
		s_allied.cancellationTeams[participant] = held[participant]->getTeam()->getID();
		s_allied.cancellationPositions[participant] = *held[participant]->getPosition();
		PrintAlliedFixtureAssaultRoster("SKIRMISH_AI_ALLIED_CANCELLATION_ROSTER_ASSERT", frame, slots[participant], rosters[participant]);
	}
	s_allied.cancellationIssued = TRUE;
	s_allied.cancellationTarget = targetID;
	s_allied.cancellationRelease = releaseFrame;
	// Use the real world-state loss boundary. No commitment or resume flag is
	// written by the fixture; normal owner updates must cancel the cohort.
	DestroySkirmishAIRecoveryObject(target);
	printf("SKIRMISH_AI_ALLIED_CANCELLATION_FAULT frame=%u release=%u target=%u "
		"held_slots=%d,%d held_members=%u,%u trigger=shared_target_loss\n",
		frame, releaseFrame, targetID, slots[0], slots[1],
		s_allied.cancellationMembers[0], s_allied.cancellationMembers[1]);
	fflush(stdout);
}

Bool RetainHeldAlliedAssaultProbes(Int firstSlot, Int secondSlot)
{
	const Int slots[2] = { firstSlot, secondSlot };
	Int captured[2] = { 0, 0 };
	std::vector<AlliedMovementProbe> probes;
	std::vector<UnsignedInt> rosters[2];
	for (Int participant = 0; participant < 2; ++participant)
	{
		Player *player = ThePlayerList->getPlayerFromSlotIndex(slots[participant]);
		AISkirmishPlayer *skirmish = GetAlliedFixtureAI(slots[participant]);
		Coord3D home;
		if (!player || !skirmish || !skirmish->getBaseCenter(&home)) return FALSE;
		if (!CaptureAlliedFixtureAssaultRoster(skirmish, &rosters[participant]) || rosters[participant].empty()) return FALSE;
		for (Object *object = TheGameLogic->getFirstObject(); object; object = object->getNextObject())
		{
			if (captured[participant] >= 64 || !IsSkirmishAIRecoveryCombatUnit(object, player) ||
				!object->getTeam() || !skirmish->isAlliedAssaultTeam(object->getTeam()->getID()) ||
				!AlliedFixtureAssaultRosterContains(rosters[participant], object->getTeam()->getID()) ||
				!skirmish->isAlliedAssaultHoldingTeam(object->getTeam()->getID())) continue;
			AIUpdateInterface *ai = object->getAIUpdateInterface();
			const Coord3D *guard = ai ? ai->getGuardLocation() : nullptr;
			if (!ai || !guard || ai->getLastCommandSource() != CMD_FROM_AI ||
				ai->getGuardTargetType() != GUARDTARGET_LOCATION) continue;
			const Real dx = guard->x - home.x;
			const Real dy = guard->y - home.y;
			if (dx * dx + dy * dy > 50.0f * 50.0f) continue;
			AlliedMovementProbe probe;
			probe.objectID = object->getID();
			probe.teamID = object->getTeam()->getID();
			probe.participant = participant;
			probe.position = *object->getPosition();
			probes.push_back(probe);
			++captured[participant];
		}
	}
	if (captured[0] == 0 || captured[1] == 0) return FALSE;
	s_allied.assaultProbes.swap(probes);
	for (Int participant = 0; participant < 2; ++participant)
		PrintAlliedFixtureAssaultRoster("SKIRMISH_AI_ALLIED_ADMITTED_ROSTER_ASSERT", TheGameLogic->getFrame(), slots[participant], rosters[participant]);
	printf("SKIRMISH_AI_ALLIED_HELD_PROBES_ASSERT frame=%u slots=%d,%d members=%d,%d "
		"offensive_hold_predicate=1 admitted_roster_membership=1 ai_home_guard=1\n", TheGameLogic->getFrame(),
		firstSlot, secondSlot, captured[0], captured[1]);
	fflush(stdout);
	return TRUE;
}

void ObserveAlliedCoordination(UnsignedInt frame)
{
	if (!TheAI || !TheAI->hasAlliedEvaluation()) return;
	const UnsignedInt nextEvaluation = TheAI->getNextAlliedEvaluationFrame();
	if (s_allied.nextEvaluation == nextEvaluation) return;
	s_allied.nextEvaluation = nextEvaluation;
	for (Int slot = 1; slot <= 7; ++slot)
	{
		AISkirmishPlayer *ai = GetAlliedFixtureAI(slot);
		Player *player = ThePlayerList->getPlayerFromSlotIndex(slot);
		if (!ai || !player) continue;
		const AISkirmishPlayer::AlliedCoordinationDiagnostics a = ai->getAlliedCoordinationDiagnostics();
		const SkirmishAIAlliedPlayerFacts *facts = TheAI->getAlliedPlayerFacts(player->getPlayerIndex());
		if (facts && facts->valid && facts->alive && facts->mode == SKIRMISH_STRATEGY_FORTIFY)
		{
			if (a.assaultActive)
			{
				FailSkirmishAITest("allied_fortify_joined_assault");
				RequestSkirmishAITestStop();
				return;
			}
			s_allied.sawFortifyDecline = TRUE;
		}
		if (!a.assaultActive || a.assaultLaunched || a.assaultFrame <= frame) continue;
		for (Int peer = slot + 1; peer <= 7; ++peer)
		{
			AISkirmishPlayer *other = GetAlliedFixtureAI(peer);
			Player *peerPlayer = ThePlayerList->getPlayerFromSlotIndex(peer);
			if (!other || !peerPlayer) continue;
			const AISkirmishPlayer::AlliedCoordinationDiagnostics b = other->getAlliedCoordinationDiagnostics();
			if (b.assaultActive && !b.assaultLaunched && a.leaderIndex == b.leaderIndex &&
				a.enemyIndex == b.enemyIndex && a.targetID == b.targetID &&
				a.targetID != INVALID_ID && a.assaultFrame == b.assaultFrame &&
				a.assaultExpiryFrame == b.assaultExpiryFrame && a.assaultExpiryFrame > a.assaultFrame)
			{
				if (!TheGameLogic->findObjectByID(a.targetID)) continue;
				s_allied.sawCoordination = TRUE;
				s_allied.coordinatedReleaseFrame = a.assaultFrame;
				if (s_allied.fixtureCase == SKIRMISH_AI_ALLIED_COORDINATION_LIVE &&
					!s_allied.assaultRetained &&
					(s_allied.withdrawalComplete || (a.assaultFrame > frame + 3 &&
						(player->getPlayerIndex() == a.leaderIndex ||
						 peerPlayer->getPlayerIndex() == a.leaderIndex))) &&
					RetainHeldAlliedAssaultProbes(slot, peer))
				{
					s_allied.assaultRetained = TRUE;
					s_allied.assaultSlots[0] = slot;
					s_allied.assaultSlots[1] = peer;
					s_allied.assaultLeader = a.leaderIndex;
					s_allied.assaultEnemy = a.enemyIndex;
					s_allied.assaultTarget = a.targetID;
					s_allied.assaultRelease = a.assaultFrame;
					if (!a.holdAdmissionValid || !b.holdAdmissionValid)
					{ FailSkirmishAITest("allied_retained_hold_admission_missing"); RequestSkirmishAITestStop(); return; }
					s_allied.assaultAdmissionFrames[0] = a.holdAdmissionFrame;
					s_allied.assaultAdmissionFrames[1] = b.holdAdmissionFrame;
					s_allied.assaultExpiry = a.assaultExpiryFrame;
					s_allied.assaultLastIntactFrame = frame;
				}
				++s_allied.checks;
				printf("SKIRMISH_AI_ALLIED_COORDINATION_ASSERT frame=%u slots=%d,%d "
					"leader=%d enemy=%d target=%u release=%u expiry=%u fortify_decline=%d\n",
					frame, slot, peer, a.leaderIndex, a.enemyIndex, a.targetID,
					a.assaultFrame, a.assaultExpiryFrame, s_allied.sawFortifyDecline);
				fflush(stdout);
				if (s_allied.fixtureCase == SKIRMISH_AI_ALLIED_SAVE_LOAD &&
					s_allied.sawAid && !s_allied.saveLoaded &&
					slot != s_allied.aidRecipientSlot && peer != s_allied.aidRecipientSlot &&
					s_allied.aidCooldownUntil > frame &&
					s_allied.aidCooldownUntil - frame >= 20 * LOGICFRAMES_PER_SECOND)
				{
					BeginAlliedCancellation(slot, peer, a.targetID, a.assaultFrame, frame);
					return;
				}
			}
		}
	}
}

Real AlliedFixtureDistanceSquared(const Coord3D &a, const Coord3D &b)
{
	const Real dx = a.x - b.x;
	const Real dy = a.y - b.y;
	return dx * dx + dy * dy;
}

void ObserveAlliedCancellationResume(UnsignedInt frame)
{
	if (!s_allied.cancellationIssued) return;
	Object *lostTarget = TheGameLogic->findObjectByID(s_allied.cancellationTarget);
	if (IsLiveSkirmishAIRecoveryObject(lostTarget)) return;
	Bool bothCanceled = TRUE;
	Bool anyPending = FALSE;
	for (Int participant = 0; participant < 2; ++participant)
	{
		const Int slot = s_allied.cancellationSlots[participant];
		AISkirmishPlayer *ai = GetAlliedFixtureAI(slot);
		Player *player = ThePlayerList->getPlayerFromSlotIndex(slot);
		if (!ai || !player)
		{ FailSkirmishAITest("allied_cancellation_held_member_lost"); RequestSkirmishAITestStop(); return; }
		const AISkirmishPlayer::AlliedCoordinationDiagnostics state = ai->getAlliedCoordinationDiagnostics();
		if (state.assaultLaunched)
		{ FailSkirmishAITest("allied_cancellation_follower_launched"); RequestSkirmishAITestStop(); return; }
		bothCanceled = bothCanceled && !state.assaultActive;
		anyPending = anyPending || state.strategyResumePending;
		// Retire the physical witness after its actual ordinary-order proof;
		// both owners' cancellation and nonlaunch checks remain live.
		if (s_allied.cancellationOrdinaryQualified[participant]) continue;
		Object *member = TheGameLogic->findObjectByID(s_allied.cancellationMembers[participant]);
		if (!IsSkirmishAIRecoveryCombatUnit(member, player) || !member->getTeam() ||
			member->getTeam()->getID() != s_allied.cancellationTeams[participant])
		{ FailSkirmishAITest("allied_cancellation_held_member_lost"); RequestSkirmishAITestStop(); return; }
		if (!s_allied.cancellationSaved || state.assaultActive || state.strategyResumePending) continue;
		// Only an owner saved and rebound with pending=true can qualify a
		// pending-to-clear transition. Other owners qualify ordinary state only.
		if (s_allied.cancellationPendingAtSave[participant] &&
			!s_allied.cancellationPendingAfterLoad[participant])
		{ FailSkirmishAITest("allied_cancellation_pending_transition_unproven"); RequestSkirmishAITestStop(); return; }
		const SkirmishAIAlliedPlayerFacts *facts = TheAI->getAlliedPlayerFacts(player->getPlayerIndex());
		if (!facts || !facts->valid || !facts->alive) continue;
		AIUpdateInterface *unitAI = member->getAIUpdateInterface();
		if (!unitAI || unitAI->getLastCommandSource() != CMD_FROM_AI) continue;
		const SkirmishStrategyMode mode = ai->getAlliedCurrentStrategyMode();
		if (mode != SKIRMISH_STRATEGY_FORTIFY &&
			(facts->immediateThreat >= 60 || facts->baseIntegrity < 65)) continue;
		Bool ordinaryOrder = mode == SKIRMISH_STRATEGY_BALANCED && unitAI->getCurrentStateID() == AI_IDLE;
		if (mode == SKIRMISH_STRATEGY_FORTIFY)
		{
			Coord3D home;
			const Coord3D *guard = unitAI->getGuardLocation();
			ordinaryOrder = ai->getBaseCenter(&home) && guard &&
				unitAI->getCurrentStateID() == AI_GUARD && unitAI->getGuardMode() == GUARDMODE_GUARD_WITHOUT_PURSUIT &&
				unitAI->getGuardTargetType() == GUARDTARGET_LOCATION &&
				AlliedFixtureDistanceSquared(*guard, home) <= 50.0f * 50.0f;
		}
		if (mode == SKIRMISH_STRATEGY_ASSAULT)
		{
			const ObjectID currentTargetID = ai->getAlliedCurrentStrategicTargetID();
			Object *currentTarget = TheGameLogic->findObjectByID(currentTargetID);
			const Coord3D *goal = unitAI->getGoalPosition();
			ordinaryOrder = currentTargetID != s_allied.cancellationTarget &&
				IsLiveSkirmishAIRecoveryObject(currentTarget) && goal &&
				unitAI->getCurrentStateID() == AI_ATTACK_MOVE_TO &&
				AlliedFixtureDistanceSquared(*goal, *currentTarget->getPosition()) <= 200.0f * 200.0f &&
				AlliedFixtureDistanceSquared(*member->getPosition(), s_allied.cancellationPositions[participant]) >= 25.0f * 25.0f;
			if (currentTargetID == INVALID_ID && unitAI->getCurrentStateID() == AI_IDLE)
				ordinaryOrder = TRUE;
		}
		if (!ordinaryOrder) continue;
		s_allied.cancellationOrdinaryQualified[participant] = TRUE;
		++s_allied.checks;
		printf("%s frame=%u slot=%d member=%u mode=%d pending_at_save=%d pending=0 "
			"transition=%s surviving_held_member=1 safe_ordinary_order=1 threat=%d base=%d ai_state=%d "
			"guard_mode=%d fortify_home_guard=%d\n",
			s_allied.cancellationPendingAtSave[participant] ?
				"SKIRMISH_AI_ALLIED_CANCELLATION_RESUME_ASSERT" :
				"SKIRMISH_AI_ALLIED_CANCELLATION_ORDINARY_STATE_ASSERT",
			frame, slot, s_allied.cancellationMembers[participant], static_cast<Int>(mode),
			s_allied.cancellationPendingAtSave[participant],
			s_allied.cancellationPendingAtSave[participant] ? "pending_to_clear" : "not_claimed",
			facts->immediateThreat, facts->baseIntegrity, unitAI->getCurrentStateID(),
			static_cast<Int>(unitAI->getGuardMode()), mode == SKIRMISH_STRATEGY_FORTIFY);
		fflush(stdout);
	}
	if (!s_allied.cancellationSaved)
	{
		if (!bothCanceled)
		{
			if (frame >= s_allied.cancellationRelease)
			{ FailSkirmishAITest("allied_cancellation_missed_release"); RequestSkirmishAITestStop(); }
			return;
		}
		if (!anyPending)
		{ FailSkirmishAITest("allied_cancellation_no_observable_pending_window"); RequestSkirmishAITestStop(); return; }
		if (!RoundTripAlliedFixture())
		{ FailSkirmishAITest("allied_pending_resume_save_load_assertion"); RequestSkirmishAITestStop(); return; }
		s_allied.cancellationSaved = TRUE;
		++s_allied.checks;
		printf("SKIRMISH_AI_ALLIED_CANCELLATION_SAVE_ASSERT frame=%u slots=%d,%d "
			"plans_canceled=2 launches=0 pending_nonzero=1 saved=1\n",
			frame, s_allied.cancellationSlots[0], s_allied.cancellationSlots[1]);
		fflush(stdout);
		return;
	}
	if (s_allied.cancellationOrdinaryQualified[0] && s_allied.cancellationOrdinaryQualified[1] &&
		s_allied.postLoadProtectionVerified)
	{
		s_runner.endFrame = frame;
		RequestSkirmishAITestStop();
	}
}

// Negative phase uses real body damage and normal leader/follower updates.
// Retained identities come from the exact offensive hold predicate above.
Bool HasAlliedFixtureOrdinaryOrder(AISkirmishPlayer *ai, AIUpdateInterface *unitAI)
{
	const SkirmishStrategyMode mode = ai->getAlliedCurrentStrategyMode();
	if (mode == SKIRMISH_STRATEGY_BALANCED) return unitAI->getCurrentStateID() == AI_IDLE;
	if (mode == SKIRMISH_STRATEGY_FORTIFY)
	{
		Coord3D home;
		const Coord3D *guard = unitAI->getGuardLocation();
		return ai->getBaseCenter(&home) && guard && unitAI->getCurrentStateID() == AI_GUARD &&
			unitAI->getGuardMode() == GUARDMODE_GUARD_WITHOUT_PURSUIT && unitAI->getGuardTargetType() == GUARDTARGET_LOCATION &&
			AlliedFixtureDistanceSquared(*guard, home) <= 50.0f * 50.0f;
	}
	if (mode == SKIRMISH_STRATEGY_ASSAULT)
	{
		const ObjectID targetID = ai->getAlliedCurrentStrategicTargetID();
		Object *target = TheGameLogic->findObjectByID(targetID);
		const Coord3D *goal = unitAI->getGoalPosition();
		return (targetID == INVALID_ID && unitAI->getCurrentStateID() == AI_IDLE) ||
			(IsLiveSkirmishAIRecoveryObject(target) && goal && unitAI->getCurrentStateID() == AI_ATTACK_MOVE_TO &&
			 AlliedFixtureDistanceSquared(*goal, *target->getPosition()) <= 200.0f * 200.0f);
	}
	return FALSE;
}

void PrintAlliedWithdrawalSurvivorFailure(UnsignedInt frame, Int failedParticipant)
{
	printf("SKIRMISH_AI_ALLIED_WITHDRAWAL_SURVIVOR_FAILURE frame=%u failed_participant=%d "
		"release=%u canceled=%d ordinary=%d,%d target=%u leader=%d\n", frame, failedParticipant,
		s_allied.assaultRelease, s_allied.withdrawalCanceled, s_allied.withdrawalOrdinary[0],
		s_allied.withdrawalOrdinary[1], s_allied.assaultTarget, s_allied.assaultLeader);
	for (Int participant = 0; participant < 2; ++participant)
	{
		Player *expectedOwner = ThePlayerList->getPlayerFromSlotIndex(s_allied.assaultSlots[participant]);
		AISkirmishPlayer *ai = GetAlliedFixtureAI(s_allied.assaultSlots[participant]);
		Object *member = TheGameLogic->findObjectByID(s_allied.withdrawalMembers[participant]);
		Player *actualOwner = member ? member->getControllingPlayer() : nullptr;
		AIUpdateInterface *unitAI = member ? member->getAIUpdateInterface() : nullptr;
		const Coord3D *position = member ? member->getPosition() : nullptr;
		printf("SKIRMISH_AI_ALLIED_WITHDRAWAL_SURVIVOR_STATE frame=%u participant=%d slot=%d "
			"original_member=%u original_team=%u found=%d expected_owner=%d actual_owner=%d owner_active=%d "
			"actual_team=%u health=%g destroyed=%d dead=%d contained=%d disabled=%d "
			"structure=%d immobile=%d dozer=%d harvester=%d projectile=%d mine=%d can_attack=%d "
			"combat_predicate=%d position=%g,%g,%g ai_state=%d last_source=%d ordinary_already_observed=%d\n",
			frame, participant, s_allied.assaultSlots[participant], s_allied.withdrawalMembers[participant],
			s_allied.withdrawalTeams[participant], member != nullptr, expectedOwner ? expectedOwner->getPlayerIndex() : -1,
			actualOwner ? actualOwner->getPlayerIndex() : -1, expectedOwner ? expectedOwner->isPlayerActive() : FALSE,
			member && member->getTeam() ? member->getTeam()->getID() : 0,
			member && member->getBodyModule() ? member->getBodyModule()->getHealth() : -1.0f,
			member ? member->isDestroyed() : FALSE, member ? member->isEffectivelyDead() : FALSE,
			member ? member->isContained() : FALSE, member ? member->isDisabled() : FALSE,
			member ? member->isKindOf(KINDOF_STRUCTURE) : FALSE, member ? member->isKindOf(KINDOF_IMMOBILE) : FALSE,
			member ? member->isKindOf(KINDOF_DOZER) : FALSE, member ? member->isKindOf(KINDOF_HARVESTER) : FALSE,
			member ? member->isKindOf(KINDOF_PROJECTILE) : FALSE, member ? member->isKindOf(KINDOF_MINE) : FALSE,
			member ? member->isAbleToAttack() : FALSE, IsSkirmishAIRecoveryCombatUnit(member, expectedOwner),
			position ? position->x : 0.0f, position ? position->y : 0.0f, position ? position->z : 0.0f,
			unitAI ? unitAI->getCurrentStateID() : -1, unitAI ? static_cast<Int>(unitAI->getLastCommandSource()) : -1,
			s_allied.withdrawalOrdinary[participant]);
		if (!ai) { printf("SKIRMISH_AI_ALLIED_WITHDRAWAL_OWNER_STATE participant=%d ai_present=0\n", participant); continue; }
		const AISkirmishPlayer::AlliedCoordinationDiagnostics state = ai->getAlliedCoordinationDiagnostics();
		printf("SKIRMISH_AI_ALLIED_WITHDRAWAL_OWNER_STATE participant=%d ai_present=1 current_mode=%d "
			"strategic_target=%u active=%d launched=%d pending=%d admission_valid=%d admission=%u "
			"leader=%d enemy=%d target=%u release=%u expiry=%u home_damage_valid=%d home_damage=%u "
			"held_damage_valid=%d held_damage=%u\n", participant, static_cast<Int>(ai->getAlliedCurrentStrategyMode()),
			ai->getAlliedCurrentStrategicTargetID(), state.assaultActive, state.assaultLaunched, state.strategyResumePending,
			state.holdAdmissionValid, state.holdAdmissionFrame, state.leaderIndex, state.enemyIndex,
			state.targetID, state.assaultFrame, state.assaultExpiryFrame, state.homeDamageValid, state.homeDamageFrame,
			state.heldDamageValid, state.heldDamageFrame);
	}
	fflush(stdout);
}

void ObserveAlliedLeaderWithdrawal(UnsignedInt frame)
{
	if (!s_allied.assaultRetained || s_allied.withdrawalComplete) return;
	Player *players[2] = {
		ThePlayerList->getPlayerFromSlotIndex(s_allied.assaultSlots[0]),
		ThePlayerList->getPlayerFromSlotIndex(s_allied.assaultSlots[1]) };
	Object *target = TheGameLogic->findObjectByID(s_allied.assaultTarget);
	if (!players[0] || !players[1] || !players[0]->isPlayerActive() ||
		!players[1]->isPlayerActive() || !IsLiveSkirmishAIRecoveryObject(target))
	{ FailSkirmishAITest("allied_withdrawal_living_owner_or_target_lost"); RequestSkirmishAITestStop(); return; }
	const Int leaderParticipant = players[0]->getPlayerIndex() == s_allied.assaultLeader ? 0 : 1;
	Player *leader = players[leaderParticipant];
	if (!s_allied.withdrawalFaultIssued)
	{
		if (frame + 3 >= s_allied.assaultRelease)
		{ FailSkirmishAITest("allied_withdrawal_no_pre_release_fault_window"); RequestSkirmishAITestStop(); return; }
		for (Int participant = 0; participant < 2; ++participant)
		{
			AISkirmishPlayer *ai = GetAlliedFixtureAI(s_allied.assaultSlots[participant]);
			if (!ai)
			{ FailSkirmishAITest("allied_withdrawal_owner_missing_before_fault"); RequestSkirmishAITestStop(); return; }
			const AISkirmishPlayer::AlliedCoordinationDiagnostics state = ai->getAlliedCoordinationDiagnostics();
			if (!state.assaultActive || state.assaultLaunched || state.leaderIndex != s_allied.assaultLeader ||
				state.enemyIndex != s_allied.assaultEnemy || state.targetID != s_allied.assaultTarget ||
				state.assaultFrame != s_allied.assaultRelease || state.assaultExpiryFrame != s_allied.assaultExpiry ||
				!state.holdAdmissionValid || state.holdAdmissionFrame != s_allied.assaultAdmissionFrames[participant])
			{ FailSkirmishAITest("allied_withdrawal_pre_fault_hold_changed"); RequestSkirmishAITestStop(); return; }
		}
		// The hold is already observable before this event. Fresh admission
		// clears the old held-hit latch, so a later hit in this frame qualifies.
		Object *members[2] = { nullptr, nullptr };
		for (size_t index = 0; index < s_allied.assaultProbes.size(); ++index)
		{
			const AlliedMovementProbe &probe = s_allied.assaultProbes[index];
			Object *member = TheGameLogic->findObjectByID(probe.objectID);
			if (members[probe.participant] || !IsSkirmishAIRecoveryCombatUnit(member, players[probe.participant]) ||
				!member->getTeam() || member->getTeam()->getID() != probe.teamID ||
				!member->getBodyModule() || member->getBodyModule()->getHealth() <= 2.0f ||
				!GetAlliedFixtureAI(s_allied.assaultSlots[probe.participant])->isAlliedAssaultHoldingTeam(probe.teamID)) continue;
			members[probe.participant] = member;
		}
		Object *source = nullptr;
		for (Object *object = TheGameLogic->getFirstObject(); object; object = object->getNextObject())
		{
			Player *enemy = object->getControllingPlayer();
			if (enemy && enemy->isPlayerActive() && object->getTeam() &&
				leader->getRelationship(object->getTeam()) == ENEMIES &&
				IsSkirmishAIRecoveryCombatUnit(object, enemy)) { source = object; break; }
		}
		if (!members[0] || !members[1] || !source) return;
		// Exercise the actual public command dispatcher while both retained
		// offensive recipients still belong to the unreleased allied hold.
		for (Int participant = 0; participant < 2; ++participant)
		{
			AISkirmishPlayer *ownerAI = GetAlliedFixtureAI(s_allied.assaultSlots[participant]);
			AIUpdateInterface *unitAI = members[participant]->getAIUpdateInterface();
			const Coord3D *guard = unitAI ? unitAI->getGuardLocation() : nullptr;
			Coord3D home;
			if (!ownerAI->shouldHoldAlliedScriptCommand(members[participant]) || !unitAI || !guard ||
				!ownerAI->getBaseCenter(&home) || unitAI->getLastCommandSource() != CMD_FROM_AI ||
				unitAI->getGuardTargetType() != GUARDTARGET_LOCATION ||
				AlliedFixtureDistanceSquared(*guard, home) > 50.0f * 50.0f)
			{ FailSkirmishAITest("allied_script_probe_not_currently_home_held"); RequestSkirmishAITestStop(); return; }
			const Coord3D guardBefore = *guard;
			const UnsignedInt teamBefore = members[participant]->getTeam()->getID();
			const Int stateBefore = unitAI->getCurrentStateID();
			for (Int command = 0; command < 2; ++command)
			{
				if (command == 0)
					unitAI->aiAttackMoveToPosition(target->getPosition(), NO_MAX_SHOTS_LIMIT, CMD_FROM_SCRIPT);
				else
					unitAI->aiMoveToPosition(target->getPosition(), CMD_FROM_SCRIPT);
				guard = unitAI->getGuardLocation();
				const AISkirmishPlayer::AlliedCoordinationDiagnostics state = ownerAI->getAlliedCoordinationDiagnostics();
				if (!members[participant]->getTeam() || members[participant]->getTeam()->getID() != teamBefore ||
					members[participant]->getControllingPlayer() != players[participant] ||
					!guard || unitAI->getLastCommandSource() != CMD_FROM_AI ||
					unitAI->getCurrentStateID() != stateBefore || unitAI->getGuardTargetType() != GUARDTARGET_LOCATION ||
					AlliedFixtureDistanceSquared(*guard, guardBefore) > 1.0f ||
					!ownerAI->shouldHoldAlliedScriptCommand(members[participant]) ||
					!state.assaultActive || state.assaultLaunched || state.targetID != s_allied.assaultTarget ||
					state.leaderIndex != s_allied.assaultLeader || state.assaultFrame != s_allied.assaultRelease)
				{ FailSkirmishAITest("allied_script_command_replaced_held_order"); RequestSkirmishAITestStop(); return; }
				++s_allied.checks;
				printf("SKIRMISH_AI_ALLIED_SCRIPT_HOLD_ASSERT frame=%u release=%u slot=%d member=%u team=%u "
					"command=%s source=script guard_unchanged=1 owner_source=ai launched=0\n",
					frame, s_allied.assaultRelease, s_allied.assaultSlots[participant],
					members[participant]->getID(), members[participant]->getTeam()->getID(),
					command == 0 ? "attack_move" : "move");
				fflush(stdout);
			}
		}
		Object *victim = members[leaderParticipant];
		const UnsignedInt victimTeamID = victim->getTeam()->getID();
		const Real before = victim->getBodyModule()->getHealth();
		const UnsignedInt attackedBefore = leader->getAttackedFrame();
		const ObjectID sourceWitnessID = source->getID();
		Player *sourceOwner = source->getControllingPlayer();
		const UnsignedInt sourceMask = sourceOwner->getPlayerMask();
		if (!sourceMask || (sourceMask & (sourceMask - 1)) != 0 ||
			!sourceOwner->isPlayerActive() || leader->getRelationship(source->getTeam()) != ENEMIES)
		{ FailSkirmishAITest("allied_withdrawal_exact_hostile_mask_unproven"); RequestSkirmishAITestStop(); return; }
		DamageInfo damage;
		damage.in.m_sourceID = INVALID_ID;
		damage.in.m_sourcePlayerMask = sourceMask;
		damage.in.m_damageType = DAMAGE_UNRESISTABLE;
		damage.in.m_amount = 1.0f;
		const ObjectID victimID = victim->getID();
		victim->attemptDamage(&damage);
		victim = TheGameLogic->findObjectByID(victimID);
		if (!IsSkirmishAIRecoveryCombatUnit(victim, leader) || !victim->getBodyModule())
		{ FailSkirmishAITest("allied_withdrawal_damage_killed_member"); RequestSkirmishAITestStop(); return; }
		members[leaderParticipant] = victim;
		const Real after = victim->getBodyModule()->getHealth();
		if (after <= 0 || after >= before || damage.out.m_actualDamageClipped <= 0 ||
			!IsLiveSkirmishAIRecoveryObject(target))
		{ FailSkirmishAITest("allied_withdrawal_real_damage_boundary_unproven"); RequestSkirmishAITestStop(); return; }
		AISkirmishPlayer *leaderAI = GetAlliedFixtureAI(s_allied.assaultSlots[leaderParticipant]);
		if (!leaderAI)
		{ FailSkirmishAITest("allied_withdrawal_damage_owner_missing"); RequestSkirmishAITestStop(); return; }
		const AISkirmishPlayer::AlliedCoordinationDiagnostics hitState = leaderAI->getAlliedCoordinationDiagnostics();
		if (!hitState.heldDamageValid || hitState.heldDamageFrame != frame)
		{ FailSkirmishAITest("allied_withdrawal_masked_hit_not_latched"); RequestSkirmishAITestStop(); return; }
		// Heal through the real body boundary before any ordinary owner update.
		// Healing replaces the body's last-damage record but must retain the hit.
		DamageInfo healing;
		healing.in.m_sourceID = INVALID_ID;
		healing.in.m_damageType = DAMAGE_HEALING;
		healing.in.m_amount = 1.0f;
		victim->attemptDamage(&healing);
		victim = TheGameLogic->findObjectByID(victimID);
		source = TheGameLogic->findObjectByID(sourceWitnessID);
		target = TheGameLogic->findObjectByID(s_allied.assaultTarget);
		const AISkirmishPlayer::AlliedCoordinationDiagnostics healedState = leaderAI->getAlliedCoordinationDiagnostics();
		if (!IsSkirmishAIRecoveryCombatUnit(victim, leader) || !victim->getBodyModule() ||
			!victim->getTeam() || victim->getTeam()->getID() != victimTeamID ||
			victim->getBodyModule()->getHealth() <= after || victim->getBodyModule()->getHealth() < before ||
			healing.out.m_actualDamageClipped >= 0 || !leader->isPlayerActive() ||
			!IsLiveSkirmishAIRecoveryObject(source) || source->getControllingPlayer() != sourceOwner ||
			!sourceOwner->isPlayerActive() || !source->getTeam() || leader->getRelationship(source->getTeam()) != ENEMIES ||
			!IsLiveSkirmishAIRecoveryObject(target) || !healedState.heldDamageValid || healedState.heldDamageFrame != frame ||
			healedState.homeDamageValid != hitState.homeDamageValid || healedState.homeDamageFrame != hitState.homeDamageFrame)
		{ FailSkirmishAITest("allied_withdrawal_healing_erased_masked_hit"); RequestSkirmishAITestStop(); return; }
		members[leaderParticipant] = victim;
		for (Int participant = 0; participant < 2; ++participant)
		{
			s_allied.withdrawalMembers[participant] = members[participant]->getID();
			s_allied.withdrawalTeams[participant] = members[participant]->getTeam()->getID();
		}
		s_allied.withdrawalFaultIssued = TRUE;
		printf("SKIRMISH_AI_ALLIED_WITHDRAWAL_FAULT frame=%u release=%u leader=%d source_witness=%u victim=%u "
			"health_before=%g health_after=%g attacked_before=%u attacked_after=%u target=%u admission=%u "
			"masked_source_hit=1 source_mask=%u health_healed=%g healing_clipped=%g held_damage_frame=%u "
			"healed_before_owner_update=1 same_frame_admission=%d nonlethal=1\n",
			frame, s_allied.assaultRelease, s_allied.assaultLeader, source->getID(), victim->getID(),
			before, after, attackedBefore, leader->getAttackedFrame(), s_allied.assaultTarget,
			s_allied.assaultAdmissionFrames[leaderParticipant], sourceMask, victim->getBodyModule()->getHealth(),
			healing.out.m_actualDamageClipped, healedState.heldDamageFrame,
			frame == s_allied.assaultAdmissionFrames[leaderParticipant]);
		fflush(stdout);
		return;
	}
	Bool bothCanceled = TRUE;
	for (Int participant = 0; participant < 2; ++participant)
	{
		AISkirmishPlayer *ai = GetAlliedFixtureAI(s_allied.assaultSlots[participant]);
		if (!ai)
		{ PrintAlliedWithdrawalSurvivorFailure(frame, participant);
		  FailSkirmishAITest("allied_withdrawal_held_survivor_lost"); RequestSkirmishAITestStop(); return; }
		const AISkirmishPlayer::AlliedCoordinationDiagnostics state = ai->getAlliedCoordinationDiagnostics();
		if (state.assaultLaunched)
		{ FailSkirmishAITest("allied_withdrawal_retained_owner_launched"); RequestSkirmishAITestStop(); return; }
		bothCanceled = bothCanceled && !state.assaultActive;
		if (s_allied.withdrawalOrdinary[participant]) continue;
		Object *member = TheGameLogic->findObjectByID(s_allied.withdrawalMembers[participant]);
		if (!IsSkirmishAIRecoveryCombatUnit(member, players[participant]) ||
			!member->getTeam() || member->getTeam()->getID() != s_allied.withdrawalTeams[participant])
		{ PrintAlliedWithdrawalSurvivorFailure(frame, participant);
		  FailSkirmishAITest("allied_withdrawal_held_survivor_lost"); RequestSkirmishAITestStop(); return; }
		if (!s_allied.withdrawalCanceled || state.assaultActive || state.strategyResumePending) continue;
		const SkirmishAIAlliedPlayerFacts *facts = TheAI->getAlliedPlayerFacts(players[participant]->getPlayerIndex());
		if (!facts || !facts->valid || !facts->alive) continue;
		AIUpdateInterface *unitAI = member->getAIUpdateInterface();
		if (!unitAI || unitAI->getLastCommandSource() != CMD_FROM_AI) continue;
		const SkirmishStrategyMode mode = ai->getAlliedCurrentStrategyMode();
		if (mode != SKIRMISH_STRATEGY_FORTIFY &&
			(facts->immediateThreat >= 60 || facts->baseIntegrity < 65)) continue;
		const Bool ordinary = HasAlliedFixtureOrdinaryOrder(ai, unitAI);
		if (ordinary && !s_allied.withdrawalOrdinary[participant])
		{
			s_allied.withdrawalOrdinary[participant] = TRUE;
			++s_allied.checks;
			printf("SKIRMISH_AI_ALLIED_WITHDRAWAL_RESUME_ASSERT frame=%u slot=%d member=%u team=%u mode=%d "
				"pending=0 safe_ordinary_order=1 surviving_held_member=1 threat=%d base=%d ai_state=%d "
				"guard_mode=%d fortify_home_guard=%d\n", frame,
				s_allied.assaultSlots[participant], member->getID(), member->getTeam()->getID(), static_cast<Int>(mode),
				facts->immediateThreat, facts->baseIntegrity, unitAI->getCurrentStateID(),
				static_cast<Int>(unitAI->getGuardMode()), mode == SKIRMISH_STRATEGY_FORTIFY);
			fflush(stdout);
		}
	}
	if (!s_allied.withdrawalCanceled)
	{
		if (frame >= s_allied.assaultRelease)
		{ FailSkirmishAITest("allied_withdrawal_cancellation_missed_release"); RequestSkirmishAITestStop(); return; }
		if (!bothCanceled) return;
		s_allied.withdrawalCanceled = TRUE;
		++s_allied.checks;
		printf("SKIRMISH_AI_ALLIED_WITHDRAWAL_CANCEL_ASSERT frame=%u release=%u leader=%d "
			"owners_canceled=2 launches=0 living_target=%u\n", frame, s_allied.assaultRelease,
			s_allied.assaultLeader, s_allied.assaultTarget);
		fflush(stdout);
	}
	if (s_allied.withdrawalOrdinary[0] && s_allied.withdrawalOrdinary[1] && bothCanceled)
	{
		s_allied.withdrawalComplete = TRUE;
		// Reset fixture observations only; a later natural cohort must supply
		// new pre-release hold identities and the successful release evidence.
		s_allied.assaultRetained = FALSE;
		s_allied.assaultProbes.clear();
	}
}

void PrintAlliedAssaultFailure(UnsignedInt frame, const char *reason)
{
	Object *target = TheGameLogic->findObjectByID(s_allied.assaultTarget);
	Player *targetOwner = target ? target->getControllingPlayer() : nullptr;
	printf("SKIRMISH_AI_ALLIED_ASSAULT_FAILURE frame=%u reason=%s retained=%d launched=%d moved=%d,%d "
		"return_fire=%d fault_frame=%u fault_evaluation=%u saved_slots=%d,%d saved_leader=%d saved_enemy=%d "
		"saved_target=%u saved_release=%u saved_expiry=%u saved_admission=%u,%u target_found=%d target_dead=%d "
		"target_destroyed=%d target_owner=%d target_owner_active=%d probes=%u\n", frame, reason,
		s_allied.assaultRetained, s_allied.assaultLaunched, s_allied.assaultMoved[0], s_allied.assaultMoved[1],
		s_allied.returnFireIssued, s_allied.returnFireFrame, s_allied.returnFireEvaluation,
		s_allied.assaultSlots[0], s_allied.assaultSlots[1], s_allied.assaultLeader, s_allied.assaultEnemy,
		s_allied.assaultTarget, s_allied.assaultRelease, s_allied.assaultExpiry,
		s_allied.assaultAdmissionFrames[0], s_allied.assaultAdmissionFrames[1], target != nullptr,
		target ? target->isEffectivelyDead() : FALSE, target ? target->isDestroyed() : FALSE,
		targetOwner ? targetOwner->getPlayerIndex() : -1, targetOwner ? targetOwner->isPlayerActive() : FALSE,
		static_cast<UnsignedInt>(s_allied.assaultProbes.size()));
	printf("SKIRMISH_AI_ALLIED_ASSAULT_ABORT_MONITOR active=%d canceled=%d attempt=%d abort_frame=%u "
		"ordinary=%d,%d members=%u,%u teams=%u,%u last_intact=%u\n", s_allied.positiveAbortActive,
		s_allied.positiveAbortCanceled, s_allied.positiveAbortCount, s_allied.positiveAbortFrame,
		s_allied.positiveAbortOrdinary[0], s_allied.positiveAbortOrdinary[1],
		s_allied.positiveAbortMembers[0], s_allied.positiveAbortMembers[1],
		s_allied.positiveAbortTeams[0], s_allied.positiveAbortTeams[1], s_allied.assaultLastIntactFrame);
	for (Int participant = 0; participant < 2; ++participant)
	{
		Player *owner = ThePlayerList->getPlayerFromSlotIndex(s_allied.assaultSlots[participant]);
		AISkirmishPlayer *ai = GetAlliedFixtureAI(s_allied.assaultSlots[participant]);
		const SkirmishAIAlliedPlayerFacts *facts = owner && TheAI ? TheAI->getAlliedPlayerFacts(owner->getPlayerIndex()) : nullptr;
		printf("SKIRMISH_AI_ALLIED_ASSAULT_OWNER participant=%d slot=%d owner=%d owner_active=%d ai_present=%d "
			"target_relationship=%d facts_present=%d facts_valid=%d facts_alive=%d economy=%d base=%d army=%d threat=%d\n",
			participant, s_allied.assaultSlots[participant], owner ? owner->getPlayerIndex() : -1,
			owner ? owner->isPlayerActive() : FALSE, ai != nullptr,
			owner && target && target->getTeam() ? static_cast<Int>(owner->getRelationship(target->getTeam())) : -1,
			facts != nullptr, facts ? facts->valid : FALSE, facts ? facts->alive : FALSE,
			facts ? facts->economyHealth : -1, facts ? facts->baseIntegrity : -1,
			facts ? facts->armyReadiness : -1, facts ? facts->immediateThreat : -1);
		if (!ai) continue;
		const AISkirmishPlayer::AlliedCoordinationDiagnostics state = ai->getAlliedCoordinationDiagnostics();
		printf("SKIRMISH_AI_ALLIED_ASSAULT_STATE participant=%d mode=%d strategic_target=%u active=%d launched=%d "
			"pending=%d admission_valid=%d admission=%u leader=%d enemy=%d target=%u release=%u expiry=%u "
			"home_damage_valid=%d home_damage=%u held_damage_valid=%d held_damage=%u\n", participant,
			static_cast<Int>(ai->getAlliedCurrentStrategyMode()), ai->getAlliedCurrentStrategicTargetID(),
			state.assaultActive, state.assaultLaunched, state.strategyResumePending, state.holdAdmissionValid,
			state.holdAdmissionFrame, state.leaderIndex, state.enemyIndex, state.targetID, state.assaultFrame,
			state.assaultExpiryFrame, state.homeDamageValid, state.homeDamageFrame, state.heldDamageValid, state.heldDamageFrame);
	}
	for (size_t index = 0; index < s_allied.assaultProbes.size(); ++index)
	{
		const AlliedMovementProbe &probe = s_allied.assaultProbes[index];
		Object *member = TheGameLogic->findObjectByID(probe.objectID);
		Player *owner = member ? member->getControllingPlayer() : nullptr;
		const Coord3D *position = member ? member->getPosition() : nullptr;
		AIUpdateInterface *unitAI = member ? member->getAIUpdateInterface() : nullptr;
		printf("SKIRMISH_AI_ALLIED_ASSAULT_PROBE participant=%d original_member=%u original_team=%u found=%d "
			"owner=%d team=%u health=%g dead=%d destroyed=%d contained=%d position=%g,%g,%g ai_state=%d last_source=%d\n",
			probe.participant, probe.objectID, probe.teamID, member != nullptr, owner ? owner->getPlayerIndex() : -1,
			member && member->getTeam() ? member->getTeam()->getID() : 0,
			member && member->getBodyModule() ? member->getBodyModule()->getHealth() : -1.0f,
			member ? member->isEffectivelyDead() : FALSE, member ? member->isDestroyed() : FALSE,
			member ? member->isContained() : FALSE, position ? position->x : 0.0f,
			position ? position->y : 0.0f, position ? position->z : 0.0f,
			unitAI ? unitAI->getCurrentStateID() : -1, unitAI ? static_cast<Int>(unitAI->getLastCommandSource()) : -1);
	}
	fflush(stdout);
}

void ObserveAlliedLaunchedReturnFire(UnsignedInt frame)
{
	Object *target = TheGameLogic->findObjectByID(s_allied.assaultTarget);
	if (!IsLiveSkirmishAIRecoveryObject(target))
	{ PrintAlliedAssaultFailure(frame, "allied_return_fire_shared_target_lost"); FailSkirmishAITest("allied_return_fire_shared_target_lost"); RequestSkirmishAITestStop(); return; }
	if (!s_allied.returnFireIssued)
	{
		const UnsignedInt nextEvaluation = TheAI ? TheAI->getNextAlliedEvaluationFrame() : 0;
		if (!TheAI || !TheAI->hasAlliedEvaluation() || nextEvaluation <= frame || nextEvaluation >= s_allied.assaultExpiry) return;
		for (Int participant = 0; participant < 2; ++participant)
		{
			Player *owner = ThePlayerList->getPlayerFromSlotIndex(s_allied.assaultSlots[participant]);
			AISkirmishPlayer *ownerAI = GetAlliedFixtureAI(s_allied.assaultSlots[participant]);
			Object *member = TheGameLogic->findObjectByID(s_allied.assaultMovedMembers[participant]);
			Coord3D home;
			if (!owner || !owner->isPlayerActive() || !ownerAI || !ownerAI->getBaseCenter(&home) ||
				!IsSkirmishAIRecoveryCombatUnit(member, owner) || !member->getBodyModule() ||
				member->getBodyModule()->getHealth() <= 2.0f || !member->getTeam() ||
				member->getTeam()->getID() != s_allied.assaultMovedTeams[participant]) continue;
			const Real homeRadius = ownerAI->getAlliedSafetyHomeRadius();
			if (AlliedFixtureDistanceSquared(*member->getPosition(), home) <= homeRadius * homeRadius ||
				owner->getAttackedFrame() == frame) continue;
			Object *source = nullptr;
			for (Object *object = TheGameLogic->getFirstObject(); object; object = object->getNextObject())
			{
				Player *enemy = object->getControllingPlayer();
				if (enemy && enemy->isPlayerActive() && object->getTeam() &&
					owner->getRelationship(object->getTeam()) == ENEMIES &&
					IsSkirmishAIRecoveryCombatUnit(object, enemy)) { source = object; break; }
			}
			if (!source) continue;
			const ObjectID memberID = member->getID();
			const ObjectID sourceID = source->getID();
			const Real before = member->getBodyModule()->getHealth();
			const UnsignedInt attackedBefore = owner->getAttackedFrame();
			DamageInfo damage;
			damage.in.m_sourceID = source->getID();
			damage.in.m_sourcePlayerMask = source->getControllingPlayer()->getPlayerMask();
			damage.in.m_damageType = DAMAGE_UNRESISTABLE;
			damage.in.m_amount = 1.0f;
			member->attemptDamage(&damage);
			member = TheGameLogic->findObjectByID(memberID);
			source = TheGameLogic->findObjectByID(sourceID);
			target = TheGameLogic->findObjectByID(s_allied.assaultTarget);
			if (!owner->isPlayerActive() || !IsLiveSkirmishAIRecoveryObject(source) ||
				!IsLiveSkirmishAIRecoveryObject(target) || !IsSkirmishAIRecoveryCombatUnit(member, owner) || !member->getBodyModule() ||
				!member->getTeam() || member->getTeam()->getID() != s_allied.assaultMovedTeams[participant] ||
				member->getBodyModule()->getHealth() <= 0 || member->getBodyModule()->getHealth() >= before ||
				damage.out.m_actualDamageClipped <= 0 || owner->getAttackedFrame() != frame ||
				owner->getAttackedFrame() == attackedBefore)
			{ PrintAlliedAssaultFailure(frame, "allied_return_fire_real_nonlethal_damage_unproven"); FailSkirmishAITest("allied_return_fire_real_nonlethal_damage_unproven"); RequestSkirmishAITestStop(); return; }
			s_allied.returnFireIssued = TRUE;
			s_allied.returnFireFrame = frame;
			s_allied.returnFireEvaluation = nextEvaluation;
			s_allied.returnFireMember = memberID;
			s_allied.returnFireTeam = s_allied.assaultMovedTeams[participant];
			s_allied.returnFireSource = source->getID();
			s_allied.returnFireParticipant = participant;
			printf("SKIRMISH_AI_ALLIED_LAUNCHED_RETURN_FIRE_FAULT frame=%u expiry=%u evaluation=%u slot=%d "
				"member=%u team=%u source=%u target=%u home_radius=%.1f health_before=%g health_after=%g "
				"damage_clipped=%g attacked_before=%u attacked_after=%u outside_home=1 nonlethal=1\n",
				frame, s_allied.assaultExpiry, nextEvaluation, s_allied.assaultSlots[participant], memberID,
				s_allied.returnFireTeam, source->getID(), s_allied.assaultTarget, homeRadius, before,
				member->getBodyModule()->getHealth(), damage.out.m_actualDamageClipped, attackedBefore, owner->getAttackedFrame());
			fflush(stdout);
			return;
		}
		return;
	}
	Player *owner = ThePlayerList->getPlayerFromSlotIndex(s_allied.assaultSlots[s_allied.returnFireParticipant]);
	Object *member = TheGameLogic->findObjectByID(s_allied.returnFireMember);
	Object *source = TheGameLogic->findObjectByID(s_allied.returnFireSource);
	if (!owner || !owner->isPlayerActive() || !IsSkirmishAIRecoveryCombatUnit(member, owner) ||
		!member->getTeam() || member->getTeam()->getID() != s_allied.returnFireTeam ||
		!IsLiveSkirmishAIRecoveryObject(source) || !source->getControllingPlayer() ||
		!source->getControllingPlayer()->isPlayerActive() || !source->getTeam() || owner->getRelationship(source->getTeam()) != ENEMIES)
	{ PrintAlliedAssaultFailure(frame, "allied_return_fire_living_witness_lost"); FailSkirmishAITest("allied_return_fire_living_witness_lost"); RequestSkirmishAITestStop(); return; }
	// Observe normal owner updates and a genuinely later central evaluation.
	// Tactical retaliation may change the unit order; the unchanged launched
	// cohort is checked by ObserveAlliedAssaultLaunch on every intervening frame.
	if (frame <= s_allied.returnFireFrame || frame < s_allied.returnFireEvaluation ||
		!TheAI || TheAI->getNextAlliedEvaluationFrame() <= s_allied.returnFireEvaluation) return;
	++s_allied.checks;
	printf("SKIRMISH_AI_ALLIED_LAUNCHED_RETURN_FIRE_ASSERT frame=%u fault_frame=%u evaluation=%u "
		"member=%u team=%u target=%u leader=%d release=%u expiry=%u cohorts_retained=2 living_witnesses=1\n",
		frame, s_allied.returnFireFrame, s_allied.returnFireEvaluation, s_allied.returnFireMember,
		s_allied.returnFireTeam, s_allied.assaultTarget, s_allied.assaultLeader, s_allied.assaultRelease, s_allied.assaultExpiry);
	fflush(stdout);
	s_runner.endFrame = frame;
	RequestSkirmishAITestStop();
}

Bool BeginAlliedPositiveLeaderAbort(UnsignedInt frame)
{
	if (!s_allied.withdrawalComplete || s_allied.assaultLaunched || s_allied.returnFireIssued ||
		frame >= s_allied.assaultRelease || s_allied.assaultLastIntactFrame >= frame) return FALSE;
	Int leaderParticipant = -1;
	AISkirmishPlayer::AlliedCoordinationDiagnostics states[2];
	Player *owners[2];
	Object *target = TheGameLogic->findObjectByID(s_allied.assaultTarget);
	if (!IsLiveSkirmishAIRecoveryObject(target) || !target->getControllingPlayer() ||
		!target->getControllingPlayer()->isPlayerActive() ||
		target->getControllingPlayer()->getPlayerIndex() != s_allied.assaultEnemy || !target->getTeam()) return FALSE;
	for (Int participant = 0; participant < 2; ++participant)
	{
		owners[participant] = ThePlayerList->getPlayerFromSlotIndex(s_allied.assaultSlots[participant]);
		AISkirmishPlayer *ai = GetAlliedFixtureAI(s_allied.assaultSlots[participant]);
		if (!owners[participant] || !owners[participant]->isPlayerActive() || !ai ||
			owners[participant]->getRelationship(target->getTeam()) != ENEMIES) return FALSE;
		states[participant] = ai->getAlliedCoordinationDiagnostics();
		if (states[participant].assaultLaunched) return FALSE;
		if (states[participant].strategyResumePending &&
			(!states[participant].holdAdmissionValid ||
			 states[participant].holdAdmissionFrame != s_allied.assaultAdmissionFrames[participant])) return FALSE;
		if (owners[participant]->getPlayerIndex() == s_allied.assaultLeader) leaderParticipant = participant;
		// A follower may still observe the original announcement for one owner
		// update. An active replacement identity is never a legitimate retry.
		if (states[participant].assaultActive &&
			(states[participant].leaderIndex != s_allied.assaultLeader || states[participant].enemyIndex != s_allied.assaultEnemy ||
			 states[participant].targetID != s_allied.assaultTarget || states[participant].assaultFrame != s_allied.assaultRelease ||
			 states[participant].assaultExpiryFrame != s_allied.assaultExpiry || !states[participant].holdAdmissionValid ||
			 states[participant].holdAdmissionFrame != s_allied.assaultAdmissionFrames[participant])) return FALSE;
	}
	if (leaderParticipant < 0 || states[leaderParticipant].assaultActive) return FALSE;
	const AISkirmishPlayer::AlliedCoordinationDiagnostics &leader = states[leaderParticipant];
	const UnsignedInt admission = s_allied.assaultAdmissionFrames[leaderParticipant];
	const Bool heldHit = leader.heldDamageValid && leader.heldDamageFrame >= admission &&
		leader.heldDamageFrame <= frame && frame - leader.heldDamageFrame <= 10 * LOGICFRAMES_PER_SECOND;
	const Bool homeHit = leader.homeDamageValid && leader.homeDamageFrame >= admission &&
		leader.homeDamageFrame <= frame && frame - leader.homeDamageFrame <= 10 * LOGICFRAMES_PER_SECOND;
	if (!heldHit && !homeHit) return FALSE;
	ObjectID members[2] = { INVALID_ID, INVALID_ID };
	UnsignedInt teams[2] = { 0, 0 };
	Real health[2] = { -1.0f, -1.0f };
	for (size_t index = 0; index < s_allied.assaultProbes.size(); ++index)
	{
		const AlliedMovementProbe &probe = s_allied.assaultProbes[index];
		Object *member = TheGameLogic->findObjectByID(probe.objectID);
		if (!IsSkirmishAIRecoveryCombatUnit(member, owners[probe.participant]) || !member->getBodyModule() ||
			!member->getTeam() || member->getTeam()->getID() != probe.teamID ||
			member->getBodyModule()->getHealth() <= health[probe.participant]) continue;
		members[probe.participant] = probe.objectID;
		teams[probe.participant] = probe.teamID;
		health[probe.participant] = member->getBodyModule()->getHealth();
	}
	if (members[0] == INVALID_ID || members[1] == INVALID_ID) return FALSE;
	s_allied.positiveAbortActive = TRUE;
	s_allied.positiveAbortCanceled = FALSE;
	s_allied.positiveAbortFrame = frame;
	++s_allied.positiveAbortCount;
	for (Int participant = 0; participant < 2; ++participant)
	{
		s_allied.positiveAbortMembers[participant] = members[participant];
		s_allied.positiveAbortTeams[participant] = teams[participant];
		s_allied.positiveAbortOrdinary[participant] = FALSE;
	}
	printf("SKIRMISH_AI_ALLIED_POSITIVE_LEADER_ABORT frame=%u attempt=%d last_intact=%u slots=%d,%d leader=%d "
		"enemy=%d target=%u release=%u expiry=%u admission=%u held_hit=%d held_hit_frame=%u home_hit=%d home_hit_frame=%u "
		"members=%u,%u teams=%u,%u target_alive=1 launches=0\n", frame, s_allied.positiveAbortCount,
		s_allied.assaultLastIntactFrame, s_allied.assaultSlots[0], s_allied.assaultSlots[1], s_allied.assaultLeader,
		s_allied.assaultEnemy, s_allied.assaultTarget, s_allied.assaultRelease, s_allied.assaultExpiry, admission,
		heldHit, leader.heldDamageFrame, homeHit, leader.homeDamageFrame, members[0], members[1], teams[0], teams[1]);
	fflush(stdout);
	return TRUE;
}

void ObserveAlliedPositiveLeaderAbort(UnsignedInt frame)
{
	Bool bothCanceled = TRUE;
	for (Int participant = 0; participant < 2; ++participant)
	{
		Player *owner = ThePlayerList->getPlayerFromSlotIndex(s_allied.assaultSlots[participant]);
		AISkirmishPlayer *ai = GetAlliedFixtureAI(s_allied.assaultSlots[participant]);
		if (!owner || !owner->isPlayerActive() || !ai)
		{ PrintAlliedAssaultFailure(frame, "allied_positive_abort_owner_lost"); FailSkirmishAITest("allied_positive_abort_owner_lost"); RequestSkirmishAITestStop(); return; }
		const AISkirmishPlayer::AlliedCoordinationDiagnostics state = ai->getAlliedCoordinationDiagnostics();
		if (state.assaultLaunched || (s_allied.positiveAbortCanceled && state.assaultActive) || (state.assaultActive &&
			(state.leaderIndex != s_allied.assaultLeader || state.enemyIndex != s_allied.assaultEnemy ||
			 state.targetID != s_allied.assaultTarget || state.assaultFrame != s_allied.assaultRelease ||
			 state.assaultExpiryFrame != s_allied.assaultExpiry || !state.holdAdmissionValid ||
			 state.holdAdmissionFrame != s_allied.assaultAdmissionFrames[participant])) ||
			(state.strategyResumePending && (!state.holdAdmissionValid ||
			 state.holdAdmissionFrame != s_allied.assaultAdmissionFrames[participant])))
		{ PrintAlliedAssaultFailure(frame, "allied_positive_abort_launch_or_identity_changed"); FailSkirmishAITest("allied_positive_abort_launch_or_identity_changed"); RequestSkirmishAITestStop(); return; }
		bothCanceled = bothCanceled && !state.assaultActive;
		if (s_allied.positiveAbortOrdinary[participant]) continue;
		Object *member = TheGameLogic->findObjectByID(s_allied.positiveAbortMembers[participant]);
		if (!IsSkirmishAIRecoveryCombatUnit(member, owner) || !member->getTeam() ||
			member->getTeam()->getID() != s_allied.positiveAbortTeams[participant])
		{ PrintAlliedAssaultFailure(frame, "allied_positive_abort_survivor_lost"); FailSkirmishAITest("allied_positive_abort_survivor_lost"); RequestSkirmishAITestStop(); return; }
		if (state.assaultActive || state.strategyResumePending) continue;
		const SkirmishAIAlliedPlayerFacts *facts = TheAI->getAlliedPlayerFacts(owner->getPlayerIndex());
		AIUpdateInterface *unitAI = member->getAIUpdateInterface();
		if (!facts || !facts->valid || !facts->alive || !unitAI || unitAI->getLastCommandSource() != CMD_FROM_AI) continue;
		const SkirmishStrategyMode mode = ai->getAlliedCurrentStrategyMode();
		if (mode != SKIRMISH_STRATEGY_FORTIFY && (facts->immediateThreat >= 60 || facts->baseIntegrity < 65)) continue;
		const Bool ordinary = HasAlliedFixtureOrdinaryOrder(ai, unitAI);
		if (!ordinary) continue;
		s_allied.positiveAbortOrdinary[participant] = TRUE;
		++s_allied.checks;
		printf("SKIRMISH_AI_ALLIED_POSITIVE_ABORT_RESUME_ASSERT frame=%u attempt=%d abort_frame=%u slot=%d "
			"member=%u team=%u mode=%d pending=0 threat=%d base=%d ai_state=%d source=ai surviving_original_held_member=1\n",
			frame, s_allied.positiveAbortCount, s_allied.positiveAbortFrame, s_allied.assaultSlots[participant],
			member->getID(), member->getTeam()->getID(), static_cast<Int>(mode), facts->immediateThreat,
			facts->baseIntegrity, unitAI->getCurrentStateID());
		fflush(stdout);
	}
	if (!s_allied.positiveAbortCanceled)
	{
		if (frame >= s_allied.assaultRelease)
		{ PrintAlliedAssaultFailure(frame, "allied_positive_abort_cancellation_missed_release"); FailSkirmishAITest("allied_positive_abort_cancellation_missed_release"); RequestSkirmishAITestStop(); return; }
		if (bothCanceled)
		{
			s_allied.positiveAbortCanceled = TRUE;
			printf("SKIRMISH_AI_ALLIED_POSITIVE_ABORT_CANCEL_ASSERT frame=%u attempt=%d release=%u owners_canceled=2 launches=0\n",
				frame, s_allied.positiveAbortCount, s_allied.assaultRelease);
			fflush(stdout);
		}
	}
	if (!bothCanceled || !s_allied.positiveAbortOrdinary[0] || !s_allied.positiveAbortOrdinary[1]) return;
	printf("SKIRMISH_AI_ALLIED_POSITIVE_ABORT_RETIRED frame=%u attempt=%d abort_frame=%u release=%u "
		"owners_canceled=2 launches=0 ordinary_proofs=2 deadline_start=%u\n", frame, s_allied.positiveAbortCount,
		s_allied.positiveAbortFrame, s_allied.assaultRelease, s_allied.startFrame);
	fflush(stdout);
	// Retire only observations of this fully proven aborted attempt. The
	// initial negative proof and fixed total deadline are never restarted.
	s_allied.positiveAbortActive = FALSE;
	s_allied.assaultRetained = FALSE;
	s_allied.assaultLaunched = FALSE;
	s_allied.assaultProbes.clear();
	s_allied.assaultMoved[0] = s_allied.assaultMoved[1] = FALSE;
	s_allied.assaultMovedMembers[0] = s_allied.assaultMovedMembers[1] = INVALID_ID;
	s_allied.assaultMovedTeams[0] = s_allied.assaultMovedTeams[1] = 0;
	s_allied.returnFireIssued = FALSE;
	s_allied.returnFireFrame = s_allied.returnFireEvaluation = 0;
	s_allied.returnFireMember = s_allied.returnFireSource = INVALID_ID;
	s_allied.returnFireTeam = 0;
	s_allied.returnFireParticipant = -1;
}

void ObserveAlliedAssaultLaunch(UnsignedInt frame)
{
	if (!s_allied.assaultRetained) return;
	if (s_allied.positiveAbortActive) { ObserveAlliedPositiveLeaderAbort(frame); return; }
	if (frame >= s_allied.assaultExpiry)
	{ PrintAlliedAssaultFailure(frame, "allied_assault_expired_without_live_movement"); FailSkirmishAITest("allied_assault_expired_without_live_movement"); RequestSkirmishAITestStop(); return; }
	Bool bothLaunched = TRUE;
	for (Int participant = 0; participant < 2; ++participant)
	{
		AISkirmishPlayer *ai = GetAlliedFixtureAI(s_allied.assaultSlots[participant]);
		Player *owner = ThePlayerList->getPlayerFromSlotIndex(s_allied.assaultSlots[participant]);
		if (!ai || !owner || !owner->isPlayerActive()) { PrintAlliedAssaultFailure(frame, "allied_assault_participant_lost"); FailSkirmishAITest("allied_assault_participant_lost"); RequestSkirmishAITestStop(); return; }
		const AISkirmishPlayer::AlliedCoordinationDiagnostics state = ai->getAlliedCoordinationDiagnostics();
		if (!state.assaultActive || state.leaderIndex != s_allied.assaultLeader ||
			state.enemyIndex != s_allied.assaultEnemy || state.targetID != s_allied.assaultTarget ||
			state.assaultFrame != s_allied.assaultRelease || state.assaultExpiryFrame != s_allied.assaultExpiry ||
			!state.holdAdmissionValid || state.holdAdmissionFrame != s_allied.assaultAdmissionFrames[participant])
		{ if (BeginAlliedPositiveLeaderAbort(frame)) { ObserveAlliedPositiveLeaderAbort(frame); return; }
		  PrintAlliedAssaultFailure(frame, "allied_assault_retained_commitment_changed"); FailSkirmishAITest("allied_assault_retained_commitment_changed"); RequestSkirmishAITestStop(); return; }
		if (s_allied.assaultLaunched && !state.assaultLaunched)
		{ PrintAlliedAssaultFailure(frame, "allied_assault_launched_state_lost"); FailSkirmishAITest("allied_assault_launched_state_lost"); RequestSkirmishAITestStop(); return; }
		if (state.assaultLaunched && frame < s_allied.assaultRelease)
		{ PrintAlliedAssaultFailure(frame, "allied_assault_launched_before_release"); FailSkirmishAITest("allied_assault_launched_before_release"); RequestSkirmishAITestStop(); return; }
		bothLaunched = bothLaunched && state.assaultLaunched;
	}
	s_allied.assaultLastIntactFrame = frame;
	if (frame < s_allied.assaultRelease || !bothLaunched) return;
	if (!s_allied.assaultLaunched)
	{
		Object *target = TheGameLogic->findObjectByID(s_allied.assaultTarget);
		if (!IsLiveSkirmishAIRecoveryObject(target))
		{ PrintAlliedAssaultFailure(frame, "allied_assault_launch_target_lost"); FailSkirmishAITest("allied_assault_launch_target_lost"); RequestSkirmishAITestStop(); return; }
		s_allied.assaultLaunched = TRUE;
		s_allied.assaultTargetPosition = *target->getPosition();
		Int surviving[2] = { 0, 0 };
		for (size_t index = 0; index < s_allied.assaultProbes.size(); ++index)
		{
			AlliedMovementProbe &probe = s_allied.assaultProbes[index];
			Object *object = TheGameLogic->findObjectByID(probe.objectID);
			Player *player = ThePlayerList->getPlayerFromSlotIndex(s_allied.assaultSlots[probe.participant]);
			if (!IsSkirmishAIRecoveryCombatUnit(object, player) || !object->getTeam() ||
				object->getTeam()->getID() != probe.teamID) continue;
			// Retain the pre-release identities, but measure forward movement
			// from the release boundary so earlier home-guard travel cannot pass.
			probe.position = *object->getPosition();
			++surviving[probe.participant];
		}
		if (surviving[0] == 0 || surviving[1] == 0)
		{ PrintAlliedAssaultFailure(frame, "allied_assault_held_members_lost_before_release"); FailSkirmishAITest("allied_assault_held_members_lost_before_release"); RequestSkirmishAITestStop(); return; }
		printf("SKIRMISH_AI_ALLIED_LAUNCH_ASSERT frame=%u release=%u slots=%d,%d target=%u probes=%u\n",
			frame, s_allied.assaultRelease, s_allied.assaultSlots[0], s_allied.assaultSlots[1],
			s_allied.assaultTarget, static_cast<UnsignedInt>(s_allied.assaultProbes.size()));
		fflush(stdout);
		++s_allied.checks;
		return;
	}
	Int surviving[2] = { 0, 0 };
	for (size_t index = 0; index < s_allied.assaultProbes.size(); ++index)
	{
		const AlliedMovementProbe &probe = s_allied.assaultProbes[index];
		Object *object = TheGameLogic->findObjectByID(probe.objectID);
		Player *player = ThePlayerList->getPlayerFromSlotIndex(s_allied.assaultSlots[probe.participant]);
		if (!IsSkirmishAIRecoveryCombatUnit(object, player) || !object->getTeam() ||
			object->getTeam()->getID() != probe.teamID) continue;
		++surviving[probe.participant];
		if (s_allied.assaultMoved[probe.participant]) continue;
		AIUpdateInterface *ai = object->getAIUpdateInterface();
		const Coord3D *goal = ai ? ai->getGoalPosition() : nullptr;
		// Formation destinations may be offset from the common target point.
		// Require the actual AI attack-move order and observable forward travel.
		if (!ai || ai->getLastCommandSource() != CMD_FROM_AI ||
			ai->getCurrentStateID() != AI_ATTACK_MOVE_TO || !goal ||
			AlliedFixtureDistanceSquared(*goal, s_allied.assaultTargetPosition) > 200.0f * 200.0f ||
			AlliedFixtureDistanceSquared(*object->getPosition(), probe.position) < 25.0f * 25.0f ||
			AlliedFixtureDistanceSquared(*object->getPosition(), s_allied.assaultTargetPosition) >=
			AlliedFixtureDistanceSquared(probe.position, s_allied.assaultTargetPosition)) continue;
		s_allied.assaultMoved[probe.participant] = TRUE;
		s_allied.assaultMovedMembers[probe.participant] = probe.objectID;
		s_allied.assaultMovedTeams[probe.participant] = probe.teamID;
		++s_allied.checks;
		printf("SKIRMISH_AI_ALLIED_MOVEMENT_ASSERT frame=%u slot=%d object=%u team=%u "
			"release=%u ai_attack_move=1 forward_displacement_min=25\n", frame,
			s_allied.assaultSlots[probe.participant], probe.objectID, probe.teamID, s_allied.assaultRelease);
		fflush(stdout);
	}
	if (surviving[0] == 0 || surviving[1] == 0)
	{ PrintAlliedAssaultFailure(frame, "allied_assault_retained_members_no_survivor"); FailSkirmishAITest("allied_assault_retained_members_no_survivor"); RequestSkirmishAITestStop(); return; }
	for (Int participant = 0; participant < 2; ++participant)
		if (s_allied.assaultMoved[participant])
		{
			Object *member = TheGameLogic->findObjectByID(s_allied.assaultMovedMembers[participant]);
			Player *player = ThePlayerList->getPlayerFromSlotIndex(s_allied.assaultSlots[participant]);
			if (!IsSkirmishAIRecoveryCombatUnit(member, player) || !member->getTeam() ||
				member->getTeam()->getID() != s_allied.assaultMovedTeams[participant])
			{ PrintAlliedAssaultFailure(frame, "allied_assault_qualified_member_lost"); FailSkirmishAITest("allied_assault_qualified_member_lost"); RequestSkirmishAITestStop(); return; }
		}
	if (s_allied.assaultMoved[0] && s_allied.assaultMoved[1] && s_allied.sawFortifyDecline)
		ObserveAlliedLaunchedReturnFire(frame);
}

Bool CheckAlliedSupportScriptHold(AISkirmishPlayer *ownerAI, Player *owner, Object *member,
	const Coord3D &scriptGoal, const char *phase, UnsignedInt frame)
{
	AIUpdateInterface *unitAI = member->getAIUpdateInterface();
	const Coord3D *guard = unitAI ? unitAI->getGuardLocation() : nullptr;
	if (!ownerAI->isAlliedSupportMember(member) || !ownerAI->shouldHoldAlliedScriptCommand(member) ||
		!unitAI || !guard || unitAI->getLastCommandSource() != CMD_FROM_AI ||
		unitAI->getGuardTargetType() != GUARDTARGET_LOCATION)
	{ FailSkirmishAITest("allied_support_script_probe_not_owned_guard"); RequestSkirmishAITestStop(); return FALSE; }
	const Coord3D guardBefore = *guard;
	const Int stateBefore = unitAI->getCurrentStateID();
	const AISkirmishPlayer::AlliedCoordinationDiagnostics assignmentBefore = ownerAI->getAlliedCoordinationDiagnostics();
	for (Int command = 0; command < 2; ++command)
	{
		if (command == 0) unitAI->aiAttackMoveToPosition(&scriptGoal, NO_MAX_SHOTS_LIMIT, CMD_FROM_SCRIPT);
		else unitAI->aiMoveToPosition(&scriptGoal, CMD_FROM_SCRIPT);
		guard = unitAI->getGuardLocation();
		if (!IsSkirmishAIRecoveryCombatUnit(member, owner) || !member->getTeam() ||
			member->getID() != s_allied.supportMemberID || member->getTeam()->getID() != s_allied.supportTeamID ||
			!guard || unitAI->getLastCommandSource() != CMD_FROM_AI || unitAI->getCurrentStateID() != stateBefore ||
			unitAI->getGuardTargetType() != GUARDTARGET_LOCATION ||
			AlliedFixtureDistanceSquared(*guard, guardBefore) > 1.0f ||
			!ownerAI->isAlliedSupportMember(member) || !ownerAI->shouldHoldAlliedScriptCommand(member) ||
			!SameAlliedDiagnostics(assignmentBefore, ownerAI->getAlliedCoordinationDiagnostics()))
		{ FailSkirmishAITest("allied_support_script_command_replaced_owned_guard"); RequestSkirmishAITestStop(); return FALSE; }
		++s_allied.checks;
		printf("SKIRMISH_AI_ALLIED_SUPPORT_SCRIPT_HOLD_ASSERT frame=%u phase=%s member=%u team=%u "
			"command=%s source=script guard_unchanged=1 owner_source=ai assignment_unchanged=1\n",
			frame, phase, member->getID(), member->getTeam()->getID(), command == 0 ? "attack_move" : "move");
		fflush(stdout);
	}
	return TRUE;
}

void ObserveAlliedSupport(UnsignedInt frame)
{
	if (s_allied.supportDonorSlot >= 0)
	{
		AISkirmishPlayer *donorAI = GetAlliedFixtureAI(s_allied.supportDonorSlot);
		Player *donor = ThePlayerList->getPlayerFromSlotIndex(s_allied.supportDonorSlot);
		Object *member = TheGameLogic->findObjectByID(s_allied.supportMemberID);
		if (!donorAI || !donor || !IsSkirmishAIRecoveryCombatUnit(member, donor) ||
			!member->getTeam() || member->getTeam()->getID() != s_allied.supportTeamID)
		{ FailSkirmishAITest("allied_support_tracked_member_lost"); RequestSkirmishAITestStop(); return; }
		Coord3D home;
		if (!donorAI->getBaseCenter(&home))
		{ FailSkirmishAITest("allied_support_home_unavailable"); RequestSkirmishAITestStop(); return; }
		const Real homeRadius = donorAI->getAlliedSupportHomeRadius();
		const AISkirmishPlayer::AlliedCoordinationDiagnostics state = donorAI->getAlliedCoordinationDiagnostics();
		if (!s_allied.supportFaultIssued)
		{
			Player *recipient = FindAlliedFixturePlayer(s_allied.supportRecipientIndex);
			Bool retainedTeam = FALSE;
			for (Int team = 0; team < state.supportTeamCount; ++team)
				retainedTeam = retainedTeam || donorAI->getAlliedSupportTeamID(team) == s_allied.supportTeamID;
			if (!recipient || !recipient->isPlayerActive() || !retainedTeam || state.supportReturning ||
				state.supportRecipientIndex != s_allied.supportRecipientIndex)
			{ FailSkirmishAITest("allied_support_withdrawn_before_departure"); RequestSkirmishAITestStop(); return; }
			AIUpdateInterface *ai = member->getAIUpdateInterface();
			const Coord3D *guard = ai ? ai->getGuardLocation() : nullptr;
			if (!ai || ai->getLastCommandSource() != CMD_FROM_AI ||
				ai->getGuardTargetType() != GUARDTARGET_LOCATION || !guard ||
				AlliedFixtureDistanceSquared(*member->getPosition(), s_allied.supportInitialPosition) < 25.0f * 25.0f ||
				AlliedFixtureDistanceSquared(*member->getPosition(), home) <=
				(homeRadius + 100.0f) * (homeRadius + 100.0f) ||
				AlliedFixtureDistanceSquared(*member->getPosition(), *guard) > 300.0f * 300.0f) return;
			// Protected tactical ownership is deliberately outside the script
			// gate. Keep this witness pinned and wait for real support eligibility.
			if (!donorAI->isAlliedSupportMember(member) || !donorAI->shouldHoldAlliedScriptCommand(member)) return;
			Bool guardedAllyAnchor = FALSE;
			for (Object *anchor = TheGameLogic->getFirstObject(); anchor; anchor = anchor->getNextObject())
				if (IsLiveSkirmishAIRecoveryObject(anchor) && anchor->getControllingPlayer() == recipient &&
					(anchor->isKindOf(KINDOF_STRUCTURE) || anchor->isKindOf(KINDOF_DOZER)) &&
					AlliedFixtureDistanceSquared(*guard, *anchor->getPosition()) <= 50.0f * 50.0f)
				{ guardedAllyAnchor = TRUE; break; }
			if (!guardedAllyAnchor) return;
			if (!CheckAlliedSupportScriptHold(donorAI, donor, member, home, "assigned", frame)) return;
			s_allied.supportAssignedScriptChecked = TRUE;
			s_allied.supportAwayPosition = *member->getPosition();
			s_allied.supportFaultIssued = TRUE;
			++s_allied.checks;
			printf("SKIRMISH_AI_ALLIED_SUPPORT_DEPARTURE_ASSERT frame=%u donor_slot=%d "
				"recipient=%d team=%u member=%u moved_min=25 ally_guarded=1 outside_home=1\n",
				frame, s_allied.supportDonorSlot, s_allied.supportRecipientIndex,
				s_allied.supportTeamID, s_allied.supportMemberID);
			fflush(stdout);
			// Only fault after this same surviving member has actually reached
			// its ally guard away from home. Ordinary AI owns all return orders.
			recipient->killPlayer();
			if (recipient->isPlayerActive())
			{ FailSkirmishAITest("allied_support_fault_not_inactive"); RequestSkirmishAITestStop(); }
			return;
		}
		if (state.supportRecipientIndex < 0 && state.supportReturning && state.supportTeamCount > 0)
		{
			s_allied.sawSupportReturning = TRUE;
			AIUpdateInterface *unitAI = member->getAIUpdateInterface();
			const Coord3D *guard = unitAI ? unitAI->getGuardLocation() : nullptr;
			if (!s_allied.supportReturningScriptChecked && unitAI && guard &&
				donorAI->isAlliedSupportMember(member) && donorAI->shouldHoldAlliedScriptCommand(member) &&
				unitAI->getLastCommandSource() == CMD_FROM_AI && unitAI->getGuardTargetType() == GUARDTARGET_LOCATION &&
				AlliedFixtureDistanceSquared(*guard, home) <= 50.0f * 50.0f &&
				AlliedFixtureDistanceSquared(*member->getPosition(), home) > homeRadius * homeRadius)
			{
				if (!CheckAlliedSupportScriptHold(donorAI, donor, member, s_allied.supportAwayPosition, "returning", frame)) return;
				s_allied.supportReturningScriptChecked = TRUE;
			}
		}
		if (state.supportTeamCount == 0 && state.supportRecipientIndex < 0 && !state.supportReturning)
		{
			if (!s_allied.sawSupportReturning || !s_allied.supportAssignedScriptChecked ||
				!s_allied.supportReturningScriptChecked ||
				AlliedFixtureDistanceSquared(*member->getPosition(), home) > homeRadius * homeRadius ||
				AlliedFixtureDistanceSquared(*member->getPosition(), s_allied.supportAwayPosition) < 25.0f * 25.0f)
			{ FailSkirmishAITest("allied_support_cleared_without_surviving_home_return"); RequestSkirmishAITestStop(); return; }
			++s_allied.checks;
			printf("SKIRMISH_AI_ALLIED_SUPPORT_RECALL_ASSERT frame=%u donor_slot=%d "
				"recipient=%d team=%u member=%u returning_observed=1 surviving_home_return=1 home_radius=%.1f\n",
				frame, s_allied.supportDonorSlot, s_allied.supportRecipientIndex,
				s_allied.supportTeamID, s_allied.supportMemberID, homeRadius);
			fflush(stdout);
			s_runner.endFrame = frame;
			RequestSkirmishAITestStop();
		}
		return;
	}
	for (Int slot = 1; slot <= 7; ++slot)
	{
		AISkirmishPlayer *donor = GetAlliedFixtureAI(slot);
		if (!donor) continue;
		const AISkirmishPlayer::AlliedCoordinationDiagnostics state = donor->getAlliedCoordinationDiagnostics();
		if (state.supportTeamCount <= 0 || state.supportRecipientIndex < 0) continue;
		Player *recipient = FindAlliedFixturePlayer(state.supportRecipientIndex);
		if (!recipient || !recipient->isPlayerActive()) continue;
		const SkirmishAIAlliedPlayerFacts *facts = TheAI->getAlliedPlayerFacts(
			ThePlayerList->getPlayerFromSlotIndex(slot)->getPlayerIndex());
		if (!facts || !facts->valid || facts->immediateThreat >= 50 ||
			facts->supportAvailableValue <= 0) continue;
		Coord3D home;
		if (!donor->getBaseCenter(&home)) continue;
		Object *trackedMember = nullptr;
		UnsignedInt trackedTeam = 0;
		for (Int team = 0; team < state.supportTeamCount && !trackedMember; ++team)
		{
			const UnsignedInt teamID = donor->getAlliedSupportTeamID(team);
			if (teamID == 0)
			{ FailSkirmishAITest("allied_support_missing_team_id"); RequestSkirmishAITestStop(); return; }
			for (Object *object = TheGameLogic->getFirstObject(); object; object = object->getNextObject())
				if (IsSkirmishAIRecoveryCombatUnit(object, ThePlayerList->getPlayerFromSlotIndex(slot)) &&
					object->getTeam() && object->getTeam()->getID() == teamID &&
					donor->isAlliedSupportMember(object) && donor->shouldHoldAlliedScriptCommand(object) &&
					AlliedFixtureDistanceSquared(*object->getPosition(), home) <=
					(donor->getAlliedSupportHomeRadius() + 350.0f) * (donor->getAlliedSupportHomeRadius() + 350.0f))
				{ trackedMember = object; trackedTeam = teamID; break; }
		}
		if (!trackedMember || state.supportReturning) continue;
		s_allied.supportDonorSlot = slot;
		s_allied.supportRecipientIndex = state.supportRecipientIndex;
		s_allied.supportTeamID = trackedTeam;
		s_allied.supportMemberID = trackedMember->getID();
		s_allied.supportInitialPosition = *trackedMember->getPosition();
		++s_allied.checks;
		printf("SKIRMISH_AI_ALLIED_SUPPORT_DISPATCH_ASSERT frame=%u donor_slot=%d "
			"recipient=%d support_teams=%d tracked_team=%u tracked_member=%u surplus_value=%d departure=pending\n", frame,
			slot, state.supportRecipientIndex, state.supportTeamCount,
			trackedTeam, trackedMember->getID(), facts->supportAvailableValue);
		fflush(stdout);
		return;
	}
}

void ObserveAlliedAid(UnsignedInt frame)
{
	if (!TheAI || !TheAI->hasAlliedEvaluation()) return;
	if (!s_allied.aidFaultApplied)
	{
		// In the save/load case establish a real future commitment first, then
		// apply the recovery fault while that cohort still has time to assemble.
		if (s_allied.fixtureCase == SKIRMISH_AI_ALLIED_SAVE_LOAD &&
			(s_allied.coordinatedReleaseFrame <= frame ||
			 s_allied.coordinatedReleaseFrame - frame < 10 * LOGICFRAMES_PER_SECOND)) return;
		for (Int donorSlot = 1; donorSlot <= 4; ++donorSlot)
		{
			Player *donor = ThePlayerList->getPlayerFromSlotIndex(donorSlot);
			const SkirmishAIAlliedPlayerFacts *healthy = donor ?
				TheAI->getAlliedPlayerFacts(donor->getPlayerIndex()) : nullptr;
			if (!healthy || !healthy->valid || !healthy->alive || !healthy->isAI ||
				healthy->economyHealth < 75 || healthy->baseIntegrity < 80 ||
				healthy->immediateThreat > 40 || healthy->donationBlocked) continue;
			for (Int recipientSlot = 1; recipientSlot <= 4; ++recipientSlot)
			{
				if (recipientSlot == donorSlot) continue;
				Player *recipient = ThePlayerList->getPlayerFromSlotIndex(recipientSlot);
				if (!recipient || !recipient->isPlayerActive() ||
					donor->getRelationship(recipient->getDefaultTeam()) != ALLIES ||
					recipient->getRelationship(donor->getDefaultTeam()) != ALLIES) continue;
				AISkirmishPlayer *recipientAI = GetAlliedFixtureAI(recipientSlot);
				if (!recipientAI || recipientAI->getAlliedCoordinationDiagnostics().receiptCooldownActive ||
					recipientAI->getAlliedCoordinationDiagnostics().donationCooldownActive ||
					TheAI->getAlliedRecipientReliefUntil(recipient->getPlayerIndex()) != 0) continue;
				Object *builder = nullptr;
				for (Object *object = TheGameLogic->getFirstObject(); object; object = object->getNextObject())
					if (IsLiveSkirmishAIRecoveryObject(object) && object->getControllingPlayer() == recipient &&
						object->isKindOf(KINDOF_DOZER)) { builder = object; break; }
				if (!builder) continue;
				const ObjectID builderID = builder->getID();
				for (Object *object = TheGameLogic->getFirstObject(); object; )
				{
					Object *next = object->getNextObject();
					if (object != builder && object->getControllingPlayer() == recipient &&
						IsLiveSkirmishAIRecoveryObject(object)) DestroySkirmishAIRecoveryObject(object);
					object = next;
				}
				for (BuildListInfo *info = recipient->getBuildList(); info; info = info->getNext())
					info->setNumRebuilds(0);
				SetSkirmishAIRecoveryCash(recipient, 10);
				SetSkirmishAIRecoveryCash(donor, 100000);
				s_allied.aidDonorSlot = donorSlot;
				s_allied.aidRecipientSlot = recipientSlot;
				s_allied.aidFaultApplied = TRUE;
				s_allied.aidEvaluation = TheAI->getNextAlliedEvaluationFrame();
				printf("SKIRMISH_AI_ALLIED_AID_FAULT frame=%u donor_slot=%d recipient_slot=%d "
					"builder=%u recipient_cash=10 donor_cash=100000 captured_economy=%d captured_base=%d\n",
					frame, donorSlot, recipientSlot, builderID, healthy->economyHealth, healthy->baseIntegrity);
				fflush(stdout);
				return;
			}
		}
		return;
	}
	AISkirmishPlayer *donorAI = GetAlliedFixtureAI(s_allied.aidDonorSlot);
	AISkirmishPlayer *recipientAI = GetAlliedFixtureAI(s_allied.aidRecipientSlot);
	Player *recipient = ThePlayerList->getPlayerFromSlotIndex(s_allied.aidRecipientSlot);
	if (!donorAI || !recipientAI || !recipient || !recipient->isPlayerActive())
	{ FailSkirmishAITest("allied_aid_participant_lost"); RequestSkirmishAITestStop(); return; }
	const AISkirmishPlayer::AlliedCoordinationDiagnostics donor = donorAI->getAlliedCoordinationDiagnostics();
	const AISkirmishPlayer::AlliedCoordinationDiagnostics received = recipientAI->getAlliedCoordinationDiagnostics();
	const Int recipientIndex = recipient->getPlayerIndex();
	const UnsignedInt nextEvaluation = TheAI->getNextAlliedEvaluationFrame();
	if (!s_allied.sawAid && nextEvaluation != s_allied.aidEvaluation)
	{
		s_allied.aidEvaluation = nextEvaluation;
		const Int streak = TheAI->getAlliedStarvationStreak(recipientIndex);
		const SkirmishAIAlliedPlayerFacts *facts = TheAI->getAlliedPlayerFacts(recipientIndex);
		if (streak == 1 && facts && facts->missingIncome && facts->missingProduction &&
			facts->recoverable && facts->economyHealth <= 25)
		{
			s_allied.sawFirstStarvationEvaluation = TRUE;
			s_allied.firstStarvationFrame = frame;
			++s_allied.checks;
		}
		if (received.receiptCooldownActive && donor.donationCooldownActive)
		{
			// Successful aid consumes the central starvation episode and resets
			// its streak. The prior observed starving batch establishes evaluation
			// one; this distinct five-second batch establishes evaluation two.
			if (!s_allied.sawFirstStarvationEvaluation || streak != 0 || !facts ||
				!facts->missingIncome || !facts->missingProduction || !facts->recoverable ||
				facts->economyHealth > 25 ||
				frame - s_allied.firstStarvationFrame < 5 * LOGICFRAMES_PER_SECOND - 1 ||
				donor.nextDonationFrame != received.mayDonateFrame ||
				TheAI->getAlliedRecipientReliefUntil(recipientIndex) != received.mayDonateFrame ||
				received.mayDonateFrame <= frame ||
				received.mayDonateFrame - frame < 120 * LOGICFRAMES_PER_SECOND - 1 ||
				received.mayDonateFrame - frame > 120 * LOGICFRAMES_PER_SECOND)
			{ FailSkirmishAITest("allied_aid_cooldown_or_two_evaluation_assertion"); RequestSkirmishAITestStop(); return; }
			s_allied.sawAid = TRUE;
			s_allied.aidCooldownUntil = received.mayDonateFrame;
			++s_allied.checks;
			printf("SKIRMISH_AI_ALLIED_AID_ASSERT frame=%u donor_slot=%d recipient_slot=%d "
				"starvation_evaluations=2 consumed_streak=0 cooldown_until=%u donor_recipient_relief_equal=1\n",
				frame, s_allied.aidDonorSlot, s_allied.aidRecipientSlot, received.mayDonateFrame);
			fflush(stdout);
		}
	}
	if (s_allied.sawAid && frame < s_allied.aidCooldownUntil)
	{
		if (donor.nextDonationFrame != s_allied.aidCooldownUntil ||
			received.mayDonateFrame != s_allied.aidCooldownUntil ||
			received.donationCooldownActive || !received.receiptCooldownActive)
		{ FailSkirmishAITest("allied_aid_cooldown_or_relay_changed"); RequestSkirmishAITestStop(); return; }
	}
	if (s_allied.fixtureCase == SKIRMISH_AI_ALLIED_SAVE_LOAD && s_allied.saveLoaded)
	{
		if (frame >= s_allied.aidCooldownUntil)
		{ FailSkirmishAITest("allied_save_load_protection_expired"); RequestSkirmishAITestStop(); return; }
		if (nextEvaluation != s_allied.postLoadEvaluation)
		{
			s_allied.postLoadEvaluation = nextEvaluation;
			Player *donorPlayer = ThePlayerList->getPlayerFromSlotIndex(s_allied.aidDonorSlot);
			const SkirmishAIAlliedPlayerFacts *donorFacts = donorPlayer ?
				TheAI->getAlliedPlayerFacts(donorPlayer->getPlayerIndex()) : nullptr;
			const SkirmishAIAlliedPlayerFacts *recipientFacts = TheAI->getAlliedPlayerFacts(recipientIndex);
			if (!donorFacts || !recipientFacts || !donorFacts->valid || !recipientFacts->valid ||
				!donorFacts->donationBlocked || !recipientFacts->donationBlocked || !recipientFacts->aidBlocked ||
				TheAI->getAlliedRecipientReliefUntil(recipientIndex) != s_allied.aidCooldownUntil)
			{ FailSkirmishAITest("allied_save_load_protection_not_enforced"); RequestSkirmishAITestStop(); return; }
			++s_allied.postLoadBlockedEvaluations;
			++s_allied.checks;
			printf("SKIRMISH_AI_ALLIED_SAVE_LOAD_PROTECTION_ASSERT frame=%u evaluation=%d "
				"cooldown_until=%u donor_blocked=1 recipient_aid_blocked=1 relay_blocked=1\n",
				frame, s_allied.postLoadBlockedEvaluations, s_allied.aidCooldownUntil);
			fflush(stdout);
			if (s_allied.postLoadBlockedEvaluations >= 2)
			{
				s_allied.postLoadProtectionVerified = TRUE;
			}
		}
	}
	if (s_allied.sawAid && frame >= s_allied.aidCooldownUntil &&
		s_allied.fixtureCase == SKIRMISH_AI_ALLIED_AID_LIFECYCLE)
	{
		++s_allied.checks;
		printf("SKIRMISH_AI_ALLIED_AID_NO_RELAY_ASSERT frame=%u through_frame=%u\n",
			frame, s_allied.aidCooldownUntil - 1);
		fflush(stdout);
		s_runner.endFrame = frame;
		RequestSkirmishAITestStop();
	}
}

void PrintAlliedFixtureTimeoutDiagnostics(UnsignedInt frame)
{
	// Read the existing owner state and last immutable batch. Do not capture a
	// new decision, refresh metrics, or change any assertion at the deadline.
	printf("SKIRMISH_AI_ALLIED_TIMEOUT_STATE frame=%u start_frame=%u elapsed_frames=%u case=%s checks=%u "
		"game_mode=%d replay=%d recorder_epoch=%d central_evaluated=%d central_next=%u observer_next=%u "
		"coordination_seen=%d fortify_decline=%d retained=%d probes=%u withdrawal_fault=%d withdrawal_canceled=%d "
		"withdrawal_complete=%d launched=%d retained_slots=%d,%d leader=%d target=%u release=%u expiry=%u "
		"aid_fault=%d aid_seen=%d cancellation_issued=%d cancellation_saved=%d support_fault=%d support_returning=%d\n",
		frame, s_allied.startFrame, frame - s_allied.startFrame, s_alliedCaseNames[s_allied.fixtureCase], s_allied.checks,
		static_cast<Int>(TheGameLogic->getGameMode()), TheGameLogic->isInReplayGame(),
		TheRecorder ? TheRecorder->getSkirmishAIReplayEpoch() : -1,
		TheAI ? TheAI->hasAlliedEvaluation() : FALSE, TheAI ? TheAI->getNextAlliedEvaluationFrame() : 0,
		s_allied.nextEvaluation, s_allied.sawCoordination, s_allied.sawFortifyDecline, s_allied.assaultRetained,
		static_cast<UnsignedInt>(s_allied.assaultProbes.size()), s_allied.withdrawalFaultIssued, s_allied.withdrawalCanceled,
		s_allied.withdrawalComplete, s_allied.assaultLaunched, s_allied.assaultSlots[0], s_allied.assaultSlots[1],
		s_allied.assaultLeader, s_allied.assaultTarget, s_allied.assaultRelease, s_allied.assaultExpiry,
		s_allied.aidFaultApplied, s_allied.sawAid, s_allied.cancellationIssued, s_allied.cancellationSaved,
		s_allied.supportFaultIssued, s_allied.sawSupportReturning);
	if (!ThePlayerList) { fflush(stdout); return; }
	for (Int slot = 0; slot < SKIRMISH_AI_TEST_SLOT_COUNT; ++slot)
	{
		Player *player = ThePlayerList->getPlayerFromSlotIndex(slot);
		if (!player)
		{ printf("SKIRMISH_AI_ALLIED_TIMEOUT_PLAYER slot=%d present=0\n", slot); continue; }
		AISkirmishPlayer *ai = GetAlliedFixtureAI(slot);
		const SkirmishAIAlliedPlayerFacts *facts = TheAI ? TheAI->getAlliedPlayerFacts(player->getPlayerIndex()) : nullptr;
		printf("SKIRMISH_AI_ALLIED_TIMEOUT_PLAYER slot=%d present=1 index=%d type=%d active=%d skirmish_ai=%d "
			"money_present=%d actual_cash=%u attacked_frame=%u cached_facts=%d\n", slot, player->getPlayerIndex(),
			static_cast<Int>(player->getPlayerType()), player->isPlayerActive(), ai != nullptr,
			player->getMoney() != nullptr, player->getMoney() ? player->getMoney()->countMoney() : 0,
			player->getAttackedFrame(), facts != nullptr);
		if (facts)
			printf("SKIRMISH_AI_ALLIED_TIMEOUT_FACTS slot=%d index=%d valid=%d alive=%d is_ai=%d mode=%d "
				"economy=%d base=%d army=%d threat=%d ready_force=%d missing_income=%d missing_production=%d recoverable=%d "
				"cash=%d starvation_cash_limit=%d protected_reserve=%d combat=%d local_combat=%d local_enemy=%d surplus=%d "
				"allied_mask=%08X enemy_mask=%08X target_enemy=%d target=%u target_score=%d donation_blocked=%d aid_blocked=%d\n",
				slot, facts->playerIndex, facts->valid, facts->alive, facts->isAI, static_cast<Int>(facts->mode),
				facts->economyHealth, facts->baseIntegrity, facts->armyReadiness, facts->immediateThreat, facts->hasReadyForce,
				facts->missingIncome, facts->missingProduction, facts->recoverable, facts->cash, facts->starvationCashLimit,
				facts->protectedReserve, facts->combatValue, facts->localCombatValue, facts->localEnemyValue,
				facts->supportAvailableValue, facts->alliedMask, facts->enemyMask, facts->targetEnemyIndex,
				facts->targetObjectID, facts->targetScore, facts->donationBlocked, facts->aidBlocked);
		if (!ai) continue;
		const AISkirmishPlayer::AlliedCoordinationDiagnostics state = ai->getAlliedCoordinationDiagnostics();
		printf("SKIRMISH_AI_ALLIED_TIMEOUT_ASSAULT slot=%d current_mode=%d strategic_target=%u "
			"active=%d launched=%d resume_pending=%d admission_valid=%d admission=%u leader=%d enemy=%d target=%u release=%u expiry=%u "
			"support_recipient=%d support_teams=%d support_returning=%d donation_cooldown=%d next_donation=%u "
			"receipt_cooldown=%d may_donate=%u\n", slot, static_cast<Int>(ai->getAlliedCurrentStrategyMode()),
			ai->getAlliedCurrentStrategicTargetID(), state.assaultActive, state.assaultLaunched, state.strategyResumePending,
			state.holdAdmissionValid, state.holdAdmissionFrame, state.leaderIndex, state.enemyIndex, state.targetID, state.assaultFrame, state.assaultExpiryFrame,
			state.supportRecipientIndex, state.supportTeamCount, state.supportReturning,
			state.donationCooldownActive, state.nextDonationFrame, state.receiptCooldownActive, state.mayDonateFrame);
	}
	fflush(stdout);
}
#endif

void UpdateSkirmishAIAlliedFixture()
{
	const UnsignedInt frame = TheGameLogic->getFrame();
	if (frame < 2) return;
	if (s_allied.startFrame == 0) s_allied.startFrame = frame;
	if (s_runner.lastObservedFrame != frame)
	{
		s_runner.lastObservedFrame = frame;
		s_runner.stalledStartMilliseconds = GetTickCount();
	}
	else if (IsSkirmishAITestProgressStalled(
		ElapsedMilliseconds(s_runner.stalledStartMilliseconds, GetTickCount())))
	{
		FailSkirmishAITest("allied_fixture_frame_stalled");
		RequestSkirmishAITestStop();
		return;
	}
#if RTS_ZEROHOUR && defined(_WIN64)
	// Check the fixed deadlines before a case can report success.
	// Coordination observes cancellation/resumption and then a second natural
	// cohort after the first cohort's unchanged cooldown, plus travel and a
	// later central evaluation. Other cases retain their single-phase budget.
	const UnsignedInt maxFrames = s_allied.fixtureCase == SKIRMISH_AI_ALLIED_COORDINATION_LIVE ? 30000 : 18000;
	if (frame - s_allied.startFrame > maxFrames ||
		ElapsedMilliseconds(s_runner.startupStartMilliseconds, GetTickCount()) > 600000)
	{
		PrintAlliedFixtureTimeoutDiagnostics(frame);
		FailSkirmishAITest("allied_fixture_assertions_timeout");
		RequestSkirmishAITestStop();
		return;
	}
#endif
#if RTS_ZEROHOUR && defined(_WIN64)
	if (s_allied.fixtureCase == SKIRMISH_AI_ALLIED_TRANSFER_COMMAND)
	{
		if (!RunAlliedTransferCommands()) FailSkirmishAITest("allied_transfer_assertion");
		s_runner.endFrame = frame;
		RequestSkirmishAITestStop();
		return;
	}
	if (s_allied.fixtureCase == SKIRMISH_AI_ALLIED_AID_LIFECYCLE ||
		s_allied.fixtureCase == SKIRMISH_AI_ALLIED_SAVE_LOAD) ObserveAlliedAid(frame);
	if (s_runner.ending) return;
	ObserveAlliedCoordination(frame);
	if (s_runner.ending) return;
	if (s_allied.fixtureCase == SKIRMISH_AI_ALLIED_SAVE_LOAD)
		ObserveAlliedCancellationResume(frame);
	if (s_runner.ending) return;
	if (s_allied.fixtureCase == SKIRMISH_AI_ALLIED_COORDINATION_LIVE)
	{
		if (!s_allied.withdrawalComplete) ObserveAlliedLeaderWithdrawal(frame);
		else ObserveAlliedAssaultLaunch(frame);
	}
	if (s_runner.ending) return;
	if (s_allied.fixtureCase == SKIRMISH_AI_ALLIED_SUPPORT_LIFECYCLE)
		ObserveAlliedSupport(frame);
	if (s_runner.ending) return;
#endif
}
}

Int FinalizeSkirmishAITestRunner(Int engineExitCode)
{
	if (!s_runner.armed)
		return engineExitCode;
	if (engineExitCode != 0 && !s_runner.failed)
		FailSkirmishAITest("engine_exit");
	if (!s_runner.finished && !s_runner.failed)
		FailSkirmishAITest("incomplete");

	if (s_allied.active)
	{
		if (strcmp(s_executableHashObserved, "unavailable") == 0)
			FailSkirmishAITest("allied_executable_hash_unavailable");
		else if (strcmp(s_executableHashInput, "unavailable") != 0 &&
			_stricmp(s_executableHashInput, s_executableHashObserved) != 0)
			FailSkirmishAITest("allied_executable_hash_mismatch");
		printf("%s seed=%d case=%s checks=%u end_frame=%u reason=%s "
			"map_crc=%08X map_size=%u executable_sha256=%s "
			"fresh_match_gate=not_run replay_gate=not_run\n",
			s_runner.failed ? "SKIRMISH_AI_ALLIED_FIXTURE_FAIL" :
				"SKIRMISH_AI_ALLIED_FIXTURE_COMPLETE",
			s_runner.seed, s_alliedCaseNames[s_allied.fixtureCase], s_allied.checks,
			s_runner.endFrame, s_runner.failureReason ? s_runner.failureReason : "none",
			s_runner.loadedMapCRC, s_runner.loadedMapSize, s_executableHashObserved);
		fflush(stdout);
		return s_runner.failed ? 1 : 0;
	}
	if (s_recovery.active)
	{
		if (s_runner.failed)
		{
			printf("%s seed=%d case=%s faction=%s reason=%s\n",
				s_recovery.fixtureCase == SKIRMISH_AI_RECOVERY_GLA_HOLE
					? "SKIRMISH_AI_RECOVERY_HOLE_FIXTURE_FAIL"
					: "SKIRMISH_AI_RECOVERY_FIXTURE_FAIL",
				s_runner.seed, GetSkirmishAIRecoveryFixtureCaseName(s_recovery.fixtureCase),
				GetSkirmishAIRecoveryFactionName(s_recovery.faction),
				s_runner.failureReason ? s_runner.failureReason : "unknown");
			fflush(stdout);
			return 1;
		}

		const char *fixtureResult =
			s_recovery.fixtureCase == SKIRMISH_AI_RECOVERY_REPEATED_COMMAND_CENTER &&
			s_recovery.secondBuilderLossSkipped
				? "SKIRMISH_AI_RECOVERY_REPEATED_CC_ONLY_COMPLETE"
				: (s_recovery.fixtureCase == SKIRMISH_AI_RECOVERY_GLA_HOLE
					? "SKIRMISH_AI_RECOVERY_HOLE_FIXTURE_COMPLETE"
					: "SKIRMISH_AI_RECOVERY_FIXTURE_COMPLETE");
		const char *builderLossResult =
			s_recovery.fixtureCase != SKIRMISH_AI_RECOVERY_REPEATED_COMMAND_CENTER
				? "not_run"
				: (s_recovery.secondBuilderLossIssued ? "verified" : "skipped");
		const char *factoryBlockResult =
			s_recovery.fixtureCase == SKIRMISH_AI_RECOVERY_DISABLED_FACTORY &&
			s_recovery.factoryBlockVerified ? "verified" : "not_run";
		const char *factoryBuilderReuseResult =
			s_recovery.fixtureCase == SKIRMISH_AI_RECOVERY_DISABLED_FACTORY &&
			s_recovery.factoryWorkerObserved &&
			s_recovery.lastConstructionBuilderID ==
				s_recovery.disabledFactoryBuilderID ? "verified" : "not_run";
		const char *postGraceReserveReleaseResult =
			s_recovery.fixtureCase == SKIRMISH_AI_RECOVERY_DISABLED_FACTORY &&
			s_recovery.factoryReserveHeldObserved &&
			s_recovery.factoryReserveReleasedObserved ? "verified" : "not_run";
		printf("%s seed=%d case=%s faction=%s "
			"template=%s map=\"%s\" map_crc=%08X map_size=%u loaded_seed=%d "
			"destructions=%d recoveries=%d construction_scaffolds=%d duplicate_cc=0 "
			"campaign_isolation=skirmish_only save_load=%s builder_loss=%s "
			"factory_block=%s post_grace_reserve_release=%s "
			"factory_builder_reuse=%s old_save_defaults=not_run\n",
			fixtureResult,
			s_runner.seed, GetSkirmishAIRecoveryFixtureCaseName(s_recovery.fixtureCase),
			GetSkirmishAIRecoveryFactionName(s_recovery.faction),
			GetSkirmishAIRecoveryFactionTemplateName(s_recovery.faction),
			s_runner.loadedMapName, s_runner.loadedMapCRC, s_runner.loadedMapSize,
			s_runner.loadedSeed, s_recovery.destructionCount,
			s_recovery.recoveryCompletionCount, s_recovery.constructionScaffoldCount,
			s_recovery.fixtureCase == SKIRMISH_AI_RECOVERY_SAVE_LOAD ? "run" : "not_run",
			builderLossResult, factoryBlockResult,
			postGraceReserveReleaseResult, factoryBuilderReuseResult);
		fflush(stdout);
		return 0;
	}

	AsciiString replayName = s_runner.replayFileName;
	AsciiString replayPath = RecorderClass::getReplayDir();
	replayPath.concat(replayName);
	if (!s_runner.failed)
	{
		// The game should have closed the recorder while leaving the game.  Close
		// it here as a defensive boundary before reading or retaining LastReplay;
		// the next runner invocation must never race this copy.
		if (TheRecorder && TheRecorder->getMode() == RECORDERMODETYPE_RECORD)
			TheRecorder->stopRecording();
		RecorderClass::ReplayHeader header;
		header.filename = replayName;
		header.forPlayback = FALSE;
		if (!TheRecorder || !TheRecorder->readReplayHeader(header) ||
			!RecorderClass::replayMatchesGameVersion(header) ||
			!IsValidSkirmishAITestReplayResult(s_runner.endFrame, header.frameCount,
				header.desyncGame, header.quitEarly, header.startTime, header.endTime))
		{
			FailSkirmishAITest("replay_validation");
		}
		else
		{
			s_runner.replayEpoch = GetSkirmishAIReplayEpoch(header.versionTimeString);
			if (s_runner.replayEpoch != GetSkirmishAIReplayRecordingEpoch())
				FailSkirmishAITest("replay_epoch_mismatch");
		}
	}
	if (!s_runner.failed)
	{
		if (strcmp(s_executableHashObserved, "unavailable") == 0)
			FailSkirmishAITest("executable_hash_unavailable");
		else if (strcmp(s_executableHashInput, "unavailable") != 0 &&
			_stricmp(s_executableHashInput, s_executableHashObserved) != 0)
			FailSkirmishAITest("executable_hash_mismatch");
	}
	if (!s_runner.failed)
	{
		char retainedReplayPath[SKIRMISH_AI_TEST_RECEIPT_PATH_LENGTH];
		const AsciiString replayDirectoryValue = RecorderClass::getReplayDir();
		const char *replayDirectory = replayDirectoryValue.str();
		const char *separator = "";
		const size_t directoryLength = strlen(replayDirectory);
		if (directoryLength != 0 &&
			replayDirectory[directoryLength - 1] != '\\' &&
			replayDirectory[directoryLength - 1] != '/')
			separator = "\\";
		const int retainedLength = _snprintf(retainedReplayPath,
			ARRAY_SIZE(retainedReplayPath), "%s%sSkirmishAI-%s-%d-%s.rep",
			replayDirectory, separator,
			SkirmishAITestScenarioName(s_runner.scenario), s_runner.seed,
			s_runner.runNonce);
		retainedReplayPath[ARRAY_SIZE(retainedReplayPath) - 1] = '\0';
		if (retainedLength < 0 ||
			!RetainSkirmishAITestReplayAtomically(replayPath.str(),
				retainedReplayPath, s_runner.replaySha256))
		{
			FailSkirmishAITest("replay_retention");
		}
		else
		{
			strlcpy(s_runner.retainedReplayPath, retainedReplayPath,
				ARRAY_SIZE(s_runner.retainedReplayPath));
		}
	}
	if (!s_runner.failed)
	{
		SkirmishAITestReplayReceipt receipt;
		memset(&receipt, 0, sizeof(receipt));
		receipt.seed = s_runner.seed;
		receipt.winnerTeam = s_runner.winnerTeam;
		receipt.endFrame = s_runner.endFrame;
		receipt.replayEpoch = s_runner.replayEpoch;
		strlcpy(receipt.scenario, SkirmishAITestScenarioName(s_runner.scenario),
			ARRAY_SIZE(receipt.scenario));
		strlcpy(receipt.executableSha256, s_executableHashObserved,
			ARRAY_SIZE(receipt.executableSha256));
		strlcpy(receipt.replaySha256, s_runner.replaySha256,
			ARRAY_SIZE(receipt.replaySha256));
		strlcpy(receipt.runNonce, s_runner.runNonce,
			ARRAY_SIZE(receipt.runNonce));
		strlcpy(receipt.replayPath, s_runner.retainedReplayPath,
			ARRAY_SIZE(receipt.replayPath));
		if (!IsValidSkirmishAITestReplayReceipt(receipt,
			GetSkirmishAIReplayRecordingEpoch()))
			FailSkirmishAITest("replay_receipt_invalid");
	}

#if defined(_WIN64)
	// Copy all owner-dependent state before GameEngine destroys the recorder,
	// player list, game data and JobSystem owner registration.
	if (s_performanceReceipt)
	{
		s_performanceReceipt->captureSchedulerBeforeTeardown();
		if (s_runner.failed)
			s_performanceReceipt->invalidate("fresh AI owner run or closed replay validation failed");
		else
			s_performanceReceipt->retainClosedReplay(s_runner.retainedReplayPath, s_runner.replaySha256);
	}
#endif
	if (s_runner.failed)
	{
		printf("SKIRMISH_AI_TEST_FAIL seed=%d scenario=%s run_nonce=%s reason=%s\n",
			s_runner.seed, SkirmishAITestScenarioName(s_runner.scenario),
			s_runner.runNonce,
			s_runner.failureReason ? s_runner.failureReason : "unknown");
		fflush(stdout);
		return 1;
	}

	if (IsSkirmishAITest4v2(s_runner.scenario))
	{
		printf("SKIRMISH_AI_TEST_COMPLETE seed=%d scenario=%s map=\"%s\" map_crc=%08X map_size=%u loaded_seed=%d "
			"actual_ai=%d actual_teams=%dv%d winner_team=%d end_frame=%u replay=%s",
			s_runner.seed, SkirmishAITestScenarioName(s_runner.scenario),
#if defined(_WIN64)
			s_reviewedMapRequest.requested ? s_reviewedMapIdentity.logicalKey :
#endif
			s_runner.loadedMapName,
			s_runner.loadedMapCRC, s_runner.loadedMapSize, s_runner.loadedSeed,
			s_runner.actualAiCount, s_runner.actualTeamCounts[0], s_runner.actualTeamCounts[1],
			s_runner.winnerTeam, s_runner.endFrame, replayPath.str());
	}
	else
	{
		printf("SKIRMISH_AI_TEST_COMPLETE seed=%d scenario=%s map=\"%s\" map_crc=%08X map_size=%u loaded_seed=%d "
			"actual_ai=%d actual_teams=%dv%d winner_team=%d end_frame=%u replay=%s",
			s_runner.seed, SkirmishAITestScenarioName(s_runner.scenario), s_runner.loadedMapName,
			s_runner.loadedMapCRC, s_runner.loadedMapSize, s_runner.loadedSeed,
			s_runner.actualAiCount, s_runner.actualTeamCounts[0], s_runner.actualTeamCounts[1],
			s_runner.winnerTeam, s_runner.endFrame, replayPath.str());
	}
#if defined(_WIN64)
	if (s_reviewedMapRequest.requested) printf(" map_sha256=%s", s_reviewedMapSha256);
#endif
	PrintSkirmishAITestManifest();
	fflush(stdout);
	return 0;
}
