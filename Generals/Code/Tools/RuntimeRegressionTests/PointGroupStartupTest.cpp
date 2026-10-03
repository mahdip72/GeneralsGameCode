#include "pointgr.h"
#include "nativew3dbufferowner.h"
#include "Renderer/RenderGameClient.h"
#include "Renderer/LegacyColorPacking.h"
#include "Renderer/PointGroupColorPacking.h"
#include "dx8vertexbuffer.h"
#include "WW3D2/vertmaterial.h"
#include "WWMath/matrix4.h"
#include "WWMath/wwmath.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace
{
using namespace rts::render;

int Check(bool condition, const char *message)
{
	if (condition)
	{
		return 0;
	}
	std::fprintf(stderr, "FAILED: %s\n", message);
	return 1;
}

int CheckPackedQuadColors()
{
	const int verticesPerChunk = 2048;
	const int vertexCount = verticesPerChunk + 8;
	for (int mode = 0; mode != 2; ++mode)
	{
		const int verticesPerPoint = mode == 0 ? 4 : 3;
		const int pointCount =
			(vertexCount + verticesPerPoint - 1) / verticesPerPoint;
		std::vector<Vector4> colors(pointCount);
		for (int i = 0; i < pointCount; ++i)
		{
			colors[i] = Vector4((i % 7) / 6.0f, (i % 11) / 10.0f,
				(i % 13) / 12.0f, (i % 17) / 16.0f);
		}
		int cachedPointIndex = -1;
		unsigned int packed = 0;
		for (int i = 0; i < vertexCount; ++i)
		{
			if (i == verticesPerChunk)
			{
				cachedPointIndex = -1;
				packed = 0;
			}
			const Vector4 &color = colors[i / verticesPerPoint];
			const unsigned int expected = PackLegacyARGB(color.X, color.Y,
				color.Z, color.W);
			const bool repacked = rts::render::PackPointGroupColorForVertex(
				&colors[0], i, verticesPerPoint, cachedPointIndex, packed);
			const bool expectedRepack = i == 0 || i == verticesPerChunk ||
				i % verticesPerPoint == 0;
			if (packed != expected || repacked != expectedRepack)
				return Check(false, mode == 0 ?
					"quad RGBA packing preserves colors across vertex chunks" :
					"triangle RGBA packing preserves colors across vertex chunks");
		}
	}
	return 0;
}

class BillboardPointGroup : public PointGroupClass
{
public:
	using PointGroupClass::Can_Pack_Billboard_Quads;
	using PointGroupClass::Pack_Billboard_Quad_Chunk;
	using PointGroupClass::Pack_Vertex_Chunk;
	using PointGroupClass::Prepare_Vertex_Arrays;

	void Reference(const Matrix4x4 &view, unsigned char *vertices,
		const FVFInfoClass &fvf, int first, int count)
	{
		int vertexCount = 0, polygonCount = 0;
		Prepare_Vertex_Arrays(view, PointLoc->Get_Array(), PointSize->Get_Array(),
			PointOrientation->Get_Array(), nullptr, vertexCount, polygonCount);
		Pack_Vertex_Chunk(vertices, fvf,
			PointDiffuse ? PointDiffuse->Get_Array() : nullptr, first, count);
	}
};

struct BillboardInput
{
	explicit BillboardInput(int count)
		: locations(new ShareBufferClass<Vector3>(count, "billboard locations")),
		colors(new ShareBufferClass<Vector4>(count, "billboard colors")),
		sizes(new ShareBufferClass<float>(count, "billboard sizes")),
		orientations(new ShareBufferClass<unsigned char>(count, "billboard orientations")),
		frames(new ShareBufferClass<unsigned char>(count, "billboard frames")),
		apt(new ShareBufferClass<unsigned int>(count, "billboard APT"))
	{
		const float testSizes[] = {0.0f, -0.0f, 0.0001f, 0.5f, 8.25f, -2.0f, 1024.0f};
		for (int i = 0; i < count; ++i) {
			locations->Get_Array()[i] = Vector3((i % 11) * 0.125f - 4.0f,
				(i % 17) * -1.25f, (i % 23) * 3.125f - 50.0f);
			colors->Get_Array()[i] = Vector4((i % 7) / 6.0f, (i % 11) / 10.0f,
				(i % 13) / 12.0f, (i % 17) / 16.0f);
			sizes->Get_Array()[i] = testSizes[i % 7];
			orientations->Get_Array()[i] = static_cast<unsigned char>(i);
			frames->Get_Array()[i] = 0;
			apt->Get_Array()[i] = static_cast<unsigned int>(i);
		}
	}
	~BillboardInput()
	{
		apt->Release_Ref(); frames->Release_Ref(); orientations->Release_Ref();
		sizes->Release_Ref(); colors->Release_Ref(); locations->Release_Ref();
	}
	void Set(BillboardPointGroup &group, int active, bool useColors = true)
	{
		group.Set_Arrays(locations, useColors ? colors : nullptr, nullptr,
			sizes, orientations, nullptr, active);
		group.Set_Point_Mode(PointGroupClass::QUADS);
		group.Set_Billboard(true);
		group.Set_Flag(PointGroupClass::TRANSFORM, true);
		group.Set_Frame_Row_Column_Count_Log2(0);
		group.Set_Point_Frame(0);
		group.Set_Point_Color(Vector3(0.125f, 0.625f, 0.875f));
		group.Set_Point_Alpha(0.375f);
	}
	ShareBufferClass<Vector3> *locations;
	ShareBufferClass<Vector4> *colors;
	ShareBufferClass<float> *sizes;
	ShareBufferClass<unsigned char> *orientations;
	ShareBufferClass<unsigned char> *frames;
	ShareBufferClass<unsigned int> *apt;
};

int CheckBillboardPackingParity()
{
	int result = 0;
	BillboardInput input(513);
	BillboardPointGroup group;
	const FVFInfoClass fvf(dynamic_fvf_type);
	Matrix4x4 views[3];
	views[0].Make_Identity();
	views[1].Init(0.0f, -1.0f, 0.0f, 12.25f,
		1.0f, 0.0f, 0.0f, -7.5f, 0.0f, 0.0f, 1.0f, 0.125f,
		0.0f, 0.0f, 0.0f, 1.0f);
	views[2].Init(0.913f, -0.217f, 0.341f, -135.5f,
		0.279f, 0.956f, -0.094f, 11.25f, -0.294f, 0.181f, 0.938f, 1024.125f,
		0.0f, 0.0f, 0.0f, 1.0f);
	const int pointCounts[] = {1, 255, 256, 511, 512, 513};
	const ShaderClass shaders[] = {ShaderClass::_PresetAdditiveSpriteShader,
		ShaderClass::_PresetAlphaSpriteShader, ShaderClass::_PresetATestSpriteShader,
		ShaderClass::_PresetMultiplicativeSpriteShader};
	const float sizeCases[] = {0.0f, -0.0f, 0.0001f, 0.5f, 8.25f, -2.0f, 1024.0f};
	for (unsigned size = 0; size < sizeof(sizeCases) / sizeof(sizeCases[0]); ++size) {
		for (int point = 0; point < 513; ++point)
			input.sizes->Get_Array()[point] = sizeCases[size];
		for (int matrix = 0; matrix < 3; ++matrix) {
			for (int colors = 0; colors < 2; ++colors) {
				for (int shader = 0; shader < 4; ++shader) {
					group.Set_Shader(shaders[shader]);
					for (unsigned test = 0; test < sizeof(pointCounts) / sizeof(pointCounts[0]); ++test) {
						const int points = pointCounts[test];
						input.Set(group, points, colors != 0);
						const size_t bytes = points * 4 * fvf.Get_FVF_Size();
						std::vector<unsigned char> expected(bytes + 32, 0xa5);
						std::vector<unsigned char> actual(bytes + 32, 0xa5);
						for (int first = 0; first < points * 4; first += 2048) {
							const int remaining = points * 4 - first;
							const int count = remaining < 2048 ? remaining : 2048;
							const size_t offset = 16 + first * fvf.Get_FVF_Size();
							group.Reference(views[matrix], &expected[offset], fvf, first, count);
							result |= Check(group.Pack_Billboard_Quad_Chunk(&actual[offset],
								fvf, views[matrix], first, count), "actual fused billboard chunk accepts eligible data");
						}
						result |= Check(expected == actual,
							"fused stream matches production transform, Update_Arrays and generic pack bytes plus guards");
					}
				}
			}
		}
	}
	return result;
}

int CheckBillboardPackingExclusions()
{
	int result = 0;
	BillboardInput input(4);
	BillboardPointGroup group;
	const FVFInfoClass fvf(dynamic_fvf_type);
	const Matrix4x4 view(true);
	std::vector<unsigned char> untouched(4 * 4 * fvf.Get_FVF_Size(), 0xa5);
	std::vector<unsigned char> output = untouched;
	input.Set(group, 4);
	result |= Check(group.Can_Pack_Billboard_Quads(dynamic_fvf_type, fvf.Get_FVF_Size()),
		"normal native billboard route is eligible");
	result |= Check(!group.Can_Pack_Billboard_Quads(DX8_FVF_XYZ, fvf.Get_FVF_Size()) &&
		!group.Can_Pack_Billboard_Quads(dynamic_fvf_type, fvf.Get_FVF_Size() - 4),
		"other vertex formats and strides retain generic packing");
	for (int test = 0; test < 9; ++test) {
		input.Set(group, 4);
		switch (test) {
			case 0: group.Set_Point_Mode(PointGroupClass::TRIS); break;
			case 1: group.Set_Billboard(false); break;
			case 2: group.Set_Flag(PointGroupClass::TRANSFORM, false); break;
			case 3: group.Set_Arrays(input.locations, input.colors, input.apt,
				input.sizes, input.orientations, nullptr, 4); break;
			case 4: group.Set_Arrays(input.locations, input.colors, nullptr,
				input.sizes, input.orientations, input.frames, 4); break;
			case 5: group.Set_Arrays(input.locations, input.colors, nullptr,
				nullptr, input.orientations, nullptr, 4); break;
			case 6: group.Set_Arrays(input.locations, input.colors, nullptr,
				input.sizes, nullptr, nullptr, 4); break;
			case 7: group.Set_Frame_Row_Column_Count_Log2(1); break;
			case 8: group.Set_Point_Mode(PointGroupClass::SCREENSPACE); break;
		}
		result |= Check(!group.Can_Pack_Billboard_Quads(dynamic_fvf_type, fvf.Get_FVF_Size()) &&
			!group.Pack_Billboard_Quad_Chunk(output.data(), fvf, view, 0, 16) && output == untouched,
			"excluded geometry rejects fused packing before writing");
	}
	input.Set(group, 4);
	group.Set_Point_Frame(1);
	result |= Check(!group.Pack_Billboard_Quad_Chunk(output.data(), fvf, view, 0, 16),
		"nondefault frames retain generic table handling");
	input.Set(group, 0);
	result |= Check(!group.Pack_Billboard_Quad_Chunk(output.data(), fvf, view, 0, 0),
		"empty groups retain Render's early return");
	input.Set(group, 4);
	result |= Check(!group.Pack_Billboard_Quad_Chunk(nullptr, fvf, view, 0, 16) &&
		!group.Pack_Billboard_Quad_Chunk(output.data(), fvf, view, -4, 4) &&
		!group.Pack_Billboard_Quad_Chunk(output.data(), fvf, view, 0, -4) &&
		!group.Pack_Billboard_Quad_Chunk(output.data(), fvf, view, 1, 4) &&
		!group.Pack_Billboard_Quad_Chunk(output.data(), fvf, view, 0, 3) &&
		!group.Pack_Billboard_Quad_Chunk(output.data(), fvf, view, 12, 8) && output == untouched,
		"invalid chunk boundaries and null output reject without writes");
	return result;
}

class PointGroupDevice;

class PointGroupContext : public IRenderContext
{
public:
	explicit PointGroupContext(PointGroupDevice *device) : m_device(device) {}
	RenderResult beginFrame() override { return RENDER_RESULT_OK; }
	RenderResult updateBuffer(GpuHandle, const void *, size_t, size_t,
		RenderBufferUpdateMode) override { return RENDER_RESULT_UNSUPPORTED; }
	RenderResult clear(const RenderFloat4 &, float, unsigned int) override
		{ return RENDER_RESULT_UNSUPPORTED; }
	RenderResult clearTargets(unsigned int, const RenderFloat4 &, float,
		unsigned int) override { return RENDER_RESULT_UNSUPPORTED; }
	RenderResult setRenderTargets(const RenderTargetBinding &) override
		{ return RENDER_RESULT_UNSUPPORTED; }
	RenderResult setRenderTargets(GpuHandle, GpuHandle) override
		{ return RENDER_RESULT_UNSUPPORTED; }
	RenderResult setViewport(float, float, float, float, float, float) override
		{ return RENDER_RESULT_UNSUPPORTED; }
	RenderResult setLegacyState(const LegacyLogicalState &,
		LegacyVertexFormat, unsigned int) override
		{ return RENDER_RESULT_UNSUPPORTED; }
	RenderResult setLegacyStateForLayout(const LegacyLogicalState &,
		const LegacyVertexLayout &, unsigned int) override
		{ return RENDER_RESULT_UNSUPPORTED; }
	RenderResult setVertexBuffer(GpuHandle, unsigned int, unsigned int) override
		{ return RENDER_RESULT_UNSUPPORTED; }
	RenderResult setIndexBuffer(GpuHandle, RenderFormat, unsigned int) override
		{ return RENDER_RESULT_UNSUPPORTED; }
	RenderResult setTexture(unsigned int, GpuHandle) override
		{ return RENDER_RESULT_UNSUPPORTED; }
	RenderResult setPrimitiveTopology(RenderPrimitiveTopology) override
		{ return RENDER_RESULT_UNSUPPORTED; }
	RenderResult draw(unsigned int, unsigned int) override
		{ return RENDER_RESULT_UNSUPPORTED; }
	RenderResult drawIndexed(unsigned int, unsigned int, int) override
		{ return RENDER_RESULT_UNSUPPORTED; }
	RenderResult endFrame() override { return RENDER_RESULT_OK; }
private:
	PointGroupDevice *m_device;
};

class PointGroupDevice : public IRenderDevice
{
public:
	struct Buffer
	{
		Buffer() : live(false), handle(), byteCount(0) {}
		bool live;
		GpuHandle handle;
		size_t byteCount;
	};

	PointGroupDevice() : m_allocator(8), m_context(this), m_operational(true),
		m_createInvocation(0), m_updateInvocation(0), m_failCreateOn(0),
		m_failUpdateOn(0), m_buffers(8) {}

	RenderBackend backend() const override { return RENDER_BACKEND_D3D11; }
	bool isOperational() const override { return m_operational; }
	RenderResult initialize(const RenderDeviceParameters &) override
		{ return RENDER_RESULT_INVALID_ARGUMENT; }
	void shutdown() override { m_operational = false; }
	IRenderContext *immediateContext() override { return &m_context; }
	RenderResult createBuffer(const BufferDescriptor &descriptor,
		const void *initialData, size_t initialDataBytes, GpuHandle *buffer) override
	{
		if (buffer == nullptr)
		{
			return RENDER_RESULT_INVALID_ARGUMENT;
		}
		*buffer = GpuHandle();
		++m_createInvocation;
		if (m_failCreateOn != 0 && m_createInvocation == m_failCreateOn)
		{
			return RENDER_RESULT_OUT_OF_MEMORY;
		}
		if (!m_operational || descriptor.byteCount == 0 ||
			(initialData == nullptr && initialDataBytes != 0) ||
			(initialData != nullptr && initialDataBytes != descriptor.byteCount))
		{
			return RENDER_RESULT_INVALID_ARGUMENT;
		}
		const GpuHandle created = m_allocator.allocate();
		if (!created.isValid() || created.index() >= m_buffers.size())
		{
			return RENDER_RESULT_OUT_OF_MEMORY;
		}
		Buffer &slot = m_buffers[created.index()];
		slot.live = true;
		slot.handle = created;
		slot.byteCount = descriptor.byteCount;
		*buffer = created;
		return RENDER_RESULT_OK;
	}
	RenderResult createTexture(const TextureDescriptor &,
		const TextureSubresourceData *, unsigned int, GpuHandle *) override
		{ return RENDER_RESULT_UNSUPPORTED; }
	RenderResult refreshTexture(GpuHandle, const TextureDescriptor &,
		const TextureSubresourceData *, unsigned int) override
		{ return RENDER_RESULT_UNSUPPORTED; }
	RenderResult copyActiveColorTargetToTexture(GpuHandle) override
		{ return RENDER_RESULT_UNSUPPORTED; }
	bool destroyResource(GpuHandle resource) override
	{
		Buffer *slot = Find(resource);
		if (slot == nullptr || !m_allocator.release(resource))
		{
			return false;
		}
		slot->live = false;
		return true;
	}
	RenderResult recoverDevice() override { return RENDER_RESULT_OK; }
	RenderResult resize(unsigned int, unsigned int) override
		{ return RENDER_RESULT_OK; }
	RenderResult present() override { return RENDER_RESULT_OK; }
	RenderResult getBackBufferInfo(RenderBackBufferInfo *) const override
		{ return RENDER_RESULT_UNSUPPORTED; }
	RenderResult captureBackBuffer(void *, size_t, size_t,
		RenderFormat *) override { return RENDER_RESULT_UNSUPPORTED; }
	RenderResult getDebugValidationErrorCount(unsigned int *count) const override
	{
		if (count == nullptr) return RENDER_RESULT_INVALID_ARGUMENT;
		*count = 0;
		return RENDER_RESULT_OK;
	}
	RenderResult reportDebugLiveObjects() override { return RENDER_RESULT_OK; }
	RenderResult updateBufferResource(GpuHandle resource, const void *data,
		size_t byteCount, size_t destinationOffset,
		RenderBufferUpdateMode) override
	{
		++m_updateInvocation;
		if (m_failUpdateOn != 0 && m_updateInvocation == m_failUpdateOn)
		{
			return RENDER_RESULT_FAILED;
		}
		Buffer *slot = Find(resource);
		return slot != nullptr && data != nullptr && byteCount != 0 &&
			destinationOffset <= slot->byteCount &&
			byteCount <= slot->byteCount - destinationOffset ?
			RENDER_RESULT_OK : RENDER_RESULT_INVALID_ARGUMENT;
	}

	void FailCreateOn(unsigned int invocation)
		{ m_failCreateOn = invocation; }
	void FailUpdateOn(unsigned int invocation)
		{ m_failUpdateOn = invocation; }
	unsigned int LiveCount() const { return m_allocator.liveCount(); }
	unsigned int CreateInvocations() const { return m_createInvocation; }
	unsigned int UpdateInvocations() const { return m_updateInvocation; }

private:
	Buffer *Find(GpuHandle handle)
	{
		if (!handle.isValid() || handle.index() >= m_buffers.size()) return nullptr;
		Buffer &slot = m_buffers[handle.index()];
		return slot.live && slot.handle == handle ? &slot : nullptr;
	}

	GpuHandleAllocator m_allocator;
	PointGroupContext m_context;
	bool m_operational;
	unsigned int m_createInvocation;
	unsigned int m_updateInvocation;
	unsigned int m_failCreateOn;
	unsigned int m_failUpdateOn;
	std::vector<Buffer> m_buffers;
};

int RunFailureCase(unsigned int failCreateOn, unsigned int failUpdateOn,
	unsigned int expectedCreates, unsigned int expectedUpdates,
	const char *message)
{
	int result = 0;
	PointGroupDevice device;
	device.FailCreateOn(failCreateOn);
	device.FailUpdateOn(failUpdateOn);
	NativeW3DResourceHost host(8);
	NativeW3DResources resources(8);
	result |= Check(host.Attach(&device, device.immediateContext()) ==
		RENDER_RESULT_OK && resources.BindHost(&host) == RENDER_RESULT_OK &&
		BindNativeW3DBufferResources(&resources) == RENDER_RESULT_OK,
		"PointGroup production failure fixture binds the native registry");
	result |= Check(!PointGroupClass::_Init() &&
		device.CreateInvocations() == expectedCreates &&
		device.UpdateInvocations() == expectedUpdates &&
		device.LiveCount() == 0, message);
	PointGroupClass::_Shutdown();
	result |= Check(UnbindNativeW3DBufferResources(&resources) ==
		RENDER_RESULT_OK && resources.Shutdown() == RENDER_RESULT_OK &&
		host.Detach() == RENDER_RESULT_OK && device.LiveCount() == 0,
		"PointGroup production failure fixture shuts down without live handles");
	return result;
}

int RunPackingCase(unsigned int failCreateOn, unsigned int failUpdateOn)
{
	int result = 0;
	PointGroupDevice device;
	NativeW3DResourceHost host(8);
	NativeW3DResources resources(8);
	result |= Check(host.Attach(&device, device.immediateContext()) == RENDER_RESULT_OK &&
		resources.BindHost(&host) == RENDER_RESULT_OK &&
		BindNativeW3DBufferResources(&resources) == RENDER_RESULT_OK,
		"billboard production fixture binds owner resources");
	const bool initialized = PointGroupClass::_Init();
	result |= Check(initialized, "billboard parity uses actual production startup tables");
	if (initialized) {
		if (failCreateOn == 0 && failUpdateOn == 0) {
			result |= CheckBillboardPackingParity();
			result |= CheckBillboardPackingExclusions();
		} else {
			device.FailCreateOn(failCreateOn);
			device.FailUpdateOn(failUpdateOn);
			BillboardInput input(1);
			BillboardPointGroup group;
			input.Set(group, 1);
			{
				DynamicVBAccessClass vertices(BUFFER_TYPE_DYNAMIC_DX8, dynamic_fvf_type, 4);
				if (failCreateOn != 0) {
					result |= Check(!vertices.Is_Valid(),
						"failed production dynamic VB creation is rejected before fused writes");
				} else {
					result |= Check(vertices.Is_Valid(), "production dynamic VB acquired for failed commit");
					if (vertices.Is_Valid()) {
						DynamicVBAccessClass::WriteLockClass lock(&vertices);
						result |= Check(lock.Is_Locked(), "production billboard write lock acquired");
						if (lock.Is_Locked()) {
							const Matrix4x4 view(true);
							result |= Check(group.Pack_Billboard_Quad_Chunk(
								reinterpret_cast<unsigned char *>(lock.Get_Formatted_Vertex_Array()),
								vertices.FVF_Info(), view, 0, 4), "fused bytes reach production dynamic lock");
							result |= Check(!lock.Commit(),
								"failed production dynamic upload is not acknowledged by commit");
						}
					}
				}
			}
			DynamicVBAccessClass::_Deinit();
		}
	}
	PointGroupClass::_Shutdown();
	result |= Check(UnbindNativeW3DBufferResources(&resources) == RENDER_RESULT_OK &&
		resources.Shutdown() == RENDER_RESULT_OK && host.Detach() == RENDER_RESULT_OK &&
		device.LiveCount() == 0, "billboard production fixture releases tables, scratch and native handles");
	return result;
}
}

int main()
{
	int result = 0;
	WWMath::Init();
	VertexMaterialClass::Init();
	result |= CheckPackedQuadColors();
	result |= RunPackingCase(0, 0);
	result |= RunPackingCase(3, 0);
	result |= RunPackingCase(0, 3);
	result |= RunFailureCase(2, 0, 2, 1,
		"actual PointGroup second allocation failure rolls back its first buffer");
	result |= RunFailureCase(0, 2, 2, 2,
		"actual PointGroup second upload failure rolls back both buffers");
	VertexMaterialClass::Shutdown();
	return result;
}
