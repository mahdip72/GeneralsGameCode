#include "Utility/CppMacros.h"
#include "WWLib/always.h"
#include "WWLib/STLUtils.h"
#include "WWLib/win.h"
#include "WWLib/WWCommon.h"
#include "WWLib/wwstring.h"

#define private public
#include "WWLib/registry.h"
#undef private

#include <windows.h>

static_assert(sizeof(RegistryClass::Key) == sizeof(HKEY),
	"RegistryClass must retain a complete native registry handle");

int main()
{
	return 0;
}
