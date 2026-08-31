/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2025 Electronic Arts Inc.
** SPDX-License-Identifier: GPL-3.0-or-later
*/
#pragma once

#include "Lib/JobSystem.h"

namespace rts
{
// These snapshots deliberately contain no WW3D objects, channels or allocators.
struct SkinningVector { float x, y, z; };
struct SkinningMatrix { float row[3][4]; };
struct SkinningVertex
{
	SkinningVector position, normal;
	unsigned bone;
};
struct SkinnedVertex { SkinningVector position, normal; };
struct PoseBone
{
	SkinningMatrix base, rotation;
	SkinningVector translation;
	unsigned parent;
	bool translate, rotate, visible;
};
struct PoseTransform { SkinningMatrix transform; bool visible; };

enum SkinningResult
{
	SKINNING_SERIAL,
	SKINNING_PARALLEL,
	SKINNING_SERIAL_FALLBACK,
	SKINNING_CANCELLED,
	SKINNING_INVALID_INPUT
};

class SkinningScratchLease;

struct SkinningOptions
{
	SkinningOptions();
	bool parallel;
	unsigned minimumGrain;
	unsigned maximumScratchBytes;
	// Optional independent cancellation token. No work is submitted to this group.
	// The token and all input/output arrays must outlive this synchronous call.
	const JobGroup *cancellationGroup;
	// Optional already-prepared owner lease, shared with the product snapshot.
	SkinningScratchLease *scratch;
};

struct SkinningMetrics
{
	SkinningMetrics();
	unsigned submittedJobs;
	JobMetricCounter waitNanoseconds;
};

bool SkinningCompleted(SkinningResult result);

// One exclusive, bounded data arena per owner thread, reused across models and
// animation frames. Nested acquisition fails rather than aliasing live storage.
// Storage uses CRT allocation so thread-exit destruction does not depend on the
// game's allocator lifetime. A lease never crosses a thread or an async frame.
class SkinningScratchLease
{
public:
	SkinningScratchLease();
	~SkinningScratchLease();
	bool prepareSkin(unsigned vertexCount, unsigned boneCount);
	bool preparePose(unsigned boneCount);
	SkinningVertex *vertices();
	SkinningMatrix *matrices();
	SkinnedVertex *skinOutput();
	PoseBone *poseBones();
	PoseTransform *poseOutput();
	unsigned capacityBytes() const;
	unsigned allocationCount() const;

private:
	SkinningScratchLease(const SkinningScratchLease &);
	SkinningScratchLease &operator=(const SkinningScratchLease &);
	struct State;
	State *m_state;
	friend SkinningResult SkinVertices(const SkinningVertex *, unsigned,
		const SkinningMatrix *, unsigned, bool, SkinnedVertex *,
		const SkinningOptions &, SkinningMetrics *);
	friend SkinningResult EvaluatePose(const PoseBone *, unsigned,
		const SkinningMatrix &, PoseTransform *, const SkinningOptions &,
		SkinningMetrics *);
};

// Inputs must be immutable for the duration of the call, with non-overlapping
// input/output arrays. A cancelled or invalid call leaves output unchanged.
// Allocation/submission failures drain accepted jobs before serial fallback.
// Normals are rotated, never translated or renormalized (legacy WW3D semantics).
SkinningResult SkinVertices(const SkinningVertex *vertices, unsigned vertexCount,
	const SkinningMatrix *bones, unsigned boneCount, bool normals,
	SkinnedVertex *output, const SkinningOptions &options,
	SkinningMetrics *metrics = 0);

// Bone zero is the supplied root. Every other parent must precede its child.
// The operation order is parent*base, translation, then rotation: precomposing
// a local transform changes float rounding and is intentionally not allowed.
SkinningResult EvaluatePose(const PoseBone *bones, unsigned boneCount,
	const SkinningMatrix &root, PoseTransform *output,
	const SkinningOptions &options, SkinningMetrics *metrics = 0);
}
