// SCRATCH TEST-FIRST DRAFT. Not compiled or executed by its author.
// Parent supplies exact generated includes from the CURRENT actual production body
// and CURRENT existing receipt fixture. Never point the include at a test owner.
#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>
#include "Lib/PerformanceReceipt.h"
#include "NativeKernelSourceConsumerTest.h"

namespace held_file_test
{
using namespace rts::performance;
using rts::JobMetricCounter;
const JobMetricCounter NativeMaximumBytes = 17179869184ULL;
const size_t ReceiptMaximumBytes = 4U * 1024U * 1024U;

struct Failure : std::runtime_error
{
    Failure(const char *message, bool prerequisite) : std::runtime_error(message), setup(prerequisite) {}
    bool setup;
};
void need(bool value, const char *message)
{
    if (!value) throw Failure(message, true);
}
void require(bool value, const char *message)
{
    if (!value) throw Failure(message, false);
}
std::string ascii(const std::wstring &value)
{
    std::string result;
    for (size_t i = 0; i != value.size(); ++i)
    {
        need(value[i] != 0 && value[i] <= 127, "fixture root must be original ASCII W text");
        result += static_cast<char>(value[i]);
    }
    return result;
}
std::wstring wide(const std::string &value)
{
    std::wstring result;
    for (size_t i = 0; i != value.size(); ++i)
    {
        need(static_cast<unsigned char>(value[i]) < 128, "fixture conversion is ASCII only");
        result += static_cast<wchar_t>(static_cast<unsigned char>(value[i]));
    }
    return result;
}
bool sameDigest(const KernelPerformanceDigest &a, const KernelPerformanceDigest &b)
{
    return a.valid && b.valid && std::memcmp(a.bytes, b.bytes, sizeof(a.bytes)) == 0;
}
KernelPerformanceDigest independentSha(const unsigned char *bytes, size_t count)
{
    // Independent Windows SHA implementation; no use of the owner's hash helper.
    BCRYPT_ALG_HANDLE algorithm = 0;
    BCRYPT_HASH_HANDLE hash = 0;
    need(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, 0, 0) == 0,
        "BCrypt SHA256 provider prerequisite");
    DWORD objectBytes = 0, returned = 0;
    const bool queried = BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
        reinterpret_cast<PUCHAR>(&objectBytes), sizeof(objectBytes), &returned, 0) == 0;
    if (!queried) { BCryptCloseAlgorithmProvider(algorithm, 0); need(false, "BCrypt object length"); }
    std::vector<unsigned char> object(objectBytes);
    const bool created = BCryptCreateHash(algorithm, &hash, object.data(), objectBytes, 0, 0, 0) == 0;
    if (!created) { BCryptCloseAlgorithmProvider(algorithm, 0); need(false, "BCrypt hash creation"); }
    bool ok = true;
    for (size_t offset = 0; offset != count; )
    {
        const ULONG chunk = static_cast<ULONG>((std::min)(count - offset, size_t(65536)));
        if (BCryptHashData(hash, const_cast<PUCHAR>(bytes + offset), chunk, 0) != 0) { ok = false; break; }
        offset += chunk;
    }
    KernelPerformanceDigest result;
    result.valid = ok && BCryptFinishHash(hash, result.bytes, sizeof(result.bytes), 0) == 0;
    const bool closedHash = BCryptDestroyHash(hash) == 0;
    const bool closedAlgorithm = BCryptCloseAlgorithmProvider(algorithm, 0) == 0;
    need(result.valid && closedHash && closedAlgorithm, "independent SHA/cleanup prerequisite");
    return result;
}
KernelPerformanceDigest independentSha(const std::vector<unsigned char> &bytes)
{
    return independentSha(bytes.data(), bytes.size());
}

class Handle
{
public:
    explicit Handle(HANDLE value = INVALID_HANDLE_VALUE) : h(value) {}
    ~Handle() { if (h != INVALID_HANDLE_VALUE && h != 0) ::CloseHandle(h); }
    HANDLE get() const { return h; }
	HANDLE release()
	{
		HANDLE result = h;
		h = INVALID_HANDLE_VALUE;
		return result;
	}
    void close()
    {
        HANDLE old = h; h = INVALID_HANDLE_VALUE;
        need(old != INVALID_HANDLE_VALUE && old != 0 && ::CloseHandle(old), "fixture checked close");
    }
private:
    Handle(const Handle &); Handle &operator=(const Handle &);
    HANDLE h;
};

// These wrappers never synthesize handles, sizes, identities, attributes or paths.
// Each fault delegates an actual operation and changes only its count/reporting.
// They wrap only the exact included production body, not fixture construction.
struct OpenEvent
{
    HANDLE handle;
    std::wstring path;
    DWORD access, sharing, disposition, flags, error;
    bool live;
    unsigned reads, writes, identityQueries, finalPathQueries;
};
struct Observations
{
    std::vector<OpenEvent> opens;
    std::wstring shortReadPath, shortWritePath, flushFailurePath, closeFailurePath;
    std::wstring seamOpenPath, seamMoveFrom, seamMoveTo;
    bool fired, seamFired, seamMoved;
    DWORD seamError;
    Observations() : fired(false), seamFired(false), seamMoved(false), seamError(0) {}
    OpenEvent *live(HANDLE h)
    {
        for (size_t i = opens.size(); i != 0; --i)
            if (opens[i-1].live && opens[i-1].handle == h) return &opens[i-1];
        return 0;
    }
    OpenEvent *last(const std::wstring &path)
    {
        for (size_t i = opens.size(); i != 0; --i)
            if (opens[i-1].path == path) return &opens[i-1];
        return 0;
    }
    unsigned successfulOpens(const std::wstring &path) const
    {
        unsigned result = 0;
        for (size_t i = 0; i != opens.size(); ++i)
            if (opens[i].path == path && opens[i].handle != INVALID_HANDLE_VALUE) ++result;
        return result;
    }
    bool allClosed() const
    {
        for (size_t i = 0; i != opens.size(); ++i) if (opens[i].live) return false;
        return true;
    }
};
Observations observed;
HANDLE WINAPI createFile(LPCWSTR path, DWORD access, DWORD sharing,
    LPSECURITY_ATTRIBUTES security, DWORD disposition, DWORD flags, HANDLE templateFile)
{
    if (!observed.seamFired && path == observed.seamOpenPath)
    {
        observed.seamFired = true;
        observed.seamMoved = ::MoveFileExW(observed.seamMoveFrom.c_str(), observed.seamMoveTo.c_str(), 0) != FALSE;
        observed.seamError = observed.seamMoved ? ERROR_SUCCESS : GetLastError();
        // Restore only the exact fresh fixture directory if an unsafe rename succeeded.
        // This restoration must not turn the failed exclusion assertion into success.
        if (observed.seamMoved)
            need(::MoveFileExW(observed.seamMoveTo.c_str(), observed.seamMoveFrom.c_str(), 0) != FALSE,
                "restore task-owned component after adversarial seam probe");
    }
    HANDLE result = ::CreateFileW(path, access, sharing, security, disposition, flags, templateFile);
    const DWORD error = result == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
    OpenEvent event = {result, path ? path : L"", access, sharing, disposition, flags, error,
        result != INVALID_HANDLE_VALUE, 0, 0, 0, 0};
    observed.opens.push_back(event);
    SetLastError(error);
    return result;
}
BOOL WINAPI readFile(HANDLE h, LPVOID bytes, DWORD count, LPDWORD read, LPOVERLAPPED overlap)
{
    OpenEvent *event = observed.live(h);
    if (event) ++event->reads;
    const bool shortRead = event && !observed.fired && event->path == observed.shortReadPath && count > 0;
    if (shortRead) observed.fired = true;
    return ::ReadFile(h, bytes, shortRead ? count - 1 : count, read, overlap);
}
BOOL WINAPI writeFile(HANDLE h, LPCVOID bytes, DWORD count, LPDWORD written, LPOVERLAPPED overlap)
{
    OpenEvent *event = observed.live(h);
    if (event) ++event->writes;
    const bool shortWrite = event && !observed.fired && event->path == observed.shortWritePath && count > 0;
    if (shortWrite) observed.fired = true;
    return ::WriteFile(h, bytes, shortWrite ? count - 1 : count, written, overlap);
}
BOOL WINAPI flushFile(HANDLE h)
{
    OpenEvent *event = observed.live(h);
    const BOOL actual = ::FlushFileBuffers(h);
    if (event && !observed.fired && event->path == observed.flushFailurePath)
    {
        observed.fired = true;
        SetLastError(ERROR_WRITE_FAULT);
        return FALSE;
    }
    return actual;
}
BOOL WINAPI closeHandle(HANDLE h)
{
    OpenEvent *event = observed.live(h);
    const bool reportFailure = event && !observed.fired && event->path == observed.closeFailurePath;
    const BOOL actual = ::CloseHandle(h);
    if (event && actual) event->live = false;
    if (reportFailure)
    {
        observed.fired = true;
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    return actual;
}
std::wstring pathForHandle(HANDLE h)
{
    wchar_t path[MAX_PATH] = {};
    const DWORD count = ::GetFinalPathNameByHandleW(h, path, MAX_PATH, 0);
    if (count < 7 || count >= MAX_PATH || std::wcsncmp(path, L"\\\\?\\", 4) != 0)
        return std::wstring();
    return std::wstring(path + 4, count - 4);
}
HANDLE WINAPI reopenFile(HANDLE original, DWORD access, DWORD sharing, DWORD flags)
{
    const std::wstring path = pathForHandle(original);
    HANDLE result = ::ReOpenFile(original, access, sharing, flags);
    const DWORD error = result == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
    OpenEvent event = {result, path, access, sharing, OPEN_EXISTING, flags, error,
        result != INVALID_HANDLE_VALUE, 0, 0, 0, 0};
    observed.opens.push_back(event);
    SetLastError(error);
    return result;
}
BOOL WINAPI duplicateHandle(HANDLE sourceProcess, HANDLE source, HANDLE targetProcess,
    LPHANDLE target, DWORD access, BOOL inherit, DWORD options)
{
    const std::wstring path = pathForHandle(source);
    const BOOL result = ::DuplicateHandle(sourceProcess, source, targetProcess, target,
        access, inherit, options);
    const DWORD error = result ? ERROR_SUCCESS : GetLastError();
    OpenEvent event = {result ? *target : INVALID_HANDLE_VALUE, path, access, 0,
        OPEN_EXISTING, options, error, result != FALSE, 0, 0, 0, 0};
    observed.opens.push_back(event);
    SetLastError(error);
    return result;
}
BOOL WINAPI information(HANDLE h, LPBY_HANDLE_FILE_INFORMATION info)
{
    OpenEvent *event = observed.live(h);
    if (event) ++event->identityQueries;
    return ::GetFileInformationByHandle(h, info);
}
DWORD WINAPI finalPath(HANDLE h, LPWSTR path, DWORD count, DWORD flags)
{
    OpenEvent *event = observed.live(h);
    if (event) ++event->finalPathQueries;
    return ::GetFinalPathNameByHandleW(h, path, count, flags);
}
} // namespace held_file_test

