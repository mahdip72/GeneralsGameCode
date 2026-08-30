/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 TheSuperHackers
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

////////////////////////////////////////////////////////////////////////////////
//																																						//
//  (c) 2001-2003 Electronic Arts Inc.																				//
//																																						//
////////////////////////////////////////////////////////////////////////////////

//////// FFmpegVideoPlayer.cpp ///////////////////////////
// Stephan Vedder, April 2025
/////////////////////////////////////////////////

//----------------------------------------------------------------------------
//         Includes
//----------------------------------------------------------------------------

#include "Lib/BaseType.h"
#include "VideoDevice/FFmpeg/FFmpegVideoPlayer.h"
#include "Common/AudioAffect.h"
#include "Common/GameAudio.h"
#include "Common/GameMemory.h"
#include "Common/GlobalData.h"
#include "Common/Registry.h"
#include "Common/FileSystem.h"

#include "VideoDevice/FFmpeg/FFmpegFile.h"
#include "VideoDevice/FFmpeg/FFmpegMoviePlayback.h"

#if defined(_WIN64)
#include "XAudio2AudioDevice/XAudio2AudioService.h"
#include "XAudio2AudioDevice/XAudio2MoviePcmSink.h"
#endif

extern "C" {
	#include <libavcodec/avcodec.h>
	#include <libavutil/pixdesc.h>
	#include <libswscale/swscale.h>
}

#include <algorithm>
#include <chrono>
#include <limits>

namespace
{
UnsignedInt64 getMonotonicMilliseconds()
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
}

double getMovieSpeechGain()
{
	// Match the legacy Bink movie attenuation, but preserve a true zero so the
	// live SpeechOn control can mute an already-playing native movie.
	return TheAudio != nullptr && TheAudio->isOn(AudioAffect_Speech)
		? static_cast<double>(TheAudio->getVolume(AudioAffect_Speech)) * 0.8 : 0.0;
}
}

//----------------------------------------------------------------------------
//         Externals
//----------------------------------------------------------------------------



//----------------------------------------------------------------------------
//         Defines
//----------------------------------------------------------------------------
#define VIDEO_LANG_PATH_FORMAT "Data/%s/Movies/%s.%s"
#define VIDEO_PATH	"Data\\Movies"
#define VIDEO_EXT		"bik"



//----------------------------------------------------------------------------
//         Private Types
//----------------------------------------------------------------------------



//----------------------------------------------------------------------------
//         Private Data
//----------------------------------------------------------------------------



//----------------------------------------------------------------------------
//         Public Data
//----------------------------------------------------------------------------



//----------------------------------------------------------------------------
//         Private Prototypes
//----------------------------------------------------------------------------



//----------------------------------------------------------------------------
//         Private Functions
//----------------------------------------------------------------------------



//----------------------------------------------------------------------------
//         Public Functions
//----------------------------------------------------------------------------


//============================================================================
// FFmpegVideoPlayer::FFmpegVideoPlayer
//============================================================================

FFmpegVideoPlayer::FFmpegVideoPlayer()
{

}

//============================================================================
// FFmpegVideoPlayer::~FFmpegVideoPlayer
//============================================================================

FFmpegVideoPlayer::~FFmpegVideoPlayer()
{
	deinit();
}

//============================================================================
// FFmpegVideoPlayer::init
//============================================================================

void	FFmpegVideoPlayer::init()
{
	// Need to load the stuff from the ini file.
	VideoPlayer::init();
#if defined(_WIN64)
	if (m_audioService == nullptr) {
		m_audioService = NEW XAudio2AudioService();
		if (m_audioService != nullptr && !m_audioService->open()) {
			delete m_audioService;
			m_audioService = nullptr;
		}
	}
#endif

}

//============================================================================
// FFmpegVideoPlayer::deinit
//============================================================================

void FFmpegVideoPlayer::deinit()
{
	closeAllStreams();
#if defined(_WIN64)
	if (m_audioService != nullptr) {
		m_audioService->shutdown();
		delete m_audioService;
		m_audioService = nullptr;
	}
#endif
	VideoPlayer::deinit();
}

