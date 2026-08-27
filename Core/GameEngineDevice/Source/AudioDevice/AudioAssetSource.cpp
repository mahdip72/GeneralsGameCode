#include "AudioDevice/AudioAssetSource.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <deque>
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

bool pathComponentEqualsIgnoreCase(const std::filesystem::path &left,
	const std::filesystem::path &right)
{
	const std::string leftText = left.string();
	const std::string rightText = right.string();
	if (leftText.size() != rightText.size()) {
		return false;
	}
	for (std::size_t index = 0; index < leftText.size(); ++index) {
		if (std::tolower(static_cast<unsigned char>(leftText[index]))
			!= std::tolower(static_cast<unsigned char>(rightText[index]))) {
			return false;
		}
	}
	return true;
}

bool isPathWithinRoot(const std::filesystem::path &root,
	const std::filesystem::path &candidate)
{
	auto rootPart = root.begin();
	auto candidatePart = candidate.begin();
	for (; rootPart != root.end(); ++rootPart, ++candidatePart) {
		if (candidatePart == candidate.end()
			|| !pathComponentEqualsIgnoreCase(*rootPart, *candidatePart)) {
			return false;
		}
	}
	return true;
}

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

#if defined(RTS_NATIVE_AUDIO_ASSET_SOURCE_TEST_HOOK)
UnsignedInt g_ffmpegReadFrameFailureAfter =
	(std::numeric_limits<UnsignedInt>::max)();
UnsignedInt g_ffmpegReadFrameSuccesses = 0;
#endif

int readFFmpegFrame(AVFormatContext *format, AVPacket *packet)
{
#if defined(RTS_NATIVE_AUDIO_ASSET_SOURCE_TEST_HOOK)
	if (g_ffmpegReadFrameFailureAfter != (std::numeric_limits<UnsignedInt>::max)()) {
		if (g_ffmpegReadFrameSuccesses >= g_ffmpegReadFrameFailureAfter) {
			return AVERROR(EIO);
		}
		++g_ffmpegReadFrameSuccesses;
	}
#endif
	return av_read_frame(format, packet);
}

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

class SequentialPcmSink final : public AudioPcmSink
{
public:
	static constexpr UnsignedInt MAX_PENDING_FRAMES =
		2U * FFmpegAudioDecoder::MAX_CHUNK_FRAMES;

	AudioPcmSubmitResult submit(AudioPcmChunk &&source) override
	{
		if (source.sampleRate != OUTPUT_SAMPLE_RATE || source.channels != OUTPUT_CHANNELS
			|| source.format != AudioPcmFormat::SIGNED_16_INTERLEAVED_LITTLE_ENDIAN
			|| source.frameCount == 0U
			|| source.frameCount > MAX_PENDING_FRAMES - m_pendingFrames
			|| source.frameCount > (std::numeric_limits<std::size_t>::max)()
			/ OUTPUT_BYTES_PER_FRAME
			|| source.data.size() != static_cast<std::size_t>(source.frameCount)
			* OUTPUT_BYTES_PER_FRAME) {
			return AudioPcmSubmitResult::FAILED;
		}
		m_pendingFrames += source.frameCount;
		m_pending.push_back(std::move(source));
		return AudioPcmSubmitResult::ACCEPTED;
	}

	void reset(std::uint64_t) override
	{
		m_pending.clear();
		m_pendingFrames = 0;
	}

	UnsignedInt pendingFrames() const { return m_pendingFrames; }

