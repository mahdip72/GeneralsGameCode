#include "Utility/CppMacros.h"
#include "GameClient/GameWindow.h"
#include "WWLib/mempool.h"

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
struct PoolNode
{
	PoolNode() : next(nullptr), payload(0) { ++constructed; }
	~PoolNode() { ++destroyed; }

	PoolNode *next;
	uintptr_t payload;
	static Int constructed;
	static Int destroyed;
};

Int PoolNode::constructed = 0;
Int PoolNode::destroyed = 0;

int check(bool condition, const char *name)
{
	if (!condition)
	{
		fprintf(stderr, "%s\n", name);
		return 1;
	}
	return 0;
}

int checkUiScalarConversions()
{
	const Int signedValues[] = { 0, 1, -1, INT32_MIN, INT32_MAX };
	for (UnsignedInt index = 0; index < sizeof(signedValues) / sizeof(signedValues[0]); ++index)
	{
		const Int value = signedValues[index];
		if (check(GadgetItemDataToInt(GadgetItemDataFromInt(value)) == value,
			"signed gadget item data must round-trip through a pointer-sized value") != 0)
			return 1;
		if (check(WindowMsgDataToInt(WindowMsgDataFromInt(value)) == value,
			"signed window message data must round-trip through a pointer-sized value") != 0)
			return 1;
	}

	const UnsignedInt unsignedValues[] = { 0U, 1U, UINT32_MAX };
	for (UnsignedInt index = 0; index < sizeof(unsignedValues) / sizeof(unsignedValues[0]); ++index)
	{
		const UnsignedInt value = unsignedValues[index];
		if (check(GadgetItemDataToUnsignedInt(GadgetItemDataFromUnsignedInt(value)) == value,
			"unsigned gadget item data must round-trip through a pointer-sized value") != 0)
			return 1;
		if (check(WindowMsgDataToUnsignedInt(WindowMsgDataFromUnsignedInt(value)) == value,
			"unsigned window message data must round-trip through a pointer-sized value") != 0)
			return 1;
	}

	return 0;
}

int checkObjectPoolPointerWidth()
{
	ObjectPoolClass<PoolNode, 4> pool;
	PoolNode *nodes[9];
	for (UnsignedInt index = 0; index < sizeof(nodes) / sizeof(nodes[0]); ++index)
		nodes[index] = pool.Allocate_Object();
	for (UnsignedInt index = 0; index < sizeof(nodes) / sizeof(nodes[0]); ++index)
		pool.Free_Object(nodes[index]);
	return check(PoolNode::constructed == 9 && PoolNode::destroyed == 9,
		"object-pool construction and destruction must remain balanced");
}
}

int main()
{
	if (checkUiScalarConversions() != 0)
		return 1;
	if (checkObjectPoolPointerWidth() != 0)
		return 1;

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
