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

//////// FFmpegFile.h ///////////////////////////
// Stephan Vedder, April 2025
/////////////////////////////////////////////////

#pragma once

#include <Lib/BaseType.h>

#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <vector>

struct AVFormatContext;
struct AVIOContext;
struct AVCodec;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct File;

struct FFmpegFrameMetadata
{
	Int streamIndex = -1;
	Int streamType = -1;
	Int timeBaseNumerator = 0;
	Int timeBaseDenominator = 1;
	std::int64_t presentationTimestamp = (std::numeric_limits<std::int64_t>::min)();
};

struct FFmpegFrameRate
{
	Int numerator = 0;
	Int denominator = 1;
};

// Neutral byte-source seam used by the device-free FFmpeg graph.  The
// production File* overload adapts the game's File contract, while tests can
// provide deterministic bytes without substituting Common/File.h's ABI.
enum class FFmpegFileSeekMode : std::uint8_t
{
	START,
	CURRENT,
	END
};

class FFmpegFileSource
{
public:
	virtual ~FFmpegFileSource() = default;
	virtual Int read(void *buffer, Int bytes) = 0;
	virtual Int64 seek(Int64 offset, FFmpegFileSeekMode mode) = 0;
	virtual Int64 size() const = 0;
	virtual void close() = 0;
};

using FFmpegFrameCallback = std::function<void(const AVFrame *, const FFmpegFrameMetadata &, void *)>;

class FFmpegFile
{
public:
	FFmpegFile();
	// The constructur takes ownership of the file
	explicit FFmpegFile(File *file);
	~FFmpegFile();

	Bool open(File *file);
	// The caller retains source ownership and must keep it alive until this
	// FFmpegFile is closed or destroyed.
	Bool open(FFmpegFileSource &source);
	void close();
	void setFrameCallback(FFmpegFrameCallback callback) { m_frameCallback = callback; }
	void setUserData(void *user_data) { m_userData = user_data; }
	// Read & decode a packet from the container. Note that we could/should split this step
	Bool decodePacket();
	Bool seekFrame(int frame_idx);
	Bool isAtEnd() const { return m_atEnd; }
	Bool hasError() const { return m_decodeError; }
	Bool hasAudio() const;

	// Audio specific
	Int getSizeForSamples(Int numSamples) const;
	Int getNumChannels() const;
	Int getSampleRate() const;
	Int getBytesPerSample() const;

	// Video specific
	Int getWidth() const;
	Int getHeight() const;
	Int getNumFrames() const;
	Int getCurrentFrame() const;
	Int getPixelFormat() const;
	UnsignedInt getFrameTime() const;
	FFmpegFrameRate getVideoFrameRate() const;

private:
	struct FFmpegStream
	{
		AVCodecContext *codec_ctx = nullptr;
		const AVCodec *codec = nullptr;
		Int stream_idx = -1;
		Int stream_type = -1;
		Int time_base_num = 0;
		Int time_base_den = 1;
		AVFrame *frame = nullptr;
		Bool drain_sent = false;
		Bool drained = false;
	};

	enum class ReceiveResult
	{
		NEEDS_INPUT,
		FRAME_SKIPPED,
		FRAME_READY,
		FINISHED,
		FAILED,
	};

	static Int readPacket(void *opaque, UnsignedByte *buf, Int buf_size);
	static Int64 seekPacket(void *opaque, Int64 offset, Int whence);
	ReceiveResult receiveFrame(FFmpegStream &stream);
	const FFmpegStream *findMatch(int type) const;

	FFmpegFrameCallback 		m_frameCallback = nullptr; ///< Callback for frame processing
	AVFormatContext 			*m_fmtCtx = nullptr; ///< Format context for AVFormat
	AVIOContext 				*m_avioCtx = nullptr; ///< IO context for AVFormat
	AVPacket 					*m_packet = nullptr; ///< Current packet
	std::vector<FFmpegStream> 	m_streams; ///< List of streams in the file
	FFmpegFileSource 			*m_source = nullptr;	///< Active neutral byte source
	std::unique_ptr<FFmpegFileSource> m_ownedSource;
	void 						*m_userData = nullptr; ///< User data for the callback
	Int 						m_receiveStreamIndex = -1; ///< Decoder with frames still pending
	size_t 					m_drainStreamIndex = 0; ///< Decoder currently being flushed at EOF
	Int 						m_currentVideoFrame = -1; ///< Zero-based delivered video frame
	Int 						m_videoFramesDelivered = 0; ///< Video frames delivered since open or seek
	Int 						m_discoveredVideoFrameCount = 0; ///< Frame total learned after decoder EOF
	Bool 						m_packetPending = false; ///< Current packet has not been accepted yet
	Bool 						m_inputEnded = false; ///< Demuxer reached end of input
	Bool 						m_atEnd = false; ///< Every selected decoder reached clean EOF
	Bool 						m_decodeError = false; ///< Demux, decode, or seek failed
	Bool 						m_hasSeekTarget = false; ///< Discard frames before the requested timestamp
	Int 						m_seekStreamIndex = -1; ///< Video stream owning the seek target
	Int64 					m_seekTargetTimestamp = 0; ///< Requested timestamp in stream time base
};
