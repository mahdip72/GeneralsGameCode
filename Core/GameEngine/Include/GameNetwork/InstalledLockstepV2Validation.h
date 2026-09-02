#pragma once

#if defined(_WIN64)

#include "Lib/LockstepV2Contract.h"

class GameInfo;

namespace rts
{

// The v2 lane is an opt-in installed-process qualification.  No ordinary
// game can enter it, and no caller-provided mask is accepted as authority.
bool ConfigureInstalledLockstepV2Qualification(const char *configuration);
bool IsInstalledLockstepV2QualificationRequested();
bool PrepareInstalledLockstepV2Qualification(unsigned buildCompatibilityCrc,
	unsigned contentCrc);
bool ServiceInstalledLockstepV2Qualification(unsigned buildCompatibilityCrc,
	unsigned contentCrc);
bool IsInstalledLockstepV2QualificationActive();
bool IsInstalledLockstepV2ProofStarted();
bool IsInstalledLockstepV2QualificationFailed();
bool RecordInstalledLockstepV2Frame(unsigned frame, unsigned crc);
unsigned GetInstalledLockstepV2ValidationAuthorityMask(
	unsigned buildCompatibilityCrc, unsigned contentCrc);
bool GetInstalledLockstepV2WorkerTelemetry(
	lockstep_v2::WorkerTelemetry *telemetry);
GameInfo *GetInstalledLockstepV2GameInfo();
void RequestInstalledLockstepV2Stop();
bool IsInstalledLockstepV2StopRequested();
bool FinalizeInstalledLockstepV2Qualification(bool cleanShutdown);

} // namespace rts

#endif // defined(_WIN64)
