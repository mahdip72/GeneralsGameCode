#include <windows.h>

namespace
{
unsigned int diagnosticClockCalls = 0;
unsigned int diagnosticThreadIdCalls = 0;
DWORD WINAPI CountDiagnosticThreadIdCalls()
{
	++diagnosticThreadIdCalls;
	return GetCurrentThreadId();
}
BOOL WINAPI CountDiagnosticClockCalls(LARGE_INTEGER *value)
{
	++diagnosticClockCalls;
	return QueryPerformanceCounter(value);
}
}

// Count the real header's clock calls without changing its production clock.
#define QueryPerformanceCounter CountDiagnosticClockCalls
#define GetCurrentThreadId CountDiagnosticThreadIdCalls
#include "Lib/FrameTimingDiagnostics.h"
#undef QueryPerformanceCounter
#undef GetCurrentThreadId

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
	char mode[32], phase[64];
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
		const int fields = sscanf(line,
			"%u,%31[^,],%u,%u,%u,%lf,%63[^,],%u,%lf,%lf,%lf,%lf,%lf,%u,%u",
			&row.session, row.mode, &row.first, &row.last, &row.frames, &row.wall, row.phase, &row.samples,
			&row.total, &row.average, &row.p95, &row.p99, &row.maximum, &row.over33, &row.over100);
		int columnCount = 1;
		for (const char *column = line; *column; ++column)
			if (*column == ',') ++columnCount;
		check(fields == 15 && columnCount == 15,
			"every ordinary CSV row retains exactly the documented fifteen fields");
		if (fields == 15 && columnCount == 15)
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

void inactiveSingleton(const std::string& directory)
{
	// A valid opt-in directory alone must not make a display gate construct
	// the singleton or open its output before the game-owned session begins.
	SetEnvironmentVariableA("RTS_FRAME_TIMING_DIR", directory.c_str());
	const unsigned int before = diagnosticClockCalls;
	check(!rts::frame_timing::IsActive(), "uninitialized singleton remains inactive");
	check(diagnosticClockCalls == before, "inactive singleton gate does not query the clock");
	check(files(directory).empty(), "inactive singleton gate does not create a CSV file");
}

void disabled(const std::string& directory)
{
	SetEnvironmentVariableA("RTS_FRAME_TIMING_DIR", NULL);
	const unsigned int before = diagnosticClockCalls;
	{
		rts::frame_timing::Capture capture;
		capture.beginSession("headless");
		capture.beginFrame(0);
		const rts::frame_timing::Phase instrumentedPhases[] = {
			rts::frame_timing::RendererConstantPack,
			rts::frame_timing::RendererConstantUpload,
			rts::frame_timing::RendererBufferUpload,
			rts::frame_timing::RendererDrawValidation,
			rts::frame_timing::RendererDrawSubmit,
			rts::frame_timing::RendererBufferShadow,
			rts::frame_timing::RendererSceneLights,
			rts::frame_timing::RendererProjectedShadows,
			rts::frame_timing::RendererVolumeShadows,
			rts::frame_timing::RendererSorting,
			rts::frame_timing::RendererParticles,
			rts::frame_timing::RendererSkinRender,
			rts::frame_timing::RendererCommandEnqueue,
			rts::frame_timing::RendererVolumePrepare,
			rts::frame_timing::RendererVolumeSubmit,
			rts::frame_timing::RendererProjectedTerrain,
			rts::frame_timing::RendererProjectedDecal,
			rts::frame_timing::RendererProjectedFlush,
			rts::frame_timing::RendererParticlePrepare,
			rts::frame_timing::RendererParticleSubmit,
			rts::frame_timing::RendererProjectedSceneMeshDrain,
			rts::frame_timing::RendererTextureOwnerDrain,
			rts::frame_timing::RendererTextureCopyPublication,
			rts::frame_timing::ClientDrawableSweep,
			rts::frame_timing::RendererWW3DSync,
			rts::frame_timing::RendererW3DViewUpdate,
			rts::frame_timing::RendererShroudSourceSync,
			rts::frame_timing::RendererSceneCustomizedRender,
			rts::frame_timing::RendererSceneFlush
		};
		const rts::frame_timing::Phase audioPhases[] = {
			rts::frame_timing::AudioAssetResolve, rts::frame_timing::AudioVirtualRead,
			rts::frame_timing::AudioFfmpegProbe, rts::frame_timing::AudioStreamOpen,
			rts::frame_timing::AudioSampleLookup, rts::frame_timing::AudioSampleHit,
			rts::frame_timing::AudioSampleMiss, rts::frame_timing::AudioSampleFill,
			rts::frame_timing::AudioSampleFallback
		};
		const unsigned int threadCallsBeforeScopes = diagnosticThreadIdCalls;
		rts::frame_timing::BindCapture captureBinding(capture);
		for (std::size_t phase = 0; phase < sizeof(instrumentedPhases) / sizeof(instrumentedPhases[0]); ++phase)
		{
			rts::frame_timing::Scope timing(capture, instrumentedPhases[phase]);
			timing.finish();
			timing.finish();
			rts::frame_timing::Scope productionTiming(instrumentedPhases[phase]);
			productionTiming.finish();
			productionTiming.finish();
		}
		capture.add(rts::frame_timing::Logic, 100);
		for (std::size_t phase = 0; phase < sizeof(audioPhases) / sizeof(audioPhases[0]); ++phase)
		{
			rts::frame_timing::Scope timing(capture, audioPhases[phase]);
			timing.finish();
			timing.finish();
			// Exercise the production overload with the disabled capture selected,
			// without consuming the singleton used by later activation tests.
			rts::frame_timing::Scope productionTiming(audioPhases[phase]);
			productionTiming.finish();
			productionTiming.finish();
		}
		check(diagnosticThreadIdCalls == threadCallsBeforeScopes,
			"disabled hot-path scopes and add do not query thread identity");
		capture.endFrame(900);
		capture.endSession();
		check(!capture.isActive(), "disabled capture remains inactive");
		check(!capture.finalize().complete,
			"disabled capture cannot provide complete receipt evidence");
	}
	check(diagnosticClockCalls == before, "disabled renderer scopes and repeated finish make no clock queries");
	check(files(directory).empty(), "disabled capture creates no output");
}