#define CreateFileW held_file_test::createFile
#define ReadFile held_file_test::readFile
#define WriteFile held_file_test::writeFile
#define FlushFileBuffers held_file_test::flushFile
#define CloseHandle held_file_test::closeHandle
#define ReOpenFile held_file_test::reopenFile
#define DuplicateHandle held_file_test::duplicateHandle
#define GetFileInformationByHandle held_file_test::information
#define GetFinalPathNameByHandleW held_file_test::finalPath
// Isolate inline/private symbols from the linked real Runtime translation unit.
// The included actual body is unchanged; this prevents an ODR/linker collision
// between wrapped fixture calls and the production object's unwrapped methods.
namespace
{
#include "NativeReceiptTraceFilesUnderTest.inc"
}
#undef CreateFileW
#undef ReadFile
#undef WriteFile
#undef FlushFileBuffers
#undef CloseHandle
#undef ReOpenFile
#undef DuplicateHandle
#undef GetFileInformationByHandle
#undef GetFinalPathNameByHandleW

namespace held_file_test
{
using native_receipt_files::NativeReceiptTraceFiles;
namespace fixture
{
using namespace rts::performance;
#include "NativeReceiptActualSourceFixture.inc"
}

void newBytes(const std::wstring &path, const std::vector<unsigned char> &bytes)
{
    Handle file(::CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
        0, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, 0));
    need(file.get() != INVALID_HANDLE_VALUE, "fresh CREATE_NEW fixture file");
    for (size_t offset = 0; offset != bytes.size(); )
    {
        const DWORD count = static_cast<DWORD>((std::min)(bytes.size() - offset, size_t(65536)));
        DWORD written = 0;
        need(::WriteFile(file.get(), bytes.data() + offset, count, &written, 0) && written == count,
            "fixture exact write");
        offset += count;
    }
    need(::FlushFileBuffers(file.get()) != FALSE, "fixture flush");
    file.close();
}
std::vector<unsigned char> readBytes(const std::wstring &path)
{
    Handle file(::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE |
        FILE_SHARE_DELETE, 0, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, 0));
    need(file.get() != INVALID_HANDLE_VALUE, "independent fixture reopen for verification");
    LARGE_INTEGER size = {};
    need(::GetFileSizeEx(file.get(), &size) && size.QuadPart >= 0 &&
        static_cast<unsigned long long>(size.QuadPart) <= ReceiptMaximumBytes + 65536,
        "independent fixture read is bounded, never a native-ceiling allocation");
    std::vector<unsigned char> bytes(static_cast<size_t>(size.QuadPart));
    DWORD count = 0;
    if (!bytes.empty())
        need(::ReadFile(file.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &count, 0) &&
            count == bytes.size(), "independent fixture exact read");
    file.close();
    return bytes;
}
void mutateByte(const std::wstring &path, bool append)
{
    Handle file(::CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
        0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0));
    need(file.get() != INVALID_HANDLE_VALUE, "pre-admission trace mutation open");
    LARGE_INTEGER zero = {};
    need(::SetFilePointerEx(file.get(), zero, 0, append ? FILE_END : FILE_BEGIN) != FALSE,
        "pre-admission trace mutation offset");
    const unsigned char changed = 0xfe;
    DWORD written = 0;
    need(::WriteFile(file.get(), &changed, 1, &written, 0) && written == 1, "trace mutation write");
    file.close();
}
std::wstring child(const std::wstring &root, const wchar_t *name)
{
    const std::wstring result = root + L"\\" + name;
    need(result.size() + 1 < MAX_PATH && ::CreateDirectoryW(result.c_str(), 0),
        "fresh task-owned fixture directory (never reuse an existing case)");
    return result;
}
struct SourceFixture
{
    std::wstring bundle, receiptPath, tracePath;
    PerformanceReceipt receipt;
    std::vector<unsigned char> trace, json;
    KernelPerformanceDigest expected;
    explicit SourceFixture(const std::wstring &directory) : bundle(directory),
        receiptPath(directory + L"\\source-receipt.json"), tracePath(directory + L"\\attempt-trace.bin")
    {
        need(fixture::makeActualWorldTraceSourceReceipt(receipt, trace),
            "actual A world trace fixture prerequisite (not transport RED)");
        need(sameDigest(independentSha(trace), receipt.kernelReference.trace.digest),
            "actual A trace digest equals independent SHA");
        receipt.receiptPath = ascii(receiptPath);
        receipt.traceFiles.tracePath = ascii(tracePath);
        encode();
    }
    void encode()
    {
        std::string text, reason;
        need(SerializePerformanceReceipt(receipt, text, &reason), "real V6 writer prerequisite");
        json.assign(text.begin(), text.end());
        expected = independentSha(json);
        PerformanceReceipt parsed;
        need(ParsePerformanceReceiptSource(json.data(), json.size(), parsed, &reason),
            "real V6 reader accepts complete fixture before transport mutation");
        need(parsed.receiptPath == receipt.receiptPath && parsed.traceFiles.tracePath == receipt.traceFiles.tracePath &&
            sameDigest(parsed.kernelReference.trace.digest, receipt.kernelReference.trace.digest),
            "writer/reader prerequisite preserves selected fixture metadata");
    }
    void write(bool writeTrace = true)
    {
        newBytes(receiptPath, json);
        if (writeTrace) newBytes(tracePath, trace);
    }
};
void deniedMutation(const std::wstring &path, bool directory = false)
{
    const DWORD access[] = {GENERIC_WRITE, DELETE};
    for (unsigned i = 0; i != 2; ++i)
    {
        HANDLE h = ::CreateFileW(path.c_str(), access[i], FILE_SHARE_READ | FILE_SHARE_WRITE |
            FILE_SHARE_DELETE, 0, OPEN_EXISTING, directory ? FILE_FLAG_BACKUP_SEMANTICS : 0, 0);
        const DWORD error = h == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
        if (h != INVALID_HANDLE_VALUE) ::CloseHandle(h);
        require(h == INVALID_HANDLE_VALUE && error == ERROR_SHARING_VIOLATION,
            "live retained file/bundle denies new write and delete access");
    }
}
void mutationAccessPrerequisite(const std::wstring &path, bool directory = false)
{
    const DWORD access[] = {GENERIC_WRITE, DELETE};
    for (unsigned i = 0; i != 2; ++i)
    {
        Handle h(::CreateFileW(path.c_str(), access[i], FILE_SHARE_READ | FILE_SHARE_WRITE |
            FILE_SHARE_DELETE, 0, OPEN_EXISTING, directory ? FILE_FLAG_BACKUP_SEMANTICS : 0, 0));
        need(h.get() != INVALID_HANDLE_VALUE, "unheld write/delete access prerequisite before sharing-denial assertion");
        h.close();
    }
}
void deniedRename(const std::wstring &path)
{
    const std::wstring destination = path + L".moved";
    const bool moved = ::MoveFileExW(path.c_str(), destination.c_str(), 0) != FALSE;
    const DWORD error = moved ? ERROR_SUCCESS : GetLastError();
    if (moved) need(::MoveFileExW(destination.c_str(), path.c_str(), 0) != FALSE,
        "restore exact task-owned name after unexpected rename success");
    require(!moved && error == ERROR_SHARING_VIOLATION, "actual retained content rename/replacement is denied");
}
void released(const std::wstring &path)
{
    Handle h(::CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE | DELETE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0, OPEN_EXISTING, 0, 0));
    require(h.get() != INVALID_HANDLE_VALUE, "owned input/output handle released after explicit closure or failure");
    h.close();
}
void assertSourceOpen(NativeReceiptTraceFiles &owner, SourceFixture &f)
{
    std::string reason;
    require(owner.openSource(ascii(f.receiptPath), f.expected, NativeMaximumBytes, &reason),
        "healthy real V6/A fixture must admit; an unsupported native policy is not an I/O test result");
    require(owner.source() != 0 && sameDigest(owner.sourceReceiptDigest(), f.expected),
        "source publication only after exact retained receipt hash/parse and held trace admission");
    require(owner.source()->receiptPath == ascii(f.receiptPath) &&
        owner.source()->traceFiles.tracePath == ascii(f.tracePath), "actual held bundle paths survive parsing");
}
void matchingPositive(const std::wstring &root, const wchar_t *name)
{
    SourceFixture f(child(root, name)); f.write();
    observed = Observations();
    {
        NativeReceiptTraceFiles owner;
        assertSourceOpen(owner, f);
        require(owner.finishSource(), "matched healthy source closes successfully");
    }
    require(observed.allClosed(), "matched positive leaked no handle");
}

