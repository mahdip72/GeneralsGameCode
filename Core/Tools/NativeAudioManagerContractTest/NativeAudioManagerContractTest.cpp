#include <Utility/CppMacros.h>
#include "Lib/BaseType.h"

#include "AudioDevice/AudioManagerFactory.h"
#include "AudioDevice/NullAudioManager.h"

#include "Common/GameAudio.h"

#include <type_traits>

static_assert(std::is_base_of<AudioManager, NullAudioManager>::value,
	"NullAudioManager must implement the common AudioManager contract");
static_assert(!std::is_base_of<LegacyVideoAudioInterface, NullAudioManager>::value,
	"NullAudioManager must not inherit the legacy video audio contract");

int main()
{
	AudioManager *dummy = AudioManagerFactory::create(true);
	if (dummy == nullptr || dummy->getDevice() != nullptr) {
		delete dummy;
		return 1;
	}
	delete dummy;

	NullAudioManager manager;
	if (manager.getDevice() != nullptr) {
		return 1;
	}
	manager.update();
	manager.reset();
	manager.closeDevice();
	return 0;
}
