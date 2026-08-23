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
	GainSink(AudioPcmSink &sink, double gain) : m_sink(sink), m_gain(std::max(0.0, gain)) {}
	void setGain(double gain) { m_gain = std::max(0.0, gain); }

	AudioPcmSubmitResult submit(AudioPcmChunk &&chunk) override
	{
		if (chunk.data.size() % sizeof(std::int16_t) != 0) {
			return AudioPcmSubmitResult::DROPPED;
		}
		for (std::size_t offset = 0; offset < chunk.data.size(); offset += sizeof(std::int16_t)) {
			std::int16_t value = 0;
			std::memcpy(&value, chunk.data.data() + offset, sizeof(value));
			value = scaleSample(value, m_gain);
			std::memcpy(chunk.data.data() + offset, &value, sizeof(value));
		}
		return m_sink.submit(std::move(chunk));
	}

	void reset(std::uint64_t generation) override
	{
		m_sink.reset(generation);
	}

	bool getPlayedSample(std::int64_t &sample) const noexcept override
	{
		return m_sink.getPlayedSample(sample);
	}

private:
	AudioPcmSink &m_sink;
	double m_gain;
};

FFmpegMoviePlayback::FFmpegMoviePlayback(FFmpegFile &file, AudioPcmSink *sink,
	const FFmpegMoviePlaybackOptions &options) :
	m_file(file),
	m_externalSink(sink),
	m_silentSink(new SilentSink()),
	m_gainSink(nullptr),
	m_audioSink(nullptr),
	m_audioDecoder(new FFmpegAudioDecoder()),
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
	m_seekTargetUs(0),
	m_clockRebased(false),
	m_audioClockRebased(false),
	m_videoGateActive(false),
	m_audioGateActive(false),
	m_newVideoFrame(false)
{
	const bool useExternalAudio = options.audioEnabled && file.hasAudio() && sink != nullptr;
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
	// The sink is reset before decoder/file state is released. The x64 adapter uses
	// this owner-side point to close its typed movie voice first.
	if (m_audioSink != nullptr) {
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

bool FFmpegMoviePlayback::isFrameReady() const
{
	if (m_currentFrame == nullptr) {
		return false;
	}
	std::int64_t playedSample = 0;
	if (m_audioSink != nullptr && m_audioSink->getPlayedSample(playedSample) && playedSample >= m_audioBaseSample) {
		const std::int64_t elapsedUs = av_rescale_q(playedSample - m_audioBaseSample,
			AVRational { 1, FFmpegAudioDecoder::OUTPUT_SAMPLE_RATE }, AVRational { 1, MICROSECONDS_PER_SECOND });
		return m_videoPresentationTimeUs <= elapsedUs;
	}
	const std::int64_t elapsedUs = nowMicroseconds() - m_clockBaseUs;
	return m_videoPresentationTimeUs <= std::max<std::int64_t>(0, elapsedUs);
}

bool FFmpegMoviePlayback::pump(std::size_t maxDecodeCalls)
{
	if (maxDecodeCalls == 0 || isTerminal()) {
		return false;
	}
	bool progressed = false;
	for (std::size_t call = 0; call < maxDecodeCalls; ++call) {
		m_newVideoFrame = false;
		if (m_file.decodePacket()) {
			progressed = true;
			if (m_state == FFmpegMoviePlaybackState::FAILED) {
				return progressed;
			}
			if (m_newVideoFrame) {
				return true;
			}
			continue;
		}
		if (m_file.hasError()) {
			return setFailed() || progressed;
		}
		if (!m_file.isAtEnd()) {
			return setFailed() || progressed;
		}
		progressed = handleEndOfInput() || progressed;
		if (isTerminal()) {
			return progressed;
		}
	}
	return progressed;
}

bool FFmpegMoviePlayback::finish(std::size_t maxPumpCalls)
{
	for (std::size_t call = 0; call < maxPumpCalls && !isTerminal(); ++call) {
		pump(64);
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
	m_state = (m_externalSink != nullptr && m_file.hasAudio()
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
	m_videoPresentationTimeUs = timestampUs == INVALID_TIMESTAMP ? 0 : std::max<std::int64_t>(0, timestampUs - m_seekTargetUs);
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
	if (!m_audioDecoder->drain(*m_audioSink)) {
		return setFailed();
	}
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
	m_seekTargetUs = 0;
	m_videoGateActive = false;
	m_audioGateActive = false;
	m_clockRebased = false;
	m_audioClockRebased = false;
	m_state = (m_externalSink != nullptr && m_file.hasAudio()
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
	if (resetAudio) {
		resetGeneration(m_generation);
	}
	clearCurrentFrame();
	m_state = FFmpegMoviePlaybackState::FAILED;
	return false;
}

bool FFmpegMoviePlayback::isAudioFrameAdmitted(const FFmpegFrameMetadata &metadata)
{
	if (!m_audioGateActive) {
		return true;
	}
	const std::int64_t timestampUs = timestampToMicroseconds(metadata);
	if (timestampUs == INVALID_TIMESTAMP || timestampUs < m_seekTargetUs) {
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
		return 0;
	}
	return av_rescale_q(frameIndex, AVRational { rate.denominator, rate.numerator },
		AVRational { 1, MICROSECONDS_PER_SECOND });
}

std::int64_t FFmpegMoviePlayback::nowMicroseconds() const
{
	return m_clock ? m_clock() : defaultClock();
}

void FFmpegMoviePlayback::rebaseClocks(std::int64_t timelineBase)
{
	(void)timelineBase;
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
