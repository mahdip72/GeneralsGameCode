#include <Utility/CppMacros.h>
#include "Lib/BaseType.h"

#include "Common/AudioAffect.h"
#include "Common/AudioEventInfo.h"
#include "Common/AudioHandleSpecialValues.h"
#include "Common/AudioSettings.h"
#include "XAudio2AudioDevice/IXAudio2AudioEngineBackend.h"
#include "XAudio2AudioDevice/XAudio2AudioManager.h"
#include "XAudio2AudioDevice/XAudio2AudioService.h"
#include "XAudio2AudioDevice/XAudio2PcmVoice.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

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

bool gainNear(float actual, float expected)
{
	return std::abs(actual - expected) < 0.000001f;
}

struct Submission
{
	UINT32 bytes = 0;
	float gain = 0;
	float pitch = 1;
	unsigned pitchCalls = 0;
	unsigned matrixCalls = 0;
};

struct VoiceSnapshot
{
	float gain = 1;
	float pitch = 1;
	float maximumPitch = 2;
	unsigned pitchCalls = 0;
	unsigned matrixCalls = 0;
	unsigned pauses = 0;
	unsigned resumes = 0;
	bool paused = false;
	bool destroyed = false;
	std::vector<Submission> submissions;
};

struct VoiceProbe
{
	std::mutex mutex;
	// Callback entry and DestroyVoice share this lock. A copied probe remains
	// valid after the owner destroys the backend, but never retains a callback.
	std::mutex callbackMutex;
	IXAudio2VoiceCallback *callback = nullptr;
	std::deque<void *> pending;
	VoiceSnapshot state;
	bool created = false;

	VoiceSnapshot snapshot()
	{
		std::lock_guard<std::mutex> lock(mutex);
		return state;
	}

	bool completeOldest()
	{
		std::lock_guard<std::mutex> callbackLock(callbackMutex);
		void *context = nullptr;
		{
			std::lock_guard<std::mutex> lock(mutex);
			if (callback == nullptr || pending.empty()) return false;
			context = pending.front();
			pending.pop_front();
		}
		callback->OnBufferEnd(context);
		return true;
	}
};

struct EngineProbe
{
	std::mutex mutex;
	std::thread::id owner;
	bool wrongThread = false;
	unsigned active = 0;
	unsigned peak = 0;
	std::vector<std::shared_ptr<VoiceProbe>> voices;

	void nativeCall()
	{
		std::lock_guard<std::mutex> lock(mutex);
		if (owner == std::thread::id()) owner = std::this_thread::get_id();
		wrongThread |= owner != std::this_thread::get_id();
	}

	std::shared_ptr<VoiceProbe> lastVoice()
	{
		std::lock_guard<std::mutex> lock(mutex);
		return voices.empty() ? nullptr : voices.back();
	}

	unsigned activeVoices()
	{
		std::lock_guard<std::mutex> lock(mutex);
		return active;
	}

	unsigned peakVoices()
	{
		std::lock_guard<std::mutex> lock(mutex);
		return peak;
	}
};

class FakeVoice final : public IXAudio2PcmVoiceBackend
{
public:
	FakeVoice(std::shared_ptr<EngineProbe> engine, std::shared_ptr<VoiceProbe> voice) :
		m_engine(std::move(engine)), m_voice(std::move(voice)) {}
	~FakeVoice() override { m_engine->nativeCall(); }