	Bool take(UnsignedInt maxFrames, AudioPcmChunk &chunk)
	{
		chunk = {};
		if (maxFrames == 0U) {
			return FALSE;
		}
		while (!m_pending.empty() && chunk.frameCount < maxFrames) {
			AudioPcmChunk &source = m_pending.front();
			const UnsignedInt frames = std::min(maxFrames - chunk.frameCount,
				source.frameCount);
			const std::size_t bytes = static_cast<std::size_t>(frames)
				* OUTPUT_BYTES_PER_FRAME;
			if (chunk.frameCount == 0U) {
				chunk.sampleRate = source.sampleRate;
				chunk.channels = source.channels;
				chunk.format = source.format;
				chunk.startSample = source.startSample;
				chunk.generation = source.generation;
				chunk.sequence = source.sequence;
				chunk.discontinuity = source.discontinuity;
			} else {
				const bool hasExpectedStart = chunk.startSample <=
					(std::numeric_limits<std::int64_t>::max)()
					- static_cast<std::int64_t>(chunk.frameCount);
				if (!hasExpectedStart || source.startSample != chunk.startSample
					+ static_cast<std::int64_t>(chunk.frameCount)) {
					chunk.discontinuity = true;
				}
				chunk.discontinuity = chunk.discontinuity || source.discontinuity;
			}
			chunk.data.insert(chunk.data.end(), source.data.begin(), source.data.begin() + bytes);
			chunk.frameCount += frames;
			m_pendingFrames -= frames;
			if (frames == source.frameCount) {
				m_pending.pop_front();
			} else {
				source.data.erase(source.data.begin(), source.data.begin() + bytes);
				source.frameCount -= frames;
				if (source.startSample <= (std::numeric_limits<std::int64_t>::max)()
					- static_cast<std::int64_t>(frames)) {
					source.startSample += static_cast<std::int64_t>(frames);
				}
				source.discontinuity = false;
			}
		}
		return chunk.frameCount != 0U;
	}

private:
	std::deque<AudioPcmChunk> m_pending;
	UnsignedInt m_pendingFrames = 0;
};

class FFmpegPcmStream final : public AudioPcmStream
{
public:
	FFmpegPcmStream() = default;
	~FFmpegPcmStream() override { close(); }

	FFmpegPcmStream(const FFmpegPcmStream &) = delete;
	FFmpegPcmStream &operator=(const FFmpegPcmStream &) = delete;

	bool openPath(const std::string &path)
	{
		close();
		if (path.empty() || avformat_open_input(&m_format, path.c_str(), nullptr, nullptr) < 0
			|| m_format == nullptr || avformat_find_stream_info(m_format, nullptr) < 0) {
			close();
			return false;
		}
		return initialize();
	}

	bool openBytes(std::vector<std::uint8_t> &&bytes)
	{
		close();
		if (bytes.empty()) {
			return false;
		}
		m_bytes = std::move(bytes);
		if (!openMemoryFFmpeg(m_bytes, m_memoryInput, m_format, m_avio)) {
			close();
			return false;
		}
		return initialize();
	}

	UnsignedInt sampleRate() const override { return OUTPUT_SAMPLE_RATE; }
	Real durationMS() const override { return m_durationMS; }
	Bool isEnded() const override
	{
		return m_eof && m_sink.pendingFrames() == 0U ? TRUE : FALSE;
	}

