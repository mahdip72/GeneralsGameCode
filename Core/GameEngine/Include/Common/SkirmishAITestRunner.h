/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2026 TheSuperHackers
**
** This program is free software: you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation, either version 3 of the License, or
** (at your option) any later version.
*/

#pragma once

struct DirectPathRuntimeMetrics;
struct OrdinaryPathRuntimeMetrics;
#if defined(_WIN64)
namespace rts
{
struct CollisionCandidateRuntimeMetrics;
struct ImmutableSpatialRuntimeMetrics;
struct ObjectStatusTimerRuntimeMetrics;
struct PhysicsIntegrationRuntimeMetrics;
}
#endif

#include "GameNetwork/GameInfo.h"
#include "Common/SkirmishAITestReceipt.h"
#include "Common/SkirmishAITestMapBinding.h"

enum
{
	SKIRMISH_AI_TEST_SLOT_COUNT = 8,
	// Legitimate long AI games can exceed 108000 frames; independent wall and
	// stall guards remain the liveness limits.
	SKIRMISH_AI_TEST_MAX_FRAME = 216000,
	// Immutable replay evidence is intentionally bounded independently of the
	// filesystem so hashing cannot consume an attacker-controlled extent.
	SKIRMISH_AI_TEST_MAX_REPLAY_BYTES = 256 * 1024 * 1024,
	SKIRMISH_AI_TEST_MAX_STARTUP_MILLISECONDS = 300000,
	SKIRMISH_AI_TEST_MAX_STALLED_MILLISECONDS = 30000,
	SKIRMISH_AI_TEST_MAX_SHUTDOWN_MILLISECONDS = 30000
};

enum SkirmishAITestScenario
{
	SKIRMISH_AI_TEST_SCENARIO_4V3,
	SKIRMISH_AI_TEST_SCENARIO_4V2,
	// This is a practical/manual lane.  It is intentionally distinct from
	// the observer-backed automated lanes and is not a replay gate scenario.
	SKIRMISH_AI_TEST_SCENARIO_PRACTICAL_1V7,
	SKIRMISH_AI_TEST_SCENARIO_ONE_CONTROLLER_7_AI =
		SKIRMISH_AI_TEST_SCENARIO_PRACTICAL_1V7,
	// Eight occupied hard-AI slots: two allied against six. The local player
	// is the engine-created replay observer, not a GameInfo slot.
	SKIRMISH_AI_TEST_SCENARIO_HARD_AI_2V6,
	// Explicit test-only rendered combat diagnostic; not AI/replay acceptance.
	SKIRMISH_AI_TEST_SCENARIO_RENDERED_BATTLE_DIAGNOSTIC,
	// Bounded development benchmark, with uncapped rendering and normal logic time.
	SKIRMISH_AI_TEST_SCENARIO_RENDERED_BATTLE_BENCHMARK
};

// CLI: -runSkirmishAIRecoveryTest <positive-seed> <case> <FactionTemplate>.
// Opt-in, full-engine recovery and infrastructure fixtures. These are
// separate from the normal replay scenarios: they stop on fixture assertions
// and never report a fixture result as a gameplay/replay gate.
// Cases: surviving_builder, factory_only, no_path_laststand, repeated_cc,
// obstructed, low_cash, gla_hole, save_load, disabled_factory,
// infrastructure_collapse. Factions are the
// 12 playable Zero Hour Faction*
// templates returned by GetSkirmishAIRecoveryFactionTemplateName().
enum SkirmishAIRecoveryFixtureCase
{
	SKIRMISH_AI_RECOVERY_SURVIVING_BUILDER,
	SKIRMISH_AI_RECOVERY_FACTORY_ONLY,
	SKIRMISH_AI_RECOVERY_NO_PATH_LASTSTAND,
	SKIRMISH_AI_RECOVERY_REPEATED_COMMAND_CENTER,
	SKIRMISH_AI_RECOVERY_OBSTRUCTED,
	SKIRMISH_AI_RECOVERY_LOW_CASH,
	SKIRMISH_AI_RECOVERY_GLA_HOLE,
	SKIRMISH_AI_RECOVERY_SAVE_LOAD,
	SKIRMISH_AI_RECOVERY_DISABLED_FACTORY,
	SKIRMISH_AI_RECOVERY_INFRASTRUCTURE_COLLAPSE,
	SKIRMISH_AI_RECOVERY_FIXTURE_CASE_COUNT
};

