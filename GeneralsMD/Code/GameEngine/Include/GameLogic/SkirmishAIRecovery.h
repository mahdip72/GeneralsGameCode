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

// This header deliberately contains only POD data and pure policy helpers.
// Keep it usable by VC6/C++98 regression tests and by the live AI code.

class ThingTemplate;

struct SkirmishAIRecoveryPolicyInput
{
	bool enabled;
	bool everCompleted;
	bool hasPrimaryCommandCenter;
	bool hasConstruction;
	bool hasBuilder;
	bool builderQueued;
	bool builderQueuePaid;
	bool hasBuilderFactory;
	bool noBuilderPath;
	bool builderAffordable;
	bool commandCenterAffordable;
	bool placementReady;
	int commandCenterCost;
	int builderCost;
	int protectedReserve;
};

struct SkirmishAIRecoveryPolicyResult
{
	bool shouldQueueBuilder;
	bool shouldConstructCommandCenter;
	bool recoveryImpossible;
	bool shouldRetry;
	int reserveCost;
};

inline int AddSkirmishAIRecoveryCost(int first, int second)
{
	if (first < 0)
		first = 0;
	if (second < 0)
		second = 0;
	if (first > 2147483647 - second)
		return 2147483647;
	return first + second;
}

inline bool IsSkirmishAIRecoveryBuilderPathUnavailable(
	bool hasBuilder, bool hasContainedBuilderRoute,
	bool builderQueuePaid, bool hasPotentialFactory)
{
	return !hasBuilder && !hasContainedBuilderRoute &&
		!builderQueuePaid && !hasPotentialFactory;
}

inline bool IsSkirmishAIRecoveryFactoryAdmissionInternallyProgressing(
	bool canMake, bool queueFull, bool parkingPlacesFull,
	bool productionCanAdvance)
{
	return productionCanAdvance &&
		(canMake || queueFull || parkingPlacesFull);
}

inline bool IsSkirmishAIRecoveryFactorySchedulingBounded(
	bool canMake, bool queueFull, bool parkingPlacesFull,
	bool productionCanAdvance)
{
	return (canMake || queueFull || parkingPlacesFull) &&
		!productionCanAdvance;
}

inline bool IsSkirmishAIRecoveryFactoryBestCandidate(
	bool canMake, bool productionCanAdvance)
{
	return canMake && productionCanAdvance;
}

inline bool IsSkirmishAIRecoveryBuilderAdmissionActionable(
	bool canMake, bool aiUpdateCanAdvance)
{
	return canMake && aiUpdateCanAdvance;
}

inline bool IsSkirmishAIRecoveryBuilderUpdateBounded(
	bool hasPhysicalRoute, bool aiUpdateCanAdvance)
{
	return hasPhysicalRoute && !aiUpdateCanAdvance;
}

inline bool IsSkirmishAIRecoveryAdmissionBounded(
	bool noMoney, bool factoryDisabled,
	bool noPrerequisite, bool maxedOutForPlayer)
{
	return noMoney || factoryDisabled || noPrerequisite || maxedOutForPlayer;
}

inline bool IsSkirmishAIRecoveryFactoryPotentialDuringGrace(
	bool internallyProgressing, bool boundedOnly, bool graceActive)
{
	return internallyProgressing || (boundedOnly && graceActive);
}

inline bool ShouldLatchSkirmishAIRecoveryLastStand(
	bool hasPhysicalRetryRoute)
{
	return !hasPhysicalRetryRoute;
}

inline bool ShouldPreserveSkirmishAIRecoveryTrackedObject(
	bool permanent, bool liveOwnedObject,
	bool underConstructionCommandCenter, bool validPrimaryRebuildHole)
{
	return !permanent && liveOwnedObject &&
		(underConstructionCommandCenter || validPrimaryRebuildHole);
}

inline bool IsSkirmishAIRecoveryPaidQueueBounded(
	bool paid, bool liveOwnedProducer,
	bool productionCanAdvance)
{
	return paid && (!liveOwnedProducer || !productionCanAdvance);
}

inline bool ShouldFailoverSkirmishAIRecoveryPaidQueue(
	bool paidQueueBounded, bool graceActive, bool replacementObserved,
	bool hasAdvancingAlternate, bool sameProducer)
{
	return paidQueueBounded && !graceActive && !replacementObserved &&
		hasAdvancingAlternate && !sameProducer;
}