void sourcePositive(const std::wstring &root)
{
    SourceFixture f(child(root, L"source-positive")); f.write();
    mutationAccessPrerequisite(f.receiptPath); mutationAccessPrerequisite(f.tracePath);
    mutationAccessPrerequisite(f.bundle, true);
    observed = Observations();
    {
        NativeReceiptTraceFiles owner;
        assertSourceOpen(owner, f);
        deniedMutation(f.receiptPath); deniedMutation(f.tracePath); deniedMutation(f.bundle, true);
        deniedRename(f.receiptPath); deniedRename(f.tracePath);
        const OpenEvent *receiptEvent = observed.last(f.receiptPath);
        const OpenEvent *traceEvent = observed.last(f.tracePath);
        require(receiptEvent && traceEvent && receiptEvent->handle != traceEvent->handle &&
            receiptEvent->live && traceEvent->live && receiptEvent->reads > 0 && traceEvent->reads > 0,
            "receipt parse buffer and trace prehash read the two live original handles");
        for (unsigned i = 0; i != 2; ++i)
        {
            const OpenEvent &event = i == 0 ? *receiptEvent : *traceEvent;
            require(event.access == GENERIC_READ && event.sharing == FILE_SHARE_READ &&
                event.disposition == OPEN_EXISTING && (event.flags & FILE_FLAG_OPEN_REPARSE_POINT) != 0,
                "input uses exact approved access/sharing/disposition/reparse policy");
            DWORD flags = 0;
            require(::GetHandleInformation(event.handle, &flags) && (flags & HANDLE_FLAG_INHERIT) == 0,
                "source handle is uninheritable");
            HANDLE writable = ::CreateFileMappingW(event.handle, 0, PAGE_READWRITE, 0, 0, 0);
            if (writable) ::CloseHandle(writable);
            require(writable == 0, "the actual held read-only source handle cannot create a writable mapping");
        }
        const unsigned readBefore = traceEvent->reads;
        std::vector<unsigned char> actual(f.trace.size());
        // Nonsequential order rules out accidental dependence on hash's file pointer.
        require(NativeReceiptTraceFiles::readAt(&owner, 500, actual.data()+500,
            static_cast<unsigned>(actual.size()-500)), "same held trace tail read");
        require(NativeReceiptTraceFiles::readAt(&owner, 0, actual.data(), 500), "same held trace prefix read");
        require(actual == f.trace && observed.last(f.tracePath)->reads >= readBefore + 2,
            "readAt returns the exact A bytes from the already observed original trace handle");
        require(observed.successfulOpens(f.receiptPath) == 1 && observed.successfulOpens(f.tracePath) == 1,
            "hash, parse and readAt never reopen either content pathname");
        // Also give the actual A consumer these callbacks. The fixture is a core
        // one-world/control0 trace; this is not native 0x5004/control1 acceptance.
        KernelPerformanceReferenceRunOptions options;
        options.mode = KERNEL_REFERENCE_PHASE_BASELINE_BINDING;
        options.trace.mode = KERNEL_TRACE_CONSUME;
        options.trace.binding = f.receipt.kernelReference.trace.binding;
        options.trace.limits = f.receipt.kernelReference.trace.limits;
        options.trace.residentAttemptCapacity = f.receipt.kernelReference.trace.residentAttemptCapacity;
        options.trace.residentRangeCapacity = f.receipt.kernelReference.trace.residentRangeCapacity;
        options.trace.readAt = NativeReceiptTraceFiles::readAt;
        options.trace.context = &owner;
        options.trace.sourceByteCount = f.trace.size();
        options.trace.sourceTraceDigest = f.receipt.kernelReference.trace.digest;
        options.trace.sourceReceiptDigest = owner.sourceReceiptDigest();
        fixture::ReceiptWindowClock clock = {100, 0};
        options.clock = fixture::ReceiptWindowClock::read; options.clockContext = &clock;
        KernelPerformanceReferenceLedger consumer;
        require(consumer.beginRun(options), "actual A source prevalidation consumes held-file readAt");
        KernelPerformanceWindowBoundary boundary;
        boundary.kind = KERNEL_WINDOW_BEGIN; boundary.sampleOrdinal = 1;
        boundary.phase = KERNEL_PHASE_COUNT;
        boundary.ownerFrameAtEntry = boundary.authorityFrame = boundary.actualOwnerFrame = 0;
        require(consumer.observeWindowBoundary(boundary), "actual held-source consumer begins world window");
        for (unsigned phase = 0; phase != KERNEL_PHASE_COUNT; ++phase)
        {
            boundary.phase = static_cast<KernelPerformancePhase>(phase);
            boundary.kind = KERNEL_WINDOW_PHASE_BEGIN; boundary.actualOwnerFrame = 0;
            require(consumer.observeWindowBoundary(boundary), "held-source consumer phase begin");
            boundary.kind = KERNEL_WINDOW_PHASE_END;
            boundary.actualOwnerFrame = phase+1 == KERNEL_PHASE_COUNT ? 1 : 0;
            require(consumer.observeWindowBoundary(boundary), "held-source consumer phase end");
        }
        boundary.kind = KERNEL_WINDOW_WORLD_END; boundary.phase = KERNEL_PHASE_COUNT; boundary.actualOwnerFrame = 1;
        require(consumer.observeWindowBoundary(boundary) && consumer.sealObservationWindow() && consumer.sealExecutionClosure(),
            "held-file actual A consumer follows real window and seal grammar");
        const KernelPerformanceReferenceSnapshot consumed = consumer.freeze();
        require(consumed.trace.complete && consumed.trace.errors == 0 &&
            consumed.trace.byteCount == f.trace.size() && sameDigest(consumed.trace.digest, f.receipt.kernelReference.trace.digest),
            "actual consumer closes the exact admitted trace without a substitute transcript");
        require(owner.finishSource(), "after-use source revalidation and checked close");
        require(observed.successfulOpens(f.receiptPath) == 1 && observed.successfulOpens(f.tracePath) == 1,
            "actual A consumption and final source verification also preserve the original handles");
        require(observed.last(f.receiptPath)->identityQueries >= 2 && observed.last(f.tracePath)->identityQueries >= 2 &&
            observed.last(f.receiptPath)->finalPathQueries >= 2 && observed.last(f.tracePath)->finalPathQueries >= 2,
            "closure repeats real held identity and normalized-path queries");
        require(!NativeReceiptTraceFiles::readAt(&owner, 0, actual.data(), 1), "readAt cannot outlive explicit source closure");
    }
    require(observed.allClosed(), "source success closes all owned handles");
    released(f.receiptPath); released(f.tracePath);
    require(readBytes(f.tracePath) == f.trace && readBytes(f.receiptPath) == f.json,
        "source use did not modify either retained evidence file");
}

