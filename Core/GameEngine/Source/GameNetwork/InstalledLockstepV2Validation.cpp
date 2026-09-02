#include "PreRTS.h"

#include "GameNetwork/InstalledLockstepV2Validation.h"

#if defined(_WIN64)

#include "Common/GlobalData.h"
#include "Common/MessageStream.h"
#include "Common/RandomValue.h"
#include "GameClient/MapUtil.h"
#include "GameNetwork/GameInfo.h"
#include "GameNetwork/NetworkInterface.h"
#include "GameLogic/AIPathfind.h"
#include "GameLogic/GameLogic.h"
#include "Lib/CollisionCandidateKernel.h"
#include "Lib/DeterministicAIPlanning.h"
#include "Lib/ImmutableSpatialQueryRuntime.h"
#include "Lib/JobSystem.h"
#include "Lib/ObjectStatusTimerKernel.h"
#include "Lib/PhysicsIntegrationKernel.h"
#include "Lib/SimulationExecutionPolicy.h"

#include <array>
#include <bcrypt.h>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>
#include <windows.h>

namespace rts
{
namespace
{

const unsigned kLoopbackAddress = 0x7f000001U;
const unsigned kMaximumConfigurationCharacters = 4095U;
const unsigned kQualificationTimeoutMilliseconds = 120000U;
const unsigned kKnownKernelMask = (1U << lockstep_v2::kKernelCount) - 1U;

unsigned CountBits(std::uint64_t value)
{

	unsigned count = 0U;
	while (value != 0U)
	{
		value &= value - 1U;
		++count;
	}
	return count;
}

bool ParseUnsigned(const std::string &text, std::uint64_t maximum,
	unsigned *value)
{

	if (text.empty() || value == 0)
		return false;
	std::uint64_t parsed = 0U;
	for (std::size_t index = 0U; index < text.size(); ++index)
	{
		const char character = text[index];
		if (character < '0' || character > '9')
			return false;
		const std::uint64_t digit = static_cast<std::uint64_t>(character - '0');
		if (digit > maximum || parsed > (maximum - digit) / 10U)
			return false;
		parsed = parsed * 10U + digit;
	}
	*value = static_cast<unsigned>(parsed);
	return true;
}

bool IsCanonicalHex(const std::string &text, std::size_t characters)
{

	return text.size() == characters &&
		lockstep_v2::IsCanonicalHex(text.c_str(), characters);
}

void Uppercase(std::string &text)
{

	for (std::size_t index = 0U; index < text.size(); ++index)
		text[index] = static_cast<char>(std::toupper(
			static_cast<unsigned char>(text[index])));
}

void Lowercase(std::string &text)
{

	for (std::size_t index = 0U; index < text.size(); ++index)
		text[index] = static_cast<char>(std::tolower(
			static_cast<unsigned char>(text[index])));
}

bool IsSafeMapName(const std::string &path)
{

	if (path.empty() || path.size() >= MAX_PATH ||
		path.find("..") != std::string::npos ||
		path.find(':') != std::string::npos ||
		path.find(';') != std::string::npos ||
		path.find('"') != std::string::npos ||
		path[0] == '\\' || path[0] == '/')
	{
		return false;
	}
	if (path.size() < 4U)
		return false;
	const std::string extension = path.substr(path.size() - 4U);
	return (extension[0] == '.' &&
		std::tolower(static_cast<unsigned char>(extension[1])) == 'm' &&
		std::tolower(static_cast<unsigned char>(extension[2])) == 'a' &&
		std::tolower(static_cast<unsigned char>(extension[3])) == 'p');
}

bool IsSafeReceiptName(const std::string &name)
{

	return !name.empty() && name.size() < MAX_PATH &&
		name.find("..") == std::string::npos &&
		name.find(':') == std::string::npos &&
		name.find('\\') == std::string::npos &&
		name.find('/') == std::string::npos &&
		name.find(';') == std::string::npos &&
		name.find('"') == std::string::npos;
}

bool IsSafeReceiptDirectory(const std::string &path)
{

	// The installed qualification runner deliberately keeps its receipts on
	// the task-owned H: volume.  This also prevents a malformed command line
	// from redirecting the proof into an arbitrary relative directory.
	return path.size() >= 4U && path.size() < MAX_PATH &&
		(path[0] == 'H' || path[0] == 'h') && path[1] == ':' &&
		(path[2] == '\\' || path[2] == '/') &&
		path.find("..") == std::string::npos &&
		path.find(';') == std::string::npos &&
		path.find('"') == std::string::npos;
}

bool IsSafeValue(const std::string &value)
{

	if (value.empty())
		return false;
	for (std::size_t index = 0U; index < value.size(); ++index)
	{
		if (std::iscntrl(static_cast<unsigned char>(value[index])) ||
			std::isspace(static_cast<unsigned char>(value[index])))
		{
			return false;
		}
	}
	return true;
}

struct InstalledLockstepV2Config
{
	bool requested;
	bool prepared;
	bool failed;
	bool authorityReady;
	bool refreshAttempted;
	bool proofStarted;
	bool gameplayCommandQueued;
	bool stopRequested;
	bool finalized;
	bool expectedBuildPresent;
	bool expectedContentPresent;
	ULONGLONG preparedAt;
	unsigned localSlot;
	unsigned peerCount;
	unsigned mapCrc;
	unsigned seed;
	unsigned expectedBuild;
	unsigned expectedContent;
	std::array<unsigned, lockstep_v2::kMaxPeerCount> ports;
	std::string runNonce;
	std::string sessionNonce;
	std::string executableSha256;
	std::string sourceRevision;
	std::string mapName;
	std::string directory;
	std::string receiptName;
	std::string receiptPath;
	lockstep_v2::WorkerTelemetry telemetry;
	GameInfo *gameInfo;

