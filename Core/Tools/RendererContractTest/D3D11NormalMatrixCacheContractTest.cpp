// Exact production-method fixture, as in D3D11BufferDiscardContractTest.
// This tests bytes/publication; it is not GPU or workload performance evidence.
#include "Renderer/RendererDevice.h"
#include "Renderer/NormalMatrixSubgroupCache.h"
#include "TransformConstantArenaPolicy.h"
#include "Lib/FrameTimingDiagnostics.h"
#include <d3d11.h>
#include <float.h>
#include <stdio.h>
#include <string.h>
#include <xmmintrin.h>

namespace {
using namespace rts::render;
typedef detail::NormalMatrixSubgroupCache Cache;
typedef detail::TransformConstantArenaPolicy Arena;
const unsigned int TRANSFORM_CONSTANT_BUFFER_COUNT = 64;
const unsigned int LEGACY_VERTEX_LAYOUT_PRETRANSFORMED = 0x80000000U;
#include "D3D11NormalConstants.inc"
#include "D3D11NormalMultiply.inc"

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
	Fixture(bool arena = false) : m_context(&context), m_context1(arena ? &context : 0),
		m_transformArena(arena ? reinterpret_cast<ID3D11Buffer *>(static_cast<size_t>(0x1000)) : 0),
		m_transformConstantCursor(0),
		m_transformConstantsValid(false), m_transformConstantsChanged(false),
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
#include "D3D11NormalPublication.inc"
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
	bool m_transformConstantsValid, m_transformConstantsChanged;
	unsigned int m_boundSignedTextureMask;
	float m_viewportX, m_viewportY, m_viewportWidth, m_viewportHeight;
	unsigned int m_width, m_height;
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
int main()
{
	SetEnvironmentVariableA("RTS_FRAME_TIMING_DIR", NULL);
	int result = TestKeysAndBytes() | TestFloatingPointDomain() |
		TestPublicationAndReset() | TestWorldViewIsolation() | TestArenaPublicationAndReset();
	if (!result) printf("D3D11 normal matrix cache contracts passed.\n");
	return result;
}
