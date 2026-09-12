/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2026 TheSuperHackers
** SPDX-License-Identifier: GPL-3.0-or-later
*/

#include "Common/Stage5PerformanceFixtureContract.h"
#include "Common/Stage5MapResolution.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <memory>
#include <vector>
#include <algorithm>

namespace {
unsigned failures = 0;
void Check(bool condition, const char *message)
{
	if (!condition) { fprintf(stderr, "FAIL: %s\n", message); ++failures; }
}

bool Parse(const char *const *args, int count, rts::fixture::Request &request,
	bool native = true)
{
	const char *error = 0;
	return rts::fixture::ParseCommandLine(count, args, native, &request, &error);
}

void TestOptInAndNormalization()
{
	rts::fixture::Request request;
	const char *ordinary[] = { "game", "-noFPSLimit" };
	Check(Parse(ordinary, 2, request) && !request.requested,
		"ordinary startup remains outside the fixture lane");
	const char *valid[] = { "game", "-headless", "-runStage5PerformanceFixture",
		"maps/Stage5 Eight/Stage5 Eight.map", "24101", "108000" };
	const bool accepted = Parse(valid, 6, request);
	Check(accepted, "explicit headless native fixture is accepted");
	if (accepted)
	{
		Check(request.requested && request.seed == 24101 && request.frameBudget == 108000,
			"fixture request retains the exact seed and frame budget");
		Check(strcmp(request.mapKey, "Maps\\Stage5 Eight\\Stage5 Eight.map") == 0,
			"relative map keys normalize separators and the Maps prefix");
	}
	Check(!Parse(valid, 6, request, false), "legacy binaries reject native fixture recording");
	const char *headlessLast[] = { "game", "-runStage5PerformanceFixture",
		"Maps\\Fixture\\Fixture.map", "1", "30", "-headless" };
	Check(Parse(headlessLast, 6, request), "headless recognition is independent of argument order");
	Check(!Parse(headlessLast, 5, request), "headless must be explicitly requested");
}

void TestUnsafeMapKeys()
{
	const char *invalid[] = { "", "C:\\Maps\\x\\x.map", "\\\\host\\Maps\\x.map",
		"\\Maps\\x\\x.map", "Maps\\..\\x.map", "Maps\\x\\..\\x.map",
		"Maps\\.\\x.map", "Maps\\x\\x.map:stream", "Maps\\x\\*.map",
		"Maps\\x\\x.rep", "Other\\x\\x.map", "Maps\\x\\x.map\\",
		"Maps\\x\\\\x.map", "Maps\\x.\\x.map", "Maps\\x \\x.map",
		"Maps\\x\\x\n.map", "Maps\\CON\\x.map" };
	for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i)
	{
		rts::fixture::Request request;
		const char *args[] = { "game", "-headless", "-runStage5PerformanceFixture", invalid[i], "1", "30" };
		Check(!Parse(args, 6, request), "unsafe or non-map path is rejected");
	}
	char tooLong[300];
	memset(tooLong, 'a', sizeof(tooLong));
	memcpy(tooLong, "Maps\\", 5);
	memcpy(tooLong + sizeof(tooLong) - 5, ".map", 5);
	rts::fixture::Request request;
	const char *args[] = { "game", "-headless", "-runStage5PerformanceFixture", tooLong, "1", "30" };
	Check(!Parse(args, 6, request), "overlong paths fail instead of truncating into a different map");
}

