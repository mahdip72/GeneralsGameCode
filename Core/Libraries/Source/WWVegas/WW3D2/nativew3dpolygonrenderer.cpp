#include "Utility/CppMacros.h"
#include "dx8polygonrenderer.h"
#include "dx8renderer.h"
#include "meshmdl.h"
#include "Renderer/RenderGameClient.h"
#include "WWDebug/wwdebug.h"
#include "WWMath/sphere.h"

DX8PolygonRendererClass::DX8PolygonRendererClass(
	unsigned index_count_, MeshModelClass *mmc_,
	DX8TextureCategoryClass *tex_cat, unsigned vertex_offset_,
	unsigned index_offset_, bool strip_, unsigned pass_)
	:
	mmc(mmc_),
	texture_category(tex_cat),
	index_offset(index_offset_),
	vertex_offset(vertex_offset_),
	index_count(index_count_),
	min_vertex_index(0),
	vertex_index_range(0),
	strip(strip_),
	pass(pass_)
{
	WWASSERT(index_count_ != 0);
	WWASSERT(mmc_ != nullptr);
	mmc_->PolygonRendererList.Add_Tail(this);
}

DX8PolygonRendererClass::DX8PolygonRendererClass(
	const DX8PolygonRendererClass &src, MeshModelClass *mmc_)
	:
	mmc(mmc_),
	texture_category(src.texture_category),
	index_offset(src.index_offset),
	vertex_offset(src.vertex_offset),
	index_count(src.index_count),
	min_vertex_index(src.min_vertex_index),
	vertex_index_range(src.vertex_index_range),
	strip(src.strip),
	pass(src.pass)
{
	WWASSERT(mmc_ != nullptr);
	mmc_->PolygonRendererList.Add_Tail(this);
}

DX8PolygonRendererClass::~DX8PolygonRendererClass()
{
	if (texture_category != nullptr)
		texture_category->Remove_Polygon_Renderer(this);
}

void DX8PolygonRendererClass::Set_Vertex_Index_Range(
	unsigned min_vertex_index_, unsigned vertex_index_range_)
{
	min_vertex_index = min_vertex_index_;
	vertex_index_range = vertex_index_range_;
}

void DX8PolygonRendererClass::Render(int base_vertex_offset)
{
	if (base_vertex_offset < 0)
		return;

	rts::render::SetGameIndexBufferOffset(
		static_cast<unsigned int>(base_vertex_offset));
	if (strip)
	{
		if (index_count < 3)
			return;
		rts::render::DrawGameStrip(
			static_cast<unsigned short>(index_offset),
			static_cast<unsigned short>(index_count - 2),
			static_cast<unsigned short>(min_vertex_index),
			static_cast<unsigned short>(vertex_index_range));
	}
	else
	{
		rts::render::DrawGameTriangles(
			static_cast<unsigned short>(index_offset),
			static_cast<unsigned short>(index_count / 3),
			static_cast<unsigned short>(min_vertex_index),
			static_cast<unsigned short>(vertex_index_range));
	}
}

void DX8PolygonRendererClass::Render_Sorted(
	int base_vertex_offset, const SphereClass &bounding_sphere)
{
	WWASSERT(!strip);
	if (strip || base_vertex_offset < 0)
		return;

	const Vector3 &center = bounding_sphere.Center;
	const rts::render::GameBoundingSphere sphere(
		center.X, center.Y, center.Z, bounding_sphere.Radius);
	rts::render::SetGameIndexBufferOffset(
		static_cast<unsigned int>(base_vertex_offset));
	rts::render::DrawGameSortedTriangles(
		sphere,
		static_cast<unsigned short>(index_offset),
		static_cast<unsigned short>(index_count / 3),
		static_cast<unsigned short>(min_vertex_index),
		static_cast<unsigned short>(vertex_index_range));
}

void DX8PolygonRendererClass::Log()
{
	StringClass work(true);
	work.Format("\t%8d %8d %6d %6d %6d %s\n",
		index_count, index_count / 3, index_offset, min_vertex_index,
		vertex_index_range, mmc != nullptr ? mmc->Get_Name() : "<null>");
	WWDEBUG_SAY((work));
}
