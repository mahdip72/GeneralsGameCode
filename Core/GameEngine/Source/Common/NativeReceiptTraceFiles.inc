// BEGIN RTS_NATIVE_RECEIPT_TRACE_FILES_PRIVATE
#include <bcrypt.h>

namespace native_receipt_files
{
// This transport is intentionally a fixed native policy. The caller may
// supply a smaller bound for a focused arithmetic test, but cannot enlarge
// either bound or select a policy through this private component.
static const unsigned HeldHashBlockBytes = 65536U;
static const rts::JobMetricCounter HeldReceiptMaximumBytes = 4194304ULL;
static const rts::JobMetricCounter HeldTraceMaximumBytes = 16ULL * 1024ULL * 1024ULL * 1024ULL;

static bool setReason(std::string *reason, const char *message)
{
	if (reason != 0)
	{
		try { *reason = message != 0 ? message : "native held-file transport failure"; }
		catch (...) {}
	}
	return false;
}

static void clearReason(std::string *reason)
{
	if (reason != 0) reason->clear();
}

// Keep the subtraction form: offset + count is never evaluated before the
// bound has been established, including at the unsigned 64-bit ceiling.
static bool heldExtentFits(rts::JobMetricCounter offset, unsigned count,
	rts::JobMetricCounter limit)
{
	return offset <= limit && static_cast<rts::JobMetricCounter>(count) <= limit - offset;
}

static char upperAscii(char value)
{
	return value >= 'a' && value <= 'z' ? static_cast<char>(value - 'a' + 'A') : value;
}

static bool equalAsciiInsensitive(const std::string &first, const std::string &second)
{
	if (first.size() != second.size()) return false;
	for (size_t index = 0; index != first.size(); ++index)
		if (upperAscii(first[index]) != upperAscii(second[index])) return false;
	return true;
}

static bool reservedDeviceName(const std::string &component)
{
	std::string base;
	try
	{
		const size_t dot = component.find('.');
		base = component.substr(0, dot == std::string::npos ? component.size() : dot);
	}
	catch (...) { return true; }
	for (size_t index = 0; index != base.size(); ++index) base[index] = upperAscii(base[index]);
	if (base == "CON" || base == "PRN" || base == "AUX" || base == "NUL") return true;
	if (base.size() == 4 && (base.compare(0, 3, "COM") == 0 || base.compare(0, 3, "LPT") == 0) &&
		base[3] >= '1' && base[3] <= '9') return true;
	return false;
}

// The receipt contract deliberately starts with original-W ASCII paths. A
// path is checked before any CreateFileW call, then every opened component is
// compared with its normalized final name to reject aliases and reparses.
static bool safeNativePath(const std::string &path)
{
	if (path.size() < 3 || path.size() >= MAX_PATH ||
		!((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) ||
		path[1] != ':' || path[2] != '\\') return false;
	if (path.size() == 3) return true;
	if (path[path.size() - 1] == '\\') return false;
	size_t start = 3;
	while (start < path.size())
	{
		const size_t end = path.find('\\', start);
		const size_t finish = end == std::string::npos ? path.size() : end;
		if (finish == start) return false;
		const std::string component = path.substr(start, finish - start);
		if (component == "." || component == ".." || reservedDeviceName(component)) return false;
		if (component[component.size() - 1] == '.' || component[component.size() - 1] == ' ') return false;
		for (size_t index = 0; index != component.size(); ++index)
		{
			const unsigned char character = static_cast<unsigned char>(component[index]);
			if (character == 0 || character < 32 || character > 127 || component[index] == '/' ||
				component[index] == ':' || component[index] == '<' || component[index] == '>' ||
				component[index] == '"' || component[index] == '|' || component[index] == '?' ||
				component[index] == '*') return false;
		}
		if (end == std::string::npos) break;
		start = end + 1;
	}
	return true;
}

static bool nativeWide(const std::string &path, std::wstring &wide)
{
	wide.clear();
	try
	{
		wide.reserve(path.size());
		for (size_t index = 0; index != path.size(); ++index)
		{
			const unsigned char character = static_cast<unsigned char>(path[index]);
			if (character == 0 || character > 127) return false;
			wide.push_back(static_cast<wchar_t>(character));
		}
	}
	catch (...) { wide.clear(); return false; }
	return true;
}

static bool ntfsName(const wchar_t *fileSystemName)
{
	if (fileSystemName == 0) return false;
	const wchar_t expected[] = L"NTFS";
	for (size_t index = 0; index != 4; ++index)
	{
		wchar_t actual = fileSystemName[index];
		if (actual >= L'a' && actual <= L'z') actual = static_cast<wchar_t>(actual - L'a' + L'A');
		if (actual != expected[index]) return false;
	}
	return fileSystemName[4] == 0;
}

static bool isNtfsVolume(const std::string &path)
{
	std::wstring root;
	try
	{
		root.push_back(static_cast<wchar_t>(path[0]));
		root += L":\\";
	}
	catch (...) { return false; }
	wchar_t fileSystemName[32] = {};
	if (GetVolumeInformationW(root.c_str(), 0, 0, 0, 0, 0, fileSystemName,
		sizeof(fileSystemName) / sizeof(fileSystemName[0])) == FALSE) return false;
	return ntfsName(fileSystemName);
}

struct FileIdentity
{
	DWORD volume;
	DWORD high;
	DWORD low;
};

static bool normalizedPathMatches(HANDLE handle, const std::string &expected)
{
	wchar_t finalPath[MAX_PATH] = {};
	const DWORD count = GetFinalPathNameByHandleW(handle, finalPath, MAX_PATH,
		FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
	if (count == 0 || count >= MAX_PATH) return false;
	size_t start = 0;
	if (count >= 4 && finalPath[0] == L'\\' && finalPath[1] == L'\\' &&
		finalPath[2] == L'?' && finalPath[3] == L'\\') start = 4;
	std::string actual;
	try
	{
		actual.reserve(count - start);
		for (size_t index = start; index != count; ++index)
		{
			if (finalPath[index] == 0 || finalPath[index] > 127) return false;
			actual.push_back(static_cast<char>(finalPath[index]));
		}
	}
	catch (...) { return false; }
	return equalAsciiInsensitive(actual, expected);
}

static bool queryIdentity(HANDLE handle, const std::string &path, bool directory,
	FileIdentity &identity)
{
	BY_HANDLE_FILE_INFORMATION information = {};
	if (GetFileInformationByHandle(handle, &information) == FALSE) return false;
	const bool actualDirectory = (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
	if (actualDirectory != directory || (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
		(!directory && information.nNumberOfLinks != 1)) return false;
	if (!normalizedPathMatches(handle, path)) return false;
	identity.volume = information.dwVolumeSerialNumber;
	identity.high = information.nFileIndexHigh;
	identity.low = information.nFileIndexLow;
	return true;
}

static bool sameIdentity(const FileIdentity &first, const FileIdentity &second)
{
	return first.volume == second.volume && first.high == second.high && first.low == second.low;
}

static bool makeHandleUninheritable(HANDLE handle)
{
	return SetHandleInformation(handle, HANDLE_FLAG_INHERIT, 0) != FALSE;
}

static bool closeOwnedHandle(HANDLE &handle)
{
	if (handle == INVALID_HANDLE_VALUE || handle == 0) return true;
	HANDLE old = handle;
	handle = INVALID_HANDLE_VALUE;
	return CloseHandle(old) != FALSE;
}

static bool closeHandleList(HANDLE *handles, unsigned &count)
{
	bool result = true;
	while (count != 0)
	{
		HANDLE handle = handles[--count];
		handles[count] = INVALID_HANDLE_VALUE;
		if (handle != INVALID_HANDLE_VALUE && handle != 0 && CloseHandle(handle) == FALSE) result = false;
	}
	return result;
}

static bool retainOnlyExactDirectory(HANDLE &exact, HANDLE *handles, unsigned &count)
{
	if (exact == INVALID_HANDLE_VALUE || exact == 0 || count == 0 || handles[count - 1] != exact)
		return false;
	HANDLE retained = exact;
	exact = INVALID_HANDLE_VALUE;
	handles[--count] = INVALID_HANDLE_VALUE;
	const bool ancestorsClosed = closeHandleList(handles, count);
	if (!ancestorsClosed)
	{
		closeOwnedHandle(retained);
		return false;
	}
	handles[0] = retained;
	count = 1;
	exact = retained;
	return true;
}

static bool openDirectoryComponents(const std::string &path, HANDLE &exact,
	HANDLE *handles, unsigned &handleCount, FileIdentity &identity, std::string *reason)
{
	exact = INVALID_HANDLE_VALUE;
	handleCount = 0;
	if (!safeNativePath(path)) return setReason(reason, "native directory path is not an original-W absolute ASCII path");
	if (!isNtfsVolume(path)) return setReason(reason, "native held-file transport requires NTFS");
	try
	{
		std::string current = path.substr(0, 3);
		size_t start = 3;
		for (;;)
		{
			std::wstring currentWide;
			if (!nativeWide(current, currentWide)) return setReason(reason, "native directory path conversion failed");
			if (handleCount == MAX_PATH) return setReason(reason, "native directory component count is outside its fixed bound");
			HANDLE handle = CreateFileW(currentWide.c_str(), GENERIC_READ, FILE_SHARE_READ, 0,
				OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, 0);
			if (handle == INVALID_HANDLE_VALUE)
			{
				closeHandleList(handles, handleCount);
				return setReason(reason, "native directory component could not be held");
			}
			if (!makeHandleUninheritable(handle))
			{
				CloseHandle(handle);
				closeHandleList(handles, handleCount);
				return setReason(reason, "native directory component inheritance could not be disabled");
			}
			FileIdentity componentIdentity;
			if (!queryIdentity(handle, current, true, componentIdentity))
			{
				CloseHandle(handle);
				closeHandleList(handles, handleCount);
				return setReason(reason, "native directory component is not a normalized non-reparse directory");
			}
			handles[handleCount++] = handle;
			exact = handle;
			identity = componentIdentity;
			if (start == path.size()) break;
			const size_t end = path.find('\\', start);
			const size_t finish = end == std::string::npos ? path.size() : end;
			current += current.size() == 3 ? path.substr(start, finish - start) :
				std::string("\\") + path.substr(start, finish - start);
			if (end == std::string::npos) start = path.size();
			else start = end + 1;
		}
	}
	catch (...)
	{
		closeHandleList(handles, handleCount);
		exact = INVALID_HANDLE_VALUE;
		return setReason(reason, "native directory component admission allocation failed");
	}
	return true;
}

static bool openExistingFile(const std::string &path, HANDLE &handle,
	FileIdentity &identity, std::string *reason)
{
	handle = INVALID_HANDLE_VALUE;
	std::wstring wide;
	if (!nativeWide(path, wide)) return setReason(reason, "native file path conversion failed");
	handle = CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING,
		FILE_FLAG_OPEN_REPARSE_POINT, 0);
	if (handle == INVALID_HANDLE_VALUE) return setReason(reason, "native source file could not be held");
	if (!makeHandleUninheritable(handle))
	{
		closeOwnedHandle(handle);
		return setReason(reason, "native source file inheritance could not be disabled");
	}
	if (!queryIdentity(handle, path, false, identity))
	{
		closeOwnedHandle(handle);
		return setReason(reason, "native source file is not a normalized single-link regular file");
	}
	return true;
}

static bool openNewFile(const std::string &path, HANDLE &handle,
	FileIdentity &identity, std::string *reason)
{
	handle = INVALID_HANDLE_VALUE;
	std::wstring wide;
	if (!nativeWide(path, wide)) return setReason(reason, "native output path conversion failed");
	handle = CreateFileW(wide.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, 0,
		CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, 0);
	if (handle == INVALID_HANDLE_VALUE) return setReason(reason, "native output requires an unused CREATE_NEW file");
	if (!makeHandleUninheritable(handle))
	{
		closeOwnedHandle(handle);
		return setReason(reason, "native output inheritance could not be disabled");
	}
	if (!queryIdentity(handle, path, false, identity))
	{
		closeOwnedHandle(handle);
		return setReason(reason, "native output is not a normalized single-link regular file");
	}
	return true;
}

static bool fileSize(HANDLE handle, rts::JobMetricCounter &size)
{
	LARGE_INTEGER value = {};
	if (GetFileSizeEx(handle, &value) == FALSE || value.QuadPart < 0) return false;
	size = static_cast<rts::JobMetricCounter>(value.QuadPart);
	return static_cast<LONGLONG>(size) == value.QuadPart;
}

class Sha256
{
public:
	Sha256() : m_algorithm(0), m_hash(0), m_finished(false) {}
	~Sha256()
	{
		if (m_hash != 0) BCryptDestroyHash(m_hash);
		if (m_algorithm != 0) BCryptCloseAlgorithmProvider(m_algorithm, 0);
	}
	bool begin()
	{
		return BCryptOpenAlgorithmProvider(&m_algorithm, BCRYPT_SHA256_ALGORITHM, 0, 0) == 0 &&
			BCryptCreateHash(m_algorithm, &m_hash, 0, 0, 0, 0, 0) == 0;
	}
	bool update(const unsigned char *bytes, size_t count)
	{
		if (m_finished || (count != 0 && bytes == 0)) return false;
		while (count != 0)
		{
			const ULONG block = static_cast<ULONG>(count > HeldHashBlockBytes ? HeldHashBlockBytes : count);
			if (BCryptHashData(m_hash, const_cast<PUCHAR>(bytes), block, 0) != 0) return false;
			bytes += block;
			count -= block;
		}
		return true;
	}
	rts::performance::KernelPerformanceDigest finish()
	{
		rts::performance::KernelPerformanceDigest digest;
		if (m_finished || m_hash == 0) return digest;
		m_finished = true;
		digest.valid = BCryptFinishHash(m_hash, digest.bytes, sizeof(digest.bytes), 0) == 0;
		return digest;
	}

	private:
	BCRYPT_ALG_HANDLE m_algorithm;
	BCRYPT_HASH_HANDLE m_hash;
	bool m_finished;
};

static bool isImmediateChild(const std::string &directory,
	const std::string &path)
{
	if (!safeNativePath(directory) || !safeNativePath(path) ||
		path.size() <= directory.size() + 1 || path[directory.size()] != '\\') return false;
	for (size_t index = 0; index != directory.size(); ++index)
		if (upperAscii(path[index]) != upperAscii(directory[index])) return false;
	return path.find('\\', directory.size() + 1) == std::string::npos;
}

static bool hashFile(HANDLE handle, rts::JobMetricCounter size,
	rts::performance::KernelPerformanceDigest &digest)
{
	if (size > HeldTraceMaximumBytes) return false;
	LARGE_INTEGER begin = {};
	if (SetFilePointerEx(handle, begin, 0, FILE_BEGIN) == FALSE) return false;
	Sha256 hash;
	if (!hash.begin()) return false;
	unsigned char block[HeldHashBlockBytes];
	rts::JobMetricCounter remaining = size;
	while (remaining != 0)
	{
		const unsigned count = static_cast<unsigned>(remaining > HeldHashBlockBytes ? HeldHashBlockBytes : remaining);
		DWORD read = 0;
		if (ReadFile(handle, block, count, &read, 0) == FALSE || read != count || !hash.update(block, count)) return false;
		remaining -= count;
	}
	unsigned char extra = 0;
	DWORD extraCount = 0;
	rts::JobMetricCounter closedSize = 0;
	if (ReadFile(handle, &extra, 1, &extraCount, 0) == FALSE || extraCount != 0 ||
		!fileSize(handle, closedSize) || closedSize != size) return false;
	digest = hash.finish();
	return digest.valid;
}

static bool readReceipt(HANDLE handle, std::vector<unsigned char> &bytes,
	rts::JobMetricCounter &size, rts::performance::KernelPerformanceDigest &digest,
	std::string *reason)
{
	if (!fileSize(handle, size) || size == 0 || size > HeldReceiptMaximumBytes ||
		size > static_cast<rts::JobMetricCounter>(~static_cast<size_t>(0)))
		return setReason(reason, "source receipt extent is empty, above 4 MiB or unavailable");
	try { bytes.assign(static_cast<size_t>(size), 0); }
	catch (...) { return setReason(reason, "source receipt allocation failed"); }
	LARGE_INTEGER begin = {};
	if (SetFilePointerEx(handle, begin, 0, FILE_BEGIN) == FALSE) return setReason(reason, "source receipt seek failed");
	Sha256 hash;
	if (!hash.begin()) return setReason(reason, "source receipt SHA256 provider failed");
	size_t offset = 0;
	while (offset != bytes.size())
	{
		const unsigned count = static_cast<unsigned>((bytes.size() - offset) > HeldHashBlockBytes ?
			HeldHashBlockBytes : bytes.size() - offset);
		DWORD read = 0;
		if (ReadFile(handle, bytes.data() + offset, count, &read, 0) == FALSE || read != count ||
			!hash.update(bytes.data() + offset, count))
			return setReason(reason, "source receipt exact read failed");
		offset += count;
	}
	digest = hash.finish();
	if (!digest.valid) return setReason(reason, "source receipt SHA256 finalization failed");
	return true;
}

class NativeReceiptTraceFiles
{
public:
	NativeReceiptTraceFiles()
		: m_bundle(INVALID_HANDLE_VALUE), m_destinationBundle(INVALID_HANDLE_VALUE),
		  m_receipt(INVALID_HANDLE_VALUE), m_trace(INVALID_HANDLE_VALUE),
		  m_raw(INVALID_HANDLE_VALUE), m_timing(INVALID_HANDLE_VALUE), m_published(INVALID_HANDLE_VALUE),
		  m_sourceGuardCount(0), m_outputGuardCount(0), m_started(false), m_sealed(false),
		  m_finished(false), m_failed(false), m_output(false), m_hasSource(false), m_destinationHeld(false),
		  m_maximumBytes(0), m_traceBytes(0), m_outputBytes(0), m_sourceReceiptBytes(0), m_bundlePath(),
		  m_destinationPath(), m_receiptPath(), m_tracePath(), m_rawPath(), m_timingPath(),
		  m_bundleIdentity(), m_destinationIdentity(), m_receiptIdentity(), m_traceIdentity(),
		  m_rawIdentity(), m_timingIdentity(), m_publishedIdentity(), m_rawBytes(0), m_timingBytes(0),
		  m_sourceReceipt(), m_sourceReceiptDigest()
	{
		for (unsigned index = 0; index != MAX_PATH; ++index)
		{
			m_sourceGuards[index] = INVALID_HANDLE_VALUE;
			m_outputGuards[index] = INVALID_HANDLE_VALUE;
		}
	}
	~NativeReceiptTraceFiles() { closeOwned(); }

	bool holdOutputDestination(const std::string &outputDirectory,
		std::string *reason = 0)
	{
		clearReason(reason);
		if (m_finished || m_failed || m_destinationHeld)
			return setReason(reason, "native output destination owner is already used");
		if (!safeNativePath(outputDirectory))
			return failAdmission(reason, "native output destination path is invalid");
		if (!openDirectoryComponents(outputDirectory, m_destinationBundle,
			m_outputGuards, m_outputGuardCount, m_destinationIdentity, reason))
			return failAdmission(reason, "native output destination admission failed");
		m_destinationPath = outputDirectory;
		m_destinationHeld = true;
		return true;
	}

	bool openOutput(const std::string &outputDirectory, const std::string &tracePath,
		rts::JobMetricCounter maximumBytes, std::string *reason = 0)
	{
		clearReason(reason);
		if (m_started || m_finished || m_failed) return setReason(reason, "native held-file owner is already used");
		if (!safeNativePath(outputDirectory) || !safeNativePath(tracePath) || maximumBytes == 0 ||
			maximumBytes > HeldTraceMaximumBytes)
			return failAdmission(reason, "native output path or fixed 16 GiB bound is invalid");
		try
		{
			const std::string expectedTrace = outputDirectory + "\\attempt-trace.bin";
			if (tracePath != expectedTrace) return failAdmission(reason, "native output must be the exact bundle trace path");
			if (!m_destinationHeld && !holdOutputDestination(outputDirectory, reason)) return false;
			if (!equalAsciiInsensitive(m_destinationPath, outputDirectory))
				return failAdmission(reason, "native output does not match its held destination");
			HANDLE trace = INVALID_HANDLE_VALUE;
			FileIdentity traceIdentity;
			if (!openNewFile(tracePath, trace, traceIdentity, reason))
				return failAdmission(reason, "native output file admission failed");
			if (!retainOnlyExactDirectory(m_destinationBundle, m_outputGuards, m_outputGuardCount))
			{
				closeOwnedHandle(trace);
				return failAdmission(reason, "native output ancestor-handle closure failed");
			}
			m_trace = trace;
			m_tracePath = tracePath;
			m_traceIdentity = traceIdentity;
			m_maximumBytes = maximumBytes; m_traceBytes = 0; m_outputBytes = 0;
			m_started = true; m_output = true; m_failed = false; m_hasSource = false;
			return true;
		}
		catch (...)
		{
			m_failed = true;
			closeOwned();
			return setReason(reason, "native output admission allocation failed");
		}
	}

	bool openSource(const std::string &receiptPath,
		const rts::performance::KernelPerformanceDigest &expectedReceiptDigest,
		rts::JobMetricCounter maximumTraceBytes, std::string *reason = 0)
	{
		clearReason(reason);
		if (m_started || m_finished || m_failed) return setReason(reason, "native held-file owner is already used");
		if (!safeNativePath(receiptPath) || !expectedReceiptDigest.valid || maximumTraceBytes == 0 ||
			maximumTraceBytes > HeldTraceMaximumBytes)
			return failAdmission(reason, "native source path, receipt digest or fixed 16 GiB bound is invalid");
		try
		{
			const size_t slash = receiptPath.rfind('\\');
			if (slash < 3) return failAdmission(reason, "native source receipt has no bundle directory");
			const std::string bundlePath = receiptPath.substr(0, slash);
			HANDLE bundle = INVALID_HANDLE_VALUE;
			FileIdentity bundleIdentity;
			if (!openDirectoryComponents(bundlePath, bundle, m_sourceGuards,
				m_sourceGuardCount, bundleIdentity, reason))
				return failAdmission(reason, "native source bundle directory admission failed");
			HANDLE receipt = INVALID_HANDLE_VALUE;
			FileIdentity receiptIdentity;
			if (!openExistingFile(receiptPath, receipt, receiptIdentity, reason))
			{
				closeHandleList(m_sourceGuards, m_sourceGuardCount);
				return failAdmission(reason, "native source receipt admission failed");
			}
			std::vector<unsigned char> receiptBytes;
			rts::JobMetricCounter receiptSize = 0;
			rts::performance::KernelPerformanceDigest receiptDigest;
			if (!readReceipt(receipt, receiptBytes, receiptSize, receiptDigest, reason) ||
				!receiptDigest.equals(expectedReceiptDigest))
			{
				closeOwnedHandle(receipt);
				closeHandleList(m_sourceGuards, m_sourceGuardCount);
				return failAdmission(reason, "native source receipt SHA256 does not match its selected bytes");
			}
			rts::performance::PerformanceReceipt parsed;
			std::string parseReason;
			if (!rts::performance::ParsePerformanceReceiptSource(receiptBytes.data(), receiptBytes.size(), parsed, &parseReason))
			{
				closeOwnedHandle(receipt);
				closeHandleList(m_sourceGuards, m_sourceGuardCount);
				if (reason != 0 && !parseReason.empty()) *reason = parseReason;
				return failAdmission(reason, "native source receipt parser rejected the held bytes");
			}
			if (parsed.receiptPath != receiptPath || !safeNativePath(parsed.traceFiles.tracePath))
			{
				closeOwnedHandle(receipt);
				closeHandleList(m_sourceGuards, m_sourceGuardCount);
				return failAdmission(reason, "native source receipt path fields do not match original-W selection");
			}
			const std::string expectedTrace = bundlePath + "\\attempt-trace.bin";
			const bool sameSelectedFile = parsed.traceFiles.tracePath == receiptPath;
			if (!sameSelectedFile && parsed.traceFiles.tracePath != expectedTrace)
			{
				closeOwnedHandle(receipt);
				closeHandleList(m_sourceGuards, m_sourceGuardCount);
				return failAdmission(reason, "native source trace path is outside the exact bundle trace leaf");
			}
			const rts::JobMetricCounter traceSizeFromReceipt = parsed.kernelReference.trace.byteCount;
			if (traceSizeFromReceipt == 0 || traceSizeFromReceipt > maximumTraceBytes ||
				traceSizeFromReceipt > HeldTraceMaximumBytes)
			{
				closeOwnedHandle(receipt);
				closeHandleList(m_sourceGuards, m_sourceGuardCount);
				return failAdmission(reason, "native source trace extent is outside the fixed bound");
			}
			HANDLE trace = INVALID_HANDLE_VALUE;
			FileIdentity traceIdentity;
			if (!openExistingFile(parsed.traceFiles.tracePath, trace, traceIdentity, reason))
			{
				closeOwnedHandle(receipt);
				closeHandleList(m_sourceGuards, m_sourceGuardCount);
				return failAdmission(reason, "native source trace admission failed");
			}
			if (sameIdentity(receiptIdentity, traceIdentity))
			{
				closeOwnedHandle(trace); closeOwnedHandle(receipt); closeHandleList(m_sourceGuards, m_sourceGuardCount);
				return failAdmission(reason, "same file identity cannot serve as both receipt and trace");
			}
			rts::JobMetricCounter actualTraceSize = 0;
			rts::performance::KernelPerformanceDigest actualTraceDigest;
			if (!fileSize(trace, actualTraceSize) || actualTraceSize != traceSizeFromReceipt ||
				actualTraceSize > maximumTraceBytes || !hashFile(trace, actualTraceSize, actualTraceDigest) ||
				!actualTraceDigest.equals(parsed.kernelReference.trace.digest))
			{
				closeOwnedHandle(trace); closeOwnedHandle(receipt); closeHandleList(m_sourceGuards, m_sourceGuardCount);
				return failAdmission(reason, "native source trace bytes do not match its receipt digest");
			}
			if (!retainOnlyExactDirectory(bundle, m_sourceGuards, m_sourceGuardCount))
			{
				closeOwnedHandle(trace); closeOwnedHandle(receipt);
				return failAdmission(reason, "native source ancestor-handle closure failed");
			}
			m_bundle = bundle; m_receipt = receipt; m_trace = trace;
			m_bundlePath = bundlePath; m_receiptPath = receiptPath; m_tracePath = parsed.traceFiles.tracePath;
			m_bundleIdentity = bundleIdentity; m_receiptIdentity = receiptIdentity; m_traceIdentity = traceIdentity;
			m_maximumBytes = maximumTraceBytes; m_traceBytes = actualTraceSize; m_outputBytes = 0;
			m_sourceReceiptBytes = receiptSize; m_sourceReceipt = parsed; m_sourceReceiptDigest = receiptDigest;
			m_started = true; m_output = false; m_failed = false; m_hasSource = true;
			return true;
		}
		catch (...)
		{
			m_failed = true;
			closeOwned();
			return setReason(reason, "native source admission allocation failed");
		}
	}

	const rts::performance::PerformanceReceipt *source() const
	{
		return m_hasSource && !m_failed && !m_finished ? &m_sourceReceipt : 0;
	}

	rts::performance::KernelPerformanceDigest sourceReceiptDigest() const
	{
		return m_hasSource && !m_failed && !m_finished ? m_sourceReceiptDigest :
			rts::performance::KernelPerformanceDigest();
	}

	static bool append(void *context, const unsigned char *bytes, unsigned count)
	{
		if (context == 0) return false;
		return static_cast<NativeReceiptTraceFiles *>(context)->appendBytes(bytes, count);
	}

	static bool readAt(void *context, rts::JobMetricCounter offset,
		unsigned char *bytes, unsigned count)
	{
		if (context == 0) return false;
		return static_cast<NativeReceiptTraceFiles *>(context)->readBytesAt(offset, bytes, count);
	}

	static bool retainPublication(void *context, void *nativeHandle,
		const char *publishedPath, std::string *reason)
	{
		if (context == 0 || nativeHandle == 0 || publishedPath == 0)
			return setReason(reason, "native publication observer input is invalid");
		return static_cast<NativeReceiptTraceFiles *>(context)->retainPublishedHandle(
			static_cast<HANDLE>(nativeHandle), publishedPath, reason);
	}

	bool sealOutput(const rts::performance::KernelPerformanceTraceSnapshot &frozen,
		std::string *reason = 0)
	{
		clearReason(reason);
		if (!m_started || !m_output || m_finished || m_sealed)
			return setReason(reason, "native output is not in a finishable state");
		bool valid = !m_failed;
		if (valid) valid = m_destinationHeld &&
			queryIdentity(m_destinationBundle, m_destinationPath, true, m_destinationIdentity) &&
			queryIdentity(m_trace, m_tracePath, false, m_traceIdentity);
		rts::JobMetricCounter actualSize = 0;
		rts::performance::KernelPerformanceDigest actualDigest;
		if (valid) valid = fileSize(m_trace, actualSize) && actualSize == m_outputBytes &&
			actualSize == frozen.byteCount && actualSize <= m_maximumBytes &&
			frozen.digest.valid && hashFile(m_trace, actualSize, actualDigest) && actualDigest.equals(frozen.digest);
		if (valid && FlushFileBuffers(m_trace) == FALSE) valid = false;
		m_sealed = valid;
		m_failed = !valid;
		if (!valid) setReason(reason, "native output bytes, identity, SHA256 or flush did not qualify");
		return valid;
	}

	bool sealSource(std::string *reason = 0)
	{
		clearReason(reason);
		if (!m_started || m_output || m_finished || m_sealed)
			return setReason(reason, "native source is not in a finishable state");
		bool valid = !m_failed;
		if (valid) valid = queryIdentity(m_bundle, m_bundlePath, true, m_bundleIdentity) &&
			queryIdentity(m_receipt, m_receiptPath, false, m_receiptIdentity) &&
			queryIdentity(m_trace, m_tracePath, false, m_traceIdentity);
		rts::JobMetricCounter receiptSize = 0, traceSize = 0;
		rts::performance::KernelPerformanceDigest receiptDigest, traceDigest;
		if (valid) valid = readReceiptForClosure(receiptSize, receiptDigest, reason) &&
			fileSize(m_trace, traceSize) && traceSize == m_traceBytes &&
			hashFile(m_trace, traceSize, traceDigest) && receiptDigest.equals(m_sourceReceiptDigest) &&
			traceDigest.equals(m_sourceReceipt.kernelReference.trace.digest);
		m_sealed = valid;
		m_failed = !valid;
		if (!valid && (reason == 0 || reason->empty())) setReason(reason, "native source bytes or identity changed before closure");
		return valid;
	}

	bool holdEvidence(HANDLE raw, const std::string &rawPath,
		HANDLE timing, const std::string &timingPath,
		std::string *reason = 0)
	{
		clearReason(reason);
		if (raw != INVALID_HANDLE_VALUE && raw != 0 && raw == timing)
		{
			closeOwnedHandle(raw);
			return setReason(reason, "one native handle cannot own both evidence files");
		}
		if (!m_sealed || m_failed || !m_destinationHeld ||
			m_raw != INVALID_HANDLE_VALUE || m_timing != INVALID_HANDLE_VALUE ||
			raw == INVALID_HANDLE_VALUE || raw == 0 ||
			timing == INVALID_HANDLE_VALUE || timing == 0 ||
			!isImmediateChild(m_destinationPath, rawPath) ||
			!isImmediateChild(m_destinationPath, timingPath) ||
			equalAsciiInsensitive(rawPath, timingPath))
		{
			closeOwnedHandle(raw); closeOwnedHandle(timing);
			return setReason(reason, "native raw/timing evidence paths are not admissible");
		}
		FileIdentity rawIdentity, timingIdentity;
		if (!makeHandleUninheritable(raw) || !makeHandleUninheritable(timing) ||
			!queryIdentity(raw, rawPath, false, rawIdentity) ||
			!queryIdentity(timing, timingPath, false, timingIdentity) ||
			sameIdentity(rawIdentity, timingIdentity))
		{
			closeOwnedHandle(raw); closeOwnedHandle(timing);
			return setReason(reason, "native producer-held raw/timing evidence identity is invalid");
		}
		rts::JobMetricCounter rawBytes = 0, timingBytes = 0;
		rts::performance::KernelPerformanceDigest rawDigest, timingDigest;
		if (!fileSize(raw, rawBytes) || !fileSize(timing, timingBytes) ||
			rawBytes > HeldTraceMaximumBytes || timingBytes > HeldTraceMaximumBytes ||
			!hashFile(raw, rawBytes, rawDigest) || !hashFile(timing, timingBytes, timingDigest))
		{
			closeOwnedHandle(raw); closeOwnedHandle(timing);
			return setReason(reason, "native raw/timing evidence extent or SHA256 is invalid");
		}
		if (!retainOnlyExactDirectory(m_destinationBundle, m_outputGuards, m_outputGuardCount))
		{
			closeOwnedHandle(raw); closeOwnedHandle(timing);
			m_failed = true;
			closeOwned();
			return setReason(reason, "native output ancestor-handle closure failed");
		}
		m_raw = raw; m_timing = timing;
		m_rawPath = rawPath; m_timingPath = timingPath;
		m_rawIdentity = rawIdentity; m_timingIdentity = timingIdentity;
		m_rawBytes = rawBytes; m_timingBytes = timingBytes;
		m_rawDigest = rawDigest; m_timingDigest = timingDigest;
		return true;
	}

	bool finishPublication(const std::string &publishedPath,
		std::string *reason = 0)
	{
		clearReason(reason);
		if (!m_sealed || m_failed || m_finished || m_raw == INVALID_HANDLE_VALUE ||
			m_timing == INVALID_HANDLE_VALUE || m_published == INVALID_HANDLE_VALUE ||
			!equalAsciiInsensitive(m_publishedPath, publishedPath))
			return setReason(reason, "native publication is not in a closable state");
		bool valid = !sameIdentity(m_publishedIdentity, m_rawIdentity) &&
			!sameIdentity(m_publishedIdentity, m_timingIdentity) &&
			queryIdentity(m_published, m_publishedPath, false, m_publishedIdentity) &&
			queryIdentity(m_destinationBundle, m_destinationPath, true, m_destinationIdentity) &&
			queryIdentity(m_raw, m_rawPath, false, m_rawIdentity) &&
			queryIdentity(m_timing, m_timingPath, false, m_timingIdentity);
		rts::JobMetricCounter rawBytes = 0, timingBytes = 0, publishedBytes = 0;
		rts::performance::KernelPerformanceDigest rawDigest, timingDigest, publishedDigest;
		if (valid) valid = fileSize(m_raw, rawBytes) && rawBytes == m_rawBytes &&
			fileSize(m_timing, timingBytes) && timingBytes == m_timingBytes &&
			fileSize(m_published, publishedBytes) && publishedBytes != 0 &&
			publishedBytes <= HeldReceiptMaximumBytes &&
			hashFile(m_raw, rawBytes, rawDigest) && rawDigest.equals(m_rawDigest) &&
			hashFile(m_timing, timingBytes, timingDigest) && timingDigest.equals(m_timingDigest) &&
			hashFile(m_published, publishedBytes, publishedDigest);
		if (valid && m_output)
			valid = queryIdentity(m_trace, m_tracePath, false, m_traceIdentity);
		if (valid && !m_output)
			valid = queryIdentity(m_bundle, m_bundlePath, true, m_bundleIdentity) &&
				queryIdentity(m_receipt, m_receiptPath, false, m_receiptIdentity) &&
				queryIdentity(m_trace, m_tracePath, false, m_traceIdentity);
		HANDLE cleanupPublished = INVALID_HANDLE_VALUE;
		if (valid && (!DuplicateHandle(GetCurrentProcess(), m_published,
			GetCurrentProcess(), &cleanupPublished, 0, FALSE, DUPLICATE_SAME_ACCESS) ||
			!makeHandleUninheritable(cleanupPublished))) valid = false;
		if (!valid)
		{
			// The receipt was exclusively created for this attempted publication.
			// Delete that exact identity even if its pathname was displaced; never
			// remove whatever may now occupy the published pathname.
			FILE_DISPOSITION_INFO disposition = { TRUE };
			SetFileInformationByHandle(m_published, FileDispositionInfo,
				&disposition, sizeof(disposition));
		}
		const bool closed = closeOwned();
		if ((!valid || !closed) && cleanupPublished != INVALID_HANDLE_VALUE)
		{
			FILE_DISPOSITION_INFO disposition = { TRUE };
			SetFileInformationByHandle(cleanupPublished, FileDispositionInfo,
				&disposition, sizeof(disposition));
		}
		const bool cleanupClosed = closeOwnedHandle(cleanupPublished);
		m_finished = true; m_started = false; m_hasSource = false;
		m_failed = !valid || !closed || !cleanupClosed;
		if (!valid) setReason(reason, "native publication bytes or identities changed before closure");
		else if (!closed) setReason(reason, "native publication checked closure failed");
		else if (!cleanupClosed) setReason(reason, "native publication cleanup-handle closure failed");
		return valid && closed && cleanupClosed;
	}

	bool finishOutput(const rts::performance::KernelPerformanceTraceSnapshot &frozen,
		std::string *reason = 0)
	{
		const bool valid = sealOutput(frozen, reason);
		const bool closed = closeOwned();
		m_finished = true; m_started = false; m_failed = !valid || !closed;
		if (valid && !closed) setReason(reason, "native output checked closure failed");
		return valid && closed;
	}

	bool finishSource(std::string *reason = 0)
	{
		const bool valid = sealSource(reason);
		const bool closed = closeOwned();
		m_finished = true; m_started = false; m_hasSource = false; m_failed = !valid || !closed;
		if (valid && !closed) setReason(reason, "native source checked closure failed");
		return valid && closed;
	}

private:
	bool retainPublishedHandle(HANDLE published, const std::string &publishedPath,
		std::string *reason)
	{
		if (!m_sealed || m_failed || m_finished || m_published != INVALID_HANDLE_VALUE ||
			!isImmediateChild(m_destinationPath, publishedPath))
			return setReason(reason, "native published receipt handle is not admissible");
		HANDLE retained = ReOpenFile(published, GENERIC_READ | DELETE,
			FILE_SHARE_READ | FILE_SHARE_DELETE,
			FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN);
		if (retained == INVALID_HANDLE_VALUE ||
			!makeHandleUninheritable(retained))
		{
			closeOwnedHandle(retained);
			return setReason(reason, "native published receipt handle could not be retained");
		}
		FileIdentity identity;
		if (!queryIdentity(retained, publishedPath, false, identity))
		{
			closeOwnedHandle(retained);
			return setReason(reason, "native published receipt identity is invalid");
		}
		m_published = retained;
		m_publishedPath = publishedPath;
		m_publishedIdentity = identity;
		return true;
	}

	bool failAdmission(std::string *reason, const char *message)
	{
		m_failed = true;
		return setReason(reason, message);
	}

	bool appendBytes(const unsigned char *bytes, unsigned count)
	{
		if (!m_started || !m_output || m_finished || m_sealed || m_failed || (count != 0 && bytes == 0))
		{
			m_failed = true;
			return false;
		}
		if (!heldExtentFits(m_outputBytes, count, m_maximumBytes))
		{
			m_failed = true;
			return false;
		}
		while (count != 0)
		{
			const unsigned block = count > HeldHashBlockBytes ? HeldHashBlockBytes : count;
			DWORD written = 0;
			if (WriteFile(m_trace, bytes, block, &written, 0) == FALSE || written != block)
			{
				m_failed = true;
				return false;
			}
			bytes += block; count -= block; m_outputBytes += block;
		}
		return true;
	}

	bool readBytesAt(rts::JobMetricCounter offset, unsigned char *bytes, unsigned count)
	{
		if (!m_started || m_output || m_finished || m_sealed || m_failed || (count != 0 && bytes == 0)) return false;
		if (!heldExtentFits(offset, count, m_traceBytes)) { m_failed = true; return false; }
		if (count == 0) return true;
		LARGE_INTEGER position;
		position.QuadPart = static_cast<LONGLONG>(offset);
		if (SetFilePointerEx(m_trace, position, 0, FILE_BEGIN) == FALSE)
		{
			m_failed = true;
			return false;
		}
		DWORD read = 0;
		if (ReadFile(m_trace, bytes, count, &read, 0) == FALSE || read != count)
		{
			m_failed = true;
			return false;
		}
		return true;
	}

	bool readReceiptForClosure(rts::JobMetricCounter &size,
		rts::performance::KernelPerformanceDigest &digest, std::string *reason)
	{
		std::vector<unsigned char> ignored;
		return readReceipt(m_receipt, ignored, size, digest, reason) && size == m_sourceReceiptBytes;
	}

	bool closeOwned()
	{
		bool result = true;
		if (!closeOwnedHandle(m_published)) result = false;
		if (!closeOwnedHandle(m_timing)) result = false;
		if (!closeOwnedHandle(m_raw)) result = false;
		if (!closeOwnedHandle(m_trace)) result = false;
		if (!closeOwnedHandle(m_receipt)) result = false;
		if (!closeHandleList(m_sourceGuards, m_sourceGuardCount)) result = false;
		if (!closeHandleList(m_outputGuards, m_outputGuardCount)) result = false;
		m_bundle = INVALID_HANDLE_VALUE;
		m_destinationBundle = INVALID_HANDLE_VALUE;
		return result;
	}

	NativeReceiptTraceFiles(const NativeReceiptTraceFiles &);
	NativeReceiptTraceFiles &operator=(const NativeReceiptTraceFiles &);
	HANDLE m_bundle, m_destinationBundle, m_receipt, m_trace, m_raw, m_timing, m_published;
	HANDLE m_sourceGuards[MAX_PATH], m_outputGuards[MAX_PATH];
	unsigned m_sourceGuardCount, m_outputGuardCount;
	bool m_started, m_sealed, m_finished, m_failed, m_output, m_hasSource, m_destinationHeld;
	rts::JobMetricCounter m_maximumBytes, m_traceBytes, m_outputBytes, m_sourceReceiptBytes;
	std::string m_bundlePath, m_destinationPath, m_receiptPath, m_tracePath, m_rawPath, m_timingPath,
		m_publishedPath;
	FileIdentity m_bundleIdentity, m_destinationIdentity, m_receiptIdentity, m_traceIdentity;
	FileIdentity m_rawIdentity, m_timingIdentity, m_publishedIdentity;
	rts::JobMetricCounter m_rawBytes, m_timingBytes;
	rts::performance::KernelPerformanceDigest m_rawDigest, m_timingDigest;
	rts::performance::PerformanceReceipt m_sourceReceipt;
	rts::performance::KernelPerformanceDigest m_sourceReceiptDigest;
};
} // namespace native_receipt_files
// END RTS_NATIVE_RECEIPT_TRACE_FILES_PRIVATE