//============================================================================
// FFmpegVideoPlayer::reset
//============================================================================

void	FFmpegVideoPlayer::reset()
{
	VideoPlayer::reset();
}

//============================================================================
// FFmpegVideoPlayer::update
//============================================================================

void	FFmpegVideoPlayer::update()
{
#if defined(_WIN64)
	if (m_audioService != nullptr) {
		m_audioService->serviceVoices();
		m_audioService->discardCompletions();
	}
#endif
	VideoPlayer::update();
#if defined(_WIN64)
	if (m_audioService != nullptr) {
		m_audioService->serviceVoices();
		m_audioService->discardCompletions();
	}
#endif
}

//============================================================================
// FFmpegVideoPlayer::loseFocus
//============================================================================

void	FFmpegVideoPlayer::loseFocus()
{
	VideoPlayer::loseFocus();
}

//============================================================================
// FFmpegVideoPlayer::regainFocus
//============================================================================

void	FFmpegVideoPlayer::regainFocus()
{
	VideoPlayer::regainFocus();
}

//============================================================================
// FFmpegVideoPlayer::createStream
//============================================================================

VideoStreamInterface* FFmpegVideoPlayer::createStream( File* file )
{

	if ( file == nullptr )
	{
		return nullptr;
	}

	FFmpegFile* ffmpegHandle = NEW FFmpegFile();
	if(!ffmpegHandle->open(file))
	{
		delete ffmpegHandle;
		return nullptr;
	}

	AudioPcmSink *audioSink = nullptr;
#if defined(_WIN64)
	if (m_audioService != nullptr && m_audioService->isOpen()) {
		XAudio2MoviePcmSink *movieSink = NEW XAudio2MoviePcmSink(*m_audioService);
		if (movieSink != nullptr && movieSink->isReady()) {
			audioSink = movieSink;
		} else {
			delete movieSink;
		}
	}
#endif

	FFmpegVideoStream *stream = NEW FFmpegVideoStream(ffmpegHandle, audioSink, getMovieSpeechGain());
	if (stream == nullptr) {
		delete audioSink;
		delete ffmpegHandle;
		return nullptr;
	}
	if (!stream->m_gotFrame) {
		delete stream;
		return nullptr;
	}

	if ( stream )
	{

		stream->m_next = m_firstStream;
		stream->m_player = this;
		m_firstStream = stream;

		DEBUG_LOG(("FFmpegVideoPlayer::createStream() - typed movie audio sink selected"));
	}

	return stream;
}

//============================================================================
// FFmpegVideoPlayer::open
//============================================================================

VideoStreamInterface*	FFmpegVideoPlayer::open( AsciiString movieTitle )
{
	VideoStreamInterface*	stream = nullptr;

	const Video* pVideo = getVideo(movieTitle);
	if (pVideo) {
		DEBUG_LOG(("FFmpegVideoPlayer::createStream() - About to open bink file"));

		if (TheGlobalData->m_modDir.isNotEmpty())
		{
			char filePath[ _MAX_PATH ];
			snprintf( filePath, ARRAY_SIZE(filePath), "%s%s\\%s.%s", TheGlobalData->m_modDir.str(), VIDEO_PATH, pVideo->m_filename.str(), VIDEO_EXT );
			File* file =  TheFileSystem->openFile(filePath);
			DEBUG_ASSERTLOG(!file, ("opened bink file %s", filePath));
			if (file)
			{
				return createStream( file );
			}
		}

		char localizedFilePath[ _MAX_PATH ];
		snprintf( localizedFilePath, ARRAY_SIZE(localizedFilePath), VIDEO_LANG_PATH_FORMAT, GetRegistryLanguage().str(), pVideo->m_filename.str(), VIDEO_EXT );
		File* file =  TheFileSystem->openFile(localizedFilePath);
		DEBUG_ASSERTLOG(!file, ("opened localized bink file %s", localizedFilePath));
		if (!file)
		{
			char filePath[ _MAX_PATH ];
			snprintf( filePath, ARRAY_SIZE(filePath), "%s\\%s.%s", VIDEO_PATH, pVideo->m_filename.str(), VIDEO_EXT );
			file = TheFileSystem->openFile(filePath);
			DEBUG_ASSERTLOG(!file, ("opened bink file %s", filePath));
		}

		DEBUG_LOG(("FFmpegVideoPlayer::createStream() - About to create stream"));
		stream = createStream( file );
	}

	return stream;
}

