#include <Utility/CppMacros.h>
#include "Lib/BaseType.h"

#include "AudioDevice/AudioManagerFactory.h"
#include "AudioDevice/NullAudioManager.h"

#include "Common/AudioAffect.h"
#include "Common/AudioEventInfo.h"
#include "Common/AudioHandleSpecialValues.h"
#include "Common/AudioSettings.h"
#include "Common/GameAudio.h"
#include "XAudio2AudioDevice/IXAudio2AudioEngineBackend.h"
#include "XAudio2AudioDevice/NativeAudioCompatibility.h"
#include "XAudio2AudioDevice/XAudio2AudioManager.h"
#include "XAudio2AudioDevice/XAudio2AudioService.h"
#include "XAudio2AudioDevice/XAudio2PcmVoice.h"

#include <cstdio>
#include <cstdint>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <type_traits>
#include <vector>

class View;
extern View *TheTacticalView;
extern int g_nativeAudioBaseUpdateCalls;
extern Bool g_nativeAudioShroudedForTest;

static_assert(std::is_base_of<AudioManager, NullAudioManager>::value,
	"NullAudioManager must implement the common AudioManager contract");
static_assert(!std::is_base_of<LegacyVideoAudioInterface, NullAudioManager>::value,
	"NullAudioManager must not inherit the legacy video audio contract");

namespace
{
	int failures = 0;

	void check(bool condition, const char *message)
	{
		if (!condition) {
			std::fprintf(stderr, "FAIL: %s\n", message);
			++failures;
		}
	}

	std::uint32_t readEncodedFrame(const std::vector<std::uint8_t> &bytes)
	{
		if (bytes.size() < 4U) {
			return (std::numeric_limits<std::uint32_t>::max)();
		}
		return static_cast<std::uint32_t>(bytes[0])
			| (static_cast<std::uint32_t>(bytes[1]) << 8U)
			| (static_cast<std::uint32_t>(bytes[2]) << 16U)
			| (static_cast<std::uint32_t>(bytes[3]) << 24U);
	}

	void writeWaveFile(const std::filesystem::path &path, UnsignedInt durationMS)
	{
		const UnsignedInt sampleRate = 48000U;
		const UnsignedShort channels = 2U;
		const UnsignedInt bytesPerFrame = channels * sizeof(Short);
		const UnsignedInt frames = durationMS * sampleRate / 1000U;
		std::ofstream output(path, std::ios::binary | std::ios::trunc);
		check(output.good(), "manager real fixture opens for writing");
		auto writeU16 = [&output](UnsignedShort value) {
			output.put(static_cast<char>(value & 0xffU));
			output.put(static_cast<char>((value >> 8U) & 0xffU));
		};
		auto writeU32 = [&output](UnsignedInt value) {
			for (UnsignedInt shift = 0; shift < 32U; shift += 8U) {
				output.put(static_cast<char>((value >> shift) & 0xffU));
			}
		};
		output.write("RIFF", 4);
		writeU32(36U + frames * bytesPerFrame);
		output.write("WAVEfmt ", 8);
		writeU32(16U);
		writeU16(1U);
		writeU16(channels);
		writeU32(sampleRate);
		writeU32(sampleRate * bytesPerFrame);
		writeU16(static_cast<UnsignedShort>(bytesPerFrame));
		writeU16(16U);
		output.write("data", 4);
		writeU32(frames * bytesPerFrame);
		for (UnsignedInt frame = 0; frame < frames; ++frame) {
			writeU16(static_cast<UnsignedShort>(frame & 0x7fffU));
			writeU16(static_cast<UnsignedShort>((frame + 1U) & 0x7fffU));
		}
		check(output.good(), "manager real fixture is fully written");
	}

	class FakeVoice final : public IXAudio2PcmVoiceBackend
	{
	public:
		FakeVoice(Bool failCreate, Bool failSubmit, Bool failStop, Bool failFlush,
			int *totalSubmitCalls) :
			m_failCreate(failCreate),
			m_failSubmit(failSubmit),
			m_failStop(failStop),
			m_failFlush(failFlush),
			m_totalSubmitCalls(totalSubmitCalls)
		{
		}
		HRESULT create(const WAVEFORMATEX &, IXAudio2VoiceCallback *callback) noexcept override
		{
			if (m_failCreate) {
				return E_FAIL;
			}
			m_callback = callback;
			return S_OK;
		}
		HRESULT submit(const XAUDIO2_BUFFER &buffer) noexcept override
		{
			if (m_failSubmit) {
				return E_FAIL;
			}
			m_lastContext = buffer.pContext;
			m_pendingContexts.push_back(buffer.pContext);
			if (buffer.pAudioData != nullptr && buffer.AudioBytes != 0) {
				submittedAudio.emplace_back(buffer.pAudioData,
					buffer.pAudioData + buffer.AudioBytes);
			} else {
				submittedAudio.emplace_back();
			}
			++submitCalls;
			if (m_totalSubmitCalls != nullptr) {
				++(*m_totalSubmitCalls);
			}
			return S_OK;
		}
		HRESULT start() noexcept override { return S_OK; }
		HRESULT stop() noexcept override { return m_failStop ? E_FAIL : S_OK; }
		HRESULT flush() noexcept override { return m_failFlush ? E_FAIL : S_OK; }
		HRESULT setVolume(float volume) noexcept override
		{
			lastVolume = volume;
			++volumeCalls;
			return S_OK;
		}
		HRESULT setOutputMatrix(UINT32 sourceChannels, UINT32 destinationChannels,
			const float *matrix) noexcept override
		{
			matrixSourceChannels = sourceChannels;
			matrixDestinationChannels = destinationChannels;
			lastMatrix.assign(matrix, matrix + sourceChannels * destinationChannels);
			++matrixCalls;
			return S_OK;
		}
		HRESULT pause() noexcept override { return S_OK; }
		HRESULT resume() noexcept override { return S_OK; }
		HRESULT getCriticalError() const noexcept override { return S_OK; }
		void destroy() noexcept override { m_callback = nullptr; }
		Bool completeLastBuffer() noexcept
		{
			if (m_callback == nullptr || m_pendingContexts.empty()) {
				return FALSE;
			}
			void *context = m_pendingContexts.back();
			m_pendingContexts.pop_back();
			if (context == m_lastContext) {
				m_lastContext = m_pendingContexts.empty() ? nullptr : m_pendingContexts.back();
			}
			m_callback->OnBufferEnd(context);
			return TRUE;
		}
		Bool completeOldestBuffer() noexcept
		{
			if (m_callback == nullptr || m_pendingContexts.empty()) {
				return FALSE;
			}
			void *context = m_pendingContexts.front();
			m_pendingContexts.erase(m_pendingContexts.begin());
			if (context == m_lastContext) {
				m_lastContext = m_pendingContexts.empty() ? nullptr : m_pendingContexts.back();
			}
			m_callback->OnBufferEnd(context);
			return TRUE;
		}
		int submitCalls = 0;
		float lastVolume = 0.0f;
		int volumeCalls = 0;
		UINT32 matrixSourceChannels = 0;
		UINT32 matrixDestinationChannels = 0;
		int matrixCalls = 0;
		std::vector<float> lastMatrix;
		std::vector<std::vector<std::uint8_t>> submittedAudio;

	private:
		Bool m_failCreate;
		Bool m_failSubmit;
		Bool m_failStop;
		Bool m_failFlush;
		int *m_totalSubmitCalls;
		IXAudio2VoiceCallback *m_callback = nullptr;
		void *m_lastContext = nullptr;
		std::vector<void *> m_pendingContexts;
	};

	class FakeEngine final : public IXAudio2AudioEngineBackend
	{
	public:
		HRESULT open(CriticalErrorCallback callback, void *context) noexcept override
		{
			++openCalls;
			m_callback = callback;
			m_context = context;
			return S_OK;
		}
		HRESULT start() noexcept override { ++startCalls; return S_OK; }
		HRESULT getOutputDetails(XAudio2OutputDetails &details) const noexcept override
		{
			details.channelMask = SPEAKER_STEREO;
			details.channelCount = 2;
			return S_OK;
		}
		HRESULT createPcmVoice(std::unique_ptr<IXAudio2PcmVoiceBackend> &voice) noexcept override
		{
			if (failCreateVoice) {
				return E_FAIL;
			}
			std::unique_ptr<FakeVoice> created = std::make_unique<FakeVoice>(
				failVoiceCreate, failSubmit, failStop, failFlush, &totalSubmitCalls);
			lastVoice = created.get();
			voices.push_back(lastVoice);
			voice = std::move(created);
			return S_OK;
		}
		HRESULT stop() noexcept override { ++stopCalls; return S_OK; }
		HRESULT close() noexcept override { ++closeCalls; return S_OK; }
		void emitCritical(HRESULT error) noexcept
		{
			if (m_callback != nullptr) {
				m_callback(m_context, error);
			}
		}
		FakeVoice *lastVoice = nullptr;
		std::vector<FakeVoice *> voices;
		Bool failCreateVoice = FALSE;
		Bool failVoiceCreate = FALSE;
		Bool failSubmit = FALSE;
		Bool failStop = FALSE;
		Bool failFlush = FALSE;
		int totalSubmitCalls = 0;
		int openCalls = 0;
		int startCalls = 0;
		int stopCalls = 0;
		int closeCalls = 0;

	private:
		CriticalErrorCallback m_callback = nullptr;
		void *m_context = nullptr;
	};

	class FixtureEvent final : public AudioEventRTS
	{
	public:
		explicit FixtureEvent(const AsciiString &name) : AudioEventRTS(name) {}
		void setDelayForTest(Real delay) { m_delay = delay; }
	};

	class CountingPcmStream final : public AudioPcmStream
	{
	public:
		CountingPcmStream(UnsignedInt totalFrames, std::vector<UnsignedInt> &readStarts,
			std::vector<UnsignedInt> &readBounds) :
			m_totalFrames(totalFrames),
			m_readStarts(readStarts),
			m_readBounds(readBounds)
		{
		}

