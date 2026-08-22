#include "Utility/CppMacros.h"
#include "GameClient/GameWindow.h"

#include <windows.h>
#include <stdio.h>

static_assert(sizeof(uintptr_t) == 8, "uintptr_t must be pointer-sized on x64");
static_assert(sizeof(Int) == 4, "gameplay Int must remain 32-bit");
static_assert(sizeof(UnsignedInt) == 4, "gameplay UnsignedInt must remain 32-bit");
static_assert(sizeof(WindowMsgData) == 8, "window messages must preserve pointers");
static_assert(sizeof(WPARAM) == 8, "Win32 WPARAM must be pointer-sized");
static_assert(sizeof(LPARAM) == 8, "Win32 LPARAM must be pointer-sized");
static_assert(sizeof(LRESULT) == 8, "Win32 LRESULT must be pointer-sized");

namespace
{
int check(bool condition, const char *name)
{
	if (!condition)
	{
		fprintf(stderr, "%s\n", name);
		return 1;
	}
	return 0;
}
}

int main()
{
	const uintptr_t syntheticAddress = UINT64_C(0x0000000100001234);
	void *pointer = reinterpret_cast<void *>(syntheticAddress);
	const uintptr_t pointerBits = reinterpret_cast<uintptr_t>(pointer);
	const WindowMsgData messageData = static_cast<WindowMsgData>(pointerBits);
	if (check(reinterpret_cast<uintptr_t>(pointer) == messageData,
		"WindowMsgData must retain high pointer bits") != 0)
		return 1;

	const WindowMsgData scalarSentinel = static_cast<WindowMsgData>(-1);
	return check(scalarSentinel == static_cast<WindowMsgData>(-1),
		"WindowMsgData must retain scalar sentinel values");
}
