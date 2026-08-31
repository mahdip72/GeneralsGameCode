#include "AudioDevice/AudioAssetSource.h"

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <process.h>
#include <string>
#include <system_error>
#include <utility>

namespace
{
void check(bool condition, const char *message = "catalog assertion failed");

struct TestFailure
{
};

class MemoryVirtualAudioSource final : public AudioVirtualFileSource
{
public:
	MemoryVirtualAudioSource(std::string name, std::vector<std::uint8_t> bytes) :
		m_name(std::move(name)), m_bytes(std::move(bytes))
	{
	}

	Bool readFile(const AsciiString &fileName, std::vector<std::uint8_t> &bytes,
		std::string &identity) const override
	{
		if (fileName.str() == nullptr || (std::string(fileName.str()) != m_name
			&& (m_alias.empty() || std::string(fileName.str()) != m_alias))) {
			return FALSE;
		}
		++m_readCalls;
		bytes = m_bytes;
		identity = "archive:" + m_name;
		return TRUE;
	}
	UnsignedInt getReadCalls() const { return m_readCalls; }
	void setAlias(const std::string &alias) { m_alias = alias; }
	Bool matchesIdentity(const AsciiString &fileName, const void *identity) const override
	{
		return fileName.str() != nullptr && std::string(fileName.str()) == m_name
			&& identity == this;
	}

private:
	std::string m_name;
	std::string m_alias;
	std::vector<std::uint8_t> m_bytes;
	mutable UnsignedInt m_readCalls = 0;
};

void writeWaveFrames(const std::filesystem::path &path, UnsignedInt frames,
	UnsignedInt sampleRate = 48000, UnsignedShort channels = 2)
{
	const UnsignedInt bytesPerFrame = static_cast<UnsignedInt>(channels) * sizeof(Short);
	const UnsignedInt dataBytes = frames * bytesPerFrame;
	std::ofstream output(path, std::ios::binary | std::ios::trunc);
	check(output.good(), "real fixture opens for writing");
	auto writeU16 = [&output](UnsignedShort value) {
		output.put(static_cast<char>(value & 0xffU));
		output.put(static_cast<char>((value >> 8U) & 0xffU));
	};
	auto writeU32 = [&output](UnsignedInt value) {
		for (UnsignedInt shift = 0; shift < 32; shift += 8) {
			output.put(static_cast<char>((value >> shift) & 0xffU));
		}
	};
	output.write("RIFF", 4);
	writeU32(36U + dataBytes);
	output.write("WAVEfmt ", 8);
	writeU32(16U);
	writeU16(1U);
	writeU16(channels);
	writeU32(sampleRate);
	writeU32(sampleRate * bytesPerFrame);
	writeU16(static_cast<UnsignedShort>(bytesPerFrame));
	writeU16(16U);
	output.write("data", 4);
	writeU32(dataBytes);
	for (UnsignedInt frame = 0; frame < frames; ++frame) {
		for (UnsignedShort channel = 0; channel < channels; ++channel) {
			const Short sample = static_cast<Short>((frame + channel) % 32767U);
			writeU16(static_cast<UnsignedShort>(sample));
		}
	}
	check(output.good(), "real fixture is fully written");
}

void writeWaveFile(const std::filesystem::path &path, UnsignedInt durationMS,
	UnsignedInt sampleRate = 48000, UnsignedShort channels = 2)
{
	writeWaveFrames(path, durationMS * sampleRate / 1000U, sampleRate, channels);
}

std::vector<std::uint8_t> readBinaryFile(const std::filesystem::path &path)
{
	std::ifstream input(path, std::ios::binary | std::ios::ate);
	check(input.good(), "binary fixture opens for reading");
	const std::streamsize size = input.tellg();
	check(size > 0, "binary fixture has content");
	input.seekg(0, std::ios::beg);
	std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
	input.read(reinterpret_cast<char *>(bytes.data()), size);
	check(input.good(), "binary fixture is fully read");
	return bytes;
}

int runFFmpeg(const std::string &executable, const std::filesystem::path &input,
	const char *codec, const std::filesystem::path &output)
{
	const std::string inputPath = input.string();
	const std::string outputPath = output.string();
	const char *arguments[] = {
		executable.c_str(), "-y", "-v", "error", "-i", inputPath.c_str(),
		"-c:a", codec, outputPath.c_str(), nullptr
	};
	return static_cast<int>(_spawnv(_P_WAIT, executable.c_str(), arguments));
}

#if defined(RTS_NATIVE_AUDIO_HAS_FFMPEG_CLI)
std::uint64_t consumePcmStream(AudioPcmStream &stream)
{
	std::uint64_t expectedStart = 0;
	UnsignedInt readCount = 0;
	for (;;) {
		AudioPcmChunk chunk;
		if (!stream.readPcm(chunk, 48000U)) {
			break;
		}
		check(chunk.sampleRate == 48000U && chunk.channels == 2U
			&& chunk.format == AudioPcmFormat::SIGNED_16_INTERLEAVED_LITTLE_ENDIAN
			&& chunk.frameCount > 0U && chunk.frameCount <= 48000U,
			"persistent PCM stream returns bounded shipping chunks");
		check(chunk.startSample == static_cast<std::int64_t>(expectedStart),
			"persistent PCM stream preserves contiguous sample offsets");
		check(chunk.data.size() == static_cast<std::size_t>(chunk.frameCount)
			* 2U * sizeof(Short), "persistent PCM stream returns complete frames");
		expectedStart += chunk.frameCount;
		check(++readCount <= 16U, "persistent PCM stream remains bounded at EOF");
	}
	check(readCount >= 4U, "persistent PCM stream consumes multiple sequential chunks");
	return expectedStart;
}
#endif
}