//============================================================================
// FFmpegVideoPlayer::load
//============================================================================

VideoStreamInterface*	FFmpegVideoPlayer::load( AsciiString movieTitle )
{
	return open(movieTitle); // load() used to have the same body as open(), so I'm combining them.  Munkee.
}

//============================================================================
//============================================================================
void FFmpegVideoPlayer::notifyVideoPlayerOfNewProvider( Bool nowHasValid )
{
	(void)nowHasValid;
}

//============================================================================
// FFmpegVideoStream::FFmpegVideoStream
//============================================================================

FFmpegVideoStream::FFmpegVideoStream(FFmpegFile* file, AudioPcmSink *audioSink, double initialGain)
: m_ffmpegFile(file), m_audioSink(audioSink)
{
	if (m_ffmpegFile == nullptr) {
		m_good = false;
		return;
	}
	FFmpegMoviePlaybackOptions options;
	options.mode = FFmpegMoviePlaybackMode::SHOW_LAST_FRAME;
	options.gain = initialGain;
	m_playback = NEW FFmpegMoviePlayback(*m_ffmpegFile, m_audioSink, options);
	if (m_playback == nullptr) {
		m_good = false;
		return;
	}
	m_playback->setVideoCallback(
		static_cast<void (*)(const AVFrame *, const FFmpegFrameMetadata &, void *)>(&FFmpegVideoStream::onFrame), this);

	// Decode until we have our first video frame, but keep open-time work
	// bounded just like frameNext/frameGoto and the destructor finish path.
	for (std::size_t attempts = 0; attempts < 256 && m_good && !m_gotFrame; ++attempts) {
		const bool progressed = m_playback->pump(1);
		if (!progressed && !m_playback->isTerminal()) {
			markPlaybackFailed();
			break;
		}
		if (m_audioSink != nullptr && !m_audioSink->serviceSink()) {
			markPlaybackFailed();
			break;
		}
		if (m_playback->state() == FFmpegMoviePlaybackState::FAILED) {
			markPlaybackFailed();
			break;
		}
	}
	if (m_good && !m_gotFrame) {
		m_good = false;
	}

	m_startTime = getMonotonicMilliseconds();
}

void FFmpegVideoStream::syncSpeechGain()
{
	if (m_playback != nullptr) {
		m_playback->setGain(getMovieSpeechGain());
	}
}

//============================================================================
// FFmpegVideoStream::~FFmpegVideoStream
//============================================================================

FFmpegVideoStream::~FFmpegVideoStream()
{
	// Closing or skipping a movie is an abort operation.  Draining a long
	// movie here can synchronously decode its remaining tail on the game thread
	// and make an intro transition appear hung.  FFmpegMoviePlayback teardown
	// resets the active audio generation before the sink is closed below.
	av_frame_free(&m_frame);
	sws_freeContext(m_swsContext);
	delete m_playback;
	m_playback = nullptr;
	if (m_audioSink != nullptr) {
		m_audioSink->close();
	}
	delete m_audioSink;
	delete m_ffmpegFile;
}

void FFmpegVideoStream::onFrame(const AVFrame *frame, const FFmpegFrameMetadata &metadata, void *user_data)
{
	FFmpegVideoStream *videoStream = static_cast<FFmpegVideoStream *>(user_data);
	if (videoStream != nullptr && metadata.streamType == AVMEDIA_TYPE_VIDEO) {
		AVFrame *cloned_frame = av_frame_clone(frame);
		if (cloned_frame != nullptr) {
			av_frame_free(&videoStream->m_frame);
			videoStream->m_frame = cloned_frame;
			videoStream->m_gotFrame = true;
		}
	}
}


