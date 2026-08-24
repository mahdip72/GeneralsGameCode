#include "AudioDevice/AudioAssetSource.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>

#if defined(RTS_HAS_FFMPEG)
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/mathematics.h>
}

#include "VideoDevice/FFmpeg/FFmpegAudioDecoder.h"
#endif

namespace
{
constexpr UnsignedInt OUTPUT_SAMPLE_RATE = 48000U;
constexpr UnsignedShort OUTPUT_CHANNELS = 2U;
constexpr std::size_t OUTPUT_BYTES_PER_FRAME = OUTPUT_CHANNELS * sizeof(std::int16_t);
constexpr UnsignedInt MAX_OUTPUT_FRAMES = OUTPUT_SAMPLE_RATE;

struct WaveInfo
{
	std::uint64_t dataOffset = 0;
	std::uint64_t dataBytes = 0;
	UnsignedInt sampleRate = 0;
	UnsignedShort channels = 0;
	UnsignedShort bitsPerSample = 0;
};

bool readExact(std::ifstream &input, char *buffer, std::streamsize size)
{
	input.read(buffer, size);
	return input.good() && input.gcount() == size;
}

UnsignedShort readU16(const char *bytes)
{
	return static_cast<UnsignedShort>(static_cast<unsigned char>(bytes[0]))
		| static_cast<UnsignedShort>(static_cast<unsigned char>(bytes[1]) << 8U);
}

UnsignedInt readU32(const char *bytes)
{
	return static_cast<UnsignedInt>(static_cast<unsigned char>(bytes[0]))
		| (static_cast<UnsignedInt>(static_cast<unsigned char>(bytes[1])) << 8U)
		| (static_cast<UnsignedInt>(static_cast<unsigned char>(bytes[2])) << 16U)
		| (static_cast<UnsignedInt>(static_cast<unsigned char>(bytes[3])) << 24U);
}

bool readWaveInfo(const std::string &path, WaveInfo &info)
{
	info = {};
	std::ifstream input(path, std::ios::binary);
	if (!input.good()) {
		return false;
	}
	input.seekg(0, std::ios::end);
	const std::streamoff fileSize = input.tellg();
	if (fileSize < 12) {
		return false;
	}
	input.seekg(0, std::ios::beg);
	char header[12] = {};
	if (!readExact(input, header, sizeof(header))
		|| std::memcmp(header, "RIFF", 4) != 0
		|| std::memcmp(header + 8, "WAVE", 4) != 0) {
		return false;
	}

	bool hasFormat = false;
	bool hasData = false;
	while (input && static_cast<std::streamoff>(input.tellg()) >= 0
		&& static_cast<std::streamoff>(input.tellg()) + 8 <= fileSize) {
		char chunkHeader[8] = {};
		if (!readExact(input, chunkHeader, sizeof(chunkHeader))) {
			return false;
		}
		const UnsignedInt chunkBytes = readU32(chunkHeader + 4);
		const std::streamoff chunkOffset = input.tellg();
		if (chunkOffset < 0
			|| static_cast<std::uint64_t>(chunkOffset) > static_cast<std::uint64_t>(fileSize)
			|| static_cast<std::uint64_t>(chunkBytes)
			> static_cast<std::uint64_t>(fileSize - chunkOffset)) {
			return false;
		}
		if (std::memcmp(chunkHeader, "fmt ", 4) == 0) {
			if (chunkBytes < 16U) {
				return false;
			}
			char format[16] = {};
			if (!readExact(input, format, sizeof(format))) {
				return false;
			}
			if (readU16(format) != 1U || readU16(format + 2) == 0U
				|| readU32(format + 4) == 0U || readU16(format + 14) != 16U) {
				return false;
			}
			info.channels = readU16(format + 2);
			info.sampleRate = readU32(format + 4);
			info.bitsPerSample = readU16(format + 14);
			hasFormat = TRUE;
			input.seekg(chunkOffset + static_cast<std::streamoff>(chunkBytes), std::ios::beg);
		} else if (std::memcmp(chunkHeader, "data", 4) == 0) {
			info.dataOffset = static_cast<std::uint64_t>(chunkOffset);
			info.dataBytes = chunkBytes;
			hasData = TRUE;
			input.seekg(chunkOffset + static_cast<std::streamoff>(chunkBytes), std::ios::beg);
		} else {
			input.seekg(chunkOffset + static_cast<std::streamoff>(chunkBytes), std::ios::beg);
		}
		if ((chunkBytes & 1U) != 0U) {
			input.seekg(1, std::ios::cur);
		}
	}

	if (!hasFormat || !hasData || info.channels == 0 || info.channels > 2
		|| info.sampleRate == 0 || info.bitsPerSample != 16U) {
		return false;
	}
	const std::uint64_t bytesPerFrame = static_cast<std::uint64_t>(info.channels) * sizeof(std::int16_t);
	return bytesPerFrame != 0 && info.dataBytes >= bytesPerFrame
		&& info.dataBytes / bytesPerFrame <= std::numeric_limits<std::uint64_t>::max();
}

bool readWaveInfo(const std::vector<std::uint8_t> &bytes, WaveInfo &info)
{
	info = {};
	if (bytes.size() < 12U || std::memcmp(bytes.data(), "RIFF", 4) != 0
		|| std::memcmp(bytes.data() + 8U, "WAVE", 4) != 0) {
		return false;
	}
	bool hasFormat = false;
	bool hasData = false;
	std::size_t offset = 12U;
	while (offset + 8U <= bytes.size()) {
		const std::uint8_t *chunkHeader = bytes.data() + offset;
		const std::uint64_t chunkBytes = readU32(reinterpret_cast<const char *>(chunkHeader + 4U));
		const std::size_t chunkOffset = offset + 8U;
		if (chunkBytes > bytes.size() - chunkOffset) {
			return false;
		}
		if (std::memcmp(chunkHeader, "fmt ", 4) == 0) {
			if (chunkBytes < 16U) {
				return false;
			}
			const char *format = reinterpret_cast<const char *>(bytes.data() + chunkOffset);
			if (readU16(format) != 1U || readU16(format + 2) == 0U
				|| readU32(format + 4) == 0U || readU16(format + 14) != 16U) {
				return false;
			}
			info.channels = readU16(format + 2);
			info.sampleRate = readU32(format + 4);
			info.bitsPerSample = readU16(format + 14);
			hasFormat = true;
		} else if (std::memcmp(chunkHeader, "data", 4) == 0) {
			info.dataOffset = chunkOffset;
			info.dataBytes = chunkBytes;
			hasData = true;
		}
		const std::size_t paddedBytes = static_cast<std::size_t>(chunkBytes + (chunkBytes & 1U));
		if (paddedBytes > bytes.size() - chunkOffset) {
			return false;
		}
		offset = chunkOffset + paddedBytes;
	}

	if (!hasFormat || !hasData || info.channels == 0 || info.channels > 2
		|| info.sampleRate == 0 || info.bitsPerSample != 16U) {
		return false;
	}
	const std::uint64_t bytesPerFrame = static_cast<std::uint64_t>(info.channels) * sizeof(std::int16_t);
	return bytesPerFrame != 0 && info.dataOffset <= bytes.size()
		&& info.dataBytes <= bytes.size() - info.dataOffset
		&& info.dataBytes >= bytesPerFrame;
}

bool decodeWaveBuffer(const std::vector<std::uint8_t> &bytes, AudioPcmChunk &chunk,
	UnsignedInt maxFrames, UnsignedInt startFrame)
{
	chunk = {};
	if (maxFrames == 0 || maxFrames > MAX_OUTPUT_FRAMES) {
		return false;
	}
	WaveInfo info;
	if (!readWaveInfo(bytes, info)) {
		return false;
	}
	const std::uint64_t sourceBytesPerFrame =
		static_cast<std::uint64_t>(info.channels) * sizeof(std::int16_t);
	const std::uint64_t sourceFrames = info.dataBytes / sourceBytesPerFrame;
	const std::uint64_t targetFrames = (sourceFrames * OUTPUT_SAMPLE_RATE
		+ info.sampleRate / 2U) / info.sampleRate;
	if (targetFrames == 0 || startFrame >= targetFrames) {
		return false;
	}
	const std::uint64_t requestedFrames = std::min<std::uint64_t>(maxFrames, targetFrames - startFrame);
	const std::uint64_t firstSourceFrame =
		(static_cast<std::uint64_t>(startFrame) * info.sampleRate) / OUTPUT_SAMPLE_RATE;
	const std::uint64_t lastTargetFrame = static_cast<std::uint64_t>(startFrame) + requestedFrames - 1U;
	const std::uint64_t lastSourceFrame =
		(lastTargetFrame * info.sampleRate) / OUTPUT_SAMPLE_RATE;
	const std::uint64_t sourceFrameCount = lastSourceFrame - firstSourceFrame + 1U;
	const std::uint64_t sourceBytes = sourceFrameCount * sourceBytesPerFrame;
	if (sourceBytes > 4U * 1024U * 1024U
		|| firstSourceFrame > sourceFrames || sourceFrameCount > sourceFrames - firstSourceFrame) {
		return false;
	}
	const std::uint64_t sourceOffset = info.dataOffset + firstSourceFrame * sourceBytesPerFrame;
	if (sourceOffset > bytes.size() || sourceBytes > bytes.size() - sourceOffset) {
		return false;
	}

	chunk.sampleRate = OUTPUT_SAMPLE_RATE;
	chunk.channels = OUTPUT_CHANNELS;
	chunk.format = AudioPcmFormat::SIGNED_16_INTERLEAVED_LITTLE_ENDIAN;
	chunk.startSample = startFrame;
	chunk.frameCount = static_cast<UnsignedInt>(requestedFrames);
	chunk.data.resize(static_cast<std::size_t>(requestedFrames) * OUTPUT_BYTES_PER_FRAME);
	for (std::uint64_t target = 0; target < requestedFrames; ++target) {
		const std::uint64_t absoluteTarget = static_cast<std::uint64_t>(startFrame) + target;
		const std::uint64_t sourceFrame =
			(absoluteTarget * info.sampleRate) / OUTPUT_SAMPLE_RATE - firstSourceFrame;
		const std::size_t sourceOffsetInBuffer = static_cast<std::size_t>(sourceFrame * sourceBytesPerFrame);
		const std::int16_t left = static_cast<std::int16_t>(
			static_cast<std::uint16_t>(bytes[static_cast<std::size_t>(sourceOffset) + sourceOffsetInBuffer])
			| (static_cast<std::uint16_t>(bytes[static_cast<std::size_t>(sourceOffset) + sourceOffsetInBuffer + 1U]) << 8U));
		const std::int16_t right = info.channels == 1
			? left
			: static_cast<std::int16_t>(
				static_cast<std::uint16_t>(bytes[static_cast<std::size_t>(sourceOffset) + sourceOffsetInBuffer + 2U])
				| (static_cast<std::uint16_t>(bytes[static_cast<std::size_t>(sourceOffset) + sourceOffsetInBuffer + 3U]) << 8U));
		const std::size_t outputOffset = static_cast<std::size_t>(target) * OUTPUT_BYTES_PER_FRAME;
		chunk.data[outputOffset] = static_cast<std::uint8_t>(left & 0xff);
		chunk.data[outputOffset + 1U] = static_cast<std::uint8_t>((left >> 8) & 0xff);
		chunk.data[outputOffset + 2U] = static_cast<std::uint8_t>(right & 0xff);
		chunk.data[outputOffset + 3U] = static_cast<std::uint8_t>((right >> 8) & 0xff);
	}
	return TRUE;
}

bool decodeWave(const std::string &path, AudioPcmChunk &chunk,
	UnsignedInt maxFrames, UnsignedInt startFrame)
{
	chunk = {};
	if (maxFrames == 0 || maxFrames > MAX_OUTPUT_FRAMES) {
		return false;
	}
	WaveInfo info;
	if (!readWaveInfo(path, info)) {
		return false;
	}
	const std::uint64_t sourceBytesPerFrame =
		static_cast<std::uint64_t>(info.channels) * sizeof(std::int16_t);
	const std::uint64_t sourceFrames = info.dataBytes / sourceBytesPerFrame;
	const std::uint64_t targetFrames = (sourceFrames * OUTPUT_SAMPLE_RATE
		+ info.sampleRate / 2U) / info.sampleRate;
	if (targetFrames == 0 || startFrame >= targetFrames) {
		return false;
	}
	const std::uint64_t requestedFrames = std::min<std::uint64_t>(maxFrames, targetFrames - startFrame);
	const std::uint64_t firstSourceFrame =
		(static_cast<std::uint64_t>(startFrame) * info.sampleRate) / OUTPUT_SAMPLE_RATE;
	const std::uint64_t lastTargetFrame = static_cast<std::uint64_t>(startFrame) + requestedFrames - 1U;
	const std::uint64_t lastSourceFrame =
		(lastTargetFrame * info.sampleRate) / OUTPUT_SAMPLE_RATE;
	const std::uint64_t sourceFrameCount = lastSourceFrame - firstSourceFrame + 1U;
	const std::uint64_t sourceBytes = sourceFrameCount * sourceBytesPerFrame;
	if (sourceBytes > 4U * 1024U * 1024U
		|| firstSourceFrame > sourceFrames || sourceFrameCount > sourceFrames - firstSourceFrame) {
		return false;
	}

	std::ifstream input(path, std::ios::binary);
	if (!input.good()) {
		return false;
	}
	const std::uint64_t sourceOffset = info.dataOffset + firstSourceFrame * sourceBytesPerFrame;
	if (sourceOffset > std::numeric_limits<std::streamoff>::max()) {
		return false;
	}
	input.seekg(static_cast<std::streamoff>(sourceOffset), std::ios::beg);
	std::vector<std::uint8_t> source(static_cast<std::size_t>(sourceBytes));
	input.read(reinterpret_cast<char *>(source.data()), static_cast<std::streamsize>(source.size()));
	if (!input.good() && input.gcount() != static_cast<std::streamsize>(source.size())) {
		return false;
	}

	chunk.sampleRate = OUTPUT_SAMPLE_RATE;
	chunk.channels = OUTPUT_CHANNELS;
	chunk.format = AudioPcmFormat::SIGNED_16_INTERLEAVED_LITTLE_ENDIAN;
	chunk.startSample = startFrame;
	chunk.frameCount = static_cast<UnsignedInt>(requestedFrames);
	chunk.data.resize(static_cast<std::size_t>(requestedFrames) * OUTPUT_BYTES_PER_FRAME);
	for (std::uint64_t target = 0; target < requestedFrames; ++target) {
		const std::uint64_t absoluteTarget = static_cast<std::uint64_t>(startFrame) + target;
		const std::uint64_t sourceFrame =
			(absoluteTarget * info.sampleRate) / OUTPUT_SAMPLE_RATE - firstSourceFrame;
		const std::size_t sourceOffsetInBuffer =
			static_cast<std::size_t>(sourceFrame * sourceBytesPerFrame);
		const std::int16_t left = static_cast<std::int16_t>(
			static_cast<std::uint16_t>(source[sourceOffsetInBuffer])
			| (static_cast<std::uint16_t>(source[sourceOffsetInBuffer + 1U]) << 8U));
		const std::int16_t right = info.channels == 1
			? left
			: static_cast<std::int16_t>(
				static_cast<std::uint16_t>(source[sourceOffsetInBuffer + 2U])
				| (static_cast<std::uint16_t>(source[sourceOffsetInBuffer + 3U]) << 8U));
		const std::size_t outputOffset = static_cast<std::size_t>(target) * OUTPUT_BYTES_PER_FRAME;
		chunk.data[outputOffset] = static_cast<std::uint8_t>(left & 0xff);
		chunk.data[outputOffset + 1U] = static_cast<std::uint8_t>((left >> 8) & 0xff);
		chunk.data[outputOffset + 2U] = static_cast<std::uint8_t>(right & 0xff);
		chunk.data[outputOffset + 3U] = static_cast<std::uint8_t>((right >> 8) & 0xff);
	}
	return TRUE;
}

#if defined(RTS_HAS_FFMPEG)
class CapturePcmSink final : public AudioPcmSink
{
public:
	CapturePcmSink(UnsignedInt startFrame, UnsignedInt maxFrames) :
		m_startFrame(startFrame), m_maxFrames(maxFrames),
		m_endFrame(static_cast<std::uint64_t>(startFrame) + maxFrames),
		m_collectedFrames(0), m_done(FALSE)
	{
	}

