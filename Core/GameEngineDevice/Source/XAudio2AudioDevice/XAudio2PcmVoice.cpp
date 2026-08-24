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
constexpr std::uint64_t CALLBACK_STATE_BITS = 3;
constexpr std::uint64_t CALLBACK_STATE_MASK = (1ULL << CALLBACK_STATE_BITS) - 1ULL;
constexpr std::uint64_t CALLBACK_STATE_PENDING = 1;
constexpr std::uint64_t CALLBACK_STATE_SUBMITTED = 2;
constexpr std::uint64_t CALLBACK_STATE_IN_FLIGHT = 3;
constexpr std::uint64_t CALLBACK_STATE_COMPLETE = 4;

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
	m_playedSample(-1),
	m_playedGeneration(0),
	m_requestedGeneration(0),
	m_activeGeneration(0),
	m_nextCallbackToken(1),
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

std::uint64_t XAudio2PcmVoice::encodeCallbackIdentity(std::uint64_t token,
	std::uint64_t state) noexcept
{
	return (token << CALLBACK_STATE_BITS) | state;
}

std::uint64_t XAudio2PcmVoice::callbackIdentityToken(std::uint64_t identity) noexcept
{
	return identity >> CALLBACK_STATE_BITS;
}

std::uint64_t XAudio2PcmVoice::callbackIdentityState(std::uint64_t identity) noexcept
{
	return identity & CALLBACK_STATE_MASK;
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
	m_playedSample.store(-1, std::memory_order_release);
	m_playedGeneration.store(m_requestedGeneration, std::memory_order_release);
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
	// Withdraw callback admission before touching the metadata.  Reclamation
	// only reaches this point after a callback published COMPLETE.
	slot.callbackIdentity.store(0, std::memory_order_release);
	slot.buffer = {};
	slot.chunk = {};
	slot.generation = 0;
	slot.sequence = 0;
	slot.cancelled = false;
	slot.state.store(SlotState::FREE, std::memory_order_release);
}

XAudio2PcmVoice::Slot *XAudio2PcmVoice::findFreeSlot()
{
	for (Slot &slot : m_slots) {
		if (slot.state.load(std::memory_order_acquire) == SlotState::FREE) {
			return &slot;
		}
	}
	return nullptr;
}

