/*
** Scratch draft. Original W inputs -> actual Begin -> native private admission
** and actual Runtime::begin. Never present a converted literal as OS evidence.
** This draft performs no file publication, owner UPDATE, or worker startup.
*/
#include "Common/GameThreadOwnership.h"
#include "Lib/PerformanceReceipt.h"
#include <atomic>
#define private protected
#include "Common/PerformanceReceiptRuntime.h"
#undef private
#include <stdio.h>
#include <string.h>
#include "NativeReceiptTestEnvironment.h"

namespace performance_receipt_wide_path_fixture
{
using namespace rts::performance;
unsigned failures = 0;
void Check(bool value, const char *message)
{
	if (!value) { fprintf(stderr, "FAIL: %s\n", message); ++failures; }
}
void Require(bool value, const char *message)
{
	if (!value) { fprintf(stderr, "PREREQUISITE: %s\n", message); exit(2); }
}
// Complete actual private input-reader group, with no test implementation or
// fake OS functions. Its ordinary fast-path must not inspect/restrict W paths.
#include "PerformanceReceiptNativeWideInputs.inc"

const char *Title()
{
#if defined(RTS_GENERALS)
	return "Generals";
#elif defined(RTS_ZEROHOUR)
	return "ZeroHour";
#else
	return "Unknown";
#endif
}
class RuntimeView : public ::PerformanceReceiptRuntime
{
public:
	const PerformanceReceipt &receipt() const { return m_receipt; }
	bool claimUntouchedLifecycle() { return m_lifecycle.begin(); }
};
struct Inputs
{
	std::wstring bundle, trace, replay;
	std::string bundleA, traceA, replayA;
	native_receipt_draft::Environment environment;
	Inputs()
	{
		wchar_t cwd[MAX_PATH] = {};
		DWORD size = GetCurrentDirectoryW(MAX_PATH, cwd);
		Require(size != 0 && size < MAX_PATH, "actual test working directory is a bounded W path");
		wchar_t suffix[96] = {};
		swprintf_s(suffix, L"\\native-wide-input-no-publication-%lu", GetCurrentProcessId());
		bundle = std::wstring(cwd, size) + suffix;
		trace = bundle + L"\\attempt-trace.bin";
		replay = bundle + L"\\held-replay-not-opened-at-begin.rep";
		Require(native_receipt_draft::Ascii(bundle, bundleA) &&
			native_receipt_draft::Ascii(trace, traceA) && native_receipt_draft::Ascii(replay, replayA),
			"this process runs under the explicitly supported ASCII qualification lane");
		Require(trace.size() < MAX_PATH && replay.size() < MAX_PATH, "test paths leave room inside MAX_PATH");
		Require(native_receipt_draft::Configure(environment, bundle, L"replay"), "actual W environment fixture is available");
		Require(environment.put(L"RTS_PERFORMANCE_ATTEMPT_TRACE_PATH", trace.c_str()), "sets original W trace key");
		SetLastError(ERROR_SUCCESS);
		const DWORD attributes = GetFileAttributesW(trace.c_str());
		const DWORD error = GetLastError();
		Require(attributes == INVALID_FILE_ATTRIBUTES && (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND),
			"this exact test trace does not pre-exist; never overwrite existing evidence");
	}
	PerformanceReceipt Begin()
	{
		PerformanceReceipt receipt;
		std::string reason;
		Require(BeginPerformanceReceipt(receipt, Title(), replayA.c_str(), 0, &reason),
			"actual existing native Begin parses otherwise valid process-local inputs");
		return receipt;
	}
	void NoTrace() const
	{
		const DWORD attributes = GetFileAttributesW(trace.c_str());
		Check(attributes == INVALID_FILE_ATTRIBUTES,
			"input admission cannot create/append a trace before held replay binding or UPDATE");
	}
};
void TestActualAsciiEntry()
{
	Inputs input;
	PerformanceReceipt receipt = input.Begin();
	PerformanceReceiptTraceFiles paths;
	KernelPerformanceDigest selected;
	std::string reason;
	Check(readNativeReceiptTracePathInputs(receipt, paths, selected, &reason),
		"actual original-W ASCII inputs pass private native admission");
	Check(receipt.outputDirectory == input.bundleA && paths.tracePath == input.traceA &&
		paths.sourceReceiptPath.empty() && !selected.valid,
		"ASCII environment survives Begin and retained native path selection byte-exactly");
	wchar_t executable[MAX_PATH] = {};
	const DWORD executableSize = GetModuleFileNameW(0, executable, MAX_PATH);
	std::string executableA, commandA;
	Require(executableSize != 0 && executableSize < MAX_PATH &&
		native_receipt_draft::Ascii(std::wstring(executable, executableSize), executableA) &&
		native_receipt_draft::Ascii(GetCommandLineW(), commandA),
		"actual test executable and command line fit the explicit ASCII lane");
	Check(receipt.executablePath == executableA && receipt.commandLine == commandA,
		"actual W process provenance agrees with the native Begin-captured bytes");
	Require(CreateDirectoryW(input.bundle.c_str(), 0) != FALSE,
		"ASCII runtime fixture exclusively creates its empty held destination");
	{
		RuntimeView runtime;
		// Admission arms the runtime and holds the existing destination, but
		// publication remains fenced until the held replay and terminal owner are
		// bound by the real pre-UPDATE path.
		Check(runtime.begin("replay", input.replayA.c_str()) && runtime.active() &&
			runtime.traceRequested(),
			"ASCII helper success activates a pending explicit Runtime before real pre-UPDATE binding");
		input.NoTrace();
		KernelPerformanceReferenceLedger::instance().freeze();
		KernelPerformanceLedger::instance().freeze();
	}
	Check(RemoveDirectoryW(input.bundle.c_str()) != FALSE,
		"checked Runtime destruction releases its empty held destination");
}
void TestRecordNonAsciiOriginalInputs()
{
	const wchar_t *keys[] = { L"RTS_PERFORMANCE_RECEIPT_DIR", L"RTS_PERFORMANCE_ATTEMPT_TRACE_PATH",
		L"RTS_PERFORMANCE_RAW_LOG_PATH", L"RTS_PERFORMANCE_TIMING_PATH", L"RTS_FRAME_TIMING_DIR" };
	for (unsigned i = 0; i != sizeof(keys)/sizeof(keys[0]); ++i)
	{
		Inputs input;
		const std::wstring actualWide = input.bundle + L"\\\u03A9-qualified-path";
		Require(input.environment.put(keys[i], actualWide.c_str()), "sets non-ASCII original W path without A conversion");
		PerformanceReceipt receipt = input.Begin();
		PerformanceReceiptTraceFiles paths;
		KernelPerformanceDigest selected;
		std::string reason;
		Check(!readNativeReceiptTracePathInputs(receipt, paths, selected, &reason) &&
			reason.find("ASCII") != std::string::npos && paths.tracePath.empty() &&
			paths.sourceReceiptPath.empty() && !selected.valid,
			"original W non-ASCII rejection is explicit and clears retained authority independently of ACP");
		RuntimeView runtime;
		Check(!runtime.begin("replay", input.replayA.c_str()) && !runtime.active(),
			"actual Runtime rejects original non-ASCII input before trace or owner execution");
		input.NoTrace();
		KernelPerformanceReferenceLedger::instance().freeze();
		KernelPerformanceLedger::instance().freeze();
	}
}
void TestMalformedOrChangedWideInput()
{
	for (unsigned mutation = 0; mutation != 5; ++mutation)
	{
		Inputs input;
		PerformanceReceipt receipt = input.Begin();
		if (mutation == 0)
		{
			const wchar_t malformed[] = { L'H', L':', L'\\', static_cast<wchar_t>(0xD800), 0 };
			Require(input.environment.put(L"RTS_PERFORMANCE_ATTEMPT_TRACE_PATH", malformed), "sets original malformed UTF-16 input");
		}
		if (mutation == 1)
		{
			const std::wstring oversized = L"H:\\" + std::wstring(MAX_PATH, L'x');
			Require(input.environment.put(L"RTS_PERFORMANCE_ATTEMPT_TRACE_PATH", oversized.c_str()), "sets original oversized input");
		}
		if (mutation == 2)
			Require(input.environment.put(L"RTS_PERFORMANCE_ATTEMPT_TRACE_PATH", L""), "present-empty key is not absent");
		if (mutation == 3) receipt.outputDirectory += "-not-the-original-W-value";
		if (mutation == 4) receipt.commandLine += " -not-the-original-W-command-line";
		PerformanceReceiptTraceFiles paths;
		paths.tracePath = "stale-authority";
		KernelPerformanceDigest selected; selected.valid = true;
		std::string reason;
		if (readNativeReceiptTracePathInputs(receipt, paths, selected, &reason) ||
			!paths.tracePath.empty() || !paths.sourceReceiptPath.empty() ||
			selected.valid || reason.empty())
		{
			fprintf(stderr, "FAIL [wide-mutation-%u]: malformed, oversized, empty or retained-A/W mismatch fails closed with no stale selection\n",
				mutation);
			++failures;
		}
		input.NoTrace();
	}
}
void TestConsumeOriginalSourcePath()
{
	// Promotion prerequisite: the actual native Begin mode parser must first
	// support phase-baseline-binding. Unknown-mode rejection is NOT this RED.
	Inputs input;
	Require(input.environment.put(L"RTS_PERFORMANCE_ATTEMPT_TRACE_PATH", 0) &&
		input.environment.put(L"RTS_PERFORMANCE_REFERENCE_MODE", L"phase-baseline-binding") &&
		input.environment.put(L"RTS_PERFORMANCE_SOURCE_RECEIPT_PATH", (input.bundle + L"\\\u03A9-source.json").c_str()) &&
		input.environment.put(L"RTS_PERFORMANCE_SOURCE_RECEIPT_SHA256", L"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"),
		"sets actual original W consume keys");
	PerformanceReceipt receipt = input.Begin();
	Require(receipt.kernelReference.mode == KERNEL_REFERENCE_PHASE_BASELINE_BINDING,
		"actual native baseline mode parser is GREEN before this source-path case");
	PerformanceReceiptTraceFiles paths;
	KernelPerformanceDigest selected;
	std::string reason;
	Check(!readNativeReceiptTracePathInputs(receipt, paths, selected, &reason) &&
		reason.find("ASCII") != std::string::npos && !selected.valid,
		"native consume rejects original W Unicode source selection rather than trusting A replacement bytes");
	RuntimeView runtime;
	Check(!runtime.begin("replay", input.replayA.c_str()) && !runtime.active(),
		"actual baseline Runtime rejects Unicode source before any file selection or fallback execution");
	input.NoTrace();
}
void TestOrdinaryInputsRemainUnrestricted()
{
	Inputs input;
	Require(input.environment.put(L"RTS_PERFORMANCE_ATTEMPT_TRACE_PATH", 0) &&
		input.environment.put(L"RTS_PERFORMANCE_RECEIPT_DIR", (input.bundle + L"\\\u03A9-ordinary").c_str()),
		"sets ordinary untraced original W input");
	PerformanceReceipt receipt = input.Begin();
	PerformanceReceiptTraceFiles paths;
	KernelPerformanceDigest selected;
	std::string reason;
	Check(readNativeReceiptTracePathInputs(receipt, paths, selected, &reason) &&
		paths.tracePath.empty() && paths.sourceReceiptPath.empty() && !selected.valid,
		"new qualification guard does not restrict ordinary V5/untraced game paths");
	RuntimeView runtime;
	Check(runtime.begin("replay", input.replayA.c_str()) && runtime.active(),
		"ordinary Runtime retains its existing Begin behavior without a trace request");
	input.NoTrace();
	KernelPerformanceReferenceLedger::instance().freeze();
	KernelPerformanceLedger::instance().freeze();
}

void TestSnapshotCaptureFailureFailsClosed()
{
	Inputs input;
	PerformanceReceipt receipt = input.Begin();
	delete g_nativeReceiptWideInputSnapshot;
	g_nativeReceiptWideInputSnapshot = 0;
	g_nativeReceiptWideInputSnapshotState = NATIVE_RECEIPT_WIDE_SNAPSHOT_CAPTURE_FAILED;
	PerformanceReceiptTraceFiles paths;
	KernelPerformanceDigest selected;
	std::string reason;
	bool explicitTraceRequested = false;
	Check(!readNativeReceiptTracePathInputs(receipt, paths, selected, &reason,
		&explicitTraceRequested) && explicitTraceRequested &&
		paths.tracePath.empty() && paths.sourceReceiptPath.empty() &&
		!selected.valid && reason.find("could not be captured") != std::string::npos,
		"an Original-W snapshot capture failure remains explicit and fails closed without stale authority");
	input.NoTrace();
}

void TestPresentReferenceModeIsExplicitIntent()
{
	const wchar_t *tokens[] = { L"", L"unsupported-mode", L"throughput-binding " };
	for (unsigned index = 0; index != sizeof(tokens) / sizeof(tokens[0]); ++index)
	{
		Inputs input;
		Require(input.environment.put(L"RTS_PERFORMANCE_ATTEMPT_TRACE_PATH", 0) &&
			input.environment.put(L"RTS_PERFORMANCE_REFERENCE_MODE", tokens[index]),
			"sets a present reference-mode value without a trace selection key");
		Check(PerformanceReceiptRuntime::explicitTraceRequestedFromEnvironment(),
			"every present reference-mode value is explicit receipt intent and cannot fail open into ordinary play");
		RuntimeView runtime;
		Check(!runtime.begin("replay", input.replayA.c_str()) && !runtime.active() &&
			runtime.claimUntouchedLifecycle(),
			"empty or unsupported reference mode fails closed before receipt or lifecycle side effects");
		input.NoTrace();
	}
}
} // namespace performance_receipt_wide_path_fixture
int RunPerformanceReceiptWidePathTitleTests()
{
	using namespace performance_receipt_wide_path_fixture;
	failures = 0;
	const bool attached = !GameThreadOwnership::IsAttached();
	if (attached) GameThreadOwnership::AttachCurrentThread();
	TestActualAsciiEntry();
	TestRecordNonAsciiOriginalInputs();
	TestMalformedOrChangedWideInput();
	TestConsumeOriginalSourcePath();
	TestOrdinaryInputsRemainUnrestricted();
	TestSnapshotCaptureFailureFailsClosed();
	TestPresentReferenceModeIsExplicitIntent();
	if (attached) GameThreadOwnership::DetachCurrentThread();
	if (failures) return 1;
	printf("Native original-W input source-connected title tests passed.\n");
	return 0;
}

#include "PerformanceReceiptConsumeWideInputs.inc"
