/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2026 TheSuperHackers
**
** This program is free software: you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation, either version 3 of the License, or
** (at your option) any later version.
*/

#include "PreRTS.h"

#include "Common/GameEngine.h"
#include "Common/GlobalData.h"
#include "Common/MessageStream.h"
#include "Common/NameKeyGenerator.h"
#include "Common/Player.h"
#include "Common/PlayerList.h"
#include "Common/RandomValue.h"
#include "Common/Recorder.h"
#include "Common/SkirmishAITestRunner.h"
#include "GameClient/MapUtil.h"
#include "GameLogic/GameLogic.h"
#include "GameLogic/AIPathfind.h"
#include "GameLogic/TerrainLogic.h"
#include "GameLogic/VictoryConditions.h"
#include "Lib/JobSystem.h"
#include "Lib/PipelineExecutionPolicy.h"
#include "Lib/SimulationExecutionPolicy.h"
#include "Lib/SimulationPhaseGraphOwnerAdapter.h"
#if defined(_WIN64)
#include "Common/FileSystem.h"
#include "Common/PerformanceReceiptRuntime.h"
#include "Common/crc.h"
#include "Lib/CollisionCandidateKernel.h"
#include "Lib/DeterministicAIPlanning.h"
#include "Lib/ImmutableSpatialQueryRuntime.h"
#include "Lib/ObjectStatusTimerKernel.h"
#include "Lib/PhysicsIntegrationKernel.h"
#include <memory>
#include <new>
#include <vector>
#endif

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <io.h>
#endif

namespace
{
struct SkirmishAITestRunnerState
{
	Bool armed;
	Bool started;
	Bool ending;
	Bool finished;
	Bool failed;
	Int seed;
	Int winnerTeam;
	UnsignedInt endFrame;
	UnsignedInt startupStartMilliseconds;
	UnsignedInt lastObservedFrame;
	UnsignedInt stalledStartMilliseconds;
	UnsignedInt shutdownStartMilliseconds;
	char replayFileName[_MAX_PATH + 1];
	const char *failureReason;
	UnsignedInt expectedMapCRC;
	UnsignedInt expectedMapSize;
	Bool loadedStateValidated;
	char loadedMapName[_MAX_PATH + 1];
	UnsignedInt loadedMapCRC;
	UnsignedInt loadedMapSize;
	Int loadedSeed;
	SkirmishAITestScenario scenario;
	Int actualAiCount;
	Int actualTeamCounts[2];
	Int replayEpoch;
	char retainedReplayPath[SKIRMISH_AI_TEST_RECEIPT_PATH_LENGTH];
	char replaySha256[SKIRMISH_AI_TEST_RECEIPT_SHA256_LENGTH + 1];
	char runNonce[SKIRMISH_AI_TEST_RECEIPT_NONCE_LENGTH + 1];
	unsigned requestedWorkerCount;
	rts::JobWorkerPolicy workerPolicy;
	unsigned effectiveWorkerCount;
	rts::PipelineExecutionMode requestedPipelineMode;
	rts::SimulationExecutionMode requestedSimulationMode;
};

SkirmishAITestRunnerState s_runner = {
	FALSE, FALSE, FALSE, FALSE, FALSE, 0, -1, 0, 0, UINT_MAX, 0, 0, { 0 }, nullptr
};
rts::JobSystemMetrics s_jobMetricsAtStart;
rts::LiveSimulationPhaseRuntimeMetrics s_phaseMetricsAtStart;
rts::LiveSimulationPhaseRuntimeMetrics s_phaseMetricsLast;
DirectPathRuntimeMetrics s_directPathMetricsAtStart;
DirectPathRuntimeMetrics s_directPathMetricsFrozen;
Bool s_directPathMetricsHaveFrozenActivity = FALSE;
Bool s_directPathMetricsAwaitingInitialReset = FALSE;
OrdinaryPathRuntimeMetrics s_ordinaryPathMetricsAtStart;
OrdinaryPathRuntimeMetrics s_ordinaryPathMetricsFrozen;
Bool s_ordinaryPathMetricsAwaitingInitialReset = FALSE;
#if defined(_WIN64)
std::unique_ptr<PerformanceReceiptRuntime> s_performanceReceipt;
bool s_performanceReceiptAttempted = false;
rts::AIPlanningRuntimeMetrics s_aiPlanningMetricsAtStart;
rts::CollisionCandidateRuntimeMetrics s_collisionMetricsAtStart;
rts::CollisionCandidateRuntimeMetrics s_collisionMetricsFrozen;
Bool s_collisionMetricsAwaitingInitialReset = FALSE;
rts::PhysicsIntegrationRuntimeMetrics s_physicsMetricsAtStart;
rts::PhysicsIntegrationRuntimeMetrics s_physicsMetricsFrozen;
Bool s_physicsMetricsAwaitingInitialReset = FALSE;
rts::ObjectStatusTimerRuntimeMetrics s_statusMetricsAtStart;
rts::ObjectStatusTimerRuntimeMetrics s_statusMetricsFrozen;
Bool s_statusMetricsAwaitingInitialReset = FALSE;
rts::ImmutableSpatialRuntimeMetrics s_spatialMetricsAtStart;
rts::ImmutableSpatialRuntimeMetrics s_spatialMetricsFrozen;
Bool s_spatialMetricsAwaitingInitialReset = FALSE;
#endif
char s_executableHashInput[65] = "unavailable";
char s_executableHashObserved[SKIRMISH_AI_TEST_RECEIPT_SHA256_LENGTH + 1] =
	"unavailable";
char s_simulationModeInput[16] = "unknown";
UnsignedInt s_finalDigest = 0;
Bool s_finalDigestAvailable = FALSE;
UnsignedInt s_runnerNonceCounter = 0;

void CaptureSkirmishAITestSliceMetrics();
Bool IsHexDigit(char value);

UnsignedInt ElapsedMilliseconds(UnsignedInt startMilliseconds, UnsignedInt nowMilliseconds)
{
	// Unsigned subtraction keeps short deadlines correct across the 32-bit
	// GetTickCount wrap and remains compatible with the VC6 reference lane.
	return nowMilliseconds - startMilliseconds;
}

class SkirmishAITestSha256
{
public:
	SkirmishAITestSha256() : m_dataLength(0), m_bitLengthLow(0),
		m_bitLengthHigh(0)
	{
		m_state[0] = 0x6a09e667U;
		m_state[1] = 0xbb67ae85U;
		m_state[2] = 0x3c6ef372U;
		m_state[3] = 0xa54ff53aU;
		m_state[4] = 0x510e527fU;
		m_state[5] = 0x9b05688cU;
		m_state[6] = 0x1f83d9abU;
		m_state[7] = 0x5be0cd19U;
	}

	void update(const unsigned char *bytes, size_t byteCount)
	{
		if (bytes == nullptr || byteCount == 0)
			return;
		const UnsignedInt lowBits = static_cast<UnsignedInt>(byteCount << 3);
		const UnsignedInt highBits = static_cast<UnsignedInt>(byteCount >> 29);
		const UnsignedInt previousLowBits = m_bitLengthLow;
		m_bitLengthLow += lowBits;
		if (m_bitLengthLow < previousLowBits)
			++m_bitLengthHigh;
		m_bitLengthHigh += highBits;

		size_t index = 0;
		while (index < byteCount)
		{
			m_data[m_dataLength++] = bytes[index++];
			if (m_dataLength == sizeof(m_data))
			{
				transform(m_data);
				m_dataLength = 0;
			}
		}
	}

	void finish(char digest[SKIRMISH_AI_TEST_RECEIPT_SHA256_LENGTH + 1])
	{
		if (digest == nullptr)
			return;
		UnsignedInt index = m_dataLength;
		m_data[index++] = 0x80;
		if (index > 56)
		{
			while (index < sizeof(m_data))
				m_data[index++] = 0;
			transform(m_data);
			index = 0;
		}
		while (index < 56)
			m_data[index++] = 0;
		m_data[56] = static_cast<unsigned char>(m_bitLengthHigh >> 24);
		m_data[57] = static_cast<unsigned char>(m_bitLengthHigh >> 16);
		m_data[58] = static_cast<unsigned char>(m_bitLengthHigh >> 8);
		m_data[59] = static_cast<unsigned char>(m_bitLengthHigh);
		m_data[60] = static_cast<unsigned char>(m_bitLengthLow >> 24);
		m_data[61] = static_cast<unsigned char>(m_bitLengthLow >> 16);
		m_data[62] = static_cast<unsigned char>(m_bitLengthLow >> 8);
		m_data[63] = static_cast<unsigned char>(m_bitLengthLow);
		transform(m_data);

		static const char hex[] = "0123456789ABCDEF";
		for (UnsignedInt stateIndex = 0; stateIndex < 8; ++stateIndex)
		{
			const UnsignedInt value = m_state[stateIndex];
			const UnsignedInt offset = stateIndex * 8;
			digest[offset] = hex[(value >> 28) & 0x0f];
			digest[offset + 1] = hex[(value >> 24) & 0x0f];
			digest[offset + 2] = hex[(value >> 20) & 0x0f];
			digest[offset + 3] = hex[(value >> 16) & 0x0f];
			digest[offset + 4] = hex[(value >> 12) & 0x0f];
			digest[offset + 5] = hex[(value >> 8) & 0x0f];
			digest[offset + 6] = hex[(value >> 4) & 0x0f];
			digest[offset + 7] = hex[value & 0x0f];
		}
		digest[SKIRMISH_AI_TEST_RECEIPT_SHA256_LENGTH] = '\0';
	}

private:
	static UnsignedInt rotateRight(UnsignedInt value, UnsignedInt count)
	{
		return (value >> count) | (value << (32 - count));
	}

	void transform(const unsigned char block[64])
	{
		static const UnsignedInt k[64] = {
			0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
			0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
			0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
			0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
			0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
			0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
			0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
			0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
			0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
			0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
			0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
			0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
			0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
			0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
			0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
			0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U
		};
		UnsignedInt words[64];
		for (UnsignedInt index = 0; index < 16; ++index)
		{
			const UnsignedInt offset = index * 4;
			words[index] = (static_cast<UnsignedInt>(block[offset]) << 24) |
				(static_cast<UnsignedInt>(block[offset + 1]) << 16) |
				(static_cast<UnsignedInt>(block[offset + 2]) << 8) |
				static_cast<UnsignedInt>(block[offset + 3]);
		}
		for (UnsignedInt wordIndex = 16; wordIndex < 64; ++wordIndex)
		{
			const UnsignedInt s0 = rotateRight(words[wordIndex - 15], 7) ^
				rotateRight(words[wordIndex - 15], 18) ^ (words[wordIndex - 15] >> 3);
			const UnsignedInt s1 = rotateRight(words[wordIndex - 2], 17) ^
				rotateRight(words[wordIndex - 2], 19) ^ (words[wordIndex - 2] >> 10);
			words[wordIndex] = words[wordIndex - 16] + s0 + words[wordIndex - 7] + s1;
		}

		UnsignedInt a = m_state[0];
		UnsignedInt b = m_state[1];
		UnsignedInt c = m_state[2];
		UnsignedInt d = m_state[3];
		UnsignedInt e = m_state[4];
		UnsignedInt f = m_state[5];
		UnsignedInt g = m_state[6];
		UnsignedInt h = m_state[7];
		for (UnsignedInt roundIndex = 0; roundIndex < 64; ++roundIndex)
		{
			const UnsignedInt sum1 = rotateRight(e, 6) ^ rotateRight(e, 11) ^
				rotateRight(e, 25);
			const UnsignedInt choice = (e & f) ^ ((~e) & g);
			const UnsignedInt temp1 = h + sum1 + choice + k[roundIndex] + words[roundIndex];
			const UnsignedInt sum0 = rotateRight(a, 2) ^ rotateRight(a, 13) ^
				rotateRight(a, 22);
			const UnsignedInt majority = (a & b) ^ (a & c) ^ (b & c);
			const UnsignedInt temp2 = sum0 + majority;
			h = g;
			g = f;
			f = e;
			e = d + temp1;
			d = c;
			c = b;
			b = a;
			a = temp1 + temp2;
		}
		m_state[0] += a;
		m_state[1] += b;
		m_state[2] += c;
		m_state[3] += d;
		m_state[4] += e;
		m_state[5] += f;
		m_state[6] += g;
		m_state[7] += h;
	}

