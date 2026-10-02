/*
** Source-connected request parsing prerequisite, not native trace activation.
** Calls the linked real BeginPerformanceReceipt and PerformanceReceiptRuntime.
** No source open, output publication, owner UPDATE, process, or worker startup.
*/
#include "Common/GameThreadOwnership.h"
#include "Common/PerformanceReceiptRuntime.h"
#include "NativeReceiptTestEnvironment.h"
#include <stdio.h>

namespace performance_receipt_mode_fixture
{
using namespace rts::performance;
unsigned failures = 0;
void Check(bool condition, const char *message)
{
	if (!condition) { fprintf(stderr, "FAIL: %s\n", message); ++failures; }
}
void Require(bool condition, const char *message)
{
	if (!condition) { fprintf(stderr, "PREREQUISITE: %s\n", message); exit(2); }
}
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
struct Inputs
{
	native_receipt_draft::Environment environment;
	std::wstring bundle;
	std::string replay;
	Inputs()
	{
		wchar_t cwd[MAX_PATH] = {};
		const DWORD length = GetCurrentDirectoryW(MAX_PATH, cwd);
		Require(length != 0 && length < MAX_PATH, "actual test directory is a bounded W path");
		wchar_t suffix[96] = {};
		swprintf_s(suffix, L"\\native-mode-no-publication-%lu", GetCurrentProcessId());
		bundle = std::wstring(cwd, length) + suffix;
		Require(bundle.size() + 32 < MAX_PATH &&
			native_receipt_draft::Ascii(bundle + L"\\not-opened.rep", replay),
			"mode fixture uses the supported bounded ASCII test lane");
		SetLastError(ERROR_SUCCESS);
		const DWORD attributes = GetFileAttributesW(bundle.c_str());
		const DWORD error = GetLastError();
		Require(attributes == INVALID_FILE_ATTRIBUTES &&
			(error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND),
			"exact prospective bundle is absent; no prior evidence can be overwritten");
		Require(native_receipt_draft::Configure(environment, bundle, L"replay"),
			"actual W environment setup succeeded");
	}
	void CheckNoPublication() const
	{
		SetLastError(ERROR_SUCCESS);
		const DWORD attributes = GetFileAttributesW(bundle.c_str());
		const DWORD error = GetLastError();
		Check(attributes == INVALID_FILE_ATTRIBUTES &&
			(error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND),
			"request parsing and rejected baseline admission cannot create evidence");
	}
};

void TestActualModeParsing()
{
	struct Case { const wchar_t *text; KernelPerformanceReferenceMode expected; };
	const Case cases[] = {
		{ 0, KERNEL_REFERENCE_THROUGHPUT_BINDING },
		{ L"throughput-binding", KERNEL_REFERENCE_THROUGHPUT_BINDING },
		{ L"serial-oracle", KERNEL_REFERENCE_SERIAL_ORACLE },
		{ L"phase-baseline-binding", KERNEL_REFERENCE_PHASE_BASELINE_BINDING }
	};
	for (unsigned i = 0; i != sizeof(cases) / sizeof(cases[0]); ++i)
	{
		Inputs input;
		Require(input.environment.put(L"RTS_PERFORMANCE_REFERENCE_MODE", cases[i].text),
			"sets the actual native mode environment");
		PerformanceReceipt receipt;
		std::string reason;
		const bool parsed = BeginPerformanceReceipt(receipt, Title(), input.replay.c_str(), 0, &reason);
		Check(parsed && receipt.kernelReference.mode == cases[i].expected,
			"real Begin recognizes the exact requested mode without treating baseline as throughput");
		if (parsed)
		{
			Check(!receipt.fixtureIdentityObserved && !receipt.seedKnown &&
				receipt.traceFiles.tracePath.empty() && receipt.traceFiles.sourceReceiptPath.empty() &&
				!receipt.kernelReference.trace.requested && !receipt.kernelReference.frozen,
				"parsing a mode cannot invent a source selection, fixture observation, or closed trace");
			if (cases[i].expected == KERNEL_REFERENCE_PHASE_BASELINE_BINDING)
				Check(receipt.schemaVersion == 6 &&
					receipt.producer == "game-executable-stage5-performance-report-v6" &&
					receipt.producerVersion == "6",
					"phase-baseline Begin promotes the production receipt wire schema to V6");
			else
				Check(receipt.producerVersion == "5", "ordinary Begin retains its existing V5 default");
		}
		input.CheckNoPublication();
	}
}

void TestUnsupportedTokens()
{
	const wchar_t *tokens[] = { L"Phase-baseline-binding", L"phase-baseline-binding ",
		L"phase-serial-baseline", L"disabled", L"consume" };
	for (unsigned i = 0; i != sizeof(tokens) / sizeof(tokens[0]); ++i)
	{
		Inputs input;
		Require(input.environment.put(L"RTS_PERFORMANCE_REFERENCE_MODE", tokens[i]),
			"sets unsupported mode token");
		PerformanceReceipt receipt;
		std::string reason;
		Check(!BeginPerformanceReceipt(receipt, Title(), input.replay.c_str(), 0, &reason) && !reason.empty(),
			"unsupported native mode cannot silently select ordinary throughput");
		input.CheckNoPublication();
	}
}

void TestMissingSourceNeverActivatesRuntime()
{
	// Permanent negative controls: no selection, receipt-only, and hash-only.
	// A token-parser fix without native Runtime admission would fail these.
	for (unsigned selection = 0; selection != 3; ++selection)
	{
		Inputs input;
		Require(input.environment.put(L"RTS_PERFORMANCE_REFERENCE_MODE", L"phase-baseline-binding"),
			"sets baseline request for actual Runtime");
		if (selection == 1)
			Require(input.environment.put(L"RTS_PERFORMANCE_SOURCE_RECEIPT_PATH",
				(input.bundle + L"\\not-opened.json").c_str()), "sets receipt-only selection");
		if (selection == 2)
			Require(input.environment.put(L"RTS_PERFORMANCE_SOURCE_RECEIPT_SHA256",
				L"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"), "sets hash-only selection");
		KernelPerformanceLedger &timing = KernelPerformanceLedger::instance();
		KernelPerformanceReferenceLedger &reference = KernelPerformanceReferenceLedger::instance();
		Require(timing.beginRun(false) && reference.beginRun(KERNEL_REFERENCE_SERIAL_ORACLE),
			"real independent ledger sentinel can be established");
		const KernelPerformanceSnapshot beforeTiming = timing.freeze();
		const KernelPerformanceReferenceSnapshot beforeReference = reference.freeze();
		::PerformanceReceiptRuntime runtime;
		Check(!runtime.begin("replay", input.replay.c_str()) && !runtime.active(),
			"missing or partial baseline source selection fails before native Runtime activation");
		const KernelPerformanceSnapshot afterTiming = timing.freeze();
		const KernelPerformanceReferenceSnapshot afterReference = reference.freeze();
		Check(afterTiming.generation == beforeTiming.generation &&
			afterTiming.runRole == beforeTiming.runRole &&
			afterReference.generation == beforeReference.generation &&
			afterReference.mode == beforeReference.mode,
			"rejected baseline cannot reset either preexisting diagnostic ledger");
		input.CheckNoPublication();
	}
}
} // namespace performance_receipt_mode_fixture

int RunPerformanceReceiptModeTitleTests()
{
	using namespace performance_receipt_mode_fixture;
	failures = 0;
	const bool attach = !GameThreadOwnership::IsAttached();
	if (attach) GameThreadOwnership::AttachCurrentThread();
	TestActualModeParsing();
	TestUnsupportedTokens();
	TestMissingSourceNeverActivatesRuntime();
	if (attach) GameThreadOwnership::DetachCurrentThread();
	if (failures != 0) return 1;
	printf("Native receipt request-mode prerequisite tests passed; no trace was activated.\n");
	return 0;
}
