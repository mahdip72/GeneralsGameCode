#pragma once

#include "AudioDevice/AudioAssetSource.h"
#include "Common/AudioEventRTS.h"
#include "Common/AudioHandleSpecialValues.h"
#include "Common/GameAudio.h"
#include "XAudio2AudioDevice/XAudio2AudioService.h"

#include <memory>
#include <vector>

struct XAudio2CompatibilityPcmBudget;

class XAudio2AudioManager final : public AudioManager
{
public:
	enum class Channel : UnsignedByte
	{
		SAMPLE_2D,
		SAMPLE_3D,
		STREAM,
		MUSIC
	};

	XAudio2AudioManager();
	XAudio2AudioManager(XAudio2AudioService *service, AudioAssetSource *assetSource);
	~XAudio2AudioManager() override;

	XAudio2AudioManager(const XAudio2AudioManager &) = delete;
	XAudio2AudioManager &operator=(const XAudio2AudioManager &) = delete;

#if defined(RTS_DEBUG)
	void audioDebugDisplay(DebugDisplayInterface *, void *, FILE * = nullptr) override {}
#endif

	void init() override;
	void postProcessLoad() override;
	void reset() override;
	void update() override;

	AudioHandle addAudioEvent(const AudioEventRTS *eventToAdd) override;
	void removeAudioEvent(AudioHandle audioEvent) override;
	void stopAudio(AudioAffect which) override;
	void pauseAudio(AudioAffect which) override;
	void resumeAudio(AudioAffect which) override;
	void pauseAmbient(Bool shouldPause) override;
	void killAudioEventImmediately(AudioHandle audioEvent) override;

	AsciiString nextMusicTrack() override;
	AsciiString prevMusicTrack() override;
	Bool isMusicPlaying() const override;
	Bool hasMusicTrackCompleted(const AsciiString &trackName, Int numberOfTimes) const override;
	Bool isCurrentlyPlaying(AudioHandle handle) override;

	void openDevice() override;
	void closeDevice() override;
	void *getDevice() override { return nullptr; }
	void notifyOfAudioCompletion(UnsignedInt, UnsignedInt) override {}

	UnsignedInt getProviderCount() const override { return 1; }
	AsciiString getProviderName(UnsignedInt providerNum) const override;
	UnsignedInt getProviderIndex(AsciiString providerName) const override;
	void selectProvider(UnsignedInt providerNum) override;
	void unselectProvider() override;
	UnsignedInt getSelectedProvider() const override { return m_providerSelected ? 0U : PROVIDER_ERROR; }
	void setSpeakerType(UnsignedInt speakerType) override { m_speakerType = speakerType; }
	UnsignedInt getSpeakerType() override { return m_speakerType; }

	UnsignedInt getNum2DSamples() const override { return m_num2DSamples; }
	UnsignedInt getNum3DSamples() const override { return m_num3DSamples; }
	UnsignedInt getNumStreams() const override { return m_numStreams; }
	UnsignedInt getNumAvailable2DSamples() const override;
	UnsignedInt getNumAvailable3DSamples() const override;

	Bool doesViolateLimit(AudioEventRTS *event) const override;
	Bool isPlayingLowerPriority(AudioEventRTS *event) const override;
	Bool isPlayingAlready(AudioEventRTS *event) const override;
	Bool isObjectPlayingVoice(UnsignedInt objID) const override;

	void adjustVolumeOfPlayingAudio(AsciiString eventName, Real newVolume) override;
	void removePlayingAudio(AsciiString eventName) override;
	void removeAllDisabledAudio() override;
	Bool has3DSensitiveStreamsPlaying() const override;
	void friend_forcePlayAudioEventRTS(const AudioEventRTS *eventToPlay) override;

	void setPreferredProvider(AsciiString provider) override { m_preferredProvider = provider; }
	void setPreferredSpeaker(AsciiString speaker) override { m_preferredSpeaker = speaker; }
	Real getFileLengthMS(AsciiString fileName) const override;
	void closeAnySamplesUsingFile(const void *fileToClose) override;

	void processRequestList() override;

