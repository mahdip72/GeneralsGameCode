#include "Lib/PerformanceReceipt.h"

#include <algorithm>
#include <ctype.h>
#include <fstream>
#include <iomanip>
#include <limits.h>
#include <sstream>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#if !defined(NOMINMAX)
#define NOMINMAX
#endif
#if !defined(_WIN32_WINNT)
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>
#include <bcrypt.h>
#endif

namespace rts { namespace performance {

namespace
{
const char *const REQUIRED_PHASE_NAMES[] =
{
	"owner-intake",
	"world-queries",
	"pathfinding",
	"object-computation",
	"spatial-work",
	"deterministic-commit",
	"verification-publication"
};

const char *const REQUIRED_KERNEL_NAMES[] =
{
	"physics",
	"status",
	"collision",
	"ai-planning",
	"spatial",
	"path"
};

void setReason(std::string *reason, const char *value)
{
	if (reason != 0)
		*reason = value != 0 ? value : "unspecified failure";
}

void setReason(std::string *reason, const std::string &value)
{
	if (reason != 0)
		*reason = value;
}

bool isHexString(const std::string &value, unsigned length)
{
	if (value.size() != length)
		return false;
	for (std::size_t index = 0; index < value.size(); ++index)
	{
		const unsigned char character =
			static_cast<unsigned char>(value[index]);
		if (!((character >= '0' && character <= '9') ||
			(character >= 'a' && character <= 'f') ||
			(character >= 'A' && character <= 'F')))
			return false;
	}
	return true;
}

bool isLowerHexString(const std::string &value, unsigned length)
{
	if (!isHexString(value, length))
		return false;
	for (std::size_t index = 0; index < value.size(); ++index)
		if (value[index] >= 'A' && value[index] <= 'F')
			return false;
	return true;
}

bool isSafeToken(const std::string &value)
{
	if (value.empty() || value.size() > 256)
		return false;
	for (std::size_t index = 0; index < value.size(); ++index)
	{
		const unsigned char character =
			static_cast<unsigned char>(value[index]);
		if (!isalnum(character) && character != '-' && character != '_' &&
			character != '.')
			return false;
	}
	return value.find("..") == std::string::npos;
}

bool readEnvironment(const char *name, std::string &value)
{
#if defined(_WIN32)
	char buffer[4096];
	const DWORD length = GetEnvironmentVariableA(name, buffer,
		static_cast<DWORD>(sizeof(buffer)));
	if (length == 0 || length >= sizeof(buffer))
		return false;
	value.assign(buffer, length);
	return !value.empty();
#else
	const char *environmentValue = getenv(name);
	if (environmentValue == 0 || environmentValue[0] == '\0')
		return false;
	value.assign(environmentValue);
	return true;
#endif
}

bool parseUnsigned(const std::string &text, unsigned &value)
{
	if (text.empty())
		return false;
	char *end = 0;
	const unsigned long parsed = strtoul(text.c_str(), &end, 10);
	if (end == text.c_str() || *end != '\0' || parsed > UINT_MAX)
		return false;
	value = static_cast<unsigned>(parsed);
	return true;
}

void appendJsonString(std::ostringstream &json, const std::string &value)
{
	static const char HEX[] = "0123456789ABCDEF";
	json << '"';
	for (std::size_t index = 0; index < value.size(); ++index)
	{
		const unsigned char character =
			static_cast<unsigned char>(value[index]);
		switch (character)
		{
		case '\\': json << "\\\\"; break;
		case '"': json << "\\\""; break;
		case '\b': json << "\\b"; break;
		case '\f': json << "\\f"; break;
		case '\n': json << "\\n"; break;
		case '\r': json << "\\r"; break;
		case '\t': json << "\\t"; break;
		default:
			if (character < 0x20)
				json << "\\u00" << HEX[character >> 4]
					<< HEX[character & 0x0f];
			else
				json << static_cast<char>(character);
			break;
		}
	}
	json << '"';
}

void appendKey(std::ostringstream &json, const char *key)
{
	appendJsonString(json, key);
	json << ':';
}

void appendStringField(std::ostringstream &json, const char *key,
	const std::string &value, bool comma = true)
{
	appendKey(json, key);
	appendJsonString(json, value);
	if (comma) json << ',';
}

void appendUnsignedField(std::ostringstream &json, const char *key,
	unsigned value, bool comma = true)
{
	appendKey(json, key);
	json << value;
	if (comma) json << ',';
}

void appendCounterField(std::ostringstream &json, const char *key,
	JobMetricCounter value, bool comma = true)
{
	appendKey(json, key);
	json << static_cast<unsigned long long>(value);
	if (comma) json << ',';
}

void appendBoolField(std::ostringstream &json, const char *key,
	bool value, bool comma = true)
{
	appendKey(json, key);
	json << (value ? "true" : "false");
	if (comma) json << ',';
}

bool hasUniqueNamesAndExactOrder(
	const std::vector<PerformanceReceiptPhase> &phases,
	const char *const *requiredNames, unsigned requiredCount)
{
	if (phases.size() != requiredCount)
		return false;
	for (unsigned index = 0; index < requiredCount; ++index)
	{
		if (phases[index].name != requiredNames[index])
			return false;
		for (unsigned prior = 0; prior < index; ++prior)
			if (phases[index].name == phases[prior].name)
				return false;
	}
	return true;
}

bool hasUniqueNamesAndExactOrder(
	const std::vector<PerformanceReceiptKernel> &kernels,
	const char *const *requiredNames, unsigned requiredCount)
{
	if (kernels.size() != requiredCount)
		return false;
	for (unsigned index = 0; index < requiredCount; ++index)
	{
		if (kernels[index].name != requiredNames[index])
			return false;
		for (unsigned prior = 0; prior < index; ++prior)
			if (kernels[index].name == kernels[prior].name)
				return false;
	}
	return true;
}

bool findCpuSet(const PerformanceReceipt &receipt, unsigned id,
	PerformanceReceiptCpuSet *result = 0)
{
	for (std::size_t index = 0; index < receipt.cpuSets.size(); ++index)
	{
		if (receipt.cpuSets[index].id == id)
		{
			if (result != 0)
				*result = receipt.cpuSets[index];
			return true;
		}
	}
	return false;
}

bool containsUnsigned(const std::vector<unsigned> &values, unsigned value)
{
	return std::find(values.begin(), values.end(), value) != values.end();
}

#if defined(_WIN32)
bool currentFileTime(JobMetricCounter &value)
{
	FILETIME fileTime;
	GetSystemTimeAsFileTime(&fileTime);
	ULARGE_INTEGER integer;
	integer.LowPart = fileTime.dwLowDateTime;
	integer.HighPart = fileTime.dwHighDateTime;
	value = static_cast<JobMetricCounter>(integer.QuadPart);
	return value != 0;
}

bool calculateSha256(const void *bytes, std::size_t byteCount,
	std::string &digest)
{
	BCRYPT_ALG_HANDLE algorithm = 0;
	BCRYPT_HASH_HANDLE hash = 0;
	DWORD objectLength = 0;
	DWORD digestLength = 0;
	DWORD propertyBytes = 0;
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
	unsigned char result[32] = { 0 };
	const bool created = BCryptCreateHash(algorithm, &hash, object.data(),
		objectLength, 0, 0, 0) == 0;
	const bool hashed = created && byteCount <= 0xffffffffU &&
		BCryptHashData(hash, reinterpret_cast<PUCHAR>(
			const_cast<void *>(bytes)), static_cast<ULONG>(byteCount), 0) == 0;
	const bool finished = hashed && BCryptFinishHash(hash, result,
		digestLength, 0) == 0;
	if (finished)
	{
		static const char HEX[] = "0123456789ABCDEF";
		digest.resize(64);
		for (unsigned index = 0; index < 32; ++index)
		{
			digest[index * 2] = HEX[result[index] >> 4];
			digest[index * 2 + 1] = HEX[result[index] & 0x0f];
		}
	}
	if (hash != 0)
		BCryptDestroyHash(hash);
	BCryptCloseAlgorithmProvider(algorithm, 0);
	return finished;
}

bool calculateFileSha256(const std::string &path, std::string &digest)
{
	HANDLE file = CreateFileA(path.c_str(), GENERIC_READ,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0,
		OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, 0);
	if (file == INVALID_HANDLE_VALUE)
		return false;
	BCRYPT_ALG_HANDLE algorithm = 0;
	BCRYPT_HASH_HANDLE hash = 0;
	DWORD objectLength = 0;
	DWORD digestLength = 0;
	DWORD propertyBytes = 0;
	bool success = BCryptOpenAlgorithmProvider(&algorithm,
		BCRYPT_SHA256_ALGORITHM, 0, 0) == 0 &&
		BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
		reinterpret_cast<PUCHAR>(&objectLength), sizeof(objectLength),
		&propertyBytes, 0) == 0 && objectLength != 0 &&
		BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH,
		reinterpret_cast<PUCHAR>(&digestLength), sizeof(digestLength),
		&propertyBytes, 0) == 0 && digestLength == 32;
	std::vector<unsigned char> object;
	if (success)
	{
		object.resize(objectLength);
		success = BCryptCreateHash(algorithm, &hash, object.data(), objectLength,
			0, 0, 0) == 0;
	}
	unsigned char buffer[64 * 1024];
	while (success)
	{
		DWORD read = 0;
		if (!ReadFile(file, buffer, sizeof(buffer), &read, 0))
		{
			success = false;
			break;
		}
		if (read == 0)
			break;
		success = BCryptHashData(hash, buffer, read, 0) == 0;
	}
	unsigned char result[32] = { 0 };
	if (success)
		success = BCryptFinishHash(hash, result, digestLength, 0) == 0;
	if (success)
	{
		static const char HEX[] = "0123456789ABCDEF";
		digest.resize(64);
		for (unsigned index = 0; index < 32; ++index)
		{
			digest[index * 2] = HEX[result[index] >> 4];
			digest[index * 2 + 1] = HEX[result[index] & 0x0f];
		}
	}
	if (hash != 0)
		BCryptDestroyHash(hash);
	if (algorithm != 0)
		BCryptCloseAlgorithmProvider(algorithm, 0);
	CloseHandle(file);
	return success;
}

bool captureProcessIdentity(PerformanceReceipt &receipt, std::string *reason)
{
	receipt.processId = GetCurrentProcessId();
	char path[MAX_PATH];
	const DWORD length = GetModuleFileNameA(0, path, sizeof(path));
	if (length == 0 || length >= sizeof(path))
	{
		setReason(reason, "GetModuleFileNameA failed");
		return false;
	}
	receipt.executablePath.assign(path, length);
	if (!calculateFileSha256(receipt.executablePath,
		receipt.executableSha256))
	{
		setReason(reason, "executable SHA-256 capture failed");
		return false;
	}
	FILETIME creation;
	FILETIME processExit;
	FILETIME kernel;
	FILETIME user;
	if (!GetProcessTimes(GetCurrentProcess(), &creation, &processExit,
		&kernel, &user))
	{
		setReason(reason, "GetProcessTimes failed");
		return false;
	}
	ULARGE_INTEGER creationInteger;
	creationInteger.LowPart = creation.dwLowDateTime;
	creationInteger.HighPart = creation.dwHighDateTime;
	receipt.processCreationTimeUtc100ns =
		static_cast<JobMetricCounter>(creationInteger.QuadPart);
	if (receipt.processCreationTimeUtc100ns == 0 ||
		!currentFileTime(receipt.processStartTimeUtc100ns))
	{
		setReason(reason, "process time capture was unavailable");
		return false;
	}
	const char *commandLine = GetCommandLineA();
	if (commandLine == 0 || commandLine[0] == '\0')
	{
		setReason(reason, "GetCommandLineA returned no command line");
		return false;
	}
	receipt.commandLine.assign(commandLine);
	receipt.processIdentityAvailable = true;
	return true;
}
#else
bool captureProcessIdentity(PerformanceReceipt &, std::string *reason)
{
	setReason(reason, "executable process provenance is unavailable on this host");
	return false;
}
#endif

bool loadOptionalUnsigned(const char *name, unsigned &value, bool &known,
	std::string *reason)
{
	std::string text;
	if (!readEnvironment(name, text))
	{
		known = false;
		return true;
	}
	if (!parseUnsigned(text, value))
	{
		setReason(reason, std::string("invalid ") + name);
		return false;
	}
	known = true;
	return true;
}

void appendCpuSet(std::ostringstream &json,
	const PerformanceReceiptCpuSet &cpuSet, bool comma)
{
	json << "{\n";
	appendUnsignedField(json, "id", cpuSet.id);
	appendUnsignedField(json, "efficiencyClass", cpuSet.efficiencyClass);
	appendUnsignedField(json, "group", cpuSet.group);
	appendUnsignedField(json, "coreIndex", cpuSet.coreIndex);
	appendUnsignedField(json, "logicalProcessorIndex",
		cpuSet.logicalProcessorIndex);
	appendBoolField(json, "parked", cpuSet.parked);
	appendBoolField(json, "allocatedToOtherProcess",
		cpuSet.allocatedToOtherProcess);
	appendBoolField(json, "availableToProcess", cpuSet.availableToProcess,
		false);
	json << "\n}";
	if (comma) json << ',';
}

void appendPhase(std::ostringstream &json,
	const PerformanceReceiptPhase &phase, bool comma)
{
	json << "{\n";
	appendStringField(json, "name", phase.name);
	appendBoolField(json, "available", phase.available);
	appendCounterField(json, "totalNanoseconds", phase.totalNanoseconds);
	appendCounterField(json, "maximumNanoseconds",
		phase.maximumNanoseconds);
	appendCounterField(json, "sampleCount", phase.sampleCount, false);
	json << "\n}";
	if (comma) json << ',';
}

void appendKernel(std::ostringstream &json,
	const PerformanceReceiptKernel &kernel, bool comma)
{
	json << "{\n";
	appendStringField(json, "name", kernel.name);
	appendBoolField(json, "available", kernel.available);
	appendCounterField(json, "submittedJobs", kernel.submittedJobs);
	appendCounterField(json, "completedJobs", kernel.completedJobs);
	appendCounterField(json, "physicalWorkerJobs",
		kernel.physicalWorkerJobs);
	appendCounterField(json, "ownerHelpedJobs", kernel.ownerHelpedJobs);
	appendCounterField(json, "physicalWorkerMask",
		kernel.physicalWorkerMask);
	appendUnsignedField(json, "distinctPhysicalWorkers",
		kernel.distinctPhysicalWorkers);
	appendBoolField(json, "physicalWorkerMaskComplete",
		kernel.physicalWorkerMaskComplete);
	appendCounterField(json, "elapsedNanoseconds",
		kernel.elapsedNanoseconds);
	appendBoolField(json, "elapsedNanosecondsKnown",
		kernel.elapsedNanosecondsKnown, false);
	json << "\n}";
	if (comma) json << ',';
}

} // namespace

