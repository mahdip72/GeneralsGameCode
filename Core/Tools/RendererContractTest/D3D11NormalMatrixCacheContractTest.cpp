// Exact production-method fixture, as in D3D11BufferDiscardContractTest.
// This tests bytes/publication; it is not GPU or workload performance evidence.
#include "Renderer/RendererDevice.h"
#include "Renderer/NormalMatrixSubgroupCache.h"
#include "Renderer/RigidInstancingPolicy.h"
#include "TransformConstantArenaPolicy.h"
#include "D3D11ResultTranslation.h"
#include "Lib/FrameTimingDiagnostics.h"
#include <d3d11.h>
#include <float.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <xmmintrin.h>

namespace {
using namespace rts::render;
typedef detail::NormalMatrixSubgroupCache Cache;
typedef detail::TransformConstantArenaPolicy Arena;
const unsigned int TRANSFORM_CONSTANT_BUFFER_COUNT = 64;
#include "D3D11LegacyLayoutFlagConstants.inc"
#include "D3D11NormalConstants.inc"
#include "D3D11NormalMultiply.inc"
#include "D3D11DomainResult.inc"
#include "D3D11LegacyLayoutHelpers.inc"

int Check(bool condition, const char *message)
{
	if (!condition) fprintf(stderr, "FAIL: %s\n", message);
	return condition ? 0 : 1;
}
struct RestoreControls
{
	RestoreControls() : saved(_mm_getcsr()) {}
	~RestoreControls() { _mm_setcsr(saved); }
	unsigned int saved;
};
float FloatBits(unsigned int bits)
{
	float value; memcpy(&value, &bits, sizeof(value)); return value;
}
int CompareStage(Cache &cache, const RenderMatrix4 &matrix,
	Cache::Lookup expected, bool publish = true)
{
	float reference[12], actual[12];
	const unsigned int controls = _mm_getcsr();
	const bool built = BuildLegacyInverseTransposeNormalMatrix(matrix, reference);
	Cache::Entry pending;
	const Cache::Lookup lookup = cache.stage(matrix, actual, pending, controls);
	int result = Check(lookup == expected, "cache hit/miss/bypass decision");
	result |= Check(memcmp(reference, actual, sizeof(reference)) == 0 &&
		pending.buildResult == built, "all twelve floats and singular result match original builder");
	if (publish) cache.commit(pending);
	return result;
}
int TestKeysAndBytes()
{
	RestoreControls restore;
	_mm_setcsr((restore.saved & ~0xe07fU) | 0x1f80U);
	Cache cache;
	RenderMatrix4 matrix; matrix.setIdentity();
	int result = CompareStage(cache, matrix, Cache::Miss);
	result |= CompareStage(cache, matrix, Cache::Hit);
	const unsigned int ignored[] = { 3, 7, 11, 12, 13, 14, 15 };
	for (unsigned int i = 0; i < 7; ++i)
	{
		matrix.values[ignored[i]] += 7.0f;
		result |= CompareStage(cache, matrix, Cache::Hit);
	}
	matrix.values[3] = FloatBits(0x7fc00042U);
	matrix.values[12] = FloatBits(0x7f800000U);
	result |= CompareStage(cache, matrix, Cache::Hit); // Builder ignores these cells entirely.
	// Signed zero remains part of the raw key even if the result is unchanged.
	matrix.values[1] = FloatBits(0x80000000U);
	result |= CompareStage(cache, matrix, Cache::Miss);
	result |= CompareStage(cache, matrix, Cache::Hit);
	matrix.values[1] = 0.0f;
	result |= CompareStage(cache, matrix, Cache::Miss);
	RenderMatrix4 a; a.setIdentity(); a.values[0] = 2.0f;
	a.values[5] = 3.0f; a.values[10] = -4.0f;
	result |= CompareStage(cache, a, Cache::Miss);
	float actual[12]; Cache::Entry pending;
	cache.stage(a, actual, pending, _mm_getcsr());
	result |= Check(actual[0] == 0.5f && actual[5] == 1.0f / 3.0f &&
		actual[10] == -0.25f, "independent nonuniform/negative-scale normal expectation");
	const unsigned int zeros[3] = { 0, 0, 0 };
	result |= Check(memcmp(&actual[3], &zeros[0], 4) == 0 &&
		memcmp(&actual[7], &zeros[1], 4) == 0 &&
		memcmp(&actual[11], &zeros[2], 4) == 0, "positive-zero padding bytes");
	RenderMatrix4 b; b.setIdentity(); b.values[1] = 1.0f;
	result |= CompareStage(cache, b, Cache::Miss);
	cache.stage(b, actual, pending, _mm_getcsr());
	result |= Check(actual[0] == 1.0f && actual[4] == -1.0f &&
		actual[5] == 1.0f, "independent shear inverse-transpose expectation");
	result |= CompareStage(cache, a, Cache::Miss); // A/B/A single-entry churn.
	b.setIdentity(); b.values[0] = 0.0f; b.values[1] = -1.0f;
	b.values[4] = 1.0f; b.values[5] = 0.0f;
	result |= CompareStage(cache, b, Cache::Miss); // Rotation.
	b.setIdentity(); b.values[5] = 0.0f;
	result |= CompareStage(cache, b, Cache::Miss);
	result |= CompareStage(cache, b, Cache::Hit);
	cache.stage(b, actual, pending, _mm_getcsr());
	const unsigned int singularZeros[12] = { 0 };
	result |= Check(!pending.buildResult && memcmp(actual, singularZeros,
		sizeof(actual)) == 0, "singular output is twelve positive-zero words");
	b.setIdentity(); b.values[0] = 0.5e-6f;
	result |= CompareStage(cache, b, Cache::Miss);
	cache.stage(b, actual, pending, _mm_getcsr());
	result |= Check(!pending.buildResult, "below determinant threshold retains fallback");
	b.values[0] = 2.0e-6f;
	result |= CompareStage(cache, b, Cache::Miss);
	cache.stage(b, actual, pending, _mm_getcsr());
	result |= Check(pending.buildResult, "above determinant threshold retains inversion");
	cache.invalidate();
	result |= CompareStage(cache, b, Cache::Miss);
	return result;
}
int TestFloatingPointDomain()
{
	RestoreControls restore;
	const unsigned int base = (restore.saved & ~0xe07fU) | 0x1f80U;
	Cache cache;
	RenderMatrix4 matrix; matrix.setIdentity();
	matrix.values[0] = 3.0f; matrix.values[1] = FloatBits(1U);
	int result = 0;
	for (unsigned int rc = 0; rc < 4; ++rc)
		for (unsigned int flush = 0; flush < 4; ++flush)
		{
			_mm_setcsr(base | (rc << 13) | ((flush & 1U) << 6) |
				((flush & 2U) << 14));
			result |= CompareStage(cache, matrix, Cache::Miss);
			result |= CompareStage(cache, matrix, Cache::Hit);
		}
	_mm_setcsr(base);
	cache.invalidate(); matrix.setIdentity();
	result |= CompareStage(cache, matrix, Cache::Miss);
	_mm_setcsr(base | 0x3fU);
	result |= CompareStage(cache, matrix, Cache::Hit); // Sticky flags are not controls.
	_mm_setcsr(base);
	const unsigned int nonfinite[] = { 0x7fc00042U, 0x7f800000U, 0xff800000U };
	for (unsigned int i = 0; i < 3; ++i)
	{
		matrix.setIdentity(); matrix.values[0] = FloatBits(nonfinite[i]);
		result |= CompareStage(cache, matrix, Cache::Bypass);
		result |= CompareStage(cache, matrix, Cache::Bypass);
	}
	matrix.setIdentity(); matrix.values[0] = FLT_MAX;
	matrix.values[5] = FLT_MAX; matrix.values[10] = FLT_MAX;
	result |= CompareStage(cache, matrix, Cache::Bypass);
	result |= CompareStage(cache, matrix, Cache::Bypass);
	// Unmask invalid only after clearing sticky flags; identity cannot trap.
	_mm_setcsr(base & ~0x80U);
	matrix.setIdentity();
	result |= CompareStage(cache, matrix, Cache::Bypass);
	result |= CompareStage(cache, matrix, Cache::Bypass);
	_mm_setcsr(base);
	return result;
}

struct RecordingContext
{
	RecordingContext() : failMap(false), nullMap(false), maps(0), unmaps(0),
		vsBinds(0), psBinds(0), vsBinds1(0), psBinds1(0), lastBuffer(0),
		lastMode(D3D11_MAP_WRITE_DISCARD), vsBuffer(0), psBuffer(0),
		vsFirst(0), psFirst(0), vsCount(0), psCount(0) { memset(bytes, 0xcd, sizeof(bytes)); }
	HRESULT Map(ID3D11Buffer *buffer, UINT, D3D11_MAP mode, UINT, D3D11_MAPPED_SUBRESOURCE *mapped)
	{
		++maps; lastBuffer = buffer; lastMode = mode;
		if (failMap) return E_FAIL;
		if (mode == D3D11_MAP_WRITE_DISCARD) memset(bytes, 0xcd, sizeof(bytes));
		mapped->pData = nullMap ? 0 : bytes; return S_OK;
	}
	void Unmap(ID3D11Buffer *, UINT) { ++unmaps; }
	void VSSetConstantBuffers(UINT, UINT, ID3D11Buffer **) { ++vsBinds; }
	void PSSetConstantBuffers(UINT, UINT, ID3D11Buffer **) { ++psBinds; }
	void VSSetConstantBuffers1(UINT, UINT, ID3D11Buffer **buffer,
		const UINT *first, const UINT *count)
	{ ++vsBinds1; vsBuffer = *buffer; vsFirst = *first; vsCount = *count; }
	void PSSetConstantBuffers1(UINT, UINT, ID3D11Buffer **buffer,
		const UINT *first, const UINT *count)
	{ ++psBinds1; psBuffer = *buffer; psFirst = *first; psCount = *count; }
	bool failMap, nullMap;
	unsigned int maps, unmaps, vsBinds, psBinds, vsBinds1, psBinds1;
	ID3D11Buffer *lastBuffer;
	D3D11_MAP lastMode;
	ID3D11Buffer *vsBuffer, *psBuffer;
	unsigned int vsFirst, psFirst, vsCount, psCount;
	unsigned char bytes[Arena::PAGE_BYTES];
};
struct QuietTiming
{
	bool enabled() const { return false; }
	void begin(bool) {}
	void beginFrame() {}
};
class Fixture
{
public:
	#include "D3D11InstancedWorldAdmissionType.inc"
	Fixture(bool arena = false) : m_context(&context), m_context1(arena ? &context : 0),
		m_transformArena(arena ? reinterpret_cast<ID3D11Buffer *>(static_cast<size_t>(0x1000)) : 0),
		m_transformConstantCursor(0),
		m_transformConstantsValid(false), m_transformConstantsForInstancing(false), m_transformConstantsOrdinaryRestorePending(false), m_originalWorldAdmissionValid(false), m_originalWorldAdmissionControls(0), m_instancedReuseLayoutValid(false), m_instancedPublicationControls(0), m_instancedReuseInputLayout(0), m_transformConstantsChanged(false),
		m_boundSignedTextureMask(0), m_viewportX(0), m_viewportY(0),
		m_viewportWidth(1920), m_viewportHeight(1080), m_width(1920), m_height(1080),
		m_frameOpen(false), m_textureBindingsValid(true), m_pipelineStateValid(false),
		m_pipelineBound(false), m_vertexBufferBound(false), m_indexBufferBound(false),
		m_topologyBound(false), m_cachedLegacyStateValid(false),
		m_cachedLegacyPipelineValid(false), m_renderTargetsBound(false), m_viewportBound(false)
	{
		memset(&m_lastTransformConstants, 0, sizeof(m_lastTransformConstants));
		for (unsigned int i = 0; i < TRANSFORM_CONSTANT_BUFFER_COUNT; ++i)
			m_transformConstants[i] = reinterpret_cast<ID3D11Buffer *>(static_cast<size_t>(i + 1));
	}
	bool isOwner() const { return true; }
	bool gpuTimingFrameInfo(bool value) const { return value; }
	void bindDefaultRenderTargets() {}
	void bindDefaultViewport(unsigned int, unsigned int) {}
	void unbindTextureResources() {}
#include "D3D11RigidVertexLayout.inc"
#include "D3D11InstancedReuse.inc"
#include "D3D11NormalPublication.inc"
#include "D3D11InstancedDomainRefresh.inc"
#include "D3D11DomainSetterPrefix.inc"
        return RENDER_RESULT_UNSUPPORTED; // General pipeline branch is outside this recording fixture.
    }
	#include "D3D11LayoutSetter.inc"
	bool supportsRigidInstancing() const { return m_rigidSupport; }
	RenderResult setLegacyStateForLayoutInternal(const LegacyLogicalState &state,
		const LegacyVertexLayout &layout, unsigned int texturePresenceMask,
		bool instancedConstants, const InstancedWorldAdmission *admission = 0)
	{
		const bool oldHasFlags = m_hasVertexLayoutFlagsOverride;
		const unsigned int oldFlags = m_vertexLayoutFlagsOverride;
		const bool oldHasInput = m_hasInputLayoutOverride;
		ID3D11InputLayout *const oldInput = m_inputLayoutOverride;
		m_hasVertexLayoutFlagsOverride = true;
		m_vertexLayoutFlagsOverride = LegacyVertexLayoutFlags(layout);
		m_hasInputLayoutOverride = true;
		m_inputLayoutOverride = reinterpret_cast<ID3D11InputLayout *>(static_cast<size_t>(0x1001));
		const RenderResult result = setLegacyStateInternal(state,
			RENDER_VERTEX_POSITION3_NORMAL_COLOR_TEX1, texturePresenceMask,
			instancedConstants, admission);
		m_hasVertexLayoutFlagsOverride = oldHasFlags;
		m_vertexLayoutFlagsOverride = oldFlags;
		m_hasInputLayoutOverride = oldHasInput;
		m_inputLayoutOverride = oldInput;
		return result;
	}
#include "D3D11InstancedDomainSetter.inc"
#include "D3D11DomainCacheState.inc"
#include "D3D11NormalBeginFrame.inc"
#include "D3D11NormalInvalidation.inc"
	RecordingContext context;
	RecordingContext *m_context;
	RecordingContext *m_context1;
	ID3D11Buffer *m_transformArena;
	Arena m_transformArenaPolicy;
	ID3D11Buffer *m_transformConstants[TRANSFORM_CONSTANT_BUFFER_COUNT];
	unsigned int m_transformConstantCursor;
	LegacyTransformConstants m_lastTransformConstants;
	Cache m_worldNormalCache, m_worldViewNormalCache;
	bool m_transformConstantsValid, m_transformConstantsForInstancing, m_transformConstantsOrdinaryRestorePending;
    bool m_originalWorldAdmissionValid;
    unsigned int m_originalWorldAdmissionControls;
    float m_originalWorldAdmissionValues[16];
    bool m_instancedReuseLayoutValid;
    unsigned int m_instancedPublicationControls;
    ID3D11InputLayout *m_instancedReuseInputLayout;
    LegacyVertexLayout m_instancedReuseLayout;
    bool m_transformConstantsChanged;
	unsigned int m_boundSignedTextureMask;
	unsigned int m_boundCubeTextureMask = 0, m_cachedLegacyCubeTextureMask = 0;
	unsigned int m_cachedLegacySignedTextureMask = 0, m_cachedLegacyTexturePresenceMask = 0;
	unsigned int m_cachedLegacyVertexLayoutFlags = 3;
	unsigned char m_cachedLegacyState[sizeof(LegacyLogicalState)] = {};
	unsigned char m_cachedLegacyPipeline[sizeof(LegacyPipelineState)] = {};
	LegacyVertexFormat m_cachedLegacyVertexFormat = RENDER_VERTEX_POSITION3_NORMAL_COLOR_TEX1;
	bool m_hasInputLayoutOverride = true, m_hasVertexLayoutFlagsOverride = true;
	unsigned int m_vertexLayoutFlagsOverride = 3;
	ID3D11InputLayout *m_inputLayoutOverride = 0, *m_cachedLegacyInputLayout = 0;
	ID3D11InputLayout *m_seaWaveLayout = 0, *m_positionColorLayout = 0;