void outputPositive(const std::wstring &root)
{
    SourceFixture f(child(root, L"output-positive"));
	const std::wstring rawPath = f.bundle + L"\\raw.log";
	const std::wstring timingPath = f.bundle + L"\\timing.csv";
	const std::wstring publishedPath = f.bundle + L"\\performance-receipt-test.json";
	newBytes(rawPath, std::vector<unsigned char>{'r','a','w'});
	newBytes(timingPath, std::vector<unsigned char>{'t','i','m','i','n','g'});
	newBytes(publishedPath, std::vector<unsigned char>{'{','}','\n'});
    mutationAccessPrerequisite(f.bundle, true);
    observed = Observations();
    {
        NativeReceiptTraceFiles owner;
        require(owner.openOutput(ascii(f.bundle), ascii(f.tracePath), NativeMaximumBytes), "exclusive trace output admission");
        const OpenEvent *event = observed.last(f.tracePath);
        require(event && event->live && event->access == (GENERIC_READ | GENERIC_WRITE) &&
            event->sharing == FILE_SHARE_READ && event->disposition == CREATE_NEW,
            "output uses read-write CREATE_NEW with only reader sharing");
        DWORD flags = 0;
        require(::GetHandleInformation(event->handle, &flags) && !(flags & HANDLE_FLAG_INHERIT), "output is uninheritable");
        LARGE_INTEGER size = {};
        require(::GetFileSizeEx(event->handle, &size) && size.QuadPart == 0,
            "transport open itself publishes no trace header before Runtime bind");
        deniedMutation(f.tracePath); deniedMutation(f.bundle, true);
        deniedRename(f.tracePath);
        require(NativeReceiptTraceFiles::append(&owner, f.trace.data(), 17) &&
            NativeReceiptTraceFiles::append(&owner, f.trace.data()+17, 789) &&
            NativeReceiptTraceFiles::append(&owner, f.trace.data()+806, static_cast<unsigned>(f.trace.size()-806)),
            "existing callback exactly appends real A trace fragments");
		require(owner.sealOutput(f.receipt.kernelReference.trace),
			"output flush and exact trace identity seal retain publication authority");
		Handle raw(::CreateFileW(rawPath.c_str(), GENERIC_READ, FILE_SHARE_READ, 0,
			OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, 0));
		Handle timing(::CreateFileW(timingPath.c_str(), GENERIC_READ, FILE_SHARE_READ, 0,
			OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, 0));
		require(raw.get() != INVALID_HANDLE_VALUE && timing.get() != INVALID_HANDLE_VALUE,
			"producer-held evidence handle fixture prerequisite");
		std::string evidenceReason;
		const bool evidenceHeld = owner.holdEvidence(raw.release(), ascii(rawPath),
			timing.release(), ascii(timingPath), &evidenceReason);
		require(evidenceHeld,
			evidenceReason.empty() ?
			"closed raw and timing artifacts are identity-held before receipt publication" :
			evidenceReason.c_str());
		require(observed.successfulOpens(rawPath) == 0 &&
			observed.successfulOpens(timingPath) == 0,
			"evidence admission consumes producer-held handles without pathname reopen");
		deniedMutation(rawPath); deniedMutation(timingPath); deniedMutation(f.tracePath);
		Handle published(::CreateFileW(publishedPath.c_str(), GENERIC_READ,
			FILE_SHARE_READ | FILE_SHARE_DELETE, 0, OPEN_EXISTING,
			FILE_FLAG_OPEN_REPARSE_POINT, 0));
		require(published.get() != INVALID_HANDLE_VALUE &&
			NativeReceiptTraceFiles::retainPublication(&owner, published.get(),
				ascii(publishedPath).c_str(), 0),
			"writer-held published receipt identity is retained before its original handle closes");
		require(owner.finishPublication(ascii(publishedPath)),
			"receipt publication revalidates trace/raw/timing/output identities before checked closure");
        require(observed.successfulOpens(f.tracePath) == 1 && observed.last(f.tracePath)->reads > 0,
            "output final SHA reads the original output handle, not a pathname reopen");
        require(observed.last(f.tracePath)->identityQueries >= 2 && observed.last(f.tracePath)->finalPathQueries >= 2,
            "output repeats real identity/final-path checks at closure");
		require(!NativeReceiptTraceFiles::append(&owner, f.trace.data(), 1), "append cannot outlive explicit closure");
    }
    require(observed.allClosed(), "output success closes all handles");
    require(readBytes(f.tracePath) == f.trace, "exclusive output matches independently generated A bytes exactly");
    released(f.tracePath);
}

void publicationCloseFailureCleanup(const std::wstring &root)
{
	SourceFixture f(child(root, L"publication-close-cleanup"));
	const std::wstring rawPath = f.bundle + L"\\raw.log";
	const std::wstring timingPath = f.bundle + L"\\timing.csv";
	const std::wstring publishedPath = f.bundle + L"\\performance-receipt-test.json";
	newBytes(rawPath, std::vector<unsigned char>{'r'});
	newBytes(timingPath, std::vector<unsigned char>{'t'});
	newBytes(publishedPath, std::vector<unsigned char>{'{','}'});
	observed = Observations();
	{
		NativeReceiptTraceFiles owner;
		require(owner.openOutput(ascii(f.bundle), ascii(f.tracePath), NativeMaximumBytes) &&
			NativeReceiptTraceFiles::append(&owner, f.trace.data(),
				static_cast<unsigned>(f.trace.size())) &&
			owner.sealOutput(f.receipt.kernelReference.trace),
			"close-fault publication fixture seals its exact trace output");
		Handle raw(::CreateFileW(rawPath.c_str(), GENERIC_READ, FILE_SHARE_READ, 0,
			OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, 0));
		Handle timing(::CreateFileW(timingPath.c_str(), GENERIC_READ, FILE_SHARE_READ, 0,
			OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, 0));
		require(raw.get() != INVALID_HANDLE_VALUE && timing.get() != INVALID_HANDLE_VALUE &&
			owner.holdEvidence(raw.release(), ascii(rawPath), timing.release(), ascii(timingPath)),
			"close-fault publication fixture retains exact raw and timing identities");
		Handle published(::CreateFileW(publishedPath.c_str(), GENERIC_READ,
			FILE_SHARE_READ | FILE_SHARE_DELETE, 0, OPEN_EXISTING,
			FILE_FLAG_OPEN_REPARSE_POINT, 0));
		require(published.get() != INVALID_HANDLE_VALUE &&
			NativeReceiptTraceFiles::retainPublication(&owner, published.get(),
				ascii(publishedPath).c_str(), 0),
			"close-fault publication fixture retains a DELETE-capable exact receipt handle");
		// Trace closes after the published, timing, and raw handles. Reporting this
		// final component close as failed proves the cleanup twin survives every
		// preceding checked close rather than relying on the published pathname.
		observed.closeFailurePath = f.tracePath;
		require(!owner.finishPublication(ascii(publishedPath)) && observed.fired,
			"late checked component-close failure rejects receipt publication");
	}
	require(observed.allClosed(), "publication close-fault cleanup leaks no actual handle");
	require(::GetFileAttributesW(publishedPath.c_str()) == INVALID_FILE_ATTRIBUTES,
		"late checked component-close failure deletes the exact published receipt identity");
}

