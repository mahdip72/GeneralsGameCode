#include "VideoDevice/FFmpeg/FFmpegAudioDecoder.h"
#include "VideoDevice/FFmpeg/FFmpegFile.h"
#include "VideoDevice/FFmpeg/FFmpegMoviePlayback.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/frame.h>
#include <libavutil/log.h>
}

#include <algorithm>
#include <cstdarg>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <utility>
#include <vector>

class FFmpegMoviePlaybackTestAccess
{
public:
	static bool admitAudioFrame(FFmpegMoviePlayback &playback, const FFmpegFrameMetadata &metadata)
	{
		return playback.isAudioFrameAdmitted(metadata);
	}

	static bool deliverFrame(FFmpegMoviePlayback &playback, const AVFrame *frame,
		const FFmpegFrameMetadata &metadata)
	{
		if (!playback.m_audioSink->canAccept(2)) {
			return false;
		}
		playback.handleFrame(frame, metadata);
		return playback.state() != FFmpegMoviePlaybackState::FAILED;
	}

	static bool endInput(FFmpegMoviePlayback &playback)
	{
		return playback.handleEndOfInput();
	}
};

static_assert(UINTPTR_MAX == UINT64_MAX, "The native FFmpeg movie playback contract is x64-only.");

static std::size_t g_capturedAvLogMessages = 0;

static void captureExpectedAvLog(void *, int, const char *, va_list)
{
    ++g_capturedAvLogMessages;
}

class ScopedExpectedAvLogCapture final
{
public:
    ScopedExpectedAvLogCapture() : m_messagesAtStart(g_capturedAvLogMessages)
    {
        av_log_set_callback(captureExpectedAvLog);
    }

    ~ScopedExpectedAvLogCapture()
    {
        av_log_set_callback(av_log_default_callback);
    }

    std::size_t messageCount() const
    {
        return g_capturedAvLogMessages - m_messagesAtStart;
    }

private:
    std::size_t m_messagesAtStart;
};

class MemoryTestFile final : public FFmpegFileSource
{
public:
    explicit MemoryTestFile(const char *path)
    {
        std::ifstream input(path, std::ios::binary);
        m_data.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    }

    Int read(void *buffer, Int bytes) override
    {
        if (m_closed || buffer == nullptr || bytes < 0 || m_position > m_data.size()
            || (failAfterReadCount >= 0 && m_readCount >= failAfterReadCount)) {
            return -1;
        }
        ++m_readCount;
        const std::size_t readableSize = truncateAfterBytes >= 0
            ? std::min(m_data.size(), static_cast<std::size_t>(truncateAfterBytes)) : m_data.size();
        if (truncateAfterBytes >= 0 && m_position >= readableSize) {
            return -1;
        }
        if (m_position > readableSize) {
            return -1;
        }
        const std::size_t available = readableSize - m_position;
        const std::size_t requested = static_cast<std::size_t>(bytes);
        const std::size_t count = std::min(available, requested);
        if (count != 0) {
            std::memcpy(buffer, m_data.data() + m_position, count);
            m_position += count;
        }
        return static_cast<Int>(count);
    }

    Int64 seek(Int64 offset, FFmpegFileSeekMode mode) override
    {
        if (m_closed) {
            return -1;
        }
        Int64 base = 0;
        switch (mode) {
            case FFmpegFileSeekMode::START:
                break;
            case FFmpegFileSeekMode::CURRENT:
                base = static_cast<Int64>(m_position);
                break;
            case FFmpegFileSeekMode::END:
                base = static_cast<Int64>(m_data.size());
                break;
        }
        const Int64 target = base + offset;
        if (target < 0 || target > static_cast<Int64>(m_data.size())) {
            return -1;
        }
        m_position = static_cast<std::size_t>(target);
        return static_cast<Int64>(m_position);
    }

    Int64 size() const override { return static_cast<Int64>(m_data.size()); }
    void close() override { m_closed = true; }
    bool valid() const { return !m_data.empty(); }
    void overwriteAll(std::uint8_t value)
    {
        std::fill(m_data.begin(), m_data.end(), static_cast<char>(value));
    }
    Int m_readCount = 0;
    Int failAfterReadCount = -1;
    Int64 truncateAfterBytes = -1;

private:
    std::vector<char> m_data;
    std::size_t m_position = 0;
    bool m_closed = false;
};

class RecordingSink : public AudioPcmSink
{
public:
	bool canAccept(std::size_t submissions) const noexcept override
	{
		++capacityChecks;
		return !resetPending && availableSubmissions >= submissions;
	}

    AudioPcmSubmitResult submit(AudioPcmChunk &&chunk) override
    {
        ++submitCalls;
        if (rejectStale && chunk.generation != currentGeneration) {
            rejected.push_back(std::move(chunk));
            return AudioPcmSubmitResult::DROPPED;
        }
        if (dropAll || dropNext) {
            dropNext = false;
            dropped.push_back(std::move(chunk));
            return AudioPcmSubmitResult::DROPPED;
        }
        if (failNext) {
            failNext = false;
            failed.push_back(std::move(chunk));
            return AudioPcmSubmitResult::FAILED;
        }
        events.push_back("submit:" + std::to_string(chunk.generation) + ":" + std::to_string(chunk.sequence));
        accepted.push_back(std::move(chunk));
        return AudioPcmSubmitResult::ACCEPTED;
    }

	void reset(std::uint64_t generation) override
	{
		currentGeneration = generation;
		++resetCalls;
			events.push_back("reset:" + std::to_string(generation));
		if (serviceReopensAdmission) {
			resetPending = true;
			availableSubmissions = 0;
		}
	}

	void endOfStream() noexcept override
	{
		++endOfStreamCalls;
	}

	bool isDrained() const noexcept override
	{
		return !holdDrain || drainReleased;
	}

	bool serviceSink() noexcept override
	{
		++serviceCalls;
		if (resetPending) {
			resetPending = false;
			availableSubmissions = (std::numeric_limits<std::size_t>::max)();
		}
		return true;
	}

	bool setOutputGain(double gain) noexcept override
	{
		outputGains.push_back(gain);
		return nativeOutputGain;
	}

    bool getPlayedSample(std::int64_t &sample) const noexcept override
    {
        if (!clockEnabled) {
            return false;
        }
        sample = playedSample;
        return true;
    }

    std::uint64_t currentGeneration = 0;
    std::vector<AudioPcmChunk> accepted;
    std::vector<AudioPcmChunk> dropped;
    std::vector<AudioPcmChunk> failed;
    std::vector<AudioPcmChunk> rejected;
	std::vector<std::string> events;
	std::vector<double> outputGains;
    std::int64_t playedSample = -1;
	std::size_t submitCalls = 0;
	std::size_t resetCalls = 0;
	std::size_t endOfStreamCalls = 0;
	std::size_t serviceCalls = 0;
	mutable std::size_t capacityChecks = 0;
	std::size_t availableSubmissions = (std::numeric_limits<std::size_t>::max)();
    bool dropNext = false;
    bool dropAll = false;
    bool failNext = false;
	bool rejectStale = true;
	bool clockEnabled = false;
	bool holdDrain = false;
	bool drainReleased = false;
	bool nativeOutputGain = false;
	bool resetPending = false;
	bool serviceReopensAdmission = false;
};

class ManualClock
{
public:
    std::int64_t now() const { return microseconds; }
    std::int64_t microseconds = 0;
};

class EightSlotSink final : public RecordingSink
{
public:
	static constexpr std::size_t SLOT_COUNT = 8;

	EightSlotSink() { availableSubmissions = SLOT_COUNT; }