	Bool readPcm(AudioPcmChunk &chunk, UnsignedInt maxFrames) override
	{
		chunk = {};
		if (!m_opened || m_failed || maxFrames == 0U || maxFrames > MAX_OUTPUT_FRAMES) {
			return FALSE;
		}
		while (chunk.frameCount < maxFrames) {
			AudioPcmChunk part;
			if (m_sink.take(maxFrames - chunk.frameCount, part)) {
				if (!append(chunk, std::move(part), maxFrames)) {
					m_failed = true;
					break;
				}
				if (chunk.frameCount >= maxFrames) {
					return TRUE;
				}
				continue;
			}
			if (m_eof) {
				break;
			}
			if (!decodeUntil(maxFrames - chunk.frameCount)) {
				m_failed = true;
				chunk = {};
				return FALSE;
			}
		}
		return chunk.frameCount != 0U ? TRUE : FALSE;
	}

private:
	bool initialize()
	{
		if (m_format == nullptr) {
			return false;
		}
		m_streamIndex = av_find_best_stream(m_format, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
		if (m_streamIndex < 0 || m_format->streams[m_streamIndex] == nullptr) {
			return false;
		}
		const AVStream *stream = m_format->streams[m_streamIndex];
		const AVCodecParameters *parameters = stream->codecpar;
		const AVCodec *codec = parameters == nullptr
			? nullptr : avcodec_find_decoder(parameters->codec_id);
		m_codecContext = codec == nullptr ? nullptr : avcodec_alloc_context3(codec);
		if (m_codecContext == nullptr || avcodec_parameters_to_context(m_codecContext, parameters) < 0
			|| avcodec_open2(m_codecContext, codec, nullptr) < 0) {
			return false;
		}
		m_packet = av_packet_alloc();
		m_frame = av_frame_alloc();
		if (m_packet == nullptr || m_frame == nullptr) {
			return false;
		}
		std::int64_t durationMicroseconds = 0;
		if (stream->duration != AV_NOPTS_VALUE && stream->time_base.den != 0) {
			durationMicroseconds = av_rescale_q(stream->duration,
				stream->time_base, AV_TIME_BASE_Q);
		} else if (m_format->duration != AV_NOPTS_VALUE) {
			durationMicroseconds = m_format->duration;
		}
		if (durationMicroseconds > 0) {
			m_durationMS = static_cast<Real>(durationMicroseconds) / 1000.0f;
		}
		m_decoder.reset(1, m_sink);
		m_opened = true;
		return true;
	}

	bool append(AudioPcmChunk &destination, AudioPcmChunk &&source,
		UnsignedInt maxFrames)
	{
		if (source.frameCount == 0U || source.frameCount > maxFrames
			|| source.data.size() != static_cast<std::size_t>(source.frameCount)
			* OUTPUT_BYTES_PER_FRAME) {
			return false;
		}
		if (destination.frameCount == 0U) {
			destination = std::move(source);
			return true;
		}
		if (source.frameCount > maxFrames - destination.frameCount) {
			return false;
		}
		const bool hasExpectedStart = destination.startSample <=
			(std::numeric_limits<std::int64_t>::max)()
			- static_cast<std::int64_t>(destination.frameCount);
		if (!hasExpectedStart || source.startSample != destination.startSample
			+ static_cast<std::int64_t>(destination.frameCount)) {
			destination.discontinuity = true;
		}
		destination.discontinuity = destination.discontinuity || source.discontinuity;
		destination.data.insert(destination.data.end(), source.data.begin(), source.data.end());
		destination.frameCount += source.frameCount;
		return true;
	}

	bool receiveFrames(UnsignedInt requiredFrames)
	{
		for (;;) {
			const int result = avcodec_receive_frame(m_codecContext, m_frame);
			if (result == AVERROR(EAGAIN)) {
				m_needReceive = false;
				if (m_flushSent) {
					m_decoderEof = true;
				}
				return true;
			}
			if (result == AVERROR_EOF) {
				m_needReceive = false;
				m_decoderEof = true;
				return true;
			}
			if (result < 0 || !m_decoder.convert(m_frame,
				m_format->streams[m_streamIndex]->time_base.num,
				m_format->streams[m_streamIndex]->time_base.den, m_sink)) {
				return false;
			}
			av_frame_unref(m_frame);
			if (m_sink.pendingFrames() >= requiredFrames) {
				return true;
			}
		}
	}

	bool decodeUntil(UnsignedInt requiredFrames)
	{
		while (m_sink.pendingFrames() < requiredFrames && !m_eof) {
			if (m_needReceive) {
				if (!receiveFrames(requiredFrames)) {
					return false;
				}
				if (m_sink.pendingFrames() >= requiredFrames) {
					return true;
				}
				if (m_decoderEof) {
					if (!m_decoder.drain(m_sink)) {
						return false;
					}
					m_eof = true;
					continue;
				}
			}
			if (m_packetReady) {
				const int result = avcodec_send_packet(m_codecContext, m_packet);
				if (result == AVERROR(EAGAIN)) {
					m_needReceive = true;
					continue;
				}
				if (result < 0) {
					return false;
				}
				av_packet_unref(m_packet);
				m_packetReady = false;
				m_needReceive = true;
				continue;
			}
			if (m_inputEof) {
				if (!m_flushSent) {
					const int result = avcodec_send_packet(m_codecContext, nullptr);
					if (result == AVERROR(EAGAIN)) {
						m_needReceive = true;
						continue;
					}
					if (result < 0) {
						return false;
					}
					m_flushSent = true;
					m_needReceive = true;
					continue;
				}
				m_needReceive = true;
				continue;
			}
			const int result = readFFmpegFrame(m_format, m_packet);
			if (result == AVERROR_EOF) {
				m_inputEof = true;
				continue;
			}
			if (result < 0) {
				m_failed = true;
				return false;
			}
			if (m_packet->stream_index != m_streamIndex) {
				av_packet_unref(m_packet);
				continue;
			}
			m_packetReady = true;
		}
		return true;
	}

	void close()
	{
		m_decoder.reset(0, m_sink);
		av_frame_free(&m_frame);
		av_packet_free(&m_packet);
		avcodec_free_context(&m_codecContext);
		if (m_avio != nullptr) {
			closeMemoryFFmpeg(m_format, m_avio);
		} else if (m_format != nullptr) {
			avformat_close_input(&m_format);
		}
		m_memoryInput = {};
		m_bytes.clear();
		m_streamIndex = -1;
		m_durationMS = 0.0f;
		m_opened = false;
		m_eof = false;
		m_failed = false;
		m_inputEof = false;
		m_flushSent = false;
		m_decoderEof = false;
		m_needReceive = false;
		m_packetReady = false;
	}

	std::vector<std::uint8_t> m_bytes;
	MemoryFFmpegInput m_memoryInput;
	AVFormatContext *m_format = nullptr;
	AVIOContext *m_avio = nullptr;
	AVCodecContext *m_codecContext = nullptr;
	AVPacket *m_packet = nullptr;
	AVFrame *m_frame = nullptr;
	int m_streamIndex = -1;
	Real m_durationMS = 0.0f;
	SequentialPcmSink m_sink;
	FFmpegAudioDecoder m_decoder;
	bool m_opened = false;
	bool m_eof = false;
	bool m_failed = false;
	bool m_inputEof = false;
	bool m_flushSent = false;
	bool m_decoderEof = false;
	bool m_needReceive = false;
	bool m_packetReady = false;
};

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
	if (packet == nullptr || frame == nullptr) {
		av_frame_free(&frame);
		av_packet_free(&packet);
		avcodec_free_context(&codecContext);
		avformat_close_input(&format);
		return false;
	}
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
	while (!sink.isComplete() && packet != nullptr) {
		const int readResult = readFFmpegFrame(format, packet);
		if (readResult == AVERROR_EOF) {
			break;
		}
		if (readResult < 0) {
			goto cleanup;
		}
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
		while (!sink.isComplete()) {
			const int readResult = readFFmpegFrame(format, packet);
			if (readResult == AVERROR_EOF) {
				break;
			}
			if (readResult < 0) {
				goto cleanup;
			}
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

#if defined(RTS_NATIVE_AUDIO_ASSET_SOURCE_TEST_HOOK) && defined(RTS_HAS_FFMPEG)
namespace NativeAudioAssetSourceTestHook
{
void failFFmpegReadFrameAfter(UnsignedInt successfulReads)
{
	g_ffmpegReadFrameFailureAfter = successfulReads;
	g_ffmpegReadFrameSuccesses = 0;
}

void clearFFmpegReadFrameFailure()
{
	g_ffmpegReadFrameFailureAfter = (std::numeric_limits<UnsignedInt>::max)();
	g_ffmpegReadFrameSuccesses = 0;
}
}
#endif

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
	rememberVirtualIdentity(fileName, identity);
	return TRUE;
}

const void *FileAudioAssetSource::findVirtualIdentity(const AsciiString &fileName) const
{
	for (const VirtualIdentity &entry : m_virtualIdentities) {
		if (entry.fileName == fileName) {
			return &entry.identity;
		}
	}
	return nullptr;
}

const void *FileAudioAssetSource::rememberVirtualIdentity(const AsciiString &fileName,
	const std::string &identity) const
{
	for (VirtualIdentity &entry : m_virtualIdentities) {
		if (entry.fileName == fileName && entry.identity == identity) {
			return &entry.identity;
		}
	}
	// Keep older identities alive so an opaque pointer already returned to a
	// caller never changes meaning when a virtual source generation changes.
	m_virtualIdentities.push_front(VirtualIdentity { fileName, identity });
	return &m_virtualIdentities.front().identity;
}

std::string FileAudioAssetSource::resolvePath(const AsciiString &fileName) const
{
	try {
		std::filesystem::path path(fileName.str() == nullptr ? "" : fileName.str());
		const bool hasRoot = !m_rootDirectory.empty();
		std::filesystem::path root;
		if (hasRoot) {
			if (path.is_absolute()) {
				return std::string();
			}
			root = std::filesystem::path(m_rootDirectory);
			std::filesystem::path::const_iterator first = path.begin();
			const std::string rootName = root.filename().string();
			const std::string firstName = first == path.end() ? std::string() : first->string();
			// Audio settings commonly provide `Audio` as the root while generated
			// event names already carry the `Audio\\` virtual prefix.  Joining both
			// would silently turn every production lookup into `Audio\\Audio\\...`.
			if (!rootName.empty() && pathComponentEqualsIgnoreCase(rootName, firstName)) {
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
		if (error) {
			return std::string();
		}
		path = path.lexically_normal();

		if (hasRoot) {
			std::error_code rootError;
			root = std::filesystem::weakly_canonical(root, rootError);
			if (rootError) {
				rootError.clear();
				root = std::filesystem::absolute(root, rootError);
			}
			if (rootError || !isPathWithinRoot(root.lexically_normal(), path)) {
				return std::string();
			}
		}
		return path.string();
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

Bool FileAudioAssetSource::openPcmStream(const AsciiString &fileName,
	std::unique_ptr<AudioPcmStream> &stream) const
{
	stream.reset();
	const std::string path = resolvePath(fileName);
	if (path.empty()) {
		return FALSE;
	}
	std::error_code error;
	const bool looseExists = std::filesystem::exists(path, error) && !error;
#if defined(RTS_HAS_FFMPEG)
	if (looseExists) {
		std::unique_ptr<FFmpegPcmStream> decoder = std::make_unique<FFmpegPcmStream>();
		if (decoder->openPath(path)) {
			stream = std::move(decoder);
			return TRUE;
		}
		return FALSE;
	}
	std::vector<std::uint8_t> bytes;
	std::string identity;
	if (readVirtualFile(fileName, bytes, identity)) {
		std::unique_ptr<FFmpegPcmStream> decoder = std::make_unique<FFmpegPcmStream>();
		if (decoder->openBytes(std::move(bytes))) {
			stream = std::move(decoder);
			return TRUE;
		}
	}
#else
	(void)looseExists;
#endif
	return FALSE;
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
	if (const void *cachedIdentity = findVirtualIdentity(fileName)) {
		return cachedIdentity;
	}
	std::vector<std::uint8_t> bytes;
	std::string identity;
	if (!readVirtualFile(fileName, bytes, identity)) {
		return nullptr;
	}
	return findVirtualIdentity(fileName);
}

Bool FileAudioAssetSource::matchesFileIdentity(const AsciiString &fileName,
	const void *callerIdentity) const
{
	if (callerIdentity == nullptr) {
		return FALSE;
	}
	const std::string path = resolvePath(fileName);
	std::error_code error;
	if (!path.empty() && std::filesystem::exists(path, error) && !error
		&& callerIdentity == getFileIdentity(fileName)) {
		return TRUE;
	}
	if (m_virtualSource != nullptr
		&& m_virtualSource->matchesIdentity(fileName, callerIdentity)) {
		return TRUE;
	}
	return callerIdentity == findVirtualIdentity(fileName);
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
