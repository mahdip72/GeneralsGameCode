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
	#include <libswscale/swscale.h>
}

#include <algorithm>
#include <chrono>

namespace
{
UnsignedInt64 getMonotonicMilliseconds()
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
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

	FFmpegVideoStream *stream = NEW FFmpegVideoStream(ffmpegHandle, audioSink);
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

FFmpegVideoStream::FFmpegVideoStream(FFmpegFile* file, AudioPcmSink *audioSink)
: m_ffmpegFile(file), m_audioSink(audioSink)
{
	if (m_ffmpegFile == nullptr) {
		m_good = false;
		return;
	}
	FFmpegMoviePlaybackOptions options;
	options.mode = FFmpegMoviePlaybackMode::SHOW_LAST_FRAME;
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
		m_good = m_playback->pump(64);
	}
	if (m_good && !m_gotFrame) {
		m_good = false;
	}

	m_startTime = getMonotonicMilliseconds();
}

//============================================================================
// FFmpegVideoStream::~FFmpegVideoStream
//============================================================================

FFmpegVideoStream::~FFmpegVideoStream()
{
	if (m_playback != nullptr && !m_playback->isTerminal()) {
		// Drain decoder/resampler tail while the sink is still alive.  This is
		// bounded and owner-side; callbacks never close the native voice.
		m_playback->finish(256);
	}
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
	if (m_playback->state() == FFmpegMoviePlaybackState::ENDED) {
		m_gotFrame = m_playback->hasCurrentFrame();
		return;
	}
	if (m_playback->state() == FFmpegMoviePlaybackState::FAILED) {
		m_good = false;
		m_gotFrame = false;
		return;
	}
	if (!m_playback->pump(32) && !m_playback->isTerminal()) {
		m_good = false;
	}
	if (m_playback->state() == FFmpegMoviePlaybackState::FAILED) {
		m_good = false;
		m_gotFrame = false;
	}
}

Bool FFmpegVideoStream::isFinished() const
{
	return !m_good || m_playback == nullptr || m_playback->isTerminal();
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

	switch (buffer->format()) {
		case VideoBuffer::TYPE_R8G8B8:
			dst_pix_fmt = AV_PIX_FMT_RGB24;
			bytes_per_pixel = 3;
			break;
		case VideoBuffer::TYPE_X8R8G8B8:
			dst_pix_fmt = AV_PIX_FMT_BGR0;
			bytes_per_pixel = 4;
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
	[[maybe_unused]] int result =
		sws_scale(m_swsContext, m_frame->data, m_frame->linesize, 0, height(), dst_data, dst_strides);
	DEBUG_ASSERTLOG(result >= 0, ("Failed to scale frame"));
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
	if (m_playback->isTerminal()) {
		m_gotFrame = m_playback->state() == FFmpegMoviePlaybackState::ENDED
			&& m_playback->hasCurrentFrame();
		if (m_playback->state() == FFmpegMoviePlaybackState::FAILED) {
			m_good = false;
		}
		return;
	}
	const Int previousFrame = m_playback->frameIndex();
	m_gotFrame = false;
	for (std::size_t attempts = 0; attempts < 256 && !m_playback->isTerminal(); ++attempts) {
		if (!m_playback->pump(32)) {
			if (!m_playback->isTerminal()) {
				m_good = false;
			}
			break;
		}
		if (m_playback->hasCurrentFrame() && m_playback->frameIndex() != previousFrame) {
			m_gotFrame = true;
			break;
		}
	}
	if (!m_gotFrame && m_playback->isTerminal()) {
		m_gotFrame = m_playback->state() == FFmpegMoviePlaybackState::ENDED
			&& m_playback->hasCurrentFrame();
		if (m_playback->state() == FFmpegMoviePlaybackState::FAILED) {
			m_good = false;
		}
	}
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
	const Int frame_count = m_ffmpegFile->getNumFrames();
	if (frame_count > 0) {
		index = std::clamp(index, 0, frame_count - 1);
	} else if (index < 0) {
		index = 0;
	}

	if (m_playback == nullptr || !m_playback->seekFrame(index)) {
		av_frame_free(&m_frame);
		m_gotFrame = false;
		m_good = false;
		return FALSE;
	}

	av_frame_free(&m_frame);
	m_gotFrame = false;
	m_good = true;
	for (std::size_t attempts = 0; attempts < 256 && m_good && !m_gotFrame; ++attempts) {
		const Int targetFrame = index;
		if (!m_playback->pump(32)) {
			m_good = m_playback->state() == FFmpegMoviePlaybackState::ENDED;
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


