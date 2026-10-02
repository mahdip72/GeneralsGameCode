/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2026 TheSuperHackers
** SPDX-License-Identifier: GPL-3.0-or-later
*/

#ifndef RTS_WW3D2_NATIVEW3DSORTING_H
#define RTS_WW3D2_NATIVEW3DSORTING_H

#include "Renderer/NativeW3DRenderer.h"
#include "Renderer/NativeW3DResources.h"
#include "Renderer/RenderGameClient.h"

#include <stddef.h>

namespace rts
{
namespace render
{

// Output affinity and owning resource references for one deferred pass. A
// content lease is deliberately not substituted for these allocation pins.
struct NativeSortedPass
{
	NativeSortedPass() : captured(false), identity(0), target(), viewport(),
		textures(), color(), depth() {}
	bool captured;
	NativeW3DSubmissionSequence identity;
	RenderTargetBinding target;
	RenderViewport viewport;
	NativeW3DTextureRetention textures[LEGACY_TEXTURE_STAGE_COUNT];
	NativeW3DTextureRetention color;
	NativeW3DTextureRetention depth;
};

// A draw group is one contiguous run of triangles that originated from the
// same legacy sorting node.  The packet deliberately contains no temporary
// resource handles; the sink binds one shared transient vertex/index pair for
// the complete batch before submitting each state group.
struct NativeSortedDraw
{
	LegacyLogicalState state;
	NativeDrawPacket packet;
};

// NativeSortingRenderer owns the only deferred sorted-geometry queue.  The
// sink is intentionally narrower than NativeW3D2 so deterministic ordering
// can be tested without a device and so the sorter cannot call back through
// DrawGameSortedTriangles/FlushGameSortedTriangles.
class NativeSortedGeometrySink
{
public:
	virtual ~NativeSortedGeometrySink() {}
	// The owner may acknowledge only a contiguous prefix of draws.  A non-OK
	// result may therefore still carry a positive prefix count; the sorter
	// retires exactly that prefix and keeps every later draw for retry.
	virtual RenderResult SubmitNativeSortedBatch(
		const NativeSortedDraw *draws, unsigned int drawCount,
		const void *vertexData, size_t vertexBytes,
		const void *indexData, size_t indexBytes,
		unsigned int *submittedDrawCount) = 0;
	virtual RenderResult SubmitNativeSortedPassBatch(const NativeSortedPass &,
		const NativeSortedDraw *draws, unsigned int drawCount,
		const void *vertexData, size_t vertexBytes,
		const void *indexData, size_t indexBytes,
		unsigned int *submittedDrawCount)
	{
		return SubmitNativeSortedBatch(draws, drawCount, vertexData, vertexBytes,
			indexData, indexBytes, submittedDrawCount);
	}
};

class NativeSortingRenderer
{
public:
	NativeSortingRenderer();
	~NativeSortingRenderer();

	NativeSortingRenderer(const NativeSortingRenderer &) = delete;
	NativeSortingRenderer &operator=(const NativeSortingRenderer &) = delete;

	// The byte views are already narrowed to packet.minimumVertexIndex and
	// packet.startIndex by the caller: vertexData contains vertexCount records
	// beginning at the selected source vertex, and indexData contains
	// indexCount R16 entries beginning at the selected source index.  The
	// packet retains those logical source offsets for the sorting kernel.
	// Queue remains admitted after failure. Once a Flush acknowledges any
	// draw (including earlier successful chunks), its original submissions
	// form a sealed retry cohort. Later queued work is owned separately and
	// drains only after that cohort; it is not globally re-sorted ahead of an
	// already emitted prefix. A zero-acknowledgement failure leaves admission
	// in the same sortable cohort because no prefix has been emitted.
	RenderResult Queue(const LegacyLogicalState &state,
		const NativeDrawPacket &packet, const void *vertexData,
		size_t vertexBytes, const void *indexData, size_t indexBytes,
		const GameBoundingSphere *boundingSphere,
		const NativeSortedPass *pass = 0, bool awaitMatchingBegin = false);
	// Native no-frame admissions keep their captured bytes/state/viewport, but
	// cannot draw before a successful Begin for this exact output. Activation
	// commits only after target/viewport/clear admission succeeds. Generic Queue
	// callers remain immediately active. Inactive nodes are never rearranged.
	void ActivateMatchingPass(const RenderTargetBinding &target,
		NativeW3DSubmissionSequence identity);
	// Distinct contiguous captured passes are never co-sorted. The optional
	// single-cohort drain retries an older failed pass before a new clear without
	// prematurely drawing newly admitted tail work ahead of that clear.
	RenderResult Flush(NativeSortedGeometrySink &sink,
		bool firstCohortOnly = false);

	// Teardown is the one intentional discard point.  A failed Flush never
	// calls this method and therefore keeps every command available for retry.
	void Clear();
	bool Empty() const;

	// Kept public only as an incomplete type so the implementation can keep
	// all queue storage out of the product header.  Callers cannot construct or
	// inspect it; ownership remains with this class.
	struct Impl;

private:
	Impl *m_impl;
};

}
}

#endif
