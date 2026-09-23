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
#if RTS_ZEROHOUR
#include "GameLogic/AI.h"
#include "GameLogic/AIPathfind.h"
#endif
#include "GameClient/MapUtil.h"
#include "GameLogic/GameLogic.h"
#include "GameLogic/Object.h"
#include "GameLogic/TerrainLogic.h"
#include "GameLogic/Module/AIUpdate.h"
#include "GameLogic/Module/DozerAIUpdate.h"
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
#if defined(_WIN32)
#include <io.h>
#endif

namespace
{
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
	"disabled_factory"
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
	if (!s_recovery.saveLoadIssued || !loadedState || plan.mapName == nullptr ||
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
		IsSkirmishAITestPracticalControllerScenario(s_runner.scenario) ? FALSE : TRUE;
	TheWritableGlobalData->m_shellMapOn = FALSE;
	if (!IsSkirmishAITestPracticalControllerScenario(s_runner.scenario))
		TheWritableGlobalData->m_useFpsLimit = FALSE;
	// Automated lanes keep logical and local retaliation modes disabled so the
	// recorder does not capture an irrelevant frame-zero preference
	// synchronization command.
	if (!IsSkirmishAITestPracticalControllerScenario(s_runner.scenario))
		TheWritableGlobalData->m_clientRetaliationModeEnabled = FALSE;
	TheRecorder->setArchiveEnabled(FALSE);
	InitRandom(static_cast<UnsignedInt>(plan.seed));

#if defined(_WIN64)
	if (!s_recovery.active &&
		!IsSkirmishAITestPracticalControllerScenario(s_runner.scenario) &&
		!s_performanceReceiptAttempted)
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
	if (!(s_recovery.active ?
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
		if (s_recovery.fixtureCase == SKIRMISH_AI_RECOVERY_SURVIVING_BUILDER)
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
				: SKIRMISH_AI_RECOVERY_BASELINE_TIMEOUT_FRAMES;
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


Int FinalizeSkirmishAITestRunner(Int engineExitCode)
{
	if (!s_runner.armed)
		return engineExitCode;
	if (engineExitCode != 0 && !s_runner.failed)
		FailSkirmishAITest("engine_exit");
	if (!s_runner.finished && !s_runner.failed)
		FailSkirmishAITest("incomplete");

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
			if (s_runner.replayEpoch != (BuildSupportsSkirmishAICounterRngPlanning() ?
				SKIRMISH_AI_REPLAY_EPOCH_COUNTER_RNG :
				SKIRMISH_AI_REPLAY_EPOCH_ADAPTIVE_GLOBAL_RNG))
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
			(BuildSupportsSkirmishAICounterRngPlanning() ?
			SKIRMISH_AI_REPLAY_EPOCH_COUNTER_RNG :
			SKIRMISH_AI_REPLAY_EPOCH_ADAPTIVE_GLOBAL_RNG)))
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
