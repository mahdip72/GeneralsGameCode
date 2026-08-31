/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2025 Electronic Arts Inc.
** SPDX-License-Identifier: GPL-3.0-or-later
*/
#include "Lib/ParallelSkinning.h"
#include "Lib/JobFloatingPointState.h"
#include "Lib/PipelineExecutionPolicy.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <new>
#include <vector>

#if !defined(_MSC_VER) || _MSC_VER >= 1300
#include <chrono>
#include <thread>
#if defined(RTS_BUILD_CORE_EXTRAS)
#include <atomic>
namespace
{
std::atomic<unsigned> skinningTestFault(0), skinningTestOccurrence(0);
bool consumeSkinningFault(unsigned fault)
{
	if (skinningTestFault.load() != fault) return false;
	unsigned value = skinningTestOccurrence.load();
	while (value != 0)
	{
		if (skinningTestOccurrence.compare_exchange_weak(value, value - 1))
			return value == 1;
	}
	return false;
}
}
extern "C" void rts_skinning_set_test_fault(unsigned fault, unsigned occurrence)
{
	skinningTestOccurrence.store(occurrence);
	skinningTestFault.store(fault);
}
#else
static bool consumeSkinningFault(unsigned) { return false; }
#endif
#endif

namespace rts
{
#if !defined(_MSC_VER) || _MSC_VER >= 1300
namespace
{
// Only backing-array capacity lives until thread exit. Completed JobHandles
// are cleared on lease release while the game allocator still exists.
template<class T> class SkinningCrtAllocator
{
public:
	typedef T value_type;
	SkinningCrtAllocator() {}
	template<class U> SkinningCrtAllocator(const SkinningCrtAllocator<U> &) {}
	T *allocate(size_t count)
	{
		if (count > static_cast<size_t>(-1) / sizeof(T)) throw std::bad_alloc();
		void *memory = malloc(count * sizeof(T));
		if (!memory) throw std::bad_alloc();
		return static_cast<T *>(memory);
	}
	void deallocate(T *memory, size_t) { free(memory); }
	template<class U> bool operator==(const SkinningCrtAllocator<U> &) const { return true; }
	template<class U> bool operator!=(const SkinningCrtAllocator<U> &) const { return false; }
};
}

struct SkinningScratchLease::State
{
	enum { MAXIMUM_BYTES = 32 * 1024 * 1024, SHAPE_NONE = 0, SHAPE_SKIN = 1, SHAPE_POSE = 2 };
	State() : arena(0), capacity(0), allocations(0), leased(false), shape(SHAPE_NONE),
		count(0), hasInputs(false), owner(std::this_thread::get_id()) {}
	~State() { free(arena); }
	bool owned() const { return leased && owner == std::this_thread::get_id(); }
	template<class T> static bool append(unsigned count, size_t &bytes, size_t &offset)
	{
		bytes = (bytes + 15) & ~static_cast<size_t>(15);
		if (bytes > MAXIMUM_BYTES || count > (MAXIMUM_BYTES - bytes) / sizeof(T)) return false;
		offset = bytes;
		bytes += static_cast<size_t>(count) * sizeof(T);
		return true;
	}
	bool reserve(size_t bytes)
	{
		if (bytes <= capacity) return true;
		if (bytes > MAXIMUM_BYTES || consumeSkinningFault(4)) return false;
		size_t grown = capacity ? capacity : 4096;
		while (grown < bytes) grown *= 2;
		void *replacement = malloc(grown);
		if (!replacement) return false;
		free(arena);
		arena = static_cast<unsigned char *>(replacement);
		capacity = grown;
		++allocations;
		return true;
	}
	bool skin(unsigned vertices, unsigned bones, bool inputs)
	{
		if (!owned()) return false;
		size_t bytes = 0, inputAt = 0, matricesAt = 0, outputAt = 0, workAt = 0;
		if (inputs && (!append<SkinningVertex>(vertices, bytes, inputAt) ||
			!append<SkinningMatrix>(bones, bytes, matricesAt) ||
			!append<SkinnedVertex>(vertices, bytes, outputAt))) return false;
		if (!append<SkinnedVertex>(vertices, bytes, workAt) || !reserve(bytes)) return false;
		inputOffset = inputAt; matrixOffset = matricesAt; outputOffset = outputAt; workOffset = workAt;
		shape = SHAPE_SKIN; count = vertices; hasInputs = inputs;
		return true;
	}
	bool pose(unsigned bones, bool inputs)
	{
		if (!owned() || bones == static_cast<unsigned>(-1)) return false;
		size_t bytes = 0, inputAt = 0, outputAt = 0, workAt = 0;
		size_t depthAt = 0, offsetsAt = 0, indicesAt = 0, cursorAt = 0;
		if (inputs && (!append<PoseBone>(bones, bytes, inputAt) ||
			!append<PoseTransform>(bones, bytes, outputAt))) return false;
		if (!append<PoseTransform>(bones, bytes, workAt) ||
			!append<unsigned>(bones, bytes, depthAt) ||
			!append<unsigned>(bones + 1, bytes, offsetsAt) ||
			!append<unsigned>(bones, bytes, indicesAt) ||
			!append<unsigned>(bones + 1, bytes, cursorAt) || !reserve(bytes)) return false;
		inputOffset = inputAt; outputOffset = outputAt; workOffset = workAt;
		depthOffset = depthAt; offsetsOffset = offsetsAt; indicesOffset = indicesAt; cursorOffset = cursorAt;
		shape = SHAPE_POSE; count = bones; hasInputs = inputs;
		return true;
	}
	template<class T> T *at(size_t offset) { return reinterpret_cast<T *>(arena + offset); }
	unsigned char *arena;
	size_t capacity;
	unsigned allocations;
	bool leased;
	unsigned shape, count;
	bool hasInputs;
	std::thread::id owner;
	size_t inputOffset, matrixOffset, outputOffset, workOffset;
	size_t depthOffset, offsetsOffset, indicesOffset, cursorOffset;
	std::vector<JobHandle, SkinningCrtAllocator<JobHandle> > previous, current;
};
#endif

SkinningScratchLease::SkinningScratchLease() : m_state(0)
{
#if !defined(_MSC_VER) || _MSC_VER >= 1300
	if (JobSystem::instance().isWorkerThread()) return;
	static thread_local State scratch;
	if (!scratch.leased)
	{
		scratch.leased = true;
		m_state = &scratch;
	}
#endif
}
SkinningScratchLease::~SkinningScratchLease()
{
#if !defined(_MSC_VER) || _MSC_VER >= 1300
	if (!m_state || !m_state->owned()) return;
	m_state->previous.clear();
	m_state->current.clear();
	m_state->shape = State::SHAPE_NONE;
	m_state->leased = false;
#endif
}
bool SkinningScratchLease::prepareSkin(unsigned vertices, unsigned bones)
{
#if !defined(_MSC_VER) || _MSC_VER >= 1300
	return m_state && vertices != 0 && vertices <= 262144 && bones != 0 && bones <= 16384 &&
		m_state->skin(vertices, bones, true);
#else
	return false;
#endif
}
bool SkinningScratchLease::preparePose(unsigned bones)
{
#if !defined(_MSC_VER) || _MSC_VER >= 1300
	return m_state && bones != 0 && bones <= 16384 && m_state->pose(bones, true);
#else
	return false;
#endif
}
SkinningVertex *SkinningScratchLease::vertices()
{
#if !defined(_MSC_VER) || _MSC_VER >= 1300
	return m_state && m_state->owned() && m_state->shape == State::SHAPE_SKIN && m_state->hasInputs ?
		m_state->at<SkinningVertex>(m_state->inputOffset) : 0;
#else
	return 0;
#endif
}
SkinningMatrix *SkinningScratchLease::matrices()
{
#if !defined(_MSC_VER) || _MSC_VER >= 1300
	return m_state && m_state->owned() && m_state->shape == State::SHAPE_SKIN && m_state->hasInputs ?
		m_state->at<SkinningMatrix>(m_state->matrixOffset) : 0;
#else
	return 0;
#endif
}
SkinnedVertex *SkinningScratchLease::skinOutput()
{
#if !defined(_MSC_VER) || _MSC_VER >= 1300
	return m_state && m_state->owned() && m_state->shape == State::SHAPE_SKIN && m_state->hasInputs ?
		m_state->at<SkinnedVertex>(m_state->outputOffset) : 0;
#else
	return 0;
#endif
}
PoseBone *SkinningScratchLease::poseBones()
{
#if !defined(_MSC_VER) || _MSC_VER >= 1300
	return m_state && m_state->owned() && m_state->shape == State::SHAPE_POSE && m_state->hasInputs ?
		m_state->at<PoseBone>(m_state->inputOffset) : 0;
#else
	return 0;
#endif
}
PoseTransform *SkinningScratchLease::poseOutput()
{
#if !defined(_MSC_VER) || _MSC_VER >= 1300
	return m_state && m_state->owned() && m_state->shape == State::SHAPE_POSE && m_state->hasInputs ?
		m_state->at<PoseTransform>(m_state->outputOffset) : 0;
#else
	return 0;
#endif
}
unsigned SkinningScratchLease::capacityBytes() const
{
#if !defined(_MSC_VER) || _MSC_VER >= 1300
	return m_state && m_state->owned() ? static_cast<unsigned>(m_state->capacity) : 0;
#else
	return 0;
#endif
}
unsigned SkinningScratchLease::allocationCount() const
{
#if !defined(_MSC_VER) || _MSC_VER >= 1300
	return m_state && m_state->owned() ? m_state->allocations : 0;
#else
	return 0;
#endif
}

SkinningOptions::SkinningOptions() : parallel(true), minimumGrain(256),
	maximumScratchBytes(16 * 1024 * 1024), cancellationGroup(0), scratch(0) {}
SkinningMetrics::SkinningMetrics() : submittedJobs(0), waitNanoseconds(0) {}
bool SkinningCompleted(SkinningResult result)
{
	return result == SKINNING_SERIAL || result == SKINNING_PARALLEL ||
		result == SKINNING_SERIAL_FALLBACK;
}

namespace
{
bool cancelled(const SkinningOptions &options)
{
	return options.cancellationGroup && options.cancellationGroup->wasCancelled();
}

void transformVertex(const SkinningVertex &vertex, const SkinningMatrix &matrix,
	bool normals, SkinnedVertex &output)
{
	const SkinningVector &v = vertex.position;
	output.position.x = matrix.row[0][0]*v.x + matrix.row[0][1]*v.y + matrix.row[0][2]*v.z + matrix.row[0][3];
	output.position.y = matrix.row[1][0]*v.x + matrix.row[1][1]*v.y + matrix.row[1][2]*v.z + matrix.row[1][3];
	output.position.z = matrix.row[2][0]*v.x + matrix.row[2][1]*v.y + matrix.row[2][2]*v.z + matrix.row[2][3];
	if (normals)
	{
		const SkinningVector &n = vertex.normal;
		// VectorProcessor::Transform uses a zero-translation matrix for normals.
		output.normal.x = matrix.row[0][0]*n.x + matrix.row[0][1]*n.y + matrix.row[0][2]*n.z + 0.0f;
		output.normal.y = matrix.row[1][0]*n.x + matrix.row[1][1]*n.y + matrix.row[1][2]*n.z + 0.0f;
		output.normal.z = matrix.row[2][0]*n.x + matrix.row[2][1]*n.y + matrix.row[2][2]*n.z + 0.0f;
	}
}

void multiply(const SkinningMatrix &a, const SkinningMatrix &b, SkinningMatrix &out)
{
	for (unsigned row = 0; row != 3; ++row)
	{
		for (unsigned column = 0; column != 3; ++column)
			out.row[row][column] = a.row[row][0]*b.row[0][column] + a.row[row][1]*b.row[1][column] + a.row[row][2]*b.row[2][column];
		out.row[row][3] = (a.row[row][0]*b.row[0][3] + a.row[row][1]*b.row[1][3] + a.row[row][2]*b.row[2][3]) + a.row[row][3];
	}
}

void evaluateBone(const PoseBone &bone, const SkinningMatrix &parent, PoseTransform &out)
{
	multiply(parent, bone.base, out.transform);
	if (bone.translate)
	{
		for (unsigned row = 0; row != 3; ++row)
			out.transform.row[row][3] += out.transform.row[row][0]*bone.translation.x + out.transform.row[row][1]*bone.translation.y + out.transform.row[row][2]*bone.translation.z;
	}
	if (bone.rotate)
	{
		SkinningMatrix original = out.transform;
		for (unsigned row = 0; row != 3; ++row)
		{
			for (unsigned column = 0; column != 3; ++column)
				out.transform.row[row][column] = original.row[row][0]*bone.rotation.row[0][column] + original.row[row][1]*bone.rotation.row[1][column] + original.row[row][2]*bone.rotation.row[2][column];
			// postMul adds its translation in this order, not Multiply's order.
			out.transform.row[row][3] += original.row[row][0]*bone.rotation.row[0][3] + original.row[row][1]*bone.rotation.row[1][3] + original.row[row][2]*bone.rotation.row[2][3];
		}
	}
	out.visible = bone.visible;
}

void skinSerial(const SkinningVertex *vertices, unsigned count,
	const SkinningMatrix *bones, bool normals, SkinnedVertex *output)
{
	for (unsigned index = 0; index != count; ++index)
		transformVertex(vertices[index], bones[vertices[index].bone], normals, output[index]);
}

void poseSerial(const PoseBone *bones, unsigned count, const SkinningMatrix &root,
	PoseTransform *output)
{
	if (!count) return;
	output[0].transform = root;
	output[0].visible = true;
	for (unsigned index = 1; index != count; ++index)
		evaluateBone(bones[index], output[bones[index].parent].transform, output[index]);
}

#if !defined(_MSC_VER) || _MSC_VER >= 1300
bool shouldParallel(unsigned count, const SkinningOptions &options)
{
	JobSystem &jobs = JobSystem::instance();
	const unsigned minimum = options.minimumGrain ? options.minimumGrain : 1;
	return UseParallelPipelines() && options.parallel && count / minimum >= 2 && !jobs.isWorkerThread() &&
		jobs.ensureStarted() && jobs.workerCount() > 1;
}

void drain(JobSystem &jobs, const JobGroup &group, SkinningMetrics &metrics)
{
	const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
	jobs.wait(group);
	metrics.waitNanoseconds += static_cast<JobMetricCounter>(
		std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
}

class SkinJob : public Job
{
public:
	static void *operator new(size_t bytes, const std::nothrow_t &) throw() { return malloc(bytes); }
	static void operator delete(void *memory) throw() { free(memory); }
	static void operator delete(void *memory, const std::nothrow_t &) throw() { free(memory); }
	SkinJob(const SkinningVertex *vertices, const SkinningMatrix *bones, bool normals,
		SkinnedVertex *output, unsigned begin, unsigned end, const SkinningOptions &options) :
		m_vertices(vertices), m_bones(bones), m_normals(normals), m_output(output),
		m_begin(begin), m_end(end), m_options(options) { m_options.scratch = 0; }
	virtual void execute(JobContext &context)
	{
		JobFloatingPointScope floatScope(m_floatState);
		if (consumeSkinningFault(3)) { context.fail(); return; }
		for (unsigned index = m_begin; index != m_end; ++index)
		{
			if ((index - m_begin) % 256 == 0 &&
				(context.isCancellationRequested() || cancelled(m_options)))
				{ context.fail(); return; }
			transformVertex(m_vertices[index], m_bones[m_vertices[index].bone], m_normals, m_output[index]);
		}
	}
private:
	const SkinningVertex *m_vertices;
	const SkinningMatrix *m_bones;
	bool m_normals;
	SkinnedVertex *m_output;
	unsigned m_begin, m_end;
	SkinningOptions m_options;
	JobFloatingPointState m_floatState;
};

class PoseJob : public Job
{
public:
	static void *operator new(size_t bytes, const std::nothrow_t &) throw() { return malloc(bytes); }
	static void operator delete(void *memory) throw() { free(memory); }
	static void operator delete(void *memory, const std::nothrow_t &) throw() { free(memory); }
	PoseJob(const PoseBone *bones, const unsigned *indices, PoseTransform *output,
		unsigned begin, unsigned end, const SkinningOptions &options) :
		m_bones(bones), m_indices(indices), m_output(output), m_begin(begin), m_end(end), m_options(options) { m_options.scratch = 0; }
	virtual void execute(JobContext &context)
	{
		JobFloatingPointScope floatScope(m_floatState);
		if (consumeSkinningFault(3)) { context.fail(); return; }
		for (unsigned item = m_begin; item != m_end; ++item)
		{
			if ((item - m_begin) % 64 == 0 &&
				(context.isCancellationRequested() || cancelled(m_options)))
				{ context.fail(); return; }
			const unsigned index = m_indices[item];
			evaluateBone(m_bones[index], m_output[m_bones[index].parent].transform, m_output[index]);
		}
	}
private:
	const PoseBone *m_bones;
	const unsigned *m_indices;
	PoseTransform *m_output;
	unsigned m_begin, m_end;
	SkinningOptions m_options;
	JobFloatingPointState m_floatState;
};
#endif
}

SkinningResult SkinVertices(const SkinningVertex *vertices, unsigned vertexCount,
	const SkinningMatrix *bones, unsigned boneCount, bool normals,
	SkinnedVertex *output, const SkinningOptions &options, SkinningMetrics *metrics)
{
	SkinningMetrics localMetrics;
	if (!metrics) metrics = &localMetrics;
	*metrics = SkinningMetrics();
	if (vertexCount && (!vertices || !bones || !boneCount || !output)) return SKINNING_INVALID_INPUT;
	for (unsigned index = 0; index != vertexCount; ++index)
		if (vertices[index].bone >= boneCount) return SKINNING_INVALID_INPUT;
	if (cancelled(options)) return SKINNING_CANCELLED;
	bool fallback = false;
#if !defined(_MSC_VER) || _MSC_VER >= 1300
	if (shouldParallel(vertexCount, options))
	{
		fallback = true;
		JobSystem &jobs = JobSystem::instance();
		JobGroup group = jobs.createGroup();
		SkinningScratchLease localLease;
		SkinningScratchLease *lease = options.scratch ? options.scratch : &localLease;
		SkinningScratchLease::State *workspace = lease->m_state;
		SkinnedVertex *scratch = 0;
		bool accepted = group.isValid();
		try
		{
			if (vertexCount > options.maximumScratchBytes / sizeof(SkinnedVertex) || consumeSkinningFault(1))
				throw std::bad_alloc();
			if (!workspace || !workspace->owned() ||
				(options.scratch ? workspace->shape != SkinningScratchLease::State::SHAPE_SKIN ||
					workspace->count != vertexCount : !workspace->skin(vertexCount, 0, false)))
				throw std::bad_alloc();
			scratch = workspace->at<SkinnedVertex>(workspace->workOffset);
			const unsigned rangeCount = JobSystem::chooseRangeCount(vertexCount, options.minimumGrain, jobs.workerCount());
			for (unsigned rangeIndex = 0; accepted && rangeIndex < rangeCount; ++rangeIndex)
			{
				JobRange range;
				JobSystem::rangeForIndex(vertexCount, rangeCount, rangeIndex, range);
				SkinJob *job = consumeSkinningFault(2) ? 0 : new (std::nothrow) SkinJob(vertices, bones, normals, &scratch[0], range.begin, range.end, options);
				JobHandle handle = job ? jobs.trySubmit(job, JOB_PRIORITY_FRAME_CRITICAL, group) : JobHandle();
				if (!handle.isValid()) { delete job; accepted = false; }
				else ++metrics->submittedJobs;
			}
		}
		catch (...) { accepted = false; }
		if (group.isValid())
		{
			if (!accepted) jobs.cancel(group);
			drain(jobs, group, *metrics);
		}
		if (cancelled(options) || (accepted && group.wasCancelled())) return SKINNING_CANCELLED;
		if (accepted && !group.failed())
		{
			for (unsigned index = 0; index != vertexCount; ++index)
			{
				output[index].position = scratch[index].position;
				if (normals) output[index].normal = scratch[index].normal;
			}
			return SKINNING_PARALLEL;
		}
		jobs.recordSerialFallback();
	}
#endif
	skinSerial(vertices, vertexCount, bones, normals, output);
	return fallback ? SKINNING_SERIAL_FALLBACK : SKINNING_SERIAL;
}

SkinningResult EvaluatePose(const PoseBone *bones, unsigned boneCount,
	const SkinningMatrix &root, PoseTransform *output,
	const SkinningOptions &options, SkinningMetrics *metrics)
{
	SkinningMetrics localMetrics;
	if (!metrics) metrics = &localMetrics;
	*metrics = SkinningMetrics();
	if (boneCount && (!bones || !output)) return SKINNING_INVALID_INPUT;
	for (unsigned index = 1; index < boneCount; ++index)
		if (bones[index].parent >= index) return SKINNING_INVALID_INPUT;
	if (cancelled(options)) return SKINNING_CANCELLED;
	bool fallback = false;
#if !defined(_MSC_VER) || _MSC_VER >= 1300
	if (shouldParallel(boneCount, options))
	{
		fallback = true;
		JobSystem &jobs = JobSystem::instance();
		JobGroup group = jobs.createGroup();
		SkinningScratchLease localLease;
		SkinningScratchLease *lease = options.scratch ? options.scratch : &localLease;
		SkinningScratchLease::State *workspace = lease->m_state;
		PoseTransform *scratch = 0;
		bool accepted = group.isValid();
		try
		{
			// Include schedule and handle storage in the scratch budget.
			const size_t bytesPerBone = sizeof(PoseTransform) + 6*sizeof(unsigned) + 2*sizeof(JobHandle);
			if (boneCount > options.maximumScratchBytes / bytesPerBone || consumeSkinningFault(1))
				throw std::bad_alloc();
			if (!workspace || !workspace->owned() ||
				(options.scratch ? workspace->shape != SkinningScratchLease::State::SHAPE_POSE ||
					workspace->count != boneCount : !workspace->pose(boneCount, false)))
				throw std::bad_alloc();
			scratch = workspace->at<PoseTransform>(workspace->workOffset);
			unsigned *depths = workspace->at<unsigned>(workspace->depthOffset);
			unsigned *offsets = workspace->at<unsigned>(workspace->offsetsOffset);
			unsigned *indices = workspace->at<unsigned>(workspace->indicesOffset);
			unsigned *cursor = workspace->at<unsigned>(workspace->cursorOffset);
			memset(depths, 0, boneCount * sizeof(unsigned));
			memset(offsets, 0, (boneCount + 1) * sizeof(unsigned));
			std::vector<JobHandle, SkinningCrtAllocator<JobHandle> > &previous = workspace->previous;
			std::vector<JobHandle, SkinningCrtAllocator<JobHandle> > &current = workspace->current;
			previous.clear();
			current.clear();
			unsigned maximumDepth = 0;
			for (unsigned index = 1; index < boneCount; ++index)
			{
				depths[index] = depths[bones[index].parent] + 1;
				if (depths[index] > maximumDepth) maximumDepth = depths[index];
				++offsets[depths[index] + 1];
			}
			for (unsigned depth = 1; depth <= maximumDepth; ++depth)
				offsets[depth + 1] += offsets[depth];
			unsigned maximumWidth = 0;
			for (unsigned depth = 1; depth <= maximumDepth; ++depth)
				if (offsets[depth + 1] - offsets[depth] > maximumWidth)
					maximumWidth = offsets[depth + 1] - offsets[depth];
			const unsigned maximumRanges = JobSystem::chooseRangeCount(maximumWidth, options.minimumGrain, jobs.workerCount());
			if (maximumRanges < 2)
			{
				if (cancelled(options)) return SKINNING_CANCELLED;
				poseSerial(bones, boneCount, root, output);
				return SKINNING_SERIAL;
			}
			// Warm both handle-array capacities before alternating depth buffers.
			// Handles themselves remain scheduler-owned per-submission metadata.
			previous.reserve(maximumRanges);
			current.reserve(maximumRanges);
			memcpy(cursor, offsets, (boneCount + 1) * sizeof(unsigned));
			for (unsigned index = 1; index < boneCount; ++index)
				indices[cursor[depths[index]]++] = index;
			scratch[0].transform = root;
			scratch[0].visible = true;
			for (unsigned depth = 1; accepted && depth <= maximumDepth; ++depth)
			{
				current.clear();
				const unsigned count = offsets[depth + 1] - offsets[depth];
				const unsigned rangeCount = JobSystem::chooseRangeCount(count, options.minimumGrain, jobs.workerCount());
				current.reserve(rangeCount);
				for (unsigned rangeIndex = 0; accepted && rangeIndex < rangeCount; ++rangeIndex)
				{
					JobRange range;
					JobSystem::rangeForIndex(count, rangeCount, rangeIndex, range);
					PoseJob *job = consumeSkinningFault(2) ? 0 : new (std::nothrow) PoseJob(bones, &indices[0], &scratch[0], offsets[depth] + range.begin, offsets[depth] + range.end, options);
					JobHandle handle = job ? jobs.trySubmitAfter(job, JOB_PRIORITY_FRAME_CRITICAL, group,
						previous.empty() ? 0 : &previous[0], static_cast<unsigned>(previous.size())) : JobHandle();
					if (!handle.isValid()) { delete job; accepted = false; }
					else { ++metrics->submittedJobs; current.push_back(handle); }
				}
				previous.swap(current);
			}
		}
		catch (...) { accepted = false; }
		if (group.isValid())
		{
			if (!accepted) jobs.cancel(group);
			drain(jobs, group, *metrics);
		}
		if (cancelled(options) || (accepted && group.wasCancelled())) return SKINNING_CANCELLED;
		if (accepted && !group.failed())
		{
			for (unsigned index = 0; index != boneCount; ++index) output[index] = scratch[index];
			return SKINNING_PARALLEL;
		}
		jobs.recordSerialFallback();
	}
#endif
	poseSerial(bones, boneCount, root, output);
	return fallback ? SKINNING_SERIAL_FALLBACK : SKINNING_SERIAL;
}
}