	AudioPcmSubmitResult submit(AudioPcmChunk &&chunk) override
	{
		if (availableSubmissions == 0) {
			dropped.push_back(std::move(chunk));
			return AudioPcmSubmitResult::DROPPED;
		}
		const AudioPcmSubmitResult result = RecordingSink::submit(std::move(chunk));
		if (result == AudioPcmSubmitResult::ACCEPTED) {
			--availableSubmissions;
		}
		return result;
	}

	void reset(std::uint64_t generation) override
	{
		RecordingSink::reset(generation);
		availableSubmissions = SLOT_COUNT;
	}

	bool isDrained() const noexcept override { return availableSubmissions == SLOT_COUNT; }

	void completePending()
	{
		if (!accepted.empty()) {
			playedSample = accepted.back().startSample + accepted.back().frameCount;
		}
		availableSubmissions = SLOT_COUNT;
	}
};

static bool deliverTestAudio(FFmpegMoviePlayback &playback, int samples,
	std::int64_t timestamp, int channels = 2)
{
	AVFrame *frame = av_frame_alloc();
	if (frame == nullptr) {
		return false;
	}
	frame->format = AV_SAMPLE_FMT_FLTP;
	frame->sample_rate = 48000;
	frame->nb_samples = samples;
	frame->pts = timestamp;
	frame->best_effort_timestamp = timestamp;
	av_channel_layout_default(&frame->ch_layout, channels);
	if (av_frame_get_buffer(frame, 0) < 0) {
		av_frame_free(&frame);
		return false;
	}
	for (int channel = 0; channel < channels; ++channel) {
		float *plane = reinterpret_cast<float *>(frame->extended_data[channel]);
		std::fill(plane, plane + samples, channel == 0 ? 0.25f : -0.25f);
	}
	FFmpegFrameMetadata metadata;
	metadata.streamType = AVMEDIA_TYPE_AUDIO;
	metadata.timeBaseNumerator = 1;
	metadata.timeBaseDenominator = 48000;
	metadata.presentationTimestamp = timestamp;
	const bool result = FFmpegMoviePlaybackTestAccess::deliverFrame(playback, frame, metadata);
	av_frame_free(&frame);
	return result;
}

static bool deliverTestVideo(FFmpegMoviePlayback &playback)
{
	AVFrame *frame = av_frame_alloc();
	if (frame == nullptr) {
		return false;
	}
	frame->format = AV_PIX_FMT_YUV420P;
	frame->width = 2;
	frame->height = 2;
	if (av_frame_get_buffer(frame, 0) < 0) {
		av_frame_free(&frame);
		return false;
	}
	FFmpegFrameMetadata metadata;
	metadata.streamType = AVMEDIA_TYPE_VIDEO;
	metadata.timeBaseNumerator = 1;
	metadata.timeBaseDenominator = 30;
	metadata.presentationTimestamp = 0;
	const bool result = FFmpegMoviePlaybackTestAccess::deliverFrame(playback, frame, metadata);
	av_frame_free(&frame);
	return result;
}

struct VideoTrace
{
    std::vector<FFmpegFrameMetadata> frames;
    std::vector<std::uint64_t> generations;
};

static void captureVideo(const AVFrame *, const FFmpegFrameMetadata &metadata, void *userData)
{
    VideoTrace *trace = static_cast<VideoTrace *>(userData);
    trace->frames.push_back(metadata);
}

struct PlaybackRun
{
    RecordingSink sink;
    VideoTrace video;
    std::vector<std::string> events;
    std::size_t pumpCalls = 0;
    std::uint64_t drainCalls = 0;
    std::uint64_t resetCount = 0;
    std::uint64_t generation = 0;
    std::int64_t totalSamples = 0;
    bool hasCurrentFrame = false;
};

static bool openFile(const char *path, MemoryTestFile &input, FFmpegFile &file)
{
    if (!input.valid() || input.size() <= 0) {
        return false;
    }
    return file.open(input);
}

static bool testManyAudioFramesPrimeBeforeFirstVideo(const char *audioPath)
{
	MemoryTestFile input(audioPath);
	FFmpegFile file;
	if (!openFile(audioPath, input, file)) {
		return false;
	}
	EightSlotSink sink;
	ManualClock clock;
	FFmpegMoviePlaybackOptions options;
	options.clock = [&clock]() { return clock.now(); };
	options.gain = 0.5;
	FFmpegMoviePlayback playback(file, &sink, options);
	bool audioPublishedBeforeVideo = false;
	playback.setVideoCallback([&sink, &audioPublishedBeforeVideo](const AVFrame *,
		const FFmpegFrameMetadata &, void *) {
		audioPublishedBeforeVideo = sink.accepted.size() == 1;
	});
	// Retail BIK packet shape: many 1920-sample planar-float audio frames,
	// only the first timestamped, before one video frame. No wall-clock time
	// or hardware completion advances while the constructor primes the file.
	for (int frame = 0; frame < 19; ++frame) {
		if (!deliverTestAudio(playback, 1920, frame == 0 ? 0 : AV_NOPTS_VALUE)
			|| !sink.serviceSink() || !sink.accepted.empty()) {
			return false;
		}
	}
	if (!deliverTestVideo(playback) || !audioPublishedBeforeVideo
		|| !playback.hasCurrentFrame() || !playback.isFrameReady()
		|| sink.accepted.size() != 1 || sink.availableSubmissions != 7
		|| !sink.dropped.empty() || clock.microseconds != 0) {
		return false;
	}
	const AudioPcmChunk &prefix = sink.accepted.front();
	if (prefix.frameCount != 19 * 1920 || prefix.startSample != 0
		|| prefix.sequence != 0 || prefix.generation != 1 || prefix.sourceChannels != 2
		|| prefix.discontinuity || prefix.data.size() != 19 * 1920 * 4) {
		return false;
	}
	for (std::size_t offset = 0; offset < prefix.data.size(); offset += sizeof(std::int16_t)) {
		std::int16_t sample = 0;
		std::memcpy(&sample, prefix.data.data() + offset, sizeof(sample));
		if (sample != (offset % 4 == 0 ? 4096 : -4096)) {
			return false;
		}
	}
	// The final partial audio chunk must flush at EOF even without another
	// video frame, and terminal completion still waits for the native sink.
	if (!deliverTestAudio(playback, 960, AV_NOPTS_VALUE)
		|| sink.accepted.size() != 1 || !FFmpegMoviePlaybackTestAccess::endInput(playback)
		|| playback.state() != FFmpegMoviePlaybackState::DRAINING
		|| sink.accepted.size() != 2 || sink.endOfStreamCalls != 1
		|| sink.accepted.back().frameCount != 960 || sink.accepted.back().startSample != 36480
		|| sink.accepted.back().sequence != 1) {
		return false;
	}
	sink.completePending();
	return playback.pump(1) && playback.state() == FFmpegMoviePlaybackState::ENDED;
}

static bool testMovieAggregationBoundAndReset(const char *audioPath)
{
	MemoryTestFile input(audioPath);
	FFmpegFile file;
	if (!openFile(audioPath, input, file)) {
		return false;
	}
	EightSlotSink sink;
	FFmpegMoviePlayback playback(file, &sink);
	for (int frame = 0; frame < 5; ++frame) {
		if (!deliverTestAudio(playback, 24000, frame == 0 ? 0 : AV_NOPTS_VALUE)) {
			return false;
		}
	}
	// Two full one-second chunks were flushed as the next input arrived;
	// the final half-second remains pending and must be discarded on seek.
	if (sink.accepted.size() != 2 || sink.accepted[0].frameCount != 48000
		|| sink.accepted[1].frameCount != 48000 || sink.accepted[0].sequence != 0
		|| sink.accepted[1].sequence != 1 || sink.accepted[1].startSample != 48000
		|| !playback.seekFrame(0) || !deliverTestAudio(playback, 960, 0)
		|| !deliverTestVideo(playback) || sink.accepted.size() != 3) {
		return false;
	}
	const AudioPcmChunk &restarted = sink.accepted.back();
	return restarted.generation == 2 && restarted.sequence == 0 && restarted.startSample == 0
		&& restarted.frameCount == 960 && !restarted.discontinuity && sink.dropped.empty();
}

