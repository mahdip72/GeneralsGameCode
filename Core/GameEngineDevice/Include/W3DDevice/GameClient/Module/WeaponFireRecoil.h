// Shared weapon-fire bounds and per-instance recoil preparation.
// No model validation, effect dispatch, RNG, serialization or global state.
#ifndef W3D_WEAPON_FIRE_RECOIL_H
#define W3D_WEAPON_FIRE_RECOIL_H

#include <cstddef>
#include <vector>

namespace rts
{
namespace weapon_fire_recoil
{
inline bool IsWeaponSlotValid(int slot, int slotCount)
{
	return slot >= 0 && slot < slotCount;
}

inline bool NormalizeBarrelIndex(int requested, std::size_t barrelCount, int *normalized)
{
	if (!normalized || barrelCount == 0) return false;
	*normalized = requested < 0 || static_cast<std::size_t>(requested) >= barrelCount ? 0 : requested;
	return true;
}

// Shared by fire and client recoil admission, including instances that have
// never fired but now observe a lazily populated shared barrel table.
template <class RecoilInfo>
void SynchronizeCount(std::vector<RecoilInfo> &recoils, std::size_t barrelCount)
{
	if (recoils.size() != barrelCount) recoils.resize(barrelCount, RecoilInfo());
}

// Call after any FX callbacks, using the current valid barrel table. resize
// preserves every surviving history entry and default-initializes additions;
// it never clears all existing entries as the model-state rebuild routine does.
template <class RecoilInfo>
bool PrepareForFire(int slot, int slotCount, std::size_t barrelCount, int requested,
	std::vector<RecoilInfo> *recoils, int *normalized)
{
	if (!IsWeaponSlotValid(slot, slotCount) || !recoils || !normalized || barrelCount == 0)
		return false;
	int index;
	if (!NormalizeBarrelIndex(requested, barrelCount, &index)) return false;
	SynchronizeCount(*recoils, barrelCount);
	*normalized = index;
	return true;
}
}
}

#endif
