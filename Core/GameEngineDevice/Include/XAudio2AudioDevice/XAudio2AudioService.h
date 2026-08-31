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
class XAudio2AudioServiceOwner;

enum class XAudio2AudioExecutionMode : std::uint8_t
{
	SHARED_OWNER,
	SERIAL_REFERENCE
};

struct XAudio2AudioOwnerMetrics
{
	std::uint64_t commands = 0;
	std::uint64_t servicePasses = 0;
	std::uint64_t coalescedControls = 0;
	std::uint64_t queueWaits = 0;
	std::uint64_t queueWaitNanoseconds = 0;
	std::uint64_t fenceWaits = 0;
	std::uint64_t fenceWaitNanoseconds = 0;
	std::uint64_t rejectedSubmissions = 0;
	std::size_t peakQueuedCommands = 0;
	std::size_t peakBufferedBytes = 0;
	bool sharedOwner = false;
	bool forcedSerial = false;
};

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
	XAudio2AudioService(std::unique_ptr<IXAudio2AudioEngineBackend> backend,
		XAudio2AudioExecutionMode executionMode);
	~XAudio2AudioService();

	XAudio2AudioService(const XAudio2AudioService &) = delete;
	XAudio2AudioService &operator=(const XAudio2AudioService &) = delete;

	bool open();
	void shutdown();
	bool isOpen() const noexcept;
	XAudio2AudioServiceState state() const noexcept;
	HRESULT getLastError() const noexcept;
	// Select before constructing services. Serial mode is an explicit reference
	// lane, never a silent response to owner creation or native device failure.
	static void setDefaultExecutionMode(XAudio2AudioExecutionMode mode) noexcept;
	XAudio2AudioOwnerMetrics ownerMetrics() const noexcept;
	// A transition/test fence, not a per-frame polling API. Ordinary controls
	// return admission status; asynchronous native failure is published in status.
	bool synchronize() noexcept;

	// Owner-thread observation point for the atomically published engine error.
	// The callback itself never takes this service mutex or touches child voices.
	bool processPendingFailure() noexcept;
	void serviceVoices() noexcept;

	// The default preserves the native source-voice limit without reserving
	// a larger resampling range for voices that do not need one.
	XAudio2PcmVoiceHandle createVoice(float maxFrequencyRatio = 2.0f) noexcept;
	bool destroyVoice(XAudio2PcmVoiceHandle handle) noexcept;
	AudioPcmSubmitResult submit(XAudio2PcmVoiceHandle handle, AudioPcmChunk &&chunk) noexcept;
	// Manager-only retry path. A bounded-admission DROPPED result leaves chunk
	// intact for a later attempt; ACCEPTED and terminal FAILED consume it. The
	// sink-facing submit() above intentionally keeps its consume-on-rejection
	// contract for movie and other non-replayable producers.
	AudioPcmSubmitResult submitRetained(XAudio2PcmVoiceHandle handle,
		AudioPcmChunk &chunk) noexcept;
	bool canVoiceAccept(XAudio2PcmVoiceHandle handle, std::size_t submissions) const noexcept;
	bool resetVoice(XAudio2PcmVoiceHandle handle, std::uint64_t generation) noexcept;
	bool serviceVoice(XAudio2PcmVoiceHandle handle) noexcept;
	bool setVoiceVolume(XAudio2PcmVoiceHandle handle, float volume) noexcept;
	bool setVoiceFrequencyRatio(XAudio2PcmVoiceHandle handle, float ratio) noexcept;
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
	void discardCompletions(XAudio2PcmVoiceHandle handle) noexcept;
	bool tryPopCompletion(XAudio2PcmVoiceHandle handle,
		XAudio2AudioCompletion &completion) noexcept;
	// Fixed PCM occupancy snapshot used to reserve command-side capacity.
	bool getVoiceBufferedState(XAudio2PcmVoiceHandle handle,
		std::size_t &buffers, std::size_t &bytes) const noexcept;

private:
	struct VoiceRecord
	{
		std::unique_ptr<XAudio2PcmVoice> voice;
		std::unique_ptr<IXAudio2PcmVoiceBackend> backend;
		std::uint64_t generation = 0;
		bool monoExpandedToStereo = false;
	};

	static void criticalErrorThunk(void *context, HRESULT error) noexcept;
	static HRESULT normalizeFailure(HRESULT error) noexcept;
	bool processPendingFailureLocked() noexcept;
	bool isHandleOwnedLocked(XAudio2PcmVoiceHandle handle) const noexcept;
	XAudio2PcmVoiceHandle invalidHandle() const noexcept;

	std::unique_ptr<IXAudio2AudioEngineBackend> m_backend;
	std::unique_ptr<XAudio2AudioServiceOwner> m_executionOwner;
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
