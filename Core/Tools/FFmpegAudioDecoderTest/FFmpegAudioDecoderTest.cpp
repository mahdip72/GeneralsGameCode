#include "VideoDevice/FFmpeg/FFmpegAudioDecoder.h"

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/mathematics.h>
#include <libavutil/samplefmt.h>
}

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

static_assert(UINTPTR_MAX == UINT64_MAX, "The native FFmpeg audio contract must be tested as x64.");

class CapturingSink final : public AudioPcmSink
{
public:
	AudioPcmSubmitResult submit(AudioPcmChunk &&chunk) override
	{
		if (dropNext) {
			dropNext = false;
			droppedChunks.push_back(std::move(chunk));
			return AudioPcmSubmitResult::DROPPED;
		}
		chunks.push_back(std::move(chunk));
		return AudioPcmSubmitResult::ACCEPTED;
	}

	void reset(std::uint64_t generation) override
	{
		lastResetGeneration = generation;
		chunks.clear();
		dropNext = false;
		droppedChunks.clear();
	}

	std::vector<AudioPcmChunk> chunks;
	std::uint64_t lastResetGeneration = std::numeric_limits<std::uint64_t>::max();
	std::vector<AudioPcmChunk> droppedChunks;
	bool dropNext = false;
};

static AVFrame *createS16Frame(int sample_rate, int samples, std::int64_t pts, bool planar = true, int channels = 2)
{
	AVFrame *frame = av_frame_alloc();
	if (frame == nullptr) {
		return nullptr;
	}
	frame->format = planar ? AV_SAMPLE_FMT_S16P : AV_SAMPLE_FMT_S16;
	frame->sample_rate = sample_rate;
	frame->nb_samples = samples;
	frame->pts = pts;
	frame->best_effort_timestamp = pts;
	av_channel_layout_default(&frame->ch_layout, channels);
	if (av_frame_get_buffer(frame, 32) < 0) {
		av_frame_free(&frame);
		return nullptr;
	}

	if (planar) {
		for (int channel = 0; channel < channels; ++channel) {
			for (int sample = 0; sample < samples; ++sample) {
				const int magnitude = 100 + channel * 400 + sample;
				reinterpret_cast<std::int16_t *>(frame->extended_data[channel])[sample]
					= static_cast<std::int16_t>(channel == 0 ? magnitude : -magnitude);
			}
		}
	} else {
		for (int sample = 0; sample < samples; ++sample) {
			reinterpret_cast<std::int16_t *>(frame->extended_data[0])[sample * 2] = static_cast<std::int16_t>(100 + sample);
			reinterpret_cast<std::int16_t *>(frame->extended_data[0])[sample * 2 + 1] = static_cast<std::int16_t>(-500 - sample);
		}
	}
	return frame;
}

static bool chunkInvariant(const AudioPcmChunk &chunk)
{
	return chunk.sampleRate == FFmpegAudioDecoder::OUTPUT_SAMPLE_RATE
		&& chunk.channels == FFmpegAudioDecoder::OUTPUT_CHANNELS
		&& chunk.format == AudioPcmFormat::SIGNED_16_INTERLEAVED_LITTLE_ENDIAN
		&& chunk.frameCount > 0
		&& chunk.startSample >= 0
		&& chunk.data.size() == static_cast<std::size_t>(chunk.frameCount) * chunk.channels * sizeof(std::int16_t);
}

static bool testOwnedInterleaving()
{
	CapturingSink sink;
	FFmpegAudioDecoder decoder;
	decoder.reset(7, sink);
	AVFrame *frame = createS16Frame(48000, 4, 96);
	if (frame == nullptr || !decoder.convert(frame, 1, 48000, sink) || sink.chunks.size() != 1) {
		av_frame_free(&frame);
		return false;
	}

	std::memset(frame->extended_data[0], 0, 4 * sizeof(std::int16_t));
	std::memset(frame->extended_data[1], 0, 4 * sizeof(std::int16_t));
	av_frame_unref(frame);
	av_frame_free(&frame);
	const AudioPcmChunk &chunk = sink.chunks.front();
	const std::int16_t expected[] = { 100, -500, 101, -501, 102, -502, 103, -503 };
	const bool content_matches = sink.lastResetGeneration == 7 && chunkInvariant(chunk) && chunk.frameCount == 4 && chunk.startSample == 0
		&& chunk.generation == 7 && chunk.sequence == 0 && !chunk.discontinuity
		&& chunk.data.size() == sizeof(expected) && std::memcmp(chunk.data.data(), expected, sizeof(expected)) == 0;
	if (!content_matches || !decoder.drain(sink) || sink.chunks.size() != 1
		|| !decoder.drain(sink) || sink.chunks.size() != 1) {
		return false;
	}
	frame = createS16Frame(48000, 4, 100);
	const bool ended_rejects_input = frame != nullptr && !decoder.convert(frame, 1, 48000, sink);
	av_frame_free(&frame);
	return ended_rejects_input && !decoder.isFailed();
}

