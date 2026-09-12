// Test inputs only. No native parser, dispatcher, or receipt authority.
#pragma once
#include "NativeReceiptTestEnvironment.h"
#include "Lib/PerformanceReceipt.h"

namespace native_receipt_caller_fence
{
inline void Require(bool value, const char *message)
{
	if (!value) { fprintf(stderr, "PREREQUISITE: %s\n", message); exit(2); }
}
struct Case
{
	const char *name;
	bool beginParses, explicitTrace, baseline, freshOnly;
};
static const Case cases[] = {
	{ "baseline-no-selection", true, true, true, false },
	{ "baseline-receipt-only", true, true, true, false },
	{ "baseline-hash-only", true, true, true, false },
	{ "baseline-conflicting-keys", true, true, true, false },
	{ "record-present-empty-output", true, true, false, false },
	{ "record-malformed-W-output", true, true, false, false },
	{ "record-missing-role", false, true, false, false },
	{ "baseline-missing-role", false, true, true, false },
	{ "record-malformed-nonce", false, true, false, false },
	{ "baseline-malformed-nonce", false, true, true, false },
	{ "throughput-with-source-key", true, true, false, false },
	{ "ordinary-missing-role", false, false, false, false },
	{ "ordinary-valid", true, false, false, false },
	{ "record-malformed-mode", false, true, false, false },
	{ "fresh-valid-record-request", true, true, false, true },
	{ "fresh-valid-baseline-request", true, true, true, true }
};
inline const char *Title()
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
	std::wstring bundle, trace, sourceReceipt;
	Inputs(unsigned scenario, const wchar_t *kind)
	{
		Require(scenario < sizeof(cases) / sizeof(cases[0]), "caller case is bounded");
		wchar_t cwd[MAX_PATH] = {}, suffix[96] = {};
		const DWORD length = GetCurrentDirectoryW(MAX_PATH, cwd);
		Require(length != 0 && length < MAX_PATH, "caller fixture has a bounded real W working directory");
		swprintf_s(suffix, L"\\native-caller-no-publication-%lu", GetCurrentProcessId());
		bundle = std::wstring(cwd, length) + suffix;
		trace = bundle + L"\\attempt-trace.bin";
		sourceReceipt = bundle + L"\\selected-source.json";
		std::string ascii;
		Require(sourceReceipt.size() < MAX_PATH && native_receipt_draft::Ascii(sourceReceipt, ascii),
			"caller fixture uses supported ASCII paths without creating a bundle");
		Require(bundleAbsent(), "exact prospective caller bundle is absent before the case");
		Require(native_receipt_draft::Configure(environment, bundle, kind), "sets actual process-local native inputs");
		if (cases[scenario].baseline)
			put(L"RTS_PERFORMANCE_REFERENCE_MODE", L"phase-baseline-binding");
		if (scenario == 1 || scenario == 3 || scenario == 7 || scenario == 9 || scenario == 15)
			put(L"RTS_PERFORMANCE_SOURCE_RECEIPT_PATH", sourceReceipt.c_str());
		if (scenario == 2 || scenario == 3 || scenario == 7 || scenario == 9 || scenario == 15)
			put(L"RTS_PERFORMANCE_SOURCE_RECEIPT_SHA256",
				L"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA");
		if (scenario == 3 || scenario == 6 || scenario == 8 || scenario == 13 || scenario == 14)
			put(L"RTS_PERFORMANCE_ATTEMPT_TRACE_PATH", trace.c_str());
		if (scenario == 4)
		{
			put(L"RTS_PERFORMANCE_ATTEMPT_TRACE_PATH", L"");
			native_receipt_draft::WideValue actual;
			Require(native_receipt_draft::ReadWideValue(L"RTS_PERFORMANCE_ATTEMPT_TRACE_PATH", actual) &&
				actual.present && actual.text.empty(), "empty trace key remains present in the actual W environment");
		}
		if (scenario == 5)
		{
			const wchar_t malformed[] = { L'H', L':', L'\\', static_cast<wchar_t>(0xD800), 0 };
			put(L"RTS_PERFORMANCE_ATTEMPT_TRACE_PATH", malformed);
		}
		if (scenario == 6 || scenario == 7 || scenario == 11) put(L"RTS_PERFORMANCE_ROLE", 0);
		if (scenario == 8 || scenario == 9) put(L"RTS_PERFORMANCE_RUN_NONCE", L"malformed-native-nonce");
		if (scenario == 10) put(L"RTS_PERFORMANCE_SOURCE_RECEIPT_PATH", sourceReceipt.c_str());
		if (scenario == 13) put(L"RTS_PERFORMANCE_REFERENCE_MODE", L"not-a-native-mode");
	}
	void put(const wchar_t *key, const wchar_t *value)
	{
		Require(environment.put(key, value), "sets and retains original W environment state");
	}
	bool bundleAbsent() const
	{
		SetLastError(ERROR_SUCCESS);
		const DWORD attributes = GetFileAttributesW(bundle.c_str());
		const DWORD error = GetLastError();
		return attributes == INVALID_FILE_ATTRIBUTES &&
			(error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND);
	}
};
inline void RequireBaselineParser()
{
	Inputs input(0, L"replay");
	rts::performance::PerformanceReceipt receipt;
	Require(rts::performance::BeginPerformanceReceipt(receipt, Title(), "not-opened.rep", 0) &&
		receipt.kernelReference.mode == rts::performance::KERNEL_REFERENCE_PHASE_BASELINE_BINDING,
		"actual baseline token parser must be GREEN before caller-fence RED");
}
} // namespace native_receipt_caller_fence