void baselineOutputDestination(const std::wstring &root)
{
	SourceFixture source(child(root, L"baseline-source")); source.write();
	const std::wstring output = child(root, L"baseline-output");
	const std::wstring rawPath = output + L"\\raw.log";
	const std::wstring timingPath = output + L"\\timing.csv";
	const std::wstring publishedPath = output + L"\\performance-receipt-test.json";
	newBytes(rawPath, std::vector<unsigned char>{'r'});
	newBytes(timingPath, std::vector<unsigned char>{'t'});
	newBytes(publishedPath, std::vector<unsigned char>{'{','}'});
	observed = Observations();
	{
		NativeReceiptTraceFiles owner;
		require(owner.holdOutputDestination(ascii(output)),
			"baseline current-output destination is admitted independently before source consumption");
		require(owner.openSource(ascii(source.receiptPath), source.expected, NativeMaximumBytes),
			"baseline source remains independently admitted");
		deniedMutation(output, true); deniedMutation(source.bundle, true);
		require(owner.sealSource(), "baseline source identity seals without releasing either bundle");
		Handle raw(::CreateFileW(rawPath.c_str(), GENERIC_READ, FILE_SHARE_READ, 0,
			OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, 0));
		Handle timing(::CreateFileW(timingPath.c_str(), GENERIC_READ, FILE_SHARE_READ, 0,
			OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, 0));
		require(raw.get() != INVALID_HANDLE_VALUE && timing.get() != INVALID_HANDLE_VALUE,
			"baseline producer-held evidence prerequisite");
		require(owner.holdEvidence(raw.release(), ascii(rawPath),
			timing.release(), ascii(timingPath)),
			"baseline output evidence is held under the independently admitted destination");
		Handle published(::CreateFileW(publishedPath.c_str(), GENERIC_READ,
			FILE_SHARE_READ | FILE_SHARE_DELETE, 0, OPEN_EXISTING,
			FILE_FLAG_OPEN_REPARSE_POINT, 0));
		require(published.get() != INVALID_HANDLE_VALUE &&
			NativeReceiptTraceFiles::retainPublication(&owner, published.get(),
				ascii(publishedPath).c_str(), 0),
			"baseline receipt identity transfers from the writer-held handle");
		require(owner.finishPublication(ascii(publishedPath)),
			"baseline publication closes source and output identities together");
	}
	require(observed.allClosed(), "baseline publication leaks no source/output/evidence handles");
}

void existingOutput(const std::wstring &root)
{
    SourceFixture f(child(root, L"output-existing"));
    const std::vector<unsigned char> sentinel = {0x81, 0, 0xff, 0x7f, 0x42};
    newBytes(f.tracePath, sentinel);
    observed = Observations();
    {
        NativeReceiptTraceFiles owner;
        require(!owner.openOutput(ascii(f.bundle), ascii(f.tracePath), NativeMaximumBytes), "existing output refuses admission");
        require(!NativeReceiptTraceFiles::append(&owner, f.trace.data(), 1) &&
            !owner.finishOutput(f.receipt.kernelReference.trace), "failed CREATE_NEW cannot recover into publication");
    }
    require(observed.allClosed() && readBytes(f.tracePath) == sentinel, "existing file bytes survive failed output admission");
}

void outputPathMutations(const std::wstring &root)
{
    for (unsigned mutation = 0; mutation != 7; ++mutation)
    {
        wchar_t name[64]; swprintf_s(name, L"output-path-%u", mutation);
        SourceFixture f(child(root, name));
        std::string directory = ascii(f.bundle), trace = ascii(f.tracePath);
        if (mutation == 0) trace = ascii(f.bundle + L"\\another-name.bin");
        if (mutation == 1) trace = ascii(root + L"\\attempt-trace.bin");
        if (mutation == 2)
        {
            const std::wstring sibling = f.bundle + L"-sibling";
            need(::CreateDirectoryW(sibling.c_str(), 0), "real prefix-sibling output directory prerequisite");
            trace = ascii(sibling + L"\\attempt-trace.bin");
        }
        if (mutation == 3)
        {
            const std::wstring sub = child(f.bundle, L"nested");
            trace = ascii(sub + L"\\attempt-trace.bin");
        }
        if (mutation == 4) directory += "\\.";
        if (mutation == 5)
        {
            directory = ascii(f.bundle + L"\\absent-directory");
            trace = directory + "\\attempt-trace.bin";
        }
        observed = Observations();
        {
            NativeReceiptTraceFiles owner;
            require(!owner.openOutput(directory, trace, mutation == 6 ? 0 : NativeMaximumBytes),
                "alternate/outside/prefix/nested/invalid/missing output path or zero cap refuses");
            for (size_t i = 0; i != observed.opens.size(); ++i)
                require(observed.opens[i].disposition != CREATE_NEW,
                    "output contract failure is detected before exclusive content creation");
        }
        require(observed.allClosed(), "invalid output path closes temporary component holds");
        require(::GetFileAttributesW(wide(trace).c_str()) == INVALID_FILE_ATTRIBUTES,
            "invalid output selection creates no artifact");
    }
}

void sourceMutations(const std::wstring &root)
{
    matchingPositive(root, L"source-mutation-control");
    for (unsigned mutation = 0; mutation != 11; ++mutation)
    {
        wchar_t name[64]; swprintf_s(name, L"source-mutation-%u", mutation);
        SourceFixture f(child(root, name));
        if (mutation == 0) f.expected.bytes[0] ^= 1;
        if (mutation == 1) { f.receipt.receiptPath += ".different"; f.encode(); }
        if (mutation == 2) { f.receipt.traceFiles.tracePath = ascii(root + L"\\attempt-trace.bin"); f.encode(); }
        if (mutation == 3) { f.receipt.traceFiles.tracePath = ascii(f.bundle + L"\\other-trace.bin"); f.encode(); }
        if (mutation == 4) { f.receipt.traceFiles.tracePath = ascii(f.bundle + L"-sibling\\attempt-trace.bin"); f.encode(); }
        if (mutation == 8) { f.receipt.traceFiles.tracePath = ascii(f.bundle) + "\\caf\xc3\xa9\\attempt-trace.bin"; f.encode(); }
        f.write(mutation < 9);
        if (mutation == 9) newBytes(f.tracePath, std::vector<unsigned char>());
        if (mutation == 5) mutateByte(f.tracePath, false);
        if (mutation == 6) mutateByte(f.tracePath, true);
        observed = Observations();
        {
            NativeReceiptTraceFiles owner;
            std::string reason;
            const JobMetricCounter cap = mutation == 7 ? f.trace.size()-1 : NativeMaximumBytes;
            require(!owner.openSource(ascii(f.receiptPath), f.expected, cap, &reason), "changed selected bytes/path/extent refuses source");
            require(owner.source() == 0 && !owner.sourceReceiptDigest().valid && !reason.empty(),
                "failed source admission clears all outward source authority");
            unsigned char byte = 0;
            require(!NativeReceiptTraceFiles::readAt(&owner, 0, &byte, 1) && !owner.finishSource(),
                "failed source cannot read or qualify");
            if (mutation >= 2 && mutation <= 4)
                require(observed.last(wide(f.receipt.traceFiles.tracePath)) == 0,
                    "outside/direct-child/alternate-name rejection precedes content open");
            if (mutation == 8)
                require(observed.successfulOpens(f.tracePath) == 0,
                    "valid Unicode parsed trace path is outside this initial ASCII transport subset");
        }
        require(observed.allClosed(), "source failure closes all acquired handles");
        released(f.receiptPath);
        if (mutation != 10) released(f.tracePath);
        else require(::GetFileAttributesW(f.tracePath.c_str()) == INVALID_FILE_ATTRIBUTES,
            "missing trace is not created as an admission fallback");
    }
}