	AudioPcmSubmitResult submit(AudioPcmChunk &&source) override
	{
		if (m_done || source.channels != OUTPUT_CHANNELS
			|| source.sampleRate != OUTPUT_SAMPLE_RATE
			|| source.frameCount == 0) {
			return AudioPcmSubmitResult::ACCEPTED;
		}
		const std::uint64_t sourceStart = source.startSample < 0
			? 0U : static_cast<std::uint64_t>(source.startSample);
		const std::uint64_t sourceEnd = sourceStart + source.frameCount;
		const std::uint64_t copyStart = std::max<std::uint64_t>(sourceStart, m_startFrame);
		const std::uint64_t copyEnd = std::min<std::uint64_t>(sourceEnd, m_endFrame);
		if (copyEnd > copyStart) {
			const std::uint64_t copyFrames = copyEnd - copyStart;
			if (copyFrames > static_cast<std::uint64_t>(m_maxFrames) - m_collectedFrames) {
				return AudioPcmSubmitResult::FAILED;
			}
			const std::size_t sourceOffset = static_cast<std::size_t>(copyStart - sourceStart) * OUTPUT_BYTES_PER_FRAME;
			const std::size_t copyBytes = static_cast<std::size_t>(copyFrames) * OUTPUT_BYTES_PER_FRAME;
			if (sourceOffset > source.data.size() || copyBytes > source.data.size() - sourceOffset) {
				return AudioPcmSubmitResult::FAILED;
			}
			if (m_chunk.data.empty()) {
				m_chunk.sampleRate = OUTPUT_SAMPLE_RATE;
				m_chunk.channels = OUTPUT_CHANNELS;
				m_chunk.format = AudioPcmFormat::SIGNED_16_INTERLEAVED_LITTLE_ENDIAN;
				m_chunk.startSample = m_startFrame;
			}
			m_chunk.data.insert(m_chunk.data.end(), source.data.begin() + sourceOffset,
				source.data.begin() + sourceOffset + copyBytes);
			m_collectedFrames += static_cast<UnsignedInt>(copyEnd - copyStart);
		}
		if (sourceEnd >= m_endFrame) {
			m_done = TRUE;
		}
		return AudioPcmSubmitResult::ACCEPTED;
	}

