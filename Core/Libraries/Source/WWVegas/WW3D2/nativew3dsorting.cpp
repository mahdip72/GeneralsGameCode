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
#if defined(_WIN64) && !defined(NOMINMAX)
#define NOMINMAX
#endif
#include "Lib/FrameTimingDiagnostics.h"

#include <algorithm>
#include <limits>
#include <math.h>
#include <new>
#include <string.h>
#include <type_traits>
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
const unsigned long long MAX_RETAINED_FLUSH_WORKSPACE_BYTES =
	24ULL * 1024ULL * 1024ULL;

struct SortedSubmission
{
	SortedSubmission() : state(), packet(), vertices(), indices(), sphere(),
		hasSphere(false), awaitingBegin(false), insertionOrder(0), submittedTriangles() {}

	LegacyLogicalState state;
	NativeDrawPacket packet;
	NativeSortedPass pass;
	std::vector<unsigned char> vertices;
	std::vector<unsigned short> indices;
	GameBoundingSphere sphere;
	bool hasSphere;
	bool awaitingBegin;
	size_t insertionOrder;
	std::vector<unsigned char> submittedTriangles;
};

static_assert(std::is_nothrow_move_constructible<SortedSubmission>::value &&
	std::is_nothrow_move_assignable<SortedSubmission>::value,
	"Acknowledged interval erasure must not allocate or add resource references");

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
	DrawRun() : lastSubmissionIndex(0), firstTriangle(0), triangleCount(0),
		chunkIndexed(false) {}

	size_t lastSubmissionIndex;
	size_t firstTriangle;
	size_t triangleCount;
	bool chunkIndexed; // This run's indices address the complete packed chunk.
};

struct FlushWorkspace
{
	std::vector<rts::SortingTriangleDescriptor> preparationDescriptors;
	std::vector<size_t> preparationNodes;
	std::vector<rts::SortingTriangleOutput> prepared;
	std::vector<SortedNode> nodes;
	std::vector<size_t> positiveNodes;
	std::vector<size_t> unsortedNodes;
	std::vector<size_t> nodeOrder;
	std::vector<SortedTriangle> triangles;
	std::vector<unsigned char> chunkVertices;
	std::vector<unsigned short> chunkIndices;
	std::vector<NativeSortedDraw> draws;
	std::vector<DrawRun> runs;
	std::vector<size_t> vertexOffsets;
	std::vector<size_t> touchedSubmissionIndexes;

	void Clear()
	{
		preparationDescriptors.clear();
		preparationNodes.clear();
		prepared.clear();
		nodes.clear();
		positiveNodes.clear();
		unsortedNodes.clear();
		nodeOrder.clear();
		triangles.clear();
		chunkVertices.clear();
		chunkIndices.clear();
		draws.clear();
		runs.clear();
		vertexOffsets.clear();
		touchedSubmissionIndexes.clear();
	}

	unsigned long long RetainedCapacityBytes() const
	{
		return VectorCapacityBytes(preparationDescriptors) +
			VectorCapacityBytes(preparationNodes) +
			VectorCapacityBytes(prepared) + VectorCapacityBytes(nodes) +
			VectorCapacityBytes(positiveNodes) +
			VectorCapacityBytes(unsortedNodes) +
			VectorCapacityBytes(nodeOrder) + VectorCapacityBytes(triangles) +
			VectorCapacityBytes(chunkVertices) +
			VectorCapacityBytes(chunkIndices) + VectorCapacityBytes(draws) +
			VectorCapacityBytes(runs) + VectorCapacityBytes(vertexOffsets) +
			VectorCapacityBytes(touchedSubmissionIndexes);
	}

	void TrimToRetainedCapacityBudget()
	{
		unsigned long long retainedBytes = RetainedCapacityBytes();
		if (retainedBytes <= MAX_RETAINED_FLUSH_WORKSPACE_BYTES)
			return;
		ReleaseCapacity(draws, retainedBytes);
		ReleaseCapacity(triangles, retainedBytes);
		ReleaseCapacity(prepared, retainedBytes);
		ReleaseCapacity(preparationDescriptors, retainedBytes);
		ReleaseCapacity(preparationNodes, retainedBytes);
		ReleaseCapacity(chunkVertices, retainedBytes);
		ReleaseCapacity(chunkIndices, retainedBytes);
		ReleaseCapacity(runs, retainedBytes);
		ReleaseCapacity(vertexOffsets, retainedBytes);
		ReleaseCapacity(touchedSubmissionIndexes, retainedBytes);
		ReleaseCapacity(nodes, retainedBytes);
		ReleaseCapacity(positiveNodes, retainedBytes);
		ReleaseCapacity(unsortedNodes, retainedBytes);
		ReleaseCapacity(nodeOrder, retainedBytes);
	}

private:
	template <typename T>
	static unsigned long long VectorCapacityBytes(const std::vector<T> &value)
	{
		return static_cast<unsigned long long>(value.capacity()) * sizeof(T);
	}

	template <typename T>
	static void ReleaseCapacity(std::vector<T> &value,
		unsigned long long &retainedBytes)
	{
		if (retainedBytes <= MAX_RETAINED_FLUSH_WORKSPACE_BYTES)
			return;
		const unsigned long long capacityBytes = VectorCapacityBytes(value);
		if (capacityBytes == 0)
			return;
		std::vector<T> empty;
		value.swap(empty);
		retainedBytes -= capacityBytes;
	}
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
unsigned long long g_nativeSortingLastFlushWorkspaceCapacityBytes = 0;
unsigned long long g_nativeSortingLastOffsetInitializations = 0;
unsigned long long g_nativeSortingLastOffsetResets = 0;
bool g_nativeSortingSingleDescriptorReference = false;
unsigned int g_nativeSortingLastParallelBatches = 0;
#endif

class FlushWorkspaceScope
{
public:
	explicit FlushWorkspaceScope(FlushWorkspace &workspace)
		: workspace(workspace)
	{
		this->workspace.Clear();
	}

