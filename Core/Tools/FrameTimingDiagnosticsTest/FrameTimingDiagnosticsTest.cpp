#include <windows.h>

namespace
{
unsigned int diagnosticClockCalls = 0;
BOOL WINAPI CountDiagnosticClockCalls(LARGE_INTEGER *value)
{
	++diagnosticClockCalls;
	return QueryPerformanceCounter(value);
}
}

// Count the real header's clock calls without changing its production clock.
#define QueryPerformanceCounter CountDiagnosticClockCalls
#include "Lib/FrameTimingDiagnostics.h"
#undef QueryPerformanceCounter

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
	__int64 bucketBegin, bucketEnd, frequency;
};

std::vector<Row> rows(const std::string& directory, bool bucketClockAnchors = false)
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
	check((strstr(line, ",bucket_begin_qpc,bucket_end_qpc,qpc_frequency") != NULL) == bucketClockAnchors,
		"CSV bucket clock anchors require explicit diagnostic opt-in");
	const int expectedFields = bucketClockAnchors ? 18 : 15;
	while (fgets(line, sizeof(line), file))
	{
		Row row = {};
		const int fields = sscanf(line, bucketClockAnchors ?
			"%u,%31[^,],%u,%u,%u,%lf,%31[^,],%u,%lf,%lf,%lf,%lf,%lf,%u,%u,%I64d,%I64d,%I64d" :
			"%u,%31[^,],%u,%u,%u,%lf,%31[^,],%u,%lf,%lf,%lf,%lf,%lf,%u,%u",
			&row.session, row.mode, &row.first, &row.last, &row.frames, &row.wall, row.phase, &row.samples,
			&row.total, &row.average, &row.p95, &row.p99, &row.maximum, &row.over33, &row.over100,
			&row.bucketBegin, &row.bucketEnd, &row.frequency);
		int columnCount = 1;
		for (const char *column = line; *column; ++column)
			if (*column == ',') ++columnCount;
		check(fields == expectedFields && columnCount == expectedFields,
			"every CSV row has exactly the documented fields for its format");
		if (fields == expectedFields && columnCount == expectedFields)
		{
			if (bucketClockAnchors)
				check(row.bucketBegin > 0 && row.bucketEnd >= row.bucketBegin && row.frequency > 0,
				"every bucket has ordered QPC anchors and a positive clock frequency");
			if (row.frequency > 0)
			{
				const double wallDifference = static_cast<double>(row.bucketEnd - row.bucketBegin) *
					1000.0 / row.frequency - row.wall;
				check(wallDifference >= -0.001 && wallDifference <= 0.001,
					"bucket clock duration agrees with rounded wall_ms");
			}
			result.push_back(row);
		}
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
	{
		rts::frame_timing::Capture capture;
		capture.beginSession("headless");
		capture.beginFrame(0);
		const unsigned int before = diagnosticClockCalls;
		{
			rts::frame_timing::ConditionalScope gated(capture,
				rts::frame_timing::ClientDisplayDraw, false);
			gated.finish();
			rts::frame_timing::ConditionalScope inactive(capture,
				rts::frame_timing::DisplayMainRender, true);
			inactive.finish();
			rts::frame_timing::ConditionalScope producerWait(
				rts::frame_timing::RenderProducerWait, rts::frame_timing::IsActive());
			producerWait.finish();
		}
		check(diagnosticClockCalls == before,
			"disabled and inactive conditional scopes do not query the clock");
		capture.add(rts::frame_timing::Logic, 100);
		capture.endFrame(900);
		capture.endSession();
		check(!capture.isActive(), "disabled capture remains inactive");
		check(!capture.finalize().complete,
			"disabled capture cannot provide complete receipt evidence");
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
		capture.add(rts::frame_timing::WaterTrackTextureBind,
			frequency / 1000);
		for (int module = 0; module < 3; ++module)
			capture.add(rts::frame_timing::WaterTrackModuleRender,
				frequency / 2000);
		capture.endFrame(1000); // Forces the headless periodic bucket without sleeping.
		std::vector<Row> data = rows(directory);
		check(data.size() == 15, "periodic flush writes frame, logic, simulation, and water-track phases before session ends");
		if (data.size() == 15)
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
		}
		capture.beginFrame(1000);
		capture.endFrame(1005);
		capture.beginFrame(1005);
		capture.endFrame(0); // Game teardown can reset GameLogic before EndFrame.
		capture.endSession();
		data = rows(directory);
		check(data.size() == 16 && data.back().frames == 5 &&
			data.back().first == 1000 && data.back().last == 1005,
			"session end preserves the final pre-reset frame range");
		capture.beginSession("interactive");
		capture.beginFrame(0);
		capture.endFrame(1);
		// Destructor must retain this final partial bucket without endSession.
	}
	const std::vector<Row> data = rows(directory);
	check(data.size() == 17 && data.back().session == 2 && data.back().frames == 1 &&
		strcmp(data.back().mode, "interactive") == 0, "destructor/session reset retains only new frame counts");
}