static bool testMovieAggregationPreservesDiscontinuityAndProvenance(const char *audioPath)
{
	MemoryTestFile input(audioPath);
	FFmpegFile file;
	if (!openFile(audioPath, input, file)) {
		return false;
	}
	EightSlotSink sink;
	FFmpegMoviePlayback playback(file, &sink);
	if (!deliverTestAudio(playback, 1920, 0) || !deliverTestAudio(playback, 1920, 1920)
		|| !deliverTestAudio(playback, 1920, 10000) || !deliverTestAudio(playback, 960, 11920)
		|| !deliverTestAudio(playback, 960, 12880, 1) || !deliverTestVideo(playback)
		|| sink.accepted.size() != 3 || !sink.dropped.empty()) {
		return false;
	}
	const AudioPcmChunk &prefix = sink.accepted[0];
	const AudioPcmChunk &gap = sink.accepted[1];
	const AudioPcmChunk &mono = sink.accepted[2];
	return prefix.frameCount == 3840 && prefix.startSample == 0 && !prefix.discontinuity
		&& prefix.sourceChannels == 2 && prefix.sequence == 0
		&& gap.frameCount == 2880 && gap.startSample == 10000 && gap.discontinuity
		&& gap.sourceChannels == 2 && gap.sequence == 1
		&& mono.frameCount == 960 && mono.startSample == 12880 && mono.discontinuity
		&& mono.sourceChannels == 1 && mono.sequence == 2;
}

static bool testMovieAggregationRetainsPerFrameSoftwareGain(const char *audioPath)
{
	MemoryTestFile input(audioPath);
	FFmpegFile file;
	if (!openFile(audioPath, input, file)) {
		return false;
	}
	RecordingSink sink;
	FFmpegMoviePlayback playback(file, &sink);
	if (!deliverTestAudio(playback, 960, 0)) {
		return false;
	}
	playback.setGain(0.0);
	if (!deliverTestAudio(playback, 960, AV_NOPTS_VALUE) || !deliverTestVideo(playback)
		|| sink.accepted.size() != 1 || sink.accepted[0].frameCount != 1920) {
		return false;
	}
	const AudioPcmChunk &chunk = sink.accepted[0];
	for (std::size_t offset = 0; offset < chunk.data.size(); offset += sizeof(std::int16_t)) {
		std::int16_t sample = 0;
		std::memcpy(&sample, chunk.data.data() + offset, sizeof(sample));
		const std::int16_t expected = offset >= 960 * 4 ? 0 : (offset % 4 == 0 ? 8192 : -8192);
		if (sample != expected) {
			return false;
		}
	}
	return true;
}

static bool runExistingMediaSmoke(const char *path)
{
	MemoryTestFile input(path);
	FFmpegFile file;
	if (!openFile(path, input, file)) {
		std::fprintf(stderr, "Could not open existing media: %s\n", path);
		return false;
	}
	EightSlotSink sink;
	FFmpegMoviePlaybackOptions options;
	options.mode = FFmpegMoviePlaybackMode::ONCE;
	FFmpegMoviePlayback playback(file, &sink, options);
	VideoTrace trace;
	playback.setVideoCallback(captureVideo, &trace);
	std::size_t firstVideoSteps = 0;
	while (firstVideoSteps < 256 && !playback.hasCurrentFrame() && !playback.isTerminal()) {
		++firstVideoSteps;
		if (!playback.pump(1) || !sink.serviceSink()) {
			return false;
		}
	}
	if (!playback.hasCurrentFrame() || playback.state() == FFmpegMoviePlaybackState::FAILED
		|| !sink.dropped.empty()) {
		std::fprintf(stderr, "First video did not prime in %zu steps; submitted=%zu dropped=%zu.\n",
			firstVideoSteps, sink.accepted.size(), sink.dropped.size());
		return false;
	}
	const std::size_t primedBuffers = sink.accepted.size();
	for (std::size_t call = 0; call < 100000 && !playback.isTerminal(); ++call) {
		// Completion is allowed only after first-frame priming has succeeded.
		sink.completePending();
		if (!playback.pump(64)) {
			return false;
		}
	}
	std::int64_t totalPcmFrames = 0;
	std::uint32_t largestChunk = 0;
	for (const AudioPcmChunk &chunk : sink.accepted) {
		totalPcmFrames += chunk.frameCount;
		largestChunk = std::max(largestChunk, chunk.frameCount);
	}
	std::fprintf(stderr, "Media smoke: first_video_steps=%zu primed_buffers=%zu video_frames=%zu "
		"pcm_frames=%lld max_pcm_chunk=%u dropped=%zu state=%u\n", firstVideoSteps, primedBuffers,
		trace.frames.size(), static_cast<long long>(totalPcmFrames), largestChunk, sink.dropped.size(),
		static_cast<unsigned int>(playback.state()));
	return playback.state() == FFmpegMoviePlaybackState::ENDED && sink.dropped.empty()
		&& largestChunk <= FFmpegAudioDecoder::MAX_CHUNK_FRAMES;
}

static bool runToEnd(const char *path, PlaybackRun &run, const FFmpegMoviePlaybackOptions &options,
    std::size_t maxPumps = 256)
{
    MemoryTestFile input(path);
    FFmpegFile file;
    if (!openFile(path, input, file)) {
        return false;
    }

    FFmpegMoviePlayback playback(file, &run.sink, options);
    playback.setVideoCallback(captureVideo, &run.video);
    while (!playback.isTerminal() && run.pumpCalls < maxPumps) {
        ++run.pumpCalls;
        if (!playback.pump(32)) {
            if (!playback.isTerminal()) {
                return false;
            }
        }
    }
    if (!playback.isTerminal() || playback.state() == FFmpegMoviePlaybackState::FAILED) {
        return false;
    }
    if (!playback.finish(32)) {
        return false;
    }
    run.drainCalls = playback.drainCount();
    run.resetCount = playback.resetCount();
    run.generation = playback.generation();
    run.hasCurrentFrame = playback.hasCurrentFrame();
    for (const AudioPcmChunk &chunk : run.sink.accepted) {
        run.totalSamples += chunk.frameCount;
    }
    return true;
}

static bool testIntegratedAudioVideoAndDrain(const char *audioPath)
{
    PlaybackRun run;
    FFmpegMoviePlaybackOptions options;
    options.mode = FFmpegMoviePlaybackMode::ONCE;
    options.gain = 1.0;
    if (!runToEnd(audioPath, run, options)) {
        return false;
    }
	if (run.video.frames.size() != 12 || run.sink.accepted.empty() || run.totalSamples != 26400
		|| run.drainCalls != 1 || run.resetCount != 1) {
		return false;
    }
    if (run.sink.accepted.front().sampleRate != 48000 || run.sink.accepted.front().channels != 2
        || run.sink.accepted.front().format != AudioPcmFormat::SIGNED_16_INTERLEAVED_LITTLE_ENDIAN) {
        return false;
    }
    std::int64_t previous = -1;
    for (const AudioPcmChunk &chunk : run.sink.accepted) {
        if (chunk.generation != 1 || chunk.startSample < previous
            || chunk.data.size() != static_cast<std::size_t>(chunk.frameCount) * 2 * sizeof(std::int16_t)) {
            return false;
        }
        previous = chunk.startSample;
        bool nonzero = false;
        for (std::uint8_t value : chunk.data) {
            nonzero = nonzero || value != 0;
        }
        if (!nonzero) {
            return false;
        }
    }
    for (std::size_t i = 1; i < run.video.frames.size(); ++i) {
        if (run.video.frames[i].presentationTimestamp < run.video.frames[i - 1].presentationTimestamp
            || run.video.frames[i].streamType != AVMEDIA_TYPE_VIDEO
            || run.video.frames[i].timeBaseNumerator <= 0 || run.video.frames[i].timeBaseDenominator <= 0) {
            return false;
        }
    }
	return true;
}