	InstalledLockstepV2Config() : requested(false), prepared(false),
		failed(false), authorityReady(false), refreshAttempted(false),
		proofStarted(false), gameplayCommandQueued(false), stopRequested(false),
		finalized(false), expectedBuildPresent(false), expectedContentPresent(false),
		preparedAt(0U),
		localSlot(0U),
		peerCount(0U), mapCrc(0U), seed(0U), expectedBuild(0U),
		expectedContent(0U), ports({{}}), telemetry(), gameInfo(nullptr) {}
};

InstalledLockstepV2Config g_config;

enum ConfigurationField
{
	FIELD_PEER,
	FIELD_PEERS,
	FIELD_PORTS,
	FIELD_RUN,
	FIELD_SESSION,
	FIELD_EXE,
	FIELD_SOURCE,
	FIELD_MAP,
	FIELD_MAP_CRC,
	FIELD_SEED,
	FIELD_DIRECTORY,
	FIELD_RECEIPT,
	FIELD_MODE,
	FIELD_ROUTER,
	FIELD_BUILD,
	FIELD_CONTENT,
	FIELD_COUNT
};

const char *const kFieldNames[FIELD_COUNT] =
{
	"peer", "peers", "ports", "run", "session", "exe", "source", "map",
	"map_crc", "seed", "dir", "receipt", "mode", "router", "build", "content"
};

bool GetFieldIndex(const std::string &name, unsigned *index)
{

	if (index == 0)
		return false;
	for (unsigned candidate = 0U; candidate < FIELD_COUNT; ++candidate)
	{
		if (name == kFieldNames[candidate])
		{
			*index = candidate;
			return true;
		}
	}
	return false;
}

bool BoundedLength(const char *text, std::size_t *length)
{

	if (text == nullptr || length == nullptr)
		return false;
	for (std::size_t index = 0U; index <= kMaximumConfigurationCharacters;
		++index)
	{
		if (text[index] == '\0')
		{
			*length = index;
			return index != 0U;
		}
	}
	return false;
}

bool ParsePorts(const std::string &text, unsigned peerCount,
	std::array<unsigned, lockstep_v2::kMaxPeerCount> *ports)
{

	if (ports == nullptr || peerCount < lockstep_v2::kMinPeerCount ||
		peerCount > lockstep_v2::kMaxPeerCount)
	{
		return false;
	}
	std::size_t cursor = 0U;
	for (unsigned index = 0U; index < peerCount; ++index)
	{
		const std::size_t end = text.find(',', cursor);
		const std::size_t length = end == std::string::npos ?
			text.size() - cursor : end - cursor;
		if (length == 0U)
			return false;
		unsigned port = 0U;
		if (!ParseUnsigned(text.substr(cursor, length), 65535U, &port) ||
			port < 1024U)
		{
			return false;
		}
		for (unsigned prior = 0U; prior < index; ++prior)
			if ((*ports)[prior] == port)
				return false;
		(*ports)[index] = port;
		if (end == std::string::npos)
		{
			return index + 1U == peerCount;
		}
		cursor = end + 1U;
	}
	return cursor == text.size();
}

bool ParseConfiguration(const char *configuration,
	InstalledLockstepV2Config *parsed)
{

	if (configuration == nullptr || parsed == nullptr)
		return false;
	std::size_t configurationLength = 0U;
	if (!BoundedLength(configuration, &configurationLength))
		return false;
	const std::string text(configuration, configurationLength);
	std::array<bool, FIELD_COUNT> seen = {{}};
	std::array<std::string, FIELD_COUNT> values;
	std::size_t cursor = 0U;
	while (cursor < text.size())
	{
		const std::size_t end = text.find(';', cursor);
		const std::size_t length = end == std::string::npos ?
			text.size() - cursor : end - cursor;
		if (length == 0U)
			return false;
		const std::string field = text.substr(cursor, length);
		const std::size_t equals = field.find('=');
		if (equals == std::string::npos || equals == 0U ||
			equals + 1U >= field.size())
		{
			return false;
		}
		unsigned fieldIndex = 0U;
		const std::string name = field.substr(0U, equals);
		const std::string value = field.substr(equals + 1U);
		if (!GetFieldIndex(name, &fieldIndex) || seen[fieldIndex] ||
			!IsSafeValue(value))
		{
			return false;
		}
		seen[fieldIndex] = true;
		values[fieldIndex] = value;
		if (end == std::string::npos)
			break;
		cursor = end + 1U;
	}

	const unsigned required[] =
	{
		FIELD_PEER, FIELD_PEERS, FIELD_PORTS, FIELD_RUN, FIELD_SESSION,
		FIELD_EXE, FIELD_SOURCE, FIELD_MAP, FIELD_MAP_CRC, FIELD_SEED,
		FIELD_DIRECTORY, FIELD_RECEIPT, FIELD_MODE, FIELD_ROUTER
	};
	for (unsigned index = 0U; index < sizeof(required) / sizeof(required[0]);
		++index)
	{
		if (!seen[required[index]])
			return false;
	}

	unsigned parsedValue = 0U;
	if (!ParseUnsigned(values[FIELD_PEER], lockstep_v2::kMaxPeerCount - 1U,
		&parsedValue))
	{
		return false;
	}
	parsed->localSlot = parsedValue;
	if (!ParseUnsigned(values[FIELD_PEERS], lockstep_v2::kMaxPeerCount,
		&parsedValue) || parsedValue < lockstep_v2::kMinPeerCount ||
		parsedValue > lockstep_v2::kMaxPeerCount || parsed->localSlot >= parsedValue)
	{
		return false;
	}
	parsed->peerCount = parsedValue;
	if (!ParsePorts(values[FIELD_PORTS], parsed->peerCount, &parsed->ports) ||
		!IsCanonicalHex(values[FIELD_RUN], lockstep_v2::kNonceHexChars) ||
		!IsCanonicalHex(values[FIELD_SESSION], lockstep_v2::kNonceHexChars) ||
		!IsCanonicalHex(values[FIELD_EXE], lockstep_v2::kSha256HexChars) ||
		!IsCanonicalHex(values[FIELD_SOURCE], lockstep_v2::kSourceRevisionHexChars) ||
		!IsSafeMapName(values[FIELD_MAP]) ||
		!IsSafeReceiptDirectory(values[FIELD_DIRECTORY]) ||
		!IsSafeReceiptName(values[FIELD_RECEIPT]) ||
		values[FIELD_MODE] != "trusted-router" ||
		!ParseUnsigned(values[FIELD_ROUTER], parsed->peerCount, &parsedValue) ||
		parsedValue != 0U)
	{
		return false;
	}
	parsed->mapName = values[FIELD_MAP];
	parsed->mapCrc = 0U;
	if (!ParseUnsigned(values[FIELD_MAP_CRC], 0xffffffffULL, &parsed->mapCrc) ||
		parsed->mapCrc == 0U ||
		!ParseUnsigned(values[FIELD_SEED], 0x7ffffffeULL, &parsed->seed))
	{
		return false;
	}
	parsed->runNonce = values[FIELD_RUN];
	parsed->sessionNonce = values[FIELD_SESSION];
	parsed->executableSha256 = values[FIELD_EXE];
	parsed->sourceRevision = values[FIELD_SOURCE];
	Uppercase(parsed->executableSha256);
	if (seen[FIELD_BUILD])
	{
		parsed->expectedBuildPresent = true;
		if (!ParseUnsigned(values[FIELD_BUILD], 0xffffffffULL,
			&parsed->expectedBuild))
		{
			return false;
		}
	}
	if (seen[FIELD_CONTENT])
	{
		parsed->expectedContentPresent = true;
		if (!ParseUnsigned(values[FIELD_CONTENT], 0xffffffffULL,
			&parsed->expectedContent))
		{
			return false;
		}
	}
	parsed->directory = values[FIELD_DIRECTORY];
	parsed->receiptName = values[FIELD_RECEIPT];
	parsed->receiptPath = parsed->directory;
	if (parsed->receiptPath[parsed->receiptPath.size() - 1U] != '\\' &&
		parsed->receiptPath[parsed->receiptPath.size() - 1U] != '/')
	{
		parsed->receiptPath += '\\';
	}
	parsed->receiptPath += parsed->receiptName;
	return true;
}

bool CalculateCurrentExecutableSha256(std::string *digest)
{

	if (digest == nullptr)
		return false;
	char path[MAX_PATH] = {};
	const DWORD pathLength = GetModuleFileNameA(nullptr, path,
		static_cast<DWORD>(sizeof(path)));
	if (pathLength == 0U || pathLength >= sizeof(path))
		return false;
	HANDLE file = CreateFileA(path, GENERIC_READ,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
		OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
		nullptr);
	if (file == INVALID_HANDLE_VALUE)
		return false;
	BCRYPT_ALG_HANDLE algorithm = nullptr;
	BCRYPT_HASH_HANDLE hash = nullptr;
	DWORD objectLength = 0U;
	DWORD hashLength = 0U;
	DWORD propertyBytes = 0U;
	std::vector<unsigned char> object;
	std::array<unsigned char, 32U> result = {{}};
	bool success = BCryptOpenAlgorithmProvider(&algorithm,
		BCRYPT_SHA256_ALGORITHM, nullptr, 0U) == 0 &&
		BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
			reinterpret_cast<PUCHAR>(&objectLength), sizeof(objectLength),
			&propertyBytes, 0U) == 0 &&
		BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH,
			reinterpret_cast<PUCHAR>(&hashLength), sizeof(hashLength),
			&propertyBytes, 0U) == 0 && hashLength == result.size();
	if (success)
	{
		object.resize(objectLength);
		success = objectLength != 0U && BCryptCreateHash(algorithm, &hash,
			object.data(), objectLength, nullptr, 0U, 0U) == 0;
	}
	if (success)
	{
		std::array<unsigned char, 64U * 1024U> buffer = {{}};
		for (;;)
		{
			DWORD bytesRead = 0U;
			if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()),
				&bytesRead, nullptr))
			{
				success = false;
				break;
			}
			if (bytesRead != 0U && BCryptHashData(hash, buffer.data(),
				bytesRead, 0U) != 0)
			{
				success = false;
				break;
			}
			if (bytesRead == 0U)
				break;
		}
	}
	if (success && BCryptFinishHash(hash, result.data(),
		static_cast<ULONG>(result.size()), 0U) != 0)
	{
		success = false;
	}
	if (hash != nullptr)
		BCryptDestroyHash(hash);
	if (algorithm != nullptr)
		BCryptCloseAlgorithmProvider(algorithm, 0U);
	CloseHandle(file);
	if (!success)
		return false;
	static const char kHex[] = "0123456789ABCDEF";
	digest->resize(result.size() * 2U);
	for (std::size_t index = 0U; index < result.size(); ++index)
	{
		(*digest)[index * 2U] = kHex[result[index] >> 4U];
		(*digest)[index * 2U + 1U] = kHex[result[index] & 0x0fU];
	}
	return true;
}

