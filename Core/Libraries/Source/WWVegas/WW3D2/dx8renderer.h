#ifndef RTS_WW3D2_MESH_RENDERER_FACADE_H
#define RTS_WW3D2_MESH_RENDERER_FACADE_H

#include "WWLib/always.h"
#include "WWLib/simplevec.h"
#include "WWLib/wwstring.h"
#include "WW3D2/shader.h"
#include "WW3D2/meshmatdesc.h"
#include "dx8list.h"

class IndexBufferClass;
class VertexBufferClass;
class MeshClass;
class MeshModelClass;
class DecalMeshClass;
class MaterialPassClass;
class MatPassTaskClass;
class PolyRenderTaskClass;
class TextureClass;
class VertexMaterialClass;
class CameraClass;
class Vertex_Split_Table;

class DX8TextureCategoryClass : public MultiListObjectClass
{
	int pass;
	TextureClass *textures[MeshMatDescClass::MAX_TEX_STAGES];
	ShaderClass shader;
	VertexMaterialClass *material;
	DX8PolygonRendererList PolygonRendererList;
	DX8FVFCategoryContainer *container;
	PolyRenderTaskClass *render_task_head;
	static bool m_gForceMultiply;

public:
	DX8TextureCategoryClass(DX8FVFCategoryContainer *container,
		TextureClass **textures, ShaderClass shader,
		VertexMaterialClass *material, int pass);
	virtual ~DX8TextureCategoryClass() override;

	void Add_Render_Task(DX8PolygonRendererClass *renderer, MeshClass *mesh);
	void Render();
	bool Anything_To_Render() { return render_task_head != nullptr; }
	void Clear_Render_List();

	TextureClass *Peek_Texture(int stage) { return textures[stage]; }
	const VertexMaterialClass *Peek_Material() { return material; }
	ShaderClass Get_Shader() { return shader; }
	DX8PolygonRendererList &Get_Polygon_Renderer_List()
	{
		return PolygonRendererList;
	}

	unsigned Add_Mesh(Vertex_Split_Table &split_buffer,
		unsigned vertex_offset, unsigned index_offset,
		IndexBufferClass *index_buffer, unsigned pass);
	void Log(bool only_visible);
	void Remove_Polygon_Renderer(DX8PolygonRendererClass *renderer);
	void Add_Polygon_Renderer(DX8PolygonRendererClass *renderer,
		DX8PolygonRendererClass *add_after_this = nullptr);
	DX8FVFCategoryContainer *Get_Container() { return container; }
	static void SetForceMultiply(bool multiply)
	{
		m_gForceMultiply = multiply;
	}
};

class DX8FVFCategoryContainer : public MultiListObjectClass
{
public:
	enum { MAX_PASSES = 4 };

protected:
	TextureCategoryList texture_category_list[MAX_PASSES];
	TextureCategoryList visible_texture_category_list[MAX_PASSES];
	MatPassTaskClass *visible_matpass_head;
	MatPassTaskClass *visible_matpass_tail;
	IndexBufferClass *index_buffer;
	int used_indices;
	unsigned FVF;
	unsigned passes;
	unsigned uv_coordinate_channels;
	bool sorting;
	bool AnythingToRender;
	bool AnyDelayedPassesToRender;

	bool Generate_Texture_Categories(Vertex_Split_Table &split_table,
		unsigned vertex_offset);
	bool Insert_To_Texture_Category(Vertex_Split_Table &split_table,
		TextureClass **textures, VertexMaterialClass *material,
		ShaderClass shader, int pass, unsigned vertex_offset);
	bool Anything_To_Render() { return AnythingToRender; }
	bool Any_Delayed_Passes_To_Render()
	{
		return AnyDelayedPassesToRender;
	}
	void Render_Procedural_Material_Passes();
	DX8TextureCategoryClass *Find_Matching_Texture_Category(
		TextureClass *texture, unsigned pass, unsigned stage,
		DX8TextureCategoryClass *reference);
	DX8TextureCategoryClass *Find_Matching_Texture_Category(
		VertexMaterialClass *material, unsigned pass,
		DX8TextureCategoryClass *reference);

public:
	DX8FVFCategoryContainer(unsigned FVF, bool sorting);
	virtual ~DX8FVFCategoryContainer() override;

