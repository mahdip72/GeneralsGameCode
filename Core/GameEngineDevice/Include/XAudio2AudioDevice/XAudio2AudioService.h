#pragma once

#include "AudioDevice/AudioPcmTypes.h"
#include "XAudio2AudioDevice/IXAudio2AudioEngineBackend.h"
#include "XAudio2AudioDevice/XAudio2FailurePublication.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <limits>
#include <mutex>
#include <vector>

#include <x3daudio.h>

class XAudio2PcmVoice;

enum class XAudio2AudioServiceState : std::uint8_t
{
	CLOSED,
	OPENING,
	RUNNING,
	FAILED,
	QUIESCING
};

struct XAudio2PcmVoiceHandle
{
	static constexpr std::uint32_t INVALID_INDEX = (std::numeric_limits<std::uint32_t>::max)();

	std::uint32_t index = INVALID_INDEX;
	std::uint64_t generation = 0;

	bool isValid() const noexcept
	{
		return index != INVALID_INDEX && generation != 0;
	}
};

inline bool operator==(const XAudio2PcmVoiceHandle &left, const XAudio2PcmVoiceHandle &right) noexcept
{
	return left.index == right.index && left.generation == right.generation;
}

inline bool operator!=(const XAudio2PcmVoiceHandle &left, const XAudio2PcmVoiceHandle &right) noexcept
{
	return !(left == right);
}

// Fixed-size data published by a voice callback and observed by its owner
// thread.  It contains no native interface or game-object pointer.
struct XAudio2AudioCompletion
{
	XAudio2PcmVoiceHandle voice;
	std::uint64_t generation = 0;
	std::uint64_t sequence = 0;
	std::int64_t endSample = -1;
};

struct XAudio2SpatializationPose
{
	float position[3] = { 0.0f, 0.0f, 0.0f };
	float front[3] = { 0.0f, 0.0f, 1.0f };
	float top[3] = { 0.0f, 1.0f, 0.0f };
};

class XAudio2AudioService
{
public:
	XAudio2AudioService();
	explicit XAudio2AudioService(std::unique_ptr<IXAudio2AudioEngineBackend> backend);
	~XAudio2AudioService();

	XAudio2AudioService(const XAudio2AudioService &) = delete;
	XAudio2AudioService &operator=(const XAudio2AudioService &) = delete;

	bool open();
	void shutdown();
	bool isOpen() const noexcept;
	XAudio2AudioServiceState state() const noexcept;
	HRESULT getLastError() const noexcept;

	// Owner-thread observation point for the atomically published engine error.
	// The callback itself never takes this service mutex or touches child voices.
	bool processPendingFailure() noexcept;
	void serviceVoices() noexcept;

	XAudio2PcmVoiceHandle createVoice() noexcept;
	bool destroyVoice(XAudio2PcmVoiceHandle handle) noexcept;
	AudioPcmSubmitResult submit(XAudio2PcmVoiceHandle handle, AudioPcmChunk &&chunk) noexcept;
	bool resetVoice(XAudio2PcmVoiceHandle handle, std::uint64_t generation) noexcept;
	bool serviceVoice(XAudio2PcmVoiceHandle handle) noexcept;
	bool setVoiceVolume(XAudio2PcmVoiceHandle handle, float volume) noexcept;
	bool setVoiceSpatialization(XAudio2PcmVoiceHandle handle,
		const XAudio2SpatializationPose &listener,
		const XAudio2SpatializationPose &emitter) noexcept;
	bool pauseVoice(XAudio2PcmVoiceHandle handle) noexcept;
	bool resumeVoice(XAudio2PcmVoiceHandle handle) noexcept;
	bool stopVoice(XAudio2PcmVoiceHandle handle) noexcept;
	bool isVoiceOpen(XAudio2PcmVoiceHandle handle) const noexcept;
	bool isVoiceFailed(XAudio2PcmVoiceHandle handle) const noexcept;
	bool isVoiceDrained(XAudio2PcmVoiceHandle handle) const noexcept;
	HRESULT getVoiceLastError(XAudio2PcmVoiceHandle handle) const noexcept;
	bool getVoicePlayedSample(XAudio2PcmVoiceHandle handle, std::int64_t &sample) const noexcept;
	bool tryPopCompletion(XAudio2AudioCompletion &completion) noexcept;
	// Dedicated owners such as movie playback do not translate PCM completions
	// into game events, but they must still consume the bounded callback FIFO.
	void discardCompletions() noexcept;

private:
	struct VoiceRecord
	{
		std::unique_ptr<XAudio2PcmVoice> voice;
		std::unique_ptr<IXAudio2PcmVoiceBackend> backend;
		std::uint64_t generation = 0;
	};

	static void criticalErrorThunk(void *context, HRESULT error) noexcept;
	static HRESULT normalizeFailure(HRESULT error) noexcept;
	bool processPendingFailureLocked() noexcept;
	bool isHandleOwnedLocked(XAudio2PcmVoiceHandle handle) const noexcept;
	XAudio2PcmVoiceHandle invalidHandle() const noexcept;

	std::unique_ptr<IXAudio2AudioEngineBackend> m_backend;
	mutable std::mutex m_mutex;
	std::vector<VoiceRecord> m_voices;
	std::uint64_t m_nextHandleGeneration;
	std::atomic<XAudio2AudioServiceState> m_state;
	std::atomic<HRESULT> m_lastError;
	XAudio2FailurePublication m_failurePublication;
	bool m_failureHandled = false;
	X3DAUDIO_HANDLE m_x3dHandle = {};
	XAudio2OutputDetails m_outputDetails;
	bool m_spatializationReady = false;
};
