/*
**	Command & Conquer Generals(tm)
**	Copyright 2025 Electronic Arts Inc.
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

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "Common/Recorder.h"
#include "Lib/FrameTimingDiagnostics.h"
#include "Common/file.h"
#include "Common/FileSystem.h"
#include "Common/PlayerList.h"
#include "Common/Player.h"
#include "Common/GlobalData.h"
#include "Common/GameEngine.h"
#include "GameClient/ClientInstance.h"
#include "GameClient/GameWindow.h"
#include "GameClient/GameWindowManager.h"
#include "GameClient/InGameUI.h"
#include "GameClient/Shell.h"
#include "GameClient/GameText.h"

#include "GameNetwork/LANAPICallbacks.h"
#include "GameNetwork/GameMessageParser.h"
#include "GameNetwork/GameSpy/PeerDefs.h"
#include "GameNetwork/networkutil.h"
#include "GameLogic/GameLogic.h"
#include "Common/RandomValue.h"
#include "Common/CRCDebug.h"
#include "Common/OptionPreferences.h"
#include "Common/version.h"
#include "Lib/ReplayFieldReader.h"

#if defined(_WIN64)
#include "Lib/RuntimeEpochContract.h"
#include "Lib/ReplayCommandContract.h"
#include <array>
#include <cstdint>
#include <cstring>
#endif

constexpr const char s_genrep[] = "GENREP";
constexpr const UnsignedInt replayBufferBytes = 8192;

Int REPLAY_CRC_INTERVAL = 100;

const char *replayExtention = ".rep";
const char *lastReplayFileName = "00000000";	// a name the user is unlikely to ever type, but won't cause panic & confusion

// TheSuperHackers @tweak helmutbuhler 25/04/2025
// The replay header contains two time fields; startTime and endTime of type time_t.
// time_t is 32 bit wide on VC6, but on newer compilers it is 64 bit wide.
// In order to remain compatible we need to load and save time values with 32 bits.
// Note that this will overflow on January 18, 2038. @todo Upgrade to 64 bits when we break compatibility.
typedef int32_t replay_time_t;

static time_t startTime;
static const UnsignedInt startTimeOffset = 6;
static const UnsignedInt endTimeOffset = startTimeOffset + sizeof(replay_time_t);
static const UnsignedInt frameCountOffset = endTimeOffset + sizeof(replay_time_t);
#if defined(_WIN64)
static const UnsignedInt desyncOffset = frameCountOffset + 4U;
static const UnsignedInt quitEarlyOffset = desyncOffset + 1U;
static const UnsignedInt disconOffset = quitEarlyOffset + 1U;
#else
static const UnsignedInt desyncOffset = frameCountOffset + sizeof(UnsignedInt);
static const UnsignedInt quitEarlyOffset = desyncOffset + sizeof(Bool);
static const UnsignedInt disconOffset = quitEarlyOffset + sizeof(Bool);
#endif

#if defined(_WIN64)
static constexpr Int kNativeReplayPayloadBase = static_cast<Int>(rts::runtime_epoch::kHeaderSize);
static constexpr Int kNativeReplayChecksumChunkSize = 64 * 1024;
// The epoch header keeps a 64-bit byte count so its wire layout is stable and
// future file backends can grow. The current File API and replay cursor are
// intentionally signed 32-bit, so schema 2 fails closed above this limit.
static constexpr std::uint64_t kNativeReplayMaxPayloadBytes =
	static_cast<std::uint64_t>(INT_MAX - kNativeReplayPayloadBase);

static_assert(static_cast<std::int32_t>(GameMessage::MSG_CLEAR_GAME_DATA) ==
	 rts::replay_command::kClearGameDataMessageType, "native replay clear-data ID must remain stable");
static_assert(static_cast<std::int32_t>(GameMessage::MSG_BEGIN_NETWORK_MESSAGES) ==
	 rts::replay_command::kBeginNetworkMessageType, "native replay network ID must remain stable");
static_assert(static_cast<std::int32_t>(GameMessage::MSG_END_NETWORK_MESSAGES) ==
	 rts::replay_command::kEndNetworkMessageType, "native replay network end ID must remain stable");
static_assert(MAX_PLAYER_COUNT - 1 == rts::replay_command::kMaxReplayPlayerIndex,
	"native replay engine player range must remain stable");

static Bool scanNativeReplayPayload(File* file,
	Int payloadBase,
	Int fileSize,
	rts::runtime_epoch::PayloadChecksumAccumulator& checksum)
{
	if (file == nullptr || payloadBase < 0 || fileSize < payloadBase)
	{
		return FALSE;
	}

	if (file->seek(payloadBase, File::seekMode::START) != payloadBase)
	{
		return FALSE;
	}

	std::array<rts::runtime_epoch::Byte, kNativeReplayChecksumChunkSize> buffer = {{}};
	Int remaining = fileSize - payloadBase;
	while (remaining > 0)
	{
		const Int chunkSize = remaining < static_cast<Int>(buffer.size())
			? remaining
			: static_cast<Int>(buffer.size());
		const Int bytesRead = file->read(buffer.data(), chunkSize);
		if (bytesRead != chunkSize)
		{
			return FALSE;
		}
		checksum.update(buffer.data(), static_cast<std::size_t>(bytesRead));
		remaining -= bytesRead;
	}

	return file->position() == fileSize;
}

static Bool writeNativeReplayContainerHeader(File* file,
	const rts::runtime_epoch::ReplayHeader& header)
{
	if (file == nullptr)
	{
		return FALSE;
	}

	const std::array<rts::runtime_epoch::Byte, rts::runtime_epoch::kHeaderSize> bytes =
		rts::runtime_epoch::Encode(header);
	if (file->seek(0, File::seekMode::START) != 0 ||
		file->write(bytes.data(), static_cast<Int>(bytes.size())) != static_cast<Int>(bytes.size()))
	{
		return FALSE;
	}
	return file->flush();
}

static Bool beginNativeReplayContainer(File* file)
{
	// The RPL3 header is the native replay compatibility boundary.  Schema 2
	// uses canonical, explicitly framed command records after the GENREP fields.
	rts::runtime_epoch::ReplayHeader header;
	header.buildCompatibilityId = rts::runtime_epoch::BuildCompatibilityIdFromExecutableCrc(
		TheGlobalData->m_exeCRC);
	header.contentHash = rts::runtime_epoch::ContentHashFromIniCrc(TheGlobalData->m_iniCRC);
	// An unfinished recording must never validate as a complete container.
	header.payloadByteCount = UINT64_MAX;
	header.payloadChecksum = 0U;
	return writeNativeReplayContainerHeader(file, header);
}

static Bool finalizeNativeReplayContainer(File* file)
{
	if (file == nullptr || !file->flush())
	{
		return FALSE;
	}

	const Int fileSize = file->size();
	if (fileSize < kNativeReplayPayloadBase)
	{
		return FALSE;
	}

	rts::runtime_epoch::PayloadChecksumAccumulator checksum;
	if (!scanNativeReplayPayload(file, kNativeReplayPayloadBase, fileSize, checksum))
	{
		return FALSE;
	}

	rts::runtime_epoch::ReplayHeader header;
	header.buildCompatibilityId = rts::runtime_epoch::BuildCompatibilityIdFromExecutableCrc(
		TheGlobalData->m_exeCRC);
	header.contentHash = rts::runtime_epoch::ContentHashFromIniCrc(TheGlobalData->m_iniCRC);
	header.payloadByteCount = checksum.byteCount();
	header.payloadChecksum = checksum.finish();
	if (!writeNativeReplayContainerHeader(file, header))
	{
		return FALSE;
	}

	return file->seek(fileSize, File::seekMode::START) == fileSize;
}

static Bool validateNativeReplayContainer(File* file, Int* payloadEnd)
{
	if (file == nullptr || TheGlobalData == nullptr)
	{
		return FALSE;
	}

	const Int fileSize = file->size();
	if (fileSize < kNativeReplayPayloadBase)
	{
		return FALSE;
	}

	std::array<rts::runtime_epoch::Byte, rts::runtime_epoch::kHeaderSize> bytes = {{}};
	if (file->seek(0, File::seekMode::START) != 0 ||
		file->read(bytes.data(), static_cast<Int>(bytes.size())) != static_cast<Int>(bytes.size()))
	{
		return FALSE;
	}

	rts::runtime_epoch::ReplayHeader header;
	if (!rts::runtime_epoch::Decode(bytes.data(), bytes.size(), &header))
	{
		return FALSE;
	}

	rts::runtime_epoch::ValidationOptions options;
	options.expectedBuildCompatibilityId =
		rts::runtime_epoch::BuildCompatibilityIdFromExecutableCrc(TheGlobalData->m_exeCRC);
	options.expectedContentHash =
		rts::runtime_epoch::ContentHashFromIniCrc(TheGlobalData->m_iniCRC);
	options.expectedSchemaVersion = rts::runtime_epoch::kCurrentReplaySchemaVersion;
	const std::uint64_t availablePayloadBytes =
		static_cast<std::uint64_t>(fileSize - kNativeReplayPayloadBase);
	options.maxPayloadByteCount = availablePayloadBytes < kNativeReplayMaxPayloadBytes
		? availablePayloadBytes
		: kNativeReplayMaxPayloadBytes;
	options.requireBuildCompatibilityMatch = true;
	options.requireContentHashMatch = true;
	if (!rts::runtime_epoch::Validate(header, options).ok())
	{
		return FALSE;
	}

	rts::runtime_epoch::PayloadChecksumAccumulator checksum;
	if (!scanNativeReplayPayload(file, kNativeReplayPayloadBase, fileSize, checksum) ||
		header.payloadByteCount != checksum.byteCount() ||
		header.payloadChecksum != checksum.finish())
	{
		return FALSE;
	}

	// Leave the stream at the first byte of the legacy replay payload.  Every
	// subsequent GENREP read is therefore relative to the validated container.
	if (header.payloadByteCount > options.maxPayloadByteCount)
	{
		return FALSE;
	}
	if (payloadEnd != nullptr)
		*payloadEnd = kNativeReplayPayloadBase + static_cast<Int>(header.payloadByteCount);
	return file->seek(kNativeReplayPayloadBase, File::seekMode::START) == kNativeReplayPayloadBase;
}
#endif

#if defined(_WIN64)
static void writeNativeReplayU16(rts::replay_command::Byte *output, std::uint16_t value)
{
	output[0] = static_cast<rts::replay_command::Byte>(value & 0xffU);
	output[1] = static_cast<rts::replay_command::Byte>((value >> 8U) & 0xffU);
}

static void writeNativeReplayU32(rts::replay_command::Byte *output, std::uint32_t value)
{
	output[0] = static_cast<rts::replay_command::Byte>(value & 0xffU);
	output[1] = static_cast<rts::replay_command::Byte>((value >> 8U) & 0xffU);
	output[2] = static_cast<rts::replay_command::Byte>((value >> 16U) & 0xffU);
	output[3] = static_cast<rts::replay_command::Byte>((value >> 24U) & 0xffU);
}

static std::uint16_t readNativeReplayU16(const rts::replay_command::Byte *input)
{
	return static_cast<std::uint16_t>(input[0]) |
		static_cast<std::uint16_t>(static_cast<std::uint16_t>(input[1]) << 8U);
}

static std::uint32_t readNativeReplayU32(const rts::replay_command::Byte *input)
{
	return static_cast<std::uint32_t>(input[0]) |
		(static_cast<std::uint32_t>(input[1]) << 8U) |
		(static_cast<std::uint32_t>(input[2]) << 16U) |
		(static_cast<std::uint32_t>(input[3]) << 24U);
}

static Bool mapNativeReplayArgumentType(GameMessageArgumentDataType input,
	rts::replay_command::ReplayArgumentType *output)
{
	if (output == nullptr)
		return FALSE;
	switch (input)
	{
	case ARGUMENTDATATYPE_INTEGER: *output = rts::replay_command::ReplayArgumentType::Integer; return TRUE;
	case ARGUMENTDATATYPE_REAL: *output = rts::replay_command::ReplayArgumentType::Real; return TRUE;
	case ARGUMENTDATATYPE_BOOLEAN: *output = rts::replay_command::ReplayArgumentType::Boolean; return TRUE;
	case ARGUMENTDATATYPE_OBJECTID: *output = rts::replay_command::ReplayArgumentType::ObjectId; return TRUE;
	case ARGUMENTDATATYPE_DRAWABLEID: *output = rts::replay_command::ReplayArgumentType::DrawableId; return TRUE;
	case ARGUMENTDATATYPE_TEAMID: *output = rts::replay_command::ReplayArgumentType::TeamId; return TRUE;
	case ARGUMENTDATATYPE_LOCATION: *output = rts::replay_command::ReplayArgumentType::Location; return TRUE;
	case ARGUMENTDATATYPE_PIXEL: *output = rts::replay_command::ReplayArgumentType::Pixel; return TRUE;
	case ARGUMENTDATATYPE_PIXELREGION: *output = rts::replay_command::ReplayArgumentType::PixelRegion; return TRUE;
	case ARGUMENTDATATYPE_TIMESTAMP: *output = rts::replay_command::ReplayArgumentType::Timestamp; return TRUE;
	case ARGUMENTDATATYPE_WIDECHAR: *output = rts::replay_command::ReplayArgumentType::WideChar; return TRUE;
	default: return FALSE;
	}
}

static Bool appendNativeReplayBytes(rts::replay_command::Byte *payload,
	std::size_t payloadCapacity,
	std::size_t *payloadOffset,
	const rts::replay_command::Byte *bytes,
	std::size_t byteCount)
{
	if (payload == nullptr || payloadOffset == nullptr || bytes == nullptr ||
		*payloadOffset > payloadCapacity || byteCount > payloadCapacity - *payloadOffset)
	{
		return FALSE;
	}
	for (std::size_t i = 0U; i < byteCount; ++i)
		payload[*payloadOffset + i] = bytes[i];
	*payloadOffset += byteCount;
	return TRUE;
}

static Bool appendNativeReplayArgument(GameMessageArgumentDataType type,
	const GameMessageArgumentType &arg,
	rts::replay_command::Byte *payload,
	std::size_t payloadCapacity,
	std::size_t *payloadOffset)
{
	std::uint32_t bits = 0U;
	std::array<rts::replay_command::Byte, 16U> bytes = {{}};
	std::size_t byteCount = 0U;
	switch (type)
	{
	case ARGUMENTDATATYPE_INTEGER:
		writeNativeReplayU32(bytes.data(), static_cast<std::uint32_t>(arg.integer)); byteCount = 4U; break;
	case ARGUMENTDATATYPE_REAL:
		std::memcpy(&bits, &arg.real, sizeof(bits)); writeNativeReplayU32(bytes.data(), bits); byteCount = 4U; break;
	case ARGUMENTDATATYPE_BOOLEAN:
		bytes[0] = arg.boolean ? 1U : 0U; byteCount = 1U; break;
	case ARGUMENTDATATYPE_OBJECTID:
		writeNativeReplayU32(bytes.data(), static_cast<std::uint32_t>(arg.objectID)); byteCount = 4U; break;
	case ARGUMENTDATATYPE_DRAWABLEID:
		writeNativeReplayU32(bytes.data(), static_cast<std::uint32_t>(arg.drawableID)); byteCount = 4U; break;
	case ARGUMENTDATATYPE_TEAMID:
		writeNativeReplayU32(bytes.data(), static_cast<std::uint32_t>(arg.teamID)); byteCount = 4U; break;
	case ARGUMENTDATATYPE_LOCATION:
		std::memcpy(&bits, &arg.location.x, sizeof(bits)); writeNativeReplayU32(bytes.data(), bits);
		std::memcpy(&bits, &arg.location.y, sizeof(bits)); writeNativeReplayU32(bytes.data() + 4U, bits);
		std::memcpy(&bits, &arg.location.z, sizeof(bits)); writeNativeReplayU32(bytes.data() + 8U, bits);
		byteCount = 12U; break;
	case ARGUMENTDATATYPE_PIXEL:
		writeNativeReplayU32(bytes.data(), static_cast<std::uint32_t>(arg.pixel.x));
		writeNativeReplayU32(bytes.data() + 4U, static_cast<std::uint32_t>(arg.pixel.y));
		byteCount = 8U; break;
	case ARGUMENTDATATYPE_PIXELREGION:
		writeNativeReplayU32(bytes.data(), static_cast<std::uint32_t>(arg.pixelRegion.lo.x));
		writeNativeReplayU32(bytes.data() + 4U, static_cast<std::uint32_t>(arg.pixelRegion.lo.y));
		writeNativeReplayU32(bytes.data() + 8U, static_cast<std::uint32_t>(arg.pixelRegion.hi.x));
		writeNativeReplayU32(bytes.data() + 12U, static_cast<std::uint32_t>(arg.pixelRegion.hi.y));
		byteCount = 16U; break;
	case ARGUMENTDATATYPE_TIMESTAMP:
		writeNativeReplayU32(bytes.data(), static_cast<std::uint32_t>(arg.timestamp)); byteCount = 4U; break;
	case ARGUMENTDATATYPE_WIDECHAR:
		writeNativeReplayU16(bytes.data(), static_cast<std::uint16_t>(arg.wChar)); byteCount = 2U; break;
	default:
		return FALSE;
	}
	return appendNativeReplayBytes(payload, payloadCapacity, payloadOffset, bytes.data(), byteCount);
}

static Bool buildNativeReplayCommand(GameMessage *msg,
	std::array<rts::replay_command::Byte, rts::replay_command::kMaxReplayCommandBytes> &record,
	std::size_t *recordBytes)
{
	if (msg == nullptr || recordBytes == nullptr)
		return FALSE;

	std::array<rts::replay_command::ReplayCommandDescriptor, rts::replay_command::kMaxReplayDescriptors> descriptors = {{}};
	std::size_t descriptorCount = 0U;
	const std::size_t argumentCount = msg->getArgumentCount();
	for (std::size_t i = 0U; i < argumentCount; ++i)
	{
		const GameMessageArgumentDataType type = msg->getArgumentDataType(static_cast<Int>(i));
		rts::replay_command::ReplayArgumentType wireType;
		if (!mapNativeReplayArgumentType(type, &wireType))
			return FALSE;
		if (descriptorCount == 0U || descriptors[descriptorCount - 1U].argumentType != wireType)
		{
			if (descriptorCount >= descriptors.size())
				return FALSE;
			descriptors[descriptorCount] = {wireType, 1U};
			++descriptorCount;
		}
		else
		{
			if (descriptors[descriptorCount - 1U].argumentCount == rts::replay_command::kMaxReplayArguments)
				return FALSE;
			++descriptors[descriptorCount - 1U].argumentCount;
		}
	}

	std::array<rts::replay_command::Byte, rts::replay_command::kMaxReplayCommandBytes> payload = {{}};
	std::size_t payloadBytes = 0U;
	for (std::size_t i = 0U; i < argumentCount; ++i)
	{
		if (!appendNativeReplayArgument(msg->getArgumentDataType(static_cast<Int>(i)),
			*msg->getArgument(static_cast<Int>(i)), payload.data(), payload.size(), &payloadBytes))
		{
			return FALSE;
		}
	}

	const rts::replay_command::ReplayCommandInput input{
		static_cast<std::uint32_t>(TheGameLogic->getFrame()),
		static_cast<std::int32_t>(msg->getType()),
		static_cast<std::int32_t>(msg->getPlayerIndex()),
		std::span<const rts::replay_command::ReplayCommandDescriptor>(descriptors.data(), descriptorCount),
		std::span<const rts::replay_command::Byte>(payload.data(), payloadBytes),
	};
	const rts::replay_command::BuildResult built =
		rts::replay_command::BuildCanonicalReplayCommand(input, record);
	if (!built.ok())
		return FALSE;
	*recordBytes = built.bytesWritten;
	return TRUE;
}

static Bool writeNativeReplayExact(File *file, const void *data, std::size_t byteCount)
{
	if (file == nullptr || data == nullptr || byteCount > static_cast<std::size_t>(INT_MAX))
		return FALSE;
	const Int position = file->position();
	if (position < kNativeReplayPayloadBase)
		return FALSE;
	const std::uint64_t payloadBytes =
		static_cast<std::uint64_t>(position - kNativeReplayPayloadBase);
	if (payloadBytes > kNativeReplayMaxPayloadBytes ||
		byteCount > kNativeReplayMaxPayloadBytes - payloadBytes)
	{
		return FALSE;
	}
	return file->write(data, static_cast<Int>(byteCount)) == static_cast<Int>(byteCount);
}

static Bool writeNativeReplayU16Field(File *file, std::uint16_t value)
{
	std::array<rts::replay_command::Byte, 2U> bytes = {{}};
	writeNativeReplayU16(bytes.data(), value);
	return writeNativeReplayExact(file, bytes.data(), bytes.size());
}

static Bool writeNativeReplayU32Field(File *file, std::uint32_t value)
{
	std::array<rts::replay_command::Byte, 4U> bytes = {{}};
	writeNativeReplayU32(bytes.data(), value);
	return writeNativeReplayExact(file, bytes.data(), bytes.size());
}

static Bool writeNativeReplayBoolField(File *file, Bool value)
{
	const rts::replay_command::Byte byte = value ? 1U : 0U;
	return writeNativeReplayExact(file, &byte, sizeof(byte));
}

static Bool writeNativeReplayAsciiString(File *file, const char *value)
{
	if (value == nullptr)
		return FALSE;
	const std::size_t length = std::strlen(value) + 1U;
	return writeNativeReplayExact(file, value, length);
}

static Bool writeNativeReplayWideString(File *file, const WideChar *value)
{
	if (value == nullptr)
		return FALSE;
	for (std::size_t i = 0U;; ++i)
	{
		const std::uint16_t character = static_cast<std::uint16_t>(value[i]);
		if (!writeNativeReplayU16Field(file, character))
			return FALSE;
		if (character == 0U)
			return TRUE;
	}
}

static Bool writeNativeReplaySystemTime(File *file, const SYSTEMTIME &value)
{
	return writeNativeReplayU16Field(file, value.wYear) &&
		writeNativeReplayU16Field(file, value.wMonth) &&
		writeNativeReplayU16Field(file, value.wDayOfWeek) &&
		writeNativeReplayU16Field(file, value.wDay) &&
		writeNativeReplayU16Field(file, value.wHour) &&
		writeNativeReplayU16Field(file, value.wMinute) &&
		writeNativeReplayU16Field(file, value.wSecond) &&
		writeNativeReplayU16Field(file, value.wMilliseconds);
}

static Bool readNativeReplayU16Field(File *file, std::uint16_t *value)
{
	std::array<rts::replay_command::Byte, 2U> bytes = {{}};
	if (file == nullptr || value == nullptr ||
		file->read(bytes.data(), static_cast<Int>(bytes.size())) != static_cast<Int>(bytes.size()))
	{
		return FALSE;
	}
	*value = readNativeReplayU16(bytes.data());
	return TRUE;
}

static Bool readNativeReplayU32Field(File *file, std::uint32_t *value)
{
	std::array<rts::replay_command::Byte, 4U> bytes = {{}};
	if (file == nullptr || value == nullptr ||
		file->read(bytes.data(), static_cast<Int>(bytes.size())) != static_cast<Int>(bytes.size()))
	{
		return FALSE;
	}
	*value = readNativeReplayU32(bytes.data());
	return TRUE;
}

static Bool readNativeReplayBoolField(File *file, Bool *value)
{
	rts::replay_command::Byte byte = 0U;
	if (file == nullptr || value == nullptr || file->read(&byte, 1) != 1 || byte > 1U)
		return FALSE;
	*value = byte != 0U;
	return TRUE;
}

static Bool readNativeReplaySystemTime(File *file, SYSTEMTIME *value)
{
	return value != nullptr &&
		readNativeReplayU16Field(file, &value->wYear) &&
		readNativeReplayU16Field(file, &value->wMonth) &&
		readNativeReplayU16Field(file, &value->wDayOfWeek) &&
		readNativeReplayU16Field(file, &value->wDay) &&
		readNativeReplayU16Field(file, &value->wHour) &&
		readNativeReplayU16Field(file, &value->wMinute) &&
		readNativeReplayU16Field(file, &value->wSecond) &&
		readNativeReplayU16Field(file, &value->wMilliseconds);
}
#endif

static Bool writeAtOffset(File* file, Int offset, const void* data, Int dataSize)
{
	if (file == nullptr || data == nullptr || offset < 0 || dataSize < 0)
		return FALSE;
	UnsignedInt fileSize = file->size();
	#if defined(_WIN64)
	const Int absoluteOffset = kNativeReplayPayloadBase + offset;
	#else
	const Int absoluteOffset = offset;
	#endif
	if (absoluteOffset < 0 || static_cast<UnsignedInt>(absoluteOffset) > fileSize ||
		static_cast<UnsignedInt>(dataSize) > fileSize - static_cast<UnsignedInt>(absoluteOffset))
	{
		return FALSE;
	}
	Bool writeSucceeded = FALSE;
	if (file->seek(absoluteOffset, File::seekMode::START) == absoluteOffset)
	{
		writeSucceeded = file->write(data, dataSize) == dataSize;
	}
	MAYBE_UNUSED Int res = file->seek(fileSize, File::seekMode::START);
	(void)res;
	DEBUG_ASSERTCRASH(res == fileSize, ("Could not seek to end of file!"));
	return writeSucceeded && res == static_cast<Int>(fileSize);
}

#if defined(_WIN64)
static Bool writeNativeReplayU32AtOffset(File *file, Int offset, std::uint32_t value)
{
	std::array<rts::replay_command::Byte, 4U> bytes = {{}};
	writeNativeReplayU32(bytes.data(), value);
	return writeAtOffset(file, offset, bytes.data(), static_cast<Int>(bytes.size()));
}

static Bool writeNativeReplayBoolAtOffset(File *file, Int offset, Bool value)
{
	const rts::replay_command::Byte byte = value ? 1U : 0U;
	return writeAtOffset(file, offset, &byte, sizeof(byte));
}
#endif

#if defined(RTS_DEBUG)
static FILE* openStatsLogFile()
{
	unsigned long bufSize = MAX_COMPUTERNAME_LENGTH + 1;
	char computerName[MAX_COMPUTERNAME_LENGTH + 1];
	if (!GetComputerName(computerName, &bufSize))
	{
		strcpy(computerName, "unknown");
	}
	AsciiString statsFile = TheGlobalData->m_baseStatsDir;
	statsFile.concat(computerName);
	statsFile.concat(".txt");
	return fopen(statsFile.str(), "a+");
}
#endif

RecorderClass::CRCInfo::CRCInfo() :
	m_sawCRCMismatch(FALSE),
	m_skippedOne(FALSE),
	m_localPlayer(0)
{}

RecorderClass::CRCInfo::CRCInfo(UnsignedInt localPlayer, Bool isMultiplayer)
{
	m_sawCRCMismatch = FALSE;
	m_skippedOne = !isMultiplayer;
	m_localPlayer = localPlayer;
}

void RecorderClass::CRCInfo::addCRC(UnsignedInt val)
{
	// TheSuperHackers @fix helmutbuhler 03/04/2025
	// In Multiplayer, the first MSG_LOGIC_CRC message somehow doesn't make it through the network.
	// Perhaps this happens because the network is not yet set up on frame 0.
	// So we also don't queue up the first local crc message, otherwise the crc
	// messages wouldn't match up anymore and we'd desync immediately during playback.
	if (!m_skippedOne)
	{
		m_skippedOne = TRUE;
		return;
	}

	m_data.push_back(val);
	//DEBUG_LOG(("CRCInfo::addCRC() - crc %8.8X pushes list to %d entries (full=%d)", val, m_data.size(), !m_data.empty()));
}

UnsignedInt RecorderClass::CRCInfo::readCRC()
{
	if (m_data.empty())
	{
		DEBUG_LOG(("CRCInfo::readCRC() - bailing, full=0, size=%d", m_data.size()));
		return 0;
	}

	UnsignedInt val = m_data.front();
	m_data.pop_front();
	//DEBUG_LOG(("CRCInfo::readCRC() - returning %8.8X, full=%d, size=%d", val, !m_data.empty(), m_data.size()));
	return val;
}

void RecorderClass::logGameStart(AsciiString options)
{
	if (!m_file)
		return;

	time(&startTime);
	replay_time_t tmp = (replay_time_t)startTime;
#if defined(_WIN64)
	if (!writeNativeReplayU32AtOffset(m_file, startTimeOffset,
		static_cast<std::uint32_t>(static_cast<std::int32_t>(tmp))))
		m_replayWriteError = TRUE;
#else
	writeAtOffset(m_file, startTimeOffset, &tmp, sizeof(tmp));
#endif

#if defined(RTS_DEBUG)
	if (TheNetwork && TheGlobalData->m_saveStats)
	{
		TheFileSystem->createDirectory(TheGlobalData->m_baseStatsDir);
		FILE *logFP = openStatsLogFile();
		if (!logFP)
		{
			TheWritableGlobalData->m_baseStatsDir = TheGlobalData->getPath_UserData();
			logFP = openStatsLogFile();
		}
		if (logFP)
		{
			struct tm *t2 = localtime(&startTime);
			fprintf(logFP, "\nGame start at %s\tOptions are %s\n", asctime(t2), options.str());
			fclose(logFP);
		}
	}
#endif
}

void RecorderClass::logPlayerDisconnect(UnicodeString player, Int slot)
{
	if (!m_file)
		return;

	DEBUG_ASSERTCRASH((slot >= 0) && (slot < MAX_SLOTS), ("Attempting to disconnect an invalid slot number"));
	if ((slot < 0) || (slot >= (MAX_SLOTS)))
	{
		return;
	}
	Bool flag = TRUE;
#if defined(_WIN64)
	Int playerSlotDisconOffset = disconOffset + slot;
	if (!writeNativeReplayBoolAtOffset(m_file, playerSlotDisconOffset, flag))
		m_replayWriteError = TRUE;
#else
	Int playerSlotDisconOffset = disconOffset + slot * sizeof(Bool);
	writeAtOffset(m_file, playerSlotDisconOffset, &flag, sizeof(flag));
#endif

#if defined(RTS_DEBUG)
	if (TheGlobalData->m_saveStats)
	{
		FILE *logFP = openStatsLogFile();
		if (logFP)
		{
			time_t t;
			time(&t);
			struct tm *t2 = localtime(&t);
			fprintf(logFP, "\tPlayer %ls dropped at %s", player.str(), asctime(t2));
			fclose(logFP);
		}
	}
#endif
}

void RecorderClass::logCRCMismatch()
{
	if (!m_file)
		return;

	Bool flag = TRUE;
#if defined(_WIN64)
	if (!writeNativeReplayBoolAtOffset(m_file, desyncOffset, flag))
		m_replayWriteError = TRUE;
#else
	writeAtOffset(m_file, desyncOffset, &flag, sizeof(flag));
#endif

#if defined(RTS_DEBUG)
	if (TheGlobalData->m_saveStats)
	{
		m_wasDesync = TRUE;
		FILE *logFP = openStatsLogFile();
		if (logFP)
		{
			time_t t;
			time(&t);
			struct tm *t2 = localtime(&t);
			fprintf(logFP, "\tCRC mismatch at %s", asctime(t2));
			fclose(logFP);
		}
	}
#endif
}

void RecorderClass::logGameEnd()
{
	if (!m_file)
		return;

	time_t t;
	time(&t);
	UnsignedInt frameCount = TheGameLogic->getFrame();
	replay_time_t tmp = (replay_time_t)t;
#if defined(_WIN64)
	if (!writeNativeReplayU32AtOffset(m_file, endTimeOffset,
			static_cast<std::uint32_t>(static_cast<std::int32_t>(tmp))) ||
		!writeNativeReplayU32AtOffset(m_file, frameCountOffset,
			static_cast<std::uint32_t>(frameCount)))
	{
		m_replayWriteError = TRUE;
	}
#else
	writeAtOffset(m_file, endTimeOffset, &tmp, sizeof(tmp));
	writeAtOffset(m_file, frameCountOffset, &frameCount, sizeof(frameCount));
#endif

#if defined(RTS_DEBUG)
	if (TheNetwork && TheGlobalData->m_saveStats)
	{
		FILE *logFP = openStatsLogFile();
		if (logFP)
		{
			struct tm *t2 = localtime(&t);
			time_t duration = t - startTime;
			Int minutes = duration/60;
			Int seconds = duration%60;
			fprintf(logFP, "Game end at   %s(%d:%2.2d elapsed time)\n", asctime(t2), minutes, seconds);
			fclose(logFP);
		}
	}
#endif
}

void RecorderClass::cleanUpReplayFile()
{
#if defined(RTS_DEBUG)
	if (TheGlobalData->m_saveStats)
	{
		char fname[_MAX_PATH+1];
		strlcpy(fname, TheGlobalData->m_baseStatsDir.str(), ARRAY_SIZE(fname));
		strlcat(fname, m_fileName.str(), ARRAY_SIZE(fname));
		DEBUG_LOG(("Saving replay to %s", fname));
		AsciiString oldFname;
		oldFname.format("%s%s", getReplayDir().str(), m_fileName.str());
		CopyFile(oldFname.str(), fname, TRUE);

#ifdef DEBUG_LOGGING
		const char* logFileName = DebugGetLogFileName();
		if (logFileName[0] == '\0')
			return;

		AsciiString debugFname = fname;
		debugFname.truncateBy(3);
		debugFname.concat("txt");
		UnsignedInt fileSize = 0;
		FILE *fp = fopen(logFileName, "rb");
		if (fp)
		{
			fseek(fp, 0, SEEK_END);
			fileSize = ftell(fp);
			fclose(fp);
			fp = nullptr;
			DEBUG_LOG(("Log file size was %d", fileSize));
		}

		const int MAX_DEBUG_SIZE = 65536;
		if (fileSize <= MAX_DEBUG_SIZE || TheGlobalData->m_saveAllStats)
		{
			DEBUG_LOG(("Using CopyFile to copy %s", logFileName));
			CopyFile(logFileName, debugFname.str(), TRUE);
		}
		else
		{
			DEBUG_LOG(("manual copy of %s", logFileName));
			FILE *ifp = fopen(logFileName, "rb");
			FILE *ofp = fopen(debugFname.str(), "wb");
			if (ifp && ofp)
			{
				fseek(ifp, fileSize-MAX_DEBUG_SIZE, SEEK_SET);
				char buf[4096];
				Int len;
				while ( (len=fread(buf, 1, 4096, ifp)) > 0 )
				{
					fwrite(buf, 1, len, ofp);
				}
				fclose(ofp);
				fclose(ifp);
				ifp = nullptr;
				ofp = nullptr;
			}
			else
			{
				if (ifp) fclose(ifp);
				if (ofp) fclose(ofp);
				ifp = nullptr;
				ofp = nullptr;
			}
		}
#endif // DEBUG_LOGGING
	}
#endif
}

/**
 * The recorder object.
 */