void TestNumericAndConflictingOptions()
{
	const char *invalidSeeds[] = { "0", "-1", "+1", " 1", "1x", "2147483648", "999999999999999999999" };
	for (unsigned i = 0; i < sizeof(invalidSeeds) / sizeof(invalidSeeds[0]); ++i)
	{
		rts::fixture::Request request;
		const char *args[] = { "game", "-headless", "-runStage5PerformanceFixture",
			"Maps\\Fixture\\Fixture.map", invalidSeeds[i], "30" };
		Check(!Parse(args, 6, request), "invalid or overflowing seed is rejected");
	}
	const char *invalidBudgets[] = { "0", "-1", "+30", "30x", "108001", "4294967296" };
	for (unsigned i = 0; i < sizeof(invalidBudgets) / sizeof(invalidBudgets[0]); ++i)
	{
		rts::fixture::Request request;
		const char *args[] = { "game", "-headless", "-runStage5PerformanceFixture",
			"Maps\\Fixture\\Fixture.map", "1", invalidBudgets[i] };
		Check(!Parse(args, 6, request), "invalid or unbounded frame budget is rejected");
	}
	const char *conflicts[] = { "-replay", "-jobs", "-installedNet3Validation",
		"-installedLockstepV2Validation", "-runSkirmishAITest", "-runSkirmishAITest4v2",
		"-runSkirmishAITestPractical1v7", "-runStage5PerformanceFixture", "-loadsave", "-benchmark", "-file" };
	for (unsigned i = 0; i < sizeof(conflicts) / sizeof(conflicts[0]); ++i)
	{
		rts::fixture::Request request;
		const char *before[] = { "game", conflicts[i], "1", "-headless",
			"-runStage5PerformanceFixture", "Maps\\Fixture\\Fixture.map", "1", "30" };
		const char *after[] = { "game", "-headless", "-runStage5PerformanceFixture",
			"Maps\\Fixture\\Fixture.map", "1", "30", conflicts[i], "1" };
		Check(!Parse(before, 8, request), "conflicting option before fixture is rejected");
		Check(!Parse(after, 8, request), "conflicting option after fixture is rejected");
	}
	rts::fixture::Request request;
	const char *hardBefore[] = { "game", "-runSkirmishAITestHardAI2v6", "1733",
		"-headless", "-runStage5PerformanceFixture", "Maps\\Fixture\\Fixture.map",
		"101", "900" };
	const char *hardAfter[] = { "game", "-headless", "-runStage5PerformanceFixture",
		"Maps\\Fixture\\Fixture.map", "101", "900",
		"-runSkirmishAITestHardAI2v6", "1733" };
	Check(!Parse(hardBefore, 8, request),
		"Hard-AI-2v6 option before fixture is rejected as conflicting");
	Check(!Parse(hardAfter, 8, request),
		"Hard-AI-2v6 option after fixture is rejected as conflicting");
	const char *missing[] = { "game", "-headless", "-runStage5PerformanceFixture", "Maps\\Fixture\\Fixture.map", "1" };
	Check(!Parse(missing, 5, request), "missing budget is rejected");
}

void TestFixedEightPlayerRoster()
{
	rts::fixture::Plan plan = {};
	Check(!rts::fixture::BuildPlan(-1, &plan), "random faction index cannot form a fixture plan");
	const bool built = rts::fixture::BuildPlan(7, &plan);
	Check(built, "installed America faction index produces an eight-player plan");
	if (!built) return;
	const int teams[8] = { 0, 0, 0, 0, 1, 1, 1, 1 };
	const char *owners[8] = { "teamplayer0", "teamSkirmishAmerica1", "teamSkirmishAmerica2",
		"teamSkirmishAmerica3", "teamSkirmishAmerica4", "teamSkirmishAmerica5",
		"teamSkirmishAmerica6", "teamSkirmishAmerica7" };
	rts::fixture::ObservedSlot observed[8] = {};
	for (int i = 0; i < 8; ++i)
	{
		Check(plan.slots[i].human == (i == 0) && plan.slots[i].factionIndex == 7 &&
			plan.slots[i].color == i && plan.slots[i].startPosition == i &&
			plan.slots[i].team == teams[i] && strcmp(plan.slots[i].mapOwner, owners[i]) == 0,
			"fixed slot plan preserves literal America start/team/map-owner identity");
		observed[i].human = i == 0;
		observed[i].brutalAi = i != 0;
		observed[i].playableSide = observed[i].ownerResolvesToPlayer = true;
		observed[i].factionIndex = observed[i].originalFactionIndex = 7;
		observed[i].color = observed[i].originalColor = i;
		observed[i].startPosition = observed[i].originalStartPosition = i;
		observed[i].team = teams[i];
		observed[i].factionName = "FactionAmerica";
	}
	Check(rts::fixture::IsExpectedRoster(plan, observed, 8), "actual matching non-observer roster is accepted");
	Check(!rts::fixture::IsExpectedRoster(plan, observed, 7), "seven players cannot be relabeled as eight");
	observed[0].observer = true;
	Check(!rts::fixture::IsExpectedRoster(plan, observed, 8), "observer controller is rejected");
	observed[0].observer = false;
	observed[7].ownerResolvesToPlayer = false;
	Check(!rts::fixture::IsExpectedRoster(plan, observed, 8), "unbound map owner is rejected");
	observed[7].ownerResolvesToPlayer = true;
	observed[4].factionName = "FactionChina";
	Check(!rts::fixture::IsExpectedRoster(plan, observed, 8), "actual non-America faction is rejected");
	observed[4].factionName = "FactionAmerica";
	observed[4].originalFactionIndex = -1;
	Check(!rts::fixture::IsExpectedRoster(plan, observed, 8), "random original faction is rejected even when resolved to America");
	observed[4].originalFactionIndex = 7;
	observed[4].originalStartPosition = -1;
	Check(!rts::fixture::IsExpectedRoster(plan, observed, 8), "random original start is rejected");
	observed[4].originalStartPosition = 4;
	observed[4].team = 0;
	Check(!rts::fixture::IsExpectedRoster(plan, observed, 8), "changed team split is rejected");
}

