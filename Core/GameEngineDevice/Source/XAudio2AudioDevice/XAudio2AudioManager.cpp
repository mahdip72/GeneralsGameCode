#include "XAudio2AudioDevice/XAudio2AudioManager.h"
#include "XAudio2AudioDevice/NativeAudioCompatibility.h"

#include "Common/AudioEventInfo.h"
#include "Common/AudioAffect.h"
#include "Common/AudioHandleSpecialValues.h"
#include "Common/AudioRequest.h"
#include "Common/AudioSettings.h"
#if defined(RTS_NATIVE_AUDIO_ENGINE_FILESYSTEM)
#include "Common/FileSystem.h"
#endif
#include "Common/GameCommon.h"
#include "AudioDevice/AudioChannelPolicy.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

class View;
extern View *TheTacticalView;

struct XAudio2CompatibilityPcmBudget
{
	std::size_t bytesInUse = 0;
};

namespace
{
constexpr Real LOGIC_FRAME_MS = MSEC_PER_LOGICFRAME_REAL;
constexpr UnsignedInt DEFAULT_2D_CHANNELS = 32;
constexpr UnsignedInt DEFAULT_3D_CHANNELS = 32;
constexpr UnsignedInt DEFAULT_STREAM_CHANNELS = 8;
constexpr UnsignedInt PCM_SAMPLE_RATE = 48000;
constexpr UnsignedShort PCM_CHANNELS = 2;
constexpr UnsignedInt PCM_MAX_FRAMES = 48000;
constexpr UnsignedInt PCM_PREQUEUE_BUFFERS = 2;
constexpr std::size_t MAX_VIRTUAL_AUDIO_BYTES = 64U * 1024U * 1024U;

class BufferedAudioPcmStream final : public AudioPcmStream
{
public:
	BufferedAudioPcmStream(AudioPcmChunk &&pcm,
		std::shared_ptr<XAudio2CompatibilityPcmBudget> budget) :
		m_pcm(std::move(pcm)), m_budget(std::move(budget)), m_reservedBytes(m_pcm.data.size())
	{
		m_budget->bytesInUse += m_reservedBytes;
	}
	~BufferedAudioPcmStream() override
	{
		if (m_budget != nullptr) {
			m_budget->bytesInUse = m_reservedBytes <= m_budget->bytesInUse
				? m_budget->bytesInUse - m_reservedBytes : 0;
		}
	}

