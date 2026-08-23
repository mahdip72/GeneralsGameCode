#include "AudioDevice/AudioAssetSource.h"

#include <cstdio>
#include <cstdlib>

namespace
{
void check(bool condition, const char *message = "catalog assertion failed")
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
	check(catalog.getDurationMS(AsciiString("attack.wav"), duration) && duration == 100.0f,
		"factory-neutral attack duration is exact");
	check(catalog.getDurationMS(AsciiString("main.wav"), duration) && duration == 400.0f,
		"factory-neutral main duration is exact");
	check(catalog.getDurationMS(AsciiString("decay.wav"), duration) && duration == 50.0f,
		"factory-neutral decay duration is exact");
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
	return 0;
}
