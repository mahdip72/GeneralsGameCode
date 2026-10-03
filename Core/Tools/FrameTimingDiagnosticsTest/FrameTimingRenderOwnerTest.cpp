#include <windows.h>
#include <atomic>

namespace
{
std::atomic<unsigned int> clockCalls(0);
std::atomic<unsigned int> threadIdCalls(0);
BOOL WINAPI CountClockCalls(LARGE_INTEGER *value)
{
	clockCalls.fetch_add(1);
	return QueryPerformanceCounter(value);
}
DWORD WINAPI CountThreadIdCalls()
{
	threadIdCalls.fetch_add(1);
	return GetCurrentThreadId();
}
}
#define QueryPerformanceCounter CountClockCalls
#define GetCurrentThreadId CountThreadIdCalls
#include "Lib/FrameTimingDiagnostics.h"
#undef QueryPerformanceCounter
#undef GetCurrentThreadId
#include <thread>
#include <vector>
#include <string>
#include <stdexcept>

namespace
{
int failures = 0;
void Check(bool value, const char *message)
{
	if (!value) { fprintf(stderr, "FAIL: %s\n", message); ++failures; }
}
std::vector<std::string> Files(const std::string &directory, const char *pattern)
{
	std::vector<std::string> result;
	WIN32_FIND_DATAA entry;
	HANDLE search = FindFirstFileA((directory + "\\" + pattern).c_str(), &entry);
	if (search != INVALID_HANDLE_VALUE)
	{
		do { result.push_back(directory + "\\" + entry.cFileName); }
		while (FindNextFileA(search, &entry));
		FindClose(search);
	}
	return result;
}
void Remove(const std::string &directory)
{
	const std::vector<std::string> paths = Files(directory, "*-timing-*.csv");
	for (size_t i = 0; i < paths.size(); ++i)
		Check(DeleteFileA(paths[i].c_str()) != FALSE, "remove only fixture-owned CSV");
	Check(RemoveDirectoryA(directory.c_str()) != FALSE, "remove empty fixture directory");
}
void Disabled(const std::string &directory)
{
	SetEnvironmentVariableA("RTS_FRAME_TIMING_DIR", NULL);
	SetEnvironmentVariableA("RTS_RENDER_OWNER_TIMING_DIR", NULL);
	const unsigned int before = clockCalls.load();
	{
		rts::frame_timing::Capture capture(rts::frame_timing::Capture::RenderOwnerStream);
		capture.beginSession("render_owner");
		Check(!capture.isEnabled(), "disabled owner capture has no enabled session");
		const unsigned int beforeThreadIds = threadIdCalls.load();
		{
			rts::frame_timing::ExecutionPacket packet(capture, 0x100000001ULL);
			rts::frame_timing::Scope timing(rts::frame_timing::RendererConstantPack);
			timing.finish(); timing.finish();
		}
		Check(threadIdCalls.load() == beforeThreadIds, "disabled packets/scopes perform no thread-ID queries");
		capture.endSession();
	}
	Check(clockCalls.load() == before, "disabled render packet/scopes perform no clock queries");
	Check(Files(directory, "*-timing-*.csv").empty(), "disabled render capture creates no CSV");
	Check(rts::frame_timing::BoundCaptureSlot() == NULL, "disabled packet restores TLS binding");
}
void GameOnly(const std::string &directory)
{
	// Existing strict scaling runners opt into only the game CSV. A render
	// owner must not create a second sidecar in their one-file receipt closure.
	SetEnvironmentVariableA("RTS_FRAME_TIMING_DIR", directory.c_str());
	SetEnvironmentVariableA("RTS_RENDER_OWNER_TIMING_DIR", NULL);
	rts::frame_timing::Capture &game = rts::frame_timing::Capture::instance();
	game.beginSession("interactive");
	game.beginFrame(1);
	const unsigned int before = clockCalls.load();
	{
		rts::frame_timing::Capture owner(rts::frame_timing::Capture::RenderOwnerStream);
		owner.beginSession("render_owner");
		Check(!owner.isEnabled(), "game-only opt-in leaves owner session disabled");
		const unsigned int beforeThreadIds = threadIdCalls.load();
		{
			rts::frame_timing::ExecutionPacket packet(owner, 1);
			rts::frame_timing::Scope timing(rts::frame_timing::RendererDrawSubmit);
		}
		Check(threadIdCalls.load() == beforeThreadIds, "game-only owner packets/scopes perform no thread-ID queries");
		owner.endSession();
	}
	Check(clockCalls.load() == before, "game-only opt-in leaves owner clock disabled");
	Check(Files(directory, "render-owner-timing-*.csv").empty(), "game-only opt-in cannot emit owner sidecar");
	// Model the real owner's loop with an active game capture on another
	// thread, owner output disabled, and no per-packet ExecutionPacket wrapper.
	std::thread renderOwner([&] {
		rts::frame_timing::Capture owner(rts::frame_timing::Capture::RenderOwnerStream);
		owner.beginSession("render_owner");
		Check(!owner.isEnabled(), "foreign loop has disabled owner telemetry");
		const unsigned int beforeThreadIds = threadIdCalls.load();
		{
			rts::frame_timing::BindCapture loopBinding(owner);
			for (unsigned int packet = 0; packet < 16; ++packet)
			{
				rts::frame_timing::Scope timing(rts::frame_timing::RendererConstantPack);
				timing.finish();
			}
		}
		Check(threadIdCalls.load() == beforeThreadIds,
			"inactive loop binding avoids foreign game-capture thread-ID queries");
		Check(rts::frame_timing::BoundCaptureSlot() == NULL,
			"owner loop restores its prior TLS binding");
		owner.endSession();
	});
	renderOwner.join();
	Check(clockCalls.load() == before, "disabled owner loop performs no clock queries with active game capture");
	game.endFrame(1);
	game.endSession();
	Check(game.finalize().complete, "game-only fixture finalizes normal main capture");
	Check(Files(directory, "frame-timing-*.csv").size() == 1,
		"game-only opt-in retains exactly its one main CSV");
	SetEnvironmentVariableA("RTS_FRAME_TIMING_DIR", NULL);
}
void Enabled(const std::string &directory)
{
	SetEnvironmentVariableA("RTS_FRAME_TIMING_DIR", NULL);
	SetEnvironmentVariableA("RTS_RENDER_OWNER_TIMING_DIR", directory.c_str());
	rts::frame_timing::Capture owner(rts::frame_timing::Capture::RenderOwnerStream);
	owner.beginSession("render_owner");
	Check(owner.isEnabled(), "owner-specific opt-in enables packet diagnostics");
	const unsigned __int64 first = 0x100000001ULL;
	{
		rts::frame_timing::ExecutionPacket packet(owner, first);
		Check(&rts::frame_timing::SelectedCapture() == &owner, "default scopes select bound render capture");
		Check(!rts::frame_timing::IsActive(), "render binding does not activate global game capture");
		rts::frame_timing::Scope timing(rts::frame_timing::RendererDrawSubmit);
		timing.finish(); timing.finish();
		const unsigned int before = clockCalls.load();
		std::thread foreign([&owner] {
			rts::frame_timing::BindCapture binding(owner);
			rts::frame_timing::Scope timing(rts::frame_timing::RendererDrawSubmit);
			timing.finish();
		});
		foreign.join();
		Check(clockCalls.load() == before, "foreign bound capture performs no clock queries");
	}
	Check(rts::frame_timing::BoundCaptureSlot() == NULL, "packet restores empty TLS binding");
	try
	{
		rts::frame_timing::ExecutionPacket packet(owner, first);
		rts::frame_timing::Scope timing(rts::frame_timing::RendererDrawSubmit);
		throw std::runtime_error("fixture unwind");
	}
	catch (const std::runtime_error &) {}
	Check(rts::frame_timing::BoundCaptureSlot() == NULL && !owner.isActive(),
		"unwind closes only its execution segment and restores TLS");
	{
		rts::frame_timing::BindCapture previous(owner);
		{
			rts::frame_timing::ExecutionPacket packet(owner, first + 1);
			rts::frame_timing::Scope timing(rts::frame_timing::RendererDrawSubmit);
		}
		Check(rts::frame_timing::BoundCaptureSlot() == &owner, "nested packet restores previous binding");
	}
	owner.endSession();
	Check(owner.finalize().closed, "render owner closes sidecar on owner thread");
	Check(Files(directory, "frame-timing-*.csv").empty(), "render sidecar does not match game receipt glob");
	const std::vector<std::string> paths = Files(directory, "render-owner-timing-*.csv");
	Check(paths.size() == 1, "exactly one distinct render owner sidecar");
	if (paths.size() != 1) return;
	FILE *file = fopen(paths[0].c_str(), "rb");
	Check(file != NULL, "closed render CSV readable");
	if (!file) return;
	char line[1024];
	Check(fgets(line, sizeof(line), file) != NULL && strstr(line,
		"session,mode,sequence_begin,sequence_end,executed_packets,wall_ms,phase,") == line,
		"render stream header labels sequences and executed segments");
	unsigned int rows = 0;
	while (fgets(line, sizeof(line), file))
	{
		unsigned int session = 0, samples = 0, over33 = 0, over100 = 0;
		unsigned __int64 begin = 0, end = 0, packets = 0;
		char mode[32], phase[32];
		double wall, total, average, p95, p99, maximum;
		const int fields = sscanf(line, "%u,%31[^,],%llu,%llu,%llu,%lf,%31[^,],%u,%lf,%lf,%lf,%lf,%lf,%u,%u",
			&session, mode, &begin, &end, &packets, &wall, phase, &samples,
			&total, &average, &p95, &p99, &maximum, &over33, &over100);
		Check(fields == 15 && strcmp(mode, "render_owner") == 0 &&
			begin == first && end == first + 1 && packets == 3 && samples == 3,
			"64-bit sequences and three executed segments survive split and unwind");
		Check(strcmp(phase, rows == 0 ? "execution_packet" : "renderer_draw_submit") == 0,
			"only packet and owner submission phases emit");
		++rows;
	}
	Check(rows == 2, "no duplicate finish or foreign-owner sample");
	fclose(file);
}
}
int main()
{
	char relative[96], absolute[MAX_PATH];
	_snprintf(relative, sizeof(relative), "FrameTimingRenderOwnerTest-%lu-%lu", GetCurrentProcessId(), GetTickCount());
	const DWORD length = GetFullPathNameA(relative, sizeof(absolute), absolute, NULL);
	if (!length || length >= sizeof(absolute) || !CreateDirectoryA(absolute, NULL)) return 1;
	const std::string root = absolute, disabled = root + "\\disabled", enabled = root + "\\enabled";
	Check(CreateDirectoryA(disabled.c_str(), NULL) != FALSE, "create disabled fixture directory");
	Check(CreateDirectoryA(enabled.c_str(), NULL) != FALSE, "create enabled fixture directory");
	Disabled(disabled);
	GameOnly(disabled);
	Enabled(enabled);
	SetEnvironmentVariableA("RTS_FRAME_TIMING_DIR", NULL);
	SetEnvironmentVariableA("RTS_RENDER_OWNER_TIMING_DIR", NULL);
	Remove(disabled); Remove(enabled);
	Check(RemoveDirectoryA(root.c_str()) != FALSE, "remove empty fixture root");
	return failures ? 1 : 0;
}
