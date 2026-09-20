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

#include "Common/UnicodeString.h"

#include <wchar.h>

enum SkirmishAIReplayEpochType
{
	SKIRMISH_AI_REPLAY_EPOCH_LEGACY = 0,
	SKIRMISH_AI_REPLAY_EPOCH_PR6_LIVENESS = 1,
	// Epoch 2 is already used by the PR7-PR9 AI behavior and must remain
	// readable so those recordings continue to replay with their original
	// decisions.
	SKIRMISH_AI_REPLAY_EPOCH_CURRENT = 2,
	SKIRMISH_AI_REPLAY_EPOCH_RECOVERY = 3,
	// Epoch 4 preserves epoch-3 recovery behavior while extending its logic CRC
	// with the grace deadline and exact paid-production provenance.
	SKIRMISH_AI_REPLAY_EPOCH_RECOVERY_CRC = 4,
	// Epoch 5 adds explicit cancellation ownership and consumes replacement
	// budget when deciding whether bare factory potential can defer disposition.
	SKIRMISH_AI_REPLAY_EPOCH_RECOVERY_OWNERSHIP = 5,
	// Epoch 6 lets an independently affordable owned replacement bypass a
	// bounded adopted queue without cancelling or refunding that ordinary work.
	SKIRMISH_AI_REPLAY_EPOCH_NONCANCELLING_FAILOVER = 6,
	// Epoch 7 permits at most one paid-queue failover during a command-center
	// recovery episode, preventing repeated cancel/refund/requeue churn.
	SKIRMISH_AI_REPLAY_EPOCH_BOUNDED_FAILOVER = 7
};

inline const WideChar *GetSkirmishAILivenessReplayMarker()
{
	return L" [SkirmishAILiveness=1]";
}

inline const WideChar *GetSkirmishAICurrentReplayMarker()
{
	return L" [SkirmishAIEpoch=2]";
}

inline const WideChar *GetSkirmishAIRecoveryReplayMarker()
{
	return L" [SkirmishAIEpoch=3]";
}

inline const WideChar *GetSkirmishAIRecoveryCRCReplayMarker()
{
	return L" [SkirmishAIEpoch=4]";
}

inline const WideChar *GetSkirmishAIRecoveryOwnershipReplayMarker()
{
	return L" [SkirmishAIEpoch=5]";
}

inline const WideChar *GetSkirmishAINonCancellingFailoverReplayMarker()
{
	return L" [SkirmishAIEpoch=6]";
}

inline const WideChar *GetSkirmishAIBoundedFailoverReplayMarker()
{
	return L" [SkirmishAIEpoch=7]";
}

inline const WideChar *GetSkirmishAIReplayMarkerPrefix()
{
	return L"[SkirmishAI";
}

inline Int CountSkirmishAIReplayMarkers(const UnicodeString& versionTimeString, const WideChar *marker)
{
	Int count = 0;
	const WideChar *position = versionTimeString.str();
	const size_t markerLength = wcslen(marker);
	while ((position = wcsstr(position, marker)) != NULL) {
		++count;
		position += markerLength;
	}
	return count;
}

inline void MarkReplayVersionForSkirmishAILivenessRecovery(UnicodeString& versionTimeString)
{
	if (CountSkirmishAIReplayMarkers(versionTimeString, GetSkirmishAIReplayMarkerPrefix()) == 0)
		versionTimeString.concat(GetSkirmishAILivenessReplayMarker());
}

inline void MarkReplayVersionForSkirmishAIRecoveryEpoch(UnicodeString& versionTimeString)
{
	if (CountSkirmishAIReplayMarkers(versionTimeString, GetSkirmishAIReplayMarkerPrefix()) == 0)
		versionTimeString.concat(GetSkirmishAIRecoveryReplayMarker());
}

inline void MarkReplayVersionForSkirmishAIRecoveryCRCEpoch(UnicodeString& versionTimeString)
{
	if (CountSkirmishAIReplayMarkers(versionTimeString, GetSkirmishAIReplayMarkerPrefix()) == 0)
		versionTimeString.concat(GetSkirmishAIRecoveryCRCReplayMarker());
}

inline void MarkReplayVersionForSkirmishAIRecoveryOwnershipEpoch(
	UnicodeString& versionTimeString)
{
	if (CountSkirmishAIReplayMarkers(
			versionTimeString, GetSkirmishAIReplayMarkerPrefix()) == 0)
		versionTimeString.concat(GetSkirmishAIRecoveryOwnershipReplayMarker());
}

inline void MarkReplayVersionForSkirmishAINonCancellingFailoverEpoch(
	UnicodeString& versionTimeString)
{
	if (CountSkirmishAIReplayMarkers(
			versionTimeString, GetSkirmishAIReplayMarkerPrefix()) == 0)
		versionTimeString.concat(GetSkirmishAINonCancellingFailoverReplayMarker());
}

