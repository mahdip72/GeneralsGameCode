/*
**	Command & Conquer Generals Zero Hour(tm)
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

#include "Compression.h"
#include "WWLib/strtok_r.h"
#include "Common/AudioEventRTS.h"
#include "Common/CRCDebug.h"
#include "Common/Debug.h"
#include "Common/file.h"
#include "Common/FileSystem.h"
#include "Common/GameAudio.h"
#include "Common/LocalFileSystem.h"
#include "Common/Player.h"
#include "Common/PlayerList.h"
#include "Common/RandomValue.h"
#include "Common/Recorder.h"

#include "GameClient/Diplomacy.h"
#include "GameClient/GameText.h"
#include "GameClient/MessageBox.h"
#include "GameNetwork/ConnectionManager.h"
#include "GameNetwork/LANAPICallbacks.h"
#include "GameNetwork/NAT.h"
#include "GameNetwork/NetCommandValidation.h"
#include "GameNetwork/NetCommandWrapperList.h"
#if defined(_WIN64)
#include "GameNetwork/InstalledLockstepV2Validation.h"
#include "Lib/LockstepV2Contract.h"
#include "Lib/NetworkCommandOriginPolicy.h"
#include "Lib/NetworkEpochHandshake.h"
#include "Lib/MultiplayerSimulationRuntimeProof.h"
#include <bcrypt.h>
#include <vector>

#endif
#include "GameNetwork/networkutil.h"
#include "GameLogic/GameLogic.h"
#include "GameLogic/ScriptActions.h"
#include "GameLogic/ScriptEngine.h"
#include "GameLogic/VictoryConditions.h"
#include "GameClient/DisconnectMenu.h"
#include "GameClient/InGameUI.h"
#include "WWLib/TARGA.h"

static Bool hasValidTransferFileExtension(const AsciiString& filePath)
{
	static const char* const validExtensions[] = {
		"map",
		"ini",
		"str",
		"wak",
		"tga",
		"txt"
	};

	const char* fileExt = strrchr(filePath.str(), '.');

	if (fileExt == nullptr || fileExt[1] == '\0')
	{
		return false;
	}

	fileExt++;

	for (Int i = 0; i < ARRAY_SIZE(validExtensions); ++i)
	{
		if (stricmp(fileExt, validExtensions[i]) == 0)
		{
			return true;
		}
	}

	return false;
}

enum TransferFileType
{
	TransferFileType_Invalid = -1,
	TransferFileType_Map,
	TransferFileType_Ini,
	TransferFileType_Str,
	TransferFileType_Txt,
	TransferFileType_Tga,
	TransferFileType_Wak,
	TransferFileType_Count
};

#if defined(_WIN64)
static Bool getCurrentExecutablePath(WideChar *path, UnsignedInt pathCount)
{
	if (path == nullptr || pathCount < 2)
		return FALSE;
	const DWORD length = GetModuleFileNameW(nullptr, path, pathCount);
	return length > 0 && length < pathCount;
}

static Bool calculateFileSha256(const WideChar *path, char output[65])
{
	if (path == nullptr || output == nullptr)
		return FALSE;
	output[0] = '\0';
	HANDLE file = CreateFileW(path, GENERIC_READ,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
		OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
	if (file == INVALID_HANDLE_VALUE)
		return FALSE;

	BCRYPT_ALG_HANDLE algorithm = nullptr;
	BCRYPT_HASH_HANDLE hash = nullptr;
	DWORD objectLength = 0;
	DWORD digestLength = 0;
	DWORD propertyBytes = 0;
	Bool success = FALSE;
	if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM,
		nullptr, 0) != 0 ||
		BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
			reinterpret_cast<PUCHAR>(&objectLength), sizeof(objectLength),
			&propertyBytes, 0) != 0 || objectLength == 0 ||
		BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH,
			reinterpret_cast<PUCHAR>(&digestLength), sizeof(digestLength),
			&propertyBytes, 0) != 0 || digestLength != 32)
	{
		if (algorithm != nullptr)
			BCryptCloseAlgorithmProvider(algorithm, 0);
		CloseHandle(file);
		return FALSE;
	}

	std::vector<UnsignedByte> objectBuffer(objectLength);
	std::vector<UnsignedByte> digest(digestLength);
	std::vector<UnsignedByte> readBuffer(64U * 1024U);
	if (BCryptCreateHash(algorithm, &hash, objectBuffer.data(), objectLength,
		nullptr, 0, 0) == 0)
	{
		for (;;)
		{
			DWORD bytesRead = 0;
			if (!ReadFile(file, readBuffer.data(),
				static_cast<DWORD>(readBuffer.size()), &bytesRead, nullptr))
			{
				break;
			}
			if (bytesRead == 0)
			{
				success = BCryptFinishHash(hash, digest.data(), digestLength, 0) == 0;
				break;
			}
			if (BCryptHashData(hash, readBuffer.data(), bytesRead, 0) != 0)
				break;
		}
	}
	if (success)
	{
		static const char HEX[] = "0123456789ABCDEF";
		for (UnsignedInt index = 0; index < digestLength; ++index)
		{
			output[index * 2] = HEX[digest[index] >> 4];
			output[index * 2 + 1] = HEX[digest[index] & 0x0f];
		}
		output[64] = '\0';
	}
	if (hash != nullptr)
		BCryptDestroyHash(hash);
	BCryptCloseAlgorithmProvider(algorithm, 0);
	CloseHandle(file);
	return success;
}

static Bool readRuntimeProofDocument(const WideChar *executablePath,
	std::vector<char> &document)
{
	if (executablePath == nullptr)
		return FALSE;
	WideChar proofPath[MAX_PATH];
	wcscpy_s(proofPath, executablePath);
	WideChar *separator = wcsrchr(proofPath, L'\\');
	if (separator == nullptr)
		return FALSE;
	separator[1] = L'\0';
	static const WideChar PROOF_NAME[] =
		L"MultiplayerSimulationRuntimeProof.txt";
	if (wcslen(proofPath) + ARRAY_SIZE(PROOF_NAME) > ARRAY_SIZE(proofPath))
		return FALSE;
	wcscat_s(proofPath, PROOF_NAME);

	HANDLE file = CreateFileW(proofPath, GENERIC_READ, FILE_SHARE_READ,
		nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
		nullptr);
	if (file == INVALID_HANDLE_VALUE)
		return FALSE;
	LARGE_INTEGER size;
	if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 ||
		size.QuadPart > 4096)
	{
		CloseHandle(file);
		return FALSE;
	}
	document.resize(static_cast<size_t>(size.QuadPart));
	DWORD bytesRead = 0;
	const Bool success = ReadFile(file, document.data(),
		static_cast<DWORD>(document.size()), &bytesRead, nullptr) &&
		bytesRead == document.size();
	CloseHandle(file);
	return success;
}

static Bool buildRuntimeProofSiblingPath(const WideChar *executablePath,
	const WideChar *relativePath, WideChar output[MAX_PATH])
{
	if (executablePath == nullptr || relativePath == nullptr || output == nullptr)
		return FALSE;
	wcscpy_s(output, MAX_PATH, executablePath);
	WideChar *separator = wcsrchr(output, L'\\');
	if (separator == nullptr)
		return FALSE;
	separator[1] = L'\0';
	const size_t rootLength = wcslen(output);
	const size_t relativeLength = wcslen(relativePath);
	if (rootLength + relativeLength >= MAX_PATH)
		return FALSE;
	wcscat_s(output, MAX_PATH, relativePath);
	return TRUE;
}

static Bool readBoundedRuntimeProofFile(const WideChar *path,
	UnsignedInt maximumBytes, std::vector<char> &document)
{
	HANDLE file = CreateFileW(path, GENERIC_READ,
		FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
	if (file == INVALID_HANDLE_VALUE)
		return FALSE;
	LARGE_INTEGER size;
	if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 ||
		size.QuadPart > maximumBytes)
	{
		CloseHandle(file);
		return FALSE;
	}
	document.resize(static_cast<size_t>(size.QuadPart));
	DWORD bytesRead = 0;
	const Bool success = ReadFile(file, document.data(),
		static_cast<DWORD>(document.size()), &bytesRead, nullptr) &&
		bytesRead == document.size();
	CloseHandle(file);
	return success;
}

static Bool verifyRuntimeProofFileSha256(const WideChar *executablePath,
	const WideChar *relativePath, const std::string &expectedSha256)
{
	WideChar path[MAX_PATH];
	char actualSha256[65];
	return buildRuntimeProofSiblingPath(executablePath, relativePath, path) &&
		calculateFileSha256(path, actualSha256) &&
		expectedSha256 == actualSha256;
}

static Bool isSafeRuntimeProofRelativePath(const std::string &path)
{
	return !path.empty() && path.size() < MAX_PATH && path[0] != '\\' &&
		path[0] != '/' && path.find(':') == std::string::npos &&
		path.find("..") == std::string::npos &&
		path.compare(0, 8, "Net3Raw\\") == 0;
}

static Bool verifyRuntimeProofRawEvidenceIndex(const WideChar *executablePath,
	const std::string &expectedIndexSha256)
{
	static const WideChar INDEX_NAME[] =
		L"MultiplayerSimulationRawEvidence.index";
	WideChar indexPath[MAX_PATH];
	if (!buildRuntimeProofSiblingPath(executablePath, INDEX_NAME, indexPath))
		return FALSE;
	char actualIndexSha256[65];
	if (!calculateFileSha256(indexPath, actualIndexSha256) ||
		expectedIndexSha256 != actualIndexSha256)
	{
		return FALSE;
	}
	std::vector<char> bytes;
	if (!readBoundedRuntimeProofFile(indexPath, 64U * 1024U, bytes))
		return FALSE;
	const std::string document(bytes.data(), bytes.size());
	static const char MAGIC[] =
		"RTS_MULTIPLAYER_SIMULATION_RAW_EVIDENCE_V1\n";
	if (document.compare(0, sizeof(MAGIC) - 1, MAGIC) != 0)
		return FALSE;
	std::size_t cursor = sizeof(MAGIC) - 1;
	std::vector<std::string> observedPaths;
	observedPaths.reserve(40U);
	for (UnsignedInt index = 0; index < 40U; ++index)
	{
		const std::size_t lineEnd = document.find('\n', cursor);
		if (lineEnd == std::string::npos)
			return FALSE;
		const std::string line(document, cursor, lineEnd - cursor);
		char expectedOrdinal[4];
		sprintf_s(expectedOrdinal, "%02u|", index);
		if (line.compare(0, 3, expectedOrdinal) != 0)
			return FALSE;
		const std::size_t separator = line.find('|', 3);
		if (separator == std::string::npos || separator + 65 != line.size())
			return FALSE;
		const std::string relativePath(line, 3, separator - 3);
		const std::string expectedSha256(line, separator + 1, 64);
		if (!isSafeRuntimeProofRelativePath(relativePath) ||
			!rts::IsCanonicalUpperHexDigest(expectedSha256, 64))
		{
			return FALSE;
		}
		for (std::size_t prior = 0; prior < observedPaths.size(); ++prior)
		{
			if (observedPaths[prior] == relativePath)
				return FALSE;
		}
		observedPaths.push_back(relativePath);
		WideChar relativeWide[MAX_PATH];
		const Int wideLength = MultiByteToWideChar(CP_UTF8,
			MB_ERR_INVALID_CHARS, relativePath.c_str(), -1, relativeWide,
			ARRAY_SIZE(relativeWide));
		if (wideLength <= 0 || !verifyRuntimeProofFileSha256(executablePath,
			relativeWide, expectedSha256))
		{
			return FALSE;
		}
		cursor = lineEnd + 1;
	}
	return document.compare(cursor, 4, "END\n") == 0 &&
		cursor + 4 == document.size();
}

static Bool verifyRuntimeProofEvidenceBundle(const WideChar *executablePath,
	const rts::MultiplayerSimulationRuntimeProof &proof)
{
	return verifyRuntimeProofFileSha256(executablePath,
		L"Net3LoopbackEvidence.json", proof.evidenceManifestSha256) &&
		verifyRuntimeProofFileSha256(executablePath,
		L"Stage5ArtifactSet.json", proof.artifactSetSha256) &&
		verifyRuntimeProofRawEvidenceIndex(executablePath,
			proof.rawEvidenceIndexSha256);
}

static unsigned getRuntimeMultiplayerSimulationReleaseProvenKernelMask(
	UnsignedInt buildCompatibilityCrc, UnsignedInt contentCrc)
{
	static Bool inspected = FALSE;
	static UnsignedInt inspectedBuildCrc = 0U;
	static UnsignedInt inspectedContentCrc = 0U;
	static unsigned resolvedMask =
		rts::MULTIPLAYER_SIMULATION_KERNEL_RELEASE_PROVEN_DEFAULT_MASK;
	if (inspected)
	{
		return buildCompatibilityCrc == inspectedBuildCrc &&
			contentCrc == inspectedContentCrc ? resolvedMask :
			rts::MULTIPLAYER_SIMULATION_KERNEL_RELEASE_PROVEN_DEFAULT_MASK;
	}
	inspected = TRUE;
	inspectedBuildCrc = buildCompatibilityCrc;
	inspectedContentCrc = contentCrc;

	WideChar executablePath[MAX_PATH];
	char executableSha256[65];
	std::vector<char> document;
	if (!getCurrentExecutablePath(executablePath, ARRAY_SIZE(executablePath)) ||
		!calculateFileSha256(executablePath, executableSha256) ||
		!readRuntimeProofDocument(executablePath, document))
	{
		return rts::MULTIPLAYER_SIMULATION_KERNEL_RELEASE_PROVEN_DEFAULT_MASK;
	}
	rts::MultiplayerSimulationRuntimeProof proof;
	if (!rts::ParseMultiplayerSimulationRuntimeProof(document.data(),
		document.size(), proof) ||
		!verifyRuntimeProofEvidenceBundle(executablePath, proof))
	{
		return rts::MULTIPLAYER_SIMULATION_KERNEL_RELEASE_PROVEN_DEFAULT_MASK;
	}
	#if RTS_GENERALS
	static const char EXPECTED_TITLE[] = "Generals";
	#else
	static const char EXPECTED_TITLE[] = "ZeroHour";
	#endif
	resolvedMask = rts::ResolveMultiplayerSimulationRuntimeProofMask(proof,
		EXPECTED_TITLE, executableSha256, buildCompatibilityCrc, contentCrc,
		static_cast<unsigned>(
			rts::MULTIPLAYER_SIMULATION_KERNEL_LIVE_INTEGRATED_MASK),
		static_cast<unsigned>(
			rts::MULTIPLAYER_SIMULATION_KERNEL_RELEASE_PROVEN_DEFAULT_MASK),
		"");
	return resolvedMask;
}

static Bool generateNetworkHelloToken(std::uint64_t *token)
{
	if (token == nullptr)
		return FALSE;

	*token = 0U;
	const NTSTATUS status = BCryptGenRandom(nullptr,
		reinterpret_cast<PUCHAR>(token), static_cast<ULONG>(sizeof(*token)),
		BCRYPT_USE_SYSTEM_PREFERRED_RNG);
	if (status != 0 || *token == 0U)
	{
		*token = 0U;
		return FALSE;
	}
	return TRUE;
}
#endif

struct TransferFileRule
{
	const char* ext;
	UnsignedInt maxSize;
};

static const TransferFileRule transferFileRules[TransferFileType_Count] =
{
	{ ".map", 5 * 1024 * 1024 },
	{ ".ini", 2 * 1024 * 1024 },
	{ ".str", 512 * 1024 },
	{ ".txt", 1 * 1024 * 1024 },
	{ ".tga", 2 * 1024 * 1024 },
	{ ".wak", 128 * 1024 },
};

static TransferFileType getTransferFileType(const char* extension)
{
	for (Int i = 0; i < TransferFileType_Count; ++i)
	{
		if (stricmp(extension, transferFileRules[i].ext) == 0)
		{
			return static_cast<TransferFileType>(i);
		}
	}
	return TransferFileType_Invalid;
}

static Bool hasValidTransferFileContent(const AsciiString& filePath, const UnsignedByte* data, UnsignedInt dataSize)
{
	const char* fileExt = strrchr(filePath.str(), '.');
	if (fileExt == nullptr)
	{
		DEBUG_LOG(("File '%s' has no extension for content validation.", filePath.str()));
		return false;
	}

	const TransferFileType fileType = getTransferFileType(fileExt);
	if (fileType == TransferFileType_Invalid)
	{
		DEBUG_LOG(("File '%s' has unrecognized extension '%s' for content validation.", filePath.str(), fileExt));
		return false;
	}

	// Check size limit
	const TransferFileRule& rule = transferFileRules[fileType];
	if (dataSize > rule.maxSize)
	{
		DEBUG_LOG(("File '%s' exceeds maximum size (%u bytes, limit %u bytes).", filePath.str(), dataSize, rule.maxSize));
		return false;
	}

	// Extension-specific content validation
	switch (fileType)
	{
	case TransferFileType_Map:
		break;

	case TransferFileType_Ini:
	{
		for (UnsignedInt i = 0; i < dataSize; ++i)
		{
			if (data[i] == 0)
			{
				DEBUG_LOG(("INI file '%s' contains null bytes (likely binary).", filePath.str()));
				return false;
			}
		}
		break;
	}

	case TransferFileType_Tga:
	{
		if (dataSize < sizeof(TGAHeader) + sizeof(TGA2Footer))
		{
			DEBUG_LOG(("TGA file '%s' is too small to be valid.", filePath.str()));
			return false;
		}
		TGA2Footer footer;
		memcpy(&footer, data + dataSize - sizeof(footer), sizeof(footer));
		const Bool isTGA2 = memcmp(footer.Signature, TGA2_SIGNATURE, sizeof(footer.Signature)) == 0
			&& footer.RsvdChar == '.'
			&& footer.BZST == '\0';
		if (!isTGA2)
		{
			DEBUG_LOG(("TGA file '%s' is missing TRUEVISION-XFILE footer signature.", filePath.str()));
			return false;
		}
		break;
	}

	default:
	{
		break;
	}
	}

	return true;
}

/**
 * Le destructor.
 */
ConnectionManager::~ConnectionManager()
{
	deleteInstance(m_localUser);
	m_localUser = nullptr;

	// Network will delete transports; we just forget them
	delete m_transport;
	m_transport = nullptr;

	Int i = 0;
	for (; i < MAX_SLOTS; ++i) {
		deleteInstance(m_frameData[i]);
		m_frameData[i] = nullptr;
	}

	for (i = 0; i < MAX_SLOTS; ++i) {
		deleteInstance(m_connections[i]);
		m_connections[i] = nullptr;
	}

	// This is done here since TheDisconnectMenu should only be there if we are in a network game.
	delete TheDisconnectMenu;
	TheDisconnectMenu = nullptr;

	delete m_disconnectManager;
	m_disconnectManager = nullptr;

	deleteInstance(m_pendingCommands);
	m_pendingCommands = nullptr;

	deleteInstance(m_relayedCommands);
	m_relayedCommands = nullptr;

	deleteInstance(m_netCommandWrapperList);
	m_netCommandWrapperList = nullptr;

#if defined(_WIN64)
	deleteInstance(m_networkHelloPendingCommands);
	m_networkHelloPendingCommands = nullptr;
	clearNetworkFrameRecovery();
#endif

	s_fileCommandMap.clear();
	s_fileRecipientMaskMap.clear();
	for (i = 0; i < MAX_SLOTS; ++i) {
		s_fileProgressMap[i].clear();
	}
}

/**
 * Le constructor
 */
ConnectionManager::ConnectionManager()
{
	for (Int i = 0; i < MAX_SLOTS; ++i) {
		m_frameData[i] = nullptr;
	}
	m_transport = nullptr;
	m_disconnectManager = nullptr;
	m_pendingCommands = nullptr;
	m_relayedCommands = nullptr;
	m_localAddr = 0;
	m_localPort = 0;
	m_netCommandWrapperList = nullptr;
	m_localUser = nullptr;
	m_localUser = newInstance(User);
#if defined(_WIN64)
	m_networkHelloStarted = FALSE;
	m_networkHelloRequired = FALSE;
	m_networkHelloFailed = FALSE;
	m_networkHelloStartTime = 0U;
	m_networkHelloLastSend = 0U;
	m_networkHelloAttempts = 0U;
	m_networkHelloLocalToken = 0U;
	m_networkHelloDeferredCount = 0U;
	m_networkHelloPendingCommands = nullptr;
	m_networkHelloPendingCommandCount = 0U;
	m_lockstepV2ReceiptRecorder.reset();
	m_lockstepV2Session = rts::lockstep_v2::SessionContract();
	m_lockstepV2TransportInitialized = FALSE;
	clearNetworkSimulationPolicy();
	clearNetworkFrameResendRequest();
	for (Int i = 0; i < MAX_SLOTS; ++i) {
		m_networkHelloValidated[i] = FALSE;
		m_networkHelloAckReceived[i] = FALSE;
		m_networkHelloRemoteToken[i] = 0U;
		m_networkRecoveryWrappers[i] = nullptr;
	}
	m_networkHelloExpectedSlots = 0U;
	for (Int i = 0; i < MAX_MESSAGES; ++i) {
		m_networkHelloDeferred[i].length = 0;
	}
#endif
}

/**
 * Initialize the connection manager and any subsystems.
 */
