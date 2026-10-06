/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2026 TheSuperHackers
** SPDX-License-Identifier: GPL-3.0-or-later
*/

#include "Utility/CppMacros.h"
#include "nativew3dsorting.h"
#include "Lib/JobSystem.h"

#include <stdio.h>
#include <stddef.h>
#include <string.h>
#include <new>
#include <type_traits>
#include <vector>

bool NativeSortingRendererTestRetireAllComplete();
bool NativeSortingRendererTestRetireMixedPending();
unsigned int NativeSortingRendererTestLastFlushScratchAllocationCount();
void NativeSortingRendererTestUseSingleDescriptorReference(bool enabled);
unsigned int NativeSortingRendererTestLastParallelBatches();
unsigned int NativeSortingRendererTestLastFlushPreparedGrowthCount();
unsigned long long NativeSortingRendererTestLastFlushWorkspaceCapacityBytes();
unsigned long long NativeSortingRendererTestLastOffsetInitializations();
unsigned long long NativeSortingRendererTestLastOffsetResets();

namespace
{

using namespace rts::render;

unsigned int failures = 0;

#define CHECK(condition) do { if (!(condition)) { ++failures; \
	fprintf(stderr, "line %u: %s\n", static_cast<unsigned>(__LINE__), \
		#condition); } } while (0)

struct TestVertex
{
	float x;
	float y;
	float z;
	unsigned int color;
};

struct TestVertexWide
{
	float x;
	float y;
	float z;
	unsigned int color;
	float u;
	float v;
};

bool SameBatchGeometry(const NativeDrawPacket &left,
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

struct CapturedBatch
{
	CapturedBatch() : acceptedDrawCount(0) {}

	std::vector<unsigned int> states;
	std::vector<unsigned int> indexCounts;
	std::vector<unsigned int> startIndices;
	std::vector<unsigned int> vertexOffsets;
	std::vector<unsigned int> vertexCounts;
	std::vector<unsigned int> vertexStrides;
	std::vector<unsigned short> indices;
	std::vector<unsigned char> vertices;
	unsigned int acceptedDrawCount;
};

struct CapturedDraw
{
	unsigned int state;
	std::vector<unsigned short> indices;
	std::vector<unsigned char> referencedVertices;
};

class RecordingSink : public NativeSortedGeometrySink
{
public:
	RecordingSink() : calls(0), failCall(0), acceptedOnFailure(0),
		requireHomogeneous(false), batches() {}

	virtual RenderResult SubmitNativeSortedBatch(
		const NativeSortedDraw *draws, unsigned int drawCount,
		const void *vertexData, size_t vertexBytes,
		const void *indexData, size_t indexBytes,
		unsigned int *submittedDrawCount)
	{
		++calls;
		if (submittedDrawCount == 0 || draws == 0 || drawCount == 0 ||
			vertexData == 0 || vertexBytes == 0 || indexData == 0 ||
			indexBytes % sizeof(unsigned short) != 0)
			return RENDER_RESULT_INVALID_ARGUMENT;
		CapturedBatch batch;
		for (unsigned int index = 0; index < drawCount; ++index)
		{
			batch.states.push_back(draws[index].state.pipeline.shaderBits);
			batch.indexCounts.push_back(draws[index].packet.indexCount);
			batch.startIndices.push_back(draws[index].packet.startIndex);
			batch.vertexOffsets.push_back(draws[index].packet.vertexOffset);
			batch.vertexCounts.push_back(draws[index].packet.vertexCount);
			batch.vertexStrides.push_back(draws[index].packet.vertexStride);
		}
		const unsigned char *sourceVertices =
			static_cast<const unsigned char *>(vertexData);
		batch.vertices.assign(sourceVertices, sourceVertices + vertexBytes);
		const unsigned short *sourceIndices =
			static_cast<const unsigned short *>(indexData);
		batch.indices.assign(sourceIndices,
			sourceIndices + indexBytes / sizeof(unsigned short));
		if (requireHomogeneous)
		{
			for (unsigned int index = 1; index < drawCount; ++index)
			{
				if (!SameBatchGeometry(draws[0].packet, draws[index].packet))
				{
					batches.push_back(batch);
					*submittedDrawCount = 0;
					return RENDER_RESULT_INVALID_ARGUMENT;
				}
			}
		}
		const bool fail = failCall != 0 && calls == failCall;
		batch.acceptedDrawCount = fail && acceptedOnFailure < drawCount ?
			acceptedOnFailure : drawCount;
		batches.push_back(batch);
		*submittedDrawCount = batch.acceptedDrawCount;
		return fail ? RENDER_RESULT_FAILED : RENDER_RESULT_OK;
	}

	unsigned int calls;
	unsigned int failCall;
	unsigned int acceptedOnFailure;
	bool requireHomogeneous;
	std::vector<CapturedBatch> batches;
};

class PassRecordingSink : public RecordingSink
{
public:
	std::vector<NativeW3DSubmissionSequence> passIdentities;
	std::vector<GpuHandle> targets;
	std::vector<RenderViewport> viewports;
	RenderResult SubmitNativeSortedPassBatch(const NativeSortedPass &pass,
		const NativeSortedDraw *draws, unsigned int drawCount,
		const void *vertices, size_t vertexBytes, const void *indices,
		size_t indexBytes, unsigned int *accepted) override
	{
		passIdentities.push_back(pass.identity);
		targets.push_back(pass.target.color.resource);
		viewports.push_back(pass.viewport);
		return SubmitNativeSortedBatch(draws, drawCount, vertices, vertexBytes,
			indices, indexBytes, accepted);
	}
};

bool CaptureAcceptedDrawStream(const RecordingSink &sink,
	std::vector<CapturedDraw> &draws)
{
	draws.clear();
	for (size_t batchIndex = 0; batchIndex < sink.batches.size(); ++batchIndex)
	{
		const CapturedBatch &batch = sink.batches[batchIndex];
		if (batch.acceptedDrawCount > batch.states.size() ||
			batch.states.size() != batch.indexCounts.size() ||
			batch.states.size() != batch.startIndices.size() ||
			batch.states.size() != batch.vertexOffsets.size() ||
			batch.states.size() != batch.vertexCounts.size() ||
			batch.states.size() != batch.vertexStrides.size())
			return false;

		for (unsigned int drawIndex = 0;
			drawIndex < batch.acceptedDrawCount; ++drawIndex)
		{
			const unsigned int startIndex = batch.startIndices[drawIndex];
			const unsigned int indexCount = batch.indexCounts[drawIndex];
			const unsigned int vertexOffset = batch.vertexOffsets[drawIndex];
			const unsigned int vertexCount = batch.vertexCounts[drawIndex];
			const unsigned int stride = batch.vertexStrides[drawIndex];
			if (stride == 0 || vertexOffset > batch.vertices.size() ||
				vertexCount > (batch.vertices.size() - vertexOffset) / stride ||
				startIndex > batch.indices.size() ||
				indexCount > batch.indices.size() - startIndex)
				return false;

			CapturedDraw captured;
			captured.state = batch.states[drawIndex];
			for (unsigned int index = 0; index < indexCount; ++index)
			{
				const unsigned short vertexIndex =
					batch.indices[startIndex + index];
				if (vertexIndex >= vertexCount)
					return false;
				const size_t vertexByteOffset = static_cast<size_t>(vertexOffset) +
					static_cast<size_t>(vertexIndex) * stride;
				if (vertexByteOffset > batch.vertices.size() ||
					stride > batch.vertices.size() - vertexByteOffset)
					return false;
				captured.indices.push_back(vertexIndex);
				captured.referencedVertices.insert(
					captured.referencedVertices.end(),
					batch.vertices.begin() + vertexByteOffset,
					batch.vertices.begin() + vertexByteOffset + stride);
			}
			draws.push_back(captured);
		}
	}
	return true;
}

void TestCapturedDrawRejectsIndexOutsideDeclaredVertexRange()
{
	RecordingSink sink;
	CapturedBatch batch;
	batch.states.push_back(1);
	batch.indexCounts.push_back(3);
	batch.startIndices.push_back(0);
	batch.vertexOffsets.push_back(0);
	batch.vertexCounts.push_back(2);
	batch.vertexStrides.push_back(sizeof(TestVertex));
	batch.indices.push_back(0);
	batch.indices.push_back(1);
	batch.indices.push_back(2);
	batch.vertices.resize(3 * sizeof(TestVertex));
	batch.acceptedDrawCount = 1;
	sink.batches.push_back(batch);

	std::vector<CapturedDraw> draws;
	CHECK(!CaptureAcceptedDrawStream(sink, draws));
}

bool SameAcceptedDrawStream(const std::vector<CapturedDraw> &left,
	const std::vector<CapturedDraw> &right)
{
	if (left.size() != right.size())
		return false;
	for (size_t index = 0; index < left.size(); ++index)
	{
		if (left[index].state != right[index].state ||
			left[index].referencedVertices != right[index].referencedVertices)
			return false;
	}
	return true;
}

// Different local index encodings are equivalent when they resolve to the
// same ordered corner bytes in the packed batch.
bool SameAcceptedTriangleVertexStream(const std::vector<CapturedDraw> &left,
	const std::vector<CapturedDraw> &right)
{
	if (left.size() != right.size())
		return false;
	for (size_t index = 0; index < left.size(); ++index)
	{
		if (left[index].referencedVertices != right[index].referencedVertices)
			return false;
	}
	return true;
}

bool SameCapturedBatchBytes(const CapturedBatch &left,
	const CapturedBatch &right)
{
	return left.states == right.states &&
		left.indexCounts == right.indexCounts &&
		left.startIndices == right.startIndices &&
		left.vertexOffsets == right.vertexOffsets &&
		left.vertexCounts == right.vertexCounts &&
		left.vertexStrides == right.vertexStrides &&
		left.indices == right.indices && left.vertices == right.vertices &&
		left.acceptedDrawCount == right.acceptedDrawCount;
}

NativeDrawPacket MakePacket(unsigned int vertexCount, unsigned int indexCount)
{
	NativeDrawPacket packet;
	packet.vertexStride = sizeof(TestVertex);
	packet.vertexOffset = 0;
	packet.indexOffset = 0;
	packet.indexFormat = RENDER_FORMAT_R16_UINT;
	packet.vertexFormat = RENDER_VERTEX_POSITION3_COLOR;
	packet.vertexLayout.stride = sizeof(TestVertex);
	packet.vertexLayout.elementCount = 2;
	packet.vertexLayout.preTransformed = false;
	packet.vertexLayout.elements[0].semantic = RENDER_VERTEX_SEMANTIC_POSITION;
	packet.vertexLayout.elements[0].semanticIndex = 0;
	packet.vertexLayout.elements[0].format = RENDER_VERTEX_DATA_FLOAT3;
	packet.vertexLayout.elements[0].byteOffset = 0;
	packet.vertexLayout.elements[1].semantic = RENDER_VERTEX_SEMANTIC_DIFFUSE;
	packet.vertexLayout.elements[1].semanticIndex = 0;
	packet.vertexLayout.elements[1].format = RENDER_VERTEX_DATA_COLOR_BGRA8;
	packet.vertexLayout.elements[1].byteOffset = sizeof(float) * 3;
	packet.topology = RENDER_PRIMITIVE_TRIANGLE_LIST;
	packet.texturePresenceMask = 0;
	packet.vertexCount = vertexCount;
	packet.startVertex = 0;
	packet.indexCount = indexCount;
	packet.startIndex = 0;
	packet.minimumVertexIndex = 0;
	packet.baseVertex = 0;
	packet.indexed = true;
	return packet;
}

void MakeVertices(std::vector<TestVertex> &vertices,
	const std::vector<float> &depths)
{
	vertices.resize(depths.size());
	for (size_t index = 0; index < vertices.size(); ++index)
	{
		vertices[index].x = 0.0f;
		vertices[index].y = 0.0f;
		vertices[index].z = depths[index];
		vertices[index].color = 0xffffffffU;
	}
}

void QueueOne(NativeSortingRenderer &renderer, unsigned int shaderBits,
	const std::vector<TestVertex> &vertices,
	const std::vector<unsigned short> &indices,
	const GameBoundingSphere *sphere)
{
	LegacyLogicalState state;
	state.pipeline.shaderBits = shaderBits;
	NativeDrawPacket packet = MakePacket(static_cast<unsigned int>(
		vertices.size()), static_cast<unsigned int>(indices.size()));
	CHECK(renderer.Queue(state, packet, vertices.data(),
		vertices.size() * sizeof(TestVertex), indices.data(),
		indices.size() * sizeof(unsigned short), sphere) == RENDER_RESULT_OK);
}

void QueueMixedSizeScratchReuseFixture(NativeSortingRenderer &renderer)
{
	const float depths[][5] = {
		{7.0f, 1.0f, 11.0f, 4.0f, 2.0f},
		{3.0f, 8.0f, 0.0f, 0.0f, 0.0f},
		{-1.0f, 0.0f, 0.0f, 0.0f, 0.0f}
	};
	const unsigned int triangleCounts[] = {5, 2, 1};
	const unsigned int shaderBits[] = {101, 202, 303};
	const unsigned int colorBases[] = {
		0x10000000U, 0x20000000U, 0x30000000U
	};
	for (unsigned int node = 0; node < 3; ++node)
	{
		std::vector<TestVertex> vertices(triangleCounts[node] * 3);
		std::vector<unsigned short> indices(triangleCounts[node] * 3);
		for (unsigned int triangle = 0;
			triangle < triangleCounts[node]; ++triangle)
		{
			for (unsigned int corner = 0; corner < 3; ++corner)
			{
				TestVertex &vertex = vertices[triangle * 3 + corner];
				vertex.x = static_cast<float>(corner);
				vertex.y = static_cast<float>(node);
				vertex.z = depths[node][triangle];
				vertex.color = colorBases[node] + triangle;
				indices[triangle * 3 + corner] = static_cast<unsigned short>(
					triangle * 3 + corner);
			}
		}
		QueueOne(renderer, shaderBits[node], vertices, indices, 0);
	}
}

void CheckMixedSizeScratchReuseStream(
	const std::vector<CapturedDraw> &draws)
{
	const unsigned int expectedStates[] = {303, 101, 202, 101, 202, 101};
	const unsigned int expectedTrianglesPerDraw[] = {1, 2, 1, 2, 1, 1};
	const unsigned int expectedColors[] = {
		0x30000000U, 0x10000001U, 0x10000004U, 0x20000000U,
		0x10000003U, 0x10000000U, 0x20000001U, 0x10000002U
	};
	const float expectedDepths[] = {-1.0f, 1.0f, 2.0f, 3.0f,
		4.0f, 7.0f, 8.0f, 11.0f};
	const size_t expectedDrawCount =
		sizeof(expectedStates) / sizeof(expectedStates[0]);
	CHECK(draws.size() == expectedDrawCount);
	if (draws.size() != expectedDrawCount)
		return;
	size_t triangleOffset = 0;
	for (size_t index = 0; index < draws.size(); ++index)
	{
		CHECK(draws[index].state == expectedStates[index]);
		const size_t triangleCount = expectedTrianglesPerDraw[index];
		CHECK(draws[index].indices.size() == triangleCount * 3);
		CHECK(draws[index].referencedVertices.size() ==
			triangleCount * 3 * sizeof(TestVertex));
		if (draws[index].referencedVertices.size() ==
			triangleCount * 3 * sizeof(TestVertex))
		{
			for (size_t triangle = 0; triangle < triangleCount; ++triangle)
			{
				TestVertex firstVertex;
				const size_t vertexOffset = triangle * 3 * sizeof(TestVertex);
				memcpy(&firstVertex,
					draws[index].referencedVertices.data() + vertexOffset,
					sizeof(firstVertex));
				CHECK(firstVertex.color == expectedColors[triangleOffset]);
				CHECK(firstVertex.z == expectedDepths[triangleOffset]);
				++triangleOffset;
			}
		}
	}
	CHECK(triangleOffset == sizeof(expectedColors) / sizeof(expectedColors[0]));
}

void QueueStableNodeOrderFixture(NativeSortingRenderer &renderer)
{
	std::vector<TestVertex> vertices;
	// Equal triangle depths keep the later small-range triangle sort stable, so
	// the recorded sequence exposes the node-ordering result directly.
	MakeVertices(vertices, std::vector<float>(3, 0.0f));
	const unsigned short sourceIndices[] = {0, 1, 2};
	const std::vector<unsigned short> indices(sourceIndices,
		sourceIndices + 3);
	GameBoundingSphere depthOne(0.0f, 0.0f, 1.0f, 1.0f);
	GameBoundingSphere depthThreeFirst(0.0f, 0.0f, 3.0f, 1.0f);
	GameBoundingSphere depthThreeSecond(0.0f, 0.0f, 3.0f, 1.0f);
	GameBoundingSphere positiveZero(0.0f, 0.0f, 0.0f, 1.0f);
	GameBoundingSphere negativeZero(0.0f, 0.0f, -0.0f, 1.0f);
	GameBoundingSphere negativeDepth(0.0f, 0.0f, -2.0f, 1.0f);

	QueueOne(renderer, 101, vertices, indices, &depthOne);
	QueueOne(renderer, 202, vertices, indices, &depthThreeFirst);
	QueueOne(renderer, 303, vertices, indices, &depthThreeSecond);
	QueueOne(renderer, 404, vertices, indices, 0);
	QueueOne(renderer, 505, vertices, indices, &positiveZero);
	QueueOne(renderer, 606, vertices, indices, &negativeZero);
	QueueOne(renderer, 707, vertices, indices, &negativeDepth);
}

void CheckStableNodeOrder(const std::vector<CapturedDraw> &draws)
{
	const unsigned int expected[] = {202, 303, 101, 404, 505, 606, 707};
	CHECK(draws.size() == sizeof(expected) / sizeof(expected[0]));
	if (draws.size() == sizeof(expected) / sizeof(expected[0]))
	{
		for (size_t index = 0; index < draws.size(); ++index)
			CHECK(draws[index].state == expected[index]);
	}
}

void TestNodeOrderingAndFlushBoundary()
{
	NativeSortingRenderer renderer;
	RecordingSink sink;
	std::vector<TestVertex> vertices;
	std::vector<unsigned short> indices(3);
	indices[0] = 0;
	indices[1] = 1;
	indices[2] = 2;
	MakeVertices(vertices, std::vector<float>(3, 0.0f));

	GameBoundingSphere front(0.0f, 0.0f, 2.0f, 1.0f);
	GameBoundingSphere behind(0.0f, 0.0f, -1.0f, 1.0f);
	QueueOne(renderer, 10, vertices, indices, &front);
	QueueOne(renderer, 20, vertices, indices, 0);
	QueueOne(renderer, 30, vertices, indices, &behind);
	CHECK(sink.calls == 0);
	CHECK(renderer.Flush(sink) == RENDER_RESULT_OK);
	CHECK(sink.calls == 1);
	CHECK(sink.batches.size() == 1);
	if (sink.batches.size() == 1)
	{
		const CapturedBatch &batch = sink.batches[0];
		CHECK(batch.states.size() == 3);
		if (batch.states.size() == 3)
		{
			CHECK(batch.states[0] == 10);
			CHECK(batch.states[1] == 20);
			CHECK(batch.states[2] == 30);
			CHECK(batch.vertexOffsets.size() == 3);
			CHECK(batch.startIndices.size() == 3);
			CHECK(batch.indices.size() == 9);
			if (batch.vertexOffsets.size() == 3 &&
				batch.startIndices.size() == 3 && batch.indices.size() == 9)
			{
				for (unsigned int draw = 0; draw < 3; ++draw)
				{
					CHECK(batch.vertexOffsets[draw] == draw * 3 * sizeof(TestVertex));
					CHECK(batch.startIndices[draw] == draw * 3);
					for (unsigned int vertex = 0; vertex < 3; ++vertex)
						CHECK(batch.indices[batch.startIndices[draw] + vertex] == vertex);
				}
			}
		}
	}
	CHECK(renderer.Empty());
	CHECK(renderer.Flush(sink) == RENDER_RESULT_OK);
	CHECK(sink.calls == 1);

	std::vector<TestVertex> nextVertices = vertices;
	for (size_t index = 0; index < nextVertices.size(); ++index)
		nextVertices[index].color = 0x12345678U +
			static_cast<unsigned int>(index);
	QueueOne(renderer, 40, nextVertices, indices, 0);
	CHECK(!renderer.Empty());
	CHECK(renderer.Flush(sink) == RENDER_RESULT_OK);
	CHECK(renderer.Empty());
	CHECK(sink.calls == 2);
	CHECK(sink.batches.size() == 2);
	if (sink.batches.size() == 2)
	{
		CHECK(sink.batches[1].states.size() == 1 &&
			sink.batches[1].states[0] == 40);
		CHECK(sink.batches[1].indices.size() == indices.size());
		if (sink.batches[1].indices.size() == indices.size())
			CHECK(memcmp(sink.batches[1].indices.data(), indices.data(),
				indices.size() * sizeof(unsigned short)) == 0);
		CHECK(sink.batches[1].vertices.size() ==
			nextVertices.size() * sizeof(TestVertex));
		if (sink.batches[1].vertices.size() ==
			nextVertices.size() * sizeof(TestVertex))
			CHECK(memcmp(sink.batches[1].vertices.data(), nextVertices.data(),
				nextVertices.size() * sizeof(TestVertex)) == 0);
	}
}

void TestStableNodeOrderingPreservesEqualDepthAndSplice()
{
	NativeSortingRenderer renderer;
	RecordingSink sink;
	QueueStableNodeOrderFixture(renderer);
	CHECK(renderer.Flush(sink) == RENDER_RESULT_OK);
	CHECK(sink.calls == 1);
	std::vector<CapturedDraw> draws;
	CHECK(CaptureAcceptedDrawStream(sink, draws));
	CheckStableNodeOrder(draws);
	CHECK(renderer.Empty());
}

void TestPerTriangleDepthOrder()
{
	NativeSortingRenderer renderer;
	RecordingSink sink;
	std::vector<TestVertex> vertices;
	MakeVertices(vertices, std::vector<float>{3.0f, 3.0f, 3.0f,
		1.0f, 1.0f, 1.0f, 2.0f, 2.0f, 2.0f});
	std::vector<unsigned short> indices;
	indices.push_back(0);
	indices.push_back(1);
	indices.push_back(2);
	indices.push_back(3);
	indices.push_back(4);
	indices.push_back(5);
	indices.push_back(6);
	indices.push_back(7);
	indices.push_back(8);
	QueueOne(renderer, 77, vertices, indices, 0);
	CHECK(renderer.Flush(sink) == RENDER_RESULT_OK);
	CHECK(sink.batches.size() == 1);
	if (sink.batches.size() == 1)
	{
		const CapturedBatch &batch = sink.batches[0];
		CHECK(batch.states.size() == 1 && batch.states[0] == 77);
		CHECK(batch.indices.size() == 9);
		if (batch.indices.size() == 9)
		{
			const unsigned short expected[] = {3, 4, 5, 6, 7, 8, 0, 1, 2};
			CHECK(memcmp(batch.indices.data(), expected,
				sizeof(expected)) == 0);
		}
	}
}

void TestMixedGeometryPreservesSortedOrder()
{
	NativeSortingRenderer renderer;
	RecordingSink sink;
	sink.requireHomogeneous = true;
	const unsigned short indices[] = {0, 1, 2};
	TestVertex narrow[3] = {};
	TestVertexWide wide[3] = {};
	for (unsigned int index = 0; index < 3; ++index)
	{
		narrow[index].z = 1.0f;
		wide[index].z = 2.0f;
	}
	LegacyLogicalState state;
	NativeDrawPacket narrowPacket = MakePacket(3, 3);
	state.pipeline.shaderBits = 10;
	CHECK(renderer.Queue(state, narrowPacket, narrow, sizeof(narrow),
		indices, sizeof(indices), 0) == RENDER_RESULT_OK);

	NativeDrawPacket widePacket = MakePacket(3, 3);
	widePacket.vertexStride = sizeof(TestVertexWide);
	widePacket.vertexLayout.stride = sizeof(TestVertexWide);
	widePacket.vertexLayout.elementCount = 3;
	widePacket.vertexLayout.elements[2].semantic =
		RENDER_VERTEX_SEMANTIC_TEXTURE_COORDINATE;
	widePacket.vertexLayout.elements[2].semanticIndex = 0;
	widePacket.vertexLayout.elements[2].format = RENDER_VERTEX_DATA_FLOAT2;
	widePacket.vertexLayout.elements[2].byteOffset = sizeof(TestVertex);
	state.pipeline.shaderBits = 20;
	CHECK(renderer.Queue(state, widePacket, wide, sizeof(wide),
		indices, sizeof(indices), 0) == RENDER_RESULT_OK);

	for (unsigned int index = 0; index < 3; ++index)
		narrow[index].z = 3.0f;
	NativeDrawPacket alternateLayout = narrowPacket;
	alternateLayout.vertexLayout.elementCount = 1;
	state.pipeline.shaderBits = 30;
	CHECK(renderer.Queue(state, alternateLayout, narrow, sizeof(narrow),
		indices, sizeof(indices), 0) == RENDER_RESULT_OK);

	for (unsigned int index = 0; index < 3; ++index)
		narrow[index].z = 4.0f;
	NativeDrawPacket alternateFormat = narrowPacket;
	alternateFormat.vertexFormat = RENDER_VERTEX_POSITION3_NORMAL_COLOR_TEX1;
	state.pipeline.shaderBits = 40;
	CHECK(renderer.Queue(state, alternateFormat, narrow, sizeof(narrow),
		indices, sizeof(indices), 0) == RENDER_RESULT_OK);

	CHECK(renderer.Flush(sink) == RENDER_RESULT_OK);
	CHECK(sink.calls == 4);
	CHECK(sink.batches.size() == 4);
	if (sink.batches.size() == 4)
	{
		const unsigned int expected[] = {10, 20, 30, 40};
		for (unsigned int index = 0; index < 4; ++index)
			CHECK(sink.batches[index].states.size() == 1 &&
				sink.batches[index].states[0] == expected[index]);
	}
	CHECK(renderer.Empty());
}

void TestSameSubmissionTrianglesBeforeIncompatibleSubmission()
{
	NativeSortingRenderer renderer;
	RecordingSink sink;
	sink.requireHomogeneous = true;
	std::vector<TestVertex> narrow;
	MakeVertices(narrow, std::vector<float>{1.0f, 1.0f, 1.0f,
		2.0f, 2.0f, 2.0f, 3.0f, 3.0f, 3.0f});
	TestVertexWide wide[3] = {};
	for (unsigned int index = 0; index < 3; ++index)
		wide[index].z = 4.0f;

	unsigned short narrowIndices[9];
	for (unsigned short index = 0; index < 9; ++index)
		narrowIndices[index] = index;
	const unsigned short wideIndices[] = {0, 1, 2};
	LegacyLogicalState state;
	state.pipeline.shaderBits = 101;
	NativeDrawPacket narrowPacket = MakePacket(9, 9);
	CHECK(renderer.Queue(state, narrowPacket, narrow.data(),
		narrow.size() * sizeof(TestVertex), narrowIndices,
		sizeof(narrowIndices), 0) == RENDER_RESULT_OK);

	state.pipeline.shaderBits = 202;
	NativeDrawPacket widePacket = MakePacket(3, 3);
	widePacket.vertexStride = sizeof(TestVertexWide);
	widePacket.vertexLayout.stride = sizeof(TestVertexWide);
	widePacket.vertexLayout.elementCount = 3;
	widePacket.vertexLayout.elements[2].semantic =
		RENDER_VERTEX_SEMANTIC_TEXTURE_COORDINATE;
	widePacket.vertexLayout.elements[2].semanticIndex = 0;
	widePacket.vertexLayout.elements[2].format = RENDER_VERTEX_DATA_FLOAT2;
	widePacket.vertexLayout.elements[2].byteOffset = sizeof(TestVertex);
	CHECK(renderer.Queue(state, widePacket, wide,
		sizeof(wide), wideIndices,
		sizeof(wideIndices), 0) == RENDER_RESULT_OK);

	CHECK(renderer.Flush(sink) == RENDER_RESULT_OK);
	CHECK(sink.calls == 2);
	CHECK(sink.batches.size() == 2);
	if (sink.batches.size() == 2)
	{
		CHECK(sink.batches[0].states.size() == 1);
		CHECK(sink.batches[0].states[0] == 101);
		CHECK(sink.batches[0].indexCounts.size() == 1);
		CHECK(sink.batches[0].indexCounts[0] == 9);
		CHECK(sink.batches[1].states.size() == 1);
		CHECK(sink.batches[1].states[0] == 202);
		CHECK(sink.batches[1].indexCounts.size() == 1);
		CHECK(sink.batches[1].indexCounts[0] == 3);
	}

	std::vector<CapturedDraw> captured;
	CHECK(CaptureAcceptedDrawStream(sink, captured));
	CHECK(captured.size() == 2);
	if (captured.size() == 2)
	{
		const unsigned short expectedNarrowIndices[] =
			{0, 1, 2, 3, 4, 5, 6, 7, 8};
		const unsigned short expectedWideIndices[] = {0, 1, 2};
		CHECK(captured[0].state == 101);
		CHECK(captured[0].indices.size() == 9);
		if (captured[0].indices.size() == 9)
			CHECK(memcmp(captured[0].indices.data(), expectedNarrowIndices,
				sizeof(expectedNarrowIndices)) == 0);
		CHECK(captured[0].referencedVertices.size() ==
			narrow.size() * sizeof(TestVertex));
		if (captured[0].referencedVertices.size() ==
			narrow.size() * sizeof(TestVertex))
			CHECK(memcmp(captured[0].referencedVertices.data(), narrow.data(),
				narrow.size() * sizeof(TestVertex)) == 0);

		CHECK(captured[1].state == 202);
		CHECK(captured[1].indices.size() == 3);
		if (captured[1].indices.size() == 3)
			CHECK(memcmp(captured[1].indices.data(), expectedWideIndices,
				sizeof(expectedWideIndices)) == 0);
		CHECK(captured[1].referencedVertices.size() ==
			3 * sizeof(TestVertexWide));
		if (captured[1].referencedVertices.size() ==
			3 * sizeof(TestVertexWide))
			CHECK(memcmp(captured[1].referencedVertices.data(), wide,
				3 * sizeof(TestVertexWide)) == 0);
	}
	CHECK(renderer.Empty());
}

void TestFailureAfterFirstChunkRetainsOnlyPendingGeometry()
{
	NativeSortingRenderer renderer;
	RecordingSink sink;
	std::vector<TestVertex> vertices;
	MakeVertices(vertices, std::vector<float>{0.0f, 0.0f, 0.0f});
	const unsigned int triangleCount = 21846;
	std::vector<unsigned short> indices(triangleCount * 3, 0);
	for (unsigned int triangle = 0; triangle < triangleCount; ++triangle)
	{
		indices[triangle * 3] = 0;
		indices[triangle * 3 + 1] = 1;
		indices[triangle * 3 + 2] = 2;
	}
	QueueOne(renderer, 91, vertices, indices, 0);
	sink.failCall = 2;
	sink.acceptedOnFailure = 0;
	CHECK(renderer.Flush(sink) == RENDER_RESULT_FAILED);
	CHECK(sink.calls == 2);
	CHECK(!renderer.Empty());
	CHECK(sink.batches.size() == 2);
	if (sink.batches.size() == 2)
	{
		CHECK(sink.batches[0].indices.size() == 65535);
		CHECK(sink.batches[1].indices.size() == 3);
	}

	sink.failCall = 0;
	CHECK(renderer.Flush(sink) == RENDER_RESULT_OK);
	CHECK(sink.calls == 3);
	CHECK(sink.batches.size() == 3);
	if (sink.batches.size() == 3)
		CHECK(sink.batches[2].indices.size() == 3);
	CHECK(renderer.Empty());
}

void TestPartialDrawFailureRetryMatchesOneShotOutput()
{
	const unsigned short indices[] = {0, 1, 2};
	TestVertex firstVertices[3] = {};
	TestVertex secondVertices[3] = {};
	for (unsigned int index = 0; index < 3; ++index)
	{
		firstVertices[index].x = static_cast<float>(index + 1);
		firstVertices[index].y = static_cast<float>(index + 4);
		firstVertices[index].z = 3.0f;
		firstVertices[index].color = 0x10203040U + index;
		secondVertices[index].x = static_cast<float>(index + 11);
		secondVertices[index].y = static_cast<float>(index + 14);
		secondVertices[index].z = 1.0f;
		secondVertices[index].color = 0x50607080U + index;
	}

	NativeSortingRenderer baselineRenderer;
	RecordingSink baselineSink;
	LegacyLogicalState firstState;
	firstState.pipeline.shaderBits = 101;
	NativeDrawPacket packet = MakePacket(3, 3);
	CHECK(baselineRenderer.Queue(firstState, packet, firstVertices,
		sizeof(firstVertices), indices, sizeof(indices), 0) == RENDER_RESULT_OK);
	LegacyLogicalState secondState;
	secondState.pipeline.shaderBits = 202;
	CHECK(baselineRenderer.Queue(secondState, packet, secondVertices,
		sizeof(secondVertices), indices, sizeof(indices), 0) == RENDER_RESULT_OK);
	CHECK(baselineRenderer.Flush(baselineSink) == RENDER_RESULT_OK);
	CHECK(baselineRenderer.Empty());
	CHECK(baselineSink.calls == 1);
	CHECK(baselineSink.batches.size() == 1);
	CHECK(baselineSink.batches.size() == 1 &&
		baselineSink.batches[0].acceptedDrawCount == 2);

	NativeSortingRenderer retryRenderer;
	RecordingSink retrySink;
	retrySink.failCall = 1;
	retrySink.acceptedOnFailure = 1;
	CHECK(retryRenderer.Queue(firstState, packet, firstVertices,
		sizeof(firstVertices), indices, sizeof(indices), 0) == RENDER_RESULT_OK);
	CHECK(retryRenderer.Queue(secondState, packet, secondVertices,
		sizeof(secondVertices), indices, sizeof(indices), 0) == RENDER_RESULT_OK);
	CHECK(retryRenderer.Flush(retrySink) == RENDER_RESULT_FAILED);
	CHECK(!retryRenderer.Empty());
	CHECK(retrySink.calls == 1);
	CHECK(retrySink.batches.size() == 1 &&
		retrySink.batches[0].acceptedDrawCount == 1);

	retrySink.failCall = 0;
	CHECK(retryRenderer.Flush(retrySink) == RENDER_RESULT_OK);
	CHECK(retryRenderer.Empty());
	CHECK(retrySink.calls == 2);
	CHECK(retrySink.batches.size() == 2);
	CHECK(retrySink.batches.size() == 2 &&
		retrySink.batches[1].acceptedDrawCount == 1);

	std::vector<CapturedDraw> baselineDraws;
	std::vector<CapturedDraw> retriedDraws;
	CHECK(CaptureAcceptedDrawStream(baselineSink, baselineDraws));
	CHECK(CaptureAcceptedDrawStream(retrySink, retriedDraws));
	CHECK(baselineDraws.size() == 2);
	CHECK(retriedDraws.size() == 2);
	CHECK(SameAcceptedDrawStream(baselineDraws, retriedDraws));
}

void TestFlushLocalScratchReuseMixedSizesAndRetry()
{
	NativeSortingRenderer baselineRenderer;
	RecordingSink baselineSink;
	QueueMixedSizeScratchReuseFixture(baselineRenderer);
	CHECK(baselineRenderer.Flush(baselineSink) == RENDER_RESULT_OK);
	CHECK(baselineRenderer.Empty());
	CHECK(NativeSortingRendererTestLastFlushScratchAllocationCount() == 1);
	CHECK(NativeSortingRendererTestLastFlushPreparedGrowthCount() == 1);
	std::vector<CapturedDraw> baselineDraws;
	CHECK(CaptureAcceptedDrawStream(baselineSink, baselineDraws));
	CheckMixedSizeScratchReuseStream(baselineDraws);

	NativeSortingRenderer retryRenderer;
	RecordingSink retrySink;
	retrySink.failCall = 1;
	retrySink.acceptedOnFailure = 3;
	QueueMixedSizeScratchReuseFixture(retryRenderer);
	CHECK(retryRenderer.Flush(retrySink) == RENDER_RESULT_FAILED);
	CHECK(!retryRenderer.Empty());
	const unsigned long long retainedAfterFailure =
		NativeSortingRendererTestLastFlushWorkspaceCapacityBytes();
	CHECK(retainedAfterFailure <= 24ULL * 1024ULL * 1024ULL);
	CHECK(NativeSortingRendererTestLastFlushScratchAllocationCount() == 1);
	CHECK(NativeSortingRendererTestLastFlushPreparedGrowthCount() == 1);

	retrySink.failCall = 0;
	CHECK(retryRenderer.Flush(retrySink) == RENDER_RESULT_OK);
	CHECK(retryRenderer.Empty());
	CHECK(NativeSortingRendererTestLastFlushWorkspaceCapacityBytes() ==
		retainedAfterFailure);
	CHECK(NativeSortingRendererTestLastFlushScratchAllocationCount() == 1);
	CHECK(NativeSortingRendererTestLastFlushPreparedGrowthCount() == 0);
	std::vector<CapturedDraw> retriedDraws;
	CHECK(CaptureAcceptedDrawStream(retrySink, retriedDraws));
	CheckMixedSizeScratchReuseStream(retriedDraws);
	CHECK(SameAcceptedDrawStream(baselineDraws, retriedDraws));
}

void TestFlushWorkspaceReusePreservesRepeatedBatchBytes()
{
	NativeSortingRenderer baselineRenderer;
	RecordingSink baselineSink;
	QueueMixedSizeScratchReuseFixture(baselineRenderer);
	CHECK(baselineRenderer.Flush(baselineSink) == RENDER_RESULT_OK);
	CHECK(baselineSink.batches.size() == 1);

	NativeSortingRenderer reuseRenderer;
	RecordingSink reuseSink;
	QueueMixedSizeScratchReuseFixture(reuseRenderer);
	CHECK(reuseRenderer.Flush(reuseSink) == RENDER_RESULT_OK);
	const unsigned long long warmCapacityBytes =
		NativeSortingRendererTestLastFlushWorkspaceCapacityBytes();
	CHECK(warmCapacityBytes > 0);
	CHECK(warmCapacityBytes <= 24ULL * 1024ULL * 1024ULL);
	QueueMixedSizeScratchReuseFixture(reuseRenderer);
	CHECK(reuseRenderer.Flush(reuseSink) == RENDER_RESULT_OK);
	CHECK(NativeSortingRendererTestLastFlushWorkspaceCapacityBytes() ==
		warmCapacityBytes);
	CHECK(reuseSink.batches.size() == 2);
	if (baselineSink.batches.size() == 1 && reuseSink.batches.size() == 2)
	{
		CHECK(SameCapturedBatchBytes(baselineSink.batches[0],
			reuseSink.batches[0]));
		CHECK(SameCapturedBatchBytes(baselineSink.batches[0],
			reuseSink.batches[1]));
	}
	CHECK(reuseRenderer.Empty());
}

void TestStableNodeOrderingPartialAckRetryMatchesOneShotOutput()
{
	NativeSortingRenderer baselineRenderer;
	RecordingSink baselineSink;
	QueueStableNodeOrderFixture(baselineRenderer);
	CHECK(baselineRenderer.Flush(baselineSink) == RENDER_RESULT_OK);
	CHECK(baselineRenderer.Empty());
	CHECK(baselineSink.calls == 1);
	std::vector<CapturedDraw> baselineDraws;
	CHECK(CaptureAcceptedDrawStream(baselineSink, baselineDraws));
	CheckStableNodeOrder(baselineDraws);

	NativeSortingRenderer retryRenderer;
	RecordingSink retrySink;
	retrySink.failCall = 1;
	retrySink.acceptedOnFailure = 3;
	QueueStableNodeOrderFixture(retryRenderer);
	CHECK(retryRenderer.Flush(retrySink) == RENDER_RESULT_FAILED);
	CHECK(!retryRenderer.Empty());
	CHECK(retrySink.calls == 1);
	CHECK(retrySink.batches.size() == 1);
	if (retrySink.batches.size() == 1)
		CHECK(retrySink.batches[0].acceptedDrawCount == 3);

	retrySink.failCall = 0;
	CHECK(retryRenderer.Flush(retrySink) == RENDER_RESULT_OK);
	CHECK(retryRenderer.Empty());
	CHECK(retrySink.calls == 2);
	std::vector<CapturedDraw> retriedDraws;
	CHECK(CaptureAcceptedDrawStream(retrySink, retriedDraws));
	CheckStableNodeOrder(retriedDraws);
	CHECK(SameAcceptedDrawStream(baselineDraws, retriedDraws));
}

// Compare actual accepted triangles, independent of legal draw regrouping at
// a retry/chunk boundary. No reference sort or acknowledgement algorithm.
bool CaptureAcceptedTriangleStream(const RecordingSink &sink,
	std::vector<CapturedDraw> &triangles)
{
	std::vector<CapturedDraw> draws;
	if (!CaptureAcceptedDrawStream(sink, draws))
		return false;
	triangles.clear();
	for (size_t drawIndex = 0; drawIndex < draws.size(); ++drawIndex)
	{
		const CapturedDraw &draw = draws[drawIndex];
		if (draw.indices.empty() || draw.indices.size() % 3 != 0 ||
			draw.referencedVertices.size() % draw.indices.size() != 0)
			return false;
		const size_t stride = draw.referencedVertices.size() / draw.indices.size();
		for (size_t index = 0; index < draw.indices.size(); index += 3)
		{
			CapturedDraw triangle;
			triangle.state = draw.state;
			triangle.indices.assign(draw.indices.begin() + index,
				draw.indices.begin() + index + 3);
			triangle.referencedVertices.assign(
				draw.referencedVertices.begin() + index * stride,
				draw.referencedVertices.begin() + (index + 3) * stride);
			triangles.push_back(triangle);
		}
	}
	return true;
}

void QueueTieCohort(NativeSortingRenderer &renderer, bool mixedLayouts,
	const NativeSortedPass *pass = 0, bool awaitBegin = false)
{
	// More than the insertion-sort threshold; each node has unique attributes,
	// and acknowledged nodes must remain in the legacy quicksort input.
	const unsigned short indices[] = {0, 1, 2};
	for (unsigned int node = 0; node < 24; ++node)
	{
		LegacyLogicalState state;
		state.pipeline.shaderBits = 1000 + node;
		TestVertexWide vertices[3] = {};
		for (unsigned int corner = 0; corner < 3; ++corner)
		{
			vertices[corner].x = static_cast<float>(node * 3 + corner);
			vertices[corner].z = 7.0f;
			vertices[corner].color = 0x10000000U + node * 3 + corner;
			vertices[corner].u = static_cast<float>(node);
		}
		NativeDrawPacket packet = MakePacket(3, 3);
		if (mixedLayouts && node % 2 != 0)
		{
			packet.vertexStride = sizeof(TestVertexWide);
			packet.vertexLayout.stride = sizeof(TestVertexWide);
			packet.vertexLayout.elementCount = 3;
			packet.vertexLayout.elements[2].semantic = RENDER_VERTEX_SEMANTIC_TEXTURE_COORDINATE;
			packet.vertexLayout.elements[2].format = RENDER_VERTEX_DATA_FLOAT2;
			packet.vertexLayout.elements[2].byteOffset = sizeof(TestVertex);
			CHECK(renderer.Queue(state, packet, vertices, sizeof(vertices),
				indices, sizeof(indices), 0, pass, awaitBegin) == RENDER_RESULT_OK);
		}
		else
		{
			TestVertex narrow[3];
			for (unsigned int corner = 0; corner < 3; ++corner)
				memcpy(&narrow[corner], &vertices[corner], sizeof(TestVertex));
			CHECK(renderer.Queue(state, packet, narrow, sizeof(narrow),
				indices, sizeof(indices), 0, pass, awaitBegin) == RENDER_RESULT_OK);
		}
	}
}

void QueueLaterTriangle(NativeSortingRenderer &renderer)
{
	std::vector<TestVertex> vertices;
	MakeVertices(vertices, std::vector<float>{-99.0f, -99.0f, -99.0f});
	QueueOne(renderer, 9000, vertices, std::vector<unsigned short>{0, 1, 2}, 0);
}

void TestCanonicalTieRetryAndQueueExtension()
{
	const unsigned int prefixes[] = {0, 1, 5, 24};
	for (unsigned int scenario = 0; scenario < 4; ++scenario)
	{
		for (unsigned int extend = 0; extend < 2; ++extend)
		{
			NativeSortingRenderer reference;
			RecordingSink expectedSink;
			QueueTieCohort(reference, false);
			if (extend && prefixes[scenario] == 0)
				QueueLaterTriangle(reference); // No emitted prefix: same cohort.
			CHECK(reference.Flush(expectedSink) == RENDER_RESULT_OK);
			if (extend && prefixes[scenario] != 0)
			{
				QueueLaterTriangle(reference); // Separate post-prefix cohort.
				CHECK(reference.Flush(expectedSink) == RENDER_RESULT_OK);
			}

			NativeSortingRenderer retry;
			RecordingSink actualSink;
			QueueTieCohort(retry, false);
			actualSink.failCall = 1;
			actualSink.acceptedOnFailure = prefixes[scenario];
			CHECK(retry.Flush(actualSink) == RENDER_RESULT_FAILED);
			CHECK(!retry.Empty()); // Includes all-accepted-but-error.
			if (extend)
				QueueLaterTriangle(retry); // CHECK inside verifies admission.
			if (prefixes[scenario] == 1)
			{
				actualSink.failCall = actualSink.calls + 1;
				actualSink.acceptedOnFailure = 2;
				CHECK(retry.Flush(actualSink) == RENDER_RESULT_FAILED);
				CHECK(!retry.Empty());
			}
			actualSink.failCall = 0;
			const unsigned int beforeCompletion = actualSink.calls;
			CHECK(retry.Flush(actualSink) == RENDER_RESULT_OK);
			CHECK(retry.Empty());
			if (prefixes[scenario] == 24)
				CHECK(actualSink.calls == beforeCompletion + extend);
			std::vector<CapturedDraw> expected, actual;
			CHECK(CaptureAcceptedTriangleStream(expectedSink, expected));
			CHECK(CaptureAcceptedTriangleStream(actualSink, actual));
			CHECK(expected.size() == 24 + extend);
			CHECK(SameAcceptedDrawStream(expected, actual));
		}
	}
}

void TestMixedLayoutCohortAndClear()
{
	NativeSortingRenderer reference;
	RecordingSink expectedSink;
	expectedSink.requireHomogeneous = true;
	QueueTieCohort(reference, true);
	CHECK(reference.Flush(expectedSink) == RENDER_RESULT_OK);
	NativeSortingRenderer retry;
	RecordingSink actualSink;
	actualSink.requireHomogeneous = true;
	QueueTieCohort(retry, true);
	actualSink.failCall = 3; // Acknowledgements from earlier successful chunks.
	actualSink.acceptedOnFailure = 0;
	CHECK(retry.Flush(actualSink) == RENDER_RESULT_FAILED);
	actualSink.failCall = actualSink.calls + 1;
	CHECK(retry.Flush(actualSink) == RENDER_RESULT_FAILED); // Repeated zero ack.
	actualSink.failCall = 0;
	CHECK(retry.Flush(actualSink) == RENDER_RESULT_OK);
	std::vector<CapturedDraw> expected, actual;
	CHECK(CaptureAcceptedTriangleStream(expectedSink, expected));
	CHECK(CaptureAcceptedTriangleStream(actualSink, actual));
	CHECK(SameAcceptedDrawStream(expected, actual));
	CHECK(retry.Empty());

	QueueTieCohort(retry, false);
	actualSink.failCall = actualSink.calls + 1;
	actualSink.acceptedOnFailure = 1;
	CHECK(retry.Flush(actualSink) == RENDER_RESULT_FAILED);
	QueueLaterTriangle(retry);
	retry.Clear();
	CHECK(retry.Empty());
	RecordingSink afterClear;
	QueueLaterTriangle(retry);
	CHECK(retry.Flush(afterClear) == RENDER_RESULT_OK);
	CHECK(afterClear.calls == 1);
	CHECK(afterClear.batches.size() == 1 && afterClear.batches[0].states.size() == 1 &&
		afterClear.batches[0].states[0] == 9000);
	CHECK(retry.Empty());
}

void TestIndexChunkCohortQueueExtension()
{
	std::vector<TestVertex> vertices;
	MakeVertices(vertices, std::vector<float>{7.0f, 7.0f, 7.0f});
	std::vector<unsigned short> indices(21846 * 3);
	for (size_t index = 0; index < indices.size(); ++index)
		indices[index] = static_cast<unsigned short>(index % 3);
	NativeSortingRenderer reference;
	RecordingSink expectedSink;
	QueueOne(reference, 8000, vertices, indices, 0);
	CHECK(reference.Flush(expectedSink) == RENDER_RESULT_OK);
	CHECK(expectedSink.calls == 2);
	QueueLaterTriangle(reference);
	CHECK(reference.Flush(expectedSink) == RENDER_RESULT_OK);
	NativeSortingRenderer retry;
	RecordingSink actualSink;
	QueueOne(retry, 8000, vertices, indices, 0);
	actualSink.failCall = 2;
	CHECK(retry.Flush(actualSink) == RENDER_RESULT_FAILED);
	QueueLaterTriangle(retry);
	actualSink.failCall = 0;
	CHECK(retry.Flush(actualSink) == RENDER_RESULT_OK);
	CHECK(retry.Empty());
	std::vector<CapturedDraw> expected, actual;
	CHECK(CaptureAcceptedTriangleStream(expectedSink, expected));
	CHECK(CaptureAcceptedTriangleStream(actualSink, actual));
	CHECK(expected.size() == 21847);
	CHECK(SameAcceptedDrawStream(expected, actual));
}

}

void QueueCapturedPassTriangle(NativeSortingRenderer &renderer,
	const NativeSortedPass &pass, unsigned int bits, float depth, bool awaitBegin = false)
{
	std::vector<TestVertex> vertices;
	MakeVertices(vertices, std::vector<float>{depth, depth, depth});
	const unsigned short indices[] = {0, 1, 2};
	LegacyLogicalState state;
	state.pipeline.shaderBits = bits;
	CHECK(renderer.Queue(state, MakePacket(3, 3), vertices.data(),
		vertices.size() * sizeof(TestVertex), indices, sizeof(indices), 0,
		&pass, awaitBegin) == RENDER_RESULT_OK);
}

void TestDeferredOutputIntervalAndRetry()
{
	NativeSortedPass main;
	main.captured = true;
	main.identity = 1;
	main.viewport = RenderViewport(7, 9, 640, 480, .25f, .75f);
	NativeSortedPass reflected = main;
	reflected.identity = 2;
	reflected.target.useBackBufferColor = false;
	reflected.target.hasColor = true;
	reflected.target.color.resource = GpuHandle(17, 3);
	reflected.viewport = RenderViewport(3, 5, 256, 256, 0, 1);
	for (unsigned int accepted : {0U, 1U, 24U})
	{
		NativeSortingRenderer reference;
		PassRecordingSink expectedSink;
		QueueTieCohort(reference, false, &reflected);
		CHECK(reference.Flush(expectedSink) == RENDER_RESULT_OK);
		QueueCapturedPassTriangle(reference, reflected, 9000, -100);
		CHECK(reference.Flush(expectedSink) == RENDER_RESULT_OK);
		QueueCapturedPassTriangle(reference, main, 33, -200);
		CHECK(reference.Flush(expectedSink) == RENDER_RESULT_OK);
		NativeSortingRenderer renderer;
		PassRecordingSink sink;
		QueueCapturedPassTriangle(renderer, main, 33, -200, true);
		CHECK(renderer.Flush(sink) == RENDER_RESULT_OK && sink.calls == 0 && !renderer.Empty());
		// Activation is full output equality, not two invalid color handles.
		RenderTargetBinding wrong = main.target;
		wrong.useBackBufferColor = false;
		wrong.hasColor = true;
		renderer.ActivateMatchingPass(wrong, 77);
		CHECK(renderer.Flush(sink) == RENDER_RESULT_OK && sink.calls == 0);
		renderer.ActivateMatchingPass(reflected.target, 2);
		QueueTieCohort(renderer, false, &reflected);
		sink.failCall = 1;
		sink.acceptedOnFailure = accepted;
		CHECK(renderer.Flush(sink) == RENDER_RESULT_FAILED);
		QueueCapturedPassTriangle(renderer, reflected, 9000, -100, true);
		if (accepted == 1)
		{
			sink.failCall = sink.calls + 1;
			sink.acceptedOnFailure = 2;
			CHECK(renderer.Flush(sink) == RENDER_RESULT_FAILED);
		}
		sink.failCall = 0;
		const unsigned int calls = sink.calls;
		CHECK(renderer.Flush(sink, true) == RENDER_RESULT_OK && !renderer.Empty());
		if (accepted == 24) CHECK(sink.calls == calls); // No empty resubmission.
		CHECK(NativeSortingRendererTestLastOffsetInitializations() == 24);
		CHECK(renderer.Flush(sink) == RENDER_RESULT_OK && !renderer.Empty());
		renderer.ActivateMatchingPass(reflected.target, 3);
		CHECK(renderer.Flush(sink) == RENDER_RESULT_OK && !renderer.Empty());
		renderer.ActivateMatchingPass(main.target, 4);
		CHECK(renderer.Flush(sink) == RENDER_RESULT_OK && renderer.Empty());
		CHECK(sink.passIdentities.back() == 4 && sink.viewports.back().x == 7 &&
			sink.viewports.back().minimumDepth == .25f);
		std::vector<CapturedDraw> expected, actual;
		CHECK(CaptureAcceptedTriangleStream(expectedSink, expected));
		CHECK(CaptureAcceptedTriangleStream(sink, actual));
		CHECK(expected.size() == 26 && SameAcceptedDrawStream(expected, actual));
	}
	// Explicit teardown releases both inactive and active intervals.
	NativeSortingRenderer renderer;
	QueueCapturedPassTriangle(renderer, main, 1, 0, true);
	QueueCapturedPassTriangle(renderer, reflected, 2, 0);
	renderer.Clear();
	CHECK(renderer.Empty());
}

void TestCapturedPassFailureAndTail()
{
	for (unsigned int accepted = 0; accepted <= 2; ++accepted)
	{
		NativeSortedPass reflection;
		reflection.captured = true;
		reflection.identity = 19;
		reflection.target.useBackBufferColor = false;
		reflection.target.hasColor = true;
		reflection.target.color.resource = GpuHandle(17, 3);
		reflection.viewport = RenderViewport(3, 5, 256, 256, .125f, .875f);
		NativeSortedPass main;
		main.captured = true;
		main.identity = 20;
		main.viewport = RenderViewport(0, 0, 640, 480, 0, 1);
		NativeSortingRenderer renderer;
		QueueCapturedPassTriangle(renderer, reflection, 11, 1);
		QueueCapturedPassTriangle(renderer, reflection, 22, 2);
		PassRecordingSink sink;
		sink.failCall = 1;
		sink.acceptedOnFailure = accepted;
		CHECK(renderer.Flush(sink) == RENDER_RESULT_FAILED);
		QueueCapturedPassTriangle(renderer, main, 33, -100);
		CHECK(renderer.Flush(sink, true) == RENDER_RESULT_OK);
		CHECK(!renderer.Empty()); // New pass must wait for its own clear.
		for (size_t call = 0; call < sink.targets.size(); ++call)
		{
			CHECK(sink.passIdentities[call] == 19);
			CHECK(sink.targets[call] == reflection.target.color.resource);
			CHECK(sink.viewports[call].x == 3 && sink.viewports[call].y == 5 &&
				sink.viewports[call].width == 256 && sink.viewports[call].height == 256 &&
				sink.viewports[call].minimumDepth == .125f &&
				sink.viewports[call].maximumDepth == .875f);
		}
		CHECK(renderer.Flush(sink) == RENDER_RESULT_OK && renderer.Empty());
		CHECK(sink.passIdentities.back() == 20 && !sink.targets.back().isValid());
		std::vector<CapturedDraw> stream;
		CHECK(CaptureAcceptedTriangleStream(sink, stream));
		CHECK(stream.size() == 3);
		if (stream.size() == 3)
			CHECK(stream[0].state == 11 && stream[1].state == 22 && stream[2].state == 33);
	}
	// Equal target identity but changed viewport/clear identity is another pass.
	NativeSortingRenderer renderer;
	NativeSortedPass a;
	a.captured = true; a.identity = 5;
	a.viewport = RenderViewport(0, 0, 256, 256, 0, 1);
	NativeSortedPass b = a;
	b.viewport.width = 128;
	QueueCapturedPassTriangle(renderer, a, 44, 3);
	QueueCapturedPassTriangle(renderer, b, 55, -3);
	PassRecordingSink sink;
	CHECK(renderer.Flush(sink) == RENDER_RESULT_OK);
	CHECK(sink.calls == 2 && sink.viewports[0].width == 256 && sink.viewports[1].width == 128);
}

void QueueArbitraryTriangle(NativeSortingRenderer &renderer,
	const LegacyLogicalState &state, const NativeDrawPacket &packet,
	float depth, unsigned int colorBase, const NativeSortedPass *pass = 0)
{
	TestVertex vertices[3] = {};
	for (unsigned int corner = 0; corner < 3; ++corner)
	{
		vertices[corner].x = static_cast<float>(colorBase + corner);
		vertices[corner].y = static_cast<float>(colorBase + corner + 3);
		vertices[corner].z = depth;
		vertices[corner].color = colorBase + corner;
	}
	const unsigned short indices[] = {0, 1, 2};
	CHECK(renderer.Queue(state, packet, vertices, sizeof(vertices), indices,
		sizeof(indices), 0, pass) == RENDER_RESULT_OK);
}

unsigned int RecordedDrawCount(const RecordingSink &sink)
{
	unsigned int count = 0;
	for (size_t batch = 0; batch < sink.batches.size(); ++batch)
		count += static_cast<unsigned int>(sink.batches[batch].states.size());
	return count;
}

void CheckKeyDifferenceKeepsSeparateDraws(
	const LegacyLogicalState &firstState, const NativeDrawPacket &firstPacket,
	const LegacyLogicalState &secondState, const NativeDrawPacket &secondPacket,
	const NativeSortedPass *firstPass = 0,
	const NativeSortedPass *secondPass = 0)
{
	NativeSortingRenderer renderer;
	QueueArbitraryTriangle(renderer, firstState, firstPacket, 8.0f,
		0x11000000U, firstPass);
	QueueArbitraryTriangle(renderer, secondState, secondPacket, 4.0f,
		0x22000000U, secondPass);
	PassRecordingSink sink;
	sink.requireHomogeneous = true;
	CHECK(renderer.Flush(sink) == RENDER_RESULT_OK);
	CHECK(renderer.Empty());
	CHECK(RecordedDrawCount(sink) == 2);
}

void PoisonLegacyLightStatePadding(LegacyLogicalState *state,
	unsigned char poison)
{
	const size_t paddingStart = offsetof(LegacyLightState, enabled) + sizeof(bool);
	const size_t paddingEnd = offsetof(LegacyLightState, type);
	if (paddingEnd > paddingStart)
	{
		unsigned char *lightBytes = reinterpret_cast<unsigned char *>(
			&state->constants.lights[0]);
		memset(lightBytes + paddingStart, poison, paddingEnd - paddingStart);
	}
}

void TestAdjacentSameStateSourcesCoalesceWithoutChangingTriangles()
{
	const unsigned int count = 257;
	const unsigned short indices[] = {0, 1, 2};
	LegacyLogicalState commonState;
	commonState.pipeline.shaderBits = 4242;
	const NativeDrawPacket packet = MakePacket(3, 3);

	NativeSortingRenderer reference;
	for (unsigned int source = 0; source < count; ++source)
	{
		LegacyLogicalState state = commonState;
		state.pipeline.shaderBits += source;
		TestVertex vertices[3] = {};
		for (unsigned int corner = 0; corner < 3; ++corner)
		{
			vertices[corner].x = static_cast<float>(source * 3 + corner);
			vertices[corner].y = static_cast<float>(source + corner);
			vertices[corner].z = static_cast<float>(count - source);
			vertices[corner].color = 0x10000000U + source * 3 + corner;
		}
		CHECK(reference.Queue(state, packet, vertices, sizeof(vertices), indices,
			sizeof(indices), 0) == RENDER_RESULT_OK);
	}
	RecordingSink referenceSink;
	CHECK(reference.Flush(referenceSink) == RENDER_RESULT_OK);
	CHECK(RecordedDrawCount(referenceSink) == count);

	NativeSortingRenderer coalesced;
	CHECK(offsetof(LegacyLightState, type) >
		offsetof(LegacyLightState, enabled) + sizeof(bool));
	unsigned char firstStateBytes[sizeof(LegacyLogicalState)];
	for (unsigned int source = 0; source < count; ++source)
	{
		TestVertex vertices[3] = {};
		for (unsigned int corner = 0; corner < 3; ++corner)
		{
			vertices[corner].x = static_cast<float>(source * 3 + corner);
			vertices[corner].y = static_cast<float>(source + corner);
			vertices[corner].z = static_cast<float>(count - source);
			vertices[corner].color = 0x10000000U + source * 3 + corner;
		}
		std::aligned_storage<sizeof(LegacyLogicalState)>::type stateStorage;
		const unsigned char poison = source % 2 == 0 ? 0xa5 : 0x5a;
		memset(&stateStorage, poison, sizeof(stateStorage));
		LegacyLogicalState *state = new (&stateStorage) LegacyLogicalState();
		state->pipeline.shaderBits = commonState.pipeline.shaderBits;
		PoisonLegacyLightStatePadding(state, poison);
		if (source == 0)
			memcpy(firstStateBytes, state, sizeof(*state));
		else if (source == 1)
			CHECK(memcmp(firstStateBytes, state, sizeof(*state)) != 0);
		CHECK(coalesced.Queue(*state, packet, vertices, sizeof(vertices),
			indices, sizeof(indices), 0) == RENDER_RESULT_OK);
		state->~LegacyLogicalState();
	}
	RecordingSink coalescedSink;
	CHECK(coalesced.Flush(coalescedSink) == RENDER_RESULT_OK);
	CHECK(RecordedDrawCount(coalescedSink) == 1);
	CHECK(coalescedSink.batches.size() == 1);
	if (coalescedSink.batches.size() == 1)
	{
		const CapturedBatch &batch = coalescedSink.batches[0];
		CHECK(batch.indexCounts.size() == 1 && batch.indexCounts[0] == count * 3);
		CHECK(batch.vertexOffsets.size() == 1 && batch.vertexOffsets[0] == 0);
		CHECK(batch.vertexCounts.size() == 1 && batch.vertexCounts[0] == count * 3);
		CHECK(batch.indices.size() == count * 3);
		CHECK(batch.vertices.size() == count * 3 * sizeof(TestVertex));
	}
	std::vector<CapturedDraw> expected, actual;
	CHECK(CaptureAcceptedTriangleStream(referenceSink, expected));
	CHECK(CaptureAcceptedTriangleStream(coalescedSink, actual));
	CHECK(expected.size() == count && actual.size() == count);
	CHECK(SameAcceptedTriangleVertexStream(expected, actual));
}

void TestAdjacentSameStateCoalescingKeyBoundaries()
{
	LegacyLogicalState state;
	state.pipeline.shaderBits = 90;
	NativeDrawPacket packet = MakePacket(3, 3);

	LegacyLogicalState shaderChange = state;
	shaderChange.pipeline.shaderBits++;
	CheckKeyDifferenceKeepsSeparateDraws(state, packet, shaderChange, packet);

	LegacyLogicalState blendChange = state;
	blendChange.pipeline.blend.blendEnable =
		!blendChange.pipeline.blend.blendEnable;
	CheckKeyDifferenceKeepsSeparateDraws(state, packet, blendChange, packet);
	LegacyLogicalState depthChange = state;
	depthChange.pipeline.depthStencil.depthWrite =
		!depthChange.pipeline.depthStencil.depthWrite;
	CheckKeyDifferenceKeepsSeparateDraws(state, packet, depthChange, packet);
	LegacyLogicalState textureStageChange = state;
	textureStageChange.pipeline.textureStages[0].colorOperation =
		RENDER_TEXTURE_OP_SELECT_ARGUMENT_1;
	CheckKeyDifferenceKeepsSeparateDraws(state, packet, textureStageChange,
		packet);

	LegacyLogicalState constantsChange = state;
	constantsChange.constants.world.values[14] = 2.0f;
	CheckKeyDifferenceKeepsSeparateDraws(state, packet, constantsChange, packet);
	LegacyLogicalState shaderConstantsChange = state;
	shaderConstantsChange.constants.pixelShaderConstants[0].x = 0.5f;
	CheckKeyDifferenceKeepsSeparateDraws(state, packet,
		shaderConstantsChange, packet);
	LegacyLogicalState lightEnabledChange = state;
	lightEnabledChange.constants.lights[0].enabled = true;
	CheckKeyDifferenceKeepsSeparateDraws(state, packet, lightEnabledChange,
		packet);
	LegacyLogicalState lightTypeChange = state;
	lightTypeChange.constants.lights[0].type = RENDER_LIGHT_POINT;
	CheckKeyDifferenceKeepsSeparateDraws(state, packet, lightTypeChange,
		packet);
	LegacyLogicalState positiveZeroState = state;
	positiveZeroState.constants.world.values[1] = 0.0f;
	LegacyLogicalState negativeZeroState = positiveZeroState;
	negativeZeroState.constants.world.values[1] = -0.0f;
	CheckKeyDifferenceKeepsSeparateDraws(positiveZeroState, packet,
		negativeZeroState, packet);

	LegacyLogicalState stateTextureMaskChange = state;
	stateTextureMaskChange.texturePresenceMask = 1;
	CheckKeyDifferenceKeepsSeparateDraws(state, packet, stateTextureMaskChange,
		packet);

	LegacyLogicalState texturedState = state;
	texturedState.texturePresenceMask = 1;
	NativeDrawPacket firstTexture = packet;
	NativeDrawPacket secondTexture = packet;
	firstTexture.texturePresenceMask = secondTexture.texturePresenceMask = 1;
	firstTexture.textures[0] = GpuHandle(17, 3);
	secondTexture.textures[0] = GpuHandle(18, 3);
	CheckKeyDifferenceKeepsSeparateDraws(texturedState, firstTexture,
		texturedState, secondTexture);
	NativeDrawPacket textureGenerationChange = firstTexture;
	textureGenerationChange.textures[0] = GpuHandle(17, 4);
	CheckKeyDifferenceKeepsSeparateDraws(texturedState, firstTexture,
		texturedState, textureGenerationChange);
	LegacyLogicalState lastStageState = state;
	lastStageState.texturePresenceMask = 1U << (LEGACY_TEXTURE_STAGE_COUNT - 1);
	NativeDrawPacket firstLastStage = packet;
	NativeDrawPacket secondLastStage = packet;
	firstLastStage.texturePresenceMask = secondLastStage.texturePresenceMask =
		1U << (LEGACY_TEXTURE_STAGE_COUNT - 1);
	firstLastStage.textures[LEGACY_TEXTURE_STAGE_COUNT - 1] = GpuHandle(20, 4);
	secondLastStage.textures[LEGACY_TEXTURE_STAGE_COUNT - 1] = GpuHandle(21, 4);
	CheckKeyDifferenceKeepsSeparateDraws(lastStageState, firstLastStage,
		lastStageState, secondLastStage);

	NativeDrawPacket packetMaskChange = firstTexture;
	packetMaskChange.texturePresenceMask = 3;
	packetMaskChange.textures[1] = GpuHandle(19, 4);
	CheckKeyDifferenceKeepsSeparateDraws(texturedState, firstTexture,
		texturedState, packetMaskChange);

	NativeDrawPacket elementCountChange = packet;
	elementCountChange.vertexLayout.elementCount = 1;
	CheckKeyDifferenceKeepsSeparateDraws(state, packet, state,
		elementCountChange);

	NativeDrawPacket elementOffsetChange = packet;
	elementOffsetChange.vertexLayout.elements[1].byteOffset = sizeof(float) * 2;
	CheckKeyDifferenceKeepsSeparateDraws(state, packet, state,
		elementOffsetChange);
	NativeDrawPacket preTransformedChange = packet;
	preTransformedChange.vertexLayout.preTransformed = true;
	CheckKeyDifferenceKeepsSeparateDraws(state, packet, state,
		preTransformedChange);
	NativeDrawPacket semanticChange = packet;
	semanticChange.vertexLayout.elements[1].semantic =
		RENDER_VERTEX_SEMANTIC_TEXTURE_COORDINATE;
	CheckKeyDifferenceKeepsSeparateDraws(state, packet, state, semanticChange);
	NativeDrawPacket semanticIndexChange = packet;
	semanticIndexChange.vertexLayout.elements[1].semanticIndex = 1;
	CheckKeyDifferenceKeepsSeparateDraws(state, packet, state,
		semanticIndexChange);
	NativeDrawPacket elementFormatChange = packet;
	elementFormatChange.vertexLayout.elements[0].format =
		RENDER_VERTEX_DATA_FLOAT4;
	CheckKeyDifferenceKeepsSeparateDraws(state, packet, state,
		elementFormatChange);

	NativeDrawPacket vertexFormatChange = packet;
	vertexFormatChange.vertexFormat = RENDER_VERTEX_POSITION3_NORMAL_COLOR_TEX1;
	CheckKeyDifferenceKeepsSeparateDraws(state, packet, state,
		vertexFormatChange);

	NativeSortedPass pass;
	pass.captured = true;
	pass.identity = 5;
	pass.viewport = RenderViewport(0, 0, 640, 480, 0.0f, 1.0f);
	NativeSortedPass captureChange;
	CheckKeyDifferenceKeepsSeparateDraws(state, packet, state, packet,
		&pass, &captureChange);
	NativeSortedPass identityChange = pass;
	identityChange.identity++;
	CheckKeyDifferenceKeepsSeparateDraws(state, packet, state, packet,
		&pass, &identityChange);

	NativeSortedPass targetChange = pass;
	targetChange.target.useBackBufferColor =
		!targetChange.target.useBackBufferColor;
	CheckKeyDifferenceKeepsSeparateDraws(state, packet, state, packet,
		&pass, &targetChange);
	NativeSortedPass backBufferDepthChange = pass;
	backBufferDepthChange.target.useBackBufferDepth =
		!backBufferDepthChange.target.useBackBufferDepth;
	CheckKeyDifferenceKeepsSeparateDraws(state, packet, state, packet,
		&pass, &backBufferDepthChange);
	NativeSortedPass hasColorChange = pass;
	hasColorChange.target.hasColor = !hasColorChange.target.hasColor;
	CheckKeyDifferenceKeepsSeparateDraws(state, packet, state, packet,
		&pass, &hasColorChange);
	NativeSortedPass hasDepthChange = pass;
	hasDepthChange.target.hasDepth = !hasDepthChange.target.hasDepth;
	CheckKeyDifferenceKeepsSeparateDraws(state, packet, state, packet,
		&pass, &hasDepthChange);
	NativeSortedPass colorResourceChange = pass;
	colorResourceChange.target.color.resource = GpuHandle(22, 1);
	CheckKeyDifferenceKeepsSeparateDraws(state, packet, state, packet,
		&pass, &colorResourceChange);
	NativeSortedPass colorGenerationChange = pass;
	colorGenerationChange.target.color.resource = GpuHandle(0, 1);
	CheckKeyDifferenceKeepsSeparateDraws(state, packet, state, packet,
		&pass, &colorGenerationChange);
	NativeSortedPass colorMipChange = pass;
	colorMipChange.target.color.mip++;
	CheckKeyDifferenceKeepsSeparateDraws(state, packet, state, packet,
		&pass, &colorMipChange);
	NativeSortedPass colorSliceChange = pass;
	colorSliceChange.target.color.arraySlice++;
	CheckKeyDifferenceKeepsSeparateDraws(state, packet, state, packet,
		&pass, &colorSliceChange);
	NativeSortedPass depthResourceChange = pass;
	depthResourceChange.target.depth.resource = GpuHandle(23, 2);
	CheckKeyDifferenceKeepsSeparateDraws(state, packet, state, packet,
		&pass, &depthResourceChange);
	NativeSortedPass depthMipChange = pass;
	depthMipChange.target.depth.mip++;
	CheckKeyDifferenceKeepsSeparateDraws(state, packet, state, packet,
		&pass, &depthMipChange);
	NativeSortedPass depthSliceChange = pass;
	depthSliceChange.target.depth.arraySlice++;
	CheckKeyDifferenceKeepsSeparateDraws(state, packet, state, packet,
		&pass, &depthSliceChange);
	NativeSortedPass viewportXChange = pass;
	viewportXChange.viewport.x++;
	CheckKeyDifferenceKeepsSeparateDraws(state, packet, state, packet,
		&pass, &viewportXChange);
	NativeSortedPass viewportYChange = pass;
	viewportYChange.viewport.y++;
	CheckKeyDifferenceKeepsSeparateDraws(state, packet, state, packet,
		&pass, &viewportYChange);
	NativeSortedPass viewportHeightChange = pass;
	viewportHeightChange.viewport.height++;
	CheckKeyDifferenceKeepsSeparateDraws(state, packet, state, packet,
		&pass, &viewportHeightChange);
	NativeSortedPass viewportMinimumDepthChange = pass;
	viewportMinimumDepthChange.viewport.minimumDepth = 0.25f;
	CheckKeyDifferenceKeepsSeparateDraws(state, packet, state, packet,
		&pass, &viewportMinimumDepthChange);
	NativeSortedPass viewportMaximumDepthChange = pass;
	viewportMaximumDepthChange.viewport.maximumDepth = 0.75f;
	CheckKeyDifferenceKeepsSeparateDraws(state, packet, state, packet,
		&pass, &viewportMaximumDepthChange);

	NativeSortedPass viewportChange = pass;
	viewportChange.viewport.width++;
	CheckKeyDifferenceKeepsSeparateDraws(state, packet, state, packet,
		&pass, &viewportChange);
}

void TestAdjacentSameStateStrideDifferenceDoesNotCoalesce()
{
	const unsigned short indices[] = {0, 1, 2};
	LegacyLogicalState state;
	state.pipeline.shaderBits = 123;
	TestVertex narrow[3] = {};
	TestVertexWide wide[3] = {};
	for (unsigned int corner = 0; corner < 3; ++corner)
	{
		narrow[corner].x = static_cast<float>(corner);
		narrow[corner].z = 8.0f;
		wide[corner].x = static_cast<float>(corner + 10);
		wide[corner].z = 4.0f;
	}
	NativeDrawPacket narrowPacket = MakePacket(3, 3);
	NativeDrawPacket widePacket = MakePacket(3, 3);
	widePacket.vertexStride = sizeof(TestVertexWide);
	widePacket.vertexFormat = RENDER_VERTEX_POSITION3_NORMAL_COLOR_TEX1;
	widePacket.vertexLayout.stride = sizeof(TestVertexWide);
	widePacket.vertexLayout.elementCount = 3;
	widePacket.vertexLayout.elements[2].semantic =
		RENDER_VERTEX_SEMANTIC_TEXTURE_COORDINATE;
	widePacket.vertexLayout.elements[2].format = RENDER_VERTEX_DATA_FLOAT2;
	widePacket.vertexLayout.elements[2].byteOffset = sizeof(TestVertex);

	NativeSortingRenderer renderer;
	CHECK(renderer.Queue(state, narrowPacket, narrow, sizeof(narrow), indices,
		sizeof(indices), 0) == RENDER_RESULT_OK);
	CHECK(renderer.Queue(state, widePacket, wide, sizeof(wide), indices,
		sizeof(indices), 0) == RENDER_RESULT_OK);
	RecordingSink sink;
	sink.requireHomogeneous = true;
	CHECK(renderer.Flush(sink) == RENDER_RESULT_OK);
	CHECK(renderer.Empty());
	CHECK(RecordedDrawCount(sink) == 2);
}

void TestUnsupportedGeometryStillFailsQueueValidation()
{
	LegacyLogicalState state;
	const NativeDrawPacket packet = MakePacket(3, 3);
	TestVertex vertices[3] = {};
	const unsigned short indices[] = {0, 1, 2};
	NativeSortingRenderer renderer;
	CHECK(renderer.Queue(state, packet, vertices, sizeof(vertices), indices,
		sizeof(indices), 0) == RENDER_RESULT_OK);
	NativeDrawPacket wrongIndexFormat = packet;
	wrongIndexFormat.indexFormat = RENDER_FORMAT_R32_UINT;
	CHECK(renderer.Queue(state, wrongIndexFormat, vertices, sizeof(vertices),
		indices, sizeof(indices), 0) == RENDER_RESULT_INVALID_ARGUMENT);
	NativeDrawPacket wrongTopology = packet;
	wrongTopology.topology = RENDER_PRIMITIVE_LINE_LIST;
	CHECK(renderer.Queue(state, wrongTopology, vertices, sizeof(vertices),
		indices, sizeof(indices), 0) == RENDER_RESULT_INVALID_ARGUMENT);
	RecordingSink sink;
	CHECK(renderer.Flush(sink) == RENDER_RESULT_OK);
	CHECK(RecordedDrawCount(sink) == 1);
}

void QueuePartialMergedDrawAcknowledgementCase(
	NativeSortingRenderer &renderer, const LegacyLogicalState &sharedState,
	const LegacyLogicalState &otherState, const NativeDrawPacket &packet,
	bool mergedRunFirst)
{
	if (mergedRunFirst)
	{
		// Queue the other state first, but sort the shared run ahead of it.
		QueueArbitraryTriangle(renderer, otherState, packet, 10.0f,
			0x33000000U);
		QueueArbitraryTriangle(renderer, sharedState, packet, 7.0f,
			0x22000000U);
		QueueArbitraryTriangle(renderer, sharedState, packet, 9.0f,
			0x11000000U);
		return;
	}

	// Preserve the previously observed standalone-first, merged-retry order.
	QueueArbitraryTriangle(renderer, sharedState, packet, 9.0f,
		0x11000000U);
	QueueArbitraryTriangle(renderer, sharedState, packet, 7.0f,
		0x22000000U);
	QueueArbitraryTriangle(renderer, otherState, packet, 5.0f,
		0x33000000U);
}

unsigned int AcceptedTriangleCount(const CapturedBatch &batch)
{
	size_t acceptedIndexCount = 0;
	for (unsigned int draw = 0; draw < batch.acceptedDrawCount &&
		draw < batch.indexCounts.size(); ++draw)
		acceptedIndexCount += batch.indexCounts[draw];
	return static_cast<unsigned int>(acceptedIndexCount / 3);
}

void CheckPartialMergedDrawAcknowledgementCase(bool mergedRunFirst)
{
	const NativeDrawPacket packet = MakePacket(3, 3);
	LegacyLogicalState sharedState;
	sharedState.pipeline.shaderBits = 700;
	LegacyLogicalState otherState = sharedState;
	otherState.pipeline.shaderBits = 800;
	const unsigned int firstState = mergedRunFirst ?
		sharedState.pipeline.shaderBits : otherState.pipeline.shaderBits;
	const unsigned int tailState = mergedRunFirst ?
		otherState.pipeline.shaderBits : sharedState.pipeline.shaderBits;
	const unsigned int firstIndexCount = mergedRunFirst ? 6 : 3;
	const unsigned int tailIndexCount = mergedRunFirst ? 3 : 6;
	const unsigned int firstAcceptedTriangles = mergedRunFirst ? 2 : 1;

	NativeSortingRenderer baseline;
	QueuePartialMergedDrawAcknowledgementCase(baseline, sharedState,
		otherState, packet, mergedRunFirst);
	RecordingSink baselineSink;
	CHECK(baseline.Flush(baselineSink) == RENDER_RESULT_OK);
	CHECK(baseline.Empty());
	CHECK(baselineSink.calls == 1 && baselineSink.batches.size() == 1);
	CHECK(RecordedDrawCount(baselineSink) == 2);
	if (baselineSink.batches.size() == 1)
	{
		const CapturedBatch &batch = baselineSink.batches[0];
		CHECK(batch.states.size() == 2 && batch.indexCounts.size() == 2);
		if (batch.states.size() == 2 && batch.indexCounts.size() == 2)
		{
			CHECK(batch.states[0] == firstState && batch.states[1] == tailState);
			CHECK(batch.indexCounts[0] == firstIndexCount &&
				batch.indexCounts[1] == tailIndexCount);
		}
		CHECK(batch.acceptedDrawCount == 2);
		CHECK(AcceptedTriangleCount(batch) == 3);
	}

	NativeSortingRenderer retry;
	QueuePartialMergedDrawAcknowledgementCase(retry, sharedState,
		otherState, packet, mergedRunFirst);
	RecordingSink retrySink;
	retrySink.failCall = 1;
	retrySink.acceptedOnFailure = 1;
	CHECK(retry.Flush(retrySink) == RENDER_RESULT_FAILED);
	CHECK(!retry.Empty());
	CHECK(retrySink.calls == 1 && retrySink.batches.size() == 1);
	if (retrySink.batches.size() == 1)
	{
		const CapturedBatch &batch = retrySink.batches[0];
		CHECK(batch.states.size() == 2 && batch.indexCounts.size() == 2);
		if (batch.states.size() == 2 && batch.indexCounts.size() == 2)
		{
			CHECK(batch.states[0] == firstState && batch.states[1] == tailState);
			CHECK(batch.indexCounts[0] == firstIndexCount &&
				batch.indexCounts[1] == tailIndexCount);
		}
		CHECK(batch.acceptedDrawCount == 1);
		CHECK(AcceptedTriangleCount(batch) == firstAcceptedTriangles);
	}

	retrySink.failCall = 0;
	CHECK(retry.Flush(retrySink) == RENDER_RESULT_OK);
	CHECK(retry.Empty());
	CHECK(retrySink.calls == 2 && retrySink.batches.size() == 2);
	if (retrySink.batches.size() == 2)
	{
		const CapturedBatch &batch = retrySink.batches[1];
		CHECK(batch.states.size() == 1 && batch.indexCounts.size() == 1);
		if (batch.states.size() == 1 && batch.indexCounts.size() == 1)
		{
			CHECK(batch.states[0] == tailState);
			CHECK(batch.indexCounts[0] == tailIndexCount);
		}
		CHECK(batch.acceptedDrawCount == 1);
		CHECK(AcceptedTriangleCount(batch) == tailIndexCount / 3);
	}

	std::vector<CapturedDraw> expected, actual;
	CHECK(CaptureAcceptedTriangleStream(baselineSink, expected));
	CHECK(CaptureAcceptedTriangleStream(retrySink, actual));
	CHECK(expected.size() == 3 && actual.size() == 3);
	CHECK(SameAcceptedDrawStream(expected, actual));
}

void TestPartialMergedDrawAcknowledgementAcrossSources()
{
	CheckPartialMergedDrawAcknowledgementCase(true);
	CheckPartialMergedDrawAcknowledgementCase(false);
}

void TestCoalescedRunsResetAcrossR16ChunksAndRetry()
{
	const unsigned int trianglesPerSource = 10923;
	const unsigned short triangleIndices[] = {0, 1, 2};
	const std::vector<unsigned short> indices(trianglesPerSource * 3,
		triangleIndices[0]);
	std::vector<unsigned short> sourceIndices = indices;
	for (size_t triangle = 0; triangle < trianglesPerSource; ++triangle)
	{
		sourceIndices[triangle * 3 + 1] = triangleIndices[1];
		sourceIndices[triangle * 3 + 2] = triangleIndices[2];
	}
	TestVertex firstVertices[3] = {};
	TestVertex secondVertices[3] = {};
	for (unsigned int corner = 0; corner < 3; ++corner)
	{
		firstVertices[corner].x = static_cast<float>(corner);
		firstVertices[corner].z = 8.0f;
		firstVertices[corner].color = 0x11000000U + corner;
		secondVertices[corner].x = static_cast<float>(corner + 10);
		secondVertices[corner].z = 7.0f;
		secondVertices[corner].color = 0x22000000U + corner;
	}
	const NativeDrawPacket packet = MakePacket(3,
		static_cast<unsigned int>(sourceIndices.size()));
	LegacyLogicalState state;
	state.pipeline.shaderBits = 99;

	NativeSortingRenderer baseline;
	CHECK(baseline.Queue(state, packet, firstVertices, sizeof(firstVertices),
		sourceIndices.data(), sourceIndices.size() * sizeof(unsigned short),
		0) == RENDER_RESULT_OK);
	CHECK(baseline.Queue(state, packet, secondVertices, sizeof(secondVertices),
		sourceIndices.data(), sourceIndices.size() * sizeof(unsigned short),
		0) == RENDER_RESULT_OK);
	RecordingSink baselineSink;
	CHECK(baseline.Flush(baselineSink) == RENDER_RESULT_OK);
	CHECK(baselineSink.calls == 2 && RecordedDrawCount(baselineSink) == 2);
	if (baselineSink.batches.size() == 2)
	{
		CHECK(baselineSink.batches[0].states.size() == 1);
		CHECK(baselineSink.batches[0].indexCounts[0] == 65535);
		CHECK(baselineSink.batches[0].vertexOffsets[0] == 0);
		CHECK(baselineSink.batches[0].vertexCounts[0] == 6);
		CHECK(baselineSink.batches[1].states.size() == 1);
		CHECK(baselineSink.batches[1].indexCounts[0] == 3);
	}

	NativeSortingRenderer retry;
	CHECK(retry.Queue(state, packet, firstVertices, sizeof(firstVertices),
		sourceIndices.data(), sourceIndices.size() * sizeof(unsigned short),
		0) == RENDER_RESULT_OK);
	CHECK(retry.Queue(state, packet, secondVertices, sizeof(secondVertices),
		sourceIndices.data(), sourceIndices.size() * sizeof(unsigned short),
		0) == RENDER_RESULT_OK);
	RecordingSink retrySink;
	retrySink.failCall = 2;
	retrySink.acceptedOnFailure = 0;
	CHECK(retry.Flush(retrySink) == RENDER_RESULT_FAILED);
	CHECK(!retry.Empty());
	CHECK(NativeSortingRendererTestLastOffsetResets() == 2);
	CHECK(retrySink.batches.size() == 2 &&
		retrySink.batches[0].acceptedDrawCount == 1 &&
		retrySink.batches[1].acceptedDrawCount == 0);

	retrySink.failCall = 0;
	CHECK(retry.Flush(retrySink) == RENDER_RESULT_OK);
	CHECK(retry.Empty());
	CHECK(retrySink.batches.size() == 3 &&
		retrySink.batches[2].states.size() == 1 &&
		retrySink.batches[2].indexCounts[0] == 3);
	std::vector<CapturedDraw> expected, actual;
	CHECK(CaptureAcceptedTriangleStream(baselineSink, expected));
	CHECK(CaptureAcceptedTriangleStream(retrySink, actual));
	CHECK(expected.size() == trianglesPerSource * 2);
	CHECK(expected.size() == actual.size());
	CHECK(SameAcceptedTriangleVertexStream(expected, actual));
}

void TestLinearOffsetWorkspaceAlternatingLayouts()
{
	NativeSortingRenderer renderer;
	const unsigned int count = 257;
	const unsigned short indices[] = {0, 1, 2};
	for (unsigned int node = 0; node < count; ++node)
	{
		TestVertex vertices[3] = {};
		for (unsigned int i = 0; i < 3; ++i) vertices[i].z = static_cast<float>(node);
		NativeDrawPacket packet = MakePacket(3, 3);
		if (node & 1U) packet.vertexLayout.elementCount = 1;
		LegacyLogicalState state;
		state.pipeline.shaderBits = node;
		CHECK(renderer.Queue(state, packet, vertices, sizeof(vertices), indices,
			sizeof(indices), 0) == RENDER_RESULT_OK);
	}
	RecordingSink sink;
	sink.requireHomogeneous = true;
	CHECK(renderer.Flush(sink) == RENDER_RESULT_OK);
	CHECK(sink.calls == count);
	CHECK(NativeSortingRendererTestLastOffsetInitializations() == count);
	CHECK(NativeSortingRendererTestLastOffsetResets() == count - 1);
	CHECK(NativeSortingRendererTestLastFlushWorkspaceCapacityBytes() <= 24ULL * 1024ULL * 1024ULL);
	std::vector<CapturedDraw> stream;
	CHECK(CaptureAcceptedTriangleStream(sink, stream) && stream.size() == count);
	for (size_t i = 0; i < stream.size(); ++i) CHECK(stream[i].state == i);
}

// Capture every state value varied by this fixture as bytes without relying
// on padding in LegacyLogicalState. Batch geometry already captures exact
// packet offsets/counts, indices and all uploaded vertex bytes.
class PreparationContractSink : public PassRecordingSink
{
public:
	std::vector<std::vector<unsigned char> > stateBytes;
	RenderResult SubmitNativeSortedPassBatch(const NativeSortedPass &pass,
		const NativeSortedDraw *draws, unsigned int drawCount,
		const void *vertices, size_t vertexBytes, const void *indices,
		size_t indexBytes, unsigned int *accepted) override
	{
		std::vector<unsigned char> bytes;
		for (unsigned int i = 0; i < drawCount; ++i)
		{
			const LegacyLogicalState &state = draws[i].state;
			Append(bytes, &state.pipeline.shaderBits, sizeof(state.pipeline.shaderBits));
			Append(bytes, &state.pipeline.alphaReference, sizeof(state.pipeline.alphaReference));
			Append(bytes, &state.pipeline.alphaTestEnable, sizeof(state.pipeline.alphaTestEnable));
			Append(bytes, &draws[i].packet.vertexFormat, sizeof(draws[i].packet.vertexFormat));
			const RenderVertexLayout &layout = draws[i].packet.vertexLayout;
			Append(bytes, &layout.elementCount, sizeof(layout.elementCount));
			for (unsigned int e = 0; e < layout.elementCount; ++e)
			{
				Append(bytes, &layout.elements[e].semantic, sizeof(layout.elements[e].semantic));
				Append(bytes, &layout.elements[e].semanticIndex, sizeof(layout.elements[e].semanticIndex));
				Append(bytes, &layout.elements[e].format, sizeof(layout.elements[e].format));
				Append(bytes, &layout.elements[e].byteOffset, sizeof(layout.elements[e].byteOffset));
			}
			Append(bytes, state.constants.world.values, sizeof(state.constants.world.values));
			Append(bytes, state.constants.view.values, sizeof(state.constants.view.values));
			Append(bytes, &state.constants.material.diffuse, sizeof(state.constants.material.diffuse));
			Append(bytes, &state.texturePresenceMask, sizeof(state.texturePresenceMask));
		}
		stateBytes.push_back(bytes);
		return PassRecordingSink::SubmitNativeSortedPassBatch(pass, draws, drawCount,
			vertices, vertexBytes, indices, indexBytes, accepted);
	}
private:
	static void Append(std::vector<unsigned char> &bytes, const void *value, size_t size)
	{
		const unsigned char *first = static_cast<const unsigned char *>(value);
		bytes.insert(bytes.end(), first, first + size);
	}
};

void QueuePreparationBatchFixture(NativeSortingRenderer &renderer,
	unsigned int sources, unsigned int trianglesPerSource, bool oversized)
{
	NativeSortedPass pass;
	pass.captured = true;
	pass.identity = 991;
	pass.viewport.width = 1920;
	pass.viewport.height = 1080;
	for (unsigned int node = 0; node < sources; ++node)
	{
		LegacyLogicalState state;
		state.pipeline.shaderBits = 5000 + node;
		state.pipeline.alphaReference = node % 255;
		state.pipeline.alphaTestEnable = node % 2 != 0;
		state.texturePresenceMask = node % 4;
		state.constants.material.diffuse.x = static_cast<float>(node % 7) / 8.0f;
		// Some common-Z descriptors, some full transforms. Repeated values
		// deliberately generate equal-depth ties across batch boundaries.
		if (node % 3 != 0)
		{
			state.constants.world.values[2] = 0.5f;
			state.constants.view.values[6] = -0.25f;
			state.constants.world.values[14] = static_cast<float>(node % 5);
		}
		TestVertexWide vertices[9] = {};
		for (unsigned int i = 0; i < 9; ++i)
		{
			vertices[i].x = static_cast<float>(i % 3);
			vertices[i].y = static_cast<float>((i / 3) % 2);
			vertices[i].z = static_cast<float>(node % 5);
			vertices[i].color = node * 16 + i;
			vertices[i].u = static_cast<float>(i) / 16.0f;
			vertices[i].v = static_cast<float>(node % 9) / 16.0f;
		}
		const unsigned int count = oversized && node == 1 ? 65536U : trianglesPerSource;
		std::vector<unsigned short> indices(count * 3);
		for (unsigned int i = 0; i < indices.size(); ++i)
			indices[i] = static_cast<unsigned short>(5 + i % 9);
		NativeDrawPacket packet = MakePacket(9, count * 3);
		packet.minimumVertexIndex = 5;
		std::vector<unsigned char> packed;
		if (node % 2 != 0)
		{
			packet.vertexStride = sizeof(TestVertexWide);
			packet.vertexLayout.stride = sizeof(TestVertexWide);
			packet.vertexLayout.elementCount = 3;
			packet.vertexLayout.elements[2].semantic = RENDER_VERTEX_SEMANTIC_TEXTURE_COORDINATE;
			packet.vertexLayout.elements[2].semanticIndex = 0;
			packet.vertexLayout.elements[2].format = RENDER_VERTEX_DATA_FLOAT2;
			packet.vertexLayout.elements[2].byteOffset = sizeof(TestVertex);
		}
		for (unsigned int i = 0; i < 9; ++i)
		{
			const unsigned char *first = reinterpret_cast<const unsigned char *>(&vertices[i]);
			packed.insert(packed.end(), first, first + packet.vertexStride);
		}
		GameBoundingSphere sphere = {};
		sphere.centerZ = static_cast<float>(static_cast<int>(node % 7) - 3);
		sphere.radius = 1.0f;
		CHECK(renderer.Queue(state, packet, packed.data(), packed.size(),
			indices.data(), indices.size() * sizeof(unsigned short),
			node % 4 == 0 ? &sphere : 0, &pass) == RENDER_RESULT_OK);
	}
}

void CheckPreparationContractSinks(const PreparationContractSink &reference,
	const PreparationContractSink &batched)
{
	CHECK(reference.batches.size() == batched.batches.size());
	for (size_t i = 0; i < reference.batches.size() && i < batched.batches.size(); ++i)
		CHECK(SameCapturedBatchBytes(reference.batches[i], batched.batches[i]));
	CHECK(reference.stateBytes == batched.stateBytes);
	CHECK(reference.passIdentities == batched.passIdentities);
	CHECK(reference.targets == batched.targets);
	CHECK(reference.viewports.size() == batched.viewports.size());
	for (size_t i = 0; i < reference.viewports.size() && i < batched.viewports.size(); ++i)
	{
		CHECK(reference.viewports[i].width == batched.viewports[i].width);
		CHECK(reference.viewports[i].height == batched.viewports[i].height);
		CHECK(reference.viewports[i].x == batched.viewports[i].x);
		CHECK(reference.viewports[i].y == batched.viewports[i].y);
		CHECK(reference.viewports[i].minimumDepth == batched.viewports[i].minimumDepth);
		CHECK(reference.viewports[i].maximumDepth == batched.viewports[i].maximumDepth);
	}
}

void TestBoundedPreparationBatchesMatchSerialDescriptorStream()
{
	// Descriptor ceiling, aggregate polygon ceiling, oversized single source.
	const unsigned int sources[] = {4350, 600, 3};
	const unsigned int counts[] = {1, 128, 3};
	for (unsigned int fixture = 0; fixture < 3; ++fixture)
	{
		PreparationContractSink reference, batched, referenceRetry, batchedRetry;
		NativeSortingRenderer serialRenderer, batchRenderer, serialRetryRenderer, batchRetryRenderer;
		QueuePreparationBatchFixture(serialRenderer, sources[fixture], counts[fixture], fixture == 2);
		QueuePreparationBatchFixture(batchRenderer, sources[fixture], counts[fixture], fixture == 2);
		NativeSortingRendererTestUseSingleDescriptorReference(true);
		CHECK(serialRenderer.Flush(reference) == RENDER_RESULT_OK);
		CHECK(serialRenderer.Empty());
		NativeSortingRendererTestUseSingleDescriptorReference(false);
		CHECK(batchRenderer.Flush(batched) == RENDER_RESULT_OK);
		CHECK(batchRenderer.Empty());
		CHECK(NativeSortingRendererTestLastParallelBatches() > 0);
		CHECK(NativeSortingRendererTestLastFlushWorkspaceCapacityBytes() <= 24ULL * 1024ULL * 1024ULL);
		CheckPreparationContractSinks(reference, batched);
		QueuePreparationBatchFixture(serialRetryRenderer, sources[fixture], counts[fixture], fixture == 2);
		QueuePreparationBatchFixture(batchRetryRenderer, sources[fixture], counts[fixture], fixture == 2);
		referenceRetry.failCall = batchedRetry.failCall = 1;
		referenceRetry.acceptedOnFailure = batchedRetry.acceptedOnFailure = 1;
		NativeSortingRendererTestUseSingleDescriptorReference(true);
		CHECK(serialRetryRenderer.Flush(referenceRetry) == RENDER_RESULT_FAILED);
		NativeSortingRendererTestUseSingleDescriptorReference(false);
		CHECK(batchRetryRenderer.Flush(batchedRetry) == RENDER_RESULT_FAILED);
		CHECK(!serialRetryRenderer.Empty() && !batchRetryRenderer.Empty());
		referenceRetry.failCall = batchedRetry.failCall = 0;
		NativeSortingRendererTestUseSingleDescriptorReference(true);
		CHECK(serialRetryRenderer.Flush(referenceRetry) == RENDER_RESULT_OK);
		NativeSortingRendererTestUseSingleDescriptorReference(false);
		CHECK(batchRetryRenderer.Flush(batchedRetry) == RENDER_RESULT_OK);
		CHECK(serialRetryRenderer.Empty() && batchRetryRenderer.Empty());
		CheckPreparationContractSinks(referenceRetry, batchedRetry);
		std::vector<CapturedDraw> expected, actual;
		CHECK(CaptureAcceptedTriangleStream(reference, expected));
		CHECK(CaptureAcceptedTriangleStream(batchedRetry, actual));
		CHECK(SameAcceptedDrawStream(expected, actual));
	}
}

int main()
{
	// Keep this focused test within the project validation budget even when the
	// host exposes more logical processors.
	rts::JobSystem::setStartupWorkerCount(6);
	TestBoundedPreparationBatchesMatchSerialDescriptorStream();
	TestNodeOrderingAndFlushBoundary();
	TestStableNodeOrderingPreservesEqualDepthAndSplice();
	TestPerTriangleDepthOrder();
	TestMixedGeometryPreservesSortedOrder();
	TestSameSubmissionTrianglesBeforeIncompatibleSubmission();
	TestFailureAfterFirstChunkRetainsOnlyPendingGeometry();
	TestPartialDrawFailureRetryMatchesOneShotOutput();
	TestFlushLocalScratchReuseMixedSizesAndRetry();
	TestFlushWorkspaceReusePreservesRepeatedBatchBytes();
	TestStableNodeOrderingPartialAckRetryMatchesOneShotOutput();
	TestCanonicalTieRetryAndQueueExtension();
	TestMixedLayoutCohortAndClear();
	TestIndexChunkCohortQueueExtension();
	TestCapturedPassFailureAndTail();
	TestDeferredOutputIntervalAndRetry();
	TestCapturedDrawRejectsIndexOutsideDeclaredVertexRange();
	TestAdjacentSameStateSourcesCoalesceWithoutChangingTriangles();
	TestAdjacentSameStateCoalescingKeyBoundaries();
	TestAdjacentSameStateStrideDifferenceDoesNotCoalesce();
	TestUnsupportedGeometryStillFailsQueueValidation();
	TestPartialMergedDrawAcknowledgementAcrossSources();
	TestCoalescedRunsResetAcrossR16ChunksAndRetry();
	TestLinearOffsetWorkspaceAlternatingLayouts();
	CHECK(NativeSortingRendererTestRetireAllComplete());
	CHECK(NativeSortingRendererTestRetireMixedPending());
	return failures == 0 ? 0 : 1;
}
