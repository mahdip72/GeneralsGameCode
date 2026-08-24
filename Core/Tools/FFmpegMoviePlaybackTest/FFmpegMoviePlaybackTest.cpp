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
            if (corruptByteOffset >= 0) {
                for (std::size_t i = 0; i < count; ++i) {
                    if (static_cast<Int64>(m_position - count + i) == corruptByteOffset) {
                        static_cast<std::uint8_t *>(buffer)[i] = corruptByteValue;
                    }
                }
            }
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
    Int m_readCount = 0;
    Int failAfterReadCount = -1;
    Int64 truncateAfterBytes = -1;
    Int64 corruptByteOffset = -1;
    std::uint8_t corruptByteValue = 0;

private:
    std::vector<char> m_data;
    std::size_t m_position = 0;
    bool m_closed = false;
};

class RecordingSink : public AudioPcmSink
{
public:
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
		return true;
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
    std::int64_t playedSample = -1;
	std::size_t submitCalls = 0;
	std::size_t resetCalls = 0;
	std::size_t endOfStreamCalls = 0;
	std::size_t serviceCalls = 0;
    bool dropNext = false;
    bool dropAll = false;
    bool failNext = false;
	bool rejectStale = true;
	bool clockEnabled = false;
	bool holdDrain = false;
	bool drainReleased = false;
};

class ManualClock
{
public:
    std::int64_t now() const { return microseconds; }
    std::int64_t microseconds = 0;
};

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
	return acceptedFrames > 19200 && sink.serviceCalls >= 4;
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