void ConnectionManager::init()
{
//	if (m_transport == nullptr) {
//		m_transport = new Transport;
//	}
//	m_transport->reset();

	UnsignedInt i = 0;
	for (; i < MAX_SLOTS; ++i) {
		m_connections[i] = nullptr;
	}

#if defined(_WIN64)
	m_networkHelloStarted = FALSE;
	m_networkHelloRequired = FALSE;
	m_networkHelloFailed = FALSE;
	m_networkHelloStartTime = 0U;
	m_networkHelloLastSend = 0U;
	m_networkHelloAttempts = 0U;
	m_networkHelloLocalToken = 0U;
	m_networkHelloDeferredCount = 0U;
	m_lockstepV2ReceiptRecorder.reset();
	m_lockstepV2Session = rts::lockstep_v2::SessionContract();
	m_lockstepV2TransportInitialized = FALSE;
	clearNetworkSimulationPolicy();
	for (i = 0; i < MAX_SLOTS; ++i) {
		m_networkHelloValidated[i] = FALSE;
		m_networkHelloAckReceived[i] = FALSE;
		m_networkHelloRemoteToken[i] = 0U;
	}
	m_networkHelloExpectedSlots = 0U;
	for (i = 0; i < MAX_MESSAGES; ++i) {
		m_networkHelloDeferred[i].length = 0;
	}
	if (m_networkHelloPendingCommands == nullptr) {
		m_networkHelloPendingCommands = newInstance(NetCommandList);
		m_networkHelloPendingCommands->init();
	}
	m_networkHelloPendingCommands->reset();
	m_networkHelloPendingCommandCount = 0U;
	clearNetworkFrameResendRequest();
	clearNetworkFrameRecovery();
#endif

	if (m_pendingCommands == nullptr) {
		m_pendingCommands = newInstance(NetCommandList);
		m_pendingCommands->init();
	}
	m_pendingCommands->reset();

	if (m_relayedCommands == nullptr) {
		m_relayedCommands = newInstance(NetCommandList);
		m_relayedCommands->init();
	}
	m_relayedCommands->reset();

	m_localSlot = -1;
#ifdef MEMORYPOOL_DEBUG
	TheMemoryPoolFactory->debugSetInitFillerIndex(m_localSlot);
#endif
	m_packetRouterSlot = 0; /// @todo The LAN/WOL interface should be telling us who the packet router is based on machine specs passed around through game options.
	for (i = 0; i < MAX_SLOTS; ++i) {
		m_packetRouterFallback[i] = -1;
	}

	for (i = 0; i < MAX_SLOTS; ++i) {
		deleteInstance(m_frameData[i]);
		m_frameData[i] = nullptr;
	}

//	m_averageFps = 30;			// since 30 fps is the desired rate, we'll start off at that.
//	m_averageLatency = (Real)0.2; // 200ms seems like a good starting point.

	for (i = 0; i < MAX_SLOTS; ++i) {
		m_fpsAverages[i] = -1;
	}
	for (i = 0; i < MAX_SLOTS; ++i) {
		m_latencyAverages[i] = 0.0; // using zero since all floating point standards should be able to specify 0.0 accurately.
	}
	m_smallestPacketArrivalCushion = -1;

	m_frameMetrics.init();

	TheDisconnectMenu = NEW DisconnectMenu;
	TheDisconnectMenu->init();

	m_disconnectManager = NEW DisconnectManager;
	m_disconnectManager->init();

	TheDisconnectMenu->attachDisconnectManager(m_disconnectManager);
	TheDisconnectMenu->hideScreen();

	m_netCommandWrapperList = newInstance(NetCommandWrapperList);
	m_netCommandWrapperList->init();

	s_fileCommandMap.clear();
	s_fileRecipientMaskMap.clear();
	for (i = 0; i < MAX_SLOTS; ++i) {
		s_fileProgressMap[i].clear();
	}
}

/**
 * Reset the connection manager and any subsystems.
 */
void ConnectionManager::reset()
{
//	if (m_transport == nullptr) {
//		m_transport = new Transport;
//	}
//	m_transport->reset();

	delete m_transport;
	m_transport = nullptr;

	UnsignedInt i = 0;
	for (; i < (UnsignedInt)MAX_SLOTS; ++i) {
		deleteInstance(m_connections[i]);
		m_connections[i] = nullptr;
	}

	for (i=0; i<(UnsignedInt)MAX_SLOTS; ++i)
	{
		deleteInstance(m_frameData[i]);
		m_frameData[i] = nullptr;
	}

	if (m_pendingCommands == nullptr) {
		m_pendingCommands = newInstance(NetCommandList);
		m_pendingCommands->init();
	}
	m_pendingCommands->reset();

	if (m_relayedCommands == nullptr) {
		m_relayedCommands = newInstance(NetCommandList);
		m_relayedCommands->init();
	}
	m_relayedCommands->reset();

	if (m_netCommandWrapperList == nullptr) {
		m_netCommandWrapperList = newInstance(NetCommandWrapperList);
		m_netCommandWrapperList->init();
	}
	m_netCommandWrapperList->reset();

	m_localSlot = -1;
#ifdef MEMORYPOOL_DEBUG
	TheMemoryPoolFactory->debugSetInitFillerIndex(m_localSlot);
#endif
	m_packetRouterSlot = -1;

#if defined(_WIN64)
	m_networkHelloStarted = FALSE;
	m_networkHelloRequired = FALSE;
	m_networkHelloFailed = FALSE;
	m_networkHelloStartTime = 0U;
	m_networkHelloLastSend = 0U;
	m_networkHelloAttempts = 0U;
	m_networkHelloLocalToken = 0U;
	m_networkHelloDeferredCount = 0U;
	m_lockstepV2ReceiptRecorder.reset();
	m_lockstepV2Session = rts::lockstep_v2::SessionContract();
	m_lockstepV2TransportInitialized = FALSE;
	clearNetworkSimulationPolicy();
	for (i = 0; i < MAX_SLOTS; ++i) {
		m_networkHelloValidated[i] = FALSE;
		m_networkHelloAckReceived[i] = FALSE;
		m_networkHelloRemoteToken[i] = 0U;
	}
	m_networkHelloExpectedSlots = 0U;
	for (i = 0; i < MAX_MESSAGES; ++i) {
		m_networkHelloDeferred[i].length = 0;
	}
	clearNetworkHelloPendingCommands();
	clearNetworkFrameResendRequest();
	clearNetworkFrameRecovery();
#endif

	for (i = 0; i < TheGlobalData->m_networkFPSHistoryLength; ++i) {
		m_fpsAverages[i] = -1;
	}
	for (i = 0; i < TheGlobalData->m_networkLatencyHistoryLength; ++i) {
		m_latencyAverages[i] = 0.0;
	}

	for (i = 0; i < (UnsignedInt)MAX_SLOTS; ++i) {
		m_packetRouterFallback[i] = -1;
	}

	m_frameMetrics.reset();
}

UnsignedInt ConnectionManager::getPingFrame()
{
	return (m_disconnectManager)?m_disconnectManager->getPingFrame():0;
}

Int ConnectionManager::getPingsSent()
{
	return (m_disconnectManager)?m_disconnectManager->getPingsSent():0;
}

Int ConnectionManager::getPingsReceived()
{
	return (m_disconnectManager)?m_disconnectManager->getPingsReceived():0;
}

Bool ConnectionManager::isPlayerConnected( Int playerID )
{
	DEBUG_ASSERTCRASH( playerID < MAX_SLOTS, ("ConnectionManager::isPlayerConnected - %d is an invalid player number", playerID) );
	return ( playerID == m_localSlot || (m_connections[playerID] && !m_connections[playerID]->isQuitting()) );
}

void ConnectionManager::attachTransport(Transport *transport) {
	delete m_transport;
	m_transport = transport;
#if defined(_WIN64)
	// An attached transport may be owned by the normal LAN/GameSpy startup
	// path.  Only initTransport() can positively report that this manager bound
	// a UDP socket, so an externally attached object is not v2-proof-ready.
	m_lockstepV2TransportInitialized = FALSE;
#endif
}

Bool ConnectionManager::isNetworkHelloReady() const
{
#if defined(_WIN64)
	return rts::network_epoch::IsNetworkHelloGateReady(
		m_networkHelloRequired, m_networkHelloFailed);
#else
	return TRUE;
#endif
}

Bool ConnectionManager::hasNetworkHelloFailure() const
{
#if defined(_WIN64)
	return m_networkHelloFailed;
#else
	return FALSE;
#endif
}

Bool ConnectionManager::isNetworkSimulationPolicyUsable() const
{
#if defined(_WIN64)
	UnsignedInt presentRemoteMask = 0U;
	UnsignedInt quittingRemoteMask = 0U;
	for (Int slot = 0; slot < MAX_SLOTS; ++slot)
	{
		const UnsignedInt slotMask = 1U << slot;
		if ((m_networkHelloExpectedSlots & slotMask) == 0U)
			continue;
		if (m_connections[slot] != nullptr &&
			m_networkSimulationRemoteIdentityReceived[slot] &&
			m_networkSimulationRemoteIdentity[slot].rosterMask ==
				m_networkSimulationRosterMask)
		{
			presentRemoteMask |= slotMask;
		}
		if (m_connections[slot] != nullptr &&
			m_connections[slot]->isQuitting())
		{
			quittingRemoteMask |= slotMask;
		}
	}
	return m_networkSimulationLocalIdentity.rosterMask ==
		m_networkSimulationRosterMask &&
		rts::IsMultiplayerSimulationPolicyLifecycleUsable(
			m_networkSimulationPolicyResolved,
			m_networkSimulationSessionPolicy.status ==
				rts::MULTIPLAYER_SIMULATION_POLICY_READY,
			isNetworkHelloReady(), m_networkHelloFailed,
			m_networkSimulationRosterMask, m_networkHelloExpectedSlots,
			presentRemoteMask, quittingRemoteMask, m_localSlot, MAX_SLOTS);
#else
	return FALSE;
#endif
}

Bool ConnectionManager::refreshNetworkSimulationPolicyForLockstepV2()
{
#if defined(_WIN64)
	if (!rts::IsInstalledLockstepV2QualificationActive() ||
		m_lockstepV2ReceiptRecorder.isActive() || !m_networkHelloStarted ||
		m_localSlot < 0 || m_localSlot >= MAX_SLOTS)
	{
		return FALSE;
	}
	// Reusing the production Hello path is important: the refreshed identity
	// and its session challenge still travel over the same UDP transport and
	// are subject to the normal endpoint/roster checks.
	beginNetworkHello();
	return !m_networkHelloFailed;
#else
	return FALSE;
#endif
}

Bool ConnectionManager::isMultiplayerSimulationKernelEnabled(
	rts::MultiplayerSimulationKernel kernel) const
{
#if defined(_WIN64)
	if (!isNetworkSimulationPolicyUsable())
		return FALSE;
	return rts::IsMultiplayerSimulationKernelEnabled(
		m_networkSimulationSessionPolicy, kernel);
#else
	return FALSE;
#endif
}

UnsignedInt ConnectionManager::getMultiplayerSimulationEnabledKernelMask() const
{
#if defined(_WIN64)
	return isNetworkSimulationPolicyUsable() ?
		m_networkSimulationSessionPolicy.enabledKernelMask : 0U;
#else
	return 0U;
#endif
}

rts::MultiplayerSimulationPolicyStatus
ConnectionManager::getMultiplayerSimulationPolicyStatus() const
{
#if defined(_WIN64)
	return m_networkSimulationPolicyResolved ?
		m_networkSimulationSessionPolicy.status :
		rts::MULTIPLAYER_SIMULATION_POLICY_SERIAL_NETWORK_UNAVAILABLE;
#else
	return rts::MULTIPLAYER_SIMULATION_POLICY_SERIAL_NETWORK_UNAVAILABLE;
#endif
}

#if defined(_WIN64)
Bool ConnectionManager::beginLockstepV2Proof(
	const rts::lockstep_v2::SessionContract &session)
{
	m_lockstepV2ReceiptRecorder.reset();
	m_lockstepV2Session = rts::lockstep_v2::SessionContract();
	if (m_transport == nullptr || !m_lockstepV2TransportInitialized ||
		!m_networkHelloStarted || !isNetworkHelloReady() || m_networkHelloFailed ||
		m_networkHelloLocalToken == 0U || m_localSlot < 0 ||
		m_localSlot >= MAX_SLOTS ||
		!rts::lockstep_v2::IsValidSessionContract(session) ||
		!rts::IsInstalledLockstepV2QualificationActive() ||
		!isNetworkSimulationPolicyUsable() ||
		TheGlobalData == nullptr ||
		session.localSlot != static_cast<UnsignedInt>(m_localSlot) ||
		session.rosterMask != m_networkSimulationRosterMask ||
		session.buildCompatibilityCrc != TheGlobalData->m_exeCRC ||
		session.contentCrc != TheGlobalData->m_iniCRC ||
		session.mapCrc != m_networkSimulationMapCrc)
	{
		return FALSE;
	}
	if (session.originMode == rts::lockstep_v2::CommandOriginMode::TrustedRouter &&
		(m_packetRouterSlot < 0 || m_packetRouterSlot >= MAX_SLOTS ||
			session.packetRouterSlot != static_cast<UnsignedInt>(m_packetRouterSlot) ||
			(m_packetRouterSlot != m_localSlot &&
				(m_connections[m_packetRouterSlot] == nullptr ||
					m_connections[m_packetRouterSlot]->isQuitting()))))
	{
		return FALSE;
	}

	WideChar executablePath[MAX_PATH];
	char executableSha256[65];
	if (!getCurrentExecutablePath(executablePath, ARRAY_SIZE(executablePath)) ||
		!calculateFileSha256(executablePath, executableSha256))
	{
		return FALSE;
	}
	if (!m_lockstepV2ReceiptRecorder.beginQualification(session,
		m_networkHelloLocalToken, executableSha256))
	{
		return FALSE;
	}
	m_lockstepV2Session = session;
	return TRUE;
}

Bool ConnectionManager::recordLockstepV2Command(UnsignedInt frame,
	UnsignedInt originSlot, UnsignedShort commandId,
	std::uint64_t commandDigest)
{
	return m_lockstepV2ReceiptRecorder.recordCommand(frame, originSlot,
		commandId, commandDigest);
}

Bool ConnectionManager::recordLockstepV2Frame(UnsignedInt frame,
	UnsignedInt crc, std::uint64_t commandDigest)
{
	return m_lockstepV2ReceiptRecorder.recordFrame(frame, crc,
		commandDigest);
}

Bool ConnectionManager::finalizeLockstepV2Proof(Bool cleanShutdown,
	rts::lockstep_v2::Receipt *receipt)
{
	if (receipt == nullptr || !m_lockstepV2ReceiptRecorder.isActive() ||
		m_transport == nullptr || !m_lockstepV2TransportInitialized ||
		!isNetworkHelloReady() || m_networkHelloFailed ||
		!isNetworkSimulationPolicyUsable() || cleanShutdown == FALSE ||
		!areAllQueuesEmpty())
	{
		return FALSE;
	}
	// A clean qualification boundary is observed from production transport
	// state, not inferred from the caller reaching the common stop frame.  Every
	// remote roster member must still be connected and non-quitting, and every
	// reliable-send queue must already be drained before the receipt is sealed.
	for (Int slot = 0; slot < MAX_SLOTS; ++slot)
	{
		if (slot == m_localSlot ||
			(m_networkSimulationRosterMask & (1U << slot)) == 0U)
		{
			continue;
		}
		if (m_connections[slot] == nullptr || m_connections[slot]->isQuitting())
			return FALSE;
	}
	rts::lockstep_v2::WorkerTelemetry workerTelemetry;
	if (!rts::GetInstalledLockstepV2WorkerTelemetry(&workerTelemetry) ||
		workerTelemetry.authorityMask != m_lockstepV2Session.provenKernelMask ||
		!m_lockstepV2ReceiptRecorder.publishWorkerTelemetry(workerTelemetry) ||
		!m_lockstepV2ReceiptRecorder.finish(cleanShutdown != FALSE,
		TRUE, TRUE))
	{
		return FALSE;
	}
	*receipt = m_lockstepV2ReceiptRecorder.receipt();
	return TRUE;
}

Bool ConnectionManager::isLockstepV2ProofActive() const
{
	return m_lockstepV2ReceiptRecorder.isActive();
}

void ConnectionManager::clearNetworkSimulationPolicy()
{
	m_networkSimulationMapCrc = 0U;
	m_networkSimulationRosterMask = 0U;
	m_networkSimulationPolicyResolved = FALSE;
	m_networkSimulationLocalIdentity =
		rts::network_epoch::NetworkSimulationPolicyIdentity();
	m_networkSimulationSessionPolicy =
		rts::MultiplayerSimulationSessionPolicy();
	for (Int slot = 0; slot < MAX_SLOTS; ++slot)
	{
		m_networkSimulationRemoteIdentity[slot] =
			rts::network_epoch::NetworkSimulationPolicyIdentity();
		m_networkSimulationRemoteIdentityReceived[slot] = FALSE;
	}
}

void ConnectionManager::revokeNetworkSimulationPolicy()
{
	m_networkSimulationPolicyResolved = FALSE;
	m_networkSimulationSessionPolicy =
		rts::MultiplayerSimulationSessionPolicy();
	m_networkSimulationLocalIdentity.provenKernelMask = 0U;
	for (Int slot = 0; slot < MAX_SLOTS; ++slot)
		m_networkSimulationRemoteIdentityReceived[slot] = FALSE;
}

Bool ConnectionManager::acceptNetworkSimulationPolicy(Int slot,
	const rts::network_epoch::NetworkSimulationPolicyIdentity &identity)
{
	if (slot < 0 || slot >= MAX_SLOTS ||
		!rts::network_epoch::IsNetworkSimulationRosterIdentityValid(
			identity.rosterMask, m_networkHelloExpectedSlots,
			m_localSlot, MAX_SLOTS))
	{
		return FALSE;
	}
	if (m_networkSimulationRemoteIdentityReceived[slot] &&
		!rts::network_epoch::IsMatchingNetworkSimulationPolicyIdentity(
			m_networkSimulationRemoteIdentity[slot], identity))
	{
		return FALSE;
	}
	m_networkSimulationRemoteIdentity[slot] = identity;
	m_networkSimulationRemoteIdentityReceived[slot] = TRUE;
	return TRUE;
}

Bool ConnectionManager::resolveNetworkSimulationPolicy()
{
	rts::MultiplayerSimulationPeerPolicy localPeer;
	localPeer.schema = m_networkSimulationLocalIdentity.schema;
	localPeer.engineEpoch = m_networkSimulationLocalIdentity.engineEpoch;
	localPeer.determinismEpoch =
		m_networkSimulationLocalIdentity.determinismEpoch;
	localPeer.buildCompatibilityCrc =
		m_networkSimulationLocalIdentity.buildCompatibilityCrc;
	localPeer.contentCrc = m_networkSimulationLocalIdentity.contentCrc;
	localPeer.mapCrc = m_networkSimulationLocalIdentity.mapCrc;
	localPeer.provenKernelMask =
		m_networkSimulationLocalIdentity.provenKernelMask;

	rts::MultiplayerSimulationPeerPolicy remotePeers[
		rts::MULTIPLAYER_SIMULATION_MAXIMUM_REMOTE_PEERS];
	unsigned remotePeerCount = 0U;
	for (Int slot = 0; slot < MAX_SLOTS; ++slot)
	{
		if ((m_networkHelloExpectedSlots & (1U << slot)) == 0U)
			continue;
		if (!m_networkSimulationRemoteIdentityReceived[slot] ||
			remotePeerCount >=
				rts::MULTIPLAYER_SIMULATION_MAXIMUM_REMOTE_PEERS)
		{
			return FALSE;
		}
		const rts::network_epoch::NetworkSimulationPolicyIdentity &identity =
			m_networkSimulationRemoteIdentity[slot];
		rts::MultiplayerSimulationPeerPolicy &remotePeer =
			remotePeers[remotePeerCount++];
		remotePeer.schema = identity.schema;
		remotePeer.engineEpoch = identity.engineEpoch;
		remotePeer.determinismEpoch = identity.determinismEpoch;
		remotePeer.buildCompatibilityCrc = identity.buildCompatibilityCrc;
		remotePeer.contentCrc = identity.contentCrc;
		remotePeer.mapCrc = identity.mapCrc;
		remotePeer.provenKernelMask = identity.provenKernelMask;
	}

	if (remotePeerCount == 0U)
		return FALSE;
	// A rejected negotiation is still a resolved, persisted serial policy.
	// Unsupported or unproven contracts must not turn a compatible NET3
	// transport exchange into implicit worker permission.
	rts::ResolveMultiplayerSimulationSessionPolicy(localPeer, remotePeers,
		remotePeerCount, rts::MULTIPLAYER_SIMULATION_KERNEL_KNOWN_MASK,
		m_networkSimulationSessionPolicy);
	m_networkSimulationPolicyResolved = TRUE;
	return TRUE;
}

void ConnectionManager::beginNetworkHello()
{
	clearNetworkHelloPendingCommands();
	m_networkHelloDeferredCount = 0U;
	for (Int i = 0; i < MAX_MESSAGES; ++i)
		m_networkHelloDeferred[i].length = 0;

	m_networkHelloStarted = TRUE;
	m_networkHelloRequired = FALSE;
	m_networkHelloFailed = FALSE;
	m_networkHelloStartTime = 0U;
	m_networkHelloLastSend = 0U;
	m_networkHelloAttempts = 0U;
	m_networkHelloLocalToken = 0U;
	m_networkHelloExpectedSlots = 0U;
	m_networkSimulationPolicyResolved = FALSE;
	m_networkSimulationSessionPolicy =
		rts::MultiplayerSimulationSessionPolicy();

	Bool hasRemotePeer = FALSE;
	for (Int i = 0; i < MAX_SLOTS; ++i)
	{
		m_networkHelloValidated[i] = FALSE;
		m_networkHelloAckReceived[i] = FALSE;
		m_networkHelloRemoteToken[i] = 0U;
		m_networkSimulationRemoteIdentity[i] =
			rts::network_epoch::NetworkSimulationPolicyIdentity();
		m_networkSimulationRemoteIdentityReceived[i] = FALSE;
		if (m_connections[i] != nullptr)
		{
			hasRemotePeer = TRUE;
			m_networkHelloExpectedSlots |= (1U << i);
		}
	}
	if (m_localSlot < 0 || m_localSlot >= MAX_SLOTS)
	{
		rejectNetworkHello(-1, "NET3 local slot is not in the game roster");
		return;
	}
	m_networkSimulationRosterMask = m_networkHelloExpectedSlots |
		(1U << m_localSlot);
	unsigned candidateKernelMask =
		getRuntimeMultiplayerSimulationReleaseProvenKernelMask(
			TheGlobalData->m_exeCRC, TheGlobalData->m_iniCRC);
#if defined(_WIN64)
	// The installed v2 qualifier refreshes this hello only after its local
	// scheduler prerequisites are ready.  This mask is permission for the
	// bounded qualification run; executable-origin kernel telemetry is gathered
	// from the real gameplay execution and published only at finalization.
	if (rts::IsInstalledLockstepV2QualificationActive())
	{
		candidateKernelMask = rts::GetInstalledLockstepV2ValidationAuthorityMask(
			TheGlobalData->m_exeCRC, TheGlobalData->m_iniCRC);
	}
#endif
	m_networkSimulationLocalIdentity =
		rts::network_epoch::MakeNetworkSimulationPolicyIdentity(
			TheGlobalData->m_exeCRC, TheGlobalData->m_iniCRC,
			m_networkSimulationMapCrc, m_networkSimulationRosterMask,
			candidateKernelMask);

	if (!hasRemotePeer)
		return;
	if (!generateNetworkHelloToken(&m_networkHelloLocalToken))
	{
		rejectNetworkHello(-1, "NET3 session token generation failed");
		return;
	}

	// parseUserList is called after the LAN/GameSpy transport has been
	// initialized.  Keep the compatibility exchange on that common transport
	// so the legacy lobby and NAT protocols remain byte-for-byte unchanged.
	m_networkHelloRequired = TRUE;
	const UnsignedInt now = static_cast<UnsignedInt>(timeGetTime());
	m_networkHelloStartTime = now;
	m_networkHelloLastSend = now;
	m_networkHelloAttempts = 1U;
	for (Int i = 0; i < MAX_SLOTS; ++i)
	{
		if (m_connections[i] != nullptr)
			sendNetworkHello(i);
	}
}

