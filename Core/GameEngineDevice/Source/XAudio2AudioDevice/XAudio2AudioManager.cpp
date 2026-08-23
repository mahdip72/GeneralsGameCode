#include "XAudio2AudioDevice/XAudio2AudioManager.h"

#include "Common/AudioEventInfo.h"
#include "Common/AudioAffect.h"
#include "Common/AudioHandleSpecialValues.h"
#include "Common/AudioRequest.h"
#include "Common/AudioSettings.h"
#include "Common/GameCommon.h"

#include <algorithm>

class View;
extern View *TheTacticalView;

namespace
{
constexpr Real LOGIC_FRAME_MS = MSEC_PER_LOGICFRAME_REAL;
constexpr UnsignedInt DEFAULT_2D_CHANNELS = 32;
constexpr UnsignedInt DEFAULT_3D_CHANNELS = 32;
constexpr UnsignedInt DEFAULT_STREAM_CHANNELS = 8;
constexpr UnsignedInt PCM_MAX_FRAMES = 48000;

XAudio2AudioManager::Channel channelFor(const AudioEventRTS &event)
{
	const AudioEventInfo *info = event.getAudioEventInfo();
	if (info == nullptr || info->m_soundType == AT_Music || info->m_soundType == AT_Streaming) {
		return XAudio2AudioManager::Channel::STREAM;
	}
	return event.isPositionalAudio()
		? XAudio2AudioManager::Channel::SAMPLE_3D
		: XAudio2AudioManager::Channel::SAMPLE_2D;
}

Bool isMusic(const AudioEventRTS &event)
{
	const AudioEventInfo *info = event.getAudioEventInfo();
	return info != nullptr && info->m_soundType == AT_Music;
}
}

XAudio2AudioManager::XAudio2AudioManager() :
	XAudio2AudioManager(nullptr, nullptr)
{
}

XAudio2AudioManager::XAudio2AudioManager(XAudio2AudioService *service,
	AudioAssetSource *assetSource) :
	m_service(service),
	m_assetSource(assetSource),
	m_lifecycleGeneration(1),
	m_num2DSamples(DEFAULT_2D_CHANNELS),
	m_num3DSamples(DEFAULT_3D_CHANNELS),
	m_numStreams(DEFAULT_STREAM_CHANNELS),
	m_speakerType(0),
	m_providerSelected(TRUE),
	m_open(FALSE),
	m_admissionOpen(FALSE),
	m_ambientPaused(FALSE)
{
}

XAudio2AudioManager::~XAudio2AudioManager()
{
	closeDevice();
}

void XAudio2AudioManager::setService(XAudio2AudioService *service)
{
	if (m_service == service) {
		return;
	}
	closeDevice();
	m_ownedService.reset();
	m_service = service;
}

void XAudio2AudioManager::init()
{
	AudioManager::init();
	if (m_audioSettings != nullptr) {
		m_num2DSamples = static_cast<UnsignedInt>(m_audioSettings->m_sampleCount2D);
		m_num3DSamples = static_cast<UnsignedInt>(m_audioSettings->m_sampleCount3D);
		m_numStreams = static_cast<UnsignedInt>(m_audioSettings->m_streamCount);
		m_speakerType = m_audioSettings->m_defaultSpeakerType3D;
	}
	openDevice();
}

void XAudio2AudioManager::postProcessLoad()
{
	AudioManager::postProcessLoad();
}

void XAudio2AudioManager::reset()
{
	// Close admission and quiesce all callbacks before releasing event metadata.
	closeDevice();
	AudioManager::reset();
	removeAllAudioRequests();
	removeLevelSpecificAudioEventInfos();
	++m_lifecycleGeneration;
	if (m_lifecycleGeneration == 0) {
		m_lifecycleGeneration = 1;
	}
}