static bool testDropsAndBoundedCompletion(const char *audioPath)
{
    PlaybackRun oneDrop;
    oneDrop.sink.dropNext = true;
    FFmpegMoviePlaybackOptions options;
    options.mode = FFmpegMoviePlaybackMode::ONCE;
    if (!runToEnd(audioPath, oneDrop, options) || oneDrop.pumpCalls >= 256
        || oneDrop.sink.dropped.empty() || oneDrop.sink.accepted.empty()) {
        return false;
    }
    bool hasDiscontinuity = false;
    for (const AudioPcmChunk &chunk : oneDrop.sink.accepted) {
        hasDiscontinuity = hasDiscontinuity || chunk.discontinuity;
    }
    if (!hasDiscontinuity || oneDrop.sink.dropped.size() != 1
        || oneDrop.sink.submitCalls != oneDrop.sink.dropped.size() + oneDrop.sink.accepted.size()) {
        return false;
    }
    const std::uint64_t droppedSequence = oneDrop.sink.dropped.front().sequence;
    bool continuation = false;
    for (const AudioPcmChunk &chunk : oneDrop.sink.accepted) {
        continuation = continuation || (chunk.sequence == droppedSequence + 1 && chunk.discontinuity);
    }
    if (!continuation) {
        return false;
    }

    PlaybackRun permanent;
    permanent.sink.dropAll = true;
    if (!runToEnd(audioPath, permanent, options) || permanent.pumpCalls >= 256
        || permanent.sink.accepted.size() != 0 || permanent.sink.dropped.empty()
        || permanent.sink.submitCalls != permanent.sink.dropped.size()) {
        return false;
    }
	return true;
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

static bool testTruncatedAndCorruptInputsFail(const char *audioPath)
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

    MemoryTestFile corruptInput(audioPath);
    corruptInput.corruptByteOffset = 1024;
    corruptInput.corruptByteValue = 0;
    FFmpegFile corruptFile;
    if (!openFile(audioPath, corruptInput, corruptFile)) {
        return false;
    }
    RecordingSink corruptSink;
    FFmpegMoviePlayback corruptPlayback(corruptFile, &corruptSink, options);
    for (std::size_t i = 0; i < 128 && !corruptPlayback.isTerminal(); ++i) {
        corruptPlayback.pump(32);
    }
    const bool corruptFailed = corruptPlayback.state() == FFmpegMoviePlaybackState::FAILED
        && corruptPlayback.generation() == 1 && corruptSink.resetCalls >= 2;
    return corruptFailed && avLogCapture.messageCount() > 0;
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

static bool testAudioMasterClock(const char *audioPath)
{
    MemoryTestFile input(audioPath);
    FFmpegFile file;
    if (!openFile(audioPath, input, file)) {
        return false;
    }
    RecordingSink sink;
    sink.clockEnabled = true;
    sink.playedSample = 0;
    FFmpegMoviePlaybackOptions options;
    options.mode = FFmpegMoviePlaybackMode::ONCE;
    FFmpegMoviePlayback playback(file, &sink, options);
    VideoTrace trace;
    playback.setVideoCallback(captureVideo, &trace);
    if (!playback.pump(1) || trace.frames.size() != 1 || !playback.isFrameReady()) {
        return false;
    }
    if (!playback.pump(64) || trace.frames.size() < 2 || playback.isFrameReady()) {
        return false;
    }
    sink.playedSample = 2000;
    return playback.isFrameReady();
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
    for (std::size_t i = 0; i < 128 && !playback.isTerminal(); ++i) {
        playback.pump(16);
    }
    if (playback.state() == FFmpegMoviePlaybackState::FAILED || !playback.isTerminal()
        || trace.frames.size() <= frameCountBeforeSeek || sink.rejected.size() != 0) {
        return false;
    }
    for (std::size_t i = frameCountBeforeSeek; i < trace.frames.size(); ++i) {
        const FFmpegFrameRate rate = file.getVideoFrameRate();
        const std::int64_t targetUs = av_rescale_q(6,
            AVRational { rate.denominator, rate.numerator }, AVRational { 1, 1000000 });
        const std::int64_t timestampUs = av_rescale_q(trace.frames[i].presentationTimestamp,
            AVRational { trace.frames[i].timeBaseNumerator, trace.frames[i].timeBaseDenominator },
            AVRational { 1, 1000000 });
        if (timestampUs < targetUs) {
            return false;
        }
    }
    return playback.drainCount() == 1;
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
    for (std::size_t i = 0; i < 512 && playback.generation() < 2; ++i) {
        if (!playback.pump(32)) {
            return false;
        }
    }
    if (playback.generation() < 2 || sink.resetCalls < 2 || trace.frames.size() < 13) {
        return false;
    }
    playback.setMode(FFmpegMoviePlaybackMode::ONCE);
    for (std::size_t i = 0; i < 256 && !playback.isTerminal(); ++i) {
        playback.pump(32);
    }
    if (!playback.isTerminal() || playback.state() == FFmpegMoviePlaybackState::FAILED) {
        return false;
    }
    const std::size_t firstGenerationFrames = once.video.frames.size();
    const bool loopResult = trace.frames[firstGenerationFrames].presentationTimestamp == once.video.frames[0].presentationTimestamp
        && sink.accepted.size() > 1 && sink.accepted.back().generation >= 2;
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
	if (!playback.pump(1) || trace.frames.empty() || !playback.isFrameReady()) {
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
    if (argc != 3) {
        std::fputs("Expected generated audio/video and video-only fixture paths.\n", stderr);
        return 1;
    }

    struct TestCase { const char *name; bool (*run)(const char *, const char *); };
    const TestCase tests[] = {
        { "integrated audio/video and drain", [](const char *audio, const char *) { return testIntegratedAudioVideoAndDrain(audio); } },
        { "accepted audio waits for sink drain", [](const char *audio, const char *) { return testAcceptedAudioWaitsForSinkDrain(audio); } },
        { "terminal reset ownership", [](const char *audio, const char *) { return testTerminalResetOwnership(audio); } },
        { "callback detaches before file reuse", [](const char *audio, const char *) { return testCallbackDetachesBeforeFileReuse(audio); } },
        { "silent video fallback", [](const char *, const char *video) { return testSilentVideoFallback(video); } },
        { "disabled audio fallback", [](const char *audio, const char *) { return testDisabledAudio(audio); } },
        { "drops and bounded completion", [](const char *audio, const char *) { return testDropsAndBoundedCompletion(audio); } },
        { "gain and mute", [](const char *audio, const char *) { return testGainAndMute(audio); } },
        { "read and seek failures do not loop", [](const char *audio, const char *) { return testReadAndSeekFailuresDoNotLoop(audio); } },
        { "truncated and corrupt inputs fail", [](const char *audio, const char *) { return testTruncatedAndCorruptInputsFail(audio); } },
        { "terminal sink failure", [](const char *audio, const char *) { return testTerminalSinkFailure(audio); } },
        { "audio master clock", [](const char *audio, const char *) { return testAudioMasterClock(audio); } },
        { "seek and generation", [](const char *audio, const char *) { return testSeekAndGeneration(audio); } },
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