class QualificationGameInfo : public SkirmishGameInfo
{
public:
	explicit QualificationGameInfo(unsigned localSlot) : m_localSlot(localSlot) {}

	virtual Int getLocalSlotNum() const override
	{
		return static_cast<Int>(m_localSlot);
	}

	virtual Bool amIHost() const override
	{
		return m_localSlot == 0U;
	}

private:
	unsigned m_localSlot;
};

bool AssignProductionKernelTelemetry(std::uint64_t physicalWorkerJobs,
	std::uint64_t physicalWorkerMask, unsigned peakConcurrentWorkers,
	bool physicalWorkerMaskComplete,
	lockstep_v2::KernelWorkerTelemetry *telemetry)
{
	if (telemetry == nullptr || physicalWorkerJobs == 0U ||
		physicalWorkerJobs > 0xffffffffULL || physicalWorkerMask == 0U ||
		!physicalWorkerMaskComplete)
	{
		return false;
	}
	const unsigned distinctWorkers = CountBits(physicalWorkerMask);
	if (distinctWorkers < 2U || peakConcurrentWorkers < 2U ||
		peakConcurrentWorkers > distinctWorkers)
	{
		return false;
	}
	telemetry->physicalWorkerMask = physicalWorkerMask;
	telemetry->physicalWorkerJobs = static_cast<std::uint32_t>(physicalWorkerJobs);
	telemetry->distinctPhysicalWorkers = distinctWorkers;
	telemetry->peakConcurrentPhysicalWorkers = peakConcurrentWorkers;
	telemetry->physicalWorkerMaskComplete = true;
	return true;
}

