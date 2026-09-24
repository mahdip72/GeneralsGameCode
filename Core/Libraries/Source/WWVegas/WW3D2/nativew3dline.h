#ifndef RTS_WW3D2_NATIVEW3DLINE_H
#define RTS_WW3D2_NATIVEW3DLINE_H

#include "Renderer/NativeW3DRenderer.h"
#include "Renderer/NativeW3DResources.h"

#include <stddef.h>

namespace rts
{
namespace render
{
enum
{
	NATIVE_LINE3D_VERTEX_COUNT = 8,
	NATIVE_LINE3D_INDEX_COUNT = 36
};

// The line object is an unlit prelit box.  Keep the vertex memory layout
// explicit so the native target can use the neutral position/color input
// contract without including a legacy FVF or descriptor.
struct NativeLine3DVertex
{
	float x;
	float y;
	float z;
	unsigned int color;
};

struct NativeLine3DGeometry
{
	NativeLine3DVertex vertices[NATIVE_LINE3D_VERTEX_COUNT];
	unsigned short indices[NATIVE_LINE3D_INDEX_COUNT];
};

// GPU handles are owned by the caller's NativeW3DResources table.  The line
// object stores this state opaquely so its legacy header keeps one object
// layout in both the VC6 and native source cohorts.
struct NativeLine3DBufferSet
{
	NativeLine3DBufferSet();

	GpuHandle vertexBuffer;
	GpuHandle indexBuffer;
	// A Line3D sidecar is heap-owned by the line until its destructor or
	// assignment drops the line reference. The render context keeps a second
	// reference while the set is registered, which lets an off-owner line
	// destruction hand its handles back to the owner thread instead of losing
	// the only cleanup path with the object.
	bool lineOwned;
	bool lineReferenceActive;
	bool contextReferenceActive;
	// Set when the context is being destroyed before a non-owner thread can
	// retry Destroy. The resource table remains the physical handle owner and
	// will finish terminal cleanup; a later Line3D destructor may reclaim only
	// the metadata sidecar without dereferencing the dead context.
	bool resourceOwnerTerminal;
};

// A render-owner integration point for the native Line3D source.  The
// concrete context below is useful for the ordinary renderer/resource pair;
// the interface also lets a title-owned render service provide its own
// lifetime and frame routing without exposing a backend object to WW3D2.
class NativeLine3DSubmitter
{
public:
	virtual ~NativeLine3DSubmitter() {}
	virtual RenderResult SubmitLine3D(const NativeLine3DGeometry &geometry,
		const LegacyLogicalState &state,
		NativeLine3DBufferSet *buffers) = 0;
	// Returns true only when every cached handle is released or already stale
	// after a terminal owner transition.  A backend refusal leaves its exact
	// handle in the buffer set so the owner can retry on the render thread.
	virtual bool ReleaseLine3D(NativeLine3DBufferSet *buffers) = 0;
};

class NativeLine3DRenderContext : public NativeLine3DSubmitter
{
public:
	NativeLine3DRenderContext(NativeW3DRenderer *renderer,
		NativeW3DResources *resources);
	~NativeLine3DRenderContext();

	virtual RenderResult SubmitLine3D(const NativeLine3DGeometry &geometry,
		const LegacyLogicalState &state,
		NativeLine3DBufferSet *buffers);
	virtual bool ReleaseLine3D(NativeLine3DBufferSet *buffers);

	// NativeW3D2 calls this while the submitter lifecycle gate is held and
	// publication is cleared.  It releases every buffer set registered by a
	// line before the resource table or backend can be torn down.  The Line3D
	// objects retain their small cache objects; once their handles are cleared,
	// a later object destructor can safely reclaim that cache without a context
	// pointer.
	void DrainLine3D();
	// Focused lifecycle diagnostics: registered sets are owner-thread cleanup
	// work, not a second resource ownership API.
	unsigned int PendingLine3DCount() const;

private:
	NativeLine3DRenderContext(const NativeLine3DRenderContext &);
	NativeLine3DRenderContext &operator=(const NativeLine3DRenderContext &);

	NativeW3DRenderer *m_renderer;
	NativeW3DResources *m_resources;
	void *m_bufferSets;
};

// This setter is render-owner state.  It must be changed only at the same
// lifecycle boundary that creates/destroys the native WW3D submission
// service, and not from worker threads.
NativeLine3DSubmitter *Get_Native_Line3D_Submitter();
void Set_Native_Line3D_Submitter(NativeLine3DSubmitter *submitter);

// Pins the published submitter while a render-owner operation is in flight.
// Set_Native_Line3D_Submitter waits for active scopes, so clearing or replacing
// the owner cannot destroy the context underneath a Line3D call.  The scope
// is intentionally non-copyable and must be held for the complete operation.
class NativeLine3DSubmitterScope
{
public:
	NativeLine3DSubmitterScope();
	~NativeLine3DSubmitterScope();

	NativeLine3DSubmitter *Get() const;

private:
	NativeLine3DSubmitterScope(const NativeLine3DSubmitterScope &);
	NativeLine3DSubmitterScope &operator=(
		const NativeLine3DSubmitterScope &);