void XAudio2AudioManager::update()
{
	if (TheTacticalView != nullptr) {
		AudioManager::update();
	} else {
		m_zoomVolume = 1.0f;
		set3DVolumeAdjustment(1.0f);
	}

	// The owner update order is deliberate: callbacks are serviced, records are
	// drained, then requests and fades are admitted on this same thread.
	if (m_service != nullptr) {
		m_service->serviceVoices();
	}
	drainCompletions();
	processActiveAudio();
	processFades();
	processRequestList();
}

DynamicAudioEventRTS *XAudio2AudioManager::copyEvent(const AudioEventRTS *eventToCopy)
{
	if (eventToCopy == nullptr) {
		return nullptr;
	}
	if (eventToCopy->getAudioEventInfo() == nullptr) {
		getInfoForAudioEvent(eventToCopy);
	}
	if (eventToCopy->getAudioEventInfo() == nullptr) {
		return nullptr;
	}

	DynamicAudioEventRTS *event = newInstance(DynamicAudioEventRTS)(*eventToCopy);
	event->setPlayingHandle(allocateNewHandle());
	if (event->getFilename().isEmpty()) {
		event->generateFilename();
	}
	event->generatePlayInfo();
	return event;
}

void XAudio2AudioManager::enqueuePlay(DynamicAudioEventRTS *event, Bool forced)
{
	if (event == nullptr) {
		return;
	}
	AudioRequest *request = allocateAudioRequest();
	request->m_pendingEvent.Assign_No_Add_Ref(event);
	request->m_requiresCheckForSample = TRUE;
	if (forced) {
		m_forcePlayHandles.push_back(event->getPlayingHandle());
	}
	appendAudioRequest(request);
}

AudioHandle XAudio2AudioManager::addAudioEvent(const AudioEventRTS *eventToAdd)
{
	if (eventToAdd == nullptr || eventToAdd->getEventName().isEmpty()
		|| eventToAdd->getEventName() == "NoSound" || !m_admissionOpen) {
		return AHSV_NoSound;
	}
	DynamicAudioEventRTS *event = copyEvent(eventToAdd);
	if (event == nullptr) {
		return AHSV_Error;
	}
	const AudioHandle handle = event->getPlayingHandle();
	enqueuePlay(event, FALSE);
	return handle;
}

void XAudio2AudioManager::friend_forcePlayAudioEventRTS(const AudioEventRTS *eventToPlay)
{
	DynamicAudioEventRTS *event = copyEvent(eventToPlay);
	if (event != nullptr) {
		enqueuePlay(event, TRUE);
	}
}

void XAudio2AudioManager::processRequestList()
{
	for (std::list<AudioRequest *>::iterator it = m_audioRequests.begin();
		it != m_audioRequests.end();) {
		AudioRequest *request = *it;
		if (request->m_pendingEvent != nullptr
			&& request->m_pendingEvent->getDelay() >= LOGIC_FRAME_MS) {
			request->m_pendingEvent->decrementDelay(LOGIC_FRAME_MS);
			request->m_requiresCheckForSample = TRUE;
			++it;
			continue;
		}

		if (request->m_request == AR_Play) {
			processPlayRequest(request);
		} else if (request->m_request == AR_Stop) {
			processStopRequest(request->m_handleToInteractOn);
		} else if (request->m_request == AR_Pause) {
			processPauseRequest(request->m_handleToInteractOn);
		}
		releaseAudioRequest(request);
		it = m_audioRequests.erase(it);
	}
}

void XAudio2AudioManager::processPlayRequest(AudioRequest *request)
{
	if (request == nullptr || request->m_pendingEvent == nullptr || !m_admissionOpen) {
		return;
	}
	DynamicAudioEventRTS *event = request->m_pendingEvent.Peek();
	if (event == nullptr || event->getAudioEventInfo() == nullptr) {
		return;
	}
	if (event->getHandleToKill() != AHSV_Error && event->getHandleToKill() != 0) {
		processStopRequest(event->getHandleToKill());
	}

	PlayingAudio playing;
	playing.event = request->m_pendingEvent;
	playing.channel = channelFor(*event);
	playing.phase = event->getNextPlayPortion();
	playing.generation = m_lifecycleGeneration;
	std::vector<AudioHandle>::iterator forced = std::find(m_forcePlayHandles.begin(),
		m_forcePlayHandles.end(), event->getPlayingHandle());
	playing.forced = forced != m_forcePlayHandles.end();
	if (forced != m_forcePlayHandles.end()) {
		m_forcePlayHandles.erase(forced);
	}

	if (!playing.forced && isChannelFull(playing.channel)) {
		PlayingAudio *victim = findLowestPriority(playing.channel, event->getAudioPriority());
		if (victim == nullptr) {
			return;
		}
		finishPlaying(*victim);
	}

	m_playing.push_back(playing);
	startNextPhase(m_playing.back());
}

