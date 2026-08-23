#include "Common/AudioAffect.h"
#include "Common/AudioEventInfo.h"
#include "Common/AudioEventRTS.h"
#include "Common/AudioRequest.h"
#include "Common/AudioSettings.h"
#include "Common/GameAudio.h"
#include "Common/SubsystemInterface.h"

#include <algorithm>

AudioManager *TheAudio = nullptr;
class View;
View *TheTacticalView = nullptr;
const char *const AudioManager::MuteAudioReasonNames[] = { "window-focus" };

SubsystemInterface::SubsystemInterface() = default;
SubsystemInterface::~SubsystemInterface() = default;

AudioManager::AudioManager() :
	m_audioSettings(nullptr),
	m_miscAudio(nullptr),
	m_music(nullptr),
	m_sound(nullptr),
	m_musicVolume(1.0f),
	m_soundVolume(1.0f),
	m_sound3DVolume(1.0f),
	m_speechVolume(1.0f),
	m_scriptMusicVolume(1.0f),
	m_scriptSoundVolume(1.0f),
	m_scriptSound3DVolume(1.0f),
	m_scriptSpeechVolume(1.0f),
	m_systemMusicVolume(1.0f),
	m_systemSoundVolume(1.0f),
	m_systemSound3DVolume(1.0f),
	m_systemSpeechVolume(1.0f),
	m_zoomVolume(1.0f),
	m_silentAudioEvent(nullptr),
	m_savedValues(nullptr),
	m_muteReasonBits(0),
	m_speechOn(TRUE),
	m_soundOn(TRUE),
	m_sound3DOn(TRUE),
	m_musicOn(TRUE),
	m_volumeHasChanged(FALSE),
	m_hardwareAccel(FALSE),
	m_surroundSpeakers(FALSE),
	m_disallowSpeech(FALSE)
{
	m_listenerPosition.zero();
	m_listenerOrientation.set(0.0f, 1.0f, 0.0f);
	theAudioHandlePool = AHSV_FirstHandle;
}

AudioManager::~AudioManager() = default;

AudioEventRTS::AudioEventRTS() :
	m_filenameToLoad(AsciiString::TheEmptyString),
	m_eventInfo(nullptr),
	m_playingHandle(0),
	m_killThisHandle(0),
	m_eventName(AsciiString::TheEmptyString),
	m_priority(AP_NORMAL),
	m_volume(-1.0f),
	m_timeOfDay(TIME_OF_DAY_AFTERNOON),
	m_ownerType(OT_INVALID),
	m_shouldFade(FALSE),
	m_isLogicalAudio(FALSE),
	m_uninterruptible(FALSE),
	m_pitchShift(1.0f),
	m_volumeShift(1.0f),
	m_delay(0.0f),
	m_loopCount(1),
	m_playingAudioIndex(-1),
	m_allCount(0),
	m_playerIndex(-1),
	m_portionToPlayNext(PP_Sound)
{
	m_positionOfAudio.zero();
}

AudioEventRTS::AudioEventRTS(const AsciiString &eventName) : AudioEventRTS()
{
	m_eventName = eventName;
}

AudioEventRTS::AudioEventRTS(const AudioEventRTS &right) : AudioEventRTS()
{
	*this = right;
}

AudioEventRTS &AudioEventRTS::operator=(const AudioEventRTS &right)
{
	if (this == &right) {
		return *this;
	}
	m_filenameToLoad = right.m_filenameToLoad;
	m_eventInfo = right.m_eventInfo;
	m_playingHandle = right.m_playingHandle;
	m_killThisHandle = right.m_killThisHandle;
	m_eventName = right.m_eventName;
	m_attackName = right.m_attackName;
	m_decayName = right.m_decayName;
	m_priority = right.m_priority;
	m_volume = right.m_volume;
	m_timeOfDay = right.m_timeOfDay;
	m_ownerType = right.m_ownerType;
	m_shouldFade = right.m_shouldFade;
	m_isLogicalAudio = right.m_isLogicalAudio;
	m_uninterruptible = right.m_uninterruptible;
	m_pitchShift = right.m_pitchShift;
	m_volumeShift = right.m_volumeShift;
	m_delay = right.m_delay;
	m_loopCount = right.m_loopCount;
	m_playingAudioIndex = right.m_playingAudioIndex;
	m_allCount = right.m_allCount;
	m_playerIndex = right.m_playerIndex;
	m_portionToPlayNext = right.m_portionToPlayNext;
	m_positionOfAudio = right.m_positionOfAudio;
	return *this;
}