	HRESULT create(const WAVEFORMATEX &format, IXAudio2VoiceCallback *callback) noexcept override
	{
		return create(format, callback, XAUDIO2_DEFAULT_FREQ_RATIO);
	}
	HRESULT create(const WAVEFORMATEX &, IXAudio2VoiceCallback *callback, float maximum) noexcept override
	{
		m_engine->nativeCall();
		{
			std::lock_guard<std::mutex> callbackLock(m_voice->callbackMutex);
			std::lock_guard<std::mutex> lock(m_voice->mutex);
			m_voice->callback = callback;
			m_voice->created = true;
			m_voice->state.maximumPitch = maximum;
		}
		std::lock_guard<std::mutex> lock(m_engine->mutex);
		m_engine->peak = std::max(m_engine->peak, ++m_engine->active);
		return S_OK;
	}
	HRESULT submit(const XAUDIO2_BUFFER &buffer) noexcept override
	{
		m_engine->nativeCall();
		std::lock_guard<std::mutex> lock(m_voice->mutex);
		const VoiceSnapshot &state = m_voice->state;
		m_voice->pending.push_back(buffer.pContext);
		m_voice->state.submissions.push_back({buffer.AudioBytes, state.gain, state.pitch,
			state.pitchCalls, state.matrixCalls});
		return S_OK;
	}
	HRESULT start() noexcept override
	{
		m_engine->nativeCall();
		std::lock_guard<std::mutex> lock(m_voice->mutex);
		m_voice->state.paused = false;
		return S_OK;
	}
	HRESULT stop() noexcept override { m_engine->nativeCall(); return S_OK; }
	HRESULT flush() noexcept override { m_engine->nativeCall(); return S_OK; }
	HRESULT getCriticalError() const noexcept override { m_engine->nativeCall(); return S_OK; }
	HRESULT setVolume(float gain) noexcept override
	{
		m_engine->nativeCall();
		std::lock_guard<std::mutex> lock(m_voice->mutex);
		m_voice->state.gain = gain;
		return S_OK;
	}
	HRESULT setFrequencyRatio(float pitch) noexcept override
	{
		m_engine->nativeCall();
		std::lock_guard<std::mutex> lock(m_voice->mutex);
		m_voice->state.pitch = pitch;
		++m_voice->state.pitchCalls;
		return S_OK;
	}
	HRESULT setOutputMatrix(UINT32, UINT32, const float *) noexcept override
	{
		m_engine->nativeCall();
		std::lock_guard<std::mutex> lock(m_voice->mutex);
		++m_voice->state.matrixCalls;
		return S_OK;
	}
	HRESULT pause() noexcept override
	{
		m_engine->nativeCall();
		std::lock_guard<std::mutex> lock(m_voice->mutex);
		m_voice->state.paused = true;
		++m_voice->state.pauses;
		return S_OK;
	}
	HRESULT resume() noexcept override
	{
		m_engine->nativeCall();
		std::lock_guard<std::mutex> lock(m_voice->mutex);
		m_voice->state.paused = false;
		++m_voice->state.resumes;
		return S_OK;
	}
	void destroy() noexcept override
	{
		m_engine->nativeCall();
		bool created = false;
		{
			std::lock_guard<std::mutex> callbackLock(m_voice->callbackMutex);
			std::lock_guard<std::mutex> lock(m_voice->mutex);
			m_voice->callback = nullptr;
			m_voice->pending.clear();
			created = m_voice->created;
			m_voice->created = false;
			m_voice->state.destroyed = true;
		}
		std::lock_guard<std::mutex> lock(m_engine->mutex);
		if (created) --m_engine->active;
	}

private:
	std::shared_ptr<EngineProbe> m_engine;
	std::shared_ptr<VoiceProbe> m_voice;
};

class FakeEngine final : public IXAudio2AudioEngineBackend
{
public:
	explicit FakeEngine(std::shared_ptr<EngineProbe> probe) : m_probe(std::move(probe)) {}
	~FakeEngine() override { m_probe->nativeCall(); }
	HRESULT open(CriticalErrorCallback, void *) noexcept override { m_probe->nativeCall(); return S_OK; }
	HRESULT start() noexcept override { m_probe->nativeCall(); return S_OK; }
	HRESULT stop() noexcept override { m_probe->nativeCall(); return S_OK; }
	HRESULT close() noexcept override { m_probe->nativeCall(); return S_OK; }
	HRESULT getOutputDetails(XAudio2OutputDetails &details) const noexcept override
	{
		m_probe->nativeCall();
		details.channelCount = 2;
		details.channelMask = SPEAKER_STEREO;
		return S_OK;
	}
	HRESULT createPcmVoice(std::unique_ptr<IXAudio2PcmVoiceBackend> &voice) noexcept override
	{
		m_probe->nativeCall();
		auto probe = std::make_shared<VoiceProbe>();
		voice = std::make_unique<FakeVoice>(m_probe, probe);
		std::lock_guard<std::mutex> lock(m_probe->mutex);
		m_probe->voices.push_back(std::move(probe));
		return S_OK;
	}
private:
	std::shared_ptr<EngineProbe> m_probe;
};