void ResetProductionKernelTelemetry()
{
	ResetPhysicsIntegrationRuntimeMetrics();
	ResetObjectStatusTimerRuntimeMetrics();
	ResetCollisionCandidateRuntimeMetrics();
	ResetAIPlanningRuntimeMetrics();
	ResetImmutableSpatialRuntimeMetrics();
	ResetOrdinaryPathRuntimeMetrics();
}

bool CollectProductionKernelTelemetry(lockstep_v2::WorkerTelemetry *telemetry)
{
	if (telemetry == nullptr || !g_config.stopRequested)
		return false;
	JobSystem &jobs = JobSystem::instance();
	if (!jobs.isRunning() || !jobs.isCurrentThread(JOB_OWNER_GAME) ||
		jobs.workerCount() < 2U || jobs.workerCount() > 64U ||
		GetSimulationExecutionMode() != SIMULATION_EXECUTION_PARALLEL)
	{
		return false;
	}

	const PhysicsIntegrationRuntimeMetrics physics =
		GetPhysicsIntegrationRuntimeMetrics();
	const ObjectStatusTimerRuntimeMetrics status =
		GetObjectStatusTimerRuntimeMetrics();
	const AIPlanningRuntimeMetrics ai = GetAIPlanningRuntimeMetrics();
	const OrdinaryPathRuntimeMetrics path = GetOrdinaryPathRuntimeMetrics();
	lockstep_v2::WorkerTelemetry observed;
	observed.executableOrigin = true;

	const bool physicsValid = physics.acceptedBatches > 0U &&
		physics.acceptedOwnerHelpedJobs == 0U &&
		physics.unexpectedFallbacks == 0U && physics.circuitBreakerTrips == 0U &&
		AssignProductionKernelTelemetry(physics.acceptedPhysicalWorkerJobs,
			physics.acceptedPhysicalWorkerMask,
			physics.maximumAcceptedPeakConcurrentPhysicalWorkers,
			physics.acceptedPhysicalWorkerMaskComplete, &observed.kernels[0]);
	const bool statusValid = status.authoritativeBatches > 0U &&
		status.ownerHelpedJobs == 0U && status.shadowMismatches == 0U &&
		AssignProductionKernelTelemetry(status.physicalWorkerJobs,
			status.physicalWorkerMask,
			status.maximumPeakConcurrentPhysicalWorkers,
			status.physicalWorkerMaskComplete, &observed.kernels[1]);
	// Collision currently records workers per accepted batch but not their peak
	// overlap.  A union/distinct count cannot be relabeled as concurrency, so the
	// v2 authority bit remains fail-closed until that production metric exists.
	const bool collisionValid = false;
	const bool aiValid = ai.parallelAuthoritativeCommits > 0U &&
		ai.ownerHelpedExecutions == 0U && ai.shadowMismatches == 0U &&
		ai.validationFailures == 0U &&
		AssignProductionKernelTelemetry(ai.physicalWorkerExecutions,
			ai.observedPhysicalWorkerMask,
			static_cast<unsigned>(ai.maximumConcurrentPhysicalWorkers), true,
			&observed.kernels[3]);
	// Spatial has the same boundary: its process-wide mask and per-collection
	// distinct maximum do not prove concurrent overlap or mask completeness.
	const bool spatialValid = false;
	const bool pathValid = path.authoritativeMultiWorkerCommits > 0U &&
		path.ownerHelpedRangeJobs == 0U && path.failedRangeJobs == 0U &&
		path.validationFailures == 0U && path.shadowMismatches == 0U &&
		AssignProductionKernelTelemetry(path.workerExecutedRangeJobs,
			path.physicalWorkerMask, path.peakActiveWorkers,
			path.physicalWorkerMaskComplete != FALSE, &observed.kernels[5]);
	if (!physicsValid || !statusValid || !collisionValid || !aiValid ||
		!spatialValid || !pathValid)
	{
		return false;
	}
	observed.authorityMask = kKnownKernelMask;
	*telemetry = observed;
	return true;
}

