#include "AudioDevice/AudioAssetSource.h"
#include "XAudio2AudioDevice/XAudio2AudioManager.h"
#include "XAudio2AudioDevice/XAudio2AudioService.h"

void nativeXAudioManagerContractCompile()
{
	XAudio2AudioService *service = nullptr;
	XAudio2AudioCompletion completion;
	(void)service->tryPopCompletion(completion);
	(void)service->setVoiceVolume({}, 0.5f);
	(void)service->pauseVoice({});
	(void)service->resumeVoice({});
	(void)service->stopVoice({});
	AudioAssetSource *assets = nullptr;
	XAudio2AudioManager manager(service, assets);
	manager.setAssetSource(assets);
	manager.setService(service);
}