inline void MarkReplayVersionForSkirmishAIBoundedFailoverEpoch(
	UnicodeString& versionTimeString)
{
	if (CountSkirmishAIReplayMarkers(
			versionTimeString, GetSkirmishAIReplayMarkerPrefix()) == 0)
		versionTimeString.concat(GetSkirmishAIBoundedFailoverReplayMarker());
}

// Global writer helper. RecorderClass shadows its unchanged member call only
// for playback compatibility checks; this helper keeps new recordings at the
// latest AI behavior and CRC epoch.
inline void MarkReplayVersionForSkirmishAICurrentEpoch(UnicodeString& versionTimeString)
{
	MarkReplayVersionForSkirmishAIBoundedFailoverEpoch(versionTimeString);
}

// Compatibility-only stamp for the already-shipped epoch-2 behavior. The
// RecorderClass playback allow-list uses this on a freshly constructed build
// time string; the global current-epoch writer above intentionally remains the
	// epoch-6 stamp for newly recorded replays.
inline void MarkReplayVersionForSkirmishAICurrentCompatibilityEpoch(UnicodeString& versionTimeString)
{
	if (CountSkirmishAIReplayMarkers(versionTimeString, GetSkirmishAIReplayMarkerPrefix()) == 0)
		versionTimeString.concat(GetSkirmishAICurrentReplayMarker());
}

inline Int GetSkirmishAIReplayEpoch(const UnicodeString& versionTimeString)
{
	Int livenessMarkerCount = CountSkirmishAIReplayMarkers(versionTimeString, GetSkirmishAILivenessReplayMarker());
	Int currentMarkerCount = CountSkirmishAIReplayMarkers(versionTimeString, GetSkirmishAICurrentReplayMarker());
	Int recoveryMarkerCount = CountSkirmishAIReplayMarkers(versionTimeString, GetSkirmishAIRecoveryReplayMarker());
	Int recoveryCRCMarkerCount = CountSkirmishAIReplayMarkers(versionTimeString, GetSkirmishAIRecoveryCRCReplayMarker());
	Int recoveryOwnershipMarkerCount = CountSkirmishAIReplayMarkers(
		versionTimeString, GetSkirmishAIRecoveryOwnershipReplayMarker());
	Int nonCancellingFailoverMarkerCount = CountSkirmishAIReplayMarkers(
		versionTimeString, GetSkirmishAINonCancellingFailoverReplayMarker());
	Int boundedFailoverMarkerCount = CountSkirmishAIReplayMarkers(
		versionTimeString, GetSkirmishAIBoundedFailoverReplayMarker());
	Int markerLikeCount = CountSkirmishAIReplayMarkers(versionTimeString, GetSkirmishAIReplayMarkerPrefix());
	if (markerLikeCount != 1 ||
		livenessMarkerCount + currentMarkerCount + recoveryMarkerCount +
			recoveryCRCMarkerCount + recoveryOwnershipMarkerCount +
			nonCancellingFailoverMarkerCount + boundedFailoverMarkerCount != 1)
		return SKIRMISH_AI_REPLAY_EPOCH_LEGACY;
	if (boundedFailoverMarkerCount == 1 &&
		versionTimeString.endsWith(GetSkirmishAIBoundedFailoverReplayMarker()))
		return SKIRMISH_AI_REPLAY_EPOCH_BOUNDED_FAILOVER;
	if (nonCancellingFailoverMarkerCount == 1 &&
		versionTimeString.endsWith(GetSkirmishAINonCancellingFailoverReplayMarker()))
		return SKIRMISH_AI_REPLAY_EPOCH_NONCANCELLING_FAILOVER;
	if (recoveryOwnershipMarkerCount == 1 &&
		versionTimeString.endsWith(GetSkirmishAIRecoveryOwnershipReplayMarker()))
		return SKIRMISH_AI_REPLAY_EPOCH_RECOVERY_OWNERSHIP;
	if (recoveryCRCMarkerCount == 1 && versionTimeString.endsWith(GetSkirmishAIRecoveryCRCReplayMarker()))
		return SKIRMISH_AI_REPLAY_EPOCH_RECOVERY_CRC;
	if (recoveryMarkerCount == 1 && versionTimeString.endsWith(GetSkirmishAIRecoveryReplayMarker()))
		return SKIRMISH_AI_REPLAY_EPOCH_RECOVERY;
	if (currentMarkerCount == 1 && versionTimeString.endsWith(GetSkirmishAICurrentReplayMarker()))
		return SKIRMISH_AI_REPLAY_EPOCH_CURRENT;
	if (livenessMarkerCount == 1 && versionTimeString.endsWith(GetSkirmishAILivenessReplayMarker()))
		return SKIRMISH_AI_REPLAY_EPOCH_PR6_LIVENESS;
	return SKIRMISH_AI_REPLAY_EPOCH_LEGACY;
}