template <std::size_t N>
bool CopyText(const std::string &text, std::array<char, N> *destination)
{

	if (destination == nullptr || text.empty() || text.size() + 1U > N)
		return false;
	std::memset(destination->data(), 0, N);
	std::memcpy(destination->data(), text.data(), text.size());
	return true;
}

bool BuildSession(unsigned buildCompatibilityCrc, unsigned contentCrc,
	lockstep_v2::SessionContract *session)
{

	if (session == nullptr || !g_config.authorityReady ||
		g_config.peerCount < lockstep_v2::kMinPeerCount)
	{
		return false;
	}
	*session = lockstep_v2::SessionContract();
	session->localSlot = g_config.localSlot;
	session->peerCount = g_config.peerCount;
	session->rosterMask = (1U << g_config.peerCount) - 1U;
	session->buildCompatibilityCrc = buildCompatibilityCrc;
	session->contentCrc = contentCrc;
	session->mapCrc = g_config.mapCrc;
	session->provenKernelMask = g_config.telemetry.authorityMask;
	session->packetRouterSlot = 0U;
	session->originMode = lockstep_v2::CommandOriginMode::TrustedRouter;
	return CopyText(g_config.runNonce, &session->runNonce) &&
		CopyText(g_config.sessionNonce, &session->sessionNonce) &&
		CopyText(g_config.executableSha256, &session->executableSha256) &&
		CopyText(g_config.sourceRevision, &session->sourceRevision) &&
		lockstep_v2::IsValidSessionContract(*session);
}

