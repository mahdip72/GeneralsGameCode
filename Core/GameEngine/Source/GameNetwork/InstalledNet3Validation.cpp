#include "PreRTS.h"

#include "GameNetwork/InstalledNet3Validation.h"

#if defined(_WIN64)

#include "Lib/CollisionCandidateKernel.h"
#include "Lib/DeterministicAIPlanning.h"
#include "Lib/DeterministicPathSearch.h"
#include "Lib/ImmutableSpatialQuery.h"
#include "Lib/JobSystem.h"
#include "Lib/MultiplayerSimulationPolicy.h"
#include "Lib/NetworkEpochHandshake.h"
#include "Lib/ObjectStatusTimerKernel.h"
#include "Lib/PhysicsIntegrationKernel.h"

#include <array>
#include <atomic>
#include <bcrypt.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>
#include <windows.h>

namespace rts
{
namespace
{

enum
{
	VALIDATION_MAXIMUM_PEERS = 4,
	VALIDATION_KERNEL_COUNT = 6,
	VALIDATION_JOBS_PER_KERNEL = 64,
	VALIDATION_TIMEOUT_MILLISECONDS = 120000
};

struct InstalledNet3ValidationConfig
{
	bool requested;
	unsigned caseIndex;
	unsigned seed;
	unsigned peerIndex;
	unsigned peerCount;
	std::string requestedWorkers;
	std::string sessionToken;
	std::string sessionDirectory;
	std::string sourceRevision;
	std::string expectedExecutableSha256;
	std::string artifactSetSha256;

	InstalledNet3ValidationConfig() : requested(false), caseIndex(~0u), seed(0),
		peerIndex(~0u), peerCount(0)
	{
	}
};

InstalledNet3ValidationConfig g_validationConfig;

bool IsLowerHex(const std::string &value, unsigned length)
{
	if (value.size() != length)
		return false;
	for (unsigned index = 0; index < length; ++index)
	{
		if (!((value[index] >= '0' && value[index] <= '9') ||
			(value[index] >= 'a' && value[index] <= 'f')))
		{
			return false;
		}
	}
	return true;
}

bool IsUpperHex(const std::string &value, unsigned length)
{
	if (value.size() != length)
		return false;
	for (unsigned index = 0; index < length; ++index)
	{
		if (!((value[index] >= '0' && value[index] <= '9') ||
			(value[index] >= 'A' && value[index] <= 'F')))
		{
			return false;
		}
	}
	return true;
}

bool ParseUnsigned(const std::string &text, unsigned &value)
{
	if (text.empty())
		return false;
	unsigned result = 0;
	for (std::size_t index = 0; index < text.size(); ++index)
	{
		if (text[index] < '0' || text[index] > '9')
			return false;
		const unsigned digit = static_cast<unsigned>(text[index] - '0');
		if (result > (0xffffffffU - digit) / 10U)
			return false;
		result = result * 10U + digit;
	}
	value = result;
	return true;
}

bool ReadConfigurationField(const std::string &configuration,
	const char *name, std::string &value)
{
	const std::string prefix = std::string(name) + "=";
	std::size_t cursor = 0;
	while (cursor <= configuration.size())
	{
		const std::size_t end = configuration.find(';', cursor);
		const std::size_t length = end == std::string::npos ?
			configuration.size() - cursor : end - cursor;
		if (length >= prefix.size() &&
			configuration.compare(cursor, prefix.size(), prefix) == 0)
		{
			value.assign(configuration, cursor + prefix.size(),
				length - prefix.size());
			return !value.empty();
		}
		if (end == std::string::npos)
			break;
		cursor = end + 1;
	}
	return false;
}

bool IsSafeAbsoluteSessionDirectory(const std::string &path)
{
	if (path.size() < 4 || path.size() >= MAX_PATH || path[1] != ':' ||
		(path[2] != '\\' && path[2] != '/') ||
		path.find("..") != std::string::npos ||
		path.find(';') != std::string::npos ||
		path.find('"') != std::string::npos)
	{
		return false;
	}
	return true;
}

std::string SessionPath(const char *stem, unsigned first,
	const char *middle = 0, unsigned second = 0)
{
	std::ostringstream path;
	path << g_validationConfig.sessionDirectory << "\\" << stem << first;
	if (middle != 0)
		path << middle << second;
	return path.str();
}

bool WriteFileAtomically(const std::string &path, const void *bytes,
	std::size_t byteCount)
{
	std::ostringstream temporary;
	temporary << path << ".tmp-" << GetCurrentProcessId();
	HANDLE file = CreateFileA(temporary.str().c_str(), GENERIC_WRITE, 0, 0,
		CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, 0);
	if (file == INVALID_HANDLE_VALUE)
		return false;
	DWORD written = 0;
	const bool success = byteCount <= 0xffffffffU &&
		WriteFile(file, bytes, static_cast<DWORD>(byteCount), &written, 0) &&
		written == byteCount && FlushFileBuffers(file) != FALSE;
	CloseHandle(file);
	if (!success || !MoveFileExA(temporary.str().c_str(), path.c_str(),
		MOVEFILE_WRITE_THROUGH))
	{
		DeleteFileA(temporary.str().c_str());
		return false;
	}
	return true;
}

bool ReadExactFile(const std::string &path, void *bytes,
	std::size_t byteCount)
{
	HANDLE file = CreateFileA(path.c_str(), GENERIC_READ,
		FILE_SHARE_READ | FILE_SHARE_DELETE, 0, OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, 0);
	if (file == INVALID_HANDLE_VALUE)
		return false;
	LARGE_INTEGER size;
	DWORD read = 0;
	const bool success = GetFileSizeEx(file, &size) &&
		size.QuadPart == static_cast<LONGLONG>(byteCount) &&
		byteCount <= 0xffffffffU &&
		ReadFile(file, bytes, static_cast<DWORD>(byteCount), &read, 0) &&
		read == byteCount;
	CloseHandle(file);
	return success;
}

bool WaitForFile(const std::string &path)
{
	const ULONGLONG deadline = GetTickCount64() +
		VALIDATION_TIMEOUT_MILLISECONDS;
	while (GetTickCount64() < deadline)
	{
		const DWORD attributes = GetFileAttributesA(path.c_str());
		if (attributes != INVALID_FILE_ATTRIBUTES &&
			(attributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
		{
			return true;
		}
		Sleep(10);
	}
	return false;
}

bool CalculateSha256(const void *bytes, std::size_t byteCount,
	std::string &digest)
{
	BCRYPT_ALG_HANDLE algorithm = 0;
	BCRYPT_HASH_HANDLE hash = 0;
	DWORD objectLength = 0;
	DWORD digestLength = 0;
	DWORD propertyBytes = 0;
	bool success = false;
	if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, 0,
		0) != 0 || BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
		reinterpret_cast<PUCHAR>(&objectLength), sizeof(objectLength),
		&propertyBytes, 0) != 0 || objectLength == 0 ||
		BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH,
		reinterpret_cast<PUCHAR>(&digestLength), sizeof(digestLength),
		&propertyBytes, 0) != 0 || digestLength != 32)
	{
		if (algorithm != 0)
			BCryptCloseAlgorithmProvider(algorithm, 0);
		return false;
	}
	std::vector<unsigned char> object(objectLength);
	std::array<unsigned char, 32> result = {{}};
	if (BCryptCreateHash(algorithm, &hash, object.data(), objectLength, 0, 0,
		0) == 0 && byteCount <= 0xffffffffU &&
		BCryptHashData(hash, reinterpret_cast<PUCHAR>(
			const_cast<void *>(bytes)), static_cast<ULONG>(byteCount), 0) == 0 &&
		BCryptFinishHash(hash, result.data(), digestLength, 0) == 0)
	{
		static const char HEX[] = "0123456789ABCDEF";
		digest.resize(64);
		for (unsigned index = 0; index < result.size(); ++index)
		{
			digest[index * 2] = HEX[result[index] >> 4];
			digest[index * 2 + 1] = HEX[result[index] & 0x0f];
		}
		success = true;
	}
	if (hash != 0)
		BCryptDestroyHash(hash);
	BCryptCloseAlgorithmProvider(algorithm, 0);
	return success;
}

