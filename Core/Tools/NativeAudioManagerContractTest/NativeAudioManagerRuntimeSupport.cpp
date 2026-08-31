#include "Common/AudioAffect.h"
#include "Common/AudioEventInfo.h"
#include "Common/AudioEventRTS.h"
#include "Common/AudioRequest.h"
#include "Common/AudioSettings.h"
#include "Common/GameAudio.h"
#include "Common/FileSystem.h"
#include "Common/SubsystemInterface.h"

#include <algorithm>

AudioManager *TheAudio = nullptr;
Bool g_nativeAudioShroudedForTest = FALSE;
Bool g_nativeAudioDeadObjectForTest = FALSE;
Bool g_nativeAudioNullPositionForTest = FALSE;
Coord3D g_nativeAudioObjectPositionForTest = { 0.0f, 0.0f, 0.0f };
Bool isAudioEventShroudedForLocalPlayer(const Coord3D *)
{
	return g_nativeAudioShroudedForTest;
}
class FileSystem;
FileSystem *TheFileSystem = nullptr;

File *FileSystem::openFile(const Char *, Int, size_t, FileInstance)
{
	return nullptr;
}
class View;
View *TheTacticalView = nullptr;
int g_nativeAudioBaseUpdateCalls = 0;
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
	m_objectID(INVALID_ID),
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
	if (m_ownerType == OT_Object) {
		m_objectID = right.m_objectID;
	} else if (m_ownerType == OT_Drawable) {
		m_drawableID = right.m_drawableID;
	}
	return *this;
}

AudioEventRTS::~AudioEventRTS() = default;
AsciiString AudioEventRTS::getFilename() { return m_filenameToLoad; }
Real AudioEventRTS::getVolumeShift() const { return m_volumeShift; }
Real AudioEventRTS::getPitchShift() const { return m_pitchShift; }
AsciiString AudioEventRTS::getAttackFilename() const { return m_attackName; }
AsciiString AudioEventRTS::getDecayFilename() const { return m_decayName; }
Real AudioEventRTS::getDelay() const { return m_delay; }
void AudioEventRTS::decrementDelay(Real amount) { m_delay -= amount; }
PortionToPlay AudioEventRTS::getNextPlayPortion() const { return m_portionToPlayNext; }
void AudioEventRTS::setNextPlayPortion(PortionToPlay portion) { m_portionToPlayNext = portion; }
void AudioEventRTS::decreaseLoopCount()
{
	if (m_loopCount == 1) {
		m_loopCount = -1;
	} else if (m_loopCount > 1) {
		--m_loopCount;
	}
}
Bool AudioEventRTS::hasMoreLoops() const { return m_loopCount >= 0; }
void AudioEventRTS::setAudioEventInfo(const AudioEventInfo *info) const { m_eventInfo = info; }
const AudioEventInfo *AudioEventRTS::getAudioEventInfo() const { return m_eventInfo; }
void AudioEventRTS::setPlayingHandle(AudioHandle handle) { m_playingHandle = handle; }
AudioHandle AudioEventRTS::getPlayingHandle() { return m_playingHandle; }
void AudioEventRTS::setHandleToKill(AudioHandle handle) { m_killThisHandle = handle; }
AudioHandle AudioEventRTS::getHandleToKill() const { return m_killThisHandle; }
Bool AudioEventRTS::getIsLogicalAudio() const { return m_isLogicalAudio; }
void AudioEventRTS::setObjectID(ObjectID objectID)
{
	if (m_ownerType == OT_Object || m_ownerType == OT_INVALID) {
		m_objectID = objectID;
		m_ownerType = OT_Object;
	}
}
ObjectID AudioEventRTS::getObjectID() { return m_ownerType == OT_Object ? m_objectID : INVALID_ID; }
void AudioEventRTS::setPosition(const Coord3D *position)
{
	if (position != nullptr && (m_ownerType == OT_Positional || m_ownerType == OT_INVALID)) {
		m_positionOfAudio = *position;
		m_ownerType = OT_Positional;
	}
}
const Coord3D *AudioEventRTS::getPosition() { return m_ownerType == OT_INVALID ? nullptr : &m_positionOfAudio; }
Bool AudioEventRTS::isPositionalAudio() const
{
	if (m_eventInfo != nullptr && !BitIsSet(m_eventInfo->m_type, ST_WORLD)) {
		return FALSE;
	}
	return m_ownerType != OT_INVALID
		&& (m_ownerType == OT_Positional || m_objectID != INVALID_ID);
}
Bool AudioEventRTS::isCurrentlyPlaying() const { return FALSE; }
AudioPriority AudioEventRTS::getAudioPriority() const { return m_priority; }
void AudioEventRTS::setAudioPriority(AudioPriority priority) { m_priority = priority; }
Real AudioEventRTS::getVolume() const { return m_volume < 0.0f && m_eventInfo != nullptr ? m_eventInfo->m_volume : m_volume; }
void AudioEventRTS::setVolume(Real volume) { m_volume = volume; }
const Coord3D *AudioEventRTS::getCurrentPosition()
{
	if (g_nativeAudioNullPositionForTest) {
		return nullptr;
	}
	switch (m_ownerType) {
		case OT_Object:
		case OT_Drawable:
			if (g_nativeAudioDeadObjectForTest) {
				m_ownerType = OT_Dead;
			} else {
				m_positionOfAudio = g_nativeAudioObjectPositionForTest;
			}
			return &m_positionOfAudio;
		case OT_Positional:
		case OT_Dead:
			return &m_positionOfAudio;
		default:
			return nullptr;
	}
}
Int AudioEventRTS::getPlayerIndex() const { return m_playerIndex; }
void AudioEventRTS::setPlayerIndex(Int index) { m_playerIndex = index; }

