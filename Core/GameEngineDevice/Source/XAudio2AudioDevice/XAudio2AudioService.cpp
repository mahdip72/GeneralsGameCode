#include "XAudio2AudioDevice/XAudio2AudioService.h"

#include "XAudio2AudioDevice/XAudio2NativeAudioEngine.h"
#include "XAudio2AudioDevice/XAudio2PcmVoice.h"

#include <new>

XAudio2AudioService::XAudio2AudioService() :
	XAudio2AudioService(std::make_unique<XAudio2NativeAudioEngine>())
{
}

XAudio2AudioService::XAudio2AudioService(std::unique_ptr<IXAudio2AudioEngineBackend> backend) :
	m_backend(std::move(backend)),
	m_nextHandleGeneration(1),
	m_state(XAudio2AudioServiceState::CLOSED),
	m_lastError(S_OK)
{
	if (m_backend == nullptr) {
		m_backend = std::make_unique<XAudio2NativeAudioEngine>();
	}
}

XAudio2AudioService::~XAudio2AudioService()
{
	shutdown();
}

HRESULT XAudio2AudioService::normalizeFailure(HRESULT error) noexcept
{
	return XAudio2FailurePublication::normalize(error);
}

XAudio2PcmVoiceHandle XAudio2AudioService::invalidHandle() const noexcept
{
	return XAudio2PcmVoiceHandle {};
}

bool XAudio2AudioService::open()
{
	std::lock_guard<std::mutex> lock(m_mutex);
	const XAudio2AudioServiceState currentState = m_state.load(std::memory_order_acquire);
	if (currentState == XAudio2AudioServiceState::RUNNING) {
		if (m_failurePublication.hasFailure()) {
			processPendingFailureLocked();
		}
		return m_state.load(std::memory_order_acquire) == XAudio2AudioServiceState::RUNNING
			&& !m_failurePublication.hasFailure();
	}
	if (currentState != XAudio2AudioServiceState::CLOSED) {
		return false;
	}

	// A previous failed cycle is consumed only after its resources are closed.
	// Advance past any retained publication before opening a new generation.
	m_failurePublication.clear();
	m_failureHandled = false;
	m_state.store(XAudio2AudioServiceState::OPENING, std::memory_order_release);
	m_lastError.store(S_OK, std::memory_order_release);

	auto unwind = [this](bool engineStarted, HRESULT fallback) noexcept {
		// Publish a fallback before changing state so a callback racing the owner
		// cannot publish a later error and make the result ambiguous.
		m_failurePublication.publish(fallback);
		m_state.store(XAudio2AudioServiceState::QUIESCING, std::memory_order_release);
		if (engineStarted) {
			const HRESULT stopResult = m_backend->stop();
			if (FAILED(stopResult)) {
				m_failurePublication.publish(stopResult);
			}
		}
		const HRESULT closeResult = m_backend->close();
		if (FAILED(closeResult)) {
			m_failurePublication.publish(closeResult);
		}
		const HRESULT firstFailure = m_failurePublication.failure();
		m_failurePublication.clear();
		m_failureHandled = false;
		m_state.store(XAudio2AudioServiceState::CLOSED, std::memory_order_release);
		m_lastError.store(firstFailure, std::memory_order_release);
		return false;
	};

	HRESULT result = m_backend->open(&XAudio2AudioService::criticalErrorThunk, this);
	if (FAILED(result)) {
		// Backend failures and callback failures use the same first-writer latch.
		m_failurePublication.publish(result);
	}
	if (FAILED(result)
		|| m_state.load(std::memory_order_acquire) != XAudio2AudioServiceState::OPENING
		|| m_failurePublication.hasFailure()) {
		return unwind(false, normalizeFailure(result));
	}

	result = m_backend->start();
	if (FAILED(result)) {
		m_failurePublication.publish(result);
	}
	if (FAILED(result) || m_failurePublication.hasFailure()
		|| m_state.load(std::memory_order_acquire) != XAudio2AudioServiceState::OPENING) {
		// Start may have partially initialized the backend, so stop before close.
		return unwind(true, normalizeFailure(result));
	}

	const std::uint64_t beforeCommit = m_failurePublication.snapshot();
	std::uint64_t committedPublication = 0;
	if (!m_failurePublication.tryCommit(beforeCommit, committedPublication)) {
		return unwind(true, E_FAIL);
	}

	XAudio2AudioServiceState expectedState = XAudio2AudioServiceState::OPENING;
	if (!m_state.compare_exchange_strong(expectedState, XAudio2AudioServiceState::RUNNING,
			std::memory_order_acq_rel, std::memory_order_acquire)) {
		return unwind(true, E_FAIL);
	}

	// The final publication check is the success linearization point.  A
	// callback that began before this check either won tryCommit above or makes
	// this validation fail before open returns success.
	if (m_failurePublication.snapshot() != committedPublication
		|| m_failurePublication.hasFailure()
		|| m_state.load(std::memory_order_acquire) != XAudio2AudioServiceState::RUNNING) {
		return unwind(true, E_FAIL);
	}
	return true;
}

