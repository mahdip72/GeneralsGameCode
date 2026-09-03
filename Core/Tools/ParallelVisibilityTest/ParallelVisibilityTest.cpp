/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2025 Electronic Arts Inc.
** SPDX-License-Identifier: GPL-3.0-or-later
*/

#include "Lib/ParallelVisibility.h"
#include "Lib/PipelineExecutionPolicy.h"
#include "../TestSupport/LocalCapacityTestLane.h"

#include <stdio.h>
#include <string.h>
#include <limits>
#include <vector>

#if !defined(_MSC_VER) || _MSC_VER >= 1300
#include <atomic>
#include <chrono>
#include <thread>
#define VISIBILITY_TEST_MODERN 1
#if defined(_WIN32)
#include <float.h>
#include <xmmintrin.h>
#endif
#endif

#if defined(RTS_BUILD_CORE_EXTRAS)
extern "C" void rts_parallel_visibility_set_test_fault(unsigned fault, unsigned occurrence);
extern "C" void rts_parallel_visibility_set_test_observer(void (*observer)());
extern "C" void rts_job_system_set_test_fault(unsigned fault, unsigned occurrence);
#endif

namespace
{
using namespace rts;
unsigned failures = 0;

#define CHECK(condition) do { if (!(condition)) { ++failures; \
	fprintf(stderr, "line %u: %s\n", static_cast<unsigned>(__LINE__), #condition); } } while (0)

VisibilityFrame makeFrame(bool zeroHour, bool reflection, bool markers)
{
	VisibilityFrame frame;
	frame.resetHiddenDrawableFlags = zeroHour;
	frame.translucentOcclusion = zeroHour;
	frame.reflection = reflection;
	frame.behindBuildingMarkers = markers;
	frame.planes[0].z = -1.0f; frame.planes[0].distance = -1.0f;
	frame.planes[1].y = -1.0f; frame.planes[1].distance = 100.0f;
	frame.planes[2].x = 1.0f; frame.planes[2].distance = 100.0f;
	frame.planes[3].y = 1.0f; frame.planes[3].distance = 100.0f;
	frame.planes[4].x = -1.0f; frame.planes[4].distance = 100.0f;
	frame.planes[5].z = 1.0f; frame.planes[5].distance = 1000.0f;
	return frame;
}

// Independent copy of the live CollisionMath plane/sphere overlap rule.
bool legacyInside(const VisibilityFrame &frame, const VisibilityInput &input)
{
	for (unsigned p = 0; p != 6; ++p)
	{
		float dot = input.x * frame.planes[p].x + input.y * frame.planes[p].y +
			input.z * frame.planes[p].z;
		float distance = dot - frame.planes[p].distance;
		unsigned overlap = 3;
		if (distance > input.radius) overlap = 1;
		else if (distance < -input.radius) overlap = 2;
		if (overlap == 1) return false;
	}
	return true;
}

struct Published
{
	bool visible;
	unsigned flags;
	unsigned lists;
};

// Original owner-loop semantics, independent of the kernel and planner. Initial
// flags intentionally contain stale bits to catch unintended flag clearing.
Published legacyPublish(const VisibilityFrame &frame, const VisibilityInput &input,
	VisibilityListBudget &remaining)
{
	Published value = { false, 0x55, 0 };
	const bool hasDrawable = (input.flags & VIS_INPUT_DRAWABLE) != 0;
	if ((input.flags & VIS_INPUT_FORCE_VISIBLE) != 0) { value.visible = true; return value; }
	if (frame.reflection)
	{
		value.visible = (!hasDrawable || (input.flags & VIS_INPUT_MIRROR) != 0) && legacyInside(frame, input);
		return value;
	}
	if ((input.flags & VIS_INPUT_HIDDEN) != 0) return value;
	value.visible = legacyInside(frame, input);
	if (!value.visible || !hasDrawable) return value;
	if ((input.flags & (VIS_INPUT_EFFECTIVELY_HIDDEN | VIS_INPUT_SHROUDED)) != 0)
	{
		value.visible = false;
		if (frame.resetHiddenDrawableFlags) value.flags = 0;
		return value;
	}
	value.flags = 0;
	if ((input.flags & VIS_INPUT_TRANSLUCENT) != 0 && remaining.translucent != 0)
	{
		--remaining.translucent;
		value.flags = VIS_LIST_TRANSLUCENT;
		value.lists |= VIS_LIST_TRANSLUCENT;
		if (!frame.translucentOcclusion) return value;
	}
	if (frame.behindBuildingMarkers)
	{
		bool translucent = (value.flags & VIS_LIST_TRANSLUCENT) != 0;
		if ((input.flags & VIS_INPUT_STRUCTURE) != 0 && remaining.occluders != 0)
		{
			if (!translucent) { --remaining.occluders; value.lists |= VIS_LIST_OCCLUDER; }
			value.flags |= VIS_LIST_OCCLUDER;
		}
		else if ((input.flags & VIS_INPUT_OCCLUDEE) != 0 && remaining.occludees != 0)
		{
			--remaining.occludees;
			value.lists |= VIS_LIST_OCCLUDEE;
			value.flags |= VIS_LIST_OCCLUDEE;
		}
		else if (value.flags == 0 && remaining.others != 0)
		{
			--remaining.others;
			value.lists |= VIS_LIST_OTHER;
			value.flags |= VIS_LIST_OTHER;
		}
	}
	return value;
}

void fillFixture(VisibilityInput *inputs, unsigned count)
{
	for (unsigned i = 0; i < count; ++i)
	{
		// Exhaust all 512 flag combinations in a visible cluster, then include
		// camera-edge, large-radius, far-away and ghost/non-drawable objects.
		inputs[i].flags = i % 512;
		inputs[i].x = i < 512 ? 0.0f : static_cast<float>(static_cast<int>((i * 17) % 281) - 140);
		inputs[i].y = i < 512 ? 0.0f : static_cast<float>(static_cast<int>((i * 29) % 239) - 119);
		inputs[i].z = i < 512 ? 50.0f : static_cast<float>((i * 37) % 1400);
		inputs[i].radius = static_cast<float>((i * 13) % 41);
	}
}

void checkPrepared(const ParallelVisibilityWorkspace &workspace, const VisibilityFrame &frame,
	const std::vector<VisibilityInput> &inputs)
{
	const unsigned limits[] = { 0, 1, 7, 1024 };
	for (unsigned limit = 0; limit < 4; ++limit)
	{
		VisibilityListBudget expectedRemaining;
		expectedRemaining.translucent = limits[limit];
		expectedRemaining.occluders = limits[(limit + 1) % 4];
		expectedRemaining.occludees = limits[(limit + 2) % 4];
		expectedRemaining.others = limits[(limit + 3) % 4];
		VisibilityListBudget actualRemaining = expectedRemaining;
		std::vector<unsigned> expectedLists[4], actualLists[4];
		for (unsigned i = 0; i < inputs.size(); ++i)
		{
			const Published expected = legacyPublish(frame, inputs[i], expectedRemaining);
			const VisibilityResult &result = workspace.results()[i];
			const VisibilityPublication publication = PlanVisibilityPublication(frame, result, actualRemaining);
			const unsigned actualFlags = (result.flags & VIS_RESULT_RESET_DRAW_FLAGS) != 0 ? publication.drawFlags : 0x55;
			CHECK(((result.flags & VIS_RESULT_VISIBLE) != 0) == expected.visible);
			CHECK(actualFlags == expected.flags);
			CHECK(publication.lists == expected.lists);
			for (unsigned list = 0; list < 4; ++list)
			{
				if ((expected.lists & (1u << list)) != 0) expectedLists[list].push_back(i);
				if ((publication.lists & (1u << list)) != 0) actualLists[list].push_back(i);
			}
		}
		CHECK(expectedRemaining.translucent == actualRemaining.translucent);
		CHECK(expectedRemaining.occluders == actualRemaining.occluders);
		CHECK(expectedRemaining.occludees == actualRemaining.occludees);
		CHECK(expectedRemaining.others == actualRemaining.others);
		for (unsigned list = 0; list < 4; ++list) CHECK(expectedLists[list] == actualLists[list]);
	}
}

void checkCase(ParallelVisibilityWorkspace &workspace, unsigned count, const VisibilityFrame &frame)
{
	CHECK(workspace.reserve(count));
	fillFixture(workspace.inputs(), count);
	std::vector<VisibilityInput> inputCopy;
	if (count != 0) inputCopy.assign(workspace.inputs(), workspace.inputs() + count);
	std::vector<VisibilityResult> serial(count);
	if (count != 0) EvaluateVisibilityReference(frame, &inputCopy[0], &serial[0], count);
	CHECK(workspace.prepare(frame, count));
	CHECK(workspace.metrics().objectCount == count);
	CHECK(JobSystem::instance().outstandingJobCount() == 0);
	if (count != 0)
	{
		CHECK(memcmp(workspace.inputs(), &inputCopy[0], sizeof(VisibilityInput) * count) == 0);
		CHECK(memcmp(workspace.results(), &serial[0], sizeof(VisibilityResult) * count) == 0);
	}
	checkPrepared(workspace, frame, inputCopy);
	CHECK(workspace.prepare(frame, count, false));
	CHECK(workspace.metrics().submittedJobs == 0);
	if (count != 0) CHECK(memcmp(workspace.results(), &serial[0], sizeof(VisibilityResult) * count) == 0);
}

void testClipEdges()
{
	const VisibilityFrame frame = makeFrame(false, false, false);
	VisibilityInput input = { 101.0f, 0.0f, 50.0f, 1.0f, 0 };
	CHECK(VisibilitySphereInside(frame, input)); // exactly tangent
	input.x = 101.0001f;
	CHECK(!VisibilitySphereInside(frame, input));
	input.x = -101.0f;
	CHECK(VisibilitySphereInside(frame, input));
	input.x = -101.0001f;
	CHECK(!VisibilitySphereInside(frame, input));
	input.x = 0.0f;
	input.z = 0.0f;
	CHECK(VisibilitySphereInside(frame, input)); // near tangent
	input.z = -0.0001f;
	CHECK(!VisibilitySphereInside(frame, input));
	input.z = 1001.0f;
	CHECK(VisibilitySphereInside(frame, input)); // far tangent
	input.z = 1001.0001f;
	CHECK(!VisibilitySphereInside(frame, input));
	input.x = std::numeric_limits<float>::quiet_NaN();
	CHECK(VisibilitySphereInside(frame, input) == legacyInside(frame, input));
	input.x = std::numeric_limits<float>::infinity();
	CHECK(VisibilitySphereInside(frame, input) == legacyInside(frame, input));
}

#if defined(VISIBILITY_TEST_MODERN) && defined(RTS_BUILD_CORE_EXTRAS)
std::atomic<unsigned> entered(0);
std::atomic<bool> observerTimeout(false);

void scalingObserver()
{
	entered.fetch_add(1);
	const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (entered.load() < 4)
	{
		if (std::chrono::steady_clock::now() > deadline) { observerTimeout.store(true); break; }
		std::this_thread::yield();
	}
}

void testScaling()
{
	JobSystem &system = JobSystem::instance();
	if (system.workerCount() < 4) return;
	ParallelVisibilityWorkspace workspace;
	CHECK(workspace.reserve(65536));
	fillFixture(workspace.inputs(), 65536);
	entered.store(0);
	observerTimeout.store(false);
	rts_parallel_visibility_set_test_observer(scalingObserver);
	CHECK(workspace.prepare(makeFrame(true, false, true), 65536));
	rts_parallel_visibility_set_test_observer(0);
	CHECK(!observerTimeout.load());
	CHECK(workspace.metrics().workerThreadsUsed > 2);
	CHECK(workspace.metrics().submittedJobs > 2);
	CHECK(workspace.metrics().completedJobs == workspace.metrics().submittedJobs);
	fprintf(stdout, "visibility scaling: configured=%u, workers used=%u, ranges=%u\n",
		system.workerCount(), workspace.metrics().workerThreadsUsed, workspace.metrics().submittedJobs);
}

#if defined(_WIN32)
void testFloatingPointState()
{
	// Workers were started before changing the owner. This distinguishes actual
	// owner-state propagation from merely inheriting the same startup defaults.
	ParallelVisibilityWorkspace workspace;
	CHECK(workspace.reserve(8192));
	std::vector<VisibilityResult> expected(8192);
	const unsigned savedMxcsr = _mm_getcsr();
#if !defined(_WIN64)
	const unsigned savedControl = _controlfp(0, 0);
	_controlfp(_PC_24, _MCW_PC);
#endif
	for (unsigned test = 0; test < 3; ++test)
	{
		const unsigned rounding = test == 0 ? _MM_ROUND_UP : _MM_ROUND_NEAREST;
		const unsigned flush = test == 2 ? _MM_FLUSH_ZERO_ON : 0;
		const unsigned ownerMxcsr = (savedMxcsr & ~(_MM_ROUND_MASK | _MM_FLUSH_ZERO_MASK | 0x40u)) | rounding | flush;
#if !defined(_WIN64)
		_controlfp(test == 0 ? _RC_UP : _RC_NEAR, _MCW_RC);
#endif
		_mm_setcsr(ownerMxcsr);
		VisibilityFrame frame;
		frame.planes[0].x = test == 0 ? 1.0f : 0.5f;
		frame.planes[0].y = test == 0 ? 1.0f : 0.0f;
		volatile float one = 1.0f;
		volatile float halfUlp = 0.000000059604644775390625f;
		volatile float smallestNormal = FLT_MIN;
		for (unsigned i = 0; i < 8192; ++i)
		{
			VisibilityInput &input = workspace.inputs()[i];
			input.x = test == 0 ? one : smallestNormal;
			input.y = test == 0 ? halfUlp : 0.0f;
			input.z = 0.0f;
			input.radius = test == 0 ? one : 0.0f;
			input.flags = 0;
		}
		EvaluateVisibilityReference(frame, workspace.inputs(), &expected[0], 8192);
		// RC_UP makes 1 + 2^-24 exceed radius 1; FTZ controls whether the
		// positive denormal dot product is outside a zero-radius sphere.
#if defined(_WIN64)
		CHECK(((expected[0].flags & VIS_RESULT_VISIBLE) != 0) == (test == 2));
#endif
		entered.store(0);
		observerTimeout.store(false);
		rts_parallel_visibility_set_test_observer(scalingObserver);
		CHECK(workspace.prepare(frame, 8192));
		rts_parallel_visibility_set_test_observer(0);
		CHECK(!observerTimeout.load());
		CHECK(workspace.metrics().workerThreadsUsed > 2);
		CHECK(workspace.metrics().serialFallbacks == 0);
		CHECK(memcmp(workspace.results(), &expected[0], sizeof(VisibilityResult) * 8192) == 0);
		CHECK((_mm_getcsr() & ~0x3fu) == (ownerMxcsr & ~0x3fu));
#if !defined(_WIN64)
		CHECK((_controlfp(0, 0) & (_MCW_PC | _MCW_RC)) ==
			(_PC_24 | (test == 0 ? _RC_UP : _RC_NEAR)));
#endif
	}
#if !defined(_WIN64)
	_controlfp(savedControl, _MCW_PC | _MCW_RC);
#endif
	_mm_setcsr(savedMxcsr);
}
#endif
#endif

#if defined(RTS_BUILD_CORE_EXTRAS)
void testFailures()
{
	ParallelVisibilityWorkspace workspace;
	CHECK(workspace.reserve(1024));
	VisibilityInput *original = workspace.inputs();
	const unsigned originalCapacity = workspace.capacity();
	for (unsigned fault = 1; fault <= 2; ++fault)
	{
		rts_parallel_visibility_set_test_fault(fault, 1);
		CHECK(!workspace.reserve(8192));
		CHECK(workspace.inputs() == original);
		CHECK(workspace.capacity() == originalCapacity);
	}
	CHECK(!workspace.reserve(ParallelVisibilityWorkspace::MAX_OBJECTS + 1));
	CHECK(!workspace.prepare(makeFrame(false, false, false), 8192));
	CHECK(workspace.reserve(8192));
	fillFixture(workspace.inputs(), 8192);
	const VisibilityFrame frame = makeFrame(true, false, true);
	std::vector<VisibilityResult> expected(8192);
	EvaluateVisibilityReference(frame, workspace.inputs(), &expected[0], 8192);
#if defined(VISIBILITY_TEST_MODERN)
	for (unsigned fault = 3; fault <= 6; ++fault)
	{
		// Partial job allocation, rejected batch, failed execution and cancellation.
		rts_parallel_visibility_set_test_fault(fault, fault == 3 ? 2 : 1);
		CHECK(workspace.prepare(frame, 8192));
		CHECK(workspace.metrics().serialFallbacks == 1);
		CHECK(memcmp(workspace.results(), &expected[0], sizeof(VisibilityResult) * 8192) == 0);
		CHECK(JobSystem::instance().outstandingJobCount() == 0);
	}
	for (unsigned jobFault = 4; jobFault <= 6; ++jobFault)
	{
		// Real shared-runtime group, record and ready-queue allocation failures.
		rts_job_system_set_test_fault(jobFault, 1);
		CHECK(workspace.prepare(frame, 8192));
		CHECK(workspace.metrics().serialFallbacks == 1);
		CHECK(memcmp(workspace.results(), &expected[0], sizeof(VisibilityResult) * 8192) == 0);
		CHECK(JobSystem::instance().outstandingJobCount() == 0);
	}
#endif
	rts_parallel_visibility_set_test_fault(0, 0);
	rts_job_system_set_test_fault(0, 0);
}
#endif

#if defined(VISIBILITY_TEST_MODERN)
class NestedPrepareJob : public Job
{
public:
	NestedPrepareJob(ParallelVisibilityWorkspace &workspace, bool &succeeded)
		: m_workspace(workspace), m_succeeded(succeeded) {}
	virtual void execute(JobContext &)
	{
		m_succeeded = m_workspace.prepare(makeFrame(false, false, true), 2048);
	}
private:
	ParallelVisibilityWorkspace &m_workspace;
	bool &m_succeeded;
};

void testWorkerFallback()
{
	ParallelVisibilityWorkspace workspace;
	CHECK(workspace.reserve(2048));
	fillFixture(workspace.inputs(), 2048);
	bool succeeded = false;
	JobSystem &system = JobSystem::instance();
	JobGroup group = system.createGroup();
	JobHandle handle = system.trySubmit(new NestedPrepareJob(workspace, succeeded), JOB_PRIORITY_NORMAL, group);
	CHECK(handle.isValid());
	CHECK(system.wait(group));
	CHECK(succeeded);
	CHECK(workspace.metrics().submittedJobs == 0);
	CHECK(workspace.metrics().serialFallbacks == 1);
	CHECK(system.metrics().workerWaitRejectionCount == 0);
}

void testQueuePressure()
{
	JobSystem &system = JobSystem::instance();
	system.shutdown();
	JobSystemConfig config;
	config.workerCount = 4;
	config.queueCapacity = 1;
	config.scratchBytesPerWorker = 4096;
	CHECK(system.start(config));
	ParallelVisibilityWorkspace workspace;
	CHECK(workspace.reserve(8192));
	fillFixture(workspace.inputs(), 8192);
	std::vector<VisibilityResult> expected(8192);
	const VisibilityFrame frame = makeFrame(true, false, true);
	EvaluateVisibilityReference(frame, workspace.inputs(), &expected[0], 8192);
	CHECK(workspace.prepare(frame, 8192));
	CHECK(workspace.metrics().submittedJobs == 0);
	CHECK(workspace.metrics().serialFallbacks == 1);
	CHECK(memcmp(workspace.results(), &expected[0], sizeof(VisibilityResult) * 8192) == 0);
	CHECK(system.outstandingJobCount() == 0);
	system.shutdown();
}
#endif

int testSerialPipelinePolicy()
{
	CHECK(SetPipelineExecutionMode(PIPELINE_EXECUTION_SERIAL));
	CHECK(!UseParallelPipelines());
	SetParallelVisibilityEnabled(true);
	CHECK(!IsParallelVisibilityEnabled());
	ParallelVisibilityWorkspace workspace;
	CHECK(workspace.reserve(8192));
	fillFixture(workspace.inputs(), 8192);
	std::vector<VisibilityResult> expected(8192);
	const VisibilityFrame frame = makeFrame(true, false, true);
	EvaluateVisibilityReference(frame, workspace.inputs(), &expected[0], 8192);
	CHECK(workspace.prepare(frame, 8192));
	CHECK(workspace.metrics().submittedJobs == 0);
	CHECK(workspace.metrics().serialFallbacks == 0);
	CHECK(!JobSystem::instance().isRunning());
	CHECK(memcmp(workspace.results(), &expected[0], sizeof(VisibilityResult) * 8192) == 0);
	return failures == 0 ? 0 : 1;
}
}

int main(int argc, char **argv)
{
	bool localCapacity = false;
	bool serialPipelines = false;
	if (!rts_test::ParseTestCapacityLane(argc, argv, &localCapacity,
		&serialPipelines, "--serial-policy"))
	{
		fprintf(stderr, "Usage: core_parallel_visibility_tests [--local-capacity] [--serial-policy]\n");
		return 2;
	}
	rts_test::PrintTestCapacityLane(localCapacity);
	if (serialPipelines) return testSerialPipelinePolicy();
	testClipEdges();
	const unsigned workers[] = { 1, 2, 4, 8, 16, 0 };
	const unsigned sizes[] = { 0, 1, 511, 1023, 1024, 1031, 8192 };
	for (unsigned worker = 0; worker < sizeof(workers) / sizeof(workers[0]); ++worker)
	{
		rts::JobSystem &system = rts::JobSystem::instance();
		system.shutdown();
		rts::JobSystemConfig config;
		const unsigned effectiveWorkerCount =
			rts_test::ResolveActualWorkerCount(workers[worker], localCapacity);
		rts_test::PrintWorkerCountSubstitution("parallel visibility",
			workers[worker], effectiveWorkerCount, localCapacity);
		config.workerCount = effectiveWorkerCount;
		config.queueCapacity = 512;
		config.scratchBytesPerWorker = 4096;
		CHECK(system.start(config));
		ParallelVisibilityWorkspace workspace;
		for (unsigned title = 0; title < 2; ++title)
			for (unsigned reflection = 0; reflection < 2; ++reflection)
				for (unsigned markers = 0; markers < 2; ++markers)
					for (unsigned size = 0; size < sizeof(sizes) / sizeof(sizes[0]); ++size)
						checkCase(workspace, sizes[size], makeFrame(title != 0, reflection != 0, markers != 0));
		checkCase(workspace, ParallelVisibilityWorkspace::MAX_OBJECTS, makeFrame(true, false, true));
#if defined(VISIBILITY_TEST_MODERN) && defined(RTS_BUILD_CORE_EXTRAS)
		if (workers[worker] == 4 || workers[worker] == 8) testScaling();
#if defined(_WIN32)
		if (workers[worker] == 4) testFloatingPointState();
#endif
#endif
#if defined(RTS_BUILD_CORE_EXTRAS)
		if (workers[worker] == 4) testFailures();
#endif
#if defined(VISIBILITY_TEST_MODERN)
		if (workers[worker] == 4) testWorkerFallback();
#endif
		system.shutdown();
	}
#if defined(VISIBILITY_TEST_MODERN)
	testQueuePressure();
#endif
	rts::SetParallelVisibilityEnabled(false);
	CHECK(!rts::IsParallelVisibilityEnabled());
	rts::SetParallelVisibilityEnabled(true);
	CHECK(rts::IsParallelVisibilityEnabled());
	printf("Parallel visibility: %u failure(s)\n", failures);
	return failures == 0 ? 0 : 1;
}
