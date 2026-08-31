#include "XAudio2AudioDevice/XAudio2PcmVoice.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <thread>
#include <atomic>
#include <utility>
#include <vector>

namespace
{
int g_failures = 0;

void check(bool condition, const char *message)
{
	if (!condition) {
		std::fprintf(stderr, "FAIL: %s\n", message);
		++g_failures;
	}
}

struct Submission
{
	const XAUDIO2_BUFFER *buffer;
	const BYTE *audio;
	UINT32 bytes;
	void *context;
};

class FakePcmVoiceBackend final : public IXAudio2PcmVoiceBackend
{
public:
	HRESULT createResult = S_OK;
	HRESULT submitResult = S_OK;
	HRESULT startResult = S_OK;
	HRESULT stopResult = S_OK;
	HRESULT flushResult = S_OK;
	HRESULT destroyResult = S_OK;
	HRESULT criticalError = S_OK;
	HRESULT setVolumeResult = S_OK;
	int createCalls = 0;
	int submitCalls = 0;
	int startCalls = 0;
	int stopCalls = 0;
	int flushCalls = 0;
	int destroyCalls = 0;
	int criticalErrorCalls = 0;
	int setVolumeCalls = 0;
	float lastVolume = -1.0f;
	WAVEFORMATEX createFormat = {};
	IXAudio2VoiceCallback *callback = nullptr;
	std::vector<Submission> submissions;
	std::vector<std::string> callLog;

	HRESULT create(const WAVEFORMATEX &format, IXAudio2VoiceCallback *voiceCallback) noexcept override
	{
		++createCalls;
		callLog.emplace_back("create");
		createFormat = format;
		callback = voiceCallback;
		return createResult;
	}

	HRESULT submit(const XAUDIO2_BUFFER &buffer) noexcept override
	{
		++submitCalls;
		callLog.emplace_back("submit");
		submissions.push_back(Submission { &buffer, buffer.pAudioData, buffer.AudioBytes, buffer.pContext });
		return submitResult;
	}

	HRESULT start() noexcept override
	{
		++startCalls;
		callLog.emplace_back("start");
		return startResult;
	}

	HRESULT stop() noexcept override
	{
		++stopCalls;
		callLog.emplace_back("stop");
		return stopResult;
	}

	HRESULT flush() noexcept override
	{
		++flushCalls;
		callLog.emplace_back("flush");
		return flushResult;
	}

	HRESULT getCriticalError() const noexcept override
	{
		const_cast<FakePcmVoiceBackend *>(this)->criticalErrorCalls++;
		return criticalError;
	}

	HRESULT setVolume(float volume) noexcept override
	{
		++setVolumeCalls;
		lastVolume = volume;
		return setVolumeResult;
	}

	void destroy() noexcept override
	{
		++destroyCalls;
		callLog.emplace_back("destroy");
		callback = nullptr;
	}
	HRESULT destroyWithResult() noexcept override
	{
		++destroyCalls;
		callLog.emplace_back("destroy-result");
		callback = nullptr;
		return destroyResult;
	}

	void complete(std::size_t index)
	{
		check(index < submissions.size(), "fake completion index is valid");
		if (index < submissions.size() && callback != nullptr) {
			callback->OnBufferEnd(submissions[index].context);
		}
	}