	unsigned char m_data[64];
	UnsignedInt m_dataLength;
	UnsignedInt m_bitLengthLow;
	UnsignedInt m_bitLengthHigh;
	UnsignedInt m_state[8];
};

Bool HasBoundedString(const char *value, size_t capacity)
{
	if (value == nullptr || capacity == 0)
		return FALSE;
	for (size_t index = 0; index < capacity; ++index)
	{
		if (value[index] == '\0')
			return TRUE;
	}
	return FALSE;
}

Bool IsHexString(const char *value, size_t length)
{
	if (value == nullptr || !HasBoundedString(value, length + 1) ||
		strlen(value) != length)
		return FALSE;
	for (size_t index = 0; index < length; ++index)
	{
		if (!IsHexDigit(value[index]))
			return FALSE;
	}
	return TRUE;
}

Bool HashSkirmishAITestFile(const char *path,
	char digest[SKIRMISH_AI_TEST_RECEIPT_SHA256_LENGTH + 1])
{
	if (path == nullptr || digest == nullptr)
		return FALSE;
	FILE *file = fopen(path, "rb");
	if (file == nullptr)
		return FALSE;
	SkirmishAITestSha256 sha256;
	unsigned char bytes[32768];
	Bool success = TRUE;
	while (!feof(file))
	{
		const size_t readCount = fread(bytes, 1, sizeof(bytes), file);
		if (readCount != 0)
			sha256.update(bytes, readCount);
		if (ferror(file))
		{
			success = FALSE;
			break;
		}
	}
	if (fclose(file) != 0)
		success = FALSE;
	if (success)
		sha256.finish(digest);
	return success;
}

Bool CommitSkirmishAITestReplay(const char *temporaryPath,
	const char *destinationPath)
{
#if defined(_WIN32)
	return MoveFileExA(temporaryPath, destinationPath, MOVEFILE_WRITE_THROUGH)
		? TRUE : FALSE;
#else
	return rename(temporaryPath, destinationPath) == 0;
#endif
}

Bool RetainSkirmishAITestReplayAtomicallyInternal(const char *sourcePath,
	const char *destinationPath,
	char sha256[SKIRMISH_AI_TEST_RECEIPT_SHA256_LENGTH + 1])
{
	if (sourcePath == nullptr || destinationPath == nullptr || sha256 == nullptr ||
		!HasBoundedString(sourcePath, SKIRMISH_AI_TEST_RECEIPT_PATH_LENGTH) ||
		!HasBoundedString(destinationPath, SKIRMISH_AI_TEST_RECEIPT_PATH_LENGTH) ||
		strlen(sourcePath) == 0 || strlen(destinationPath) == 0)
	{
		return FALSE;
	}
#if defined(_WIN32)
	if (_stricmp(sourcePath, destinationPath) == 0)
#else
	if (strcmp(sourcePath, destinationPath) == 0)
#endif
		return FALSE;
	char temporaryPath[SKIRMISH_AI_TEST_RECEIPT_PATH_LENGTH];
	if (strlen(destinationPath) + 5 >= sizeof(temporaryPath))
		return FALSE;
	_snprintf(temporaryPath, sizeof(temporaryPath), "%s.tmp", destinationPath);
	temporaryPath[sizeof(temporaryPath) - 1] = '\0';
	FILE *existingTemporary = fopen(temporaryPath, "rb");
	if (existingTemporary != nullptr)
	{
		fclose(existingTemporary);
		return FALSE;
	}
	FILE *source = fopen(sourcePath, "rb");
	if (source == nullptr)
		return FALSE;
	FILE *temporary = fopen(temporaryPath, "wb");
	if (temporary == nullptr)
	{
		fclose(source);
		return FALSE;
	}
	SkirmishAITestSha256 hasher;
	unsigned char bytes[32768];
	Bool success = TRUE;
	while (!feof(source))
	{
		const size_t readCount = fread(bytes, 1, sizeof(bytes), source);
		if (readCount != 0)
		{
			hasher.update(bytes, readCount);
			if (fwrite(bytes, 1, readCount, temporary) != readCount)
			{
				success = FALSE;
				break;
			}
		}
		if (ferror(source))
		{
			success = FALSE;
			break;
		}
	}
	if (fflush(temporary) != 0)
		success = FALSE;
#if defined(_WIN32)
	if (success && _commit(_fileno(temporary)) != 0)
		success = FALSE;
#endif
	if (fclose(source) != 0)
		success = FALSE;
	if (fclose(temporary) != 0)
		success = FALSE;
	if (!success || !CommitSkirmishAITestReplay(temporaryPath, destinationPath))
	{
		remove(temporaryPath);
		return FALSE;
	}
	hasher.finish(sha256);
	return TRUE;
}

Bool CaptureSkirmishAITestExecutableHash(
	char digest[SKIRMISH_AI_TEST_RECEIPT_SHA256_LENGTH + 1])
{
#if defined(_WIN32)
	char modulePath[SKIRMISH_AI_TEST_RECEIPT_PATH_LENGTH];
	const DWORD pathLength = GetModuleFileNameA(nullptr, modulePath,
		static_cast<DWORD>(sizeof(modulePath)));
	if (pathLength == 0 || pathLength >= sizeof(modulePath))
		return FALSE;
	modulePath[pathLength] = '\0';
	return HashSkirmishAITestFile(modulePath, digest);
#else
	(void)digest;
	return FALSE;
#endif
}

Bool IsHexDigit(char value)
{
	return (value >= '0' && value <= '9') ||
		(value >= 'a' && value <= 'f') ||
		(value >= 'A' && value <= 'F');
}

const char *SkirmishAITestPipelineModeName(rts::PipelineExecutionMode mode)
{
	return mode == rts::PIPELINE_EXECUTION_SERIAL ? "serial" : "parallel";
}

const char *SkirmishAITestSimulationModeName(rts::SimulationExecutionMode mode)
{
	if (mode == rts::SIMULATION_EXECUTION_PARALLEL) return "parallel";
	if (mode == rts::SIMULATION_EXECUTION_SHADOW) return "shadow";
	return "serial";
}

void CaptureSkirmishAITestRuntimeState()
{
	const unsigned workerCount = rts::JobSystem::instance().workerCount();
	if (workerCount > s_runner.effectiveWorkerCount)
		s_runner.effectiveWorkerCount = workerCount;
	if (TheGameLogic != nullptr)
		s_phaseMetricsLast = TheGameLogic->getStage5PhaseRuntimeMetrics();
	CaptureSkirmishAITestSliceMetrics();
}

#if defined(_WIN64)
struct ClosePerformanceMapFile
{
	void operator()(File *file) const { if (file != 0) file->close(); }
};

void BindSkirmishAITestPerformanceMap()
{
	if (!s_performanceReceipt || !s_performanceReceipt->active()) return;
	if (TheFileSystem == 0)
	{
		s_performanceReceipt->invalidate("loaded map filesystem was unavailable");
		return;
	}
	std::unique_ptr<File, ClosePerformanceMapFile> file(TheFileSystem->openFile(
		s_runner.loadedMapName, File::READ | File::BINARY | File::STREAMING));
	const Int length = file ? file->size() : 0;
	if (length <= 0 || length > 64 * 1024 * 1024 ||
		static_cast<unsigned>(length) != s_runner.loadedMapSize)
	{
		s_performanceReceipt->invalidate("loaded map content size could not be verified");
		return;
	}
	try
	{
		std::vector<unsigned char> bytes(static_cast<size_t>(length));
		Int offset = 0;
		while (offset < length)
		{
			const Int count = file->read(&bytes[static_cast<size_t>(offset)], length - offset);
			if (count <= 0 || count > length - offset) break;
			offset += count;
		}
		unsigned char extra = 0;
		if (offset != length || file->read(&extra, 1) != 0)
		{
			s_performanceReceipt->invalidate("loaded map content was not read exactly");
			return;
		}
		CRC crc;
		crc.computeCRC(&bytes[0], length);
		char sha256[65];
		if (crc.get() != s_runner.loadedMapCRC ||
			!HashSkirmishAITestBytes(&bytes[0], bytes.size(), sha256))
		{
			s_performanceReceipt->invalidate("loaded map content disagrees with the live map");
			return;
		}
		s_performanceReceipt->bindFixture("fresh-ai-map", s_runner.loadedMapName,
			sha256, static_cast<unsigned>(s_runner.loadedSeed));
	}
	catch (const std::bad_alloc &)
	{
		s_performanceReceipt->invalidate("loaded map hashing storage was unavailable");
	}
}
#endif

rts::JobMetricCounter JobMetricDelta(rts::JobMetricCounter finalValue,
	rts::JobMetricCounter initialValue)
{
	return finalValue >= initialValue ? finalValue - initialValue : finalValue;
}

} // namespace

Bool RetainSkirmishAITestReplayAtomically(const char *sourcePath,
	const char *destinationPath,
	char sha256[SKIRMISH_AI_TEST_RECEIPT_SHA256_LENGTH + 1])
{
	return RetainSkirmishAITestReplayAtomicallyInternal(sourcePath,
		destinationPath, sha256);
}

Bool HashSkirmishAITestBytes(const void *bytes, size_t byteCount,
	char sha256[SKIRMISH_AI_TEST_RECEIPT_SHA256_LENGTH + 1])
{
	if ((!bytes && byteCount != 0) || !sha256) return FALSE;
	SkirmishAITestSha256 hash;
	if (byteCount != 0)
		hash.update(static_cast<const unsigned char *>(bytes), byteCount);
	hash.finish(sha256);
	return TRUE;
}

Bool CaptureSkirmishAITestValidatedExecutableHash(
	char sha256[SKIRMISH_AI_TEST_RECEIPT_SHA256_LENGTH + 1])
{
	return CaptureSkirmishAITestExecutableHash(sha256) &&
		(strcmp(s_executableHashInput, "unavailable") == 0 ||
			_stricmp(s_executableHashInput, sha256) == 0);
}

Bool HashSkirmishAITestContentFile(const char *path,
	char sha256[SKIRMISH_AI_TEST_RECEIPT_SHA256_LENGTH + 1])
{
	return HashSkirmishAITestFile(path, sha256);
}

void AccumulateSkirmishAITestDirectPathMetrics(
	DirectPathRuntimeMetrics *baseline,
	const DirectPathRuntimeMetrics &current,
	DirectPathRuntimeMetrics *frozen,
	Bool *hasFrozenActivity,
	Bool *awaitingInitialReset)
{
	if (baseline == nullptr || frozen == nullptr || hasFrozenActivity == nullptr ||
		awaitingInitialReset == nullptr)
		return;
	if (*awaitingInitialReset)
	{
		// Arm occurs in the shell before MSG_NEW_GAME resets Pathfinder. Ignore
		// shell diagnostics until that first epoch transition, then treat the
		// new epoch as the match-local zero baseline.
		if (current.resetEpoch == baseline->resetEpoch)
			return;
		memset(baseline, 0, sizeof(*baseline));
		baseline->resetEpoch = current.resetEpoch;
		*awaitingInitialReset = FALSE;
	}
	else if (current.resetEpoch != baseline->resetEpoch)
	{
		// A later transition is teardown. Frozen match evidence must survive it.
		return;
	}
#define CAPTURE_PATH_COUNTER(member) \
	do { const UnsignedInt captured = current.member >= baseline->member ? \
		current.member - baseline->member : current.member; \
		if (captured > frozen->member) frozen->member = captured; } while (0)
	CAPTURE_PATH_COUNTER(eligibleRequests);
	CAPTURE_PATH_COUNTER(submittedJobs);
	CAPTURE_PATH_COUNTER(executedJobs);
	CAPTURE_PATH_COUNTER(workerExecutedJobs);
	CAPTURE_PATH_COUNTER(ownerHelpedJobs);
	CAPTURE_PATH_COUNTER(authoritativeCommits);
	CAPTURE_PATH_COUNTER(authoritativeMultiWorkerCommits);
	CAPTURE_PATH_COUNTER(staleRejections);
	CAPTURE_PATH_COUNTER(validationFailures);
	CAPTURE_PATH_COUNTER(serialFallbacks);
	CAPTURE_PATH_COUNTER(unsupportedAuthoritativeCommits);
	CAPTURE_PATH_COUNTER(shadowAuthoritativeCommits);
	CAPTURE_PATH_COUNTER(staleAuthoritativeCommits);
	CAPTURE_PATH_COUNTER(malformedAuthoritativeCommits);
	CAPTURE_PATH_COUNTER(shadowOnlyExecutions);
	CAPTURE_PATH_COUNTER(timeoutCancellations);
	CAPTURE_PATH_COUNTER(lateDrainExecutions);
#undef CAPTURE_PATH_COUNTER
	if (current.peakActiveWorkers > frozen->peakActiveWorkers)
		frozen->peakActiveWorkers = current.peakActiveWorkers;
	if (current.minimumCallbackCount != 0 &&
		(frozen->minimumCallbackCount == 0 ||
		 current.minimumCallbackCount < frozen->minimumCallbackCount))
		frozen->minimumCallbackCount = current.minimumCallbackCount;
	if (current.maximumCallbackCount > frozen->maximumCallbackCount)
		frozen->maximumCallbackCount = current.maximumCallbackCount;
	if (frozen->eligibleRequests != 0 || frozen->submittedJobs != 0 ||
		frozen->executedJobs != 0 || frozen->workerExecutedJobs != 0 ||
		frozen->ownerHelpedJobs != 0 || frozen->authoritativeCommits != 0 ||
		frozen->authoritativeMultiWorkerCommits != 0 ||
		frozen->staleRejections != 0 || frozen->validationFailures != 0 ||
		frozen->serialFallbacks != 0 ||
		frozen->unsupportedAuthoritativeCommits != 0 ||
		frozen->shadowAuthoritativeCommits != 0 ||
		frozen->staleAuthoritativeCommits != 0 ||
		frozen->malformedAuthoritativeCommits != 0 ||
		frozen->shadowOnlyExecutions != 0 ||
		frozen->timeoutCancellations != 0 ||
		frozen->lateDrainExecutions != 0 ||
		frozen->peakActiveWorkers != 0 ||
		frozen->minimumCallbackCount != 0 ||
		frozen->maximumCallbackCount != 0)
		*hasFrozenActivity = TRUE;
}

void AccumulateSkirmishAITestOrdinaryPathMetrics(
	OrdinaryPathRuntimeMetrics *baseline,
	const OrdinaryPathRuntimeMetrics &current,
	OrdinaryPathRuntimeMetrics *frozen,
	Bool *awaitingInitialReset)
{
	if (baseline == nullptr || frozen == nullptr ||
		awaitingInitialReset == nullptr)
	{
		return;
	}
	if (*awaitingInitialReset)
	{
		if (current.resetEpoch == baseline->resetEpoch)
			return;
		memset(baseline, 0, sizeof(*baseline));
		baseline->resetEpoch = current.resetEpoch;
		frozen->physicalWorkerMaskComplete = TRUE;
		*awaitingInitialReset = FALSE;
	}
	else if (current.resetEpoch != baseline->resetEpoch)
	{
		// MSG_CLEAR_GAME_DATA starts a new epoch after the match. Preserve the
		// last same-epoch worker and owner-commit evidence for the manifest.
		return;
	}
#define CAPTURE_ORDINARY_PATH_COUNTER(member) \
	do { const UnsignedInt captured = current.member >= baseline->member ? \
		current.member - baseline->member : current.member; \
		if (captured > frozen->member) frozen->member = captured; } while (0)
	CAPTURE_ORDINARY_PATH_COUNTER(eligibleRequests);
	CAPTURE_ORDINARY_PATH_COUNTER(submittedRequests);
	CAPTURE_ORDINARY_PATH_COUNTER(submittedRangeJobs);
	CAPTURE_ORDINARY_PATH_COUNTER(workerExecutedRequests);
	CAPTURE_ORDINARY_PATH_COUNTER(workerExecutedRangeJobs);
	CAPTURE_ORDINARY_PATH_COUNTER(ownerHelpedRangeJobs);
	CAPTURE_ORDINARY_PATH_COUNTER(failedRangeJobs);
	CAPTURE_ORDINARY_PATH_COUNTER(authoritativeCommits);
	CAPTURE_ORDINARY_PATH_COUNTER(authoritativeMultiWorkerCommits);
	CAPTURE_ORDINARY_PATH_COUNTER(staleRejections);
	CAPTURE_ORDINARY_PATH_COUNTER(validationFailures);
	CAPTURE_ORDINARY_PATH_COUNTER(serialFallbacks);
	CAPTURE_ORDINARY_PATH_COUNTER(shadowComparisons);
	CAPTURE_ORDINARY_PATH_COUNTER(shadowMismatches);
	CAPTURE_ORDINARY_PATH_COUNTER(timeoutCancellations);
	CAPTURE_ORDINARY_PATH_COUNTER(lateDrainExecutions);
#undef CAPTURE_ORDINARY_PATH_COUNTER
	frozen->physicalWorkerMask |= current.physicalWorkerMask;
	if (current.distinctPhysicalWorkers > frozen->distinctPhysicalWorkers)
		frozen->distinctPhysicalWorkers = current.distinctPhysicalWorkers;
	if (!current.physicalWorkerMaskComplete)
		frozen->physicalWorkerMaskComplete = FALSE;
	if (current.peakActiveWorkers > frozen->peakActiveWorkers)
		frozen->peakActiveWorkers = current.peakActiveWorkers;
	if (current.maximumBatchRequests > frozen->maximumBatchRequests)
		frozen->maximumBatchRequests = current.maximumBatchRequests;
	if (current.maximumRangeCount > frozen->maximumRangeCount)
		frozen->maximumRangeCount = current.maximumRangeCount;
	if (current.maximumGrainSize > frozen->maximumGrainSize)
		frozen->maximumGrainSize = current.maximumGrainSize;
	frozen->resetEpoch = baseline->resetEpoch;
}

#if defined(_WIN64)
void AccumulateSkirmishAITestCollisionMetrics(
	rts::CollisionCandidateRuntimeMetrics *baseline,
	const rts::CollisionCandidateRuntimeMetrics &current,
	rts::CollisionCandidateRuntimeMetrics *frozen,
	Bool *awaitingInitialReset)
{
	if (baseline == nullptr || frozen == nullptr || awaitingInitialReset == nullptr)
		return;
	if (*awaitingInitialReset)
	{
		if (current.resetEpoch == baseline->resetEpoch)
			return;
		*baseline = rts::CollisionCandidateRuntimeMetrics();
		baseline->resetEpoch = current.resetEpoch;
		*awaitingInitialReset = FALSE;
	}
	else if (current.resetEpoch != baseline->resetEpoch)
	{
		return;
	}
#define CAPTURE_COLLISION_COUNTER(member) \
	frozen->member = current.member - baseline->member
	CAPTURE_COLLISION_COUNTER(authoritativeCommits);
	CAPTURE_COLLISION_COUNTER(shadowExecutions);
	CAPTURE_COLLISION_COUNTER(shadowMismatches);
	CAPTURE_COLLISION_COUNTER(ownerFallbacks);
	CAPTURE_COLLISION_COUNTER(unexpectedFallbacks);
	CAPTURE_COLLISION_COUNTER(ineligibleSlices);
	CAPTURE_COLLISION_COUNTER(staleRejections);
	CAPTURE_COLLISION_COUNTER(committedCandidates);
	CAPTURE_COLLISION_COUNTER(shadowComparedCandidates);
	CAPTURE_COLLISION_COUNTER(preparedPairs);
	CAPTURE_COLLISION_COUNTER(uniqueCandidates);
	CAPTURE_COLLISION_COUNTER(submittedJobs);
	CAPTURE_COLLISION_COUNTER(completedJobs);
	CAPTURE_COLLISION_COUNTER(physicalWorkerJobs);
	CAPTURE_COLLISION_COUNTER(ownerHelpedJobs);
#undef CAPTURE_COLLISION_COUNTER
	// A worker identity mask is an accumulated set, not a monotonic counter.
	// The first match reset establishes an empty epoch-local baseline.
	frozen->physicalWorkerMask = current.physicalWorkerMask;
	frozen->distinctPhysicalWorkers = current.distinctPhysicalWorkers;
	frozen->physicalWorkerMaskComplete = current.physicalWorkerMaskComplete;
	frozen->resetEpoch = baseline->resetEpoch;
}

