#include "VideoDevice/FFmpeg/FFmpegMoviePlayback.h"

#include "VideoDevice/FFmpeg/FFmpegAudioDecoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/mathematics.h>
}

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

namespace
{
constexpr std::int64_t INVALID_TIMESTAMP = (std::numeric_limits<std::int64_t>::min)();
constexpr std::int64_t MICROSECONDS_PER_SECOND = 1000000;
constexpr std::int64_t DRAIN_NO_PROGRESS_TIMEOUT_US = 10 * MICROSECONDS_PER_SECOND;

bool validTimeBase(const FFmpegFrameMetadata &metadata)
{
	return metadata.timeBaseNumerator > 0 && metadata.timeBaseDenominator > 0;
}

std::int16_t scaleSample(std::int16_t value, double gain)
{
	if (gain <= 0.0) {
		return 0;
	}
	const double scaled = static_cast<double>(value) * gain;
	if (scaled >= static_cast<double>((std::numeric_limits<std::int16_t>::max)())) {
		return (std::numeric_limits<std::int16_t>::max)();
	}
	if (scaled <= static_cast<double>((std::numeric_limits<std::int16_t>::min)())) {
		return (std::numeric_limits<std::int16_t>::min)();
	}
	return static_cast<std::int16_t>(std::lrint(scaled));
}
}

class FFmpegMoviePlayback::SilentSink final : public AudioPcmSink
{
public:
	AudioPcmSubmitResult submit(AudioPcmChunk &&chunk) override
	{
		m_generation = chunk.generation;
		m_sequence = chunk.sequence;
		m_playedSample = chunk.startSample + static_cast<std::int64_t>(chunk.frameCount);
		return AudioPcmSubmitResult::ACCEPTED;
	}

	void reset(std::uint64_t generation) override
	{
		m_generation = generation;
		m_sequence = 0;
		m_playedSample = 0;
	}

	bool getPlayedSample(std::int64_t &sample) const noexcept override
	{
		sample = m_playedSample;
		return false;
	}

private:
	std::uint64_t m_generation = 0;
	std::uint64_t m_sequence = 0;
	std::int64_t m_playedSample = 0;
};

class FFmpegMoviePlayback::GainSink final : public AudioPcmSink
{
public:
	GainSink(AudioPcmSink &sink, double gain) : m_sink(sink), m_gain(1.0) { setGain(gain); }
	void setGain(double gain)
	{
		gain = std::max(0.0, gain);
		m_gain = m_sink.setOutputGain(gain) ? 1.0 : gain;
	}

	bool canAccept(std::size_t submissions) const noexcept override
	{
		return m_sink.canAccept(submissions);
	}

	AudioPcmSubmitResult submit(AudioPcmChunk &&chunk) override
	{
		if (chunk.frameCount == 0 || chunk.frameCount > FFmpegAudioDecoder::MAX_CHUNK_FRAMES
			|| chunk.channels != FFmpegAudioDecoder::OUTPUT_CHANNELS
			|| chunk.sampleRate != FFmpegAudioDecoder::OUTPUT_SAMPLE_RATE
			|| chunk.format != AudioPcmFormat::SIGNED_16_INTERLEAVED_LITTLE_ENDIAN
			|| chunk.data.size() != static_cast<std::size_t>(chunk.frameCount)
				* chunk.channels * sizeof(std::int16_t)) {
			return AudioPcmSubmitResult::FAILED;
		}
		for (std::size_t offset = 0; offset < chunk.data.size(); offset += sizeof(std::int16_t)) {
			std::int16_t value = 0;
			std::memcpy(&value, chunk.data.data() + offset, sizeof(value));
			value = scaleSample(value, m_gain);
			std::memcpy(chunk.data.data() + offset, &value, sizeof(value));
		}
		// A single BIK packet can decode many small audio frames before its
		// first video frame. Keep contiguous PCM together so opening a movie
		// does not need physical playback to free one native slot per AVFrame.
		if (m_pending.frameCount != 0 && !canAppend(chunk) && !flush()) {
			return AudioPcmSubmitResult::FAILED;
		}
		m_lastInputSequence = chunk.sequence;
		if (m_pending.frameCount == 0) {
			m_pending = std::move(chunk);
		} else {
			m_pending.data.insert(m_pending.data.end(), chunk.data.begin(), chunk.data.end());
			m_pending.frameCount += chunk.frameCount;
		}
		// Even a full chunk stays pending until the next input or video/EOF
		// boundary. Each convert() submission can therefore flush at most one
		// native buffer, preserving pump()'s two-slot admission reservation.
		return AudioPcmSubmitResult::ACCEPTED;
	}