RecorderClass *TheRecorder = nullptr;

/**
 * Constructor
 */
RecorderClass::RecorderClass()
{
	m_originalGameMode = GAME_NONE;
	m_mode = RECORDERMODETYPE_RECORD;
	m_file = nullptr;
	m_fileName.clear();
	m_currentFilePosition = 0;
	m_doingAnalysis = FALSE;
	m_archiveReplays = FALSE;
	m_nextFrame = 0;
	m_replayReadError = FALSE;
	m_replayWriteError = FALSE;
#if defined(_WIN64)
	m_nativeReplayContainer = FALSE;
	m_nativeReplayPayloadEnd = 0;
	m_nativeReplayRecordBytes = 0U;
#endif
	m_wasDesync = FALSE;
	init(); // just for the heck of it.
}

/**
 * Destructor
 */
RecorderClass::~RecorderClass() {
}

/**
 * Initialization
 * The recorder will record by default since every game will be recorded.
 * Obviously a game that is being played back will not be recorded.
 * Since the playback is done through a special interface, that interface
 * will set the recorder mode to RECORDERMODETYPE_PLAYBACK.
 */
void RecorderClass::init() {
	m_originalGameMode = GAME_NONE;
	m_mode = RECORDERMODETYPE_NONE;
	m_file = nullptr;
	m_fileName.clear();
	m_currentFilePosition = 0;
	m_gameInfo.clearSlotList();
	m_gameInfo.reset();
	if (TheGlobalData->m_pendingFile.isEmpty())
		m_gameInfo.setMap(TheGlobalData->m_mapName);
	else
		m_gameInfo.setMap(TheGlobalData->m_pendingFile);
	m_gameInfo.setSeed(GetGameLogicRandomSeed());
	m_wasDesync = FALSE;
	m_doingAnalysis = FALSE;
	m_playbackFrameCount = 0;
	m_replayReadError = FALSE;
	m_replayWriteError = FALSE;
#if defined(_WIN64)
	m_nativeReplayContainer = FALSE;
	m_nativeReplayPayloadEnd = 0;
	m_nativeReplayRecordBytes = 0U;
#endif

	OptionPreferences optionPref;
	m_archiveReplays = optionPref.getArchiveReplaysEnabled();
}