void audioStages(const std::string& directory)
{
	SetEnvironmentVariableA("RTS_FRAME_TIMING_DIR", directory.c_str());
	{
		rts::frame_timing::Capture capture;
		capture.beginSession("headless");
		capture.beginFrame(0);
		const rts::frame_timing::Phase phases[] = {
			rts::frame_timing::AudioAssetResolve, rts::frame_timing::AudioVirtualRead,
			rts::frame_timing::AudioFfmpegProbe, rts::frame_timing::AudioStreamOpen,
			rts::frame_timing::AudioSampleLookup, rts::frame_timing::AudioSampleHit,
			rts::frame_timing::AudioSampleMiss, rts::frame_timing::AudioSampleFill,
			rts::frame_timing::AudioSampleFallback
		};
		for (std::size_t phase = 0; phase < sizeof(phases) / sizeof(phases[0]); ++phase)
		{
			rts::frame_timing::Scope timing(capture, phases[phase]);
			timing.finish();
			const unsigned int finishedAt = diagnosticClockCalls;
			timing.finish();
			check(diagnosticClockCalls == finishedAt, "audio timing finishes exactly once");
		}
		capture.endFrame(900);
		capture.endSession();
	}
	const std::vector<Row> data = rows(directory);
	const char *names[] = {
		"audio_asset_resolve", "audio_virtual_read", "audio_ffmpeg_probe", "audio_stream_open",
		"audio_sample_lookup", "audio_sample_hit", "audio_sample_miss", "audio_sample_fill",
		"audio_sample_uncached_fallback"
	};
	check(data.size() == 10, "frame plus all nine audio stages retain the CSV schema");
	if (data.size() == 10)
		for (std::size_t phase = 0; phase < sizeof(names) / sizeof(names[0]); ++phase)
			check(strcmp(data[phase + 1].phase, names[phase]) == 0 &&
				data[phase + 1].samples == 1, "audio stage name and invocation count");
}