	bool flush()
	{
		if (m_pending.frameCount == 0) {
			return true;
		}
		m_pending.sequence = m_outputSequence;
		const AudioPcmSubmitResult result = m_sink.submit(std::move(m_pending));
		m_pending = {};
		// Movie input has already been consumed when it reaches the sink.  A
		// defensive drop must fail the generation instead of creating an audio
		// hole while video continues on its monotonic clock.
		if (result != AudioPcmSubmitResult::ACCEPTED) {
			return false;
		}
		++m_outputSequence;
		return true;
	}

	void reset(std::uint64_t generation) override
	{
		m_pending = {};
		m_outputSequence = 0;
		m_lastInputSequence = 0;
		m_sink.reset(generation);
	}

	void endOfStream() noexcept override
	{
		m_sink.endOfStream();
	}

	bool isDrained() const noexcept override
	{
		return m_pending.frameCount == 0 && m_sink.isDrained();
	}

	bool serviceSink() noexcept override
	{
		return m_sink.serviceSink();
	}

	void close() noexcept override
	{
		m_pending = {};
		m_sink.close();
	}

	bool getPlayedSample(std::int64_t &sample) const noexcept override
	{
		return m_sink.getPlayedSample(sample);
	}

private:
	bool canAppend(const AudioPcmChunk &chunk) const
	{
		return !chunk.discontinuity && m_pending.generation == chunk.generation
			&& m_lastInputSequence != (std::numeric_limits<std::uint64_t>::max)()
			&& chunk.sequence == m_lastInputSequence + 1
			&& m_pending.sampleRate == chunk.sampleRate && m_pending.channels == chunk.channels
			&& m_pending.sourceChannels == chunk.sourceChannels && m_pending.format == chunk.format
			&& m_pending.startSample <= (std::numeric_limits<std::int64_t>::max)() - m_pending.frameCount
			&& chunk.startSample == m_pending.startSample + m_pending.frameCount
			&& chunk.frameCount <= FFmpegAudioDecoder::MAX_CHUNK_FRAMES - m_pending.frameCount;
	}

	AudioPcmSink &m_sink;
	double m_gain;
	AudioPcmChunk m_pending;
	std::uint64_t m_outputSequence = 0;
	std::uint64_t m_lastInputSequence = 0;
};

FFmpegMoviePlayback::FFmpegMoviePlayback(FFmpegFile &file, AudioPcmSink *sink,
	const FFmpegMoviePlaybackOptions &options) :
	m_file(file),
	m_externalSink(sink),
	m_silentSink(new SilentSink()),
	m_gainSink(nullptr),
	m_audioSink(nullptr),
	m_audioDecoder(new FFmpegAudioDecoder()),
	m_audioEnabled(options.audioEnabled && file.hasAudio() && sink != nullptr),
	m_mode(options.mode),
	m_state(FFmpegMoviePlaybackState::ACTIVE),
	m_clock(options.clock ? options.clock : &FFmpegMoviePlayback::defaultClock),
	m_videoCallback(),
	m_videoUserData(nullptr),
	m_currentFrame(nullptr),
	m_currentMetadata(),
	m_generation(1),
	m_drainCount(0),
	m_resetCount(0),
	m_clockBaseUs(0),
	m_audioBaseSample(0),
	m_videoPresentationTimeUs(0),
	m_videoTimelineOriginUs(file.getVideoStartTimeMicroseconds()),
	m_seekTargetUs(0),
	m_videoTimelineBaseUs(0),
	m_drainWatchdogStartUs(0),
	m_drainWatchdogProgressSample(0),
	m_clockRebased(false),
	m_audioClockRebased(false),
	m_videoGateActive(false),
	m_audioGateActive(false),
	m_newVideoFrame(false),
	m_drainWatchdogActive(false),
	m_drainWatchdogHasProgress(false)
{
	const bool useExternalAudio = m_audioEnabled;
	m_audioSink = useExternalAudio ? sink : m_silentSink;
	m_gainSink = new GainSink(*m_audioSink, options.gain);
	m_audioSink = m_gainSink;
	m_file.setFrameCallback(&FFmpegMoviePlayback::onFrame);
	m_file.setUserData(this);
	m_state = (useExternalAudio ? FFmpegMoviePlaybackState::ACTIVE : FFmpegMoviePlaybackState::SILENT_AUDIO);
	resetGeneration(m_generation);
}

