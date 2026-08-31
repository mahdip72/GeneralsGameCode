#include "Lib/ReplayCommandContract.h"

#include <limits>

namespace rts
{
namespace replay_command
{

namespace
{

constexpr bool IsRecordableMessageType(std::int32_t type)
{
	return type == kClearGameDataMessageType ||
		(type > kBeginNetworkMessageType && type < kEndNetworkMessageType);
}

constexpr bool IsKnownArgumentType(ReplayArgumentType type)
{
	return static_cast<Byte>(type) <= static_cast<Byte>(ReplayArgumentType::WideChar);
}

void WriteU16(Byte *output, std::uint16_t value)
{
	output[0] = static_cast<Byte>(value & 0xffU);
	output[1] = static_cast<Byte>((value >> 8U) & 0xffU);
}

void WriteU32(Byte *output, std::uint32_t value)
{
	output[0] = static_cast<Byte>(value & 0xffU);
	output[1] = static_cast<Byte>((value >> 8U) & 0xffU);
	output[2] = static_cast<Byte>((value >> 16U) & 0xffU);
	output[3] = static_cast<Byte>((value >> 24U) & 0xffU);
}

std::uint16_t ReadU16(const Byte *input)
{
	return static_cast<std::uint16_t>(input[0]) |
		static_cast<std::uint16_t>(static_cast<std::uint16_t>(input[1]) << 8U);
}

std::uint32_t ReadU32(const Byte *input)
{
	return static_cast<std::uint32_t>(input[0]) |
		(static_cast<std::uint32_t>(input[1]) << 8U) |
		(static_cast<std::uint32_t>(input[2]) << 16U) |
		(static_cast<std::uint32_t>(input[3]) << 24U);
}

bool AddChecked(std::size_t left, std::size_t right, std::size_t &result)
{
	if (right > std::numeric_limits<std::size_t>::max() - left)
		return false;
	result = left + right;
	return true;
}

} // namespace

std::size_t GetReplayArgumentWireSize(ReplayArgumentType type)
{
	switch (type)
	{
	case ReplayArgumentType::Integer:
	case ReplayArgumentType::Real:
	case ReplayArgumentType::ObjectId:
	case ReplayArgumentType::DrawableId:
	case ReplayArgumentType::TeamId:
	case ReplayArgumentType::Timestamp:
		return 4U;
	case ReplayArgumentType::Boolean:
		return 1U;
	case ReplayArgumentType::Location:
		return 12U;
	case ReplayArgumentType::Pixel:
		return 8U;
	case ReplayArgumentType::PixelRegion:
		return 16U;
	case ReplayArgumentType::WideChar:
		return 2U;
	default:
		return 0U;
	}
}

bool IsCanonicalReplayFrameTransitionValid(std::uint32_t previousFrame,
	std::uint32_t nextFrame,
	std::uint32_t finalFrame)
{
	return nextFrame != std::numeric_limits<std::uint32_t>::max() &&
		nextFrame >= previousFrame && nextFrame <= finalFrame;
}

ParseResult ParseCanonicalReplayCommand(const Byte *bytes, std::size_t availableBytes)
{
	ParseResult result;
	if (bytes == nullptr)
	{
		result.error = ReplayCommandError::NullInput;
		return result;
	}
	if (availableBytes < sizeof(std::uint32_t))
	{
		result.error = ReplayCommandError::Truncated;
		return result;
	}

	const std::uint32_t recordByteCount = ReadU32(bytes);
	if (recordByteCount < kCanonicalReplayCommandHeaderBytes)
	{
		result.error = ReplayCommandError::InvalidLength;
		return result;
	}
	if (recordByteCount > kMaxReplayCommandBytes)
	{
		result.error = ReplayCommandError::TooLarge;
		return result;
	}
	if (recordByteCount > availableBytes)
	{
		result.error = ReplayCommandError::Truncated;
		return result;
	}

	const std::uint8_t descriptorCount = bytes[16];
	if (bytes[17] != 0U)
	{
		result.error = ReplayCommandError::InvalidReserved;
		return result;
	}
	const std::uint16_t argumentCount = ReadU16(bytes + 18U);
	if (argumentCount > kMaxReplayArguments)
	{
		result.error = ReplayCommandError::InvalidArgumentCount;
		return result;
	}
	if (!IsRecordableMessageType(static_cast<std::int32_t>(ReadU32(bytes + 8U))))
	{
		result.error = ReplayCommandError::InvalidMessageType;
		return result;
	}
	const std::int32_t playerIndex = static_cast<std::int32_t>(ReadU32(bytes + 12U));
	if (playerIndex < -1 || playerIndex > kMaxReplayPlayerIndex)
	{
		result.error = ReplayCommandError::InvalidPlayerIndex;
		return result;
	}

	std::size_t descriptorBytes = 0U;
	if (!AddChecked(0U,
		static_cast<std::size_t>(descriptorCount) * kCanonicalReplayCommandDescriptorBytes,
		descriptorBytes))
	{
		result.error = ReplayCommandError::InvalidLength;
		return result;
	}
	std::size_t payloadOffset = 0U;
	if (!AddChecked(kCanonicalReplayCommandHeaderBytes, descriptorBytes, payloadOffset) ||
		payloadOffset > recordByteCount)
	{
		result.error = ReplayCommandError::InvalidLength;
		return result;
	}

	std::uint32_t totalArguments = 0U;
	std::size_t expectedPayloadBytes = 0U;
	for (std::uint8_t i = 0U; i < descriptorCount; ++i)
	{
		const Byte *descriptor = bytes + kCanonicalReplayCommandHeaderBytes +
			static_cast<std::size_t>(i) * kCanonicalReplayCommandDescriptorBytes;
		if (descriptor[1] != 0U)
		{
			result.error = ReplayCommandError::InvalidReserved;
			return result;
		}
		const ReplayArgumentType type = static_cast<ReplayArgumentType>(descriptor[0]);
		if (!IsKnownArgumentType(type))
		{
			result.error = ReplayCommandError::UnknownArgumentType;
			return result;
		}
		const std::uint16_t count = ReadU16(descriptor + 2U);
		if (count == 0U)
		{
			result.error = ReplayCommandError::InvalidArgumentCount;
			return result;
		}
		if (count > kMaxReplayArguments - totalArguments)
		{
			result.error = ReplayCommandError::ArgumentCountMismatch;
			return result;
		}
		totalArguments += count;
		const std::size_t typeBytes = GetReplayArgumentWireSize(type);
		if (typeBytes > (kMaxReplayCommandBytes - payloadOffset) / count)
		{
			result.error = ReplayCommandError::PayloadSizeMismatch;
			return result;
		}
		expectedPayloadBytes += typeBytes * count;
	}

	if (totalArguments != argumentCount)
	{
		result.error = ReplayCommandError::ArgumentCountMismatch;
		return result;
	}
	if (recordByteCount - payloadOffset != expectedPayloadBytes)
	{
		result.error = ReplayCommandError::PayloadSizeMismatch;
		return result;
	}

	result.bytesConsumed = recordByteCount;
	result.view.frame = ReadU32(bytes + 4U);
	result.view.messageType = static_cast<std::int32_t>(ReadU32(bytes + 8U));
	result.view.playerIndex = static_cast<std::int32_t>(ReadU32(bytes + 12U));
	result.view.argumentCount = argumentCount;
	result.view.descriptorCount = descriptorCount;
	for (std::uint8_t i = 0U; i < descriptorCount; ++i)
	{
		const Byte *descriptor = bytes + kCanonicalReplayCommandHeaderBytes +
			static_cast<std::size_t>(i) * kCanonicalReplayCommandDescriptorBytes;
		result.view.descriptors[i] = {
			static_cast<ReplayArgumentType>(descriptor[0]),
			ReadU16(descriptor + 2U),
		};
	}
	result.view.payload = std::span<const Byte>(bytes + payloadOffset, expectedPayloadBytes);
	return result;
}

BuildResult BuildCanonicalReplayCommand(const ReplayCommandInput &input,
	Byte *output,
	std::size_t outputSize)
{
	BuildResult result;
	if (output == nullptr)
	{
		result.error = ReplayCommandError::NullInput;
		return result;
	}
	if (!IsRecordableMessageType(input.messageType))
	{
		result.error = ReplayCommandError::InvalidMessageType;
		return result;
	}
	if (input.playerIndex < -1 || input.playerIndex > kMaxReplayPlayerIndex)
	{
		result.error = ReplayCommandError::InvalidPlayerIndex;
		return result;
	}
	if (input.descriptors.size() > kMaxReplayDescriptors)
	{
		result.error = ReplayCommandError::InvalidArgumentCount;
		return result;
	}

	std::uint32_t totalArguments = 0U;
	std::size_t expectedPayloadBytes = 0U;
	for (const ReplayCommandDescriptor &descriptor : input.descriptors)
	{
		if (!IsKnownArgumentType(descriptor.argumentType))
		{
			result.error = ReplayCommandError::UnknownArgumentType;
			return result;
		}
		if (descriptor.argumentCount == 0U ||
			descriptor.argumentCount > kMaxReplayArguments - totalArguments)
		{
			result.error = ReplayCommandError::InvalidArgumentCount;
			return result;
		}
		totalArguments += descriptor.argumentCount;
		const std::size_t typeBytes = GetReplayArgumentWireSize(descriptor.argumentType);
		if (typeBytes > (kMaxReplayCommandBytes - kCanonicalReplayCommandHeaderBytes) /
				 descriptor.argumentCount)
		{
			result.error = ReplayCommandError::TooLarge;
			return result;
		}
		expectedPayloadBytes += typeBytes * descriptor.argumentCount;
	}
	if (totalArguments > kMaxReplayArguments || input.payload.size() != expectedPayloadBytes)
	{
		result.error = ReplayCommandError::PayloadSizeMismatch;
		return result;
	}

	std::size_t descriptorBytes = input.descriptors.size() * kCanonicalReplayCommandDescriptorBytes;
	const std::size_t recordByteCount = kCanonicalReplayCommandHeaderBytes +
		descriptorBytes + expectedPayloadBytes;
	if (recordByteCount > kMaxReplayCommandBytes)
	{
		result.error = ReplayCommandError::TooLarge;
		return result;
	}
	if (outputSize < recordByteCount)
	{
		result.error = ReplayCommandError::OutputTooSmall;
		return result;
	}

	WriteU32(output, static_cast<std::uint32_t>(recordByteCount));
	WriteU32(output + 4U, input.frame);
	WriteU32(output + 8U, static_cast<std::uint32_t>(input.messageType));
	WriteU32(output + 12U, static_cast<std::uint32_t>(input.playerIndex));
	output[16] = static_cast<Byte>(input.descriptors.size());
	output[17] = 0U;
	WriteU16(output + 18U, static_cast<std::uint16_t>(totalArguments));

	for (std::size_t i = 0U; i < input.descriptors.size(); ++i)
	{
		Byte *descriptor = output + kCanonicalReplayCommandHeaderBytes +
			i * kCanonicalReplayCommandDescriptorBytes;
		descriptor[0] = static_cast<Byte>(input.descriptors[i].argumentType);
		descriptor[1] = 0U;
		WriteU16(descriptor + 2U, input.descriptors[i].argumentCount);
	}
	for (std::size_t i = 0U; i < input.payload.size(); ++i)
		output[kCanonicalReplayCommandHeaderBytes + descriptorBytes + i] = input.payload[i];

	result.bytesWritten = recordByteCount;
	return result;
}

} // namespace replay_command
} // namespace rts
