#include "Common/File.h"
#include "VideoDevice/FFmpeg/FFmpegFile.h"

extern "C" {
#include <libavutil/avutil.h>
#include <libavutil/frame.h>
}

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

class MemoryTestFile final : public File
{
public:
	explicit MemoryTestFile(const char *path)
	{
		std::ifstream input(path, std::ios::binary);
		m_data.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
	}

	Int read(void *buffer, Int bytes) override
	{
		if (m_closed || buffer == nullptr || bytes < 0) {
			return -1;
		}
		const size_t available = m_data.size() - m_position;
		const size_t requested = static_cast<size_t>(bytes);
		const size_t count = std::min(available, requested);
		if (count != 0) {
			std::memcpy(buffer, m_data.data() + m_position, count);
			m_position += count;
		}
		return static_cast<Int>(count);
	}

	Int seek(Int offset, seekMode mode) override
	{
		++m_seekCount;
		if (m_closed) {
			return -1;
		}

		Int64 base = 0;
		switch (mode) {
			case START:
				break;
			case CURRENT:
				base = static_cast<Int64>(m_position);
				break;
			case END:
				base = static_cast<Int64>(m_data.size());
				break;
		}

		const Int64 target = base + offset;
		if (target < 0 || target > static_cast<Int64>(m_data.size())) {
			return -1;
		}
		m_position = static_cast<size_t>(target);
		return static_cast<Int>(m_position);
	}

	Int size() override { return static_cast<Int>(m_data.size()); }
	void close() override
	{
		++m_closeCount;
		m_closed = true;
	}
	Bool isClosed() const { return m_closed; }
	Bool isValid() const { return !m_data.empty(); }
	Int getCloseCount() const { return m_closeCount; }
	Int getSeekCount() const { return m_seekCount; }

private:
	std::vector<char> m_data;
	size_t m_position = 0;
	Bool m_closed = false;
	Int m_closeCount = 0;
	Int m_seekCount = 0;
};

struct DecodeState
{
	FFmpegFile *decoder = nullptr;
	Int videoFrames = 0;
	Int expectedNextFrame = 0;
	Bool sawBFrame = false;
	Bool indicesAreSequential = true;
	Bool firstFrameWasKey = false;
	std::vector<Int64> timestamps;
};

static void onFrame(AVFrame *frame, int, int stream_type, void *user_data)
{
	if (stream_type != AVMEDIA_TYPE_VIDEO) {
		return;
	}

	DecodeState *state = static_cast<DecodeState *>(user_data);
	if (state->videoFrames == 0) {
		state->firstFrameWasKey = (frame->flags & AV_FRAME_FLAG_KEY) != 0 || frame->pict_type == AV_PICTURE_TYPE_I;
	}
	if (state->decoder->getCurrentFrame() != state->expectedNextFrame) {
		state->indicesAreSequential = false;
	}
	state->sawBFrame = state->sawBFrame || frame->pict_type == AV_PICTURE_TYPE_B;
	state->timestamps.push_back(frame->best_effort_timestamp);
	++state->videoFrames;
	++state->expectedNextFrame;
}

int main(int argc, char **argv)
{
	if (argc != 2) {
		std::fputs("Expected one generated movie fixture path.\n", stderr);
		return 1;
	}

	MemoryTestFile file(argv[1]);
	if (!file.isValid() || file.size() <= 0x10000) {
		std::fputs("Generated movie fixture is empty.\n", stderr);
		return 1;
	}

	{
		FFmpegFile decoder;
		DecodeState state { &decoder };
		decoder.setFrameCallback(onFrame);
		decoder.setUserData(&state);
		if (!decoder.open(&file)) {
			std::fputs("FFmpegFile failed to open the generated movie.\n", stderr);
			return 1;
		}

		if (decoder.getWidth() != 256 || decoder.getHeight() != 256 || decoder.hasAudio()) {
			std::fputs("Generated movie metadata does not match the behavior contract.\n", stderr);
			return 1;
		}

		Int decodeCalls = 0;
		while (decoder.decodePacket()) {
			if (++decodeCalls > 1024) {
				std::fputs("Decoder failed to reach EOF within the bounded call count.\n", stderr);
				return 1;
			}
		}

		if (state.videoFrames != 12 || !state.sawBFrame || !state.indicesAreSequential || !decoder.isAtEnd()
			|| decoder.hasError()) {
			std::fputs("Decoder did not emit the complete deterministic B-frame sequence.\n", stderr);
			return 1;
		}
		const std::vector<Int64> goldenTimestamps = state.timestamps;
		for (Int repeat = 0; repeat < 3; ++repeat) {
			if (decoder.decodePacket() || state.videoFrames != 12) {
				std::fputs("Decoder did not remain stable after EOF.\n", stderr);
				return 1;
			}
		}

		state.videoFrames = 0;
		state.expectedNextFrame = 6;
		state.indicesAreSequential = true;
		state.firstFrameWasKey = false;
		state.timestamps.clear();
		const Int seekCountBefore = file.getSeekCount();
		if (!decoder.seekFrame(6)) {
			std::fputs("Decoder did not exercise the custom seek path.\n", stderr);
			return 1;
		}
		decodeCalls = 0;
		while (decoder.decodePacket()) {
			if (++decodeCalls > 1024) {
				std::fputs("Decoder failed to reach EOF after a non-keyframe seek.\n", stderr);
				return 1;
			}
		}
		const Bool tailTimestampsMatch = state.timestamps.size() == 6
			&& std::equal(state.timestamps.begin(), state.timestamps.end(), goldenTimestamps.begin() + 6);
		if (file.getSeekCount() <= seekCountBefore || state.videoFrames != 6 || decoder.getCurrentFrame() != 11
			|| !state.indicesAreSequential || state.firstFrameWasKey
			|| !tailTimestampsMatch || !decoder.isAtEnd()
			|| decoder.hasError()) {
			std::fputs("Decoder did not emit the expected tail after a non-keyframe seek.\n", stderr);
			return 1;
		}

		state.videoFrames = 0;
		state.expectedNextFrame = 0;
		state.indicesAreSequential = true;
		state.firstFrameWasKey = false;
		state.timestamps.clear();
		if (!decoder.seekFrame(0)) {
			std::fputs("Decoder failed its second seek after EOF.\n", stderr);
			return 1;
		}
		decodeCalls = 0;
		while (decoder.decodePacket()) {
			if (++decodeCalls > 1024) {
				std::fputs("Decoder failed to reach EOF after rewinding.\n", stderr);
				return 1;
			}
		}
		if (state.videoFrames != 12 || decoder.getCurrentFrame() != 11 || !state.indicesAreSequential
			|| state.timestamps != goldenTimestamps || !decoder.isAtEnd()
			|| decoder.hasError()) {
			std::fputs("Decoder did not replay the full sequence after rewinding.\n", stderr);
			return 1;
		}
		for (Int repeat = 0; repeat < 3; ++repeat) {
			if (decoder.decodePacket() || state.videoFrames != 12) {
				std::fputs("Decoder did not remain stable after the rewind EOF.\n", stderr);
				return 1;
			}
		}
	}

	if (!file.isClosed() || file.getCloseCount() != 1) {
		std::fputs("Decoder destructor did not close its owned file exactly once.\n", stderr);
		return 1;
	}

	std::puts("FFmpegFile behavior contract passed.");
	return 0;
}