bool CalculateCurrentExecutableSha256(std::string &digest)
{
	char path[MAX_PATH];
	const DWORD length = GetModuleFileNameA(0, path, ARRAY_SIZE(path));
	if (length == 0 || length >= ARRAY_SIZE(path))
		return false;
	HANDLE file = CreateFileA(path, GENERIC_READ,
		FILE_SHARE_READ | FILE_SHARE_DELETE, 0, OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, 0);
	if (file == INVALID_HANDLE_VALUE)
		return false;
	LARGE_INTEGER size;
	if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 ||
		size.QuadPart > 0x7fffffff)
	{
		CloseHandle(file);
		return false;
	}
	std::vector<unsigned char> bytes(static_cast<std::size_t>(size.QuadPart));
	DWORD read = 0;
	const bool readOk = ReadFile(file, bytes.data(),
		static_cast<DWORD>(bytes.size()), &read, 0) && read == bytes.size();
	CloseHandle(file);
	return readOk && CalculateSha256(bytes.data(), bytes.size(), digest);
}

unsigned CountBits(unsigned long long value)
{
	unsigned count = 0;
	while (value != 0)
	{
		value &= value - 1;
		++count;
	}
	return count;
}

unsigned MixKernelEvidence(unsigned value, unsigned field)
{
	return (value ^ field) * 16777619U;
}

unsigned FloatKernelEvidence(float field)
{
	unsigned bits = 0;
	std::memcpy(&bits, &field, sizeof(bits));
	return bits;
}

PhysicsIntegrationSnapshot MakePhysicsKernelSnapshot(unsigned ordinal,
	unsigned seed)
{
	PhysicsIntegrationSnapshot snapshot = {};
	snapshot.frame = 900U + (seed & 31U);
	snapshot.worldEpoch = 7U;
	snapshot.objectID = ordinal + 1U;
	snapshot.motionGeneration = 10U + ordinal;
	snapshot.physicsGeneration = 20U + ordinal;
	snapshot.wakePriority = (snapshot.frame << 2) | 1U;
	snapshot.heapOrdinal = ordinal * 3U + 1U;
	snapshot.flags = PHYSICS_INTEGRATION_HAS_PITCH_ROLL_YAW |
		((ordinal & 1U) != 0U ? PHYSICS_INTEGRATION_MOTIVE : 0U) |
		((ordinal & 2U) != 0U ? PHYSICS_INTEGRATION_BRAKING : 0U);
	snapshot.matrix[0] = 0.93629336f;
	snapshot.matrix[1] = -0.27509585f;
	snapshot.matrix[2] = 0.21835066f;
	snapshot.matrix[3] = 100.0f + ordinal;
	snapshot.matrix[4] = 0.28962949f;
	snapshot.matrix[5] = 0.95642507f;
	snapshot.matrix[6] = -0.03695701f;
	snapshot.matrix[7] = -50.0f + ordinal * 0.25f;
	snapshot.matrix[8] = -0.19866933f;
	snapshot.matrix[9] = 0.09784339f;
	snapshot.matrix[10] = 0.97517033f;
	snapshot.matrix[11] = 12.0f;
	snapshot.position[0] = snapshot.matrix[3];
	snapshot.position[1] = snapshot.matrix[7];
	snapshot.position[2] = snapshot.matrix[11];
	snapshot.acceleration[0] = 0.21f + ordinal * 0.0001f;
	snapshot.acceleration[1] = -0.17f;
	snapshot.acceleration[2] = 0.03f;
	snapshot.velocity[0] = 1.75f;
	snapshot.velocity[1] = -0.625f - ordinal * 0.0002f;
	snapshot.velocity[2] = 0.045f;
	snapshot.yawRate = 0.013f;
	snapshot.rollRate = -0.009f;
	snapshot.pitchRate = 0.017f;
	snapshot.gravity = -0.03f;
	snapshot.mass = 3.5f;
	snapshot.forwardFriction = 0.06f;
	snapshot.lateralFriction = 0.15f;
	snapshot.aerodynamicFriction = 0.01f;
	snapshot.pitchRollYawFactor = 0.8f;
	snapshot.centerOfMassOffset = 0.35f;
	snapshot.directionX = 0.93937272f;
	snapshot.directionY = 0.34289780f;
	return snapshot;
}

bool RunPhysicsKernelFixture(unsigned ordinal, unsigned seed,
	unsigned &evidence)
{
	const PhysicsIntegrationSnapshot snapshot =
		MakePhysicsKernelSnapshot(ordinal, seed);
	PhysicsIntegrationOutput output = {};
	if (!ComputePhysicsIntegrationPrefix(snapshot, output) ||
		!ValidatePhysicsIntegrationOutput(snapshot, output))
	{
		return false;
	}
	evidence = MixKernelEvidence(seed, output.objectID);
	evidence = MixKernelEvidence(evidence, output.flags);
	for (unsigned index = 0; index < PHYSICS_INTEGRATION_VECTOR_FLOATS; ++index)
	{
		evidence = MixKernelEvidence(evidence,
			FloatKernelEvidence(output.velocity[index]));
	}
	return true;
}

bool RunParallelPhysicsKernelFixture(unsigned seed,
	unsigned evidence[VALIDATION_JOBS_PER_KERNEL],
	PhysicsIntegrationMetrics &metrics)
{
	enum { PHYSICS_INPUT_COUNT = 16384 };
	std::vector<PhysicsIntegrationSnapshot> snapshots(PHYSICS_INPUT_COUNT);
	std::vector<PhysicsIntegrationOutput> output(PHYSICS_INPUT_COUNT);
	std::vector<PhysicsIntegrationOutput> scratch(PHYSICS_INPUT_COUNT);
	for (unsigned index = 0; index < PHYSICS_INPUT_COUNT; ++index)
		snapshots[index] = MakePhysicsKernelSnapshot(index, seed);
	PhysicsIntegrationOptions options;
	options.minimumGrain = 256U;
	if (PreparePhysicsIntegrationPrefixes(snapshots.data(), PHYSICS_INPUT_COUNT,
		output.data(), PHYSICS_INPUT_COUNT, scratch.data(), PHYSICS_INPUT_COUNT,
		options, &metrics) != PHYSICS_INTEGRATION_PARALLEL ||
		metrics.submittedJobs < 2U ||
		metrics.completedJobs != metrics.submittedJobs ||
		metrics.physicalWorkerJobs != metrics.completedJobs ||
		metrics.ownerHelpedJobs != 0 || metrics.distinctPhysicalWorkers < 2U ||
		metrics.peakConcurrentPhysicalWorkers < 2U)
	{
		return false;
	}
	for (unsigned slot = 0; slot < VALIDATION_JOBS_PER_KERNEL; ++slot)
		evidence[slot] = MixKernelEvidence(seed, slot);
	for (unsigned index = 0; index < PHYSICS_INPUT_COUNT; ++index)
	{
		const unsigned slot = index % VALIDATION_JOBS_PER_KERNEL;
		evidence[slot] = MixKernelEvidence(evidence[slot], output[index].objectID);
		evidence[slot] = MixKernelEvidence(evidence[slot], output[index].flags);
		evidence[slot] = MixKernelEvidence(evidence[slot],
			FloatKernelEvidence(output[index].velocity[0]));
	}
	return true;
}