XAudio2PcmVoice::Slot *XAudio2PcmVoice::findNextPendingSlot()
{
	Slot *next = nullptr;
	for (Slot &slot : m_slots) {
		if (slot.state.load(std::memory_order_acquire) != SlotState::PENDING
			|| slot.generation != m_activeGeneration) {
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
		if (slot.state.load(std::memory_order_acquire) == SlotState::SUBMITTED
			&& callbackIdentityState(slot.callbackIdentity.load(std::memory_order_acquire))
				== CALLBACK_STATE_COMPLETE) {
			clearSlot(slot);
		}
	}
}

bool XAudio2PcmVoice::hasSubmittedOldSlot() const
{
	for (const Slot &slot : m_slots) {
		if (slot.state.load(std::memory_order_acquire) == SlotState::SUBMITTED && slot.cancelled) {
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

void XAudio2PcmVoice::publishCallbackError(HRESULT error) noexcept
{
	if (SUCCEEDED(error)) {
		error = E_FAIL;
	}
	m_callbackErrorCode.store(error, std::memory_order_release);
	m_lastError.store(error, std::memory_order_release);
	m_callbackError.store(true, std::memory_order_release);
	m_failed.store(true, std::memory_order_release);
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
		const std::uint64_t token = callbackIdentityToken(
			slot->callbackIdentity.load(std::memory_order_acquire));
		slot->callbackIdentity.store(encodeCallbackIdentity(token, CALLBACK_STATE_SUBMITTED),
			std::memory_order_release);
		slot->state.store(SlotState::SUBMITTED, std::memory_order_release);
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

void XAudio2PcmVoice::close() noexcept
{
	bool destroyBackend = false;
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_open.store(false, std::memory_order_release);
		destroyBackend = m_backendCreated;
	}

	// The backend contract guarantees that destroy has quiesced callbacks before returning.
	if (destroyBackend) {
		const HRESULT destroyResult = m_backend.destroyWithResult();
		if (FAILED(destroyResult)) {
			HRESULT expected = S_OK;
			if (m_lastError.compare_exchange_strong(expected, destroyResult,
				std::memory_order_acq_rel, std::memory_order_acquire)) {
				m_failed.store(true, std::memory_order_release);
			}
		}
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
	m_playedSample.store(-1, std::memory_order_release);
	m_playedGeneration.store(m_requestedGeneration, std::memory_order_release);
	for (CompletionSlot &completion : m_completions) {
		completion.ready.store(false, std::memory_order_release);
	}
	m_completionRead.store(0, std::memory_order_release);
	m_completionWrite.store(0, std::memory_order_release);
	m_failed.store(false, std::memory_order_release);
}

bool XAudio2PcmVoice::getPlayedSample(std::int64_t &sample) const noexcept
{
	const std::int64_t played = m_playedSample.load(std::memory_order_acquire);
	if (played < 0) {
		return false;
	}
	sample = played;
	return true;
}

bool XAudio2PcmVoice::isOpen() const noexcept
{
	return m_open.load(std::memory_order_acquire);
}

bool XAudio2PcmVoice::isFailed() const noexcept
{
	return m_failed.load(std::memory_order_acquire);
}

bool XAudio2PcmVoice::isDrained() const noexcept
{
	std::lock_guard<std::mutex> lock(m_mutex);
	if (!m_open.load(std::memory_order_acquire)
		|| m_failed.load(std::memory_order_acquire) || m_resetPending) {
		return false;
	}
	for (const Slot &slot : m_slots) {
		if (slot.state.load(std::memory_order_acquire) != SlotState::FREE) {
			return false;
		}
	}
	return true;
}

HRESULT XAudio2PcmVoice::getLastError() const noexcept
{
	return m_lastError.load(std::memory_order_acquire);
}

bool XAudio2PcmVoice::setVolume(float volume) noexcept
{
	std::lock_guard<std::mutex> lock(m_mutex);
	if (!m_open.load(std::memory_order_acquire)
		|| m_failed.load(std::memory_order_acquire)) {
		return false;
	}
	const HRESULT result = m_backend.setVolume(volume);
	if (FAILED(result)) {
		fail(result);
		return false;
	}
	return true;
}

bool XAudio2PcmVoice::setOutputMatrix(UINT32 sourceChannels, UINT32 destinationChannels,
	const float *matrix) noexcept
{
	std::lock_guard<std::mutex> lock(m_mutex);
	if (!m_open.load(std::memory_order_acquire)
		|| m_failed.load(std::memory_order_acquire) || matrix == nullptr) {
		return false;
	}
	const HRESULT result = m_backend.setOutputMatrix(sourceChannels, destinationChannels, matrix);
	if (result == E_NOTIMPL) {
		return true;
	}
	if (FAILED(result)) {
		fail(result);
		return false;
	}
	return true;
}

bool XAudio2PcmVoice::pause() noexcept
{
	std::lock_guard<std::mutex> lock(m_mutex);
	if (!m_open.load(std::memory_order_acquire)
		|| m_failed.load(std::memory_order_acquire)) {
		return false;
	}
	const HRESULT result = m_backend.pause();
	if (FAILED(result)) {
		fail(result);
		return false;
	}
	m_started = false;
	return true;
}

bool XAudio2PcmVoice::resume() noexcept
{
	std::lock_guard<std::mutex> lock(m_mutex);
	if (!m_open.load(std::memory_order_acquire)
		|| m_failed.load(std::memory_order_acquire)) {
		return false;
	}
	const HRESULT result = m_backend.resume();
	if (FAILED(result)) {
		fail(result);
		return false;
	}
	m_started = true;
	return true;
}

bool XAudio2PcmVoice::stop() noexcept
{
	std::lock_guard<std::mutex> lock(m_mutex);
	if (!m_open.load(std::memory_order_acquire)
		|| m_failed.load(std::memory_order_acquire)) {
		return false;
	}
	const HRESULT result = m_backend.stop();
	if (FAILED(result)) {
		fail(result);
		return false;
	}
	m_started = false;
	return true;
}

void XAudio2PcmVoice::publishCompletion(const XAudio2PcmCompletionRecord &completion) noexcept
{
	const std::uint32_t write = m_completionWrite.load(std::memory_order_relaxed);
	const std::uint32_t read = m_completionRead.load(std::memory_order_acquire);
	if (write - read >= COMPLETION_COUNT) {
		publishCallbackError(HRESULT_FROM_WIN32(ERROR_BUFFER_OVERFLOW));
		return;
	}
	CompletionSlot &slot = m_completions[write % COMPLETION_COUNT];
	if (slot.ready.load(std::memory_order_acquire)) {
		publishCallbackError(HRESULT_FROM_WIN32(ERROR_BUFFER_OVERFLOW));
		return;
	}
	slot.record = completion;
	slot.ready.store(true, std::memory_order_release);
	m_completionWrite.store(write + 1, std::memory_order_release);
}

bool XAudio2PcmVoice::tryPopCompletion(XAudio2PcmCompletionRecord &completion) noexcept
{
	const std::uint32_t read = m_completionRead.load(std::memory_order_relaxed);
	if (read == m_completionWrite.load(std::memory_order_acquire)) {
		return false;
	}
	CompletionSlot &slot = m_completions[read % COMPLETION_COUNT];
	if (!slot.ready.load(std::memory_order_acquire)) {
		return false;
	}
	completion = slot.record;
	slot.ready.store(false, std::memory_order_release);
	m_completionRead.store(read + 1, std::memory_order_release);
	return true;
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
	if (!m_open.load(std::memory_order_acquire)) {
		return drop();
	}
	if (m_failed.load(std::memory_order_acquire)) {
		chunk = {};
		return AudioPcmSubmitResult::FAILED;
	}
	if (chunk.generation != m_requestedGeneration || !isValidChunk(chunk)) {
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
	if (++m_nextCallbackToken >= (1ULL << (64 - CALLBACK_STATE_BITS))) {
		m_nextCallbackToken = 1;
	}
	// The callback context is an opaque per-submission token, not a slot
	// address. This lets the callback reject a stale completion after the owner
	// has reclaimed and reused the fixed slot.
	slot->buffer.pContext = reinterpret_cast<void *>(
		static_cast<std::uintptr_t>(m_nextCallbackToken));
	slot->callbackIdentity.store(
		encodeCallbackIdentity(m_nextCallbackToken, CALLBACK_STATE_PENDING),
		std::memory_order_release);
	slot->cancelled = false;
	slot->state.store(SlotState::PENDING, std::memory_order_release);
	return AudioPcmSubmitResult::ACCEPTED;
}

void XAudio2PcmVoice::reset(std::uint64_t generation)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	const bool repeatedPendingReset = m_resetPending && m_requestedGeneration == generation;
	m_requestedGeneration = generation;
	m_playedSample.store(-1, std::memory_order_release);
	m_playedGeneration.store(generation, std::memory_order_release);
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
			if (slot.state.load(std::memory_order_acquire) == SlotState::PENDING) {
				clearSlot(slot);
			}
		}
	}
	for (Slot &slot : m_slots) {
		if (slot.state.load(std::memory_order_acquire) == SlotState::SUBMITTED) {
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
		const std::uint64_t observed = slot.callbackIdentity.load(std::memory_order_acquire);
		const std::uint64_t callbackToken = callbackIdentityToken(observed);
		if (callbackToken == 0
			|| pBufferContext != reinterpret_cast<void *>(static_cast<std::uintptr_t>(callbackToken))
			|| callbackIdentityState(observed) != CALLBACK_STATE_SUBMITTED
			|| slot.state.load(std::memory_order_acquire) != SlotState::SUBMITTED) {
			continue;
		}
		std::uint64_t expected = observed;
		if (!slot.callbackIdentity.compare_exchange_strong(expected,
			encodeCallbackIdentity(callbackToken, CALLBACK_STATE_IN_FLIGHT),
			std::memory_order_acq_rel, std::memory_order_acquire)) {
			continue;
		}

		// The claim above prevents owner-side reclamation. Read and account for
		// every slot field before publishing COMPLETE to the owner.
		const std::uint64_t generation = slot.chunk.generation;
		const std::uint64_t sequence = slot.chunk.sequence;
		const std::int64_t endSample = slot.chunk.startSample
			+ static_cast<std::int64_t>(slot.chunk.frameCount);
		const bool currentGeneration = slot.generation
			== m_playedGeneration.load(std::memory_order_acquire);
		if (currentGeneration) {
			std::int64_t previousSample = m_playedSample.load(std::memory_order_acquire);
			while (previousSample < endSample
				&& !m_playedSample.compare_exchange_weak(previousSample, endSample,
					std::memory_order_release, std::memory_order_acquire)) {
			}
		}
		publishCompletion(XAudio2PcmCompletionRecord {
			generation,
			sequence,
			callbackToken,
			endSample
		});
		slot.callbackIdentity.store(encodeCallbackIdentity(callbackToken, CALLBACK_STATE_COMPLETE),
			std::memory_order_release);
		return;
	}
}

void STDMETHODCALLTYPE XAudio2PcmVoice::OnLoopEnd(void *)
{
}

void STDMETHODCALLTYPE XAudio2PcmVoice::OnVoiceError(void *, HRESULT Error)
{
	publishCallbackError(Error);
}
