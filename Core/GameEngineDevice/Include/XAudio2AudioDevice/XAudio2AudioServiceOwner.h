#pragma once

#include "XAudio2AudioDevice/XAudio2AudioService.h"

struct XAudio2AudioOwnerState;

// Command-side adapter. The process owner retains the native service and all
// callbacks until its close fence completes; this object never owns COM state.
class XAudio2AudioServiceOwner
{
public:
	static constexpr std::size_t MAX_SERVICES = 8;
	static constexpr std::size_t MAX_VOICES = 256;
	static constexpr std::size_t MAX_COMMANDS = 512;
	static constexpr std::size_t MAX_PCM_BYTES = 32 * 1024 * 1024;

	explicit XAudio2AudioServiceOwner(std::unique_ptr<IXAudio2AudioEngineBackend> backend);
	~XAudio2AudioServiceOwner();
	bool open() noexcept;
	void shutdown() noexcept;
	bool synchronize() noexcept;
	XAudio2AudioServiceState state() const noexcept;
	HRESULT getLastError() const noexcept;
	XAudio2AudioOwnerMetrics metrics() const noexcept;
	bool processPendingFailure() noexcept;
	XAudio2PcmVoiceHandle createVoice(float maxFrequencyRatio) noexcept;
	bool destroyVoice(XAudio2PcmVoiceHandle handle) noexcept;
	AudioPcmSubmitResult submit(XAudio2PcmVoiceHandle handle, AudioPcmChunk &&chunk) noexcept;
	// Manager-only retry path. A bounded-admission DROPPED result leaves chunk
	// intact for a later attempt; ACCEPTED and terminal FAILED consume it. The
	// sink-facing submit() intentionally keeps its consume-on-rejection contract.
	AudioPcmSubmitResult submitRetained(XAudio2PcmVoiceHandle handle,
		AudioPcmChunk &chunk) noexcept;
	bool canVoiceAccept(XAudio2PcmVoiceHandle handle, std::size_t submissions) const noexcept;
	bool resetVoice(XAudio2PcmVoiceHandle handle, std::uint64_t generation) noexcept;
	bool serviceVoice(XAudio2PcmVoiceHandle handle) noexcept;
	void serviceVoices() noexcept;
	bool setVoiceVolume(XAudio2PcmVoiceHandle handle, float volume) noexcept;
	bool setVoiceFrequencyRatio(XAudio2PcmVoiceHandle handle, float ratio) noexcept;
	bool setVoiceSpatialization(XAudio2PcmVoiceHandle handle,
		const XAudio2SpatializationPose &listener, const XAudio2SpatializationPose &emitter) noexcept;
	bool pauseVoice(XAudio2PcmVoiceHandle handle) noexcept;
	bool resumeVoice(XAudio2PcmVoiceHandle handle) noexcept;
	bool stopVoice(XAudio2PcmVoiceHandle handle) noexcept;
	bool isVoiceOpen(XAudio2PcmVoiceHandle handle) const noexcept;
	bool isVoiceFailed(XAudio2PcmVoiceHandle handle) const noexcept;
	bool isVoiceDrained(XAudio2PcmVoiceHandle handle) const noexcept;
	HRESULT getVoiceLastError(XAudio2PcmVoiceHandle handle) const noexcept;
	bool getVoicePlayedSample(XAudio2PcmVoiceHandle handle, std::int64_t &sample) const noexcept;
	bool getVoiceBufferedState(XAudio2PcmVoiceHandle handle,
		std::size_t &buffers, std::size_t &bytes) const noexcept;
	bool tryPopCompletion(XAudio2AudioCompletion &completion) noexcept;
	bool tryPopCompletion(XAudio2PcmVoiceHandle handle, XAudio2AudioCompletion &completion) noexcept;
	void discardCompletions() noexcept;
	void discardCompletions(XAudio2PcmVoiceHandle handle) noexcept;

private:
	std::shared_ptr<XAudio2AudioOwnerState> m_state;
};
