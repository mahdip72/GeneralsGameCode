#include "XAudio2AudioDevice/IXAudio2AudioEngineBackend.h"
#include "XAudio2AudioDevice/XAudio2AudioService.h"
#include "XAudio2AudioDevice/XAudio2PcmVoice.h"

#include <cstdio>
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
	HRESULT start() noexcept override { ++starts; return S_OK; }
	HRESULT stop() noexcept override { ++stops; return S_OK; }
	HRESULT flush() noexcept override { ++flushes; return S_OK; }
	HRESULT setVolume(float) noexcept override { ++volumes; return S_OK; }
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
	service.shutdown();
	return failures == 0 ? 0 : 1;
}
