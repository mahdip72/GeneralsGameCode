#include "AudioDevice/NullAudioManager.h"

#include "AudioDevice/AudioAssetSource.h"
#include "Common/AudioEventRTS.h"
#include "Common/AudioSettings.h"
#include "Common/AudioRequest.h"
#include "Common/AudioHandleSpecialValues.h"

class View;
extern View *TheTacticalView;

NullAudioManager::NullAudioManager() :
	m_ownedAssetSource(std::make_unique<FileAudioAssetSource>()),
	m_assetSource(m_ownedAssetSource.get()),
	m_preferredProvider(AsciiString::TheEmptyString),
	m_preferredSpeaker(AsciiString::TheEmptyString),
	m_lifecycleGeneration(1)
{
}

NullAudioManager::~NullAudioManager() = default;

void NullAudioManager::setAssetSource(AudioAssetSource *source)
{
	if (source != nullptr) {
		m_ownedAssetSource.reset();
		m_assetSource = source;
		return;
	}
	m_ownedAssetSource = std::make_unique<FileAudioAssetSource>();
	m_assetSource = m_ownedAssetSource.get();
}

void NullAudioManager::init()
{
	AudioManager::init();
	if (m_ownedAssetSource != nullptr && m_audioSettings != nullptr
		&& !m_audioSettings->m_audioRoot.isEmpty()) {
		m_ownedAssetSource = std::make_unique<FileAudioAssetSource>(m_audioSettings->m_audioRoot);
		m_assetSource = m_ownedAssetSource.get();
	}
}

void NullAudioManager::postProcessLoad()
{
	AudioManager::postProcessLoad();
}

void NullAudioManager::reset()
{
	// The null backend has no device resources, but it keeps the same owner-side
	// metadata/request ordering as the native manager.
	AudioManager::reset();
	removeAllAudioRequests();
	removeLevelSpecificAudioEventInfos();
	++m_lifecycleGeneration;
	if (m_lifecycleGeneration == 0) {
		m_lifecycleGeneration = 1;
	}
}

void NullAudioManager::update()
{
	// AudioManager::update historically assumes a tactical view.  Headless
	// scripts intentionally run without one, so retain deterministic volume and
	// listener state while avoiding that dereference.
	if (TheTacticalView != nullptr) {
		AudioManager::update();
	} else {
		m_zoomVolume = 1.0f;
		set3DVolumeAdjustment(1.0f);
	}
	removeAllAudioRequests();
}

void NullAudioManager::killAudioEventImmediately(AudioHandle)
{
}

AsciiString NullAudioManager::nextMusicTrack()
{
	return nextTrackName(AsciiString::TheEmptyString);
}

AsciiString NullAudioManager::prevMusicTrack()
{
	return prevTrackName(AsciiString::TheEmptyString);
}

AsciiString NullAudioManager::getProviderName(UnsignedInt) const
{
	return AsciiString::TheEmptyString;
}

Real NullAudioManager::getFileLengthMS(AsciiString fileName) const
{
	Real durationMS = 0.0f;
	if (m_assetSource != nullptr && m_assetSource->getDurationMS(fileName, durationMS)) {
		return durationMS;
	}
	return 0.0f;
}
