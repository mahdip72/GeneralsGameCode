#include <Utility/CppMacros.h>
#include "Lib/BaseType.h"

#include "AudioDevice/AudioManagerFactory.h"
#include "AudioDevice/NullAudioManager.h"

#include "Common/AudioAffect.h"
#include "Common/AudioEventInfo.h"
#include "Common/AudioHandleSpecialValues.h"
#include "Common/AudioSettings.h"
#include "Common/GameAudio.h"
#include "XAudio2AudioDevice/IXAudio2AudioEngineBackend.h"
#include "XAudio2AudioDevice/XAudio2AudioManager.h"
#include "XAudio2AudioDevice/XAudio2AudioService.h"
#include "XAudio2AudioDevice/XAudio2PcmVoice.h"

#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <type_traits>
#include <vector>

class View;
extern View *TheTacticalView;
extern int g_nativeAudioBaseUpdateCalls;

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

	void writeWaveFile(const std::filesystem::path &path, UnsignedInt durationMS)
	{
		const UnsignedInt sampleRate = 48000U;
		const UnsignedShort channels = 2U;
		const UnsignedInt bytesPerFrame = channels * sizeof(Short);
		const UnsignedInt frames = durationMS * sampleRate / 1000U;
		std::ofstream output(path, std::ios::binary | std::ios::trunc);
		check(output.good(), "manager real fixture opens for writing");
		auto writeU16 = [&output](UnsignedShort value) {
			output.put(static_cast<char>(value & 0xffU));
			output.put(static_cast<char>((value >> 8U) & 0xffU));
		};
		auto writeU32 = [&output](UnsignedInt value) {
			for (UnsignedInt shift = 0; shift < 32U; shift += 8U) {
				output.put(static_cast<char>((value >> shift) & 0xffU));
			}
		};
		output.write("RIFF", 4);
		writeU32(36U + frames * bytesPerFrame);
		output.write("WAVEfmt ", 8);
		writeU32(16U);
		writeU16(1U);
		writeU16(channels);
		writeU32(sampleRate);
		writeU32(sampleRate * bytesPerFrame);
		writeU16(static_cast<UnsignedShort>(bytesPerFrame));
		writeU16(16U);
		output.write("data", 4);
		writeU32(frames * bytesPerFrame);
		for (UnsignedInt frame = 0; frame < frames; ++frame) {
			writeU16(static_cast<UnsignedShort>(frame & 0x7fffU));
			writeU16(static_cast<UnsignedShort>((frame + 1U) & 0x7fffU));
		}
		check(output.good(), "manager real fixture is fully written");
	}

	class FakeVoice final : public IXAudio2PcmVoiceBackend
	{
	public:
		FakeVoice(Bool failCreate, Bool failSubmit, Bool failStop, Bool failFlush) :
			m_failCreate(failCreate),
			m_failSubmit(failSubmit),
			m_failStop(failStop),
			m_failFlush(failFlush)
		{
		}
		HRESULT create(const WAVEFORMATEX &, IXAudio2VoiceCallback *callback) noexcept override
		{
			if (m_failCreate) {
				return E_FAIL;
			}
			m_callback = callback;
			return S_OK;
		}
		HRESULT submit(const XAUDIO2_BUFFER &buffer) noexcept override
		{
			if (m_failSubmit) {
				return E_FAIL;
			}
			m_lastContext = buffer.pContext;
			++submitCalls;
			return S_OK;
		}
		HRESULT start() noexcept override { return S_OK; }
		HRESULT stop() noexcept override { return m_failStop ? E_FAIL : S_OK; }
		HRESULT flush() noexcept override { return m_failFlush ? E_FAIL : S_OK; }
		HRESULT setVolume(float volume) noexcept override
		{
			lastVolume = volume;
			++volumeCalls;
			return S_OK;
		}
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
		float lastVolume = 0.0f;
		int volumeCalls = 0;

	private:
		Bool m_failCreate;
		Bool m_failSubmit;
		Bool m_failStop;
		Bool m_failFlush;
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
			if (failCreateVoice) {
				return E_FAIL;
			}
			std::unique_ptr<FakeVoice> created = std::make_unique<FakeVoice>(
				failVoiceCreate, failSubmit, failStop, failFlush);
			lastVoice = created.get();
			voices.push_back(lastVoice);
			voice = std::move(created);
			return S_OK;
		}
		HRESULT stop() noexcept override { return S_OK; }
		HRESULT close() noexcept override { return S_OK; }
		FakeVoice *lastVoice = nullptr;
		std::vector<FakeVoice *> voices;
		Bool failCreateVoice = FALSE;
		Bool failVoiceCreate = FALSE;
		Bool failSubmit = FALSE;
		Bool failStop = FALSE;
		Bool failFlush = FALSE;
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
	const std::filesystem::path realRoot = std::filesystem::temp_directory_path()
		/ "rts-native-audio-manager-real-source";
	std::filesystem::create_directories(realRoot);
	const std::filesystem::path realAttack = realRoot / "attack.wav";
	const std::filesystem::path realMain = realRoot / "main.wav";
	const std::filesystem::path realDecay = realRoot / "decay.wav";
	writeWaveFile(realAttack, 100U);
	writeWaveFile(realMain, 400U);
	writeWaveFile(realDecay, 50U);

	AudioManager *dummy = AudioManagerFactory::create(true);
	check(dummy != nullptr && dummy->getDevice() == nullptr,
		"factory dummy remains device-free");
	delete dummy;
	AudioManager *native = AudioManagerFactory::create(false);
	XAudio2AudioManager *nativeManager = dynamic_cast<XAudio2AudioManager *>(native);
	check(nativeManager != nullptr && nativeManager->getAssetSource() != nullptr,
		"x64 factory wires a neutral production asset source into the native manager");
	check(nativeManager != nullptr
		&& dynamic_cast<FileAudioAssetSource *>(nativeManager->getAssetSource()) != nullptr,
		"x64 factory selects the filesystem/container decoder rather than a synthetic catalog");
	check(nativeManager != nullptr
		&& nativeManager->getFileLengthMS(AsciiString(realAttack.string().c_str())) == 100.0f,
		"factory-created native manager exposes exact real-file script duration");
	delete native;

	NullAudioManager nullManager;
	check(nullManager.getDevice() == nullptr, "NullAudioManager has no device");
	check(dynamic_cast<FileAudioAssetSource *>(nullManager.getAssetSource()) != nullptr,
		"NullAudioManager uses the same production-neutral asset source");
	check(nullManager.getFileLengthMS(AsciiString(realAttack.string().c_str())) == 100.0f,
		"NullAudioManager exposes exact real-file script duration");
	nullManager.update();
	nullManager.reset();

	std::unique_ptr<FakeEngine> ownedEngine = std::make_unique<FakeEngine>();
	FakeEngine *engine = ownedEngine.get();
	XAudio2AudioService service(std::move(ownedEngine));
	AudioAssetCatalog catalog;
	catalog.setDurationMS(AsciiString("attack.wav"), 100.0f);
	catalog.setDurationMS(AsciiString("main.wav"), 400.0f);
	catalog.setDurationMS(AsciiString("decay.wav"), 50.0f);
	catalog.setDurationMS(AsciiString("long.wav"), 2500.0f);
	XAudio2AudioManager manager(&service, &catalog);
	manager.openDevice();
	check(manager.isOpen(), "manager opens the injected device-free service");
	TheTacticalView = reinterpret_cast<View *>(static_cast<std::uintptr_t>(1));
	g_nativeAudioBaseUpdateCalls = 0;
	manager.update();
	check(g_nativeAudioBaseUpdateCalls == 1 && manager.getZoomVolume() == 0.5f,
		"manager update runs the base listener/zoom phase exactly once after service work");
	TheTacticalView = nullptr;
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

	AudioSettings admissionSettings;
	admissionSettings.m_minVolume = 0.5f;
	manager.setAudioSettingsForTest(&admissionSettings);
	manager.setAudioEventVolumeOverride(AsciiString("admission-muted"), 0.25f);
	AudioEventInfo *admissionInfo = newInstance(AudioEventInfo);
	*admissionInfo = *fixtureInfo;
	admissionInfo->m_audioName = AsciiString("admission-muted");
	FixtureEvent admissionEvent(AsciiString("admission-muted"));
	admissionEvent.setAudioEventInfo(admissionInfo);
	check(manager.addAudioEvent(&admissionEvent) == AHSV_Muted,
		"common admission rejects adjusted events below the configured minimum volume");
	manager.setAudioEventVolumeOverride(AsciiString("admission-muted"), -1.0f);
	manager.setOn(FALSE, AudioAffect_Sound);
	FixtureEvent disabledEvent(AsciiString("admission-disabled"));
	disabledEvent.setAudioEventInfo(fixtureInfo);
	check(manager.addAudioEvent(&disabledEvent) == AHSV_NoSound,
		"common admission honors the sound affect switch before native queueing");
	manager.setOn(TRUE, AudioAffect_Sound);
	manager.setAudioSettingsForTest(nullptr);
	deleteInstance(admissionInfo);

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
	check(manager.getPendingAudioRequestCount() == 1 && manager.isCurrentlyPlaying(delayedHandle),
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
	check(delayedEvent.getPlayingAudioIndex() >= 0,
		"common admission updates the caller event's selected sound index");
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

	const int pausedSubmitCalls = positionalVoice == nullptr ? 0 : positionalVoice->submitCalls;
	check(positionalVoice != nullptr && positionalVoice->completeLastBuffer(),
		"fake backend publishes completion for a paused positional phase");
	manager.update();
	check(manager.getActiveAudioCount() == 2 && manager.isCurrentlyPlaying(positionalHandle)
		&& positionalVoice->submitCalls == pausedSubmitCalls,
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

	manager.setChannelLimitsForTest(1U, 1U, 1U);
	AudioEventInfo *lowInfo = newInstance(AudioEventInfo);
	*lowInfo = *fixtureInfo;
	lowInfo->m_attackSounds.clear();
	lowInfo->m_decaySounds.clear();
	lowInfo->m_sounds.clear();
	lowInfo->m_sounds.push_back(AsciiString("main.wav"));
	lowInfo->m_limit = 0;
	lowInfo->m_priority = AP_LOW;
	FixtureEvent lowPriorityEvent(AsciiString("policy-low"));
	lowPriorityEvent.setAudioEventInfo(lowInfo);
	lowPriorityEvent.setAudioPriority(AP_LOW);
	const AudioHandle lowPriorityHandle = manager.addAudioEvent(&lowPriorityEvent);
	manager.update();
	check(manager.isCurrentlyPlaying(lowPriorityHandle),
		"configured channel policy admits the first low-priority event");

	AudioEventInfo *highInfo = newInstance(AudioEventInfo);
	*highInfo = *lowInfo;
	highInfo->m_priority = AP_HIGH;
	FixtureEvent highPriorityEvent(AsciiString("policy-high"));
	highPriorityEvent.setAudioEventInfo(highInfo);
	highPriorityEvent.setAudioPriority(AP_HIGH);
	const AudioHandle highPriorityHandle = manager.addAudioEvent(&highPriorityEvent);
	manager.update();
	check(manager.isCurrentlyPlaying(highPriorityHandle)
		&& !manager.isCurrentlyPlaying(lowPriorityHandle),
		"configured channel policy replaces a lower-priority event");
	manager.stopAudio(AudioAffect_Sound);
	manager.update();

	AudioEventInfo *protectedInfo = newInstance(AudioEventInfo);
	*protectedInfo = *lowInfo;
	FixtureEvent protectedEvent(AsciiString("policy-protected"));
	protectedEvent.setAudioEventInfo(protectedInfo);
	protectedEvent.setUninterruptible(TRUE);
	const AudioHandle protectedHandle = manager.addAudioEvent(&protectedEvent);
	manager.update();
	FixtureEvent blockedEvent(AsciiString("policy-blocked"));
	blockedEvent.setAudioEventInfo(highInfo);
	blockedEvent.setAudioPriority(AP_HIGH);
	const AudioHandle blockedHandle = manager.addAudioEvent(&blockedEvent);
	manager.update();
	check(manager.isCurrentlyPlaying(protectedHandle) && !manager.isCurrentlyPlaying(blockedHandle),
		"protected uninterruptible events cannot be replaced by priority policy");
	manager.stopAudio(AudioAffect_Sound);
	manager.update();

	AudioEventInfo *objectInfo = newInstance(AudioEventInfo);
	*objectInfo = *lowInfo;
	objectInfo->m_type = ST_WORLD | ST_VOICE;
	FixtureEvent objectVoiceEvent(AsciiString("policy-object-voice"));
	objectVoiceEvent.setAudioEventInfo(objectInfo);
	objectVoiceEvent.setObjectID(static_cast<ObjectID>(42U));
	const AudioHandle objectVoiceHandle = manager.addAudioEvent(&objectVoiceEvent);
	manager.update();
	check(manager.isCurrentlyPlaying(objectVoiceHandle) && manager.isObjectPlayingVoice(42U),
		"configured object voice query reports the active voice record");
	FixtureEvent voiceReplacement(AsciiString("policy-voice-replacement"));
	voiceReplacement.setAudioEventInfo(objectInfo);
	voiceReplacement.setAudioPriority(AP_HIGH);
	Coord3D voiceReplacementPosition;
	voiceReplacementPosition.set(12.0f, 0.0f, 0.0f);
	voiceReplacement.setPosition(&voiceReplacementPosition);
	const AudioHandle voiceReplacementHandle = manager.addAudioEvent(&voiceReplacement);
	manager.update();
	check(manager.isCurrentlyPlaying(objectVoiceHandle)
		&& !manager.isCurrentlyPlaying(voiceReplacementHandle),
		"configured 3D policy protects voice channels from priority replacement");
	manager.stopAudio(AudioAffect_Sound3D);
	manager.update();

	manager.setChannelLimitsForTest(1U, 1U, 1U);
	Coord3D forcedPosition;
	forcedPosition.set(14.0f, 0.0f, 0.0f);
	FixtureEvent forcedFiller(AsciiString("policy-forced-filler"));
	forcedFiller.setAudioEventInfo(lowInfo);
	forcedFiller.setAudioPriority(AP_LOW);
	forcedFiller.setPosition(&forcedPosition);
	const AudioHandle forcedFillerHandle = manager.addAudioEvent(&forcedFiller);
	manager.update();
	check(manager.isCurrentlyPlaying(forcedFillerHandle),
		"forced-admission fixture fills the configured 3D channel pool");
	AudioEventInfo *forcedInfo = newInstance(AudioEventInfo);
	*forcedInfo = *lowInfo;
	forcedInfo->m_audioName = AsciiString("policy-forced-voice");
	forcedInfo->m_type = ST_WORLD | ST_VOICE;
	FixtureEvent forcedEvent(AsciiString("policy-forced-voice"));
	forcedEvent.setAudioEventInfo(forcedInfo);
	forcedEvent.setPosition(&forcedPosition);
	forcedEvent.setObjectID(static_cast<ObjectID>(99U));
	manager.friend_forcePlayAudioEventRTS(&forcedEvent);
	manager.update();
	check(manager.isObjectPlayingVoice(99U),
		"force-play admits a protected voice while its normal channel is full");
	AudioEventInfo *forcedHighInfo = newInstance(AudioEventInfo);
	*forcedHighInfo = *lowInfo;
	forcedHighInfo->m_audioName = AsciiString("policy-after-force-high");
	forcedHighInfo->m_priority = AP_HIGH;
	FixtureEvent forcedHigh(AsciiString("policy-after-force-high"));
	forcedHigh.setAudioEventInfo(forcedHighInfo);
	forcedHigh.setAudioPriority(AP_HIGH);
	forcedHigh.setPosition(&forcedPosition);
	const AudioHandle forcedHighHandle = manager.addAudioEvent(&forcedHigh);
	manager.update();
	check(manager.isObjectPlayingVoice(99U) && manager.isCurrentlyPlaying(forcedHighHandle),
		"normal high-priority admission cannot evict a forced record");
	AudioEventInfo *forcedLowInfo = newInstance(AudioEventInfo);
	*forcedLowInfo = *lowInfo;
	forcedLowInfo->m_audioName = AsciiString("policy-after-force-low");
	FixtureEvent forcedLow(AsciiString("policy-after-force-low"));
	forcedLow.setAudioEventInfo(forcedLowInfo);
	forcedLow.setAudioPriority(AP_LOW);
	forcedLow.setPosition(&forcedPosition);
	const AudioHandle forcedLowHandle = manager.addAudioEvent(&forcedLow);
	manager.update();
	check(manager.isObjectPlayingVoice(99U) && !manager.isCurrentlyPlaying(forcedLowHandle),
		"normal lower-priority admission leaves the forced record protected");
	manager.stopAudio(AudioAffect_Sound3D);
	manager.update();
	check(!manager.isObjectPlayingVoice(99U) && manager.getActiveAudioCount() == 0,
		"affect stop cleans up forced and normal records together");
	deleteInstance(forcedInfo);
	deleteInstance(forcedHighInfo);
	deleteInstance(forcedLowInfo);

	catalog.setDurationMS(AsciiString("short.wav"), 1.0f);
	AudioEventInfo *nonLoopInfo = newInstance(AudioEventInfo);
	*nonLoopInfo = *lowInfo;
	nonLoopInfo->m_sounds.clear();
	nonLoopInfo->m_sounds.push_back(AsciiString("short.wav"));
	nonLoopInfo->m_loopCount = 2;
	nonLoopInfo->m_control = 0;
	FixtureEvent nonLoopEvent(AsciiString("non-loop"));
	nonLoopEvent.setAudioEventInfo(nonLoopInfo);
	const AudioHandle nonLoopHandle = manager.addAudioEvent(&nonLoopEvent);
	manager.update();
	manager.update();
	FakeVoice *nonLoopVoice = engine->lastVoice;
	check(nonLoopVoice != nullptr && nonLoopVoice->completeLastBuffer(),
		"fake backend completes non-loop audio");
	manager.update();
	manager.update();
	check(!manager.isCurrentlyPlaying(nonLoopHandle),
		"non-loop audio does not restart when only a loop count is present");

	AudioEventInfo *loopInfo = newInstance(AudioEventInfo);
	*loopInfo = *nonLoopInfo;
	loopInfo->m_control = AC_LOOP;
	FixtureEvent loopEvent(AsciiString("loop"));
	loopEvent.setAudioEventInfo(loopInfo);
	const AudioHandle loopHandle = manager.addAudioEvent(&loopEvent);
	manager.update();
	manager.update();
	FakeVoice *loopVoice = engine->lastVoice;
	check(loopVoice != nullptr && loopVoice->completeLastBuffer(),
		"fake backend completes loop audio");
	manager.update();
	manager.update();
	check(manager.isCurrentlyPlaying(loopHandle) && loopVoice->submitCalls >= 2,
		"AC_LOOP audio deterministically restarts after terminal phase completion");
	manager.stopAudio(AudioAffect_Sound);
	manager.update();

	AudioEventInfo *musicOneInfo = newInstance(AudioEventInfo);
	*musicOneInfo = *lowInfo;
	musicOneInfo->m_audioName = AsciiString("music-one");
	musicOneInfo->m_soundType = AT_Music;
	musicOneInfo->m_sounds.clear();
	musicOneInfo->m_sounds.push_back(AsciiString("short.wav"));
	musicOneInfo->m_loopCount = 0;
	musicOneInfo->m_control = 0;
	AudioEventInfo *musicTwoInfo = newInstance(AudioEventInfo);
	*musicTwoInfo = *musicOneInfo;
	musicTwoInfo->m_audioName = AsciiString("music-two");
	manager.addAudioEventInfo(musicOneInfo);
	manager.addAudioEventInfo(musicTwoInfo);
	manager.setChannelLimitsForTest(1U, 1U, 2U);
	manager.addTrackName(AsciiString("music-one"));
	manager.addTrackName(AsciiString("music-two"));
	manager.setActiveMusicTrackForTest(AsciiString("music-one"));
	FixtureEvent musicEvent(AsciiString("music-one"));
	musicEvent.setAudioEventInfo(musicOneInfo);
	const AudioHandle musicHandle = manager.addAudioEvent(&musicEvent);
	manager.update();
	manager.update();
	FakeVoice *musicVoice = engine->lastVoice;
	check(manager.isCurrentlyPlaying(musicHandle) && manager.isMusicPlaying(),
		"music fixture starts through the native stream channel");
	check(musicVoice != nullptr && musicVoice->completeLastBuffer(),
		"fake backend completes natural music playback");
	manager.update();
	manager.update();
	check(!manager.isMusicPlaying() && manager.hasMusicTrackCompleted(AsciiString("music-one"), 1),
		"only natural terminal music completion records history");
	const AsciiString nextTrack = manager.nextMusicTrack();
	check(nextTrack == AsciiString("music-two"),
		"next music selects and enqueues the next configured track");
	manager.update();
	check(manager.isMusicPlaying(), "next music transition leaves the new track active");
	const AsciiString previousTrack = manager.prevMusicTrack();
	check(previousTrack == AsciiString("music-one"),
		"previous music selects and enqueues the prior configured track");
	manager.update();
	check(manager.isMusicPlaying(), "previous music transition leaves the new track active");

	FixtureEvent forceEvent(AsciiString("music-two"));
	forceEvent.setAudioEventInfo(musicTwoInfo);
	manager.friend_forcePlayAudioEventRTS(&forceEvent);
	manager.update();
	check(manager.isMusicPlaying(), "force-play music enters the normal active lifecycle");
	manager.stopAudio(AudioAffect_Music);
	manager.update();
	check(!manager.isMusicPlaying() && !manager.hasMusicTrackCompleted(AsciiString("music-two"), 1),
		"stopped music does not record natural completion");

	AudioSettings settings;
	settings.m_use3DSoundRangeVolumeFade = TRUE;
	settings.m_3DSoundRangeVolumeFadeExponent = 2.0f;
	settings.m_globalMinRange = 10;
	settings.m_globalMaxRange = 100;
	settings.m_fadeAudioFrames = 3;
	manager.setAudioSettingsForTest(&settings);
	manager.setVolume(0.5f, AudioAffect_Sound3D);
	AudioEventInfo *attenuationInfo = newInstance(AudioEventInfo);
	*attenuationInfo = *lowInfo;
	attenuationInfo->m_type = ST_WORLD | ST_GLOBAL;
	attenuationInfo->m_volume = 0.8f;
	attenuationInfo->m_volumeShift = 0.5f;
	attenuationInfo->m_minDistance = 0.0f;
	attenuationInfo->m_maxDistance = 100.0f;
	FixtureEvent attenuationEvent(AsciiString("configured-attenuation"));
	attenuationEvent.setAudioEventInfo(attenuationInfo);
	Coord3D attenuationPosition;
	attenuationPosition.set(55.0f, 0.0f, 0.0f);
	attenuationEvent.setPosition(&attenuationPosition);
	const AudioHandle attenuationHandle = manager.addAudioEvent(&attenuationEvent);
	manager.update();
	manager.update();
	FakeVoice *attenuationVoice = engine->lastVoice;
	check(attenuationVoice != nullptr && attenuationVoice->lastVolume > 0.149f
		&& attenuationVoice->lastVolume < 0.151f,
		"effective volume applies event shifts, category volume, and configured global attenuation");
	TheTacticalView = reinterpret_cast<View *>(static_cast<std::uintptr_t>(1));
	g_nativeAudioBaseUpdateCalls = 0;
	manager.update();
	TheTacticalView = nullptr;
	check(g_nativeAudioBaseUpdateCalls == 1 && attenuationVoice->lastVolume > 0.074f
		&& attenuationVoice->lastVolume < 0.076f,
		"active 3D volume applies the base zoom adjustment exactly once");
	Coord3D listenerPosition;
	listenerPosition.set(55.0f, 0.0f, 0.0f);
	manager.setListenerPosition(&listenerPosition, nullptr);
	manager.update();
	check(attenuationVoice->lastVolume > 0.19f && attenuationVoice->lastVolume < 0.21f,
		"listener movement updates active 3D attenuation on the owner thread");
	manager.setAudioEventVolumeOverride(AsciiString("configured-attenuation"), 0.4f);
	check(attenuationVoice->lastVolume > 0.099f && attenuationVoice->lastVolume < 0.101f,
		"active event volume override replaces the event volume exactly once");
	manager.stopAudio(AudioAffect_Sound3D);
	manager.update();
	check(!manager.isCurrentlyPlaying(attenuationHandle), "3D attenuation fixture stops cleanly");

	AudioEventInfo *fadeInfo = newInstance(AudioEventInfo);
	*fadeInfo = *musicOneInfo;
	fadeInfo->m_audioName = AsciiString("fade-music");
	FixtureEvent fadeEvent(AsciiString("fade-music"));
	fadeEvent.setAudioEventInfo(fadeInfo);
	const AudioHandle fadeHandle = manager.addAudioEvent(&fadeEvent);
	manager.update();
	FakeVoice *fadeVoice = engine->lastVoice;
	const float fadeStartVolume = fadeVoice == nullptr ? 0.0f : fadeVoice->lastVolume;
	manager.removeAudioEvent(AHSV_StopTheMusicFade);
	manager.update();
	check(fadeVoice != nullptr && fadeVoice->lastVolume > 0.0f
		&& fadeVoice->lastVolume < fadeStartVolume,
		"configured fade frame count reduces volume before stopping");
	check(fadeVoice != nullptr && fadeVoice->completeLastBuffer(),
		"fake backend completes a fade-pending music buffer");
	manager.update();
	check(!manager.isCurrentlyPlaying(fadeHandle)
		&& !manager.hasMusicTrackCompleted(AsciiString("fade-music"), 1),
		"completion observed during a pending fade is treated as an intentional stop");
	manager.update();
	manager.update();
	manager.update();
	check(!manager.isCurrentlyPlaying(fadeHandle)
		&& !manager.hasMusicTrackCompleted(AsciiString("fade-music"), 1),
		"faded music stops without recording natural completion");

	AudioEventInfo *speechInfo = newInstance(AudioEventInfo);
	*speechInfo = *lowInfo;
	speechInfo->m_soundType = AT_Streaming;
	speechInfo->m_sounds.clear();
	speechInfo->m_sounds.push_back(AsciiString("short.wav"));
	FixtureEvent guardedSpeech(AsciiString("guarded-speech"));
	guardedSpeech.setAudioEventInfo(speechInfo);
	guardedSpeech.setUninterruptible(TRUE);
	const AudioHandle guardedSpeechHandle = manager.addAudioEvent(&guardedSpeech);
	manager.update();
	check(manager.isCurrentlyPlaying(guardedSpeechHandle) && manager.getDisallowSpeech(),
		"uninterruptible native speech raises the disallow-speech guard");
	FixtureEvent blockedSpeech(AsciiString("blocked-speech"));
	blockedSpeech.setAudioEventInfo(speechInfo);
	const AudioHandle blockedSpeechHandle = manager.addAudioEvent(&blockedSpeech);
	check(blockedSpeechHandle == AHSV_NoSound || blockedSpeechHandle == AHSV_Error,
		"disallow-speech blocks a second native speech stream");
	manager.stopAudio(AudioAffect_Speech);
	manager.update();
	check(!manager.getDisallowSpeech(), "speech guard releases after native stream stop");
	const AudioHandle releasedSpeechHandle = manager.addAudioEvent(&blockedSpeech);
	manager.update();
	check(releasedSpeechHandle != AHSV_NoSound && manager.isCurrentlyPlaying(releasedSpeechHandle),
		"speech admission resumes after the uninterruptible stream ends");
	manager.stopAudio(AudioAffect_Speech);
	manager.update();

	std::unique_ptr<FakeEngine> failureOwnedEngine = std::make_unique<FakeEngine>();
	FakeEngine *failureEngine = failureOwnedEngine.get();
	XAudio2AudioService failureService(std::move(failureOwnedEngine));
	XAudio2AudioManager failureManager(&failureService, &catalog);
	failureManager.openDevice();
	AudioEventInfo *failureMusicInfo = newInstance(AudioEventInfo);
	*failureMusicInfo = *lowInfo;
	failureMusicInfo->m_audioName = AsciiString("failure-create-music");
	failureMusicInfo->m_soundType = AT_Music;
	failureMusicInfo->m_type = 0;
	failureMusicInfo->m_sounds.clear();
	failureMusicInfo->m_sounds.push_back(AsciiString("short.wav"));
	FixtureEvent failureMusic(AsciiString("failure-create-music"));
	failureMusic.setAudioEventInfo(failureMusicInfo);
	failureEngine->failCreateVoice = TRUE;
	const AudioHandle failureCreateHandle = failureManager.addAudioEvent(&failureMusic);
	failureManager.update();
	check(failureCreateHandle != AHSV_NoSound && !failureManager.isCurrentlyPlaying(failureCreateHandle)
		&& failureManager.getActiveAudioCount() == 0
		&& !failureManager.hasMusicTrackCompleted(AsciiString("failure-create-music"), 1),
		"voice-create failure releases the music handle without false natural completion");

	AudioEventInfo *failureResetInfo = newInstance(AudioEventInfo);
	*failureResetInfo = *speechInfo;
	failureResetInfo->m_audioName = AsciiString("failure-reset-speech");
	FixtureEvent failureReset(AsciiString("failure-reset-speech"));
	failureReset.setAudioEventInfo(failureResetInfo);
	failureReset.setUninterruptible(TRUE);
	failureEngine->failCreateVoice = FALSE;
	failureEngine->failStop = TRUE;
	const AudioHandle failureResetHandle = failureManager.addAudioEvent(&failureReset);
	failureManager.update();
	check(failureManager.isCurrentlyPlaying(failureResetHandle)
		&& failureManager.getDisallowSpeech(),
		"reset-failure fixture first raises the native speech guard");
	failureManager.update();
	check(!failureManager.isCurrentlyPlaying(failureResetHandle)
		&& failureManager.getActiveAudioCount() == 0 && !failureManager.getDisallowSpeech(),
		"voice-reset failure deterministically releases the speech guard and active handle");

	AudioEventInfo *failureSubmitInfo = newInstance(AudioEventInfo);
	*failureSubmitInfo = *lowInfo;
	failureSubmitInfo->m_audioName = AsciiString("failure-submit");
	failureSubmitInfo->m_sounds.clear();
	failureSubmitInfo->m_sounds.push_back(AsciiString("short.wav"));
	FixtureEvent failureSubmit(AsciiString("failure-submit"));
	failureSubmit.setAudioEventInfo(failureSubmitInfo);
	failureEngine->failStop = FALSE;
	failureEngine->failSubmit = TRUE;
	const AudioHandle failureSubmitHandle = failureManager.addAudioEvent(&failureSubmit);
	failureManager.update();
	check(failureManager.isCurrentlyPlaying(failureSubmitHandle),
		"submit-failure fixture reaches the typed voice submission boundary");
	failureManager.update();
	check(!failureManager.isCurrentlyPlaying(failureSubmitHandle)
		&& failureManager.getActiveAudioCount() == 0,
		"voice-submit failure releases the active handle without waiting forever");
	failureManager.closeDevice();
	deleteInstance(failureMusicInfo);
	deleteInstance(failureResetInfo);
	deleteInstance(failureSubmitInfo);

	deleteInstance(lowInfo);
	deleteInstance(highInfo);
	deleteInstance(protectedInfo);
	deleteInstance(objectInfo);
	deleteInstance(nonLoopInfo);
	deleteInstance(loopInfo);
	deleteInstance(attenuationInfo);
	deleteInstance(fadeInfo);
	deleteInstance(speechInfo);
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
	std::filesystem::remove_all(realRoot);
	return failures == 0 ? 0 : 1;
}