static bool testResamplingAndDrain()
{
	CapturingSink sink;
	FFmpegAudioDecoder decoder;
	decoder.reset(11, sink);
	constexpr int frame_count = 8;
	constexpr int samples_per_frame = 1024;
	for (int frame_index = 0; frame_index < frame_count; ++frame_index) {
		AVFrame *frame = createS16Frame(44100, samples_per_frame, frame_index * samples_per_frame);
		const bool converted = frame != nullptr && decoder.convert(frame, 1, 44100, sink);
		av_frame_free(&frame);
		if (!converted) {
			return false;
		}
	}
	if (!decoder.drain(sink)) {
		return false;
	}
	const std::size_t after_drain = sink.chunks.size();
	if (!decoder.drain(sink) || sink.chunks.size() != after_drain) {
		return false;
	}

	std::uint64_t expected_sequence = 0;
	std::int64_t expected_start = 0;
	std::uint32_t total_frames = 0;
	std::uint32_t channel_pattern_frames = 0;
	for (const AudioPcmChunk &chunk : sink.chunks) {
		if (!chunkInvariant(chunk) || chunk.generation != 11 || chunk.sequence != expected_sequence
			|| chunk.startSample != expected_start || chunk.discontinuity) {
			return false;
		}
		++expected_sequence;
		expected_start += chunk.frameCount;
		total_frames += chunk.frameCount;
		const std::int16_t *samples = reinterpret_cast<const std::int16_t *>(chunk.data.data());
		for (std::uint32_t frame = 0; frame < chunk.frameCount; ++frame) {
			if (samples[frame * 2] > 0 && samples[frame * 2 + 1] < 0) {
				++channel_pattern_frames;
			}
		}
	}
	const std::int64_t expected_frames = av_rescale_rnd(frame_count * samples_per_frame, 48000, 44100, AV_ROUND_NEAR_INF);
	if (channel_pattern_frames < total_frames * 9 / 10
		|| total_frames < expected_frames - 2 || total_frames > expected_frames + 2) {
		return false;
	}
	return true;
}

static bool testTimelineAndReset()
{
	CapturingSink sink;
	FFmpegAudioDecoder decoder;
	decoder.reset(20, sink);
	const std::int64_t timestamps[] = { -100, -96, 100, -100 };
	for (std::int64_t timestamp : timestamps) {
		AVFrame *frame = createS16Frame(48000, 4, timestamp);
		const bool converted = frame != nullptr && decoder.convert(frame, 1, 48000, sink);
		av_frame_free(&frame);
		if (!converted) {
			return false;
		}
	}
	if (sink.chunks.size() != 4 || sink.chunks[0].startSample != 0 || sink.chunks[0].discontinuity
		|| sink.chunks[1].startSample != 4 || sink.chunks[1].discontinuity
		|| sink.chunks[2].startSample != 200 || !sink.chunks[2].discontinuity
		|| sink.chunks[3].startSample != 0 || !sink.chunks[3].discontinuity) {
		return false;
	}

	decoder.reset(21, sink);
	if (sink.lastResetGeneration != 21 || !sink.chunks.empty() || decoder.getGeneration() != 21 || decoder.isFailed()) {
		return false;
	}
	AVFrame *frame = createS16Frame(48000, 4, AV_NOPTS_VALUE);
	if (frame == nullptr) {
		return false;
	}
	const bool converted = decoder.convert(frame, 0, 0, sink);
	av_frame_free(&frame);
	if (!converted || sink.chunks.size() != 1 || sink.chunks[0].startSample != 0
		|| sink.chunks[0].generation != 21 || sink.chunks[0].sequence != 0 || sink.chunks[0].discontinuity) {
		return false;
	}

	decoder.reset(22, sink);
	const std::int64_t timestamp_sequence[] = { 0, AV_NOPTS_VALUE, 100 };
	for (std::int64_t timestamp : timestamp_sequence) {
		frame = createS16Frame(48000, 4, timestamp);
		const bool sequence_converted = frame != nullptr && decoder.convert(frame, 1, 48000, sink);
		av_frame_free(&frame);
		if (!sequence_converted) {
			return false;
		}
	}
	return sink.chunks.size() == 3 && !sink.chunks[0].discontinuity && !sink.chunks[1].discontinuity
		&& sink.chunks[1].startSample == 4 && sink.chunks[2].discontinuity && sink.chunks[2].startSample == 100
		&& sink.chunks[2].startSample - (sink.chunks[1].startSample + sink.chunks[1].frameCount) == 92;
}