inline Bool ReplayVersionUsesSkirmishAILivenessRecovery(const UnicodeString& versionTimeString)
{
	return GetSkirmishAIReplayEpoch(versionTimeString) >= SKIRMISH_AI_REPLAY_EPOCH_PR6_LIVENESS;
}

inline Bool ShouldUseSkirmishAICurrentBehavior(Bool isReplayGame, Int replayEpoch)
{
	return !isReplayGame ||
		replayEpoch == SKIRMISH_AI_REPLAY_EPOCH_CURRENT ||
		replayEpoch == SKIRMISH_AI_REPLAY_EPOCH_RECOVERY ||
		replayEpoch == SKIRMISH_AI_REPLAY_EPOCH_RECOVERY_CRC ||
		replayEpoch == SKIRMISH_AI_REPLAY_EPOCH_RECOVERY_OWNERSHIP ||
		replayEpoch == SKIRMISH_AI_REPLAY_EPOCH_NONCANCELLING_FAILOVER ||
		replayEpoch == SKIRMISH_AI_REPLAY_EPOCH_BOUNDED_FAILOVER;
}

inline Bool ShouldUseSkirmishAIRecoveryBehavior(Bool isReplayGame, Int replayEpoch)
{
	return !isReplayGame ||
		replayEpoch == SKIRMISH_AI_REPLAY_EPOCH_RECOVERY ||
		replayEpoch == SKIRMISH_AI_REPLAY_EPOCH_RECOVERY_CRC ||
		replayEpoch == SKIRMISH_AI_REPLAY_EPOCH_RECOVERY_OWNERSHIP ||
		replayEpoch == SKIRMISH_AI_REPLAY_EPOCH_NONCANCELLING_FAILOVER ||
		replayEpoch == SKIRMISH_AI_REPLAY_EPOCH_BOUNDED_FAILOVER;
}

inline Bool ShouldUseSkirmishAIRecoveryNativeHoleOwnership(Bool isReplayGame, Int replayEpoch)
{
	return !isReplayGame ||
		replayEpoch == SKIRMISH_AI_REPLAY_EPOCH_RECOVERY_CRC ||
		replayEpoch == SKIRMISH_AI_REPLAY_EPOCH_RECOVERY_OWNERSHIP ||
		replayEpoch == SKIRMISH_AI_REPLAY_EPOCH_NONCANCELLING_FAILOVER ||
		replayEpoch == SKIRMISH_AI_REPLAY_EPOCH_BOUNDED_FAILOVER;
}

inline Bool ShouldIncludeSkirmishAIRecoveryCRCFields(Bool isReplayGame, Int replayEpoch)
{
	return ShouldUseSkirmishAIRecoveryNativeHoleOwnership(isReplayGame, replayEpoch);
}

inline Bool ShouldUseSkirmishAIRecoveryCancellationOwnership(
	Bool isReplayGame, Int replayEpoch)
{
	return !isReplayGame ||
		replayEpoch == SKIRMISH_AI_REPLAY_EPOCH_RECOVERY_OWNERSHIP ||
		replayEpoch == SKIRMISH_AI_REPLAY_EPOCH_NONCANCELLING_FAILOVER ||
		replayEpoch == SKIRMISH_AI_REPLAY_EPOCH_BOUNDED_FAILOVER;
}

inline Bool ShouldUseSkirmishAIRecoveryUnownedQueueFailover(
	Bool isReplayGame, Int replayEpoch)
{
	return !isReplayGame ||
		replayEpoch == SKIRMISH_AI_REPLAY_EPOCH_NONCANCELLING_FAILOVER ||
		replayEpoch == SKIRMISH_AI_REPLAY_EPOCH_BOUNDED_FAILOVER;
}

inline Bool ShouldUseSkirmishAIRecoveryBoundedFailover(
	Bool isReplayGame, Int replayEpoch)
{
	return !isReplayGame ||
		replayEpoch == SKIRMISH_AI_REPLAY_EPOCH_BOUNDED_FAILOVER;
}

inline Bool ShouldIncludeSkirmishAIRecoveryCancellationOwnershipCRCField(
	Bool isReplayGame, Int replayEpoch)
{
	return ShouldUseSkirmishAIRecoveryCancellationOwnership(
		isReplayGame, replayEpoch);
}

inline Bool ShouldIncludeSkirmishAIRecoveryFailoverConsumedCRCField(
	Bool isReplayGame, Int replayEpoch)
{
	return ShouldUseSkirmishAIRecoveryBoundedFailover(
		isReplayGame, replayEpoch);
}
