/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2026 TheSuperHackers
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
*/

#pragma once

#include "Common/GameCommon.h"
#include "Common/GameType.h"

enum SkirmishStrategyMode
{
	SKIRMISH_STRATEGY_NONE = -1,
	SKIRMISH_STRATEGY_BALANCED = 0,
	SKIRMISH_STRATEGY_FORTIFY,
	SKIRMISH_STRATEGY_ASSAULT
};

enum SkirmishStrategyAttemptStatus
{
	SKIRMISH_STRATEGY_ATTEMPT_NOT_STARTED = 0,
	SKIRMISH_STRATEGY_ATTEMPT_PENDING,
	SKIRMISH_STRATEGY_ATTEMPT_SUCCEEDED,
	SKIRMISH_STRATEGY_ATTEMPT_FAILED
};

enum SkirmishStrategyReason
{
	SKIRMISH_STRATEGY_REASON_NOT_DUE = 0,
	SKIRMISH_STRATEGY_REASON_NO_TRIGGER,
	SKIRMISH_STRATEGY_REASON_PENDING_STARTED,
	SKIRMISH_STRATEGY_REASON_PENDING_CONTINUED,
	SKIRMISH_STRATEGY_REASON_PENDING_CLEARED,
	SKIRMISH_STRATEGY_REASON_MINIMUM_DURATION,

	SKIRMISH_STRATEGY_REASON_FORTIFY_CRITICAL_BASE,
	SKIRMISH_STRATEGY_REASON_FORTIFY_SEVERE_THREAT,
	SKIRMISH_STRATEGY_REASON_FORTIFY_PRESSURE,

	SKIRMISH_STRATEGY_REASON_ASSAULT_READINESS,
	SKIRMISH_STRATEGY_REASON_ASSAULT_EXPOSED_OPPORTUNITY,
	SKIRMISH_STRATEGY_REASON_ASSAULT_SUPERWEAPON_FIRED,
	SKIRMISH_STRATEGY_REASON_ASSAULT_FORCE_ASSEMBLED,

	SKIRMISH_STRATEGY_REASON_BALANCED_FORTIFY_STABILIZED,
	SKIRMISH_STRATEGY_REASON_BALANCED_ASSAULT_LOW_READINESS,
	SKIRMISH_STRATEGY_REASON_BALANCED_ASSAULT_FORCE_LOSS,
	SKIRMISH_STRATEGY_REASON_BALANCED_ASSAULT_OBJECTIVE_COMPLETE,
	SKIRMISH_STRATEGY_REASON_BALANCED_ASSAULT_TARGET_UNAVAILABLE,

	SKIRMISH_STRATEGY_REASON_ASSAULT_FORTIFY_DEADLINE,
	SKIRMISH_STRATEGY_REASON_BALANCED_FORTIFY_DEADLINE
};

struct SkirmishStrategyMetrics
{
	Int economyHealth;
	Int baseIntegrity;
	Int armyReadiness;
	Int immediateThreat;
	Int attackConfidence;
	Int enemyOpportunity;
	Int alliedDistress;
	Int availableCombatValue;

	Bool hasStrategicTarget;
	Bool assaultLostHalfForce;
	Bool assaultObjectiveComplete;
	Bool viableAssaultForceAssembled;
};

struct SkirmishStrategyState
{
	SkirmishStrategyMode currentMode;
	SkirmishStrategyMode pendingMode;
	UnsignedInt modeEntryFrame;
	UnsignedInt pendingSinceFrame;
	UnsignedInt nextEvaluationFrame;

	ObjectID strategicTargetID;
	Bool strategicTargetObserved;
	UnsignedInt strategicTargetLastSeenFrame;
	Int assaultEntryCombatValue;

	SkirmishStrategyAttemptStatus fortifyAttemptStatus;
	SkirmishStrategyAttemptStatus superweaponAttemptStatus;

	Bool assaultAssemblyDeadlineActive;
	UnsignedInt assaultAssemblyDeadlineFrame;

	Bool alliedCoordinationCooldownActive;
	UnsignedInt nextAlliedCoordinationFrame;

	Bool donationCooldownActive;
	UnsignedInt nextDonationFrame;
};

struct SkirmishStrategyDecision
{
	SkirmishStrategyState nextState;
	SkirmishStrategyReason reason;
	UnsignedInt fortifyPressureHundredths;
	UnsignedInt assaultReadinessHundredths;
	Bool evaluated;
	Bool modeChanged;
};

UnsignedInt CalculateSkirmishFortifyPressure( const SkirmishStrategyMetrics &metrics );
UnsignedInt CalculateSkirmishAssaultReadiness( const SkirmishStrategyMetrics &metrics );

Bool IsSkirmishStrategyFrameReached( UnsignedInt currentFrame, UnsignedInt targetFrame );
Bool HasSkirmishStrategyDurationElapsed( UnsignedInt currentFrame, UnsignedInt startFrame,
	UnsignedInt durationFrames );
Bool IsSkirmishStrategyTargetObservationAvailable( Bool observed,
	UnsignedInt currentFrame, UnsignedInt lastSeenFrame );
void ClearSkirmishStrategyTargetObservation( SkirmishStrategyState *state );

void InitializeSkirmishStrategyState( SkirmishStrategyState *state, UnsignedInt currentFrame );
void InitializeOldSaveSkirmishStrategyState( SkirmishStrategyState *state, UnsignedInt currentFrame );
void ArmOldSaveSkirmishFortifyDeadline( SkirmishStrategyState *state,
	GameDifficulty difficulty, UnsignedInt currentFrame );

SkirmishStrategyDecision EvaluateSkirmishStrategy( const SkirmishStrategyState &state,
	const SkirmishStrategyMetrics &metrics, GameDifficulty difficulty, UnsignedInt currentFrame,
	Bool useProductionBehavior = false );