	void reset(std::uint64_t) override
	{
		m_chunk = {};
		m_collectedFrames = 0;
		m_done = FALSE;
	}

	Bool isComplete() const { return m_done; }
	Bool hasData() const { return m_collectedFrames != 0; }
	AudioPcmChunk take()
	{
		m_chunk.frameCount = m_collectedFrames;
		return std::move(m_chunk);
	}

	private:
	UnsignedInt m_startFrame;
	UnsignedInt m_maxFrames;
	std::uint64_t m_endFrame;
	UnsignedInt m_collectedFrames;
	Bool m_done;
	AudioPcmChunk m_chunk;
};

struct MemoryFFmpegInput
{
	const std::vector<std::uint8_t> *bytes = nullptr;
	std::size_t position = 0;
};

int readMemoryFFmpeg(void *opaque, std::uint8_t *buffer, int bufferSize)
{
	MemoryFFmpegInput *input = static_cast<MemoryFFmpegInput *>(opaque);
	if (input == nullptr || input->bytes == nullptr || buffer == nullptr || bufferSize <= 0
		|| input->position > input->bytes->size()) {
		return AVERROR(EINVAL);
	}
	const std::size_t available = input->bytes->size() - input->position;
	const std::size_t count = std::min<std::size_t>(available,
		static_cast<std::size_t>(bufferSize));
	if (count == 0) {
		return AVERROR_EOF;
	}
	std::memcpy(buffer, input->bytes->data() + input->position, count);
	input->position += count;
	return static_cast<int>(count);
}

int64_t seekMemoryFFmpeg(void *opaque, int64_t offset, int whence)
{
	MemoryFFmpegInput *input = static_cast<MemoryFFmpegInput *>(opaque);
	if (input == nullptr || input->bytes == nullptr) {
		return AVERROR(EINVAL);
	}
	if ((whence & AVSEEK_SIZE) != 0) {
		if (input->bytes->size() > static_cast<std::size_t>((std::numeric_limits<int64_t>::max)())) {
			return AVERROR(EOVERFLOW);
		}
		return static_cast<int64_t>(input->bytes->size());
	}
	if (input->bytes->size() > static_cast<std::size_t>((std::numeric_limits<int64_t>::max)())) {
		return AVERROR(EOVERFLOW);
	}
	whence &= ~AVSEEK_FORCE;
	int64_t base = 0;
	if (whence == SEEK_CUR) {
		base = static_cast<int64_t>(input->position);
	} else if (whence == SEEK_END) {
		base = static_cast<int64_t>(input->bytes->size());
	} else if (whence != SEEK_SET) {
		return AVERROR(EINVAL);
	}
	if ((offset > 0 && base > (std::numeric_limits<int64_t>::max)() - offset)
		|| (offset < 0 && offset < -base)) {
		return AVERROR(EOVERFLOW);
	}
	const int64_t target = base + offset;
	if (target < 0 || target > static_cast<int64_t>(input->bytes->size())) {
		return AVERROR(EIO);
	}
	input->position = static_cast<std::size_t>(target);
	return target;
}

bool openMemoryFFmpeg(const std::vector<std::uint8_t> &bytes,
	MemoryFFmpegInput &input, AVFormatContext *&format, AVIOContext *&avio)
{
	format = nullptr;
	avio = nullptr;
	if (bytes.empty()) {
		return false;
	}
	input.bytes = &bytes;
	input.position = 0;
	format = avformat_alloc_context();
	if (format == nullptr) {
		return false;
	}
	constexpr int IO_BUFFER_SIZE = 64 * 1024;
	std::uint8_t *buffer = static_cast<std::uint8_t *>(av_malloc(IO_BUFFER_SIZE));
	if (buffer == nullptr) {
		avformat_free_context(format);
		format = nullptr;
		return false;
	}
	avio = avio_alloc_context(buffer, IO_BUFFER_SIZE, 0, &input,
		&readMemoryFFmpeg, nullptr, &seekMemoryFFmpeg);
	if (avio == nullptr) {
		av_free(buffer);
		avformat_free_context(format);
		format = nullptr;
		return false;
	}
	format->pb = avio;
	format->flags |= AVFMT_FLAG_CUSTOM_IO;
	if (avformat_open_input(&format, nullptr, nullptr, nullptr) < 0 || format == nullptr) {
		return false;
	}
	return avformat_find_stream_info(format, nullptr) >= 0;
}

void closeMemoryFFmpeg(AVFormatContext *&format, AVIOContext *&avio)
{
	if (format != nullptr) {
		avformat_close_input(&format);
	}
	if (avio != nullptr) {
		av_freep(&avio->buffer);
		avio_context_free(&avio);
	}
}

bool getFFmpegDuration(const std::string &path, Real &durationMS)
{
	AVFormatContext *format = nullptr;
	if (avformat_open_input(&format, path.c_str(), nullptr, nullptr) < 0 || format == nullptr) {
		return false;
	}
	const bool hasInfo = avformat_find_stream_info(format, nullptr) >= 0;
	const int streamIndex = hasInfo
		? av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0) : -1;
	bool success = false;
	if (streamIndex >= 0 && format->streams[streamIndex] != nullptr) {
		const AVStream *stream = format->streams[streamIndex];
		if (stream->duration != AV_NOPTS_VALUE && stream->time_base.den != 0) {
			durationMS = static_cast<Real>(av_rescale_q(stream->duration,
				stream->time_base, AVRational { 1, 1000 }));
			success = durationMS >= 0.0f;
		} else if (format->duration != AV_NOPTS_VALUE) {
			durationMS = static_cast<Real>(format->duration) / 1000.0f;
			success = durationMS >= 0.0f;
		}
	}
	avformat_close_input(&format);
	return success;
}