		UnsignedInt sampleRate() const override { return 48000U; }
		Real durationMS() const override
		{
			return static_cast<Real>(m_totalFrames) * 1000.0f / 48000.0f;
		}
		Bool readPcm(AudioPcmChunk &chunk, UnsignedInt maxFrames) override
		{
			m_readStarts.push_back(m_nextFrame);
			m_readBounds.push_back(maxFrames);
			if (maxFrames == 0 || m_nextFrame >= m_totalFrames) {
				chunk = {};
				return FALSE;
			}
			const UnsignedInt frameCount = std::min(maxFrames, m_totalFrames - m_nextFrame);
			chunk.sampleRate = 48000U;
			chunk.channels = 2U;
			chunk.format = AudioPcmFormat::SIGNED_16_INTERLEAVED_LITTLE_ENDIAN;
			chunk.frameCount = frameCount;
			chunk.startSample = static_cast<std::int64_t>(m_nextFrame);
			chunk.data.assign(static_cast<std::size_t>(frameCount) * 4U, 0x5aU);
			m_nextFrame += frameCount;
			return TRUE;
		}

	private:
		UnsignedInt m_totalFrames;
		UnsignedInt m_nextFrame = 0;
		std::vector<UnsignedInt> &m_readStarts;
		std::vector<UnsignedInt> &m_readBounds;
	};

	class StreamingAudioAssetSource final : public AudioAssetSource
	{
	public:
		explicit StreamingAudioAssetSource(UnsignedInt totalFrames) : m_totalFrames(totalFrames) {}

		Bool getDurationMS(const AsciiString &, Real &durationMS) const override
		{
			++getDurationCalls;
			durationMS = static_cast<Real>(m_totalFrames) * 1000.0f / 48000.0f;
			return TRUE;
		}
		Bool decodePcm(const AsciiString &, AudioPcmChunk &chunk, UnsignedInt) const override
		{
			++decodePcmCalls;
			chunk = {};
			return FALSE;
		}
		Bool decodePcmAt(const AsciiString &, AudioPcmChunk &chunk,
			UnsignedInt, UnsignedInt) const override
		{
			++decodePcmAtCalls;
			chunk = {};
			return FALSE;
		}
		Bool openPcmStream(const AsciiString &, std::unique_ptr<AudioPcmStream> &stream) const override
		{
			++openStreamCalls;
			stream = std::make_unique<CountingPcmStream>(m_totalFrames, readStarts, readBounds);
			return TRUE;
		}
		const void *getFileIdentity(const AsciiString &) const override
		{
			return this;
		}

		mutable int openStreamCalls = 0;
		mutable int getDurationCalls = 0;
		mutable int decodePcmCalls = 0;
		mutable int decodePcmAtCalls = 0;
		mutable std::vector<UnsignedInt> readStarts;
		mutable std::vector<UnsignedInt> readBounds;

	private:
		UnsignedInt m_totalFrames;
	};

	class LegacyAudioAssetSource final : public AudioAssetSource
	{
	public:
		explicit LegacyAudioAssetSource(UnsignedInt totalFrames, Bool materialize = TRUE,
			UnsignedInt sampleRate = 48000U, UnsignedShort channels = 2U) :
			m_totalFrames(totalFrames), m_materialize(materialize), m_sampleRate(sampleRate),
			m_channels(channels)
		{
		}

		Bool getDurationMS(const AsciiString &, Real &durationMS) const override
		{
			durationMS = static_cast<Real>(m_totalFrames) * 1000.0f
				/ static_cast<Real>(m_sampleRate);
			return TRUE;
		}
		Bool decodePcm(const AsciiString &, AudioPcmChunk &chunk,
			UnsignedInt maxFrames) const override
		{
			decodeBounds.push_back(maxFrames);
			if (maxFrames == 0) {
				chunk = {};
				return FALSE;
			}
			if (!m_materialize && maxFrames > 1U) {
				chunk = {};
				return FALSE;
			}
			const UnsignedInt frameCount = std::min(maxFrames, m_totalFrames);
			chunk.sampleRate = m_sampleRate;
			chunk.channels = m_channels;
			chunk.format = AudioPcmFormat::SIGNED_16_INTERLEAVED_LITTLE_ENDIAN;
			chunk.frameCount = frameCount;
			chunk.startSample = 0;
			const std::size_t bytesPerFrame = static_cast<std::size_t>(m_channels) * sizeof(Short);
			chunk.data.assign(static_cast<std::size_t>(frameCount) * bytesPerFrame, 0U);
			for (UnsignedInt frame = 0; frame < frameCount; ++frame) {
				for (std::size_t byte = 0; byte < std::min<std::size_t>(4U, bytesPerFrame); ++byte) {
					chunk.data[static_cast<std::size_t>(frame) * bytesPerFrame + byte]
						= static_cast<std::uint8_t>((frame >> (byte * 8U)) & 0xffU);
				}
			}
			return TRUE;
		}
		const void *getFileIdentity(const AsciiString &) const override
		{
			return this;
		}

		mutable std::vector<UnsignedInt> decodeBounds;

	private:
		UnsignedInt m_totalFrames;
		Bool m_materialize;
		UnsignedInt m_sampleRate;
		UnsignedShort m_channels;
	};

	class BoundedRangeAudioAssetSource final : public AudioAssetSource
	{
	public:
		explicit BoundedRangeAudioAssetSource(UnsignedInt totalFrames) :
			m_totalFrames(totalFrames)
		{
		}

		Bool getDurationMS(const AsciiString &, Real &durationMS) const override
		{
			durationMS = static_cast<Real>(m_totalFrames) * 1000.0f / 48000.0f;
			return TRUE;
		}
		Bool decodePcm(const AsciiString &, AudioPcmChunk &chunk,
			UnsignedInt maxFrames) const override
		{
			decodePcmBounds.push_back(maxFrames);
			if (maxFrames > 48000U) {
				chunk = {};
				return FALSE;
			}
			return decodeRange(chunk, maxFrames, 0);
		}
		Bool decodePcmAt(const AsciiString &, AudioPcmChunk &chunk,
			UnsignedInt maxFrames, UnsignedInt startFrame) const override
		{
			decodeStarts.push_back(startFrame);
			decodeRangeBounds.push_back(maxFrames);
			return decodeRange(chunk, maxFrames, startFrame);
		}
		Bool supportsPcmRangeDecode() const override { return TRUE; }

		mutable std::vector<UnsignedInt> decodePcmBounds;
		mutable std::vector<UnsignedInt> decodeStarts;
		mutable std::vector<UnsignedInt> decodeRangeBounds;

	private:
		Bool decodeRange(AudioPcmChunk &chunk, UnsignedInt maxFrames,
			UnsignedInt startFrame) const
		{
			if (maxFrames == 0 || startFrame >= m_totalFrames) {
				chunk = {};
				return FALSE;
			}
			const UnsignedInt frameCount = std::min(maxFrames, m_totalFrames - startFrame);
			chunk.sampleRate = 48000U;
			chunk.channels = 2U;
			chunk.format = AudioPcmFormat::SIGNED_16_INTERLEAVED_LITTLE_ENDIAN;
			chunk.frameCount = frameCount;
			chunk.startSample = static_cast<std::int64_t>(startFrame);
			chunk.data.resize(static_cast<std::size_t>(frameCount) * 4U);
			for (UnsignedInt frame = 0; frame < frameCount; ++frame) {
				const UnsignedInt absoluteFrame = startFrame + frame;
				for (UnsignedInt byte = 0; byte < 4U; ++byte) {
					chunk.data[static_cast<std::size_t>(frame) * 4U + byte]
						= static_cast<std::uint8_t>((absoluteFrame >> (byte * 8U)) & 0xffU);
				}
			}
			return TRUE;
		}

		UnsignedInt m_totalFrames;
	};
}