PerformanceReceiptCpuSet::PerformanceReceiptCpuSet()
	: id(0), efficiencyClass(0), group(0), coreIndex(UINT_MAX),
	  logicalProcessorIndex(0), parked(false),
	  allocatedToOtherProcess(false), availableToProcess(false)
{
}

PerformanceReceiptPhase::PerformanceReceiptPhase()
	: available(false), totalNanoseconds(0), maximumNanoseconds(0),
	  sampleCount(0)
{
}

PerformanceReceiptKernel::PerformanceReceiptKernel()
	: available(false), submittedJobs(0), completedJobs(0),
	  physicalWorkerJobs(0), ownerHelpedJobs(0), physicalWorkerMask(0),
	  distinctPhysicalWorkers(0), physicalWorkerMaskComplete(false),
	  elapsedNanoseconds(0), elapsedNanosecondsKnown(false)
{
}

PerformanceReceiptRawEvidence::PerformanceReceiptRawEvidence()
{
}

PerformanceReceipt::PerformanceReceipt()
	: schemaVersion(PERFORMANCE_RECEIPT_SCHEMA_VERSION),
	  producer(PERFORMANCE_RECEIPT_PRODUCER),
	  evidenceKind(PERFORMANCE_RECEIPT_EVIDENCE_KIND), status("pending"),
	  processId(0), processCreationTimeUtc100ns(0),
	  processStartTimeUtc100ns(0), processEndTimeUtc100ns(0),
	  processIdentityAvailable(false), processExitCode(0),
	  processExitCodeKnown(false), seed(0), seedKnown(false),
	  playerCount(0), playerCountKnown(false), unitCount(0),
	  unitCountKnown(false), frameStart(0), frameEnd(0), finalFrame(0),
	  finalCrcKnown(false), finalCrc(0), requestedWorkerCount(0),
	  effectiveWorkerCount(0), workersPinned(false),
	  availableLogicalCpuCount(0), reservedOwnerCpuCount(0),
	  selectedWorkerCpuCount(0), selectedWorkerPhysicalCoreCount(0),
	  selectedWorkerPhysicalCoreMask(0),
	  selectedWorkerPhysicalCoreMaskComplete(false)
{
}