FFmpegMoviePlayback::~FFmpegMoviePlayback()
{
	// The file owns the callback storage. Detach it before this object becomes
	// invalid so a later file seek/decode cannot call back into freed playback.
	m_file.setFrameCallback(nullptr);
	m_file.setUserData(nullptr);
	// Abort teardown resets the active generation before the sink closes. A
	// normally completed stream has already drained and must close without a
	// reset; seek/loop/failure paths perform their own generation reset.
	if (m_audioSink != nullptr
		&& m_state != FFmpegMoviePlaybackState::ENDED
		&& m_state != FFmpegMoviePlaybackState::FAILED) {
		m_audioSink->reset(m_generation);
	}
	delete m_audioDecoder;
	av_frame_free(&m_currentFrame);
	delete m_gainSink;
	delete m_silentSink;
}

void FFmpegMoviePlayback::clearCurrentFrame()
{
	av_frame_free(&m_currentFrame);
	m_currentMetadata = {};
	m_videoPresentationTimeUs = 0;
	m_newVideoFrame = false;
}

void FFmpegMoviePlayback::setVideoCallback(FFmpegMovieVideoCallback callback, void *userData)
{
	m_videoCallback = std::move(callback);
	m_videoUserData = userData;
}

void FFmpegMoviePlayback::setMode(FFmpegMoviePlaybackMode mode)
{
	m_mode = mode;
	if (m_state == FFmpegMoviePlaybackState::ENDED && mode != FFmpegMoviePlaybackMode::LOOP) {
		return;
	}
	if (m_state == FFmpegMoviePlaybackState::ENDED && mode == FFmpegMoviePlaybackMode::LOOP) {
		m_state = FFmpegMoviePlaybackState::ACTIVE;
	}
}

void FFmpegMoviePlayback::setGain(double gain)
{
	if (m_gainSink != nullptr) {
		m_gainSink->setGain(gain);
	}
}

bool FFmpegMoviePlayback::isTerminal() const
{
	return m_state == FFmpegMoviePlaybackState::ENDED || m_state == FFmpegMoviePlaybackState::FAILED;
}

void FFmpegMoviePlayback::failPlayback()
{
	setFailed();
}

bool FFmpegMoviePlayback::isFrameReady() const
{
	if (m_currentFrame == nullptr) {
		return false;
	}
	// The native sink publishes completed buffers rather than a continuously
	// advancing in-buffer cursor. A BIK audio packet can cover roughly three
	// quarters of a second, so using that staircase as the movie master clock
	// freezes video between completions. Keep presentation on the generation-
	// rebased monotonic clock; bounded admission prevents capacity holes.
	const std::int64_t elapsedUs = nowMicroseconds() - m_clockBaseUs;
	return m_videoPresentationTimeUs <= std::max<std::int64_t>(0, elapsedUs);
}