	float m_viewportX, m_viewportY, m_viewportWidth, m_viewportHeight;
	unsigned int m_width, m_height;
	bool m_rigidSupport = true;
	bool m_frameOpen, m_textureBindingsValid, m_pipelineStateValid, m_pipelineBound;
	bool m_vertexBufferBound, m_indexBufferBound, m_topologyBound;
	bool m_cachedLegacyStateValid, m_cachedLegacyPipelineValid, m_renderTargetsBound, m_viewportBound;
	GpuHandle m_boundTextures[LEGACY_TEXTURE_STAGE_COUNT];
	QuietTiming m_gpuTiming, m_presentTiming;
};
bool IsHit(Cache &cache, const RenderMatrix4 &matrix)
{
	float output[12]; Cache::Entry pending;
	return cache.stage(matrix, output, pending, _mm_getcsr()) == Cache::Hit;
}
int TestPublicationAndReset()
{
	RestoreControls restore;
	_mm_setcsr((restore.saved & ~0xe07fU) | 0x1f80U);
	Fixture fixture;
	LegacyLogicalState state;
	int result = Check(!IsHit(fixture.m_worldNormalCache, state.constants.world),
		"fresh device cache starts invalid");
	result |= Check(fixture.updateTransformConstants(state, 3U, 1U, 0U) == S_OK &&
		fixture.context.maps == 1 && fixture.context.unmaps == 1 &&
		fixture.context.vsBinds == 1 && fixture.context.psBinds == 1 &&
		fixture.m_transformConstantCursor == 1 && fixture.m_transformConstantsValid &&
		IsHit(fixture.m_worldNormalCache, state.constants.world), "successful Map/bind commits cache");
	result |= Check(memcmp(fixture.context.bytes, &fixture.m_lastTransformConstants,
		sizeof(LegacyTransformConstants)) == 0, "full unchanged shader block uploaded");
	// Force a staged miss with an equal GPU block to exercise equality success.
	fixture.m_worldNormalCache.invalidate(); fixture.m_worldViewNormalCache.invalidate();
	result |= Check(fixture.updateTransformConstants(state, 3U, 1U, 0U) == S_OK &&
		fixture.context.maps == 1 && fixture.context.vsBinds == 1 &&
		fixture.m_transformConstantCursor == 1 && !fixture.m_transformConstantsChanged &&
		IsHit(fixture.m_worldNormalCache, state.constants.world) &&
		IsHit(fixture.m_worldViewNormalCache, state.constants.world), "equality return commits pending without Map/bind");
	const LegacyTransformConstants published = fixture.m_lastTransformConstants;
	RenderMatrix4 oldWorld = state.constants.world;
	state.constants.world.values[0] = 2.0f;
	fixture.context.failMap = true;
	result |= Check(fixture.updateTransformConstants(state, 3U, 1U, 0U) == E_FAIL &&
		fixture.m_transformConstantCursor == 2 && fixture.context.unmaps == 1 &&
		fixture.context.vsBinds == 1 && !fixture.m_transformConstantsValid &&
		fixture.m_transformConstantsChanged &&
		memcmp(&fixture.m_lastTransformConstants, &published, sizeof(published)) == 0 &&
		IsHit(fixture.m_worldNormalCache, oldWorld) &&
		!IsHit(fixture.m_worldNormalCache, state.constants.world) &&
		!IsHit(fixture.m_worldViewNormalCache, state.constants.world), "failed Map retains old entries and discards both pending entries");
	fixture.context.failMap = false; fixture.context.nullMap = true;
	result |= Check(fixture.updateTransformConstants(state, 3U, 1U, 0U) == E_FAIL &&
		fixture.m_transformConstantCursor == 3 && fixture.context.unmaps == 2 &&
		fixture.context.vsBinds == 1 && !fixture.m_transformConstantsValid &&
		fixture.m_transformConstantsChanged &&
		memcmp(&fixture.m_lastTransformConstants, &published, sizeof(published)) == 0 &&
		IsHit(fixture.m_worldNormalCache, oldWorld) &&
		!IsHit(fixture.m_worldNormalCache, state.constants.world), "null Map unmaps once, retains published bytes/cache and original ring advance");
	fixture.context.nullMap = false;
	result |= Check(fixture.updateTransformConstants(state, 3U, 1U, 0U) == S_OK &&
		fixture.m_transformConstantCursor == 4 && fixture.context.vsBinds == 2 &&
		IsHit(fixture.m_worldNormalCache, state.constants.world), "retry uploads/binds next ring slot and commits new key");
	fixture.invalidatePipelineBindings();
	result |= Check(!fixture.m_transformConstantsValid &&
		!IsHit(fixture.m_worldNormalCache, state.constants.world) &&
		!IsHit(fixture.m_worldViewNormalCache, state.constants.world), "pipeline invalidation revokes both caches and GPU proof");
	unsigned int maps = fixture.context.maps;
	fixture.updateTransformConstants(state, 3U, 1U, 0U);
	result |= Check(fixture.context.maps == maps + 1, "cache reset never skips required Map");
	fixture.invalidateContextBindings();
	result |= Check(!IsHit(fixture.m_worldNormalCache, state.constants.world) &&
		!IsHit(fixture.m_worldViewNormalCache, state.constants.world), "context invalidation reaches both caches");
	fixture.updateTransformConstants(state, 3U, 1U, 0U);
	result |= Check(fixture.beginFrame() == RENDER_RESULT_OK &&
		!fixture.m_transformConstantsValid &&
		!IsHit(fixture.m_worldNormalCache, state.constants.world) &&
		!IsHit(fixture.m_worldViewNormalCache, state.constants.world), "actual beginFrame invalidates both caches");
	maps = fixture.context.maps;
	fixture.updateTransformConstants(state, 3U, 1U, 0U);
	result |= Check(fixture.context.maps == maps + 1, "first publication after beginFrame uploads");
	Fixture recreated;
	result |= Check(!IsHit(recreated.m_worldNormalCache, state.constants.world) &&
		!IsHit(recreated.m_worldViewNormalCache, state.constants.world), "recreated fixed records start invalid");
	return result;
}
int TestWorldViewIsolation()
{
	RestoreControls restore;
	_mm_setcsr((restore.saved & ~0xe07fU) | 0x1f80U);
	Cache worldCache, viewCache;
	RenderMatrix4 world, view, product;
	world.setIdentity(); view.setIdentity(); view.values[0] = 2.0f;
	MultiplyMatrices(world.values, view.values, product.values);
	int result = CompareStage(worldCache, world, Cache::Miss);
	result |= CompareStage(viewCache, product, Cache::Miss);
	result |= CompareStage(worldCache, world, Cache::Hit);
	result |= CompareStage(viewCache, world, Cache::Miss); // Cannot borrow world entry.
	view.setIdentity(); view.values[0] = 0.0f; view.values[1] = -1.0f;
	view.values[4] = 1.0f; view.values[5] = 0.0f;
	MultiplyMatrices(world.values, view.values, product.values);
	result |= CompareStage(worldCache, world, Cache::Hit);
	result |= CompareStage(viewCache, product, Cache::Miss);
	// Nonaffine world: its ignored column couples to the view's translation row.
	world.values[3] = 1.0f; view.setIdentity(); view.values[12] = 2.0f;
	MultiplyMatrices(world.values, view.values, product.values);
	result |= CompareStage(worldCache, world, Cache::Hit);
	result |= CompareStage(viewCache, product, Cache::Miss);
	view.values[12] = 3.0f;
	MultiplyMatrices(world.values, view.values, product.values);
	result |= CompareStage(worldCache, world, Cache::Hit);
	result |= CompareStage(viewCache, product, Cache::Miss);
	// Exercise the real backend with nonaffine view terms as well. The world
	// key is unchanged while an ignored world cell changes the computed key.
	Fixture fixture; LegacyLogicalState state;
	state.constants.world = world; state.constants.view = view;
	state.constants.view.values[3] = 0.25f;
	result |= Check(fixture.updateTransformConstants(state, 3U, 0U, 0U) == S_OK,
		"nonaffine-view publication succeeds");
	state.constants.world.values[3] = 2.0f;
	MultiplyMatrices(state.constants.world.values, state.constants.view.values, product.values);
	result |= Check(IsHit(fixture.m_worldNormalCache, state.constants.world) &&
		!IsHit(fixture.m_worldViewNormalCache, product),
		"backend keeps separate exact world and computed world-view keys");
	result |= Check(fixture.updateTransformConstants(state, 3U, 0U, 0U) == S_OK,
		"changed computed world-view subgroup publishes");
	float reference[12];
	BuildLegacyInverseTransposeNormalMatrix(product, reference);
	result |= Check(memcmp(reference, fixture.m_lastTransformConstants.worldViewNormalMatrix,
		sizeof(reference)) == 0, "backend world-view bytes match unchanged multiply and original builder");
	return result;
}
int CheckArenaSlice(const Fixture &fixture, const LegacyLogicalState &state,
	unsigned int slot)
{
	const unsigned char *slice = fixture.context.bytes + slot * Arena::SLICE_BYTES;
	int result = Check(sizeof(LegacyTransformConstants) == Arena::PAYLOAD_BYTES &&
		memcmp(slice, &fixture.m_lastTransformConstants, sizeof(LegacyTransformConstants)) == 0,
		"arena slice contains the complete published shader payload");
	for (unsigned int i = Arena::PAYLOAD_BYTES; i < Arena::SLICE_BYTES; ++i)
		result |= Check(slice[i] == 0, "arena slice tail is zero-padded");
	float reference[12]; RenderMatrix4 product;
	BuildLegacyInverseTransposeNormalMatrix(state.constants.world, reference);
	result |= Check(memcmp(reference, fixture.m_lastTransformConstants.worldNormalMatrix,
		sizeof(reference)) == 0, "arena world subgroup matches all twelve original-builder floats");
	MultiplyMatrices(state.constants.world.values, state.constants.view.values, product.values);
	BuildLegacyInverseTransposeNormalMatrix(product, reference);
	result |= Check(memcmp(reference, fixture.m_lastTransformConstants.worldViewNormalMatrix,
		sizeof(reference)) == 0, "arena world-view subgroup matches all twelve original-builder floats");
	result |= Check(fixture.context.vsBuffer == fixture.m_transformArena &&
		fixture.context.psBuffer == fixture.m_transformArena &&
		fixture.context.vsFirst == slot * Arena::SLICE_BYTES / 16 &&
		fixture.context.psFirst == fixture.context.vsFirst &&
		fixture.context.vsCount == Arena::RANGE_CONSTANTS &&
		fixture.context.psCount == Arena::RANGE_CONSTANTS &&
		fixture.context.vsBinds == 0 && fixture.context.psBinds == 0,
		"both stages bind the arena offset/count without a legacy rebind");
	return result;
}
int TestArenaPublicationAndReset()
{
	RestoreControls restore;
	_mm_setcsr((restore.saved & ~0xe07fU) | 0x1f80U);
	Fixture fixture(true); LegacyLogicalState state;
	fixture.context.failMap = true;
	int result = Check(fixture.updateTransformConstants(state, 3U, 1U, 0U) == E_FAIL &&
		fixture.context.lastMode == D3D11_MAP_WRITE_DISCARD &&
		fixture.m_transformArenaPolicy.reserve().slot == 0 &&
		fixture.m_transformConstantCursor == 0 && !fixture.m_transformConstantsValid &&
		fixture.m_transformConstantsChanged && fixture.context.unmaps == 0 &&
		fixture.context.vsBinds1 == 0 && fixture.context.psBinds1 == 0 &&
		!IsHit(fixture.m_worldNormalCache, state.constants.world) &&
		!IsHit(fixture.m_worldViewNormalCache, state.constants.world),
		"failed first arena Map discards pending caches without consuming DISCARD range");
	fixture.context.failMap = false; fixture.context.nullMap = true;
	result |= Check(fixture.updateTransformConstants(state, 3U, 1U, 0U) == E_FAIL &&
		fixture.context.lastMode == D3D11_MAP_WRITE_DISCARD && fixture.context.unmaps == 1 &&
		fixture.m_transformArenaPolicy.reserve().slot == 0 &&
		fixture.m_transformConstantCursor == 0 && !fixture.m_transformConstantsValid &&
		fixture.m_transformConstantsChanged && fixture.context.vsBinds1 == 0 &&
		fixture.context.psBinds1 == 0 && !IsHit(fixture.m_worldNormalCache, state.constants.world) &&
		!IsHit(fixture.m_worldViewNormalCache, state.constants.world),
		"null first arena Map unmaps, revokes GPU validity and preserves unconsumed DISCARD range");
	fixture.context.nullMap = false;
	result |= Check(fixture.updateTransformConstants(state, 3U, 1U, 0U) == S_OK &&
		fixture.context.lastMode == D3D11_MAP_WRITE_DISCARD &&
		fixture.context.lastBuffer == fixture.m_transformArena &&
		fixture.m_transformArenaPolicy.reserve().slot == 1 &&
		fixture.m_transformConstantCursor == 0 && fixture.context.vsBinds1 == 1 &&
		fixture.context.psBinds1 == 1 && IsHit(fixture.m_worldNormalCache, state.constants.world),
		"first arena retry publishes slice zero and commits CPU entries after offset binds");
	result |= CheckArenaSlice(fixture, state, 0);
	unsigned char oldSlice[Arena::SLICE_BYTES];
	memcpy(oldSlice, fixture.context.bytes, sizeof(oldSlice));
	const LegacyTransformConstants oldPublished = fixture.m_lastTransformConstants;
	const RenderMatrix4 oldWorld = state.constants.world;
	state.constants.world.values[0] = 2.0f;
	fixture.context.failMap = true;
	result |= Check(fixture.updateTransformConstants(state, 3U, 1U, 0U) == E_FAIL &&
		fixture.context.lastMode == D3D11_MAP_WRITE_NO_OVERWRITE &&
		fixture.m_transformArenaPolicy.reserve().slot == 1 &&
		!fixture.m_transformConstantsValid && fixture.m_transformConstantsChanged &&
		fixture.context.unmaps == 2 && fixture.context.vsBinds1 == 1 && fixture.context.psBinds1 == 1 &&
		memcmp(&oldPublished, &fixture.m_lastTransformConstants, sizeof(oldPublished)) == 0 &&
		IsHit(fixture.m_worldNormalCache, oldWorld) &&
		IsHit(fixture.m_worldViewNormalCache, oldWorld) &&
		!IsHit(fixture.m_worldNormalCache, state.constants.world) &&
		!IsHit(fixture.m_worldViewNormalCache, state.constants.world),
		"failed append revokes GPU proof, keeps old CPU entries and discards both pending misses");
	fixture.context.failMap = false; fixture.context.nullMap = true;
	result |= Check(fixture.updateTransformConstants(state, 3U, 1U, 0U) == E_FAIL &&
		fixture.context.lastMode == D3D11_MAP_WRITE_NO_OVERWRITE &&
		fixture.m_transformArenaPolicy.reserve().slot == 1 &&
		!fixture.m_transformConstantsValid && fixture.m_transformConstantsChanged &&
		fixture.context.unmaps == 3 && fixture.context.vsBinds1 == 1 && fixture.context.psBinds1 == 1 &&
		memcmp(&oldPublished, &fixture.m_lastTransformConstants, sizeof(oldPublished)) == 0 &&
		IsHit(fixture.m_worldNormalCache, oldWorld) &&
		IsHit(fixture.m_worldViewNormalCache, oldWorld) &&
		!IsHit(fixture.m_worldNormalCache, state.constants.world) &&
		!IsHit(fixture.m_worldViewNormalCache, state.constants.world),
		"null append leaves prior CPU publication and unconsumed next range");
	fixture.context.nullMap = false;
	result |= Check(fixture.updateTransformConstants(state, 3U, 1U, 0U) == S_OK &&
		fixture.context.lastMode == D3D11_MAP_WRITE_NO_OVERWRITE &&
		fixture.m_transformArenaPolicy.reserve().slot == 2 && fixture.m_transformConstantsValid &&
		IsHit(fixture.m_worldNormalCache, state.constants.world) &&
		memcmp(oldSlice, fixture.context.bytes, sizeof(oldSlice)) == 0,
		"successful fresh append consumes one range and preserves earlier live slice bytes");
	result |= CheckArenaSlice(fixture, state, 1);
	const unsigned int maps = fixture.context.maps;
	const unsigned int offsetBinds = fixture.context.vsBinds1;
	fixture.m_worldNormalCache.invalidate(); fixture.m_worldViewNormalCache.invalidate();
	result |= Check(fixture.updateTransformConstants(state, 3U, 1U, 0U) == S_OK &&
		fixture.context.maps == maps && fixture.context.vsBinds1 == offsetBinds &&
		fixture.context.psBinds1 == offsetBinds && fixture.m_transformArenaPolicy.reserve().slot == 2 &&
		!fixture.m_transformConstantsChanged && IsHit(fixture.m_worldNormalCache, state.constants.world) &&
		IsHit(fixture.m_worldViewNormalCache, state.constants.world),
		"arena equality return commits pending normal entries without consuming or rebinding a range");
	// Fill the remaining distinct slots through the actual upload method.
	for (unsigned int slot = 2; slot < Arena::SLICE_COUNT; ++slot)
	{
		state.constants.world.values[0] = static_cast<float>(slot + 2);
		result |= Check(fixture.updateTransformConstants(state, 3U, 1U, 0U) == S_OK &&
			fixture.context.lastMode == D3D11_MAP_WRITE_NO_OVERWRITE &&
			fixture.m_transformArenaPolicy.reserve().slot == (slot + 1) % Arena::SLICE_COUNT &&
			fixture.m_transformConstantCursor == 0 &&
			memcmp(oldSlice, fixture.context.bytes, sizeof(oldSlice)) == 0,
			"every fresh arena append preserves prior slice until wrap");
		result |= CheckArenaSlice(fixture, state, slot);
	}
	state.constants.world.values[0] = 99.0f;
	result |= Check(fixture.updateTransformConstants(state, 3U, 1U, 0U) == S_OK &&
		fixture.context.lastMode == D3D11_MAP_WRITE_DISCARD &&
		fixture.m_transformArenaPolicy.reserve().slot == 1 && fixture.m_transformConstantCursor == 0,
		"full arena wraps with DISCARD instead of overwriting a live range");
	result |= CheckArenaSlice(fixture, state, 0);
	// The page still has a published range when beginFrame resets the policy.
	result |= Check(fixture.beginFrame() == RENDER_RESULT_OK &&
		fixture.m_transformArenaPolicy.reserve().slot == 0 &&
		fixture.m_transformArenaPolicy.reserve().discard && !fixture.m_transformConstantsValid &&
		!IsHit(fixture.m_worldNormalCache, state.constants.world) &&
		!IsHit(fixture.m_worldViewNormalCache, state.constants.world),
		"actual beginFrame forces DISCARD and cache misses while old ranges could remain live");
	const unsigned int resetMaps = fixture.context.maps;
	result |= Check(fixture.updateTransformConstants(state, 3U, 1U, 0U) == S_OK &&
		fixture.context.maps == resetMaps + 1 &&
		fixture.context.lastMode == D3D11_MAP_WRITE_DISCARD &&
		fixture.m_transformArenaPolicy.reserve().slot == 1,
		"GPU invalidation prevents equal payload from skipping post-frame-reset DISCARD");
	result |= CheckArenaSlice(fixture, state, 0);
	fixture.invalidateContextBindings();
	result |= Check(!fixture.m_transformConstantsValid &&
		!IsHit(fixture.m_worldNormalCache, state.constants.world) &&
		!IsHit(fixture.m_worldViewNormalCache, state.constants.world),
		"context invalidation clears GPU proof and both normal caches");
	result |= Check(fixture.updateTransformConstants(state, 3U, 1U, 0U) == S_OK &&
		fixture.context.lastMode == D3D11_MAP_WRITE_NO_OVERWRITE &&
		fixture.m_transformArenaPolicy.reserve().slot == 2,
		"context rebind uses fresh unconsumed arena range without rewinding live page");
	result |= CheckArenaSlice(fixture, state, 1);
	return result;
}
}
// Executes actual extracted packing/refresh/reset production bodies, not a model.
int TestInstancedConstantDomain()
{
    RestoreControls restore;
    _mm_setcsr((restore.saved & ~0xe07fU) | 0x1f80U);
    int result=0;
    for (unsigned int arena=0;arena<2;++arena) {
        Fixture f(arena!=0);
        LegacyLogicalState state;
        state.constants.world.values[0]=2;
        state.constants.world.values[12]=5;
        state.pipeline.secondaryGradientEnable=true;
        state.pipeline.lightingEnable=true;
        state.constants.material.specularPower=9;
        state.constants.view.values[1]=.125f;
        state.constants.view.values[14]=.2f;
        result|=Check(f.updateTransformConstants(state,3,0,0,true)==S_OK &&
            f.m_transformConstantsForInstancing,"canonical domain publishes only with successful full buffer");
        const LegacyTransformConstants canonical=f.m_lastTransformConstants;
        unsigned int maps=f.context.maps;
        state.constants.world.values[0]=3;
        state.constants.world.values[12]=11;
        result|=Check(f.updateTransformConstants(state,3,0,0,true)==S_OK &&
            f.context.maps==maps,"world-only changes skip Map in canonical domain");
        result|=Check(memcmp(&canonical,&f.m_lastTransformConstants,sizeof(canonical))==0,
            "canonical b0 bytes do not claim original logical world");
        state.constants.view.values[4]=.25f;
        result|=Check(f.updateTransformConstants(state,3,0,0,true)==S_OK &&
            f.context.maps==++maps && memcmp(f.m_lastTransformConstants.view,
            state.constants.view.values,sizeof(state.constants.view.values))==0,
            "live View changes upload in canonical domain");
        const LegacyTransformConstants actualCanonical=f.m_lastTransformConstants;
        result|=Check(f.updateTransformConstants(state,3,0,0,false)==S_OK &&
            f.context.maps==++maps && !f.m_transformConstantsForInstancing,
            "ordinary domain republishes exact original world");
        LegacyTransformConstants ordinary=f.m_lastTransformConstants;
        LegacyTransformConstants normalized=actualCanonical;
        memset(ordinary.worldViewProjection,0,sizeof(ordinary.worldViewProjection));
        memset(ordinary.worldView,0,sizeof(ordinary.worldView));
        memset(ordinary.world,0,sizeof(ordinary.world));
        memset(ordinary.worldNormalMatrix,0,sizeof(ordinary.worldNormalMatrix));
        memset(ordinary.worldViewNormalMatrix,0,sizeof(ordinary.worldViewNormalMatrix));
        memset(normalized.worldViewProjection,0,sizeof(normalized.worldViewProjection));
        memset(normalized.worldView,0,sizeof(normalized.worldView));
        memset(normalized.world,0,sizeof(normalized.world));
        memset(normalized.worldNormalMatrix,0,sizeof(normalized.worldNormalMatrix));
        memset(normalized.worldViewNormalMatrix,0,sizeof(normalized.worldViewNormalMatrix));
        result|=Check(memcmp(&ordinary,&normalized,sizeof(ordinary))==0,
            "all bytes outside the five overridden matrices remain original");
        result|=Check(f.updateTransformConstants(state,3,0,0,true)==S_OK,"seed instanced domain");
        maps=f.context.maps;
        memcpy(f.m_cachedLegacyState,&state,sizeof(state));f.m_cachedLegacyStateValid=true;
        f.m_cachedLegacyVertexLayoutFlags=3;f.m_transformConstantsChanged=false;
        result|=Check(f.refreshTransformConstantsForDraw()==RENDER_RESULT_OK &&
            f.context.maps==maps+1 && !f.m_transformConstantsForInstancing &&
            memcmp(f.m_lastTransformConstants.world,state.constants.world.values,
                sizeof(state.constants.world.values))==0,
            "equal logical state ordinary draw forces physical world recovery");
        maps=f.context.maps;
        result|=Check(f.refreshTransformConstantsForDraw(f.m_transformConstantsForInstancing)==RENDER_RESULT_OK &&
            f.context.maps==maps,"old ordinary-set then instanced-draw chain avoids repack");
        f.updateTransformConstants(state,3,0,0,true);maps=f.context.maps;
        // Texture binding dirtiness refreshes live b0 without losing canonical domain.
        f.m_boundCubeTextureMask=1;f.m_transformConstantsChanged=true;
        result|=Check(f.refreshTransformConstantsForDraw(true)==RENDER_RESULT_OK &&
            f.context.maps==maps+1 && f.m_transformConstantsForInstancing,
            "dirty live texture-mask refresh retains selected physical domain");
        const LegacyTransformConstants published=f.m_lastTransformConstants;
        LegacyLogicalState bad=state;bad.constants.world.values[0]=0;
        maps=f.context.maps;
        result|=Check(f.updateTransformConstants(bad,3,0,1,true)==E_INVALIDARG &&
            f.context.maps==maps && f.m_transformConstantsForInstancing,
            "singular original owner world cannot be hidden by normalization");
        bad=state;bad.constants.world.values[0]=FloatBits(0x7fc00042U);
        result|=Check(f.updateTransformConstants(bad,3,0,1,true)==E_INVALIDARG &&
            f.context.maps==maps && memcmp(&published,&f.m_lastTransformConstants,sizeof(published))==0,
            "nonfinite original owner world rejected without publication");
        state.constants.view.values[12]+=.2f;f.context.failMap=true;
        result|=Check(FAILED(f.updateTransformConstants(state,3,0,1,true)) &&
            !f.m_transformConstantsValid && !f.m_transformConstantsForInstancing &&
            memcmp(&published,&f.m_lastTransformConstants,sizeof(published))==0,
            "failed canonical Map revokes domain proof and keeps old published bytes");
        f.context.failMap=false;f.context.nullMap=true;
        result|=Check(FAILED(f.updateTransformConstants(state,3,0,1,true)) &&
            !f.m_transformConstantsValid && !f.m_transformConstantsForInstancing,
            "null canonical Map cannot publish domain");
        f.context.nullMap=false;
        result|=Check(f.updateTransformConstants(state,3,0,1,true)==S_OK &&
            f.m_transformConstantsForInstancing,"canonical retry republishes successfully");
        f.invalidateContextBindings();
        result|=Check(!f.m_transformConstantsValid && !f.m_transformConstantsForInstancing,
            "context invalidation revokes physical domain");
        f.updateTransformConstants(state,3,0,1,true);
        memcpy(f.m_cachedLegacyState,&state,sizeof(state));f.m_cachedLegacyStateValid=true;
        _mm_setcsr((_mm_getcsr() & ~0x3fU) & ~0x80U);
        result|=Check(f.refreshTransformConstantsForDraw(true)==RENDER_RESULT_OK &&
            !f.m_transformConstantsForInstancing,
            "unmasked control switch falls back to original ordinary expressions");
        _mm_setcsr((restore.saved & ~0xe07fU) | 0x1f80U);
    }
    return result;
}