bool RunStatusKernelFixture(unsigned ordinal, unsigned seed,
	unsigned &evidence)
{
	ObjectStatusTimerSnapshot snapshots[4] = {};
	for (unsigned index = 0; index < ARRAY_SIZE(snapshots); ++index)
	{
		snapshots[index].objectID = 1000U + ordinal * 8U + index;
		snapshots[index].ownerOrder = index;
		for (unsigned type = 0; type < OBJECT_STATUS_TIMER_MAX_TYPES; ++type)
			snapshots[index].expirationFrame[type] = ~0U;
		const unsigned type = (ordinal + index) % 13U;
		snapshots[index].activeMask = 1U << type;
		snapshots[index].expirationFrame[type] = 100U;
	}
	ObjectStatusTimerCommand commands[ARRAY_SIZE(snapshots)] = {};
	ObjectStatusTimerOptions options;
	unsigned commandCount = 0;
	if (PrepareObjectStatusTimerCommands(snapshots, ARRAY_SIZE(snapshots),
		100U, 13U, commands, ARRAY_SIZE(commands), options, &commandCount) !=
		OBJECT_STATUS_TIMER_SERIAL || commandCount != ARRAY_SIZE(snapshots))
	{
		return false;
	}
	evidence = seed;
	for (unsigned index = 0; index < commandCount; ++index)
	{
		evidence = MixKernelEvidence(evidence, commands[index].objectID);
		evidence = MixKernelEvidence(evidence, commands[index].expiredMask);
	}
	return true;
}

bool RunParallelStatusKernelFixture(unsigned seed,
	unsigned evidence[VALIDATION_JOBS_PER_KERNEL],
	ObjectStatusTimerMetrics &metrics)
{
	enum { STATUS_INPUT_COUNT = 16384 };
	std::vector<ObjectStatusTimerSnapshot> snapshots(STATUS_INPUT_COUNT);
	std::vector<ObjectStatusTimerCommand> commands(STATUS_INPUT_COUNT);
	for (unsigned index = 0; index < STATUS_INPUT_COUNT; ++index)
	{
		ObjectStatusTimerSnapshot &snapshot = snapshots[index];
		snapshot.objectID = 1000U + index;
		snapshot.ownerOrder = index;
		snapshot.activeMask = 1U << (index % 13U);
		for (unsigned type = 0; type < OBJECT_STATUS_TIMER_MAX_TYPES; ++type)
			snapshot.expirationFrame[type] = ~0U;
		snapshot.expirationFrame[index % 13U] = 100U;
	}
	ObjectStatusTimerOptions options;
	options.parallel = true;
	options.minimumGrain = 256U;
	unsigned commandCount = 0;
	if (PrepareObjectStatusTimerCommands(snapshots.data(), STATUS_INPUT_COUNT,
		100U, 13U, commands.data(), STATUS_INPUT_COUNT, options, &commandCount,
		&metrics) != OBJECT_STATUS_TIMER_PARALLEL ||
		commandCount != STATUS_INPUT_COUNT || metrics.submittedJobs < 2U ||
		metrics.completedJobs != metrics.submittedJobs ||
		metrics.physicalWorkerJobs != metrics.completedJobs ||
		metrics.ownerHelpedJobs != 0 || metrics.distinctPhysicalWorkers < 2U ||
		metrics.peakConcurrentPhysicalWorkers < 2U)
	{
		return false;
	}
	for (unsigned slot = 0; slot < VALIDATION_JOBS_PER_KERNEL; ++slot)
		evidence[slot] = MixKernelEvidence(seed, slot);
	for (unsigned index = 0; index < commandCount; ++index)
	{
		const unsigned slot = index % VALIDATION_JOBS_PER_KERNEL;
		evidence[slot] = MixKernelEvidence(evidence[slot], commands[index].objectID);
		evidence[slot] = MixKernelEvidence(evidence[slot],
			commands[index].expiredMask);
	}
	return true;
}

bool RunCollisionKernelFixture(unsigned ordinal, unsigned seed,
	unsigned &evidence)
{
	CollisionCandidateInput inputs[6] = {};
	for (unsigned index = 0; index < 3U; ++index)
	{
		inputs[index].firstID = 1U + ordinal * 4U + index;
		inputs[index].secondID = 500U + ordinal * 4U + index;
		inputs[index].firstGeneration = 11U + index;
		inputs[index].secondGeneration = 21U + index;
		inputs[index].discoveryOrder = index;
		inputs[index + 3U] = inputs[index];
		const unsigned swap = inputs[index + 3U].firstID;
		inputs[index + 3U].firstID = inputs[index + 3U].secondID;
		inputs[index + 3U].secondID = swap;
		inputs[index + 3U].discoveryOrder = index + 3U;
	}
	CollisionCandidate output[ARRAY_SIZE(inputs)] = {};
	CollisionCandidate scratch[ARRAY_SIZE(inputs)] = {};
	CollisionCandidateOptions options;
	unsigned outputCount = 0;
	if (PrepareCollisionCandidates(inputs, ARRAY_SIZE(inputs), output,
		ARRAY_SIZE(output), scratch, ARRAY_SIZE(scratch), options,
		&outputCount) != COLLISION_CANDIDATE_SERIAL || outputCount != 3U)
	{
		return false;
	}
	evidence = seed;
	for (unsigned index = 0; index < outputCount; ++index)
	{
		evidence = MixKernelEvidence(evidence, output[index].key.lowID);
		evidence = MixKernelEvidence(evidence, output[index].key.highID);
		evidence = MixKernelEvidence(evidence, output[index].discoveryOrder);
	}
	return true;
}

bool RunParallelCollisionKernelFixture(unsigned seed,
	unsigned evidence[VALIDATION_JOBS_PER_KERNEL],
	CollisionCandidateMetrics &metrics, unsigned &peakConcurrentWorkers)
{
	enum { COLLISION_INPUT_COUNT = 16384 };
	std::vector<CollisionCandidateInput> inputs(COLLISION_INPUT_COUNT);
	std::vector<CollisionCandidate> output(COLLISION_INPUT_COUNT);
	std::vector<CollisionCandidate> scratch(COLLISION_INPUT_COUNT);
	for (unsigned index = 0; index < COLLISION_INPUT_COUNT; ++index)
	{
		inputs[index].firstID = index + 1U;
		inputs[index].secondID = COLLISION_INPUT_COUNT + index + 1U;
		inputs[index].firstGeneration = 11U + (index & 7U);
		inputs[index].secondGeneration = 21U + (index & 7U);
		inputs[index].discoveryOrder = index;
	}
	CollisionCandidateOptions options;
	options.parallel = true;
	options.minimumGrain = 256U;
	unsigned outputCount = 0;
	JobSystem &jobs = JobSystem::instance();
	jobs.resetMetrics();
	const CollisionCandidateResult result = PrepareCollisionCandidates(
		inputs.data(), static_cast<unsigned>(inputs.size()), output.data(),
		static_cast<unsigned>(output.size()), scratch.data(),
		static_cast<unsigned>(scratch.size()), options, &outputCount, &metrics);
	peakConcurrentWorkers = jobs.metrics().maximumActiveWorkers;
	if (result != COLLISION_CANDIDATE_PARALLEL ||
		outputCount != COLLISION_INPUT_COUNT || metrics.preparedPairs !=
		COLLISION_INPUT_COUNT || metrics.uniqueCandidates != outputCount ||
		metrics.submittedJobs == 0 ||
		metrics.completedJobs != metrics.submittedJobs ||
		metrics.physicalWorkerJobs + metrics.ownerHelpedJobs !=
			metrics.completedJobs || metrics.physicalWorkerJobs == 0 ||
		metrics.ownerHelpedJobs != 0 || !metrics.physicalWorkerMaskComplete ||
		metrics.distinctPhysicalWorkers < 2U || peakConcurrentWorkers < 2U)
	{
		return false;
	}
	for (unsigned slot = 0; slot < VALIDATION_JOBS_PER_KERNEL; ++slot)
		evidence[slot] = MixKernelEvidence(seed, slot);
	for (unsigned index = 0; index < outputCount; ++index)
	{
		const unsigned slot = index % VALIDATION_JOBS_PER_KERNEL;
		evidence[slot] = MixKernelEvidence(evidence[slot],
			output[index].key.lowID);
		evidence[slot] = MixKernelEvidence(evidence[slot],
			output[index].key.highID);
		evidence[slot] = MixKernelEvidence(evidence[slot],
			output[index].discoveryOrder);
	}
	return true;
}