void receiptSizeAndParse(const std::wstring &root)
{
    for (unsigned mutation = 0; mutation != 4; ++mutation)
    {
        wchar_t name[64]; swprintf_s(name, L"receipt-extent-%u", mutation);
        SourceFixture f(child(root, name));
        if (mutation <= 1) f.json.resize(ReceiptMaximumBytes + mutation, ' ');
        if (mutation == 2) f.json.clear();
        if (mutation == 3) f.json.assign(1, '{');
        f.expected = independentSha(f.json); f.write();
        if (mutation == 0)
        {
            PerformanceReceipt parsed;
            need(ParsePerformanceReceiptSource(f.json.data(), f.json.size(), parsed), "exact 4 MiB whitespace-padded valid receipt prerequisite");
        }
        observed = Observations();
        {
            NativeReceiptTraceFiles owner;
            const bool ok = owner.openSource(ascii(f.receiptPath), f.expected, NativeMaximumBytes);
            require(ok == (mutation == 0), "receipt accepts exact 4 MiB, rejects oversize/empty/malformed content");
            if (ok) require(owner.finishSource(), "exact-boundary receipt closure");
            if (mutation == 1 || mutation == 2)
            {
                const OpenEvent *event = observed.last(f.receiptPath);
                require(event && event->reads == 0, "invalid receipt extent rejects before allocating/reading content");
            }
        }
        require(observed.allClosed(), "receipt extent/parse branch closes all handles");
    }
}

void readBoundsAndFailures(const std::wstring &root)
{
    for (unsigned mutation = 0; mutation != 6; ++mutation)
    {
        wchar_t name[64]; swprintf_s(name, L"source-read-%u", mutation);
        SourceFixture f(child(root, name)); f.write();
        observed = Observations();
        {
            NativeReceiptTraceFiles owner;
            if (mutation >= 4)
            {
                observed.shortReadPath = mutation == 4 ? f.receiptPath : f.tracePath;
                require(!owner.openSource(ascii(f.receiptPath), f.expected, NativeMaximumBytes) && observed.fired,
                    "actual short receipt/prehash read fails admission, never retried into success");
            }
            else
            {
                assertSourceOpen(owner, f);
                unsigned char bytes[4] = {};
                const unsigned before = observed.last(f.tracePath)->reads;
                if (mutation == 0) require(!NativeReceiptTraceFiles::readAt(&owner, f.trace.size(), bytes, 1), "EOF readAt refuses");
                if (mutation == 1) require(!NativeReceiptTraceFiles::readAt(&owner, f.trace.size()-1, bytes, 2), "cross-extent readAt refuses");
                if (mutation == 2) require(!NativeReceiptTraceFiles::readAt(&owner,
                    (std::numeric_limits<JobMetricCounter>::max)()-1, bytes, 4), "u64 readAt addition overflow refuses");
                if (mutation == 3)
                {
                    observed.shortReadPath = f.tracePath;
                    require(!NativeReceiptTraceFiles::readAt(&owner, 0, bytes, 4) && observed.fired,
                        "real partial ReadFile completion refuses exact readAt");
                }
                else require(observed.last(f.tracePath)->reads == before, "bounds failure precedes any ReadFile");
                require(!NativeReceiptTraceFiles::readAt(&owner, 0, bytes, 1) && !owner.finishSource(),
                    "readAt failure is sticky through later valid request and finalization");
            }
        }
        require(observed.allClosed(), "short/invalid source read cleanup");
        released(f.receiptPath); released(f.tracePath);
    }
}

void outputFailures(const std::wstring &root)
{
    for (unsigned mutation = 0; mutation != 6; ++mutation)
    {
        wchar_t name[64]; swprintf_s(name, L"output-failure-%u", mutation);
        SourceFixture f(child(root, name));
        observed = Observations();
        {
            NativeReceiptTraceFiles owner;
            // Small internal transport cap tests arithmetic only. Runtime still has
            // exactly 16 GiB; this does not offer a native override or policy knob.
            const JobMetricCounter cap = mutation == 0 ? f.trace.size()-1 : NativeMaximumBytes;
            require(owner.openOutput(ascii(f.bundle), ascii(f.tracePath), cap), "output failure case starts with actual successful admission");
            if (mutation == 1) observed.shortWritePath = f.tracePath;
            const bool appended = NativeReceiptTraceFiles::append(&owner, f.trace.data(), static_cast<unsigned>(f.trace.size()));
            if (mutation <= 1)
            {
                require(!appended && (mutation == 0 || observed.fired), "cap/actual partial WriteFile refuses whole append");
                require(observed.last(f.tracePath)->writes == (mutation == 0 ? 0U : 1U), "no I/O on cap failure, no retry after partial write");
            }
            else require(appended, "full actual A trace append before finalization fault");
            KernelPerformanceTraceSnapshot frozen = f.receipt.kernelReference.trace;
            if (mutation == 2) ++frozen.byteCount;
            if (mutation == 3) frozen.digest.bytes[0] ^= 1;
            if (mutation == 4) observed.flushFailurePath = f.tracePath;
            if (mutation == 5) observed.closeFailurePath = f.tracePath;
            require(!owner.finishOutput(frozen), "cap/write/length/SHA/flush/checked-close failure prevents output qualification");
            if (mutation >= 4) require(observed.fired, "the intended actual flush/close reporting seam was reached");
            require(!NativeReceiptTraceFiles::append(&owner, f.trace.data(), 1), "output failure cannot resume appending");
        }
        require(observed.allClosed(), "output failure releases all live handles");
        const std::vector<unsigned char> retained = readBytes(f.tracePath);
        const size_t expected = mutation == 0 ? 0 : mutation == 1 ? f.trace.size()-1 : f.trace.size();
        require(retained.size() == expected && std::equal(retained.begin(), retained.end(), f.trace.begin()),
            "failure retains the actual partial/full evidence without deletion or truncation");
        released(f.tracePath);
    }
}

class WritableMapping
{
public:
    WritableMapping(const std::wstring &path, bool onlyView, bool readOnly = false) : section(0), view(0)
    {
        Handle file(::CreateFileW(path.c_str(), readOnly ? GENERIC_READ : GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0, OPEN_EXISTING, 0, 0));
        need(file.get() != INVALID_HANDLE_VALUE, "mapping fixture original file handle");
        try
        {
            section = ::CreateFileMappingW(file.get(), 0, readOnly ? PAGE_READONLY : PAGE_READWRITE, 0, 0, 0);
            need(section != 0, "mapping fixture real section");
            if (onlyView)
            {
                view = ::MapViewOfFile(section, readOnly ? FILE_MAP_READ : FILE_MAP_WRITE, 0, 0, 0);
                need(view != 0, "mapping fixture real surviving view");
                need(::CloseHandle(section) != FALSE, "original mapping handle closed while only view survives");
                section = 0;
            }
            file.close(); // Both handle categories are gone in onlyView=true.
            if (view && !readOnly)
            {
                volatile unsigned char *p = static_cast<volatile unsigned char *>(view);
                const unsigned char before = p[0]; p[0] = static_cast<unsigned char>(before ^ 1); p[0] = before;
                need(::FlushViewOfFile(view, 1) != FALSE,
                    "only surviving writable view can actually store after both original handles are closed");
            }
        }
        catch (...)
        {
            if (view) ::UnmapViewOfFile(view);
            if (section) ::CloseHandle(section);
            view = 0; section = 0;
            throw;
        }
    }
    ~WritableMapping() { if (view) ::UnmapViewOfFile(view); if (section) ::CloseHandle(section); }
    void close()
    {
        if (view) { void *old = view; view = 0; need(::UnmapViewOfFile(old) != FALSE, "unmap exact fixture view"); }
        if (section) { HANDLE old = section; section = 0; need(::CloseHandle(old) != FALSE, "close exact fixture section"); }
    }
private:
    WritableMapping(const WritableMapping &); WritableMapping &operator=(const WritableMapping &);
    HANDLE section; void *view;
};
void mappingCases(const std::wstring &root)
{
    for (unsigned fileIndex = 0; fileIndex != 2; ++fileIndex)
        for (unsigned variant = 0; variant != 3; ++variant)
        {
            wchar_t name[64]; swprintf_s(name, L"mapping-%u-%u", fileIndex, variant);
            SourceFixture f(child(root, name)); f.write();
            const std::wstring path = fileIndex == 0 ? f.receiptPath : f.tracePath;
            WritableMapping mapping(path, variant != 1, variant == 2);
            // Actual NTFS/API capability, separate from owner's behavior. No test
            // result from a different filesystem or unsupported setup is reused.
            HANDLE probe = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, 0,
                OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, 0);
            const DWORD error = probe == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
            if (probe != INVALID_HANDLE_VALUE) ::CloseHandle(probe);
            need(variant == 2 ? probe != INVALID_HANDLE_VALUE :
                probe == INVALID_HANDLE_VALUE && error == ERROR_SHARING_VIOLATION,
                "actual surviving mapping API capability prerequisite, not an owner RED");
            observed = Observations();
            {
                NativeReceiptTraceFiles owner;
                const bool ok = owner.openSource(ascii(f.receiptPath), f.expected, NativeMaximumBytes);
                require(ok == (variant == 2), "owner refuses both surviving writable variants, admits read-only view");
                const OpenEvent *event = observed.last(path);
                require(event && (variant == 2 ? event->live : event->error == ERROR_SHARING_VIOLATION),
                    "the intended original mapped file was actually opened with approved sharing");
                if (ok) require(owner.finishSource(), "read-only mapping source closure");
            }
            require(observed.allClosed(), "mapped source branch cleanup");
            mapping.close();
            observed = Observations();
            {
                NativeReceiptTraceFiles owner; assertSourceOpen(owner, f);
                deniedMutation(f.receiptPath); deniedMutation(f.tracePath);
                require(owner.finishSource(), "identical source admits after original writable mapping is gone");
            }
            require(observed.allClosed(), "post-unmap control cleanup");
        }
}

