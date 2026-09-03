#ifndef RTS_WW3D2_POLYGON_RENDERER_FACADE_H
#define RTS_WW3D2_POLYGON_RENDERER_FACADE_H

#include "WWLib/always.h"
#include "dx8list.h"

class MeshModelClass;
class SphereClass;
class DX8TextureCategoryClass;

// Historical mesh code stores one of these records for every material batch.
// The record layout remains title-visible, while its operations are forwarded
// through the renderer-neutral owner in the native implementation.
class DX8PolygonRendererClass : public MultiListObjectClass
{
	MeshModelClass *mmc;
	DX8TextureCategoryClass *texture_category;
	unsigned index_offset;
	unsigned vertex_offset;
	unsigned index_count;
	unsigned min_vertex_index;
	unsigned vertex_index_range;
	bool strip;
	unsigned pass;

public:
	DX8PolygonRendererClass(unsigned index_count,
		MeshModelClass *mmc_, DX8TextureCategoryClass *tex_cat,
		unsigned vertex_offset, unsigned index_offset, bool strip,
		unsigned pass);
	DX8PolygonRendererClass(const DX8PolygonRendererClass &src,
		MeshModelClass *mmc_);
	virtual ~DX8PolygonRendererClass() override;

	void Render(int base_vertex_offset);
	void Render_Sorted(int base_vertex_offset,
		const SphereClass &bounding_sphere);
	void Set_Vertex_Index_Range(unsigned min_vertex_index_,
		unsigned vertex_index_range_);

	unsigned Get_Vertex_Offset() { return vertex_offset; }
	unsigned Get_Index_Offset() { return index_offset; }
	unsigned Get_Pass() { return pass; }
	MeshModelClass *Get_Mesh_Model_Class() { return mmc; }
	DX8TextureCategoryClass *Get_Texture_Category() { return texture_category; }
	void Set_Texture_Category(DX8TextureCategoryClass *tc)
	{
		texture_category = tc;
	}

	void Log();
};

#endif