void AccumulateSkirmishAITestPhysicsMetrics(
	rts::PhysicsIntegrationRuntimeMetrics *baseline,
	const rts::PhysicsIntegrationRuntimeMetrics &current,
	rts::PhysicsIntegrationRuntimeMetrics *frozen,
	Bool *awaitingInitialReset)
{
	if (baseline == nullptr || frozen == nullptr || awaitingInitialReset == nullptr)
		return;
	if (*awaitingInitialReset)
	{
		if (current.resetEpoch == baseline->resetEpoch)
			return;
		*baseline = rts::PhysicsIntegrationRuntimeMetrics();
		baseline->resetEpoch = current.resetEpoch;
		*awaitingInitialReset = FALSE;
	}
	else if (current.resetEpoch != baseline->resetEpoch)
	{
		return;
	}
#define CAPTURE_PHYSICS_COUNTER(member) \
	frozen->member = current.member - baseline->member
	CAPTURE_PHYSICS_COUNTER(acceptedBatches);
	CAPTURE_PHYSICS_COUNTER(acceptedPrefixes);
	CAPTURE_PHYSICS_COUNTER(acceptedRanges);
	CAPTURE_PHYSICS_COUNTER(acceptedSubmittedJobs);
	CAPTURE_PHYSICS_COUNTER(acceptedCompletedJobs);
	CAPTURE_PHYSICS_COUNTER(acceptedPhysicalWorkerJobs);
	CAPTURE_PHYSICS_COUNTER(acceptedOwnerHelpedJobs);
	frozen->acceptedPhysicalWorkerMask = current.acceptedPhysicalWorkerMask;
	if (!current.acceptedPhysicalWorkerMaskComplete)
		frozen->acceptedPhysicalWorkerMaskComplete = false;
	if (current.maximumAcceptedDistinctPhysicalWorkers >
		frozen->maximumAcceptedDistinctPhysicalWorkers)
		frozen->maximumAcceptedDistinctPhysicalWorkers =
			current.maximumAcceptedDistinctPhysicalWorkers;
	if (current.maximumAcceptedPeakConcurrentPhysicalWorkers >
		frozen->maximumAcceptedPeakConcurrentPhysicalWorkers)
		frozen->maximumAcceptedPeakConcurrentPhysicalWorkers =
			current.maximumAcceptedPeakConcurrentPhysicalWorkers;
	CAPTURE_PHYSICS_COUNTER(acceptedAllocatedBytes);
	CAPTURE_PHYSICS_COUNTER(acceptedCaptureNanoseconds);
	CAPTURE_PHYSICS_COUNTER(acceptedPrepareNanoseconds);
	CAPTURE_PHYSICS_COUNTER(acceptedWaitNanoseconds);
	CAPTURE_PHYSICS_COUNTER(acceptedCommitNanoseconds);
	CAPTURE_PHYSICS_COUNTER(acceptedStorageBytes);
	CAPTURE_PHYSICS_COUNTER(acceptedStorageCapacityBytes);
	CAPTURE_PHYSICS_COUNTER(acceptedStorageAllocations);
	CAPTURE_PHYSICS_COUNTER(shadowBatches);
	CAPTURE_PHYSICS_COUNTER(shadowPrefixes);
	CAPTURE_PHYSICS_COUNTER(shadowRanges);
	CAPTURE_PHYSICS_COUNTER(shadowSubmittedJobs);
	CAPTURE_PHYSICS_COUNTER(shadowCompletedJobs);
	CAPTURE_PHYSICS_COUNTER(shadowMatches);
	CAPTURE_PHYSICS_COUNTER(shadowMismatches);
	CAPTURE_PHYSICS_COUNTER(ownerFallbacks);
	CAPTURE_PHYSICS_COUNTER(ineligibleSlices);
	CAPTURE_PHYSICS_COUNTER(unexpectedFallbacks);
	CAPTURE_PHYSICS_COUNTER(staleRejections);
	CAPTURE_PHYSICS_COUNTER(circuitBreakerTrips);
#undef CAPTURE_PHYSICS_COUNTER
	frozen->resetEpoch = baseline->resetEpoch;
}

void AccumulateSkirmishAITestObjectStatusTimerMetrics(
	rts::ObjectStatusTimerRuntimeMetrics *baseline,
	const rts::ObjectStatusTimerRuntimeMetrics &current,
	rts::ObjectStatusTimerRuntimeMetrics *frozen,
	Bool *awaitingInitialReset)
{
	if (baseline == nullptr || frozen == nullptr || awaitingInitialReset == nullptr)
		return;
	if (*awaitingInitialReset)
	{
		if (current.resetEpoch == baseline->resetEpoch)
			return;
		*baseline = rts::ObjectStatusTimerRuntimeMetrics();
		baseline->resetEpoch = current.resetEpoch;
		*awaitingInitialReset = FALSE;
	}
	else if (current.resetEpoch != baseline->resetEpoch)
	{
		return;
	}
#define CAPTURE_STATUS_COUNTER(member) \
	frozen->member = current.member - baseline->member
	CAPTURE_STATUS_COUNTER(authoritativeBatches);
	CAPTURE_STATUS_COUNTER(committedCommands);
	CAPTURE_STATUS_COUNTER(submittedJobs);
	CAPTURE_STATUS_COUNTER(completedJobs);
	CAPTURE_STATUS_COUNTER(physicalWorkerJobs);
	CAPTURE_STATUS_COUNTER(ownerHelpedJobs);
	frozen->physicalWorkerMask = current.physicalWorkerMask;
	if (!current.physicalWorkerMaskComplete)
		frozen->physicalWorkerMaskComplete = false;
	if (current.maximumDistinctPhysicalWorkers >
		frozen->maximumDistinctPhysicalWorkers)
		frozen->maximumDistinctPhysicalWorkers =
			current.maximumDistinctPhysicalWorkers;
	if (current.maximumPeakConcurrentPhysicalWorkers >
		frozen->maximumPeakConcurrentPhysicalWorkers)
		frozen->maximumPeakConcurrentPhysicalWorkers =
			current.maximumPeakConcurrentPhysicalWorkers;
	CAPTURE_STATUS_COUNTER(shadowExecutions);
	CAPTURE_STATUS_COUNTER(shadowCommands);
	CAPTURE_STATUS_COUNTER(shadowMatches);
	CAPTURE_STATUS_COUNTER(shadowMismatches);
	CAPTURE_STATUS_COUNTER(ownerFallbacks);
	CAPTURE_STATUS_COUNTER(staleRejections);
#undef CAPTURE_STATUS_COUNTER
	frozen->resetEpoch = baseline->resetEpoch;
}

void AccumulateSkirmishAITestImmutableSpatialMetrics(
	rts::ImmutableSpatialRuntimeMetrics *baseline,
	const rts::ImmutableSpatialRuntimeMetrics &current,
	rts::ImmutableSpatialRuntimeMetrics *frozen,
	Bool *awaitingInitialReset)
{
	if (baseline == nullptr || frozen == nullptr || awaitingInitialReset == nullptr)
		return;
	if (*awaitingInitialReset)
	{
		if (current.resetEpoch == baseline->resetEpoch)
			return;
		*baseline = rts::ImmutableSpatialRuntimeMetrics();
		baseline->resetEpoch = current.resetEpoch;
		*awaitingInitialReset = FALSE;
	}
	else if (current.resetEpoch != baseline->resetEpoch)
	{
		return;
	}
#define CAPTURE_SPATIAL_COUNTER(member) \
	frozen->member = current.member - baseline->member
#define CAPTURE_SPATIAL_CONSUMER(consumer, member) \
	frozen->consumer.member = current.consumer.member - baseline->consumer.member
	CAPTURE_SPATIAL_COUNTER(capturedArenas);
	CAPTURE_SPATIAL_COUNTER(captureFailures);
	CAPTURE_SPATIAL_COUNTER(successfulCollections);
	CAPTURE_SPATIAL_COUNTER(successfulCollectionQueries);
	CAPTURE_SPATIAL_COUNTER(successfulCollectionRanges);
	CAPTURE_SPATIAL_COUNTER(multiRangeCollections);
	CAPTURE_SPATIAL_COUNTER(collectionSubmittedJobs);
	CAPTURE_SPATIAL_COUNTER(collectionCompletedJobs);
	CAPTURE_SPATIAL_COUNTER(collectionPhysicalWorkerJobs);
	CAPTURE_SPATIAL_COUNTER(collectionOwnerHelpedJobs);
	frozen->collectionPhysicalWorkerMask |=
		current.collectionPhysicalWorkerMask &
		~baseline->collectionPhysicalWorkerMask;
	if (current.maximumCollectionQueries > frozen->maximumCollectionQueries)
		frozen->maximumCollectionQueries = current.maximumCollectionQueries;
	if (current.maximumCollectionRanges > frozen->maximumCollectionRanges)
		frozen->maximumCollectionRanges = current.maximumCollectionRanges;
	if (current.maximumCollectionDistinctPhysicalWorkers >
		frozen->maximumCollectionDistinctPhysicalWorkers)
	{
		frozen->maximumCollectionDistinctPhysicalWorkers =
			current.maximumCollectionDistinctPhysicalWorkers;
	}
#define CAPTURE_SPATIAL_CONSUMER_COUNTERS(consumer) \
	CAPTURE_SPATIAL_CONSUMER(consumer, eligibleQueries); \
	CAPTURE_SPATIAL_CONSUMER(consumer, authoritativeQueries); \
	CAPTURE_SPATIAL_CONSUMER(consumer, authoritativeCandidates); \
	CAPTURE_SPATIAL_CONSUMER(consumer, shadowQueries); \
	CAPTURE_SPATIAL_CONSUMER(consumer, shadowMatches); \
	CAPTURE_SPATIAL_CONSUMER(consumer, shadowMismatches); \
	CAPTURE_SPATIAL_CONSUMER(consumer, submittedJobs); \
	CAPTURE_SPATIAL_CONSUMER(consumer, completedJobs); \
	CAPTURE_SPATIAL_CONSUMER(consumer, physicalWorkerJobs); \
	CAPTURE_SPATIAL_CONSUMER(consumer, ownerHelpedJobs); \
	CAPTURE_SPATIAL_CONSUMER(consumer, expectedFallbacks); \
	CAPTURE_SPATIAL_CONSUMER(consumer, unexpectedFallbacks); \
	CAPTURE_SPATIAL_CONSUMER(consumer, staleRejections); \
	CAPTURE_SPATIAL_CONSUMER(consumer, validationFailures); \
	CAPTURE_SPATIAL_CONSUMER(consumer, circuitBreakerTrips)
	CAPTURE_SPATIAL_CONSUMER_COUNTERS(healing);
	CAPTURE_SPATIAL_CONSUMER_COUNTERS(pointDefenseLaser);
#undef CAPTURE_SPATIAL_CONSUMER_COUNTERS
#undef CAPTURE_SPATIAL_CONSUMER
#undef CAPTURE_SPATIAL_COUNTER
	frozen->resetEpoch = baseline->resetEpoch;
}
#endif

