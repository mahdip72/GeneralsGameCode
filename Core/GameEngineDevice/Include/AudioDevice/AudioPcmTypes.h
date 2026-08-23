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
	// Optional owner-provided playback position for audio-master video timing.
	// Sinks without a hardware/playback clock use the injected monotonic fallback.
	virtual bool getPlayedSample(std::int64_t &sample) const noexcept
	{
		(void)sample;
		return false;
	}
};