enum SkirmishAIRecoveryFaction
{
	SKIRMISH_AI_RECOVERY_FACTION_AMERICA,
	SKIRMISH_AI_RECOVERY_FACTION_AMERICA_SUPER_WEAPON_GENERAL,
	SKIRMISH_AI_RECOVERY_FACTION_AMERICA_LASER_GENERAL,
	SKIRMISH_AI_RECOVERY_FACTION_AMERICA_AIR_FORCE_GENERAL,
	SKIRMISH_AI_RECOVERY_FACTION_CHINA,
	SKIRMISH_AI_RECOVERY_FACTION_CHINA_TANK_GENERAL,
	SKIRMISH_AI_RECOVERY_FACTION_CHINA_INFANTRY_GENERAL,
	SKIRMISH_AI_RECOVERY_FACTION_CHINA_NUKE_GENERAL,
	SKIRMISH_AI_RECOVERY_FACTION_GLA,
	SKIRMISH_AI_RECOVERY_FACTION_GLA_TOXIN_GENERAL,
	SKIRMISH_AI_RECOVERY_FACTION_GLA_DEMOLITION_GENERAL,
	SKIRMISH_AI_RECOVERY_FACTION_GLA_STEALTH_GENERAL,
	SKIRMISH_AI_RECOVERY_FACTION_COUNT
};

enum SkirmishAITestProgress
{
	SKIRMISH_AI_TEST_RUNNING,
	SKIRMISH_AI_TEST_COMPLETE,
	SKIRMISH_AI_TEST_TIMED_OUT
};

enum
{
	RENDERED_BATTLE_DIAGNOSTIC_UNITS_PER_PLAYER = 32,
	RENDERED_BATTLE_DIAGNOSTIC_MAX_FRAMES = 1800,
	RENDERED_BATTLE_DIAGNOSTIC_MAX_MILLISECONDS = 120000,
	RENDERED_BATTLE_DIAGNOSTIC_PLACEMENT_SCHEMA_VERSION = 2,
	RENDERED_BATTLE_DIAGNOSTIC_LOCAL_TRIAL_COUNT = 9,
	RENDERED_BATTLE_DIAGNOSTIC_LOCAL_STEP = 22,
	RENDERED_BATTLE_DIAGNOSTIC_LOCAL_ARENA_CAP = 8
};
enum
{
	RENDERED_BATTLE_BENCHMARK_UNITS_PER_PLAYER = 64,
	RENDERED_BATTLE_BENCHMARK_WARMUP_FRAMES = 150,
	RENDERED_BATTLE_BENCHMARK_MEASURE_FRAMES = 450,
	RENDERED_BATTLE_BENCHMARK_MAX_MILLISECONDS = 45000,
	RENDERED_BATTLE_BENCHMARK_GRID_STEP = 52,
	RENDERED_BATTLE_BENCHMARK_BAND_STEP = 208,
	RENDERED_BATTLE_BENCHMARK_FORMATION_X = 459,
	RENDERED_BATTLE_BENCHMARK_FORMATION_Y = 416,
	RENDERED_BATTLE_BENCHMARK_INSET_X = 503,
	RENDERED_BATTLE_BENCHMARK_INSET_Y = 460,
	RENDERED_BATTLE_BENCHMARK_ARENA_SEARCH_OPERATIONS = 4000000,
	RENDERED_BATTLE_BENCHMARK_TOTAL_SEARCH_OPERATIONS = 16000000,
	RENDERED_BATTLE_BENCHMARK_NAMED_LOCAL_ARENA_CAP = 130,
	RENDERED_BATTLE_BENCHMARK_REFINEMENT_OFFSETS = 80,
	RENDERED_BATTLE_BENCHMARK_REFINEMENT_CANDIDATE_CAP = 10400,
	RENDERED_BATTLE_BENCHMARK_REFINEMENT_UNARY_CAP = 4000000
};
struct RenderedBattleBenchmarkGeometry
{
	Int gridStep, extraOffset, bandStep, formationX, formationY, insetX, insetY, revealRadius;
};
RenderedBattleBenchmarkGeometry GetRenderedBattleBenchmarkGeometry();

// Pure admission/roster helpers used by the production diagnostic and fixture.
// Ordinary command lines are accepted unchanged when the new flag is absent.
Bool ValidateRenderedBattleDiagnosticArguments(Int argc, const char *const *argv,
	Bool supported, const char **reason);