/**
 * Reset the recorder to the "initialized state."
 */
void RecorderClass::reset() {
	if (m_file != nullptr) {
		m_file->close();
		m_file = nullptr;
	}
	m_fileName.clear();

	init();
}

/**
 * update
 * Do the update for this frame.
 */
void RecorderClass::update() {
	rts::frame_timing::Scope frameTiming(rts::frame_timing::RecorderUpdate);
	if (m_mode == RECORDERMODETYPE_RECORD || m_mode == RECORDERMODETYPE_NONE) {
		updateRecord();
	} else if (isPlaybackMode()) {
		updatePlayback();
	}
}

/**
 * Do the update for the next frame of this playback.
 */
void RecorderClass::updatePlayback() {
	// Remove any bad commands that have been inserted by the local user that shouldn't be
	// executed during playback.
	CullBadCommandsResult result = cullBadCommands();

	#if defined(_WIN64)
	if (m_nativeReplayContainer && result.hasNewGameMessage) {
		// Native replay frame-zero commands were recorded after the new map was
		// initialized. Defer them until MSG_NEW_GAME has been consumed so their
		// temporary AI groups cannot advance deterministic IDs before map setup.
		return;
	}
	#endif

	if (result.hasClearGameDataMessage) {
		// TheSuperHackers @bugfix Stop appending more commands if the replay playback is about to end.
		// Previously this would be able to append more commands, which could have unintended consequences,
		// such as crashing the game when a MSG_PLACE_BEACON is appended after MSG_CLEAR_GAME_DATA.
		// MSG_CLEAR_GAME_DATA is supposed to be processed later this frame, which will then stop this playback.
		return;
	}

	if (m_nextFrame == -1) {
		// This is reached if there are no more commands to be executed.
		return;
	}
	UnsignedInt curFrame = TheGameLogic->getFrame();
	if (m_doingAnalysis)
		curFrame = m_nextFrame;

	// While there are commands to be queued up for this frame, do it.
	while (m_nextFrame == curFrame) {
		appendNextCommand();	// append the next command to TheCommandQueue
		if (m_replayReadError)
			return;
		readNextFrame();	// Read the next command's frame number for playback.
		if (m_replayReadError)
			return;
	}
}