bool getFFmpegDuration(const std::vector<std::uint8_t> &bytes, Real &durationMS)
{
	MemoryFFmpegInput input;
	AVFormatContext *format = nullptr;
	AVIOContext *avio = nullptr;
	if (!openMemoryFFmpeg(bytes, input, format, avio)) {
		closeMemoryFFmpeg(format, avio);
		return false;
	}
	const int streamIndex = av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
	bool success = false;
	if (streamIndex >= 0 && format->streams[streamIndex] != nullptr) {
		const AVStream *stream = format->streams[streamIndex];
		if (stream->duration != AV_NOPTS_VALUE && stream->time_base.den != 0) {
			durationMS = static_cast<Real>(av_rescale_q(stream->duration,
				stream->time_base, AVRational { 1, 1000 }));
			success = durationMS >= 0.0f;
		} else if (format->duration != AV_NOPTS_VALUE) {
			durationMS = static_cast<Real>(format->duration) / 1000.0f;
			success = durationMS >= 0.0f;
		}
	}
	closeMemoryFFmpeg(format, avio);
	return success;
}

bool decodeWithFFmpeg(const std::string &path, AudioPcmChunk &chunk,
	UnsignedInt maxFrames, UnsignedInt startFrame)
{
	chunk = {};
	if (maxFrames == 0 || maxFrames > MAX_OUTPUT_FRAMES) {
		return false;
	}
	AVFormatContext *format = nullptr;
	AVCodecContext *codecContext = nullptr;
	AVPacket *packet = nullptr;
	AVFrame *frame = nullptr;
	bool success = false;
	if (avformat_open_input(&format, path.c_str(), nullptr, nullptr) < 0 || format == nullptr
		|| avformat_find_stream_info(format, nullptr) < 0) {
		avformat_close_input(&format);
		return false;
	}
	const int streamIndex = av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
	if (streamIndex < 0 || format->streams[streamIndex] == nullptr) {
		avformat_close_input(&format);
		return false;
	}
	const AVCodecParameters *parameters = format->streams[streamIndex]->codecpar;
	const AVCodec *codec = avcodec_find_decoder(parameters->codec_id);
	codecContext = codec == nullptr ? nullptr : avcodec_alloc_context3(codec);
	if (codecContext == nullptr || avcodec_parameters_to_context(codecContext, parameters) < 0
		|| avcodec_open2(codecContext, codec, nullptr) < 0) {
		avcodec_free_context(&codecContext);
		avformat_close_input(&format);
		return false;
	}
	packet = av_packet_alloc();
	frame = av_frame_alloc();
	CapturePcmSink sink(startFrame, maxFrames);
	FFmpegAudioDecoder decoder;
	decoder.reset(1, sink);
	auto receiveFrames = [&]() {
		for (;;) {
			const int result = avcodec_receive_frame(codecContext, frame);
			if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) return true;
			if (result < 0) return false;
			if (!decoder.convert(frame, format->streams[streamIndex]->time_base.num,
				format->streams[streamIndex]->time_base.den, sink)) return false;
			av_frame_unref(frame);
			if (sink.isComplete()) return true;
		}
	};
	while (!sink.isComplete() && packet != nullptr && av_read_frame(format, packet) >= 0) {
		if (packet->stream_index == streamIndex) {
			if (avcodec_send_packet(codecContext, packet) < 0 || !receiveFrames()) {
				av_packet_unref(packet);
				goto cleanup;
			}
		}
		av_packet_unref(packet);
	}
	if (!sink.isComplete()) {
		if (avcodec_send_packet(codecContext, nullptr) < 0 || !receiveFrames()
			|| !decoder.drain(sink)) {
			goto cleanup;
		}
	}
	if (sink.hasData()) {
		chunk = sink.take();
		success = TRUE;
	}

