#include "VideoDevice/FFmpeg/FFmpegAudioDecoder.h"

extern "C" {
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/mathematics.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

#include <limits>

namespace
{
constexpr std::int64_t TIMESTAMP_TOLERANCE_SAMPLES = 1;
}

FFmpegAudioDecoder::FFmpegAudioDecoder(MonoMix monoMix) :
	m_monoMix(monoMix),
	m_context(nullptr),
	m_inputSampleRate(0),
	m_inputSampleFormat(AV_SAMPLE_FMT_NONE),
	m_nextSample(0),
	m_timestampOrigin(0),
	m_timestampAnchorSample(0),
	m_timestampAnchorInputFrames(0),
	m_timestampAnchorInputRate(0),
	m_generation(0),
	m_sequence(0),
	m_hasTimeline(false),
	m_hasTimestampOrigin(false),
	m_hasTimestampAnchor(false),
	m_reconfigured(false),
	m_pendingDiscontinuity(false),
	m_drained(false),
	m_ended(false),
	m_failed(false)
{
}

FFmpegAudioDecoder::~FFmpegAudioDecoder()
{
	releaseContext();
}

bool FFmpegAudioDecoder::configure(const AVFrame *frame, AudioPcmSink &sink)
{
	if (frame == nullptr || frame->sample_rate <= 0 || frame->format == AV_SAMPLE_FMT_NONE
		|| av_channel_layout_check(&frame->ch_layout) == 0 || frame->ch_layout.nb_channels <= 0) {
		return false;
	}

	if (m_context != nullptr && m_inputSampleRate == frame->sample_rate && m_inputSampleFormat == frame->format
		&& av_channel_layout_compare(&m_inputChannelLayout, &frame->ch_layout) == 0) {
		return true;
	}

	if (m_context != nullptr && !flushResampler(sink)) {
		return false;
	}
	const bool is_reconfiguration = m_context != nullptr;
	releaseContext();
	AVChannelLayout output_layout = AV_CHANNEL_LAYOUT_STEREO;
	const int result = swr_alloc_set_opts2(&m_context,
		&output_layout,
		AV_SAMPLE_FMT_S16,
		OUTPUT_SAMPLE_RATE,
		&frame->ch_layout,
		static_cast<AVSampleFormat>(frame->format),
		frame->sample_rate,
		0,
		nullptr);
	const double monoMatrix[] = { 1.0, 1.0 };
	if (result < 0 || m_context == nullptr
		|| (m_monoMix == MonoMix::UnityDuplicate && frame->ch_layout.nb_channels == 1
			&& swr_set_matrix(m_context, monoMatrix, 1) < 0)
		|| swr_init(m_context) < 0
		|| av_channel_layout_copy(&m_inputChannelLayout, &frame->ch_layout) < 0) {
		releaseContext();
		return false;
	}

	m_inputSampleRate = frame->sample_rate;
	m_inputSampleFormat = frame->format;
	m_reconfigured = is_reconfiguration && m_hasTimeline;
	m_drained = false;
	return true;
}

bool FFmpegAudioDecoder::restartResampler()
{
	if (m_context == nullptr) {
		return false;
	}
	swr_close(m_context);
	if (swr_init(m_context) < 0) {
		m_failed = true;
		return false;
	}
	m_drained = false;
	return true;
}

bool FFmpegAudioDecoder::flushResampler(AudioPcmSink &sink)
{
	if (m_context == nullptr || m_inputSampleRate <= 0 || m_drained) {
		return true;
	}

	for (;;) {
		const std::int64_t delay = swr_get_delay(m_context, m_inputSampleRate);
		const std::int64_t capacity = av_rescale_rnd(delay,
			OUTPUT_SAMPLE_RATE,
			m_inputSampleRate,
			AV_ROUND_UP);
		if (capacity <= 0) {
			m_drained = true;
			return true;
		}
		if (capacity > MAX_CHUNK_FRAMES || capacity > std::numeric_limits<int>::max()) {
			m_failed = true;
			return false;
		}
		const std::uint64_t sequence = m_sequence;
		if (!submitSamples(static_cast<std::uint32_t>(capacity), nullptr, 0, m_nextSample, false, sink)) {
			return false;
		}
		if (sequence == m_sequence) {
			m_drained = true;
			return true;
		}
	}
}

bool FFmpegAudioDecoder::convert(const AVFrame *frame, int time_base_num, int time_base_den, AudioPcmSink &sink)
{
	if (frame == nullptr || frame->nb_samples <= 0 || frame->extended_data == nullptr || frame->extended_data[0] == nullptr
		|| frame->sample_rate <= 0 || frame->format < 0 || frame->format >= AV_SAMPLE_FMT_NB
		|| av_channel_layout_check(&frame->ch_layout) == 0 || frame->ch_layout.nb_channels <= 0
		|| m_failed || m_ended) {
		return false;
	}
	const int input_planes = av_sample_fmt_is_planar(static_cast<AVSampleFormat>(frame->format))
		? frame->ch_layout.nb_channels
		: 1;
	for (int plane = 0; plane < input_planes; ++plane) {
		if (frame->extended_data[plane] == nullptr) {
			return false;
		}
	}
	if (!configure(frame, sink)) {
		return false;
	}

	std::int64_t timestamp = frame->best_effort_timestamp;
	if (timestamp == AV_NOPTS_VALUE) {
		timestamp = frame->pts;
	}
	const bool has_timestamp = timestamp != AV_NOPTS_VALUE && time_base_num > 0 && time_base_den > 0;
	std::int64_t timestamp_sample = m_nextSample;
	std::int64_t absolute_timestamp_sample = 0;
	bool discontinuity = m_reconfigured;
	if (has_timestamp) {
		absolute_timestamp_sample = av_rescale_q(timestamp,
			AVRational { time_base_num, time_base_den },
			AVRational { 1, OUTPUT_SAMPLE_RATE });
		if (!m_hasTimestampOrigin) {
			if (absolute_timestamp_sample < std::numeric_limits<std::int64_t>::min() + m_nextSample) {
				return false;
			}
			m_timestampOrigin = absolute_timestamp_sample - m_nextSample;
			m_hasTimestampOrigin = true;
		}
		if ((m_timestampOrigin > 0 && absolute_timestamp_sample < std::numeric_limits<std::int64_t>::min() + m_timestampOrigin)
			|| (m_timestampOrigin < 0 && absolute_timestamp_sample > std::numeric_limits<std::int64_t>::max() + m_timestampOrigin)) {
			return false;
		}
		timestamp_sample = absolute_timestamp_sample - m_timestampOrigin;
		if (timestamp_sample < 0) {
			timestamp_sample = 0;
			discontinuity = m_hasTimeline;
		}
		if (m_hasTimestampAnchor && m_timestampAnchorInputRate == m_inputSampleRate) {
			const std::int64_t anchor_duration = av_rescale_rnd(
				m_timestampAnchorInputFrames,
				OUTPUT_SAMPLE_RATE,
				m_timestampAnchorInputRate,
				AV_ROUND_NEAR_INF);
			if (anchor_duration >= 0 && m_timestampAnchorSample <= std::numeric_limits<std::int64_t>::max() - anchor_duration) {
				const std::int64_t expected_timestamp_sample = m_timestampAnchorSample + anchor_duration;
				const bool too_early = expected_timestamp_sample >= std::numeric_limits<std::int64_t>::min() + TIMESTAMP_TOLERANCE_SAMPLES
					&& absolute_timestamp_sample < expected_timestamp_sample - TIMESTAMP_TOLERANCE_SAMPLES;
				const bool too_late = expected_timestamp_sample <= std::numeric_limits<std::int64_t>::max() - TIMESTAMP_TOLERANCE_SAMPLES
					&& absolute_timestamp_sample > expected_timestamp_sample + TIMESTAMP_TOLERANCE_SAMPLES;
				discontinuity = discontinuity || too_early || too_late;
			} else {
				m_hasTimestampAnchor = false;
			}
		}
	}

	if (discontinuity && (m_hasTimeline || swr_get_delay(m_context, m_inputSampleRate) > 0)) {
		if (!flushResampler(sink) || !restartResampler()) {
			return false;
		}
	}
	m_reconfigured = false;
	const std::int64_t start_sample = discontinuity ? timestamp_sample : m_nextSample;

	const std::int64_t delay = swr_get_delay(m_context, m_inputSampleRate);
	if (delay > std::numeric_limits<std::int64_t>::max() - frame->nb_samples) {
		m_failed = true;
		return false;
	}
	const std::int64_t capacity = av_rescale_rnd(delay + frame->nb_samples,
		OUTPUT_SAMPLE_RATE,
		m_inputSampleRate,
		AV_ROUND_UP);
	if (capacity <= 0 || capacity > MAX_CHUNK_FRAMES || capacity > std::numeric_limits<int>::max()) {
		m_failed = true;
		return false;
	}
	if (start_sample > std::numeric_limits<std::int64_t>::max() - capacity) {
		m_failed = true;
		return false;
	}

	if (has_timestamp) {
		m_timestampAnchorSample = absolute_timestamp_sample;
		m_timestampAnchorInputFrames = frame->nb_samples;
		m_timestampAnchorInputRate = m_inputSampleRate;
		m_hasTimestampAnchor = true;
	} else if (m_hasTimestampAnchor && m_timestampAnchorInputRate == m_inputSampleRate
		&& m_timestampAnchorInputFrames <= std::numeric_limits<std::int64_t>::max() - frame->nb_samples) {
		m_timestampAnchorInputFrames += frame->nb_samples;
	} else {
		m_hasTimestampAnchor = false;
	}

	const std::uint8_t *const *input = frame->extended_data;
	m_drained = false;
	return submitSamples(static_cast<std::uint32_t>(capacity), input, frame->nb_samples, start_sample, discontinuity, sink);
}

bool FFmpegAudioDecoder::drain(AudioPcmSink &sink)
{
	if (m_failed) {
		return false;
	}
	if (m_ended) {
		return true;
	}
	if (!flushResampler(sink)) {
		return false;
	}
	m_ended = true;
	return true;
}

void FFmpegAudioDecoder::reset(std::uint64_t generation, AudioPcmSink &sink)
{
	sink.reset(generation);
	releaseContext();
	m_nextSample = 0;
	m_timestampOrigin = 0;
	m_timestampAnchorSample = 0;
	m_timestampAnchorInputFrames = 0;
	m_timestampAnchorInputRate = 0;
	m_generation = generation;
	m_sequence = 0;
	m_hasTimeline = false;
	m_hasTimestampOrigin = false;
	m_hasTimestampAnchor = false;
	m_reconfigured = false;
	m_pendingDiscontinuity = false;
	m_drained = false;
	m_ended = false;
	m_failed = false;
}

bool FFmpegAudioDecoder::submitSamples(std::uint32_t capacity, const std::uint8_t *const *input, int input_frames,
	std::int64_t start_sample, bool discontinuity, AudioPcmSink &sink)
{
	constexpr std::size_t bytes_per_frame = OUTPUT_CHANNELS * sizeof(std::int16_t);
	if (capacity == 0 || capacity > MAX_CHUNK_FRAMES || capacity > std::numeric_limits<int>::max()
		|| capacity > std::numeric_limits<std::size_t>::max() / bytes_per_frame || start_sample < 0) {
		return false;
	}

	AudioPcmChunk chunk;
	chunk.sampleRate = OUTPUT_SAMPLE_RATE;
	chunk.channels = OUTPUT_CHANNELS;
	chunk.sourceChannels = m_inputChannelLayout.nb_channels > 0
		? static_cast<std::uint16_t>(m_inputChannelLayout.nb_channels) : 0;
	chunk.startSample = start_sample;
	chunk.generation = m_generation;
	chunk.sequence = m_sequence;
	m_pendingDiscontinuity = m_pendingDiscontinuity || discontinuity;
	chunk.discontinuity = m_pendingDiscontinuity;
	chunk.data.resize(static_cast<std::size_t>(capacity) * bytes_per_frame);

	std::uint8_t *output[] = { chunk.data.data() };
	const int converted = swr_convert(m_context, output, static_cast<int>(capacity), input, input_frames);
	if (converted < 0) {
		m_failed = true;
		return false;
	}
	if (converted == 0) {
		return true;
	}

	chunk.frameCount = static_cast<std::uint32_t>(converted);
	chunk.data.resize(static_cast<std::size_t>(chunk.frameCount) * bytes_per_frame);
	if (start_sample > std::numeric_limits<std::int64_t>::max() - chunk.frameCount) {
		m_failed = true;
		return false;
	}
	const std::int64_t next_sample = start_sample + chunk.frameCount;
	const AudioPcmSubmitResult result = sink.submit(std::move(chunk));
	if (result == AudioPcmSubmitResult::FAILED) {
		m_failed = true;
		return false;
	}

	++m_sequence;
	m_nextSample = next_sample;
	m_hasTimeline = true;
	m_pendingDiscontinuity = result == AudioPcmSubmitResult::DROPPED;
	return true;
}

void FFmpegAudioDecoder::releaseContext()
{
	swr_free(&m_context);
	av_channel_layout_uninit(&m_inputChannelLayout);
	m_inputSampleRate = 0;
	m_inputSampleFormat = AV_SAMPLE_FMT_NONE;
}
