/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2026 TheSuperHackers
** SPDX-License-Identifier: GPL-3.0-or-later
**
** Deferred triangle sorting for the native render-owner seam.  The queue
** retains copied source bytes until every accepted draw group is acknowledged
** by the owner, so an owner failure can be retried without duplicating work.
*/

#include "Utility/CppMacros.h"
#include "nativew3dsorting.h"

#include "Lib/SortingTriangleKernel.h"

#include <algorithm>
#include <limits>
#include <math.h>
#include <new>
#include <string.h>
#include <utility>
#include <vector>

namespace
{

using namespace rts::render;

const unsigned int MAX_SORTING_INDEX_COUNT = 65535U;
const unsigned int MAX_SORTING_VERTEX_COUNT = 65535U;
const unsigned int MAX_SORTING_VERTEX_CAPACITY = 65536U;
const unsigned int MAX_SORTING_TRIANGLES_PER_CHUNK =
	MAX_SORTING_INDEX_COUNT / 3U;
const unsigned int MAX_SORTING_TRIANGLES_PER_KERNEL_CALL = 65535U;

struct SortedSubmission
{
	SortedSubmission() : state(), packet(), vertices(), indices(), sphere(),
		hasSphere(false), insertionOrder(0), submittedTriangles() {}

	LegacyLogicalState state;
	NativeDrawPacket packet;
	std::vector<unsigned char> vertices;
	std::vector<unsigned short> indices;
	GameBoundingSphere sphere;
	bool hasSphere;
	size_t insertionOrder;
	std::vector<unsigned char> submittedTriangles;
};

struct SortedNode
{
	SortedNode() : submissionIndex(0), centerDepth(0.0f), hasSphere(false),
		insertionOrder(0) {}

	size_t submissionIndex;
	float centerDepth;
	bool hasSphere;
	size_t insertionOrder;
	float worldView[16];
};

struct SortedTriangle
{
	SortedTriangle() : submissionIndex(0), sourceTriangle(0), i(0), j(0),
		k(0), depth(0.0f) {}

	size_t submissionIndex;
	unsigned int sourceTriangle;
	unsigned short i;
	unsigned short j;
	unsigned short k;
	float depth;
};

struct DrawRun
{
	DrawRun() : submissionIndex(0), firstTriangle(0), triangleCount(0) {}

