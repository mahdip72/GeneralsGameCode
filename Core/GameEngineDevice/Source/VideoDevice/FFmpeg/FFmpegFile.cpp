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

//////// FFmpegFile.cpp ///////////////////////////
// Stephan Vedder, April 2025
/////////////////////////////////////////////////

#include <Utility/CppMacros.h>

#include "VideoDevice/FFmpeg/FFmpegFile.h"
#include "Common/Debug.h"
#include "Common/File.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#include <cerrno>
#include <climits>
#include <algorithm>
#include <cmath>
#include <limits>

namespace
{
class GameFileSource final : public FFmpegFileSource
{
public:
	explicit GameFileSource(File *file) : m_file(file) {}

	Int read(void *buffer, Int bytes) override
	{
		return m_file == nullptr ? -1 : m_file->read(buffer, bytes);
	}

	Int64 seek(Int64 offset, FFmpegFileSeekMode mode) override
	{
		if (m_file == nullptr || offset < (std::numeric_limits<Int>::min)()
			|| offset > (std::numeric_limits<Int>::max)()) {
			return -1;
		}
		File::seekMode fileMode = File::CURRENT;
		switch (mode) {
			case FFmpegFileSeekMode::START:
				fileMode = File::START;
				break;
			case FFmpegFileSeekMode::CURRENT:
				fileMode = File::CURRENT;
				break;
			case FFmpegFileSeekMode::END:
				fileMode = File::END;
				break;
		}
		return m_file->seek(static_cast<Int>(offset), fileMode);
	}

	Int64 size() const override
	{
		return m_file == nullptr ? -1 : m_file->size();
	}

	void close() override
	{
		if (m_file != nullptr) {
			m_file->close();
			m_file = nullptr;
		}
	}

private:
	File *m_file;
};
}

FFmpegFile::FFmpegFile() {}

FFmpegFile::FFmpegFile(File *file)
{
	open(file);
}

FFmpegFile::~FFmpegFile()
{
	close();
}

Bool FFmpegFile::open(File *file)
{
	DEBUG_ASSERTCRASH(file != nullptr, ("null file pointer"));
	if (file == nullptr || m_source != nullptr || m_fmtCtx != nullptr) {
		return false;
	}
	m_ownedSource = std::make_unique<GameFileSource>(file);
	return open(*m_ownedSource);
}