AudioEventRTS::~AudioEventRTS() = default;
AsciiString AudioEventRTS::getFilename() { return m_filenameToLoad; }
Real AudioEventRTS::getVolumeShift() const { return m_volumeShift; }
AsciiString AudioEventRTS::getAttackFilename() const { return m_attackName; }
AsciiString AudioEventRTS::getDecayFilename() const { return m_decayName; }
Real AudioEventRTS::getDelay() const { return m_delay; }
void AudioEventRTS::decrementDelay(Real amount) { m_delay -= amount; }
PortionToPlay AudioEventRTS::getNextPlayPortion() const { return m_portionToPlayNext; }
void AudioEventRTS::setNextPlayPortion(PortionToPlay portion) { m_portionToPlayNext = portion; }
void AudioEventRTS::decreaseLoopCount() { if (m_loopCount > 0) --m_loopCount; }
Bool AudioEventRTS::hasMoreLoops() const { return m_loopCount != 0; }
void AudioEventRTS::setAudioEventInfo(const AudioEventInfo *info) const { m_eventInfo = info; }
const AudioEventInfo *AudioEventRTS::getAudioEventInfo() const { return m_eventInfo; }
void AudioEventRTS::setPlayingHandle(AudioHandle handle) { m_playingHandle = handle; }
AudioHandle AudioEventRTS::getPlayingHandle() { return m_playingHandle; }
AudioHandle AudioEventRTS::getHandleToKill() const { return m_killThisHandle; }
Bool AudioEventRTS::getIsLogicalAudio() const { return m_isLogicalAudio; }
void AudioEventRTS::setObjectID(ObjectID objectID) { m_objectID = objectID; m_ownerType = OT_Object; }
ObjectID AudioEventRTS::getObjectID() { return m_objectID; }
void AudioEventRTS::setPosition(const Coord3D *position)
{
	if (position != nullptr) {
		m_positionOfAudio = *position;
		m_ownerType = OT_Positional;
	}
}
const Coord3D *AudioEventRTS::getPosition() { return &m_positionOfAudio; }
Bool AudioEventRTS::isPositionalAudio() const { return m_ownerType == OT_Positional || m_ownerType == OT_Object || m_ownerType == OT_Drawable; }
Bool AudioEventRTS::isCurrentlyPlaying() const { return FALSE; }
AudioPriority AudioEventRTS::getAudioPriority() const { return m_priority; }
void AudioEventRTS::setAudioPriority(AudioPriority priority) { m_priority = priority; }
Real AudioEventRTS::getVolume() const { return m_volume < 0.0f && m_eventInfo != nullptr ? m_eventInfo->m_volume : m_volume; }
void AudioEventRTS::setVolume(Real volume) { m_volume = volume; }
const Coord3D *AudioEventRTS::getCurrentPosition() { return &m_positionOfAudio; }
Int AudioEventRTS::getPlayerIndex() const { return m_playerIndex; }
void AudioEventRTS::setPlayerIndex(Int index) { m_playerIndex = index; }

AudioRequest::~AudioRequest() = default;
AudioEventInfo::~AudioEventInfo() = default;

void AudioEventRTS::generateFilename()
{
	if (m_eventInfo != nullptr) {
		if (!m_eventInfo->m_filename.isEmpty()) {
			m_filenameToLoad = m_eventInfo->m_filename;
		} else if (!m_eventInfo->m_sounds.empty()) {
			m_filenameToLoad = m_eventInfo->m_sounds.front();
		}
	}
}

