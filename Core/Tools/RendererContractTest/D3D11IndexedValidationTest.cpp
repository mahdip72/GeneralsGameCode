#include "Renderer/RendererDevice.h"
#include <stdio.h>
#include <string.h>
#include <vector>

#if defined(RTS_RENDERER_HAS_D3D11)
#include <windows.h>

namespace
{
using namespace rts::render;

int Check(bool condition, const char *message)
{
	if (!condition) fprintf(stderr, "FAIL: %s\n", message);
	return condition ? 0 : 1;
}

struct Fixture
{
	Fixture() : device(CreateD3D11RenderDevice()), context(0) {}
	~Fixture() { delete device; }

	bool Initialize()
	{
		RenderDeviceParameters parameters;
		parameters.backend = RENDER_BACKEND_D3D11;
		parameters.width = parameters.height = 64;
		parameters.enableVsync = false;
		if (device == 0 || device->initialize(parameters) != RENDER_RESULT_OK)
			return false;
		context = device->immediateContext();
		return context != 0;
	}

	bool Buffer(unsigned int binding, const void *data, size_t size,
		GpuHandle *handle, RenderUsage usage = RENDER_USAGE_DYNAMIC)
	{
		BufferDescriptor descriptor;
		descriptor.byteCount = size;
		descriptor.stride = binding == RENDER_BUFFER_VERTEX ? 16 : 2;
		descriptor.binding = binding;
		descriptor.usage = usage;
		return device->createBuffer(descriptor, data, data ? size : 0,
			handle) == RENDER_RESULT_OK;
	}

	bool Begin(GpuHandle vb, GpuHandle ib, RenderFormat format,
		unsigned int vertexOffset = 0, unsigned int indexOffset = 0)
	{
		LegacyLogicalState state;
		state.pipeline.rasterizer.cullMode = RENDER_CULL_NONE;
		return context->beginFrame() == RENDER_RESULT_OK &&
			context->setLegacyState(state, RENDER_VERTEX_POSITION3_COLOR, 0) ==
				RENDER_RESULT_OK &&
			context->setVertexBuffer(vb, 16, vertexOffset) == RENDER_RESULT_OK &&
			context->setIndexBuffer(ib, format, indexOffset) == RENDER_RESULT_OK &&
			context->setPrimitiveTopology(RENDER_PRIMITIVE_TRIANGLE_LIST) ==
				RENDER_RESULT_OK;
	}