bool BeginPerformanceReceipt(PerformanceReceipt &receipt, const char *title,
	const char *replayPath, unsigned ordinal, std::string *reason)
{
	receipt = PerformanceReceipt();
	if (title == 0 || title[0] == '\0' || replayPath == 0 ||
		replayPath[0] == '\0')
	{
		setReason(reason, "title and replay path are required");
		return false;
	}
	receipt.title = title;
	receipt.replayPath = replayPath;

	if (!readEnvironment("RTS_PERFORMANCE_RUN_ID", receipt.runId) ||
		!readEnvironment("RTS_PERFORMANCE_RECEIPT_DIR",
			receipt.outputDirectory) ||
		!readEnvironment("RTS_PERFORMANCE_SOURCE_COMMIT", receipt.sourceCommit) ||
		!readEnvironment("RTS_PERFORMANCE_ARTIFACT_SET_SHA256",
			receipt.artifactSetSha256) ||
		!readEnvironment("RTS_PERFORMANCE_FIXTURE_ID", receipt.fixtureId) ||
		!readEnvironment("RTS_PERFORMANCE_FIXTURE_SHA256",
			receipt.fixtureContentSha256) ||
		!readEnvironment("RTS_PERFORMANCE_RAW_LOG_PATH",
			receipt.rawEvidence.rawLogPath) ||
		!readEnvironment("RTS_PERFORMANCE_TIMING_PATH",
			receipt.rawEvidence.timingPath) ||
		!readEnvironment("RTS_PERFORMANCE_VERIFIER_BOUNDARY",
			receipt.rawEvidence.verifierBoundary))
	{
		setReason(reason, "required performance receipt environment is missing");
		return false;
	}
	std::string seedText;
	if (!readEnvironment("RTS_PERFORMANCE_SEED", seedText) ||
		!parseUnsigned(seedText, receipt.seed))
	{
		setReason(reason, "RTS_PERFORMANCE_SEED is missing or invalid");
		return false;
	}
	receipt.seedKnown = true;
	readEnvironment("RTS_PERFORMANCE_RAW_LOG_SHA256",
		receipt.rawEvidence.rawLogSha256);
	readEnvironment("RTS_PERFORMANCE_TIMING_SHA256",
		receipt.rawEvidence.timingSha256);
	if (!loadOptionalUnsigned("RTS_PERFORMANCE_PLAYER_COUNT",
		receipt.playerCount, receipt.playerCountKnown, reason) ||
		!loadOptionalUnsigned("RTS_PERFORMANCE_UNIT_COUNT", receipt.unitCount,
			receipt.unitCountKnown, reason))
		return false;
	if (!captureProcessIdentity(receipt, reason))
		return false;
	// The ordinal is deliberately retained in the run ID only when the host
	// provided a nonzero ordinal. It cannot replace the host's unique nonce.
	if (ordinal != 0)
	{
		std::ostringstream suffix;
		suffix << '-' << ordinal;
		receipt.runId += suffix.str();
	}
	return true;
}

