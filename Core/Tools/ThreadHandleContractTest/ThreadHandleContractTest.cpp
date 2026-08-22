#include "Utility/CppMacros.h"
#include "WWLib/always.h"
#include "WWLib/STLUtils.h"
#include "WWLib/win.h"
#include "WWLib/WWCommon.h"
#include "WWLib/wwstring.h"

#define private public
#include "WWLib/thread.h"
#undef private

#include <windows.h>

static_assert(sizeof(ThreadClass::handle) == sizeof(HANDLE),
	"ThreadClass must retain the complete caller-owned native thread handle");

int main()
{
	return 0;
}