bool IsDirectoryReady(const std::string &path)
{

	const DWORD attributes = GetFileAttributesA(path.c_str());
	return attributes != INVALID_FILE_ATTRIBUTES &&
		(attributes & FILE_ATTRIBUTE_DIRECTORY) != 0U;
}

bool WriteFileAtomically(const std::string &path, const char *bytes,
	std::size_t byteCount)
{

	if (bytes == nullptr || byteCount == 0U || byteCount > 0xffffffffULL)
		return false;
	std::ostringstream temporaryPath;
	temporaryPath << path << ".tmp-" << GetCurrentProcessId() << "-" <<
		GetTickCount();
	const std::string temporary = temporaryPath.str();
	HANDLE file = CreateFileA(temporary.c_str(), GENERIC_WRITE, 0U, nullptr,
		CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
	if (file == INVALID_HANDLE_VALUE)
		return false;
	DWORD written = 0U;
	const bool success = WriteFile(file, bytes, static_cast<DWORD>(byteCount),
		&written, nullptr) != FALSE && written == byteCount &&
		FlushFileBuffers(file) != FALSE;
	CloseHandle(file);
	if (!success || MoveFileExA(temporary.c_str(), path.c_str(),
		MOVEFILE_WRITE_THROUGH) == FALSE)
	{
		DeleteFileA(temporary.c_str());
		return false;
	}
	return true;
}

std::string Failure(const char *reason)
{

	if (reason != nullptr)
	{
		std::printf("LOCKSTEP_V2_VALIDATION_FAIL reason=%s\\n", reason);
		std::fflush(stdout);
	}
	return std::string();
}

} // namespace

bool ConfigureInstalledLockstepV2Qualification(const char *configuration)
{

	if (g_config.requested || configuration == nullptr)
		return false;
	InstalledLockstepV2Config parsed;
	if (!ParseConfiguration(configuration, &parsed))
		return false;
	parsed.requested = true;
	g_config = parsed;
	return true;
}

bool IsInstalledLockstepV2QualificationRequested()
{

	return g_config.requested;
}