bool CapturePerformanceReceiptJobSystem(PerformanceReceipt &receipt,
	const JobSystem &jobs, const JobSystemMetrics &metrics,
	std::string *reason)
{
	const JobSystemConfig config = JobSystem::startupConfig();
	receipt.schedulerMetrics = metrics;
	receipt.requestedWorkerCount = config.workerCount;
	receipt.workerPolicy = config.workerPolicy == JOB_WORKER_POLICY_ALL ?
		"all" : (config.workerPolicy == JOB_WORKER_POLICY_AUTO ? "auto" : "unknown");
	if (receipt.workerPolicy == "unknown")
	{
		setReason(reason, "unknown JobSystem worker policy");
		return false;
	}
	if (jobs.workerCount() != 0)
		receipt.effectiveWorkerCount = jobs.workerCount();
	receipt.workersPinned = config.pinWorkers;
	receipt.availableLogicalCpuCount = metrics.availableLogicalCpuCount;
	receipt.reservedOwnerCpuCount = metrics.reservedOwnerCpuCount;
	receipt.selectedWorkerCpuCount = metrics.selectedWorkerCpuCount;
	receipt.selectedWorkerPhysicalCoreCount =
		metrics.selectedWorkerPhysicalCoreCount;
	receipt.selectedWorkerPhysicalCoreMask =
		metrics.selectedWorkerPhysicalCoreMask;
	receipt.selectedWorkerPhysicalCoreMaskComplete =
		metrics.selectedWorkerPhysicalCoreMaskComplete;

	receipt.cpuSets.clear();
	const unsigned cpuSetCount = jobs.cpuSetCount();
	for (unsigned index = 0; index < cpuSetCount; ++index)
	{
		JobCpuSetInfo source;
		if (!jobs.cpuSetAt(index, source))
		{
			setReason(reason, "JobSystem CPU-set snapshot changed during capture");
			return false;
		}
		PerformanceReceiptCpuSet target;
		target.id = source.id;
		target.efficiencyClass = source.efficiencyClass;
		target.group = source.group;
		target.coreIndex = source.coreIndex;
		target.logicalProcessorIndex = source.logicalProcessorIndex;
		target.parked = source.parked;
		target.allocatedToOtherProcess = source.allocatedToOtherProcess;
		target.availableToProcess = source.availableToProcess;
		receipt.cpuSets.push_back(target);
	}
	receipt.selectedWorkerCpuSetIds.clear();
	for (unsigned index = 0; index < jobs.selectedWorkerCpuSetCount(); ++index)
	{
		unsigned id = 0;
		if (!jobs.selectedWorkerCpuSetIdAt(index, id))
		{
			setReason(reason, "JobSystem selected CPU-set snapshot changed");
			return false;
		}
		receipt.selectedWorkerCpuSetIds.push_back(id);
	}
	receipt.ownerCpuSetIds.clear();
	for (unsigned index = 0; index < jobs.ownerCpuSetCount(); ++index)
	{
		unsigned id = 0;
		if (!jobs.ownerCpuSetIdAt(index, id))
		{
			setReason(reason, "JobSystem owner CPU-set snapshot changed");
			return false;
		}
		receipt.ownerCpuSetIds.push_back(id);
	}
	return true;
}

