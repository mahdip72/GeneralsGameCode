#pragma once

#include "AudioDevice/AudioPcmTypes.h"
#include "VideoDevice/FFmpeg/FFmpegFile.h"

#include <cstddef>
#include <cstdint>
#include <functional>

struct AVFrame;
class FFmpegAudioDecoder;

enum class FFmpegMoviePlaybackMode : std::uint8_t
{
	ONCE,
	SHOW_LAST_FRAME,
	LOOP
};

enum class FFmpegMoviePlaybackState : std::uint8_t
{
	ACTIVE,
	DRAINING,
	ENDED,
	SILENT_AUDIO,
	FAILED
};

using FFmpegMovieClock = std::function<std::int64_t()>;
using FFmpegMovieVideoCallback = std::function<void(const AVFrame *, const FFmpegFrameMetadata &, void *)>;

struct FFmpegMoviePlaybackOptions
{
	FFmpegMoviePlaybackMode mode = FFmpegMoviePlaybackMode::SHOW_LAST_FRAME;
	bool audioEnabled = true;
	double gain = 1.0;
	FFmpegMovieClock clock;
};

class FFmpegMoviePlayback final
{
public:
	FFmpegMoviePlayback(FFmpegFile &file, AudioPcmSink *sink, const FFmpegMoviePlaybackOptions &options = {});
	~FFmpegMoviePlayback();

	FFmpegMoviePlayback(const FFmpegMoviePlayback &) = delete;
	FFmpegMoviePlayback &operator=(const FFmpegMoviePlayback &) = delete;

	void setVideoCallback(FFmpegMovieVideoCallback callback, void *userData = nullptr);
	void setMode(FFmpegMoviePlaybackMode mode);
	void setGain(double gain);
	FFmpegMoviePlaybackMode mode() const { return m_mode; }

	// Decode at most maxDecodeCalls. Every call consumes input; no sink drop is retried.
	bool pump(std::size_t maxDecodeCalls = 64);
	// Finish a non-looping generation, including decoder/resampler EOF drain.
	bool finish(std::size_t maxPumpCalls = 256);
	bool seekFrame(Int frameIndex);

	FFmpegMoviePlaybackState state() const { return m_state; }
	bool isTerminal() const;
	bool isFrameReady() const;
	bool hasCurrentFrame() const { return m_currentFrame != nullptr; }
	const AVFrame *currentFrame() const { return m_currentFrame; }
	const FFmpegFrameMetadata &currentFrameMetadata() const { return m_currentMetadata; }

	Int frameIndex() const { return m_file.getCurrentFrame(); }
	Int frameCount() const { return m_file.getNumFrames(); }
	Int width() const { return m_file.getWidth(); }
	Int height() const { return m_file.getHeight(); }
	std::uint64_t generation() const { return m_generation; }
	std::size_t drainCount() const { return m_drainCount; }
	std::size_t resetCount() const { return m_resetCount; }
	std::int64_t videoPresentationTimeUs() const { return m_videoPresentationTimeUs; }

private:
	class SilentSink;
	class GainSink;

	static void onFrame(const AVFrame *frame, const FFmpegFrameMetadata &metadata, void *userData);
	static std::int64_t defaultClock();

	void handleFrame(const AVFrame *frame, const FFmpegFrameMetadata &metadata);
	bool handleEndOfInput();
	bool resetGeneration(std::uint64_t generation);
	bool setFailed(bool resetAudio = true);
	void clearCurrentFrame();
	bool isAudioFrameAdmitted(const FFmpegFrameMetadata &metadata);
	std::int64_t timestampToMicroseconds(const FFmpegFrameMetadata &metadata) const;
	std::int64_t targetTimeForFrame(Int frameIndex) const;
	std::int64_t nowMicroseconds() const;
	void rebaseClocks(std::int64_t timelineBase);

	FFmpegFile &m_file;
	AudioPcmSink *m_externalSink;
	SilentSink *m_silentSink;
	GainSink *m_gainSink;
	AudioPcmSink *m_audioSink;
	FFmpegAudioDecoder *m_audioDecoder;
	FFmpegMoviePlaybackMode m_mode;
	FFmpegMoviePlaybackState m_state;
	FFmpegMovieClock m_clock;
	FFmpegMovieVideoCallback m_videoCallback;
	void *m_videoUserData;
	AVFrame *m_currentFrame;
	FFmpegFrameMetadata m_currentMetadata;
	std::uint64_t m_generation;
	std::size_t m_drainCount;
	std::size_t m_resetCount;
	std::int64_t m_clockBaseUs;
	std::int64_t m_audioBaseSample;
	std::int64_t m_videoPresentationTimeUs;
	std::int64_t m_seekTargetUs;
	bool m_clockRebased;
	bool m_audioClockRebased;
	bool m_videoGateActive;
	bool m_audioGateActive;
	bool m_newVideoFrame;
};
