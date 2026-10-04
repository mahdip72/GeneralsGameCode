// Supplementary exact-method evidence. Real D3D pixels and lifecycle behavior
// are tested separately in D3D11IndexedValidationTest.cpp; these recording
// doubles expose private CPU mirror bytes and failure boundaries, not a GPU.
#include "Renderer/RendererDevice.h"
#include "Lib/FrameTimingDiagnostics.h"
#include "../../Libraries/Source/Renderer/D3D11ResultTranslation.h"
#include "../../Libraries/Source/Renderer/IndexedDrawValidationCache.h"
#include <d3d11.h>
#include <algorithm>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <vector>

namespace
{
using namespace rts::render;
int Check(bool condition, const char *message)
{
	if (!condition) fprintf(stderr, "FAIL: %s\n", message);
	return condition ? 0 : 1;
}
#include "D3D11BufferRangeMethods.inc"
#include "D3D11BufferResourceSlot.inc"

struct RecordingContext
{
	RecordingContext() : failMap(false), nullMap(false), mapCalls(0), unmapCalls(0), gpu(64, 0xcc) {}
	HRESULT Map(ID3D11Resource *, UINT, D3D11_MAP, UINT, D3D11_MAPPED_SUBRESOURCE *mapped)
	{
		++mapCalls;
		if (failMap) return E_FAIL;
		std::fill(gpu.begin(), gpu.end(), 0xcc);
		mapped->pData = nullMap ? 0 : &gpu[0];
		return S_OK;
	}
	void Unmap(ID3D11Resource *, UINT) { ++unmapCalls; }
	void UpdateSubresource(ID3D11Resource *, UINT, const D3D11_BOX *box,
		const void *data, UINT, UINT)
	{ memcpy(&gpu[box->left], data, box->right - box->left); }
	bool failMap, nullMap;
	unsigned int mapCalls, unmapCalls;
	std::vector<unsigned char> gpu;
};
class Fixture
{
public:
	Fixture(unsigned int binding) : handles(2), m_handles(&handles), m_resources(2), m_context(&context)
	{
		handle = handles.allocate();
		ResourceSlot &slot = m_resources[handle.index()];
		slot.kind = RESOURCE_BUFFER; slot.usage = RENDER_USAGE_DYNAMIC;
		slot.binding = binding; slot.byteCount = 64;
		slot.bufferImage.assign(64, 0xa7); slot.bufferContentValid = true;
		slot.bufferContentVersion = 7;
		slot.initializedBufferRanges.push_back(BufferByteRange{ 0, 64 });
	}
	bool isOwner() const { return true; }
	RenderResult TranslateResult(HRESULT result) { return detail::TranslateD3D11Result(result); }
#include "D3D11BufferAdvanceVersion.inc"
#include "D3D11BufferUpdateResource.inc"
	ResourceSlot& slot() { return m_resources[handle.index()]; }
	RecordingContext context;
	GpuHandle handle;
	GpuHandleAllocator handles;
	GpuHandleAllocator *m_handles;
	std::vector<ResourceSlot> m_resources;
	RecordingContext *m_context;
	detail::IndexedDrawValidationCache m_indexedDrawValidation;
	detail::IndexRangeSummaryCache m_indexRangeSummaries;
};

int EquivalentBytes(unsigned int binding, size_t prefix)
{
	Fixture fixture(binding);
	unsigned char data[64];
	for (unsigned int index = 0; index < sizeof(data); ++index) data[index] = static_cast<unsigned char>(index + 1);
	// Independent pre-optimization oracle: clear the entire prior CPU image,
	// then overwrite the uploaded prefix. Compare every byte, including holes.
	std::vector<unsigned char> expected(64, 0);
	memcpy(&expected[0], data, prefix);
	int result = Check(fixture.updateBufferResource(fixture.handle, data, prefix, 0,
		RENDER_BUFFER_UPDATE_DISCARD) == RENDER_RESULT_OK && fixture.slot().bufferImage == expected,
		"production DISCARD yields byte-identical legacy full-clear mirror");
	result |= Check(fixture.slot().bufferContentVersion == 8 &&
		IsBufferRangeInitialized(fixture.slot().initializedBufferRanges, 0, prefix) &&
		(prefix == 64 || !IsBufferRangeInitialized(fixture.slot().initializedBufferRanges, prefix, 64)),
		"production DISCARD advances version and initializes only prefix");
	unsigned char replacement[3] = { 0xe1, 0xe2, 0xe3 };
	memcpy(&expected[11], replacement, sizeof(replacement));
	result |= Check(fixture.updateBufferResource(fixture.handle, replacement, sizeof(replacement), 11,
		RENDER_BUFFER_UPDATE_PRESERVE) == RENDER_RESULT_OK && fixture.slot().bufferImage == expected &&
		fixture.context.gpu == expected,
		"production PRESERVE republishes exact prefix, zero tail and changed range");
	const unsigned int version = fixture.slot().bufferContentVersion;
	const unsigned int maps = fixture.context.mapCalls;
	result |= Check(fixture.updateBufferResource(fixture.handle, data, 1, 1,
		RENDER_BUFFER_UPDATE_DISCARD) == RENDER_RESULT_INVALID_ARGUMENT &&
		fixture.slot().bufferImage == expected && fixture.slot().bufferContentVersion == version &&
		fixture.context.mapCalls == maps,
		"rejected offset leaves image/version unchanged without Map");
	return result;
}
int FailedMapLeavesMirror()
{
	Fixture fixture(RENDER_BUFFER_INDEX);
	const std::vector<unsigned char> original = fixture.slot().bufferImage;
	unsigned char bytes[3] = { 1, 2, 3 };
	fixture.context.failMap = true;
	int result = Check(fixture.updateBufferResource(fixture.handle, bytes, sizeof(bytes), 0,
		RENDER_BUFFER_UPDATE_DISCARD) == RENDER_RESULT_FAILED && fixture.slot().bufferImage == original &&
		fixture.slot().bufferContentVersion == 8 && fixture.context.unmapCalls == 0 &&
		IsBufferRangeInitialized(fixture.slot().initializedBufferRanges, 0, 64),
		"failed recorded Map preserves prior mirror/ranges but advances proof version");
	fixture.context.failMap = false; fixture.context.nullMap = true;
	result |= Check(fixture.updateBufferResource(fixture.handle, bytes, sizeof(bytes), 0,
		RENDER_BUFFER_UPDATE_DISCARD) == RENDER_RESULT_FAILED && fixture.slot().bufferImage == original &&
		fixture.slot().bufferContentVersion == 9 && !fixture.slot().bufferContentValid &&
		fixture.slot().initializedBufferRanges.empty() && fixture.context.unmapCalls == 1,
		"null recorded mapping revokes initialization without changing prior mirror");
	return result;
}
}
int main()
{
	SetEnvironmentVariableA("RTS_FRAME_TIMING_DIR", NULL);
	int result = FailedMapLeavesMirror();
	const size_t prefixes[] = { 1, 31, 64 };
	for (unsigned int binding = 0; binding < 2; ++binding)
		for (unsigned int prefix = 0; prefix < sizeof(prefixes) / sizeof(prefixes[0]); ++prefix)
			result |= EquivalentBytes(binding ? RENDER_BUFFER_INDEX : RENDER_BUFFER_VERTEX, prefixes[prefix]);
	if (!result) printf("Supplementary exact-method DISCARD contract passed.\n");
	return result;
}