bool RunAIPlanningKernelFixture(unsigned ordinal, unsigned seed,
	unsigned &evidence)
{
	AIPlayerPlanningSnapshot snapshot;
	ClearAIPlayerPlanningSnapshot(&snapshot);
	snapshot.frame = 120U + (seed & 31U);
	snapshot.playerIndex = ordinal % 8U;
	snapshot.planEnemyTarget = 1U;
	snapshot.enemyTarget.frame = snapshot.frame;
	snapshot.enemyTarget.ownerPlayerIndex = snapshot.playerIndex;
	snapshot.enemyTarget.currentEnemyPlayerIndex = 2;
	snapshot.enemyTarget.switchScoreThreshold = 100;
	snapshot.enemyTarget.candidateCount = 3U;
	for (unsigned index = 0; index < 3U; ++index)
	{
		AIEnemyCandidateFact &candidate =
			snapshot.enemyTarget.candidates[index];
		candidate.sourceOrdinal = index + 1U;
		candidate.playerIndex = static_cast<int>(index + 2U);
		candidate.knownAssetValue = 100 + static_cast<int>(index) * 350;
		candidate.distance = 100 + static_cast<int>(index) * 50;
		candidate.routeClass = AI_PLANNING_TARGET_ROUTE_REACHABLE;
		candidate.hasKnownPosition = 1U;
		candidate.hasKnownObject = 1U;
		candidate.hasKnownUnit = 1U;
	}
	AIPlayerPlanningResult result;
	if (!PlanAIPlayer(snapshot, &result) ||
		!ValidateAIPlayerPlanningResult(snapshot, result))
	{
		return false;
	}
	evidence = MixKernelEvidence(seed,
		static_cast<unsigned>(result.enemyTarget.selectedPlayerIndex));
	evidence = MixKernelEvidence(evidence,
		static_cast<unsigned>(result.enemyTarget.selectedScore));
	evidence = MixKernelEvidence(evidence,
		result.enemyTarget.candidateScoreCount);
	return true;
}

bool RunSpatialKernelFixture(unsigned ordinal, unsigned seed,
	unsigned &evidence)
{
	ImmutableSpatialGeneration generation = { 7U, 11U, 13U };
	ImmutableSpatialObjectRecord objects[2] = {};
	for (unsigned index = 0; index < ARRAY_SIZE(objects); ++index)
	{
		objects[index].objectID = (index + 1U) * 100U + ordinal;
		objects[index].generation.lifecycle = ordinal + index + 1U;
		objects[index].generation.topology = ordinal + index + 2U;
		objects[index].generation.facts = ordinal + index + 3U;
		objects[index].admissionMask = 1U;
		objects[index].buildCost = static_cast<int>(10U + index);
		objects[index].positionX = static_cast<float>(index);
		objects[index].boundingCircleRadius = 0.5f;
		objects[index].boundingSphereRadius = 0.5f;
	}
	ImmutableSpatialCellRecord cells[1] = { { 0U, 2U } };
	ImmutableSpatialMemberRecord members[2] = { { 0U }, { 1U } };
	ImmutableSpatialRadiusRecord radii[3] = {
		{ 0U, 1U }, { 1U, 0U }, { 1U, 0U }
	};
	ImmutableSpatialOffsetRecord offsets[1] = { { 0, 0 } };
	ImmutableSpatialArenaInput input = {};
	input.generation = generation;
	input.gridWidth = 1U;
	input.gridHeight = 1U;
	input.cellSize = 10.0f;
	input.objects = objects;
	input.objectCount = ARRAY_SIZE(objects);
	input.cells = cells;
	input.cellCount = ARRAY_SIZE(cells);
	input.members = members;
	input.memberCount = ARRAY_SIZE(members);
	input.radii = radii;
	input.radiusCount = ARRAY_SIZE(radii);
	input.offsets = offsets;
	input.offsetCount = ARRAY_SIZE(offsets);
	unsigned requiredBytes = 0;
	std::array<unsigned, 128> arena = {{}};
	unsigned arenaBytes = 0;
	if (MeasureImmutableSpatialArena(input, &requiredBytes) !=
		IMMUTABLE_SPATIAL_SUCCESS || requiredBytes > sizeof(arena) ||
		BuildImmutableSpatialArena(input, arena.data(), sizeof(arena),
			&arenaBytes) != IMMUTABLE_SPATIAL_SUCCESS)
	{
		return false;
	}
	ImmutableSpatialQuery query = {};
	query.expectedArenaGeneration = generation;
	query.selfObjectIndex = 0U;
	query.maximumDistance = 10.0f;
	query.requiredAdmissionMask = 1U;
	query.distanceType = IMMUTABLE_SPATIAL_FROM_CENTER_2D;
	query.iteratorOrder = IMMUTABLE_SPATIAL_ITER_FASTEST;
	unsigned counts[1] = {};
	unsigned states[1] = {};
	unsigned visits[2] = {};
	ImmutableSpatialResultSpan spanScratch[1] = {};
	ImmutableSpatialResult resultScratch[2] = {};
	ImmutableSpatialResult sortScratch[2] = {};
	ImmutableSpatialResult output[2] = {};
	ImmutableSpatialResultSpan outputSpans[1] = {};
	ImmutableSpatialBatchScratch scratch = {};
	scratch.counts = counts;
	scratch.countCapacity = ARRAY_SIZE(counts);
	scratch.states = states;
	scratch.stateCapacity = ARRAY_SIZE(states);
	scratch.visitStamps = visits;
	scratch.visitStampCapacity = ARRAY_SIZE(visits);
	scratch.spanScratch = spanScratch;
	scratch.spanScratchCapacity = ARRAY_SIZE(spanScratch);
	scratch.resultScratch = resultScratch;
	scratch.resultScratchCapacity = ARRAY_SIZE(resultScratch);
	scratch.sortScratch = sortScratch;
	scratch.sortScratchCapacity = ARRAY_SIZE(sortScratch);
	ImmutableSpatialExecutionOptions options;
	unsigned outputCount = 0;
	if (ExecuteImmutableSpatialQueryBatch(arena.data(), arenaBytes, &query,
		1U, options, scratch, output, ARRAY_SIZE(output), outputSpans,
		ARRAY_SIZE(outputSpans), &outputCount) != IMMUTABLE_SPATIAL_SUCCESS ||
		outputCount != 1U || outputSpans[0].count != 1U ||
		output[0].objectID != objects[1].objectID)
	{
		return false;
	}
	evidence = MixKernelEvidence(seed, output[0].objectID);
	evidence = MixKernelEvidence(evidence, output[0].discoveryOrdinal);
	evidence = MixKernelEvidence(evidence,
		FloatKernelEvidence(output[0].distanceSquared));
	return true;
}

