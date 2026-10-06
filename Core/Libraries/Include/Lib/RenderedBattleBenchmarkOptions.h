#pragma once

#include "Lib/ValidationProfileRoot.h"
#include <stdio.h>
#include <string.h>

namespace rts { namespace rendered_battle {

enum BenchmarkProfile
{
	BENCHMARK_PROFILE_LEGACY_512 = 0,
	BENCHMARK_PROFILE_COMBINED_ARMS_256,
	BENCHMARK_PROFILE_COMBINED_ARMS_512,
	BENCHMARK_PROFILE_MECHANIZED_256,
	BENCHMARK_PROFILE_MECHANIZED_512,
	BENCHMARK_PROFILE_INFANTRY_LINE_256,
	BENCHMARK_PROFILE_INFANTRY_LINE_512
};

struct BenchmarkProfileContract
{
	const char *profileId;
	const char *rosterContract;
	const char *phaseContract;
	unsigned int unitsPerPlayer;
	unsigned int templateCounts[4];
};

inline const char *BenchmarkProfileSchema()
{
	return "ggc.r2.rendered-battle-profile.v1";
}

inline const char *BenchmarkPhaseContract()
{
	return "ggc.r2.rendered-battle.phase.normal30hz-150-450-1080p.v1";
}

inline bool IsExplicitBenchmarkProfile(BenchmarkProfile profile)
{
	return profile != BENCHMARK_PROFILE_LEGACY_512;
}

inline bool GetBenchmarkProfileContract(BenchmarkProfile profile, BenchmarkProfileContract *contract)
{
	if (!contract) return false;
	contract->phaseContract = BenchmarkPhaseContract();
	contract->unitsPerPlayer = 0;
	for (unsigned int i = 0; i < 4; ++i) contract->templateCounts[i] = 0;
	switch (profile)
	{
	case BENCHMARK_PROFILE_COMBINED_ARMS_256:
		contract->profileId = "combined_arms_256";
		contract->rosterContract = "ggc.r2.rendered-battle.roster.combined-arms-256.v1";
		contract->unitsPerPlayer = 32;
		contract->templateCounts[0] = 4; contract->templateCounts[1] = 4;
		contract->templateCounts[2] = 12; contract->templateCounts[3] = 12;
		return true;
	case BENCHMARK_PROFILE_COMBINED_ARMS_512:
		contract->profileId = "combined_arms_512";
		contract->rosterContract = "ggc.r2.rendered-battle.roster.combined-arms-512.v1";
		contract->unitsPerPlayer = 64;
		contract->templateCounts[0] = 8; contract->templateCounts[1] = 8;
		contract->templateCounts[2] = 24; contract->templateCounts[3] = 24;
		return true;
	case BENCHMARK_PROFILE_MECHANIZED_256:
		contract->profileId = "mechanized_256";
		contract->rosterContract = "ggc.r2.rendered-battle.roster.mechanized-256.v1";
		contract->unitsPerPlayer = 32;
		contract->templateCounts[0] = 16; contract->templateCounts[1] = 16;
		return true;
	case BENCHMARK_PROFILE_MECHANIZED_512:
		contract->profileId = "mechanized_512";
		contract->rosterContract = "ggc.r2.rendered-battle.roster.mechanized-512.v1";
		contract->unitsPerPlayer = 64;
		contract->templateCounts[0] = 32; contract->templateCounts[1] = 32;
		return true;
	case BENCHMARK_PROFILE_INFANTRY_LINE_256:
		contract->profileId = "infantry_line_256";
		contract->rosterContract = "ggc.r2.rendered-battle.roster.infantry-line-256.v1";
		contract->unitsPerPlayer = 32;
		contract->templateCounts[2] = 16; contract->templateCounts[3] = 16;
		return true;
	case BENCHMARK_PROFILE_INFANTRY_LINE_512:
		contract->profileId = "infantry_line_512";
		contract->rosterContract = "ggc.r2.rendered-battle.roster.infantry-line-512.v1";
		contract->unitsPerPlayer = 64;
		contract->templateCounts[2] = 32; contract->templateCounts[3] = 32;
		return true;
	default:
		contract->profileId = NULL;
		contract->rosterContract = NULL;
		return false;
	}
}

// Shared ordered type selection for the explicit benchmark profiles. Keep this
// beside the immutable roster counts so the fixture producer and its tests use
// the same selection rule. combined_arms_512 preserves the legacy creation
// order rather than grouping the two infantry templates.
inline int GetBenchmarkProfileUnitType(BenchmarkProfile profile, unsigned int unit)
{
	BenchmarkProfileContract contract;
	if (!GetBenchmarkProfileContract(profile, &contract) || unit >= contract.unitsPerPlayer)
		return -1;
	if (profile == BENCHMARK_PROFILE_COMBINED_ARMS_512)
		return unit >= 32 ? 2 + static_cast<int>(unit % 2) : static_cast<int>(unit % 4);
	unsigned int remaining = unit;
	for (int type = 0; type < 4; ++type)
	{
		if (remaining < contract.templateCounts[type]) return type;
		remaining -= contract.templateCounts[type];
	}
	return -1;
}

inline bool ReadBenchmarkProfile(const char *value, BenchmarkProfile *profile)
{
	if (!value || !profile) return false;
	if (strcmp(value, "combined_arms_256") == 0) *profile = BENCHMARK_PROFILE_COMBINED_ARMS_256;
	else if (strcmp(value, "combined_arms_512") == 0) *profile = BENCHMARK_PROFILE_COMBINED_ARMS_512;
	else if (strcmp(value, "mechanized_256") == 0) *profile = BENCHMARK_PROFILE_MECHANIZED_256;
	else if (strcmp(value, "mechanized_512") == 0) *profile = BENCHMARK_PROFILE_MECHANIZED_512;
	else if (strcmp(value, "infantry_line_256") == 0) *profile = BENCHMARK_PROFILE_INFANTRY_LINE_256;
	else if (strcmp(value, "infantry_line_512") == 0) *profile = BENCHMARK_PROFILE_INFANTRY_LINE_512;
	else return false;
	return true;
}

inline bool ReadBenchmarkProfileEnvironment(BenchmarkProfile *profile)
{
	if (!profile) return false;
	char value[64];
	SetLastError(ERROR_SUCCESS);
	const DWORD length = GetEnvironmentVariableA("RTS_RENDERED_BATTLE_PROFILE", value, sizeof(value));
	if (length == 0)
	{
		if (GetLastError() != ERROR_ENVVAR_NOT_FOUND) return false;
		*profile = BENCHMARK_PROFILE_LEGACY_512;
		return true;
	}
	if (length >= sizeof(value)) return false;
	value[length] = '\0';
	return ReadBenchmarkProfile(value, profile);
}

struct TestOptions
{
	bool backgroundStartup;
	bool visualCaptureOnly;
	bool visualSamples;
	bool benchmarkRequested;
	BenchmarkProfile benchmarkProfile;
	char profileRoot[MAX_PATH];
	TestOptions() : backgroundStartup(false), visualCaptureOnly(false),
		visualSamples(false), benchmarkRequested(false),
		benchmarkProfile(BENCHMARK_PROFILE_LEGACY_512) { profileRoot[0] = '\0'; }
};

// Shared by the startup parser, both title bootstraps, and the fixture. These
// options are initialized before any window exists and never reread mid-run.
inline TestOptions &ProcessTestOptions()
{
	static TestOptions options;
	return options;
}

inline bool ReadTestOptIn(const char *name, bool *enabled)
{
	char value[2];
	SetLastError(ERROR_SUCCESS);
	const DWORD length = GetEnvironmentVariableA(name, value, sizeof(value));
	*enabled = false;
	if (length == 0)
		return GetLastError() == ERROR_ENVVAR_NOT_FOUND;
	if (length != 1 || value[0] != '1') return false;
	*enabled = true;
	return true;
}

// Stronger admission applies only to the explicit test lanes. Check every
// existing directory component, since checking the leaf misses junction parents.
inline bool IsNonReparseDirectoryTree(const char *path)
{
	char resolved[MAX_PATH];
	DWORD length = GetFullPathNameA(path, sizeof(resolved), resolved, NULL);
	if (!length || length >= sizeof(resolved) || resolved[1] != ':') return false;
	for (DWORD i = 0; i < length; ++i) if (resolved[i] == '/') resolved[i] = '\\';
	while (length > 3 && resolved[length - 1] == '\\') resolved[--length] = '\0';
	for (DWORD i = 3; i <= length; ++i)
	{
		if (i != length && resolved[i] != '\\') continue;
		const char saved = resolved[i]; resolved[i] = '\0';
		const DWORD attributes = GetFileAttributesA(resolved);
		resolved[i] = saved;
		if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY) ||
			(attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
	}
	return true;
}

inline bool ValidateVisualSamplesGrammar(int argc, const char *const *argv)
{
	// Consume values as part of their option; a flag-shaped value cannot supply
	// a second interpretation of the command line. Unknown shell options reject.
	for (int i = 1; i < argc; ++i)
	{
		if (_stricmp(argv[i], "-win") == 0 || _stricmp(argv[i], "-nologo") == 0) continue;
		const char *expected = NULL;
		if (_stricmp(argv[i], "-xres") == 0) expected = "1920";
		else if (_stricmp(argv[i], "-yres") == 0) expected = "1080";
		else if (_stricmp(argv[i], "-renderer") == 0) expected = "d3d11";
		else if (_stricmp(argv[i], "-simulationMode") == 0) expected = "parallel";
		else if (_stricmp(argv[i], "-workerPolicy") == 0) expected = "auto";
		else if (_stricmp(argv[i], "-runRenderedBattleBenchmark") == 0)
		{
			if (++i >= argc || !argv[i][0]) return false;
			for (const char *digit = argv[i]; *digit; ++digit) if (*digit < '0' || *digit > '9') return false;
			continue;
		}
		else return false;
		if (++i >= argc || _stricmp(argv[i], expected) != 0) return false;
	}
	return true;
}

// The caller first applies the existing rendered-battle seed/conflict validator.
inline bool ConfigureTestOptions(int argc, const char *const *argv,
	bool supportedTitle, const char **reason, bool nativeSupported = false)
{
	static const volatile char backgroundCapability[] =
		"RTS_RENDERED_BATTLE_BACKGROUND_STARTUP_CAPABILITY_V1";
	static const volatile char visualCapability[] =
		"RTS_RENDERED_BATTLE_VISUAL_CAPTURE_CAPABILITY_V1";
	static const volatile char samplesCapability[] =
		"RTS_RENDER_VISUAL_CAPTURE_SAMPLES_CAPABILITY_V1";
	TestOptions requested;
	*reason = NULL;
	if (backgroundCapability[0] == '\0' || visualCapability[0] == '\0' || samplesCapability[0] == '\0' ||
		!ReadTestOptIn("RTS_RENDERED_BATTLE_BACKGROUND_STARTUP", &requested.backgroundStartup) ||
		!ReadTestOptIn("RTS_RENDERED_BATTLE_VISUAL_CAPTURE", &requested.visualCaptureOnly) ||
		!ReadTestOptIn("RTS_RENDER_VISUAL_CAPTURE_SAMPLES", &requested.visualSamples))
		*reason = "invalid_test_opt_in";
	int benchmarkOptions = 0;
	for (int i = 1; argv && i < argc; ++i)
		if (_stricmp(argv[i], "-runRenderedBattleBenchmark") == 0) ++benchmarkOptions;
	if (!*reason && benchmarkOptions == 1 &&
		!ReadBenchmarkProfileEnvironment(&requested.benchmarkProfile))
		*reason = "invalid_rendered_battle_profile";
	if (!*reason && (requested.backgroundStartup || requested.visualCaptureOnly || requested.visualSamples))
	{
		int benchmarks = 0, windows = 0, widths = 0, heights = 0, renderers = 0;
		for (int i = 1; argv && i < argc; ++i)
		{
			if (_stricmp(argv[i], "-runRenderedBattleBenchmark") == 0) ++benchmarks;
			if (_stricmp(argv[i], "-win") == 0) ++windows;
			if (_stricmp(argv[i], "-fullscreen") == 0) *reason = "test_requires_windowed";
			if (_stricmp(argv[i], "-headless") == 0) *reason = "test_conflicts_with_headless";
			if (_stricmp(argv[i], "-rendererCaptureFrame") == 0)
				*reason = "test_conflicts_with_renderer_capture";
			if (requested.visualSamples)
			{
				const char *conflicts[] = { "-headless", "-replay", "-jobs", "-noFPSLimit",
					"-mod", "-map", "-loadsave", "-benchmark", "-runRenderedBattleDiagnostic",
					"-runSkirmishAITest", "-runSkirmishAITest4v2", "-runSkirmishAITestPractical1v7",
					"-runSkirmishAITestHardAI2v6", "-runSkirmishAIRecoveryTest", "-runSkirmishAILegacySaveTest",
					"-runStage5PerformanceFixture", "-skirmishAITestReviewedMap", "-installedNet3Validation",
					"-installedLockstepV2Validation", "-noshaders", "-particleEdit" };
				for (unsigned j = 0; j < sizeof(conflicts) / sizeof(conflicts[0]); ++j)
					if (_stricmp(argv[i], conflicts[j]) == 0) *reason = "visual_samples_conflicting_option";
				if (_stricmp(argv[i], "-renderer") == 0)
				{
					++renderers;
					if (i + 1 >= argc || _stricmp(argv[i + 1], "d3d11") != 0)
						*reason = "visual_samples_require_native_renderer";
				}
			}
			if (_stricmp(argv[i], "-xres") == 0)
			{
				++widths;
				if (i + 1 >= argc || strcmp(argv[i + 1], "1920") != 0)
					*reason = "test_requires_1920x1080";
			}
			if (_stricmp(argv[i], "-yres") == 0)
			{
				++heights;
				if (i + 1 >= argc || strcmp(argv[i + 1], "1080") != 0)
					*reason = "test_requires_1920x1080";
			}
		}
		if (!supportedTitle) *reason = "unsupported_test_title";
		else if ((requested.backgroundStartup || requested.visualCaptureOnly) && benchmarks != 1)
			*reason = "test_requires_rendered_battle_benchmark";
		else if (requested.visualSamples && (!nativeSupported || renderers != 1 || benchmarks > 1))
			*reason = "visual_samples_require_native_menu_or_benchmark";
		else if (windows != 1 || widths != 1 || heights != 1)
			*reason = "test_requires_explicit_1920x1080_windowed";
		if (!*reason && requested.visualSamples && !ValidateVisualSamplesGrammar(argc, argv))
			*reason = "visual_samples_invalid_cli_grammar";
		if (!*reason && validation::ReadProcessLocalProfileRoot(requested.profileRoot, sizeof(requested.profileRoot)) !=
			validation::PROCESS_LOCAL_PROFILE_ROOT_VALID)
			*reason = "test_requires_valid_process_local_profile";
		if (!*reason && !IsNonReparseDirectoryTree(requested.profileRoot))
			*reason = "test_profile_reparse_ancestor";
		if (!*reason && requested.visualSamples && strlen(requested.profileRoot) + 80 >= MAX_PATH)
			*reason = "visual_samples_profile_path_too_long";
		requested.benchmarkRequested = benchmarks == 1;
		if (requested.visualSamples && requested.benchmarkRequested) requested.visualCaptureOnly = true;
	}
	if (*reason) return false;
	ProcessTestOptions() = requested;
	return true;
}

inline const char *VisualCaptureOnlyMarker()
{
	return "RENDERED_BATTLE_VISUAL_CAPTURE_ONLY numerical_eligible=0 wall_cap_ms=120000";
}

inline const char *BackgroundStartupMarker()
{
	// This records the requested startup policy, never observed Windows state.
	return "RENDERED_BATTLE_BACKGROUND_STARTUP_OPT_IN latched=1 observed_window_state=external";
}

inline const char *VisualSamplesMarker()
{
	return "RENDER_VISUAL_CAPTURE_SAMPLES_ONLY numerical_eligible=0";
}

struct VisualSampleState
{
	bool battleStaged, ready, complete, captureDisabled;
	DWORD readyTick, lastSampleTick;
	unsigned emitted, written, failed, currentSample, pendingSample;
	VisualSampleState() : battleStaged(false), ready(false), complete(false), captureDisabled(false),
		readyTick(0), lastSampleTick(0), emitted(0), written(0), failed(0), currentSample(0), pendingSample(0) {}
};
inline VisualSampleState &ProcessVisualSampleState()
{
	static VisualSampleState state;
	return state;
}

inline bool VisualSampleProfileMatches(const char *effectiveProfile)
{
	if (!effectiveProfile) return false;
	if (!IsNonReparseDirectoryTree(effectiveProfile) || !IsNonReparseDirectoryTree(ProcessTestOptions().profileRoot)) return false;
	char actual[MAX_PATH], expected[MAX_PATH];
	const DWORD actualSize = GetFullPathNameA(effectiveProfile, sizeof(actual), actual, NULL);
	const DWORD expectedSize = GetFullPathNameA(ProcessTestOptions().profileRoot,
		sizeof(expected), expected, NULL);
	if (!actualSize || actualSize >= sizeof(actual) || !expectedSize || expectedSize >= sizeof(expected)) return false;
	DWORD lengths[2] = { actualSize, expectedSize };
	char *paths[2] = { actual, expected };
	for (unsigned p = 0; p < 2; ++p)
	{
		for (DWORD i = 0; i < lengths[p]; ++i) if (paths[p][i] == '/') paths[p][i] = '\\';
		while (lengths[p] > 3 && paths[p][lengths[p] - 1] == '\\') paths[p][--lengths[p]] = '\0';
	}
	return _stricmp(actual, expected) == 0;
}

// A due request never catches up by issuing a burst after a slow frame.
inline bool VisualSampleDue(VisualSampleState &state, DWORD now, bool benchmark)
{
	const unsigned total = benchmark ? 20 : 40;
	if (state.complete || state.failed || state.pendingSample || state.emitted >= total ||
		(benchmark && !state.battleStaged)) return false;
	if (!state.ready) { state.ready = true; state.readyTick = now; }
	const DWORD delay = benchmark ? 0 : (state.emitted < 20 ? 15000 : 60000);
	if (now - state.readyTick < delay ||
		(state.emitted && now - state.lastSampleTick < 500)) return false;
	state.lastSampleTick = now;
	state.currentSample = ++state.emitted;
	state.pendingSample = state.currentSample;
	return true;
}

inline void VisualSampleTerminal(VisualSampleState &state, unsigned sample, bool written, unsigned expected)
{
	if (!sample || sample != state.pendingSample) written = false;
	if (sample == state.pendingSample) state.pendingSample = 0;
	if (written) ++state.written;
	else { ++state.failed; state.captureDisabled = true; }
	state.complete = state.written == expected && state.emitted == expected && !state.failed && !state.pendingSample;
}

inline bool VisualSamplesComplete(const VisualSampleState &state, unsigned expected)
{
	return state.complete && state.written == expected && state.emitted == expected && !state.failed && !state.pendingSample;
}

// Called only on the GAME thread for this explicitly instrumented visual mode.
inline bool WriteVisualSampleRecord(const char *record)
{
	const TestOptions &options = ProcessTestOptions();
	if (!options.visualSamples) return false;
	if (!IsNonReparseDirectoryTree(options.profileRoot)) return false;
	char path[MAX_PATH];
	if (strlen(options.profileRoot) + strlen("RenderVisualSamples.txt") >= sizeof(path)) return false;
	strcpy(path, options.profileRoot);
	strcat(path, "RenderVisualSamples.txt");
	const DWORD attributes = GetFileAttributesA(path);
	if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
	FILE *file = fopen(path, "a");
	if (!file) return false;
	const bool written = fputs(record, file) >= 0;
	const bool closed = fclose(file) == 0;
	return written && closed;
}

inline bool WriteVisualSampleReady(DWORD tick, unsigned frame)
{
	LARGE_INTEGER qpc, frequency;
	if (!QueryPerformanceCounter(&qpc) || !QueryPerformanceFrequency(&frequency)) return false;
	char record[256];
	_snprintf(record, sizeof(record), "RENDER_VISUAL_SAMPLES_READY ready_tick=%lu qpc=%I64d qpc_frequency=%I64d frame=%u\n",
		tick, qpc.QuadPart, frequency.QuadPart, frame);
	record[sizeof(record) - 1] = '\0';
	return WriteVisualSampleRecord(record);
}

inline bool FinalizeVisualSamples()
{
	VisualSampleState &state = ProcessVisualSampleState();
	const unsigned expected = ProcessTestOptions().benchmarkRequested ? 20 : 40;
	if (!VisualSamplesComplete(state, expected))
	{
		if (!state.failed) ++state.failed;
		state.captureDisabled = true;
		state.complete = false;
	}
	char record[256];
	_snprintf(record, sizeof(record), "RENDER_VISUAL_SAMPLES_TERMINAL status=%s expected=%u requested=%u written=%u failed=%u pending=%u\n",
		state.complete ? "COMPLETE" : "FAILED", expected, state.emitted, state.written, state.failed, state.pendingSample);
	record[sizeof(record) - 1] = '\0';
	if (!WriteVisualSampleRecord(record))
	{
		++state.failed;
		state.complete = false;
		state.captureDisabled = true;
	}
	return VisualSamplesComplete(state, expected);
}

} }