bool XAudio2AudioService::processPendingFailureLocked() noexcept
{
	if (!m_failurePublication.hasFailure() || m_failureHandled) {
		return false;
	}

	// Do not clear the latch while the service remains FAILED.  Keeping the
	// first publication prevents a later callback from replacing the error
	// before shutdown/reopen establishes a new lifecycle generation.
	const HRESULT error = m_failurePublication.failure();
	m_lastError.store(error, std::memory_order_release);
	const XAudio2AudioServiceState currentState = m_state.load(std::memory_order_acquire);
	if (currentState != XAudio2AudioServiceState::CLOSED
		&& currentState != XAudio2AudioServiceState::QUIESCING) {
		m_state.store(XAudio2AudioServiceState::FAILED, std::memory_order_release);
	}
	for (VoiceRecord &record : m_voices) {
		if (record.voice != nullptr) {
			record.voice->failFromService(error);
		}
	}
	m_failureHandled = true;
	return true;
}

bool XAudio2AudioService::processPendingFailure() noexcept
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return processPendingFailureLocked();
}

void XAudio2AudioService::shutdown()
{
	std::lock_guard<std::mutex> lock(m_mutex);
	const XAudio2AudioServiceState currentState = m_state.load(std::memory_order_acquire);
	if (currentState == XAudio2AudioServiceState::CLOSED) {
		m_failurePublication.clear();
		m_failureHandled = false;
		return;
	}
	if (currentState == XAudio2AudioServiceState::QUIESCING) {
		return;
	}

	processPendingFailureLocked();
	m_state.store(XAudio2AudioServiceState::QUIESCING, std::memory_order_release);

	HRESULT firstFailure = m_failurePublication.hasFailure()
		? m_failurePublication.failure()
		: m_lastError.load(std::memory_order_acquire);
	for (VoiceRecord &record : m_voices) {
		if (record.voice != nullptr) {
			record.voice->close();
			const HRESULT voiceFailure = record.voice->getLastError();
			if (SUCCEEDED(firstFailure) && FAILED(voiceFailure)) {
				firstFailure = voiceFailure;
			}
			record.voice.reset();
		}
		record.backend.reset();
		record.generation = 0;
	}
	m_voices.clear();

	const HRESULT stopResult = m_backend->stop();
	if (FAILED(stopResult)) {
		m_failurePublication.publish(stopResult);
		if (SUCCEEDED(firstFailure)) {
			firstFailure = stopResult;
		}
	}
	const HRESULT closeResult = m_backend->close();
	if (FAILED(closeResult)) {
		m_failurePublication.publish(closeResult);
		if (SUCCEEDED(firstFailure)) {
			firstFailure = closeResult;
		}
	}

	if (SUCCEEDED(firstFailure) && m_failurePublication.hasFailure()) {
		firstFailure = m_failurePublication.failure();
	}
	m_failurePublication.clear();
	m_failureHandled = false;
	m_state.store(XAudio2AudioServiceState::CLOSED, std::memory_order_release);
	m_lastError.store(firstFailure, std::memory_order_release);
}

bool XAudio2AudioService::isOpen() const noexcept
{
	return m_state.load(std::memory_order_acquire) == XAudio2AudioServiceState::RUNNING
		&& !m_failurePublication.hasFailure();
}

XAudio2AudioServiceState XAudio2AudioService::state() const noexcept
{
	return m_state.load(std::memory_order_acquire);
}

HRESULT XAudio2AudioService::getLastError() const noexcept
{
	return m_lastError.load(std::memory_order_acquire);
}

