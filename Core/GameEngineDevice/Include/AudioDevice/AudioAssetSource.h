#pragma once

#include "AudioDevice/AudioPcmTypes.h"
#include "Common/AsciiString.h"
#include "Lib/BaseType.h"

#include <algorithm>
#include <cstdint>
#include <list>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// Optional per-playback sequential decoder.  A stream owns its decoder state
// and advances monotonically; callers must open a new stream for a new phase
// or after a source-generation change.
class AudioPcmStream
{
public:
	virtual ~AudioPcmStream() = default;

	virtual UnsignedInt sampleRate() const = 0;
	virtual Real durationMS() const = 0;
	virtual Bool readPcm(AudioPcmChunk &chunk, UnsignedInt maxFrames) = 0;
};

// Neutral source/decoder seam used by both device-free scripts and native audio.
// Implementations own their storage; callers receive bounded, fixed-format PCM.
class AudioAssetSource
{
public:
	virtual ~AudioAssetSource() = default;

	virtual Bool getDurationMS(const AsciiString &fileName, Real &durationMS) const = 0;
	virtual Bool decodePcm(const AsciiString &fileName, AudioPcmChunk &chunk,
		UnsignedInt maxFrames) const = 0;

	// Optional persistent sequential decoder.  The default keeps injected and
	// legacy sources source-compatible; callers retain decodePcmAt as fallback.
	virtual Bool openPcmStream(const AsciiString &, std::unique_ptr<AudioPcmStream> &stream) const
	{
		stream.reset();
		return FALSE;
	}

	// Implementations that can seek should override this to avoid materializing
	// an entire duration-only asset.  The default remains correct for small
	// injected sources and keeps the seam source-compatible.
	virtual Bool decodePcmAt(const AsciiString &fileName, AudioPcmChunk &chunk,
		UnsignedInt maxFrames, UnsignedInt startFrame) const
	{
		if (startFrame == 0) {
			return decodePcm(fileName, chunk, maxFrames);
		}
		if (maxFrames == 0 || startFrame > static_cast<UnsignedInt>(-1) - maxFrames) {
			chunk = {};
			return FALSE;
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

	// Legacy callers may hold an identity from a different source ABI.  The
	// source, which owns the stable identity mapping, is responsible for
	// deciding whether that caller identity refers to this asset.
	virtual Bool matchesFileIdentity(const AsciiString &fileName,
		const void *callerIdentity) const
	{
		return callerIdentity != nullptr && callerIdentity == getFileIdentity(fileName);
	}

	Bool lookupDurationMS(const AsciiString &fileName, Real &durationMS) const
	{
		return getDurationMS(fileName, durationMS);
	}
};

// A deterministic catalog for tests, headless timing, and injected sources.
// It intentionally accepts only signed 16-bit interleaved
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
	}

	void setDurationMS(const AsciiString &fileName, Real durationMS)
	{
		Entry &entry = findOrCreate(fileName);
		entry.durationMS = durationMS < 0.0f ? 0.0f : durationMS;
		entry.hasDuration = TRUE;
	}

	Bool setPcm(const AsciiString &fileName, const AudioPcmChunk &source,
		Real durationMS = -1.0f)
	{
		if (source.sampleRate != DEFAULT_SAMPLE_RATE
			|| source.channels != DEFAULT_CHANNELS
			|| source.format != AudioPcmFormat::SIGNED_16_INTERLEAVED_LITTLE_ENDIAN
			|| source.frameCount == 0
			|| source.frameCount > (std::numeric_limits<std::size_t>::max)() / BYTES_PER_FRAME
			|| source.data.size() != static_cast<std::size_t>(source.frameCount) * BYTES_PER_FRAME) {
			return FALSE;
		}
		Entry &entry = findOrCreate(fileName);
		entry.pcm = source;
		entry.hasPcm = TRUE;
		if (durationMS >= 0.0f) {
			entry.durationMS = durationMS;
			entry.hasDuration = TRUE;
		} else if (source.sampleRate != 0) {
			entry.durationMS = (static_cast<Real>(source.frameCount) * 1000.0f)
				/ static_cast<Real>(source.sampleRate);
			entry.hasDuration = TRUE;
		}
		return TRUE;
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

// Optional engine-file adapter.  A production implementation can read loose
// files and BIG/archive entries through the engine FileSystem without making
// the device-free catalog depend on the legacy filesystem ABI.
class AudioVirtualFileSource
{
public:
	virtual ~AudioVirtualFileSource() = default;
	virtual Bool readFile(const AsciiString &fileName, std::vector<std::uint8_t> &bytes,
		std::string &identity) const = 0;
	virtual Bool matchesIdentity(const AsciiString &, const void *) const { return FALSE; }
};

// The production-neutral adapter resolves an asset to the current game
// directory (or an optional root) and returns bounded 48 kHz stereo s16 PCM.
// When the FFmpeg backend is configured it accepts every audio container that
// FFmpeg can decode; the built-in RIFF/WAVE reader remains available for
// device-free builds and safe fallback handling.
class FileAudioAssetSource final : public AudioAssetSource
{
public:
	FileAudioAssetSource();
	explicit FileAudioAssetSource(const AsciiString &rootDirectory);
	FileAudioAssetSource(const AsciiString &rootDirectory, AudioVirtualFileSource *virtualSource);
	~FileAudioAssetSource() override = default;

	Bool getDurationMS(const AsciiString &fileName, Real &durationMS) const override;
	Bool decodePcm(const AsciiString &fileName, AudioPcmChunk &chunk,
		UnsignedInt maxFrames) const override;
	Bool decodePcmAt(const AsciiString &fileName, AudioPcmChunk &chunk,
		UnsignedInt maxFrames, UnsignedInt startFrame) const override;
	Bool openPcmStream(const AsciiString &fileName,
		std::unique_ptr<AudioPcmStream> &stream) const override;
	const void *getFileIdentity(const AsciiString &fileName) const override;
	Bool matchesFileIdentity(const AsciiString &fileName,
		const void *callerIdentity) const override;
	AudioVirtualFileSource *getVirtualFileSource() const { return m_virtualSource; }
	void setVirtualFileSource(AudioVirtualFileSource *virtualSource)
	{
		m_ownedVirtualSource.reset();
		m_virtualSource = virtualSource;
		m_virtualIdentities.clear();
	}
	void setOwnedVirtualFileSource(std::shared_ptr<AudioVirtualFileSource> virtualSource)
	{
		m_ownedVirtualSource = std::move(virtualSource);
		m_virtualSource = m_ownedVirtualSource.get();
		m_virtualIdentities.clear();
	}

	Bool getEventDurationMS(const AsciiString &attackFile, const AsciiString &mainFile,
		const AsciiString &decayFile, Real &durationMS) const;

private:
	struct VirtualIdentity
	{
		AsciiString fileName;
		std::string identity;
	};

	std::string resolvePath(const AsciiString &fileName) const;
	Bool readVirtualFile(const AsciiString &fileName, std::vector<std::uint8_t> &bytes,
		std::string &identity) const;
	const void *findVirtualIdentity(const AsciiString &fileName) const;
	const void *rememberVirtualIdentity(const AsciiString &fileName,
		const std::string &identity) const;

	std::string m_rootDirectory;
	std::shared_ptr<AudioVirtualFileSource> m_ownedVirtualSource;
	AudioVirtualFileSource *m_virtualSource = nullptr;
	mutable std::list<std::string> m_identityPaths;
	mutable std::list<VirtualIdentity> m_virtualIdentities;
};