	static unsigned Define_FVF(MeshModelClass *mmc, bool enable_lighting);
	bool Is_Sorting() const { return sorting; }
	void Change_Polygon_Renderer_Texture(
		DX8PolygonRendererList &polygon_renderer_list,
		TextureClass *texture, TextureClass *new_texture,
		unsigned pass, unsigned stage);
	void Change_Polygon_Renderer_Material(
		DX8PolygonRendererList &polygon_renderer_list,
		VertexMaterialClass *material, VertexMaterialClass *new_material,
		unsigned pass);
	void Remove_Texture_Category(DX8TextureCategoryClass *category);

	virtual void Render() = 0;
	virtual void Add_Mesh(MeshModelClass *mmc) = 0;
	virtual void Log(bool only_visible) = 0;
	virtual bool Check_If_Mesh_Fits(MeshModelClass *mmc) = 0;

	unsigned Get_FVF() const { return FVF; }
	void Add_Visible_Texture_Category(
		DX8TextureCategoryClass *category, int pass)
	{
		WWASSERT(pass >= 0 && pass < MAX_PASSES);
		WWASSERT(category != nullptr);
		WWASSERT(texture_category_list[pass].Contains(category));
		visible_texture_category_list[pass].Add(category);
		AnythingToRender = true;
	}

	void Add_Visible_Material_Pass(MaterialPassClass *pass, MeshClass *mesh);
	virtual void Add_Delayed_Visible_Material_Pass(
		MaterialPassClass *pass, MeshClass *mesh) = 0;
	virtual void Render_Delayed_Procedural_Material_Passes() = 0;
};

class DX8RigidFVFCategoryContainer : public DX8FVFCategoryContainer
{
public:
	DX8RigidFVFCategoryContainer(unsigned FVF, bool sorting);
	virtual ~DX8RigidFVFCategoryContainer() override;
	virtual void Add_Mesh(MeshModelClass *mmc) override;
	virtual void Log(bool only_visible) override;
	virtual bool Check_If_Mesh_Fits(MeshModelClass *mmc) override;
	virtual void Render() override;
	virtual void Add_Delayed_Visible_Material_Pass(
		MaterialPassClass *pass, MeshClass *mesh) override;
	virtual void Render_Delayed_Procedural_Material_Passes() override;

protected:
	VertexBufferClass *vertex_buffer;
	int used_vertices;
	MatPassTaskClass *delayed_matpass_head;
	MatPassTaskClass *delayed_matpass_tail;
};

class DX8SkinFVFCategoryContainer : public DX8FVFCategoryContainer
{
public:
	DX8SkinFVFCategoryContainer(bool sorting);
	virtual ~DX8SkinFVFCategoryContainer() override;
	virtual void Render() override;
	virtual void Add_Mesh(MeshModelClass *mmc) override;
	virtual void Log(bool only_visible) override;
	virtual bool Check_If_Mesh_Fits(MeshModelClass *mmc) override;
	void Add_Visible_Skin(MeshClass *mesh);
	virtual void Add_Delayed_Visible_Material_Pass(
		MaterialPassClass *pass, MeshClass *mesh) override
	{
		Add_Visible_Material_Pass(pass, mesh);
	}
	virtual void Render_Delayed_Procedural_Material_Passes() override {}

private:
	void Reset();
	void clearVisibleSkinList();
	unsigned int VisibleVertexCount;
	MeshClass *VisibleSkinHead;
	MeshClass *VisibleSkinTail;
};

class DX8MeshRendererClass
{
public:
	DX8MeshRendererClass();
	~DX8MeshRendererClass();

	void Init();
	void Shutdown();
	void Flush();
	void Clear_Pending_Delete_Lists();
	void Log_Statistics_String(bool only_visible);
	static void Request_Log_Statistics();
	void Register_Mesh_Type(MeshModelClass *mmc);
	void Unregister_Mesh_Type(MeshModelClass *mmc);
	void Set_Camera(CameraClass *camera) { this->camera = camera; }
	CameraClass *Peek_Camera() { return camera; }
	void Add_To_Render_List(DecalMeshClass *decalmesh);
	void Enable_Lighting(bool enable) { enable_lighting = enable; }
	void Invalidate(bool shutdown = false);

protected:
	void Render_Decal_Meshes();
	bool enable_lighting;
	CameraClass *camera;
	SimpleDynVecClass<FVFCategoryList *> texture_category_container_lists_rigid;
	FVFCategoryList *texture_category_container_list_skin;
	DecalMeshClass *visible_decal_meshes;
};

extern DX8MeshRendererClass TheDX8MeshRenderer;

#endif