/**
 * Stop the currently running playback. This is probably due either to the user exiting out of the playback or
 * reaching the end of the playback file.
 */
void RecorderClass::stopPlayback() {
	if (m_file != nullptr) {
		m_file->close();
		m_file = nullptr;
	}
	m_fileName.clear();
	#if defined(_WIN64)
	m_nativeReplayContainer = FALSE;
	m_nativeReplayPayloadEnd = 0;
	m_nativeReplayRecordBytes = 0U;
	#endif

	if (!m_doingAnalysis)
	{
		TheGameLogic->exitGame();
	}
}

/**
 * Update function for recording a game. Basically all the pertinent logic commands for this frame are written out
 * to a file.
 */
void RecorderClass::updateRecord()
{
	Bool needFlush = FALSE;
	static Int lastFrame = -1;
	GameMessage *msg = TheCommandList->getFirstMessage();
	while (msg != nullptr) {
		if (msg->getType() == GameMessage::MSG_NEW_GAME &&
			 msg->getArgument(0)->integer != GAME_SHELL &&
			 msg->getArgument(0)->integer != GAME_NONE) {
			m_originalGameMode = msg->getArgument(0)->integer;
			DEBUG_LOG(("RecorderClass::updateRecord() - original game is mode %d", m_originalGameMode));
			lastFrame = 0;
			GameDifficulty diff = DIFFICULTY_NORMAL;
			if (msg->getArgumentCount() >= 2)
				diff = (GameDifficulty)msg->getArgument(1)->integer;
			Int rankPoints = 0;
			if (msg->getArgumentCount() >= 3)
				rankPoints = msg->getArgument(2)->integer;
			Int maxFPS = 0;
			if (msg->getArgumentCount() >= 4)
				maxFPS = msg->getArgument(3)->integer;

			startRecording(diff, m_originalGameMode, rankPoints, maxFPS);
		} else if (msg->getType() == GameMessage::MSG_CLEAR_GAME_DATA) {
			if (m_file != nullptr) {
				lastFrame = -1;
				writeToFile(msg);
				stopRecording();
				needFlush = FALSE;
			}
			m_fileName.clear();
		} else {
			if (m_file != nullptr) {
				if ((msg->getType() > GameMessage::MSG_BEGIN_NETWORK_MESSAGES) &&
						(msg->getType() < GameMessage::MSG_END_NETWORK_MESSAGES)) {
					// Only write the important messages to the file.
					writeToFile(msg);
					needFlush = TRUE;
				}
			}
		}
		msg = msg->next();
	}

	if (needFlush) {
		rts::frame_timing::Scope flushTiming(rts::frame_timing::RecorderFlush);
		DEBUG_ASSERTCRASH(m_file != nullptr, ("RecorderClass::updateRecord() - unexpected call to fflush(m_file)"));
		m_file->flush();
	}
}

/**
 * Start a new file for recording. This will always overwrite the "LastReplay.rep" file with the new one.
 * So don't call this unless you really mean it.
 */
void RecorderClass::startRecording(GameDifficulty diff, Int originalGameMode, Int rankPoints, Int maxFPS) {
	DEBUG_ASSERTCRASH(m_file == nullptr, ("Starting to record game while game is in progress."));

	reset();

	AsciiString filepath = getReplayDir();

	// We have to make sure the replay dir exists.
	TheFileSystem->createDirectory(filepath);

	m_fileName = getLastReplayFileName();
	m_fileName.concat(getReplayExtention());
	filepath.concat(m_fileName);
	#if defined(_WIN64)
	m_file = TheFileSystem->openFile(filepath.str(), File::READWRITE | File::BINARY | File::TRUNCATE);
	#else
	m_file = TheFileSystem->openFile(filepath.str(), File::WRITE | File::BINARY);
	#endif
	if (m_file == nullptr) {
		DEBUG_ASSERTCRASH(m_file != nullptr, ("Failed to create replay file"));
		m_fileName.clear();
		return;
	}
	#if defined(_WIN64)
	if (!beginNativeReplayContainer(m_file))
	{
		DEBUG_LOG(("RecorderClass::startRecording - failed to write RPL3 container header"));
		m_file->close();
		m_file = nullptr;
		if (!DeleteFile(filepath.str()))
			DEBUG_LOG(("RecorderClass::startRecording - failed to remove incomplete RPL3 replay"));
		m_fileName.clear();
		return;
	}
	#endif
	m_mode = RECORDERMODETYPE_RECORD;
	#if defined(_WIN64)
	Bool nativeHeaderWriteOk = writeNativeReplayExact(m_file, s_genrep, sizeof(s_genrep) - 1U);
	#else
	// TheSuperHackers @info the null terminator needs to be ignored to maintain retail replay file layout
	m_file->writeFormat("%s", s_genrep);
	#endif

	//
	// save space for stats to be filled in.
	//
	// **** if this changes, change the LAN code above ****
	//
	#if defined(_WIN64)
	nativeHeaderWriteOk = writeNativeReplayU32Field(m_file, 0U) && nativeHeaderWriteOk;
	nativeHeaderWriteOk = writeNativeReplayU32Field(m_file, 0U) && nativeHeaderWriteOk;
	#else
	replay_time_t time = 0;
	m_file->write(&time, sizeof(time));	// reserve space for start time
	m_file->write(&time, sizeof(time));	// reserve space for end time
	#endif

	#if defined(_WIN64)
	nativeHeaderWriteOk = writeNativeReplayU32Field(m_file, 0U) && nativeHeaderWriteOk;
	#else
	UnsignedInt frames = 0;
	m_file->write(&frames, sizeof(frames));	// reserve space for duration in frames
	#endif

	Bool flag = FALSE;
	#if defined(_WIN64)
	nativeHeaderWriteOk = writeNativeReplayBoolField(m_file, flag) && nativeHeaderWriteOk;
	nativeHeaderWriteOk = writeNativeReplayBoolField(m_file, flag) && nativeHeaderWriteOk;
	#else
	m_file->write(&flag, sizeof(flag));	// reserve space for flag (true if we desync)
	m_file->write(&flag, sizeof(flag));	// reserve space for flag (true if we quit early)
	#endif
	for (Int i=0; i<MAX_SLOTS; ++i)
	{
		#if defined(_WIN64)
		nativeHeaderWriteOk = writeNativeReplayBoolField(m_file, flag) && nativeHeaderWriteOk;
		#else
		m_file->write(&flag, sizeof(flag));	// reserve space for flag (true if player i disconnects)
		#endif
	}

	// Print out the name of the replay.
	UnicodeString replayName;
	replayName = TheGameText->fetch("GUI:LastReplay");
	#if defined(_WIN64)
	nativeHeaderWriteOk = writeNativeReplayWideString(m_file, replayName.str()) && nativeHeaderWriteOk;
	#else
	m_file->writeFormat(L"%s", replayName.str());
	m_file->writeChar(L"\0");
	#endif

	// Date and Time
	SYSTEMTIME systemTime;
	GetLocalTime( &systemTime );
	#if defined(_WIN64)
	nativeHeaderWriteOk = writeNativeReplaySystemTime(m_file, systemTime) && nativeHeaderWriteOk;
	#else
	m_file->write(&systemTime, sizeof(systemTime));
	#endif

	// write out version info
	UnicodeString versionString = TheVersion->getUnicodeVersion();
	UnicodeString versionTimeString = TheVersion->getUnicodeBuildTime();
	UnsignedInt versionNumber = TheVersion->getVersionNumber();
	#if defined(_WIN64)
	nativeHeaderWriteOk = writeNativeReplayWideString(m_file, versionString.str()) && nativeHeaderWriteOk;
	nativeHeaderWriteOk = writeNativeReplayWideString(m_file, versionTimeString.str()) && nativeHeaderWriteOk;
	nativeHeaderWriteOk = writeNativeReplayU32Field(m_file, versionNumber) && nativeHeaderWriteOk;
	nativeHeaderWriteOk = writeNativeReplayU32Field(m_file, TheGlobalData->m_exeCRC) && nativeHeaderWriteOk;
	nativeHeaderWriteOk = writeNativeReplayU32Field(m_file, TheGlobalData->m_iniCRC) && nativeHeaderWriteOk;
	#else
	m_file->writeFormat(L"%s", versionString.str());
	m_file->writeChar(L"\0");
	m_file->writeFormat(L"%s", versionTimeString.str());
	m_file->writeChar(L"\0");
	m_file->write(&versionNumber, sizeof(versionNumber));
	m_file->write(&(TheGlobalData->m_exeCRC), sizeof(TheGlobalData->m_exeCRC));
	m_file->write(&(TheGlobalData->m_iniCRC), sizeof(TheGlobalData->m_iniCRC));
	#endif

	// Number of players
	/*
	Int numPlayers = ThePlayerList->getPlayerCount();
	fwrite(&numPlayers, sizeof(numPlayers), 1, m_file);
	*/

	// Write the slot list.
	AsciiString theSlotList;
	Int localIndex = -1;
	if (TheNetwork)
	{
		if (TheLAN)
		{
			GameInfo *game = TheLAN->GetMyGame();
			DEBUG_ASSERTCRASH(game, ("Starting a LAN game with no LANGameInfo object!"));
			theSlotList = GameInfoToAsciiString(game);

			for (Int i=0; i<MAX_SLOTS; ++i)
			{
				if (game->getLocalIP() == game->getSlot(i)->getIP())
				{
					localIndex = i;
					break;
				}
			}
		}
		else
		{
			theSlotList = GameInfoToAsciiString(TheGameSpyGame);
			localIndex = TheGameSpyGame->getLocalSlotNum();
		}
	}
	else
	{
    if(TheSkirmishGameInfo)
    {
			TheSkirmishGameInfo->setCRCInterval(REPLAY_CRC_INTERVAL);
      theSlotList = GameInfoToAsciiString(TheSkirmishGameInfo);
      DEBUG_LOG(("GameInfo String: %s",theSlotList.str()));
			localIndex = 0;
    }
    else
    {
		  // single player.  format the generic (empty) slotlist
			m_gameInfo.setCRCInterval(REPLAY_CRC_INTERVAL);
		  theSlotList = GameInfoToAsciiString(&m_gameInfo);
    }
	}
	logGameStart(theSlotList);
	DEBUG_LOG(("RecorderClass::startRecording - theSlotList = %s", theSlotList.str()));

	// write slot list (starting spots, color, alliances, etc
	#if defined(_WIN64)
	nativeHeaderWriteOk = writeNativeReplayAsciiString(m_file, theSlotList.str()) && nativeHeaderWriteOk;
	AsciiString localIndexString;
	localIndexString.format("%d", localIndex);
	nativeHeaderWriteOk = writeNativeReplayAsciiString(m_file, localIndexString.str()) && nativeHeaderWriteOk;
	#else
	m_file->writeFormat("%s", theSlotList.str());
	m_file->writeChar("\0");

	m_file->writeFormat("%d", localIndex);
	m_file->writeChar("\0");
	#endif

	/*
	/// @todo fix this to use starting spots and player alliances when those are put in the game.
	for (Int i = 0; i < numPlayers; ++i) {
		Player *player = ThePlayerList->getNthPlayer(i);
		if (player == nullptr) {
			continue;
		}
		UnicodeString name = player->getPlayerDisplayName();
		fwprintf(m_file, L"%s", name.str());
		fputwc(0, m_file);
		UnicodeString faction = player->getFaction()->getFactionDisplayName();
		fwprintf(m_file, L"%s", faction.str());
		fputwc(0, m_file);
		Int color = player->getColor()->getAsInt();
		fwrite(&color, sizeof(color), 1, m_file);
		Int team = 0;
		Int startingSpot = 0;
		fwrite(&startingSpot, sizeof(Int), 1, m_file);
		fwrite(&team, sizeof(Int), 1, m_file);
	}
	*/

	// Write the game difficulty.
	#if defined(_WIN64)
	nativeHeaderWriteOk = writeNativeReplayU32Field(m_file,
		static_cast<std::uint32_t>(static_cast<std::int32_t>(diff))) && nativeHeaderWriteOk;
	nativeHeaderWriteOk = writeNativeReplayU32Field(m_file,
		static_cast<std::uint32_t>(originalGameMode)) && nativeHeaderWriteOk;
	nativeHeaderWriteOk = writeNativeReplayU32Field(m_file,
		static_cast<std::uint32_t>(rankPoints)) && nativeHeaderWriteOk;
	nativeHeaderWriteOk = writeNativeReplayU32Field(m_file,
		static_cast<std::uint32_t>(maxFPS)) && nativeHeaderWriteOk;
	if (!nativeHeaderWriteOk)
	{
		m_replayWriteError = TRUE;
		DEBUG_LOG(("RecorderClass::startRecording - failed to write canonical replay fields"));
	}
	#else
	m_file->write(&diff, sizeof(diff));

	// Write original game mode
	m_file->write(&originalGameMode, sizeof(originalGameMode));

	// Write rank points to add at game start
	m_file->write(&rankPoints, sizeof(rankPoints));

	// Write maxFPS chosen
	m_file->write(&maxFPS, sizeof(maxFPS));
	#endif

	DEBUG_LOG(("RecorderClass::startRecording() - diff=%d, mode=%d, FPS=%d", diff, originalGameMode, maxFPS));

	/*
	// Write the map name.
	fprintf(m_file, "%s", (TheGlobalData->m_mapName).str());
	fputc(0, m_file);
	*/

	/// @todo Need to write game options when there are some to be written.
}