// The device thread must never inherit the manager's source/decoder objects.
class OwnerCheckedSource final : public AudioAssetSource
{
public:
	AudioAssetCatalog catalog;
	mutable std::atomic<bool> wrongThread { false };
	const std::thread::id owner = std::this_thread::get_id();
	Bool getDurationMS(const AsciiString &name, Real &duration) const override
	{
		called();
		return catalog.getDurationMS(name, duration);
	}
	Bool decodePcm(const AsciiString &name, AudioPcmChunk &chunk, UnsignedInt maxFrames) const override
	{
		called();
		return catalog.decodePcm(name, chunk, maxFrames);
	}
	Bool decodePcmAt(const AsciiString &name, AudioPcmChunk &chunk, UnsignedInt maxFrames,
		UnsignedInt startFrame) const override
	{
		called();
		return catalog.decodePcmAt(name, chunk, maxFrames, startFrame);
	}
	Bool supportsPcmRangeDecode() const override { called(); return TRUE; }
	const void *getFileIdentity(const AsciiString &) const override { called(); return this; }
private:
	void called() const { if (owner != std::this_thread::get_id()) wrongThread.store(true); }
};

class FixtureEvent final : public AudioEventRTS
{
public:
	explicit FixtureEvent(const char *name) : AudioEventRTS(AsciiString(name)) {}
	void setPitch(Real value) { m_pitchShift = value; }
};

struct Fixture
{
	std::shared_ptr<EngineProbe> engine = std::make_shared<EngineProbe>();
	OwnerCheckedSource source;
	AudioSettings settings;
	XAudio2AudioService service;
	XAudio2AudioManager manager;
	std::vector<AudioEventInfo *> infos;

	Fixture() : service(std::make_unique<FakeEngine>(engine), XAudio2AudioExecutionMode::SHARED_OWNER),
		manager(&service, &source)
	{
		source.catalog.setDurationMS(AsciiString("attack.wav"), 100.0f);
		source.catalog.setDurationMS(AsciiString("main.wav"), 400.0f);
		source.catalog.setDurationMS(AsciiString("decay.wav"), 50.0f);
		source.catalog.setDurationMS(AsciiString("short.wav"), 100.0f);
		source.catalog.setDurationMS(AsciiString("long.wav"), 2500.0f);
		settings.m_minVolume = 0.0f;
		settings.m_fadeAudioFrames = 12;
		settings.m_use3DSoundRangeVolumeFade = FALSE;
		manager.setAudioSettingsForTest(&settings);
		manager.setChannelLimitsForTest(4, 4, 3);
		manager.openDevice();
		check(manager.isOpen() && service.ownerMetrics().sharedOwner && !service.ownerMetrics().forcedSerial,
			"manager explicitly opens the shared-owner service");
	}
	~Fixture()
	{
		manager.closeDevice();
		fence();
		check(engine->activeVoices() == 0, "manager close quiesces all native voices");
		check(!source.wrongThread.load(), "asset lookup and PCM decoding remain on the game owner");
		{
			std::lock_guard<std::mutex> lock(engine->mutex);
			check(!engine->wrongThread && engine->owner != std::this_thread::get_id(),
				"all native calls run on one service owner distinct from the game owner");
		}
		manager.setAudioSettingsForTest(nullptr);
		for (AudioEventInfo *info : infos) deleteInstance(info);
	}

	AudioEventInfo *info(AudioType type, const char *file = "short.wav")
	{
		AudioEventInfo *result = newInstance(AudioEventInfo);
		infos.push_back(result);
		result->m_soundType = type;
		result->m_volume = result->m_volumeShift = 1.0f;
		result->m_minVolume = 0.0f;
		result->m_pitchShiftMin = result->m_pitchShiftMax = 1.0f;
		result->m_delayMin = result->m_delayMax = 0;
		result->m_lowPassFreq = 1.0f;
		result->m_limit = 0;
		result->m_priority = AP_NORMAL;
		result->m_type = type == AT_SoundEffect ? ST_WORLD : 0;
		result->m_control = 0;
		result->m_loopCount = 0;
		result->m_minDistance = 10.0f;
		result->m_maxDistance = 100.0f;
		if (type == AT_SoundEffect) result->m_sounds.push_back(AsciiString(file));
		else result->m_filename = AsciiString(file);
		return result;
	}

