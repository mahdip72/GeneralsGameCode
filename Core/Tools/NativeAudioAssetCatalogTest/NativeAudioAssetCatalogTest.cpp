#include "AudioDevice/AudioAssetSource.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <process.h>
#include <string>
#include <utility>

namespace
{
void check(bool condition, const char *message = "catalog assertion failed");

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
		if (fileName.str() == nullptr || std::string(fileName.str()) != m_name) {
			return FALSE;
		}
		++m_readCalls;
		bytes = m_bytes;
		identity = "archive:" + m_name;
		return TRUE;
	}
	UnsignedInt getReadCalls() const { return m_readCalls; }
	Bool matchesIdentity(const AsciiString &fileName, const void *identity) const override
	{
		return fileName.str() != nullptr && std::string(fileName.str()) == m_name
			&& identity == this;
	}

private:
	std::string m_name;
	std::vector<std::uint8_t> m_bytes;
	mutable UnsignedInt m_readCalls = 0;
};

void writeWaveFile(const std::filesystem::path &path, UnsignedInt durationMS,
	UnsignedInt sampleRate = 48000, UnsignedShort channels = 2)
{
	const UnsignedInt frames = durationMS * sampleRate / 1000U;
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
		std::abort();
	}
}
}

int main(int argc, char *argv[])
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

	const std::filesystem::path root = std::filesystem::temp_directory_path()
		/ "rts-native-audio-real-source";
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

	const std::filesystem::path longPath = root / "long.wav";
	const std::filesystem::path longAdpcmPath = root / "long_adpcm.wav";
	writeWaveFile(longPath, 5000U);
	check(runFFmpeg(ffmpegExecutable, longPath, "adpcm_ima_wav", longAdpcmPath) == 0,
		"FFmpeg creates the persistent-stream ADPCM fixture");
	std::unique_ptr<AudioPcmStream> looseStream;
	check(realSource.openPcmStream(
		AsciiString(longAdpcmPath.string().c_str()), looseStream) && looseStream != nullptr,
		"filesystem source opens a persistent FFmpeg PCM stream");
	check(looseStream->sampleRate() == 48000U && looseStream->durationMS() > 4900.0f
		&& looseStream->durationMS() < 5100.0f,
		"persistent filesystem stream exposes bounded output metadata");
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
	const std::uint64_t archiveFrames = consumePcmStream(*archiveStream);
	check(archiveFrames >= 4U * 48000U && archiveFrames <= 6U * 48000U,
		"persistent archive stream drains the complete fixture");
	// Release decoder/file handles before removing the temporary fixture tree.
	archiveStream.reset();
	looseStream.reset();

#endif
	std::filesystem::remove_all(root);
	return 0;
}