void ConnectionManager::serviceNetworkHello()
{
	if (!m_networkHelloRequired || m_networkHelloFailed)
		return;
	if (m_transport == nullptr)
	{
		rejectNetworkHello(-1, "NET3 handshake has no transport");
		return;
	}

	for (Int i = 0; i < MAX_SLOTS; ++i)
	{
		if ((m_networkHelloExpectedSlots & (1U << i)) != 0U &&
			(m_connections[i] == nullptr || m_connections[i]->isQuitting()))
		{
			rejectNetworkHello(i, "expected NET3 peer disappeared");
			return;
		}
	}

	UnsignedInt validatedSlots = 0U;
	UnsignedInt acknowledgedSlots = 0U;
	for (Int i = 0; i < MAX_SLOTS; ++i)
	{
		if (m_networkHelloValidated[i])
			validatedSlots |= (1U << i);
		if (m_networkHelloAckReceived[i])
			acknowledgedSlots |= (1U << i);
	}
	if (rts::network_epoch::IsNetworkHelloComplete(m_networkHelloExpectedSlots,
		validatedSlots, acknowledgedSlots))
	{
		if (!resolveNetworkSimulationPolicy())
		{
			rejectNetworkHello(-1,
				"NET3 simulation policy roster could not be resolved");
			return;
		}
		m_networkHelloRequired = FALSE;
		drainNetworkHelloPendingCommands();
		return;
	}

	const UnsignedInt now = static_cast<UnsignedInt>(timeGetTime());
	if (rts::network_epoch::IsNetworkHelloTimedOut(now, m_networkHelloStartTime))
	{
		rejectNetworkHello(-1, "NET3 handshake timed out");
		return;
	}
	if (!rts::network_epoch::IsNetworkHelloRetryDue(now, m_networkHelloLastSend))
		return;
	if (rts::network_epoch::IsNetworkHelloAttemptLimitReached(m_networkHelloAttempts))
	{
		rejectNetworkHello(-1, "NET3 handshake retry limit reached");
		return;
	}

	Bool needsHelloRetry = FALSE;
	for (Int i = 0; i < MAX_SLOTS; ++i)
	{
		if ((m_networkHelloExpectedSlots & (1U << i)) != 0U)
		{
			if (!m_networkHelloValidated[i] || !m_networkHelloAckReceived[i])
			{
				needsHelloRetry = TRUE;
				sendNetworkHello(i);
			}
		}
	}
	if (!needsHelloRetry)
		return;
	m_networkHelloLastSend = now;
	++m_networkHelloAttempts;
}

Bool ConnectionManager::sendNetworkHello(Int slot)
{
	if (m_transport == nullptr || slot < 0 || slot >= MAX_SLOTS ||
		m_connections[slot] == nullptr || m_connections[slot]->getUser() == nullptr)
		return FALSE;

	const std::array<rts::runtime_epoch::Byte, rts::network_epoch::kNetworkHelloWireSize> encoded =
		rts::network_epoch::EncodeNetworkHello(TheGlobalData->m_exeCRC, TheGlobalData->m_iniCRC,
		static_cast<UnsignedInt>(m_localSlot), static_cast<UnsignedInt>(slot),
		m_networkHelloLocalToken, rts::network_epoch::NetworkHelloKind::Hello,
		m_networkSimulationLocalIdentity);
	User *user = m_connections[slot]->getUser();
	if (m_transport->queueSend(user->GetIPAddr(), user->GetPort(), encoded.data(),
		static_cast<Int>(encoded.size())))
	{
		m_networkHelloLastSend = static_cast<UnsignedInt>(timeGetTime());
		DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("ConnectionManager::sendNetworkHello - sent NET3 hello to slot %d at %X:%d",
			slot, user->GetIPAddr(), user->GetPort()));
		return TRUE;
	}
	return FALSE;
}

Bool ConnectionManager::sendNetworkHelloAck(Int slot)
{
	if (m_transport == nullptr || slot < 0 || slot >= MAX_SLOTS ||
		m_connections[slot] == nullptr || m_connections[slot]->getUser() == nullptr ||
		m_networkHelloRemoteToken[slot] == 0U)
		return FALSE;

	const std::array<rts::runtime_epoch::Byte, rts::network_epoch::kNetworkHelloWireSize> encoded =
		rts::network_epoch::EncodeNetworkHello(TheGlobalData->m_exeCRC, TheGlobalData->m_iniCRC,
		static_cast<UnsignedInt>(m_localSlot), static_cast<UnsignedInt>(slot),
		m_networkHelloRemoteToken[slot],
		rts::network_epoch::NetworkHelloKind::Ack,
		m_networkSimulationLocalIdentity);
	User *user = m_connections[slot]->getUser();
	if (m_transport->queueSend(user->GetIPAddr(), user->GetPort(), encoded.data(),
		static_cast<Int>(encoded.size())))
	{
		DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("ConnectionManager::sendNetworkHelloAck - sent NET3 ack to slot %d at %X:%d",
			slot, user->GetIPAddr(), user->GetPort()));
		return TRUE;
	}

	DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("ConnectionManager::sendNetworkHelloAck - send queue full for slot %d; peer hello retry will request another ack",
		slot));
	return FALSE;
}

Int ConnectionManager::findNetworkHelloSlot(UnsignedInt senderSlot, UnsignedInt recipientSlot) const
{
	if (recipientSlot != m_localSlot || senderSlot >= MAX_SLOTS ||
		senderSlot == m_localSlot || m_connections[senderSlot] == nullptr ||
		m_connections[senderSlot]->isQuitting())
		return -1;
	return static_cast<Int>(senderSlot);
}

Bool ConnectionManager::matchesNetworkPeerEndpoint(const TransportMessage &message, Int slot) const
{
	if (slot < 0 || slot >= MAX_SLOTS || m_connections[slot] == nullptr ||
		m_connections[slot]->getUser() == nullptr)
	{
		return FALSE;
	}

	User *user = m_connections[slot]->getUser();
	return rts::network_epoch::IsMatchingNetworkPeerEndpoint(
		message.addr, message.port, user->GetIPAddr(), user->GetPort());
}

Int ConnectionManager::findNetworkPeerEndpoint(const TransportMessage &message) const
{
	for (Int slot = 0; slot < MAX_SLOTS; ++slot)
	{
		if (m_connections[slot] != nullptr && !m_connections[slot]->isQuitting() &&
			matchesNetworkPeerEndpoint(message, slot))
		{
			return slot;
		}
	}
	return -1;
}

Bool ConnectionManager::isKnownNetworkPeerEndpoint(const TransportMessage &message) const
{
	return findNetworkPeerEndpoint(message) >= 0;
}

Bool ConnectionManager::isNetworkCommandSourceAuthorized(const NetCommandMsg *msg, Int sourceSlot) const
{
	if (msg == nullptr || sourceSlot < 0 || sourceSlot >= MAX_SLOTS)
		return FALSE;

	UnsignedInt packetRouterSlot = MAX_SLOTS;
	if (m_packetRouterSlot < MAX_SLOTS && m_packetRouterSlot != m_localSlot &&
		m_connections[m_packetRouterSlot] != nullptr &&
		!m_connections[m_packetRouterSlot]->isQuitting())
	{
		packetRouterSlot = m_packetRouterSlot;
	}

	if (m_lockstepV2ReceiptRecorder.isActive())
	{
		if (m_lockstepV2Session.originMode ==
			rts::lockstep_v2::CommandOriginMode::TrustedRouter &&
			(m_packetRouterSlot < 0 || m_packetRouterSlot >= MAX_SLOTS ||
				static_cast<UnsignedInt>(m_packetRouterSlot) !=
					m_lockstepV2Session.packetRouterSlot ||
				(m_packetRouterSlot != m_localSlot &&
					(m_connections[m_packetRouterSlot] == nullptr ||
						m_connections[m_packetRouterSlot]->isQuitting()))))
		{
			return FALSE;
		}
		return rts::IsLockstepV2CommandSourceAuthorized(
			static_cast<unsigned>(sourceSlot),
			static_cast<unsigned>(msg->getPlayerID()),
			static_cast<unsigned>(packetRouterSlot),
			m_lockstepV2Session.originMode,
			static_cast<unsigned>(MAX_SLOTS));
	}

	return rts::network_epoch::IsNetworkCommandSourceAuthorized(
		static_cast<std::uint32_t>(sourceSlot),
		static_cast<std::uint32_t>(msg->getPlayerID()),
		static_cast<std::uint32_t>(packetRouterSlot));
}

Bool ConnectionManager::getLockstepV2CommandDigest(const NetCommandRef *ref,
	std::uint64_t *digest) const
{
	if (ref == nullptr || digest == nullptr || ref->getCommand() == nullptr)
		return FALSE;
	const NetCommandMsg *command = ref->getCommand();
	const std::size_t byteCount = command->getSizeForNetPacket();
	if (byteCount == 0U || byteCount > static_cast<std::size_t>(MAX_NETWORK_MESSAGE_LEN))
		return FALSE;
	std::vector<UnsignedByte> bytes(byteCount);
	const std::size_t written = command->copyBytesForNetPacket(bytes.data(), *ref);
	if (written != byteCount)
		return FALSE;
	if (command->getNetCommandType() == NETCOMMANDTYPE_GAMECOMMAND)
	{
		// Relay masks describe this hop, not the authored command.  Remove the
		// mutable relay value before comparing a contribution across peers while
		// retaining the field tag and every origin/frame/payload byte.
		const std::size_t relayValueOffset =
			sizeof(NetPacketCommandTypeField) + sizeof(NetPacketFrameField) + 1U;
		if (relayValueOffset >= bytes.size())
			return FALSE;
		bytes[relayValueOffset] = 0U;
	}
	*digest = rts::lockstep_v2::ComputeCommandDigest(bytes.data(), written);
	return *digest != 0U;
}

void ConnectionManager::clearNetworkFrameResendRequest()
{
	m_frameResendRequestOutstanding = FALSE;
	m_frameResendRequestResponder = MAX_SLOTS;
	m_frameResendRequestFrame = 0U;
	m_frameResendRequestStartTime = 0U;
	m_frameResendRequestExpectedInfoMask = 0U;
	m_frameResendRequestReceivedInfoMask = 0U;
}

void ConnectionManager::clearNetworkFrameRecovery()
{
	m_networkWrapperAckHistory.clear();
	for (Int slot = 0; slot < MAX_SLOTS; ++slot)
	{
		m_disconnectFrameRecovery[slot] = {};
		deleteInstance(m_networkRecoveryWrappers[slot]);
		m_networkRecoveryWrappers[slot] = nullptr;
	}
}

void ConnectionManager::allowNetworkDisconnectFrameRecovery(UnsignedInt responder,
	UnsignedInt firstFrame, UnsignedInt endFrame)
{
	if (responder >= MAX_SLOTS || responder == m_localSlot ||
		m_connections[responder] == nullptr || m_connections[responder]->isQuitting())
		return;

	UnsignedInt originMask = 0U;
	for (Int slot = 0; slot < MAX_SLOTS; ++slot)
		if (slot != m_localSlot && m_frameData[slot] != nullptr)
			originMask |= 1U << slot;
	rts::network_epoch::TrySetNetworkDisconnectFrameRecovery(m_disconnectFrameRecovery[responder],
		firstFrame, endFrame, originMask, TheGameLogic->getFrame(), FRAMES_TO_KEEP);
}

Bool ConnectionManager::isNetworkFrameRecoveryAuthorized(const NetCommandMsg *command,
	Int sourceSlot, Bool wrapper) const
{
	if (sourceSlot < 0 || sourceSlot >= MAX_SLOTS || command == nullptr ||
		m_connections[sourceSlot] == nullptr || m_connections[sourceSlot]->isQuitting())
		return FALSE;
	const UnsignedInt now = static_cast<UnsignedInt>(timeGetTime());
	const UnsignedInt currentFrame = TheGameLogic->getFrame();
	const Bool isFrameData = wrapper ? command->getNetCommandType() == NETCOMMANDTYPE_WRAPPER :
		IsCommandSynchronized(command->getNetCommandType());
	const Bool requestExpired = static_cast<UnsignedInt>(now - m_frameResendRequestStartTime) >=
		rts::network_epoch::kNetworkFrameResendResponseTimeoutMs;
	if (currentFrame <= m_frameResendRequestFrame &&
		rts::network_epoch::IsNetworkFrameResendResponseAuthorized(sourceSlot,
			m_frameResendRequestResponder, command->getPlayerID(), m_frameResendRequestExpectedInfoMask,
			MAX_SLOTS, m_frameResendRequestOutstanding, requestExpired, isFrameData,
			wrapper ? m_frameResendRequestFrame : command->getExecutionFrame(), m_frameResendRequestFrame))
		return TRUE;
	return rts::network_epoch::IsNetworkDisconnectFrameRecoveryAuthorized(
		m_disconnectFrameRecovery[sourceSlot], command->getPlayerID(), MAX_SLOTS,
		currentFrame, FRAMES_TO_KEEP, isFrameData,
		wrapper ? currentFrame : command->getExecutionFrame());
}

void ConnectionManager::ackNetworkFrameRecoveryCommand(NetCommandRef *ref, Int sourceSlot)
{
	if (sourceSlot < 0 || sourceSlot >= MAX_SLOTS || m_connections[sourceSlot] == nullptr ||
		m_connections[sourceSlot]->isQuitting())
		return;
	// Direct data, including the responder's own origin, belongs to this peer's
	// resend queue, not a separate packet router's queue.
	NetAckBothCommandMsg *ack = newInstance(NetAckBothCommandMsg)(ref->getCommand());
	ack->setPlayerID(m_localSlot);
	m_connections[sourceSlot]->sendNetCommandMsg(ack, 1U << sourceSlot);
	ack->detach();
}

Bool ConnectionManager::processNetworkFrameRecoveryWrapper(NetCommandRef *ref, Int sourceSlot)
{
	if (sourceSlot < 0 || sourceSlot >= MAX_SLOTS || m_connections[sourceSlot] == nullptr ||
		m_connections[sourceSlot]->isQuitting())
		return FALSE;
	const Bool sourceAuthorized = isNetworkCommandSourceAuthorized(ref->getCommand(), sourceSlot);
	if (!rts::network_epoch::IsNetworkFrameRecoveryDelivery(TRUE, ref->getRelay(), m_localSlot, MAX_SLOTS))
		return FALSE;
	const NetWrapperCommandMsg *wrapper = static_cast<NetWrapperCommandMsg *>(ref->getCommand());
	if (!rts::network_epoch::IsNetworkRecoveryWrapperBounded(
		wrapper->getTotalDataLength(), wrapper->getNumChunks()))
		return FALSE;
	const size_t receiptSize = wrapper->getSizeForNetPacket();
	std::array<rts::runtime_epoch::Byte, rts::network_epoch::kNetworkWrapperAckMaxBytes> receiptBytes;
	if (receiptSize == 0U || receiptSize > receiptBytes.size() ||
		wrapper->copyBytesForNetPacket(receiptBytes.data(), *ref) != receiptSize)
		return FALSE;
	const UnsignedInt receiptKey = rts::network_epoch::MakeNetworkWrapperAckKey(
		sourceSlot, wrapper->getPlayerID(), wrapper->getID());
	const UnsignedInt now = static_cast<UnsignedInt>(timeGetTime());
	if (!sourceAuthorized && !isNetworkFrameRecoveryAuthorized(ref->getCommand(), sourceSlot, TRUE))
	{
		// A lost ACK may be retried after catch-up revoked the frame proof.
		// Only a previously accepted exact chunk may be ACKed without it.
		if (m_networkWrapperAckHistory.matches(receiptKey, receiptBytes.data(), receiptSize, now))
			ackNetworkFrameRecoveryCommand(ref, sourceSlot);
		return FALSE;
	}

	// Local-only bounded wrappers always use the same responder-specific list,
	// even when no proof exists or it expires mid-transfer. Their payload may
	// be ordinary file/control data: only decoding can distinguish recovery.
	NetCommandWrapperList *&wrappers = m_networkRecoveryWrappers[sourceSlot];
	if (wrappers == nullptr)
	{
		wrappers = newInstance(NetCommandWrapperList);
		wrappers->init();
	}
	if (!processWrapper(ref, wrappers))
		return FALSE;
	m_networkWrapperAckHistory.remember(receiptKey, receiptBytes.data(), receiptSize, now);
	ackNetworkFrameRecoveryCommand(ref, sourceSlot);
	NetCommandList *ready = wrappers->getReadyCommands();
	Bool accepted = FALSE;
	for (NetCommandRef *command = ready->getFirstMessage(); command; command = command->getNext())
	{
		const Bool decodedSourceAuthorized = isNetworkCommandSourceAuthorized(command->getCommand(), sourceSlot);
		const Bool frameRecoveryDelivery = rts::network_epoch::IsNetworkFrameRecoveryDelivery(
			isNetworkFrameRecoveryAuthorized(command->getCommand(), sourceSlot, FALSE),
			command->getRelay(), m_localSlot, MAX_SLOTS);
		if (!decodedSourceAuthorized && !frameRecoveryDelivery)
			continue;
		if (frameRecoveryDelivery)
			command->setRelay(1U << m_localSlot);
		else if (CommandRequiresAck(command->getCommand()))
			ackCommand(command, m_localSlot);
		if (!processNetCommand(command))
			sendRemoteCommand(command);
		if (frameRecoveryDelivery && command->getCommand()->getNetCommandType() == NETCOMMANDTYPE_FRAMEINFO &&
			m_frameResendRequestOutstanding && sourceSlot == m_frameResendRequestResponder &&
			command->getCommand()->getExecutionFrame() == m_frameResendRequestFrame)
			m_frameResendRequestReceivedInfoMask |= 1U << command->getCommand()->getPlayerID();
		accepted = accepted || frameRecoveryDelivery;
	}
	deleteInstance(ready);
	return accepted;
}

void ConnectionManager::rejectNetworkHello(Int slot, const char *reason)
{
	m_networkHelloFailed = TRUE;
	revokeNetworkSimulationPolicy();
	clearNetworkHelloPendingCommands();
	for (UnsignedInt index = 0; index < m_networkHelloDeferredCount; ++index)
		m_networkHelloDeferred[index].length = 0;
	m_networkHelloDeferredCount = 0U;
	DEBUG_LOG(("ConnectionManager::rejectNetworkHello - rejecting slot %d: %s",
		slot, reason != nullptr ? reason : "invalid NET3 record"));
	if (slot >= 0 && slot < MAX_SLOTS && m_connections[slot] != nullptr)
		m_connections[slot]->setQuitting();
}

void ConnectionManager::dropInvalidNetworkHelloPacket(Int slot, const char *reason)
{
	// Invalid NET3 traffic is consumed by the caller, but it must not alter
	// membership or the handshake gate. A valid retry from the same endpoint
	// may still complete the bounded exchange; an incomplete exchange reaches
	// the normal retry/timeout failure path.
	DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("ConnectionManager::dropInvalidNetworkHelloPacket - dropping packet for slot %d: %s",
		slot, reason != nullptr ? reason : "invalid NET3 peer"));
}

Bool ConnectionManager::isNetworkHelloCandidate(const TransportMessage &message) const
{
	rts::network_epoch::NetworkHelloIdentity identity;
	if (!rts::network_epoch::DecodeNetworkHelloIdentity(
		reinterpret_cast<const rts::runtime_epoch::Byte *>(message.data),
		message.length > 0 ? static_cast<std::size_t>(message.length) : 0U,
		&identity))
	{
		return FALSE;
	}

	const Int slot = findNetworkHelloSlot(identity.senderSlot, identity.recipientSlot);
	return matchesNetworkPeerEndpoint(message, slot);
}

void ConnectionManager::deferNetworkMessage(const TransportMessage &message)
{
	if (m_networkHelloFailed)
		return;

	const Int sourceSlot = findNetworkPeerEndpoint(message);
	if (sourceSlot < 0)
	{
		DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("ConnectionManager::deferNetworkMessage - discarding packet from unknown or unavailable endpoint"));
		return;
	}

	UnsignedInt peerDeferredMessages = 0U;
	for (UnsignedInt index = 0U; index < m_networkHelloDeferredCount; ++index)
	{
		if (matchesNetworkPeerEndpoint(m_networkHelloDeferred[index], sourceSlot))
			++peerDeferredMessages;
	}
	if (rts::network_epoch::IsNetworkHelloDeferredPeerQuotaExceeded(
		peerDeferredMessages, static_cast<UnsignedInt>(MAX_MESSAGES), static_cast<UnsignedInt>(MAX_SLOTS)))
	{
		dropInvalidNetworkHelloPacket(sourceSlot, "NET3 deferred packet queue peer limit exceeded");
		return;
	}

	if (m_networkHelloDeferredCount >= static_cast<UnsignedInt>(MAX_MESSAGES))
	{
		// A full shared queue must not let one packet disconnect unrelated peers.
		// Drop only this incoming packet; the fixed queue remains bounded and the
		// peer can still complete the exchange with a later valid Hello.
		DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("ConnectionManager::deferNetworkMessage - dropping packet from slot %d because the shared queue is full",
			sourceSlot));
		return;
	}

	m_networkHelloDeferred[m_networkHelloDeferredCount] = message;
	++m_networkHelloDeferredCount;
}

Bool ConnectionManager::queueNetworkHelloCommand(NetCommandMsg *msg, UnsignedByte relay)
{
	if (msg == nullptr || m_networkHelloFailed || m_networkHelloPendingCommands == nullptr)
		return FALSE;

	NetCommandRef *existing = m_networkHelloPendingCommands->findMessage(msg);
	if (existing != nullptr)
	{
		// NetCommandList intentionally suppresses equivalent duplicates. Preserve
		// every destination requested by callers instead of silently retaining
		// only the first duplicate's relay mask.
		existing->setRelay(existing->getRelay() | relay);
		return TRUE;
	}
	if (m_networkHelloPendingCommandCount >= static_cast<UnsignedInt>(MAX_MESSAGES))
	{
		rejectNetworkHello(-1, "NET3 pending command queue overflow");
		return FALSE;
	}

	NetCommandRef *ref = m_networkHelloPendingCommands->addMessage(msg);
	if (ref == nullptr)
	{
		rejectNetworkHello(-1, "NET3 pending command queue insertion failure");
		return FALSE;
	}

	ref->setRelay(relay);
	++m_networkHelloPendingCommandCount;
	return TRUE;
}