static bool testAcceptedAudioWaitsForSinkDrain(const char *audioPath)
{
	MemoryTestFile input(audioPath);
	FFmpegFile file;
	if (!openFile(audioPath, input, file)) {
		return false;
	}
	RecordingSink sink;
	sink.holdDrain = true;
	VideoTrace trace;
	FFmpegMoviePlaybackOptions options;
	options.mode = FFmpegMoviePlaybackMode::SHOW_LAST_FRAME;
	FFmpegMoviePlayback playback(file, &sink, options);
	playback.setVideoCallback(captureVideo, &trace);
	for (std::size_t i = 0; i < 256 && playback.state() != FFmpegMoviePlaybackState::DRAINING;
		++i) {
		if (!playback.pump(32)) {
			return false;
		}
	}
	if (playback.state() != FFmpegMoviePlaybackState::DRAINING
		|| trace.frames.size() != 12 || !playback.hasCurrentFrame()
		|| sink.endOfStreamCalls != 1 || sink.accepted.empty()) {
		return false;
	}
	for (std::size_t i = 0; i < 3; ++i) {
		if (!playback.pump(1) || playback.state() != FFmpegMoviePlaybackState::DRAINING) {
			return false;
		}
	}
	sink.drainReleased = true;
	if (!playback.pump(1) || playback.state() != FFmpegMoviePlaybackState::ENDED
		|| !playback.hasCurrentFrame() || sink.endOfStreamCalls != 1) {
		return false;
	}
	std::int64_t acceptedFrames = 0;
	for (const AudioPcmChunk &chunk : sink.accepted) {
		acceptedFrames += chunk.frameCount;
	}
	return acceptedFrames == 26400 && sink.serviceCalls >= 4;
}

static bool testNoCallbackDrainFailsBoundedly(const char *audioPath)
{
	MemoryTestFile input(audioPath);
	FFmpegFile file;
	if (!openFile(audioPath, input, file)) {
		return false;
	}
	RecordingSink sink;
	// The sink accepts queued audio but never reports completion. This models a
	// device that has stopped making callback progress without publishing an
	// explicit error.
	sink.holdDrain = true;
	ManualClock clock;
	FFmpegMoviePlaybackOptions options;
	options.mode = FFmpegMoviePlaybackMode::SHOW_LAST_FRAME;
	options.clock = [&clock]() { return clock.now(); };
	FFmpegMoviePlayback playback(file, &sink, options);
	for (std::size_t i = 0; i < 256 && playback.state() != FFmpegMoviePlaybackState::DRAINING; ++i) {
		if (!playback.pump(32)) {
			return false;
		}
	}
	if (playback.state() != FFmpegMoviePlaybackState::DRAINING
		|| sink.endOfStreamCalls != 1 || sink.resetCalls != 1) {
		return false;
	}

	clock.microseconds = 5000000;
	if (!playback.pump(1) || playback.state() != FFmpegMoviePlaybackState::DRAINING) {
		return false;
	}
	clock.microseconds = 11000000;
	const bool failed = !playback.pump(1)
		&& playback.isTerminal()
		&& playback.state() == FFmpegMoviePlaybackState::FAILED
		&& sink.resetCalls == 2;
	return failed && sink.endOfStreamCalls == 1;
}

static bool testNoCallbackBackpressureFailsBoundedly(const char *audioPath)
{
	MemoryTestFile input(audioPath);
	FFmpegFile file;
	if (!openFile(audioPath, input, file)) {
		return false;
	}
	RecordingSink sink;
	// No completed callbacks ever free an admission slot.  The stream must
	// remain healthy for a short stall, then fail instead of spinning forever.
	sink.availableSubmissions = 0;
	ManualClock clock;
	FFmpegMoviePlaybackOptions options;
	options.clock = [&clock]() { return clock.now(); };
	FFmpegMoviePlayback playback(file, &sink, options);
	if (playback.state() != FFmpegMoviePlaybackState::ACTIVE
		|| !playback.pump(1) || playback.state() != FFmpegMoviePlaybackState::ACTIVE
		|| sink.submitCalls != 0) {
		return false;
	}
	clock.microseconds = 5000000;
	if (!playback.pump(1) || playback.state() != FFmpegMoviePlaybackState::ACTIVE) {
		return false;
	}
	clock.microseconds = 11000000;
	const bool failed = !playback.pump(1)
		&& playback.isTerminal()
		&& playback.state() == FFmpegMoviePlaybackState::FAILED
		&& sink.resetCalls == 2;
	return failed && sink.endOfStreamCalls == 0;
}

static bool testCallbackDetachesBeforeFileReuse(const char *audioPath)
{
    MemoryTestFile input(audioPath);
    FFmpegFile file;
    if (!openFile(audioPath, input, file)) {
        return false;
    }
    {
        RecordingSink sink;
        FFmpegMoviePlaybackOptions options;
        options.mode = FFmpegMoviePlaybackMode::ONCE;
        FFmpegMoviePlayback playback(file, &sink, options);
        if (!playback.pump(1)) {
            return false;
        }
    }
    if (!file.seekFrame(0)) {
        return false;
    }
    for (std::size_t i = 0; i < 256; ++i) {
        if (file.decodePacket()) {
            continue;
        }
        return file.isAtEnd() && !file.hasError();
    }
    return false;
}

static bool testSilentVideoFallback(const char *videoPath)
{
    PlaybackRun run;
    FFmpegMoviePlaybackOptions options;
    options.mode = FFmpegMoviePlaybackMode::ONCE;
    if (!runToEnd(videoPath, run, options)) {
        return false;
    }
    return run.video.frames.size() == 12 && run.sink.submitCalls == 0 && run.sink.accepted.empty()
        && run.sink.dropped.empty() && run.drainCalls == 1;
}

static bool testDisabledAudio(const char *audioPath)
{
    PlaybackRun run;
    FFmpegMoviePlaybackOptions options;
    options.mode = FFmpegMoviePlaybackMode::ONCE;
    options.audioEnabled = false;
    if (!runToEnd(audioPath, run, options)) {
        return false;
    }
    return run.video.frames.size() == 12 && run.sink.submitCalls == 0
        && run.sink.accepted.empty() && run.sink.dropped.empty()
        && run.sink.failed.empty() && run.drainCalls == 1;
}

static bool testBackpressurePreservesAudioAndVideo(const char *audioPath)
{
	MemoryTestFile input(audioPath);
	FFmpegFile file;
	if (!openFile(audioPath, input, file)) {
		return false;
	}
	RecordingSink sink;
	sink.availableSubmissions = 1;
	VideoTrace trace;
	FFmpegMoviePlayback playback(file, &sink);
	playback.setVideoCallback(captureVideo, &trace);
	for (std::size_t call = 0; call < 3; ++call) {
		if (!playback.pump(32)) {
			return false;
		}
	}
	if (!trace.frames.empty() || sink.submitCalls != 0 || sink.serviceCalls != 3
		|| sink.capacityChecks < 3 || playback.isTerminal()) {
		return false;
	}

	sink.availableSubmissions = (std::numeric_limits<std::size_t>::max)();
	for (std::size_t call = 0; call < 256 && !playback.isTerminal(); ++call) {
		if (!playback.pump(32)) {
			return false;
		}
	}
	if (playback.state() != FFmpegMoviePlaybackState::ENDED || trace.frames.size() != 12
		|| !sink.dropped.empty() || sink.endOfStreamCalls != 1) {
		return false;
	}
	std::int64_t expectedStart = 0;
	std::uint64_t expectedSequence = 0;
	for (const AudioPcmChunk &chunk : sink.accepted) {
		if (chunk.sequence != expectedSequence++ || chunk.startSample != expectedStart) {
			return false;
		}
		expectedStart += chunk.frameCount;
	}
	return expectedStart == 26400;
}