void XAudio2AudioManager::processStopRequest(AudioHandle handle)
{
	if (handle == AHSV_StopTheMusic || handle == AHSV_StopTheMusicFade) {
		for (PlayingAudio &playing : m_playing) {
			if (isMusic(*playing.event)) {
				if (handle == AHSV_StopTheMusicFade) {
					playing.fadeFrames = 1;
				} else {
					playing.stopping = TRUE;
				}
			}
		}
		return;
	}
	PlayingAudio *playing = findPlaying(handle);
	if (playing != nullptr) {
		playing->stopping = TRUE;
	}
}

void XAudio2AudioManager::processPauseRequest(AudioHandle handle)
{
	PlayingAudio *playing = findPlaying(handle);
	if (playing == nullptr) {
		return;
	}
	playing->paused = TRUE;
	if (m_service != nullptr && playing->voiceOpen) {
		m_service->pauseVoice(playing->voice);
	}
}

void XAudio2AudioManager::startNextPhase(PlayingAudio &playing)
{
	if (playing.event == nullptr) {
		playing.phase = PP_Done;
		return;
	}
	playing.phase = playing.event->getNextPlayPortion();
	while (playing.phase != PP_Done) {
		AsciiString fileName;
		if (playing.phase == PP_Attack) {
			fileName = playing.event->getAttackFilename();
		} else if (playing.phase == PP_Decay) {
			fileName = playing.event->getDecayFilename();
		} else {
			fileName = playing.event->getFilename();
		}
		if (fileName.isEmpty()) {
			playing.event->advanceNextPlayPortion();
			playing.phase = playing.event->getNextPlayPortion();
			continue;
		}

		playing.phaseDurationMS = 0.0f;
		if (m_assetSource == nullptr
			|| !m_assetSource->getDurationMS(fileName, playing.phaseDurationMS)) {
			// Unknown assets are not fabricated.  A known catalog entry is required
			// for native PCM, while logical metadata still remains deterministic.
			playing.phaseDurationMS = 0.0f;
		}
		playing.phaseRemainingMS = playing.phaseDurationMS;
		if (playing.phaseDurationMS > 0.0f) {
			if (!ensureVoice(playing)) {
				playing.voiceOpen = FALSE;
			}
			if (playing.voiceOpen) {
				submitPhase(playing);
			}
			return;
		}
		playing.event->advanceNextPlayPortion();
		playing.phase = playing.event->getNextPlayPortion();
	}
}

Bool XAudio2AudioManager::ensureVoice(PlayingAudio &playing)
{
	if (playing.voiceOpen || m_service == nullptr || !m_service->isOpen()) {
		return playing.voiceOpen;
	}
	playing.voice = m_service->createVoice();
	playing.voiceOpen = playing.voice.isValid();
	return playing.voiceOpen;
}