cleanup:
	av_frame_free(&frame);
	av_packet_free(&packet);
	avcodec_free_context(&codecContext);
	avformat_close_input(&format);
	return success;
}

bool decodeWithFFmpeg(const std::vector<std::uint8_t> &bytes, AudioPcmChunk &chunk,
	UnsignedInt maxFrames, UnsignedInt startFrame)
{
	chunk = {};
	if (maxFrames == 0 || maxFrames > MAX_OUTPUT_FRAMES) {
		return false;
	}
	MemoryFFmpegInput input;
	AVFormatContext *format = nullptr;
	AVIOContext *avio = nullptr;
	if (!openMemoryFFmpeg(bytes, input, format, avio)) {
		closeMemoryFFmpeg(format, avio);
		return false;
	}
	AVCodecContext *codecContext = nullptr;
	AVPacket *packet = nullptr;
	AVFrame *frame = nullptr;
	const AVCodecParameters *parameters = nullptr;
	const AVCodec *codec = nullptr;
	bool success = false;
	const int streamIndex = av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
	if (streamIndex < 0 || format->streams[streamIndex] == nullptr) {
		goto cleanup;
	}
	parameters = format->streams[streamIndex]->codecpar;
	codec = avcodec_find_decoder(parameters->codec_id);
	codecContext = codec == nullptr ? nullptr : avcodec_alloc_context3(codec);
	if (codecContext == nullptr || avcodec_parameters_to_context(codecContext, parameters) < 0
		|| avcodec_open2(codecContext, codec, nullptr) < 0) {
		goto cleanup;
	}
	packet = av_packet_alloc();
	frame = av_frame_alloc();
	if (packet == nullptr || frame == nullptr) {
		goto cleanup;
	}
	{
		CapturePcmSink sink(startFrame, maxFrames);
		FFmpegAudioDecoder decoder;
		decoder.reset(1, sink);
		auto receiveFrames = [&]() {
			for (;;) {
				const int result = avcodec_receive_frame(codecContext, frame);
				if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) return true;
				if (result < 0) return false;
				if (!decoder.convert(frame, format->streams[streamIndex]->time_base.num,
					format->streams[streamIndex]->time_base.den, sink)) return false;
				av_frame_unref(frame);
				if (sink.isComplete()) return true;
			}
		};
		while (!sink.isComplete() && av_read_frame(format, packet) >= 0) {
			if (packet->stream_index == streamIndex) {
				if (avcodec_send_packet(codecContext, packet) < 0 || !receiveFrames()) {
					av_packet_unref(packet);
					goto cleanup;
				}
			}
			av_packet_unref(packet);
		}
		if (!sink.isComplete()) {
			if (avcodec_send_packet(codecContext, nullptr) < 0 || !receiveFrames()
				|| !decoder.drain(sink)) {
				goto cleanup;
			}
		}
		if (sink.hasData()) {
			chunk = sink.take();
			success = TRUE;
		}
	}