bool FFmpegMoviePlayback::pump(std::size_t maxDecodeSteps)
{
	if (maxDecodeSteps == 0 || isTerminal()) {
		return false;
	}
	if (m_state == FFmpegMoviePlaybackState::DRAINING) {
		if (m_audioSink == nullptr || !m_audioSink->serviceSink()) {
			return setFailed();
		}
		return completeDrain();
	}
	bool progressed = false;
	for (std::size_t step = 0; step < maxDecodeSteps; ++step) {
		// One decoded audio frame can submit both a delayed resampler tail and
		// its current PCM. Reserve both slots before consuming the next bounded
		// demux/decode transition. Backpressure yields to the owner without
		// advancing either audio or video.
		if (m_audioSink == nullptr) {
			return setFailed() || progressed;
		}
		if (!m_audioSink->canAccept(2)) {
			if (!m_audioSink->serviceSink()) {
				return setFailed() || progressed;
			}
			// Service may reclaim a completed slot immediately.  Recheck before
			// declaring a stall so ordinary short-lived backpressure remains
			// lossless and does not consume the no-progress budget.
			if (m_audioSink->canAccept(2)) {
				clearDrainWatchdog();
				// The service/reset transition itself is forward progress.  With
				// pump(1), returning false here makes stream owners mistake a
				// reopened native voice for a playback failure before the next
				// bounded decode step can run.
				progressed = true;
				continue;
			}
			if (!m_drainWatchdogActive) {
				beginDrainWatchdog();
			} else if (drainWatchdogExpired()) {
				return setFailed() || progressed;
			}
			return true;
		}
		clearDrainWatchdog();
		m_newVideoFrame = false;
		switch (m_file.decodeStep()) {
			case FFmpegDecodeStepResult::PROGRESSED:
				progressed = true;
				continue;
			case FFmpegDecodeStepResult::FRAME_READY:
				progressed = true;
				if (m_state == FFmpegMoviePlaybackState::FAILED) {
					return progressed;
				}
				if (m_newVideoFrame) {
					return true;
				}
				continue;
			case FFmpegDecodeStepResult::FAILED:
				return setFailed() || progressed;
			case FFmpegDecodeStepResult::END_OF_INPUT:
				progressed = handleEndOfInput() || progressed;
				return progressed;
		}
	}
	return progressed;
}

bool FFmpegMoviePlayback::finish(std::size_t maxPumpCalls)
{
	for (std::size_t call = 0; call < maxPumpCalls && !isTerminal(); ++call) {
		if (!pump(64) && m_state != FFmpegMoviePlaybackState::DRAINING) {
			break;
		}
	}
	return m_state == FFmpegMoviePlaybackState::ENDED;
}

bool FFmpegMoviePlayback::seekFrame(Int frameIndex)
{
	if (m_state == FFmpegMoviePlaybackState::FAILED) {
		return false;
	}
	++m_generation;
	clearCurrentFrame();
	if (!resetGeneration(m_generation)) {
		return setFailed(false);
	}
	if (frameIndex < 0) {
		return setFailed(false);
	}
	if (!m_file.seekFrame(frameIndex)) {
		return setFailed(false);
	}
	m_seekTargetUs = targetTimeForFrame(frameIndex);
	m_audioGateActive = frameIndex > 0;
	m_videoGateActive = frameIndex > 0;
	m_clockRebased = false;
	m_audioClockRebased = false;
	m_newVideoFrame = false;
	m_state = (m_audioEnabled
		? FFmpegMoviePlaybackState::ACTIVE : FFmpegMoviePlaybackState::SILENT_AUDIO);
	return true;
}

void FFmpegMoviePlayback::onFrame(const AVFrame *frame, const FFmpegFrameMetadata &metadata, void *userData)
{
	FFmpegMoviePlayback *playback = static_cast<FFmpegMoviePlayback *>(userData);
	if (playback != nullptr) {
		playback->handleFrame(frame, metadata);
	}
}