/**
 * This will stop the current recording session and close the file. This should always be called at the end of
 * every game.
 */
void RecorderClass::stopRecording() {
	logGameEnd();
	Bool replayFinalized = !m_replayWriteError;
	#if defined(_WIN64)
	if (replayFinalized && m_file != nullptr)
	{
		replayFinalized = finalizeNativeReplayContainer(m_file);
		if (!replayFinalized)
		{
			DEBUG_LOG(("RecorderClass::stopRecording - failed to finalize RPL3 container"));
		}
	}
	#endif
	if (TheNetwork)
	{
		//if (TheLAN)
		{
			if (m_wasDesync)
				cleanUpReplayFile();
			m_wasDesync = FALSE;
		}
	}
	if (m_file != nullptr) {
		m_file->close();
		m_file = nullptr;

		if (replayFinalized && m_archiveReplays)
			archiveReplay(m_fileName);
		else if (!replayFinalized)
		{
			AsciiString invalidReplayPath = getReplayDir();
			invalidReplayPath.concat(m_fileName);
			if (!DeleteFile(invalidReplayPath.str()))
				DEBUG_LOG(("RecorderClass::stopRecording - failed to remove incomplete RPL3 replay"));
			m_mode = RECORDERMODETYPE_NONE;
		}
	}
	m_fileName.clear();
}

/**
 * TheSuperHackers @feature Stubbjax 17/10/2025 Copy the replay file to the archive directory and rename it using the current timestamp.
 */