bool XAudio2AudioService::isHandleOwnedLocked(XAudio2PcmVoiceHandle handle) const noexcept
{
	return handle.isValid() && handle.index < m_voices.size()
		&& m_voices[handle.index].generation == handle.generation
		&& m_voices[handle.index].voice != nullptr;
}

XAudio2PcmVoiceHandle XAudio2AudioService::createVoice() noexcept
{
	std::lock_guard<std::mutex> lock(m_mutex);
	processPendingFailureLocked();
	if (m_state.load(std::memory_order_acquire) != XAudio2AudioServiceState::RUNNING) {
		return invalidHandle();
	}
	const std::uint64_t beforeCommit = m_failurePublication.snapshot();

	std::unique_ptr<IXAudio2PcmVoiceBackend> backend;
	const HRESULT backendResult = m_backend->createPcmVoice(backend);
	if (FAILED(backendResult) || backend == nullptr) {
		if (backend != nullptr) {
			backend->destroy();
			backend.reset();
		}
		if (m_failurePublication.hasFailure()) {
			processPendingFailureLocked();
		} else {
			m_lastError.store(FAILED(backendResult) ? backendResult : normalizeFailure(backendResult),
				std::memory_order_release);
		}
		return invalidHandle();
	}

	std::unique_ptr<XAudio2PcmVoice> voice;
	try {
		voice = std::make_unique<XAudio2PcmVoice>(*backend);
	} catch (const std::bad_alloc &) {
		backend->destroy();
		if (m_failurePublication.hasFailure()) {
			processPendingFailureLocked();
		} else {
			m_lastError.store(E_OUTOFMEMORY, std::memory_order_release);
		}
		return invalidHandle();
	}

	if (!voice->open()) {
		const HRESULT failure = normalizeFailure(voice->getLastError());
		// XAudio2PcmVoice deliberately does not own a partially-created backend
		// when create() fails; the service owns this defensive unwind.
		backend->destroy();
		backend.reset();
		if (m_failurePublication.hasFailure()) {
			processPendingFailureLocked();
		} else {
			m_lastError.store(failure, std::memory_order_release);
		}
		return invalidHandle();
	}

	std::size_t index = m_voices.size();
	for (std::size_t i = 0; i < m_voices.size(); ++i) {
		if (m_voices[i].voice == nullptr) {
			index = i;
			break;
		}
	}
	try {
		if (index == m_voices.size()) {
			m_voices.emplace_back();
		}
	} catch (const std::bad_alloc &) {
		voice->close();
		backend.reset();
		if (m_failurePublication.hasFailure()) {
			processPendingFailureLocked();
		} else {
			m_lastError.store(E_OUTOFMEMORY, std::memory_order_release);
		}
		return invalidHandle();
	}

	std::uint64_t committedPublication = 0;
	const bool committed = m_state.load(std::memory_order_acquire)
		== XAudio2AudioServiceState::RUNNING
		&& !m_failurePublication.hasFailure()
		&& m_failurePublication.tryCommit(beforeCommit, committedPublication);
	if (!committed) {
		if (m_failurePublication.hasFailure()) {
			processPendingFailureLocked();
		} else {
			m_lastError.store(normalizeFailure(E_FAIL), std::memory_order_release);
		}
		voice->close();
		backend.reset();
		return invalidHandle();
	}
	if (m_state.load(std::memory_order_acquire) != XAudio2AudioServiceState::RUNNING
		|| m_failurePublication.hasFailure()
		|| m_failurePublication.snapshot() != committedPublication) {
		if (m_failurePublication.hasFailure()) {
			processPendingFailureLocked();
		} else {
			m_lastError.store(normalizeFailure(E_FAIL), std::memory_order_release);
		}
		voice->close();
		backend.reset();
		return invalidHandle();
	}

	VoiceRecord &record = m_voices[index];
	record.backend = std::move(backend);
	record.voice = std::move(voice);
	record.generation = m_nextHandleGeneration++;
	if (m_nextHandleGeneration == 0) {
		m_nextHandleGeneration = 1;
	}
	return XAudio2PcmVoiceHandle {
		static_cast<std::uint32_t>(index),
		record.generation
	};
}

