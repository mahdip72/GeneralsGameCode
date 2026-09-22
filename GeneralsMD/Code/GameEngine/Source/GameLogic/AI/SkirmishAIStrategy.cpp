/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2026 TheSuperHackers
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
*/

#include "PreRTS.h"

#include "GameLogic/SkirmishAIStrategy.h"

namespace
{
	const UnsignedInt SKIRMISH_FORTIFY_ENTRY_THRESHOLD = 6500;
	const UnsignedInt SKIRMISH_FORTIFY_EXIT_THRESHOLD = 4000;
	const UnsignedInt SKIRMISH_ASSAULT_ENTRY_THRESHOLD = 7000;
	const UnsignedInt SKIRMISH_ASSAULT_EXIT_THRESHOLD = 4500;
	const UnsignedInt SKIRMISH_TARGET_LAST_SEEN_GRACE_FRAMES =
		30 * LOGICFRAMES_PER_SECOND;
	const UnsignedInt SKIRMISH_FRAME_HALF_RANGE = 0x80000000U;

	Int ClampStrategyMetric( Int value )
	{
		if( value < 0 )
			return 0;
		if( value > 100 )
			return 100;
		return value;
	}

	UnsignedInt GetEvaluationIntervalFrames( GameDifficulty difficulty )
	{
		if( difficulty == DIFFICULTY_EASY )
			return 20 * LOGICFRAMES_PER_SECOND;
		if( difficulty == DIFFICULTY_HARD )
			return 5 * LOGICFRAMES_PER_SECOND;
		return 10 * LOGICFRAMES_PER_SECOND;
	}

	UnsignedInt GetConfirmationFrames( GameDifficulty difficulty )
	{
		if( difficulty == DIFFICULTY_EASY )
			return 30 * LOGICFRAMES_PER_SECOND;
		if( difficulty == DIFFICULTY_HARD )
			return 10 * LOGICFRAMES_PER_SECOND;
		return 20 * LOGICFRAMES_PER_SECOND;
	}

	UnsignedInt GetMinimumModeFrames( SkirmishStrategyMode mode )
	{
		if( mode == SKIRMISH_STRATEGY_FORTIFY )
			return 120 * LOGICFRAMES_PER_SECOND;
		return 90 * LOGICFRAMES_PER_SECOND;
	}

	Bool MeetsNormalAssaultRequirements( const SkirmishStrategyMetrics &metrics,
		UnsignedInt readiness )
	{
		return readiness >= SKIRMISH_ASSAULT_ENTRY_THRESHOLD &&
			metrics.hasStrategicTarget &&
			ClampStrategyMetric( metrics.economyHealth ) >= 50 &&
			ClampStrategyMetric( metrics.baseIntegrity ) >= 65 &&
			ClampStrategyMetric( metrics.armyReadiness ) >= 60 &&
			ClampStrategyMetric( metrics.immediateThreat ) < 60;
	}

	Bool MeetsFortifyOpportunityRequirements( const SkirmishStrategyMetrics &metrics )
	{
		return ClampStrategyMetric( metrics.enemyOpportunity ) >= 90 &&
			metrics.hasStrategicTarget &&
			ClampStrategyMetric( metrics.economyHealth ) >= 50 &&
			ClampStrategyMetric( metrics.baseIntegrity ) >= 70 &&
			ClampStrategyMetric( metrics.armyReadiness ) >= 55 &&
			ClampStrategyMetric( metrics.immediateThreat ) <= 35;
	}

	void ClearPendingMode( SkirmishStrategyState *state )
	{
		state->pendingMode = SKIRMISH_STRATEGY_NONE;
		state->pendingSinceFrame = 0;
	}

