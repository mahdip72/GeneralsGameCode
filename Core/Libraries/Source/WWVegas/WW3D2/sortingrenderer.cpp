/*
** Native WW3D2 sorting facade.  The x64 product submits deferred work to the
** backend-neutral render owner; the VC6/x86 implementation lives under
** Core/LegacyRenderer and is selected by the x86 build graph.
*/

#include "Utility/CppMacros.h"
#include "sortingrenderer.h"
#include "Renderer/RenderGameClient.h"
#include "WWMath/sphere.h"

bool SortingRendererClass::_EnableTriangleDraw = true;

void SortingRendererClass::SetMinVertexBufferSize(unsigned val)
{
	(void)val;
}

void SortingRendererClass::Insert_Triangles(
	const SphereClass &bounding_sphere,
	unsigned short start_index,
	unsigned short polygon_count,
	unsigned short min_vertex_index,
	unsigned short vertex_count)
{
	const Vector3 &center = bounding_sphere.Center;
	const rts::render::GameBoundingSphere neutral_sphere(
		center.X, center.Y, center.Z, bounding_sphere.Radius);
	rts::render::DrawGameSortedTriangles(neutral_sphere, start_index,
		polygon_count, min_vertex_index, vertex_count);
}

void SortingRendererClass::Insert_Triangles(
	unsigned short start_index,
	unsigned short polygon_count,
	unsigned short min_vertex_index,
	unsigned short vertex_count)
{
	rts::render::DrawGameSortedTriangles(start_index, polygon_count,
		min_vertex_index, vertex_count);
}

void SortingRendererClass::Flush_Sorting_Pool()
{
	// The native owner drains the deferred queue from the explicit Flush call.
}

void SortingRendererClass::Insert_To_Sorted_List(SortingNodeStruct *state)
{
	(void)state;
}

void SortingRendererClass::Insert_To_Sorting_Pool(SortingNodeStruct *state)
{
	(void)state;
}

void SortingRendererClass::Flush()
{
	// Preserve WW3D2's explicit flush boundary.  The native owner records any
	// failure in its frame latch while returning the exact RenderResult here.
	const rts::render::RenderResult result =
		rts::render::FlushGameSortedTriangles();
	(void)result;
}

void SortingRendererClass::Deinit()
{
}