	NativeLine3DSubmitter *m_submitter;
	bool m_locked;
};

// Lifecycle owners use this scope across publication changes and resource
// teardown/rebind.  It uses the same gate as render scopes, so no Line3D call
// can begin while the owner is mutating the context/resource lifetime.  Publish
// is intentionally available only through this locked scope to avoid a
// clear-then-rebind window being interleaved with another owner.
class NativeLine3DSubmitterLifecycleScope
{
public:
	NativeLine3DSubmitterLifecycleScope();
	~NativeLine3DSubmitterLifecycleScope();

	NativeLine3DSubmitter *Get() const;
	void Publish(NativeLine3DSubmitter *submitter);

private:
	NativeLine3DSubmitterLifecycleScope(
		const NativeLine3DSubmitterLifecycleScope &);
	NativeLine3DSubmitterLifecycleScope &operator=(
		const NativeLine3DSubmitterLifecycleScope &);

	bool m_locked;
};

// Converts the historical local 8-vertex box into the neutral position/color
// stream.  The strict vertex-count check is intentional: malformed callers
// must not cause an undersized upload or an out-of-bounds read.
unsigned int Pack_Native_Line3D_Color(const RenderFloat4 &color);
bool Build_Native_Line3D_Geometry(const float *positions,
	unsigned int vertexCount, const RenderFloat4 &color,
	NativeLine3DGeometry *geometry);
void Build_Native_Line3D_Layout(RenderVertexLayout *layout);

// Builds the logical state consumed by NativeW3DRenderer::Submit.  A base
// state, when supplied, preserves the camera/view/projection and other
// owner-published values; only the line's shader pipeline, world transform,
// and texture absence are replaced.
bool Build_Native_Line3D_State(unsigned int shaderBits,
	const LegacyLogicalState *baseState, const float *worldMatrix,
	LegacyLogicalState *state);

RenderResult Upload_Native_Line3D_Geometry(
	NativeW3DResources *resources, const NativeLine3DGeometry *geometry,
	NativeLine3DBufferSet *buffers);
RenderResult Submit_Native_Line3D(NativeW3DRenderer *renderer,
	NativeW3DResources *resources, const LegacyLogicalState *state,
	const NativeLine3DGeometry *geometry, NativeLine3DBufferSet *buffers);
bool Release_Native_Line3D_Buffers(NativeW3DResources *resources,
	NativeLine3DBufferSet *buffers);

// The pure geometry conversion stays inline so the VC6 legacy source can
// share the exact shape and color packing without linking native resource
// ownership code into the 32-bit oracle.
inline unsigned int Pack_Native_Line3D_Color(const RenderFloat4 &color)
{
	return (static_cast<unsigned int>(color.w * 255.0f) << 24) |
		(static_cast<unsigned int>(color.x * 255.0f) << 16) |
		(static_cast<unsigned int>(color.y * 255.0f) << 8) |
		static_cast<unsigned int>(color.z * 255.0f);
}

inline bool Build_Native_Line3D_Geometry(const float *positions,
	unsigned int vertexCount, const RenderFloat4 &color,
	NativeLine3DGeometry *geometry)
{
	if (positions == 0 || geometry == 0 ||
		vertexCount != NATIVE_LINE3D_VERTEX_COUNT)
	{
		return false;
	}
	const unsigned int packedColor = Pack_Native_Line3D_Color(color);
	for (unsigned int vertex = 0; vertex < NATIVE_LINE3D_VERTEX_COUNT;
		++vertex)
	{
		const unsigned int position = vertex * 3;
		geometry->vertices[vertex].x = positions[position];
		geometry->vertices[vertex].y = positions[position + 1];
		geometry->vertices[vertex].z = positions[position + 2];
		geometry->vertices[vertex].color = packedColor;
	}
	static const unsigned short indices[NATIVE_LINE3D_INDEX_COUNT] = {
		3, 5, 1, 7, 5, 3, 1, 5, 0, 5, 4, 0,
		4, 2, 0, 4, 6, 2, 7, 3, 2, 6, 7, 2,
		7, 6, 5, 5, 6, 4, 2, 3, 1, 2, 1, 0
	};
	for (unsigned int index = 0; index < NATIVE_LINE3D_INDEX_COUNT;
		++index)
	{
		geometry->indices[index] = indices[index];
	}
	return true;
}

inline void Build_Native_Line3D_Layout(RenderVertexLayout *layout)
{
	if (layout == 0)
	{
		return;
	}
	*layout = RenderVertexLayout();
	layout->stride = sizeof(NativeLine3DVertex);
	layout->elementCount = 2;
	layout->elements[0].semantic = RENDER_VERTEX_SEMANTIC_POSITION;
	layout->elements[0].semanticIndex = 0;
	layout->elements[0].format = RENDER_VERTEX_DATA_FLOAT3;
	layout->elements[0].byteOffset = 0;
	layout->elements[1].semantic = RENDER_VERTEX_SEMANTIC_DIFFUSE;
	layout->elements[1].semanticIndex = 0;
	layout->elements[1].format = RENDER_VERTEX_DATA_COLOR_BGRA8;
	layout->elements[1].byteOffset = 12;
}
}
}

#endif