inline bool ShouldSearchSkirmishAIRecoveryPaidQueueFailover(
	bool paidQueueBounded, bool graceActive, bool replacementObserved)
{
	return paidQueueBounded && !graceActive && !replacementObserved;
}

inline bool ShouldReturnFromSkirmishAIRecoveryBoundedQueueFailure(
	bool paidQueueBounded, bool failoverSucceeded,
	bool hasPresentUnusableNativeWorker)
{
	return paidQueueBounded &&
		(failoverSucceeded || !hasPresentUnusableNativeWorker);
}

inline bool IsSkirmishAIRecoveryFailoverAdmissionEligible(
	bool canMake, bool noMoney, bool maxed, bool refundAffordable,
	bool cancellationFreesMax, bool nonMaxBuildable, bool queueReady)
{
	return canMake ||
		(noMoney && refundAffordable) ||
		(maxed && refundAffordable && cancellationFreesMax &&
		 nonMaxBuildable && queueReady);
}

inline int GetSkirmishAIRecoveryFailoverAdmissionRank(
	bool canMake, bool noMoney, bool maxed)
{
	if (canMake)
		return 0;
	if (noMoney)
		return 1;
	if (maxed)
		return 2;
	return 3;
}

inline bool ShouldClearSkirmishAIRecoveryFailoverBinding(
	bool sameFactory, bool equivalentTemplate, bool incomplete)
{
	return sameFactory && equivalentTemplate && incomplete;
}

inline bool WouldSkirmishAIRecoveryCancellationFreeMax(
	unsigned int maxCount, unsigned int predictedCountAfterCancellation)
{
	return maxCount != 0 && predictedCountAfterCancellation < maxCount;
}

inline bool ShouldSelectSkirmishAIRecoveryPaidQueueProducer(
	bool hasSelection, bool selectedCanAdvance, bool candidateCanAdvance)
{
	return !hasSelection || (!selectedCanAdvance && candidateCanAdvance);
}

inline bool ShouldPreferTrackedSkirmishAIRecoveryPaidQueue(
	bool trackedEntryExists, bool selectedCanAdvance)
{
	// A progressing compatible queue is already the fastest recovery route.
	// Exact provenance breaks ties only when every compatible queue is bounded.
	return trackedEntryExists && !selectedCanAdvance;
}

inline bool ShouldClearSkirmishAIRecoveryExactFailoverBinding(
	bool selectedIsFirstCompatible, bool sameFactory,
	bool equivalentTemplate, bool incomplete)
{
	return selectedIsFirstCompatible &&
		ShouldClearSkirmishAIRecoveryFailoverBinding(
			sameFactory, equivalentTemplate, incomplete);
}

inline bool IsSkirmishAIRecoveryReservedNativeWorker(
	bool liveOwnedHole, int holeWorkerID, int candidateWorkerID,
	int invalidObjectID)
{
	return liveOwnedHole && holeWorkerID != invalidObjectID &&
		holeWorkerID == candidateWorkerID;
}

inline bool ShouldUseSkirmishAIRecoveryNativeWorkerAsAssigned(
	bool centerAssignedWorkerUsable, bool currentNativeHole,
	bool nativeWorkerLive, bool nativeWorkerCompatible)
{
	return !centerAssignedWorkerUsable && currentNativeHole &&
		nativeWorkerLive && nativeWorkerCompatible;
}

inline bool IsSkirmishAIRecoveryAssignedBuilderOperational(
	bool liveOwned, bool contained, bool unmanned, bool hasAIUpdate,
	bool hasDozerAI, bool updateCanAdvance)
{
	return liveOwned && !contained && !unmanned && hasAIUpdate && hasDozerAI &&
		updateCanAdvance;
}

inline bool IsSkirmishAIRecoveryAssignedBuilderUsable(
	bool operational, bool buildDockFound, bool pathAvailable)
{
	return operational && buildDockFound && pathAvailable;
}

