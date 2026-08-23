#include "XAudio2AudioDevice/XAudio2PcmVoice.h"

#if defined(RTS_XAUDIO2_PCM_HAS_FFMPEG)
#include "VideoDevice/FFmpeg/FFmpegAudioDecoder.h"
#endif

#include <limits>

namespace
{
constexpr std::uint16_t PCM_CHANNELS = 2;
constexpr std::uint32_t PCM_SAMPLE_RATE = 48000;
constexpr std::uint16_t PCM_BITS_PER_SAMPLE = 16;
constexpr std::size_t PCM_BYTES_PER_FRAME = PCM_CHANNELS * sizeof(std::int16_t);

#if defined(RTS_XAUDIO2_PCM_HAS_FFMPEG)
constexpr std::uint32_t PCM_MAX_CHUNK_FRAMES = FFmpegAudioDecoder::MAX_CHUNK_FRAMES;
#else
// Keep the XAudio-only graph usable when the optional FFmpeg graph is disabled.
// FFmpegAudioDecoder::MAX_CHUNK_FRAMES is the same fixed one-second bound.
constexpr std::uint32_t PCM_MAX_CHUNK_FRAMES = PCM_SAMPLE_RATE;
#endif
}

XAudio2PcmVoice::XAudio2PcmVoice(IXAudio2PcmVoiceBackend &backend) :
	m_backend(backend),
	m_open(false),
	m_failed(false),
	m_callbackError(false),
	m_callbackErrorCode(S_OK),
	m_lastError(S_OK),
	m_requestedGeneration(0),
	m_activeGeneration(0),
	m_resetPending(false),
	m_started(false),
	m_backendCreated(false)
{
}

XAudio2PcmVoice::~XAudio2PcmVoice()
{
	close();
}

WAVEFORMATEX XAudio2PcmVoice::pcmFormat()
{
	WAVEFORMATEX format = {};
	format.wFormatTag = WAVE_FORMAT_PCM;
	format.nChannels = PCM_CHANNELS;
	format.nSamplesPerSec = PCM_SAMPLE_RATE;
	format.nAvgBytesPerSec = PCM_SAMPLE_RATE * PCM_BYTES_PER_FRAME;
	format.nBlockAlign = static_cast<WORD>(PCM_BYTES_PER_FRAME);
	format.wBitsPerSample = PCM_BITS_PER_SAMPLE;
	format.cbSize = 0;
	return format;
}

bool XAudio2PcmVoice::isValidChunk(const AudioPcmChunk &chunk)
{
	if (chunk.sampleRate != PCM_SAMPLE_RATE || chunk.channels != PCM_CHANNELS
		|| chunk.format != AudioPcmFormat::SIGNED_16_INTERLEAVED_LITTLE_ENDIAN
		|| chunk.frameCount == 0 || chunk.frameCount > PCM_MAX_CHUNK_FRAMES || chunk.startSample < 0) {
		return false;
	}
	if (chunk.frameCount > (std::numeric_limits<std::size_t>::max)() / PCM_BYTES_PER_FRAME) {
		return false;
	}
	return chunk.data.size() == static_cast<std::size_t>(chunk.frameCount) * PCM_BYTES_PER_FRAME;
}

bool XAudio2PcmVoice::open()
{
	std::lock_guard<std::mutex> lock(m_mutex);
	if (m_open.load(std::memory_order_acquire)) {
		return !m_failed.load(std::memory_order_acquire);
	}
	if (m_failed.load(std::memory_order_acquire)) {
		return false;
	}

	m_callbackError.store(false, std::memory_order_release);
	m_callbackErrorCode.store(S_OK, std::memory_order_release);
	m_lastError.store(S_OK, std::memory_order_release);
	const HRESULT result = m_backend.create(pcmFormat(), this);
	if (FAILED(result)) {
		m_lastError.store(result, std::memory_order_release);
		m_failed.store(true, std::memory_order_release);
		return false;
	}

	m_backendCreated = true;
	m_started = false;
	m_activeGeneration = m_requestedGeneration;
	m_resetPending = false;
	m_failed.store(false, std::memory_order_release);
	m_open.store(true, std::memory_order_release);
	return true;
}