static bool testResetPendingServiceReopensAdmission(const char *audioPath)
{
	MemoryTestFile input(audioPath);
	FFmpegFile file;
	if (!openFile(audioPath, input, file)) {
		return false;
	}
	RecordingSink sink;
	// XAudio2PcmVoice::reset leaves a newly opened voice reset-pending until
	// owner-side service performs stop/flush.  Admission is unavailable during
	// that transition and reopens only after service returns.
	sink.serviceReopensAdmission = true;
	VideoTrace trace;
	FFmpegMoviePlayback playback(file, &sink);
	playback.setVideoCallback(captureVideo, &trace);
	if (sink.resetCalls != 1 || !sink.resetPending || sink.availableSubmissions != 0
		|| sink.serviceCalls != 0) {
		return false;
	}
	// A one-step owner call must report the successful reset/service transition
	// as progress; otherwise FFmpegVideoStream treats the still-healthy stream
	// as failed before it can attempt its first decode step.
	if (!playback.pump(1) || playback.state() != FFmpegMoviePlaybackState::ACTIVE
		|| sink.resetPending || sink.serviceCalls != 1 || sink.submitCalls != 0
		|| !trace.frames.empty()) {
		return false;
	}
	for (std::size_t step = 0; step < 128 && trace.frames.empty(); ++step) {
		if (!playback.pump(1)) {
			return false;
		}
	}
	return trace.frames.size() == 1 && playback.state() == FFmpegMoviePlaybackState::ACTIVE;
}

static bool testUnexpectedMovieDropFailsBoundedly(const char *audioPath)
{
	MemoryTestFile input(audioPath);
	FFmpegFile file;
	if (!openFile(audioPath, input, file)) {
		return false;
	}
	RecordingSink sink;
	sink.dropNext = true;
	FFmpegMoviePlayback playback(file, &sink);
	for (std::size_t call = 0; call < 64 && !playback.isTerminal(); ++call) {
		playback.pump(1);
	}
	return playback.state() == FFmpegMoviePlaybackState::FAILED
		&& sink.dropped.size() == 1 && sink.endOfStreamCalls == 0;
}

static bool testTerminalResetOwnership(const char *audioPath)
{
	RecordingSink normalSink;
	{
		MemoryTestFile input(audioPath);
		FFmpegFile file;
		if (!openFile(audioPath, input, file)) {
			return false;
		}
		FFmpegMoviePlaybackOptions options;
		options.mode = FFmpegMoviePlaybackMode::ONCE;
		FFmpegMoviePlayback playback(file, &normalSink, options);
		for (std::size_t i = 0; i < 256 && !playback.isTerminal(); ++i) {
			playback.pump(32);
		}
		if (playback.state() != FFmpegMoviePlaybackState::ENDED || normalSink.resetCalls != 1) {
			return false;
		}
	}
	if (normalSink.resetCalls != 1) {
		return false;
	}

	RecordingSink failedSink;
	{
		MemoryTestFile input(audioPath);
		FFmpegFile file;
		if (!openFile(audioPath, input, file)) {
			return false;
		}
		failedSink.failNext = true;
		FFmpegMoviePlaybackOptions options;
		options.mode = FFmpegMoviePlaybackMode::LOOP;
		FFmpegMoviePlayback playback(file, &failedSink, options);
		for (std::size_t i = 0; i < 256 && !playback.isTerminal(); ++i) {
			playback.pump(32);
		}
		if (playback.state() != FFmpegMoviePlaybackState::FAILED || failedSink.resetCalls != 2) {
			return false;
		}
	}
	return failedSink.resetCalls == 2;
}

static bool testGainAndMute(const char *audioPath)
{
    PlaybackRun full;
    PlaybackRun half;
    PlaybackRun muted;
    PlaybackRun clipped;
    FFmpegMoviePlaybackOptions fullOptions;
    fullOptions.mode = FFmpegMoviePlaybackMode::ONCE;
    fullOptions.gain = 1.0;
    FFmpegMoviePlaybackOptions halfOptions = fullOptions;
    halfOptions.gain = 0.5;
    FFmpegMoviePlaybackOptions muteOptions = fullOptions;
    muteOptions.gain = 0.0;
    FFmpegMoviePlaybackOptions clippedOptions = fullOptions;
    clippedOptions.gain = 2.0;
    if (!runToEnd(audioPath, full, fullOptions) || !runToEnd(audioPath, half, halfOptions)
        || !runToEnd(audioPath, muted, muteOptions) || !runToEnd(audioPath, clipped, clippedOptions)
        || full.sink.accepted.empty()
        || full.sink.accepted.size() != half.sink.accepted.size()
        || full.sink.accepted.size() != muted.sink.accepted.size()) {
        return false;
    }
    for (std::size_t chunkIndex = 0; chunkIndex < full.sink.accepted.size(); ++chunkIndex) {
        const AudioPcmChunk &fullChunk = full.sink.accepted[chunkIndex];
        const AudioPcmChunk &halfChunk = half.sink.accepted[chunkIndex];
        const AudioPcmChunk &muteChunk = muted.sink.accepted[chunkIndex];
        if (fullChunk.data.size() != halfChunk.data.size() || fullChunk.data.size() != muteChunk.data.size()) {
            return false;
        }
        for (std::size_t offset = 0; offset < fullChunk.data.size(); offset += sizeof(std::int16_t)) {
            std::int16_t fullSample = 0;
            std::int16_t halfSample = 0;
            std::int16_t muteSample = 0;
            std::memcpy(&fullSample, fullChunk.data.data() + offset, sizeof(fullSample));
            std::memcpy(&halfSample, halfChunk.data.data() + offset, sizeof(halfSample));
            std::memcpy(&muteSample, muteChunk.data.data() + offset, sizeof(muteSample));
            if (halfSample != static_cast<std::int16_t>(std::lrint(static_cast<double>(fullSample) * 0.5))
                || muteSample != 0) {
                return false;
            }
        }
    }
    bool sawPositiveClip = false;
    bool sawNegativeClip = false;
    for (const AudioPcmChunk &chunk : clipped.sink.accepted) {
        for (std::size_t offset = 0; offset < chunk.data.size(); offset += sizeof(std::int16_t)) {
            std::int16_t sample = 0;
            std::memcpy(&sample, chunk.data.data() + offset, sizeof(sample));
            sawPositiveClip = sawPositiveClip || sample == (std::numeric_limits<std::int16_t>::max)();
            sawNegativeClip = sawNegativeClip || sample == (std::numeric_limits<std::int16_t>::min)();
        }
    }
    return full.totalSamples == half.totalSamples && half.totalSamples == muted.totalSamples
        && muted.totalSamples == clipped.totalSamples && sawPositiveClip && sawNegativeClip;
}