// Calls the real extracted public wrapper/internal cache prefix. Setup seeds an
// already bound matching pipeline; the omitted general pipeline branch fails closed.
int TestOrdinaryStateRestoreRetry()
{
    RestoreControls restore;
    const unsigned int masked=(restore.saved & ~0xe07fU) | 0x1f80U;
    _mm_setcsr(masked);
    int result=0;
    for (unsigned int arena=0;arena<2;++arena) {
        Fixture f(arena!=0);
        LegacyLogicalState state;state.constants.world.values[0]=2;
        state.constants.world.values[12]=7;
        f.m_frameOpen=true;f.m_pipelineStateValid=true;f.m_pipelineBound=true;
        f.m_cachedLegacyPipelineValid=true;
        memcpy(f.m_cachedLegacyPipeline,&state.pipeline,sizeof(state.pipeline));
        result|=Check(f.setLegacyStateInternal(state,RENDER_VERTEX_POSITION3_NORMAL_COLOR_TEX1,0,true)==RENDER_RESULT_OK &&
            f.m_cachedLegacyStateValid && f.m_transformConstantsForInstancing,
            "production instanced setter prefix eagerly publishes canonical b0 and original logical S");
        unsigned int maps=f.context.maps;
        f.context.failMap=true;
        result|=Check(f.setLegacyState(state,RENDER_VERTEX_POSITION3_NORMAL_COLOR_TEX1,0)==RENDER_RESULT_FAILED &&
            f.context.maps==++maps && f.m_transformConstantsOrdinaryRestorePending &&
            !f.m_transformConstantsValid && !f.m_transformConstantsForInstancing,
            "public ordinary setter failure latches restoration while revoking physical proof");
        result|=Check(f.setLegacyState(state,RENDER_VERTEX_POSITION3_NORMAL_COLOR_TEX1,0)==RENDER_RESULT_FAILED &&
            f.context.maps==++maps && f.m_transformConstantsOrdinaryRestorePending,
            "same-state public ordinary retry actually retries failed Map instead of returning false OK");
        f.context.failMap=false;f.context.nullMap=true;
        result|=Check(f.setLegacyState(state,RENDER_VERTEX_POSITION3_NORMAL_COLOR_TEX1,0)==RENDER_RESULT_FAILED &&
            f.context.maps==++maps && f.m_transformConstantsOrdinaryRestorePending,
            "public setter null Map keeps restoration pending");
        f.context.nullMap=false;
        result|=Check(f.setLegacyState(state,RENDER_VERTEX_POSITION3_NORMAL_COLOR_TEX1,0)==RENDER_RESULT_OK &&
            f.context.maps==++maps && f.m_transformConstantsValid &&
            !f.m_transformConstantsOrdinaryRestorePending && !f.m_transformConstantsForInstancing &&
            memcmp(f.m_lastTransformConstants.world,state.constants.world.values,sizeof(state.constants.world.values))==0 &&
            memcmp(f.m_cachedLegacyState,&state,sizeof(state))==0,
            "successful eager ordinary retry publishes original world and clears restoration latch");
        result|=Check(f.setLegacyState(state,RENDER_VERTEX_POSITION3_NORMAL_COLOR_TEX1,0)==RENDER_RESULT_OK &&
            f.context.maps==maps,"restored equal-state shortcut avoids redundant Map");
        // Existing ordinary-only dirtiness keeps its original setter/draw timing.
        f.m_viewportWidth+=1;f.m_transformConstantsChanged=true;f.context.failMap=true;
        result|=Check(f.setLegacyState(state,RENDER_VERTEX_POSITION3_NORMAL_COLOR_TEX1,0)==RENDER_RESULT_OK &&
            f.context.maps==maps && f.m_transformConstantsChanged && !f.m_transformConstantsOrdinaryRestorePending,
            "ordinary-only dirty equal-state setter retains original no-Map timing");
        result|=Check(f.refreshTransformConstantsForDraw()==RENDER_RESULT_FAILED &&
            f.context.maps==++maps,"ordinary-only dirty publication still fails at draw refresh");
        f.context.failMap=false;f.refreshTransformConstantsForDraw();
        result|=Check(f.setLegacyStateInternal(state,RENDER_VERTEX_POSITION3_NORMAL_COLOR_TEX1,0,true)==RENDER_RESULT_OK,
            "reenter canonical domain for effective ordinary fallback test");
        _mm_setcsr(masked & ~0x80U);f.context.failMap=true;f.m_transformConstantsChanged=false;
        maps=f.context.maps;
        result|=Check(f.refreshTransformConstantsForDraw(true)==RENDER_RESULT_FAILED &&
            f.context.maps==++maps && f.m_transformConstantsOrdinaryRestorePending,
            "true domain request falling back under unmasked controls latches failed ordinary publication");
        result|=Check(f.setLegacyState(state,RENDER_VERTEX_POSITION3_NORMAL_COLOR_TEX1,0)==RENDER_RESULT_FAILED &&
            f.context.maps==++maps,"public retry cannot elide effective ordinary fallback failure");
        _mm_setcsr(masked);f.context.failMap=false;
        result|=Check(f.setLegacyState(state,RENDER_VERTEX_POSITION3_NORMAL_COLOR_TEX1,0)==RENDER_RESULT_OK &&
            !f.m_transformConstantsOrdinaryRestorePending,"effective ordinary retry clears only on success");
        f.setLegacyStateInternal(state,RENDER_VERTEX_POSITION3_NORMAL_COLOR_TEX1,0,true);
        f.context.failMap=true;f.setLegacyState(state,RENDER_VERTEX_POSITION3_NORMAL_COLOR_TEX1,0);
        f.invalidatePipelineBindings();
        result|=Check(!f.m_transformConstantsOrdinaryRestorePending && !f.m_cachedLegacyStateValid &&
            !f.m_transformConstantsValid,"full logical/physical invalidation clears obsolete restoration latch");
    }
    return result;
}