	void fence() { check(service.synchronize(), "shared owner fence completes without native failure"); }
	void tick() { manager.update(); fence(); }
	std::shared_ptr<VoiceProbe> lastVoice()
	{
		auto voice = engine->lastVoice();
		check(voice != nullptr, "manager creates the expected native voice");
		return voice;
	}
	void complete(const std::shared_ptr<VoiceProbe> &voice)
	{
		check(voice != nullptr && voice->completeOldest(), "fake callback completes the oldest queued PCM buffer");
		// The first fence publishes the callback mailbox; exactly one manager
		// frame then consumes it. No sleeps, polling loops or extra fade frames.
		fence();
		tick();
	}
};

VoiceSnapshot snapshot(const std::shared_ptr<VoiceProbe> &voice)
{
	return voice != nullptr ? voice->snapshot() : VoiceSnapshot {};
}

void testPitchAndCategoryControls()
{
	Fixture f;
	AudioEventInfo *sample = f.info(AT_SoundEffect, "main.wav");
	sample->m_attackSounds.push_back(AsciiString("attack.wav"));
	sample->m_decaySounds.push_back(AsciiString("decay.wav"));
	for (bool positional : { false, true }) {
		for (float pitch : { 0.75f, 1.25f, 3.0f }) {
			FixtureEvent event("shared-sample-pitch");
			event.setAudioEventInfo(sample);
			event.setPitch(pitch);
			Coord3D position = { 1.0f, 0.0f, 0.0f };
			if (positional) event.setPosition(&position);
			const AudioHandle handle = f.manager.addAudioEvent(&event);
			f.tick();
			auto voice = f.lastVoice();
			VoiceSnapshot state = snapshot(voice);
			check(handle >= AHSV_FirstHandle && state.submissions.size() == 1
				&& state.pitch == pitch && state.maximumPitch == std::max(1.0f, pitch)
				&& state.submissions[0].pitch == pitch && state.submissions[0].pitchCalls == 1
				&& gainNear(state.submissions[0].gain, positional ? 1.0f : 0.81225239f)
				&& (state.submissions[0].matrixCalls != 0) == positional,
				"2D/3D sample pitch, gain and spatial state precede the first PCM submission");
			const AudioAffect affect = positional ? AudioAffect_Sound3D : AudioAffect_Sound;
			f.manager.pauseAudio(affect);
			f.fence();
			check(snapshot(voice).paused, "manager pause reaches the shared native voice");
			f.manager.resumeAudio(affect);
			f.fence();
			check(!snapshot(voice).paused, "manager resume reaches the same native voice");
			for (unsigned phase = 0; phase != 2; ++phase) {
				f.complete(voice);
				state = snapshot(voice);
				check(state.pitch == pitch && state.pitchCalls == 1 && state.submissions.size() == phase + 2,
					"selected sample pitch persists across attack/body/decay without reapplication");
			}
			f.complete(voice);
			check(!f.manager.isCurrentlyPlaying(handle) && f.engine->activeVoices() == 0
				&& snapshot(voice).destroyed, "final sample completion retires its manager and native voice");
		}
	}

	const std::array<AudioAffect, 4> affects = { AudioAffect_Sound, AudioAffect_Sound3D,
		AudioAffect_Music, AudioAffect_Speech };
	std::array<std::shared_ptr<VoiceProbe>, 4> voices;
	std::array<AudioHandle, 4> handles;
	for (std::size_t i = 0; i != voices.size(); ++i) {
		FixtureEvent event(i == 0 ? "category-2d" : i == 1 ? "category-3d" : i == 2 ? "category-music" : "category-speech");
		event.setAudioEventInfo(f.info(i < 2 ? AT_SoundEffect : i == 2 ? AT_Music : AT_Streaming, "long.wav"));
		event.setPitch(3.0f);
		Coord3D position = { 1.0f, 0.0f, 0.0f };
		if (i == 1) event.setPosition(&position);
		handles[i] = f.manager.addAudioEvent(&event);
		f.tick();
		voices[i] = f.lastVoice();
		const VoiceSnapshot state = snapshot(voices[i]);
		check(handles[i] >= AHSV_FirstHandle && !state.submissions.empty()
			&& (i < 2 ? state.pitch == 3.0f : state.pitch == 1.0f && state.pitchCalls == 0),
			"sample-only pitch does not leak into music or speech streams");
	}
	for (std::size_t selected = 0; selected != voices.size(); ++selected) {
		f.manager.setVolume(0.25f, static_cast<AudioAffect>(affects[selected] | AudioAffect_SystemSetting));
		f.tick();
		for (std::size_t i = 0; i != voices.size(); ++i) {
			// Exact Stage3 Miles gain oracles; the 3D source is inside min range.
			const float expected = i == selected ? (i == 1 ? 0.099212566f : 0.08058564f)
				: (i == 1 ? 1.0f : 0.81225239f);
			check(gainNear(snapshot(voices[i]).gain, expected), "a live category slider changes only that category's converted gain");
		}
		f.manager.setVolume(0.5f, affects[selected]);
		f.tick();
		check(gainNear(snapshot(voices[selected]).gain, selected == 1 ? 0.03125f : 0.025382888f),
			"script volume and the selected system slider multiply before legacy gain conversion");
		f.manager.setVolume(0.0f, static_cast<AudioAffect>(affects[selected] | AudioAffect_SystemSetting));
		f.tick();
		check(snapshot(voices[selected]).gain == 0.0f && f.manager.isCurrentlyPlaying(handles[selected]),
			"category mute silences but does not retire the active manager voice");
		f.manager.setVolume(1.0f, affects[selected]);
		f.manager.setVolume(1.0f, static_cast<AudioAffect>(affects[selected] | AudioAffect_SystemSetting));
		f.tick();
	}
}

