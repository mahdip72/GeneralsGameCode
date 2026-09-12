#include "Lib/ReplayPathContract.h"

#include <stdio.h>
#include <string.h>

namespace
{

int Check(bool condition, const char *message)
{
	if (!condition)
	{
		fprintf(stderr, "FAIL: %s\n", message);
		return 1;
	}
	return 0;
}

int CheckResolved(const char *filename, bool nativeHeadless,
	const char *expected, const char *message)
{
	char resolved[260];
	const bool accepted = rts::replay::ResolveReplayPlaybackPath(
		"R:\\profile\\Replays\\", filename, nativeHeadless,
		resolved, sizeof(resolved));
	return Check(accepted && strcmp(resolved, expected) == 0, message);
}

int CheckRejected(const char *filename, const char *message)
{
	char resolved[260];
	strcpy(resolved, "must-not-open");
	const bool accepted = rts::replay::ResolveReplayPlaybackPath(
		"R:\\profile\\Replays\\", filename, true,
		resolved, sizeof(resolved));
	return Check(!accepted && resolved[0] == '\0', message);
}

int TestNativeConcreteAbsolutePaths()
{
	int result = CheckResolved("R:\\fixtures\\match.rep", true,
		"R:\\fixtures\\match.rep", "native drive-rooted replay is not prefixed");
	result |= CheckResolved("r:/fixtures/Mixed Case.rep", true,
		"r:/fixtures/Mixed Case.rep", "native local replay preserves exact path bytes");
	result |= CheckResolved("\\\\server\\share\\fixtures\\match.rep", true,
		"\\\\server\\share\\fixtures\\match.rep", "native UNC replay is not prefixed");
	result |= CheckResolved("//server/share/fixtures/match.rep", true,
		"//server/share/fixtures/match.rep", "native slash UNC replay preserves exact path bytes");
	return result;
}

int TestRelativeAndLegacyResolution()
{
	int result = CheckResolved("match.rep", true,
		"R:\\profile\\Replays\\match.rep", "native relative replay retains replay-directory lookup");
	result |= CheckResolved("fixtures/match.rep", true,
		"R:\\profile\\Replays\\fixtures/match.rep", "relative separators are not normalized");
	result |= CheckResolved("fixtures/*.rep", true,
		"R:\\profile\\Replays\\fixtures/*.rep", "existing relative wildcard discovery keeps its input");
	result |= CheckResolved("R:\\fixtures\\match.rep", false,
		"R:\\profile\\Replays\\R:\\fixtures\\match.rep", "disabled native gate retains legacy absolute-input behavior");
	result |= CheckResolved("\\\\server\\share\\match.rep", false,
		"R:\\profile\\Replays\\\\\\server\\share\\match.rep", "interactive and legacy UNC semantics remain unchanged");
	result |= CheckResolved("fixtures/../match.rep", false,
		"R:\\profile\\Replays\\fixtures/../match.rep", "legacy relative bytes are unchanged");
	return result;
}

int TestRejectedNativeRootedForms()
{
	const char *rejected[] = {
		"", "R:match.rep", "R:", "\\match.rep", "/match.rep",
		"\\\\server", "\\\\server\\share", "\\\\server\\share\\",
		"\\\\\\server\\share\\match.rep", "\\/server/share/match.rep",
		"\\\\server\\\\match.rep", "\\\\.\\pipe\\match.rep",
		"\\\\?\\R:\\fixtures\\match.rep", "//?/UNC/server/share/match.rep",
		"\\??\\R:\\fixtures\\match.rep", "R:\\", "1:\\match.rep",
		"R:\\fixtures\\*.rep", "R:\\fixtures\\match?.rep",
		"\\\\server\\share\\*.rep", "R:\\match.rep:stream",
		"R:\\fixtures\\bad|name.rep", "R:\\fixtures\\bad\"name.rep"
	};
	int result = 0;
	for (size_t i = 0; i < sizeof(rejected) / sizeof(rejected[0]); ++i)
		result |= CheckRejected(rejected[i], rejected[i]);
	return result;
}

int TestBoundedOutput()
{
	char resolved[8];
	int result = Check(rts::replay::ResolveReplayPlaybackPath(
		"D/", "a.rep", true, resolved, sizeof(resolved)) &&
		strcmp(resolved, "D/a.rep") == 0,
		"exact-fit relative path leaves room for its terminator");
	strcpy(resolved, "poison");
	result |= Check(!rts::replay::ResolveReplayPlaybackPath(
		"D/", "ab.rep", true, resolved, sizeof(resolved)) && resolved[0] == '\0',
		"overlength relative path fails without a truncated filename");
	result |= Check(rts::replay::ResolveReplayPlaybackPath(
		"ignored-directory-that-does-not-fit", "R:/a.rep", true,
		resolved, sizeof(resolved)) == false && resolved[0] == '\0',
		"overlength absolute path fails closed independently of the replay directory");
	char absolute[9];
	result |= Check(rts::replay::ResolveReplayPlaybackPath(
		"ignored-directory-that-does-not-fit", "R:/a.rep", true,
		absolute, sizeof(absolute)) && strcmp(absolute, "R:/a.rep") == 0,
		"exact-fit absolute path does not consume replay-directory capacity");
	result |= Check(!rts::replay::ResolveReplayPlaybackPath(
		"", "a.rep", true, 0, 4), "null output is rejected");
	char untouched = 'x';
	result |= Check(!rts::replay::ResolveReplayPlaybackPath(
		"", "a.rep", true, &untouched, 0) && untouched == 'x',
		"zero output capacity is rejected without writing");
	result |= Check(!rts::replay::ResolveReplayPlaybackPath(
		"", 0, true, resolved, sizeof(resolved)) && resolved[0] == '\0',
		"null input cannot reuse a stale resolved path");
	return result;
}

} // namespace

int main()
{
	return TestNativeConcreteAbsolutePaths() | TestRelativeAndLegacyResolution() |
		TestRejectedNativeRootedForms() | TestBoundedOutput();
}