	void voiceError(HRESULT error)
	{
		check(callback != nullptr, "fake voice callback is registered");
		if (callback != nullptr) {
			callback->OnVoiceError(nullptr, error);
		}
	}
};

AudioPcmChunk makeChunk(std::uint64_t generation, std::uint64_t sequence, std::uint8_t marker,
	std::uint32_t frameCount = 2)
{
	AudioPcmChunk chunk;
	chunk.sampleRate = 48000;
	chunk.channels = 2;
	chunk.format = AudioPcmFormat::SIGNED_16_INTERLEAVED_LITTLE_ENDIAN;
	chunk.frameCount = frameCount;
	chunk.startSample = static_cast<std::int64_t>(sequence * frameCount);
	chunk.generation = generation;
	chunk.sequence = sequence;
	chunk.data.resize(static_cast<std::size_t>(frameCount) * 4);
	for (std::size_t i = 0; i < chunk.data.size(); ++i) {
		chunk.data[i] = static_cast<std::uint8_t>(marker + i);
	}
	return chunk;
}

std::size_t firstCall(const std::vector<std::string> &calls, const char *name)
{
	const auto it = std::find(calls.begin(), calls.end(), name);
	return it == calls.end() ? calls.size() : static_cast<std::size_t>(it - calls.begin());
}

void testFixedFormatAndBoundedAdmission()
{
	FakePcmVoiceBackend backend;
	XAudio2PcmVoice voice(backend);
	check(voice.open(), "voice opens through the injected backend");
	check(backend.createCalls == 1, "open creates exactly one source voice");
	check(backend.createFormat.wFormatTag == WAVE_FORMAT_PCM, "source voice uses PCM format");
	check(backend.createFormat.nChannels == 2, "source voice format is stereo");
	check(backend.createFormat.nSamplesPerSec == 48000, "source voice format is 48 kHz");
	check(backend.createFormat.nAvgBytesPerSec == 48000 * 2 * sizeof(std::int16_t),
		"source voice format has exact byte rate");
	check(backend.createFormat.nBlockAlign == 2 * sizeof(std::int16_t),
		"source voice format has exact block alignment");
	check(backend.createFormat.wBitsPerSample == 16, "source voice format is signed 16-bit PCM");
	check(backend.createFormat.cbSize == 0, "source voice format has no extension bytes");
	check(voice.canAccept(XAudio2PcmVoice::SLOT_COUNT),
		"a new voice advertises all fixed admission slots");
	check(voice.setVolume(0.4f) && backend.setVolumeCalls == 1 && backend.lastVolume == 0.4f,
		"native output gain reaches the already-created source voice");

	for (std::uint64_t sequence = 0; sequence < XAudio2PcmVoice::SLOT_COUNT; ++sequence) {
		check(voice.submit(makeChunk(0, sequence, static_cast<std::uint8_t>(sequence)))
				== AudioPcmSubmitResult::ACCEPTED,
			"the eight fixed slots admit valid chunks");
	}
	check(!voice.canAccept(1), "a full voice publishes backpressure before input is consumed");
	check(voice.submit(makeChunk(0, XAudio2PcmVoice::SLOT_COUNT, 100)) == AudioPcmSubmitResult::DROPPED,
		"the ninth chunk is dropped when all slots are pending");
	check(backend.submitCalls == 0 && backend.criticalErrorCalls == 0,
		"submit never calls the backend, including its critical-error query");

	voice.service();
	check(backend.submitCalls == XAudio2PcmVoice::SLOT_COUNT,
		"service submits every admitted chunk");
	check(backend.startCalls == 1, "service starts the source voice after the first submission");
	for (std::size_t i = 0; i < backend.submissions.size(); ++i) {
		check(backend.submissions[i].audio[0] == static_cast<BYTE>(i),
			"service submits pending chunks in sequence order");
		check(backend.submissions[i].bytes == 8, "submitted byte count matches owned PCM data");
	}
	voice.close();

	FakePcmVoiceBackend unsupportedGainBackend;
	unsupportedGainBackend.setVolumeResult = E_NOTIMPL;
	XAudio2PcmVoice unsupportedGainVoice(unsupportedGainBackend);
	check(unsupportedGainVoice.open() && !unsupportedGainVoice.setVolume(0.5f)
			&& !unsupportedGainVoice.isFailed(),
		"an unsupported native gain hook falls back without failing the voice");
	unsupportedGainVoice.close();
}

void testOwnedStorageAndOwnerOnlyReclaim()
{
	FakePcmVoiceBackend backend;
	XAudio2PcmVoice voice(backend);
	check(voice.open(), "storage test voice opens");
	AudioPcmChunk first = makeChunk(0, 0, 37);
	const std::vector<std::uint8_t> expected = first.data;
	check(voice.submit(std::move(first)) == AudioPcmSubmitResult::ACCEPTED,
		"storage test admits its first chunk");
	for (std::uint64_t sequence = 1; sequence < XAudio2PcmVoice::SLOT_COUNT; ++sequence) {
		check(voice.submit(makeChunk(0, sequence, static_cast<std::uint8_t>(37 + sequence)))
				== AudioPcmSubmitResult::ACCEPTED,
			"storage test fills every remaining slot");
	}
	voice.service();
	check(backend.submissions.size() == XAudio2PcmVoice::SLOT_COUNT,
		"storage test submits every filled slot");
	const Submission submitted = backend.submissions.front();
	check(submitted.buffer->pAudioData == submitted.audio, "buffer points at its owned stable PCM bytes");
	check(std::memcmp(submitted.audio, expected.data(), expected.size()) == 0,
		"owned PCM bytes remain unchanged after admission");
	voice.service();
	check(submitted.buffer->pAudioData == submitted.audio &&
			std::memcmp(submitted.audio, expected.data(), expected.size()) == 0,
		"repeated service does not reclaim a slot before its callback");

	backend.complete(0);
	check(!voice.canAccept(1), "callback completion is not producer capacity until owner service");
	check(voice.submit(makeChunk(0, 1, 55)) == AudioPcmSubmitResult::DROPPED,
		"callback completion alone does not reclaim a slot");
	voice.service();
	check(voice.canAccept(1), "owner service republishes a reclaimed admission slot");
	check(voice.submit(makeChunk(0, 1, 55)) == AudioPcmSubmitResult::ACCEPTED,
		"the owner service reclaims a callback-complete slot");
	voice.close();
}

void testValidationAndClosedAdmission()
{
	FakePcmVoiceBackend backend;
	XAudio2PcmVoice voice(backend);
	check(voice.open(), "validation test voice opens");

	AudioPcmChunk invalid = makeChunk(0, 0, 1);
	invalid.sampleRate = 44100;
	check(voice.submit(std::move(invalid)) == AudioPcmSubmitResult::DROPPED,
		"wrong sample rate is dropped");
	invalid = makeChunk(0, 0, 1);
	invalid.channels = 1;
	check(voice.submit(std::move(invalid)) == AudioPcmSubmitResult::DROPPED,
		"wrong channel count is dropped");
	invalid = makeChunk(0, 0, 1);
	invalid.format = static_cast<AudioPcmFormat>(99);
	check(voice.submit(std::move(invalid)) == AudioPcmSubmitResult::DROPPED,
		"wrong PCM format is dropped");
	invalid = makeChunk(0, 0, 1, 0);
	check(voice.submit(std::move(invalid)) == AudioPcmSubmitResult::DROPPED,
		"empty chunks are dropped");
	invalid = makeChunk(0, 0, 1);
	invalid.startSample = -1;
	check(voice.submit(std::move(invalid)) == AudioPcmSubmitResult::DROPPED,
		"negative start samples are dropped");
	invalid = makeChunk(0, 0, 1);
	invalid.data.pop_back();
	check(voice.submit(std::move(invalid)) == AudioPcmSubmitResult::DROPPED,
		"inexact PCM byte counts are dropped");
	invalid = makeChunk(0, 0, 1, 48001);
	check(voice.submit(std::move(invalid)) == AudioPcmSubmitResult::DROPPED,
		"chunks over the decoder bound are dropped");
	check(voice.submit(makeChunk(1, 0, 2)) == AudioPcmSubmitResult::DROPPED,
		"stale generations are dropped");
	invalid = makeChunk(0, 0, 2);
	invalid.startSample = (std::numeric_limits<std::int64_t>::max)();
	check(voice.submit(std::move(invalid)) == AudioPcmSubmitResult::DROPPED,
		"chunks whose absolute end sample overflows are dropped");
	check(backend.submitCalls == 0 && backend.criticalErrorCalls == 0,
		"invalid and stale submissions do not call the backend");

	voice.close();
	check(voice.submit(makeChunk(0, 0, 3)) == AudioPcmSubmitResult::DROPPED,
		"closed voices drop submissions");
	check(backend.submitCalls == 0, "closed submissions do not call the backend");
}

void testPlayedSampleClockAndGeneration()
{
	FakePcmVoiceBackend backend;
	XAudio2PcmVoice voice(backend);
	check(voice.open(), "clock test voice opens");
	std::int64_t playedSample = 0;
	check(!voice.getPlayedSample(playedSample), "clock is unavailable before a buffer completes");
	check(voice.submit(makeChunk(0, 0, 7)) == AudioPcmSubmitResult::ACCEPTED,
		"clock test admits its buffer");
	voice.service();
	check(!voice.getPlayedSample(playedSample), "clock remains unavailable while the buffer is queued");
	backend.complete(0);
	check(voice.getPlayedSample(playedSample) && playedSample == 2,
		"buffer completion publishes the played sample position");
	voice.reset(1);
	check(!voice.getPlayedSample(playedSample), "generation reset rebases the played sample clock");
	voice.close();
}

void testPauseBeforeFirstServiceAndPendingRefill()
{
	FakePcmVoiceBackend backend;
	XAudio2PcmVoice voice(backend);
	check(voice.open(), "pause-before-service voice opens");
	check(voice.submit(makeChunk(0, 0, 70)) == AudioPcmSubmitResult::ACCEPTED,
		"pause-before-service voice admits its first pending chunk");
	check(voice.pause(), "voice pauses before its first native submission");
	voice.service();
	voice.service();
	check(backend.submitCalls == 0 && backend.startCalls == 0,
		"repeated service neither submits nor starts a paused pending voice");
	check(voice.resume(), "pending voice resumes explicitly");
	voice.service();
	voice.service();
	check(backend.submitCalls == 1 && backend.startCalls == 1,
		"resume and service start the first pending chunk exactly once");

	check(voice.submit(makeChunk(0, 1, 71)) == AudioPcmSubmitResult::ACCEPTED,
		"running voice admits a pending refill before pause");
	check(voice.pause(), "running voice pauses with a pending refill");
	backend.complete(0);
	voice.service();
	check(backend.submitCalls == 1 && backend.startCalls == 1,
		"reclaiming an in-flight completion does not start a paused refill");
	check(voice.canAccept(XAudio2PcmVoice::SLOT_COUNT - 1),
		"paused service still reclaims completed storage");
	check(voice.resume(), "paused refill resumes explicitly");
	voice.service();
	check(backend.submitCalls == 2 && backend.startCalls == 2,
		"resuming the refill issues only one additional native start");
	voice.close();
}

void testPauseSurvivesGenerationReset()
{
	FakePcmVoiceBackend backend;
	XAudio2PcmVoice voice(backend);
	check(voice.open() && voice.pause(), "generation-reset voice opens paused");
	check(voice.submit(makeChunk(0, 0, 72)) == AudioPcmSubmitResult::ACCEPTED,
		"paused voice admits old pending data");
	voice.reset(1);
	check(voice.submit(makeChunk(1, 0, 73)) == AudioPcmSubmitResult::ACCEPTED,
		"paused reset admits new-generation pending data");
	voice.service();
	voice.service();
	check(backend.flushCalls == 1 && backend.submitCalls == 0 && backend.startCalls == 0,
		"reset completes its empty old-generation barrier without unpausing");
	check(voice.submit(makeChunk(0, 1, 74)) == AudioPcmSubmitResult::DROPPED,
		"paused reset still rejects old-generation admission");
	check(voice.resume(), "new generation resumes explicitly");
	voice.service();
	check(backend.submitCalls == 1 && backend.startCalls == 1
		&& !backend.submissions.empty() && backend.submissions.front().audio[0] == 73,
		"resumed reset plays only the retained new-generation data");
	voice.close();
}

void testStopPreventsPendingRestartAndCloseClearsPause()
{
	FakePcmVoiceBackend backend;
	XAudio2PcmVoice voice(backend);
	check(voice.open(), "stop-before-service voice opens");
	check(voice.submit(makeChunk(0, 0, 75)) == AudioPcmSubmitResult::ACCEPTED,
		"stop-before-service voice admits pending data");
	check(voice.stop(), "pending voice stops before its native submission");
	voice.service();
	voice.service();
	check(backend.submitCalls == 0 && backend.startCalls == 0,
		"owner service cannot restart a stopped pending voice");
	voice.close();
	voice.service();
	check(backend.destroyCalls == 1 && backend.submitCalls == 0,
		"closing a stopped voice cancels pending data without submitting it");
	check(voice.open(), "stopped voice can open a fresh lifecycle");
	check(voice.submit(makeChunk(0, 0, 76)) == AudioPcmSubmitResult::ACCEPTED,
		"fresh lifecycle admits data without inheriting stop state");
	voice.service();
	check(backend.submitCalls == 1 && backend.startCalls == 1
		&& !backend.submissions.empty() && backend.submissions.front().audio[0] == 76,
		"reopened voice starts only fresh data without an explicit resume");
	voice.close();
}

void testPlayedSampleClockIgnoresTimelineGaps()
{
	FakePcmVoiceBackend backend;
	XAudio2PcmVoice voice(backend);
	check(voice.open(), "discontinuity clock voice opens");
	AudioPcmChunk first = makeChunk(0, 0, 10);
	AudioPcmChunk afterGap = makeChunk(0, 1, 20);
	afterGap.startSample = 1000;
	afterGap.discontinuity = true;
	check(voice.submit(std::move(first)) == AudioPcmSubmitResult::ACCEPTED
			&& voice.submit(std::move(afterGap)) == AudioPcmSubmitResult::ACCEPTED,
		"discontinuity clock admits both chunks");
	voice.service();
	backend.complete(0);
	backend.complete(1);
	std::int64_t playedSample = -1;
	check(voice.getPlayedSample(playedSample) && playedSample == 4,
		"played clock counts audible frames instead of absolute timeline gaps");
	voice.close();
}

void testSameGenerationResetRejectsStaleClockCompletion()
{
	FakePcmVoiceBackend backend;
	XAudio2PcmVoice voice(backend);
	check(voice.open(), "same-generation clock reset voice opens");
	check(voice.submit(makeChunk(0, 0, 30)) == AudioPcmSubmitResult::ACCEPTED,
		"same-generation clock reset admits the old chunk");
	voice.service();
	voice.reset(0);
	backend.complete(0);
	std::int64_t playedSample = -1;
	check(!voice.getPlayedSample(playedSample),
		"same-generation reset keeps a stale completion out of the rebased clock");
	voice.service();
	check(!voice.getPlayedSample(playedSample),
		"clock stays unavailable until new-generation audio completes");
	voice.close();
}

void testResetBarrierAndGenerationActivation()
{
	FakePcmVoiceBackend backend;
	XAudio2PcmVoice voice(backend);
	check(voice.open(), "reset test voice opens");
	check(voice.submit(makeChunk(0, 0, 10)) == AudioPcmSubmitResult::ACCEPTED,
		"old submitted chunk is admitted");
	voice.service();
	check(backend.submissions.size() == 1, "old chunk reaches the backend");
	check(voice.submit(makeChunk(0, 1, 11)) == AudioPcmSubmitResult::ACCEPTED,
		"old pending chunk is admitted before reset");
	voice.reset(1);
	check(voice.submit(makeChunk(1, 0, 20)) == AudioPcmSubmitResult::ACCEPTED,
		"new generation chunk is admitted during the reset barrier");
	check(voice.submit(makeChunk(1, 1, 21)) == AudioPcmSubmitResult::ACCEPTED,
		"multiple new generation chunks remain pending during the reset barrier");
	voice.reset(1);
	voice.service();
	check(backend.submissions.size() == 1,
		"new generation chunks do not reach the backend before old completion");
	check(backend.stopCalls == 0 && backend.flushCalls == 0,
		"reset waits for every submitted old callback before its barrier");

	backend.complete(0);
	voice.service();
	check(backend.submissions.size() == 3,
		"new generation chunks reach the backend after the old callback barrier");
	check(backend.submissions[1].audio[0] == 20 && backend.submissions[2].audio[0] == 21,
		"reset submits only the new generation after discarding old pending data");
	check(backend.stopCalls == 1 && backend.flushCalls == 1,
		"reset requests one owner-side stop and flush barrier");
	const std::size_t stop = firstCall(backend.callLog, "stop");
	const std::size_t flush = firstCall(backend.callLog, "flush");
	const std::size_t newSubmit = backend.callLog.size() > 0
		? static_cast<std::size_t>(std::find(backend.callLog.begin() + flush + 1, backend.callLog.end(), "submit")
			- backend.callLog.begin())
		: backend.callLog.size();
	check(stop < flush && flush < newSubmit, "stop and flush complete before new submissions");
	check(backend.startCalls == 2, "the new generation starts after its first submission");
	voice.close();
}

void testTerminalFailures()
{
	{
		FakePcmVoiceBackend backend;
		backend.submitResult = E_FAIL;
		XAudio2PcmVoice voice(backend);
		check(voice.open(), "submit failure voice opens");
		voice.submit(makeChunk(0, 0, 30));
		voice.service();
		check(voice.isFailed(), "submit failure enters terminal state");
		check(backend.startCalls == 0, "failed submit does not start the source voice");
		check(voice.submit(makeChunk(0, 1, 31)) == AudioPcmSubmitResult::FAILED,
			"failed voices report terminal failure for later submissions");
		voice.close();
		check(backend.destroyCalls == 1, "submit failure cleanup destroys the voice once");
	}
	{
		FakePcmVoiceBackend backend;
		backend.startResult = E_FAIL;
		XAudio2PcmVoice voice(backend);
		check(voice.open(), "start failure voice opens");
		voice.submit(makeChunk(0, 0, 32));
		voice.service();
		check(voice.isFailed(), "start failure enters terminal state");
		voice.close();
	}
	{
		FakePcmVoiceBackend backend;
		backend.stopResult = E_FAIL;
		XAudio2PcmVoice voice(backend);
		check(voice.open(), "stop failure voice opens");
		voice.submit(makeChunk(0, 0, 33));
		voice.service();
		voice.reset(1);
		voice.submit(makeChunk(1, 0, 34));
		backend.complete(0);
		voice.service();
		check(voice.isFailed(), "stop failure enters terminal state");
		check(backend.submissions.size() == 1, "stop failure blocks new generation submission");
		voice.close();
	}
	{
		FakePcmVoiceBackend backend;
		backend.flushResult = E_FAIL;
		XAudio2PcmVoice voice(backend);
		check(voice.open(), "flush failure voice opens");
		voice.submit(makeChunk(0, 0, 35));
		voice.service();
		voice.reset(1);
		voice.submit(makeChunk(1, 0, 36));
		backend.complete(0);
		voice.service();
		check(voice.isFailed(), "flush failure enters terminal state");
		check(backend.submissions.size() == 1, "flush failure blocks new generation submission");
		voice.close();
	}
	{
		FakePcmVoiceBackend backend;
		XAudio2PcmVoice voice(backend);
		check(voice.open(), "source error voice opens");
		voice.submit(makeChunk(0, 0, 37));
		voice.service();
		backend.voiceError(E_FAIL);
		check(voice.submit(makeChunk(0, 1, 39)) == AudioPcmSubmitResult::FAILED,
			"source voice callback error reports terminal failure before service observes it");
		voice.service();
		check(voice.isFailed(), "source voice callback error enters terminal state");
		voice.close();
	}
	{
		FakePcmVoiceBackend backend;
		backend.criticalError = E_FAIL;
		XAudio2PcmVoice voice(backend);
		check(voice.open(), "critical-engine error voice opens");
		voice.submit(makeChunk(0, 0, 38));
		voice.service();
		check(voice.isFailed(), "critical engine error enters terminal state");
		check(backend.submitCalls == 0, "critical engine error prevents submission");
		voice.close();
	}
	{
		FakePcmVoiceBackend backend;
		backend.destroyResult = E_FAIL;
		XAudio2PcmVoice voice(backend);
		check(voice.open(), "destroy failure voice opens");
		voice.close();
		check(voice.getLastError() == E_FAIL,
			"close preserves the first actionable backend destroy failure");
	}
}

void testSameGenerationResetBarrier()
{
	FakePcmVoiceBackend backend;
	XAudio2PcmVoice voice(backend);
	check(voice.open(), "same-generation reset voice opens");
	check(voice.submit(makeChunk(0, 0, 45)) == AudioPcmSubmitResult::ACCEPTED,
		"same-generation old chunk is admitted");
	voice.service();
	check(backend.submissions.size() == 1, "same-generation old chunk reaches the backend");

	voice.reset(0);
	check(voice.submit(makeChunk(0, 1, 46)) == AudioPcmSubmitResult::ACCEPTED,
		"same-generation new chunk is admitted behind reset");
	voice.service();
	check(backend.submissions.size() == 1,
		"same-generation reset keeps new data behind the old callback barrier");
	check(backend.stopCalls == 0 && backend.flushCalls == 0,
		"same-generation reset does not stop or flush before old completion");

	backend.complete(0);
	voice.service();
	check(backend.stopCalls == 1 && backend.flushCalls == 1,
		"same-generation reset requests its barrier after old completion");
	check(backend.submissions.size() == 2,
		"same-generation new data reaches the backend after the barrier");
	check(backend.submissions[1].audio[0] == 46,
		"same-generation reset submits the new chunk after the old chunk");
	voice.close();
}

void testReopenAndRepeatedCleanup()
{
	FakePcmVoiceBackend backend;
	XAudio2PcmVoice voice(backend);
	check(voice.open(), "cleanup test voice opens");
	voice.submit(makeChunk(0, 0, 40));
	voice.service();
	check(backend.submissions.size() == 1, "cleanup test submits one buffer");
	void *oldContext = backend.submissions.front().context;
	backend.complete(0);
	voice.service();
	voice.service();
	voice.close();
	voice.close();
	check(backend.destroyCalls == 1, "repeated close destroys the source voice once");
	voice.OnBufferEnd(oldContext);
	voice.service();
	check(backend.submitCalls == 1, "late completion after close does not access destroyed storage");

	check(voice.open(), "close permits a clean reopen");
	check(!voice.isFailed(), "reopen clears the terminal state");
	voice.reset(5);
	voice.service();
	voice.close();
	check(backend.destroyCalls == 2, "reopen has one additional destroy");
}

void testStaleCallbackTokenIsIgnoredAfterSlotReuse()
{
	FakePcmVoiceBackend backend;
	XAudio2PcmVoice voice(backend);
	check(voice.open(), "stale callback voice opens");
	check(voice.submit(makeChunk(0, 0, 50)) == AudioPcmSubmitResult::ACCEPTED,
		"stale callback first buffer is admitted");
	voice.service();
	void *oldContext = backend.submissions.front().context;
	backend.complete(0);
	XAudio2PcmCompletionRecord completion;
	check(voice.tryPopCompletion(completion) && completion.sequence == 0,
		"stale callback first completion is observed once");
	voice.service();
	check(voice.submit(makeChunk(0, 1, 51)) == AudioPcmSubmitResult::ACCEPTED,
		"stale callback slot is reused only by the owner");
	voice.service();
	voice.OnBufferEnd(oldContext);
	check(!voice.tryPopCompletion(completion),
		"a callback token from a reclaimed submission cannot complete its replacement");
	backend.complete(1);
	check(voice.tryPopCompletion(completion) && completion.sequence == 1,
		"the replacement submission still publishes its own completion");
	voice.close();
}

void testCallbackClaimPrecedesOwnerReclamation()
{
	FakePcmVoiceBackend backend;
	XAudio2PcmVoice voice(backend);
	check(voice.open(), "callback-owner race voice opens");
	for (std::uint64_t sequence = 0; sequence < 256; ++sequence) {
		check(voice.submit(makeChunk(0, sequence, static_cast<std::uint8_t>(sequence)))
				== AudioPcmSubmitResult::ACCEPTED,
			"callback-owner race admits a chunk");
		voice.service();
		const void *context = backend.submissions.back().context;
		std::atomic<bool> started { false };
		std::thread callback([&voice, context, &started]() {
			started.store(true, std::memory_order_release);
			voice.OnBufferEnd(const_cast<void *>(context));
		});
		while (!started.load(std::memory_order_acquire)) {
			std::this_thread::yield();
		}
		// Reclamation may race the callback, but it must not clear metadata until
		// the callback has finished all reads and accounting.
		voice.service();
		callback.join();
		voice.service();
		XAudio2PcmCompletionRecord completion;
		check(voice.tryPopCompletion(completion) && completion.sequence == sequence,
			"callback-owner race publishes one coherent completion");
	}
	voice.close();
}

void testOptionalPitchControlCompatibility()
{
	FakePcmVoiceBackend backend;
	XAudio2PcmVoice voice(backend);
	check(!voice.setFrequencyRatio(1.0f) && voice.open(),
		"closed voices reject pitch and older backends still open at the default range");
	check(voice.setFrequencyRatio(1.0f) && !voice.isFailed(),
		"older backends preserve their initial unity pitch without implementing a new control");
	check(!voice.setFrequencyRatio(1.25f) && voice.isFailed()
		&& voice.getLastError() == E_NOTIMPL,
		"older backends cannot silently discard an authored non-unity pitch");
	voice.close();
	const int createsBefore = backend.createCalls;
	check(!voice.open(3.0f) && backend.createCalls == createsBefore
		&& voice.getLastError() == E_NOTIMPL,
		"older backends reject larger ranges before creating a voice with an insufficient maximum");
	voice.close();
}

void testCompletionOverflowFailsClosed()
{
	FakePcmVoiceBackend backend;
	XAudio2PcmVoice voice(backend);
	check(voice.open(), "completion overflow voice opens");
	for (std::uint64_t sequence = 0; sequence <= 32; ++sequence) {
		check(voice.submit(makeChunk(0, sequence, static_cast<std::uint8_t>(sequence)))
				== AudioPcmSubmitResult::ACCEPTED,
			"completion overflow cycle admits one chunk");
		voice.service();
		backend.complete(static_cast<std::size_t>(sequence));
		voice.service();
	}
	check(voice.isFailed(), "completion overflow enters a terminal state instead of stranding playback");
	check(voice.getLastError() == HRESULT_FROM_WIN32(ERROR_BUFFER_OVERFLOW),
		"completion overflow publishes an actionable error");
	voice.close();
}
}

int main()
{
	testFixedFormatAndBoundedAdmission();
	testOwnedStorageAndOwnerOnlyReclaim();
	testValidationAndClosedAdmission();
	testPlayedSampleClockAndGeneration();
	testPauseBeforeFirstServiceAndPendingRefill();
	testPauseSurvivesGenerationReset();
	testStopPreventsPendingRestartAndCloseClearsPause();
	testPlayedSampleClockIgnoresTimelineGaps();
	testSameGenerationResetRejectsStaleClockCompletion();
	testResetBarrierAndGenerationActivation();
	testSameGenerationResetBarrier();
	testTerminalFailures();
	testReopenAndRepeatedCleanup();
	testStaleCallbackTokenIsIgnoredAfterSlotReuse();
	testCallbackClaimPrecedesOwnerReclamation();
	testOptionalPitchControlCompatibility();
	testCompletionOverflowFailsClosed();
	if (g_failures != 0) {
		std::fprintf(stderr, "%d XAudio2PcmVoice checks failed\n", g_failures);
		return 1;
	}
	std::puts("XAudio2PcmVoice checks passed");
	return 0;
}