void volumeParticleStages(const std::string& directory)
{
	const rts::frame_timing::Phase phases[] = {
		rts::frame_timing::RendererVolumeParticle,
		rts::frame_timing::RendererVolumeParticleTransform,
		rts::frame_timing::RendererVolumeParticleUpdateArrays,
		rts::frame_timing::RendererVolumeParticlePackSubmit
	};
	SetEnvironmentVariableA("RTS_FRAME_TIMING_DIR", NULL);
	const unsigned int disabledClockCalls = diagnosticClockCalls;
	{
		rts::frame_timing::Capture capture;
		capture.beginSession("headless");
		capture.beginFrame(0);
		rts::frame_timing::BindCapture captureBinding(capture);
		const unsigned int beforeScopes = diagnosticClockCalls;
		for (std::size_t phase = 0; phase < sizeof(phases) / sizeof(phases[0]); ++phase)
		{
			rts::frame_timing::Scope timing(phases[phase]);
			timing.finish();
			timing.finish();
		}
		check(diagnosticClockCalls == beforeScopes,
			"disabled production volume-particle scopes do not query the clock");
		capture.endFrame(900);
		capture.endSession();
	}
	check(diagnosticClockCalls == disabledClockCalls,
		"disabled volume-particle capture remains clock-free");
	check(files(directory).empty(), "disabled volume-particle scopes create no output");

	SetEnvironmentVariableA("RTS_FRAME_TIMING_DIR", directory.c_str());
	{
		rts::frame_timing::Capture capture;
		capture.beginSession("headless");
		capture.beginFrame(0);
		rts::frame_timing::BindCapture captureBinding(capture);
		{
			rts::frame_timing::Scope timing(phases[0]);
			timing.finish();
			const unsigned int finishedAt = diagnosticClockCalls;
			timing.finish();
			check(diagnosticClockCalls == finishedAt,
				"volume-particle whole-call timing finishes exactly once");
		}
		for (int layer = 0; layer < 3; ++layer)
		{
			for (std::size_t phase = 1; phase < sizeof(phases) / sizeof(phases[0]); ++phase)
			{
				rts::frame_timing::Scope timing(phases[phase]);
				timing.finish();
				const unsigned int finishedAt = diagnosticClockCalls;
				timing.finish();
				check(diagnosticClockCalls == finishedAt,
					"volume-particle layer timing finishes exactly once");
			}
		}
		capture.endFrame(900);
		capture.endSession();
	}
	const std::vector<Row> data = rows(directory);
	const char *names[] = {
		"renderer_volume_particle", "renderer_volume_particle_transform",
		"renderer_volume_particle_update_arrays", "renderer_volume_particle_pack_submit"
	};
	const unsigned int samples[] = { 1, 3, 3, 3 };
	check(data.size() == 5, "frame plus four volume-particle phases retain the CSV schema");
	if (data.size() == 5)
		for (std::size_t phase = 0; phase < sizeof(names) / sizeof(names[0]); ++phase)
			check(strcmp(data[phase + 1].phase, names[phase]) == 0 &&
				data[phase + 1].samples == samples[phase],
				"volume-particle phase name and whole-call/layer counts");
}