void RecorderClass::archiveReplay(AsciiString fileName)
{
	SYSTEMTIME st;
	GetLocalTime(&st);

	AsciiString archiveFileName;
	// Use a standard YYYYMMDD_HHMMSS format for simplicity and to avoid conflicts.
	archiveFileName.format("%04d%02d%02d_%02d%02d%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

	AsciiString extension = getReplayExtention();
	AsciiString sourcePath = getReplayDir();
	sourcePath.concat(fileName);

	if (!sourcePath.endsWith(extension))
		sourcePath.concat(extension);

	AsciiString destPath = getReplayArchiveDir();
	TheFileSystem->createDirectory(destPath.str());

	destPath.concat(archiveFileName);
	destPath.concat(extension);

	if (!CopyFile(sourcePath.str(), destPath.str(), FALSE))
		DEBUG_LOG(("RecorderClass::archiveReplay: Failed to copy %s to %s", sourcePath.str(), destPath.str()));
}

/**
 * Write this game message to the record file. This also writes the game message's execution frame.
 */
void RecorderClass::writeToFile(GameMessage * msg) {
	rts::frame_timing::Scope encodeTiming(rts::frame_timing::RecorderEncode);
#if defined(_WIN64)
	std::array<rts::replay_command::Byte, rts::replay_command::kMaxReplayCommandBytes> record = {{}};
	std::size_t recordBytes = 0U;
	if (!buildNativeReplayCommand(msg, record, &recordBytes) ||
		!writeNativeReplayExact(m_file, record.data(), recordBytes))
	{
		m_replayWriteError = TRUE;
		DEBUG_LOG(("RecorderClass::writeToFile - failed to write canonical replay command"));
	}
	return;
#else
	// Write the frame number for this command.
	UnsignedInt frame = TheGameLogic->getFrame();
	m_file->write(&frame, sizeof(frame));

	// Write the command type
	GameMessage::Type type = msg->getType();
	m_file->write(&type, sizeof(type));

	// Write the player index
	Int playerIndex = msg->getPlayerIndex();
	m_file->write(&playerIndex, sizeof(playerIndex));

#ifdef DEBUG_LOGGING
	AsciiString commandName = msg->getCommandAsString();
	if (type < GameMessage::MSG_BEGIN_NETWORK_MESSAGES || type > GameMessage::MSG_END_NETWORK_MESSAGES)
	{
		commandName.concat(" (Non-Network message!)");
	}
	else if (type == GameMessage::MSG_BEGIN_NETWORK_MESSAGES)
	{
		AsciiString tmp;
		tmp.format(" (CRC 0x%8.8X)", msg->getArgument(0)->integer);
		commandName.concat(tmp);
	}

	//DEBUG_LOG(("RecorderClass::writeToFile - Adding %s command from player %d to TheCommandList on frame %d",
		//commandName.str(), msg->getPlayerIndex(), TheGameLogic->getFrame()));
#endif // DEBUG_LOGGING

	GameMessageParser *parser = newInstance(GameMessageParser)(msg);
	UnsignedByte numTypes = parser->getNumTypes();
	m_file->write(&numTypes, sizeof(numTypes));

	GameMessageParserArgumentType *argType = parser->getFirstArgumentType();
	while (argType != nullptr) {
		UnsignedByte type = (UnsignedByte)(argType->getType());
		m_file->write(&type, sizeof(type));

		UnsignedByte argTypeCount = (UnsignedByte)(argType->getArgCount());
		m_file->write(&argTypeCount, sizeof(argTypeCount));

		argType = argType->getNext();
	}

	const size_t argsCount = msg->getArgumentCount();

	for (size_t i = 0; i < argsCount; ++i) {
		GameMessageArgumentDataType argType = msg->getArgumentDataType(i);
		const GameMessageArgumentType* arg = msg->getArgument(i);
		writeArgument(argType, *arg);
	}

	deleteInstance(parser);
	parser = nullptr;
#endif
}

void RecorderClass::writeArgument(GameMessageArgumentDataType type, const GameMessageArgumentType arg) {

	switch (type) {

		case ARGUMENTDATATYPE_INTEGER:
			m_file->write( &(arg.integer), sizeof(arg.integer) );
			break;
		case ARGUMENTDATATYPE_REAL:
			m_file->write( &(arg.real), sizeof(arg.real) );
			break;
		case ARGUMENTDATATYPE_BOOLEAN:
			m_file->write( &(arg.boolean), sizeof(arg.boolean) );
			break;
		case ARGUMENTDATATYPE_OBJECTID:
			m_file->write( &(arg.objectID), sizeof(arg.objectID) );
			break;
		case ARGUMENTDATATYPE_DRAWABLEID:
			m_file->write( &(arg.drawableID), sizeof(arg.drawableID) );
			break;
		case ARGUMENTDATATYPE_TEAMID:
			m_file->write( &(arg.teamID), sizeof(arg.teamID) );
			break;
		case ARGUMENTDATATYPE_LOCATION:
			m_file->write( &(arg.location), sizeof(arg.location) );
			break;
		case ARGUMENTDATATYPE_PIXEL:
			m_file->write( &(arg.pixel), sizeof(arg.pixel) );
			break;
		case ARGUMENTDATATYPE_PIXELREGION:
			m_file->write( &(arg.pixelRegion), sizeof(arg.pixelRegion) );
			break;
		case ARGUMENTDATATYPE_TIMESTAMP:
			m_file->write( &(arg.timestamp), sizeof(arg.timestamp) );
			break;
		case ARGUMENTDATATYPE_WIDECHAR:
			m_file->write( &(arg.wChar), sizeof(arg.wChar) );
			break;
		default:
			DEBUG_LOG(("Unknown GameMessageArgumentDataType in RecorderClass::writeArgument"));
			break;
	}
}

/**
 * Read in a replay header, for (1) populating a replay listbox or (2) starting playback.  In
 * case (2), set FILE *m_file.
 */
Bool RecorderClass::readReplayHeader(ReplayHeader& header)
{
	AsciiString filepath = getReplayDir();
	filepath.concat(header.filename.str());

	// TheSuperHackers @performance More buffered data reduces disk overhead and will improve fast forward playback
	const UnsignedInt buffersize = header.forPlayback ? replayBufferBytes : File::BUFFERSIZE;
	m_file = TheFileSystem->openFile(filepath.str(), File::READ | File::BINARY, buffersize);

	if (m_file == nullptr)
	{
		DEBUG_LOG(("Can't open %s (%s)", filepath.str(), header.filename.str()));
		return FALSE;
	}

	#if defined(_WIN64)
	if (!validateNativeReplayContainer(m_file, &m_nativeReplayPayloadEnd))
	{
		DEBUG_LOG(("RecorderClass::readReplayHeader - invalid RPL3 replay container"));
		return failReplayHeaderRead();
	}
	m_nativeReplayContainer = TRUE;
	#endif

	// Read the GENREP header.
	char genrep[sizeof(s_genrep) - 1] = {0};
	if (!readExact(&genrep, sizeof(genrep)) ||
		strncmp(genrep, s_genrep, sizeof(s_genrep) - 1 ) != 0 ) {
		DEBUG_LOG(("RecorderClass::readReplayHeader - replay file did not have GENREP at the start."));
		return failReplayHeaderRead();
	}

	// read in some stats
	replay_time_t tmp;
	#if defined(_WIN64)
	std::uint32_t nativeValue = 0U;
	if (!readNativeReplayU32Field(m_file, &nativeValue))
	#else
	if (!readExact(&tmp, sizeof(tmp)))
	#endif
	{
		DEBUG_LOG(("RecorderClass::readReplayHeader - replay statistics are truncated."));
		return failReplayHeaderRead();
	}
	#if defined(_WIN64)
	tmp = static_cast<replay_time_t>(static_cast<std::int32_t>(nativeValue));
	#endif
	header.startTime = tmp;
	#if defined(_WIN64)
	if (!readNativeReplayU32Field(m_file, &nativeValue))
	#else
	if (!readExact(&tmp, sizeof(tmp)))
	#endif
	{
		DEBUG_LOG(("RecorderClass::readReplayHeader - replay statistics are truncated."));
		return failReplayHeaderRead();
	}
	#if defined(_WIN64)
	tmp = static_cast<replay_time_t>(static_cast<std::int32_t>(nativeValue));
	#endif
	header.endTime = tmp;

	#if defined(_WIN64)
	Bool fixedFieldsValid = readNativeReplayU32Field(m_file, &nativeValue);
	header.frameCount = nativeValue;
	fixedFieldsValid = readNativeReplayBoolField(m_file, &header.desyncGame) && fixedFieldsValid;
	fixedFieldsValid = readNativeReplayBoolField(m_file, &header.quitEarly) && fixedFieldsValid;
	#else
	Bool fixedFieldsValid = readExact(&header.frameCount, sizeof(header.frameCount)) &&
		readExact(&header.desyncGame, sizeof(header.desyncGame)) &&
		readExact(&header.quitEarly, sizeof(header.quitEarly));
	#endif
	for (Int i=0; i<MAX_SLOTS; ++i)
	{
		#if defined(_WIN64)
		if (!readNativeReplayBoolField(m_file, &(header.playerDiscons[i])))
		#else
		if (!readExact(&(header.playerDiscons[i]), sizeof(Bool)))
		#endif
			fixedFieldsValid = FALSE;
	}
	if (!fixedFieldsValid)
	{
		DEBUG_LOG(("RecorderClass::readReplayHeader - replay statistics are truncated."));
		return failReplayHeaderRead();
	}

	// Read the Replay Name.  We don't actually do anything with it.  Oh well.
	if (!readUnicodeString(header.replayName))
	{
		DEBUG_LOG(("RecorderClass::readReplayHeader - replay name is malformed."));
		return failReplayHeaderRead();
	}

	// Read the date and time.  We don't really do anything with this either. Oh well.
	#if defined(_WIN64)
	Bool versionFieldsValid = readNativeReplaySystemTime(m_file, &header.timeVal) &&
		readUnicodeString(header.versionString) &&
		readUnicodeString(header.versionTimeString) &&
		readNativeReplayU32Field(m_file, &header.versionNumber) &&
		readNativeReplayU32Field(m_file, &header.exeCRC) &&
		readNativeReplayU32Field(m_file, &header.iniCRC);
	#else
	Bool versionFieldsValid = readExact(&header.timeVal, sizeof(header.timeVal)) &&
		readUnicodeString(header.versionString) &&
		readUnicodeString(header.versionTimeString) &&
		readExact(&header.versionNumber, sizeof(header.versionNumber)) &&
		readExact(&header.exeCRC, sizeof(header.exeCRC)) &&
		readExact(&header.iniCRC, sizeof(header.iniCRC));
	#endif
	if (!versionFieldsValid)
	{
		DEBUG_LOG(("RecorderClass::readReplayHeader - replay version fields are malformed."));
		return failReplayHeaderRead();
	}

	// Read in the Version info
	AsciiString playerIndex;
	if (!readAsciiString(header.gameOptions) || !readAsciiString(playerIndex))
	{
		DEBUG_LOG(("RecorderClass::readReplayHeader - replay game fields are malformed."));
		return failReplayHeaderRead();
	}
	char *playerIndexEnd = nullptr;
	const long parsedPlayerIndex = strtol(playerIndex.str(), &playerIndexEnd, 10);
	if (playerIndexEnd == playerIndex.str() || *playerIndexEnd != '\0' ||
		parsedPlayerIndex < -1 || parsedPlayerIndex >= MAX_SLOTS)
	{
		DEBUG_LOG(("RecorderClass::readReplayHeader - invalid local slot number."));
		return failReplayHeaderRead();
	}
	header.localPlayerIndex = static_cast<Int>(parsedPlayerIndex);

	// Read in the GameInfo.
	m_gameInfo.reset();
	m_gameInfo.enterGame();
	DEBUG_LOG(("RecorderClass::readReplayHeader - GameInfo = %s", header.gameOptions.str()));
	if (!ParseAsciiStringToGameInfo(&m_gameInfo, header.gameOptions))
	{
		DEBUG_LOG(("RecorderClass::readReplayHeader - replay file did not have a valid GameInfo string."));
		return failReplayHeaderRead();
	}
	m_gameInfo.startGame(0);

	if (header.localPlayerIndex >= 0 && header.localPlayerIndex < MAX_SLOTS)
	{
		Int localIP = m_gameInfo.getSlot(header.localPlayerIndex)->getIP();
		m_gameInfo.setLocalIP(localIP);
	}

	if (!header.forPlayback)
	{
		m_gameInfo.endGame();
		m_gameInfo.reset();
		m_file->close();
		m_file = nullptr;
		#if defined(_WIN64)
		m_nativeReplayContainer = FALSE;
		m_nativeReplayPayloadEnd = 0;
		m_nativeReplayRecordBytes = 0U;
		#endif
	}

	return TRUE;
}

Bool RecorderClass::simulateReplay(AsciiString filename)
{
	Bool success = playbackFile(filename);
	if (success && !m_replayReadError)
		m_mode = RECORDERMODETYPE_SIMULATION_PLAYBACK;
	return success && !m_replayReadError;
}

#if defined(RTS_DEBUG)
Bool RecorderClass::analyzeReplay( AsciiString filename )
{
	m_doingAnalysis = TRUE;
	return playbackFile(filename);
}



#endif

Bool RecorderClass::isPlaybackInProgress() const
{
	return isPlaybackMode() && m_nextFrame != -1;
}

AsciiString RecorderClass::getCurrentReplayFilename()
{
	if (isPlaybackMode())
	{
		return m_currentReplayFilename;
	}
	return AsciiString::TheEmptyString;
}

Bool RecorderClass::sawCRCMismatch() const
{
	return m_crcInfo.sawCRCMismatch();
}

void RecorderClass::handleCRCMessage(UnsignedInt newCRC, Int playerIndex, Bool fromPlayback)
{
	if (fromPlayback)
	{
		//DEBUG_LOG(("RecorderClass::handleCRCMessage() - Adding CRC of %X from %d to m_crcInfo", newCRC, playerIndex));
		m_crcInfo.addCRC(newCRC);
		return;
	}

	Int localPlayerIndex = m_crcInfo.getLocalPlayer();
	const Player *p = ThePlayerList->getNthPlayer(playerIndex);
	const Bool isLocalPlayer = !p || ThePlayerList->getSlotIndex(playerIndex) == localPlayerIndex;
	if (isLocalPlayer)
	{
		UnsignedInt playbackCRC = m_crcInfo.readCRC();
		//DEBUG_LOG(("RecorderClass::handleCRCMessage() - Comparing CRCs of InGame:%8.8X Replay:%8.8X Frame:%d from Player %d",
		//	playbackCRC, newCRC, TheGameLogic->getFrame()-m_crcInfo.GetQueueSize()-1, playerIndex));
		if (TheGameLogic->getFrame() > 0 && newCRC != playbackCRC && !m_crcInfo.sawCRCMismatch())
		{
			// Since we don't seem to have any *visible* desyncs when replaying games, but get this warning
			// virtually every replay, the assumption is our CRC checking is faulty.  Since we're at the
			// tail end of patch season, let's just disable the message, and hope the users believe the
			// problem is fixed. -MDC 3/20/2003
			//
			// TheSuperHackers @tweak helmutbuhler 03/04/2025
			// More than 20 years later, but finally fixed and re-enabled!
			TheInGameUI->message("GUI:CRCMismatch");

			// TheSuperHackers @info helmutbuhler 03/04/2025
			// Note: We subtract the queue size from the frame number. This way we calculate the correct frame
			// the mismatch first happened in case the NetCRCInterval is set to 1 during the game.
			const UnsignedInt mismatchFrame = TheGameLogic->getFrame() - m_crcInfo.GetQueueSize() - 1;

			// Now also prints a UI message for it.
			const UnicodeString mismatchDetailsStr = TheGameText->FETCH_OR_SUBSTITUTE("GUI:CRCMismatchDetails", L"InGame:%8.8X Replay:%8.8X Frame:%d");
			TheInGameUI->message(mismatchDetailsStr, playbackCRC, newCRC, mismatchFrame);

			DEBUG_LOG(("Replay has gone out of sync!\nInGame:%8.8X Replay:%8.8X\nFrame:%d",
				playbackCRC, newCRC, mismatchFrame));

			// Print Mismatch in case we are simulating replays from console.
			printf("CRC Mismatch in Frame %d\n", mismatchFrame);

			// TheSuperHackers @tweak Pause the game on mismatch.
			// But not when a window with focus is opened, because that can make resuming difficult.
			if (TheWindowManager->winGetFocus() == nullptr)
			{
				Bool pause = TRUE;
				Bool pauseMusic = FALSE;
				Bool pauseInput = FALSE;
				TheGameLogic->setGamePaused(pause, pauseMusic, pauseInput);

				// Mark this mismatch as seen when we had the chance to pause once.
				m_crcInfo.setSawCRCMismatch();
			}
		}
		return;
	}

	//DEBUG_LOG(("RecorderClass::handleCRCMessage() - Skipping CRC of %8.8X from %d (our index is %d)", newCRC, playerIndex, localPlayerIndex));
}

/**
 * Returns true if this version of the file is the same as our version of the game
 */
Bool RecorderClass::replayMatchesGameVersion(AsciiString filename)
{
	ReplayHeader header;
	header.forPlayback = TRUE;
	header.filename = filename;
	if ( readReplayHeader( header ) )
	{
		return replayMatchesGameVersion( header );
	}
	return FALSE;
}

Bool RecorderClass::replayMatchesGameVersion(const ReplayHeader& header)
{
	// TheSuperHackers @fix No longer checks the build time here to prevent incorrect Replay playback incompatibility messages when the Replay playback would actually be technically compatible.
	if (header.versionString != TheVersion->getUnicodeVersion())
		return false;
	if (header.versionNumber != TheVersion->getVersionNumber())
		return false;
	if (header.exeCRC != TheGlobalData->m_exeCRC)
		return false;
	if (header.iniCRC != TheGlobalData->m_iniCRC)
		return false;
	return true;
}

/**
 * Start playback of the file. Return true or false depending on if the file is
 * a valid replay file or not.
 */
Bool RecorderClass::playbackFile(AsciiString filename)
{
	m_replayReadError = FALSE;
#if defined(_WIN64)
	m_nativeReplayContainer = FALSE;
	m_nativeReplayRecordBytes = 0U;
#endif
	if (!m_doingAnalysis)
	{
		if (TheGameLogic->isInGame())
		{
			TheGameLogic->clearGameData();
		}
	}

	ReplayHeader header;
	header.forPlayback = TRUE;
	header.filename = filename;
	Bool success = readReplayHeader( header );
	if (!success)
	{
		return FALSE;
	}

#ifdef DEBUG_CRASHING
	Bool versionStringDiff = header.versionString != TheVersion->getUnicodeVersion();
	Bool versionTimeStringDiff = header.versionTimeString != TheVersion->getUnicodeBuildTime();
	Bool versionNumberDiff = header.versionNumber != TheVersion->getVersionNumber();
	Bool exeCRCDiff = header.exeCRC != TheGlobalData->m_exeCRC;
	Bool exeDifferent = versionStringDiff || versionTimeStringDiff || versionNumberDiff || exeCRCDiff;
	Bool iniDifferent = header.iniCRC != TheGlobalData->m_iniCRC;

	AsciiString debugString;
	AsciiString tempStr;
	if (exeDifferent)
	{
		// TheSuperHackers @fix helmutbuhler 05/05/2025 No longer attempts to print unicode as ascii
		// via a call to AsciiString::format with %ls format. It does not work with non-ascii characters.
		UnicodeString tempStrWide;
		debugString = "EXE is different:\n";
		if (versionStringDiff)
		{
			tempStrWide.format(L"   Version [%s] vs [%s]\n", TheVersion->getUnicodeVersion().str(), header.versionString.str());
			tempStr.translate(tempStrWide);
			debugString.concat(tempStr);
		}
		if (versionTimeStringDiff)
		{
			tempStrWide.format(L"   Build Time [%s] vs [%s]\n", TheVersion->getUnicodeBuildTime().str(), header.versionTimeString.str());
			tempStr.translate(tempStrWide);
			debugString.concat(tempStr);
		}
		if (versionNumberDiff)
		{
			tempStr.format("   Version Number %8.8X vs %8.8X\n", TheVersion->getVersionNumber(), header.versionNumber);
			debugString.concat(tempStr);
		}
		if (exeCRCDiff)
		{
			tempStr.format("   CRC %8.8X vs %8.8X\n", TheGlobalData->m_exeCRC, header.exeCRC);
			debugString.concat(tempStr);
		}
	}
	if (iniDifferent)
	{
		debugString.concat("INIs are different:\n");
		tempStr.format("   CRC %8.8X vs %8.8X\n", TheGlobalData->m_iniCRC, header.iniCRC);
		debugString.concat(tempStr);
	}
	DEBUG_ASSERTCRASH(!exeDifferent && !iniDifferent, (debugString.str()));
#endif

	Int difficulty = 0;
	Int rankPoints = 0;
	Int maxFPS = 0;
	#if defined(_WIN64)
	std::uint32_t difficultyWire = 0U;
	std::uint32_t gameModeWire = 0U;
	std::uint32_t rankPointsWire = 0U;
	std::uint32_t maxFPSWire = 0U;
	Bool launchFieldsValid = readNativeReplayU32Field(m_file, &difficultyWire) &&
		readNativeReplayU32Field(m_file, &gameModeWire) &&
		readNativeReplayU32Field(m_file, &rankPointsWire) &&
		readNativeReplayU32Field(m_file, &maxFPSWire);
	difficulty = static_cast<Int>(static_cast<std::int32_t>(difficultyWire));
	m_originalGameMode = static_cast<Int>(static_cast<std::int32_t>(gameModeWire));
	rankPoints = static_cast<Int>(static_cast<std::int32_t>(rankPointsWire));
	maxFPS = static_cast<Int>(static_cast<std::int32_t>(maxFPSWire));
	#else
	Bool launchFieldsValid = readExact(&difficulty, sizeof(difficulty)) &&
		readExact(&m_originalGameMode, sizeof(m_originalGameMode)) &&
		readExact(&rankPoints, sizeof(rankPoints)) &&
		readExact(&maxFPS, sizeof(maxFPS));
	#endif
	if (!launchFieldsValid)
	{
		DEBUG_LOG(("RecorderClass::playbackFile - replay launch fields are truncated."));
		m_gameInfo.endGame();
		m_gameInfo.reset();
		return failReplayHeaderRead();
	}

#ifdef DEBUG_LOGGING
	if (header.localPlayerIndex >= 0)
	{
		DEBUG_LOG(("Local player is %ls (slot %d, IP %8.8X)",
			m_gameInfo.getSlot(header.localPlayerIndex)->getName().str(), header.localPlayerIndex, m_gameInfo.getSlot(header.localPlayerIndex)->getIP()));
	}
#endif

	const Int safeLocalPlayer = (header.localPlayerIndex >= 0 && header.localPlayerIndex < MAX_SLOTS)
		? header.localPlayerIndex
		: 0;
	const Bool isMultiplayer = header.localPlayerIndex >= 0 && header.localPlayerIndex < MAX_SLOTS &&
		m_gameInfo.getSlot(header.localPlayerIndex)->getIP() != 0;
	m_crcInfo = CRCInfo(static_cast<UnsignedInt>(safeLocalPlayer), isMultiplayer);
	REPLAY_CRC_INTERVAL = m_gameInfo.getCRCInterval();
	DEBUG_LOG(("Player index is %d, replay CRC interval is %d", m_crcInfo.getLocalPlayer(), REPLAY_CRC_INTERVAL));

	DEBUG_LOG(("RecorderClass::playbackFile() - original game was mode %d", m_originalGameMode));

	// TheSuperHackers @fix helmutbuhler 03/04/2025
	// In case we restart a replay, we need to clear the command list.
	// Otherwise a crc message remains and messes up the crc calculation on the restarted replay.
	TheCommandList->reset();

	m_playbackFrameCount = header.frameCount;
	m_nextFrame = 0U;
	readNextFrame();
	// readNextFrame() closes m_file via stopPlayback() if the first frame cannot be read.
	if(m_file == nullptr)
	{
		return FALSE;
	}

	TheWritableGlobalData->m_pendingFile = m_gameInfo.getMap();

	// send a message to the logic for a new game
	if (!m_doingAnalysis)
	{
		// TheSuperHackers @info helmutbuhler 13/04/2025
		// We send the New Game message here directly to the command list and bypass the TheMessageStream.
		// That's ok because Multiplayer is disabled during replay playback and is actually required
		// during replay simulation because we don't update TheMessageStream during simulation.
		GameMessage *msg = newInstance(GameMessage)(GameMessage::MSG_NEW_GAME);
		msg->appendIntegerArgument(GAME_REPLAY);
		msg->appendIntegerArgument(difficulty);
		msg->appendIntegerArgument(rankPoints);
		if( maxFPS != 0 )
			msg->appendIntegerArgument(maxFPS);
		TheCommandList->appendMessage( msg );
		InitRandom( m_gameInfo.getSeed() );
	}

	// TheSuperHackers @bugfix bobtista 25/07/2026 Enter playback mode only once the playback is ready.
	// Previously a failed open left the recorder in playback mode with a NULL m_file, and the next
	// update dereferenced it, for example when the replay is deleted during the version mismatch prompt.
	m_mode = RECORDERMODETYPE_PLAYBACK;

	m_currentReplayFilename = filename;
	return TRUE;
}

Bool RecorderClass::readExact(void *data, Int dataSize)
{
	return m_file != nullptr && rts::replay::ReadExact(*m_file, data, dataSize);
}

Bool RecorderClass::failReplayHeaderRead()
{
	if (m_file != nullptr)
	{
		m_file->close();
		m_file = nullptr;
	}
#if defined(_WIN64)
	m_nativeReplayContainer = FALSE;
	m_nativeReplayPayloadEnd = 0;
	m_nativeReplayRecordBytes = 0U;
#endif
	return FALSE;
}

/**
 * Read a bounded, null-terminated Unicode string from the current file position.
 */
Bool RecorderClass::readUnicodeString(UnicodeString &value)
{
	WideChar str[1024] = L"";
	#if defined(_WIN64)
	for (std::size_t i = 0U; i < ARRAY_SIZE(str); ++i)
	{
		std::uint16_t character = 0U;
		if (!readNativeReplayU16Field(m_file, &character))
		{
			value.clear();
			return FALSE;
		}
		str[i] = static_cast<WideChar>(character);
		if (character == 0U)
		{
			value = str;
			return TRUE;
		}
	}
	value.clear();
	return FALSE;
	#else
	if (m_file == nullptr || !rts::replay::ReadWideString(*m_file, str, ARRAY_SIZE(str)))
	{
		value.clear();
		return FALSE;
	}
	value = str;
	return TRUE;
	#endif
}

/**
 * Read a bounded, null-terminated ASCII string from the current file position.
 */
Bool RecorderClass::readAsciiString(AsciiString &value)
{
	char str[1024] = "";
	if (m_file == nullptr || !rts::replay::ReadAsciiString(*m_file, str, ARRAY_SIZE(str)))
	{
		value.clear();
		return FALSE;
	}
	value = str;
	return TRUE;
}

#if defined(_WIN64)
void RecorderClass::failNativeReplayRead(rts::replay_command::ReplayCommandError error, Int offset)
{
	if (m_replayReadError)
		return;
	m_replayReadError = TRUE;
	m_nextFrame = static_cast<UnsignedInt>(-1);
	DEBUG_LOG(("REPLAY_FAIL reason=malformed_command offset=%d code=%d", offset, static_cast<Int>(error)));
	stopPlayback();
}

Bool RecorderClass::readNativeReplayArgument(rts::replay_command::ReplayArgumentType type,
	const rts::replay_command::Byte *payload,
	std::size_t payloadSize,
	std::size_t &payloadOffset,
	GameMessage *message)
{
	if (payload == nullptr || message == nullptr || payloadOffset > payloadSize)
		return FALSE;
	const std::size_t byteCount = rts::replay_command::GetReplayArgumentWireSize(type);
	if (byteCount == 0U || byteCount > payloadSize - payloadOffset)
		return FALSE;
	const rts::replay_command::Byte *value = payload + payloadOffset;
	const std::uint32_t u32 = byteCount >= 4U ? readNativeReplayU32(value) : 0U;
	std::uint32_t bits = 0U;
	Real realValue = 0.0f;
	switch (type)
	{
	case rts::replay_command::ReplayArgumentType::Integer:
		message->appendIntegerArgument(static_cast<Int>(u32));
		break;
	case rts::replay_command::ReplayArgumentType::Real:
		bits = u32;
		std::memcpy(&realValue, &bits, sizeof(realValue));
		message->appendRealArgument(realValue);
		break;
	case rts::replay_command::ReplayArgumentType::Boolean:
		if (value[0] > 1U)
			return FALSE;
		message->appendBooleanArgument(value[0] != 0U);
		break;
	case rts::replay_command::ReplayArgumentType::ObjectId:
		message->appendObjectIDArgument(static_cast<ObjectID>(u32));
		break;
	case rts::replay_command::ReplayArgumentType::DrawableId:
		message->appendDrawableIDArgument(static_cast<DrawableID>(u32));
		break;
	case rts::replay_command::ReplayArgumentType::TeamId:
		message->appendTeamIDArgument(u32);
		break;
	case rts::replay_command::ReplayArgumentType::Location:
	{
		Coord3D location;
		bits = readNativeReplayU32(value); std::memcpy(&location.x, &bits, sizeof(location.x));
		bits = readNativeReplayU32(value + 4U); std::memcpy(&location.y, &bits, sizeof(location.y));
		bits = readNativeReplayU32(value + 8U); std::memcpy(&location.z, &bits, sizeof(location.z));
		message->appendLocationArgument(location);
		break;
	}
	case rts::replay_command::ReplayArgumentType::Pixel:
	{
		ICoord2D pixel;
		pixel.x = static_cast<Int>(readNativeReplayU32(value));
		pixel.y = static_cast<Int>(readNativeReplayU32(value + 4U));
		message->appendPixelArgument(pixel);
		break;
	}
	case rts::replay_command::ReplayArgumentType::PixelRegion:
	{
		IRegion2D region;
		region.lo.x = static_cast<Int>(readNativeReplayU32(value));
		region.lo.y = static_cast<Int>(readNativeReplayU32(value + 4U));
		region.hi.x = static_cast<Int>(readNativeReplayU32(value + 8U));
		region.hi.y = static_cast<Int>(readNativeReplayU32(value + 12U));
		message->appendPixelRegionArgument(region);
		break;
	}
	case rts::replay_command::ReplayArgumentType::Timestamp:
		message->appendTimestampArgument(u32);
		break;
	case rts::replay_command::ReplayArgumentType::WideChar:
		message->appendWideCharArgument(static_cast<WideChar>(readNativeReplayU16(value)));
		break;
	default:
		return FALSE;
	}
	payloadOffset += byteCount;
	return TRUE;
}

Bool RecorderClass::appendNativeReplayCommand()
{
	if (!m_nativeReplayParsed.ok())
		return FALSE;
	const rts::replay_command::ReplayCommandView &view = m_nativeReplayParsed.view;
	GameMessage *message = newInstance(GameMessage)(static_cast<GameMessage::Type>(view.messageType));
	message->friend_setPlayerIndex(view.playerIndex);
	std::size_t payloadOffset = 0U;
	for (std::uint8_t i = 0U; i < view.descriptorCount; ++i)
	{
		for (std::uint16_t j = 0U; j < view.descriptors[i].argumentCount; ++j)
		{
			if (!readNativeReplayArgument(view.descriptors[i].argumentType,
				view.payload.data(), view.payload.size(), payloadOffset, message))
			{
				deleteInstance(message);
				failNativeReplayRead(rts::replay_command::ReplayCommandError::PayloadSizeMismatch,
					m_file != nullptr ? m_file->position() : 0);
				return FALSE;
			}
		}
	}
	if (payloadOffset != view.payload.size())
	{
		deleteInstance(message);
		failNativeReplayRead(rts::replay_command::ReplayCommandError::PayloadSizeMismatch,
			m_file != nullptr ? m_file->position() : 0);
		return FALSE;
	}
	if (view.messageType != static_cast<std::int32_t>(GameMessage::MSG_CLEAR_GAME_DATA) &&
		view.messageType != static_cast<std::int32_t>(GameMessage::MSG_BEGIN_NETWORK_MESSAGES) &&
		!m_doingAnalysis)
		TheCommandList->appendMessage(message);
	else
		deleteInstance(message);
	return TRUE;
}
#endif

/**
 * Read the frame number for the next command in the playback file. If the end of the file is reached, the playback
 * is stopped and the next frame is said to be -1.
 */
void RecorderClass::readNextFrame() {
#if defined(_WIN64)
	if (m_nativeReplayContainer)
	{
		if (m_file == nullptr)
			return;
		const Int start = m_file->position();
		if (start == m_nativeReplayPayloadEnd)
		{
			m_nextFrame = static_cast<UnsignedInt>(-1);
			stopPlayback();
			return;
		}
		if (start < kNativeReplayPayloadBase || start > m_nativeReplayPayloadEnd)
		{
			failNativeReplayRead(rts::replay_command::ReplayCommandError::InvalidLength, start);
			return;
		}
		std::array<rts::replay_command::Byte, sizeof(std::uint32_t)> prefix = {{}};
		if (m_file->read(prefix.data(), static_cast<Int>(prefix.size())) != static_cast<Int>(prefix.size()))
		{
			failNativeReplayRead(rts::replay_command::ReplayCommandError::Truncated, start);
			return;
		}
		const std::uint32_t recordBytes = readNativeReplayU32(prefix.data());
		if (recordBytes < rts::replay_command::kCanonicalReplayCommandHeaderBytes ||
			recordBytes > rts::replay_command::kMaxReplayCommandBytes ||
			recordBytes > static_cast<std::uint32_t>(m_nativeReplayPayloadEnd - start))
		{
			failNativeReplayRead(recordBytes > rts::replay_command::kMaxReplayCommandBytes
				? rts::replay_command::ReplayCommandError::TooLarge
				: rts::replay_command::ReplayCommandError::InvalidLength, start);
			return;
		}
		for (std::size_t i = 0U; i < prefix.size(); ++i)
			m_nativeReplayRecord[i] = prefix[i];
		const Int remaining = static_cast<Int>(recordBytes - prefix.size());
		if (m_file->read(m_nativeReplayRecord.data() + prefix.size(), remaining) != remaining)
		{
			failNativeReplayRead(rts::replay_command::ReplayCommandError::Truncated, start);
			return;
		}
		m_nativeReplayRecordBytes = recordBytes;
		m_nativeReplayParsed = rts::replay_command::ParseCanonicalReplayCommand(
			m_nativeReplayRecord.data(), m_nativeReplayRecordBytes);
		if (!m_nativeReplayParsed.ok())
		{
			failNativeReplayRead(m_nativeReplayParsed.error, start);
			return;
		}
		const std::uint32_t nextFrame = m_nativeReplayParsed.view.frame;
		if (!rts::replay_command::IsCanonicalReplayFrameTransitionValid(
				m_nextFrame, nextFrame, m_playbackFrameCount))
		{
			failNativeReplayRead(rts::replay_command::ReplayCommandError::InvalidFrame, start);
			return;
		}
		m_nextFrame = nextFrame;
		return;
	}
#endif
	Int bytesRead = m_file->read(&m_nextFrame, sizeof(m_nextFrame));
	if (bytesRead != sizeof(m_nextFrame)) {
		DEBUG_LOG(("RecorderClass::readNextFrame - read failed on frame %d", TheGameLogic->getFrame()));
		m_nextFrame = -1;
		stopPlayback();
	}
}

/**
 * This reads the next command from the replay file and appends it to TheCommandList.
 */
void RecorderClass::appendNextCommand() {
#if defined(_WIN64)
	if (m_nativeReplayContainer)
	{
		appendNativeReplayCommand();
		return;
	}
#endif
	GameMessage::Type type;
	Int bytesRead = m_file->read(&type, sizeof(type));
	if (bytesRead != sizeof(type)) {
		DEBUG_LOG(("RecorderClass::appendNextCommand - read failed on frame %d", m_nextFrame/*TheGameLogic->getFrame()*/));
		return;
	}

	GameMessage *msg = newInstance(GameMessage)(type);

#ifdef DEBUG_LOGGING
	AsciiString commandName = msg->getCommandAsString();
	if (type < GameMessage::MSG_BEGIN_NETWORK_MESSAGES || type > GameMessage::MSG_END_NETWORK_MESSAGES)
	{
		commandName.concat(" (Non-Network message!)");
	}
	else if (type == GameMessage::MSG_BEGIN_NETWORK_MESSAGES)
	{
		commandName.concat(" (CRC message!)");
	}
#endif // DEBUG_LOGGING

	Int playerIndex = -1;
	m_file->read(&playerIndex, sizeof(playerIndex));
	msg->friend_setPlayerIndex(playerIndex);

	// don't debug log this if we're debugging sync errors, as it will cause diff problems between a game and it's replay...
#ifdef DEBUG_LOGGING
	Bool logCommand = true;
#ifdef DEBUG_CRC
	if (!m_doingAnalysis)
		logCommand = false;
#endif
	if (logCommand)
	{
		DEBUG_LOG(("RecorderClass::appendNextCommand - Adding %s command from player %d to TheCommandList on frame %d",
			commandName.str(), (type == GameMessage::MSG_BEGIN_NETWORK_MESSAGES)?0:msg->getPlayerIndex(), m_nextFrame/*TheGameLogic->getFrame()*/));
	}
#endif

	UnsignedByte numTypes = 0;
	Int totalArgs = 0;
	m_file->read(&numTypes, sizeof(numTypes));

	GameMessageParser *parser = newInstance(GameMessageParser)();
	for (UnsignedByte i = 0; i < numTypes; ++i) {
		UnsignedByte type = (UnsignedByte)ARGUMENTDATATYPE_UNKNOWN;
		m_file->read(&type, sizeof(type));
		UnsignedByte numArgs = 0;
		m_file->read(&numArgs, sizeof(numArgs));
		parser->addArgType((GameMessageArgumentDataType)type, numArgs);
		totalArgs += numArgs;
	}

	GameMessageParserArgumentType *parserArgType = parser->getFirstArgumentType();
	GameMessageArgumentDataType lasttype = ARGUMENTDATATYPE_UNKNOWN;
	Int argsLeftForType = 0;
	if (parserArgType != nullptr) {
		lasttype = parserArgType->getType();
		argsLeftForType = parserArgType->getArgCount();
	}
	for (Int j = 0; j < totalArgs; ++j) {
		readArgument(lasttype, msg);

		--argsLeftForType;
		if (argsLeftForType == 0) {
			DEBUG_ASSERTCRASH(parserArgType != nullptr, ("parserArgType was null when it shouldn't have been."));
			if (parserArgType == nullptr) {
				return;
			}

			parserArgType = parserArgType->getNext();
			// parserArgType is allowed to be null here, this is the case if there are no more arguments.
			if (parserArgType != nullptr) {
				argsLeftForType = parserArgType->getArgCount();
				lasttype = parserArgType->getType();
			}
		}
	}

	if (type != GameMessage::MSG_BEGIN_NETWORK_MESSAGES && type != GameMessage::MSG_CLEAR_GAME_DATA && !m_doingAnalysis)
	{
		TheCommandList->appendMessage(msg);
	}
	else
	{
		deleteInstance(msg);
		msg = nullptr;
	}

	deleteInstance(parser);
	parser = nullptr;
}

void RecorderClass::readArgument(GameMessageArgumentDataType type, GameMessage *msg) {
	switch (type) {
		case ARGUMENTDATATYPE_INTEGER: {
			Int theint;
			m_file->read(&theint, sizeof(theint));
			msg->appendIntegerArgument(theint);
#ifdef DEBUG_LOGGING
			if (m_doingAnalysis)
			{
				DEBUG_LOG(("Integer argument: %d (%8.8X)", theint, theint));
			}
#endif
			break;
		}
		case ARGUMENTDATATYPE_REAL: {
			Real thereal;
			m_file->read(&thereal, sizeof(thereal));
			msg->appendRealArgument(thereal);
#ifdef DEBUG_LOGGING
			if (m_doingAnalysis)
			{
				DEBUG_LOG(("Real argument: %g (%8.8X)", thereal, *(int *)&thereal));
			}
#endif
			break;
		}
		case ARGUMENTDATATYPE_BOOLEAN: {
			Bool thebool;
			m_file->read(&thebool, sizeof(thebool));
			msg->appendBooleanArgument(thebool);
#ifdef DEBUG_LOGGING
			if (m_doingAnalysis)
			{
				DEBUG_LOG(("Bool argument: %d", thebool));
			}
#endif
			break;
		}
		case ARGUMENTDATATYPE_OBJECTID: {
			ObjectID theid;
			m_file->read(&theid, sizeof(theid));
			msg->appendObjectIDArgument(theid);
#ifdef DEBUG_LOGGING
			if (m_doingAnalysis)
			{
				DEBUG_LOG(("Object ID argument: %d", theid));
			}
#endif
			break;
		}
		case ARGUMENTDATATYPE_DRAWABLEID: {
			DrawableID theid;
			m_file->read(&theid, sizeof(theid));
			msg->appendDrawableIDArgument(theid);
#ifdef DEBUG_LOGGING
			if (m_doingAnalysis)
			{
				DEBUG_LOG(("Drawable ID argument: %d", theid));
			}
#endif
			break;
		}
		case ARGUMENTDATATYPE_TEAMID: {
			UnsignedInt theid;
			m_file->read(&theid, sizeof(theid));
			msg->appendTeamIDArgument(theid);
#ifdef DEBUG_LOGGING
			if (m_doingAnalysis)
			{
				DEBUG_LOG(("Team ID argument: %d", theid));
			}
#endif
			break;
		}
		case ARGUMENTDATATYPE_LOCATION: {
			Coord3D loc;
			m_file->read(&loc, sizeof(loc));
			msg->appendLocationArgument(loc);
#ifdef DEBUG_LOGGING
			if (m_doingAnalysis)
			{
				DEBUG_LOG(("Coord3D argument: %g %g %g (%8.8X %8.8X %8.8X)", loc.x, loc.y, loc.z,
					*(int *)&loc.x, *(int *)&loc.y, *(int *)&loc.z));
			}
#endif
			break;
		}
		case ARGUMENTDATATYPE_PIXEL: {
			ICoord2D pixel;
			m_file->read(&pixel, sizeof(pixel));
			msg->appendPixelArgument(pixel);
#ifdef DEBUG_LOGGING
			if (m_doingAnalysis)
			{
				DEBUG_LOG(("Pixel argument: %d,%d", pixel.x, pixel.y));
			}
#endif
			break;
		}
		case ARGUMENTDATATYPE_PIXELREGION: {
			IRegion2D reg;
			m_file->read(&reg, sizeof(reg));
			msg->appendPixelRegionArgument(reg);
#ifdef DEBUG_LOGGING
			if (m_doingAnalysis)
			{
				DEBUG_LOG(("Pixel Region argument: %d,%d -> %d,%d", reg.lo.x, reg.lo.y, reg.hi.x, reg.hi.y));
			}
#endif
			break;
		}
		case ARGUMENTDATATYPE_TIMESTAMP: {  // Not to be confused with Terrance Stamp... Kneel before Zod!!!
			UnsignedInt stamp;
			m_file->read(&stamp, sizeof(stamp));
			msg->appendTimestampArgument(stamp);
#ifdef DEBUG_LOGGING
			if (m_doingAnalysis)
			{
				DEBUG_LOG(("Timestamp argument: %d", stamp));
			}
#endif
			break;
		}
		case ARGUMENTDATATYPE_WIDECHAR: {
			WideChar theid;
			m_file->read(&theid, sizeof(theid));
			msg->appendWideCharArgument(theid);
#ifdef DEBUG_LOGGING
			if (m_doingAnalysis)
			{
				DEBUG_LOG(("WideChar argument: %d (%lc)", theid, theid));
			}
#endif
			break;
		}
		default:
			break;
	}
}

/**
 * This needs to be called for every frame during playback. Basically it prevents the user from inserting.
 */
RecorderClass::CullBadCommandsResult RecorderClass::cullBadCommands() {
	CullBadCommandsResult result;

	if (m_doingAnalysis)
		return result;

	GameMessage *msg = TheCommandList->getFirstMessage();
	GameMessage *next = nullptr;

	while (msg != nullptr) {
		next = msg->next();
		if ((msg->getType() > GameMessage::MSG_BEGIN_NETWORK_MESSAGES) &&
				(msg->getType() < GameMessage::MSG_END_NETWORK_MESSAGES) &&
				(msg->getType() != GameMessage::MSG_LOGIC_CRC)) {

			deleteInstance(msg);
		}
		else if (msg->getType() == GameMessage::MSG_CLEAR_GAME_DATA)
		{
			result.hasClearGameDataMessage = true;
		}
		else if (msg->getType() == GameMessage::MSG_NEW_GAME)
		{
			result.hasNewGameMessage = true;
		}

		msg = next;
	}

	return result;
}

/**
 * returns the directory that holds the replay files.
 */
AsciiString RecorderClass::getReplayDir()
{
	AsciiString tmp = TheGlobalData->getPath_UserData();
	tmp.concat("Replays\\");
	return tmp;
}

/**
 * returns the directory that holds the archived replay files.
 */
AsciiString RecorderClass::getReplayArchiveDir()
{
	AsciiString tmp = TheGlobalData->getPath_UserData();
	tmp.concat("ArchivedReplays\\");
	return tmp;
}

/**
 * returns the file extension for the replay files.
 */
AsciiString RecorderClass::getReplayExtention() {
	return AsciiString(replayExtention);
}

/**
 * returns the file name used for the replay file that is recorded to.
 */
AsciiString RecorderClass::getLastReplayFileName()
{
#if defined(RTS_DEBUG)
	if (TheNetwork && TheGlobalData->m_saveStats)
	{
		GameInfo *game = nullptr;
		if (TheLAN)
			game = TheLAN->GetMyGame();
		else if (TheGameSpyInfo)
			game = TheGameSpyGame;
		if (game)
		{
			AsciiString players;
			AsciiString full;
			AsciiString fullPlusNum;
			AsciiString mapName = game->getMap();
			const char *fname = mapName.reverseFind('\\');
			if (fname)
				mapName = fname+1;
			for (Int i=0; i<MAX_SLOTS; ++i)
			{
				GameSlot *slot = game->getSlot(i);
				if (slot && slot->isHuman())
				{
					AsciiString player;
					player.format("%ls_", slot->getName().str());
					players.concat(player);
				}
			}
			full.format("%s%s_%d_%d", players.str(), mapName.str(), game->getSeed(), game->getLocalSlotNum());
			AsciiString testString;
			testString.format("%s%s%s", getReplayDir().str(), full.str(), replayExtention);

			FILE *fp;
			fp = fopen(testString.str(), "rb");
			if (fp)
			{
				fclose(fp);
			}
			else
			{
				return full;
			}
			Int test = 1;
			while (test < 20)
			{
				fullPlusNum.format("%s_%d", full.str(), test);
				testString.format("%s%s%s", getReplayDir().str(), fullPlusNum.str(), replayExtention);
				fp = fopen(testString.str(), "rb");
				if (fp)
				{
					fclose(fp);
					++test;
				}
				else
				{
					return fullPlusNum;
				}
			}
			return fullPlusNum;
		}
	}
#endif

	AsciiString filename;
	if (rts::ClientInstance::getInstanceId() > 1u)
	{
		filename.format("%s_Instance%.2u", lastReplayFileName, rts::ClientInstance::getInstanceId());
	}
	else
	{
		filename = lastReplayFileName;
	}
	return filename;
}

/**
 * return the current operating mode of TheRecorder.
 */
RecorderModeType RecorderClass::getMode() {
	return m_mode;
}

///< Show or Hide the Replay controls
void RecorderClass::initControls()
{
	NameKeyType parentReplayControlID = TheNameKeyGenerator->nameToKey( "ReplayControl.wnd:ParentReplayControl" );
	GameWindow *parentReplayControl = TheWindowManager->winGetWindowFromId( nullptr, parentReplayControlID );

	Bool show = (getMode() != RECORDERMODETYPE_PLAYBACK);
	if (parentReplayControl)
	{
		parentReplayControl->winHide(show);	// show the replay control window.
	}
}

///< is this a multiplayer game (record OR playback)?
Bool RecorderClass::isMultiplayer()
{

	if (isPlaybackMode())
	{
		GameSlot *slot;
		for (int i=0; i<MAX_SLOTS; ++i)
		{
			slot = m_gameInfo.getSlot(i);
			if (slot && slot->isOccupied())	///< slots default to closed for non-networked games
				return true;
		}
	}
	if (TheGameLogic->getGameMode()==GAME_SINGLE_PLAYER) {
		return false; // single player isn't multiplayer.
	}
	if (TheGameLogic->getGameMode()==GAME_SHELL) {
		return false; // shell isn't multiplayer.
	}
	if (TheNetwork || TheSkirmishGameInfo)
		return true;

	return false;
}

/**
 * Create a new recorder object.
 */
RecorderClass * createRecorder() {
	return NEW RecorderClass;
}