	IRenderDevice *device;
	IRenderContext *context;
};

double Milliseconds(const LARGE_INTEGER &begin, const LARGE_INTEGER &end,
	const LARGE_INTEGER &frequency)
{
	return 1000.0 * static_cast<double>(end.QuadPart - begin.QuadPart) /
		static_cast<double>(frequency.QuadPart);
}

int Performance(Fixture &fixture)
{
	// Degenerate triangles and a headless device isolate owner-thread cost.
	// This is backend validation timing, not game/render acceptance.
	const unsigned int indexCount = 65535;
	const unsigned int rangeCount = 512;
	const unsigned int repeats = 32;
	std::vector<unsigned char> vertices(rangeCount * 2 * 16, 0);
	std::vector<unsigned short> indices(indexCount);
	for (unsigned int i = 0; i < indexCount; ++i)
		indices[i] = static_cast<unsigned short>((i % rangeCount) * 2);
	GpuHandle full, partial, fragmented, ib;
	int result = Check(fixture.Buffer(RENDER_BUFFER_VERTEX, &vertices[0],
		vertices.size(), &full) &&
		fixture.Buffer(RENDER_BUFFER_VERTEX, 0, vertices.size(), &partial) &&
		fixture.Buffer(RENDER_BUFFER_VERTEX, 0, vertices.size(), &fragmented) &&
		fixture.Buffer(RENDER_BUFFER_INDEX, &indices[0],
			indices.size() * sizeof(indices[0]), &ib), "performance buffers create");
	if (result) return result;
	result |= Check(fixture.device->updateBufferResource(partial, &vertices[0],
		vertices.size() - 16, 0, RENDER_BUFFER_UPDATE_DISCARD) == RENDER_RESULT_OK,
		"performance partial interval publishes");
	for (unsigned int range = 0; range < rangeCount; ++range)
		result |= Check(fixture.device->updateBufferResource(fragmented,
			&vertices[range * 2 * 16], 16, range * 2 * 16,
			range == 0 ? RENDER_BUFFER_UPDATE_DISCARD :
				RENDER_BUFFER_UPDATE_NO_OVERWRITE) == RENDER_RESULT_OK,
			"performance fragmented interval publishes");
	LARGE_INTEGER frequency;
	QueryPerformanceFrequency(&frequency);
	GpuHandle buffers[3] = { full, partial, fragmented };
	const char *names[3] = { "full", "partial-contiguous", "fragmented" };
	for (unsigned int kind = 0; kind < 3; ++kind)
	{
		result |= Check(fixture.Begin(buffers[kind], ib, RENDER_FORMAT_R16_UINT),
			"performance draw setup");
		LARGE_INTEGER begin, first, end;
		QueryPerformanceCounter(&begin);
		result |= Check(fixture.context->drawIndexed(indexCount, 0, 0) ==
			RENDER_RESULT_OK, "performance first draw validates");
		QueryPerformanceCounter(&first);
		for (unsigned int draw = 0; draw < repeats; ++draw)
			result |= Check(fixture.context->drawIndexed(indexCount, 0, 0) ==
				RENDER_RESULT_OK, "performance repeated draw validates");
		QueryPerformanceCounter(&end);
		double invalidatedDrawMilliseconds = 0.0;
		for (unsigned int draw = 0; draw < repeats; ++draw)
		{
			// Upload timing is deliberately outside draw timing. Each accepted
			// write advances the content version, even for identical bytes.
			result |= Check(fixture.device->updateBufferResource(ib, &indices[0],
				indices.size() * sizeof(indices[0]), 0,
				RENDER_BUFFER_UPDATE_PRESERVE) == RENDER_RESULT_OK,
				"performance content-version mutation publishes");
			LARGE_INTEGER invalidatedBegin, invalidatedEnd;
			QueryPerformanceCounter(&invalidatedBegin);
			result |= Check(fixture.context->drawIndexed(indexCount, 0, 0) ==
				RENDER_RESULT_OK, "performance invalidated draw validates");
			QueryPerformanceCounter(&invalidatedEnd);
			invalidatedDrawMilliseconds += Milliseconds(invalidatedBegin,
				invalidatedEnd, frequency);
		}
		result |= Check(fixture.context->endFrame() == RENDER_RESULT_OK,
			"performance frame ends");
		printf("INDEX_VALIDATION_PERF kind=%s indices=%u ranges=%u repeats=%u "
			"first_ms=%.6f repeat_total_ms=%.6f invalidated_total_ms=%.6f\n", names[kind], indexCount,
			kind == 2 ? rangeCount : 1, repeats,
			Milliseconds(begin, first, frequency), Milliseconds(first, end, frequency),
			invalidatedDrawMilliseconds);
	}
	return result;
}
}
#endif