void testMusicLifecycle()
{
	Fixture f;
	FixtureEvent music("shared-natural-music");
	music.setAudioEventInfo(f.info(AT_Music));
	const AudioHandle handle = f.manager.addAudioEvent(&music);
	f.tick();
	auto voice = f.lastVoice();
	check(f.manager.isMusicPlaying() && !f.manager.hasMusicTrackCompleted(AsciiString("shared-natural-music"), 1),
		"music without AC_LOOP begins with no natural completion");
	for (int completion = 1; completion <= 2; ++completion) {
		f.complete(voice);
		check(f.manager.isCurrentlyPlaying(handle) && f.manager.isMusicPlaying()
			&& f.manager.hasMusicTrackCompleted(AsciiString("shared-natural-music"), completion)
			&& !f.manager.hasMusicTrackCompleted(AsciiString("shared-natural-music"), completion + 1)
			&& snapshot(voice).submissions.size() == static_cast<std::size_t>(completion + 1),
			"natural shared-owner music EOS loops and increments exactly one live completion");
	}
	f.manager.pauseAudio(AudioAffect_Music);
	f.fence();
	check(!f.manager.isMusicPlaying() && !f.manager.hasMusicTrackCompleted(AsciiString("shared-natural-music"), 2)
		&& snapshot(voice).paused, "paused music is excluded from active completion queries");
	f.manager.resumeAudio(AudioAffect_Music);
	f.fence();
	check(f.manager.isMusicPlaying() && f.manager.hasMusicTrackCompleted(AsciiString("shared-natural-music"), 2),
		"resuming music preserves its live completion count");
	f.manager.removeAudioEvent(AHSV_StopTheMusicFade);
	f.tick();
	check(!f.manager.isMusicPlaying() && !f.manager.hasMusicTrackCompleted(AsciiString("shared-natural-music"), 2),
		"fading music no longer qualifies as the active track");
	f.complete(voice);
	check(!f.manager.isCurrentlyPlaying(handle) && snapshot(voice).destroyed
		&& !f.manager.hasMusicTrackCompleted(AsciiString("shared-natural-music"), 1),
		"EOS during an intentional music fade retires without a natural loop");
	const AudioHandle restarted = f.manager.addAudioEvent(&music);
	f.tick();
	voice = f.lastVoice();
	check(f.manager.isCurrentlyPlaying(restarted)
		&& !f.manager.hasMusicTrackCompleted(AsciiString("shared-natural-music"), 1),
		"same-name restart has no stale completion history");
	f.complete(voice);
	check(f.manager.hasMusicTrackCompleted(AsciiString("shared-natural-music"), 1),
		"same-name restart records its own first completion");
	const UnsignedInt generation = f.manager.getLifecycleGeneration();
	f.manager.reset();
	f.fence();
	check(f.manager.isOpen() && f.manager.getLifecycleGeneration() != generation
		&& !f.manager.isCurrentlyPlaying(restarted) && f.manager.getActiveAudioCount() == 0
		&& !f.manager.hasMusicTrackCompleted(AsciiString("shared-natural-music"), 1)
		&& voice != nullptr && !voice->completeOldest(),
		"manager reset clears music history and quiesces the old callback generation");
	const AudioHandle afterReset = f.manager.addAudioEvent(&music);
	f.tick();
	check(f.manager.isCurrentlyPlaying(afterReset)
		&& !f.manager.hasMusicTrackCompleted(AsciiString("shared-natural-music"), 1),
		"post-reset music starts in a fresh shared-owner lifecycle");
}