	UnsignedInt sampleRate() const override { return m_pcm.sampleRate; }
	Real durationMS() const override
	{
		return static_cast<Real>(m_pcm.frameCount) * 1000.0f
			/ static_cast<Real>(m_pcm.sampleRate);
	}
	Bool readPcm(AudioPcmChunk &chunk, UnsignedInt maxFrames) override
	{
		chunk = {};
		if (maxFrames == 0 || m_nextFrame >= m_pcm.frameCount || m_pcm.channels == 0) {
			return FALSE;
		}
		const UnsignedInt frameCount = std::min(maxFrames, m_pcm.frameCount - m_nextFrame);
		const std::size_t bytesPerFrame = static_cast<std::size_t>(m_pcm.channels) * sizeof(Short);
		const std::size_t begin = static_cast<std::size_t>(m_nextFrame) * bytesPerFrame;
		const std::size_t end = begin + static_cast<std::size_t>(frameCount) * bytesPerFrame;
		if (end > m_pcm.data.size()) {
			return FALSE;
		}
		chunk.sampleRate = m_pcm.sampleRate;
		chunk.channels = m_pcm.channels;
		chunk.format = m_pcm.format;
		chunk.frameCount = frameCount;
		chunk.startSample = static_cast<std::int64_t>(m_nextFrame);
		chunk.data.assign(m_pcm.data.begin() + begin, m_pcm.data.begin() + end);
		m_nextFrame += frameCount;
		return TRUE;
	}

private:
	AudioPcmChunk m_pcm;
	std::shared_ptr<XAudio2CompatibilityPcmBudget> m_budget;
	std::size_t m_reservedBytes;
	UnsignedInt m_nextFrame = 0;
};

class EngineVirtualAudioSource final : public AudioVirtualFileSource
{
public:
	Bool readFile(const AsciiString &fileName, std::vector<std::uint8_t> &bytes,
		std::string &identity) const override
	{
		bytes.clear();
		identity.clear();
#if !defined(RTS_NATIVE_AUDIO_ENGINE_FILESYSTEM)
		(void)fileName;
		return FALSE;
#else
		if (TheFileSystem == nullptr || fileName.isEmpty()) {
			return FALSE;
		}
		File *file = TheFileSystem->openFile(fileName.str(), File::READ | File::BINARY);
		if (file == nullptr) {
			return FALSE;
		}
		auto closeFile = [&file]() {
			File *closing = file;
			file = nullptr;
			if (closing != nullptr) {
				closing->close();
			}
		};
		const Int fileSize = file->size();
		if (fileSize <= 0 || static_cast<std::size_t>(fileSize) > MAX_VIRTUAL_AUDIO_BYTES) {
			closeFile();
			return FALSE;
		}
		bytes.resize(static_cast<std::size_t>(fileSize));
		std::size_t offset = 0;
		while (offset < bytes.size()) {
			const Int remaining = static_cast<Int>(std::min<std::size_t>(
				bytes.size() - offset, static_cast<std::size_t>((std::numeric_limits<Int>::max)())));
			const Int read = file->read(bytes.data() + offset, remaining);
			if (read <= 0 || read > remaining) {
				bytes.clear();
				closeFile();
				return FALSE;
			}
			offset += static_cast<std::size_t>(read);
		}
		closeFile();
		identity = fileName.str();
		return TRUE;
#endif
	}
};

void configureVirtualAssetSource(FileAudioAssetSource *source,
	const std::shared_ptr<AudioVirtualFileSource> &virtualSource)
{
	if (source != nullptr && source->getVirtualFileSource() == nullptr) {
		source->setOwnedVirtualFileSource(virtualSource);
	}
}

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

XAudio2SpatializationPose makeSpatialPose(const Coord3D &position, const Coord3D &orientation)
{
	XAudio2SpatializationPose pose;
	pose.position[0] = position.x;
	pose.position[1] = position.z;
	pose.position[2] = position.y;
	float front[3] = { orientation.x, orientation.z, orientation.y };
	float frontLength = std::sqrt(front[0] * front[0] + front[1] * front[1]
		+ front[2] * front[2]);
	if (!std::isfinite(frontLength) || frontLength <= 0.0001f) {
		front[0] = 0.0f;
		front[1] = 0.0f;
		front[2] = 1.0f;
		frontLength = 1.0f;
	}
	for (int component = 0; component < 3; ++component) {
		pose.front[component] = front[component] / frontLength;
	}
	float top[3] = { 0.0f, 1.0f, 0.0f };
	if (std::abs(pose.front[1]) > 0.99f) {
		top[0] = 0.0f;
		top[1] = 0.0f;
		top[2] = 1.0f;
	}
	const float projection = top[0] * pose.front[0] + top[1] * pose.front[1]
		+ top[2] * pose.front[2];
	for (int component = 0; component < 3; ++component) {
		top[component] -= projection * pose.front[component];
	}
	const float topLength = std::sqrt(top[0] * top[0] + top[1] * top[1] + top[2] * top[2]);
	for (int component = 0; component < 3; ++component) {
		pose.top[component] = top[component] / topLength;
	}
	return pose;
}
}

XAudio2AudioManager::XAudio2AudioManager() :
	XAudio2AudioManager(nullptr, nullptr)
{
}

