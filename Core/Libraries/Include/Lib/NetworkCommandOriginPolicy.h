#pragma once

#if defined(_WIN64)
#include "Lib/LockstepV2Contract.h"
#endif

namespace rts
{

// The decoded command must retain the wrapper's claimed player ID.  Direct
// peer traffic is endpoint-bound before this check; router-relayed traffic
// preserves the legacy trusted-router topology and is not cryptographic
// proof that the named player authored the command.  Hostile-router resistance
// requires a separately versioned per-origin authenticator.
inline bool IsWrappedNetworkCommandOriginAuthorized(unsigned claimedWrapperPlayerId,
	unsigned decodedCommandPlayerId, unsigned maxPlayerSlots)
{
	return claimedWrapperPlayerId < maxPlayerSlots &&
		decodedCommandPlayerId < maxPlayerSlots &&
		claimedWrapperPlayerId == decodedCommandPlayerId;
}

#if defined(_WIN64)
// V2 is direct-origin by default.  A router is accepted only when the session
// contract explicitly selected the trusted-router mode; the legacy helper
// above remains available for the pre-v2 relay topology.  This boundary is
// intentionally separate from wrapper/inner-player equality, which is not an
// authenticator for a hostile router.
inline bool IsLockstepV2CommandSourceAuthorized(
	unsigned observedSlot,
	unsigned claimedSlot,
	unsigned packetRouterSlot,
	lockstep_v2::CommandOriginMode originMode,
	unsigned maxPlayerSlots)
{
	if (observedSlot >= maxPlayerSlots || claimedSlot >= maxPlayerSlots)
		return false;
	if (originMode == lockstep_v2::CommandOriginMode::DirectAuthenticated)
		return observedSlot == claimedSlot;
	if (originMode == lockstep_v2::CommandOriginMode::TrustedRouter)
		return observedSlot == claimedSlot ||
			(observedSlot == packetRouterSlot && packetRouterSlot < maxPlayerSlots);
	return false;
}
#endif

} // namespace rts