int main(int argc, char **argv)
{
#if defined(RTS_RENDERER_HAS_D3D11)
	const bool performance = argc == 2 && strcmp(argv[1], "--performance") == 0;
	using namespace rts::render;
	Fixture fixture;
	int result = Check(fixture.Initialize(), "indexed validation real backend initializes");
	if (result) return result;
	if (performance) return Performance(fixture);
	unsigned char vertices[4 * 16] = { 0 };
	unsigned short indices[6] = { 4, 0, 1, 2, 3, 0 };
	unsigned int wide[6] = { 4, 1, 2, 3, 4, 0xffffffffU };
	GpuHandle vb, ib, wideIb;
	result |= Check(fixture.Buffer(RENDER_BUFFER_VERTEX, vertices, sizeof(vertices), &vb) &&
		fixture.Buffer(RENDER_BUFFER_INDEX, indices, sizeof(indices), &ib) &&
		fixture.Buffer(RENDER_BUFFER_INDEX, wide, sizeof(wide), &wideIb),
		"indexed validation full buffers create");
	if (result) return result;
	result |= Check(fixture.Begin(vb, ib, RENDER_FORMAT_R16_UINT), "R16 draw begins");
	result |= Check(fixture.context->drawIndexed(1, 0, 0) == RENDER_RESULT_INVALID_ARGUMENT,
		"fully initialized R16 buffer rejects physical overrun");
	result |= Check(fixture.context->drawIndexed(3, 1, 0) == RENDER_RESULT_OK &&
		fixture.context->drawIndexed(3, 1, 0) == RENDER_RESULT_OK,
		"R16 unchanged valid subrange repeats");
	result |= Check(fixture.context->setVertexBuffer(vb, 32, 0) == RENDER_RESULT_OK &&
		fixture.context->drawIndexed(3, 1, 0) == RENDER_RESULT_INVALID_ARGUMENT,
		"vertex stride change cannot reuse the smaller-stride proof");
	result |= Check(fixture.context->setVertexBuffer(vb, 16, 0) == RENDER_RESULT_OK,
		"R16 stride restores after rejection probe");
	result |= Check(fixture.context->drawIndexed(3, 1, -1) == RENDER_RESULT_INVALID_ARGUMENT,
		"fully initialized R16 buffer rejects negative addressed vertex");
	result |= Check(fixture.context->drawIndexed(1, 4, 1) == RENDER_RESULT_INVALID_ARGUMENT,
		"R16 signed base rejects upper overrun");
	result |= Check(fixture.context->setVertexBuffer(vb, 16, 16) == RENDER_RESULT_OK &&
		fixture.context->drawIndexed(1, 4, 0) == RENDER_RESULT_INVALID_ARGUMENT,
		"R16 vertex binding offset enters physical bounds");
	result |= Check(fixture.context->setVertexBuffer(vb, 16, 0) == RENDER_RESULT_OK &&
		fixture.context->setIndexBuffer(ib, RENDER_FORMAT_R16_UINT, 2) == RENDER_RESULT_OK &&
		fixture.context->drawIndexed(3, 0, 0) == RENDER_RESULT_OK &&
		fixture.context->drawIndexed(1, 3, 1) == RENDER_RESULT_INVALID_ARGUMENT &&
		fixture.context->drawIndexed(0, 0, 0) == RENDER_RESULT_INVALID_ARGUMENT,
		"R16 binding offset and start index select exact bytes");
	result |= Check(fixture.context->endFrame() == RENDER_RESULT_OK, "R16 frame ends");
	result |= Check(fixture.Begin(vb, wideIb, RENDER_FORMAT_R32_UINT), "R32 draw begins");
	result |= Check(fixture.context->drawIndexed(1, 0, 0) == RENDER_RESULT_INVALID_ARGUMENT &&
		fixture.context->drawIndexed(3, 1, -1) == RENDER_RESULT_OK &&
		fixture.context->drawIndexed(1, 5, 1) == RENDER_RESULT_INVALID_ARGUMENT &&
		fixture.context->setIndexBuffer(wideIb, RENDER_FORMAT_R32_UINT, 4) == RENDER_RESULT_OK &&
		fixture.context->drawIndexed(3, 0, -1) == RENDER_RESULT_OK,
		"R32 full buffers validate overrun overflow signed base and byte offset");
	result |= Check(fixture.context->endFrame() == RENDER_RESULT_OK, "R32 frame ends");
	// One byte range interpreted as R16 and R32 must have different proofs.
	unsigned short reinterpreted[2] = { 2, 3 };
	GpuHandle formatIb;
	result |= Check(fixture.Buffer(RENDER_BUFFER_INDEX, reinterpreted,
		sizeof(reinterpreted), &formatIb) &&
		fixture.Begin(vb, formatIb, RENDER_FORMAT_R16_UINT) &&
		fixture.context->drawIndexed(1, 0, 0) == RENDER_RESULT_OK &&
		fixture.context->setIndexBuffer(formatIb, RENDER_FORMAT_R32_UINT, 0) == RENDER_RESULT_OK &&
		fixture.context->drawIndexed(1, 0, 0) == RENDER_RESULT_INVALID_ARGUMENT,
		"R16 proof cannot authorize the same bytes interpreted as R32");
	result |= Check(fixture.context->endFrame() == RENDER_RESULT_OK,
		"format reinterpretation frame ends");
	std::vector<unsigned short> cacheRanges(300, 0);
	cacheRanges.back() = 4;
	GpuHandle cacheIb;
	result |= Check(fixture.Buffer(RENDER_BUFFER_INDEX, &cacheRanges[0],
		cacheRanges.size() * sizeof(cacheRanges[0]), &cacheIb) &&
		fixture.Begin(vb, cacheIb, RENDER_FORMAT_R16_UINT), "cache churn begins");
	for (unsigned int range = 0; range < 299; ++range)
		result |= Check(fixture.context->drawIndexed(1, range, 0) == RENDER_RESULT_OK,
			"more than 256 draw subranges remain valid");
	result |= Check(fixture.context->drawIndexed(1, 299, 0) == RENDER_RESULT_INVALID_ARGUMENT,
		"bounded cache churn preserves exact range rejection and evicted proof");
	result |= Check(fixture.context->drawIndexed(1, 0, 0) == RENDER_RESULT_OK &&
		fixture.context->endFrame() == RENDER_RESULT_OK,
		"churned valid proof validates again and frame ends");
	unsigned char unorderedVertices[8 * 16] = { 0 };
	unsigned short unorderedIndices[3] = { 0, 2, 6 };
	GpuHandle unorderedVb, unorderedIb;
	result |= Check(fixture.Buffer(RENDER_BUFFER_VERTEX, 0, sizeof(unorderedVertices),
		&unorderedVb) && fixture.Buffer(RENDER_BUFFER_INDEX, unorderedIndices,
			sizeof(unorderedIndices), &unorderedIb), "unordered range buffers create");
	const unsigned int publishOrder[4] = { 6, 2, 4, 0 };
	for (unsigned int range = 0; range < 4; ++range)
		result |= Check(fixture.device->updateBufferResource(unorderedVb,
			unorderedVertices, 16, publishOrder[range] * 16,
			RENDER_BUFFER_UPDATE_NO_OVERWRITE) == RENDER_RESULT_OK,
			"out-of-order interval publishes");
	result |= Check(fixture.Begin(unorderedVb, unorderedIb, RENDER_FORMAT_R16_UINT) &&
		fixture.context->drawIndexed(3, 0, 0) == RENDER_RESULT_OK,
		"binary interval lookup accepts out-of-order publications");
	unorderedIndices[1] = 3;
	result |= Check(fixture.device->updateBufferResource(unorderedIb, unorderedIndices,
		sizeof(unorderedIndices), 0, RENDER_BUFFER_UPDATE_PRESERVE) == RENDER_RESULT_OK &&
		fixture.context->drawIndexed(3, 0, 0) == RENDER_RESULT_INVALID_ARGUMENT &&
		fixture.device->updateBufferResource(unorderedVb, unorderedVertices, 5 * 16, 16,
			RENDER_BUFFER_UPDATE_PRESERVE) == RENDER_RESULT_OK &&
		fixture.context->drawIndexed(3, 0, 0) == RENDER_RESULT_OK &&
		fixture.context->endFrame() == RENDER_RESULT_OK,
		"overlapping adjacent publication merges sorted intervals exactly");

	// Seed a successful proof, then mutate the index content without rebinding.
	result |= Check(fixture.Begin(vb, ib, RENDER_FORMAT_R16_UINT) &&
		fixture.context->drawIndexed(3, 1, 0) == RENDER_RESULT_OK,
		"mutable proof seeds");
	indices[2] = 4;
	result |= Check(fixture.device->updateBufferResource(ib, indices, sizeof(indices), 0,
		RENDER_BUFFER_UPDATE_PRESERVE) == RENDER_RESULT_OK &&
		fixture.context->drawIndexed(3, 1, 0) == RENDER_RESULT_INVALID_ARGUMENT,
		"index PRESERVE invalidates successful proof");
	indices[2] = 2;
	result |= Check(fixture.device->updateBufferResource(ib, indices, sizeof(indices), 0,
		RENDER_BUFFER_UPDATE_DISCARD) == RENDER_RESULT_OK &&
		fixture.context->drawIndexed(3, 1, 0) == RENDER_RESULT_OK,
		"index DISCARD restores a valid exact proof");
	result |= Check(fixture.device->updateBufferResource(vb, vertices, 16, 0,
		RENDER_BUFFER_UPDATE_DISCARD) == RENDER_RESULT_OK &&
		fixture.context->drawIndexed(3, 1, 0) == RENDER_RESULT_INVALID_ARGUMENT,
		"vertex DISCARD invalidates successful proof and referenced holes");
	result |= Check(fixture.device->updateBufferResource(vb, vertices + 2 * 16, 16, 2 * 16,
		RENDER_BUFFER_UPDATE_NO_OVERWRITE) == RENDER_RESULT_OK,
		"second disjoint vertex interval publishes");
	indices[1] = indices[2] = 0; indices[3] = 2;
	result |= Check(fixture.device->updateBufferResource(ib, indices, sizeof(indices), 0,
		RENDER_BUFFER_UPDATE_PRESERVE) == RENDER_RESULT_OK &&
		fixture.context->drawIndexed(3, 1, 0) == RENDER_RESULT_OK &&
		fixture.context->drawIndexed(3, 1, 0) == RENDER_RESULT_OK,
		"fragmented proof accepts repeated references across unused holes");
	indices[2] = 1;
	result |= Check(fixture.device->updateBufferResource(ib, indices, sizeof(indices), 0,
		RENDER_BUFFER_UPDATE_NO_OVERWRITE) == RENDER_RESULT_OK &&
		fixture.context->drawIndexed(3, 1, 0) == RENDER_RESULT_INVALID_ARGUMENT &&
		fixture.device->updateBufferResource(vb, vertices + 16, 16, 16,
			RENDER_BUFFER_UPDATE_PRESERVE) == RENDER_RESULT_OK &&
		fixture.context->drawIndexed(3, 1, 0) == RENDER_RESULT_OK,
		"index NO_OVERWRITE invalidates and filling vertex hole permits draw");
	// A rejected upload does not alter bytes or make an invalid proof valid.
	result |= Check(fixture.device->updateBufferResource(ib, indices, sizeof(indices), 1,
		RENDER_BUFFER_UPDATE_DISCARD) == RENDER_RESULT_INVALID_ARGUMENT &&
		fixture.context->drawIndexed(3, 1, 0) == RENDER_RESULT_OK,
		"rejected update preserves validated content");
	result |= Check(fixture.context->endFrame() == RENDER_RESULT_OK &&
		fixture.device->recoverDevice() == RENDER_RESULT_OK,
		"proof-bearing buffers recover");
	result |= Check(fixture.device->updateBufferResource(vb, vertices, 16, 0,
		RENDER_BUFFER_UPDATE_PRESERVE) == RENDER_RESULT_OK &&
		fixture.device->updateBufferResource(ib, indices, sizeof(indices), 0,
			RENDER_BUFFER_UPDATE_PRESERVE) == RENDER_RESULT_OK &&
		fixture.Begin(vb, ib, RENDER_FORMAT_R16_UINT) &&
		fixture.context->drawIndexed(3, 1, 0) == RENDER_RESULT_INVALID_ARGUMENT,
		"recovery cannot reuse proof for unpublished vertex ranges");
	result |= Check(fixture.context->endFrame() == RENDER_RESULT_OK &&
		fixture.device->destroyResource(vb), "original vertex resource releases");
	GpuHandle replacement;
	result |= Check(fixture.Buffer(RENDER_BUFFER_VERTEX, vertices, 16, &replacement) &&
		replacement.index() == vb.index() && replacement.generation() != vb.generation() &&
		fixture.Begin(replacement, ib, RENDER_FORMAT_R16_UINT) &&
		fixture.context->drawIndexed(3, 1, 0) == RENDER_RESULT_INVALID_ARGUMENT,
		"new vertex generation cannot reuse old proof");
	result |= Check(fixture.context->endFrame() == RENDER_RESULT_OK,
		"replacement generation frame ends");
	GpuHandle immutableVb, immutableIb;
	unsigned short immutableIndices[3] = { 0, 1, 2 };
	result |= Check(fixture.Buffer(RENDER_BUFFER_VERTEX, vertices, sizeof(vertices),
		&immutableVb, RENDER_USAGE_IMMUTABLE) &&
		fixture.Buffer(RENDER_BUFFER_INDEX, immutableIndices, sizeof(immutableIndices),
			&immutableIb, RENDER_USAGE_IMMUTABLE) &&
		fixture.Begin(immutableVb, immutableIb, RENDER_FORMAT_R16_UINT) &&
		fixture.context->drawIndexed(3, 0, 0) == RENDER_RESULT_OK &&
		fixture.context->endFrame() == RENDER_RESULT_OK &&
		fixture.device->recoverDevice() == RENDER_RESULT_OK &&
		fixture.Begin(immutableVb, immutableIb, RENDER_FORMAT_R16_UINT) &&
		fixture.context->drawIndexed(3, 0, 0) == RENDER_RESULT_OK &&
		fixture.context->drawIndexed(3, 0, 2) == RENDER_RESULT_INVALID_ARGUMENT &&
		fixture.context->endFrame() == RENDER_RESULT_OK,
		"immutable recovery rebuilds physical bounds proofs from retained index source");
	if (result == 0) printf("D3D11 indexed validation tests passed.\n");
	return result;
#else
	(void)argc; (void)argv;
	fprintf(stderr, "D3D11 backend is required for indexed validation tests.\n");
	return 1;
#endif
}