const char *GetRenderedBattleDiagnosticFactionName(Int slot);
const char *GetRenderedBattleDiagnosticObjectName(Int slot, Int unit, Bool benchmark = FALSE);
Bool GetRenderedBattleDiagnosticOffset(Int slot, Int unit, Coord3D *offset, Bool benchmark = FALSE);
// Planning order is independent of canonical slot/roster storage: in dense
// mode all original 256 units precede every added infantry position.
// Invalid ranks/output pointers preserve supplied outputs.
Bool GetRenderedBattleDiagnosticPlacementUnit(Int rank, Bool benchmark, Int *slot, Int *unit);
// Mirrors the existing engine fast-mode admission, including debug-cheat builds.
Bool IsRenderedBattleBenchmarkFastModeActive(Bool fastMode, Bool replayGame, Bool debugCheatsAllowed);
// Qualify all 512 nominal pairs once before any live arena search. Conservative
// class maxima must satisfy the same uninflated-radius +3 clearance gate.
Bool ValidateRenderedBattleBenchmarkNominalGeometry(Real vehicleRadius, Real infantryRadius);
enum RenderedBattleBenchmarkPlacementResult
{
	RB_BENCHMARK_PLACEMENT_SOLVED, RB_BENCHMARK_PLACEMENT_EMPTY_DOMAIN,
	RB_BENCHMARK_PLACEMENT_UNSATISFIABLE, RB_BENCHMARK_PLACEMENT_BUDGET_EXHAUSTED,
	RB_BENCHMARK_PLACEMENT_INVALID
};
struct RenderedBattleBenchmarkPlacementStats
{
	Int operations, pairComparisons, assignments, backtracks, maxAssigned, emptyDomains;
};
// Pure dense-only planner: canonical unit-major nine-position domains already
// passed every live unary gate. MRV ties and position trials use ascending
// canonical indices. Forward checking/backtracking never queries the world.
// At most 512 units. Budget counts each MRV/copy scan, assignment and pair gate;
// fixed nine-bit scans and bounded input validation are separate. On failure
// choices remain untouched; stats describe the bounded attempt, not acceptance.
RenderedBattleBenchmarkPlacementResult SolveRenderedBattleBenchmarkPlacement(
	Int unitCount, const Coord3D *domains, const Real *radii, const UnsignedInt *domainMasks,
	Int operationBudget, Int *choices, RenderedBattleBenchmarkPlacementStats *stats);
// Version 2: original 49 centers first, then a row-major 9x9 grid inset by
// 403/366 formation + 22 local movement + 22 footprint = 447/410.
// Dense-only 52-grid/208-player bands have envelope459/416, insets503/460.
// Invalid extents have no candidates; too-small extents
// retain only the original search. Failure leaves the supplied center intact.
Int GetRenderedBattleDiagnosticSearchCount(Real loX, Real loY, Real hiX, Real hiY, Bool benchmark = FALSE);
Bool GetRenderedBattleDiagnosticSearchCenter(Real loX, Real loY, Real hiX, Real hiY,
	Int candidate, Coord3D *center, Bool benchmark = FALSE);
// Explicit mechanized512 only. Row-major 10-unit shifts in [-40,40], omitting
// zero; reject outside the complete inset envelope, never clamp. No world queries.
Bool IsRenderedBattleBenchmarkRefinementEligible(Bool benchmark);
Bool CanRenderedBattleBenchmarkRefinementQuery(Int used, Int queries);
Bool GetRenderedBattleBenchmarkRefinementCenter(Real loX, Real loY, Real hiX, Real hiY,
	Int parentCandidate, Int offsetIndex, Coord3D *center);
// Fixed local stencil, mirrored in X by battle side; no RNG or world queries.
Bool GetRenderedBattleDiagnosticLocalOffset(Int slot, Int trial, Coord3D *offset);
// Geometry radii are the uninflated template radii. Same total clearance as
// the world occupancy gate: current+1, previous, then 2 additional units.
Bool AreRenderedBattleDiagnosticPositionsSeparated(const Coord3D &a, Real radiusA,
	const Coord3D &b, Real radiusB);
// Retain arenas by descending valid prefix, ascending index.
// Legacy/default capacity remains eight; named profiles retain all 130 candidates.
// Arrays have arenaCapacity entries; rejected inserts preserve them/count.
Bool RememberRenderedBattleDiagnosticArena(Int candidate, Int validUnits,
	Int *candidates, Int *validPrefixes, Int *count, Bool benchmark = FALSE,
	Int arenaCapacity = RENDERED_BATTLE_DIAGNOSTIC_LOCAL_ARENA_CAP);
// Bounded append: failure preserves the prior bytes and length.
Bool AppendRenderedBattleDiagnosticReportRecord(char *buffer, UnsignedInt capacity,
	UnsignedInt *used, const char *record, UnsignedInt recordBytes);

struct SkirmishAITestSlotPlan
{
	SlotState state;
	Int playerTemplate;
	Int color;
	Int startPosition;
	Int teamNumber;
	Bool isController;
};

