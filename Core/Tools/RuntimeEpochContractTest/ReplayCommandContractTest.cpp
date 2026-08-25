#include "Lib/ReplayCommandContract.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <span>

namespace
{

using Byte = std::uint8_t;
using namespace rts::replay_command;

int Check(bool condition, const char *message)
{
	if (!condition)
	{
		std::fprintf(stderr, "FAIL: %s\n", message);
		return 1;
	}
	return 0;
}

int CheckBytes(std::span<const Byte> actual, std::span<const Byte> expected, const char *message)
{
	if (actual.size() != expected.size())
		return Check(false, message);
	for (std::size_t i = 0; i < actual.size(); ++i)
	{
		if (actual[i] != expected[i])
			return Check(false, message);
	}
	return 0;
}

int TestBuildAndParseUsesCanonicalLittleEndianBytes()
{
	const std::array<ReplayCommandDescriptor, 2> descriptors = {{
		{ReplayArgumentType::Integer, 1U},
		{ReplayArgumentType::PixelRegion, 1U},
	}};
	const std::array<Byte, 20> payload = {{
		0x44U, 0x33U, 0x22U, 0x11U,
		0x01U, 0x00U, 0x00U, 0x00U,
		0x02U, 0x00U, 0x00U, 0x00U,
		0x03U, 0x00U, 0x00U, 0x00U,
		0x04U, 0x00U, 0x00U, 0x00U,
	}};
	const ReplayCommandInput input{
		0x01020304U,
		1001,
		-1,
		descriptors,
		payload,
	};

	std::array<Byte, kMaxReplayCommandBytes> output = {{}};
	const BuildResult built = BuildCanonicalReplayCommand(input, output);
	const std::array<Byte, 48> expected = {{
		0x30U, 0x00U, 0x00U, 0x00U,
		0x04U, 0x03U, 0x02U, 0x01U,
		0xE9U, 0x03U, 0x00U, 0x00U,
		0xFFU, 0xFFU, 0xFFU, 0xFFU,
		0x02U, 0x00U, 0x02U, 0x00U,
		0x00U, 0x00U, 0x01U, 0x00U,
		0x08U, 0x00U, 0x01U, 0x00U,
		0x44U, 0x33U, 0x22U, 0x11U,
		0x01U, 0x00U, 0x00U, 0x00U,
		0x02U, 0x00U, 0x00U, 0x00U,
		0x03U, 0x00U, 0x00U, 0x00U,
		0x04U, 0x00U, 0x00U, 0x00U,
	}};

	int result = 0;
	result |= Check(built.ok(), "canonical command builder accepts a valid command");
	result |= Check(built.bytesWritten == expected.size(), "builder reports the complete record size");
	result |= CheckBytes(std::span<const Byte>(output.data(), built.bytesWritten), expected,
		"builder emits the hand-checked little-endian record");

	const ParseResult parsed = ParseCanonicalReplayCommand(output.data(), built.bytesWritten);
	result |= Check(parsed.ok(), "canonical command parser accepts the complete record");
	result |= Check(parsed.bytesConsumed == expected.size(), "parser consumes exactly one record");
	if (parsed.ok())
	{
		result |= Check(parsed.view.frame == 0x01020304U, "parser returns the frame");
		result |= Check(parsed.view.messageType == 1001, "parser returns the message type");
		result |= Check(parsed.view.playerIndex == -1, "parser returns the player index");
		result |= Check(parsed.view.argumentCount == 2U, "parser returns the total argument count");
		result |= Check(parsed.view.descriptorCount == 2U, "parser returns all descriptors");
		result |= Check(parsed.view.payload.size() == payload.size(), "parser returns the exact payload span");
		result |= Check(parsed.view.descriptors[1].argumentType == ReplayArgumentType::PixelRegion,
			"parser preserves descriptor order");
	}
	return result;
}

int TestAllArgumentTypesAndEmptyCommand()
{
	const std::array<ReplayCommandDescriptor, 11> descriptors = {{
		{ReplayArgumentType::Integer, 1U},
		{ReplayArgumentType::Real, 1U},
		{ReplayArgumentType::Boolean, 1U},
		{ReplayArgumentType::ObjectId, 1U},
		{ReplayArgumentType::DrawableId, 1U},
		{ReplayArgumentType::TeamId, 1U},
		{ReplayArgumentType::Location, 1U},
		{ReplayArgumentType::Pixel, 1U},
		{ReplayArgumentType::PixelRegion, 1U},
		{ReplayArgumentType::Timestamp, 1U},
		{ReplayArgumentType::WideChar, 1U},
	}};
	const std::array<Byte, 63> payload = {{}};
	const ReplayCommandInput input{1U, 1002, 0, descriptors, payload};
	std::array<Byte, kMaxReplayCommandBytes> output = {{}};
	const BuildResult built = BuildCanonicalReplayCommand(input, output);
	int result = 0;
	result |= Check(built.ok(), "builder accepts every supported argument type");
	result |= Check(built.bytesWritten == 127U, "all supported types have fixed canonical sizes");
	const ParseResult parsed = ParseCanonicalReplayCommand(output.data(), built.bytesWritten);
	result |= Check(parsed.ok(), "parser accepts every supported argument type");
	result |= Check(parsed.view.argumentCount == 11U, "parser counts all supported arguments");

	const std::array<ReplayCommandDescriptor, 0> noDescriptors = {{}};
	const std::array<Byte, 0> noPayload = {{}};
	const ReplayCommandInput emptyInput{2U, 1003, 1, noDescriptors, noPayload};
	const BuildResult emptyBuilt = BuildCanonicalReplayCommand(emptyInput, output);
	result |= Check(emptyBuilt.ok(), "zero-argument command is valid");
	result |= Check(emptyBuilt.bytesWritten == 20U, "zero-argument command has only the fixed header");
	return result;
}

int TestMalformedRecordsAreRejected()
{
	const std::array<ReplayCommandDescriptor, 1> descriptors = {{
		{ReplayArgumentType::Integer, 1U},
	}};
	const std::array<Byte, 4> payload = {{0x01U, 0x00U, 0x00U, 0x00U}};
	const ReplayCommandInput input{7U, 1004, 2, descriptors, payload};
	std::array<Byte, kMaxReplayCommandBytes> output = {{}};
	const BuildResult built = BuildCanonicalReplayCommand(input, output);
	int result = 0;
	result |= Check(built.ok(), "malformed-record fixtures start from a valid command");

	result |= Check(ParseCanonicalReplayCommand(output.data(), 3U).error == ReplayCommandError::Truncated,
		"truncated record prefix is rejected");

	std::array<Byte, 28> tooSmall = {{}};
	for (std::size_t i = 0; i < built.bytesWritten; ++i)
		tooSmall[i] = output[i];
	tooSmall[0] = 19U;
	result |= Check(ParseCanonicalReplayCommand(tooSmall.data(), tooSmall.size()).error ==
		ReplayCommandError::InvalidLength, "record shorter than its fixed header is rejected");

	std::array<Byte, 28> tooLarge = {{}};
	for (std::size_t i = 0; i < built.bytesWritten; ++i)
		tooLarge[i] = output[i];
	tooLarge[0] = 0x01U;
	tooLarge[1] = 0x20U;
	result |= Check(ParseCanonicalReplayCommand(tooLarge.data(), tooLarge.size()).error ==
		ReplayCommandError::TooLarge, "record larger than the maximum is rejected before reading");

	std::array<Byte, 28> badReserved = {{}};
	for (std::size_t i = 0; i < built.bytesWritten; ++i)
		badReserved[i] = output[i];
	badReserved[17] = 1U;
	result |= Check(ParseCanonicalReplayCommand(badReserved.data(), badReserved.size()).error ==
		ReplayCommandError::InvalidReserved, "nonzero record reserved byte is rejected");

	std::array<Byte, 28> badType = {{}};
	for (std::size_t i = 0; i < built.bytesWritten; ++i)
		badType[i] = output[i];
	badType[20] = 0xFFU;
	result |= Check(ParseCanonicalReplayCommand(badType.data(), badType.size()).error ==
		ReplayCommandError::UnknownArgumentType, "unknown argument type is rejected");

	std::array<Byte, 28> badArgumentCount = {{}};
	for (std::size_t i = 0; i < built.bytesWritten; ++i)
		badArgumentCount[i] = output[i];
	badArgumentCount[18] = 2U;
	result |= Check(ParseCanonicalReplayCommand(badArgumentCount.data(), badArgumentCount.size()).error ==
		ReplayCommandError::ArgumentCountMismatch, "descriptor and argument totals must agree");

	std::array<Byte, 28> badMessageType = {{}};
	for (std::size_t i = 0; i < built.bytesWritten; ++i)
		badMessageType[i] = output[i];
	badMessageType[8] = 0U;
	result |= Check(ParseCanonicalReplayCommand(badMessageType.data(), badMessageType.size()).error ==
		ReplayCommandError::InvalidMessageType, "non-recordable message type is rejected");

	std::array<Byte, 28> badPlayerIndex = {{}};
	for (std::size_t i = 0; i < built.bytesWritten; ++i)
		badPlayerIndex[i] = output[i];
	badPlayerIndex[12] = 0x08U;
	result |= Check(ParseCanonicalReplayCommand(badPlayerIndex.data(), badPlayerIndex.size()).error ==
		ReplayCommandError::InvalidPlayerIndex, "player index outside the replay slot range is rejected");

	return result;
}

int TestBuilderRejectsInvalidInput()
{
	const std::array<Byte, 4> payload = {{0U, 0U, 0U, 0U}};
	std::array<Byte, kMaxReplayCommandBytes> output = {{}};
	int result = 0;

	const std::array<ReplayCommandDescriptor, 1> unknown = {{
		{static_cast<ReplayArgumentType>(255U), 1U},
	}};
	result |= Check(BuildCanonicalReplayCommand({1U, 1001, 0, unknown, payload}, output).error ==
		ReplayCommandError::UnknownArgumentType, "builder rejects unknown argument types");

	const std::array<ReplayCommandDescriptor, 1> zeroCount = {{
		{ReplayArgumentType::Integer, 0U},
	}};
	const std::array<Byte, 0> noPayload = {{}};
	result |= Check(BuildCanonicalReplayCommand({1U, 1001, 0, zeroCount, noPayload}, output).error ==
		ReplayCommandError::InvalidArgumentCount, "builder rejects zero-count descriptors");

	result |= Check(BuildCanonicalReplayCommand({1U, 1001, 0, std::span<const ReplayCommandDescriptor>(), payload}, output).error ==
		ReplayCommandError::PayloadSizeMismatch, "builder rejects payload without descriptors");

	result |= Check(BuildCanonicalReplayCommand({1U, 1001, 8, std::span<const ReplayCommandDescriptor>(),
		std::span<const Byte>()}, output).error == ReplayCommandError::InvalidPlayerIndex,
		"builder rejects a player index outside the replay slot range");

	return result;
}

} // namespace

int main()
{
	return TestBuildAndParseUsesCanonicalLittleEndianBytes() |
		TestAllArgumentTypesAndEmptyCommand() |
		TestMalformedRecordsAreRejected() |
		TestBuilderRejectsInvalidInput();
}
