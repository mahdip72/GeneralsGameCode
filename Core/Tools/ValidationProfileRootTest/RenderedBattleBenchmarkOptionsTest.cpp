#include "Lib/RenderedBattleBenchmarkOptions.h"
#include <stdio.h>
#include <string.h>
#include <winioctl.h>
#include <wchar.h>

// Resolve the disposable root and validate its existing parent chain before
// creating a missing root leaf or allowing the fixture to write beneath it.
static bool PrepareFixtureRoot(const char *requestedRoot)
{
	char root[MAX_PATH], parent[MAX_PATH];
	DWORD length, attributes, error;
	char *separator;
	if (!requestedRoot || !requestedRoot[0]) return false;
	length = GetFullPathNameA(requestedRoot, MAX_PATH, root, NULL);
	if (!length || length >= MAX_PATH) return false;
	for (DWORD index = 0; index < length; ++index)
		if (root[index] == '/') root[index] = '\\';
	while (length > 3 && root[length - 1] == '\\') root[--length] = '\0';
	if (length <= 3 || root[1] != ':' || root[2] != '\\') return false;
	strcpy(parent, root);
	separator = strrchr(parent, '\\');
	if (!separator) return false;
	if (separator == parent + 2) parent[3] = '\0';
	else *separator = '\0';
	if (!rts::rendered_battle::IsNonReparseDirectoryTree(parent)) return false;
	attributes = GetFileAttributesA(root);
	if (attributes != INVALID_FILE_ATTRIBUTES)
		return (attributes & FILE_ATTRIBUTE_DIRECTORY) &&
			!(attributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
			rts::rendered_battle::IsNonReparseDirectoryTree(root);
	error = GetLastError();
	if (error != ERROR_FILE_NOT_FOUND) return false;
	if (!CreateDirectoryA(root, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) return false;
	attributes = GetFileAttributesA(root);
	return attributes != INVALID_FILE_ATTRIBUTES &&
		(attributes & FILE_ATTRIBUTE_DIRECTORY) &&
		!(attributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
		rts::rendered_battle::IsNonReparseDirectoryTree(root);
}

// Require the raw fixture root to be a canonical drive-absolute path before
// PrepareFixtureRoot is allowed to create a missing leaf.
static bool IsCanonicalFixtureRootArgument(const char *requestedRoot)
{
	char raw[MAX_PATH], resolved[MAX_PATH];
	size_t length, index;
	DWORD resolvedLength;
	char *component, *separator;
	if (!requestedRoot || !requestedRoot[0]) return false;
	length = strlen(requestedRoot);
	if (length >= MAX_PATH) return false;
	strcpy(raw, requestedRoot);
	for (index = 0; index < length; ++index)
		if (raw[index] == '/') raw[index] = '\\';
	if (length < 3 ||
		!((raw[0] >= 'A' && raw[0] <= 'Z') || (raw[0] >= 'a' && raw[0] <= 'z')) ||
		raw[1] != ':' || raw[2] != '\\') return false;
	for (index = 3; index < length; ++index)
		if (raw[index] == '\\' && raw[index - 1] == '\\') return false;
	if (length > 3 && raw[length - 1] == '\\') raw[--length] = '\0';
	component = raw + 3;
	while (*component)
	{
		size_t componentLength;
		separator = strchr(component, '\\');
		componentLength = separator ? static_cast<size_t>(separator - component) : strlen(component);
		if ((componentLength == 1 && component[0] == '.') ||
			(componentLength == 2 && component[0] == '.' && component[1] == '.')) return false;
		if (!separator) break;
		component = separator + 1;
	}
	resolvedLength = GetFullPathNameA(raw, MAX_PATH, resolved, NULL);
	if (!resolvedLength || resolvedLength >= MAX_PATH) return false;
	for (index = 0; index < resolvedLength; ++index)
		if (resolved[index] == '/') resolved[index] = '\\';
	while (resolvedLength > 3 && resolved[resolvedLength - 1] == '\\')
		resolved[--resolvedLength] = '\0';
	if (resolvedLength < 3 || resolved[1] != ':' || resolved[2] != '\\' ||
		_stricmp(raw, resolved) != 0) return false;
	return true;
}

// A fresh fixture path must be absent before the guarded helper creates it.
static bool CreateFreshFixtureDirectory(const char *path)
{
	DWORD attributes = GetFileAttributesA(path);
	if (attributes != INVALID_FILE_ATTRIBUTES || GetLastError() != ERROR_FILE_NOT_FOUND) return false;
	return PrepareFixtureRoot(path);
}

static bool CreateTestJunction(const char *alias, const char *target)
{
	struct MountPointData
	{
		DWORD tag;
		WORD dataLength, reserved, substituteOffset, substituteLength, printOffset, printLength;
		WCHAR paths[MAX_PATH * 2];
	} data;
	ZeroMemory(&data, sizeof(data));
	data.tag = IO_REPARSE_TAG_MOUNT_POINT;
	// CTest may supply forward slashes. The NT substitute name needs a
	// canonical absolute Windows path so the junction reaches the real leaf.
	char absoluteTarget[MAX_PATH];
	const DWORD targetLength = GetFullPathNameA(target, MAX_PATH, absoluteTarget, NULL);
	if (!targetLength || targetLength >= MAX_PATH) return false;
	for (DWORD index = 0; index < targetLength; ++index)
		if (absoluteTarget[index] == '/') absoluteTarget[index] = '\\';
	WCHAR wideTarget[MAX_PATH];
	if (!MultiByteToWideChar(CP_ACP, 0, absoluteTarget, -1, wideTarget, MAX_PATH)) return false;
	wcscpy(data.paths, L"\\??\\");
	wcscat(data.paths, wideTarget);
	data.substituteLength = static_cast<WORD>(wcslen(data.paths) * sizeof(WCHAR));
	data.printOffset = data.substituteLength + sizeof(WCHAR);
	wcscpy(reinterpret_cast<WCHAR *>(reinterpret_cast<char *>(data.paths) + data.printOffset), wideTarget);
	data.printLength = static_cast<WORD>(wcslen(wideTarget) * sizeof(WCHAR));
	data.dataLength = 8 + data.printOffset + data.printLength + sizeof(WCHAR);
	HANDLE directory = CreateFileA(alias, GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
		FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, NULL);
	if (directory == INVALID_HANDLE_VALUE) return false;
	DWORD returned = 0;
	const bool created = DeviceIoControl(directory, FSCTL_SET_REPARSE_POINT, &data,
		data.dataLength + 8, NULL, 0, &returned, NULL) != FALSE;
	CloseHandle(directory);
	return created;
}

// Run only against a disposable profile root. Environment changes belong to
// this test process, not the caller or live game.
int main(int argc, char **argv)
{
	char fixtureRoot[MAX_PATH];
	if (argc != 2 || strlen(argv[1]) + 80 >= MAX_PATH) return 2;
	if (!IsCanonicalFixtureRootArgument(argv[1]))
	{
		printf("FAIL fixture root must be canonical and drive-absolute\n");
		return 2;
	}
	if (!GetFullPathNameA(argv[1], MAX_PATH, fixtureRoot, NULL)) return 2;
	for (DWORD index = 0; fixtureRoot[index]; ++index)
		if (fixtureRoot[index] == '/') fixtureRoot[index] = '\\';
	while (strlen(fixtureRoot) > 3 && fixtureRoot[strlen(fixtureRoot) - 1] == '\\')
		fixtureRoot[strlen(fixtureRoot) - 1] = '\0';
	argv[1] = fixtureRoot;
	if (!PrepareFixtureRoot(argv[1]))
	{
		printf("FAIL invalid or reparse fixture root\n");
		return 2;
	}
	const char *request[] = { "test", "-runRenderedBattleBenchmark", "637808953",
		"-win", "-xres", "1920", "-yres", "1080", "-fullscreen" };
	const char *error = NULL;
	int failures = 0;
#define CHECK(condition) do { if (!(condition)) { ++failures; printf("FAIL line=%d\n", __LINE__); } } while (0)
	SetEnvironmentVariableA("RTS_RENDERED_BATTLE_BACKGROUND_STARTUP", NULL);
	SetEnvironmentVariableA("RTS_RENDERED_BATTLE_VISUAL_CAPTURE", NULL);
	SetEnvironmentVariableA("RTS_RENDER_VISUAL_CAPTURE_SAMPLES", NULL);
	SetEnvironmentVariableA("RTS_STAGE5_VALIDATION_PROFILE_ROOT", NULL);
	CHECK(rts::rendered_battle::ConfigureTestOptions(1, request, false, &error));
	CHECK(!rts::rendered_battle::ProcessTestOptions().backgroundStartup);
	CHECK(!rts::rendered_battle::ProcessTestOptions().visualCaptureOnly);
	// Default policy ignores a profile override unless a test opt-in requests it.
	SetEnvironmentVariableA("RTS_STAGE5_VALIDATION_PROFILE_ROOT", "relative-profile");
	CHECK(rts::rendered_battle::ConfigureTestOptions(1, request, false, &error));
	SetEnvironmentVariableA("RTS_STAGE5_VALIDATION_PROFILE_ROOT", NULL);
	SetEnvironmentVariableA("RTS_RENDERED_BATTLE_BACKGROUND_STARTUP", "1");
	CHECK(!rts::rendered_battle::ConfigureTestOptions(8, request, true, &error));
	SetEnvironmentVariableA("RTS_STAGE5_VALIDATION_PROFILE_ROOT", "relative-profile");
	CHECK(!rts::rendered_battle::ConfigureTestOptions(8, request, true, &error));
	SetEnvironmentVariableA("RTS_STAGE5_VALIDATION_PROFILE_ROOT", argv[1]);
	CHECK(rts::rendered_battle::ConfigureTestOptions(8, request, true, &error));
	CHECK(rts::rendered_battle::ProcessTestOptions().backgroundStartup);
	CHECK(!rts::rendered_battle::ConfigureTestOptions(8, request, false, &error));
	CHECK(!rts::rendered_battle::ConfigureTestOptions(1, request, true, &error));
	CHECK(!rts::rendered_battle::ConfigureTestOptions(9, request, true, &error));
	const char *duplicates[] = { "test", "-runRenderedBattleBenchmark", "637808953",
		"-win", "-xres", "1920", "-yres", "1080", "-xres", "1920" };
	CHECK(!rts::rendered_battle::ConfigureTestOptions(10, duplicates, true, &error));
	duplicates[8] = "-yres"; duplicates[9] = "1080";
	CHECK(!rts::rendered_battle::ConfigureTestOptions(10, duplicates, true, &error));
	duplicates[8] = "-runRenderedBattleBenchmark"; duplicates[9] = "637808953";
	CHECK(!rts::rendered_battle::ConfigureTestOptions(10, duplicates, true, &error));
	duplicates[8] = "-rendererCaptureFrame"; duplicates[9] = "1";
	CHECK(!rts::rendered_battle::ConfigureTestOptions(10, duplicates, true, &error));
	SetEnvironmentVariableA("RTS_RENDERED_BATTLE_BACKGROUND_STARTUP", "0");
	CHECK(!rts::rendered_battle::ConfigureTestOptions(8, request, true, &error));
	SetEnvironmentVariableA("RTS_RENDERED_BATTLE_BACKGROUND_STARTUP", "11");
	CHECK(!rts::rendered_battle::ConfigureTestOptions(8, request, true, &error));
	SetEnvironmentVariableA("RTS_RENDERED_BATTLE_BACKGROUND_STARTUP", NULL);
	SetEnvironmentVariableA("RTS_RENDERED_BATTLE_VISUAL_CAPTURE", "1");
	CHECK(rts::rendered_battle::ConfigureTestOptions(8, request, true, &error));
	CHECK(!rts::rendered_battle::ProcessTestOptions().backgroundStartup);
	CHECK(rts::rendered_battle::ProcessTestOptions().visualCaptureOnly);
	CHECK(!rts::rendered_battle::ConfigureTestOptions(10, duplicates, true, &error));
	SetEnvironmentVariableA("RTS_RENDERED_BATTLE_VISUAL_CAPTURE", "true");
	CHECK(!rts::rendered_battle::ConfigureTestOptions(8, request, true, &error));
	SetEnvironmentVariableA("RTS_RENDERED_BATTLE_VISUAL_CAPTURE", "1");
	SetEnvironmentVariableA("RTS_STAGE5_VALIDATION_PROFILE_ROOT", NULL);
	CHECK(!rts::rendered_battle::ConfigureTestOptions(8, request, true, &error));
	SetEnvironmentVariableA("RTS_STAGE5_VALIDATION_PROFILE_ROOT", argv[1]);
	CHECK(strcmp(rts::rendered_battle::VisualCaptureOnlyMarker(),
		"RENDERED_BATTLE_VISUAL_CAPTURE_ONLY numerical_eligible=0 wall_cap_ms=120000") == 0);
	request[5] = "1920junk";
	CHECK(!rts::rendered_battle::ConfigureTestOptions(8, request, true, &error));
	request[5] = "1920";
	request[1] = "-runRenderedBattleDiagnostic";
	CHECK(!rts::rendered_battle::ConfigureTestOptions(8, request, true, &error));
	SetEnvironmentVariableA("RTS_RENDERED_BATTLE_VISUAL_CAPTURE", NULL);
	SetEnvironmentVariableA("RTS_RENDER_VISUAL_CAPTURE_SAMPLES", "1");
	const char *menu[] = { "test", "-win", "-xres", "1920", "-yres", "1080", "-renderer", "d3d11",
		"-headless", "-runRenderedBattleBenchmark", "637808953" };
	CHECK(!rts::rendered_battle::ConfigureTestOptions(8, menu, true, &error));
	CHECK(rts::rendered_battle::ConfigureTestOptions(8, menu, true, &error, true));
	CHECK(rts::rendered_battle::ProcessTestOptions().visualSamples);
	CHECK(!rts::rendered_battle::ProcessTestOptions().visualCaptureOnly);
	CHECK(rts::rendered_battle::VisualSampleProfileMatches(argv[1]));
	CHECK(!rts::rendered_battle::ConfigureTestOptions(9, menu, true, &error, true));
	menu[8] = "-runRenderedBattleBenchmark"; menu[9] = "637808953";
	CHECK(rts::rendered_battle::ConfigureTestOptions(10, menu, true, &error, true));
	CHECK(rts::rendered_battle::ProcessTestOptions().visualCaptureOnly);
	CHECK(rts::rendered_battle::ProcessTestOptions().benchmarkRequested);
	menu[7] = "dx8";
	CHECK(!rts::rendered_battle::ConfigureTestOptions(8, menu, true, &error, true));
	menu[7] = "d3d11";
	SetEnvironmentVariableA("RTS_RENDER_VISUAL_CAPTURE_SAMPLES", "true");
	CHECK(!rts::rendered_battle::ConfigureTestOptions(8, menu, true, &error, true));
	SetEnvironmentVariableA("RTS_RENDER_VISUAL_CAPTURE_SAMPLES", "1");
	SetEnvironmentVariableA("RTS_STAGE5_VALIDATION_PROFILE_ROOT", NULL);
	CHECK(!rts::rendered_battle::ConfigureTestOptions(8, menu, true, &error, true));
	SetEnvironmentVariableA("RTS_STAGE5_VALIDATION_PROFILE_ROOT", argv[1]);
	// Effective game argv includes the approved launcher.lcf option pairs.
	const char *launched[] = { "generalszh.exe", "-simulationMode", "parallel", "-workerPolicy", "auto",
		"-win", "-nologo", "-renderer", "d3d11", "-xres", "1920", "-yres", "1080",
		"-runRenderedBattleBenchmark", "637808953" };
	CHECK(rts::rendered_battle::ConfigureTestOptions(13, launched, true, &error, true));
	CHECK(!rts::rendered_battle::ProcessTestOptions().benchmarkRequested);
	CHECK(rts::rendered_battle::ConfigureTestOptions(15, launched, true, &error, true));
	CHECK(rts::rendered_battle::ProcessTestOptions().visualCaptureOnly);
	launched[2] = "serial";
	CHECK(!rts::rendered_battle::ConfigureTestOptions(13, launched, true, &error, true));
	launched[2] = "-workerPolicy";
	CHECK(!rts::rendered_battle::ConfigureTestOptions(15, launched, true, &error, true));
	launched[2] = "parallel"; launched[4] = "fixed";
	CHECK(!rts::rendered_battle::ConfigureTestOptions(15, launched, true, &error, true));
	launched[4] = "-win";
	CHECK(!rts::rendered_battle::ConfigureTestOptions(13, launched, true, &error, true));
	launched[4] = "auto";
	const char *missingSimulation[] = { "generalszh.exe", "-simulationMode", "-workerPolicy", "auto",
		"-win", "-nologo", "-renderer", "d3d11", "-xres", "1920", "-yres", "1080" };
	CHECK(!rts::rendered_battle::ConfigureTestOptions(12, missingSimulation, true, &error, true));
	const char *missingWorker[] = { "generalszh.exe", "-simulationMode", "parallel", "-workerPolicy",
		"-win", "-nologo", "-renderer", "d3d11", "-xres", "1920", "-yres", "1080",
		"-runRenderedBattleBenchmark", "637808953" };
	CHECK(!rts::rendered_battle::ConfigureTestOptions(14, missingWorker, true, &error, true));
	const char *shellFlags[] = { "-shellmap", "-noshellmap", "-quickstart" };
	for (unsigned flag = 0; flag < 3; ++flag)
	{
		menu[8] = shellFlags[flag];
		CHECK(!rts::rendered_battle::ConfigureTestOptions(9, menu, true, &error, true));
	}
	menu[8] = "-runSkirmishAIAlliedTest"; menu[9] = "637808953";
	CHECK(!rts::rendered_battle::ConfigureTestOptions(10, menu, true, &error, true));
	menu[8] = "-nologo"; menu[9] = "-shellmap";
	CHECK(!rts::rendered_battle::ConfigureTestOptions(10, menu, true, &error, true));
	menu[3] = "-noshellmap";
	CHECK(!rts::rendered_battle::ConfigureTestOptions(8, menu, true, &error, true));
	menu[3] = "1920";
	SetEnvironmentVariableA("RTS_RENDER_VISUAL_CAPTURE_SAMPLES", NULL);
	request[1] = "-runRenderedBattleBenchmark"; request[8] = "-headless";
	SetEnvironmentVariableA("RTS_RENDERED_BATTLE_BACKGROUND_STARTUP", "1");
	CHECK(!rts::rendered_battle::ConfigureTestOptions(9, request, true, &error));
	SetEnvironmentVariableA("RTS_RENDERED_BATTLE_BACKGROUND_STARTUP", NULL);
	SetEnvironmentVariableA("RTS_RENDERED_BATTLE_VISUAL_CAPTURE", "1");
	CHECK(!rts::rendered_battle::ConfigureTestOptions(9, request, true, &error));
	SetEnvironmentVariableA("RTS_RENDERED_BATTLE_VISUAL_CAPTURE", NULL);
	rts::rendered_battle::VisualSampleState battle;
	CHECK(!rts::rendered_battle::VisualSampleDue(battle, 1000, true));
	battle.battleStaged = true;
	CHECK(rts::rendered_battle::VisualSampleDue(battle, 1000, true));
	CHECK(!battle.complete && battle.written == 0 && battle.pendingSample == 1);
	CHECK(!rts::rendered_battle::VisualSampleDue(battle, 50000, true));
	rts::rendered_battle::VisualSampleTerminal(battle, 1, true, 20);
	CHECK(!rts::rendered_battle::VisualSampleDue(battle, 1499, true));
	CHECK(rts::rendered_battle::VisualSampleDue(battle, 20000, true));
	rts::rendered_battle::VisualSampleTerminal(battle, 2, true, 20);
	CHECK(!rts::rendered_battle::VisualSampleDue(battle, 20001, true));
	for (unsigned i = 2; i < 20; ++i)
	{
		CHECK(rts::rendered_battle::VisualSampleDue(battle, 20000 + (i - 1) * 500, true));
		if (i == 19) CHECK(!battle.complete && battle.written == 19);
		rts::rendered_battle::VisualSampleTerminal(battle, i + 1, true, 20);
	}
	CHECK(rts::rendered_battle::VisualSamplesComplete(battle, 20));
	CHECK(!rts::rendered_battle::VisualSampleDue(battle, 40000, true));
	rts::rendered_battle::VisualSampleState shell;
	CHECK(!rts::rendered_battle::VisualSampleDue(shell, 1000, false));
	CHECK(!rts::rendered_battle::VisualSampleDue(shell, 15999, false));
	for (unsigned i = 0; i < 20; ++i)
	{
		CHECK(rts::rendered_battle::VisualSampleDue(shell, 16000 + i * 500, false));
		rts::rendered_battle::VisualSampleTerminal(shell, i + 1, true, 40);
	}
	CHECK(!rts::rendered_battle::VisualSampleDue(shell, 60999, false));
	for (unsigned i = 0; i < 20; ++i)
	{
		CHECK(rts::rendered_battle::VisualSampleDue(shell, 61000 + i * 500, false));
		rts::rendered_battle::VisualSampleTerminal(shell, i + 21, true, 40);
	}
	CHECK(rts::rendered_battle::VisualSamplesComplete(shell, 40));
	// Cancellation, allocation/encode/receipt failure share one sticky terminal
	// transition. A later success or duplicate completion cannot undo failure.
	rts::rendered_battle::VisualSampleState cancelled;
	cancelled.battleStaged = true;
	CHECK(rts::rendered_battle::VisualSampleDue(cancelled, 1000, true));
	rts::rendered_battle::VisualSampleTerminal(cancelled, 1, false, 20);
	CHECK(cancelled.failed == 1 && !cancelled.pendingSample && !cancelled.complete);
	CHECK(!rts::rendered_battle::VisualSampleDue(cancelled, 50000, true));
	rts::rendered_battle::VisualSampleTerminal(cancelled, 1, true, 20);
	CHECK(cancelled.failed && !rts::rendered_battle::VisualSamplesComplete(cancelled, 20));
	rts::rendered_battle::VisualSampleState rollover;
	rollover.battleStaged = true;
	CHECK(rts::rendered_battle::VisualSampleDue(rollover, 0xffffff00UL, true));
	rts::rendered_battle::VisualSampleTerminal(rollover, 1, true, 20);
	CHECK(!rts::rendered_battle::VisualSampleDue(rollover, 0x000000f3UL, true));
	CHECK(rts::rendered_battle::VisualSampleDue(rollover, 0x000000f4UL, true));
	CHECK(!rts::rendered_battle::VisualSamplesComplete(rollover, 20));
	// A normal leaf under a junction is accepted by the historical validator;
	// explicit diagnostic admission must additionally reject that ancestor.
	char target[MAX_PATH], leaf[MAX_PATH], alias[MAX_PATH], aliasedLeaf[MAX_PATH];
	char blockedRoot[MAX_PATH], redirectedSentinel[MAX_PATH];
	_snprintf(target, sizeof(target), "%s\\junction-target-%lu", argv[1], GetCurrentProcessId());
	_snprintf(leaf, sizeof(leaf), "%s\\profile", target);
	_snprintf(alias, sizeof(alias), "%s\\junction-alias-%lu", argv[1], GetCurrentProcessId());
	_snprintf(aliasedLeaf, sizeof(aliasedLeaf), "%s\\profile", alias);
	_snprintf(blockedRoot, sizeof(blockedRoot), "%s\\guarded-new-root-%lu",
		aliasedLeaf, GetCurrentProcessId());
	_snprintf(redirectedSentinel, sizeof(redirectedSentinel), "%s\\guarded-new-root-%lu",
		leaf, GetCurrentProcessId());
	if (!PrepareFixtureRoot(argv[1]))
	{
		++failures;
		printf("FAIL fixture root changed before filesystem setup\n");
		printf("RenderedBattleBenchmarkOptionsTest failures=%d\n", failures);
		return 1;
	}
	if (!CreateFreshFixtureDirectory(target))
	{
		++failures;
		printf("FAIL fresh fixture target creation\n");
		printf("RenderedBattleBenchmarkOptionsTest failures=%d\n", failures);
		return 1;
	}
	if (!CreateFreshFixtureDirectory(leaf))
	{
		++failures;
		printf("FAIL fresh fixture profile leaf creation\n");
		CHECK(RemoveDirectoryA(target) != FALSE);
		printf("RenderedBattleBenchmarkOptionsTest failures=%d\n", failures);
		return 1;
	}
	SetEnvironmentVariableA("RTS_STAGE5_VALIDATION_PROFILE_ROOT", argv[1]);
	SetEnvironmentVariableA("RTS_RENDER_VISUAL_CAPTURE_SAMPLES", "1");
	CHECK(rts::rendered_battle::ConfigureTestOptions(8, menu, true, &error, true));
	CHECK(rts::rendered_battle::IsNonReparseDirectoryTree(leaf));
	CHECK(!rts::rendered_battle::VisualSampleProfileMatches(leaf));
	CHECK(rts::rendered_battle::VisualSampleProfileMatches(argv[1]));
	if (!CreateFreshFixtureDirectory(alias))
	{
		++failures;
		printf("FAIL fresh fixture junction alias creation\n");
		CHECK(RemoveDirectoryA(leaf) != FALSE);
		CHECK(RemoveDirectoryA(target) != FALSE);
		printf("RenderedBattleBenchmarkOptionsTest failures=%d\n", failures);
		return 1;
	}
	const bool junctionCreated = CreateTestJunction(alias, target);
	CHECK(junctionCreated);
	if (junctionCreated)
	{
		CHECK(rts::validation::IsProcessLocalProfileRootPathValid(aliasedLeaf, static_cast<DWORD>(strlen(aliasedLeaf))));
		CHECK(!rts::rendered_battle::IsNonReparseDirectoryTree(aliasedLeaf));
		CHECK(GetFileAttributesA(redirectedSentinel) == INVALID_FILE_ATTRIBUTES &&
			GetLastError() == ERROR_FILE_NOT_FOUND);
		CHECK(!PrepareFixtureRoot(aliasedLeaf));
		CHECK(!PrepareFixtureRoot(blockedRoot));
		CHECK(!CreateFreshFixtureDirectory(aliasedLeaf));
		CHECK(!CreateFreshFixtureDirectory(blockedRoot));
		CHECK(GetFileAttributesA(redirectedSentinel) == INVALID_FILE_ATTRIBUTES &&
			GetLastError() == ERROR_FILE_NOT_FOUND);
		SetEnvironmentVariableA("RTS_STAGE5_VALIDATION_PROFILE_ROOT", aliasedLeaf);
		SetEnvironmentVariableA("RTS_RENDER_VISUAL_CAPTURE_SAMPLES", "1");
		CHECK(!rts::rendered_battle::ConfigureTestOptions(8, menu, true, &error, true));
	}
	CHECK(RemoveDirectoryA(alias) != FALSE);
	CHECK(RemoveDirectoryA(leaf) != FALSE);
	CHECK(RemoveDirectoryA(target) != FALSE);
	printf("RenderedBattleBenchmarkOptionsTest failures=%d\n", failures);
	return failures ? 1 : 0;
}