inline bool IsSkirmishAIRecoveryNativeWorkerActivelyBuilding(
	bool liveOwned, bool updateCanAdvance, bool currentTaskIsBuild,
	bool buildSubTaskIsAtDock, bool targetsExactScaffold)
{
	return liveOwned && updateCanAdvance && currentTaskIsBuild &&
		buildSubTaskIsAtDock && targetsExactScaffold;
}

inline bool ShouldPreserveSkirmishAIRecoveryNativeWorkerLifecycle(
	bool currentNativeHole, bool nativeWorkerExists,
	bool nativeWorkerLiveOwned, bool nativeWorkerIsDozer,
	bool nativeWorkerCompatible, bool nativeWorkerResumeUsable)
{
	if (!currentNativeHole)
		return false;
	// A missing exact worker is the native respawn gap. If the exact ObjectID
	// still resolves, the hole will not start its respawn timer, so preserve the
	// native lifecycle only while that object is an owned compatible dozer that
	// can actually reach and resume this exact scaffold.
	return !nativeWorkerExists ||
		(nativeWorkerLiveOwned && nativeWorkerIsDozer && nativeWorkerCompatible &&
		 nativeWorkerResumeUsable);
}

inline bool ShouldPreserveSkirmishAIRecoveryNativeHoleLifecycle(
	bool useCurrentNativeHoleOwnership, bool hasPrimaryNativeHole)
{
	return !useCurrentNativeHoleOwnership && hasPrimaryNativeHole;
}

inline bool ShouldCancelSkirmishAIRecoveryPaidQueueForNativeRespawn(
	bool hasNativeHole, bool assignedWorkerLive,
	bool exactIdentityTracked, bool exactEntryExists)
{
	return hasNativeHole && !assignedWorkerLive &&
		exactIdentityTracked && exactEntryExists;
}

inline bool ShouldCancelSkirmishAIRecoveryExactPaidQueueForNativeLifecycle(
	bool nativeLifecycleOwned, bool cancellationOwned,
	bool exactIdentityTracked,
	bool exactEntryExists)
{
	return nativeLifecycleOwned && cancellationOwned &&
		exactIdentityTracked && exactEntryExists;
}

inline bool ShouldCancelSkirmishAIRecoveryExactPaidQueueForFailover(
	bool cancellationOwned, bool exactIdentityMatches,
	bool exactEntryExists)
{
	return cancellationOwned && exactIdentityMatches && exactEntryExists;
}

inline bool ShouldUseSkirmishAIRecoveryNonCancellingFailover(
	bool cancellationOwned, bool exactIdentityMatches,
	bool exactEntryExists)
{
	return !cancellationOwned && exactIdentityMatches && exactEntryExists;
}

inline bool IsSkirmishAIRecoveryNonCancellingFailoverAdmissionEligible(
	bool canMake, bool independentlyAffordable, bool queueReady)
{
	return canMake && independentlyAffordable && queueReady;
}

inline bool ShouldSelectSkirmishAIPrimaryCommandCenter(
	bool hasSelection, bool selectedCompleted, int selectedObjectID,
	bool candidateCompleted, int candidateObjectID)
{
	return !hasSelection ||
		(candidateCompleted && !selectedCompleted) ||
		(candidateCompleted == selectedCompleted &&
		 candidateObjectID < selectedObjectID);
}

inline bool IsSkirmishAIRecoveryContainedBuilderRoute(
	bool hasCompatibleBuilder, bool isContained,
	bool hasLiveContainer, bool hasOwnedContainer,
	bool hasContainInterface, bool containReportsBuilder)
{
	return hasCompatibleBuilder && isContained && hasLiveContainer &&
		hasOwnedContainer && hasContainInterface && containReportsBuilder;
}

inline bool ShouldOrderSkirmishAIRecoveryBuilderExit(
	bool hasValidContainedBuilderRoute, bool builderAlreadyExiting)
{
	return hasValidContainedBuilderRoute && !builderAlreadyExiting;
}

inline bool IsSkirmishAIRecoveryInsuranceBuilderCandidate(
	bool isContained, bool isDozer, bool isUnmanned)
{
	return !isContained && isDozer && !isUnmanned;
}

