#include "Utility/CppMacros.h"
#include "dx8rendererdebugger.h"
#include "WWLib/hashtemplate.h"
#include "mesh.h"
#include "meshmdl.h"

static HashTemplateClass<unsigned, MeshClass *> MeshHash;

bool DX8RendererDebugger::Enabled = false;

void DX8RendererDebugger::Enable(bool enable)
{
	Enabled = enable;
	if (!enable)
		Update();
}

void DX8RendererDebugger::Get_String(StringClass &s)
{
	if (!Enabled) {
		s = "";
		return;
	}

	s = "\n\n\n\n";
	int count = 0;
	HashTemplateIterator<unsigned, MeshClass *> iterator(MeshHash);
	for (iterator.First(); !iterator.Is_Done(); iterator.Next()) {
		StringClass line(0, true);
		MeshClass *mesh = iterator.Peek_Value();
		MeshModelClass *model = mesh != nullptr ? mesh->Peek_Model() : nullptr;
		const int polygons = model != nullptr ? model->Get_Polygon_Count() : 0;
		const int vertices = model != nullptr ? model->Get_Vertex_Count() : 0;
		line.Format("id: %5.5d mesh: %s %d polys, %d verts",
			iterator.Peek_Key(),
			mesh != nullptr ? mesh->Get_Name() : "<null>",
			polygons, vertices);
		s += line;
		if (mesh != nullptr && mesh->Is_Disabled_By_Debugger())
			s += " (disabled)\n";
		else
			s += "\n";
		if (++count > 20)
			break;
	}
}

void DX8RendererDebugger::Update()
{
	HashTemplateIterator<unsigned, MeshClass *> iterator(MeshHash);
	for (iterator.First(); !iterator.Is_Done(); iterator.Next()) {
		MeshClass *mesh = iterator.Peek_Value();
		if (mesh != nullptr)
			mesh->Release_Ref();
	}
	MeshHash.Remove_All();
}

#ifdef WWDEBUG
void DX8RendererDebugger::Add_Mesh(MeshClass *mesh)
{
	if (!Enabled || mesh == nullptr)
		return;

	const unsigned id = mesh->Get_Debug_Id();
	if (MeshHash.Get(id) != nullptr)
		return;

	mesh->Add_Ref();
	MeshHash.Insert(id, mesh);
}
#endif

void DX8RendererDebugger::Disable_Mesh(unsigned id)
{
	if (!Enabled)
		return;
	MeshClass *mesh = MeshHash.Get(id);
	if (mesh != nullptr)
		mesh->Set_Debugger_Disable(true);
}

void DX8RendererDebugger::Enable_Mesh(unsigned id)
{
	if (!Enabled)
		return;
	MeshClass *mesh = MeshHash.Get(id);
	if (mesh != nullptr)
		mesh->Set_Debugger_Disable(false);
}

void DX8RendererDebugger::Disable_All()
{
	if (!Enabled)
		return;
	HashTemplateIterator<unsigned, MeshClass *> iterator(MeshHash);
	for (iterator.First(); !iterator.Is_Done(); iterator.Next()) {
		MeshClass *mesh = iterator.Peek_Value();
		if (mesh != nullptr)
			mesh->Set_Debugger_Disable(true);
	}
}

void DX8RendererDebugger::Enable_All()
{
	if (!Enabled)
		return;
	HashTemplateIterator<unsigned, MeshClass *> iterator(MeshHash);
	for (iterator.First(); !iterator.Is_Done(); iterator.Next()) {
		MeshClass *mesh = iterator.Peek_Value();
		if (mesh != nullptr)
			mesh->Set_Debugger_Disable(false);
	}
}