void testMultiChunkFade()
{
	Fixture f;
	FixtureEvent music("shared-multichunk-fade");
	music.setAudioEventInfo(f.info(AT_Music, "long.wav"));
	const AudioHandle handle = f.manager.addAudioEvent(&music);
	f.tick();
	auto voice = f.lastVoice();
	VoiceSnapshot state = snapshot(voice);
	check(state.submissions.size() == 2 && state.submissions[0].bytes == 48000 * 4
		&& state.submissions[1].bytes == 48000 * 4, "music starts with two bounded one-second PCM submissions");
	f.manager.removeAudioEvent(AHSV_StopTheMusicFade);
	f.tick();
	const float firstFade = snapshot(voice).gain;
	f.complete(voice);
	state = snapshot(voice);
	check(f.manager.isCurrentlyPlaying(handle) && f.engine->activeVoices() == 1
		&& state.submissions.size() == 3 && state.submissions[2].bytes == 24000 * 4
		&& state.gain > 0 && state.gain < firstFade,
		"non-terminal fade completion replenishes the final half-second while gain continues declining");
	f.complete(voice);
	check(f.manager.isCurrentlyPlaying(handle), "decoder EOF is not playback EOS while the final fade chunk is queued");
	f.complete(voice);
	check(!f.manager.isCurrentlyPlaying(handle) && f.engine->activeVoices() == 0
		&& !f.manager.hasMusicTrackCompleted(AsciiString("shared-multichunk-fade"), 1),
		"true fade EOS releases the voice without looping or recording natural completion");

	const AudioHandle cancelled = f.manager.addAudioEvent(&music);
	f.tick();
	auto cancelledVoice = f.lastVoice();
	f.manager.removeAudioEvent(AHSV_StopTheMusicFade);
	f.tick();
	f.manager.stopAudio(AudioAffect_Music);
	f.tick();
	check(!f.manager.isCurrentlyPlaying(cancelled) && snapshot(cancelledVoice).destroyed,
		"explicit cancellation quiesces an in-flight multichunk fade");
	const AudioHandle expired = f.manager.addAudioEvent(&music);
	f.tick();
	f.manager.removeAudioEvent(AHSV_StopTheMusicFade);
	for (int frame = 0; frame != f.settings.m_fadeAudioFrames + 1; ++frame) f.tick();
	check(!f.manager.isCurrentlyPlaying(expired) && f.engine->activeVoices() == 0,
		"configured manager fade deadline stops PCM even without device callbacks");
}