rts::fixture::Completion ValidCompletion()
{
	rts::fixture::Completion value = {};
	value.title = value.replayTitle = rts::fixture::ZeroHourTitle;
	value.frameBudget = 900;
	value.endFrame = 400;
	value.replayFrameCount = 401;
	value.winnerTeam = 1;
	value.replayEpoch = 3;
	value.naturalVictory = value.recorderClosed = value.finalCrcKnown = true;
	value.loadedMapVerified = value.rosterVerified = true;
	value.observedPlayers = 8;
	value.observedFrameSamples = 400;
	value.startTime = 100;
	value.endTime = 110;
	return value;
}

void TestNaturalCompletionAndBudget()
{
	Check(rts::fixture::EvaluateProgress(0, 29, 30) == rts::fixture::Running,
		"an unfinished game may run only below its budget");
	Check(rts::fixture::EvaluateProgress(0, 30, 30) == rts::fixture::TimedOut,
		"frame budget cannot manufacture completion");
	Check(rts::fixture::EvaluateProgress(30, 31, 30) == rts::fixture::Complete,
		"natural victory on the final budgeted frame can complete");
	Check(rts::fixture::EvaluateProgress(31, 32, 30) == rts::fixture::TimedOut,
		"late victory cannot bypass the requested budget");
	Check(rts::fixture::IsValidCompletion(ValidCompletion()), "closed natural epoch-three replay is accepted");
	rts::fixture::Completion value = ValidCompletion();
	value.naturalVictory = false;
	Check(!rts::fixture::IsValidCompletion(value), "direct stop is not a natural completion");
	value = ValidCompletion(); value.recorderClosed = false;
	Check(!rts::fixture::IsValidCompletion(value), "open recorder cannot produce an accepted fixture");
	value = ValidCompletion(); value.endFrame = 0;
	Check(!rts::fixture::IsValidCompletion(value), "missing victory frame is rejected");
	value = ValidCompletion(); value.replayFrameCount = 400;
	Check(!rts::fixture::IsValidCompletion(value), "truncated replay closure is rejected");
	value = ValidCompletion(); value.replayEpoch = 2;
	Check(!rts::fixture::IsValidCompletion(value), "legacy epoch cannot be relabeled as native epoch three");
	value = ValidCompletion(); value.quitEarly = true;
	Check(!rts::fixture::IsValidCompletion(value), "quit-early replay is rejected");
	value = ValidCompletion(); value.desync = true;
	Check(!rts::fixture::IsValidCompletion(value), "desynchronized replay is rejected");
	value = ValidCompletion(); value.endTime = 99;
	Check(!rts::fixture::IsValidCompletion(value), "invalid replay timestamps are rejected");
	value = ValidCompletion(); value.winnerTeam = -1;
	Check(!rts::fixture::IsValidCompletion(value), "missing winner is rejected");
	value = ValidCompletion(); value.finalCrcKnown = false;
	Check(!rts::fixture::IsValidCompletion(value), "unknown final state CRC is rejected");
	value = ValidCompletion(); value.loadedMapVerified = false;
	Check(!rts::fixture::IsValidCompletion(value), "unverified loaded map cannot yield a fixture");
	value = ValidCompletion(); value.rosterVerified = false;
	Check(!rts::fixture::IsValidCompletion(value), "requested roster is not a substitute for observed roster");
	value = ValidCompletion(); value.observedPlayers = 7;
	Check(!rts::fixture::IsValidCompletion(value), "actual seven-player workload is rejected");
	value = ValidCompletion(); value.observedFrameSamples = 0;
	Check(!rts::fixture::IsValidCompletion(value), "unobserved unit counts are rejected");
	value = ValidCompletion(); value.frameBudget = 399;
	Check(!rts::fixture::IsValidCompletion(value), "victory after the fixture budget is rejected");
	value = ValidCompletion();
	value.title = value.replayTitle = rts::fixture::GeneralsTitle;
	value.replayEpoch = 1;
	Check(rts::fixture::IsValidCompletion(value), "Generals preserves its native AI planning epoch one");
	value.replayEpoch = 3;
	Check(!rts::fixture::IsValidCompletion(value), "Zero Hour epoch cannot be assigned to Generals");
	value = ValidCompletion(); value.replayTitle = rts::fixture::GeneralsTitle;
	Check(!rts::fixture::IsValidCompletion(value), "wrong-title replay marker is rejected");
	value = ValidCompletion(); value.title = rts::fixture::UnknownTitle;
	Check(!rts::fixture::IsValidCompletion(value), "unbound title is rejected");
}
}