Bool XAudio2AudioManager::submitPhase(PlayingAudio &playing)
{
	if (!playing.voiceOpen || m_assetSource == nullptr) {
		return FALSE;
	}
	AsciiString fileName;
	if (playing.phase == PP_Attack) {
		fileName = playing.event->getAttackFilename();
	} else if (playing.phase == PP_Decay) {
		fileName = playing.event->getDecayFilename();
	} else {
		fileName = playing.event->getFilename();
	}
	AudioPcmChunk chunk;
	if (!m_assetSource->decodePcm(fileName, chunk, PCM_MAX_FRAMES)) {
		return FALSE;
	}
	chunk.generation = playing.generation;
	chunk.sequence = playing.voiceSequence++;
	chunk.startSample = 0;
	if (m_service->submit(playing.voice, std::move(chunk)) != AudioPcmSubmitResult::ACCEPTED) {
		return FALSE;
	}
	m_service->setVoiceVolume(playing.voice, effectiveVolume(playing));
	return TRUE;
}

void XAudio2AudioManager::drainCompletions()
{
	if (m_service == nullptr) {
		return;
	}
	XAudio2AudioCompletion completion;
	while (m_service->tryPopCompletion(completion)) {
		if (completion.generation != m_lifecycleGeneration) {
			continue;
		}
		for (PlayingAudio &playing : m_playing) {
			if (playing.voiceOpen && playing.voice == completion.voice
				&& playing.generation == completion.generation
				&& completion.sequence + 1 >= playing.voiceSequence) {
				playing.phaseRemainingMS = 0.0f;
				break;
			}
		}
	}
}

void XAudio2AudioManager::processActiveAudio()
{
	for (std::size_t index = 0; index < m_playing.size();) {
		PlayingAudio &playing = m_playing[index];
		if (!playing.paused) {
			playing.phaseRemainingMS -= LOGIC_FRAME_MS;
		}
		if (!playing.stopping && playing.phaseRemainingMS <= 0.0f
			&& playing.event->getDelay() > 0.0f) {
			playing.event->decrementDelay(LOGIC_FRAME_MS);
			++index;
			continue;
		}
		if (playing.stopping || playing.phaseRemainingMS <= 0.0f) {
			if (playing.stopping) {
				finishPlaying(playing);
				m_playing.erase(m_playing.begin() + index);
				continue;
			}
			playing.event->advanceNextPlayPortion();
			if (playing.event->getNextPlayPortion() == PP_Done
				&& playing.event->hasMoreLoops()) {
				playing.event->decreaseLoopCount();
				playing.event->generateFilename();
				playing.event->setNextPlayPortion(PP_Sound);
			}
			if (playing.event->getNextPlayPortion() == PP_Done) {
				finishPlaying(playing);
				m_playing.erase(m_playing.begin() + index);
				continue;
			}
			startNextPhase(playing);
		}
		++index;
	}
}

void XAudio2AudioManager::processFades()
{
	for (PlayingAudio &playing : m_playing) {
		if (playing.fadeFrames <= 0) {
			continue;
		}
		++playing.fadeFrames;
		playing.fadeVolume = 1.0f / static_cast<Real>(playing.fadeFrames);
		if (playing.voiceOpen && m_service != nullptr) {
			m_service->setVoiceVolume(playing.voice, effectiveVolume(playing));
		}
		if (playing.fadeFrames > 30) {
			playing.stopping = TRUE;
		}
	}
}

void XAudio2AudioManager::releaseVoice(PlayingAudio &playing)
{
	if (m_service != nullptr && playing.voiceOpen) {
		m_service->stopVoice(playing.voice);
		m_service->destroyVoice(playing.voice);
	}
	playing.voiceOpen = FALSE;
	playing.voice = {};
}

void XAudio2AudioManager::finishPlaying(PlayingAudio &playing)
{
	recordMusicCompletion(playing);
	releaseVoice(playing);
	playing.phase = PP_Done;
}

void XAudio2AudioManager::clearPlaying()
{
	for (PlayingAudio &playing : m_playing) {
		releaseVoice(playing);
	}
	m_playing.clear();
	m_forcePlayHandles.clear();
}

void XAudio2AudioManager::stopAudio(AudioAffect which)
{
	for (PlayingAudio &playing : m_playing) {
		const Bool music = isMusic(*playing.event);
		const Bool speech = playing.event->getAudioEventInfo()->m_soundType == AT_Streaming;
		const Bool sound = !music && !speech;
		if ((music && BitIsSet(which, AudioAffect_Music))
			|| (speech && BitIsSet(which, AudioAffect_Speech))
			|| (sound && (BitIsSet(which, AudioAffect_Sound) || BitIsSet(which, AudioAffect_Sound3D)))) {
			playing.stopping = TRUE;
		}
	}
}