void ConnectionManager::clearNetworkHelloPendingCommands()
{
	if (m_networkHelloPendingCommands != nullptr)
		m_networkHelloPendingCommands->reset();
	m_networkHelloPendingCommandCount = 0U;
}

void ConnectionManager::drainNetworkHelloPendingCommands()
{
	if (m_networkHelloPendingCommands == nullptr ||
		m_networkHelloPendingCommandCount == 0U)
		return;

	NetCommandRef *ref = m_networkHelloPendingCommands->getFirstMessage();
	while (ref != nullptr)
	{
		NetCommandRef *next = ref->getNext();
		NetCommandMsg *msg = ref->getCommand();
		const UnsignedByte relay = ref->getRelay();

		m_networkHelloPendingCommands->removeMessage(ref);
		if (m_networkHelloPendingCommandCount > 0U)
			--m_networkHelloPendingCommandCount;

		// The pending reference retains ownership while the normal send path
		// creates its own references. Release it only after the immediate send
		// has completed.
		sendLocalCommandImmediate(msg, relay);
		deleteInstance(ref);
		ref = next;
	}

	m_networkHelloPendingCommandCount = 0U;
}

Bool ConnectionManager::processNetworkHello(const TransportMessage &message, Bool enforceFailure)
{
	rts::network_epoch::NetworkHelloKind kind;
	rts::network_epoch::NetworkHelloIdentity identity;
	std::uint64_t receivedSessionToken = 0U;
	rts::runtime_epoch::NetworkHello hello;
	rts::network_epoch::NetworkSimulationPolicyIdentity simulationIdentity;
	const rts::runtime_epoch::ValidationResult result =
		rts::network_epoch::DecodeAndValidateNetworkHelloRecord(
			message.data, static_cast<std::size_t>(message.length),
			TheGlobalData->m_exeCRC, TheGlobalData->m_iniCRC,
			&hello, &kind, &identity, &receivedSessionToken,
			&simulationIdentity);
	if (!result.ok())
	{
		if (enforceFailure)
		{
			rts::network_epoch::NetworkHelloIdentity candidateIdentity;
			Int candidateSlot = -1;
			if (rts::network_epoch::DecodeNetworkHelloIdentity(
				reinterpret_cast<const rts::runtime_epoch::Byte *>(message.data),
				message.length > 0 ? static_cast<std::size_t>(message.length) : 0U,
				&candidateIdentity))
			{
				candidateSlot = findNetworkHelloSlot(candidateIdentity.senderSlot,
					candidateIdentity.recipientSlot);
			}
			if (candidateSlot >= 0)
				dropInvalidNetworkHelloPacket(candidateSlot, "malformed or incompatible NET3 record");
		}
		else
			DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("ConnectionManager::processNetworkHello - ignoring malformed late NET3 record"));
		return FALSE;
	}

	const Int slot = findNetworkHelloSlot(identity.senderSlot, identity.recipientSlot);
	if (slot < 0)
	{
		DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("ConnectionManager::processNetworkHello - ignoring unknown or unavailable NET3 identity"));
		return FALSE;
	}

	if (!rts::network_epoch::IsNetworkHelloSessionTokenAccepted(
		kind, m_networkHelloLocalToken, receivedSessionToken))
	{
		// A delayed record from an earlier exchange must not disconnect the
		// current peer. Keep the gate closed and let the current Hello retry or
		// the bounded timeout resolve the exchange.
		DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("ConnectionManager::processNetworkHello - ignoring stale NET3 token from slot %d", slot));
		return FALSE;
	}
	if (!acceptNetworkSimulationPolicy(slot, simulationIdentity))
	{
		if (enforceFailure)
			dropInvalidNetworkHelloPacket(slot,
				"NET3 simulation policy roster or identity changed");
		return FALSE;
	}

	if (kind == rts::network_epoch::NetworkHelloKind::Hello)
	{
		m_networkHelloRemoteToken[slot] = receivedSessionToken;
		m_networkHelloValidated[slot] = TRUE;
		// A duplicate or newer Hello replaces the remembered peer challenge and
		// is deliberately answered again. This makes the
		// exchange recover when the first Ack was lost or the send queue was
		// temporarily full.
		sendNetworkHelloAck(slot);
		DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("ConnectionManager::processNetworkHello - accepted NET3 hello from slot %d",
			slot));
	}
	else if (kind == rts::network_epoch::NetworkHelloKind::Ack)
	{
		m_networkHelloAckReceived[slot] = TRUE;
		DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("ConnectionManager::processNetworkHello - accepted NET3 ack from slot %d",
			slot));
	}
	return TRUE;
}
#endif

/**
 * zero out the command counts for the given frames.  Presently this is used for
 * the start of a game since there won't be any commands for the first few frames due to runahead.
 */
void ConnectionManager::zeroFrames(UnsignedInt startingFrame, UnsignedInt numFrames) {
	for (Int i = 0; i < MAX_SLOTS; ++i) {
		if (m_frameData[i] != nullptr) {
//			DEBUG_LOG(("Calling zeroFrames on player %d, starting frame %d, numFrames %d", i, startingFrame, numFrames));
			m_frameData[i]->zeroFrames(startingFrame, numFrames);
		}
	}
}

/**
 * Destroy any game messages that are left over due to the run ahead.
 */
void ConnectionManager::destroyGameMessages() {
	for (Int i = 0; i < MAX_SLOTS; ++i) {
		// Need to destroy these game messages because when the game ends, there are
		// still some game messages left over because of the run ahead aspect of
		// network play.
		if (m_frameData[i] != nullptr) {
			m_frameData[i]->destroyGameMessages();
		}
	}
}

/**
 * ConnectionManager::doRelay()
 * Queries the transport for commands that need to be relayed to another client.
 * Get those commands and relay them to the appropriate Connection(s). We make the
 * assumption that a command will only be relayed once.
 */
void ConnectionManager::processTransportMessage(const TransportMessage &message)
{
	#if defined(_WIN64)
	const Int sourceSlot = findNetworkPeerEndpoint(message);
	if (sourceSlot < 0)
	{
		DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("ConnectionManager::processTransportMessage - discarding packet from unknown or unavailable endpoint"));
		return;
	}

	const UnsignedInt now = static_cast<UnsignedInt>(timeGetTime());
	const Bool frameResendExpired = m_frameResendRequestOutstanding &&
		static_cast<UnsignedInt>(now - m_frameResendRequestStartTime) >=
		rts::network_epoch::kNetworkFrameResendResponseTimeoutMs;
	if (frameResendExpired)
		clearNetworkFrameResendRequest();
	Bool frameResendResponseAccepted = FALSE;
	UnsignedInt frameResendInfoMask = 0U;
	#endif

	NetPacket packet(message);
	NetCommandList *cmdList = packet.getCommandList();
	for (NetCommandRef* cmd = cmdList->getFirstMessage(); cmd; cmd = cmd->getNext()) {
		#if defined(_WIN64)
		NetCommandMsg *command = cmd->getCommand();
		const Bool sourceAuthorized = isNetworkCommandSourceAuthorized(cmd->getCommand(), sourceSlot);
		if (command->getNetCommandType() == NETCOMMANDTYPE_WRAPPER)
		{
			const NetWrapperCommandMsg *wrapper = static_cast<NetWrapperCommandMsg *>(command);
			if (rts::network_epoch::ShouldStageNetworkFrameWrapper(sourceAuthorized,
				cmd->getRelay(), m_localSlot, MAX_SLOTS, wrapper->getTotalDataLength(), wrapper->getNumChunks()))
			{
				if (processNetworkFrameRecoveryWrapper(cmd, sourceSlot))
					frameResendResponseAccepted = TRUE;
				continue;
			}
		}
		const Bool isFrameDataCommand = IsCommandSynchronized(command->getNetCommandType());
		const Bool frameResendResponseAuthorized =
			rts::network_epoch::IsNetworkFrameResendResponseAuthorized(
				static_cast<std::uint32_t>(sourceSlot),
				m_frameResendRequestResponder,
				static_cast<std::uint32_t>(command->getPlayerID()),
				m_frameResendRequestExpectedInfoMask,
				static_cast<std::uint32_t>(MAX_SLOTS),
				m_frameResendRequestOutstanding,
				frameResendExpired,
				isFrameDataCommand,
				static_cast<std::uint32_t>(command->getExecutionFrame()),
				static_cast<std::uint32_t>(m_frameResendRequestFrame));
		const Bool frameRecoveryAuthorized = isNetworkFrameRecoveryAuthorized(command, sourceSlot, FALSE);
		Bool frameRecoveryDelivery = rts::network_epoch::IsNetworkFrameRecoveryDelivery(
			frameResendResponseAuthorized || frameRecoveryAuthorized, cmd->getRelay(), m_localSlot, MAX_SLOTS);
		if (m_lockstepV2ReceiptRecorder.isActive() &&
			m_lockstepV2Session.originMode ==
				rts::lockstep_v2::CommandOriginMode::DirectAuthenticated)
		{
			// A direct-origin v2 session cannot let a responder republish cached
			// commands authored by a third peer.  Recovery of that shape needs a
			// separately authenticated per-origin envelope; until then the normal
			// source check remains fail-closed.
			frameRecoveryDelivery = FALSE;
		}
		const Bool directFrameAck = frameRecoveryDelivery || rts::network_epoch::ShouldAckNetworkDirectFrame(
			sourceAuthorized, isFrameDataCommand, cmd->getRelay(), m_localSlot, MAX_SLOTS,
			command->getExecutionFrame(), TheGameLogic->getFrame());
		if (!sourceAuthorized && !frameRecoveryDelivery)
		{
			if (directFrameAck)
				ackNetworkFrameRecoveryCommand(cmd, sourceSlot);
			DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("ConnectionManager::processTransportMessage - discarding command with mismatched claimed source"));
			continue;
		}
		if (frameResendResponseAuthorized && frameRecoveryDelivery)
		{
			frameResendResponseAccepted = TRUE;
			if (command->getNetCommandType() == NETCOMMANDTYPE_FRAMEINFO)
				frameResendInfoMask |= 1U << command->getPlayerID();
		}
		if (frameRecoveryDelivery)
		{
			// A recovery permission is local publication, never relay authority.
			cmd->setRelay(1U << m_localSlot);
		}
		if (directFrameAck && CommandRequiresAck(command))
			ackNetworkFrameRecoveryCommand(cmd, sourceSlot);
		else
		#endif
		if (CommandRequiresAck(cmd->getCommand())) {
			ackCommand(cmd, m_localSlot);
		}
	#if defined(_WIN64)
		if (m_lockstepV2ReceiptRecorder.isActive() &&
			command->getNetCommandType() == NETCOMMANDTYPE_GAMECOMMAND &&
			command->getExecutionFrame() > 0U &&
			command->getExecutionFrame() <= rts::lockstep_v2::kCommonStopFrame &&
			command->getExecutionFrame() >= TheGameLogic->getFrame())
		{
			std::uint64_t commandDigest = 0U;
			const Bool digestValid = getLockstepV2CommandDigest(cmd, &commandDigest);
			FrameDataManager *originFrameData = command->getPlayerID() < MAX_SLOTS ?
				m_frameData[command->getPlayerID()] : nullptr;
			NetCommandList *originCommands = originFrameData != nullptr ?
				originFrameData->getFrameCommandList(command->getExecutionFrame()) : nullptr;
			NetCommandRef *existingCommand = originCommands != nullptr ?
				originCommands->findMessage(command->getID(), command->getPlayerID()) : nullptr;
			std::uint64_t existingDigest = 0U;
			const Bool existingDigestValid = existingCommand == nullptr ||
				getLockstepV2CommandDigest(existingCommand, &existingDigest);
			const Bool contributionAccepted = digestValid && originFrameData != nullptr &&
				(existingCommand == nullptr ?
					recordLockstepV2Command(command->getExecutionFrame(),
						command->getPlayerID(), command->getID(), commandDigest) :
					(existingDigestValid && existingDigest == commandDigest));
			if (!contributionAccepted)
			{
				m_lockstepV2ReceiptRecorder.reset();
			}
		}
	#endif
		if (!processNetCommand(cmd)) {
			sendRemoteCommand(cmd);
		}
	}
	deleteInstance(cmdList);

	#if defined(_WIN64)
	if (frameResendInfoMask != 0U)
		m_frameResendRequestReceivedInfoMask |= frameResendInfoMask;
	if (frameResendResponseAccepted && m_frameResendRequestOutstanding)
	{
		UnsignedInt frameResendReadyCommandMask = 0U;
		for (Int sourceSlot = 0; sourceSlot < MAX_SLOTS; ++sourceSlot)
		{
			const UnsignedInt sourceMask = 1U << sourceSlot;
			if ((m_frameResendRequestExpectedInfoMask & sourceMask) != 0U &&
				m_frameData[sourceSlot] != nullptr &&
				m_frameData[sourceSlot]->getFrameCommandCount(m_frameResendRequestFrame) ==
				m_frameData[sourceSlot]->getCommandCount(m_frameResendRequestFrame))
			{
				frameResendReadyCommandMask |= sourceMask;
			}
		}
		if (rts::network_epoch::IsNetworkFrameResendResponseComplete(
			m_frameResendRequestExpectedInfoMask,
			m_frameResendRequestReceivedInfoMask,
			frameResendReadyCommandMask))
		{
			clearNetworkFrameResendRequest();
		}
	}
	#endif
}

void ConnectionManager::doRelay() {
	#if defined(_WIN64)
	if (m_networkHelloFailed)
	{
		// Compatibility or CSPRNG failure is terminal for this network
		// instance. Never allow queued gameplay traffic to bypass the gate.
		for (size_t i = 0; i < ARRAY_SIZE(m_transport->m_inBuffer); ++i)
			m_transport->m_inBuffer[i].length = 0;
		return;
	}
	if (!m_networkHelloRequired && m_networkHelloDeferredCount > 0U)
	{
		for (UnsignedInt index = 0; index < m_networkHelloDeferredCount; ++index)
		{
			if (m_networkHelloDeferred[index].length > 0)
				processTransportMessage(m_networkHelloDeferred[index]);
			m_networkHelloDeferred[index].length = 0;
		}
		m_networkHelloDeferredCount = 0U;
	}
	#endif

	for (size_t i = 0; i < ARRAY_SIZE(m_transport->m_inBuffer); ++i) {
		if (m_transport->m_inBuffer[i].length > 0) {
			// This transport buffer has yet to be processed.
#if defined(_WIN64)
		TransportMessage &message = m_transport->m_inBuffer[i];
		const std::size_t messageLength = static_cast<std::size_t>(message.length);
		const bool hasNetworkHelloPrefix = rts::network_epoch::HasNetworkHelloPrefix(message.data, messageLength);
		const Int sourceSlot = findNetworkPeerEndpoint(message);
		const bool endpointKnown = sourceSlot >= 0;
		if (hasNetworkHelloPrefix)
		{
			if (!m_networkHelloStarted)
			{
				// A peer may finish sending while this process is still building
				// its GameInfo connection list. Do not acknowledge pre-start
				// records: beginNetworkHello() will send a fresh Hello and the
				// peer will retry its record if needed.
				DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("ConnectionManager::doRelay - ignoring pre-start NET3 record"));
				message.length = 0;
				continue;
			}

			const bool hasNetworkHelloMagic = rts::network_epoch::HasNetworkHelloMagic(message.data, messageLength);
			const rts::network_epoch::NetworkIngressDisposition disposition =
				rts::network_epoch::ClassifyNetworkIngress(true, hasNetworkHelloMagic,
					m_networkHelloRequired, endpointKnown, hasNetworkHelloMagic && isNetworkHelloCandidate(message));
			if (disposition == rts::network_epoch::NetworkIngressDisposition::Process)
				processNetworkHello(message, m_networkHelloRequired);
			else if (disposition == rts::network_epoch::NetworkIngressDisposition::Quarantine)
			{
				if (sourceSlot >= 0)
					dropInvalidNetworkHelloPacket(sourceSlot, "malformed or unsupported NET3 payload");
				else
					DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("ConnectionManager::doRelay - discarding malformed or unsupported NET3 payload"));
			}
			else
				DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("ConnectionManager::doRelay - discarding unknown NET3 payload"));
			message.length = 0;
			continue;
		}

		if (m_networkHelloRequired)
		{
			const rts::network_epoch::NetworkIngressDisposition disposition =
				rts::network_epoch::ClassifyNetworkIngress(false, false, true, endpointKnown, false);
			if (disposition == rts::network_epoch::NetworkIngressDisposition::Defer)
				deferNetworkMessage(message);
			else
				DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("ConnectionManager::doRelay - discarding packet from unknown peer endpoint"));
			message.length = 0;
			continue;
		}

		const rts::network_epoch::NetworkIngressDisposition disposition =
			rts::network_epoch::ClassifyNetworkIngress(false, false, false, endpointKnown, false);
		if (disposition == rts::network_epoch::NetworkIngressDisposition::Process)
			processTransportMessage(message);
		else
			DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("ConnectionManager::doRelay - discarding gameplay packet from unknown peer endpoint"));
		message.length = 0;
		continue;
#endif

			// make a NetPacket out of this data so it can be broken up into individual commands.
			NetPacket packet(m_transport->m_inBuffer[i]);

			//DEBUG_LOG(("ConnectionManager::doRelay() - got a packet with %d commands", packet.getNumCommands()));
			//LOGBUFFER( packet.getData(), packet.getLength() );

			// Get the command list from the packet.
			NetCommandList *cmdList = packet.getCommandList();

			// Iterate through the commands in this packet and send them to the proper connections.
			for (NetCommandRef* cmd = cmdList->getFirstMessage(); cmd; cmd = cmd->getNext()) {
				//DEBUG_LOG(("ConnectionManager::doRelay() - Looking at a command of type %s",
					//GetNetCommandTypeAsString(cmd->getCommand()->getNetCommandType())));

				if (CommandRequiresAck(cmd->getCommand())) {
					ackCommand(cmd, m_localSlot);
				}
				if (!processNetCommand(cmd)) {
					sendRemoteCommand(cmd);
				}
			}

			deleteInstance(cmdList);
			cmdList = nullptr;

			// signal that this has been processed.
			m_transport->m_inBuffer[i].length = 0;
		}
#if !defined(_WIN64)
		else {
			break;
		}
#endif
	}

#if defined(_WIN64)
	// Wrapper chunks are ordinary gameplay traffic and must remain held until
	// every peer has passed the compatibility exchange.
	if (m_networkHelloRequired)
		return;
	for (Int sourceSlot = 0; sourceSlot < MAX_SLOTS; ++sourceSlot)
		if (m_networkRecoveryWrappers[sourceSlot] != nullptr)
			m_networkRecoveryWrappers[sourceSlot]->purgeExpired(static_cast<UnsignedInt>(timeGetTime()));
#endif

	NetCommandList *cmdList = m_netCommandWrapperList->getReadyCommands();
	for (NetCommandRef* cmd = cmdList->getFirstMessage(); cmd; cmd = cmd->getNext()) {
		if (CommandRequiresAck(cmd->getCommand())) {
			ackCommand(cmd, m_localSlot);
		}
		if (!processNetCommand(cmd)) {
			sendRemoteCommand(cmd);
		}
	}

	deleteInstance(cmdList);
	cmdList = nullptr;
}

/**
 * This is where the non-synchronized network commands should be processed.
 * Return TRUE if the command should not be relayed. Return FALSE if it should be relayed.
 */
Bool ConnectionManager::processNetCommand(NetCommandRef *ref) {
	NetCommandMsg *msg = ref->getCommand();
	NetCommandType cmdType = msg->getNetCommandType();

	// Every command path below indexes player-owned state or relay bits.
	if (msg->getPlayerID() >= MAX_SLOTS) {
		return TRUE;
	}

	if ((cmdType == NETCOMMANDTYPE_ACKSTAGE1) ||
			(cmdType == NETCOMMANDTYPE_ACKSTAGE2) ||
			(cmdType == NETCOMMANDTYPE_ACKBOTH)) {
		processAck(msg);
		return FALSE;
	}

	if ((m_connections[msg->getPlayerID()] == nullptr) && (msg->getPlayerID() != m_localSlot)) {
		// if this is from a player that is no longer in the game, then ignore them.
		return TRUE;
	}

	// Handle WRAPPER commands (before second connection validation)
	if (cmdType == NETCOMMANDTYPE_WRAPPER) {
		processWrapper(ref); // need to send the NetCommandRef since we have to construct the relay for the wrapped command.
		return FALSE;
	}

	if (msg->getPlayerID() != m_localSlot) {
		if (m_connections[msg->getPlayerID()] == nullptr) {
			return TRUE;
		}
	}

	// Don't allow an out of date command to be sent through.
	// Its unnecessary traffic and it could cause problems.
	//
	// This was a fix for a command count bug where a command would be
	// executed, then a command for that old frame would be added to the
	// FrameData for that frame + 256, and would screw up the command count.
	if (IsCommandSynchronized(cmdType)) {
		if (ref->getCommand()->getExecutionFrame() < TheGameLogic->getFrame()) {
			return TRUE;
		}
	}

	// Handle disconnect commands as a range
	if ((cmdType > NETCOMMANDTYPE_DISCONNECTSTART) && (cmdType < NETCOMMANDTYPE_DISCONNECTEND)) {
		m_disconnectManager->processDisconnectCommand(ref, this);
		return TRUE;
	}

	// Process command by type
	switch (cmdType) {

		case NETCOMMANDTYPE_FRAMEINFO: {
			processFrameInfo((NetFrameCommandMsg *)msg);
			// need to set the relay so we don't send it to ourselves.
			UnsignedByte relay = ref->getRelay();
			relay = relay & (0xff ^ (1 << m_localSlot));
			ref->setRelay(relay);
			return FALSE;
		}

		case NETCOMMANDTYPE_PROGRESS: {
			//DEBUG_LOG(("ConnectionManager::processNetCommand - got a progress net command from player %d", msg->getPlayerID()));
			processProgress((NetProgressCommandMsg *) msg);
			// need to set the relay so we don't send it to ourselves.
			UnsignedByte relay = ref->getRelay();
			relay = relay & (0xff ^ (1 << m_localSlot));
			ref->setRelay(relay);
			return FALSE;
		}

		case NETCOMMANDTYPE_TIMEOUTSTART:
			DEBUG_LOG(("ConnectionManager::processNetCommand - got a TimeOut GameStart net command from player %d", msg->getPlayerID()));
			processTimeOutGameStart(msg);
			return FALSE;

		case NETCOMMANDTYPE_RUNAHEADMETRICS:
			processRunAheadMetrics((NetRunAheadMetricsCommandMsg *)msg);
			return TRUE;

		case NETCOMMANDTYPE_KEEPALIVE:
			return TRUE;

		case NETCOMMANDTYPE_DISCONNECTCHAT:
			processDisconnectChat((NetDisconnectChatCommandMsg *)msg);
			return FALSE;

		case NETCOMMANDTYPE_LOADCOMPLETE:
			DEBUG_LOG(("ConnectionManager::processNetCommand - got a Load Complete net command from player %d", msg->getPlayerID()));
			processLoadComplete(msg);
			return FALSE;

		case NETCOMMANDTYPE_CHAT:
			processChat((NetChatCommandMsg *)msg);
			return FALSE;

		case NETCOMMANDTYPE_FILE:
			processFile((NetFileCommandMsg *)msg);
			return FALSE;

		case NETCOMMANDTYPE_FILEANNOUNCE:
			processFileAnnounce((NetFileAnnounceCommandMsg *)msg);
			return FALSE;

		case NETCOMMANDTYPE_FILEPROGRESS:
			processFileProgress((NetFileProgressCommandMsg *)msg);
			return FALSE;

		case NETCOMMANDTYPE_FRAMERESENDREQUEST:
			processFrameResendRequest((NetFrameResendRequestCommandMsg *)msg);
			return TRUE;

		default:
			return FALSE;
	}
}