cleanup:
	av_frame_free(&frame);
	av_packet_free(&packet);
	avcodec_free_context(&codecContext);
	closeMemoryFFmpeg(format, avio);
	return success;
}
#endif
}

FileAudioAssetSource::FileAudioAssetSource() = default;

FileAudioAssetSource::FileAudioAssetSource(const AsciiString &rootDirectory) :
	m_rootDirectory(rootDirectory.isEmpty() ? std::string() : rootDirectory.str())
{
}

FileAudioAssetSource::FileAudioAssetSource(const AsciiString &rootDirectory,
	AudioVirtualFileSource *virtualSource) :
	m_rootDirectory(rootDirectory.isEmpty() ? std::string() : rootDirectory.str()),
	m_virtualSource(virtualSource)
{
}

Bool FileAudioAssetSource::readVirtualFile(const AsciiString &fileName,
	std::vector<std::uint8_t> &bytes, std::string &identity) const
{
	bytes.clear();
	identity.clear();
	if (m_virtualSource == nullptr || !m_virtualSource->readFile(fileName, bytes, identity)
		|| bytes.empty() || bytes.size() > 64U * 1024U * 1024U || identity.empty()) {
		bytes.clear();
		identity.clear();
		return FALSE;
	}
	return TRUE;
}

std::string FileAudioAssetSource::resolvePath(const AsciiString &fileName) const
{
	try {
		std::filesystem::path path(fileName.str() == nullptr ? "" : fileName.str());
		if (!path.is_absolute() && !m_rootDirectory.empty()) {
			const std::filesystem::path root(m_rootDirectory);
			std::filesystem::path::const_iterator first = path.begin();
			const std::string rootName = root.filename().string();
			const std::string firstName = first == path.end() ? std::string() : first->string();
			const auto equalsIgnoreCase = [](const std::string &left, const std::string &right) {
				if (left.size() != right.size()) {
					return false;
				}
				for (std::size_t index = 0; index < left.size(); ++index) {
					if (std::tolower(static_cast<unsigned char>(left[index]))
						!= std::tolower(static_cast<unsigned char>(right[index]))) {
						return false;
					}
				}
				return true;
			};
			// Audio settings commonly provide `Audio` as the root while generated
			// event names already carry the `Audio\\` virtual prefix.  Joining both
			// would silently turn every production lookup into `Audio\\Audio\\...`.
			if (!rootName.empty() && equalsIgnoreCase(rootName, firstName)) {
				path = root.parent_path() / path;
			} else {
				path = root / path;
			}
		}
		std::error_code error;
		path = std::filesystem::weakly_canonical(path, error);
		if (error) {
			error.clear();
			path = std::filesystem::absolute(path, error);
		}
		return path.lexically_normal().string();
	} catch (...) {
		return std::string();
	}
}