static bool testResampledMissingTimestamps()
{
	CapturingSink sink;
	FFmpegAudioDecoder decoder;
	decoder.reset(23, sink);
	struct Fixture
	{
		int samples;
		std::int64_t timestamp;
	};
	const Fixture fixtures[] = {
		{ 1024, 0 },
		{ 512, AV_NOPTS_VALUE },
		{ 1024, 1536 },
		{ 256, AV_NOPTS_VALUE },
		{ 1024, 3000 },
	};
	for (const Fixture &fixture : fixtures) {
		AVFrame *frame = createS16Frame(44100, fixture.samples, fixture.timestamp);
		const bool converted = frame != nullptr && decoder.convert(frame, 1, 44100, sink);
		av_frame_free(&frame);
		if (!converted) {
			return false;
		}
	}
	if (!decoder.drain(sink) || sink.chunks.empty()) {
		return false;
	}
	std::uint32_t discontinuities = 0;
	std::int64_t discontinuity_start = -1;
	for (const AudioPcmChunk &chunk : sink.chunks) {
		if (chunk.discontinuity) {
			++discontinuities;
			discontinuity_start = chunk.startSample;
		}
	}
	const std::int64_t expected_rebase = av_rescale_rnd(3000, 48000, 44100, AV_ROUND_NEAR_INF);
	return discontinuities == 1 && discontinuity_start == expected_rebase;
}

static bool testDropAndReconfiguration()
{
	CapturingSink sink;
	FFmpegAudioDecoder decoder;
	decoder.reset(30, sink);
	sink.dropNext = true;
	AVFrame *frame = createS16Frame(48000, 4, 0);
	bool converted = frame != nullptr && decoder.convert(frame, 1, 48000, sink);
	av_frame_free(&frame);
	if (!converted || sink.droppedChunks.size() != 1 || !sink.chunks.empty()) {
		return false;
	}

	frame = createS16Frame(48000, 4, 4, false);
	converted = frame != nullptr && decoder.convert(frame, 1, 48000, sink);
	av_frame_free(&frame);
	if (!converted || sink.chunks.size() != 1 || !chunkInvariant(sink.chunks[0])
		|| sink.chunks[0].sequence != 1 || sink.chunks[0].startSample != 4 || !sink.chunks[0].discontinuity) {
		return false;
	}

	decoder.reset(31, sink);
	frame = createS16Frame(44100, 147, 0);
	converted = frame != nullptr && decoder.convert(frame, 1, 44100, sink);
	av_frame_free(&frame);
	if (!converted) {
		return false;
	}
	frame = createS16Frame(48000, 4, 160, false);
	converted = frame != nullptr && decoder.convert(frame, 1, 48000, sink);
	av_frame_free(&frame);
	if (!converted || sink.chunks.size() < 2 || !sink.chunks.back().discontinuity
		|| sink.chunks.back().startSample != 160 || sink.chunks.back().sequence != sink.chunks.size() - 1) {
		return false;
	}
	for (std::size_t index = 0; index + 1 < sink.chunks.size(); ++index) {
		if (sink.chunks[index].discontinuity || sink.chunks[index + 1].startSample
			!= sink.chunks[index].startSample + sink.chunks[index].frameCount) {
			return false;
		}
	}
	std::uint32_t old_segment_frames = 0;
	bool old_segment_has_audio = false;
	for (std::size_t index = 0; index + 1 < sink.chunks.size(); ++index) {
		old_segment_frames += sink.chunks[index].frameCount;
		for (std::uint8_t byte : sink.chunks[index].data) {
			old_segment_has_audio = old_segment_has_audio || byte != 0;
		}
	}
	return old_segment_frames == 160 && old_segment_has_audio;
}

