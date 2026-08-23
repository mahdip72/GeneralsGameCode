#include <Utility/CppMacros.h>
#include "Lib/BaseType.h"

#include "AudioDevice/AudioManagerFactory.h"
#include "AudioDevice/NullAudioManager.h"

#include "Common/AudioAffect.h"
#include "Common/AudioEventInfo.h"
#include "Common/AudioHandleSpecialValues.h"
#include "Common/GameAudio.h"
#include "XAudio2AudioDevice/IXAudio2AudioEngineBackend.h"
#include "XAudio2AudioDevice/XAudio2AudioManager.h"
#include "XAudio2AudioDevice/XAudio2AudioService.h"
#include "XAudio2AudioDevice/XAudio2PcmVoice.h"

#include <cstdio>
#include <memory>
#include <type_traits>
#include <vector>

static_assert(std::is_base_of<AudioManager, NullAudioManager>::value,
	"NullAudioManager must implement the common AudioManager contract");
static_assert(!std::is_base_of<LegacyVideoAudioInterface, NullAudioManager>::value,
	"NullAudioManager must not inherit the legacy video audio contract");

namespace
{
	int failures = 0;

	void check(bool condition, const char *message)
	{
		if (!condition) {
			std::fprintf(stderr, "FAIL: %s\n", message);
			++failures;
		}
	}

	class FakeVoice final : public IXAudio2PcmVoiceBackend
	{
	public:
		HRESULT create(const WAVEFORMATEX &, IXAudio2VoiceCallback *callback) noexcept override
		{
			m_callback = callback;
			return S_OK;
		}
		HRESULT submit(const XAUDIO2_BUFFER &buffer) noexcept override
		{
			m_lastContext = buffer.pContext;
			++submitCalls;
			return S_OK;
		}
		HRESULT start() noexcept override { return S_OK; }
		HRESULT stop() noexcept override { return S_OK; }
		HRESULT flush() noexcept override { return S_OK; }
		HRESULT setVolume(float) noexcept override { return S_OK; }
		HRESULT pause() noexcept override { return S_OK; }
		HRESULT resume() noexcept override { return S_OK; }
		HRESULT getCriticalError() const noexcept override { return S_OK; }
		void destroy() noexcept override { m_callback = nullptr; }
		Bool completeLastBuffer() noexcept
		{
			if (m_callback == nullptr || m_lastContext == nullptr) {
				return FALSE;
			}
			void *context = m_lastContext;
			m_lastContext = nullptr;
			m_callback->OnBufferEnd(context);
			return TRUE;
		}
		int submitCalls = 0;

	private:
		IXAudio2VoiceCallback *m_callback = nullptr;
		void *m_lastContext = nullptr;
	};

	class FakeEngine final : public IXAudio2AudioEngineBackend
	{
	public:
		HRESULT open(CriticalErrorCallback, void *) noexcept override { return S_OK; }
		HRESULT start() noexcept override { return S_OK; }
		HRESULT createPcmVoice(std::unique_ptr<IXAudio2PcmVoiceBackend> &voice) noexcept override
		{
			std::unique_ptr<FakeVoice> created = std::make_unique<FakeVoice>();
			lastVoice = created.get();
			voices.push_back(lastVoice);
			voice = std::move(created);
			return S_OK;
		}
		HRESULT stop() noexcept override { return S_OK; }
		HRESULT close() noexcept override { return S_OK; }
		FakeVoice *lastVoice = nullptr;
		std::vector<FakeVoice *> voices;
	};

	class FixtureEvent final : public AudioEventRTS
	{
	public:
		explicit FixtureEvent(const AsciiString &name) : AudioEventRTS(name) {}
		void setDelayForTest(Real delay) { m_delay = delay; }
	};
}