	size_t submissionIndex;
	size_t firstTriangle;
	size_t triangleCount;
};

struct FlushScope
{
	explicit FlushScope(bool &activeFlag) : active(activeFlag)
	{
		active = true;
	}
	~FlushScope()
	{
		active = false;
	}
	bool &active;
};

#if defined(RTS_NATIVE_SORTING_TESTS)
unsigned int g_nativeSortingLastFlushScratchAllocationCount = 0;
unsigned int g_nativeSortingLastFlushPreparedGrowthCount = 0;
#endif

bool IsFiniteFloat(float value)
{
	return _finite(value) != 0;
}

bool IsByteCountValid(unsigned int count, unsigned int stride,
	size_t *required)
{
	if (required == 0 || stride == 0)
		return false;
	if (static_cast<size_t>(count) >
		std::numeric_limits<size_t>::max() / stride)
		return false;
	*required = static_cast<size_t>(count) * stride;
	return true;
}

bool ReadFloat(const std::vector<unsigned char> &bytes, size_t offset,
	float *value)
{
	if (value == 0 || offset > bytes.size() || bytes.size() - offset <
		sizeof(float))
		return false;
	memcpy(value, &bytes[offset], sizeof(*value));
	return true;
}

void MultiplySortingMatrices(const RenderMatrix4 &left,
	const RenderMatrix4 &right, float *result)
{
	for (unsigned int row = 0; row < 4; ++row)
	{
		for (unsigned int column = 0; column < 4; ++column)
		{
			result[row * 4 + column] =
				left.values[row * 4 + 0] * right.values[0 * 4 + column] +
				left.values[row * 4 + 1] * right.values[1 * 4 + column] +
				left.values[row * 4 + 2] * right.values[2 * 4 + column] +
				left.values[row * 4 + 3] * right.values[3 * 4 + column];
		}
	}
}

bool IsFiniteMatrix(const float *matrix)
{
	for (unsigned int index = 0; index < 16; ++index)
	{
		if (!IsFiniteFloat(matrix[index]))
			return false;
	}
	return true;
}

float TransformDepth(const float *matrix, float x, float y, float z)
{
	return x * matrix[0 * 4 + 2] + y * matrix[1 * 4 + 2] +
		z * matrix[2 * 4 + 2] + matrix[3 * 4 + 2];
}

bool ValidateSubmission(const NativeDrawPacket &packet,
	const void *vertexData, size_t vertexBytes, const void *indexData,
	size_t indexBytes, const GameBoundingSphere *sphere,
	size_t *requiredVertexBytes, size_t *requiredIndexBytes)
{
	if (!IsByteCountValid(packet.vertexCount, packet.vertexStride,
		requiredVertexBytes) ||
		!IsByteCountValid(packet.indexCount,
			static_cast<unsigned int>(sizeof(unsigned short)),
			requiredIndexBytes))
		return false;
	if (packet.vertexStride < sizeof(float) * 3 || packet.vertexCount == 0 ||
		packet.indexCount == 0 || (packet.indexCount % 3) != 0 ||
		packet.vertexCount > MAX_SORTING_VERTEX_COUNT ||
		packet.minimumVertexIndex > MAX_SORTING_VERTEX_COUNT ||
		packet.vertexCount > 65536U - packet.minimumVertexIndex ||
		!packet.indexed || packet.indexFormat != RENDER_FORMAT_R16_UINT ||
		packet.vertexLayout.stride != packet.vertexStride ||
		packet.vertexLayout.elementCount > RenderVertexLayout::MAX_ELEMENT_COUNT ||
		packet.topology != RENDER_PRIMITIVE_TRIANGLE_LIST ||
		vertexData == 0 || indexData == 0 || vertexBytes < *requiredVertexBytes ||
		indexBytes < *requiredIndexBytes)
		return false;
	if (sphere != 0 && (!IsFiniteFloat(sphere->centerX) ||
		!IsFiniteFloat(sphere->centerY) || !IsFiniteFloat(sphere->centerZ) ||
		!IsFiniteFloat(sphere->radius) || sphere->radius < 0.0f))
		return false;
	return true;
}

bool HasPendingTriangles(const SortedSubmission &submission)
{
	for (size_t index = 0; index < submission.submittedTriangles.size(); ++index)
	{
		if (!submission.submittedTriangles[index])
			return true;
	}
	return false;
}

struct CompletedSubmissionPredicate
{
	bool operator()(const SortedSubmission &submission) const
	{
		return !HasPendingTriangles(submission);
	}
};

void RetireCompletedSubmissions(std::vector<SortedSubmission> &submissions)
{
	// remove_if preserves the relative order of pending submissions and moves
	// survivors only once; erasing the tail releases completed submissions.
	const std::vector<SortedSubmission>::iterator pendingEnd =
		std::remove_if(submissions.begin(), submissions.end(),
			CompletedSubmissionPredicate());
	submissions.erase(pendingEnd, submissions.end());
}

struct SortedNodeDepthDescending
{
	explicit SortedNodeDepthDescending(
		const std::vector<SortedNode> &sourceNodes) : nodes(&sourceNodes) {}

	bool operator()(size_t leftIndex, size_t rightIndex) const
	{
		return (*nodes)[leftIndex].centerDepth >
			(*nodes)[rightIndex].centerDepth;
	}