	void setService(XAudio2AudioService *service);
	XAudio2AudioService *getService() const { return m_service; }
	void setAssetSource(AudioAssetSource *assetSource);
	AudioAssetSource *getAssetSource() const { return m_assetSource; }
	UnsignedInt getLifecycleGeneration() const { return m_lifecycleGeneration; }
	UnsignedInt getActiveAudioCount() const { return static_cast<UnsignedInt>(m_playing.size()); }
	UnsignedInt getPendingAudioRequestCount() const
	{
		return static_cast<UnsignedInt>(m_audioRequests.size());
	}
	Bool isOpen() const { return m_open; }

#if defined(RTS_NATIVE_AUDIO_TEST_HOOK)
	// Device-free contract hook used only by the isolated native-audio fixture.
	// It exercises the same voice admission/submission path as a phase without
	// constructing legacy game-world objects.
	Bool runInjectedPlaybackProbe(AsciiString fileName);
	void setChannelLimitsForTest(UnsignedInt samples2D, UnsignedInt samples3D, UnsignedInt streams);
	void setAudioSettingsForTest(AudioSettings *settings);
	void setActiveMusicTrackForTest(const AsciiString &track);
	void setOwnedServiceForTest(std::unique_ptr<XAudio2AudioService> service);
#endif

protected:
	void setDeviceListenerPosition() override;

private:
	struct PlayingAudio
	{
		RefCountPtr<DynamicAudioEventRTS> event;
		XAudio2PcmVoiceHandle voice;
		Channel channel = Channel::SAMPLE_2D;
		PortionToPlay phase = PP_Done;
		UnsignedInt generation = 0;
		std::uint64_t voiceSequence = 0;
		std::uint64_t phaseFirstSequence = 0;
		Real phaseRemainingMS = 0.0f;
		Real phaseDurationMS = 0.0f;
		Real volume = 1.0f;
		Real fadeVolume = 1.0f;
		Int fadeFrames = 0;
		Bool paused = FALSE;
		Bool stopping = FALSE;
		Bool forced = FALSE;
		Bool speechVolumeOverride = FALSE;
		Bool waitingForGeneratedDelay = FALSE;
		Bool voiceOpen = FALSE;
		Bool needsVoiceService = FALSE;
		AsciiString assetFileName;
		std::unique_ptr<AudioPcmStream> pcmStream;
		UnsignedInt phaseSubmittedFrames = 0;
		UnsignedInt phaseQueuedBuffers = 0;
		UnsignedInt phaseCompletedFrames = 0;
		UnsignedInt phaseTotalFrames = 0;
	};

	DynamicAudioEventRTS *copyEvent(const AudioEventRTS *eventToCopy);
	void enqueuePlay(DynamicAudioEventRTS *event, Bool forced);
	void processPlayRequest(AudioRequest *request);
	void processStopRequest(AudioHandle handle);
	void processPauseRequest(AudioHandle handle);
	void startNextPhase(PlayingAudio &playing);
	void finishPlaying(PlayingAudio &playing, Bool naturalCompletion = FALSE);
	void failPlaying(PlayingAudio &playing);
	void drainCompletions();
	void processActiveAudio();
	void processFades();
	void releaseVoice(PlayingAudio &playing);
	Bool preparePhaseSource(PlayingAudio &playing, const AsciiString &fileName);
	Bool ensureVoice(PlayingAudio &playing);
	Bool submitPhase(PlayingAudio &playing);
	Bool queuePhaseLowWater(PlayingAudio &playing);
	Bool affectMatches(const PlayingAudio &playing, AudioAffect which) const;
	Bool requestAffectMatches(const AudioRequest *request, AudioAffect which) const;
	Bool canReplace(const PlayingAudio &victim, const AudioEventRTS &incoming) const;
	Bool isChannelFull(Channel channel) const;
	Bool isChannelFullForEvent(Channel channel, AudioHandle excludeHandle) const;
	UnsignedInt channelCount(Channel channel) const;
	UnsignedInt pendingChannelCount(Channel channel, AudioHandle excludeHandle = AHSV_NoSound) const;
	UnsignedInt channelLimit(Channel channel) const;
	UnsignedInt pendingEventCount(const AsciiString &eventName,
		AudioHandle excludeHandle = AHSV_NoSound) const;
	Bool eventAffectEnabled(const AudioEventRTS &event) const;
	Bool isAudibleAtAdmission(const AudioEventRTS &event) const;
	void stopExistingSpeechForUninterruptible(AudioHandle exceptHandle);
	PlayingAudio *findPlaying(AudioHandle handle);
	const PlayingAudio *findPlaying(AudioHandle handle) const;
	PlayingAudio *findLowestPriority(Channel channel, const AudioEventRTS &incoming);
	Real effectiveVolume(const PlayingAudio &playing) const;
	Real outputVolume(const PlayingAudio &playing) const;
	void recordMusicCompletion(const PlayingAudio &playing);
	void updatePlayingVolumes();
	void updateDisallowSpeechGuard();
	void clearPlaying();

	std::unique_ptr<XAudio2AudioService> m_ownedService;
	std::unique_ptr<AudioAssetSource> m_ownedAssetSource;
	std::shared_ptr<AudioVirtualFileSource> m_ownedVirtualFileSource;
	std::shared_ptr<XAudio2CompatibilityPcmBudget> m_compatibilityPcmBudget;
	XAudio2AudioService *m_service;
	Bool m_ownsService;
	AudioAssetSource *m_assetSource;
	std::vector<PlayingAudio> m_playing;
	std::vector<AudioHandle> m_forcePlayHandles;
	std::vector<std::pair<AsciiString, Int>> m_musicCompletions;
	AsciiString m_activeMusicTrack;
	AsciiString m_preferredProvider;
	AsciiString m_preferredSpeaker;
	UnsignedInt m_lifecycleGeneration;
	UnsignedInt m_num2DSamples;
	UnsignedInt m_num3DSamples;
	UnsignedInt m_numStreams;
	UnsignedInt m_speakerType;
	Bool m_providerSelected;
	Bool m_open;
	Bool m_admissionOpen;
	Bool m_ambientPaused;
	XAudio2SpatializationPose m_listenerPose;
};