void ConnectionManager::processFrameResendRequest(NetFrameResendRequestCommandMsg *msg) {
	// first make sure this is a valid slot
	const UnsignedInt playerID = msg->getPlayerID();
	if (playerID >= MAX_SLOTS) {
		return;
	}

	// make sure this player is still in our game.
	if ((m_connections[playerID] == nullptr) || (m_connections[playerID]->isQuitting() == TRUE)) {
		return;
	}

#if defined(_WIN64)
	// Native provenance is exactly the requested frame, not an open-ended
	// permission for every subsequent command in the responder's cache.
	sendSingleFrameToPlayer(playerID, msg->getFrameToResend());
#else
	sendFrameDataToPlayer(playerID, msg->getFrameToResend());
#endif
}

/**
 * We have received a wrapper for a command too big to fit in a packet.
 */
Bool ConnectionManager::processWrapper(NetCommandRef *ref, NetCommandWrapperList *wrappers)
{
	if (wrappers == nullptr)
		wrappers = m_netCommandWrapperList;
	NetWrapperCommandMsg *wrapperMsg = (NetWrapperCommandMsg *)(ref->getCommand());
	UnsignedShort commandID = wrapperMsg->getWrappedCommandID();
	DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("ConnectionManager::processWrapper() - wrapped commandID is %d, commandID is %d",
		commandID, wrapperMsg->getID()));
	Int origProgress = 0;
	FileCommandMap::iterator fcIt = s_fileCommandMap.find(commandID);
	if (fcIt != s_fileCommandMap.end())
	{
		origProgress = s_fileProgressMap[m_localSlot][commandID];
	}
	DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("ConnectionManager::processWrapper() - origProgress[%d] == %d for command %d",
		m_localSlot, origProgress, commandID));

	const Bool accepted = wrappers->processWrapper(ref);

	if (accepted && fcIt != s_fileCommandMap.end())
	{
		Int newProgress = wrappers->getPercentComplete(wrapperMsg->getPlayerID(), commandID);
		DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("ConnectionManager::processWrapper() - newProgress[%d] == %d for command %d",
			m_localSlot, newProgress, commandID));
		if (newProgress > origProgress && newProgress < 100)
		{
			DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("ConnectionManager::processWrapper() - sending a NetFileProgressCommandMsg"));
			s_fileProgressMap[m_localSlot][commandID] = newProgress;

			Int progressMask = 0xff ^ (1 << m_localSlot);
			NetFileProgressCommandMsg *msg = newInstance(NetFileProgressCommandMsg);
			msg->setPlayerID(m_localSlot);
			msg->setID(0);
			if (DoesCommandRequireACommandID(msg->getNetCommandType()))
			{
				msg->setID(GenerateNextCommandID());
			}
			msg->setFileID(commandID);
			msg->setProgress(newProgress);
			sendLocalCommand(msg, progressMask);
			processFileProgress(msg);
			msg->detach();
		}
	}
	return accepted;
}

/**
 * A client has sent us their run ahead metrics, lets store them away for future calculations.
 */
void ConnectionManager::processRunAheadMetrics(NetRunAheadMetricsCommandMsg *msg)
{
	const UnsignedInt playerID = msg->getPlayerID();
	if (playerID >= MAX_SLOTS) {
		return;
	}
	if (isPlayerConnected(playerID)) {
		m_latencyAverages[playerID] = msg->getAverageLatency();
		m_fpsAverages[playerID] = msg->getAverageFps();
		//DEBUG_LOG(("ConnectionManager::processRunAheadMetrics - player %d, fps = %d, latency = %f", player, msg->getAverageFps(), msg->getAverageLatency()));
		if (m_fpsAverages[playerID] > 100) {
			// limit the reported frame rate average to 100.  This is done because if a
			// user alt-tab's out of the game their frame rate climbs to in the neighborhood of
			// 300, that was deemed "ugly" by the powers that be.
			m_fpsAverages[playerID] = 100;
		}
	}
}

void ConnectionManager::processDisconnectChat(NetDisconnectChatCommandMsg *msg)
{
	UnicodeString unitext;
	UnicodeString name;
	const UnsignedInt playerID = msg->getPlayerID();
	if (playerID >= MAX_SLOTS) {
		return;
	}
	if (playerID == m_localSlot) {
		name = m_localUser->GetName();
	} else if (isPlayerConnected(playerID)) {
		name = m_connections[playerID]->getUser()->GetName();
	}
	unitext.format(L"[%ls] %ls", name.str(), msg->getText().str());
//	DEBUG_LOG(("ConnectionManager::processDisconnectChat - got message from player %d, message is %ls", playerID, unitext.str()));
	TheDisconnectMenu->showChat(unitext); // <-- need to implement this
}

void ConnectionManager::processChat(NetChatCommandMsg *msg)
{
	UnicodeString unitext;
	UnicodeString name;
	const UnsignedInt playerID = msg->getPlayerID();
	if (playerID >= MAX_SLOTS) {
		return;
	}
	//DEBUG_LOG(("processChat(): playerID = %d", playerID));
	if (playerID == m_localSlot) {
		name = m_localUser->GetName();
		//DEBUG_LOG(("connection is null, using %ls", name.str()));
	} else if ((m_connections[playerID] != nullptr) && (m_connections[playerID]->isQuitting() == FALSE)) {
		name = m_connections[playerID]->getUser()->GetName();
		//DEBUG_LOG(("connection is non-null, using %ls", name.str()));
	}
	unitext.format(L"[%ls] %ls", name.str(), msg->getText().str());
//	DEBUG_LOG(("ConnectionManager::processChat - got message from player %d (mask %8.8X), message is %ls", playerID, msg->getPlayerMask(), unitext.str()));

	const Player *player = ThePlayerList->getPlayerFromSlotIndex(playerID);
	if (!player)
	{
		TheInGameUI->message(L"%ls", unitext.str());
		return;
	}

	Bool fromObserver = !player->isPlayerActive();
	Bool amIObserver = !ThePlayerList->getLocalPlayer()->isPlayerActive();
	Bool canSeeChat = (amIObserver || !fromObserver) && !TheGameInfo->getConstSlot(playerID)->isMuted();

	if ( ((1<<m_localSlot) & msg->getPlayerMask() ) && canSeeChat  )
	{
		RGBColor rgb;
		rgb.setFromInt(player->getPlayerColor());
		TheInGameUI->messageColor(&rgb, L"%ls", unitext.str());

		// feedback for received chat messages in-game
		AudioEventRTS audioEvent("GUICommunicatorIncoming");
		TheAudio->addAudioEvent(&audioEvent);
	}
}

void ConnectionManager::processFile(NetFileCommandMsg *msg)
{
	if (msg->getFileLength() == 0)
	{
		DEBUG_LOG(("Ignoring empty network file transfer"));
		return;
	}
#ifdef DEBUG_LOGGING
	UnicodeString log;
	log.format(L"Saw file transfer: '%hs' of %d bytes from %d", msg->getPortableFilename().str(), msg->getFileLength(), msg->getPlayerID());
	DEBUG_LOG(("%ls", log.str()));
#endif

	AsciiString realFileName = msg->getRealFilename();
	if (realFileName.isEmpty())
	{
		// TheSuperHackers @security slurmlord 18/06/2025 As the file name/path from the NetFileCommandMsg failed to normalize,
		// in other words is bogus and points outside of the approved target directory, avoid an arbitrary file overwrite vulnerability
		// by simply returning and let the transfer time out.
		DEBUG_LOG(("Got a file name transferred that failed to normalize: '%s'!", msg->getPortableFilename().str()));
		return;
	}

	// TheSuperHackers @security bobtista 06/11/2025 Validate file extension to prevent arbitrary file types
	if (!hasValidTransferFileExtension(realFileName))
	{
		DEBUG_LOG(("File '%s' has invalid extension for transfer operations.", realFileName.str()));
		return;
	}

	if (TheFileSystem->doesFileExist(realFileName.str()))
	{
		DEBUG_LOG(("File exists already!"));
		//return;
	}

	UnsignedByte *buf = msg->getFileData();
	Int len = msg->getFileLength();

	// uncompress Targas
#ifdef COMPRESS_TARGAS
	Bool deleteBuf = FALSE;
	if (msg->getPortableFilename().endsWith(".tga") && CompressionManager::isDataCompressed(buf, len))
	{
		Int uncompLen = CompressionManager::getUncompressedSize(buf, len);
		UnsignedByte *uncompBuffer = NEW UnsignedByte[uncompLen];
		Int actualLen = CompressionManager::decompressData(buf, len, uncompBuffer, uncompLen);
		if (actualLen == uncompLen)
		{
			DEBUG_LOG(("Uncompressed Targa after map transfer"));
			deleteBuf = TRUE;
			buf = uncompBuffer;
			len = uncompLen;
		}
		else
		{
			DEBUG_LOG(("Failed to uncompress Targa after map transfer"));
			delete[] uncompBuffer; // failed to decompress, so just use the source
		}
	}
#endif // COMPRESS_TARGAS

	// TheSuperHackers @security bobtista 12/02/2026 Validate file content in memory before writing to disk
	if (!hasValidTransferFileContent(realFileName, buf, len))
	{
		DEBUG_LOG(("File '%s' failed content validation. Transfer aborted.", realFileName.str()));
#ifdef COMPRESS_TARGAS
		if (deleteBuf)
		{
			delete[] buf;
			buf = nullptr;
		}
#endif // COMPRESS_TARGAS
		return;
	}

	File *fp = TheFileSystem->openFile(realFileName.str(), File::CREATE | File::BINARY | File::WRITE);
	if (fp)
	{
		fp->write(buf, len);
		fp->close();
		fp = nullptr;
		DEBUG_LOG(("Wrote %d bytes to file %s!", len, realFileName.str()));

	}
	else
	{
		DEBUG_LOG(("Cannot open file!"));
	}

	DEBUG_LOG(("ConnectionManager::processFile() - sending a NetFileProgressCommandMsg"));

	Int commandID = msg->getID();
	Int newProgress = 100;

	s_fileProgressMap[m_localSlot][commandID] = newProgress;

	Int progressMask = 0xff ^ (1 << m_localSlot);
	NetFileProgressCommandMsg *progressMsg = newInstance(NetFileProgressCommandMsg);
	progressMsg->setPlayerID(m_localSlot);
	progressMsg->setID(0);
	if (DoesCommandRequireACommandID(progressMsg->getNetCommandType()))
	{
		progressMsg->setID(GenerateNextCommandID());
	}
	progressMsg->setFileID(commandID);
	progressMsg->setProgress(newProgress);
	sendLocalCommand(progressMsg, progressMask);
	processFileProgress(progressMsg);
	progressMsg->detach();

#ifdef COMPRESS_TARGAS
	if (deleteBuf)
	{
		delete[] buf;
		buf = nullptr;
	}
#endif // COMPRESS_TARGAS
}

void ConnectionManager::processFileAnnounce(NetFileAnnounceCommandMsg *msg)
{
	DEBUG_LOG(("ConnectionManager::processFileAnnounce() - expecting '%s' (%s) in command %d", msg->getPortableFilename().str(), msg->getRealFilename().str(), msg->getFileID()));
	s_fileCommandMap[msg->getFileID()] = msg->getRealFilename();
	s_fileRecipientMaskMap[msg->getFileID()] = msg->getPlayerMask();
	for (Int i=0; i<MAX_SLOTS; ++i)
	{
		if ( (1<<i) & msg->getPlayerMask() )
		{
			s_fileProgressMap[i][msg->getFileID()] = 0;
		}
		else
		{
			s_fileProgressMap[i][msg->getFileID()] = 100; // they don't need to get it, so they're already done.
		}
	}
}

void ConnectionManager::processFileProgress(NetFileProgressCommandMsg *msg)
{
	DEBUG_LOG(("ConnectionManager::processFileProgress() - command %d is at %d%%",
		msg->getFileID(), msg->getProgress()));

	const UnsignedInt playerID = msg->getPlayerID();
	if (playerID >= MAX_SLOTS) {
		return;
	}
	const UnsignedShort fileID = msg->getFileID();
	const Int oldProgress = s_fileProgressMap[playerID][fileID];
	s_fileProgressMap[playerID][fileID] = max(oldProgress, msg->getProgress());
}

void ConnectionManager::processProgress( NetProgressCommandMsg *msg )
{
	TheGameLogic->processProgress(msg->getPlayerID(), msg->getPercentage());
}

void ConnectionManager::processLoadComplete( NetCommandMsg *msg )
{
	TheGameLogic->processProgressComplete(msg->getPlayerID());
}

void ConnectionManager::processTimeOutGameStart( NetCommandMsg *msg )
{
	TheGameLogic->timeOutGameStart();
}

/**
 * Another client has sent us the command count for a new frame.
 */
void ConnectionManager::processFrameInfo(NetFrameCommandMsg *msg) {
	//stupid frame info, why don't you process yourself?

	const UnsignedInt playerID = msg->getPlayerID();

	if (playerID < MAX_SLOTS) {
		if (m_frameData[playerID] != nullptr) {
//			DEBUG_LOG(("ConnectionManager::processFrameInfo - player %d, frame %d, command count %d, received on frame %d", playerID, msg->getExecutionFrame(), msg->getCommandCount(), TheGameLogic->getFrame()));
			m_frameData[playerID]->setFrameCommandCount(msg->getExecutionFrame(), msg->getCommandCount());
		}
	}
}

/**
 * We just got a stage 1 ack from someone.  So we should remove it from the connection that sent it so
 * it doesn't keep resending it.
 */
void ConnectionManager::processAckStage1(NetCommandMsg *msg) {
#if defined(RTS_DEBUG)
	Bool doDebug = (msg->getNetCommandType() == NETCOMMANDTYPE_DISCONNECTFRAME) ? TRUE : FALSE;
#endif

	const UnsignedInt playerID = msg->getPlayerID();
	NetCommandRef *ref = nullptr;

#if defined(RTS_DEBUG)
	if (doDebug == TRUE) {
		DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("ConnectionManager::processAck - processing ack for command %d from player %d", ((NetAckStage1CommandMsg *)msg)->getCommandID(), playerID));
	}
#endif

	if (playerID < MAX_SLOTS) {
		if (m_connections[playerID] != nullptr) {
			ref = m_connections[playerID]->processAck(msg);
		}
	} else {
		DEBUG_CRASH(("ConnectionManager::processAck - %d is an invalid player number", playerID));
	}

	if (ref != nullptr) {
		if (ref->getCommand()->getNetCommandType() == NETCOMMANDTYPE_FRAMEINFO) {
			m_frameMetrics.processLatencyResponse(((NetFrameCommandMsg *)(ref->getCommand()))->getExecutionFrame());
		}

		deleteInstance(ref);
		ref = nullptr;
	}
}

/**
 * We just got a stage 2 ack from someone.  So remove it from the pending commands list so it doesn't
 * get sent in the case of a new packet router.
 */
void ConnectionManager::processAckStage2(NetCommandMsg *msg) {
	UnsignedShort commandID = 0;
	UnsignedByte playerID = 0;
	if (msg->getNetCommandType() == NETCOMMANDTYPE_ACKSTAGE2) {
		commandID = ((NetAckStage2CommandMsg *)msg)->getCommandID();
		playerID = ((NetAckStage2CommandMsg *)msg)->getOriginalPlayerID();
	} else if (msg->getNetCommandType() == NETCOMMANDTYPE_ACKBOTH) {
		commandID = ((NetAckBothCommandMsg *)msg)->getCommandID();
		playerID = ((NetAckBothCommandMsg *)msg)->getOriginalPlayerID();
	} else {
		return;
	}
	if (playerID >= MAX_SLOTS || msg->getPlayerID() >= MAX_SLOTS)
		return;

	NetCommandRef *ref = m_pendingCommands->findMessage(commandID, playerID);
	if (ref != nullptr) {
		//DEBUG_LOG(("ConnectionManager::processAckStage2 - removing command %d from the pending commands list.", commandID));
		DEBUG_ASSERTCRASH((m_localSlot == playerID), ("Found a command in the pending commands list that wasn't originated by the local player"));
		m_pendingCommands->removeMessage(ref);
		deleteInstance(ref);
		ref = nullptr;
	} else {
		//DEBUG_LOG(("ConnectionManager::processAckStage2 - Couldn't find command %d from player %d in the pending commands list.", commandID, playerID));
	}

	ref = m_relayedCommands->findMessage(commandID, playerID);
	if (ref != nullptr) {
		//DEBUG_LOG(("ConnectionManager::processAckStage2 - found command ID %d from player %d in the relayed commands list.", commandID, playerID));
		UnsignedByte prevRelay = ref->getRelay();
		UnsignedByte relay = prevRelay & ~(1 << msg->getPlayerID());
		//DEBUG_LOG(("ConnectionManager::processAckStage2 - relay was %d and is now %d", relay, prevRelay));
		if (relay == 0) {
			//DEBUG_LOG(("ConnectionManager::processAckStage2 - relay is 0, removing command from the relayed commands list."));
			m_relayedCommands->removeMessage(ref);
			NetAckStage2CommandMsg *ackmsg = newInstance(NetAckStage2CommandMsg)(ref->getCommand());
			sendLocalCommand(ackmsg, 1 << ackmsg->getOriginalPlayerID());
			deleteInstance(ref);
			ref = nullptr;

			ackmsg->detach();
			ackmsg = nullptr;
		} else {
			ref->setRelay(relay);
		}
	}
}

/**
 * We just got a "both" ack from someone.  So process it as both a stage 1 and stage 2 ack.
 */
void ConnectionManager::processAck(NetCommandMsg *msg) {
	if ((msg->getNetCommandType() == NETCOMMANDTYPE_ACKSTAGE1) || (msg->getNetCommandType() == NETCOMMANDTYPE_ACKBOTH)) {
		processAckStage1(msg);
	}
	if ((msg->getNetCommandType() == NETCOMMANDTYPE_ACKSTAGE2) || (msg->getNetCommandType() == NETCOMMANDTYPE_ACKBOTH)) {
		processAckStage2(msg);
	}
}

/**
 * A player has just left our game. Delete their connection and frame data manager.
 * return codes are:
 * PLAYERLEAVECODE_UNKNOWN - player didn't have a valid slot number.
 * PLAYERLEAVECODE_CLIENT - someone in the game that wasn't us or the packet router.
 * PLAYERLEAVECODE_LOCAL - We are leaving the game, we could also be the packet router.
 * PLAYERLEAVECODE_PACKETROUTER - The packet router left the game.
 *
 * If we are leaving and are also the packet router, it will return the PLAYERLEAVECODE_LOCAL return code.
 */
PlayerLeaveCode ConnectionManager::processPlayerLeave(NetPlayerLeaveCommandMsg *msg) {
	UnsignedByte playerID = msg->getLeavingPlayerID();
	if (playerID >= MAX_SLOTS)
		return PLAYERLEAVECODE_UNKNOWN;

#if defined(_WIN64)
	if (playerID == m_localSlot ||
		(m_networkHelloExpectedSlots & (1U << playerID)) != 0U)
	{
		revokeNetworkSimulationPolicy();
	}
#endif

	if ((playerID != m_localSlot) && (m_connections[playerID] != nullptr)) {
		DEBUG_LOG(("ConnectionManager::processPlayerLeave() - setQuitting() on player %d on frame %d", playerID, TheGameLogic->getFrame()));
		m_connections[playerID]->setQuitting();
	}
	DEBUG_ASSERTCRASH(m_frameData[playerID] == nullptr || m_frameData[playerID]->getIsQuitting() == FALSE,
		("Player %d is already quitting", playerID));
	if ((playerID != m_localSlot) && (m_frameData[playerID] != nullptr) && (m_frameData[playerID]->getIsQuitting() == FALSE)) {
		DEBUG_LOG(("ConnectionManager::processPlayerLeave - setQuitFrame on player %d for frame %d", playerID, TheGameLogic->getFrame()+1));
		m_frameData[playerID]->setQuitFrame(TheGameLogic->getFrame() + FRAMES_TO_KEEP + 1);
	}

	if (playerID == m_localSlot)
	{
		// we're leaving, so mark our connections and frame datas to go away.
		for (Int i=0; i<MAX_SLOTS; ++i)
		{
			if (m_connections[i])
			{
				m_connections[i]->clearCommandsExceptFrom(m_localSlot);
				m_connections[i]->setQuitting();
			}
		}
	}

	PlayerLeaveCode code = disconnectPlayer(playerID);
	DEBUG_LOG(("ConnectionManager::processPlayerLeave() - just disconnected player %d with ret code %d", playerID, code));
	if (code == PLAYERLEAVECODE_PACKETROUTER)
		resendPendingCommands();

	PopulateInGameDiplomacyPopup();
	return code;
}

