#include "Lib/FrameTimingDiagnostics.h"

#include <string>
#include <vector>

namespace
{
int failures = 0;

void check(bool condition, const char* message)
{
	if (!condition)
	{
		fprintf(stderr, "FAIL: %s\n", message);
		++failures;
	}
}

std::vector<std::string> files(const std::string& directory)
{
	std::vector<std::string> result;
	WIN32_FIND_DATAA data;
	HANDLE search = FindFirstFileA((directory + "\\frame-timing-*.csv").c_str(), &data);
	if (search != INVALID_HANDLE_VALUE)
	{
		do { result.push_back(directory + "\\" + data.cFileName); }
		while (FindNextFileA(search, &data));
		FindClose(search);
	}
	return result;
}

struct Row
{
	unsigned int session, first, last, frames, samples, over33, over100;
	char mode[32], phase[32];
	double wall, total, average, p95, p99, maximum;
};

std::vector<Row> rows(const std::string& directory)
{
	const std::vector<std::string> paths = files(directory);
	check(paths.size() == 1, "exactly one capture file");
	std::vector<Row> result;
	if (paths.size() != 1)
		return result;
	FILE* file = fopen(paths[0].c_str(), "rb");
	check(file != NULL, "capture is readable after flush");
	if (!file)
		return result;
	char line[1024];
	check(fgets(line, sizeof(line), file) != NULL, "CSV header is flushed");
	while (fgets(line, sizeof(line), file))
	{
		Row row = {};
		const int fields = sscanf(line, "%u,%31[^,],%u,%u,%u,%lf,%31[^,],%u,%lf,%lf,%lf,%lf,%lf,%u,%u",
			&row.session, row.mode, &row.first, &row.last, &row.frames, &row.wall, row.phase, &row.samples,
			&row.total, &row.average, &row.p95, &row.p99, &row.maximum, &row.over33, &row.over100);
		check(fields == 15, "every CSV row has the documented fields");
		if (fields == 15)
			result.push_back(row);
	}
	fclose(file);
	return result;
}

void removeCase(const std::string& directory)
{
	const std::vector<std::string> paths = files(directory);
	for (std::size_t i = 0; i < paths.size(); ++i)
		check(DeleteFileA(paths[i].c_str()) != FALSE, "remove only this test capture");
	check(RemoveDirectoryA(directory.c_str()) != FALSE, "remove empty test case directory");
}

void disabled(const std::string& directory)
{
	SetEnvironmentVariableA("RTS_FRAME_TIMING_DIR", NULL);
	{
		rts::frame_timing::Capture capture;
		capture.beginSession("headless");
		capture.beginFrame(0);
		capture.add(rts::frame_timing::Logic, 100);
		capture.endFrame(900);
		capture.endSession();
		check(!capture.isActive(), "disabled capture remains inactive");
	}
	check(files(directory).empty(), "disabled capture creates no output");
}

void enabled(const std::string& directory, __int64 frequency)
{
	SetEnvironmentVariableA("RTS_FRAME_TIMING_DIR", directory.c_str());
	{
		rts::frame_timing::Capture capture;
		capture.beginSession("headless");
		capture.beginFrame(100);
		for (int i = 0; i < 19; ++i)
			capture.add(rts::frame_timing::Logic, frequency / 1000);
		capture.add(rts::frame_timing::Logic, frequency / 10);
		const rts::frame_timing::Phase simulationPhases[] = {
			rts::frame_timing::SimulationSnapshot,
			rts::frame_timing::SimulationSerial,
			rts::frame_timing::SimulationParallel,
			rts::frame_timing::SimulationWait,
			rts::frame_timing::SimulationReduce,
			rts::frame_timing::SimulationShadowCompare,
			rts::frame_timing::SimulationCommit,
			rts::frame_timing::CollisionAdmission,
			rts::frame_timing::CollisionLiveValidation,
			rts::frame_timing::CollisionExistingFilter,
			rts::frame_timing::CollisionCommitPrepare
		};
		for (std::size_t phase = 0; phase < sizeof(simulationPhases) / sizeof(simulationPhases[0]); ++phase)
			capture.add(simulationPhases[phase], frequency / 2000);
		capture.endFrame(1000); // Forces the headless periodic bucket without sleeping.
		std::vector<Row> data = rows(directory);
		check(data.size() == 13, "periodic flush writes frame, logic, and simulation phases before session ends");
		if (data.size() == 13)
		{
			const Row& logic = data[1];
			check(strcmp(logic.phase, "logic") == 0 && logic.samples == 20, "logic sample count");
			check(logic.frames == 900 && logic.first == 100 && logic.last == 1000, "periodic frame range/count");
			check(logic.p95 > 0.0 && logic.p95 <= 1.1 && logic.p99 >= 99.0 && logic.p99 <= 100.1,
				"histogram percentile upper bounds retain the tail");
			check(logic.maximum >= 99.0 && logic.over33 == 1, "max and stall threshold counter");
			check(logic.total > 118.0 && logic.total < 120.0, "sample totals");
			const char *simulationNames[] = {
				"simulation_snapshot", "simulation_serial", "simulation_parallel", "simulation_wait",
				"simulation_reduce", "simulation_shadow_compare", "simulation_commit",
				"collision_admission", "collision_live_validation", "collision_existing_filter",
				"collision_commit_prepare"
			};
			for (std::size_t phase = 0; phase < sizeof(simulationNames) / sizeof(simulationNames[0]); ++phase)
			{
				check(strcmp(data[phase + 2].phase, simulationNames[phase]) == 0 &&
					data[phase + 2].samples == 1, "simulation phase name and sample count");
			}
		}
		capture.beginFrame(1000);
		capture.endFrame(1005);
		capture.beginFrame(1005);
		capture.endFrame(0); // Game teardown can reset GameLogic before EndFrame.
		capture.endSession();
		data = rows(directory);
		check(data.size() == 14 && data.back().frames == 5 &&
			data.back().first == 1000 && data.back().last == 1005,
			"session end preserves the final pre-reset frame range");
		capture.beginSession("interactive");
		capture.beginFrame(0);
		capture.endFrame(1);
		// Destructor must retain this final partial bucket without endSession.
	}
	const std::vector<Row> data = rows(directory);
	check(data.size() == 15 && data.back().session == 2 && data.back().frames == 1 &&
		strcmp(data.back().mode, "interactive") == 0, "destructor/session reset retains only new frame counts");
}

void bounded(const std::string& directory)
{
	SetEnvironmentVariableA("RTS_FRAME_TIMING_DIR", directory.c_str());
	{
		rts::frame_timing::Capture capture;
		capture.beginSession("headless");
		for (unsigned int i = 0; i < 16400; ++i)
		{
			capture.beginFrame(i * 900);
			capture.endFrame((i + 1) * 900);
		}
		check(!capture.isActive(), "row limit leaves capture inactive");
	}
	check(rows(directory).size() == 16384, "capture stops at the fixed row bound");
}
}