static bool testReadAndSeekFailuresDoNotLoop(const char *audioPath)
{
    ScopedExpectedAvLogCapture avLogCapture;
    MemoryTestFile readInput(audioPath);
    FFmpegFile readFile;
    if (!openFile(audioPath, readInput, readFile)) {
        return false;
    }
    if (!readFile.seekFrame(0)) {
        return false;
    }
    readInput.failAfterReadCount = readInput.m_readCount;
    RecordingSink readSink;
    FFmpegMoviePlaybackOptions options;
    options.mode = FFmpegMoviePlaybackMode::LOOP;
    FFmpegMoviePlayback readPlayback(readFile, &readSink, options);
    bool readFailed = false;
    std::size_t readPumpCalls = 0;
    for (; readPumpCalls < 256 && !readPlayback.isTerminal(); ++readPumpCalls) {
        readPlayback.pump(32);
        if (readPlayback.state() == FFmpegMoviePlaybackState::FAILED) {
            readFailed = true;
            break;
        }
    }
    if (!readFailed || readPlayback.state() != FFmpegMoviePlaybackState::FAILED
        || readPlayback.generation() != 1 || readPlayback.drainCount() != 0
        || readSink.resetCalls < 2) {
        return false;
    }

    MemoryTestFile seekInput(audioPath);
    FFmpegFile seekFile;
    if (!openFile(audioPath, seekInput, seekFile)) {
        return false;
    }
    RecordingSink seekSink;
    FFmpegMoviePlayback seekPlayback(seekFile, &seekSink, options);
    if (!seekPlayback.pump(8)) {
        return false;
    }
    const bool seekResult = seekPlayback.seekFrame(-1);
    const bool success = !seekResult && seekPlayback.state() == FFmpegMoviePlaybackState::FAILED
        && !seekPlayback.hasCurrentFrame() && seekSink.resetCalls == 2
        && seekSink.events.size() >= 2 && seekSink.events[1].find("reset:2") == 0;
    return success && avLogCapture.messageCount() > 0;
}

static bool testTruncatedInputFailsAndMalformedContainerIsRejected(const char *audioPath)
{
    ScopedExpectedAvLogCapture avLogCapture;
    MemoryTestFile truncatedInput(audioPath);
    FFmpegFile truncatedFile;
    if (!openFile(audioPath, truncatedInput, truncatedFile)) {
        return false;
    }
    truncatedInput.truncateAfterBytes = truncatedInput.size() / 2;
    RecordingSink truncatedSink;
    FFmpegMoviePlaybackOptions options;
    options.mode = FFmpegMoviePlaybackMode::LOOP;
    FFmpegMoviePlayback truncatedPlayback(truncatedFile, &truncatedSink, options);
    for (std::size_t i = 0; i < 128 && !truncatedPlayback.isTerminal(); ++i) {
        truncatedPlayback.pump(32);
    }
    if (truncatedPlayback.state() != FFmpegMoviePlaybackState::FAILED
        || truncatedPlayback.drainCount() != 0 || truncatedSink.resetCalls < 2) {
        return false;
    }

    // Arbitrary media-byte damage is allowed to be concealed by FFmpeg and is
    // therefore not a portable failure fixture. Destroy the complete probe
    // prefix instead so every supported FFmpeg version must reject the
    // malformed input before playback construction.
    MemoryTestFile malformedInput(audioPath);
    malformedInput.overwriteAll(0);
    FFmpegFile malformedFile;
    const bool malformedRejected = !openFile(audioPath, malformedInput, malformedFile);
    return malformedRejected && avLogCapture.messageCount() > 0;
}

static bool testTerminalSinkFailure(const char *audioPath)
{
    MemoryTestFile input(audioPath);
    FFmpegFile file;
    if (!openFile(audioPath, input, file)) {
        return false;
    }
    RecordingSink sink;
    sink.failNext = true;
    FFmpegMoviePlaybackOptions options;
    options.mode = FFmpegMoviePlaybackMode::LOOP;
    FFmpegMoviePlayback playback(file, &sink, options);
    for (std::size_t i = 0; i < 128 && !playback.isTerminal(); ++i) {
        playback.pump(32);
    }
    return playback.state() == FFmpegMoviePlaybackState::FAILED
        && playback.generation() == 1 && playback.drainCount() == 0
        && sink.failed.size() == 1 && sink.submitCalls == 1 && sink.resetCalls == 2
        && sink.events.back().find("reset:1") == 0;
}

static bool testMonotonicClockIgnoresCompletedAudioStaircase(const char *audioPath)
{
    MemoryTestFile input(audioPath);
    FFmpegFile file;
    if (!openFile(audioPath, input, file)) {
        return false;
    }
    RecordingSink sink;
    sink.clockEnabled = true;
    sink.playedSample = 0;
	ManualClock clock;
    FFmpegMoviePlaybackOptions options;
    options.mode = FFmpegMoviePlaybackMode::ONCE;
	options.clock = [&clock]() { return clock.now(); };
    FFmpegMoviePlayback playback(file, &sink, options);
    VideoTrace trace;
    playback.setVideoCallback(captureVideo, &trace);
	for (std::size_t step = 0; step < 128 && trace.frames.empty(); ++step) {
		if (!playback.pump(1)) {
			return false;
		}
	}
	if (trace.frames.size() != 1 || !playback.isFrameReady()) {
		return false;
	}
    if (!playback.pump(64) || trace.frames.size() < 2 || playback.isFrameReady()) {
        return false;
    }
	// A completed-buffer clock can remain at zero for most of a large BIK
	// audio packet.  It must neither freeze nor prematurely release video.
	sink.playedSample = 2000;
	if (playback.isFrameReady()) {
		return false;
	}
	clock.microseconds = playback.videoPresentationTimeUs();
	return playback.isFrameReady();
}

static bool testNativeGainAvoidsQueuedPcmRewrite(const char *audioPath)
{
	PlaybackRun full;
	FFmpegMoviePlaybackOptions fullOptions;
	fullOptions.mode = FFmpegMoviePlaybackMode::ONCE;
	if (!runToEnd(audioPath, full, fullOptions) || full.sink.accepted.empty()) {
		return false;
	}

	MemoryTestFile input(audioPath);
	FFmpegFile file;
	if (!openFile(audioPath, input, file)) {
		return false;
	}
	RecordingSink sink;
	sink.nativeOutputGain = true;
	FFmpegMoviePlaybackOptions options;
	options.mode = FFmpegMoviePlaybackMode::ONCE;
	options.gain = 0.5;
	FFmpegMoviePlayback playback(file, &sink, options);
	playback.setGain(0.25);
	for (std::size_t call = 0; call < 256 && !playback.isTerminal(); ++call) {
		if (!playback.pump(32)) {
			return false;
		}
	}
	if (playback.state() != FFmpegMoviePlaybackState::ENDED
		|| sink.outputGains.size() != 2 || sink.outputGains[0] != 0.5
		|| sink.outputGains[1] != 0.25 || sink.accepted.size() != full.sink.accepted.size()) {
		return false;
	}
	for (std::size_t index = 0; index < sink.accepted.size(); ++index) {
		if (sink.accepted[index].data != full.sink.accepted[index].data) {
			return false;
		}
	}
	return true;
}

