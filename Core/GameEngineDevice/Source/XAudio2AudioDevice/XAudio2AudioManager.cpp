#include "XAudio2AudioDevice/XAudio2AudioManager.h"

class View;
extern View *TheTacticalView;

XAudio2AudioManager::XAudio2AudioManager() :
	m_preferredProvider(AsciiString::TheEmptyString),
	m_preferredSpeaker(AsciiString::TheEmptyString),
	m_open(FALSE)
{
}

XAudio2AudioManager::~XAudio2AudioManager() = default;

void XAudio2AudioManager::init()
{
	AudioManager::init();
	openDevice();
}

void XAudio2AudioManager::postProcessLoad()
{
	AudioManager::postProcessLoad();
}

void XAudio2AudioManager::reset()
{
	closeDevice();
	AudioManager::reset();
	removeAllAudioRequests();
	removeLevelSpecificAudioEventInfos();
}

void XAudio2AudioManager::update()
{
	if (TheTacticalView != nullptr) {
		AudioManager::update();
	} else {
		m_zoomVolume = 1.0f;
		set3DVolumeAdjustment(1.0f);
	}
}

AsciiString XAudio2AudioManager::nextMusicTrack()
{
	return nextTrackName(AsciiString::TheEmptyString);
}

AsciiString XAudio2AudioManager::prevMusicTrack()
{
	return prevTrackName(AsciiString::TheEmptyString);
}

void XAudio2AudioManager::openDevice()
{
	m_open = TRUE;
}

void XAudio2AudioManager::closeDevice()
{
	m_open = FALSE;
}

AsciiString XAudio2AudioManager::getProviderName(UnsignedInt providerNum) const
{
	return providerNum == 0 ? AsciiString("XAudio2") : AsciiString::TheEmptyString;
}

UnsignedInt XAudio2AudioManager::getProviderIndex(AsciiString providerName) const
{
	return providerName == AsciiString("XAudio2") ? 0 : PROVIDER_ERROR;
}