Bool FileAudioAssetSource::getDurationMS(const AsciiString &fileName, Real &durationMS) const
{
	durationMS = 0.0f;
	const std::string path = resolvePath(fileName);
	if (path.empty()) {
		return FALSE;
	}
	std::error_code error;
	const bool looseExists = std::filesystem::exists(path, error) && !error;
	WaveInfo info;
	if (readWaveInfo(path, info)) {
		const std::uint64_t frames = info.dataBytes
			/ (static_cast<std::uint64_t>(info.channels) * sizeof(std::int16_t));
		durationMS = static_cast<Real>(frames) * 1000.0f / static_cast<Real>(info.sampleRate);
		return TRUE;
	}
	if (!looseExists) {
		std::vector<std::uint8_t> bytes;
		std::string identity;
		if (readVirtualFile(fileName, bytes, identity)) {
			if (readWaveInfo(bytes, info)) {
				const std::uint64_t frames = info.dataBytes
					/ (static_cast<std::uint64_t>(info.channels) * sizeof(std::int16_t));
				durationMS = static_cast<Real>(frames) * 1000.0f
					/ static_cast<Real>(info.sampleRate);
				return TRUE;
			}
#if defined(RTS_HAS_FFMPEG)
			if (getFFmpegDuration(bytes, durationMS)) {
				return TRUE;
			}
#endif
		}
	}
#if defined(RTS_HAS_FFMPEG)
	return getFFmpegDuration(path, durationMS) ? TRUE : FALSE;
#else
	return FALSE;
#endif
}