void volumeShadowStages(const std::string& directory)
{
	const rts::frame_timing::Phase phases[] = {
		rts::frame_timing::RendererVolumeStaticDraw,
		rts::frame_timing::RendererVolumeDynamicDraw,
		rts::frame_timing::RendererVolumeDynamicUpload,
		rts::frame_timing::RendererVolumeDynamicCommands
	};
	SetEnvironmentVariableA("RTS_FRAME_TIMING_DIR", NULL);
	const unsigned int beforeDisabled = diagnosticClockCalls;
	{
		rts::frame_timing::Capture capture;
		rts::frame_timing::BindCapture binding(capture);
		capture.beginSession("headless");
		capture.beginFrame(0);
		for (std::size_t phase = 0; phase < sizeof(phases) / sizeof(phases[0]); ++phase)
		{
			rts::frame_timing::Scope timing(phases[phase]);
			timing.finish();
			timing.finish();
		}
		capture.endFrame(1);
		capture.endSession();
	}
	check(diagnosticClockCalls == beforeDisabled,
		"disabled production volume-shadow stages remain clock-free");
	check(files(directory).empty(), "disabled volume-shadow stages create no output");
	SetEnvironmentVariableA("RTS_FRAME_TIMING_DIR", directory.c_str());
	{
		rts::frame_timing::Capture capture;
		rts::frame_timing::BindCapture binding(capture);
		capture.beginSession("headless");
		capture.beginFrame(0);
		for (int pass = 0; pass < 2; ++pass)
		{
			rts::frame_timing::Scope timing(phases[0]);
			timing.finish();
			const unsigned int finishedAt = diagnosticClockCalls;
			timing.finish();
			check(diagnosticClockCalls == finishedAt, "static volume timing finishes once");
		}
		for (int attempt = 0; attempt < 3; ++attempt)
		{
			rts::frame_timing::Scope dynamicTiming(phases[1]);
			{
				rts::frame_timing::Scope uploadTiming(phases[2]);
				uploadTiming.finish();
				const unsigned int finishedAt = diagnosticClockCalls;
				uploadTiming.finish();
				check(diagnosticClockCalls == finishedAt, "dynamic upload timing finishes once");
			}
			// Model an admitted upload failure: no command phase follows.
			if (attempt < 2)
			{
				rts::frame_timing::Scope commandTiming(phases[3]);
				commandTiming.finish();
				const unsigned int finishedAt = diagnosticClockCalls;
				commandTiming.finish();
				check(diagnosticClockCalls == finishedAt, "dynamic command timing finishes once");
			}
			dynamicTiming.finish();
			const unsigned int finishedAt = diagnosticClockCalls;
			dynamicTiming.finish();
			check(diagnosticClockCalls == finishedAt, "dynamic volume timing finishes once");
		}
		capture.endFrame(900);
		capture.endSession();
	}
	const std::vector<Row> data = rows(directory);
	const char *names[] = { "renderer_volume_static_draw", "renderer_volume_dynamic_draw",
		"renderer_volume_dynamic_upload", "renderer_volume_dynamic_commands" };
	const unsigned int samples[] = { 2, 3, 3, 2 };
	check(data.size() == 5, "frame plus four volume-shadow stages retain the CSV schema");
	if (data.size() == 5)
		for (std::size_t phase = 0; phase < sizeof(names) / sizeof(names[0]); ++phase)
			check(strcmp(data[phase + 1].phase, names[phase]) == 0 &&
				data[phase + 1].samples == samples[phase],
				"volume-shadow stage name and admitted attempt counts");
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
		capture.add(rts::frame_timing::WaterTrackTextureBind,
			frequency / 1000);
		for (int module = 0; module < 3; ++module)
			capture.add(rts::frame_timing::WaterTrackModuleRender,
				frequency / 2000);
		const rts::frame_timing::Phase instrumentedPhases[] = {
			rts::frame_timing::RendererConstantPack,
			rts::frame_timing::RendererConstantUpload,
			rts::frame_timing::RendererBufferUpload,
			rts::frame_timing::RendererDrawValidation,
			rts::frame_timing::RendererDrawSubmit,
			rts::frame_timing::RendererBufferShadow,
			rts::frame_timing::RendererSceneLights,
			rts::frame_timing::RendererProjectedShadows,
			rts::frame_timing::RendererVolumeShadows,
			rts::frame_timing::RendererSorting,
			rts::frame_timing::RendererParticles,
			rts::frame_timing::RendererSkinRender,
			rts::frame_timing::RendererCommandEnqueue,
			rts::frame_timing::RendererVolumePrepare,
			rts::frame_timing::RendererVolumeSubmit,
			rts::frame_timing::RendererProjectedTerrain,
			rts::frame_timing::RendererProjectedDecal,
			rts::frame_timing::RendererProjectedFlush,
			rts::frame_timing::RendererParticlePrepare,
			rts::frame_timing::RendererParticleSubmit,
			rts::frame_timing::RendererProjectedSceneMeshDrain,
			rts::frame_timing::RendererTextureOwnerDrain,
			rts::frame_timing::RendererTextureCopyPublication,
			rts::frame_timing::ClientDrawableSweep,
			rts::frame_timing::RendererWW3DSync,
			rts::frame_timing::RendererW3DViewUpdate,
			rts::frame_timing::RendererShroudSourceSync,
			rts::frame_timing::RendererSceneCustomizedRender,
			rts::frame_timing::RendererSceneFlush
		};
		for (std::size_t phase = 0; phase < sizeof(instrumentedPhases) / sizeof(instrumentedPhases[0]); ++phase)
		{
			unsigned int finishedAt = 0;
			{
				rts::frame_timing::Scope timing(capture, instrumentedPhases[phase]);
				timing.finish();
				finishedAt = diagnosticClockCalls;
				timing.finish();
				check(diagnosticClockCalls == finishedAt, "repeated finish does not query the clock");
			}
			check(diagnosticClockCalls == finishedAt, "destruction after finish does not query the clock");
		}
		capture.endFrame(1000); // Forces the headless periodic bucket without sleeping.
		std::vector<Row> data = rows(directory);
		check(data.size() == 44, "periodic flush writes existing phases and all twenty-nine renderer/client phases before session ends");
		if (data.size() == 44)
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
			check(strcmp(data[13].phase, "water_track_texture_bind") == 0 &&
				data[13].samples == 1, "water track texture bind phase and sample count");
			check(strcmp(data[14].phase, "water_track_module_render") == 0 &&
				data[14].samples == 3, "water track module count and phase name");
			const char *instrumentedNames[] = {
				"renderer_constant_pack", "renderer_constant_upload", "renderer_buffer_upload",
				"renderer_draw_validation", "renderer_draw_submit", "renderer_buffer_shadow",
				"renderer_scene_lights", "renderer_projected_shadows", "renderer_volume_shadows",
				"renderer_sorting", "renderer_particles", "renderer_skin_render",
				"renderer_command_enqueue", "renderer_volume_prepare", "renderer_volume_submit",
				"renderer_projected_terrain", "renderer_projected_decal", "renderer_projected_flush",
				"renderer_particle_prepare", "renderer_particle_submit",
				"renderer_projected_scene_mesh_drain", "renderer_texture_owner_drain",
				"renderer_texture_copy_publication", "client_drawable_sweep",
				"renderer_ww3d_sync", "renderer_w3d_view_update", "renderer_shroud_source_sync",
				"renderer_scene_customized_render", "renderer_scene_flush"
			};
			for (std::size_t phase = 0; phase < sizeof(instrumentedNames) / sizeof(instrumentedNames[0]); ++phase)
				check(strcmp(data[phase + 15].phase, instrumentedNames[phase]) == 0 &&
					data[phase + 15].samples == 1, "instrumented phase names and finished scopes emit exactly one sample");
		}
		capture.beginFrame(1000);
		capture.endFrame(1005);
		capture.beginFrame(1005);
		capture.endFrame(0); // Game teardown can reset GameLogic before EndFrame.
		capture.endSession();
		data = rows(directory);
		check(data.size() == 45 && data.back().frames == 5 &&
			data.back().first == 1000 && data.back().last == 1005,
			"session end preserves the final pre-reset frame range");
		capture.beginSession("interactive");
		capture.beginFrame(0);
		capture.endFrame(1);
		// Destructor must retain this final partial bucket without endSession.
	}
	const std::vector<Row> data = rows(directory);
	check(data.size() == 46 && data.back().session == 2 && data.back().frames == 1 &&
		strcmp(data.back().mode, "interactive") == 0, "destructor/session reset retains only new frame counts");
}

