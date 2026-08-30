#include "XAudio2AudioDevice/IXAudio2AudioEngineBackend.h"
#include "XAudio2AudioDevice/XAudio2CallbackGate.h"
#include "XAudio2AudioDevice/XAudio2AudioService.h"
#include "XAudio2AudioDevice/XAudio2FailurePublication.h"
#include "XAudio2AudioDevice/XAudio2PcmVoice.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <limits>
#include <memory>
#include <thread>
#include <string>
#include <vector>

namespace
{
int g_failures = 0;
std::atomic<int> g_backendUseAfterDestroy { 0 };

void check(bool condition, const char *message)
{
	if (!condition) {
		std::fprintf(stderr, "FAIL: %s\n", message);
		++g_failures;
	}
}

class FakePcmVoiceBackend final : public IXAudio2PcmVoiceBackend
{
public:
	FakePcmVoiceBackend(std::vector<std::string> &calls, int &destroyCount, HRESULT createResult) :
		m_calls(calls),
		m_destroyCount(destroyCount),
		m_createResult(createResult)
	{
	}

	HRESULT create(const WAVEFORMATEX &, IXAudio2VoiceCallback *callback) noexcept override
	{
		m_callback = callback;
		m_calls.push_back("voice.create");
		return m_createResult;
	}

	HRESULT submit(const XAUDIO2_BUFFER &buffer) noexcept override
	{
		if (m_destroyed) {
			++g_backendUseAfterDestroy;
		}
		m_submittedContexts.push_back(buffer.pContext);
		return S_OK;
	}
	HRESULT start() noexcept override
	{
		if (m_destroyed) {
			++g_backendUseAfterDestroy;
		}
		return S_OK;
	}
	HRESULT stop() noexcept override
	{
		if (m_destroyed) {
			++g_backendUseAfterDestroy;
		}
		return S_OK;
	}
	HRESULT flush() noexcept override
	{
		if (m_destroyed) {
			++g_backendUseAfterDestroy;
		}
		return S_OK;
	}
	HRESULT getCriticalError() const noexcept override { return S_OK; }
	HRESULT setVolume(float volume) noexcept override
	{
		lastVolume = volume;
		++setVolumeCalls;
		return S_OK;
	}
	HRESULT setOutputMatrix(UINT32 sourceChannels, UINT32 destinationChannels,
		const float *matrix) noexcept override
	{
		lastSourceChannels = sourceChannels;
		lastDestinationChannels = destinationChannels;
		lastMatrix.assign(matrix, matrix + sourceChannels * destinationChannels);
		return S_OK;
	}

	void destroy() noexcept override
	{
		m_callback = nullptr;
		m_destroyed = true;
		++m_destroyCount;
		m_calls.push_back("voice.destroy");
	}

	void complete(std::size_t index)
	{
		if (m_callback != nullptr && index < m_submittedContexts.size()) {
			m_callback->OnBufferEnd(m_submittedContexts[index]);
		}
	}

	std::size_t submissionCount() const noexcept { return m_submittedContexts.size(); }
	UINT32 lastSourceChannels = 0;
	UINT32 lastDestinationChannels = 0;
	float lastVolume = -1.0f;
	int setVolumeCalls = 0;
	std::vector<float> lastMatrix;

private:
	std::vector<std::string> &m_calls;
	int &m_destroyCount;
	HRESULT m_createResult;
	IXAudio2VoiceCallback *m_callback = nullptr;
	std::vector<void *> m_submittedContexts;
	bool m_destroyed = false;
};

class FakeAudioEngine final : public IXAudio2AudioEngineBackend
{
public:
	std::vector<std::string> calls;
	int voiceDestroyCount = 0;
	HRESULT openResult = S_OK;
	HRESULT startResult = S_OK;
	HRESULT createVoiceResult = S_OK;
	bool returnPartialBackendOnCreateFailure = false;
	HRESULT voiceCreateResult = S_OK;
	HRESULT stopResult = S_OK;
	HRESULT closeResult = S_OK;
	int openCalls = 0;
	int startCalls = 0;
	int createVoiceCalls = 0;
	int stopCalls = 0;
	int closeCalls = 0;
	bool callbackDuringClose = false;
	bool openingObserved = false;
	bool quiescingObserved = false;
	bool invokeCallbackDuringClose = false;
	std::atomic<bool> callbackInFlightCompleted { false };
	bool blockStart = false;
	std::atomic<bool> startEntered { false };
	std::atomic<bool> releaseStart { true };
	bool blockCreateBackend = false;
	std::atomic<bool> createBackendEntered { false };
	std::atomic<bool> releaseCreateBackend { true };
	FakePcmVoiceBackend *lastVoiceBackend = nullptr;
	bool provideOutputDetails = false;
	UINT32 outputChannelMask = SPEAKER_STEREO;
	UINT32 outputChannelCount = 2;

	HRESULT open(CriticalErrorCallback callback, void *context) noexcept override
	{
		++openCalls;
		m_callback = callback;
		m_context = context;
		calls.push_back("open");
		openingObserved = static_cast<XAudio2AudioService *>(context)->state()
			== XAudio2AudioServiceState::OPENING;
		return openResult;
	}

	HRESULT start() noexcept override
	{
		++startCalls;
		calls.push_back("start");
		if (blockStart) {
			startEntered.store(true, std::memory_order_release);
			while (!releaseStart.load(std::memory_order_acquire)) {
				std::this_thread::yield();
			}
		}
		return startResult;
	}

