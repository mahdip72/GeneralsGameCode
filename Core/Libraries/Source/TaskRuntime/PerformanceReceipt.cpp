#include "Lib/PerformanceReceipt.h"

#include <algorithm>
#include <ctype.h>
#include <fstream>
#include <iomanip>
#include <limits.h>
#include <new>
#include <sstream>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <utility>

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
	"legacy-mutable-island",
	"spatial-work",
	"owner-tail",
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

bool isCanonicalUuid(const std::string &value)
{
	if (value.size() != 36 || value[8] != '-' || value[13] != '-' ||
		value[18] != '-' || value[23] != '-')
		return false;
	for (std::size_t index = 0; index < value.size(); ++index)
	{
		if (index == 8 || index == 13 || index == 18 || index == 23)
			continue;
		const unsigned char character =
			static_cast<unsigned char>(value[index]);
		if (!((character >= '0' && character <= '9') ||
			(character >= 'a' && character <= 'f') ||
			(character >= 'A' && character <= 'F')))
			return false;
	}
	const char version = value[14];
	const char variant = value[19];
	return version >= '1' && version <= '5' &&
		((variant >= '8' && variant <= '9') ||
		 (variant >= 'a' && variant <= 'b') ||
		 (variant >= 'A' && variant <= 'B'));
}

