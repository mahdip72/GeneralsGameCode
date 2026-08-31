#include "XAudio2AudioDevice/XAudio2NativeAudioEngine.h"

#include "Lib/FrameTimingDiagnostics.h"
#include "XAudio2AudioDevice/XAudio2CallbackGate.h"
#include "XAudio2AudioDevice/XAudio2PcmVoice.h"

#include <xaudio2.h>

#include <atomic>
#include <cstdint>
#include <new>

namespace
{
class NativeEngineCallback final : public IXAudio2EngineCallback
{
public:
	using CriticalErrorCallback = IXAudio2AudioEngineBackend::CriticalErrorCallback;

	void setSink(CriticalErrorCallback callback, void *context) noexcept
	{
		disableAndWait();
		m_context.store(context, std::memory_order_release);
		m_sink.store(callback, std::memory_order_release);
		m_generation.store(m_gate.enable(), std::memory_order_release);
	}

	void disableAndWait() noexcept
	{
		m_gate.disableAndWait();
		m_sink.store(nullptr, std::memory_order_release);
		m_context.store(nullptr, std::memory_order_release);
	}

	void STDMETHODCALLTYPE OnProcessingPassStart() override
	{
		XAudio2CallbackGate::Token token;
		if (begin(token)) {
			end(token);
		}
	}

	void STDMETHODCALLTYPE OnProcessingPassEnd() override
	{
		XAudio2CallbackGate::Token token;
		if (begin(token)) {
			end(token);
		}
	}

	void STDMETHODCALLTYPE OnCriticalError(HRESULT error) override
	{
		XAudio2CallbackGate::Token token;
		if (!begin(token)) {
			return;
		}
		CriticalErrorCallback sink = m_sink.load(std::memory_order_acquire);
		void *context = m_context.load(std::memory_order_acquire);
		if (sink != nullptr) {
			sink(context, error);
		}
		end(token);
	}

private:
	bool begin(XAudio2CallbackGate::Token &token) noexcept
	{
		return m_gate.tryEnter(token, m_generation.load(std::memory_order_acquire));
	}

	void end(XAudio2CallbackGate::Token &token) noexcept
	{
		m_gate.leave(token);
	}

	XAudio2CallbackGate m_gate;
	std::atomic<std::uint64_t> m_generation { 0 };
	std::atomic<CriticalErrorCallback> m_sink { nullptr };
	std::atomic<void *> m_context { nullptr };
};

class NativeSourceVoiceCallback final : public IXAudio2VoiceCallback
{
public:
	void setSink(IXAudio2VoiceCallback *sink) noexcept
	{
		disableAndWait();
		m_sink.store(sink, std::memory_order_release);
		m_generation.store(m_gate.enable(), std::memory_order_release);
	}

	void disableAndWait() noexcept
	{
		m_gate.disableAndWait();
		m_sink.store(nullptr, std::memory_order_release);
	}

	void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32 bytesRequired) override
	{
		IXAudio2VoiceCallback *sink = nullptr;
		XAudio2CallbackGate::Token token;
		if (begin(sink, token)) {
			sink->OnVoiceProcessingPassStart(bytesRequired);
			end(token);
		}
	}

	void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() override
	{
		IXAudio2VoiceCallback *sink = nullptr;
		XAudio2CallbackGate::Token token;
		if (begin(sink, token)) {
			sink->OnVoiceProcessingPassEnd();
			end(token);
		}
	}

	void STDMETHODCALLTYPE OnStreamEnd() override
	{
		IXAudio2VoiceCallback *sink = nullptr;
		XAudio2CallbackGate::Token token;
		if (begin(sink, token)) {
			sink->OnStreamEnd();
			end(token);
		}
	}

	void STDMETHODCALLTYPE OnBufferStart(void *context) override
	{
		IXAudio2VoiceCallback *sink = nullptr;
		XAudio2CallbackGate::Token token;
		if (begin(sink, token)) {
			sink->OnBufferStart(context);
			end(token);
		}
	}

	void STDMETHODCALLTYPE OnBufferEnd(void *context) override
	{
		IXAudio2VoiceCallback *sink = nullptr;
		XAudio2CallbackGate::Token token;
		if (begin(sink, token)) {
			sink->OnBufferEnd(context);
			end(token);
		}
	}

	void STDMETHODCALLTYPE OnLoopEnd(void *context) override
	{
		IXAudio2VoiceCallback *sink = nullptr;
		XAudio2CallbackGate::Token token;
		if (begin(sink, token)) {
			sink->OnLoopEnd(context);
			end(token);
		}
	}

	void STDMETHODCALLTYPE OnVoiceError(void *context, HRESULT error) override
	{
		IXAudio2VoiceCallback *sink = nullptr;
		XAudio2CallbackGate::Token token;
		if (begin(sink, token)) {
			sink->OnVoiceError(context, error);
			end(token);
		}
	}

private:
	bool begin(IXAudio2VoiceCallback *&sink, XAudio2CallbackGate::Token &token) noexcept
	{
		if (!m_gate.tryEnter(token, m_generation.load(std::memory_order_acquire))) {
			return false;
		}
		sink = m_sink.load(std::memory_order_acquire);
		if (sink == nullptr) {
			m_gate.leave(token);
			return false;
		}
		return true;
	}