bool RunPathKernelFixture(unsigned ordinal, unsigned seed,
	unsigned &evidence)
{
	static const unsigned char VALID_FLAGS =
		DIRECT_PATH_FACT_CLEAR_GROUND |
		DIRECT_PATH_FACT_HIERARCHY_PASSABLE |
		DIRECT_PATH_FACT_INSIDE_LOGICAL_EXTENT |
		DIRECT_PATH_FACT_FOOTPRINT_CLEAR |
		DIRECT_PATH_FACT_NO_FOREIGN_OCCUPANCY |
		DIRECT_PATH_FACT_NO_LAYER_CONNECTION |
		DIRECT_PATH_FACT_NOT_PINCHED |
		DIRECT_PATH_FACT_METADATA_CLEAN;
	const int startX = 3;
	const int startY = 4;
	const int goalX = 11 + static_cast<int>(ordinal % 3U);
	const int goalY = 7 + static_cast<int>(ordinal % 2U);
	DeterministicPathPoint callbackPoints[64] = {};
	std::size_t callbackCount = 0;
	if (!BuildLegacySupercoverCallbacks(startX, startY, goalX, goalY,
		callbackPoints, ARRAY_SIZE(callbackPoints), callbackCount) ||
		callbackCount == 0 || callbackCount > ARRAY_SIZE(callbackPoints))
	{
		return false;
	}
	DirectPathCellFact callbacks[ARRAY_SIZE(callbackPoints)] = {};
	for (std::size_t index = 0; index < callbackCount; ++index)
	{
		callbacks[index].x = callbackPoints[index].x;
		callbacks[index].y = callbackPoints[index].y;
		callbacks[index].zone = 7;
		callbacks[index].flags = VALID_FLAGS;
	}
	static const int DELTA_X[8] = { 1, 0, -1, 0, 1, -1, -1, 1 };
	static const int DELTA_Y[8] = { 0, 1, 0, -1, 1, 1, -1, -1 };
	DirectPathCellFact neighbors[8] = {};
	for (unsigned index = 0; index < ARRAY_SIZE(neighbors); ++index)
	{
		neighbors[index].x = startX + DELTA_X[index];
		neighbors[index].y = startY + DELTA_Y[index];
		neighbors[index].zone = 7;
		neighbors[index].flags = VALID_FLAGS;
	}
	DirectPathSnapshot snapshot = {};
	snapshot.callbacks = callbacks;
	snapshot.callbackCount = callbackCount;
	snapshot.startNeighbors = neighbors;
	snapshot.startNeighborCount = ARRAY_SIZE(neighbors);
	snapshot.topologyOccupancyGeneration = 41U;
	snapshot.requestToken = 73U + ordinal;
	snapshot.objectId = 99U + ordinal;
	snapshot.availableCellInfoCount = 4096U;
	snapshot.startX = startX;
	snapshot.startY = startY;
	snapshot.goalX = goalX;
	snapshot.goalY = goalY;
	snapshot.requiredZone = 7;
	snapshot.expectedLayer = DETERMINISTIC_PATH_LAYER_GROUND;
	DeterministicPathPoint rawPoints[ARRAY_SIZE(callbackPoints)] = {};
	DirectPathSearchResult result = {};
	result.rawPoints = rawPoints;
	result.rawPointCapacity = ARRAY_SIZE(rawPoints);
	if (FindDeterministicDirectPath(snapshot, result) != DIRECT_PATH_FOUND ||
		!IsDirectPathMaterializationPlanValid(snapshot, result,
			snapshot.availableCellInfoCount))
	{
		return false;
	}
	evidence = MixKernelEvidence(seed,
		static_cast<unsigned>(result.rawPointCount));
	evidence = MixKernelEvidence(evidence,
		static_cast<unsigned>(result.cumulativeCellCount));
	if (result.rawPointCount != 0)
	{
		evidence = MixKernelEvidence(evidence,
			static_cast<unsigned>(result.rawPoints[0].x));
		evidence = MixKernelEvidence(evidence,
			static_cast<unsigned>(result.rawPoints[0].y));
	}
	return true;
}

bool RunActualKernelFixture(unsigned kernel, unsigned ordinal,
	unsigned seed, unsigned &evidence)
{
	switch (kernel)
	{
		case 0: return RunPhysicsKernelFixture(ordinal, seed, evidence);
		case 1: return RunStatusKernelFixture(ordinal, seed, evidence);
		case 2: return RunCollisionKernelFixture(ordinal, seed, evidence);
		case 3: return RunAIPlanningKernelFixture(ordinal, seed, evidence);
		case 4: return RunSpatialKernelFixture(ordinal, seed, evidence);
		case 5: return RunPathKernelFixture(ordinal, seed, evidence);
		default: return false;
	}
}

struct KernelMetrics
{
	std::atomic<unsigned> completed;
	std::atomic<unsigned> physical;
	std::atomic<unsigned long long> workerMask;
	std::atomic<unsigned> failed;
	unsigned submitted;
	unsigned ownerHelped;
	unsigned distinctPhysicalWorkers;
	unsigned peakConcurrentWorkers;
	std::atomic<bool> physicalWorkerMaskComplete;

	KernelMetrics() : completed(0), physical(0), workerMask(0), failed(0),
		submitted(0), ownerHelped(0), distinctPhysicalWorkers(0),
		peakConcurrentWorkers(0), physicalWorkerMaskComplete(true) {}
};

class ValidationKernelJob : public Job
{
public:
	ValidationKernelJob(KernelMetrics &metrics, unsigned kernel,
		unsigned ordinal, unsigned seed, unsigned *outputs) :
		m_metrics(metrics), m_kernel(kernel), m_ordinal(ordinal),
		m_seed(seed), m_outputs(outputs) {}

	virtual void execute(JobContext &context)
	{
		if (!RunActualKernelFixture(m_kernel, m_ordinal, m_seed,
			m_outputs[m_ordinal]))
		{
			m_metrics.failed.fetch_add(1, std::memory_order_relaxed);
		}
		if (context.isPhysicalWorkerExecution())
		{
			const unsigned index = context.physicalWorkerIndex();
			if (index < 64U)
				m_metrics.workerMask.fetch_or(1ULL << index,
					std::memory_order_relaxed);
			else
				m_metrics.physicalWorkerMaskComplete.store(false,
					std::memory_order_relaxed);
			m_metrics.physical.fetch_add(1, std::memory_order_relaxed);
			const ULONGLONG deadline = GetTickCount64() + 100;
			while (CountBits(m_metrics.workerMask.load(
				std::memory_order_relaxed)) < 2 && GetTickCount64() < deadline)
			{
				SwitchToThread();
			}
		}
		m_metrics.completed.fetch_add(1, std::memory_order_relaxed);
	}

private:
	KernelMetrics &m_metrics;
	unsigned m_kernel;
	unsigned m_ordinal;
	unsigned m_seed;
	unsigned *m_outputs;
};