// Separate counting adapter extracts the exact admission helper, with a wrapper
// that calls the real validator. Main fixture/benchmark has no added counters.
struct AdmissionFixture
{
    bool m_originalWorldAdmissionValid = false;
    unsigned int m_originalWorldAdmissionControls = 0, validations = 0;
    float m_originalWorldAdmissionValues[16];
    bool countActualValidator(const RenderMatrix4 &world)
    {
        ++validations;
        return rts::render::RigidInstancingWorldValid(world);
    }
#define RigidInstancingWorldValid countActualValidator
#include "D3D11OriginalWorldAdmission.inc"
#undef RigidInstancingWorldValid
};
int TestOriginalWorldAdmissionMemo()
{
    RestoreControls restore;
    const unsigned int masked=(restore.saved & ~0xe07fU) | 0x1f80U;
    _mm_setcsr(masked);
    AdmissionFixture f;RenderMatrix4 world;
    int result=Check(f.validateOriginalInstancedWorld(world) && f.validations==1 &&
        f.validateOriginalInstancedWorld(world) && f.validations==1,
        "actual helper validates once then reuses exact original world/control proof");
    unsigned int expected=f.validations;
    for(unsigned int i=0;i<16;++i) {
        world.setIdentity();world.values[i]+=.0625f;
        result|=Check(f.validateOriginalInstancedWorld(world) && f.validations==++expected,
            "all sixteen original world cells participate in admission key");
    }
    world.setIdentity();world.values[3]=FloatBits(0x80000000U);
    result|=Check(f.validateOriginalInstancedWorld(world) && f.validations==++expected &&
        f.validateOriginalInstancedWorld(world) && f.validations==expected,
        "signed zero and otherwise unused world cells cannot alias a stale proof");
    for(unsigned int rc=0;rc<4;++rc) for(unsigned int flush=0;flush<4;++flush) {
        const unsigned int controls=masked | (rc<<13) | ((flush&1U)<<6) | ((flush&2U)<<14);
        _mm_setcsr(controls);const unsigned int before=f.validations;
        const bool hit=f.m_originalWorldAdmissionValid && f.m_originalWorldAdmissionControls==(controls & ~0x3fU);
        result|=Check(f.validateOriginalInstancedWorld(world) && f.validations==before+(hit?0U:1U),
            "every RC/DAZ/FTZ control mode requires its exact admission proof");
        const unsigned int after=f.validations;
        _mm_setcsr(controls | 0x3fU);
        result|=Check(f.validateOriginalInstancedWorld(world) && f.validations==after,
            "sticky flags do not masquerade as FP control changes");
    }
    _mm_setcsr(masked & ~0x80U);expected=f.validations;
    result|=Check(!f.validateOriginalInstancedWorld(world) && !f.m_originalWorldAdmissionValid &&
        f.validations==expected,"unmasked domain cannot reuse or populate admission memo");
    _mm_setcsr(masked);world.setIdentity();world.values[12]=FloatBits(0x7fc00042U);
    result|=Check(!f.validateOriginalInstancedWorld(world) && !f.m_originalWorldAdmissionValid &&
        f.validations==++expected,"nonfinite ignored normal-matrix cell is still rejected by actual validator");
    world.setIdentity();world.values[0]=0;
    result|=Check(!f.validateOriginalInstancedWorld(world) && !f.m_originalWorldAdmissionValid &&
        f.validations==++expected,"singular original world never populates positive memo");
    world.setIdentity();result|=Check(f.validateOriginalInstancedWorld(world),"valid world recovers admission after rejection");
    for(unsigned int arena=0;arena<2;++arena) {
        Fixture p(arena!=0);LegacyLogicalState state;state.constants.world.values[0]=2;
        result|=Check(p.updateTransformConstants(state,3,0,0,true)==S_OK && p.m_originalWorldAdmissionValid,
            "actual pack wrapper publishes independent CPU admission proof");
        p.context.failMap=true;state.constants.view.values[12]=.25f;
        result|=Check(FAILED(p.updateTransformConstants(state,3,0,0,true)) &&
            !p.m_originalWorldAdmissionValid && !p.m_transformConstantsValid,
            "failed physical publication conservatively revokes memo without weakening failure");
        p.context.failMap=false;
        result|=Check(p.updateTransformConstants(state,3,0,0,true)==S_OK && p.m_originalWorldAdmissionValid,
            "retry revalidates and republishes safely");
        p.invalidateContextBindings();
        result|=Check(!p.m_originalWorldAdmissionValid,"full context reset invalidates admission memo");
    }
    return result;
}

