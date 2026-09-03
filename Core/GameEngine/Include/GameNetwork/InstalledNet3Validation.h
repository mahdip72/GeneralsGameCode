#pragma once

namespace rts
{

// The installed NET3 release-proof lane is an explicit, local-only process
// mode. It is unavailable to VC6/Win32 and normal game/network startup.
bool ConfigureInstalledNet3Validation(const char *configuration);
bool IsInstalledNet3ValidationRequested();
int RunInstalledNet3Validation(unsigned buildCompatibilityCrc,
	unsigned contentCrc);

} // namespace rts