void FFmpegMoviePlayback::handleFrame(const AVFrame *frame, const FFmpegFrameMetadata &metadata)
{
	if (frame == nullptr || m_state == FFmpegMoviePlaybackState::FAILED) {
		return;
	}
	if (metadata.streamType == AVMEDIA_TYPE_AUDIO) {
		if (!isAudioFrameAdmitted(metadata)) {
			return;
		}
		if (!m_audioDecoder->convert(frame, metadata.timeBaseNumerator, metadata.timeBaseDenominator, *m_audioSink)) {
			setFailed();
		}
		return;
	}
	if (metadata.streamType != AVMEDIA_TYPE_VIDEO) {
		return;
	}
	const std::int64_t timestampUs = timestampToMicroseconds(metadata);
	if (m_videoGateActive) {
		if (timestampUs == INVALID_TIMESTAMP || timestampUs < m_seekTargetUs) {
			return;
		}
		m_videoGateActive = false;
	}
	// Publish the bounded PCM prefix before the associated video callback.
	// Owner-side service can then start both at the same presentation boundary.
	if (!m_gainSink->flush()) {
		setFailed();
		return;
	}
	AVFrame *clone = av_frame_clone(frame);
	if (clone == nullptr) {
		setFailed();
		return;
	}
	av_frame_free(&m_currentFrame);
	m_currentFrame = clone;
	m_currentMetadata = metadata;
	if (!m_clockRebased) {
		rebaseClocks(timestampUs == INVALID_TIMESTAMP ? 0 : timestampUs);
	}
	m_videoPresentationTimeUs = timestampUs == INVALID_TIMESTAMP ? 0
		: std::max<std::int64_t>(0, timestampUs - m_videoTimelineBaseUs);
	m_newVideoFrame = true;
	if (m_videoCallback) {
		m_videoCallback(m_currentFrame, m_currentMetadata, m_videoUserData);
	}
}

bool FFmpegMoviePlayback::handleEndOfInput()
{
	if (m_state == FFmpegMoviePlaybackState::DRAINING || m_state == FFmpegMoviePlaybackState::FAILED) {
		return false;
	}
	m_state = FFmpegMoviePlaybackState::DRAINING;
	++m_drainCount;
	beginDrainWatchdog();
	if (!m_audioDecoder->drain(*m_audioSink) || !m_gainSink->flush()) {
		return setFailed();
	}
	m_audioSink->endOfStream();
	return completeDrain();
}

bool FFmpegMoviePlayback::completeDrain()
{
	if (m_state != FFmpegMoviePlaybackState::DRAINING) {
		return false;
	}
	if (m_audioSink == nullptr) {
		return setFailed(false);
	}
	if (!m_audioSink->isDrained()) {
		if (drainWatchdogExpired()) {
			return setFailed();
		}
		return true;
	}
	clearDrainWatchdog();
	if (m_mode != FFmpegMoviePlaybackMode::LOOP) {
		if (m_mode == FFmpegMoviePlaybackMode::ONCE) {
			clearCurrentFrame();
		}
		m_state = FFmpegMoviePlaybackState::ENDED;
		return true;
	}
	++m_generation;
	clearCurrentFrame();
	if (!resetGeneration(m_generation)) {
		return setFailed(false);
	}
	if (!m_file.seekFrame(0)) {
		return setFailed(false);
	}
	m_seekTargetUs = m_videoTimelineOriginUs;
	m_videoGateActive = false;
	m_audioGateActive = false;
	m_clockRebased = false;
	m_audioClockRebased = false;
	m_state = (m_audioEnabled
		? FFmpegMoviePlaybackState::ACTIVE : FFmpegMoviePlaybackState::SILENT_AUDIO);
	return true;
}

bool FFmpegMoviePlayback::resetGeneration(std::uint64_t generation)
{
	if (m_audioDecoder == nullptr || m_audioSink == nullptr) {
		return false;
	}
	m_audioDecoder->reset(generation, *m_audioSink);
	++m_resetCount;
	m_audioBaseSample = 0;
	return true;
}

bool FFmpegMoviePlayback::setFailed(bool resetAudio)
{
	if (m_state == FFmpegMoviePlaybackState::FAILED) {
		return false;
	}
	clearDrainWatchdog();
	if (resetAudio) {
		resetGeneration(m_generation);
	}
	clearCurrentFrame();
	m_state = FFmpegMoviePlaybackState::FAILED;
	return false;
}

void FFmpegMoviePlayback::beginDrainWatchdog()
{
	m_drainWatchdogStartUs = nowMicroseconds();
	m_drainWatchdogProgressSample = 0;
	m_drainWatchdogActive = true;
	m_drainWatchdogHasProgress = false;
	if (m_audioSink != nullptr) {
		std::int64_t playedSample = 0;
		if (m_audioSink->getPlayedSample(playedSample) && playedSample >= 0) {
			m_drainWatchdogProgressSample = playedSample;
			m_drainWatchdogHasProgress = true;
		}
	}
}

