#include "AudioDevice/AudioManagerFactory.h"

#include "AudioDevice/NullAudioManager.h"
#include "XAudio2AudioDevice/XAudio2AudioManager.h"

namespace AudioManagerFactory
{
	AudioManager *create(Bool dummy)
	{
		if (dummy) {
			return NEW NullAudioManager;
		}
		return NEW XAudio2AudioManager;
	}
}
