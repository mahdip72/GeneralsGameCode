#include "Utility/CppMacros.h"
#include "GameClient/Mouse.h"

#include <stdio.h>

namespace
{
int check(bool condition, const char *testName, const char *expression)
{
	if (!condition)
	{
		fprintf(stderr, "%s: %s\n", testName, expression);
		return 1;
	}
	return 0;
}

#define CHECK(testName, expression) \
	do { if (check((expression), testName, #expression) != 0) return 1; } while (0)

int testInvisibleHardwareCursorFallback()
{
	CHECK("invisible hardware cursor fallback", Mouse::resolveHardwareCursorMode(Mouse::RM_DX8, FALSE) == Mouse::RM_POLYGON);
	CHECK("invisible windows cursor", Mouse::resolveHardwareCursorMode(Mouse::RM_WINDOWS, FALSE) == Mouse::RM_WINDOWS);
	CHECK("invisible W3D cursor", Mouse::resolveHardwareCursorMode(Mouse::RM_W3D, FALSE) == Mouse::RM_W3D);
	CHECK("invisible polygon cursor", Mouse::resolveHardwareCursorMode(Mouse::RM_POLYGON, FALSE) == Mouse::RM_POLYGON);
	return 0;
}

int testVisibleHardwareCursorPreservesConfiguredMode()
{
	CHECK("visible hardware cursor", Mouse::resolveHardwareCursorMode(Mouse::RM_DX8, TRUE) == Mouse::RM_DX8);
	CHECK("visible windows cursor", Mouse::resolveHardwareCursorMode(Mouse::RM_WINDOWS, TRUE) == Mouse::RM_WINDOWS);
	CHECK("visible W3D cursor", Mouse::resolveHardwareCursorMode(Mouse::RM_W3D, TRUE) == Mouse::RM_W3D);
	CHECK("visible polygon cursor", Mouse::resolveHardwareCursorMode(Mouse::RM_POLYGON, TRUE) == Mouse::RM_POLYGON);
	return 0;
}
}

int main()
{
	if (testInvisibleHardwareCursorFallback() != 0)
		return 1;
	if (testVisibleHardwareCursorPreservesConfiguredMode() != 0)
		return 1;
	return 0;
}