	const std::vector<SortedNode> *nodes;
};

bool ValidateSourceIndices(const SortedSubmission &submission)
{
	const unsigned int minimum = submission.packet.minimumVertexIndex;
	const unsigned int vertexCount = submission.packet.vertexCount;
	for (size_t index = 0; index < submission.indices.size(); ++index)
	{
		const unsigned int value = submission.indices[index];
		if (value < minimum || value >= minimum + vertexCount)
			return false;
	}
	return true;
}

RenderResult AppendPreparedTriangles(const SortedSubmission &submission,
	size_t submissionIndex, const float *matrix,
	rts::SortingTriangleScratchLease &scratch,
	std::vector<rts::SortingTriangleOutput> &prepared,
	std::vector<SortedTriangle> &triangles)
{
	if (!ValidateSourceIndices(submission))
		return RENDER_RESULT_INVALID_ARGUMENT;

	const unsigned int triangleCount =
		submission.packet.indexCount / 3U;
	const unsigned int minimum = submission.packet.minimumVertexIndex;
	const unsigned int stride = submission.packet.vertexStride;
	const bool commonZ = matrix[0 * 4 + 2] == 0.0f &&
		matrix[1 * 4 + 2] == 0.0f && matrix[3 * 4 + 2] == 0.0f &&
		matrix[2 * 4 + 2] == 1.0f;

	rts::SortingTriangleOptions options;
	options.parallel = true;
	for (unsigned int batchStart = 0; batchStart < triangleCount; )
	{
		const unsigned int batchCount = std::min(
			MAX_SORTING_TRIANGLES_PER_KERNEL_CALL, triangleCount - batchStart);
		if (batchCount == 0)
			break;
		if (!scratch.prepare(1, batchCount, options.maximumScratchBytes))
			return RENDER_RESULT_OUT_OF_MEMORY;

		rts::SortingTriangleDescriptor descriptor;
		descriptor.vertices = submission.vertices.data();
		descriptor.vertexStrideBytes = stride;
		descriptor.indices = submission.indices.data() +
			static_cast<size_t>(batchStart) * 3;
		descriptor.minVertexIndex = static_cast<unsigned short>(minimum);
		descriptor.vertexCount = static_cast<unsigned short>(
			submission.packet.vertexCount);
		descriptor.polygonCount = static_cast<unsigned short>(batchCount);
		descriptor.vertexOffset = 0;
		descriptor.outputOffset = 0;
		descriptor.nodeIndex = 0;
		descriptor.zX = matrix[0 * 4 + 2];
		descriptor.zY = matrix[1 * 4 + 2];
		descriptor.zZ = matrix[2 * 4 + 2];
		descriptor.zTranslation = matrix[3 * 4 + 2];
		descriptor.commonZ = commonZ ? 1U : 0U;

		if (prepared.size() < batchCount)
		{
			const bool growsCapacity = prepared.capacity() < batchCount;
			prepared.resize(batchCount);
#if defined(RTS_NATIVE_SORTING_TESTS)
			if (growsCapacity)
				++g_nativeSortingLastFlushPreparedGrowthCount;
#endif
		}
		rts::SortingTriangleMetrics metrics;
		rts::SortingTriangleResult result = rts::PrepareSortingTriangles(
			&descriptor, 1, batchCount, prepared.data(), scratch.outputs(),
			options, &metrics);
		if (result == rts::SORTING_TRIANGLE_SERIAL_FALLBACK)
		{
			// The parallel call has fenced all accepted jobs before returning.
			// Reuse the same owner-thread scratch storage for the reference loop.
			options.parallel = false;
			result = rts::PrepareSortingTriangles(&descriptor, 1, batchCount,
				prepared.data(), scratch.outputs(), options, &metrics);
			options.parallel = true;
		}
		if (!rts::SortingTriangleCompleted(result))
		return result == rts::SORTING_TRIANGLE_INVALID_INPUT ?
			RENDER_RESULT_INVALID_ARGUMENT : RENDER_RESULT_FAILED;

		for (unsigned int local = 0; local < batchCount; ++local)
		{
			const unsigned int sourceTriangle = batchStart + local;
			if (submission.submittedTriangles[sourceTriangle])
				continue;
			if (!IsFiniteFloat(prepared[local].z))
				return RENDER_RESULT_INVALID_ARGUMENT;
			SortedTriangle triangle;
			triangle.submissionIndex = submissionIndex;
			triangle.sourceTriangle = sourceTriangle;
			triangle.i = static_cast<unsigned short>(
				prepared[local].tri.i);
			triangle.j = static_cast<unsigned short>(
				prepared[local].tri.j);
			triangle.k = static_cast<unsigned short>(
				prepared[local].tri.k);
			triangle.depth = prepared[local].z;
			triangles.push_back(triangle);
		}
		batchStart += batchCount;
	}
	return RENDER_RESULT_OK;
}

bool operator>(const SortedTriangle &left, const SortedTriangle &right)
{
	return left.depth > right.depth;
}

void InsertionSort(SortedTriangle *begin, SortedTriangle *end)
{
	for (SortedTriangle *iter = begin + 1; iter < end; ++iter)
	{
		SortedTriangle value = iter[0];
		SortedTriangle *insert = iter;
		while (insert != begin && insert[-1] > value)
		{
			insert[0] = insert[-1];
			insert -= 1;
		}
		insert[0] = value;
	}
}

void Sort(SortedTriangle *begin, SortedTriangle *end)
{
	const int difference = static_cast<int>(end - begin);
	if (difference <= 16)
	{
		InsertionSort(begin, end);
		return;
	}

	SortedTriangle *middle = begin + difference / 2;
	std::swap(middle[0], begin[1]);
	if (begin[1] > end[-1])
		std::swap(begin[1], end[-1]);
	if (begin[0] > end[-1])
		std::swap(begin[0], end[-1]);
	if (begin[1] > begin[0])
		std::swap(begin[1], begin[0]);

	SortedTriangle *beginGuard = begin + 1;
	SortedTriangle *endGuard = end - 1;
	SortedTriangle *left = begin + 1;
	SortedTriangle *right = end - 1;
	for (;;)
	{
		do ++left; while (left < endGuard && left[0].depth < begin[0].depth);
		do --right; while (right > beginGuard && right[0].depth > begin[0].depth);
		if (right < left)
			break;
		std::swap(left[0], right[0]);
	}
	std::swap(begin[0], right[0]);

	if (right - begin > end - (right + 1))
	{
		Sort(right + 1, end);
		Sort(begin, right);
	}
	else
	{
		Sort(begin, right);
		Sort(right + 1, end);
	}
}

bool SameChunkGeometry(const NativeDrawPacket &left,
	const NativeDrawPacket &right)
{
	const RenderVertexLayout &a = left.vertexLayout;
	const RenderVertexLayout &b = right.vertexLayout;
	if (left.vertexStride != right.vertexStride ||
		left.vertexFormat != right.vertexFormat ||
		left.topology != right.topology ||
		left.indexFormat != right.indexFormat ||
		a.stride != b.stride || a.elementCount != b.elementCount ||
		a.preTransformed != b.preTransformed)
		return false;
	for (unsigned int index = 0; index < a.elementCount; ++index)
	{
		if (a.elements[index].semantic != b.elements[index].semantic ||
			a.elements[index].semanticIndex != b.elements[index].semanticIndex ||
			a.elements[index].format != b.elements[index].format ||
			a.elements[index].byteOffset != b.elements[index].byteOffset)
			return false;
	}
	return true;
}

void AddDrawRun(std::vector<NativeSortedDraw> &draws,
	std::vector<DrawRun> &runs, const SortedSubmission &submission,
	size_t submissionIndex, size_t localTriangle, size_t vertexOffset)
{
	if (!runs.empty() && runs.back().submissionIndex == submissionIndex &&
		runs.back().firstTriangle + runs.back().triangleCount == localTriangle)
	{
		runs.back().triangleCount += 1;
		draws.back().packet.indexCount += 3;
		return;
	}

	NativeSortedDraw draw;
	draw.state = submission.state;
	draw.packet = submission.packet;
	draw.packet.vertexBuffer = GpuHandle();
	draw.packet.indexBuffer = GpuHandle();
	draw.packet.vertexOffset = static_cast<unsigned int>(vertexOffset);
	draw.packet.indexOffset = 0;
	draw.packet.startVertex = 0;
	draw.packet.startIndex = static_cast<unsigned int>(localTriangle * 3);
	draw.packet.indexCount = 3;
	draw.packet.minimumVertexIndex = 0;
	draw.packet.baseVertex = 0;
	draw.packet.indexed = true;

	DrawRun run;
	run.submissionIndex = submissionIndex;
	run.firstTriangle = localTriangle;
	run.triangleCount = 1;
	draws.push_back(draw);
	runs.push_back(run);
}

void MarkRunsSubmitted(const std::vector<DrawRun> &runs,
	const std::vector<SortedTriangle> &triangles, size_t chunkOffset,
	unsigned int acceptedDrawCount, std::vector<SortedSubmission> &submissions)
{
	for (unsigned int drawIndex = 0; drawIndex < acceptedDrawCount;
		++drawIndex)
	{
		const DrawRun &run = runs[drawIndex];
		for (size_t local = 0; local < run.triangleCount; ++local)
		{
			const size_t triangleIndex = chunkOffset + run.firstTriangle + local;
			if (triangleIndex >= triangles.size())
				continue;
			const SortedTriangle &triangle = triangles[triangleIndex];
			if (triangle.submissionIndex < submissions.size() &&
				triangle.sourceTriangle < submissions[
					triangle.submissionIndex].submittedTriangles.size())
			{
				submissions[triangle.submissionIndex].submittedTriangles[
					triangle.sourceTriangle] = 1;
			}
		}
	}
}

RenderResult SubmitChunk(NativeSortedGeometrySink &sink,
	const std::vector<NativeSortedDraw> &draws,
	const std::vector<DrawRun> &runs,
	const std::vector<SortedTriangle> &triangles, size_t chunkOffset,
	std::vector<SortedSubmission> &submissions,
	const std::vector<unsigned char> &vertices,
	const std::vector<unsigned short> &indices)
{
	if (draws.empty() || draws.size() != runs.size() || vertices.empty() ||
		indices.empty())
		return RENDER_RESULT_INVALID_ARGUMENT;
	unsigned int submittedDrawCount = 0;
	const RenderResult result = sink.SubmitNativeSortedBatch(draws.data(),
		static_cast<unsigned int>(draws.size()), vertices.data(),
		vertices.size(), indices.data(),
		indices.size() * sizeof(unsigned short), &submittedDrawCount);
	if (submittedDrawCount > draws.size())
		return RENDER_RESULT_FAILED;
	MarkRunsSubmitted(runs, triangles, chunkOffset, submittedDrawCount,
		submissions);
	if (result != RENDER_RESULT_OK)
		return result;
	return submittedDrawCount == draws.size() ? RENDER_RESULT_OK :
		RENDER_RESULT_FAILED;
}

} // namespace