namespace
{

void CaptureSkirmishAITestSliceMetrics()
{
	const DirectPathRuntimeMetrics currentPath = GetDirectPathRuntimeMetrics();
	AccumulateSkirmishAITestDirectPathMetrics(&s_directPathMetricsAtStart,
		currentPath, &s_directPathMetricsFrozen,
		&s_directPathMetricsHaveFrozenActivity,
		&s_directPathMetricsAwaitingInitialReset);
	const OrdinaryPathRuntimeMetrics currentOrdinaryPath =
		GetOrdinaryPathRuntimeMetrics();
	AccumulateSkirmishAITestOrdinaryPathMetrics(
		&s_ordinaryPathMetricsAtStart, currentOrdinaryPath,
		&s_ordinaryPathMetricsFrozen,
		&s_ordinaryPathMetricsAwaitingInitialReset);
#if defined(_WIN64)
	const rts::CollisionCandidateRuntimeMetrics currentCollision =
		rts::GetCollisionCandidateRuntimeMetrics();
	AccumulateSkirmishAITestCollisionMetrics(&s_collisionMetricsAtStart,
		currentCollision, &s_collisionMetricsFrozen,
		&s_collisionMetricsAwaitingInitialReset);
	const rts::PhysicsIntegrationRuntimeMetrics currentPhysics =
		rts::GetPhysicsIntegrationRuntimeMetrics();
	AccumulateSkirmishAITestPhysicsMetrics(&s_physicsMetricsAtStart,
		currentPhysics, &s_physicsMetricsFrozen,
		&s_physicsMetricsAwaitingInitialReset);
	const rts::ObjectStatusTimerRuntimeMetrics currentStatus =
		rts::GetObjectStatusTimerRuntimeMetrics();
	AccumulateSkirmishAITestObjectStatusTimerMetrics(&s_statusMetricsAtStart,
		currentStatus, &s_statusMetricsFrozen,
		&s_statusMetricsAwaitingInitialReset);
	const rts::ImmutableSpatialRuntimeMetrics currentSpatial =
		rts::GetImmutableSpatialRuntimeMetrics();
	AccumulateSkirmishAITestImmutableSpatialMetrics(&s_spatialMetricsAtStart,
		currentSpatial, &s_spatialMetricsFrozen,
		&s_spatialMetricsAwaitingInitialReset);
#endif
}

void PrintJobMetric(const char *name, rts::JobMetricCounter value)
{
#if defined(_MSC_VER) && _MSC_VER < 1300
	printf(" %s=%I64u", name, static_cast<unsigned __int64>(value));
#else
	printf(" %s=%llu", name, static_cast<unsigned long long>(value));
#endif
}

void PrintSkirmishAITestManifest()
{
	CaptureSkirmishAITestRuntimeState();
	const rts::JobSystemMetrics metrics = rts::JobSystem::instance().metrics();
	rts::LiveSimulationPhaseRuntimeMetrics phase;
	phase.attemptedFrames = JobMetricDelta(
		s_phaseMetricsLast.attemptedFrames, s_phaseMetricsAtStart.attemptedFrames);
	phase.completedFrames = JobMetricDelta(
		s_phaseMetricsLast.completedFrames, s_phaseMetricsAtStart.completedFrames);
	phase.stableSequenceFrames = JobMetricDelta(
		s_phaseMetricsLast.stableSequenceFrames,
		s_phaseMetricsAtStart.stableSequenceFrames);
	phase.stoppedByOwnerFrames = JobMetricDelta(
		s_phaseMetricsLast.stoppedByOwnerFrames,
		s_phaseMetricsAtStart.stoppedByOwnerFrames);
	phase.fallbackBeforeMutationFrames = JobMetricDelta(
		s_phaseMetricsLast.fallbackBeforeMutationFrames,
		s_phaseMetricsAtStart.fallbackBeforeMutationFrames);
	phase.failedAfterMutationFrames = JobMetricDelta(
		s_phaseMetricsLast.failedAfterMutationFrames,
		s_phaseMetricsAtStart.failedAfterMutationFrames);
	phase.committedPhases = JobMetricDelta(
		s_phaseMetricsLast.committedPhases, s_phaseMetricsAtStart.committedPhases);
	phase.sequenceViolationFrames = JobMetricDelta(
		s_phaseMetricsLast.sequenceViolationFrames,
		s_phaseMetricsAtStart.sequenceViolationFrames);
	phase.lastFrame = s_phaseMetricsLast.lastFrame;
	phase.lastGeneration = s_phaseMetricsLast.lastGeneration;
	phase.lastCommittedPhaseCount =
		s_phaseMetricsLast.lastCommittedPhaseCount;
	phase.lastSequenceSignature = s_phaseMetricsLast.lastSequenceSignature;
	UnsignedInt phaseOrdinal;
	for (phaseOrdinal = 0;
		phaseOrdinal < rts::LIVE_SIMULATION_PHASE_COUNT - 1; ++phaseOrdinal)
	{
		phase.ownerPhaseTotalNanoseconds[phaseOrdinal] = JobMetricDelta(
			s_phaseMetricsLast.ownerPhaseTotalNanoseconds[phaseOrdinal],
			s_phaseMetricsAtStart.ownerPhaseTotalNanoseconds[phaseOrdinal]);
		phase.ownerPhaseMaximumNanoseconds[phaseOrdinal] =
			s_phaseMetricsLast.ownerPhaseMaximumNanoseconds[phaseOrdinal];
		phase.ownerPhaseSampleCount[phaseOrdinal] = JobMetricDelta(
			s_phaseMetricsLast.ownerPhaseSampleCount[phaseOrdinal],
			s_phaseMetricsAtStart.ownerPhaseSampleCount[phaseOrdinal]);
	}
	phase.frameSimulationTotalNanoseconds = JobMetricDelta(
		s_phaseMetricsLast.frameSimulationTotalNanoseconds,
		s_phaseMetricsAtStart.frameSimulationTotalNanoseconds);
	phase.frameSimulationMaximumNanoseconds =
		s_phaseMetricsLast.frameSimulationMaximumNanoseconds;
	phase.frameSimulationSampleCount = JobMetricDelta(
		s_phaseMetricsLast.frameSimulationSampleCount,
		s_phaseMetricsAtStart.frameSimulationSampleCount);
	phase.serialIslandTotalNanoseconds = JobMetricDelta(
		s_phaseMetricsLast.serialIslandTotalNanoseconds,
		s_phaseMetricsAtStart.serialIslandTotalNanoseconds);
	phase.serialIslandMaximumNanoseconds =
		s_phaseMetricsLast.serialIslandMaximumNanoseconds;
	phase.serialIslandSampleCount = JobMetricDelta(
		s_phaseMetricsLast.serialIslandSampleCount,
		s_phaseMetricsAtStart.serialIslandSampleCount);
	const Bool stablePhaseEvidence =
		rts::HasStableLiveSimulationPhaseEvidence(phase) ? TRUE : FALSE;
	// The recorder publishes end_frame as the winning frame index and validates
	// an inclusive frame count of end_frame + 1. Use the same contract here.
	const UnsignedInt expectedPhaseFrames = s_runner.endFrame < UINT_MAX ?
		s_runner.endFrame + 1U : 0;
	const Bool completePhaseCoverage = stablePhaseEvidence &&
		phase.attemptedFrames == expectedPhaseFrames;
	const Bool releasePhaseWorkerCount =
		rts::IsLiveSimulationPhaseReleaseWorkerCount(
			s_runner.effectiveWorkerCount) ? TRUE : FALSE;
	const Bool amdahlOneWorkerEvidence = completePhaseCoverage &&
		s_runner.effectiveWorkerCount == 1 &&
		phase.frameSimulationTotalNanoseconds != 0 &&
		phase.serialIslandTotalNanoseconds <=
			phase.frameSimulationTotalNanoseconds;
	rts::JobMetricCounter aiCapturedSnapshots = 0;
	rts::JobMetricCounter aiCapturedCandidates = 0;
	rts::JobMetricCounter aiRequestedBatches = 0;
	rts::JobMetricCounter aiSubmittedJobs = 0;
	rts::JobMetricCounter aiCompletedJobs = 0;
	rts::JobMetricCounter aiSerialFallbacks = 0;
	rts::JobMetricCounter aiShadowMatches = 0;
	rts::JobMetricCounter aiShadowMismatches = 0;
	rts::JobMetricCounter aiValidationFailures = 0;
	rts::JobMetricCounter aiCommittedBatches = 0;
	rts::JobMetricCounter aiParallelAuthoritativeCommits = 0;
	rts::JobMetricCounter aiRejectedCommits = 0;
	rts::JobMetricCounter collisionAuthoritativeCommits = 0;
	rts::JobMetricCounter collisionShadowExecutions = 0;
	rts::JobMetricCounter collisionShadowComparedCandidates = 0;
	rts::JobMetricCounter collisionShadowMismatches = 0;
	rts::JobMetricCounter collisionOwnerFallbacks = 0;
	rts::JobMetricCounter collisionUnexpectedFallbacks = 0;
	rts::JobMetricCounter collisionIneligibleSlices = 0;
	rts::JobMetricCounter collisionStaleRejections = 0;
	rts::JobMetricCounter collisionCommittedCandidates = 0;
	rts::JobMetricCounter collisionPreparedPairs = 0;
	rts::JobMetricCounter collisionUniqueCandidates = 0;
	rts::JobMetricCounter collisionSubmittedJobs = 0;
	rts::JobMetricCounter collisionCompletedJobs = 0;
	rts::JobMetricCounter collisionPhysicalWorkerJobs = 0;
	rts::JobMetricCounter collisionOwnerHelpedJobs = 0;
	rts::JobMetricCounter collisionPhysicalWorkerMask = 0;
	UnsignedInt collisionDistinctPhysicalWorkers = 0;
	const DirectPathRuntimeMetrics &path = s_directPathMetricsFrozen;
	const OrdinaryPathRuntimeMetrics &ordinaryPath =
		s_ordinaryPathMetricsFrozen;
#if defined(_WIN64)
	const rts::AIPlanningRuntimeMetrics ai = rts::GetAIPlanningRuntimeMetrics();
	aiCapturedSnapshots = JobMetricDelta(ai.capturedSnapshots,
		s_aiPlanningMetricsAtStart.capturedSnapshots);
	aiCapturedCandidates = JobMetricDelta(ai.capturedCandidates,
		s_aiPlanningMetricsAtStart.capturedCandidates);
	aiRequestedBatches = JobMetricDelta(ai.requestedBatches,
		s_aiPlanningMetricsAtStart.requestedBatches);
	aiSubmittedJobs = JobMetricDelta(ai.submittedJobs,
		s_aiPlanningMetricsAtStart.submittedJobs);
	aiCompletedJobs = JobMetricDelta(ai.completedJobs,
		s_aiPlanningMetricsAtStart.completedJobs);
	aiSerialFallbacks = JobMetricDelta(ai.serialFallbacks,
		s_aiPlanningMetricsAtStart.serialFallbacks);
	aiShadowMatches = JobMetricDelta(ai.shadowMatches,
		s_aiPlanningMetricsAtStart.shadowMatches);
	aiShadowMismatches = JobMetricDelta(ai.shadowMismatches,
		s_aiPlanningMetricsAtStart.shadowMismatches);
	aiValidationFailures = JobMetricDelta(ai.validationFailures,
		s_aiPlanningMetricsAtStart.validationFailures);
	aiCommittedBatches = JobMetricDelta(ai.committedBatches,
		s_aiPlanningMetricsAtStart.committedBatches);
	aiParallelAuthoritativeCommits = JobMetricDelta(
		ai.parallelAuthoritativeCommits,
		s_aiPlanningMetricsAtStart.parallelAuthoritativeCommits);
	aiRejectedCommits = JobMetricDelta(ai.rejectedCommits,
		s_aiPlanningMetricsAtStart.rejectedCommits);
	const rts::CollisionCandidateRuntimeMetrics &collision =
		s_collisionMetricsFrozen;
	collisionAuthoritativeCommits = collision.authoritativeCommits;
	collisionShadowExecutions = collision.shadowExecutions;
	collisionShadowComparedCandidates = collision.shadowComparedCandidates;
	collisionShadowMismatches = collision.shadowMismatches;
	collisionOwnerFallbacks = collision.ownerFallbacks;
	collisionUnexpectedFallbacks = collision.unexpectedFallbacks;
	collisionIneligibleSlices = collision.ineligibleSlices;
	collisionStaleRejections = collision.staleRejections;
	collisionCommittedCandidates = collision.committedCandidates;
	collisionPreparedPairs = collision.preparedPairs;
	collisionUniqueCandidates = collision.uniqueCandidates;
	collisionSubmittedJobs = collision.submittedJobs;
	collisionCompletedJobs = collision.completedJobs;
	collisionPhysicalWorkerJobs = collision.physicalWorkerJobs;
	collisionOwnerHelpedJobs = collision.ownerHelpedJobs;
	collisionPhysicalWorkerMask = collision.physicalWorkerMask;
	collisionDistinctPhysicalWorkers = collision.distinctPhysicalWorkers;
#endif
	char requestedWorkers[16];
	if (s_runner.requestedWorkerCount == 0)
		strlcpy(requestedWorkers, "auto", ARRAY_SIZE(requestedWorkers));
	else
		_snprintf(requestedWorkers, ARRAY_SIZE(requestedWorkers), "%u", s_runner.requestedWorkerCount);
	requestedWorkers[ARRAY_SIZE(requestedWorkers) - 1] = '\0';
	char finalDigest[16];
	if (s_finalDigestAvailable)
		_snprintf(finalDigest, ARRAY_SIZE(finalDigest), "%08X", s_finalDigest);
	else
		strlcpy(finalDigest, "unavailable", ARRAY_SIZE(finalDigest));
	finalDigest[ARRAY_SIZE(finalDigest) - 1] = '\0';
	const char *executableHash = s_executableHashObserved;
	const char *executableHashOrigin = "module";
	if (strcmp(executableHash, "unavailable") == 0)
	{
		executableHash = s_executableHashInput;
		executableHashOrigin = strcmp(s_executableHashInput, "unavailable") == 0
			? "unavailable" : "input";
	}

	printf(" executable_sha256=%s simulation_mode=%s requested_pipeline=%s effective_pipeline=%s "
		"requested_simulation=%s effective_simulation=%s requested_workers=%s effective_workers=%u "
		"worker_policy=%s final_digest=%s wall_ms=%u run_nonce=%s replay_epoch=%d "
		"replay_sha256=%s replay_retained=\"%s\" outcome=winner_team_%d "
		"executable_sha256_origin=%s",
		executableHash, s_simulationModeInput,
		SkirmishAITestPipelineModeName(s_runner.requestedPipelineMode),
		SkirmishAITestPipelineModeName(rts::GetPipelineExecutionMode()),
		SkirmishAITestSimulationModeName(s_runner.requestedSimulationMode),
		SkirmishAITestSimulationModeName(rts::GetSimulationExecutionMode()), requestedWorkers,
		s_runner.effectiveWorkerCount,
		s_runner.workerPolicy == rts::JOB_WORKER_POLICY_ALL ? "all" : "auto",
		finalDigest, ElapsedMilliseconds(s_runner.startupStartMilliseconds, GetTickCount()),
		s_runner.runNonce, s_runner.replayEpoch,
		s_runner.replaySha256[0] != '\0' ? s_runner.replaySha256 : "unavailable",
		s_runner.retainedReplayPath[0] != '\0' ? s_runner.retainedReplayPath : "unavailable",
		s_runner.winnerTeam, executableHashOrigin);
	PrintJobMetric("job_submitted", JobMetricDelta(metrics.submittedJobCount,
		s_jobMetricsAtStart.submittedJobCount));
	PrintJobMetric("job_executed", JobMetricDelta(metrics.executedJobCount,
		s_jobMetricsAtStart.executedJobCount));
	PrintJobMetric("job_steals", JobMetricDelta(metrics.stealCount, s_jobMetricsAtStart.stealCount));
	PrintJobMetric("job_owner_help", JobMetricDelta(metrics.ownerHelpCount,
		s_jobMetricsAtStart.ownerHelpCount));
	PrintJobMetric("job_waits", JobMetricDelta(metrics.waitCount, s_jobMetricsAtStart.waitCount));
	PrintJobMetric("job_worker_wait_reject", JobMetricDelta(metrics.workerWaitRejectionCount,
		s_jobMetricsAtStart.workerWaitRejectionCount));
	PrintJobMetric("job_failed", JobMetricDelta(metrics.failedJobCount, s_jobMetricsAtStart.failedJobCount));
	PrintJobMetric("job_cancelled", JobMetricDelta(metrics.cancelledJobCount,
		s_jobMetricsAtStart.cancelledJobCount));
	PrintJobMetric("job_fallback", JobMetricDelta(metrics.serialFallbackCount,
		s_jobMetricsAtStart.serialFallbackCount));
	PrintJobMetric("job_queue_latency_ns", JobMetricDelta(metrics.totalQueueLatencyNanoseconds,
		s_jobMetricsAtStart.totalQueueLatencyNanoseconds));
	PrintJobMetric("job_max_queue_latency_ns", metrics.maximumQueueLatencyNanoseconds);
	PrintJobMetric("job_sleeps", JobMetricDelta(metrics.workerSleepCount,
		s_jobMetricsAtStart.workerSleepCount));
	PrintJobMetric("job_wakes", JobMetricDelta(metrics.workerWakeCount,
		s_jobMetricsAtStart.workerWakeCount));
	printf(" scheduler_perf_schema=%u scheduler_perf_unit=ns",
		rts::JOB_SYSTEM_PERFORMANCE_SCHEMA_VERSION);
	PrintJobMetric("job_worker_busy_total_ns", JobMetricDelta(
		metrics.workerBusyNanoseconds,
		s_jobMetricsAtStart.workerBusyNanoseconds));
	PrintJobMetric("job_worker_busy_max_ns",
		metrics.maximumWorkerBusyNanoseconds);
	PrintJobMetric("job_worker_busy_samples", JobMetricDelta(
		metrics.workerBusySampleCount,
		s_jobMetricsAtStart.workerBusySampleCount));
	PrintJobMetric("job_worker_wait_total_ns", JobMetricDelta(
		metrics.workerWaitNanoseconds,
		s_jobMetricsAtStart.workerWaitNanoseconds));
	PrintJobMetric("job_worker_wait_max_ns",
		metrics.maximumWorkerWaitNanoseconds);
	PrintJobMetric("job_worker_wait_samples", JobMetricDelta(
		metrics.workerWaitSampleCount,
		s_jobMetricsAtStart.workerWaitSampleCount));
	PrintJobMetric("job_affinity_failures", JobMetricDelta(metrics.affinityFailureCount,
		s_jobMetricsAtStart.affinityFailureCount));
	PrintJobMetric("phase_attempted_frames", phase.attemptedFrames);
	PrintJobMetric("phase_completed_frames", phase.completedFrames);
	PrintJobMetric("phase_stable_sequence_frames", phase.stableSequenceFrames);
	PrintJobMetric("phase_stopped_frames", phase.stoppedByOwnerFrames);
	PrintJobMetric("phase_fallback_before_mutation",
		phase.fallbackBeforeMutationFrames);
	PrintJobMetric("phase_failed_after_mutation",
		phase.failedAfterMutationFrames);
	PrintJobMetric("phase_committed_phases", phase.committedPhases);
	PrintJobMetric("phase_sequence_violations", phase.sequenceViolationFrames);
	printf(" phase_perf_schema=%u phase_perf_unit=ns phase_perf_crc_excluded=1",
		rts::LIVE_SIMULATION_PHASE_PERFORMANCE_SCHEMA_VERSION);
	static const char *phasePerformanceNames[
		rts::LIVE_SIMULATION_PHASE_COUNT - 1] = {
		"owner_intake", "legacy_mutable_island", "spatial", "owner_tail",
		"verification_publication"
	};
	for (phaseOrdinal = 0;
		phaseOrdinal < rts::LIVE_SIMULATION_PHASE_COUNT - 1; ++phaseOrdinal)
	{
		char fieldName[96];
		_snprintf(fieldName, ARRAY_SIZE(fieldName), "phase_%s_total_ns",
			phasePerformanceNames[phaseOrdinal]);
		fieldName[ARRAY_SIZE(fieldName) - 1] = '\0';
		PrintJobMetric(fieldName,
			phase.ownerPhaseTotalNanoseconds[phaseOrdinal]);
		_snprintf(fieldName, ARRAY_SIZE(fieldName), "phase_%s_max_ns",
			phasePerformanceNames[phaseOrdinal]);
		fieldName[ARRAY_SIZE(fieldName) - 1] = '\0';
		PrintJobMetric(fieldName,
			phase.ownerPhaseMaximumNanoseconds[phaseOrdinal]);
		_snprintf(fieldName, ARRAY_SIZE(fieldName), "phase_%s_samples",
			phasePerformanceNames[phaseOrdinal]);
		fieldName[ARRAY_SIZE(fieldName) - 1] = '\0';
		PrintJobMetric(fieldName,
			phase.ownerPhaseSampleCount[phaseOrdinal]);
	}
	PrintJobMetric("simulation_frame_total_ns",
		phase.frameSimulationTotalNanoseconds);
	PrintJobMetric("simulation_frame_max_ns",
		phase.frameSimulationMaximumNanoseconds);
	PrintJobMetric("simulation_frame_samples",
		phase.frameSimulationSampleCount);
	PrintJobMetric("simulation_serial_island_total_ns",
		phase.serialIslandTotalNanoseconds);
	PrintJobMetric("simulation_serial_island_max_ns",
		phase.serialIslandMaximumNanoseconds);
	PrintJobMetric("simulation_serial_island_samples",
		phase.serialIslandSampleCount);
	printf(" amdahl_one_worker_eligible=%u", amdahlOneWorkerEvidence);
	PrintJobMetric("amdahl_one_worker_frame_total_ns",
		amdahlOneWorkerEvidence ? phase.frameSimulationTotalNanoseconds : 0);
	PrintJobMetric("amdahl_one_worker_serial_total_ns",
		amdahlOneWorkerEvidence ? phase.serialIslandTotalNanoseconds : 0);
	PrintJobMetric("amdahl_one_worker_frame_samples",
		amdahlOneWorkerEvidence ? phase.frameSimulationSampleCount : 0);
	printf(" phase_expected_frames=%u phase_last_frame=%u phase_last_generation=%u "
		"phase_last_committed_phases=%u phase_last_sequence=%u "
		"phase_evidence_stable=%u phase_matches_end_frame=%u "
		"phase_release_worker_count_supported=%u",
		expectedPhaseFrames, phase.lastFrame, phase.lastGeneration,
		phase.lastCommittedPhaseCount,
		phase.lastSequenceSignature, stablePhaseEvidence,
		completePhaseCoverage, releasePhaseWorkerCount);
	// These slice-specific counters prove that authoritative AI planning work,
	// rather than an unrelated JobSystem consumer or duplicate shadow path job,
	// reached the owner-thread commit boundary during the live match.
	PrintJobMetric("authoritative_commits", aiParallelAuthoritativeCommits);
	PrintJobMetric("shadow_executions", aiShadowMatches + aiShadowMismatches);
	PrintJobMetric("owner_fallbacks", aiSerialFallbacks);
	PrintJobMetric("ai_captured_snapshots", aiCapturedSnapshots);
	PrintJobMetric("ai_captured_candidates", aiCapturedCandidates);
	PrintJobMetric("ai_requested_batches", aiRequestedBatches);
	PrintJobMetric("ai_submitted_jobs", aiSubmittedJobs);
	PrintJobMetric("ai_completed_jobs", aiCompletedJobs);
	PrintJobMetric("ai_serial_fallbacks", aiSerialFallbacks);
	PrintJobMetric("ai_shadow_matches", aiShadowMatches);
	PrintJobMetric("ai_shadow_mismatches", aiShadowMismatches);
	PrintJobMetric("ai_validation_failures", aiValidationFailures);
	PrintJobMetric("ai_committed_batches", aiCommittedBatches);
	PrintJobMetric("ai_parallel_authoritative_commits",
		aiParallelAuthoritativeCommits);
	PrintJobMetric("ai_rejected_commits", aiRejectedCommits);
	PrintJobMetric("direct_eligible", path.eligibleRequests);
	PrintJobMetric("direct_submitted", path.submittedJobs);
	PrintJobMetric("direct_executed", path.executedJobs);
	PrintJobMetric("direct_worker_executed", path.workerExecutedJobs);
	PrintJobMetric("direct_owner_helped", path.ownerHelpedJobs);
	PrintJobMetric("direct_authoritative_commits", path.authoritativeCommits);
	PrintJobMetric("direct_authoritative_multiworker_commits",
		path.authoritativeMultiWorkerCommits);
	PrintJobMetric("direct_stale_rejections", path.staleRejections);
	PrintJobMetric("direct_validation_failures", path.validationFailures);
	PrintJobMetric("direct_serial_fallbacks", path.serialFallbacks);
	PrintJobMetric("direct_unsupported_authority",
		path.unsupportedAuthoritativeCommits);
	PrintJobMetric("direct_shadow_authority", path.shadowAuthoritativeCommits);
	PrintJobMetric("direct_stale_acceptance", path.staleAuthoritativeCommits);
	PrintJobMetric("direct_malformed_acceptance",
		path.malformedAuthoritativeCommits);
	PrintJobMetric("direct_shadow_only", path.shadowOnlyExecutions);
	PrintJobMetric("direct_timeouts", path.timeoutCancellations);
	PrintJobMetric("direct_late_drains", path.lateDrainExecutions);
	PrintJobMetric("direct_peak_active_workers", path.peakActiveWorkers);
	PrintJobMetric("direct_callback_min", path.minimumCallbackCount);
	PrintJobMetric("direct_callback_max", path.maximumCallbackCount);
	PrintJobMetric("ordinary_path_eligible", ordinaryPath.eligibleRequests);
	PrintJobMetric("ordinary_path_submitted_requests",
		ordinaryPath.submittedRequests);
	PrintJobMetric("ordinary_path_submitted_ranges",
		ordinaryPath.submittedRangeJobs);
	PrintJobMetric("ordinary_path_worker_executed_requests",
		ordinaryPath.workerExecutedRequests);
	PrintJobMetric("ordinary_path_worker_executed_range_jobs",
		ordinaryPath.workerExecutedRangeJobs);
	PrintJobMetric("ordinary_path_owner_helped_range_jobs",
		ordinaryPath.ownerHelpedRangeJobs);
	PrintJobMetric("ordinary_path_failed_range_jobs",
		ordinaryPath.failedRangeJobs);
	PrintJobMetric("ordinary_path_physical_worker_mask",
		ordinaryPath.physicalWorkerMask);
	PrintJobMetric("ordinary_path_distinct_physical_workers",
		ordinaryPath.distinctPhysicalWorkers);
	PrintJobMetric("ordinary_path_physical_worker_mask_complete",
		ordinaryPath.physicalWorkerMaskComplete ? 1 : 0);
	PrintJobMetric("ordinary_path_authoritative_commits",
		ordinaryPath.authoritativeCommits);
	PrintJobMetric("ordinary_path_authoritative_multiworker_commits",
		ordinaryPath.authoritativeMultiWorkerCommits);
	PrintJobMetric("ordinary_path_stale_rejections",
		ordinaryPath.staleRejections);
	PrintJobMetric("ordinary_path_validation_failures",
		ordinaryPath.validationFailures);
	PrintJobMetric("ordinary_path_serial_fallbacks",
		ordinaryPath.serialFallbacks);
	PrintJobMetric("ordinary_path_shadow_comparisons",
		ordinaryPath.shadowComparisons);
	PrintJobMetric("ordinary_path_shadow_mismatches",
		ordinaryPath.shadowMismatches);
	PrintJobMetric("ordinary_path_timeouts",
		ordinaryPath.timeoutCancellations);
	PrintJobMetric("ordinary_path_late_drains",
		ordinaryPath.lateDrainExecutions);
	PrintJobMetric("ordinary_path_peak_active_workers",
		ordinaryPath.peakActiveWorkers);
	PrintJobMetric("ordinary_path_max_batch_requests",
		ordinaryPath.maximumBatchRequests);
	PrintJobMetric("ordinary_path_max_range_count",
		ordinaryPath.maximumRangeCount);
	PrintJobMetric("ordinary_path_max_grain_size",
		ordinaryPath.maximumGrainSize);
	PrintJobMetric("collision_authoritative_commits",
		collisionAuthoritativeCommits);
	PrintJobMetric("collision_shadow_executions", collisionShadowExecutions);
	PrintJobMetric("collision_shadow_compared_candidates",
		collisionShadowComparedCandidates);
	PrintJobMetric("collision_shadow_mismatches", collisionShadowMismatches);
	PrintJobMetric("collision_owner_fallbacks", collisionOwnerFallbacks);
	PrintJobMetric("collision_unexpected_fallbacks",
		collisionUnexpectedFallbacks);
	PrintJobMetric("collision_ineligible_slices", collisionIneligibleSlices);
	PrintJobMetric("collision_stale_rejections", collisionStaleRejections);
	PrintJobMetric("collision_committed_candidates",
		collisionCommittedCandidates);
	PrintJobMetric("collision_prepared_pairs", collisionPreparedPairs);
	PrintJobMetric("collision_unique_candidates", collisionUniqueCandidates);
	PrintJobMetric("collision_submitted_jobs", collisionSubmittedJobs);
	PrintJobMetric("collision_completed_jobs", collisionCompletedJobs);
	PrintJobMetric("collision_physical_worker_jobs",
		collisionPhysicalWorkerJobs);
	PrintJobMetric("collision_owner_helped_jobs", collisionOwnerHelpedJobs);
	PrintJobMetric("collision_physical_worker_mask",
		collisionPhysicalWorkerMask);
	PrintJobMetric("collision_distinct_physical_workers",
		collisionDistinctPhysicalWorkers);
	#if defined(_WIN64)
	PrintJobMetric("collision_physical_worker_mask_complete",
		collision.physicalWorkerMaskComplete ? 1 : 0);
	#else
	PrintJobMetric("collision_physical_worker_mask_complete", 0);
	#endif
#if defined(_WIN64)
	PrintJobMetric("physics_authoritative_batches",
		s_physicsMetricsFrozen.acceptedBatches);
	PrintJobMetric("physics_committed_prefixes",
		s_physicsMetricsFrozen.acceptedPrefixes);
	PrintJobMetric("physics_ranges", s_physicsMetricsFrozen.acceptedRanges);
	PrintJobMetric("physics_submitted_jobs",
		s_physicsMetricsFrozen.acceptedSubmittedJobs);
	PrintJobMetric("physics_completed_jobs",
		s_physicsMetricsFrozen.acceptedCompletedJobs);
	PrintJobMetric("physics_physical_worker_jobs",
		s_physicsMetricsFrozen.acceptedPhysicalWorkerJobs);
	PrintJobMetric("physics_owner_helped_jobs",
		s_physicsMetricsFrozen.acceptedOwnerHelpedJobs);
	PrintJobMetric("physics_physical_worker_mask",
		s_physicsMetricsFrozen.acceptedPhysicalWorkerMask);
	PrintJobMetric("physics_distinct_physical_workers",
		s_physicsMetricsFrozen.maximumAcceptedDistinctPhysicalWorkers);
	PrintJobMetric("physics_physical_worker_mask_complete",
		s_physicsMetricsFrozen.acceptedPhysicalWorkerMaskComplete ? 1 : 0);
	PrintJobMetric("physics_peak_concurrent_physical_workers",
		s_physicsMetricsFrozen.maximumAcceptedPeakConcurrentPhysicalWorkers);
	PrintJobMetric("physics_allocated_bytes",
		s_physicsMetricsFrozen.acceptedAllocatedBytes);
	PrintJobMetric("physics_capture_ns",
		s_physicsMetricsFrozen.acceptedCaptureNanoseconds);
	PrintJobMetric("physics_prepare_ns",
		s_physicsMetricsFrozen.acceptedPrepareNanoseconds);
	PrintJobMetric("physics_wait_ns",
		s_physicsMetricsFrozen.acceptedWaitNanoseconds);
	PrintJobMetric("physics_commit_ns",
		s_physicsMetricsFrozen.acceptedCommitNanoseconds);
	PrintJobMetric("physics_storage_bytes",
		s_physicsMetricsFrozen.acceptedStorageBytes);
	PrintJobMetric("physics_storage_capacity_bytes",
		s_physicsMetricsFrozen.acceptedStorageCapacityBytes);
	PrintJobMetric("physics_storage_allocations",
		s_physicsMetricsFrozen.acceptedStorageAllocations);
	PrintJobMetric("physics_shadow_executions", s_physicsMetricsFrozen.shadowBatches);
	PrintJobMetric("physics_shadow_prefixes", s_physicsMetricsFrozen.shadowPrefixes);
	PrintJobMetric("physics_shadow_ranges", s_physicsMetricsFrozen.shadowRanges);
	PrintJobMetric("physics_shadow_submitted_jobs",
		s_physicsMetricsFrozen.shadowSubmittedJobs);
	PrintJobMetric("physics_shadow_completed_jobs",
		s_physicsMetricsFrozen.shadowCompletedJobs);
	PrintJobMetric("physics_shadow_matches", s_physicsMetricsFrozen.shadowMatches);
	PrintJobMetric("physics_shadow_mismatches", s_physicsMetricsFrozen.shadowMismatches);
	PrintJobMetric("physics_owner_fallbacks", s_physicsMetricsFrozen.ownerFallbacks);
	PrintJobMetric("physics_ineligible_slices", s_physicsMetricsFrozen.ineligibleSlices);
	PrintJobMetric("physics_unexpected_fallbacks",
		s_physicsMetricsFrozen.unexpectedFallbacks);
	PrintJobMetric("physics_stale_rejections", s_physicsMetricsFrozen.staleRejections);
	PrintJobMetric("physics_circuit_breaker_trips",
		s_physicsMetricsFrozen.circuitBreakerTrips);
	PrintJobMetric("status_authoritative_batches",
		s_statusMetricsFrozen.authoritativeBatches);
	PrintJobMetric("status_committed_commands",
		s_statusMetricsFrozen.committedCommands);
	PrintJobMetric("status_submitted_jobs", s_statusMetricsFrozen.submittedJobs);
	PrintJobMetric("status_completed_jobs", s_statusMetricsFrozen.completedJobs);
	PrintJobMetric("status_physical_worker_jobs",
		s_statusMetricsFrozen.physicalWorkerJobs);
	PrintJobMetric("status_owner_helped_jobs",
		s_statusMetricsFrozen.ownerHelpedJobs);
	PrintJobMetric("status_physical_worker_mask",
		s_statusMetricsFrozen.physicalWorkerMask);
	PrintJobMetric("status_distinct_physical_workers",
		s_statusMetricsFrozen.maximumDistinctPhysicalWorkers);
	PrintJobMetric("status_physical_worker_mask_complete",
		s_statusMetricsFrozen.physicalWorkerMaskComplete ? 1 : 0);
	PrintJobMetric("status_peak_concurrent_physical_workers",
		s_statusMetricsFrozen.maximumPeakConcurrentPhysicalWorkers);
	PrintJobMetric("status_shadow_executions",
		s_statusMetricsFrozen.shadowExecutions);
	PrintJobMetric("status_shadow_commands", s_statusMetricsFrozen.shadowCommands);
	PrintJobMetric("status_shadow_matches", s_statusMetricsFrozen.shadowMatches);
	PrintJobMetric("status_shadow_mismatches",
		s_statusMetricsFrozen.shadowMismatches);
	PrintJobMetric("status_owner_fallbacks", s_statusMetricsFrozen.ownerFallbacks);
	PrintJobMetric("status_stale_rejections", s_statusMetricsFrozen.staleRejections);
	const rts::ImmutableSpatialRuntimeMetrics &spatial = s_spatialMetricsFrozen;
	PrintJobMetric("spatial_captured_arenas", spatial.capturedArenas);
	PrintJobMetric("spatial_capture_failures", spatial.captureFailures);
	PrintJobMetric("spatial_successful_collections", spatial.successfulCollections);
	PrintJobMetric("spatial_successful_collection_queries",
		spatial.successfulCollectionQueries);
	PrintJobMetric("spatial_successful_collection_ranges",
		spatial.successfulCollectionRanges);
	PrintJobMetric("spatial_multi_range_collections",
		spatial.multiRangeCollections);
	PrintJobMetric("spatial_collection_submitted_jobs",
		spatial.collectionSubmittedJobs);
	PrintJobMetric("spatial_collection_completed_jobs",
		spatial.collectionCompletedJobs);
	PrintJobMetric("spatial_collection_physical_worker_jobs",
		spatial.collectionPhysicalWorkerJobs);
	PrintJobMetric("spatial_collection_owner_helped_jobs",
		spatial.collectionOwnerHelpedJobs);
	PrintJobMetric("spatial_collection_physical_worker_mask",
		spatial.collectionPhysicalWorkerMask);
	PrintJobMetric("spatial_maximum_collection_queries",
		spatial.maximumCollectionQueries);
	PrintJobMetric("spatial_maximum_collection_ranges",
		spatial.maximumCollectionRanges);
	PrintJobMetric("spatial_maximum_collection_distinct_physical_workers",
		spatial.maximumCollectionDistinctPhysicalWorkers);
#define PRINT_SPATIAL_CONSUMER(prefix, consumer) \
	PrintJobMetric(prefix "_eligible_queries", consumer.eligibleQueries); \
	PrintJobMetric(prefix "_authoritative_queries", consumer.authoritativeQueries); \
	PrintJobMetric(prefix "_authoritative_candidates", consumer.authoritativeCandidates); \
	PrintJobMetric(prefix "_shadow_queries", consumer.shadowQueries); \
	PrintJobMetric(prefix "_shadow_matches", consumer.shadowMatches); \
	PrintJobMetric(prefix "_shadow_mismatches", consumer.shadowMismatches); \
	PrintJobMetric(prefix "_submitted_jobs", consumer.submittedJobs); \
	PrintJobMetric(prefix "_completed_jobs", consumer.completedJobs); \
	PrintJobMetric(prefix "_physical_worker_jobs", consumer.physicalWorkerJobs); \
	PrintJobMetric(prefix "_owner_helped_jobs", consumer.ownerHelpedJobs); \
	PrintJobMetric(prefix "_expected_fallbacks", consumer.expectedFallbacks); \
	PrintJobMetric(prefix "_unexpected_fallbacks", consumer.unexpectedFallbacks); \
	PrintJobMetric(prefix "_stale_rejections", consumer.staleRejections); \
	PrintJobMetric(prefix "_validation_failures", consumer.validationFailures); \
	PrintJobMetric(prefix "_circuit_breaker_trips", consumer.circuitBreakerTrips)
	PRINT_SPATIAL_CONSUMER("spatial_healing", spatial.healing);
	PRINT_SPATIAL_CONSUMER("spatial_pdl", spatial.pointDefenseLaser);
#undef PRINT_SPATIAL_CONSUMER
#endif
	printf(" job_queue_high_water=%u job_peak_active_workers=%u available_cpus=%u "
		"reserved_owner_cpus=%u selected_worker_cpus=%u\n",
		metrics.injectionHighWater, metrics.maximumActiveWorkers,
		metrics.availableLogicalCpuCount, metrics.reservedOwnerCpuCount,
		metrics.selectedWorkerCpuCount);
}
} // namespace