int main()
{
	AudioManager *dummy = AudioManagerFactory::create(true);
	check(dummy != nullptr && dummy->getDevice() == nullptr,
		"factory dummy remains device-free");
	delete dummy;
	AudioManager *native = AudioManagerFactory::create(false);
	XAudio2AudioManager *nativeManager = dynamic_cast<XAudio2AudioManager *>(native);
	check(nativeManager != nullptr && nativeManager->getAssetSource() != nullptr,
		"x64 factory wires a neutral asset catalog into the native manager");
	check(nativeManager != nullptr
		&& nativeManager->getFileLengthMS(AsciiString("attack.wav")) == 100.0f,
		"factory-created native manager exposes exact neutral script duration");
	delete native;

	NullAudioManager nullManager;
	check(nullManager.getDevice() == nullptr, "NullAudioManager has no device");
	check(nullManager.getFileLengthMS(AsciiString("attack.wav")) == 100.0f,
		"NullAudioManager uses the neutral catalog duration adapter");
	nullManager.update();
	nullManager.reset();

	std::unique_ptr<FakeEngine> ownedEngine = std::make_unique<FakeEngine>();
	FakeEngine *engine = ownedEngine.get();
	XAudio2AudioService service(std::move(ownedEngine));
	AudioAssetCatalog catalog;
	XAudio2AudioManager manager(&service, &catalog);
	manager.openDevice();
	check(manager.isOpen(), "manager opens the injected device-free service");
	check(manager.runInjectedPlaybackProbe(AsciiString("main.wav")),
		"manager submits an injected phase through its generation-reset path");
	check(engine->lastVoice != nullptr && engine->lastVoice->submitCalls == 1,
		"fake service observes one bounded manager PCM submission");

	AudioEventInfo *fixtureInfo = newInstance(AudioEventInfo);
	fixtureInfo->m_soundType = AT_SoundEffect;
	fixtureInfo->m_volume = 1.0f;
	fixtureInfo->m_volumeShift = 1.0f;
	fixtureInfo->m_minVolume = 0.0f;
	fixtureInfo->m_limit = 4;
	fixtureInfo->m_priority = AP_NORMAL;
	fixtureInfo->m_type = ST_WORLD;
	fixtureInfo->m_control = 0;
	fixtureInfo->m_loopCount = 0;
	fixtureInfo->m_minDistance = 0.0f;
	fixtureInfo->m_maxDistance = 100.0f;
	fixtureInfo->m_sounds.push_back(AsciiString("main.wav"));
	fixtureInfo->m_attackSounds.push_back(AsciiString("attack.wav"));
	fixtureInfo->m_decaySounds.push_back(AsciiString("decay.wav"));

	FixtureEvent delayedEvent(AsciiString("fixture-delayed"));
	delayedEvent.setAudioEventInfo(fixtureInfo);
	delayedEvent.setDelayForTest(1.0f);
	const AudioHandle delayedHandle = manager.addAudioEvent(&delayedEvent);
	check(delayedHandle != AHSV_NoSound && delayedHandle != AHSV_Error,
		"manager admits a real delayed event through common admission semantics");
	manager.update();
	check(manager.getPendingAudioRequestCount() == 1
		&& manager.isCurrentlyPlaying(delayedHandle),
		"positive residual delay remains pending while its handle stays generation-valid");
	manager.pauseAudio(AudioAffect_Sound);
	check(manager.getPendingAudioRequestCount() == 1,
		"pausing one affect preserves its pending request for resume");

	Coord3D positionalPosition;
	positionalPosition.set(10.0f, 0.0f, 0.0f);
	FixtureEvent positionalEvent(AsciiString("fixture-positional"));
	positionalEvent.setAudioEventInfo(fixtureInfo);
	positionalEvent.setPosition(&positionalPosition);
	const AudioHandle positionalHandle = manager.addAudioEvent(&positionalEvent);
	manager.update();
	check(positionalHandle != AHSV_NoSound && manager.getPendingAudioRequestCount() == 1
		&& manager.getActiveAudioCount() == 1,
		"unrelated positional request is not deleted by a sound pause");
	FakeVoice *positionalVoice = engine->lastVoice;
	manager.update();
	manager.pauseAudio(AudioAffect_Sound3D);
	manager.resumeAudio(AudioAffect_Sound);
	manager.update();
	FakeVoice *delayedVoice = engine->lastVoice;
	manager.update();
	check(delayedVoice != nullptr && delayedVoice->submitCalls >= 1
		&& manager.getActiveAudioCount() == 2
		&& manager.isCurrentlyPlaying(delayedHandle),
		"resumed delayed event submits its attack phase while the 3D event stays paused");

	check(positionalVoice != nullptr && positionalVoice->completeLastBuffer(),
		"fake backend publishes completion for a paused positional phase");
	manager.update();
	check(manager.getActiveAudioCount() == 2 && manager.isCurrentlyPlaying(positionalHandle),
		"paused records do not advance phases on completion");
	manager.resumeAudio(AudioAffect_Sound3D);
	manager.update();
	manager.update();
	check(positionalVoice->submitCalls >= 2,
		"resuming the exact affect advances the positional event to its next phase");

	check(delayedVoice->completeLastBuffer(), "fake backend completes the attack phase");
	manager.update();
	manager.update();
	check(delayedVoice->submitCalls >= 2,
		"manager continues the event with its main phase after completion");
	check(delayedVoice->completeLastBuffer(), "fake backend completes the main phase");
	manager.update();
	manager.update();
	check(delayedVoice->submitCalls >= 3,
		"manager continues the event with its decay phase after completion");
	check(delayedVoice->completeLastBuffer(), "fake backend completes the decay phase");
	manager.update();
	check(!manager.isCurrentlyPlaying(delayedHandle) && manager.getActiveAudioCount() == 1,
		"completed handle is no longer reported as playing");

	catalog.setDurationMS(AsciiString("long.wav"), 2500.0f);
	AudioEventInfo *longInfo = newInstance(AudioEventInfo);
	*longInfo = *fixtureInfo;
	longInfo->m_attackSounds.clear();
	longInfo->m_decaySounds.clear();
	longInfo->m_sounds.clear();
	longInfo->m_sounds.push_back(AsciiString("long.wav"));
	FixtureEvent longEvent(AsciiString("fixture-long"));
	longEvent.setAudioEventInfo(longInfo);
	const AudioHandle longHandle = manager.addAudioEvent(&longEvent);
	manager.update();
	manager.update();
	FakeVoice *longVoice = engine->lastVoice;
	check(longVoice != nullptr && longVoice->submitCalls == 1,
		"duration-only long asset starts with one bounded PCM chunk");
	check(longVoice->completeLastBuffer(), "fake backend completes the first long-asset chunk");
	manager.update();
	manager.update();
	check(longVoice->submitCalls >= 2,
		"completion deterministically submits the next long-asset chunk");
	manager.stopAudio(AudioAffect_Sound);
	manager.update();
	check(!manager.isCurrentlyPlaying(longHandle),
		"stopping the long asset clears its generation-aware handle state");
	deleteInstance(longInfo);

	AudioEventInfo *simpleInfo = newInstance(AudioEventInfo);
	*simpleInfo = *fixtureInfo;
	simpleInfo->m_attackSounds.clear();
	simpleInfo->m_decaySounds.clear();
	simpleInfo->m_sounds.clear();
	simpleInfo->m_sounds.push_back(AsciiString("decay.wav"));
	FixtureEvent closeEvent(AsciiString("fixture-close"));
	closeEvent.setAudioEventInfo(simpleInfo);
	const AudioHandle closeHandle = manager.addAudioEvent(&closeEvent);
	manager.update();
	manager.update();
	check(manager.isCurrentlyPlaying(closeHandle),
		"file-close fixture is active before its identity is closed");
	manager.closeAnySamplesUsingFile(catalog.getFileIdentity(AsciiString("decay.wav")));
	manager.update();
	check(!manager.isCurrentlyPlaying(closeHandle) && manager.isCurrentlyPlaying(positionalHandle),
		"closeAnySamplesUsingFile stops only records using the matching identity");
	manager.stopAudio(AudioAffect_Sound);
	manager.update();
	check(manager.isCurrentlyPlaying(positionalHandle),
		"sound stop does not affect the still-playing positional record");
	manager.stopAudio(AudioAffect_Sound3D);
	manager.update();
	check(!manager.isCurrentlyPlaying(positionalHandle) && manager.getActiveAudioCount() == 0,
		"3D stop removes the remaining positional record");
	deleteInstance(simpleInfo);
	deleteInstance(fixtureInfo);

	const UnsignedInt firstGeneration = manager.getLifecycleGeneration();
	manager.reset();
	check(manager.isOpen(), "manager reset reopens its reusable service");
	check(manager.getLifecycleGeneration() != firstGeneration,
		"manager reset advances the lifecycle generation");
	check(manager.runInjectedPlaybackProbe(AsciiString("decay.wav")),
		"manager admits playback after reset");

	const XAudio2PcmVoiceHandle staleHandle = service.createVoice();
	check(staleHandle.isValid(), "service allocates a typed voice for stale-generation test");
	check(service.resetVoice(staleHandle, 41), "service activates the first test generation");
	AudioPcmChunk staleChunk;
	staleChunk.sampleRate = 48000;
	staleChunk.channels = 2;
	staleChunk.frameCount = 1;
	staleChunk.data.assign(4, 0);
	staleChunk.generation = 40;
	check(service.submit(staleHandle, std::move(staleChunk)) == AudioPcmSubmitResult::DROPPED,
		"stale generation is rejected by the typed voice service");
	service.destroyVoice(staleHandle);
	manager.closeDevice();
	check(!manager.isOpen(), "manager close removes playback admission");
	return failures == 0 ? 0 : 1;
}