void AudioEventRTS::generatePlayInfo()
{
	if (m_eventInfo == nullptr) {
		return;
	}
	m_volumeShift = m_eventInfo->m_volumeShift;
	m_loopCount = m_eventInfo->m_loopCount;
	if (!m_eventInfo->m_attackSounds.empty()) {
		m_attackName = m_eventInfo->m_attackSounds.front();
	}
	if (!m_eventInfo->m_decaySounds.empty()) {
		m_decayName = m_eventInfo->m_decaySounds.front();
	}
	m_portionToPlayNext = m_attackName.isEmpty() ? PP_Sound : PP_Attack;
}

void AudioEventRTS::advanceNextPlayPortion()
{
	switch (m_portionToPlayNext) {
		case PP_Attack: m_portionToPlayNext = PP_Sound; break;
		case PP_Sound: m_portionToPlayNext = m_decayName.isEmpty() ? PP_Done : PP_Decay; break;
		case PP_Decay:
		case PP_Done: m_portionToPlayNext = PP_Done; break;
	}
}

void AudioManager::init() {}
void AudioManager::postProcessLoad() {}
void AudioManager::reset() {}
void AudioManager::update() {}

AudioHandle AudioManager::addAudioEvent(const AudioEventRTS *) { return AHSV_NoSound; }
void AudioManager::removeAudioEvent(AudioHandle) {}
Bool AudioManager::isValidAudioEvent(const AudioEventRTS *) const { return FALSE; }
Bool AudioManager::isValidAudioEvent(AudioEventRTS *) const { return FALSE; }
void AudioManager::setAudioEventEnabled(AsciiString, Bool) {}
void AudioManager::setAudioEventVolumeOverride(AsciiString, Real) {}
void AudioManager::removeAudioEvent(AsciiString) {}
void AudioManager::removeDisabledEvents() {}
void AudioManager::getInfoForAudioEvent(const AudioEventRTS *) const {}
Bool AudioManager::isCurrentlyPlaying(AudioHandle) { return FALSE; }

UnsignedInt AudioManager::translateSpeakerTypeToUnsignedInt(const AsciiString &) { return 0; }
AsciiString AudioManager::translateUnsignedIntToSpeakerType(UnsignedInt) { return AsciiString::TheEmptyString; }

Bool AudioManager::isOn(AudioAffect which) const
{
	if (which & AudioAffect_Music) return m_musicOn;
	if (which & AudioAffect_Sound3D) return m_sound3DOn;
	if (which & AudioAffect_Sound) return m_soundOn;
	return m_speechOn;
}

void AudioManager::setOn(Bool enabled, AudioAffect which)
{
	if (which & AudioAffect_Music) m_musicOn = enabled;
	if (which & AudioAffect_Sound) m_soundOn = enabled;
	if (which & AudioAffect_Sound3D) m_sound3DOn = enabled;
	if (which & AudioAffect_Speech) m_speechOn = enabled;
}

void AudioManager::setVolume(Real volume, AudioAffect which)
{
	if (which & AudioAffect_Music) m_musicVolume = volume;
	if (which & AudioAffect_Sound) m_soundVolume = volume;
	if (which & AudioAffect_Sound3D) m_sound3DVolume = volume;
	if (which & AudioAffect_Speech) m_speechVolume = volume;
}

Real AudioManager::getVolume(AudioAffect which)
{
	if (which & AudioAffect_Music) return m_musicVolume;
	if (which & AudioAffect_Sound3D) return m_sound3DVolume;
	if (which & AudioAffect_Sound) return m_soundVolume;
	return m_speechVolume;
}

void AudioManager::set3DVolumeAdjustment(Real volume)
{
	m_sound3DVolume = std::max(0.0f, std::min(1.0f, volume));
}

void AudioManager::setListenerPosition(const Coord3D *position, const Coord3D *orientation)
{
	if (position != nullptr) m_listenerPosition = *position;
	if (orientation != nullptr) m_listenerOrientation = *orientation;
}