namespace rts
{
namespace render
{

struct NativeSortingRenderer::Impl
{
	std::vector<SortedSubmission> submissions;
	size_t nextInsertionOrder;
	bool flushing;

	Impl() : submissions(), nextInsertionOrder(1), flushing(false) {}
};

NativeSortingRenderer::NativeSortingRenderer() : m_impl(new Impl())
{
}

NativeSortingRenderer::~NativeSortingRenderer()
{
	delete m_impl;
	m_impl = 0;
}

RenderResult NativeSortingRenderer::Queue(const LegacyLogicalState &state,
	const NativeDrawPacket &packet, const void *vertexData,
	size_t vertexBytes, const void *indexData, size_t indexBytes,
	const GameBoundingSphere *boundingSphere)
{
	size_t requiredVertexBytes = 0;
	size_t requiredIndexBytes = 0;
	if (m_impl == 0 || !ValidateSubmission(packet, vertexData, vertexBytes,
		indexData, indexBytes, boundingSphere, &requiredVertexBytes,
		&requiredIndexBytes))
		return RENDER_RESULT_INVALID_ARGUMENT;

	try
	{
		SortedSubmission submission;
		submission.state = state;
		submission.packet = packet;
		submission.vertices.resize(requiredVertexBytes);
		memcpy(submission.vertices.data(), vertexData, requiredVertexBytes);
		submission.indices.resize(packet.indexCount);
		memcpy(submission.indices.data(), indexData, requiredIndexBytes);
		if (boundingSphere != 0)
		{
			submission.sphere = *boundingSphere;
			// A zero-radius sphere is the legacy unsorted-node representation.
			submission.hasSphere = boundingSphere->radius > 0.0f;
		}
		submission.submittedTriangles.assign(packet.indexCount / 3, 0);
		submission.insertionOrder = m_impl->nextInsertionOrder++;
		if (m_impl->nextInsertionOrder == 0)
			m_impl->nextInsertionOrder = 1;
		m_impl->submissions.push_back(std::move(submission));
	}
	catch (const std::bad_alloc &)
	{
		return RENDER_RESULT_OUT_OF_MEMORY;
	}
	catch (...)
	{
		return RENDER_RESULT_FAILED;
	}
	return RENDER_RESULT_OK;
}

RenderResult NativeSortingRenderer::Flush(NativeSortedGeometrySink &sink)
{
#if defined(RTS_NATIVE_SORTING_TESTS)
	g_nativeSortingLastFlushScratchAllocationCount = 0;
	g_nativeSortingLastFlushPreparedGrowthCount = 0;
#endif
	if (m_impl == 0 || m_impl->submissions.empty())
		return RENDER_RESULT_OK;
	if (m_impl->flushing)
		return RENDER_RESULT_FAILED;
	FlushScope flushScope(m_impl->flushing);

	try
	{
		// SortingTriangleScratchLease is synchronously fenced by each kernel
		// call, so one Flush-local workspace safely serves every sorted node.
		rts::SortingTriangleScratchLease scratch;
		std::vector<rts::SortingTriangleOutput> prepared;
		std::vector<SortedNode> nodes;
		std::vector<size_t> positiveNodes;
		std::vector<size_t> unsortedNodes;
		nodes.reserve(m_impl->submissions.size());
		for (size_t index = 0; index < m_impl->submissions.size(); ++index)
		{
			const SortedSubmission &submission = m_impl->submissions[index];
			if (!HasPendingTriangles(submission))
				continue;
			SortedNode node;
			node.submissionIndex = index;
			node.hasSphere = submission.hasSphere;
			node.insertionOrder = submission.insertionOrder;
			MultiplySortingMatrices(submission.state.constants.world,
				submission.state.constants.view, node.worldView);
			if (!IsFiniteMatrix(node.worldView))
				return RENDER_RESULT_INVALID_ARGUMENT;
			node.centerDepth = submission.hasSphere ? TransformDepth(
				node.worldView,
				submission.sphere.centerX, submission.sphere.centerY,
				submission.sphere.centerZ) : 0.0f;
			if (!IsFiniteFloat(node.centerDepth))
				return RENDER_RESULT_INVALID_ARGUMENT;
			nodes.push_back(node);
			if (node.hasSphere)
				positiveNodes.push_back(nodes.size() - 1);
			else
				unsortedNodes.push_back(nodes.size() - 1);
		}

		// Preserve submission order for equal depths, matching the old insertion
		// rule while avoiding repeated vector shifts for every sphere node.
		std::stable_sort(positiveNodes.begin(), positiveNodes.end(),
			SortedNodeDepthDescending(nodes));

		// Match the legacy splice: all unsorted nodes are inserted before the
		// first sorted node whose transformed center is at or behind zero.
		size_t splice = positiveNodes.size();
		for (size_t index = 0; index < positiveNodes.size(); ++index)
		{
			if (nodes[positiveNodes[index]].centerDepth <= 0.0f)
			{
				splice = index;
				break;
			}
		}
		std::vector<size_t> nodeOrder;
		nodeOrder.reserve(nodes.size());
		nodeOrder.insert(nodeOrder.end(), positiveNodes.begin(),
			positiveNodes.begin() + splice);
		nodeOrder.insert(nodeOrder.end(), unsortedNodes.begin(),
			unsortedNodes.end());
		nodeOrder.insert(nodeOrder.end(), positiveNodes.begin() + splice,
			positiveNodes.end());

		std::vector<SortedTriangle> triangles;
		for (size_t order = 0; order < nodeOrder.size(); ++order)
		{
			const SortedNode &node = nodes[nodeOrder[order]];
			const size_t submissionIndex = node.submissionIndex;
			const SortedSubmission &submission = m_impl->submissions[
				submissionIndex];
			const RenderResult prepareResult = AppendPreparedTriangles(submission,
				submissionIndex, node.worldView, scratch, prepared, triangles);
#if defined(RTS_NATIVE_SORTING_TESTS)
			g_nativeSortingLastFlushScratchAllocationCount =
				scratch.allocationCount();
#endif
			if (prepareResult != RENDER_RESULT_OK)
				return prepareResult;
		}
		if (triangles.empty())
		{
			RetireCompletedSubmissions(m_impl->submissions);
			return RENDER_RESULT_OK;
		}

		Sort(triangles.data(), triangles.data() + triangles.size());

		for (size_t chunkOffset = 0; chunkOffset < triangles.size(); )
		{
			std::vector<unsigned char> chunkVertices;
			std::vector<unsigned short> chunkIndices;
			std::vector<NativeSortedDraw> draws;
			std::vector<DrawRun> runs;
			std::vector<size_t> vertexOffsets(m_impl->submissions.size(),
				std::numeric_limits<size_t>::max());
			const NativeDrawPacket *chunkPacket = 0;
			const size_t maximumChunkEnd = std::min(triangles.size(),
				chunkOffset + static_cast<size_t>(
					MAX_SORTING_TRIANGLES_PER_CHUNK));
			chunkIndices.reserve((maximumChunkEnd - chunkOffset) * 3);
			draws.reserve(maximumChunkEnd - chunkOffset);
			runs.reserve(maximumChunkEnd - chunkOffset);

			size_t local = 0;
			for (; chunkOffset + local < maximumChunkEnd; ++local)
			{
				const SortedTriangle &triangle = triangles[chunkOffset + local];
				const size_t submissionIndex = triangle.submissionIndex;
				const SortedSubmission &submission = m_impl->submissions[
					submissionIndex];
				if (submissionIndex >= vertexOffsets.size())
					return RENDER_RESULT_INVALID_ARGUMENT;
				if (vertexOffsets[submissionIndex] ==
					std::numeric_limits<size_t>::max())
				{
					if (chunkPacket != 0 &&
						!SameChunkGeometry(*chunkPacket, submission.packet))
						break;
					if (chunkPacket == 0)
						chunkPacket = &submission.packet;

					const size_t stride = submission.packet.vertexStride;
					if (stride == 0 || chunkVertices.size() % stride != 0)
						return RENDER_RESULT_INVALID_ARGUMENT;
					const size_t currentVertexCount = chunkVertices.size() / stride;
					if (submission.packet.vertexCount >
						MAX_SORTING_VERTEX_CAPACITY - currentVertexCount)
					{
						if (local == 0)
							return RENDER_RESULT_INVALID_ARGUMENT;
						break;
					}
					const size_t vertexOffset = chunkVertices.size();
					chunkVertices.insert(chunkVertices.end(),
						submission.vertices.begin(), submission.vertices.end());
					vertexOffsets[submissionIndex] = vertexOffset;
				}

				const size_t vertexOffset = vertexOffsets[submissionIndex];
				const size_t stride = submission.packet.vertexStride;
				if (stride == 0 || vertexOffset % stride != 0)
					return RENDER_RESULT_INVALID_ARGUMENT;
				const size_t vertexBase = vertexOffset / stride;
				if (vertexBase + triangle.i > MAX_SORTING_INDEX_COUNT ||
					vertexBase + triangle.j > MAX_SORTING_INDEX_COUNT ||
					vertexBase + triangle.k > MAX_SORTING_INDEX_COUNT)
					return RENDER_RESULT_INVALID_ARGUMENT;
				// Each draw binds vertexOffset at the beginning of this submission.
				// D3D11 adds the index to that offset, so indices stay local here.
				chunkIndices.push_back(triangle.i);
				chunkIndices.push_back(triangle.j);
				chunkIndices.push_back(triangle.k);
				if (vertexOffset > std::numeric_limits<unsigned int>::max())
					return RENDER_RESULT_OUT_OF_MEMORY;
				AddDrawRun(draws, runs, submission, submissionIndex, local,
					vertexOffset);
			}

			if (local == 0)
				return RENDER_RESULT_INVALID_ARGUMENT;
			const RenderResult result = SubmitChunk(sink, draws, runs, triangles,
				chunkOffset, m_impl->submissions, chunkVertices, chunkIndices);
			if (result != RENDER_RESULT_OK)
				return result;
			chunkOffset += local;
		}
	}
	catch (const std::bad_alloc &)
	{
		return RENDER_RESULT_OUT_OF_MEMORY;
	}
	catch (...)
	{
		return RENDER_RESULT_FAILED;
	}

	// Reaching this point means every chunk was fully acknowledged. Failures
	// return above with their unacknowledged triangles retained for retry, so
	// scanning every acknowledgement byte again here is unnecessary.
	m_impl->submissions.clear();
	return RENDER_RESULT_OK;
}

void NativeSortingRenderer::Clear()
{
	if (m_impl != 0)
		m_impl->submissions.clear();
}

bool NativeSortingRenderer::Empty() const
{
	return m_impl == 0 || m_impl->submissions.empty();
}

}
}

