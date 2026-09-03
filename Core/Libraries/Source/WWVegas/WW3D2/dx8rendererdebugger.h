#ifndef RTS_WW3D2_RENDERER_DEBUGGER_FACADE_H
#define RTS_WW3D2_RENDERER_DEBUGGER_FACADE_H

#include "WWLib/always.h"

class StringClass;
class MeshClass;

// The debugger is a renderer-neutral mesh filter.  The historical class name
// remains part of the title-facing interface so existing tools keep working.
class DX8RendererDebugger
{
	static bool Enabled;

public:
	static void Enable(bool enable);
	WWINLINE static bool Is_Enabled() { return Enabled; }
	static void Get_String(StringClass &s);
	static void Update();
#ifdef WWDEBUG
	static void Add_Mesh(MeshClass *mesh);
#else
	static void Add_Mesh(MeshClass *) {}
#endif

	static void Disable_Mesh(unsigned id);
	static void Enable_Mesh(unsigned id);
	static void Disable_All();
	static void Enable_All();
};

#endif