void XAudio2AudioManager::pauseAudio(AudioAffect which)
{
	for (PlayingAudio &playing : m_playing) {
		const Bool music = isMusic(*playing.event);
		const Bool speech = playing.event->getAudioEventInfo()->m_soundType == AT_Streaming;
		const Bool sound = !music && !speech;
		if ((music && BitIsSet(which, AudioAffect_Music))
			|| (speech && BitIsSet(which, AudioAffect_Speech))
			|| (sound && (BitIsSet(which, AudioAffect_Sound) || BitIsSet(which, AudioAffect_Sound3D)))) {
			playing.paused = TRUE;
			if (m_service != nullptr && playing.voiceOpen) {
				m_service->pauseVoice(playing.voice);
			}
		}
	}
	for (std::list<AudioRequest *>::iterator it = m_audioRequests.begin(); it != m_audioRequests.end();) {
		if ((*it)->m_request == AR_Play) {
			releaseAudioRequest(*it);
			it = m_audioRequests.erase(it);
		} else {
			++it;
		}
	}
}

void XAudio2AudioManager::resumeAudio(AudioAffect which)
{
	for (PlayingAudio &playing : m_playing) {
		const Bool music = isMusic(*playing.event);
		const Bool speech = playing.event->getAudioEventInfo()->m_soundType == AT_Streaming;
		const Bool sound = !music && !speech;
		if ((music && BitIsSet(which, AudioAffect_Music))
			|| (speech && BitIsSet(which, AudioAffect_Speech))
			|| (sound && (BitIsSet(which, AudioAffect_Sound) || BitIsSet(which, AudioAffect_Sound3D)))) {
			playing.paused = FALSE;
			if (m_service != nullptr && playing.voiceOpen) {
				m_service->resumeVoice(playing.voice);
			}
		}
	}
}

void XAudio2AudioManager::pauseAmbient(Bool shouldPause)
{
	m_ambientPaused = shouldPause;
}

void XAudio2AudioManager::killAudioEventImmediately(AudioHandle audioEvent)
{
	for (std::list<AudioRequest *>::iterator it = m_audioRequests.begin(); it != m_audioRequests.end();) {
		AudioRequest *request = *it;
		if (request->m_pendingEvent != nullptr && request->m_pendingEvent->getPlayingHandle() == audioEvent) {
			releaseAudioRequest(request);
			it = m_audioRequests.erase(it);
		} else {
			++it;
		}
	}
	PlayingAudio *playing = findPlaying(audioEvent);
	if (playing != nullptr) {
		playing->stopping = TRUE;
	}
}

AsciiString XAudio2AudioManager::nextMusicTrack()
{
	m_activeMusicTrack = nextTrackName(m_activeMusicTrack);
	return m_activeMusicTrack;
}

AsciiString XAudio2AudioManager::prevMusicTrack()
{
	m_activeMusicTrack = prevTrackName(m_activeMusicTrack);
	return m_activeMusicTrack;
}

Bool XAudio2AudioManager::isMusicPlaying() const
{
	for (const PlayingAudio &playing : m_playing) {
		if (isMusic(*playing.event) && !playing.stopping) {
			return TRUE;
		}
	}
	return FALSE;
}

Bool XAudio2AudioManager::hasMusicTrackCompleted(const AsciiString &trackName, Int numberOfTimes) const
{
	for (const std::pair<AsciiString, Int> &completion : m_musicCompletions) {
		if (completion.first == trackName) {
			return completion.second >= numberOfTimes;
		}
	}
	return FALSE;
}

void XAudio2AudioManager::openDevice()
{
	if (m_service == nullptr) {
		m_ownedService = std::make_unique<XAudio2AudioService>();
		m_service = m_ownedService.get();
	}
	m_open = m_service->open();
	m_admissionOpen = m_open;
}