void FailSkirmishAITest(const char *reason)
{
	s_runner.failed = TRUE;
	s_runner.failureReason = reason;
}

void RequestSkirmishAITestStop()
{
	CaptureSkirmishAITestRuntimeState();
	if (TheGameLogic && TheGameLogic->isInGame())
	{
		if (!s_runner.ending)
		{
			TheGameLogic->exitGame();
			s_runner.ending = TRUE;
			s_runner.shutdownStartMilliseconds = GetTickCount();
		}
	}
	else if (TheGameEngine)
	{
		TheGameEngine->setQuitting(TRUE);
	}
}

Bool IsSkirmishAITest4v2(SkirmishAITestScenario scenario)
{
	return scenario == SKIRMISH_AI_TEST_SCENARIO_4V2;
}

Int ExpectedSkirmishAITestAiCount(SkirmishAITestScenario scenario)
{
	return IsSkirmishAITest4v2(scenario) ? 6 : 7;
}

const char *SkirmishAITestScenarioName(SkirmishAITestScenario scenario)
{
	if (IsSkirmishAITest4v2(scenario))
		return "4v2";
	if (scenario == SKIRMISH_AI_TEST_SCENARIO_PRACTICAL_1V7)
		return "practical-1v7";
	return "4v3";
}

Bool IsSkirmishAITestPracticalControllerScenario(
	SkirmishAITestScenario scenario)
{
	return scenario == SKIRMISH_AI_TEST_SCENARIO_PRACTICAL_1V7;
}

