#include "AudioDevice/AudioAssetSource.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace
{
void check(bool condition, const char *message = "catalog assertion failed");

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

int main()
{
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
	catalog.setPcm(AsciiString("voice.wav"), source, 12.0f);
	check(catalog.decodePcm(AsciiString("voice.wav"), pcm, 3));
	check(pcm.sampleRate == 22050);
	check(pcm.channels == 1);
	check(pcm.frameCount == 3);
	check(pcm.data.size() == 3U * 2U);
	check(pcm.data[0] == 0x7fU);
	check(pcm.data[5] == 0x7fU);

	const std::filesystem::path root = std::filesystem::temp_directory_path()
		/ "rts-native-audio-real-source";
	std::filesystem::create_directories(root);
	const std::filesystem::path attackPath = root / "attack.wav";
	const std::filesystem::path mainPath = root / "main.wav";
	const std::filesystem::path decayPath = root / "decay.wav";
	writeWaveFile(attackPath, 125U);
	writeWaveFile(mainPath, 350U);
	writeWaveFile(decayPath, 75U);
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

#if defined(RTS_NATIVE_AUDIO_HAS_FFMPEG)
	const std::filesystem::path genericPath = root / "main.aiff";
	const std::string ffmpegCommand = "ffmpeg -y -v error -i \""
		+ mainPath.string() + "\" -c:a pcm_s16le \"" + genericPath.string() + "\"";
	check(std::system(ffmpegCommand.c_str()) == 0,
		"FFmpeg creates a deterministic non-WAVE audio container");
	check(realSource.getDurationMS(AsciiString(genericPath.string().c_str()), duration)
		&& duration == 350.0f, "FFmpeg source reports exact generic audio duration");
	AudioPcmChunk genericChunk;
	check(realSource.decodePcmAt(AsciiString(genericPath.string().c_str()), genericChunk, 3U, 7U),
		"FFmpeg source decodes generic audio without a video stream");
	check(genericChunk.sampleRate == 48000U && genericChunk.channels == 2U
		&& genericChunk.frameCount == 3U && genericChunk.data.size() == 3U * 2U * sizeof(Short),
		"FFmpeg generic PCM remains bounded 48 kHz stereo s16");
#endif
	std::filesystem::remove_all(root);
	return 0;
}