static bool testSeekAndGeneration(const char *audioPath)
{
    MemoryTestFile input(audioPath);
    FFmpegFile file;
    if (!openFile(audioPath, input, file)) {
        return false;
    }
    RecordingSink sink;
    sink.rejectStale = true;
    VideoTrace trace;
    FFmpegMoviePlaybackOptions options;
    options.mode = FFmpegMoviePlaybackMode::ONCE;
    FFmpegMoviePlayback playback(file, &sink, options);
    playback.setVideoCallback(captureVideo, &trace);
    if (!playback.pump(8) || !playback.seekFrame(6) || playback.generation() != 2
        || sink.resetCalls != 2 || sink.events.size() < 2 || sink.events[0].find("reset:1") != 0
        || sink.events[1].find("reset:2") != 0) {
        return false;
    }
    const std::size_t frameCountBeforeSeek = trace.frames.size();
	if (!playback.pump(1) || trace.frames.size() != frameCountBeforeSeek) {
		return false;
	}
    for (std::size_t i = 0; i < 128 && !playback.isTerminal(); ++i) {
        playback.pump(16);
    }
    if (playback.state() == FFmpegMoviePlaybackState::FAILED || !playback.isTerminal()
        || trace.frames.size() != frameCountBeforeSeek + 6 || sink.rejected.size() != 0) {
        return false;
    }
    const std::int64_t videoOriginUs = file.getVideoStartTimeMicroseconds();
    if (videoOriginUs < 4900000) {
        return false;
    }
    const FFmpegFrameRate rate = file.getVideoFrameRate();
    if (rate.numerator <= 0 || rate.denominator <= 0) {
        return false;
    }
    const std::int64_t targetUs = videoOriginUs + av_rescale_q(6,
        AVRational { rate.denominator, rate.numerator }, AVRational { 1, 1000000 });
    for (std::size_t i = frameCountBeforeSeek; i < trace.frames.size(); ++i) {
        const std::int64_t timestampUs = av_rescale_q(trace.frames[i].presentationTimestamp,
            AVRational { trace.frames[i].timeBaseNumerator, trace.frames[i].timeBaseDenominator },
            AVRational { 1, 1000000 });
        if (timestampUs < targetUs) {
            return false;
        }
    }
    return playback.drainCount() == 1;
}

static bool testSeekUsesAbsoluteTimeline(const char *audioPath)
{
	MemoryTestFile input(audioPath);
	FFmpegFile file;
	if (!openFile(audioPath, input, file)) {
		return false;
	}
	const std::int64_t videoOriginUs = file.getVideoStartTimeMicroseconds();
	const FFmpegFrameRate rate = file.getVideoFrameRate();
	if (videoOriginUs < 4900000 || rate.numerator <= 0 || rate.denominator <= 0) {
		return false;
	}
	const std::int64_t targetUs = videoOriginUs + av_rescale_q(6,
		AVRational { rate.denominator, rate.numerator }, AVRational { 1, 1000000 });
	FFmpegMoviePlayback playback(file, nullptr);
	if (!playback.seekFrame(6)) {
		return false;
	}
	FFmpegFrameMetadata metadata;
	metadata.streamType = AVMEDIA_TYPE_AUDIO;
	metadata.timeBaseNumerator = 1;
	metadata.timeBaseDenominator = 48000;
	metadata.presentationTimestamp = av_rescale_q(targetUs - 1000,
		AVRational { 1, 1000000 }, AVRational { 1, 48000 });
	if (FFmpegMoviePlaybackTestAccess::admitAudioFrame(playback, metadata)) {
		return false;
	}
	metadata.presentationTimestamp = av_rescale_q(targetUs + 1000,
		AVRational { 1, 1000000 }, AVRational { 1, 48000 });
	return FFmpegMoviePlaybackTestAccess::admitAudioFrame(playback, metadata);
}

static bool testSeekAdmitsTimestampLessAudio(const char *audioPath)
{
	MemoryTestFile input(audioPath);
	FFmpegFile file;
	if (!openFile(audioPath, input, file)) {
		return false;
	}
	RecordingSink sink;
	FFmpegMoviePlayback playback(file, &sink);
	if (!playback.seekFrame(6)) {
		return false;
	}
	FFmpegFrameMetadata metadata;
	metadata.streamType = AVMEDIA_TYPE_AUDIO;
	metadata.timeBaseNumerator = 1;
	metadata.timeBaseDenominator = 48000;
	metadata.presentationTimestamp = (std::numeric_limits<std::int64_t>::min)();
	return FFmpegMoviePlaybackTestAccess::admitAudioFrame(playback, metadata)
		&& FFmpegMoviePlaybackTestAccess::admitAudioFrame(playback, metadata);
}

static bool testExplicitEndModesAndLoop(const char *audioPath)
{
    PlaybackRun once;
    FFmpegMoviePlaybackOptions onceOptions;
    onceOptions.mode = FFmpegMoviePlaybackMode::ONCE;
    if (!runToEnd(audioPath, once, onceOptions) || once.generation != 1 || once.hasCurrentFrame) {
        return false;
    }

    PlaybackRun showLast;
    FFmpegMoviePlaybackOptions showOptions;
    showOptions.mode = FFmpegMoviePlaybackMode::SHOW_LAST_FRAME;
    if (!runToEnd(audioPath, showLast, showOptions) || showLast.generation != 1
        || !showLast.hasCurrentFrame || showLast.video.frames.size() != once.video.frames.size()) {
        return false;
    }

    MemoryTestFile input(audioPath);
    FFmpegFile file;
    if (!openFile(audioPath, input, file)) {
        return false;
    }
    RecordingSink sink;
    VideoTrace trace;
    FFmpegMoviePlaybackOptions loopOptions;
    loopOptions.mode = FFmpegMoviePlaybackMode::LOOP;
    FFmpegMoviePlayback playback(file, &sink, loopOptions);
    playback.setVideoCallback(captureVideo, &trace);
    for (std::size_t i = 0;
        i < 512 && (playback.generation() < 2 || trace.frames.size() < 13); ++i) {
        if (!playback.pump(32)) {
            return false;
        }
    }
    if (playback.generation() < 2 || sink.resetCalls < 2 || trace.frames.size() < 13) {
        std::fprintf(stderr, "loop did not restart: generation=%llu resets=%llu frames=%zu\n",
            static_cast<unsigned long long>(playback.generation()),
            static_cast<unsigned long long>(sink.resetCalls), trace.frames.size());
        return false;
    }
    playback.setMode(FFmpegMoviePlaybackMode::ONCE);
    for (std::size_t i = 0; i < 256 && !playback.isTerminal(); ++i) {
        playback.pump(32);
    }
    if (!playback.isTerminal() || playback.state() == FFmpegMoviePlaybackState::FAILED) {
        std::fprintf(stderr, "loop did not terminate after mode switch: state=%d generation=%llu frames=%zu\n",
            static_cast<int>(playback.state()), static_cast<unsigned long long>(playback.generation()),
            trace.frames.size());
        return false;
    }
    const std::size_t firstGenerationFrames = once.video.frames.size();
    const bool loopResult = trace.frames[firstGenerationFrames].presentationTimestamp == once.video.frames[0].presentationTimestamp
        && sink.accepted.size() > 1 && sink.accepted.back().generation >= 2;
    if (!loopResult) {
        std::fprintf(stderr,
            "loop result mismatch: first_count=%zu loop_frames=%zu first_pts=%lld loop_pts=%lld accepted=%zu last_generation=%llu\n",
            firstGenerationFrames, trace.frames.size(),
            static_cast<long long>(once.video.frames[0].presentationTimestamp),
            static_cast<long long>(trace.frames[firstGenerationFrames].presentationTimestamp), sink.accepted.size(),
            static_cast<unsigned long long>(sink.accepted.empty() ? 0 : sink.accepted.back().generation));
    }
    return loopResult;
}