bool SetPerformanceReceiptReplayResult(PerformanceReceipt &receipt,
	unsigned frameStart, unsigned finalFrame, unsigned finalCrc,
	bool finalCrcKnown, int processExitCode, bool processExitCodeKnown,
	const char *exitBoundary, bool clean, std::string *reason)
{
	if (exitBoundary == 0 || exitBoundary[0] == '\0')
	{
		setReason(reason, "process exit boundary is required");
		return false;
	}
	if (!processExitCodeKnown)
	{
		setReason(reason, "process exit code is unavailable");
		return false;
	}
	receipt.frameStart = frameStart;
	receipt.frameEnd = finalFrame;
	receipt.finalFrame = finalFrame;
	receipt.finalCrc = finalCrc;
	receipt.finalCrcKnown = finalCrcKnown;
	receipt.processExitCode = processExitCode;
	receipt.processExitCodeKnown = processExitCodeKnown;
	receipt.processExitBoundary = exitBoundary;
#if defined(_WIN32)
	if (!currentFileTime(receipt.processEndTimeUtc100ns))
	{
		setReason(reason, "process end time capture was unavailable");
		return false;
	}
#endif
	receipt.status = clean ? "complete" : "failed";
	return true;
}

bool SerializePerformanceReceipt(const PerformanceReceipt &receipt,
	std::string &document, std::string *reason)
{
	(void)reason;
	std::ostringstream json;
	json << "{\n";
	appendUnsignedField(json, "schemaVersion", receipt.schemaVersion);
	appendStringField(json, "producer", receipt.producer);
	appendStringField(json, "evidenceKind", receipt.evidenceKind);
	appendStringField(json, "status", receipt.status);
	appendStringField(json, "title", receipt.title);
	appendStringField(json, "runId", receipt.runId);
	appendStringField(json, "sourceCommit", receipt.sourceCommit);
	appendStringField(json, "artifactSetSha256", receipt.artifactSetSha256);
	appendStringField(json, "executablePath", receipt.executablePath);
	appendStringField(json, "executableSha256", receipt.executableSha256);
	appendStringField(json, "commandLine", receipt.commandLine);
	json << "\"process\":{\n";
	appendUnsignedField(json, "id", receipt.processId);
	appendCounterField(json, "creationTimeUtc100ns",
		receipt.processCreationTimeUtc100ns);
	appendCounterField(json, "startTimeUtc100ns",
		receipt.processStartTimeUtc100ns);
	appendCounterField(json, "endTimeUtc100ns",
		receipt.processEndTimeUtc100ns);
	appendBoolField(json, "identityAvailable",
		receipt.processIdentityAvailable);
	appendBoolField(json, "exitCodeKnown", receipt.processExitCodeKnown);
	appendKey(json, "exitCode");
	json << receipt.processExitCode << ',';
	appendStringField(json, "exitBoundary", receipt.processExitBoundary, false);
	json << "\n},\n";
	json << "\"fixture\":{\n";
	appendStringField(json, "id", receipt.fixtureId);
	appendStringField(json, "contentSha256", receipt.fixtureContentSha256);
	appendStringField(json, "replayPath", receipt.replayPath);
	appendUnsignedField(json, "seed", receipt.seed);
	appendBoolField(json, "seedKnown", receipt.seedKnown);
	appendUnsignedField(json, "playerCount", receipt.playerCount);
	appendBoolField(json, "playerCountKnown", receipt.playerCountKnown);
	appendUnsignedField(json, "unitCount", receipt.unitCount);
	appendBoolField(json, "unitCountKnown", receipt.unitCountKnown, false);
	json << "\n},\n";
	json << "\"frames\":{\n";
	appendUnsignedField(json, "start", receipt.frameStart);
	appendUnsignedField(json, "end", receipt.frameEnd);
	appendUnsignedField(json, "final", receipt.finalFrame);
	appendBoolField(json, "finalCrcKnown", receipt.finalCrcKnown);
	appendUnsignedField(json, "finalCrc", receipt.finalCrc, false);
	json << "\n},\n";
	json << "\"worker\":{\n";
	appendUnsignedField(json, "requestedCount", receipt.requestedWorkerCount);
	appendUnsignedField(json, "effectiveCount", receipt.effectiveWorkerCount);
	appendStringField(json, "policy", receipt.workerPolicy);
	appendBoolField(json, "pinned", receipt.workersPinned);
	appendUnsignedField(json, "availableLogicalCpuCount",
		receipt.availableLogicalCpuCount);
	appendUnsignedField(json, "reservedOwnerCpuCount",
		receipt.reservedOwnerCpuCount);
	appendUnsignedField(json, "selectedWorkerCpuCount",
		receipt.selectedWorkerCpuCount);
	appendUnsignedField(json, "selectedWorkerPhysicalCoreCount",
		receipt.selectedWorkerPhysicalCoreCount);
	appendCounterField(json, "selectedWorkerPhysicalCoreMask",
		receipt.selectedWorkerPhysicalCoreMask);
	appendBoolField(json, "selectedWorkerPhysicalCoreMaskComplete",
		receipt.selectedWorkerPhysicalCoreMaskComplete, false);
	json << "\n},\n";
	json << "\"topology\":{\n";
	appendStringField(json, "source", "GetSystemCpuSetInformation");
	appendKey(json, "cpuSets");
	json << "[\n";
	for (std::size_t index = 0; index < receipt.cpuSets.size(); ++index)
		appendCpuSet(json, receipt.cpuSets[index],
			index + 1 != receipt.cpuSets.size());
	json << "\n],\n";
	appendKey(json, "ownerCpuSetIds");
	json << '[';
	for (std::size_t index = 0; index < receipt.ownerCpuSetIds.size(); ++index)
	{
		if (index != 0) json << ',';
		json << receipt.ownerCpuSetIds[index];
	}
	json << "],\n";
	appendKey(json, "selectedWorkerCpuSetIds");
	json << '[';
	for (std::size_t index = 0;
		index < receipt.selectedWorkerCpuSetIds.size(); ++index)
	{
		if (index != 0) json << ',';
		json << receipt.selectedWorkerCpuSetIds[index];
	}
	json << "]\n},\n";
	json << "\"rawEvidence\":{\n";
	appendStringField(json, "verifierBoundary",
		receipt.rawEvidence.verifierBoundary);
	appendStringField(json, "rawLogPath", receipt.rawEvidence.rawLogPath);
	appendStringField(json, "rawLogSha256",
		receipt.rawEvidence.rawLogSha256);
	appendStringField(json, "timingPath", receipt.rawEvidence.timingPath);
	appendStringField(json, "timingSha256",
		receipt.rawEvidence.timingSha256, false);
	json << "\n},\n";
	json << "\"schedulerMetrics\":{\n";
	appendCounterField(json, "submittedJobCount",
		receipt.schedulerMetrics.submittedJobCount);
	appendCounterField(json, "executedJobCount",
		receipt.schedulerMetrics.executedJobCount);
	appendCounterField(json, "stealCount",
		receipt.schedulerMetrics.stealCount);
	appendCounterField(json, "ownerHelpCount",
		receipt.schedulerMetrics.ownerHelpCount);
	appendCounterField(json, "waitCount", receipt.schedulerMetrics.waitCount);
	appendCounterField(json, "workerWaitRejectionCount",
		receipt.schedulerMetrics.workerWaitRejectionCount);
	appendCounterField(json, "failedJobCount",
		receipt.schedulerMetrics.failedJobCount);
	appendCounterField(json, "cancelledJobCount",
		receipt.schedulerMetrics.cancelledJobCount);
	appendCounterField(json, "serialFallbackCount",
		receipt.schedulerMetrics.serialFallbackCount);
	appendCounterField(json, "totalQueueLatencyNanoseconds",
		receipt.schedulerMetrics.totalQueueLatencyNanoseconds);
	appendCounterField(json, "maximumQueueLatencyNanoseconds",
		receipt.schedulerMetrics.maximumQueueLatencyNanoseconds);
	appendCounterField(json, "workerBusyNanoseconds",
		receipt.schedulerMetrics.workerBusyNanoseconds);
	appendCounterField(json, "workerWaitNanoseconds",
		receipt.schedulerMetrics.workerWaitNanoseconds);
	appendCounterField(json, "affinityFailureCount",
		receipt.schedulerMetrics.affinityFailureCount);
	appendUnsignedField(json, "injectionHighWater",
		receipt.schedulerMetrics.injectionHighWater);
	appendUnsignedField(json, "maximumActiveWorkers",
		receipt.schedulerMetrics.maximumActiveWorkers);
	appendUnsignedField(json, "availableLogicalCpuCount",
		receipt.schedulerMetrics.availableLogicalCpuCount);
	appendUnsignedField(json, "reservedOwnerCpuCount",
		receipt.schedulerMetrics.reservedOwnerCpuCount);
	appendUnsignedField(json, "selectedWorkerCpuCount",
		receipt.schedulerMetrics.selectedWorkerCpuCount);
	appendUnsignedField(json, "selectedWorkerPhysicalCoreCount",
		receipt.schedulerMetrics.selectedWorkerPhysicalCoreCount);
	appendCounterField(json, "selectedWorkerPhysicalCoreMask",
		receipt.schedulerMetrics.selectedWorkerPhysicalCoreMask);
	appendBoolField(json, "selectedWorkerPhysicalCoreMaskComplete",
		receipt.schedulerMetrics.selectedWorkerPhysicalCoreMaskComplete,
		false);
	json << "\n},\n";
	appendKey(json, "phases");
	json << "[\n";
	for (std::size_t index = 0; index < receipt.phases.size(); ++index)
		appendPhase(json, receipt.phases[index],
			index + 1 != receipt.phases.size());
	json << "\n],\n";
	appendKey(json, "kernels");
	json << "[\n";
	for (std::size_t index = 0; index < receipt.kernels.size(); ++index)
		appendKernel(json, receipt.kernels[index],
			index + 1 != receipt.kernels.size());
	json << "\n]\n}\n";
	document = json.str();
	return !document.empty();
}