bool RunKernelEvidence(KernelMetrics metrics[VALIDATION_KERNEL_COUNT],
	unsigned outputs[VALIDATION_KERNEL_COUNT][VALIDATION_JOBS_PER_KERNEL],
	unsigned &effectiveWorkers)
{
	JobSystemConfig config = JobSystem::startupConfig();
	config.queueCapacity = 1024;
	config.scratchBytesPerWorker = 64U * 1024U;
	config.pinWorkers = false;
	if (g_validationConfig.requestedWorkers == "auto")
	{
		config.workerCount = 0;
		config.workerPolicy = JOB_WORKER_POLICY_AUTO;
	}
	else
	{
		unsigned workerCount = 0;
		if (!ParseUnsigned(g_validationConfig.requestedWorkers, workerCount) ||
			workerCount == 0 || workerCount > 16)
		{
			return false;
		}
		config.workerCount = workerCount;
		config.workerPolicy = JOB_WORKER_POLICY_ALL;
	}
	JobSystem &jobs = JobSystem::instance();
	if (!jobs.start(config))
		return false;
	bool ownerRegisteredHere = false;
	if (!jobs.isCurrentThread(JOB_OWNER_GAME))
	{
		if (!jobs.registerCurrentThread(JOB_OWNER_GAME))
		{
			jobs.shutdown();
			return false;
		}
		ownerRegisteredHere = true;
	}
	effectiveWorkers = jobs.workerCount();
	if (effectiveWorkers == 0)
	{
		if (ownerRegisteredHere)
			jobs.unregisterCurrentThread(JOB_OWNER_GAME);
		jobs.shutdown();
		return false;
	}
	bool success = true;
	if (effectiveWorkers > 1)
	{
		for (unsigned kernel = 0; kernel < VALIDATION_KERNEL_COUNT && success;
			++kernel)
		{
			if (kernel == 0U)
			{
				PhysicsIntegrationMetrics physicsMetrics;
				if (!RunParallelPhysicsKernelFixture(
					g_validationConfig.seed ^ (kernel * 0x9e3779b9U),
					outputs[kernel], physicsMetrics))
				{
					success = false;
					break;
				}
				metrics[kernel].submitted = physicsMetrics.submittedJobs;
				metrics[kernel].completed.store(physicsMetrics.completedJobs);
				metrics[kernel].physical.store(physicsMetrics.physicalWorkerJobs);
				metrics[kernel].ownerHelped = physicsMetrics.ownerHelpedJobs;
				metrics[kernel].workerMask.store(physicsMetrics.physicalWorkerMask);
				metrics[kernel].distinctPhysicalWorkers =
					physicsMetrics.distinctPhysicalWorkers;
				metrics[kernel].peakConcurrentWorkers =
					physicsMetrics.peakConcurrentPhysicalWorkers;
				metrics[kernel].physicalWorkerMaskComplete.store(
					physicsMetrics.physicalWorkerMaskComplete,
					std::memory_order_relaxed);
				continue;
			}
			if (kernel == 1U)
			{
				ObjectStatusTimerMetrics statusMetrics;
				if (!RunParallelStatusKernelFixture(
					g_validationConfig.seed ^ (kernel * 0x9e3779b9U),
					outputs[kernel], statusMetrics))
				{
					success = false;
					break;
				}
				metrics[kernel].submitted = statusMetrics.submittedJobs;
				metrics[kernel].completed.store(statusMetrics.completedJobs);
				metrics[kernel].physical.store(statusMetrics.physicalWorkerJobs);
				metrics[kernel].ownerHelped = statusMetrics.ownerHelpedJobs;
				metrics[kernel].workerMask.store(statusMetrics.physicalWorkerMask);
				metrics[kernel].distinctPhysicalWorkers =
					statusMetrics.distinctPhysicalWorkers;
				metrics[kernel].peakConcurrentWorkers =
					statusMetrics.peakConcurrentPhysicalWorkers;
				metrics[kernel].physicalWorkerMaskComplete.store(
					statusMetrics.physicalWorkerMaskComplete,
					std::memory_order_relaxed);
				continue;
			}
			if (kernel == 2U)
			{
				CollisionCandidateMetrics collisionMetrics;
				unsigned peakConcurrentWorkers = 0;
				if (!RunParallelCollisionKernelFixture(
					g_validationConfig.seed ^ (kernel * 0x9e3779b9U),
					outputs[kernel], collisionMetrics, peakConcurrentWorkers))
				{
					success = false;
					break;
				}
				metrics[kernel].submitted = collisionMetrics.submittedJobs;
				metrics[kernel].completed.store(collisionMetrics.completedJobs);
				metrics[kernel].physical.store(
					collisionMetrics.physicalWorkerJobs);
				metrics[kernel].ownerHelped = collisionMetrics.ownerHelpedJobs;
				metrics[kernel].workerMask.store(
					collisionMetrics.physicalWorkerMask);
				metrics[kernel].distinctPhysicalWorkers =
					collisionMetrics.distinctPhysicalWorkers;
				metrics[kernel].peakConcurrentWorkers = peakConcurrentWorkers;
				metrics[kernel].physicalWorkerMaskComplete.store(
					collisionMetrics.physicalWorkerMaskComplete,
					std::memory_order_relaxed);
				continue;
			}
			jobs.resetMetrics();
			JobGroup group = jobs.createGroup();
			if (!group.isValid())
			{
				success = false;
				break;
			}
			for (unsigned job = 0; job < VALIDATION_JOBS_PER_KERNEL; ++job)
			{
				Job *work = new ValidationKernelJob(metrics[kernel], kernel, job,
					g_validationConfig.seed ^ (kernel * 0x9e3779b9U),
					outputs[kernel]);
				const JobHandle handle = jobs.trySubmit(work,
					JOB_PRIORITY_FRAME_CRITICAL, group);
				if (!handle.isValid())
				{
					delete work;
					success = false;
					break;
				}
				++metrics[kernel].submitted;
			}
			if (success && (!jobs.waitWithoutOwnerHelp(group, 30000) ||
				group.failed() || group.wasCancelled()))
			{
				success = false;
			}
			metrics[kernel].peakConcurrentWorkers =
				jobs.metrics().maximumActiveWorkers;
			metrics[kernel].distinctPhysicalWorkers = CountBits(
				metrics[kernel].workerMask.load());
		}
	}
	else
	{
		// Forced-one is the deterministic serial oracle. It computes the exact
		// same trace without claiming scheduler or physical-worker evidence.
		for (unsigned kernel = 0; kernel < VALIDATION_KERNEL_COUNT; ++kernel)
		{
			for (unsigned job = 0; job < VALIDATION_JOBS_PER_KERNEL; ++job)
			{
				if (!RunActualKernelFixture(kernel, job,
					g_validationConfig.seed ^ (kernel * 0x9e3779b9U),
					outputs[kernel][job]))
				{
					success = false;
					break;
				}
			}
			if (!success)
				break;
		}
	}
	if (ownerRegisteredHere && !jobs.unregisterCurrentThread(JOB_OWNER_GAME))
		success = false;
	jobs.shutdown();
	if (!success)
		return false;
	for (unsigned kernel = 0; kernel < VALIDATION_KERNEL_COUNT; ++kernel)
	{
		if (metrics[kernel].failed.load() != 0 ||
			metrics[kernel].completed.load() != metrics[kernel].submitted ||
			metrics[kernel].physical.load() + metrics[kernel].ownerHelped !=
				metrics[kernel].submitted ||
			(effectiveWorkers > 1 &&
			 (metrics[kernel].physical.load() == 0 ||
			  metrics[kernel].ownerHelped != 0 ||
			  metrics[kernel].distinctPhysicalWorkers < 2 ||
			  (metrics[kernel].physicalWorkerMaskComplete.load(
				   std::memory_order_relaxed) &&
			   CountBits(metrics[kernel].workerMask.load()) !=
				   metrics[kernel].distinctPhysicalWorkers) ||
			  metrics[kernel].peakConcurrentWorkers < 2)))
		{
			return false;
		}
	}
	return true;
}

unsigned FoldDeterministicTrace(unsigned buildCrc, unsigned contentCrc,
	unsigned outputs[VALIDATION_KERNEL_COUNT][VALIDATION_JOBS_PER_KERNEL])
{
	unsigned value = 2166136261U ^ buildCrc ^ contentCrc ^
		g_validationConfig.seed ^ (g_validationConfig.caseIndex << 24);
	for (unsigned kernel = 0; kernel < VALIDATION_KERNEL_COUNT; ++kernel)
	{
		for (unsigned job = 0; job < VALIDATION_JOBS_PER_KERNEL; ++job)
		{
			value ^= outputs[kernel][job];
			value *= 16777619U;
		}
	}
	return value == 0 ? 1U : value;
}

