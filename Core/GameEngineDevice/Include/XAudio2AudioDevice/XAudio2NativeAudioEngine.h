#pragma once

#include "XAudio2AudioDevice/IXAudio2AudioEngineBackend.h"

#include <memory>

class XAudio2NativeAudioEngine final : public IXAudio2AudioEngineBackend
{
public:
	XAudio2NativeAudioEngine();
	~XAudio2NativeAudioEngine() override;

	XAudio2NativeAudioEngine(const XAudio2NativeAudioEngine &) = delete;
	XAudio2NativeAudioEngine &operator=(const XAudio2NativeAudioEngine &) = delete;

	HRESULT open(CriticalErrorCallback callback, void *context) noexcept override;
	HRESULT start() noexcept override;
	HRESULT createPcmVoice(std::unique_ptr<IXAudio2PcmVoiceBackend> &voice) noexcept override;
	HRESULT getOutputDetails(XAudio2OutputDetails &details) const noexcept override;
	HRESULT stop() noexcept override;
	HRESULT close() noexcept override;

private:
	class Impl;
	std::unique_ptr<Impl> m_impl;
};
