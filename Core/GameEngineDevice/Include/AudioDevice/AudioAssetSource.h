#pragma once

#include "AudioDevice/AudioPcmTypes.h"
#include "Common/AsciiString.h"
#include "Lib/BaseType.h"

#include <vector>

// Neutral source/decoder seam used by both device-free scripts and native audio.
// Implementations own their storage; callers receive bounded, fixed-format PCM.
class AudioAssetSource
{
public:
	virtual ~AudioAssetSource() = default;

	virtual Bool getDurationMS(const AsciiString &fileName, Real &durationMS) const = 0;
	virtual Bool decodePcm(const AsciiString &fileName, AudioPcmChunk &chunk,
		UnsignedInt maxFrames) const = 0;

	Bool lookupDurationMS(const AsciiString &fileName, Real &durationMS) const
	{
		return getDurationMS(fileName, durationMS);
	}
};

// A deterministic catalog for tests, headless timing, and the first native
// decoder adapter.  It intentionally accepts only signed 16-bit interleaved
// PCM and clips every returned buffer to the requested frame bound.
class AudioAssetCatalog final : public AudioAssetSource
{
public:
	static constexpr UnsignedInt DEFAULT_SAMPLE_RATE = 48000;
	static constexpr UnsignedShort DEFAULT_CHANNELS = 2;
	static constexpr UnsignedInt BYTES_PER_SAMPLE = sizeof(Short);
	static constexpr UnsignedInt BYTES_PER_FRAME = DEFAULT_CHANNELS * BYTES_PER_SAMPLE;

	void setDurationMS(const AsciiString &fileName, Real durationMS)
	{
		Entry &entry = findOrCreate(fileName);
		entry.durationMS = durationMS < 0.0f ? 0.0f : durationMS;
		entry.hasDuration = TRUE;
	}

	void setPcm(const AsciiString &fileName, const AudioPcmChunk &source,
		Real durationMS = -1.0f)
	{
		Entry &entry = findOrCreate(fileName);
		entry.pcm = source;
		entry.pcm.format = AudioPcmFormat::SIGNED_16_INTERLEAVED_LITTLE_ENDIAN;
		entry.hasPcm = TRUE;
		if (durationMS >= 0.0f) {
			entry.durationMS = durationMS;
			entry.hasDuration = TRUE;
		} else if (source.sampleRate != 0) {
			entry.durationMS = (static_cast<Real>(source.frameCount) * 1000.0f)
				/ static_cast<Real>(source.sampleRate);
			entry.hasDuration = TRUE;
		}
	}

	Bool getDurationMS(const AsciiString &fileName, Real &durationMS) const override
	{
		const Entry *entry = find(fileName);
		if (entry == nullptr || !entry->hasDuration) {
			return FALSE;
		}
		durationMS = entry->durationMS;
		return TRUE;
	}

	Bool decodePcm(const AsciiString &fileName, AudioPcmChunk &chunk,
		UnsignedInt maxFrames) const override
	{
		const Entry *entry = find(fileName);
		if (entry == nullptr || (!entry->hasPcm && !entry->hasDuration)) {
			chunk = {};
			return FALSE;
		}

		if (entry->hasPcm) {
			chunk = entry->pcm;
			const UnsignedInt bytesPerFrame = static_cast<UnsignedInt>(chunk.channels) * BYTES_PER_SAMPLE;
			if (chunk.sampleRate == 0 || chunk.channels == 0 || bytesPerFrame == 0) {
				chunk = {};
				return FALSE;
			}
			const UnsignedInt availableFrames = static_cast<UnsignedInt>(chunk.data.size() / bytesPerFrame);
			if (chunk.frameCount > availableFrames) {
				chunk.frameCount = availableFrames;
			}
		} else {
			chunk = {};
			chunk.sampleRate = DEFAULT_SAMPLE_RATE;
			chunk.channels = DEFAULT_CHANNELS;
			chunk.format = AudioPcmFormat::SIGNED_16_INTERLEAVED_LITTLE_ENDIAN;
			const Real frameValue = entry->durationMS * static_cast<Real>(DEFAULT_SAMPLE_RATE) / 1000.0f;
			chunk.frameCount = frameValue <= 0.0f ? 0U : static_cast<UnsignedInt>(frameValue);
			chunk.data.assign(static_cast<std::size_t>(chunk.frameCount) * BYTES_PER_FRAME, 0U);
		}

		if (chunk.frameCount > maxFrames) {
			chunk.frameCount = maxFrames;
		}
		const UnsignedInt bytesPerFrame = static_cast<UnsignedInt>(chunk.channels) * BYTES_PER_SAMPLE;
		chunk.data.resize(static_cast<std::size_t>(chunk.frameCount) * bytesPerFrame);
		return TRUE;
	}

private:
	struct Entry
	{
		AsciiString fileName;
		Real durationMS = 0.0f;
		AudioPcmChunk pcm;
		Bool hasDuration = FALSE;
		Bool hasPcm = FALSE;
	};

	Entry &findOrCreate(const AsciiString &fileName)
	{
		for (Entry &entry : m_entries) {
			if (entry.fileName == fileName) {
				return entry;
			}
		}
		m_entries.push_back(Entry());
		m_entries.back().fileName = fileName;
		return m_entries.back();
	}

	const Entry *find(const AsciiString &fileName) const
	{
		for (const Entry &entry : m_entries) {
			if (entry.fileName == fileName) {
				return &entry;
			}
		}
		return nullptr;
	}

	std::vector<Entry> m_entries;
};