bool ValidatePerformanceReceipt(const PerformanceReceipt &receipt,
	std::string *reason)
{
	if (receipt.schemaVersion != PERFORMANCE_RECEIPT_SCHEMA_VERSION ||
		receipt.producer != PERFORMANCE_RECEIPT_PRODUCER ||
		receipt.evidenceKind != PERFORMANCE_RECEIPT_EVIDENCE_KIND)
	{
		setReason(reason, "receipt schema or producer is not recognized");
		return false;
	}
	if (receipt.status != "complete")
	{
		setReason(reason, "receipt is not complete");
		return false;
	}
	if (receipt.title.empty() || receipt.runId.empty() ||
		receipt.sourceCommit.empty() || receipt.commandLine.empty() ||
		receipt.fixtureId.empty() || !isSafeToken(receipt.runId) ||
		!isLowerHexString(receipt.sourceCommit, 40) ||
		!isSafeToken(receipt.fixtureId))
	{
		setReason(reason, "receipt identity fields are incomplete");
		return false;
	}
	if (!isHexString(receipt.artifactSetSha256, 64) ||
		!isHexString(receipt.fixtureContentSha256, 64) ||
		!isHexString(receipt.executableSha256, 64) ||
		receipt.executablePath.empty())
	{
		setReason(reason, "binary or fixture hash identity is incomplete");
		return false;
	}
	if (!receipt.processIdentityAvailable || receipt.processId == 0 ||
		receipt.processCreationTimeUtc100ns == 0 ||
		receipt.processStartTimeUtc100ns == 0 ||
		receipt.processEndTimeUtc100ns < receipt.processStartTimeUtc100ns ||
		!receipt.processExitCodeKnown || receipt.processExitCode != 0 ||
		receipt.processExitBoundary.empty())
	{
		setReason(reason, "process provenance or exit boundary is unavailable");
		return false;
	}
	if (!receipt.seedKnown || receipt.frameEnd < receipt.frameStart ||
		receipt.finalFrame != receipt.frameEnd || !receipt.finalCrcKnown)
	{
		setReason(reason, "fixture result frame or CRC is incomplete");
		return false;
	}
	if ((receipt.workerPolicy != "auto" && receipt.workerPolicy != "all") ||
		!receipt.workersPinned ||
		receipt.effectiveWorkerCount == 0 ||
		receipt.availableLogicalCpuCount == 0 ||
		receipt.selectedWorkerCpuCount == 0 ||
		receipt.selectedWorkerPhysicalCoreCount == 0 ||
		receipt.selectedWorkerPhysicalCoreMask == 0 ||
		receipt.effectiveWorkerCount != receipt.selectedWorkerCpuCount ||
	!receipt.selectedWorkerPhysicalCoreMaskComplete)
	{
		setReason(reason, "effective worker or physical topology proof is incomplete");
		return false;
	}
	if (receipt.cpuSets.empty() ||
		receipt.selectedWorkerCpuSetIds.size() !=
			receipt.selectedWorkerCpuCount ||
		receipt.ownerCpuSetIds.size() != receipt.reservedOwnerCpuCount ||
		receipt.availableLogicalCpuCount < receipt.selectedWorkerCpuCount)
	{
		setReason(reason, "CPU-set topology arrays are incomplete");
		return false;
	}
	std::vector<unsigned> seenCpuSetIds;
	for (std::size_t index = 0; index < receipt.cpuSets.size(); ++index)
	{
		const PerformanceReceiptCpuSet &cpuSet = receipt.cpuSets[index];
		if (containsUnsigned(seenCpuSetIds, cpuSet.id))
		{
			setReason(reason, "CPU-set topology contains duplicate IDs");
			return false;
		}
		seenCpuSetIds.push_back(cpuSet.id);
	}
	std::vector<unsigned> selectedIds;
	for (std::size_t index = 0;
		index < receipt.selectedWorkerCpuSetIds.size(); ++index)
	{
		const unsigned id = receipt.selectedWorkerCpuSetIds[index];
		PerformanceReceiptCpuSet cpuSet;
		if (containsUnsigned(selectedIds, id) || !findCpuSet(receipt, id,
			&cpuSet) || !cpuSet.availableToProcess || cpuSet.parked ||
			cpuSet.allocatedToOtherProcess)
		{
			setReason(reason, "selected CPU-set is unavailable or duplicated");
			return false;
		}
		selectedIds.push_back(id);
	}
	for (std::size_t first = 0; first < selectedIds.size(); ++first)
	{
		PerformanceReceiptCpuSet firstSet;
		findCpuSet(receipt, selectedIds[first], &firstSet);
		for (std::size_t second = first + 1; second < selectedIds.size();
			++second)
		{
			PerformanceReceiptCpuSet secondSet;
			findCpuSet(receipt, selectedIds[second], &secondSet);
			if (firstSet.group == secondSet.group &&
				firstSet.coreIndex == secondSet.coreIndex)
			{
				setReason(reason, "selected CPU-sets share one physical core");
				return false;
			}
		}
	}
	if (selectedIds.size() != receipt.selectedWorkerPhysicalCoreCount)
	{
		setReason(reason, "selected physical-core count does not match CPU sets");
		return false;
	}
	std::vector<unsigned> ownerIds;
	for (std::size_t index = 0; index < receipt.ownerCpuSetIds.size(); ++index)
	{
		PerformanceReceiptCpuSet ownerSet;
		if (containsUnsigned(ownerIds, receipt.ownerCpuSetIds[index]) ||
			!findCpuSet(receipt, receipt.ownerCpuSetIds[index], &ownerSet) ||
			!ownerSet.availableToProcess || ownerSet.parked ||
			ownerSet.allocatedToOtherProcess)
		{
			setReason(reason, "owner CPU-set is absent or duplicated");
			return false;
		}
		else
			ownerIds.push_back(receipt.ownerCpuSetIds[index]);
	}
	if (receipt.rawEvidence.verifierBoundary.empty() ||
		receipt.rawEvidence.rawLogPath.empty() ||
		receipt.rawEvidence.timingPath.empty() ||
		!isHexString(receipt.rawEvidence.rawLogSha256, 64) ||
		!isHexString(receipt.rawEvidence.timingSha256, 64))
	{
		setReason(reason, "raw log or timing evidence boundary is incomplete");
		return false;
	}
	const unsigned requiredPhaseCount =
		static_cast<unsigned>(sizeof(REQUIRED_PHASE_NAMES) /
			sizeof(REQUIRED_PHASE_NAMES[0]));
	if (!hasUniqueNamesAndExactOrder(receipt.phases, REQUIRED_PHASE_NAMES,
		requiredPhaseCount))
	{
		setReason(reason, "executable phase metrics are not the exact canonical set");
		return false;
	}
	for (std::size_t index = 0; index < receipt.phases.size(); ++index)
	{
		const PerformanceReceiptPhase &phase = receipt.phases[index];
		if (phase.available && (phase.sampleCount == 0 ||
			phase.totalNanoseconds == 0 || phase.maximumNanoseconds == 0))
		{
			setReason(reason, "available phase metric has no positive timing");
			return false;
		}
		if (!phase.available && (phase.totalNanoseconds != 0 ||
			phase.maximumNanoseconds != 0 || phase.sampleCount != 0))
		{
			setReason(reason, "unavailable phase metric contains timing data");
			return false;
		}
	}
	const unsigned requiredKernelCount =
		static_cast<unsigned>(sizeof(REQUIRED_KERNEL_NAMES) /
			sizeof(REQUIRED_KERNEL_NAMES[0]));
	if (!hasUniqueNamesAndExactOrder(receipt.kernels, REQUIRED_KERNEL_NAMES,
		requiredKernelCount))
	{
		setReason(reason, "executable kernel metrics are not the exact canonical set");
		return false;
	}
	for (std::size_t index = 0; index < receipt.kernels.size(); ++index)
	{
		const PerformanceReceiptKernel &kernel = receipt.kernels[index];
		if (!kernel.available && (kernel.submittedJobs != 0 ||
			kernel.completedJobs != 0 || kernel.physicalWorkerJobs != 0 ||
			kernel.ownerHelpedJobs != 0 || kernel.physicalWorkerMask != 0 ||
			kernel.distinctPhysicalWorkers != 0 ||
			kernel.physicalWorkerMaskComplete ||
			kernel.elapsedNanoseconds != 0 || kernel.elapsedNanosecondsKnown))
		{
			setReason(reason, "unavailable kernel metric contains evidence");
			return false;
		}
		if (kernel.elapsedNanosecondsKnown &&
			kernel.elapsedNanoseconds == 0)
		{
			setReason(reason, "known kernel timing is not positive");
			return false;
		}
	}
	return true;
}