//============================================================================
// FFmpegVideoStream::update
//============================================================================

void FFmpegVideoStream::update()
{
	if (!m_good || m_playback == nullptr) {
		return;
	}
	syncSpeechGain();
	// Blocking load-screen loops update the stream directly rather than going
	// through FFmpegVideoPlayer::update().  Service the stream-owned voice here
	// as well so pending PCM starts and completion/drain state keeps moving.
	if (m_audioSink != nullptr && !m_audioSink->serviceSink()) {
		markPlaybackFailed();
		return;
	}
	if (m_playback->state() == FFmpegMoviePlaybackState::ENDED) {
		m_gotFrame = m_playback->hasCurrentFrame();
		return;
	}
	if (m_playback->state() == FFmpegMoviePlaybackState::FAILED) {
		markPlaybackFailed();
		return;
	}
	if (m_playback->state() == FFmpegMoviePlaybackState::DRAINING) {
		// A retained final frame must not prevent callback-driven audio drain
		// from publishing the terminal state.  DRAINING pump() only services
		// the sink; it cannot replace the display-owned frame.
		const bool progressed = m_playback->pump(1);
		if (m_playback->state() == FFmpegMoviePlaybackState::FAILED
			|| (!progressed && !m_playback->isTerminal())) {
			markPlaybackFailed();
		}
		return;
	}
	// Display owns frame advancement through frameNext(). Do not replace a
	// decoded frame before that frame has been presented; doing so lets a fast
	// game update loop continually move the presentation timestamp ahead of the
	// display and leaves the newly allocated movie buffer black.
	if (m_gotFrame) {
		return;
	}
	const bool progressed = m_playback->pump(32);
	if (!progressed && !m_playback->isTerminal()) {
		markPlaybackFailed();
	}
	if (m_playback->state() == FFmpegMoviePlaybackState::FAILED) {
		markPlaybackFailed();
	}
}

void FFmpegVideoStream::markPlaybackFailed()
{
	const Bool wasGood = m_good;
	m_good = false;
	m_gotFrame = false;
	if (m_playback != nullptr && !m_playback->isTerminal()) {
		m_playback->failPlayback();
	}
	// The owner-side close quiesces native callbacks.  The stream remains
	// terminal so hosts can observe the failure and advance before deleting it.
	if (m_audioSink != nullptr) {
		m_audioSink->close();
	}
	if (wasGood) {
		DEBUG_LOG(("FFmpegVideoStream playback failed; native movie audio closed"));
	}
}

Bool FFmpegVideoStream::isFinished() const
{
	return !m_good || m_playback == nullptr || m_playback->isTerminal();
}

Bool FFmpegVideoStream::isPlaybackFailed() const
{
	return !m_good || (m_playback != nullptr
		&& m_playback->state() == FFmpegMoviePlaybackState::FAILED);
}

//============================================================================
// FFmpegVideoStream::isFrameReady
//============================================================================

Bool FFmpegVideoStream::isFrameReady()
{
	return m_good && m_playback != nullptr && m_gotFrame && m_playback->isFrameReady();

	//return !BinkWait( m_handle );
}

//============================================================================
// FFmpegVideoStream::frameDecompress
//============================================================================

void FFmpegVideoStream::frameDecompress()
{
	//BinkDoFrame( m_handle );
}

//============================================================================
// FFmpegVideoStream::frameRender
//============================================================================