Bool IsValidSkirmishAITestPracticalControllerPlan(
	const SkirmishAITestPlan &plan)
{
	const SkirmishAITestSlotPlan &controller = plan.slots[0];
	if (controller.state != SLOT_PLAYER ||
		controller.playerTemplate == PLAYERTEMPLATE_OBSERVER ||
		!controller.isController || controller.color != 0 ||
		controller.startPosition != 0 || controller.teamNumber != 0)
	{
		return FALSE;
	}
	Int aiCount = 0;
	Int teamCounts[2] = { 0, 0 };
	for (Int slotIndex = 1; slotIndex < SKIRMISH_AI_TEST_SLOT_COUNT; ++slotIndex)
	{
		const SkirmishAITestSlotPlan &slot = plan.slots[slotIndex];
		if (slot.state != SLOT_BRUTAL_AI || slot.isController ||
			slot.playerTemplate != PLAYERTEMPLATE_RANDOM ||
			slot.color != slotIndex || slot.startPosition != slotIndex ||
			slot.teamNumber != (slotIndex <= 3 ? 0 : 1))
		{
			return FALSE;
		}
		++aiCount;
		++teamCounts[slot.teamNumber];
	}
	return aiCount == 7 && controller.teamNumber == 0 &&
		teamCounts[0] == 3 && teamCounts[1] == 4;
}

Bool IsValidSkirmishAITestReplayReceipt(
	const SkirmishAITestReplayReceipt &receipt, Int expectedReplayEpoch)
{
	if (expectedReplayEpoch <= 0 || receipt.seed <= 0 ||
		(receipt.winnerTeam != 0 && receipt.winnerTeam != 1) ||
		receipt.endFrame == 0 || receipt.replayEpoch != expectedReplayEpoch ||
		!HasBoundedString(receipt.scenario,
			SKIRMISH_AI_TEST_RECEIPT_SCENARIO_LENGTH) ||
		!HasBoundedString(receipt.executableSha256,
			SKIRMISH_AI_TEST_RECEIPT_SHA256_LENGTH + 1) ||
		!HasBoundedString(receipt.replaySha256,
			SKIRMISH_AI_TEST_RECEIPT_SHA256_LENGTH + 1) ||
		!HasBoundedString(receipt.runNonce,
			SKIRMISH_AI_TEST_RECEIPT_NONCE_LENGTH + 1) ||
		!HasBoundedString(receipt.replayPath,
			SKIRMISH_AI_TEST_RECEIPT_PATH_LENGTH))
	{
		return FALSE;
	}
	if (strcmp(receipt.scenario, "4v3") != 0 &&
		strcmp(receipt.scenario, "4v2") != 0 &&
		strcmp(receipt.scenario, "practical-1v7") != 0)
	{
		return FALSE;
	}
	if (!IsHexString(receipt.executableSha256,
			SKIRMISH_AI_TEST_RECEIPT_SHA256_LENGTH) ||
		!IsHexString(receipt.replaySha256,
			SKIRMISH_AI_TEST_RECEIPT_SHA256_LENGTH) ||
		strlen(receipt.runNonce) == 0)
	{
		return FALSE;
	}
	for (size_t index = 0; receipt.runNonce[index] != '\0'; ++index)
	{
		const char value = receipt.runNonce[index];
		if (!((value >= '0' && value <= '9') ||
			(value >= 'A' && value <= 'F') ||
			(value >= 'a' && value <= 'f') || value == '-'))
		{
			return FALSE;
		}
	}
	return receipt.replayPath[0] != '\0';
}

Bool TryParseSkirmishAITestSeed(const char *text, Int *seed)
{
	if (text == nullptr || text[0] == '\0' || seed == nullptr)
		return FALSE;

	errno = 0;
	char *end = nullptr;
	const long value = strtol(text, &end, 10);
	if (errno == ERANGE || end == text || *end != '\0' || value <= 0 || value > INT_MAX)
		return FALSE;

	*seed = static_cast<Int>(value);
	return TRUE;
}

Bool SetSkirmishAITestExecutableHashInput(const char *sha256)
{
	if (sha256 == nullptr || strlen(sha256) != 64)
		return FALSE;
	for (Int index = 0; index < 64; ++index)
	{
		if (!IsHexDigit(sha256[index]))
			return FALSE;
	}
	strlcpy(s_executableHashInput, sha256, ARRAY_SIZE(s_executableHashInput));
	return TRUE;
}

Bool SetSkirmishAITestSimulationModeInput(const char *mode)
{
	if (mode == nullptr ||
		(strcmp(mode, "serial") != 0 && strcmp(mode, "parallel") != 0 &&
			strcmp(mode, "shadow") != 0))
	{
		return FALSE;
	}
	strlcpy(s_simulationModeInput, mode, ARRAY_SIZE(s_simulationModeInput));
	return TRUE;
}

void SetSkirmishAITestFinalDigest(UnsignedInt digest)
{
	s_finalDigest = digest;
	s_finalDigestAvailable = TRUE;
}

Bool ShouldBypassFramePacingForSkirmishAITest(Bool runnerArmed)
{
	if (!runnerArmed)
		return FALSE;
	// Automated headless gates run without pacing. The practical controller
	// lane keeps normal presentation/input cadence so it remains playable.
	return !s_runner.armed ||
		!IsSkirmishAITestPracticalControllerScenario(s_runner.scenario);
}

void BuildSkirmishAITestPlan(Int seed, SkirmishAITestPlan *plan)
{
	BuildSkirmishAITestPlan(seed, SKIRMISH_AI_TEST_SCENARIO_4V3, plan);
}

void BuildSkirmishAITestPlan(Int seed, SkirmishAITestScenario scenario,
	SkirmishAITestPlan *plan)
{
	if (plan == nullptr)
		return;
	if (scenario != SKIRMISH_AI_TEST_SCENARIO_4V2 &&
		scenario != SKIRMISH_AI_TEST_SCENARIO_PRACTICAL_1V7)
		scenario = SKIRMISH_AI_TEST_SCENARIO_4V3;

	plan->seed = seed;
	plan->mapName = "Maps\\Twilight Flame\\Twilight Flame.map";

	SkirmishAITestSlotPlan &localSlot = plan->slots[0];
	localSlot.state = SLOT_PLAYER;
	localSlot.isController = IsSkirmishAITestPracticalControllerScenario(scenario);
	if (localSlot.isController)
	{
		localSlot.playerTemplate = PLAYERTEMPLATE_RANDOM;
		localSlot.color = 0;
		localSlot.startPosition = 0;
		localSlot.teamNumber = 0;
	}
	else
	{
		localSlot.playerTemplate = PLAYERTEMPLATE_OBSERVER;
		localSlot.color = -1;
		localSlot.startPosition = -1;
		localSlot.teamNumber = -1;
	}

	for (Int i = 1; i < SKIRMISH_AI_TEST_SLOT_COUNT; ++i)
	{
		SkirmishAITestSlotPlan &slot = plan->slots[i];
		slot.isController = FALSE;
		if (IsSkirmishAITest4v2(scenario) && i == 7)
		{
			slot.state = SLOT_CLOSED;
			slot.playerTemplate = -1;
			slot.color = -1;
			slot.startPosition = -1;
			slot.teamNumber = -1;
			continue;
		}
		slot.state = SLOT_BRUTAL_AI;
		slot.playerTemplate = PLAYERTEMPLATE_RANDOM;
		if (IsSkirmishAITestPracticalControllerScenario(scenario))
		{
			slot.color = i;
			slot.startPosition = i;
			slot.teamNumber = i <= 3 ? 0 : 1;
		}
		else
		{
			slot.color = i - 1;
			slot.startPosition = i - 1;
			slot.teamNumber = i <= 4 ? 0 : 1;
		}
	}
}

Bool IsExpectedSkirmishAITestLoadedState(const SkirmishAITestPlan &plan,
	UnsignedInt expectedMapCRC, UnsignedInt expectedMapSize,
	const SkirmishAITestLoadedState *loadedState)
{
	if (plan.mapName == nullptr || loadedState == nullptr ||
		loadedState->gameInfoMapName == nullptr || loadedState->globalMapName == nullptr ||
		loadedState->terrainMapName == nullptr)
	{
		return FALSE;
	}

	return _stricmp(loadedState->gameInfoMapName, plan.mapName) == 0 &&
		_stricmp(loadedState->globalMapName, plan.mapName) == 0 &&
		_stricmp(loadedState->terrainMapName, plan.mapName) == 0 &&
		loadedState->mapCRC == expectedMapCRC &&
		loadedState->mapSize == expectedMapSize &&
		loadedState->seed == plan.seed;
}

Bool IsValidSkirmishAITestReplayResult(UnsignedInt expectedFrameCount,
	UnsignedInt actualFrameCount, Bool desyncGame, Bool quitEarly,
	time_t startTime, time_t endTime)
{
	// VictoryConditions records the winning frame before GameLogic advances to
	// the frame written by RecorderClass::logGameEnd().
	return expectedFrameCount != 0 && expectedFrameCount < UINT_MAX &&
		actualFrameCount == expectedFrameCount + 1U &&
		!desyncGame && !quitEarly && startTime > 0 && endTime >= startTime;
}

SkirmishAITestProgress EvaluateSkirmishAITestProgress(UnsignedInt endFrame, UnsignedInt currentFrame)
{
	if (endFrame != 0)
		return SKIRMISH_AI_TEST_COMPLETE;
	if (currentFrame >= SKIRMISH_AI_TEST_MAX_FRAME)
		return SKIRMISH_AI_TEST_TIMED_OUT;
	return SKIRMISH_AI_TEST_RUNNING;
}

Bool IsSkirmishAITestShutdownTimedOut(UnsignedInt elapsedMilliseconds)
{
	return elapsedMilliseconds >= SKIRMISH_AI_TEST_MAX_SHUTDOWN_MILLISECONDS;
}

Bool IsSkirmishAITestStartupTimedOut(UnsignedInt elapsedMilliseconds)
{
	return elapsedMilliseconds >= SKIRMISH_AI_TEST_MAX_STARTUP_MILLISECONDS;
}