inline bool ShouldSuppressSkirmishAIRecoveryBuilderOrder(
	bool isResourceGatherer, bool isCompatibleBuilder,
	bool recoveryEnabled, bool recoveryEverCompleted,
	bool recoveryImpossible, bool hasCompletedPrimaryCommandCenter,
	int reserveCost, bool hasPaidCompatibleBuilderQueue,
	bool nativePrimaryRebuildPending = false)
{
	if (!isCompatibleBuilder || !recoveryEnabled ||
		!recoveryEverCompleted || recoveryImpossible ||
		hasCompletedPrimaryCommandCenter)
		return false;
	// The native hole owns a free worker lifecycle for this exact primary
	// reconstruction. Ordinary WorkOrders must not replace an exact recovery
	// cancellation while that worker is live or awaiting respawn.
	if (nativePrimaryRebuildPending)
		return true;
	// Once recovery has paid for an equivalent builder, suppress every ordinary
	// WorkOrder so a resumed producer cannot pay a duplicate. Without a paid
	// entry, a resource worker remains an income route while other builders keep
	// the reserve gate.
	if (hasPaidCompatibleBuilderQueue)
		return true;
	if (isResourceGatherer)
		return false;
	return reserveCost > 0;
}

inline bool IsSkirmishAIRecoveryReusableWorkOrder(
	bool unbound, bool equivalentTemplate, bool incomplete,
	bool belongsToDefaultTeam, bool reinforcement)
{
	return unbound && equivalentTemplate && incomplete &&
		belongsToDefaultTeam && !reinforcement;
}

inline bool ShouldSelectSkirmishAIRecoveryReusableWorkOrder(
	bool hasSelection, bool selectedIsResourceGatherer,
	bool candidateEligible, bool candidateIsResourceGatherer)
{
	return candidateEligible &&
		(!hasSelection ||
			 (!selectedIsResourceGatherer && candidateIsResourceGatherer));
}

struct SkirmishAIRecoveryQueueCommit
{
	bool bindReusableWorkOrder;
	bool storeProductionIdentity;
};

inline SkirmishAIRecoveryQueueCommit GetSkirmishAIRecoveryQueueCommit(
	bool hasReusableWorkOrder, bool queueSucceeded)
{
	SkirmishAIRecoveryQueueCommit result;
	result.bindReusableWorkOrder = hasReusableWorkOrder && queueSucceeded;
	result.storeProductionIdentity = queueSucceeded;
	return result;
}

inline bool IsSkirmishAIRecoveryFactoryFinisherUsable(
	bool isContained, bool isUnmanned, bool hasDozerAI,
	bool hasBuildTask, bool targetsFactory, bool hasBuildDock,
	bool hasPathToBuildDock, bool aiUpdateCanAdvance)
{
	return !isContained && !isUnmanned && hasDozerAI &&
		hasBuildTask && targetsFactory && hasBuildDock &&
		hasPathToBuildDock && aiUpdateCanAdvance;
}

inline bool IsSkirmishAIRecoveryHoleConstructionMatch(
	bool isLiveOwnedHole, bool hasRebuildTemplate,
	bool rebuildTemplateEquivalent, unsigned int reconstructedBuildingID,
	unsigned int constructionID)
{
	return isLiveOwnedHole && hasRebuildTemplate &&
		rebuildTemplateEquivalent &&
		reconstructedBuildingID == constructionID;
}

inline bool ShouldRestoreSkirmishAIRecoveryScaffoldBuilder(
	bool priorBuilderStillLive, bool replacementTaskEstablished)
{
	return priorBuilderStillLive && !replacementTaskEstablished;
}

inline bool HasSkirmishAIRecoveryScaffoldReplacementAttempt(
	int placementAttempt, int placementOffsetCount)
{
	// Placement uses this value modulo the offset count.  The next band is a
	// persisted one-bit marker, avoiding a snapshot layout change.
	return placementOffsetCount > 0 &&
		placementAttempt >= placementOffsetCount;
}

inline int MarkSkirmishAIRecoveryScaffoldReplacementAttempt(
	int placementAttempt, int placementOffsetCount)
{
	if (placementOffsetCount <= 0)
		return placementAttempt;
	if (placementAttempt < 0)
		placementAttempt = 0;
	return HasSkirmishAIRecoveryScaffoldReplacementAttempt(
		placementAttempt, placementOffsetCount)
		? placementAttempt : placementAttempt + placementOffsetCount;
}