void XAudio2PcmVoice::clearSlot(Slot &slot)
{
	slot.buffer = {};
	slot.chunk = {};
	slot.generation = 0;
	slot.sequence = 0;
	slot.cancelled = false;
	slot.callbackComplete.store(false, std::memory_order_release);
	slot.state = SlotState::FREE;
}

XAudio2PcmVoice::Slot *XAudio2PcmVoice::findFreeSlot()
{
	for (Slot &slot : m_slots) {
		if (slot.state == SlotState::FREE) {
			return &slot;
		}
	}
	return nullptr;
}

XAudio2PcmVoice::Slot *XAudio2PcmVoice::findNextPendingSlot()
{
	Slot *next = nullptr;
	for (Slot &slot : m_slots) {
		if (slot.state != SlotState::PENDING || slot.generation != m_activeGeneration) {
			continue;
		}
		if (next == nullptr || slot.sequence < next->sequence) {
			next = &slot;
		}
	}
	return next;
}

void XAudio2PcmVoice::reclaimCompletedSlots()
{
	for (Slot &slot : m_slots) {
		if (slot.state == SlotState::SUBMITTED
			&& slot.callbackComplete.exchange(false, std::memory_order_acq_rel)) {
			clearSlot(slot);
		}
	}
}

bool XAudio2PcmVoice::hasSubmittedOldSlot() const
{
	for (const Slot &slot : m_slots) {
		if (slot.state == SlotState::SUBMITTED && slot.cancelled) {
			return true;
		}
	}
	return false;
}

bool XAudio2PcmVoice::consumeCallbackError(HRESULT &error)
{
	if (!m_callbackError.exchange(false, std::memory_order_acq_rel)) {
		return false;
	}
	error = m_callbackErrorCode.load(std::memory_order_acquire);
	if (SUCCEEDED(error)) {
		error = E_FAIL;
	}
	return true;
}

void XAudio2PcmVoice::fail(HRESULT error)
{
	if (SUCCEEDED(error)) {
		error = E_FAIL;
	}
	m_lastError.store(error, std::memory_order_release);
	m_failed.store(true, std::memory_order_release);
}

bool XAudio2PcmVoice::checkExternalFailure()
{
	HRESULT callbackError = S_OK;
	if (consumeCallbackError(callbackError)) {
		fail(callbackError);
		return true;
	}
	const HRESULT criticalError = m_backend.getCriticalError();
	if (FAILED(criticalError)) {
		fail(criticalError);
		return true;
	}
	return false;
}

void XAudio2PcmVoice::service()
{
	std::lock_guard<std::mutex> lock(m_mutex);
	if (!m_open.load(std::memory_order_acquire)) {
		return;
	}

	reclaimCompletedSlots();
	if (m_failed.load(std::memory_order_acquire)) {
		return;
	}
	if (checkExternalFailure()) {
		return;
	}

	if (m_resetPending) {
		if (hasSubmittedOldSlot()) {
			return;
		}
		HRESULT result = m_backend.stop();
		if (FAILED(result)) {
			fail(result);
			return;
		}
		if (checkExternalFailure()) {
			return;
		}
		result = m_backend.flush();
		if (FAILED(result)) {
			fail(result);
			return;
		}
		if (checkExternalFailure()) {
			return;
		}
		m_started = false;
		m_activeGeneration = m_requestedGeneration;
		m_resetPending = false;
	}

	for (;;) {
		Slot *slot = findNextPendingSlot();
		if (slot == nullptr) {
			return;
		}
		slot->state = SlotState::SUBMITTED;
		slot->callbackComplete.store(false, std::memory_order_release);
		const HRESULT result = m_backend.submit(slot->buffer);
		if (FAILED(result)) {
			fail(result);
			return;
		}
		if (checkExternalFailure()) {
			return;
		}
		if (!m_started) {
			const HRESULT startResult = m_backend.start();
			if (FAILED(startResult)) {
				fail(startResult);
				return;
			}
			m_started = true;
			if (checkExternalFailure()) {
				return;
			}
		}
	}
}