Bool IsSkirmishAITestProgressStalled(UnsignedInt elapsedMilliseconds)
{
	return elapsedMilliseconds >= SKIRMISH_AI_TEST_MAX_STALLED_MILLISECONDS;
}

void ArmSkirmishAITestRunner(Int seed, SkirmishAITestScenario scenario)
{
	if (scenario != SKIRMISH_AI_TEST_SCENARIO_4V2 &&
		scenario != SKIRMISH_AI_TEST_SCENARIO_PRACTICAL_1V7)
		scenario = SKIRMISH_AI_TEST_SCENARIO_4V3;
	s_runner.armed = TRUE;
	s_runner.started = FALSE;
	s_runner.ending = FALSE;
	s_runner.finished = FALSE;
	s_runner.failed = FALSE;
	s_runner.seed = seed;
	s_runner.winnerTeam = -1;
	s_runner.endFrame = 0;
	s_runner.startupStartMilliseconds = 0;
	s_runner.lastObservedFrame = UINT_MAX;
	s_runner.stalledStartMilliseconds = 0;
	s_runner.shutdownStartMilliseconds = 0;
	s_runner.replayFileName[0] = '\0';
	s_runner.failureReason = nullptr;
	s_runner.expectedMapCRC = 0;
	s_runner.expectedMapSize = 0;
	s_runner.loadedStateValidated = FALSE;
	s_runner.loadedMapName[0] = '\0';
	s_runner.loadedMapCRC = 0;
	s_runner.loadedMapSize = 0;
	s_runner.loadedSeed = 0;
	s_runner.scenario = scenario;
	s_runner.actualAiCount = 0;
	s_runner.actualTeamCounts[0] = 0;
	s_runner.actualTeamCounts[1] = 0;
	s_runner.replayEpoch = SKIRMISH_AI_REPLAY_EPOCH_LEGACY;
	s_runner.retainedReplayPath[0] = '\0';
	s_runner.replaySha256[0] = '\0';
	++s_runnerNonceCounter;
	if (s_runnerNonceCounter == 0)
		++s_runnerNonceCounter;
	_snprintf(s_runner.runNonce, ARRAY_SIZE(s_runner.runNonce),
		"%08X-%08X-%08X", GetTickCount(),
		static_cast<UnsignedInt>(seed), s_runnerNonceCounter);
	s_runner.runNonce[ARRAY_SIZE(s_runner.runNonce) - 1] = '\0';
	strlcpy(s_executableHashObserved, "unavailable",
		ARRAY_SIZE(s_executableHashObserved));
	CaptureSkirmishAITestExecutableHash(s_executableHashObserved);
	const rts::JobSystemConfig jobConfig = rts::JobSystem::startupConfig();
	s_runner.requestedWorkerCount = jobConfig.workerCount;
	s_runner.workerPolicy = jobConfig.workerPolicy;
	s_runner.effectiveWorkerCount = 0;
	s_runner.requestedPipelineMode = rts::GetPipelineExecutionMode();
	s_runner.requestedSimulationMode = rts::GetSimulationExecutionMode();
	s_jobMetricsAtStart = rts::JobSystem::instance().metrics();
	s_phaseMetricsAtStart = TheGameLogic != nullptr ?
		TheGameLogic->getStage5PhaseRuntimeMetrics() :
		rts::LiveSimulationPhaseRuntimeMetrics();
	s_phaseMetricsLast = s_phaseMetricsAtStart;
	s_directPathMetricsAtStart = GetDirectPathRuntimeMetrics();
	memset(&s_directPathMetricsFrozen, 0, sizeof(s_directPathMetricsFrozen));
	s_directPathMetricsHaveFrozenActivity = FALSE;
	s_directPathMetricsAwaitingInitialReset = TRUE;
	s_ordinaryPathMetricsAtStart = GetOrdinaryPathRuntimeMetrics();
	memset(&s_ordinaryPathMetricsFrozen, 0,
		sizeof(s_ordinaryPathMetricsFrozen));
	s_ordinaryPathMetricsAwaitingInitialReset = TRUE;
#if defined(_WIN64)
	s_aiPlanningMetricsAtStart = rts::GetAIPlanningRuntimeMetrics();
	s_collisionMetricsAtStart = rts::GetCollisionCandidateRuntimeMetrics();
	s_collisionMetricsFrozen = rts::CollisionCandidateRuntimeMetrics();
	s_collisionMetricsAwaitingInitialReset = TRUE;
	s_physicsMetricsAtStart = rts::GetPhysicsIntegrationRuntimeMetrics();
	s_physicsMetricsFrozen = rts::PhysicsIntegrationRuntimeMetrics();
	s_physicsMetricsAwaitingInitialReset = TRUE;
	s_statusMetricsAtStart = rts::GetObjectStatusTimerRuntimeMetrics();
	s_statusMetricsFrozen = rts::ObjectStatusTimerRuntimeMetrics();
	s_statusMetricsAwaitingInitialReset = TRUE;
	s_spatialMetricsAtStart = rts::GetImmutableSpatialRuntimeMetrics();
	s_spatialMetricsFrozen = rts::ImmutableSpatialRuntimeMetrics();
	s_spatialMetricsAwaitingInitialReset = TRUE;
#endif
	s_finalDigest = 0;
	s_finalDigestAvailable = FALSE;
}

Bool IsSkirmishAITestRunnerArmed()
{
	return s_runner.armed;
}

Bool StartSkirmishAITestRunner()
{
	if (!s_runner.armed)
		return TRUE;
	DEBUG_LOG(("SkirmishAITestRunner::start phase=entry seed=%d", s_runner.seed));
	s_runner.startupStartMilliseconds = GetTickCount();
	CaptureSkirmishAITestRuntimeState();
	if (!TheGlobalData->m_simulateReplays.empty())
	{
		FailSkirmishAITest("conflicting_replay_mode");
		return FALSE;
	}
	if (!TheMapCache || !TheMessageStream || !TheRecorder || !TheWritableGlobalData)
	{
		FailSkirmishAITest("engine_not_ready");
		return FALSE;
	}
	DEBUG_LOG(("SkirmishAITestRunner::start phase=dependencies_ready"));

	SkirmishAITestPlan plan;
	BuildSkirmishAITestPlan(s_runner.seed, s_runner.scenario, &plan);
	const MapMetaData *map = TheMapCache->findMap(plan.mapName);
	if (!map || !map->m_doesExist || !map->m_isMultiplayer ||
		map->m_numPlayers < ExpectedSkirmishAITestAiCount(s_runner.scenario) + 1)
	{
		FailSkirmishAITest("twilight_flame_unavailable");
		return FALSE;
	}
	DEBUG_LOG(("SkirmishAITestRunner::start phase=map_ready"));
	s_runner.expectedMapCRC = map->m_CRC;
	s_runner.expectedMapSize = map->m_filesize;

	delete TheSkirmishGameInfo;
	TheSkirmishGameInfo = NEW SkirmishGameInfo;
	TheSkirmishGameInfo->init();
	TheSkirmishGameInfo->clearSlotList();
	TheSkirmishGameInfo->reset();
	TheSkirmishGameInfo->setLocalIP(0);
	TheSkirmishGameInfo->enterGame();
	DEBUG_LOG(("SkirmishAITestRunner::start phase=game_info_ready"));

	for (Int i = 0; i < SKIRMISH_AI_TEST_SLOT_COUNT; ++i)
	{
		const SkirmishAITestSlotPlan &slotPlan = plan.slots[i];
		GameSlot *slot = TheSkirmishGameInfo->getSlot(i);
		UnicodeString localName;
		if (i == 0)
			localName.set(slotPlan.isController ? L"Practical Controller" :
				L"Automated Observer");
		slot->setState(slotPlan.state, localName, 0);
		slot->setPlayerTemplate(slotPlan.playerTemplate);
		slot->setColor(slotPlan.color);
		slot->setStartPos(slotPlan.startPosition);
		slot->setTeamNumber(slotPlan.teamNumber);
		if (i == 0)
		{
			slot->setAccept();
			slot->setMapAvailability(TRUE);
		}
	}
	DEBUG_LOG(("SkirmishAITestRunner::start phase=slots_ready"));

	TheSkirmishGameInfo->setMap(plan.mapName);
	TheSkirmishGameInfo->setMapCRC(map->m_CRC);
	TheSkirmishGameInfo->setMapSize(map->m_filesize);
	TheSkirmishGameInfo->setSeed(plan.seed);
	TheSkirmishGameInfo->startGame(0);
	DEBUG_LOG(("SkirmishAITestRunner::start phase=start_game_complete"));

	TheWritableGlobalData->m_mapName = plan.mapName;
	// The practical controller lane is intentionally interactive.  The two
	// automated lanes remain headless and continue to be the only replay-gate
	// scenarios.
	TheWritableGlobalData->m_headless =
		IsSkirmishAITestPracticalControllerScenario(s_runner.scenario) ? FALSE : TRUE;
	TheWritableGlobalData->m_shellMapOn = FALSE;
	if (!IsSkirmishAITestPracticalControllerScenario(s_runner.scenario))
		TheWritableGlobalData->m_useFpsLimit = FALSE;
	// The automated observer owns no units. Keep its logical and local retaliation modes disabled
	// so the recorder does not capture an irrelevant frame-zero preference synchronization command.
	if (!IsSkirmishAITestPracticalControllerScenario(s_runner.scenario))
		TheWritableGlobalData->m_clientRetaliationModeEnabled = FALSE;
	TheRecorder->setArchiveEnabled(FALSE);
	InitRandom(static_cast<UnsignedInt>(plan.seed));

#if defined(_WIN64)
	if (!IsSkirmishAITestPracticalControllerScenario(s_runner.scenario) &&
		!s_performanceReceiptAttempted)
	{
		s_performanceReceiptAttempted = true;
		s_performanceReceipt.reset(new PerformanceReceiptRuntime);
		if (!s_performanceReceipt->begin("fresh-ai-map", "")) s_performanceReceipt.reset();
	}
#endif
	GameMessage *message = TheMessageStream->appendMessage(GameMessage::MSG_NEW_GAME);
	message->appendIntegerArgument(GAME_SKIRMISH);
	message->appendIntegerArgument(DIFFICULTY_NORMAL);
	message->appendIntegerArgument(0);

	s_runner.started = TRUE;
	if (IsSkirmishAITest4v2(s_runner.scenario))
	{
		printf("SKIRMISH_AI_TEST_START seed=%d scenario=%s map=\"%s\" expected_ai=6 expected_teams=4v2\n",
			plan.seed, SkirmishAITestScenarioName(s_runner.scenario), plan.mapName);
	}
	else if (IsSkirmishAITestPracticalControllerScenario(s_runner.scenario))
	{
		printf("SKIRMISH_AI_TEST_START seed=%d scenario=%s map=\"%s\" expected_ai=7 expected_teams=1-controller+3v4-ai\n",
			plan.seed, SkirmishAITestScenarioName(s_runner.scenario), plan.mapName);
	}
	else
	{
		printf("SKIRMISH_AI_TEST_START seed=%d scenario=%s map=\"%s\" expected_ai=7 expected_teams=4v3\n",
			plan.seed, SkirmishAITestScenarioName(s_runner.scenario), plan.mapName);
	}
	fflush(stdout);
	return TRUE;
}