bool XAudio2AudioService::destroyVoice(XAudio2PcmVoiceHandle handle) noexcept
{
	std::lock_guard<std::mutex> lock(m_mutex);
	processPendingFailureLocked();
	const XAudio2AudioServiceState currentState = m_state.load(std::memory_order_acquire);
	if (currentState == XAudio2AudioServiceState::CLOSED
		|| currentState == XAudio2AudioServiceState::QUIESCING
		|| !isHandleOwnedLocked(handle)) {
		return false;
	}

	VoiceRecord &record = m_voices[handle.index];
	record.voice->close();
	const HRESULT voiceFailure = record.voice->getLastError();
	record.voice.reset();
	record.backend.reset();
	record.generation = 0;
	if (FAILED(voiceFailure)) {
		m_lastError.store(voiceFailure, std::memory_order_release);
		return false;
	}
	return true;
}

AudioPcmSubmitResult XAudio2AudioService::submit(XAudio2PcmVoiceHandle handle, AudioPcmChunk &&chunk) noexcept
{
	std::lock_guard<std::mutex> lock(m_mutex);
	processPendingFailureLocked();
	const XAudio2AudioServiceState currentState = m_state.load(std::memory_order_acquire);
	if ((currentState != XAudio2AudioServiceState::RUNNING
			&& currentState != XAudio2AudioServiceState::FAILED)
		|| !isHandleOwnedLocked(handle)) {
		chunk = {};
		return AudioPcmSubmitResult::DROPPED;
	}
	if (currentState == XAudio2AudioServiceState::FAILED) {
		chunk = {};
		return AudioPcmSubmitResult::FAILED;
	}
	return m_voices[handle.index].voice->submit(std::move(chunk));
}

bool XAudio2AudioService::resetVoice(XAudio2PcmVoiceHandle handle, std::uint64_t generation) noexcept
{
	std::lock_guard<std::mutex> lock(m_mutex);
	processPendingFailureLocked();
	const XAudio2AudioServiceState currentState = m_state.load(std::memory_order_acquire);
	if ((currentState != XAudio2AudioServiceState::RUNNING
			&& currentState != XAudio2AudioServiceState::FAILED)
		|| !isHandleOwnedLocked(handle)) {
		return false;
	}
	m_voices[handle.index].voice->reset(generation);
	return true;
}

bool XAudio2AudioService::serviceVoice(XAudio2PcmVoiceHandle handle) noexcept
{
	std::lock_guard<std::mutex> lock(m_mutex);
	processPendingFailureLocked();
	const XAudio2AudioServiceState currentState = m_state.load(std::memory_order_acquire);
	if ((currentState != XAudio2AudioServiceState::RUNNING
			&& currentState != XAudio2AudioServiceState::FAILED)
		|| !isHandleOwnedLocked(handle)) {
		return false;
	}
	m_voices[handle.index].voice->service();
	return true;
}

bool XAudio2AudioService::setVoiceVolume(XAudio2PcmVoiceHandle handle, float volume) noexcept
{
	std::lock_guard<std::mutex> lock(m_mutex);
	processPendingFailureLocked();
	const XAudio2AudioServiceState currentState = m_state.load(std::memory_order_acquire);
	if ((currentState != XAudio2AudioServiceState::RUNNING
			&& currentState != XAudio2AudioServiceState::FAILED)
		|| !isHandleOwnedLocked(handle)) {
		return false;
	}
	return m_voices[handle.index].voice->setVolume(volume);
}

bool XAudio2AudioService::pauseVoice(XAudio2PcmVoiceHandle handle) noexcept
{
	std::lock_guard<std::mutex> lock(m_mutex);
	processPendingFailureLocked();
	const XAudio2AudioServiceState currentState = m_state.load(std::memory_order_acquire);
	if ((currentState != XAudio2AudioServiceState::RUNNING
			&& currentState != XAudio2AudioServiceState::FAILED)
		|| !isHandleOwnedLocked(handle)) {
		return false;
	}
	return m_voices[handle.index].voice->pause();
}

bool XAudio2AudioService::resumeVoice(XAudio2PcmVoiceHandle handle) noexcept
{
	std::lock_guard<std::mutex> lock(m_mutex);
	processPendingFailureLocked();
	const XAudio2AudioServiceState currentState = m_state.load(std::memory_order_acquire);
	if ((currentState != XAudio2AudioServiceState::RUNNING
			&& currentState != XAudio2AudioServiceState::FAILED)
		|| !isHandleOwnedLocked(handle)) {
		return false;
	}
	return m_voices[handle.index].voice->resume();
}