bool isIsoUtcTimestamp(const std::string &value)
{
	if (value.size() < 20 || value[4] != '-' || value[7] != '-' ||
		value[10] != 'T' || value[13] != ':' || value[16] != ':')
		return false;
	for (std::size_t index = 0; index < 19; ++index)
	{
		if (index == 4 || index == 7 || index == 10 || index == 13 ||
			index == 16)
			continue;
		if (value[index] < '0' || value[index] > '9')
			return false;
	}
	if (value[19] == 'Z')
		return true;
	if (value[19] != '.')
		return false;
	std::size_t index = 20;
	while (index < value.size() && value[index] >= '0' && value[index] <= '9')
		++index;
	return index > 20 && index < value.size() && index + 1 == value.size() &&
		value[index] == 'Z';
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
	unsigned parsed = 0;
	for (std::size_t index = 0; index != text.size(); ++index)
	{
		if (text[index] < '0' || text[index] > '9')
			return false;
		const unsigned digit = static_cast<unsigned>(text[index] - '0');
		if (parsed > (UINT_MAX - digit) / 10)
			return false;
		parsed = parsed * 10 + digit;
	}
	value = parsed;
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

bool currentUtcTimestamp(std::string &value)
{
	SYSTEMTIME now;
	GetSystemTime(&now);
	std::ostringstream text;
	text << std::setfill('0') << std::setw(4) << now.wYear << '-'
		<< std::setw(2) << now.wMonth << '-' << std::setw(2) << now.wDay
		<< 'T' << std::setw(2) << now.wHour << ':' << std::setw(2)
		<< now.wMinute << ':' << std::setw(2) << now.wSecond << '.'
		<< std::setw(3) << now.wMilliseconds << 'Z';
	value = text.str();
	return isIsoUtcTimestamp(value);
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

struct PerformanceReceiptFileIdentity
{
	PerformanceReceiptFileIdentity() : volume(0), indexHigh(0), indexLow(0), size(0) {}
	DWORD volume, indexHigh, indexLow;
	unsigned long long size;
};

bool queryPerformanceReceiptFile(HANDLE file, const std::string &expectedPath,
	PerformanceReceiptFileIdentity &identity)
{
	BY_HANDLE_FILE_INFORMATION information = {};
	char nativePath[32768] = {};
	char fullPath[32768] = {};
	const DWORD nativeLength = GetFinalPathNameByHandleA(file, nativePath,
		static_cast<DWORD>(sizeof(nativePath)), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
	const DWORD fullLength = GetFullPathNameA(expectedPath.c_str(),
		static_cast<DWORD>(sizeof(fullPath)), fullPath, 0);
	if (!GetFileInformationByHandle(file, &information) ||
		nativeLength < 7 || nativeLength >= sizeof(nativePath) ||
		strncmp(nativePath, "\\\\?\\", 4) != 0 ||
		fullLength == 0 || fullLength >= sizeof(fullPath) ||
		_stricmp(nativePath + 4, fullPath) != 0 ||
		(information.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0 ||
		information.nNumberOfLinks != 1)
		return false;
	identity.volume = information.dwVolumeSerialNumber;
	identity.indexHigh = information.nFileIndexHigh;
	identity.indexLow = information.nFileIndexLow;
	identity.size = (static_cast<unsigned long long>(information.nFileSizeHigh) << 32) |
		information.nFileSizeLow;
	return true;
}

bool samePerformanceReceiptFile(const PerformanceReceiptFileIdentity &left,
	const PerformanceReceiptFileIdentity &right)
{
	return left.volume == right.volume && left.indexHigh == right.indexHigh &&
		left.indexLow == right.indexLow && left.size == right.size;
}

bool calculateFileSha256(const std::string &path, std::string &digest)
{
	digest.clear();
	HANDLE file = CreateFileA(path.c_str(), GENERIC_READ,
		FILE_SHARE_READ, 0, OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, 0);
	if (file == INVALID_HANDLE_VALUE)
		return false;
	const unsigned long long maximumBytes = 16ULL * 1024ULL * 1024ULL * 1024ULL;
	PerformanceReceiptFileIdentity initialIdentity;
	bool success = queryPerformanceReceiptFile(file, path, initialIdentity) &&
		initialIdentity.size <= maximumBytes;
	BCRYPT_ALG_HANDLE algorithm = 0;
	BCRYPT_HASH_HANDLE hash = 0;
	DWORD objectLength = 0;
	DWORD digestLength = 0;
	DWORD propertyBytes = 0;
	success = success && BCryptOpenAlgorithmProvider(&algorithm,
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
	unsigned long long remaining = initialIdentity.size;
	while (success && remaining != 0)
	{
		const DWORD requested = static_cast<DWORD>((std::min<unsigned long long>)(
			remaining, sizeof(buffer)));
		DWORD read = 0;
		if (!ReadFile(file, buffer, requested, &read, 0) || read != requested)
		{
			success = false;
			break;
		}
		success = BCryptHashData(hash, buffer, read, 0) == 0;
		remaining -= read;
	}
	unsigned char extra = 0;
	DWORD extraCount = 0;
	PerformanceReceiptFileIdentity finalIdentity;
	if (success && (!ReadFile(file, &extra, 1, &extraCount, 0) || extraCount != 0 ||
		!queryPerformanceReceiptFile(file, path, finalIdentity) ||
		!samePerformanceReceiptFile(initialIdentity, finalIdentity))) success = false;
	unsigned char result[32] = { 0 };
	if (success)
		success = BCryptFinishHash(hash, result, digestLength, 0) == 0;
	if (hash != 0)
		success = BCryptDestroyHash(hash) == 0 && success;
	if (algorithm != 0)
		success = BCryptCloseAlgorithmProvider(algorithm, 0) == 0 && success;
	success = CloseHandle(file) != FALSE && success;
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
	const PerformanceReceiptPhase &phase, bool comma, bool v6 = false)
{
	json << "{\n";
	appendStringField(json, "name", phase.name);
	appendBoolField(json, "available", phase.available);
	appendCounterField(json, "totalNanoseconds", phase.totalNanoseconds);
	appendCounterField(json, "maximumNanoseconds",
		phase.maximumNanoseconds);
	appendCounterField(json, "sampleCount", phase.sampleCount);
	appendCounterField(json, "serialNanoseconds", phase.serialNanoseconds);
	appendBoolField(json, "serialNanosecondsKnown", phase.serialNanosecondsKnown, v6);
	if (v6)
	{
		appendCounterField(json, "pureNanoseconds", phase.pureNanoseconds);
		appendBoolField(json, "pureNanosecondsKnown", phase.pureNanosecondsKnown, false);
	}
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

void appendRawLog(std::ostringstream &json, const char *name,
	const std::string &path, const std::string &sha256, bool comma)
{
	json << "{\n";
	appendStringField(json, "name", name);
	appendStringField(json, "path", path);
	appendStringField(json, "sha256", sha256, false);
	json << "\n}";
	if (comma) json << ',';
}

void appendKernelTiming(std::ostringstream &json,
	const KernelPerformanceSnapshot &snapshot)
{
	static const char *const stages[] = { "capture", "schedule", "wait", "validate", "commit" };
	json << "\"kernelTiming\":{\n";
	appendUnsignedField(json, "schemaVersion", 1);
	const bool baseline = snapshot.runRole == KERNEL_PERFORMANCE_PHASE_SERIAL_BASELINE;
	appendStringField(json, "mode", baseline ? "owner-inline-baseline-observation" : "owner-pipeline-observation");
	appendStringField(json, "attribution", baseline ? "owner-inline-baseline-exclusive-v1" : "owner-stack-exclusive-v1");
	appendBoolField(json, "enabled", snapshot.enabled);
	appendBoolField(json, "frozen", snapshot.frozen);
	appendBoolField(json, "complete", snapshot.complete);
	appendUnsignedField(json, "errors", snapshot.errors);
	appendCounterField(json, "generation", snapshot.generation);
	// This ledger never executes a serial reference. Unknown is not zero cost.
	appendBoolField(json, "serialReferenceKnown", false);
	json << "\"streams\":[";
	for (unsigned index = 0; index != snapshot.streamCount; ++index)
	{
		if (index != 0) json << ',';
		const KernelPerformanceStream &stream = snapshot.streams[index];
		json << '{';
		appendStringField(json, "name", REQUIRED_KERNEL_NAMES[stream.kernel]);
		appendUnsignedField(json, "subtype", stream.subtype);
		appendCounterField(json, "attemptedBatches", stream.attemptedBatches);
		appendCounterField(json, "admittedBatches", stream.admittedBatches);
		appendCounterField(json, "committedBatches", stream.committedBatches);
		appendCounterField(json, "abortedBatches", stream.abortedBatches);
		appendUnsignedField(json, "firstFrame", stream.firstFrame);
		appendUnsignedField(json, "lastFrame", stream.lastFrame);
		appendCounterField(json, "activePipelineNanoseconds", stream.activePipelineNanoseconds);
		appendCounterField(json, "inclusiveBatchNanoseconds", stream.inclusiveBatchNanoseconds);
		appendCounterField(json, "maximumBatchNanoseconds", stream.maximumBatchNanoseconds);
		json << "\"stages\":[";
		for (unsigned stage = 0; stage != KERNEL_PERFORMANCE_STAGE_COUNT; ++stage)
		{
			if (stage != 0) json << ',';
			json << '{';
			appendStringField(json, "name", stages[stage]);
			appendCounterField(json, "totalNanoseconds", stream.stageNanoseconds[stage]);
			appendCounterField(json, "sampleCount", stream.stageSamples[stage], false);
			json << '}';
		}
		json << "]}";
	}
	json << "]}";
}

bool validKernelTimingStorage(const KernelPerformanceSnapshot &snapshot)
{
	if (snapshot.streamCount > KERNEL_PERFORMANCE_MAXIMUM_STREAMS) return false;
	for (unsigned index = 0; index != snapshot.streamCount; ++index)
		if (snapshot.streams[index].kernel < KERNEL_PERFORMANCE_PHYSICS ||
			snapshot.streams[index].kernel >= KERNEL_PERFORMANCE_KERNEL_COUNT)
			return false;
	return true;
}

bool validKernelTiming(const PerformanceReceipt &receipt)
{
	const KernelPerformanceSnapshot &snapshot = receipt.kernelTiming;
	const bool baseline = snapshot.runRole == KERNEL_PERFORMANCE_PHASE_SERIAL_BASELINE;
	if (!validKernelTimingStorage(snapshot) || !snapshot.enabled || !snapshot.frozen ||
		snapshot.errors != 0 || snapshot.generation == 0 ||
		snapshot.complete != (snapshot.streamCount != 0) ||
		(snapshot.runRole != KERNEL_PERFORMANCE_PIPELINE && !baseline) ||
		(baseline && (receipt.schemaVersion != 6 ||
			receipt.kernelReference.mode != KERNEL_REFERENCE_PHASE_BASELINE_BINDING))) return false;
	for (unsigned index = 0; index != snapshot.streamCount; ++index)
	{
		const KernelPerformanceStream &stream = snapshot.streams[index];
		const unsigned maximumSubtype = stream.kernel == KERNEL_PERFORMANCE_AI ||
			stream.kernel == KERNEL_PERFORMANCE_PATH ? 1 : 0;
		if (stream.subtype > maximumSubtype || stream.attemptedBatches == 0 ||
			stream.admittedBatches > stream.attemptedBatches ||
			stream.committedBatches > stream.admittedBatches ||
			stream.abortedBatches != stream.admittedBatches - stream.committedBatches ||
			stream.firstFrame > stream.lastFrame || stream.firstFrame < receipt.frameStart ||
			stream.lastFrame > receipt.frameEnd ||
			stream.activePipelineNanoseconds > stream.inclusiveBatchNanoseconds ||
			stream.maximumBatchNanoseconds > stream.inclusiveBatchNanoseconds) return false;
		for (unsigned prior = 0; prior != index; ++prior)
			if (snapshot.streams[prior].kernel == stream.kernel &&
				snapshot.streams[prior].subtype == stream.subtype) return false;
		JobMetricCounter total = 0;
		for (unsigned stage = 0; stage != KERNEL_PERFORMANCE_STAGE_COUNT; ++stage)
		{
			if (baseline && stage == KERNEL_PERFORMANCE_WAIT)
			{
				if (stream.stageNanoseconds[stage] != 0 || stream.stageSamples[stage] != 0) return false;
				continue;
			}
			if (stream.stageNanoseconds[stage] > ~static_cast<JobMetricCounter>(0) - total ||
				(stream.stageNanoseconds[stage] != 0 && stream.stageSamples[stage] == 0) ||
				stream.stageSamples[stage] < stream.committedBatches) return false;
			total += stream.stageNanoseconds[stage];
		}
		if (total != stream.activePipelineNanoseconds) return false;
	}
	return true;
}

const char *referenceModeName(KernelPerformanceReferenceMode mode)
{
	return mode == KERNEL_REFERENCE_THROUGHPUT_BINDING ? "throughput-binding" :
		(mode == KERNEL_REFERENCE_SERIAL_ORACLE ? "serial-oracle" :
		(mode == KERNEL_REFERENCE_PHASE_BASELINE_BINDING ? "phase-baseline-binding" : "disabled"));
}

std::string referenceDigestText(const KernelPerformanceDigest &digest)
{
	if (!digest.valid) return std::string();
	static const char hex[] = "0123456789ABCDEF";
	std::string text(64, '0');
	for (unsigned index = 0; index != 32; ++index)
	{
		text[index * 2] = hex[digest.bytes[index] >> 4];
		text[index * 2 + 1] = hex[digest.bytes[index] & 15];
	}
	return text;
}

void appendKernelReference(std::ostringstream &json,
	const KernelPerformanceReferenceSnapshot &snapshot)
{
	json << "\"kernelReference\":{\n";
	appendUnsignedField(json, "schemaVersion", 1);
	appendStringField(json, "mode", referenceModeName(snapshot.mode));
	appendBoolField(json, "frozen", snapshot.frozen);
	appendBoolField(json, "complete", snapshot.complete);
	appendUnsignedField(json, "errors", snapshot.errors);
	appendCounterField(json, "generation", snapshot.generation);
	json << "\"streams\":[";
	for (unsigned index = 0; index != snapshot.streamCount; ++index)
	{
		if (index != 0) json << ',';
		const KernelPerformanceReferenceStream &stream = snapshot.streams[index];
		json << '{';
		appendStringField(json, "name", REQUIRED_KERNEL_NAMES[stream.kernel]);
		appendUnsignedField(json, "subtype", stream.subtype);
		appendUnsignedField(json, "fieldSchema", stream.fieldSchema);
		appendUnsignedField(json, "firstFrame", stream.firstFrame);
		appendUnsignedField(json, "lastFrame", stream.lastFrame);
		appendCounterField(json, "validatedBatchCount", stream.validatedBatchCount);
		appendCounterField(json, "committedBatchCount", stream.committedBatchCount);
		appendCounterField(json, "abortedBatchCount", stream.abortedBatchCount);
		appendCounterField(json, "validatedOperationCount", stream.validatedOperationCount);
		appendCounterField(json, "committedOperationCount", stream.committedOperationCount);
		appendCounterField(json, "serialSampleCount", stream.serialSampleCount);
		appendCounterField(json, "serialNanoseconds", stream.serialNanoseconds);
		appendCounterField(json, "maximumSerialNanoseconds", stream.maximumSerialNanoseconds);
		appendStringField(json, "inputSha256", referenceDigestText(stream.inputDigest));
		appendStringField(json, "outputSha256", referenceDigestText(stream.outputDigest));
		appendStringField(json, "commitSha256", referenceDigestText(stream.commitDigest), false);
		json << '}';
	}
	json << "]}";
}

bool validKernelReferenceStorage(const KernelPerformanceReferenceSnapshot &snapshot)
{
	if (snapshot.streamCount > KERNEL_PERFORMANCE_MAXIMUM_STREAMS) return false;
	for (unsigned index = 0; index != snapshot.streamCount; ++index)
		if (snapshot.streams[index].kernel < KERNEL_PERFORMANCE_PHYSICS ||
			snapshot.streams[index].kernel >= KERNEL_PERFORMANCE_KERNEL_COUNT)
			return false;
	return true;
}

bool validKernelReference(const PerformanceReceipt &receipt)
{
	const KernelPerformanceReferenceSnapshot &snapshot = receipt.kernelReference;
	if (!validKernelReferenceStorage(snapshot) || !snapshot.frozen ||
		snapshot.errors != 0 || snapshot.generation == 0 ||
		snapshot.complete != (snapshot.streamCount != 0) ||
		(snapshot.mode != KERNEL_REFERENCE_THROUGHPUT_BINDING &&
		 snapshot.mode != KERNEL_REFERENCE_SERIAL_ORACLE &&
		 !(receipt.schemaVersion == 6 && snapshot.mode == KERNEL_REFERENCE_PHASE_BASELINE_BINDING))) return false;
	// Complete means observed accounting closed, not six-kernel qualification.
	// Missing streams remain useful local unknowns; present ones must reconcile.
	for (unsigned index = 0; index != snapshot.streamCount; ++index)
	{
		const KernelPerformanceReferenceStream &stream = snapshot.streams[index];
		const unsigned maximumSubtype = stream.kernel == KERNEL_PERFORMANCE_AI ||
			stream.kernel == KERNEL_PERFORMANCE_PATH ? 1 : 0;
		if (stream.subtype > maximumSubtype || stream.fieldSchema == 0 ||
			stream.validatedBatchCount == 0 ||
			stream.committedBatchCount > stream.validatedBatchCount ||
			stream.abortedBatchCount != stream.validatedBatchCount - stream.committedBatchCount ||
			stream.validatedOperationCount < stream.validatedBatchCount ||
			stream.committedOperationCount < stream.committedBatchCount ||
			stream.committedOperationCount > stream.validatedOperationCount ||
			stream.validatedOperationCount - stream.committedOperationCount < stream.abortedBatchCount ||
			(stream.committedBatchCount == 0 && stream.committedOperationCount != 0) ||
			(stream.abortedBatchCount == 0 && stream.validatedOperationCount != stream.committedOperationCount) ||
			stream.firstFrame > stream.lastFrame || stream.firstFrame < receipt.frameStart ||
			stream.lastFrame > receipt.frameEnd ||
			!stream.inputDigest.valid || !stream.outputDigest.valid || !stream.commitDigest.valid ||
			stream.maximumSerialNanoseconds > stream.serialNanoseconds) return false;
		for (unsigned prior = 0; prior != index; ++prior)
			if (snapshot.streams[prior].kernel == stream.kernel &&
				snapshot.streams[prior].subtype == stream.subtype) return false;
		const KernelPerformanceStream *timing = 0;
		for (unsigned timingIndex = 0; timingIndex != receipt.kernelTiming.streamCount; ++timingIndex)
			if (receipt.kernelTiming.streams[timingIndex].kernel == stream.kernel &&
				receipt.kernelTiming.streams[timingIndex].subtype == stream.subtype)
				timing = &receipt.kernelTiming.streams[timingIndex];
		if (timing == 0 || stream.committedBatchCount != timing->committedBatches ||
			stream.validatedBatchCount > timing->admittedBatches ||
			stream.firstFrame < timing->firstFrame || stream.lastFrame > timing->lastFrame) return false;
		if (snapshot.mode == KERNEL_REFERENCE_THROUGHPUT_BINDING ||
			snapshot.mode == KERNEL_REFERENCE_PHASE_BASELINE_BINDING)
		{
			if (stream.serialSampleCount != 0 || stream.serialNanoseconds != 0 ||
				stream.maximumSerialNanoseconds != 0) return false;
		}
		else if (stream.serialSampleCount != stream.committedBatchCount ||
			(stream.serialNanoseconds != 0 && stream.serialSampleCount == 0)) return false;
	}
	return true;
}

bool addReceiptCounter(JobMetricCounter &total, JobMetricCounter value)
{
	if (value > ~static_cast<JobMetricCounter>(0) - total) return false;
	total += value;
	return true;
}

bool validPhaseAccounting(const PerformanceReceipt &receipt)
{
	const KernelPerformancePhaseAccountingSnapshot &phase = receipt.kernelTiming.phaseAccounting;
	const bool baseline = receipt.kernelReference.mode == KERNEL_REFERENCE_PHASE_BASELINE_BINDING;
	if (!baseline)
		return !phase.requested && !phase.complete && phase.errors == 0 &&
			receipt.kernelTiming.runRole == KERNEL_PERFORMANCE_PIPELINE;
	if (receipt.schemaVersion != 6 || receipt.kernelTiming.runRole != KERNEL_PERFORMANCE_PHASE_SERIAL_BASELINE ||
		!phase.requested || !phase.frozen || !phase.complete || phase.errors != 0 ||
		!phase.schedulerClosureKnown || phase.completedFrameCount == 0 ||
		phase.firstCompletedFrame == 0 || phase.lastCompletedFrame < phase.firstCompletedFrame ||
		phase.completedFrameCount != static_cast<JobMetricCounter>(phase.lastCompletedFrame) - phase.firstCompletedFrame + 1 ||
		phase.completedFrameCount != receipt.workload.sampleCount ||
		phase.firstCompletedFrame != receipt.workload.firstFrame || phase.lastCompletedFrame != receipt.workload.lastFrame ||
		phase.frameNanoseconds == 0 || phase.maximumFrameNanoseconds > phase.frameNanoseconds ||
		(phase.completionSerialNanoseconds != 0 && phase.completionSampleCount == 0) ||
		receipt.phases.size() != KERNEL_PHASE_COUNT) return false;
	const KernelPerformanceSchedulerBoundary &begin = phase.schedulerBegin, &end = phase.schedulerEnd;
	if (begin.outstandingJobs != 0 || begin.pendingJobs != 0 || end.outstandingJobs != 0 || end.pendingJobs != 0 ||
		begin.submittedJobs != end.submittedJobs || begin.executedJobs != end.executedJobs ||
		begin.ownerHelpJobs != end.ownerHelpJobs) return false;
	if (phase.controlWindowCount == 0)
	{
		if (phase.firstControlSampleOrdinal != 0 || phase.lastControlSampleOrdinal != 0 ||
			phase.controlNanoseconds != 0 || phase.maximumControlNanoseconds != 0 ||
			phase.controlUnscopedSerialNanoseconds != 0) return false;
	}
	else if (phase.firstControlSampleOrdinal == 0 ||
		phase.lastControlSampleOrdinal < phase.firstControlSampleOrdinal ||
		phase.lastControlSampleOrdinal - phase.firstControlSampleOrdinal != phase.controlWindowCount - 1 ||
		phase.controlNanoseconds == 0 || phase.maximumControlNanoseconds > phase.controlNanoseconds) return false;
	JobMetricCounter total = phase.unscopedSerialNanoseconds, serial = total, pure = 0;
	for (unsigned index = 0; index != KERNEL_PHASE_COUNT; ++index)
	{
		const KernelPerformancePhaseAccountingRow &row = phase.phases[index];
		const PerformanceReceiptPhase &wire = receipt.phases[index];
		JobMetricCounter rowTotal = row.serialNanoseconds;
		if (!addReceiptCounter(rowTotal, row.pureNanoseconds) || rowTotal != row.totalNanoseconds ||
			row.samples != phase.completedFrameCount || row.maximumNanoseconds > row.totalNanoseconds ||
			!wire.available || wire.name != REQUIRED_PHASE_NAMES[index] ||
			!wire.serialNanosecondsKnown || !wire.pureNanosecondsKnown ||
			wire.totalNanoseconds != row.totalNanoseconds || wire.maximumNanoseconds != row.maximumNanoseconds ||
			wire.sampleCount != row.samples || wire.serialNanoseconds != row.serialNanoseconds ||
			wire.pureNanoseconds != row.pureNanoseconds || !addReceiptCounter(total, row.totalNanoseconds) ||
			!addReceiptCounter(serial, row.serialNanoseconds) || !addReceiptCounter(pure, row.pureNanoseconds)) return false;
	}
	if (total != phase.frameNanoseconds) return false;
	JobMetricCounter controlTotal = phase.controlUnscopedSerialNanoseconds;
	if (!addReceiptCounter(serial, phase.controlUnscopedSerialNanoseconds)) return false;
	for (unsigned index = 0; index != KERNEL_PHASE_COUNT; ++index)
	{
		const KernelPerformancePhaseAccountingRow &row = phase.controlPhases[index];
		JobMetricCounter rowTotal = row.serialNanoseconds;
		if (!addReceiptCounter(rowTotal, row.pureNanoseconds) || rowTotal != row.totalNanoseconds ||
			row.samples != phase.controlWindowCount || row.maximumNanoseconds > row.totalNanoseconds ||
			!addReceiptCounter(controlTotal, row.totalNanoseconds) ||
			!addReceiptCounter(serial, row.serialNanoseconds) || !addReceiptCounter(pure, row.pureNanoseconds)) return false;
	}
	if (controlTotal != phase.controlNanoseconds || !addReceiptCounter(total, controlTotal) ||
		!addReceiptCounter(total, phase.completionSerialNanoseconds) ||
		!addReceiptCounter(serial, phase.completionSerialNanoseconds) || !addReceiptCounter(serial, pure)) return false;
	return total == serial;
}

KernelPerformanceDigest receiptRunIdentity(const std::string &runId, const std::string &nonce,
	unsigned processId, JobMetricCounter creation)
{
	KernelPerformanceCanonicalWriter writer;
	writer.begin(0x5003);
	writer.sequence(1, static_cast<unsigned>(runId.size()));
	for (unsigned index = 0; index != runId.size(); ++index)
		writer.u32(1, static_cast<unsigned char>(runId[index]));
	writer.sequence(2, static_cast<unsigned>(nonce.size()));
	for (unsigned index = 0; index != nonce.size(); ++index)
		writer.u32(2, static_cast<unsigned char>(nonce[index]));
	writer.u32(3, processId);
	writer.u64(4, creation);
	return writer.finish();
}

bool validAttemptTrace(const PerformanceReceipt &receipt)
{
	const KernelPerformanceTraceSnapshot &trace = receipt.kernelReference.trace;
	const PerformanceReceiptTraceFiles &files = receipt.traceFiles;
	const bool baseline = receipt.kernelReference.mode == KERNEL_REFERENCE_PHASE_BASELINE_BINDING;
	if (!trace.requested)
		return !baseline && trace.mode == KERNEL_TRACE_DISABLED && !trace.complete && trace.errors == 0 &&
			files.tracePath.empty() && files.sourceReceiptPath.empty() && files.sourceRunId.empty() &&
			files.sourceRunNonce.empty() && files.sourceProcessId == 0 && files.sourceProcessCreationTimeUtc100ns == 0;
	if (receipt.schemaVersion != 6 || !trace.frozen || !trace.complete || trace.errors != 0 ||
		!trace.observationSealed || !trace.executionSealed || !trace.digest.valid ||
		!trace.binding.nativeRunIdentity.valid || !trace.binding.executable.valid ||
		!trace.binding.fixture.valid || !trace.binding.sourcePolicy.valid || files.tracePath.empty() ||
		(baseline ? trace.mode != KERNEL_TRACE_CONSUME :
			(trace.mode != KERNEL_TRACE_RECORD || receipt.kernelReference.mode != KERNEL_REFERENCE_THROUGHPUT_BINDING))) return false;
	const KernelPerformanceTraceLimits &limits = trace.limits;
	JobMetricCounter attempts = trace.admittedAttemptCount;
	JobMetricCounter windows = trace.completedWindowCount;
	if (trace.windowBoundaryCount == 0 || trace.windowBoundaryCount > trace.recordCount ||
		trace.completedWindowCount != receipt.workload.sampleCount ||
		!addReceiptCounter(windows, trace.controlWindowCount) || windows > trace.windowBoundaryCount ||
		(baseline && trace.controlWindowCount != receipt.kernelTiming.phaseAccounting.controlWindowCount)) return false;
	if (limits.maximumBytes == 0 || limits.maximumRecords == 0 || limits.maximumLogicalEvents == 0 ||
		limits.maximumAttempts == 0 || limits.maximumRanges == 0 || trace.byteCount == 0 ||
		trace.byteCount > limits.maximumBytes || trace.recordCount == 0 || trace.recordCount > limits.maximumRecords ||
		trace.logicalEventCount < trace.recordCount || trace.logicalEventCount > limits.maximumLogicalEvents ||
		(trace.coalescedSpanCount == 0 && trace.logicalEventCount != trace.recordCount) ||
		trace.coalescedSpanCount > trace.recordCount || trace.coalescedSpanCount > trace.coalescedAttemptCount ||
		trace.coalescedAttemptCount > trace.attemptCount || trace.attemptCount > limits.maximumAttempts ||
		!addReceiptCounter(attempts, trace.notAdmittedAttemptCount) || attempts != trace.attemptCount ||
		trace.abortedAfterAdmissionAttemptCount > trace.admittedAttemptCount || trace.reapCount != trace.attemptCount ||
		trace.capturedAttemptCount > trace.attemptCount || trace.rangeCount > limits.maximumRanges ||
		trace.releasedRangeCount != trace.rangeCount || trace.residentAttemptCapacity == 0 ||
		trace.residentRangeCapacity == 0 || trace.residentAttemptCount != 0 || trace.residentRangeCount != 0 ||
		trace.residentAttemptHighWater > trace.residentAttemptCapacity ||
		trace.residentRangeHighWater > trace.residentRangeCapacity) return false;
	KernelPerformanceDigest identity;
	if (baseline)
	{
		if (!trace.sourceReceiptDigest.valid || files.sourceReceiptPath.empty() ||
			!isSafeToken(files.sourceRunId) || !isCanonicalUuid(files.sourceRunNonce) ||
			files.sourceProcessId == 0 || files.sourceProcessCreationTimeUtc100ns == 0 ||
			files.sourceRunId == receipt.runId || files.sourceRunNonce == receipt.runNonce ||
			files.sourceProcessId == receipt.processId ||
			files.sourceProcessCreationTimeUtc100ns == receipt.processCreationTimeUtc100ns) return false;
		identity = receiptRunIdentity(files.sourceRunId, files.sourceRunNonce,
			files.sourceProcessId, files.sourceProcessCreationTimeUtc100ns);
	}
	else
	{
		if (trace.sourceReceiptDigest.valid || !files.sourceReceiptPath.empty() || !files.sourceRunId.empty() ||
			!files.sourceRunNonce.empty() || files.sourceProcessId != 0 || files.sourceProcessCreationTimeUtc100ns != 0) return false;
		identity = receiptRunIdentity(receipt.runId, receipt.runNonce, receipt.processId, receipt.processCreationTimeUtc100ns);
	}
	return identity.valid && referenceDigestText(identity) == referenceDigestText(trace.binding.nativeRunIdentity) &&
		referenceDigestText(trace.binding.executable) == receipt.executableSha256 &&
		referenceDigestText(trace.binding.fixture) == receipt.fixtureContentSha256;
}

void appendSchedulerBoundary(std::ostringstream &json, const KernelPerformanceSchedulerBoundary &boundary)
{
	json << '{';
	appendCounterField(json, "submittedJobs", boundary.submittedJobs);
	appendCounterField(json, "executedJobs", boundary.executedJobs);
	appendCounterField(json, "ownerHelpJobs", boundary.ownerHelpJobs);
	appendCounterField(json, "outstandingJobs", boundary.outstandingJobs);
	appendCounterField(json, "pendingJobs", boundary.pendingJobs, false);
	json << '}';
}

void appendPhaseAccounting(std::ostringstream &json, const KernelPerformancePhaseAccountingSnapshot &phase)
{
	json << "\"phaseAccounting\":";
	if (!phase.requested) { json << "null"; return; }
	json << '{';
	appendUnsignedField(json, "schemaVersion", 1);
	appendStringField(json, "mode", "owner-inline-source-admissions-v1");
	appendStringField(json, "accountingOrigin", "kernel-performance-ledger-whole-frame-v1");
	appendBoolField(json, "frozen", phase.frozen);
	appendBoolField(json, "complete", phase.complete);
	appendUnsignedField(json, "errors", phase.errors);
	appendCounterField(json, "completedFrameCount", phase.completedFrameCount);
	appendUnsignedField(json, "firstCompletedFrame", phase.firstCompletedFrame);
	appendUnsignedField(json, "lastCompletedFrame", phase.lastCompletedFrame);
	appendCounterField(json, "frameNanoseconds", phase.frameNanoseconds);
	appendCounterField(json, "maximumFrameNanoseconds", phase.maximumFrameNanoseconds);
	appendCounterField(json, "unscopedSerialNanoseconds", phase.unscopedSerialNanoseconds);
	appendCounterField(json, "completionSerialNanoseconds", phase.completionSerialNanoseconds);
	appendCounterField(json, "completionSampleCount", phase.completionSampleCount);
	appendBoolField(json, "schedulerClosureKnown", phase.schedulerClosureKnown);
	json << "\"schedulerBegin\":"; appendSchedulerBoundary(json, phase.schedulerBegin);
	json << ",\"schedulerEnd\":"; appendSchedulerBoundary(json, phase.schedulerEnd);
	json << ",\"controlAccounting\":{";
	appendCounterField(json, "windowCount", phase.controlWindowCount);
	appendCounterField(json, "firstSampleOrdinal", phase.firstControlSampleOrdinal);
	appendCounterField(json, "lastSampleOrdinal", phase.lastControlSampleOrdinal);
	appendCounterField(json, "totalNanoseconds", phase.controlNanoseconds);
	appendCounterField(json, "maximumNanoseconds", phase.maximumControlNanoseconds);
	appendCounterField(json, "unscopedSerialNanoseconds", phase.controlUnscopedSerialNanoseconds);
	json << "\"phases\":[";
	for (unsigned index = 0; index != KERNEL_PHASE_COUNT; ++index)
	{
		const KernelPerformancePhaseAccountingRow &row = phase.controlPhases[index];
		PerformanceReceiptPhase control;
		control.name = REQUIRED_PHASE_NAMES[index];
		control.available = phase.controlWindowCount != 0;
		control.totalNanoseconds = row.totalNanoseconds;
		control.maximumNanoseconds = row.maximumNanoseconds;
		control.sampleCount = row.samples;
		control.serialNanoseconds = row.serialNanoseconds;
		control.pureNanoseconds = row.pureNanoseconds;
		control.serialNanosecondsKnown = control.pureNanosecondsKnown = true;
		appendPhase(json, control, index + 1 != KERNEL_PHASE_COUNT, true);
	}
	json << "]}}";
}

void appendAttemptTrace(std::ostringstream &json, const PerformanceReceipt &receipt)
{
	const KernelPerformanceTraceSnapshot &trace = receipt.kernelReference.trace;
	json << "\"attemptTrace\":";
	if (!trace.requested) { json << "null"; return; }
	json << '{';
	appendUnsignedField(json, "schemaVersion", 1);
	appendStringField(json, "encoding", "typed-canonical-le-v1");
	appendUnsignedField(json, "fieldSchema", 0x5001);
	appendStringField(json, "mode", trace.mode == KERNEL_TRACE_RECORD ? "record" : "consume");
	appendBoolField(json, "frozen", trace.frozen);
	appendBoolField(json, "complete", trace.complete);
	appendUnsignedField(json, "errors", trace.errors);
	appendBoolField(json, "observationIngressSealed", trace.observationSealed);
	appendBoolField(json, "executionClosureSealed", trace.executionSealed);
	json << "\"file\":{";
	appendStringField(json, "path", receipt.traceFiles.tracePath);
	appendStringField(json, "sha256", referenceDigestText(trace.digest));
	appendCounterField(json, "byteCount", trace.byteCount, false);
	json << "},\"binding\":{";
	appendStringField(json, "nativeRunIdentitySha256", referenceDigestText(trace.binding.nativeRunIdentity));
	appendStringField(json, "executableSha256", referenceDigestText(trace.binding.executable));
	appendStringField(json, "fixtureSha256", referenceDigestText(trace.binding.fixture));
	appendStringField(json, "sourcePolicySha256", referenceDigestText(trace.binding.sourcePolicy), false);
	json << "},\"limits\":{";
	appendCounterField(json, "maximumBytes", trace.limits.maximumBytes);
	appendCounterField(json, "maximumRecords", trace.limits.maximumRecords);
	appendCounterField(json, "maximumLogicalEvents", trace.limits.maximumLogicalEvents);
	appendCounterField(json, "maximumAttempts", trace.limits.maximumAttempts);
	appendCounterField(json, "maximumRanges", trace.limits.maximumRanges, false);
	json << "},";
	appendCounterField(json, "residentAttemptCapacity", trace.residentAttemptCapacity);
	appendCounterField(json, "residentRangeCapacity", trace.residentRangeCapacity);
	appendCounterField(json, "residentAttemptCount", trace.residentAttemptCount);
	appendCounterField(json, "residentAttemptHighWater", trace.residentAttemptHighWater);
	appendCounterField(json, "residentRangeCount", trace.residentRangeCount);
	appendCounterField(json, "residentRangeHighWater", trace.residentRangeHighWater);
	appendCounterField(json, "recordCount", trace.recordCount);
	appendCounterField(json, "logicalEventCount", trace.logicalEventCount);
	appendCounterField(json, "coalescedSpanCount", trace.coalescedSpanCount);
	appendCounterField(json, "coalescedAttemptCount", trace.coalescedAttemptCount);
	appendCounterField(json, "attemptCount", trace.attemptCount);
	appendCounterField(json, "admittedAttemptCount", trace.admittedAttemptCount);
	appendCounterField(json, "notAdmittedAttemptCount", trace.notAdmittedAttemptCount);
	appendCounterField(json, "abortedAfterAdmissionAttemptCount", trace.abortedAfterAdmissionAttemptCount);
	appendCounterField(json, "reapCount", trace.reapCount);
	appendCounterField(json, "capturedAttemptCount", trace.capturedAttemptCount);
	appendCounterField(json, "capturedOperationCount", trace.capturedOperationCount);
	appendCounterField(json, "dispatchCount", trace.dispatchCount);
	appendCounterField(json, "rangeCount", trace.rangeCount);
	appendCounterField(json, "releasedRangeCount", trace.releasedRangeCount);
	appendCounterField(json, "windowBoundaryCount", trace.windowBoundaryCount);
	appendCounterField(json, "completedWindowCount", trace.completedWindowCount);
	appendCounterField(json, "controlWindowCount", trace.controlWindowCount);
	json << "\"sourceBinding\":";
	if (trace.mode == KERNEL_TRACE_CONSUME)
	{
		json << "{\"receipt\":{";
		appendStringField(json, "path", receipt.traceFiles.sourceReceiptPath);
		appendStringField(json, "sha256", referenceDigestText(trace.sourceReceiptDigest), false);
		json << "},";
		appendStringField(json, "runId", receipt.traceFiles.sourceRunId);
		appendStringField(json, "runNonce", receipt.traceFiles.sourceRunNonce);
		appendUnsignedField(json, "processId", receipt.traceFiles.sourceProcessId);
		appendCounterField(json, "processCreationTimeUtc100ns", receipt.traceFiles.sourceProcessCreationTimeUtc100ns, false);
		json << '}';
	}
	else json << "null";
	json << '}';
}

void appendRuntimeClosure(std::ostringstream &json,
	const PerformanceReceipt &receipt)
{
	json << "\"runtimeClosure\":{\n";
	appendStringField(json, "dependencyManifestSha256",
		receipt.runtimeClosureDependencyManifestSha256);
	appendStringField(json, "closureSha256", receipt.runtimeClosureSha256,
		false);
	json << "\n},\n";
}

#if defined(_WIN32)
bool fileTimeToUtcTimestamp(JobMetricCounter fileTimeValue,
	std::string &value)
{
	ULARGE_INTEGER integer;
	integer.QuadPart = static_cast<ULONGLONG>(fileTimeValue);
	FILETIME fileTime;
	fileTime.dwLowDateTime = integer.LowPart;
	fileTime.dwHighDateTime = integer.HighPart;
	SYSTEMTIME systemTime;
	if (!FileTimeToSystemTime(&fileTime, &systemTime))
		return false;
	std::ostringstream text;
	text << std::setfill('0') << std::setw(4) << systemTime.wYear << '-'
		<< std::setw(2) << systemTime.wMonth << '-' << std::setw(2)
		<< systemTime.wDay << 'T' << std::setw(2) << systemTime.wHour << ':'
		<< std::setw(2) << systemTime.wMinute << ':' << std::setw(2)
		<< systemTime.wSecond << '.' << std::setw(7)
		<< static_cast<unsigned long long>(integer.QuadPart % 10000000ULL)
		<< 'Z';
	value = text.str();
	return isIsoUtcTimestamp(value);
}
#endif

// Closed V6 source reader: one bounded cursor, directly into the existing
// receipt. No opaque subtree skipping, generic JSON nodes or file authority.
class PerformanceReceiptSourceReader
{
public:
	PerformanceReceiptSourceReader(const unsigned char *bytes, std::size_t size)
		: m_bytes(bytes), m_size(size), m_at(0), m_tokens(0), m_depth(0),
		  m_error("invalid source receipt"), m_provenanceProcessId(0)
	{
	}

	bool parse(PerformanceReceipt &receipt);
	const char *error() const { return m_error; }
	std::size_t offset() const { return m_at; }

private:
	bool fail(const char *message) { m_error = message; return false; }
	void whitespace()
	{
		while (m_at != m_size && (m_bytes[m_at] == ' ' || m_bytes[m_at] == '\t' ||
			m_bytes[m_at] == '\r' || m_bytes[m_at] == '\n')) ++m_at;
	}
	bool token()
	{
		whitespace();
		if (m_tokens == 262144) return fail("token limit exceeded");
		++m_tokens;
		return true;
	}
	bool punctuation(unsigned char value)
	{
		if (!token() || m_at == m_size || m_bytes[m_at] != value)
			return fail("missing JSON delimiter");
		++m_at;
		return true;
	}
	bool begin(unsigned char value)
	{
		if (m_depth == 16) return fail("container depth limit exceeded");
		if (!punctuation(value)) return false;
		++m_depth;
		return true;
	}
	bool end(unsigned char value)
	{
		if (!punctuation(value)) return false;
		--m_depth;
		return true;
	}
	bool ahead(unsigned char value)
	{
		whitespace();
		return m_at != m_size && m_bytes[m_at] == value;
	}
	bool literal(const char *value)
	{
		if (!token()) return false;
		const std::size_t length = strlen(value);
		if (length > m_size - m_at || memcmp(m_bytes + m_at, value, length) != 0)
			return fail("incorrect literal or field type");
		m_at += length;
		return true;
	}
	static int hex(unsigned char c)
	{
		if (c >= '0' && c <= '9') return c - '0';
		if (c >= 'a' && c <= 'f') return c - 'a' + 10;
		if (c >= 'A' && c <= 'F') return c - 'A' + 10;
		return -1;
	}
	bool hexUnit(unsigned &unit)
	{
		if (m_size - m_at < 4) return fail("truncated Unicode escape");
		unit = 0;
		for (unsigned index = 0; index != 4; ++index)
		{
			const int digit = hex(m_bytes[m_at++]);
			if (digit < 0) return fail("invalid Unicode escape");
			unit = unit * 16 + static_cast<unsigned>(digit);
		}
		return true;
	}
	bool appendScalar(std::string &value, unsigned scalar)
	{
		if (scalar == 0 || scalar > 0x10ffff || (scalar >= 0xd800 && scalar <= 0xdfff))
			return fail("invalid Unicode scalar or decoded NUL");
		const unsigned count = scalar < 0x80 ? 1 : scalar < 0x800 ? 2 : scalar < 0x10000 ? 3 : 4;
		if (value.size() > 131072 - count) return fail("decoded string limit exceeded");
		if (count == 1) value.push_back(static_cast<char>(scalar));
		else
		{
			const unsigned lead = count == 2 ? 0xc0 : count == 3 ? 0xe0 : 0xf0;
			value.push_back(static_cast<char>(lead | (scalar >> ((count - 1) * 6))));
			for (unsigned remaining = count - 1; remaining != 0; --remaining)
				value.push_back(static_cast<char>(0x80 | ((scalar >> ((remaining - 1) * 6)) & 0x3f)));
		}
		return true;
	}
	bool string(std::string &value)
	{
		if (!token() || m_at == m_size || m_bytes[m_at++] != '"')
			return fail("expected JSON string");
		value.clear();
		while (m_at != m_size)
		{
			unsigned scalar = m_bytes[m_at++];
			if (scalar == '"') return true;
			if (scalar < 0x20) return fail("unescaped string control");
			if (scalar == '\\')
			{
				if (m_at == m_size) return fail("truncated string escape");
				switch (m_bytes[m_at++])
				{
				case '"': scalar = '"'; break;
				case '\\': scalar = '\\'; break;
				case '/': scalar = '/'; break;
				case 'b': scalar = '\b'; break;
				case 'f': scalar = '\f'; break;
				case 'n': scalar = '\n'; break;
				case 'r': scalar = '\r'; break;
				case 't': scalar = '\t'; break;
				case 'u':
					if (!hexUnit(scalar)) return false;
					if (scalar >= 0xd800 && scalar <= 0xdbff)
					{
						if (m_size - m_at < 2 || m_bytes[m_at] != '\\' || m_bytes[m_at + 1] != 'u')
							return fail("unpaired high surrogate");
						m_at += 2;
						unsigned low = 0;
						if (!hexUnit(low) || low < 0xdc00 || low > 0xdfff)
							return fail("invalid low surrogate");
						scalar = 0x10000 + ((scalar - 0xd800) << 10) + (low - 0xdc00);
					}
					break;
				default: return fail("invalid string escape");
				}
			}
			else if (scalar >= 0x80)
			{
				const unsigned lead = scalar;
				const unsigned count = lead >= 0xc2 && lead <= 0xdf ? 2 :
					lead >= 0xe0 && lead <= 0xef ? 3 : lead >= 0xf0 && lead <= 0xf4 ? 4 : 0;
				if (count == 0 || m_size - m_at < count - 1) return fail("invalid UTF8 sequence");
				scalar = lead & (count == 2 ? 0x1f : count == 3 ? 0x0f : 0x07);
				for (unsigned index = 1; index != count; ++index)
				{
					const unsigned next = m_bytes[m_at++];
					if ((next & 0xc0) != 0x80) return fail("invalid UTF8 continuation");
					scalar = (scalar << 6) | (next & 0x3f);
				}
				if (scalar < (count == 2 ? 0x80u : count == 3 ? 0x800u : 0x10000u))
					return fail("overlong UTF8 sequence");
			}
			if (!appendScalar(value, scalar)) return false;
		}
		return fail("unterminated string");
	}
	bool read(std::string &value)
	{
		if (!string(value)) return false;
		for (std::size_t index = 0; index != value.size(); ++index)
			if (static_cast<unsigned char>(value[index]) < 0x20)
				return fail("control in path or identity text");
		return true;
	}
	bool read(JobMetricCounter &value)
	{
		if (!token() || m_at == m_size || m_bytes[m_at] < '0' || m_bytes[m_at] > '9')
			return fail("expected unsigned integer");
		const bool zero = m_bytes[m_at] == '0';
		value = 0;
		unsigned digits = 0;
		do
		{
			const unsigned digit = m_bytes[m_at++] - '0';
			if ((zero && digits != 0) || value > (~static_cast<JobMetricCounter>(0) - digit) / 10)
				return fail("noncanonical or overflowing integer");
			value = value * 10 + digit;
			++digits;
		} while (m_at != m_size && m_bytes[m_at] >= '0' && m_bytes[m_at] <= '9');
		// Separators are checked by the enclosing exact object/array. This also
		// rejects fractional/exponent suffixes rather than truncating a number.
		return true;
	}
	bool read(unsigned &value)
	{
		JobMetricCounter wide = 0;
		if (!read(wide) || wide > UINT_MAX) return fail("unsigned32 value out of range");
		value = static_cast<unsigned>(wide);
		return true;
	}
	bool read(bool &value)
	{
		if (ahead('t')) { value = true; return literal("true"); }
		value = false;
		return literal("false");
	}
	bool exact(const char *value)
	{
		std::string actual;
		return read(actual) && (actual == value || fail("incorrect fixed string"));
	}
	bool exact(unsigned value)
	{
		unsigned actual = 0;
		return read(actual) && (actual == value || fail("incorrect fixed integer"));
	}
	bool zeroExit(int &value)
	{
		value = 0;
		return exact(0u);
	}
	bool nullable(unsigned &value, bool &isNull)
	{
		isNull = ahead('n');
		value = 0;
		return isNull ? literal("null") : read(value);
	}
	bool digest(KernelPerformanceDigest &value)
	{
		std::string text;
		if (!read(text) || !isHexString(text, 64)) return fail("invalid SHA256 text");
		for (unsigned index = 0; index != 32; ++index)
			value.bytes[index] = static_cast<unsigned char>((hex(text[index * 2]) << 4) | hex(text[index * 2 + 1]));
		value.valid = true;
		return true;
	}
	bool kernelName(KernelPerformanceKernel &kernel)
	{
		std::string name;
		if (!read(name)) return false;
		for (unsigned index = 0; index != KERNEL_PERFORMANCE_KERNEL_COUNT; ++index)
			if (name == REQUIRED_KERNEL_NAMES[index])
			{
				kernel = static_cast<KernelPerformanceKernel>(index);
				return true;
			}
		return fail("unknown canonical kernel name");
	}
	template<std::size_t Count, typename ReadField>
	bool object(const char *const (&names)[Count], ReadField field)
	{
		static_assert(Count > 0 && Count <= 64, "source object field bound");
		if (!begin('{')) return false;
		JobMetricCounter seen = 0;
		unsigned members = 0;
		if (ahead('}')) return fail("required object fields missing");
		for (;;)
		{
			if (members == 64) return fail("object member limit exceeded");
			++members;
			std::string key;
			if (!string(key) || !punctuation(':')) return false;
			unsigned index = 0;
			while (index != Count && key != names[index]) ++index;
			if (index == Count) return fail("unknown object field");
			const JobMetricCounter bit = static_cast<JobMetricCounter>(1) << index;
			if ((seen & bit) != 0) return fail("duplicate decoded object field");
			seen |= bit;
			if (!field(index)) return false;
			if (ahead('}'))
			{
				if (members != Count) return fail("required object fields missing");
				return end('}');
			}
			if (!punctuation(',')) return false;
		}
	}
	template<typename ReadElement>
	bool array(unsigned maximum, unsigned &count, ReadElement element)
	{
		if (!begin('[')) return false;
		count = 0;
		if (ahead(']')) return end(']');
		for (;;)
		{
			if (count == maximum) return fail("array element limit exceeded");
			if (!element(count)) return false;
			++count;
			if (ahead(']')) return end(']');
			if (!punctuation(',')) return false;
		}
	}
	bool ids(std::vector<unsigned> &values)
	{
		unsigned count = 0;
		return array(4096, count, [&](unsigned) {
			unsigned value = 0;
			if (!read(value)) return false;
			values.push_back(value);
			return true;
		});
	}

	bool root(PerformanceReceipt &r);
	bool closure(PerformanceReceipt &r);
	bool process(PerformanceReceipt &r);
	bool fixture(PerformanceReceipt &r);
	bool workload(PerformanceReceiptWorkload &w);
	bool frameTiming(PerformanceReceipt &r);
	bool frames(PerformanceReceipt &r);
	bool worker(PerformanceReceipt &r);
	bool topology(PerformanceReceipt &r);
	bool cpuSet(PerformanceReceiptCpuSet &row);
	bool rawEvidence(PerformanceReceiptRawEvidence &r);
	bool rawLog(unsigned index);
	bool provenance(PerformanceReceipt &r);
	bool metrics(JobSystemMetrics &m);
	bool phase(PerformanceReceiptPhase &p);
	bool kernel(PerformanceReceiptKernel &k);
	bool timing(KernelPerformanceSnapshot &s);
	bool timingStream(KernelPerformanceStream &s);
	bool timingStage(KernelPerformanceStream &s, unsigned index);
	bool reference(KernelPerformanceReferenceSnapshot &s);
	bool referenceStream(KernelPerformanceReferenceStream &s);
	bool trace(PerformanceReceipt &r);
	bool traceFile(PerformanceReceipt &r);
	bool traceBinding(KernelPerformanceTraceBinding &b);
	bool traceLimits(KernelPerformanceTraceLimits &l);

	const unsigned char *m_bytes;
	std::size_t m_size, m_at;
	unsigned m_tokens, m_depth;
	const char *m_error;
	unsigned m_provenanceProcessId;
	std::string m_topologySource, m_creationUtc;
	std::string m_provenanceExecutable, m_provenanceSha, m_provenanceCommand;
	std::string m_rawPaths[2], m_rawHashes[2];
};

bool PerformanceReceiptSourceReader::root(PerformanceReceipt &r)
{
	unsigned count = 0;
	static const char *const names[] = {
		"schemaVersion", "producer", "evidenceKind", "status", "role", "measurementRole", "simulationMode",
		"schedulerStarted", "producerVersion", "title", "runId", "runNonce", "cohortNonce", "cohortCreatedUtc",
		"recordedUtc", "architecture", "sourceCommit", "artifactSetSha256", "runtimeClosure", "executablePath",
		"executableSha256", "commandLine", "process", "fixture", "workload", "frameSimulation", "frames", "worker",
		"topology", "rawEvidence", "rawLogs", "provenance", "schedulerMetrics", "phases", "kernels",
		"kernelTiming", "kernelReference", "phaseAccounting", "attemptTrace"
	};
	return object(names, [&](unsigned field) {
		switch (field)
		{
		case 0: return read(r.schemaVersion);
		case 1: return read(r.producer);
		case 2: return read(r.evidenceKind);
		case 3: return read(r.status);
		case 4: return read(r.role);
		case 5: return exact("throughput");
		case 6: return read(r.simulationMode);
		case 7: return read(r.schedulerStarted);
		case 8: return read(r.producerVersion);
		case 9: return read(r.title);
		case 10: return read(r.runId);
		case 11: return read(r.runNonce);
		case 12: return read(r.cohortNonce);
		case 13: return read(r.cohortCreatedUtc);
		case 14: return read(r.recordedUtc);
		case 15: return read(r.architecture);
		case 16: return read(r.sourceCommit);
		case 17: return read(r.artifactSetSha256);
		case 18: return closure(r);
		case 19: return read(r.executablePath);
		case 20: return read(r.executableSha256);
		case 21: return string(r.commandLine);
		case 22: return process(r);
		case 23: return fixture(r);
		case 24: return workload(r.workload);
		case 25: return frameTiming(r);
		case 26: return frames(r);
		case 27: return worker(r);
		case 28: return topology(r);
		case 29: return rawEvidence(r.rawEvidence);
		case 30: return array(2, count, [&](unsigned index) { return rawLog(index); }) &&
			(count == 2 || fail("raw log role array must have two rows"));
		case 31: return provenance(r);
		case 32: return metrics(r.schedulerMetrics);
		case 33: return array(5, count, [&](unsigned) {
				PerformanceReceiptPhase row;
				if (!phase(row)) return false;
				r.phases.push_back(row);
				return true;
			}) && (count == 5 || fail("phase array must have five rows"));
		case 34: return array(6, count, [&](unsigned) {
				PerformanceReceiptKernel row;
				if (!kernel(row)) return false;
				r.kernels.push_back(row);
				return true;
			}) && (count == 6 || fail("kernel array must have six rows"));
		case 35: return timing(r.kernelTiming);
		case 36: return reference(r.kernelReference);
		case 37: return literal("null");
		case 38: return trace(r);
		default: return fail("invalid source schema field");
		}
	});
}

bool PerformanceReceiptSourceReader::closure(PerformanceReceipt &r)
{
	static const char *const names[] = {
		"dependencyManifestSha256", "closureSha256"
	};
	return object(names, [&](unsigned field) {
		switch (field)
		{
		case 0: return read(r.runtimeClosureDependencyManifestSha256);
		case 1: return read(r.runtimeClosureSha256);
		default: return fail("invalid source schema field");
		}
	});
}

bool PerformanceReceiptSourceReader::process(PerformanceReceipt &r)
{
	static const char *const names[] = {
		"id", "creationTimeUtc100ns", "startTimeUtc100ns", "endTimeUtc100ns", "identityAvailable", "exitCodeKnown",
		"exitCode", "exitBoundary"
	};
	return object(names, [&](unsigned field) {
		switch (field)
		{
		case 0: return read(r.processId);
		case 1: return read(r.processCreationTimeUtc100ns);
		case 2: return read(r.processStartTimeUtc100ns);
		case 3: return read(r.processEndTimeUtc100ns);
		case 4: return read(r.processIdentityAvailable);
		case 5: return read(r.processExitCodeKnown);
		case 6: return zeroExit(r.processExitCode);
		case 7: return read(r.processExitBoundary);
		default: return fail("invalid source schema field");
		}
	});
}

bool PerformanceReceiptSourceReader::fixture(PerformanceReceipt &r)
{
	bool playersNull = false, unitsNull = false;
	static const char *const names[] = {
		"id", "kind", "workloadQualification", "contentPath", "identityObserved", "contentSha256", "replayPath",
		"retainedReplayPath", "retainedReplaySha256", "seed", "seedKnown", "requestedPlayerCount",
		"requestedMinimumUnitCount"
	};
	if (!object(names, [&](unsigned field) {
		switch (field)
		{
		case 0: return read(r.fixtureId);
		case 1: return read(r.fixtureKind);
		case 2: return read(r.workloadQualification);
		case 3: return read(r.fixtureContentPath);
		case 4: return read(r.fixtureIdentityObserved);
		case 5: return read(r.fixtureContentSha256);
		case 6: return read(r.replayPath);
		case 7: return read(r.retainedReplayPath);
		case 8: return read(r.retainedReplaySha256);
		case 9: return read(r.seed);
		case 10: return read(r.seedKnown);
		case 11: return nullable(r.requestedPlayerCount, playersNull);
		case 12: return nullable(r.requestedMinimumUnitCount, unitsNull);
		default: return fail("invalid source schema field");
		}
	})) return false;
	const bool observedOnly = r.workloadQualification == "observed-only";
	return (playersNull == observedOnly && unitsNull == observedOnly) ||
		fail("requested fixture counts disagree with qualification nullability");
}

bool PerformanceReceiptSourceReader::workload(PerformanceReceiptWorkload &w)
{
	static const char *const names[] = {
		"sampling", "sampleCount", "firstFrame", "lastFrame", "playerCount", "rosterStable", "contiguous",
		"initialUnitCount", "minimumUnitCount", "peakUnitCount"
	};
	return object(names, [&](unsigned field) {
		switch (field)
		{
		case 0: return exact("completed-simulation-frame-boundary-v1");
		case 1: return read(w.sampleCount);
		case 2: return read(w.firstFrame);
		case 3: return read(w.lastFrame);
		case 4: return read(w.playerCount);
		case 5: return read(w.rosterStable);
		case 6: return read(w.contiguous);
		case 7: return read(w.initialUnitCount);
		case 8: return read(w.minimumUnitCount);
		case 9: return read(w.peakUnitCount);
		default: return fail("invalid source schema field");
		}
	});
}

bool PerformanceReceiptSourceReader::frameTiming(PerformanceReceipt &r)
{
	static const char *const names[] = {
		"totalNanoseconds", "maximumNanoseconds", "sampleCount"
	};
	return object(names, [&](unsigned field) {
		switch (field)
		{
		case 0: return read(r.frameSimulationTotalNanoseconds);
		case 1: return read(r.frameSimulationMaximumNanoseconds);
		case 2: return read(r.frameSimulationSampleCount);
		default: return fail("invalid source schema field");
		}
	});
}

bool PerformanceReceiptSourceReader::frames(PerformanceReceipt &r)
{
	static const char *const names[] = {
		"start", "end", "final", "finalCrcKnown", "finalCrc"
	};
	return object(names, [&](unsigned field) {
		switch (field)
		{
		case 0: return read(r.frameStart);
		case 1: return read(r.frameEnd);
		case 2: return read(r.finalFrame);
		case 3: return read(r.finalCrcKnown);
		case 4: return read(r.finalCrc);
		default: return fail("invalid source schema field");
		}
	});
}

bool PerformanceReceiptSourceReader::worker(PerformanceReceipt &r)
{
	static const char *const names[] = {
		"requestedCount", "effectiveCount", "policy", "pinned", "availableLogicalCpuCount",
		"reservedOwnerCpuCount", "selectedWorkerCpuCount", "selectedWorkerPhysicalCoreCount",
		"selectedWorkerPhysicalCoreMask", "selectedWorkerPhysicalCoreMaskComplete"
	};
	return object(names, [&](unsigned field) {
		switch (field)
		{
		case 0: return read(r.requestedWorkerCount);
		case 1: return read(r.effectiveWorkerCount);
		case 2: return read(r.workerPolicy);
		case 3: return read(r.workersPinned);
		case 4: return read(r.availableLogicalCpuCount);
		case 5: return read(r.reservedOwnerCpuCount);
		case 6: return read(r.selectedWorkerCpuCount);
		case 7: return read(r.selectedWorkerPhysicalCoreCount);
		case 8: return read(r.selectedWorkerPhysicalCoreMask);
		case 9: return read(r.selectedWorkerPhysicalCoreMaskComplete);
		default: return fail("invalid source schema field");
		}
	});
}

bool PerformanceReceiptSourceReader::topology(PerformanceReceipt &r)
{
	unsigned count = 0;
	static const char *const names[] = {
		"source", "cpuSets", "ownerCpuSetIds", "selectedWorkerCpuSetIds"
	};
	return object(names, [&](unsigned field) {
		switch (field)
		{
		case 0: return read(m_topologySource);
		case 1: return array(4096, count, [&](unsigned) {
				PerformanceReceiptCpuSet row;
				if (!cpuSet(row)) return false;
				r.cpuSets.push_back(row);
				return true;
			});
		case 2: return ids(r.ownerCpuSetIds);
		case 3: return ids(r.selectedWorkerCpuSetIds);
		default: return fail("invalid source schema field");
		}
	});
}

bool PerformanceReceiptSourceReader::cpuSet(PerformanceReceiptCpuSet &row)
{
	static const char *const names[] = {
		"id", "efficiencyClass", "group", "coreIndex", "logicalProcessorIndex", "parked",
		"allocatedToOtherProcess", "availableToProcess"
	};
	return object(names, [&](unsigned field) {
		switch (field)
		{
		case 0: return read(row.id);
		case 1: return read(row.efficiencyClass);
		case 2: return read(row.group);
		case 3: return read(row.coreIndex);
		case 4: return read(row.logicalProcessorIndex);
		case 5: return read(row.parked);
		case 6: return read(row.allocatedToOtherProcess);
		case 7: return read(row.availableToProcess);
		default: return fail("invalid source schema field");
		}
	});
}

bool PerformanceReceiptSourceReader::rawEvidence(PerformanceReceiptRawEvidence &r)
{
	static const char *const names[] = {
		"verifierBoundary", "rawLogPath", "rawLogSha256", "timingPath", "timingSha256", "timingClosed",
		"timingWriteSucceeded", "timingTruncated", "timingComplete", "timingSessionCount", "timingFrameSamples",
		"timingFirstFrame", "timingLastFrame"
	};
	return object(names, [&](unsigned field) {
		switch (field)
		{
		case 0: return read(r.verifierBoundary);
		case 1: return read(r.rawLogPath);
		case 2: return read(r.rawLogSha256);
		case 3: return read(r.timingPath);
		case 4: return read(r.timingSha256);
		case 5: return read(r.timingClosed);
		case 6: return read(r.timingWriteSucceeded);
		case 7: return read(r.timingTruncated);
		case 8: return read(r.timingComplete);
		case 9: return read(r.timingSessionCount);
		case 10: return read(r.timingFrameSamples);
		case 11: return read(r.timingFirstFrame);
		case 12: return read(r.timingLastFrame);
		default: return fail("invalid source schema field");
		}
	});
}

bool PerformanceReceiptSourceReader::rawLog(unsigned index)
{
	static const char *const names[] = {
		"name", "path", "sha256"
	};
	return object(names, [&](unsigned field) {
		switch (field)
		{
		case 0: return exact(index == 0 ? "raw-log" : "timing");
		case 1: return read(m_rawPaths[index]);
		case 2: return read(m_rawHashes[index]);
		default: return fail("invalid source schema field");
		}
	});
}

bool PerformanceReceiptSourceReader::provenance(PerformanceReceipt &r)
{
	static const char *const names[] = {
		"kind", "receiptPath", "processId", "processCreationUtc", "executablePath", "executableSha256",
		"commandLine", "exitCode"
	};
	return object(names, [&](unsigned field) {
		switch (field)
		{
		case 0: return exact("native-executable-observation");
		case 1: return read(r.receiptPath);
		case 2: return read(m_provenanceProcessId);
		case 3: return read(m_creationUtc);
		case 4: return read(m_provenanceExecutable);
		case 5: return read(m_provenanceSha);
		case 6: return string(m_provenanceCommand);
		case 7: return exact(0u);
		default: return fail("invalid source schema field");
		}
	});
}

bool PerformanceReceiptSourceReader::metrics(JobSystemMetrics &m)
{
	static const char *const names[] = {
		"submittedJobCount", "executedJobCount", "stealCount", "ownerHelpCount", "waitCount",
		"workerWaitRejectionCount", "failedJobCount", "cancelledJobCount", "serialFallbackCount",
		"totalQueueLatencyNanoseconds", "maximumQueueLatencyNanoseconds", "workerBusyNanoseconds",
		"workerWaitNanoseconds", "affinityFailureCount", "injectionHighWater", "maximumActiveWorkers",
		"availableLogicalCpuCount", "reservedOwnerCpuCount", "selectedWorkerCpuCount",
		"selectedWorkerPhysicalCoreCount", "selectedWorkerPhysicalCoreMask",
		"selectedWorkerPhysicalCoreMaskComplete"
	};
	return object(names, [&](unsigned field) {
		switch (field)
		{
		case 0: return read(m.submittedJobCount);
		case 1: return read(m.executedJobCount);
		case 2: return read(m.stealCount);
		case 3: return read(m.ownerHelpCount);
		case 4: return read(m.waitCount);
		case 5: return read(m.workerWaitRejectionCount);
		case 6: return read(m.failedJobCount);
		case 7: return read(m.cancelledJobCount);
		case 8: return read(m.serialFallbackCount);
		case 9: return read(m.totalQueueLatencyNanoseconds);
		case 10: return read(m.maximumQueueLatencyNanoseconds);
		case 11: return read(m.workerBusyNanoseconds);
		case 12: return read(m.workerWaitNanoseconds);
		case 13: return read(m.affinityFailureCount);
		case 14: return read(m.injectionHighWater);
		case 15: return read(m.maximumActiveWorkers);
		case 16: return read(m.availableLogicalCpuCount);
		case 17: return read(m.reservedOwnerCpuCount);
		case 18: return read(m.selectedWorkerCpuCount);
		case 19: return read(m.selectedWorkerPhysicalCoreCount);
		case 20: return read(m.selectedWorkerPhysicalCoreMask);
		case 21: return read(m.selectedWorkerPhysicalCoreMaskComplete);
		default: return fail("invalid source schema field");
		}
	});
}

bool PerformanceReceiptSourceReader::phase(PerformanceReceiptPhase &p)
{
	static const char *const names[] = {
		"name", "available", "totalNanoseconds", "maximumNanoseconds", "sampleCount", "serialNanoseconds",
		"serialNanosecondsKnown", "pureNanoseconds", "pureNanosecondsKnown"
	};
	return object(names, [&](unsigned field) {
		switch (field)
		{
		case 0: return read(p.name);
		case 1: return read(p.available);
		case 2: return read(p.totalNanoseconds);
		case 3: return read(p.maximumNanoseconds);
		case 4: return read(p.sampleCount);
		case 5: return read(p.serialNanoseconds);
		case 6: return read(p.serialNanosecondsKnown);
		case 7: return read(p.pureNanoseconds);
		case 8: return read(p.pureNanosecondsKnown);
		default: return fail("invalid source schema field");
		}
	});
}

bool PerformanceReceiptSourceReader::kernel(PerformanceReceiptKernel &k)
{
	static const char *const names[] = {
		"name", "available", "submittedJobs", "completedJobs", "physicalWorkerJobs", "ownerHelpedJobs",
		"physicalWorkerMask", "distinctPhysicalWorkers", "physicalWorkerMaskComplete", "elapsedNanoseconds",
		"elapsedNanosecondsKnown"
	};
	return object(names, [&](unsigned field) {
		switch (field)
		{
		case 0: return read(k.name);
		case 1: return read(k.available);
		case 2: return read(k.submittedJobs);
		case 3: return read(k.completedJobs);
		case 4: return read(k.physicalWorkerJobs);
		case 5: return read(k.ownerHelpedJobs);
		case 6: return read(k.physicalWorkerMask);
		case 7: return read(k.distinctPhysicalWorkers);
		case 8: return read(k.physicalWorkerMaskComplete);
		case 9: return read(k.elapsedNanoseconds);
		case 10: return read(k.elapsedNanosecondsKnown);
		default: return fail("invalid source schema field");
		}
	});
}

bool PerformanceReceiptSourceReader::timing(KernelPerformanceSnapshot &s)
{
	s.runRole = KERNEL_PERFORMANCE_PIPELINE;
	static const char *const names[] = {
		"schemaVersion", "mode", "attribution", "enabled", "frozen", "complete", "errors", "generation",
		"serialReferenceKnown", "streams"
	};
	return object(names, [&](unsigned field) {
		switch (field)
		{
		case 0: return exact(1u);
		case 1: return exact("owner-pipeline-observation");
		case 2: return exact("owner-stack-exclusive-v1");
		case 3: return read(s.enabled);
		case 4: return read(s.frozen);
		case 5: return read(s.complete);
		case 6: return read(s.errors);
		case 7: return read(s.generation);
		case 8: return literal("false");
		case 9: return array(KERNEL_PERFORMANCE_MAXIMUM_STREAMS, s.streamCount,
				[&](unsigned index) { return timingStream(s.streams[index]); });
		default: return fail("invalid source schema field");
		}
	});
}

bool PerformanceReceiptSourceReader::timingStream(KernelPerformanceStream &s)
{
	unsigned count = 0;
	static const char *const names[] = {
		"name", "subtype", "attemptedBatches", "admittedBatches", "committedBatches", "abortedBatches",
		"firstFrame", "lastFrame", "activePipelineNanoseconds", "inclusiveBatchNanoseconds",
		"maximumBatchNanoseconds", "stages"
	};
	return object(names, [&](unsigned field) {
		switch (field)
		{
		case 0: return kernelName(s.kernel);
		case 1: return read(s.subtype);
		case 2: return read(s.attemptedBatches);
		case 3: return read(s.admittedBatches);
		case 4: return read(s.committedBatches);
		case 5: return read(s.abortedBatches);
		case 6: return read(s.firstFrame);
		case 7: return read(s.lastFrame);
		case 8: return read(s.activePipelineNanoseconds);
		case 9: return read(s.inclusiveBatchNanoseconds);
		case 10: return read(s.maximumBatchNanoseconds);
		case 11: return array(KERNEL_PERFORMANCE_STAGE_COUNT, count,
				[&](unsigned index) { return timingStage(s, index); }) &&
			(count == KERNEL_PERFORMANCE_STAGE_COUNT || fail("timing stages must have five rows"));
		default: return fail("invalid source schema field");
		}
	});
}

bool PerformanceReceiptSourceReader::timingStage(KernelPerformanceStream &s, unsigned index)
{
	static const char *const stages[] = { "capture", "schedule", "wait", "validate", "commit" };
	static const char *const names[] = {
		"name", "totalNanoseconds", "sampleCount"
	};
	return object(names, [&](unsigned field) {
		switch (field)
		{
		case 0: return exact(stages[index]);
		case 1: return read(s.stageNanoseconds[index]);
		case 2: return read(s.stageSamples[index]);
		default: return fail("invalid source schema field");
		}
	});
}

bool PerformanceReceiptSourceReader::reference(KernelPerformanceReferenceSnapshot &s)
{
	s.mode = KERNEL_REFERENCE_THROUGHPUT_BINDING;
	static const char *const names[] = {
		"schemaVersion", "mode", "frozen", "complete", "errors", "generation", "streams"
	};
	return object(names, [&](unsigned field) {
		switch (field)
		{
		case 0: return exact(1u);
		case 1: return exact("throughput-binding");
		case 2: return read(s.frozen);
		case 3: return read(s.complete);
		case 4: return read(s.errors);
		case 5: return read(s.generation);
		case 6: return array(KERNEL_PERFORMANCE_MAXIMUM_STREAMS, s.streamCount,
				[&](unsigned index) { return referenceStream(s.streams[index]); });
		default: return fail("invalid source schema field");
		}
	});
}

bool PerformanceReceiptSourceReader::referenceStream(KernelPerformanceReferenceStream &s)
{
	static const char *const names[] = {
		"name", "subtype", "fieldSchema", "firstFrame", "lastFrame", "validatedBatchCount", "committedBatchCount",
		"abortedBatchCount", "validatedOperationCount", "committedOperationCount", "serialSampleCount",
		"serialNanoseconds", "maximumSerialNanoseconds", "inputSha256", "outputSha256", "commitSha256"
	};
	return object(names, [&](unsigned field) {
		switch (field)
		{
		case 0: return kernelName(s.kernel);
		case 1: return read(s.subtype);
		case 2: return read(s.fieldSchema);
		case 3: return read(s.firstFrame);
		case 4: return read(s.lastFrame);
		case 5: return read(s.validatedBatchCount);
		case 6: return read(s.committedBatchCount);
		case 7: return read(s.abortedBatchCount);
		case 8: return read(s.validatedOperationCount);
		case 9: return read(s.committedOperationCount);
		case 10: return read(s.serialSampleCount);
		case 11: return read(s.serialNanoseconds);
		case 12: return read(s.maximumSerialNanoseconds);
		case 13: return digest(s.inputDigest);
		case 14: return digest(s.outputDigest);
		case 15: return digest(s.commitDigest);
		default: return fail("invalid source schema field");
		}
	});
}

bool PerformanceReceiptSourceReader::trace(PerformanceReceipt &r)
{
	KernelPerformanceTraceSnapshot &t = r.kernelReference.trace;
	t.requested = true;
	t.mode = KERNEL_TRACE_RECORD;
	static const char *const names[] = {
		"schemaVersion", "encoding", "fieldSchema", "mode", "frozen", "complete", "errors",
		"observationIngressSealed", "executionClosureSealed", "file", "binding", "limits",
		"residentAttemptCapacity", "residentRangeCapacity", "residentAttemptCount", "residentAttemptHighWater",
		"residentRangeCount", "residentRangeHighWater", "recordCount", "logicalEventCount", "coalescedSpanCount",
		"coalescedAttemptCount", "attemptCount", "admittedAttemptCount", "notAdmittedAttemptCount",
		"abortedAfterAdmissionAttemptCount", "reapCount", "capturedAttemptCount", "capturedOperationCount",
		"dispatchCount", "rangeCount", "releasedRangeCount", "windowBoundaryCount", "completedWindowCount",
		"controlWindowCount", "sourceBinding"
	};
	return object(names, [&](unsigned field) {
		switch (field)
		{
		case 0: return exact(1u);
		case 1: return exact("typed-canonical-le-v1");
		case 2: return exact(0x5001u);
		case 3: return exact("record");
		case 4: return read(t.frozen);
		case 5: return read(t.complete);
		case 6: return read(t.errors);
		case 7: return read(t.observationSealed);
		case 8: return read(t.executionSealed);
		case 9: return traceFile(r);
		case 10: return traceBinding(t.binding);
		case 11: return traceLimits(t.limits);
		case 12: return read(t.residentAttemptCapacity);
		case 13: return read(t.residentRangeCapacity);
		case 14: return read(t.residentAttemptCount);
		case 15: return read(t.residentAttemptHighWater);
		case 16: return read(t.residentRangeCount);
		case 17: return read(t.residentRangeHighWater);
		case 18: return read(t.recordCount);
		case 19: return read(t.logicalEventCount);
		case 20: return read(t.coalescedSpanCount);
		case 21: return read(t.coalescedAttemptCount);
		case 22: return read(t.attemptCount);
		case 23: return read(t.admittedAttemptCount);
		case 24: return read(t.notAdmittedAttemptCount);
		case 25: return read(t.abortedAfterAdmissionAttemptCount);
		case 26: return read(t.reapCount);
		case 27: return read(t.capturedAttemptCount);
		case 28: return read(t.capturedOperationCount);
		case 29: return read(t.dispatchCount);
		case 30: return read(t.rangeCount);
		case 31: return read(t.releasedRangeCount);
		case 32: return read(t.windowBoundaryCount);
		case 33: return read(t.completedWindowCount);
		case 34: return read(t.controlWindowCount);
		case 35: return literal("null");
		default: return fail("invalid source schema field");
		}
	});
}

bool PerformanceReceiptSourceReader::traceFile(PerformanceReceipt &r)
{
	static const char *const names[] = {
		"path", "sha256", "byteCount"
	};
	return object(names, [&](unsigned field) {
		switch (field)
		{
		case 0: return read(r.traceFiles.tracePath);
		case 1: return digest(r.kernelReference.trace.digest);
		case 2: return read(r.kernelReference.trace.byteCount);
		default: return fail("invalid source schema field");
		}
	});
}

bool PerformanceReceiptSourceReader::traceBinding(KernelPerformanceTraceBinding &b)
{
	static const char *const names[] = {
		"nativeRunIdentitySha256", "executableSha256", "fixtureSha256", "sourcePolicySha256"
	};
	return object(names, [&](unsigned field) {
		switch (field)
		{
		case 0: return digest(b.nativeRunIdentity);
		case 1: return digest(b.executable);
		case 2: return digest(b.fixture);
		case 3: return digest(b.sourcePolicy);
		default: return fail("invalid source schema field");
		}
	});
}

bool PerformanceReceiptSourceReader::traceLimits(KernelPerformanceTraceLimits &l)
{
	static const char *const names[] = {
		"maximumBytes", "maximumRecords", "maximumLogicalEvents", "maximumAttempts", "maximumRanges"
	};
	return object(names, [&](unsigned field) {
		switch (field)
		{
		case 0: return read(l.maximumBytes);
		case 1: return read(l.maximumRecords);
		case 2: return read(l.maximumLogicalEvents);
		case 3: return read(l.maximumAttempts);
		case 4: return read(l.maximumRanges);
		default: return fail("invalid source schema field");
		}
	});
}

// Validate the entire UTC token, including calendar components. The legacy
// diagnostic validator deliberately remains unchanged; source selection uses
// this stricter whole-token check and exact FILETIME projection below.
bool sourceReceiptUtc(const std::string &text)
{
	if (text.size() < 20 || text[4] != '-' || text[7] != '-' || text[10] != 'T' ||
		text[13] != ':' || text[16] != ':' || text.back() != 'Z') return false;
	const unsigned positions[] = { 0, 5, 8, 11, 14, 17 };
	unsigned values[6] = { 0 };
	for (unsigned part = 0; part != 6; ++part)
		for (unsigned digit = 0; digit != (part == 0 ? 4u : 2u); ++digit)
		{
			const char c = text[positions[part] + digit];
			if (c < '0' || c > '9') return false;
			values[part] = values[part] * 10 + static_cast<unsigned>(c - '0');
		}
	if (text.size() != 20)
	{
		if (text[19] != '.' || text.size() < 22 || text.size() > 28) return false;
		for (std::size_t index = 20; index + 1 != text.size(); ++index)
			if (text[index] < '0' || text[index] > '9') return false;
	}
	else if (text[19] != 'Z') return false;
	const unsigned year = values[0], month = values[1], day = values[2];
	const unsigned days[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
	if (year < 1601 || month == 0 || month > 12 || day == 0 ||
		values[3] > 23 || values[4] > 59 || values[5] > 59) return false;
	const bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
	return day <= days[month - 1] + (month == 2 && leap ? 1u : 0u);
}

bool PerformanceReceiptSourceReader::parse(PerformanceReceipt &r)
{
	if (!root(r)) return false;
	whitespace();
	if (m_at != m_size || m_depth != 0) return fail("trailing bytes after source receipt");
	if (r.schemaVersion != 6 || r.producer != "game-executable-stage5-performance-report-v6" ||
		r.producerVersion != "6" || !GetPerformanceReceiptRunIdentity(r).valid)
		return fail("invalid V6 source identity");
	if (!sourceReceiptUtc(r.cohortCreatedUtc) || !sourceReceiptUtc(r.recordedUtc) ||
		!sourceReceiptUtc(m_creationUtc)) return fail("invalid whole UTC timestamp");
	std::string projectedUtc;
	if (!fileTimeToUtcTimestamp(r.processCreationTimeUtc100ns, projectedUtc) ||
		projectedUtc != m_creationUtc) return fail("creation FILETIME and UTC disagree");
	if (m_provenanceProcessId != r.processId || m_provenanceExecutable != r.executablePath ||
		m_provenanceSha != r.executableSha256 || m_provenanceCommand != r.commandLine ||
		m_topologySource != (r.schedulerStarted ? "GetSystemCpuSetInformation" : "scheduler-not-started"))
		return fail("redundant provenance or topology fields disagree");
	if (m_rawPaths[0] != r.rawEvidence.rawLogPath || m_rawHashes[0] != r.rawEvidence.rawLogSha256 ||
		m_rawPaths[1] != r.rawEvidence.timingPath || m_rawHashes[1] != r.rawEvidence.timingSha256)
		return fail("redundant raw evidence fields disagree");
	return true;
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
	  sampleCount(0), serialNanoseconds(0), serialNanosecondsKnown(false),
	  pureNanoseconds(0), pureNanosecondsKnown(false)
{
}

PerformanceReceiptTraceFiles::PerformanceReceiptTraceFiles()
	: sourceProcessId(0), sourceProcessCreationTimeUtc100ns(0)
{
}

PerformanceReceiptWorkload::PerformanceReceiptWorkload()
	: sampleCount(0), firstFrame(0), lastFrame(0), playerCount(0),
	  initialUnitCount(0), minimumUnitCount(0), peakUnitCount(0),
	  rosterStable(true), contiguous(true)
{
}

bool IsPerformanceReceiptRosterPlayer(bool playableSide, bool observer)
{
	return playableSide && !observer;
}

bool IsPerformanceReceiptLiveUnit(bool infantry, bool vehicle,
	bool effectivelyDead, bool destroyed)
{
	return (infantry || vehicle) && !effectivelyDead && !destroyed;
}

bool ObservePerformanceReceiptWorkload(PerformanceReceiptWorkload &workload,
	unsigned frame, unsigned playerCount, unsigned liveUnitCount)
{
	if (workload.sampleCount != 0 && frame <= workload.lastFrame)
		return false;
	if (workload.sampleCount == 0)
	{
		workload.firstFrame = frame;
		workload.playerCount = playerCount;
		workload.initialUnitCount = liveUnitCount;
		workload.minimumUnitCount = liveUnitCount;
	}
	else
	{
		workload.contiguous = workload.contiguous &&
			static_cast<JobMetricCounter>(frame) ==
				static_cast<JobMetricCounter>(workload.lastFrame) + 1;
		workload.rosterStable = workload.rosterStable &&
			workload.playerCount == playerCount;
	}
	workload.lastFrame = frame;
	++workload.sampleCount;
	if (liveUnitCount < workload.minimumUnitCount)
		workload.minimumUnitCount = liveUnitCount;
	if (liveUnitCount > workload.peakUnitCount)
		workload.peakUnitCount = liveUnitCount;
	return true;
}

PerformanceReceiptKernel::PerformanceReceiptKernel()
	: available(false), submittedJobs(0), completedJobs(0),
	  physicalWorkerJobs(0), ownerHelpedJobs(0), physicalWorkerMask(0),
	  distinctPhysicalWorkers(0), physicalWorkerMaskComplete(false),
	  elapsedNanoseconds(0), elapsedNanosecondsKnown(false)
{
}

PerformanceReceiptRawEvidence::PerformanceReceiptRawEvidence()
	: timingClosed(false), timingWriteSucceeded(false), timingTruncated(false),
	  timingComplete(false), timingSessionCount(0), timingFrameSamples(0),
	  timingFirstFrame(0), timingLastFrame(0)
{
}

PerformanceReceipt::PerformanceReceipt()
	: schemaVersion(PERFORMANCE_RECEIPT_SCHEMA_VERSION),
	  producer(PERFORMANCE_RECEIPT_PRODUCER),
	  evidenceKind(PERFORMANCE_RECEIPT_EVIDENCE_KIND), status("pending"),
	  role("performance-report"), producerVersion("5"), architecture("x64"),
	  processId(0), processCreationTimeUtc100ns(0),
	  processStartTimeUtc100ns(0), processEndTimeUtc100ns(0),
	  processIdentityAvailable(false), processExitCode(0),
	  processExitCodeKnown(false), fixtureKind("replay"),
	  workloadQualification("minimum-qualified"), fixtureIdentityObserved(false), fixtureObservationFailed(false),
	  expectedSeed(0), expectedSeedKnown(false), seed(0), seedKnown(false),
	  requestedPlayerCount(0), requestedMinimumUnitCount(0),
	  frameSimulationTotalNanoseconds(0), frameSimulationMaximumNanoseconds(0),
	  frameSimulationSampleCount(0), frameStart(0), frameEnd(0), finalFrame(0),
	  finalCrcKnown(false), finalCrc(0), requestedWorkerCount(0),
	  simulationMode("unknown"), schedulerStarted(false),
	  effectiveWorkerCount(0), workersPinned(false),
	  availableLogicalCpuCount(0), reservedOwnerCpuCount(0),
	  selectedWorkerCpuCount(0), selectedWorkerPhysicalCoreCount(0),
	  selectedWorkerPhysicalCoreMask(0),
	  selectedWorkerPhysicalCoreMaskComplete(false)
{
	kernelReference.mode = KERNEL_REFERENCE_THROUGHPUT_BINDING;
}

KernelPerformanceDigest GetPerformanceReceiptRunIdentity(const PerformanceReceipt &receipt)
{
	const std::string &runId = receipt.runId;
	if (runId.empty() || runId.size() > 256 || runId.find("..") != std::string::npos ||
		!isCanonicalUuid(receipt.runNonce) || receipt.processId == 0 ||
		receipt.processCreationTimeUtc100ns == 0)
		return KernelPerformanceDigest();
	for (std::size_t index = 0; index != runId.size(); ++index)
	{
		const unsigned char character = static_cast<unsigned char>(runId[index]);
		if (!((character >= 'A' && character <= 'Z') ||
			(character >= 'a' && character <= 'z') ||
			(character >= '0' && character <= '9') ||
			character == '-' || character == '_' || character == '.'))
			return KernelPerformanceDigest();
	}
	return receiptRunIdentity(runId, receipt.runNonce, receipt.processId,
		receipt.processCreationTimeUtc100ns);
}

bool ParsePerformanceReceiptSource(const unsigned char *bytes, std::size_t byteCount,
	PerformanceReceipt &source, std::string *reason)
{
	// Invalidate prior authority without allocating, even if constructing the
	// normal empty receipt below subsequently fails. Parsed fields are never
	// published incrementally.
	source.status.clear();
	source.runId.clear();
	source.runNonce.clear();
	source.receiptPath.clear();
	source.fixtureId.clear();
	source.executablePath.clear();
	source.commandLine.clear();
	source.rawEvidence.rawLogPath.clear();
	source.rawEvidence.timingPath.clear();
	source.traceFiles.tracePath.clear();
	source.traceFiles.sourceReceiptPath.clear();
	source.traceFiles.sourceRunId.clear();
	source.traceFiles.sourceRunNonce.clear();
	source.traceFiles.sourceProcessId = 0;
	source.traceFiles.sourceProcessCreationTimeUtc100ns = 0;
	source.processId = 0;
	source.processCreationTimeUtc100ns = source.processStartTimeUtc100ns = source.processEndTimeUtc100ns = 0;
	source.processIdentityAvailable = source.processExitCodeKnown = false;
	source.kernelReference = KernelPerformanceReferenceSnapshot();
	source.kernelTiming = KernelPerformanceSnapshot();
	source.workload = PerformanceReceiptWorkload();
	source.cpuSets.clear();
	source.ownerCpuSetIds.clear();
	source.selectedWorkerCpuSetIds.clear();
	source.phases.clear();
	source.kernels.clear();
	try
	{
		source = PerformanceReceipt();
		if (bytes == 0 || byteCount == 0 || byteCount > 4194304)
		{
			setReason(reason, "source receipt byte extent is empty, null or above 4 MiB");
			return false;
		}
		PerformanceReceipt candidate;
		PerformanceReceiptSourceReader reader(bytes, byteCount);
		if (!reader.parse(candidate))
		{
			std::ostringstream detail;
			detail << "source receipt byte " << reader.offset() << ": " << reader.error();
			setReason(reason, detail.str());
			return false;
		}
		if (!ValidatePerformanceReceipt(candidate, reason)) return false;
		if (reason != 0) reason->clear();
		source = std::move(candidate);
		return true;
	}
	catch (const std::bad_alloc &)
	{
		// Keep output invalid when even a diagnostic allocation cannot succeed.
		if (reason != 0)
		{
			try { *reason = "source receipt allocation failed"; }
			catch (const std::bad_alloc &) { reason->clear(); }
		}
		return false;
	}
}

bool BeginPerformanceReceipt(PerformanceReceipt &receipt, const char *title,
	const char *replayPath, unsigned ordinal, std::string *reason)
{
	receipt = PerformanceReceipt();
	std::string qualification, fixtureKind;
	if (readEnvironment("RTS_PERFORMANCE_WORKLOAD_QUALIFICATION", qualification))
		receipt.workloadQualification = qualification;
	if (readEnvironment("RTS_PERFORMANCE_FIXTURE_KIND", fixtureKind))
		receipt.fixtureKind = fixtureKind;
	const bool observedOnly = receipt.workloadQualification == "observed-only";
	if ((!observedOnly && receipt.workloadQualification != "minimum-qualified") ||
		(receipt.fixtureKind != "replay" && receipt.fixtureKind != "fresh-ai-map") ||
		(receipt.fixtureKind == "fresh-ai-map" && !observedOnly))
	{
		setReason(reason, "fixture kind or workload qualification is invalid");
		return false;
	}
	if (title == 0 || title[0] == '\0' ||
		(receipt.fixtureKind == "replay" && (replayPath == 0 || replayPath[0] == '\0')) ||
		(receipt.fixtureKind == "fresh-ai-map" && replayPath != 0 && replayPath[0] != '\0'))
	{
		setReason(reason, "title and replay path are required");
		return false;
	}
	receipt.title = title;
	receipt.replayPath = replayPath != 0 ? replayPath : "";

	if (!readEnvironment("RTS_PERFORMANCE_ROLE", receipt.role) ||
		!readEnvironment("RTS_PERFORMANCE_RUN_ID", receipt.runId) ||
		!readEnvironment("RTS_PERFORMANCE_RUN_NONCE", receipt.runNonce) ||
		!readEnvironment("RTS_PERFORMANCE_COHORT_NONCE", receipt.cohortNonce) ||
		!readEnvironment("RTS_PERFORMANCE_COHORT_CREATED_UTC",
			receipt.cohortCreatedUtc) ||
		!readEnvironment("RTS_PERFORMANCE_RECEIPT_DIR",
			receipt.outputDirectory) ||
		!readEnvironment("RTS_PERFORMANCE_SOURCE_COMMIT", receipt.sourceCommit) ||
		!readEnvironment("RTS_PERFORMANCE_ARTIFACT_SET_SHA256",
			receipt.artifactSetSha256) ||
		!readEnvironment("RTS_PERFORMANCE_RUNTIME_MANIFEST_SHA256",
			receipt.runtimeClosureDependencyManifestSha256) ||
		!readEnvironment("RTS_PERFORMANCE_RUNTIME_CLOSURE_SHA256",
			receipt.runtimeClosureSha256) ||
		!readEnvironment("RTS_PERFORMANCE_FIXTURE_ID", receipt.fixtureId) ||
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
	if (receipt.role != "performance-report" ||
		!isCanonicalUuid(receipt.runNonce) ||
		!isCanonicalUuid(receipt.cohortNonce) ||
		!isIsoUtcTimestamp(receipt.cohortCreatedUtc) ||
		!currentUtcTimestamp(receipt.recordedUtc))
	{
		setReason(reason, "performance receipt role, nonce, or cohort timestamp is invalid");
		return false;
	}
	std::string referenceMode;
	if (readEnvironment("RTS_PERFORMANCE_REFERENCE_MODE", referenceMode))
	{
		if (referenceMode == "serial-oracle")
			receipt.kernelReference.mode = KERNEL_REFERENCE_SERIAL_ORACLE;
		else if (referenceMode == "phase-baseline-binding")
		{
			receipt.kernelReference.mode = KERNEL_REFERENCE_PHASE_BASELINE_BINDING;
			// A phase-baseline receipt carries the V6-only exact phase and trace
			// partitions. Keep the constructor's V5 default for ordinary
			// throughput/oracle runs, but promote the wire identity as soon as the
			// executable accepts this role so production validation can reach it.
			receipt.schemaVersion = 6;
			receipt.producer = "game-executable-stage5-performance-report-v6";
			receipt.producerVersion = "6";
		}
		else if (referenceMode != "throughput-binding")
		{
			setReason(reason, "RTS_PERFORMANCE_REFERENCE_MODE is invalid");
			return false;
		}
	}
	const bool expectedHashKnown = readEnvironment("RTS_PERFORMANCE_FIXTURE_SHA256",
		receipt.expectedFixtureContentSha256);
	if ((!observedOnly && !expectedHashKnown) ||
		(expectedHashKnown && !isHexString(receipt.expectedFixtureContentSha256, 64)) ||
		!loadOptionalUnsigned("RTS_PERFORMANCE_SEED", receipt.expectedSeed, receipt.expectedSeedKnown, reason) ||
		(!observedOnly && !receipt.expectedSeedKnown))
	{
		setReason(reason, "expected fixture hash or seed is missing or invalid");
		return false;
	}
	// Hashes are computed from the closed producer-owned files at publication.
	// Host-supplied hashes cannot stand in for an executable observation.
	bool playerCountKnown = false;
	bool unitCountKnown = false;
	if (!loadOptionalUnsigned("RTS_PERFORMANCE_PLAYER_COUNT",
		receipt.requestedPlayerCount, playerCountKnown, reason) ||
		!loadOptionalUnsigned("RTS_PERFORMANCE_UNIT_COUNT",
			receipt.requestedMinimumUnitCount, unitCountKnown, reason))
		return false;
	if ((observedOnly && (playerCountKnown || unitCountKnown)) ||
		(!observedOnly && (!playerCountKnown || !unitCountKnown ||
			receipt.requestedPlayerCount == 0 || receipt.requestedMinimumUnitCount == 0)))
	{
		setReason(reason, "requested performance workload environment is missing or zero");
		return false;
	}
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

bool BindPerformanceReceiptFixtureObservation(PerformanceReceipt &receipt,
	const char *kind, const char *contentPath, const char *observedContentSha256,
	unsigned observedSeed, std::string *reason)
{
	// Copy first: callers may pass strings already retained in this receipt.
	const std::string actualKind = kind != 0 ? kind : "";
	const std::string actualPath = contentPath != 0 ? contentPath : "";
	std::string actualHash = observedContentSha256 != 0 ? observedContentSha256 : "";
	std::string expectedHash = receipt.expectedFixtureContentSha256;
	for (std::size_t index = 0; index != actualHash.size(); ++index)
		actualHash[index] = static_cast<char>(toupper(static_cast<unsigned char>(actualHash[index])));
	for (std::size_t index = 0; index != expectedHash.size(); ++index)
		expectedHash[index] = static_cast<char>(toupper(static_cast<unsigned char>(expectedHash[index])));
	if (receipt.fixtureIdentityObserved || receipt.fixtureObservationFailed || actualKind != receipt.fixtureKind ||
		(actualKind != "replay" && actualKind != "fresh-ai-map") || actualPath.empty() || !isHexString(actualHash, 64) ||
		(!expectedHash.empty() && expectedHash != actualHash) ||
		(receipt.expectedSeedKnown && receipt.expectedSeed != observedSeed))
	{
		receipt.fixtureIdentityObserved = false;
		receipt.fixtureObservationFailed = true;
		setReason(reason, "actual fixture observation differs from its expected identity or was rebound");
		return false;
	}
	receipt.fixtureContentPath = actualPath;
	receipt.fixtureContentSha256 = actualHash;
	receipt.replayPath = actualKind == "replay" ? actualPath : "";
	receipt.seed = observedSeed;
	receipt.seedKnown = true;
	receipt.fixtureIdentityObserved = true;
	return true;
}

bool CapturePerformanceReceiptJobSystem(PerformanceReceipt &receipt,
	const JobSystem &jobs, const JobSystemMetrics &metrics,
	std::string *reason)
{
	const JobSystemConfig config = JobSystem::startupConfig();
	if (jobs.workerCount() != 0 || metrics.selectedWorkerCpuCount != 0)
		receipt.schedulerStarted = true;
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
	receipt.workersPinned = receipt.schedulerStarted && config.pinWorkers;
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
	receipt.status = clean ? "passed" : "failed";
	return true;
}

bool SerializePerformanceReceipt(const PerformanceReceipt &receipt,
	std::string &document, std::string *reason)
{
	if (!validKernelTimingStorage(receipt.kernelTiming) ||
		!validKernelReferenceStorage(receipt.kernelReference))
	{
		setReason(reason, "kernel evidence storage is outside its fixed bounds");
		return false;
	}
	std::ostringstream json;
	json << "{\n";
	appendUnsignedField(json, "schemaVersion", receipt.schemaVersion);
	appendStringField(json, "producer", receipt.producer);
	appendStringField(json, "evidenceKind", receipt.evidenceKind);
	appendStringField(json, "status", receipt.status);
	appendStringField(json, "role", receipt.role);
	appendStringField(json, "measurementRole",
		receipt.kernelReference.mode == KERNEL_REFERENCE_THROUGHPUT_BINDING ? "throughput" :
		(receipt.kernelReference.mode == KERNEL_REFERENCE_SERIAL_ORACLE ? "serial-oracle" :
		(receipt.kernelReference.mode == KERNEL_REFERENCE_PHASE_BASELINE_BINDING ? "phase-serial-baseline" : "disabled")));
	appendStringField(json, "simulationMode", receipt.simulationMode);
	appendBoolField(json, "schedulerStarted", receipt.schedulerStarted);
	appendStringField(json, "producerVersion", receipt.producerVersion);
	appendStringField(json, "title", receipt.title);
	appendStringField(json, "runId", receipt.runId);
	appendStringField(json, "runNonce", receipt.runNonce);
	appendStringField(json, "cohortNonce", receipt.cohortNonce);
	appendStringField(json, "cohortCreatedUtc", receipt.cohortCreatedUtc);
	appendStringField(json, "recordedUtc", receipt.recordedUtc);
	appendStringField(json, "architecture", receipt.architecture);
	appendStringField(json, "sourceCommit", receipt.sourceCommit);
	appendStringField(json, "artifactSetSha256", receipt.artifactSetSha256);
	appendRuntimeClosure(json, receipt);
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
	appendStringField(json, "kind", receipt.fixtureKind);
	appendStringField(json, "workloadQualification", receipt.workloadQualification);
	appendStringField(json, "contentPath", receipt.fixtureContentPath);
	appendBoolField(json, "identityObserved", receipt.fixtureIdentityObserved);
	appendStringField(json, "contentSha256", receipt.fixtureContentSha256);
	appendStringField(json, "replayPath", receipt.replayPath);
	appendStringField(json, "retainedReplayPath", receipt.retainedReplayPath);
	appendStringField(json, "retainedReplaySha256", receipt.retainedReplaySha256);
	appendUnsignedField(json, "seed", receipt.seed);
	appendBoolField(json, "seedKnown", receipt.seedKnown);
	if (receipt.workloadQualification == "observed-only")
		json << "\"requestedPlayerCount\":null,\"requestedMinimumUnitCount\":null";
	else
	{
		appendUnsignedField(json, "requestedPlayerCount", receipt.requestedPlayerCount);
		appendUnsignedField(json, "requestedMinimumUnitCount", receipt.requestedMinimumUnitCount, false);
	}
	json << "\n},\n";
	json << "\"workload\":{\n";
	appendStringField(json, "sampling", "completed-simulation-frame-boundary-v1");
	appendCounterField(json, "sampleCount", receipt.workload.sampleCount);
	appendUnsignedField(json, "firstFrame", receipt.workload.firstFrame);
	appendUnsignedField(json, "lastFrame", receipt.workload.lastFrame);
	appendUnsignedField(json, "playerCount", receipt.workload.playerCount);
	appendBoolField(json, "rosterStable", receipt.workload.rosterStable);
	appendBoolField(json, "contiguous", receipt.workload.contiguous);
	appendUnsignedField(json, "initialUnitCount", receipt.workload.initialUnitCount);
	appendUnsignedField(json, "minimumUnitCount", receipt.workload.minimumUnitCount);
	appendUnsignedField(json, "peakUnitCount", receipt.workload.peakUnitCount, false);
	json << "\n},\n";
	json << "\"frameSimulation\":{\n";
	appendCounterField(json, "totalNanoseconds", receipt.frameSimulationTotalNanoseconds);
	appendCounterField(json, "maximumNanoseconds", receipt.frameSimulationMaximumNanoseconds);
	appendCounterField(json, "sampleCount", receipt.frameSimulationSampleCount, false);
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
	appendStringField(json, "source", receipt.schedulerStarted ? "GetSystemCpuSetInformation" : "scheduler-not-started");
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
		receipt.rawEvidence.timingSha256);
	appendBoolField(json, "timingClosed", receipt.rawEvidence.timingClosed);
	appendBoolField(json, "timingWriteSucceeded", receipt.rawEvidence.timingWriteSucceeded);
	appendBoolField(json, "timingTruncated", receipt.rawEvidence.timingTruncated);
	appendBoolField(json, "timingComplete", receipt.rawEvidence.timingComplete);
	appendUnsignedField(json, "timingSessionCount", receipt.rawEvidence.timingSessionCount);
	appendCounterField(json, "timingFrameSamples", receipt.rawEvidence.timingFrameSamples);
	appendUnsignedField(json, "timingFirstFrame", receipt.rawEvidence.timingFirstFrame);
	appendUnsignedField(json, "timingLastFrame", receipt.rawEvidence.timingLastFrame, false);
	json << "\n},\n";
	json << "\"rawLogs\":[\n";
	appendRawLog(json, "raw-log", receipt.rawEvidence.rawLogPath,
		receipt.rawEvidence.rawLogSha256, true);
	appendRawLog(json, "timing", receipt.rawEvidence.timingPath,
		receipt.rawEvidence.timingSha256, false);
	json << "\n],\n";
	std::string processCreationUtc;
#if defined(_WIN32)
	if (!fileTimeToUtcTimestamp(receipt.processCreationTimeUtc100ns,
		processCreationUtc))
	{
		setReason(reason, "process creation timestamp serialization failed");
		return false;
	}
#endif
	json << "\"provenance\":{\n";
	appendStringField(json, "kind", "native-executable-observation");
	appendStringField(json, "receiptPath", receipt.receiptPath);
	appendUnsignedField(json, "processId", receipt.processId);
	appendStringField(json, "processCreationUtc", processCreationUtc);
	appendStringField(json, "executablePath", receipt.executablePath);
	appendStringField(json, "executableSha256", receipt.executableSha256);
	appendStringField(json, "commandLine", receipt.commandLine);
	appendUnsignedField(json, "exitCode",
		static_cast<unsigned>(receipt.processExitCode), false);
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
			index + 1 != receipt.phases.size(), receipt.schemaVersion == 6);
	json << "\n],\n";
	appendKey(json, "kernels");
	json << "[\n";
	for (std::size_t index = 0; index < receipt.kernels.size(); ++index)
		appendKernel(json, receipt.kernels[index],
			index + 1 != receipt.kernels.size());
	json << "\n],\n";
	appendKernelTiming(json, receipt.kernelTiming);
	json << ",\n";
	appendKernelReference(json, receipt.kernelReference);
	if (receipt.schemaVersion == 6)
	{
		json << ",\n";
		appendPhaseAccounting(json, receipt.kernelTiming.phaseAccounting);
		json << ",\n";
		appendAttemptTrace(json, receipt);
	}
	json << "\n}\n";
	document = json.str();
	return !document.empty();
}

bool ValidatePerformanceReceipt(const PerformanceReceipt &receipt,
	std::string *reason)
{
	const bool v6 = receipt.schemaVersion == 6 &&
		receipt.producer == "game-executable-stage5-performance-report-v6" && receipt.producerVersion == "6";
	const bool v5 = receipt.schemaVersion == 5 &&
		receipt.producer == "game-executable-stage5-performance-report-v5" && receipt.producerVersion == "5";
	const bool baseline = receipt.kernelReference.mode == KERNEL_REFERENCE_PHASE_BASELINE_BINDING;
	if ((!v5 && !v6) ||
		receipt.evidenceKind != PERFORMANCE_RECEIPT_EVIDENCE_KIND ||
		receipt.role != "performance-report" ||
		receipt.architecture != "x64")
	{
		setReason(reason, "receipt schema or producer is not recognized");
		return false;
	}
	if (receipt.status != "passed")
	{
		setReason(reason, "receipt is not complete");
		return false;
	}
	if (!validKernelTiming(receipt))
	{
		setReason(reason, "kernel timing is not a finalized, internally consistent run snapshot");
		return false;
	}
	if (!validKernelReference(receipt))
	{
		setReason(reason, "kernel reference is not a finalized, role-consistent canonical binding");
		return false;
	}
	if (!validPhaseAccounting(receipt) || !validAttemptTrace(receipt))
	{
		setReason(reason, "phase partition or selected trace is not a complete role-bound snapshot");
		return false;
	}
	if (receipt.title.empty() || receipt.runId.empty() ||
		receipt.runNonce.empty() || receipt.cohortNonce.empty() ||
		receipt.cohortCreatedUtc.empty() || receipt.recordedUtc.empty() ||
		receipt.sourceCommit.empty() || receipt.commandLine.empty() ||
		receipt.fixtureId.empty() || !isSafeToken(receipt.runId) ||
		!isCanonicalUuid(receipt.runNonce) || !isCanonicalUuid(receipt.cohortNonce) ||
		!isIsoUtcTimestamp(receipt.cohortCreatedUtc) ||
		!isIsoUtcTimestamp(receipt.recordedUtc) ||
		receipt.recordedUtc < receipt.cohortCreatedUtc ||
		!isLowerHexString(receipt.sourceCommit, 40) ||
		!isSafeToken(receipt.fixtureId) || receipt.receiptPath.empty())
	{
		setReason(reason, "receipt identity fields are incomplete");
		return false;
	}
	if (!isHexString(receipt.artifactSetSha256, 64) ||
		!isHexString(receipt.runtimeClosureDependencyManifestSha256, 64) ||
		!isHexString(receipt.runtimeClosureSha256, 64) ||
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
	const bool observedOnly = receipt.workloadQualification == "observed-only";
	if ((!observedOnly && receipt.workloadQualification != "minimum-qualified") ||
		!receipt.fixtureIdentityObserved || receipt.fixtureObservationFailed || receipt.fixtureContentPath.empty() ||
		(receipt.fixtureKind != "replay" && receipt.fixtureKind != "fresh-ai-map") ||
		(receipt.fixtureKind == "replay" && (receipt.replayPath != receipt.fixtureContentPath ||
			!receipt.retainedReplayPath.empty() || !receipt.retainedReplaySha256.empty())) ||
		(receipt.fixtureKind == "fresh-ai-map" && (!observedOnly || !receipt.replayPath.empty() ||
			receipt.retainedReplayPath.empty() || !isHexString(receipt.retainedReplaySha256, 64))))
	{
		setReason(reason, "fixture observation, kind, or retained replay identity is incomplete");
		return false;
	}
	const PerformanceReceiptWorkload &workload = receipt.workload;
	if ((observedOnly && (receipt.requestedPlayerCount != 0 || receipt.requestedMinimumUnitCount != 0)) ||
		(!observedOnly && (receipt.requestedPlayerCount == 0 || receipt.requestedMinimumUnitCount == 0 ||
			workload.playerCount != receipt.requestedPlayerCount || workload.initialUnitCount < receipt.requestedMinimumUnitCount)) ||
		workload.sampleCount == 0 || !workload.rosterStable || !workload.contiguous ||
		workload.playerCount == 0 ||
		static_cast<JobMetricCounter>(workload.firstFrame) !=
			static_cast<JobMetricCounter>(receipt.frameStart) + 1 ||
		workload.lastFrame != receipt.frameEnd ||
		workload.lastFrame < workload.firstFrame ||
		workload.sampleCount != static_cast<JobMetricCounter>(workload.lastFrame) -
			workload.firstFrame + 1 ||
		workload.minimumUnitCount > workload.initialUnitCount ||
		workload.peakUnitCount < workload.initialUnitCount)
	{
		setReason(reason, "completed-frame workload observation is incomplete or below the requested minimum");
		return false;
	}
	if (receipt.frameSimulationTotalNanoseconds == 0 ||
		receipt.frameSimulationMaximumNanoseconds == 0 ||
		receipt.frameSimulationMaximumNanoseconds > receipt.frameSimulationTotalNanoseconds ||
		receipt.frameSimulationSampleCount < workload.sampleCount)
	{
		setReason(reason, "measured frame simulation timing is incomplete");
		return false;
	}
	if ((receipt.simulationMode != "serial" && receipt.simulationMode != "parallel" && receipt.simulationMode != "shadow") ||
		(receipt.workerPolicy != "auto" && receipt.workerPolicy != "all"))
	{
		setReason(reason, "actual simulation mode or worker policy is unknown");
		return false;
	}
	if (!receipt.schedulerStarted && (!observedOnly || receipt.simulationMode != "serial" ||
		receipt.effectiveWorkerCount != 0 || receipt.workersPinned || receipt.availableLogicalCpuCount != 0 ||
		receipt.reservedOwnerCpuCount != 0 || receipt.selectedWorkerCpuCount != 0 ||
		receipt.selectedWorkerPhysicalCoreCount != 0 || receipt.selectedWorkerPhysicalCoreMask != 0 ||
		receipt.selectedWorkerPhysicalCoreMaskComplete || !receipt.cpuSets.empty() ||
		!receipt.ownerCpuSetIds.empty() || !receipt.selectedWorkerCpuSetIds.empty()))
	{
		setReason(reason, "absent scheduler cannot claim workers or physical topology");
		return false;
	}
	if (receipt.schedulerStarted && (!receipt.workersPinned ||
		receipt.effectiveWorkerCount == 0 ||
		receipt.availableLogicalCpuCount == 0 ||
		receipt.selectedWorkerCpuCount == 0 ||
		receipt.selectedWorkerPhysicalCoreCount == 0 ||
		receipt.selectedWorkerPhysicalCoreMask == 0 ||
		receipt.effectiveWorkerCount != receipt.selectedWorkerCpuCount ||
		!receipt.selectedWorkerPhysicalCoreMaskComplete))
	{
		setReason(reason, "effective worker or physical topology proof is incomplete");
		return false;
	}
	if ((receipt.schedulerStarted && receipt.cpuSets.empty()) ||
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
	std::vector<PerformanceReceiptCpuSet> selectedCpuSets;
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
		selectedCpuSets.push_back(cpuSet);
	}
	for (std::size_t first = 0; first < selectedIds.size(); ++first)
	{
		const PerformanceReceiptCpuSet &firstSet = selectedCpuSets[first];
		for (std::size_t second = first + 1; second < selectedIds.size();
			++second)
		{
			const PerformanceReceiptCpuSet &secondSet = selectedCpuSets[second];
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
	if (!receipt.rawEvidence.timingClosed || !receipt.rawEvidence.timingWriteSucceeded ||
		receipt.rawEvidence.timingTruncated || !receipt.rawEvidence.timingComplete ||
		receipt.rawEvidence.timingSessionCount != 1 ||
		receipt.rawEvidence.timingFrameSamples < workload.sampleCount ||
		receipt.rawEvidence.timingFirstFrame > receipt.frameStart ||
		receipt.rawEvidence.timingLastFrame < receipt.frameEnd)
	{
		setReason(reason, "timing capture is not closed, complete, and frame-correlated");
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
	JobMetricCounter phaseTotal = 0;
	for (std::size_t index = 0; index < receipt.phases.size(); ++index)
	{
		const PerformanceReceiptPhase &phase = receipt.phases[index];
		if (baseline) continue; // Exact snapshot arithmetic uses its independent owner clock above.
		if (phase.pureNanosecondsKnown || phase.pureNanoseconds != 0 ||
			(v6 && (phase.serialNanosecondsKnown || phase.serialNanoseconds != 0)))
		{
			setReason(reason, "ordinary phases cannot backfill V6 serial or pure coverage");
			return false;
		}
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
		if (phase.maximumNanoseconds > phase.totalNanoseconds ||
			phase.sampleCount > receipt.frameSimulationSampleCount ||
			phase.totalNanoseconds > receipt.frameSimulationTotalNanoseconds - phaseTotal ||
			(!phase.serialNanosecondsKnown && phase.serialNanoseconds != 0) ||
			(phase.serialNanosecondsKnown && (!phase.available ||
				phase.serialNanoseconds > phase.totalNanoseconds)))
		{
			setReason(reason, "owner phase timing or serial coverage is inconsistent");
			return false;
		}
		phaseTotal += phase.totalNanoseconds;
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
		if (baseline && (kernel.submittedJobs != 0 || kernel.completedJobs != 0 ||
			kernel.physicalWorkerJobs != 0 || kernel.ownerHelpedJobs != 0 || kernel.physicalWorkerMask != 0 ||
			kernel.distinctPhysicalWorkers != 0 || kernel.elapsedNanoseconds != 0 || kernel.elapsedNanosecondsKnown))
		{
			setReason(reason, "phase baseline cannot claim physical kernel execution");
			return false;
		}
		if (!receipt.schedulerStarted && (kernel.physicalWorkerJobs != 0 ||
			kernel.physicalWorkerMask != 0 || kernel.distinctPhysicalWorkers != 0))
		{
			setReason(reason, "absent scheduler cannot report physical kernel execution");
			return false;
		}
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
	const char *directory, std::string *writtenPath, std::string *reason,
	PerformanceReceiptPublicationObserver observer, void *observerContext,
	PerformanceReceiptHandleCloser closer, void *closerContext)
{
	if (writtenPath != 0)
		writtenPath->clear();
	if (directory == 0 || directory[0] == '\0')
	{
		setReason(reason, "receipt destination directory is missing");
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
	receipt.receiptPath = finalPath;
#if defined(_WIN32)
	if (!receipt.rawEvidence.timingClosed || !receipt.rawEvidence.timingWriteSucceeded ||
		receipt.rawEvidence.timingTruncated || !receipt.rawEvidence.timingComplete)
	{
		setReason(reason, "timing capture must be completely finalized before hashing");
		return false;
	}
	if (!calculateFileSha256(receipt.rawEvidence.rawLogPath,
			receipt.rawEvidence.rawLogSha256) ||
		!calculateFileSha256(receipt.rawEvidence.timingPath,
			receipt.rawEvidence.timingSha256))
	{
		setReason(reason, "closed raw evidence could not be hashed");
		return false;
	}
#endif
	if (!ValidatePerformanceReceipt(receipt, reason))
		return false;
	std::string document;
	if (!SerializePerformanceReceipt(receipt, document, reason))
	{
		setReason(reason, "receipt serialization failed");
		return false;
	}
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
	HANDLE file = CreateFileA(temporaryPath.c_str(), GENERIC_READ | GENERIC_WRITE | DELETE,
		FILE_SHARE_READ | FILE_SHARE_DELETE, 0, CREATE_NEW,
		FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_WRITE_THROUGH, 0);
	if (file == INVALID_HANDLE_VALUE)
	{
		setReason(reason, "temporary receipt creation failed");
		return false;
	}
	PerformanceReceiptFileIdentity initialIdentity;
	std::size_t offset = 0;
	bool success = queryPerformanceReceiptFile(file, temporaryPath, initialIdentity) &&
		initialIdentity.size == 0;
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
	PerformanceReceiptFileIdentity writtenIdentity;
	if (success && (!queryPerformanceReceiptFile(file, temporaryPath, writtenIdentity) ||
		writtenIdentity.volume != initialIdentity.volume ||
		writtenIdentity.indexHigh != initialIdentity.indexHigh ||
		writtenIdentity.indexLow != initialIdentity.indexLow ||
		writtenIdentity.size != document.size())) success = false;
	HANDLE heldFile = INVALID_HANDLE_VALUE;
	if (success && (!DuplicateHandle(GetCurrentProcess(), file,
		GetCurrentProcess(), &heldFile, 0, FALSE, DUPLICATE_SAME_ACCESS) ||
		!SetHandleInformation(heldFile, HANDLE_FLAG_INHERIT, 0))) success = false;
	if (!success)
	{
		FILE_DISPOSITION_INFO disposition = { TRUE };
		SetFileInformationByHandle(file, FileDispositionInfo,
			&disposition, sizeof(disposition));
		if (heldFile != INVALID_HANDLE_VALUE)
		{
			SetFileInformationByHandle(heldFile, FileDispositionInfo,
				&disposition, sizeof(disposition));
			CloseHandle(heldFile);
		}
		CloseHandle(file);
		setReason(reason, "temporary receipt write or exact-handle retention failed");
		return false;
	}
	// The writing handle has a separately retained identity twin. Its checked
	// closure precedes the pathname commit, while the twin prevents the temp
	// object from being replaced and remains available to the observer.
	const bool writingHandleClosed = closer != 0 ?
		closer(closerContext, file) : CloseHandle(file) != FALSE;
	if (!writingHandleClosed)
	{
		FILE_DISPOSITION_INFO disposition = { TRUE };
		SetFileInformationByHandle(heldFile, FileDispositionInfo,
			&disposition, sizeof(disposition));
		CloseHandle(heldFile);
		setReason(reason, "temporary receipt checked closure failed");
		return false;
	}
	bool moved = false;
	moved = MoveFileExA(temporaryPath.c_str(), finalPath.c_str(),
		MOVEFILE_WRITE_THROUGH) != FALSE;
	PerformanceReceiptFileIdentity publishedIdentity;
	if (moved && (!queryPerformanceReceiptFile(heldFile, finalPath, publishedIdentity) ||
		!samePerformanceReceiptFile(writtenIdentity, publishedIdentity))) success = false;
	if (success && observer != 0)
		success = observer(observerContext, heldFile, finalPath.c_str(), reason);
	PerformanceReceiptFileIdentity observedIdentity;
	if (success && (!queryPerformanceReceiptFile(heldFile, finalPath, observedIdentity) ||
		!samePerformanceReceiptFile(publishedIdentity, observedIdentity))) success = false;
	HANDLE cleanupFile = INVALID_HANDLE_VALUE;
	if (success && (!DuplicateHandle(GetCurrentProcess(), heldFile,
		GetCurrentProcess(), &cleanupFile, 0, FALSE, DUPLICATE_SAME_ACCESS) ||
		!SetHandleInformation(cleanupFile, HANDLE_FLAG_INHERIT, 0))) success = false;
	if (!success || !moved)
	{
		FILE_DISPOSITION_INFO disposition = { TRUE };
		SetFileInformationByHandle(heldFile, FileDispositionInfo,
			&disposition, sizeof(disposition));
		if (cleanupFile != INVALID_HANDLE_VALUE)
		{
			SetFileInformationByHandle(cleanupFile, FileDispositionInfo,
				&disposition, sizeof(disposition));
			CloseHandle(cleanupFile);
		}
		CloseHandle(heldFile);
		setReason(reason, "atomic receipt publication failed");
		return false;
	}
	const bool publishedHandleClosed = closer != 0 ?
		closer(closerContext, heldFile) : CloseHandle(heldFile) != FALSE;
	if (!publishedHandleClosed)
	{
		FILE_DISPOSITION_INFO disposition = { TRUE };
		SetFileInformationByHandle(cleanupFile, FileDispositionInfo,
			&disposition, sizeof(disposition));
		CloseHandle(cleanupFile);
		setReason(reason, "published receipt checked closure failed");
		return false;
	}
	if (CloseHandle(cleanupFile) == FALSE)
	{
		setReason(reason, "published receipt cleanup-handle closure failed");
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
