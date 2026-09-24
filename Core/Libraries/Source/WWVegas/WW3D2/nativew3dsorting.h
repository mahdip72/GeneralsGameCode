/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2026 TheSuperHackers
** SPDX-License-Identifier: GPL-3.0-or-later
*/

#ifndef RTS_WW3D2_NATIVEW3DSORTING_H
#define RTS_WW3D2_NATIVEW3DSORTING_H

#include "Renderer/NativeW3DRenderer.h"
#include "Renderer/RenderGameClient.h"

#include <stddef.h>

namespace rts
{
namespace render
{

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
	RenderResult Queue(const LegacyLogicalState &state,
		const NativeDrawPacket &packet, const void *vertexData,
		size_t vertexBytes, const void *indexData, size_t indexBytes,
		const GameBoundingSphere *boundingSphere);
	RenderResult Flush(NativeSortedGeometrySink &sink);

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