UnsignedInt ConnectionManager::getPacketRouterFallbackSlot(Int packetRouterNumber) {
	if ((packetRouterNumber >= 0) && (packetRouterNumber < MAX_SLOTS)) {
		return m_packetRouterFallback[packetRouterNumber];
	}
	return MAX_SLOTS;
}

UnsignedInt ConnectionManager::getPacketRouterSlot() {
	return m_packetRouterSlot;
}

Bool ConnectionManager::areAllQueuesEmpty() {
	Bool retval = TRUE;
	for (Int i = 0; (i < MAX_SLOTS) && retval; ++i) {
		if (m_connections[i] != nullptr) {
			if (m_connections[i]->isQueueEmpty() == FALSE) {
				//DEBUG_LOG(("ConnectionManager::areAllQueuesEmpty() - m_connections[%d] is not empty", i));
				//m_connections[i]->debugPrintCommands();
				retval = FALSE;
			}
		}
	}

	return retval;
}

Bool ConnectionManager::canILeave() {
	return areAllQueuesEmpty();
}

/**
 * The local player is leaving. Tell the local player as well as the other players
 * to remove this player at the specified frame.
 */
void ConnectionManager::handleLocalPlayerLeaving(UnsignedInt frame) {
	NetPlayerLeaveCommandMsg *msg = newInstance(NetPlayerLeaveCommandMsg);

	msg->setLeavingPlayerID(m_localSlot);
	msg->setExecutionFrame(frame);
	if (DoesCommandRequireACommandID(msg->getNetCommandType())) {
		msg->setID(GenerateNextCommandID());
	}
	msg->setPlayerID(m_localSlot);

	DEBUG_LOG(("ConnectionManager::handleLocalPlayerLeaving - Local player leaving on frame %d", frame));
	DEBUG_ASSERTCRASH(m_packetRouterSlot >= 0, ("ConnectionManager::handleLocalPlayerLeaving, packet router is %d, illegal value.", m_packetRouterSlot));

	sendLocalCommand(msg);

	msg->detach();
}

/**
 * We just got a message that needs to be ack'd, so ack it!
 */
void ConnectionManager::ackCommand(NetCommandRef *ref, UnsignedInt localSlot) {
	NetCommandMsg *msg = ref->getCommand();
	NetCommandMsg *ackmsg;
	UnsignedShort commandID;
	UnsignedByte originalPlayerID;
	UnsignedByte sendRelay = 0;

	// Make send relay a bitmask for the connections that the relay will actually be sent to. This is
	// necessary to determine whether or not we have to wait to send a stage 2 ack.
	for (Int i = 0; i < MAX_SLOTS; ++i) {
		if (((m_connections[i] != nullptr) && (m_connections[i]->isQuitting() == FALSE))) {
			sendRelay = sendRelay | (1 << i);
		}
	}

#if defined(RTS_DEBUG)
	Bool doDebug = (msg->getNetCommandType() == NETCOMMANDTYPE_DISCONNECTFRAME) ? TRUE : FALSE;
#endif

	sendRelay = sendRelay & ref->getRelay();
	if (sendRelay == 0) {
		NetAckBothCommandMsg *bothmsg = newInstance(NetAckBothCommandMsg)(ref->getCommand());
		ackmsg = bothmsg;
		commandID = bothmsg->getCommandID();
		originalPlayerID = bothmsg->getOriginalPlayerID();
#if defined(RTS_DEBUG)
		if (doDebug) {
			DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("ConnectionManager::ackCommand - doing ack both for command %d from player %d", bothmsg->getCommandID(), bothmsg->getOriginalPlayerID()));
		}
#endif
	} else {
		NetAckStage1CommandMsg *stage1msg = newInstance(NetAckStage1CommandMsg)(ref->getCommand());
		ackmsg = stage1msg;
		commandID = stage1msg->getCommandID();
		originalPlayerID = stage1msg->getOriginalPlayerID();
#if defined(RTS_DEBUG)
		if (doDebug) {
			DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("ConnectionManager::ackCommand - doing ack stage 1 for command %d from player %d", stage1msg->getCommandID(), stage1msg->getOriginalPlayerID()));
		}
#endif
	}

	ackmsg->setPlayerID(localSlot); // Tell the player who this ack is coming from.

	if (CommandRequiresDirectSend(msg) && CommandRequiresAck(msg)) {
		// Send this ack directly back to the sending player, don't go through the packet router.
		if (msg->getPlayerID() < MAX_SLOTS) {
			if (m_connections[msg->getPlayerID()] != nullptr) {
				m_connections[msg->getPlayerID()]->sendNetCommandMsg(ackmsg, 1 << msg->getPlayerID());
			}
		}
	} else {
		// The local connection may be the packet router, in that case, the connection would be null.  So do something about it!
		if ((m_packetRouterSlot >= 0) && (m_packetRouterSlot < MAX_SLOTS)) {
			if (m_connections[m_packetRouterSlot] != nullptr) {
//				DEBUG_LOG(("ConnectionManager::ackCommand - acking command %d from player %d to packet router.", commandID, m_packetRouterSlot));
				m_connections[m_packetRouterSlot]->sendNetCommandMsg(ackmsg, 1 << m_packetRouterSlot);
			} else if (m_localSlot == m_packetRouterSlot) {
				// we are the packet router, send the ack to the player that sent the command.
				if (msg->getPlayerID() < MAX_SLOTS) {
					if (m_connections[msg->getPlayerID()] != nullptr) {
//						DEBUG_LOG(("ConnectionManager::ackCommand - acking command %d from player %d directly to player.", commandID, msg->getPlayerID()));
						m_connections[msg->getPlayerID()]->sendNetCommandMsg(ackmsg, 1 << msg->getPlayerID());
					} else {
	//					DEBUG_CRASH(("Connection to player is null"));
					}
				} else {
					DEBUG_CRASH(("Command sent by an invalid player ID."));
				}
			} else {
				DEBUG_CRASH(("Connection to packet router is null"));
			}
		} else {
			DEBUG_CRASH(("I don't know who the packet router is."));
		}
	}

	ackmsg->detach();
}

/**
 * This is where we relay a command from one client to others (including ourselves).
 * This should only be done by the current packet router.
 */
void ConnectionManager::sendRemoteCommand(NetCommandRef *msg) {
	UnsignedByte actualRelay = 0;
	if (msg->getCommand() == nullptr) {
		return;
	}

	DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("ConnectionManager::sendRemoteCommand - sending net command %d of type %s from player %d, relay is 0x%x",
		msg->getCommand()->getID(), GetNetCommandTypeAsString(msg->getCommand()->getNetCommandType()), msg->getCommand()->getPlayerID(), msg->getRelay()));

	const UnsignedByte relay = msg->getRelay();
	const UnsignedInt playerID = msg->getCommand()->getPlayerID();
	FrameDataManager *frameDataMgr = playerID < MAX_SLOTS ? m_frameData[playerID] : nullptr;
	if ((relay & (1 << m_localSlot)) && (frameDataMgr != nullptr)) {
		if (IsCommandSynchronized(msg->getCommand()->getNetCommandType())) {
			DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("ConnectionManager::sendRemoteCommand - adding net command of type %s to player %d for frame %d", GetNetCommandTypeAsString(msg->getCommand()->getNetCommandType()), msg->getCommand()->getPlayerID(), msg->getCommand()->getExecutionFrame()));
			frameDataMgr->addNetCommandMsg(msg->getCommand());
		}
	}

	for (Int i = 0; i < MAX_SLOTS; ++i) {
		if ((relay & (1 << i)) && ((m_connections[i] != nullptr) && (m_connections[i]->isQuitting() == FALSE))) {
			DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("ConnectionManager::sendRemoteCommand - relaying command %d to player %d", msg->getCommand()->getID(), i));
			m_connections[i]->sendNetCommandMsg(msg->getCommand(), 1 << i);
			actualRelay = actualRelay | (1 << i);
		}
	}

	if ((actualRelay != 0) && (CommandRequiresAck(msg->getCommand()) == TRUE)) {
		NetCommandRef *ref = m_relayedCommands->addMessage(msg->getCommand());
		if (ref != nullptr) {
			ref->setRelay(actualRelay);
			//DEBUG_LOG(("ConnectionManager::sendRemoteCommand - command %d added to relayed commands with relay %d", msg->getCommand()->getID(), ref->getRelay()));
		}
	}

	// Do some metrics to find the minimum packet arrival cushion.
	if (IsCommandSynchronized(msg->getCommand()->getNetCommandType())) {
//		DEBUG_LOG(("ConnectionManager::sendRemoteCommand - about to call allCommandsReady"));
		if (allCommandsReady(msg->getCommand()->getExecutionFrame(), TRUE)) {
			UnsignedInt cushion = msg->getCommand()->getExecutionFrame() - TheGameLogic->getFrame();
			if ((cushion < m_smallestPacketArrivalCushion) || (m_smallestPacketArrivalCushion == -1)) {
				m_smallestPacketArrivalCushion = cushion;
			}
			m_frameMetrics.addCushion(cushion);
//			DEBUG_LOG(("Adding %d to cushion for frame %d", cushion, msg->getCommand()->getExecutionFrame()));
		}
	}
}

/**
 * ConnectionManager::update
 * Update the connections. Tell them to do the receive and send.  Also relay
 * commands to their final destinations as necessary.
 */
void ConnectionManager::update(Bool isInGame) {
//
// 1. do this
// 2. do that
// 3. do something else
// 4. blow something up
// 5. bust some cap
//

	if ((m_localAddr == 0) || (m_localPort == 0)) {
		// we don't have a local address or port yet, this is bad.
		DEBUG_ASSERTCRASH((m_localAddr != 0) && (m_localPort != 0), ("ConnectionManager doesn't have a local address."));
		return;
	}

#if defined(_WIN64)
	if (m_networkSimulationPolicyResolved &&
		!isNetworkSimulationPolicyUsable())
	{
		revokeNetworkSimulationPolicy();
	}
	serviceNetworkHello();
#endif
	if (m_transport == nullptr)
		return;
	m_transport->doRecv();

	if (isInGame) {
		m_disconnectManager->update(this);
	}

	// take the packets from the transport, break them up into commands, and give them to the appropriate connections.
	doRelay();

	// send any necessary keep-alive packets.
	doKeepAlive();

	for (Int i = 0; i < MAX_SLOTS; ++i) {
		if (m_connections[i] != nullptr) {
			/*
			if (m_connections[i]->isQueueEmpty() == FALSE) {
//				DEBUG_LOG(("ConnectionManager::update - calling doSend on connection %d", i));
			}
			*/

			m_connections[i]->doSend();

			if (m_connections[i]->isQuitting() && m_connections[i]->isQueueEmpty())
			{
				DEBUG_LOG(("ConnectionManager::update - deleting connection for slot %d", i));
				deleteInstance(m_connections[i]);
				m_connections[i] = nullptr;
			}
		}

		if ((m_frameData[i] != nullptr) && (m_frameData[i]->getIsQuitting() == TRUE)) {
			if (m_frameData[i]->getQuitFrame() == TheGameLogic->getFrame()) {
				DEBUG_LOG(("ConnectionManager::update - deleting frame data for slot %d on quitting frame %d", i, m_frameData[i]->getQuitFrame()));
				deleteInstance(m_frameData[i]);
				m_frameData[i] = nullptr;
			}
		}
	}

	m_transport->doSend();
}

void ConnectionManager::updateRunAhead(Int oldRunAhead, Int frameRate, Bool didSelfSlug, Int nextExecutionFrame) {
	static time_t lasttimesent = 0;
	time_t curTime = timeGetTime();

	if ((lasttimesent == 0) || ((curTime - lasttimesent) > TheGlobalData->m_networkRunAheadMetricsTime)) {
		if (m_localSlot == m_packetRouterSlot) {
			// We are the packet router, time to compute a new run ahead for this game.
			m_latencyAverages[m_localSlot] = m_frameMetrics.getAverageLatency();

			// since we are now using the display frame rate rather than the logic frame rate to get our average FPS,
			// it doesn't make sense to send the desired logic frame rate if we "slugged" ourself.
//			if (didSelfSlug) {
//				m_fpsAverages[m_localSlot] = frameRate;
//			} else {
				m_fpsAverages[m_localSlot] = m_frameMetrics.getAverageFPS();
//			}
			if (didSelfSlug) {
				//DEBUG_LOG(("ConnectionManager::updateRunAhead - local player run ahead metrics, fps = %d, actual fps = %d, latency = %f, didSelfSlug = true", m_fpsAverages[m_localSlot], m_frameMetrics.getAverageFPS(), m_latencyAverages[m_localSlot]));
			} else {
				//DEBUG_LOG(("ConnectionManager::updateRunAhead - local player run ahead metrics, fps = %d, latency = %f, didSelfSlug = false", m_fpsAverages[m_localSlot], m_latencyAverages[m_localSlot]));
			}
			Int minFps;
			Int minFpsPlayer;
			getMinimumFps(minFps, minFpsPlayer);
			DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("ConnectionManager::updateRunAhead - max latency = %f, min fps = %d, min fps player = %d old FPS = %d", getMaximumLatency(), minFps, minFpsPlayer, frameRate));
			if ((minFps >= ((frameRate * 9) / 10)) && (minFps < frameRate)) {
				// if the minimum fps is within 10% of the desired framerate, then keep the current minimum fps.
				minFps = frameRate;
			}

			// TheSuperHackers @info this clamps the logic time scale fps in network games
			minFps = clamp<Int>(MIN_LOGIC_FRAMES, minFps, TheGlobalData->m_framesPerSecondLimit);
			DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("ConnectionManager::updateRunAhead - minFps after adjustment is %d", minFps));

			// TheSuperHackers @bugfix Mauller 21/08/2025 calculate the runahead so it always follows the latency
			// The runahead should always be rounded up to the next integer value to prevent variations in latency from causing stutter
			// The network slack pushes the runahead up to the next value when the latency is within the slack percentage of the current runahead
			const Real runAheadSlackScale = 1.0f + ( (Real)TheGlobalData->m_networkRunAheadSlack / 100.0f );
			Int newRunAhead = ceilf( getMaximumLatency() * runAheadSlackScale * (Real)minFps );

			// TheSuperHackers @info if the runahead goes below 3 logic frames it can start to introduce stutter
			// We also limit the upper range of the runahead to prevent it getting out of hand
			newRunAhead = clamp<Int>(MIN_RUNAHEAD, newRunAhead, MAX_FRAMES_AHEAD / 2);

			NetRunAheadCommandMsg *msg = newInstance(NetRunAheadCommandMsg);
			msg->setPlayerID(m_localSlot);
			if (DoesCommandRequireACommandID(msg->getNetCommandType())) {
				msg->setID(GenerateNextCommandID());
			}

			// needs to be set to the greater of getExecutionFrame and TheGameLogic->getFrame() + oldRunAhead
			// This prevents the case of...
			// run ahead starts at 30
			// run ahead changes to 10 at frame 31 (the command was created on frame 1)
			// run ahead changes to 10 at frame 56 (the command was created on frame 46)
			// notice that 56 is within the previous run ahead of 30 which has triggered
			// the frame command count being set for frames 1 through 60 since run ahead
			// didn't change for the first time till frame 31.  This creates an extra command
			// for frame 56 that isn't accounted for in the frame command count that is sent
			// out in the NetFrameCommandMsg.  sheesh.
			if (nextExecutionFrame > (TheGameLogic->getFrame() + oldRunAhead)) {
				msg->setExecutionFrame(nextExecutionFrame);
			} else {
				msg->setExecutionFrame(TheGameLogic->getFrame() + oldRunAhead);
			}

			msg->setRunAhead(newRunAhead);
			msg->setFrameRate(minFps);
			//DEBUG_LOG(("ConnectionManager::updateRunAhead - new run ahead = %d, new frame rate = %d, execution frame %d", newRunAhead, minFps, msg->getExecutionFrame()));
			sendLocalCommand(msg, 0xff ^ (1 << minFpsPlayer)); // Send the packet to everyone but the lowest FPS player.

			NetRunAheadCommandMsg *msg2 = newInstance(NetRunAheadCommandMsg);
			msg2->setPlayerID(m_localSlot);
			if (DoesCommandRequireACommandID(msg2->getNetCommandType())) {
				/*
				 * Ok there needs to be a big friggin comment about this change...
				 * What happens is that the two run ahead commands get sent to different players
				 * using different command ID's.  So player 1 has the run ahead command as command x
				 * and player 2 has the command as command x+1.  This is all good except when it comes
				 * to players being disconnected.  With the new disconnect scheme player 1 could potentially
				 * send his run ahead command to player 2 (or the other way around) to let player 2 catch
				 * up to him.  So if player 2 has his run ahead command as x+1 and now he gets player 1's
				 * command list with the run ahead command listed as command x, he won't see them as being
				 * the same command and will now think he has two different run ahead commands for that frame
				 * and thus his command list will have an extra command and he will never be able to recover.
				 * So to fix this we have to use the same command ID for both run ahead commands.  That way
				 * when the commands are copied places for the disconnect screen they will be seen as the
				 * same command, and all will be good.
				 */
//				msg2->setID(GenerateNextCommandID());
				msg2->setID(msg->getID());
			}
			if (nextExecutionFrame > (TheGameLogic->getFrame() + oldRunAhead)) {
				msg2->setExecutionFrame(nextExecutionFrame);
			} else {
				msg2->setExecutionFrame(TheGameLogic->getFrame() + oldRunAhead);
			}

			// Let the player with the slowest FPS run a little faster than the other computers...
			// just in case they are able to.  Then we might be able to run the game faster which would be good.
			Int newMinFps = (minFps * 11) / 10;
			if (newMinFps == minFps) {
				newMinFps = minFps + 1;
			}
			if (newMinFps > 30) {
				newMinFps = 30; // Cap FPS to 30.
			}
			msg2->setRunAhead(newRunAhead);
			msg2->setFrameRate(newMinFps);

			sendLocalCommand(msg2, 1 << minFpsPlayer);

			msg->detach();
			msg2->detach();
		} else if (IsValidPacketRouterSlot(m_packetRouterSlot) &&
			m_connections[m_packetRouterSlot] != nullptr) {
			// We are not the packet router, send our metrics info to the packet router.
			NetRunAheadMetricsCommandMsg *msg = newInstance(NetRunAheadMetricsCommandMsg);
			msg->setPlayerID(m_localSlot);
			if (DoesCommandRequireACommandID(msg->getNetCommandType())) {
				msg->setID(GenerateNextCommandID());
			}
			msg->setAverageLatency(m_frameMetrics.getAverageLatency());

			// see above for explanation.
//			if (didSelfSlug) {
//				msg->setAverageFps(frameRate);
//			} else {
				msg->setAverageFps(m_frameMetrics.getAverageFPS());
//			}
			if (didSelfSlug) {
				//DEBUG_LOG(("ConnectionManager::updateRunAhead - average latency = %f, average fps = %d, actual fps = %d, didSelfSlug = true", m_frameMetrics.getAverageLatency(), m_frameMetrics.getAverageFPS(), m_frameMetrics.getAverageFPS()));
			} else {
				//DEBUG_LOG(("ConnectionManager::updateRunAhead - average latency = %f, average fps = %d, didSelfSlug = false", m_frameMetrics.getAverageLatency(), m_frameMetrics.getAverageFPS()));
			}
			m_connections[m_packetRouterSlot]->sendNetCommandMsg(msg, 1 << m_packetRouterSlot);
			msg->detach();
		} else {
			DEBUG_LOG(("ConnectionManager::updateRunAhead - no valid packet router connection"));
		}
		lasttimesent = curTime;
	}
}

Real ConnectionManager::getMaximumLatency() {

	Real lat1 = 0.0f;
	Real lat2 = 0.0f;

	for (Int i = 0; i < MAX_SLOTS; ++i) {
		if (isPlayerConnected(i)) {
			if (m_latencyAverages[i] != 0.0f) {
				if (m_latencyAverages[i] > lat1) {
					lat2 = lat1;
					lat1 = m_latencyAverages[i];
				}
				else if (m_latencyAverages[i] > lat2) {
					lat2 = m_latencyAverages[i];
				}
			}
		}
	}

	return (lat1 + lat2) / 2.0f;
}

void ConnectionManager::getMinimumFps(Int &minFps, Int &minFpsPlayer) {
	minFps = -1;
	minFpsPlayer = -1;
//	DEBUG_LOG_RAW(("ConnectionManager::getMinimumFps -"));
	for (Int i = 0; i < MAX_SLOTS; ++i) {
		if ((m_connections[i] != nullptr) || (i == m_localSlot)) {
//			DEBUG_LOG_RAW((" %d: %d,", i, m_fpsAverages[i]));
			if (m_fpsAverages[i] != -1) {
				if ((minFps == -1) || (m_fpsAverages[i] < minFps)) {
					minFps = m_fpsAverages[i];
					minFpsPlayer = i;
				}
			}
		}
	}
//	DEBUG_LOG_RAW(("\n"));
}

UnsignedInt ConnectionManager::getMinimumCushion() {
	return m_frameMetrics.getMinimumCushion();
}

/**
 * The commands for the given frame are all ready, time to send out our command count for that frame.
 */
void ConnectionManager::processFrameTick(UnsignedInt frame) {
	if ((m_frameData[m_localSlot] == nullptr) || (m_frameData[m_localSlot]->getIsQuitting() == TRUE)) {
		// if the local frame data stuff is null, we must be leaving the game.
		return;
	}
	UnsignedShort commandCount = m_frameData[m_localSlot]->getCommandCount(frame);
	NetFrameCommandMsg *msg = newInstance(NetFrameCommandMsg);
	msg->setExecutionFrame(frame);
	msg->setCommandCount(commandCount);
	if (DoesCommandRequireACommandID(msg->getNetCommandType())) {
		msg->setID(GenerateNextCommandID());
	}
	msg->setPlayerID(m_localSlot);

	m_frameMetrics.doPerFrameMetrics(frame);

	DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("ConnectionManager::processFrameTick - sending frame info for frame %d, ID %d, command count %d", frame, msg->getID(), commandCount));

	sendLocalCommand(msg, 0xff & ~(1 << m_localSlot));

	msg->detach();
}

/**
 * Set the local address.
 */
void ConnectionManager::setLocalAddress(UnsignedInt ip, UnsignedInt port) {
	DEBUG_LOG(("ConnectionManager::setLocalAddress() - local address is %X:%d", ip, port));
	m_localAddr = ip;
	m_localPort = port;
}

/**
 * Initialize the transport object
 */