Bool FFmpegFile::open(FFmpegFileSource &source)
{
	DEBUG_ASSERTCRASH(m_source == nullptr, ("already open"));
	if (m_source != nullptr || m_fmtCtx != nullptr) {
		return false;
	}
#if LOGGING_LEVEL != LOGLEVEL_NONE
	av_log_set_level(AV_LOG_INFO);
#endif

// This is required for FFmpeg older than 4.0 -> deprecated afterwards though
#if LIBAVFORMAT_VERSION_MAJOR < 58
	av_register_all();
#endif

	m_source = &source;
	m_ownsSource = m_ownedSource.get() == &source;

	// FFmpeg setup
	m_fmtCtx = avformat_alloc_context();
	if (!m_fmtCtx) {
		DEBUG_LOG(("Failed to alloc AVFormatContext"));
		close();
		return false;
	}

	constexpr size_t avio_ctx_buffer_size = 0x10000;
	uint8_t *buffer = static_cast<uint8_t *>(av_malloc(avio_ctx_buffer_size));
	if (buffer == nullptr) {
		DEBUG_LOG(("Failed to alloc AVIOContextBuffer"));
		close();
		return false;
	}

	m_avioCtx = avio_alloc_context(buffer, avio_ctx_buffer_size, 0, &source, &readPacket, nullptr, &seekPacket);
	if (m_avioCtx == nullptr) {
		DEBUG_LOG(("Failed to alloc AVIOContext"));
		av_free(buffer);
		close();
		return false;
	}

	m_fmtCtx->pb = m_avioCtx;
	m_fmtCtx->flags |= AVFMT_FLAG_CUSTOM_IO;

	int result = avformat_open_input(&m_fmtCtx, nullptr, nullptr, nullptr);
	if (result < 0) {
		char error_buffer[1024];
		av_strerror(result, error_buffer, sizeof(error_buffer));
		DEBUG_LOG(("Failed 'avformat_open_input': %s", error_buffer));
		close();
		return false;
	}

	result = avformat_find_stream_info(m_fmtCtx, nullptr);
	if (result < 0) {
		char error_buffer[1024];
		av_strerror(result, error_buffer, sizeof(error_buffer));
		DEBUG_LOG(("Failed 'avformat_find_stream_info': %s", error_buffer));
		close();
		return false;
	}

	const int video_stream_idx = av_find_best_stream(m_fmtCtx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
	if (video_stream_idx < 0) {
		DEBUG_LOG(("Input contains no supported video stream"));
		close();
		return false;
	}
	const int audio_stream_idx = av_find_best_stream(m_fmtCtx, AVMEDIA_TYPE_AUDIO, -1, video_stream_idx, nullptr, 0);

	m_streams.resize(m_fmtCtx->nb_streams);
	for (unsigned int stream_idx = 0; stream_idx < m_fmtCtx->nb_streams; stream_idx++) {
		if (static_cast<int>(stream_idx) != video_stream_idx && static_cast<int>(stream_idx) != audio_stream_idx) {
			continue;
		}

		AVStream *av_stream = m_fmtCtx->streams[stream_idx];
		const AVCodec *input_codec = avcodec_find_decoder(av_stream->codecpar->codec_id);
		if (input_codec == nullptr) {
			DEBUG_LOG(("Codec not supported: '%s'", avcodec_get_name(av_stream->codecpar->codec_id)));
			if (static_cast<int>(stream_idx) == video_stream_idx) {
				close();
				return false;
			}
			continue;
		}

		FFmpegStream &output_stream = m_streams[stream_idx];
		const Bool required_video = static_cast<int>(stream_idx) == video_stream_idx;
		output_stream.codec = input_codec;
		output_stream.stream_type = input_codec->type;
		output_stream.stream_idx = stream_idx;
		output_stream.time_base_num = av_stream->time_base.num;
		output_stream.time_base_den = av_stream->time_base.den;

		AVCodecContext *codec_ctx = avcodec_alloc_context3(input_codec);
		if (codec_ctx == nullptr) {
			DEBUG_LOG(("Could not allocate codec context"));
			if (required_video) {
				close();
				return false;
			}
			output_stream = FFmpegStream {};
			continue;
		}
		output_stream.codec_ctx = codec_ctx;

		result = avcodec_parameters_to_context(codec_ctx, av_stream->codecpar);
		if (result < 0) {
			char error_buffer[1024];
			av_strerror(result, error_buffer, sizeof(error_buffer));
			DEBUG_LOG(("Failed 'avcodec_parameters_to_context': %s", error_buffer));
			if (required_video) {
				close();
				return false;
			}
			avcodec_free_context(&output_stream.codec_ctx);
			output_stream = FFmpegStream {};
			continue;
		}
		// Do not turn decoder concealment into a successful movie generation.
		// The playback core must distinguish a corrupt packet from true EOF so a
		// loop cannot repeatedly restart damaged input.
		codec_ctx->err_recognition |= AV_EF_EXPLODE;

		result = avcodec_open2(codec_ctx, input_codec, nullptr);
		if (result < 0) {
			char error_buffer[1024];
			av_strerror(result, error_buffer, sizeof(error_buffer));
			DEBUG_LOG(("Failed 'avcodec_open2': %s", error_buffer));
			if (required_video) {
				close();
				return false;
			}
			avcodec_free_context(&output_stream.codec_ctx);
			output_stream = FFmpegStream {};
			continue;
		}

		output_stream.frame = av_frame_alloc();
		if (output_stream.frame == nullptr) {
			DEBUG_LOG(("Could not allocate decoded frame"));
			if (required_video) {
				close();
				return false;
			}
			avcodec_free_context(&output_stream.codec_ctx);
			output_stream = FFmpegStream {};
			continue;
		}
	}

	m_packet = av_packet_alloc();
	if (m_packet == nullptr) {
		DEBUG_LOG(("Could not allocate demux packet"));
		close();
		return false;
	}

	if (findMatch(AVMEDIA_TYPE_VIDEO) == nullptr) {
		DEBUG_LOG(("Input contains no usable video stream"));
		close();
		return false;
	}

	return true;
}

/**
 * Read an FFmpeg packet from file
 */
int FFmpegFile::readPacket(void *opaque, uint8_t *buf, int buf_size)
{
	FFmpegFileSource *source = static_cast<FFmpegFileSource *>(opaque);
	if (source == nullptr || buf == nullptr || buf_size <= 0) {
		return AVERROR(EINVAL);
	}
	const int read = source->read(buf, buf_size);

	// Streaming protocol requires us to return real errors - when we read less equal 0 we're at EOF
	if (read < 0)
		return AVERROR(EIO);
	if (read == 0)
		return AVERROR_EOF;
	if (read > buf_size)
		return AVERROR(EIO);

	return read;
}

Int64 FFmpegFile::seekPacket(void *opaque, Int64 offset, Int whence)
{
	FFmpegFileSource *source = static_cast<FFmpegFileSource *>(opaque);
	if (source == nullptr) {
		return AVERROR(EINVAL);
	}

	if ((whence & AVSEEK_SIZE) == AVSEEK_SIZE) {
		return source->size();
	}

	whence &= ~AVSEEK_FORCE;
	FFmpegFileSeekMode mode;
	switch (whence) {
		case SEEK_SET:
			mode = FFmpegFileSeekMode::START;
			break;
		case SEEK_CUR:
			mode = FFmpegFileSeekMode::CURRENT;
			break;
		case SEEK_END:
			mode = FFmpegFileSeekMode::END;
			break;
		default:
			return AVERROR(EINVAL);
	}

	const Int64 position = source->seek(offset, mode);
	return position < 0 ? AVERROR(EIO) : position;
}

/**
 * close all the open FFmpeg handles for an open file.
 */
void FFmpegFile::close()
{
	if (m_fmtCtx != nullptr) {
		avformat_close_input(&m_fmtCtx);
	}

	for (auto &stream : m_streams) {
		if (stream.codec_ctx != nullptr) {
			avcodec_free_context(&stream.codec_ctx);
			av_frame_free(&stream.frame);
		}
	}
	m_streams.clear();

	if (m_avioCtx != nullptr) {
		av_freep(&m_avioCtx->buffer);
		avio_context_free(&m_avioCtx);
	}

	if (m_packet != nullptr) {
		av_packet_free(&m_packet);
	}
	m_receiveStreamIndex = -1;
	m_drainStreamIndex = 0;
	m_currentVideoFrame = -1;
	m_videoFramesDelivered = 0;
	m_discoveredVideoFrameCount = 0;
	m_packetPending = false;
	m_inputEnded = false;
	m_atEnd = false;
	m_decodeError = false;
	m_hasSeekTarget = false;
	m_seekStreamIndex = -1;
	m_seekTargetTimestamp = 0;

	if (m_source != nullptr && m_ownsSource) {
		m_source->close();
	}
	m_source = nullptr;
	m_ownsSource = FALSE;
	m_ownedSource.reset();
}

FFmpegDecodeStepResult FFmpegFile::decodeStep()
{
	DEBUG_ASSERTCRASH(m_fmtCtx != nullptr, ("null format context"));
	DEBUG_ASSERTCRASH(m_packet != nullptr, ("null packet pointer"));
	if (m_fmtCtx == nullptr || m_packet == nullptr || m_decodeError) {
		m_decodeError = true;
		return FFmpegDecodeStepResult::FAILED;
	}
	if (m_atEnd) {
		return FFmpegDecodeStepResult::END_OF_INPUT;
	}

	if (m_receiveStreamIndex >= 0) {
		FFmpegStream &stream = m_streams[m_receiveStreamIndex];
		const ReceiveResult receive_result = receiveFrame(stream);
		if (receive_result == ReceiveResult::FRAME_SKIPPED) {
			return FFmpegDecodeStepResult::PROGRESSED;
		}
		if (receive_result == ReceiveResult::FRAME_READY) {
			return FFmpegDecodeStepResult::FRAME_READY;
		}
		if (receive_result == ReceiveResult::FAILED) {
			m_decodeError = true;
			return FFmpegDecodeStepResult::FAILED;
		}
		if (receive_result == ReceiveResult::FINISHED) {
			stream.drained = true;
		}
		if (receive_result == ReceiveResult::NEEDS_INPUT && m_inputEnded && stream.drain_sent) {
			DEBUG_LOG(("Decoder requested input after its drain packet"));
			m_decodeError = true;
			return FFmpegDecodeStepResult::FAILED;
		}
		m_receiveStreamIndex = -1;
		return FFmpegDecodeStepResult::PROGRESSED;
	}

	if (m_packetPending) {
		const Int stream_idx = m_packet->stream_index;
		if (stream_idx < 0 || static_cast<size_t>(stream_idx) >= m_streams.size()) {
			DEBUG_LOG(("Demuxer returned invalid stream index %d", stream_idx));
			av_packet_unref(m_packet);
			m_packetPending = false;
			m_decodeError = true;
			return FFmpegDecodeStepResult::FAILED;
		}

		FFmpegStream &stream = m_streams[stream_idx];
		if (stream.codec_ctx == nullptr) {
			av_packet_unref(m_packet);
			m_packetPending = false;
			return FFmpegDecodeStepResult::PROGRESSED;
		}

		const int result = avcodec_send_packet(stream.codec_ctx, m_packet);
		if (result == AVERROR(EAGAIN)) {
			m_receiveStreamIndex = stream_idx;
			return FFmpegDecodeStepResult::PROGRESSED;
		}
		if (result < 0) {
			char error_buffer[1024];
			av_strerror(result, error_buffer, sizeof(error_buffer));
			DEBUG_LOG(("Failed 'avcodec_send_packet': %s", error_buffer));
			av_packet_unref(m_packet);
			m_packetPending = false;
			m_decodeError = true;
			return FFmpegDecodeStepResult::FAILED;
		}

		av_packet_unref(m_packet);
		m_packetPending = false;
		m_receiveStreamIndex = stream_idx;
		return FFmpegDecodeStepResult::PROGRESSED;
	}

	if (!m_inputEnded) {
		const int result = av_read_frame(m_fmtCtx, m_packet);
		if (result >= 0) {
			m_packetPending = true;
			return FFmpegDecodeStepResult::PROGRESSED;
		}
		if (result == AVERROR(EAGAIN)) {
			DEBUG_LOG(("Local movie demuxer unexpectedly requested more input"));
			m_decodeError = true;
			return FFmpegDecodeStepResult::FAILED;
		}
		if (result != AVERROR_EOF) {
			char error_buffer[1024];
			av_strerror(result, error_buffer, sizeof(error_buffer));
			DEBUG_LOG(("Failed 'av_read_frame': %s", error_buffer));
			m_decodeError = true;
			return FFmpegDecodeStepResult::FAILED;
		}
		m_inputEnded = true;
		return FFmpegDecodeStepResult::PROGRESSED;
	}

	if (m_drainStreamIndex < m_streams.size()) {
		FFmpegStream &stream = m_streams[m_drainStreamIndex];
		if (stream.codec_ctx == nullptr || stream.drained) {
			++m_drainStreamIndex;
			return FFmpegDecodeStepResult::PROGRESSED;
		}

		if (!stream.drain_sent) {
			const int result = avcodec_send_packet(stream.codec_ctx, nullptr);
			if (result == AVERROR(EAGAIN)) {
				m_receiveStreamIndex = static_cast<Int>(m_drainStreamIndex);
				return FFmpegDecodeStepResult::PROGRESSED;
			}
			if (result == AVERROR_EOF) {
				stream.drained = true;
				++m_drainStreamIndex;
				return FFmpegDecodeStepResult::PROGRESSED;
			}
			if (result < 0) {
				char error_buffer[1024];
				av_strerror(result, error_buffer, sizeof(error_buffer));
				DEBUG_LOG(("Failed to drain decoder: %s", error_buffer));
				m_decodeError = true;
				return FFmpegDecodeStepResult::FAILED;
			}
			stream.drain_sent = true;
			return FFmpegDecodeStepResult::PROGRESSED;
		}

		m_receiveStreamIndex = static_cast<Int>(m_drainStreamIndex);
		return FFmpegDecodeStepResult::PROGRESSED;
	}

	if (m_videoFramesDelivered > 0) {
		m_discoveredVideoFrameCount = std::max(m_discoveredVideoFrameCount, m_currentVideoFrame + 1);
	}
	m_atEnd = true;
	return FFmpegDecodeStepResult::END_OF_INPUT;
}

Bool FFmpegFile::decodePacket()
{
	for (;;) {
		switch (decodeStep()) {
			case FFmpegDecodeStepResult::PROGRESSED:
				continue;
			case FFmpegDecodeStepResult::FRAME_READY:
				return true;
			case FFmpegDecodeStepResult::END_OF_INPUT:
			case FFmpegDecodeStepResult::FAILED:
				return false;
		}
	}
}

FFmpegFile::ReceiveResult FFmpegFile::receiveFrame(FFmpegStream &stream)
{
	if (stream.codec_ctx == nullptr || stream.frame == nullptr) {
		DEBUG_LOG(("Decoder frame storage is unavailable"));
		return ReceiveResult::FAILED;
	}
	const int result = avcodec_receive_frame(stream.codec_ctx, stream.frame);
	if (result == AVERROR(EAGAIN)) {
		return ReceiveResult::NEEDS_INPUT;
	}
	if (result == AVERROR_EOF) {
		return ReceiveResult::FINISHED;
	}
	if (result < 0) {
		char error_buffer[1024];
		av_strerror(result, error_buffer, sizeof(error_buffer));
		DEBUG_LOG(("Failed 'avcodec_receive_frame': %s", error_buffer));
		return ReceiveResult::FAILED;
	}

	if (stream.stream_type == AVMEDIA_TYPE_VIDEO && m_hasSeekTarget && stream.stream_idx == m_seekStreamIndex) {
		Int64 timestamp = stream.frame->best_effort_timestamp;
		if (timestamp == AV_NOPTS_VALUE) {
			timestamp = stream.frame->pts;
		}
		if (timestamp == AV_NOPTS_VALUE) {
			timestamp = stream.frame->pkt_dts;
		}
		if (timestamp == AV_NOPTS_VALUE) {
			DEBUG_LOG(("Decoded frame has no timestamp while seeking"));
			av_frame_unref(stream.frame);
			return ReceiveResult::FAILED;
		}
		if (timestamp < m_seekTargetTimestamp) {
			av_frame_unref(stream.frame);
			return ReceiveResult::FRAME_SKIPPED;
		}
		m_hasSeekTarget = false;
	}

	if (stream.stream_type == AVMEDIA_TYPE_VIDEO) {
		++m_currentVideoFrame;
		++m_videoFramesDelivered;
	}
	if (m_frameCallback != nullptr) {
		FFmpegFrameMetadata metadata;
		metadata.streamIndex = stream.stream_idx;
		metadata.streamType = stream.stream_type;
		metadata.timeBaseNumerator = stream.time_base_num;
		metadata.timeBaseDenominator = stream.time_base_den;
		metadata.presentationTimestamp = stream.frame->best_effort_timestamp;
		if (metadata.presentationTimestamp == AV_NOPTS_VALUE) {
			metadata.presentationTimestamp = stream.frame->pts;
		}
		if (metadata.presentationTimestamp == AV_NOPTS_VALUE) {
			metadata.presentationTimestamp = stream.frame->pkt_dts;
		}
		m_frameCallback(stream.frame, metadata, m_userData);
	}
	av_frame_unref(stream.frame);
	return ReceiveResult::FRAME_READY;
}

Bool FFmpegFile::seekFrame(int frame_idx)
{
	const FFmpegStream *video_stream = findMatch(AVMEDIA_TYPE_VIDEO);
	if (m_fmtCtx == nullptr || video_stream == nullptr || frame_idx < 0) {
		m_decodeError = true;
		return false;
	}

	AVStream *stream = m_fmtCtx->streams[video_stream->stream_idx];
	if (stream->avg_frame_rate.num <= 0 || stream->avg_frame_rate.den <= 0) {
		DEBUG_LOG(("Cannot seek video with an invalid average frame rate"));
		m_decodeError = true;
		return false;
	}

	const Int64 relative_timestamp = av_rescale_q(frame_idx, av_inv_q(stream->avg_frame_rate), stream->time_base);
	const Int64 origin_timestamp = getVideoStartTimestamp();
	Int64 timestamp = relative_timestamp;
	if (origin_timestamp != AV_NOPTS_VALUE) {
		if ((relative_timestamp > 0
				&& origin_timestamp > (std::numeric_limits<Int64>::max)() - relative_timestamp)
			|| (relative_timestamp < 0
				&& origin_timestamp < (std::numeric_limits<Int64>::min)() - relative_timestamp)) {
			DEBUG_LOG(("Video seek timestamp overflow"));
			m_decodeError = true;
			return false;
		}
		timestamp = origin_timestamp + relative_timestamp;
	}
	const int result = av_seek_frame(m_fmtCtx, video_stream->stream_idx, timestamp, AVSEEK_FLAG_BACKWARD);
	if (result < 0) {
		char error_buffer[1024];
		av_strerror(result, error_buffer, sizeof(error_buffer));
		DEBUG_LOG(("Failed 'av_seek_frame': %s", error_buffer));
		m_decodeError = true;
		return false;
	}
	const int flush_result = avformat_flush(m_fmtCtx);
	if (flush_result < 0) {
		char error_buffer[1024];
		av_strerror(flush_result, error_buffer, sizeof(error_buffer));
		DEBUG_LOG(("Failed 'avformat_flush': %s", error_buffer));
		m_decodeError = true;
		return false;
	}

	for (auto &decoder_stream : m_streams) {
		if (decoder_stream.codec_ctx != nullptr) {
			avcodec_flush_buffers(decoder_stream.codec_ctx);
			decoder_stream.drain_sent = false;
			decoder_stream.drained = false;
		}
	}
	av_packet_unref(m_packet);
	m_receiveStreamIndex = -1;
	m_drainStreamIndex = 0;
	m_currentVideoFrame = frame_idx - 1;
	m_videoFramesDelivered = 0;
	m_packetPending = false;
	m_inputEnded = false;
	m_atEnd = false;
	m_decodeError = false;
	m_hasSeekTarget = true;
	m_seekStreamIndex = video_stream->stream_idx;
	m_seekTargetTimestamp = timestamp;
	return true;
}

Bool FFmpegFile::hasAudio() const
{
	const FFmpegStream *stream = findMatch(AVMEDIA_TYPE_AUDIO);
	return stream != nullptr;
}

const FFmpegFile::FFmpegStream *FFmpegFile::findMatch(Int type) const
{
	for (auto &stream : m_streams) {
		if (stream.stream_type == type)
			return &stream;
	}

	return nullptr;
}

Int64 FFmpegFile::getVideoStartTimestamp() const
{
	const FFmpegStream *video_stream = findMatch(AVMEDIA_TYPE_VIDEO);
	if (m_fmtCtx == nullptr || video_stream == nullptr
		|| video_stream->stream_idx < 0
		|| static_cast<unsigned int>(video_stream->stream_idx) >= m_fmtCtx->nb_streams) {
		return AV_NOPTS_VALUE;
	}

	const AVStream *stream = m_fmtCtx->streams[video_stream->stream_idx];
	return stream == nullptr ? AV_NOPTS_VALUE : stream->start_time;
}

Int FFmpegFile::getNumChannels() const
{
	const FFmpegStream *stream = findMatch(AVMEDIA_TYPE_AUDIO);
	if (stream == nullptr)
		return 0;

	return stream->codec_ctx->ch_layout.nb_channels;
}

Int FFmpegFile::getSampleRate() const
{
	const FFmpegStream *stream = findMatch(AVMEDIA_TYPE_AUDIO);
	if (stream == nullptr)
		return 0;

	return stream->codec_ctx->sample_rate;
}

Int FFmpegFile::getBytesPerSample() const
{
	const FFmpegStream *stream = findMatch(AVMEDIA_TYPE_AUDIO);
	if (stream == nullptr)
		return 0;

	return av_get_bytes_per_sample(stream->codec_ctx->sample_fmt);
}

Int FFmpegFile::getSizeForSamples(Int numSamples) const
{
	const FFmpegStream *stream = findMatch(AVMEDIA_TYPE_AUDIO);
	if (stream == nullptr)
		return 0;

	return av_samples_get_buffer_size(nullptr, stream->codec_ctx->ch_layout.nb_channels, numSamples, stream->codec_ctx->sample_fmt, 1);
}

Int FFmpegFile::getHeight() const
{
	const FFmpegStream *stream = findMatch(AVMEDIA_TYPE_VIDEO);
	if (stream == nullptr)
		return 0;

	return stream->codec_ctx->height;
}

Int FFmpegFile::getWidth() const
{
	const FFmpegStream *stream = findMatch(AVMEDIA_TYPE_VIDEO);
	if (stream == nullptr)
		return 0;

	return stream->codec_ctx->width;
}

Int FFmpegFile::getNumFrames() const
{
	const FFmpegStream *stream = findMatch(AVMEDIA_TYPE_VIDEO);
	if (m_fmtCtx == nullptr || stream == nullptr || m_fmtCtx->streams[stream->stream_idx] == nullptr)
		return 0;

	if (m_discoveredVideoFrameCount > 0) {
		return m_discoveredVideoFrameCount;
	}

	const AVStream *av_stream = m_fmtCtx->streams[stream->stream_idx];
	if (av_stream->nb_frames > 0) {
		return static_cast<Int>(std::min<Int64>(av_stream->nb_frames, std::numeric_limits<Int>::max()));
	}

	const AVRational frame_rate = av_stream->avg_frame_rate;
	if (frame_rate.num <= 0 || frame_rate.den <= 0) {
		return 0;
	}

	double duration_seconds = 0.0;
	if (av_stream->duration != AV_NOPTS_VALUE) {
		duration_seconds = av_stream->duration * av_q2d(av_stream->time_base);
	} else if (m_fmtCtx->duration != AV_NOPTS_VALUE) {
		duration_seconds = m_fmtCtx->duration / static_cast<double>(AV_TIME_BASE);
	}
	if (!std::isfinite(duration_seconds) || duration_seconds <= 0.0) {
		return 0;
	}

	const double frame_count = duration_seconds * av_q2d(frame_rate);
	const double bounded_frame_count = std::clamp(
		frame_count, 0.0, static_cast<double>(std::numeric_limits<Int>::max()));
	return static_cast<Int>(std::llround(bounded_frame_count));
}

Int FFmpegFile::getCurrentFrame() const
{
	const FFmpegStream *stream = findMatch(AVMEDIA_TYPE_VIDEO);
	if (stream == nullptr)
		return 0;
	return m_currentVideoFrame < 0 ? 0 : m_currentVideoFrame;
}

Int FFmpegFile::getPixelFormat() const
{
	const FFmpegStream *stream = findMatch(AVMEDIA_TYPE_VIDEO);
	if (stream == nullptr)
		return AV_PIX_FMT_NONE;

	return stream->codec_ctx->pix_fmt;
}

UnsignedInt FFmpegFile::getFrameTime() const
{
	const FFmpegStream *stream = findMatch(AVMEDIA_TYPE_VIDEO);
	if (m_fmtCtx == nullptr || stream == nullptr || m_fmtCtx->streams[stream->stream_idx] == nullptr)
		return 0u;
	const AVRational frame_rate = m_fmtCtx->streams[stream->stream_idx]->avg_frame_rate;
	if (frame_rate.num <= 0 || frame_rate.den <= 0) {
		return 0u;
	}

	const double frame_time = 1000.0 / av_q2d(frame_rate);
	if (!std::isfinite(frame_time) || frame_time <= 0.0) {
		return 0u;
	}
	return static_cast<UnsignedInt>(std::clamp(
		std::round(frame_time), 1.0, static_cast<double>(std::numeric_limits<UnsignedInt>::max())));
}

FFmpegFrameRate FFmpegFile::getVideoFrameRate() const
{
	const FFmpegStream *stream = findMatch(AVMEDIA_TYPE_VIDEO);
	if (m_fmtCtx == nullptr || stream == nullptr || m_fmtCtx->streams[stream->stream_idx] == nullptr) {
		return {};
	}
	const AVRational frame_rate = m_fmtCtx->streams[stream->stream_idx]->avg_frame_rate;
	return { frame_rate.num, frame_rate.den };
}

Int64 FFmpegFile::getVideoStartTimeMicroseconds() const
{
	const FFmpegStream *video_stream = findMatch(AVMEDIA_TYPE_VIDEO);
	if (m_fmtCtx == nullptr || video_stream == nullptr
		|| video_stream->stream_idx < 0
		|| static_cast<unsigned int>(video_stream->stream_idx) >= m_fmtCtx->nb_streams) {
		return 0;
	}

	const AVStream *stream = m_fmtCtx->streams[video_stream->stream_idx];
	const Int64 timestamp = getVideoStartTimestamp();
	if (stream == nullptr || timestamp == AV_NOPTS_VALUE
		|| stream->time_base.num <= 0 || stream->time_base.den <= 0) {
		return 0;
	}
	return av_rescale_q(timestamp, stream->time_base, AVRational { 1, AV_TIME_BASE });
}