inline bool HasSkirmishAIRecoveryObservedReplacement(
	int placementAttempt, int placementOffsetCount)
{
	return placementOffsetCount > 0 &&
		placementAttempt >= 2 * placementOffsetCount;
}

inline bool HasSkirmishAIRecoveryNativeWorkerRecycleAttempt(
	int placementAttempt, int placementOffsetCount)
{
	return placementOffsetCount > 0 &&
		placementAttempt >= 3 * placementOffsetCount;
}

inline int MarkSkirmishAIRecoveryNativeWorkerRecycleAttempt(
	int placementAttempt, int placementOffsetCount)
{
	if (placementOffsetCount <= 0)
		return placementAttempt;
	if (placementAttempt < 0)
		placementAttempt = 0;
	return 3 * placementOffsetCount +
		placementAttempt % placementOffsetCount;
}

inline int ClearSkirmishAIRecoveryNativeWorkerRecycleAttempt(
	int placementAttempt, int placementOffsetCount)
{
	if (placementOffsetCount <= 0 || placementAttempt < 0)
		return placementAttempt;
	return placementAttempt % placementOffsetCount;
}

inline int ReconcileSkirmishAIRecoveryNativeWorkerRecycleAfterLineageScan(
	int placementAttempt, int placementOffsetCount,
	bool hasLivePrimaryScaffold, bool hasMatchingNativeHole)
{
	(void)hasLivePrimaryScaffold;
	if (hasMatchingNativeHole ||
		!HasSkirmishAIRecoveryNativeWorkerRecycleAttempt(
			placementAttempt, placementOffsetCount))
		return placementAttempt;
	return ClearSkirmishAIRecoveryNativeWorkerRecycleAttempt(
		placementAttempt, placementOffsetCount);
}

inline int MarkSkirmishAIRecoveryObservedReplacement(
	int placementAttempt, int placementOffsetCount)
{
	if (placementOffsetCount <= 0)
		return placementAttempt;
	if (placementAttempt < 0)
		placementAttempt = 0;
	if (HasSkirmishAIRecoveryNativeWorkerRecycleAttempt(
			placementAttempt, placementOffsetCount))
		return placementAttempt;
	return 2 * placementOffsetCount +
		placementAttempt % placementOffsetCount;
}

inline int ReconcileSkirmishAIRecoveryReplacementAttempt(
	int placementAttempt, int placementOffsetCount,
	bool paidQueueExists)
{
	if (!HasSkirmishAIRecoveryScaffoldReplacementAttempt(
			placementAttempt, placementOffsetCount) || paidQueueExists)
		return placementAttempt;
	if (!HasSkirmishAIRecoveryObservedReplacement(
			placementAttempt, placementOffsetCount))
		return placementAttempt % placementOffsetCount;
	return placementAttempt;
}

inline int AdvanceSkirmishAIRecoveryPlacementAttempt(
	int placementAttempt, int placementOffsetCount)
{
	if (placementOffsetCount <= 0)
		return placementAttempt;
	if (placementAttempt < 0)
		placementAttempt = 0;
	const int replacementBand =
		HasSkirmishAIRecoveryNativeWorkerRecycleAttempt(
			placementAttempt, placementOffsetCount)
		? 3 * placementOffsetCount :
		(HasSkirmishAIRecoveryObservedReplacement(
			placementAttempt, placementOffsetCount)
		? 2 * placementOffsetCount :
		(HasSkirmishAIRecoveryScaffoldReplacementAttempt(
			placementAttempt, placementOffsetCount)
			? placementOffsetCount : 0));
	return replacementBand +
		((placementAttempt % placementOffsetCount) + 1) % placementOffsetCount;
}

inline bool ShouldSellSkirmishAIRecoveryScaffold(
	bool hasCompatibleBuilder, bool resumeGraceActive,
	bool replacementQueuePaid, bool replacementAttempted,
	bool hasPotentialFactory)
{
	return !resumeGraceActive && !replacementQueuePaid &&
		(replacementAttempted ||
			(hasCompatibleBuilder && !hasPotentialFactory));
}

