#include "Renderer/RenderSubmissionPolicy.h"
#include "Renderer/LegacyBridgeValidation.h"
#if __cplusplus >= 201703L
#include "Renderer/LegacyBridgeCache.h"
#endif
#include "Renderer/WindowPresentation.h"

#include <stdio.h>

namespace
{
int check(bool condition, const char *testName, const char *expression)
{
	if (!condition)
	{
		fprintf(stderr, "%s: %s\n", testName, expression);
		return 1;
	}
	return 0;
}

#define CHECK(testName, expression) \
	do { if (check((expression), testName, #expression) != 0) return 1; } while (0)

int testD3D11OwnsOperationalTarget()
{
	const rts::render::RenderSubmissionDecision decision =
		rts::render::ChooseVisibleSubmissionBackend(true, true);
	CHECK("operational D3D11 target", decision.backend ==
		rts::render::RENDER_SUBMISSION_D3D11);
	CHECK("operational D3D11 target has one owner", !decision.submitLegacy &&
		decision.submitD3D11);
	CHECK("operational D3D11 target has exactly one visible owner",
		(static_cast<unsigned int>(decision.submitLegacy) +
			static_cast<unsigned int>(decision.submitD3D11)) == 1U);
	CHECK("D3D11 failure is observable", decision.d3d11FailureIsTerminal);
	return 0;
}

int testLegacyFallbackIsExplicit()
{
	const rts::render::RenderSubmissionDecision inactive =
		rts::render::ChooseVisibleSubmissionBackend(false, false);
	CHECK("inactive D3D11 fallback", inactive.backend ==
		rts::render::RENDER_SUBMISSION_LEGACY);
	CHECK("inactive D3D11 legacy owner", inactive.submitLegacy &&
		!inactive.submitD3D11 && !inactive.d3d11FailureIsTerminal);
	CHECK("inactive D3D11 target has exactly one visible owner",
		(static_cast<unsigned int>(inactive.submitLegacy) +
			static_cast<unsigned int>(inactive.submitD3D11)) == 1U);

	const rts::render::RenderSubmissionDecision transitioning =
		rts::render::ChooseVisibleSubmissionBackend(true, false);
	CHECK("transitioning D3D11 fallback", transitioning.backend ==
		rts::render::RENDER_SUBMISSION_UNAVAILABLE);
	CHECK("transitioning D3D11 has no hidden owner", !transitioning.submitLegacy &&
		!transitioning.submitD3D11);
	CHECK("transitioning D3D11 target has no visible owner",
		(static_cast<unsigned int>(transitioning.submitLegacy) +
			static_cast<unsigned int>(transitioning.submitD3D11)) == 0U);
	CHECK("transitioning D3D11 failure is observable",
		transitioning.d3d11FailureIsTerminal);
	return 0;
}

int testPresentationOwnershipIsLatched()
{
	CHECK("legacy presentation is allowed before D3D11 starts",
		rts::render::ShouldPresentLegacyFrame(false, false));
	CHECK("legacy presentation is suppressed while D3D11 owns the frame",
		!rts::render::ShouldPresentLegacyFrame(true, true));
	CHECK("legacy presentation stays suppressed after D3D11 teardown",
		!rts::render::ShouldPresentLegacyFrame(true, false));
	CHECK("ambiguous active bridge does not expose legacy presentation",
		!rts::render::ShouldPresentLegacyFrame(false, true));
	return 0;
}

int testLegacyPreTransformBackendGate()
{
	CHECK("legacy pre-transform remains available",
		rts::render::UseLegacyPreTransformVertexPath(false));
	CHECK("D3D11 pre-transform is disabled",
		!rts::render::UseLegacyPreTransformVertexPath(true));
	return 0;
}

int testCaptureFrameGate()
{
	rts::render::RenderCaptureFrameGate gate;
	CHECK("capture gate starts clear", !gate.isRequested() && !gate.isArmed());
	gate.request();
	CHECK("capture gate waits for D3D11", !gate.arm(false) &&
		gate.isRequested() && !gate.isArmed());
	CHECK("capture gate arms one visible frame", gate.arm(true) &&
		gate.isArmed());
	CHECK("capture gate does not double-arm", !gate.arm(true));
	CHECK("failed frame keeps capture pending", !gate.complete(false) &&
		gate.isRequested() && !gate.isArmed());
	CHECK("capture gate retries the next frame", gate.arm(true));
	CHECK("completed frame consumes capture", gate.complete(true) &&
		!gate.isRequested() && !gate.isArmed());
	CHECK("completed capture cannot fire twice", !gate.complete(true));
	return 0;
}

int testWindowPresentationPolicy()
{
	const DWORD savedStyle = WS_OVERLAPPEDWINDOW | WS_VISIBLE;
	const DWORD savedExStyle = WS_EX_APPWINDOW | WS_EX_WINDOWEDGE;
	const RECT savedWindowRect = { 100, 200, 1380, 920 };
	const RECT monitorRect = { -1920, 0, 0, 1080 };

	const rts::render::WindowPresentationPlan fullscreen =
		rts::render::ChooseWindowPresentationPlan(true, false,
			savedStyle, savedExStyle, savedWindowRect, monitorRect);
	CHECK("D3D11 fullscreen is borderless",
		fullscreen.kind == rts::render::WINDOW_PRESENTATION_BORDERLESS_FULLSCREEN);
	CHECK("D3D11 fullscreen uses WS_POPUP",
		rts::render::IsBorderlessWindowStyle(fullscreen.style));
	CHECK("D3D11 fullscreen removes extended frame",
		(fullscreen.exStyle & WS_EX_WINDOWEDGE) == 0);
	CHECK("D3D11 fullscreen covers selected monitor",
		fullscreen.rect.left == monitorRect.left &&
		fullscreen.rect.top == monitorRect.top &&
		fullscreen.rect.right == monitorRect.right &&
		fullscreen.rect.bottom == monitorRect.bottom);
	CHECK("D3D11 fullscreen does not request exclusive mode",
		!fullscreen.usesExclusiveMode);

	const rts::render::WindowPresentationPlan windowed =
		rts::render::ChooseWindowPresentationPlan(true, true,
			savedStyle, savedExStyle, savedWindowRect, monitorRect);
	CHECK("D3D11 windowed mode is captioned",
		windowed.kind == rts::render::WINDOW_PRESENTATION_WINDOWED &&
			windowed.style == savedStyle);
	CHECK("D3D11 windowed mode restores extended style",
		windowed.exStyle == savedExStyle);
	CHECK("D3D11 windowed mode restores placement",
		windowed.rect.left == savedWindowRect.left &&
		windowed.rect.top == savedWindowRect.top &&
		windowed.rect.right == savedWindowRect.right &&
		windowed.rect.bottom == savedWindowRect.bottom);

	const rts::render::WindowPresentationPlan legacy =
		rts::render::ChooseWindowPresentationPlan(false, false,
			savedStyle, savedExStyle, savedWindowRect, monitorRect);
	CHECK("DX8 fullscreen remains exclusive",
		legacy.kind == rts::render::WINDOW_PRESENTATION_LEGACY_EXCLUSIVE_FULLSCREEN &&
			legacy.usesExclusiveMode);
	return 0;
}

int testCheckedIndexedSubmissionBounds()
{
	unsigned int index_count = 0;
	CHECK("checked triangle-list arithmetic",
		rts::render::Checked_D3D8_Primitive_Index_Count(
			rts::render::LEGACY_D3DPT_TRIANGLELIST, 2, &index_count) &&
			index_count == 6);
	CHECK("triangle-list multiplication overflow is rejected",
		!rts::render::Checked_D3D8_Primitive_Index_Count(
			rts::render::LEGACY_D3DPT_TRIANGLELIST,
			static_cast<unsigned int>(-1) / 3 + 1,
			&index_count));
	CHECK("triangle-strip addition overflow is rejected",
		!rts::render::Checked_D3D8_Primitive_Index_Count(
			rts::render::LEGACY_D3DPT_TRIANGLESTRIP,
			static_cast<unsigned int>(-1), &index_count));
	CHECK("indexed range at buffer end is valid",
		rts::render::Is_D3D8_Indexed_Range_Valid(8, 7, 1));
	CHECK("indexed range crossing buffer end is rejected",
		!rts::render::Is_D3D8_Indexed_Range_Valid(8, 7, 2));
	CHECK("indexed range with start past buffer is rejected",
		!rts::render::Is_D3D8_Indexed_Range_Valid(8, 9, 0));
	return 0;
}

int testRenderStatePublicationDisposition()
{
	CHECK("tracked fog state is deliberately irrelevant",
		rts::render::Is_D3D11_Irrelevant_Render_State(
			rts::render::LEGACY_D3DRS_FOGSTART));
	CHECK("ambient state remains relevant for neutral lighting",
		!rts::render::Is_D3D11_Irrelevant_Render_State(
			rts::render::LEGACY_D3DRS_AMBIENT));
	CHECK("water wrap state is deliberately irrelevant",
		rts::render::Is_D3D11_Irrelevant_Render_State(
			rts::render::LEGACY_D3DRS_WRAP0));
	CHECK("tracked fog enable is deliberately irrelevant",
		rts::render::Is_D3D11_Irrelevant_Render_State(
			rts::render::LEGACY_D3DRS_FOGENABLE));
	CHECK("shader-derived specular enable is deliberately irrelevant",
		rts::render::Is_D3D11_Irrelevant_Render_State(
			rts::render::LEGACY_D3DRS_SPECULARENABLE));
	CHECK("supported cull state is not deliberately irrelevant",
		!rts::render::Is_D3D11_Irrelevant_Render_State(
			rts::render::LEGACY_D3DRS_CULLMODE));
	CHECK("unsupported relevant state poisons the frame",
		rts::render::Should_Poison_D3D11_Render_State(
			rts::render::LEGACY_D3DRS_CULLMODE, false));
	CHECK("unsupported deliberately irrelevant state does not poison",
		!rts::render::Should_Poison_D3D11_Render_State(
			rts::render::LEGACY_D3DRS_FOGSTART, false));
	CHECK("shader-derived specular state does not poison",
		!rts::render::Should_Poison_D3D11_Render_State(
			rts::render::LEGACY_D3DRS_SPECULARENABLE, false));
	CHECK("ambient publication failure poisons the frame",
		rts::render::Should_Poison_D3D11_Render_State(
			rts::render::LEGACY_D3DRS_AMBIENT, false));
	CHECK("successful publication never poisons",
		!rts::render::Should_Poison_D3D11_Render_State(
			rts::render::LEGACY_D3DRS_CULLMODE, true));
	return 0;
}

int testDepthAwareRenderTargetIdentity()
{
	const void *color = reinterpret_cast<const void *>(1);
	const void *depth_a = reinterpret_cast<const void *>(2);
	const void *depth_b = reinterpret_cast<const void *>(3);
	CHECK("same color and depth binding compares equal",
		rts::render::Is_Legacy_Render_Target_Binding_Equal(
			color, depth_a, false, color, depth_a, false));
	CHECK("custom depth change invalidates same-color binding",
		!rts::render::Is_Legacy_Render_Target_Binding_Equal(
			color, depth_a, false, color, depth_b, false));
	CHECK("default depth mode invalidates same-color binding",
		!rts::render::Is_Legacy_Render_Target_Binding_Equal(
			color, depth_a, false, color, depth_a, true));
	CHECK("default depth binding remains identity-stable",
		rts::render::Is_Legacy_Render_Target_Binding_Equal(
			color, 0, true, color, depth_a, true));
	return 0;
}

#if __cplusplus >= 201703L
int testLegacyBridgeCacheIndexAndUploadPolicy()
{
	rts::render::LegacyBridgePointerIndex index;
	int source_a = 0;
	int source_b = 0;
	int source_c = 0;
	unsigned int entry_index = 0;
	CHECK("cache index inserts first source", index.Insert(&source_a, 0));
	CHECK("cache index inserts second source", index.Insert(&source_b, 1));
	CHECK("cache index inserts third source", index.Insert(&source_c, 2));
	CHECK("cache index finds source in O(1) map", index.Find(&source_b,
		&entry_index) && entry_index == 1);
	CHECK("cache index rejects missing source", !index.Find(
		reinterpret_cast<const void *>(4), &entry_index));
	CHECK("cache index erases and shifts vector indices",
		index.EraseAt(&source_b, 1) && index.Find(&source_c, &entry_index) &&
		entry_index == 1);
	CHECK("cache index keeps surviving source stable",
		index.Find(&source_a, &entry_index) && entry_index == 0);
	CHECK("cache index reinsert uses recovered tail index",
		index.Insert(&source_b, 2) && index.Find(&source_b, &entry_index) &&
		entry_index == 2);

	CHECK("typed buffer skips unchanged generation",
		!rts::render::LegacyBridgeTypedBufferNeedsUpload(7, 7));
	CHECK("typed buffer refreshes changed generation",
		rts::render::LegacyBridgeTypedBufferNeedsUpload(8, 7));
	CHECK("raw buffer skips clean source",
		!rts::render::LegacyBridgeRawBufferNeedsUpload(false));
	CHECK("raw buffer refreshes invalidated source",
		rts::render::LegacyBridgeRawBufferNeedsUpload(true));

	rts::render::LegacyBridgeCacheCounters counters;
	counters.RecordBufferLookup(true);
	counters.RecordBufferLookup(false);
	counters.RecordTextureLookup(true);
	counters.RecordTextureLookup(false);
	counters.RecordBufferUpload();
	CHECK("cache counters expose lookup hit/miss accounting",
		counters.bufferLookups == 2 && counters.bufferHits == 1 &&
		counters.textureLookups == 2 && counters.textureHits == 1 &&
		counters.bufferUploads == 1);
	return 0;
}
#endif
}

int main()
{
	if (testD3D11OwnsOperationalTarget() != 0)
		return 1;
	if (testLegacyFallbackIsExplicit() != 0)
		return 1;
	if (testPresentationOwnershipIsLatched() != 0)
		return 1;
	if (testLegacyPreTransformBackendGate() != 0)
		return 1;
	if (testCaptureFrameGate() != 0)
		return 1;
	if (testWindowPresentationPolicy() != 0)
		return 1;
	if (testCheckedIndexedSubmissionBounds() != 0)
		return 1;
	if (testRenderStatePublicationDisposition() != 0)
		return 1;
	if (testDepthAwareRenderTargetIdentity() != 0)
		return 1;
	#if __cplusplus >= 201703L
	if (testLegacyBridgeCacheIndexAndUploadPolicy() != 0)
		return 1;
	#endif
	return 0;
}