void testVictoryOrderingAndBudgets()
{
	Fixture f;
	f.settings.m_fadeAudioFrames = 3;
	f.manager.setChannelLimitsForTest(1, 1, 3);
	AudioEventInfo *music = f.info(AT_Music, "long.wav");
	AudioEventInfo *speech = f.info(AT_Streaming, "long.wav");
	FixtureEvent outgoing("shared-old-music");
	outgoing.setAudioEventInfo(music);
	const AudioHandle outgoingHandle = f.manager.addAudioEvent(&outgoing);
	FixtureEvent dialogueOne("shared-dialogue-one");
	dialogueOne.setAudioEventInfo(speech);
	const AudioHandle one = f.manager.addAudioEvent(&dialogueOne);
	FixtureEvent dialogueTwo("shared-dialogue-two");
	dialogueTwo.setAudioEventInfo(speech);
	const AudioHandle two = f.manager.addAudioEvent(&dialogueTwo);
	f.tick();
	check(f.manager.isCurrentlyPlaying(outgoingHandle) && f.manager.isCurrentlyPlaying(one)
		&& f.manager.isCurrentlyPlaying(two) && f.engine->activeVoices() == 3,
		"old music and two dialogues occupy independent manager budgets");
	f.manager.removeAudioEvent(AHSV_StopTheMusicFade);
	FixtureEvent victory("End_USA");
	victory.setAudioEventInfo(music);
	const AudioHandle victoryHandle = f.manager.addAudioEvent(&victory);
	FixtureEvent dialogueThree("shared-dialogue-three");
	dialogueThree.setAudioEventInfo(speech);
	const AudioHandle three = f.manager.addAudioEvent(&dialogueThree);
	FixtureEvent dialogueFour("shared-dialogue-four");
	dialogueFour.setAudioEventInfo(speech);
	check(victoryHandle >= AHSV_FirstHandle && three >= AHSV_FirstHandle
		&& f.manager.addAudioEvent(&dialogueFour) == AHSV_NoSound,
		"victory music has its own reservation while pending dialogue respects the three-stream cap");
	f.tick();
	check(f.manager.isCurrentlyPlaying(outgoingHandle) && f.manager.isCurrentlyPlaying(victoryHandle)
		&& f.manager.isCurrentlyPlaying(three) && f.engine->activeVoices() == 5,
		"fading old music and victory music coexist with all three dialogue streams");
	for (int frame = 0; frame != 3; ++frame) f.tick();
	check(!f.manager.isCurrentlyPlaying(outgoingHandle) && f.manager.isCurrentlyPlaying(victoryHandle)
		&& f.manager.isCurrentlyPlaying(one) && f.manager.isCurrentlyPlaying(two)
		&& f.manager.isCurrentlyPlaying(three) && f.engine->activeVoices() == 4
		&& f.engine->peakVoices() <= 5
		&& !f.manager.hasMusicTrackCompleted(AsciiString("shared-old-music"), 1),
		"outgoing fade completion preserves victory/dialogue and does not create natural completion");

	FixtureEvent firstScripted("shared-first-scripted-speech");
	firstScripted.setAudioEventInfo(speech);
	firstScripted.setUninterruptible(TRUE);
	FixtureEvent victorySpeech("shared-victory-scripted-speech");
	victorySpeech.setAudioEventInfo(speech);
	victorySpeech.setUninterruptible(TRUE);
	const AudioHandle first = f.manager.addAudioEvent(&firstScripted);
	const AudioHandle final = f.manager.addAudioEvent(&victorySpeech);
	check(first >= AHSV_FirstHandle && final >= AHSV_FirstHandle,
		"same-tick uninterruptible speech requests are admitted even at full dialogue capacity");
	f.tick();
	check(!f.manager.isCurrentlyPlaying(first) && f.manager.isCurrentlyPlaying(final)
		&& !f.manager.isCurrentlyPlaying(one) && !f.manager.isCurrentlyPlaying(two)
		&& !f.manager.isCurrentlyPlaying(three) && f.manager.isCurrentlyPlaying(victoryHandle)
		&& f.manager.getDisallowSpeech() && f.engine->activeVoices() == 2,
		"later same-tick victory speech replaces prior/pending dialogue while preserving victory music");
	f.manager.killAudioEventImmediately(final);
	f.fence();
	check(!f.manager.isCurrentlyPlaying(final) && !f.manager.getDisallowSpeech()
		&& f.engine->activeVoices() == 1, "immediate victory-speech kill releases the native voice and admission guard");
}
}

int main()
{
#if defined(_MSC_VER)
	_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
	testPitchAndCategoryControls();
	testMusicLifecycle();
	testMultiChunkFade();
	testVictoryOrderingAndBudgets();
	std::printf("Native audio manager shared-owner contracts: %s\n", failures == 0 ? "PASS" : "FAIL");
	return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
