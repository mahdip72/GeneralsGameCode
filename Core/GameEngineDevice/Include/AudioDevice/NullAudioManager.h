#pragma once

#include "Common/GameAudio.h"

#include <memory>

class AudioAssetSource;

// A deterministic, device-free AudioManager.  It deliberately inherits only
// AudioManager so headless x64 code cannot acquire a legacy video/audio handle.
class NullAudioManager final : public AudioManager
{
public:
	NullAudioManager();
	~NullAudioManager() override;

#if defined(RTS_DEBUG)
	void audioDebugDisplay(DebugDisplayInterface *, void *, FILE * = nullptr) override {}
#endif

	void init() override;
	void postProcessLoad() override;
	void reset() override;
	void update() override;

	void stopAudio(AudioAffect) override {}
	void pauseAudio(AudioAffect) override {}
	void resumeAudio(AudioAffect) override {}
	void pauseAmbient(Bool) override {}
	void killAudioEventImmediately(AudioHandle) override;

	AsciiString nextMusicTrack() override;
	AsciiString prevMusicTrack() override;
	Bool isMusicPlaying() const override { return FALSE; }
	Bool hasMusicTrackCompleted(const AsciiString &, Int) const override { return FALSE; }

	void openDevice() override {}
	void closeDevice() override {}
	void *getDevice() override { return nullptr; }

	void notifyOfAudioCompletion(UnsignedInt, UnsignedInt) override {}
	UnsignedInt getProviderCount() const override { return 0; }
	AsciiString getProviderName(UnsignedInt) const override;
	UnsignedInt getProviderIndex(AsciiString) const override { return PROVIDER_ERROR; }
	void selectProvider(UnsignedInt) override {}
	void unselectProvider() override {}
	UnsignedInt getSelectedProvider() const override { return PROVIDER_ERROR; }
	void setSpeakerType(UnsignedInt) override {}
	UnsignedInt getSpeakerType() override { return 0; }

	UnsignedInt getNum2DSamples() const override { return 0; }
	UnsignedInt getNum3DSamples() const override { return 0; }
	UnsignedInt getNumStreams() const override { return 0; }
	UnsignedInt getNumAvailable2DSamples() const override { return 0; }
	UnsignedInt getNumAvailable3DSamples() const override { return 0; }

	Bool doesViolateLimit(AudioEventRTS *) const override { return FALSE; }
	Bool isPlayingLowerPriority(AudioEventRTS *) const override { return FALSE; }
	Bool isPlayingAlready(AudioEventRTS *) const override { return FALSE; }
	Bool isObjectPlayingVoice(UnsignedInt) const override { return FALSE; }

	void adjustVolumeOfPlayingAudio(AsciiString, Real) override {}
	void removePlayingAudio(AsciiString) override {}
	void removeAllDisabledAudio() override {}
	Bool has3DSensitiveStreamsPlaying() const override { return FALSE; }
	void friend_forcePlayAudioEventRTS(const AudioEventRTS *) override {}

	void setPreferredProvider(AsciiString provider) override { m_preferredProvider = provider; }
	void setPreferredSpeaker(AsciiString speaker) override { m_preferredSpeaker = speaker; }
	Real getFileLengthMS(AsciiString) const override;
	void closeAnySamplesUsingFile(const void *) override {}

	void setAssetSource(AudioAssetSource *source);
	AudioAssetSource *getAssetSource() const { return m_assetSource; }
	UnsignedInt getLifecycleGeneration() const { return m_lifecycleGeneration; }

protected:
	void setDeviceListenerPosition() override {}

private:
	std::unique_ptr<AudioAssetSource> m_ownedAssetSource;
	AudioAssetSource *m_assetSource;
	AsciiString m_preferredProvider;
	AsciiString m_preferredSpeaker;
	UnsignedInt m_lifecycleGeneration;
};
