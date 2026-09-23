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

bool NativeSortingRendererTestRetireAllComplete();
bool NativeSortingRendererTestRetireMixedPending();

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
			batch.states.size() != batch.vertexStrides.size())
			return false;

		for (unsigned int drawIndex = 0;
			drawIndex < batch.acceptedDrawCount; ++drawIndex)
		{
			const unsigned int startIndex = batch.startIndices[drawIndex];
			const unsigned int indexCount = batch.indexCounts[drawIndex];
			const unsigned int vertexOffset = batch.vertexOffsets[drawIndex];
			const unsigned int stride = batch.vertexStrides[drawIndex];
			if (stride == 0 || startIndex > batch.indices.size() ||
				indexCount > batch.indices.size() - startIndex)
				return false;

			CapturedDraw captured;
			captured.state = batch.states[drawIndex];
			for (unsigned int index = 0; index < indexCount; ++index)
			{
				const unsigned short vertexIndex =
					batch.indices[startIndex + index];
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

bool SameAcceptedDrawStream(const std::vector<CapturedDraw> &left,
	const std::vector<CapturedDraw> &right)
{
	if (left.size() != right.size())
		return false;
	for (size_t index = 0; index < left.size(); ++index)
	{
		if (left[index].state != right[index].state ||
			left[index].indices != right[index].indices ||
			left[index].referencedVertices != right[index].referencedVertices)
			return false;
	}
	return true;
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

}

int main()
{
	// Keep this focused test within the project validation budget even when the
	// host exposes more logical processors.
	rts::JobSystem::setStartupWorkerCount(6);
	TestNodeOrderingAndFlushBoundary();
	TestPerTriangleDepthOrder();
	TestMixedGeometryPreservesSortedOrder();
	TestFailureAfterFirstChunkRetainsOnlyPendingGeometry();
	TestPartialDrawFailureRetryMatchesOneShotOutput();
	CHECK(NativeSortingRendererTestRetireAllComplete());
	CHECK(NativeSortingRendererTestRetireMixedPending());
	return failures == 0 ? 0 : 1;
}