	~FlushWorkspaceScope()
	{
		workspace.Clear();
		workspace.TrimToRetainedCapacityBudget();
#if defined(RTS_NATIVE_SORTING_TESTS)
		g_nativeSortingLastFlushWorkspaceCapacityBytes =
			workspace.RetainedCapacityBytes();
#endif
	}

private:
	FlushWorkspace &workspace;
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

#if defined(RTS_NATIVE_SORTING_TESTS)
bool HasPendingTriangles(const SortedSubmission &submission)
{
	for (size_t index = 0; index < submission.submittedTriangles.size(); ++index)
	{
		if (!submission.submittedTriangles[index])
			return true;
	}
	return false;
}

// Retained for the existing retirement-helper assertions. Product retirement
// now removes only a whole completed cohort so tie-sort input stays intact.
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
#endif

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

// The descriptor and triangle ceilings bound each synchronous preparation call,
// independent of cohort size. Source bytes and transforms stay owner-owned and
// immutable until the kernel has fenced; output and scratch are separate arenas.
RenderResult AppendPreparedTriangleBatches(
	const std::vector<SortedSubmission> &submissions,
	FlushWorkspace &workspace, rts::SortingTriangleScratchLease &scratch)
{
	std::vector<rts::SortingTriangleDescriptor> &descriptors =
		workspace.preparationDescriptors;
	std::vector<size_t> &preparationNodes = workspace.preparationNodes;
	std::vector<rts::SortingTriangleOutput> &prepared = workspace.prepared;
	rts::SortingTriangleOptions options;
	options.parallel = true;
	unsigned int descriptorLimit = rts::SORTING_TRIANGLE_MAX_DESCRIPTORS;
#if defined(RTS_NATIVE_SORTING_TESTS)
	if (g_nativeSortingSingleDescriptorReference)
	{
		descriptorLimit = 1;
		options.parallel = false;
	}
#endif
	size_t order = 0;
	unsigned int sourceStart = 0;
	while (order < workspace.nodeOrder.size())
	{
		descriptors.clear();
		preparationNodes.clear();
		unsigned int batchCount = 0;
		while (order < workspace.nodeOrder.size() &&
			descriptors.size() < descriptorLimit &&
			batchCount < MAX_SORTING_TRIANGLES_PER_KERNEL_CALL)
		{
			const size_t nodeIndex = workspace.nodeOrder[order];
			const SortedNode &node = workspace.nodes[nodeIndex];
			const SortedSubmission &submission = submissions[node.submissionIndex];
			if (sourceStart == 0 && !ValidateSourceIndices(submission))
				return RENDER_RESULT_INVALID_ARGUMENT;
			const unsigned int triangleCount = submission.packet.indexCount / 3U;
			const unsigned int count = std::min(triangleCount - sourceStart,
				MAX_SORTING_TRIANGLES_PER_KERNEL_CALL - batchCount);
			rts::SortingTriangleDescriptor descriptor;
			descriptor.vertices = submission.vertices.data();
			descriptor.vertexStrideBytes = submission.packet.vertexStride;
			descriptor.indices = submission.indices.data() +
				static_cast<size_t>(sourceStart) * 3;
			descriptor.minVertexIndex = static_cast<unsigned short>(
				submission.packet.minimumVertexIndex);
			descriptor.vertexCount = static_cast<unsigned short>(
				submission.packet.vertexCount);
			descriptor.polygonCount = static_cast<unsigned short>(count);
			descriptor.vertexOffset = 0;
			descriptor.outputOffset = batchCount;
			// The kernel's output identity is ushort, so use the bounded batch
			// ordinal rather than the potentially unbounded cohort node index.
			descriptor.nodeIndex = static_cast<unsigned int>(descriptors.size());
			descriptor.zX = node.worldView[0 * 4 + 2];
			descriptor.zY = node.worldView[1 * 4 + 2];
			descriptor.zZ = node.worldView[2 * 4 + 2];
			descriptor.zTranslation = node.worldView[3 * 4 + 2];
			descriptor.commonZ = descriptor.zX == 0.0f && descriptor.zY == 0.0f &&
				descriptor.zTranslation == 0.0f && descriptor.zZ == 1.0f ? 1U : 0U;
			descriptors.push_back(descriptor);
			preparationNodes.push_back(nodeIndex);
			batchCount += count;
			sourceStart += count;
			if (sourceStart == triangleCount)
			{
				++order;
				sourceStart = 0;
			}
		}
		const unsigned int descriptorCount = static_cast<unsigned int>(descriptors.size());
		if (!scratch.prepare(descriptorCount, batchCount, options.maximumScratchBytes))
			return RENDER_RESULT_OUT_OF_MEMORY;
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
			descriptors.data(), descriptorCount, batchCount, prepared.data(),
			scratch.outputs(), options, &metrics);
#if defined(RTS_NATIVE_SORTING_TESTS)
		if (result == rts::SORTING_TRIANGLE_PARALLEL)
			++g_nativeSortingLastParallelBatches;
#endif
		if (result == rts::SORTING_TRIANGLE_SERIAL_FALLBACK)
		{
			// All accepted worker jobs have drained before this result. Keep
			// descriptors/source/output alive and rerun the identical serial kernel.
			const bool parallel = options.parallel;
			options.parallel = false;
			result = rts::PrepareSortingTriangles(descriptors.data(), descriptorCount,
				batchCount, prepared.data(), scratch.outputs(), options, &metrics);
			options.parallel = parallel;
		}
		if (!rts::SortingTriangleCompleted(result))
			return result == rts::SORTING_TRIANGLE_INVALID_INPUT ?
				RENDER_RESULT_INVALID_ARGUMENT : RENDER_RESULT_FAILED;
		for (unsigned int descriptorIndex = 0; descriptorIndex < descriptorCount;
			++descriptorIndex)
		{
			const rts::SortingTriangleDescriptor &descriptor = descriptors[descriptorIndex];
			const size_t submissionIndex = workspace.nodes[
				preparationNodes[descriptorIndex]].submissionIndex;
			const SortedSubmission &submission = submissions[submissionIndex];
			const unsigned int firstTriangle = static_cast<unsigned int>(
				(descriptor.indices - submission.indices.data()) / 3);
			for (unsigned int local = 0; local < descriptor.polygonCount; ++local)
			{
				const rts::SortingTriangleOutput &output = prepared[descriptor.outputOffset + local];
				if (!IsFiniteFloat(output.z))
					return RENDER_RESULT_INVALID_ARGUMENT;
				SortedTriangle triangle;
				triangle.submissionIndex = submissionIndex;
				triangle.sourceTriangle = firstTriangle + local;
				triangle.i = output.tri.i;
				triangle.j = output.tri.j;
				triangle.k = output.tri.k;
				triangle.depth = output.z;
				workspace.triangles.push_back(triangle);
			}
		}
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

bool SameFloatBits(const float &left, const float &right)
{
	return memcmp(&left, &right, sizeof(left)) == 0;
}

bool SameRenderFloat4(const RenderFloat4 &left, const RenderFloat4 &right)
{
	return SameFloatBits(left.x, right.x) &&
		SameFloatBits(left.y, right.y) &&
		SameFloatBits(left.z, right.z) &&
		SameFloatBits(left.w, right.w);
}

bool SameRenderMatrix4(const RenderMatrix4 &left, const RenderMatrix4 &right)
{
	for (unsigned int index = 0; index < 16; ++index)
	{
		if (!SameFloatBits(left.values[index], right.values[index]))
			return false;
	}
	return true;
}

bool SameLegacyBlendState(const LegacyBlendState &left,
	const LegacyBlendState &right)
{
	return left.blendEnable == right.blendEnable &&
		left.sourceColor == right.sourceColor &&
		left.destinationColor == right.destinationColor &&
		left.colorOperation == right.colorOperation &&
		left.sourceAlpha == right.sourceAlpha &&
		left.destinationAlpha == right.destinationAlpha &&
		left.alphaOperation == right.alphaOperation &&
		left.colorWriteMask == right.colorWriteMask;
}

bool SameLegacyDepthStencilState(const LegacyDepthStencilState &left,
	const LegacyDepthStencilState &right)
{
	return left.depthEnable == right.depthEnable &&
		left.depthWrite == right.depthWrite &&
		left.depthFunction == right.depthFunction &&
		left.stencilEnable == right.stencilEnable &&
		left.stencilReadMask == right.stencilReadMask &&
		left.stencilWriteMask == right.stencilWriteMask &&
		left.stencilReference == right.stencilReference &&
		left.stencilFunction == right.stencilFunction &&
		left.stencilFail == right.stencilFail &&
		left.stencilDepthFail == right.stencilDepthFail &&
		left.stencilPass == right.stencilPass;
}

bool SameLegacyRasterizerState(const LegacyRasterizerState &left,
	const LegacyRasterizerState &right)
{
	return left.fillMode == right.fillMode &&
		left.cullMode == right.cullMode &&
		left.frontCounterClockwise == right.frontCounterClockwise &&
		left.scissorEnable == right.scissorEnable &&
		left.depthBias == right.depthBias &&
		SameFloatBits(left.slopeScaledDepthBias, right.slopeScaledDepthBias);
}

bool SameLegacySamplerState(const LegacySamplerState &left,
	const LegacySamplerState &right)
{
	return left.addressU == right.addressU &&
		left.addressV == right.addressV &&
		left.addressW == right.addressW &&
		left.minification == right.minification &&
		left.magnification == right.magnification &&
		left.mipmapping == right.mipmapping &&
		left.maximumAnisotropy == right.maximumAnisotropy &&
		left.maximumMipLevel == right.maximumMipLevel &&
		SameFloatBits(left.mipLodBias, right.mipLodBias) &&
		SameRenderFloat4(left.borderColor, right.borderColor);
}

bool SameLegacyTextureStageState(const LegacyTextureStageState &left,
	const LegacyTextureStageState &right)
{
	return left.colorOperation == right.colorOperation &&
		left.colorArgument0 == right.colorArgument0 &&
		left.colorArgument1 == right.colorArgument1 &&
		left.colorArgument2 == right.colorArgument2 &&
		left.alphaOperation == right.alphaOperation &&
		left.alphaArgument0 == right.alphaArgument0 &&
		left.alphaArgument1 == right.alphaArgument1 &&
		left.alphaArgument2 == right.alphaArgument2 &&
		left.colorArgument0Complement == right.colorArgument0Complement &&
		left.colorArgument0AlphaReplicate == right.colorArgument0AlphaReplicate &&
		left.colorArgument1Complement == right.colorArgument1Complement &&
		left.colorArgument1AlphaReplicate == right.colorArgument1AlphaReplicate &&
		left.colorArgument2Complement == right.colorArgument2Complement &&
		left.colorArgument2AlphaReplicate == right.colorArgument2AlphaReplicate &&
		left.alphaArgument0Complement == right.alphaArgument0Complement &&
		left.alphaArgument0AlphaReplicate == right.alphaArgument0AlphaReplicate &&
		left.alphaArgument1Complement == right.alphaArgument1Complement &&
		left.alphaArgument1AlphaReplicate == right.alphaArgument1AlphaReplicate &&
		left.alphaArgument2Complement == right.alphaArgument2Complement &&
		left.alphaArgument2AlphaReplicate == right.alphaArgument2AlphaReplicate &&
		left.resultArgument == right.resultArgument &&
		left.textureCoordinateIndex == right.textureCoordinateIndex &&
		left.cameraSpacePosition == right.cameraSpacePosition &&
		left.cameraSpaceNormal == right.cameraSpaceNormal &&
		left.cameraSpaceReflectionVector == right.cameraSpaceReflectionVector &&
		left.textureTransformEnable == right.textureTransformEnable &&
		left.projectedCoordinates == right.projectedCoordinates &&
		left.textureTransformCount == right.textureTransformCount &&
		SameFloatBits(left.bumpEnvironmentMatrix00,
			right.bumpEnvironmentMatrix00) &&
		SameFloatBits(left.bumpEnvironmentMatrix01,
			right.bumpEnvironmentMatrix01) &&
		SameFloatBits(left.bumpEnvironmentMatrix10,
			right.bumpEnvironmentMatrix10) &&
		SameFloatBits(left.bumpEnvironmentMatrix11,
			right.bumpEnvironmentMatrix11) &&
		SameFloatBits(left.bumpEnvironmentLuminanceScale,
			right.bumpEnvironmentLuminanceScale) &&
		SameFloatBits(left.bumpEnvironmentLuminanceOffset,
			right.bumpEnvironmentLuminanceOffset) &&
		SameLegacySamplerState(left.sampler, right.sampler);
}

bool SameLegacyPipelineState(const LegacyPipelineState &left,
	const LegacyPipelineState &right)
{
	if (left.shaderBits != right.shaderBits ||
		left.pixelProgram != right.pixelProgram ||
		left.vertexProgram != right.vertexProgram ||
		!SameLegacyBlendState(left.blend, right.blend) ||
		!SameLegacyDepthStencilState(left.depthStencil, right.depthStencil) ||
		!SameLegacyRasterizerState(left.rasterizer, right.rasterizer) ||
		left.fogMode != right.fogMode ||
		left.rangeFogEnable != right.rangeFogEnable ||
		left.secondaryGradientEnable != right.secondaryGradientEnable ||
		left.nPatchEnable != right.nPatchEnable ||
		left.lightingEnable != right.lightingEnable ||
		left.normalizeNormals != right.normalizeNormals ||
		left.alphaTestEnable != right.alphaTestEnable ||
		left.alphaFunction != right.alphaFunction ||
		left.alphaReference != right.alphaReference ||
		left.textureFactor != right.textureFactor ||
		left.clipPlaneEnableMask != right.clipPlaneEnableMask ||
		left.ambientMaterialSource != right.ambientMaterialSource ||
		left.diffuseMaterialSource != right.diffuseMaterialSource ||
		left.emissiveMaterialSource != right.emissiveMaterialSource ||
		left.specularMaterialSource != right.specularMaterialSource)
		return false;
	for (unsigned int stage = 0; stage < LEGACY_TEXTURE_STAGE_COUNT; ++stage)
	{
		if (!SameLegacyTextureStageState(left.textureStages[stage],
			right.textureStages[stage]))
			return false;
	}
	return true;
}

bool SameLegacyMaterialState(const LegacyMaterialState &left,
	const LegacyMaterialState &right)
{
	return SameRenderFloat4(left.diffuse, right.diffuse) &&
		SameRenderFloat4(left.ambient, right.ambient) &&
		SameRenderFloat4(left.specular, right.specular) &&
		SameRenderFloat4(left.emissive, right.emissive) &&
		SameFloatBits(left.specularPower, right.specularPower);
}

bool SameLegacyLightState(const LegacyLightState &left,
	const LegacyLightState &right)
{
	return left.enabled == right.enabled && left.type == right.type &&
		SameRenderFloat4(left.diffuse, right.diffuse) &&
		SameRenderFloat4(left.specular, right.specular) &&
		SameRenderFloat4(left.ambient, right.ambient) &&
		SameRenderFloat4(left.position, right.position) &&
		SameRenderFloat4(left.direction, right.direction) &&
		SameFloatBits(left.range, right.range) &&
		SameFloatBits(left.falloff, right.falloff) &&
		SameFloatBits(left.attenuation0, right.attenuation0) &&
		SameFloatBits(left.attenuation1, right.attenuation1) &&
		SameFloatBits(left.attenuation2, right.attenuation2) &&
		SameFloatBits(left.theta, right.theta) &&
		SameFloatBits(left.phi, right.phi);
}

bool SameLegacyFogConstants(const LegacyFogConstants &left,
	const LegacyFogConstants &right)
{
	return left.enabled == right.enabled &&
		SameRenderFloat4(left.color, right.color) &&
		SameFloatBits(left.start, right.start) &&
		SameFloatBits(left.end, right.end) &&
		SameFloatBits(left.density, right.density);
}

bool SameLegacyFixedFunctionConstants(
	const LegacyFixedFunctionConstants &left,
	const LegacyFixedFunctionConstants &right)
{
	if (!SameRenderMatrix4(left.world, right.world) ||
		!SameRenderMatrix4(left.view, right.view) ||
		!SameRenderMatrix4(left.projection, right.projection) ||
		!SameLegacyMaterialState(left.material, right.material) ||
		!SameLegacyFogConstants(left.fog, right.fog) ||
		!SameRenderFloat4(left.globalAmbient, right.globalAmbient))
		return false;
	for (unsigned int stage = 0; stage < LEGACY_TEXTURE_STAGE_COUNT; ++stage)
	{
		if (!SameRenderMatrix4(left.textureTransforms[stage],
			right.textureTransforms[stage]))
			return false;
	}
	for (unsigned int light = 0; light < LEGACY_LIGHT_COUNT; ++light)
	{
		if (!SameLegacyLightState(left.lights[light], right.lights[light]))
			return false;
	}
	for (unsigned int plane = 0; plane < LEGACY_CLIP_PLANE_COUNT; ++plane)
	{
		if (!SameRenderFloat4(left.clipPlanes[plane], right.clipPlanes[plane]))
			return false;
	}
	for (unsigned int index = 0; index < LEGACY_VERTEX_CONSTANT_COUNT; ++index)
	{
		if (!SameRenderFloat4(left.vertexShaderConstants[index],
			right.vertexShaderConstants[index]))
			return false;
	}
	for (unsigned int index = 0; index < LEGACY_PIXEL_CONSTANT_COUNT; ++index)
	{
		if (!SameRenderFloat4(left.pixelShaderConstants[index],
			right.pixelShaderConstants[index]))
			return false;
	}
	return true;
}

bool SameLegacyLogicalState(const LegacyLogicalState &left,
	const LegacyLogicalState &right)
{
	return SameLegacyPipelineState(left.pipeline, right.pipeline) &&
		SameLegacyFixedFunctionConstants(left.constants, right.constants) &&
		left.texturePresenceMask == right.texturePresenceMask;
}

bool SameSortedDrawBindings(const NativeSortedDraw &left,
	const SortedSubmission &right)
{
	// Compare every logical field because raw structure bytes include padding.
	if (!SameLegacyLogicalState(left.state, right.state) ||
		!SameChunkGeometry(left.packet, right.packet) ||
		left.packet.texturePresenceMask != right.packet.texturePresenceMask)
		return false;
	for (unsigned int stage = 0; stage < LEGACY_TEXTURE_STAGE_COUNT; ++stage)
	{
		if (left.packet.textures[stage] != right.packet.textures[stage])
			return false;
	}
	return true;
}

void ExtendDrawRun(std::vector<NativeSortedDraw> &draws,
	std::vector<DrawRun> &runs, size_t submissionIndex)
{
	++runs.back().triangleCount;
	draws.back().packet.indexCount += 3;
	runs.back().lastSubmissionIndex = submissionIndex;
}

void StartDrawRun(std::vector<NativeSortedDraw> &draws,
	std::vector<DrawRun> &runs, const SortedSubmission &submission,
	size_t submissionIndex, size_t localTriangle, size_t vertexOffset)
{
	NativeSortedDraw draw = { submission.state, submission.packet };
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
	run.lastSubmissionIndex = submissionIndex;
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
	std::vector<SortedSubmission> &submissions, size_t cohortStart, size_t cohortSize,
	size_t &acknowledgedCohortStart, size_t &acknowledgedCohortSize,
	const std::vector<unsigned char> &vertices,
	const std::vector<unsigned short> &indices)
{
	if (draws.empty() || draws.size() != runs.size() || vertices.empty() ||
		indices.empty())
		return RENDER_RESULT_INVALID_ARGUMENT;
	unsigned int submittedDrawCount = 0;
	const NativeSortedPass &pass = submissions[cohortStart].pass;
	const RenderResult result = sink.SubmitNativeSortedPassBatch(pass, draws.data(),
		static_cast<unsigned int>(draws.size()), vertices.data(),
		vertices.size(), indices.data(),
		indices.size() * sizeof(unsigned short), &submittedDrawCount);
	if (submittedDrawCount > draws.size())
		return RENDER_RESULT_FAILED;
	MarkRunsSubmitted(runs, triangles, chunkOffset, submittedDrawCount,
		submissions);
	// Seal on any valid acknowledgement, including a successful earlier chunk
	// before a later failure. Queue continues appending outside this cohort.
	if (submittedDrawCount != 0 && acknowledgedCohortSize == 0)
	{
		acknowledgedCohortStart = cohortStart;
		acknowledgedCohortSize = cohortSize;
	}
	if (result != RENDER_RESULT_OK)
		return result;
	return submittedDrawCount == draws.size() ? RENDER_RESULT_OK :
		RENDER_RESULT_FAILED;
}

} // namespace

static bool SameSortedTarget(const rts::render::RenderTargetBinding &a,
	const rts::render::RenderTargetBinding &b)
{
	return a.useBackBufferColor == b.useBackBufferColor &&
		a.useBackBufferDepth == b.useBackBufferDepth &&
		a.hasColor == b.hasColor && a.hasDepth == b.hasDepth &&
		a.color.resource == b.color.resource && a.color.mip == b.color.mip &&
		a.color.arraySlice == b.color.arraySlice &&
		a.depth.resource == b.depth.resource && a.depth.mip == b.depth.mip &&
		a.depth.arraySlice == b.depth.arraySlice;
}

static bool SameSortedPass(const rts::render::NativeSortedPass &a,
	const rts::render::NativeSortedPass &b)
{
	if (!a.captured || !b.captured)
		return a.captured == b.captured;
	return a.identity == b.identity && SameSortedTarget(a.target, b.target) &&
		a.viewport.x == b.viewport.x && a.viewport.y == b.viewport.y &&
		a.viewport.width == b.viewport.width && a.viewport.height == b.viewport.height &&
		a.viewport.minimumDepth == b.viewport.minimumDepth &&
		a.viewport.maximumDepth == b.viewport.maximumDepth;
}

namespace rts
{
namespace render
{

struct NativeSortingRenderer::Impl
{
	std::vector<SortedSubmission> submissions;
	FlushWorkspace workspace;
	size_t nextInsertionOrder;
	size_t acknowledgedCohortStart;
	size_t acknowledgedCohortSize;
	bool flushing;