void componentSeams(const std::wstring &root)
{
    for (unsigned output = 0; output != 2; ++output)
    {
        const std::wstring parent = child(root, output ? L"output-component-parent" : L"source-component-parent");
        SourceFixture f(child(parent, L"bundle")); if (!output) f.write();
        const std::wstring movedParent = parent + L"-moved";
        need(::MoveFileExW(parent.c_str(), movedParent.c_str(), 0) != FALSE,
            "unheld ancestor is actually renameable before substitution assertion");
        need(::MoveFileExW(movedParent.c_str(), parent.c_str(), 0) != FALSE,
            "restore exact fresh ancestor prerequisite");
        observed = Observations();
        observed.seamOpenPath = output ? f.tracePath : f.receiptPath;
        observed.seamMoveFrom = parent;
        observed.seamMoveTo = parent + L"-moved";
        {
            NativeReceiptTraceFiles owner;
            const bool ok = output ? owner.openOutput(ascii(f.bundle), ascii(f.tracePath), NativeMaximumBytes) :
                owner.openSource(ascii(f.receiptPath), f.expected, NativeMaximumBytes);
            require(ok && observed.seamFired, "component substitution test reached the actual child-open boundary");
            require(!observed.seamMoved && observed.seamError == ERROR_SHARING_VIOLATION,
                "checked ancestor remains held across actual child open/final-path validation");
            const OpenEvent *ancestor = observed.last(parent);
            const OpenEvent *bundle = observed.last(f.bundle);
            require(ancestor && !ancestor->live && bundle && bundle->live,
                "broader ancestors release after admission; exact bundle remains live");
            require(ancestor->identityQueries && ancestor->finalPathQueries &&
                (ancestor->flags & FILE_FLAG_OPEN_REPARSE_POINT) && (ancestor->flags & FILE_FLAG_BACKUP_SEMANTICS),
                "component path checks use actual opened directory identities/normalized names");
            if (output)
            {
                require(NativeReceiptTraceFiles::append(&owner, f.trace.data(), static_cast<unsigned>(f.trace.size())), "component output actual trace append");
                require(owner.finishOutput(f.receipt.kernelReference.trace), "component output closure");
            }
            else require(owner.finishSource(), "component source closure");
        }
        require(observed.allClosed(), "component holds all close");
    }
}

void lexicalPaths(const std::wstring &root)
{
    matchingPositive(root, L"literal~tilde-long-name");
    SourceFixture f(child(root, L"lexical-paths")); f.write();
    const std::string path = ascii(f.receiptPath), bundle = ascii(f.bundle);
    std::vector<std::string> invalid = {
        "relative.json", "H:relative.json", "\\root-relative.json", "\\\\server\\share\\source.json",
        "\\\\?\\" + path, "\\\\.\\" + path, path + ":stream", bundle + "\\.\\source-receipt.json",
        bundle + "\\..\\source-receipt.json", bundle + "\\\\source-receipt.json", path + " ", path + ".",
        bundle + "\\NUL", bundle + "\\CON.txt", bundle + "\\COM1.json", bundle + "\\LPT9.json",
        bundle + "\\bad?.json", bundle + "\\bad*.json", bundle + "\\bad|.json", bundle + "\\bad<.json",
        bundle + "\\bad>.json", bundle + "\\bad\".json", bundle + "\\bad\x01.json",
        bundle + "\\non-ascii-\xc3\xa9.json", bundle + "\\bad-\xc0\xaf.json", bundle + "\\bad-\xed\xa0\x80.json",
        path + std::string(1, '\0') + "suffix", bundle + "\\" + std::string(MAX_PATH, 'a')
    };
    std::string slash = path; std::replace(slash.begin(), slash.end(), '\\', '/'); invalid.push_back(slash);
    for (size_t i = 0; i != invalid.size(); ++i)
    {
        observed = Observations();
        NativeReceiptTraceFiles owner;
        require(!owner.openSource(invalid[i], f.expected, NativeMaximumBytes), "invalid source lexical path refuses");
        require(observed.opens.empty(), "invalid original/parsed transport path is rejected before any file open");
    }
    // A long-name path containing a literal tilde is not automatically an 8.3
    // alias. Actual alias rejection is tested by normalized returned names below.
}

void hardLinksAndIdentity(const std::wstring &root)
{
    matchingPositive(root, L"hardlink-control");
    for (unsigned target = 0; target != 2; ++target)
    {
        wchar_t name[64]; swprintf_s(name, L"hardlink-%u", target);
        SourceFixture f(child(root, name)); f.write();
        const std::wstring path = target ? f.tracePath : f.receiptPath;
        need(::CreateHardLinkW((f.bundle + L"\\second-name.bin").c_str(), path.c_str(), 0) != FALSE, "real NTFS hardlink prerequisite");
        Handle check(::CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE |
            FILE_SHARE_DELETE, 0, OPEN_EXISTING, 0, 0));
        BY_HANDLE_FILE_INFORMATION info = {};
        need(check.get() != INVALID_HANDLE_VALUE && ::GetFileInformationByHandle(check.get(), &info) &&
            info.nNumberOfLinks == 2, "actual hardlink count prerequisite"); check.close();
        observed = Observations();
        {
            NativeReceiptTraceFiles owner;
            require(!owner.openSource(ascii(f.receiptPath), f.expected, NativeMaximumBytes), "two-link source rejects");
            const OpenEvent *event = observed.last(path);
            require(event && event->identityQueries && event->reads == 0,
                "actual two-link file is rejected before its content read, not by downstream policy");
        }
        require(observed.allClosed(), "hardlink admission cleanup");
    }
    SourceFixture same(child(root, L"same-file-identity"));
    same.receiptPath = same.tracePath; same.receipt.receiptPath = ascii(same.receiptPath); same.encode(); same.write(false);
    observed = Observations();
    {
        NativeReceiptTraceFiles owner; std::string reason;
        require(!owner.openSource(ascii(same.receiptPath), same.expected, NativeMaximumBytes, &reason), "same actual file cannot be receipt and trace");
        // Diagnostic is deliberately the private pre-content identity check. Without
        // it this fixture also fails trace length/hash and would mask a removed
        // identity predicate. No fake ID or forged compatible JSON/binary is used.
        require(reason.find("same file identity") != std::string::npos,
            "same-file fixture reaches the explicit identity refusal, not an unrelated content failure");
    }
    require(observed.allClosed(), "same-identity branch cleanup");
}

void sourceCloseFailure(const std::wstring &root)
{
    for (unsigned target = 0; target != 3; ++target)
    {
        wchar_t name[64]; swprintf_s(name, L"source-close-%u", target);
        SourceFixture f(child(root, name)); f.write(); observed = Observations();
        {
            NativeReceiptTraceFiles owner; assertSourceOpen(owner, f);
            observed.closeFailurePath = target == 0 ? f.receiptPath : target == 1 ? f.tracePath : f.bundle;
            require(!owner.finishSource() && observed.fired, "receipt/trace/bundle checked-close failure prevents qualification");
        }
        require(observed.allClosed(), "reported close failure does not leak actual handles");
    }
}

