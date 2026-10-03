// Executes the exact header helpers called by W3DModelDraw::handleWeaponFireFX.
// Arbitrary history records avoid a copied model/fire/recoil-state oracle.
#include "W3DDevice/GameClient/Module/WeaponFireRecoil.h"
#include <cstdio>
#include <vector>

namespace
{
int failures = 0;
int historyDefaultConstructions = 0;
void Check(bool value, int line)
{
	if (!value) { std::fprintf(stderr, "weapon fire recoil contract failed at line %d\n", line); ++failures; }
}
#define CHECK(value) Check((value), __LINE__)

struct History
{
	int token;
	float shift, rate;
	History() : token(-7), shift(0.0f), rate(0.0f) { ++historyDefaultConstructions; }
};
bool Same(const History &a, const History &b)
{
	return a.token == b.token && a.shift == b.shift && a.rate == b.rate;
}
History Active()
{
	History h; h.token = 123; h.shift = 7.5f; h.rate = -0.75f; return h;
}

void TestIndexAndSlot()
{
	using namespace rts::weapon_fire_recoil;
	CHECK(IsWeaponSlotValid(0, 3) && IsWeaponSlotValid(2, 3));
	CHECK(!IsWeaponSlotValid(-1, 3) && !IsWeaponSlotValid(3, 3));
	CHECK(!IsWeaponSlotValid(0, 0) && !IsWeaponSlotValid(0, -1));
	int normalized = 99;
	CHECK(!NormalizeBarrelIndex(0, 0, &normalized) && normalized == 99);
	CHECK(!NormalizeBarrelIndex(0, 1, 0) && normalized == 99);
	CHECK(NormalizeBarrelIndex(-1, 3, &normalized) && normalized == 0);
	CHECK(NormalizeBarrelIndex(0, 3, &normalized) && normalized == 0);
	CHECK(NormalizeBarrelIndex(2, 3, &normalized) && normalized == 2);
	CHECK(NormalizeBarrelIndex(3, 3, &normalized) && normalized == 0); // exactly size
	CHECK(NormalizeBarrelIndex(4, 3, &normalized) && normalized == 0);
}

void TestLazyGrowthHistoryAndShrink()
{
	using namespace rts::weapon_fire_recoil;
	std::vector<History> histories;
	int normalized = -99;
	CHECK(PrepareForFire(0, 3, 3, 2, &histories, &normalized));
	CHECK(normalized == 2 && histories.size() == 3);
	const History idle;
	for (std::size_t index = 0; index < histories.size(); ++index) CHECK(Same(histories[index], idle));
	histories[0] = Active(); histories[1].token = 456; histories[1].shift = 3.0f; histories[1].rate = 2.0f;
	const History first = histories[0], second = histories[1];
	CHECK(PrepareForFire(0, 3, 5, 4, &histories, &normalized));
	CHECK(normalized == 4 && histories.size() == 5);
	CHECK(Same(histories[0], first) && Same(histories[1], second));
	CHECK(Same(histories[3], idle) && Same(histories[4], idle));
	History *storage = &histories[0];
	const int constructorsBeforeRepeat = historyDefaultConstructions;
	for (int fire = 0; fire < 10; ++fire)
	{
		CHECK(PrepareForFire(0, 3, 5, 1, &histories, &normalized));
		CHECK(normalized == 1 && &histories[0] == storage);
		CHECK(historyDefaultConstructions == constructorsBeforeRepeat);
		CHECK(Same(histories[0], first) && Same(histories[1], second)); // no reset on repeat
	}
	CHECK(PrepareForFire(0, 3, 2, 2, &histories, &normalized));
	CHECK(histories.size() == 2 && normalized == 0);
	CHECK(Same(histories[0], first) && Same(histories[1], second));
	CHECK(PrepareForFire(0, 3, 1, -1, &histories, &normalized));
	CHECK(histories.size() == 1 && normalized == 0 && Same(histories[0], first));
	CHECK(PrepareForFire(0, 3, 3, 2, &histories, &normalized));
	CHECK(histories.size() == 3 && Same(histories[0], first));
	CHECK(Same(histories[1], idle) && Same(histories[2], idle)); // re-added, not stale tail
}

void TestInvalidAdmissionPreservesState()
{
	using namespace rts::weapon_fire_recoil;
	std::vector<History> histories(2);
	histories[0] = Active(); histories[1].token = 456;
	const History first = histories[0], second = histories[1];
	int normalized = 99;
	CHECK(!PrepareForFire(-1, 3, 5, 0, &histories, &normalized));
	CHECK(!PrepareForFire(3, 3, 5, 0, &histories, &normalized));
	CHECK(!PrepareForFire(0, 0, 5, 0, &histories, &normalized));
	CHECK(!PrepareForFire(0, 3, 0, 0, &histories, &normalized));
	CHECK(!PrepareForFire<History>(0, 3, 5, 0, 0, &normalized));
	CHECK(!PrepareForFire(0, 3, 5, 0, &histories, 0));
	CHECK(normalized == 99 && histories.size() == 2);
	CHECK(Same(histories[0], first) && Same(histories[1], second));
}

void TestMixedSlotAndClientAdmissionBeforeAnyFire()
{
	using namespace rts::weapon_fire_recoil;
	// Preparation is slot-wide, not conditioned on the selected barrel's bones.
	// Barrel0 may be FX-only while barrel1 needs recoil/muzzle client iteration.
	std::vector<History> mixedSlot;
	int normalized = -1;
	CHECK(PrepareForFire(0, 3, 2, 0, &mixedSlot, &normalized));
	CHECK(normalized == 0 && mixedSlot.size() == 2);
	const History idle;
	CHECK(Same(mixedSlot[0], idle) && Same(mixedSlot[1], idle));
	mixedSlot[1] = Active();
	CHECK(PrepareForFire(0, 3, 2, 0, &mixedSlot, &normalized));
	CHECK(Same(mixedSlot[1], Active())); // FX-only preparation preserves another barrel

	// Client admission itself establishes size without requiring a fire index.
	std::vector<History> firstInstance[3], neverFiredInstance[3];
	firstInstance[1].push_back(Active());
	const std::size_t sharedCount = 4;
	SynchronizeCount(firstInstance[1], sharedCount);
	SynchronizeCount(neverFiredInstance[1], sharedCount);
	CHECK(firstInstance[1].size() == sharedCount && Same(firstInstance[1][0], Active()));
	CHECK(neverFiredInstance[1].size() == sharedCount);
	for (std::size_t index = 0; index < sharedCount; ++index)
		CHECK(Same(neverFiredInstance[1][index], idle));
	for (std::size_t index = 1; index < sharedCount; ++index)
		CHECK(Same(firstInstance[1][index], idle));
	CHECK(firstInstance[0].empty() && firstInstance[2].empty());
	CHECK(neverFiredInstance[0].empty() && neverFiredInstance[2].empty());
	History *storage = &firstInstance[1][0];
	for (int clientFrame = 0; clientFrame < 10; ++clientFrame)
	{
		SynchronizeCount(firstInstance[1], sharedCount);
		CHECK(&firstInstance[1][0] == storage && Same(firstInstance[1][0], Active()));
	}
	SynchronizeCount(firstInstance[1], 1);
	CHECK(firstInstance[1].size() == 1 && Same(firstInstance[1][0], Active()));
	SynchronizeCount(firstInstance[1], 3);
	CHECK(firstInstance[1].size() == 3 && Same(firstInstance[1][0], Active()));
	CHECK(Same(firstInstance[1][1], idle) && Same(firstInstance[1][2], idle));
	SynchronizeCount(firstInstance[1], 0);
	CHECK(firstInstance[1].empty());
	SynchronizeCount(firstInstance[1], 2);
	CHECK(firstInstance[1].size() == 2 && Same(firstInstance[1][0], idle));
	CHECK(Same(firstInstance[1][1], idle));
	CHECK(neverFiredInstance[1].size() == sharedCount); // no global instance mutation
}

void TestSharedCountsIndependentInstancesAndPostCallbackPreparation()
{
	using namespace rts::weapon_fire_recoil;
	std::vector<History> instanceA[3], instanceB[3];
	instanceA[1].push_back(Active()); instanceB[1].push_back(History());
	instanceB[1][0].token = 888;
	std::size_t sharedBarrelCount = 0;
	int normalized = 99;
	CHECK(!PrepareForFire(1, 3, sharedBarrelCount, 0, &instanceA[1], &normalized));
	sharedBarrelCount = 4; // lazy shared metadata becomes valid
	CHECK(PrepareForFire(1, 3, sharedBarrelCount, 3, &instanceA[1], &normalized));
	CHECK(instanceA[1].size() == 4 && Same(instanceA[1][0], Active()));
	CHECK(instanceB[1].size() == 1 && instanceB[1][0].token == 888); // another draw stays untouched
	CHECK(PrepareForFire(1, 3, sharedBarrelCount, 3, &instanceB[1], &normalized));
	CHECK(instanceB[1].size() == 4 && instanceB[1][0].token == 888);
	CHECK(Same(instanceA[1][0], Active()));
	CHECK(instanceA[0].empty() && instanceA[2].empty() && instanceB[0].empty() && instanceB[2].empty());
	// The production caller re-reads current metadata after FX callbacks.
	// These are helper inputs, not a copied simulation of the draw/FX routine.
	int beforeCallbackIndex = 3;
	CHECK(NormalizeBarrelIndex(beforeCallbackIndex, sharedBarrelCount, &beforeCallbackIndex));
	sharedBarrelCount = 1;
	CHECK(PrepareForFire(1, 3, sharedBarrelCount, beforeCallbackIndex, &instanceA[1], &normalized));
	CHECK(normalized == 0 && instanceA[1].size() == 1 && Same(instanceA[1][0], Active()));
	CHECK(instanceB[1].size() == 4); // private sizing and history stay private
	sharedBarrelCount = 5;
	CHECK(PrepareForFire(1, 3, sharedBarrelCount, normalized, &instanceA[1], &normalized));
	CHECK(normalized == 0 && instanceA[1].size() == 5 && Same(instanceA[1][0], Active()));
	const History idle;
	for (std::size_t index = 1; index < instanceA[1].size(); ++index) CHECK(Same(instanceA[1][index], idle));
}
}

int main()
{
	TestIndexAndSlot();
	TestLazyGrowthHistoryAndShrink();
	TestInvalidAdmissionPreservesState();
	TestMixedSlotAndClientAdmissionBeforeAnyFire();
	TestSharedCountsIndependentInstancesAndPostCallbackPreparation();
	if (failures) return 1;
	std::printf("weapon fire recoil shared helper contracts passed\n");
	return 0;
}