	void end(XAudio2CallbackGate::Token &token) noexcept
	{
		m_gate.leave(token);
	}

	XAudio2CallbackGate m_gate;
	std::atomic<std::uint64_t> m_generation { 0 };
	std::atomic<IXAudio2VoiceCallback *> m_sink { nullptr };
};

class NativePcmVoiceBackend final : public IXAudio2PcmVoiceBackend
{
public:
	NativePcmVoiceBackend(IXAudio2 *engine, IXAudio2MasteringVoice *masteringVoice,
		UINT32 destinationChannels) :
		m_engine(engine),
		m_masteringVoice(masteringVoice),
		m_destinationChannels(destinationChannels)
	{
	}

	HRESULT create(const WAVEFORMATEX &format, IXAudio2VoiceCallback *callback) noexcept override
	{
		return create(format, callback, XAUDIO2_DEFAULT_FREQ_RATIO);
	}

	HRESULT create(const WAVEFORMATEX &format, IXAudio2VoiceCallback *callback,
		float maxFrequencyRatio) noexcept override
	{
		rts::frame_timing::Scope timing(rts::frame_timing::AudioVoiceCreate);
		if (m_engine == nullptr || m_voice != nullptr) {
			return E_HANDLE;
		}
		m_callback.setSink(callback);
		HRESULT result = m_engine->CreateSourceVoice(&m_voice, &format, 0, maxFrequencyRatio,
			&m_callback, nullptr, nullptr);
		if (FAILED(result)) {
			m_callback.disableAndWait();
			m_voice = nullptr;
		} else {
			m_sourceChannels = format.nChannels;
		}
		return result;
	}

	HRESULT submit(const XAUDIO2_BUFFER &buffer) noexcept override
	{
		return m_voice != nullptr ? m_voice->SubmitSourceBuffer(&buffer, nullptr) : E_HANDLE;
	}

	HRESULT start() noexcept override
	{
		return m_voice != nullptr ? m_voice->Start(0, 0) : E_HANDLE;
	}

	HRESULT stop() noexcept override
	{
		return m_voice != nullptr ? m_voice->Stop(0, 0) : E_HANDLE;
	}

	HRESULT pause() noexcept override
	{
		return stop();
	}

	HRESULT resume() noexcept override
	{
		return m_voice != nullptr ? m_voice->Start(0, 0) : E_HANDLE;
	}

	HRESULT setVolume(float volume) noexcept override
	{
		return m_voice != nullptr ? m_voice->SetVolume(volume) : E_HANDLE;
	}

	HRESULT setFrequencyRatio(float ratio) noexcept override
	{
		return m_voice != nullptr ? m_voice->SetFrequencyRatio(ratio, XAUDIO2_COMMIT_NOW) : E_HANDLE;
	}

	HRESULT setOutputMatrix(UINT32 sourceChannels, UINT32 destinationChannels,
		const float *matrix) noexcept override
	{
		if (m_voice == nullptr || m_masteringVoice == nullptr || matrix == nullptr) {
			return E_HANDLE;
		}
		if (sourceChannels != m_sourceChannels || destinationChannels != m_destinationChannels) {
			return E_INVALIDARG;
		}
		return m_voice->SetOutputMatrix(m_masteringVoice, sourceChannels,
			destinationChannels, matrix, XAUDIO2_COMMIT_NOW);
	}

	HRESULT flush() noexcept override
	{
		return m_voice != nullptr ? m_voice->FlushSourceBuffers() : E_HANDLE;
	}

	HRESULT getCriticalError() const noexcept override
	{
		return m_criticalError.load(std::memory_order_acquire);
	}

	void destroy() noexcept override
	{
		(void)destroyWithResult();
	}

	HRESULT destroyWithResult() noexcept override
	{
		rts::frame_timing::Scope timing(rts::frame_timing::AudioVoiceDestroy);
		IXAudio2SourceVoice *voice = m_voice;
		if (voice == nullptr) {
			m_callback.disableAndWait();
			return S_OK;
		}

		// Stop and flush before DestroyVoice.  Delivery is disabled first so a
		// callback cannot enter the project voice while its owner is tearing down.
		m_callback.disableAndWait();
		HRESULT firstFailure = S_OK;
		const HRESULT stopResult = voice->Stop(0, 0);
		if (FAILED(stopResult)) {
			firstFailure = stopResult;
		}
		const HRESULT flushResult = voice->FlushSourceBuffers();
		if (SUCCEEDED(firstFailure) && FAILED(flushResult)) {
			firstFailure = flushResult;
		}
		voice->DestroyVoice();
		m_voice = nullptr;
		m_callback.disableAndWait();
		return firstFailure;
	}

private:
	IXAudio2 *m_engine;
	IXAudio2MasteringVoice *m_masteringVoice;
	IXAudio2SourceVoice *m_voice = nullptr;
	UINT32 m_sourceChannels = 0;
	UINT32 m_destinationChannels;
	NativeSourceVoiceCallback m_callback;
	std::atomic<HRESULT> m_criticalError { S_OK };
};
}