void arithmeticAndAbandonment(const std::wstring &root)
{
    struct Extent { JobMetricCounter offset; unsigned count; JobMetricCounter limit; bool result; } cases[] = {
        {0, 0, 0, true}, {0, 1, 0, false},
        {NativeMaximumBytes-1, 1, NativeMaximumBytes, true},
        {NativeMaximumBytes, 0, NativeMaximumBytes, true},
        {NativeMaximumBytes, 1, NativeMaximumBytes, false},
        {(std::numeric_limits<JobMetricCounter>::max)()-1, 4, (std::numeric_limits<JobMetricCounter>::max)(), false},
        {(std::numeric_limits<JobMetricCounter>::max)(), 0, (std::numeric_limits<JobMetricCounter>::max)(), true},
        {0, (std::numeric_limits<unsigned>::max)(), NativeMaximumBytes, true}
    };
    for (unsigned i = 0; i != sizeof(cases)/sizeof(cases[0]); ++i)
        require(native_receipt_files::heldExtentFits(cases[i].offset, cases[i].count, cases[i].limit) == cases[i].result,
            "actual shared extent predicate checks cap/subtraction/u64 overflow without ceiling allocation");
    for (unsigned output = 0; output != 2; ++output)
    {
        SourceFixture f(child(root, output ? L"abandon-output" : L"abandon-source"));
        if (!output) f.write();
        observed = Observations();
        {
            NativeReceiptTraceFiles owner;
            if (output)
            {
                require(owner.openOutput(ascii(f.bundle), ascii(f.tracePath), NativeMaximumBytes), "abandoned output admitted");
                require(NativeReceiptTraceFiles::append(&owner, f.trace.data(), 17), "abandoned output retains actual partial bytes");
            }
            else assertSourceOpen(owner, f);
            // Deliberately no successful finish call. Runtime must not publish.
        }
        require(observed.allClosed(), "destructor closes abandoned handles without being a success proof");
        const auto bytes = readBytes(f.tracePath);
        require(bytes.size() == (output ? size_t(17) : f.trace.size()) &&
            std::equal(bytes.begin(), bytes.end(), f.trace.begin()), "abandonment preserves partial/full original evidence");
        released(f.tracePath);
    }
}

void nonRegularInputs(const std::wstring &root)
{
    for (unsigned target = 0; target != 2; ++target)
    {
        SourceFixture f(child(root, target ? L"directory-trace" : L"directory-receipt"));
        if (target) { f.write(false); need(::CreateDirectoryW(f.tracePath.c_str(), 0), "real trace-as-directory fixture"); }
        else { need(::CreateDirectoryW(f.receiptPath.c_str(), 0), "real receipt-as-directory fixture"); newBytes(f.tracePath, f.trace); }
        observed = Observations();
        {
            NativeReceiptTraceFiles owner;
            require(!owner.openSource(ascii(f.receiptPath), f.expected, NativeMaximumBytes), "directory cannot be regular source content");
        }
        require(observed.allClosed(), "nonregular input cleanup");
    }
}

void reparseAndAlias(const std::wstring &root)
{
    // Separate required capability subgroup. Unavailable real symlink/8.3 setup
    // returns prerequisite status 2, never a skipped PASS or fabricated metadata.
    for (unsigned target = 0; target != 3; ++target)
    {
        wchar_t name[64]; swprintf_s(name, L"reparse-%u", target);
        const std::wstring base = child(root, name);
        SourceFixture f(child(base, L"real-bundle")); f.write();
        std::wstring selection, reparsePath;
        if (target == 0)
        {
            selection = base + L"\\receipt-link.json";
            reparsePath = selection;
            need(::CreateSymbolicLinkW(selection.c_str(), f.receiptPath.c_str(), 2) != FALSE,
                "real file symlink needs already-approved host capability; do not enable privileges/Developer Mode here");
        }
        if (target == 1)
        {
            const std::wstring alias = base + L"\\bundle-link";
            reparsePath = alias;
            need(::CreateSymbolicLinkW(alias.c_str(), f.bundle.c_str(), 3) != FALSE, "real directory symlink prerequisite");
            selection = alias + L"\\source-receipt.json";
        }
        if (target == 2)
        {
            // Trace leaf symlink with fully valid same-bundle receipt paths.
            SourceFixture selected(child(base, L"selected-bundle"));
            selected.write(false);
            need(::CreateSymbolicLinkW(selected.tracePath.c_str(), f.tracePath.c_str(), 2) != FALSE, "real trace symlink prerequisite");
            f.receiptPath = selected.receiptPath; f.expected = selected.expected;
            selection = selected.receiptPath;
            reparsePath = selected.tracePath;
        }
        observed = Observations();
        {
            NativeReceiptTraceFiles owner;
            require(!owner.openSource(ascii(selection), f.expected, NativeMaximumBytes), "actual reparse component/file refuses admission");
            const OpenEvent *event = observed.last(reparsePath);
            require(event && event->identityQueries > 0 && event->reads == 0 &&
                (event->flags & FILE_FLAG_OPEN_REPARSE_POINT) != 0,
                "actual reparse object is identified before content access, not masked by parsed provenance mismatch");
        }
        require(observed.allClosed(), "reparse rejection cleanup");
    }
    SourceFixture f(child(root, L"alias-long-component-name")); f.write();
    wchar_t shortName[MAX_PATH] = {};
    const DWORD count = ::GetShortPathNameW(f.receiptPath.c_str(), shortName, MAX_PATH);
    need(count > 0 && count < MAX_PATH && std::wstring(shortName) != f.receiptPath,
        "actual 8.3 alias must exist; GetShortPathName returning original name is not alias coverage");
    observed = Observations();
    {
        NativeReceiptTraceFiles owner;
        require(!owner.openSource(ascii(shortName), f.expected, NativeMaximumBytes), "real 8.3 alias cannot supply source authority");
        for (size_t i = 0; i != observed.opens.size(); ++i)
            require(observed.opens[i].reads == 0, "normalized alias rejection precedes receipt content and later provenance comparisons");
    }
    require(observed.allClosed(), "alias rejection cleanup");
}

typedef void (*Case)(const std::wstring &);
int runCase(const char *name, Case body, const std::wstring &root)
{
    try { body(root); std::printf("PASS %s\n", name); return 0; }
    catch (const Failure &failure)
    {
        std::fprintf(stderr, "%s %s: %s (Win32=%lu)\n", failure.setup ? "PREREQUISITE" : "FAIL",
            name, failure.what(), GetLastError());
        return failure.setup ? 2 : 1;
    }
}
} // namespace held_file_test

// Parent may route these into existing native fixture binaries. No game launch.
// Each invocation requires its own pre-created empty ASCII local-directory root.
int RunPerformanceReceiptTraceFileTests(const wchar_t *freshRoot, bool reparseAliasOnly)
{
    using namespace held_file_test;
    try
    {
        need(freshRoot != 0, "parent supplies fresh exact fixture root");
        const std::wstring root(freshRoot);
        need(root.size() >= 3 && root[1] == L':' && root[2] == L'\\' && root.size()+90 < MAX_PATH,
            "bounded local absolute fixture root, with space for concrete children");
        ascii(root);
        const DWORD attributes = ::GetFileAttributesW(root.c_str());
        need(attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0,
            "parent-created fixture root must exist");
        if (reparseAliasOnly) return runCase("real-reparse-and-alias", reparseAndAlias, root);
        struct Named { const char *name; Case body; } cases[] = {
			{"source-positive", sourcePositive}, {"output-positive", outputPositive},
			{"publication-close-failure-cleanup", publicationCloseFailureCleanup},
			{"baseline-output-destination", baselineOutputDestination},
            {"existing-output", existingOutput}, {"output-path-mutations", outputPathMutations},
            {"source-mutations", sourceMutations},
            {"receipt-size-and-parse", receiptSizeAndParse}, {"read-bounds-and-partial", readBoundsAndFailures},
            {"output-failures", outputFailures}, {"surviving-original-mappings", mappingCases},
            {"held-component-seams", componentSeams}, {"lexical-paths", lexicalPaths},
            {"hardlinks-and-same-identity", hardLinksAndIdentity}, {"source-close-failure", sourceCloseFailure},
            {"arithmetic-and-abandonment", arithmeticAndAbandonment}, {"nonregular-inputs", nonRegularInputs}
        };
        int result = 0;
        for (size_t i = 0; i != sizeof(cases)/sizeof(cases[0]); ++i)
        {
            const int actual = runCase(cases[i].name, cases[i].body, root);
            if (actual == 2) result = 2;
            else if (actual && result == 0) result = 1;
        }
        return result;
    }
    catch (const Failure &failure)
    {
        std::fprintf(stderr, "PREREQUISITE root: %s\n", failure.what()); return 2;
    }
}
