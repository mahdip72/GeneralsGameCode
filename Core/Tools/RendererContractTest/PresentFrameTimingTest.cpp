// Recording-driver proof of the production Present clock/capture path, not FPS.
#include "../../Libraries/Source/Renderer/PresentFrameTiming.h"
#include <vector>
#include <string>
#include <thread>

namespace
{
using namespace rts::render::detail;
int Check(bool condition, const char *message)
{
	if (!condition) fprintf(stderr, "FAIL: %s\n", message);
	return condition ? 0 : 1;
}
struct RecordingQuery { explicit RecordingQuery(bool disjoint) : disjoint(disjoint), value(0) {} bool disjoint; uint64_t value; };
struct State
{
	State() : clocks(0), frequencies(0), creates(0), releases(0), reads(0), tick(100), stamp(100),
		ready(false), failCreate(false) {}
	~State() { for (size_t index = 0; index < queries.size(); ++index) delete queries[index]; }
	unsigned int clocks, frequencies, creates, releases, reads;
	uint64_t tick, stamp;
	bool ready, failCreate;
	std::vector<RecordingQuery *> queries;
};
struct Driver
{
	typedef RecordingQuery Query;
	Driver(State *state = 0) : state(state) {}
	uint64_t clock() { ++state->clocks; return state->tick += 50; }
	uint64_t cpuFrequency() { ++state->frequencies; return 1000; }
	uint32_t ownerTick() { return 123; }
	bool create(bool disjoint, Query **query)
	{
		++state->creates;
		if (state->failCreate) { *query = 0; return false; }
		*query = new Query(disjoint); state->queries.push_back(*query); return true;
	}
	void release(Query *) { ++state->releases; }
	void begin(Query *) {}
	void end(Query *query) { query->value = state->stamp; state->stamp += 100; }
	GpuTimingRead readDisjoint(Query *, uint64_t& frequency, bool& disjoint)
	{ ++state->reads; frequency = 1000; disjoint = false; return state->ready ? GpuTimingReady : GpuTimingNotReady; }
	GpuTimingRead readTimestamp(Query *query, uint64_t& value)
	{ ++state->reads; value = query->value; return state->ready ? GpuTimingReady : GpuTimingNotReady; }
	GpuTimingDeviceInfo deviceInfo() { GpuTimingDeviceInfo info; info.identityValid = true; return info; }
	State *state;
};
typedef GpuTimingCapture<Driver> Gpu;
typedef PresentTimingCapture<Driver> Present;
void ActualPresent(Gpu& gpu, Present& present, long result = 0)
{
	GpuTimingFrameInfo info;
	gpu.beforeResolve(info); gpu.afterResolve(); gpu.beforePresent();
	const uint64_t start = BeginPresentTiming(gpu, present);
	EndPresentTiming(gpu, present, start, result, 0, 0, 1920, 1080);
}
int ClockModesAndOutcomes()
{
	int result = 0;
	for (unsigned int mode = 0; mode < 4; ++mode)
	{
		State state; Gpu gpu; Present present;
		if (mode & 1) { gpu.enable(); gpu.attach(Driver(&state)); }
		if (mode & 2) { present.enable(Driver(&state), 7, 11, 13); present.attachDevice(); }
		const unsigned int clocks = state.clocks, frequencies = state.frequencies;
		gpu.begin(GpuTimingFrameInfo()); present.beginFrame(); ActualPresent(gpu, present);
		result |= Check(state.clocks - clocks == (mode ? 2U : 0U) &&
			state.frequencies - frequencies == (mode ? 1U : 0U), "off/GPU/Present/both preserve exact clock call budgets");
		result |= Check(present.counters().calls == ((mode & 2) ? 1U : 0U), "only opted-in actual calls create Present events");
		if (mode & 2)
		{
			const PresentTimingRecord& row = present.record(0);
			result |= Check(row.epoch == 1 && row.frame == 1 && row.ordinal == 1 && row.result == 0 &&
				row.end - row.start == 50 && row.frequency == 1000 && row.width == 1920 && row.height == 1080 &&
				present.owner().session == 7 && present.owner().process == 11 && present.owner().thread == 13,
				"event retains precise clocks, native result, frame and owner metadata");
			if (mode & 1)
				result |= Check(gpu.record(0).presentStartQpc == row.start && gpu.record(0).presentEndQpc == row.end &&
					gpu.record(0).cpuQpcFrequency == row.frequency && gpu.record(0).cpuPresentMs == 50.0,
					"combined captures use identical existing clock values");
		}
		if (mode == 0) result |= Check(state.creates == 0 && gpu.recordCount() == 0 && !present.enabled(),
			"off/off allocates no records/queries and performs no clock calls");
	}
	State state; Gpu gpu; Present present; gpu.enable(); gpu.attach(Driver(&state));
	present.enable(Driver(&state), 1, 2, 3); present.attachDevice();
	gpu.begin(GpuTimingFrameInfo());
	present.beginFrame(); ActualPresent(gpu, present, 0); gpu.failPresentation(-1);
	result |= Check(gpu.record(0).presentResult == -1 && present.record(0).result == 0,
		"post-call device failure cannot rewrite raw native Present result");
	present.beginFrame(); ActualPresent(gpu, present, DXGI_STATUS_OCCLUDED);
	present.beginFrame(); ActualPresent(gpu, present, -1);
	result |= Check(present.record(0).result == 0 && present.record(1).result == DXGI_STATUS_OCCLUDED &&
		present.record(2).result == -1 && present.counters().succeeded == 1 && present.counters().positive == 1 &&
		present.counters().failed == 1 && strcmp(PresentTimingResultName(DXGI_STATUS_OCCLUDED), "occluded") == 0,
		"raw S_OK, occluded and failed native calls remain independent outcomes");
	present.attachDevice(); present.beginFrame(); ActualPresent(gpu, present);
	result |= Check(present.record(3).epoch == 2 && present.record(3).frame == 1 && present.record(3).ordinal == 4,
		"device recreation resets frame ordinal, not owner-session Present ordinal");
	present.reset();
	result |= Check(!present.enabled() && present.counters().calls == 0 && present.owner().session == 0,
		"normal reset clears records, counters and previous owner metadata");
	present.enable(Driver(&state), 2, 2, 3); present.attachDevice(); present.beginFrame(); ActualPresent(gpu, present);
	return result | Check(present.record(0).epoch == 1 && present.record(0).ordinal == 1 && present.owner().session == 2,
		"next owner session starts clean");
}
int IndependentQueryCoverage()
{
	int result = 0;
	State state; Gpu gpu; Present present;
	gpu.enable(); gpu.attach(Driver(&state)); present.enable(Driver(&state), 1, 2, 3); present.attachDevice();
	for (unsigned int index = 0; index < Gpu::SlotCount; ++index)
	{
		gpu.begin(GpuTimingFrameInfo()); present.beginFrame();
		gpu.cancel(GpuTimingUnpresented);
	}
	result |= Check(present.counters().calls == 0 && gpu.counters().pending == Gpu::SlotCount,
		"pending unpresented query intervals never become Present events");
	gpu.begin(GpuTimingFrameInfo()); present.beginFrame();
	const unsigned int clocks = state.clocks, frequencies = state.frequencies;
	ActualPresent(gpu, present);
	result |= Check(gpu.counters().skippedFull == 1 && gpu.recordCount() == Gpu::SlotCount &&
		present.counters().calls == 1 && present.record(0).frame == Gpu::SlotCount + 1 &&
		state.clocks - clocks == 2 && state.frequencies - frequencies == 1,
		"native Present after query-slot skip is independently retained with two clocks");
	State failed; failed.failCreate = true; Gpu unavailable; Present independent;
	unavailable.enable(); unavailable.attach(Driver(&failed)); independent.enable(Driver(&failed), 1, 2, 3);
	independent.attachDevice(); unavailable.begin(GpuTimingFrameInfo()); independent.beginFrame();
	ActualPresent(unavailable, independent);
	result |= Check(unavailable.counters().allocationFailures == 1 && unavailable.counters().skippedUnavailable == 1 &&
		independent.counters().calls == 1 && independent.record(0).frequency == 1000,
		"query allocation failure cannot disable independent Present clocks");
	State ready; ready.ready = true; Gpu capped; Present afterCap;
	capped.enable(); capped.attach(Driver(&ready));
	for (unsigned int index = 0; index < Gpu::RecordCapacity; ++index)
	{ capped.begin(GpuTimingFrameInfo()); ActualPresent(capped, afterCap); }
	afterCap.enable(Driver(&ready), 1, 2, 3); afterCap.attachDevice();
	capped.begin(GpuTimingFrameInfo()); afterCap.beginFrame(); ActualPresent(capped, afterCap);
	return result | Check(capped.counters().skippedCap == 1 && afterCap.counters().calls == 1 &&
		afterCap.record(0).end > afterCap.record(0).start, "GPU record cap does not cap Present events");
}
struct Output
{
	enum Failure { None, Open, Header, Frame, Summary, Stream, Flush, Close, Publish };
	explicit Output(Failure failure) : failure(failure), closes(0), publishes(0), frames(0), footer(false) {}
	bool open() { return failure != Open; }
	bool header() { return failure != Header; }
	bool frame(const PresentTimingRecord&) { ++frames; return failure != Frame; }
	bool summary(const char *name, uint64_t) { if (strcmp(name, "export_end") == 0) footer = true; return failure != Summary; }
	bool healthy() { return failure != Stream; }
	bool flush() { return failure != Flush; }
	bool close() { ++closes; return failure != Close; }
	bool publish() { ++publishes; return failure != Publish; }
	Failure failure;
	unsigned int closes, publishes, frames;
	bool footer;
};
int BoundsAndExport()
{
	State state; Present present; present.enable(Driver(&state), 1, 2, 3); present.attachDevice();
	for (unsigned int index = 0; index < Present::Capacity + 3; ++index)
	{ present.beginFrame(); present.record(100, 100, 1000, 0, 0, 0, 1920, 1080); }
	int result = Check(present.counters().retained == Present::Capacity && present.counters().dropped == 3 &&
		present.counters().calls == Present::Capacity + 3 && present.counters().succeeded == Present::Capacity + 3 &&
		present.record(Present::Capacity - 1).ordinal == Present::Capacity && present.counters().invalidClocks == 0,
		"bounded prefix never overwrites, total counters continue and equal positive ticks are valid");
	present.record(0, 50, 1000, 0, 0, 0, 1, 1); present.record(100, 0, 1000, 0, 0, 0, 1, 1);
	present.record(100, 50, 1000, 0, 0, 0, 1, 1); present.record(100, 150, 0, 0, 0, 0, 1, 1);
	result |= Check(present.counters().invalidClocks == 4, "invalid clocks remain counted even after event capacity");
	unsigned int columns = 1;
	for (const char *cursor = PresentTimingCsvHeader(); *cursor; ++cursor) if (*cursor == ',') ++columns;
	result |= Check(columns == 18 && columns == PresentTimingCsvColumns &&
		strstr(PresentTimingCsvHeader(), ",status,count\n") != 0, "production Present schema has exact columns and final count");
	for (unsigned int failure = Output::None; failure <= Output::Publish; ++failure)
	{
		State sample; Present capture; capture.enable(Driver(&sample), 1, 2, 3); capture.attachDevice();
		capture.record(100, 150, 1000, 0, 0, 0, 1920, 1080);
		Output output(static_cast<Output::Failure>(failure));
		result |= Check(ExportPresentTimingCapture(capture, output) == (failure == Output::None) && output.closes == 1 &&
			capture.counters().ioFailures == (failure == Output::None ? 0U : 1U), "production exporter checks every stage without rendering failure");
		result |= Check(output.publishes == (failure == Output::None || failure == Output::Publish ? 1U : 0U),
			"publication follows successful body, flush and close only");
	}
	return result;
}

class Environment
{
public:
	Environment()
	{
		const DWORD count = GetEnvironmentVariableW(L"RTS_PRESENT_FRAME_TIMING_DIR", 0, 0);
		if (count) { std::vector<wchar_t> value(count); GetEnvironmentVariableW(L"RTS_PRESENT_FRAME_TIMING_DIR", &value[0], count); saved = &value[0]; }
		SetEnvironmentVariableW(L"RTS_PRESENT_FRAME_TIMING_DIR", 0);
	}
	~Environment() { SetEnvironmentVariableW(L"RTS_PRESENT_FRAME_TIMING_DIR", saved.empty() ? 0 : saved.c_str()); }
	std::wstring saved;
};
int NativeConfigurationAndCsv()
{
	Environment environment;
	D3D11PresentFrameTiming off; off.attachDevice(); off.beginFrame(); off.writeOnShutdown();
	int result = Check(!off.enabled(), "production wrapper is off with no opt-in");
	SetEnvironmentVariableW(L"RTS_PRESENT_FRAME_TIMING_DIR", L"relative-present-directory");
	D3D11PresentFrameTiming relative; relative.attachDevice();
	result |= Check(!relative.enabled(), "production configuration rejects relative directories");
	wchar_t temp[MAX_PATH], leaf[MAX_PATH], absolute[MAX_PATH];
	const DWORD length = GetTempPathW(MAX_PATH, temp);
	if (!length || length >= MAX_PATH || _snwprintf_s(leaf, MAX_PATH, _TRUNCATE,
		L"%lspresent-frame-timing-test-%lu-%llu", temp, GetCurrentProcessId(), static_cast<unsigned long long>(GetTickCount64())) < 0)
		return result | Check(false, "bounded task-temp leaf resolved");
	const DWORD absoluteLength = GetFullPathNameW(leaf, MAX_PATH, absolute, 0);
	if (!absoluteLength || absoluteLength >= MAX_PATH || !CreateDirectoryW(absolute, 0))
		return result | Check(false, "unique task-temp directory created");
	SetEnvironmentVariableW(L"RTS_PRESENT_FRAME_TIMING_DIR", absolute);
	D3D11PresentFrameTiming capture;
	// Reuse the production wrapper, not manually assigned recording-driver IDs.
	for (unsigned long long expectedSession = 1; expectedSession <= 2; ++expectedSession)
	{
	capture.attachDevice(); capture.beginFrame();
	Gpu gpu;
	const uint64_t start = BeginPresentTiming(gpu, capture);
	EndPresentTiming(gpu, capture, start, 0, 0, 0, 1920, 1080);
	wchar_t pattern[MAX_PATH]; _snwprintf_s(pattern, MAX_PATH, _TRUNCATE, L"%ls\\present-frame-timing-*.csv*", absolute);
	WIN32_FIND_DATAW found;
	HANDLE files = FindFirstFileW(pattern, &found);
	result |= Check(files == INVALID_HANDLE_VALUE, "native recording performs no hot file I/O");
	if (files != INVALID_HANDLE_VALUE) FindClose(files);
	std::thread foreign([&capture] { capture.writeOnShutdown(); }); foreign.join();
	result |= Check(capture.enabled(), "foreign shutdown cannot export or discard owner records");
	capture.writeOnShutdown(); capture.writeOnShutdown();
	files = FindFirstFileW(pattern, &found);
	result |= Check(files != INVALID_HANDLE_VALUE, "owner shutdown exports once");
	unsigned int fileCount = 0;
	if (files != INVALID_HANDLE_VALUE)
	{
		do
		{
			++fileCount;
			result |= Check(wcsstr(found.cFileName, L".pending") == 0, "only successfully published CSV qualifies");
			wchar_t path[MAX_PATH]; _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%ls\\%ls", absolute, found.cFileName);
			FILE *file = _wfopen(path, L"rb");
			if (file)
			{
				char line[2048]; result |= Check(fgets(line, sizeof(line), file) && strcmp(line, PresentTimingCsvHeader()) == 0,
					"physical published CSV preserves exact header");
				unsigned int presents = 0, summaries = 0;
				while (fgets(line, sizeof(line), file))
				{
					unsigned int fields = 1; for (const char *p = line; *p; ++p) if (*p == ',') ++fields;
					result |= Check(fields == PresentTimingCsvColumns, "physical frame and summary rows preserve field count");
					if (strncmp(line, "present,", 8) == 0)
					{
						++presents;
						unsigned long long session, ownerStart, epoch, frame, ordinal, callStart, callEnd, frequency;
						unsigned long process, thread; long nativeResult;
						const int parsed = sscanf(line, "present,%llu,%lu,%lu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%ld",
							&session, &process, &thread, &ownerStart, &epoch, &frame, &ordinal, &callStart, &callEnd, &frequency, &nativeResult);
						result |= Check(parsed == 11 && session == expectedSession && process == GetCurrentProcessId() && thread == GetCurrentThreadId() &&
							ownerStart > 0 && epoch == 1 && frame == 1 && ordinal == 1 && callStart > 0 && callEnd >= callStart &&
							frequency > 0 && nativeResult == 0, "wrapper reinitialize advances session and resets frame/Present ordinals");
					}
					if (strncmp(line, "summary,", 8) == 0) ++summaries;
				}
				result |= Check(presents == 1 && summaries == 10 && !ferror(file), "one native event and complete summaries exported");
				fclose(file);
			}
			else result |= Check(false, "published file readable");
			result |= Check(DeleteFileW(path) != FALSE, "exact task-owned test output removed");
		} while (FindNextFileW(files, &found));
		FindClose(files);
	}
	result |= Check(fileCount == 1 && !capture.enabled(), "repeat shutdown creates no duplicate export");
	}
	result |= Check(RemoveDirectoryW(absolute) != FALSE, "owned empty test leaf removed");
	return result;
}
}
int main()
{
	int result = ClockModesAndOutcomes(); result |= IndependentQueryCoverage();
	result |= BoundsAndExport(); result |= NativeConfigurationAndCsv();
	if (!result) puts("Independent native Present capture contracts passed (recording driver, no GPU)");
	return result;
}