Bool FileAudioAssetSource::decodePcm(const AsciiString &fileName, AudioPcmChunk &chunk,
	UnsignedInt maxFrames) const
{
	return decodePcmAt(fileName, chunk, maxFrames, 0);
}

Bool FileAudioAssetSource::decodePcmAt(const AsciiString &fileName, AudioPcmChunk &chunk,
	UnsignedInt maxFrames, UnsignedInt startFrame) const
{
	const std::string path = resolvePath(fileName);
	if (path.empty()) {
		chunk = {};
		return FALSE;
	}
	std::error_code error;
	const bool looseExists = std::filesystem::exists(path, error) && !error;
	WaveInfo info;
	if (readWaveInfo(path, info)) {
		return decodeWave(path, chunk, maxFrames, startFrame) ? TRUE : FALSE;
	}
	if (!looseExists) {
		std::vector<std::uint8_t> bytes;
		std::string identity;
		if (readVirtualFile(fileName, bytes, identity)) {
			if (decodeWaveBuffer(bytes, chunk, maxFrames, startFrame)) {
				return TRUE;
			}
#if defined(RTS_HAS_FFMPEG)
			if (decodeWithFFmpeg(bytes, chunk, maxFrames, startFrame)) {
				return TRUE;
			}
#endif
		}
	}
#if defined(RTS_HAS_FFMPEG)
	if (decodeWithFFmpeg(path, chunk, maxFrames, startFrame)) {
		return TRUE;
	}
#endif
	return FALSE;
}

const void *FileAudioAssetSource::getFileIdentity(const AsciiString &fileName) const
{
	const std::string path = resolvePath(fileName);
	std::error_code error;
	if (!path.empty() && std::filesystem::exists(path, error) && !error) {
		for (std::string &identity : m_identityPaths) {
			if (identity == path) {
				return &identity;
			}
		}
		m_identityPaths.push_back(path);
		return &m_identityPaths.back();
	}
	std::vector<std::uint8_t> bytes;
	std::string identity;
	if (!readVirtualFile(fileName, bytes, identity)) {
		return nullptr;
	}
	for (std::string &storedIdentity : m_identityPaths) {
		if (storedIdentity == identity) {
			return &storedIdentity;
		}
	}
	m_identityPaths.push_back(identity);
	return &m_identityPaths.back();
}

Bool FileAudioAssetSource::getEventDurationMS(const AsciiString &attackFile,
	const AsciiString &mainFile, const AsciiString &decayFile, Real &durationMS) const
{
	durationMS = 0.0f;
	const AsciiString files[] = { attackFile, mainFile, decayFile };
	for (const AsciiString &fileName : files) {
		if (fileName.isEmpty()) {
			continue;
		}
		Real part = 0.0f;
		if (!getDurationMS(fileName, part)) {
			durationMS = 0.0f;
			return FALSE;
		}
		durationMS += part;
	}
	return TRUE;
}
