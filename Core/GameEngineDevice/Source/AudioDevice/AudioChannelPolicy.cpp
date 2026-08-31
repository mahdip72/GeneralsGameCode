#include "AudioDevice/AudioChannelPolicy.h"

namespace rts
{
	unsigned int GetAdaptive3DChannelTarget(unsigned int configuredCount)
	{
		const unsigned int recommendedChannelCount = 64;
		return configuredCount < recommendedChannelCount ? recommendedChannelCount : configuredCount;
	}

	bool ShouldGrow3DChannelPool(
		unsigned int availableCount,
		unsigned int allocatedCount,
		unsigned int requestedCount,
		unsigned int reservedCount)
	{
		return availableCount == 0 && allocatedCount < requestedCount
			&& reservedCount < requestedCount - allocatedCount;
	}

	bool CanReplace3DChannel(
		bool incomingInterrupt,
		int incomingPriority,
		int victimPriority,
		bool victimCritical,
		bool victimVoice,
		bool victimUI,
		bool victimGlobal,
		bool victimLooping)
	{
		if (victimCritical || victimVoice || victimUI || victimGlobal || victimLooping)
		{
			return false;
		}

		if (victimPriority < incomingPriority)
		{
			return true;
		}

		return incomingInterrupt && victimPriority == incomingPriority;
	}

	bool IsPreferred3DChannelReplacement(int candidatePriority, int currentPriority)
	{
		return candidatePriority < currentPriority;
	}
}