void ConnectionManager::initTransport() {
	DEBUG_ASSERTCRASH((m_transport == nullptr), ("m_transport already exists when trying to init it."));
	DEBUG_LOG(("ConnectionManager::initTransport - Initializing Transport"));

	delete m_transport;
	m_transport = new Transport;
	m_transport->reset();
#if defined(_WIN64)
	const Bool transportInitialized = m_transport->init(m_localAddr, m_localPort);
	m_lockstepV2TransportInitialized = transportInitialized;
#else
	m_transport->init(m_localAddr, m_localPort);
#endif
}

/**
 * This is where the commands from the local client are sent to the other clients in
 * the game.  This is also where the local commands are put into the frame data for
 * future execution.
 */
void ConnectionManager::sendLocalGameMessage(GameMessage *msg, UnsignedInt frame) {
	UnsignedShort currentID = 0;
	if (DoesCommandRequireACommandID(NETCOMMANDTYPE_GAMECOMMAND)) {
		currentID = GenerateNextCommandID();
	}

	NetCommandMsg *netmsg = newInstance(NetGameCommandMsg)(msg);
	netmsg->setExecutionFrame(frame);
	netmsg->setPlayerID(m_localSlot);
	netmsg->setID(currentID);

	sendLocalCommand(netmsg);

#if defined(_WIN64)
	// Record the canonical bytes at the owner intake boundary.  The same
	// command may be retransmitted; ReceiptRecorder deduplicates an identical
	// command ID/digest and rejects altered bytes instead of counting either as
	// a second contribution.
	if (m_lockstepV2ReceiptRecorder.isActive())
	{
		NetCommandRef *receiptRef = NEW_NETCOMMANDREF(netmsg);
		receiptRef->setRelay(0xff);
		std::uint64_t commandDigest = 0U;
		if (!getLockstepV2CommandDigest(receiptRef, &commandDigest) ||
			!recordLockstepV2Command(frame, m_localSlot, currentID, commandDigest))
		{
			m_lockstepV2ReceiptRecorder.reset();
		}
		deleteInstance(receiptRef);
	}
#endif

	netmsg->detach();
}

/**
 * This is a NetCommandMsg that originated on the local computer. Send this to everyone specified
 * in the relay field.  Commands sent in this way go through the packet router.
 */
void ConnectionManager::sendLocalCommand(NetCommandMsg *msg, UnsignedByte relay /* = 0xff by default*/) {
	#if defined(_WIN64)
	if (msg != nullptr && !isNetworkHelloReady())
	{
		const NetCommandType commandType = msg->getNetCommandType();
		const Bool isFileTraffic =
			commandType == NETCOMMANDTYPE_FILE ||
			commandType == NETCOMMANDTYPE_FILEANNOUNCE ||
			commandType == NETCOMMANDTYPE_FILEPROGRESS;
		if (IsCommandSynchronized(commandType) || isFileTraffic)
		{
			if (queueNetworkHelloCommand(msg, relay))
			{
				DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("ConnectionManager::sendLocalCommand - queued command type %d until NET3 is ready", commandType));
			}
			return;
		}
	}
	#endif

	sendLocalCommandImmediate(msg, relay);
}

void ConnectionManager::sendLocalCommandImmediate(NetCommandMsg *msg, UnsignedByte relay) {
	#if defined(_WIN64)
	// DirectAuthenticated is the v2 default and deliberately bypasses the
	// legacy packet-router relay.  A trusted-router session must be explicit in
	// its receipt contract and retains the legacy routing behavior below.
	if (m_lockstepV2ReceiptRecorder.isActive() &&
		m_lockstepV2Session.originMode ==
			rts::lockstep_v2::CommandOriginMode::DirectAuthenticated)
	{
		sendLocalCommandDirect(msg, relay);
		return;
	}
	#endif

	const Bool packetRouterHasConnection = m_packetRouterSlot < MAX_SLOTS &&
		m_connections[m_packetRouterSlot] != nullptr;
	const Bool packetRouterIsQuitting = packetRouterHasConnection &&
		m_connections[m_packetRouterSlot]->isQuitting();
	Bool packetRouterEligible = FALSE;
#if defined(_WIN64)
	packetRouterEligible = rts::network_epoch::IsNetworkPacketRouterEligible(
		m_packetRouterSlot, m_localSlot, MAX_SLOTS, packetRouterHasConnection, packetRouterIsQuitting);
#else
	// Keep the legacy Win32/VC6 router decision local because the NET3 helper
	// is only available to the native x64 handshake lane.
	packetRouterEligible = m_packetRouterSlot < MAX_SLOTS &&
		(m_packetRouterSlot == m_localSlot ||
		(packetRouterHasConnection && !packetRouterIsQuitting));
#endif
	if (CommandRequiresDirectSend(msg) || !packetRouterEligible) {
		sendLocalCommandDirect(msg, relay);
		return;
	}
	msg->attach();

	DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("ConnectionManager::sendLocalCommand - sending net command %d of type %s", msg->getID(),
		GetNetCommandTypeAsString(msg->getNetCommandType())));

	if (relay & (1 << m_localSlot)) {
		DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("ConnectionManager::sendLocalCommand - adding net command of type %s to player %d for frame %d", GetNetCommandTypeAsString(msg->getNetCommandType()), msg->getPlayerID(), msg->getExecutionFrame()));
		m_frameData[m_localSlot]->addNetCommandMsg(msg);
	}

	// Send the packet to everyone else
	if (m_localSlot == m_packetRouterSlot) {
		// I am the packet router, I need to send this packet to everyone individually.
		for (Int i = 0; i < MAX_SLOTS; ++i) {
			// Send it to all open connections.
			if (((m_connections[i] != nullptr) && (m_connections[i]->isQuitting() == FALSE)) && (relay & (1 << i))) {
				// Set the relay mask to only go to this player so he knows not to relay it to anyone else.
				UnsignedByte temprelay = 1 << i;
				m_connections[i]->sendNetCommandMsg(msg, temprelay); // This will create a new copy of netmsg for this connection.
			}
		}
	} else {
		// Send the command to everyone else via the packet router.
		UnsignedByte temprelay = relay & ~(1 << m_localSlot);	// Tell the packet router to relay the message to everyone but myself.
														// Hopefully the packet router is smart enough to not send it
														// to slots that are not in the game.

		m_connections[m_packetRouterSlot]->sendNetCommandMsg(msg, temprelay); // This will create a new copy of netmsg for this connection.

		if (CommandRequiresAck(msg)) {
			NetCommandRef *ref = m_pendingCommands->addMessage(msg);
			//DEBUG_LOG(("ConnectionManager::sendLocalCommand - added command %d to pending commands list.", msg->getID()));
			if (ref != nullptr) {
				ref->setRelay(temprelay);
			}
		}
	}

	msg->detach(); // detach from the command msg.
}

/**
 * This is a NetCommandMsg that originated on the local computer.  Send this to everyone specified
 * in the relay field.  Commands sent in this way do not go through the packet router.
 */
void ConnectionManager::sendLocalCommandDirect(NetCommandMsg *msg, UnsignedByte relay) {
	msg->attach();

	if (((relay & (1 << m_localSlot)) != 0) && (m_frameData[m_localSlot] != nullptr)) {
		if (IsCommandSynchronized(msg->getNetCommandType()) == TRUE) {
			DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("ConnectionManager::sendLocalCommandDirect - adding net command of type %s to player %d for frame %d", GetNetCommandTypeAsString(msg->getNetCommandType()), msg->getPlayerID(), msg->getExecutionFrame()));
			m_frameData[m_localSlot]->addNetCommandMsg(msg);
		}
	}

	for (Int i = 0; i < MAX_SLOTS; ++i) {
		if ((relay & (1 << i)) != 0) {
			if ((m_connections[i] != nullptr) && (m_connections[i]->isQuitting() == FALSE)) {
				UnsignedByte temprelay = 1 << i;
				m_connections[i]->sendNetCommandMsg(msg, temprelay);
				DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("ConnectionManager::sendLocalCommandDirect - Sending direct command %d of type %s to player %d", msg->getID(), GetNetCommandTypeAsString(msg->getNetCommandType()), i));
			}
		}
	}

	msg->detach();
}

Int commandsReadyDebugSpewage = 0;

/**
 * Returns true if all the commands for the given frame are ready to be executed.
 */
Bool ConnectionManager::allCommandsReady(UnsignedInt frame, Bool justTesting /* = FALSE */) {
	Bool retval = TRUE;
	FrameDataReturnType frameRetVal = FRAMEDATA_NOTREADY;
//	retval = FALSE;  // ****for testing purposes only!!!!!!****
	Int i = 0;
	for (; (i < MAX_SLOTS) && retval; ++i) {
		if ((m_frameData[i] != nullptr) && (m_frameData[i]->getIsQuitting() == FALSE)) {
/*
			if (!(m_frameData[i]->allCommandsReady(frame, (frame != commandsReadyDebugSpewage) && (justTesting == FALSE)))) {
				if ((frame != commandsReadyDebugSpewage) && (justTesting == FALSE)) {
					DEBUG_LOG(("ConnectionManager::allCommandsReady, frame %d player %d not ready.", frame, i));
					commandsReadyDebugSpewage = frame;
				}
				retval = FALSE;
			} else {
//				DEBUG_LOG(("ConnectionManager::allCommandsReady, frame %d player %d is ready.", frame, i));
			}
*/

			frameRetVal = m_frameData[i]->allCommandsReady(frame, (frame != commandsReadyDebugSpewage) && (justTesting == FALSE));
			if (frameRetVal == FRAMEDATA_NOTREADY) {
				retval = FALSE;
			} else if (frameRetVal == FRAMEDATA_RESEND) {
				requestFrameDataResend(i, frame);
				retval = FALSE;
			}
		}
	}

	if (frameRetVal == FRAMEDATA_RESEND) {
		// this frame's data is really screwed up, we need to clean it out so it can be resent to us.
		for (i = 0; i < MAX_SLOTS; ++i) {
			if ((m_frameData[i] != nullptr) && (i != m_localSlot)) {
				m_frameData[i]->resetFrame(frame, FALSE);
			}
		}
	}

	if ((retval == TRUE) && (justTesting == FALSE)) {
		m_disconnectManager->allCommandsReady(TheGameLogic->getFrame(), this);
		retval = m_disconnectManager->allowedToContinue(); // allow the disconnect manager to keep us on this frame
																											// in case we are waiting for a new packet router or something.
	}

	return retval;
}

void ConnectionManager::handleAllCommandsReady()
{
	m_disconnectManager->allCommandsReady(TheGameLogic->getFrame(), this, FALSE);
}


/**
 * Only call this after making sure that all the commands are there for this frame.
 * After calling this the commands for this frame will be removed from the connection.
 *
 * BGC - To account for the case where the host disconnects without sending the
 *       same commands to all players, we now have to keep around the last 'run ahead'
 *       frames so we can potentially send those commands to the other players in the
 *       game so they can catch up.
 */
NetCommandList *ConnectionManager::getFrameCommandList(UnsignedInt frame)
{
	NetCommandList *retlist = newInstance(NetCommandList);
	retlist->init();

	for (Int i = 0; i < MAX_SLOTS; ++i) {
		if (m_frameData[i] != nullptr) {
			retlist->appendList(m_frameData[i]->getFrameCommandList(frame));
			if (frame > FRAMES_TO_KEEP) {
				m_frameData[i]->resetFrame(frame - FRAMES_TO_KEEP);	// After getting the commands for that frame from this
													// FrameDataManager object, we need to tell it that we're
													// done with the messages for that frame.
				DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("getFrameCommandList - called reset frame on player %d for frame %d", i, frame - FRAMES_TO_KEEP));
			}
		}
	}

	return retlist; // retlist deallocated by calling function.
}

void ConnectionManager::setFrameGrouping(time_t frameGrouping) {
	// Since we are the packet router, we should send more packets per second since we
	// may become the latency bottleneck for sending packets from one player to the next.
	// This is probably ok since the packet router should have the fastest connection of all
	// the players in the game.
	if (m_localSlot == m_packetRouterSlot) {
		frameGrouping = frameGrouping / 2;
	}
	for (Int i = 0; i < MAX_SLOTS; ++i) {
		if (m_connections[i] != nullptr) {
			m_connections[i]->setFrameGrouping(frameGrouping);
		}
	}
}

/*
void ConnectionManager::determineRouterFallbackPlan() {
	memset(m_packetRouterFallback, 0, sizeof(m_packetRouterFallback));
	Int curnum = 1;
	for (Int i = 0; i < MAX_SLOTS; ++i) {
		if (m_connections[i] != nullptr) {
			m_packetRouterFallback[i] = curnum;
			if (curnum == 1) {
				m_packetRouterSlot = i;
			}
			++curnum;
		}
	}
}
*/

void ConnectionManager::doKeepAlive() {
	static Int nextIndex = 0;
	static time_t startTime = 0;

	time_t curTime = timeGetTime();

	if (startTime == 0) {
		startTime = curTime;
		return;
	}

	time_t numSeconds = (curTime - startTime) / 1000;

	while ((nextIndex <= numSeconds) && (nextIndex < MAX_SLOTS)) {
//		DEBUG_LOG(("ConnectionManager::doKeepAlive - trying to send keep alive message to player %d", nextIndex));
		if (m_connections[nextIndex] != nullptr) {
			NetKeepAliveCommandMsg *msg = newInstance(NetKeepAliveCommandMsg);
			msg->setPlayerID(m_localSlot);
			if (DoesCommandRequireACommandID(msg->getNetCommandType()) == TRUE) {
				msg->setID(GenerateNextCommandID());
			}
//			DEBUG_LOG(("ConnectionManager::doKeepAlive - sending keep alive message to player %d", nextIndex));
			sendLocalCommandDirect(msg, 1 << nextIndex);
			msg->detach();
		}
		++nextIndex;
	}
	if (nextIndex == MAX_SLOTS) {
		nextIndex = 0;
		startTime = curTime;
	}
}

PlayerLeaveCode ConnectionManager::disconnectPlayer(Int slot) {
	// Need to do the deletion of the slot's connection and frame data here.
	PlayerLeaveCode retval = PLAYERLEAVECODE_CLIENT;
	DEBUG_LOG(("ConnectionManager::disconnectPlayer - disconnecting slot %d on frame %d", slot, TheGameLogic->getFrame()));

	if ((slot < 0) || (slot >= MAX_SLOTS)) {
		return PLAYERLEAVECODE_UNKNOWN;
	}

#if defined(_WIN64)
	if (static_cast<UnsignedInt>(slot) == m_localSlot ||
		(m_networkHelloExpectedSlots & (1U << slot)) != 0U)
	{
		revokeNetworkSimulationPolicy();
	}
#endif

	if (m_netCommandWrapperList != nullptr)
		m_netCommandWrapperList->removeForPlayer(static_cast<UnsignedByte>(slot));
#if defined(_WIN64)
	m_disconnectFrameRecovery[slot] = {};
	m_networkWrapperAckHistory.removePeer(static_cast<UnsignedInt>(slot));
	deleteInstance(m_networkRecoveryWrappers[slot]);
	m_networkRecoveryWrappers[slot] = nullptr;
	for (Int responder = 0; responder < MAX_SLOTS; ++responder)
	{
		m_disconnectFrameRecovery[responder].originMask &= ~(1U << slot);
		if (m_networkRecoveryWrappers[responder] != nullptr)
			m_networkRecoveryWrappers[responder]->removeForPlayer(static_cast<UnsignedByte>(slot));
	}
	if (m_frameResendRequestResponder == static_cast<UnsignedInt>(slot))
		clearNetworkFrameResendRequest();
	else
		m_frameResendRequestExpectedInfoMask &= ~(1U << slot);
#endif

	if (TheGameInfo)
	{
		GameSlot *gSlot = TheGameInfo->getSlot( slot );
		if (gSlot && !gSlot->lastFrameInGame())
		{
			DEBUG_LOG(("ConnectionManager::disconnectPlayer(%d) - slot is last in the game on frame %d",
				slot, TheGameLogic->getFrame()));
			gSlot->setLastFrameInGame(TheGameLogic->getFrame());
		}
	}

	UnicodeString unicodeName;
	unicodeName = getPlayerName(slot);
	if (!unicodeName.isEmpty() && m_connections[slot]) {
		TheInGameUI->message("Network:PlayerLeftGame", unicodeName.str());

		// People are boneheads. Also play a sound
		static AudioEventRTS leftGameSound("GUIMessageReceived");
		TheAudio->addAudioEvent(&leftGameSound);
	}

	if ((m_frameData[slot] != nullptr) && (m_frameData[slot]->getIsQuitting() == FALSE)) {
		DEBUG_LOG(("ConnectionManager::disconnectPlayer - deleting player %d frame data", slot));
		deleteInstance(m_frameData[slot]);
		m_frameData[slot] = nullptr;
	}

	if (m_connections[slot] != nullptr && !m_connections[slot]->isQuitting()) {
		DEBUG_LOG(("ConnectionManager::disconnectPlayer - deleting player %d connection", slot));
		deleteInstance(m_connections[slot]);
		m_connections[slot] = nullptr;
	}

//	if (playerID == m_localSlot) {
//		TheMessageStream->appendMessage(GameMessage::MSG_CLEAR_GAME_DATA);
//	}

	if (slot == m_packetRouterSlot) {
		m_packetRouterSlot = getNextPacketRouterSlot(m_packetRouterSlot);
		DEBUG_LOG(("Packet router left.  New packet router is slot %d", m_packetRouterSlot));
		retval = PLAYERLEAVECODE_PACKETROUTER;
	}
	if (m_localSlot == slot) {
		DEBUG_LOG(("Disconnecting self"));
		retval = PLAYERLEAVECODE_LOCAL;
	}

	// Take the player out of the fallback plan
	Int fallbackindex = 0;
	while ((fallbackindex < MAX_SLOTS) && (m_packetRouterFallback[fallbackindex] != slot)) {
		++fallbackindex;
	}

	for (Int i = fallbackindex; i < MAX_SLOTS-1; ++i) {
		m_packetRouterFallback[i] = m_packetRouterFallback[i+1];
	}
	m_packetRouterFallback[MAX_SLOTS-1] = -1;

	return retval;
}

void ConnectionManager::quitGame() {
	// Need to do the NetDisconnectPlayerCommandMsg creation and sending here.
	NetDisconnectPlayerCommandMsg *disconnectMsg = newInstance(NetDisconnectPlayerCommandMsg);
	disconnectMsg->setDisconnectSlot(m_localSlot);
	disconnectMsg->setDisconnectFrame(TheGameLogic->getFrame());
	disconnectMsg->setPlayerID(m_localSlot);
	if (DoesCommandRequireACommandID(disconnectMsg->getNetCommandType())) {
		disconnectMsg->setID(GenerateNextCommandID());
	}
	//DEBUG_LOG(("ConnectionManager::disconnectLocalPlayer - about to send disconnect command"));
	sendLocalCommandDirect(disconnectMsg, 0xff ^ (1 << m_localSlot));

	//DEBUG_LOG(("ConnectionManager::disconnectLocalPlayer - about to flush connections"));
	flushConnections(); // need to do this so our packet actually gets sent before the connections are deleted.
	//DEBUG_LOG(("ConnectionManager::disconnectLocalPlayer - done flushing connections"));

	disconnectMsg->detach();

#if RTS_GENERALS
	// if we get here, we hit Quit on the disconnect screen.  Mark everyone as having disconnected from us
	// so the online stats can give us appropriate feedback.
	if (TheGameInfo)
	{
		for (Int i = 0; i < MAX_SLOTS; ++i)
		{
			GameSlot *gSlot = TheGameInfo->getSlot( i );
			if (gSlot && !gSlot->lastFrameInGame())
			{
				gSlot->markAsDisconnected();
			}
		}
	}
#endif

	disconnectLocalPlayer();
}

void ConnectionManager::disconnectLocalPlayer() {
	// kill the frame data and the connections for all the other players.
	DEBUG_LOG(("ConnectionManager::disconnectLocalPlayer()"));
	for (Int i = 0; i < MAX_SLOTS; ++i) {
		if (i != m_localSlot) {
			disconnectPlayer(i);
		}
	}
}

/**
 * Takes all the commands that are ready to send and sends them right now.
 */
void ConnectionManager::flushConnections() {
	for (Int i = 0; i < MAX_SLOTS; ++i) {
		if (m_connections[i] != nullptr) {
//			DEBUG_LOG(("ConnectionManager::flushConnections - flushing connection to player %d", i));
			/*
			if (m_connections[i]->isQueueEmpty()) {
//				DEBUG_LOG(("ConnectionManager::flushConnections - connection queue empty"));
			}
			*/
			m_connections[i]->doSend();
		}
	}

	if (m_transport != nullptr) {
		m_transport->doSend();
	}
}

void ConnectionManager::resendPendingCommands() {
	//DEBUG_LOG(("ConnectionManager::resendPendingCommands()"));
	if (m_pendingCommands == nullptr) {
		return;
	}

	NetCommandRef *ref = m_pendingCommands->getFirstMessage();
	while (ref != nullptr) {
		//DEBUG_LOG(("ConnectionManager::resendPendingCommands - resending command %d", ref->getCommand()->getID()));
		sendLocalCommand(ref->getCommand(), ref->getRelay());
		ref = ref->getNext();
	}
}

UnsignedInt ConnectionManager::getLocalPlayerID() {
	return m_localSlot;
}

UnicodeString ConnectionManager::getPlayerName(Int playerNum) {
	UnicodeString retval;
	if( playerNum == m_localSlot ) {
		retval = m_localUser->GetName();
	}	else if (((m_connections[playerNum] != nullptr) && (m_connections[playerNum]->isQuitting() == FALSE))) {
		retval = m_connections[playerNum]->getUser()->GetName();
	}
	return retval;
}

/**
 * Take a user list and make connections and frame data manager objects for each of the players.
 * For now, this is also how we'll determine the packet router fallback plan.
 */