void FFmpegVideoStream::frameRender( VideoBuffer *buffer )
{
	if (buffer == nullptr) {
		return;
	}

	if (m_frame == nullptr) {
		return;
	}

	if (m_frame->data[0] == nullptr) {
		return;
	}

	AVPixelFormat dst_pix_fmt;
	UnsignedInt bytes_per_pixel;
	Bool direct_bgra8_publication = FALSE;

	switch (buffer->format()) {
		case VideoBuffer::TYPE_R8G8B8:
			dst_pix_fmt = AV_PIX_FMT_BGR24;
			bytes_per_pixel = 3;
			break;
		case VideoBuffer::TYPE_X8R8G8B8:
			// The direct D3D11 publication path consumes BGRA8. BGRA also gives
			// non-alpha movie sources the opaque alpha that the legacy X8 surface
			// conversion supplied before DRAW_IMAGE_ALPHA sampling.
			dst_pix_fmt = AV_PIX_FMT_BGRA;
			bytes_per_pixel = 4;
			{
				const AVPixFmtDescriptor *source_descriptor = av_pix_fmt_desc_get(
					static_cast<AVPixelFormat>(m_frame->format));
				direct_bgra8_publication = source_descriptor != nullptr &&
					(source_descriptor->flags & AV_PIX_FMT_FLAG_ALPHA) == 0;
			}
			break;
		case VideoBuffer::TYPE_R5G6B5:
			dst_pix_fmt = AV_PIX_FMT_RGB565;
			bytes_per_pixel = 2;
			break;
		case VideoBuffer::TYPE_X1R5G5B5:
			dst_pix_fmt = AV_PIX_FMT_RGB555;
			bytes_per_pixel = 2;
			break;
		default:
			return;
	}

	m_swsContext = sws_getCachedContext(m_swsContext,
		width(),
		height(),
		static_cast<AVPixelFormat>(m_frame->format),
		buffer->width(),
		buffer->height(),
		dst_pix_fmt,
		SWS_BICUBIC,
		nullptr,
		nullptr,
		nullptr);
	if (m_swsContext == nullptr) {
		DEBUG_LOG(("Failed to allocate video scaling context"));
		return;
	}

	uint8_t *buffer_data = static_cast<uint8_t *>(buffer->lock());
	if (buffer_data == nullptr) {
		DEBUG_LOG(("Failed to lock videobuffer"));
		return;
	}

	int dst_strides[] = { (int)buffer->pitch() };
	uint8_t *dst_data[] = {
		buffer_data + buffer->yPos() * buffer->pitch() + buffer->xPos() * bytes_per_pixel
	};
	const int result =
		sws_scale(m_swsContext, m_frame->data, m_frame->linesize, 0, height(), dst_data, dst_strides);
	DEBUG_ASSERTLOG(result >= 0, ("Failed to scale frame"));
	if (result == static_cast<int>(buffer->height()) &&
		direct_bgra8_publication) {
		buffer->publishLockedFrame();
	}
	buffer->unlock();
}

//============================================================================
// FFmpegVideoStream::frameNext
//============================================================================

void FFmpegVideoStream::frameNext()
{
	if (!m_good || m_playback == nullptr) {
		return;
	}
	syncSpeechGain();
	if (m_playback->isTerminal()) {
		m_gotFrame = m_playback->state() == FFmpegMoviePlaybackState::ENDED
			&& m_playback->hasCurrentFrame();
		if (m_playback->state() == FFmpegMoviePlaybackState::FAILED) {
			markPlaybackFailed();
		}
		return;
	}
	const Int previousFrame = m_playback->frameIndex();
	m_gotFrame = false;
	for (std::size_t attempts = 0; attempts < 32 && !m_playback->isTerminal(); ++attempts) {
		const bool progressed = m_playback->pump(1);
		if (!progressed && !m_playback->isTerminal()) {
			markPlaybackFailed();
			break;
		}
		if (m_audioSink != nullptr && !m_audioSink->serviceSink()) {
			markPlaybackFailed();
			break;
		}
		if (m_playback->hasCurrentFrame() && m_playback->frameIndex() != previousFrame) {
			// Display owns frame cadence. Stop on the first new video frame;
			// dropping multiple frames is unsafe until the native sink exposes a
			// continuous media cursor across queue discontinuities.
			m_gotFrame = true;
			break;
		}
		if (m_playback->state() == FFmpegMoviePlaybackState::DRAINING) {
			break;
		}
	}
	if (!m_gotFrame && m_playback->isTerminal()) {
		m_gotFrame = m_playback->state() == FFmpegMoviePlaybackState::ENDED
			&& m_playback->hasCurrentFrame();
		if (m_playback->state() == FFmpegMoviePlaybackState::FAILED) {
			markPlaybackFailed();
		}
	}
}

