/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

////////////////////////////////////////////////////////////////////////////////
//																																						//
//  (c) 2001-2003 Electronic Arts Inc.																				//
//																																						//
////////////////////////////////////////////////////////////////////////////////

// AISkirmishPlayer.cpp
// Computerized opponent
// Author: Michael S. Booth, January 2002

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include <algorithm>

#include "Common/GameMemory.h"
#include "Common/GlobalData.h"
#include "Common/Player.h"
#include "Common/PlayerTemplate.h"
#include "Common/PlayerList.h"
#include "Common/Recorder.h"
#include "Common/RandomValue.h"
#include "Common/Team.h"
#include "Common/ThingFactory.h"
#include "Common/BuildAssistant.h"
#include "Common/SpecialPower.h"
#include "Common/ThingTemplate.h"
#include "Common/Upgrade.h"
#include "Common/WellKnownKeys.h"
#include "Common/Xfer.h"
#include "GameLogic/GameLogic.h"
#include "GameLogic/Object.h"
#include "GameLogic/AISkirmishPlayer.h"
#include "GameLogic/SidesList.h"
#include "GameLogic/AI.h"
#include "GameLogic/AIPathfind.h"
#include "GameLogic/TerrainLogic.h"
#include "GameLogic/Module/AIUpdate.h"
#include "GameLogic/Module/BodyModule.h"
#include "GameLogic/Module/ContainModule.h"
#include "GameLogic/Module/DozerAIUpdate.h"
#include "GameLogic/Module/RebuildHoleBehavior.h"
#include "GameLogic/Module/SpecialPowerModule.h"
#include "GameLogic/Module/SupplyTruckAIUpdate.h"
#include "GameLogic/Module/WorkerAIUpdate.h"
#include "GameLogic/Module/SupplyWarehouseDockUpdate.h"
#include "GameLogic/Module/UpdateModule.h"
#include "GameLogic/PartitionManager.h"
#include "GameLogic/ScriptEngine.h"
#include "GameLogic/SkirmishAIDecision.h"
#include "GameLogic/SkirmishAIRecovery.h"
#include "GameLogic/Weapon.h"
#include "GameLogic/WeaponSet.h"
#include "GameLogic/Module/ProductionUpdate.h"
#include "GameClient/ControlBar.h"
#include "GameClient/TerrainVisual.h"
#include "GameNetwork/GameInfo.h"


#define USE_DOZER 1

static Bool ShouldUseCurrentSkirmishAIBehavior()
{
	return ShouldUseSkirmishAICurrentBehavior(
		TheGameLogic->isInReplayGame(),
		TheRecorder ? TheRecorder->getSkirmishAIReplayEpoch() : SKIRMISH_AI_REPLAY_EPOCH_LEGACY);
}

static Bool ShouldUseCurrentSkirmishAIStrategyControllerBehavior()
{
	return TheGameLogic && ShouldUseSkirmishAIStrategyBehavior(
		TheGameLogic->isInReplayGame(),
		TheRecorder ? TheRecorder->getSkirmishAIReplayEpoch() :
			SKIRMISH_AI_REPLAY_EPOCH_LEGACY);
}

static Bool ShouldUseCurrentSkirmishAIProductionBehavior()
{
	return TheGameLogic && ShouldUseSkirmishAIProductionBehavior(
		TheGameLogic->isInReplayGame(),
		TheRecorder ? TheRecorder->getSkirmishAIReplayEpoch() :
			SKIRMISH_AI_REPLAY_EPOCH_LEGACY);
}

static void GatherSkirmishAICollectorRoles(
	Player *owner, std::map<ObjectID, Bool> *roles)
{
	roles->clear();
	if (!owner || !TheGameLogic)
		return;
	for (Object *object = TheGameLogic->getFirstObject(); object;
		object = object->getNextObject()) {
		if (object->getControllingPlayer() != owner)
			continue;
		AIUpdateInterface *ai = object->getAIUpdateInterface();
		WorkerAIInterface *worker = ai ? ai->getWorkerAIInterface() : nullptr;
		if (worker)
			(*roles)[object->getID()] = worker->isStage3CollectorRole();
	}
}

static Bool IsSkirmishAIStrategicLaunchPower(SpecialPowerType type)
{
	switch (type) {
		case SPECIAL_NEUTRON_MISSILE:
		case NUKE_SPECIAL_NEUTRON_MISSILE:
		case SUPW_SPECIAL_NEUTRON_MISSILE:
		case SPECIAL_SCUD_STORM:
		case SPECIAL_PARTICLE_UPLINK_CANNON:
		case SUPW_SPECIAL_PARTICLE_UPLINK_CANNON:
		case LAZR_SPECIAL_PARTICLE_UPLINK_CANNON:
		case SUPR_SPECIAL_CRUISE_MISSILE:
			return true;
		default:
			return false;
	}
}

static Bool IsUsableSkirmishAIStrategicSource(
	Object *object, const Player *owner)
{
	return object && owner && object->getControllingPlayer() == owner &&
		object->isKindOf(KINDOF_FS_SUPERWEAPON) &&
		!object->isKindOf(KINDOF_REBUILD_HOLE) &&
		!object->isEffectivelyDead() && !object->isDestroyed() &&
		!object->testStatus(OBJECT_STATUS_SOLD) &&
		!object->testStatus(OBJECT_STATUS_UNDER_CONSTRUCTION) &&
		!object->testStatus(OBJECT_STATUS_RECONSTRUCTING);
}

static Bool HasSkirmishAIStrategicPowerModule(
	Object *source, const SpecialPowerTemplate *requestedPower)
{
	if (!source)
		return false;
	if (requestedPower)
		return IsSkirmishAIStrategicLaunchPower(
			requestedPower->getSpecialPowerType()) &&
			source->getSpecialPowerModule(requestedPower) != nullptr;
	if (!TheSpecialPowerStore)
		return false;
	const Int powerCount = TheSpecialPowerStore->getNumSpecialPowers();
	for (Int powerIndex = 0; powerIndex < powerCount; ++powerIndex) {
		const SpecialPowerTemplate *power =
			TheSpecialPowerStore->getSpecialPowerTemplateByIndex(
				(UnsignedInt)powerIndex);
		if (power && IsSkirmishAIStrategicLaunchPower(
				power->getSpecialPowerType()) &&
			source->getSpecialPowerModule(power))
			return true;
	}
	return false;
}

static Bool IsSkirmishAIPlannedSuperweaponObject(
	Player *owner, const Object *object)
{
	if (!owner || !object || !object->getTemplate() || !TheThingFactory)
		return false;
	for (BuildListInfo *build = owner->getBuildList(); build;
		build = build->getNext()) {
		const ThingTemplate *plan =
			TheThingFactory->findTemplate(build->getTemplateName());
		if (plan && plan->isKindOf(KINDOF_FS_SUPERWEAPON) &&
			object->getTemplate()->isEquivalentTo(plan))
			return true;
	}
	return false;
}

static Object *FindSkirmishAISuperweaponConstruction(Player *owner)
{
	if (!owner || !TheGameLogic)
		return nullptr;
	Object *selected = nullptr;
	for (Object *object = TheGameLogic->getFirstObject(); object;
		object = object->getNextObject()) {
		if (!ShouldKeepSkirmishAISuperweaponConstructionPending(
				object != nullptr,
				object->getControllingPlayer() == owner,
				object->isKindOf(KINDOF_FS_SUPERWEAPON),
				object->isKindOf(KINDOF_REBUILD_HOLE),
				object->isEffectivelyDead(), object->isDestroyed(),
				object->testStatus(OBJECT_STATUS_SOLD),
				object->testStatus(OBJECT_STATUS_UNDER_CONSTRUCTION),
				object->testStatus(OBJECT_STATUS_RECONSTRUCTING)))
			continue;
		if (!IsSkirmishAIPlannedSuperweaponObject(owner, object))
			continue;
		if (!selected || object->getID() < selected->getID())
			selected = object;
	}
	return selected;
}

static Object *FindSkirmishAIStrategicSource(
	Player *owner, const SpecialPowerTemplate *requestedPower)
{
	if (!owner || !TheGameLogic)
		return nullptr;
	Object *selected = nullptr;
	for (Object *object = TheGameLogic->getFirstObject(); object;
		object = object->getNextObject()) {
		if (!IsUsableSkirmishAIStrategicSource(object, owner) ||
			!HasSkirmishAIStrategicPowerModule(object, requestedPower))
			continue;
		if (requestedPower) {
			SpecialPowerModuleInterface *module =
				object->getSpecialPowerModule(requestedPower);
			if (!module || !module->isReady() || !module->isDispatchable())
				continue;
		}
		if (!selected || object->getID() < selected->getID())
			selected = object;
	}
	return selected;
}

static Bool IsSkirmishAIProducerIDBefore(Object *left, Object *right)
{
	return left->getID() < right->getID();
}

static Bool IsSkirmishAIReinforcementTeamIDBefore(Team *left, Team *right)
{
	return left->getID() < right->getID();
}

static Bool HasSkirmishAIReinforcementDeficit(Team *team)
{
	const TeamPrototype *prototype = team ? team->getPrototype() : nullptr;
	const TeamTemplateInfo *teamInfo = prototype
		? prototype->getTemplateInfo() : nullptr;
	if (!teamInfo || !TheThingFactory)
		return false;
	for (Int unitIndex = 0; unitIndex < teamInfo->m_numUnitsInfo;
		++unitIndex) {
		const TCreateUnitsInfo *unitInfo = &teamInfo->m_unitsInfo[unitIndex];
		if (unitInfo->maxUnits < 1)
			continue;
		const ThingTemplate *thing =
			TheThingFactory->findTemplate(unitInfo->unitThingName);
		if (!thing)
			continue;
		Int count = 0;
		team->countObjectsByThingTemplate(1, &thing, false, &count);
		if (count < unitInfo->maxUnits)
			return true;
	}
	return false;
}

static void FindSkirmishAIProductionProducers(
	Player *owner, std::vector<Object *> *factories)
{
	if (!owner || !factories || !TheGameLogic)
		return;
	for (Object *factory = TheGameLogic->getFirstObject(); factory;
		factory = factory->getNextObject()) {
		if (!IsSkirmishAIOperationalProducer(
				factory->getControllingPlayer() == owner,
				factory->isEffectivelyDead(), factory->isDestroyed(),
				factory->isKindOf(KINDOF_REBUILD_HOLE),
				factory->testStatus(OBJECT_STATUS_SOLD),
				factory->testStatus(OBJECT_STATUS_UNDER_CONSTRUCTION),
				factory->testStatus(OBJECT_STATUS_RECONSTRUCTING),
				factory->isDisabled(),
				factory->isDisabledByType(DISABLED_UNMANNED),
				factory->getProductionUpdateInterface() != nullptr))
			continue;
		factories->push_back(factory);
	}
	std::sort(factories->begin(), factories->end(),
		IsSkirmishAIProducerIDBefore);
}

static void FindSkirmishAICompatibleProducers(
	Player *owner, const ThingTemplate *thing,
	std::vector<Object *> *factories)
{
	if (!owner || !thing || !factories || !TheGameLogic)
		return;
	FindSkirmishAIProductionProducers(owner, factories);
	for (std::vector<Object *>::iterator factory = factories->begin();
		factory != factories->end();) {
		if (!TheBuildAssistant->isPossibleToMakeUnit(*factory, thing)) {
			factory = factories->erase(factory);
			continue;
		}
		++factory;
	}
}

static Object *QueueSkirmishAIUnitAtCompatibleProducer(
	const std::vector<Object *> &factories, const ThingTemplate *thing,
	Bool busyOK, ProductionID *queuedProductionID)
{
	if (queuedProductionID)
		*queuedProductionID = PRODUCTIONID_INVALID;
	if (!thing)
		return nullptr;
	for (Int pass = 0; pass < (busyOK ? 2 : 1); ++pass) {
		for (std::vector<Object *>::const_iterator factory = factories.begin();
			factory != factories.end(); ++factory) {
			ProductionUpdateInterface *production =
				(*factory)->getProductionUpdateInterface();
			const Bool busy = production->getProductionCount() > 0;
			if ((pass == 0 && busy) || (pass == 1 && !busy))
				continue;
			const ProductionID productionID = production->requestUniqueUnitID();
			if (production->queueCreateUnit(thing, productionID)) {
				if (queuedProductionID)
					*queuedProductionID = productionID;
				return *factory;
			}
		}
	}
	return nullptr;
}

static Bool IsSkirmishAIStrategyAuthorizedStructure(
	const ThingTemplate *thing)
{
	return thing &&
		(thing->isKindOf(KINDOF_COMMANDCENTER) ||
		 thing->isKindOf(KINDOF_FS_POWER) ||
		 IsSkirmishAIAlternateIncomeStructure(
			 thing->isKindOf(KINDOF_CASH_GENERATOR),
			 thing->isKindOf(KINDOF_FS_SUPPLY_DROPZONE),
			 thing->isKindOf(KINDOF_FS_BLACK_MARKET),
			 thing->isKindOf(KINDOF_FS_INTERNET_CENTER)) ||
		 thing->isKindOf(KINDOF_FS_SUPPLY_CENTER) ||
		 thing->isKindOf(KINDOF_FS_FACTORY) ||
		 thing->isKindOf(KINDOF_FS_BARRACKS) ||
		 thing->isKindOf(KINDOF_FS_WARFACTORY) ||
		 thing->isKindOf(KINDOF_FS_AIRFIELD));
}

static Int ClampSkirmishStrategyPercent(Int value)
{
	if (value < 0)
		return 0;
	if (value > 100)
		return 100;
	return value;
}

static Int GetSkirmishStrategyHealthPercent(const Object *object)
{
	if (!object || !object->getBodyModule())
		return 100;
	const Real maximum = object->getBodyModule()->getMaxHealth();
	if (maximum <= 0.0f)
		return 100;
	return ClampSkirmishStrategyPercent(
		(Int)(object->getBodyModule()->getHealth() * 100.0f / maximum + 0.5f));
}

static Int AddSkirmishStrategyValue(Int total, Int value)
{
	if (value <= 0)
		return total;
	if (total > 2147483647 - value)
		return 2147483647;
	return total + value;
}

static Int GetSkirmishStrategyValuePercent(Int value, Int scale)
{
	if (value <= 0)
		return 0;
	if (scale <= 0 || value >= scale)
		return 100;
	return ClampSkirmishStrategyPercent(
		(Int)((__int64)value * 100 / scale));
}

static Int GetSkirmishStrategyCategoryPercent(
	Int healthTotal, Int expectedCount)
{
	if (expectedCount <= 0)
		return 100;
	return ClampSkirmishStrategyPercent(
		(Int)((__int64)healthTotal / expectedCount));
}

static Bool IsSkirmishStrategyCombatObject(const Object *object)
{
	return object && !object->isKindOf(KINDOF_STRUCTURE) &&
		!object->isKindOf(KINDOF_DOZER) &&
		!object->isKindOf(KINDOF_HARVESTER) &&
		!object->isKindOf(KINDOF_PROJECTILE) &&
		!object->isKindOf(KINDOF_MINE) &&
		!object->isKindOf(KINDOF_INERT) && object->isAbleToAttack();
}

static Bool IsSkirmishStrategyStaticTarget(const Object *object)
{
	return object && object->isKindOf(KINDOF_STRUCTURE) &&
		object->isKindOf(KINDOF_IMMOBILE) && !object->isContained();
}

enum {
	MAX_SKIRMISH_STRATEGY_TARGET_CANDIDATES = 4,
	MAX_SKIRMISH_STRATEGY_GROUND_PROBES_PER_TARGET = 4,
	MAX_SKIRMISH_STRATEGY_QUICK_PATH_QUERIES = 16
};

struct SkirmishStrategyCapabilityCandidate
{
	Object *object;
	Int value;
};

static Bool IsSkirmishStrategyOffensiveTeamType(
	Team *team, Player *player)
{
	if (!team || !player || team == player->getDefaultTeam())
		return false;
	const TeamPrototype *prototype = team->getPrototype();
	const TeamTemplateInfo *info = prototype ?
		prototype->getTemplateInfo() : 0;
	return info && !info->m_isBaseDefense && !info->m_isPerimeterDefense;
}

static Bool IsSkirmishStrategyOffensiveTeam(Team *team, Player *player)
{
	return IsSkirmishStrategyOffensiveTeamType(team, player) &&
		team->isActive();
}

static Bool IsSkirmishStrategyPotentialOffensiveRecipient(
	Object *object, Player *player, Team *team)
{
	return object && player && object->getControllingPlayer() == player &&
		object->getTeam() == team &&
		!object->isEffectivelyDead() && !object->isDestroyed() &&
		!object->testStatus(OBJECT_STATUS_SOLD) &&
		!object->testStatus(OBJECT_STATUS_UNDER_CONSTRUCTION) &&
		!object->isContained() &&
		IsSkirmishStrategyCombatObject(object) && object->getAIUpdateInterface();
}

static Bool IsSkirmishStrategyOffensiveRecipient(
	Object *object, Player *player)
{
	Team *team = object ? object->getTeam() : 0;
	return IsSkirmishStrategyPotentialOffensiveRecipient(
		object, player, team) && IsSkirmishStrategyOffensiveTeam(team, player);
}

static void InsertSkirmishStrategyTargetCandidate(
	SkirmishStrategyCapabilityCandidate *candidates, Int *candidateCount,
	Object *object, Int value)
{
	Int insertAt = *candidateCount;
	Int index;
	for (index = 0; index < *candidateCount; ++index) {
		if (value > candidates[index].value ||
			(value == candidates[index].value &&
			 object->getID() < candidates[index].object->getID())) {
			insertAt = index;
			break;
		}
	}
	if (insertAt >= MAX_SKIRMISH_STRATEGY_TARGET_CANDIDATES)
		return;
	Int last = *candidateCount < MAX_SKIRMISH_STRATEGY_TARGET_CANDIDATES ?
		*candidateCount : MAX_SKIRMISH_STRATEGY_TARGET_CANDIDATES - 1;
	for (index = last; index > insertAt; --index)
		candidates[index] = candidates[index - 1];
	candidates[insertAt].object = object;
	candidates[insertAt].value = value;
	if (*candidateCount < MAX_SKIRMISH_STRATEGY_TARGET_CANDIDATES)
		++(*candidateCount);
}

static Bool IsSkirmishStrategyCapabilityCandidateBefore(
	const SkirmishStrategyCapabilityCandidate &left,
	const SkirmishStrategyCapabilityCandidate &right)
{
	return left.value > right.value ||
		(left.value == right.value && left.object->getID() < right.object->getID());
}

static void SortSkirmishStrategyCapabilityCandidates(
	std::vector<SkirmishStrategyCapabilityCandidate> *candidates)
{
	std::sort(candidates->begin(), candidates->end(),
		IsSkirmishStrategyCapabilityCandidateBefore);
}

static void AppendSkirmishStrategyCapabilityCandidate(
	std::vector<SkirmishStrategyCapabilityCandidate> *candidates,
	Object *object, Int value)
{
	size_t index;
	for (index = 0; index < candidates->size(); ++index) {
		if ((*candidates)[index].object->getID() == object->getID())
			return;
	}
	SkirmishStrategyCapabilityCandidate candidate;
	candidate.object = object;
	candidate.value = value;
	candidates->push_back(candidate);
}

static Bool IsSkirmishStrategyReadyTeamActivationDue(
	const TeamInQueue *readyTeam)
{
	if (!readyTeam || readyTeam->m_reinforcement || !readyTeam->m_team ||
		!readyTeam->m_team->getPrototype())
		return false;
	Bool allIdle = readyTeam->m_team->isIdle();
	Bool anyIdle = false;
	{
		for (DLINK_ITERATOR<Object> member =
				readyTeam->m_team->iterate_TeamMemberList();
			!member.done(); member.advance()) {
			Object *object = member.cur();
			if (object && object->getAI() && object->getAI()->isIdle())
				anyIdle = true;
		}
	}
	const TeamTemplateInfo *info =
		readyTeam->m_team->getPrototype()->getTemplateInfo();
	if (anyIdle && info && info->m_executeActions) {
		const Script *script = TheScriptEngine->findScriptByName(
			info->m_productionCondition);
		if (script && script->getAction())
			allIdle = true;
	}
	if (readyTeam->m_frameStarted + 60 * LOGICFRAMES_PER_SECOND <
		TheGameLogic->getFrame())
		allIdle = true;
	return allIdle;
}

static Bool HasSkirmishStrategyTargetCapability(
	Object *target,
	const std::vector<SkirmishStrategyCapabilityCandidate> &airAttackers,
	const std::vector<SkirmishStrategyCapabilityCandidate> &groundAttackers,
	Int *quickPathQueryCount)
{
	size_t index;
	for (index = 0; index < airAttackers.size(); ++index) {
		if (airAttackers[index].object->getAbleToAttackSpecificObject(
				ATTACK_NEW_TARGET, target, CMD_FROM_AI) != ATTACKRESULT_NOT_POSSIBLE)
			return true;
	}
	if (!TheAI || !TheAI->pathfinder())
		return false;
	Int capableGroundProbeCount = 0;
	for (index = 0; index < groundAttackers.size(); ++index) {
		Object *attacker = groundAttackers[index].object;
		if (attacker->getAbleToAttackSpecificObject(
				ATTACK_NEW_TARGET, target, CMD_FROM_AI) == ATTACKRESULT_NOT_POSSIBLE)
			continue;
		if (capableGroundProbeCount >= MAX_SKIRMISH_STRATEGY_GROUND_PROBES_PER_TARGET)
			break;
		if (*quickPathQueryCount >= MAX_SKIRMISH_STRATEGY_QUICK_PATH_QUERIES)
			return false;
		++capableGroundProbeCount;
		++(*quickPathQueryCount);
		AIUpdateInterface *ai = attacker->getAIUpdateInterface();
		if (ai && TheAI->pathfinder()->clientSafeQuickDoesPathExist(
				ai->getLocomotorSet(), attacker->getPosition(), target->getPosition()))
			return true;
	}
	return false;
}

struct SkirmishStrategyGroupRecipientContext
{
	Player *player;
	AIGroup *group;
	Bool found;
};

static void CollectSkirmishStrategyGroupRecipient(Object *object, void *userData)
{
	SkirmishStrategyGroupRecipientContext *context =
		(SkirmishStrategyGroupRecipientContext *)userData;
	if (!context || !IsSkirmishStrategyOffensiveRecipient(object, context->player))
		return;
	context->found = true;
	if (context->group)
		context->group->add(object);
}

static Bool IsSkirmishStrategyIntelEligible(
	const Object *object, const Player *observer)
{
	if (!object || !observer)
		return false;
	const ObjectShroudStatus shroud =
		object->getShroudedStatus(observer->getPlayerIndex());
	const Bool visible = shroud == OBJECTSHROUD_CLEAR ||
		shroud == OBJECTSHROUD_PARTIAL_CLEAR;
	if (!visible)
		return false;
	return IsSkirmishAIIntelEligible(
		object->isKindOf(KINDOF_STRUCTURE), visible, false,
		object->testStatus(OBJECT_STATUS_STEALTHED),
		object->testStatus(OBJECT_STATUS_DETECTED),
		object->testStatus(OBJECT_STATUS_MASKED));
}

struct SkirmishStrategyExpectedAssets
{
	Int commandCenters;
	Int economyStructures;
	Int powerStructures;
	Int productionStructures;
};

static SkirmishStrategyExpectedAssets GetSkirmishStrategyExpectedAssets(
	Player *player)
{
	SkirmishStrategyExpectedAssets result;
	result.commandCenters = 0;
	result.economyStructures = 0;
	result.powerStructures = 0;
	result.productionStructures = 0;
	for (BuildListInfo *info = player ? player->getBuildList() : nullptr;
		info; info = info->getNext()) {
		const ThingTemplate *plan =
			TheThingFactory->findTemplate(info->getTemplateName());
		if (!plan)
			continue;
		if (plan->isKindOf(KINDOF_COMMANDCENTER))
			++result.commandCenters;
		else if (plan->isKindOf(KINDOF_FS_SUPPLY_CENTER) ||
			plan->isKindOf(KINDOF_CASH_GENERATOR))
			++result.economyStructures;
		else if (plan->isKindOf(KINDOF_FS_POWER) &&
			!plan->isKindOf(KINDOF_CASH_GENERATOR))
			++result.powerStructures;
		else if (plan->isKindOf(KINDOF_FS_BARRACKS) ||
			plan->isKindOf(KINDOF_FS_WARFACTORY) ||
			plan->isKindOf(KINDOF_FS_AIRFIELD))
			++result.productionStructures;
	}
	if (result.commandCenters < 1)
		result.commandCenters = 1;
	return result;
}

static Bool ShouldUseCurrentSkirmishAIRecoveryNativeHoleOwnership()
{
	return ShouldUseSkirmishAIRecoveryNativeHoleOwnership(
		TheGameLogic && TheGameLogic->isInReplayGame(),
		TheRecorder ? TheRecorder->getSkirmishAIReplayEpoch() : SKIRMISH_AI_REPLAY_EPOCH_LEGACY);
}

static Bool ShouldUseCurrentSkirmishAIRecoveryCancellationOwnership()
{
	return ShouldUseSkirmishAIRecoveryCancellationOwnership(
		TheGameLogic && TheGameLogic->isInReplayGame(),
		TheRecorder ? TheRecorder->getSkirmishAIReplayEpoch() :
			SKIRMISH_AI_REPLAY_EPOCH_LEGACY);
}

static Bool ShouldUseCurrentSkirmishAIRecoveryUnownedQueueFailover()
{
	return ShouldUseSkirmishAIRecoveryUnownedQueueFailover(
		TheGameLogic && TheGameLogic->isInReplayGame(),
		TheRecorder ? TheRecorder->getSkirmishAIReplayEpoch() :
			SKIRMISH_AI_REPLAY_EPOCH_LEGACY);
}

static Bool ShouldUseCurrentSkirmishAIRecoveryBoundedFailover()
{
	return ShouldUseSkirmishAIRecoveryBoundedFailover(
		TheGameLogic && TheGameLogic->isInReplayGame(),
		TheRecorder ? TheRecorder->getSkirmishAIReplayEpoch() :
			SKIRMISH_AI_REPLAY_EPOCH_LEGACY);
}

static Bool ShouldUseCurrentSkirmishAIRecoveryResourceWorkerPreservation()
{
	return ShouldUseSkirmishAIRecoveryResourceWorkerPreservation(
		TheGameLogic && TheGameLogic->isInReplayGame(),
		TheRecorder ? TheRecorder->getSkirmishAIReplayEpoch() :
			SKIRMISH_AI_REPLAY_EPOCH_LEGACY);
}

static Bool IsCriticalRecoveryModeEnabled(Player *player)
{
	if (!player || player->getPlayerType() != PLAYER_COMPUTER || !TheGameLogic)
		return false;

	const Bool replay = TheGameLogic->isInReplayGame();
	const Int gameMode = replay
		? (TheRecorder ? TheRecorder->getGameMode() : GAME_NONE)
		: TheGameLogic->getGameMode();
	if (!IsSkirmishAIRecoveryGameMode(gameMode))
		return false;

	return ShouldUseSkirmishAIRecoveryBehavior(
		replay,
		TheRecorder ? TheRecorder->getSkirmishAIReplayEpoch() : SKIRMISH_AI_REPLAY_EPOCH_LEGACY);
}

struct SkirmishAIRecoveryOffset
{
	Int x;
	Int y;
};

// Keep placement deterministic and bounded.  The final position is tested by
// BuildAssistant and by the builder's pathfinder before any object is created.
static const SkirmishAIRecoveryOffset g_skirmishAIRecoveryOffsets[] =
{
	{ 0, 0 },
	{ 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 },
	{ 1, 1 }, { -1, 1 }, { 1, -1 }, { -1, -1 },
	{ 2, 0 }, { -2, 0 }, { 0, 2 }, { 0, -2 },
	{ 2, 1 }, { -2, 1 }, { 2, -1 }, { -2, -1 },
	{ 1, 2 }, { -1, 2 }, { 1, -2 }, { -1, -2 },
	{ 3, 0 }, { -3, 0 }, { 0, 3 }, { 0, -3 },
	{ 3, 1 }, { -3, 1 }, { 3, -1 }, { -3, -1 },
	{ 1, 3 }, { -1, 3 }, { 1, -3 }, { -1, -3 },
	{ 4, 0 }, { -4, 0 }, { 0, 4 }, { 0, -4 }
};

static const Int g_skirmishAIRecoveryOffsetCount =
	sizeof(g_skirmishAIRecoveryOffsets) / sizeof(g_skirmishAIRecoveryOffsets[0]);

static Bool HasSkirmishAICommandSetForTemplate(
	const AsciiString &commandSetName, const ThingTemplate *product)
{
	if (!product || !TheControlBar)
		return false;
	const CommandSet *commandSet = TheControlBar->findCommandSet(commandSetName);
	if (!commandSet)
		return false;
	for (Int command = 0; command < MAX_COMMANDS_PER_SET; ++command) {
		const CommandButton *button = commandSet->getCommandButton(command);
		if (button &&
			(button->getCommandType() == GUI_COMMAND_UNIT_BUILD ||
			 button->getCommandType() == GUI_COMMAND_DOZER_CONSTRUCT) &&
			button->getThingTemplate() &&
			button->getThingTemplate()->isEquivalentTo(product))
			return true;
	}
	return false;
}

static Bool HasSkirmishAICommandForTemplate(
	const Object *producer, const ThingTemplate *product)
{
	if (!producer || !product)
		return false;
	return HasSkirmishAICommandSetForTemplate(
		producer->getCommandSetString(), product);
}

static Bool IsLiveSkirmishAIRecoveryObject(
	const Object *object, const Player *owner)
{
	return object && !object->isDestroyed() && !object->isEffectivelyDead() &&
		!object->testStatus(OBJECT_STATUS_SOLD) &&
		(!owner || object->getControllingPlayer() == owner);
}

static Bool CanSkirmishAIRecoveryUpdateAdvance(
	Object *object, UpdateModule *update)
{
	if (!object || !update)
		return false;
	const DisabledMaskType disabled = object->getDisabledFlags();
	const DisabledMaskType accepted = update->getDisabledTypesToProcess();
#if RETAIL_COMPATIBLE_CRC
	return !disabled.any() || disabled.anyIntersectionWith(accepted);
#else
	return accepted.testForAll(disabled);
#endif
}

static Bool HasSkirmishAIRecoveryProduction(
	Object *factory, const ThingTemplate *product)
{
	if (!factory || !product || !IsLiveSkirmishAIRecoveryObject(
		factory, factory->getControllingPlayer()))
		return false;
	ProductionUpdateInterface *production =
		factory->getProductionUpdateInterface();
	if (!production)
		return false;
	for (const ProductionEntry *entry = production->firstProduction(); entry;
		entry = production->nextProduction(entry)) {
		if (entry->getProductionType() == PRODUCTION_UNIT &&
			entry->getProductionObject() &&
			entry->getProductionObject()->isEquivalentTo(product) &&
			entry->getProductionQuantityRemaining() > 0)
			return true;
	}
	return false;
}

static Object *FindSkirmishAIRecoveryHoleForConstruction(
	const Player *owner, const ThingTemplate *rebuildTemplate,
	ObjectID constructionID)
{
	if (!owner || !rebuildTemplate || constructionID == INVALID_ID ||
		!TheGameLogic)
		return nullptr;

	Object *best = nullptr;
	for (Object *object = TheGameLogic->getFirstObject(); object;
		object = object->getNextObject()) {
		const Bool liveOwnedHole =
			IsLiveSkirmishAIRecoveryObject(object, owner) &&
			object->isKindOf(KINDOF_REBUILD_HOLE);
		RebuildHoleBehaviorInterface *holeAI = liveOwnedHole
			? RebuildHoleBehavior::getRebuildHoleBehaviorInterfaceFromObject(object)
			: nullptr;
		const ThingTemplate *candidateTemplate = holeAI
			? holeAI->getRebuildTemplate() : nullptr;
		if (!holeAI || !IsSkirmishAIRecoveryHoleConstructionMatch(
				liveOwnedHole, candidateTemplate != nullptr,
				candidateTemplate &&
					candidateTemplate->isEquivalentTo(rebuildTemplate),
				holeAI->getReconstructedBuildingID(), constructionID))
			continue;
		if (!best || object->getID() < best->getID())
			best = object;
	}
	return best;
}

static Bool IsSkirmishAIRecoveryWorkerReservedByNativeHole(
	const Player *owner, const Object *worker)
{
	if (!ShouldUseCurrentSkirmishAIRecoveryNativeHoleOwnership() ||
		!owner || !worker || !TheGameLogic)
		return false;
	for (Object *object = TheGameLogic->getFirstObject(); object;
		object = object->getNextObject()) {
		const Bool liveOwnedHole =
			IsLiveSkirmishAIRecoveryObject(object, owner) &&
			object->isKindOf(KINDOF_REBUILD_HOLE);
		RebuildHoleBehaviorInterface *holeAI = liveOwnedHole
			? RebuildHoleBehavior::getRebuildHoleBehaviorInterfaceFromObject(object)
			: nullptr;
		if (holeAI && IsSkirmishAIRecoveryReservedNativeWorker(
				liveOwnedHole, holeAI->getWorkerID(), worker->getID(), INVALID_ID))
			return true;
	}
	return false;
}

static Bool HasSkirmishAIRecoveryFactoryFinisher(
	Object *factory, const Player *owner, Bool *blockedFinisher)
{
	if (blockedFinisher)
		*blockedFinisher = false;
	if (!factory || !factory->testStatus(OBJECT_STATUS_UNDER_CONSTRUCTION))
		return true;
	if (!TheGameLogic)
		return false;
	Object *builder = TheGameLogic->findObjectByID(factory->getBuilderID());
	const Bool hasLiveAssignedBuilder =
		IsLiveSkirmishAIRecoveryObject(builder, owner);
	if (hasLiveAssignedBuilder) {
		AIUpdateInterface *builderAI = builder->getAIUpdateInterface();
		DozerAIInterface *dozerAI = builderAI
			? builderAI->getDozerAIInterface() : nullptr;
		const Coord3D *buildDock = dozerAI
			? dozerAI->getDockPoint(
				DOZER_TASK_BUILD, DOZER_DOCK_POINT_ACTION) : nullptr;
		if (!buildDock && dozerAI)
			buildDock = dozerAI->getDockPoint(
				DOZER_TASK_BUILD, DOZER_DOCK_POINT_START);
		if (IsSkirmishAIRecoveryFactoryFinisherUsable(
			builder->isContained(),
			builder->isDisabledByType(DISABLED_UNMANNED),
			dozerAI != nullptr,
			dozerAI && dozerAI->isTaskPending(DOZER_TASK_BUILD),
			dozerAI && dozerAI->getTaskTarget(DOZER_TASK_BUILD) ==
				factory->getID(),
			buildDock != nullptr,
			builderAI && buildDock && builderAI->isPathAvailable(buildDock),
			CanSkirmishAIRecoveryUpdateAdvance(builder, builderAI)))
			return true;
		// A live assigned worker that cannot advance remains a bounded generic
		// recovery route.  Do not let the native-hole respawn fallback hide it.
		if (blockedFinisher)
			*blockedFinisher = true;
		return false;
	}

	// A GLA rebuild scaffold may temporarily have no worker while its hole's
	// respawn timer is active.  Preserve that self-rebuild route, but only when
	// the live hole owns this exact reconstruction template.
	Object *hole = TheGameLogic->findObjectByID(factory->getProducerID());
	RebuildHoleBehaviorInterface *holeAI = hole
		? RebuildHoleBehavior::getRebuildHoleBehaviorInterfaceFromObject(hole)
		: nullptr;
	if (!(IsLiveSkirmishAIRecoveryObject(hole, owner) && holeAI &&
		factory->getTemplate() && holeAI->getRebuildTemplate() &&
		holeAI->getRebuildTemplate()->isEquivalentTo(factory->getTemplate()) &&
		holeAI->getReconstructedBuildingID() == factory->getID()))
		hole = FindSkirmishAIRecoveryHoleForConstruction(
			owner, factory->getTemplate(), factory->getID());
	return hole != nullptr;
}

static Bool IsSkirmishAIRecoveryPrimaryHole(
	Object *hole, const Player *owner, const ThingTemplate *primaryTemplate,
	const BuildListInfo *info, ObjectID trackedID)
{
	if (!IsLiveSkirmishAIRecoveryObject(hole, owner) ||
		!hole->isKindOf(KINDOF_REBUILD_HOLE) || !primaryTemplate)
		return false;
	RebuildHoleBehaviorInterface *holeAI =
		RebuildHoleBehavior::getRebuildHoleBehaviorInterfaceFromObject(hole);
	if (!holeAI || !holeAI->getRebuildTemplate() ||
		!holeAI->getRebuildTemplate()->isEquivalentTo(primaryTemplate))
		return false;
	return trackedID == hole->getID() ||
		(info && info->getObjectID() == hole->getID()) ||
		(trackedID != INVALID_ID &&
		 (holeAI->getSpawnerID() == trackedID ||
		  holeAI->getReconstructedBuildingID() == trackedID));
}

static Bool IsSkirmishAIRecoveryLocationSafe(
	Player *player, const Coord3D *position, const ThingTemplate *thing)
{
	if (!player || !position || !thing || !TheAI || !ThePartitionManager)
		return false;
	Real radius = TheAI->getAiData()->m_supplyCenterSafeRadius +
		thing->getTemplateGeometryInfo().getBoundingCircleRadius();

	PartitionFilterPlayerAffiliation filterTeam(
		player, (ALLOW_ALLIES | ALLOW_NEUTRAL), false);
	PartitionFilterAlive filterAlive;
	PartitionFilterInsignificantBuildings filterInsignificant(true, false);
	PartitionFilterRejectByKindOf filterHarvesters(
		MAKE_KINDOF_MASK(KINDOF_HARVESTER), KINDOFMASK_NONE);
	PartitionFilterRejectByKindOf filterDozer(
		MAKE_KINDOF_MASK(KINDOF_DOZER), KINDOFMASK_NONE);
	PartitionFilter *filters[] = {
		&filterTeam, &filterAlive, &filterInsignificant,
		&filterHarvesters, &filterDozer, nullptr
	};
	ObjectIterator *iter = ThePartitionManager->iterateObjectsInRange(
		position, radius, FROM_BOUNDINGSPHERE_2D, filters);
	MemoryPoolObjectHolder hold(iter);
	for (Object *object = iter->first(); object; object = iter->next()) {
		ObjectShroudStatus shroud =
			object->getShroudedStatus(player->getPlayerIndex());
		Bool visible = shroud == OBJECTSHROUD_CLEAR ||
			shroud == OBJECTSHROUD_PARTIAL_CLEAR;
		Bool fogged = shroud == OBJECTSHROUD_FOGGED;
		if (IsSkirmishAIIntelEligible(
			object->isKindOf(KINDOF_STRUCTURE), visible, fogged,
			object->testStatus(OBJECT_STATUS_STEALTHED),
			object->testStatus(OBJECT_STATUS_DETECTED),
			object->testStatus(OBJECT_STATUS_MASKED)))
			return false;
	}
	return true;
}

struct SkirmishAIRecoveryBuilderCandidate
{
	Object *object;
	Bool idle;
	Real distance;
};

static Bool IsSkirmishAIRecoveryBuilderCandidateBetter(
	const SkirmishAIRecoveryBuilderCandidate &candidate,
	const SkirmishAIRecoveryBuilderCandidate &current)
{
	return (candidate.idle && !current.idle) ||
		(candidate.idle == current.idle &&
		 (candidate.distance < current.distance ||
		  (candidate.distance == current.distance &&
		   candidate.object->getID() < current.object->getID())));
}

static Bool RecoverSkirmishAIContainedBuilders(
	Player *player, const ThingTemplate *primaryTemplate)
{
	if (!player || !TheGameLogic || !primaryTemplate)
		return false;
	Bool hasContainedBuilderRoute = false;
	for (Object *object = TheGameLogic->getFirstObject(); object;
		object = object->getNextObject()) {
		if (!IsLiveSkirmishAIRecoveryObject(object, player) ||
			!object->isContained() ||
			!object->isKindOf(KINDOF_DOZER) ||
			object->isDisabledByType(DISABLED_UNMANNED) ||
			!object->getAIUpdateInterface() ||
			!object->getAIUpdateInterface()->getDozerAIInterface() ||
			!HasSkirmishAICommandForTemplate(object, primaryTemplate))
			continue;
		Object *container = object->getContainedBy();
		ContainModuleInterface *contain = container
			? container->getContain() : nullptr;
		const Bool hasValidRoute =
			IsSkirmishAIRecoveryContainedBuilderRoute(
				true, object->isContained(),
				IsLiveSkirmishAIRecoveryObject(container, nullptr),
				container && container->getControllingPlayer() == player,
				contain != nullptr, contain && contain->isContained(object));
		if (!hasValidRoute)
			continue;
		hasContainedBuilderRoute = true;

		AIUpdateInterface *builderAI = object->getAIUpdateInterface();
		if (!CanSkirmishAIRecoveryUpdateAdvance(object, builderAI))
			continue;
		const StateID builderState = builderAI->getCurrentStateID();
		const Bool builderAlreadyExiting = builderState == AI_EXIT ||
			builderState == AI_EXIT_INSTANTLY ||
			builderState == AI_FOLLOW_EXITPRODUCTION_PATH;
		if (!ShouldOrderSkirmishAIRecoveryBuilderExit(
			hasValidRoute, builderAlreadyExiting))
			continue;
		builderAI->aiExit(container, CMD_FROM_AI);
	}
	return hasContainedBuilderRoute;
}

static void CollectSkirmishAIRecoveryBuilders(
	Player *player, const Coord3D *position,
	const ThingTemplate *primaryTemplate, std::vector<Object *> *builders)
{
	if (!builders)
		return;
	builders->clear();
	if (!player || !TheGameLogic || !primaryTemplate)
		return;

	std::vector<SkirmishAIRecoveryBuilderCandidate> candidates;
	for (Object *object = TheGameLogic->getFirstObject(); object;
		object = object->getNextObject()) {
		if (!IsLiveSkirmishAIRecoveryObject(object, player) ||
			object->isContained() ||
			!object->isKindOf(KINDOF_DOZER) ||
			object->isDisabledByType(DISABLED_UNMANNED) ||
			IsSkirmishAIRecoveryWorkerReservedByNativeHole(player, object) ||
			!object->getAIUpdateInterface() ||
			!HasSkirmishAICommandForTemplate(object, primaryTemplate))
			continue;
		DozerAIInterface *dozerAI =
			object->getAIUpdateInterface()->getDozerAIInterface();
		if (!dozerAI)
			continue;

		SkirmishAIRecoveryBuilderCandidate candidate;
		candidate.object = object;
		candidate.idle = !dozerAI->isAnyTaskPending();
		candidate.distance = 0.0f;
		if (position) {
			Real dx = object->getPosition()->x - position->x;
			Real dy = object->getPosition()->y - position->y;
			candidate.distance = dx * dx + dy * dy;
		}
		candidates.push_back(candidate);
	}

	for (UnsignedInt i = 0; i < candidates.size(); ++i) {
		UnsignedInt best = i;
		for (UnsignedInt j = i + 1; j < candidates.size(); ++j) {
			if (IsSkirmishAIRecoveryBuilderCandidateBetter(
				candidates[j], candidates[best]))
				best = j;
		}
		if (best != i) {
			SkirmishAIRecoveryBuilderCandidate temp = candidates[i];
			candidates[i] = candidates[best];
			candidates[best] = temp;
		}
		builders->push_back(candidates[i].object);
	}
}

struct SkirmishProductionCandidate
{
	TeamPrototype *prototype;
	SkirmishAICostRange costRange;
	Int factoryWaitFrames;
	UnsignedInt sourceOrdinal;
};

struct SkirmishFactoryProjection
{
	Object *factory;
	Int projectedFrames;
	Bool usedByCandidate;
	Bool idle;
};

struct SkirmishEnemyCandidate
{
	Player *player;
	Int playerIndex;
	Int knownAssetValue;
	Bool hasKnownObject;
	Bool hasKnownUnit;
	Bool hasKnownBuildFacility;
	Coord3D knownPosition;
	Bool hasKnownPosition;
	Int distance;
	SkirmishAITargetRouteClass routeClass;
	Int score;
};

static Int getSkirmishProductionEntryFrames(
	const ProductionEntry *entry, Player *player, Bool firstEntry)
{
	if (!entry)
		return 0;
	Int buildFrames = 0;
	// ProductionEntry is a tagged union. Preserve the historical timing path
	// for old recordings, but never treat an upgrade as a unit in new games.
	if (IsCriticalRecoveryModeEnabled(player)) {
		if (entry->getProductionType() == PRODUCTION_UNIT && entry->getProductionObject())
			buildFrames = entry->getProductionObject()->calcTimeToBuild(player);
		else if (entry->getProductionType() == PRODUCTION_UPGRADE && entry->getProductionUpgrade())
			buildFrames = entry->getProductionUpgrade()->calcTimeToBuild(player);
	}
	else if (entry->getProductionObject())
		buildFrames = entry->getProductionObject()->calcTimeToBuild(player);
	else if (entry->getProductionUpgrade())
		buildFrames = entry->getProductionUpgrade()->calcTimeToBuild(player);
	if (buildFrames <= 0)
		return 0;

	return GetSkirmishAIProductionEntryWaitFrames(
		buildFrames,
		entry->getPercentComplete(),
		firstEntry,
		entry->getProductionQuantityRemaining());
}

static Bool appendSkirmishProductionOrder(
	std::vector<SkirmishFactoryProjection> &factories,
	Player *player,
	const ThingTemplate *thing,
	Int unitCount,
	Int *productionCost)
{
	if (unitCount <= 0)
		return true;

	std::vector<Int> projectedLoads;
	std::vector<Int> compatibleFactories;
	for (std::vector<SkirmishFactoryProjection>::iterator factory = factories.begin();
		factory != factories.end(); ++factory) {
		projectedLoads.push_back(factory->projectedFrames);
		compatibleFactories.push_back(
			TheBuildAssistant->isPossibleToMakeUnit(factory->factory, thing) ? 1 : 0);
	}

	Int candidateFrames = thing->calcTimeToBuild(player);
	Int unitsRemaining = unitCount;
	while (unitsRemaining > 0) {
		Int selectedFactory = GetSkirmishAILeastLoadedFactoryIndex(
			projectedLoads.empty() ? nullptr : &projectedLoads[0],
			compatibleFactories.empty() ? nullptr : &compatibleFactories[0],
			(Int)factories.size());
		if (selectedFactory < 0)
			return false;
		Int productionQuantity = ProductionUpdate::getProductionQuantityForUnitFromObject(
			factories[selectedFactory].factory, thing);
		*productionCost = AddSkirmishAICostValue(
			*productionCost, thing->calcCostToBuild(player), 1);
		factories[selectedFactory].projectedFrames = AddSkirmishAIFrameValue(
			factories[selectedFactory].projectedFrames, candidateFrames);
		factories[selectedFactory].usedByCandidate = true;
		projectedLoads[selectedFactory] = factories[selectedFactory].projectedFrames;
		unitsRemaining = GetSkirmishAIUnitsRemainingAfterProductionEntry(
			unitsRemaining, productionQuantity);
	}
	return true;
}



///////////////////////////////////////////////////////////////////////////////////////////////////
// PRIVATE DATA ///////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////////////

AISkirmishPlayer::AISkirmishPlayer( Player *p ) :	AIPlayer(p),
m_curFlankBaseDefense(0),
m_curFrontBaseDefense(0),
m_curFrontLeftDefenseAngle(0),
m_curFrontRightDefenseAngle(0),
m_curLeftFlankLeftDefenseAngle(0),
m_curLeftFlankRightDefenseAngle(0),
m_curRightFlankLeftDefenseAngle(0),
m_curRightFlankRightDefenseAngle(0),
	m_frameToCheckEnemy(0),
	m_currentEnemy(nullptr),
	m_currentEnemyPlayerIndex(-1),
	m_strategyProductionReserveCost(0),
	m_strategySuperweaponID(INVALID_ID),
	m_strategyAuthorizedThing(nullptr),
	m_strategySpendAuthorization(SKIRMISH_AI_SPEND_AUTHORIZATION_NONE),
	m_strategyProductionReserveRefreshing(false),
	m_strategyProductionReserveLoaded(false),
	m_strategySourceCommandLocked(false),
	m_strategyLockedSourceID(INVALID_ID),
	m_strategyLockedPowerID(0),
	m_reinforcementRoundRobinCursor(0),
	m_recoveryEverCompleted(false),
	m_recoveryImpossible(false),
	m_recoveryConstructionID(INVALID_ID),
	m_recoveryPlacementAttempt(0),
	m_recoveryNextAttemptFrame(0),
	m_recoveryEvacuationDeadline(0),
	m_recoveryReserveCost(0),
	m_recoveryBuilderFactoryID(INVALID_ID),
	m_recoveryBuilderProductionID(PRODUCTIONID_INVALID),
	m_recoveryBuilderCancellationOwned(false),
	m_recoveryBuilderFailoverConsumed(false),
	m_recoveryAuthorizedThing(nullptr)

{
	m_frameLastBuildingBuilt = TheGameLogic->getFrame();
	m_recoveryLocation.zero();
	m_recoveryAngle = 0.0f;
	InitializeSkirmishStrategyState(
		&m_strategyState, TheGameLogic ? TheGameLogic->getFrame() : 0);
	p->setCanBuildUnits(true); // turn on ai production by default.
}

AISkirmishPlayer::~AISkirmishPlayer()
{
	clearTeamsInQueue();
}

Bool AISkirmishPlayer::usesCriticalRecoveryBehavior() const
{
	return IsCriticalRecoveryModeEnabled(m_player);
}

Bool AISkirmishPlayer::usesProductionBehavior() const
{
	if (!m_player || m_player->getPlayerType() != PLAYER_COMPUTER ||
		!TheGameLogic)
		return false;
	const Bool replay = TheGameLogic->isInReplayGame();
	const Int gameMode = replay
		? (TheRecorder ? TheRecorder->getGameMode() : GAME_NONE)
		: TheGameLogic->getGameMode();
	if (!IsSkirmishAIRecoveryGameMode(gameMode))
		return false;
	return ShouldUseCurrentSkirmishAIProductionBehavior();
}

void AISkirmishPlayer::clearStrategySourceCommandLock()
{
	m_strategySourceCommandLocked = false;
	m_strategyLockedSourceID = INVALID_ID;
	m_strategyLockedPowerID = 0;
}

Bool AISkirmishPlayer::isStrategySourceCommandLockValid() const
{
	if (!m_strategySourceCommandLocked || !TheGameLogic ||
		m_strategyState.currentMode != SKIRMISH_STRATEGY_FORTIFY ||
		m_strategyLockedSourceID == INVALID_ID ||
		m_strategyLockedPowerID == 0 || !TheSpecialPowerStore)
		return false;
	Object *source = TheGameLogic->findObjectByID(m_strategyLockedSourceID);
	const SpecialPowerTemplate *power =
		TheSpecialPowerStore->findSpecialPowerTemplateByID(
			m_strategyLockedPowerID);
	SpecialPowerModuleInterface *module = source && power
		? source->getSpecialPowerModule(power)
		: nullptr;
	return power && IsSkirmishAIStrategicLaunchPower(
		power->getSpecialPowerType()) &&
		m_strategySuperweaponID == m_strategyLockedSourceID &&
		source && source->getControllingPlayer() == m_player &&
		source->isKindOf(KINDOF_FS_SUPERWEAPON) &&
		!source->isKindOf(KINDOF_REBUILD_HOLE) &&
		!source->isEffectivelyDead() && !source->isDestroyed() &&
		!source->testStatus(OBJECT_STATUS_SOLD) &&
		!source->testStatus(OBJECT_STATUS_UNDER_CONSTRUCTION) &&
		!source->testStatus(OBJECT_STATUS_RECONSTRUCTING) && module &&
		module->isDispatchable();
}

Bool AISkirmishPlayer::hasUsableSupplySource(
	const Coord3D *position, Real centerRadius) const
{
	if (!position || !m_player || !ThePartitionManager)
		return false;
	static const NameKeyType keyWarehouseUpdate =
		NAMEKEY("SupplyWarehouseDockUpdate");
	const Real supplyCenterCloseDistance = 20 * PATHFIND_CELL_SIZE_F;
	const Real baseRadius = supplyCenterCloseDistance + centerRadius;
	PartitionFilterAcceptByKindOf filterSupplySource(
		MAKE_KINDOF_MASK(KINDOF_SUPPLY_SOURCE), KINDOFMASK_NONE);
	PartitionFilterPlayerAffiliation filterAffiliation(
		m_player, ALLOW_ALLIES | ALLOW_NEUTRAL, true);
	PartitionFilterAlive filterAlive;
	PartitionFilterOnMap filterMapStatus;
	PartitionFilter *filters[] = {
		&filterSupplySource, &filterAffiliation, &filterAlive,
		&filterMapStatus, nullptr
	};
	ObjectIterator *iter = ThePartitionManager->iterateObjectsInRange(
		position, baseRadius, FROM_BOUNDINGSPHERE_2D, filters);
	MemoryPoolObjectHolder hold(iter);
	for (Object *source = iter->first(); source; source = iter->next()) {
		SupplyWarehouseDockUpdate *warehouse =
			(SupplyWarehouseDockUpdate *)source->findUpdateModule(
				keyWarehouseUpdate);
		if (warehouse && warehouse->getBoxesStored() > 0)
			return true;
	}
	return false;
}

Bool AISkirmishPlayer::hasOwnedSupplyCenter(
	const ThingTemplate *supplyPlan) const
{
	if (!supplyPlan || !m_player || !TheGameLogic)
		return false;
	for (Object *object = TheGameLogic->getFirstObject(); object;
		object = object->getNextObject()) {
		if (object->getControllingPlayer() == m_player &&
			object->isKindOf(KINDOF_FS_SUPPLY_CENTER) &&
			object->getTemplate() &&
			object->getTemplate()->isEquivalentTo(supplyPlan) &&
			!object->isKindOf(KINDOF_REBUILD_HOLE) &&
			!object->isEffectivelyDead() && !object->isDestroyed() &&
			!object->testStatus(OBJECT_STATUS_SOLD) &&
			!object->testStatus(OBJECT_STATUS_UNDER_CONSTRUCTION) &&
			!object->testStatus(OBJECT_STATUS_RECONSTRUCTING))
			return true;
	}
	return false;
}

Bool AISkirmishPlayer::hasQueuedSupplyCenter(
	const ThingTemplate *supplyPlan) const
{
	if (!supplyPlan || !m_player || !TheGameLogic)
		return false;
	for (Object *object = TheGameLogic->getFirstObject(); object;
		object = object->getNextObject()) {
		if (object->getControllingPlayer() == m_player &&
			object->isKindOf(KINDOF_FS_SUPPLY_CENTER) &&
			object->getTemplate() &&
			object->getTemplate()->isEquivalentTo(supplyPlan) &&
			!object->isKindOf(KINDOF_REBUILD_HOLE) &&
			!object->isEffectivelyDead() && !object->isDestroyed() &&
			!object->testStatus(OBJECT_STATUS_SOLD) &&
			(object->testStatus(OBJECT_STATUS_UNDER_CONSTRUCTION) ||
			 object->testStatus(OBJECT_STATUS_RECONSTRUCTING)))
			return true;
	}
	return false;
}

Bool AISkirmishPlayer::hasUsableSupplyCenterForCollectors() const
{
	if (!m_player || !TheGameLogic)
		return false;
	for (Object *center = TheGameLogic->getFirstObject(); center;
		center = center->getNextObject()) {
		if (center->getControllingPlayer() != m_player ||
			!center->isKindOf(KINDOF_FS_SUPPLY_CENTER) ||
			center->isKindOf(KINDOF_REBUILD_HOLE) ||
			center->isEffectivelyDead() || center->isDestroyed() ||
			center->testStatus(OBJECT_STATUS_SOLD) ||
			center->testStatus(OBJECT_STATUS_UNDER_CONSTRUCTION) ||
			center->testStatus(OBJECT_STATUS_RECONSTRUCTING))
			continue;
		if (hasUsableSupplySource(
				center->getPosition(),
				center->getGeometryInfo().getBoundingCircleRadius()))
			return true;
	}
	return false;
}

void AISkirmishPlayer::cancelDepletedCollectorProduction()
{
	if (!usesProductionBehavior() || !TheGameLogic)
		return;
	const Int collectorDemand = getStage3SupplyCollectorDemand();
	const Bool hasUsableCollectorEconomy =
		hasUsableSupplyCenterForCollectors();
	Int queuedCollectorQuantity = 0;
	for (Object *queuedFactory = TheGameLogic->getFirstObject(); queuedFactory;
		queuedFactory = queuedFactory->getNextObject()) {
		if (queuedFactory->getControllingPlayer() != m_player ||
			queuedFactory->isEffectivelyDead() || queuedFactory->isDestroyed() ||
			queuedFactory->testStatus(OBJECT_STATUS_SOLD))
			continue;
		ProductionUpdateInterface *queuedProduction =
			queuedFactory->getProductionUpdateInterface();
		for (const ProductionEntry *queuedEntry = queuedProduction
				? queuedProduction->firstProduction() : nullptr;
			 queuedEntry;
			 queuedEntry = queuedProduction->nextProduction(queuedEntry)) {
			if (!isSkirmishAIPendingCollectorEntry(
					queuedFactory, queuedEntry))
				continue;
			const Int queuedRemaining =
				queuedEntry->getProductionQuantityRemaining();
			if (queuedRemaining > 0) {
				queuedCollectorQuantity = AddSkirmishAISupplyCollectorDeficit(
					queuedCollectorQuantity, queuedRemaining, 0);
			}
		}
	}
	std::vector<WorkOrder *> processed;
	for (DLINK_ITERATOR<TeamInQueue> teamIt = iterate_TeamBuildQueue();
		!teamIt.done(); teamIt.advance()) {
		TeamInQueue *team = teamIt.cur();
		// Reinforcement orders belong to a team's deficit, not economy staffing.
		for (WorkOrder *leader = team && !team->m_reinforcement
				? team->m_workOrders : nullptr;
			leader; leader = leader->m_next) {
			if (!hasUsableCollectorEconomy && team->m_priorityBuild &&
				team->m_team == m_player->getDefaultTeam() &&
				leader->m_isResourceGatherer && leader->m_thing &&
				leader->m_thing->isKindOf(KINDOF_HARVESTER) &&
				leader->m_factoryID == INVALID_ID &&
				leader->m_productionID == 0 &&
				leader->m_numCompleted == 0 &&
				leader->m_numRequired > 0) {
				leader->m_numRequired = 0;
				leader->m_required = false;
				continue;
			}
			if (!leader->m_isResourceGatherer || !leader->m_thing ||
				leader->m_factoryID == INVALID_ID ||
				leader->m_numCompleted >= leader->m_numRequired)
				continue;
			Bool alreadyProcessed = false;
			for (std::vector<WorkOrder *>::const_iterator seen = processed.begin();
				seen != processed.end(); ++seen) {
				if (*seen == leader) {
					alreadyProcessed = true;
					break;
				}
			}
			if (alreadyProcessed)
				continue;

			Object *factory = TheGameLogic->findObjectByID(leader->m_factoryID);
			ProductionUpdateInterface *production = factory &&
				factory->getControllingPlayer() == m_player
					? factory->getProductionUpdateInterface() : nullptr;
			if (!production)
				continue;

			if (leader->m_productionID != 0) {
				const ProductionEntry *exactEntry = nullptr;
				for (const ProductionEntry *entry = production->firstProduction(); entry;
					entry = production->nextProduction(entry)) {
					if (static_cast<Int>(entry->getProductionID()) ==
							leader->m_productionID) {
						exactEntry = entry;
						break;
					}
				}
				if (!exactEntry || exactEntry->getProductionType() != PRODUCTION_UNIT ||
					!exactEntry->getProductionObject() ||
					!exactEntry->getProductionObject()->isEquivalentTo(leader->m_thing) ||
					exactEntry->getProductionQuantityRemaining() <= 0)
					continue;
				const Int matchingEntryQuantity =
					exactEntry->getProductionQuantityRemaining();
				const Bool depletedSupplyCenter =
					factory->isKindOf(KINDOF_FS_SUPPLY_CENTER) &&
					!hasUsableSupplySource(factory->getPosition(),
						factory->getGeometryInfo().getBoundingCircleRadius());
				const Bool retainEntry = exactEntry->getPercentComplete() > 0.0f ||
					(!depletedSupplyCenter &&
					 queuedCollectorQuantity - matchingEntryQuantity < collectorDemand);
				processed.push_back(leader);
				if (retainEntry)
					continue;
				production->cancelUnitCreate(exactEntry->getProductionID());
				queuedCollectorQuantity =
					matchingEntryQuantity >= queuedCollectorQuantity
						? 0 : queuedCollectorQuantity - matchingEntryQuantity;
				leader->m_numRequired = leader->m_numCompleted;
				leader->m_factoryID = INVALID_ID;
				leader->m_productionID = 0;
				leader->m_required = false;
				continue;
			}

			std::vector<WorkOrder *> matchingOrders;
			Bool hasNonCollectorOrder = false;
			for (DLINK_ITERATOR<TeamInQueue> matchTeamIt = iterate_TeamBuildQueue();
				!matchTeamIt.done(); matchTeamIt.advance()) {
				TeamInQueue *matchTeam = matchTeamIt.cur();
				for (WorkOrder *order = matchTeam && !matchTeam->m_reinforcement
						? matchTeam->m_workOrders : nullptr;
					order; order = order->m_next) {
					if (!order->m_thing || order->m_factoryID != leader->m_factoryID ||
						!order->m_thing->isEquivalentTo(leader->m_thing) ||
						order->m_numCompleted >= order->m_numRequired)
						continue;
					if (order->m_isResourceGatherer) {
						matchingOrders.push_back(order);
						processed.push_back(order);
					} else {
						hasNonCollectorOrder = true;
					}
				}
			}
			const ProductionEntry *matchingEntry = nullptr;
			Int matchingEntryCount = 0;
			Int matchingEntryQuantity = 0;
			Int outstandingOrderQuantity = 0;
			for (UnsignedInt orderIndex = 0;
				orderIndex < matchingOrders.size(); ++orderIndex) {
				outstandingOrderQuantity +=
					matchingOrders[orderIndex]->m_numRequired -
					matchingOrders[orderIndex]->m_numCompleted;
			}
			const Bool depletedSupplyCenter =
				factory->isKindOf(KINDOF_FS_SUPPLY_CENTER) &&
				!hasUsableSupplySource(factory->getPosition(),
					factory->getGeometryInfo().getBoundingCircleRadius());
			for (const ProductionEntry *entry = production->firstProduction(); entry;
				entry = production->nextProduction(entry)) {
				const Bool isMatchingEntry = entry->getProductionType() == PRODUCTION_UNIT &&
					entry->getProductionObject() &&
					entry->getProductionObject()->isEquivalentTo(leader->m_thing);
				if (!isMatchingEntry)
					continue;
				const Int remaining = entry->getProductionQuantityRemaining();
				if (remaining <= 0)
					continue;
				matchingEntry = entry;
				++matchingEntryCount;
				matchingEntryQuantity += remaining;
			}
			const Bool ambiguousMapping = hasNonCollectorOrder ||
				matchingOrders.size() != 1 || matchingEntryCount != 1 ||
				outstandingOrderQuantity != 1 || matchingEntryQuantity != 1;
			if (!IsSkirmishAIDepletedCollectorQueueMappingUnambiguous(
					outstandingOrderQuantity, matchingEntryQuantity,
					ambiguousMapping))
				continue;

			// WorkOrder has no ProductionID. Only a one-unit, single-order/single-entry
			// case proves ownership strongly enough to mutate. Multi-unit batches and
			// indistinguishable entries retain their existing callback bindings.
			const Bool inProgress = matchingEntry->getPercentComplete() > 0.0f;
			const Bool retainEntry = inProgress ||
				(!depletedSupplyCenter &&
				 queuedCollectorQuantity - matchingEntryQuantity < collectorDemand);
			if (retainEntry)
				continue;

			production->cancelUnitCreate(matchingEntry->getProductionID());
			queuedCollectorQuantity =
				matchingEntryQuantity >= queuedCollectorQuantity
					? 0 : queuedCollectorQuantity - matchingEntryQuantity;
			WorkOrder *order = matchingOrders[0];
			order->m_numRequired = order->m_numCompleted;
			order->m_factoryID = INVALID_ID;
			order->m_productionID = 0;
			order->m_required = false;
		}
	}
}

Bool AISkirmishPlayer::isSupplyCenterPrerequisiteNeeded(
	const ThingTemplate *supplyPlan) const
{
	if (!supplyPlan || !m_player)
		return false;
	for (BuildListInfo *build = m_player->getBuildList(); build;
		build = build->getNext()) {
		const ThingTemplate *candidate =
			TheThingFactory->findTemplate(build->getTemplateName());
		if (!candidate || candidate->isEquivalentTo(supplyPlan) ||
			!build->isBuildable())
			continue;
		Object *existing = TheGameLogic
			? TheGameLogic->findObjectByID(build->getObjectID()) : nullptr;
		if (DoesSkirmishAIDependentStructureSatisfyPrerequisiteDemand(
			existing != nullptr,
			existing && existing->getControllingPlayer() == m_player,
			existing && existing->isKindOf(KINDOF_REBUILD_HOLE),
			existing && existing->isEffectivelyDead(),
			existing && existing->isDestroyed(),
			existing && existing->testStatus(OBJECT_STATUS_SOLD),
			existing && existing->testStatus(OBJECT_STATUS_UNDER_CONSTRUCTION),
			existing && existing->testStatus(OBJECT_STATUS_RECONSTRUCTING)))
			continue;
		for (Int prereqIndex = 0;
			prereqIndex < candidate->getPrereqCount(); ++prereqIndex) {
			const ProductionPrerequisite *prereq =
				candidate->getNthPrereq(prereqIndex);
			if (!ShouldInspectSkirmishAIPrerequisiteAlternative(
					prereq != nullptr,
					prereq && prereq->isSatisfied(m_player)))
				continue;
			const ThingTemplate *facilities[32];
			const Int facilityCount = prereq
				? prereq->getAllPossibleBuildFacilityTemplates(facilities, 32) : 0;
			for (Int facilityIndex = 0; facilityIndex < facilityCount;
				++facilityIndex) {
				if (facilities[facilityIndex] &&
					facilities[facilityIndex]->isEquivalentTo(supplyPlan))
					return true;
			}
		}
	}
	return false;
}

void AISkirmishPlayer::refreshStrategyProductionState()
{
	if (!usesProductionBehavior()) {
		m_strategyProductionReserveCost = 0;
		m_strategySuperweaponID = INVALID_ID;
		m_strategyAuthorizedThing = nullptr;
		m_strategySpendAuthorization = SKIRMISH_AI_SPEND_AUTHORIZATION_NONE;
		clearStrategySourceCommandLock();
		m_reinforcementRoundRobinCursor = 0;
		return;
	}
	cancelDepletedCollectorProduction();

	if (m_strategyState.currentMode == SKIRMISH_STRATEGY_FORTIFY) {
		if (m_strategyState.superweaponAttemptStatus ==
			SKIRMISH_STRATEGY_ATTEMPT_NOT_STARTED) {
			Object *selected = FindSkirmishAIStrategicSource(m_player, nullptr);
			if (!selected)
				selected = FindSkirmishAISuperweaponConstruction(m_player);
			if (selected) {
				m_strategySuperweaponID = selected->getID();
				m_strategyState.superweaponAttemptStatus =
					SKIRMISH_STRATEGY_ATTEMPT_PENDING;
			}
		}
		if (m_strategyState.superweaponAttemptStatus ==
			SKIRMISH_STRATEGY_ATTEMPT_PENDING) {
			if (ShouldFailSkirmishAIStrategicPowerSourceLock(
					m_strategySourceCommandLocked, true,
					isStrategySourceCommandLockValid())) {
				m_strategyState.superweaponAttemptStatus =
					SKIRMISH_STRATEGY_ATTEMPT_FAILED;
				m_strategySuperweaponID = INVALID_ID;
				clearStrategySourceCommandLock();
			}
		}
		if (m_strategyState.superweaponAttemptStatus ==
			SKIRMISH_STRATEGY_ATTEMPT_PENDING) {
			Object *tracked = m_strategySuperweaponID != INVALID_ID
				? TheGameLogic->findObjectByID(m_strategySuperweaponID) : nullptr;
			const Bool trackedConstructionPending =
				ShouldKeepSkirmishAISuperweaponConstructionPending(
					tracked != nullptr,
					tracked && tracked->getControllingPlayer() == m_player,
					tracked && tracked->isKindOf(KINDOF_FS_SUPERWEAPON),
					tracked && tracked->isKindOf(KINDOF_REBUILD_HOLE),
					tracked && tracked->isEffectivelyDead(),
					tracked && tracked->isDestroyed(),
					tracked && tracked->testStatus(OBJECT_STATUS_SOLD),
					tracked && tracked->testStatus(OBJECT_STATUS_UNDER_CONSTRUCTION),
					tracked && tracked->testStatus(OBJECT_STATUS_RECONSTRUCTING));
			if (!trackedConstructionPending &&
				(!IsUsableSkirmishAIStrategicSource(tracked, m_player) ||
				!HasSkirmishAIStrategicPowerModule(tracked, nullptr))) {
				Object *replacement =
					FindSkirmishAIStrategicSource(m_player, nullptr);
				if (!replacement)
					replacement = FindSkirmishAISuperweaponConstruction(m_player);
				if (replacement) {
					m_strategySuperweaponID = replacement->getID();
				} else {
					m_strategyState.superweaponAttemptStatus =
						SKIRMISH_STRATEGY_ATTEMPT_FAILED;
					m_strategySuperweaponID = INVALID_ID;
				}
			}
		}
		if (m_strategyState.superweaponAttemptStatus !=
			SKIRMISH_STRATEGY_ATTEMPT_PENDING)
			clearStrategySourceCommandLock();
	} else {
		m_strategySuperweaponID = INVALID_ID;
		clearStrategySourceCommandLock();
	}

	refreshStrategyProductionReserve();
}

void AISkirmishPlayer::refreshStrategyProductionReserve()
{
	if (!usesProductionBehavior() || m_strategyProductionReserveRefreshing) {
		if (!usesProductionBehavior())
			m_strategyProductionReserveCost = 0;
		return;
	}
	m_strategyProductionReserveRefreshing = true;
	if (!TheAI || !TheAI->getAiData()) {
		m_strategyProductionReserveCost = 0;
		m_strategyProductionReserveRefreshing = false;
		return;
	}
	const Int rebuildReserve = getCriticalRebuildReserve(nullptr);
	m_strategyProductionReserveCost = GetFreshSkirmishAIProductionReserve(
		TheAI->getAiData()->m_resourcesPoor, rebuildReserve,
		getActiveRecoveryReserveCost());
	m_strategyProductionReserveRefreshing = false;
}

Bool AISkirmishPlayer::queueAuthorizedStrategyBuilder(
	const ThingTemplate *structure)
{
	if (!usesProductionBehavior() || !structure || !m_player || !TheThingFactory)
		return false;
	for (DLINK_ITERATOR<TeamInQueue> queue = iterate_TeamBuildQueue();
		!queue.done(); queue.advance()) {
		TeamInQueue *team = queue.cur();
		for (WorkOrder *order = team ? team->m_workOrders : nullptr;
			order; order = order->m_next) {
			if (order->m_thing && !order->m_isResourceGatherer &&
				order->m_thing->isKindOf(KINDOF_DOZER) &&
				order->m_numCompleted < order->m_numRequired &&
				HasSkirmishAICommandSetForTemplate(
					order->m_thing->friend_getCommandSetString(), structure)) {
				if (order->m_factoryID != INVALID_ID)
					return true;
				const Bool canBuildUnits = m_player->getCanBuildUnits();
				m_player->setCanBuildUnits(true);
				const ThingTemplate *previousThing = m_strategyAuthorizedThing;
				const SkirmishAISpendAuthorization previousClass =
					m_strategySpendAuthorization;
				m_strategyAuthorizedThing = order->m_thing;
				m_strategySpendAuthorization =
					SKIRMISH_AI_SPEND_AUTHORIZATION_BUILDER;
				const Bool queued = startTraining(
					order, true, team->m_team->getName());
				m_strategyAuthorizedThing = previousThing;
				m_strategySpendAuthorization = previousClass;
				m_player->setCanBuildUnits(canBuildUnits);
				if (queued)
					m_teamDelay = 0;
				return queued;
			}
		}
	}

	Bool canBuildUnits = m_player->getCanBuildUnits();
	m_player->setCanBuildUnits(true);
	Bool queued = false;
	for (const ThingTemplate *builder = TheThingFactory->firstTemplate(); builder;
		builder = builder->friend_getNextTemplate()) {
		if (!builder->isKindOf(KINDOF_DOZER) ||
			!HasSkirmishAICommandSetForTemplate(
				builder->friend_getCommandSetString(), structure))
			continue;
		std::vector<Object *> producers;
		FindSkirmishAICompatibleProducers(m_player, builder, &producers);
		if (producers.empty())
			continue;
		TeamInQueue *team = newInstance(TeamInQueue);
		WorkOrder *order = newInstance(WorkOrder);
		order->m_thing = builder;
		order->m_factoryID = INVALID_ID;
		order->m_numRequired = 1;
		order->m_required = true;
		order->m_isResourceGatherer = false;
		order->m_next = nullptr;
		team->m_priorityBuild = true;
		team->m_workOrders = order;
		team->m_frameStarted = TheGameLogic->getFrame();
		team->m_team = m_player->getDefaultTeam();
		prependTo_TeamBuildQueue(team);
		const ThingTemplate *previousThing = m_strategyAuthorizedThing;
		const SkirmishAISpendAuthorization previousClass =
			m_strategySpendAuthorization;
		m_strategyAuthorizedThing = builder;
		m_strategySpendAuthorization = SKIRMISH_AI_SPEND_AUTHORIZATION_BUILDER;
		queued = startTraining(order, true, team->m_team->getName());
		m_strategyAuthorizedThing = previousThing;
		m_strategySpendAuthorization = previousClass;
		if (!queued) {
			removeFrom_TeamBuildQueue(team);
			deleteInstance(team);
			continue;
		}
		m_teamDelay = 0;
		break;
	}
	m_player->setCanBuildUnits(canBuildUnits);
	return queued;
}

void AISkirmishPlayer::notifySpecialPowerFired(
	Object *source, const SpecialPowerTemplate *power)
{
	if (!usesProductionBehavior() || !source || !power ||
		m_strategyState.currentMode != SKIRMISH_STRATEGY_FORTIFY ||
		m_strategyState.superweaponAttemptStatus !=
			SKIRMISH_STRATEGY_ATTEMPT_PENDING ||
		source->getControllingPlayer() != m_player ||
		!source->isKindOf(KINDOF_FS_SUPERWEAPON) ||
		source->getID() != m_strategyLockedSourceID ||
		power->getID() != m_strategyLockedPowerID ||
		!IsSkirmishAIStrategicLaunchPower(power->getSpecialPowerType()))
		return;
	m_strategyState.superweaponAttemptStatus =
		SKIRMISH_STRATEGY_ATTEMPT_SUCCEEDED;
	clearStrategySourceCommandLock();
}

Bool AISkirmishPlayer::shouldUseSkirmishSpecialPowerSource(
	Object *source, const SpecialPowerTemplate *power)
{
	if (!usesProductionBehavior() || !source || !power ||
		m_strategyState.currentMode != SKIRMISH_STRATEGY_FORTIFY ||
		!IsSkirmishAIStrategicLaunchPower(power->getSpecialPowerType()))
		return true;
	if (m_strategyState.superweaponAttemptStatus ==
		SKIRMISH_STRATEGY_ATTEMPT_NOT_STARTED) {
		Object *attemptSource = FindSkirmishAIStrategicSource(m_player, nullptr);
		if (!attemptSource)
			return false;
		m_strategyState.superweaponAttemptStatus =
			SKIRMISH_STRATEGY_ATTEMPT_PENDING;
		m_strategySuperweaponID = attemptSource->getID();
	}
	if (m_strategyState.superweaponAttemptStatus !=
		SKIRMISH_STRATEGY_ATTEMPT_PENDING)
		return false;
	const Bool lockedCommandValid = isStrategySourceCommandLockValid();
	if (ShouldRejectSkirmishAIStrategicPowerRequest(
			m_strategySourceCommandLocked, lockedCommandValid))
		return false;
	if (ShouldFailSkirmishAIStrategicPowerSourceLock(
			m_strategySourceCommandLocked, true, lockedCommandValid)) {
		m_strategyState.superweaponAttemptStatus =
			SKIRMISH_STRATEGY_ATTEMPT_FAILED;
		m_strategySuperweaponID = INVALID_ID;
		clearStrategySourceCommandLock();
		return false;
	}
	Object *selected = IsUsableSkirmishAIStrategicSource(source, m_player) &&
		HasSkirmishAIStrategicPowerModule(source, power)
		? source : nullptr;
	SpecialPowerModuleInterface *selectedModule = selected
		? selected->getSpecialPowerModule(power) : nullptr;
	const Bool exactSource = selectedModule && selectedModule->isReady() &&
		selectedModule->isDispatchable();
	if (!ShouldLockSkirmishAIStrategicPowerSource(true,
			m_strategySourceCommandLocked, exactSource))
		return false;
	m_strategySuperweaponID = selected->getID();
	m_strategySourceCommandLocked = true;
	m_strategyLockedSourceID = selected->getID();
	m_strategyLockedPowerID = power->getID();
	return true;
}

void AISkirmishPlayer::resolveSpecialPowerDispatchAttempt(
	Object *source, const SpecialPowerTemplate *power, Bool accepted)
{
	const Bool exactAttempt = source && power &&
		source->getID() == m_strategyLockedSourceID &&
		power->getID() == m_strategyLockedPowerID;
	if (m_strategySourceCommandLocked && exactAttempt &&
		!ShouldRetainSkirmishAIStrategicPowerDispatchLock(
			m_strategySourceCommandLocked, accepted))
		clearStrategySourceCommandLock();
}

Bool AISkirmishPlayer::canSpendForCriticalRecovery(
	Int cost, const ThingTemplate *thing, Bool isUpgrade,
	Bool refreshProductionReserve)
{
	if (cost <= 0)
		return true;
	const Bool recoveryBehavior = usesCriticalRecoveryBehavior();
	const Bool productionBehavior = usesProductionBehavior();
	if (!recoveryBehavior && !productionBehavior)
		return true;
	if (!m_player || !m_player->getMoney())
		return false;
	const Int money = m_player->getMoney()->countMoney();
	if (cost > money)
		return false;
	const Bool recoveryAuthorized = !isUpgrade && thing &&
		m_recoveryAuthorizedThing &&
		thing->isEquivalentTo(m_recoveryAuthorizedThing);
	if (recoveryAuthorized)
		return true;
	const Bool strategyAuthorized = !isUpgrade && thing &&
		m_strategyAuthorizedThing &&
		thing->isEquivalentTo(m_strategyAuthorizedThing);
	if (productionBehavior && refreshProductionReserve)
		refreshStrategyProductionReserve();
	Int reserve = productionBehavior ? m_strategyProductionReserveCost : 0;
	const Int activeRecoveryReserve = getActiveRecoveryReserveCost();
	if (strategyAuthorized)
		return CanSkirmishAISpendWithAuthorization(
			money, cost, reserve, activeRecoveryReserve,
			m_strategySpendAuthorization);
	if (!recoveryBehavior) {
		return cost <= money - reserve;
	}
	const Bool boundedGraceExpired = TheGameLogic &&
		IsSkirmishAIRecoveryBoundedGraceExpired(
			TheGameLogic->getFrame(), m_recoveryEvacuationDeadline);
	if (m_recoveryEverCompleted && m_recoveryConstructionID != INVALID_ID &&
		TheGameLogic) {
		Object *tracked = TheGameLogic->findObjectByID(m_recoveryConstructionID);
		if (!IsLiveSkirmishAIRecoveryObject(tracked, m_player) &&
			!boundedGraceExpired)
			return false;
	}
	if (activeRecoveryReserve <= 0)
		return cost <= money - reserve;
	if (activeRecoveryReserve > reserve)
		reserve = activeRecoveryReserve;
	return cost <= money - reserve;
}

Bool AISkirmishPlayer::findPrimaryCommandCenter(
	const ThingTemplate *primaryTemplate, Object **center) const
{
	if (center)
		*center = nullptr;
	if (!primaryTemplate || !TheGameLogic || !m_player)
		return false;

	Object *best = nullptr;
	for (Object *object = TheGameLogic->getFirstObject(); object;
		object = object->getNextObject()) {
		if (!IsLiveSkirmishAIRecoveryObject(object, m_player) ||
			!object->isKindOf(KINDOF_COMMANDCENTER) || !object->getTemplate() ||
			!object->getTemplate()->isEquivalentTo(primaryTemplate))
			continue;
		const Bool candidateCompleted =
			!object->testStatus(OBJECT_STATUS_UNDER_CONSTRUCTION);
		const Bool selectedCompleted = best &&
			!best->testStatus(OBJECT_STATUS_UNDER_CONSTRUCTION);
		if (ShouldSelectSkirmishAIPrimaryCommandCenter(
			best != nullptr, selectedCompleted,
			best ? best->getID() : INVALID_ID,
			candidateCompleted, object->getID()))
			best = object;
	}
	if (center)
		*center = best;
	return best != nullptr;
}

BuildListInfo *AISkirmishPlayer::findPrimaryCommandCenterBuildInfo(
	const ThingTemplate *primaryTemplate) const
{
	if (!primaryTemplate || !m_player)
		return nullptr;
	for (BuildListInfo *info = m_player->getBuildList(); info; info = info->getNext()) {
		const ThingTemplate *plan = TheThingFactory->findTemplate(info->getTemplateName());
		if (plan && plan->isEquivalentTo(primaryTemplate))
			return info;
	}
	return nullptr;
}

Bool AISkirmishPlayer::findRecoveryBuilderTemplateAndFactory(
	const ThingTemplate *primaryTemplate,
	const ThingTemplate **builderTemplate, Object **factory,
	Bool *hasPotentialFactory, Bool *hasBoundedFactory)
{
	if (builderTemplate)
		*builderTemplate = nullptr;
	if (factory)
		*factory = nullptr;
	if (hasPotentialFactory)
		*hasPotentialFactory = false;
	if (hasBoundedFactory)
		*hasBoundedFactory = false;
	if (!primaryTemplate || !m_player || !TheGameLogic || !TheThingFactory)
		return false;

	// Inspect each owned factory's command set once.  This deliberately does
	// not call isPossibleToMakeUnit(): temporary prerequisites, max-count,
	// disabled, and cash failures remain retryable recovery states.
	Object *bestFactory = nullptr;
	const ThingTemplate *bestTemplate = nullptr;
	Int bestCommand = 0;
	Object *fallbackFactory = nullptr;
	const ThingTemplate *fallbackTemplate = nullptr;
	Int fallbackCommand = 0;
	for (Object *object = TheGameLogic->getFirstObject(); object;
		object = object->getNextObject()) {
		if (!IsLiveSkirmishAIRecoveryObject(object, m_player) ||
			!object->getProductionUpdateInterface())
			continue;
		const CommandSet *commandSet = TheControlBar
			? TheControlBar->findCommandSet(object->getCommandSetString()) : nullptr;
		if (!commandSet)
			continue;
		for (Int command = 0; command < MAX_COMMANDS_PER_SET; ++command) {
			const CommandButton *button = commandSet->getCommandButton(command);
			const ThingTemplate *product = button ? button->getThingTemplate() : nullptr;
			if (!button ||
				(button->getCommandType() != GUI_COMMAND_UNIT_BUILD &&
				 button->getCommandType() != GUI_COMMAND_DOZER_CONSTRUCT) ||
				!product || !product->isKindOf(KINDOF_DOZER) ||
				!HasSkirmishAICommandSetForTemplate(
					product->friend_getCommandSetString(), primaryTemplate))
				continue;
			if (object->testStatus(OBJECT_STATUS_UNDER_CONSTRUCTION)) {
				// A factory scaffold with no assigned live finisher is an
				// abandoned object, not a future recovery route.  Completed
				// factories remain potential routes even when temporarily
				// disabled or blocked by prerequisites/capacity.
				Bool blockedFinisher = false;
				if (!HasSkirmishAIRecoveryFactoryFinisher(
						object, m_player, &blockedFinisher)) {
					if (blockedFinisher && hasBoundedFactory)
						*hasBoundedFactory = true;
					if (blockedFinisher &&
						(!fallbackFactory || object->getID() < fallbackFactory->getID() ||
						 (object->getID() == fallbackFactory->getID() &&
						  command < fallbackCommand))) {
						fallbackFactory = object;
						fallbackTemplate = product;
						fallbackCommand = command;
					}
					continue;
				}
				if (hasPotentialFactory)
					*hasPotentialFactory = true;
				if (!fallbackFactory || object->getID() < fallbackFactory->getID() ||
					(object->getID() == fallbackFactory->getID() &&
						command < fallbackCommand)) {
					fallbackFactory = object;
					fallbackTemplate = product;
					fallbackCommand = command;
				}
				continue;
			}
			const CanMakeType admission =
				TheBuildAssistant->canMakeUnit(object, product);
			ProductionUpdateInterface *production =
				object->getProductionUpdateInterface();
			const Bool productionCanAdvance = CanSkirmishAIRecoveryUpdateAdvance(
				object, static_cast<ProductionUpdate *>(production));
			const Bool internallyProgressing =
				IsSkirmishAIRecoveryFactoryAdmissionInternallyProgressing(
					admission == CANMAKE_OK,
					admission == CANMAKE_QUEUE_FULL,
					admission == CANMAKE_PARKING_PLACES_FULL,
					productionCanAdvance);
			const Bool boundedOnly =
				IsSkirmishAIRecoveryAdmissionBounded(
					admission == CANMAKE_NO_MONEY,
					admission == CANMAKE_FACTORY_IS_DISABLED,
					admission == CANMAKE_NO_PREREQ,
					admission == CANMAKE_MAXED_OUT_FOR_PLAYER) ||
				IsSkirmishAIRecoveryFactorySchedulingBounded(
					admission == CANMAKE_OK,
					admission == CANMAKE_QUEUE_FULL,
					admission == CANMAKE_PARKING_PLACES_FULL,
					productionCanAdvance);
			if (internallyProgressing && hasPotentialFactory)
				*hasPotentialFactory = true;
			if (boundedOnly && hasBoundedFactory)
				*hasBoundedFactory = true;
			if (!internallyProgressing && !boundedOnly)
				continue;
			if (!fallbackFactory || object->getID() < fallbackFactory->getID() ||
				(object->getID() == fallbackFactory->getID() &&
					command < fallbackCommand)) {
				fallbackFactory = object;
				fallbackTemplate = product;
				fallbackCommand = command;
			}
			if (!IsSkirmishAIRecoveryFactoryBestCandidate(
					admission == CANMAKE_OK, productionCanAdvance))
				continue;
			if (!bestFactory || object->getID() < bestFactory->getID() ||
				(object->getID() == bestFactory->getID() && command < bestCommand)) {
				bestFactory = object;
				bestTemplate = product;
				bestCommand = command;
			}
		}
	}
	if (!bestFactory) {
		bestFactory = fallbackFactory;
		bestTemplate = fallbackTemplate;
		bestCommand = fallbackCommand;
	}
	if (bestFactory && bestTemplate) {
		if (builderTemplate)
			*builderTemplate = bestTemplate;
		if (factory)
			*factory = bestFactory;
		return true;
	}

	return false;
}

void AISkirmishPlayer::normalizeRecoveryWorkOrders(
	const ThingTemplate *primaryTemplate)
{
	if (!primaryTemplate || !m_player || !TheGameLogic)
		return;

	// Production death/capture refunds the entry but does not own the AI
	// WorkOrder.  Clear only compatible stale bindings here, before recovery
	// decides whether to allocate or reuse an order.  A valid binding must
	// still point at an owned live producer with the matching real queue entry.
	for (DLINK_ITERATOR<TeamInQueue> iter = iterate_TeamBuildQueue();
		!iter.done(); iter.advance()) {
		TeamInQueue *team = iter.cur();
		if (!team)
			continue;
		for (WorkOrder *order = team->m_workOrders; order;
			order = order->m_next) {
			if (order->m_factoryID == INVALID_ID || !order->m_thing ||
				!order->m_thing->isKindOf(KINDOF_DOZER) ||
				!HasSkirmishAICommandSetForTemplate(
					order->m_thing->friend_getCommandSetString(), primaryTemplate) ||
				order->m_numCompleted >= order->m_numRequired)
				continue;

			Object *factory = TheGameLogic->findObjectByID(order->m_factoryID);
			if (!IsLiveSkirmishAIRecoveryObject(factory, m_player) ||
				!HasSkirmishAIRecoveryProduction(factory, order->m_thing))
				order->m_factoryID = INVALID_ID;
		}
	}
}

static Bool CanSkirmishAIRecoveryProductionAdvance(
	Object *factory, ProductionUpdateInterface *production)
{
	return production && CanSkirmishAIRecoveryUpdateAdvance(
		factory, static_cast<ProductionUpdate *>(production));
}

static Bool CanSkirmishAIRecoveryBuildIgnoringMax(
	const Player *player, const ThingTemplate *product)
{
	if (!player || !product || !player->allowedToBuild(product) ||
		product->getBuildable() == BSTATUS_NO)
		return false;
	if (product->getBuildable() == BSTATUS_IGNORE_PREREQUISITES)
		return true;
	if (product->getBuildable() == BSTATUS_ONLY_BY_AI &&
		player->getPlayerType() != PLAYER_COMPUTER)
		return false;

	Bool prerequisitesReady = true;
	for (Int i = 0; i < product->getPrereqCount(); ++i) {
		const ProductionPrerequisite *prerequisite = product->getNthPrereq(i);
		if (!prerequisite || !prerequisite->isSatisfied(player))
			prerequisitesReady = false;
	}
#if defined(RTS_DEBUG)
	if (player->ignoresPrereqs())
		prerequisitesReady = true;
#endif
	return prerequisitesReady;
}

static Bool WouldSkirmishAIRecoveryQueueCancellationFreeMax(
	const Player *player, const ThingTemplate *product,
	const Object *cancelFactory, ProductionID cancelProductionID)
{
	if (!player || !product || !TheGameLogic)
		return false;
	const UnsignedInt maxCount = product->getMaxSimultaneousOfType();
	if (maxCount == 0)
		return false;

	UnsignedInt predictedCount = 0;
	const NameKeyType linkKey = product->getMaxSimultaneousLinkKey();
	for (Object *object = TheGameLogic->getFirstObject(); object;
		object = object->getNextObject()) {
		if (object->getControllingPlayer() != player || object->isEffectivelyDead())
			continue;
		const ThingTemplate *objectTemplate = object->getTemplate();
		if (objectTemplate &&
			(product->isEquivalentTo(objectTemplate) ||
			 (linkKey != NAMEKEY_INVALID &&
			  linkKey == objectTemplate->getMaxSimultaneousLinkKey())))
			++predictedCount;

		if (!product->isKindOf(KINDOF_STRUCTURE)) {
			ProductionUpdateInterface *production =
				object->getProductionUpdateInterface();
			for (const ProductionEntry *entry = production
					? production->firstProduction() : nullptr;
				 entry; entry = production->nextProduction(entry)) {
				if (object == cancelFactory &&
					entry->getProductionID() == cancelProductionID)
					continue;
				if (entry->getProductionType() == PRODUCTION_UNIT &&
					entry->getProductionObject() == product)
					++predictedCount;
			}
		}
		if (!WouldSkirmishAIRecoveryCancellationFreeMax(
				maxCount, predictedCount))
			return false;
	}
	return WouldSkirmishAIRecoveryCancellationFreeMax(maxCount, predictedCount);
}

Bool AISkirmishPlayer::hasRecoveryBuilderQueued(
	const ThingTemplate *primaryTemplate, Bool *paid, ObjectID *factoryID,
	ProductionID *productionID)
{
	if (paid)
		*paid = false;
	if (factoryID)
		*factoryID = INVALID_ID;
	if (productionID)
		*productionID = PRODUCTIONID_INVALID;
	// Production queues are authoritative for paid work.  This also sees
	// script-issued dozers and GLA worker/supply-worker entries that do not
	// necessarily have a matching TeamInQueue order.
	ObjectID selectedPaidFactoryID = INVALID_ID;
	ProductionID selectedPaidProductionID = PRODUCTIONID_INVALID;
	Bool selectedPaidFactoryCanAdvance = false;
	ObjectID trackedPaidFactoryID = INVALID_ID;
	ProductionID trackedPaidProductionID = PRODUCTIONID_INVALID;
	Bool trackedPaidEntryExists = false;
	if (m_player && TheGameLogic) {
		for (Object *factory = TheGameLogic->getFirstObject(); factory;
			factory = factory->getNextObject()) {
			if (!IsLiveSkirmishAIRecoveryObject(factory, m_player))
				continue;
			ProductionUpdateInterface *production = factory->getProductionUpdateInterface();
			if (!production)
				continue;
			for (const ProductionEntry *entry = production->firstProduction(); entry;
				entry = production->nextProduction(entry)) {
				if (entry->getProductionType() != PRODUCTION_UNIT)
					continue;
				const ThingTemplate *product = entry->getProductionObject();
				if (product && product->isKindOf(KINDOF_DOZER) &&
					HasSkirmishAICommandSetForTemplate(
						product->friend_getCommandSetString(), primaryTemplate) &&
					entry->getProductionQuantityRemaining() > 0) {
					const Bool productionCanAdvance =
						CanSkirmishAIRecoveryProductionAdvance(factory, production);
					if (IsSkirmishAIRecoveryProductionIdentityMatch(
							m_recoveryBuilderFactoryID,
							m_recoveryBuilderProductionID,
							factory->getID(),
							static_cast<Int>(entry->getProductionID()),
							INVALID_ID, PRODUCTIONID_INVALID)) {
						trackedPaidFactoryID = factory->getID();
						trackedPaidProductionID = entry->getProductionID();
						trackedPaidEntryExists = true;
					}
					if (ShouldSelectSkirmishAIRecoveryPaidQueueProducer(
						selectedPaidFactoryID != INVALID_ID,
						selectedPaidFactoryCanAdvance, productionCanAdvance)) {
						selectedPaidFactoryID = factory->getID();
						selectedPaidProductionID = entry->getProductionID();
						selectedPaidFactoryCanAdvance = productionCanAdvance;
					}
				}
			}
		}
	}
	if (ShouldPreferTrackedSkirmishAIRecoveryPaidQueue(
			trackedPaidEntryExists, selectedPaidFactoryCanAdvance)) {
		selectedPaidFactoryID = trackedPaidFactoryID;
		selectedPaidProductionID = trackedPaidProductionID;
	}
	if (selectedPaidFactoryID != INVALID_ID) {
		if (paid)
			*paid = true;
		if (factoryID)
			*factoryID = selectedPaidFactoryID;
		if (productionID)
			*productionID = selectedPaidProductionID;
		return true;
	}

	// A WorkOrder without a real production entry is an unpaid waiting
	// request.  Count all dozer templates here; resource-gatherer workers are
	// also recoverable builders and must not trigger another paid queue.
	for (DLINK_ITERATOR<TeamInQueue> iter = iterate_TeamBuildQueue();
		!iter.done(); iter.advance()) {
		TeamInQueue *team = iter.cur();
		if (!team)
			continue;
		for (WorkOrder *order = team->m_workOrders; order; order = order->m_next) {
			const Bool compatibleOrder = order->m_thing &&
				order->m_thing->isKindOf(KINDOF_DOZER) &&
				HasSkirmishAICommandSetForTemplate(
					order->m_thing->friend_getCommandSetString(), primaryTemplate);
			const Bool reusable = ShouldUseCurrentSkirmishAIStrategyControllerBehavior()
				? IsSkirmishAIRecoveryReusableWorkOrder(
					order->m_factoryID == INVALID_ID,
					compatibleOrder,
					order->m_numCompleted < order->m_numRequired,
					team->m_team == m_player->getDefaultTeam(),
					team->m_reinforcement)
				: order->m_factoryID == INVALID_ID && compatibleOrder &&
					order->m_numCompleted < order->m_numRequired;
			if (!reusable)
				continue;
			if (factoryID)
				*factoryID = order->m_factoryID;
			return true;
		}
	}
	return false;
}

Bool AISkirmishPlayer::queueRecoveryBuilder(
	const ThingTemplate *builderTemplate, Object *factory)
{
	if (!builderTemplate || !factory || !m_player ||
		!IsLiveSkirmishAIRecoveryObject(factory, m_player) ||
		factory->testStatus(OBJECT_STATUS_UNDER_CONSTRUCTION) ||
		factory->isDestroyed())
		return false;
	ProductionUpdateInterface *production = factory->getProductionUpdateInterface();
	if (!production ||
		!CanSkirmishAIRecoveryProductionAdvance(factory, production) ||
		TheBuildAssistant->canMakeUnit(factory, builderTemplate) != CANMAKE_OK)
		return false;

	WorkOrder *order = nullptr;
	Bool selectedIsResourceGatherer = false;
	for (DLINK_ITERATOR<TeamInQueue> iter = iterate_TeamBuildQueue();
		!iter.done(); iter.advance()) {
		TeamInQueue *team = iter.cur();
		if (!team)
			continue;
		for (WorkOrder *waiting = team->m_workOrders; waiting;
			waiting = waiting->m_next) {
			const Bool eligible = IsSkirmishAIRecoveryReusableWorkOrder(
				waiting->m_factoryID == INVALID_ID,
				waiting->m_thing &&
					waiting->m_thing->isEquivalentTo(builderTemplate),
				waiting->m_numCompleted < waiting->m_numRequired,
				team->m_team == m_player->getDefaultTeam(),
				team->m_reinforcement);
			if (ShouldSelectSkirmishAIRecoveryReusableWorkOrder(
					order != nullptr, selectedIsResourceGatherer,
					eligible, waiting->m_isResourceGatherer)) {
				order = waiting;
				selectedIsResourceGatherer = waiting->m_isResourceGatherer;
			}
		}
	}
	Bool queued = false;
	const ThingTemplate *previousAuthorization = m_recoveryAuthorizedThing;
	m_recoveryAuthorizedThing = builderTemplate;
	const ProductionID productionID = production->requestUniqueUnitID();
	queued = production->queueCreateUnit(builderTemplate, productionID);
	m_recoveryAuthorizedThing = previousAuthorization;
	const SkirmishAIRecoveryQueueCommit commit =
		GetSkirmishAIRecoveryQueueCommit(order != nullptr, queued);
	if (!commit.storeProductionIdentity)
		return false;

	if (commit.bindReusableWorkOrder) {
		order->m_factoryID = factory->getID();
		if (usesProductionBehavior())
			order->m_productionID = static_cast<Int>(productionID);
	}
	m_recoveryBuilderFactoryID = factory->getID();
	m_recoveryBuilderProductionID = productionID;
	m_recoveryBuilderCancellationOwned = true;
	m_teamDelay = 0;
	if (TheGlobalData->m_debugAI) {
		AsciiString message = "Critical recovery queued builder from ";
		message.concat(factory->getTemplate()->getName());
		TheScriptEngine->AppendDebugMessage(message, false);
	}
	return true;
}

void AISkirmishPlayer::clearRecoveryBuilderProduction()
{
	m_recoveryBuilderFactoryID = INVALID_ID;
	m_recoveryBuilderProductionID = PRODUCTIONID_INVALID;
	m_recoveryBuilderCancellationOwned = false;
}

void AISkirmishPlayer::validateRecoveryBuilderProduction()
{
	if (!IsSkirmishAIRecoveryProductionIdentityTracked(
			m_recoveryBuilderFactoryID, m_recoveryBuilderProductionID,
			INVALID_ID, PRODUCTIONID_INVALID)) {
		clearRecoveryBuilderProduction();
		return;
	}
	Object *factory = TheGameLogic
		? TheGameLogic->findObjectByID(m_recoveryBuilderFactoryID) : nullptr;
	ProductionUpdateInterface *production =
		IsLiveSkirmishAIRecoveryObject(factory, m_player)
			? factory->getProductionUpdateInterface() : nullptr;
	Bool hasExactEntry = false;
	for (const ProductionEntry *entry = production
			? production->firstProduction() : nullptr;
		entry; entry = production->nextProduction(entry)) {
		if (entry->getProductionType() == PRODUCTION_UNIT &&
			static_cast<Int>(entry->getProductionID()) ==
				m_recoveryBuilderProductionID &&
			entry->getProductionQuantityRemaining() > 0) {
			hasExactEntry = true;
			break;
		}
	}
	if (ShouldClearSkirmishAIRecoveryProductionIdentity(
			production != nullptr, hasExactEntry))
		clearRecoveryBuilderProduction();
}

void AISkirmishPlayer::bindRecoveryBuilderProductionIfNeeded(
	Bool hasCompletedPrimaryCenter, Bool paidQueueExists,
	ObjectID factoryID, ProductionID productionID)
{
	if (ShouldBindSkirmishAIRecoveryProductionIdentity(
			usesCriticalRecoveryBehavior(), hasCompletedPrimaryCenter,
			HasSkirmishAIRecoveryScaffoldReplacementAttempt(
				m_recoveryPlacementAttempt, g_skirmishAIRecoveryOffsetCount),
			HasSkirmishAIRecoveryObservedReplacement(
				m_recoveryPlacementAttempt, g_skirmishAIRecoveryOffsetCount),
			IsSkirmishAIRecoveryProductionIdentityTracked(
				m_recoveryBuilderFactoryID, m_recoveryBuilderProductionID,
				INVALID_ID, PRODUCTIONID_INVALID),
			paidQueueExists, factoryID != INVALID_ID,
			productionID != PRODUCTIONID_INVALID)) {
		m_recoveryBuilderFactoryID = factoryID;
		m_recoveryBuilderProductionID = productionID;
		m_recoveryBuilderCancellationOwned = false;
	}
}

Bool AISkirmishPlayer::failoverRecoveryBuilderQueue(
	const ThingTemplate *primaryTemplate, Object *boundedFactory,
	ProductionID boundedProductionID)
{
	if (!primaryTemplate || !boundedFactory || !m_player ||
		!IsLiveSkirmishAIRecoveryObject(boundedFactory, m_player) ||
		boundedProductionID == PRODUCTIONID_INVALID)
		return false;
	if (ShouldUseCurrentSkirmishAIRecoveryBoundedFailover() &&
		m_recoveryBuilderFailoverConsumed)
		return false;
	if (HasSkirmishAIRecoveryObservedReplacement(
			m_recoveryPlacementAttempt, g_skirmishAIRecoveryOffsetCount))
		return false;

	ProductionUpdateInterface *boundedProduction =
		boundedFactory->getProductionUpdateInterface();
	if (!boundedProduction || CanSkirmishAIRecoveryProductionAdvance(
			boundedFactory, boundedProduction))
		return false;

	const ProductionEntry *boundedEntry = nullptr;
	ProductionID firstCompatibleProductionID = PRODUCTIONID_INVALID;
	for (const ProductionEntry *entry = boundedProduction->firstProduction(); entry;
		entry = boundedProduction->nextProduction(entry)) {
		const ThingTemplate *product = entry->getProductionType() == PRODUCTION_UNIT
			? entry->getProductionObject() : nullptr;
		if (product && product->isKindOf(KINDOF_DOZER) &&
			HasSkirmishAICommandSetForTemplate(
				product->friend_getCommandSetString(), primaryTemplate) &&
			entry->getProductionQuantityRemaining() > 0) {
			if (firstCompatibleProductionID == PRODUCTIONID_INVALID)
				firstCompatibleProductionID = entry->getProductionID();
			if (entry->getProductionID() == boundedProductionID) {
				boundedEntry = entry;
				break;
			}
		}
	}
	if (!boundedEntry)
		return false;
	const Bool exactIdentityMatches =
		IsSkirmishAIRecoveryProductionIdentityMatch(
			m_recoveryBuilderFactoryID, m_recoveryBuilderProductionID,
			boundedFactory->getID(), static_cast<Int>(boundedProductionID),
			INVALID_ID, PRODUCTIONID_INVALID);
	const Bool cancellationOwned =
		!ShouldUseCurrentSkirmishAIRecoveryCancellationOwnership() ||
		m_recoveryBuilderCancellationOwned;
	const Bool cancelBoundedEntry =
		ShouldCancelSkirmishAIRecoveryExactPaidQueueForFailover(
			cancellationOwned, exactIdentityMatches, boundedEntry != nullptr);
	const Bool retainBoundedEntry =
		ShouldUseCurrentSkirmishAIRecoveryUnownedQueueFailover() &&
		ShouldUseSkirmishAIRecoveryNonCancellingFailover(
			cancellationOwned, exactIdentityMatches, boundedEntry != nullptr);
	if (!cancelBoundedEntry && !retainBoundedEntry)
		return false;

	const ThingTemplate *boundedTemplate = boundedEntry->getProductionObject();
	const Int refund = boundedTemplate->calcCostToBuild(m_player);
	const Int currentMoney = m_player->getMoney()->countMoney();
	const Int availableAfterRefund = AddSkirmishAIRecoveryCost(currentMoney, refund);
	Object *alternateFactory = nullptr;
	const ThingTemplate *alternateTemplate = nullptr;
	Int alternateCommand = 0;
	Int alternateRank = 3;
	for (Object *object = TheGameLogic->getFirstObject(); object;
		object = object->getNextObject()) {
		if (object == boundedFactory ||
			!IsLiveSkirmishAIRecoveryObject(object, m_player) ||
			object->testStatus(OBJECT_STATUS_UNDER_CONSTRUCTION))
			continue;
		ProductionUpdateInterface *production =
			object->getProductionUpdateInterface();
		if (!production || !CanSkirmishAIRecoveryProductionAdvance(
				object, production))
			continue;
		const CommandSet *commandSet = TheControlBar
			? TheControlBar->findCommandSet(object->getCommandSetString()) : nullptr;
		if (!commandSet)
			continue;
		for (Int command = 0; command < MAX_COMMANDS_PER_SET; ++command) {
			const CommandButton *button = commandSet->getCommandButton(command);
			const ThingTemplate *product = button ? button->getThingTemplate() : nullptr;
			if (!button ||
				(button->getCommandType() != GUI_COMMAND_UNIT_BUILD &&
				 button->getCommandType() != GUI_COMMAND_DOZER_CONSTRUCT) ||
				!product || !product->isKindOf(KINDOF_DOZER) ||
				!HasSkirmishAICommandSetForTemplate(
					product->friend_getCommandSetString(), primaryTemplate))
				continue;
			const CanMakeType admission =
				TheBuildAssistant->canMakeUnit(object, product);
			const Int cost = product->calcCostToBuild(m_player);
			const Bool refundAffordable = cost >= 0 && availableAfterRefund >= cost;
			const Bool independentlyAffordable = cost >= 0 && currentMoney >= cost;
			const Bool cancellationFreesMax =
				WouldSkirmishAIRecoveryQueueCancellationFreeMax(
					m_player, product, boundedFactory, boundedProductionID);
			const Bool nonMaxBuildable =
				CanSkirmishAIRecoveryBuildIgnoringMax(m_player, product);
			const Bool queueReady =
				production->canQueueCreateUnit(product) == CANMAKE_OK;
			const Bool eligible = retainBoundedEntry
				? IsSkirmishAIRecoveryNonCancellingFailoverAdmissionEligible(
					admission == CANMAKE_OK, independentlyAffordable, queueReady)
				: IsSkirmishAIRecoveryFailoverAdmissionEligible(
					admission == CANMAKE_OK,
					admission == CANMAKE_NO_MONEY,
					admission == CANMAKE_MAXED_OUT_FOR_PLAYER,
					refundAffordable, cancellationFreesMax,
					nonMaxBuildable, queueReady);
			if (!eligible)
				continue;
			const Int rank = GetSkirmishAIRecoveryFailoverAdmissionRank(
				admission == CANMAKE_OK, admission == CANMAKE_NO_MONEY,
				admission == CANMAKE_MAXED_OUT_FOR_PLAYER);
			if (!alternateFactory || rank < alternateRank ||
				(rank == alternateRank && object->getID() < alternateFactory->getID()) ||
				(rank == alternateRank && object->getID() == alternateFactory->getID() &&
				 command < alternateCommand)) {
				alternateFactory = object;
				alternateTemplate = product;
				alternateCommand = command;
				alternateRank = rank;
			}
		}
	}
	if (!alternateFactory || !alternateTemplate)
		return false;
	if (retainBoundedEntry) {
		// An adopted ordinary queue is authoritative but is not ours to cancel.
		// Pay an independently admissible alternate without touching that entry,
		// its WorkOrder, or its original debit. queueRecoveryBuilder changes the
		// tracked identity only after the new queue succeeds.
		if (TheBuildAssistant->canMakeUnit(
				alternateFactory, alternateTemplate) != CANMAKE_OK ||
			!queueRecoveryBuilder(alternateTemplate, alternateFactory))
			return false;
		if (ShouldUseCurrentSkirmishAIRecoveryBoundedFailover())
			m_recoveryBuilderFailoverConsumed =
				GetSkirmishAIRecoveryBuilderFailoverConsumedAfterQueueAttempt(
					m_recoveryBuilderFailoverConsumed, true);
		m_recoveryPlacementAttempt = MarkSkirmishAIRecoveryScaffoldReplacementAttempt(
			m_recoveryPlacementAttempt, g_skirmishAIRecoveryOffsetCount);
		return true;
	}

	clearRecoveryBuilderProduction();
	boundedProduction->cancelUnitCreate(boundedProductionID);
	Bool bindingCleared = false;
	const Bool selectedIsFirstCompatible =
		firstCompatibleProductionID == boundedProductionID;
	const Bool productionBehavior = usesProductionBehavior();
	for (DLINK_ITERATOR<TeamInQueue> iter = iterate_TeamBuildQueue();
		!iter.done() && !bindingCleared; iter.advance()) {
		TeamInQueue *team = iter.cur();
		if (!team)
			continue;
		for (WorkOrder *order = team->m_workOrders; order;
			order = order->m_next) {
			const Bool ownsCancelledEntry = productionBehavior
				? (order->m_productionID == static_cast<Int>(boundedProductionID) ||
				   (order->m_productionID == 0 && selectedIsFirstCompatible))
				: selectedIsFirstCompatible;
			if (order->m_thing && ownsCancelledEntry &&
				ShouldClearSkirmishAIRecoveryExactFailoverBinding(
					true, order->m_factoryID == boundedFactory->getID(),
					order->m_thing->isEquivalentTo(boundedTemplate),
					order->m_numCompleted < order->m_numRequired)) {
				order->m_factoryID = INVALID_ID;
				if (productionBehavior)
					order->m_productionID = 0;
				bindingCleared = true;
				break;
			}
		}
	}

	m_recoveryPlacementAttempt = ReconcileSkirmishAIRecoveryReplacementAttempt(
		m_recoveryPlacementAttempt, g_skirmishAIRecoveryOffsetCount, false);
	// MAXED becomes actionable only after the exact bounded entry leaves the
	// player's queued-unit count.  Recheck before paying the alternate.
	if (TheBuildAssistant->canMakeUnit(
			alternateFactory, alternateTemplate) != CANMAKE_OK)
		return false;
	if (!queueRecoveryBuilder(alternateTemplate, alternateFactory))
		return false;
	if (ShouldUseCurrentSkirmishAIRecoveryBoundedFailover())
		m_recoveryBuilderFailoverConsumed =
			GetSkirmishAIRecoveryBuilderFailoverConsumedAfterQueueAttempt(
				m_recoveryBuilderFailoverConsumed, true);
	m_recoveryPlacementAttempt = MarkSkirmishAIRecoveryScaffoldReplacementAttempt(
		m_recoveryPlacementAttempt, g_skirmishAIRecoveryOffsetCount);
	return true;
}

Bool AISkirmishPlayer::cancelRecoveryBuilderQueueForNativeRespawn(
	const ThingTemplate *primaryTemplate)
{
	if (!ShouldUseCurrentSkirmishAIRecoveryNativeHoleOwnership() ||
		!primaryTemplate || !m_player || !TheGameLogic ||
		(ShouldUseCurrentSkirmishAIRecoveryCancellationOwnership() &&
		 !m_recoveryBuilderCancellationOwned) ||
		!IsSkirmishAIRecoveryProductionIdentityTracked(
			m_recoveryBuilderFactoryID, m_recoveryBuilderProductionID,
			INVALID_ID, PRODUCTIONID_INVALID))
		return false;

	Object *factory = TheGameLogic->findObjectByID(m_recoveryBuilderFactoryID);
	ProductionUpdateInterface *production =
		IsLiveSkirmishAIRecoveryObject(factory, m_player)
			? factory->getProductionUpdateInterface() : nullptr;
	if (!production)
		return false;

	const ProductionEntry *exactEntry = nullptr;
	ProductionID firstCompatibleProductionID = PRODUCTIONID_INVALID;
	for (const ProductionEntry *entry = production->firstProduction(); entry;
		entry = production->nextProduction(entry)) {
		const ThingTemplate *product = entry->getProductionType() == PRODUCTION_UNIT
			? entry->getProductionObject() : nullptr;
		if (!product || !product->isKindOf(KINDOF_DOZER) ||
			!HasSkirmishAICommandSetForTemplate(
				product->friend_getCommandSetString(), primaryTemplate) ||
			entry->getProductionQuantityRemaining() <= 0)
			continue;
		if (firstCompatibleProductionID == PRODUCTIONID_INVALID)
			firstCompatibleProductionID = entry->getProductionID();
		if (IsSkirmishAIRecoveryProductionIdentityMatch(
				m_recoveryBuilderFactoryID, m_recoveryBuilderProductionID,
				factory->getID(), static_cast<Int>(entry->getProductionID()),
				INVALID_ID, PRODUCTIONID_INVALID)) {
			exactEntry = entry;
			break;
		}
	}
	if (!ShouldCancelSkirmishAIRecoveryExactPaidQueueForNativeLifecycle(
			true,
			!ShouldUseCurrentSkirmishAIRecoveryCancellationOwnership() ||
				m_recoveryBuilderCancellationOwned,
			true, exactEntry != nullptr))
		return false;

	const ThingTemplate *cancelledTemplate = exactEntry->getProductionObject();
	const ProductionID cancelledProductionID = exactEntry->getProductionID();
	production->cancelUnitCreate(cancelledProductionID);
	clearRecoveryBuilderProduction();

	Bool bindingCleared = false;
	const Bool selectedIsFirstCompatible =
		firstCompatibleProductionID == cancelledProductionID;
	const Bool productionBehavior = usesProductionBehavior();
	for (DLINK_ITERATOR<TeamInQueue> iter = iterate_TeamBuildQueue();
		!iter.done() && !bindingCleared; iter.advance()) {
		TeamInQueue *team = iter.cur();
		if (!team)
			continue;
		for (WorkOrder *order = team->m_workOrders; order;
			order = order->m_next) {
			const Bool ownsCancelledEntry = productionBehavior
				? (order->m_productionID == static_cast<Int>(cancelledProductionID) ||
				   (order->m_productionID == 0 && selectedIsFirstCompatible))
				: selectedIsFirstCompatible;
			if (order->m_thing && ownsCancelledEntry &&
				ShouldClearSkirmishAIRecoveryExactFailoverBinding(
					true, order->m_factoryID == factory->getID(),
					order->m_thing->isEquivalentTo(cancelledTemplate),
					order->m_numCompleted < order->m_numRequired)) {
				order->m_factoryID = INVALID_ID;
				if (productionBehavior)
					order->m_productionID = 0;
				bindingCleared = true;
				break;
			}
		}
	}
	m_recoveryPlacementAttempt = ReconcileSkirmishAIRecoveryReplacementAttempt(
		m_recoveryPlacementAttempt, g_skirmishAIRecoveryOffsetCount, false);
	return true;
}

Object *AISkirmishPlayer::findRecoveryBuilder(
	const Coord3D *position, const ThingTemplate *primaryTemplate) const
{
	std::vector<Object *> builders;
	CollectSkirmishAIRecoveryBuilders(
		m_player, position, primaryTemplate, &builders);
	return builders.empty() ? nullptr : builders[0];
}

Bool AISkirmishPlayer::hasCriticalRecoveryPlacementRoute(
	const ThingTemplate *primaryTemplate, Object *builder) const
{
	if (!primaryTemplate || !builder || !m_player || !TheGameLogic ||
		!IsLiveSkirmishAIRecoveryObject(builder, m_player) ||
		builder->isDisabledByType(DISABLED_UNMANNED))
		return false;
	AIUpdateInterface *ai = builder->getAIUpdateInterface();
	if (!ai || !ai->getDozerAIInterface())
		return false;

	Coord3D origin = m_recoveryLocation;
	BuildListInfo *info = findPrimaryCommandCenterBuildInfo(primaryTemplate);
	if (origin.x == 0.0f && origin.y == 0.0f && origin.z == 0.0f && info)
		origin = *info->getLocation();
	const Int start = m_recoveryPlacementAttempt >= 0
		? m_recoveryPlacementAttempt % g_skirmishAIRecoveryOffsetCount : 0;
	const Real placementStep = max(PATHFIND_CELL_SIZE_F,
		2.0f * primaryTemplate->getTemplateGeometryInfo().getBoundingCircleRadius() +
		PATHFIND_CELL_SIZE_F);
	for (Int attempt = 0; attempt < g_skirmishAIRecoveryOffsetCount; ++attempt) {
		const SkirmishAIRecoveryOffset &offset =
			g_skirmishAIRecoveryOffsets[(start + attempt) % g_skirmishAIRecoveryOffsetCount];
		Coord3D position = origin;
		position.x += offset.x * placementStep;
		position.y += offset.y * placementStep;
		position.z = TheTerrainLogic->getGroundHeight(position.x, position.y);
		const Bool usable = IsSkirmishAIRecoveryLocationSafe(
			m_player, &position, primaryTemplate) &&
			TheBuildAssistant->isLocationLegalToBuild(
				&position, primaryTemplate, m_recoveryAngle,
				BuildAssistant::CLEAR_PATH |
				BuildAssistant::TERRAIN_RESTRICTIONS |
				BuildAssistant::NO_OBJECT_OVERLAP,
				builder, m_player) == LBC_OK &&
			ai->isPathAvailable(&position);
		TheTerrainVisual->removeAllBibs();
		if (usable)
			return true;
	}
	return false;
}

Bool AISkirmishPlayer::prepareCriticalRecoveryBuilder(Object *builder)
{
	if (!builder || !m_player || !TheGameLogic ||
		!IsLiveSkirmishAIRecoveryObject(builder, m_player) ||
		!builder->isKindOf(KINDOF_DOZER) ||
		builder->isDisabledByType(DISABLED_UNMANNED))
		return false;

	AIUpdateInterface *ai = builder->getAIUpdateInterface();
	DozerAIInterface *dozerAI = ai ? ai->getDozerAIInterface() : nullptr;
	if (!ai || !dozerAI ||
		!CanSkirmishAIRecoveryUpdateAdvance(builder, ai))
		return false;
	if (ShouldUseCurrentSkirmishAIStrategyControllerBehavior()) {
		Team *defaultTeam = m_player->getDefaultTeam();
		if (defaultTeam && builder->getTeam() != defaultTeam)
			builder->setTeam(defaultTeam);
	}
	// A recovery-funded GLA worker may still carry the resource order's forced
	// supply transition.  Clear it before that transition resets the build task.
	SupplyTruckAIInterface *supplyAI = ai->getSupplyTruckAIInterface();
	if (supplyAI)
		supplyAI->setForceWantingState(false);

	// Keep an already active recovery scaffold untouched.  This helper is
	// called only while the primary center is missing, but the guard also
	// protects a same-frame callback from cancelling a critical build task.
	const ObjectID criticalID = m_recoveryConstructionID;
	for (Int task = DOZER_TASK_FIRST; task < DOZER_NUM_TASKS; ++task) {
		const DozerTask dozerTask = static_cast<DozerTask>(task);
		if (!dozerAI->isTaskPending(dozerTask))
			continue;
		const ObjectID targetID = dozerAI->getTaskTarget(dozerTask);
		Object *target = targetID != INVALID_ID
			? TheGameLogic->findObjectByID(targetID) : nullptr;
		if (criticalID != INVALID_ID && targetID == criticalID &&
			dozerAI->getCurrentTask() == dozerTask &&
			IsLiveSkirmishAIRecoveryObject(target, m_player) &&
			target->getBuilderID() == builder->getID() &&
			target->isKindOf(KINDOF_COMMANDCENTER) &&
			target->testStatus(OBJECT_STATUS_UNDER_CONSTRUCTION))
			return false;
		dozerAI->cancelTask(dozerTask);
	}

	// A current task is not necessarily still present in the pending-task
	// array.  Cancel it as well when it is ordinary work, then force the same
	// idle transition used by the normal build command path.
	const DozerTask currentTask = dozerAI->getCurrentTask();
	if (currentTask >= DOZER_TASK_FIRST && currentTask < DOZER_NUM_TASKS) {
		const ObjectID targetID = dozerAI->getTaskTarget(currentTask);
		if (criticalID == INVALID_ID || targetID != criticalID)
			dozerAI->cancelTask(currentTask);
	}
	ai->aiIdle(CMD_FROM_AI);
	return !dozerAI->isAnyTaskPending();
}

Bool AISkirmishPlayer::tryCriticalCommandCenterConstruction(
	const ThingTemplate *primaryTemplate, BuildListInfo *info, Object *builder)
{
	if (!primaryTemplate || !builder || !m_player || !TheGameLogic || !info ||
		!IsLiveSkirmishAIRecoveryObject(builder, m_player) ||
		builder->isDisabledByType(DISABLED_UNMANNED))
		return false;
	AIUpdateInterface *ai = builder->getAIUpdateInterface();
	DozerAIInterface *dozerAI = ai ? ai->getDozerAIInterface() : nullptr;
	if (!ai || !dozerAI ||
		!CanSkirmishAIRecoveryUpdateAdvance(builder, ai) ||
		TheBuildAssistant->canMakeUnit(builder, primaryTemplate) != CANMAKE_OK)
		return false;

	const Int cost = primaryTemplate->calcCostToBuild(m_player);
	if (cost < 0 || m_player->getMoney()->countMoney() < cost)
		return false;

	Coord3D origin = m_recoveryLocation;
	if (origin.x == 0.0f && origin.y == 0.0f && origin.z == 0.0f && info)
		origin = *info->getLocation();
	const Int start = m_recoveryPlacementAttempt >= 0
		? m_recoveryPlacementAttempt % g_skirmishAIRecoveryOffsetCount : 0;
	const Real angle = m_recoveryAngle;
	// A few path cells still overlap an obstructed command-center footprint.
	// Space the alternatives by the building's diameter so a neighboring site
	// can be legal even when another structure occupies the original center.
	const Real placementStep = max(PATHFIND_CELL_SIZE_F,
		2.0f * primaryTemplate->getTemplateGeometryInfo().getBoundingCircleRadius() +
		PATHFIND_CELL_SIZE_F);
	for (Int attempt = 0; attempt < g_skirmishAIRecoveryOffsetCount; ++attempt) {
		const SkirmishAIRecoveryOffset &offset =
			g_skirmishAIRecoveryOffsets[(start + attempt) % g_skirmishAIRecoveryOffsetCount];
		Coord3D position = origin;
		position.x += offset.x * placementStep;
		position.y += offset.y * placementStep;
		position.z = TheTerrainLogic->getGroundHeight(position.x, position.y);

		if (!IsSkirmishAIRecoveryLocationSafe(
				m_player, &position, primaryTemplate) ||
			TheBuildAssistant->isLocationLegalToBuild(
				&position,
				primaryTemplate,
				angle,
				BuildAssistant::CLEAR_PATH |
				BuildAssistant::TERRAIN_RESTRICTIONS |
				BuildAssistant::NO_OBJECT_OVERLAP,
				builder,
				m_player) != LBC_OK ||
			!ai->isPathAvailable(&position)) {
			TheTerrainVisual->removeAllBibs();
			continue;
		}
		TheTerrainVisual->removeAllBibs();

		// Defer optional-task preemption until the candidate has passed all
		// affordability, placement, and path checks.  This preserves useful
		// work when recovery is temporarily cash-starved or obstructed.
		if (!prepareCriticalRecoveryBuilder(builder)) {
			m_recoveryNextAttemptFrame = GetSkirmishAIRecoveryRetryFrame(
				TheGameLogic->getFrame(), 2 * LOGICFRAMES_PER_SECOND);
			return false;
		}
		ai = builder->getAIUpdateInterface();
		dozerAI = ai ? ai->getDozerAIInterface() : nullptr;
		if (!ai || !dozerAI || dozerAI->isAnyTaskPending() ||
			TheBuildAssistant->canMakeUnit(builder, primaryTemplate) != CANMAKE_OK) {
			m_recoveryNextAttemptFrame = GetSkirmishAIRecoveryRetryFrame(
				TheGameLogic->getFrame(), 2 * LOGICFRAMES_PER_SECOND);
			return false;
		}

		// Match the normal BuildAssistant command transition, including workers
		// that need to leave their supply-truck state before beginning a build.
		ai->aiIdle(CMD_FROM_AI);
		const ThingTemplate *previousAuthorization = m_recoveryAuthorizedThing;
		m_recoveryAuthorizedThing = primaryTemplate;
		Object *construction = ai->construct(
			primaryTemplate, &position, angle, m_player, FALSE);
		m_recoveryAuthorizedThing = previousAuthorization;
		if (!construction)
			continue;

		m_recoveryConstructionID = construction->getID();
		m_recoveryEvacuationDeadline = 0;
		m_recoveryLocation = position;
		m_recoveryPlacementAttempt = 0;
		m_recoveryNextAttemptFrame = 0;
		m_recoveryReserveCost = 0;
		info->setObjectID(construction->getID());
		info->setObjectTimestamp(TheGameLogic->getFrame() + 1);
		info->setUnderConstruction(true);
		m_buildDelay = 0;
		m_readyToBuildStructure = false;
		return true;
	}

	m_recoveryPlacementAttempt = AdvanceSkirmishAIRecoveryPlacementAttempt(
		m_recoveryPlacementAttempt, g_skirmishAIRecoveryOffsetCount);
	m_recoveryNextAttemptFrame =
		GetSkirmishAIRecoveryRetryFrame(
			TheGameLogic->getFrame(), 2 * LOGICFRAMES_PER_SECOND);
	return false;
}

void AISkirmishPlayer::enterRecoveryLastStand(Bool permanent)
{
	const Bool maintenance = m_recoveryImpossible || !permanent;
	if (permanent)
		m_recoveryEvacuationDeadline = 0;
	Object *tracked = m_recoveryConstructionID != INVALID_ID && TheGameLogic
		? TheGameLogic->findObjectByID(m_recoveryConstructionID) : nullptr;
	const Bool liveTracked =
		IsLiveSkirmishAIRecoveryObject(tracked, m_player);
	const PlayerTemplate *playerTemplate = m_player
		? m_player->getPlayerTemplate() : nullptr;
	const ThingTemplate *primaryTemplate = playerTemplate && TheThingFactory
		? TheThingFactory->findTemplate(playerTemplate->getStartingBuilding()) : nullptr;
	BuildListInfo *info = primaryTemplate
		? findPrimaryCommandCenterBuildInfo(primaryTemplate) : nullptr;
	const Bool trackedScaffold = liveTracked && tracked &&
		tracked->isKindOf(KINDOF_COMMANDCENTER) &&
		tracked->testStatus(OBJECT_STATUS_UNDER_CONSTRUCTION) &&
		tracked->getTemplate() && primaryTemplate &&
		tracked->getTemplate()->isEquivalentTo(primaryTemplate);
	const Bool trackedHole = liveTracked && IsSkirmishAIRecoveryPrimaryHole(
		tracked, m_player, primaryTemplate, info, m_recoveryConstructionID);
	const Bool preserveTracked =
		ShouldPreserveSkirmishAIRecoveryTrackedObject(
			permanent, liveTracked, trackedScaffold, trackedHole);
	// Hunt deliberately searches enemies without line-of-sight checks. Use a
	// known objective and ordinary attack-move acquisition for the last stand.
	Coord3D target;
	if (!getKnownEnemyPosition(getAiEnemy(), &target)) {
		// Objective discovery can lag the recovery decision on a fogged map.
		// Keep this state retryable instead of permanently abandoning the
		// surviving forces without issuing any command.
		if (!preserveTracked)
			m_recoveryConstructionID = INVALID_ID;
		if (!permanent)
			m_recoveryImpossible = false;
		m_recoveryReserveCost = 0;
		m_recoveryNextAttemptFrame = GetSkirmishAIRecoveryRetryFrame(
			TheGameLogic->getFrame(), 2 * LOGICFRAMES_PER_SECOND);
		return;
	}
	m_recoveryImpossible = permanent;
	if (!preserveTracked)
		m_recoveryConstructionID = INVALID_ID;
	m_recoveryReserveCost = 0;
	m_recoveryNextAttemptFrame = GetSkirmishAIRecoveryRetryFrame(
		TheGameLogic->getFrame(), 2 * LOGICFRAMES_PER_SECOND);
	for (Object *object = TheGameLogic->getFirstObject(); object;
		object = object->getNextObject()) {
		const Bool currentCandidate = IsSkirmishAIRecoveryLastStandCombatCandidate(
			IsLiveSkirmishAIRecoveryObject(object, m_player),
			object->isKindOf(KINDOF_IMMOBILE),
			object->isKindOf(KINDOF_DOZER),
			object->isKindOf(KINDOF_HARVESTER), object->isAbleToAttack());
		const Bool legacyCandidate = IsLiveSkirmishAIRecoveryObject(object, m_player) &&
			!object->isKindOf(KINDOF_IMMOBILE) && object->isAbleToAttack();
		if (ShouldUseCurrentSkirmishAIStrategyControllerBehavior()
				? !currentCandidate : !legacyCandidate)
			continue;
		AIUpdateInterface *ai = object->getAIUpdateInterface();
		if (ai) {
			if (maintenance && (ai->isAttacking() || ai->isAttackPath()))
				continue;
			// Sleep rejects ordinary AI commands before the attack-action mood
			// policy is consulted.  Normalize either blocker, then use the legal
			// wake command before issuing the fair known-position attack move.
			const UnsignedInt mood = ai->getMoodMatrixValue();
			if ((mood & MM_Controller_AI) &&
				((mood & MM_Mood_Sleep) ||
				 (ai->getMoodMatrixActionAdjustment(MM_Action_Attack) &
				  MAA_Action_Ok) == 0))
				ai->setAttitude(ATTITUDE_NORMAL);
			ai->aiMoveToPositionEvenIfSleeping(
				object->getPosition(), CMD_FROM_AI);
			ai->aiAttackMoveToPosition(&target, NO_MAX_SHOTS_LIMIT, CMD_FROM_AI);
		}
	}
	if (TheGlobalData->m_debugAI)
		TheScriptEngine->AppendDebugMessage(
			permanent
				? "Critical recovery impossible; surviving forces attack a known objective."
				: "Critical recovery route blocked; surviving forces attack while recovery rescans.",
			false);
}

void AISkirmishPlayer::updateCriticalRecovery()
{
	if (!usesCriticalRecoveryBehavior()) {
		m_recoveryReserveCost = 0;
		m_recoveryEvacuationDeadline = 0;
		m_recoveryBuilderFailoverConsumed =
			ReconcileSkirmishAIRecoveryBuilderFailoverConsumed(
				m_recoveryBuilderFailoverConsumed, false, false);
		clearRecoveryBuilderProduction();
		return;
	}
	validateRecoveryBuilderProduction();
	const UnsignedInt frame = TheGameLogic->getFrame();
	if (ShouldReleaseSkirmishAIRecoveryReserveForExpiredGrace(
			IsSkirmishAIRecoveryBoundedGraceExpired(
				frame, m_recoveryEvacuationDeadline),
			m_recoveryNextAttemptFrame == m_recoveryEvacuationDeadline))
		m_recoveryReserveCost = 0;
	if (m_recoveryImpossible) {
		m_recoveryReserveCost = 0;
		m_recoveryEvacuationDeadline = 0;
		if (IsSkirmishAIRecoveryRetryDue(frame, m_recoveryNextAttemptFrame)) {
			enterRecoveryLastStand(true);
		}
		return;
	}
	const ObjectID priorConstructionID = m_recoveryConstructionID;
	if (IsSkirmishAIRecoveryRetryDue(frame, m_recoveryNextAttemptFrame))
		m_recoveryNextAttemptFrame = 0;
	if (!IsSkirmishAIRecoveryRetryDue(frame, m_recoveryNextAttemptFrame)) {
		// A tracked center lets us cheaply notice destruction/capture while the
		// periodic recovery evaluation is asleep.  Missing targets retain their
		// bounded retry deadline without rescanning every frame.
		if (m_recoveryConstructionID != INVALID_ID) {
			Object *tracked = TheGameLogic->findObjectByID(m_recoveryConstructionID);
			if (IsLiveSkirmishAIRecoveryObject(tracked, m_player))
				return;
			m_recoveryConstructionID = INVALID_ID;
		}
		else
			return;
	}

	const PlayerTemplate *playerTemplate = m_player ? m_player->getPlayerTemplate() : nullptr;
	AsciiString startingBuilding = playerTemplate
		? playerTemplate->getStartingBuilding() : AsciiString::TheEmptyString;
	const ThingTemplate *primaryTemplate = startingBuilding.isEmpty()
		? nullptr : TheThingFactory->findTemplate(startingBuilding);
	if (!primaryTemplate || !primaryTemplate->isKindOf(KINDOF_COMMANDCENTER)) {
		m_recoveryReserveCost = 0;
		m_recoveryNextAttemptFrame = GetSkirmishAIRecoveryRetryFrame(
			frame, 5 * LOGICFRAMES_PER_SECOND);
		return;
	}

	BuildListInfo *info = findPrimaryCommandCenterBuildInfo(primaryTemplate);
	Object *center = nullptr;
	const Bool hasCenter = findPrimaryCommandCenter(primaryTemplate, &center);
	normalizeRecoveryWorkOrders(primaryTemplate);
	Bool hasConstruction = false;
	if (hasCenter && center->testStatus(OBJECT_STATUS_UNDER_CONSTRUCTION)) {
		hasConstruction = true;
		// A captured incomplete center must not become a completed recovery
		// history entry.  Only our own recovery construction receives an ID.
		if (m_recoveryEverCompleted)
			m_recoveryConstructionID = center->getID();
	} else if (hasCenter) {
		m_recoveryEverCompleted = true;
		m_recoveryImpossible = false;
		m_recoveryEvacuationDeadline = 0;
		m_recoveryBuilderFailoverConsumed =
			ReconcileSkirmishAIRecoveryBuilderFailoverConsumed(
				m_recoveryBuilderFailoverConsumed, true, true);
		m_recoveryConstructionID = center->getID();
		m_recoveryLocation = *center->getPosition();
		m_recoveryAngle = center->getOrientation();
	}

	// A recovery scaffold remains a live critical objective even after its
	// original builder disappears.  Rebind it through the normal resume command
	// before allowing the ordinary base-building path to observe the scaffold.
	if (hasCenter && hasConstruction && m_recoveryEverCompleted) {
		const Bool useCurrentNativeHoleOwnership =
			ShouldUseCurrentSkirmishAIRecoveryNativeHoleOwnership();
		Object *nativeHole = TheGameLogic->findObjectByID(
			center->getProducerID());
		if (!IsSkirmishAIRecoveryPrimaryHole(
				nativeHole, m_player, primaryTemplate, info, center->getID())) {
			nativeHole = useCurrentNativeHoleOwnership
				? FindSkirmishAIRecoveryHoleForConstruction(
					m_player, primaryTemplate, center->getID())
				: nullptr;
		}
		if (ShouldPreserveSkirmishAIRecoveryNativeHoleLifecycle(
				useCurrentNativeHoleOwnership, nativeHole != nullptr)) {
			// Epoch 3 returned for every producer-linked primary hole before
			// inspecting its assigned worker. Preserve those recorded decisions.
			m_recoveryConstructionID = center->getID();
			m_recoveryEvacuationDeadline = 0;
			m_recoveryReserveCost = 0;
			m_recoveryNextAttemptFrame = GetSkirmishAIRecoveryRetryFrame(
				frame, 2 * LOGICFRAMES_PER_SECOND);
			return;
		}
		if (useCurrentNativeHoleOwnership) {
			// Band 3 belongs to a matching native hole, not to the scaffold by
			// itself. If external state removed the hole, release that exhausted
			// lineage before replacement admission and factory policy run.
			m_recoveryPlacementAttempt =
				ReconcileSkirmishAIRecoveryNativeWorkerRecycleAfterLineageScan(
					m_recoveryPlacementAttempt,
					g_skirmishAIRecoveryOffsetCount, true,
					nativeHole != nullptr);
		}
		Object *assignedBuilder = TheGameLogic->findObjectByID(
			center->getBuilderID());
		Bool assignedLive = IsLiveSkirmishAIRecoveryObject(
			assignedBuilder, m_player);
		AIUpdateInterface *assignedAI = assignedLive
			? assignedBuilder->getAIUpdateInterface() : nullptr;
		DozerAIInterface *assignedDozer = assignedAI
			? assignedAI->getDozerAIInterface() : nullptr;
		Bool assignedOperational =
			IsSkirmishAIRecoveryAssignedBuilderOperational(
				assignedLive, assignedLive && assignedBuilder->isContained(),
				assignedLive && assignedBuilder->isDisabledByType(DISABLED_UNMANNED),
				assignedAI != nullptr, assignedDozer != nullptr,
				assignedLive && assignedAI &&
					CanSkirmishAIRecoveryUpdateAdvance(assignedBuilder, assignedAI));
		Coord3D assignedActionPosition;
		Bool assignedBuildDockFound = assignedOperational &&
			DozerAIUpdate::findGoodBuildOrRepairPosition(
				assignedBuilder, center, assignedActionPosition);
		Bool assignedPathable = assignedBuildDockFound &&
			assignedAI->isPathAvailable(&assignedActionPosition);
		Bool assignedUsable = IsSkirmishAIRecoveryAssignedBuilderUsable(
			assignedOperational, assignedBuildDockFound,
			assignedPathable);
		RebuildHoleBehaviorInterface *primaryNativeHoleAI = nativeHole
			? RebuildHoleBehavior::getRebuildHoleBehaviorInterfaceFromObject(
				nativeHole) : nullptr;
		Object *primaryNativeWorker = primaryNativeHoleAI
			? TheGameLogic->findObjectByID(primaryNativeHoleAI->getWorkerID())
			: nullptr;
		Bool nativeWorkerExists = primaryNativeWorker != nullptr;
		Bool nativeWorkerOwned = nativeWorkerExists &&
			primaryNativeWorker->getControllingPlayer() == m_player;
		Bool nativeWorkerRecycleAttempted =
			HasSkirmishAIRecoveryNativeWorkerRecycleAttempt(
				m_recoveryPlacementAttempt, g_skirmishAIRecoveryOffsetCount);
		// A captured exact worker must be detached before any assigned-builder,
		// grace, or production-progress path can return. Otherwise the hole keeps
		// the foreign ObjectID and can destroy that unit when reconstruction
		// completes or the hole is destroyed.
		if (primaryNativeHoleAI &&
			ShouldDetachSkirmishAIRecoveryNativeWorkerWithoutDestroying(
				nativeWorkerExists, nativeWorkerOwned)) {
			const Bool recycleAttemptedBeforeDetach =
				nativeWorkerRecycleAttempted;
			if (!nativeWorkerRecycleAttempted) {
				m_recoveryPlacementAttempt =
					MarkSkirmishAIRecoveryNativeWorkerRecycleAttempt(
						m_recoveryPlacementAttempt,
						g_skirmishAIRecoveryOffsetCount);
				nativeWorkerRecycleAttempted = true;
			}
			primaryNativeHoleAI->restartRebuildProcessWithoutDestroyingWorker(
				GetSkirmishAIRecoveryExactNativeRebuildTemplate(
					primaryNativeHoleAI->getRebuildTemplate(), primaryTemplate),
				primaryNativeHoleAI->getSpawnerID());
			primaryNativeWorker = nullptr;
			nativeWorkerExists = false;
			nativeWorkerOwned = false;
			const SkirmishAINativeCapturedWorkerTerminalTransition
				capturedTransition =
					GetSkirmishAINativeCapturedWorkerTerminalTransition(
						true, recycleAttemptedBeforeDetach);
			if (capturedTransition.abandonImmediately) {
				// A second captured native worker proves this lineage cannot supply a
				// stable owned finisher. Teardown is immediate so assigned, grace, and
				// production routes cannot preserve another capture/respawn cycle.
				cancelRecoveryBuilderQueueForNativeRespawn(primaryTemplate);
				center->setProducer(nullptr);
				center->clearStatus(
					MAKE_OBJECT_STATUS_MASK(OBJECT_STATUS_RECONSTRUCTING));
				TheGameLogic->destroyObject(nativeHole);
				TheGameLogic->destroyObject(center);
				if (info) {
					info->setObjectID(INVALID_ID);
					info->setObjectTimestamp(frame + 1);
					info->setUnderConstruction(false);
				}
				m_recoveryConstructionID = INVALID_ID;
				m_recoveryEvacuationDeadline = 0;
				m_recoveryPlacementAttempt =
					ClearSkirmishAIRecoveryNativeWorkerRecycleAttempt(
						m_recoveryPlacementAttempt,
						g_skirmishAIRecoveryOffsetCount);
				m_recoveryReserveCost = max(1,
					primaryTemplate->calcCostToBuild(m_player));
				m_recoveryNextAttemptFrame = GetSkirmishAIRecoveryRetryFrame(
					frame, 2 * LOGICFRAMES_PER_SECOND);
				return;
			}
		}
		const Bool nativeWorkerLive = IsLiveSkirmishAIRecoveryObject(
			primaryNativeWorker, m_player);
		const Bool nativeWorkerIsDozer = nativeWorkerExists &&
			primaryNativeWorker->isKindOf(KINDOF_DOZER);
		const Bool nativeWorkerCompatible = nativeWorkerExists &&
			HasSkirmishAICommandForTemplate(primaryNativeWorker, primaryTemplate);
		AIUpdateInterface *nativeWorkerAI = nativeWorkerLive
			? primaryNativeWorker->getAIUpdateInterface() : nullptr;
		DozerAIInterface *nativeWorkerDozer = nativeWorkerAI
			? nativeWorkerAI->getDozerAIInterface() : nullptr;
		const Bool nativeWorkerOperational = nativeWorkerLive &&
			!primaryNativeWorker->isContained() &&
			!primaryNativeWorker->isDisabledByType(DISABLED_UNMANNED) &&
			nativeWorkerAI && nativeWorkerDozer &&
			CanSkirmishAIRecoveryUpdateAdvance(
				primaryNativeWorker, nativeWorkerAI);
		const Bool nativeWorkerActivelyBuilding =
			IsSkirmishAIRecoveryNativeWorkerActivelyBuilding(
				nativeWorkerLive, nativeWorkerOperational,
				nativeWorkerDozer &&
					nativeWorkerDozer->getCurrentTask() == DOZER_TASK_BUILD,
				nativeWorkerDozer &&
					nativeWorkerDozer->getBuildSubTask() == DOZER_DO_BUILD_AT_DOCK,
				nativeWorkerDozer &&
					nativeWorkerDozer->getTaskTarget(DOZER_TASK_BUILD) == center->getID());
		Coord3D nativeWorkerActionPosition;
		const Bool nativeWorkerBuildDockFound = nativeWorkerOperational &&
			DozerAIUpdate::findGoodBuildOrRepairPosition(
				primaryNativeWorker, center, nativeWorkerActionPosition);
		const Bool nativeWorkerPathUsable = nativeWorkerBuildDockFound &&
			nativeWorkerAI->isPathAvailable(&nativeWorkerActionPosition);
		const Bool nativeWorkerResumeUsable = nativeWorkerActivelyBuilding ||
			nativeWorkerPathUsable;
		const Bool preserveNativeWorkerLifecycle =
			ShouldPreserveSkirmishAIRecoveryNativeWorkerLifecycle(
				nativeHole != nullptr, nativeWorkerExists, nativeWorkerLive,
				nativeWorkerIsDozer, nativeWorkerCompatible,
				nativeWorkerResumeUsable);
		const Bool hasPresentUnusableNativeWorker = nativeHole != nullptr &&
			nativeWorkerExists && !preserveNativeWorkerLifecycle;
		const Bool nativeWorkerFailureNeedsGrace =
			hasPresentUnusableNativeWorker && !nativeWorkerRecycleAttempted;
		if (preserveNativeWorkerLifecycle)
			cancelRecoveryBuilderQueueForNativeRespawn(primaryTemplate);
		if (nativeWorkerActivelyBuilding) {
			m_recoveryConstructionID = center->getID();
			m_recoveryEvacuationDeadline = 0;
			m_recoveryReserveCost = 0;
			m_recoveryNextAttemptFrame = GetSkirmishAIRecoveryRetryFrame(
				frame, 2 * LOGICFRAMES_PER_SECOND);
			return;
		}
		if (ShouldUseSkirmishAIRecoveryNativeWorkerAsAssigned(
				assignedUsable, nativeHole != nullptr,
				nativeWorkerLive,
				nativeWorkerIsDozer && nativeWorkerCompatible &&
					nativeWorkerResumeUsable)) {
			// A replacement can take the construction binding and cause the hole's
			// exact worker to finish its old task while remaining alive. If that
			// replacement later dies, resume through the dedicated assigned path.
			assignedBuilder = primaryNativeWorker;
			assignedLive = true;
			assignedAI = nativeWorkerAI;
			assignedDozer = assignedAI
				? assignedAI->getDozerAIInterface() : nullptr;
			assignedOperational =
				IsSkirmishAIRecoveryAssignedBuilderOperational(
					assignedLive, assignedLive && assignedBuilder->isContained(),
					assignedLive && assignedBuilder->isDisabledByType(DISABLED_UNMANNED),
					assignedAI != nullptr, assignedDozer != nullptr,
					assignedLive && assignedAI &&
						CanSkirmishAIRecoveryUpdateAdvance(assignedBuilder, assignedAI));
			assignedBuildDockFound = assignedOperational &&
				DozerAIUpdate::findGoodBuildOrRepairPosition(
					assignedBuilder, center, assignedActionPosition);
			assignedPathable = assignedBuildDockFound &&
				assignedAI->isPathAvailable(&assignedActionPosition);
			assignedUsable = IsSkirmishAIRecoveryAssignedBuilderUsable(
				assignedOperational, assignedBuildDockFound,
				assignedPathable);
		}
		if (preserveNativeWorkerLifecycle && !nativeWorkerExists &&
			!assignedUsable) {
			// The hole owns the free worker and native isRebuild construction;
			// preserve its respawn gap and do not retain a paid duplicate. Exact
			// production provenance ensures unrelated compatible queues survive.
			m_recoveryConstructionID = center->getID();
			m_recoveryEvacuationDeadline = 0;
			m_recoveryReserveCost = 0;
			m_recoveryNextAttemptFrame = GetSkirmishAIRecoveryRetryFrame(
				frame, 2 * LOGICFRAMES_PER_SECOND);
			return;
		}
		m_recoveryConstructionID = center->getID();
		// A dead stale binding cannot resume this scaffold.  Keep a live binding
		// intact until a distinct replacement has passed every admission and path
		// check, so the native-hole retry remains aware of its blocked worker.
		if (!assignedLive)
			center->setBuilder(nullptr);
		if (assignedUsable) {
			if (assignedPathable &&
				(assignedDozer->getCurrentTask() != DOZER_TASK_BUILD ||
				 assignedDozer->getTaskTarget(DOZER_TASK_BUILD) != center->getID()) &&
				prepareCriticalRecoveryBuilder(assignedBuilder))
				assignedAI->aiResumeConstruction(center, CMD_FROM_AI);
			if (assignedPathable &&
				assignedDozer->isTaskPending(DOZER_TASK_BUILD) &&
				assignedDozer->getTaskTarget(DOZER_TASK_BUILD) == center->getID()) {
				m_recoveryEvacuationDeadline = 0;
				m_recoveryReserveCost = 0;
				m_recoveryNextAttemptFrame = GetSkirmishAIRecoveryRetryFrame(
					frame, 2 * LOGICFRAMES_PER_SECOND);
				return;
			}
			// A live assigned builder or pending task does not prove that this
			// scaffold is reachable.  Let the bounded replacement pass consider
			// every compatible builder before retrying the assigned one later.
		}
		const Bool hasContainedBuilder =
			RecoverSkirmishAIContainedBuilders(m_player, primaryTemplate);

		std::vector<Object *> replacementBuilders;
		CollectSkirmishAIRecoveryBuilders(
			m_player, center->getPosition(), primaryTemplate,
			&replacementBuilders);
		for (std::vector<Object *>::iterator replacement =
			replacementBuilders.begin(); replacement != replacementBuilders.end();
			++replacement) {
			Object *replacementBuilder = *replacement;
			if (replacementBuilder == assignedBuilder)
				continue;
			AIUpdateInterface *replacementAI =
				replacementBuilder->getAIUpdateInterface();
			if (!CanSkirmishAIRecoveryUpdateAdvance(
					replacementBuilder, replacementAI))
				continue;
			Coord3D actionPosition;
			const Bool buildDockFound =
				DozerAIUpdate::findGoodBuildOrRepairPosition(
					replacementBuilder, center, actionPosition);
			if (!replacementAI || !buildDockFound ||
				!replacementAI->isPathAvailable(&actionPosition))
				continue;
			if (prepareCriticalRecoveryBuilder(replacementBuilder)) {
				Object *priorLiveBuilder = assignedLive ? assignedBuilder : nullptr;
				center->setBuilder(nullptr);
				replacementAI->aiResumeConstruction(center, CMD_FROM_AI);
				DozerAIInterface *replacementDozer = replacementAI
					? replacementAI->getDozerAIInterface() : nullptr;
				const Bool replacementTaskEstablished = replacementDozer &&
					replacementDozer->isTaskPending(DOZER_TASK_BUILD) &&
					replacementDozer->getTaskTarget(DOZER_TASK_BUILD) == center->getID();
				if (replacementTaskEstablished) {
					m_recoveryEvacuationDeadline = 0;
					m_recoveryReserveCost = 0;
					m_recoveryNextAttemptFrame = GetSkirmishAIRecoveryRetryFrame(
						frame, 2 * LOGICFRAMES_PER_SECOND);
					return;
				}
				// The resume command did not establish ownership.  Preserve the
				// original live binding so the next retry cannot mistake a blocked
				// native worker for an absent worker in its hole's respawn gap.
				if (ShouldRestoreSkirmishAIRecoveryScaffoldBuilder(
						IsLiveSkirmishAIRecoveryObject(priorLiveBuilder, m_player),
						replacementTaskEstablished))
					center->setBuilder(priorLiveBuilder);
			}
		}
		Bool builderQueuedPaid = false;
		ObjectID queuedFactoryID = INVALID_ID;
		ProductionID queuedProductionID = PRODUCTIONID_INVALID;
		const Bool builderQueued = hasRecoveryBuilderQueued(
			primaryTemplate, &builderQueuedPaid, &queuedFactoryID,
			&queuedProductionID);
		Object *queuedFactory = builderQueuedPaid && queuedFactoryID != INVALID_ID
			? TheGameLogic->findObjectByID(queuedFactoryID) : nullptr;
		ProductionUpdateInterface *queuedProduction = queuedFactory
			? queuedFactory->getProductionUpdateInterface() : nullptr;
		const Bool queuedProductionCanAdvance =
			CanSkirmishAIRecoveryProductionAdvance(queuedFactory, queuedProduction);
		const Bool paidQueueBounded = IsSkirmishAIRecoveryPaidQueueBounded(
			builderQueuedPaid,
			IsLiveSkirmishAIRecoveryObject(queuedFactory, m_player),
			queuedProductionCanAdvance);
		const Bool paidQueueProgressing = builderQueuedPaid && !paidQueueBounded;
		const ThingTemplate *replacementTemplate = nullptr;
		Object *replacementFactory = nullptr;
		Bool hasPotentialFactory = false;
		Bool hasBoundedFactory = false;
		findRecoveryBuilderTemplateAndFactory(
			primaryTemplate, &replacementTemplate, &replacementFactory,
			&hasPotentialFactory, &hasBoundedFactory);
		if (paidQueueProgressing)
			m_recoveryEvacuationDeadline = 0;
		const Bool hasFailedResumeRoute = hasContainedBuilder ||
			!replacementBuilders.empty();
		const Bool hasBoundedResumeRoute = nativeWorkerFailureNeedsGrace ||
			paidQueueBounded ||
			(!builderQueuedPaid &&
			 (hasFailedResumeRoute ||
			  (hasBoundedFactory && !hasPotentialFactory)));
		m_recoveryEvacuationDeadline = GetSkirmishAIRecoveryEvacuationDeadline(
			frame, m_recoveryEvacuationDeadline, hasBoundedResumeRoute,
			2 * LOGICFRAMES_PER_SECOND);
		const Bool resumeGraceActive =
			IsSkirmishAIRecoveryEvacuationGraceActive(
				frame, m_recoveryEvacuationDeadline, hasBoundedResumeRoute);
		const Bool factoryPotential =
			IsSkirmishAIRecoveryFactoryPotentialDuringGrace(
				hasPotentialFactory, hasBoundedFactory,
				resumeGraceActive);
		const Int replacementCost = replacementTemplate
			? replacementTemplate->calcCostToBuild(m_player) : 0;
		if (builderQueuedPaid)
			m_recoveryPlacementAttempt =
				MarkSkirmishAIRecoveryScaffoldReplacementAttempt(
					m_recoveryPlacementAttempt,
					g_skirmishAIRecoveryOffsetCount);
		bindRecoveryBuilderProductionIfNeeded(
			false, builderQueuedPaid, queuedFactoryID, queuedProductionID);
		m_recoveryPlacementAttempt = ReconcileSkirmishAIRecoveryReplacementAttempt(
			m_recoveryPlacementAttempt, g_skirmishAIRecoveryOffsetCount,
			builderQueuedPaid);
		const Bool replacementAttempted =
			HasSkirmishAIRecoveryScaffoldReplacementAttempt(
				m_recoveryPlacementAttempt,
				g_skirmishAIRecoveryOffsetCount);
		if (resumeGraceActive) {
			// A compatible builder may be waiting on a transient topology or
			// cancellation transition.  Bound that grace while protecting the
			// cost of the one paid fallback builder.
			m_recoveryReserveCost = replacementCost > 0 ? replacementCost : 0;
			m_recoveryNextAttemptFrame = m_recoveryEvacuationDeadline;
			return;
		}
		if (paidQueueBounded) {
			const Bool replacementObserved =
				HasSkirmishAIRecoveryObservedReplacement(
					m_recoveryPlacementAttempt,
					g_skirmishAIRecoveryOffsetCount);
			const Bool failoverSucceeded =
				ShouldSearchSkirmishAIRecoveryPaidQueueFailover(
					paidQueueBounded, resumeGraceActive, replacementObserved,
					ShouldUseCurrentSkirmishAIRecoveryBoundedFailover(),
					m_recoveryBuilderFailoverConsumed) &&
				failoverRecoveryBuilderQueue(
					primaryTemplate, queuedFactory, queuedProductionID);
			if (failoverSucceeded) {
				m_recoveryEvacuationDeadline = 0;
				m_recoveryReserveCost = replacementCost > 0 ? replacementCost : 0;
				m_recoveryNextAttemptFrame = GetSkirmishAIRecoveryRetryFrame(
					frame, 2 * LOGICFRAMES_PER_SECOND);
				return;
			}
			if (ShouldReturnFromSkirmishAIRecoveryBoundedQueueFailure(
					paidQueueBounded, failoverSucceeded,
					hasPresentUnusableNativeWorker)) {
				// The paid entry remains authoritative, but a producer whose
				// ProductionUpdate cannot run cannot hold the recovery reserve forever.
				// Keep both the entry and scaffold, release the reserve through the
				// retryable last stand, and rescan the same producer later.
				enterRecoveryLastStand(false);
				return;
			}
			// The bounded queue did not advance or fail over, and the exact native
			// worker cannot enter respawn while it still resolves. Preserve the paid
			// entry and continue to the one-shot native-worker reset below; the next
			// absent-worker pass cancels and refunds that exact tracked production.
		}
		if (!replacementAttempted &&
			(!builderQueued || !builderQueuedPaid) && replacementFactory &&
			replacementTemplate && replacementCost >= 0 &&
			m_player->getMoney()->countMoney() >= replacementCost &&
			TheBuildAssistant->canMakeUnit(
				replacementFactory, replacementTemplate) == CANMAKE_OK &&
			queueRecoveryBuilder(replacementTemplate, replacementFactory)) {
			m_recoveryPlacementAttempt =
				MarkSkirmishAIRecoveryScaffoldReplacementAttempt(
					m_recoveryPlacementAttempt,
					g_skirmishAIRecoveryOffsetCount);
			m_recoveryEvacuationDeadline = 0;
			m_recoveryReserveCost = replacementCost;
			m_recoveryNextAttemptFrame = GetSkirmishAIRecoveryRetryFrame(
				frame, 2 * LOGICFRAMES_PER_SECOND);
			return;
		}
		// Do not dispose while paid production advances. Factory potential remains
		// retryable for other states, but cannot mask the free bounded reset of a
		// present unusable exact native worker. Grace and successful queue/failover
		// actions already returned above.
		if (ShouldDeferSkirmishAIRecoveryStalledDisposition(
				paidQueueProgressing, factoryPotential,
				ShouldUseCurrentSkirmishAIRecoveryCancellationOwnership() &&
					replacementAttempted,
				hasPresentUnusableNativeWorker)) {
			m_recoveryEvacuationDeadline = 0;
			m_recoveryReserveCost = replacementCost > 0 ? replacementCost : 0;
			m_recoveryNextAttemptFrame = GetSkirmishAIRecoveryRetryFrame(
				frame, 2 * LOGICFRAMES_PER_SECOND);
			return;
		}
		const Bool shouldSellScaffold = ShouldSellSkirmishAIRecoveryScaffold(
			!replacementBuilders.empty(), resumeGraceActive,
			paidQueueProgressing, replacementAttempted,
			factoryPotential);
		RebuildHoleBehaviorInterface *nativeHoleAI = primaryNativeHoleAI;
		const Bool stalledNativeWorkerExists = nativeWorkerExists;
		const Bool stalledNativeWorkerUsable = nativeWorkerExists &&
			preserveNativeWorkerLifecycle;
		const SkirmishAIRecoveryStalledScaffoldAction scaffoldAction =
			GetSkirmishAIRecoveryStalledScaffoldAction(
				shouldSellScaffold, nativeHole != nullptr,
				stalledNativeWorkerExists, stalledNativeWorkerUsable,
				nativeWorkerRecycleAttempted);
		if (scaffoldAction ==
				SKIRMISH_AI_RECOVERY_SCAFFOLD_ABANDON_NATIVE_LINEAGE) {
			// The one persisted canonical recycle still produced a worker that cannot
			// resume this site. Remove both native owners before ordinary recovery
			// relocates, so no hole can later create a duplicate center.
			cancelRecoveryBuilderQueueForNativeRespawn(primaryTemplate);
			if (nativeHoleAI && nativeHoleAI->getRebuildTemplate())
				nativeHoleAI->restartRebuildProcessWithoutDestroyingWorker(
					nativeHoleAI->getRebuildTemplate(),
					nativeHoleAI->getSpawnerID());
			center->setProducer(nullptr);
			center->clearStatus(
				MAKE_OBJECT_STATUS_MASK(OBJECT_STATUS_RECONSTRUCTING));
			if (nativeHole)
				TheGameLogic->destroyObject(nativeHole);
			TheGameLogic->destroyObject(center);
			if (info) {
				info->setObjectID(INVALID_ID);
				info->setObjectTimestamp(frame + 1);
				info->setUnderConstruction(false);
			}
			m_recoveryConstructionID = INVALID_ID;
			m_recoveryEvacuationDeadline = 0;
			m_recoveryPlacementAttempt =
				ClearSkirmishAIRecoveryNativeWorkerRecycleAttempt(
					m_recoveryPlacementAttempt,
					g_skirmishAIRecoveryOffsetCount);
			m_recoveryReserveCost = max(1,
				primaryTemplate->calcCostToBuild(m_player));
			m_recoveryNextAttemptFrame = GetSkirmishAIRecoveryRetryFrame(
				frame, 2 * LOGICFRAMES_PER_SECOND);
			return;
		}
		if (scaffoldAction ==
				SKIRMISH_AI_RECOVERY_SCAFFOLD_RECYCLE_NATIVE_WORKER ||
			scaffoldAction ==
				SKIRMISH_AI_RECOVERY_SCAFFOLD_RESET_UNUSABLE_NATIVE_WORKER) {
			// A native rebuild hole owns both this site and its free worker.  Selling
			// the scaffold would sever the reconstruction ID before the hole can
			// observe the loss, allowing paid recovery to build a second center. An
			// exact worker that still exists but is unusable cannot trigger the hole's
			// null-ID respawn path, so reset it once through the canonical cycle too.
			if (scaffoldAction ==
					SKIRMISH_AI_RECOVERY_SCAFFOLD_RESET_UNUSABLE_NATIVE_WORKER ||
				scaffoldAction ==
					SKIRMISH_AI_RECOVERY_SCAFFOLD_RECYCLE_NATIVE_WORKER) {
				cancelRecoveryBuilderQueueForNativeRespawn(primaryTemplate);
				m_recoveryPlacementAttempt =
					MarkSkirmishAIRecoveryNativeWorkerRecycleAttempt(
						m_recoveryPlacementAttempt,
						g_skirmishAIRecoveryOffsetCount);
			}
			if (nativeHoleAI && nativeHoleAI->getRebuildTemplate()) {
				if (scaffoldAction ==
						SKIRMISH_AI_RECOVERY_SCAFFOLD_RESET_UNUSABLE_NATIVE_WORKER &&
					ShouldDetachSkirmishAIRecoveryNativeWorkerWithoutDestroying(
						nativeWorkerExists, nativeWorkerOwned))
					nativeHoleAI->restartRebuildProcessWithoutDestroyingWorker(
						nativeHoleAI->getRebuildTemplate(),
						nativeHoleAI->getSpawnerID());
				else
					nativeHoleAI->startRebuildProcess(
						nativeHoleAI->getRebuildTemplate(),
						nativeHoleAI->getSpawnerID());
			}
			m_recoveryEvacuationDeadline = 0;
			m_recoveryReserveCost = 0;
			m_recoveryNextAttemptFrame = GetSkirmishAIRecoveryRetryFrame(
				frame, 2 * LOGICFRAMES_PER_SECOND);
			return;
		}
		if (shouldSellScaffold && nativeHole &&
			scaffoldAction == SKIRMISH_AI_RECOVERY_SCAFFOLD_KEEP) {
			// The native hole already owns a respawn countdown.  Preserve the
			// scaffold and lineage without restarting that countdown on each AI retry.
			m_recoveryEvacuationDeadline = 0;
			m_recoveryReserveCost = 0;
			m_recoveryNextAttemptFrame = GetSkirmishAIRecoveryRetryFrame(
				frame, 2 * LOGICFRAMES_PER_SECOND);
			return;
		}
		if (scaffoldAction == SKIRMISH_AI_RECOVERY_SCAFFOLD_SELL) {
			// A surviving builder from another terrain zone cannot finish this
			// paid scaffold, and one exhausted replacement is enough evidence
			// not to buy an unbounded stream for the same site.  Relocate through
			// the ordinary path-checked recovery placement.
			TheBuildAssistant->sellObject(center);
			if (info) {
				info->setObjectID(INVALID_ID);
				info->setObjectTimestamp(frame + 1);
				info->setUnderConstruction(false);
			}
			m_recoveryConstructionID = INVALID_ID;
			m_recoveryEvacuationDeadline = 0;
			const Int commandCenterCost =
				primaryTemplate->calcCostToBuild(m_player);
			m_recoveryReserveCost = max(1, commandCenterCost);
			m_recoveryNextAttemptFrame = GetSkirmishAIRecoveryRetryFrame(
				frame, 2 * LOGICFRAMES_PER_SECOND);
			return;
		}
		enterRecoveryLastStand(
			ShouldLatchSkirmishAIRecoveryLastStand(hasBoundedResumeRoute));
		return;
	}

	if (m_recoveryConstructionID != INVALID_ID && !hasCenter) {
		Object *tracked = TheGameLogic->findObjectByID(m_recoveryConstructionID);
		if (IsSkirmishAIRecoveryPrimaryHole(
			tracked, m_player, primaryTemplate, info, m_recoveryConstructionID)) {
			m_recoveryReserveCost = 0;
			m_recoveryNextAttemptFrame = GetSkirmishAIRecoveryRetryFrame(
				frame, 2 * LOGICFRAMES_PER_SECOND);
			return;
		}
		if (!IsLiveSkirmishAIRecoveryObject(tracked, m_player) ||
			!tracked->isKindOf(KINDOF_COMMANDCENTER) || !tracked->getTemplate() ||
			!tracked->getTemplate()->isEquivalentTo(primaryTemplate) ||
			!tracked->testStatus(OBJECT_STATUS_UNDER_CONSTRUCTION))
			m_recoveryConstructionID = INVALID_ID;
		else
			hasConstruction = true;
	}

	const Int protectedReserve = max(0, TheAI->getAiData()->m_resourcesPoor);
	m_recoveryReserveCost = 0;

	// GLA's rebuild-hole behavior owns its own free worker and construction
	// object.  Scan the holes directly because the build-list object ID still
	// names the destroyed center during the first frame of the transition.
	// Treat only a hole for this player's original center as pending so a
	// captured enemy hole cannot freeze paid recovery.
	if (!hasCenter) {
		for (Object *hole = TheGameLogic->getFirstObject(); hole;
			hole = hole->getNextObject()) {
			if (!IsLiveSkirmishAIRecoveryObject(hole, m_player) ||
				!hole->isKindOf(KINDOF_REBUILD_HOLE))
				continue;
			RebuildHoleBehaviorInterface *holeAI =
				RebuildHoleBehavior::getRebuildHoleBehaviorInterfaceFromObject(hole);
			if (!holeAI || !holeAI->getRebuildTemplate() ||
				!holeAI->getRebuildTemplate()->isEquivalentTo(primaryTemplate))
				continue;
			const Bool trackedByBuildList = info && info->getObjectID() == hole->getID();
			const Bool trackedByRecoveryHole =
				m_recoveryConstructionID == hole->getID();
			const Bool trackedBySpawner = priorConstructionID != INVALID_ID &&
				holeAI->getSpawnerID() == priorConstructionID;
			const Bool trackedByReconstruction = priorConstructionID != INVALID_ID &&
				holeAI->getReconstructedBuildingID() == priorConstructionID;
			if (!trackedByBuildList && !trackedByRecoveryHole &&
				!trackedBySpawner && !trackedByReconstruction)
				continue;
			if (info && !trackedByBuildList)
			info->setObjectID(hole->getID());
			ObjectID reconstructionID = holeAI->getReconstructedBuildingID();
			m_recoveryConstructionID = reconstructionID != INVALID_ID
				? reconstructionID : hole->getID();
			m_recoveryReserveCost = 0;
			m_recoveryNextAttemptFrame = GetSkirmishAIRecoveryRetryFrame(
				frame, 2 * LOGICFRAMES_PER_SECOND);
			return;
		}
	}
	if (!hasCenter && !hasConstruction) {
		// Once both the primary scaffold and its matching native hole are gone,
		// the persisted one-recycle budget belongs to a dead lineage. Retain the
		// lower placement/replacement state while allowing ordinary factory
		// recovery to proceed.
		m_recoveryPlacementAttempt =
			ReconcileSkirmishAIRecoveryNativeWorkerRecycleAfterLineageScan(
				m_recoveryPlacementAttempt,
				g_skirmishAIRecoveryOffsetCount, false, false);
	}

	if (hasCenter) {
		if (!m_recoveryEverCompleted || m_recoveryImpossible || hasConstruction)
		{
			m_recoveryNextAttemptFrame = GetSkirmishAIRecoveryRetryFrame(
				frame, 2 * LOGICFRAMES_PER_SECOND);
			return;
		}

		Int builderCount = 0;
		for (Object *object = TheGameLogic->getFirstObject(); object;
			object = object->getNextObject()) {
			if (IsLiveSkirmishAIRecoveryObject(object, m_player) &&
				IsSkirmishAIRecoveryInsuranceBuilderCandidate(
					object->isContained(), object->isKindOf(KINDOF_DOZER),
					object->isDisabledByType(DISABLED_UNMANNED)) &&
				object->getAIUpdateInterface() &&
				object->getAIUpdateInterface()->getDozerAIInterface() &&
				HasSkirmishAICommandForTemplate(object, primaryTemplate))
				++builderCount;
		}

		Bool builderQueuedPaid = false;
		ObjectID queuedFactoryID = INVALID_ID;
		const Bool builderQueued = hasRecoveryBuilderQueued(
			primaryTemplate, &builderQueuedPaid, &queuedFactoryID, nullptr);
		const Int desiredBuilders =
			(!hasConstruction && m_player->getMoney()->countMoney() >=
				TheAI->getAiData()->m_resourcesWealthy) ? 2 : 1;
		if (builderCount < desiredBuilders && !builderQueued) {
			const ThingTemplate *builderTemplate = nullptr;
			Object *factory = nullptr;
			Bool hasPotentialFactory = false;
			if (findRecoveryBuilderTemplateAndFactory(
				primaryTemplate,
				&builderTemplate, &factory, &hasPotentialFactory,
				nullptr) && factory && builderTemplate) {
				const Int builderCost = builderTemplate->calcCostToBuild(m_player);
				if (builderCost >= 0 &&
					m_player->getMoney()->countMoney() >=
						AddSkirmishAIRecoveryCost(builderCost, protectedReserve) &&
					TheBuildAssistant->canMakeUnit(factory, builderTemplate) == CANMAKE_OK)
					queueRecoveryBuilder(builderTemplate, factory);
			}
		}
		m_recoveryNextAttemptFrame = GetSkirmishAIRecoveryRetryFrame(
			frame, 5 * LOGICFRAMES_PER_SECOND);
		return;
	}

	// Old snapshots intentionally default to no recovery history.  A live
	// center establishes that history above; without one, do not infer that a
	// captured/incomplete center was ever completed.
	if (!m_recoveryEverCompleted || m_recoveryImpossible) {
		m_recoveryNextAttemptFrame = GetSkirmishAIRecoveryRetryFrame(
			frame, 5 * LOGICFRAMES_PER_SECOND);
		return;
	}
	if (!info) {
		enterRecoveryLastStand(true);
		return;
	}
	if (m_recoveryLocation.x == 0.0f && m_recoveryLocation.y == 0.0f &&
		m_recoveryLocation.z == 0.0f)
		m_recoveryLocation = *info->getLocation();
	if (m_recoveryAngle == 0.0f)
		m_recoveryAngle = info->getAngle();

	Bool builderQueuedPaid = false;
	ObjectID queuedFactoryID = INVALID_ID;
	ProductionID queuedProductionID = PRODUCTIONID_INVALID;
	const Bool builderQueued = hasRecoveryBuilderQueued(
		primaryTemplate, &builderQueuedPaid, &queuedFactoryID,
		&queuedProductionID);
	Object *queuedFactory = builderQueuedPaid && queuedFactoryID != INVALID_ID
		? TheGameLogic->findObjectByID(queuedFactoryID) : nullptr;
	ProductionUpdateInterface *queuedProduction = queuedFactory
		? queuedFactory->getProductionUpdateInterface() : nullptr;
	const Bool queuedProductionCanAdvance =
		CanSkirmishAIRecoveryProductionAdvance(queuedFactory, queuedProduction);
	const Bool paidQueueBounded = IsSkirmishAIRecoveryPaidQueueBounded(
		builderQueuedPaid,
		IsLiveSkirmishAIRecoveryObject(queuedFactory, m_player),
		queuedProductionCanAdvance);
	const Bool paidQueueProgressing = builderQueuedPaid && !paidQueueBounded;
	const ThingTemplate *builderTemplate = nullptr;
	Object *builderFactory = nullptr;
	Bool hasPotentialFactory = false;
	Bool hasBoundedFactory = false;
	findRecoveryBuilderTemplateAndFactory(
		primaryTemplate,
		&builderTemplate, &builderFactory, &hasPotentialFactory,
		&hasBoundedFactory);
	if (builderQueuedPaid)
		m_recoveryPlacementAttempt =
			MarkSkirmishAIRecoveryScaffoldReplacementAttempt(
				m_recoveryPlacementAttempt,
				g_skirmishAIRecoveryOffsetCount);
	bindRecoveryBuilderProductionIfNeeded(
		false, builderQueuedPaid, queuedFactoryID, queuedProductionID);
	std::vector<Object *> constructionBuilders;
	CollectSkirmishAIRecoveryBuilders(
		m_player, &m_recoveryLocation, primaryTemplate,
		&constructionBuilders);
	const Bool hasCompatibleBuilder = !constructionBuilders.empty();
	Bool hasGeographicBuilderRoute = false;
	Bool hasActionableBuilderRoute = false;
	Bool hasBoundedBuilderAdmissionRoute = false;
	for (std::vector<Object *>::iterator candidate =
		constructionBuilders.begin(); candidate != constructionBuilders.end();
		++candidate) {
		if (!hasCriticalRecoveryPlacementRoute(primaryTemplate, *candidate))
			continue;
		hasGeographicBuilderRoute = true;
		AIUpdateInterface *candidateAI = (*candidate)->getAIUpdateInterface();
		const Bool candidateCanAdvance =
			CanSkirmishAIRecoveryUpdateAdvance(*candidate, candidateAI);
		if (IsSkirmishAIRecoveryBuilderUpdateBounded(
				hasGeographicBuilderRoute, candidateCanAdvance)) {
			hasBoundedBuilderAdmissionRoute = true;
			continue;
		}
		const CanMakeType admission =
			TheBuildAssistant->canMakeUnit(*candidate, primaryTemplate);
		if (IsSkirmishAIRecoveryBuilderAdmissionActionable(
			admission == CANMAKE_OK, candidateCanAdvance))
			hasActionableBuilderRoute = true;
		else if (IsSkirmishAIRecoveryAdmissionBounded(
			admission == CANMAKE_NO_MONEY,
			admission == CANMAKE_FACTORY_IS_DISABLED,
			admission == CANMAKE_NO_PREREQ,
			admission == CANMAKE_MAXED_OUT_FOR_PLAYER))
			hasBoundedBuilderAdmissionRoute = true;
	}
	const Bool hasProgressingBuilderRoute = hasActionableBuilderRoute;
	const Bool hasContainedBuilder =
		RecoverSkirmishAIContainedBuilders(m_player, primaryTemplate);
	m_recoveryPlacementAttempt = ReconcileSkirmishAIRecoveryReplacementAttempt(
		m_recoveryPlacementAttempt, g_skirmishAIRecoveryOffsetCount,
		builderQueuedPaid);
	const Bool replacementAttempted =
		HasSkirmishAIRecoveryScaffoldReplacementAttempt(
			m_recoveryPlacementAttempt, g_skirmishAIRecoveryOffsetCount);
	const Bool hasUnreachableBuilderRoute = hasCompatibleBuilder &&
		!hasGeographicBuilderRoute && !paidQueueProgressing;
	const Bool hasBoundedFactoryRoute = hasBoundedFactory &&
		!hasPotentialFactory && !replacementAttempted;
	const Bool hasBoundedRoute = hasContainedBuilder ||
		hasUnreachableBuilderRoute || hasBoundedBuilderAdmissionRoute ||
		hasBoundedFactoryRoute || paidQueueBounded;
	if (hasActionableBuilderRoute ||
		ShouldClearSkirmishAIRecoveryDeadlineForProgressingRoute(
			paidQueueProgressing, hasPotentialFactory))
		m_recoveryEvacuationDeadline = 0;
	else
		m_recoveryEvacuationDeadline = GetSkirmishAIRecoveryEvacuationDeadline(
			frame, m_recoveryEvacuationDeadline, hasBoundedRoute,
			2 * LOGICFRAMES_PER_SECOND);
	const Bool evacuationGraceActive = IsSkirmishAIRecoveryEvacuationGraceActive(
		frame, m_recoveryEvacuationDeadline, hasBoundedRoute);
	const Bool factoryPotential = !replacementAttempted &&
		IsSkirmishAIRecoveryFactoryPotentialDuringGrace(
			hasPotentialFactory, hasBoundedFactory,
			evacuationGraceActive);
	const Bool retryDue = IsSkirmishAIRecoveryRetryDue(
		TheGameLogic->getFrame(), m_recoveryNextAttemptFrame);
	const Int commandCenterCost = primaryTemplate->calcCostToBuild(m_player);
	const Int builderCost = builderTemplate
		? builderTemplate->calcCostToBuild(m_player) : 0;
	const Int money = m_player->getMoney()->countMoney();
	if (!hasProgressingBuilderRoute && evacuationGraceActive) {
		const Int recoveryCost = builderQueuedPaid
			? commandCenterCost
			: AddSkirmishAIRecoveryCost(commandCenterCost, builderCost);
		m_recoveryReserveCost = max(protectedReserve, recoveryCost);
		m_recoveryNextAttemptFrame = m_recoveryEvacuationDeadline;
		return;
	}
	if (paidQueueBounded) {
		const Bool replacementObserved =
			HasSkirmishAIRecoveryObservedReplacement(
				m_recoveryPlacementAttempt, g_skirmishAIRecoveryOffsetCount);
		if (ShouldSearchSkirmishAIRecoveryPaidQueueFailover(
				paidQueueBounded, evacuationGraceActive, replacementObserved,
				ShouldUseCurrentSkirmishAIRecoveryBoundedFailover(),
				m_recoveryBuilderFailoverConsumed) &&
			failoverRecoveryBuilderQueue(
				primaryTemplate, queuedFactory, queuedProductionID)) {
			m_recoveryEvacuationDeadline = 0;
			m_recoveryReserveCost = max(protectedReserve, commandCenterCost);
			m_recoveryNextAttemptFrame = GetSkirmishAIRecoveryRetryFrame(
				frame, 2 * LOGICFRAMES_PER_SECOND);
			return;
		}
	}

	SkirmishAIRecoveryPolicyInput input;
	input.enabled = true;
	input.everCompleted = m_recoveryEverCompleted;
	input.hasPrimaryCommandCenter = false;
	input.hasConstruction = hasConstruction;
	input.hasBuilder = hasProgressingBuilderRoute;
	input.builderQueued = builderQueued;
	input.builderQueuePaid = paidQueueProgressing;
	input.hasBuilderFactory = builderFactory != nullptr && factoryPotential;
	input.noBuilderPath = IsSkirmishAIRecoveryBuilderPathUnavailable(
		hasProgressingBuilderRoute, evacuationGraceActive, paidQueueProgressing,
		factoryPotential);
	input.builderAffordable = builderFactory && builderTemplate &&
		factoryPotential && retryDue &&
		TheBuildAssistant->canMakeUnit(builderFactory, builderTemplate) == CANMAKE_OK &&
		money >= builderCost;
	input.commandCenterAffordable = commandCenterCost >= 0 &&
		money >= commandCenterCost;
	input.placementReady = hasActionableBuilderRoute && retryDue;
	input.commandCenterCost = commandCenterCost;
	input.builderCost = builderCost;
	input.protectedReserve = protectedReserve;
	SkirmishAIRecoveryPolicyResult decision = DecideSkirmishAIRecovery(input);
	m_recoveryReserveCost = decision.reserveCost;

	if (decision.recoveryImpossible) {
		enterRecoveryLastStand(
			ShouldLatchSkirmishAIRecoveryLastStand(hasBoundedRoute));
		return;
	}
	if (decision.shouldQueueBuilder) {
		if (queueRecoveryBuilder(builderTemplate, builderFactory)) {
			m_recoveryPlacementAttempt =
				MarkSkirmishAIRecoveryScaffoldReplacementAttempt(
					m_recoveryPlacementAttempt,
					g_skirmishAIRecoveryOffsetCount);
			m_recoveryEvacuationDeadline = 0;
			m_recoveryNextAttemptFrame = GetSkirmishAIRecoveryRetryFrame(
				TheGameLogic->getFrame(), 2 * LOGICFRAMES_PER_SECOND);
			Bool paid = false;
			ObjectID factoryID = INVALID_ID;
			if (hasRecoveryBuilderQueued(
					primaryTemplate, &paid, &factoryID, nullptr))
				m_recoveryReserveCost = paid
					? max(protectedReserve, commandCenterCost)
					: max(protectedReserve,
						AddSkirmishAIRecoveryCost(commandCenterCost, builderCost));
		}
		else if (m_recoveryNextAttemptFrame == 0) {
			m_recoveryNextAttemptFrame = GetSkirmishAIRecoveryRetryFrame(
				TheGameLogic->getFrame(), 2 * LOGICFRAMES_PER_SECOND);
		}
		return;
	}
	if (decision.shouldConstructCommandCenter) {
		for (std::vector<Object *>::iterator candidate =
			constructionBuilders.begin(); candidate != constructionBuilders.end();
			++candidate) {
			if (tryCriticalCommandCenterConstruction(
				primaryTemplate, info, *candidate))
				return;
		}
		if (m_recoveryNextAttemptFrame == 0)
			m_recoveryNextAttemptFrame = GetSkirmishAIRecoveryRetryFrame(
				TheGameLogic->getFrame(), 2 * LOGICFRAMES_PER_SECOND);
	}
	if (decision.shouldRetry && m_recoveryNextAttemptFrame == 0)
		m_recoveryNextAttemptFrame = GetSkirmishAIRecoveryRetryFrame(
			TheGameLogic->getFrame(), 2 * LOGICFRAMES_PER_SECOND);
}


/**
 * Build our base.
 */
void AISkirmishPlayer::processBaseBuilding()
{
	if (usesProductionBehavior())
		refreshStrategyProductionState();
	if (usesCriticalRecoveryBehavior() && m_recoveryEverCompleted &&
		!m_recoveryImpossible && m_recoveryConstructionID != INVALID_ID &&
		TheGameLogic) {
		Object *construction = TheGameLogic->findObjectByID(
			m_recoveryConstructionID);
		if (IsLiveSkirmishAIRecoveryObject(construction, m_player) &&
			construction->isKindOf(KINDOF_COMMANDCENTER) &&
			construction->testStatus(OBJECT_STATUS_UNDER_CONSTRUCTION))
			return;
		const PlayerTemplate *playerTemplate = m_player->getPlayerTemplate();
		const ThingTemplate *primaryTemplate = playerTemplate
			? TheThingFactory->findTemplate(playerTemplate->getStartingBuilding())
			: nullptr;
		if (IsSkirmishAIRecoveryPrimaryHole(
			construction, m_player, primaryTemplate,
			primaryTemplate ? findPrimaryCommandCenterBuildInfo(primaryTemplate) : nullptr,
			m_recoveryConstructionID))
			return;
	}
	// While a completed primary center is missing, recovery owns all builder
	// and structure spending.  Leave the ordinary build-list path untouched
	// once a center/scaffold exists or recovery has entered last stand.
	if (usesCriticalRecoveryBehavior() && m_recoveryEverCompleted &&
		!m_recoveryImpossible && m_recoveryConstructionID == INVALID_ID &&
		m_recoveryReserveCost > 0)
		return;
	//
	// Refresh base buildings. Scan through list, if a building is missing,
	// rebuild it, unless it's rebuild count is zero.
	//
	if (m_readyToBuildStructure)
	{
		const ThingTemplate *bldgPlan=nullptr;
		BuildListInfo	*bldgInfo = nullptr;
		Bool isPriority = false;
		Int selectedStructurePriority = (-2147483647 - 1);
		Object *bldg = nullptr;
		const ThingTemplate *powerPlan=nullptr;
		BuildListInfo	*powerInfo = nullptr;
		Bool isUnderPowered = !m_player->getEnergy()->hasSufficientPower();
		Bool powerUnderConstruction = false;
		for( BuildListInfo *info = m_player->getBuildList(); info; info = info->getNext() )
		{
			AsciiString name = info->getTemplateName();
			if (name.isEmpty()) continue;
			const ThingTemplate *curPlan = TheThingFactory->findTemplate( name );
			if (!curPlan) {
				DEBUG_LOG(("*** ERROR - Build list building '%s' doesn't exist.", name.str()));
				continue;
			}
			bldg = TheGameLogic->findObjectByID( info->getObjectID() );
			// check for hole.
			if (info->getObjectID() != INVALID_ID) {
				// used to have a building.
				Object *bldg = TheGameLogic->findObjectByID( info->getObjectID() );
				if (bldg==nullptr) {
					// got destroyed.
					ObjectID priorID;
					priorID = info->getObjectID();
					info->setObjectID(INVALID_ID);
					info->setObjectTimestamp(TheGameLogic->getFrame()+1);
					// Scan for a GLA hole.	KINDOF_REBUILD_HOLE
					Object *obj;
					for( obj = TheGameLogic->getFirstObject(); obj; obj = obj->getNextObject() ) {
						if (!obj->isKindOf(KINDOF_REBUILD_HOLE)) continue;
						RebuildHoleBehaviorInterface *rhbi = RebuildHoleBehavior::getRebuildHoleBehaviorInterfaceFromObject( obj );
						if( rhbi ) {
							ObjectID spawnerID = rhbi->getSpawnerID();
							if (priorID == spawnerID) {
								DEBUG_LOG(("AI Found hole to rebuild %s", curPlan->getName().str()));
								info->setObjectID(obj->getID());
							}
						}
					}
				}	else {
					if (bldg->getControllingPlayer() == m_player) {
						// Check for built or dozer missing.
						if( bldg->getStatusBits().test( OBJECT_STATUS_UNDER_CONSTRUCTION ) )
						{
							if (bldg->isKindOf(KINDOF_FS_POWER) && !bldg->isKindOf(KINDOF_CASH_GENERATOR))
							{
								powerUnderConstruction = true;
							}
							// make sure dozer is working on him.
							ObjectID builder = bldg->getBuilderID();
							Object* myDozer = TheGameLogic->findObjectByID(builder);

              if (myDozer && ( myDozer->getControllingPlayer() != m_player || myDozer->isDisabledByType( DISABLED_UNMANNED ) ) )
              {//I don't expect this dozer to work well with me.
                myDozer = nullptr;
                bldg->setBuilder( nullptr );
              }

							if (myDozer==nullptr) {
								DEBUG_LOG(("AI's Dozer got killed (or captured).  Find another dozer."));
								if (usesProductionBehavior() &&
									IsSkirmishAIStrategyAuthorizedStructure(curPlan))
									queueAuthorizedStrategyBuilder(curPlan);
								else
									queueDozer();
								myDozer = findDozer(bldg->getPosition());
								if (myDozer==nullptr || myDozer->getAI()==nullptr) {
									continue;
								}
								myDozer->getAI()->aiResumeConstruction(bldg, CMD_FROM_AI);
							}	else {
								// make sure he is building.
								myDozer->getAI()->aiResumeConstruction(bldg, CMD_FROM_AI);
							}
						}
					} else {
						// oops, got captured.
						info->setObjectID(INVALID_ID);
						info->setObjectTimestamp(TheGameLogic->getFrame()+1);
					}
				}
			}
			if (info->getObjectID()==INVALID_ID && info->getObjectTimestamp()>0) {
				// this object was built at some time, and got destroyed at or near objectTimestamp.
				// Wait a few seconds before initiating a rebuild.
				if (info->getObjectTimestamp()+TheAI->getAiData()->m_rebuildDelaySeconds*LOGICFRAMES_PER_SECOND > TheGameLogic->getFrame()) {
					continue;
				}	else {
					DEBUG_LOG(("Enabling rebuild for %s", info->getTemplateName().str()));
					info->setObjectTimestamp(0); // ready to build.
				}
			}
			if (bldg) {
				continue; // already built.
			}
			Bool usableSupplyCenter = false;
			Bool prerequisiteSupplyCenter = false;
			const Bool productionBehavior = usesProductionBehavior();
			const Bool isSupplyCenter =
				curPlan->isKindOf(KINDOF_FS_SUPPLY_CENTER);
			if (productionBehavior && isSupplyCenter &&
				!info->isPriorityBuild()) {
				usableSupplyCenter = hasUsableSupplySource(
					info->getLocation(),
					curPlan->getTemplateGeometryInfo().getBoundingCircleRadius());
				if (!usableSupplyCenter) {
					prerequisiteSupplyCenter =
						ShouldBuildSkirmishAIPrerequisiteSupplyCenter(
							false, hasOwnedSupplyCenter(curPlan),
							hasQueuedSupplyCenter(curPlan)) &&
						isSupplyCenterPrerequisiteNeeded(curPlan);
					if (!prerequisiteSupplyCenter || !m_baseCenterSet)
						continue;
					Coord3D prerequisiteLocation = m_baseCenter;
					if (!calcClosestConstructionZoneLocation(
							curPlan, &prerequisiteLocation))
						continue;
					info->setLocation(prerequisiteLocation);
				}
			}
			const Bool isAutomaticSuperweapon = productionBehavior &&
				curPlan->isKindOf(KINDOF_FS_SUPERWEAPON) &&
				!info->isPriorityBuild();
			const Bool admittedSuperweapon = isAutomaticSuperweapon &&
				m_strategyState.currentMode == SKIRMISH_STRATEGY_FORTIFY &&
				m_strategyState.superweaponAttemptStatus ==
					SKIRMISH_STRATEGY_ATTEMPT_NOT_STARTED;
			if (isAutomaticSuperweapon && !admittedSuperweapon)
				continue;

			// Make sure it is safe to build here.
			if (!isLocationSafe(info->getLocation(), curPlan)) {
				continue;
			}
			if (ShouldUseCurrentSkirmishAIBehavior() && !ShouldSkirmishAIConsiderRebuild(
				info->isAutomaticBuild(),
				info->isPriorityBuild(),
				curPlan->isKindOf(KINDOF_FS_POWER) && !curPlan->isKindOf(KINDOF_CASH_GENERATOR),
				isUnderPowered)) {
				continue;
			}
			if (!productionBehavior && info->isPriorityBuild()) {
				// Always take priority build, unless we already have priority build.
				if (!isPriority) {
					bldgPlan = curPlan;
					bldgInfo = info;
					isPriority = true;
				}
			}
			if (curPlan->isKindOf(KINDOF_FS_POWER)) {
				if (powerPlan==nullptr && !curPlan->isKindOf(KINDOF_CASH_GENERATOR)) {
					if (isUnderPowered || info->isAutomaticBuild()) {
						powerPlan = curPlan;
						powerInfo = info;
					}
				}
			}
			if (!info->isAutomaticBuild() &&
				(!productionBehavior || !info->isPriorityBuild())) {
				continue; // marked to not build automatically.
			}
			Object *dozer = findDozer(info->getLocation());
			const Bool authorizedWithoutBuilder = productionBehavior &&
				info->isBuildable() &&
				(info->isPriorityBuild() ||
				 IsSkirmishAIStrategyAuthorizedStructure(curPlan));
			if (dozer==nullptr) {
				if (!authorizedWithoutBuilder && (isUnderPowered ||
					(productionBehavior && info->isBuildable()))) {
					queueDozer();
				}
				if (!authorizedWithoutBuilder)
					continue;
			}
			if (dozer && TheBuildAssistant->canMakeUnit(dozer,
					GetSkirmishAutomaticConstructionPlan(curPlan, bldgPlan))!=CANMAKE_OK) {
				if (info->isBuildable()) {
					AsciiString bldgName = info->getTemplateName();
					bldgName.concat(" - Dozer unable to build - money or technology missing.");
					TheScriptEngine->AppendDebugMessage(bldgName, false);
				}
				continue;
			}
			if (productionBehavior && info->isBuildable()) {
				const ThingTemplate *previousAuthorization =
					m_strategyAuthorizedThing;
				const SkirmishAISpendAuthorization previousAuthorizationClass =
					m_strategySpendAuthorization;
				const Bool authorizeCriticalCandidate =
					info->isPriorityBuild() ||
					(IsSkirmishAIStrategyAuthorizedStructure(curPlan) &&
					 (!dozer || canStartCriticalRebuildNow(info, curPlan)));
				m_strategyAuthorizedThing = authorizeCriticalCandidate
					? curPlan : nullptr;
				m_strategySpendAuthorization = authorizeCriticalCandidate
					? SKIRMISH_AI_SPEND_AUTHORIZATION_PRIORITY_STRUCTURE
					: SKIRMISH_AI_SPEND_AUTHORIZATION_NONE;
				const Bool candidateCanSpend = canSpendForCriticalRecovery(
					curPlan->calcCostToBuild(m_player), curPlan, false, false);
				m_strategyAuthorizedThing = previousAuthorization;
				m_strategySpendAuthorization = previousAuthorizationClass;
				if (!candidateCanSpend)
					continue;
			}
			// check if this building has any "rebuilds" left
			if (info->isBuildable())
			{
				if (productionBehavior) {
					const Bool cashGenerator =
						IsSkirmishAIAlternateIncomeStructure(
							curPlan->isKindOf(KINDOF_CASH_GENERATOR),
							curPlan->isKindOf(KINDOF_FS_SUPPLY_DROPZONE),
							curPlan->isKindOf(KINDOF_FS_BLACK_MARKET),
							curPlan->isKindOf(KINDOF_FS_INTERNET_CENTER));
					const Bool productionFacility =
						!curPlan->isKindOf(KINDOF_COMMANDCENTER) &&
						(curPlan->isKindOf(KINDOF_FS_FACTORY) ||
						 curPlan->isKindOf(KINDOF_FS_BARRACKS) ||
						 curPlan->isKindOf(KINDOF_FS_WARFACTORY) ||
						 curPlan->isKindOf(KINDOF_FS_AIRFIELD));
					const Int priority = GetSkirmishAIStructurePriority(
						info->isPriorityBuild(),
						curPlan->isKindOf(KINDOF_COMMANDCENTER),
						curPlan->isKindOf(KINDOF_FS_POWER) &&
							!curPlan->isKindOf(KINDOF_CASH_GENERATOR) &&
							isUnderPowered,
						cashGenerator,
						isSupplyCenter &&
							(usableSupplyCenter || prerequisiteSupplyCenter),
						productionFacility,
						curPlan->isKindOf(KINDOF_FS_BASE_DEFENSE),
						m_strategyState.currentMode == SKIRMISH_STRATEGY_FORTIFY,
						admittedSuperweapon);
					if (!bldgPlan || priority > selectedStructurePriority) {
						bldgPlan = curPlan;
						bldgInfo = info;
						selectedStructurePriority = priority;
					}
				} else if (bldgPlan == nullptr) {
					bldgPlan = curPlan;
					bldgInfo = info;
				}
			}
		}
		if (!usesProductionBehavior() && powerInfo && powerPlan &&
			!powerPlan->isEquivalentTo(bldgPlan)) {
			if (!powerUnderConstruction) {
				bldgPlan = powerPlan;
				bldgInfo = powerInfo;
				DEBUG_LOG(("Forcing build of power plant."));
			}
		}
		if (bldgPlan && bldgInfo) {
#ifdef USE_DOZER
			if (!findDozer(bldgInfo->getLocation())) {
				queueAuthorizedStrategyBuilder(bldgPlan);
				return;
			}
			// dozer-construct the building
			const Bool authorizeCriticalBuild = usesProductionBehavior() &&
				(bldgInfo->isPriorityBuild() ||
				 (IsSkirmishAIStrategyAuthorizedStructure(bldgPlan) &&
				  canStartCriticalRebuildNow(bldgInfo, bldgPlan)));
			const ThingTemplate *previousAuthorization =
				m_strategyAuthorizedThing;
			const SkirmishAISpendAuthorization previousAuthorizationClass =
				m_strategySpendAuthorization;
			m_strategyAuthorizedThing = authorizeCriticalBuild
				? bldgPlan : nullptr;
			m_strategySpendAuthorization = authorizeCriticalBuild
				? SKIRMISH_AI_SPEND_AUTHORIZATION_PRIORITY_STRUCTURE
				: SKIRMISH_AI_SPEND_AUTHORIZATION_NONE;
			bldg = buildStructureWithDozer(bldgPlan, bldgInfo);
			m_strategyAuthorizedThing = previousAuthorization;
			m_strategySpendAuthorization = previousAuthorizationClass;
			// store the object with the build order
			if (bldg)
			{
				bldgInfo->setObjectID( bldg->getID() );
				bldgInfo->decrementNumRebuilds();
				if (usesProductionBehavior() &&
					bldgPlan->isKindOf(KINDOF_FS_SUPERWEAPON) &&
					m_strategyState.currentMode == SKIRMISH_STRATEGY_FORTIFY &&
					m_strategyState.superweaponAttemptStatus ==
						SKIRMISH_STRATEGY_ATTEMPT_NOT_STARTED) {
					m_strategySuperweaponID = bldg->getID();
					m_strategyState.superweaponAttemptStatus =
						SKIRMISH_STRATEGY_ATTEMPT_PENDING;
				}
				if (usesProductionBehavior())
					refreshStrategyProductionReserve();

				m_readyToBuildStructure = false;
				m_structureTimer = TheAI->getAiData()->m_structureSeconds*LOGICFRAMES_PER_SECOND;
				if (m_player->getMoney()->countMoney() < TheAI->getAiData()->m_resourcesPoor) {
					m_structureTimer = m_structureTimer/TheAI->getAiData()->m_structuresPoorMod;
				}	else if (m_player->getMoney()->countMoney() > TheAI->getAiData()->m_resourcesWealthy) {
					m_structureTimer = m_structureTimer/TheAI->getAiData()->m_structuresWealthyMod;
				}
				m_frameLastBuildingBuilt = TheGameLogic->getFrame();
				// only build one building per delay loop
			}

#else
			// force delay between rebuilds
			Int framesToBuild = bldgPlan->calcTimeToBuild(m_player);
			if (TheGameLogic->getFrame() - m_frameLastBuildingBuilt < framesToBuild)
			{
				m_buildDelay = framesToBuild - (TheGameLogic->getFrame() - m_frameLastBuildingBuilt);
				return;
			}	else {
				// building is missing, (re)build it
				// deduct money to build, if we have it
				Int cost = bldgPlan->calcCostToBuild( m_player );
				if (m_player->getMoney()->countMoney() >= cost &&
					canSpendForCriticalRecovery(cost, bldgPlan, false, false))
				{
					// we have the money, deduct it
					m_player->getMoney()->withdraw( cost );

					// inst-construct the building
					bldg = buildStructureNow(bldgPlan, bldgInfo);
					// store the object with the build order
					if (bldg)
					{
						bldgInfo->setObjectID( bldg->getID() );
						bldgInfo->decrementNumRebuilds();

						m_readyToBuildStructure = false;
						m_structureTimer = TheAI->getAiData()->m_structureSeconds*LOGICFRAMES_PER_SECOND;
						if (m_player->getMoney()->countMoney() < TheAI->getAiData()->m_resourcesPoor) {
							m_structureTimer = m_structureTimer/TheAI->getAiData()->m_structuresPoorMod;
						}	else if (m_player->getMoney()->countMoney() > TheAI->getAiData()->m_resourcesWealthy) {
							m_structureTimer = m_structureTimer/TheAI->getAiData()->m_structuresWealthyMod;
						}
						m_frameLastBuildingBuilt = TheGameLogic->getFrame();
						if (usesProductionBehavior())
							refreshStrategyProductionReserve();
					}
				}
			}
#endif
		}
	}
}

/**
 * Invoked when a unit I am training comes into existence
 */
void AISkirmishPlayer::onUnitProduced(
	Object *factory, Object *unit, Int productionID)
{
	Bool newlyObservedRecoveryReplacement = false;
	Bool producedRecoveryReplacement = false;
	Bool trackedRecoveryReplacement = false;
	Int recoveryCommandCenterCost = -1;
	ProductionUpdateInterface *production = factory
		? factory->getProductionUpdateInterface() : nullptr;
	const ProductionEntry *currentEntry = production
		? production->firstProduction() : nullptr;
	if (production && productionID != 0) {
		currentEntry = nullptr;
		for (const ProductionEntry *entry = production->firstProduction(); entry;
			entry = production->nextProduction(entry)) {
			if (static_cast<Int>(entry->getProductionID()) == productionID) {
				currentEntry = entry;
				break;
			}
		}
	}
	const ThingTemplate *currentProduct = currentEntry &&
		currentEntry->getProductionType() == PRODUCTION_UNIT
			? currentEntry->getProductionObject() : nullptr;
	const Bool currentEntryMatchesUnit = currentEntry && currentProduct && unit &&
		unit->getTemplate() && currentEntry->getProductionQuantityRemaining() > 0 &&
		currentProduct->isEquivalentTo(unit->getTemplate());
	const Bool identityTracked = IsSkirmishAIRecoveryProductionIdentityTracked(
		m_recoveryBuilderFactoryID, m_recoveryBuilderProductionID,
		INVALID_ID, PRODUCTIONID_INVALID);
	if (usesCriticalRecoveryBehavior() && factory && unit &&
		unit->getControllingPlayer() == m_player &&
		IsLiveSkirmishAIRecoveryObject(factory, m_player) &&
		unit->getProducerID() == factory->getID() &&
		unit->isKindOf(KINDOF_DOZER) && m_player->getPlayerTemplate()) {
		const ThingTemplate *primaryTemplate = TheThingFactory->findTemplate(
			m_player->getPlayerTemplate()->getStartingBuilding());
		if (primaryTemplate)
			recoveryCommandCenterCost = primaryTemplate->calcCostToBuild(m_player);
		Object *primaryCenter = nullptr;
		const Bool hasCompletedPrimaryCenter = primaryTemplate &&
			findPrimaryCommandCenter(primaryTemplate, &primaryCenter) &&
			primaryCenter &&
			!primaryCenter->testStatus(OBJECT_STATUS_UNDER_CONSTRUCTION);
		if (primaryTemplate && unit->getTemplate() &&
			HasSkirmishAICommandSetForTemplate(
				unit->getTemplate()->friend_getCommandSetString(), primaryTemplate) &&
			!hasCompletedPrimaryCenter &&
			HasSkirmishAIRecoveryScaffoldReplacementAttempt(
				m_recoveryPlacementAttempt, g_skirmishAIRecoveryOffsetCount)) {
			newlyObservedRecoveryReplacement =
				!HasSkirmishAIRecoveryObservedReplacement(
					m_recoveryPlacementAttempt, g_skirmishAIRecoveryOffsetCount);
			trackedRecoveryReplacement = currentEntryMatchesUnit &&
				IsSkirmishAIRecoveryProductionIdentityMatch(
					m_recoveryBuilderFactoryID, m_recoveryBuilderProductionID,
					factory->getID(), static_cast<Int>(currentEntry->getProductionID()),
					INVALID_ID, PRODUCTIONID_INVALID);
			producedRecoveryReplacement = trackedRecoveryReplacement ||
				// Production can complete before the first post-load AI update.
				// In that narrow ordering window, adopt the current compatible
				// paid entry only while the replacement is still unobserved.
				ShouldAdoptSkirmishAIRecoveryProduction(
					identityTracked, newlyObservedRecoveryReplacement,
					currentEntryMatchesUnit);
		}
		if (producedRecoveryReplacement && newlyObservedRecoveryReplacement) {
			m_recoveryPlacementAttempt = MarkSkirmishAIRecoveryObservedReplacement(
				m_recoveryPlacementAttempt, g_skirmishAIRecoveryOffsetCount);
		}
	}

	WorkOrder *recoveryOrder = nullptr;
	Bool recoveryOrderWasResourceGatherer = false;
	Bool recoveryOrderWasForeignTeam = false;
	Bool startDirectRecoveryResourceGathering = false;
	SupplyTruckAIInterface *directRecoverySupplyAI = nullptr;
	if (producedRecoveryReplacement) {
		// Match the order the base callback will consume.  Keep a resource worker
		// gathering when its post-debit cash cannot yet fund the command center;
		// recovery will preempt it after affordability and placement checks pass.
		// Otherwise suppress supply routing only for this produced recovery builder.
		// Keep the stored role intact for cancellation and later economy scheduling.
		// A direct recovery queue has no order for the base callback to route, so
		// start only an actual harvester with a supply interface after that callback.
		for (DLINK_ITERATOR<TeamInQueue> iter = iterate_TeamBuildQueue();
			!iter.done() && !recoveryOrder; iter.advance()) {
			TeamInQueue *team = iter.cur();
			for (WorkOrder *order = team->m_workOrders; order; order = order->m_next) {
				if (order->m_factoryID == factory->getID() &&
					(!usesProductionBehavior() || order->m_productionID == 0 ||
					 (productionID != 0 &&
					  order->m_productionID == productionID)) &&
					order->m_numCompleted < order->m_numRequired &&
					unit->getTemplate()->isEquivalentTo(order->m_thing)) {
					recoveryOrder = order;
					recoveryOrderWasResourceGatherer = order->m_isResourceGatherer;
					recoveryOrderWasForeignTeam =
						team->m_team != m_player->getDefaultTeam();
					break;
				}
			}
		}
		const Bool preservationBehavior =
			ShouldUseCurrentSkirmishAIRecoveryResourceWorkerPreservation();
		if (preservationBehavior && !recoveryOrder &&
			unit->isKindOf(KINDOF_HARVESTER)) {
			AIUpdateInterface *recoveryAI = unit->getAIUpdateInterface();
			if (recoveryAI)
				directRecoverySupplyAI = recoveryAI->getSupplyTruckAIInterface();
		}
		const SkirmishAIRecoveryResourceRoutingDecision routing =
			GetSkirmishAIRecoveryResourceRoutingDecision(
				preservationBehavior,
				recoveryOrder != nullptr, recoveryOrderWasResourceGatherer,
				directRecoverySupplyAI != nullptr,
				m_player->getMoney()->countMoney(), recoveryCommandCenterCost);
		if (recoveryOrder)
			recoveryOrder->m_isResourceGatherer =
				routing.resourceGathererDuringCallback;
		startDirectRecoveryResourceGathering =
			routing.startDirectResourceGatheringAfterCallback;
	}
	AIPlayer::onUnitProduced(factory, unit, productionID);
	if (recoveryOrder)
		recoveryOrder->m_isResourceGatherer = recoveryOrderWasResourceGatherer;
	else if (startDirectRecoveryResourceGathering && directRecoverySupplyAI)
		directRecoverySupplyAI->setForceWantingState(true);
	if (ShouldUseCurrentSkirmishAIStrategyControllerBehavior() &&
		producedRecoveryReplacement && recoveryOrderWasForeignTeam && unit &&
		m_player && m_player->getDefaultTeam()) {
		unit->setTeam(m_player->getDefaultTeam());
		AIUpdateInterface *recoveryAI = unit->getAIUpdateInterface();
		if (recoveryAI)
			recoveryAI->aiIdle(CMD_FROM_AI);
	}
#if defined(_MSC_VER) && _MSC_VER < 1300
	// The retail-compatible base callback initializes its local supply flag true.
	// A direct recovery queue has no WorkOrder to clear that flag, so preserve
	// the ordinary non-resource dozer completion behavior explicitly for VC6.
	if (producedRecoveryReplacement && !recoveryOrder && unit &&
		unit->isKindOf(KINDOF_DOZER)) {
		if (m_dozerQueuedForRepair) {
			m_repairDozer = unit->getID();
			m_dozerQueuedForRepair = false;
		} else {
			m_buildDelay = 0;
			m_structureTimer = 1;
		}
	}
#endif
	if (trackedRecoveryReplacement)
		clearRecoveryBuilderProduction();
	if (usesCriticalRecoveryBehavior() && unit && unit->isKindOf(KINDOF_DOZER))
		m_recoveryNextAttemptFrame = 0;
}

void AISkirmishPlayer::onStructureProduced(Object *factory, Object *structure)
{
	AIPlayer::onStructureProduced(factory, structure);
	if (!usesCriticalRecoveryBehavior() || !structure ||
		structure->getControllingPlayer() != m_player || !m_player->getPlayerTemplate())
		return;
	const ThingTemplate *primaryTemplate = TheThingFactory->findTemplate(
		m_player->getPlayerTemplate()->getStartingBuilding());
	if (!primaryTemplate || !structure->isKindOf(KINDOF_COMMANDCENTER) ||
		!structure->getTemplate() ||
		!structure->getTemplate()->isEquivalentTo(primaryTemplate))
		return;
	// A captured incomplete center does not establish history while it is
	// incomplete.  Once this completion callback fires, however, the owned
	// primary center is a real completed recovery landmark.
	m_recoveryEverCompleted = true;
	m_recoveryImpossible = false;
	m_recoveryConstructionID = structure->getID();
	m_recoveryEvacuationDeadline = 0;
	m_recoveryLocation = *structure->getPosition();
	m_recoveryAngle = structure->getOrientation();
	m_recoveryPlacementAttempt = 0;
	m_recoveryBuilderFailoverConsumed =
		ReconcileSkirmishAIRecoveryBuilderFailoverConsumed(
			m_recoveryBuilderFailoverConsumed, true, true);
	m_recoveryNextAttemptFrame = 0;
	m_recoveryReserveCost = 0;
}

/**
 * Search the computer player's buildings for one that can build the given request
 * and start training the unit.
 * If busyOK is true, it will queue a unit even if one is building.  This lets
 * script invoked teams "push" to the front of the queue.
 */
Bool AISkirmishPlayer::startTraining( WorkOrder *order, Bool busyOK, AsciiString teamName)
{
	// While a completed primary center is absent, recovery owns ordinary
	// compatible builder admission.  Preserve a GLA resource-worker income route
	// until recovery has actually paid for an equivalent builder; after that, suppress
	// its ordinary WorkOrder so a bounded factory cannot pay a duplicate when it
	// resumes.
	if (order && order->m_thing && order->m_thing->isKindOf(KINDOF_DOZER) &&
		m_player && m_player->getPlayerTemplate()) {
		const ThingTemplate *primaryTemplate = TheThingFactory->findTemplate(
			m_player->getPlayerTemplate()->getStartingBuilding());
		const Bool compatibleBuilder = primaryTemplate &&
			HasSkirmishAICommandSetForTemplate(
				order->m_thing->friend_getCommandSetString(), primaryTemplate);
		Object *primaryCenter = nullptr;
		const Bool hasPrimaryCenter = primaryTemplate &&
			findPrimaryCommandCenter(primaryTemplate, &primaryCenter);
		const Bool hasCompletedPrimaryCenter = hasPrimaryCenter &&
			primaryCenter &&
			!primaryCenter->testStatus(OBJECT_STATUS_UNDER_CONSTRUCTION);
		Bool nativePrimaryRebuildPending = false;
		if (compatibleBuilder &&
			ShouldUseCurrentSkirmishAIRecoveryNativeHoleOwnership() &&
			hasPrimaryCenter && primaryCenter &&
			primaryCenter->testStatus(OBJECT_STATUS_UNDER_CONSTRUCTION)) {
			BuildListInfo *primaryInfo =
				findPrimaryCommandCenterBuildInfo(primaryTemplate);
			Object *primaryHole = TheGameLogic->findObjectByID(
				primaryCenter->getProducerID());
			if (!IsSkirmishAIRecoveryPrimaryHole(
					primaryHole, m_player, primaryTemplate, primaryInfo,
					primaryCenter->getID()))
				primaryHole = FindSkirmishAIRecoveryHoleForConstruction(
					m_player, primaryTemplate, primaryCenter->getID());
			RebuildHoleBehaviorInterface *primaryHoleAI = primaryHole
				? RebuildHoleBehavior::getRebuildHoleBehaviorInterfaceFromObject(
					primaryHole) : nullptr;
			Object *nativeWorker = primaryHoleAI
				? TheGameLogic->findObjectByID(primaryHoleAI->getWorkerID())
				: nullptr;
			const Bool nativeWorkerExists = nativeWorker != nullptr;
			const Bool nativeWorkerLive = IsLiveSkirmishAIRecoveryObject(
				nativeWorker, m_player);
			const Bool nativeWorkerIsDozer = nativeWorkerExists &&
				nativeWorker->isKindOf(KINDOF_DOZER);
			const Bool nativeWorkerCompatible = nativeWorkerExists &&
				HasSkirmishAICommandForTemplate(nativeWorker, primaryTemplate);
			AIUpdateInterface *nativeWorkerAI = nativeWorkerLive
				? nativeWorker->getAIUpdateInterface() : nullptr;
			DozerAIInterface *nativeWorkerDozer = nativeWorkerAI
				? nativeWorkerAI->getDozerAIInterface() : nullptr;
			const Bool nativeWorkerOperational = nativeWorkerLive &&
				!nativeWorker->isContained() &&
				!nativeWorker->isDisabledByType(DISABLED_UNMANNED) &&
				nativeWorkerAI && nativeWorkerDozer &&
				CanSkirmishAIRecoveryUpdateAdvance(nativeWorker, nativeWorkerAI);
			const Bool nativeWorkerActivelyBuilding =
				IsSkirmishAIRecoveryNativeWorkerActivelyBuilding(
					nativeWorkerLive, nativeWorkerOperational,
					nativeWorkerDozer &&
						nativeWorkerDozer->getCurrentTask() == DOZER_TASK_BUILD,
					nativeWorkerDozer &&
						nativeWorkerDozer->getBuildSubTask() == DOZER_DO_BUILD_AT_DOCK,
					nativeWorkerDozer &&
						nativeWorkerDozer->getTaskTarget(DOZER_TASK_BUILD) ==
							primaryCenter->getID());
			Coord3D nativeWorkerActionPosition;
			const Bool nativeWorkerBuildDockFound = nativeWorkerOperational &&
				DozerAIUpdate::findGoodBuildOrRepairPosition(
					nativeWorker, primaryCenter, nativeWorkerActionPosition);
			const Bool nativeWorkerResumeUsable = nativeWorkerActivelyBuilding ||
				(nativeWorkerBuildDockFound &&
				 nativeWorkerAI->isPathAvailable(&nativeWorkerActionPosition));
			nativePrimaryRebuildPending =
				ShouldPreserveSkirmishAIRecoveryNativeWorkerLifecycle(
					primaryHole != nullptr, nativeWorkerExists,
					nativeWorkerLive, nativeWorkerIsDozer,
					nativeWorkerCompatible, nativeWorkerResumeUsable);
		}
		Bool paidCompatibleBuilderQueue = false;
		if (compatibleBuilder &&
			usesCriticalRecoveryBehavior() && m_recoveryEverCompleted &&
			!m_recoveryImpossible && !hasCompletedPrimaryCenter)
			hasRecoveryBuilderQueued(
				primaryTemplate, &paidCompatibleBuilderQueue, nullptr, nullptr);
		if (ShouldSuppressSkirmishAIRecoveryBuilderOrder(
			order->m_isResourceGatherer, compatibleBuilder,
			usesCriticalRecoveryBehavior(), m_recoveryEverCompleted,
			m_recoveryImpossible, hasCompletedPrimaryCenter,
			m_recoveryReserveCost, paidCompatibleBuilderQueue,
			nativePrimaryRebuildPending))
			return false;
	}
	Object *factory = nullptr;
	if (usesProductionBehavior()) {
		std::vector<Object *> compatibleProducers;
		FindSkirmishAICompatibleProducers(
			m_player, order->m_thing, &compatibleProducers);
		const Bool economyRecoveryCollector =
			order->m_isResourceGatherer &&
			order->m_thing->isKindOf(KINDOF_HARVESTER);
		if (economyRecoveryCollector &&
			!hasUsableSupplyCenterForCollectors())
			return false;
		if (economyRecoveryCollector) {
			for (std::vector<Object *>::iterator producer =
				compatibleProducers.begin(); producer != compatibleProducers.end();) {
				const Bool supplyCenter =
					(*producer)->isKindOf(KINDOF_FS_SUPPLY_CENTER);
				const Bool usableLocalSource = !supplyCenter ||
					hasUsableSupplySource(
						(*producer)->getPosition(),
						(*producer)->getGeometryInfo().getBoundingCircleRadius());
				if (!IsSkirmishAICollectorProducerEligible(
						supplyCenter, usableLocalSource)) {
					producer = compatibleProducers.erase(producer);
					continue;
				}
				++producer;
			}
		}
		if (compatibleProducers.empty())
			return false;
		// Candidate selection reuses the current snapshot. queueCreateUnit performs
		// the single fresh reserve check at the authoritative debit boundary.
		const ThingTemplate *previousAuthorization = m_strategyAuthorizedThing;
		const SkirmishAISpendAuthorization previousAuthorizationClass =
			m_strategySpendAuthorization;
		if (economyRecoveryCollector) {
			m_strategyAuthorizedThing = order->m_thing;
			m_strategySpendAuthorization =
				SKIRMISH_AI_SPEND_AUTHORIZATION_COLLECTOR;
		}
		ProductionID queuedProductionID = PRODUCTIONID_INVALID;
		factory = QueueSkirmishAIUnitAtCompatibleProducer(
			compatibleProducers, order->m_thing, busyOK,
			&queuedProductionID);
		if (factory)
			order->m_productionID = static_cast<Int>(queuedProductionID);
		m_strategyAuthorizedThing = previousAuthorization;
		m_strategySpendAuthorization = previousAuthorizationClass;
	} else {
		factory = findFactory(order->m_thing, busyOK);
	}
	if( factory )
	{
		ProductionUpdateInterface *pu = factory->getProductionUpdateInterface();
		const Bool queued = usesProductionBehavior() ||
			(pu && pu->queueCreateUnit(
				order->m_thing, pu->requestUniqueUnitID()));
		if (queued) {
			order->m_factoryID = factory->getID();
			if (TheGlobalData->m_debugAI) {
				AsciiString teamStr = "Queuing ";
				teamStr.concat(order->m_thing->getName());
				teamStr.concat(" for ");
				teamStr.concat(teamName);
				TheScriptEngine->AppendDebugMessage(teamStr, false);
			}
			return true;
		}
	}

	return FALSE;

}


/**
 * Check if this team is buildable, doesn't exceed maximum limits, meets conditions, and isn't under construction.
 */
Bool AISkirmishPlayer::isAGoodIdeaToBuildTeam( TeamPrototype *proto )
{
	// Check condition.
	if (!proto->evaluateProductionCondition()) {
		return false;
	}
	// check build limit
	if (proto->countTeamInstances() >= proto->getTemplateInfo()->m_maxInstances){
		if (TheGlobalData->m_debugAI) {
			AsciiString str;
			str.format("Team %s not chosen - %d already exist.", proto->getName().str(), proto->countTeamInstances());
			TheScriptEngine->AppendDebugMessage(str, false);
		}
		return false;	// Max already built.
	}

	for ( DLINK_ITERATOR<TeamInQueue> iter = iterate_TeamBuildQueue(); !iter.done(); iter.advance())
	{
		TeamInQueue *team = iter.cur();
		if (team->m_team->getPrototype() == proto) {
			return false; // currently building one of these.
		}
	}
	Bool needMoney;
	if (!isPossibleToBuildTeam( proto, true, needMoney)) {
		if (TheGlobalData->m_debugAI) {
			AsciiString str;
			if (needMoney) {
				str.format("Team %s not chosen - Not enough money.", proto->getName().str());
			} else {
				str.format("Team %s not chosen - Factory/tech missing or busy.", proto->getName().str());
			}
			TheScriptEngine->AppendDebugMessage(str, false);
		}
		return false;
	}
	return true;
}

/**
 * See if any existing teams need reinforcements, and have higher priority.
 */
Bool AISkirmishPlayer::selectTeamToReinforce( Int minPriority )
{
	if (!usesProductionBehavior())
		return AIPlayer::selectTeamToReinforce(minPriority);
	refreshStrategyProductionState();
	refreshStrategyProductionReserve();
	const Int resources = m_player->getMoney()->countMoney();
	const Int reserve = m_strategyProductionReserveCost;
	const Int recoveryReserve = getActiveRecoveryReserveCost();
	const Bool hasUsableCollectorEconomy =
		hasUsableSupplyCenterForCollectors();
	Int priorityCeiling = 2147483647;
	while (true) {
		Int selectedPriority = minPriority;
		Player::PlayerTeamList::const_iterator priorityIt;
		for (priorityIt = m_player->getPlayerTeams()->begin();
			priorityIt != m_player->getPlayerTeams()->end(); ++priorityIt) {
			TeamPrototype *prototype = *priorityIt;
			const Int priority = prototype->getTemplateInfo()->m_productionPriority;
			if (prototype->getTemplateInfo()->m_automaticallyReinforce &&
				priority > selectedPriority && priority < priorityCeiling)
				selectedPriority = priority;
		}
		if (selectedPriority <= minPriority)
			return false;

		std::vector<Team *> candidates;
		Player::PlayerTeamList::const_iterator prototypeIt;
		for (prototypeIt = m_player->getPlayerTeams()->begin();
			prototypeIt != m_player->getPlayerTeams()->end(); ++prototypeIt) {
			TeamPrototype *prototype = *prototypeIt;
			if (!prototype->getTemplateInfo()->m_automaticallyReinforce ||
				prototype->getTemplateInfo()->m_productionPriority != selectedPriority)
				continue;
			Bool busy = false;
			for (DLINK_ITERATOR<TeamInQueue> queueIt = iterate_TeamBuildQueue();
				!queueIt.done(); queueIt.advance()) {
				TeamInQueue *queued = queueIt.cur();
				if (queued && queued->m_team &&
					queued->m_team->getPrototype() == prototype) {
					busy = true;
					break;
				}
			}
			if (busy)
				continue;
			for (DLINK_ITERATOR<Team> teamIt = prototype->iterate_TeamInstanceList();
				!teamIt.done(); teamIt.advance()) {
				Team *team = teamIt.cur();
				if (team && team->hasAnyUnits() &&
					HasSkirmishAIReinforcementDeficit(team))
					candidates.push_back(team);
			}
		}
		std::sort(candidates.begin(), candidates.end(),
			IsSkirmishAIReinforcementTeamIDBefore);
		for (Int roundRobinPass = 0; roundRobinPass < 3; ++roundRobinPass) {
			for (std::vector<Team *>::const_iterator teamIt = candidates.begin();
				teamIt != candidates.end(); ++teamIt) {
			Team *team = *teamIt;
			if (GetSkirmishAIReinforcementRoundRobinPass(
					team->getID(), m_reinforcementRoundRobinCursor) !=
					roundRobinPass)
				continue;
			const TeamPrototype *prototype = team->getPrototype();
			if (!prototype)
				continue;
			const TeamTemplateInfo *teamInfo = prototype->getTemplateInfo();
			for (Int unitIndex = 0; unitIndex < teamInfo->m_numUnitsInfo;
				++unitIndex) {
				const TCreateUnitsInfo *unitInfo = &teamInfo->m_unitsInfo[unitIndex];
				if (unitInfo->maxUnits < 1)
					continue;
				const ThingTemplate *thing =
					TheThingFactory->findTemplate(unitInfo->unitThingName);
				if (!thing)
					continue;
				Int count = 0;
				team->countObjectsByThingTemplate(1, &thing, false, &count);
				if (!ShouldTrySkirmishAIRecruitBeforePaidTraining(
						count, unitInfo->maxUnits, false, false))
					continue;
				const Bool collector = thing->isKindOf(KINDOF_HARVESTER) &&
					!thing->isKindOf(KINDOF_DOZER);
				if (collector && !hasUsableCollectorEconomy)
					continue;

				Coord3D origin = prototype->getTemplateInfo()->m_homeLocation;
				if (team->getFirstItemIn_TeamMemberList())
					origin = *team->getFirstItemIn_TeamMemberList()->getPosition();
				Object *recruit = team->tryToRecruit(
					thing, &origin, TheAI->getAiData()->m_maxRecruitDistance);
				std::vector<Object *> producers;
				if (!recruit) {
					FindSkirmishAICompatibleProducers(m_player, thing, &producers);
					Bool idleProducer = false;
					for (std::vector<Object *>::const_iterator producer = producers.begin();
						producer != producers.end(); ++producer) {
						ProductionUpdateInterface *production =
							(*producer)->getProductionUpdateInterface();
						if (production && production->getProductionCount() == 0) {
							idleProducer = true;
							break;
						}
					}
					const SkirmishAISpendAuthorization authorization = collector
						? SKIRMISH_AI_SPEND_AUTHORIZATION_COLLECTOR
						: SKIRMISH_AI_SPEND_AUTHORIZATION_NONE;
					const Bool reserveAdmitted = CanSkirmishAISpendWithAuthorization(
							resources, thing->calcCostToBuild(m_player), reserve,
							recoveryReserve, authorization);
					if (!ShouldSelectSkirmishAIReinforcementCandidate(
							false, idleProducer, reserveAdmitted))
						continue;
				}

				TeamInQueue *teamQueue = newInstance(TeamInQueue);
				WorkOrder *order = newInstance(WorkOrder);
				order->m_thing = thing;
				order->m_factoryID = INVALID_ID;
				order->m_numRequired = 1;
				order->m_required = true;
				order->m_isResourceGatherer = collector;
				order->m_next = nullptr;
				teamQueue->m_priorityBuild = false;
				teamQueue->m_reinforcement = true;
				teamQueue->m_workOrders = order;
				teamQueue->m_frameStarted = TheGameLogic->getFrame();
				teamQueue->m_team = team;
				prependTo_TeamBuildQueue(teamQueue);
				if (recruit) {
					order->m_numCompleted = 1;
					recruit->setTeam(team);
					teamQueue->m_reinforcementID = recruit->getID();
					AIUpdateInterface *ai = recruit->getAIUpdateInterface();
					if (ai)
						ai->aiIdle(CMD_FROM_AI);
				} else if (!startTraining(
						order, false, team->getName())) {
					removeFrom_TeamBuildQueue(teamQueue);
					deleteInstance(teamQueue);
					continue;
				}
				m_teamDelay = 0;
				m_reinforcementRoundRobinCursor =
					AdvanceSkirmishAIRoundRobinCursor(
						m_reinforcementRoundRobinCursor, team->getID(), true);
				return true;
			}
		}
		}
		priorityCeiling = selectedPriority;
	}
}

Bool AISkirmishPlayer::isAdaptiveProductionCandidate(
	TeamPrototype *proto, SkirmishAICostRange *costRange, Int *factoryWaitFrames)
{
	if (!proto || !costRange || !factoryWaitFrames || !proto->evaluateProductionCondition())
		return false;
	if (proto->countTeamInstances() >= proto->getTemplateInfo()->m_maxInstances)
		return false;
	for (DLINK_ITERATOR<TeamInQueue> iter = iterate_TeamBuildQueue(); !iter.done(); iter.advance()) {
		TeamInQueue *team = iter.cur();
		if (team && team->m_team && team->m_team->getPrototype() == proto)
			return false;
	}

	Bool anyIdleFactory = false;
	Bool containsCollector = false;
	const TeamTemplateInfo *info = proto->getTemplateInfo();
	for (Int i = 0; i < info->m_numUnitsInfo; ++i) {
		const TCreateUnitsInfo *unitInfo = &info->m_unitsInfo[i];
		const ThingTemplate *thing = TheThingFactory->findTemplate(unitInfo->unitThingName);
		if (!thing)
			continue;
		if (thing->isKindOf(KINDOF_HARVESTER) &&
			!thing->isKindOf(KINDOF_DOZER))
			containsCollector = true;
		if (usesProductionBehavior()) {
			std::vector<Object *> compatibleProducers;
			FindSkirmishAICompatibleProducers(
				m_player, thing, &compatibleProducers);
			if (compatibleProducers.empty())
				return false;
			for (std::vector<Object *>::const_iterator producer =
					compatibleProducers.begin();
				producer != compatibleProducers.end(); ++producer) {
				ProductionUpdateInterface *production =
					(*producer)->getProductionUpdateInterface();
				if (production && production->getProductionCount() == 0) {
					anyIdleFactory = true;
					break;
				}
			}
		} else {
			if (!findFactory(thing, true))
				return false;
			if (findFactory(thing, false))
				anyIdleFactory = true;
		}
	}
	if (!anyIdleFactory)
		return false;
	// Initial team creation is atomic, so a team containing any collector must
	// wait for a live supply economy. Reinforcement queues only one order and
	// can skip an individual collector instead.
	if (usesProductionBehavior() && containsCollector &&
		!hasUsableSupplyCenterForCollectors())
		return false;

	*costRange = MakeSkirmishAICostRange();
	Int ignoredCompletionFrames = 0;
	if (!estimateTeamProduction(
		proto, false, &costRange->minimumCost, &ignoredCompletionFrames))
		return false;
	if (!estimateTeamProduction(
		proto, true, &costRange->plannedCost, factoryWaitFrames))
		return false;

	return true;
}

Int AISkirmishPlayer::getActiveRecoveryReserveCost() const
{
	if (m_recoveryImpossible || m_recoveryReserveCost <= 0)
		return 0;
	const Bool boundedGraceExpired = TheGameLogic &&
		IsSkirmishAIRecoveryBoundedGraceExpired(
			TheGameLogic->getFrame(), m_recoveryEvacuationDeadline);
	if (ShouldReleaseSkirmishAIRecoveryReserveForExpiredGrace(
			boundedGraceExpired,
			m_recoveryNextAttemptFrame == m_recoveryEvacuationDeadline))
		return 0;
	return m_recoveryReserveCost;
}

Int AISkirmishPlayer::getCriticalRebuildReserve(Bool *canStartNow)
{
	if (canStartNow)
		*canStartNow = false;
	Int cheapestCost = 0;
	Bool isUnderPowered = !m_player->getEnergy()->hasSufficientPower();
	const Bool productionBehavior = usesProductionBehavior();
	for (BuildListInfo *info = m_player->getBuildList(); info; info = info->getNext()) {
		const ThingTemplate *plan = TheThingFactory->findTemplate(info->getTemplateName());
		if (!plan)
			continue;
		const Bool isSupplyCenter =
			plan->isKindOf(KINDOF_FS_SUPPLY_CENTER);
		const Bool alternateIncome = productionBehavior &&
			IsSkirmishAIAlternateIncomeStructure(
				plan->isKindOf(KINDOF_CASH_GENERATOR),
				plan->isKindOf(KINDOF_FS_SUPPLY_DROPZONE),
				plan->isKindOf(KINDOF_FS_BLACK_MARKET),
				plan->isKindOf(KINDOF_FS_INTERNET_CENTER));
		const Bool critical = plan->isKindOf(KINDOF_COMMANDCENTER) ||
			plan->isKindOf(KINDOF_FS_POWER) ||
			alternateIncome ||
			isSupplyCenter ||
			plan->isKindOf(KINDOF_FS_FACTORY) ||
			plan->isKindOf(KINDOF_FS_BARRACKS) ||
			plan->isKindOf(KINDOF_FS_WARFACTORY) ||
			plan->isKindOf(KINDOF_FS_AIRFIELD);
		if (productionBehavior && isSupplyCenter &&
			!hasUsableSupplySource(
				info->getLocation(),
				plan->getTemplateGeometryInfo().getBoundingCircleRadius())) {
			if (hasOwnedSupplyCenter(plan) || hasQueuedSupplyCenter(plan) ||
				!isSupplyCenterPrerequisiteNeeded(plan))
				continue;
		}
		if (!critical || !info->isBuildable() ||
			!ShouldSkirmishAIConsiderRebuild(
				info->isAutomaticBuild(),
				info->isPriorityBuild(),
				plan->isKindOf(KINDOF_FS_POWER) && !plan->isKindOf(KINDOF_CASH_GENERATOR),
				isUnderPowered))
			continue;

		Object *object = TheGameLogic->findObjectByID(info->getObjectID());
		if (object && object->getControllingPlayer() == m_player)
			continue;

		Int cost = plan->calcCostToBuild(m_player);
		if (cost > 0 && (cheapestCost == 0 || cost < cheapestCost))
			cheapestCost = cost;
		if (canStartNow && !*canStartNow && canStartCriticalRebuildNow(info, plan))
			*canStartNow = true;
	}
	const Int activeRecoveryReserve = productionBehavior
		? getActiveRecoveryReserveCost() : m_recoveryReserveCost;
	const Bool recoveryReserveAvailable = productionBehavior
		? activeRecoveryReserve > 0 :
			!m_recoveryImpossible && m_recoveryReserveCost > 0;
	if (usesCriticalRecoveryBehavior() && m_recoveryEverCompleted &&
		recoveryReserveAvailable && activeRecoveryReserve > cheapestCost) {
		cheapestCost = activeRecoveryReserve;
		if (canStartNow)
			*canStartNow = true;
	}
	return cheapestCost;
}

Bool AISkirmishPlayer::canStartCriticalRebuildNow(
	BuildListInfo *info, const ThingTemplate *plan)
{
	Bool rebuildReady = info->getObjectTimestamp() == 0 ||
		info->getObjectTimestamp() +
			TheAI->getAiData()->m_rebuildDelaySeconds * LOGICFRAMES_PER_SECOND <=
			TheGameLogic->getFrame();
	if (rebuildReady)
		rebuildReady = isLocationSafe(info->getLocation(), plan);

	Object *dozer = findDozer(info->getLocation());
	Bool hasDozer = dozer != nullptr;
	Bool canMake = hasDozer && TheBuildAssistant->canMakeUnit(dozer, plan) == CANMAKE_OK;
	Bool hasAI = hasDozer && dozer->getAIUpdateInterface() != nullptr;
	Bool clearOfEnemies = false;
	if (canMake && hasAI && rebuildReady) {
		Coord3D position = *info->getLocation();
		position.z += TheTerrainLogic->getGroundHeight(position.x, position.y);
		clearOfEnemies = TheBuildAssistant->isLocationLegalToBuild(
			&position,
			plan,
			info->getAngle(),
			BuildAssistant::NO_ENEMY_OBJECT_OVERLAP,
			dozer,
			m_player) == LBC_OK;
		TheTerrainVisual->removeAllBibs();
	}
	return IsSkirmishAICriticalRebuildStartable(
		hasDozer, canMake, hasAI, rebuildReady, clearOfEnemies);
}

Bool AISkirmishPlayer::estimateTeamProduction(
	TeamPrototype *proto, Bool planned, Int *productionCost, Int *completionFrames)
{
	*productionCost = 0;
	*completionFrames = 0;
	std::vector<SkirmishFactoryProjection> factories;
	std::vector<Object *> ownedFactories;
	if (usesProductionBehavior())
		FindSkirmishAIProductionProducers(m_player, &ownedFactories);
	for (BuildListInfo *build = usesProductionBehavior()
			? nullptr : m_player->getBuildList();
		build; build = build->getNext()) {
		Object *factory = TheGameLogic->findObjectByID(build->getObjectID());
		if (!factory || factory->getControllingPlayer() != m_player ||
			factory->testStatus(OBJECT_STATUS_UNDER_CONSTRUCTION) ||
			factory->testStatus(OBJECT_STATUS_SOLD))
			continue;
		ProductionUpdateInterface *production = factory->getProductionUpdateInterface();
		if (!production)
			continue;

		Bool duplicate = false;
		for (std::vector<SkirmishFactoryProjection>::iterator existing = factories.begin();
			existing != factories.end(); ++existing) {
			if (existing->factory == factory) {
				duplicate = true;
				break;
			}
		}
		if (duplicate)
			continue;

		SkirmishFactoryProjection projection;
		projection.factory = factory;
		projection.projectedFrames = 0;
		projection.usedByCandidate = false;
		projection.idle = production->getProductionCount() <= 0;
		Bool firstEntry = true;
		for (const ProductionEntry *entry = production->firstProduction(); entry;
			entry = production->nextProduction(entry)) {
			projection.projectedFrames = AddSkirmishAIFrameValue(
				projection.projectedFrames,
				getSkirmishProductionEntryFrames(entry, m_player, firstEntry));
			firstEntry = false;
		}
		factories.push_back(projection);
	}
	for (std::vector<Object *>::const_iterator owned = ownedFactories.begin();
		owned != ownedFactories.end(); ++owned) {
		Object *factory = *owned;
		ProductionUpdateInterface *production =
			factory->getProductionUpdateInterface();
		SkirmishFactoryProjection projection;
		projection.factory = factory;
		projection.projectedFrames = 0;
		projection.usedByCandidate = false;
		Bool firstEntry = true;
		for (const ProductionEntry *entry = production->firstProduction(); entry;
			entry = production->nextProduction(entry)) {
			projection.projectedFrames = AddSkirmishAIFrameValue(
				projection.projectedFrames,
				getSkirmishProductionEntryFrames(entry, m_player, firstEntry));
			firstEntry = false;
		}
		factories.push_back(projection);
	}

	const TeamTemplateInfo *info = proto->getTemplateInfo();
	for (Int phase = 0; phase < (planned ? 2 : 1); ++phase) {
		for (Int i = info->m_numUnitsInfo - 1; i >= 0; --i) {
			const TCreateUnitsInfo *unitInfo = &info->m_unitsInfo[i];
			Int unitCount = phase == 0 ? unitInfo->minUnits :
				unitInfo->maxUnits - unitInfo->minUnits;
			if (unitCount <= 0)
				continue;
			const ThingTemplate *thing = TheThingFactory->findTemplate(unitInfo->unitThingName);
			if (!thing)
				continue;
			if (!appendSkirmishProductionOrder(
				factories, m_player, thing, unitCount, productionCost))
				return false;
		}
	}

	for (std::vector<SkirmishFactoryProjection>::iterator factory = factories.begin();
		factory != factories.end(); ++factory) {
		if (factory->usedByCandidate && factory->projectedFrames > *completionFrames)
			*completionFrames = factory->projectedFrames;
	}
	return true;
}

void AISkirmishPlayer::getVisibleEnemyComposition(
	Int *aircraftValue, Int *vehicleValue, Int *infantryValue,
	Coord3D *routeTarget, Bool *hasRouteTarget)
{
	getVisibleEnemyCompositionFor(getAiEnemy(), aircraftValue, vehicleValue,
		infantryValue, routeTarget, hasRouteTarget);
}

void AISkirmishPlayer::getVisibleEnemyCompositionFor(
	Player *enemy, Int *aircraftValue, Int *vehicleValue, Int *infantryValue,
	Coord3D *routeTarget, Bool *hasRouteTarget) const
{
	*aircraftValue = 0;
	*vehicleValue = 0;
	*infantryValue = 0;
	*hasRouteTarget = false;
	routeTarget->zero();
	if (!enemy)
		return;

	for (Object *object = TheGameLogic->getFirstObject(); object; object = object->getNextObject()) {
		if (object->getControllingPlayer() != enemy || object->isEffectivelyDead())
			continue;
		ObjectShroudStatus shroud = object->getShroudedStatus(m_player->getPlayerIndex());
		if (ShouldUseCurrentSkirmishAIBehavior()) {
			Bool visible = shroud == OBJECTSHROUD_CLEAR || shroud == OBJECTSHROUD_PARTIAL_CLEAR;
			Bool fogged = shroud == OBJECTSHROUD_FOGGED;
			if (!IsSkirmishAIIntelEligible(
				object->isKindOf(KINDOF_STRUCTURE),
				visible,
				fogged,
				object->testStatus(OBJECT_STATUS_STEALTHED),
				object->testStatus(OBJECT_STATUS_DETECTED),
				object->testStatus(OBJECT_STATUS_MASKED)))
				continue;
		} else {
			if (shroud != OBJECTSHROUD_CLEAR && shroud != OBJECTSHROUD_PARTIAL_CLEAR)
				continue;
			if (object->testStatus(OBJECT_STATUS_STEALTHED) &&
				!object->testStatus(OBJECT_STATUS_DETECTED))
				continue;
		}
		if (object->isKindOf(KINDOF_STRUCTURE))
			continue;

		Int value = object->getTemplate()->calcCostToBuild(enemy);
		if (object->isKindOf(KINDOF_AIRCRAFT))
			*aircraftValue = AddSkirmishAICostValue(*aircraftValue, value, 1);
		else if (object->isKindOf(KINDOF_VEHICLE))
			*vehicleValue = AddSkirmishAICostValue(*vehicleValue, value, 1);
		else if (object->isKindOf(KINDOF_INFANTRY))
			*infantryValue = AddSkirmishAICostValue(*infantryValue, value, 1);
		else
			continue;
		if (!*hasRouteTarget && IsSkirmishAIGroundRouteTarget(
			object->isKindOf(KINDOF_STRUCTURE),
			object->isKindOf(KINDOF_AIRCRAFT),
			object->isKindOf(KINDOF_VEHICLE),
			object->isKindOf(KINDOF_INFANTRY))) {
			*routeTarget = *object->getPosition();
			*hasRouteTarget = true;
		}
	}
}

Int AISkirmishPlayer::getCandidateCounterFit(
	TeamPrototype *proto, Int aircraftValue, Int vehicleValue, Int infantryValue)
{
	Int plannedValue = 0;
	Int antiAircraftValue = 0;
	Int antiVehicleValue = 0;
	Int antiInfantryValue = 0;
	const TeamTemplateInfo *info = proto->getTemplateInfo();
	for (Int i = 0; i < info->m_numUnitsInfo; ++i) {
		const TCreateUnitsInfo *unitInfo = &info->m_unitsInfo[i];
		const ThingTemplate *thing = TheThingFactory->findTemplate(unitInfo->unitThingName);
		if (!thing || unitInfo->maxUnits <= 0)
			continue;
		Int value = AddSkirmishAICostValue(0, thing->calcCostToBuild(m_player), unitInfo->maxUnits);
		plannedValue = AddSkirmishAICostValue(plannedValue, value, 1);

		Bool attacksAircraft = false;
		Bool attacksGround = false;
		Bool prefersVehicle = false;
		Bool prefersInfantry = false;
		WeaponSetFlags flags;
		flags.clear();
		const WeaponTemplateSet *weaponSet = thing->findWeaponTemplateSet(flags);
		if (weaponSet) {
			for (Int slot = 0; slot < WEAPONSLOT_COUNT; ++slot) {
				const WeaponTemplate *weapon = weaponSet->getNth((WeaponSlotType)slot);
				if (!weapon)
					continue;
				Int antiMask = weapon->getAntiMask();
				if (antiMask & (WEAPON_ANTI_AIRBORNE_VEHICLE | WEAPON_ANTI_AIRBORNE_INFANTRY))
					attacksAircraft = true;
				if (antiMask & WEAPON_ANTI_GROUND)
					attacksGround = true;
				const KindOfMaskType &preferred = weaponSet->getNthPreferredAgainstMask((WeaponSlotType)slot);
				if (preferred.test(KINDOF_AIRCRAFT))
					attacksAircraft = true;
				if (preferred.test(KINDOF_VEHICLE))
					prefersVehicle = true;
				if (preferred.test(KINDOF_INFANTRY))
					prefersInfantry = true;
			}
		}
		if (attacksAircraft)
			antiAircraftValue = AddSkirmishAICostValue(antiAircraftValue, value, 1);
		if (attacksGround || prefersVehicle)
			antiVehicleValue = AddSkirmishAICostValue(antiVehicleValue, value, 1);
		if (attacksGround || prefersInfantry)
			antiInfantryValue = AddSkirmishAICostValue(antiInfantryValue, value, 1);
	}
	return GetSkirmishAICounterFitScore(
		aircraftValue, vehicleValue, infantryValue,
		plannedValue, antiAircraftValue, antiVehicleValue, antiInfantryValue);
}

SkirmishAIRouteClass AISkirmishPlayer::classifyTeamRoute(
	TeamPrototype *proto, const Coord3D *routeTarget, Bool hasRouteTarget)
{
	Bool hasGround = false;
	Bool hasAir = false;
	const TeamTemplateInfo *info = proto->getTemplateInfo();
	for (Int i = 0; i < info->m_numUnitsInfo; ++i) {
		if (info->m_unitsInfo[i].maxUnits <= 0)
			continue;
		const ThingTemplate *thing = TheThingFactory->findTemplate(info->m_unitsInfo[i].unitThingName);
		if (!thing)
			continue;
		if (thing->isKindOf(KINDOF_AIRCRAFT))
			hasAir = true;
		else if (thing->isKindOf(KINDOF_VEHICLE) || thing->isKindOf(KINDOF_INFANTRY))
			hasGround = true;
	}
	if (!hasGround || !hasRouteTarget)
		return SKIRMISH_AI_ROUTE_UNKNOWN;

	Object *representative = nullptr;
	for (Object *object = TheGameLogic->getFirstObject(); object; object = object->getNextObject()) {
		if (object->getControllingPlayer() == m_player && !object->isEffectivelyDead() &&
			!object->isKindOf(KINDOF_STRUCTURE) && !object->isKindOf(KINDOF_AIRCRAFT) &&
			object->getAIUpdateInterface()) {
			representative = object;
			break;
		}
	}
	if (!representative)
		return SKIRMISH_AI_ROUTE_UNKNOWN;
	if (TheAI->pathfinder()->clientSafeQuickDoesPathExist(
		representative->getAIUpdateInterface()->getLocomotorSet(),
		representative->getPosition(), routeTarget))
		return SKIRMISH_AI_ROUTE_GROUND_REACHABLE;
	return hasAir ? SKIRMISH_AI_ROUTE_MIXED_UNREACHABLE : SKIRMISH_AI_ROUTE_GROUND_UNREACHABLE;
}

Bool AISkirmishPlayer::getKnownEnemyPosition(Player *enemy, Coord3D *position) const
{
	if (!enemy || !position)
		return false;
	Int slotIndex = ThePlayerList->getSlotIndex(enemy->getPlayerIndex());
	if (TheGameInfo && slotIndex >= 0) {
		const GameSlot *slot = TheGameInfo->getConstSlot(slotIndex);
		if (slot && slot->getStartPos() >= 0) {
			AsciiString waypointName;
			waypointName.format("Player_%d_Start", slot->getStartPos() + 1);
			Waypoint *waypoint = TheTerrainLogic->getWaypointByName(waypointName);
			if (waypoint) {
				*position = *waypoint->getLocation();
				return true;
			}
		}
	}

	Object *knownObject = nullptr;
	for (Object *object = TheGameLogic->getFirstObject(); object; object = object->getNextObject()) {
		if (object->getControllingPlayer() != enemy || object->isEffectivelyDead())
			continue;
		ObjectShroudStatus shroud = object->getShroudedStatus(m_player->getPlayerIndex());
		Bool visible = shroud == OBJECTSHROUD_CLEAR || shroud == OBJECTSHROUD_PARTIAL_CLEAR;
		Bool fogged = shroud == OBJECTSHROUD_FOGGED;
		if (!IsSkirmishAIIntelEligible(
			object->isKindOf(KINDOF_STRUCTURE),
			visible,
			fogged,
			object->testStatus(OBJECT_STATUS_STEALTHED),
			object->testStatus(OBJECT_STATUS_DETECTED),
			object->testStatus(OBJECT_STATUS_MASKED)))
			continue;
		if (!knownObject || object->getID() < knownObject->getID())
			knownObject = object;
	}
	if (!knownObject)
		return false;
	*position = *knownObject->getPosition();
	return true;
}

Int AISkirmishPlayer::getKnownEnemyAssetValue(Player *enemy, Bool *hasKnownObject,
	Bool *hasKnownUnit, Bool *hasKnownBuildFacility) const
{
	Int value = 0;
	*hasKnownObject = false;
	*hasKnownUnit = false;
	*hasKnownBuildFacility = false;
	for (Object *object = TheGameLogic->getFirstObject(); object; object = object->getNextObject()) {
		if (object->getControllingPlayer() != enemy || object->isEffectivelyDead())
			continue;
		ObjectShroudStatus shroud = object->getShroudedStatus(m_player->getPlayerIndex());
		Bool visible = shroud == OBJECTSHROUD_CLEAR || shroud == OBJECTSHROUD_PARTIAL_CLEAR;
		Bool fogged = shroud == OBJECTSHROUD_FOGGED;
		if (!IsSkirmishAIIntelEligible(
			object->isKindOf(KINDOF_STRUCTURE),
			visible,
			fogged,
			object->testStatus(OBJECT_STATUS_STEALTHED),
			object->testStatus(OBJECT_STATUS_DETECTED),
			object->testStatus(OBJECT_STATUS_MASKED)))
			continue;
		*hasKnownObject = true;
		if (!object->isKindOf(KINDOF_STRUCTURE) &&
			!object->isKindOf(KINDOF_PROJECTILE) &&
			!object->isKindOf(KINDOF_MINE))
			*hasKnownUnit = true;
		if (object->getTemplate()->isBuildFacility())
			*hasKnownBuildFacility = true;
		value = AddSkirmishAICostValue(value, object->getTemplate()->calcCostToBuild(enemy), 1);
	}
	return value;
}

Object *AISkirmishPlayer::findEnemyRouteRepresentative() const
{
	Object *representative = nullptr;
	for (Object *object = TheGameLogic->getFirstObject(); object; object = object->getNextObject()) {
		if (object->getControllingPlayer() != m_player || object->isEffectivelyDead() ||
			object->isKindOf(KINDOF_STRUCTURE) || object->isKindOf(KINDOF_AIRCRAFT) ||
			!object->getAIUpdateInterface())
			continue;
		if (!representative || object->getID() < representative->getID())
			representative = object;
	}
	return representative;
}

SkirmishAITargetRouteClass AISkirmishPlayer::classifyEnemyRoute(
	Object *representative, const Coord3D *enemyPosition, Bool hasEnemyPosition) const
{
	if (!representative || !hasEnemyPosition || !enemyPosition || !TheAI || !TheAI->pathfinder())
		return SKIRMISH_AI_TARGET_ROUTE_UNKNOWN;
	AIUpdateInterface *ai = representative->getAIUpdateInterface();
	if (!ai)
		return SKIRMISH_AI_TARGET_ROUTE_UNKNOWN;

	Coord3D targets[5];
	targets[0] = *enemyPosition;
	Real dx = representative->getPosition()->x - enemyPosition->x;
	Real dy = representative->getPosition()->y - enemyPosition->y;
	Real length = sqrt(dx * dx + dy * dy);
	Real xDirection = 1.0f;
	Real yDirection = 0.0f;
	if (length > 1.0f) {
		xDirection = dx / length;
		yDirection = dy / length;
	}
	const Real approachDistance = 200.0f;
	targets[1] = *enemyPosition;
	targets[1].x += xDirection * approachDistance;
	targets[1].y += yDirection * approachDistance;
	targets[2] = *enemyPosition;
	targets[2].x -= yDirection * approachDistance;
	targets[2].y += xDirection * approachDistance;
	targets[3] = *enemyPosition;
	targets[3].x += yDirection * approachDistance;
	targets[3].y -= xDirection * approachDistance;
	targets[4] = *enemyPosition;
	targets[4].x -= xDirection * approachDistance;
	targets[4].y -= yDirection * approachDistance;

	for (Int i = 0; i < 5; ++i) {
		targets[i].z = TheTerrainLogic->getGroundHeight(targets[i].x, targets[i].y);
		if (TheAI->pathfinder()->clientSafeQuickDoesPathExist(
			ai->getLocomotorSet(), representative->getPosition(), &targets[i]))
			return SKIRMISH_AI_TARGET_ROUTE_REACHABLE;
	}
	return SKIRMISH_AI_TARGET_ROUTE_UNREACHABLE;
}

Int AISkirmishPlayer::countAlliedSkirmishAIsTargeting(Player *enemy) const
{
	Int count = 0;
	for (Int i = 0; i < ThePlayerList->getPlayerCount(); ++i) {
		Player *other = ThePlayerList->getNthPlayer(i);
		if (!other || other == m_player || !other->isSkirmishAIPlayer())
			continue;
		if (m_player->getRelationship(other->getDefaultTeam()) != ALLIES)
			continue;
		if (other->getCachedCurrentEnemy() == enemy)
			++count;
	}
	return count;
}

SkirmishAIDecisionDifficulty AISkirmishPlayer::getDecisionDifficulty() const
{
	if (m_difficulty == DIFFICULTY_EASY)
		return SKIRMISH_AI_DIFFICULTY_EASY;
	if (m_difficulty == DIFFICULTY_NORMAL)
		return SKIRMISH_AI_DIFFICULTY_NORMAL;
	return SKIRMISH_AI_DIFFICULTY_HARD;
}

#if defined(_WIN64)
static Bool ShouldUseCounterBasedSkirmishAIPlanning()
{
	return TheGameLogic && ShouldUseSkirmishAICounterRng(
		TheGameLogic->isInReplayGame(),
		TheRecorder ? TheRecorder->getSkirmishAIReplayEpoch() : SKIRMISH_AI_REPLAY_EPOCH_LEGACY);
}

#endif

#if defined(_WIN64)
Bool AISkirmishPlayer::isEnemyPlanningDue() const
{
	Bool currentEnemyInvalid = m_currentEnemy &&
		(m_player->getRelationship(m_currentEnemy->getDefaultTeam()) != ENEMIES ||
		 !(usesProductionBehavior()
			? m_currentEnemy->hasOffensiveTargetableObjects()
			: m_currentEnemy->hasAnyObjects()));
	return ShouldEvaluateSkirmishAITarget(currentEnemyInvalid,
		TheGameLogic->getFrame(), m_frameToCheckEnemy, true);
}

//-------------------------------------------------------------------------------------------------
/** Capture target facts in PlayerList order. Path, shroud, and object reads remain owner-only. */
//-------------------------------------------------------------------------------------------------
Bool AISkirmishPlayer::captureEnemyPlanningSnapshot(
	rts::AIEnemyPlanningSnapshot *snapshot ) const
{
	if (!snapshot)
		return false;
	rts::ClearAIEnemyPlanningSnapshot(snapshot);
	snapshot->frame = TheGameLogic->getFrame();
	snapshot->ownerPlayerIndex = (UnsignedInt)m_player->getPlayerIndex();
	snapshot->currentEnemyPlayerIndex = m_currentEnemy ? m_currentEnemy->getPlayerIndex() : -1;
	snapshot->switchScoreThreshold = 200;

	Object *representative = findEnemyRouteRepresentative();
	for (Int i = 0; i < ThePlayerList->getPlayerCount(); ++i)
	{
		Player *candidatePlayer = ThePlayerList->getNthPlayer(i);
		if (!candidatePlayer ||
			m_player->getRelationship(candidatePlayer->getDefaultTeam()) != ENEMIES ||
			!(usesProductionBehavior()
				? candidatePlayer->hasOffensiveTargetableObjects()
				: candidatePlayer->hasAnyObjects()))
		{
			continue;
		}
		if (snapshot->candidateCount >= rts::AI_PLANNING_MAX_PLAYERS)
			return false;

		rts::AIEnemyCandidateFact &fact = snapshot->candidates[snapshot->candidateCount++];
		fact.sourceOrdinal = (UnsignedInt)i;
		fact.playerIndex = candidatePlayer->getPlayerIndex();
		Bool hasKnownObject = false;
		Bool hasKnownUnit = false;
		Bool hasKnownBuildFacility = false;
		fact.knownAssetValue = getKnownEnemyAssetValue(candidatePlayer,
			&hasKnownObject, &hasKnownUnit, &hasKnownBuildFacility);
		fact.hasKnownObject = hasKnownObject ? 1 : 0;
		fact.hasKnownUnit = hasKnownUnit ? 1 : 0;
		fact.hasKnownBuildFacility = hasKnownBuildFacility ? 1 : 0;

		Coord3D knownPosition = m_baseCenter;
		Bool hasKnownPosition = getKnownEnemyPosition(candidatePlayer, &knownPosition);
		fact.hasKnownPosition = hasKnownPosition ? 1 : 0;
		fact.distance = 0;
		if (hasKnownPosition)
		{
			double dx = (double)knownPosition.x - (double)m_baseCenter.x;
			double dy = (double)knownPosition.y - (double)m_baseCenter.y;
			double distance = sqrt(dx * dx + dy * dy);
			fact.distance = distance >= 2147483647.0 ? 2147483647 : (Int)(distance + 0.5);
		}
		fact.routeClass = (Int)classifyEnemyRoute(
			representative, &knownPosition, hasKnownPosition);
		fact.targetingThisAI = candidatePlayer->getCachedCurrentEnemy() == m_player ? 1 : 0;
		fact.alliedAIsTargeting = countAlliedSkirmishAIsTargeting(candidatePlayer);
	}
	return true;
}

//-------------------------------------------------------------------------------------------------
/** Resolve a target ID without publishing it. */
//-------------------------------------------------------------------------------------------------
Bool AISkirmishPlayer::resolveEnemyPlanningCommit(
	const rts::AIEnemyPlanningSnapshot &snapshot,
	const rts::AIEnemyPlanningResult &result,
	Player **resolvedEnemy ) const
{
	if (!resolvedEnemy || !validateEnemyPlanningCommit(snapshot, result))
		return false;

	*resolvedEnemy = nullptr;
	if (result.selectedPlayerIndex >= 0)
	{
		for (Int i = 0; i < ThePlayerList->getPlayerCount(); ++i)
		{
			Player *candidate = ThePlayerList->getNthPlayer(i);
			if (candidate && candidate->getPlayerIndex() == result.selectedPlayerIndex)
			{
				*resolvedEnemy = candidate;
				break;
			}
		}
	}
	return result.selectedPlayerIndex < 0 || *resolvedEnemy != nullptr;
}

void AISkirmishPlayer::applyEnemyPlanningCommit(Player *resolvedEnemy)
{
	m_frameToCheckEnemy = TheGameLogic->getFrame() + 5*LOGICFRAMES_PER_SECOND;

	if (resolvedEnemy != m_currentEnemy)
	{
		m_currentEnemy = resolvedEnemy;
		m_currentEnemyPlayerIndex = m_currentEnemy ? m_currentEnemy->getPlayerIndex() : -1;
		if (m_currentEnemy)
		{
			AsciiString message = TheNameKeyGenerator->keyToName(m_player->getPlayerNameKey());
			message.concat(" acquiring target enemy player: ");
			message.concat(TheNameKeyGenerator->keyToName(m_currentEnemy->getPlayerNameKey()));
			TheScriptEngine->AppendDebugMessage(message, false);
		}
	}
}

Bool AISkirmishPlayer::validateEnemyPlanningCommit(
	const rts::AIEnemyPlanningSnapshot &snapshot,
	const rts::AIEnemyPlanningResult &result ) const
{
	// ExecuteAIPlanningBatch performs the single canonical numeric validation
	// before this owner commit. Keep this boundary structural and live-state
	// aware so normal mode does not recompute the same enemy oracle twice.
	if (!result.valid || snapshot.frame != TheGameLogic->getFrame() ||
		snapshot.ownerPlayerIndex != (UnsignedInt)m_player->getPlayerIndex() ||
		result.orderKey.frame != snapshot.frame ||
		result.orderKey.playerIndex != snapshot.ownerPlayerIndex ||
		result.orderKey.subphase != rts::AI_PLANNING_SUBPHASE_ENEMY_TARGET ||
		result.orderKey.emissionOrdinal != 0U ||
		snapshot.candidateCount > rts::AI_PLANNING_MAX_PLAYERS)
		return false;
	if (result.selectedPlayerIndex < 0)
		return result.selectedPlayerIndex == -1 &&
			result.orderKey.sourceOrdinal == rts::AI_PLANNING_INVALID_ORDINAL;

	Bool snapshotMember = false;
	for (UnsignedInt i = 0; i < snapshot.candidateCount; ++i)
	{
		if (snapshot.candidates[i].playerIndex == result.selectedPlayerIndex &&
			snapshot.candidates[i].sourceOrdinal == result.orderKey.sourceOrdinal)
		{
			snapshotMember = true;
			break;
		}
	}
	if (!snapshotMember)
		return false;
	Player *candidate = ThePlayerList->getNthPlayer((Int)result.orderKey.sourceOrdinal);
	return candidate && candidate->getPlayerIndex() == result.selectedPlayerIndex &&
		m_player->getRelationship(candidate->getDefaultTeam()) == ENEMIES &&
		(usesProductionBehavior()
			? candidate->hasOffensiveTargetableObjects()
			: candidate->hasAnyObjects());
}

//-------------------------------------------------------------------------------------------------
/**
 * Capture the complete immutable production source view on the owner thread.
 * The old implementation called the factory, queue, template, weapon, and
 * path helpers once per candidate. This adapter performs each live traversal
 * once, stores only bounded POD facts, and lets the shared planner shard the
 * expensive candidate analysis without exposing live objects to workers.
 */
//-------------------------------------------------------------------------------------------------
Bool AISkirmishPlayer::captureAdaptiveProductionCandidateFacts(
	rts::AIProductionPlanningSnapshot *snapshot,
	const rts::AICounterRngKey &baseRandomKey,
	Int *highestPriority,
	Bool *overflowed )
{
	if (!snapshot || !highestPriority || !overflowed)
		return false;
	*overflowed = false;
	rts::ClearAIProductionPlanningSnapshot(snapshot);
	snapshot->frame = TheGameLogic->getFrame();
	snapshot->ownerPlayerIndex = (UnsignedInt)m_player->getPlayerIndex();
	snapshot->tieBreakKey = baseRandomKey;
	snapshot->tieBreakKey.frame = snapshot->frame;
	snapshot->tieBreakKey.domain = rts::AI_COUNTER_RNG_DOMAIN_PLAYER_PLANNING;
	snapshot->tieBreakKey.playerIndex = snapshot->ownerPlayerIndex;
	snapshot->tieBreakKey.ownerStableId = snapshot->ownerPlayerIndex;
	snapshot->tieBreakKey.sourceStableId = 0;
	snapshot->tieBreakKey.eventKind = rts::AI_COUNTER_RNG_EVENT_PRODUCTION_TIE;
	snapshot->tieBreakKey.eventOrdinal = 0;
	snapshot->tieBreakKey.drawOrdinal = 0;

	std::vector<SkirmishFactoryProjection> factories;
	for (BuildListInfo *build = m_player->getBuildList(); build;
		build = build->getNext())
	{
		Object *factory = TheGameLogic->findObjectByID(build->getObjectID());
		if (!factory || factory->getControllingPlayer() != m_player ||
			factory->testStatus(OBJECT_STATUS_UNDER_CONSTRUCTION) ||
			factory->testStatus(OBJECT_STATUS_SOLD))
			continue;
		ProductionUpdateInterface *production =
			factory->getProductionUpdateInterface();
		if (!production)
			continue;
		Bool duplicate = false;
		for (std::vector<SkirmishFactoryProjection>::const_iterator existing =
			factories.begin(); existing != factories.end(); ++existing)
		{
			if (existing->factory == factory)
			{
				duplicate = true;
				break;
			}
		}
		if (duplicate)
			continue;
		if (factories.size() >= rts::AI_PLANNING_MAX_PRODUCTION_FACTORIES)
		{
			*overflowed = true;
			return false;
		}
		SkirmishFactoryProjection projection;
		projection.factory = factory;
		projection.projectedFrames = 0;
		projection.usedByCandidate = false;
		projection.idle = production->getProductionCount() <= 0;
		Bool firstEntry = true;
		for (const ProductionEntry *entry = production->firstProduction(); entry;
			entry = production->nextProduction(entry))
		{
			projection.projectedFrames = AddSkirmishAIFrameValue(
				projection.projectedFrames,
				getSkirmishProductionEntryFrames(entry, m_player, firstEntry));
			firstEntry = false;
		}
		factories.push_back(projection);
	}
	snapshot->sourceFacts.factoryCount = (UnsignedInt)factories.size();
	for (UnsignedInt factory = 0U; factory < snapshot->sourceFacts.factoryCount;
		++factory)
	{
		snapshot->sourceFacts.factories[factory].projectedFrames =
			factories[factory].projectedFrames;
		snapshot->sourceFacts.factories[factory].valid = 1U;
		snapshot->sourceFacts.factories[factory].idle =
			factories[factory].idle ? 1U : 0U;
	}

	// Enemy composition and the route target are one owner-side object scan for
	// the whole batch. A second single representative query supplies the shared
	// ground-route fact; neither is repeated per candidate.
	Coord3D routeTarget;
	Bool hasRouteTarget = false;
	getVisibleEnemyCompositionFor(m_currentEnemy,
		&snapshot->sourceFacts.enemyAircraftValue,
		&snapshot->sourceFacts.enemyVehicleValue,
		&snapshot->sourceFacts.enemyInfantryValue,
		&routeTarget, &hasRouteTarget);
	snapshot->sourceFacts.hasRouteTarget = hasRouteTarget ? 1U : 0U;
	Object *representative = nullptr;
	for (Object *object = TheGameLogic->getFirstObject(); object;
		object = object->getNextObject())
	{
		if (object->getControllingPlayer() == m_player &&
			!object->isEffectivelyDead() &&
			!object->isKindOf(KINDOF_STRUCTURE) &&
			!object->isKindOf(KINDOF_AIRCRAFT) &&
			object->getAIUpdateInterface())
		{
			representative = object;
			break;
		}
	}
	if (representative && hasRouteTarget && representative->getAIUpdateInterface())
	{
		snapshot->sourceFacts.groundRouteKnown = 1U;
		snapshot->sourceFacts.groundRouteReachable =
			TheAI->pathfinder()->clientSafeQuickDoesPathExist(
				representative->getAIUpdateInterface()->getLocomotorSet(),
				representative->getPosition(), &routeTarget) ? 1U : 0U;
	}

	const TeamPrototype *queuedPrototypes[
		rts::AI_PLANNING_MAX_PRODUCTION_CANDIDATES];
	UnsignedInt queuedCount = 0U;
	for (DLINK_ITERATOR<TeamInQueue> iter = iterate_TeamBuildQueue();
		!iter.done(); iter.advance())
	{
		TeamInQueue *team = iter.cur();
		if (!team || !team->m_team || !team->m_team->getPrototype())
			continue;
		if (queuedCount >= rts::AI_PLANNING_MAX_PRODUCTION_CANDIDATES)
		{
			*overflowed = true;
			return false;
		}
		queuedPrototypes[queuedCount++] = team->m_team->getPrototype();
	}

	Bool hasCandidate = false;
	*highestPriority = (-2147483647 - 1);
	UnsignedInt sourceOrdinal = 0;
	Player::PlayerTeamList::const_iterator teamIt;
	for (teamIt = m_player->getPlayerTeams()->begin();
		teamIt != m_player->getPlayerTeams()->end(); ++teamIt, ++sourceOrdinal)
	{
		TeamPrototype *prototype = *teamIt;
		if (!prototype || !prototype->evaluateProductionCondition() ||
			prototype->countTeamInstances() >=
				prototype->getTemplateInfo()->m_maxInstances)
			continue;
		Bool queued = false;
		for (UnsignedInt queuedIndex = 0U; queuedIndex < queuedCount;
			++queuedIndex)
		{
			if (queuedPrototypes[queuedIndex] == prototype)
			{
				queued = true;
				break;
			}
		}
		if (queued)
			continue;
		if (rts::RequiresAIProductionOwnerSerialFallback(
			snapshot->candidateCount + 1U))
		{
			*overflowed = true;
			return false;
		}

		const TeamTemplateInfo *info = prototype->getTemplateInfo();
		rts::AIProductionCandidateSourceFact &source =
			snapshot->sourceFacts.candidates[snapshot->candidateCount];
		memset(&source, 0, sizeof(source));
		Bool hasUnit = false;
		Bool candidateFactoriesAvailable = true;
		Bool anyIdleFactory = false;
		for (Int unitIndex = 0; unitIndex < info->m_numUnitsInfo; ++unitIndex)
		{
			const TCreateUnitsInfo *unitInfo = &info->m_unitsInfo[unitIndex];
			const ThingTemplate *thing =
				TheThingFactory->findTemplate(unitInfo->unitThingName);
			if (!thing)
				continue;
			if (source.unitCount >= rts::AI_PLANNING_MAX_PRODUCTION_UNITS)
			{
				*overflowed = true;
				return false;
			}
			rts::AIProductionUnitSourceFact &unit =
				source.units[source.unitCount++];
			hasUnit = true;
			unit.cost = thing->calcCostToBuild(m_player);
			unit.buildFrames = thing->calcTimeToBuild(m_player);
			unit.minUnits = usesProductionBehavior()
				? unitInfo->maxUnits : unitInfo->minUnits;
			unit.maxUnits = unitInfo->maxUnits;
			if (thing->isKindOf(KINDOF_AIRCRAFT))
				unit.flags |= rts::AI_PRODUCTION_SOURCE_AIRCRAFT;
			if (thing->isKindOf(KINDOF_VEHICLE))
				unit.flags |= rts::AI_PRODUCTION_SOURCE_VEHICLE;
			if (thing->isKindOf(KINDOF_INFANTRY))
				unit.flags |= rts::AI_PRODUCTION_SOURCE_INFANTRY;
			WeaponSetFlags weaponFlags;
			weaponFlags.clear();
			const WeaponTemplateSet *weaponSet =
				thing->findWeaponTemplateSet(weaponFlags);
			if (weaponSet)
			{
				for (Int slot = 0; slot < WEAPONSLOT_COUNT; ++slot)
				{
					const WeaponTemplate *weapon =
						weaponSet->getNth((WeaponSlotType)slot);
					if (weapon && (weapon->getAntiMask() &
						(WEAPON_ANTI_AIRBORNE_VEHICLE |
						 WEAPON_ANTI_AIRBORNE_INFANTRY)))
						unit.flags |= rts::AI_PRODUCTION_SOURCE_ATTACKS_AIRCRAFT;
					if (weapon && (weapon->getAntiMask() & WEAPON_ANTI_GROUND))
						unit.flags |= rts::AI_PRODUCTION_SOURCE_ATTACKS_GROUND;
					if (!weapon)
						continue;
					const KindOfMaskType &preferred =
						weaponSet->getNthPreferredAgainstMask((WeaponSlotType)slot);
					if (preferred.test(KINDOF_AIRCRAFT))
						unit.flags |= rts::AI_PRODUCTION_SOURCE_ATTACKS_AIRCRAFT;
					if (preferred.test(KINDOF_VEHICLE))
						unit.flags |= rts::AI_PRODUCTION_SOURCE_PREFERS_VEHICLE;
					if (preferred.test(KINDOF_INFANTRY))
						unit.flags |= rts::AI_PRODUCTION_SOURCE_PREFERS_INFANTRY;
				}
			}
			for (UnsignedInt factory = 0U;
				factory < snapshot->sourceFacts.factoryCount; ++factory)
			{
				if (!TheBuildAssistant->isPossibleToMakeUnit(
					factories[factory].factory, thing))
					continue;
				unit.compatibleFactoryMask |= 1U << factory;
				Int quantity = ProductionUpdate::getProductionQuantityForUnitFromObject(
					factories[factory].factory, thing);
				if (quantity < 1)
					quantity = 1;
				if (quantity > 255)
				{
					*overflowed = true;
					return false;
				}
				unit.productionQuantity[factory] = (unsigned char)quantity;
				if (factories[factory].idle)
					anyIdleFactory = true;
			}
			if (unit.compatibleFactoryMask == 0U)
				candidateFactoriesAvailable = false;
		}
		if (!hasUnit || !candidateFactoriesAvailable || !anyIdleFactory)
			continue;

		source.valid = 1U;
		rts::AIProductionCandidateFact &fact =
			snapshot->candidates[snapshot->candidateCount++];
		fact.sourceOrdinal = sourceOrdinal;
		fact.candidateStableId = (UnsignedInt)prototype->getID();
		fact.configuredPriority = prototype->getTemplateInfo()->m_productionPriority;
		fact.eligible = 1;
		if (ShouldReplaceSkirmishAIHighestPriority(
			hasCandidate, fact.configuredPriority, *highestPriority))
		{
			*highestPriority = fact.configuredPriority;
		}
		hasCandidate = true;
	}
	snapshot->sourceFacts.valid = 1U;
	return true;
}

//-------------------------------------------------------------------------------------------------
/** Finish small owner state capture after the immutable source view is ready. */
//-------------------------------------------------------------------------------------------------
Bool AISkirmishPlayer::finishAdaptiveProductionPlanningSnapshot(
	rts::AIProductionPlanningSnapshot *snapshot )
{
	if (!snapshot || snapshot->candidateCount > rts::AI_PLANNING_MAX_PRODUCTION_CANDIDATES)
		return false;

	Bool criticalRebuildCanStart = false;
	Int rebuildReserve = getCriticalRebuildReserve(&criticalRebuildCanStart);
	Int poorReserve = TheAI->getAiData()->m_resourcesPoor;
	snapshot->initialReserve = usesProductionBehavior()
		? GetSkirmishAIAggregateReserve(poorReserve, rebuildReserve,
			getActiveRecoveryReserveCost())
		: GetSkirmishAIReserve(poorReserve, rebuildReserve);
	snapshot->retryReserve = usesProductionBehavior()
		? snapshot->initialReserve : GetSkirmishAIReserve(poorReserve, 0);
	if (usesProductionBehavior())
		m_strategyProductionReserveCost = snapshot->initialReserve;
	Bool rebuildReserveApplied = snapshot->initialReserve > snapshot->retryReserve;
	snapshot->retryWithoutInitialReserve =
		!usesProductionBehavior() && rebuildReserveApplied &&
		!criticalRebuildCanStart ? 1 : 0;
	snapshot->resources = m_player->getMoney()->countMoney();
	snapshot->logicFramesPerSecond = LOGICFRAMES_PER_SECOND;
	SkirmishAIDecisionDifficulty difficulty = getDecisionDifficulty();
	snapshot->difficulty = (Int)difficulty;
	snapshot->contextInfluencePercent = GetSkirmishAIContextInfluencePercent(difficulty);

	// Source facts already contain the one-time enemy/object/nav observations and
	// each candidate's immutable unit/weapon/factory facts. Feedback counters are
	// owner state, so copy them once here without reopening any per-candidate
	// live traversal.
	UnsignedInt sourceOrdinal = 0U;
	UnsignedInt candidateIndex = 0U;
	Player::PlayerTeamList::const_iterator teamIt;
	for (teamIt = m_player->getPlayerTeams()->begin();
		teamIt != m_player->getPlayerTeams()->end() &&
			candidateIndex < snapshot->candidateCount;
		++teamIt, ++sourceOrdinal)
	{
		rts::AIProductionCandidateFact &fact =
			snapshot->candidates[candidateIndex];
		if (fact.sourceOrdinal != sourceOrdinal)
			continue;
		TeamPrototype *prototype = *teamIt;
		if (!prototype || (UnsignedInt)prototype->getID() !=
			fact.candidateStableId ||
			!snapshot->sourceFacts.candidates[candidateIndex].valid)
			return false;
		fact.recentLossCount = prototype->getRecentSkirmishAILossCount();
		fact.recentPathFailureCount = prototype->getRecentSkirmishAIPathFailureCount();
		++candidateIndex;
	}
	return candidateIndex == snapshot->candidateCount;
}

Bool AISkirmishPlayer::prepareAdaptiveProductionPlanningSnapshot(
	rts::AIProductionPlanningSnapshot *snapshot,
	const rts::AICounterRngKey &baseRandomKey,
	Bool *handled,
	Bool *overflowed)
{
	if (!snapshot || !handled || !overflowed)
		return false;
	*handled = false;
	*overflowed = false;

	// Feedback decay is an owner mutation and must happen once, before the
	// immutable candidate view is handed to the shared planner.
	Player::PlayerTeamList::const_iterator teamIt;
	for (teamIt = m_player->getPlayerTeams()->begin();
		teamIt != m_player->getPlayerTeams()->end(); ++teamIt)
	{
		(*teamIt)->decaySkirmishAIFeedback(TheGameLogic->getFrame());
	}

	Int highestPriority = (-2147483647 - 1);
	if (!captureAdaptiveProductionCandidateFacts(
		snapshot, baseRandomKey, &highestPriority, overflowed))
		return false;

	// Preserve the legacy reinforcement decision boundary before admitting the
	// expensive production context to the worker batch.
	if (selectTeamToReinforce(highestPriority))
	{
		// The retail process path queues existing work again after a successful
		// reinforcement selection.  Keep that owner-side pass before marking the
		// batched production boundary handled.
		queueUnits();
		markProductionPlanningHandled();
		*handled = true;
		return true;
	}
	if (snapshot->candidateCount == 0U)
	{
		markProductionPlanningHandled();
		*handled = true;
		return true;
	}
	return finishAdaptiveProductionPlanningSnapshot(snapshot);
}

//-------------------------------------------------------------------------------------------------
/** Rare overflow lane: preserve epoch-12 scoring and counter-RNG semantics without a fixed array. */
//-------------------------------------------------------------------------------------------------
Bool AISkirmishPlayer::selectTeamToBuildCounterSerialFallback(
	const rts::AICounterRngKey &randomKey )
{
	Bool hasCandidate = false;
	Int highestPriority = (-2147483647 - 1);
	std::vector<SkirmishProductionCandidate> candidates;
	UnsignedInt sourceOrdinal = 0U;
	Player::PlayerTeamList::const_iterator teamIt;
	for (teamIt = m_player->getPlayerTeams()->begin();
		teamIt != m_player->getPlayerTeams()->end(); ++teamIt, ++sourceOrdinal)
	{
		SkirmishProductionCandidate candidate;
		candidate.prototype = *teamIt;
		candidate.sourceOrdinal = sourceOrdinal;
		if (!isAdaptiveProductionCandidate(candidate.prototype,
			&candidate.costRange, &candidate.factoryWaitFrames))
		{
			continue;
		}
		candidates.push_back(candidate);
		const Int priority = candidate.prototype->getTemplateInfo()->m_productionPriority;
		if (ShouldReplaceSkirmishAIHighestPriority(
			hasCandidate, priority, highestPriority))
		{
			highestPriority = priority;
		}
		hasCandidate = true;
	}

	// Match the fixed-snapshot path's legacy reinforcement boundary exactly.
	if (selectTeamToReinforce(highestPriority))
		return true;
	if (!hasCandidate)
		return false;

	rts::AIProductionPlanningSnapshot context;
	rts::ClearAIProductionPlanningSnapshot(&context);
	context.frame = TheGameLogic->getFrame();
	context.ownerPlayerIndex = (UnsignedInt)m_player->getPlayerIndex();
	context.tieBreakKey = randomKey;
	Bool criticalRebuildCanStart = false;
	const Int rebuildReserve = getCriticalRebuildReserve(&criticalRebuildCanStart);
	const Int poorReserve = TheAI->getAiData()->m_resourcesPoor;
	context.initialReserve = usesProductionBehavior()
		? GetSkirmishAIAggregateReserve(poorReserve, rebuildReserve,
			getActiveRecoveryReserveCost())
		: GetSkirmishAIReserve(poorReserve, rebuildReserve);
	context.retryReserve = usesProductionBehavior()
		? context.initialReserve : GetSkirmishAIReserve(poorReserve, 0);
	if (usesProductionBehavior())
		m_strategyProductionReserveCost = context.initialReserve;
	const Bool rebuildReserveApplied = context.initialReserve > context.retryReserve;
	context.retryWithoutInitialReserve =
		!usesProductionBehavior() && rebuildReserveApplied &&
		!criticalRebuildCanStart ? 1U : 0U;
	context.resources = m_player->getMoney()->countMoney();
	context.logicFramesPerSecond = LOGICFRAMES_PER_SECOND;
	const SkirmishAIDecisionDifficulty difficulty = getDecisionDifficulty();
	context.difficulty = (Int)difficulty;
	context.contextInfluencePercent = GetSkirmishAIContextInfluencePercent(difficulty);

	Int enemyAircraftValue = 0;
	Int enemyVehicleValue = 0;
	Int enemyInfantryValue = 0;
	Coord3D routeTarget;
	Bool hasRouteTarget = false;
	getVisibleEnemyCompositionFor(m_currentEnemy, &enemyAircraftValue,
		&enemyVehicleValue, &enemyInfantryValue, &routeTarget, &hasRouteTarget);
	SkirmishAIProductionMode productionMode = SKIRMISH_AI_PRODUCTION_BALANCED;
	if (m_strategyState.currentMode == SKIRMISH_STRATEGY_FORTIFY)
		productionMode = SKIRMISH_AI_PRODUCTION_FORTIFY;
	else if (m_strategyState.currentMode == SKIRMISH_STRATEGY_ASSAULT)
		productionMode = SKIRMISH_AI_PRODUCTION_ASSAULT;

	std::vector<rts::AIProductionCandidateFact> candidateFacts;
	candidateFacts.reserve(candidates.size());
	for (std::vector<SkirmishProductionCandidate>::const_iterator candidateIt = candidates.begin();
		candidateIt != candidates.end(); ++candidateIt)
	{
		rts::AIProductionCandidateFact fact;
		memset(&fact, 0, sizeof(fact));
		fact.sourceOrdinal = candidateIt->sourceOrdinal;
		fact.candidateStableId = (UnsignedInt)candidateIt->prototype->getID();
		fact.configuredPriority =
			candidateIt->prototype->getTemplateInfo()->m_productionPriority;
		fact.minimumCost = candidateIt->costRange.minimumCost;
		fact.plannedCost = candidateIt->costRange.plannedCost;
		if (usesProductionBehavior())
			fact.minimumCost = fact.plannedCost;
		fact.factoryWaitFrames = candidateIt->factoryWaitFrames;
		fact.counterFitScore = getCandidateCounterFit(candidateIt->prototype,
			enemyAircraftValue, enemyVehicleValue, enemyInfantryValue);
		if (usesProductionBehavior())
			fact.counterFitScore = GetSkirmishAIProductionCounterFitScore(
				fact.counterFitScore, productionMode);
		fact.routeClass = (Int)classifyTeamRoute(
			candidateIt->prototype, &routeTarget, hasRouteTarget);
		fact.recentLossCount = candidateIt->prototype->getRecentSkirmishAILossCount();
		fact.recentPathFailureCount =
			candidateIt->prototype->getRecentSkirmishAIPathFailureCount();
		fact.eligible = 1U;
		candidateFacts.push_back(fact);
	}

	rts::RecordAIPlanningOwnerCapture((UnsignedInt)candidateFacts.size());
	rts::AIProductionSelectionResult result;
	if (!rts::PlanAIProductionSelectionOwnerSerial(context,
		&candidateFacts[0], (UnsignedInt)candidateFacts.size(), &result) ||
		!result.valid)
	{
		rts::RecordAIPlanningOwnerCommit(false);
		return false;
	}
	// A valid planner result with no affordable/admitted selection is a normal
	// no-production outcome; no owner mutation was attempted to reject.
	if (!result.hasSelection)
		return false;

	TeamPrototype *selectedPrototype = NULL;
	for (std::vector<SkirmishProductionCandidate>::const_iterator candidateIt = candidates.begin();
		candidateIt != candidates.end(); ++candidateIt)
	{
		if (candidateIt->sourceOrdinal == result.selectedSourceOrdinal &&
			(UnsignedInt)candidateIt->prototype->getID() == result.selectedStableId)
		{
			selectedPrototype = candidateIt->prototype;
			break;
		}
	}
	if (!selectedPrototype)
	{
		rts::RecordAIPlanningOwnerCommit(false);
		return false;
	}

	// Revalidate the stable ID at the original live-list ordinal immediately
	// before the single commit mutation.
	sourceOrdinal = 0U;
	for (teamIt = m_player->getPlayerTeams()->begin();
		teamIt != m_player->getPlayerTeams()->end(); ++teamIt, ++sourceOrdinal)
	{
		if (sourceOrdinal != result.selectedSourceOrdinal)
			continue;
		const Bool accepted = *teamIt == selectedPrototype &&
			(UnsignedInt)(*teamIt)->getID() == result.selectedStableId &&
			queueSelectedTeam(*teamIt);
		rts::RecordAIPlanningOwnerCommit(accepted != FALSE);
		return accepted;
	}
	rts::RecordAIPlanningOwnerCommit(false);
	return false;
}

Bool AISkirmishPlayer::validateProductionPlanningCommit(
	const rts::AIProductionPlanningSnapshot &snapshot,
	const rts::AIProductionPlanningResult &result ) const
{
	// The shared batch executor has already performed the canonical numeric
	// validation.  Keep this owner boundary structural and membership-only so
	// the normal production path does not recompute the oracle.
	if (!result.valid ||
		snapshot.frame != (UnsignedInt)TheGameLogic->getFrame() ||
		snapshot.ownerPlayerIndex != (UnsignedInt)m_player->getPlayerIndex() ||
		result.orderKey.frame != snapshot.frame ||
		result.orderKey.playerIndex != snapshot.ownerPlayerIndex ||
		result.orderKey.subphase != rts::AI_PLANNING_SUBPHASE_TEAM_PRODUCTION ||
		result.orderKey.emissionOrdinal != 0U ||
		result.selectedSourceOrdinal != result.orderKey.sourceOrdinal ||
		snapshot.candidateCount > rts::AI_PLANNING_MAX_PRODUCTION_CANDIDATES)
		return false;
	if (!result.hasSelection)
		return result.selectedSourceOrdinal == rts::AI_PLANNING_INVALID_ORDINAL &&
			result.orderKey.sourceOrdinal == rts::AI_PLANNING_INVALID_ORDINAL &&
			result.selectedStableId == 0U && result.tieCount == 0U;

	Bool snapshotMember = false;
	for (UnsignedInt i = 0; i < snapshot.candidateCount; ++i)
	{
		const rts::AIProductionCandidateFact &candidate = snapshot.candidates[i];
		if (candidate.eligible &&
			candidate.sourceOrdinal == result.selectedSourceOrdinal &&
			candidate.candidateStableId == result.selectedStableId)
		{
			snapshotMember = true;
			break;
		}
	}
	if (!snapshotMember)
		return false;

	UnsignedInt sourceOrdinal = 0;
	Player::PlayerTeamList::const_iterator teamIt;
	for (teamIt = m_player->getPlayerTeams()->begin();
		teamIt != m_player->getPlayerTeams()->end(); ++teamIt, ++sourceOrdinal)
	{
		if (sourceOrdinal == result.selectedSourceOrdinal)
			return (UnsignedInt)(*teamIt)->getID() == result.selectedStableId;
	}
	return false;
}

Bool AISkirmishPlayer::validateProductionPlanningBatchCommit(
	const rts::AIProductionPlanningSnapshot &snapshot,
	const rts::AIProductionPlanningResult &result) const
{
	return validateProductionPlanningCommit(snapshot, result);
}

Bool AISkirmishPlayer::commitProductionPlanningResult(
	const rts::AIProductionPlanningSnapshot &snapshot,
	const rts::AIProductionPlanningResult &result )
{
	if (!validateProductionPlanningCommit(snapshot, result))
		return false;

	UnsignedInt sourceOrdinal = 0;
	Player::PlayerTeamList::const_iterator teamIt;
	for (teamIt = m_player->getPlayerTeams()->begin();
		teamIt != m_player->getPlayerTeams()->end(); ++teamIt, ++sourceOrdinal)
	{
		if (sourceOrdinal == result.selectedSourceOrdinal)
			return queueSelectedTeam(*teamIt);
	}
	return false;
}

Bool AISkirmishPlayer::selectTeamToBuildWithPlanning()
{
	// Feedback decay is an explicit owner mutation and therefore happens before
	// immutable capture rather than being hidden inside a capture adapter.
	Player::PlayerTeamList::const_iterator teamIt;
	for (teamIt = m_player->getPlayerTeams()->begin();
		teamIt != m_player->getPlayerTeams()->end(); ++teamIt)
	{
		(*teamIt)->decaySkirmishAIFeedback(TheGameLogic->getFrame());
	}

	rts::AICounterRngKey randomKey;
	rts::ClearAICounterRngKey(&randomKey);
	randomKey.simulationEpoch = SKIRMISH_AI_REPLAY_EPOCH_COUNTER_RNG;
	randomKey.matchSeed = GetGameLogicRandomSeed();
	randomKey.frame = TheGameLogic->getFrame();
	randomKey.domain = rts::AI_COUNTER_RNG_DOMAIN_PLAYER_PLANNING;
	randomKey.playerIndex = (UnsignedInt)m_player->getPlayerIndex();
	randomKey.ownerStableId = randomKey.playerIndex;
	randomKey.sourceStableId = 0U;
	randomKey.eventKind = rts::AI_COUNTER_RNG_EVENT_PRODUCTION_TIE;
	randomKey.eventOrdinal = 0U;
	randomKey.drawOrdinal = 0U;

	// The normal current-epoch path is admitted only by AI::update's one-batch
	// lane. If that lane cannot publish (capture, admission, or validation
	// failure), keep the deterministic counter-RNG result on the owner without
	// manufacturing a one-snapshot JobSystem batch.
	return selectTeamToBuildCounterSerialFallback(randomKey);
}
#endif

Bool AISkirmishPlayer::usesStrategyBehavior() const
{
	if (!m_player || m_player->getPlayerType() != PLAYER_COMPUTER ||
		!TheGameLogic)
		return false;
	const Bool replay = TheGameLogic->isInReplayGame();
	const Int gameMode = replay
		? (TheRecorder ? TheRecorder->getGameMode() : GAME_NONE)
		: TheGameLogic->getGameMode();
	if (!IsSkirmishAIRecoveryGameMode(gameMode))
		return false;
	return ShouldUseSkirmishAIStrategyBehavior(
		replay, TheRecorder ? TheRecorder->getSkirmishAIReplayEpoch() :
			SKIRMISH_AI_REPLAY_EPOCH_LEGACY);
}

void AISkirmishPlayer::collectStrategyMetrics(
	SkirmishStrategyMetrics *metrics, ObjectID *strategicTargetID)
{
	metrics->economyHealth = 0;
	metrics->baseIntegrity = 0;
	metrics->armyReadiness = 0;
	metrics->immediateThreat = 0;
	metrics->attackConfidence = 0;
	metrics->enemyOpportunity = 0;
	metrics->alliedDistress = 0;
	metrics->availableCombatValue = 0;
	metrics->hasStrategicTarget = false;
	metrics->assaultLostHalfForce = false;
	metrics->assaultObjectiveComplete = false;
	metrics->viableAssaultForceAssembled = false;
	*strategicTargetID = INVALID_ID;

	const SkirmishStrategyExpectedAssets expected =
		GetSkirmishStrategyExpectedAssets(m_player);
	Int commandHealth = 0;
	Int economyHealth = 0;
	Int powerHealth = 0;
	Int productionHealth = 0;
	Int ownCombatValue = 0;
	Int ownLocalCombatValue = 0;
	Int enemyCombatValue = 0;
	Int enemyLocalCombatValue = 0;
	Int knownOpportunityValue = 0;
	std::vector<SkirmishStrategyCapabilityCandidate> groundAttackers;
	std::vector<SkirmishStrategyCapabilityCandidate> airAttackers;
	SkirmishStrategyCapabilityCandidate
		targetCandidates[MAX_SKIRMISH_STRATEGY_TARGET_CANDIDATES];
	Int targetCandidateCount = 0;
	Object *persistedTarget = nullptr;
	Player *enemy = m_currentEnemy;
	const Bool retainObservedTarget =
		m_strategyState.currentMode == SKIRMISH_STRATEGY_ASSAULT &&
		m_strategyState.strategicTargetID != INVALID_ID &&
		IsSkirmishStrategyTargetObservationAvailable(
			m_strategyState.strategicTargetObserved, TheGameLogic->getFrame(),
			m_strategyState.strategicTargetLastSeenFrame);
	Bool persistedTargetVisible = false;
	const Real threatRadius = (m_baseRadius > 0.0f ? m_baseRadius : 0.0f) + 500.0f;
	const Real threatRadiusSquared = threatRadius * threatRadius;

	for (Object *object = TheGameLogic->getFirstObject(); object;
		object = object->getNextObject()) {
		const Bool isPersistedAssaultTarget =
			(m_strategyState.currentMode == SKIRMISH_STRATEGY_ASSAULT ||
			 m_strategyState.pendingMode == SKIRMISH_STRATEGY_ASSAULT) &&
			m_strategyState.strategicTargetID != INVALID_ID &&
			object->getID() == m_strategyState.strategicTargetID;
		if (isPersistedAssaultTarget &&
			!IsSkirmishStrategyIntelEligible(object, m_player))
			continue;
		if (isPersistedAssaultTarget) {
			persistedTargetVisible = true;
			metrics->assaultObjectiveComplete = object->isEffectivelyDead() ||
				object->isDestroyed() || object->testStatus(OBJECT_STATUS_SOLD) ||
				!enemy || object->getControllingPlayer() != enemy;
			if (!metrics->assaultObjectiveComplete &&
				IsSkirmishStrategyStaticTarget(object))
				persistedTarget = object;
		}
		if (!object || object->isEffectivelyDead() || object->isDestroyed() ||
			object->testStatus(OBJECT_STATUS_SOLD))
			continue;
		Player *owner = object->getControllingPlayer();
		if (owner == m_player) {
			const Int health = GetSkirmishStrategyHealthPercent(object);
			if (object->isKindOf(KINDOF_COMMANDCENTER))
				commandHealth = AddSkirmishStrategyValue(commandHealth, health);
			else if (object->isKindOf(KINDOF_FS_SUPPLY_CENTER) ||
				object->isKindOf(KINDOF_CASH_GENERATOR))
				economyHealth = AddSkirmishStrategyValue(economyHealth, health);
			else if (object->isKindOf(KINDOF_FS_POWER) &&
				!object->isKindOf(KINDOF_CASH_GENERATOR))
				powerHealth = AddSkirmishStrategyValue(powerHealth, health);
			else if (object->isKindOf(KINDOF_FS_BARRACKS) ||
				object->isKindOf(KINDOF_FS_WARFACTORY) ||
				object->isKindOf(KINDOF_FS_AIRFIELD))
				productionHealth = AddSkirmishStrategyValue(productionHealth, health);

			if (IsSkirmishStrategyCombatObject(object) && !object->isContained()) {
				const Int cost = object->getTemplate()->calcCostToBuild(m_player);
				const Int value = cost > 0 ?
					(Int)((__int64)cost * health / 100) : 0;
				ownCombatValue = AddSkirmishStrategyValue(ownCombatValue, value);
				if (m_baseCenterSet) {
					const Real dx = object->getPosition()->x - m_baseCenter.x;
					const Real dy = object->getPosition()->y - m_baseCenter.y;
					if (dx * dx + dy * dy <= threatRadiusSquared)
						ownLocalCombatValue = AddSkirmishStrategyValue(
							ownLocalCombatValue, value);
				}
				if (IsSkirmishStrategyOffensiveRecipient(object, m_player)) {
					if (object->isKindOf(KINDOF_AIRCRAFT))
						AppendSkirmishStrategyCapabilityCandidate(
							&airAttackers, object, value);
					else
						AppendSkirmishStrategyCapabilityCandidate(
							&groundAttackers, object, value);
				}
			}
			continue;
		}

		if (!owner || !IsSkirmishStrategyIntelEligible(object, m_player))
			continue;
		const Int health = GetSkirmishStrategyHealthPercent(object);
		const Int cost = object->getTemplate()->calcCostToBuild(owner);
		const Int value = cost > 0 ?
			(Int)((__int64)cost * health / 100) : 0;
		Team *ownerDefaultTeam = owner->getDefaultTeam();
		if (ownerDefaultTeam &&
			m_player->getRelationship(ownerDefaultTeam) == ENEMIES &&
			IsSkirmishStrategyCombatObject(object) && m_baseCenterSet) {
			const Real dx = object->getPosition()->x - m_baseCenter.x;
			const Real dy = object->getPosition()->y - m_baseCenter.y;
			if (dx * dx + dy * dy <= threatRadiusSquared)
				enemyLocalCombatValue = AddSkirmishStrategyValue(
					enemyLocalCombatValue, value);
		}
		if (!enemy || owner != enemy)
			continue;
		if (IsSkirmishStrategyCombatObject(object))
			enemyCombatValue = AddSkirmishStrategyValue(enemyCombatValue, value);
		if (object->isKindOf(KINDOF_STRUCTURE) ||
			object->isKindOf(KINDOF_HARVESTER))
			knownOpportunityValue = AddSkirmishStrategyValue(
				knownOpportunityValue, value);
		if (IsSkirmishStrategyStaticTarget(object)) {
			InsertSkirmishStrategyTargetCandidate(
				targetCandidates, &targetCandidateCount, object, value);
		}
	}
	if (m_strategyState.currentMode == SKIRMISH_STRATEGY_FORTIFY) {
		for (DLINK_ITERATOR<TeamInQueue> ready = iterate_TeamReadyQueue();
			!ready.done(); ready.advance()) {
			TeamInQueue *readyTeam = ready.cur();
			Team *team = readyTeam ? readyTeam->m_team : 0;
			if (!IsSkirmishStrategyOffensiveTeamType(team, m_player) ||
				!IsSkirmishStrategyReadyTeamActivationDue(readyTeam))
				continue;
			for (DLINK_ITERATOR<Object> member = team->iterate_TeamMemberList();
				!member.done(); member.advance()) {
				Object *readyObject = member.cur();
				if (!IsSkirmishStrategyPotentialOffensiveRecipient(
						readyObject, m_player, team))
					continue;
				const Int health = GetSkirmishStrategyHealthPercent(readyObject);
				const Int cost = readyObject->getTemplate()->calcCostToBuild(m_player);
				const Int value = cost > 0 ?
					(Int)((__int64)cost * health / 100) : 0;
				if (readyObject->isKindOf(KINDOF_AIRCRAFT))
					AppendSkirmishStrategyCapabilityCandidate(
						&airAttackers, readyObject, value);
				else
					AppendSkirmishStrategyCapabilityCandidate(
						&groundAttackers, readyObject, value);
			}
		}
	}
	SortSkirmishStrategyCapabilityCandidates(&airAttackers);
	SortSkirmishStrategyCapabilityCandidates(&groundAttackers);

	const Int poor = TheAI->getAiData()->m_resourcesPoor > 0 ?
		TheAI->getAiData()->m_resourcesPoor : 2500;
	const __int64 fallbackWealthy = (__int64)poor * 4;
	const Int wealthy = TheAI->getAiData()->m_resourcesWealthy > poor ?
		TheAI->getAiData()->m_resourcesWealthy :
		(Int)(fallbackWealthy > 2147483647 ? 2147483647 : fallbackWealthy);
	const Int money = m_player->getMoney()->countMoney();
	Int cashScore = 0;
	if (money <= poor)
		cashScore = ClampSkirmishStrategyPercent(
			(Int)((__int64)(money > 0 ? money : 0) * 50 / poor));
	else if (money >= wealthy)
		cashScore = 100;
	else
		cashScore = 50 +
			(Int)((__int64)(money - poor) * 50 / (wealthy - poor));
	const Int economyStructureScore = GetSkirmishStrategyCategoryPercent(
		economyHealth, expected.economyStructures);
	metrics->economyHealth = ClampSkirmishStrategyPercent(
		(60 * cashScore + 40 * economyStructureScore) / 100);

	Int baseWeighted = 35 * GetSkirmishStrategyCategoryPercent(
		commandHealth, expected.commandCenters);
	Int baseWeight = 35;
	if (expected.economyStructures > 0) {
		baseWeighted += 20 * economyStructureScore;
		baseWeight += 20;
	}
	if (expected.powerStructures > 0) {
		baseWeighted += 15 * GetSkirmishStrategyCategoryPercent(
			powerHealth, expected.powerStructures);
		baseWeight += 15;
	}
	if (expected.productionStructures > 0) {
		baseWeighted += 30 * GetSkirmishStrategyCategoryPercent(
			productionHealth, expected.productionStructures);
		baseWeight += 30;
	}
	metrics->baseIntegrity = ClampSkirmishStrategyPercent(
		baseWeight > 0 ? baseWeighted / baseWeight : 0);
	metrics->armyReadiness = GetSkirmishStrategyValuePercent(
		ownCombatValue, wealthy);
	metrics->availableCombatValue = ownCombatValue;
	metrics->assaultLostHalfForce =
		m_strategyState.currentMode == SKIRMISH_STRATEGY_ASSAULT &&
		m_strategyState.assaultEntryCombatValue > 0 &&
		(__int64)ownCombatValue * 2 <= m_strategyState.assaultEntryCombatValue;

	const Int absoluteThreat = GetSkirmishStrategyValuePercent(
		enemyLocalCombatValue, poor);
	Int relativeThreat = 0;
	if (enemyLocalCombatValue > 0)
		relativeThreat = ClampSkirmishStrategyPercent(
			(Int)((__int64)enemyLocalCombatValue * 100 /
				((__int64)enemyLocalCombatValue + ownLocalCombatValue)));
	metrics->immediateThreat = absoluteThreat > relativeThreat ?
		absoluteThreat : relativeThreat;
	const UnsignedInt attackedFrame = m_player->getAttackedFrame();
	if (attackedFrame != 0 &&
		TheGameLogic->getFrame() - attackedFrame <= 10 * LOGICFRAMES_PER_SECOND &&
		metrics->immediateThreat < 60)
		metrics->immediateThreat = 60;

	Int forceConfidence = 75;
	if (enemyCombatValue > 0)
		forceConfidence = ClampSkirmishStrategyPercent(
			(Int)((__int64)ownCombatValue * 100 /
				((__int64)ownCombatValue + enemyCombatValue)));
	const Bool preserveHiddenTarget = retainObservedTarget &&
		!persistedTargetVisible;
	Object *selectedTarget = nullptr;
	Int quickPathQueryCount = 0;
	Int targetAttemptCount = 0;
	if (!preserveHiddenTarget && persistedTarget) {
		++targetAttemptCount;
		if (HasSkirmishStrategyTargetCapability(
				persistedTarget, airAttackers, groundAttackers,
				&quickPathQueryCount))
			selectedTarget = persistedTarget;
	}
	if (!preserveHiddenTarget && !selectedTarget) {
		Int candidateIndex;
		for (candidateIndex = 0;
			candidateIndex < targetCandidateCount &&
				targetAttemptCount < MAX_SKIRMISH_STRATEGY_TARGET_CANDIDATES;
			++candidateIndex) {
			Object *candidate = targetCandidates[candidateIndex].object;
			if (candidate == persistedTarget)
				continue;
			++targetAttemptCount;
			if (HasSkirmishStrategyTargetCapability(
					candidate, airAttackers, groundAttackers,
					&quickPathQueryCount)) {
				selectedTarget = candidate;
				break;
			}
		}
	}
	const Bool routeAvailable = selectedTarget != nullptr;
	const Bool hasVisibleTargetCandidate =
		persistedTarget != nullptr || targetCandidateCount > 0;
	const Int routeConfidence = preserveHiddenTarget ? 50 :
		(hasVisibleTargetCandidate ? (routeAvailable ? 100 : 20) : 50);
	metrics->attackConfidence = ClampSkirmishStrategyPercent(
		(70 * forceConfidence + 30 * routeConfidence) / 100);
	metrics->enemyOpportunity = GetSkirmishStrategyValuePercent(
		knownOpportunityValue, wealthy);
	metrics->viableAssaultForceAssembled = metrics->armyReadiness >= 70;
	metrics->hasStrategicTarget = preserveHiddenTarget || routeAvailable;
	if (routeAvailable)
		*strategicTargetID = selectedTarget->getID();
}

void AISkirmishPlayer::commandOffensiveTeams(
	SkirmishStrategyMode mode, Object *target)
{
	if (!TheAI || !m_player)
		return;
	std::vector<Team *> teams;
	Player::PlayerTeamList::const_iterator prototype;
	for (prototype = m_player->getPlayerTeams()->begin();
		prototype != m_player->getPlayerTeams()->end(); ++prototype) {
		for (DLINK_ITERATOR<Team> teamInstance =
				(*prototype)->iterate_TeamInstanceList();
			!teamInstance.done(); teamInstance.advance()) {
			Team *candidate = teamInstance.cur();
			if (IsSkirmishStrategyOffensiveTeam(candidate, m_player) &&
				candidate->hasAnyObjects()) {
				SkirmishStrategyGroupRecipientContext context;
				context.player = m_player;
				context.group = 0;
				context.found = false;
				candidate->iterateObjects(
					CollectSkirmishStrategyGroupRecipient, &context);
				if (context.found)
					teams.push_back(candidate);
			}
		}
	}
	size_t i;
	for (i = 1; i < teams.size(); ++i) {
		Team *candidate = teams[i];
		size_t position = i;
		while (position > 0 && candidate->getID() < teams[position - 1]->getID()) {
			teams[position] = teams[position - 1];
			--position;
		}
		teams[position] = candidate;
	}
	std::vector<Team *>::iterator teamIterator;
	for (teamIterator = teams.begin();
		teamIterator != teams.end(); ++teamIterator) {
		AIGroupPtr group = TheAI->createGroup();
		if (!group)
			continue;
#if RETAIL_COMPATIBLE_AIGROUP
		AIGroup *groupObject = group;
#else
		AIGroup *groupObject = group.Peek();
#endif
		SkirmishStrategyGroupRecipientContext context;
		context.player = m_player;
		context.group = groupObject;
		context.found = false;
		(*teamIterator)->iterateObjects(
			CollectSkirmishStrategyGroupRecipient, &context);
		if (!context.found)
			continue;
		if (mode == SKIRMISH_STRATEGY_BALANCED) {
			group->groupIdle(CMD_FROM_AI);
		} else if (mode == SKIRMISH_STRATEGY_FORTIFY) {
			if (m_baseCenterSet)
				group->groupGuardPosition(
					&m_baseCenter, GUARDMODE_GUARD_WITHOUT_PURSUIT, CMD_FROM_AI);
			else
				group->groupIdle(CMD_FROM_AI);
		} else if (mode == SKIRMISH_STRATEGY_ASSAULT) {
			if (target)
				group->groupAttackMoveToPosition(
					target->getPosition(), NO_MAX_SHOTS_LIMIT, CMD_FROM_AI);
			else
				group->groupIdle(CMD_FROM_AI);
		}
	}
}

void AISkirmishPlayer::applyStrategyMode(
	SkirmishStrategyMode previousMode, SkirmishStrategyMode currentMode,
	ObjectID previousTargetID)
{
	// Strategy commands are transition impulses.  Native team scripts retain
	// ownership on later frames; stable modes must not keep overwriting them.
	if (previousMode == currentMode &&
		(currentMode != SKIRMISH_STRATEGY_ASSAULT ||
		 previousTargetID == m_strategyState.strategicTargetID))
		return;
	Object *target = currentMode == SKIRMISH_STRATEGY_ASSAULT &&
		m_strategyState.strategicTargetID != INVALID_ID
		? TheGameLogic->findObjectByID(m_strategyState.strategicTargetID) : nullptr;
	if (target && !IsSkirmishStrategyIntelEligible(target, m_player))
		target = nullptr;
	if (target && (!IsSkirmishStrategyStaticTarget(target) ||
		 target->isEffectivelyDead() || target->isDestroyed() ||
		 target->testStatus(OBJECT_STATUS_SOLD) || !m_currentEnemy ||
		 target->getControllingPlayer() != m_currentEnemy))
		target = nullptr;
	if (currentMode == SKIRMISH_STRATEGY_ASSAULT && !target)
		ClearSkirmishStrategyTargetObservation(&m_strategyState);
	commandOffensiveTeams(currentMode, target);
}

Bool AISkirmishPlayer::updateStrategy()
{
	const UnsignedInt currentFrame = TheGameLogic->getFrame();
	if (!usesStrategyBehavior() || !IsSkirmishStrategyFrameReached(
			currentFrame, m_strategyState.nextEvaluationFrame))
		return false;
	SkirmishStrategyMetrics metrics;
	ObjectID targetID = INVALID_ID;
	collectStrategyMetrics(&metrics, &targetID);
	const SkirmishStrategyMode previousMode = m_strategyState.currentMode;
	SkirmishStrategyDecision decision = EvaluateSkirmishStrategy(
		m_strategyState, metrics, m_difficulty, currentFrame,
		usesProductionBehavior());
	const Bool leftAssault = previousMode == SKIRMISH_STRATEGY_ASSAULT &&
		decision.nextState.currentMode != SKIRMISH_STRATEGY_ASSAULT;
	if (leftAssault) {
		ClearSkirmishStrategyTargetObservation(&decision.nextState);
	} else if (targetID != INVALID_ID &&
		(decision.nextState.currentMode == SKIRMISH_STRATEGY_ASSAULT ||
		 decision.nextState.pendingMode == SKIRMISH_STRATEGY_ASSAULT)) {
		decision.nextState.strategicTargetID = targetID;
		decision.nextState.strategicTargetObserved = true;
		decision.nextState.strategicTargetLastSeenFrame = currentFrame;
	} else if (decision.nextState.currentMode == SKIRMISH_STRATEGY_ASSAULT &&
		!metrics.hasStrategicTarget) {
		ClearSkirmishStrategyTargetObservation(&decision.nextState);
	} else if (decision.nextState.currentMode != SKIRMISH_STRATEGY_ASSAULT) {
		ClearSkirmishStrategyTargetObservation(&decision.nextState);
	}
	m_strategyState = decision.nextState;
	if (previousMode != SKIRMISH_STRATEGY_FORTIFY &&
		m_strategyState.currentMode == SKIRMISH_STRATEGY_FORTIFY)
		clearStrategySourceCommandLock();
	else if (m_strategyState.superweaponAttemptStatus !=
		SKIRMISH_STRATEGY_ATTEMPT_PENDING)
		clearStrategySourceCommandLock();
	if (TheGlobalData->m_debugAI && TheScriptEngine && decision.evaluated) {
		AsciiString message;
		message.format("AI strategy mode=%d reason=%d E=%d B=%d A=%d T=%d C=%d O=%d L=%d fortify=%u assault=%u",
			(Int)m_strategyState.currentMode, (Int)decision.reason,
			metrics.economyHealth, metrics.baseIntegrity, metrics.armyReadiness,
			metrics.immediateThreat, metrics.attackConfidence,
			metrics.enemyOpportunity, metrics.alliedDistress,
			decision.fortifyPressureHundredths,
			decision.assaultReadinessHundredths);
		TheScriptEngine->AppendDebugMessage(message, false);
	}
	return decision.evaluated;
}

/**
 * Determine the next team to build.  Return true if one was selected.
 */
Bool AISkirmishPlayer::selectTeamToBuild()
{
	if (!ShouldUseCurrentSkirmishAIBehavior())
		return AIPlayer::selectTeamToBuild();
#if defined(_WIN64)
	if (hasStagedProductionPlanningResult())
	{
		rts::AIProductionPlanningSnapshot snapshot;
		rts::AIProductionPlanningResult result;
		if (!takeStagedProductionPlanningResult(&snapshot, &result))
			return false;
		if (!result.valid || !result.hasSelection)
			return false;
		return commitProductionPlanningResult(snapshot, result);
	}
	if (ShouldUseCounterBasedSkirmishAIPlanning())
		return selectTeamToBuildWithPlanning();
#endif

	Bool hasCandidate = false;
	Int highestPriority = (-2147483647 - 1);
	std::vector<SkirmishProductionCandidate> candidates;
	Player::PlayerTeamList::const_iterator teamIt;
	for (teamIt = m_player->getPlayerTeams()->begin(); teamIt != m_player->getPlayerTeams()->end(); ++teamIt) {
		SkirmishProductionCandidate candidate;
		candidate.prototype = *teamIt;
		candidate.sourceOrdinal = 0U;
		candidate.prototype->decaySkirmishAIFeedback(TheGameLogic->getFrame());
		if (!isAdaptiveProductionCandidate(
			candidate.prototype, &candidate.costRange, &candidate.factoryWaitFrames))
			continue;
		candidates.push_back(candidate);
		Int priority = candidate.prototype->getTemplateInfo()->m_productionPriority;
		if (ShouldReplaceSkirmishAIHighestPriority(hasCandidate, priority, highestPriority))
			highestPriority = priority;
		hasCandidate = true;
	}

	if (selectTeamToReinforce(highestPriority))
		return true;
	if (!hasCandidate)
		return false;

	Bool criticalRebuildCanStart = false;
	Int rebuildReserve = getCriticalRebuildReserve(&criticalRebuildCanStart);
	Int poorReserve = TheAI->getAiData()->m_resourcesPoor;
	Int reserve = usesProductionBehavior()
		? GetSkirmishAIAggregateReserve(poorReserve, rebuildReserve,
			getActiveRecoveryReserveCost())
		: GetSkirmishAIReserve(poorReserve, rebuildReserve);
	if (usesProductionBehavior())
		m_strategyProductionReserveCost = reserve;
	Bool rebuildReserveApplied = reserve > GetSkirmishAIReserve(poorReserve, 0);
	Int resources = m_player->getMoney()->countMoney();
	SkirmishAIDecisionDifficulty difficulty = getDecisionDifficulty();
	SkirmishAIProductionMode productionMode = SKIRMISH_AI_PRODUCTION_BALANCED;
	if (m_strategyState.currentMode == SKIRMISH_STRATEGY_FORTIFY)
		productionMode = SKIRMISH_AI_PRODUCTION_FORTIFY;
	else if (m_strategyState.currentMode == SKIRMISH_STRATEGY_ASSAULT)
		productionMode = SKIRMISH_AI_PRODUCTION_ASSAULT;

	Int enemyAircraftValue;
	Int enemyVehicleValue;
	Int enemyInfantryValue;
	Coord3D routeTarget;
	Bool hasRouteTarget;
	getVisibleEnemyComposition(
		&enemyAircraftValue, &enemyVehicleValue, &enemyInfantryValue, &routeTarget, &hasRouteTarget);

	std::vector<TeamPrototype *> bestTeams;
	__int64 bestScore = 0;
	Bool hasBestScore = false;
	for (Int pass = 0; pass < 2; ++pass) {
		bestTeams.clear();
		hasBestScore = false;
		for (std::vector<SkirmishProductionCandidate>::iterator candidateIt = candidates.begin();
			candidateIt != candidates.end(); ++candidateIt) {
			TeamPrototype *prototype = candidateIt->prototype;
			Int priority = prototype->getTemplateInfo()->m_productionPriority;
			if (!IsSkirmishAIPriorityAdmitted(priority, highestPriority, difficulty) ||
				!IsSkirmishAIAffordable(resources,
					usesProductionBehavior() ? candidateIt->costRange.plannedCost :
						candidateIt->costRange.minimumCost,
					reserve))
				continue;

			SkirmishAITeamScoreInput input;
			input.configuredPriority = priority;
			input.counterFitScore = getCandidateCounterFit(
				prototype, enemyAircraftValue, enemyVehicleValue, enemyInfantryValue);
			if (usesProductionBehavior())
				input.counterFitScore = GetSkirmishAIProductionCounterFitScore(
					input.counterFitScore, productionMode);
			input.resources = resources;
			input.minimumCost = candidateIt->costRange.minimumCost;
			input.plannedCost = candidateIt->costRange.plannedCost;
			input.reserve = reserve;
			input.factoryWaitFrames = candidateIt->factoryWaitFrames;
			input.logicFramesPerSecond = LOGICFRAMES_PER_SECOND;
			input.routeClass = classifyTeamRoute(prototype, &routeTarget, hasRouteTarget);
			input.recentLossCount = prototype->getRecentSkirmishAILossCount();
			input.recentPathFailureCount = prototype->getRecentSkirmishAIPathFailureCount();
			input.difficulty = difficulty;
			SkirmishAITeamScoreResult score = ScoreSkirmishAITeam(
				input, usesProductionBehavior());

			if (TheGlobalData->m_debugAI) {
				AsciiString message;
				message.format("AI team %s score=%I64d pri=%d counter=%d econ=%d wait=%d route=%d loss=%d path=%d reserve=%d min=%d planned=%d",
					prototype->getName().str(), score.finalScore, priority, score.counterFitScore,
					score.economyScore, score.factoryWaitScore, score.routeScore,
					score.lossScore, score.pathFailureScore, reserve,
					candidateIt->costRange.minimumCost, candidateIt->costRange.plannedCost);
				TheScriptEngine->AppendDebugMessage(message, false);
			}

			if (!hasBestScore || score.finalScore > bestScore) {
				bestScore = score.finalScore;
				hasBestScore = true;
				bestTeams.clear();
				bestTeams.push_back(prototype);
			} else if (IsSkirmishAITeamScoreTie(score.finalScore, bestScore)) {
				bestTeams.push_back(prototype);
			}
		}

		if (!bestTeams.empty())
			break;
		if (usesProductionBehavior())
			break;
		if (!ShouldRetrySkirmishAIReserve(
			criticalRebuildCanStart, rebuildReserveApplied, false))
			break;
		reserve = GetSkirmishAIReserve(poorReserve, 0);
		rebuildReserveApplied = false;
	}

	if (bestTeams.empty())
		return false;
	Int selected = 0;
	if (bestTeams.size() > 1)
		selected = GetSkirmishAITieSelectionIndex(
			(Int)bestTeams.size(), GameLogicRandomValue(0, (Int)bestTeams.size() - 1));
	return queueSelectedTeam(bestTeams[selected]);
}

/**
	Build a specific building.
	*/
void AISkirmishPlayer::buildSpecificAIBuilding(const AsciiString &thingName)
{
	//
	Bool found = false;
	Bool foundUnbuilt = false;
	for( BuildListInfo *info = m_player->getBuildList(); info; info = info->getNext() )
	{
		if (info->getTemplateName()==thingName)
		{
			AsciiString name = info->getTemplateName();
			if (name.isEmpty()) continue;
			const ThingTemplate *bldgPlan = TheThingFactory->findTemplate( name );
			if (!bldgPlan) {
				DEBUG_LOG(("*** ERROR - Build list building '%s' doesn't exist.", name.str()));
				continue;
			}
			Object *bldg = TheGameLogic->findObjectByID( info->getObjectID() );
			found = true;
			if (bldg) {
				continue; // already built.
			}
			if (info->isPriorityBuild()) {
				continue; // already marked for priority build.
			}
			foundUnbuilt = true;
			info->markPriorityBuild();
			break;
		}
	}
	if (foundUnbuilt) {
		m_buildDelay = 0;
		AsciiString buildingStr = "Queueing building '";
		buildingStr.concat(thingName);
		buildingStr.concat("' for construction.");
		TheScriptEngine->AppendDebugMessage(buildingStr, false);
	}	else if (found) {
		AsciiString buildingStr = "Warning - all instances of building '";
		buildingStr.concat(thingName);
		buildingStr.concat("' are already built or queued for build, not queueing.");
		TheScriptEngine->AppendDebugMessage(buildingStr, false);
	}	else {
		AsciiString buildingStr = "Error - could not find building '";
		buildingStr.concat(thingName);
		buildingStr.concat("' in the building template list.");
		TheScriptEngine->AppendDebugMessage(buildingStr, false);
	}
}



/**
	Gets the player index of my enemy.
	*/
Int AISkirmishPlayer::getMyEnemyPlayerIndex() {
	Int playerNdx;
	const Bool productionBehavior = usesProductionBehavior();
	if (m_currentEnemy) {
		if (!productionBehavior ||
			(m_currentEnemy->getDefaultTeam() &&
			 m_player->getRelationship(m_currentEnemy->getDefaultTeam()) == ENEMIES))
			return m_currentEnemy->getPlayerIndex();
	}
	if (productionBehavior)
		return -1;
	// For now, return first human player, as there should only be one. jba
	for (playerNdx=0; playerNdx<ThePlayerList->getPlayerCount(); playerNdx++) {
		if (ThePlayerList->getNthPlayer(playerNdx)->getPlayerType() == PLAYER_HUMAN) {
			break;
		}
	}
	return playerNdx;
}

/**
	Preserve the retail target selection and evaluation schedule for legacy
	replays and the PR6 liveness epoch.  This is intentionally kept separate
	from the current scoring implementation so replay compatibility does not
	depend on the current AI's observations or tie-breaking rules.
*/
void AISkirmishPlayer::acquireEnemyLegacy()
{
	Player *bestEnemy = nullptr;
	Real bestDistanceSqr = HUGE_DIST*HUGE_DIST;

	if (m_currentEnemy) {
		Bool inBadShape = !m_currentEnemy->hasAnyUnits() || !m_currentEnemy->hasAnyBuildFacility();
		if (!inBadShape) return;
	}

	// look for the closest enemy.
	Int i;
	for (i=0; i<ThePlayerList->getPlayerCount(); i++) {
		Player *curPlayer = ThePlayerList->getNthPlayer(i);
		if (m_player->getRelationship(curPlayer->getDefaultTeam()) == ENEMIES) {
			if (curPlayer->hasAnyObjects()==false) continue; // not much of an enemy.
			// ok, we got an enemy;
			// If a player is out of units, or out of build facilities, we can lower his priority.
			Bool inBadShape = !curPlayer->hasAnyUnits() || !curPlayer->hasAnyBuildFacility();

			Coord3D enemyPos = m_baseCenter;
			Region2D bounds;
			getPlayerStructureBounds(&bounds, i);
			enemyPos.x = bounds.lo.x + bounds.width()/2;
			enemyPos.y = bounds.lo.y + bounds.height()/2;
			Real curDistSqr = sqr(enemyPos.x-m_baseCenter.x) + sqr(enemyPos.y-m_baseCenter.y);

			//Fudge for in bad shape.  If an enemy is crippled, concentrate on the other ones.
			if (inBadShape) {
				curDistSqr = HUGE_DIST*HUGE_DIST*0.5f;
			}
			// See if other ai's are attacking this target.
			// We don't want the ai's to gang up on one enemy.
			Int k;
			for (k=0; k<ThePlayerList->getPlayerCount(); k++) {
				if (k==i) continue;  // don't count self.
				Player *somePlayer = ThePlayerList->getNthPlayer(k);
				if (somePlayer->isSkirmishAIPlayer() && (somePlayer->getCurrentEnemy()==curPlayer)) {
					// Some ai is already targeting this guy.  Add a distance penalty.
					curDistSqr += (500*500);
				}
			}
			if (ShouldPreferSkirmishRetaliation(curPlayer->isSkirmishAIPlayer(), curPlayer->getCurrentEnemy()==m_player)) {
				// He is attacking me.  So I will (gently) prefer to attack him.
				curDistSqr -= (25*25);
				if (curDistSqr<0) curDistSqr = 0;
			}

			// Ai enemy - will take if we don't get a better offer.
			if (curDistSqr<bestDistanceSqr) {
				bestEnemy = curPlayer;
				bestDistanceSqr = curDistSqr;
			}
		}
	}
	if (bestEnemy!=nullptr && (bestEnemy!=m_currentEnemy)) {
		m_currentEnemy = bestEnemy;
		m_currentEnemyPlayerIndex = m_currentEnemy->getPlayerIndex();
		AsciiString msg = TheNameKeyGenerator->keyToName(m_player->getPlayerNameKey());
		msg.concat(" acquiring target enemy player: ");
		msg.concat(TheNameKeyGenerator->keyToName(m_currentEnemy->getPlayerNameKey()));
		TheScriptEngine->AppendDebugMessage( msg, false);
	}
}

/**
	Get the AI's enemy.  Recalc if it has been a while (5 seconds.)
*/
void AISkirmishPlayer::acquireEnemy()
{
	const Bool productionBehavior = usesProductionBehavior();
	std::vector<SkirmishEnemyCandidate> candidates;
	Int maximumKnownAssetValue = 0;
	Int minimumDistance = 2147483647;
	Int maximumDistance = 0;
	Bool hasKnownDistance = false;
	Object *representative = findEnemyRouteRepresentative();
	for (Int i = 0; i < ThePlayerList->getPlayerCount(); ++i) {
		Player *candidatePlayer = ThePlayerList->getNthPlayer(i);
		const Bool hasTargetObjects = candidatePlayer &&
			(productionBehavior
				? candidatePlayer->hasOffensiveTargetableObjects()
				: candidatePlayer->hasAnyObjects());
		if (!candidatePlayer ||
			m_player->getRelationship(candidatePlayer->getDefaultTeam()) != ENEMIES ||
			!hasTargetObjects)
			continue;

		SkirmishEnemyCandidate candidate;
		candidate.player = candidatePlayer;
		candidate.playerIndex = candidatePlayer->getPlayerIndex();
		candidate.knownAssetValue = getKnownEnemyAssetValue(
			candidatePlayer,
			&candidate.hasKnownObject,
			&candidate.hasKnownUnit,
			&candidate.hasKnownBuildFacility);
		candidate.hasKnownPosition = getKnownEnemyPosition(candidatePlayer, &candidate.knownPosition);
		candidate.distance = 0;
		if (candidate.hasKnownPosition) {
			double dx = (double)candidate.knownPosition.x - (double)m_baseCenter.x;
			double dy = (double)candidate.knownPosition.y - (double)m_baseCenter.y;
			double distance = sqrt(dx * dx + dy * dy);
			candidate.distance = distance >= 2147483647.0 ? 2147483647 : (Int)(distance + 0.5);
			if (candidate.distance < minimumDistance)
				minimumDistance = candidate.distance;
			if (candidate.distance > maximumDistance)
				maximumDistance = candidate.distance;
			hasKnownDistance = true;
		}
		candidate.routeClass = classifyEnemyRoute(
			representative, &candidate.knownPosition, candidate.hasKnownPosition);
		candidate.score = 0;
		if (candidate.knownAssetValue > maximumKnownAssetValue)
			maximumKnownAssetValue = candidate.knownAssetValue;
		candidates.push_back(candidate);
	}

	Bool hasBest = false;
	SkirmishEnemyCandidate *best = nullptr;
	SkirmishEnemyCandidate *current = nullptr;
	for (std::vector<SkirmishEnemyCandidate>::iterator candidate = candidates.begin();
		candidate != candidates.end(); ++candidate) {
		SkirmishAIEnemyScoreInput input;
		input.knownAssetScore = GetSkirmishAIKnownAssetScore(
			candidate->knownAssetValue, maximumKnownAssetValue);
		input.targetingThisAI = candidate->player->getCachedCurrentEnemy() == m_player;
		input.routeClass = candidate->routeClass;
		input.alliedAIsTargeting = countAlliedSkirmishAIsTargeting(candidate->player);
		input.distanceScore = candidate->hasKnownPosition && hasKnownDistance ?
			GetSkirmishAIDistanceScore(candidate->distance, minimumDistance, maximumDistance) : 0;
		input.crippled = IsSkirmishAIKnownCrippled(
			candidate->hasKnownObject,
			candidate->hasKnownUnit,
			candidate->hasKnownBuildFacility);
		SkirmishAIEnemyScoreResult result = ScoreSkirmishAIEnemy(input);
		candidate->score = result.totalScore;
		if (candidate->player == m_currentEnemy)
			current = &(*candidate);
		if (ShouldReplaceSkirmishAITargetCandidate(
			hasBest,
			candidate->score,
			candidate->playerIndex,
			best ? best->score : 0,
			best ? best->playerIndex : 0)) {
			best = &(*candidate);
			hasBest = true;
		}
		if (TheGlobalData->m_debugAI) {
			DEBUG_LOG(("AI target score player %d: assets %d retaliation %d route %d allies %d distance %d crippled %d total %d",
				candidate->playerIndex,
				result.knownAssetScore,
				result.retaliationScore,
				result.routeScore,
				result.allyTargetScore,
				result.distanceScore,
				result.crippledScore,
				result.totalScore));
		}
	}

	Player *newEnemy = m_currentEnemy;
	if (!current)
		newEnemy = best ? best->player : nullptr;
	else if (best && best->player != current->player &&
		ShouldSwitchSkirmishAITarget(true, true, current->score, best->score))
		newEnemy = best->player;

	if (newEnemy != m_currentEnemy) {
		m_currentEnemy = newEnemy;
		m_currentEnemyPlayerIndex = m_currentEnemy ? m_currentEnemy->getPlayerIndex() : -1;
		if (m_currentEnemy) {
			AsciiString msg = TheNameKeyGenerator->keyToName(m_player->getPlayerNameKey());
			msg.concat(" acquiring target enemy player: ");
			msg.concat(TheNameKeyGenerator->keyToName(m_currentEnemy->getPlayerNameKey()));
			TheScriptEngine->AppendDebugMessage(msg, false);
		}
	}

}



/**
	Get the AI's enemy.  Recalc if it has been a while (20 seconds.)
*/
Player *AISkirmishPlayer::getAiEnemy()
{
	if (!ShouldUseCurrentSkirmishAIBehavior()) {
		if (TheGameLogic->getFrame()>=m_frameToCheckEnemy) {
			m_frameToCheckEnemy = TheGameLogic->getFrame() + 5*LOGICFRAMES_PER_SECOND;
			acquireEnemyLegacy();
		}
		return m_currentEnemy;
	}

	const Bool productionBehavior = usesProductionBehavior();
	const Bool currentEnemyInvalid = m_currentEnemy &&
		(!m_currentEnemy->getDefaultTeam() ||
		 m_player->getRelationship(m_currentEnemy->getDefaultTeam()) != ENEMIES ||
		 (productionBehavior ? !m_currentEnemy->hasOffensiveTargetableObjects() :
			!m_currentEnemy->hasAnyObjects()));
	if (productionBehavior && currentEnemyInvalid) {
		m_currentEnemy = nullptr;
		m_currentEnemyPlayerIndex = -1;
		m_frameToCheckEnemy = TheGameLogic->getFrame() +
			5 * LOGICFRAMES_PER_SECOND;
		acquireEnemy();
	}
	if (ShouldEvaluateSkirmishAITarget(
		productionBehavior ? false : currentEnemyInvalid,
		TheGameLogic->getFrame(), m_frameToCheckEnemy, true)) {
		m_frameToCheckEnemy = TheGameLogic->getFrame() + 5*LOGICFRAMES_PER_SECOND;
		acquireEnemy();
	}
	return m_currentEnemy;
}

/**
	Build base defense structures on the front or flank of the base.
*/
void AISkirmishPlayer::buildAIBaseDefense(Bool flank)
{
	const AISideInfo *resInfo = TheAI->getAiData()->m_sideInfo;
	AsciiString defenseTemplateName;
	while (resInfo) {
		if (resInfo->m_side == m_player->getSide()) {
			defenseTemplateName = resInfo->m_baseDefenseStructure1;
			break;
		}
		resInfo = resInfo->m_next;
	}
	if (resInfo) {
		buildAIBaseDefenseStructure(resInfo->m_baseDefenseStructure1, flank);
	}
}

/**
	Build base defense structures on the front or flank of the base.
	Base defenses are placed as follows:
	m_baseCenter and m_baseRadius are calculated on map load.
	Defenses are placed along the this circle.
	Front defenses (!flank) are placed starting at the "Center" approach path.
	The first front defense is placed towards th Center path.  Number 2 is placed
	to the left of #1, #3 is placed to the right of #1, #4 is placed to the left of
	#2 and so on.  So it looks like:

												#1
									 #2 			#3
					#6  #4								 #5	  #7
		  #8																	#9

	The flank base defenses cover the "Flank" approach, and the "Backdoor" approach.
	They alternate between these two, so the first flank defense covers flank, and the second
	covers backdoor, and continue to alternate.  They cover the approach using the same
	pattern as front above.
	John A.

	*/
void AISkirmishPlayer::buildAIBaseDefenseStructure(const AsciiString &thingName, Bool flank)
{
	const ThingTemplate *tTemplate = TheThingFactory->findTemplate(thingName);
	if (tTemplate==nullptr) {
		DEBUG_CRASH(("Couldn't find base defense structure '%s' for side %s", thingName.str(), m_player->getSide().str()));
		return;
	}
	do {
		AsciiString pathLabel;
		if (flank) {
			if (m_curFlankBaseDefense&1) {
				pathLabel.format("%s%d", SKIRMISH_FLANK, m_player->getMpStartIndex()+1);
			}	else {
				pathLabel.format("%s%d", SKIRMISH_BACKDOOR, m_player->getMpStartIndex()+1);
			}
		}	else {
			pathLabel.format("%s%d", SKIRMISH_CENTER, m_player->getMpStartIndex()+1);
		}

		Coord3D goalPos = m_baseCenter;
		Waypoint *way = TheTerrainLogic->getClosestWaypointOnPath( &goalPos, pathLabel );
		if (way) {
			goalPos = *way->getLocation();
		} else {
			if (flank) return;
			Region2D bounds;
			const Int enemyIndex = getMyEnemyPlayerIndex();
			if (enemyIndex < 0)
				return;
			getPlayerStructureBounds(&bounds, enemyIndex);
			goalPos.x = bounds.lo.x + bounds.width()/2;
			goalPos.y = bounds.lo.y + bounds.height()/2;
		}
		Coord2D offset;
		offset.x = goalPos.x-m_baseCenter.x;
		offset.y = goalPos.y-m_baseCenter.y;
		offset.normalize();
		Real defenseDistance = m_baseRadius;
		defenseDistance += TheAI->getAiData()->m_skirmishBaseDefenseExtraDistance;
		offset.x *= defenseDistance;
		offset.y *= defenseDistance;

		Real structureRadius = tTemplate->getTemplateGeometryInfo().getBoundingCircleRadius();
		Real baseCircumference = 2*PI*defenseDistance;
		Real angleOffset = 2*PI*(structureRadius*4/baseCircumference);

		Int selector;
		Real angle;
		if (flank) {
			selector = m_curFlankBaseDefense>>1;
			if (m_curFlankBaseDefense&1) {
				if (selector&1) {
					m_curLeftFlankRightDefenseAngle -= angleOffset;
					angle = m_curLeftFlankRightDefenseAngle;
				}	else {
					angle = m_curLeftFlankLeftDefenseAngle;
					m_curLeftFlankLeftDefenseAngle += angleOffset;
				}
			}	else {
				if (selector&1) {
					m_curRightFlankRightDefenseAngle -= angleOffset;
					angle = m_curRightFlankRightDefenseAngle;
				}	else {
					angle = m_curRightFlankLeftDefenseAngle;
					m_curRightFlankLeftDefenseAngle += angleOffset;
				}
			}

		} else {
			selector = m_curFrontBaseDefense;
			if (selector&1) {
				m_curFrontRightDefenseAngle -= angleOffset;
				angle = m_curFrontRightDefenseAngle;
			}	else {
				angle = m_curFrontLeftDefenseAngle;
				m_curFrontLeftDefenseAngle += angleOffset;
			}
		}

		if (angle > PI/3) break;
		Real s = sin(angle);
		Real c = cos(angle);

// TheSuperHackers @info helmutbuhler 21/04/2025 This debug mutates the code to become CRC incompatible
#if defined(RTS_DEBUG) || !RETAIL_COMPATIBLE_CRC
		DEBUG_LOG(("buildAIBaseDefenseStructure -- Angle is %f sin %f, cos %f", 180*angle/PI, s, c));
		DEBUG_LOG(("buildAIBaseDefenseStructure -- Offset is %f  %f, Final Position is %f, %f",
			offset.x, offset.y,
			offset.x*c - offset.y*s,
			offset.y*c + offset.x*s
			));
#endif
		Coord3D buildPos = m_baseCenter;
		buildPos.x += offset.x*c - offset.y*s;
		buildPos.y += offset.y*c + offset.x*s;

		/* See if we can build there. */
		Bool canBuild;
		Real placeAngle = tTemplate->getPlacementViewAngle();
		canBuild = LBC_OK == TheBuildAssistant->isLocationLegalToBuild(&buildPos, tTemplate, placeAngle,
			BuildAssistant::TERRAIN_RESTRICTIONS|BuildAssistant::NO_OBJECT_OVERLAP, nullptr, m_player);
		TheTerrainVisual->removeAllBibs();	// isLocationLegalToBuild adds bib feedback, turn it off.  jba.
		if (flank) {
			m_curFlankBaseDefense++;
		} else {
			m_curFrontBaseDefense++;
		}
		if (canBuild) {
			m_player->addToPriorityBuildList(thingName, &buildPos, placeAngle);
			break;
		}
	}	while (true);

}


/**
	Checks bridges along a waypoint path.  If any are destroyed, sends a dozer to fix, and returns true.
	If there is no bridge problem, returns false.
	*/
Bool AISkirmishPlayer::checkBridges(Object *unit, Waypoint *way)
{
	Coord3D unitPos = *unit->getPosition();
	AIUpdateInterface *ai = unit->getAI();
	if (!ai) return false; // no ai
	const LocomotorSet& locoSet = ai->getLocomotorSet();
	Waypoint *curWay;
	for (curWay = way; curWay; curWay = curWay->getNext()) {
		if (TheAI->pathfinder()->clientSafeQuickDoesPathExist(locoSet, &unitPos, curWay->getLocation())) {
			continue;
		}
		ObjectID brokenBridge = INVALID_ID;
		if (TheAI->pathfinder()->findBrokenBridge(locoSet, &unitPos, curWay->getLocation(), &brokenBridge)) {
			repairStructure(brokenBridge);
			return true;
		}
	}
	return false;

}


/**
	Build a specific team.  If priorityBuild, put at front of queue with priority set.
	*/
void AISkirmishPlayer::buildSpecificAITeam( TeamPrototype *teamProto, Bool priorityBuild)
{
	AIPlayer::buildSpecificAITeam(teamProto, priorityBuild);
}


/**
	Recruit a specific team, within the specific radius of the home position.
	*/
void AISkirmishPlayer::recruitSpecificAITeam(TeamPrototype *teamProto, Real recruitRadius)
{
	if (recruitRadius < 1) recruitRadius = 99999.0f;
	//
	// Create "Team in queue" based on team population
	//
	if (teamProto)
	{
		if (teamProto->getIsSingleton()) {
			Team *singletonTeam = TheTeamFactory->findTeam( teamProto->getName() );
			if (singletonTeam && singletonTeam->hasAnyObjects()) {
				AsciiString teamStr = "Unable to recruit singleton team '";
				teamStr.concat("' because team already exists.");
				TheScriptEngine->AppendDebugMessage(teamStr, false);
				return;
			}
		}
		if (!teamProto->getTemplateInfo()->m_hasHomeLocation)
		{
			AsciiString teamStr = "Error : team '";
			teamStr.concat(teamProto->getName());
			teamStr.concat("' has no Home Position (or Origin).");
			TheScriptEngine->AppendDebugMessage(teamStr, false);
		}
		// create inactive team to place members into as they are built
		// when team is complete, the team is activated
		Team *theTeam = TheTeamFactory->createInactiveTeam( teamProto->getName() );
		AsciiString teamName = teamProto->getName();
		teamName.concat(" - Recruiting.");
		TheScriptEngine->AppendDebugMessage(teamName, false);
		const TCreateUnitsInfo *unitInfo = &teamProto->getTemplateInfo()->m_unitsInfo[0];
//		WorkOrder *orders = nullptr;
		Int i;
		Int unitsRecruited = 0;
		// Recruit.
		for( i=0; i<teamProto->getTemplateInfo()->m_numUnitsInfo; i++ )
		{
			const ThingTemplate *thing = TheThingFactory->findTemplate( unitInfo[i].unitThingName );
			if (thing)
			{
				int count = unitInfo[i].maxUnits;
				while (count>0) {
					Object *unit = theTeam->tryToRecruit(thing, &teamProto->getTemplateInfo()->m_homeLocation, recruitRadius);
					if (unit)
					{
						unitsRecruited++;

						AsciiString teamStr = "Team '";
						teamStr.concat(theTeam->getPrototype()->getName());
						teamStr.concat("' recruits ");
						teamStr.concat(thing->getName());
						teamStr.concat(" from team '");
						teamStr.concat(unit->getTeam()->getPrototype()->getName());
						teamStr.concat("'");
						TheScriptEngine->AppendDebugMessage(teamStr, false);

						unit->setTeam(theTeam);

						AIUpdateInterface *ai = unit->getAIUpdateInterface();
						if (ai)
						{
#ifdef DEBUG_LOGGING
							Coord3D pos = *unit->getPosition();
							Coord3D to = teamProto->getTemplateInfo()->m_homeLocation;
							DEBUG_LOG(("Moving unit from %f,%f to %f,%f", pos.x, pos.y , to.x, to.y ));
#endif
							ai->aiMoveToPosition( &teamProto->getTemplateInfo()->m_homeLocation, CMD_FROM_AI);
						}
					} else {
						break;
					}
					count--;
				}
			}
		}
		if (unitsRecruited>0)
		{
			/* We have something to build. */
			TeamInQueue *team = newInstance(TeamInQueue);
			// Put in front of queue.
			prependTo_TeamReadyQueue(team);
			team->m_priorityBuild = false;
			team->m_workOrders = nullptr;
			team->m_frameStarted = TheGameLogic->getFrame();
			team->m_team = theTeam;
			AsciiString teamName = teamProto->getName();
			teamName.concat(" - Finished recruiting.");
			TheScriptEngine->AppendDebugMessage(teamName, false);
		}	else {
			//disband.
			if (!theTeam->getPrototype()->getIsSingleton()) {
				deleteInstance(theTeam);
				theTeam = nullptr;
			}
			AsciiString teamName = teamProto->getName();
			teamName.concat(" - Recruited 0 units, disbanding.");
			TheScriptEngine->AppendDebugMessage(teamName, false);
		}
	}
}




/**
 * Train our teams.
 */
void AISkirmishPlayer::processTeamBuilding()
{
	// select a new team
	if (selectTeamToBuild()) {
		queueUnits();
	}
}

//----------------------------------------------------------------------------------------------------------
/**
 * See if it's time to build another base building.
 */
void AISkirmishPlayer::doBaseBuilding()
{
	if (usesCriticalRecoveryBehavior())
		updateCriticalRecovery();
	// The recovery controller owns all base-spend decisions until the missing
	// completed center has a real construction object or enters last stand.
	if (usesCriticalRecoveryBehavior() && m_recoveryEverCompleted &&
		!m_recoveryImpossible && m_recoveryConstructionID == INVALID_ID &&
		m_recoveryReserveCost > 0)
		return;
	if (m_player->getCanBuildBase()) {
		// See if we are ready to start trying a structure.
		if (!m_readyToBuildStructure) {
			m_structureTimer--;
			if (m_structureTimer<=0) {
				m_readyToBuildStructure = true;
				m_buildDelay = 0;
			}
			if (m_structureTimer > 3*LOGICFRAMES_PER_SECOND) {
				m_structureTimer = 3*LOGICFRAMES_PER_SECOND;
			}
		}
		// This timer is to keep from banging on the logic each frame.  If something interesting
		// happens, like a building is added or a unit finished, the timers are shortcut.
		m_buildDelay--;
		if (m_buildDelay<1) {
			if (m_readyToBuildStructure) {
				processBaseBuilding();
			}
			if (m_buildDelay<1) {	// processBaseBuilding may reset m_buildDelay.
				m_buildDelay = 2*LOGICFRAMES_PER_SECOND; // check again in 2 seconds.
			}
			// Note that this timer gets shortcut when a building is completed.
		}
	}
}

//----------------------------------------------------------------------------------------------------------
/**
 * See if any ready teams have finished moving to the rally point.
 */
void AISkirmishPlayer::checkReadyTeams()
{
	AIPlayer::checkReadyTeams();
}

//----------------------------------------------------------------------------------------------------------
/**
 * Hold completed offensive teams while Fortify is active. Reinforcements and
 * defensive teams remain available, and permanent recovery last stand releases
 * every ready team.
 */
Bool AISkirmishPlayer::canActivateReadyTeam( const TeamInQueue *team ) const
{
	if (!team || !usesStrategyBehavior() ||
		m_strategyState.currentMode != SKIRMISH_STRATEGY_FORTIFY)
		return true;
	if (team->m_reinforcement || m_recoveryImpossible)
		return true;
	if (!team->m_team || team->m_team == m_player->getDefaultTeam() ||
		!team->m_team->getPrototype())
		return true;
	const TeamTemplateInfo *info = team->m_team->getPrototype()->getTemplateInfo();
	return !info || info->m_isBaseDefense || info->m_isPerimeterDefense;
}

//----------------------------------------------------------------------------------------------------------
/**
 * See if any queued teams have finished building, or have run out of time.
 */
void AISkirmishPlayer::checkQueuedTeams()
{
	AIPlayer::checkQueuedTeams();
}

//----------------------------------------------------------------------------------------------------------
/**
 * See if it is time to start another ai team building.
 */
void AISkirmishPlayer::doTeamBuilding()
{
	// See if any teams are expired.
	if (m_player->getCanBuildUnits()) {
		// See if we are ready to start trying a team.
		if (!m_readyToBuildTeam) {
			m_teamTimer--;
			if (m_teamTimer<=0) {
				m_readyToBuildTeam = true;
				m_teamDelay = 0;
			}
			if (m_teamTimer > 3*LOGICFRAMES_PER_SECOND) {
				m_teamTimer = 3*LOGICFRAMES_PER_SECOND;
			}
		}

		// This timer is to keep from banging on the logic each frame.  If something interesting
		// happens, like a building is added or a unit finished, the timers are shortcut.
		m_teamDelay--;
		if (m_teamDelay<1) {
			#if defined(_WIN64)
			if (!consumeProductionPlanningQueue())
				queueUnits(); // update the queues.
			#else
			queueUnits(); // update the queues.
			#endif
			if (m_readyToBuildTeam) {
				#if defined(_WIN64)
				if (!consumeProductionPlanningHandled())
					processTeamBuilding();
				#else
				processTeamBuilding();
				#endif
			}
			m_teamDelay = 2*LOGICFRAMES_PER_SECOND; // check again in 5 seconds.
			// Note that this timer gets shortcut when a unit or building is completed.
		}
	}
}

//----------------------------------------------------------------------------------------------------------
/**
 * Perform computer-controlled player AI
 */
void AISkirmishPlayer::update()
{
	const SkirmishStrategyMode previousMode = m_strategyState.currentMode;
	const ObjectID previousTargetID = m_strategyState.strategicTargetID;
	const Bool strategyAllowedAfterRecovery =
		ShouldContinueSkirmishAIStrategyAfterRecovery(
			m_recoveryImpossible, usesProductionBehavior());
	Bool strategyEvaluated = false;
	Bool strategyTargetExpired = false;
	if (strategyAllowedAfterRecovery && usesStrategyBehavior() &&
		m_strategyState.currentMode == SKIRMISH_STRATEGY_ASSAULT &&
		m_strategyState.strategicTargetID != INVALID_ID &&
		m_strategyState.strategicTargetObserved &&
		!IsSkirmishStrategyTargetObservationAvailable(
			m_strategyState.strategicTargetObserved, TheGameLogic->getFrame(),
			m_strategyState.strategicTargetLastSeenFrame)) {
		// This scalar check runs every frame so the fair target grace is an exact
		// bound.  Clearing the ID makes command dispatch idle assault groups once.
		ClearSkirmishStrategyTargetObservation(&m_strategyState);
		strategyTargetExpired = true;
	}
	if (ShouldUseCurrentSkirmishAIBehavior()) {
		getAiEnemy();
		if (strategyAllowedAfterRecovery)
			strategyEvaluated = updateStrategy();
	}
	AIPlayer::update();
	if ((strategyEvaluated || strategyTargetExpired) &&
		strategyAllowedAfterRecovery)
		applyStrategyMode(
			previousMode, m_strategyState.currentMode, previousTargetID);
}

//----------------------------------------------------------------------------------------------------------
/**
 * Adjusts the build list to match the starting position.
 */
void AISkirmishPlayer::adjustBuildList(BuildListInfo *list)
{
	Bool foundStart = false;
	Coord3D startPos;

	// Find our command center location.
	Object *obj;
	for( obj = TheGameLogic->getFirstObject(); obj; obj = obj->getNextObject() )
	{

		Player *owner = obj->getControllingPlayer();
		if (owner==m_player) {
			// See if it's a command center.
			if (obj->isKindOf(KINDOF_COMMANDCENTER)) {
				foundStart = true;
				startPos = *obj->getPosition();
				m_player->onStructureUndone(obj);
				TheAI->pathfinder()->removeObjectFromPathfindMap(obj);
				TheGameLogic->destroyObject(obj);
				break;
			}
		}
	}
	if (!foundStart) {
		DEBUG_LOG(("Couldn't find starting command center for ai player."));
		return;
	}
	// Find the location of the command center in the build list.
	Bool foundInBuildList = false;
	Coord3D buildPos;
	BuildListInfo *cur = list;
	while (cur) {
		const ThingTemplate *tTemplate = TheThingFactory->findTemplate(cur->getTemplateName());
		if (tTemplate && tTemplate->isKindOf(KINDOF_COMMANDCENTER)) {
			foundInBuildList = true;
			buildPos = *cur->getLocation();
			cur->setInitiallyBuilt(true);
		}
		cur = cur->getNext();
	}
	Region3D bounds;
	TheTerrainLogic->getMaximumPathfindExtent(&bounds);
	/* calculate section of 3x3 grid:
		6 7 8
		3 4 5
		0 1 2 */

	Int gridIndex = 0;
	if (startPos.x > bounds.lo.x + bounds.width()/3) {
		gridIndex++;
	}
	if (startPos.x > bounds.lo.x + 2*bounds.width()/3) {
		gridIndex++;
	}

	if (startPos.y > bounds.lo.y + bounds.height()/3) {
		gridIndex+=3;
	}
	if (startPos.y > bounds.lo.y + 2*bounds.height()/3) {
		gridIndex+=3;
	}

	Real angle = 0;
	if (TheAI->getAiData()->m_rotateSkirmishBases) {
		switch (gridIndex) {
			case 0 : angle = 0; break;
			case 1 : angle = PI/4; break;// 45 degrees.
			case 2 : angle = PI/2; break; // 90 degrees;
			case 3 : angle = -PI/4; break; // -45 degrees.
			case 4 : angle = 0; break;
			case 5 : angle = 3*PI/4; break; // 135 degrees.
			case 6 : angle = -PI/2; break; // -90 degrees;
			case 7 : angle = -3*PI/4; break; // -135 degrees.
			case 8 : angle = PI; break; // 180 degrees.
		}
	}

	angle += 3*PI/4;

	Real s = sin(angle);
	Real c = cos(angle);

	cur = list;
	while (cur) {
		const ThingTemplate *tTemplate = TheThingFactory->findTemplate(list->getTemplateName());
		if (tTemplate && tTemplate->isKindOf(KINDOF_COMMANDCENTER)) {
			foundInBuildList = true;
			Coord3D curPos = *cur->getLocation();
			// Transform to new coords.
			curPos.x -= buildPos.x;
			curPos.y -= buildPos.y;
			Real newX = curPos.x*c - curPos.y*s;
			Real newY = curPos.y*c + curPos.x*s;
			curPos.x = newX + startPos.x;
			curPos.y = newY + startPos.y;
			cur->setLocation(curPos);
			cur->setAngle(cur->getAngle());
		}
		cur = cur->getNext();
	}

}



//----------------------------------------------------------------------------------------------------------
/**
 * Find any things that build stuff & add them to the build list.  Then build any initially built
 * buildings.
 */
void AISkirmishPlayer::newMap()
{
	InitializeSkirmishStrategyState(
		&m_strategyState, TheGameLogic ? TheGameLogic->getFrame() : 0);
	m_strategyProductionReserveCost = 0;
	m_strategySuperweaponID = INVALID_ID;
	m_strategyAuthorizedThing = nullptr;
	m_strategySpendAuthorization = SKIRMISH_AI_SPEND_AUTHORIZATION_NONE;
	m_strategyProductionReserveRefreshing = false;
	clearStrategySourceCommandLock();
	m_reinforcementRoundRobinCursor = 0;

	/* Get our proper build list. */
	AsciiString mySide = m_player->getSide();
	DEBUG_LOG(("AI Player side is %s", mySide.str()));
	const AISideBuildList *build = TheAI->getAiData()->m_sideBuildLists;
	while (build) {
		if (build->m_side == mySide) {
			BuildListInfo *buildList = build->m_buildList->duplicate();
			adjustBuildList(buildList); // adjust to  our start position.
			m_player->setBuildList(buildList);
			computeCenterAndRadiusOfBase(&m_baseCenter, &m_baseRadius);
			break;
		}
		build = build->m_next;
	}
	DEBUG_ASSERTLOG(build!=nullptr, ("Couldn't find build list for skirmish player."));

	// Build any with the initially built flag.
	for( BuildListInfo *info = m_player->getBuildList(); info; info = info->getNext() )
	{
		AsciiString name = info->getTemplateName();
		if (name.isEmpty()) continue;
		const ThingTemplate *bldgPlan = TheThingFactory->findTemplate( name );
		if (!bldgPlan) {
			DEBUG_LOG(("*** ERROR - Build list building '%s' doesn't exist.", name.str()));
			continue;
		}
		if (info->isInitiallyBuilt()) {
			buildStructureNow(bldgPlan, info);
		} else {
			info->incrementNumRebuilds(); // the initial build in the normal build list consumes a rebuild, so add one.
		}
	}

	// Initial construction remains the existing instant-build path.  Record
	// only the completed primary center so recovery cannot mistake an
	// incomplete captured center for prior ownership.
	if (usesCriticalRecoveryBehavior() && m_player->getPlayerTemplate()) {
		const ThingTemplate *primaryTemplate = TheThingFactory->findTemplate(
			m_player->getPlayerTemplate()->getStartingBuilding());
		Object *center = nullptr;
		if (primaryTemplate && findPrimaryCommandCenter(primaryTemplate, &center) &&
			center && !center->testStatus(OBJECT_STATUS_UNDER_CONSTRUCTION)) {
			m_recoveryEverCompleted = true;
			m_recoveryImpossible = false;
			m_recoveryConstructionID = center->getID();
			m_recoveryEvacuationDeadline = 0;
			m_recoveryBuilderFailoverConsumed =
				ReconcileSkirmishAIRecoveryBuilderFailoverConsumed(
					m_recoveryBuilderFailoverConsumed, true, true);
			m_recoveryLocation = *center->getPosition();
			m_recoveryAngle = center->getOrientation();
			m_recoveryReserveCost = 0;
		}
	}
}

//----------------------------------------------------------------------------------------------------------
/**
 * Queues up a dozer.
 */
void AISkirmishPlayer::queueDozer()
{
	AIPlayer::queueDozer();
}

//----------------------------------------------------------------------------------------------------------
/**
 * Finds a dozer that isn't building or collecting resources.
 */
Object * AISkirmishPlayer::findDozer( const Coord3D *pos )
{
	return AIPlayer::findDozer(pos);
}


//----------------------------------------------------------------------------------------------------------
/**
 * Find a good spot to fire a superweapon.
 */
Bool AISkirmishPlayer::computeSuperweaponTarget(const SpecialPowerTemplate *power, Coord3D *retPos, Int playerNdx, Real weaponRadius)
{
	const Bool defensiveMines = power &&
		(power->getSpecialPowerType() == SPECIAL_CLUSTER_MINES ||
		 power->getSpecialPowerType() == NUKE_SPECIAL_CLUSTER_MINES);
	if (usesProductionBehavior() && !defensiveMines) {
		if (!power || !retPos || !ThePlayerList || playerNdx < 0 ||
			playerNdx >= ThePlayerList->getPlayerCount())
			return false;
		Player *target = ThePlayerList->getNthPlayer(playerNdx);
		if (!IsSkirmishAIOffensiveTargetEligible(
			target != nullptr,
			target && target->getDefaultTeam() &&
				m_player->getRelationship(target->getDefaultTeam()) == ENEMIES,
			target && target->hasOffensiveTargetableObjects()))
			return false;
	}

	if( power->getSpecialPowerType() == SPECIAL_CLUSTER_MINES || power->getSpecialPowerType() == NUKE_SPECIAL_CLUSTER_MINES )
	{
		// hackus brutus - mine the entrances to our base.
		AsciiString pathLabel;
		Int mode = GameLogicRandomValue(0, 2);
		if (mode==1) {
				pathLabel.format("%s%d", SKIRMISH_FLANK, m_player->getMpStartIndex()+1);
		}	else if (mode==2) {
				pathLabel.format("%s%d", SKIRMISH_BACKDOOR, m_player->getMpStartIndex()+1);
		}	else {
			pathLabel.format("%s%d", SKIRMISH_CENTER, m_player->getMpStartIndex()+1);
		}

		Coord3D goalPos = m_baseCenter;
		Waypoint *way = TheTerrainLogic->getClosestWaypointOnPath( &goalPos, pathLabel );
		if (way) {
			goalPos = *way->getLocation();
		} else {
			Region2D bounds;
			const Int referenceIndex = usesProductionBehavior() ?
				m_player->getPlayerIndex() : getMyEnemyPlayerIndex();
			if (referenceIndex < 0)
				return FALSE;
			getPlayerStructureBounds(&bounds, referenceIndex);
			goalPos.x = bounds.lo.x + bounds.width()/2;
			goalPos.y = bounds.lo.y + bounds.height()/2;
		}
		Coord2D offset;
		offset.x = goalPos.x-m_baseCenter.x;
		offset.y = goalPos.y-m_baseCenter.y;
		offset.normalize();
		offset.x *= m_baseRadius;
		offset.y *= m_baseRadius;
		*retPos = m_baseCenter;
		retPos->x += offset.x;
		retPos->y += offset.y;
		retPos->z = TheTerrainLogic->getGroundHeight(retPos->x, retPos->y);
		return TRUE;
	}

	return AIPlayer::computeSuperweaponTarget(power, retPos, playerNdx, weaponRadius);

}

static void XferSkirmishStrategyState(
	Xfer *xfer, SkirmishStrategyState *state)
{
	Int currentMode = (Int)state->currentMode;
	Int pendingMode = (Int)state->pendingMode;
	Int fortifyAttemptStatus = (Int)state->fortifyAttemptStatus;
	Int superweaponAttemptStatus = (Int)state->superweaponAttemptStatus;
	xfer->xferInt(&currentMode);
	xfer->xferInt(&pendingMode);
	xfer->xferUnsignedInt(&state->modeEntryFrame);
	xfer->xferUnsignedInt(&state->pendingSinceFrame);
	xfer->xferUnsignedInt(&state->nextEvaluationFrame);
	xfer->xferObjectID(&state->strategicTargetID);
	xfer->xferBool(&state->strategicTargetObserved);
	xfer->xferUnsignedInt(&state->strategicTargetLastSeenFrame);
	xfer->xferInt(&fortifyAttemptStatus);
	xfer->xferInt(&superweaponAttemptStatus);
	xfer->xferBool(&state->assaultAssemblyDeadlineActive);
	xfer->xferUnsignedInt(&state->assaultAssemblyDeadlineFrame);
	xfer->xferBool(&state->alliedCoordinationCooldownActive);
	xfer->xferUnsignedInt(&state->nextAlliedCoordinationFrame);
	xfer->xferBool(&state->donationCooldownActive);
	xfer->xferUnsignedInt(&state->nextDonationFrame);
	xfer->xferInt(&state->assaultEntryCombatValue);
	if (xfer->getXferMode() == XFER_LOAD) {
		state->currentMode = (SkirmishStrategyMode)currentMode;
		state->pendingMode = (SkirmishStrategyMode)pendingMode;
		state->fortifyAttemptStatus =
			(SkirmishStrategyAttemptStatus)fortifyAttemptStatus;
		state->superweaponAttemptStatus =
			(SkirmishStrategyAttemptStatus)superweaponAttemptStatus;
	}
}

// ------------------------------------------------------------------------------------------------
/** CRC */
// ------------------------------------------------------------------------------------------------
void AISkirmishPlayer::crc( Xfer *xfer )
{
	if (!usesCriticalRecoveryBehavior() && !usesProductionBehavior())
		return;
	xfer->xferBool(&m_recoveryEverCompleted);
	xfer->xferBool(&m_recoveryImpossible);
	xfer->xferObjectID(&m_recoveryConstructionID);
	xfer->xferInt(&m_recoveryPlacementAttempt);
	xfer->xferUnsignedInt(&m_recoveryNextAttemptFrame);
	// Epoch 3 retains its recorded CRC layout. Epoch 4 adds the deadline and
	// exact production identity. Epochs 5 and 6 add cancellation ownership.
	// Live games and epoch 7 also cover one-shot paid-queue failover state.
	const Bool replay = TheGameLogic && TheGameLogic->isInReplayGame();
	const Int replayEpoch = TheRecorder
		? TheRecorder->getSkirmishAIReplayEpoch()
		: SKIRMISH_AI_REPLAY_EPOCH_LEGACY;
	if (ShouldIncludeSkirmishAIRecoveryCRCFields(replay, replayEpoch)) {
		xfer->xferUnsignedInt(&m_recoveryEvacuationDeadline);
		xfer->xferObjectID(&m_recoveryBuilderFactoryID);
		xfer->xferUser(
			&m_recoveryBuilderProductionID, sizeof(ProductionID));
	}
	if (ShouldIncludeSkirmishAIRecoveryCancellationOwnershipCRCField(
			replay, replayEpoch)) {
		xfer->xferBool(&m_recoveryBuilderCancellationOwned);
	}
	if (ShouldIncludeSkirmishAIRecoveryFailoverConsumedCRCField(
			replay, replayEpoch)) {
		xfer->xferBool(&m_recoveryBuilderFailoverConsumed);
	}
	xfer->xferCoord3D(&m_recoveryLocation);
	xfer->xferReal(&m_recoveryAngle);
	xfer->xferInt(&m_recoveryReserveCost);
	if (ShouldIncludeSkirmishAIStrategyCRCFields(replay, replayEpoch))
		XferSkirmishStrategyState(xfer, &m_strategyState);
	if (ShouldIncludeSkirmishAIProductionCRCFields(replay, replayEpoch)) {
		xfer->xferInt(&m_strategyProductionReserveCost);
		xfer->xferObjectID(&m_strategySuperweaponID);
		xfer->xferBool(&m_strategySourceCommandLocked);
		xfer->xferObjectID(&m_strategyLockedSourceID);
		xfer->xferUnsignedInt(&m_strategyLockedPowerID);
		xfer->xferUnsignedInt(&m_reinforcementRoundRobinCursor);
		AIPlayer::crc(xfer);
		std::map<ObjectID, Bool> collectorRoles;
		GatherSkirmishAICollectorRoles(m_player, &collectorRoles);
		UnsignedInt roleCount = static_cast<UnsignedInt>(collectorRoles.size());
		xfer->xferUnsignedInt(&roleCount);
		for (std::map<ObjectID, Bool>::iterator role = collectorRoles.begin();
			role != collectorRoles.end(); ++role) {
			ObjectID objectID = role->first;
			Bool collector = role->second;
			xfer->xferObjectID(&objectID);
			xfer->xferBool(&collector);
		}
	}
}

// ------------------------------------------------------------------------------------------------
/** Xfer method
	* Version Info;
	* 1: Initial version
	* 2: Current enemy and next enemy evaluation frame
	* 3: Critical command-center recovery state
	* 4: Contained-builder evacuation grace deadline
	* 5: Recovery builder production identity
	* 6: Recovery builder cancellation ownership
	* 7: Recovery builder bounded-failover consumption
	* 8: Deterministic skirmish strategy controller state
	 * 9: Stage 3 production reserve and Fortify superweapon identity
	 * 10: One-shot Fortify strategic-source command lock
	 * 11: Exact strategic command identity and reinforcement fairness cursor
	 * 12: Stage 3 worker collector roles, keyed by ObjectID */
// ------------------------------------------------------------------------------------------------
void AISkirmishPlayer::xfer( Xfer *xfer )
{

	// version
	XferVersion currentVersion = 12;
	XferVersion version = currentVersion;
	xfer->xferVersion( &version, currentVersion );

	// xfer base class info
	AIPlayer::xfer( xfer );

	// front base defense
	xfer->xferInt( &m_curFrontBaseDefense );

	// flank base defense
	xfer->xferInt( &m_curFlankBaseDefense );

	// front left defense angle
	xfer->xferReal( &m_curFrontLeftDefenseAngle );

	// front right defense angle
	xfer->xferReal( &m_curFrontRightDefenseAngle );

	// left flank left defense angle
	xfer->xferReal( &m_curLeftFlankLeftDefenseAngle );

	// left flank right defense angle
	xfer->xferReal( &m_curLeftFlankRightDefenseAngle );

	// right flank left defense angle
	xfer->xferReal( &m_curRightFlankLeftDefenseAngle );

	// right flank right defense angle
	xfer->xferReal( &m_curRightFlankRightDefenseAngle );

	if (xfer->getXferMode() == XFER_SAVE)
		m_currentEnemyPlayerIndex = m_currentEnemy ? m_currentEnemy->getPlayerIndex() : -1;
	UnsignedInt nextEvaluationFrame = m_frameToCheckEnemy;
	if (version >= 2) {
		xfer->xferInt(&m_currentEnemyPlayerIndex);
		xfer->xferUnsignedInt(&nextEvaluationFrame);
	}
	if (xfer->getXferMode() == XFER_LOAD) {
		SkirmishAITargetSnapshotState state = GetSkirmishAITargetSnapshotState(
			version, m_currentEnemyPlayerIndex, nextEvaluationFrame);
		m_currentEnemy = nullptr;
		m_currentEnemyPlayerIndex = state.enemyPlayerIndex;
		m_frameToCheckEnemy = state.nextEvaluationFrame;
	}
	if (version >= 3) {
		xfer->xferBool(&m_recoveryEverCompleted);
		xfer->xferBool(&m_recoveryImpossible);
		xfer->xferObjectID(&m_recoveryConstructionID);
		xfer->xferInt(&m_recoveryPlacementAttempt);
		xfer->xferUnsignedInt(&m_recoveryNextAttemptFrame);
		xfer->xferCoord3D(&m_recoveryLocation);
		xfer->xferReal(&m_recoveryAngle);
		xfer->xferInt(&m_recoveryReserveCost);
	} else if (xfer->getXferMode() == XFER_LOAD) {
		m_recoveryEverCompleted = false;
		m_recoveryImpossible = false;
		m_recoveryConstructionID = INVALID_ID;
		m_recoveryPlacementAttempt = 0;
		m_recoveryNextAttemptFrame = 0;
		m_recoveryLocation.zero();
		m_recoveryAngle = 0.0f;
		m_recoveryReserveCost = 0;
	}
	if (version >= 4)
		xfer->xferUnsignedInt(&m_recoveryEvacuationDeadline);
	else if (xfer->getXferMode() == XFER_LOAD)
		m_recoveryEvacuationDeadline =
			GetSkirmishAIRecoveryEvacuationDeadlineForVersion(version, 0);
	if (version >= 5) {
		xfer->xferObjectID(&m_recoveryBuilderFactoryID);
		xfer->xferUser(&m_recoveryBuilderProductionID, sizeof(ProductionID));
	} else if (xfer->getXferMode() == XFER_LOAD) {
		SkirmishAIRecoveryProductionIdentity identity =
			GetSkirmishAIRecoveryProductionIdentityForVersion(
				version, INVALID_ID, PRODUCTIONID_INVALID,
				INVALID_ID, PRODUCTIONID_INVALID);
		m_recoveryBuilderFactoryID = static_cast<ObjectID>(identity.factoryID);
		m_recoveryBuilderProductionID =
			static_cast<ProductionID>(identity.productionID);
	}
	if (version >= 6)
		xfer->xferBool(&m_recoveryBuilderCancellationOwned);
	else if (xfer->getXferMode() == XFER_LOAD)
		m_recoveryBuilderCancellationOwned =
			GetSkirmishAIRecoveryProductionCancellationOwnershipForVersion(
				version, false);
	if (version >= 7)
		xfer->xferBool(&m_recoveryBuilderFailoverConsumed);
	else if (xfer->getXferMode() == XFER_LOAD)
		m_recoveryBuilderFailoverConsumed =
			GetSkirmishAIRecoveryBuilderFailoverConsumedForVersion(
				version, false);
	if (version >= 8)
		XferSkirmishStrategyState(xfer, &m_strategyState);
	else if (xfer->getXferMode() == XFER_LOAD)
		InitializeOldSaveSkirmishStrategyState(
			&m_strategyState, TheGameLogic ? TheGameLogic->getFrame() : 0);
	if (xfer->getXferMode() == XFER_LOAD && version == 8 &&
		usesProductionBehavior())
		ArmOldSaveSkirmishFortifyDeadline(
			&m_strategyState, m_difficulty,
			TheGameLogic ? TheGameLogic->getFrame() : 0);
	if (version >= 9) {
		xfer->xferInt(&m_strategyProductionReserveCost);
		xfer->xferObjectID(&m_strategySuperweaponID);
	} else if (xfer->getXferMode() == XFER_LOAD) {
		m_strategyProductionReserveCost = 0;
		m_strategySuperweaponID = INVALID_ID;
	}
	if (version >= 10)
		xfer->xferBool(&m_strategySourceCommandLocked);
	else if (xfer->getXferMode() == XFER_LOAD)
		m_strategySourceCommandLocked =
			GetSkirmishAIStrategicPowerSourceLockForVersion(version, false);
	if (version >= 11) {
		xfer->xferObjectID(&m_strategyLockedSourceID);
		xfer->xferUnsignedInt(&m_strategyLockedPowerID);
		xfer->xferUnsignedInt(&m_reinforcementRoundRobinCursor);
	} else if (xfer->getXferMode() == XFER_LOAD) {
		clearStrategySourceCommandLock();
		m_reinforcementRoundRobinCursor =
			GetSkirmishAIReinforcementCursorForVersion(version, 0);
	}
	// WorkerAIUpdate remains v1 in retail-compatible saves. Keep the Stage 3
	// role here so an idle worker can resume as a collector after loading.
	m_stage3CollectorRolesToRestore.clear();
	if (version >= 12) {
		std::map<ObjectID, Bool> collectorRoles;
		if (xfer->getXferMode() == XFER_SAVE)
			GatherSkirmishAICollectorRoles(m_player, &collectorRoles);
		UnsignedInt roleCount = static_cast<UnsignedInt>(collectorRoles.size());
		xfer->xferUnsignedInt(&roleCount);
		if (xfer->getXferMode() == XFER_SAVE) {
			for (std::map<ObjectID, Bool>::iterator role = collectorRoles.begin();
				role != collectorRoles.end(); ++role) {
				ObjectID objectID = role->first;
				Bool collector = role->second;
				xfer->xferObjectID(&objectID);
				xfer->xferBool(&collector);
			}
		} else {
			for (UnsignedInt i = 0; i < roleCount; ++i) {
				ObjectID objectID = INVALID_ID;
				Bool collector = false;
				xfer->xferObjectID(&objectID);
				xfer->xferBool(&collector);
				m_stage3CollectorRolesToRestore[objectID] = collector;
			}
		}
	}
	if (xfer->getXferMode() == XFER_LOAD) {
		m_strategyProductionReserveLoaded = version >= 9;
	}
	m_recoveryAuthorizedThing = nullptr;
	m_strategyAuthorizedThing = nullptr;
	m_strategySpendAuthorization = SKIRMISH_AI_SPEND_AUTHORIZATION_NONE;
	m_strategyProductionReserveRefreshing = false;

}

// ------------------------------------------------------------------------------------------------
/** Load post process */
// ------------------------------------------------------------------------------------------------
void AISkirmishPlayer::loadPostProcess()
{
	if (TheGameLogic) {
		for (std::map<ObjectID, Bool>::const_iterator role =
			m_stage3CollectorRolesToRestore.begin();
			role != m_stage3CollectorRolesToRestore.end(); ++role) {
			Object *object = TheGameLogic->findObjectByID(role->first);
			if (!object || object->getControllingPlayer() != m_player)
				continue;
			AIUpdateInterface *ai = object->getAIUpdateInterface();
			WorkerAIInterface *worker = ai ? ai->getWorkerAIInterface() : nullptr;
			if (worker)
				worker->setStage3CollectorRole(role->second);
		}
	}
	m_stage3CollectorRolesToRestore.clear();
	m_currentEnemy = nullptr;
	for (Int i = 0; i < ThePlayerList->getPlayerCount(); ++i) {
		Player *player = ThePlayerList->getNthPlayer(i);
		if (player && player->getPlayerIndex() == m_currentEnemyPlayerIndex) {
			m_currentEnemy = player;
			break;
		}
	}
	if (!m_currentEnemy || (usesProductionBehavior() &&
		(!m_currentEnemy->getDefaultTeam() ||
		 m_player->getRelationship(m_currentEnemy->getDefaultTeam()) != ENEMIES ||
		 !m_currentEnemy->hasOffensiveTargetableObjects()))) {
		m_currentEnemy = nullptr;
		m_currentEnemyPlayerIndex = -1;
	}
	if (m_strategyState.currentMode < SKIRMISH_STRATEGY_BALANCED ||
		m_strategyState.currentMode > SKIRMISH_STRATEGY_ASSAULT ||
		m_strategyState.pendingMode < SKIRMISH_STRATEGY_NONE ||
		m_strategyState.pendingMode > SKIRMISH_STRATEGY_ASSAULT) {
		InitializeOldSaveSkirmishStrategyState(
			&m_strategyState, TheGameLogic ? TheGameLogic->getFrame() : 0);
	}
	const UnsignedInt currentFrame = TheGameLogic ? TheGameLogic->getFrame() : 0;
	if (m_strategyState.strategicTargetID == INVALID_ID ||
		!m_strategyState.strategicTargetObserved ||
		(m_strategyState.currentMode != SKIRMISH_STRATEGY_ASSAULT &&
		 m_strategyState.pendingMode != SKIRMISH_STRATEGY_ASSAULT) ||
		!IsSkirmishStrategyFrameReached(
			currentFrame, m_strategyState.strategicTargetLastSeenFrame))
		ClearSkirmishStrategyTargetObservation(&m_strategyState);
	if (!TheGameLogic) {
		ClearSkirmishStrategyTargetObservation(&m_strategyState);
	} else if (m_strategyState.strategicTargetID != INVALID_ID) {
		Object *strategyTarget =
			TheGameLogic->findObjectByID(m_strategyState.strategicTargetID);
		if (strategyTarget &&
			IsSkirmishStrategyIntelEligible(strategyTarget, m_player) &&
			(!IsSkirmishStrategyStaticTarget(strategyTarget) ||
			 strategyTarget->isEffectivelyDead() || strategyTarget->isDestroyed() ||
			 strategyTarget->testStatus(OBJECT_STATUS_SOLD) || !m_currentEnemy ||
			 strategyTarget->getControllingPlayer() != m_currentEnemy))
			ClearSkirmishStrategyTargetObservation(&m_strategyState);
	}
	if (m_strategyState.assaultEntryCombatValue < 0 ||
		m_strategyState.currentMode != SKIRMISH_STRATEGY_ASSAULT)
		m_strategyState.assaultEntryCombatValue = 0;
	m_recoveryAuthorizedThing = nullptr;
	m_strategyAuthorizedThing = nullptr;
	m_strategySpendAuthorization = SKIRMISH_AI_SPEND_AUTHORIZATION_NONE;
	m_strategyProductionReserveRefreshing = false;
	if (!usesProductionBehavior()) {
		clearStrategySourceCommandLock();
		m_reinforcementRoundRobinCursor = 0;
	} else if (!m_strategySourceCommandLocked) {
		clearStrategySourceCommandLock();
	}
	if (m_strategyProductionReserveCost < 0)
		m_strategyProductionReserveCost = 0;
	if (usesProductionBehavior() && !m_strategyProductionReserveLoaded)
		refreshStrategyProductionReserve();
	if (usesProductionBehavior() &&
		m_strategyState.currentMode == SKIRMISH_STRATEGY_FORTIFY &&
		m_strategyState.superweaponAttemptStatus ==
			SKIRMISH_STRATEGY_ATTEMPT_NOT_STARTED) {
		Object *construction = FindSkirmishAIStrategicSource(m_player, nullptr);
		if (!construction)
			construction = FindSkirmishAISuperweaponConstruction(m_player);
		if (construction) {
			m_strategySuperweaponID = construction->getID();
			m_strategyState.superweaponAttemptStatus =
				SKIRMISH_STRATEGY_ATTEMPT_PENDING;
		}
	}
	if (ShouldFailSkirmishAIStrategicPowerSourceLock(
			m_strategySourceCommandLocked,
			m_strategyState.superweaponAttemptStatus ==
				SKIRMISH_STRATEGY_ATTEMPT_PENDING,
			isStrategySourceCommandLockValid())) {
		m_strategyState.superweaponAttemptStatus =
			SKIRMISH_STRATEGY_ATTEMPT_FAILED;
		m_strategySuperweaponID = INVALID_ID;
		clearStrategySourceCommandLock();
	}
	if (m_strategySuperweaponID != INVALID_ID && TheGameLogic) {
		Object *superweapon = TheGameLogic->findObjectByID(
			m_strategySuperweaponID);
		const Bool recognizedLiveSource =
			IsUsableSkirmishAIStrategicSource(superweapon, m_player) &&
			HasSkirmishAIStrategicPowerModule(superweapon, nullptr);
		const Bool pendingConstruction =
			ShouldKeepSkirmishAISuperweaponConstructionPending(
				superweapon != nullptr,
				superweapon && superweapon->getControllingPlayer() == m_player,
				superweapon && superweapon->isKindOf(KINDOF_FS_SUPERWEAPON),
				superweapon && superweapon->isKindOf(KINDOF_REBUILD_HOLE),
				superweapon && superweapon->isEffectivelyDead(),
				superweapon && superweapon->isDestroyed(),
				superweapon && superweapon->testStatus(OBJECT_STATUS_SOLD),
				superweapon && superweapon->testStatus(OBJECT_STATUS_UNDER_CONSTRUCTION),
				superweapon && superweapon->testStatus(OBJECT_STATUS_RECONSTRUCTING)) &&
			IsSkirmishAIPlannedSuperweaponObject(m_player, superweapon);
		if (!superweapon || superweapon->getControllingPlayer() != m_player ||
			!superweapon->isKindOf(KINDOF_FS_SUPERWEAPON) ||
			superweapon->isEffectivelyDead() || superweapon->isDestroyed() ||
			superweapon->testStatus(OBJECT_STATUS_SOLD) ||
			(!recognizedLiveSource && !pendingConstruction)) {
			Object *replacement = FindSkirmishAIStrategicSource(m_player, nullptr);
			if (!replacement)
				replacement = FindSkirmishAISuperweaponConstruction(m_player);
			if (replacement) {
				m_strategySuperweaponID = replacement->getID();
			} else {
				m_strategySuperweaponID = INVALID_ID;
				if (m_strategyState.superweaponAttemptStatus ==
					SKIRMISH_STRATEGY_ATTEMPT_PENDING) {
					m_strategyState.superweaponAttemptStatus =
						SKIRMISH_STRATEGY_ATTEMPT_FAILED;
				}
			}
		}
	}
	if (m_strategyState.superweaponAttemptStatus !=
		SKIRMISH_STRATEGY_ATTEMPT_PENDING)
		clearStrategySourceCommandLock();
	m_strategyProductionReserveLoaded = false;
	if (m_recoveryPlacementAttempt < 0)
		m_recoveryPlacementAttempt = 0;
	if (m_recoveryReserveCost < 0)
		m_recoveryReserveCost = 0;

	// Post-processing runs after objects and production queues have loaded, so
	// canonicalize saved provenance before the first ProductionUpdate callback.
	validateRecoveryBuilderProduction();
	if (!usesCriticalRecoveryBehavior() || !m_player ||
		!m_player->getPlayerTemplate() || !TheThingFactory)
		return;
	const ThingTemplate *primaryTemplate = TheThingFactory->findTemplate(
		m_player->getPlayerTemplate()->getStartingBuilding());
	if (!primaryTemplate)
		return;
	Object *primaryCenter = nullptr;
	const Bool hasCompletedPrimaryCenter =
		findPrimaryCommandCenter(primaryTemplate, &primaryCenter) &&
		primaryCenter &&
		!primaryCenter->testStatus(OBJECT_STATUS_UNDER_CONSTRUCTION);
	m_recoveryBuilderFailoverConsumed =
		ReconcileSkirmishAIRecoveryBuilderFailoverConsumed(
			m_recoveryBuilderFailoverConsumed, true,
			hasCompletedPrimaryCenter);
	Bool paidQueueExists = false;
	ObjectID factoryID = INVALID_ID;
	ProductionID productionID = PRODUCTIONID_INVALID;
	hasRecoveryBuilderQueued(
		primaryTemplate, &paidQueueExists, &factoryID, &productionID);
	bindRecoveryBuilderProductionIfNeeded(
		hasCompletedPrimaryCenter, paidQueueExists, factoryID, productionID);
}