	Impl() : submissions(), workspace(), nextInsertionOrder(1),
		acknowledgedCohortStart(0), acknowledgedCohortSize(0), flushing(false) {}
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
	const GameBoundingSphere *boundingSphere, const NativeSortedPass *pass,
	bool awaitMatchingBegin)
{
	size_t requiredVertexBytes = 0;
	size_t requiredIndexBytes = 0;
	if (m_impl == 0 || (awaitMatchingBegin && (pass == 0 || !pass->captured)) ||
		!ValidateSubmission(packet, vertexData, vertexBytes,
		indexData, indexBytes, boundingSphere, &requiredVertexBytes,
		&requiredIndexBytes))
		return RENDER_RESULT_INVALID_ARGUMENT;

	try
	{
		SortedSubmission submission;
		submission.state = state;
		submission.packet = packet;
		if (pass != 0)
			submission.pass = *pass;
		submission.awaitingBegin = awaitMatchingBegin;
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

void NativeSortingRenderer::ActivateMatchingPass(const RenderTargetBinding &target,
	NativeW3DSubmissionSequence identity)
{
	if (m_impl == 0 || m_impl->flushing)
		return;
	for (size_t index = 0; index < m_impl->submissions.size(); ++index)
	{
		SortedSubmission &submission = m_impl->submissions[index];
		if (submission.awaitingBegin && SameSortedTarget(submission.pass.target, target))
		{
			submission.pass.identity = identity;
			submission.awaitingBegin = false;
		}
	}
}

RenderResult NativeSortingRenderer::Flush(NativeSortedGeometrySink &sink,
	bool firstCohortOnly)
{
	rts::frame_timing::Scope sortingTiming(rts::frame_timing::RendererSorting);
#if defined(RTS_NATIVE_SORTING_TESTS)
	g_nativeSortingLastFlushScratchAllocationCount = 0;
	g_nativeSortingLastFlushPreparedGrowthCount = 0;
	g_nativeSortingLastOffsetInitializations = 0;
	g_nativeSortingLastOffsetResets = 0;
	g_nativeSortingLastParallelBatches = 0;
#endif
	if (m_impl == 0 || m_impl->submissions.empty())
		return RENDER_RESULT_OK;
	if (m_impl->flushing)
		return RENDER_RESULT_FAILED;
	FlushScope flushScope(m_impl->flushing);
	FlushWorkspaceScope workspaceScope(m_impl->workspace);

nextCohort:
	const bool retryingAcknowledgedCohort =
		m_impl->acknowledgedCohortSize != 0;
	size_t cohortStart = m_impl->acknowledgedCohortStart;
	size_t cohortSize = m_impl->acknowledgedCohortSize;
	if (!retryingAcknowledgedCohort)
	{
		cohortStart = 0;
		while (cohortStart < m_impl->submissions.size() &&
			m_impl->submissions[cohortStart].awaitingBegin)
			++cohortStart;
		if (cohortStart == m_impl->submissions.size())
			return RENDER_RESULT_OK;
		cohortSize = 1;
		while (cohortStart + cohortSize < m_impl->submissions.size() &&
			!m_impl->submissions[cohortStart + cohortSize].awaitingBegin && SameSortedPass(
			m_impl->submissions[cohortStart].pass,
			m_impl->submissions[cohortStart + cohortSize].pass))
			++cohortSize;
	}
	try
	{
		// SortingTriangleScratchLease is synchronously fenced by each kernel
		// call, so one workspace safely serves every sorted node and Flush.
		rts::SortingTriangleScratchLease scratch;
		std::vector<SortedNode> &nodes = m_impl->workspace.nodes;
		std::vector<size_t> &positiveNodes = m_impl->workspace.positiveNodes;
		std::vector<size_t> &unsortedNodes = m_impl->workspace.unsortedNodes;
		nodes.reserve(cohortSize);
		for (size_t index = cohortStart; index < cohortStart + cohortSize; ++index)
		{
			const SortedSubmission &submission = m_impl->submissions[index];
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
		std::vector<size_t> &nodeOrder = m_impl->workspace.nodeOrder;
		nodeOrder.reserve(nodes.size());
		nodeOrder.insert(nodeOrder.end(), positiveNodes.begin(),
			positiveNodes.begin() + splice);
		nodeOrder.insert(nodeOrder.end(), unsortedNodes.begin(),
			unsortedNodes.end());
		nodeOrder.insert(nodeOrder.end(), positiveNodes.begin() + splice,
			positiveNodes.end());

		std::vector<SortedTriangle> &triangles = m_impl->workspace.triangles;
		// Queue initializes all acknowledgements to zero; only an accepted
		// draw can set them, and it also fixes the acknowledged retry cohort.
		// Prepare the full cohort in both cases to retain legacy equal-depth ties.
		const RenderResult prepareResult = AppendPreparedTriangleBatches(
			m_impl->submissions, m_impl->workspace, scratch);
#if defined(RTS_NATIVE_SORTING_TESTS)
		g_nativeSortingLastFlushScratchAllocationCount = scratch.allocationCount();
#endif
		if (prepareResult != RENDER_RESULT_OK)
			return prepareResult;
		if (!triangles.empty())
			Sort(triangles.data(), triangles.data() + triangles.size());
		if (retryingAcknowledgedCohort)
		{
			// The legacy depth-only quicksort is not stable for ties. Recreate
			// its exact full-cohort permutation BEFORE removing acknowledgements,
			// including completed nodes which influenced the original permutation.
			size_t pendingCount = 0;
			for (size_t index = 0; index < triangles.size(); ++index)
			{
				const SortedTriangle &triangle = triangles[index];
				if (!m_impl->submissions[triangle.submissionIndex].
					submittedTriangles[triangle.sourceTriangle])
					triangles[pendingCount++] = triangle;
			}
			triangles.resize(pendingCount);
		}

		std::vector<size_t> &vertexOffsets = m_impl->workspace.vertexOffsets;
		std::vector<size_t> &touchedSubmissionIndexes =
			m_impl->workspace.touchedSubmissionIndexes;
		vertexOffsets.assign(cohortSize, std::numeric_limits<size_t>::max());
	#if defined(RTS_NATIVE_SORTING_TESTS)
		g_nativeSortingLastOffsetInitializations += cohortSize;
	#endif
		for (size_t chunkOffset = 0; chunkOffset < triangles.size(); )
		{
			std::vector<unsigned char> &chunkVertices =
				m_impl->workspace.chunkVertices;
			std::vector<unsigned short> &chunkIndices =
				m_impl->workspace.chunkIndices;
			std::vector<NativeSortedDraw> &draws = m_impl->workspace.draws;
			std::vector<DrawRun> &runs = m_impl->workspace.runs;
			chunkVertices.clear();
			chunkIndices.clear();
			draws.clear();
			// A coalesced draw may span several submissions, so chunk ownership
			// cannot be recovered from DrawRun alone. Reset each packed source once.
			for (size_t touched = 0;
				touched < touchedSubmissionIndexes.size(); ++touched)
			{
				vertexOffsets[touchedSubmissionIndexes[touched]] =
					std::numeric_limits<size_t>::max();
			#if defined(RTS_NATIVE_SORTING_TESTS)
				++g_nativeSortingLastOffsetResets;
			#endif
			}
			touchedSubmissionIndexes.clear();
			runs.clear();
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
				if (submissionIndex < cohortStart || submissionIndex - cohortStart >= vertexOffsets.size())
					return RENDER_RESULT_INVALID_ARGUMENT;
				const size_t relativeIndex = submissionIndex - cohortStart;
				if (vertexOffsets[relativeIndex] ==
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
					vertexOffsets[relativeIndex] = vertexOffset;
					touchedSubmissionIndexes.push_back(relativeIndex);
				}

				const size_t vertexOffset = vertexOffsets[relativeIndex];
				const size_t stride = submission.packet.vertexStride;
				if (stride == 0 || vertexOffset % stride != 0)
					return RENDER_RESULT_INVALID_ARGUMENT;
				const size_t vertexBase = vertexOffset / stride;
				if (vertexBase + triangle.i > MAX_SORTING_INDEX_COUNT ||
					vertexBase + triangle.j > MAX_SORTING_INDEX_COUNT ||
					vertexBase + triangle.k > MAX_SORTING_INDEX_COUNT)
					return RENDER_RESULT_INVALID_ARGUMENT;
				if (vertexOffset > std::numeric_limits<unsigned int>::max())
					return RENDER_RESULT_OUT_OF_MEMORY;

				const bool adjacentToPreviousRun = !runs.empty() &&
					runs.back().firstTriangle + runs.back().triangleCount == local;
				const bool sameSourceRun = adjacentToPreviousRun &&
					runs.back().lastSubmissionIndex == submissionIndex;
				const bool sameBindingsRun = adjacentToPreviousRun &&
					runs.back().lastSubmissionIndex != submissionIndex &&
					SameSortedDrawBindings(draws.back(), submission);
				if (sameSourceRun || sameBindingsRun)
				{
					DrawRun &run = runs.back();
					NativeSortedDraw &draw = draws.back();
					if (sameBindingsRun && !sameSourceRun && !run.chunkIndexed)
					{
						// Existing local indices address one source's byte-offset view.
						// Rebase the whole adjacent run before changing its IA offset to 0.
						size_t expandedVertexCount = 0;
						for (size_t previous = 0;
							previous < run.triangleCount; ++previous)
						{
							const size_t previousTriangleIndex = chunkOffset +
								run.firstTriangle + previous;
							const SortedTriangle &previousTriangle = triangles[
								previousTriangleIndex];
							if (previousTriangle.submissionIndex >=
								m_impl->submissions.size() ||
								previousTriangle.submissionIndex < cohortStart ||
								previousTriangle.submissionIndex - cohortStart >=
								vertexOffsets.size())
								return RENDER_RESULT_INVALID_ARGUMENT;
							const size_t previousRelativeIndex =
								previousTriangle.submissionIndex - cohortStart;
							const size_t previousVertexOffset =
								vertexOffsets[previousRelativeIndex];
							if (previousVertexOffset ==
								std::numeric_limits<size_t>::max() ||
								previousVertexOffset % stride != 0)
								return RENDER_RESULT_INVALID_ARGUMENT;
							const size_t previousVertexBase =
								previousVertexOffset / stride;
							const SortedSubmission &previousSubmission =
								m_impl->submissions[
									previousTriangle.submissionIndex];
							const size_t vertexEnd = previousVertexBase +
								previousSubmission.packet.vertexCount;
							if (vertexEnd > MAX_SORTING_VERTEX_CAPACITY)
								return RENDER_RESULT_INVALID_ARGUMENT;
							expandedVertexCount = std::max(expandedVertexCount,
								vertexEnd);
							const size_t rebased[] = {
								previousVertexBase + previousTriangle.i,
								previousVertexBase + previousTriangle.j,
								previousVertexBase + previousTriangle.k
							};
							const size_t indexStart =
								(run.firstTriangle + previous) * 3;
							for (unsigned int corner = 0; corner < 3; ++corner)
							{
								if (rebased[corner] > MAX_SORTING_INDEX_COUNT ||
									indexStart + corner >= chunkIndices.size())
									return RENDER_RESULT_INVALID_ARGUMENT;
								chunkIndices[indexStart + corner] =
									static_cast<unsigned short>(rebased[corner]);
							}
						}
						if (expandedVertexCount >
							std::numeric_limits<unsigned int>::max())
							return RENDER_RESULT_OUT_OF_MEMORY;
						draw.packet.vertexOffset = 0;
						draw.packet.vertexCount = static_cast<unsigned int>(
							expandedVertexCount);
						run.chunkIndexed = true;
					}

					if (run.chunkIndexed)
					{
						const size_t vertexEnd = vertexBase +
							submission.packet.vertexCount;
						if (vertexEnd > MAX_SORTING_VERTEX_CAPACITY ||
							vertexEnd > std::numeric_limits<unsigned int>::max())
							return RENDER_RESULT_INVALID_ARGUMENT;
						chunkIndices.push_back(static_cast<unsigned short>(
							vertexBase + triangle.i));
						chunkIndices.push_back(static_cast<unsigned short>(
							vertexBase + triangle.j));
						chunkIndices.push_back(static_cast<unsigned short>(
							vertexBase + triangle.k));
						draw.packet.vertexCount = std::max(
							draw.packet.vertexCount,
							static_cast<unsigned int>(vertexEnd));
					}
					else
					{
						// Each draw binds this source's byte offset, so indices stay local.
						chunkIndices.push_back(triangle.i);
						chunkIndices.push_back(triangle.j);
						chunkIndices.push_back(triangle.k);
					}
					ExtendDrawRun(draws, runs, submissionIndex);
				}
				else
				{
					chunkIndices.push_back(triangle.i);
					chunkIndices.push_back(triangle.j);
					chunkIndices.push_back(triangle.k);
					StartDrawRun(draws, runs, submission, submissionIndex, local,
						vertexOffset);
				}
			}

			if (local == 0)
				return RENDER_RESULT_INVALID_ARGUMENT;
			const RenderResult result = SubmitChunk(sink, draws, runs, triangles,
				chunkOffset, m_impl->submissions, cohortStart, cohortSize,
				m_impl->acknowledgedCohortStart, m_impl->acknowledgedCohortSize,
				chunkVertices, chunkIndices);
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
	m_impl->submissions.erase(m_impl->submissions.begin() + cohortStart,
		m_impl->submissions.begin() + cohortStart + cohortSize);
	m_impl->acknowledgedCohortStart = m_impl->acknowledgedCohortSize = 0;
	if (!m_impl->submissions.empty())
	{
		// Rare retry-only tail: preserve admission, but never interleave new
		// work with a prefix already emitted from the acknowledged cohort.
		m_impl->workspace.Clear();
		if (firstCohortOnly)
			return RENDER_RESULT_OK;
		goto nextCohort;
	}
	return RENDER_RESULT_OK;
}

void NativeSortingRenderer::Clear()
{
	if (m_impl != 0)
	{
		m_impl->submissions.clear();
		m_impl->acknowledgedCohortStart = m_impl->acknowledgedCohortSize = 0;
	}
}

bool NativeSortingRenderer::Empty() const
{
	return m_impl == 0 || m_impl->submissions.empty();
}

}
}

#if defined(RTS_NATIVE_SORTING_TESTS)
void NativeSortingRendererTestUseSingleDescriptorReference(bool enabled)
{
	g_nativeSortingSingleDescriptorReference = enabled;
}

unsigned int NativeSortingRendererTestLastParallelBatches()
{
	return g_nativeSortingLastParallelBatches;
}

unsigned long long NativeSortingRendererTestLastOffsetInitializations()
{
	return g_nativeSortingLastOffsetInitializations;
}

unsigned long long NativeSortingRendererTestLastOffsetResets()
{
	return g_nativeSortingLastOffsetResets;
}

unsigned int NativeSortingRendererTestLastFlushScratchAllocationCount()
{
	return g_nativeSortingLastFlushScratchAllocationCount;
}

unsigned int NativeSortingRendererTestLastFlushPreparedGrowthCount()
{
	return g_nativeSortingLastFlushPreparedGrowthCount;
}

unsigned long long NativeSortingRendererTestLastFlushWorkspaceCapacityBytes()
{
	return g_nativeSortingLastFlushWorkspaceCapacityBytes;
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