void UpdateSkirmishAITestRunner()
{
	if (s_runner.armed)
		CaptureSkirmishAITestRuntimeState();
	static UnsignedInt lastDiagnosticMilliseconds = 0;
	const UnsignedInt diagnosticMilliseconds = GetTickCount();
	if (s_runner.armed &&
		(lastDiagnosticMilliseconds == 0 ||
			ElapsedMilliseconds(lastDiagnosticMilliseconds, diagnosticMilliseconds) >= 10000))
	{
		lastDiagnosticMilliseconds = diagnosticMilliseconds;
		DEBUG_LOG(("SkirmishAITestRunner::update armed=%d started=%d ending=%d finished=%d failed=%d frame=%u",
			s_runner.armed, s_runner.started, s_runner.ending, s_runner.finished, s_runner.failed,
			TheGameLogic ? TheGameLogic->getFrame() : 0));
	}

	if (!s_runner.armed || !s_runner.started || s_runner.finished)
		return;
	if (!TheGameLogic)
	{
		FailSkirmishAITest("runtime_state_unavailable");
		RequestSkirmishAITestStop();
		return;
	}
	if (s_runner.ending)
	{
		const UnsignedInt shutdownElapsed =
			ElapsedMilliseconds(s_runner.shutdownStartMilliseconds, GetTickCount());
		if (!TheGameLogic->isInGame())
		{
			s_runner.finished = !s_runner.failed;
			TheGameEngine->setQuitting(TRUE);
		}
		else if (IsSkirmishAITestShutdownTimedOut(shutdownElapsed))
		{
			FailSkirmishAITest("shutdown_timeout");
			if (TheRecorder && TheRecorder->getMode() == RECORDERMODETYPE_RECORD)
				TheRecorder->stopRecording();
			TheGameLogic->clearGameData(FALSE);
			TheGameEngine->setQuitting(TRUE);
		}
		return;
	}
	if (!TheGameLogic->isInGame() || TheGameLogic->isLoadingMap() || !TheGameInfo)
	{
		const UnsignedInt startupElapsed =
			ElapsedMilliseconds(s_runner.startupStartMilliseconds, GetTickCount());
		if (IsSkirmishAITestStartupTimedOut(startupElapsed))
		{
			FailSkirmishAITest("startup_timeout");
			RequestSkirmishAITestStop();
		}
		return;
	}
	if (!TheVictoryConditions || !ThePlayerList || !TheRecorder)
	{
		FailSkirmishAITest("runtime_state_unavailable");
		RequestSkirmishAITestStop();
		return;
	}

	SkirmishAITestPlan expectedPlan;
	BuildSkirmishAITestPlan(s_runner.seed, s_runner.scenario, &expectedPlan);
	const AsciiString gameInfoMap = TheGameInfo->getMap();
	const AsciiString globalMap = TheGlobalData->m_mapName;
	const AsciiString terrainMap = TheTerrainLogic
		? TheTerrainLogic->getSourceFilename()
		: AsciiString::TheEmptyString;
	SkirmishAITestLoadedState loadedState = {
		gameInfoMap.str(), globalMap.str(), terrainMap.str(),
		TheGameInfo->getMapCRC(), TheGameInfo->getMapSize(), TheGameInfo->getSeed()
	};
	if (!IsExpectedSkirmishAITestLoadedState(expectedPlan, s_runner.expectedMapCRC,
		s_runner.expectedMapSize, &loadedState))
	{
		FailSkirmishAITest("loaded_state_mismatch");
		RequestSkirmishAITestStop();
		return;
	}
	if (!s_runner.loadedStateValidated)
	{
		s_runner.loadedStateValidated = TRUE;
		strlcpy(s_runner.loadedMapName, loadedState.gameInfoMapName, ARRAY_SIZE(s_runner.loadedMapName));
		s_runner.loadedMapCRC = loadedState.mapCRC;
		s_runner.loadedMapSize = loadedState.mapSize;
		s_runner.loadedSeed = loadedState.seed;
#if defined(_WIN64)
		BindSkirmishAITestPerformanceMap();
#endif
	}

	if (!IsSkirmishAITestPracticalControllerScenario(s_runner.scenario))
		TheWritableGlobalData->m_useFpsLimit = FALSE;
	// RecorderClass::startRecording() resets this preference after the runner's
	// startup hook. Reassert it while recording so LastReplay remains at the
	// exact path reported and validated by this test.
	TheRecorder->setArchiveEnabled(FALSE);
	if (TheRecorder->getMode() == RECORDERMODETYPE_RECORD && !TheRecorder->hasOpenRecordingFile())
	{
		FailSkirmishAITest("recorder_file_unavailable");
		RequestSkirmishAITestStop();
		return;
	}
	if (s_runner.replayFileName[0] == '\0' && TheRecorder->getMode() == RECORDERMODETYPE_RECORD)
	{
		const AsciiString recordingFileName = TheRecorder->getRecordingFileName();
		strlcpy(s_runner.replayFileName, recordingFileName.str(), ARRAY_SIZE(s_runner.replayFileName));
	}
	const UnsignedInt endFrame = TheVictoryConditions->getEndFrame();
	const UnsignedInt currentFrame = TheGameLogic->getFrame();
	const SkirmishAITestProgress progress =
		EvaluateSkirmishAITestProgress(endFrame, currentFrame);
	if (progress == SKIRMISH_AI_TEST_RUNNING)
	{
		const UnsignedInt nowMilliseconds = GetTickCount();
		if (currentFrame == s_runner.lastObservedFrame)
		{
			const UnsignedInt stalledElapsed =
				ElapsedMilliseconds(s_runner.stalledStartMilliseconds, nowMilliseconds);
			if (IsSkirmishAITestProgressStalled(stalledElapsed))
			{
				FailSkirmishAITest("frame_stalled");
				RequestSkirmishAITestStop();
			}
		}
		else
		{
			s_runner.lastObservedFrame = currentFrame;
			s_runner.stalledStartMilliseconds = nowMilliseconds;
		}
		return;
	}
	if (progress == SKIRMISH_AI_TEST_TIMED_OUT)
	{
		FailSkirmishAITest("frame_limit");
		RequestSkirmishAITestStop();
		return;
	}

	const GameSlot *localSlot = TheGameInfo->getConstSlot(0);
	Player *localPlayer = ThePlayerList->findPlayerWithNameKey(NAMEKEY("player0"));
	Bool validMatch = FALSE;
	if (IsSkirmishAITestPracticalControllerScenario(s_runner.scenario))
	{
		// A practical run must own a normal player slot.  An observer is not a
		// substitute for controller coverage and is rejected explicitly here.
		validMatch = localSlot && localSlot->isHuman() &&
			localSlot->getOriginalPlayerTemplate() != PLAYERTEMPLATE_OBSERVER &&
			localPlayer && !localPlayer->isPlayerObserver();
	}
	else
	{
		validMatch = localSlot && localSlot->isHuman() &&
			localSlot->getOriginalPlayerTemplate() == PLAYERTEMPLATE_OBSERVER &&
			localPlayer && localPlayer->isPlayerObserver();
	}
	Int expectedAiCount = 0;
	Int expectedTeamCounts[2] = { 0, 0 };
	for (Int i = 1; i < SKIRMISH_AI_TEST_SLOT_COUNT; ++i)
	{
		const SkirmishAITestSlotPlan &slotPlan = expectedPlan.slots[i];
		if (slotPlan.state == SLOT_BRUTAL_AI)
		{
			++expectedAiCount;
			if (slotPlan.teamNumber == 0 || slotPlan.teamNumber == 1)
				++expectedTeamCounts[slotPlan.teamNumber];
		}
	}
	Int actualAiCount = 0;
	Int actualTeamCounts[2] = { 0, 0 };
	for (Int slotIndex = 1; slotIndex < SKIRMISH_AI_TEST_SLOT_COUNT; ++slotIndex)
	{
		const SkirmishAITestSlotPlan &expectedSlot = expectedPlan.slots[slotIndex];
		const GameSlot *slot = TheGameInfo->getConstSlot(slotIndex);
		AsciiString playerName;
		playerName.format("player%d", slotIndex);
		Player *player = ThePlayerList->findPlayerWithNameKey(NAMEKEY(playerName));
		if (slot && slot->getState() == SLOT_BRUTAL_AI)
		{
			++actualAiCount;
			const Int actualTeam = slot->getTeamNumber();
			if (actualTeam == 0 || actualTeam == 1)
				++actualTeamCounts[actualTeam];
			else
				validMatch = FALSE;
		}

		if (expectedSlot.state == SLOT_CLOSED)
		{
			if (!slot || slot->getState() != SLOT_CLOSED || player != nullptr)
				validMatch = FALSE;
			continue;
		}

		if (!slot || slot->getState() != SLOT_BRUTAL_AI ||
			slot->getOriginalPlayerTemplate() != expectedSlot.playerTemplate ||
			slot->getOriginalColor() != expectedSlot.color ||
			slot->getOriginalStartPos() != expectedSlot.startPosition ||
			!player || player->getPlayerType() != PLAYER_COMPUTER ||
			slot->getTeamNumber() != expectedSlot.teamNumber)
		{
			validMatch = FALSE;
		}
	}
	if (IsSkirmishAITestPracticalControllerScenario(s_runner.scenario) &&
		(!localSlot || localSlot->getTeamNumber() != 0 ||
		localSlot->getOriginalPlayerTemplate() == PLAYERTEMPLATE_OBSERVER))
	{
		validMatch = FALSE;
	}
	if (!validMatch || actualAiCount != expectedAiCount ||
		actualTeamCounts[0] != expectedTeamCounts[0] ||
		actualTeamCounts[1] != expectedTeamCounts[1])
	{
		const char *failureReason = "invalid_4v3_setup";
		if (IsSkirmishAITest4v2(s_runner.scenario))
			failureReason = "invalid_4v2_setup";
		else if (IsSkirmishAITestPracticalControllerScenario(s_runner.scenario))
			failureReason = "invalid_practical_1v7_setup";
		FailSkirmishAITest(failureReason);
		RequestSkirmishAITestStop();
		return;
	}
	s_runner.actualAiCount = actualAiCount;
	s_runner.actualTeamCounts[0] = actualTeamCounts[0];
	s_runner.actualTeamCounts[1] = actualTeamCounts[1];

	Int winnerTeam = -1;
	Bool conflictingWinners = FALSE;
	for (Int winnerIndex = 0; winnerIndex < SKIRMISH_AI_TEST_SLOT_COUNT; ++winnerIndex)
	{
		AsciiString playerName;
		playerName.format("player%d", winnerIndex);
		Player *player = ThePlayerList->findPlayerWithNameKey(NAMEKEY(playerName));
		if (player && TheVictoryConditions->hasAchievedVictory(player))
		{
			const GameSlot *slot = TheGameInfo->getConstSlot(winnerIndex);
			if (!slot)
			{
				conflictingWinners = TRUE;
				continue;
			}
			const Int playerTeam = slot->getTeamNumber();
			if (winnerTeam == -1)
				winnerTeam = playerTeam;
			else if (winnerTeam != playerTeam)
				conflictingWinners = TRUE;
		}
	}
	if (conflictingWinners || (winnerTeam != 0 && winnerTeam != 1))
	{
		FailSkirmishAITest("winner_unavailable");
		RequestSkirmishAITestStop();
		return;
	}
	if (TheRecorder->getMode() != RECORDERMODETYPE_RECORD || !TheRecorder->hasOpenRecordingFile())
	{
		FailSkirmishAITest("recorder_file_unavailable");
		RequestSkirmishAITestStop();
		return;
	}
	if (s_runner.replayFileName[0] == '\0')
	{
		FailSkirmishAITest("replay_filename_unavailable");
		RequestSkirmishAITestStop();
		return;
	}

	s_runner.winnerTeam = winnerTeam;
	s_runner.endFrame = endFrame;
	// Capture the authoritative owner-thread state digest before exitGame tears
	// down the live simulation. Validation compares this value across worker
	// counts and execution modes; it must never be derived from worker order.
	const UnsignedInt finalCRC = TheGameLogic->getCRC(CRC_RECALC);
	SetSkirmishAITestFinalDigest(finalCRC);
#if defined(_WIN64)
	// The victory event frame remains unchanged in the replay contract. Its
	// authoritative digest belongs to the actual current owner frame instead.
	if (s_performanceReceipt)
		s_performanceReceipt->captureTerminalResult(currentFrame, finalCRC, true, true);
#endif
	RequestSkirmishAITestStop();
}

#if defined(_WIN64)
void ObserveSkirmishAITestCompletedFrame(unsigned previousFrame)
{
	if (!s_performanceReceipt || !s_performanceReceipt->active() ||
		!s_runner.loadedStateValidated || s_runner.failed)
		return;
	// ending may already be true: exitGame queued teardown, but this frame
	// still contains the terminal owner state and must be observed after EndFrame.
	s_performanceReceipt->captureCompletedFrame(previousFrame,
		s_collisionMetricsFrozen, s_physicsMetricsFrozen, s_statusMetricsFrozen,
		s_spatialMetricsFrozen, s_ordinaryPathMetricsFrozen);
}

void FinalizeSkirmishAITestPerformanceReceipt(Int engineExitCode)
{
	if (s_performanceReceipt)
	{
		s_performanceReceipt->finish(engineExitCode, "GameMain:engine-destroyed-before-owner-detach");
		s_performanceReceipt.reset();
	}
}
#endif

Int FinalizeSkirmishAITestRunner(Int engineExitCode)
{
	if (!s_runner.armed)
		return engineExitCode;
	if (engineExitCode != 0 && !s_runner.failed)
		FailSkirmishAITest("engine_exit");
	if (!s_runner.finished && !s_runner.failed)
		FailSkirmishAITest("incomplete");

	AsciiString replayName = s_runner.replayFileName;
	AsciiString replayPath = RecorderClass::getReplayDir();
	replayPath.concat(replayName);
	if (!s_runner.failed)
	{
		// The game should have closed the recorder while leaving the game.  Close
		// it here as a defensive boundary before reading or retaining LastReplay;
		// the next runner invocation must never race this copy.
		if (TheRecorder && TheRecorder->getMode() == RECORDERMODETYPE_RECORD)
			TheRecorder->stopRecording();
		RecorderClass::ReplayHeader header;
		header.filename = replayName;
		header.forPlayback = FALSE;
		if (!TheRecorder || !TheRecorder->readReplayHeader(header) ||
			!RecorderClass::replayMatchesGameVersion(header) ||
			!IsValidSkirmishAITestReplayResult(s_runner.endFrame, header.frameCount,
				header.desyncGame, header.quitEarly, header.startTime, header.endTime))
		{
			FailSkirmishAITest("replay_validation");
		}
		else
		{
			s_runner.replayEpoch = GetSkirmishAIReplayEpoch(header.versionTimeString);
			if (s_runner.replayEpoch != SKIRMISH_AI_REPLAY_EPOCH_CURRENT)
				FailSkirmishAITest("replay_epoch_mismatch");
		}
	}
	if (!s_runner.failed)
	{
		if (strcmp(s_executableHashObserved, "unavailable") == 0)
			FailSkirmishAITest("executable_hash_unavailable");
		else if (strcmp(s_executableHashInput, "unavailable") != 0 &&
			_stricmp(s_executableHashInput, s_executableHashObserved) != 0)
			FailSkirmishAITest("executable_hash_mismatch");
	}
	if (!s_runner.failed)
	{
		char retainedReplayPath[SKIRMISH_AI_TEST_RECEIPT_PATH_LENGTH];
		const AsciiString replayDirectoryValue = RecorderClass::getReplayDir();
		const char *replayDirectory = replayDirectoryValue.str();
		const char *separator = "";
		const size_t directoryLength = strlen(replayDirectory);
		if (directoryLength != 0 &&
			replayDirectory[directoryLength - 1] != '\\' &&
			replayDirectory[directoryLength - 1] != '/')
			separator = "\\";
		const int retainedLength = _snprintf(retainedReplayPath,
			ARRAY_SIZE(retainedReplayPath), "%s%sSkirmishAI-%s-%d-%s.rep",
			replayDirectory, separator,
			SkirmishAITestScenarioName(s_runner.scenario), s_runner.seed,
			s_runner.runNonce);
		retainedReplayPath[ARRAY_SIZE(retainedReplayPath) - 1] = '\0';
		if (retainedLength < 0 ||
			!RetainSkirmishAITestReplayAtomically(replayPath.str(),
				retainedReplayPath, s_runner.replaySha256))
		{
			FailSkirmishAITest("replay_retention");
		}
		else
		{
			strlcpy(s_runner.retainedReplayPath, retainedReplayPath,
				ARRAY_SIZE(s_runner.retainedReplayPath));
		}
	}
	if (!s_runner.failed)
	{
		SkirmishAITestReplayReceipt receipt;
		memset(&receipt, 0, sizeof(receipt));
		receipt.seed = s_runner.seed;
		receipt.winnerTeam = s_runner.winnerTeam;
		receipt.endFrame = s_runner.endFrame;
		receipt.replayEpoch = s_runner.replayEpoch;
		strlcpy(receipt.scenario, SkirmishAITestScenarioName(s_runner.scenario),
			ARRAY_SIZE(receipt.scenario));
		strlcpy(receipt.executableSha256, s_executableHashObserved,
			ARRAY_SIZE(receipt.executableSha256));
		strlcpy(receipt.replaySha256, s_runner.replaySha256,
			ARRAY_SIZE(receipt.replaySha256));
		strlcpy(receipt.runNonce, s_runner.runNonce,
			ARRAY_SIZE(receipt.runNonce));
		strlcpy(receipt.replayPath, s_runner.retainedReplayPath,
			ARRAY_SIZE(receipt.replayPath));
		if (!IsValidSkirmishAITestReplayReceipt(receipt,
			SKIRMISH_AI_REPLAY_EPOCH_CURRENT))
			FailSkirmishAITest("replay_receipt_invalid");
	}

#if defined(_WIN64)
	// Copy all owner-dependent state before GameEngine destroys the recorder,
	// player list, game data and JobSystem owner registration.
	if (s_performanceReceipt)
	{
		s_performanceReceipt->captureSchedulerBeforeTeardown();
		if (s_runner.failed)
			s_performanceReceipt->invalidate("fresh AI owner run or closed replay validation failed");
		else
			s_performanceReceipt->retainClosedReplay(s_runner.retainedReplayPath, s_runner.replaySha256);
	}
#endif
	if (s_runner.failed)
	{
		printf("SKIRMISH_AI_TEST_FAIL seed=%d scenario=%s run_nonce=%s reason=%s\n",
			s_runner.seed, SkirmishAITestScenarioName(s_runner.scenario),
			s_runner.runNonce,
			s_runner.failureReason ? s_runner.failureReason : "unknown");
		fflush(stdout);
		return 1;
	}

	if (IsSkirmishAITest4v2(s_runner.scenario))
	{
		printf("SKIRMISH_AI_TEST_COMPLETE seed=%d scenario=%s map=\"%s\" map_crc=%08X map_size=%u loaded_seed=%d "
			"actual_ai=%d actual_teams=%dv%d winner_team=%d end_frame=%u replay=%s",
			s_runner.seed, SkirmishAITestScenarioName(s_runner.scenario), s_runner.loadedMapName,
			s_runner.loadedMapCRC, s_runner.loadedMapSize, s_runner.loadedSeed,
			s_runner.actualAiCount, s_runner.actualTeamCounts[0], s_runner.actualTeamCounts[1],
			s_runner.winnerTeam, s_runner.endFrame, replayPath.str());
	}
	else
	{
		printf("SKIRMISH_AI_TEST_COMPLETE seed=%d scenario=%s map=\"%s\" map_crc=%08X map_size=%u loaded_seed=%d "
			"actual_ai=%d actual_teams=%dv%d winner_team=%d end_frame=%u replay=%s",
			s_runner.seed, SkirmishAITestScenarioName(s_runner.scenario), s_runner.loadedMapName,
			s_runner.loadedMapCRC, s_runner.loadedMapSize, s_runner.loadedSeed,
			s_runner.actualAiCount, s_runner.actualTeamCounts[0], s_runner.actualTeamCounts[1],
			s_runner.winnerTeam, s_runner.endFrame, replayPath.str());
	}
	PrintSkirmishAITestManifest();
	fflush(stdout);
	return 0;
}