namespace
{
void check(bool condition, const char *message)
{
	if (!condition) {
		std::fprintf(stderr, "FAIL: %s\n", message);
		std::fflush(stderr);
		throw TestFailure{};
	}
}

class FixtureDirectory
{
public:
	explicit FixtureDirectory(std::filesystem::path path) : m_path(std::move(path)) {}
	~FixtureDirectory()
	{
		std::error_code cleanupError;
		std::filesystem::remove_all(m_path, cleanupError);
	}
	const std::filesystem::path &path() const { return m_path; }

private:
	std::filesystem::path m_path;
};
}

int runCatalogTest(int argc, char *argv[])
{
#if defined(RTS_NATIVE_AUDIO_HAS_FFMPEG_CLI)
	check(argc == 3 && std::string(argv[1]) == "--ffmpeg" && argv[2][0] != '\0',
		"FFmpeg-enabled catalog test receives the resolved fixture executable");
	const std::string ffmpegExecutable = argv[2];
#else
	(void)argc;
	(void)argv;
#endif
	AudioAssetCatalog catalog;
	Real duration = 0.0f;
	catalog.setDurationMS(AsciiString("attack.wav"), 100.0f);
	catalog.setDurationMS(AsciiString("main.wav"), 400.0f);
	catalog.setDurationMS(AsciiString("decay.wav"), 50.0f);
	check(catalog.getDurationMS(AsciiString("attack.wav"), duration) && duration == 100.0f,
		"injected attack duration is exact");
	check(catalog.getDurationMS(AsciiString("main.wav"), duration) && duration == 400.0f,
		"injected main duration is exact");
	check(catalog.getDurationMS(AsciiString("decay.wav"), duration) && duration == 50.0f,
		"injected decay duration is exact");
	AudioPcmChunk defaultPcm;
	check(catalog.decodePcm(AsciiString("main.wav"), defaultPcm, 3),
		"duration-only catalog entry produces bounded PCM");
	check(defaultPcm.frameCount == 3 && defaultPcm.data.size() == 3U * AudioAssetCatalog::BYTES_PER_FRAME,
		"duration-only PCM allocation is bounded before materialization");
	AudioPcmChunk continuation;
	check(catalog.decodePcmAt(AsciiString("main.wav"), continuation, 2, 100),
		"duration-only catalog entry supports deterministic continuation");
	check(continuation.frameCount == 2 && continuation.startSample == 100,
		"continuation preserves its nonzero source frame offset");
	check(catalog.getFileIdentity(AsciiString("main.wav")) != nullptr,
		"catalog exposes stable file identity for close notifications");

	catalog.setDurationMS(AsciiString("generals_attack.wav"), 125.0f);
	catalog.setDurationMS(AsciiString("generals_main.wav"), 350.0f);
	catalog.setDurationMS(AsciiString("generals_decay.wav"), 75.0f);
	catalog.setDurationMS(AsciiString("zerohour_attack.wav"), 10.0f);
	catalog.setDurationMS(AsciiString("zerohour_main.wav"), 20.0f);
	catalog.setDurationMS(AsciiString("zerohour_decay.wav"), 30.0f);

	check(catalog.getDurationMS(AsciiString("generals_attack.wav"), duration));
	check(duration == 125.0f);
	check(catalog.lookupDurationMS(AsciiString("generals_main.wav"), duration));
	check(duration == 350.0f);
	check(!catalog.getDurationMS(AsciiString("missing.wav"), duration));
	check(catalog.getEventDurationMS(AsciiString("generals_attack.wav"),
		AsciiString("generals_main.wav"), AsciiString("generals_decay.wav"), duration));
	check(duration == 550.0f);
	check(catalog.getEventDurationMS(AsciiString("zerohour_attack.wav"),
		AsciiString("zerohour_main.wav"), AsciiString("zerohour_decay.wav"), duration));
	check(duration == 60.0f);

	AudioPcmChunk pcm;
	check(catalog.decodePcm(AsciiString("generals_main.wav"), pcm, 4));
	check(pcm.sampleRate == AudioAssetCatalog::DEFAULT_SAMPLE_RATE);
	check(pcm.channels == AudioAssetCatalog::DEFAULT_CHANNELS);
	check(pcm.frameCount == 4);
	check(pcm.data.size() == 4U * AudioAssetCatalog::BYTES_PER_FRAME);

	AudioPcmChunk source;
	source.sampleRate = 22050;
	source.channels = 1;
	source.frameCount = 8;
	source.data.assign(8U * 2U, 0x7fU);
	check(!catalog.setPcm(AsciiString("voice.wav"), source, 12.0f),
		"catalog rejects non-shipping PCM instead of rewriting its format");
	source.sampleRate = AudioAssetCatalog::DEFAULT_SAMPLE_RATE;
	source.channels = AudioAssetCatalog::DEFAULT_CHANNELS;
	source.frameCount = 8;
	source.data.assign(8U * AudioAssetCatalog::BYTES_PER_FRAME, 0x7fU);
	source.format = AudioPcmFormat::SIGNED_16_INTERLEAVED_LITTLE_ENDIAN;
	check(catalog.setPcm(AsciiString("voice.wav"), source, 12.0f),
		"catalog accepts the bounded shipping PCM format");
	check(catalog.decodePcm(AsciiString("voice.wav"), pcm, 3));
	check(pcm.sampleRate == AudioAssetCatalog::DEFAULT_SAMPLE_RATE);
	check(pcm.channels == AudioAssetCatalog::DEFAULT_CHANNELS);
	check(pcm.frameCount == 3);
	check(pcm.data.size() == 3U * AudioAssetCatalog::BYTES_PER_FRAME);
	check(pcm.data[0] == 0x7fU);
	check(pcm.data[7] == 0x7fU);

	FixtureDirectory fixture(std::filesystem::temp_directory_path()
		/ ("rts-native-audio-real-source-" + std::to_string(_getpid())));
	const std::filesystem::path &root = fixture.path();
	std::filesystem::create_directories(root);
	const std::filesystem::path attackPath = root / "attack.wav";
	const std::filesystem::path mainPath = root / "main.wav";
	const std::filesystem::path decayPath = root / "decay.wav";
	writeWaveFile(attackPath, 125U);
	writeWaveFile(mainPath, 350U);
	writeWaveFile(decayPath, 75U);
	FileAudioAssetSource rootedSource(AsciiString((root / "Audio").string().c_str()));
	std::filesystem::create_directories(root / "Audio");
	writeWaveFile(root / "Audio" / "generated_attack.wav", 125U);
	check(rootedSource.getDurationMS(
		AsciiString("Audio\\generated_attack.wav"), duration)
		&& duration == 125.0f,
		"generated Audio root is not duplicated when resolving a virtual asset name");
	check(!rootedSource.getDurationMS(AsciiString("..\\main.wav"), duration),
		"configured audio root rejects parent traversal");
	check(!rootedSource.getDurationMS(AsciiString("Audio\\..\\main.wav"), duration),
		"configured audio root rejects traversal through its virtual prefix");
	check(!rootedSource.getDurationMS(AsciiString(mainPath.string().c_str()), duration),
		"configured audio root rejects absolute paths");
	const std::filesystem::path virtualPath = root / "virtual_main.wav";
	writeWaveFile(virtualPath, 350U);
	std::ifstream virtualInput(virtualPath, std::ios::binary | std::ios::ate);
	check(virtualInput.good(), "archive fixture opens for reading");
	const std::streamsize virtualSize = virtualInput.tellg();
	virtualInput.seekg(0, std::ios::beg);
	std::vector<std::uint8_t> virtualBytes(static_cast<std::size_t>(virtualSize));
	virtualInput.read(reinterpret_cast<char *>(virtualBytes.data()), virtualSize);
	check(virtualInput.good(), "archive fixture is fully read");
	virtualInput.close();
	MemoryVirtualAudioSource archiveSource("archive\\virtual_main.wav", std::move(virtualBytes));
	FileAudioAssetSource archiveAwareSource(AsciiString(root.string().c_str()), &archiveSource);
	check(archiveAwareSource.getDurationMS(AsciiString("archive\\virtual_main.wav"), duration)
		&& duration == 350.0f,
		"archive-backed audio is resolved when no loose file exists");
	AudioPcmChunk archiveChunk;
	check(archiveAwareSource.decodePcmAt(AsciiString("archive\\virtual_main.wav"),
		archiveChunk, 3U, 7U)
		&& archiveChunk.frameCount == 3U && archiveChunk.startSample == 7,
		"archive-backed audio decodes a bounded continuation");
	const UnsignedInt fallbackReadsBeforeIdentity = archiveSource.getReadCalls();
	check(archiveAwareSource.getFileIdentity(AsciiString("archive\\virtual_main.wav")) != nullptr
		&& archiveSource.getReadCalls() == fallbackReadsBeforeIdentity,
		"archive fallback exposes cached identity without another provider read");
	check(archiveAwareSource.matchesFileIdentity(
		AsciiString("archive\\virtual_main.wav"), &archiveSource),
		"archive source bridges a legacy caller identity through its provider");
	FileAudioAssetSource realSource;
	check(realSource.getDurationMS(AsciiString(attackPath.string().c_str()), duration)
		&& duration == 125.0f, "filesystem source reports exact Generals attack duration");
	check(realSource.getDurationMS(AsciiString(mainPath.string().c_str()), duration)
		&& duration == 350.0f, "filesystem source reports exact Generals main duration");
	check(realSource.getDurationMS(AsciiString(decayPath.string().c_str()), duration)
		&& duration == 75.0f, "filesystem source reports exact Generals decay duration");
	Real generalsDuration = 0.0f;
	check(realSource.getEventDurationMS(
		AsciiString(attackPath.string().c_str()),
		AsciiString(mainPath.string().c_str()),
		AsciiString(decayPath.string().c_str()), generalsDuration)
		&& generalsDuration == 550.0f, "filesystem source sums Generals script duration");

	const std::filesystem::path zhAttackPath = root / "zh_attack.wav";
	const std::filesystem::path zhMainPath = root / "zh_main.wav";
	const std::filesystem::path zhDecayPath = root / "zh_decay.wav";
	writeWaveFile(zhAttackPath, 10U);
	writeWaveFile(zhMainPath, 20U);
	writeWaveFile(zhDecayPath, 30U);
	Real zhDuration = 0.0f;
	check(realSource.getEventDurationMS(
		AsciiString(zhAttackPath.string().c_str()),
		AsciiString(zhMainPath.string().c_str()),
		AsciiString(zhDecayPath.string().c_str()), zhDuration)
		&& zhDuration == 60.0f, "filesystem source sums Zero Hour script duration");

	AudioPcmChunk realChunk;
	check(realSource.decodePcmAt(AsciiString(mainPath.string().c_str()), realChunk, 3U, 7U),
		"filesystem source decodes bounded PCM continuation");
	check(realChunk.sampleRate == 48000U && realChunk.channels == 2U
		&& realChunk.frameCount == 3U && realChunk.startSample == 7,
		"filesystem PCM preserves output format and continuation offset");
	check(realChunk.data.size() == 3U * 2U * sizeof(Short),
		"filesystem PCM allocation is bounded to requested frames");
	check(realSource.getFileIdentity(AsciiString(mainPath.string().c_str())) != nullptr,
		"filesystem source exposes stable file identity");
	check(!realSource.getDurationMS(AsciiString((root / "missing.wav").string().c_str()), duration),
		"filesystem source safely rejects missing assets");
	const std::filesystem::path corruptPath = root / "corrupt.wav";
	std::ofstream corrupt(corruptPath, std::ios::binary | std::ios::trunc);
	corrupt.write("not-a-wave", 10);
	corrupt.close();
	check(!realSource.getDurationMS(AsciiString(corruptPath.string().c_str()), duration),
		"filesystem source safely rejects corrupt assets");

#if defined(RTS_NATIVE_AUDIO_HAS_FFMPEG_CLI)
	const std::filesystem::path monoPath = root / "mono.wav";
	writeWaveFrames(monoPath, 512U, 48000U, 1U);
	AudioPcmChunk monoReference;
	check(realSource.decodePcmAt(AsciiString(monoPath.string().c_str()), monoReference, 512U, 0U)
		&& monoReference.sourceChannels == 1U && monoReference.frameCount == 512U,
		"mono PCM fallback exposes the original channel provenance");
	std::unique_ptr<AudioPcmStream> monoStream;
	AudioPcmChunk monoStreamChunk;
	check(realSource.openPcmStream(AsciiString(monoPath.string().c_str()), monoStream)
		&& monoStream->readPcm(monoStreamChunk, 512U)
		&& monoStreamChunk.sourceChannels == 1U && monoStreamChunk.data == monoReference.data,
		"legacy asset streams duplicate mono at unity like the PCM fallback");
	monoStream.reset();
	MemoryVirtualAudioSource monoArchive("archive\\mono.wav", readBinaryFile(monoPath));
	FileAudioAssetSource monoSource(AsciiString(root.string().c_str()), &monoArchive);
	monoSource.setSamplePcmCacheBudget(4096U);
	for (int playback = 0; playback < 2; ++playback) {
		check(monoSource.openPcmSampleStream(AsciiString("archive\\mono.wav"), monoStream)
			&& monoStream->readPcm(monoStreamChunk, 512U)
			&& monoStreamChunk.sourceChannels == 1U && monoStreamChunk.data == monoReference.data,
			"archive and cached mono samples retain unity duplication and provenance");
		monoStream.reset();
	}
	check(monoArchive.getReadCalls() == 1U, "mono sample gain is preserved on a real cache hit");
	const std::filesystem::path monoGenericPath = root / "mono.aiff";
	check(runFFmpeg(ffmpegExecutable, monoPath, "pcm_s16le", monoGenericPath) == 0,
		"FFmpeg creates the lossless mono generic-container fixture");
	check(realSource.decodePcmAt(AsciiString(monoGenericPath.string().c_str()), monoStreamChunk, 512U, 0U)
		&& monoStreamChunk.sourceChannels == 1U && monoStreamChunk.data == monoReference.data,
		"loose FFmpeg range decoding matches unity mono PCM fallback levels");
	MemoryVirtualAudioSource monoGenericArchive("archive\\mono.aiff", readBinaryFile(monoGenericPath));
	FileAudioAssetSource monoGenericSource(AsciiString(root.string().c_str()), &monoGenericArchive);
	check(monoGenericSource.decodePcmAt(AsciiString("archive\\mono.aiff"), monoStreamChunk, 512U, 0U)
		&& monoStreamChunk.sourceChannels == 1U && monoStreamChunk.data == monoReference.data,
		"archive FFmpeg range decoding matches unity mono PCM fallback levels");

	const std::filesystem::path adpcmPath = root / "adpcm.wav";
	check(runFFmpeg(ffmpegExecutable, mainPath, "adpcm_ima_wav", adpcmPath) == 0,
		"FFmpeg creates a deterministic IMA ADPCM fixture");
	MemoryVirtualAudioSource adpcmArchive("archive\\adpcm.wav", readBinaryFile(adpcmPath));
	FileAudioAssetSource adpcmSource(AsciiString(root.string().c_str()), &adpcmArchive);
	check(adpcmSource.getDurationMS(AsciiString("archive\\adpcm.wav"), duration)
		&& duration == 350.0f, "archive ADPCM duration remains exact");
	AudioPcmChunk adpcmChunk;
	check(adpcmSource.decodePcmAt(AsciiString("archive\\adpcm.wav"), adpcmChunk, 3U, 7U),
		"archive ADPCM decodes through the audio-only FFmpeg provider path");
	check(adpcmChunk.sampleRate == 48000U && adpcmChunk.channels == 2U
		&& adpcmChunk.frameCount == 3U && adpcmChunk.startSample == 7
		&& adpcmChunk.data.size() == 3U * 2U * sizeof(Short),
		"archive ADPCM PCM remains bounded 48 kHz stereo s16");

	MemoryVirtualAudioSource cachedArchive("archive\\cached.wav", readBinaryFile(mainPath));
	cachedArchive.setAlias("archive\\cache-alias.wav");
	FileAudioAssetSource cachedSource(AsciiString(root.string().c_str()), &cachedArchive);
	const AsciiString cachedName("archive\\cached.wav");
	const AsciiString aliasName("archive\\cache-alias.wav");
	std::unique_ptr<AudioPcmStream> cachedFirst;
	std::unique_ptr<AudioPcmStream> cachedSecond;
	check(cachedSource.openPcmSampleStream(cachedName, cachedFirst)
		&& cachedSource.openPcmSampleStream(cachedName, cachedSecond)
		&& cachedArchive.getReadCalls() == 2U,
		"short sample caching is disabled unless its owner opts in");
	cachedFirst.reset();
	cachedSecond.reset();
	constexpr std::size_t shortSampleBytes = 350U * 48U * 4U;
	cachedSource.setSamplePcmCacheBudget(shortSampleBytes);
	const UnsignedInt readsBeforeCache = cachedArchive.getReadCalls();
	check(cachedSource.openPcmSampleStream(cachedName, cachedFirst)
		&& cachedSource.openPcmSampleStream(cachedName, cachedSecond)
		&& cachedArchive.getReadCalls() == readsBeforeCache + 1U,
		"short sample playback reuses decoded PCM without another archive read");
	AudioPcmChunk cachedFirstChunk;
	AudioPcmChunk cachedSecondChunk;
	check(cachedFirst->readPcm(cachedFirstChunk, 7U)
		&& cachedSecond->readPcm(cachedSecondChunk, 7U)
		&& cachedFirstChunk.data == cachedSecondChunk.data
		&& cachedFirstChunk.startSample == 0 && cachedSecondChunk.startSample == 0,
		"cached playbacks have independent cursors and identical source samples");
	check(cachedFirst->readPcm(cachedFirstChunk, 7U) && cachedFirstChunk.startSample == 7,
		"one cached playback advances without changing another playback");
	std::unique_ptr<AudioPcmStream> uncachedSpeech;
	check(cachedSource.openPcmStream(cachedName, uncachedSpeech)
		&& cachedArchive.getReadCalls() == readsBeforeCache + 2U,
		"the music and speech stream entrypoint bypasses the sample cache");
	uncachedSpeech.reset();
	cachedSource.invalidateSamplePcmCache();
	const UnsignedInt readsBeforePinned = cachedArchive.getReadCalls();
	std::unique_ptr<AudioPcmStream> pinnedFallback;
	check(cachedSource.openPcmSampleStream(aliasName, pinnedFallback),
		"an old-generation pinned sample does not block uncached playback");
	pinnedFallback.reset();
	check(cachedSource.openPcmSampleStream(aliasName, pinnedFallback)
		&& cachedArchive.getReadCalls() == readsBeforePinned + 2U,
		"invalidated but live PCM keeps its budget charge and cannot grow the cache");
	pinnedFallback.reset();
	check(cachedSecond->readPcm(cachedSecondChunk, 7U) && cachedSecondChunk.startSample == 7,
		"invalidation leaves an active immutable sample and cursor valid");
	cachedFirst.reset();
	cachedSecond.reset();
	const UnsignedInt readsAfterUnpin = cachedArchive.getReadCalls();
	check(cachedSource.openPcmSampleStream(aliasName, cachedFirst),
		"released pinned PCM returns its byte budget");
	cachedFirst.reset();
	check(cachedSource.openPcmSampleStream(aliasName, cachedFirst)
		&& cachedArchive.getReadCalls() == readsAfterUnpin + 1U,
		"a newly available budget caches the replacement sample");
	cachedFirst.reset();
	const UnsignedInt readsBeforeEviction = cachedArchive.getReadCalls();
	check(cachedSource.openPcmSampleStream(cachedName, cachedFirst),
		"an unpinned least-recent sample can be evicted for a new sound");
	cachedFirst.reset();
	check(cachedSource.openPcmSampleStream(aliasName, cachedFirst)
		&& cachedArchive.getReadCalls() == readsBeforeEviction + 2U,
		"evicted entries are decoded again instead of exceeding the one-sample budget");
	cachedFirst.reset();
	MemoryVirtualAudioSource replacementArchive("archive\\cached.wav", readBinaryFile(decayPath));
	cachedSource.setVirtualFileSource(&replacementArchive);
	check(cachedSource.openPcmSampleStream(cachedName, cachedFirst)
		&& cachedFirst->durationMS() == 75.0f && replacementArchive.getReadCalls() == 1U,
		"replacing the virtual source invalidates cached PCM from the previous generation");
	cachedFirst.reset();
	cachedSource.setSamplePcmCacheBudget(0);
	check(cachedSource.openPcmSampleStream(cachedName, cachedFirst)
		&& cachedSource.openPcmSampleStream(cachedName, cachedSecond)
		&& replacementArchive.getReadCalls() == 3U,
		"disabling the cache releases reusable entries and restores sequential playback");
	cachedFirst.reset();
	cachedSecond.reset();

	FileAudioAssetSource looseCachedSource;
	looseCachedSource.setSamplePcmCacheBudget(192000U);
	const std::filesystem::path looseCachedPath = root / "cached_loose.wav";
	writeWaveFile(looseCachedPath, 125U);
	const AsciiString looseCachedName(looseCachedPath.string().c_str());
	check(looseCachedSource.openPcmSampleStream(looseCachedName, cachedFirst)
		&& cachedFirst->durationMS() == 125.0f, "a loose short sample is cacheable");
	writeWaveFile(looseCachedPath, 75U);
	check(looseCachedSource.openPcmSampleStream(looseCachedName, cachedSecond)
		&& cachedSecond->durationMS() == 75.0f && cachedFirst->durationMS() == 125.0f,
		"loose file metadata invalidates a changed asset without mutating active PCM");
	cachedFirst.reset();
	cachedSecond.reset();
	MemoryVirtualAudioSource overrideArchive("archive\\override.wav", readBinaryFile(mainPath));
	FileAudioAssetSource overrideSource(AsciiString(root.string().c_str()), &overrideArchive);
	overrideSource.setSamplePcmCacheBudget(192000U);
	const AsciiString overrideName("archive\\override.wav");
	check(overrideSource.openPcmSampleStream(overrideName, cachedFirst)
		&& cachedFirst->durationMS() == 350.0f && overrideArchive.getReadCalls() == 1U,
		"archive PCM is cached before a matching loose override exists");
	std::filesystem::create_directories(root / "archive");
	writeWaveFile(root / "archive" / "override.wav", 75U);
	check(overrideSource.openPcmSampleStream(overrideName, cachedSecond)
		&& cachedSecond->durationMS() == 75.0f && cachedFirst->durationMS() == 350.0f
		&& overrideArchive.getReadCalls() == 1U,
		"new loose overrides invalidate virtual cache hits while active archive PCM stays immutable");
	cachedFirst.reset();
	cachedSecond.reset();
#if defined(RTS_NATIVE_AUDIO_ASSET_SOURCE_TEST_HOOK)
	cachedSource.setVirtualFileSource(&cachedArchive);
	cachedSource.setSamplePcmCacheBudget(192000U);
	NativeAudioAssetSourceTestHook::failFFmpegReadFrameAfter(1U);
	check(cachedSource.openPcmSampleStream(cachedName, cachedFirst),
		"a failed cache fill falls back to a fresh ordinary decoder");
	AudioPcmChunk failedCacheChunk;
	check(!cachedFirst->readPcm(failedCacheChunk, 48000U) && !cachedFirst->isEnded(),
		"a cache-fill decode failure is not published as truncated PCM or clean EOF");
	NativeAudioAssetSourceTestHook::clearFFmpegReadFrameFailure();
	cachedFirst.reset();
	const UnsignedInt readsAfterCacheFailure = cachedArchive.getReadCalls();
	check(cachedSource.openPcmSampleStream(cachedName, cachedFirst)
		&& cachedSource.openPcmSampleStream(cachedName, cachedSecond)
		&& cachedArchive.getReadCalls() == readsAfterCacheFailure + 1U,
		"a failed cache fill does not poison subsequent complete sample reuse");
	cachedFirst.reset();
	cachedSecond.reset();
#endif

	const std::filesystem::path genericPath = root / "main.aiff";
	check(runFFmpeg(ffmpegExecutable, mainPath, "pcm_s16le", genericPath) == 0,
		"FFmpeg creates a deterministic non-WAVE audio container");
	check(realSource.getDurationMS(AsciiString(genericPath.string().c_str()), duration)
		&& duration == 350.0f, "FFmpeg source reports exact generic audio duration");
	AudioPcmChunk genericChunk;
	check(realSource.decodePcmAt(AsciiString(genericPath.string().c_str()), genericChunk, 3U, 7U),
		"FFmpeg source decodes generic audio without a video stream");
	check(genericChunk.sampleRate == 48000U && genericChunk.channels == 2U
		&& genericChunk.frameCount == 3U && genericChunk.data.size() == 3U * 2U * sizeof(Short),
		"FFmpeg generic PCM remains bounded 48 kHz stereo s16");

	constexpr UnsignedInt exactSourceFrames = 220523U;
	constexpr UnsignedInt exactSourceRate = 44100U;
	const std::filesystem::path exactDurationPath = root / "exact_duration.wav";
	writeWaveFrames(exactDurationPath, exactSourceFrames, exactSourceRate);
	std::unique_ptr<AudioPcmStream> exactDurationStream;
	check(realSource.openPcmStream(AsciiString(exactDurationPath.string().c_str()),
		exactDurationStream) && exactDurationStream != nullptr,
		"filesystem source opens a non-millisecond persistent stream");
	const std::uint64_t estimatedExactFrames =
		(static_cast<std::uint64_t>(exactSourceFrames) * 48000U + exactSourceRate / 2U)
		/ exactSourceRate;
	check(exactDurationStream->durationMS() > 5000.0f
		&& exactDurationStream->durationMS() < 5001.0f,
		"persistent stream preserves sub-millisecond duration metadata");
	const std::uint64_t drainedExactFrames = consumePcmStream(*exactDurationStream);
	check(drainedExactFrames >= estimatedExactFrames
		&& drainedExactFrames <= estimatedExactFrames + 1U,
		"non-millisecond persistent stream drains through decoder EOF");

	const std::filesystem::path longPath = root / "long.wav";
	const std::filesystem::path longAdpcmPath = root / "long_adpcm.wav";
	writeWaveFile(longPath, 5000U);
	MemoryVirtualAudioSource uncachedLongArchive("archive\\long-cache-bypass.wav", readBinaryFile(longPath));
	FileAudioAssetSource uncachedLongSource(AsciiString(root.string().c_str()), &uncachedLongArchive);
	uncachedLongSource.setSamplePcmCacheBudget(1920000U);
	check(uncachedLongSource.openPcmSampleStream(AsciiString("archive\\long-cache-bypass.wav"), cachedFirst)
		&& uncachedLongSource.openPcmSampleStream(AsciiString("archive\\long-cache-bypass.wav"), cachedSecond)
		&& uncachedLongArchive.getReadCalls() == 2U,
		"long sound effects remain sequential streams even when the cache budget could hold them");
	cachedFirst.reset();
	cachedSecond.reset();
	check(runFFmpeg(ffmpegExecutable, longPath, "adpcm_ima_wav", longAdpcmPath) == 0,
		"FFmpeg creates the persistent-stream ADPCM fixture");
	std::unique_ptr<AudioPcmStream> looseStream;
	check(realSource.openPcmStream(
		AsciiString(longAdpcmPath.string().c_str()), looseStream) && looseStream != nullptr,
		"filesystem source opens a persistent FFmpeg PCM stream");
	check(looseStream->sampleRate() == 48000U && looseStream->durationMS() > 4900.0f
		&& looseStream->durationMS() < 5100.0f,
		"persistent filesystem stream exposes bounded output metadata");
#if defined(RTS_NATIVE_AUDIO_ASSET_SOURCE_TEST_HOOK)
	NativeAudioAssetSourceTestHook::failFFmpegReadFrameAfter(1U);
	AudioPcmChunk streamReadFailureChunk;
	check(!looseStream->readPcm(streamReadFailureChunk, 48000U)
		&& streamReadFailureChunk.frameCount == 0U
		&& streamReadFailureChunk.data.empty(),
		"persistent FFmpeg stream rejects non-EOF read errors without returning partial PCM");
	NativeAudioAssetSourceTestHook::clearFFmpegReadFrameFailure();
	looseStream.reset();
	check(realSource.openPcmStream(
		AsciiString(longAdpcmPath.string().c_str()), looseStream) && looseStream != nullptr,
		"persistent FFmpeg stream can be reopened after a terminal read failure");
#endif
	const std::uint64_t looseFrames = consumePcmStream(*looseStream);
	check(looseFrames >= 4U * 48000U && looseFrames <= 6U * 48000U,
		"persistent filesystem stream drains the complete fixture");

	MemoryVirtualAudioSource longArchive("archive\\long_adpcm.wav", readBinaryFile(longAdpcmPath));
	FileAudioAssetSource longArchiveSource(AsciiString(root.string().c_str()), &longArchive);
	std::unique_ptr<AudioPcmStream> archiveStream;
	check(longArchiveSource.openPcmStream(AsciiString("archive\\long_adpcm.wav"), archiveStream)
		&& archiveStream != nullptr, "archive source opens a persistent FFmpeg PCM stream");
	check(longArchive.getReadCalls() == 1U,
		"archive-backed persistent stream reads its virtual asset exactly once");
	const void *archiveIdentity = longArchiveSource.getFileIdentity(
		AsciiString("archive\\long_adpcm.wav"));
	check(archiveIdentity != nullptr && longArchive.getReadCalls() == 1U
		&& longArchiveSource.matchesFileIdentity(
			AsciiString("archive\\long_adpcm.wav"), archiveIdentity)
		&& longArchive.getReadCalls() == 1U,
		"stream-cached archive identity matches without rereading the asset");
#if defined(RTS_NATIVE_AUDIO_ASSET_SOURCE_TEST_HOOK)
	NativeAudioAssetSourceTestHook::failFFmpegReadFrameAfter(1U);
	AudioPcmChunk boundedReadFailureChunk;
	check(!longArchiveSource.decodePcmAt(AsciiString("archive\\long_adpcm.wav"),
		boundedReadFailureChunk, 48000U, 0U)
		&& boundedReadFailureChunk.frameCount == 0U
		&& boundedReadFailureChunk.data.empty(),
		"bounded FFmpeg fallback rejects non-EOF read errors without returning partial PCM");
	NativeAudioAssetSourceTestHook::clearFFmpegReadFrameFailure();
#endif
	const std::uint64_t archiveFrames = consumePcmStream(*archiveStream);
	check(archiveFrames >= 4U * 48000U && archiveFrames <= 6U * 48000U,
		"persistent archive stream drains the complete fixture");
	// Release decoder/file handles before removing the temporary fixture tree.
	archiveStream.reset();
	looseStream.reset();

#endif
	return 0;
}

int main(int argc, char *argv[])
{
#if defined(_MSC_VER)
	// Keep unexpected CRT failures in the CTest log instead of opening an
	// interactive Visual C++ Runtime or Windows Error Reporting dialog.
	_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
	try {
		return runCatalogTest(argc, argv);
	} catch (const TestFailure &) {
		return EXIT_FAILURE;
	} catch (const std::exception &error) {
		std::fprintf(stderr, "FAIL: catalog test exception: %s\n", error.what());
		return EXIT_FAILURE;
	}
}