void XAudio2AudioManager::closeDevice()
{
	m_admissionOpen = FALSE;
	clearPlaying();
	if (m_service != nullptr) {
		m_service->shutdown();
	}
	m_open = FALSE;
}

AsciiString XAudio2AudioManager::getProviderName(UnsignedInt providerNum) const
{
	return providerNum == 0 ? AsciiString("XAudio2") : AsciiString::TheEmptyString;
}

UnsignedInt XAudio2AudioManager::getProviderIndex(AsciiString providerName) const
{
	return providerName == "XAudio2" ? 0U : PROVIDER_ERROR;
}

void XAudio2AudioManager::selectProvider(UnsignedInt providerNum)
{
	m_providerSelected = providerNum == 0;
}

void XAudio2AudioManager::unselectProvider()
{
	m_providerSelected = FALSE;
}

UnsignedInt XAudio2AudioManager::channelCount(Channel channel) const
{
	UnsignedInt count = 0;
	for (const PlayingAudio &playing : m_playing) {
		if (playing.channel == channel) {
			++count;
		}
	}
	return count;
}

UnsignedInt XAudio2AudioManager::channelLimit(Channel channel) const
{
	switch (channel) {
		case Channel::SAMPLE_2D: return m_num2DSamples;
		case Channel::SAMPLE_3D: return m_num3DSamples;
		case Channel::STREAM: return m_numStreams;
	}
	return 0;
}

Bool XAudio2AudioManager::isChannelFull(Channel channel) const
{
	return channelCount(channel) >= channelLimit(channel);
}

UnsignedInt XAudio2AudioManager::getNumAvailable2DSamples() const
{
	const UnsignedInt used = channelCount(Channel::SAMPLE_2D);
	return used >= m_num2DSamples ? 0U : m_num2DSamples - used;
}

UnsignedInt XAudio2AudioManager::getNumAvailable3DSamples() const
{
	const UnsignedInt used = channelCount(Channel::SAMPLE_3D);
	return used >= m_num3DSamples ? 0U : m_num3DSamples - used;
}

Bool XAudio2AudioManager::doesViolateLimit(AudioEventRTS *event) const
{
	return event != nullptr && isChannelFull(channelFor(*event));
}

Bool XAudio2AudioManager::isPlayingLowerPriority(AudioEventRTS *event) const
{
	if (event == nullptr || event->getAudioEventInfo() == nullptr) {
		return FALSE;
	}
	const Channel channel = channelFor(*event);
	for (const PlayingAudio &playing : m_playing) {
		if (playing.channel == channel && playing.event->getAudioPriority() < event->getAudioPriority()) {
			return TRUE;
		}
	}
	return FALSE;
}

Bool XAudio2AudioManager::isPlayingAlready(AudioEventRTS *event) const
{
	return event != nullptr && findPlaying(event->getPlayingHandle()) != nullptr;
}

Bool XAudio2AudioManager::isObjectPlayingVoice(UnsignedInt objID) const
{
	for (const PlayingAudio &playing : m_playing) {
		if (playing.event->getAudioEventInfo() != nullptr
			&& BitIsSet(playing.event->getAudioEventInfo()->m_type, ST_VOICE)
			&& playing.event->getObjectID() == objID) {
			return TRUE;
		}
	}
	return FALSE;
}

void XAudio2AudioManager::adjustVolumeOfPlayingAudio(AsciiString eventName, Real newVolume)
{
	for (PlayingAudio &playing : m_playing) {
		if (playing.event->getEventName() == eventName) {
			playing.volume = newVolume;
			if (playing.voiceOpen && m_service != nullptr) {
				m_service->setVoiceVolume(playing.voice, effectiveVolume(playing));
			}
		}
	}
}

void XAudio2AudioManager::removePlayingAudio(AsciiString eventName)
{
	for (PlayingAudio &playing : m_playing) {
		if (playing.event->getEventName() == eventName) {
			playing.stopping = TRUE;
		}
	}
}

