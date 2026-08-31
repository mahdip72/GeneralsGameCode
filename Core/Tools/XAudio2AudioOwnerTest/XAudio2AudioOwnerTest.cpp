#include "XAudio2AudioDevice/XAudio2AudioService.h"
#include "XAudio2AudioDevice/XAudio2AudioServiceOwner.h"
#include "XAudio2AudioDevice/XAudio2MoviePcmSink.h"
#include "XAudio2AudioDevice/XAudio2PcmVoice.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace
{
int failures = 0;

void check(bool value, const char *message)
{
	if (!value) { ++failures; std::fprintf(stderr, "FAIL: %s\n", message); }
}

template<class Predicate>
bool waitUntil(Predicate predicate)
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
	while (!predicate()) {
		if (std::chrono::steady_clock::now() >= deadline) return false;
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	return true;
}

struct Latch
{
	std::atomic<bool> entered { false };
	std::atomic<bool> release { true };
	void wait() noexcept
	{
		entered.store(true, std::memory_order_release);
		while (!release.load(std::memory_order_acquire)) std::this_thread::yield();
	}
};

struct VoiceProbe
{
	std::mutex mutex;
	std::mutex callbackMutex;
	IXAudio2VoiceCallback *callback = nullptr;
	std::vector<void *> contexts;
	std::vector<std::uint8_t> firstBytes;
	std::vector<float> gainsAtSubmit;
	std::vector<float> pitchesAtSubmit;
	float gain = 1;
	float pitch = 1;
	float maximumPitch = 2;
	HRESULT volumeResult = S_OK;
	int volumeCalls = 0;
	int starts = 0;
	int stops = 0;
	int matrices = 0;
	std::atomic<bool> destroyEntered { false };
	std::atomic<bool> destroyed { false };
	Latch submitLatch;
	Latch destroyLatch;
	bool failSubmit = false;

	std::size_t submitted()
	{
		std::lock_guard<std::mutex> lock(mutex);
		return contexts.size();
	}

	void complete(std::size_t index, Latch *latch = nullptr)
	{
		std::lock_guard<std::mutex> callbackLock(callbackMutex);
		void *context = nullptr;
		{
			std::lock_guard<std::mutex> lock(mutex);
			if (index >= contexts.size()) return;
			context = contexts[index];
		}
		if (latch != nullptr) latch->wait();
		if (callback != nullptr) callback->OnBufferEnd(context);
	}
};

struct EngineProbe
{
	std::mutex mutex;
	std::thread::id owner;
	bool wrongThread = false;
	bool failOpen = false;
	HRESULT volumeResult = S_OK;
	std::vector<std::shared_ptr<VoiceProbe>> voices;
	IXAudio2AudioEngineBackend::CriticalErrorCallback callback = nullptr;
	void *callbackContext = nullptr;
	std::atomic<int> closes { 0 };
	std::atomic<int> destructors { 0 };

	void call() noexcept
	{
		std::lock_guard<std::mutex> lock(mutex);
		if (owner == std::thread::id()) owner = std::this_thread::get_id();
		wrongThread |= owner != std::this_thread::get_id();
	}

	std::shared_ptr<VoiceProbe> voice(std::size_t index)
	{
		std::lock_guard<std::mutex> lock(mutex);
		return voices.at(index);
	}

	void critical(HRESULT error)
	{
		std::lock_guard<std::mutex> lock(mutex);
		if (callback != nullptr) callback(callbackContext, error);
	}
};

class FakeVoice final : public IXAudio2PcmVoiceBackend
{
public:
	FakeVoice(std::shared_ptr<EngineProbe> engine, std::shared_ptr<VoiceProbe> voice) :
		m_engine(std::move(engine)), m_voice(std::move(voice)) {}
	~FakeVoice() override { m_engine->call(); }
	HRESULT create(const WAVEFORMATEX &format, IXAudio2VoiceCallback *callback) noexcept override
	{
		return create(format, callback, 2);
	}
	HRESULT create(const WAVEFORMATEX &, IXAudio2VoiceCallback *callback, float maximum) noexcept override
	{
		m_engine->call();
		std::lock_guard<std::mutex> lock(m_voice->callbackMutex);
		m_voice->callback = callback;
		m_voice->maximumPitch = maximum;
		return S_OK;
	}
	HRESULT submit(const XAUDIO2_BUFFER &buffer) noexcept override
	{
		m_engine->call();
		m_voice->submitLatch.wait();
		std::lock_guard<std::mutex> lock(m_voice->mutex);
		m_voice->contexts.push_back(buffer.pContext);
		m_voice->firstBytes.push_back(buffer.pAudioData[0]);
		m_voice->gainsAtSubmit.push_back(m_voice->gain);
		m_voice->pitchesAtSubmit.push_back(m_voice->pitch);
		return m_voice->failSubmit ? E_ACCESSDENIED : S_OK;
	}
	HRESULT start() noexcept override
	{
		m_engine->call();
		std::lock_guard<std::mutex> lock(m_voice->mutex);
		++m_voice->starts;
		return S_OK;
	}
	HRESULT stop() noexcept override
	{
		m_engine->call();
		std::lock_guard<std::mutex> lock(m_voice->mutex);
		++m_voice->stops;
		return S_OK;
	}
	HRESULT flush() noexcept override { m_engine->call(); return S_OK; }
	HRESULT getCriticalError() const noexcept override { m_engine->call(); return S_OK; }
	HRESULT setVolume(float gain) noexcept override
	{
		m_engine->call();
		std::lock_guard<std::mutex> lock(m_voice->mutex);
		++m_voice->volumeCalls;
		if (FAILED(m_voice->volumeResult)) return m_voice->volumeResult;
		m_voice->gain = gain;
		return m_voice->volumeResult;
	}
	HRESULT setFrequencyRatio(float pitch) noexcept override
	{
		m_engine->call();
		std::lock_guard<std::mutex> lock(m_voice->mutex);
		m_voice->pitch = pitch;
		return S_OK;
	}
	HRESULT setOutputMatrix(UINT32, UINT32, const float *) noexcept override
	{
		m_engine->call();
		std::lock_guard<std::mutex> lock(m_voice->mutex);
		++m_voice->matrices;
		return S_OK;
	}
	void destroy() noexcept override
	{
		m_engine->call();
		m_voice->destroyEntered.store(true, std::memory_order_release);
		m_voice->destroyLatch.wait();
		std::lock_guard<std::mutex> lock(m_voice->callbackMutex);
		m_voice->callback = nullptr;
		m_voice->destroyed.store(true, std::memory_order_release);
	}

private:
	std::shared_ptr<EngineProbe> m_engine;
	std::shared_ptr<VoiceProbe> m_voice;
};

class FakeEngine final : public IXAudio2AudioEngineBackend
{
public:
	explicit FakeEngine(std::shared_ptr<EngineProbe> probe) : m_probe(std::move(probe)) {}
	~FakeEngine() override { m_probe->call(); ++m_probe->destructors; }
	HRESULT open(CriticalErrorCallback callback, void *context) noexcept override
	{
		m_probe->call();
		std::lock_guard<std::mutex> lock(m_probe->mutex);
		m_probe->callback = callback;
		m_probe->callbackContext = context;
		return m_probe->failOpen ? E_ACCESSDENIED : S_OK;
	}
	HRESULT start() noexcept override { m_probe->call(); return S_OK; }
	HRESULT stop() noexcept override { m_probe->call(); return S_OK; }
	HRESULT close() noexcept override
	{
		m_probe->call();
		std::lock_guard<std::mutex> lock(m_probe->mutex);
		m_probe->callback = nullptr;
		++m_probe->closes;
		return S_OK;
	}
	HRESULT getOutputDetails(XAudio2OutputDetails &details) const noexcept override
	{
		m_probe->call();
		details.channelCount = 2;
		details.channelMask = SPEAKER_STEREO;
		return S_OK;
	}
	HRESULT createPcmVoice(std::unique_ptr<IXAudio2PcmVoiceBackend> &voice) noexcept override
	{
		m_probe->call();
		auto probe = std::make_shared<VoiceProbe>();
		{
			std::lock_guard<std::mutex> lock(m_probe->mutex);
			probe->volumeResult = m_probe->volumeResult;
		}
		voice = std::make_unique<FakeVoice>(m_probe, probe);
		std::lock_guard<std::mutex> lock(m_probe->mutex);
		m_probe->voices.push_back(std::move(probe));
		return S_OK;
	}
private:
	std::shared_ptr<EngineProbe> m_probe;
};

AudioPcmChunk pcm(std::uint64_t generation, std::uint64_t sequence, std::uint8_t marker = 23)
{
	AudioPcmChunk chunk;
	chunk.sampleRate = 48000;
	chunk.channels = 2;
	chunk.sourceChannels = 1;
	chunk.frameCount = 64;
	chunk.startSample = static_cast<std::int64_t>(sequence) * 64;
	chunk.generation = generation;
	chunk.sequence = sequence;
	chunk.data.assign(256, marker);
	return chunk;
}

std::unique_ptr<XAudio2AudioService> service(const std::shared_ptr<EngineProbe> &probe)
{
	return std::make_unique<XAudio2AudioService>(std::make_unique<FakeEngine>(probe),
		XAudio2AudioExecutionMode::SHARED_OWNER);
}

void testProcessOwnerAndMovieRouting()
{
	auto gameProbe = std::make_shared<EngineProbe>();
	auto videoProbe = std::make_shared<EngineProbe>();
	{
		auto game = service(gameProbe);
		auto video = service(videoProbe);
		check(game->open() && video->open(), "two logical services open on the shared execution owner");
		const auto gameVoice = game->createVoice(3.5f);
		const auto videoVoice = video->createVoice();
		XAudio2MoviePcmSink movie(*game); // Intentionally shares a logical service with a game voice.
		check(movie.isReady(), "movie voice is ready on shared owner");
		game->resetVoice(gameVoice, 7);
		video->resetVoice(videoVoice, 9);
		movie.reset(11);
		check(movie.canAccept(2), "movie reset settles before the bounded first-frame decode loop");
		AudioPcmChunk owned = pcm(7, 0, 41);
		check(game->submit(gameVoice, std::move(owned)) == AudioPcmSubmitResult::ACCEPTED,
			"game PCM is moved into an owned command");
		owned.data.assign(256, 99);
		game->setVoiceVolume(gameVoice, .37f);
		game->setVoiceFrequencyRatio(gameVoice, 3.25f);
		XAudio2SpatializationPose listener, emitter;
		emitter.position[2] = 20;
		game->setVoiceSpatialization(gameVoice, listener, emitter);
		check(game->synchronize(), "pending PCM can be fenced without submitting it before controls");
		check(gameProbe->voice(0)->submitted() == 0, "native submission waits for explicit service boundary");
		check(video->submit(videoVoice, pcm(9, 0)) == AudioPcmSubmitResult::ACCEPTED
			&& movie.submit(pcm(11, 0)) == AudioPcmSubmitResult::ACCEPTED, "overlapping movie PCM is admitted");
		game->serviceVoice(gameVoice);
		video->serviceVoice(videoVoice);
		movie.setOutputGain(.6);
		movie.service();
		check(game->synchronize() && video->synchronize(), "both clients reach their native service fences");
		{
			const auto voice = gameProbe->voice(0);
			std::lock_guard<std::mutex> lock(voice->mutex);
			check(voice->firstBytes.size() == 1 && voice->firstBytes[0] == 41,
				"producer mutation does not alter moved PCM storage");
			check(voice->maximumPitch == 3.5f && voice->pitchesAtSubmit[0] == 3.25f,
				"authored pitch above two reaches source creation and first submission");
			check(voice->gainsAtSubmit[0] == .37f && voice->matrices == 1,
				"initial gain and spatial controls precede first submission");
		}
		std::thread callback([&]() {
			gameProbe->voice(0)->complete(0);
			gameProbe->voice(1)->complete(0);
			videoProbe->voice(0)->complete(0);
		});
		callback.join();
		game->synchronize();
		video->synchronize();
		movie.service(); // Must discard only its own PCM completion.
		XAudio2AudioCompletion completion;
		check(game->tryPopCompletion(completion) && completion.voice == gameVoice && completion.generation == 7,
			"movie servicing preserves the game's completion for game-owner publication");
		check(video->tryPopCompletion(completion) && completion.voice == videoVoice && completion.generation == 9,
			"another logical service retains its completion on the same execution thread");
		check(!game->tryPopCompletion(completion), "only the movie handle's completion is discarded");
		check(game->ownerMetrics().sharedOwner && !game->ownerMetrics().forcedSerial,
			"shared owner is not reported as a serial fallback");
		movie.close();
		game->shutdown();
		video->shutdown();
	}
	check(gameProbe->owner == videoProbe->owner && gameProbe->owner != std::this_thread::get_id(),
		"game and movie engines use one process-wide native thread distinct from game owner");
	check(!gameProbe->wrongThread && !videoProbe->wrongThread
		&& gameProbe->destructors == 1 && videoProbe->destructors == 1,
		"every backend operation and destructor remains on that owner");
}

void testOverlapAndBoundedPressure()
{
	auto probe = std::make_shared<EngineProbe>();
	auto audio = service(probe);
	check(audio->open(), "pressure service opens");
	const auto handle = audio->createVoice();
	audio->resetVoice(handle, 1);
	auto voice = probe->voice(0);
	voice->submitLatch.release = false;
	check(audio->submit(handle, pcm(1, 0)) == AudioPcmSubmitResult::ACCEPTED, "blocked native PCM is admitted");
	audio->serviceVoice(handle);
	check(waitUntil([&]() { return voice->submitLatch.entered.load(); }), "native owner reaches deliberately blocked backend");
	// These ordinary calls must finish while native submission is still blocked.
	check(audio->isVoiceOpen(handle) && audio->setVoiceVolume(handle, .4f),
		"cached status and asynchronous control overlap a blocked native submission");
	for (std::size_t index = 1; index < XAudio2AudioServiceOwner::MAX_COMMANDS; ++index) {
		check(index % 2 ? audio->pauseVoice(handle) : audio->resumeVoice(handle), "mandatory control enters bounded queue");
	}
	std::atomic<bool> backpressureReturned { false };
	std::thread producer([&]() {
		audio->setVoiceVolume(handle, .25f);
		backpressureReturned.store(true, std::memory_order_release);
	});
	check(waitUntil([&]() { return audio->ownerMetrics().queueWaits != 0; }), "full control queue applies observable backpressure");
	check(!backpressureReturned.load(), "mandatory control is not silently discarded under queue pressure");
	AudioPcmChunk rejected = pcm(1, 1);
	check(audio->submit(handle, std::move(rejected)) == AudioPcmSubmitResult::DROPPED && rejected.data.empty(),
		"bounded submission rejects and consumes input when its queue is full");
	AudioPcmChunk retained = pcm(1, 2, 91);
	const std::vector<std::uint8_t> retainedBytes = retained.data;
	check(audio->submitRetained(handle, retained) == AudioPcmSubmitResult::DROPPED
		&& retained.data == retainedBytes && retained.sequence == 2,
		"retryable bounded submission retains PCM and sequence when its queue is full");
	voice->submitLatch.release = true;
	producer.join();
	check(audio->synchronize(), "pressure queue drains to its fence");
	const auto metrics = audio->ownerMetrics();
	check(metrics.peakQueuedCommands == XAudio2AudioServiceOwner::MAX_COMMANDS
		&& metrics.queueWaitNanoseconds != 0 && metrics.rejectedSubmissions == 2,
		"queue saturation, wait cost and both rejection paths are measured");
	{
		std::lock_guard<std::mutex> lock(voice->mutex);
		check(voice->gain == .25f, "backpressured final gain is eventually applied in order");
	}
	voice->complete(0);
	// The alternating saturation controls end paused. Resume explicitly so
	// retained-submit checks exercise playback, not intentional pause behavior.
	check(audio->resumeVoice(handle) && audio->synchronize(), "pressure voice resumes before retained PCM retry");
	check(audio->submitRetained(handle, retained) == AudioPcmSubmitResult::ACCEPTED
		&& retained.data.empty(), "retained PCM is accepted after owner progress without re-decoding");
	audio->serviceVoice(handle);
	check(audio->synchronize(), "retained PCM reaches the native voice after retry");
	{
		std::lock_guard<std::mutex> lock(voice->mutex);
		check(voice->firstBytes.size() >= 2 && voice->firstBytes[1] == 91,
			"retry submits the original retained PCM bytes");
	}
	voice->complete(1);
	audio->synchronize();
	audio->discardCompletions();
	for (std::size_t index = 0; index < XAudio2PcmVoice::SLOT_COUNT; ++index)
		check(audio->submit(handle, pcm(1, index + 1)) == AudioPcmSubmitResult::ACCEPTED, "voice reserves one bounded PCM slot");
	check(!audio->canVoiceAccept(handle, 1)
		&& audio->submit(handle, pcm(1, 20)) == AudioPcmSubmitResult::DROPPED,
		"command-side reservations cannot overbook the native eight-slot voice");
	audio->shutdown();
	AudioPcmChunk terminal = pcm(1, 21);
	check(audio->submitRetained(handle, terminal) == AudioPcmSubmitResult::FAILED
		&& terminal.data.empty(), "terminal service failure consumes retained PCM and fails playback");
}

void testOptionalVolumeAndDestroyAdmission()
{
	auto optionalProbe = std::make_shared<EngineProbe>();
	optionalProbe->volumeResult = E_NOTIMPL;
	auto optional = service(optionalProbe);
	check(optional->open(), "optional-volume service opens");
	const auto optionalHandle = optional->createVoice();
	auto optionalVoice = optionalProbe->voice(0);
	{
		std::lock_guard<std::mutex> lock(optionalVoice->mutex);
		check(optionalHandle.isValid() && optionalVoice->volumeCalls == 1,
			"optional volume capability is probed once during voice creation");
	}
	check(!optional->setVoiceVolume(optionalHandle, .5f) && !optional->isVoiceFailed(optionalHandle),
		"unsupported native volume is rejected without failing the voice");
	{
		std::lock_guard<std::mutex> lock(optionalVoice->mutex);
		check(optionalVoice->volumeCalls == 1,
			"unsupported volume does not perform a synchronous native call per gain update");
	}
	optional->shutdown();

	auto failureProbe = std::make_shared<EngineProbe>();
	auto failure = service(failureProbe);
	check(failure->open(), "volume-failure service opens");
	const auto failureHandle = failure->createVoice();
	auto failureVoice = failureProbe->voice(0);
	{
		std::lock_guard<std::mutex> lock(failureVoice->mutex);
		failureVoice->volumeResult = E_ACCESSDENIED;
	}
	check(failure->setVoiceVolume(failureHandle, .5f),
		"supported volume remains admitted before asynchronous backend failure");
	check(failure->synchronize() && failure->isVoiceFailed(failureHandle)
		&& failure->getVoiceLastError(failureHandle) == E_ACCESSDENIED,
		"a real native volume failure remains terminal and observable");
	{
		std::lock_guard<std::mutex> lock(failureVoice->mutex);
		check(failureVoice->volumeCalls == 2,
			"real volume failure follows the one creation probe with one queued call");
	}
	failure->shutdown();

	auto destroyProbe = std::make_shared<EngineProbe>();
	auto destroy = service(destroyProbe);
	check(destroy->open(), "destroy-admission service opens");
	const auto destroyHandle = destroy->createVoice();
	auto destroyVoice = destroyProbe->voice(0);
	check(destroy->setVoiceVolume(destroyHandle, .6f),
		"control preceding destroy is admitted into the FIFO");
	destroyVoice->destroyLatch.release = false;
	std::atomic<bool> destroyResult { false };
	std::thread destroyer([&]() {
		destroyResult.store(destroy->destroyVoice(destroyHandle), std::memory_order_release);
	});
	check(waitUntil([&]() { return destroyVoice->destroyEntered.load(std::memory_order_acquire); }),
		"destroy admission reaches the deliberately gated native backend");
	AudioPcmChunk rejected = pcm(0, 0);
	check(destroy->submit(destroyHandle, std::move(rejected)) == AudioPcmSubmitResult::DROPPED
		&& rejected.data.empty(), "PCM after destroy admission is rejected and consumed");
	check(!destroy->setVoiceVolume(destroyHandle, .25f),
		"control after destroy admission is rejected without reaching the FIFO");
	check(!destroy->destroyVoice(destroyHandle),
		"a second destroy is rejected while the first destroy is pending");
	destroyVoice->destroyLatch.release = true;
	destroyer.join();
	check(destroyResult.load(std::memory_order_acquire) && destroyVoice->destroyed
		&& !destroy->isVoiceOpen(destroyHandle),
		"the admitted destroy completes and invalidates its handle");
	{
		std::lock_guard<std::mutex> lock(destroyVoice->mutex);
		check(destroyVoice->volumeCalls == 2 && destroyVoice->gain == .6f,
			"commands admitted before destroy retain FIFO order and execute");
	}
	destroy->shutdown();
}

void testPauseGainGenerationsAndFailure()
{
	auto probe = std::make_shared<EngineProbe>();
	auto audio = service(probe);
	check(audio->open(), "controls service opens");
	const auto handle = audio->createVoice();
	auto voice = probe->voice(0);
	audio->resetVoice(handle, 3);
	audio->pauseVoice(handle);
	audio->submit(handle, pcm(3, 0));
	audio->setVoiceVolume(handle, 0);
	audio->serviceVoice(handle);
	audio->synchronize();
	check(voice->submitted() == 0, "pause before first PCM remains persistent across service passes");
	audio->resumeVoice(handle);
	audio->synchronize();
	check(voice->submitted() == 1, "resume admits the paused PCM");
	{
		std::lock_guard<std::mutex> lock(voice->mutex);
		check(voice->gainsAtSubmit[0] == 0, "mute reaches queued PCM before it starts");
	}
	check(audio->resetVoice(handle, 4), "new generation is admitted");
	check(audio->submit(handle, pcm(3, 1)) == AudioPcmSubmitResult::DROPPED,
		"old producer generation is rejected before native execution");
	check(audio->submit(handle, pcm(4, 0)) == AudioPcmSubmitResult::ACCEPTED,
		"new generation owns separate PCM while the old callback drains");
	audio->setVoiceVolume(handle, .5f);
	audio->serviceVoice(handle);
	voice->complete(0);
	audio->synchronize();
	check(voice->submitted() == 2, "new generation begins after cancelled callback quiescence");
	std::int64_t sample = -1;
	check(!audio->getVoicePlayedSample(handle, sample), "cancelled generation cannot publish the new playback clock");
	voice->complete(1);
	audio->synchronize();
	check(audio->getVoicePlayedSample(handle, sample) && sample == 64, "only current generation advances playback clock");
	check(audio->destroyVoice(handle) && !audio->isVoiceOpen(handle), "destroy fence invalidates the old handle");
	const auto replacement = audio->createVoice();
	check(replacement.index == handle.index && replacement.generation != handle.generation
		&& !audio->setVoiceVolume(handle, 1), "reused voice slots reject stale handles");
	auto failingVoice = probe->voice(1);
	failingVoice->failSubmit = true;
	audio->resetVoice(replacement, 5);
	check(audio->submit(replacement, pcm(5, 0)) == AudioPcmSubmitResult::ACCEPTED, "asynchronous failure starts from accepted PCM");
	audio->serviceVoice(replacement);
	audio->synchronize();
	check(audio->isVoiceFailed(replacement) && audio->getVoiceLastError(replacement) == E_ACCESSDENIED,
		"native submission failure is published terminally instead of losing accepted PCM silently");
	probe->critical(E_ABORT);
	audio->synchronize();
	check(audio->state() == XAudio2AudioServiceState::FAILED && audio->processPendingFailure()
		&& !audio->processPendingFailure(), "engine failure is observed once by the game-side client");
	audio->shutdown();
	check(audio->open(), "fully quiesced failed service can reopen on the same owner");
	check(!audio->isVoiceOpen(replacement), "reopen never revives stale voice handles");
}

void testCallbackShutdownAndOpenFailure()
{
	auto probe = std::make_shared<EngineProbe>();
	auto audio = service(probe);
	check(audio->open(), "callback-shutdown service opens");
	const auto handle = audio->createVoice();
	audio->resetVoice(handle, 1);
	audio->submit(handle, pcm(1, 0));
	audio->serviceVoice(handle);
	audio->synchronize();
	auto voice = probe->voice(0);
	Latch callbackLatch;
	callbackLatch.release = false;
	std::thread callback([&]() { voice->complete(0, &callbackLatch); });
	check(waitUntil([&]() { return callbackLatch.entered.load(); }), "callback is in flight before teardown");
	std::atomic<bool> shutdownFinished { false };
	std::thread closer([&]() { audio->shutdown(); shutdownFinished = true; });
	check(waitUntil([&]() { return voice->destroyEntered.load(); }), "owner enters native callback-quiescing destroy");
	check(!shutdownFinished.load(), "shutdown fence waits for callback quiescence");
	callbackLatch.release = true;
	callback.join();
	closer.join();
	check(shutdownFinished && voice->destroyed && audio->state() == XAudio2AudioServiceState::CLOSED,
		"callback completes before owned memory and handles are released");
	voice->complete(0);
	XAudio2AudioCompletion completion;
	check(!audio->tryPopCompletion(completion), "quiesced callback cannot resurrect a closed completion");
	audio.reset();
	check(!probe->wrongThread && probe->destructors == 1, "failure-safe native destruction is on service owner");
	probe = std::make_shared<EngineProbe>();
	probe->failOpen = true;
	audio = service(probe);
	check(!audio->open() && audio->getLastError() == E_ACCESSDENIED
		&& audio->state() == XAudio2AudioServiceState::CLOSED, "open failure retains original error and closed state");
	check(probe->closes == 1, "partially open native backend is unwound once");
}

void testPcmByteBudgetAndCompletionBackpressure()
{
	auto probe = std::make_shared<EngineProbe>();
	auto audio = service(probe);
	check(audio->open(), "byte-budget service opens");
	std::vector<XAudio2PcmVoiceHandle> handles;
	for (int index = 0; index < 22; ++index) {
		const auto handle = audio->createVoice();
		check(handle.isValid(), "budget voice is allocated on owner");
		audio->resetVoice(handle, 1);
		handles.push_back(handle);
	}
	std::size_t accepted = 0;
	for (const auto handle : handles) {
		for (std::size_t index = 0; index < XAudio2PcmVoice::SLOT_COUNT; ++index) {
			AudioPcmChunk chunk = pcm(1, index);
			chunk.frameCount = 48000;
			chunk.data.assign(48000 * 4, 17);
			if (audio->submit(handle, std::move(chunk)) == AudioPcmSubmitResult::ACCEPTED) ++accepted;
		}
	}
	check(accepted == XAudio2AudioServiceOwner::MAX_PCM_BYTES / (48000 * 4),
		"PCM byte budget includes both queued and native-owned pending chunks");
	check(audio->ownerMetrics().peakBufferedBytes <= XAudio2AudioServiceOwner::MAX_PCM_BYTES,
		"measured owned PCM never exceeds configured byte bound");
	audio->shutdown();
	check(audio->open(), "byte-budget teardown releases all pending chunks");
	const auto handle = audio->createVoice();
	auto voice = probe->voice(22);
	audio->resetVoice(handle, 2);
	for (std::size_t batch = 0; batch < 4; ++batch) {
		for (std::size_t index = 0; index < 8; ++index)
			check(audio->submit(handle, pcm(2, batch * 8 + index)) == AudioPcmSubmitResult::ACCEPTED,
				"bounded completion mailbox reserves capacity before producer advances");
		audio->setVoiceVolume(handle, 1.0f - static_cast<float>(batch) * .2f);
		audio->serviceVoice(handle);
		audio->synchronize();
		for (std::size_t index = 0; index < 8; ++index) voice->complete(batch * 8 + index);
		audio->synchronize();
	}
	check(!audio->canVoiceAccept(handle, 1)
		&& audio->submit(handle, pcm(2, 32)) == AudioPcmSubmitResult::DROPPED,
		"unconsumed completion mailbox backpressures future PCM without losing completions");
	XAudio2AudioCompletion completion;
	std::size_t completed = 0;
	while (audio->tryPopCompletion(handle, completion)) {
		check(completion.generation == 2 && completion.sequence == completed,
			"multi-buffer completion publication retains generation and sequence through gain changes");
		++completed;
	}
	check(completed == 32 && audio->canVoiceAccept(handle, 1),
		"game-owner completion consumption releases capacity for another music phase or loop");
	check(audio->submit(handle, pcm(2, 32)) == AudioPcmSubmitResult::ACCEPTED,
		"buffer completion is not treated as stream EOS and the same voice can continue");
	audio->serviceVoice(handle);
	audio->synchronize();
	{
		std::lock_guard<std::mutex> lock(voice->mutex);
		check(voice->gainsAtSubmit.size() == 33
			&& std::abs(voice->gainsAtSubmit[0] - 1.0f) < .0001f
			&& std::abs(voice->gainsAtSubmit[32] - .4f) < .0001f,
			"owner keeps authored fade gains across multiple completion/replenishment batches");
	}
}

void testExplicitReferenceAndServiceBound()
{
	auto probe = std::make_shared<EngineProbe>();
	XAudio2AudioService reference(std::make_unique<FakeEngine>(probe));
	check(reference.open() && reference.ownerMetrics().forcedSerial && !reference.ownerMetrics().sharedOwner,
		"injected serial reference is explicit in metrics");
	check(probe->owner == std::this_thread::get_id(), "reference lane retains deterministic caller execution");
	reference.shutdown();
	std::vector<std::unique_ptr<XAudio2AudioService>> services;
	for (std::size_t index = 0; index < XAudio2AudioServiceOwner::MAX_SERVICES; ++index) {
		auto current = service(std::make_shared<EngineProbe>());
		check(current->open(), "bounded logical service slot opens");
		services.push_back(std::move(current));
	}
	auto excessProbe = std::make_shared<EngineProbe>();
	auto excess = service(excessProbe);
	check(!excess->open() && excess->getLastError() == E_OUTOFMEMORY,
		"logical service saturation is explicit rather than creating another thread");
	check(!excess->ownerMetrics().sharedOwner && !excess->ownerMetrics().forcedSerial
		&& excessProbe->owner == std::thread::id() && excessProbe->closes.load() == 0,
		"failed owner admission is not reported as shared and makes no native calls");
}
}

int main()
{
	testProcessOwnerAndMovieRouting();
	testOverlapAndBoundedPressure();
	testOptionalVolumeAndDestroyAdmission();
	testPauseGainGenerationsAndFailure();
	testCallbackShutdownAndOpenFailure();
	testPcmByteBudgetAndCompletionBackpressure();
	testExplicitReferenceAndServiceBound();
	if (failures != 0) { std::fprintf(stderr, "%d audio owner checks failed\n", failures); return 1; }
	std::puts("XAudio2 shared execution-owner checks passed");
	return 0;
}
