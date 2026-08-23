#pragma once

#include "Lib/BaseType.h"

class AudioManager;

namespace AudioManagerFactory
{
	// The implementation is selected by the target architecture.  The common
	// Win32 engine only depends on this neutral declaration.
	AudioManager *create(Bool dummy);
}