enum SkirmishAIRecoveryStalledScaffoldAction
{
	SKIRMISH_AI_RECOVERY_SCAFFOLD_KEEP,
	SKIRMISH_AI_RECOVERY_SCAFFOLD_RECYCLE_NATIVE_WORKER,
	SKIRMISH_AI_RECOVERY_SCAFFOLD_RESET_UNUSABLE_NATIVE_WORKER,
	SKIRMISH_AI_RECOVERY_SCAFFOLD_ABANDON_NATIVE_LINEAGE,
	SKIRMISH_AI_RECOVERY_SCAFFOLD_SELL
};

inline SkirmishAIRecoveryStalledScaffoldAction
GetSkirmishAIRecoveryStalledScaffoldAction(
	bool shouldSell, bool hasNativeRebuildHole,
	bool nativeWorkerExists, bool nativeWorkerUsable,
	bool nativeWorkerRecycleAttempted = false)
{
	if (!hasNativeRebuildHole)
		return shouldSell ? SKIRMISH_AI_RECOVERY_SCAFFOLD_SELL :
			SKIRMISH_AI_RECOVERY_SCAFFOLD_KEEP;
	if (!nativeWorkerExists)
		return SKIRMISH_AI_RECOVERY_SCAFFOLD_KEEP;
	if (nativeWorkerRecycleAttempted)
		return SKIRMISH_AI_RECOVERY_SCAFFOLD_ABANDON_NATIVE_LINEAGE;
	return nativeWorkerUsable
		? SKIRMISH_AI_RECOVERY_SCAFFOLD_RECYCLE_NATIVE_WORKER
		: SKIRMISH_AI_RECOVERY_SCAFFOLD_RESET_UNUSABLE_NATIVE_WORKER;
}

inline bool ShouldCancelSkirmishAIRecoveryPaidQueueForNativeWorkerReset(
	SkirmishAIRecoveryStalledScaffoldAction action,
	bool exactIdentityTracked, bool exactEntryExists)
{
	return action ==
			SKIRMISH_AI_RECOVERY_SCAFFOLD_RESET_UNUSABLE_NATIVE_WORKER &&
		exactIdentityTracked && exactEntryExists;
}

inline bool ShouldDetachSkirmishAIRecoveryNativeWorkerWithoutDestroying(
	bool nativeWorkerExists, bool nativeWorkerOwned)
{
	return nativeWorkerExists && !nativeWorkerOwned;
}

inline bool ShouldImmediatelyAbandonSkirmishAINativeLineageAfterCapture(
	bool capturedWorkerDetached, bool recycleAttemptedBeforeDetach)
{
	return capturedWorkerDetached && recycleAttemptedBeforeDetach;
}

struct SkirmishAINativeCapturedWorkerTerminalTransition
{
	bool abandonImmediately;
	bool preserveDetachedWorker;
	bool removeHole;
	bool removeScaffold;
	bool clearRecycleMarker;
	bool spawnReplacementWorker;
};

inline SkirmishAINativeCapturedWorkerTerminalTransition
GetSkirmishAINativeCapturedWorkerTerminalTransition(
	bool capturedWorkerDetached, bool recycleAttemptedBeforeDetach)
{
	SkirmishAINativeCapturedWorkerTerminalTransition result;
	result.abandonImmediately =
		ShouldImmediatelyAbandonSkirmishAINativeLineageAfterCapture(
			capturedWorkerDetached, recycleAttemptedBeforeDetach);
	result.preserveDetachedWorker = result.abandonImmediately;
	result.removeHole = result.abandonImmediately;
	result.removeScaffold = result.abandonImmediately;
	result.clearRecycleMarker = result.abandonImmediately;
	result.spawnReplacementWorker = false;
	return result;
}

inline const ThingTemplate *GetSkirmishAIRecoveryExactNativeRebuildTemplate(
	const ThingTemplate *nativeRebuildTemplate,
	const ThingTemplate *equivalentPrimaryTemplate)
{
	(void)equivalentPrimaryTemplate;
	return nativeRebuildTemplate;
}

inline bool ShouldClearSkirmishAIRecoveryHoleImposedUnselectable(
	bool detachedWorkerExists, bool workerIsUnselectable,
	bool workerIsContained, bool workerIsMasked,
	bool workerIsGenericallyHeld,
	bool hasIndependentUnselectableOwner)
{
	// DISABLED_HELD is also used by scripts that do not own selection state.
	// Only a concrete containment/masking/module owner may retain this shared bit.
	(void)workerIsGenericallyHeld;
	return detachedWorkerExists && workerIsUnselectable && !workerIsContained &&
		!workerIsMasked && !hasIndependentUnselectableOwner;
}