	HRESULT createPcmVoice(std::unique_ptr<IXAudio2PcmVoiceBackend> &voice) noexcept override
	{
		++createVoiceCalls;
		calls.push_back("voice.create.backend");
		if (blockCreateBackend) {
			createBackendEntered.store(true, std::memory_order_release);
			while (!releaseCreateBackend.load(std::memory_order_acquire)) {
				std::this_thread::yield();
			}
		}
		if (FAILED(createVoiceResult)) {
			if (returnPartialBackendOnCreateFailure) {
				voice = std::make_unique<FakePcmVoiceBackend>(calls, voiceDestroyCount, S_OK);
			}
			return createVoiceResult;
		}
		auto pcmBackend = std::make_unique<FakePcmVoiceBackend>(calls, voiceDestroyCount, voiceCreateResult);
		lastVoiceBackend = pcmBackend.get();
		voice = std::move(pcmBackend);
		return createVoiceResult;
	}

	HRESULT getOutputDetails(XAudio2OutputDetails &details) const noexcept override
	{
		if (!provideOutputDetails) {
			details = {};
			return E_NOTIMPL;
		}
		details.channelMask = outputChannelMask;
		details.channelCount = outputChannelCount;
		return S_OK;
	}

	HRESULT stop() noexcept override
	{
		++stopCalls;
		calls.push_back("stop");
		return stopResult;
	}

	HRESULT close() noexcept override
	{
		++closeCalls;
		calls.push_back("close");
		callbackDuringClose = m_callback != nullptr;
		quiescingObserved = static_cast<XAudio2AudioService *>(m_context)->state()
			== XAudio2AudioServiceState::QUIESCING;
		if (invokeCallbackDuringClose && m_callback != nullptr) {
			callbackInFlightCompleted.store(false, std::memory_order_release);
			std::thread callbackThread([this]() {
				m_callback(m_context, E_UNEXPECTED);
				callbackInFlightCompleted.store(true, std::memory_order_release);
			});
			callbackThread.join();
		}
		return closeResult;
	}

	void emitCritical(HRESULT error)
	{
		if (m_callback != nullptr) {
			m_callback(m_context, error);
		}
	}

