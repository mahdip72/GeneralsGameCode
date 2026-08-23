#include "XAudio2AudioDevice/IXAudio2AudioEngineBackend.h"
#include "XAudio2AudioDevice/XAudio2AudioService.h"
#include "XAudio2AudioDevice/XAudio2PcmVoice.h"

#include <atomic>
#include <cstdio>
#include <memory>
#include <thread>
#include <string>
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

	HRESULT submit(const XAUDIO2_BUFFER &) noexcept override { return S_OK; }
	HRESULT start() noexcept override { return S_OK; }
	HRESULT stop() noexcept override { return S_OK; }
	HRESULT flush() noexcept override { return S_OK; }
	HRESULT getCriticalError() const noexcept override { return S_OK; }

	void destroy() noexcept override
	{
		m_callback = nullptr;
		++m_destroyCount;
		m_calls.push_back("voice.destroy");
	}

private:
	std::vector<std::string> &m_calls;
	int &m_destroyCount;
	HRESULT m_createResult;
	IXAudio2VoiceCallback *m_callback = nullptr;
};

class FakeAudioEngine final : public IXAudio2AudioEngineBackend
{
public:
	std::vector<std::string> calls;
	int voiceDestroyCount = 0;
	HRESULT openResult = S_OK;
	HRESULT startResult = S_OK;
	HRESULT createVoiceResult = S_OK;
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
		return startResult;
	}

	HRESULT createPcmVoice(std::unique_ptr<IXAudio2PcmVoiceBackend> &voice) noexcept override
	{
		++createVoiceCalls;
		calls.push_back("voice.create.backend");
		if (FAILED(createVoiceResult)) {
			return createVoiceResult;
		}
		voice = std::make_unique<FakePcmVoiceBackend>(calls, voiceDestroyCount, voiceCreateResult);
		return createVoiceResult;
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
	check(service.getVoice(first) != nullptr && service.getVoice(second) != nullptr
			&& service.getVoice(third) != nullptr,
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
	check(service.getVoice(first) != nullptr, "a source create failure preserves existing records");
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
	check(!service.getVoice(first) && !service.destroyVoice(first),
		"destroyed handles cannot access or destroy a replacement record");
	const XAudio2PcmVoiceHandle replacement = service.createVoice();
	check(replacement.isValid() && replacement.index == first.index
			&& replacement.generation != first.generation,
		"reused slots receive a new generation");
	check(service.getVoice(first) == nullptr && service.getVoice(replacement) != nullptr,
		"stale handles remain rejected after slot reuse");

	backendView->emitCritical(E_ABORT);
	check(service.state() == XAudio2AudioServiceState::FAILED,
		"a critical callback atomically publishes FAILED");
	check(service.processPendingFailure(), "the owner observes the pending critical error");
	check(service.getVoice(replacement) != nullptr && service.getVoice(replacement)->isFailed(),
		"existing voices observe terminal failure on the owner service");
	check(!service.createVoice().isValid(), "failed services reject new voices");

	service.shutdown();
	check(backendView->quiescingObserved, "shutdown publishes QUIESCING before backend close");
	check(!service.createVoice().isValid(), "closed services reject new voices after shutdown");
	check(service.getVoice(replacement) == nullptr, "shutdown invalidates all voice handles");

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
}

int main()
{
	testInjectedLifecycleAndIndependentVoices();
	testOpenAndStartFailuresUnwindAndReopen();
	testSourceFailureDoesNotConsumeGeneration();
	testClosedFailedQuiescingAndStaleHandles();
	testShutdownOrderFailureAndIdempotence();
	testRepeatedCyclesRemainDeterministic();
	if (g_failures != 0) {
		std::fprintf(stderr, "%d XAudio2AudioService checks failed\n", g_failures);
		return 1;
	}
	std::puts("XAudio2AudioService checks passed");
	return 0;
}
