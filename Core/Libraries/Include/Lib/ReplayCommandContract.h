#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace rts
{
namespace replay_command
{

using Byte = std::uint8_t;

constexpr std::size_t kCanonicalReplayCommandHeaderBytes = 20U;
constexpr std::size_t kCanonicalReplayCommandDescriptorBytes = 4U;
constexpr std::size_t kMaxReplayCommandBytes = 8192U;
constexpr std::uint16_t kMaxReplayArguments = 255U;
constexpr std::uint8_t kMaxReplayDescriptors = 255U;

// GameMessage IDs are part of the native replay wire contract.  Keep the
// values here so the parser does not depend on a game target's enum layout.
constexpr std::int32_t kClearGameDataMessageType = 27;
constexpr std::int32_t kBeginNetworkMessageType = 1000;
constexpr std::int32_t kEndNetworkMessageType = 1999;
// Commands store engine PlayerIndex values, not lobby slots. Player zero is
// neutral; eight occupied lobby slots can therefore produce player index 8.
// Both title recorders assert this bound against MAX_PLAYER_COUNT.
constexpr std::int32_t kMaxReplayPlayerIndex = 15;

// These are explicit wire IDs.  They intentionally mirror the stable
// GameMessageArgumentDataType order without depending on that C++ enum's
// underlying representation in this library.
enum class ReplayArgumentType : Byte
{
	Integer = 0U,
	Real = 1U,
	Boolean = 2U,
	ObjectId = 3U,
	DrawableId = 4U,
	TeamId = 5U,
	Location = 6U,
	Pixel = 7U,
	PixelRegion = 8U,
	Timestamp = 9U,
	WideChar = 10U,
};

struct ReplayCommandDescriptor
{
	ReplayArgumentType argumentType;
	std::uint16_t argumentCount;
};

struct ReplayCommandInput
{
	std::uint32_t frame;
	std::int32_t messageType;
	std::int32_t playerIndex;
	std::span<const ReplayCommandDescriptor> descriptors;
	std::span<const Byte> payload;
};

struct ReplayCommandView
{
	std::uint32_t frame = 0U;
	std::int32_t messageType = 0;
	std::int32_t playerIndex = 0;
	std::uint16_t argumentCount = 0U;
	std::uint8_t descriptorCount = 0U;
	std::array<ReplayCommandDescriptor, kMaxReplayDescriptors> descriptors = {{}};
	std::span<const Byte> payload;
};

enum class ReplayCommandError : std::uint8_t
{
	None = 0U,
	NullInput,
	Truncated,
	InvalidLength,
	TooLarge,
	InvalidReserved,
	InvalidMessageType,
	InvalidPlayerIndex,
	UnknownArgumentType,
	InvalidArgumentCount,
	ArgumentCountMismatch,
	PayloadSizeMismatch,
	InvalidFrame,
	OutputTooSmall,
};

struct ParseResult
{
	ReplayCommandError error = ReplayCommandError::None;
	std::size_t bytesConsumed = 0U;
	ReplayCommandView view;

	constexpr bool ok() const { return error == ReplayCommandError::None; }
};

struct BuildResult
{
	ReplayCommandError error = ReplayCommandError::None;
	std::size_t bytesWritten = 0U;

	constexpr bool ok() const { return error == ReplayCommandError::None; }
};

std::size_t GetReplayArgumentWireSize(ReplayArgumentType type);

bool IsCanonicalReplayFrameTransitionValid(std::uint32_t previousFrame,
	std::uint32_t nextFrame,
	std::uint32_t finalFrame);

ParseResult ParseCanonicalReplayCommand(const Byte *bytes, std::size_t availableBytes);

BuildResult BuildCanonicalReplayCommand(const ReplayCommandInput &input,
	Byte *output,
	std::size_t outputSize);

template <std::size_t N>
BuildResult BuildCanonicalReplayCommand(const ReplayCommandInput &input,
	std::array<Byte, N> &output)
{
	return BuildCanonicalReplayCommand(input, output.data(), output.size());
}

} // namespace replay_command
} // namespace rts