	void CommitMode( SkirmishStrategyDecision *decision, SkirmishStrategyMode mode,
		SkirmishStrategyReason reason, UnsignedInt currentFrame )
	{
		decision->modeChanged = decision->nextState.currentMode != mode;
		decision->nextState.currentMode = mode;
		if( mode != SKIRMISH_STRATEGY_ASSAULT )
		{
			decision->nextState.assaultEntryCombatValue = 0;
			ClearSkirmishStrategyTargetObservation( &decision->nextState );
		}
		decision->nextState.modeEntryFrame = currentFrame;
		ClearPendingMode( &decision->nextState );
		decision->reason = reason;
	}
}

UnsignedInt CalculateSkirmishFortifyPressure( const SkirmishStrategyMetrics &metrics )
{
	const Int economy = ClampStrategyMetric( metrics.economyHealth );
	const Int base = ClampStrategyMetric( metrics.baseIntegrity );
	const Int army = ClampStrategyMetric( metrics.armyReadiness );
	const Int threat = ClampStrategyMetric( metrics.immediateThreat );
	const Int confidence = ClampStrategyMetric( metrics.attackConfidence );

	return 30 * (100 - base) + 25 * (100 - economy) + 20 * threat +
		15 * (100 - army) + 10 * (100 - confidence);
}

UnsignedInt CalculateSkirmishAssaultReadiness( const SkirmishStrategyMetrics &metrics )
{
	const Int economy = ClampStrategyMetric( metrics.economyHealth );
	const Int base = ClampStrategyMetric( metrics.baseIntegrity );
	const Int army = ClampStrategyMetric( metrics.armyReadiness );
	const Int confidence = ClampStrategyMetric( metrics.attackConfidence );
	const Int opportunity = ClampStrategyMetric( metrics.enemyOpportunity );

	return 25 * economy + 20 * base + 25 * army + 15 * confidence + 15 * opportunity;
}

Bool IsSkirmishStrategyFrameReached( UnsignedInt currentFrame, UnsignedInt targetFrame )
{
	return currentFrame - targetFrame < SKIRMISH_FRAME_HALF_RANGE;
}

Bool HasSkirmishStrategyDurationElapsed( UnsignedInt currentFrame, UnsignedInt startFrame,
	UnsignedInt durationFrames )
{
	return currentFrame - startFrame >= durationFrames;
}

Bool IsSkirmishStrategyTargetObservationAvailable( Bool observed,
	UnsignedInt currentFrame, UnsignedInt lastSeenFrame )
{
	return observed && !HasSkirmishStrategyDurationElapsed(
		currentFrame, lastSeenFrame, SKIRMISH_TARGET_LAST_SEEN_GRACE_FRAMES );
}

void ClearSkirmishStrategyTargetObservation( SkirmishStrategyState *state )
{
	state->strategicTargetID = INVALID_ID;
	state->strategicTargetObserved = false;
	state->strategicTargetLastSeenFrame = 0;
}

void InitializeSkirmishStrategyState( SkirmishStrategyState *state, UnsignedInt currentFrame )
{
	state->currentMode = SKIRMISH_STRATEGY_BALANCED;
	state->pendingMode = SKIRMISH_STRATEGY_NONE;
	state->modeEntryFrame = currentFrame;
	state->pendingSinceFrame = 0;
	state->nextEvaluationFrame = currentFrame;
	ClearSkirmishStrategyTargetObservation( state );
	state->assaultEntryCombatValue = 0;
	state->fortifyAttemptStatus = SKIRMISH_STRATEGY_ATTEMPT_NOT_STARTED;
	state->superweaponAttemptStatus = SKIRMISH_STRATEGY_ATTEMPT_NOT_STARTED;
	state->assaultAssemblyDeadlineActive = false;
	state->assaultAssemblyDeadlineFrame = 0;
	state->alliedCoordinationCooldownActive = false;
	state->nextAlliedCoordinationFrame = 0;
	state->donationCooldownActive = false;
	state->nextDonationFrame = 0;
}

void InitializeOldSaveSkirmishStrategyState( SkirmishStrategyState *state, UnsignedInt currentFrame )
{
	InitializeSkirmishStrategyState( state, currentFrame );
	state->modeEntryFrame = currentFrame - 90 * LOGICFRAMES_PER_SECOND;
}

