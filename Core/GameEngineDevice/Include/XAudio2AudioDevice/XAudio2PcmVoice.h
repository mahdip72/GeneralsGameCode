#pragma once

#include "AudioDevice/AudioPcmTypes.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>

#include <xaudio2.h>

class IXAudio2PcmVoiceBackend
{
public:
	virtual ~IXAudio2PcmVoiceBackend() = default;

	virtual HRESULT create(const WAVEFORMATEX &format, IXAudio2VoiceCallback *callback) noexcept = 0;
	virtual HRESULT submit(const XAUDIO2_BUFFER &buffer) noexcept = 0;
	virtual HRESULT start() noexcept = 0;
	virtual HRESULT stop() noexcept = 0;
	virtual HRESULT flush() noexcept = 0;
	virtual HRESULT getCriticalError() const noexcept = 0;
	// Optional controls keep older injected backends source-compatible while
	// exposing typed controls to the native manager.
	virtual HRESULT setVolume(float volume) noexcept
	{
		(void)volume;
		return E_NOTIMPL;
	}
	virtual HRESULT pause() noexcept { return stop(); }
	virtual HRESULT resume() noexcept { return start(); }
	virtual void destroy() noexcept = 0;
	virtual HRESULT destroyWithResult() noexcept
	{
		destroy();
		return S_OK;
	}
};

struct XAudio2PcmCompletionRecord
{
	std::uint64_t generation = 0;
	std::uint64_t sequence = 0;
	std::uint64_t callbackToken = 0;
	std::int64_t endSample = -1;
};

class XAudio2PcmVoice final : public AudioPcmSink, public IXAudio2VoiceCallback
{
public:
	static constexpr std::size_t SLOT_COUNT = 8;

	explicit XAudio2PcmVoice(IXAudio2PcmVoiceBackend &backend);
	~XAudio2PcmVoice();

	XAudio2PcmVoice(const XAudio2PcmVoice &) = delete;
	XAudio2PcmVoice &operator=(const XAudio2PcmVoice &) = delete;

	// These methods are owned by the audio thread. The backend is only touched here.
	bool open();
	void service();
	void close() noexcept override;

	bool isOpen() const noexcept;
	bool isFailed() const noexcept;
	bool isDrained() const noexcept;
	HRESULT getLastError() const noexcept;
	bool setVolume(float volume) noexcept;
	bool pause() noexcept;
	bool resume() noexcept;
	bool stop() noexcept;
	// Called by the owning audio service after it observes an engine-level
	// critical error.  The service owns the failure transition; callbacks only
	// publish atomics and never call this method.
	void failFromService(HRESULT error) noexcept;

	AudioPcmSubmitResult submit(AudioPcmChunk &&chunk) override;
	void reset(std::uint64_t generation) override;
	bool getPlayedSample(std::int64_t &sample) const noexcept override;
	bool tryPopCompletion(XAudio2PcmCompletionRecord &completion) noexcept;

	void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32 BytesRequired) override;
	void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() override;
	void STDMETHODCALLTYPE OnStreamEnd() override;
	void STDMETHODCALLTYPE OnBufferStart(void *pBufferContext) override;
	void STDMETHODCALLTYPE OnBufferEnd(void *pBufferContext) override;
	void STDMETHODCALLTYPE OnLoopEnd(void *pBufferContext) override;
	void STDMETHODCALLTYPE OnVoiceError(void *pBufferContext, HRESULT Error) override;

private:
	enum class SlotState : std::uint8_t
	{
		FREE,
		PENDING,
		SUBMITTED
	};

	struct Slot
	{
		AudioPcmChunk chunk;
		XAUDIO2_BUFFER buffer = {};
		std::uint64_t generation = 0;
		std::uint64_t sequence = 0;
		std::atomic<SlotState> state { SlotState::FREE };
		bool cancelled = false;
		// Callback ownership is claimed before reading the non-atomic metadata;
		// the owner reclaims only after the callback publishes COMPLETE.
		std::atomic<std::uint64_t> callbackIdentity { 0 };
	};

	static constexpr std::size_t COMPLETION_COUNT = 32;
	struct CompletionSlot
	{
		XAudio2PcmCompletionRecord record;
		std::atomic<bool> ready { false };
	};

	static WAVEFORMATEX pcmFormat();
	static bool isValidChunk(const AudioPcmChunk &chunk);
	static std::uint64_t encodeCallbackIdentity(std::uint64_t token,
		std::uint64_t state) noexcept;
	static std::uint64_t callbackIdentityToken(std::uint64_t identity) noexcept;
	static std::uint64_t callbackIdentityState(std::uint64_t identity) noexcept;

	void clearSlot(Slot &slot);
	void reclaimCompletedSlots();
	Slot *findFreeSlot();
	Slot *findNextPendingSlot();
	bool hasSubmittedOldSlot() const;
	bool consumeCallbackError(HRESULT &error);
	void fail(HRESULT error);
	bool checkExternalFailure();
	void publishCompletion(const XAudio2PcmCompletionRecord &completion) noexcept;

	IXAudio2PcmVoiceBackend &m_backend;
	mutable std::mutex m_mutex;
	std::array<Slot, SLOT_COUNT> m_slots;
	std::array<CompletionSlot, COMPLETION_COUNT> m_completions;
	std::atomic<std::uint32_t> m_completionWrite { 0 };
	std::atomic<std::uint32_t> m_completionRead { 0 };
	std::atomic<bool> m_open;
	std::atomic<bool> m_failed;
	std::atomic<bool> m_callbackError;
	std::atomic<HRESULT> m_callbackErrorCode;
	std::atomic<HRESULT> m_lastError;
	std::atomic<std::int64_t> m_playedSample;
	std::atomic<std::uint64_t> m_playedGeneration;
	std::uint64_t m_requestedGeneration;
	std::uint64_t m_activeGeneration;
	std::uint64_t m_nextCallbackToken;
	bool m_resetPending;
	bool m_started;
	bool m_backendCreated;
};
