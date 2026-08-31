#pragma once

#include <windows.h>

#include <cstdint>
#include <memory>

class IXAudio2PcmVoiceBackend;

struct XAudio2OutputDetails
{
	std::uint32_t channelMask = 0;
	std::uint32_t channelCount = 0;
};

// Project-owned seam for the XAudio2 engine.  Consumers never receive an
// IXAudio2, mastering-voice, or source-voice pointer; the native adapter owns
// those Windows interfaces behind this boundary.
class IXAudio2AudioEngineBackend
{
public:
	using CriticalErrorCallback = void (*)(void *context, HRESULT error) noexcept;

	virtual ~IXAudio2AudioEngineBackend() = default;

	virtual HRESULT open(CriticalErrorCallback callback, void *context) noexcept = 0;
	virtual HRESULT start() noexcept = 0;
	virtual HRESULT createPcmVoice(std::unique_ptr<IXAudio2PcmVoiceBackend> &voice) noexcept = 0;
	virtual HRESULT getOutputDetails(XAudio2OutputDetails &details) const noexcept
	{
		details = {};
		return E_NOTIMPL;
	}
	virtual HRESULT stop() noexcept = 0;
	virtual HRESULT close() noexcept = 0;
};