static bool testLoopResetClearsFrameBeforeNewGeneration(const char *audioPath)
{
    MemoryTestFile input(audioPath);
    FFmpegFile file;
    if (!openFile(audioPath, input, file)) {
        return false;
    }
    RecordingSink sink;
    FFmpegMoviePlaybackOptions options;
    options.mode = FFmpegMoviePlaybackMode::LOOP;
    FFmpegMoviePlayback playback(file, &sink, options);
    VideoTrace trace;
    playback.setVideoCallback(captureVideo, &trace);
    for (std::size_t i = 0; i < 256 && trace.frames.size() < 12; ++i) {
        playback.pump(32);
    }
    if (trace.frames.size() != 12 || playback.generation() != 1) {
        return false;
    }
    const std::size_t eventCountBeforeLoop = sink.events.size();
    while (playback.generation() < 2) {
        if (!playback.pump(1)) {
            return false;
        }
    }
    if (playback.hasCurrentFrame() || playback.isFrameReady()) {
        return false;
    }
    const std::size_t resetEvent = [&sink, eventCountBeforeLoop]() {
        for (std::size_t i = eventCountBeforeLoop; i < sink.events.size(); ++i) {
            if (sink.events[i] == "reset:2") {
                return i;
            }
        }
        return sink.events.size();
    }();
    if (resetEvent == sink.events.size()) {
        return false;
    }
    for (std::size_t i = resetEvent + 1; i < sink.events.size(); ++i) {
        if (sink.events[i].find("submit:2:") == 0) {
            return false;
        }
    }
    for (std::size_t i = 0; i < 128 && trace.frames.size() == 12; ++i) {
        if (!playback.pump(1)) {
            break;
        }
    }
    if (!playback.hasCurrentFrame() || trace.frames.size() != 13) {
        return false;
    }
    for (std::size_t i = 0; i < 128 && std::none_of(sink.events.begin() + resetEvent + 1,
            sink.events.end(), [](const std::string &event) { return event.find("submit:2:") == 0; }); ++i) {
        if (!playback.pump(1)) {
            break;
        }
    }
    const std::size_t firstNewSubmit = [&sink, resetEvent]() {
        for (std::size_t i = resetEvent + 1; i < sink.events.size(); ++i) {
            if (sink.events[i].find("submit:2:") == 0) {
                return i;
            }
        }
        return sink.events.size();
    }();
    if (firstNewSubmit == sink.events.size() || resetEvent >= firstNewSubmit) {
        return false;
    }
    return true;
}

static bool testClockBoundaries(const char *videoPath)
{
    MemoryTestFile input(videoPath);
    FFmpegFile file;
    if (!openFile(videoPath, input, file)) {
        return false;
    }
    RecordingSink sink;
    ManualClock clock;
    FFmpegMoviePlaybackOptions options;
    options.mode = FFmpegMoviePlaybackMode::ONCE;
    options.clock = [&clock]() { return clock.now(); };
    FFmpegMoviePlayback playback(file, &sink, options);
    VideoTrace trace;
    playback.setVideoCallback(captureVideo, &trace);
	for (std::size_t step = 0; step < 128 && trace.frames.empty(); ++step) {
		if (!playback.pump(1)) {
			return false;
		}
	}
	if (trace.frames.size() != 1 || !playback.isFrameReady()) {
		return false;
	}
	clock.microseconds = 0;
	if (!playback.pump(32) || trace.frames.size() < 2 || playback.isFrameReady()) {
		return false;
	}
	clock.microseconds = 33367;
	if (!playback.isFrameReady() || playback.videoPresentationTimeUs() < 33300
		|| playback.videoPresentationTimeUs() > 33400) {
        return false;
    }
    return playback.finish(64) && playback.state() != FFmpegMoviePlaybackState::FAILED;
}

int main(int argc, char **argv)
{
	if (argc == 3 && std::strcmp(argv[1], "--media-smoke") == 0) {
		return runExistingMediaSmoke(argv[2]) ? 0 : 1;
	}
    if (argc != 3) {
        std::fputs("Expected generated audio/video and video-only fixture paths.\n", stderr);
        return 1;
    }

    struct TestCase { const char *name; bool (*run)(const char *, const char *); };
    const TestCase tests[] = {
		{ "many audio frames prime before first video", [](const char *audio, const char *) { return testManyAudioFramesPrimeBeforeFirstVideo(audio); } },
		{ "movie aggregation stays bounded and resets", [](const char *audio, const char *) { return testMovieAggregationBoundAndReset(audio); } },
		{ "movie aggregation preserves discontinuity and provenance", [](const char *audio, const char *) { return testMovieAggregationPreservesDiscontinuityAndProvenance(audio); } },
		{ "movie aggregation retains per-frame software gain", [](const char *audio, const char *) { return testMovieAggregationRetainsPerFrameSoftwareGain(audio); } },
        { "integrated audio/video and drain", [](const char *audio, const char *) { return testIntegratedAudioVideoAndDrain(audio); } },
        { "accepted audio waits for sink drain", [](const char *audio, const char *) { return testAcceptedAudioWaitsForSinkDrain(audio); } },
        { "no-callback drain fails boundedly", [](const char *audio, const char *) { return testNoCallbackDrainFailsBoundedly(audio); } },
        { "no-callback backpressure fails boundedly", [](const char *audio, const char *) { return testNoCallbackBackpressureFailsBoundedly(audio); } },
        { "terminal reset ownership", [](const char *audio, const char *) { return testTerminalResetOwnership(audio); } },
        { "callback detaches before file reuse", [](const char *audio, const char *) { return testCallbackDetachesBeforeFileReuse(audio); } },
        { "silent video fallback", [](const char *, const char *video) { return testSilentVideoFallback(video); } },
        { "disabled audio fallback", [](const char *audio, const char *) { return testDisabledAudio(audio); } },
		{ "backpressure preserves audio and video", [](const char *audio, const char *) { return testBackpressurePreservesAudioAndVideo(audio); } },
		{ "reset-pending service reopens admission", [](const char *audio, const char *) { return testResetPendingServiceReopensAdmission(audio); } },
		{ "unexpected movie drop fails boundedly", [](const char *audio, const char *) { return testUnexpectedMovieDropFailsBoundedly(audio); } },
		{ "gain and mute", [](const char *audio, const char *) { return testGainAndMute(audio); } },
		{ "native gain avoids queued PCM rewrite", [](const char *audio, const char *) { return testNativeGainAvoidsQueuedPcmRewrite(audio); } },
        { "read and seek failures do not loop", [](const char *audio, const char *) { return testReadAndSeekFailuresDoNotLoop(audio); } },
        { "truncated input fails and malformed container is rejected", [](const char *audio, const char *) { return testTruncatedInputFailsAndMalformedContainerIsRejected(audio); } },
        { "terminal sink failure", [](const char *audio, const char *) { return testTerminalSinkFailure(audio); } },
        { "monotonic clock ignores completed-audio staircase", [](const char *audio, const char *) { return testMonotonicClockIgnoresCompletedAudioStaircase(audio); } },
        { "seek and generation", [](const char *audio, const char *) { return testSeekAndGeneration(audio); } },
        { "seek uses absolute timeline", [](const char *audio, const char *) { return testSeekUsesAbsoluteTimeline(audio); } },
        { "seek admits timestamp-less audio", [](const char *audio, const char *) { return testSeekAdmitsTimestampLessAudio(audio); } },
        { "explicit end modes and loop", [](const char *audio, const char *) { return testExplicitEndModesAndLoop(audio); } },
        { "loop reset clears frame before new generation", [](const char *audio, const char *) { return testLoopResetClearsFrameBeforeNewGeneration(audio); } },
        { "clock boundaries", [](const char *, const char *video) { return testClockBoundaries(video); } },
    };
    int failures = 0;
    for (const TestCase &test : tests) {
        std::fprintf(stderr, "BEGIN %s\n", test.name);
        if (!test.run(argv[1], argv[2])) {
            std::fprintf(stderr, "%s failed.\n", test.name);
            ++failures;
        }
        std::fprintf(stderr, "END %s\n", test.name);
    }
    if (failures != 0) {
        std::fprintf(stderr, "%d FFmpeg movie playback checks failed.\n", failures);
        return 1;
    }
    std::puts("FFmpeg movie playback integration contract passed.");
    return 0;
}
