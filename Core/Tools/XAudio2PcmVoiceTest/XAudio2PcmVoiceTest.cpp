#include "XAudio2AudioDevice/XAudio2PcmVoice.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
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
	HRESULT criticalError = S_OK;
	int createCalls = 0;
	int submitCalls = 0;
	int startCalls = 0;
	int stopCalls = 0;
	int flushCalls = 0;
	int destroyCalls = 0;
	int criticalErrorCalls = 0;
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

	void destroy() noexcept override
	{
		++destroyCalls;
		callLog.emplace_back("destroy");
		callback = nullptr;
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

	for (std::uint64_t sequence = 0; sequence < XAudio2PcmVoice::SLOT_COUNT; ++sequence) {
		check(voice.submit(makeChunk(0, sequence, static_cast<std::uint8_t>(sequence)))
				== AudioPcmSubmitResult::ACCEPTED,
			"the eight fixed slots admit valid chunks");
	}
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
	check(voice.submit(makeChunk(0, 1, 55)) == AudioPcmSubmitResult::DROPPED,
		"callback completion alone does not reclaim a slot");
	voice.service();
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
	check(backend.submitCalls == 0 && backend.criticalErrorCalls == 0,
		"invalid and stale submissions do not call the backend");

	voice.close();
	check(voice.submit(makeChunk(0, 0, 3)) == AudioPcmSubmitResult::DROPPED,
		"closed voices drop submissions");
	check(backend.submitCalls == 0, "closed submissions do not call the backend");
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
		check(voice.submit(makeChunk(0, 1, 31)) == AudioPcmSubmitResult::DROPPED,
			"failed voices drop later submissions");
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
}

int main()
{
	testFixedFormatAndBoundedAdmission();
	testOwnedStorageAndOwnerOnlyReclaim();
	testValidationAndClosedAdmission();
	testResetBarrierAndGenerationActivation();
	testTerminalFailures();
	testReopenAndRepeatedCleanup();
	if (g_failures != 0) {
		std::fprintf(stderr, "%d XAudio2PcmVoice checks failed\n", g_failures);
		return 1;
	}
	std::puts("XAudio2PcmVoice checks passed");
	return 0;
}