	void emitRetainedAfterClose(HRESULT error)
	{
		emitCritical(error);
	}

private:
	CriticalErrorCallback m_callback = nullptr;
	void *m_context = nullptr;
};

void testInjectedLifecycleAndIndependentVoices()
{
	auto backend = std::make_unique<FakeAudioEngine>();
	FakeAudioEngine *backendView = backend.get();
	XAudio2AudioService service(std::move(backend));

	check(service.open(), "injected engine opens");
	check(service.state() == XAudio2AudioServiceState::RUNNING,
		"open leaves the service running");
	const XAudio2PcmVoiceHandle first = service.createVoice();
	const XAudio2PcmVoiceHandle second = service.createVoice();
	const XAudio2PcmVoiceHandle third = service.createVoice();
	check(first.isValid() && second.isValid() && third.isValid(),
		"service creates three typed voice handles");
	check(first != second && second != third && first != third,
		"every source voice receives an independent handle");
	check(service.isVoiceOpen(first) && service.isVoiceOpen(second)
			&& service.isVoiceOpen(third),
		"typed handles resolve to service-owned voices");

	service.shutdown();
	check(service.state() == XAudio2AudioServiceState::CLOSED,
		"shutdown closes the service");
	check(backendView->voiceDestroyCount == 3,
		"shutdown destroys every child voice");
	check(backendView->calls.size() >= 5 && backendView->calls[0] == "open"
			&& backendView->calls[1] == "start"
			&& backendView->calls[backendView->calls.size() - 2] == "stop"
			&& backendView->calls.back() == "close",
		"shutdown stops the engine before closing it");
}

std::size_t firstCall(const std::vector<std::string> &calls, const char *name)
{
	for (std::size_t i = 0; i < calls.size(); ++i) {
		if (calls[i] == name) {
			return i;
		}
	}
	return calls.size();
}

std::size_t lastCall(const std::vector<std::string> &calls, const char *name)
{
	for (std::size_t i = calls.size(); i > 0; --i) {
		if (calls[i - 1] == name) {
			return i - 1;
		}
	}
	return calls.size();
}

AudioPcmChunk makeChunk(std::uint64_t generation, std::uint64_t sequence)
{
	AudioPcmChunk chunk;
	chunk.sampleRate = 48000;
	chunk.channels = 2;
	chunk.format = AudioPcmFormat::SIGNED_16_INTERLEAVED_LITTLE_ENDIAN;
	chunk.frameCount = 2;
	chunk.startSample = static_cast<std::int64_t>(sequence * chunk.frameCount);
	chunk.generation = generation;
	chunk.sequence = sequence;
	chunk.data.resize(8, static_cast<std::uint8_t>(sequence));
	return chunk;
}

void testFailurePublicationFenceAndOrdering()
{
	XAudio2FailurePublication callbackFirst;
	const std::uint64_t beforeCallback = callbackFirst.snapshot();
	check(callbackFirst.publish(E_ABORT),
		"the first callback failure publishes an atomic failure fence");
	std::uint64_t committed = 0;
	check(!callbackFirst.tryCommit(beforeCallback, committed),
		"an owner commit cannot cross a callback publication fence");
	check(callbackFirst.failure() == E_ABORT,
		"the callback publication retains its normalized first failure");

	XAudio2FailurePublication backendFirst;
	const std::uint64_t beforeBackend = backendFirst.snapshot();
	check(backendFirst.publish(E_FAIL),
		"a backend HRESULT can publish through the shared failure latch");
	check(!backendFirst.publish(E_ACCESSDENIED),
		"a later callback cannot overwrite an earlier backend failure");
	check(backendFirst.failure() == E_FAIL,
		"the backend publication retains the first failure");

	XAudio2FailurePublication callbackWins;
	check(callbackWins.publish(E_ABORT), "callback-first ordering is publishable");
	check(!callbackWins.publish(E_FAIL), "a later backend error cannot overwrite a callback failure");
	check(callbackWins.failure() == E_ABORT, "callback-first ordering remains observable");
}

void testCallbackGateAdmissionDrainAndReopen()
{
	XAudio2CallbackGate gate;
	const std::uint64_t firstGeneration = gate.enable();
	XAudio2CallbackGate::Token held;
	check(gate.tryEnter(held, firstGeneration), "callback gate admits the active generation");

	std::atomic<bool> closeStarted { false };
	std::atomic<bool> closeReturned { false };
	std::thread closer([&]() {
		closeStarted.store(true, std::memory_order_release);
		gate.disableAndWait();
		closeReturned.store(true, std::memory_order_release);
	});
	while (!closeStarted.load(std::memory_order_acquire)) {
		std::this_thread::yield();
	}
	for (;;) {
		XAudio2CallbackGate::Token probe;
		if (!gate.tryEnter(probe, firstGeneration)) {
			break;
		}
		gate.leave(probe);
		std::this_thread::yield();
	}
	check(!closeReturned.load(std::memory_order_acquire),
		"callback gate closes admission while an in-flight callback remains held");
	gate.leave(held);
	closer.join();
	check(closeReturned.load(std::memory_order_acquire),
		"callback gate close returns after the callback leaves");

	const std::uint64_t secondGeneration = gate.enable();
	check(secondGeneration != firstGeneration, "callback gate increments its reopen generation");
	XAudio2CallbackGate::Token stale;
	check(!gate.tryEnter(stale, firstGeneration),
		"a pre-disable callback token cannot enter after reopen");
	XAudio2CallbackGate::Token current;
	check(gate.tryEnter(current, secondGeneration), "the reopened generation admits callbacks");
	gate.leave(current);

	std::atomic<bool> raceCloseReturned { false };
	std::atomic<int> admissionsAfterClose { 0 };
	std::thread entrant([&]() {
		while (!raceCloseReturned.load(std::memory_order_acquire)) {
			XAudio2CallbackGate::Token token;
			if (gate.tryEnter(token, secondGeneration)) {
				if (raceCloseReturned.load(std::memory_order_acquire)) {
					++admissionsAfterClose;
				}
				gate.leave(token);
			}
		}
	});
	std::thread raceCloser([&]() {
		gate.disableAndWait();
		raceCloseReturned.store(true, std::memory_order_release);
	});
	raceCloser.join();
	entrant.join();
	check(admissionsAfterClose.load(std::memory_order_acquire) == 0,
		"a callback cannot check in after close has drained the generation");
}

void testHandleScopedOperationsAndConcurrentStaleAccess()
{
	g_backendUseAfterDestroy.store(0, std::memory_order_release);
	auto backend = std::make_unique<FakeAudioEngine>();
	FakeAudioEngine *backendView = backend.get();
	XAudio2AudioService service(std::move(backend));
	check(service.open(), "handle-operation service opens");
	const XAudio2PcmVoiceHandle handle = service.createVoice();
	check(handle.isValid(), "handle-operation service creates a voice");
	check(service.isVoiceOpen(handle), "valid handles report an open voice");
	check(service.getVoiceLastError(handle) == S_OK, "valid handles query voice errors");
	check(service.canVoiceAccept(handle, 1),
		"new valid handles initially accept audio");
	check(service.resetVoice(handle, 1), "valid handles reset their voice");
	check(!service.canVoiceAccept(handle, 1),
		"pending generation resets suspend admission until owner service");
	check(service.serviceVoice(handle),
		"owner service commits pending generation resets");
	check(service.canVoiceAccept(handle, 1),
		"serviced valid handles resume audio admission");
	check(service.setVoiceVolume(handle, 0.4f)
			&& backendView->lastVoiceBackend->setVolumeCalls == 1
			&& backendView->lastVoiceBackend->lastVolume == 0.4f,
		"valid handles update native output gain");
	check(service.submit(handle, makeChunk(1, 0)) == AudioPcmSubmitResult::ACCEPTED,
		"valid handles submit through the service");
	check(service.serviceVoice(handle), "valid handles service their voice");

	std::atomic<bool> start { false };
	std::thread destroyer([&]() {
		while (!start.load(std::memory_order_acquire)) {
			std::this_thread::yield();
		}
		service.destroyVoice(handle);
	});
	std::thread staleAccess([&]() {
		while (!start.load(std::memory_order_acquire)) {
			std::this_thread::yield();
		}
		for (int i = 0; i < 128; ++i) {
			service.isVoiceOpen(handle);
			service.isVoiceFailed(handle);
			service.getVoiceLastError(handle);
			service.resetVoice(handle, 2);
			service.serviceVoice(handle);
			service.submit(handle, makeChunk(2, static_cast<std::uint64_t>(i)));
		}
	});
	start.store(true, std::memory_order_release);
	destroyer.join();
	staleAccess.join();
	check(!service.isVoiceOpen(handle), "stale handle queries are rejected after destruction");
	check(!service.canVoiceAccept(handle, 1), "stale handles publish no admission capacity");
	check(g_backendUseAfterDestroy.load(std::memory_order_acquire) == 0,
		"stale handle operations never touch a destroyed backend");
	service.shutdown();
}

void testDedicatedMovieOwnerDiscardsCompletions()
{
	auto backend = std::make_unique<FakeAudioEngine>();
	FakeAudioEngine *backendView = backend.get();
	XAudio2AudioService service(std::move(backend));
	check(service.open(), "movie-completion service opens");
	const XAudio2PcmVoiceHandle handle = service.createVoice();
	check(handle.isValid() && backendView->lastVoiceBackend != nullptr,
		"movie-completion service creates one observable voice");
	check(service.resetVoice(handle, 1), "movie-completion voice activates its generation");

	for (std::uint64_t sequence = 0; sequence < 40; ++sequence) {
		check(service.submit(handle, makeChunk(1, sequence)) == AudioPcmSubmitResult::ACCEPTED,
			"movie-completion cycle admits one chunk");
		check(service.serviceVoice(handle), "movie-completion cycle submits its chunk");
		check(backendView->lastVoiceBackend->submissionCount() == sequence + 1,
			"movie-completion backend observes each submission");
		backendView->lastVoiceBackend->complete(static_cast<std::size_t>(sequence));
		check(service.serviceVoice(handle), "movie-completion owner reclaims its completed slot");
		if ((sequence + 1) % XAudio2PcmVoice::SLOT_COUNT == 0) {
			service.discardCompletions();
		}
		check(!service.isVoiceFailed(handle),
			"dedicated movie completion draining prevents bounded-queue overflow");
	}
	service.discardCompletions();
	service.shutdown();
}

void testPartialBackendFailureIsDestroyedOnce()
{
	auto backend = std::make_unique<FakeAudioEngine>();
	FakeAudioEngine *backendView = backend.get();
	backendView->createVoiceResult = E_FAIL;
	backendView->returnPartialBackendOnCreateFailure = true;
	XAudio2AudioService service(std::move(backend));
	check(service.open(), "partial-backend service opens");
	check(!service.createVoice().isValid(), "partial backend failure returns no handle");
	check(backendView->voiceDestroyCount == 1,
		"a non-null backend returned with failure is destroyed exactly once");
	backendView->createVoiceResult = S_OK;
	backendView->returnPartialBackendOnCreateFailure = false;
	check(service.createVoice().isValid(), "service remains usable after partial backend cleanup");
	service.shutdown();
}

void testConcurrentTransitionsPreserveFirstFailure()
{
	{
		auto backend = std::make_unique<FakeAudioEngine>();
		FakeAudioEngine *backendView = backend.get();
		backendView->blockStart = true;
		backendView->releaseStart.store(false, std::memory_order_release);
		XAudio2AudioService service(std::move(backend));
		std::atomic<bool> opened { true };
		std::thread opener([&]() { opened.store(service.open(), std::memory_order_release); });
		while (!backendView->startEntered.load(std::memory_order_acquire)) {
			std::this_thread::yield();
		}
		// Keep the owner transaction paused inside StartEngine while the native
		// callback publishes its failure.  The shared publication must prevent
		// the later OPENING -> RUNNING commit from succeeding.
		backendView->emitCritical(E_ABORT);
		backendView->emitCritical(E_ACCESSDENIED);
		check(service.state() == XAudio2AudioServiceState::FAILED,
			"a paused open transaction observes the callback failure before resume");
		backendView->releaseStart.store(true, std::memory_order_release);
		opener.join();
		check(!opened.load(std::memory_order_acquire),
			"an asynchronous critical error prevents open from committing RUNNING");
		check(service.state() == XAudio2AudioServiceState::CLOSED,
			"a failed open cannot be overwritten by a late RUNNING store");
		check(service.getLastError() == E_ABORT,
			"the first competing critical failure is preserved");
	}
	{
		auto backend = std::make_unique<FakeAudioEngine>();
		FakeAudioEngine *backendView = backend.get();
		backendView->startResult = E_FAIL;
		backendView->blockStart = true;
		backendView->releaseStart.store(false, std::memory_order_release);
		XAudio2AudioService service(std::move(backend));
		std::atomic<bool> opened { true };
		std::thread opener([&]() { opened.store(service.open(), std::memory_order_release); });
		while (!backendView->startEntered.load(std::memory_order_acquire)) {
			std::this_thread::yield();
		}
		backendView->emitCritical(E_ABORT);
		backendView->releaseStart.store(true, std::memory_order_release);
		opener.join();
		check(!opened.load(std::memory_order_acquire) && service.getLastError() == E_ABORT,
			"an earlier critical failure wins over a later StartEngine failure");
	}
	{
		auto backend = std::make_unique<FakeAudioEngine>();
		FakeAudioEngine *backendView = backend.get();
		backendView->blockCreateBackend = true;
		backendView->releaseCreateBackend.store(false, std::memory_order_release);
		XAudio2AudioService service(std::move(backend));
		check(service.open(), "create-race service opens");
		std::atomic<bool> created { true };
		std::thread creator([&]() { created.store(service.createVoice().isValid(), std::memory_order_release); });
		while (!backendView->createBackendEntered.load(std::memory_order_acquire)) {
			std::this_thread::yield();
		}
		backendView->emitCritical(E_ABORT);
		backendView->releaseCreateBackend.store(true, std::memory_order_release);
		creator.join();
		check(!created.load(std::memory_order_acquire),
			"a voice creation transaction cannot commit after an asynchronous failure");
		service.shutdown();
	}
}

void testOpenAndStartFailuresUnwindAndReopen()
{
	{
		auto backend = std::make_unique<FakeAudioEngine>();
		FakeAudioEngine *backendView = backend.get();
		backendView->openResult = E_ACCESSDENIED;
		XAudio2AudioService service(std::move(backend));
		check(!service.open(), "an engine-open failure is reported");
		check(service.state() == XAudio2AudioServiceState::CLOSED,
			"an engine-open failure leaves the service closed");
		check(service.getLastError() == E_ACCESSDENIED,
			"the original engine-open failure is preserved");
		check(backendView->openCalls == 1 && backendView->closeCalls == 1 && backendView->stopCalls == 0,
			"an engine-open failure closes exactly once without stopping an unstarted engine");

		backendView->openResult = S_OK;
		check(service.open(), "the service reopens after an engine-open failure");
		service.shutdown();
	}
	{
		auto backend = std::make_unique<FakeAudioEngine>();
		FakeAudioEngine *backendView = backend.get();
		backendView->startResult = E_FAIL;
		XAudio2AudioService service(std::move(backend));
		check(!service.open(), "an engine-start failure is reported");
		check(service.state() == XAudio2AudioServiceState::CLOSED,
			"an engine-start failure leaves the service closed");
		check(service.getLastError() == E_FAIL, "the original engine-start failure is preserved");
		check(backendView->openCalls == 1 && backendView->startCalls == 1
				&& backendView->stopCalls == 1 && backendView->closeCalls == 1,
			"an engine-start failure unwinds open resources exactly once");

		backendView->startResult = S_OK;
		check(service.open(), "the service reopens after an engine-start failure");
		service.shutdown();
		check(backendView->stopCalls == 2 && backendView->closeCalls == 2,
			"reopen performs one additional stop and close");
	}
}

void testSourceFailureDoesNotConsumeGeneration()
{
	auto backend = std::make_unique<FakeAudioEngine>();
	FakeAudioEngine *backendView = backend.get();
	XAudio2AudioService service(std::move(backend));
	check(service.open(), "source-failure service opens");
	const XAudio2PcmVoiceHandle first = service.createVoice();
	check(first.isValid(), "the first source voice is created");

	backendView->voiceCreateResult = E_FAIL;
	const XAudio2PcmVoiceHandle failed = service.createVoice();
	check(!failed.isValid(), "a source create failure returns an invalid handle");
	check(service.isVoiceOpen(first), "a source create failure preserves existing records");
	check(backendView->voiceDestroyCount == 1,
		"a partially-created source backend is destroyed exactly once");

	backendView->voiceCreateResult = S_OK;
	const XAudio2PcmVoiceHandle second = service.createVoice();
	check(second.isValid() && second.generation == first.generation + 1,
		"a failed source create does not consume a handle generation");
	service.shutdown();
}

void testClosedFailedQuiescingAndStaleHandles()
{
	auto backend = std::make_unique<FakeAudioEngine>();
	FakeAudioEngine *backendView = backend.get();
	XAudio2AudioService service(std::move(backend));
	check(!service.createVoice().isValid(), "closed services reject voice creation");
	check(service.open(), "state-transition service opens");
	check(backendView->openingObserved, "the service publishes OPENING during backend open");

	const XAudio2PcmVoiceHandle first = service.createVoice();
	check(service.destroyVoice(first), "the first voice can be destroyed");
	check(!service.isVoiceOpen(first) && !service.destroyVoice(first),
		"destroyed handles cannot access or destroy a replacement record");
	const XAudio2PcmVoiceHandle replacement = service.createVoice();
	check(replacement.isValid() && replacement.index == first.index
			&& replacement.generation != first.generation,
		"reused slots receive a new generation");
	check(!service.isVoiceOpen(first) && service.isVoiceOpen(replacement),
		"stale handles remain rejected after slot reuse");

	backendView->emitCritical(E_ABORT);
	check(service.state() == XAudio2AudioServiceState::FAILED,
		"a critical callback atomically publishes FAILED");
	check(!service.isOpen(),
		"a latched critical failure makes the service non-open before owner processing");
	check(service.processPendingFailure(), "the owner observes the pending critical error");
	check(!service.processPendingFailure(),
		"the first failure publication is applied once while FAILED");
	check(service.isVoiceOpen(replacement) && service.isVoiceFailed(replacement),
		"existing voices observe terminal failure on the owner service");
	check(service.submit(replacement, makeChunk(replacement.generation, 0)) == AudioPcmSubmitResult::FAILED,
		"a failed service reports terminal failure for valid voice submissions");
	check(!service.createVoice().isValid(), "failed services reject new voices");

	service.shutdown();
	check(backendView->quiescingObserved, "shutdown publishes QUIESCING before backend close");
	check(!service.createVoice().isValid(), "closed services reject new voices after shutdown");
	check(!service.isVoiceOpen(replacement), "shutdown invalidates all voice handles");

	const HRESULT errorBeforeLateCallback = service.getLastError();
	backendView->emitRetainedAfterClose(E_ACCESSDENIED);
	check(service.state() == XAudio2AudioServiceState::CLOSED
			&& service.getLastError() == errorBeforeLateCallback,
		"callback delivery after shutdown is ignored");

	check(service.open(), "the service reopens after full shutdown");
	const XAudio2PcmVoiceHandle reopened = service.createVoice();
	check(reopened.isValid() && reopened != replacement,
		"reopen creates a new service generation and rejects old handles");
	service.shutdown();
}

void testShutdownOrderFailureAndIdempotence()
{
	auto backend = std::make_unique<FakeAudioEngine>();
	FakeAudioEngine *backendView = backend.get();
	backendView->stopResult = E_FAIL;
	backendView->closeResult = E_ACCESSDENIED;
	backendView->invokeCallbackDuringClose = true;
	XAudio2AudioService service(std::move(backend));
	check(service.open(), "shutdown-failure service opens");
	const XAudio2PcmVoiceHandle first = service.createVoice();
	const XAudio2PcmVoiceHandle second = service.createVoice();
	check(first.isValid() && second.isValid(), "shutdown-failure service owns live voices");

	service.shutdown();
	check(backendView->callbackInFlightCompleted.load(std::memory_order_acquire),
		"shutdown drains the deliberately in-flight callback before backend close returns");
	check(service.state() == XAudio2AudioServiceState::CLOSED,
		"shutdown remains closed after stop and close failures");
	check(service.getLastError() == E_FAIL,
		"shutdown preserves the first stop failure over close failure");
	check(backendView->voiceDestroyCount == 2,
		"shutdown destroys every live child before stopping the engine");
	check(lastCall(backendView->calls, "voice.destroy") < firstCall(backendView->calls, "stop"),
		"every child is destroyed before engine stop");
	const int stopCalls = backendView->stopCalls;
	const int closeCalls = backendView->closeCalls;
	service.shutdown();
	check(backendView->stopCalls == stopCalls && backendView->closeCalls == closeCalls,
		"shutdown is idempotent after resource teardown");
	backendView->stopResult = S_OK;
	backendView->closeResult = S_OK;
	check(service.open(), "shutdown failure still permits a clean reopen");
	service.shutdown();
}

void testRepeatedCyclesRemainDeterministic()
{
	auto backend = std::make_unique<FakeAudioEngine>();
	FakeAudioEngine *backendView = backend.get();
	XAudio2AudioService service(std::move(backend));
	for (int cycle = 0; cycle < 4; ++cycle) {
		check(service.open(), "repeated cycle opens");
		check(service.open(), "repeated open is idempotent while running");
		const XAudio2PcmVoiceHandle handle = service.createVoice();
		check(handle.isValid(), "repeated cycle creates a voice");
		service.shutdown();
		check(service.state() == XAudio2AudioServiceState::CLOSED,
			"repeated cycle closes deterministically");
	}
	check(backendView->openCalls == 4 && backendView->startCalls == 4
			&& backendView->stopCalls == 4 && backendView->closeCalls == 4,
		"repeated open and shutdown cycles acquire and release once per cycle");
}

void testPointSourceSpatializationPreservesMonoLevel()
{
	auto backend = std::make_unique<FakeAudioEngine>();
	FakeAudioEngine *backendView = backend.get();
	backendView->provideOutputDetails = true;
	XAudio2AudioService service(std::move(backend));
	check(service.open(), "spatialization service opens");
	const XAudio2PcmVoiceHandle monoHandle = service.createVoice();
	FakePcmVoiceBackend *monoVoice = backendView->lastVoiceBackend;
	const XAudio2PcmVoiceHandle stereoHandle = service.createVoice();
	FakePcmVoiceBackend *stereoVoice = backendView->lastVoiceBackend;
	check(monoHandle.isValid() && stereoHandle.isValid()
			&& monoVoice != nullptr && stereoVoice != nullptr,
		"spatialization service creates independent voices");
	AudioPcmChunk monoChunk = makeChunk(monoHandle.generation, 0);
	monoChunk.sourceChannels = 1;
	AudioPcmChunk stereoChunk = makeChunk(stereoHandle.generation, 0);
	stereoChunk.sourceChannels = 2;
	check(service.resetVoice(monoHandle, monoHandle.generation)
			&& service.resetVoice(stereoHandle, stereoHandle.generation),
		"spatialization voices accept their playback generations");
	check(service.submit(monoHandle, std::move(monoChunk)) == AudioPcmSubmitResult::ACCEPTED
			&& service.submit(stereoHandle, std::move(stereoChunk)) == AudioPcmSubmitResult::ACCEPTED,
		"spatialization voices accept mono and stereo provenance");
	XAudio2SpatializationPose listener = {};
	XAudio2SpatializationPose emitter = listener;
	emitter.position[2] = 1.0f;
	check(service.setVoiceSpatialization(monoHandle, listener, emitter)
			&& service.setVoiceSpatialization(stereoHandle, listener, emitter),
		"point-source spatialization succeeds for both provenances");
	check(monoVoice->lastSourceChannels == 2 && monoVoice->lastDestinationChannels == 2
			&& monoVoice->lastMatrix.size() == 4 && stereoVoice->lastMatrix.size() == 4,
		"point-source spatialization publishes stereo matrices");
	if (monoVoice->lastMatrix.size() == 4 && stereoVoice->lastMatrix.size() == 4) {
		for (std::size_t index = 0; index < 4; ++index) {
			check(std::abs(monoVoice->lastMatrix[index] * 2.0f
					- stereoVoice->lastMatrix[index]) < 0.0001f,
				"mono-expanded matrix is compensated without attenuating stereo");
		}
	}
	AudioPcmChunk unknownChunk = makeChunk(monoHandle.generation, 1);
	check(service.submit(monoHandle, std::move(unknownChunk)) == AudioPcmSubmitResult::ACCEPTED
			&& service.setVoiceSpatialization(monoHandle, listener, emitter),
		"accepted unknown provenance clears prior mono compensation");
	if (monoVoice->lastMatrix.size() == 4 && stereoVoice->lastMatrix.size() == 4) {
		for (std::size_t index = 0; index < 4; ++index) {
			check(std::abs(monoVoice->lastMatrix[index]
					- stereoVoice->lastMatrix[index]) < 0.0001f,
				"unknown provenance is not treated as mono-expanded audio");
		}
	}
	check(service.resetVoice(monoHandle, monoHandle.generation + 1)
			&& service.setVoiceSpatialization(monoHandle, listener, emitter),
		"voice reset clears prior asset provenance");
	if (monoVoice->lastMatrix.size() == 4 && stereoVoice->lastMatrix.size() == 4) {
		for (std::size_t index = 0; index < 4; ++index) {
			check(std::abs(monoVoice->lastMatrix[index]
					- stereoVoice->lastMatrix[index]) < 0.0001f,
				"reset voice does not retain mono compensation for its next asset");
		}
	}
	service.shutdown();
}

void testLegacyStereoPointSourceDirections()
{
	auto backend = std::make_unique<FakeAudioEngine>();
	FakeAudioEngine *backendView = backend.get();
	backendView->provideOutputDetails = true;
	XAudio2AudioService service(std::move(backend));
	check(service.open(), "legacy stereo direction service opens");
	const XAudio2PcmVoiceHandle handle = service.createVoice();
	FakePcmVoiceBackend *voice = backendView->lastVoiceBackend;
	AudioPcmChunk chunk = makeChunk(handle.generation, 0);
	chunk.sourceChannels = 1;
	check(handle.isValid() && voice != nullptr
			&& service.resetVoice(handle, handle.generation)
			&& service.submit(handle, std::move(chunk)) == AudioPcmSubmitResult::ACCEPTED,
		"legacy stereo direction voice accepts mono provenance");
	if (voice == nullptr) return;
	check(service.setVoiceVolume(handle, 0.37f), "direction test sets independent volume");
	const int volumeCalls = voice->setVolumeCalls;
	struct DirectionCase {
		float x, y, z;
		float left, right;
		const char *message;
	};
	const DirectionCase cases[] = {
		{ 0, 0, 50, .5f, .5f, "front center retains legacy half-gain per speaker" },
		{ 0, 0, -50, .375f, .375f, "rear center includes legacy .75 rear gain" },
		{ 50, 0, 0, 0, 1, "right side reaches the right speaker" },
		{ -50, 0, 0, 1, 0, "left side reaches the left speaker" },
		{ 50, 0, 50, .25f, .75f, "front diagonal uses angle-linear panning" },
		{ 50, 0, -50, .1875f, .5625f, "rear diagonal includes rear gain" },
		{ 0, -50, 50, .5f, .5f, "lower front center retains legacy center gain" },
		{ 50, -86.60254f, 0, 1.0f / 3.0f, 2.0f / 3.0f,
			"lower side uses full 3D direction instead of horizontal azimuth" },
		{ 50, 86.60254f, 0, 1.0f / 3.0f, 2.0f / 3.0f,
			"upper side uses the same elevation-aware split" },
		{ 0, 0, 0, .5f, .5f, "coincident source remains finite and centered" },
		{ .00001f, 0, 0, .5f, .5f, "near-coincident source uses legacy center branch" },
	};
	XAudio2SpatializationPose listener;
	for (const DirectionCase &test : cases) {
		XAudio2SpatializationPose emitter;
		emitter.position[0] = test.x;
		emitter.position[1] = test.y;
		emitter.position[2] = test.z;
		check(service.setVoiceSpatialization(handle, listener, emitter), test.message);
		check(voice->lastMatrix.size() == 4, "legacy stereo matrix has four coefficients");
		if (voice->lastMatrix.size() != 4) continue;
		check(std::abs(voice->lastMatrix[0] - test.left * .5f) < .00001f
				&& std::abs(voice->lastMatrix[1] - test.left * .5f) < .00001f
				&& std::abs(voice->lastMatrix[2] - test.right * .5f) < .00001f
				&& std::abs(voice->lastMatrix[3] - test.right * .5f) < .00001f,
			test.message);
	}
	XAudio2SpatializationPose emitter;
	listener.front[0] = 1.0f;
	listener.front[2] = 0.0f;
	emitter.position[0] = 50.0f;
	emitter.position[2] = -50.0f;
	check(service.setVoiceSpatialization(handle, listener, emitter)
			&& voice->lastMatrix.size() == 4
			&& std::abs(voice->lastMatrix[0] - .125f) < .00001f
			&& std::abs(voice->lastMatrix[2] - .375f) < .00001f,
		"rotated horizontal listener preserves right-front diagonal panning");
	const std::vector<float> validMatrix = voice->lastMatrix;
	emitter.position[0] = std::numeric_limits<float>::infinity();
	check(!service.setVoiceSpatialization(handle, listener, emitter)
			&& voice->lastMatrix == validMatrix,
		"infinite emitter does not publish a replacement matrix");
	emitter.position[0] = 50.0f;
	listener.front[0] = std::numeric_limits<float>::quiet_NaN();
	check(!service.setVoiceSpatialization(handle, listener, emitter)
			&& voice->lastMatrix == validMatrix,
		"nonfinite listener does not publish a replacement matrix");
	check(voice->setVolumeCalls == volumeCalls && voice->lastVolume == .37f,
		"direction matrix never changes the manager's independent gain");
	service.shutdown();
}

void testOtherSpatializationPathsKeepX3DAudio()
{
	for (bool multichannel : { false, true }) {
		auto backend = std::make_unique<FakeAudioEngine>();
		FakeAudioEngine *backendView = backend.get();
		backendView->provideOutputDetails = true;
		if (multichannel) {
			backendView->outputChannelMask = SPEAKER_5POINT1;
			backendView->outputChannelCount = 6;
		}
		XAudio2AudioService service(std::move(backend));
		check(service.open(), "unchanged X3DAudio path opens");
		const XAudio2PcmVoiceHandle handle = service.createVoice();
		FakePcmVoiceBackend *voice = backendView->lastVoiceBackend;
		AudioPcmChunk chunk = makeChunk(handle.generation, 0);
		chunk.sourceChannels = multichannel ? 1 : 2;
		check(handle.isValid() && voice != nullptr
				&& service.resetVoice(handle, handle.generation)
				&& service.submit(handle, std::move(chunk)) == AudioPcmSubmitResult::ACCEPTED,
			"unchanged path accepts its original channel provenance");
		if (voice == nullptr) continue;
		XAudio2SpatializationPose listenerPose;
		XAudio2SpatializationPose emitterPose;
		emitterPose.position[0] = 50.0f;
		emitterPose.position[1] = -50.0f;
		emitterPose.position[2] = -50.0f;
		check(service.setVoiceSpatialization(handle, listenerPose, emitterPose),
			"unchanged path spatialization succeeds");
		X3DAUDIO_HANDLE x3d;
		check(SUCCEEDED(X3DAudioInitialize(backendView->outputChannelMask,
			X3DAUDIO_SPEED_OF_SOUND, x3d)), "reference X3DAudio initializes");
		X3DAUDIO_LISTENER listener = {};
		listener.OrientFront = { 0, 0, 1 };
		listener.OrientTop = { 0, 1, 0 };
		float azimuths[2] = {};
		X3DAUDIO_DISTANCE_CURVE_POINT points[2] = { { 0, 1 }, { 1, 1 } };
		X3DAUDIO_DISTANCE_CURVE curve = { points, 2 };
		X3DAUDIO_EMITTER emitter = {};
		emitter.Position = { 50, -50, -50 };
		emitter.OrientFront = listener.OrientFront;
		emitter.OrientTop = listener.OrientTop;
		emitter.ChannelCount = 2;
		emitter.pChannelAzimuths = azimuths;
		emitter.pVolumeCurve = &curve;
		emitter.CurveDistanceScaler = 1;
		emitter.DopplerScaler = 1;
		std::vector<float> expected(2 * backendView->outputChannelCount);
		X3DAUDIO_DSP_SETTINGS settings = {};
		settings.SrcChannelCount = 2;
		settings.DstChannelCount = backendView->outputChannelCount;
		settings.pMatrixCoefficients = expected.data();
		X3DAudioCalculate(x3d, &listener, &emitter, X3DAUDIO_CALCULATE_MATRIX, &settings);
		check(voice->lastMatrix.size() == expected.size(), "unchanged matrix size matches X3DAudio");
		if (voice->lastMatrix.size() == expected.size()) {
			for (std::size_t index = 0; index < expected.size(); ++index) {
				const float normalization = multichannel ? .5f : 1.0f;
				check(std::abs(voice->lastMatrix[index] - expected[index] * normalization) < .00001f,
					"genuine stereo and multichannel mono retain X3DAudio coefficients");
			}
		}
		service.shutdown();
	}
}
}

int main()
{
	testFailurePublicationFenceAndOrdering();
	testCallbackGateAdmissionDrainAndReopen();
	testHandleScopedOperationsAndConcurrentStaleAccess();
	testDedicatedMovieOwnerDiscardsCompletions();
	testPartialBackendFailureIsDestroyedOnce();
	testConcurrentTransitionsPreserveFirstFailure();
	testInjectedLifecycleAndIndependentVoices();
	testOpenAndStartFailuresUnwindAndReopen();
	testSourceFailureDoesNotConsumeGeneration();
	testClosedFailedQuiescingAndStaleHandles();
	testShutdownOrderFailureAndIdempotence();
	testRepeatedCyclesRemainDeterministic();
	testPointSourceSpatializationPreservesMonoLevel();
	testLegacyStereoPointSourceDirections();
	testOtherSpatializationPathsKeepX3DAudio();
	if (g_failures != 0) {
		std::fprintf(stderr, "%d XAudio2AudioService checks failed\n", g_failures);
		return 1;
	}
	std::puts("XAudio2AudioService checks passed");
	return 0;
}
