#include "AudioDevice/AudioManagerFactory.h"

#include "Common/AudioEventRTS.h"
#include "MilesAudioDevice/MilesAudioManager.h"

namespace AudioManagerFactory
{
	AudioManager *create(Bool dummy)
	{
		if (dummy) {
			return NEW MilesAudioManagerDummy;
		}
		return NEW MilesAudioManager;
	}
}