AudioRequest::~AudioRequest() = default;
AudioEventInfo::~AudioEventInfo() = default;

void AudioEventRTS::generateFilename()
{
	if (m_eventInfo != nullptr) {
		// Fixtures supply resolved paths, but the source field must match the
		// production event type: streams never fall back to the sample list.
		if (m_eventInfo->m_soundType == AT_Music || m_eventInfo->m_soundType == AT_Streaming) {
			m_filenameToLoad = m_eventInfo->m_filename;
		} else if (!m_eventInfo->m_sounds.empty()) {
			m_playingAudioIndex = (m_playingAudioIndex + 1)
				% static_cast<Int>(m_eventInfo->m_sounds.size());
			m_filenameToLoad = m_eventInfo->m_sounds[m_playingAudioIndex];
		} else {
			m_filenameToLoad = AsciiString::TheEmptyString;
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
void AudioManager::update()
{
	++g_nativeAudioBaseUpdateCalls;
	m_zoomVolume *= 0.5f;
	set3DVolumeAdjustment(m_zoomVolume);
}

AudioHandle AudioManager::addAudioEvent(const AudioEventRTS *) { return AHSV_NoSound; }
void AudioManager::removeAudioEvent(AudioHandle) {}
Bool AudioManager::isValidAudioEvent(const AudioEventRTS *) const { return FALSE; }
Bool AudioManager::isValidAudioEvent(AudioEventRTS *) const { return FALSE; }
void AudioManager::setAudioEventEnabled(AsciiString, Bool) {}
void AudioManager::setAudioEventVolumeOverride(AsciiString eventToAffect, Real newVolume)
{
	if (eventToAffect == AsciiString::TheEmptyString) {
		m_adjustedVolumes.clear();
		return;
	}
	if (newVolume != -1.0f) {
		adjustVolumeOfPlayingAudio(eventToAffect, newVolume);
	}
	for (std::list<std::pair<AsciiString, Real> >::iterator it = m_adjustedVolumes.begin();
		it != m_adjustedVolumes.end(); ++it) {
		if (it->first == eventToAffect) {
			if (newVolume == -1.0f) {
				m_adjustedVolumes.erase(it);
			}
			else {
				it->second = newVolume;
			}
			return;
		}
	}
	if (newVolume != -1.0f) {
		m_adjustedVolumes.push_front(std::make_pair(eventToAffect, newVolume));
	}
}
void AudioManager::removeAudioEvent(AsciiString) {}
void AudioManager::removeDisabledEvents() {}
void AudioManager::getInfoForAudioEvent(const AudioEventRTS *event) const
{
	if (event != nullptr && event->getAudioEventInfo() == nullptr) {
		event->setAudioEventInfo(findAudioEventInfo(event->getEventName()));
	}
}
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
	if (which & AudioAffect_Music) {
		if (which & AudioAffect_SystemSetting) m_systemMusicVolume = volume;
		else m_scriptMusicVolume = volume;
		m_musicVolume = m_scriptMusicVolume * m_systemMusicVolume;
	}
	if (which & AudioAffect_Sound) {
		if (which & AudioAffect_SystemSetting) m_systemSoundVolume = volume;
		else m_scriptSoundVolume = volume;
		m_soundVolume = m_scriptSoundVolume * m_systemSoundVolume;
	}
	if (which & AudioAffect_Sound3D) {
		if (which & AudioAffect_SystemSetting) m_systemSound3DVolume = volume;
		else m_scriptSound3DVolume = volume;
		m_sound3DVolume = m_scriptSound3DVolume * m_systemSound3DVolume;
	}
	if (which & AudioAffect_Speech) {
		if (which & AudioAffect_SystemSetting) m_systemSpeechVolume = volume;
		else m_scriptSpeechVolume = volume;
		m_speechVolume = m_scriptSpeechVolume * m_systemSpeechVolume;
	}
	m_volumeHasChanged = TRUE;
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
	m_sound3DVolume = std::max(0.0f, std::min(1.0f,
		volume * m_scriptSound3DVolume * m_systemSound3DVolume));
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

AudioEventInfo *AudioManager::newAudioEventInfo(AsciiString name)
{
	AudioEventInfo *info = newInstance(AudioEventInfo);
	info->m_audioName = name;
	m_allAudioEventInfo[name] = info;
	return info;
}

void AudioManager::addAudioEventInfo(AudioEventInfo *info)
{
	if (info != nullptr) {
		m_allAudioEventInfo[info->m_audioName] = info;
	}
}

AudioEventInfo *AudioManager::findAudioEventInfo(AsciiString name) const
{
	AudioEventInfoHash::const_iterator it = m_allAudioEventInfo.find(name);
	return it == m_allAudioEventInfo.end() ? nullptr : it->second;
}
void AudioManager::refreshCachedVariables() {}
const AudioSettings *AudioManager::getAudioSettings() const { return m_audioSettings; }
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
AsciiString AudioManager::nextTrackName(const AsciiString &currentTrack)
{
	if (m_musicTracks.empty()) {
		return AsciiString::TheEmptyString;
	}
	std::vector<AsciiString>::const_iterator it = m_musicTracks.begin();
	for (; it != m_musicTracks.end(); ++it) {
		if (*it == currentTrack) {
			break;
		}
	}
	if (it != m_musicTracks.end()) {
		++it;
	}
	if (it == m_musicTracks.end()) {
		it = m_musicTracks.begin();
	}
	return *it;
}

AsciiString AudioManager::prevTrackName(const AsciiString &currentTrack)
{
	if (m_musicTracks.empty()) {
		return AsciiString::TheEmptyString;
	}
	std::vector<AsciiString>::const_reverse_iterator it = m_musicTracks.rbegin();
	for (; it != m_musicTracks.rend(); ++it) {
		if (*it == currentTrack) {
			break;
		}
	}
	if (it != m_musicTracks.rend()) {
		++it;
	}
	if (it == m_musicTracks.rend()) {
		it = m_musicTracks.rbegin();
	}
	return *it;
}

Bool AudioManager::prepareAudioEventForPlayback(const AudioEventRTS *eventToAdd,
	RefCountPtr<DynamicAudioEventRTS> &preparedEvent, Bool forced)
{
	preparedEvent.Clear();
	if (eventToAdd == nullptr || eventToAdd->getEventName().isEmpty()
		|| eventToAdd->getEventName() == "NoSound") {
		return FALSE;
	}
	if (eventToAdd->getAudioEventInfo() == nullptr) {
		getInfoForAudioEvent(eventToAdd);
	}
	if (eventToAdd->getAudioEventInfo() == nullptr) {
		return FALSE;
	}
	const AudioEventInfo *info = eventToAdd->getAudioEventInfo();
	if (!forced) {
		if (info->m_soundType == AT_Music && !isOn(AudioAffect_Music)) return FALSE;
		if (info->m_soundType == AT_SoundEffect
			&& (eventToAdd->isPositionalAudio()
				? !isOn(AudioAffect_Sound3D) : !isOn(AudioAffect_Sound))) return FALSE;
		if (info->m_soundType == AT_Streaming
			&& ((getDisallowSpeech() && !eventToAdd->getUninterruptible())
				|| !isOn(AudioAffect_Speech))) return FALSE;
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
	if (!forced && m_audioSettings != nullptr
		&& preparedEvent->getVolume() < m_audioSettings->m_minVolume) {
		preparedEvent.Clear();
		return FALSE;
	}
	return TRUE;
}