void publishedSingletonGate(const std::string& directory)
{
	SetEnvironmentVariableA("RTS_FRAME_TIMING_DIR", directory.c_str());
	rts::frame_timing::Capture& capture = rts::frame_timing::Capture::instance();
	check(!rts::frame_timing::IsActive(), "published singleton is inactive before its frame");
	capture.beginSession("headless");
	capture.beginFrame(0);
	check(rts::frame_timing::IsActive(), "producer frame activates the published singleton gate");
	capture.add(rts::frame_timing::Logic, 100);
	capture.endFrame(900);
	check(!rts::frame_timing::IsActive(), "ending the producer frame clears the singleton gate");
	capture.endSession();
	const rts::frame_timing::FinalizedCapture final = capture.finalize();
	check(final.complete, "published singleton gate retains complete frame evidence");
	const std::vector<Row> data = rows(directory);
	check(data.size() == 2 && strcmp(data[0].phase, "frame") == 0 &&
		strcmp(data[1].phase, "logic") == 0,
		"published singleton retains only the ordinary frame and logic rows");
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
		const rts::frame_timing::FinalizedCapture final = capture.finalize();
		check(final.closed && final.truncated && !final.complete,
			"row-limited capture is closed but cannot qualify as complete evidence");
	}
	check(rows(directory).size() == 16384, "capture stops at the fixed row bound");
}

