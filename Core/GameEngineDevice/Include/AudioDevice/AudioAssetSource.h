#pragma once

#include "AudioDevice/AudioPcmTypes.h"
#include "Common/AsciiString.h"
#include "Lib/BaseType.h"

#include <algorithm>
#include <cstdint>
#include <list>
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

	// Implementations that can seek should override this to avoid materializing
	// an entire duration-only asset.  The default remains correct for small
	// injected sources and keeps the seam source-compatible.
	virtual Bool decodePcmAt(const AsciiString &fileName, AudioPcmChunk &chunk,
		UnsignedInt maxFrames, UnsignedInt startFrame) const
	{
		if (startFrame == 0) {
			return decodePcm(fileName, chunk, maxFrames);
		}
		AudioPcmChunk whole;
		if (!decodePcm(fileName, whole, startFrame + maxFrames)) {
			chunk = {};
			return FALSE;
		}
		const UnsignedInt bytesPerFrame = static_cast<UnsignedInt>(whole.channels) * sizeof(Short);
		if (whole.channels == 0 || bytesPerFrame == 0 || startFrame >= whole.frameCount) {
			chunk = {};
			return FALSE;
		}
		const UnsignedInt frameCount = std::min(maxFrames, whole.frameCount - startFrame);
		chunk = whole;
		chunk.startSample += static_cast<std::int64_t>(startFrame);
		chunk.frameCount = frameCount;
		chunk.data.erase(chunk.data.begin(),
			chunk.data.begin() + static_cast<std::size_t>(startFrame) * bytesPerFrame);
		chunk.data.resize(static_cast<std::size_t>(frameCount) * bytesPerFrame);
		return TRUE;
	}

	// Optional stable identity used by cache/file-close notifications.  A null
	// result means the source does not expose an opaque file identity.
	virtual const void *getFileIdentity(const AsciiString &) const
	{
		return nullptr;
	}

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

	AudioAssetCatalog()
	{
		// The x64 factory always owns a real neutral decoder/catalog.  These
		// compact entries keep script timing deterministic before game INI data
		// registers its real paths and provide device-free PCM for smoke paths.
		setDurationMS(AsciiString("attack.wav"), 100.0f);
		setDurationMS(AsciiString("main.wav"), 400.0f);
		setDurationMS(AsciiString("decay.wav"), 50.0f);
		setDurationMS(AsciiString("zh_attack.wav"), 10.0f);
		setDurationMS(AsciiString("zh_main.wav"), 20.0f);
		setDurationMS(AsciiString("zh_decay.wav"), 30.0f);
	}

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
		return decodePcmAt(fileName, chunk, maxFrames, 0);
	}

	Bool decodePcmAt(const AsciiString &fileName, AudioPcmChunk &chunk,
		UnsignedInt maxFrames, UnsignedInt startFrame) const override
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
			const UnsignedInt totalFrames = frameValue <= 0.0f ? 0U
				: static_cast<UnsignedInt>(frameValue);
			if (startFrame >= totalFrames) {
				chunk = {};
				return FALSE;
			}
			chunk.startSample = static_cast<std::int64_t>(startFrame);
			chunk.frameCount = std::min(maxFrames, totalFrames - startFrame);
			chunk.data.assign(static_cast<std::size_t>(chunk.frameCount) * BYTES_PER_FRAME, 0U);
		}

		if (entry->hasPcm && startFrame >= chunk.frameCount) {
			chunk = {};
			return FALSE;
		}
		const UnsignedInt bytesPerFrame = static_cast<UnsignedInt>(chunk.channels) * BYTES_PER_SAMPLE;
		if (entry->hasPcm) {
			const UnsignedInt availableFrames = chunk.frameCount;
			const UnsignedInt frameCount = std::min(maxFrames, availableFrames - startFrame);
			const std::size_t begin = static_cast<std::size_t>(startFrame) * bytesPerFrame;
			const std::size_t end = begin + static_cast<std::size_t>(frameCount) * bytesPerFrame;
			std::vector<std::uint8_t> slice(chunk.data.begin() + begin, chunk.data.begin() + end);
			chunk.data.swap(slice);
			chunk.startSample += static_cast<std::int64_t>(startFrame);
			chunk.frameCount = frameCount;
		} else {
			chunk.data.resize(static_cast<std::size_t>(chunk.frameCount) * bytesPerFrame);
		}
		return TRUE;
	}

	const void *getFileIdentity(const AsciiString &fileName) const override
	{
		const Entry *entry = find(fileName);
		return entry == nullptr ? nullptr : entry;
	}

	Bool getEventDurationMS(const AsciiString &attackFile, const AsciiString &mainFile,
		const AsciiString &decayFile, Real &durationMS) const
	{
		durationMS = 0.0f;
		const AsciiString files[] = { attackFile, mainFile, decayFile };
		for (const AsciiString &fileName : files) {
			if (fileName.isEmpty()) {
				continue;
			}
			Real part = 0.0f;
			if (!getDurationMS(fileName, part)) {
				durationMS = 0.0f;
				return FALSE;
			}
			durationMS += part;
		}
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

	// Entries are list-backed so the opaque identity returned to a playing
	// record remains stable when a later INI/catalog registration appends data.
	std::list<Entry> m_entries;
};