class XAudio2NativeAudioEngine::Impl
{
public:
	~Impl()
	{
		close();
	}

	HRESULT open(CriticalErrorCallback callback, void *context) noexcept
	{
		if (m_engine != nullptr) {
			return S_OK;
		}

		m_callback.setSink(callback, context);
		HRESULT result = XAudio2Create(&m_engine, 0, XAUDIO2_USE_DEFAULT_PROCESSOR);
		if (FAILED(result)) {
			m_callback.disableAndWait();
			return result;
		}

		result = m_engine->RegisterForCallbacks(&m_callback);
		if (FAILED(result)) {
			close();
			return result;
		}
		m_callbacksRegistered = true;

		result = m_engine->CreateMasteringVoice(&m_masteringVoice);
		if (FAILED(result)) {
			close();
			return result;
		}
		XAUDIO2_VOICE_DETAILS details = {};
		m_masteringVoice->GetVoiceDetails(&details);
		DWORD channelMask = 0;
		result = m_masteringVoice->GetChannelMask(&channelMask);
		if (FAILED(result) || details.InputChannels == 0
			|| details.InputChannels > XAUDIO2_MAX_AUDIO_CHANNELS || channelMask == 0) {
			close();
			return FAILED(result) ? result : E_INVALIDARG;
		}
		m_outputDetails.channelMask = channelMask;
		m_outputDetails.channelCount = details.InputChannels;
		m_stopIssued = false;
		return S_OK;
	}

	HRESULT start() noexcept
	{
		if (m_engine == nullptr || m_masteringVoice == nullptr) {
			return E_HANDLE;
		}
		m_stopIssued = false;
		return m_engine->StartEngine();
	}

	HRESULT createPcmVoice(std::unique_ptr<IXAudio2PcmVoiceBackend> &voice) noexcept
	{
		if (m_engine == nullptr || m_masteringVoice == nullptr) {
			return E_HANDLE;
		}
		NativePcmVoiceBackend *nativeVoice = new (std::nothrow) NativePcmVoiceBackend(
			m_engine, m_masteringVoice, m_outputDetails.channelCount);
		if (nativeVoice == nullptr) {
			return E_OUTOFMEMORY;
		}
		voice.reset(nativeVoice);
		return S_OK;
	}

	HRESULT getOutputDetails(XAudio2OutputDetails &details) const noexcept
	{
		if (m_engine == nullptr || m_masteringVoice == nullptr
			|| m_outputDetails.channelCount == 0 || m_outputDetails.channelMask == 0) {
			details = {};
			return E_HANDLE;
		}
		details = m_outputDetails;
		return S_OK;
	}

	HRESULT stop() noexcept
	{
		if (m_engine == nullptr || m_stopIssued) {
			return S_OK;
		}
		m_engine->StopEngine();
		m_stopIssued = true;
		return S_OK;
	}

	HRESULT close() noexcept
	{
		if (m_engine == nullptr) {
			m_callback.disableAndWait();
			return S_OK;
		}

		// Disable and drain before unregistering.  This guarantees the service
		// sink cannot be reached after native close returns.
		m_callback.disableAndWait();
		if (m_callbacksRegistered) {
			m_engine->UnregisterForCallbacks(&m_callback);
			m_callbacksRegistered = false;
		}
		if (m_masteringVoice != nullptr) {
			m_masteringVoice->DestroyVoice();
			m_masteringVoice = nullptr;
		}
		m_engine->Release();
		m_engine = nullptr;
		m_outputDetails = {};
		m_stopIssued = true;
		return S_OK;
	}

private:
	IXAudio2 *m_engine = nullptr;
	IXAudio2MasteringVoice *m_masteringVoice = nullptr;
	XAudio2OutputDetails m_outputDetails;
	NativeEngineCallback m_callback;
	bool m_callbacksRegistered = false;
	bool m_stopIssued = true;
};

XAudio2NativeAudioEngine::XAudio2NativeAudioEngine() :
	m_impl(std::make_unique<Impl>())
{
}

XAudio2NativeAudioEngine::~XAudio2NativeAudioEngine() = default;

HRESULT XAudio2NativeAudioEngine::open(CriticalErrorCallback callback, void *context) noexcept
{
	return m_impl->open(callback, context);
}

HRESULT XAudio2NativeAudioEngine::start() noexcept
{
	return m_impl->start();
}

HRESULT XAudio2NativeAudioEngine::createPcmVoice(std::unique_ptr<IXAudio2PcmVoiceBackend> &voice) noexcept
{
	return m_impl->createPcmVoice(voice);
}

HRESULT XAudio2NativeAudioEngine::getOutputDetails(XAudio2OutputDetails &details) const noexcept
{
	return m_impl->getOutputDetails(details);
}

HRESULT XAudio2NativeAudioEngine::stop() noexcept
{
	return m_impl->stop();
}

HRESULT XAudio2NativeAudioEngine::close() noexcept
{
	return m_impl->close();
}