inline int GetSkirmishAIRecoveryNativeWorkerIDAfterOwnershipDetach(
	bool detachedWithoutDestroying, int nativeWorkerID, int invalidObjectID)
{
	return detachedWithoutDestroying ? invalidObjectID : nativeWorkerID;
}

inline int GetSkirmishAIRecoveryReserveCost(
	const SkirmishAIRecoveryPolicyInput &input)
{
	if (!input.enabled || !input.everCompleted ||
		input.hasPrimaryCommandCenter || input.hasConstruction ||
		input.noBuilderPath)
		return 0;

	int required = input.commandCenterCost;
	if (!input.hasBuilder && !input.builderQueuePaid)
		required = AddSkirmishAIRecoveryCost(required, input.builderCost);
	return required > input.protectedReserve ? required :
		(input.protectedReserve > 0 ? input.protectedReserve : 0);
}

inline SkirmishAIRecoveryPolicyResult DecideSkirmishAIRecovery(
	const SkirmishAIRecoveryPolicyInput &input)
{
	SkirmishAIRecoveryPolicyResult result;
	result.shouldQueueBuilder = false;
	result.shouldConstructCommandCenter = false;
	result.recoveryImpossible = false;
	result.shouldRetry = false;
	result.reserveCost = GetSkirmishAIRecoveryReserveCost(input);

	if (!input.enabled || !input.everCompleted ||
		input.hasPrimaryCommandCenter || input.hasConstruction)
		return result;

	if (input.noBuilderPath && !input.hasBuilder && !input.builderQueuePaid)
	{
		result.recoveryImpossible = true;
		return result;
	}

	if (!input.hasBuilder && !input.builderQueuePaid)
	{
		// Queue a paid unit, reusing any existing unpaid work order. Merely
		// waiting on that request would let its own reserve starve it forever.
		if (input.hasBuilderFactory && input.builderAffordable)
		{
			result.shouldQueueBuilder = true;
		}
		else
		{
			// Cash, queue, disabled-factory, and placement failures are
			// retryable.  The caller supplies the bounded retry timer.
			result.shouldRetry = true;
		}
		return result;
	}

	if (input.hasBuilder && input.commandCenterAffordable && input.placementReady)
	{
		result.shouldConstructCommandCenter = true;
		return result;
	}

	result.shouldRetry = true;
	return result;
}

inline bool IsSkirmishAIRecoveryGameMode(int gameMode)
{
	// GameMode values are declared in GameLogic.h: LAN=1, SKIRMISH=2,
	// INTERNET=5.  Keep the policy header independent of engine globals.
	return gameMode == 1 || gameMode == 2 || gameMode == 5;
}

// A zero deadline is the unset sentinel and therefore means the retry is due
// immediately.  Keep this check separate from HasSkirmishAIFrameArrived so a
// legitimate frame value at or beyond the signed half-wrap is handled safely.
inline bool IsSkirmishAIRecoveryRetryDue(
	unsigned int currentFrame, unsigned int retryFrame)
{
	return retryFrame == 0 ||
		(int)(currentFrame - retryFrame) >= 0;
}

inline bool IsSkirmishAIRecoveryBoundedGraceExpired(
	unsigned int currentFrame, unsigned int deadline)
{
	return deadline != 0 &&
		IsSkirmishAIRecoveryRetryDue(currentFrame, deadline);
}

inline bool ShouldReleaseSkirmishAIRecoveryReserveForExpiredGrace(
	bool boundedGraceExpired, bool deadlineIsPendingWakeup)
{
	return boundedGraceExpired && deadlineIsPendingWakeup;
}

inline bool ShouldClearSkirmishAIRecoveryDeadlineForProgressingRoute(
	bool paidQueueProgressing, bool factoryPotential)
{
	return paidQueueProgressing || factoryPotential;
}