XAudio2AudioManager::XAudio2AudioManager(XAudio2AudioService *service,
	AudioAssetSource *assetSource) :
	m_compatibilityPcmBudget(std::make_shared<XAudio2CompatibilityPcmBudget>()),
	m_service(service),
	m_ownsService(FALSE),
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
	m_ownedVirtualFileSource = std::make_shared<EngineVirtualAudioSource>();
	configureVirtualAssetSource(dynamic_cast<FileAudioAssetSource *>(m_assetSource),
		m_ownedVirtualFileSource);
	setDeviceListenerPosition();
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
	if (!preparePhaseSource(playing, fileName)) {
		releaseVoice(playing);
		return FALSE;
	}
	if (!ensureVoice(playing) || !queuePhaseLowWater(playing)) {
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

void XAudio2AudioManager::setOwnedServiceForTest(std::unique_ptr<XAudio2AudioService> service)
{
	closeDevice();
	m_ownedService = std::move(service);
	m_service = m_ownedService.get();
	m_ownsService = m_service != nullptr;
}
#endif

void XAudio2AudioManager::setAssetSource(AudioAssetSource *assetSource)
{
	if (assetSource != nullptr && assetSource == m_assetSource) {
		return;
	}
	const Bool reopen = m_open;
	// A source owns the bytes and stable identities used by active records.  A
	// replacement is therefore a generation barrier: close admission, release
	// every voice/pending request, and only then publish the new source.
	closeDevice();
	++m_lifecycleGeneration;
	if (m_lifecycleGeneration == 0) {
		m_lifecycleGeneration = 1;
	}
	if (assetSource != nullptr) {
		m_ownedAssetSource.reset();
		m_assetSource = assetSource;
		configureVirtualAssetSource(dynamic_cast<FileAudioAssetSource *>(m_assetSource),
			m_ownedVirtualFileSource);
	} else {
		m_ownedAssetSource = std::make_unique<FileAudioAssetSource>();
		m_assetSource = m_ownedAssetSource.get();
		configureVirtualAssetSource(dynamic_cast<FileAudioAssetSource *>(m_assetSource),
			m_ownedVirtualFileSource);
	}
	if (reopen) {
		openDevice();
	}
}

Bool eventAffectEnabledForAudio(const AudioEventRTS &event, const AudioManager &manager)
{
	const AudioEventInfo *info = event.getAudioEventInfo();
	if (info == nullptr) {
		return FALSE;
	}
	if (info->m_soundType == AT_Music) {
		return manager.isOn(AudioAffect_Music);
	}
	if (info->m_soundType == AT_Streaming) {
		return manager.isOn(AudioAffect_Speech);
	}
	return event.isPositionalAudio()
		? manager.isOn(AudioAffect_Sound3D)
		: manager.isOn(AudioAffect_Sound);
}

void XAudio2AudioManager::setService(XAudio2AudioService *service)
{
	if (m_service == service) {
		return;
	}
	closeDevice();
	m_ownedService.reset();
	m_service = service;
	m_ownsService = FALSE;
}

void XAudio2AudioManager::init()
{
	AudioManager::init();
	if (m_ownedAssetSource != nullptr && m_audioSettings != nullptr
		&& !m_audioSettings->m_audioRoot.isEmpty()) {
		m_ownedAssetSource = std::make_unique<FileAudioAssetSource>(m_audioSettings->m_audioRoot);
		m_assetSource = m_ownedAssetSource.get();
		configureVirtualAssetSource(dynamic_cast<FileAudioAssetSource *>(m_assetSource),
			m_ownedVirtualFileSource);
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
		if (m_service->state() == XAudio2AudioServiceState::FAILED) {
			// The owner must fully quiesce a failed native service before beginning
			// a new callback generation. Injected services remain externally owned.
			m_open = FALSE;
			m_admissionOpen = FALSE;
			removeAllAudioRequests();
			clearPlaying();
			++m_lifecycleGeneration;
			if (m_lifecycleGeneration == 0) {
				m_lifecycleGeneration = 1;
			}
			if (m_ownsService) {
				m_service->shutdown();
				m_open = m_service->state() == XAudio2AudioServiceState::CLOSED
					&& m_service->open();
				m_admissionOpen = m_open;
			}
			return;
		}
		if (!m_service->isOpen()) {
			m_open = FALSE;
			m_admissionOpen = FALSE;
			removeAllAudioRequests();
			clearPlaying();
			return;
		}
		for (PlayingAudio &playing : m_playing) {
			if (playing.voiceOpen && m_service->isVoiceFailed(playing.voice)) {
				failPlaying(playing);
			}
		}
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
	setDeviceListenerPosition();
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
			if (eventToAdd->isPositionalAudio()
				? !isOn(AudioAffect_Sound3D) : !isOn(AudioAffect_Sound)) return AHSV_NoSound;
			break;
		case AT_Streaming:
			if ((getDisallowSpeech() && !eventToAdd->getUninterruptible())
				|| !isOn(AudioAffect_Speech)) return AHSV_NoSound;
			break;
	}
	if (!eventToAdd->getIsLogicalAudio() && !eventToAdd->getUninterruptible()
		&& !shouldPlayLocally(eventToAdd)) {
		return AHSV_NotForLocal;
	}
	if (!isAudibleAtAdmission(*eventToAdd)) {
		return AHSV_NotForLocal;
	}
	const UnsignedInt objectID = const_cast<AudioEventRTS *>(eventToAdd)->getObjectID();
	Bool pendingSameObjectVoice = FALSE;
	for (const AudioRequest *request : m_audioRequests) {
		if (request != nullptr && request->m_request == AR_Play
			&& request->m_pendingEvent != nullptr
			&& request->m_pendingEvent->getAudioEventInfo() != nullptr
			&& BitIsSet(request->m_pendingEvent->getAudioEventInfo()->m_type, ST_VOICE)
			&& request->m_pendingEvent->getObjectID() == objectID) {
			pendingSameObjectVoice = TRUE;
			break;
		}
	}
	if (BitIsSet(eventInfo->m_type, ST_VOICE) && objectID != 0
		&& (isObjectPlayingVoice(objectID) || pendingSameObjectVoice)
		&& !BitIsSet(eventInfo->m_control, AC_INTERRUPT)) {
		return AHSV_NoSound;
	}
	const Bool interrupting = BitIsSet(eventInfo->m_control, AC_INTERRUPT);
	const Bool pendingLimitFull = eventInfo->m_limit > 0
		&& pendingEventCount(eventToAdd->getEventName())
			>= static_cast<UnsignedInt>(eventInfo->m_limit);
	if (doesViolateLimit(const_cast<AudioEventRTS *>(eventToAdd))
		&& (pendingLimitFull
			|| (!interrupting && !isPlayingLowerPriority(const_cast<AudioEventRTS *>(eventToAdd))))) {
		return AHSV_NoSound;
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
	if (eventToPlay == nullptr || !m_admissionOpen) {
		return;
	}
	if (eventToPlay->getAudioEventInfo() == nullptr) {
		getInfoForAudioEvent(eventToPlay);
	}
	if (eventToPlay->getAudioEventInfo() != nullptr
		&& eventAffectEnabled(*eventToPlay)
		&& prepareAudioEventForPlayback(eventToPlay, prepared, TRUE)) {
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
	const Bool forcedRequest = std::find(m_forcePlayHandles.begin(), m_forcePlayHandles.end(),
		event->getPlayingHandle()) != m_forcePlayHandles.end();
	if ((!forcedRequest && !isAudibleAtAdmission(*event))
		|| (BitIsSet(event->getAudioEventInfo()->m_type, ST_VOICE)
			&& event->getObjectID() != 0 && isObjectPlayingVoice(event->getObjectID())
			&& !BitIsSet(event->getAudioEventInfo()->m_control, AC_INTERRUPT))) {
		return;
	}
	if (event->getHandleToKill() != AHSV_Error && event->getHandleToKill() != 0) {
		processStopRequest(event->getHandleToKill());
	}
	if (event->getAudioEventInfo()->m_soundType == AT_Streaming
		&& event->getUninterruptible()) {
		stopExistingSpeechForUninterruptible(event->getPlayingHandle());
	}

	PlayingAudio playing;
	playing.event = request->m_pendingEvent;
	playing.channel = channelFor(*event);
	playing.phase = event->getNextPlayPortion();
	playing.generation = m_lifecycleGeneration;
	playing.volume = event->getVolume();
	std::vector<AudioHandle>::iterator forced = std::find(m_forcePlayHandles.begin(),
		m_forcePlayHandles.end(), event->getPlayingHandle());
	playing.forced = forced != m_forcePlayHandles.end();
	if (forced != m_forcePlayHandles.end()) {
		m_forcePlayHandles.erase(forced);
	}
	playing.speechVolumeOverride = playing.forced
		&& event->getAudioEventInfo()->m_soundType == AT_Streaming;

	if (!playing.forced && isChannelFullForEvent(playing.channel, event->getPlayingHandle())) {
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
			if (!candidate.forced && !candidate.stopping && candidate.event != nullptr
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

	m_playing.push_back(std::move(playing));
	if (isMusic(*event)) {
		m_activeMusicTrack = event->getEventName();
	}
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
		failPlaying(playing);
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

		{
			playing.assetFileName = fileName;
			playing.phaseSubmittedFrames = 0;
			playing.phaseQueuedBuffers = 0;
			playing.phaseCompletedFrames = 0;
			playing.phaseTotalFrames = 0;
			playing.phaseFirstSequence = playing.voiceSequence;
			if (!preparePhaseSource(playing, fileName)) {
				// Unreadable assets are terminal failures, never fabricated zero-length
				// phases that silently advance or loop forever.
				failPlaying(playing);
				return;
			}
			playing.phaseRemainingMS = playing.phaseDurationMS;
			if (!ensureVoice(playing) || !queuePhaseLowWater(playing)) {
				failPlaying(playing);
			}
			return;
		}
	}
}

Bool XAudio2AudioManager::preparePhaseSource(PlayingAudio &playing,
	const AsciiString &fileName)
{
	playing.pcmStream.reset();
	playing.phaseDurationMS = 0.0f;
	playing.phaseTotalFrames = 0;
	if (m_assetSource == nullptr) {
		return FALSE;
	}

	std::unique_ptr<AudioPcmStream> stream;
	UnsignedInt sampleRate = 0;
	if (m_assetSource->openPcmStream(fileName, stream)) {
		if (stream == nullptr || stream->durationMS() <= 0.0f
			|| stream->sampleRate() == 0) {
			return FALSE;
		}
		playing.phaseDurationMS = stream->durationMS();
		sampleRate = stream->sampleRate();
		playing.pcmStream = std::move(stream);
	} else {
		AudioPcmChunk probe;
		if (!m_assetSource->getDurationMS(fileName, playing.phaseDurationMS)
			|| playing.phaseDurationMS <= 0.0f
			|| !m_assetSource->decodePcmAt(fileName, probe, 1, 0)
			|| probe.sampleRate != PCM_SAMPLE_RATE || probe.channels != PCM_CHANNELS
			|| probe.format != AudioPcmFormat::SIGNED_16_INTERLEAVED_LITTLE_ENDIAN) {
			return FALSE;
		}
		sampleRate = probe.sampleRate;
		if (!durationToFrames(playing.phaseDurationMS, sampleRate,
			playing.phaseTotalFrames)) {
			return FALSE;
		}
		if (m_assetSource->supportsPcmRangeDecode()) {
			return TRUE;
		}
		const std::size_t bytesPerFrame = static_cast<std::size_t>(probe.channels) * sizeof(Short);
		if (m_compatibilityPcmBudget == nullptr
			|| !NativeAudioCompatibility::canReservePcm(
				m_compatibilityPcmBudget->bytesInUse,
				playing.phaseTotalFrames, probe.channels)) {
			return FALSE;
		}
		AudioPcmChunk pcm;
		if (!m_assetSource->decodePcm(fileName, pcm, playing.phaseTotalFrames)
			|| pcm.sampleRate != sampleRate || pcm.channels != probe.channels
			|| pcm.format != probe.format || pcm.frameCount == 0
			|| pcm.frameCount > playing.phaseTotalFrames
			|| pcm.data.size() != static_cast<std::size_t>(pcm.frameCount) * bytesPerFrame) {
			return FALSE;
		}
		playing.phaseTotalFrames = pcm.frameCount;
		playing.phaseDurationMS = static_cast<Real>(pcm.frameCount) * 1000.0f
			/ static_cast<Real>(sampleRate);
		playing.pcmStream = std::make_unique<BufferedAudioPcmStream>(
			std::move(pcm), m_compatibilityPcmBudget);
		return TRUE;
	}
	return durationToFrames(playing.phaseDurationMS, sampleRate,
		playing.phaseTotalFrames);
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
	const Bool decoded = playing.pcmStream != nullptr
		? playing.pcmStream->readPcm(chunk, framesToSubmit)
		: m_assetSource->decodePcmAt(fileName, chunk, framesToSubmit,
			playing.phaseSubmittedFrames);
	if (!decoded) {
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

Bool XAudio2AudioManager::queuePhaseLowWater(PlayingAudio &playing)
{
	while (playing.phaseQueuedBuffers < PCM_PREQUEUE_BUFFERS
		&& playing.phaseSubmittedFrames < playing.phaseTotalFrames) {
		if (!submitPhase(playing)) {
			return FALSE;
		}
		++playing.phaseQueuedBuffers;
	}
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
				&& completion.sequence >= playing.phaseFirstSequence
				&& completion.sequence < playing.voiceSequence) {
				if (playing.phaseQueuedBuffers > 0) {
					--playing.phaseQueuedBuffers;
				}
				if (completion.endSample >= 0) {
					playing.phaseCompletedFrames = std::min(
						playing.phaseTotalFrames,
						static_cast<UnsignedInt>(completion.endSample));
				}
				if (playing.fadeFrames > 0) {
					playing.stopping = TRUE;
					break;
				}
				if (playing.paused || playing.stopping) {
					break;
				}
				if (playing.phaseSubmittedFrames < playing.phaseTotalFrames) {
					if (!queuePhaseLowWater(playing)) {
						failPlaying(playing);
					} else {
						// The completion was observed by the owner, so submit the
						// replacement immediately instead of waiting for the next update.
						m_service->serviceVoice(playing.voice);
					}
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
		if (playing.waitingForGeneratedDelay) {
			if (playing.event->getDelay() > 0.0f) {
				playing.event->decrementDelay(LOGIC_FRAME_MS);
				++index;
				continue;
			}
			playing.waitingForGeneratedDelay = FALSE;
			playing.phaseRemainingMS = 0.0f;
			startNextPhase(playing);
			if (playing.stopping) {
				finishPlaying(playing, FALSE);
				m_playing.erase(m_playing.begin() + index);
				continue;
			}
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
		if (playing.fadeFrames > 0 && playing.phaseRemainingMS <= 0.0f) {
			finishPlaying(playing, FALSE);
			m_playing.erase(m_playing.begin() + index);
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
				playing.waitingForGeneratedDelay = playing.event->getDelay() > 0.0f;
			}
			if (playing.event->getNextPlayPortion() == PP_Done) {
				finishPlaying(playing, TRUE);
				m_playing.erase(m_playing.begin() + index);
				continue;
			}
			if (!playing.waitingForGeneratedDelay) {
				startNextPhase(playing);
			}
			if (playing.stopping) {
				finishPlaying(playing, FALSE);
				m_playing.erase(m_playing.begin() + index);
				continue;
			}
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
		if (!m_service->stopVoice(playing.voice)) {
			m_open = FALSE;
			m_admissionOpen = FALSE;
		}
		if (!m_service->destroyVoice(playing.voice)) {
			m_open = FALSE;
			m_admissionOpen = FALSE;
		}
	}
	playing.voiceOpen = FALSE;
	playing.voice = {};
}

void XAudio2AudioManager::stopExistingSpeechForUninterruptible(AudioHandle exceptHandle)
{
	for (std::size_t index = 0; index < m_playing.size();) {
		PlayingAudio &playing = m_playing[index];
		if (playing.event != nullptr && playing.event->getAudioEventInfo() != nullptr
			&& playing.event->getAudioEventInfo()->m_soundType == AT_Streaming) {
			finishPlaying(playing, FALSE);
			m_playing.erase(m_playing.begin() + index);
			continue;
		}
		++index;
	}
	for (std::list<AudioRequest *>::iterator it = m_audioRequests.begin();
		it != m_audioRequests.end();) {
		AudioRequest *request = *it;
		const DynamicAudioEventRTS *pending = request == nullptr
			? nullptr : request->m_pendingEvent.Peek();
		if (request != nullptr && pending != nullptr
			&& request->m_request == AR_Play
			&& const_cast<DynamicAudioEventRTS *>(pending)->getPlayingHandle() != exceptHandle
			&& pending->getAudioEventInfo() != nullptr
			&& pending->getAudioEventInfo()->m_soundType == AT_Streaming) {
			releaseAudioRequest(request);
			it = m_audioRequests.erase(it);
			continue;
		}
		++it;
	}
	setDisallowSpeech(FALSE);
}

void XAudio2AudioManager::failPlaying(PlayingAudio &playing)
{
	playing.stopping = TRUE;
	playing.phase = PP_Done;
	playing.phaseRemainingMS = 0.0f;
	playing.phaseDurationMS = 0.0f;
	releaseVoice(playing);
}

void XAudio2AudioManager::finishPlaying(PlayingAudio &playing, Bool naturalCompletion)
{
	if (naturalCompletion && !playing.stopping && playing.fadeFrames <= 0) {
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
				if (!queuePhaseLowWater(playing)) {
					failPlaying(playing);
				}
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
	m_forcePlayHandles.erase(std::remove(m_forcePlayHandles.begin(), m_forcePlayHandles.end(),
		audioEvent), m_forcePlayHandles.end());
	PlayingAudio *playing = findPlaying(audioEvent);
	if (playing != nullptr) {
		for (std::size_t index = 0; index < m_playing.size(); ++index) {
			if (&m_playing[index] == playing) {
				finishPlaying(m_playing[index], FALSE);
				m_playing.erase(m_playing.begin() + index);
				break;
			}
		}
		updateDisallowSpeechGuard();
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
		m_ownsService = TRUE;
	}
	if (m_service->state() == XAudio2AudioServiceState::FAILED) {
		if (!m_ownsService) {
			m_open = FALSE;
			m_admissionOpen = FALSE;
			return;
		}
		m_service->shutdown();
		++m_lifecycleGeneration;
		if (m_lifecycleGeneration == 0) {
			m_lifecycleGeneration = 1;
		}
	}
	m_open = m_service->isOpen() || m_service->open();
	m_admissionOpen = m_open;
}

void XAudio2AudioManager::closeDevice()
{
	m_admissionOpen = FALSE;
	removeAllAudioRequests();
	clearPlaying();
	if (m_service != nullptr && m_ownsService) {
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
		if (!playing.forced && playing.channel == channel && !playing.stopping) {
			++count;
		}
	}
	return count;
}

UnsignedInt XAudio2AudioManager::pendingChannelCount(Channel channel,
	AudioHandle excludeHandle) const
{
	UnsignedInt count = 0;
	for (const AudioRequest *request : m_audioRequests) {
		if (request == nullptr || request->m_request != AR_Play
			|| request->m_pendingEvent == nullptr) {
			continue;
		}
		const DynamicAudioEventRTS *event = request->m_pendingEvent.Peek();
		if (event == nullptr
			|| const_cast<DynamicAudioEventRTS *>(event)->getPlayingHandle() == excludeHandle) {
			continue;
		}
		if (std::find(m_forcePlayHandles.begin(), m_forcePlayHandles.end(),
			const_cast<DynamicAudioEventRTS *>(event)->getPlayingHandle()) != m_forcePlayHandles.end()) {
			continue;
		}
		if (channelFor(*event) == channel) {
			++count;
		}
	}
	return count;
}

UnsignedInt XAudio2AudioManager::pendingEventCount(const AsciiString &eventName,
	AudioHandle excludeHandle) const
{
	UnsignedInt count = 0;
	for (const AudioRequest *request : m_audioRequests) {
		if (request == nullptr || request->m_request != AR_Play
			|| request->m_pendingEvent == nullptr) {
			continue;
		}
		const DynamicAudioEventRTS *event = request->m_pendingEvent.Peek();
		if (event != nullptr
			&& const_cast<DynamicAudioEventRTS *>(event)->getPlayingHandle() != excludeHandle
			&& event->getEventName() == eventName
			&& std::find(m_forcePlayHandles.begin(), m_forcePlayHandles.end(),
				const_cast<DynamicAudioEventRTS *>(event)->getPlayingHandle()) == m_forcePlayHandles.end()) {
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
	return isChannelFullForEvent(channel, AHSV_NoSound);
}

Bool XAudio2AudioManager::isChannelFullForEvent(Channel channel,
	AudioHandle excludeHandle) const
{
	const UnsignedInt used = channelCount(channel) + pendingChannelCount(channel, excludeHandle);
	return used >= channelLimit(channel);
}

UnsignedInt XAudio2AudioManager::getNumAvailable2DSamples() const
{
	const UnsignedInt used = channelCount(Channel::SAMPLE_2D)
		+ pendingChannelCount(Channel::SAMPLE_2D);
	return used >= m_num2DSamples ? 0U : m_num2DSamples - used;
}

UnsignedInt XAudio2AudioManager::getNumAvailable3DSamples() const
{
	const UnsignedInt used = channelCount(Channel::SAMPLE_3D)
		+ pendingChannelCount(Channel::SAMPLE_3D);
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
		if (!playing.forced && !playing.stopping && playing.event != nullptr
			&& playing.event->getEventName() == event->getEventName()) {
			++sameEvent;
		}
	}
	const UnsignedInt pending = pendingEventCount(event->getEventName());
	sameEvent += pending;
	if (info->m_limit > 0 && sameEvent >= static_cast<UnsignedInt>(info->m_limit)) {
		if (BitIsSet(info->m_control, AC_INTERRUPT)
			&& pending < static_cast<UnsignedInt>(info->m_limit)) {
			for (const PlayingAudio &playing : m_playing) {
				if (!playing.forced && !playing.stopping && playing.event != nullptr
					&& playing.event->getEventName() == event->getEventName()) {
					event->setHandleToKill(playing.event->getPlayingHandle());
					break;
				}
			}
			return FALSE;
		}
		return TRUE;
	}
	return isChannelFull(channelFor(*event));
}

Bool XAudio2AudioManager::isPlayingLowerPriority(AudioEventRTS *event) const
{
	if (event == nullptr || event->getAudioEventInfo() == nullptr) {
		return FALSE;
	}
	const Channel channel = channelFor(*event);
	for (const PlayingAudio &playing : m_playing) {
		if (!playing.forced && !playing.stopping && playing.channel == channel
			&& playing.event->getAudioPriority() < event->getAudioPriority()) {
			return TRUE;
		}
	}
	return FALSE;
}

Bool XAudio2AudioManager::isPlayingAlready(AudioEventRTS *event) const
{
	if (event == nullptr) {
		return FALSE;
	}
	const PlayingAudio *playing = findPlaying(event->getPlayingHandle());
	if (playing != nullptr && playing->generation == m_lifecycleGeneration && !playing->stopping) {
		return TRUE;
	}
	for (const PlayingAudio &candidate : m_playing) {
		if (!candidate.stopping && candidate.event != nullptr
			&& candidate.event->getEventName() == event->getEventName()) {
			return TRUE;
		}
	}
	for (const AudioRequest *request : m_audioRequests) {
		if (request != nullptr && request->m_request == AR_Play
			&& request->m_pendingEvent != nullptr
			&& request->m_pendingEvent->getEventName() == event->getEventName()) {
			return TRUE;
		}
	}
	return FALSE;
}

Bool XAudio2AudioManager::isObjectPlayingVoice(UnsignedInt objID) const
{
	if (objID == 0) {
		return FALSE;
	}
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
		if (playing.volume <= 0.0f) {
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
		if (m_assetSource != nullptr
			&& m_assetSource->matchesFileIdentity(playing.assetFileName, fileToClose)) {
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

Bool XAudio2AudioManager::eventAffectEnabled(const AudioEventRTS &event) const
{
	return eventAffectEnabledForAudio(event, *this);
}

Bool XAudio2AudioManager::isAudibleAtAdmission(const AudioEventRTS &event) const
{
	const AudioEventInfo *info = event.getAudioEventInfo();
	if (info == nullptr || !event.isPositionalAudio()
		|| BitIsSet(info->m_type, ST_GLOBAL) || info->m_priority == AP_CRITICAL) {
		return TRUE;
	}
	const Coord3D *position = const_cast<AudioEventRTS &>(event).getCurrentPosition();
	if (position == nullptr) {
		return TRUE;
	}
	Coord3D distance = *position;
	distance.sub(m_listenerPosition);
	if (distance.length() >= info->m_maxDistance) {
		return FALSE;
	}
	if (BitIsSet(info->m_type, ST_SHROUDED)
		&& isAudioEventShroudedForLocalPlayer(position)) {
		return FALSE;
	}
	return TRUE;
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
	if (victim.forced || victim.event == nullptr || victim.event->getAudioEventInfo() == nullptr
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
		if (playing.forced || playing.stopping || playing.channel != channel || playing.event == nullptr
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
		volume *= playing.event->getVolumeShift();
	}
	if (playing.event != nullptr && playing.event->getAudioEventInfo() != nullptr) {
		const AudioEventInfo *info = playing.event->getAudioEventInfo();
		if (playing.speechVolumeOverride) volume *= m_speechVolume;
		else if (info->m_soundType == AT_Music) volume *= m_musicVolume;
		else if (info->m_soundType == AT_Streaming) volume *= m_speechVolume;
		else if (playing.event->isPositionalAudio()) volume *= m_sound3DVolume;
		else volume *= m_soundVolume;
	}
	if (playing.channel == Channel::SAMPLE_3D) {
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
				} else if (length > minDistance
					&& m_audioSettings != nullptr
					&& m_audioSettings->m_use3DSoundRangeVolumeFade) {
					const Real normalized = std::max(0.0f, std::min(1.0f,
						(length - minDistance) / (maxDistance - minDistance)));
					Real attenuation = 1.0f - normalized;
					if (m_audioSettings != nullptr && m_audioSettings->m_use3DSoundRangeVolumeFade) {
						attenuation = 1.0f - static_cast<Real>(std::pow(
							normalized, m_audioSettings->m_3DSoundRangeVolumeFadeExponent));
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
			if (playing.channel == Channel::SAMPLE_3D && playing.event != nullptr
				&& playing.event->isPositionalAudio()) {
				const Coord3D *position = playing.event->getCurrentPosition();
				if (position != nullptr) {
					const XAudio2SpatializationPose emitterPose = makeSpatialPose(
						*position, m_listenerOrientation);
					if (!m_service->setVoiceSpatialization(playing.voice,
							m_listenerPose, emitterPose)) {
						failPlaying(playing);
						continue;
					}
				}
			}
			m_service->setVoiceVolume(playing.voice, effectiveVolume(playing));
		}
	}
}

void XAudio2AudioManager::setDeviceListenerPosition()
{
	m_listenerPose = makeSpatialPose(m_listenerPosition, m_listenerOrientation);
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