#if defined(RTS_NATIVE_SORTING_TESTS)
unsigned int NativeSortingRendererTestLastFlushScratchAllocationCount()
{
	return g_nativeSortingLastFlushScratchAllocationCount;
}

unsigned int NativeSortingRendererTestLastFlushPreparedGrowthCount()
{
	return g_nativeSortingLastFlushPreparedGrowthCount;
}

bool NativeSortingRendererTestRetireAllComplete()
{
	std::vector<SortedSubmission> submissions(3);
	for (size_t index = 0; index < submissions.size(); ++index)
		submissions[index].submittedTriangles.assign(3, 1);
	RetireCompletedSubmissions(submissions);
	return submissions.empty();
}

bool NativeSortingRendererTestRetireMixedPending()
{
	std::vector<SortedSubmission> submissions(5);
	for (size_t index = 0; index < submissions.size(); ++index)
	{
		submissions[index].insertionOrder = (index + 1) * 10;
		submissions[index].state.pipeline.shaderBits =
			static_cast<unsigned int>((index + 1) * 100);
		submissions[index].vertices.push_back(
			static_cast<unsigned char>(index + 1));
		submissions[index].indices.push_back(
			static_cast<unsigned short>(index + 11));
	}
	submissions[0].submittedTriangles.assign(2, 1);
	submissions[1].submittedTriangles.push_back(1);
	submissions[1].submittedTriangles.push_back(0);
	submissions[2].submittedTriangles.assign(1, 1);
	submissions[3].submittedTriangles.push_back(0);
	submissions[3].submittedTriangles.push_back(1);
	submissions[4].submittedTriangles.assign(2, 1);

	RetireCompletedSubmissions(submissions);
	if (submissions.size() != 2)
		return false;
	return submissions[0].insertionOrder == 20 &&
		submissions[0].state.pipeline.shaderBits == 200 &&
		submissions[0].submittedTriangles.size() == 2 &&
		submissions[0].submittedTriangles[0] == 1 &&
		submissions[0].submittedTriangles[1] == 0 &&
		submissions[0].vertices.size() == 1 &&
		submissions[0].vertices[0] == 2 &&
		submissions[0].indices.size() == 1 &&
		submissions[0].indices[0] == 12 &&
		submissions[1].insertionOrder == 40 &&
		submissions[1].state.pipeline.shaderBits == 400 &&
		submissions[1].submittedTriangles.size() == 2 &&
		submissions[1].submittedTriangles[0] == 0 &&
		submissions[1].submittedTriangles[1] == 1 &&
		submissions[1].vertices.size() == 1 &&
		submissions[1].vertices[0] == 4 &&
		submissions[1].indices.size() == 1 &&
		submissions[1].indices[0] == 14;
}
#endif