class AdmissionUpdateFixture : public Fixture
{
public:
    AdmissionUpdateFixture(bool arena) : Fixture(arena), validations(0) {}
    unsigned int validations;
    bool validateOriginalInstancedWorld(const RenderMatrix4 &world)
    { ++validations; return Fixture::validateOriginalInstancedWorld(world); }
#include "D3D11InstancedConstantUpdate.inc"
#include "D3D11InstancedDomainRefresh.inc"
};
int TestLocalInstancedWorldAdmission()
{
    RestoreControls restore;const unsigned int masked=(restore.saved & ~0xe07fU)|0x1f80U;
    _mm_setcsr(masked);int result=0;
    for(unsigned int arena=0;arena<2;++arena) {
        AdmissionUpdateFixture f(arena!=0);LegacyLogicalState state;state.constants.world.values[0]=2;
        result|=Check(f.validateOriginalInstancedWorld(state.constants.world),"real validator mints positive local admission");
        unsigned int validated=f.validations;
        {
            const Fixture::InstancedWorldAdmission proof(state.constants.world,_mm_getcsr() & ~0x3fU);
            result|=Check(f.updateTransformConstants(state,3,0,0,true,&proof)==S_OK && f.validations==validated &&
                f.m_transformConstantsForInstancing,"matching original address/fresh controls skip duplicate updater validation");
        }
        LegacyLogicalState equalCopy=state;
        {
            const Fixture::InstancedWorldAdmission wrongAddress(equalCopy.constants.world,_mm_getcsr() & ~0x3fU);
            result|=Check(f.updateTransformConstants(state,3,0,0,true,&wrongAddress)==S_OK && f.validations==++validated,
                "equal World bytes at a different address require full original updater validation");
        }
        {
            const Fixture::InstancedWorldAdmission oldControls(state.constants.world,_mm_getcsr() & ~0x3fU);
            _mm_setcsr(masked ^ 0x2000U);
            result|=Check(f.updateTransformConstants(state,3,0,0,true,&oldControls)==S_OK && f.validations==++validated &&
                f.m_instancedPublicationControls==(_mm_getcsr() & ~0x3fU),"changed fresh controls reject prior local proof and validate current World");
        }
        _mm_setcsr(masked);
        result|=Check(f.updateTransformConstants(state,3,0,0,true)==S_OK && f.validations==++validated,
            "null proof keeps complete original validator path");
        {
            const Fixture::InstancedWorldAdmission wrongControls(state.constants.world,(masked ^ 0x4000U));
            result|=Check(f.updateTransformConstants(state,3,0,0,true,&wrongControls)==S_OK && f.validations==++validated,
                "forged mismatched control value cannot certify admission");
        }
        {
            const Fixture::InstancedWorldAdmission maskedProof(state.constants.world,masked);
            _mm_setcsr(masked & ~0x80U);
            result|=Check(f.updateTransformConstants(state,3,0,0,true,&maskedProof)==S_OK && f.validations==validated &&
                !f.m_transformConstantsForInstancing,"unmasked controls reject proof and preserve original ordinary fallback, not instanced admission");
        }
        _mm_setcsr(masked);
        result|=Check(f.updateTransformConstants(state,3,0,0,true)==S_OK && f.validations==++validated,"restore canonical with no local proof");
        memcpy(f.m_cachedLegacyState,&state,sizeof(state));f.m_cachedLegacyStateValid=true;
        f.m_cachedLegacyVertexLayoutFlags=3;f.m_cachedLegacyTexturePresenceMask=0;f.m_transformConstantsChanged=true;
        result|=Check(f.refreshTransformConstantsForDraw(true)==RENDER_RESULT_OK && f.validations==++validated,
            "actual draw refresh passes no proof and invokes the full original validator");
        LegacyLogicalState invalid=state;invalid.constants.world.values[0]=0;
        {
            const Fixture::InstancedWorldAdmission other(state.constants.world,masked);
            const unsigned int maps=f.context.maps;
            result|=Check(f.updateTransformConstants(invalid,3,0,0,true,&other)==E_INVALIDARG && f.validations==++validated &&
                f.context.maps==maps,"wrong address cannot hide a singular different original World");
        }
        result|=Check(f.validateOriginalInstancedWorld(state.constants.world),"new real admission after rejected different World");
        validated=f.validations;state.constants.view.values[12]=.25f;f.context.failMap=true;
        const LegacyTransformConstants before=f.m_lastTransformConstants;
        {
            const Fixture::InstancedWorldAdmission proof(state.constants.world,masked);
            result|=Check(FAILED(f.updateTransformConstants(state,3,0,0,true,&proof)) && f.validations==validated &&
                !f.m_transformConstantsValid && !f.m_instancedReuseLayoutValid &&
                memcmp(&before,&f.m_lastTransformConstants,sizeof(before))==0,
                "local proof cannot publish failed ring/arena Map or change last successful bytes");
        }
        f.context.failMap=false;
        result|=Check(f.validateOriginalInstancedWorld(state.constants.world),"retry obtains fresh real admission after failed publication");
        validated=f.validations;
        {
            const Fixture::InstancedWorldAdmission proof(state.constants.world,masked);
            result|=Check(f.updateTransformConstants(state,3,0,0,true,&proof)==S_OK && f.validations==validated,
                "fresh scoped proof retries failed publication without duplicate validation");
        }
        state.constants.view.values[12]=.5f;f.context.nullMap=true;
        result|=Check(f.validateOriginalInstancedWorld(state.constants.world),"admit before null ring/arena Map test");
        validated=f.validations;
        {
            const Fixture::InstancedWorldAdmission proof(state.constants.world,masked);
            result|=Check(FAILED(f.updateTransformConstants(state,3,0,0,true,&proof)) && f.validations==validated &&
                !f.m_transformConstantsValid && !f.m_instancedReuseLayoutValid,"null Map cannot publish a local proof");
        }
        f.context.nullMap=false;
    }
    return result;
}