void XAudio2PcmVoice::close()
{
	bool destroyBackend = false;
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_open.store(false, std::memory_order_release);
		destroyBackend = m_backendCreated;
	}

	// The backend contract guarantees that destroy has quiesced callbacks before returning.
	if (destroyBackend) {
		m_backend.destroy();
	}

	std::lock_guard<std::mutex> lock(m_mutex);
	for (Slot &slot : m_slots) {
		clearSlot(slot);
	}
	m_backendCreated = false;
	m_started = false;
	m_resetPending = false;
	m_activeGeneration = m_requestedGeneration;
	m_callbackError.store(false, std::memory_order_release);
	m_callbackErrorCode.store(S_OK, std::memory_order_release);
	m_failed.store(false, std::memory_order_release);
}

bool XAudio2PcmVoice::isOpen() const noexcept
{
	return m_open.load(std::memory_order_acquire);
}

bool XAudio2PcmVoice::isFailed() const noexcept
{
	return m_failed.load(std::memory_order_acquire);
}

HRESULT XAudio2PcmVoice::getLastError() const noexcept
{
	return m_lastError.load(std::memory_order_acquire);
}

void XAudio2PcmVoice::failFromService(HRESULT error) noexcept
{
	std::lock_guard<std::mutex> lock(m_mutex);
	if (m_open.load(std::memory_order_acquire)) {
		fail(error);
	}
}

AudioPcmSubmitResult XAudio2PcmVoice::submit(AudioPcmChunk &&chunk)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	auto drop = [&chunk]() {
		chunk = {};
		return AudioPcmSubmitResult::DROPPED;
	};
	if (!m_open.load(std::memory_order_acquire) || m_failed.load(std::memory_order_acquire)
		|| chunk.generation != m_requestedGeneration || !isValidChunk(chunk)) {
		return drop();
	}

	Slot *slot = findFreeSlot();
	if (slot == nullptr) {
		return drop();
	}
	// The producer owns the chunk until this move completes; the slot then owns all
	// storage referenced by its stable XAUDIO2_BUFFER.
	slot->chunk = std::move(chunk);
	slot->generation = slot->chunk.generation;
	slot->sequence = slot->chunk.sequence;
	slot->buffer = {};
	slot->buffer.AudioBytes = static_cast<UINT32>(slot->chunk.data.size());
	slot->buffer.pAudioData = slot->chunk.data.data();
	slot->buffer.pContext = slot;
	slot->callbackComplete.store(false, std::memory_order_release);
	slot->cancelled = false;
	slot->state = SlotState::PENDING;
	return AudioPcmSubmitResult::ACCEPTED;
}

void XAudio2PcmVoice::reset(std::uint64_t generation)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	const bool repeatedPendingReset = m_resetPending && m_requestedGeneration == generation;
	m_requestedGeneration = generation;
	if (!m_open.load(std::memory_order_acquire)) {
		m_activeGeneration = generation;
		m_resetPending = false;
		return;
	}
	if (m_failed.load(std::memory_order_acquire)) {
		return;
	}

	m_resetPending = true;
	if (!repeatedPendingReset) {
		for (Slot &slot : m_slots) {
			if (slot.state == SlotState::PENDING) {
				clearSlot(slot);
			}
		}
	}
	for (Slot &slot : m_slots) {
		if (slot.state == SlotState::SUBMITTED) {
			slot.cancelled = true;
		}
	}
}

void STDMETHODCALLTYPE XAudio2PcmVoice::OnVoiceProcessingPassStart(UINT32)
{
}

void STDMETHODCALLTYPE XAudio2PcmVoice::OnVoiceProcessingPassEnd()
{
}

void STDMETHODCALLTYPE XAudio2PcmVoice::OnStreamEnd()
{
}

void STDMETHODCALLTYPE XAudio2PcmVoice::OnBufferStart(void *)
{
}

void STDMETHODCALLTYPE XAudio2PcmVoice::OnBufferEnd(void *pBufferContext)
{
	for (Slot &slot : m_slots) {
		if (pBufferContext == static_cast<void *>(&slot)) {
			slot.callbackComplete.store(true, std::memory_order_release);
			return;
		}
	}
}

void STDMETHODCALLTYPE XAudio2PcmVoice::OnLoopEnd(void *)
{
}

void STDMETHODCALLTYPE XAudio2PcmVoice::OnVoiceError(void *, HRESULT Error)
{
	m_callbackErrorCode.store(Error, std::memory_order_release);
	m_lastError.store(FAILED(Error) ? Error : E_FAIL, std::memory_order_release);
	m_callbackError.store(true, std::memory_order_release);
	m_failed.store(true, std::memory_order_release);
}
