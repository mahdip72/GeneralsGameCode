#pragma once

#include <cstdint>
#include <utility>
#include <vector>

enum class AudioPcmFormat : std::uint8_t
{
	SIGNED_16_INTERLEAVED_LITTLE_ENDIAN
};

enum class AudioPcmSubmitResult : std::uint8_t
{
	ACCEPTED,
	DROPPED,
	FAILED
};

struct AudioPcmChunk
{
	std::uint32_t sampleRate = 0;
	std::uint16_t channels = 0;
	// Original decoded channel count before normalization to the native PCM
	// format. Zero means that the producer has no provenance information.
	std::uint16_t sourceChannels = 0;
	AudioPcmFormat format = AudioPcmFormat::SIGNED_16_INTERLEAVED_LITTLE_ENDIAN;
	std::uint32_t frameCount = 0;
	std::int64_t startSample = 0;
	std::uint64_t generation = 0;
	std::uint64_t sequence = 0;
	bool discontinuity = false;
	std::vector<std::uint8_t> data;
};

class AudioPcmSink
{
public:
	virtual ~AudioPcmSink() = default;
	// Submission always consumes the chunk. A bounded sink reports DROPPED
	// instead of asking the decoder to retry consumed input; FAILED is terminal.
	virtual AudioPcmSubmitResult submit(AudioPcmChunk &&chunk) = 0;
	virtual void reset(std::uint64_t generation) = 0;
	// Owner-side teardown hook.  The default keeps device-free sinks source
	// compatible; native sinks use it to quiesce producer/voice state.
	virtual void close() noexcept {}
	// End-of-stream is separate from reset/close: accepted PCM may still be
	// queued in a hardware sink after the decoder reaches EOF.
	virtual void endOfStream() noexcept {}
	virtual bool isDrained() const noexcept { return true; }
	// Owner-side service point for sinks whose completion state is callback
	// driven. Device-free sinks have nothing to service.
	virtual bool serviceSink() noexcept { return true; }
	// Optional owner-provided playback position for audio-master video timing.
	// Sinks without a hardware/playback clock use the injected monotonic fallback.
	virtual bool getPlayedSample(std::int64_t &sample) const noexcept
	{
		(void)sample;
		return false;
	}
};
