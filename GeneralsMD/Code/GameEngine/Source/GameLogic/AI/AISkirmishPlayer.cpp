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
#include <vector>

#include "Common/GameMemory.h"
#include "Common/GlobalData.h"
#include "Common/Player.h"
#include "Common/PlayerTemplate.h"
#include "Common/PlayerList.h"
#include "Common/Recorder.h"
#include "Common/Team.h"
#include "Common/ThingFactory.h"
#include "Common/BuildAssistant.h"
#include "Common/SpecialPower.h"
#include "Common/ThingTemplate.h"
#include "Common/TunnelTracker.h"
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
#include "GameLogic/SkirmishAIDefense.h"
#include "GameLogic/SkirmishAIRecovery.h"
#include "GameLogic/SkirmishAITunnelRoute.h"
#include "GameLogic/Weapon.h"
#include "GameLogic/WeaponSet.h"
#include "GameLogic/Module/ProductionUpdate.h"
#include "GameClient/ControlBar.h"
#include "GameClient/TerrainVisual.h"
#include "GameNetwork/GameInfo.h"


#define USE_DOZER 1

struct SkirmishAIDefenseContext;

static void CollectSkirmishAIDefenseSupplyPositions(
	Player *player, std::vector<Coord2D> *positions);
static Bool IsSkirmishAIDefenseLineSite(
	const Coord3D &baseCenter, Real baseRadius,
	const SkirmishAIDefenseContext &context, const Coord3D &position,
	Real structureRadius, const std::vector<Coord2D> &supplyPositions);
static Bool IsSkirmishAIDefenseSiteOverlappingPendingBuild(
	Player *player, const Coord3D &position, Real structureRadius,
	const BuildListInfo *ignoreInfo = nullptr);

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