struct SkirmishAITestPlan
{
	Int seed;
	const char *mapName;
	SkirmishAITestSlotPlan slots[SKIRMISH_AI_TEST_SLOT_COUNT];
};

struct SkirmishAITestLoadedState
{
	const char *gameInfoMapName;
	const char *globalMapName;
	const char *terrainMapName;
	UnsignedInt mapCRC;
	UnsignedInt mapSize;
	Int seed;
};

Bool TryParseSkirmishAITestSeed(const char *text, Int *seed);
#if defined(_WIN64)
Bool ConfigureSkirmishAITestReviewedMap(const rts::ai_fixture::MapRequest &request);
#endif
Bool TryParseSkirmishAIRecoveryFixtureCase(const char *text, Int *fixtureCase);
Bool TryParseSkirmishAIRecoveryFaction(const char *text, Int *faction);
Bool IsSupportedSkirmishAIRecoveryFixtureCombination(Int fixtureCase, Int faction);
const char *GetSkirmishAIRecoveryFixtureCaseName(Int fixtureCase);
const char *GetSkirmishAIRecoveryFactionName(Int faction);
const char *GetSkirmishAIRecoveryFactionTemplateName(Int faction);
Bool ShouldBypassFramePacingForSkirmishAITest(Bool runnerArmed);
void BuildSkirmishAITestPlan(Int seed, SkirmishAITestPlan *plan);
void BuildSkirmishAITestPlan(Int seed, SkirmishAITestScenario scenario,
	SkirmishAITestPlan *plan);
Bool IsExpectedSkirmishAITestLoadedState(const SkirmishAITestPlan &plan,
	UnsignedInt expectedMapCRC, UnsignedInt expectedMapSize,
	const SkirmishAITestLoadedState *loadedState);
Bool IsValidSkirmishAITestReplayResult(UnsignedInt expectedFrameCount,
	UnsignedInt actualFrameCount, Bool desyncGame, Bool quitEarly,
	time_t startTime, time_t endTime);
SkirmishAITestProgress EvaluateSkirmishAITestProgress(UnsignedInt endFrame, UnsignedInt currentFrame);
Bool IsSkirmishAITestStartupTimedOut(UnsignedInt elapsedMilliseconds);
Bool IsSkirmishAITestProgressStalled(UnsignedInt elapsedMilliseconds);

// Pure lifecycle accumulator used by the installed runner and focused tests.
// It freezes nonzero path authority before a later game-data reset epoch.
void AccumulateSkirmishAITestDirectPathMetrics(
	DirectPathRuntimeMetrics *baseline,
	const DirectPathRuntimeMetrics &current,
	DirectPathRuntimeMetrics *frozen,
	Bool *hasFrozenActivity,
	Bool *awaitingInitialReset);
void AccumulateSkirmishAITestOrdinaryPathMetrics(
	OrdinaryPathRuntimeMetrics *baseline,
	const OrdinaryPathRuntimeMetrics &current,
	OrdinaryPathRuntimeMetrics *frozen,
	Bool *awaitingInitialReset);
#if defined(_WIN64)
// Pure reset-epoch accumulators shared by installed lifecycle code and paired
// title fixtures. The first observed reset rebases shell state; a later reset
// is teardown and cannot erase the frozen match/replay evidence.
void AccumulateSkirmishAITestCollisionMetrics(
	rts::CollisionCandidateRuntimeMetrics *baseline,
	const rts::CollisionCandidateRuntimeMetrics &current,
	rts::CollisionCandidateRuntimeMetrics *frozen,
	Bool *awaitingInitialReset);
void AccumulateSkirmishAITestPhysicsMetrics(
	rts::PhysicsIntegrationRuntimeMetrics *baseline,
	const rts::PhysicsIntegrationRuntimeMetrics &current,
	rts::PhysicsIntegrationRuntimeMetrics *frozen,
	Bool *awaitingInitialReset);
void AccumulateSkirmishAITestObjectStatusTimerMetrics(
	rts::ObjectStatusTimerRuntimeMetrics *baseline,
	const rts::ObjectStatusTimerRuntimeMetrics &current,
	rts::ObjectStatusTimerRuntimeMetrics *frozen,
	Bool *awaitingInitialReset);
void AccumulateSkirmishAITestImmutableSpatialMetrics(
	rts::ImmutableSpatialRuntimeMetrics *baseline,
	const rts::ImmutableSpatialRuntimeMetrics &current,
	rts::ImmutableSpatialRuntimeMetrics *frozen,
	Bool *awaitingInitialReset);