void FFmpegMoviePlayback::clearDrainWatchdog()
{
	m_drainWatchdogActive = false;
	m_drainWatchdogHasProgress = false;
	m_drainWatchdogStartUs = 0;
	m_drainWatchdogProgressSample = 0;
}

bool FFmpegMoviePlayback::drainWatchdogExpired()
{
	if (!m_drainWatchdogActive) {
		beginDrainWatchdog();
	}

	const std::int64_t now = nowMicroseconds();
	if (m_audioSink != nullptr) {
		std::int64_t playedSample = 0;
		if (m_audioSink->getPlayedSample(playedSample) && playedSample >= 0
			&& (!m_drainWatchdogHasProgress || playedSample > m_drainWatchdogProgressSample)) {
			m_drainWatchdogProgressSample = playedSample;
			m_drainWatchdogHasProgress = true;
			m_drainWatchdogStartUs = now;
			return false;
		}
	}

	return now >= m_drainWatchdogStartUs
		&& now - m_drainWatchdogStartUs >= DRAIN_NO_PROGRESS_TIMEOUT_US;
}

bool FFmpegMoviePlayback::isAudioFrameAdmitted(const FFmpegFrameMetadata &metadata)
{
	if (!m_audioGateActive) {
		return true;
	}
	const std::int64_t timestampUs = timestampToMicroseconds(metadata);
	if (timestampUs == INVALID_TIMESTAMP) {
		// The file seek already established the new decode generation. Streams
		// without timestamps cannot be compared with the video target, so admit
		// their first post-seek frame instead of leaving audio gated forever.
		m_audioGateActive = false;
		return true;
	}
	if (timestampUs < m_seekTargetUs) {
		return false;
	}
	m_audioGateActive = false;
	return true;
}

std::int64_t FFmpegMoviePlayback::timestampToMicroseconds(const FFmpegFrameMetadata &metadata) const
{
	if (metadata.presentationTimestamp == INVALID_TIMESTAMP || !validTimeBase(metadata)) {
		return INVALID_TIMESTAMP;
	}
	return av_rescale_q(metadata.presentationTimestamp,
		AVRational { metadata.timeBaseNumerator, metadata.timeBaseDenominator },
		AVRational { 1, MICROSECONDS_PER_SECOND });
}

std::int64_t FFmpegMoviePlayback::targetTimeForFrame(Int frameIndex) const
{
	const FFmpegFrameRate rate = m_file.getVideoFrameRate();
	if (frameIndex <= 0 || rate.numerator <= 0 || rate.denominator <= 0) {
		return m_videoTimelineOriginUs;
	}
	const std::int64_t relativeUs = av_rescale_q(frameIndex,
		AVRational { rate.denominator, rate.numerator }, AVRational { 1, MICROSECONDS_PER_SECOND });
	if ((relativeUs > 0 && m_videoTimelineOriginUs > (std::numeric_limits<std::int64_t>::max)() - relativeUs)
		|| (relativeUs < 0 && m_videoTimelineOriginUs < (std::numeric_limits<std::int64_t>::min)() - relativeUs)) {
		return relativeUs > 0 ? (std::numeric_limits<std::int64_t>::max)()
			: (std::numeric_limits<std::int64_t>::min)();
	}
	return m_videoTimelineOriginUs + relativeUs;
}

std::int64_t FFmpegMoviePlayback::nowMicroseconds() const
{
	return m_clock ? m_clock() : defaultClock();
}

void FFmpegMoviePlayback::rebaseClocks(std::int64_t timelineBase)
{
	m_videoTimelineBaseUs = timelineBase;
	m_clockBaseUs = nowMicroseconds();
	m_audioBaseSample = 0;
	m_clockRebased = true;
	m_audioClockRebased = true;
}

std::int64_t FFmpegMoviePlayback::defaultClock()
{
	return std::chrono::duration_cast<std::chrono::microseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
}