SkirmishStrategyDecision EvaluateSkirmishStrategy( const SkirmishStrategyState &state,
	const SkirmishStrategyMetrics &metrics, GameDifficulty difficulty, UnsignedInt currentFrame )
{
	SkirmishStrategyDecision decision;
	SkirmishStrategyMode candidateMode = SKIRMISH_STRATEGY_NONE;
	SkirmishStrategyReason candidateReason = SKIRMISH_STRATEGY_REASON_NO_TRIGGER;
	const Int base = ClampStrategyMetric( metrics.baseIntegrity );
	const Int threat = ClampStrategyMetric( metrics.immediateThreat );

	decision.nextState = state;
	decision.reason = SKIRMISH_STRATEGY_REASON_NOT_DUE;
	decision.fortifyPressureHundredths = CalculateSkirmishFortifyPressure( metrics );
	decision.assaultReadinessHundredths = CalculateSkirmishAssaultReadiness( metrics );
	decision.evaluated = false;
	decision.modeChanged = false;

	if( !IsSkirmishStrategyFrameReached( currentFrame, state.nextEvaluationFrame ) )
		return decision;

	decision.evaluated = true;
	decision.nextState.nextEvaluationFrame = currentFrame + GetEvaluationIntervalFrames( difficulty );
	if( state.currentMode != SKIRMISH_STRATEGY_ASSAULT )
		decision.nextState.assaultEntryCombatValue = 0;

	if( base < 35 )
	{
		ClearPendingMode( &decision.nextState );
		if( state.currentMode != SKIRMISH_STRATEGY_FORTIFY )
			CommitMode( &decision, SKIRMISH_STRATEGY_FORTIFY,
				SKIRMISH_STRATEGY_REASON_FORTIFY_CRITICAL_BASE, currentFrame );
		else
			decision.reason = SKIRMISH_STRATEGY_REASON_FORTIFY_CRITICAL_BASE;
		return decision;
	}

	if( threat >= 85 )
	{
		ClearPendingMode( &decision.nextState );
		if( state.currentMode != SKIRMISH_STRATEGY_FORTIFY )
			CommitMode( &decision, SKIRMISH_STRATEGY_FORTIFY,
				SKIRMISH_STRATEGY_REASON_FORTIFY_SEVERE_THREAT, currentFrame );
		else
			decision.reason = SKIRMISH_STRATEGY_REASON_FORTIFY_SEVERE_THREAT;
		return decision;
	}

	if( state.currentMode == SKIRMISH_STRATEGY_ASSAULT && metrics.assaultObjectiveComplete )
	{
		CommitMode( &decision, SKIRMISH_STRATEGY_BALANCED,
			SKIRMISH_STRATEGY_REASON_BALANCED_ASSAULT_OBJECTIVE_COMPLETE, currentFrame );
		return decision;
	}

	if( state.currentMode == SKIRMISH_STRATEGY_BALANCED )
	{
		if( decision.fortifyPressureHundredths >= SKIRMISH_FORTIFY_ENTRY_THRESHOLD )
		{
			candidateMode = SKIRMISH_STRATEGY_FORTIFY;
			candidateReason = SKIRMISH_STRATEGY_REASON_FORTIFY_PRESSURE;
		}
		else if( MeetsNormalAssaultRequirements( metrics,
			decision.assaultReadinessHundredths ) )
		{
			candidateMode = SKIRMISH_STRATEGY_ASSAULT;
			candidateReason = SKIRMISH_STRATEGY_REASON_ASSAULT_READINESS;
		}
	}
	else if( state.currentMode == SKIRMISH_STRATEGY_FORTIFY )
	{
		if( metrics.hasStrategicTarget &&
			state.superweaponAttemptStatus == SKIRMISH_STRATEGY_ATTEMPT_SUCCEEDED )
		{
			candidateMode = SKIRMISH_STRATEGY_ASSAULT;
			candidateReason = SKIRMISH_STRATEGY_REASON_ASSAULT_SUPERWEAPON_FIRED;
		}
		else if( metrics.hasStrategicTarget &&
			metrics.viableAssaultForceAssembled )
		{
			candidateMode = SKIRMISH_STRATEGY_ASSAULT;
			candidateReason = SKIRMISH_STRATEGY_REASON_ASSAULT_FORCE_ASSEMBLED;
		}
		else if( MeetsFortifyOpportunityRequirements( metrics ) )
		{
			candidateMode = SKIRMISH_STRATEGY_ASSAULT;
			candidateReason = SKIRMISH_STRATEGY_REASON_ASSAULT_EXPOSED_OPPORTUNITY;
		}
		else if( decision.fortifyPressureHundredths <= SKIRMISH_FORTIFY_EXIT_THRESHOLD )
		{
			candidateMode = SKIRMISH_STRATEGY_BALANCED;
			candidateReason = SKIRMISH_STRATEGY_REASON_BALANCED_FORTIFY_STABILIZED;
		}
	}
	else if( state.currentMode == SKIRMISH_STRATEGY_ASSAULT )
	{
		if( decision.fortifyPressureHundredths >= SKIRMISH_FORTIFY_ENTRY_THRESHOLD )
		{
			candidateMode = SKIRMISH_STRATEGY_FORTIFY;
			candidateReason = SKIRMISH_STRATEGY_REASON_FORTIFY_PRESSURE;
		}
		else if( !metrics.hasStrategicTarget )
		{
			candidateMode = SKIRMISH_STRATEGY_BALANCED;
			candidateReason = SKIRMISH_STRATEGY_REASON_BALANCED_ASSAULT_TARGET_UNAVAILABLE;
		}
		else if( metrics.assaultLostHalfForce )
		{
			candidateMode = SKIRMISH_STRATEGY_BALANCED;
			candidateReason = SKIRMISH_STRATEGY_REASON_BALANCED_ASSAULT_FORCE_LOSS;
		}
		else if( decision.assaultReadinessHundredths < SKIRMISH_ASSAULT_EXIT_THRESHOLD )
		{
			candidateMode = SKIRMISH_STRATEGY_BALANCED;
			candidateReason = SKIRMISH_STRATEGY_REASON_BALANCED_ASSAULT_LOW_READINESS;
		}
	}

	if( candidateMode == SKIRMISH_STRATEGY_NONE )
	{
		if( decision.nextState.pendingMode != SKIRMISH_STRATEGY_NONE )
		{
			ClearPendingMode( &decision.nextState );
			decision.reason = SKIRMISH_STRATEGY_REASON_PENDING_CLEARED;
		}
		else
		{
			decision.reason = SKIRMISH_STRATEGY_REASON_NO_TRIGGER;
		}
		return decision;
	}

	if( decision.nextState.pendingMode != candidateMode )
	{
		decision.nextState.pendingMode = candidateMode;
		decision.nextState.pendingSinceFrame = currentFrame;
		decision.reason = SKIRMISH_STRATEGY_REASON_PENDING_STARTED;
		return decision;
	}

	if( !HasSkirmishStrategyDurationElapsed( currentFrame,
		decision.nextState.pendingSinceFrame, GetConfirmationFrames( difficulty ) ) )
	{
		decision.reason = SKIRMISH_STRATEGY_REASON_PENDING_CONTINUED;
		return decision;
	}

	if( !HasSkirmishStrategyDurationElapsed( currentFrame, state.modeEntryFrame,
		GetMinimumModeFrames( state.currentMode ) ) )
	{
		decision.reason = SKIRMISH_STRATEGY_REASON_MINIMUM_DURATION;
		return decision;
	}

	CommitMode( &decision, candidateMode, candidateReason, currentFrame );
	if( candidateMode == SKIRMISH_STRATEGY_ASSAULT )
		decision.nextState.assaultEntryCombatValue =
			metrics.availableCombatValue > 0 ? metrics.availableCombatValue : 0;
	return decision;
}
