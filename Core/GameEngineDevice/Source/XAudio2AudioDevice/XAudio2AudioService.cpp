#include "XAudio2AudioDevice/XAudio2AudioService.h"

#include "XAudio2AudioDevice/XAudio2NativeAudioEngine.h"
#include "XAudio2AudioDevice/XAudio2PcmVoice.h"

#include <new>

namespace
{
constexpr HRESULT NORMALIZED_FAILURE = E_FAIL;
}

XAudio2AudioService::XAudio2AudioService() :
	XAudio2AudioService(std::make_unique<XAudio2NativeAudioEngine>())
{
}

XAudio2AudioService::XAudio2AudioService(std::unique_ptr<IXAudio2AudioEngineBackend> backend) :
	m_backend(std::move(backend)),
	m_nextHandleGeneration(1),
	m_state(XAudio2AudioServiceState::CLOSED),
	m_lastError(S_OK),
	m_pendingCriticalError(false),
	m_pendingCriticalErrorCode(S_OK)
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
	return FAILED(error) ? error : NORMALIZED_FAILURE;
}

XAudio2PcmVoiceHandle XAudio2AudioService::invalidHandle() const noexcept
{
	return XAudio2PcmVoiceHandle {};
}

bool XAudio2AudioService::open()
{
	std::lock_guard<std::mutex> lock(m_mutex);
	if (m_state.load(std::memory_order_acquire) == XAudio2AudioServiceState::RUNNING) {
		return true;
	}
	if (m_state.load(std::memory_order_acquire) != XAudio2AudioServiceState::CLOSED) {
		return false;
	}

	m_state.store(XAudio2AudioServiceState::OPENING, std::memory_order_release);
	m_pendingCriticalError.store(false, std::memory_order_release);
	m_pendingCriticalErrorCode.store(S_OK, std::memory_order_release);
	m_lastError.store(S_OK, std::memory_order_release);

	HRESULT result = m_backend->open(&XAudio2AudioService::criticalErrorThunk, this);
	if (FAILED(result)) {
		const HRESULT originalFailure = normalizeFailure(result);
		m_backend->close();
		m_lastError.store(originalFailure, std::memory_order_release);
		m_state.store(XAudio2AudioServiceState::CLOSED, std::memory_order_release);
		return false;
	}

	result = m_backend->start();
	if (FAILED(result) || m_pendingCriticalError.load(std::memory_order_acquire)) {
		const HRESULT originalFailure = FAILED(result)
			? normalizeFailure(result)
			: normalizeFailure(m_pendingCriticalErrorCode.load(std::memory_order_acquire));
		// A failed StartEngine may have partially started the engine.  Stop is
		// intentionally attempted once before the common close path.
		m_backend->stop();
		m_backend->close();
		m_lastError.store(originalFailure, std::memory_order_release);
		m_pendingCriticalError.store(false, std::memory_order_release);
		m_state.store(XAudio2AudioServiceState::CLOSED, std::memory_order_release);
		return false;
	}

	m_state.store(XAudio2AudioServiceState::RUNNING, std::memory_order_release);
	return true;
}

bool XAudio2AudioService::processPendingFailureLocked() noexcept
{
	if (!m_pendingCriticalError.exchange(false, std::memory_order_acq_rel)) {
		return false;
	}

	const HRESULT error = normalizeFailure(m_pendingCriticalErrorCode.load(std::memory_order_acquire));
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
		return;
	}
	if (currentState == XAudio2AudioServiceState::QUIESCING) {
		return;
	}

	processPendingFailureLocked();
	m_state.store(XAudio2AudioServiceState::QUIESCING, std::memory_order_release);

	HRESULT firstFailure = m_lastError.load(std::memory_order_acquire);
	for (VoiceRecord &record : m_voices) {
		if (record.voice != nullptr) {
			record.voice->close();
			record.voice.reset();
		}
		record.backend.reset();
		record.generation = 0;
	}
	m_voices.clear();

	const HRESULT stopResult = m_backend->stop();
	if (SUCCEEDED(firstFailure) && FAILED(stopResult)) {
		firstFailure = normalizeFailure(stopResult);
	}
	const HRESULT closeResult = m_backend->close();
	if (SUCCEEDED(firstFailure) && FAILED(closeResult)) {
		firstFailure = normalizeFailure(closeResult);
	}

	m_pendingCriticalError.store(false, std::memory_order_release);
	m_pendingCriticalErrorCode.store(S_OK, std::memory_order_release);
	m_state.store(XAudio2AudioServiceState::CLOSED, std::memory_order_release);
	m_lastError.store(firstFailure, std::memory_order_release);
}

bool XAudio2AudioService::isOpen() const noexcept
{
	return m_state.load(std::memory_order_acquire) == XAudio2AudioServiceState::RUNNING;
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

	std::unique_ptr<IXAudio2PcmVoiceBackend> backend;
	const HRESULT backendResult = m_backend->createPcmVoice(backend);
	if (FAILED(backendResult) || backend == nullptr) {
		m_lastError.store(FAILED(backendResult) ? backendResult : NORMALIZED_FAILURE,
			std::memory_order_release);
		return invalidHandle();
	}

	std::unique_ptr<XAudio2PcmVoice> voice;
	try {
		voice = std::make_unique<XAudio2PcmVoice>(*backend);
	} catch (const std::bad_alloc &) {
		backend->destroy();
		m_lastError.store(E_OUTOFMEMORY, std::memory_order_release);
		return invalidHandle();
	}

	if (!voice->open()) {
		const HRESULT failure = normalizeFailure(voice->getLastError());
		// XAudio2PcmVoice deliberately does not own a partially-created backend
		// when create() fails; the service owns this defensive unwind.
		backend->destroy();
		m_lastError.store(failure, std::memory_order_release);
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
		m_lastError.store(E_OUTOFMEMORY, std::memory_order_release);
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
	record.voice.reset();
	record.backend.reset();
	record.generation = 0;
	return true;
}

XAudio2PcmVoice *XAudio2AudioService::getVoice(XAudio2PcmVoiceHandle handle) noexcept
{
	std::lock_guard<std::mutex> lock(m_mutex);
	processPendingFailureLocked();
	return isHandleOwnedLocked(handle) ? m_voices[handle.index].voice.get() : nullptr;
}

const XAudio2PcmVoice *XAudio2AudioService::getVoice(XAudio2PcmVoiceHandle handle) const noexcept
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return isHandleOwnedLocked(handle) ? m_voices[handle.index].voice.get() : nullptr;
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
	service->m_pendingCriticalErrorCode.store(normalizeFailure(error), std::memory_order_release);
	service->m_pendingCriticalError.store(true, std::memory_order_release);
	service->m_state.store(XAudio2AudioServiceState::FAILED, std::memory_order_release);
}