const Coord3D *AudioManager::getListenerPosition() const { return &m_listenerPosition; }

AudioRequest *AudioManager::allocateAudioRequest() { return new AudioRequest; }
void AudioManager::releaseAudioRequest(AudioRequest *request) { deleteInstance(request); }
void AudioManager::appendAudioRequest(AudioRequest *request) { m_audioRequests.push_back(request); }
void AudioManager::processRequestList() {}

AudioEventInfo *AudioManager::newAudioEventInfo(AsciiString) { return nullptr; }
void AudioManager::addAudioEventInfo(AudioEventInfo *) {}
AudioEventInfo *AudioManager::findAudioEventInfo(AsciiString) const { return nullptr; }
void AudioManager::refreshCachedVariables() {}
Real AudioManager::getAudioLengthMS(const AudioEventRTS *) { return 0.0f; }
void AudioManager::findAllAudioEventsOfType(AudioType, std::vector<AudioEventInfo *> &) {}

Bool AudioManager::isCurrentProviderHardwareAccelerated() { return FALSE; }
Bool AudioManager::isCurrentSpeakerTypeSurroundSound() { return FALSE; }
Bool AudioManager::shouldPlayLocally(const AudioEventRTS *) { return TRUE; }
AudioHandle AudioManager::allocateNewHandle() { return theAudioHandlePool++; }
void AudioManager::removeLevelSpecificAudioEventInfos() {}

void AudioManager::removeAllAudioRequests()
{
	for (AudioRequest *request : m_audioRequests) {
		deleteInstance(request);
	}
	m_audioRequests.clear();
}

void AudioManager::addTrackName(const AsciiString &trackName) { m_musicTracks.push_back(trackName); }
AsciiString AudioManager::nextTrackName(const AsciiString &) { return m_musicTracks.empty() ? AsciiString::TheEmptyString : m_musicTracks.front(); }
AsciiString AudioManager::prevTrackName(const AsciiString &) { return m_musicTracks.empty() ? AsciiString::TheEmptyString : m_musicTracks.back(); }

Bool AudioManager::prepareAudioEventForPlayback(const AudioEventRTS *eventToAdd,
	RefCountPtr<DynamicAudioEventRTS> &preparedEvent, Bool forced)
{
	preparedEvent.Clear();
	if (eventToAdd == nullptr || eventToAdd->getEventName().isEmpty()
		|| eventToAdd->getEventName() == "NoSound"
		|| eventToAdd->getAudioEventInfo() == nullptr) {
		return FALSE;
	}
	const AudioEventInfo *info = eventToAdd->getAudioEventInfo();
	if (!forced) {
		if (info->m_soundType == AT_Music && !isOn(AudioAffect_Music)) return FALSE;
		if (info->m_soundType == AT_SoundEffect
			&& (!isOn(AudioAffect_Sound) || !isOn(AudioAffect_Sound3D))) return FALSE;
		if (info->m_soundType == AT_Streaming
			&& (getDisallowSpeech() || !isOn(AudioAffect_Speech))) return FALSE;
		if (!eventToAdd->getIsLogicalAudio() && !eventToAdd->getUninterruptible()
			&& !shouldPlayLocally(eventToAdd)) return FALSE;
	}
	preparedEvent.Assign_No_Add_Ref(newInstance(DynamicAudioEventRTS)(*eventToAdd));
	preparedEvent->setPlayingHandle(allocateNewHandle());
	preparedEvent->generateFilename();
	eventToAdd->setPlayingAudioIndex(preparedEvent->getPlayingAudioIndex());
	preparedEvent->generatePlayInfo();
	for (const std::pair<AsciiString, Real> &adjustment : m_adjustedVolumes) {
		if (adjustment.first == preparedEvent->getEventName()) {
			preparedEvent->setVolume(adjustment.second);
			break;
		}
	}
	return TRUE;
}
