#include "Lib/FrameTimingDiagnostics.h"

#include <atomic>
#include <chrono>
#include <stdio.h>
#include <string>
#include <string.h>
#include <thread>
#include <vector>

namespace
{
int failures = 0;

void check(bool condition, const char *message)
{
	if (!condition)
	{
		fprintf(stderr, "FAIL: %s\n", message);
		++failures;
	}
}

std::vector<std::string> files(const std::string &directory)
{
	std::vector<std::string> result;
	WIN32_FIND_DATAA data;
	const std::string pattern = directory + "\\frame-timing-*.csv";
	HANDLE search = FindFirstFileA(pattern.c_str(), &data);
	if (search == INVALID_HANDLE_VALUE)
		return result;
	do
	{
		result.push_back(directory + "\\" + data.cFileName);
	}
	while (FindNextFileA(search, &data));
	FindClose(search);
	return result;
}

unsigned countPhase(const std::string &directory, const char *phase)
{
	const std::vector<std::string> paths = files(directory);
	check(paths.size() == 1, "owner fixture creates one capture file");
	if (paths.size() != 1)
		return 0;
	FILE *file = fopen(paths[0].c_str(), "rb");
	check(file != NULL, "owner fixture capture is readable");
	if (file == NULL)
		return 0;
	char line[1024];
	unsigned count = 0;
	while (fgets(line, sizeof(line), file) != NULL)
	{
		const std::string marker = std::string(",") + phase + ",";
		if (strstr(line, marker.c_str()) != NULL)
			++count;
	}
	fclose(file);
	return count;
}

void removeCapture(const std::string &directory)
{
	const std::vector<std::string> paths = files(directory);
	for (std::size_t i = 0; i < paths.size(); ++i)
		check(DeleteFileA(paths[i].c_str()) != FALSE, "remove owner fixture capture");
	check(RemoveDirectoryA(directory.c_str()) != FALSE, "remove owner fixture directory");
}

bool waitFor(const std::atomic<bool> &value)
{
	const std::chrono::steady_clock::time_point deadline =
		std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (!value.load(std::memory_order_acquire) &&
		std::chrono::steady_clock::now() < deadline)
		std::this_thread::yield();
	return value.load(std::memory_order_acquire);
}

void ownerOnlyEmission()
{
	char relative[96], absolute[MAX_PATH];
	_snprintf(relative, sizeof(relative), "FrameTimingDiagnosticsOwner-%lu-%lu",
		GetCurrentProcessId(), GetTickCount());
	const DWORD pathLength = GetFullPathNameA(relative, sizeof(absolute), absolute, NULL);
	check(pathLength != 0 && pathLength < sizeof(absolute), "owner fixture path is bounded");
	if (pathLength == 0 || pathLength >= sizeof(absolute))
		return;
	const std::string directory = absolute;
	check(CreateDirectoryA(directory.c_str(), NULL) != FALSE,
		"owner fixture directory is newly created");
	if (failures != 0)
		return;

	SetEnvironmentVariableA("RTS_FRAME_TIMING_DIR", directory.c_str());
	LARGE_INTEGER frequency = {};
	check(QueryPerformanceFrequency(&frequency) != FALSE && frequency.QuadPart > 0,
		"owner fixture has a performance counter");
	if (frequency.QuadPart <= 0)
	{
		SetEnvironmentVariableA("RTS_FRAME_TIMING_DIR", NULL);
		removeCapture(directory);
		return;
	}

	{
		rts::frame_timing::Capture capture;
		capture.beginSession("headless");
		capture.beginFrame(100);
		// The owner emits one known row. Foreign scopes must emit none, even
		// while this owner repeatedly toggles the mutable frame state.
		capture.add(rts::frame_timing::AudioVoiceCreate, frequency.QuadPart / 1000);

		std::atomic<bool> entered(false), go(false), exercised(false), stop(false);
		std::atomic<unsigned> foreignScopes(0);
		std::thread foreign([&capture, &entered, &go, &exercised, &stop, &foreignScopes]() {
			entered.store(true, std::memory_order_release);
			while (!go.load(std::memory_order_acquire) &&
				!stop.load(std::memory_order_acquire))
				std::this_thread::yield();
			while (!stop.load(std::memory_order_acquire))
			{
				{
					rts::frame_timing::Scope timing(capture,
						rts::frame_timing::AudioVoiceCreate);
				}
				foreignScopes.fetch_add(1, std::memory_order_relaxed);
				exercised.store(true, std::memory_order_release);
			}
		});

		check(waitFor(entered), "foreign timing thread enters within bound");
		go.store(true, std::memory_order_release);
		check(waitFor(exercised), "foreign timing thread executes before the owner stress loop");
		for (unsigned int frame = 101; frame != 613; ++frame)
		{
			capture.endFrame(frame);
			capture.beginFrame(frame + 1);
		}
		stop.store(true, std::memory_order_release);
		foreign.join();
		check(foreignScopes.load(std::memory_order_relaxed) != 0,
			"foreign timing thread exercised bounded scopes");
		capture.endFrame(1000);
		capture.endSession();
	}

	SetEnvironmentVariableA("RTS_FRAME_TIMING_DIR", NULL);
	check(countPhase(directory, "audio_voice_create") == 1,
		"only the owner emits the voice-create timing row");
	removeCapture(directory);
}
}

int main()
{
	ownerOnlyEmission();
	return failures == 0 ? 0 : 1;
}
