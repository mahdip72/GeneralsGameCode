#include "XAudio2AudioDevice/XAudio2AudioManager.h"

#include "Common/AudioEventInfo.h"
#include "Common/AudioAffect.h"
#include "Common/AudioHandleSpecialValues.h"
#include "Common/AudioRequest.h"
#include "Common/AudioSettings.h"
#include "Common/GameCommon.h"
#include "AudioDevice/AudioChannelPolicy.h"

#include <algorithm>
#include <cmath>
#include <limits>

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

Bool durationToFrames(Real durationMS, UnsignedInt sampleRate, UnsignedInt &frames)
{
	frames = 0;
	if (durationMS <= 0.0f || sampleRate == 0) {
		return FALSE;
	}
	const double frameValue = static_cast<double>(durationMS)
		* static_cast<double>(sampleRate) / 1000.0;
	if (!std::isfinite(frameValue) || frameValue < 1.0
		|| frameValue > static_cast<double>(std::numeric_limits<UnsignedInt>::max())) {
		return FALSE;
	}
	frames = static_cast<UnsignedInt>(std::floor(frameValue + 0.5));
	return frames != 0;
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
	if (m_assetSource == nullptr) {
		m_ownedAssetSource = std::make_unique<FileAudioAssetSource>();
		m_assetSource = m_ownedAssetSource.get();
	}
}

XAudio2AudioManager::~XAudio2AudioManager()
{
	closeDevice();
}

#if defined(RTS_NATIVE_AUDIO_TEST_HOOK)
Bool XAudio2AudioManager::runInjectedPlaybackProbe(AsciiString fileName)
{
	if (!m_admissionOpen || m_service == nullptr || m_assetSource == nullptr) {
		return FALSE;
	}
	PlayingAudio playing;
	playing.generation = m_lifecycleGeneration;
	playing.phase = PP_Sound;
	playing.assetFileName = fileName;
	playing.assetIdentity = m_assetSource->getFileIdentity(fileName);
	if (!m_assetSource->getDurationMS(fileName, playing.phaseDurationMS)
		|| playing.phaseDurationMS <= 0.0f) {
		return FALSE;
	}
	AudioPcmChunk probe;
	if (!m_assetSource->decodePcmAt(fileName, probe, 1, 0) || probe.sampleRate == 0) {
		return FALSE;
	}
	if (!durationToFrames(playing.phaseDurationMS, probe.sampleRate, playing.phaseTotalFrames)) {
		releaseVoice(playing);
		return FALSE;
	}
	if (!ensureVoice(playing) || !submitPhase(playing)) {
		releaseVoice(playing);
		return FALSE;
	}
	m_service->serviceVoice(playing.voice);
	const Bool accepted = playing.voiceOpen;
	releaseVoice(playing);
	return accepted;
}

void XAudio2AudioManager::setChannelLimitsForTest(UnsignedInt samples2D,
	UnsignedInt samples3D, UnsignedInt streams)
{
	m_num2DSamples = samples2D;
	m_num3DSamples = samples3D;
	m_numStreams = streams;
}

void XAudio2AudioManager::setAudioSettingsForTest(AudioSettings *settings)
{
	m_audioSettings = settings;
}

void XAudio2AudioManager::setActiveMusicTrackForTest(const AsciiString &track)
{
	m_activeMusicTrack = track;
}
#endif

void XAudio2AudioManager::setAssetSource(AudioAssetSource *assetSource)
{
	if (assetSource != nullptr) {
		m_ownedAssetSource.reset();
		m_assetSource = assetSource;
		return;
	}
	m_ownedAssetSource = std::make_unique<FileAudioAssetSource>();
	m_assetSource = m_ownedAssetSource.get();
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
	if (m_ownedAssetSource != nullptr && m_audioSettings != nullptr
		&& !m_audioSettings->m_audioRoot.isEmpty()) {
		m_ownedAssetSource = std::make_unique<FileAudioAssetSource>(m_audioSettings->m_audioRoot);
		m_assetSource = m_ownedAssetSource.get();
	}
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
	const Bool reopen = m_open;
	closeDevice();
	AudioManager::reset();
	removeAllAudioRequests();
	removeLevelSpecificAudioEventInfos();
	++m_lifecycleGeneration;
	if (m_lifecycleGeneration == 0) {
		m_lifecycleGeneration = 1;
	}
	if (reopen) {
		openDevice();
	}
}