static Bool ShouldUseCurrentSkirmishAITacticalBehavior()
{
	return TheGameLogic && ShouldUseSkirmishAITacticalBehavior(
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
	MAX_SKIRMISH_STRATEGY_QUICK_PATH_QUERIES = 16,
	MAX_SKIRMISH_AI_TACTICAL_QUICK_PATH_QUERIES_PER_UPDATE = 16,
	MAX_SKIRMISH_AI_TACTICAL_RETREAT_PATH_QUERIES_PER_TEAM = 10
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

static Bool HasSkirmishStrategyPotentialOffensiveRecipient(
	Team *team, Player *player)
{
	if (!team || !player)
		return false;
	for (DLINK_ITERATOR<Object> member = team->iterate_TeamMemberList();
		!member.done(); member.advance()) {
		if (IsSkirmishStrategyPotentialOffensiveRecipient(
				member.cur(), player, team))
			return true;
	}
	return false;
}

static const Int MAX_SKIRMISH_AI_TUNNEL_MEMBERS = 32;
static const UnsignedInt MAX_SKIRMISH_AI_TUNNEL_ENDPOINT_PROBES = 512;
static const UnsignedInt MAX_SKIRMISH_AI_TACTICAL_TEAMS = 512;
static const Int MAX_SKIRMISH_AI_TUNNEL_PATH_QUERIES_PER_ATTEMPT = 256;
static const Int MAX_SKIRMISH_AI_TUNNEL_PATH_QUERIES_PER_UPDATE = 512;
static const UnsignedInt SKIRMISH_AI_TUNNEL_TRANSIT_TIMEOUT_SECONDS = 30;
static const UnsignedInt SKIRMISH_AI_TUNNEL_RETRY_COOLDOWN_SECONDS = 20;
static const UnsignedInt SKIRMISH_AI_TUNNEL_CAPACITY_WAIT_SECONDS = 45;
static const UnsignedInt SKIRMISH_AI_TUNNEL_PAIR_PROBE_SECONDS = 2;
static const UnsignedInt SKIRMISH_AI_TUNNEL_SUCCESS_COOLDOWN_SECONDS = 45;

static Bool GetSkirmishAIStrategyGroundApproach(
	const Coord3D *origin, Object *structure, Int candidate,
	Coord3D *approach)
{
	if (!origin || !structure || !approach || !TheTerrainLogic)
		return false;
	*approach = *structure->getPosition();
	const Real radius = structure->getTemplate()
		? structure->getTemplate()->getTemplateGeometryInfo()
			.getBoundingCircleRadius() : 0.0f;
	if (!SkirmishAITunnelRoute::GetApproachPoint(
			approach->x, approach->y, origin->x, origin->y,
			radius, 2.0f * PATHFIND_CELL_SIZE_F,
			candidate, &approach->x, &approach->y))
		return false;
	approach->z = TheTerrainLogic->getGroundHeight(approach->x, approach->y);
	return true;
}

static Bool ProbeSkirmishAITunnelQuickPath(
	const LocomotorSet &locomotorSet, const Coord3D *from,
	const Coord3D *to, Int *attemptQueries, Int *aggregateQueries)
{
	if (!from || !to || !attemptQueries || !aggregateQueries ||
		!TheAI || !TheAI->pathfinder() ||
		*attemptQueries >= MAX_SKIRMISH_AI_TUNNEL_PATH_QUERIES_PER_ATTEMPT ||
		*aggregateQueries >= MAX_SKIRMISH_AI_TUNNEL_PATH_QUERIES_PER_UPDATE)
		return false;
	++(*attemptQueries);
	++(*aggregateQueries);
	return TheAI->pathfinder()->clientSafeQuickDoesPathExist(
		locomotorSet, from, to);
}

// A result covers the entire team at the positions sampled in this sweep.
// The caller reserves the maximum two probes per member before invoking it.
static Int ProbeSkirmishAITunnelEndpointRole(
	const std::vector<Object *> &members, Object *endpoint, Object *target,
	Bool entryRole, Int *attemptQueries, Int *aggregateQueries)
{
	if (!endpoint || !target)
		return -1;
	for (size_t memberIndex = 0; memberIndex < members.size(); ++memberIndex) {
		Object *member = members[memberIndex];
		AIUpdateInterface *ai = member ? member->getAIUpdateInterface() : nullptr;
		if (!ai)
			return -1;
		Bool reachable = FALSE;
		for (Int candidate = 0; candidate < 2 && !reachable; ++candidate) {
			Coord3D from;
			Coord3D to;
			if (entryRole) {
				if (!GetSkirmishAIStrategyGroundApproach(
						member->getPosition(), endpoint, candidate, &to))
					continue;
				from = *member->getPosition();
			} else {
				if (!GetSkirmishAIStrategyGroundApproach(
						target->getPosition(), endpoint, candidate, &from) ||
					!GetSkirmishAIStrategyGroundApproach(
						endpoint->getPosition(), target, candidate, &to))
					continue;
			}
			reachable = ProbeSkirmishAITunnelQuickPath(
				ai->getLocomotorSet(), &from, &to,
				attemptQueries, aggregateQueries);
		}
		if (!reachable)
			return -1;
	}
	return 1;
}

static Bool IsSkirmishAIStrategyObjectIDBefore(
	const Object *left, const Object *right)
{
	return left && right && left->getID() < right->getID();
}

static Bool IsSkirmishAIStrategyTunnelTransitMember(
	Object *object, Player *player, Team *team, Bool allowContained)
{
	return object && player && team &&
		object->getControllingPlayer() == player && object->getTeam() == team &&
		!object->isEffectivelyDead() && !object->isDestroyed() &&
		!object->testStatus(OBJECT_STATUS_SOLD) &&
		!object->testStatus(OBJECT_STATUS_UNDER_CONSTRUCTION) &&
		object->isMobile() && (allowContained || !object->isContained()) &&
		IsSkirmishStrategyCombatObject(object) &&
		object->getAIUpdateInterface() != nullptr;
}

static Bool IsSkirmishAIStrategyTunnelEndpointLive(
	Object *object, Player *owner)
{
	ContainModuleInterface *contain = object ? object->getContain() : nullptr;
	return object && owner && object->getControllingPlayer() == owner &&
		!object->isEffectivelyDead() && !object->isDestroyed() &&
		!object->testStatus(OBJECT_STATUS_SOLD) &&
		!object->testStatus(OBJECT_STATUS_UNDER_CONSTRUCTION) &&
		!object->testStatus(OBJECT_STATUS_RECONSTRUCTING) && contain &&
		contain->isTunnelContain();
}

static Bool HasSkirmishAIStrategyTunnelRouteInfrastructure(
	Player *owner, TunnelTracker *tracker, Bool canBuildMissingEndpoints)
{
	if (!owner || !tracker)
		return false;
	const std::list<ObjectID> *endpointIDs = tracker->getContainerList();
	if (!endpointIDs)
		return canBuildMissingEndpoints;
	Int liveEndpointCount = 0;
	ObjectID firstEndpointID = INVALID_ID;
	for (std::list<ObjectID>::const_iterator endpointID = endpointIDs->begin();
		endpointID != endpointIDs->end(); ++endpointID) {
		Object *endpoint = TheGameLogic
			? TheGameLogic->findObjectByID(*endpointID) : nullptr;
		if (!IsSkirmishAIStrategyTunnelEndpointLive(endpoint, owner) ||
			endpoint->getID() == firstEndpointID)
			continue;
		if (firstEndpointID == INVALID_ID)
			firstEndpointID = endpoint->getID();
		if (++liveEndpointCount >= 2)
			return true;
	}
	return canBuildMissingEndpoints;
}

static Bool IsSkirmishAIStrategyTunnelEndpointRegistered(
	Object *object, Player *owner, TunnelTracker *tracker)
{
	if (!tracker || !IsSkirmishAIStrategyTunnelEndpointLive(object, owner))
		return false;
	const std::list<ObjectID> *tunnelIDs = tracker->getContainerList();
	if (!tunnelIDs)
		return false;
	std::list<ObjectID>::const_iterator tunnelID;
	for (tunnelID = tunnelIDs->begin(); tunnelID != tunnelIDs->end(); ++tunnelID) {
		if (*tunnelID == object->getID())
			return true;
	}
	return false;
}

static Bool IsSkirmishAISupportedGLASide(const AsciiString &side)
{
	return side == AsciiString("GLA") ||
		side == AsciiString("GLADemolitionGeneral") ||
		side == AsciiString("GLAStealthGeneral") ||
		side == AsciiString("GLAToxinGeneral");
}

static Bool IsSkirmishAIStrategyTunnelTemplate(const ThingTemplate *plan)
{
	if (!plan || !plan->isKindOf(KINDOF_STRUCTURE))
		return false;
	const ModuleInfo &modules = plan->getBehaviorModuleInfo();
	for (Int i = 0; i < modules.getCount(); ++i) {
		if (modules.getNthName(i) == AsciiString("TunnelContain"))
			return true;
	}
	return false;
}

static Bool IsSkirmishAIStrategyTunnelSiteVisible(
	Player *player, const Coord3D &position)
{
	if (!player || !ThePartitionManager)
		return false;
	const ObjectShroudStatus shroud =
		ThePartitionManager->getPropShroudStatusForPlayer(
			player->getPlayerIndex(), &position);
	return shroud == OBJECTSHROUD_CLEAR ||
		shroud == OBJECTSHROUD_PARTIAL_CLEAR;
}

static Object *SelectSkirmishAIStrategyFallbackTunnel(
	Player *owner, TunnelTracker *tracker, ObjectID afterID,
	ObjectID preferredID)
{
	if (!owner || !tracker || !TheGameLogic)
		return nullptr;
	const std::list<ObjectID> *ids = tracker->getContainerList();
	if (!ids)
		return nullptr;
	Object *first = nullptr;
	Object *next = nullptr;
	Object *preferred = nullptr;
	for (std::list<ObjectID>::const_iterator it = ids->begin();
		it != ids->end(); ++it) {
		Object *candidate = TheGameLogic->findObjectByID(*it);
		if (!IsSkirmishAIStrategyTunnelEndpointLive(candidate, owner))
			continue;
		if (!first || candidate->getID() < first->getID())
			first = candidate;
		if (candidate->getID() == preferredID)
			preferred = candidate;
		if (afterID != INVALID_ID && candidate->getID() > afterID &&
			(!next || candidate->getID() < next->getID()))
			next = candidate;
	}
	return preferred ? preferred : (next ? next : first);
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

static Bool IsSkirmishStrategyTopTargetCandidate(
	Object *object, const SkirmishStrategyCapabilityCandidate *candidates,
	Int candidateCount)
{
	for (Int index = 0; index < candidateCount; ++index) {
		if (candidates[index].object == object)
			return true;
	}
	return false;
}

static void InsertSkirmishStrategyTargetByID(
	Object **candidates, Int *candidateCount, Object *object)
{
	Int insertAt = 0;
	while (insertAt < *candidateCount &&
		candidates[insertAt]->getID() < object->getID())
		++insertAt;
	if (insertAt >= MAX_SKIRMISH_STRATEGY_TARGET_CANDIDATES)
		return;
	const Int last = *candidateCount < MAX_SKIRMISH_STRATEGY_TARGET_CANDIDATES ?
		*candidateCount : MAX_SKIRMISH_STRATEGY_TARGET_CANDIDATES - 1;
	for (Int index = last; index > insertAt; --index)
		candidates[index] = candidates[index - 1];
	candidates[insertAt] = object;
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

static Bool IsSkirmishStrategyTunnelTeamIDBefore(Team *left, Team *right)
{
	return left->getID() < right->getID();
}

static void AppendSkirmishStrategyTunnelTeamCandidate(
	std::vector<Team *> *teams, Team *team)
{
	if (!teams || !team)
		return;
	teams->push_back(team);
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
	Int *quickPathQueryCount, Bool rotateGroundProbes)
{
	size_t index;
	for (index = 0; index < airAttackers.size(); ++index) {
		if (airAttackers[index].object->getAbleToAttackSpecificObject(
				ATTACK_NEW_TARGET, target, CMD_FROM_AI) != ATTACKRESULT_NOT_POSSIBLE)
			return true;
	}
	if (!TheAI || !TheAI->pathfinder())
		return false;
	// The old replays use the highest-value prefix. Current games rotate the
	// same four-query budget so lower-ranked, reachable units get a turn.
	const size_t firstGroundIndex = rotateGroundProbes &&
		groundAttackers.size() > MAX_SKIRMISH_STRATEGY_GROUND_PROBES_PER_TARGET
		? (TheGameLogic->getFrame() / (LOGICFRAMES_PER_SECOND + 1)) %
			groundAttackers.size() : 0;
	Int capableGroundProbeCount = 0;
	for (index = 0; index < groundAttackers.size(); ++index) {
		const size_t candidateIndex =
			(firstGroundIndex + index) % groundAttackers.size();
		Object *attacker = groundAttackers[candidateIndex].object;
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

static Bool IsSkirmishAIStrategyTunnelTargetUsable(
	Object *target, Player *owner, Player *enemy);

// The known defense is the obstacle being bypassed. Exempt only that
// currently observed object from the normal enemy exclusion radius; all
// other visible enemies still veto the forward tunnel site.
static Bool IsSkirmishAIForwardTunnelLocationSafe(Player *owner,
	Player *enemy, const Coord3D *position, const ThingTemplate *plan,
	Object *blocker, Object *target)
{
	if (!owner || !enemy || !position || !plan || !blocker ||
		!TheAI || !ThePartitionManager ||
		!IsSkirmishAIStrategyTunnelTargetUsable(target, owner, enemy) ||
		blocker->getControllingPlayer() != enemy ||
		!blocker->isKindOf(KINDOF_FS_BASE_DEFENSE) ||
		blocker->isEffectivelyDead() || blocker->isDestroyed() ||
		blocker->testStatus(OBJECT_STATUS_SOLD) ||
		!IsSkirmishStrategyIntelEligible(blocker, owner) ||
		!IsSkirmishAIStrategyTunnelSiteVisible(owner, *position))
		return false;
	const Real tunnelRadius = plan->getTemplateGeometryInfo()
		.getBoundingCircleRadius();
	const Real blockerRadius = blocker->getTemplate()
		? blocker->getTemplate()->getTemplateGeometryInfo()
			.getBoundingCircleRadius() : 0.0f;
	const Real targetRadius = target->getTemplate()
		? target->getTemplate()->getTemplateGeometryInfo()
			.getBoundingCircleRadius() : 0.0f;
	if (!SkirmishAITunnelRoute::IsForwardSiteClearOfBlocker(
			blocker->getPosition()->x, blocker->getPosition()->y,
			target->getPosition()->x, target->getPosition()->y,
			position->x, position->y, blockerRadius, targetRadius,
			tunnelRadius, PATHFIND_CELL_SIZE_F))
		return false;
	const Real radius = TheAI->getAiData()->m_supplyCenterSafeRadius +
		tunnelRadius;
	PartitionFilterPlayerAffiliation filterTeam(
		owner, (ALLOW_ALLIES | ALLOW_NEUTRAL), false);
	PartitionFilterAlive filterAlive;
	PartitionFilterRejectByObjectStatus filterStealth(
		MAKE_OBJECT_STATUS_MASK(OBJECT_STATUS_STEALTHED),
		MAKE_OBJECT_STATUS_MASK2(OBJECT_STATUS_DETECTED,
			OBJECT_STATUS_DISGUISED));
	PartitionFilterInsignificantBuildings filterInsignificant(true, false);
	PartitionFilterRejectByKindOf filterHarvesters(
		MAKE_KINDOF_MASK(KINDOF_HARVESTER), KINDOFMASK_NONE);
	PartitionFilterRejectByKindOf filterDozer(
		MAKE_KINDOF_MASK(KINDOF_DOZER), KINDOFMASK_NONE);
	PartitionFilter *filters[] = {
		&filterTeam, &filterAlive, &filterStealth, &filterInsignificant,
		&filterHarvesters, &filterDozer, nullptr
	};
	ObjectIterator *iter = ThePartitionManager->iterateObjectsInRange(
		position, radius, FROM_BOUNDINGSPHERE_2D, filters);
	MemoryPoolObjectHolder hold(iter);
	for (Object *object = iter->first(); object; object = iter->next()) {
		if (object != blocker && IsSkirmishStrategyIntelEligible(object, owner))
			return false;
	}
	return true;
}

static Bool HasSkirmishAIStrategyTunnelTargetCapability(
	Object *target, Player *owner, Player *enemy,
	const std::vector<Team *> &candidateTeams,
	const std::vector<Object *> &visibleDefenseBlockers,
	TunnelTracker *tracker, Bool hasTunnelRouteInfrastructure,
	UnsignedInt probeEpoch)
{
	if (!target || !owner || !enemy || !tracker ||
		!hasTunnelRouteInfrastructure || visibleDefenseBlockers.empty() ||
		!IsSkirmishStrategyIntelEligible(target, owner) ||
		target->getControllingPlayer() != enemy ||
		!IsSkirmishStrategyStaticTarget(target) ||
		target->isEffectivelyDead() || target->isDestroyed() ||
		target->testStatus(OBJECT_STATUS_SOLD))
		return false;

	const Int tunnelCapacity = tracker->getContainMax();
	if (tunnelCapacity <= 0 || candidateTeams.empty())
		return false;
	// Target qualification is called for up to four structures per strategy
	// evaluation. Rotate a small window over stable team IDs so a large army
	// cannot multiply the full member and blocker scans on every evaluation.
	const size_t maxTeamProbes = 8;
	const size_t teamProbeCount = candidateTeams.size() < maxTeamProbes ?
		candidateTeams.size() : maxTeamProbes;
	const size_t firstTeamIndex = candidateTeams.size() > teamProbeCount ?
		((size_t)probeEpoch * maxTeamProbes) % candidateTeams.size() : 0;
	for (size_t teamOffset = 0; teamOffset < teamProbeCount; ++teamOffset) {
		Team *team = candidateTeams[
			(firstTeamIndex + teamOffset) % candidateTeams.size()];
		if (!IsSkirmishStrategyOffensiveTeam(team, owner))
			continue;
		std::vector<Object *> members;
		Bool eligibleGroundTeam = true;
		Bool canAttackTarget = false;
		for (DLINK_ITERATOR<Object> member = team->iterate_TeamMemberList();
			!member.done(); member.advance()) {
			Object *object = member.cur();
			if (!IsSkirmishStrategyPotentialOffensiveRecipient(
					object, owner, team))
				continue;
			if (object->isKindOf(KINDOF_AIRCRAFT) ||
				!IsSkirmishAIStrategyTunnelTransitMember(
					object, owner, team, false)) {
				eligibleGroundTeam = false;
				break;
			}
			members.push_back(object);
			if (object->getAbleToAttackSpecificObject(
					ATTACK_NEW_TARGET, target, CMD_FROM_AI) !=
					ATTACKRESULT_NOT_POSSIBLE)
				canAttackTarget = true;
			if (members.size() > MAX_SKIRMISH_AI_TUNNEL_MEMBERS) {
				eligibleGroundTeam = false;
				break;
			}
		}
		// Current occupancy is transient. Keep the target eligible while the
		// tactical tunnel controller waits for capacity to become available.
		if (!eligibleGroundTeam || members.empty() || !canAttackTarget ||
			members.size() > (size_t)tunnelCapacity)
			continue;
		std::sort(members.begin(), members.end(),
			IsSkirmishAIStrategyObjectIDBefore);
		Bool containerCompatible = true;
		for (size_t memberIndex = 0; memberIndex < members.size(); ++memberIndex) {
			if (!tracker->isValidContainerFor(members[memberIndex], false)) {
				containerCompatible = false;
				break;
			}
		}
		if (!containerCompatible)
			continue;
		// The executable bypass needs a visible defense near this team's
		// approach corridor, not merely a tunnel-capable target.
		Bool hasBlocker = false;
		for (size_t blockerIndex = 0;
			blockerIndex < visibleDefenseBlockers.size(); ++blockerIndex) {
			Object *candidate = visibleDefenseBlockers[blockerIndex];
			if (SkirmishAITunnelRoute::IsCorridorBlocker(
					members[0]->getPosition()->x,
					members[0]->getPosition()->y,
					target->getPosition()->x, target->getPosition()->y,
					candidate->getPosition()->x, candidate->getPosition()->y,
					350.0f)) {
				hasBlocker = true;
				break;
			}
		}
		if (!hasBlocker)
			continue;
		return true;
	}
	return false;
}

struct SkirmishAIDefenseContext
{
	SkirmishAIDefenseAnchor anchors[SKIRMISH_AI_DEFENSE_ROUTE_COUNT];
	Coord2D direction[SKIRMISH_AI_DEFENSE_ROUTE_COUNT];
	SkirmishAIDefenseThreat threat;
};

static Bool IsSkirmishAIDefenseLineSite(
	const Coord3D &baseCenter, Real baseRadius,
	const SkirmishAIDefenseContext &context, const Coord3D &position,
	Real structureRadius, const std::vector<Coord2D> &supplyPositions);

static void CollectSkirmishAIDefenseContext(
	Player *player, const Coord3D &baseCenter, Real baseRadius,
	SkirmishAIDefenseContext *context)
{
	if (!context) return;
	ClearSkirmishAIDefenseThreat(&context->threat);
	const Char *labels[SKIRMISH_AI_DEFENSE_ROUTE_COUNT] = {
		SKIRMISH_CENTER, SKIRMISH_FLANK, SKIRMISH_BACKDOOR };
	for (Int route = 0; route < SKIRMISH_AI_DEFENSE_ROUTE_COUNT; ++route) {
		context->anchors[route].available = false;
		context->anchors[route].x = context->anchors[route].y = 0;
		context->direction[route].x = context->direction[route].y = 0.0f;
		if (!player || !TheTerrainLogic) continue;
		AsciiString label;
		label.format("%s%d", labels[route], player->getMpStartIndex() + 1);
		Coord3D origin = baseCenter;
		Waypoint *way = TheTerrainLogic->getClosestWaypointOnPath(&origin, label);
		if (!way) continue;
		const Real dx = way->getLocation()->x - baseCenter.x;
		const Real dy = way->getLocation()->y - baseCenter.y;
		const Real length = sqrt(dx * dx + dy * dy);
		if (length < 1.0f) continue;
		context->direction[route].x = dx / length;
		context->direction[route].y = dy / length;
		context->anchors[route].x =
			(Int)(context->direction[route].x * 1000.0f);
		context->anchors[route].y =
			(Int)(context->direction[route].y * 1000.0f);
		context->anchors[route].available = true;
	}
	// Custom maps need not provide approach paths. Map bounds are public and
	// stable; never infer a missing entrance from hidden enemy structures.
	if (TheTerrainLogic) {
		Real centerX = context->direction[SKIRMISH_AI_DEFENSE_CENTER].x;
		Real centerY = context->direction[SKIRMISH_AI_DEFENSE_CENTER].y;
		if (!context->anchors[SKIRMISH_AI_DEFENSE_CENTER].available) {
			Region3D bounds;
			TheTerrainLogic->getMaximumPathfindExtent(&bounds);
			centerX = bounds.lo.x + bounds.width() * 0.5f - baseCenter.x;
			centerY = bounds.lo.y + bounds.height() * 0.5f - baseCenter.y;
			Real length = sqrt(centerX * centerX + centerY * centerY);
			if (length < 1.0f) { centerX = 1.0f; centerY = 0.0f; }
			else { centerX /= length; centerY /= length; }
		}
		for (Int route = 0; route < SKIRMISH_AI_DEFENSE_ROUTE_COUNT; ++route) {
			if (context->anchors[route].available) continue;
			Real x = centerX;
			Real y = centerY;
			if (route == SKIRMISH_AI_DEFENSE_FLANK) {
				x = -0.5f * centerX - 0.8660254f * centerY;
				y =  0.8660254f * centerX - 0.5f * centerY;
			} else if (route == SKIRMISH_AI_DEFENSE_BACKDOOR) {
				x = -0.5f * centerX + 0.8660254f * centerY;
				y = -0.8660254f * centerX - 0.5f * centerY;
			}
			context->direction[route].x = x;
			context->direction[route].y = y;
			context->anchors[route].x = (Int)(x * 1000.0f);
			context->anchors[route].y = (Int)(y * 1000.0f);
			context->anchors[route].available = true;
		}
	}
	if (!player || !TheGameLogic) return;
	Real outerRadius = baseRadius + 400.0f;
	if (outerRadius < 500.0f) outerRadius = 500.0f;
	if (outerRadius > 1200.0f) outerRadius = 1200.0f;
	Real innerRadius = baseRadius * 0.8f;
	if (innerRadius < 160.0f) innerRadius = 160.0f;
	for (Object *object = TheGameLogic->getFirstObject(); object;
		object = object->getNextObject()) {
		Player *owner = object->getControllingPlayer();
		if (!owner || !owner->getDefaultTeam() ||
			player->getRelationship(owner->getDefaultTeam()) != ENEMIES ||
			!IsSkirmishStrategyIntelEligible(object, player) ||
			!IsSkirmishStrategyCombatObject(object) || object->isContained() ||
			object->isKindOf(KINDOF_AIRCRAFT) || object->isEffectivelyDead() ||
			object->isDestroyed())
			continue;
		const Real dx = object->getPosition()->x - baseCenter.x;
		const Real dy = object->getPosition()->y - baseCenter.y;
		const Real distanceSqr = dx * dx + dy * dy;
		if (distanceSqr > outerRadius * outerRadius) continue;
		const Int route = ClassifySkirmishAIDefenseRoute(
			(Int)dx, (Int)dy, context->anchors);
		if (!IsSkirmishAIDefenseRoute(route)) continue;
		const Int cost = object->getTemplate()->calcCostToBuild(owner);
		Int value = cost > 0
			? (Int)((__int64)cost * GetSkirmishStrategyHealthPercent(object) / 100)
			: 0;
		if (value < 100) value = 100;
		AddSkirmishAIDefenseObservation(&context->threat, route,
			true, true, distanceSqr <= innerRadius * innerRadius, value);
	}
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
};

struct SkirmishFactoryProjection
{
	Object *factory;
	Int projectedFrames;
	Bool usedByCandidate;
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
	m_strategyTargetFallbackPending(false),
	m_strategyTargetFallbackAfterID(INVALID_ID),
	m_strategyTargetFallbackEnemyIndex(-1),
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
	m_tacticalNextTeamScanFrame(0),
	m_tunnelBuildPhase(SKIRMISH_AI_TUNNEL_BUILD_NONE),
	m_tunnelHomeAttempted(false),
	m_tunnelForwardAttempted(false),
	m_tunnelForwardRetryConsumed(false),
	m_tunnelHomeEndpointID(INVALID_ID),
	m_tunnelForwardEndpointID(INVALID_ID),
	m_tunnelForwardAttemptTargetID(INVALID_ID),
	m_tunnelBuildBuilderID(INVALID_ID),
	m_tunnelBuildTargetID(INVALID_ID),
	m_tunnelBuildBlockerID(INVALID_ID),
	m_tunnelBuildObjectID(INVALID_ID),
	m_tunnelBuildLockedBuilderID(INVALID_ID),
	m_tunnelPendingBuilderCursor(0),
	m_defenseBuildLockedBuilderID(INVALID_ID),
	m_tunnelBuildDeadlineFrame(0),
	m_tunnelBuildCooldownUntilFrame(0),
	m_defensePatrolRoute(SKIRMISH_AI_DEFENSE_NO_ROUTE),
	m_defensePatrolTeamID(0),
	m_defensePatrolObjectID(INVALID_ID),
	m_defenseNextPatrolFrame(0),
	m_defenseQuietPatrolDeadlineFrame(0),
	m_defensePlacementAttempt(0),
	m_defensePlacementNextFrame(0),
	m_defensePursuitTargetID(INVALID_ID),
	m_defensePursuitStartFrame(0),
	m_defenseInterceptProbeAfterID(INVALID_ID),
	m_defensePatrolRouteMemberAfterID(INVALID_ID),
	m_defenseInterceptMemberAfterID(INVALID_ID),
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
	m_tunnelBuildLocation.zero();
	for (Int i = 0; i < SkirmishAITunnelRoute::MAX_GENERATED_FORWARD_ENDPOINTS;
		++i) {
		m_tunnelGeneratedForwardEndpointIDs[i] = INVALID_ID;
		m_tunnelGeneratedForwardTargetIDs[i] = INVALID_ID;
	}
	for (Int i = 0; i < SkirmishAITunnelRoute::MAX_EXHAUSTED_FORWARD_TARGETS;
		++i)
		m_tunnelExhaustedForwardTargetIDs[i] = INVALID_ID;
	m_defenseBuildLockedLocation.zero();
	m_defenseQuietPatrolWaypoint.zero();
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


template <class GeneratedDefenseMarker>
static Bool MatchesGeneratedDefenseBuild(
	const GeneratedDefenseMarker &marker, const BuildListInfo *info)
{
	if (!info || !info->getLocation())
		return false;
	const Coord3D *location = info->getLocation();
	return marker.templateName == info->getTemplateName() &&
		marker.location.x == location->x &&
		marker.location.y == location->y && marker.angle == info->getAngle();
}

template <class GeneratedDefenseMarkers>
static Bool IsGeneratedDefenseBuildInfo(
	const GeneratedDefenseMarkers &markers, const BuildListInfo *info)
{
	for (UnsignedInt i = 0; i < markers.size(); ++i) {
		if (MatchesGeneratedDefenseBuild(markers[i], info))
			return true;
	}
	return false;
}

template <class GeneratedDefenseMarkers>
static void EraseGeneratedDefenseBuildInfo(
	GeneratedDefenseMarkers *markers, const BuildListInfo *info)
{
	if (!markers || !info)
		return;
	for (UnsignedInt i = 0; i < markers->size(); ++i) {
		if (MatchesGeneratedDefenseBuild((*markers)[i], info)) {
			markers->erase(markers->begin() + i);
			return;
		}
	}
}

template <class GeneratedDefenseMarkers>
static void PruneGeneratedDefenseBuildMarkers(
	GeneratedDefenseMarkers *markers, Player *player)
{
	if (!markers || !player)
		return;
	for (UnsignedInt i = 0; i < markers->size();) {
		Bool stillQueued = false;
		for (BuildListInfo *info = player->getBuildList(); info;
			info = info->getNext()) {
			if (info->getNumRebuilds() != 0 &&
				MatchesGeneratedDefenseBuild((*markers)[i], info)) {
				stillQueued = true;
				break;
			}
		}
		if (stillQueued)
			++i;
		else
			markers->erase(markers->begin() + i);
	}
}

static Bool IsAvailableSkirmishAIDefenseBuilder(
	Object *candidate, Player *player, const ThingTemplate *plan,
	ObjectID excludedBuilderID)
{
	if (!candidate || !player || !plan || !TheBuildAssistant ||
		candidate->getControllingPlayer() != player ||
		!candidate->isKindOf(KINDOF_DOZER) || candidate->isContained() ||
		candidate->isEffectivelyDead() || candidate->isDestroyed() ||
		candidate->isDisabledByType(DISABLED_UNMANNED) ||
		candidate->getID() == excludedBuilderID)
		return false;
	AIUpdateInterface *ai = candidate->getAIUpdateInterface();
	DozerAIInterface *dozerAI = ai ? ai->getDozerAIInterface() : nullptr;
	// The build command idles its chosen dozer. Do not cancel a pending repair,
	// move, or supply task just because it is not a construction task.
	if (!dozerAI || !ai->isIdle() || dozerAI->isAnyTaskPending())
		return false;
	SupplyTruckAIInterface *supplyAI = ai->getSupplyTruckAIInterface();
	if (supplyAI && (supplyAI->isCurrentlyFerryingSupplies() ||
		supplyAI->isForcedIntoWantingState()))
		return false;
	return TheBuildAssistant->canMakeUnit(candidate, plan) == CANMAKE_OK;
}

static Bool HasAvailableSkirmishAIDefenseBuilder(
	Player *player, const ThingTemplate *plan, ObjectID excludedBuilderID,
	Bool *hasLivingBuilder)
{
	if (hasLivingBuilder) *hasLivingBuilder = false;
	if (!TheGameLogic) return false;
	for (Object *candidate = TheGameLogic->getFirstObject(); candidate;
		candidate = candidate->getNextObject()) {
		if (hasLivingBuilder && candidate->getControllingPlayer() == player &&
			candidate->isKindOf(KINDOF_DOZER) && !candidate->isContained() &&
			!candidate->isEffectivelyDead() && !candidate->isDestroyed())
			*hasLivingBuilder = true;
		if (IsAvailableSkirmishAIDefenseBuilder(
				candidate, player, plan, excludedBuilderID))
			return true;
	}
	return false;
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
	const Bool queuedTunnelBuild =
		m_tunnelBuildPhase == SKIRMISH_AI_TUNNEL_BUILD_HOME_QUEUED ||
		m_tunnelBuildPhase == SKIRMISH_AI_TUNNEL_BUILD_FORWARD_QUEUED;
	if (queuedTunnelBuild && m_player && TheGameLogic && TheThingFactory) {
		const UnsignedInt now = TheGameLogic->getFrame();
		const ThingTemplate *tunnelPlan = findTunnelContainBuildTemplate();
		Bool pendingInfoFound = false;
		for (BuildListInfo *info = m_player->getBuildList(); info;
			info = info->getNext()) {
			const ThingTemplate *plan = TheThingFactory->findTemplate(
				info->getTemplateName());
			if (isPendingTunnelBuildInfo(info, plan)) {
				pendingInfoFound = true;
				break;
			}
		}
		Object *target = m_tunnelBuildTargetID != INVALID_ID
			? TheGameLogic->findObjectByID(m_tunnelBuildTargetID) : nullptr;
		Object *blocker = m_tunnelBuildBlockerID != INVALID_ID
			? TheGameLogic->findObjectByID(m_tunnelBuildBlockerID) : nullptr;
		const Bool invalidPendingPlan =
			!pendingInfoFound || !tunnelPlan ||
			m_strategyState.currentMode != SKIRMISH_STRATEGY_ASSAULT ||
			m_strategyState.strategicTargetID != m_tunnelBuildTargetID ||
			!IsSkirmishAIStrategyTunnelTargetUsable(
				target, m_player, m_currentEnemy) ||
			!blocker || blocker->getControllingPlayer() != m_currentEnemy ||
			!blocker->isKindOf(KINDOF_FS_BASE_DEFENSE) ||
			blocker->isEffectivelyDead() || blocker->isDestroyed() ||
			blocker->testStatus(OBJECT_STATUS_SOLD) ||
			!IsSkirmishStrategyIntelEligible(blocker, m_player) ||
			IsSkirmishStrategyFrameReached(now, m_tunnelBuildDeadlineFrame);
		if (invalidPendingPlan)
			abandonTunnelBuildPlan(now, TRUE);
	}
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
			const Bool queuedGeneratedDefense =
				ShouldUseCurrentSkirmishAITacticalBehavior() &&
				info->isPriorityBuild() &&
				curPlan->isKindOf(KINDOF_FS_BASE_DEFENSE) &&
				IsGeneratedDefenseBuildInfo(m_generatedDefenseBuilds, info);
			if (queuedGeneratedDefense && info->getObjectID() == INVALID_ID) {
				// Reuse the saved build-list timestamp as this queued site's age.
				// A permanently busy builder must not occupy a defense slot forever.
				if (info->getObjectTimestamp() == 0)
					info->setObjectTimestamp(TheGameLogic->getFrame() + 1);
				if (IsSkirmishStrategyFrameReached(TheGameLogic->getFrame(),
						info->getObjectTimestamp() +
						60 * LOGICFRAMES_PER_SECOND)) {
					info->setNumRebuilds(0);
					EraseGeneratedDefenseBuildInfo(&m_generatedDefenseBuilds, info);
					m_defensePlacementNextFrame = TheGameLogic->getFrame() +
						10 * LOGICFRAMES_PER_SECOND;
					continue;
				}
				// A path-probe budget may defer this generated entry. Let other
				// build-list work run until its next short retry; keep its age intact.
				if (!IsSkirmishStrategyFrameReached(TheGameLogic->getFrame(),
						m_defensePlacementNextFrame))
					continue;
			} else if (info->getObjectID()==INVALID_ID && info->getObjectTimestamp()>0) {
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
			const Bool pendingTunnelBuild =
				isPendingTunnelBuildInfo(info, curPlan);
			if (pendingTunnelBuild &&
				(m_strategyState.currentMode != SKIRMISH_STRATEGY_ASSAULT ||
				 m_strategyState.strategicTargetID != m_tunnelBuildTargetID)) {
				abandonTunnelBuildPlan(TheGameLogic->getFrame(), TRUE);
				continue;
			}
			if (pendingTunnelBuild &&
				IsSkirmishStrategyFrameReached(
					TheGameLogic->getFrame(), m_tunnelBuildDeadlineFrame)) {
				abandonTunnelBuildPlan(TheGameLogic->getFrame(), TRUE);
				continue;
			}
			if (pendingTunnelBuild && !IsSkirmishStrategyFrameReached(
					TheGameLogic->getFrame(), m_tunnelBuildCooldownUntilFrame))
				continue;
			if (pendingTunnelBuild)
				m_tunnelBuildCooldownUntilFrame = TheGameLogic->getFrame() +
					2 * LOGICFRAMES_PER_SECOND;
			// A generated defense can wait for a busy builder without holding up
			// another affordable structure in this build pass.
			Bool livingDefenseBuilder = false;
			if (queuedGeneratedDefense &&
				!HasAvailableSkirmishAIDefenseBuilder(
					m_player, curPlan, m_repairDozer,
					&livingDefenseBuilder) && livingDefenseBuilder)
				continue;

			// Make sure it is safe to build here.
			const Bool forwardTunnelBuild = pendingTunnelBuild &&
				(m_tunnelBuildPhase == SKIRMISH_AI_TUNNEL_BUILD_FORWARD_QUEUED ||
				 m_tunnelBuildPhase == SKIRMISH_AI_TUNNEL_BUILD_FORWARD_CONSTRUCTING);
			Object *forwardBlocker = forwardTunnelBuild && TheGameLogic
				? TheGameLogic->findObjectByID(m_tunnelBuildBlockerID) : nullptr;
			Object *forwardTarget = forwardTunnelBuild && TheGameLogic
				? TheGameLogic->findObjectByID(m_tunnelBuildTargetID) : nullptr;
			if (forwardTunnelBuild
				? !IsSkirmishAIForwardTunnelLocationSafe(m_player,
					m_currentEnemy, info->getLocation(), curPlan,
					forwardBlocker, forwardTarget)
				: !isLocationSafe(info->getLocation(), curPlan)) {
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
				(!productionBehavior || !info->isPriorityBuild()) &&
				!pendingTunnelBuild) {
				continue; // marked to not build automatically.
			}
			Object *dozer = nullptr;
			if (pendingTunnelBuild) {
				Bool permanentFailure = FALSE;
				if (!validatePendingTunnelBuild(
						info, curPlan, &dozer, &permanentFailure)) {
					if (permanentFailure)
						abandonTunnelBuildPlan(
							TheGameLogic->getFrame(), TRUE);
					continue;
				}
			} else {
				dozer = findDozer(info->getLocation());
			}
			const Bool authorizedWithoutBuilder = !pendingTunnelBuild &&
				productionBehavior &&
				info->isBuildable() &&
				(info->isPriorityBuild() ||
				 IsSkirmishAIStrategyAuthorizedStructure(curPlan));
			if (dozer==nullptr && !pendingTunnelBuild) {
				if (!authorizedWithoutBuilder && (isUnderPowered ||
					(productionBehavior && info->isBuildable()))) {
					queueDozer();
				}
				if (!authorizedWithoutBuilder)
					continue;
			}
			if (dozer && !pendingTunnelBuild &&
				TheBuildAssistant->canMakeUnit(dozer,
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
				const Bool authorizeCriticalCandidate = !pendingTunnelBuild &&
					(info->isPriorityBuild() ||
					 (IsSkirmishAIStrategyAuthorizedStructure(curPlan) &&
					  (!dozer || canStartCriticalRebuildNow(info, curPlan))));
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
			PruneGeneratedDefenseBuildMarkers(
				&m_generatedDefenseBuilds, m_player);
			const Bool pendingTunnelBuild =
				isPendingTunnelBuildInfo(bldgInfo, bldgPlan);
			const Bool priorityDefenseBuild =
				ShouldUseCurrentSkirmishAITacticalBehavior() &&
				m_baseCenterSet && bldgInfo->isPriorityBuild() &&
				bldgPlan->isKindOf(KINDOF_FS_BASE_DEFENSE) &&
				IsGeneratedDefenseBuildInfo(
					m_generatedDefenseBuilds, bldgInfo);
			Object *selectedBuilder = nullptr;
			if (pendingTunnelBuild) {
				Bool permanentFailure = FALSE;
				if (!validatePendingTunnelBuild(
						bldgInfo, bldgPlan, &selectedBuilder,
						&permanentFailure)) {
					if (permanentFailure)
						abandonTunnelBuildPlan(
							TheGameLogic->getFrame(), TRUE);
					return;
				}
				if (!canSpendForCriticalRecovery(
						bldgPlan->calcCostToBuild(m_player),
						bldgPlan, false, false))
					return;
			} else if (!priorityDefenseBuild) {
				selectedBuilder = findDozer(bldgInfo->getLocation());
				if (!selectedBuilder) {
					queueAuthorizedStrategyBuilder(bldgPlan);
					return;
				}
			}
			if (priorityDefenseBuild) {
				SkirmishAIDefenseContext defenseContext;
				CollectSkirmishAIDefenseContext(m_player, m_baseCenter,
					m_baseRadius, &defenseContext);
				const Real defenseRadius = bldgPlan->getTemplateGeometryInfo()
					.getBoundingCircleRadius();
				std::vector<Coord2D> supplyPositions;
				CollectSkirmishAIDefenseSupplyPositions(m_player,
					&supplyPositions);
				if (!IsSkirmishAIDefenseLineSite(m_baseCenter,
					m_baseRadius, defenseContext, *bldgInfo->getLocation(),
					defenseRadius, supplyPositions) ||
					IsSkirmishAIDefenseSiteOverlappingPendingBuild(m_player,
						*bldgInfo->getLocation(), defenseRadius, bldgInfo)) {
					bldgInfo->setNumRebuilds(0);
					EraseGeneratedDefenseBuildInfo(
						&m_generatedDefenseBuilds, bldgInfo);
					m_defensePlacementNextFrame = TheGameLogic->getFrame() +
						10 * LOGICFRAMES_PER_SECOND;
					return;
				}
				// A supply site added after this priority defense was queued must
				// not let the inherited quick-path failure teleport its builder.
				Coord3D buildPosition = *bldgInfo->getLocation();
				buildPosition.z += TheTerrainLogic->getGroundHeight(
					buildPosition.x, buildPosition.y);
				std::vector<Object *> eligibleBuilders;
				for (Object *candidate = TheGameLogic->getFirstObject(); candidate;
					candidate = candidate->getNextObject()) {
					if (!IsAvailableSkirmishAIDefenseBuilder(
							candidate, m_player, bldgPlan, m_repairDozer)) continue;
					eligibleBuilders.push_back(candidate);
				}
				std::sort(eligibleBuilders.begin(), eligibleBuilders.end(),
					IsSkirmishAIStrategyObjectIDBefore);
				Object *reachableBuilder = nullptr;
				Real bestDistanceSqr = 0.0f;
				const UnsignedInt markerAge = IsSkirmishStrategyFrameReached(
					TheGameLogic->getFrame(), bldgInfo->getObjectTimestamp())
					? TheGameLogic->getFrame() - bldgInfo->getObjectTimestamp() : 0;
				const UnsignedInt window = markerAge /
					(2 * LOGICFRAMES_PER_SECOND);
				const size_t candidateCount = eligibleBuilders.size();
				const size_t start = candidateCount ?
					(static_cast<size_t>(window) * 16) % candidateCount : 0;
				const size_t probes = candidateCount < 16 ? candidateCount : 16;
				for (size_t index = 0; index < probes; ++index) {
					Object *candidate = eligibleBuilders[(start + index) % candidateCount];
					AIUpdateInterface *candidateAI = candidate->getAIUpdateInterface();
					if (!TheAI->pathfinder()->clientSafeQuickDoesPathExist(
							candidateAI->getLocomotorSet(), candidate->getPosition(),
							&buildPosition))
						continue;
					const Real dx = buildPosition.x - candidate->getPosition()->x;
					const Real dy = buildPosition.y - candidate->getPosition()->y;
					const Real distanceSqr = dx * dx + dy * dy;
					if (!reachableBuilder || distanceSqr < bestDistanceSqr ||
						(distanceSqr == bestDistanceSqr &&
						 candidate->getID() < reachableBuilder->getID())) {
						reachableBuilder = candidate;
						bestDistanceSqr = distanceSqr;
					}
				}
				if (!reachableBuilder) {
					if (candidateCount > probes) {
						// The unprobed builders may still reach this site. Retry a
						// different ID window after the next two-second build pass,
						// so other structures can build in that pass.
						m_defensePlacementNextFrame = TheGameLogic->getFrame() +
							3 * LOGICFRAMES_PER_SECOND;
						return;
					}
					// Abandon this unreachable site and let other queued work proceed.
					bldgInfo->setNumRebuilds(0);
					EraseGeneratedDefenseBuildInfo(
						&m_generatedDefenseBuilds, bldgInfo);
					m_defensePlacementNextFrame = TheGameLogic->getFrame() +
						10 * LOGICFRAMES_PER_SECOND;
					return;
				}
				selectedBuilder = reachableBuilder;
				const Bool legal = TheBuildAssistant->isLocationLegalToBuild(
					&buildPosition, bldgPlan, bldgInfo->getAngle(),
					BuildAssistant::CLEAR_PATH |
					BuildAssistant::TERRAIN_RESTRICTIONS |
					BuildAssistant::NO_OBJECT_OVERLAP,
					selectedBuilder, m_player) == LBC_OK;
				if (TheTerrainVisual) TheTerrainVisual->removeAllBibs();
				if (!legal) {
					bldgInfo->setNumRebuilds(0);
					EraseGeneratedDefenseBuildInfo(
						&m_generatedDefenseBuilds, bldgInfo);
					m_defensePlacementNextFrame = TheGameLogic->getFrame() +
						10 * LOGICFRAMES_PER_SECOND;
					return;
				}
			}
			// dozer-construct the building
			const Bool authorizeCriticalBuild = !pendingTunnelBuild &&
				usesProductionBehavior() &&
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
			const ObjectID previousTunnelBuildLock = m_tunnelBuildLockedBuilderID;
			const ObjectID previousDefenseBuildLock = m_defenseBuildLockedBuilderID;
			const Coord3D previousDefenseBuildLocation = m_defenseBuildLockedLocation;
			if (pendingTunnelBuild)
				m_tunnelBuildLockedBuilderID = selectedBuilder->getID();
			if (priorityDefenseBuild) {
				m_defenseBuildLockedBuilderID = selectedBuilder->getID();
				m_defenseBuildLockedLocation = *bldgInfo->getLocation();
			}
			bldg = pendingTunnelBuild || priorityDefenseBuild
				? buildStructureWithDozer(bldgPlan, bldgInfo, FALSE, FALSE)
				: buildStructureWithDozer(bldgPlan, bldgInfo);
			m_tunnelBuildLockedBuilderID = previousTunnelBuildLock;
			m_defenseBuildLockedBuilderID = previousDefenseBuildLock;
			m_defenseBuildLockedLocation = previousDefenseBuildLocation;
			m_strategyAuthorizedThing = previousAuthorization;
			m_strategySpendAuthorization = previousAuthorizationClass;
			if (pendingTunnelBuild && !bldg)
				abandonTunnelBuildPlan(TheGameLogic->getFrame(), TRUE);
			// store the object with the build order
			if (bldg)
			{
				bldgInfo->setObjectID( bldg->getID() );
				bldgInfo->decrementNumRebuilds();
				// Keep the marker while this priority entry has rebuilds left.
				// A later rebuild must use the same site and reachable-builder
				// checks as the first construction.
				if (priorityDefenseBuild && bldgInfo->getNumRebuilds() == 0)
					EraseGeneratedDefenseBuildInfo(
						&m_generatedDefenseBuilds, bldgInfo);
				if (pendingTunnelBuild) {
					m_tunnelBuildPhase = m_tunnelBuildPhase ==
						SKIRMISH_AI_TUNNEL_BUILD_HOME_QUEUED
						? SKIRMISH_AI_TUNNEL_BUILD_HOME_CONSTRUCTING
						: SKIRMISH_AI_TUNNEL_BUILD_FORWARD_CONSTRUCTING;
					m_tunnelBuildObjectID = bldg->getID();
					m_tunnelBuildBuilderID = INVALID_ID;
					m_tunnelBuildDeadlineFrame = TheGameLogic->getFrame() +
						120 * LOGICFRAMES_PER_SECOND;
				}
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
	*aircraftValue = 0;
	*vehicleValue = 0;
	*infantryValue = 0;
	*hasRouteTarget = false;
	routeTarget->zero();
	Player *enemy = getAiEnemy();
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
	const Bool targetFallbackEnabled =
		ShouldUseCurrentSkirmishAITacticalBehavior();
	const Bool tunnelTargetBootstrapEnabled =
		targetFallbackEnabled && m_player &&
		IsSkirmishAISupportedGLASide(m_player->getSide());
	// A strategy evaluation is scheduled at this difficulty-specific interval.
	// Advancing by one full team window per evaluation visits every team ID.
	const UnsignedInt tunnelProbeInterval =
		(m_difficulty == DIFFICULTY_EASY ? 20 :
			m_difficulty == DIFFICULTY_HARD ? 5 : 10) *
		LOGICFRAMES_PER_SECOND;
	const UnsignedInt tunnelProbeEpoch =
		TheGameLogic->getFrame() / tunnelProbeInterval;
	std::vector<SkirmishStrategyCapabilityCandidate> groundAttackers;
	std::vector<SkirmishStrategyCapabilityCandidate> airAttackers;
	std::vector<Team *> tunnelCandidateTeams;
	std::vector<Object *> visibleDefenseBlockers;
	TunnelTracker *activeTransitTracker = targetFallbackEnabled && m_player
		? m_player->getTunnelSystem() : nullptr;
	Bool activeTransitForPersistedTarget = FALSE;
	SkirmishStrategyCapabilityCandidate
		targetCandidates[MAX_SKIRMISH_STRATEGY_TARGET_CANDIDATES];
	Int targetCandidateCount = 0;
	Int staticTargetCount = 0;
	Object *persistedTarget = nullptr;
	Player *enemy = m_currentEnemy;
	const Int enemyIndex = enemy ? enemy->getPlayerIndex() : -1;
	if (targetFallbackEnabled &&
		m_strategyTargetFallbackEnemyIndex != enemyIndex) {
		m_strategyTargetFallbackEnemyIndex = enemyIndex;
		m_strategyTargetFallbackPending = false;
		m_strategyTargetFallbackAfterID = INVALID_ID;
	}
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

			Bool trackedTunnelPassenger = FALSE;
			if (object->isContained() && activeTransitTracker &&
				activeTransitTracker->isInContainer(object)) {
				Team *team = object->getTeam();
				if (team) {
					std::map<UnsignedInt, TacticalTeamState>::const_iterator transit =
						m_tacticalTeams.find(team->getID());
					if (transit != m_tacticalTeams.end() &&
						transit->second.tunnelTransitPhase !=
							SKIRMISH_AI_TUNNEL_TRANSIT_NONE &&
						IsSkirmishAIStrategyTunnelTransitMember(
							object, m_player, team, TRUE)) {
						const TacticalTeamState &transitState = transit->second;
						for (Int memberIndex = 0;
							memberIndex < transitState.tunnelMemberCount &&
							memberIndex < MAX_SKIRMISH_AI_TUNNEL_MEMBERS;
							++memberIndex)
							if (transitState.tunnelMemberIDs[memberIndex] ==
								object->getID()) {
								trackedTunnelPassenger = TRUE;
								if (m_strategyState.currentMode ==
										SKIRMISH_STRATEGY_ASSAULT &&
									m_strategyState.strategicTargetObserved &&
									transitState.tunnelTransitPhase !=
										SKIRMISH_AI_TUNNEL_TRANSIT_FALLBACK_EXIT &&
									transitState.tunnelTargetID ==
										m_strategyState.strategicTargetID &&
									transitState.tunnelStrategicTargetID ==
										m_strategyState.strategicTargetID)
									activeTransitForPersistedTarget = TRUE;
								break;
							}
					}
				}
			}
			if (IsSkirmishStrategyCombatObject(object) &&
				(!object->isContained() || trackedTunnelPassenger)) {
				const Int cost = object->getTemplate()->calcCostToBuild(m_player);
				const Int value = cost > 0 ?
					(Int)((__int64)cost * health / 100) : 0;
				ownCombatValue = AddSkirmishStrategyValue(ownCombatValue, value);
				if (!object->isContained() && m_baseCenterSet) {
					const Real dx = object->getPosition()->x - m_baseCenter.x;
					const Real dy = object->getPosition()->y - m_baseCenter.y;
					if (dx * dx + dy * dy <= threatRadiusSquared)
						ownLocalCombatValue = AddSkirmishStrategyValue(
							ownLocalCombatValue, value);
				}
				if (!object->isContained() &&
					IsSkirmishStrategyOffensiveRecipient(object, m_player)) {
					if (object->isKindOf(KINDOF_AIRCRAFT))
						AppendSkirmishStrategyCapabilityCandidate(
							&airAttackers, object, value);
					else {
						AppendSkirmishStrategyCapabilityCandidate(
							&groundAttackers, object, value);
						if (tunnelTargetBootstrapEnabled)
							AppendSkirmishStrategyTunnelTeamCandidate(
								&tunnelCandidateTeams, object->getTeam());
					}
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
		if (tunnelTargetBootstrapEnabled &&
			object->isKindOf(KINDOF_FS_BASE_DEFENSE))
			visibleDefenseBlockers.push_back(object);
		if (IsSkirmishStrategyCombatObject(object))
			enemyCombatValue = AddSkirmishStrategyValue(enemyCombatValue, value);
		if (object->isKindOf(KINDOF_STRUCTURE) ||
			object->isKindOf(KINDOF_HARVESTER))
			knownOpportunityValue = AddSkirmishStrategyValue(
				knownOpportunityValue, value);
		if (IsSkirmishStrategyStaticTarget(object)) {
			if (staticTargetCount <= MAX_SKIRMISH_STRATEGY_TARGET_CANDIDATES)
				++staticTargetCount;
			InsertSkirmishStrategyTargetCandidate(
				targetCandidates, &targetCandidateCount, object, value);
		}
	}
	if (tunnelCandidateTeams.size() > 1) {
		std::sort(tunnelCandidateTeams.begin(), tunnelCandidateTeams.end(),
			IsSkirmishStrategyTunnelTeamIDBefore);
		tunnelCandidateTeams.erase(std::unique(tunnelCandidateTeams.begin(),
			tunnelCandidateTeams.end()), tunnelCandidateTeams.end());
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
	Bool tunnelInfrastructureChecked = false;
	Bool hasTunnelRouteInfrastructure = false;
	TunnelTracker *tunnelTracker = nullptr;
	const ThingTemplate *tunnelPlan = nullptr;
	if (!preserveHiddenTarget && persistedTarget) {
		++targetAttemptCount;
		Bool targetCapability = activeTransitForPersistedTarget &&
			IsSkirmishAIStrategyTunnelTargetUsable(
				persistedTarget, m_player, enemy);
		if (!targetCapability)
			targetCapability = HasSkirmishStrategyTargetCapability(
				persistedTarget, airAttackers, groundAttackers,
				&quickPathQueryCount, targetFallbackEnabled);
		if (!targetCapability && tunnelTargetBootstrapEnabled) {
			if (!tunnelInfrastructureChecked) {
				tunnelInfrastructureChecked = true;
				tunnelTracker = m_player->getTunnelSystem();
				tunnelPlan = findTunnelContainBuildTemplate();
				hasTunnelRouteInfrastructure =
					HasSkirmishAIStrategyTunnelRouteInfrastructure(
						m_player, tunnelTracker,
						m_baseCenterSet && tunnelPlan &&
						IsSkirmishAIStrategyTunnelTemplate(tunnelPlan));
			}
			targetCapability = HasSkirmishAIStrategyTunnelTargetCapability(
				persistedTarget, m_player, enemy, tunnelCandidateTeams,
				visibleDefenseBlockers,
				tunnelTracker, hasTunnelRouteInfrastructure, tunnelProbeEpoch);
		}
		if (targetCapability)
			selectedTarget = persistedTarget;
	}
	if (!preserveHiddenTarget && !selectedTarget) {
		Object *targetBatch[MAX_SKIRMISH_STRATEGY_TARGET_CANDIDATES];
		Int targetBatchCount = 0;
		const Bool fallbackPass = targetFallbackEnabled &&
			m_strategyTargetFallbackPending;
		if (fallbackPass) {
			// Keep the expensive capability probes bounded while rotating through
			// targets ranked below the four most valuable visible structures.
			Object *afterCursor[MAX_SKIRMISH_STRATEGY_TARGET_CANDIDATES];
			Object *wrapped[MAX_SKIRMISH_STRATEGY_TARGET_CANDIDATES];
			Int afterCount = 0;
			Int wrappedCount = 0;
			Int batchIndex;
			for (Object *fallbackObject = TheGameLogic->getFirstObject();
				fallbackObject; fallbackObject = fallbackObject->getNextObject()) {
				if (fallbackObject->getControllingPlayer() != enemy ||
					!IsSkirmishStrategyIntelEligible(fallbackObject, m_player) ||
					fallbackObject->isEffectivelyDead() ||
					fallbackObject->isDestroyed() ||
					fallbackObject->testStatus(OBJECT_STATUS_SOLD) ||
					!IsSkirmishStrategyStaticTarget(fallbackObject) ||
					IsSkirmishStrategyTopTargetCandidate(
						fallbackObject, targetCandidates, targetCandidateCount))
					continue;
				if (m_strategyTargetFallbackAfterID == INVALID_ID ||
					fallbackObject->getID() > m_strategyTargetFallbackAfterID)
					InsertSkirmishStrategyTargetByID(
						afterCursor, &afterCount, fallbackObject);
				else
					InsertSkirmishStrategyTargetByID(
						wrapped, &wrappedCount, fallbackObject);
			}
			for (batchIndex = 0; batchIndex < afterCount &&
				targetBatchCount < MAX_SKIRMISH_STRATEGY_TARGET_CANDIDATES;
				++batchIndex)
				targetBatch[targetBatchCount++] = afterCursor[batchIndex];
			for (batchIndex = 0; batchIndex < wrappedCount &&
				targetBatchCount < MAX_SKIRMISH_STRATEGY_TARGET_CANDIDATES;
				++batchIndex)
				targetBatch[targetBatchCount++] = wrapped[batchIndex];
		} else {
			for (Int index = 0; index < targetCandidateCount; ++index)
				targetBatch[targetBatchCount++] = targetCandidates[index].object;
		}
		for (Int candidateIndex = 0;
			candidateIndex < targetBatchCount &&
			targetAttemptCount < MAX_SKIRMISH_STRATEGY_TARGET_CANDIDATES;
			++candidateIndex) {
			Object *candidate = targetBatch[candidateIndex];
			if (candidate == persistedTarget)
				continue;
			++targetAttemptCount;
			if (fallbackPass)
				m_strategyTargetFallbackAfterID = candidate->getID();
			Bool targetCapability = HasSkirmishStrategyTargetCapability(
					candidate, airAttackers, groundAttackers,
					&quickPathQueryCount, targetFallbackEnabled);
			if (!targetCapability && tunnelTargetBootstrapEnabled) {
				if (!tunnelInfrastructureChecked) {
					tunnelInfrastructureChecked = true;
					tunnelTracker = m_player->getTunnelSystem();
					tunnelPlan = findTunnelContainBuildTemplate();
					hasTunnelRouteInfrastructure =
						HasSkirmishAIStrategyTunnelRouteInfrastructure(
							m_player, tunnelTracker,
							m_baseCenterSet && tunnelPlan &&
							IsSkirmishAIStrategyTunnelTemplate(tunnelPlan));
				}
				targetCapability = HasSkirmishAIStrategyTunnelTargetCapability(
					candidate, m_player, enemy, tunnelCandidateTeams,
					visibleDefenseBlockers,
					tunnelTracker, hasTunnelRouteInfrastructure, tunnelProbeEpoch);
			}
			if (targetCapability) {
				selectedTarget = candidate;
				break;
			}
		}
		// A failed top-four pass earns one rotating fallback pass. Then return
		// to the highest-value targets, so newly reachable priorities are retried.
		if (targetFallbackEnabled)
			m_strategyTargetFallbackPending = !fallbackPass &&
				!selectedTarget && staticTargetCount > targetCandidateCount;
	}
	if (targetFallbackEnabled && selectedTarget)
		m_strategyTargetFallbackPending = false;
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

static Bool IsSkirmishTacticalRetreatPointSafe(
	const Coord3D *start, const Coord3D *destination,
	const std::vector<Object *> &hazards);

static Bool IsSkirmishAIStrategyTunnelTargetUsable(
	Object *target, Player *owner, Player *enemy)
{
	return target && owner && enemy &&
		IsSkirmishStrategyIntelEligible(target, owner) &&
		target->getControllingPlayer() == enemy &&
		IsSkirmishStrategyStaticTarget(target) &&
		!target->isEffectivelyDead() && !target->isDestroyed() &&
		!target->testStatus(OBJECT_STATUS_SOLD);
}

static Bool IsSkirmishAIDefenseTeamScriptFree(const TeamTemplateInfo *info)
{
	if (!info || !(info->m_isBaseDefense || info->m_isPerimeterDefense) ||
		info->m_executeActions ||
		!info->m_scriptOnCreate.isEmpty() || !info->m_scriptOnIdle.isEmpty() ||
		!info->m_scriptOnEnemySighted.isEmpty() ||
		!info->m_scriptOnAllClear.isEmpty() ||
		!info->m_scriptOnUnitDestroyed.isEmpty() ||
		!info->m_scriptOnDestroyed.isEmpty())
		return false;
	for (Int script = 0; script < MAX_GENERIC_SCRIPTS; ++script) {
		if (!info->m_teamGenericScripts[script].isEmpty()) return false;
	}
	return true;
}

static size_t GetSkirmishAIPatrolDefenderStart(
	const std::vector<Object *> &defenders, ObjectID afterID)
{
	if (afterID == INVALID_ID) return 0;
	size_t start = 0;
	while (start < defenders.size() && defenders[start]->getID() <= afterID)
		++start;
	return start == defenders.size() ? 0 : start;
}

void AISkirmishPlayer::updateDefensePatrol()
{
	if (!ShouldUseCurrentSkirmishAITacticalBehavior() || !usesStrategyBehavior() ||
		!m_player || !m_baseCenterSet || !TheAI || !TheAI->pathfinder() ||
		!TheTerrainLogic || !TheGameLogic)
		return;
	const UnsignedInt now = TheGameLogic->getFrame();
	if (m_defensePursuitTargetID != INVALID_ID) {
		Real leash = m_baseRadius + 250.0f;
		if (leash > 800.0f) leash = 800.0f;
		if (leash < 300.0f) leash = 300.0f;
		Team *ownedTeam = nullptr;
		for (Player::PlayerTeamList::const_iterator prototype =
				m_player->getPlayerTeams()->begin();
			prototype != m_player->getPlayerTeams()->end() && !ownedTeam;
			++prototype) {
			if (!IsSkirmishAIDefenseTeamScriptFree(
					(*prototype)->getTemplateInfo())) continue;
			for (DLINK_ITERATOR<Team> instance =
					(*prototype)->iterate_TeamInstanceList();
				!instance.done(); instance.advance()) {
				if (instance.cur()->getID() == m_defensePatrolTeamID) {
					ownedTeam = instance.cur();
					break;
				}
			}
		}
		Object *ownedObject = m_defensePatrolObjectID != INVALID_ID ?
			TheGameLogic->findObjectByID(m_defensePatrolObjectID) : nullptr;
		const Bool legacyTeamPatrol = m_defensePatrolObjectID == INVALID_ID &&
			ownedTeam != nullptr;
		const Bool ownedMemberValid = ownedObject &&
			ownedObject->getControllingPlayer() == m_player &&
			!ownedObject->isEffectivelyDead() && !ownedObject->isDestroyed() &&
			(m_defensePatrolTeamID == 0 ?
				ownedObject->getTeam() == m_player->getDefaultTeam() :
				ownedTeam && ownedObject->getTeam() == ownedTeam);
		Object *target = TheGameLogic->findObjectByID(m_defensePursuitTargetID);
		Bool teamInside = legacyTeamPatrol || ownedMemberValid;
		Int ownValue = 0;
		if (legacyTeamPatrol) {
			for (DLINK_ITERATOR<Object> member =
					ownedTeam->iterate_TeamMemberList();
				!member.done(); member.advance()) {
				Object *object = member.cur();
				if (!object || object->getControllingPlayer() != m_player ||
					object->isEffectivelyDead() || object->isDestroyed()) continue;
				const Real dx = object->getPosition()->x - m_baseCenter.x;
				const Real dy = object->getPosition()->y - m_baseCenter.y;
				if (dx * dx + dy * dy >= leash * leash) teamInside = false;
				const Int cost = object->getTemplate()->calcCostToBuild(m_player);
				if (cost > 0) ownValue = AddSkirmishStrategyValue(ownValue,
					(Int)((__int64)cost * GetSkirmishStrategyHealthPercent(object) / 100));
			}
		} else if (ownedMemberValid) {
			const Real dx = ownedObject->getPosition()->x - m_baseCenter.x;
			const Real dy = ownedObject->getPosition()->y - m_baseCenter.y;
			teamInside = dx * dx + dy * dy < leash * leash;
			const Int cost = ownedObject->getTemplate()->calcCostToBuild(m_player);
			if (cost > 0) ownValue =
				(Int)((__int64)cost * GetSkirmishStrategyHealthPercent(ownedObject) / 100);
		} else teamInside = false;
		const Bool targetValid = target && !target->isEffectivelyDead() &&
			!target->isDestroyed() && IsSkirmishStrategyIntelEligible(target, m_player) &&
			IsSkirmishStrategyCombatObject(target) &&
			target->getControllingPlayer() &&
			target->getControllingPlayer()->getDefaultTeam() &&
			m_player->getRelationship(
				target->getControllingPlayer()->getDefaultTeam()) == ENEMIES;
		Bool targetInside = false;
		Bool targetPenetrated = false;
		if (targetValid) {
			const Real dx = target->getPosition()->x - m_baseCenter.x;
			const Real dy = target->getPosition()->y - m_baseCenter.y;
			targetInside = dx * dx + dy * dy < (leash - 50.0f) * (leash - 50.0f);
			targetPenetrated = dx * dx + dy * dy <
				(0.7f * m_baseRadius) * (0.7f * m_baseRadius);
		}
		const UnsignedInt attackedFrame = m_player->getAttackedFrame();
		const Bool recentBasePressure = attackedFrame != 0 &&
			now - attackedFrame < 3 * LOGICFRAMES_PER_SECOND;
		const Bool baseSafe = !m_recoveryImpossible &&
			(!recentBasePressure || targetPenetrated);
		const Bool immediateRecall = !targetValid || !teamInside ||
			!targetInside || !baseSafe ||
			now - m_defensePursuitStartFrame >= 4 * LOGICFRAMES_PER_SECOND;
		if (!immediateRecall &&
			!IsSkirmishStrategyFrameReached(now, m_defenseNextPatrolFrame))
			return;
		m_defenseNextPatrolFrame = now + LOGICFRAMES_PER_SECOND;
		Int enemyValue = 0;
		if (!immediateRecall) {
			for (Object *enemy = TheGameLogic->getFirstObject(); enemy;
				enemy = enemy->getNextObject()) {
				Player *owner = enemy->getControllingPlayer();
				if (!owner || !owner->getDefaultTeam() ||
					m_player->getRelationship(owner->getDefaultTeam()) != ENEMIES ||
					!IsSkirmishStrategyIntelEligible(enemy, m_player) ||
					!IsSkirmishStrategyCombatObject(enemy) ||
					enemy->isEffectivelyDead()) continue;
				const Real ex = enemy->getPosition()->x - target->getPosition()->x;
				const Real ey = enemy->getPosition()->y - target->getPosition()->y;
				if (ex * ex + ey * ey > 250.0f * 250.0f) continue;
				const Int cost = enemy->getTemplate()->calcCostToBuild(owner);
				if (cost > 0) enemyValue = AddSkirmishStrategyValue(enemyValue,
					(Int)((__int64)cost * GetSkirmishStrategyHealthPercent(enemy) / 100));
			}
		}
		if (ShouldRecallSkirmishAIDefender(targetValid, teamInside,
			targetInside, (__int64)ownValue * 4 >= (__int64)enemyValue * 5 &&
				enemyValue > 0,
			baseSafe,
			now - m_defensePursuitStartFrame, 4 * LOGICFRAMES_PER_SECOND)) {
			AIGroupPtr group = TheAI->createGroup();
			if (group) {
				if (legacyTeamPatrol) {
#if RETAIL_COMPATIBLE_AIGROUP
					ownedTeam->getTeamAsAIGroup(group);
#else
					ownedTeam->getTeamAsAIGroup(group.Peek());
#endif
				} else if (ownedMemberValid)
					group->add(ownedObject);
				if (legacyTeamPatrol || ownedMemberValid)
					group->groupGuardPosition(&m_baseCenter,
						GUARDMODE_GUARD_WITHOUT_PURSUIT, CMD_FROM_AI);
			}
			m_defensePursuitTargetID = INVALID_ID;
			m_defensePursuitStartFrame = 0;
			m_defenseNextPatrolFrame = now + 12 * LOGICFRAMES_PER_SECOND;
			m_defenseQuietPatrolDeadlineFrame = 0;
		}
		return;
	}
	if (!IsSkirmishStrategyFrameReached(now, m_defenseNextPatrolFrame))
		return;
	m_defenseNextPatrolFrame = now + 10 * LOGICFRAMES_PER_SECOND;
	SkirmishAIDefenseContext context;
	CollectSkirmishAIDefenseContext(m_player, m_baseCenter,
		m_baseRadius, &context);
	Int route = DecideSkirmishAIDefensePatrolRoute(
		&context.threat, m_defensePatrolRoute, true, true, 100, 150);
	const Bool quietPatrol = !IsSkirmishAIDefenseRoute(route);
	if (quietPatrol) {
		if (IsSkirmishAIDefenseRoute(m_defensePatrolRoute) &&
			context.anchors[m_defensePatrolRoute].available)
			route = m_defensePatrolRoute;
		else
			route = SelectQuietSkirmishAIDefenseRoute(context.anchors,
				now / (10 * LOGICFRAMES_PER_SECOND), m_player->getPlayerIndex());
	}
	if (!IsSkirmishAIDefenseRoute(route) ||
		!context.anchors[route].available) {
		m_defensePatrolRoute = SKIRMISH_AI_DEFENSE_NO_ROUTE;
		m_defensePatrolTeamID = 0;
		m_defensePatrolObjectID = INVALID_ID;
		m_defenseQuietPatrolDeadlineFrame = 0;
		return;
	}
	Real leash = m_baseRadius + 250.0f;
	if (leash > 800.0f) leash = 800.0f;
	if (leash < 300.0f) leash = 300.0f;
	const Real leashSqr = leash * leash;
	Team *selectedTeam = nullptr;
	Object *representative = nullptr;
	Player::PlayerTeamList::const_iterator prototype;
	for (prototype = m_player->getPlayerTeams()->begin();
		prototype != m_player->getPlayerTeams()->end(); ++prototype) {
		const TeamTemplateInfo *info = (*prototype)->getTemplateInfo();
		if (!IsSkirmishAIDefenseTeamScriptFree(info)) continue;
		for (DLINK_ITERATOR<Team> instance =
			(*prototype)->iterate_TeamInstanceList();
			!instance.done(); instance.advance()) {
			Team *team = instance.cur();
			if (!team || team == m_player->getDefaultTeam() ||
				!team->isActive() || !team->hasAnyObjects() ||
				(team->getID() != m_defensePatrolTeamID && !team->isIdle()))
				continue;
			Object *first = nullptr;
			Bool safe = true;
			for (DLINK_ITERATOR<Object> member = team->iterate_TeamMemberList();
				!member.done(); member.advance()) {
				Object *object = member.cur();
				if (!object || object->isEffectivelyDead() || object->isDestroyed())
					continue;
				const Real dx = object->getPosition()->x - m_baseCenter.x;
				const Real dy = object->getPosition()->y - m_baseCenter.y;
				if (object->getControllingPlayer() != m_player ||
					object->isContained() || object->isKindOf(KINDOF_AIRCRAFT) ||
					!IsSkirmishStrategyCombatObject(object) ||
					!object->getAIUpdateInterface() || dx * dx + dy * dy > leashSqr) {
					safe = false;
					break;
				}
				if (!first || object->getID() < first->getID()) first = object;
			}
			if (!safe || !first) continue;
			if (!selectedTeam || team->getID() < selectedTeam->getID()) {
				selectedTeam = team;
				representative = first;
			}
		}
	}
	Object *fallback = nullptr;
	if (!selectedTeam) {
		// Default-team combat units have no team script. Borrow one idle unit,
		// never a builder, collector, aircraft, or an active attack recipient.
		for (Object *object = TheGameLogic->getFirstObject(); object;
			object = object->getNextObject()) {
			AIUpdateInterface *ai = object->getAIUpdateInterface();
			if (object->getControllingPlayer() != m_player ||
				object->getTeam() != m_player->getDefaultTeam() ||
				!IsSkirmishStrategyCombatObject(object) ||
				object->isKindOf(KINDOF_AIRCRAFT) || object->isContained() ||
				object->isEffectivelyDead() || object->isDestroyed() ||
				!ai || (object->getID() != m_defensePatrolObjectID &&
					!ai->isIdle()))
				continue;
			const Real dx = object->getPosition()->x - m_baseCenter.x;
			const Real dy = object->getPosition()->y - m_baseCenter.y;
			if (dx * dx + dy * dy > leashSqr) continue;
			if (!fallback || object->getID() < fallback->getID())
				fallback = object;
		}
		representative = fallback;
	}
	if (!representative) {
		m_defensePatrolTeamID = 0;
		m_defensePatrolObjectID = INVALID_ID;
		m_defenseQuietPatrolDeadlineFrame = 0;
		return;
	}
	std::vector<Object *> patrolDefenders;
	if (selectedTeam) {
		for (DLINK_ITERATOR<Object> member =
				selectedTeam->iterate_TeamMemberList();
			!member.done(); member.advance()) {
			Object *object = member.cur();
			if (!object || object->getControllingPlayer() != m_player ||
				object->isEffectivelyDead() || object->isDestroyed() ||
				object->isContained() || object->isKindOf(KINDOF_AIRCRAFT) ||
				!IsSkirmishStrategyCombatObject(object) ||
				!object->getAIUpdateInterface())
				continue;
			const Real dx = object->getPosition()->x - m_baseCenter.x;
			const Real dy = object->getPosition()->y - m_baseCenter.y;
			if (dx * dx + dy * dy > leashSqr)
				continue;
			patrolDefenders.push_back(object);
		}
	} else if (fallback) {
		patrolDefenders.push_back(fallback);
	}
	std::sort(patrolDefenders.begin(), patrolDefenders.end(),
		IsSkirmishAIStrategyObjectIDBefore);
	if (patrolDefenders.empty()) {
		m_defensePatrolTeamID = 0;
		m_defensePatrolObjectID = INVALID_ID;
		m_defenseQuietPatrolDeadlineFrame = 0;
		return;
	}
	representative = patrolDefenders[0];
	Bool samePatrolDefender = false;
	if (selectedTeam && selectedTeam->getID() == m_defensePatrolTeamID) {
		for (size_t defender = 0; defender < patrolDefenders.size(); ++defender) {
			if (patrolDefenders[defender]->getID() == m_defensePatrolObjectID) {
				representative = patrolDefenders[defender];
				samePatrolDefender = true;
				break;
			}
		}
	} else if (!selectedTeam && fallback && m_defensePatrolTeamID == 0 &&
		fallback->getID() == m_defensePatrolObjectID)
		samePatrolDefender = true;
	const Bool defenderMoving =
		!representative->getAIUpdateInterface()->isIdle();
	if (quietPatrol && m_defenseQuietPatrolDeadlineFrame != 0 &&
		route == m_defensePatrolRoute) {
		const Real waypointDx = representative->getPosition()->x -
			m_defenseQuietPatrolWaypoint.x;
		const Real waypointDy = representative->getPosition()->y -
			m_defenseQuietPatrolWaypoint.y;
		if (ShouldHoldQuietSkirmishAIDefenseWaypoint(true,
			samePatrolDefender && defenderMoving,
			context.anchors[route].available,
			IsSkirmishStrategyFrameReached(now,
				m_defenseQuietPatrolDeadlineFrame),
			waypointDx * waypointDx + waypointDy * waypointDy, 85.0f))
			return;
		if (samePatrolDefender)
			route = AdvanceQuietSkirmishAIDefenseRoute(context.anchors,
				route, now / (10 * LOGICFRAMES_PER_SECOND),
				m_player->getPlayerIndex());
	}
	Int tacticalQuickPathQueryCount = 0;
	Real radius = m_baseRadius + 50.0f;
	if (radius > leash - 80.0f) radius = leash - 80.0f;
	if (radius < 120.0f) radius = 120.0f;
	const Real side = (now / (10 * LOGICFRAMES_PER_SECOND)) & 1 ?
		60.0f : -60.0f;
	Coord3D destination = m_baseCenter;
	Bool reachableRoute = false;
	Int routePathQueries = 0;
	Object *selectedRouteDefender = nullptr;
	const size_t routeDefenderStart = GetSkirmishAIPatrolDefenderStart(
		patrolDefenders, m_defensePatrolRouteMemberAfterID);
	for (Int attempt = 0; attempt < SKIRMISH_AI_DEFENSE_ROUTE_COUNT; ++attempt) {
		const Int candidateRoute = (route + attempt) % SKIRMISH_AI_DEFENSE_ROUTE_COUNT;
		if (!context.anchors[candidateRoute].available)
			continue;
		Coord3D candidate = m_baseCenter;
		candidate.x += context.direction[candidateRoute].x * radius -
			context.direction[candidateRoute].y * side;
		candidate.y += context.direction[candidateRoute].y * radius +
			context.direction[candidateRoute].x * side;
		candidate.z = TheTerrainLogic->getGroundHeight(candidate.x, candidate.y);
		const Real dx = candidate.x - m_baseCenter.x;
		const Real dy = candidate.y - m_baseCenter.y;
		if (dx * dx + dy * dy > leashSqr)
			continue;
		Bool candidateReachable = false;
		// Reserve one query for each remaining route, including routes whose
		// anchors may prove unavailable, so the first lane cannot use all four.
		const Int candidateQueryAllowance = 4 - routePathQueries -
			(SKIRMISH_AI_DEFENSE_ROUTE_COUNT - attempt - 1);
		Int candidateQueries = 0;
		for (size_t defender = 0; defender < patrolDefenders.size(); ++defender) {
			if (candidateQueries >= candidateQueryAllowance ||
				routePathQueries >= 4 ||
				!TryConsumeSkirmishAITacticalPathQuery(
					&tacticalQuickPathQueryCount,
					MAX_SKIRMISH_AI_TACTICAL_QUICK_PATH_QUERIES_PER_UPDATE))
				break;
			// Rotate across the whole team, including failed probes on other lanes.
			Object *pathDefender = patrolDefenders[
				(routeDefenderStart + routePathQueries) % patrolDefenders.size()];
			m_defensePatrolRouteMemberAfterID = pathDefender->getID();
			AIUpdateInterface *pathDefenderAI =
				pathDefender->getAIUpdateInterface();
			++routePathQueries;
			++candidateQueries;
			if (pathDefenderAI &&
				TheAI->pathfinder()->clientSafeQuickDoesPathExist(
					pathDefenderAI->getLocomotorSet(),
					pathDefender->getPosition(), &candidate)) {
				selectedRouteDefender = pathDefender;
				candidateReachable = true;
				break;
			}
		}
		if (!candidateReachable)
			continue;
		route = candidateRoute;
		destination = candidate;
		reachableRoute = true;
		break;
	}
	if (!reachableRoute || !selectedRouteDefender)
		return;
	AIGroupPtr group = TheAI->createGroup();
	if (!group) return;
	// A reachable team member does not prove the rest of a mixed team can move here.
	group->add(selectedRouteDefender);
	m_defensePatrolTeamID = selectedTeam ? selectedTeam->getID() : 0;
	m_defensePatrolObjectID = selectedRouteDefender->getID();
	std::vector<Object *> contacts;
	for (Object *enemy = TheGameLogic->getFirstObject(); enemy;
		enemy = enemy->getNextObject()) {
		Player *owner = enemy->getControllingPlayer();
		if (!owner || !owner->getDefaultTeam() ||
			m_player->getRelationship(owner->getDefaultTeam()) != ENEMIES ||
			!IsSkirmishStrategyIntelEligible(enemy, m_player) ||
			!IsSkirmishStrategyCombatObject(enemy) ||
			enemy->isKindOf(KINDOF_AIRCRAFT) || enemy->isContained() ||
			enemy->isEffectivelyDead() || enemy->isDestroyed()) continue;
		const Real ex = enemy->getPosition()->x - m_baseCenter.x;
		const Real ey = enemy->getPosition()->y - m_baseCenter.y;
		if (ex * ex + ey * ey > (leash - 150.0f) * (leash - 150.0f) ||
			ClassifySkirmishAIDefenseRoute((Int)ex, (Int)ey,
				context.anchors) != route) continue;
		contacts.push_back(enemy);
	}
	std::sort(contacts.begin(), contacts.end(),
		IsSkirmishAIStrategyObjectIDBefore);
	const UnsignedInt attackedFrame = m_player->getAttackedFrame();
	const Bool recentBasePressure = attackedFrame != 0 &&
		now - attackedFrame < 3 * LOGICFRAMES_PER_SECOND;
	if (!contacts.empty() && !m_recoveryImpossible) {
		Int ownValue = 0;
		const Int cost = selectedRouteDefender->getTemplate()->calcCostToBuild(m_player);
		if (cost > 0) ownValue =
			(Int)((__int64)cost * GetSkirmishStrategyHealthPercent(
				selectedRouteDefender) / 100);
		size_t start = 0;
		if (m_defenseInterceptProbeAfterID != INVALID_ID) {
			while (start < contacts.size() &&
				contacts[start]->getID() <= m_defenseInterceptProbeAfterID)
				++start;
			if (start == contacts.size()) start = 0;
		}
		Int memberProbes = 0;
		for (size_t offset = 0; offset < contacts.size() &&
			memberProbes < MAX_SKIRMISH_AI_TACTICAL_QUICK_PATH_QUERIES_PER_UPDATE &&
			tacticalQuickPathQueryCount <
				MAX_SKIRMISH_AI_TACTICAL_QUICK_PATH_QUERIES_PER_UPDATE;
			++offset) {
			Object *intercept = contacts[(start + offset) % contacts.size()];
			m_defenseInterceptProbeAfterID = intercept->getID();
			const Real ex = intercept->getPosition()->x - m_baseCenter.x;
			const Real ey = intercept->getPosition()->y - m_baseCenter.y;
			const Bool targetPenetrated = ex * ex + ey * ey <
				(0.7f * m_baseRadius) * (0.7f * m_baseRadius);
			if (recentBasePressure && !targetPenetrated)
				continue;
			++memberProbes;
			m_defenseInterceptMemberAfterID = selectedRouteDefender->getID();
			if (selectedRouteDefender->getAbleToAttackSpecificObject(
					ATTACK_NEW_TARGET, intercept, CMD_FROM_AI) ==
				ATTACKRESULT_NOT_POSSIBLE)
				continue;
			if (!TryConsumeSkirmishAITacticalPathQuery(
					&tacticalQuickPathQueryCount,
					MAX_SKIRMISH_AI_TACTICAL_QUICK_PATH_QUERIES_PER_UPDATE))
				break;
			AIUpdateInterface *pathDefenderAI =
				selectedRouteDefender->getAIUpdateInterface();
			if (!pathDefenderAI ||
				!TheAI->pathfinder()->clientSafeQuickDoesPathExist(
					pathDefenderAI->getLocomotorSet(),
					selectedRouteDefender->getPosition(),
					intercept->getPosition()))
				continue;
			Int enemyValue = 0;
			for (Object *enemy = TheGameLogic->getFirstObject(); enemy;
				enemy = enemy->getNextObject()) {
				Player *owner = enemy->getControllingPlayer();
				if (!owner || !owner->getDefaultTeam() ||
					m_player->getRelationship(owner->getDefaultTeam()) != ENEMIES ||
					!IsSkirmishStrategyIntelEligible(enemy, m_player) ||
					!IsSkirmishStrategyCombatObject(enemy) ||
					enemy->isEffectivelyDead()) continue;
				const Real enemyDx = enemy->getPosition()->x - intercept->getPosition()->x;
				const Real enemyDy = enemy->getPosition()->y - intercept->getPosition()->y;
				if (enemyDx * enemyDx + enemyDy * enemyDy > 250.0f * 250.0f) continue;
				const Int cost = enemy->getTemplate()->calcCostToBuild(owner);
				if (cost > 0) enemyValue = AddSkirmishStrategyValue(enemyValue,
					(Int)((__int64)cost * GetSkirmishStrategyHealthPercent(enemy) / 100));
			}
			if (ShouldSkirmishAIDefenderPursue(true, true, true,
				(Int)sqrt(ex * ex + ey * ey), (Int)leash,
				ownValue, enemyValue)) {
				group->groupAttackObject(intercept, NO_MAX_SHOTS_LIMIT, CMD_FROM_AI);
				m_defensePursuitTargetID = intercept->getID();
				m_defensePursuitStartFrame = now;
				m_defenseNextPatrolFrame = now + LOGICFRAMES_PER_SECOND;
				m_defensePatrolRoute = route;
				m_defenseQuietPatrolDeadlineFrame = 0;
				return;
			}
		}
	}
	group->groupGuardPosition(&destination,
		GUARDMODE_GUARD_WITHOUT_PURSUIT, CMD_FROM_AI);
	m_defensePatrolRoute = route;
	if (quietPatrol) {
		m_defenseQuietPatrolWaypoint = destination;
		m_defenseQuietPatrolDeadlineFrame = now +
			90 * LOGICFRAMES_PER_SECOND;
	} else m_defenseQuietPatrolDeadlineFrame = 0;
}

static Bool IsSkirmishAITacticalTeamBefore(Team *left, Team *right)
{
	return left->getID() < right->getID();
}

enum {
	MAX_SKIRMISH_AI_TACTICAL_TEAM_EVALUATIONS_PER_UPDATE = 64,
	MAX_SKIRMISH_AI_TACTICAL_RETREAT_PATH_QUERIES_PER_UPDATE = 32
};

void AISkirmishPlayer::updateTacticalTeams()
{
	if (!ShouldUseCurrentSkirmishAITacticalBehavior() || !usesStrategyBehavior() ||
		!m_player || !TheAI ||
		!TheAI->pathfinder() || !m_baseCenterSet)
		return;
	const UnsignedInt now = TheGameLogic->getFrame();
	if (!IsSkirmishStrategyFrameReached(now, m_tacticalNextTeamScanFrame))
		return;
	m_tacticalNextTeamScanFrame = now + 2 * LOGICFRAMES_PER_SECOND;
	Int aggregateTunnelPathQueryCount = 0;
	Int tacticalQuickPathQueryCount = 0;
	Int retreatPathQueryCount = 0;
	std::vector<Team *> teams;
	Player::PlayerTeamList::const_iterator prototype;
	for (prototype = m_player->getPlayerTeams()->begin();
		prototype != m_player->getPlayerTeams()->end(); ++prototype) {
		for (DLINK_ITERATOR<Team> instance = (*prototype)->iterate_TeamInstanceList();
			!instance.done(); instance.advance()) {
			Team *team = instance.cur();
			if (!IsSkirmishStrategyOffensiveTeam(team, m_player))
				continue;
			std::map<UnsignedInt, TacticalTeamState>::const_iterator previous =
				m_tacticalTeams.find(team->getID());
			const Bool activeTunnelTransit =
				previous != m_tacticalTeams.end() &&
				previous->second.tunnelTransitPhase !=
					SKIRMISH_AI_TUNNEL_TRANSIT_NONE;
			if (activeTunnelTransit ||
				HasSkirmishStrategyPotentialOffensiveRecipient(team, m_player))
				teams.push_back(team);
		}
	}
	std::sort(teams.begin(), teams.end(), IsSkirmishAITacticalTeamBefore);
	// The world list is stable during this synchronous update. Filter once so
	// each team's tactical decisions walk only the relevant known objects.
	std::vector<Object *> visibleCombatEnemies;
	std::vector<Object *> retreatHazards;
	std::vector<Object *> alternateTargets;
	std::vector<Object *> corridorDefenses;
	std::vector<Object *> retreatFacilities;
	if (!teams.empty() &&
		m_strategyState.currentMode == SKIRMISH_STRATEGY_ASSAULT) {
		for (Object *object = TheGameLogic->getFirstObject(); object;
			object = object->getNextObject()) {
			Player *owner = object->getControllingPlayer();
			if (owner == m_player) {
				if (!object->isEffectivelyDead() && !object->isDestroyed() &&
					!object->testStatus(OBJECT_STATUS_UNDER_CONSTRUCTION) &&
					(object->isKindOf(KINDOF_REPAIR_PAD) ||
					 object->isKindOf(KINDOF_HEAL_PAD) ||
					 object->isKindOf(KINDOF_FS_AIRFIELD) ||
					 object->isKindOf(KINDOF_FS_BASE_DEFENSE) ||
					 object->isKindOf(KINDOF_FS_BARRACKS) ||
					 object->isKindOf(KINDOF_FS_WARFACTORY)))
					retreatFacilities.push_back(object);
				continue;
			}
			if (!owner || !owner->getDefaultTeam() ||
				m_player->getRelationship(owner->getDefaultTeam()) != ENEMIES ||
				!IsSkirmishStrategyIntelEligible(object, m_player) ||
				object->isEffectivelyDead() || object->isDestroyed())
				continue;
			const Bool combat = IsSkirmishStrategyCombatObject(object);
			const Bool armedStructure = object->isKindOf(KINDOF_STRUCTURE) &&
				!object->testStatus(OBJECT_STATUS_UNDER_CONSTRUCTION) &&
				!object->testStatus(OBJECT_STATUS_SOLD) &&
				object->getLargestWeaponRange() > 0.0f;
			if (combat)
				visibleCombatEnemies.push_back(object);
			if (combat || armedStructure)
				retreatHazards.push_back(object);
			if (owner == m_currentEnemy &&
				!object->testStatus(OBJECT_STATUS_SOLD)) {
				if (IsSkirmishStrategyStaticTarget(object))
					alternateTargets.push_back(object);
				if (object->isKindOf(KINDOF_FS_BASE_DEFENSE))
					corridorDefenses.push_back(object);
			}
		}
	}
	size_t active = 0;
	for (std::map<UnsignedInt, TacticalTeamState>::iterator old = m_tacticalTeams.begin();
		old != m_tacticalTeams.end();) {
		while (active < teams.size() && teams[active]->getID() < old->first)
			++active;
		if (active == teams.size() || teams[active]->getID() != old->first)
			m_tacticalTeams.erase(old++);
		else
			++old;
	}
	// Visit a complete set of windows before shifting their boundaries. The
	// one-team shift lets every team lead a budget-limited scan over time.
	const UnsignedInt scanTick = now / (2 * LOGICFRAMES_PER_SECOND);
	const size_t firstTeamIndex = (size_t)
		GetSkirmishAITacticalTeamProbeStartIndex(
			scanTick, (unsigned int)teams.size(),
			MAX_SKIRMISH_AI_TACTICAL_TEAM_EVALUATIONS_PER_UPDATE);
	Int evaluatedTeams = 0;
	for (size_t offset = 0; offset < teams.size(); ++offset) {
		const size_t i = (firstTeamIndex + offset) % teams.size();
		Team *team = teams[i];
		std::map<UnsignedInt, TacticalTeamState>::iterator existing =
			m_tacticalTeams.find(team->getID());
		if (existing != m_tacticalTeams.end() &&
			existing->second.tunnelTransitPhase !=
				SKIRMISH_AI_TUNNEL_TRANSIT_NONE) {
			TacticalTeamState &transit = existing->second;
			if (IsSkirmishStrategyFrameReached(now, transit.nextCheckFrame)) {
				transit.nextCheckFrame = now + 2 * LOGICFRAMES_PER_SECOND;
				updateTunnelTransit(team, transit, now);
			}
			continue;
		}
		if (evaluatedTeams >=
			MAX_SKIRMISH_AI_TACTICAL_TEAM_EVALUATIONS_PER_UPDATE)
			continue;
		const Bool newTacticalTeam =
			existing == m_tacticalTeams.end();
		if (newTacticalTeam &&
			m_tacticalTeams.size() >= MAX_SKIRMISH_AI_TACTICAL_TEAMS)
			continue;
		TacticalTeamState &state = m_tacticalTeams[team->getID()];
		if (newTacticalTeam)
			state.tunnelCooldownUntilFrame = now +
				(team->getID() % 4) * 2 * LOGICFRAMES_PER_SECOND;
		if (!IsSkirmishStrategyFrameReached(now, state.nextCheckFrame))
			continue;
		state.nextCheckFrame = now + 2 * LOGICFRAMES_PER_SECOND;
		++evaluatedTeams;
		Int health = 0;
		Int teamCombatValue = 0;
		Int memberCount = 0;
		Object *representative = nullptr;
		Bool hasAircraft = false;
		Bool hasArtillery = false;
		for (DLINK_ITERATOR<Object> member = team->iterate_TeamMemberList();
			!member.done(); member.advance()) {
			Object *object = member.cur();
			if (!IsSkirmishStrategyPotentialOffensiveRecipient(object, m_player, team))
				continue;
			++memberCount;
			const Int memberHealth = GetSkirmishStrategyHealthPercent(object);
			health += memberHealth;
			const Int cost = object->getTemplate()->calcCostToBuild(m_player);
			if (cost > 0)
				teamCombatValue = AddSkirmishStrategyValue(teamCombatValue,
					(Int)((__int64)cost * memberHealth / 100));
			if (!representative ||
				(representative->isKindOf(KINDOF_AIRCRAFT) &&
				 !object->isKindOf(KINDOF_AIRCRAFT)) ||
				(representative->isKindOf(KINDOF_AIRCRAFT) ==
				 object->isKindOf(KINDOF_AIRCRAFT) &&
				 object->getID() < representative->getID()))
				representative = object;
			if (object->isKindOf(KINDOF_AIRCRAFT))
				hasAircraft = true;
			else if (object->getLargestWeaponRange() >= 250.0f)
				hasArtillery = true;
		}
		// Balanced and Fortify hand control back to the native team scripts.
		// A wounded team must not remain in the tactical regroup loop forever.
		if (m_strategyState.currentMode != SKIRMISH_STRATEGY_ASSAULT) {
			state.retreating = false;
			state.woundedReserve = false;
			state.alternateAttackIssuedTargetID = INVALID_ID;
			continue;
		}
		if (!representative || !memberCount)
			continue;
		const Int averageHealth = health / memberCount;
		Bool reengage = false;
		if (state.woundedReserve) {
			if (averageHealth < 50)
				continue;
			state.woundedReserve = false;
			state.nextRetreatFrame = now + 20 * LOGICFRAMES_PER_SECOND;
			reengage = true;
		}
		if (state.retreating && !IsSkirmishStrategyFrameReached(now, state.regroupUntilFrame))
			continue;
		if (state.retreating && averageHealth < 50) {
			if (now - state.retreatStartFrame >= 30 * LOGICFRAMES_PER_SECOND) {
				// Stay on the safe guard order as a defensive reserve until healed
				// or a mode change gives control back to the team script.
				state.retreating = false;
				state.woundedReserve = true;
				continue;
			}
			state.regroupUntilFrame = now + 8 * LOGICFRAMES_PER_SECOND;
			continue;
		}
		if (state.retreating &&
			state.routeExhaustedTargetID == m_strategyState.strategicTargetID &&
			state.routeExhaustedTargetID != INVALID_ID &&
			!IsSkirmishStrategyFrameReached(now, state.routeExhaustedUntilFrame)) {
			state.regroupUntilFrame = now + 8 * LOGICFRAMES_PER_SECOND;
			continue;
		}
		if (state.retreating) {
			state.retreating = false;
			state.nextRetreatFrame = now + 20 * LOGICFRAMES_PER_SECOND;
			reengage = true;
		}
		Object *target = state.targetID != INVALID_ID
			? TheGameLogic->findObjectByID(state.targetID) : nullptr;
		if (target && (!IsSkirmishStrategyIntelEligible(target, m_player) ||
			target->getControllingPlayer() != m_currentEnemy ||
			!IsSkirmishStrategyStaticTarget(target) ||
			target->isEffectivelyDead() || target->isDestroyed() ||
			target->testStatus(OBJECT_STATUS_SOLD)))
			target = nullptr;
		if (!target && m_strategyState.strategicTargetID != INVALID_ID) {
			target = TheGameLogic->findObjectByID(m_strategyState.strategicTargetID);
			if (target && (!IsSkirmishStrategyIntelEligible(target, m_player) ||
				target->getControllingPlayer() != m_currentEnemy ||
				!IsSkirmishStrategyStaticTarget(target) ||
				target->isEffectivelyDead() || target->isDestroyed() ||
				target->testStatus(OBJECT_STATUS_SOLD)))
				target = nullptr;
		}
		Object *threat = nullptr;
		Int visibleThreatValue = 0;
		Real threatDistance = 350.0f * 350.0f;
		for (size_t enemyIndex = 0;
			enemyIndex < visibleCombatEnemies.size(); ++enemyIndex) {
			Object *enemy = visibleCombatEnemies[enemyIndex];
			Player *owner = enemy->getControllingPlayer();
			const Real dx = enemy->getPosition()->x - representative->getPosition()->x;
			const Real dy = enemy->getPosition()->y - representative->getPosition()->y;
			const Real distance = dx * dx + dy * dy;
			if (distance <= 350.0f * 350.0f) {
				const Int cost = enemy->getTemplate()->calcCostToBuild(owner);
				if (cost > 0)
					visibleThreatValue = AddSkirmishStrategyValue(visibleThreatValue,
						(Int)((__int64)cost * GetSkirmishStrategyHealthPercent(enemy) / 100));
			}
			if (distance < threatDistance ||
				(distance == threatDistance && threat && enemy->getID() < threat->getID())) {
				threat = enemy;
				threatDistance = distance;
			}
		}
		Int threatCapableCount = 0;
		if (threat) {
			for (DLINK_ITERATOR<Object> member = team->iterate_TeamMemberList();
				!member.done(); member.advance()) {
				Object *object = member.cur();
				if (IsSkirmishStrategyPotentialOffensiveRecipient(object, m_player, team) &&
					object->getAbleToAttackSpecificObject(ATTACK_NEW_TARGET, threat,
						CMD_FROM_AI) != ATTACKRESULT_NOT_POSSIBLE)
					++threatCapableCount;
			}
		}
		Int targetCapableCount = 0;
		if (target) {
			for (DLINK_ITERATOR<Object> member = team->iterate_TeamMemberList();
				!member.done(); member.advance()) {
				Object *object = member.cur();
				if (IsSkirmishStrategyPotentialOffensiveRecipient(object, m_player, team) &&
					object->getAbleToAttackSpecificObject(ATTACK_NEW_TARGET, target,
						CMD_FROM_AI) != ATTACKRESULT_NOT_POSSIBLE)
					++targetCapableCount;
			}
		}
		Bool stalled = false;
		if (target && m_strategyState.currentMode == SKIRMISH_STRATEGY_ASSAULT) {
			const Real dx = target->getPosition()->x - representative->getPosition()->x;
			const Real dy = target->getPosition()->y - representative->getPosition()->y;
			const Real distance = dx * dx + dy * dy;
			const Real targetHealth = target->getBodyModule()
				? target->getBodyModule()->getHealth() : 0.0f;
			if (state.targetID != target->getID()) {
				state.targetID = target->getID();
				state.targetHealth = targetHealth;
				state.distanceToTargetSqr = distance;
				state.lastProgressFrame = now;
				state.blockedSinceFrame = 0;
				state.approachAttempt = 0;
				state.alternateProbeAfterID = INVALID_ID;
				state.alternateAttackIssuedTargetID = INVALID_ID;
				state.tunnelBuilderWaitUntilFrame = 0;
				state.tunnelBuilderWindowsRemaining = 0;
				state.routeExhaustedTargetID = INVALID_ID;
				state.routeExhaustedUntilFrame = 0;
			} else {
				if (targetHealth < state.targetHealth ||
					distance + 40.0f * 40.0f < state.distanceToTargetSqr) {
					state.lastProgressFrame = now;
					state.distanceToTargetSqr = distance;
					state.routeExhaustedTargetID = INVALID_ID;
					state.routeExhaustedUntilFrame = 0;
					state.alternateProbeAfterID = INVALID_ID;
				}
				state.targetHealth = targetHealth;
				AIUpdateInterface *ai = representative->getAIUpdateInterface();
				if (ai && (ai->isBlockedAndStuck() ||
					ai->getNumFramesBlocked() > 2 * LOGICFRAMES_PER_SECOND)) {
					if (!state.blockedSinceFrame)
						state.blockedSinceFrame = now;
				} else {
					state.blockedSinceFrame = 0;
				}
				stalled = targetCapableCount == 0 ||
					now - state.lastProgressFrame >= 12 * LOGICFRAMES_PER_SECOND ||
					(state.blockedSinceFrame &&
					 now - state.blockedSinceFrame >= 6 * LOGICFRAMES_PER_SECOND);
			}
		} else {
			state.targetID = INVALID_ID;
			state.blockedSinceFrame = 0;
			state.alternateAttackIssuedTargetID = INVALID_ID;
		}
		if (stalled && !IsSkirmishStrategyFrameReached(now, state.regroupUntilFrame))
			stalled = false;
		const Bool outmatched = teamCombatValue > 0 &&
			(__int64)visibleThreatValue * 2 >= (__int64)teamCombatValue * 5;
		const Bool retreat = IsSkirmishStrategyFrameReached(now, state.nextRetreatFrame) &&
			(averageHealth < 35 || (threat && threatCapableCount == 0) || outmatched);
		Bool alternateReady = false;
		if (target && target->getID() != m_strategyState.strategicTargetID &&
			targetCapableCount > 0) {
			const Real dx = target->getPosition()->x - representative->getPosition()->x;
			const Real dy = target->getPosition()->y - representative->getPosition()->y;
			const Real radius = target->getTemplate()->getTemplateGeometryInfo()
				.getBoundingCircleRadius();
			const Real closeEnough = radius +
				max(representative->getLargestWeaponRange(), 150.0f) +
				2.0f * PATHFIND_CELL_SIZE_F;
			alternateReady = dx * dx + dy * dy <= closeEnough * closeEnough;
		}
		if (!alternateReady || retreat)
			state.alternateAttackIssuedTargetID = INVALID_ID;
		if (!retreat && !stalled && !reengage &&
			(!alternateReady || state.alternateAttackIssuedTargetID == target->getID()))
			continue;
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
		team->iterateObjects(CollectSkirmishStrategyGroupRecipient, &context);
		if (!context.found)
			continue;
		if (alternateReady && !retreat) {
			if (state.alternateAttackIssuedTargetID != target->getID()) {
				group->groupAttackObject(target, NO_MAX_SHOTS_LIMIT, CMD_FROM_AI);
				state.alternateAttackIssuedTargetID = target->getID();
				state.targetHealth = target->getBodyModule()
					? target->getBodyModule()->getHealth() : 0.0f;
				const Real targetDX = target->getPosition()->x -
					representative->getPosition()->x;
				const Real targetDY = target->getPosition()->y -
					representative->getPosition()->y;
				state.distanceToTargetSqr =
					targetDX * targetDX + targetDY * targetDY;
				state.lastProgressFrame = now;
				state.blockedSinceFrame = 0;
				state.regroupUntilFrame = now +
					12 * LOGICFRAMES_PER_SECOND;
				continue;
			}
			if (!stalled && !reengage)
				continue;
		}
		if (reengage && !retreat && !stalled) {
			if (target && m_strategyState.currentMode == SKIRMISH_STRATEGY_ASSAULT)
				group->groupAttackMoveToPosition(target->getPosition(),
					NO_MAX_SHOTS_LIMIT, CMD_FROM_AI);
			else if (m_strategyState.currentMode == SKIRMISH_STRATEGY_BALANCED)
				group->groupIdle(CMD_FROM_AI);
			continue;
		}
		if (stalled && !retreat) {
			Bool approachIssued = false;
			Bool pathProbeDeferred = false;
			if (target && !representative->isKindOf(KINDOF_AIRCRAFT) &&
				state.approachAttempt < 4) {
				const Real dx = representative->getPosition()->x - target->getPosition()->x;
				const Real dy = representative->getPosition()->y - target->getPosition()->y;
				const Real length = sqrt(dx * dx + dy * dy);
				const Real forwardX = length > 1.0f ? dx / length : 1.0f;
				const Real forwardY = length > 1.0f ? dy / length : 0.0f;
				Coord3D approaches[4];
				for (Int j = 0; j < 4; ++j)
					approaches[j] = *target->getPosition();
				approaches[0].x += forwardX * 250.0f;
				approaches[0].y += forwardY * 250.0f;
				approaches[1].x -= forwardY * 250.0f;
				approaches[1].y += forwardX * 250.0f;
				approaches[2].x += forwardY * 250.0f;
				approaches[2].y -= forwardX * 250.0f;
				approaches[3].x -= forwardX * 250.0f;
				approaches[3].y -= forwardY * 250.0f;
				for (Int j = state.approachAttempt; j < 4; ++j) {
					if (!TryConsumeSkirmishAITacticalPathQuery(
							&tacticalQuickPathQueryCount,
							MAX_SKIRMISH_AI_TACTICAL_QUICK_PATH_QUERIES_PER_UPDATE)) {
						pathProbeDeferred = true;
						break;
					}
					approaches[j].z = TheTerrainLogic->getGroundHeight(
						approaches[j].x, approaches[j].y);
					const Bool reachable =
						TheAI->pathfinder()->clientSafeQuickDoesPathExist(
							representative->getAIUpdateInterface()->getLocomotorSet(),
							representative->getPosition(), &approaches[j]);
					state.approachAttempt = j + 1;
					if (reachable) {
						group->groupAttackMoveToPosition(&approaches[j],
							NO_MAX_SHOTS_LIMIT, CMD_FROM_AI);
						state.lastProgressFrame = now;
						state.blockedSinceFrame = 0;
						state.regroupUntilFrame = now + 12 * LOGICFRAMES_PER_SECOND;
						approachIssued = true;
						break;
					}
				}
			}
			if (approachIssued)
				continue;
			Object *candidates[4] = { nullptr, nullptr, nullptr, nullptr };
			Bool moreCandidates = false;
			for (size_t candidateIndex = 0;
				candidateIndex < alternateTargets.size(); ++candidateIndex) {
				Object *candidate = alternateTargets[candidateIndex];
				if (candidate == target || candidate->getControllingPlayer() != m_currentEnemy ||
					(state.alternateProbeAfterID != INVALID_ID &&
					 candidate->getID() <= state.alternateProbeAfterID) ||
					(representative->getAbleToAttackSpecificObject(ATTACK_NEW_TARGET,
						candidate, CMD_FROM_AI) == ATTACKRESULT_NOT_POSSIBLE))
					continue;
				if (candidates[3] && candidate->getID() > candidates[3]->getID()) {
					moreCandidates = true;
					continue;
				}
				for (Int j = 0; j < 4; ++j) {
					if (!candidates[j] || candidate->getID() < candidates[j]->getID()) {
						if (candidates[3])
							moreCandidates = true;
						for (Int k = 3; k > j; --k)
							candidates[k] = candidates[k - 1];
						candidates[j] = candidate;
						break;
					}
				}
			}
			Object *alternate = nullptr;
			Coord3D alternateApproach;
			ObjectID lastAlternateProbeID = INVALID_ID;
			for (Int j = 0; j < 4 && candidates[j]; ++j) {
				for (Int candidate = 0; candidate < 2; ++candidate) {
					Coord3D approach;
					if (!GetSkirmishAIStrategyGroundApproach(
							representative->getPosition(), candidates[j],
							candidate, &approach))
						continue;
					if (!TryConsumeSkirmishAITacticalPathQuery(
							&tacticalQuickPathQueryCount,
								MAX_SKIRMISH_AI_TACTICAL_QUICK_PATH_QUERIES_PER_UPDATE)) {
						pathProbeDeferred = true;
						break;
					}
					if (TheAI->pathfinder()->clientSafeQuickDoesPathExist(
							representative->getAIUpdateInterface()->getLocomotorSet(),
							representative->getPosition(), &approach)) {
						alternate = candidates[j];
						alternateApproach = approach;
						break;
					}
				}
				if (pathProbeDeferred || alternate)
					break;
				lastAlternateProbeID = candidates[j]->getID();
			}
			if (lastAlternateProbeID != INVALID_ID)
				state.alternateProbeAfterID = lastAlternateProbeID;
			if (alternate) {
				group->groupAttackMoveToPosition(&alternateApproach,
					NO_MAX_SHOTS_LIMIT, CMD_FROM_AI);
				state.alternateAttackIssuedTargetID = INVALID_ID;
				state.targetID = alternate->getID();
				state.targetHealth = alternate->getBodyModule()
					? alternate->getBodyModule()->getHealth() : 0.0f;
				const Real alternateDX = alternate->getPosition()->x -
					representative->getPosition()->x;
				const Real alternateDY = alternate->getPosition()->y -
					representative->getPosition()->y;
				state.distanceToTargetSqr =
					alternateDX * alternateDX + alternateDY * alternateDY;
				state.approachAttempt = 0;
				state.alternateProbeAfterID = INVALID_ID;
				state.routeExhaustedTargetID = INVALID_ID;
				state.routeExhaustedUntilFrame = 0;
				state.lastProgressFrame = now;
				state.blockedSinceFrame = 0;
				state.regroupUntilFrame = now + 20 * LOGICFRAMES_PER_SECOND;
				continue;
			}
			Object *blockingDefense = nullptr;
			double blockingDistance = 0.0;
			if (target) {
				const double lineX = (double)target->getPosition()->x -
					representative->getPosition()->x;
				const double lineY = (double)target->getPosition()->y -
					representative->getPosition()->y;
				for (size_t candidateIndex = 0;
					candidateIndex < corridorDefenses.size(); ++candidateIndex) {
					Object *candidate = corridorDefenses[candidateIndex];
					if (!SkirmishAITunnelRoute::IsCorridorBlocker(
							representative->getPosition()->x,
							representative->getPosition()->y,
							target->getPosition()->x, target->getPosition()->y,
							candidate->getPosition()->x,
							candidate->getPosition()->y, 350.0f))
						continue;
					const double dx = (double)candidate->getPosition()->x -
						representative->getPosition()->x;
					const double dy = (double)candidate->getPosition()->y -
						representative->getPosition()->y;
					const double cross = dx * lineY - dy * lineX;
					const double distance = cross * cross;
					if (!blockingDefense || distance < blockingDistance ||
						(distance == blockingDistance && blockingDefense &&
						 candidate->getID() < blockingDefense->getID())) {
						blockingDefense = candidate;
						blockingDistance = distance;
					}
				}
			}
			Bool tunnelBypassHandled = false;
			if (blockingDefense)
				tunnelBypassHandled = tryTunnelBypass(team, target,
					blockingDefense, groupObject, state, representative,
					hasAircraft, memberCount, now,
					&aggregateTunnelPathQueryCount);
			if (state.tunnelTransitPhase != SKIRMISH_AI_TUNNEL_TRANSIT_NONE)
				continue;
			if (blockingDefense && (hasAircraft || hasArtillery)) {
				AIGroupPtr strike = TheAI->createGroup();
				if (strike) {
					Bool hasStrike = false;
					for (DLINK_ITERATOR<Object> member = team->iterate_TeamMemberList();
						!member.done(); member.advance()) {
						Object *object = member.cur();
						const Bool aircraftExposed = object &&
							object->isKindOf(KINDOF_AIRCRAFT) &&
							blockingDefense->getAbleToAttackSpecificObject(
								ATTACK_NEW_TARGET, object, CMD_FROM_AI) != ATTACKRESULT_NOT_POSSIBLE;
						if (IsSkirmishStrategyPotentialOffensiveRecipient(object, m_player, team) &&
							(object->isKindOf(KINDOF_AIRCRAFT) ||
							 object->getLargestWeaponRange() >= 250.0f) &&
							!aircraftExposed &&
							object->getAbleToAttackSpecificObject(ATTACK_NEW_TARGET,
								blockingDefense, CMD_FROM_AI) != ATTACKRESULT_NOT_POSSIBLE) {
							strike->add(object);
							hasStrike = true;
						}
					}
					if (hasStrike) {
						strike->groupAttackObject(blockingDefense,
							NO_MAX_SHOTS_LIMIT, CMD_FROM_AI);
						state.lastProgressFrame = now;
						state.blockedSinceFrame = 0;
						state.regroupUntilFrame = now + 20 * LOGICFRAMES_PER_SECOND;
						continue;
					}
				}
			}
			if (tunnelBypassHandled)
				continue;
			// A deferred path probe is still eligible on the next scan. The
			// defense bypass and strike opportunities above have already run.
			if (pathProbeDeferred)
				continue;
			if (!moreCandidates && target &&
				target->getID() == m_strategyState.strategicTargetID) {
				state.routeExhaustedTargetID = target->getID();
				state.routeExhaustedUntilFrame = now +
					60 * LOGICFRAMES_PER_SECOND;
				state.alternateProbeAfterID = INVALID_ID;
			}
		}
		std::vector<Object *> retreatObjects[3];
		for (size_t facilityIndex = 0;
			facilityIndex < retreatFacilities.size(); ++facilityIndex) {
			Object *friendly = retreatFacilities[facilityIndex];
			Int kind = -1;
			if ((representative->isKindOf(KINDOF_VEHICLE) &&
				friendly->isKindOf(KINDOF_REPAIR_PAD)) ||
				(representative->isKindOf(KINDOF_INFANTRY) &&
				 friendly->isKindOf(KINDOF_HEAL_PAD)) ||
				(representative->isKindOf(KINDOF_AIRCRAFT) &&
				 friendly->isKindOf(KINDOF_FS_AIRFIELD)))
				kind = 0;
			else if (friendly->isKindOf(KINDOF_FS_BASE_DEFENSE))
				kind = 1;
			else if (friendly->isKindOf(KINDOF_FS_BARRACKS) ||
				friendly->isKindOf(KINDOF_FS_WARFACTORY) ||
				friendly->isKindOf(KINDOF_FS_AIRFIELD))
				kind = 2;
			if (kind >= 0)
				retreatObjects[kind].push_back(friendly);
		}
		UnsignedInt facilityCount = 0;
		for (Int kind = 0; kind < 3; ++kind) {
			std::sort(retreatObjects[kind].begin(), retreatObjects[kind].end(),
				IsSkirmishAIProducerIDBefore);
			facilityCount += (UnsignedInt)retreatObjects[kind].size();
		}
		AIUpdateInterface *ai = representative->getAIUpdateInterface();
		Bool foundRetreat = false;
		Int teamRetreatPathQueryCount = 0;
		Bool retreatProbeDeferred = false;
		// Resume after the last deferred probe so a large set of blocked
		// facilities cannot hide a later reachable one behind the team budget.
		UnsignedInt start = state.retreatProbeCursor <= facilityCount
			? state.retreatProbeCursor : 0;
		for (UnsignedInt index = start;
			index < facilityCount && !foundRetreat; ++index) {
			UnsignedInt site = index;
			Int kind = 0;
			while (kind < 2 && site >= retreatObjects[kind].size()) {
				site -= (UnsignedInt)retreatObjects[kind].size();
				++kind;
			}
			Coord3D destination =
				*retreatObjects[kind][site]->getPosition();
			Real dx = representative->getPosition()->x - destination.x;
			Real dy = representative->getPosition()->y - destination.y;
			Real length = sqrt(dx * dx + dy * dy);
			if (length > 1.0f) {
				destination.x += dx * 100.0f / length;
				destination.y += dy * 100.0f / length;
			}
			destination.z = TheTerrainLogic->getGroundHeight(
				destination.x, destination.y);
			if (!IsSkirmishTacticalRetreatPointSafe(
					representative->getPosition(), &destination,
					retreatHazards) || !ai)
				continue;
			if (teamRetreatPathQueryCount >=
					MAX_SKIRMISH_AI_TACTICAL_RETREAT_PATH_QUERIES_PER_TEAM ||
				retreatPathQueryCount >=
					MAX_SKIRMISH_AI_TACTICAL_RETREAT_PATH_QUERIES_PER_UPDATE) {
				state.retreatProbeCursor = index;
				retreatProbeDeferred = true;
				break;
			}
			++retreatPathQueryCount;
			++teamRetreatPathQueryCount;
			if (!TheAI->pathfinder()->clientSafeQuickDoesPathExist(
						ai->getLocomotorSet(), representative->getPosition(),
						&destination))
				continue;
			group->groupGuardPosition(&destination,
				GUARDMODE_GUARD_WITHOUT_PURSUIT, CMD_FROM_AI);
			foundRetreat = true;
		}
		if (retreatProbeDeferred)
			continue;
		// The base center is tried only after the facility sweep. A separate
		// cursor value preserves that pending attempt when the shared budget ends.
		if (!foundRetreat && ai &&
			IsSkirmishTacticalRetreatPointSafe(
				representative->getPosition(), &m_baseCenter, retreatHazards)) {
			if (teamRetreatPathQueryCount >=
					MAX_SKIRMISH_AI_TACTICAL_RETREAT_PATH_QUERIES_PER_TEAM ||
				retreatPathQueryCount >=
					MAX_SKIRMISH_AI_TACTICAL_RETREAT_PATH_QUERIES_PER_UPDATE) {
				state.retreatProbeCursor = facilityCount;
				continue;
			}
			++retreatPathQueryCount;
			++teamRetreatPathQueryCount;
			if (TheAI->pathfinder()->clientSafeQuickDoesPathExist(
						ai->getLocomotorSet(), representative->getPosition(),
						&m_baseCenter)) {
				group->groupGuardPosition(&m_baseCenter,
					GUARDMODE_GUARD_WITHOUT_PURSUIT, CMD_FROM_AI);
				foundRetreat = true;
			}
		}
		state.retreatProbeCursor = 0;
		if (!foundRetreat)
			group->groupGuardPosition(representative->getPosition(),
				GUARDMODE_GUARD_WITHOUT_PURSUIT, CMD_FROM_AI);
		state.retreating = true;
		state.retreatStartFrame = now;
		// A failed objective waits for a full strategy window before another
		// assault attempt. Health/counter retreats can regroup sooner.
		state.regroupUntilFrame = now +
			(stalled && !retreat ? 45 : 8) * LOGICFRAMES_PER_SECOND;
		state.lastProgressFrame = now;
		state.blockedSinceFrame = 0;
	}
}

Bool AISkirmishPlayer::tryTunnelBypass(
	Team *team, Object *target, Object *blockingDefense, AIGroup *group,
	TacticalTeamState &state, Object *representative,
	Bool hasAircraft, Int memberCount, UnsignedInt now,
	Int *aggregatePathQueryCount)
{
	Int tunnelAttemptPathQueryCount = 0;
	if (!team || !target || !blockingDefense || !group || !representative ||
		!aggregatePathQueryCount ||
		!m_player || !TheGameLogic || !TheAI || !TheAI->pathfinder() ||
		!IsSkirmishAISupportedGLASide(m_player->getSide()) || hasAircraft ||
		memberCount <= 0 || memberCount > MAX_SKIRMISH_AI_TUNNEL_MEMBERS ||
		state.tunnelTransitPhase != SKIRMISH_AI_TUNNEL_TRANSIT_NONE ||
		m_strategyState.currentMode != SKIRMISH_STRATEGY_ASSAULT ||
		!IsSkirmishAIStrategyTunnelTargetUsable(target, m_player, m_currentEnemy) ||
		blockingDefense->getControllingPlayer() != m_currentEnemy ||
		!blockingDefense->isKindOf(KINDOF_FS_BASE_DEFENSE) ||
		blockingDefense->isEffectivelyDead() || blockingDefense->isDestroyed() ||
		blockingDefense->testStatus(OBJECT_STATUS_SOLD) ||
		!IsSkirmishStrategyIntelEligible(blockingDefense, m_player))
		return false;
	// A cooldown belongs to the team, not the target that initiated it. If
	// strategy switches targets during the wait, defer that blocked target too;
	// otherwise the caller would mark its untried bypass as exhausted.
	if (!IsSkirmishStrategyFrameReached(now, state.tunnelCooldownUntilFrame))
		return true;
	state.tunnelWaitTargetID = INVALID_ID;

	// Only one assault team may reserve the shared tunnel capacity at a time.
	// This also makes simultaneous attempts deterministic in team-ID order.
	std::map<UnsignedInt, TacticalTeamState>::const_iterator otherState;
	for (otherState = m_tacticalTeams.begin(); otherState != m_tacticalTeams.end();
		++otherState) {
		if (otherState->first != team->getID() &&
			otherState->second.tunnelTransitPhase !=
				SKIRMISH_AI_TUNNEL_TRANSIT_NONE) {
			state.tunnelCooldownUntilFrame = now +
				SKIRMISH_AI_TUNNEL_RETRY_COOLDOWN_SECONDS * LOGICFRAMES_PER_SECOND;
			state.tunnelWaitTargetID = target->getID();
			return true;
		}
	}

	std::vector<Object *> members;
	for (DLINK_ITERATOR<Object> member = team->iterate_TeamMemberList();
		!member.done(); member.advance()) {
		Object *object = member.cur();
		if (!IsSkirmishAIStrategyTunnelTransitMember(
				object, m_player, team, false))
			continue;
		if (object->isKindOf(KINDOF_AIRCRAFT))
			return false;
		members.push_back(object);
	}
	std::sort(members.begin(), members.end(),
		IsSkirmishAIStrategyObjectIDBefore);
	if (members.empty() || (Int)members.size() != memberCount)
		return false;

	TunnelTracker *tracker = m_player->getTunnelSystem();
	if (!tracker)
		return false;
	const Int tunnelCapacity = tracker->getContainMax();
	if (tunnelCapacity <= 0 || members.size() > (UnsignedInt)tunnelCapacity)
		return false;
	if (tracker->getContainCount() + members.size() >
		(UnsignedInt)tunnelCapacity) {
		if (SkirmishAITunnelRoute::DeferFullTunnelCapacity(
				target->getID(), now,
				SKIRMISH_AI_TUNNEL_CAPACITY_WAIT_SECONDS *
					LOGICFRAMES_PER_SECOND,
				&state.tunnelCapacityWaitTargetID,
				&state.tunnelCapacityWaitDeadlineFrame)) {
			state.tunnelCooldownUntilFrame = now +
				SKIRMISH_AI_TUNNEL_PAIR_PROBE_SECONDS *
					LOGICFRAMES_PER_SECOND;
			state.tunnelWaitTargetID = target->getID();
			return true;
		}
		return false;
	}
	state.tunnelCapacityWaitTargetID = INVALID_ID;
	state.tunnelCapacityWaitDeadlineFrame = 0;
	for (size_t memberIndex = 0; memberIndex < members.size(); ++memberIndex) {
		if (!tracker->isValidContainerFor(members[memberIndex], false))
			return false;
	}

	std::vector<Object *> tunnelObjects;
	const std::list<ObjectID> *registeredIDs = tracker->getContainerList();
	if (!registeredIDs)
		return false;
	std::list<ObjectID>::const_iterator tunnelID;
	for (tunnelID = registeredIDs->begin(); tunnelID != registeredIDs->end();
		tunnelID++) {
		Object *object = TheGameLogic->findObjectByID(*tunnelID);
		if (IsSkirmishAIStrategyTunnelEndpointLive(object, m_player))
			tunnelObjects.push_back(object);
	}
	std::sort(tunnelObjects.begin(), tunnelObjects.end(),
		IsSkirmishAIStrategyObjectIDBefore);
	for (Int slot = 0;
		slot < SkirmishAITunnelRoute::MAX_GENERATED_FORWARD_ENDPOINTS;
		++slot) {
		const ObjectID generatedID = m_tunnelGeneratedForwardEndpointIDs[slot];
		Object *generated = generatedID != INVALID_ID
			? TheGameLogic->findObjectByID(generatedID) : nullptr;
		if (!generated || generated->getControllingPlayer() != m_player ||
			generated->isEffectivelyDead() || generated->isDestroyed() ||
			generated->testStatus(OBJECT_STATUS_SOLD)) {
			m_tunnelGeneratedForwardEndpointIDs[slot] = INVALID_ID;
			m_tunnelGeneratedForwardTargetIDs[slot] = INVALID_ID;
		}
	}
	Object *homeEndpoint = m_tunnelHomeEndpointID != INVALID_ID ?
		TheGameLogic->findObjectByID(m_tunnelHomeEndpointID) : nullptr;
	if (SkirmishAITunnelRoute::ShouldReplaceLostEndpoint(
			m_tunnelHomeAttempted, m_tunnelHomeEndpointID,
			IsSkirmishAIStrategyTunnelEndpointLive(homeEndpoint, m_player))) {
		m_tunnelHomeAttempted = FALSE;
		m_tunnelHomeEndpointID = INVALID_ID;
		m_tunnelBuildCooldownUntilFrame = now +
			SKIRMISH_AI_TUNNEL_RETRY_COOLDOWN_SECONDS * LOGICFRAMES_PER_SECOND;
	}
	Object *forwardEndpoint = m_tunnelForwardEndpointID != INVALID_ID ?
		TheGameLogic->findObjectByID(m_tunnelForwardEndpointID) : nullptr;
	if (SkirmishAITunnelRoute::ShouldReplaceLostEndpoint(
			m_tunnelForwardAttempted, m_tunnelForwardEndpointID,
			IsSkirmishAIStrategyTunnelEndpointLive(forwardEndpoint, m_player))) {
		m_tunnelForwardAttempted = FALSE;
		m_tunnelForwardEndpointID = INVALID_ID;
		m_tunnelForwardAttemptTargetID = INVALID_ID;
		m_tunnelBuildCooldownUntilFrame = now +
			SKIRMISH_AI_TUNNEL_RETRY_COOLDOWN_SECONDS * LOGICFRAMES_PER_SECOND;
	}
	if (m_tunnelBuildPhase == SKIRMISH_AI_TUNNEL_BUILD_HOME_CONSTRUCTING ||
		m_tunnelBuildPhase == SKIRMISH_AI_TUNNEL_BUILD_FORWARD_CONSTRUCTING) {
		Bool endpointRegistered = false;
		for (size_t i = 0; i < tunnelObjects.size(); ++i) {
			if (tunnelObjects[i]->getID() == m_tunnelBuildObjectID) {
				endpointRegistered = true;
				break;
			}
		}
		if (endpointRegistered) {
			if (m_tunnelBuildPhase ==
					SKIRMISH_AI_TUNNEL_BUILD_HOME_CONSTRUCTING) {
				m_tunnelHomeAttempted = TRUE;
				m_tunnelHomeEndpointID = m_tunnelBuildObjectID;
			} else {
				m_tunnelForwardAttempted = TRUE;
				m_tunnelForwardEndpointID = m_tunnelBuildObjectID;
				SkirmishAITunnelRoute::RecordGeneratedForwardEndpoint(
					m_tunnelGeneratedForwardEndpointIDs,
					m_tunnelGeneratedForwardTargetIDs,
					m_tunnelBuildObjectID, m_tunnelBuildTargetID);
			}
			m_tunnelBuildPhase = SKIRMISH_AI_TUNNEL_BUILD_NONE;
			m_tunnelBuildBuilderID = INVALID_ID;
			m_tunnelBuildTargetID = INVALID_ID;
			m_tunnelBuildBlockerID = INVALID_ID;
			m_tunnelBuildObjectID = INVALID_ID;
			m_tunnelBuildDeadlineFrame = 0;
			m_tunnelBuildLocation.zero();
			m_tunnelPendingBuilderCursor = 0;
		} else {
			Object *construction = m_tunnelBuildObjectID != INVALID_ID
				? TheGameLogic->findObjectByID(m_tunnelBuildObjectID) : nullptr;
			const Bool constructionAlive = construction &&
				!construction->isEffectivelyDead() && !construction->isDestroyed();
			if (!constructionAlive)
				abandonTunnelBuildPlan(now, TRUE);
			else if (IsSkirmishStrategyFrameReached(
					now, m_tunnelBuildDeadlineFrame)) {
				const Bool homeScaffold = m_tunnelBuildPhase ==
					SKIRMISH_AI_TUNNEL_BUILD_HOME_CONSTRUCTING;
				const ObjectID scaffoldID = m_tunnelBuildObjectID;
				const ObjectID scaffoldTargetID = m_tunnelBuildTargetID;
				abandonTunnelBuildPlan(now, TRUE);
				m_tunnelBuildPhase = homeScaffold ?
					SKIRMISH_AI_TUNNEL_BUILD_HOME_RETRY_AVAILABLE :
					SKIRMISH_AI_TUNNEL_BUILD_FORWARD_RETRY_AVAILABLE;
				m_tunnelBuildObjectID = scaffoldID;
				m_tunnelBuildTargetID = scaffoldTargetID;
				m_tunnelBuildDeadlineFrame = now +
					120 * LOGICFRAMES_PER_SECOND;
				return scaffoldTargetID == target->getID();
			}
			else if (m_tunnelBuildTargetID == target->getID())
				return true;
		}
	}
	if ((m_tunnelBuildPhase == SKIRMISH_AI_TUNNEL_BUILD_HOME_RETRY_AVAILABLE ||
		 m_tunnelBuildPhase == SKIRMISH_AI_TUNNEL_BUILD_FORWARD_RETRY_AVAILABLE) &&
		m_tunnelBuildObjectID != INVALID_ID) {
		Object *scaffold = TheGameLogic->findObjectByID(m_tunnelBuildObjectID);
		Bool scaffoldRegistered = false;
		for (size_t i = 0; i < tunnelObjects.size(); ++i)
			if (tunnelObjects[i]->getID() == m_tunnelBuildObjectID) {
				scaffoldRegistered = true;
				break;
			}
		if (scaffoldRegistered) {
			if (m_tunnelBuildPhase == SKIRMISH_AI_TUNNEL_BUILD_HOME_RETRY_AVAILABLE) {
				m_tunnelHomeAttempted = TRUE;
				m_tunnelHomeEndpointID = m_tunnelBuildObjectID;
			} else {
				m_tunnelForwardAttempted = TRUE;
				m_tunnelForwardEndpointID = m_tunnelBuildObjectID;
				SkirmishAITunnelRoute::RecordGeneratedForwardEndpoint(
					m_tunnelGeneratedForwardEndpointIDs,
					m_tunnelGeneratedForwardTargetIDs,
					m_tunnelBuildObjectID, m_tunnelBuildTargetID);
			}
			m_tunnelBuildPhase = SKIRMISH_AI_TUNNEL_BUILD_NONE;
			m_tunnelBuildBuilderID = INVALID_ID;
			m_tunnelBuildTargetID = INVALID_ID;
			m_tunnelBuildBlockerID = INVALID_ID;
			m_tunnelBuildObjectID = INVALID_ID;
			m_tunnelBuildDeadlineFrame = 0;
			m_tunnelBuildLocation.zero();
			m_tunnelPendingBuilderCursor = 0;
		} else if (scaffold && !scaffold->isEffectivelyDead() &&
			!scaffold->isDestroyed()) {
			if (!IsSkirmishStrategyFrameReached(
					now, m_tunnelBuildDeadlineFrame))
				return m_tunnelBuildTargetID == target->getID();
			// This owned scaffold has exhausted its registration grace. Carry
			// the deadline into the generic unregistered-scaffold guard so it
			// cannot start another wait for the same object.
			state.tunnelScaffoldWaitObjectID = m_tunnelBuildObjectID;
			state.tunnelScaffoldWaitUntilFrame = m_tunnelBuildDeadlineFrame;
		} else {
			m_tunnelBuildObjectID = INVALID_ID;
			m_tunnelBuildDeadlineFrame = 0;
		}
	}
	if (m_tunnelBuildPhase ==
			SKIRMISH_AI_TUNNEL_BUILD_HOME_RETRY_AVAILABLE &&
		SkirmishAITunnelRoute::ShouldConsumeHomeRetry(
			(UnsignedInt)tunnelObjects.size())) {
		m_tunnelBuildPhase = SKIRMISH_AI_TUNNEL_BUILD_NONE;
		m_tunnelHomeAttempted = TRUE;
		m_tunnelBuildTargetID = INVALID_ID;
		m_tunnelBuildBlockerID = INVALID_ID;
		m_tunnelBuildDeadlineFrame = 0;
	}
	if (m_tunnelBuildPhase ==
			SKIRMISH_AI_TUNNEL_BUILD_FORWARD_RETRY_AVAILABLE &&
		tunnelObjects.empty()) {
		m_tunnelBuildPhase = SKIRMISH_AI_TUNNEL_BUILD_NONE;
		m_tunnelForwardAttempted = TRUE;
		m_tunnelBuildTargetID = INVALID_ID;
		m_tunnelBuildBlockerID = INVALID_ID;
		m_tunnelBuildDeadlineFrame = 0;
	}
	if (tunnelObjects.size() < 2) {
		Bool queuedTunnel = false;
		Bool deferredBuild = false;
		const Bool needsHomeReplacement =
			m_tunnelHomeEndpointID == INVALID_ID &&
			tunnelObjects.size() == 1 &&
			SkirmishAITunnelRoute::IsTrackedGeneratedForwardEndpoint(
				tunnelObjects[0]->getID(),
				m_tunnelGeneratedForwardEndpointIDs);
		if ((tunnelObjects.empty() || needsHomeReplacement) &&
			(m_tunnelBuildPhase == SKIRMISH_AI_TUNNEL_BUILD_NONE ||
			 m_tunnelBuildPhase ==
				SKIRMISH_AI_TUNNEL_BUILD_HOME_RETRY_AVAILABLE))
			queuedTunnel = tryQueueTunnelEndpoint(
				team, target, blockingDefense, TRUE, now,
				&tunnelAttemptPathQueryCount, aggregatePathQueryCount,
				state, &deferredBuild);
		else if (!tunnelObjects.empty() &&
			(m_tunnelBuildPhase == SKIRMISH_AI_TUNNEL_BUILD_NONE ||
			 m_tunnelBuildPhase ==
				SKIRMISH_AI_TUNNEL_BUILD_FORWARD_RETRY_AVAILABLE))
			queuedTunnel = tryQueueTunnelEndpoint(
				team, target, blockingDefense, FALSE, now,
				&tunnelAttemptPathQueryCount, aggregatePathQueryCount,
				state, &deferredBuild);
		const Bool pendingBuildForTarget =
			m_tunnelBuildTargetID == target->getID() &&
			(m_tunnelBuildPhase == SKIRMISH_AI_TUNNEL_BUILD_HOME_QUEUED ||
			 m_tunnelBuildPhase == SKIRMISH_AI_TUNNEL_BUILD_FORWARD_QUEUED ||
			 m_tunnelBuildPhase == SKIRMISH_AI_TUNNEL_BUILD_HOME_CONSTRUCTING ||
			 m_tunnelBuildPhase == SKIRMISH_AI_TUNNEL_BUILD_FORWARD_CONSTRUCTING);
		const Bool retryCooldownPending =
			((m_tunnelBuildPhase ==
				SKIRMISH_AI_TUNNEL_BUILD_HOME_RETRY_AVAILABLE &&
				m_tunnelBuildTargetID == target->getID()) ||
			 (m_tunnelBuildPhase ==
				SKIRMISH_AI_TUNNEL_BUILD_FORWARD_RETRY_AVAILABLE &&
				m_tunnelBuildTargetID == target->getID())) &&
			!IsSkirmishStrategyFrameReached(
				now, m_tunnelBuildCooldownUntilFrame);
		return queuedTunnel || deferredBuild || pendingBuildForTarget || retryCooldownPending;
	}

	if (tunnelObjects.size() > MAX_SKIRMISH_AI_TUNNEL_ENDPOINT_PROBES) {
		// A scripted network can exceed the saveable cache bound. Keep ground
		// tactics available without creating state that cannot be serialized.
		state.tunnelEndpointProbes.clear();
		state.tunnelPairSweepTargetID = INVALID_ID;
		return false;
	}

	// Cache team reachability by endpoint for this bounded sweep. Unit motion
	// does not restart the cursor; the selected route is probed again live.
	UnsignedInt endpointSignature = (UnsignedInt)tunnelObjects.size();
	for (size_t i = 0; i < tunnelObjects.size(); ++i)
		endpointSignature = endpointSignature * 33u +
			(UnsignedInt)tunnelObjects[i]->getID();
	// A failed approach belongs to the unit positions where it was tested.
	// Quantize motion so tiny movement does not restart a large pair sweep.
	UnsignedInt movementSignature = (UnsignedInt)members.size();
	for (size_t i = 0; i < members.size(); ++i) {
		const Coord3D *position = members[i]->getPosition();
		movementSignature = movementSignature * 33u +
			(UnsignedInt)(Int)(position->x / 80.0f);
		movementSignature = movementSignature * 33u +
			(UnsignedInt)(Int)(position->y / 80.0f);
	}
	const UnsignedInt pairCount = (UnsignedInt)(
		tunnelObjects.size() * (tunnelObjects.size() - 1) / 2);
	Bool cacheIdentityChanged =
		state.tunnelPairSweepTargetID != target->getID() ||
		state.tunnelPairEndpointSignature != endpointSignature ||
		state.tunnelEndpointProbes.size() != tunnelObjects.size() ||
		state.tunnelProbeMemberCount != (Int)members.size();
	if (!cacheIdentityChanged) {
		for (size_t i = 0; i < tunnelObjects.size(); ++i)
			if (state.tunnelEndpointProbes[i].objectID !=
				tunnelObjects[i]->getID()) {
				cacheIdentityChanged = TRUE;
				break;
			}
		for (size_t i = 0; i < members.size(); ++i)
			if (state.tunnelProbeMemberIDs[i] != members[i]->getID()) {
				cacheIdentityChanged = TRUE;
				break;
			}
	}
	if (cacheIdentityChanged) {
		state.tunnelPairSweepTargetID = target->getID();
		state.tunnelPairEndpointSignature = endpointSignature;
		state.tunnelProbeMovementSignature = movementSignature;
		state.tunnelPairSweepRemaining = pairCount;
		state.tunnelPairSweepStartFrame = now;
		state.tunnelPairResumeAfterFrame = 0;
		state.tunnelPairRetryAfterFrame = 0;
		state.tunnelPairRefreshCursor = state.tunnelPairCursor;
		state.tunnelPairRefreshRemaining = pairCount;
		state.tunnelPairRefreshAfterFrame = now +
			15 * LOGICFRAMES_PER_SECOND;
		state.tunnelPairRefreshCacheAfterFrame =
			state.tunnelPairRefreshAfterFrame;
		state.tunnelPairRefreshYieldMain = FALSE;
		state.tunnelProbeMemberCount = (Int)members.size();
		for (size_t i = 0; i < members.size(); ++i)
			state.tunnelProbeMemberIDs[i] = members[i]->getID();
		state.tunnelEndpointProbes.clear();
		for (size_t i = 0; i < tunnelObjects.size(); ++i) {
			TacticalTeamState::TunnelEndpointProbe probe;
			probe.objectID = tunnelObjects[i]->getID();
			state.tunnelEndpointProbes.push_back(probe);
		}
	}
	if (!cacheIdentityChanged &&
		state.tunnelProbeMovementSignature != movementSignature)
		state.tunnelPairRefreshAfterFrame = now;
	if (state.tunnelPairSweepRemaining > pairCount)
		state.tunnelPairSweepRemaining = pairCount;
	if (state.tunnelPairRefreshRemaining > pairCount)
		state.tunnelPairRefreshRemaining = pairCount;
	if (SkirmishAITunnelRoute::ShouldRestartEndpointSweep(
			state.tunnelPairSweepRemaining, now,
			state.tunnelPairRetryAfterFrame)) {
		state.tunnelPairSweepRemaining = pairCount;
		state.tunnelPairSweepStartFrame = now;
		state.tunnelPairResumeAfterFrame = 0;
		state.tunnelPairRetryAfterFrame = 0;
		for (size_t i = 0; i < state.tunnelEndpointProbes.size(); ++i) {
			state.tunnelEndpointProbes[i].entryResult = 0;
			state.tunnelEndpointProbes[i].exitResult = 0;
		}
	}
	if (state.tunnelPairSweepRemaining &&
		state.tunnelPairResumeAfterFrame &&
		!IsSkirmishStrategyFrameReached(
			now, state.tunnelPairResumeAfterFrame))
		return false;
	if (state.tunnelPairResumeAfterFrame) {
		state.tunnelPairResumeAfterFrame = 0;
		state.tunnelPairSweepStartFrame = now;
	}
	if (state.tunnelPairSweepRemaining &&
		SkirmishAITunnelRoute::ProbeEpochExpired(now,
			state.tunnelPairSweepStartFrame,
			60 * LOGICFRAMES_PER_SECOND)) {
		// Let normal tactical recovery run while retaining the saved pair cursor.
		state.tunnelPairResumeAfterFrame = now +
			45 * LOGICFRAMES_PER_SECOND;
		for (size_t i = 0; i < state.tunnelEndpointProbes.size(); ++i) {
			state.tunnelEndpointProbes[i].entryResult = 0;
			state.tunnelEndpointProbes[i].exitResult = 0;
		}
		return false;
	}
	// Revisit earlier pairs on a separate cursor. This notices changed paths
	// without restarting or starving the main finite sweep.
	Bool refreshTurn = FALSE;
	if (state.tunnelPairRefreshYieldMain)
		state.tunnelPairRefreshYieldMain = FALSE;
	else if (IsSkirmishStrategyFrameReached(
			now, state.tunnelPairRefreshAfterFrame)) {
		refreshTurn = TRUE;
		state.tunnelPairRefreshYieldMain = TRUE;
		state.tunnelPairRefreshAfterFrame = now +
			4 * LOGICFRAMES_PER_SECOND;
		if (!state.tunnelPairRefreshRemaining)
			state.tunnelPairRefreshRemaining = pairCount;
		if (state.tunnelProbeMovementSignature != movementSignature ||
			IsSkirmishStrategyFrameReached(
				now, state.tunnelPairRefreshCacheAfterFrame)) {
			state.tunnelProbeMovementSignature = movementSignature;
			state.tunnelPairRefreshCacheAfterFrame = now +
				15 * LOGICFRAMES_PER_SECOND;
			for (size_t i = 0; i < state.tunnelEndpointProbes.size(); ++i) {
				if (state.tunnelEndpointProbes[i].entryResult < 0)
					state.tunnelEndpointProbes[i].entryResult = 0;
				if (state.tunnelEndpointProbes[i].exitResult < 0)
					state.tunnelEndpointProbes[i].exitResult = 0;
			}
		}
	}
	// Geometry is cheap; keep a CPU cap while endpoint path results are reused.
	for (Int pairProbe = 0; pairProbe < 256; ++pairProbe) {
	const Bool refreshPair = refreshTurn && pairProbe < 128 &&
		state.tunnelPairRefreshRemaining > 0;
	if (!refreshPair && state.tunnelPairSweepRemaining == 0)
		break;
	UnsignedInt *pairCursor = refreshPair
		? &state.tunnelPairRefreshCursor : &state.tunnelPairCursor;
	UnsignedInt *pairRemaining = refreshPair
		? &state.tunnelPairRefreshRemaining : &state.tunnelPairSweepRemaining;
	Int firstIndex = 0;
	Int secondIndex = 0;
	if (!SkirmishAITunnelRoute::SelectPairIndices(
			(Int)tunnelObjects.size(), *pairCursor,
			&firstIndex, &secondIndex))
		return false;
	SkirmishAITunnelRoute::Endpoint pairEndpoints[2];
	for (Int endpointIndex = 0; endpointIndex < 2; ++endpointIndex) {
		Object *object = endpointIndex == 0 ?
			tunnelObjects[firstIndex] : tunnelObjects[secondIndex];
		pairEndpoints[endpointIndex].objectID = object->getID();
		pairEndpoints[endpointIndex].x = object->getPosition()->x;
		pairEndpoints[endpointIndex].y = object->getPosition()->y;
		pairEndpoints[endpointIndex].usable = TRUE;
		pairEndpoints[endpointIndex].groundApproachReachable = FALSE;
		pairEndpoints[endpointIndex].groundExitReachable = FALSE;
	}
	Int entryIndex = 0;
	Int exitIndex = 0;
	if (!SkirmishAITunnelRoute::SelectDirectedPair(pairEndpoints,
			target->getPosition()->x, target->getPosition()->y, 10000.0,
			&entryIndex, &exitIndex)) {
		++*pairCursor;
		--*pairRemaining;
		continue;
	}
	const Int entryObjectIndex = entryIndex == 0 ? firstIndex : secondIndex;
	const Int exitObjectIndex = exitIndex == 0 ? firstIndex : secondIndex;
	Object *entryObject = tunnelObjects[entryObjectIndex];
	Object *exitObject = tunnelObjects[exitObjectIndex];
	TacticalTeamState::TunnelEndpointProbe &entryProbe =
		state.tunnelEndpointProbes[entryObjectIndex];
	TacticalTeamState::TunnelEndpointProbe &exitProbe =
		state.tunnelEndpointProbes[exitObjectIndex];
	const Int sideAllowance = SkirmishAITunnelRoute::
		EndpointSideQueryAllowance((Int)members.size());
	if (entryProbe.entryResult == 0) {
		if (!SkirmishAITunnelRoute::CanReservePairQueries(
				sideAllowance, tunnelAttemptPathQueryCount,
				*aggregatePathQueryCount,
				MAX_SKIRMISH_AI_TUNNEL_PATH_QUERIES_PER_ATTEMPT,
				MAX_SKIRMISH_AI_TUNNEL_PATH_QUERIES_PER_UPDATE)) {
			state.tunnelCooldownUntilFrame = now +
				SKIRMISH_AI_TUNNEL_PAIR_PROBE_SECONDS * LOGICFRAMES_PER_SECOND;
			return true;
		}
		entryProbe.entryResult = ProbeSkirmishAITunnelEndpointRole(
			members, entryObject, target, TRUE,
			&tunnelAttemptPathQueryCount, aggregatePathQueryCount);
	}
	if (entryProbe.entryResult < 0) {
		++*pairCursor;
		--*pairRemaining;
		continue;
	}
	if (exitProbe.exitResult == 0) {
		if (!SkirmishAITunnelRoute::CanReservePairQueries(
				sideAllowance, tunnelAttemptPathQueryCount,
				*aggregatePathQueryCount,
				MAX_SKIRMISH_AI_TUNNEL_PATH_QUERIES_PER_ATTEMPT,
				MAX_SKIRMISH_AI_TUNNEL_PATH_QUERIES_PER_UPDATE)) {
			state.tunnelCooldownUntilFrame = now +
				SKIRMISH_AI_TUNNEL_PAIR_PROBE_SECONDS * LOGICFRAMES_PER_SECOND;
			return true;
		}
		exitProbe.exitResult = ProbeSkirmishAITunnelEndpointRole(
			members, exitObject, target, FALSE,
			&tunnelAttemptPathQueryCount, aggregatePathQueryCount);
	}
	if (exitProbe.exitResult < 0) {
		++*pairCursor;
		--*pairRemaining;
		continue;
	}
	pairEndpoints[entryIndex].groundApproachReachable = TRUE;
	pairEndpoints[exitIndex].groundExitReachable = TRUE;

	SkirmishAITunnelRoute::Plan plan;
	if (!SkirmishAITunnelRoute::SelectAssaultPlan(
			TRUE, TRUE, FALSE, TRUE, TRUE,
			representative->getPosition()->x,
			representative->getPosition()->y,
			target->getPosition()->x, target->getPosition()->y,
			10000.0, pairEndpoints, 2, &plan))
	{
		++*pairCursor;
		--*pairRemaining;
		continue;
	}
	Object *entry = TheGameLogic->findObjectByID(plan.entryTunnelID);
	Object *exit = TheGameLogic->findObjectByID(plan.exitTunnelID);
	if (!IsSkirmishAIStrategyTunnelEndpointRegistered(entry, m_player, tracker) ||
		!IsSkirmishAIStrategyTunnelEndpointRegistered(exit, m_player, tracker) ||
		tracker->getContainCount() + members.size() > (UnsignedInt)tunnelCapacity) {
		++*pairCursor;
		--*pairRemaining;
		continue;
	}
	// Cached results are only a search hint. Validate every current member
	// immediately before the group enters, without losing this pair on budget.
	if (!SkirmishAITunnelRoute::CanReservePairQueries(
			SkirmishAITunnelRoute::LivePairQueryAllowance(
				(Int)members.size()), tunnelAttemptPathQueryCount,
			*aggregatePathQueryCount,
			MAX_SKIRMISH_AI_TUNNEL_PATH_QUERIES_PER_ATTEMPT,
			MAX_SKIRMISH_AI_TUNNEL_PATH_QUERIES_PER_UPDATE)) {
		state.tunnelCooldownUntilFrame = now +
			SKIRMISH_AI_TUNNEL_PAIR_PROBE_SECONDS * LOGICFRAMES_PER_SECOND;
		return true;
	}
	const Int liveEntry = ProbeSkirmishAITunnelEndpointRole(
		members, entry, target, TRUE,
		&tunnelAttemptPathQueryCount, aggregatePathQueryCount);
	const Int liveExit = liveEntry > 0 ?
		ProbeSkirmishAITunnelEndpointRole(members, exit, target, FALSE,
			&tunnelAttemptPathQueryCount, aggregatePathQueryCount) : 0;
	++*pairCursor;
	--*pairRemaining;
	if (liveEntry < 0 || liveExit < 0) {
		if (liveEntry < 0)
			entryProbe.entryResult = -1;
		if (liveExit < 0)
			exitProbe.exitResult = -1;
		continue;
	}

	state.tunnelTransitPhase = SKIRMISH_AI_TUNNEL_TRANSIT_ENTERING;
	state.tunnelPhaseDeadlineFrame = now +
		SKIRMISH_AI_TUNNEL_TRANSIT_TIMEOUT_SECONDS * LOGICFRAMES_PER_SECOND;
	state.tunnelCooldownUntilFrame = now +
		SKIRMISH_AI_TUNNEL_SUCCESS_COOLDOWN_SECONDS * LOGICFRAMES_PER_SECOND;
	state.tunnelMemberCount = (Int)members.size();
	for (size_t memberIndex = 0; memberIndex < members.size(); ++memberIndex)
		state.tunnelMemberIDs[memberIndex] = members[memberIndex]->getID();
	state.tunnelEntryID = entry->getID();
	state.tunnelExitID = exit->getID();
	state.tunnelTargetID = target->getID();
	state.tunnelCommittedTargetLastSeenFrame = now;
	state.tunnelWaitTargetID = target->getID();
	state.tunnelStrategicTargetID = m_strategyState.strategicTargetID;
	state.targetID = target->getID();
	state.lastProgressFrame = now;
	state.blockedSinceFrame = 0;
	state.tunnelPairSweepTargetID = INVALID_ID;
	group->groupEnter(entry, CMD_FROM_AI);
	return true;
	}
	if (state.tunnelPairSweepRemaining == 0) {
		if (!state.tunnelPairRetryAfterFrame) {
			state.tunnelPairRetryAfterFrame = now +
				60 * LOGICFRAMES_PER_SECOND;
			if (!state.tunnelPairRetryAfterFrame)
				state.tunnelPairRetryAfterFrame = 1;
		}
		Bool queuedTunnel = false;
		Bool deferredBuild = false;
		if (m_tunnelBuildPhase == SKIRMISH_AI_TUNNEL_BUILD_NONE ||
			m_tunnelBuildPhase ==
				SKIRMISH_AI_TUNNEL_BUILD_FORWARD_RETRY_AVAILABLE)
			queuedTunnel = tryQueueTunnelEndpoint(
				team, target, blockingDefense, FALSE, now,
				&tunnelAttemptPathQueryCount, aggregatePathQueryCount,
				state, &deferredBuild);
		const Bool pendingBuildForTarget =
			m_tunnelBuildTargetID == target->getID() &&
			(m_tunnelBuildPhase == SKIRMISH_AI_TUNNEL_BUILD_HOME_QUEUED ||
			 m_tunnelBuildPhase == SKIRMISH_AI_TUNNEL_BUILD_FORWARD_QUEUED ||
			 m_tunnelBuildPhase == SKIRMISH_AI_TUNNEL_BUILD_HOME_CONSTRUCTING ||
			 m_tunnelBuildPhase == SKIRMISH_AI_TUNNEL_BUILD_FORWARD_CONSTRUCTING);
		const Bool retryCooldownPending =
			m_tunnelBuildPhase ==
				SKIRMISH_AI_TUNNEL_BUILD_FORWARD_RETRY_AVAILABLE &&
			m_tunnelBuildTargetID == target->getID() &&
			!IsSkirmishStrategyFrameReached(
				now, m_tunnelBuildCooldownUntilFrame);
		return queuedTunnel || deferredBuild || pendingBuildForTarget ||
			retryCooldownPending;
	}
	state.tunnelCooldownUntilFrame = now +
		SKIRMISH_AI_TUNNEL_PAIR_PROBE_SECONDS * LOGICFRAMES_PER_SECOND;
	return true;
}

static void ExitContainedSkirmishAITunnelMembers(
	const std::vector<Object *> &members, TunnelTracker *tracker,
	Object *tunnel)
{
	for (size_t memberIndex = 0; memberIndex < members.size(); ++memberIndex) {
		Object *member = members[memberIndex];
		const Bool contained = tracker ? tracker->isInContainer(member) :
			member && member->isContained();
		AIUpdateInterface *ai = member ? member->getAIUpdateInterface() : nullptr;
		if (contained && ai)
			ai->aiExit(tunnel, CMD_FROM_AI);
	}
}

void AISkirmishPlayer::updateTunnelTransit(
	Team *team, TacticalTeamState &state, UnsignedInt now)
{
	if (!team || !TheGameLogic ||
		state.tunnelTransitPhase == SKIRMISH_AI_TUNNEL_TRANSIT_NONE)
		return;

	std::vector<Object *> members;
	Bool memberSetStable = state.tunnelMemberCount > 0;
	TunnelTracker *tracker = m_player ? m_player->getTunnelSystem() : nullptr;
	for (Int memberIndex = 0; memberIndex < state.tunnelMemberCount; ++memberIndex) {
		Object *object = TheGameLogic->findObjectByID(
			state.tunnelMemberIDs[memberIndex]);
		if (!object || object->isEffectivelyDead() || object->isDestroyed() ||
			object->getControllingPlayer() != m_player) {
			memberSetStable = false;
			continue;
		}
		if (!IsSkirmishAIStrategyTunnelTransitMember(
				object, m_player, team, true) ||
			object->isKindOf(KINDOF_AIRCRAFT)) {
			memberSetStable = false;
			// A saved passenger can change teams while contained. Keep it in
			// the exit accounting, but never issue the former team's orders.
			const Bool contained = tracker ? tracker->isInContainer(object) :
				object->isContained();
			if (contained && object->getTeam() != team)
				members.push_back(object);
			continue;
		}
		members.push_back(object);
	}
	Int currentTeamMembers = 0;
	for (DLINK_ITERATOR<Object> member = team->iterate_TeamMemberList();
		!member.done(); member.advance()) {
		Object *object = member.cur();
		if (!IsSkirmishAIStrategyTunnelTransitMember(
				object, m_player, team, true) ||
			object->isKindOf(KINDOF_AIRCRAFT))
			continue;
		++currentTeamMembers;
		const Bool contained = tracker ? tracker->isInContainer(object) :
			object->isContained();
		if (!contained)
			continue;
		Bool alreadyTracked = FALSE;
		for (size_t i = 0; i < members.size(); ++i)
			if (members[i]->getID() == object->getID()) {
				alreadyTracked = TRUE;
				break;
			}
		if (!alreadyTracked)
			members.push_back(object);
	}
	if (currentTeamMembers != state.tunnelMemberCount)
		memberSetStable = false;
	std::sort(members.begin(), members.end(),
		IsSkirmishAIStrategyObjectIDBefore);

	AIGroupPtr group = TheAI ? TheAI->createGroup() : nullptr;
	AIGroup *groupObject = nullptr;
	if (group) {
#if RETAIL_COMPATIBLE_AIGROUP
		groupObject = group;
#else
		groupObject = group.Peek();
#endif
		for (size_t memberIndex = 0; memberIndex < members.size(); ++memberIndex)
			if (members[memberIndex]->getTeam() == team)
				groupObject->add(members[memberIndex]);
	}

	Int containedCount = 0;
	for (size_t memberIndex = 0; memberIndex < members.size(); ++memberIndex) {
		const Bool contained = tracker
			? tracker->isInContainer(members[memberIndex])
			: members[memberIndex]->isContained();
		if (contained)
			++containedCount;
	}
	const Bool allContained = memberSetStable && !members.empty() &&
		containedCount == state.tunnelMemberCount;
	const Bool allOut = containedCount == 0;

	Object *target = state.tunnelTargetID != INVALID_ID
		? TheGameLogic->findObjectByID(state.tunnelTargetID) : nullptr;
	const Bool targetUnchanged =
		m_strategyState.strategicTargetID == state.tunnelStrategicTargetID;
	const Bool targetVisibleUsable = IsSkirmishAIStrategyTunnelTargetUsable(
		target, m_player, m_currentEnemy);
	// Boarding can remove the team's only vision of its already committed
	// objective. Let it finish the planned transit while that observation is
	// fresh, but never use a hidden target for a new attack order.
	const ObjectShroudStatus targetShroud = target && m_player
		? target->getShroudedStatus(m_player->getPlayerIndex())
		: OBJECTSHROUD_CLEAR;
	const Bool targetHiddenByFog = target && m_player &&
		targetShroud != OBJECTSHROUD_CLEAR &&
		targetShroud != OBJECTSHROUD_PARTIAL_CLEAR;
	const Bool targetLiveEnemyStatic = target && m_player && m_currentEnemy &&
		target->getControllingPlayer() == m_currentEnemy &&
		IsSkirmishStrategyStaticTarget(target) &&
		!target->isEffectivelyDead() && !target->isDestroyed() &&
		!target->testStatus(OBJECT_STATUS_SOLD);
	const Bool targetWouldBeIntelEligible = target &&
		IsSkirmishAIIntelEligible(target->isKindOf(KINDOF_STRUCTURE), TRUE,
			FALSE, target->testStatus(OBJECT_STATUS_STEALTHED),
			target->testStatus(OBJECT_STATUS_DETECTED),
			target->testStatus(OBJECT_STATUS_MASKED));
	const Bool targetGraceUsable = targetUnchanged &&
		targetHiddenByFog && targetLiveEnemyStatic &&
		targetWouldBeIntelEligible &&
		(state.tunnelTargetID == state.tunnelStrategicTargetID
			? IsSkirmishStrategyTargetObservationAvailable(
				m_strategyState.strategicTargetObserved, now,
				m_strategyState.strategicTargetLastSeenFrame)
			: IsSkirmishStrategyTargetObservationAvailable(TRUE, now,
				state.tunnelCommittedTargetLastSeenFrame));
	const Bool targetUsable = m_strategyState.currentMode ==
		SKIRMISH_STRATEGY_ASSAULT && targetUnchanged &&
		(targetVisibleUsable || targetGraceUsable);
	Object *currentTarget = m_strategyState.strategicTargetID != INVALID_ID
		? TheGameLogic->findObjectByID(m_strategyState.strategicTargetID) : nullptr;
	if (!IsSkirmishAIStrategyTunnelTargetUsable(
			currentTarget, m_player, m_currentEnemy))
		currentTarget = nullptr;

	Object *entry = state.tunnelEntryID != INVALID_ID
		? TheGameLogic->findObjectByID(state.tunnelEntryID) : nullptr;
	Object *exit = state.tunnelExitID != INVALID_ID
		? TheGameLogic->findObjectByID(state.tunnelExitID) : nullptr;
	const Bool entryRegistered = IsSkirmishAIStrategyTunnelEndpointRegistered(
		entry, m_player, tracker);
	const Bool exitRegistered = IsSkirmishAIStrategyTunnelEndpointRegistered(
		exit, m_player, tracker);

	if (members.empty()) {
		state.tunnelWaitTargetID = targetVisibleUsable && targetUsable
			? state.tunnelTargetID : INVALID_ID;
		state.tunnelTransitPhase = SKIRMISH_AI_TUNNEL_TRANSIT_NONE;
		state.tunnelPhaseDeadlineFrame = 0;
		state.tunnelMemberCount = 0;
		for (Int i = 0; i < MAX_SKIRMISH_AI_TUNNEL_MEMBERS; ++i)
			state.tunnelMemberIDs[i] = INVALID_ID;
		state.tunnelEntryID = INVALID_ID;
		state.tunnelExitID = INVALID_ID;
		state.tunnelTargetID = INVALID_ID;
		state.tunnelStrategicTargetID = INVALID_ID;
		state.tunnelCommittedTargetLastSeenFrame = 0;
		state.tunnelCooldownUntilFrame = now +
			SKIRMISH_AI_TUNNEL_SUCCESS_COOLDOWN_SECONDS * LOGICFRAMES_PER_SECOND;
		return;
	}

	Bool abortTransit = !memberSetStable || !targetUsable;
	if (state.tunnelTransitPhase == SKIRMISH_AI_TUNNEL_TRANSIT_ENTERING) {
		const Int capacity = tracker ? tracker->getContainMax() : 0;
		const Int remaining = state.tunnelMemberCount - containedCount;
		const Bool capacityAvailable = tracker && capacity > 0 && remaining >= 0 &&
			tracker->getContainCount() + (UnsignedInt)remaining <=
				(UnsignedInt)capacity;
		if (!entryRegistered || !capacityAvailable ||
			IsSkirmishStrategyFrameReached(now, state.tunnelPhaseDeadlineFrame))
			abortTransit = true;
		if (!abortTransit && allContained) {
			if (exitRegistered && targetUsable) {
				if (groupObject)
					groupObject->groupExit(exit, CMD_FROM_AI);
				else {
					for (size_t memberIndex = 0; memberIndex < members.size(); ++memberIndex) {
						AIUpdateInterface *ai = members[memberIndex]->getAIUpdateInterface();
						if (ai && tracker->isInContainer(members[memberIndex]))
							ai->aiExit(exit, CMD_FROM_AI);
					}
				}
				state.tunnelTransitPhase = SKIRMISH_AI_TUNNEL_TRANSIT_EXITING;
				state.tunnelPhaseDeadlineFrame = now +
					SKIRMISH_AI_TUNNEL_TRANSIT_TIMEOUT_SECONDS * LOGICFRAMES_PER_SECOND;
				return;
			}
			abortTransit = true;
		}
		if (!abortTransit)
			return;
	} else if (state.tunnelTransitPhase == SKIRMISH_AI_TUNNEL_TRANSIT_EXITING) {
		if (allOut) {
			abortTransit = false;
		} else if (!exitRegistered ||
			IsSkirmishStrategyFrameReached(now, state.tunnelPhaseDeadlineFrame)) {
			abortTransit = true;
		} else {
			return;
		}
	} else if (state.tunnelTransitPhase ==
			SKIRMISH_AI_TUNNEL_TRANSIT_FALLBACK_EXIT) {
		if (!allOut) {
			if (IsSkirmishStrategyFrameReached(now, state.tunnelPhaseDeadlineFrame)) {
				Object *fallbackExit = SelectSkirmishAIStrategyFallbackTunnel(
					m_player, tracker, state.tunnelExitID, INVALID_ID);
				state.tunnelExitID = fallbackExit ? fallbackExit->getID() : INVALID_ID;
				ExitContainedSkirmishAITunnelMembers(
					members, tracker, fallbackExit);
				state.tunnelPhaseDeadlineFrame = now +
					SKIRMISH_AI_TUNNEL_TRANSIT_TIMEOUT_SECONDS * LOGICFRAMES_PER_SECOND;
			}
			return;
		}
	} else {
		abortTransit = true;
	}

	if (abortTransit && !allOut) {
		if (state.tunnelTransitPhase != SKIRMISH_AI_TUNNEL_TRANSIT_FALLBACK_EXIT) {
			Object *fallbackExit = SelectSkirmishAIStrategyFallbackTunnel(
				m_player, tracker, INVALID_ID, state.tunnelEntryID);
			state.tunnelExitID = fallbackExit ? fallbackExit->getID() : INVALID_ID;
			ExitContainedSkirmishAITunnelMembers(
				members, tracker, fallbackExit);
			state.tunnelTransitPhase = SKIRMISH_AI_TUNNEL_TRANSIT_FALLBACK_EXIT;
			state.tunnelPhaseDeadlineFrame = now +
				SKIRMISH_AI_TUNNEL_TRANSIT_TIMEOUT_SECONDS * LOGICFRAMES_PER_SECOND;
			state.tunnelCooldownUntilFrame = now +
				SKIRMISH_AI_TUNNEL_SUCCESS_COOLDOWN_SECONDS * 2 *
				LOGICFRAMES_PER_SECOND;
		}
		return;
	}

	const Bool completedAtPlannedExit =
		state.tunnelTransitPhase == SKIRMISH_AI_TUNNEL_TRANSIT_EXITING &&
		targetUsable && exitRegistered;
	state.tunnelWaitTargetID = targetVisibleUsable && targetUsable
		? state.tunnelTargetID : INVALID_ID;
	state.tunnelTransitPhase = SKIRMISH_AI_TUNNEL_TRANSIT_NONE;
	state.tunnelPhaseDeadlineFrame = 0;
	state.tunnelMemberCount = 0;
	for (Int i = 0; i < MAX_SKIRMISH_AI_TUNNEL_MEMBERS; ++i)
		state.tunnelMemberIDs[i] = INVALID_ID;
	state.tunnelEntryID = INVALID_ID;
	state.tunnelExitID = INVALID_ID;
	state.tunnelTargetID = INVALID_ID;
	state.tunnelStrategicTargetID = INVALID_ID;
	state.tunnelCommittedTargetLastSeenFrame = 0;
	state.tunnelCooldownUntilFrame = now +
		(completedAtPlannedExit ? SKIRMISH_AI_TUNNEL_SUCCESS_COOLDOWN_SECONDS :
			SKIRMISH_AI_TUNNEL_SUCCESS_COOLDOWN_SECONDS * 2) *
		LOGICFRAMES_PER_SECOND;
	state.lastProgressFrame = now;
	state.blockedSinceFrame = 0;
	state.regroupUntilFrame = now + 8 * LOGICFRAMES_PER_SECOND;
	if (!targetUnchanged) {
		state.targetID = INVALID_ID;
		state.targetHealth = 0.0f;
		state.distanceToTargetSqr = 0.0f;
		state.approachAttempt = 0;
		state.alternateProbeAfterID = INVALID_ID;
		state.routeExhaustedTargetID = INVALID_ID;
		state.routeExhaustedUntilFrame = 0;
	}

	if (m_strategyState.currentMode == SKIRMISH_STRATEGY_ASSAULT &&
		!currentTarget && targetVisibleUsable && targetUsable)
		currentTarget = target;
	if (groupObject) {
		if (m_strategyState.currentMode == SKIRMISH_STRATEGY_ASSAULT && currentTarget)
			groupObject->groupAttackMoveToPosition(currentTarget->getPosition(),
				NO_MAX_SHOTS_LIMIT, CMD_FROM_AI);
		else if (m_strategyState.currentMode == SKIRMISH_STRATEGY_FORTIFY &&
			m_baseCenterSet)
			groupObject->groupGuardPosition(&m_baseCenter,
				GUARDMODE_GUARD_WITHOUT_PURSUIT, CMD_FROM_AI);
		else
			groupObject->groupIdle(CMD_FROM_AI);
	} else {
		for (size_t memberIndex = 0; memberIndex < members.size(); ++memberIndex) {
			if (members[memberIndex]->getTeam() != team)
				continue;
			AIUpdateInterface *ai = members[memberIndex]->getAIUpdateInterface();
			if (!ai)
				continue;
			if (m_strategyState.currentMode == SKIRMISH_STRATEGY_ASSAULT &&
				currentTarget)
				ai->aiAttackMoveToPosition(currentTarget->getPosition(),
					NO_MAX_SHOTS_LIMIT, CMD_FROM_AI);
			else if (m_strategyState.currentMode == SKIRMISH_STRATEGY_FORTIFY &&
				m_baseCenterSet)
				ai->aiGuardPosition(&m_baseCenter,
					GUARDMODE_GUARD_WITHOUT_PURSUIT, CMD_FROM_AI);
			else
				ai->aiIdle(CMD_FROM_AI);
		}
	}
}

static Bool IsSkirmishTacticalRetreatPointSafe(
	const Coord3D *start, const Coord3D *destination,
	const std::vector<Object *> &hazards)
{
	if (!start || !destination)
		return false;
	const Real segmentX = destination->x - start->x;
	const Real segmentY = destination->y - start->y;
	const Real segmentLengthSqr = segmentX * segmentX + segmentY * segmentY;
	for (size_t hazardIndex = 0; hazardIndex < hazards.size(); ++hazardIndex) {
		Object *enemy = hazards[hazardIndex];
		const Bool armedStructure = enemy->isKindOf(KINDOF_STRUCTURE) &&
			!enemy->testStatus(OBJECT_STATUS_UNDER_CONSTRUCTION) &&
			!enemy->testStatus(OBJECT_STATUS_SOLD) &&
			enemy->getLargestWeaponRange() > 0.0f;
		Real destinationClearance = 225.0f;
		Real corridorClearance = 150.0f;
		if (armedStructure) {
			const Real weaponClearance = enemy->getLargestWeaponRange() + 50.0f;
			if (weaponClearance > destinationClearance)
				destinationClearance = weaponClearance;
			if (weaponClearance > corridorClearance)
				corridorClearance = weaponClearance;
			if (destinationClearance > 800.0f) destinationClearance = 800.0f;
			if (corridorClearance > 800.0f) corridorClearance = 800.0f;
		}
		const Real dx = enemy->getPosition()->x - destination->x;
		const Real dy = enemy->getPosition()->y - destination->y;
		if (dx * dx + dy * dy < destinationClearance * destinationClearance)
			return false;
		if (segmentLengthSqr > 1.0f) {
			const Real fromStartX = enemy->getPosition()->x - start->x;
			const Real fromStartY = enemy->getPosition()->y - start->y;
			const Real fraction = (fromStartX * segmentX +
				fromStartY * segmentY) / segmentLengthSqr;
			// A turret at or behind the start cannot get closer along this path.
			// Let the team escape its current range, but reject turrets ahead.
			// Mobile threats still allow the initial contested tenth.
			if ((armedStructure ? fraction > 0.0f : fraction >= 0.1f) &&
				fraction <= 1.0f) {
				const Real lateralX = fromStartX - fraction * segmentX;
				const Real lateralY = fromStartY - fraction * segmentY;
				if (lateralX * lateralX + lateralY * lateralY <
					corridorClearance * corridorClearance)
					return false;
			}
		}
	}
	return true;
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
	if (ShouldUseCurrentSkirmishAITacticalBehavior() &&
		currentMode == SKIRMISH_STRATEGY_ASSAULT &&
		previousTargetID != m_strategyState.strategicTargetID) {
		const UnsignedInt now = TheGameLogic->getFrame();
		for (std::map<UnsignedInt, TacticalTeamState>::iterator it =
				m_tacticalTeams.begin(); it != m_tacticalTeams.end(); ++it) {
			TacticalTeamState &state = it->second;
			if (state.tunnelTransitPhase != SKIRMISH_AI_TUNNEL_TRANSIT_NONE)
				continue;
			state.targetID = INVALID_ID;
			state.targetHealth = 0.0f;
			state.distanceToTargetSqr = 0.0f;
			state.lastProgressFrame = now;
			state.blockedSinceFrame = 0;
			state.approachAttempt = 0;
			state.alternateProbeAfterID = INVALID_ID;
			state.routeExhaustedTargetID = INVALID_ID;
			state.routeExhaustedUntilFrame = 0;
		}
	}
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
	const Bool tunnelBuildPendingForTarget = targetID != INVALID_ID &&
		m_tunnelBuildTargetID == targetID &&
		(m_tunnelBuildPhase == SKIRMISH_AI_TUNNEL_BUILD_HOME_QUEUED ||
		 m_tunnelBuildPhase == SKIRMISH_AI_TUNNEL_BUILD_FORWARD_QUEUED ||
		 m_tunnelBuildPhase == SKIRMISH_AI_TUNNEL_BUILD_HOME_CONSTRUCTING ||
		 m_tunnelBuildPhase == SKIRMISH_AI_TUNNEL_BUILD_FORWARD_CONSTRUCTING);
	Bool tacticalRouteExhausted = false;
	Bool viableSuperweaponPlan = false;
	if (ShouldUseCurrentSkirmishAITacticalBehavior()) {
		Int exhaustedTeams = 0;
		Int availableTeams = 0;
		Int regroupingTeams = 0;
		Player::PlayerTeamList::const_iterator prototype;
		for (prototype = m_player->getPlayerTeams()->begin();
			prototype != m_player->getPlayerTeams()->end(); ++prototype) {
			for (DLINK_ITERATOR<Team> instance = (*prototype)->iterate_TeamInstanceList();
				!instance.done(); instance.advance()) {
				Team *team = instance.cur();
				if (!IsSkirmishStrategyOffensiveTeam(team, m_player))
					continue;
				std::map<UnsignedInt, TacticalTeamState>::iterator it =
					m_tacticalTeams.find(team->getID());
				const Bool hasState = it != m_tacticalTeams.end();
				if (!HasSkirmishStrategyPotentialOffensiveRecipient(team, m_player) &&
					(!hasState || it->second.tunnelTransitPhase ==
						SKIRMISH_AI_TUNNEL_TRANSIT_NONE))
					continue;
				if (!hasState) {
					++availableTeams;
					continue;
				}
				TacticalTeamState &teamState = it->second;
			if (teamState.routeExhaustedTargetID != INVALID_ID &&
				(tunnelBuildPendingForTarget ||
				 teamState.routeExhaustedTargetID != targetID ||
				 IsSkirmishStrategyFrameReached(currentFrame,
					teamState.routeExhaustedUntilFrame) ||
				 m_strategyState.superweaponAttemptStatus ==
					SKIRMISH_STRATEGY_ATTEMPT_SUCCEEDED)) {
				teamState.routeExhaustedTargetID = INVALID_ID;
				teamState.routeExhaustedUntilFrame = 0;
			}
			if (targetID != INVALID_ID &&
				teamState.routeExhaustedTargetID == targetID)
				++exhaustedTeams;
			else if (teamState.retreating || teamState.woundedReserve)
				++regroupingTeams;
			else
				++availableTeams;
			}
		}
		tacticalRouteExhausted = targetID != INVALID_ID &&
			exhaustedTeams > 0 && availableTeams == 0 &&
			regroupingTeams == 0 && !tunnelBuildPendingForTarget;
		if (m_strategyState.currentMode == SKIRMISH_STRATEGY_ASSAULT &&
			availableTeams == 0 && regroupingTeams > 0)
			metrics.assaultLostHalfForce = true;
		if (tacticalRouteExhausted && usesProductionBehavior())
			viableSuperweaponPlan =
				FindSkirmishAIStrategicSource(m_player, nullptr) != nullptr ||
				FindSkirmishAISuperweaponConstruction(m_player) != nullptr;
	}
	const SkirmishStrategyMode previousMode = m_strategyState.currentMode;
	SkirmishStrategyDecision decision = EvaluateSkirmishStrategy(
		m_strategyState, metrics, m_difficulty, currentFrame,
		usesProductionBehavior(), tacticalRouteExhausted, viableSuperweaponPlan);
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
	if ((m_tunnelBuildPhase == SKIRMISH_AI_TUNNEL_BUILD_HOME_QUEUED ||
		 m_tunnelBuildPhase == SKIRMISH_AI_TUNNEL_BUILD_FORWARD_QUEUED) &&
		(m_strategyState.currentMode != SKIRMISH_STRATEGY_ASSAULT ||
		 m_strategyState.strategicTargetID != m_tunnelBuildTargetID))
		abandonTunnelBuildPlan(currentFrame, TRUE);
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

	Bool hasCandidate = false;
	Int highestPriority = (-2147483647 - 1);
	std::vector<SkirmishProductionCandidate> candidates;
	Player::PlayerTeamList::const_iterator teamIt;
	for (teamIt = m_player->getPlayerTeams()->begin(); teamIt != m_player->getPlayerTeams()->end(); ++teamIt) {
		SkirmishProductionCandidate candidate;
		candidate.prototype = *teamIt;
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
static void CollectSkirmishAIDefenseSupplyPositions(
	Player *player, std::vector<Coord2D> *positions)
{
	positions->clear();
	for (Object *object = TheGameLogic->getFirstObject(); object;
		object = object->getNextObject()) {
		if (object->isEffectivelyDead() || object->isDestroyed() ||
			!(object->isKindOf(KINDOF_SUPPLY_SOURCE) ||
			 (object->getControllingPlayer() == player &&
			  object->isKindOf(KINDOF_FS_SUPPLY_CENTER))))
			continue;
		Coord2D supplyPosition;
		supplyPosition.x = object->getPosition()->x;
		supplyPosition.y = object->getPosition()->y;
		positions->push_back(supplyPosition);
	}
	for (BuildListInfo *info = player->getBuildList(); info;
		info = info->getNext()) {
		const ThingTemplate *plan = TheThingFactory->findTemplate(
			info->getTemplateName());
		if (!plan || !plan->isKindOf(KINDOF_FS_SUPPLY_CENTER) ||
			!info->isBuildable())
			continue;
		Coord2D supplyPosition;
		supplyPosition.x = info->getLocation()->x;
		supplyPosition.y = info->getLocation()->y;
		positions->push_back(supplyPosition);
	}
}

static Bool IsSkirmishAIDefenseNearSupply(
	const std::vector<Coord2D> &supplyPositions,
	const Coord3D &position, Real structureRadius);

static Bool IsSkirmishAIDefenseLineSite(
	const Coord3D &baseCenter, Real baseRadius,
	const SkirmishAIDefenseContext &context, const Coord3D &position,
	Real structureRadius, const std::vector<Coord2D> &supplyPositions)
{
	const Real dx = position.x - baseCenter.x;
	const Real dy = position.y - baseCenter.y;
	const Int route = ClassifySkirmishAIDefenseRoute(
		(Int)dx, (Int)dy, context.anchors);
	if (!IsSkirmishAIDefenseRoute(route)) return false;
	const Real along = dx * context.direction[route].x +
		dy * context.direction[route].y;
	const Real lateral = dx * context.direction[route].y -
		dy * context.direction[route].x;
	if (!IsSkirmishAIDefenseLinePosition(
			(Int)along, (Int)lateral, (Int)baseRadius,
			(Int)structureRadius))
		return false;
	return !IsSkirmishAIDefenseNearSupply(supplyPositions, position,
		structureRadius);
}

static void CountSkirmishAIDefenseLine(
	Player *player, const Coord3D &baseCenter, Real baseRadius,
	const SkirmishAIDefenseContext &context,
	const std::vector<Coord2D> &supplyPositions,
	SkirmishAIDefenseBuildCounts *counts)
{
	for (Int route = 0; route < SKIRMISH_AI_DEFENSE_ROUTE_COUNT; ++route) {
		counts->owned[route] = 0;
		counts->queued[route] = 0;
	}
	for (Object *object = TheGameLogic->getFirstObject(); object;
		object = object->getNextObject()) {
		if (object->getControllingPlayer() != player ||
			!object->isKindOf(KINDOF_FS_BASE_DEFENSE) ||
			object->isEffectivelyDead() || object->isDestroyed() ||
			!IsSkirmishAIDefenseLineSite(baseCenter, baseRadius,
				context, *object->getPosition(),
				object->getTemplate()->getTemplateGeometryInfo().getBoundingCircleRadius(),
				supplyPositions))
			continue;
		const Int route = ClassifySkirmishAIDefenseRoute(
			(Int)(object->getPosition()->x - baseCenter.x),
			(Int)(object->getPosition()->y - baseCenter.y), context.anchors);
		if (IsSkirmishAIDefenseRoute(route)) ++counts->owned[route];
	}
	for (BuildListInfo *info = player->getBuildList(); info;
		info = info->getNext()) {
		const ThingTemplate *plan = TheThingFactory->findTemplate(
			info->getTemplateName());
		if (!plan || !plan->isKindOf(KINDOF_FS_BASE_DEFENSE) ||
			!info->isBuildable() ||
			!IsSkirmishAIDefenseLineSite(baseCenter, baseRadius,
				context, *info->getLocation(),
				plan->getTemplateGeometryInfo().getBoundingCircleRadius(),
				supplyPositions))
			continue;
		// A live structure was counted above. An unstarted priority entry
		// with INVALID_ID reserves a line slot immediately.
		Object *built = info->getObjectID() == INVALID_ID ? nullptr :
			TheGameLogic->findObjectByID(info->getObjectID());
		if (built && built->getControllingPlayer() == player &&
			!built->isEffectivelyDead())
			continue;
		const Int route = ClassifySkirmishAIDefenseRoute(
			(Int)(info->getLocation()->x - baseCenter.x),
			(Int)(info->getLocation()->y - baseCenter.y), context.anchors);
		if (IsSkirmishAIDefenseRoute(route)) ++counts->queued[route];
	}
}

static Bool IsSkirmishAIDefenseNearSupply(
	const std::vector<Coord2D> &supplyPositions,
	const Coord3D &position, Real structureRadius)
{
	Real clearance = structureRadius + 160.0f;
	if (clearance < 220.0f) clearance = 220.0f;
	const Real clearanceSqr = clearance * clearance;
	for (size_t i = 0; i < supplyPositions.size(); ++i) {
		const Real dx = supplyPositions[i].x - position.x;
		const Real dy = supplyPositions[i].y - position.y;
		if (dx * dx + dy * dy < clearanceSqr) return true;
	}
	return false;
}

static Bool IsSkirmishAIDefenseSiteOverlappingPendingBuild(
	Player *player, const Coord3D &position, Real structureRadius,
	const BuildListInfo *ignoreInfo)
{
	if (!player || !TheThingFactory)
		return false;
	for (BuildListInfo *info = player->getBuildList(); info;
		info = info->getNext()) {
		if (info == ignoreInfo) continue;
		if (!info->isBuildable())
			continue;
		const ThingTemplate *pendingPlan = TheThingFactory->findTemplate(
			info->getTemplateName());
		if (!pendingPlan || !pendingPlan->isKindOf(KINDOF_STRUCTURE))
			continue;
		Object *built = info->getObjectID() == INVALID_ID ? nullptr :
			TheGameLogic->findObjectByID(info->getObjectID());
		if (built && built->getControllingPlayer() == player &&
			!built->isEffectivelyDead() && !built->isDestroyed())
			continue;
		const Real pendingRadius = pendingPlan->getTemplateGeometryInfo()
			.getBoundingCircleRadius();
		if (DoSkirmishAIDefenseFootprintsOverlap(
				position.x, position.y, structureRadius,
				info->getLocation()->x, info->getLocation()->y, pendingRadius))
			return true;
	}
	return false;
}

static Bool HasReachableSkirmishAIDefenseBuilder(
	const std::vector<Object *> &builders, const Coord3D &position,
	size_t startIndex, Int *pathQueriesUsed)
{
	if (!TheAI || !TheAI->pathfinder()) return false;
	for (size_t offset = 0; offset < builders.size(); ++offset) {
		Object *object = builders[(startIndex + offset) % builders.size()];
		AIUpdateInterface *ai = object->getAIUpdateInterface();
		if (!ai || !ai->getDozerAIInterface()) continue;
		if (!TryConsumeSkirmishAITacticalPathQuery(pathQueriesUsed, 16))
			return false;
		if (TheAI->pathfinder()->clientSafeQuickDoesPathExist(
				ai->getLocomotorSet(), object->getPosition(), &position))
			return true;
	}
	return false;
}

static Bool QueueSkirmishAIDefenseLine(
	Player *player, const Coord3D &baseCenter, Real baseRadius,
	const ThingTemplate *plan, const AsciiString &thingName,
	Bool flank, Int flankCounter, UnsignedInt placementAttempt,
	ObjectID excludedBuilderID)
{
	if (!player || !plan || !TheAI || !TheGameLogic || !TheBuildAssistant ||
		!TheTerrainLogic || !TheTerrainVisual || baseRadius <= 0.0f)
		return false;
	SkirmishAIDefenseContext context;
	CollectSkirmishAIDefenseContext(player, baseCenter, baseRadius, &context);
	std::vector<Coord2D> supplyPositions;
	CollectSkirmishAIDefenseSupplyPositions(player, &supplyPositions);
	SkirmishAIDefenseBuildCounts counts;
	CountSkirmishAIDefenseLine(player, baseCenter, baseRadius,
		context, supplyPositions, &counts);
	Int routeOrder[SKIRMISH_AI_DEFENSE_ROUTE_COUNT] = {
		SKIRMISH_AI_DEFENSE_CENTER, SKIRMISH_AI_DEFENSE_FLANK,
		SKIRMISH_AI_DEFENSE_BACKDOOR };
	if (flank) {
		const Int flankScore = GetSkirmishAIDefenseRouteScore(
			&context.threat, SKIRMISH_AI_DEFENSE_FLANK);
		const Int backdoorScore = GetSkirmishAIDefenseRouteScore(
			&context.threat, SKIRMISH_AI_DEFENSE_BACKDOOR);
		Int preferredFlank = SKIRMISH_AI_DEFENSE_FLANK;
		if (flankScore != backdoorScore)
			preferredFlank = flankScore > backdoorScore ?
				SKIRMISH_AI_DEFENSE_FLANK : SKIRMISH_AI_DEFENSE_BACKDOOR;
		else {
			const Int flankCount = counts.owned[SKIRMISH_AI_DEFENSE_FLANK] +
				counts.queued[SKIRMISH_AI_DEFENSE_FLANK];
			const Int backdoorCount = counts.owned[SKIRMISH_AI_DEFENSE_BACKDOOR] +
				counts.queued[SKIRMISH_AI_DEFENSE_BACKDOOR];
			preferredFlank = flankCount != backdoorCount ?
				(flankCount < backdoorCount ? SKIRMISH_AI_DEFENSE_FLANK :
				 SKIRMISH_AI_DEFENSE_BACKDOOR) :
				(flankCounter & 1 ? SKIRMISH_AI_DEFENSE_FLANK :
				 SKIRMISH_AI_DEFENSE_BACKDOOR);
		}
		routeOrder[0] = preferredFlank;
		routeOrder[1] = preferredFlank == SKIRMISH_AI_DEFENSE_FLANK ?
			SKIRMISH_AI_DEFENSE_BACKDOOR : SKIRMISH_AI_DEFENSE_FLANK;
		routeOrder[2] = SKIRMISH_AI_DEFENSE_CENTER;
	}
	// Stable score ordering gives an active center, flank, or backdoor threat
	// first choice; the requested front/flank side breaks equal-score ties.
	for (Int i = 1; i < SKIRMISH_AI_DEFENSE_ROUTE_COUNT; ++i) {
		const Int route = routeOrder[i];
		const Int score = GetSkirmishAIDefenseRouteScore(&context.threat, route);
		Int j = i;
		while (j > 0 && score > GetSkirmishAIDefenseRouteScore(
				&context.threat, routeOrder[j - 1])) {
			routeOrder[j] = routeOrder[j - 1];
			--j;
		}
		routeOrder[j] = route;
	}
	Int total = 0;
	for (Int i = 0; i < SKIRMISH_AI_DEFENSE_ROUTE_COUNT; ++i)
		total += counts.owned[i] + counts.queued[i];
	if (total >= 5)
		return false;
	const Real structureRadius =
		plan->getTemplateGeometryInfo().getBoundingCircleRadius();
	const Real sideStep = structureRadius * 2.0f + 60.0f;
	const Int sideOrder[5] = { 0, -1, 1, -2, 2 };
	const Real radialOrder[3] = { 0.0f, 70.0f, -70.0f };
	double phaseRadial = 0.0;
	double phaseSide = 0.0;
	// A site starts every attempt. The extra phase step gives every site all
	// eight offsets over time, despite forty-five candidate slots per pass.
	const UnsignedInt phase = placementAttempt % 8 +
		(placementAttempt / 45) % 8;
	GetSkirmishAIDefensePlacementPhase(phase, sideStep,
		&phaseRadial, &phaseSide);
	std::vector<Object *> builders;
	for (Object *candidate = TheGameLogic->getFirstObject(); candidate;
		candidate = candidate->getNextObject()) {
		if (IsAvailableSkirmishAIDefenseBuilder(candidate, player, plan,
				excludedBuilderID))
			builders.push_back(candidate);
	}
	if (builders.empty()) return false;
	std::sort(builders.begin(), builders.end(),
		IsSkirmishAIStrategyObjectIDBefore);
	Int pathQueriesUsed = 0;
	const Int siteCount = 15 * SKIRMISH_AI_DEFENSE_ROUTE_COUNT;
	const Int siteStart = placementAttempt % 15;
	for (Int slot = 0; slot < siteCount && pathQueriesUsed < 16; ++slot) {
		const Int routeIndex = slot % SKIRMISH_AI_DEFENSE_ROUTE_COUNT;
		const Int siteIndex = (siteStart +
			slot / SKIRMISH_AI_DEFENSE_ROUTE_COUNT) % 15;
		const Int radial = siteIndex / 5;
		const Int side = siteIndex % 5;
		const Int route = routeOrder[routeIndex];
		if (!context.anchors[route].available)
			continue;
		const Int routeScore = GetSkirmishAIDefenseRouteScore(
			&context.threat, route);
		const Int localCount = counts.owned[route] + counts.queued[route];
		const Bool penetrated = context.threat.penetrationScore[route] > 0;
		const Int perRouteCap = penetrated ? 3 : 2;
		// Penetration permits one extra inner site; the global cap stays at five.
		const Int minimumScore = localCount >= 2 && penetrated ? 1 :
			(localCount > 0 ? 101 : 100);
		if (routeScore == 0 ? localCount >= 1 :
			!ShouldQueueSkirmishAIDefense(&context.threat, &counts,
				route, minimumScore, perRouteCap, 5))
			continue;
		Real lineDistance = baseRadius +
			TheAI->getAiData()->m_skirmishBaseDefenseExtraDistance;
		if (context.threat.penetrationScore[route] > 0) {
			lineDistance -= 120.0f;
			if (lineDistance < baseRadius * 0.7f)
				lineDistance = baseRadius * 0.7f;
		}
		if (lineDistance < structureRadius * 2.0f + 80.0f)
			lineDistance = structureRadius * 2.0f + 80.0f;
		Coord3D position = baseCenter;
		const Real distance = lineDistance + radialOrder[radial] +
			(Real)phaseRadial;
		if (distance < structureRadius * 2.0f + 80.0f ||
			distance > baseRadius + 250.0f) continue;
		const Real lateral = sideStep * sideOrder[side] + phaseSide;
		position.x += context.direction[route].x * distance -
			context.direction[route].y * lateral;
		position.y += context.direction[route].y * distance +
			context.direction[route].x * lateral;
		position.z = TheTerrainLogic->getGroundHeight(position.x, position.y);
		if (IsSkirmishAIDefenseNearSupply(supplyPositions, position,
			structureRadius)) continue;
		if (IsSkirmishAIDefenseSiteOverlappingPendingBuild(
				player, position, structureRadius)) continue;
		if (ClassifySkirmishAIDefenseRoute(
				(Int)(position.x - baseCenter.x),
				(Int)(position.y - baseCenter.y), context.anchors) != route ||
			!IsSkirmishAIDefenseLineSite(baseCenter, baseRadius,
				context, position, structureRadius, supplyPositions))
			continue;
		const size_t builderStart =
			((placementAttempt / 240) + (placementAttempt % 240) +
			 siteIndex + routeIndex * 15) % builders.size();
		if (!HasReachableSkirmishAIDefenseBuilder(builders, position,
			builderStart, &pathQueriesUsed))
			continue;
		const Real angle = plan->getPlacementViewAngle();
		const Bool legal = LBC_OK == TheBuildAssistant->isLocationLegalToBuild(
			&position, plan, angle,
			BuildAssistant::CLEAR_PATH |
			BuildAssistant::TERRAIN_RESTRICTIONS |
			BuildAssistant::NO_OBJECT_OVERLAP, nullptr, player);
		TheTerrainVisual->removeAllBibs();
		if (!legal) continue;
		// buildStructureWithDozer adds terrain height to BuildListInfo.z.
		Coord3D queuedPosition = position;
		queuedPosition.z = 0.0f;
		player->addToPriorityBuildList(thingName, &queuedPosition, angle);
		return true;
	}
	return false;
}

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
	if (ShouldUseCurrentSkirmishAITacticalBehavior() &&
		tTemplate->isKindOf(KINDOF_FS_BASE_DEFENSE)) {
		const UnsignedInt now = TheGameLogic->getFrame();
		if (!IsSkirmishStrategyFrameReached(
				now, m_defensePlacementNextFrame))
			return;
		PruneGeneratedDefenseBuildMarkers(
			&m_generatedDefenseBuilds, m_player);
		Bool queued = false;
		if (m_generatedDefenseBuilds.size() < 5)
			queued = QueueSkirmishAIDefenseLine(
				m_player, m_baseCenter, m_baseRadius, tTemplate, thingName,
				flank, m_curFlankBaseDefense, m_defensePlacementAttempt,
				m_repairDozer);
		if (queued) {
			BuildListInfo *queuedInfo = m_player->getBuildList();
			if (queuedInfo && queuedInfo->getLocation()) {
				GeneratedDefenseBuild marker;
				marker.templateName = queuedInfo->getTemplateName();
				marker.location.x = queuedInfo->getLocation()->x;
				marker.location.y = queuedInfo->getLocation()->y;
				marker.angle = queuedInfo->getAngle();
				m_generatedDefenseBuilds.push_back(marker);
				queuedInfo->setObjectTimestamp(now + 1);
			}
		}
		++m_defensePlacementAttempt;
		m_defensePlacementNextFrame = queued ? now :
			now + 10 * LOGICFRAMES_PER_SECOND;
		if (queued) {
			if (flank) ++m_curFlankBaseDefense;
			else ++m_curFrontBaseDefense;
		}
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
			queueUnits(); // update the queues.
			if (m_readyToBuildTeam) {
				processTeamBuilding();
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
	if (strategyAllowedAfterRecovery)
		updateTacticalTeams();
	if (strategyAllowedAfterRecovery)
		updateDefensePatrol();
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
	m_strategyTargetFallbackPending = false;
	m_strategyTargetFallbackAfterID = INVALID_ID;
	m_strategyTargetFallbackEnemyIndex = -1;
	m_strategyProductionReserveCost = 0;
	m_strategySuperweaponID = INVALID_ID;
	m_strategyAuthorizedThing = nullptr;
	m_strategySpendAuthorization = SKIRMISH_AI_SPEND_AUTHORIZATION_NONE;
	m_strategyProductionReserveRefreshing = false;
	clearStrategySourceCommandLock();
	m_reinforcementRoundRobinCursor = 0;
	m_tacticalTeams.clear();
	m_tacticalNextTeamScanFrame = 0;
	m_tunnelBuildPhase = SKIRMISH_AI_TUNNEL_BUILD_NONE;
	m_tunnelHomeAttempted = false;
	m_tunnelForwardAttempted = false;
	m_tunnelForwardRetryConsumed = false;
	m_tunnelHomeEndpointID = INVALID_ID;
	m_tunnelForwardEndpointID = INVALID_ID;
	m_tunnelForwardAttemptTargetID = INVALID_ID;
	for (Int i = 0; i < SkirmishAITunnelRoute::MAX_GENERATED_FORWARD_ENDPOINTS;
		++i) {
		m_tunnelGeneratedForwardEndpointIDs[i] = INVALID_ID;
		m_tunnelGeneratedForwardTargetIDs[i] = INVALID_ID;
	}
	for (Int i = 0; i < SkirmishAITunnelRoute::MAX_EXHAUSTED_FORWARD_TARGETS;
		++i)
		m_tunnelExhaustedForwardTargetIDs[i] = INVALID_ID;
	m_tunnelBuildBuilderID = INVALID_ID;
	m_tunnelBuildTargetID = INVALID_ID;
	m_tunnelBuildBlockerID = INVALID_ID;
	m_tunnelBuildObjectID = INVALID_ID;
	m_tunnelBuildLockedBuilderID = INVALID_ID;
	m_tunnelPendingBuilderCursor = 0;
	m_tunnelBuildLocation.zero();
	m_defenseBuildLockedBuilderID = INVALID_ID;
	m_defenseBuildLockedLocation.zero();
	m_tunnelBuildDeadlineFrame = 0;
	m_tunnelBuildCooldownUntilFrame = 0;
	m_defensePatrolRoute = SKIRMISH_AI_DEFENSE_NO_ROUTE;
	m_defensePatrolTeamID = 0;
	m_defensePatrolObjectID = INVALID_ID;
	m_defenseNextPatrolFrame = 0;
	m_defenseQuietPatrolWaypoint.zero();
	m_defenseQuietPatrolDeadlineFrame = 0;
	m_defensePlacementAttempt = 0;
	m_defensePlacementNextFrame = 0;
	m_defensePursuitTargetID = INVALID_ID;
	m_defensePursuitStartFrame = 0;
	m_defenseInterceptProbeAfterID = INVALID_ID;
	m_defensePatrolRouteMemberAfterID = INVALID_ID;
	m_defenseInterceptMemberAfterID = INVALID_ID;

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

const ThingTemplate *AISkirmishPlayer::findTunnelContainBuildTemplate() const
{
	if (!m_player || !IsSkirmishAISupportedGLASide(m_player->getSide()) ||
		!TheThingFactory)
		return nullptr;
	for (BuildListInfo *info = m_player->getBuildList(); info;
		info = info->getNext()) {
		const AsciiString name = info->getTemplateName();
		if (name.isEmpty())
			continue;
		const ThingTemplate *plan = TheThingFactory->findTemplate(name);
		if (IsSkirmishAIStrategyTunnelTemplate(plan))
			return plan;
	}
	return nullptr;
}

Bool AISkirmishPlayer::isTunnelBuildBuilderAvailable(Object *builder) const
{
	if (!builder || !m_player || !TheGameLogic ||
		builder->getControllingPlayer() != m_player ||
		!builder->isKindOf(KINDOF_DOZER) ||
		builder->isContained() ||
		builder->getID() == m_repairDozer ||
		builder->isEffectivelyDead() || builder->isDestroyed() ||
		builder->isDisabledByType(DISABLED_UNMANNED))
		return false;
	AIUpdateInterface *ai = builder->getAIUpdateInterface();
	DozerAIInterface *dozer = ai ? ai->getDozerAIInterface() : nullptr;
	if (!ai || !dozer || !ai->isIdle() || dozer->isAnyTaskPending())
		return false;
	SupplyTruckAIInterface *supply = ai->getSupplyTruckAIInterface();
	if (supply && (supply->isCurrentlyFerryingSupplies() ||
		supply->isForcedIntoWantingState()))
		return false;
	return true;
}

Bool AISkirmishPlayer::isPendingTunnelBuildInfo(
	const BuildListInfo *info, const ThingTemplate *plan) const
{
	if (!info ||
		(m_tunnelBuildPhase != SKIRMISH_AI_TUNNEL_BUILD_HOME_QUEUED &&
		 m_tunnelBuildPhase != SKIRMISH_AI_TUNNEL_BUILD_FORWARD_QUEUED))
		return false;
	const Coord3D *location = info->getLocation();
	const ThingTemplate *expected = findTunnelContainBuildTemplate();
	return plan && expected && plan->isEquivalentTo(expected) &&
		IsSkirmishAIStrategyTunnelTemplate(plan) &&
		info->getTemplateName() == plan->getName() && location &&
		location->x == m_tunnelBuildLocation.x &&
		location->y == m_tunnelBuildLocation.y;
}

Bool AISkirmishPlayer::validatePendingTunnelBuild(
	BuildListInfo *info, const ThingTemplate *plan, Object **builderOut,
	Bool *permanentFailure)
{
	if (builderOut)
		*builderOut = nullptr;
	if (permanentFailure)
		*permanentFailure = FALSE;
	if (!isPendingTunnelBuildInfo(info, plan)) {
		if (permanentFailure)
			*permanentFailure = TRUE;
		return false;
	}
	if (!m_player || !TheGameLogic || !TheAI || !TheAI->pathfinder() ||
		!TheBuildAssistant || !TheTerrainLogic)
		return false;
	const ThingTemplate *activeTunnelPlan = findTunnelContainBuildTemplate();
	if (!activeTunnelPlan || !plan->isEquivalentTo(activeTunnelPlan) ||
		!IsSkirmishAIStrategyTunnelTemplate(plan)) {
		if (permanentFailure)
			*permanentFailure = TRUE;
		return false;
	}
	Object *target = m_tunnelBuildTargetID != INVALID_ID
		? TheGameLogic->findObjectByID(m_tunnelBuildTargetID) : nullptr;
	Object *blocker = m_tunnelBuildBlockerID != INVALID_ID
		? TheGameLogic->findObjectByID(m_tunnelBuildBlockerID) : nullptr;
	if (!IsSkirmishAIStrategyTunnelTargetUsable(
			target, m_player, m_currentEnemy) ||
		m_strategyState.currentMode != SKIRMISH_STRATEGY_ASSAULT ||
		m_strategyState.strategicTargetID != m_tunnelBuildTargetID ||
		!blocker || blocker->getControllingPlayer() != m_currentEnemy ||
		!blocker->isKindOf(KINDOF_FS_BASE_DEFENSE) ||
		blocker->isEffectivelyDead() || blocker->isDestroyed() ||
		blocker->testStatus(OBJECT_STATUS_SOLD) ||
		!IsSkirmishStrategyIntelEligible(blocker, m_player) ||
		IsSkirmishStrategyFrameReached(
			TheGameLogic->getFrame(), m_tunnelBuildDeadlineFrame)) {
		if (permanentFailure)
			*permanentFailure = TRUE;
		return false;
	}
	Coord3D buildPosition = *info->getLocation();
	buildPosition.z = TheTerrainLogic->getGroundHeight(
		buildPosition.x, buildPosition.y);
	std::vector<Object *> builders;
	for (Object *candidate = TheGameLogic->getFirstObject(); candidate;
		candidate = candidate->getNextObject()) {
		if (isTunnelBuildBuilderAvailable(candidate))
			builders.push_back(candidate);
	}
	std::sort(builders.begin(), builders.end(),
		IsSkirmishAIStrategyObjectIDBefore);
	Object *builder = nullptr;
	const size_t builderLimit = min((size_t)8, builders.size());
	const size_t builderStart = builders.empty() ? 0 :
		(size_t)(m_tunnelPendingBuilderCursor % (UnsignedInt)builders.size());
	m_tunnelPendingBuilderCursor += (UnsignedInt)builderLimit;
	for (size_t pass = 0; pass < builderLimit; ++pass) {
		Object *candidate = builders[(builderStart + pass) % builders.size()];
		AIUpdateInterface *candidateAI = candidate->getAIUpdateInterface();
		if (!candidateAI ||
			TheBuildAssistant->canMakeUnit(candidate, plan) != CANMAKE_OK ||
			!TheAI->pathfinder()->clientSafeQuickDoesPathExist(
				candidateAI->getLocomotorSet(), candidate->getPosition(),
				&buildPosition))
			continue;
		builder = candidate;
		break;
	}
	if (!builder)
		return false;
	m_tunnelBuildBuilderID = builder->getID();
	if (!IsSkirmishAIStrategyTunnelSiteVisible(
			m_player, *info->getLocation())) {
		if (permanentFailure)
			*permanentFailure = TRUE;
		return false;
	}
	Object *forwardBlocker = m_tunnelBuildPhase ==
		SKIRMISH_AI_TUNNEL_BUILD_FORWARD_QUEUED ? blocker : nullptr;
	if ((forwardBlocker
		? !IsSkirmishAIForwardTunnelLocationSafe(m_player, m_currentEnemy,
			&buildPosition, plan, forwardBlocker, target)
		: !isLocationSafe(&buildPosition, plan)) ||
		TheBuildAssistant->isLocationLegalToBuild(
			&buildPosition, plan, info->getAngle(),
			BuildAssistant::CLEAR_PATH |
			BuildAssistant::TERRAIN_RESTRICTIONS |
			BuildAssistant::NO_OBJECT_OVERLAP,
			builder, m_player) != LBC_OK) {
		if (TheTerrainVisual)
			TheTerrainVisual->removeAllBibs();
		if (permanentFailure)
			*permanentFailure = TRUE;
		return false;
	}
	if (TheTerrainVisual)
		TheTerrainVisual->removeAllBibs();
	if (builderOut)
		*builderOut = builder;
	return true;
}

void AISkirmishPlayer::abandonTunnelBuildPlan(
	UnsignedInt now, Bool allowRetry)
{
	const Bool homeEndpoint =
		m_tunnelBuildPhase == SKIRMISH_AI_TUNNEL_BUILD_HOME_QUEUED ||
		m_tunnelBuildPhase == SKIRMISH_AI_TUNNEL_BUILD_HOME_CONSTRUCTING;
	const Bool forwardEndpoint =
		m_tunnelBuildPhase == SKIRMISH_AI_TUNNEL_BUILD_FORWARD_QUEUED ||
		m_tunnelBuildPhase == SKIRMISH_AI_TUNNEL_BUILD_FORWARD_CONSTRUCTING;
	if (m_tunnelBuildPhase == SKIRMISH_AI_TUNNEL_BUILD_HOME_QUEUED ||
		m_tunnelBuildPhase == SKIRMISH_AI_TUNNEL_BUILD_FORWARD_QUEUED) {
		for (BuildListInfo *info = m_player ? m_player->getBuildList() : nullptr;
			info; info = info->getNext()) {
			const ThingTemplate *plan = TheThingFactory
				? TheThingFactory->findTemplate(info->getTemplateName()) : nullptr;
			if (isPendingTunnelBuildInfo(info, plan) &&
				info->getObjectID() == INVALID_ID)
				info->setNumRebuilds(0);
		}
	}
	if (homeEndpoint && allowRetry && !m_tunnelHomeAttempted) {
		m_tunnelBuildPhase = SKIRMISH_AI_TUNNEL_BUILD_HOME_RETRY_AVAILABLE;
		m_tunnelBuildBuilderID = INVALID_ID;
		m_tunnelBuildBlockerID = INVALID_ID;
		m_tunnelBuildObjectID = INVALID_ID;
		m_tunnelBuildDeadlineFrame = 0;
		m_tunnelBuildLocation.zero();
	} else if (forwardEndpoint &&
		SkirmishAITunnelRoute::ShouldOfferForwardRetry(
			allowRetry, m_tunnelForwardRetryConsumed)) {
		m_tunnelBuildPhase = SKIRMISH_AI_TUNNEL_BUILD_FORWARD_RETRY_AVAILABLE;
		m_tunnelBuildBuilderID = INVALID_ID;
		m_tunnelBuildBlockerID = INVALID_ID;
		m_tunnelBuildObjectID = INVALID_ID;
		m_tunnelBuildDeadlineFrame = 0;
		m_tunnelBuildLocation.zero();
	} else {
		if (homeEndpoint)
			m_tunnelHomeAttempted = TRUE;
		else if (forwardEndpoint)
		{
			m_tunnelForwardAttempted = TRUE;
			SkirmishAITunnelRoute::RememberExhaustedForwardTarget(
				m_tunnelBuildTargetID, m_tunnelExhaustedForwardTargetIDs);
		}
		m_tunnelBuildPhase = SKIRMISH_AI_TUNNEL_BUILD_NONE;
		m_tunnelBuildTargetID = INVALID_ID;
		m_tunnelBuildBuilderID = INVALID_ID;
		m_tunnelBuildBlockerID = INVALID_ID;
		m_tunnelBuildObjectID = INVALID_ID;
		m_tunnelBuildDeadlineFrame = 0;
		m_tunnelBuildLocation.zero();
	}
	m_tunnelBuildCooldownUntilFrame = now +
		SKIRMISH_AI_TUNNEL_RETRY_COOLDOWN_SECONDS * LOGICFRAMES_PER_SECOND;
}

Bool AISkirmishPlayer::tryQueueTunnelEndpoint(
	Team *team, Object *target, Object *blockingDefense,
	Bool homeEndpoint, UnsignedInt now,
	Int *attemptPathQueryCount, Int *aggregatePathQueryCount,
	TacticalTeamState &state, Bool *deferred)
{
	if (deferred) *deferred = FALSE;
	const Int retryPhase = homeEndpoint
		? SKIRMISH_AI_TUNNEL_BUILD_HOME_RETRY_AVAILABLE
		: SKIRMISH_AI_TUNNEL_BUILD_FORWARD_RETRY_AVAILABLE;
	const Bool retryAvailable = m_tunnelBuildPhase == retryPhase;
	const Bool forwardRetryForTarget = !homeEndpoint &&
		SkirmishAITunnelRoute::IsForwardRetryForTarget(
			retryAvailable, m_tunnelBuildTargetID,
			target ? target->getID() : INVALID_ID);
	if (m_tunnelBuildObjectID != INVALID_ID && TheGameLogic) {
		Object *scaffold = TheGameLogic->findObjectByID(m_tunnelBuildObjectID);
		if (scaffold && !scaffold->isEffectivelyDead() &&
			!scaffold->isDestroyed()) {
			if (!IsSkirmishStrategyFrameReached(
					now, m_tunnelBuildDeadlineFrame)) {
				if (deferred && target &&
					m_tunnelBuildTargetID == target->getID())
					*deferred = TRUE;
				return false;
			}
			state.tunnelScaffoldWaitObjectID = m_tunnelBuildObjectID;
			state.tunnelScaffoldWaitUntilFrame = m_tunnelBuildDeadlineFrame;
		}
	}
	if (!team || !target || !blockingDefense || !m_player ||
		!attemptPathQueryCount || !aggregatePathQueryCount ||
		!TheGameLogic || !TheTerrainLogic || !TheBuildAssistant || !TheAI ||
		!TheAI->pathfinder() || !IsSkirmishAISupportedGLASide(m_player->getSide()) ||
		(m_tunnelBuildPhase != SKIRMISH_AI_TUNNEL_BUILD_NONE && !retryAvailable) ||
		(homeEndpoint && m_tunnelHomeAttempted && !retryAvailable) ||
		!IsSkirmishAIStrategyTunnelTargetUsable(
			target, m_player, m_currentEnemy) ||
		m_strategyState.strategicTargetID != target->getID() ||
		blockingDefense->getControllingPlayer() != m_currentEnemy ||
		!blockingDefense->isKindOf(KINDOF_FS_BASE_DEFENSE) ||
		blockingDefense->isEffectivelyDead() || blockingDefense->isDestroyed() ||
		blockingDefense->testStatus(OBJECT_STATUS_SOLD) ||
		!IsSkirmishStrategyIntelEligible(blockingDefense, m_player))
		return false;
	if (!IsSkirmishStrategyFrameReached(now,
		m_tunnelBuildCooldownUntilFrame)) {
		if (deferred) *deferred = TRUE;
		return false;
	}

	const ThingTemplate *plan = findTunnelContainBuildTemplate();
	if (!plan || !m_baseCenterSet || !IsSkirmishAIStrategyTunnelTemplate(plan))
		return false;

	TunnelTracker *tracker = m_player->getTunnelSystem();
	if (!tracker)
		return false;
	std::vector<Object *> endpoints;
	const std::list<ObjectID> *registeredIDs = tracker->getContainerList();
	if (registeredIDs) {
		for (std::list<ObjectID>::const_iterator id = registeredIDs->begin();
			id != registeredIDs->end(); ++id) {
			Object *object = TheGameLogic->findObjectByID(*id);
			if (IsSkirmishAIStrategyTunnelEndpointLive(object, m_player))
				endpoints.push_back(object);
		}
	}
	std::sort(endpoints.begin(), endpoints.end(),
		IsSkirmishAIStrategyObjectIDBefore);
	if (!homeEndpoint) {
		Bool latestEndpointLive = FALSE;
		for (size_t i = 0; i < endpoints.size(); ++i)
			if (endpoints[i]->getID() == m_tunnelForwardEndpointID) {
				latestEndpointLive = TRUE;
				break;
			}
		if (!SkirmishAITunnelRoute::CanAttemptForwardEndpoint(
				m_tunnelForwardAttempted, m_tunnelForwardAttemptTargetID,
				m_tunnelForwardEndpointID, latestEndpointLive,
				target->getID(), m_tunnelGeneratedForwardEndpointIDs,
				m_tunnelGeneratedForwardTargetIDs, forwardRetryForTarget,
				m_tunnelExhaustedForwardTargetIDs))
			return false;
	}
	const Bool replacingHome = homeEndpoint &&
		m_tunnelHomeEndpointID == INVALID_ID &&
		endpoints.size() == 1 &&
		SkirmishAITunnelRoute::IsTrackedGeneratedForwardEndpoint(
			endpoints[0]->getID(),
			m_tunnelGeneratedForwardEndpointIDs);
	if (homeEndpoint && !endpoints.empty() && !replacingHome)
		return false;
	if (!homeEndpoint && endpoints.empty())
		return false;
	if (!homeEndpoint) {
		// A stock or scripted scaffold may be alive before TunnelTracker
		// registers it. Do not queue a third endpoint beside that scaffold.
		ObjectID unregisteredID = INVALID_ID;
		for (Object *object = TheGameLogic->getFirstObject(); object;
			object = object->getNextObject()) {
			if (object->getControllingPlayer() != m_player ||
				object->isEffectivelyDead() || object->isDestroyed() ||
				object->testStatus(OBJECT_STATUS_SOLD) ||
				!IsSkirmishAIStrategyTunnelTemplate(object->getTemplate()))
				continue;
			Bool registered = FALSE;
			for (size_t i = 0; i < endpoints.size(); ++i)
				if (endpoints[i]->getID() == object->getID()) {
					registered = TRUE;
					break;
				}
			if (!registered && (unregisteredID == INVALID_ID ||
				object->getID() < unregisteredID))
				unregisteredID = object->getID();
		}
		if (unregisteredID != INVALID_ID) {
			if (state.tunnelScaffoldWaitObjectID != unregisteredID) {
				state.tunnelScaffoldWaitObjectID = unregisteredID;
				state.tunnelScaffoldWaitUntilFrame = now +
					120 * LOGICFRAMES_PER_SECOND;
			}
			if (!IsSkirmishStrategyFrameReached(
					now, state.tunnelScaffoldWaitUntilFrame)) {
				if (deferred) *deferred = TRUE;
				return false;
			}
		} else {
			state.tunnelScaffoldWaitObjectID = INVALID_ID;
			state.tunnelScaffoldWaitUntilFrame = 0;
		}
	}
	Bool hasAvailableBuilder = FALSE;
	for (Object *object = TheGameLogic->getFirstObject(); object;
		object = object->getNextObject()) {
		if (isTunnelBuildBuilderAvailable(object)) {
			hasAvailableBuilder = TRUE;
			break;
		}
	}
	if (!hasAvailableBuilder) {
		if (!state.tunnelBuilderWaitUntilFrame)
			state.tunnelBuilderWaitUntilFrame = now +
				120 * LOGICFRAMES_PER_SECOND;
		if (deferred) *deferred = !IsSkirmishStrategyFrameReached(
			now, state.tunnelBuilderWaitUntilFrame);
		return false;
	}
	state.tunnelBuilderWaitUntilFrame = 0;

	// Match the exposed assault subset selected by tryTunnelBypass. Scripted
	// extras and members already contained do not veto this group's route.
	// A forward exit still requires every selected member to reach an entry.
	std::vector<Object *> members;
	for (DLINK_ITERATOR<Object> member = team->iterate_TeamMemberList();
		!member.done(); member.advance()) {
		Object *object = member.cur();
		if (!IsSkirmishAIStrategyTunnelTransitMember(
				object, m_player, team, false))
			continue;
		if (object->isKindOf(KINDOF_AIRCRAFT))
			return false;
		members.push_back(object);
	}
	std::sort(members.begin(), members.end(),
		IsSkirmishAIStrategyObjectIDBefore);
	if (members.empty() || members.size() > MAX_SKIRMISH_AI_TUNNEL_MEMBERS)
		return false;
	if (!homeEndpoint) {
		Bool reachableEntry = false;
		// Reserve half of the 256-query attempt for candidate sites and builders.
		UnsignedInt entrySignature = (UnsignedInt)endpoints.size();
		for (size_t i = 0; i < endpoints.size(); ++i) {
			const Coord3D *position = endpoints[i]->getPosition();
			entrySignature = entrySignature * 33u +
				(UnsignedInt)endpoints[i]->getID();
			entrySignature = entrySignature * 33u +
				(UnsignedInt)(Int)(position->x / 80.0f);
			entrySignature = entrySignature * 33u +
				(UnsignedInt)(Int)(position->y / 80.0f);
		}
		entrySignature = entrySignature * 33u + (UnsignedInt)members.size();
		for (size_t i = 0; i < members.size(); ++i) {
			const Coord3D *position = members[i]->getPosition();
			entrySignature = entrySignature * 33u +
				(UnsignedInt)members[i]->getID();
			entrySignature = entrySignature * 33u +
				(UnsignedInt)(Int)(position->x / 80.0f);
			entrySignature = entrySignature * 33u +
				(UnsignedInt)(Int)(position->y / 80.0f);
		}
		if (state.tunnelForwardEntryTargetID != target->getID()) {
			state.tunnelForwardEntryTargetID = target->getID();
			state.tunnelForwardEntrySignature = entrySignature;
			state.tunnelForwardEntryCursor = (UnsignedInt)team->getID();
			state.tunnelForwardEntryRemaining = (UnsignedInt)endpoints.size();
			state.tunnelForwardEntryRetryAfterFrame = 0;
		}
		if (state.tunnelForwardEntryRemaining > endpoints.size())
			state.tunnelForwardEntryRemaining = (UnsignedInt)endpoints.size();
		// Finish an active sweep even when the group moves. A failed sweep
		// refreshes after 15 seconds for changed paths, or after five seconds
		// when a member or endpoint crosses an 80-unit position cell.
		const Bool changedEntry =
			state.tunnelForwardEntrySignature != entrySignature;
		const Bool refreshEntry =
			SkirmishAITunnelRoute::ShouldRestartEndpointSweep(
				state.tunnelForwardEntryRemaining, now,
				state.tunnelForwardEntryRetryAfterFrame) ||
			(changedEntry &&
				SkirmishAITunnelRoute::ShouldRestartEndpointSweep(
					state.tunnelForwardEntryRemaining,
					now + 10 * LOGICFRAMES_PER_SECOND,
					state.tunnelForwardEntryRetryAfterFrame));
		if (refreshEntry) {
			state.tunnelForwardEntrySignature = entrySignature;
			state.tunnelForwardEntryRemaining = (UnsignedInt)endpoints.size();
			state.tunnelForwardEntryRetryAfterFrame = 0;
		}
		const size_t entryLimit = min((size_t)2,
			(size_t)state.tunnelForwardEntryRemaining);
		for (size_t probe = 0; probe < entryLimit &&
			!reachableEntry; ++probe) {
			const Int requiredQueries = (Int)(members.size() * 2);
			if (!SkirmishAITunnelRoute::CanReservePairQueries(
					requiredQueries, *attemptPathQueryCount,
					*aggregatePathQueryCount,
					MAX_SKIRMISH_AI_TUNNEL_PATH_QUERIES_PER_ATTEMPT,
					MAX_SKIRMISH_AI_TUNNEL_PATH_QUERIES_PER_UPDATE)) {
				if (deferred) *deferred = TRUE;
				return false;
			}
			const size_t endpointIndex =
				state.tunnelForwardEntryCursor % endpoints.size();
			Bool allReachable = true;
			for (size_t memberIndex = 0; memberIndex < members.size(); ++memberIndex) {
				AIUpdateInterface *ai = members[memberIndex]->getAIUpdateInterface();
				Bool memberReachable = FALSE;
				for (Int candidate = 0; candidate < 2 && ai &&
					!memberReachable; ++candidate) {
					Coord3D entryApproach;
					if (GetSkirmishAIStrategyGroundApproach(
							members[memberIndex]->getPosition(),
							endpoints[endpointIndex], candidate,
							&entryApproach))
						memberReachable = ProbeSkirmishAITunnelQuickPath(
							ai->getLocomotorSet(),
							members[memberIndex]->getPosition(), &entryApproach,
							attemptPathQueryCount, aggregatePathQueryCount);
				}
				if (!memberReachable) {
					allReachable = false;
					break;
				}
			}
			reachableEntry = allReachable;
			if (!reachableEntry)
				SkirmishAITunnelRoute::AdvanceEndpointSweep(
					&state.tunnelForwardEntryCursor,
					&state.tunnelForwardEntryRemaining);
		}
		if (!reachableEntry && state.tunnelForwardEntryRemaining) {
			m_tunnelBuildCooldownUntilFrame = now +
				SKIRMISH_AI_TUNNEL_PAIR_PROBE_SECONDS * LOGICFRAMES_PER_SECOND;
			if (deferred) *deferred = TRUE;
			return false;
		}
		if (!reachableEntry) {
			if (!state.tunnelForwardEntryRemaining &&
				!state.tunnelForwardEntryRetryAfterFrame) {
				state.tunnelForwardEntryRetryAfterFrame = now +
					15 * LOGICFRAMES_PER_SECOND;
				if (!state.tunnelForwardEntryRetryAfterFrame)
					state.tunnelForwardEntryRetryAfterFrame = 1;
			}
			return false;
		}
	}

	// Do not build a duplicate home endpoint when a stock/scripted tunnel is
	// already being placed but has not registered with TunnelTracker yet.
	if (homeEndpoint) {
		ObjectID unregisteredID = INVALID_ID;
		for (Object *object = TheGameLogic->getFirstObject(); object;
			object = object->getNextObject()) {
			if (object->getControllingPlayer() == m_player &&
				(!replacingHome || object->getID() !=
					endpoints[0]->getID()) &&
				!object->isEffectivelyDead() && !object->isDestroyed() &&
				IsSkirmishAIStrategyTunnelTemplate(object->getTemplate()) &&
				(unregisteredID == INVALID_ID ||
					object->getID() < unregisteredID))
				unregisteredID = object->getID();
		}
		if (unregisteredID != INVALID_ID) {
			if (state.tunnelScaffoldWaitObjectID != unregisteredID) {
				state.tunnelScaffoldWaitObjectID = unregisteredID;
				state.tunnelScaffoldWaitUntilFrame = now +
					120 * LOGICFRAMES_PER_SECOND;
			}
			if (!IsSkirmishStrategyFrameReached(
					now, state.tunnelScaffoldWaitUntilFrame)) {
				if (deferred) *deferred = TRUE;
				return false;
			}
		} else {
			state.tunnelScaffoldWaitObjectID = INVALID_ID;
			state.tunnelScaffoldWaitUntilFrame = 0;
		}
	}

	std::vector<Object *> builders;
	for (Object *object = TheGameLogic->getFirstObject(); object;
		object = object->getNextObject()) {
		if (isTunnelBuildBuilderAvailable(object))
			builders.push_back(object);
	}
	std::sort(builders.begin(), builders.end(),
		IsSkirmishAIStrategyObjectIDBefore);
	if (builders.empty())
	{
		if (!state.tunnelBuilderWaitUntilFrame)
			state.tunnelBuilderWaitUntilFrame = now +
				120 * LOGICFRAMES_PER_SECOND;
		if (deferred) *deferred = !IsSkirmishStrategyFrameReached(
			now, state.tunnelBuilderWaitUntilFrame);
		return false;
	}
	const size_t builderProbeCount = min((size_t)8, builders.size());
	UnsignedInt builderSignature = (UnsignedInt)builders.size();
	for (size_t builderIndex = 0; builderIndex < builders.size(); ++builderIndex)
		builderSignature = builderSignature * 33u +
			(UnsignedInt)builders[builderIndex]->getID();
	if (state.tunnelBuilderSignature != builderSignature ||
		!state.tunnelBuilderWindowsRemaining) {
		state.tunnelBuilderSignature = builderSignature;
		state.tunnelBuilderWindowsRemaining =
			SkirmishAITunnelRoute::BuilderWindowCount(
				(UnsignedInt)builders.size(), 8);
	}
	const size_t builderStart =
		(size_t)(state.tunnelBuilderCursor % (UnsignedInt)builders.size());

	const Real tunnelRadius = max(
		plan->getTemplateGeometryInfo().getBoundingCircleRadius(),
		PATHFIND_CELL_SIZE_F);
	Coord3D candidates[5];
	Int candidateCount = 0;
	if (homeEndpoint) {
		const Real offset = max(m_baseRadius, 0.0f) + tunnelRadius +
			2.0f * PATHFIND_CELL_SIZE_F;
		const Real offsets[5][2] = {
			{ 1.0f, 0.0f }, { 0.0f, 1.0f }, { -1.0f, 0.0f },
			{ 0.0f, -1.0f }, { 0.70710678f, 0.70710678f }
		};
		for (Int i = 0; i < 5; ++i) {
			candidates[candidateCount] = m_baseCenter;
			candidates[candidateCount].x += offsets[i][0] * offset;
			candidates[candidateCount].y += offsets[i][1] * offset;
			candidates[candidateCount].z = 0.0f;
			++candidateCount;
		}
	} else {
		const Real dx = target->getPosition()->x - blockingDefense->getPosition()->x;
		const Real dy = target->getPosition()->y - blockingDefense->getPosition()->y;
		const Real length = sqrt(dx * dx + dy * dy);
		if (length <= 1.0f)
			return false;
		const Real ux = dx / length;
		const Real uy = dy / length;
		const Real defenseRadius = blockingDefense->getTemplate()
			? blockingDefense->getTemplate()->getTemplateGeometryInfo()
				.getBoundingCircleRadius() : 0.0f;
		const Real targetRadius = target->getTemplate()
			? target->getTemplate()->getTemplateGeometryInfo()
				.getBoundingCircleRadius() : 0.0f;
		const Real forward = defenseRadius + tunnelRadius + PATHFIND_CELL_SIZE_F;
		const Real targetClearance = targetRadius + tunnelRadius + PATHFIND_CELL_SIZE_F;
		if (length <= forward + targetClearance)
			return false;
		const Real lateralOffsets[5] = { 0.0f, 2.0f * tunnelRadius,
			-2.0f * tunnelRadius, 4.0f * tunnelRadius,
			-4.0f * tunnelRadius };
		for (Int i = 0; i < 5; ++i) {
			candidates[candidateCount] = *blockingDefense->getPosition();
			candidates[candidateCount].x += ux * forward - uy * lateralOffsets[i];
			candidates[candidateCount].y += uy * forward + ux * lateralOffsets[i];
			candidates[candidateCount].z = 0.0f;
			++candidateCount;
		}
	}

	Bool eligibleSiteFound = FALSE;
	for (Int siteAttempt = 0; siteAttempt < candidateCount; ++siteAttempt) {
		const Int siteIndex = SkirmishAITunnelRoute::SelectSiteIndex(
			candidateCount, state.tunnelSiteCursor);
		if (*attemptPathQueryCount >=
			MAX_SKIRMISH_AI_TUNNEL_PATH_QUERIES_PER_ATTEMPT ||
			*aggregatePathQueryCount >=
			MAX_SKIRMISH_AI_TUNNEL_PATH_QUERIES_PER_UPDATE) {
			if (deferred) *deferred = TRUE;
			return false;
		}
		// A site may consume the full path budget; resume at the next one.
		++state.tunnelSiteCursor;
		Coord3D buildPosition = candidates[siteIndex];
		if (!IsSkirmishAIStrategyTunnelSiteVisible(m_player, buildPosition))
			continue;
		buildPosition.z = TheTerrainLogic->getGroundHeight(
			buildPosition.x, buildPosition.y);
		if (homeEndpoint
			? !isLocationSafe(&buildPosition, plan)
			: !IsSkirmishAIForwardTunnelLocationSafe(m_player,
				m_currentEnemy, &buildPosition, plan,
				blockingDefense, target))
			continue;
		Bool groupRoute = true;
		for (size_t memberIndex = 0; memberIndex < members.size(); ++memberIndex) {
			AIUpdateInterface *ai = members[memberIndex]->getAIUpdateInterface();
			Bool memberRoute = FALSE;
			for (Int candidate = 0; candidate < 2 && ai &&
				!memberRoute; ++candidate) {
				const Coord3D *approachOrigin = homeEndpoint
					? members[memberIndex]->getPosition() : target->getPosition();
				Coord3D tunnelApproach = buildPosition;
				if (!SkirmishAITunnelRoute::GetApproachPoint(
						buildPosition.x, buildPosition.y,
						approachOrigin->x, approachOrigin->y,
						tunnelRadius, 2.0f * PATHFIND_CELL_SIZE_F,
						candidate, &tunnelApproach.x, &tunnelApproach.y))
					continue;
				tunnelApproach.z = TheTerrainLogic->getGroundHeight(
					tunnelApproach.x, tunnelApproach.y);
				Coord3D targetApproach;
				if (!homeEndpoint && !GetSkirmishAIStrategyGroundApproach(
						&buildPosition, target, candidate, &targetApproach))
					continue;
				memberRoute = ProbeSkirmishAITunnelQuickPath(
					ai->getLocomotorSet(),
					homeEndpoint ? members[memberIndex]->getPosition()
						: &tunnelApproach,
					homeEndpoint ? &tunnelApproach : &targetApproach,
					attemptPathQueryCount, aggregatePathQueryCount);
			}
			if (!memberRoute) {
				if (*attemptPathQueryCount >=
					MAX_SKIRMISH_AI_TUNNEL_PATH_QUERIES_PER_ATTEMPT ||
					*aggregatePathQueryCount >=
					MAX_SKIRMISH_AI_TUNNEL_PATH_QUERIES_PER_UPDATE) {
					if (deferred) *deferred = TRUE;
					return false;
				}
				groupRoute = false;
				break;
			}
		}
		if (!groupRoute)
			continue;
		eligibleSiteFound = TRUE;
		for (size_t builderIndex = 0; builderIndex < builderProbeCount;
			++builderIndex) {
			if (*attemptPathQueryCount >=
				MAX_SKIRMISH_AI_TUNNEL_PATH_QUERIES_PER_ATTEMPT ||
				*aggregatePathQueryCount >=
				MAX_SKIRMISH_AI_TUNNEL_PATH_QUERIES_PER_UPDATE) {
				if (deferred) *deferred = TRUE;
				return false;
			}
			Object *builder = builders[(builderStart + builderIndex) %
				builders.size()];
			if (!isTunnelBuildBuilderAvailable(builder) ||
				TheBuildAssistant->canMakeUnit(builder, plan) != CANMAKE_OK)
				continue;
			AIUpdateInterface *builderAI = builder->getAIUpdateInterface();
			if (!builderAI || !ProbeSkirmishAITunnelQuickPath(
					builderAI->getLocomotorSet(), builder->getPosition(),
					&buildPosition, attemptPathQueryCount,
					aggregatePathQueryCount))
				continue;
			const Bool legal = TheBuildAssistant->isLocationLegalToBuild(
				&buildPosition, plan, plan->getPlacementViewAngle(),
				BuildAssistant::CLEAR_PATH |
				BuildAssistant::TERRAIN_RESTRICTIONS |
				BuildAssistant::NO_OBJECT_OVERLAP,
				builder, m_player) == LBC_OK;
			if (TheTerrainVisual)
				TheTerrainVisual->removeAllBibs();
			if (!legal)
				continue;

			Coord3D queuedPosition = candidates[siteIndex];
			queuedPosition.z = 0.0f;
			m_player->addToPriorityBuildList(
				plan->getName(), &queuedPosition, plan->getPlacementViewAngle());
			m_tunnelBuildPhase = homeEndpoint
				? SKIRMISH_AI_TUNNEL_BUILD_HOME_QUEUED
				: SKIRMISH_AI_TUNNEL_BUILD_FORWARD_QUEUED;
			if (homeEndpoint && retryAvailable)
				m_tunnelHomeAttempted = TRUE;
			if (!homeEndpoint) {
				m_tunnelForwardRetryConsumed = forwardRetryForTarget;
				if (forwardRetryForTarget)
					m_tunnelForwardAttempted = TRUE;
			}
			m_tunnelBuildBuilderID = builder->getID();
			m_tunnelPendingBuilderCursor = 0;
			m_tunnelBuildTargetID = target->getID();
			if (!homeEndpoint)
				m_tunnelForwardAttemptTargetID = target->getID();
			m_tunnelBuildBlockerID = blockingDefense->getID();
			m_tunnelBuildObjectID = INVALID_ID;
			m_tunnelBuildLocation = queuedPosition;
			m_tunnelBuildDeadlineFrame = now +
				120 * LOGICFRAMES_PER_SECOND;
			m_tunnelBuildCooldownUntilFrame = now;
			return true;
		}
	}
	if (eligibleSiteFound && builders.size() > builderProbeCount &&
		state.tunnelBuilderWindowsRemaining > 1) {
		--state.tunnelBuilderWindowsRemaining;
		state.tunnelBuilderCursor += (UnsignedInt)builderProbeCount;
		m_tunnelBuildCooldownUntilFrame = now +
			SKIRMISH_AI_TUNNEL_PAIR_PROBE_SECONDS * LOGICFRAMES_PER_SECOND;
		if (deferred) *deferred = TRUE;
		return false;
	}
	state.tunnelBuilderWindowsRemaining = 0;
	m_tunnelBuildCooldownUntilFrame = now +
		SKIRMISH_AI_TUNNEL_RETRY_COOLDOWN_SECONDS * LOGICFRAMES_PER_SECOND;
	return false;
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
	if (m_defenseBuildLockedBuilderID != INVALID_ID && pos &&
		pos->x == m_defenseBuildLockedLocation.x &&
		pos->y == m_defenseBuildLockedLocation.y) {
		Object *builder = TheGameLogic
			? TheGameLogic->findObjectByID(m_defenseBuildLockedBuilderID) : nullptr;
		return builder && builder->getControllingPlayer() == m_player &&
			!builder->isEffectivelyDead() && !builder->isDestroyed()
			? builder : nullptr;
	}
	if (m_tunnelBuildLockedBuilderID != INVALID_ID && pos &&
		pos->x == m_tunnelBuildLocation.x &&
		pos->y == m_tunnelBuildLocation.y) {
		Object *builder = TheGameLogic
			? TheGameLogic->findObjectByID(m_tunnelBuildLockedBuilderID) : nullptr;
		return isTunnelBuildBuilderAvailable(builder) ? builder : nullptr;
	}
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
void AISkirmishPlayer::xferTunnelEndpointProbes(
	Xfer *xfer, TacticalTeamState &state)
{
	xfer->xferUnsignedInt(&state.tunnelPairRefreshCursor);
	xfer->xferUnsignedInt(&state.tunnelPairRefreshRemaining);
	xfer->xferUnsignedInt(&state.tunnelPairRefreshAfterFrame);
	xfer->xferUnsignedInt(&state.tunnelPairRefreshCacheAfterFrame);
	xfer->xferBool(&state.tunnelPairRefreshYieldMain);
	xfer->xferUnsignedInt(&state.tunnelProbeMovementSignature);
	xfer->xferUnsignedInt(&state.tunnelScaffoldWaitUntilFrame);
	xfer->xferObjectID(&state.tunnelScaffoldWaitObjectID);
	xfer->xferInt(&state.tunnelProbeMemberCount);
	for (Int i = 0; i < MAX_SKIRMISH_AI_TUNNEL_MEMBERS; ++i)
		xfer->xferObjectID(&state.tunnelProbeMemberIDs[i]);
	UnsignedInt count = (UnsignedInt)state.tunnelEndpointProbes.size();
	xfer->xferUnsignedInt(&count);
	if (count > MAX_SKIRMISH_AI_TUNNEL_ENDPOINT_PROBES)
		throw XFER_INVALID_PARAMETERS;
	if (xfer->getXferMode() == XFER_LOAD)
		state.tunnelEndpointProbes.clear();
	for (UnsignedInt i = 0; i < count; ++i) {
		TacticalTeamState::TunnelEndpointProbe probe;
		if (xfer->getXferMode() != XFER_LOAD)
			probe = state.tunnelEndpointProbes[i];
		xfer->xferObjectID(&probe.objectID);
		xfer->xferInt(&probe.entryResult);
		xfer->xferInt(&probe.exitResult);
		if (xfer->getXferMode() == XFER_LOAD)
			state.tunnelEndpointProbes.push_back(probe);
	}
	if (xfer->getXferMode() == XFER_LOAD &&
		(state.tunnelProbeMemberCount < 0 ||
		 state.tunnelProbeMemberCount > MAX_SKIRMISH_AI_TUNNEL_MEMBERS)) {
		state.tunnelProbeMemberCount = 0;
		state.tunnelEndpointProbes.clear();
	}
}

void AISkirmishPlayer::xferGeneratedDefenseBuilds(Xfer *xfer)
{
	UnsignedInt count = (UnsignedInt)m_generatedDefenseBuilds.size();
	xfer->xferUnsignedInt(&count);
	if (count > 5)
		throw XFER_INVALID_PARAMETERS;
	if (xfer->getXferMode() == XFER_LOAD)
		m_generatedDefenseBuilds.clear();
	for (UnsignedInt i = 0; i < count; ++i) {
		GeneratedDefenseBuild build;
		if (xfer->getXferMode() != XFER_LOAD)
			build = m_generatedDefenseBuilds[i];
		xfer->xferAsciiString(&build.templateName);
		xfer->xferReal(&build.location.x);
		xfer->xferReal(&build.location.y);
		xfer->xferReal(&build.angle);
		if (xfer->getXferMode() == XFER_LOAD && i < 5)
			m_generatedDefenseBuilds.push_back(build);
	}
}

void AISkirmishPlayer::xferTacticalTeams(Xfer *xfer, XferVersion version)
{
	UnsignedInt count = static_cast<UnsignedInt>(m_tacticalTeams.size());
	xfer->xferUnsignedInt(&count);
	if (count > MAX_SKIRMISH_AI_TACTICAL_TEAMS)
		throw XFER_INVALID_PARAMETERS;
	if (xfer->getXferMode() == XFER_LOAD)
		m_tacticalTeams.clear();
	if (xfer->getXferMode() == XFER_SAVE) {
		for (std::map<UnsignedInt, TacticalTeamState>::iterator it =
				m_tacticalTeams.begin(); it != m_tacticalTeams.end(); ++it) {
			UnsignedInt teamID = it->first;
			TacticalTeamState &state = it->second;
			xfer->xferUnsignedInt(&teamID);
			xfer->xferObjectID(&state.targetID);
			xfer->xferReal(&state.targetHealth);
			xfer->xferReal(&state.distanceToTargetSqr);
			xfer->xferUnsignedInt(&state.lastProgressFrame);
			xfer->xferUnsignedInt(&state.nextCheckFrame);
			xfer->xferUnsignedInt(&state.regroupUntilFrame);
			xfer->xferUnsignedInt(&state.blockedSinceFrame);
			xfer->xferUnsignedInt(&state.retreatStartFrame);
			xfer->xferUnsignedInt(&state.nextRetreatFrame);
			if (version >= 15)
				xfer->xferUnsignedInt(&state.retreatProbeCursor);
			xfer->xferUnsignedInt(&state.routeExhaustedUntilFrame);
			xfer->xferObjectID(&state.routeExhaustedTargetID);
			xfer->xferObjectID(&state.alternateProbeAfterID);
			xfer->xferObjectID(&state.alternateAttackIssuedTargetID);
			xfer->xferInt(&state.approachAttempt);
			xfer->xferBool(&state.retreating);
			xfer->xferBool(&state.woundedReserve);
			xfer->xferInt(&state.tunnelTransitPhase);
			xfer->xferInt(&state.tunnelMemberCount);
			for (Int memberIndex = 0; memberIndex < MAX_SKIRMISH_AI_TUNNEL_MEMBERS;
				++memberIndex)
				xfer->xferObjectID(&state.tunnelMemberIDs[memberIndex]);
			xfer->xferUnsignedInt(&state.tunnelPairCursor);
			xfer->xferUnsignedInt(&state.tunnelPairSweepRemaining);
			xfer->xferUnsignedInt(&state.tunnelPairSweepStartFrame);
			xfer->xferUnsignedInt(&state.tunnelPairResumeAfterFrame);
			xfer->xferUnsignedInt(&state.tunnelPairRetryAfterFrame);
			xfer->xferUnsignedInt(&state.tunnelPairEndpointSignature);
			xfer->xferObjectID(&state.tunnelPairSweepTargetID);
			xfer->xferObjectID(&state.tunnelWaitTargetID);
			xfer->xferUnsignedInt(&state.tunnelSiteCursor);
			xfer->xferUnsignedInt(&state.tunnelBuilderWaitUntilFrame);
			xfer->xferUnsignedInt(&state.tunnelBuilderCursor);
			xfer->xferUnsignedInt(&state.tunnelBuilderSignature);
			xfer->xferUnsignedInt(&state.tunnelBuilderWindowsRemaining);
			xfer->xferUnsignedInt(&state.tunnelForwardEntryCursor);
			xfer->xferUnsignedInt(&state.tunnelForwardEntryRemaining);
			xfer->xferUnsignedInt(&state.tunnelForwardEntrySignature);
			xfer->xferObjectID(&state.tunnelForwardEntryTargetID);
			xfer->xferUnsignedInt(&state.tunnelForwardEntryRetryAfterFrame);
			xfer->xferObjectID(&state.tunnelEntryID);
			xfer->xferObjectID(&state.tunnelExitID);
			xfer->xferObjectID(&state.tunnelTargetID);
			xfer->xferObjectID(&state.tunnelStrategicTargetID);
			xfer->xferUnsignedInt(&state.tunnelPhaseDeadlineFrame);
			xfer->xferUnsignedInt(&state.tunnelCooldownUntilFrame);
			if (version >= 14)
				xferTunnelEndpointProbes(xfer, state);
			if (version >= 18) {
				xfer->xferObjectID(&state.tunnelCapacityWaitTargetID);
				xfer->xferUnsignedInt(&state.tunnelCapacityWaitDeadlineFrame);
			}
			if (version >= 19)
				xfer->xferUnsignedInt(
					&state.tunnelCommittedTargetLastSeenFrame);
		}
	} else {
		for (UnsignedInt i = 0; i < count; ++i) {
			UnsignedInt teamID = 0;
			TacticalTeamState state;
			xfer->xferUnsignedInt(&teamID);
			xfer->xferObjectID(&state.targetID);
			xfer->xferReal(&state.targetHealth);
			xfer->xferReal(&state.distanceToTargetSqr);
			xfer->xferUnsignedInt(&state.lastProgressFrame);
			xfer->xferUnsignedInt(&state.nextCheckFrame);
			xfer->xferUnsignedInt(&state.regroupUntilFrame);
			xfer->xferUnsignedInt(&state.blockedSinceFrame);
			xfer->xferUnsignedInt(&state.retreatStartFrame);
			xfer->xferUnsignedInt(&state.nextRetreatFrame);
			if (version >= 15)
				xfer->xferUnsignedInt(&state.retreatProbeCursor);
			xfer->xferUnsignedInt(&state.routeExhaustedUntilFrame);
			xfer->xferObjectID(&state.routeExhaustedTargetID);
			xfer->xferObjectID(&state.alternateProbeAfterID);
			xfer->xferObjectID(&state.alternateAttackIssuedTargetID);
			xfer->xferInt(&state.approachAttempt);
			xfer->xferBool(&state.retreating);
			xfer->xferBool(&state.woundedReserve);
			xfer->xferInt(&state.tunnelTransitPhase);
			xfer->xferInt(&state.tunnelMemberCount);
			for (Int memberIndex = 0; memberIndex < MAX_SKIRMISH_AI_TUNNEL_MEMBERS;
				++memberIndex)
				xfer->xferObjectID(&state.tunnelMemberIDs[memberIndex]);
			xfer->xferUnsignedInt(&state.tunnelPairCursor);
			xfer->xferUnsignedInt(&state.tunnelPairSweepRemaining);
			xfer->xferUnsignedInt(&state.tunnelPairSweepStartFrame);
			xfer->xferUnsignedInt(&state.tunnelPairResumeAfterFrame);
			xfer->xferUnsignedInt(&state.tunnelPairRetryAfterFrame);
			xfer->xferUnsignedInt(&state.tunnelPairEndpointSignature);
			xfer->xferObjectID(&state.tunnelPairSweepTargetID);
			xfer->xferObjectID(&state.tunnelWaitTargetID);
			xfer->xferUnsignedInt(&state.tunnelSiteCursor);
			xfer->xferUnsignedInt(&state.tunnelBuilderWaitUntilFrame);
			xfer->xferUnsignedInt(&state.tunnelBuilderCursor);
			xfer->xferUnsignedInt(&state.tunnelBuilderSignature);
			xfer->xferUnsignedInt(&state.tunnelBuilderWindowsRemaining);
			xfer->xferUnsignedInt(&state.tunnelForwardEntryCursor);
			xfer->xferUnsignedInt(&state.tunnelForwardEntryRemaining);
			xfer->xferUnsignedInt(&state.tunnelForwardEntrySignature);
			xfer->xferObjectID(&state.tunnelForwardEntryTargetID);
			xfer->xferUnsignedInt(&state.tunnelForwardEntryRetryAfterFrame);
			xfer->xferObjectID(&state.tunnelEntryID);
			xfer->xferObjectID(&state.tunnelExitID);
			xfer->xferObjectID(&state.tunnelTargetID);
			xfer->xferObjectID(&state.tunnelStrategicTargetID);
			xfer->xferUnsignedInt(&state.tunnelPhaseDeadlineFrame);
			xfer->xferUnsignedInt(&state.tunnelCooldownUntilFrame);
			if (version >= 14)
				xferTunnelEndpointProbes(xfer, state);
			if (version >= 18) {
				xfer->xferObjectID(&state.tunnelCapacityWaitTargetID);
				xfer->xferUnsignedInt(&state.tunnelCapacityWaitDeadlineFrame);
			}
			if (version >= 19)
				xfer->xferUnsignedInt(
					&state.tunnelCommittedTargetLastSeenFrame);
			// Team instances can be restored after the player snapshot.
			if (teamID) {
				if (state.approachAttempt < 0 || state.approachAttempt > 4)
					state.approachAttempt = 0;
				if (state.tunnelTransitPhase <
						SKIRMISH_AI_TUNNEL_TRANSIT_NONE ||
					state.tunnelTransitPhase >
						SKIRMISH_AI_TUNNEL_TRANSIT_FALLBACK_EXIT ||
					state.tunnelMemberCount < 0 ||
					state.tunnelMemberCount > MAX_SKIRMISH_AI_TUNNEL_MEMBERS) {
					state.tunnelTransitPhase =
						SKIRMISH_AI_TUNNEL_TRANSIT_NONE;
					state.tunnelMemberCount = 0;
				}
				if (state.tunnelTransitPhase ==
						SKIRMISH_AI_TUNNEL_TRANSIT_NONE) {
					for (Int memberIndex = 0;
						memberIndex < MAX_SKIRMISH_AI_TUNNEL_MEMBERS;
						++memberIndex)
						state.tunnelMemberIDs[memberIndex] = INVALID_ID;
					state.tunnelEntryID = INVALID_ID;
					state.tunnelExitID = INVALID_ID;
					state.tunnelTargetID = INVALID_ID;
					state.tunnelStrategicTargetID = INVALID_ID;
					state.tunnelCommittedTargetLastSeenFrame = 0;
					state.tunnelPhaseDeadlineFrame = 0;
				}
				m_tacticalTeams[teamID] = state;
			}
		}
	}
}

void AISkirmishPlayer::crcTacticalTeams(Xfer *xfer)
{
	UnsignedInt count = static_cast<UnsignedInt>(m_tacticalTeams.size());
	xfer->xferUnsignedInt(&count);
	for (std::map<UnsignedInt, TacticalTeamState>::iterator it =
			m_tacticalTeams.begin(); it != m_tacticalTeams.end(); ++it) {
		UnsignedInt teamID = it->first;
		TacticalTeamState &state = it->second;
		xfer->xferUnsignedInt(&teamID);
		xfer->xferObjectID(&state.targetID);
		xfer->xferReal(&state.targetHealth);
		xfer->xferReal(&state.distanceToTargetSqr);
		xfer->xferUnsignedInt(&state.lastProgressFrame);
		xfer->xferUnsignedInt(&state.nextCheckFrame);
		xfer->xferUnsignedInt(&state.regroupUntilFrame);
		xfer->xferUnsignedInt(&state.blockedSinceFrame);
		xfer->xferUnsignedInt(&state.retreatStartFrame);
		xfer->xferUnsignedInt(&state.nextRetreatFrame);
		xfer->xferUnsignedInt(&state.retreatProbeCursor);
		xfer->xferUnsignedInt(&state.routeExhaustedUntilFrame);
		xfer->xferObjectID(&state.routeExhaustedTargetID);
		xfer->xferObjectID(&state.alternateProbeAfterID);
		xfer->xferObjectID(&state.alternateAttackIssuedTargetID);
		xfer->xferInt(&state.approachAttempt);
		xfer->xferBool(&state.retreating);
		xfer->xferBool(&state.woundedReserve);
		xfer->xferInt(&state.tunnelTransitPhase);
		xfer->xferInt(&state.tunnelMemberCount);
		for (Int memberIndex = 0; memberIndex < MAX_SKIRMISH_AI_TUNNEL_MEMBERS;
			++memberIndex)
			xfer->xferObjectID(&state.tunnelMemberIDs[memberIndex]);
		xfer->xferUnsignedInt(&state.tunnelPairCursor);
		xfer->xferUnsignedInt(&state.tunnelPairSweepRemaining);
		xfer->xferUnsignedInt(&state.tunnelPairSweepStartFrame);
		xfer->xferUnsignedInt(&state.tunnelPairResumeAfterFrame);
		xfer->xferUnsignedInt(&state.tunnelPairRetryAfterFrame);
		xfer->xferUnsignedInt(&state.tunnelPairEndpointSignature);
		xfer->xferObjectID(&state.tunnelPairSweepTargetID);
		xfer->xferObjectID(&state.tunnelWaitTargetID);
		xfer->xferUnsignedInt(&state.tunnelSiteCursor);
		xfer->xferUnsignedInt(&state.tunnelBuilderWaitUntilFrame);
		xfer->xferUnsignedInt(&state.tunnelBuilderCursor);
		xfer->xferUnsignedInt(&state.tunnelBuilderSignature);
		xfer->xferUnsignedInt(&state.tunnelBuilderWindowsRemaining);
		xfer->xferUnsignedInt(&state.tunnelForwardEntryCursor);
		xfer->xferUnsignedInt(&state.tunnelForwardEntryRemaining);
		xfer->xferUnsignedInt(&state.tunnelForwardEntrySignature);
		xfer->xferObjectID(&state.tunnelForwardEntryTargetID);
		xfer->xferUnsignedInt(&state.tunnelForwardEntryRetryAfterFrame);
		xfer->xferObjectID(&state.tunnelEntryID);
		xfer->xferObjectID(&state.tunnelExitID);
		xfer->xferObjectID(&state.tunnelTargetID);
		xfer->xferObjectID(&state.tunnelStrategicTargetID);
		xfer->xferUnsignedInt(&state.tunnelPhaseDeadlineFrame);
		xfer->xferUnsignedInt(&state.tunnelCooldownUntilFrame);
		xferTunnelEndpointProbes(xfer, state);
		xfer->xferObjectID(&state.tunnelCapacityWaitTargetID);
		xfer->xferUnsignedInt(&state.tunnelCapacityWaitDeadlineFrame);
		xfer->xferUnsignedInt(&state.tunnelCommittedTargetLastSeenFrame);
	}
}

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
	if (ShouldIncludeSkirmishAITacticalCRCFields(replay, replayEpoch)) {
		xfer->xferBool(&m_strategyTargetFallbackPending);
		xfer->xferObjectID(&m_strategyTargetFallbackAfterID);
		xfer->xferInt(&m_strategyTargetFallbackEnemyIndex);
		xfer->xferUnsignedInt(&m_tacticalNextTeamScanFrame);
		crcTacticalTeams(xfer);
		xferGeneratedDefenseBuilds(xfer);
		xfer->xferInt(&m_defensePatrolRoute);
		xfer->xferUnsignedInt(&m_defensePatrolTeamID);
		xfer->xferObjectID(&m_defensePatrolObjectID);
		xfer->xferUnsignedInt(&m_defenseNextPatrolFrame);
		xfer->xferCoord3D(&m_defenseQuietPatrolWaypoint);
		xfer->xferUnsignedInt(&m_defenseQuietPatrolDeadlineFrame);
		xfer->xferUnsignedInt(&m_defensePlacementAttempt);
		xfer->xferUnsignedInt(&m_defensePlacementNextFrame);
		xfer->xferObjectID(&m_defensePursuitTargetID);
		xfer->xferUnsignedInt(&m_defensePursuitStartFrame);
		xfer->xferObjectID(&m_defenseInterceptProbeAfterID);
		xfer->xferObjectID(&m_defensePatrolRouteMemberAfterID);
		xfer->xferObjectID(&m_defenseInterceptMemberAfterID);
		xfer->xferInt(&m_tunnelBuildPhase);
		xfer->xferBool(&m_tunnelHomeAttempted);
		xfer->xferBool(&m_tunnelForwardAttempted);
		xfer->xferObjectID(&m_tunnelHomeEndpointID);
		xfer->xferObjectID(&m_tunnelForwardEndpointID);
		xfer->xferObjectID(&m_tunnelBuildBuilderID);
		xfer->xferObjectID(&m_tunnelBuildTargetID);
		xfer->xferObjectID(&m_tunnelBuildBlockerID);
		xfer->xferObjectID(&m_tunnelBuildObjectID);
		xfer->xferUnsignedInt(&m_tunnelPendingBuilderCursor);
		xfer->xferCoord3D(&m_tunnelBuildLocation);
		xfer->xferUnsignedInt(&m_tunnelBuildDeadlineFrame);
		xfer->xferUnsignedInt(&m_tunnelBuildCooldownUntilFrame);
		xfer->xferObjectID(&m_tunnelForwardAttemptTargetID);
		for (Int i = 0;
			i < SkirmishAITunnelRoute::MAX_GENERATED_FORWARD_ENDPOINTS; ++i) {
			xfer->xferObjectID(&m_tunnelGeneratedForwardEndpointIDs[i]);
			xfer->xferObjectID(&m_tunnelGeneratedForwardTargetIDs[i]);
		}
		xfer->xferBool(&m_tunnelForwardRetryConsumed);
		for (Int i = 0;
			i < SkirmishAITunnelRoute::MAX_EXHAUSTED_FORWARD_TARGETS; ++i)
			xfer->xferObjectID(&m_tunnelExhaustedForwardTargetIDs[i]);
	}
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
	 * 12: Stage 3 worker collector roles, keyed by ObjectID
	 * 13: Stage 4 tactical progress, regroup, and tunnel transit state
	 * 14: Tunnel endpoint probe cache, generated defense identity, and
	 *     rotating strategic target fallback
	 * 15: Rotating tactical retreat facility coverage
	 * 16: Quiet defense patrol waypoint and bounded hold deadline
	 * 17: Generated forward tunnel targets and bounded live exits
	 * 18: Per-target forward retry and bounded full-tunnel wait
	 * 19: Exhausted forward targets and committed alternate-target observation */
// ------------------------------------------------------------------------------------------------
void AISkirmishPlayer::xfer( Xfer *xfer )
{

	// version
	XferVersion currentVersion = 19;
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
	if (version >= 13) {
		xfer->xferUnsignedInt(&m_tacticalNextTeamScanFrame);
		xferTacticalTeams(xfer, version);
		if (version >= 14) {
			xfer->xferBool(&m_strategyTargetFallbackPending);
			xfer->xferObjectID(&m_strategyTargetFallbackAfterID);
			xfer->xferInt(&m_strategyTargetFallbackEnemyIndex);
		} else if (xfer->getXferMode() == XFER_LOAD) {
			m_strategyTargetFallbackPending = false;
			m_strategyTargetFallbackAfterID = INVALID_ID;
			m_strategyTargetFallbackEnemyIndex = -1;
		}
		xfer->xferInt(&m_defensePatrolRoute);
		xfer->xferUnsignedInt(&m_defensePatrolTeamID);
		xfer->xferObjectID(&m_defensePatrolObjectID);
		xfer->xferUnsignedInt(&m_defenseNextPatrolFrame);
		if (version >= 16) {
			xfer->xferCoord3D(&m_defenseQuietPatrolWaypoint);
			xfer->xferUnsignedInt(&m_defenseQuietPatrolDeadlineFrame);
		} else if (xfer->getXferMode() == XFER_LOAD) {
			m_defenseQuietPatrolWaypoint.zero();
			m_defenseQuietPatrolDeadlineFrame = 0;
		}
		xfer->xferUnsignedInt(&m_defensePlacementAttempt);
		xfer->xferUnsignedInt(&m_defensePlacementNextFrame);
		xfer->xferObjectID(&m_defensePursuitTargetID);
		xfer->xferUnsignedInt(&m_defensePursuitStartFrame);
		xfer->xferObjectID(&m_defenseInterceptProbeAfterID);
		if (version >= 14) {
			xfer->xferObjectID(&m_defensePatrolRouteMemberAfterID);
			xfer->xferObjectID(&m_defenseInterceptMemberAfterID);
		}
		xfer->xferInt(&m_tunnelBuildPhase);
		xfer->xferBool(&m_tunnelHomeAttempted);
		xfer->xferBool(&m_tunnelForwardAttempted);
		xfer->xferObjectID(&m_tunnelHomeEndpointID);
		xfer->xferObjectID(&m_tunnelForwardEndpointID);
		xfer->xferObjectID(&m_tunnelBuildBuilderID);
		xfer->xferObjectID(&m_tunnelBuildTargetID);
		xfer->xferObjectID(&m_tunnelBuildBlockerID);
		xfer->xferObjectID(&m_tunnelBuildObjectID);
		xfer->xferUnsignedInt(&m_tunnelPendingBuilderCursor);
		xfer->xferCoord3D(&m_tunnelBuildLocation);
		xfer->xferUnsignedInt(&m_tunnelBuildDeadlineFrame);
		xfer->xferUnsignedInt(&m_tunnelBuildCooldownUntilFrame);
		if (version >= 17) {
			xfer->xferObjectID(&m_tunnelForwardAttemptTargetID);
			for (Int i = 0;
				i < SkirmishAITunnelRoute::MAX_GENERATED_FORWARD_ENDPOINTS;
				++i) {
				xfer->xferObjectID(&m_tunnelGeneratedForwardEndpointIDs[i]);
				xfer->xferObjectID(&m_tunnelGeneratedForwardTargetIDs[i]);
			}
		} else if (xfer->getXferMode() == XFER_LOAD) {
			// Older saves know only the latest AI-built forward endpoint.
			// Its target is unavailable, but the live exit still consumes a slot.
			m_tunnelForwardAttemptTargetID = INVALID_ID;
			for (Int i = 0;
				i < SkirmishAITunnelRoute::MAX_GENERATED_FORWARD_ENDPOINTS;
				++i) {
				m_tunnelGeneratedForwardEndpointIDs[i] = INVALID_ID;
				m_tunnelGeneratedForwardTargetIDs[i] = INVALID_ID;
			}
			if (m_tunnelForwardAttempted &&
				m_tunnelForwardEndpointID != INVALID_ID)
				m_tunnelGeneratedForwardEndpointIDs[0] =
					m_tunnelForwardEndpointID;
		}
		if (version >= 18)
			xfer->xferBool(&m_tunnelForwardRetryConsumed);
		else if (xfer->getXferMode() == XFER_LOAD)
			m_tunnelForwardRetryConsumed = m_tunnelForwardAttempted;
		if (version >= 19) {
			for (Int i = 0;
				i < SkirmishAITunnelRoute::MAX_EXHAUSTED_FORWARD_TARGETS; ++i)
				xfer->xferObjectID(&m_tunnelExhaustedForwardTargetIDs[i]);
		} else if (xfer->getXferMode() == XFER_LOAD) {
			for (Int i = 0;
				i < SkirmishAITunnelRoute::MAX_EXHAUSTED_FORWARD_TARGETS; ++i)
				m_tunnelExhaustedForwardTargetIDs[i] = INVALID_ID;
		}
		if (xfer->getXferMode() == XFER_LOAD) {
			if (m_tunnelBuildPhase < SKIRMISH_AI_TUNNEL_BUILD_NONE ||
				m_tunnelBuildPhase >
					SKIRMISH_AI_TUNNEL_BUILD_FORWARD_RETRY_AVAILABLE ||
				(m_tunnelBuildPhase == SKIRMISH_AI_TUNNEL_BUILD_HOME_QUEUED &&
				 (m_tunnelBuildBuilderID == INVALID_ID ||
				  m_tunnelBuildTargetID == INVALID_ID ||
				  m_tunnelBuildBlockerID == INVALID_ID)) ||
				(m_tunnelBuildPhase == SKIRMISH_AI_TUNNEL_BUILD_FORWARD_QUEUED &&
				 (m_tunnelBuildBuilderID == INVALID_ID ||
				  m_tunnelBuildTargetID == INVALID_ID ||
				  m_tunnelBuildBlockerID == INVALID_ID)) ||
				((m_tunnelBuildPhase ==
					SKIRMISH_AI_TUNNEL_BUILD_HOME_CONSTRUCTING ||
				  m_tunnelBuildPhase ==
					SKIRMISH_AI_TUNNEL_BUILD_FORWARD_CONSTRUCTING) &&
				 m_tunnelBuildObjectID == INVALID_ID) ||
				(m_tunnelBuildPhase ==
					SKIRMISH_AI_TUNNEL_BUILD_HOME_RETRY_AVAILABLE &&
				 m_tunnelBuildTargetID == INVALID_ID) ||
				(m_tunnelBuildPhase ==
					SKIRMISH_AI_TUNNEL_BUILD_FORWARD_RETRY_AVAILABLE &&
				 m_tunnelBuildTargetID == INVALID_ID)) {
				m_tunnelBuildPhase = SKIRMISH_AI_TUNNEL_BUILD_NONE;
				m_tunnelBuildBuilderID = INVALID_ID;
				m_tunnelBuildTargetID = INVALID_ID;
				m_tunnelBuildBlockerID = INVALID_ID;
				m_tunnelBuildObjectID = INVALID_ID;
				m_tunnelBuildDeadlineFrame = 0;
				m_tunnelBuildLocation.zero();
				m_tunnelPendingBuilderCursor = 0;
			}
			if (m_tunnelBuildPhase == SKIRMISH_AI_TUNNEL_BUILD_NONE) {
				m_tunnelBuildBuilderID = INVALID_ID;
				m_tunnelBuildTargetID = INVALID_ID;
				m_tunnelBuildBlockerID = INVALID_ID;
				m_tunnelBuildObjectID = INVALID_ID;
				m_tunnelBuildDeadlineFrame = 0;
				m_tunnelBuildLocation.zero();
				m_tunnelPendingBuilderCursor = 0;
			} else if (m_tunnelBuildPhase ==
					SKIRMISH_AI_TUNNEL_BUILD_HOME_RETRY_AVAILABLE ||
				m_tunnelBuildPhase ==
					SKIRMISH_AI_TUNNEL_BUILD_FORWARD_RETRY_AVAILABLE) {
				m_tunnelBuildBuilderID = INVALID_ID;
				m_tunnelBuildBlockerID = INVALID_ID;
				if (m_tunnelBuildObjectID == INVALID_ID)
					m_tunnelBuildDeadlineFrame = 0;
				m_tunnelBuildLocation.zero();
			}
		}
	} else if (xfer->getXferMode() == XFER_LOAD) {
		m_tacticalTeams.clear();
		m_tacticalNextTeamScanFrame = 0;
		m_strategyTargetFallbackPending = false;
		m_strategyTargetFallbackAfterID = INVALID_ID;
		m_strategyTargetFallbackEnemyIndex = -1;
		m_tunnelBuildPhase = SKIRMISH_AI_TUNNEL_BUILD_NONE;
		m_tunnelHomeAttempted = false;
		m_tunnelForwardAttempted = false;
		m_tunnelForwardRetryConsumed = false;
		m_tunnelHomeEndpointID = INVALID_ID;
		m_tunnelForwardEndpointID = INVALID_ID;
		m_tunnelForwardAttemptTargetID = INVALID_ID;
		for (Int i = 0;
			i < SkirmishAITunnelRoute::MAX_GENERATED_FORWARD_ENDPOINTS; ++i) {
			m_tunnelGeneratedForwardEndpointIDs[i] = INVALID_ID;
			m_tunnelGeneratedForwardTargetIDs[i] = INVALID_ID;
		}
		m_tunnelBuildBuilderID = INVALID_ID;
		m_tunnelBuildTargetID = INVALID_ID;
		m_tunnelBuildBlockerID = INVALID_ID;
		m_tunnelBuildObjectID = INVALID_ID;
		m_tunnelPendingBuilderCursor = 0;
		m_tunnelBuildLocation.zero();
		m_tunnelBuildDeadlineFrame = 0;
		m_tunnelBuildCooldownUntilFrame = 0;
		m_defensePatrolRoute = SKIRMISH_AI_DEFENSE_NO_ROUTE;
		m_defensePatrolTeamID = 0;
		m_defensePatrolObjectID = INVALID_ID;
		m_defenseNextPatrolFrame = 0;
		m_defenseQuietPatrolWaypoint.zero();
		m_defenseQuietPatrolDeadlineFrame = 0;
		m_defensePlacementAttempt = 0;
		m_defensePlacementNextFrame = 0;
		m_defensePursuitTargetID = INVALID_ID;
		m_defensePursuitStartFrame = 0;
		m_defenseInterceptProbeAfterID = INVALID_ID;
		m_defensePatrolRouteMemberAfterID = INVALID_ID;
		m_defenseInterceptMemberAfterID = INVALID_ID;
	}
	if (version >= 14)
		xferGeneratedDefenseBuilds(xfer);
	else if (xfer->getXferMode() == XFER_LOAD)
		m_generatedDefenseBuilds.clear();
	if (xfer->getXferMode() == XFER_LOAD) {
		m_tunnelBuildLockedBuilderID = INVALID_ID;
		m_defenseBuildLockedBuilderID = INVALID_ID;
		m_defenseBuildLockedLocation.zero();
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

