#pragma once

#include "Lib/LockstepV2Contract.h"
#include "Lib/MultiplayerSimulationPolicy.h"

#ifndef RTS_LOCKSTEP_V2_PRODUCT_PROMOTED_KERNEL_MASK
#define RTS_LOCKSTEP_V2_PRODUCT_PROMOTED_KERNEL_MASK 0U
#endif

namespace rts
{
namespace lockstep_v2
{

// Product promotion is a build-owned trust root.  It is deliberately separate
// from both the mutable InstalledNet3Validation v1 files and the opt-in
// installed lockstep-v2 qualification process.  A normal product may advertise
// worker authority only when its Release configuration embeds this exact
// contract.
enum
{
	PRODUCT_PROMOTION_SCHEMA_VERSION = 2
};

struct ProductPromotionAuthority
{
	unsigned promotionSchemaVersion;
	unsigned lockstepSchemaVersion;
	unsigned protocolEpoch;
	unsigned promotedKernelMask;
};

inline ProductPromotionAuthority MakeProductPromotionAuthority(
	unsigned kernelMask)
{
	ProductPromotionAuthority authority;
	authority.promotionSchemaVersion = PRODUCT_PROMOTION_SCHEMA_VERSION;
	authority.lockstepSchemaVersion = kSchemaVersion;
	authority.protocolEpoch = kProtocolEpoch;
	authority.promotedKernelMask = kernelMask;
	return authority;
}

inline unsigned ResolveProductPromotionKernelMask(
	const ProductPromotionAuthority &authority,
	unsigned liveIntegratedKernelMask)
{
	const unsigned knownKernelMask = static_cast<unsigned>(
		MULTIPLAYER_SIMULATION_KERNEL_KNOWN_MASK);
	if (authority.promotionSchemaVersion !=
			PRODUCT_PROMOTION_SCHEMA_VERSION ||
		authority.lockstepSchemaVersion != kSchemaVersion ||
		authority.protocolEpoch != kProtocolEpoch ||
		liveIntegratedKernelMask == 0U ||
		(liveIntegratedKernelMask & ~knownKernelMask) != 0U ||
		authority.promotedKernelMask != liveIntegratedKernelMask)
	{
		return MULTIPLAYER_SIMULATION_KERNEL_RELEASE_PROVEN_DEFAULT_MASK;
	}
	return authority.promotedKernelMask;
}

inline unsigned ResolveEmbeddedProductPromotionKernelMask(
	unsigned liveIntegratedKernelMask)
{
	const ProductPromotionAuthority authority =
		MakeProductPromotionAuthority(
			RTS_LOCKSTEP_V2_PRODUCT_PROMOTED_KERNEL_MASK);
	return ResolveProductPromotionKernelMask(authority,
		liveIntegratedKernelMask);
}

} // namespace lockstep_v2
} // namespace rts
