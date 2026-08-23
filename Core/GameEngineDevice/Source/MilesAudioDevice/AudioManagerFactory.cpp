#include "AudioDevice/AudioManagerFactory.h"

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
