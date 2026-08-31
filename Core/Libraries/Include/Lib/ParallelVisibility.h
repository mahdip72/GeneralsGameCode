/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2025 Electronic Arts Inc.
** SPDX-License-Identifier: GPL-3.0-or-later
*/

#pragma once

#include "Lib/JobSystem.h"

namespace rts
{
// These are value-only worker inputs. No scene, drawable, camera or allocator
// may be consulted between dispatch and completion.
struct VisibilityPlane
{
	float x, y, z, distance;
};

enum VisibilityInputFlags
{
	VIS_INPUT_FORCE_VISIBLE = 1,
	VIS_INPUT_HIDDEN = 2,
	VIS_INPUT_DRAWABLE = 4,
	VIS_INPUT_MIRROR = 8,
	VIS_INPUT_EFFECTIVELY_HIDDEN = 16,
	VIS_INPUT_SHROUDED = 32,
	VIS_INPUT_TRANSLUCENT = 64,
	VIS_INPUT_STRUCTURE = 128,
	VIS_INPUT_OCCLUDEE = 256
};

struct VisibilityInput
{
	float x, y, z, radius;
	unsigned flags;
};

struct VisibilityFrame
{
	VisibilityFrame();
	VisibilityPlane planes[6];
	bool reflection;
	bool resetHiddenDrawableFlags; // Zero Hour resets these; Generals does not.
	bool translucentOcclusion; // Zero Hour also classifies translucent objects.
	bool behindBuildingMarkers;
};

enum VisibilityResultFlags
{
	VIS_RESULT_VISIBLE = 1,
	VIS_RESULT_RESET_DRAW_FLAGS = 2,
	VIS_RESULT_TRANSLUCENT = 4,
	VIS_RESULT_OCCLUDER = 8,
	VIS_RESULT_OCCLUDEE = 16
};

struct VisibilityResult
{
	unsigned flags;
};

enum VisibilityListFlags
{
	VIS_LIST_TRANSLUCENT = 1,
	VIS_LIST_OCCLUDER = 2,
	VIS_LIST_OCCLUDEE = 4,
	VIS_LIST_OTHER = 8
};

struct VisibilityListBudget
{
	VisibilityListBudget();
	unsigned translucent, occluders, occludees, others;
};

struct VisibilityPublication
{
	unsigned lists;
	unsigned drawFlags; // VisibilityListFlags, mapped to DrawableInfo by owner.
};

// Call in original RenderList order. Capacity admission is intentionally serial:
// a full translucent/occluder queue changes the legacy classification branch.
VisibilityPublication PlanVisibilityPublication(const VisibilityFrame &frame,
	const VisibilityResult &result, VisibilityListBudget &remaining);

bool VisibilitySphereInside(const VisibilityFrame &frame,
	const VisibilityInput &input);
void EvaluateVisibilityReference(const VisibilityFrame &frame,
	const VisibilityInput *inputs, VisibilityResult *outputs, unsigned count);

struct ParallelVisibilityMetrics
{
	ParallelVisibilityMetrics();
	unsigned objectCount;
	unsigned submittedJobs;
	unsigned completedJobs;
	unsigned workerThreadsUsed;
	unsigned serialFallbacks;
	JobMetricCounter snapshotNanoseconds;
	JobMetricCounter prepareNanoseconds;
	JobMetricCounter waitNanoseconds;
	JobMetricCounter publicationNanoseconds;
};

JobMetricCounter VisibilityClockNanoseconds();
// Owner-thread controls. Disabled means the product uses its original scene
// implementation, including original allocation/culling behavior.
void SetParallelVisibilityEnabled(bool enabled);
bool IsParallelVisibilityEnabled();

class ParallelVisibilityWorkspace
{
public:
	enum { MAX_OBJECTS = 131072, MIN_PARALLEL_OBJECTS = 1024 };
	ParallelVisibilityWorkspace();
	~ParallelVisibilityWorkspace();
	// Transactional, owner-only, bounded allocation. Failure preserves old storage.
	bool reserve(unsigned count);
	VisibilityInput *inputs() { return m_inputs; }
	const VisibilityResult *results() const { return m_results; }
	unsigned capacity() const { return m_capacity; }
	// Synchronous fence for this flat CPU stage only. Never called by a worker.
	// Failed/cancelled jobs are drained before a complete serial recomputation.
	bool prepare(const VisibilityFrame &frame, unsigned count, bool parallel = true);
	// Keep diagnostics current when a small/disabled/OOM scene takes the original
	// owner loop instead of calling prepare.
	void recordSceneReference(unsigned count, bool fallback);
	void recordOwnerTimings(JobMetricCounter snapshot, JobMetricCounter publication);
	const ParallelVisibilityMetrics &metrics() const { return m_metrics; }

private:
	ParallelVisibilityWorkspace(const ParallelVisibilityWorkspace &);
	ParallelVisibilityWorkspace &operator=(const ParallelVisibilityWorkspace &);
	VisibilityInput *m_inputs;
	VisibilityResult *m_results;
	unsigned m_capacity;
	ParallelVisibilityMetrics m_metrics;
};
}
