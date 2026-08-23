/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2025 Electronic Arts Inc.
*/

#pragma once

namespace rts
{
	unsigned int GetAdaptive3DChannelTarget(unsigned int configuredCount);
	bool ShouldGrow3DChannelPool(
		unsigned int availableCount,
		unsigned int allocatedCount,
		unsigned int requestedCount,
		unsigned int reservedCount);

	bool CanReplace3DChannel(
		bool incomingInterrupt,
		int incomingPriority,
		int victimPriority,
		bool victimCritical,
		bool victimVoice,
		bool victimUI,
		bool victimGlobal,
		bool victimLooping);

	bool IsPreferred3DChannelReplacement(int candidatePriority, int currentPriority);
}