void clientDisplayPhases(const std::string& directory)
{
	SetEnvironmentVariableA("RTS_FRAME_TIMING_DIR", directory.c_str());
	SetEnvironmentVariableA("RTS_FRAME_TIMING_BUCKET_ANCHORS", "1");
	rts::frame_timing::Capture& capture = rts::frame_timing::Capture::instance();
	SetEnvironmentVariableA("RTS_FRAME_TIMING_BUCKET_ANCHORS", NULL);
	check(!rts::frame_timing::IsActive(), "published singleton is inactive before its frame");
	capture.beginSession("headless");
	capture.beginFrame(0);
	check(rts::frame_timing::IsActive(), "producer frame activates the published singleton gate");
	const rts::frame_timing::Phase phases[] = {
		rts::frame_timing::ClientDrawables, rts::frame_timing::ClientTerrainVisual,
		rts::frame_timing::ClientDisplayUpdate, rts::frame_timing::ClientDisplayDraw,
		rts::frame_timing::DisplayPreframe, rts::frame_timing::DisplayViews,
		rts::frame_timing::DisplayRtt, rts::frame_timing::DisplayBeginRender,
		rts::frame_timing::DisplayMainRender, rts::frame_timing::DisplayEndRender,
		rts::frame_timing::RenderProducerWait,
		rts::frame_timing::ViewScene3D, rts::frame_timing::ViewScene2D,
		rts::frame_timing::NativeSortingFlush, rts::frame_timing::NativeSkinRender,
		rts::frame_timing::HeightMapRender, rts::frame_timing::NativeRigidBatchRender,
		rts::frame_timing::WaterRender,
		rts::frame_timing::SceneObjectSubmit, rts::frame_timing::SceneShadowPrepare,
		rts::frame_timing::SceneShadowRender, rts::frame_timing::SceneTrees,
		rts::frame_timing::SceneParticleSubmit, rts::frame_timing::SceneOcclusion,
		rts::frame_timing::SceneTranslucent, rts::frame_timing::SceneFlush,
		rts::frame_timing::SceneMeshFlush, rts::frame_timing::SceneStaticSort,
		rts::frame_timing::SceneProjectedShadows, rts::frame_timing::SceneVolumeShadows,
		rts::frame_timing::SmudgeRender, rts::frame_timing::SmudgeColorCopy,
		rts::frame_timing::ParticleVisibleBounds, rts::frame_timing::ParticleOwnerCapture,
		rts::frame_timing::ParticlePrepareJoin, rts::frame_timing::ParticleCompact,
		rts::frame_timing::ParticleSerialGather, rts::frame_timing::ParticleTextureLookup,
		rts::frame_timing::ParticlePointSubmit, rts::frame_timing::ParticleVolumeSubmit,
		rts::frame_timing::ParticleStreakSubmit, rts::frame_timing::ParticleSnowSubmit,
		rts::frame_timing::ParticleSmudgeSubmit, rts::frame_timing::PointCenterTransform,
		rts::frame_timing::PointUpdateArrays, rts::frame_timing::PointPackedVertexFill,
		rts::frame_timing::NativeSortingQueue,
		rts::frame_timing::PointInputPrepare, rts::frame_timing::PointTransformSetup,
		rts::frame_timing::PointMaterialApply, rts::frame_timing::PointShaderApply,
		rts::frame_timing::PointTextureApply, rts::frame_timing::PointChunkLifetime,
		rts::frame_timing::PointVBAcquire, rts::frame_timing::PointVBLock,
		rts::frame_timing::PointVBCommit, rts::frame_timing::PointIBBind,
		rts::frame_timing::PointVBBind, rts::frame_timing::PointDrawSubmit,
		rts::frame_timing::PointTransformRestore,
		rts::frame_timing::NativeSortingNodeOrder, rts::frame_timing::NativeSortingPrepare,
		rts::frame_timing::NativeSortingTriangleSort, rts::frame_timing::NativeSortingChunkWork,
		rts::frame_timing::NativeSortingOffsetReset, rts::frame_timing::NativeSortingSubmitChunk
	};
	const char *names[] = {
		"client_drawables", "client_terrain_visual", "client_display_update", "client_display_draw",
		"display_preframe", "display_views", "display_rtt", "display_begin_render",
		"display_main_render", "display_end_render", "render_producer_wait",
		"view_scene_3d", "view_scene_2d", "native_sorting_flush", "native_skin_render",
		"height_map_render", "native_rigid_batch_render", "water_render",
		"scene_object_submit", "scene_shadow_prepare", "scene_shadow_render",
		"scene_trees", "scene_particle_submit", "scene_occlusion",
		"scene_translucent", "scene_flush", "scene_mesh_flush", "scene_static_sort",
		"scene_projected_shadows", "scene_volume_shadows", "smudge_render", "smudge_color_copy",
		"particle_visible_bounds", "particle_owner_capture", "particle_prepare_join",
		"particle_compact", "particle_serial_gather", "particle_texture_lookup",
		"particle_point_submit", "particle_volume_submit", "particle_streak_submit",
		"particle_snow_submit", "particle_smudge_submit",
		"point_center_transform", "point_update_arrays", "point_packed_vertex_fill",
		"native_sorting_queue",
		"point_input_prepare", "point_transform_setup", "point_material_apply",
		"point_shader_apply", "point_texture_apply", "point_chunk_lifetime",
		"point_vb_acquire", "point_vb_lock", "point_vb_commit",
		"point_ib_bind", "point_vb_bind", "point_draw_submit", "point_transform_restore",
		"native_sorting_node_order", "native_sorting_prepare", "native_sorting_triangle_sort",
		"native_sorting_chunk_work", "native_sorting_offset_reset", "native_sorting_submit_chunk"
	};
	for (std::size_t phase = 0; phase < sizeof(phases) / sizeof(phases[0]); ++phase)
	{
		const unsigned int before = diagnosticClockCalls;
		{
			rts::frame_timing::ConditionalScope gated(capture, phases[phase], false);
			gated.finish();
		}
		check(diagnosticClockCalls == before,
			"false gate makes no clock query even inside an active capture");
		{
			rts::frame_timing::ConditionalScope active(phases[phase], true);
			active.finish();
			active.finish();
		}
		check(diagnosticClockCalls == before + 2,
			"conditional phase begins and finishes once despite explicit finish and destructor");
	}
	capture.endFrame(900);
	check(!rts::frame_timing::IsActive(), "ending the producer frame clears the singleton gate");
	capture.endSession();
	const rts::frame_timing::FinalizedCapture final = capture.finalize();
	check(final.complete, "child diagnostic phases retain complete frame evidence");
	const std::vector<Row> data = rows(directory, true);
	check(data.size() == 67, "frame plus all sixty-six child diagnostic phases are emitted");
	if (data.size() == 67)
	{
		for (std::size_t row = 1; row < data.size(); ++row)
			check(data[row].bucketBegin == data[0].bucketBegin &&
				data[row].bucketEnd == data[0].bucketEnd && data[row].frequency == data[0].frequency,
				"all phases in one bucket share identical clock anchors");
		for (std::size_t phase = 0; phase < sizeof(phases) / sizeof(phases[0]); ++phase)
			check(strcmp(data[phase + 1].phase, names[phase]) == 0 &&
				data[phase + 1].samples == 1, "child diagnostic phase name and exactly one sample");
	}
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
	SetEnvironmentVariableA("RTS_FRAME_TIMING_BUCKET_ANCHORS", NULL);
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
	const std::string clientDisplayDir = root + "\\client-display";
	check(CreateDirectoryA(disabledDir.c_str(), NULL) != FALSE, "create disabled case");
	check(CreateDirectoryA(enabledDir.c_str(), NULL) != FALSE, "create enabled case");
	check(CreateDirectoryA(boundedDir.c_str(), NULL) != FALSE, "create bounded case");
	check(CreateDirectoryA(finalizedDir.c_str(), NULL) != FALSE, "create finalized case");
	check(CreateDirectoryA(incompleteDir.c_str(), NULL) != FALSE, "create incomplete case");
	check(CreateDirectoryA(clientDisplayDir.c_str(), NULL) != FALSE, "create client/display case");
	LARGE_INTEGER frequency;
	if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0)
		return 1;
	inactiveSingleton(disabledDir);
	disabled(disabledDir);
	enabled(enabledDir, frequency.QuadPart);
	clientDisplayPhases(clientDisplayDir);
	bounded(boundedDir);
	finalized(finalizedDir);
	incomplete(incompleteDir);
	SetEnvironmentVariableA("RTS_FRAME_TIMING_DIR", NULL);
	removeCase(disabledDir);
	removeCase(enabledDir);
	removeCase(boundedDir);
	removeCase(finalizedDir);
	removeCase(incompleteDir);
	removeCase(clientDisplayDir);
	check(RemoveDirectoryA(root.c_str()) != FALSE, "remove empty test root");
	return failures ? 1 : 0;
}