class ForeignOwnerFixture : public Fixture
{
public:
    bool isOwner() const { return false; }
#include "D3D11DomainSetterPrefix.inc"
        return RENDER_RESULT_UNSUPPORTED; // Same omitted general pipeline boundary.
    }
};
class ViewRouteFixture : public Fixture
{
public:
    ViewRouteFixture(bool arena) : Fixture(arena), ordinaryCalls(0), canonicalCalls(0) {}
    unsigned int ordinaryCalls, canonicalCalls;
    RenderResult setLegacyStateForLayout(const LegacyLogicalState &state,
        const LegacyVertexLayout &layout, unsigned int mask)
    { ++ordinaryCalls; return Fixture::setLegacyStateForLayout(state,layout,mask); }
    RenderResult setLegacyStateForLayoutInternal(const LegacyLogicalState &state,
        const LegacyVertexLayout &layout, unsigned int mask, bool instanced, const InstancedWorldAdmission *admission = 0)
    { if(instanced) ++canonicalCalls; return Fixture::setLegacyStateForLayoutInternal(state,layout,mask,instanced,admission); }
#include "D3D11InstancedDomainSetter.inc"
};
void ConfigureViewRouteFixture(ViewRouteFixture &f, const LegacyLogicalState &state,
    const LegacyVertexLayout &layout)
{
    f.m_frameOpen=true;f.m_pipelineStateValid=true;f.m_pipelineBound=true;
    f.m_cachedLegacyPipelineValid=true;f.m_cachedLegacyStateValid=false;
    f.m_cachedLegacyVertexFormat=RENDER_VERTEX_POSITION3_NORMAL_COLOR_TEX1;
    f.m_cachedLegacyTexturePresenceMask=0;f.m_boundCubeTextureMask=f.m_boundSignedTextureMask=0;
    f.m_cachedLegacyCubeTextureMask=f.m_cachedLegacySignedTextureMask=0;
    f.m_cachedLegacyVertexLayoutFlags=LegacyVertexLayoutFlags(layout);
    f.m_hasVertexLayoutFlagsOverride=true;f.m_vertexLayoutFlagsOverride=LegacyVertexLayoutFlags(layout);
    f.m_hasInputLayoutOverride=true;
    f.m_inputLayoutOverride=f.m_cachedLegacyInputLayout=
        reinterpret_cast<ID3D11InputLayout *>(static_cast<size_t>(0x1001));
    memcpy(f.m_cachedLegacyPipeline,&state.pipeline,sizeof(state.pipeline));
}
int TestViewBoundaryRoute()
{
    RestoreControls restore;const unsigned int masked=(restore.saved & ~0xe07fU)|0x1f80U;
    _mm_setcsr(masked);int result=0;
    for(unsigned int arena=0;arena<2;++arena) {
        ViewRouteFixture f(arena!=0);LegacyLogicalState state;state.constants.world.values[0]=2;
        const LegacyVertexLayout layout=Fixture::rigidVertexLayout();ConfigureViewRouteFixture(f,state,layout);
        result|=Check(f.setLegacyStateForInstancedDraw(state,layout,0)==RENDER_RESULT_OK &&
            f.m_transformConstantsForInstancing && f.m_instancedReuseLayoutValid,"actual hook seeds canonical before View boundary");
        unsigned int maps=f.context.maps,ordinary=f.ordinaryCalls;
        unsigned char acceptedPipeline[sizeof(state.pipeline)];memcpy(acceptedPipeline,f.m_cachedLegacyPipeline,sizeof(acceptedPipeline));
        const unsigned int acceptedFlags=f.m_cachedLegacyVertexLayoutFlags;
        ID3D11InputLayout *const acceptedLayout=f.m_cachedLegacyInputLayout;
        state.constants.world.values[12]=3; // Both permitted matrix regions differ on the fast ordinary call.
        state.constants.view.values[0]=.75f;
        result|=Check(f.setLegacyStateForInstancedDraw(state,layout,0)==RENDER_RESULT_OK &&
            f.ordinaryCalls==ordinary && f.context.maps==maps+1 && !f.m_transformConstantsForInstancing &&
            f.m_instancedReuseLayoutValid && !f.m_transformConstantsOrdinaryRestorePending &&
            memcmp(f.m_lastTransformConstants.world,state.constants.world.values,sizeof(state.constants.world.values))==0 &&
            memcmp(f.m_lastTransformConstants.view,state.constants.view.values,sizeof(state.constants.view.values))==0,
            "actual current-certified View directly publishes ordinary b0 without repeated setter checks");
        result|=Check(memcmp(f.m_cachedLegacyState,&state,sizeof(state))==0 &&
            memcmp(f.m_cachedLegacyPipeline,acceptedPipeline,sizeof(acceptedPipeline))==0 &&
            f.m_cachedLegacyVertexLayoutFlags==acceptedFlags && f.m_cachedLegacyInputLayout==acceptedLayout &&
            f.m_cachedLegacyTexturePresenceMask==0 && f.m_cachedLegacyCubeTextureMask==0 && f.m_cachedLegacySignedTextureMask==0,
            "fast ordinary success commits exact logical World/View only with proven metadata unchanged");
        maps=f.context.maps;
        result|=Check(f.refreshTransformConstantsForDraw(f.m_transformConstantsForInstancing)==RENDER_RESULT_OK &&
            f.context.maps==maps && !f.m_transformConstantsForInstancing,"old instanced draw refresh accepts valid ordinary-domain View b0");
        state.constants.view.values[0]=.5f;ordinary=f.ordinaryCalls;
        result|=Check(f.setLegacyStateForInstancedDraw(state,layout,0)==RENDER_RESULT_OK &&
            f.ordinaryCalls==ordinary && !f.m_transformConstantsForInstancing,"repeated current-certified camera changes call real ordinary pack without setter wrapper");
        state.constants.world.values[12]=4;maps=f.context.maps;
        result|=Check(f.setLegacyStateForInstancedDraw(state,layout,0)==RENDER_RESULT_OK &&
            f.context.maps==maps+1 && f.m_transformConstantsForInstancing,"stable View first changed World republishes canonical once");
        state.constants.world.values[12]=8;maps=f.context.maps;f.context.failMap=true;
        result|=Check(f.setLegacyStateForInstancedDraw(state,layout,0)==RENDER_RESULT_OK &&
            f.context.maps==maps && f.m_transformConstantsForInstancing,"later World reuses canonical even with Map failure armed");
        f.context.failMap=false;state.constants.view.values[0]=.4f;maps=f.context.maps;
        unsigned char beforeFailure[sizeof(state)];memcpy(beforeFailure,f.m_cachedLegacyState,sizeof(beforeFailure));
        f.context.failMap=true;
        result|=Check(f.setLegacyStateForInstancedDraw(state,layout,0)==RENDER_RESULT_FAILED &&
            f.context.maps==maps+1 && !f.m_instancedReuseLayoutValid && !f.m_transformConstantsValid &&
            f.m_transformConstantsOrdinaryRestorePending && memcmp(beforeFailure,f.m_cachedLegacyState,sizeof(beforeFailure))==0,
            "failed fast ordinary View leaves logical cache unchanged and preserves eager ordinary retry");
        f.context.failMap=false;ordinary=f.ordinaryCalls;
        result|=Check(f.setLegacyStateForInstancedDraw(state,layout,0)==RENDER_RESULT_OK &&
            f.ordinaryCalls==ordinary+1 && !f.m_transformConstantsForInstancing && f.m_instancedReuseLayoutValid &&
            !f.m_transformConstantsOrdinaryRestorePending,"View retry runs admitted ordinary setter and publishes proof only after success");
        state.constants.view.values[0]=.3f;f.context.nullMap=true;
        result|=Check(f.setLegacyStateForInstancedDraw(state,layout,0)==RENDER_RESULT_FAILED &&
            !f.m_instancedReuseLayoutValid && !f.m_transformConstantsValid,"null View fallback Map cannot publish admission");
        f.context.nullMap=false;
        result|=Check(f.setLegacyStateForInstancedDraw(state,layout,0)==RENDER_RESULT_OK,"null View fallback retries through real ordinary publication");
        state.constants.view.values[0]=.2f;_mm_setcsr(masked^0x2000U);
        result|=Check(f.setLegacyStateForInstancedDraw(state,layout,0)==RENDER_RESULT_OK &&
            !f.m_transformConstantsForInstancing && f.m_instancedPublicationControls==(_mm_getcsr() & ~0x3fU),
            "ordinary fallback stores CURRENT validated controls after control-mode change");
        _mm_setcsr(masked & ~0x80U);ordinary=f.ordinaryCalls;
        result|=Check(f.setLegacyStateForInstancedDraw(state,layout,0)==RENDER_RESULT_OK &&
            f.ordinaryCalls==ordinary+1 && !f.m_instancedReuseLayoutValid,"unmasked hook retains original ordinary fallback and revokes proof");
        _mm_setcsr(masked);
        result|=Check(f.setLegacyStateForInstancedDraw(state,layout,0)==RENDER_RESULT_OK,"recover masked canonical entry");
        result|=Check(f.setLegacyStateForLayout(state,layout,0)==RENDER_RESULT_OK && !f.m_instancedReuseLayoutValid,
            "external ordinary setter revokes certificate after valid owner/frame admission");
        LegacyLogicalState invalid=state;invalid.constants.world.values[12]=FloatBits(0x7fc00042U);
        result|=Check(f.setLegacyStateForLayout(invalid,layout,0)==RENDER_RESULT_OK && !f.m_instancedReuseLayoutValid,
            "external ordinary invalid original world never acquires instanced admission");
        maps=f.context.maps;ordinary=f.ordinaryCalls;
        result|=Check(f.setLegacyStateForInstancedDraw(invalid,layout,0)==RENDER_RESULT_INVALID_ARGUMENT &&
            f.context.maps==maps && f.ordinaryCalls==ordinary,"eligible invalid original world is rejected before View fallback");
        LegacyVertexLayout badLayout=layout;badLayout.elements[0].byteOffset+=4;
        result|=Check(f.setLegacyStateForInstancedDraw(invalid,badLayout,0)==RENDER_RESULT_UNSUPPORTED &&
            f.context.maps==maps,"unsupported layout wins over invalid original world");
        invalid.pipeline.alphaTestEnable=true;
        result|=Check(f.setLegacyStateForInstancedDraw(invalid,layout,0)==RENDER_RESULT_UNSUPPORTED &&
            f.context.maps==maps,"unsupported pipeline wins over invalid original world");
        f.m_frameOpen=false;f.m_instancedReuseLayoutValid=true;
        result|=Check(f.setLegacyState(state,RENDER_VERTEX_POSITION3_NORMAL_COLOR_TEX1,0)==RENDER_RESULT_INVALID_ARGUMENT &&
            f.m_instancedReuseLayoutValid,"invalid frame cannot mutate external ordinary proof before owner/frame check");
    }
    ForeignOwnerFixture foreign;foreign.m_frameOpen=true;foreign.m_instancedReuseLayoutValid=true;
    LegacyLogicalState foreignState;
    result|=Check(foreign.setLegacyState(foreignState,RENDER_VERTEX_POSITION3_NORMAL_COLOR_TEX1,0)==RENDER_RESULT_INVALID_ARGUMENT &&
        foreign.m_instancedReuseLayoutValid,"actual foreign-owner ordinary setter cannot mutate admission proof");
    // Combined View+other-live changes must stay on the established canonical path.
    for(unsigned int variant=0;variant<3;++variant) {
        ViewRouteFixture f(false);LegacyLogicalState state;const LegacyVertexLayout layout=Fixture::rigidVertexLayout();
        ConfigureViewRouteFixture(f,state,layout);
        result|=Check(f.setLegacyStateForInstancedDraw(state,layout,0)==RENDER_RESULT_OK,"seed combined View miss");
        state.constants.view.values[0]=.75f;unsigned int mask=0;
        if(variant==0) state.constants.material.diffuse.x=.25f;
        if(variant==1) state.constants.textureTransforms[0].values[12]=.25f;
        if(variant==2) {mask=1;state.texturePresenceMask=1;}
        const unsigned int ordinary=f.ordinaryCalls,canonical=f.canonicalCalls;
        const RenderResult code=f.setLegacyStateForInstancedDraw(state,layout,mask);
        result|=Check(f.ordinaryCalls==ordinary && f.canonicalCalls==canonical+1 &&
            (variant==2 ? code==RENDER_RESULT_UNSUPPORTED : code==RENDER_RESULT_OK && f.m_transformConstantsForInstancing),
            "combined View/live miss never selects ordinary View-only shortcut (general fixture branch remains unsupported)");
    }
    return result;
}