void finalized(const std::string& directory)
{
	SetEnvironmentVariableA("RTS_FRAME_TIMING_DIR", directory.c_str());
	rts::frame_timing::Capture capture;
	capture.beginSession("headless");
	capture.beginFrame(0);
	capture.endFrame(1);
	capture.beginFrame(1);
	capture.endFrame(2);
	capture.beginFrame(2);
	capture.endFrame(0);
	capture.endSession();
	const rts::frame_timing::FinalizedCapture final = capture.finalize();
	const std::vector<std::string> paths = files(directory);
	check(paths.size() == 1 && final.path == paths[0],
		"finalization identifies the exact producer-owned file");
	check(final.closed && final.writeSucceeded && !final.truncated && final.complete,
		"complete finalization proves successful file close");
	check(final.sessionCount == 1 && final.frameSamples == 3 &&
		final.firstFrame == 0 && final.lastFrame == 2,
		"finalization retains pre-reset frame coverage");
	HANDLE exclusive = CreateFileA(final.path.c_str(), GENERIC_READ | GENERIC_WRITE,
		0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	check(exclusive != INVALID_HANDLE_VALUE,
		"receipt file is physically closed, not merely flushed");
	if (exclusive != INVALID_HANDLE_VALUE) CloseHandle(exclusive);
	capture.beginSession("headless");
	capture.beginFrame(100);
	capture.endFrame(101);
	const rts::frame_timing::FinalizedCapture again = capture.finalize();
	check(again.path == final.path && again.frameSamples == 3 && again.complete,
		"finalization is idempotent and prevents later session writes");
}

void incomplete(const std::string& directory)
{
	SetEnvironmentVariableA("RTS_FRAME_TIMING_DIR", directory.c_str());
	rts::frame_timing::Capture capture;
	capture.beginSession("headless");
	capture.beginFrame(0);
	capture.endFrame(1);
	capture.beginFrame(1);
	capture.endSession();
	const rts::frame_timing::FinalizedCapture final = capture.finalize();
	check(final.closed && final.writeSucceeded && !final.complete,
		"unfinished frame fails coverage despite a successful close");
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
	const std::string finalizedDir = root + "\\finalized", incompleteDir = root + "\\incomplete";
	const std::string singletonDir = root + "\\singleton-gate";
	const std::string audioDir = root + "\\audio-stages";
	const std::string volumeParticleDir = root + "\\volume-particle-stages";
	const std::string volumeShadowDir = root + "\\volume-shadow-stages";
	check(CreateDirectoryA(disabledDir.c_str(), NULL) != FALSE, "create disabled case");
	check(CreateDirectoryA(enabledDir.c_str(), NULL) != FALSE, "create enabled case");
	check(CreateDirectoryA(boundedDir.c_str(), NULL) != FALSE, "create bounded case");
	check(CreateDirectoryA(finalizedDir.c_str(), NULL) != FALSE, "create finalized case");
	check(CreateDirectoryA(incompleteDir.c_str(), NULL) != FALSE, "create incomplete case");
	check(CreateDirectoryA(singletonDir.c_str(), NULL) != FALSE, "create singleton gate case");
	check(CreateDirectoryA(audioDir.c_str(), NULL) != FALSE, "create audio stages case");
	check(CreateDirectoryA(volumeParticleDir.c_str(), NULL) != FALSE, "create volume-particle stages case");
	check(CreateDirectoryA(volumeShadowDir.c_str(), NULL) != FALSE, "create volume-shadow stages case");
	LARGE_INTEGER frequency;
	if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0)
		return 1;
	inactiveSingleton(disabledDir);
	disabled(disabledDir);
	enabled(enabledDir, frequency.QuadPart);
	audioStages(audioDir);
	volumeParticleStages(volumeParticleDir);
	volumeShadowStages(volumeShadowDir);
	publishedSingletonGate(singletonDir);
	bounded(boundedDir);
	finalized(finalizedDir);
	incomplete(incompleteDir);
	SetEnvironmentVariableA("RTS_FRAME_TIMING_DIR", NULL);
	removeCase(disabledDir);
	removeCase(enabledDir);
	removeCase(boundedDir);
	removeCase(finalizedDir);
	removeCase(incompleteDir);
	removeCase(singletonDir);
	removeCase(audioDir);
	removeCase(volumeParticleDir);
	removeCase(volumeShadowDir);
	check(RemoveDirectoryA(root.c_str()) != FALSE, "remove empty test root");
	return failures ? 1 : 0;
}
