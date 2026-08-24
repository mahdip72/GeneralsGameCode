#pragma once

#include "AudioDevice/AudioPcmTypes.h"
#include "XAudio2AudioDevice/XAudio2AudioService.h"

#include <cstdint>
#include <mutex>

// XAudio2 owner-side adapter for movie PCM.  FFmpegMoviePlayback only sees the
// neutral AudioPcmSink contract; the service retains all native voice ownership.
class XAudio2MoviePcmSink final : public AudioPcmSink
{
public:
	explicit XAudio2MoviePcmSink(XAudio2AudioService &service) noexcept;
	~XAudio2MoviePcmSink() override;

	XAudio2MoviePcmSink(const XAudio2MoviePcmSink &) = delete;
	XAudio2MoviePcmSink &operator=(const XAudio2MoviePcmSink &) = delete;

	bool isReady() const noexcept;
	AudioPcmSubmitResult submit(AudioPcmChunk &&chunk) override;
	void reset(std::uint64_t generation) override;
	void endOfStream() noexcept override;
	bool isDrained() const noexcept override;
	bool getPlayedSample(std::int64_t &sample) const noexcept override;

	// Called by the audio owner after producer submissions.  This is deliberately
	// separate from submit so decoder callbacks never touch XAudio2.
	bool service() noexcept;
	bool serviceSink() noexcept override { return service(); }
	void close() noexcept override;

private:
	mutable std::mutex m_mutex;
	XAudio2AudioService *m_service;
	XAudio2PcmVoiceHandle m_handle;
	bool m_closed;
	bool m_endOfStream;
};