static bool testDelayedDrop()
{
	CapturingSink sink;
	FFmpegAudioDecoder decoder;
	decoder.reset(32, sink);
	sink.dropNext = true;
	AVFrame *frame = createS16Frame(44100, 147, 0);
	const bool converted = frame != nullptr && decoder.convert(frame, 1, 44100, sink);
	av_frame_free(&frame);
	if (!converted || sink.droppedChunks.size() != 1 || !sink.chunks.empty() || !decoder.drain(sink)
		|| sink.chunks.size() != 1 || !sink.chunks[0].discontinuity || sink.chunks[0].sequence != 1) {
		return false;
	}
	const std::uint32_t total_frames = sink.droppedChunks[0].frameCount + sink.chunks[0].frameCount;
	const std::size_t chunk_count = sink.chunks.size();
	return total_frames == 160 && decoder.drain(sink) && sink.chunks.size() == chunk_count;
}

static bool testInvalidInputs()
{
	CapturingSink sink;
	FFmpegAudioDecoder decoder;
	decoder.reset(40, sink);
	if (decoder.convert(nullptr, 1, 48000, sink) || !sink.chunks.empty()) {
		return false;
	}
	AVFrame *frame = createS16Frame(48000, 4, 0);
	if (frame == nullptr) {
		return false;
	}
	frame->nb_samples = 0;
	const bool zero_samples_rejected = !decoder.convert(frame, 1, 48000, sink);
	frame->nb_samples = 4;
	frame->sample_rate = 0;
	const bool zero_rate_rejected = !decoder.convert(frame, 1, 48000, sink);
	frame->sample_rate = 48000;
	av_channel_layout_uninit(&frame->ch_layout);
	const bool missing_layout_rejected = !decoder.convert(frame, 1, 48000, sink);
	av_frame_free(&frame);
	frame = createS16Frame(48000, 4, 0);
	if (frame == nullptr) {
		return false;
	}
	std::uint8_t *second_plane = frame->extended_data[1];
	frame->extended_data[1] = nullptr;
	const bool missing_plane_rejected = !decoder.convert(frame, 1, 48000, sink);
	frame->extended_data[1] = second_plane;
	av_frame_free(&frame);
	frame = createS16Frame(48000, 4, 0, true, 3);
	if (frame == nullptr) {
		return false;
	}
	std::uint8_t *third_plane = frame->extended_data[2];
	frame->extended_data[2] = nullptr;
	const bool missing_multichannel_plane_rejected = !decoder.convert(frame, 1, 48000, sink);
	frame->extended_data[2] = third_plane;
	frame->format = AV_SAMPLE_FMT_NONE;
	const bool invalid_format_rejected = !decoder.convert(frame, 1, 48000, sink);
	av_frame_free(&frame);
	frame = createS16Frame(48000, FFmpegAudioDecoder::MAX_CHUNK_FRAMES + 1, 0);
	if (frame == nullptr) {
		return false;
	}
	const bool oversized_frame_is_terminal = !decoder.convert(frame, 1, 48000, sink) && decoder.isFailed();
	av_frame_free(&frame);
	decoder.reset(41, sink);
	return zero_samples_rejected && zero_rate_rejected && missing_layout_rejected && missing_plane_rejected
		&& missing_multichannel_plane_rejected && invalid_format_rejected
		&& oversized_frame_is_terminal && sink.chunks.empty() && !decoder.isFailed() && decoder.getGeneration() == 41;
}

int main()
{
	int failures = 0;
	const auto run = [&failures](const char *name, bool (*test)()) {
		if (!test()) {
			std::fprintf(stderr, "%s failed.\n", name);
			++failures;
		}
	};
	run("owned interleaving", testOwnedInterleaving);
	run("resampling and drain", testResamplingAndDrain);
	run("timeline and reset", testTimelineAndReset);
	run("resampled missing timestamps", testResampledMissingTimestamps);
	run("drop and reconfiguration", testDropAndReconfiguration);
	run("delayed drop", testDelayedDrop);
	run("invalid inputs", testInvalidInputs);
	if (failures != 0) {
		std::fprintf(stderr, "%d FFmpeg audio decoder contract checks failed.\n", failures);
		return 1;
	}
	std::puts("FFmpeg audio decoder contract passed.");
	return 0;
}