class ReuseAdmissionFixture : public Fixture
{
public:
    ReuseAdmissionFixture(bool arena) : Fixture(arena), validations(0) {}
    unsigned int validations;
    bool validateOriginalInstancedWorld(const RenderMatrix4 &world)
    { ++validations; return Fixture::validateOriginalInstancedWorld(world); }
#include "D3D11InstancedReuse.inc"
};

int TestCanonicalReuse()
{
    RestoreControls restore;
    const unsigned int masked=(restore.saved & ~0xe07fU) | 0x1f80U;
    _mm_setcsr(masked);
    int result=0;
    for(unsigned int arena=0;arena<2;++arena) {
        ReuseAdmissionFixture f(arena!=0);LegacyLogicalState state;LegacyVertexLayout layout=Fixture::rigidVertexLayout();
        state.constants.world.values[0]=2;
        f.m_frameOpen=true;f.m_pipelineStateValid=true;f.m_pipelineBound=true;
        f.m_cachedLegacyPipelineValid=true;
        memcpy(f.m_cachedLegacyPipeline,&state.pipeline,sizeof(state.pipeline));
        result|=Check(f.setLegacyStateInternal(state,RENDER_VERTEX_POSITION3_NORMAL_COLOR_TEX1,0,true)==RENDER_RESULT_OK,
            "seed successful canonical physical publication");
        memcpy(&f.m_instancedReuseLayout,&layout,sizeof(layout));
        f.m_instancedReuseLayoutValid=true;f.m_instancedReuseInputLayout=f.m_cachedLegacyInputLayout;
        const LegacyTransformConstants published=f.m_lastTransformConstants;
        unsigned int maps=f.context.maps;
        state.constants.world.values[12]=4;
        result|=Check(f.tryReuseInstancedConstants(state,layout,0) && f.validations==1 &&
            f.context.maps==maps && memcmp(&published,&f.m_lastTransformConstants,sizeof(published))==0 &&
            memcmp(f.m_cachedLegacyState,&state,sizeof(state))==0,
            "admitted world-only hit advances logical snapshot with no b0 work");
        const unsigned int validated=f.validations;
        result|=Check(f.tryReuseInstancedConstants(state,layout,0) && f.validations==validated,
            "unchanged successful physical world proof avoids admission helper entirely");
        _mm_setcsr(masked | 0x3fU);f.context.failMap=true;
        result|=Check(f.tryReuseInstancedConstants(state,layout,0) && f.validations==validated && f.context.maps==maps,
            "sticky status bits preserve physical proof; armed Map failure cannot affect actual no-upload reuse");
        f.context.failMap=false;_mm_setcsr(masked);
        bool admitted=false;
        state.constants.view.values[0]=.75f;
        result|=Check(!f.tryReuseInstancedConstants(state,layout,0,&admitted) && admitted && f.validations==validated,
            "changed live View retains eligibility proof but requires regular setter publication");
        state.constants.view.values[0]=1;
        admitted=false;
        result|=Check(!f.tryReuseInstancedConstants(state,layout,1,&admitted) && admitted && f.validations==validated,
            "changed texture presence retains eligibility proof but cannot skip publication");
        LegacyVertexLayout equivalent=layout;
        equivalent.elements[LegacyVertexLayout::MAX_ELEMENT_COUNT-1].byteOffset=99;
        unsigned char *layoutBytes=reinterpret_cast<unsigned char *>(&equivalent);
        for(size_t i=offsetof(LegacyVertexLayout,preTransformed)+sizeof(equivalent.preTransformed);
            i<offsetof(LegacyVertexLayout,elements);++i) layoutBytes[i]^=0x5a;
        result|=Check(f.tryReuseInstancedConstants(state,equivalent,0) && f.validations==validated,
            "unused layout elements and padding intentionally preserve semantic layout hit");
        LegacyVertexLayout unsupported=layout;unsupported.elements[0].byteOffset+=4;admitted=true;
        result|=Check(!f.tryReuseInstancedConstants(state,unsupported,0,&admitted) && !admitted && f.validations==validated,
            "changed declared layout cannot inherit rigid admission");
        for(unsigned int variant=0;variant<5;++variant) {
            LegacyLogicalState bad=state;
            if(variant==0) bad.pipeline.alphaTestEnable=true;
            if(variant==1) bad.pipeline.blend.blendEnable=true;
            if(variant==2) bad.pipeline.nPatchEnable=true;
            if(variant==3) bad.pipeline.vertexProgram=RENDER_LEGACY_VERTEX_WATER_SEA;
            if(variant==4) bad.pipeline.pixelProgram=RENDER_LEGACY_PIXEL_WATER_SEA;
            bad.constants.world.values[0]=FloatBits(0x7fc00042U);admitted=true;
            result|=Check(!f.tryReuseInstancedConstants(bad,layout,0,&admitted) && !admitted && f.validations==validated,
                "unsupported pipeline cannot inherit eligibility or validate invalid world early");
        }
        LegacyLogicalState invalidWorld=state;invalidWorld.constants.world.values[0]=FloatBits(0x7fc00042U);
        admitted=true;
        result|=Check(!f.tryReuseInstancedConstants(invalidWorld,layout,0,&admitted) && !admitted && f.validations==validated+1 &&
            f.context.maps==maps,"eligible invalid original world actually validates and cannot reuse or certify");
        f.m_rigidSupport=false;admitted=true;
        result|=Check(!f.tryReuseInstancedConstants(state,layout,0,&admitted) && !admitted,
            "lost rigid support cannot use physical proof");f.m_rigidSupport=true;
        state.constants.view.values[0]=.75f;
        result|=Check(!f.tryReuseInstancedConstants(state,layout,0),"live View change misses reuse");
        state.constants.view.values[0]=1;
        result|=Check(!f.tryReuseInstancedConstants(state,layout,1),"texture presence change misses reuse");
        f.m_boundCubeTextureMask=1;
        result|=Check(!f.tryReuseInstancedConstants(state,layout,0),"cube mask change misses reuse");
        f.m_boundCubeTextureMask=0;
        f.m_boundSignedTextureMask=1;
        result|=Check(!f.tryReuseInstancedConstants(state,layout,0),"signed mask change misses reuse");
        f.m_boundSignedTextureMask=0;
        f.m_cachedLegacyInputLayout=reinterpret_cast<ID3D11InputLayout *>(static_cast<size_t>(3));
        result|=Check(!f.tryReuseInstancedConstants(state,layout,0),"physical layout identity change misses reuse");
        f.m_cachedLegacyInputLayout=f.m_instancedReuseInputLayout;
        _mm_setcsr(masked ^ 0x2000U);
        f.validateOriginalInstancedWorld(state.constants.world);
        result|=Check(!f.tryReuseInstancedConstants(state,layout,0),"new mathematical memo cannot forge publication controls");
        _mm_setcsr(masked);
        f.m_transformConstantsChanged=true;
        result|=Check(!f.tryReuseInstancedConstants(state,layout,0),"dirty canonical publication cannot shortcut");
        f.m_transformConstantsChanged=false;f.m_transformConstantsOrdinaryRestorePending=true;
        result|=Check(!f.tryReuseInstancedConstants(state,layout,0),"ordinary restoration pending cannot shortcut");
        f.m_transformConstantsOrdinaryRestorePending=false;f.m_pipelineBound=false;
        result|=Check(!f.tryReuseInstancedConstants(state,layout,0),"unbound pipeline cannot shortcut");
        f.m_pipelineBound=true;state.constants.world.values[0]=0;
        result|=Check(!f.validateOriginalInstancedWorld(state.constants.world),"singular world still rejected before hook reuse");
        state.constants.world.setIdentity();f.m_frameOpen=true;
        f.context.failMap=true;
        result|=Check(f.setLegacyState(state,RENDER_VERTEX_POSITION3_NORMAL_COLOR_TEX1,0)==RENDER_RESULT_FAILED &&
            f.m_transformConstantsOrdinaryRestorePending,"failed ordinary restoration sets pending latch");
        f.m_frameOpen=false;
        result|=Check(f.beginFrame()==RENDER_RESULT_OK && !f.m_transformConstantsOrdinaryRestorePending &&
            !f.m_transformConstantsValid && !f.m_instancedReuseLayoutValid,
            "next frame drops failed old physical publication and its restore latch together");
        f.context.failMap=false;f.m_pipelineStateValid=true;f.m_pipelineBound=true;f.m_cachedLegacyPipelineValid=true;
        memcpy(f.m_cachedLegacyPipeline,&state.pipeline,sizeof(state.pipeline));
        result|=Check(f.setLegacyStateInternal(state,RENDER_VERTEX_POSITION3_NORMAL_COLOR_TEX1,0,true)==RENDER_RESULT_OK,
            "next frame canonical publication succeeds after old ordinary failure");
        memcpy(&f.m_instancedReuseLayout,&layout,sizeof(layout));f.m_instancedReuseInputLayout=f.m_cachedLegacyInputLayout;
        f.m_instancedReuseLayoutValid=true;state.constants.world.values[12]=8;
        result|=Check(f.tryReuseInstancedConstants(state,layout,0),"next frame canonical world reuse is not blocked by stale restore latch");
        f.context.failMap=true;maps=f.context.maps;
        result|=Check(f.setLegacyState(state,RENDER_VERTEX_POSITION3_NORMAL_COLOR_TEX1,0)==RENDER_RESULT_FAILED &&
            f.context.maps==maps+1 && f.m_transformConstantsOrdinaryRestorePending,
            "new frame ordinary restoration still eagerly Maps and preserves failed retry");
        f.context.failMap=false;
        f.invalidateContextBindings();
        result|=Check(!f.m_instancedReuseLayoutValid && !f.tryReuseInstancedConstants(state,layout,0),
            "full invalidation revokes reuse key");
    }
    return result;
}

int main()
{
	SetEnvironmentVariableA("RTS_FRAME_TIMING_DIR", NULL);
	int result = TestKeysAndBytes() | TestFloatingPointDomain() |
		TestPublicationAndReset() | TestWorldViewIsolation() | TestArenaPublicationAndReset() | TestInstancedConstantDomain() | TestOrdinaryStateRestoreRetry() | TestOriginalWorldAdmissionMemo() | TestCanonicalReuse() | TestViewBoundaryRoute() | TestLocalInstancedWorldAdmission();
	if (!result) printf("D3D11 normal matrix cache contracts passed.\n");
	return result;
}
