#include "WWLib/verchk.h"

#include <type_traits>

typedef int (*VersionComparisonFunction)(HINSTANCE, const char *);
static_assert(std::is_same<decltype(&Compare_EXE_Version),
	VersionComparisonFunction>::value,
	"Compare_EXE_Version must accept a complete native instance handle");

int main()
{
	return 0;
}