bool XAudio2AudioService::stopVoice(XAudio2PcmVoiceHandle handle) noexcept
{
	std::lock_guard<std::mutex> lock(m_mutex);
	processPendingFailureLocked();
	const XAudio2AudioServiceState currentState = m_state.load(std::memory_order_acquire);
	if ((currentState != XAudio2AudioServiceState::RUNNING
			&& currentState != XAudio2AudioServiceState::FAILED)
		|| !isHandleOwnedLocked(handle)) {
		return false;
	}
	return m_voices[handle.index].voice->stop();
}

bool XAudio2AudioService::isVoiceOpen(XAudio2PcmVoiceHandle handle) const noexcept
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return isHandleOwnedLocked(handle) && m_voices[handle.index].voice->isOpen();
}

bool XAudio2AudioService::isVoiceFailed(XAudio2PcmVoiceHandle handle) const noexcept
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return isHandleOwnedLocked(handle) && m_voices[handle.index].voice->isFailed();
}

bool XAudio2AudioService::isVoiceDrained(XAudio2PcmVoiceHandle handle) const noexcept
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return isHandleOwnedLocked(handle) && m_voices[handle.index].voice->isDrained();
}

HRESULT XAudio2AudioService::getVoiceLastError(XAudio2PcmVoiceHandle handle) const noexcept
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return isHandleOwnedLocked(handle) ? m_voices[handle.index].voice->getLastError() : E_HANDLE;
}

bool XAudio2AudioService::getVoicePlayedSample(XAudio2PcmVoiceHandle handle,
	std::int64_t &sample) const noexcept
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return isHandleOwnedLocked(handle) && m_voices[handle.index].voice->getPlayedSample(sample);
}

bool XAudio2AudioService::tryPopCompletion(XAudio2AudioCompletion &completion) noexcept
{
	std::lock_guard<std::mutex> lock(m_mutex);
	for (std::size_t index = 0; index < m_voices.size(); ++index) {
		VoiceRecord &record = m_voices[index];
		if (record.voice == nullptr) {
			continue;
		}
		XAudio2PcmCompletionRecord voiceCompletion;
		if (!record.voice->tryPopCompletion(voiceCompletion)) {
			continue;
		}
		completion.voice = XAudio2PcmVoiceHandle {
			static_cast<std::uint32_t>(index),
			record.generation
		};
		completion.generation = voiceCompletion.generation;
		completion.sequence = voiceCompletion.sequence;
		completion.endSample = voiceCompletion.endSample;
		return true;
	}
	return false;
}

void XAudio2AudioService::serviceVoices() noexcept
{
	std::lock_guard<std::mutex> lock(m_mutex);
	processPendingFailureLocked();
	const XAudio2AudioServiceState currentState = m_state.load(std::memory_order_acquire);
	if (currentState != XAudio2AudioServiceState::RUNNING
		&& currentState != XAudio2AudioServiceState::FAILED) {
		return;
	}
	for (VoiceRecord &record : m_voices) {
		if (record.voice != nullptr) {
			record.voice->service();
		}
	}
}

void XAudio2AudioService::criticalErrorThunk(void *context, HRESULT error) noexcept
{
	XAudio2AudioService *service = static_cast<XAudio2AudioService *>(context);
	if (service == nullptr) {
		return;
	}
	const XAudio2AudioServiceState currentState = service->m_state.load(std::memory_order_acquire);
	if (currentState == XAudio2AudioServiceState::CLOSED
		|| currentState == XAudio2AudioServiceState::QUIESCING) {
		return;
	}
	// The publication CAS is the callback's first observable action.  Owner
	// transactions compete with this same word, so a callback that began before
	// their commit cannot be lost behind a separate state/sequence read.
	service->m_failurePublication.publish(error);
	XAudio2AudioServiceState expectedState = currentState;
	while (expectedState != XAudio2AudioServiceState::CLOSED
		&& expectedState != XAudio2AudioServiceState::QUIESCING
		&& expectedState != XAudio2AudioServiceState::FAILED) {
		if (service->m_state.compare_exchange_weak(expectedState,
				XAudio2AudioServiceState::FAILED, std::memory_order_acq_rel,
				std::memory_order_acquire)) {
			break;
		}
	}
}