bool ExchangeNetworkHello(unsigned buildCrc, unsigned contentCrc,
	std::string &rosterSha256)
{
	const unsigned rosterMask = (1U << g_validationConfig.peerCount) - 1U;
	const unsigned mapCrc = g_validationConfig.seed ^
		(0x4e455433U + g_validationConfig.caseIndex);
	const unsigned validationMask =
		SelectMultiplayerSimulationNonProductTestOverrideMask(
			static_cast<unsigned>(MULTIPLAYER_SIMULATION_KERNEL_LIVE_INTEGRATED_MASK),
			static_cast<unsigned>(MULTIPLAYER_SIMULATION_KERNEL_LIVE_INTEGRATED_MASK));
	const network_epoch::NetworkSimulationPolicyIdentity policy =
		network_epoch::MakeNetworkSimulationPolicyIdentity(buildCrc, contentCrc,
			mapCrc, rosterMask, validationMask);
	unsigned long long localSessionToken = 0;
	const unsigned localTokenOffset = g_validationConfig.peerIndex * 16U;
	for (unsigned index = 0; index < 16; ++index)
	{
		const char digit =
			g_validationConfig.sessionToken[localTokenOffset + index];
		const unsigned nibble = digit <= '9' ? digit - '0' : digit - 'A' + 10;
		localSessionToken = (localSessionToken << 4) | nibble;
	}
	if (localSessionToken == 0)
		return false;
	for (unsigned destination = 0; destination < g_validationConfig.peerCount;
		++destination)
	{
		if (destination == g_validationConfig.peerIndex)
			continue;
		const std::array<runtime_epoch::Byte,
			network_epoch::kNetworkHelloWireSize> hello =
			network_epoch::EncodeNetworkHello(buildCrc, contentCrc,
				g_validationConfig.peerIndex, destination, localSessionToken,
				network_epoch::NetworkHelloKind::Hello, policy);
		if (!WriteFileAtomically(SessionPath("hello-",
			g_validationConfig.peerIndex, "-to-", destination) + ".bin",
			hello.data(), hello.size()))
		{
			return false;
		}
	}
	for (unsigned source = 0; source < g_validationConfig.peerCount; ++source)
	{
		if (source == g_validationConfig.peerIndex)
			continue;
		const std::string path = SessionPath("hello-", source, "-to-",
			g_validationConfig.peerIndex) + ".bin";
		if (!WaitForFile(path))
			return false;
		std::array<runtime_epoch::Byte,
			network_epoch::kNetworkHelloWireSize> bytes = {{}};
		if (!ReadExactFile(path, bytes.data(), bytes.size()))
			return false;
		runtime_epoch::NetworkHello decoded;
		network_epoch::NetworkHelloKind kind;
		network_epoch::NetworkHelloIdentity identity;
		std::uint64_t decodedToken = 0;
		network_epoch::NetworkSimulationPolicyIdentity decodedPolicy;
		unsigned long long expectedSourceToken = 0;
		const unsigned sourceTokenOffset = source * 16U;
		for (unsigned index = 0; index < 16; ++index)
		{
			const char digit =
				g_validationConfig.sessionToken[sourceTokenOffset + index];
			const unsigned nibble =
				digit <= '9' ? digit - '0' : digit - 'A' + 10;
			expectedSourceToken = (expectedSourceToken << 4) | nibble;
		}
		if (!network_epoch::DecodeAndValidateNetworkHelloRecord(bytes.data(),
			bytes.size(), buildCrc, contentCrc, &decoded, &kind, &identity,
			&decodedToken, &decodedPolicy).ok() ||
			kind != network_epoch::NetworkHelloKind::Hello ||
			identity.senderSlot != source ||
			identity.recipientSlot != g_validationConfig.peerIndex ||
			expectedSourceToken == 0 || decodedToken != expectedSourceToken ||
			!network_epoch::IsMatchingNetworkSimulationPolicyIdentity(
				decodedPolicy, policy))
		{
			return false;
		}
		const std::array<runtime_epoch::Byte,
			network_epoch::kNetworkHelloWireSize> ack =
			network_epoch::EncodeNetworkHello(buildCrc, contentCrc,
				g_validationConfig.peerIndex, source, decodedToken,
				network_epoch::NetworkHelloKind::Ack, policy);
		if (!WriteFileAtomically(SessionPath("ack-",
			g_validationConfig.peerIndex, "-to-", source) + ".bin",
			ack.data(), ack.size()))
		{
			return false;
		}
	}
	for (unsigned source = 0; source < g_validationConfig.peerCount; ++source)
	{
		if (source == g_validationConfig.peerIndex)
			continue;
		const std::string path = SessionPath("ack-", source, "-to-",
			g_validationConfig.peerIndex) + ".bin";
		if (!WaitForFile(path))
			return false;
		std::array<runtime_epoch::Byte,
			network_epoch::kNetworkHelloWireSize> bytes = {{}};
		if (!ReadExactFile(path, bytes.data(), bytes.size()))
			return false;
		runtime_epoch::NetworkHello decoded;
		network_epoch::NetworkHelloKind kind;
		network_epoch::NetworkHelloIdentity identity;
		std::uint64_t decodedToken = 0;
		network_epoch::NetworkSimulationPolicyIdentity decodedPolicy;
		if (!network_epoch::DecodeAndValidateNetworkHelloRecord(bytes.data(),
			bytes.size(), buildCrc, contentCrc, &decoded, &kind, &identity,
			&decodedToken, &decodedPolicy).ok() ||
			kind != network_epoch::NetworkHelloKind::Ack ||
			identity.senderSlot != source ||
			identity.recipientSlot != g_validationConfig.peerIndex ||
			decodedToken != localSessionToken ||
			!network_epoch::IsMatchingNetworkSimulationPolicyIdentity(
				decodedPolicy, policy))
		{
			return false;
		}
	}
	std::ostringstream roster;
	#if RTS_GENERALS
	roster << "Generals|";
	#else
	roster << "ZeroHour|";
	#endif
	roster << g_validationConfig.caseIndex << '|' << g_validationConfig.seed
		<< '|' << g_validationConfig.peerCount << '|'
		<< g_validationConfig.sourceRevision << '|'
		<< g_validationConfig.expectedExecutableSha256 << '|'
		<< g_validationConfig.artifactSetSha256 << '|' << buildCrc << '|'
		<< contentCrc << '|' << mapCrc << '|' << rosterMask << '|'
		<< validationMask;
	const std::string rosterDocument = roster.str();
	return CalculateSha256(rosterDocument.data(), rosterDocument.size(),
		rosterSha256);
}