Bool FFmpegVideoStream::finishPlayback()
{
	if (m_playback == nullptr) {
		markPlaybackFailed();
		return TRUE;
	}
	if (!m_good) {
		markPlaybackFailed();
		return TRUE;
	}
	if (m_playback->state() == FFmpegMoviePlaybackState::FAILED) {
		markPlaybackFailed();
		return TRUE;
	}
	if (m_playback->state() == FFmpegMoviePlaybackState::ENDED) {
		return TRUE;
	}
	syncSpeechGain();
	m_gotFrame = false;
	// Perform one bounded owner-side service quantum. Blocking hosts retain
	// control of abort polling between calls, so a stalled device cannot make
	// Escape, shutdown, or a load transition unresponsive.
	if (m_audioSink != nullptr && !m_audioSink->serviceSink()) {
		markPlaybackFailed();
		return TRUE;
	}
	const bool progressed = m_playback->pump(1);
	if (m_playback->state() == FFmpegMoviePlaybackState::FAILED
		|| (!progressed && m_playback->state() != FFmpegMoviePlaybackState::DRAINING)) {
		markPlaybackFailed();
		return TRUE;
	}
	return m_playback->isTerminal() ? TRUE : FALSE;
}

//============================================================================
// FFmpegVideoStream::frameIndex
//============================================================================

Int FFmpegVideoStream::frameIndex()
{
	return m_ffmpegFile->getCurrentFrame();
}

//============================================================================
// FFmpegVideoStream::totalFrames
//============================================================================

Int	FFmpegVideoStream::frameCount()
{
	return m_ffmpegFile->getNumFrames();
}

//============================================================================
// FFmpegVideoStream::frameGoto
//============================================================================

Bool FFmpegVideoStream::frameGoto( Int index )
{
	syncSpeechGain();
	const Int frame_count = m_ffmpegFile->getNumFrames();
	if (frame_count > 0) {
		index = std::clamp(index, 0, frame_count - 1);
	} else if (index < 0) {
		index = 0;
	}

	if (m_playback == nullptr || !m_playback->seekFrame(index)) {
		av_frame_free(&m_frame);
		markPlaybackFailed();
		return FALSE;
	}

	av_frame_free(&m_frame);
	m_gotFrame = false;
	m_good = true;
	for (std::size_t attempts = 0; attempts < 256 && m_good && !m_gotFrame; ++attempts) {
		const Int targetFrame = index;
		const bool progressed = m_playback->pump(32);
		if (!progressed) {
			if (m_playback->state() == FFmpegMoviePlaybackState::ENDED) {
				m_good = true;
			} else {
				markPlaybackFailed();
			}
			break;
		}
		if (m_audioSink != nullptr && !m_audioSink->serviceSink()) {
			markPlaybackFailed();
			break;
		}
		if (m_playback->state() == FFmpegMoviePlaybackState::FAILED) {
			markPlaybackFailed();
			break;
		}
		if (m_playback->hasCurrentFrame() && m_playback->frameIndex() >= targetFrame) {
			m_gotFrame = true;
		}
	}

	m_startTime = getMonotonicMilliseconds();
	return m_good && m_gotFrame;
}

//============================================================================
// VideoStream::height
//============================================================================

Int		FFmpegVideoStream::height()
{
	return m_ffmpegFile->getHeight();
}

//============================================================================
// VideoStream::width
//============================================================================

Int		FFmpegVideoStream::width()
{
	return m_ffmpegFile->getWidth();
}


