/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2026 TheSuperHackers
** SPDX-License-Identifier: GPL-3.0-or-later
*/

#include "Utility/CppMacros.h"
#include "nativew3dsorting.h"
#include "Lib/JobSystem.h"

#include <stdio.h>
#include <string.h>
#include <vector>

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

struct CapturedBatch
{
	std::vector<unsigned int> states;
	std::vector<unsigned int> indexCounts;
	std::vector<unsigned int> startIndices;
	std::vector<unsigned int> vertexOffsets;
	std::vector<unsigned short> indices;
};

class RecordingSink : public NativeSortedGeometrySink
{
public:
	RecordingSink() : calls(0), failCall(0), acceptedOnFailure(0), batches() {}

	virtual RenderResult SubmitNativeSortedBatch(
		const NativeSortedDraw *draws, unsigned int drawCount,
		const void *, size_t, const void *indexData, size_t indexBytes,
		unsigned int *submittedDrawCount)
	{
		++calls;
		if (submittedDrawCount == 0 || draws == 0 || drawCount == 0 ||
			indexData == 0 || indexBytes % sizeof(unsigned short) != 0)
			return RENDER_RESULT_INVALID_ARGUMENT;
		CapturedBatch batch;
		for (unsigned int index = 0; index < drawCount; ++index)
		{
			batch.states.push_back(draws[index].state.pipeline.shaderBits);
			batch.indexCounts.push_back(draws[index].packet.indexCount);
			batch.startIndices.push_back(draws[index].packet.startIndex);
			batch.vertexOffsets.push_back(draws[index].packet.vertexOffset);
		}
		const unsigned short *sourceIndices =
			static_cast<const unsigned short *>(indexData);
		batch.indices.assign(sourceIndices,
			sourceIndices + indexBytes / sizeof(unsigned short));
		batches.push_back(batch);
		if (failCall != 0 && calls == failCall)
		{
			*submittedDrawCount = acceptedOnFailure < drawCount ?
				acceptedOnFailure : drawCount;
			return RENDER_RESULT_FAILED;
		}
		*submittedDrawCount = drawCount;
		return RENDER_RESULT_OK;
	}

	unsigned int calls;
	unsigned int failCall;
	unsigned int acceptedOnFailure;
	std::vector<CapturedBatch> batches;
};

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
		}
	}
	CHECK(renderer.Empty());
	CHECK(renderer.Flush(sink) == RENDER_RESULT_OK);
	CHECK(sink.calls == 1);
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

}

int main()
{
	// Keep this focused test within the project validation budget even when the
	// host exposes more logical processors.
	rts::JobSystem::setStartupWorkerCount(6);
	TestNodeOrderingAndFlushBoundary();
	TestPerTriangleDepthOrder();
	TestFailureAfterFirstChunkRetainsOnlyPendingGeometry();
	return failures == 0 ? 0 : 1;
}