bool PrepareInstalledLockstepV2Qualification(unsigned buildCompatibilityCrc,
	unsigned contentCrc)
{

	if (!g_config.requested || g_config.prepared || g_config.failed ||
		buildCompatibilityCrc == 0U || contentCrc == 0U ||
		(g_config.expectedBuildPresent &&
			g_config.expectedBuild != buildCompatibilityCrc) ||
		(g_config.expectedContentPresent &&
			g_config.expectedContent != contentCrc))
	{
		g_config.failed = true;
		return false;
	}
	std::string executableSha256;
	if (!CalculateCurrentExecutableSha256(&executableSha256) ||
		executableSha256 != g_config.executableSha256 ||
		!IsDirectoryReady(g_config.directory) || TheMapCache == nullptr ||
		TheGlobalData == nullptr || TheMessageStream == nullptr)
	{
		g_config.failed = true;
		return false;
	}
	const MapMetaData *map = TheMapCache->findMap(
		AsciiString(g_config.mapName.c_str()));
	if (map == nullptr || !map->m_doesExist || map->m_CRC != g_config.mapCrc)
	{
		g_config.failed = true;
		return false;
	}

	QualificationGameInfo *game = NEW QualificationGameInfo(g_config.localSlot);
	if (game == nullptr)
	{
		g_config.failed = true;
		return false;
	}
	game->init();
	game->clearSlotList();
	game->enterGame();
	game->setLocalIP(kLoopbackAddress);
	for (unsigned slotIndex = 0U; slotIndex < MAX_SLOTS; ++slotIndex)
	{
		GameSlot *slot = game->getSlot(static_cast<Int>(slotIndex));
		if (slotIndex >= g_config.peerCount)
		{
			slot->setState(SLOT_CLOSED);
			continue;
		}
		AsciiString name;
		name.format("lockstep-v2-peer-%u", slotIndex);
		UnicodeString unicodeName;
		unicodeName.translate(name);
		slot->setState(SLOT_PLAYER, unicodeName, kLoopbackAddress);
		slot->setPort(static_cast<UnsignedShort>(g_config.ports[slotIndex]));
		slot->setPlayerTemplate(0);
		slot->setColor(static_cast<Int>(slotIndex));
		slot->setStartPos(static_cast<Int>(slotIndex));
		slot->setTeamNumber(static_cast<Int>(slotIndex & 1U));
		slot->setMapAvailability(TRUE);
		slot->setAccept();
	}
	game->setMap(AsciiString(g_config.mapName.c_str()));
	game->setMapCRC(g_config.mapCrc);
	game->setMapSize(map->m_filesize);
	game->setMapContentsMask(1);
	game->setSeed(static_cast<Int>(g_config.seed));
	game->setCRCInterval(1);
	for (unsigned slotIndex = 0U; slotIndex < g_config.peerCount; ++slotIndex)
		game->getSlot(static_cast<Int>(slotIndex))->setMapAvailability(TRUE);
	game->startGame(0);

	delete TheSkirmishGameInfo;
	TheSkirmishGameInfo = game;
	TheGameInfo = game;
	g_config.gameInfo = game;
	TheWritableGlobalData->m_mapName = game->getMap();
	InitRandom(g_config.seed);

	if (TheNetwork == nullptr)
		TheNetwork = NetworkInterface::createNetwork();
	if (TheNetwork == nullptr)
	{
		g_config.failed = true;
		return false;
	}
	TheNetwork->init();
	TheNetwork->setLocalAddress(kLoopbackAddress,
		g_config.ports[g_config.localSlot]);
	TheNetwork->initTransport();
	TheNetwork->parseUserList(game);
	GameMessage *message = TheMessageStream->appendMessage(
		GameMessage::MSG_NEW_GAME);
	if (message == nullptr)
	{
		g_config.failed = true;
		return false;
	}
	message->appendIntegerArgument(GAME_LAN);
	message->appendIntegerArgument(DIFFICULTY_NORMAL);
	message->appendIntegerArgument(0);
	g_config.preparedAt = GetTickCount64();
	g_config.prepared = true;
	std::printf("LOCKSTEP_V2_VALIDATION_PREPARED peer=%u peers=%u port=%u\n",
		g_config.localSlot, g_config.peerCount,
		g_config.ports[g_config.localSlot]);
	std::fflush(stdout);
	return true;
}

bool ServiceInstalledLockstepV2Qualification(unsigned buildCompatibilityCrc,
	unsigned contentCrc)
{

	if (!g_config.requested || !g_config.prepared || g_config.failed ||
		g_config.finalized)
	{
		return false;
	}
	if (GetTickCount64() - g_config.preparedAt >
		kQualificationTimeoutMilliseconds)
	{
		g_config.failed = true;
		return false;
	}
	if (TheNetwork == nullptr)
	{
		g_config.failed = true;
		return false;
	}
	TheNetwork->liteupdate();
	if (!g_config.authorityReady)
	{
		JobSystem &jobs = JobSystem::instance();
		if (!jobs.isRunning() || !jobs.isCurrentThread(JOB_OWNER_GAME) ||
			jobs.workerCount() < 2U || jobs.workerCount() > 64U ||
			GetSimulationExecutionMode() != SIMULATION_EXECUTION_PARALLEL)
		{
			return false;
		}
		g_config.telemetry = lockstep_v2::WorkerTelemetry();
		g_config.telemetry.authorityMask = kKnownKernelMask;
		g_config.telemetry.executableOrigin = true;
		g_config.authorityReady = true;
	}
	if (TheNetwork == nullptr || TheNetwork->hasNetworkHelloFailure())
	{
		g_config.failed = true;
		return false;
	}
	if (!g_config.refreshAttempted)
	{
		g_config.refreshAttempted = true;
		if (!TheNetwork->refreshNetworkSimulationPolicyForLockstepV2())
		{
			g_config.failed = true;
			return false;
		}
		return true;
	}
	if (TheNetwork->hasNetworkHelloFailure())
	{
		g_config.failed = true;
		return false;
	}
	if (!TheNetwork->isNetworkHelloReady() ||
		!TheNetwork->isNetworkSimulationPolicyUsable())
	{
		return true;
	}
	if (TheNetwork->getMultiplayerSimulationEnabledKernelMask() !=
		g_config.telemetry.authorityMask)
	{
		g_config.failed = true;
		return false;
	}
	if (!g_config.proofStarted)
	{
		lockstep_v2::SessionContract session;
		if (!BuildSession(buildCompatibilityCrc, contentCrc, &session) ||
			!TheNetwork->beginLockstepV2Proof(session))
		{
			g_config.failed = true;
			return false;
		}
		ResetProductionKernelTelemetry();
		g_config.proofStarted = true;
		std::printf("LOCKSTEP_V2_VALIDATION_ACTIVE peer=%u frame_limit=%u\n",
			g_config.localSlot, lockstep_v2::kCommonStopFrame);
		std::fflush(stdout);
	}
	return true;
}