#endif
Bool IsSkirmishAITestShutdownTimedOut(UnsignedInt elapsedMilliseconds);

Bool IsSkirmishAITestPracticalControllerScenario(SkirmishAITestScenario scenario);
Bool IsValidSkirmishAITestPracticalControllerPlan(
	const SkirmishAITestPlan &plan);

// Narrow per-invocation seams for retention commit/final-close policy tests.
// Production callers use the ordinary three-argument wrapper below.
namespace SkirmishAITestDetail
{
typedef Bool (*ReplayCommitCallback)(
	const char *temporaryPath, const char *destinationPath, void *context);
typedef Bool (*ReplayFinalHandleCloseCallback)(void *nativeHandle, void *context);
Bool RetainSkirmishAITestReplayAtomically(
	const char *sourcePath, const char *destinationPath,
	char sha256[SKIRMISH_AI_TEST_RECEIPT_SHA256_LENGTH + 1],
	ReplayCommitCallback commitCallback, void *context,
	ReplayFinalHandleCloseCallback finalCloseCallback = 0,
	void *finalCloseContext = 0);
}

Bool SetSkirmishAITestExecutableHashInput(const char *sha256);
// Shared validation hashing; these do not arm or alter any AI-test scenario.
Bool HashSkirmishAITestBytes(const void *bytes, size_t byteCount,
	char sha256[SKIRMISH_AI_TEST_RECEIPT_SHA256_LENGTH + 1]);
Bool HashSkirmishAITestContentFile(const char *path,
	char sha256[SKIRMISH_AI_TEST_RECEIPT_SHA256_LENGTH + 1]);
// Hash an already-open native file handle without reopening its pathname.
// The handle remains owned by the caller and its file position is restored.
// The opaque handle type keeps this shared declaration portable to non-Windows
// title fixtures; native callers pass the Win32 HANDLE value as void*.
Bool HashSkirmishAITestContentHandle(void *handle,
	char sha256[SKIRMISH_AI_TEST_RECEIPT_SHA256_LENGTH + 1]);
Bool CaptureSkirmishAITestValidatedExecutableHash(
	char sha256[SKIRMISH_AI_TEST_RECEIPT_SHA256_LENGTH + 1]);
Bool SetSkirmishAITestSimulationModeInput(const char *mode);
void SetSkirmishAITestFinalDigest(UnsignedInt digest);

void ArmSkirmishAITestRunner(Int seed,
	SkirmishAITestScenario scenario = SKIRMISH_AI_TEST_SCENARIO_4V3);
void ArmSkirmishAIRecoveryFixtureRunner(Int seed, Int fixtureCase, Int faction);
// Opt-in native x64 Zero Hour full-engine allied fixtures; never a
// fresh-match/replay acceptance lane.
enum SkirmishAIAlliedFixtureCase
{
	SKIRMISH_AI_ALLIED_TRANSFER_COMMAND,
	SKIRMISH_AI_ALLIED_COORDINATION_LIVE,
	SKIRMISH_AI_ALLIED_SAVE_LOAD,
	SKIRMISH_AI_ALLIED_SUPPORT_LIFECYCLE,
	SKIRMISH_AI_ALLIED_AID_LIFECYCLE,
	SKIRMISH_AI_ALLIED_FIXTURE_CASE_COUNT
};
Bool TryParseSkirmishAIAlliedFixtureCase(const char *text, Int *fixtureCase);
Bool ConfigureSkirmishAIAlliedFixture(Int fixtureCase);
// Read-only opt-in for live allied-fixture diagnostics, false in other lanes.
Bool IsSkirmishAIAlliedFixtureActive();
// Opt-in bounded witness for direct roster transfers held by the actual engine.
void ObserveSkirmishAIAlliedTeamTransferHeld(UnsignedInt sourceTeamID,
	UnsignedInt destinationTeamID, const char *operation);
Bool IsSkirmishAITestRunnerArmed();
Bool StartSkirmishAITestRunner();
void UpdateSkirmishAITestRunner();
Int FinalizeSkirmishAITestRunner(Int engineExitCode);
#if defined(_WIN64)
void ObserveSkirmishAITestCompletedFrame(unsigned previousFrame);
// Release the exact existing borrow while GameLogic is still alive, even if
// runner reporting has been disarmed. Runtime storage remains through drain.
void ReleaseSkirmishAITestPerformanceReceiptOwner();
// Call only after the real engine has drained and been destroyed. This uses
// retained diagnostic snapshots, never reset/destroyed game globals.
void FinalizeSkirmishAITestPerformanceReceipt(Int engineExitCode);
#endif
