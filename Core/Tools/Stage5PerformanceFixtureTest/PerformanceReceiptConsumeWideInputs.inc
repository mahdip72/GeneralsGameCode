// Include after the existing W title fixture. Reuses its actual extracted
// helper, linked Begin/Runtime, original-W Environment, and no-file Inputs.
namespace performance_receipt_wide_path_fixture
{
const wchar_t consumeUpperSha[] =
	L"0123456789ABCDEF00112233445566778899AABBCCDDEEFF1020304050607080";
const wchar_t consumeLowerSha[] =
	L"0123456789abcdef00112233445566778899aabbccddeeff1020304050607080";
const unsigned char consumeExpectedSha[32] = {
	0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
	0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
	0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,
	0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80
};

void ConfigureConsumeWide(Inputs &input, const wchar_t *sha, std::string &sourceAscii)
{
	const std::wstring source = input.bundle + L"\\selected-source.json";
	Require(native_receipt_draft::Ascii(source, sourceAscii), "selected source input is literal ASCII");
	Require(input.environment.put(L"RTS_PERFORMANCE_ATTEMPT_TRACE_PATH", 0) &&
		input.environment.put(L"RTS_PERFORMANCE_REFERENCE_MODE", L"phase-baseline-binding") &&
		input.environment.put(L"RTS_PERFORMANCE_SOURCE_RECEIPT_PATH", source.c_str()) &&
		input.environment.put(L"RTS_PERFORMANCE_SOURCE_RECEIPT_SHA256", sha),
		"sets one original-W selected-source pair with no record key");
}

void SeedStaleConsumeSelection(PerformanceReceiptTraceFiles &paths, KernelPerformanceDigest &selected)
{
	paths.tracePath = "stale-trace";
	paths.sourceReceiptPath = "stale-source";
	paths.sourceRunId = "stale-run";
	paths.sourceRunNonce = "stale-nonce";
	paths.sourceProcessId = 123;
	paths.sourceProcessCreationTimeUtc100ns = 456;
	selected.valid = true;
	memset(selected.bytes, 0xa5, sizeof(selected.bytes));
}

void TestConsumeAsciiSelection()
{
	const wchar_t *const shaInputs[] = { consumeUpperSha, consumeLowerSha };
	for (unsigned index = 0; index != sizeof(shaInputs) / sizeof(shaInputs[0]); ++index)
	{
		Inputs input;
		std::string sourceAscii;
		ConfigureConsumeWide(input, shaInputs[index], sourceAscii);
		PerformanceReceipt receipt = input.Begin();
		Require(receipt.kernelReference.mode == KERNEL_REFERENCE_PHASE_BASELINE_BINDING,
			"actual Begin parses baseline before selected-source input tests");
		PerformanceReceiptTraceFiles paths;
		KernelPerformanceDigest selected;
		SeedStaleConsumeSelection(paths, selected);
		std::string reason;
		bool requested = false;
		Check(readNativeReceiptTracePathInputs(receipt, paths, selected, &reason, &requested),
			"actual original-W consume ASCII path and exact SHA pass input validation");
		Check(requested && reason.empty() && paths.tracePath.empty() &&
			paths.sourceReceiptPath == sourceAscii && selected.valid &&
			memcmp(selected.bytes, consumeExpectedSha, sizeof(consumeExpectedSha)) == 0,
			"uppercase and lowercase SHA decode to independently literal bytes and retain only the selected receipt path");
		Check(paths.sourceRunId.empty() && paths.sourceRunNonce.empty() &&
			paths.sourceProcessId == 0 && paths.sourceProcessCreationTimeUtc100ns == 0,
			"input strings do not invent a source run tuple or parsed trace path");
		RuntimeView runtime;
		Check(!runtime.begin("replay", input.replayA.c_str()) && !runtime.active() && runtime.traceRequested(),
			"consume input validation alone cannot activate Runtime before genuine native source binding");
		input.NoTrace();
	}
}

void TestConsumeMalformedSelection()
{
	const char *const mutations[] = { "empty-sha", "short-sha", "long-sha", "nonhex-sha", "padded-sha",
		"missing-sha", "missing-path", "empty-path", "record-key-conflict", "wrong-mode",
		"surrogate-sha", "retained-mode-mismatch", "both-selection-keys-missing" };
	for (unsigned mutation = 0; mutation != sizeof(mutations) / sizeof(mutations[0]); ++mutation)
	{
		Inputs input;
		std::string sourceAscii;
		ConfigureConsumeWide(input, consumeUpperSha, sourceAscii);
		std::wstring changed = consumeUpperSha;
		switch (mutation)
		{
		case 0: changed.clear(); break;
		case 1: changed.resize(63); break;
		case 2: changed += L"0"; break;
		case 3: changed[7] = L'G'; break;
		case 4: changed = L" " + changed; break;
		case 5:
			Require(input.environment.put(L"RTS_PERFORMANCE_SOURCE_RECEIPT_SHA256", 0), "removes selected SHA"); break;
		case 6:
			Require(input.environment.put(L"RTS_PERFORMANCE_SOURCE_RECEIPT_PATH", 0), "removes selected path"); break;
		case 7:
			Require(input.environment.put(L"RTS_PERFORMANCE_SOURCE_RECEIPT_PATH", L""), "sets present-empty selected path"); break;
		case 8:
			Require(input.environment.put(L"RTS_PERFORMANCE_ATTEMPT_TRACE_PATH", input.trace.c_str()), "sets conflicting record key"); break;
		case 9:
			Require(input.environment.put(L"RTS_PERFORMANCE_REFERENCE_MODE", L"throughput-binding"), "selects record mode with source-only keys"); break;
		case 10: changed[7] = static_cast<wchar_t>(0xd800); break;
		case 11: break;
		case 12:
			Require(input.environment.put(L"RTS_PERFORMANCE_SOURCE_RECEIPT_PATH", 0) &&
				input.environment.put(L"RTS_PERFORMANCE_SOURCE_RECEIPT_SHA256", 0), "removes both selected-source keys"); break;
		}
		if (mutation < 5 || mutation == 10)
			Require(input.environment.put(L"RTS_PERFORMANCE_SOURCE_RECEIPT_SHA256", changed.c_str()), "sets original-W malformed SHA input");
		PerformanceReceipt receipt = input.Begin();
		Require(receipt.kernelReference.mode == (mutation == 9 ? KERNEL_REFERENCE_THROUGHPUT_BINDING :
			KERNEL_REFERENCE_PHASE_BASELINE_BINDING), "malformed selection does not fail the real mode-parser prerequisite");
		if (mutation == 11) receipt.kernelReference.mode = KERNEL_REFERENCE_THROUGHPUT_BINDING;
		PerformanceReceiptTraceFiles paths;
		KernelPerformanceDigest selected;
		SeedStaleConsumeSelection(paths, selected);
		std::string reason;
		bool requested = false;
		const bool admitted = readNativeReceiptTracePathInputs(receipt, paths, selected, &reason, &requested);
		if (admitted || !requested || reason.empty() || !paths.tracePath.empty() || !paths.sourceReceiptPath.empty() ||
			!paths.sourceRunId.empty() || !paths.sourceRunNonce.empty() || paths.sourceProcessId != 0 ||
			paths.sourceProcessCreationTimeUtc100ns != 0 || selected.valid)
		{
			fprintf(stderr, "FAIL [%s]: malformed selected-source input must reject, retain request intent, and clear all stale selection (admitted=%d requested=%d reason=%s trace=%zu source=%zu run=%zu nonce=%zu pid=%u creation=%llu digest=%d)\n",
				mutations[mutation], admitted ? 1 : 0, requested ? 1 : 0,
				reason.c_str(), paths.tracePath.size(), paths.sourceReceiptPath.size(),
				paths.sourceRunId.size(), paths.sourceRunNonce.size(), paths.sourceProcessId,
				static_cast<unsigned long long>(paths.sourceProcessCreationTimeUtc100ns),
				selected.valid ? 1 : 0);
			++failures;
		}
		input.NoTrace();
	}
}
} // namespace performance_receipt_wide_path_fixture

int RunPerformanceReceiptConsumeWidePathTitleTests()
{
	using namespace performance_receipt_wide_path_fixture;
	failures = 0;
	const bool attached = !GameThreadOwnership::IsAttached();
	if (attached) GameThreadOwnership::AttachCurrentThread();
	Require(GameThreadOwnership::IsCurrentThread(), "consume input fixture uses the actual current owner");
	TestConsumeAsciiSelection();
	TestConsumeMalformedSelection();
	if (attached) GameThreadOwnership::DetachCurrentThread();
	if (failures != 0) return 1;
	printf("Selected-source original-W ASCII/SHA input title tests passed; no selected source opened.\n");
	return 0;
}
