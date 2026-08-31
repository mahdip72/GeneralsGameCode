#include "XAudio2AudioDevice/IXAudio2AudioEngineBackend.h"
#include "XAudio2AudioDevice/XAudio2AudioService.h"
#include "XAudio2AudioDevice/XAudio2PcmVoice.h"

#include <cstdio>
#include <limits>
#include <memory>

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

class FakeVoice final : public IXAudio2PcmVoiceBackend
{
public:
	HRESULT create(const WAVEFORMATEX &, IXAudio2VoiceCallback *callback) noexcept override
	{
		m_callback = callback;
		return S_OK;
	}
	HRESULT submit(const XAUDIO2_BUFFER &buffer) noexcept override
	{
		m_context = buffer.pContext;
		return S_OK;
	}
	HRESULT create(const WAVEFORMATEX &format, IXAudio2VoiceCallback *callback,
		float maxRatio) noexcept override
	{
		maxFrequencyRatio = maxRatio;
		return create(format, callback);
	}
	HRESULT start() noexcept override { ++starts; return S_OK; }
	HRESULT stop() noexcept override { ++stops; return S_OK; }
	HRESULT flush() noexcept override { ++flushes; return S_OK; }
	HRESULT setVolume(float) noexcept override { ++volumes; return S_OK; }
	HRESULT setFrequencyRatio(float ratio) noexcept override
	{
		++frequencyRatios;
		lastFrequencyRatio = ratio;
		if (failDuringFrequencyRatio && m_callback != nullptr) {
			m_callback->OnVoiceError(nullptr, E_ABORT);
		}
		if (failEngineDuringFrequencyRatio && engineCallback != nullptr) {
			engineCallback(engineContext, E_ABORT);
		}
		return frequencyRatioResult;
	}
	HRESULT pause() noexcept override { ++pauses; return S_OK; }
	HRESULT resume() noexcept override { ++resumes; return S_OK; }
	HRESULT getCriticalError() const noexcept override { return S_OK; }
	void destroy() noexcept override { m_callback = nullptr; }

	void complete() noexcept
	{
		if (m_callback != nullptr) {
			m_callback->OnBufferEnd(m_context);
		}
	}

	int starts = 0;
	int stops = 0;
	int flushes = 0;
	int volumes = 0;
	int pauses = 0;
	int resumes = 0;
	int frequencyRatios = 0;
	float lastFrequencyRatio = 1.0f;
	float maxFrequencyRatio = 0.0f;
	HRESULT frequencyRatioResult = S_OK;
	bool failDuringFrequencyRatio = false;
	bool failEngineDuringFrequencyRatio = false;
	IXAudio2AudioEngineBackend::CriticalErrorCallback engineCallback = nullptr;
	void *engineContext = nullptr;

private:
	IXAudio2VoiceCallback *m_callback = nullptr;
	void *m_context = nullptr;
};

class FakeEngine final : public IXAudio2AudioEngineBackend
{
public:
	HRESULT open(CriticalErrorCallback callback, void *context) noexcept override
	{
		m_callback = callback;
		m_context = context;
		return S_OK;
	}
	HRESULT start() noexcept override { return S_OK; }
	HRESULT createPcmVoice(std::unique_ptr<IXAudio2PcmVoiceBackend> &voice) noexcept override
	{
		std::unique_ptr<FakeVoice> created = std::make_unique<FakeVoice>();
		m_voice = created.get();
		m_voice->engineCallback = m_callback;
		m_voice->engineContext = m_context;
		voice = std::move(created);
		return S_OK;
	}
	HRESULT stop() noexcept override { return S_OK; }
	HRESULT close() noexcept override
	{
		m_callback = nullptr;
		m_context = nullptr;
		return S_OK;
	}
	FakeVoice *m_voice = nullptr;

private:
	CriticalErrorCallback m_callback = nullptr;
	void *m_context = nullptr;
};
}

