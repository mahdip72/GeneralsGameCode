/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2025 Electronic Arts Inc.
**
** This program is free software: you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation, either version 3 of the License, or
** (at your option) any later version.
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
