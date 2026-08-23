#pragma once

#include "XAudio2AudioDevice/IXAudio2AudioEngineBackend.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <limits>
#include <mutex>
#include <vector>

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
	XAudio2PcmVoice *getVoice(XAudio2PcmVoiceHandle handle) noexcept;
	const XAudio2PcmVoice *getVoice(XAudio2PcmVoiceHandle handle) const noexcept;

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
	std::atomic<bool> m_pendingCriticalError;
	std::atomic<HRESULT> m_pendingCriticalErrorCode;
};
