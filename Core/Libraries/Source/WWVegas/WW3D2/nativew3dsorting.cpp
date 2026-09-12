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

void RetireCompletedSubmissions(std::vector<SortedSubmission> &submissions)
{
	for (std::vector<SortedSubmission>::iterator submission =
		submissions.begin(); submission != submissions.end(); )
	{
		if (!HasPendingTriangles(*submission))
			submission = submissions.erase(submission);
		else
			++submission;
	}
}

void InsertSortedNode(std::vector<size_t> &order,
	const std::vector<SortedNode> &nodes, size_t nodeIndex)
{
	for (std::vector<size_t>::iterator iter = order.begin(); iter != order.end();
		++iter)
	{
		if (nodes[nodeIndex].centerDepth > nodes[*iter].centerDepth)
		{
			order.insert(iter, nodeIndex);
			return;
		}
	}
	order.push_back(nodeIndex);
}

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

	rts::SortingTriangleScratchLease scratch;
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

		std::vector<rts::SortingTriangleOutput> prepared(batchCount);
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
		m_impl->submissions.push_back(submission);
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
	if (m_impl == 0 || m_impl->submissions.empty())
		return RENDER_RESULT_OK;
	if (m_impl->flushing)
		return RENDER_RESULT_FAILED;
	FlushScope flushScope(m_impl->flushing);

	try
	{
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
			float matrix[16];
			MultiplySortingMatrices(submission.state.constants.world,
				submission.state.constants.view, matrix);
			if (!IsFiniteMatrix(matrix))
				return RENDER_RESULT_INVALID_ARGUMENT;
			node.centerDepth = submission.hasSphere ? TransformDepth(matrix,
				submission.sphere.centerX, submission.sphere.centerY,
				submission.sphere.centerZ) : 0.0f;
			if (!IsFiniteFloat(node.centerDepth))
				return RENDER_RESULT_INVALID_ARGUMENT;
			nodes.push_back(node);
			if (node.hasSphere)
				InsertSortedNode(positiveNodes, nodes, nodes.size() - 1);
			else
				unsortedNodes.push_back(nodes.size() - 1);
		}

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
			const size_t submissionIndex = nodes[nodeOrder[order]].submissionIndex;
			const SortedSubmission &submission = m_impl->submissions[
				submissionIndex];
			float matrix[16];
			MultiplySortingMatrices(submission.state.constants.world,
				submission.state.constants.view, matrix);
			const RenderResult prepareResult = AppendPreparedTriangles(submission,
				submissionIndex, matrix, triangles);
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
				chunkIndices.push_back(static_cast<unsigned short>(
					vertexBase + triangle.i));
				chunkIndices.push_back(static_cast<unsigned short>(
					vertexBase + triangle.j));
				chunkIndices.push_back(static_cast<unsigned short>(
					vertexBase + triangle.k));
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

	RetireCompletedSubmissions(m_impl->submissions);
	return m_impl->submissions.empty() ? RENDER_RESULT_OK :
		RENDER_RESULT_FAILED;
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