namespace map_resolution_test {
typedef int Int;
const bool TRUE = true, FALSE = false;
struct AsciiString {
    std::string value;
    AsciiString(const char *text = "") : value(text) {}
    const char *str() const { return value.c_str(); }
    int getLength() const { return static_cast<int>(value.size()); }
    bool isEmpty() const { return value.empty(); }
    bool startsWithNoCase(const AsciiString &prefix) const {
        if (value.size() < prefix.value.size()) return false;
        for (size_t i=0;i<prefix.value.size();++i)
            if (rts::fixture::LowerAscii(value[i]) != rts::fixture::LowerAscii(prefix.value[i])) return false;
        return true;
    }
    void concat(const AsciiString &text) { value += text.value; }
    void concat(char text) { value += text; }
    void toLower() { for(size_t i=0;i<value.size();++i) value[i]=rts::fixture::LowerAscii(value[i]); }
    const char *find(char c) const { return strchr(str(),c); }
    bool nextToken(AsciiString *out, const char *separators) {
        size_t first=value.find_first_not_of(separators);
        if(first==std::string::npos) { out->value.clear(); value.clear(); return false; }
        size_t end=value.find_first_of(separators,first);
        out->value=value.substr(first,end==std::string::npos?end:end-first);
        value=end==std::string::npos?"":value.substr(end); return true;
    }
    static AsciiString TheEmptyString;
};
AsciiString AsciiString::TheEmptyString;
struct MapMetaData { bool m_doesExist, m_isMultiplayer; unsigned m_numPlayers, m_CRC, m_filesize; };
struct MapCache {
    std::string profile, selected, lastLookup;
    bool indexed, filePresent, directoryPresent, officialPresent;
    MapMetaData metadata;
    MapCache() : profile("H:\\Producer\\Profile\\Maps"), selected(profile+"\\S5P1000v2\\S5P1000v2.map"), indexed(true), filePresent(true), directoryPresent(true), officialPresent(false) {
        metadata.m_doesExist=metadata.m_isMultiplayer=true; metadata.m_numPlayers=8; metadata.m_CRC=17; metadata.m_filesize=11;
    }
    AsciiString getMapDir() const { return "Maps"; }
    AsciiString getUserMapDir() const { return profile.c_str(); }
    const MapMetaData *findMap(const char *path) {
        lastLookup=path;
        return ((indexed && rts::fixture::SameMapPath(path,selected.c_str())) ||
            (officialPresent && rts::fixture::SameMapPath(path,"Maps\\S5P1000v2\\S5P1000v2.map"))) ? &metadata : 0;
    }
} mapCache;
MapCache *TheMapCache=&mapCache;
struct File {
    enum { READ=1, BINARY=2, STREAMING=4 };
    size_t offset;
    File() : offset(0) {}
    Int size() const { return 11; }
    Int read(void *out, Int count) {
        size_t amount=std::min(static_cast<size_t>(count),11-offset);
        memcpy(out,"fixture-map"+offset,amount);offset+=amount;return static_cast<Int>(amount);
    }
    void close() { delete this; }
};
struct FileSystem {
    std::string lastOpen;
    static bool isPathInDirectory(const AsciiString &path,const AsciiString &parent) { return path.startsWithNoCase(parent) && path.value.find("..") == std::string::npos; }
    bool doesFileExist(const char *path) const {
        return (mapCache.filePresent && rts::fixture::SameMapPath(path,mapCache.selected.c_str())) ||
            (mapCache.directoryPresent && rts::fixture::SameMapPath(path,mapCache.selected.substr(0,mapCache.selected.find_last_of('\\')).c_str()));
    }
    File *openFile(const char *path,int) {
        lastOpen=path;
        return (doesFileExist(path) || (mapCache.officialPresent && rts::fixture::SameMapPath(path,"Maps\\S5P1000v2\\S5P1000v2.map"))) ? new File : 0;
    }
} fileSystem;
FileSystem *TheFileSystem=&fileSystem;
struct GameState {
    AsciiString getSaveDirectory() const { return "H:\\Producer\\Profile\\Save\\"; }
    AsciiString getMapLeafName(const AsciiString &name) const { const char *last=strrchr(name.str(),'\\'); return last?last+1:name.str(); }
    AsciiString realMapPathToPortableMapPath(const AsciiString &) const;
    AsciiString portableMapPathToRealMapPath(const AsciiString &) const;
} gameState;
GameState *TheGameState=&gameState;
#define DEBUG_CRASH(x) ((void)0)
#define DEBUG_LOG(x) ((void)0)
#include "Stage5MapPathCodeUnderTest.inc"
#undef DEBUG_CRASH
#undef DEBUG_LOG
struct GameInfo {
    AsciiString map; unsigned crc,size; int seed;
    AsciiString getMap() const { return map; }
    void setMap(const char *path) { map=path; }
    void setMapCRC(unsigned value) { crc=value; }
    void setMapSize(unsigned value) { size=value; }
    void setSeed(int value) { seed=value; }
    void startGame(int) {}
} gameInfo;
GameInfo *TheSkirmishGameInfo=&gameInfo;
struct GlobalData { AsciiString m_mapName; } globalData;
GlobalData *TheWritableGlobalData=&globalData;
AsciiString EncodeRecorderMap(GameInfo *game) {
#define DEBUG_LOG(x) ((void)0)
#include "Stage5MapOptionsCodeUnderTest.inc"
#undef DEBUG_LOG
    return newMapName;
}
rts::fixture::Request s_request;
struct Runner { unsigned mapCrc,mapSize; char mapSha256[65]; rts::fixture::ResolvedMapIdentity mapIdentity; } s_runner;
const char *failure=0;
void FailFixture(const char *reason) { failure=reason; }
struct CloseFixtureFile { void operator()(File *file) const { if(file) file->close(); } };
struct CRC { void computeCRC(const void*,Int) {} unsigned get() const { return 17; } };
bool HashSkirmishAITestBytes(const void *bytes,size_t count,char sha[65]) {
    if(count!=11 || memcmp(bytes,"fixture-map",11)!=0) return false;
    strcpy(sha,"fixture-sha");return true;
}
#include "Stage5MapResolverUnderTest.inc"
#include "Stage5MapHashUnderTest.inc"
bool StartMapGate() {
#include "Stage5MapGateUnderTest.inc"
    return true;
}
void ApplyMapForLoad() {
#include "Stage5MapLoadUnderTest.inc"
}
void TestProfileMapGate() {
    strcpy(s_request.mapKey,"Maps\\S5P1000v2\\S5P1000v2.map");
    s_request.seed=24101;
    Check(StartMapGate(),"profile map discovered under its absolute cache key reaches the unchanged eight-start/content gate");
    Check(strcmp(s_request.mapKey,"Maps\\S5P1000v2\\S5P1000v2.map")==0,"resolving a profile map does not mutate its portable request key");
    ApplyMapForLoad();
    Check(rts::fixture::SameMapPath(mapCache.lastLookup.c_str(),fileSystem.lastOpen.c_str()) &&
        rts::fixture::SameMapPath(gameInfo.map.str(),fileSystem.lastOpen.c_str()) &&
        rts::fixture::SameMapPath(globalData.m_mapName.str(),fileSystem.lastOpen.c_str()),
        "cache, exact-byte hash, GameInfo and game-load path use one resolved identity");
    Check(gameInfo.crc==17 && gameInfo.size==11 && gameInfo.seed==24101,"resolution preserves exact CRC, byte size and seed");
    Check(rts::fixture::SameMapPath(EncodeRecorderMap(&gameInfo).str(),"userdata/maps/S5P1000v2"),
        "actual recorder map-options encoder stores a portable custom-map directory, not the producing profile");
    const AsciiString portable=gameState.realMapPathToPortableMapPath(gameInfo.map);
    mapCache.profile="H:\\Consumer\\Different Profile\\Maps";
    Check(rts::fixture::SameMapPath(gameState.portableMapPathToRealMapPath(portable).str(),
        "H:\\Consumer\\Different Profile\\Maps\\S5P1000v2\\S5P1000v2.map"),
        "actual portable replay map is rebound against the consuming profile");
    mapCache.profile="H:\\Producer\\Profile\\Maps";
    mapCache.indexed=false; mapCache.officialPresent=true;
    Check(!StartMapGate() && strcmp(failure,"eight_start_map_unavailable")==0,
        "staged profile file without metadata cannot fall back to an installed map");
    mapCache.filePresent=false;
    Check(!StartMapGate(),"staged directory with missing map cannot fall back to an installed map");
    mapCache.directoryPresent=false;
    Check(StartMapGate() && !s_runner.mapIdentity.profileMap,"original installed-map path remains supported when no staged profile candidate exists");
    mapCache.indexed=mapCache.filePresent=mapCache.directoryPresent=true;
    mapCache.metadata.m_numPlayers=7;
    Check(!StartMapGate(),"resolved profile map still requires eight-player metadata");
    mapCache.metadata.m_numPlayers=8; mapCache.metadata.m_CRC=18;
    Check(!StartMapGate() && strcmp(failure,"map_content_metadata_mismatch")==0,"resolved bytes must still match metadata CRC");
    mapCache.metadata.m_CRC=17; mapCache.metadata.m_filesize=12;
    Check(!StartMapGate(),"resolved bytes must still match metadata size");
    mapCache.metadata.m_filesize=11;
    mapCache.metadata.m_isMultiplayer=false;
    Check(!StartMapGate(),"resolved profile map still requires multiplayer metadata");
    mapCache.metadata.m_isMultiplayer=true; mapCache.metadata.m_doesExist=false;
    Check(!StartMapGate(),"resolved profile map still requires existing-map metadata");
    mapCache.metadata.m_doesExist=true;
    rts::fixture::ResolvedMapIdentity identity;
    Check(!ResolveStage5MapIdentity("Maps\\nested\\S5P1000v2\\S5P1000v2.map",&identity),"keys flattened by existing replay serialization are rejected");
    Check(!ResolveStage5MapIdentity("Maps\\S5P1000v2\\other.map",&identity),"directory/file mismatches cannot produce an unreplayable recording");
    mapCache.profile.assign(1100,'x');
    Check(!ResolveStage5MapIdentity(s_request.mapKey,&identity),"overlong physical profile paths fail without truncation");
    strcpy(identity.runtimePath,"stale"); TheGameState=0;
    Check(!ResolveStage5MapIdentity(s_request.mapKey,&identity) && !identity.runtimePath[0],"unavailable engine owner clears a stale map identity");
    TheGameState=&gameState;
}
}

int main()
{
	TestOptInAndNormalization();
	TestUnsafeMapKeys();
	TestNumericAndConflictingOptions();
	TestFixedEightPlayerRoster();
	TestNaturalCompletionAndBudget();
    map_resolution_test::TestProfileMapGate();
	if (failures != 0) { fprintf(stderr, "%u fixture contract checks failed\n", failures); return 1; }
	puts("PASS: stage-five performance fixture contracts");
	return 0;
}