void ConnectionManager::parseUserList(const GameInfo *game)
{
	if (!game)
		return;

	Int i;
	Int numUsers = 0;
	m_localSlot = -1;
	DEBUG_LOG(("Local slot is %d", game->getLocalSlotNum()));
	for (i=0; i<MAX_SLOTS; ++i)
	{
		const GameSlot *slot = game->getConstSlot(i);	// badness, but since we cast right back to const, we should be ok
		if (slot->isHuman())
		{
			if (game->getLocalSlotNum() == i)
			{
				m_localSlot = i;
   			m_localUser->setName(slot->getName());
			}

			if (m_localSlot != i)
			{
				m_connections[i] = newInstance(Connection)();
				m_connections[i]->init();
				m_connections[i]->attachTransport(m_transport);
//				UnsignedShort port = (TheNAT)?TheNAT->getSlotPort(i):8088;
				UnsignedShort port = slot->getPort();
				m_connections[i]->setUser(newInstance(User)(slot->getName(), slot->getIP(), port));
				m_frameData[i] = newInstance(FrameDataManager)(FALSE);
				DEBUG_LOG(("Remote user is at %X:%d", slot->getIP(), slot->getPort()));
			}
			else
			{
				DEBUG_LOG(("Local user is %d (%X:%d)", m_localSlot, slot->getIP(), slot->getPort()));
				m_frameData[i] = newInstance(FrameDataManager)(TRUE);
			}
			m_frameData[i]->init();
			m_frameData[i]->reset();

			m_packetRouterFallback[numUsers] = i;

			++numUsers;

		}
	}

#if defined(_WIN64)
	clearNetworkSimulationPolicy();
	m_networkSimulationMapCrc = game->getMapCRC();
	beginNetworkHello();
#endif
#ifdef MEMORYPOOL_DEBUG
	TheMemoryPoolFactory->debugSetInitFillerIndex(m_localSlot);
#endif

	/*
	if ( numUsers < 2 || m_localSlot == -1 )
	{
		DEBUG_CRASH(("FAILED parseUserList - network game won't work as expected"));
		return;
	}

	char * list = strdup(buf);
	char *listPtr = list;
	if (!list)
		return;

	User users[MAX_SLOTS];
	int localUser = -1;
	int i;

	for (i=0; i<MAX_SLOTS; i++)
	{
		users[i].setName("");
	}

	char *userStr, *nameStr, *addrStr, *portStr;
	AsciiString addrAsciiStr;

	char *listPos;

	DEBUG_LOG(("ConnectionManager::parseUserList - looking for local user at %d.%d.%d.%d:%d",
		PRINTF_IP_AS_4_INTS(m_localAddr),
		m_localPort));

	int numUsers = 0;
	while ( (userStr=strtok_r(listPtr, ",", &listPos)) != nullptr )
	{
		listPtr = nullptr;
		char *pos = nullptr;

		nameStr = strtok_r(userStr, "@", &pos);
		addrStr = strtok_r(nullptr, "@:", &pos);
		portStr = strtok_r(nullptr, ": ", &pos);

		if (!portStr || numUsers >= MAX_SLOTS)
		{
			DEBUG_LOG(("ConnectionManager::parseUserList - (numUsers = %d) FAILED parseUserList with list [%s]", numUsers, buf));
			return;
		}

		addrAsciiStr = addrStr;
		UnsignedInt addr = ResolveIP(addrAsciiStr);
		UnsignedInt port = atoi(portStr);

//		if ((m_localAddr != addr) || (m_localPort != port)) {
		if (loginName.compare(nameStr) != 0) {
			m_connections[numUsers] = newInstance(Connection)();
			m_connections[numUsers]->init();
			m_connections[numUsers]->attachTransport(m_transport);
			m_connections[numUsers]->setUser(newInstance(User)(nameStr, addr, port));

			m_frameData[numUsers] = newInstance(FrameDataManager)(FALSE);

			DEBUG_LOG(("ConnectionManager::parseUserList - User %d is %s", numUsers, nameStr));
		} else {
			m_localSlot = numUsers;
			m_localUser.setName(nameStr);

			DEBUG_LOG(("ConnectionManager::parseUserList - User %d is %s", numUsers, nameStr));
			DEBUG_LOG(("Local user is %d", m_localSlot));

			m_frameData[numUsers] = newInstance(FrameDataManager)(TRUE);
		}
		m_frameData[numUsers]->init();
		m_frameData[numUsers]->reset();

		m_packetRouterFallback[numUsers] = numUsers;

		numUsers++;
	}

	if (numUsers < 2 || m_localSlot == -1)
	{
		DEBUG_LOG(("ConnectionManager::parseUserList - FAILED (local user = %d, num players = %d) with list [%s]", m_localSlot, numUsers, buf));
		return;
	}

	free(list); // from the strdup above.
	list = nullptr;
	*/
}

/**
 * Return the number of incoming bytes per second averaged over 30 sec.
 */
Real ConnectionManager::getIncomingBytesPerSecond()
{
	if (m_transport)
		return m_transport->getIncomingBytesPerSecond();
	else
	  return 0.0;
}

/**
 * Return the number of incoming packets per second averaged over the last 30 sec.
 */
Real ConnectionManager::getIncomingPacketsPerSecond()
{
	if (m_transport)
		return m_transport->getIncomingPacketsPerSecond();
	else
	  return 0.0;
}

/**
 * Return the number of outgoing bytes per second averaged over the last 30 sec.
 */
Real ConnectionManager::getOutgoingBytesPerSecond()
{
	if (m_transport)
		return m_transport->getOutgoingBytesPerSecond();
	else
	  return 0.0;
}

/**
 * Return the number of outgoing packets per second averaged over the last 30 sec.
 */
Real ConnectionManager::getOutgoingPacketsPerSecond()
{
	if (m_transport) {
		return m_transport->getOutgoingPacketsPerSecond();
	} else {
	  return 0.0;
	}
}

/**
 * Return the number of bytes not from generals clients received per second averaged over the last 30 sec.
 */
Real ConnectionManager::getUnknownBytesPerSecond()
{
	if (m_transport)
		return m_transport->getUnknownBytesPerSecond();
	else
	  return 0.0;
}

/**
 * Return the number ov packets not from generals clients received per second averaged over the last 30 sec.
 */
Real ConnectionManager::getUnknownPacketsPerSecond()
{
	if (m_transport)
		return m_transport->getUnknownPacketsPerSecond();
	else
	  return 0.0;
}

/**
 * Return the smallest packet arrival cushion since this was last called.
 */
UnsignedInt ConnectionManager::getPacketArrivalCushion() {
	UnsignedInt retval = m_smallestPacketArrivalCushion;
	m_smallestPacketArrivalCushion = -1;
	return retval;
}

void ConnectionManager::sendChat(UnicodeString text, Int playerMask, UnsignedInt executionFrame)
{
	NetChatCommandMsg *msg = newInstance(NetChatCommandMsg);
	msg->setText(text);
	msg->setPlayerMask(playerMask);
	msg->setPlayerID(m_localSlot);
	msg->setID(0);
	msg->setExecutionFrame(executionFrame);
	if (DoesCommandRequireACommandID(msg->getNetCommandType()))
	{
		msg->setID(GenerateNextCommandID());
	}
	DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("Chat message has ID of %d, mask of %8.8X, text of %ls", msg->getID(), msg->getPlayerMask(), msg->getText().str()));

	sendLocalCommand(msg, 0xff ^ (1 << m_localSlot));
	processChat(msg);

	msg->detach();
}

void ConnectionManager::sendDisconnectChat(UnicodeString text) {
	NetDisconnectChatCommandMsg *msg = newInstance(NetDisconnectChatCommandMsg);
	msg->setPlayerID(m_localSlot);
	if (DoesCommandRequireACommandID(msg->getNetCommandType())) {
		msg->setID(GenerateNextCommandID());
	}
	msg->setText(text);

	sendLocalCommandDirect(msg, 0xff ^ (1 << m_localSlot));
	processDisconnectChat(msg);
}

UnsignedShort ConnectionManager::sendFileAnnounce(AsciiString path, UnsignedByte playerMask)
{
	#if defined(_WIN64)
	if (!isNetworkHelloReady())
	{
		DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("ConnectionManager::sendFileAnnounce - NET3 is not ready"));
		return 0;
	}
	#endif

	File *theFile = TheLocalFileSystem->openFile(path.str());
	if (!theFile || !theFile->size())
	{
		UnicodeString log;
		log.format(L"Not sending file '%hs' to %X", path.str(), playerMask);
		DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("%ls", log.str()));
		if (TheLAN)
			TheLAN->OnChat(L"sendFile", 0, log, LANAPI::LANCHAT_SYSTEM);
		return 0;
	}

	theFile->close();

	Int announceMask = 0xff ^ (1 << m_localSlot);
	NetFileAnnounceCommandMsg *announceMsg = newInstance(NetFileAnnounceCommandMsg);
	announceMsg->setPlayerID(m_localSlot);
	if (DoesCommandRequireACommandID(announceMsg->getNetCommandType()) == TRUE) {
		announceMsg->setID(GenerateNextCommandID());
	}
	announceMsg->setRealFilename(path);
	announceMsg->setPlayerMask(playerMask);
	UnsignedShort fileID = GenerateNextCommandID();
	announceMsg->setFileID(fileID);
	DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("ConnectionManager::sendFileAnnounce() - creating announce message with ID of %d from %d to mask %X for '%s' going to %X as command %d",
		announceMsg->getID(), announceMsg->getPlayerID(), announceMask, announceMsg->getRealFilename().str(),
		announceMsg->getPlayerMask(), announceMsg->getFileID()));

	processFileAnnounce(announceMsg); // set up things for the host

	DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("Sending file announce to %X", announceMask));
	sendLocalCommand(announceMsg, announceMask);
	announceMsg->detach();

	return fileID;
}

void ConnectionManager::sendFile(AsciiString path, UnsignedByte playerMask, UnsignedShort commandID)
{
	#if defined(_WIN64)
	if (!isNetworkHelloReady())
	{
		DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("ConnectionManager::sendFile - NET3 is not ready"));
		return;
	}
	#endif

	File *theFile = TheLocalFileSystem->openFile(path.str());
	if (!theFile || !theFile->size())
	{
		UnicodeString log;
		log.format(L"Not sending file '%hs' to %X", path.str(), playerMask);
		DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("%ls", log.str()));
		if (TheLAN)
			TheLAN->OnChat(L"sendFile", 0, log, LANAPI::LANCHAT_SYSTEM);
		return;
	}

	Int len = theFile->size();
	char *buf = theFile->readEntireAndClose();
	NetCommandDataChunk rawDataChunk(buf, len);

	// compress Targas
#ifdef COMPRESS_TARGAS
	char *compressedBuf = nullptr;
	Int compressedLen = path.endsWith(".tga")?CompressionManager::getMaxCompressedSize(len, CompressionManager::getPreferredCompression()):0;
	Int compressedSize = 0;
	if (compressedLen)
		compressedSize = CompressionManager::compressData(CompressionManager::getPreferredCompression(),
		buf, len, compressedBuf, compressedLen);

	if (!compressedSize)
	{
		delete[] compressedBuf;
		compressedBuf = nullptr;
	}

	NetCommandDataChunk compressedDataChunk(compressedBuf, compressedSize);
#endif // COMPRESS_TARGAS

	NetFileCommandMsg *fileMsg = newInstance(NetFileCommandMsg);
	fileMsg->setPlayerID(m_localSlot);
	fileMsg->setID(commandID);
	fileMsg->setRealFilename(path);

#ifdef COMPRESS_TARGAS
	if (compressedBuf)
	{
		DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("Compressed '%s' from %d to %d (%g%%) before transfer", path.str(), len, compressedSize,
			(Real)compressedSize/(Real)len*100.0f));
		fileMsg->setFileData(compressedDataChunk);
	}
	else
#endif // COMPRESS_TARGAS
	{
		fileMsg->setFileData(rawDataChunk);
	}

	DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("ConnectionManager::sendFile() - creating file message with ID of %d for '%s' going to %X from %d, size of %d",
		fileMsg->getID(), fileMsg->getRealFilename().str(), playerMask, fileMsg->getPlayerID(), fileMsg->getFileLength()));

	DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("Sending file: '%s', len %d, to %X", path.str(), len, playerMask));

	sendLocalCommand(fileMsg, playerMask);

	fileMsg->detach();
}

Int ConnectionManager::getFileTransferProgress(Int playerID, AsciiString path)
{
	FileCommandMap::iterator commandIt = s_fileCommandMap.begin();
	while (commandIt != s_fileCommandMap.end())
	{
		//DEBUG_LOG(("ConnectionManager::getFileTransferProgress(%s): looking at existing transfer of '%s'",
		//	path.str(), commandIt->second.str()));
		if (commandIt->second == path)
		{
			return s_fileProgressMap[playerID][commandIt->first];
		}
		++commandIt;
	}
	//DEBUG_LOG(("Falling back to 0, since we couldn't find the map"));
	DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("ConnectionManager::getFileTransferProgress: path %s not found",path.str()));
	return 0;
}


void ConnectionManager::voteForPlayerDisconnect(Int slot) {
	if (m_disconnectManager != nullptr) {
		m_disconnectManager->voteForPlayerDisconnect(slot, this);
	}
}

Int ConnectionManager::getNumPlayers()
{
	Int retval = 0;
	for (Int i = 0; i < MAX_SLOTS; ++i)
	{
		if (isPlayerConnected(i)) {
			++retval;
		}
	}

	return retval;
}

void ConnectionManager::updateLoadProgress( Int progress )
{
	NetProgressCommandMsg *msg = newInstance(NetProgressCommandMsg);
	msg->setPercentage( progress );
	msg->setPlayerID( m_localSlot );
	if (DoesCommandRequireACommandID(msg->getNetCommandType()) == TRUE) {
		msg->setID(GenerateNextCommandID());
	}
	processProgress(msg);
	sendLocalCommand(msg, 0xff ^ (1 << m_localSlot));

	msg->detach();
}

void ConnectionManager::loadProgressComplete()
{
	NetLoadCompleteCommandMsg *msg = newInstance(NetLoadCompleteCommandMsg);
	msg->setPlayerID( m_localSlot );
	if (DoesCommandRequireACommandID(msg->getNetCommandType()) == TRUE) {
		msg->setID(GenerateNextCommandID());
	}
	processLoadComplete(msg);
	sendLocalCommand(msg, 0xff ^ (1 << m_localSlot));

	msg->detach();
}

void ConnectionManager::sendTimeOutGameStart()
{
	NetTimeOutGameStartCommandMsg *msg = newInstance(NetTimeOutGameStartCommandMsg);
	msg->setPlayerID( m_localSlot );
	if (DoesCommandRequireACommandID(msg->getNetCommandType()) == TRUE) {
		msg->setID(GenerateNextCommandID());
	}
	processTimeOutGameStart(msg);
	sendLocalCommand(msg, 0xff ^ (1 << m_localSlot));

	msg->detach();
}

Bool ConnectionManager::isPacketRouter()
{
	return m_localSlot == m_packetRouterSlot;
}

Int ConnectionManager::getAverageFPS()
{
	return m_frameMetrics.getAverageFPS();
}

Int ConnectionManager::getSlotAverageFPS(Int slot) {
	if ((slot < 0) || (slot >= MAX_SLOTS)) {
		return -1;
	}
	if ((m_packetRouterSlot != m_localSlot) && (slot == m_localSlot)) {
		// our framerate data isn't valid for other players unless we are the
		// packet router, so don't fake someone out.
		return -1;
	}
	return m_fpsAverages[slot];
}

#if defined(RTS_DEBUG)
void ConnectionManager::debugPrintConnectionCommands() {
	DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("ConnectionManager::debugPrintConnectionCommands - begin commands"));
	for (Int i = 0; i < MAX_SLOTS; ++i) {
		if (m_connections[i] != nullptr) {
			DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("ConnectionManager::debugPrintConnectionCommands - commands for connection %d", i));
			m_connections[i]->debugPrintCommands();
		}
	}
	DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("ConnectionManager::debugPrintConnectionCommands - end commands"));
}
#endif

void ConnectionManager::notifyOthersOfCurrentFrame(Int frame) {
	NetDisconnectFrameCommandMsg *msg = newInstance(NetDisconnectFrameCommandMsg);

	msg->setPlayerID(m_localSlot);
	msg->setDisconnectFrame(frame);
	if (DoesCommandRequireACommandID(msg->getNetCommandType())) {
		msg->setID(GenerateNextCommandID());
	}

	DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("ConnectionManager::notifyOthersOfCurrentFrame - sending disconnect frame of %d, command ID = %d", frame, msg->getID()));
	sendLocalCommandDirect(msg, 0xff ^ (1 << m_localSlot));
	NetCommandRef *ref = NEW_NETCOMMANDREF(msg);
	ref->setRelay(1 << m_localSlot);
	m_disconnectManager->processDisconnectCommand(ref, this);
	deleteInstance(ref);

	msg->detach();

	DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("ConnectionManager::notifyOthersOfCurrentFrame - start screen on debug stuff"));
#if defined(RTS_DEBUG)
	debugPrintConnectionCommands();
#endif
}

void ConnectionManager::notifyOthersOfNewFrame(UnsignedInt frame) {
	NetDisconnectScreenOffCommandMsg *msg = newInstance(NetDisconnectScreenOffCommandMsg);

	msg->setPlayerID(m_localSlot);
	msg->setNewFrame(frame);
	if (DoesCommandRequireACommandID(msg->getNetCommandType())) {
		msg->setID(GenerateNextCommandID());
	}

	sendLocalCommandDirect(msg, 0xff ^ (1 << m_localSlot));
	NetCommandRef *ref = NEW_NETCOMMANDREF(msg);
	ref->setRelay(1 << m_localSlot);
	m_disconnectManager->processDisconnectCommand(ref, this);
	deleteInstance(ref);

	msg->detach();
}

void ConnectionManager::sendFrameDataToPlayer(UnsignedInt playerID, UnsignedInt startingFrame) {
#if defined(_WIN64)
	if (playerID >= MAX_SLOTS || !rts::network_epoch::IsNetworkCachedFrameRangeValid(
		startingFrame, TheGameLogic->getFrame(), FRAMES_TO_KEEP))
		return; // Bound the outer loop, not just each attempted frame send.
#endif
	DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("ConnectionManager::sendFrameDataToPlayer - sending frame data to player %d starting with frame %d", playerID, startingFrame));
	for (UnsignedInt frame = startingFrame; frame < TheGameLogic->getFrame(); ++frame) {
		sendSingleFrameToPlayer(playerID, frame);
	}
	DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("ConnectionManager::sendFrameDataToPlayer - done sending commands to player %d", playerID));
}

void ConnectionManager::sendSingleFrameToPlayer(UnsignedInt playerID, UnsignedInt frame) {
#if defined(_WIN64)
	const UnsignedInt currentFrame = TheGameLogic->getFrame();
	if (playerID >= MAX_SLOTS || !rts::network_epoch::IsNetworkCachedFrameRangeValid(frame, currentFrame, FRAMES_TO_KEEP))
		return; // Only completed frames still held by the bounded cache.
	if (m_connections[playerID] == nullptr || m_connections[playerID]->isQuitting() || !isNetworkHelloReady())
		return;
#else
	if ((TheGameLogic->getFrame() - FRAMES_TO_KEEP) > frame) {
		DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("ConnectionManager::sendSingleFrameToPlayer - player %d requested frame %d when we are on frame %d, this is too far in the past.", playerID, frame, TheGameLogic->getFrame()));
		return;
	}
#endif

	UnsignedByte relay = 1 << playerID;

	DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("ConnectionManager::sendFrameDataToPlayer - sending data for frame %d", frame));
	for (Int i = 0; i < MAX_SLOTS; ++i) {
		if ((m_frameData[i] != nullptr) && (i != playerID)) { // no need to send his own commands to him.
			NetCommandList *list = m_frameData[i]->getFrameCommandList(frame);
			if (list != nullptr) {
				NetCommandRef *ref = list->getFirstMessage();
				while (ref != nullptr) {
					DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("ConnectionManager::sendFrameDataToPlayer - sending command %d from player %d to player %d using relay 0x%x", ref->getCommand()->getID(), i, playerID, relay));
#if defined(_WIN64)
					m_connections[playerID]->sendNetCommandMsg(ref->getCommand(), relay, TRUE);
#else
					sendLocalCommandDirect(ref->getCommand(), relay);
#endif
					ref = ref->getNext();
				}
			}
			UnsignedInt frameCommandCount = m_frameData[i]->getFrameCommandCount(frame);
			NetFrameCommandMsg *msg = newInstance(NetFrameCommandMsg);
			msg->setExecutionFrame(frame);
			msg->setCommandCount(frameCommandCount);
			if (DoesCommandRequireACommandID(msg->getNetCommandType())) {
				msg->setID(GenerateNextCommandID());
			}
			msg->setPlayerID(i);
			DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("ConnectionManager::sendFrameDataToPlayer - sending frame info from player %d to player %d for frame %d with command count %d and ID %d and relay %d", i, playerID, msg->getExecutionFrame(), msg->getCommandCount(), msg->getID(), relay));
#if defined(_WIN64)
			m_connections[playerID]->sendNetCommandMsg(msg, relay, TRUE);
#else
			sendLocalCommandDirect(msg, relay);
#endif
			msg->detach();
		}
	}
}

UnsignedInt ConnectionManager::getNextPacketRouterSlot(UnsignedInt playerID) {
	return FindNextPacketRouterSlot(m_packetRouterFallback, playerID);
}

void ConnectionManager::requestFrameDataResend(Int playerID, UnsignedInt frame) {
	NetFrameResendRequestCommandMsg *msg = newInstance(NetFrameResendRequestCommandMsg);
	msg->setPlayerID(m_localSlot);
	msg->setFrameToResend(frame);
	if (DoesCommandRequireACommandID(msg->getNetCommandType())) {
		msg->setID(GenerateNextCommandID());
	}

#if defined(_WIN64)
	// A newer resend supersedes any older provenance exception, including a
	// request that could not find a connected responder.
	clearNetworkFrameResendRequest();
#endif

	if (isPlayerConnected(playerID) == FALSE) {
		playerID = 0;
		while ((playerID < MAX_SLOTS) && (isPlayerConnected(playerID) == FALSE)) {
			++playerID;
		}
	}

	if (playerID < MAX_SLOTS) {
	#if defined(_WIN64)
		if (static_cast<UnsignedInt>(playerID) != m_localSlot)
		{
			for (Int sourceSlot = 0; sourceSlot < MAX_SLOTS; ++sourceSlot)
			{
				if (sourceSlot != m_localSlot && m_frameData[sourceSlot] != nullptr)
					m_frameResendRequestExpectedInfoMask |= 1U << sourceSlot;
			}
			m_frameResendRequestResponder = static_cast<UnsignedInt>(playerID);
			m_frameResendRequestFrame = frame;
			m_frameResendRequestStartTime = static_cast<UnsignedInt>(timeGetTime());
			m_frameResendRequestOutstanding =
				m_frameResendRequestExpectedInfoMask != 0U;
		}
	#endif
		sendLocalCommandDirect(msg, 1 << playerID);
	}

	msg->detach();
}