void XAudio2AudioManager::removeAllDisabledAudio()
{
	for (PlayingAudio &playing : m_playing) {
		if (playing.event->getVolume() <= 0.0f) {
			playing.stopping = TRUE;
		}
	}
}

Bool XAudio2AudioManager::has3DSensitiveStreamsPlaying() const
{
	for (const PlayingAudio &playing : m_playing) {
		if (playing.channel == Channel::STREAM && playing.event->isPositionalAudio()) {
			return TRUE;
		}
	}
	return FALSE;
}

Real XAudio2AudioManager::getFileLengthMS(AsciiString fileName) const
{
	Real durationMS = 0.0f;
	if (m_assetSource != nullptr && m_assetSource->getDurationMS(fileName, durationMS)) {
		return durationMS;
	}
	return 0.0f;
}

void XAudio2AudioManager::closeAnySamplesUsingFile(const void *fileToClose)
{
	if (fileToClose == nullptr) {
		return;
	}
	for (PlayingAudio &playing : m_playing) {
		playing.stopping = TRUE;
	}
}

XAudio2AudioManager::PlayingAudio *XAudio2AudioManager::findPlaying(AudioHandle handle)
{
	for (PlayingAudio &playing : m_playing) {
		if (playing.event != nullptr && playing.event->getPlayingHandle() == handle) {
			return &playing;
		}
	}
	return nullptr;
}

const XAudio2AudioManager::PlayingAudio *XAudio2AudioManager::findPlaying(AudioHandle handle) const
{
	for (const PlayingAudio &playing : m_playing) {
		if (playing.event != nullptr && playing.event->getPlayingHandle() == handle) {
			return &playing;
		}
	}
	return nullptr;
}

XAudio2AudioManager::PlayingAudio *XAudio2AudioManager::findLowestPriority(Channel channel,
	AudioPriority minimumPriority)
{
	PlayingAudio *victim = nullptr;
	for (PlayingAudio &playing : m_playing) {
		if (playing.channel != channel || playing.event == nullptr
			|| playing.event->getAudioPriority() >= minimumPriority) {
			continue;
		}
		if (victim == nullptr || playing.event->getAudioPriority() < victim->event->getAudioPriority()) {
			victim = &playing;
		}
	}
	return victim;
}

Real XAudio2AudioManager::effectiveVolume(const PlayingAudio &playing) const
{
	Real volume = playing.volume * playing.fadeVolume;
	if (playing.event != nullptr) {
		volume *= playing.event->getVolume();
	}
	if (playing.channel == Channel::SAMPLE_3D) {
		volume *= getZoomVolume();
		if (playing.event != nullptr && playing.event->getAudioEventInfo() != nullptr
			&& playing.event->isPositionalAudio()) {
			const Coord3D *position = playing.event->getCurrentPosition();
			const AudioEventInfo *info = playing.event->getAudioEventInfo();
			if (position != nullptr && info != nullptr && info->m_maxDistance > info->m_minDistance) {
				Coord3D distance = *position;
				distance.sub(m_listenerPosition);
				const Real length = distance.length();
				if (length >= info->m_maxDistance) {
					volume = 0.0f;
				} else if (length > info->m_minDistance) {
					volume *= (info->m_maxDistance - length)
						/ (info->m_maxDistance - info->m_minDistance);
				}
			}
		}
	}
	if (volume < 0.0f) return 0.0f;
	if (volume > 1.0f) return 1.0f;
	return volume;
}

void XAudio2AudioManager::recordMusicCompletion(const PlayingAudio &playing)
{
	if (playing.event == nullptr || !isMusic(*playing.event)) {
		return;
	}
	for (std::pair<AsciiString, Int> &completion : m_musicCompletions) {
		if (completion.first == playing.event->getEventName()) {
			++completion.second;
			return;
		}
	}
	m_musicCompletions.push_back(std::make_pair(playing.event->getEventName(), 1));
}