inline bool ShouldDeferSkirmishAIRecoveryStalledDisposition(
	bool paidQueueProgressing, bool factoryPotential,
	bool replacementAttempted,
	bool hasPresentUnusableNativeWorker)
{
	// Progressing paid production always defers. Bare factory potential is a
	// retryable route only while the one paid replacement budget remains. A
	// present unusable exact native worker still takes its stronger bounded reset.
	return paidQueueProgressing ||
		(factoryPotential && !replacementAttempted &&
		 !hasPresentUnusableNativeWorker);
}

// Never produce zero for a scheduled deadline: zero is reserved for the
// unset/due sentinel.  Unsigned subtraction in the due check keeps ordinary
// short delays correct across the 32-bit frame wrap.
inline unsigned int GetSkirmishAIRecoveryRetryFrame(
	unsigned int currentFrame, unsigned int delayFrames)
{
	unsigned int retryFrame = currentFrame + delayFrames;
	if (retryFrame == 0)
		retryFrame = 1;
	return retryFrame;
}

inline unsigned int GetSkirmishAIRecoveryEvacuationDeadline(
	unsigned int currentFrame, unsigned int existingDeadline,
	bool hasContainedBuilder, unsigned int graceFrames)
{
	if (!hasContainedBuilder)
		return 0;
	return existingDeadline != 0 ? existingDeadline :
		GetSkirmishAIRecoveryRetryFrame(currentFrame, graceFrames);
}

inline bool IsSkirmishAIRecoveryEvacuationGraceActive(
	unsigned int currentFrame, unsigned int deadline,
	bool hasContainedBuilder)
{
	return hasContainedBuilder && deadline != 0 &&
		!IsSkirmishAIRecoveryRetryDue(currentFrame, deadline);
}

inline unsigned int GetSkirmishAIRecoveryEvacuationDeadlineForVersion(
	int version, unsigned int storedDeadline)
{
	return version >= 4 ? storedDeadline : 0;
}

struct SkirmishAIRecoveryProductionIdentity
{
	int factoryID;
	int productionID;
};

inline SkirmishAIRecoveryProductionIdentity
GetSkirmishAIRecoveryProductionIdentityForVersion(
	int version, int storedFactoryID, int storedProductionID,
	int invalidFactoryID, int invalidProductionID)
{
	SkirmishAIRecoveryProductionIdentity identity;
	identity.factoryID = version >= 5 ? storedFactoryID : invalidFactoryID;
	identity.productionID = version >= 5 ? storedProductionID : invalidProductionID;
	return identity;
}

inline bool GetSkirmishAIRecoveryProductionCancellationOwnershipForVersion(
	int version, bool storedOwnership)
{
	return version >= 6 ? storedOwnership : false;
}

inline bool IsSkirmishAIRecoveryProductionIdentityTracked(
	int factoryID, int productionID,
	int invalidFactoryID, int invalidProductionID)
{
	return factoryID != invalidFactoryID && productionID != invalidProductionID;
}

inline bool IsSkirmishAIRecoveryProductionIdentityMatch(
	int trackedFactoryID, int trackedProductionID,
	int currentFactoryID, int currentProductionID,
	int invalidFactoryID, int invalidProductionID)
{
	return IsSkirmishAIRecoveryProductionIdentityTracked(
		trackedFactoryID, trackedProductionID,
		invalidFactoryID, invalidProductionID) &&
		trackedFactoryID == currentFactoryID &&
		trackedProductionID == currentProductionID;
}

inline bool ShouldAdoptSkirmishAIRecoveryProduction(
	bool identityTracked, bool newlyObserved, bool currentEntryCompatible)
{
	return !identityTracked && newlyObserved && currentEntryCompatible;
}

inline bool ShouldBindSkirmishAIRecoveryProductionIdentity(
	bool recoveryActive, bool hasCompletedPrimaryCenter,
	bool replacementAttempted, bool replacementObserved,
	bool identityTracked, bool paidQueueExists,
	bool hasFactoryID, bool hasProductionID)
{
	return recoveryActive && !hasCompletedPrimaryCenter &&
		replacementAttempted && !replacementObserved &&
		!identityTracked && paidQueueExists &&
		hasFactoryID && hasProductionID;
}

inline bool ShouldClearSkirmishAIRecoveryProductionIdentity(
	bool hasLiveProducer, bool hasExactEntry)
{
	return !hasLiveProducer || !hasExactEntry;
}