int main()
{
#if defined(_MSC_VER)
	_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
	const std::filesystem::path realRoot = std::filesystem::temp_directory_path()
		/ "rts-native-audio-manager-real-source";
	std::filesystem::create_directories(realRoot);
	const std::filesystem::path realAttack = realRoot / "attack.wav";
	const std::filesystem::path realMain = realRoot / "main.wav";
	const std::filesystem::path realDecay = realRoot / "decay.wav";
	const std::filesystem::path realReplacement = realRoot / "replacement.wav";
	writeWaveFile(realAttack, 100U);
	writeWaveFile(realMain, 400U);
	writeWaveFile(realDecay, 50U);
	writeWaveFile(realReplacement, 100U);

	AudioManager *dummy = AudioManagerFactory::create(true);
	check(dummy != nullptr && dummy->getDevice() == nullptr,
		"factory dummy remains device-free");
	check(dummy != nullptr && dummy->getLegacyVideoAudioInterface() == nullptr,
		"x64 factory dummy exposes no legacy video-audio capability");
	delete dummy;
	AudioManager *native = AudioManagerFactory::create(false);
	XAudio2AudioManager *nativeManager = dynamic_cast<XAudio2AudioManager *>(native);
	check(native != nullptr && native->getLegacyVideoAudioInterface() == nullptr,
		"x64 factory native manager exposes no legacy video-audio capability");
	check(nativeManager != nullptr && nativeManager->getAssetSource() != nullptr,
		"x64 factory wires a neutral production asset source into the native manager");
	check(nativeManager != nullptr
		&& dynamic_cast<FileAudioAssetSource *>(nativeManager->getAssetSource()) != nullptr,
		"x64 factory selects the filesystem/container decoder rather than a synthetic catalog");
	check(nativeManager != nullptr
		&& nativeManager->getFileLengthMS(AsciiString(realAttack.string().c_str())) == 100.0f,
		"factory-created native manager exposes exact real-file script duration");
	delete native;

	NullAudioManager nullManager;
	check(nullManager.getDevice() == nullptr, "NullAudioManager has no device");
	check(nullManager.getLegacyVideoAudioInterface() == nullptr,
		"NullAudioManager exposes no legacy video-audio capability");
	check(dynamic_cast<FileAudioAssetSource *>(nullManager.getAssetSource()) != nullptr,
		"NullAudioManager uses the same production-neutral asset source");
	check(nullManager.getFileLengthMS(AsciiString(realAttack.string().c_str())) == 100.0f,
		"NullAudioManager exposes exact real-file script duration");
	nullManager.update();
	nullManager.reset();

	std::unique_ptr<FakeEngine> ownedEngine = std::make_unique<FakeEngine>();
	FakeEngine *engine = ownedEngine.get();
	XAudio2AudioService service(std::move(ownedEngine));
	AudioAssetCatalog catalog;
	catalog.setDurationMS(AsciiString("attack.wav"), 100.0f);
	catalog.setDurationMS(AsciiString("main.wav"), 400.0f);
	catalog.setDurationMS(AsciiString("decay.wav"), 50.0f);
	catalog.setDurationMS(AsciiString("long.wav"), 2500.0f);
	XAudio2AudioManager manager(&service, &catalog);
	manager.openDevice();
	check(manager.isOpen(), "manager opens the injected device-free service");
	TheTacticalView = reinterpret_cast<View *>(static_cast<std::uintptr_t>(1));
	g_nativeAudioBaseUpdateCalls = 0;
	manager.update();
	check(g_nativeAudioBaseUpdateCalls == 1 && manager.getZoomVolume() == 0.5f,
		"manager update runs the base listener/zoom phase exactly once after service work");
	TheTacticalView = nullptr;
	check(manager.runInjectedPlaybackProbe(AsciiString("main.wav")),
		"manager submits an injected phase through its generation-reset path");
	check(engine->totalSubmitCalls == 1,
		"fake service observes one bounded manager PCM submission");

	AudioEventInfo *fixtureInfo = newInstance(AudioEventInfo);
	fixtureInfo->m_soundType = AT_SoundEffect;
	fixtureInfo->m_volume = 1.0f;
	fixtureInfo->m_volumeShift = 1.0f;
	fixtureInfo->m_minVolume = 0.0f;
	fixtureInfo->m_limit = 4;
	fixtureInfo->m_priority = AP_NORMAL;
	fixtureInfo->m_type = ST_WORLD;
	fixtureInfo->m_control = 0;
	fixtureInfo->m_loopCount = 0;
	fixtureInfo->m_minDistance = 0.0f;
	fixtureInfo->m_maxDistance = 100.0f;
	fixtureInfo->m_sounds.push_back(AsciiString("main.wav"));
	fixtureInfo->m_attackSounds.push_back(AsciiString("attack.wav"));
	fixtureInfo->m_decaySounds.push_back(AsciiString("decay.wav"));

	AudioSettings admissionSettings;
	admissionSettings.m_minVolume = 0.5f;
	manager.setAudioSettingsForTest(&admissionSettings);
	manager.setAudioEventVolumeOverride(AsciiString("admission-muted"), 0.25f);
	AudioEventInfo *admissionInfo = newInstance(AudioEventInfo);
	*admissionInfo = *fixtureInfo;
	admissionInfo->m_audioName = AsciiString("admission-muted");
	FixtureEvent admissionEvent(AsciiString("admission-muted"));
	admissionEvent.setAudioEventInfo(admissionInfo);
	check(manager.addAudioEvent(&admissionEvent) == AHSV_Muted,
		"common admission rejects adjusted events below the configured minimum volume");
	manager.setAudioEventVolumeOverride(AsciiString("admission-muted"), -1.0f);
	manager.setOn(FALSE, AudioAffect_Sound);
	FixtureEvent disabledEvent(AsciiString("admission-disabled"));
	disabledEvent.setAudioEventInfo(fixtureInfo);
	check(manager.addAudioEvent(&disabledEvent) == AHSV_NoSound,
		"common admission honors the sound affect switch before native queueing");
	manager.setOn(TRUE, AudioAffect_Sound);
	manager.setAudioSettingsForTest(nullptr);
	deleteInstance(admissionInfo);
	AudioEventInfo *shroudedInfo = newInstance(AudioEventInfo);
	*shroudedInfo = *fixtureInfo;
	shroudedInfo->m_type = ST_WORLD | ST_SHROUDED;
	FixtureEvent shroudedEvent(AsciiString("fixture-shrouded"));
	shroudedEvent.setAudioEventInfo(shroudedInfo);
	Coord3D shroudedPosition;
	shroudedPosition.set(10.0f, 0.0f, 0.0f);
	shroudedEvent.setPosition(&shroudedPosition);
	g_nativeAudioShroudedForTest = TRUE;
	check(manager.addAudioEvent(&shroudedEvent) == AHSV_NotForLocal,
		"shrouded positional admission is culled before consuming a channel");
	g_nativeAudioShroudedForTest = FALSE;
	deleteInstance(shroudedInfo);
	FixtureEvent outOfRangeEvent(AsciiString("fixture-out-of-range"));
	outOfRangeEvent.setAudioEventInfo(fixtureInfo);
	Coord3D outOfRangePosition;
	outOfRangePosition.set(100.0f, 0.0f, 0.0f);
	outOfRangeEvent.setPosition(&outOfRangePosition);
	check(manager.addAudioEvent(&outOfRangeEvent) == AHSV_NotForLocal,
		"maximum-range positional admission is rejected before channel allocation");

	FixtureEvent delayedEvent(AsciiString("fixture-delayed"));
	delayedEvent.setAudioEventInfo(fixtureInfo);
	delayedEvent.setDelayForTest(1.0f);
	const AudioHandle delayedHandle = manager.addAudioEvent(&delayedEvent);
	check(delayedHandle != AHSV_NoSound && delayedHandle != AHSV_Error,
		"manager admits a real delayed event through common admission semantics");
	manager.update();
	check(manager.getPendingAudioRequestCount() == 1
		&& manager.isCurrentlyPlaying(delayedHandle),
		"positive residual delay remains pending while its handle stays generation-valid");
	manager.pauseAudio(AudioAffect_Sound);
	check(manager.getPendingAudioRequestCount() == 1 && manager.isCurrentlyPlaying(delayedHandle),
		"pausing one affect preserves its pending request for resume");

	Coord3D positionalPosition;
	positionalPosition.set(10.0f, 0.0f, 0.0f);
	FixtureEvent positionalEvent(AsciiString("fixture-positional"));
	positionalEvent.setAudioEventInfo(fixtureInfo);
	positionalEvent.setPosition(&positionalPosition);
	const AudioHandle positionalHandle = manager.addAudioEvent(&positionalEvent);
	manager.update();
	check(positionalHandle != AHSV_NoSound && manager.getPendingAudioRequestCount() == 1
		&& manager.getActiveAudioCount() == 1,
		"unrelated positional request is not deleted by a sound pause");
	check(delayedEvent.getPlayingAudioIndex() >= 0,
		"common admission updates the caller event's selected sound index");
	FakeVoice *positionalVoice = engine->lastVoice;
	manager.update();
	manager.pauseAudio(AudioAffect_Sound3D);
	manager.resumeAudio(AudioAffect_Sound);
	manager.update();
	FakeVoice *delayedVoice = engine->lastVoice;
	manager.update();
	check(delayedVoice != nullptr && delayedVoice->submitCalls >= 1
		&& manager.getActiveAudioCount() == 2
		&& manager.isCurrentlyPlaying(delayedHandle),
		"resumed delayed event submits its attack phase while the 3D event stays paused");

	const int pausedSubmitCalls = positionalVoice == nullptr ? 0 : positionalVoice->submitCalls;
	check(positionalVoice != nullptr && positionalVoice->completeLastBuffer(),
		"fake backend publishes completion for a paused positional phase");
	manager.update();
	check(manager.getActiveAudioCount() == 2 && manager.isCurrentlyPlaying(positionalHandle)
		&& positionalVoice->submitCalls == pausedSubmitCalls,
		"paused records do not advance phases on completion");
	manager.resumeAudio(AudioAffect_Sound3D);
	manager.update();
	manager.update();
	check(positionalVoice->submitCalls >= 2,
		"resuming the exact affect advances the positional event to its next phase");

	check(delayedVoice->completeLastBuffer(), "fake backend completes the attack phase");
	manager.update();
	manager.update();
	check(delayedVoice->submitCalls >= 2,
		"manager continues the event with its main phase after completion");
	check(delayedVoice->completeLastBuffer(), "fake backend completes the main phase");
	manager.update();
	manager.update();
	check(delayedVoice->submitCalls >= 3,
		"manager continues the event with its decay phase after completion");
	check(delayedVoice->completeLastBuffer(), "fake backend completes the decay phase");
	manager.update();
	check(!manager.isCurrentlyPlaying(delayedHandle) && manager.getActiveAudioCount() == 1,
		"completed handle is no longer reported as playing");

	AudioEventInfo *longInfo = newInstance(AudioEventInfo);
	*longInfo = *fixtureInfo;
	longInfo->m_attackSounds.clear();
	longInfo->m_decaySounds.clear();
	longInfo->m_sounds.clear();
	longInfo->m_sounds.push_back(AsciiString("long.wav"));
	FixtureEvent longEvent(AsciiString("fixture-long"));
	longEvent.setAudioEventInfo(longInfo);
	const AudioHandle longHandle = manager.addAudioEvent(&longEvent);
	manager.update();
	manager.update();
	FakeVoice *longVoice = engine->lastVoice;
	check(longVoice != nullptr && longVoice->submitCalls == 2,
		"duration-only long asset starts with two bounded PCM chunks");
	check(longVoice->completeOldestBuffer(), "fake backend completes the oldest long-asset chunk");
	manager.update();
	check(longVoice->submitCalls == 3,
		"oldest completion deterministically refills the low-water queue");
	manager.stopAudio(AudioAffect_Sound);
	manager.update();
	check(!manager.isCurrentlyPlaying(longHandle),
		"stopping the long asset clears its generation-aware handle state");
	deleteInstance(longInfo);

	StreamingAudioAssetSource streamingSource(120000U);
	std::unique_ptr<FakeEngine> streamingOwnedEngine = std::make_unique<FakeEngine>();
	FakeEngine *streamingEngine = streamingOwnedEngine.get();
	XAudio2AudioService streamingService(std::move(streamingOwnedEngine));
	XAudio2AudioManager streamingManager(&streamingService, &streamingSource);
	streamingManager.openDevice();
	AudioEventInfo *streamingInfo = newInstance(AudioEventInfo);
	*streamingInfo = *fixtureInfo;
	streamingInfo->m_attackSounds.clear();
	streamingInfo->m_decaySounds.clear();
	streamingInfo->m_sounds.clear();
	streamingInfo->m_sounds.push_back(AsciiString("stream.wav"));
	FixtureEvent streamingEvent(AsciiString("fixture-stream"));
	streamingEvent.setAudioEventInfo(streamingInfo);
	const AudioHandle streamingHandle = streamingManager.addAudioEvent(&streamingEvent);
	streamingManager.update();
	streamingManager.update();
	FakeVoice *streamingVoice = streamingEngine->lastVoice;
	check(streamingVoice != nullptr && streamingVoice->submitCalls == 2
		&& streamingSource.openStreamCalls == 1
		&& streamingSource.decodePcmCalls == 0 && streamingSource.decodePcmAtCalls == 0
		&& streamingSource.readStarts.size() == 2
		&& streamingSource.readStarts[0] == 0U
		&& streamingSource.readStarts[1] == 48000U
		&& streamingSource.readBounds[0] == 48000U
		&& streamingSource.readBounds[1] == 48000U,
		"long playback opens one sequential PCM stream and prequeues its first two chunks");
	check(streamingVoice != nullptr && streamingVoice->completeOldestBuffer(),
		"streaming fixture completes its oldest queued chunk");
	streamingManager.update();
	check(streamingVoice != nullptr && streamingVoice->submitCalls == 3
		&& streamingSource.readStarts.size() == 3
		&& streamingSource.readStarts[2] == 96000U
		&& streamingSource.decodePcmCalls == 0 && streamingSource.decodePcmAtCalls == 0,
		"streaming completion reads the next linear chunk without decode fallback");
	streamingManager.stopAudio(AudioAffect_Sound);
	streamingManager.update();
	check(!streamingManager.isCurrentlyPlaying(streamingHandle),
		"stopping streaming playback releases its stream-backed handle");
	streamingManager.closeDevice();
	deleteInstance(streamingInfo);

	BoundedRangeAudioAssetSource rangedSource(120000U);
	std::unique_ptr<FakeEngine> rangedOwnedEngine = std::make_unique<FakeEngine>();
	FakeEngine *rangedEngine = rangedOwnedEngine.get();
	XAudio2AudioService rangedService(std::move(rangedOwnedEngine));
	XAudio2AudioManager rangedManager(&rangedService, &rangedSource);
	rangedManager.openDevice();
	AudioEventInfo *rangedInfo = newInstance(AudioEventInfo);
	*rangedInfo = *fixtureInfo;
	rangedInfo->m_attackSounds.clear();
	rangedInfo->m_decaySounds.clear();
	rangedInfo->m_sounds.clear();
	rangedInfo->m_sounds.push_back(AsciiString("ranged.wav"));
	FixtureEvent rangedEvent(AsciiString("fixture-ranged"));
	rangedEvent.setAudioEventInfo(rangedInfo);
	const AudioHandle rangedHandle = rangedManager.addAudioEvent(&rangedEvent);
	rangedManager.update();
	rangedManager.update();
	FakeVoice *rangedVoice = rangedEngine->lastVoice;
	check(rangedVoice != nullptr && rangedVoice->submitCalls == 2
		&& rangedManager.isCurrentlyPlaying(rangedHandle)
		&& rangedSource.decodePcmBounds.empty()
		&& rangedSource.decodeStarts.size() == 3U
		&& rangedSource.decodeStarts[0] == 0U
		&& rangedSource.decodeStarts[1] == 0U
		&& rangedSource.decodeStarts[2] == 48000U
		&& rangedVoice->submittedAudio.size() == 2U
		&& readEncodedFrame(rangedVoice->submittedAudio[0]) == 0U
		&& readEncodedFrame(rangedVoice->submittedAudio[1]) == 48000U,
		"bounded range sources keep using their efficient chunk decoder when stream opening fails");
	check(rangedVoice != nullptr && rangedVoice->completeOldestBuffer(),
		"bounded range fixture completes its oldest queued chunk");
	rangedManager.update();
	check(rangedVoice != nullptr && rangedVoice->submitCalls == 3
		&& rangedSource.decodeStarts.size() == 4U
		&& rangedSource.decodeStarts[3] == 96000U
		&& rangedVoice->submittedAudio.size() == 3U
		&& readEncodedFrame(rangedVoice->submittedAudio[2]) == 96000U,
		"bounded range playback continues without an oversized full-source decode");
	rangedManager.stopAudio(AudioAffect_Sound);
	rangedManager.update();
	rangedManager.closeDevice();
	deleteInstance(rangedInfo);

	LegacyAudioAssetSource legacySource(120000U);
	std::unique_ptr<FakeEngine> legacyOwnedEngine = std::make_unique<FakeEngine>();
	FakeEngine *legacyEngine = legacyOwnedEngine.get();
	XAudio2AudioService legacyService(std::move(legacyOwnedEngine));
	XAudio2AudioManager legacyManager(&legacyService, &legacySource);
	legacyManager.openDevice();
	AudioEventInfo *legacyInfo = newInstance(AudioEventInfo);
	*legacyInfo = *fixtureInfo;
	legacyInfo->m_attackSounds.clear();
	legacyInfo->m_decaySounds.clear();
	legacyInfo->m_sounds.clear();
	legacyInfo->m_sounds.push_back(AsciiString("legacy.wav"));
	FixtureEvent legacyEvent(AsciiString("fixture-legacy"));
	legacyEvent.setAudioEventInfo(legacyInfo);
	const AudioHandle legacyHandle = legacyManager.addAudioEvent(&legacyEvent);
	legacyManager.update();
	legacyManager.update();
	FakeVoice *legacyVoice = legacyEngine->lastVoice;
	check(legacyVoice != nullptr && legacyVoice->submitCalls == 2
		&& legacyManager.isCurrentlyPlaying(legacyHandle)
		&& legacySource.decodeBounds.size() == 2U
		&& legacySource.decodeBounds[0] == 1U
		&& legacySource.decodeBounds[1] == 120000U
		&& legacyVoice->submittedAudio.size() == 2U
		&& legacyVoice->submittedAudio[0].size() == 48000U * 4U
		&& legacyVoice->submittedAudio[1].size() == 48000U * 4U
		&& readEncodedFrame(legacyVoice->submittedAudio[0]) == 0U
		&& readEncodedFrame(legacyVoice->submittedAudio[1]) == 48000U,
		"legacy playback materializes one bounded phase instead of decoding growing prefixes");
	check(legacyVoice != nullptr && legacyVoice->completeOldestBuffer(),
		"legacy buffered fixture completes its oldest queued chunk");
	legacyManager.update();
	check(legacyVoice != nullptr && legacyVoice->submitCalls == 3
		&& legacySource.decodeBounds.size() == 2U
		&& legacyVoice->submittedAudio.size() == 3U
		&& legacyVoice->submittedAudio[2].size() == 24000U * 4U
		&& readEncodedFrame(legacyVoice->submittedAudio[2]) == 96000U,
		"legacy buffered completion reuses the phase without another source decode");
	LegacyAudioAssetSource replacementLegacySource(96000U);
	legacyManager.setAssetSource(&replacementLegacySource);
	check(legacyManager.getActiveAudioCount() == 0
		&& !legacyManager.isCurrentlyPlaying(legacyHandle)
		&& legacySource.decodeBounds.size() == 2U,
		"replacing the source releases an active compatibility buffer without another decode");
	FixtureEvent replacementLegacyEvent(AsciiString("fixture-replacement-legacy"));
	replacementLegacyEvent.setAudioEventInfo(legacyInfo);
	const AudioHandle replacementLegacyHandle = legacyManager.addAudioEvent(&replacementLegacyEvent);
	legacyManager.update();
	legacyManager.update();
	check(legacyManager.isCurrentlyPlaying(replacementLegacyHandle)
		&& replacementLegacySource.decodeBounds.size() == 2U
		&& replacementLegacySource.decodeBounds[0] == 1U
		&& replacementLegacySource.decodeBounds[1] == 96000U,
		"legacy admission resumes with a fresh bounded buffer after source replacement");
	legacyManager.closeDevice();
	deleteInstance(legacyInfo);

	LegacyAudioAssetSource oversizedLegacySource(19200000U, FALSE);
	std::unique_ptr<FakeEngine> oversizedOwnedEngine = std::make_unique<FakeEngine>();
	FakeEngine *oversizedEngine = oversizedOwnedEngine.get();
	XAudio2AudioService oversizedService(std::move(oversizedOwnedEngine));
	XAudio2AudioManager oversizedManager(&oversizedService, &oversizedLegacySource);
	oversizedManager.openDevice();
	AudioEventInfo *oversizedInfo = newInstance(AudioEventInfo);
	*oversizedInfo = *fixtureInfo;
	oversizedInfo->m_attackSounds.clear();
	oversizedInfo->m_decaySounds.clear();
	oversizedInfo->m_sounds.clear();
	oversizedInfo->m_sounds.push_back(AsciiString("oversized-legacy.wav"));
	FixtureEvent oversizedEvent(AsciiString("fixture-oversized-legacy"));
	oversizedEvent.setAudioEventInfo(oversizedInfo);
	const AudioHandle oversizedHandle = oversizedManager.addAudioEvent(&oversizedEvent);
	oversizedManager.update();
	check(!oversizedManager.isCurrentlyPlaying(oversizedHandle),
		"oversized legacy playback releases its admitted handle");
	check(oversizedEngine->lastVoice == nullptr,
		"oversized legacy playback fails before creating a voice");
	check(oversizedLegacySource.decodeBounds.size() == 1U
		&& oversizedLegacySource.decodeBounds[0] == 1U,
		"oversized legacy playback performs only its bounded format probe");
	oversizedManager.closeDevice();
	deleteInstance(oversizedInfo);

	check(NativeAudioCompatibility::canBufferPcm(16777216U, 2U),
		"legacy compatibility admission accepts an exact 64 MiB stereo PCM phase");
	check(!NativeAudioCompatibility::canBufferPcm(16777217U, 2U),
		"legacy compatibility admission rejects the first frame beyond 64 MiB");

	LegacyAudioAssetSource malformedLegacySource(120000U, FALSE, 44100U, 2U);
	std::unique_ptr<FakeEngine> malformedOwnedEngine = std::make_unique<FakeEngine>();
	FakeEngine *malformedEngine = malformedOwnedEngine.get();
	XAudio2AudioService malformedService(std::move(malformedOwnedEngine));
	XAudio2AudioManager malformedManager(&malformedService, &malformedLegacySource);
	malformedManager.openDevice();
	AudioEventInfo *malformedInfo = newInstance(AudioEventInfo);
	*malformedInfo = *fixtureInfo;
	malformedInfo->m_attackSounds.clear();
	malformedInfo->m_decaySounds.clear();
	malformedInfo->m_sounds.clear();
	malformedInfo->m_sounds.push_back(AsciiString("malformed-legacy.wav"));
	FixtureEvent malformedEvent(AsciiString("fixture-malformed-legacy"));
	malformedEvent.setAudioEventInfo(malformedInfo);
	const AudioHandle malformedHandle = malformedManager.addAudioEvent(&malformedEvent);
	malformedManager.update();
	check(!malformedManager.isCurrentlyPlaying(malformedHandle)
		&& malformedEngine->lastVoice == nullptr
		&& malformedLegacySource.decodeBounds.size() == 1U
		&& malformedLegacySource.decodeBounds[0] == 1U,
		"non-shipping legacy PCM fails after its probe and before phase materialization");
	malformedManager.closeDevice();
	deleteInstance(malformedInfo);

	LegacyAudioAssetSource budgetedLegacySource(10000000U);
	std::unique_ptr<FakeEngine> budgetOwnedEngine = std::make_unique<FakeEngine>();
	XAudio2AudioService budgetService(std::move(budgetOwnedEngine));
	XAudio2AudioManager budgetManager(&budgetService, &budgetedLegacySource);
	budgetManager.openDevice();
	AudioEventInfo *budgetInfo = newInstance(AudioEventInfo);
	*budgetInfo = *fixtureInfo;
	budgetInfo->m_attackSounds.clear();
	budgetInfo->m_decaySounds.clear();
	budgetInfo->m_sounds.clear();
	budgetInfo->m_sounds.push_back(AsciiString("budgeted-legacy.wav"));
	FixtureEvent firstBudgetEvent(AsciiString("fixture-budgeted-legacy-first"));
	firstBudgetEvent.setAudioEventInfo(budgetInfo);
	const AudioHandle firstBudgetHandle = budgetManager.addAudioEvent(&firstBudgetEvent);
	budgetManager.update();
	budgetManager.update();
	FixtureEvent secondBudgetEvent(AsciiString("fixture-budgeted-legacy-second"));
	secondBudgetEvent.setAudioEventInfo(budgetInfo);
	const AudioHandle secondBudgetHandle = budgetManager.addAudioEvent(&secondBudgetEvent);
	budgetManager.update();
	check(budgetManager.isCurrentlyPlaying(firstBudgetHandle)
		&& !budgetManager.isCurrentlyPlaying(secondBudgetHandle)
		&& budgetManager.getActiveAudioCount() == 1U
		&& budgetedLegacySource.decodeBounds.size() == 3U
		&& budgetedLegacySource.decodeBounds[0] == 1U
		&& budgetedLegacySource.decodeBounds[1] == 10000000U
		&& budgetedLegacySource.decodeBounds[2] == 1U,
		"legacy compatibility buffers share one aggregate 64 MiB manager budget");
	budgetManager.closeDevice();
	deleteInstance(budgetInfo);

	AudioEventInfo *simpleInfo = newInstance(AudioEventInfo);
	*simpleInfo = *fixtureInfo;
	simpleInfo->m_attackSounds.clear();
	simpleInfo->m_decaySounds.clear();
	simpleInfo->m_sounds.clear();
	simpleInfo->m_sounds.push_back(AsciiString("decay.wav"));
	FixtureEvent closeEvent(AsciiString("fixture-close"));
	closeEvent.setAudioEventInfo(simpleInfo);
	const AudioHandle closeHandle = manager.addAudioEvent(&closeEvent);
	manager.update();
	manager.update();
	check(manager.isCurrentlyPlaying(closeHandle),
		"file-close fixture is active before its identity is closed");
	manager.closeAnySamplesUsingFile(catalog.getFileIdentity(AsciiString("decay.wav")));
	manager.update();
	check(!manager.isCurrentlyPlaying(closeHandle) && manager.isCurrentlyPlaying(positionalHandle),
		"closeAnySamplesUsingFile stops only records using the matching identity");
	manager.stopAudio(AudioAffect_Sound);
	manager.update();
	check(manager.isCurrentlyPlaying(positionalHandle),
		"sound stop does not affect the still-playing positional record");
	manager.stopAudio(AudioAffect_Sound3D);
	manager.update();
	check(!manager.isCurrentlyPlaying(positionalHandle) && manager.getActiveAudioCount() == 0,
		"3D stop removes the remaining positional record");
	deleteInstance(simpleInfo);

	manager.setChannelLimitsForTest(1U, 1U, 1U);
	AudioEventInfo *lowInfo = newInstance(AudioEventInfo);
	*lowInfo = *fixtureInfo;
	lowInfo->m_attackSounds.clear();
	lowInfo->m_decaySounds.clear();
	lowInfo->m_sounds.clear();
	lowInfo->m_sounds.push_back(AsciiString("main.wav"));
	lowInfo->m_limit = 0;
	lowInfo->m_priority = AP_LOW;
	FixtureEvent lowPriorityEvent(AsciiString("policy-low"));
	lowPriorityEvent.setAudioEventInfo(lowInfo);
	lowPriorityEvent.setAudioPriority(AP_LOW);
	const AudioHandle lowPriorityHandle = manager.addAudioEvent(&lowPriorityEvent);
	manager.update();
	check(manager.isCurrentlyPlaying(lowPriorityHandle),
		"configured channel policy admits the first low-priority event");

	AudioEventInfo *highInfo = newInstance(AudioEventInfo);
	*highInfo = *lowInfo;
	highInfo->m_priority = AP_HIGH;
	FixtureEvent highPriorityEvent(AsciiString("policy-high"));
	highPriorityEvent.setAudioEventInfo(highInfo);
	highPriorityEvent.setAudioPriority(AP_HIGH);
	const AudioHandle highPriorityHandle = manager.addAudioEvent(&highPriorityEvent);
	manager.update();
	check(manager.isCurrentlyPlaying(highPriorityHandle)
		&& !manager.isCurrentlyPlaying(lowPriorityHandle),
		"configured channel policy replaces a lower-priority event");
	manager.stopAudio(AudioAffect_Sound);
	manager.update();

	AudioEventInfo *protectedInfo = newInstance(AudioEventInfo);
	*protectedInfo = *lowInfo;
	FixtureEvent protectedEvent(AsciiString("policy-protected"));
	protectedEvent.setAudioEventInfo(protectedInfo);
	protectedEvent.setUninterruptible(TRUE);
	const AudioHandle protectedHandle = manager.addAudioEvent(&protectedEvent);
	manager.update();
	FixtureEvent blockedEvent(AsciiString("policy-blocked"));
	blockedEvent.setAudioEventInfo(highInfo);
	blockedEvent.setAudioPriority(AP_HIGH);
	const AudioHandle blockedHandle = manager.addAudioEvent(&blockedEvent);
	manager.update();
	check(manager.isCurrentlyPlaying(protectedHandle) && !manager.isCurrentlyPlaying(blockedHandle),
		"protected uninterruptible events cannot be replaced by priority policy");
	manager.stopAudio(AudioAffect_Sound);
	manager.update();

	AudioEventInfo *objectInfo = newInstance(AudioEventInfo);
	*objectInfo = *lowInfo;
	objectInfo->m_type = ST_WORLD | ST_VOICE;
	FixtureEvent objectVoiceEvent(AsciiString("policy-object-voice"));
	objectVoiceEvent.setAudioEventInfo(objectInfo);
	objectVoiceEvent.setObjectID(static_cast<ObjectID>(42U));
	const AudioHandle objectVoiceHandle = manager.addAudioEvent(&objectVoiceEvent);
	manager.update();
	check(manager.isCurrentlyPlaying(objectVoiceHandle) && manager.isObjectPlayingVoice(42U),
		"configured object voice query reports the active voice record");
	FixtureEvent voiceReplacement(AsciiString("policy-voice-replacement"));
	voiceReplacement.setAudioEventInfo(objectInfo);
	voiceReplacement.setAudioPriority(AP_HIGH);
	Coord3D voiceReplacementPosition;
	voiceReplacementPosition.set(12.0f, 0.0f, 0.0f);
	voiceReplacement.setPosition(&voiceReplacementPosition);
	const AudioHandle voiceReplacementHandle = manager.addAudioEvent(&voiceReplacement);
	manager.update();
	check(manager.isCurrentlyPlaying(objectVoiceHandle)
		&& !manager.isCurrentlyPlaying(voiceReplacementHandle),
		"configured 3D policy protects voice channels from priority replacement");
	manager.stopAudio(AudioAffect_Sound3D);
	manager.update();

	manager.setChannelLimitsForTest(1U, 1U, 1U);
	Coord3D forcedPosition;
	forcedPosition.set(14.0f, 0.0f, 0.0f);
	FixtureEvent forcedFiller(AsciiString("policy-forced-filler"));
	forcedFiller.setAudioEventInfo(lowInfo);
	forcedFiller.setAudioPriority(AP_LOW);
	forcedFiller.setPosition(&forcedPosition);
	const AudioHandle forcedFillerHandle = manager.addAudioEvent(&forcedFiller);
	manager.update();
	check(manager.isCurrentlyPlaying(forcedFillerHandle),
		"forced-admission fixture fills the configured 3D channel pool");
	AudioEventInfo *forcedInfo = newInstance(AudioEventInfo);
	*forcedInfo = *lowInfo;
	forcedInfo->m_audioName = AsciiString("policy-forced-voice");
	forcedInfo->m_type = ST_WORLD | ST_VOICE;
	FixtureEvent forcedEvent(AsciiString("policy-forced-voice"));
	forcedEvent.setAudioEventInfo(forcedInfo);
	forcedEvent.setPosition(&forcedPosition);
	forcedEvent.setObjectID(static_cast<ObjectID>(99U));
	manager.friend_forcePlayAudioEventRTS(&forcedEvent);
	manager.update();
	check(manager.isObjectPlayingVoice(99U),
		"force-play admits a protected voice while its normal channel is full");
	AudioEventInfo *forcedHighInfo = newInstance(AudioEventInfo);
	*forcedHighInfo = *lowInfo;
	forcedHighInfo->m_audioName = AsciiString("policy-after-force-high");
	forcedHighInfo->m_priority = AP_HIGH;
	FixtureEvent forcedHigh(AsciiString("policy-after-force-high"));
	forcedHigh.setAudioEventInfo(forcedHighInfo);
	forcedHigh.setAudioPriority(AP_HIGH);
	forcedHigh.setPosition(&forcedPosition);
	const AudioHandle forcedHighHandle = manager.addAudioEvent(&forcedHigh);
	manager.update();
	check(manager.isObjectPlayingVoice(99U) && manager.isCurrentlyPlaying(forcedHighHandle),
		"normal high-priority admission cannot evict a forced record");
	AudioEventInfo *forcedLowInfo = newInstance(AudioEventInfo);
	*forcedLowInfo = *lowInfo;
	forcedLowInfo->m_audioName = AsciiString("policy-after-force-low");
	FixtureEvent forcedLow(AsciiString("policy-after-force-low"));
	forcedLow.setAudioEventInfo(forcedLowInfo);
	forcedLow.setAudioPriority(AP_LOW);
	forcedLow.setPosition(&forcedPosition);
	const AudioHandle forcedLowHandle = manager.addAudioEvent(&forcedLow);
	manager.update();
	check(manager.isObjectPlayingVoice(99U) && !manager.isCurrentlyPlaying(forcedLowHandle),
		"normal lower-priority admission leaves the forced record protected");
	manager.stopAudio(AudioAffect_Sound3D);
	manager.update();
	check(!manager.isObjectPlayingVoice(99U) && manager.getActiveAudioCount() == 0,
		"affect stop cleans up forced and normal records together");
	deleteInstance(forcedInfo);
	deleteInstance(forcedHighInfo);
	deleteInstance(forcedLowInfo);

	AudioEventInfo *pendingLimitInfo = newInstance(AudioEventInfo);
	*pendingLimitInfo = *lowInfo;
	pendingLimitInfo->m_limit = 1;
	pendingLimitInfo->m_sounds.clear();
	pendingLimitInfo->m_sounds.push_back(AsciiString("main.wav"));
	FixtureEvent pendingLimitedFirst(AsciiString("pending-limited"));
	pendingLimitedFirst.setAudioEventInfo(pendingLimitInfo);
	pendingLimitedFirst.setDelayForTest(100.0f);
	const AudioHandle pendingLimitedHandle = manager.addAudioEvent(&pendingLimitedFirst);
	FixtureEvent pendingLimitedSecond(AsciiString("pending-limited"));
	pendingLimitedSecond.setAudioEventInfo(pendingLimitInfo);
	check(pendingLimitedHandle != AHSV_NoSound
		&& manager.addAudioEvent(&pendingLimitedSecond) == AHSV_NoSound
		&& manager.getPendingAudioRequestCount() == 1,
		"pending requests count toward the per-event admission limit");
	manager.killAudioEventImmediately(pendingLimitedHandle);
	deleteInstance(pendingLimitInfo);

	catalog.setDurationMS(AsciiString("short.wav"), 1.0f);
	AudioEventInfo *nonLoopInfo = newInstance(AudioEventInfo);
	*nonLoopInfo = *lowInfo;
	nonLoopInfo->m_sounds.clear();
	nonLoopInfo->m_sounds.push_back(AsciiString("short.wav"));
	nonLoopInfo->m_loopCount = 2;
	nonLoopInfo->m_control = 0;
	FixtureEvent nonLoopEvent(AsciiString("non-loop"));
	nonLoopEvent.setAudioEventInfo(nonLoopInfo);
	const AudioHandle nonLoopHandle = manager.addAudioEvent(&nonLoopEvent);
	manager.update();
	manager.update();
	FakeVoice *nonLoopVoice = engine->lastVoice;
	check(nonLoopVoice != nullptr && nonLoopVoice->completeLastBuffer(),
		"fake backend completes non-loop audio");
	manager.update();
	manager.update();
	check(!manager.isCurrentlyPlaying(nonLoopHandle),
		"non-loop audio does not restart when only a loop count is present");

	AudioEventInfo *loopInfo = newInstance(AudioEventInfo);
	*loopInfo = *nonLoopInfo;
	loopInfo->m_control = AC_LOOP;
	FixtureEvent loopEvent(AsciiString("loop"));
	loopEvent.setAudioEventInfo(loopInfo);
	const AudioHandle loopHandle = manager.addAudioEvent(&loopEvent);
	manager.update();
	manager.update();
	FakeVoice *loopVoice = engine->lastVoice;
	check(loopVoice != nullptr && loopVoice->completeLastBuffer(),
		"fake backend completes loop audio");
	manager.update();
	manager.update();
	check(manager.isCurrentlyPlaying(loopHandle) && loopVoice->submitCalls >= 2,
		"AC_LOOP audio deterministically restarts after terminal phase completion");
	manager.stopAudio(AudioAffect_Sound);
	manager.update();

	AudioEventInfo *musicOneInfo = newInstance(AudioEventInfo);
	*musicOneInfo = *lowInfo;
	musicOneInfo->m_audioName = AsciiString("music-one");
	musicOneInfo->m_soundType = AT_Music;
	musicOneInfo->m_sounds.clear();
	musicOneInfo->m_sounds.push_back(AsciiString("short.wav"));
	musicOneInfo->m_loopCount = 0;
	musicOneInfo->m_control = 0;
	AudioEventInfo *musicTwoInfo = newInstance(AudioEventInfo);
	*musicTwoInfo = *musicOneInfo;
	musicTwoInfo->m_audioName = AsciiString("music-two");
	manager.addAudioEventInfo(musicOneInfo);
	manager.addAudioEventInfo(musicTwoInfo);
	manager.setChannelLimitsForTest(1U, 1U, 2U);
	manager.addTrackName(AsciiString("music-one"));
	manager.addTrackName(AsciiString("music-two"));
	manager.setActiveMusicTrackForTest(AsciiString("music-one"));
	FixtureEvent musicEvent(AsciiString("music-one"));
	musicEvent.setAudioEventInfo(musicOneInfo);
	const AudioHandle musicHandle = manager.addAudioEvent(&musicEvent);
	manager.update();
	manager.update();
	FakeVoice *musicVoice = engine->lastVoice;
	check(manager.isCurrentlyPlaying(musicHandle) && manager.isMusicPlaying(),
		"music fixture starts through the native stream channel");
	check(musicVoice != nullptr && musicVoice->completeLastBuffer(),
		"fake backend completes natural music playback");
	manager.update();
	manager.update();
	check(!manager.isMusicPlaying() && manager.hasMusicTrackCompleted(AsciiString("music-one"), 1),
		"only natural terminal music completion records history");
	const AsciiString nextTrack = manager.nextMusicTrack();
	check(nextTrack == AsciiString("music-two"),
		"next music selects and enqueues the next configured track");
	manager.update();
	check(manager.isMusicPlaying(), "next music transition leaves the new track active");
	const AsciiString previousTrack = manager.prevMusicTrack();
	check(previousTrack == AsciiString("music-one"),
		"previous music selects and enqueues the prior configured track");
	manager.update();
	check(manager.isMusicPlaying(), "previous music transition leaves the new track active");

	FixtureEvent forceEvent(AsciiString("music-two"));
	forceEvent.setAudioEventInfo(musicTwoInfo);
	manager.friend_forcePlayAudioEventRTS(&forceEvent);
	manager.update();
	check(manager.isMusicPlaying(), "force-play music enters the normal active lifecycle");
	manager.stopAudio(AudioAffect_Music);
	manager.update();
	check(!manager.isMusicPlaying() && !manager.hasMusicTrackCompleted(AsciiString("music-two"), 1),
		"stopped music does not record natural completion");

	AudioSettings settings;
	settings.m_use3DSoundRangeVolumeFade = TRUE;
	settings.m_3DSoundRangeVolumeFadeExponent = 2.0f;
	settings.m_globalMinRange = 10;
	settings.m_globalMaxRange = 100;
	settings.m_fadeAudioFrames = 3;
	settings.m_minVolume = 0.0f;
	manager.setAudioSettingsForTest(&settings);
	manager.setVolume(0.5f, AudioAffect_Sound3D);
	AudioEventInfo *attenuationInfo = newInstance(AudioEventInfo);
	*attenuationInfo = *lowInfo;
	attenuationInfo->m_type = ST_WORLD | ST_GLOBAL;
	attenuationInfo->m_volume = 0.8f;
	attenuationInfo->m_volumeShift = 0.5f;
	attenuationInfo->m_minDistance = 0.0f;
	attenuationInfo->m_maxDistance = 100.0f;
	FixtureEvent attenuationEvent(AsciiString("configured-attenuation"));
	attenuationEvent.setAudioEventInfo(attenuationInfo);
	Coord3D attenuationPosition;
	attenuationPosition.set(55.0f, 0.0f, 0.0f);
	attenuationEvent.setPosition(&attenuationPosition);
	const AudioHandle attenuationHandle = manager.addAudioEvent(&attenuationEvent);
	manager.update();
	manager.update();
	FakeVoice *attenuationVoice = engine->lastVoice;
	check(attenuationVoice != nullptr && attenuationVoice->lastVolume > 0.149f
		&& attenuationVoice->lastVolume < 0.151f,
		"effective volume applies event shifts, category volume, and configured global attenuation");
	check(attenuationVoice != nullptr && attenuationVoice->matrixCalls > 0
		&& attenuationVoice->matrixSourceChannels == 2
		&& attenuationVoice->matrixDestinationChannels == 2
		&& attenuationVoice->lastMatrix.size() == 4
		&& attenuationVoice->lastMatrix[2] + attenuationVoice->lastMatrix[3]
			> attenuationVoice->lastMatrix[0] + attenuationVoice->lastMatrix[1],
		"positional audio applies a right-biased stereo X3DAudio matrix");
	TheTacticalView = reinterpret_cast<View *>(static_cast<std::uintptr_t>(1));
	g_nativeAudioBaseUpdateCalls = 0;
	manager.update();
	TheTacticalView = nullptr;
	check(g_nativeAudioBaseUpdateCalls == 1 && attenuationVoice->lastVolume > 0.074f
		&& attenuationVoice->lastVolume < 0.076f,
		"active 3D volume applies the base zoom adjustment exactly once");
	Coord3D listenerPosition;
	listenerPosition.set(55.0f, 0.0f, 0.0f);
	manager.setListenerPosition(&listenerPosition, nullptr);
	manager.update();
	check(attenuationVoice->lastVolume > 0.19f && attenuationVoice->lastVolume < 0.21f,
		"listener movement updates active 3D attenuation on the owner thread");
	check(attenuationVoice->lastMatrix.size() == 4
		&& std::abs((attenuationVoice->lastMatrix[0] + attenuationVoice->lastMatrix[1])
			- (attenuationVoice->lastMatrix[2] + attenuationVoice->lastMatrix[3])) < 0.001f,
		"coincident listener and emitter produce a centered stereo matrix");
	Coord3D listenerBeyond;
	listenerBeyond.set(110.0f, 0.0f, 0.0f);
	manager.setListenerPosition(&listenerBeyond, nullptr);
	manager.update();
	check(attenuationVoice->lastMatrix.size() == 4
		&& attenuationVoice->lastMatrix[0] + attenuationVoice->lastMatrix[1]
			> attenuationVoice->lastMatrix[2] + attenuationVoice->lastMatrix[3],
		"mirrored listener position produces a left-biased stereo matrix");
	manager.setListenerPosition(&listenerPosition, nullptr);
	manager.update();
	manager.setAudioEventVolumeOverride(AsciiString("configured-attenuation"), 0.4f);
	check(attenuationVoice->lastVolume > 0.099f && attenuationVoice->lastVolume < 0.101f,
		"active event volume override replaces the event volume exactly once");
	manager.stopAudio(AudioAffect_Sound3D);
	manager.update();
	check(!manager.isCurrentlyPlaying(attenuationHandle), "3D attenuation fixture stops cleanly");
	FixtureEvent matrix2DEvent(AsciiString("matrix-2d"));
	matrix2DEvent.setAudioEventInfo(lowInfo);
	const AudioHandle matrix2DHandle = manager.addAudioEvent(&matrix2DEvent);
	manager.update();
	FakeVoice *matrix2DVoice = engine->lastVoice;
	check(manager.isCurrentlyPlaying(matrix2DHandle) && matrix2DVoice != nullptr
		&& matrix2DVoice->matrixCalls == 0,
		"non-positional audio does not receive an X3DAudio output matrix");
	manager.killAudioEventImmediately(matrix2DHandle);
	settings.m_use3DSoundRangeVolumeFade = FALSE;
	Coord3D origin;
	origin.zero();
	manager.setListenerPosition(&origin, &origin);
	manager.setAudioEventVolumeOverride(AsciiString("configured-attenuation"), -1.0f);
	const AudioHandle noFadeHandle = manager.addAudioEvent(&attenuationEvent);
	manager.update();
	FakeVoice *noFadeVoice = engine->lastVoice;
	check(noFadeVoice != nullptr && noFadeVoice->lastVolume > 0.199f
		&& noFadeVoice->lastVolume < 0.201f,
		"disabled range fading keeps full volume until the maximum-range cut");
	manager.killAudioEventImmediately(noFadeHandle);

	AudioEventInfo *fadeInfo = newInstance(AudioEventInfo);
	*fadeInfo = *musicOneInfo;
	fadeInfo->m_audioName = AsciiString("fade-music");
	FixtureEvent fadeEvent(AsciiString("fade-music"));
	fadeEvent.setAudioEventInfo(fadeInfo);
	const AudioHandle fadeHandle = manager.addAudioEvent(&fadeEvent);
	manager.update();
	FakeVoice *fadeVoice = engine->lastVoice;
	const float fadeStartVolume = fadeVoice == nullptr ? 0.0f : fadeVoice->lastVolume;
	manager.removeAudioEvent(AHSV_StopTheMusicFade);
	manager.update();
	check(fadeVoice != nullptr && fadeVoice->lastVolume > 0.0f
		&& fadeVoice->lastVolume < fadeStartVolume,
		"configured fade frame count reduces volume before stopping");
	check(fadeVoice != nullptr && fadeVoice->completeLastBuffer(),
		"fake backend completes a fade-pending music buffer");
	manager.update();
	check(!manager.isCurrentlyPlaying(fadeHandle)
		&& !manager.hasMusicTrackCompleted(AsciiString("fade-music"), 1),
		"completion observed during a pending fade is treated as an intentional stop");
	manager.update();
	manager.update();
	manager.update();
	check(!manager.isCurrentlyPlaying(fadeHandle)
		&& !manager.hasMusicTrackCompleted(AsciiString("fade-music"), 1),
		"faded music stops without recording natural completion");

	AudioEventInfo *speechInfo = newInstance(AudioEventInfo);
	*speechInfo = *lowInfo;
	speechInfo->m_soundType = AT_Streaming;
	speechInfo->m_sounds.clear();
	speechInfo->m_sounds.push_back(AsciiString("short.wav"));
	FixtureEvent guardedSpeech(AsciiString("guarded-speech"));
	guardedSpeech.setAudioEventInfo(speechInfo);
	guardedSpeech.setUninterruptible(TRUE);
	const AudioHandle guardedSpeechHandle = manager.addAudioEvent(&guardedSpeech);
	manager.update();
	check(manager.isCurrentlyPlaying(guardedSpeechHandle) && manager.getDisallowSpeech(),
		"uninterruptible native speech raises the disallow-speech guard");
	FixtureEvent blockedSpeech(AsciiString("blocked-speech"));
	blockedSpeech.setAudioEventInfo(speechInfo);
	const AudioHandle blockedSpeechHandle = manager.addAudioEvent(&blockedSpeech);
	check(blockedSpeechHandle == AHSV_NoSound || blockedSpeechHandle == AHSV_Error,
		"disallow-speech blocks a second native speech stream");
	manager.stopAudio(AudioAffect_Speech);
	manager.update();
	check(!manager.getDisallowSpeech(), "speech guard releases after native stream stop");
	const AudioHandle releasedSpeechHandle = manager.addAudioEvent(&blockedSpeech);
	manager.update();
	check(releasedSpeechHandle != AHSV_NoSound && manager.isCurrentlyPlaying(releasedSpeechHandle),
		"speech admission resumes after the uninterruptible stream ends");
	manager.stopAudio(AudioAffect_Speech);
	manager.update();

	FixtureEvent delayedSpeech(AsciiString("delayed-speech"));
	delayedSpeech.setAudioEventInfo(speechInfo);
	delayedSpeech.setDelayForTest(100.0f);
	const AudioHandle delayedSpeechHandle = manager.addAudioEvent(&delayedSpeech);
	FixtureEvent delayedTakeover(AsciiString("delayed-takeover"));
	delayedTakeover.setAudioEventInfo(speechInfo);
	delayedTakeover.setUninterruptible(TRUE);
	const AudioHandle delayedTakeoverHandle = manager.addAudioEvent(&delayedTakeover);
	manager.update();
	check(!manager.isCurrentlyPlaying(delayedSpeechHandle)
		&& manager.isCurrentlyPlaying(delayedTakeoverHandle),
		"uninterruptible speech removes already-pending speech before it can start");
	manager.killAudioEventImmediately(delayedTakeoverHandle);

	FixtureEvent regularSpeech(AsciiString("regular-speech"));
	regularSpeech.setAudioEventInfo(speechInfo);
	const AudioHandle regularSpeechHandle = manager.addAudioEvent(&regularSpeech);
	manager.update();
	check(manager.isCurrentlyPlaying(regularSpeechHandle),
		"a regular speech stream is active before uninterruptible takeover");
	FixtureEvent takeoverSpeech(AsciiString("takeover-speech"));
	takeoverSpeech.setAudioEventInfo(speechInfo);
	takeoverSpeech.setUninterruptible(TRUE);
	const AudioHandle takeoverSpeechHandle = manager.addAudioEvent(&takeoverSpeech);
	manager.update();
	check(!manager.isCurrentlyPlaying(regularSpeechHandle)
		&& manager.isCurrentlyPlaying(takeoverSpeechHandle)
		&& manager.getDisallowSpeech(),
		"starting uninterruptible speech synchronously stops existing speech");
	manager.killAudioEventImmediately(takeoverSpeechHandle);
	check(!manager.isCurrentlyPlaying(takeoverSpeechHandle)
		&& manager.getActiveAudioCount() == 0 && !manager.getDisallowSpeech(),
		"immediate speech kill synchronously releases the voice and guard");

	manager.setVolume(0.25f, AudioAffect_Speech);
	FixtureEvent forcedSpeech(AsciiString("forced-speech"));
	forcedSpeech.setAudioEventInfo(speechInfo);
	manager.friend_forcePlayAudioEventRTS(&forcedSpeech);
	manager.update();
	check(engine->lastVoice != nullptr && engine->lastVoice->lastVolume > 0.24f
		&& engine->lastVoice->lastVolume < 0.26f,
		"briefing force-play honors the speech slider for streaming audio");
	manager.stopAudio(AudioAffect_Speech);
	manager.update();
	manager.setVolume(1.0f, AudioAffect_Speech);

	std::unique_ptr<FakeEngine> replacementOwnedEngine = std::make_unique<FakeEngine>();
	FakeEngine *replacementEngine = replacementOwnedEngine.get();
	XAudio2AudioService replacementService(std::move(replacementOwnedEngine));
	XAudio2AudioManager replacementManager(&replacementService, &catalog);
	FileAudioAssetSource replacementSource(AsciiString(realRoot.string().c_str()));
	replacementManager.openDevice();
	AudioAssetCatalog replacementCatalog;
	replacementCatalog.setDurationMS(AsciiString("replacement.wav"), 100.0f);
	AudioEventInfo *replacementInfo = newInstance(AudioEventInfo);
	*replacementInfo = *lowInfo;
	replacementInfo->m_audioName = AsciiString("replacement-source");
	replacementInfo->m_sounds.clear();
	replacementInfo->m_sounds.push_back(AsciiString("main.wav"));
	FixtureEvent replacementEvent(AsciiString("replacement-source"));
	replacementEvent.setAudioEventInfo(replacementInfo);
	const AudioHandle replacementHandle = replacementManager.addAudioEvent(&replacementEvent);
	replacementManager.update();
	check(replacementEngine->lastVoice != nullptr
		&& replacementManager.isCurrentlyPlaying(replacementHandle)
		&& replacementManager.getActiveAudioCount() == 1,
		"source replacement fixture starts with an active native record");
	replacementManager.setAssetSource(&replacementCatalog);
	check(replacementManager.isOpen() && replacementManager.getActiveAudioCount() == 0
		&& !replacementManager.isCurrentlyPlaying(replacementHandle),
		"replacing the asset source quiesces active records before admission resumes");
	replacementInfo->m_audioName = AsciiString("replacement-file");
	replacementInfo->m_sounds.clear();
	replacementInfo->m_sounds.push_back(AsciiString("replacement.wav"));
	replacementManager.setAssetSource(&replacementSource);
	check(replacementSource.getVirtualFileSource() != nullptr,
		"replacement FileAudioAssetSource receives a manager-owned virtual/BIG provider");
	FixtureEvent replacementFileEvent(AsciiString("replacement-file"));
	replacementFileEvent.setAudioEventInfo(replacementInfo);
	const AudioHandle replacementFileHandle = replacementManager.addAudioEvent(&replacementFileEvent);
	replacementManager.update();
	check(replacementManager.getFileLengthMS(AsciiString("replacement.wav")) == 100.0f
		&& replacementEngine->lastVoice != nullptr
		&& replacementManager.isCurrentlyPlaying(replacementFileHandle),
		"replacement FileAudioAssetSource decodes the actual replacement.wav after provider wiring");
	replacementManager.closeDevice();
	deleteInstance(replacementInfo);

	std::unique_ptr<FakeEngine> failureOwnedEngine = std::make_unique<FakeEngine>();
	FakeEngine *failureEngine = failureOwnedEngine.get();
	XAudio2AudioService failureService(std::move(failureOwnedEngine));
	XAudio2AudioManager failureManager(&failureService, &catalog);
	failureManager.openDevice();
	AudioEventInfo *failureMusicInfo = newInstance(AudioEventInfo);
	*failureMusicInfo = *lowInfo;
	failureMusicInfo->m_audioName = AsciiString("failure-create-music");
	failureMusicInfo->m_soundType = AT_Music;
	failureMusicInfo->m_type = 0;
	failureMusicInfo->m_sounds.clear();
	failureMusicInfo->m_sounds.push_back(AsciiString("short.wav"));
	FixtureEvent failureMusic(AsciiString("failure-create-music"));
	failureMusic.setAudioEventInfo(failureMusicInfo);
	failureEngine->failCreateVoice = TRUE;
	const AudioHandle failureCreateHandle = failureManager.addAudioEvent(&failureMusic);
	failureManager.update();
	check(failureCreateHandle != AHSV_NoSound && !failureManager.isCurrentlyPlaying(failureCreateHandle)
		&& failureManager.getActiveAudioCount() == 0
		&& !failureManager.hasMusicTrackCompleted(AsciiString("failure-create-music"), 1),
		"voice-create failure releases the music handle without false natural completion");

	AudioEventInfo *failureResetInfo = newInstance(AudioEventInfo);
	*failureResetInfo = *speechInfo;
	failureResetInfo->m_audioName = AsciiString("failure-reset-speech");
	FixtureEvent failureReset(AsciiString("failure-reset-speech"));
	failureReset.setAudioEventInfo(failureResetInfo);
	failureReset.setUninterruptible(TRUE);
	failureEngine->failCreateVoice = FALSE;
	failureEngine->failStop = TRUE;
	const AudioHandle failureResetHandle = failureManager.addAudioEvent(&failureReset);
	failureManager.update();
	check(failureManager.isCurrentlyPlaying(failureResetHandle)
		&& failureManager.getDisallowSpeech(),
		"reset-failure fixture first raises the native speech guard");
	failureManager.update();
	check(!failureManager.isCurrentlyPlaying(failureResetHandle)
		&& failureManager.getActiveAudioCount() == 0 && !failureManager.getDisallowSpeech(),
		"voice-reset failure deterministically releases the speech guard and active handle");

	AudioEventInfo *failureSubmitInfo = newInstance(AudioEventInfo);
	*failureSubmitInfo = *lowInfo;
	failureSubmitInfo->m_audioName = AsciiString("failure-submit");
	failureSubmitInfo->m_sounds.clear();
	failureSubmitInfo->m_sounds.push_back(AsciiString("short.wav"));
	FixtureEvent failureSubmit(AsciiString("failure-submit"));
	failureSubmit.setAudioEventInfo(failureSubmitInfo);
	failureEngine->failStop = FALSE;
	failureEngine->failSubmit = TRUE;
	const AudioHandle failureSubmitHandle = failureManager.addAudioEvent(&failureSubmit);
	failureManager.update();
	check(!failureManager.isCurrentlyPlaying(failureSubmitHandle),
		"submit-failure fixture is terminal at the typed voice submission boundary");
	failureManager.update();
	check(!failureManager.isCurrentlyPlaying(failureSubmitHandle)
		&& failureManager.getActiveAudioCount() == 0,
		"voice-submit failure releases the active handle without waiting forever");
	failureManager.closeDevice();
	deleteInstance(failureMusicInfo);
	deleteInstance(failureResetInfo);
	deleteInstance(failureSubmitInfo);

	std::unique_ptr<FakeEngine> recoveryOwnedEngine = std::make_unique<FakeEngine>();
	FakeEngine *recoveryEngine = recoveryOwnedEngine.get();
	auto recoveryService = std::make_unique<XAudio2AudioService>(std::move(recoveryOwnedEngine));
	XAudio2AudioManager recoveryManager(nullptr, &catalog);
	recoveryManager.setOwnedServiceForTest(std::move(recoveryService));
	recoveryManager.openDevice();
	const UnsignedInt recoveryGeneration = recoveryManager.getLifecycleGeneration();
	check(recoveryManager.runInjectedPlaybackProbe(AsciiString("short.wav")),
		"owned recovery fixture submits before a critical failure");
	recoveryEngine->emitCritical(E_ABORT);
	recoveryManager.update();
	check(recoveryManager.isOpen()
		&& recoveryManager.getLifecycleGeneration() != recoveryGeneration
		&& recoveryEngine->stopCalls == 1 && recoveryEngine->closeCalls == 1
		&& recoveryEngine->openCalls == 2 && recoveryEngine->startCalls == 2,
		"owned manager quiesces and reopens its service after a critical failure");
	check(recoveryManager.runInjectedPlaybackProbe(AsciiString("short.wav")),
		"owned manager admits fresh playback after critical-error recovery");
	recoveryManager.closeDevice();

	std::unique_ptr<FakeEngine> injectedFailureEngine = std::make_unique<FakeEngine>();
	FakeEngine *injectedFailureEngineView = injectedFailureEngine.get();
	XAudio2AudioService injectedFailureService(std::move(injectedFailureEngine));
	XAudio2AudioManager injectedFailureManager(&injectedFailureService, &catalog);
	injectedFailureManager.openDevice();
	injectedFailureEngineView->emitCritical(E_ABORT);
	injectedFailureManager.update();
	check(!injectedFailureManager.isOpen()
		&& injectedFailureService.state() == XAudio2AudioServiceState::FAILED
		&& injectedFailureEngineView->stopCalls == 0 && injectedFailureEngineView->closeCalls == 0,
		"injected manager leaves failed-service teardown to its external owner");
	injectedFailureManager.openDevice();
	check(!injectedFailureManager.isOpen() && injectedFailureEngineView->openCalls == 1,
		"injected manager cannot reopen a failed externally owned service");
	injectedFailureService.shutdown();
	check(injectedFailureService.open(), "external owner reopens its injected service after shutdown");
	injectedFailureManager.openDevice();
	check(injectedFailureManager.isOpen(),
		"injected manager resumes only after the external owner completes recovery");
	injectedFailureManager.closeDevice();
	injectedFailureService.shutdown();

	deleteInstance(lowInfo);
	deleteInstance(highInfo);
	deleteInstance(protectedInfo);
	deleteInstance(objectInfo);
	deleteInstance(nonLoopInfo);
	deleteInstance(loopInfo);
	deleteInstance(attenuationInfo);
	deleteInstance(fadeInfo);
	deleteInstance(speechInfo);
	deleteInstance(fixtureInfo);

	const UnsignedInt firstGeneration = manager.getLifecycleGeneration();
	manager.reset();
	check(manager.isOpen(), "manager reset reopens its reusable service");
	check(manager.getLifecycleGeneration() != firstGeneration,
		"manager reset advances the lifecycle generation");
	check(manager.runInjectedPlaybackProbe(AsciiString("decay.wav")),
		"manager admits playback after reset");

	const XAudio2PcmVoiceHandle staleHandle = service.createVoice();
	check(staleHandle.isValid(), "service allocates a typed voice for stale-generation test");
	check(service.resetVoice(staleHandle, 41), "service activates the first test generation");
	AudioPcmChunk staleChunk;
	staleChunk.sampleRate = 48000;
	staleChunk.channels = 2;
	staleChunk.frameCount = 1;
	staleChunk.data.assign(4, 0);
	staleChunk.generation = 40;
	check(service.submit(staleHandle, std::move(staleChunk)) == AudioPcmSubmitResult::DROPPED,
		"stale generation is rejected by the typed voice service");
	service.destroyVoice(staleHandle);
	manager.closeDevice();
	check(!manager.isOpen(), "manager close removes playback admission");
	std::filesystem::remove_all(realRoot);
	return failures == 0 ? 0 : 1;
}