int main()
{
	FakeEngine *engine = nullptr;
	std::unique_ptr<FakeEngine> owned = std::make_unique<FakeEngine>();
	engine = owned.get();
	XAudio2AudioService service(std::move(owned));
	check(service.open(), "service opens with injected engine");
	XAudio2PcmVoiceHandle handle = service.createVoice();
	check(handle.isValid(), "typed voice handle created");
	check(service.resetVoice(handle, 7), "voice generation reset accepted");
	service.serviceVoice(handle);

	AudioPcmChunk chunk;
	chunk.sampleRate = 48000;
	chunk.channels = 2;
	chunk.frameCount = 1;
	chunk.startSample = 0;
	chunk.generation = 7;
	chunk.sequence = 11;
	chunk.data.assign(4, 0);
	check(service.submit(handle, std::move(chunk)) == AudioPcmSubmitResult::ACCEPTED,
		"bounded PCM submitted");
	service.serviceVoices();
	engine->m_voice->complete();

	XAudio2AudioCompletion completion;
	check(service.tryPopCompletion(completion), "owner observes completion");
	check(completion.generation == 7 && completion.sequence == 11,
		"completion carries generation and sequence");
	check(service.setVoiceVolume(handle, 0.5f), "typed volume control");
	check(service.pauseVoice(handle), "typed pause control");
	check(service.resumeVoice(handle), "typed resume control");
	check(service.stopVoice(handle), "typed stop control");
	check(engine->m_voice->volumes == 1 && engine->m_voice->pauses == 1
		&& engine->m_voice->resumes == 1 && engine->m_voice->stops >= 1,
		"backend controls observed");
	service.destroyVoice(handle);
	check(!service.setVoiceFrequencyRatio(handle, 1.0f),
		"destroyed voice handles reject pitch controls");
	const XAudio2PcmVoiceHandle pitched = service.createVoice(3.0f);
	check(pitched.isValid() && engine->m_voice->maxFrequencyRatio == 3.0f,
		"a voice reserves only its requested pitch range, including ratios above the default maximum");
	check(service.setVoiceFrequencyRatio(pitched, XAUDIO2_MIN_FREQ_RATIO)
		&& service.setVoiceFrequencyRatio(pitched, 0.75f)
		&& service.setVoiceFrequencyRatio(pitched, 1.25f)
		&& service.setVoiceFrequencyRatio(pitched, 3.0f)
		&& engine->m_voice->lastFrequencyRatio == 3.0f,
		"typed pitch controls preserve lower, upper and non-unity frequency ratios");
	const int validPitchCalls = engine->m_voice->frequencyRatios;
	check(!service.setVoiceFrequencyRatio(pitched, 0.0f)
		&& !service.setVoiceFrequencyRatio(pitched, -1.0f)
		&& !service.setVoiceFrequencyRatio(pitched, XAUDIO2_MIN_FREQ_RATIO * 0.5f)
		&& !service.setVoiceFrequencyRatio(pitched, 3.1f)
		&& !service.setVoiceFrequencyRatio(pitched, std::numeric_limits<float>::infinity())
		&& !service.setVoiceFrequencyRatio(pitched, std::numeric_limits<float>::quiet_NaN())
		&& engine->m_voice->frequencyRatios == validPitchCalls && !service.isVoiceFailed(pitched),
		"invalid pitch never reaches XAudio2 or poisons a usable voice");
	check(service.pauseVoice(pitched) && service.resumeVoice(pitched)
		&& service.resetVoice(pitched, 9) && service.serviceVoice(pitched)
		&& engine->m_voice->lastFrequencyRatio == 3.0f
		&& engine->m_voice->frequencyRatios == validPitchCalls,
		"pause, resume and PCM generation reset retain the selected pitch");
	check(!service.setVoiceFrequencyRatio(handle, 1.0f)
		&& engine->m_voice->frequencyRatios == validPitchCalls,
		"a stale handle cannot alter a replacement voice's pitch");
	engine->m_voice->frequencyRatioResult = E_FAIL;
	check(!service.setVoiceFrequencyRatio(pitched, 1.0f) && service.isVoiceFailed(pitched)
		&& service.getVoiceLastError(pitched) == E_FAIL,
		"native pitch-control failure is published on the owning voice");
	const int failedPitchCalls = engine->m_voice->frequencyRatios;
	check(!service.setVoiceFrequencyRatio(pitched, 1.0f)
		&& engine->m_voice->frequencyRatios == failedPitchCalls,
		"failed voices reject subsequent pitch controls without touching the backend");
	(void)service.destroyVoice(pitched);
	const XAudio2PcmVoiceHandle unsupportedPitch = service.createVoice();
	engine->m_voice->frequencyRatioResult = E_NOTIMPL;
	check(!service.setVoiceFrequencyRatio(unsupportedPitch, 1.25f)
		&& service.isVoiceFailed(unsupportedPitch)
		&& service.getVoiceLastError(unsupportedPitch) == E_NOTIMPL,
		"unsupported non-unity pitch cannot silently succeed at unity");
	(void)service.destroyVoice(unsupportedPitch);
	const XAudio2PcmVoiceHandle callbackFailure = service.createVoice();
	engine->m_voice->failDuringFrequencyRatio = true;
	check(!service.setVoiceFrequencyRatio(callbackFailure, 1.25f)
		&& service.isVoiceFailed(callbackFailure)
		&& service.getVoiceLastError(callbackFailure) == E_ABORT,
		"a callback failure during pitch control is observed before reporting success");
	(void)service.destroyVoice(callbackFailure);
	check(!service.createVoice(0.0f).isValid()
		&& !service.createVoice(XAUDIO2_MAX_FREQ_RATIO + 1.0f).isValid()
		&& !service.createVoice(std::numeric_limits<float>::infinity()).isValid()
		&& !service.createVoice(std::numeric_limits<float>::quiet_NaN()).isValid(),
		"invalid voice pitch ranges are rejected before native creation");
	const XAudio2PcmVoiceHandle criticalFailure = service.createVoice();
	engine->m_voice->failEngineDuringFrequencyRatio = true;
	check(!service.setVoiceFrequencyRatio(criticalFailure, 1.25f)
		&& engine->m_voice->frequencyRatios == 1 && service.isVoiceFailed(criticalFailure),
		"an engine failure during pitch control is observed before reporting success");
	check(!service.setVoiceFrequencyRatio(criticalFailure, 1.25f)
		&& engine->m_voice->frequencyRatios == 1,
		"pending engine failure rejects pitch before reaching a native voice");
	service.shutdown();
	check(!service.setVoiceFrequencyRatio(criticalFailure, 1.0f),
		"closed services reject pitch controls from the previous lifecycle");
	check(service.open(), "service reopens after pitch failure lifecycle teardown");
	const XAudio2PcmVoiceHandle reopened = service.createVoice();
	check(reopened.isValid() && service.setVoiceFrequencyRatio(reopened, 1.0f)
		&& engine->m_voice->lastFrequencyRatio == 1.0f
		&& !service.setVoiceFrequencyRatio(criticalFailure, 1.5f),
		"reopening starts at unity and never accepts an old lifecycle's handle");
	service.shutdown();
	return failures == 0 ? 0 : 1;
}