void XAudio2AudioManager::update()
{
	// The owner update order is deliberate: callbacks are serviced, records are
	// drained, then the common listener state is updated once, followed by
	// request admission, active records, fades, and listener-driven controls.
	if (m_service != nullptr) {
		m_service->serviceVoices();
	}
	drainCompletions();
	if (TheTacticalView != nullptr) {
		AudioManager::update();
	} else {
		// The native manager keeps headless scripts device-free, but a live game
		// still owns the listener/zoom update performed by the common manager.
		m_zoomVolume = 1.0f;
		set3DVolumeAdjustment(1.0f);
	}
	processRequestList();
	processActiveAudio();
	processFades();
	updatePlayingVolumes();
}

void XAudio2AudioManager::removeAudioEvent(AudioHandle audioEvent)
{
	if (audioEvent == AHSV_StopTheMusic || audioEvent == AHSV_StopTheMusicFade
		|| audioEvent >= AHSV_FirstHandle) {
		AudioRequest *request = allocateAudioRequest();
		request->m_handleToInteractOn = audioEvent;
		request->m_request = AR_Stop;
		appendAudioRequest(request);
	}
}

DynamicAudioEventRTS *XAudio2AudioManager::copyEvent(const AudioEventRTS *eventToCopy)
{
	RefCountPtr<DynamicAudioEventRTS> prepared;
	if (!prepareAudioEventForPlayback(eventToCopy, prepared, FALSE)) {
		return nullptr;
	}
	return prepared.Release();
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
	if (eventToAdd->getAudioEventInfo() == nullptr) {
		getInfoForAudioEvent(eventToAdd);
		if (eventToAdd->getAudioEventInfo() == nullptr) {
			return AHSV_Error;
		}
	}
	const AudioEventInfo *eventInfo = eventToAdd->getAudioEventInfo();
	switch (eventInfo->m_soundType) {
		case AT_Music:
			if (!isOn(AudioAffect_Music)) return AHSV_NoSound;
			break;
		case AT_SoundEffect:
			if (!isOn(AudioAffect_Sound) || !isOn(AudioAffect_Sound3D)) return AHSV_NoSound;
			break;
		case AT_Streaming:
			if (getDisallowSpeech() || !isOn(AudioAffect_Speech)) return AHSV_NoSound;
			break;
	}
	if (!eventToAdd->getIsLogicalAudio() && !eventToAdd->getUninterruptible()
		&& !shouldPlayLocally(eventToAdd)) {
		return AHSV_NotForLocal;
	}
	Real adjustedVolume = eventToAdd->getVolume();
	for (const std::pair<AsciiString, Real> &adjustment : m_adjustedVolumes) {
		if (adjustment.first == eventToAdd->getEventName()) {
			adjustedVolume = adjustment.second;
			break;
		}
	}
	if (m_audioSettings != nullptr && adjustedVolume < m_audioSettings->m_minVolume) {
		return AHSV_Muted;
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
	RefCountPtr<DynamicAudioEventRTS> prepared;
	if (m_admissionOpen && prepareAudioEventForPlayback(eventToPlay, prepared, TRUE)) {
		enqueuePlay(prepared.Release(), TRUE);
	}
}

void XAudio2AudioManager::processRequestList()
{
	for (std::list<AudioRequest *>::iterator it = m_audioRequests.begin();
		it != m_audioRequests.end();) {
		AudioRequest *request = *it;
		if (request->m_paused) {
			++it;
			continue;
		}
		if (request->m_pendingEvent != nullptr && request->m_pendingEvent->getDelay() > 0.0f) {
			const Real remainingDelay = request->m_pendingEvent->getDelay();
			request->m_pendingEvent->decrementDelay(
				remainingDelay < LOGIC_FRAME_MS ? remainingDelay : LOGIC_FRAME_MS);
			request->m_requiresCheckForSample = TRUE;
			// A positive delay is never consumed and played in the same owner tick;
			// this keeps a sub-frame residual from being rounded up early.
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
		if (victim == nullptr || !canReplace(*victim, *event)) {
			return;
		}
		for (std::size_t victimIndex = 0; victimIndex < m_playing.size(); ++victimIndex) {
			if (&m_playing[victimIndex] == victim) {
				finishPlaying(m_playing[victimIndex]);
				m_playing.erase(m_playing.begin() + victimIndex);
				break;
			}
		}
	}
	if (!playing.forced && event->getAudioEventInfo()->m_limit > 0) {
		UnsignedInt sameEvent = 0;
		PlayingAudio *sameEventVictim = nullptr;
		for (PlayingAudio &candidate : m_playing) {
			if (!candidate.stopping && candidate.event != nullptr
				&& candidate.event->getEventName() == event->getEventName()) {
				++sameEvent;
				if (sameEventVictim == nullptr
					|| candidate.event->getAudioPriority() < sameEventVictim->event->getAudioPriority()) {
					sameEventVictim = &candidate;
				}
			}
		}
		if (sameEvent >= static_cast<UnsignedInt>(event->getAudioEventInfo()->m_limit)) {
			if (sameEventVictim == nullptr || !canReplace(*sameEventVictim, *event)) {
				return;
			}
			for (std::size_t victimIndex = 0; victimIndex < m_playing.size(); ++victimIndex) {
				if (&m_playing[victimIndex] == sameEventVictim) {
					finishPlaying(m_playing[victimIndex]);
					m_playing.erase(m_playing.begin() + victimIndex);
					break;
				}
			}
		}
	}

	m_playing.push_back(playing);
	startNextPhase(m_playing.back());
	updateDisallowSpeechGuard();
}

void XAudio2AudioManager::processStopRequest(AudioHandle handle)
{
	if (handle == AHSV_StopTheMusic || handle == AHSV_StopTheMusicFade) {
		for (PlayingAudio &playing : m_playing) {
			if (isMusic(*playing.event)) {
				if (handle == AHSV_StopTheMusicFade) {
					playing.fadeFrames = m_audioSettings != nullptr
						? std::max(1, m_audioSettings->m_fadeAudioFrames) : 30;
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
			playing.assetFileName = fileName;
			playing.assetIdentity = m_assetSource == nullptr
				? nullptr : m_assetSource->getFileIdentity(fileName);
			playing.phaseSubmittedFrames = 0;
			playing.phaseCompletedFrames = 0;
			playing.phaseTotalFrames = 0;
			if (m_assetSource != nullptr) {
				AudioPcmChunk probe;
				if (m_assetSource->decodePcmAt(fileName, probe, 1, 0)
					&& probe.sampleRate != 0) {
					if (!durationToFrames(playing.phaseDurationMS, probe.sampleRate,
						playing.phaseTotalFrames)) {
						playing.phaseDurationMS = 0.0f;
						playing.phaseRemainingMS = 0.0f;
					}
				}
			}
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
	if (playing.voiceOpen && !m_service->resetVoice(playing.voice, playing.generation)) {
		m_service->destroyVoice(playing.voice);
		playing.voice = {};
		playing.voiceOpen = FALSE;
	}
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
		fileName = playing.event != nullptr ? playing.event->getFilename() : playing.assetFileName;
	}
	if (playing.phaseTotalFrames == 0) {
		AudioPcmChunk probe;
		if (!m_assetSource->decodePcmAt(fileName, probe, 1, 0) || probe.sampleRate == 0) {
			return FALSE;
		}
		if (!durationToFrames(playing.phaseDurationMS, probe.sampleRate,
			playing.phaseTotalFrames)) {
			return FALSE;
		}
	}
	if (playing.phaseSubmittedFrames >= playing.phaseTotalFrames) {
		return TRUE;
	}
	const UnsignedInt framesToSubmit = std::min(
		PCM_MAX_FRAMES, playing.phaseTotalFrames - playing.phaseSubmittedFrames);
	AudioPcmChunk chunk;
	if (!m_assetSource->decodePcmAt(fileName, chunk, framesToSubmit,
		playing.phaseSubmittedFrames)) {
		return FALSE;
	}
	if (chunk.frameCount == 0 || chunk.frameCount > framesToSubmit) {
		return FALSE;
	}
	chunk.generation = playing.generation;
	chunk.sequence = playing.voiceSequence++;
	chunk.startSample = static_cast<std::int64_t>(playing.phaseSubmittedFrames);
	const UnsignedInt submittedFrames = chunk.frameCount;
	if (m_service->submit(playing.voice, std::move(chunk)) != AudioPcmSubmitResult::ACCEPTED) {
		return FALSE;
	}
	playing.phaseSubmittedFrames += submittedFrames;
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
				if (completion.endSample >= 0) {
					playing.phaseCompletedFrames = std::min(
						playing.phaseTotalFrames,
						static_cast<UnsignedInt>(completion.endSample));
				}
				if (playing.paused || playing.stopping) {
					break;
				}
				if (playing.phaseSubmittedFrames < playing.phaseTotalFrames) {
					submitPhase(playing);
				} else {
					playing.phaseRemainingMS = 0.0f;
				}
				break;
			}
		}
	}
}

void XAudio2AudioManager::processActiveAudio()
{
	for (std::size_t index = 0; index < m_playing.size();) {
		PlayingAudio &playing = m_playing[index];
		if (playing.stopping) {
			finishPlaying(playing);
			m_playing.erase(m_playing.begin() + index);
			continue;
		}
		if (playing.paused && !playing.stopping) {
			++index;
			continue;
		}
		playing.phaseRemainingMS -= LOGIC_FRAME_MS;
		if (playing.phaseRemainingMS <= 0.0f
			&& playing.phaseTotalFrames > 0
			&& playing.phaseCompletedFrames < playing.phaseTotalFrames) {
			playing.phaseRemainingMS = LOGIC_FRAME_MS;
			++index;
			continue;
		}
		if (!playing.stopping && playing.phaseRemainingMS <= 0.0f
			&& playing.event->getDelay() > 0.0f) {
			playing.event->decrementDelay(LOGIC_FRAME_MS);
			++index;
			continue;
		}
		if (playing.phaseRemainingMS <= 0.0f) {
			playing.event->advanceNextPlayPortion();
			if (playing.event->getNextPlayPortion() == PP_Done
				&& playing.event->getAudioEventInfo() != nullptr
				&& BitIsSet(playing.event->getAudioEventInfo()->m_control, AC_LOOP)
				&& playing.event->hasMoreLoops()) {
				playing.event->decreaseLoopCount();
				playing.event->generateFilename();
				playing.event->setNextPlayPortion(PP_Sound);
			}
			if (playing.event->getNextPlayPortion() == PP_Done) {
				finishPlaying(playing, TRUE);
				m_playing.erase(m_playing.begin() + index);
				continue;
			}
			startNextPhase(playing);
		}
		++index;
	}
	updateDisallowSpeechGuard();
}

void XAudio2AudioManager::processFades()
{
	for (PlayingAudio &playing : m_playing) {
		if (playing.fadeFrames <= 0) {
			continue;
		}
		const Int totalFadeFrames = m_audioSettings != nullptr
			? std::max(1, m_audioSettings->m_fadeAudioFrames) : 30;
		playing.fadeFrames = std::max(0, playing.fadeFrames - 1);
		playing.fadeVolume = static_cast<Real>(playing.fadeFrames)
			/ static_cast<Real>(totalFadeFrames);
		if (playing.voiceOpen && m_service != nullptr) {
			m_service->setVoiceVolume(playing.voice, effectiveVolume(playing));
		}
		if (playing.fadeFrames == 0) {
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

void XAudio2AudioManager::finishPlaying(PlayingAudio &playing, Bool naturalCompletion)
{
	if (naturalCompletion) {
		recordMusicCompletion(playing);
	}
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
	setDisallowSpeech(FALSE);
}

void XAudio2AudioManager::stopAudio(AudioAffect which)
{
	for (PlayingAudio &playing : m_playing) {
		if (affectMatches(playing, which)) {
			playing.stopping = TRUE;
		}
	}
	for (std::list<AudioRequest *>::iterator it = m_audioRequests.begin(); it != m_audioRequests.end();) {
		AudioRequest *request = *it;
		if (request->m_request == AR_Play && requestAffectMatches(request, which)) {
			releaseAudioRequest(request);
			it = m_audioRequests.erase(it);
		} else {
			++it;
		}
	}
}

void XAudio2AudioManager::pauseAudio(AudioAffect which)
{
	for (PlayingAudio &playing : m_playing) {
		if (affectMatches(playing, which)) {
			playing.paused = TRUE;
			if (m_service != nullptr && playing.voiceOpen) {
				m_service->pauseVoice(playing.voice);
			}
		}
	}
	for (AudioRequest *request : m_audioRequests) {
		if (request->m_request == AR_Play && requestAffectMatches(request, which)) {
			request->m_paused = TRUE;
		}
	}
}

void XAudio2AudioManager::resumeAudio(AudioAffect which)
{
	for (PlayingAudio &playing : m_playing) {
		if (affectMatches(playing, which)) {
			playing.paused = FALSE;
			if (m_service != nullptr && playing.voiceOpen) {
				m_service->resumeVoice(playing.voice);
			}
			if (playing.phaseSubmittedFrames < playing.phaseTotalFrames) {
				submitPhase(playing);
			} else if (playing.phaseCompletedFrames >= playing.phaseTotalFrames) {
				playing.phaseRemainingMS = 0.0f;
			}
		}
	}
	for (AudioRequest *request : m_audioRequests) {
		if (request->m_request == AR_Play && requestAffectMatches(request, which)) {
			request->m_paused = FALSE;
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
	for (PlayingAudio &playing : m_playing) {
		if (isMusic(*playing.event)) {
			playing.stopping = TRUE;
		}
	}
	m_activeMusicTrack = nextTrackName(m_activeMusicTrack);
	if (m_admissionOpen && !m_activeMusicTrack.isEmpty()) {
		AudioEventRTS track(m_activeMusicTrack);
		getInfoForAudioEvent(&track);
		if (track.getAudioEventInfo() != nullptr) {
			addAudioEvent(&track);
		}
	}
	return m_activeMusicTrack;
}

AsciiString XAudio2AudioManager::prevMusicTrack()
{
	for (PlayingAudio &playing : m_playing) {
		if (isMusic(*playing.event)) {
			playing.stopping = TRUE;
		}
	}
	m_activeMusicTrack = prevTrackName(m_activeMusicTrack);
	if (m_admissionOpen && !m_activeMusicTrack.isEmpty()) {
		AudioEventRTS track(m_activeMusicTrack);
		getInfoForAudioEvent(&track);
		if (track.getAudioEventInfo() != nullptr) {
			addAudioEvent(&track);
		}
	}
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

Bool XAudio2AudioManager::isCurrentlyPlaying(AudioHandle handle)
{
	if (handle == AHSV_NoSound || handle == AHSV_Error || !m_admissionOpen) {
		return FALSE;
	}
	const PlayingAudio *playing = findPlaying(handle);
	if (playing != nullptr) {
		return playing->generation == m_lifecycleGeneration && !playing->stopping;
	}
	for (const AudioRequest *request : m_audioRequests) {
		if (request->m_pendingEvent != nullptr
			&& request->m_pendingEvent->getPlayingHandle() == handle) {
			return TRUE;
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
		if (playing.channel == channel && !playing.stopping) {
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
	if (event == nullptr || event->getAudioEventInfo() == nullptr) {
		return FALSE;
	}
	const AudioEventInfo *info = event->getAudioEventInfo();
	UnsignedInt sameEvent = 0;
	for (const PlayingAudio &playing : m_playing) {
		if (!playing.stopping && playing.event != nullptr
			&& playing.event->getEventName() == event->getEventName()) {
			++sameEvent;
		}
	}
	return (info->m_limit > 0 && sameEvent >= static_cast<UnsignedInt>(info->m_limit))
		|| isChannelFull(channelFor(*event));
}

Bool XAudio2AudioManager::isPlayingLowerPriority(AudioEventRTS *event) const
{
	if (event == nullptr || event->getAudioEventInfo() == nullptr) {
		return FALSE;
	}
	const Channel channel = channelFor(*event);
	for (const PlayingAudio &playing : m_playing) {
		if (!playing.stopping && playing.channel == channel && playing.event->getAudioPriority() < event->getAudioPriority()) {
			return TRUE;
		}
	}
	return FALSE;
}

Bool XAudio2AudioManager::isPlayingAlready(AudioEventRTS *event) const
{
	const PlayingAudio *playing = event == nullptr ? nullptr : findPlaying(event->getPlayingHandle());
	return playing != nullptr && playing->generation == m_lifecycleGeneration && !playing->stopping;
}

Bool XAudio2AudioManager::isObjectPlayingVoice(UnsignedInt objID) const
{
	for (const PlayingAudio &playing : m_playing) {
		if (!playing.stopping && playing.event->getAudioEventInfo() != nullptr
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
		if (!playing.stopping && playing.channel == Channel::STREAM && playing.event->isPositionalAudio()) {
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
		if (playing.assetIdentity == fileToClose) {
			playing.stopping = TRUE;
		}
	}
}

Bool XAudio2AudioManager::affectMatches(const PlayingAudio &playing, AudioAffect which) const
{
	if (playing.event == nullptr || playing.event->getAudioEventInfo() == nullptr) {
		return FALSE;
	}
	const AudioEventInfo *info = playing.event->getAudioEventInfo();
	if (info->m_soundType == AT_Music) {
		return BitIsSet(which, AudioAffect_Music);
	}
	if (info->m_soundType == AT_Streaming) {
		return BitIsSet(which, AudioAffect_Speech);
	}
	return playing.event->isPositionalAudio()
		? BitIsSet(which, AudioAffect_Sound3D)
		: BitIsSet(which, AudioAffect_Sound);
}

Bool XAudio2AudioManager::requestAffectMatches(const AudioRequest *request, AudioAffect which) const
{
	if (request == nullptr || request->m_pendingEvent == nullptr) {
		return FALSE;
	}
	const DynamicAudioEventRTS *event = request->m_pendingEvent.Peek();
	if (event == nullptr || event->getAudioEventInfo() == nullptr) {
		return FALSE;
	}
	const AudioEventInfo *info = event->getAudioEventInfo();
	if (info->m_soundType == AT_Music) return BitIsSet(which, AudioAffect_Music);
	if (info->m_soundType == AT_Streaming) return BitIsSet(which, AudioAffect_Speech);
	return event->isPositionalAudio()
		? BitIsSet(which, AudioAffect_Sound3D)
		: BitIsSet(which, AudioAffect_Sound);
}

Bool XAudio2AudioManager::canReplace(const PlayingAudio &victim,
	const DynamicAudioEventRTS &incoming) const
{
	if (victim.event == nullptr || victim.event->getAudioEventInfo() == nullptr
		|| victim.event->getUninterruptible()) {
		return FALSE;
	}
	const AudioEventInfo *victimInfo = victim.event->getAudioEventInfo();
	const AudioEventInfo *incomingInfo = incoming.getAudioEventInfo();
	const Bool incomingInterrupt = incomingInfo != nullptr
		&& BitIsSet(incomingInfo->m_control, AC_INTERRUPT);
	if (victim.channel == Channel::SAMPLE_3D) {
		return rts::CanReplace3DChannel(
			incomingInterrupt,
			static_cast<int>(incoming.getAudioPriority()),
			static_cast<int>(victim.event->getAudioPriority()),
			victim.event->getAudioPriority() == AP_CRITICAL,
			BitIsSet(victimInfo->m_type, ST_VOICE),
			BitIsSet(victimInfo->m_type, ST_UI),
			BitIsSet(victimInfo->m_type, ST_GLOBAL),
			BitIsSet(victimInfo->m_control, AC_LOOP));
	}
	return victim.event->getAudioPriority() < incoming.getAudioPriority()
		|| (incomingInterrupt && victim.event->getAudioPriority() == incoming.getAudioPriority());
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
		if (playing.stopping || playing.channel != channel || playing.event == nullptr
			|| playing.event->getAudioPriority() > minimumPriority) {
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
		volume *= playing.event->getVolumeShift();
	}
	if (playing.event != nullptr && playing.event->getAudioEventInfo() != nullptr) {
		const AudioEventInfo *info = playing.event->getAudioEventInfo();
		if (info->m_soundType == AT_Music) volume *= m_musicVolume;
		else if (info->m_soundType == AT_Streaming) volume *= m_speechVolume;
		else if (playing.event->isPositionalAudio()) volume *= m_sound3DVolume;
		else volume *= m_soundVolume;
	}
	if (playing.channel == Channel::SAMPLE_3D) {
		volume *= getZoomVolume();
		if (playing.event != nullptr && playing.event->getAudioEventInfo() != nullptr
			&& playing.event->isPositionalAudio()) {
			const Coord3D *position = playing.event->getCurrentPosition();
			const AudioEventInfo *info = playing.event->getAudioEventInfo();
			Real minDistance = info->m_minDistance;
			Real maxDistance = info->m_maxDistance;
			if (m_audioSettings != nullptr && BitIsSet(info->m_type, ST_GLOBAL)) {
				minDistance = static_cast<Real>(m_audioSettings->m_globalMinRange);
				maxDistance = static_cast<Real>(m_audioSettings->m_globalMaxRange);
			}
			if (position != nullptr && info != nullptr && maxDistance > minDistance) {
				Coord3D distance = *position;
				distance.sub(m_listenerPosition);
				const Real length = distance.length();
				if (length >= maxDistance) {
					volume = 0.0f;
				} else if (length > minDistance) {
					Real attenuation = (maxDistance - length)
						/ (maxDistance - minDistance);
					if (m_audioSettings != nullptr && m_audioSettings->m_use3DSoundRangeVolumeFade) {
						attenuation = static_cast<Real>(std::pow(
							std::max(0.0f, attenuation),
							m_audioSettings->m_3DSoundRangeVolumeFadeExponent));
					}
					volume *= attenuation;
				}
			}
		}
	}
	if (volume < 0.0f) return 0.0f;
	if (volume > 1.0f) return 1.0f;
	return volume;
}

void XAudio2AudioManager::updatePlayingVolumes()
{
	for (PlayingAudio &playing : m_playing) {
		if (playing.voiceOpen && m_service != nullptr) {
			m_service->setVoiceVolume(playing.voice, effectiveVolume(playing));
		}
	}
}

void XAudio2AudioManager::updateDisallowSpeechGuard()
{
	Bool guarded = FALSE;
	for (const PlayingAudio &playing : m_playing) {
		if (playing.stopping || playing.event == nullptr
			|| playing.event->getAudioEventInfo() == nullptr) {
			continue;
		}
		if (playing.event->getAudioEventInfo()->m_soundType == AT_Streaming
			&& playing.event->getUninterruptible()) {
			guarded = TRUE;
			break;
		}
	}
	setDisallowSpeech(guarded);
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