bool WritePerformanceReceiptAtomically(PerformanceReceipt &receipt,
	const char *directory, std::string *writtenPath, std::string *reason)
{
	if (writtenPath != 0)
		writtenPath->clear();
	if (directory == 0 || directory[0] == '\0')
	{
		setReason(reason, "receipt destination directory is missing");
		return false;
	}
#if defined(_WIN32)
	if (receipt.rawEvidence.rawLogSha256.empty() &&
		!receipt.rawEvidence.rawLogPath.empty())
		calculateFileSha256(receipt.rawEvidence.rawLogPath,
			receipt.rawEvidence.rawLogSha256);
	if (receipt.rawEvidence.timingSha256.empty() &&
		!receipt.rawEvidence.timingPath.empty())
		calculateFileSha256(receipt.rawEvidence.timingPath,
			receipt.rawEvidence.timingSha256);
#endif
	if (!ValidatePerformanceReceipt(receipt, reason))
		return false;
	std::string document;
	if (!SerializePerformanceReceipt(receipt, document, reason))
	{
		setReason(reason, "receipt serialization failed");
		return false;
	}
	std::ostringstream finalName;
	finalName << directory;
	const std::string directoryText(directory);
	if (!directoryText.empty() && directoryText[directoryText.size() - 1] != '\\' &&
		directoryText[directoryText.size() - 1] != '/')
		finalName << '\\';
	finalName << "performance-receipt-" << receipt.runId << '-'
		<< receipt.processId << ".json";
	const std::string finalPath = finalName.str();
#if defined(_WIN32)
	const DWORD attributes = GetFileAttributesA(directory);
	if (attributes == INVALID_FILE_ATTRIBUTES ||
		(attributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
	{
		setReason(reason, "receipt destination is not an existing directory");
		return false;
	}
	std::ostringstream temporaryName;
	temporaryName << finalPath << ".tmp-" << GetCurrentProcessId() << '-'
		<< GetTickCount();
	const std::string temporaryPath = temporaryName.str();
	HANDLE file = CreateFileA(temporaryPath.c_str(), GENERIC_WRITE, 0, 0,
		CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, 0);
	if (file == INVALID_HANDLE_VALUE)
	{
		setReason(reason, "temporary receipt creation failed");
		return false;
	}
	std::size_t offset = 0;
	bool success = true;
	while (offset < document.size())
	{
		const DWORD requested = static_cast<DWORD>(std::min<std::size_t>(
			document.size() - offset, 0xffffffffU));
		DWORD written = 0;
		if (!WriteFile(file, document.data() + offset, requested, &written, 0) ||
			written != requested)
		{
			success = false;
			break;
		}
		offset += written;
	}
	if (success && !FlushFileBuffers(file))
		success = false;
	CloseHandle(file);
	if (!success || !MoveFileExA(temporaryPath.c_str(), finalPath.c_str(),
		MOVEFILE_WRITE_THROUGH))
	{
		DeleteFileA(temporaryPath.c_str());
		setReason(reason, "atomic receipt publication failed");
		return false;
	}
#else
	const std::string temporaryPath = finalPath + ".tmp";
	{
		std::ofstream output(temporaryPath.c_str(),
			std::ios::out | std::ios::binary | std::ios::trunc);
		if (!output.is_open())
		{
			setReason(reason, "temporary receipt creation failed");
			return false;
		}
		output.write(document.data(), static_cast<std::streamsize>(document.size()));
		output.flush();
		if (!output.good())
		{
			setReason(reason, "temporary receipt write failed");
			return false;
		}
	}
	if (rename(temporaryPath.c_str(), finalPath.c_str()) != 0)
	{
		remove(temporaryPath.c_str());
		setReason(reason, "atomic receipt publication failed");
		return false;
	}
#endif
	if (writtenPath != 0)
		*writtenPath = finalPath;
	return true;
}

} }
