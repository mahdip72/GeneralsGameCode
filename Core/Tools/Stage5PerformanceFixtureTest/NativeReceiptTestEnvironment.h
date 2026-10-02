// Scratch test dependency only. Not a native parser, producer, or OS seam.
#pragma once
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <vector>

namespace native_receipt_draft
{
struct WideValue
{
	WideValue() : present(false) {}
	bool present;
	std::wstring text;
};
inline bool ReadWideValue(const wchar_t *name, WideValue &value)
{
	value = WideValue();
	SetLastError(ERROR_SUCCESS);
	const DWORD count = GetEnvironmentVariableW(name, 0, 0);
	if (count == 0)
	{
		const DWORD error = GetLastError();
		value.present = error == ERROR_SUCCESS;
		return value.present || error == ERROR_ENVVAR_NOT_FOUND;
	}
	std::vector<wchar_t> bytes(count);
	const DWORD copied = GetEnvironmentVariableW(name, &bytes[0], count);
	if (copied + 1 != count) return false;
	value.present = true;
	value.text.assign(&bytes[0], copied);
	return true;
}
class Environment
{
public:
	bool put(const wchar_t *name, const wchar_t *value)
	{
		bool saved = false;
		for (size_t i = 0; i != m_saved.size(); ++i)
			if (m_saved[i].name == name) saved = true;
		if (!saved)
		{
			Entry entry;
			entry.name = name;
			if (!ReadWideValue(name, entry.value)) return false;
			m_saved.push_back(entry);
		}
		return SetEnvironmentVariableW(name, value) != FALSE;
	}
	~Environment()
	{
		for (size_t i = m_saved.size(); i != 0; --i)
		{
			const Entry &entry = m_saved[i - 1];
			if (!SetEnvironmentVariableW(entry.name.c_str(),
				entry.value.present ? entry.value.text.c_str() : 0))
			{
				fprintf(stderr, "PREREQUISITE: failed to restore original W environment\n");
				exit(2);
			}
			WideValue actual;
			if (!ReadWideValue(entry.name.c_str(), actual) ||
				actual.present != entry.value.present || actual.text != entry.value.text)
			{
				fprintf(stderr, "PREREQUISITE: restored W environment did not read back\n");
				exit(2);
			}
		}
	}
private:
	struct Entry { std::wstring name; WideValue value; };
	std::vector<Entry> m_saved;
	Environment(const Environment &);
	Environment &operator=(const Environment &);
public:
	Environment() {}
};
inline bool Ascii(const std::wstring &wide, std::string &narrow)
{
	narrow.clear();
	for (size_t i = 0; i != wide.size(); ++i)
	{
		if (wide[i] == 0 || wide[i] > 127) return false;
		narrow.push_back(static_cast<char>(wide[i]));
	}
	return true;
}
inline bool Configure(Environment &environment, const std::wstring &bundle,
	const wchar_t *kind)
{
	const wchar_t *inputs[][2] = {
		{ L"RTS_PERFORMANCE_ROLE", L"performance-report" },
		{ L"RTS_PERFORMANCE_RUN_ID", L"native-draft-no-publication" },
		{ L"RTS_PERFORMANCE_RUN_NONCE", L"11111111-1111-4111-8111-111111111111" },
		{ L"RTS_PERFORMANCE_COHORT_NONCE", L"22222222-2222-4222-8222-222222222222" },
		{ L"RTS_PERFORMANCE_COHORT_CREATED_UTC", L"2026-01-01T00:00:00Z" },
		{ L"RTS_PERFORMANCE_SOURCE_COMMIT", L"0123456789abcdef0123456789abcdef01234567" },
		{ L"RTS_PERFORMANCE_ARTIFACT_SET_SHA256", L"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" },
		{ L"RTS_PERFORMANCE_RUNTIME_MANIFEST_SHA256", L"BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB" },
		{ L"RTS_PERFORMANCE_RUNTIME_CLOSURE_SHA256", L"CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC" },
		{ L"RTS_PERFORMANCE_FIXTURE_ID", L"native-draft-no-publication" },
		{ L"RTS_PERFORMANCE_VERIFIER_BOUNDARY", L"test-only-no-publication" },
		{ L"RTS_PERFORMANCE_REFERENCE_MODE", L"throughput-binding" },
		{ L"RTS_PERFORMANCE_WORKLOAD_QUALIFICATION", L"observed-only" },
		{ L"RTS_PERFORMANCE_FIXTURE_SHA256", 0 },
		{ L"RTS_PERFORMANCE_PLAYER_COUNT", 0 },
		{ L"RTS_PERFORMANCE_UNIT_COUNT", 0 },
		{ L"RTS_PERFORMANCE_SEED", 0 },
		{ L"RTS_FRAME_TIMING_DIR", 0 },
		{ L"RTS_PERFORMANCE_ATTEMPT_TRACE_PATH", 0 },
		{ L"RTS_PERFORMANCE_SOURCE_RECEIPT_PATH", 0 },
		{ L"RTS_PERFORMANCE_SOURCE_RECEIPT_SHA256", 0 }
	};
	for (unsigned i = 0; i != sizeof(inputs) / sizeof(inputs[0]); ++i)
		if (!environment.put(inputs[i][0], inputs[i][1])) return false;
	return environment.put(L"RTS_PERFORMANCE_FIXTURE_KIND", kind) &&
		environment.put(L"RTS_PERFORMANCE_RECEIPT_DIR", bundle.c_str()) &&
		environment.put(L"RTS_PERFORMANCE_RAW_LOG_PATH", (bundle + L"\\raw.log").c_str()) &&
		environment.put(L"RTS_PERFORMANCE_TIMING_PATH", (bundle + L"\\timing.csv").c_str());
}
} // namespace native_receipt_draft