bool IsInstalledLockstepV2QualificationActive()
{

	return g_config.requested && g_config.prepared &&
		g_config.authorityReady && !g_config.failed && !g_config.finalized;
}

bool IsInstalledLockstepV2ProofStarted()
{

	return IsInstalledLockstepV2QualificationActive() &&
		g_config.proofStarted;
}

bool IsInstalledLockstepV2QualificationFailed()
{

	return g_config.failed;
}

bool RecordInstalledLockstepV2Frame(unsigned frame, unsigned crc)
{

	if (!IsInstalledLockstepV2ProofStarted() || TheNetwork == nullptr ||
		frame == 0U || frame > lockstep_v2::kCommonStopFrame ||
		!TheNetwork->recordLockstepV2Frame(frame, crc, 0U))
	{
		g_config.failed = true;
		return false;
	}
	// Every peer must originate a real synchronized game command after the
	// network has entered gameplay.  Creating an empty hotkey group is a
	// deterministic, bounded no-op for the validation roster, but it still
	// traverses MessageStream -> Network -> ConnectionManager and is recorded
	// from the canonical serialized NetGameCommandMsg bytes on every peer.
	if (!g_config.gameplayCommandQueued && frame >= 8U)
	{
		const GameMessage::Type commandType = static_cast<GameMessage::Type>(
			GameMessage::MSG_CREATE_TEAM0 + (g_config.localSlot % 10U));
		if (TheMessageStream == nullptr ||
			TheMessageStream->appendMessage(commandType) == nullptr)
		{
			g_config.failed = true;
			return false;
		}
		g_config.gameplayCommandQueued = true;
	}
	if (frame == lockstep_v2::kCommonStopFrame)
		g_config.stopRequested = true;
	return true;
}

unsigned GetInstalledLockstepV2ValidationAuthorityMask(
	unsigned buildCompatibilityCrc, unsigned contentCrc)
{

	if (!IsInstalledLockstepV2QualificationActive() ||
		buildCompatibilityCrc == 0U || contentCrc == 0U)
	{
		return 0U;
	}
	if (g_config.expectedBuildPresent &&
		g_config.expectedBuild != buildCompatibilityCrc)
	{
		return 0U;
	}
	if (g_config.expectedContentPresent &&
		g_config.expectedContent != contentCrc)
	{
		return 0U;
	}
	return g_config.telemetry.authorityMask;
}

bool GetInstalledLockstepV2WorkerTelemetry(
	lockstep_v2::WorkerTelemetry *telemetry)
{

	if (telemetry == nullptr || !IsInstalledLockstepV2QualificationActive())
		return false;
	return CollectProductionKernelTelemetry(telemetry);
}

GameInfo *GetInstalledLockstepV2GameInfo()
{

	return g_config.requested && g_config.prepared && !g_config.failed ?
		g_config.gameInfo : nullptr;
}

void RequestInstalledLockstepV2Stop()
{

	if (g_config.proofStarted && IsInstalledLockstepV2QualificationActive())
		g_config.stopRequested = true;
}

bool IsInstalledLockstepV2StopRequested()
{

	return g_config.stopRequested;
}

bool FinalizeInstalledLockstepV2Qualification(bool cleanShutdown)
{

	if (!g_config.proofStarted || !g_config.stopRequested ||
		g_config.finalized || g_config.failed || TheNetwork == nullptr ||
		!cleanShutdown)
	{
		return false;
	}
	lockstep_v2::Receipt receipt;
	if (!TheNetwork->finalizeLockstepV2Proof(TRUE, &receipt))
	{
		g_config.failed = true;
		return false;
	}
	std::array<char, lockstep_v2::kReceiptBufferBytes> encoded = {{}};
	std::size_t written = 0U;
	if (!lockstep_v2::EncodeReceipt(receipt, encoded.data(), encoded.size(),
		&written) || !WriteFileAtomically(g_config.receiptPath, encoded.data(),
		written))
	{
		g_config.failed = true;
		return false;
	}
	g_config.finalized = true;
	std::printf("LOCKSTEP_V2_VALIDATION_PASS peer=%u pid=%lu frame=%u crc=%08X\n",
		g_config.localSlot, static_cast<unsigned long>(GetCurrentProcessId()),
		receipt.finalFrame,
		receipt.checkpoints[receipt.checkpointCount - 1U].crc);
	std::fflush(stdout);
	return true;
}

} // namespace rts

#endif // defined(_WIN64)