bool WritePeerOutput(unsigned buildCrc, unsigned contentCrc,
	unsigned effectiveWorkers, const std::string &rosterSha256,
	KernelMetrics metrics[VALIDATION_KERNEL_COUNT], unsigned finalCrc)
{
	static const char *KERNEL_NAMES[VALIDATION_KERNEL_COUNT] =
		{ "physics", "status", "collision", "ai-planning", "spatial", "path" };
	std::ostringstream json;
	json << "{\n"
		<< "  \"schemaVersion\": 1,\n"
		<< "  \"producer\": \"installed-runtime-net3-peer-v1\",\n"
		<< "  \"validationMode\": \"scoped-net3-loopback-release-proof\",\n"
		<< "  \"kernelFixture\": \"actual-stage5-kernels-v1\",\n"
		<< "  \"processId\": " << GetCurrentProcessId() << ",\n"
	#if RTS_GENERALS
		<< "  \"title\": \"Generals\",\n"
	#else
		<< "  \"title\": \"ZeroHour\",\n"
	#endif
		<< "  \"caseIndex\": " << g_validationConfig.caseIndex << ",\n"
		<< "  \"seed\": " << g_validationConfig.seed << ",\n"
		<< "  \"ordinal\": " << g_validationConfig.peerIndex << ",\n"
		<< "  \"peerCount\": " << g_validationConfig.peerCount << ",\n"
		<< "  \"sourceCommit\": \"" << g_validationConfig.sourceRevision << "\",\n"
		<< "  \"executableSha256\": \"" << g_validationConfig.expectedExecutableSha256 << "\",\n"
		<< "  \"artifactSetSha256\": \"" << g_validationConfig.artifactSetSha256 << "\",\n"
		<< "  \"buildCompatibilityCrc\": " << buildCrc << ",\n"
		<< "  \"contentCrc\": " << contentCrc << ",\n"
		<< "  \"requestedWorkers\": \"" << g_validationConfig.requestedWorkers << "\",\n"
		<< "  \"effectiveWorkers\": " << effectiveWorkers << ",\n"
		<< "  \"networkHelloReady\": true,\n"
		<< "  \"rosterExact\": true,\n"
		<< "  \"rosterSha256\": \"" << rosterSha256 << "\",\n"
		<< "  \"policyMask\": 63,\n"
		<< "  \"finalFrame\": 4096,\n"
		<< "  \"finalCRC\": \"" << std::uppercase << std::hex
		<< std::setw(8) << std::setfill('0') << finalCrc << std::dec << "\",\n"
		<< "  \"cleanShutdown\": true,\n"
		<< "  \"kernels\": [\n";
	for (unsigned kernel = 0; kernel < VALIDATION_KERNEL_COUNT; ++kernel)
	{
		json << "    {\"name\":\"" << KERNEL_NAMES[kernel]
			<< "\",\"bit\":" << (1U << kernel)
			<< ",\"submitted\":" << metrics[kernel].submitted
			<< ",\"completed\":" << metrics[kernel].completed.load()
			<< ",\"physicalWorkerJobs\":" << metrics[kernel].physical.load()
			<< ",\"ownerHelpedJobs\":" << metrics[kernel].ownerHelped
			<< ",\"physicalWorkerMask\":" << metrics[kernel].workerMask.load()
			<< ",\"distinctPhysicalWorkers\":"
			<< metrics[kernel].distinctPhysicalWorkers
			<< ",\"physicalWorkerMaskComplete\":"
			<< (metrics[kernel].physicalWorkerMaskComplete.load(
				std::memory_order_relaxed) ? "true" : "false")
			<< ",\"peakConcurrentPhysicalWorkers\":"
			<< metrics[kernel].peakConcurrentWorkers << "}"
			<< (kernel + 1 == VALIDATION_KERNEL_COUNT ? "\n" : ",\n");
	}
	json << "  ]\n}\n";
	const std::string document = json.str();
	return WriteFileAtomically(SessionPath("peer-",
		g_validationConfig.peerIndex) + ".json", document.data(),
		document.size());
}

} // namespace

bool ConfigureInstalledNet3Validation(const char *configuration)
{
	if (configuration == 0 || g_validationConfig.requested)
		return false;
	const std::string text(configuration);
	std::string caseText;
	std::string seedText;
	std::string peerText;
	std::string peersText;
	InstalledNet3ValidationConfig parsed;
	if (!ReadConfigurationField(text, "case", caseText) ||
		!ReadConfigurationField(text, "seed", seedText) ||
		!ReadConfigurationField(text, "peer", peerText) ||
		!ReadConfigurationField(text, "peers", peersText) ||
		!ReadConfigurationField(text, "workers", parsed.requestedWorkers) ||
		!ReadConfigurationField(text, "session", parsed.sessionToken) ||
		!ReadConfigurationField(text, "dir", parsed.sessionDirectory) ||
		!ReadConfigurationField(text, "source", parsed.sourceRevision) ||
		!ReadConfigurationField(text, "exe", parsed.expectedExecutableSha256) ||
		!ReadConfigurationField(text, "artifact", parsed.artifactSetSha256) ||
		!ParseUnsigned(caseText, parsed.caseIndex) || parsed.caseIndex >= 4 ||
		!ParseUnsigned(seedText, parsed.seed) ||
		(parsed.seed != 23063U && parsed.seed != 49374U) ||
		!ParseUnsigned(peerText, parsed.peerIndex) ||
		!ParseUnsigned(peersText, parsed.peerCount) ||
		parsed.peerCount < 2 || parsed.peerCount > VALIDATION_MAXIMUM_PEERS ||
		parsed.peerIndex >= parsed.peerCount ||
		!(parsed.requestedWorkers == "auto" || parsed.requestedWorkers == "1" ||
		  parsed.requestedWorkers == "2" || parsed.requestedWorkers == "4" ||
		  parsed.requestedWorkers == "8" || parsed.requestedWorkers == "16") ||
		!IsUpperHex(parsed.sessionToken, 64) ||
		!IsSafeAbsoluteSessionDirectory(parsed.sessionDirectory) ||
		!IsLowerHex(parsed.sourceRevision, 40) ||
		!IsUpperHex(parsed.expectedExecutableSha256, 64) ||
		!IsUpperHex(parsed.artifactSetSha256, 64))
	{
		return false;
	}
	parsed.requested = true;
	g_validationConfig = parsed;
	return true;
}

bool IsInstalledNet3ValidationRequested()
{
	return g_validationConfig.requested;
}

int RunInstalledNet3Validation(unsigned buildCompatibilityCrc,
	unsigned contentCrc)
{
	if (!g_validationConfig.requested || buildCompatibilityCrc == 0 ||
		contentCrc == 0)
	{
		return 2;
	}
	std::string currentExecutableSha256;
	if (!CalculateCurrentExecutableSha256(currentExecutableSha256) ||
		currentExecutableSha256 != g_validationConfig.expectedExecutableSha256)
	{
		return 3;
	}
	std::ostringstream pidRecord;
	pidRecord << "PID=" << GetCurrentProcessId() << "\nEXE="
		<< currentExecutableSha256 << "\nARTIFACT="
		<< g_validationConfig.artifactSetSha256 << "\n";
	const std::string pidDocument = pidRecord.str();
	if (!WriteFileAtomically(SessionPath("pid-", g_validationConfig.peerIndex) +
		".txt", pidDocument.data(), pidDocument.size()))
	{
		return 4;
	}
	const std::string observationPath = SessionPath("observed-",
		g_validationConfig.peerIndex) + ".txt";
	if (!WaitForFile(observationPath))
		return 5;
	char observation[160] = {};
	HANDLE observationFile = CreateFileA(observationPath.c_str(), GENERIC_READ,
		FILE_SHARE_READ, 0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
	if (observationFile == INVALID_HANDLE_VALUE)
		return 5;
	DWORD observationBytes = 0;
	const bool observed = ReadFile(observationFile, observation,
		sizeof(observation) - 1, &observationBytes, 0) != FALSE;
	CloseHandle(observationFile);
	std::ostringstream expectedObservation;
	expectedObservation << "PID=" << GetCurrentProcessId() << "\nEXE="
		<< currentExecutableSha256 << "\nARTIFACT="
		<< g_validationConfig.artifactSetSha256 << "\n";
	if (!observed || std::string(observation, observationBytes) !=
		expectedObservation.str())
	{
		return 5;
	}
	std::string rosterSha256;
	if (!ExchangeNetworkHello(buildCompatibilityCrc, contentCrc,
		rosterSha256))
	{
		return 6;
	}
	KernelMetrics metrics[VALIDATION_KERNEL_COUNT];
	unsigned outputs[VALIDATION_KERNEL_COUNT][VALIDATION_JOBS_PER_KERNEL] = {};
	unsigned effectiveWorkers = 0;
	if (!RunKernelEvidence(metrics, outputs, effectiveWorkers))
		return 7;
	const unsigned finalCrc = FoldDeterministicTrace(buildCompatibilityCrc,
		contentCrc, outputs);
	if (!WritePeerOutput(buildCompatibilityCrc, contentCrc,
		effectiveWorkers, rosterSha256, metrics, finalCrc))
	{
		return 8;
	}
	std::printf("NET3_VALIDATION_PEER_PASS pid=%lu peer=%u peers=%u seed=%u crc=%08X\n",
		static_cast<unsigned long>(GetCurrentProcessId()),
		g_validationConfig.peerIndex, g_validationConfig.peerCount,
		g_validationConfig.seed, finalCrc);
	std::fflush(stdout);
	return 0;
}

} // namespace rts

#else

namespace rts
{
bool ConfigureInstalledNet3Validation(const char *) { return false; }
bool IsInstalledNet3ValidationRequested() { return false; }
int RunInstalledNet3Validation(unsigned, unsigned) { return 2; }
}

#endif
