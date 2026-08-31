#pragma once

#include "AudioDevice/AudioPcmTypes.h"

extern "C" {
#include <libavutil/channel_layout.h>
}

#include <cstdint>

struct AVFrame;
struct SwrContext;

class FFmpegAudioDecoder
{
public:
	static constexpr std::uint32_t OUTPUT_SAMPLE_RATE = 48000;
	static constexpr std::uint16_t OUTPUT_CHANNELS = 2;
	// Frames producing more than one second of PCM are rejected terminally; callers reset rather than silently skipping audio.
	static constexpr std::uint32_t MAX_CHUNK_FRAMES = OUTPUT_SAMPLE_RATE;

	enum class MonoMix { DefaultMix, UnityDuplicate };
	// Movies retain FFmpeg's default mix; legacy audio assets request unity mono duplication.
	explicit FFmpegAudioDecoder(MonoMix monoMix = MonoMix::DefaultMix);
	~FFmpegAudioDecoder();

	FFmpegAudioDecoder(const FFmpegAudioDecoder &) = delete;
	FFmpegAudioDecoder &operator=(const FFmpegAudioDecoder &) = delete;

	bool convert(const AVFrame *frame, int time_base_num, int time_base_den, AudioPcmSink &sink);
	bool drain(AudioPcmSink &sink);
	void reset(std::uint64_t generation, AudioPcmSink &sink);
	std::uint64_t getGeneration() const { return m_generation; }
	bool isFailed() const { return m_failed; }

private:
	bool configure(const AVFrame *frame, AudioPcmSink &sink);
	bool flushResampler(AudioPcmSink &sink);
	bool restartResampler();
	bool submitSamples(std::uint32_t capacity, const std::uint8_t *const *input, int input_frames,
		std::int64_t start_sample, bool discontinuity, AudioPcmSink &sink);
	void releaseContext();

	const MonoMix m_monoMix;
	SwrContext *m_context;
	int m_inputSampleRate;
	int m_inputSampleFormat;
	AVChannelLayout m_inputChannelLayout {};
	std::int64_t m_nextSample;
	std::int64_t m_timestampOrigin;
	std::int64_t m_timestampAnchorSample;
	std::int64_t m_timestampAnchorInputFrames;
	int m_timestampAnchorInputRate;
	std::uint64_t m_generation;
	std::uint64_t m_sequence;
	bool m_hasTimeline;
	bool m_hasTimestampOrigin;
	bool m_hasTimestampAnchor;
	bool m_reconfigured;
	bool m_pendingDiscontinuity;
	bool m_drained;
	bool m_ended;
	bool m_failed;
};