int main()
{
	// CTest's working directory is the build tree, never the live game profile.
	char relative[80], absolute[MAX_PATH];
	_snprintf(relative, sizeof(relative), "FrameTimingDiagnosticsTest-%lu-%lu", GetCurrentProcessId(), GetTickCount());
	const DWORD pathLength = GetFullPathNameA(relative, sizeof(absolute), absolute, NULL);
	if (pathLength == 0 || pathLength >= sizeof(absolute))
		return 1;
	const std::string root = absolute;
	if (!CreateDirectoryA(root.c_str(), NULL))
		return 1; // Never reuse or remove a directory owned by another run.
	const std::string disabledDir = root + "\\disabled", enabledDir = root + "\\enabled", boundedDir = root + "\\bounded";
	check(CreateDirectoryA(disabledDir.c_str(), NULL) != FALSE, "create disabled case");
	check(CreateDirectoryA(enabledDir.c_str(), NULL) != FALSE, "create enabled case");
	check(CreateDirectoryA(boundedDir.c_str(), NULL) != FALSE, "create bounded case");
	LARGE_INTEGER frequency;
	if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0)
		return 1;
	disabled(disabledDir);
	enabled(enabledDir, frequency.QuadPart);
	bounded(boundedDir);
	SetEnvironmentVariableA("RTS_FRAME_TIMING_DIR", NULL);
	removeCase(disabledDir);
	removeCase(enabledDir);
	removeCase(boundedDir);
	check(RemoveDirectoryA(root.c_str()) != FALSE, "remove empty test root");
	return failures ? 1 : 0;
}
