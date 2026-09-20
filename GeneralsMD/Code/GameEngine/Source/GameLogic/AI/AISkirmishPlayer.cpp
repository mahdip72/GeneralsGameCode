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
#include "GameLogic/Module/DozerAIUpdate.h"
#include "GameLogic/Module/RebuildHoleBehavior.h"
#include "GameLogic/Module/SupplyTruckAIUpdate.h"
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

static Bool HasSkirmishAIRecoveryFactoryFinisher(
	Object *factory, const Player *owner)
{
	if (!factory || !factory->testStatus(OBJECT_STATUS_UNDER_CONSTRUCTION))
		return true;
	if (!TheGameLogic)
		return false;
	Object *builder = TheGameLogic->findObjectByID(factory->getBuilderID());
	if (IsLiveSkirmishAIRecoveryObject(builder, owner) &&
		!builder->isDisabledByType(DISABLED_UNMANNED) &&
		builder->getAIUpdateInterface() &&
		builder->getAIUpdateInterface()->getDozerAIInterface())
		return true;

	// A GLA rebuild scaffold may temporarily have no worker while its hole's
	// respawn timer is active.  Preserve that self-rebuild route, but only when
	// the live hole owns this exact reconstruction template.
	Object *hole = TheGameLogic->findObjectByID(factory->getProducerID());
	RebuildHoleBehaviorInterface *holeAI = hole
		? RebuildHoleBehavior::getRebuildHoleBehaviorInterfaceFromObject(hole)
		: nullptr;
	return IsLiveSkirmishAIRecoveryObject(hole, owner) && holeAI &&
		factory->getTemplate() && holeAI->getRebuildTemplate() &&
		holeAI->getRebuildTemplate()->isEquivalentTo(factory->getTemplate());
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
	Bool hasContainedBuilder = false;
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
		hasContainedBuilder = true;

		Object *container = object->getContainedBy();
		AIUpdateInterface *builderAI = object->getAIUpdateInterface();
		ProductionUpdateInterface *production = container
			? container->getProductionUpdateInterface() : nullptr;
		const Bool containerHasActiveProduction = production &&
			IsSkirmishAIRecoveryProductionActive(
				production->getProductionCount());
		const StateID builderState = builderAI->getCurrentStateID();
		const Bool builderAlreadyExiting = builderState == AI_EXIT ||
			builderState == AI_EXIT_INSTANTLY ||
			builderState == AI_FOLLOW_EXITPRODUCTION_PATH;
		if (!ShouldOrderSkirmishAIRecoveryBuilderExit(
			true, IsLiveSkirmishAIRecoveryObject(container, player),
			container && container->getContain(),
			containerHasActiveProduction,
			builderAlreadyExiting))
			continue;
		builderAI->aiExit(container, CMD_FROM_AI);
	}
	return hasContainedBuilder;
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
	m_recoveryEverCompleted(false),
	m_recoveryImpossible(false),
	m_recoveryConstructionID(INVALID_ID),
	m_recoveryPlacementAttempt(0),
	m_recoveryNextAttemptFrame(0),
	m_recoveryEvacuationDeadline(0),
	m_recoveryReserveCost(0),
	m_recoveryAuthorizedThing(nullptr)

{
	m_frameLastBuildingBuilt = TheGameLogic->getFrame();
	m_recoveryLocation.zero();
	m_recoveryAngle = 0.0f;
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

Bool AISkirmishPlayer::canSpendForCriticalRecovery(
	Int cost, const ThingTemplate *thing, Bool isUpgrade) const
{
	if (cost <= 0 || !usesCriticalRecoveryBehavior())
		return true;
	if (!m_player || !m_player->getMoney())
		return false;
	const Int money = m_player->getMoney()->countMoney();
	if (cost > money)
		return false;
	if (m_recoveryAuthorizedThing && !isUpgrade && thing &&
		thing->isEquivalentTo(m_recoveryAuthorizedThing))
		return true;
	if (m_recoveryEverCompleted && m_recoveryConstructionID != INVALID_ID &&
		TheGameLogic) {
		Object *tracked = TheGameLogic->findObjectByID(m_recoveryConstructionID);
		if (!IsLiveSkirmishAIRecoveryObject(tracked, m_player))
			return false;
	}
	if (m_recoveryImpossible || m_recoveryReserveCost <= 0)
		return true;
	return cost <= money - m_recoveryReserveCost;
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
		if (!best || object->getID() < best->getID())
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
	const ThingTemplate **builderTemplate, Object **factory, Bool *hasPotentialFactory)
{
	if (builderTemplate)
		*builderTemplate = nullptr;
	if (factory)
		*factory = nullptr;
	if (hasPotentialFactory)
		*hasPotentialFactory = false;
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
				if (!HasSkirmishAIRecoveryFactoryFinisher(object, m_player))
					continue;
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
			if (hasPotentialFactory)
				*hasPotentialFactory = true;
			if (!fallbackFactory || object->getID() < fallbackFactory->getID() ||
				(object->getID() == fallbackFactory->getID() &&
					command < fallbackCommand)) {
				fallbackFactory = object;
				fallbackTemplate = product;
				fallbackCommand = command;
			}
			if (TheBuildAssistant->canMakeUnit(object, product) != CANMAKE_OK)
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

Bool AISkirmishPlayer::hasRecoveryBuilderQueued(
	const ThingTemplate *primaryTemplate, Bool *paid, ObjectID *factoryID)
{
	if (paid)
		*paid = false;
	if (factoryID)
		*factoryID = INVALID_ID;
	// Production queues are authoritative for paid work.  This also sees
	// script-issued dozers and GLA worker/supply-worker entries that do not
	// necessarily have a matching TeamInQueue order.
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
					if (paid)
						*paid = true;
					if (factoryID)
						*factoryID = factory->getID();
					return true;
				}
			}
		}
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
			if (!order->m_thing || !order->m_thing->isKindOf(KINDOF_DOZER) ||
				!HasSkirmishAICommandSetForTemplate(
					order->m_thing->friend_getCommandSetString(), primaryTemplate) ||
				order->m_numCompleted >= order->m_numRequired ||
				order->m_factoryID != INVALID_ID)
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
	if (!production || TheBuildAssistant->canMakeUnit(factory, builderTemplate) != CANMAKE_OK)
		return false;

	WorkOrder *order = nullptr;
	for (DLINK_ITERATOR<TeamInQueue> iter = iterate_TeamBuildQueue();
		!iter.done() && !order; iter.advance()) {
		for (WorkOrder *waiting = iter.cur()->m_workOrders; waiting;
			waiting = waiting->m_next) {
			if (waiting->m_factoryID == INVALID_ID && waiting->m_thing &&
				waiting->m_thing->isEquivalentTo(builderTemplate) &&
				waiting->m_numCompleted < waiting->m_numRequired) {
				order = waiting;
				break;
			}
		}
	}
	TeamInQueue *newTeam = nullptr;
	if (!order) {
		order = newInstance(WorkOrder);
		order->m_thing = builderTemplate;
		order->m_numRequired = 1;
		order->m_required = true;
		order->m_isResourceGatherer = false;
		newTeam = newInstance(TeamInQueue);
		newTeam->m_priorityBuild = true;
		newTeam->m_workOrders = order;
		newTeam->m_frameStarted = TheGameLogic->getFrame();
		newTeam->m_team = m_player->getDefaultTeam();
		prependTo_TeamBuildQueue(newTeam);
	}

	Bool queued = false;
	const ThingTemplate *previousAuthorization = m_recoveryAuthorizedThing;
	m_recoveryAuthorizedThing = builderTemplate;
	queued = production->queueCreateUnit(builderTemplate, production->requestUniqueUnitID());
	m_recoveryAuthorizedThing = previousAuthorization;
	if (!queued) {
		if (newTeam) {
			removeFrom_TeamBuildQueue(newTeam);
			deleteInstance(newTeam);
		}
		return false;
	}

	order->m_factoryID = factory->getID();
	m_teamDelay = 0;
	if (TheGlobalData->m_debugAI) {
		AsciiString message = "Critical recovery queued builder from ";
		message.concat(factory->getTemplate()->getName());
		TheScriptEngine->AppendDebugMessage(message, false);
	}
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

Bool AISkirmishPlayer::prepareCriticalRecoveryBuilder(Object *builder)
{
	if (!builder || !m_player || !TheGameLogic ||
		!IsLiveSkirmishAIRecoveryObject(builder, m_player) ||
		!builder->isKindOf(KINDOF_DOZER) ||
		builder->isDisabledByType(DISABLED_UNMANNED))
		return false;

	AIUpdateInterface *ai = builder->getAIUpdateInterface();
	DozerAIInterface *dozerAI = ai ? ai->getDozerAIInterface() : nullptr;
	if (!ai || !dozerAI)
		return false;
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

	m_recoveryPlacementAttempt = (start + 1) % g_skirmishAIRecoveryOffsetCount;
	m_recoveryNextAttemptFrame =
		GetSkirmishAIRecoveryRetryFrame(
			TheGameLogic->getFrame(), 2 * LOGICFRAMES_PER_SECOND);
	return false;
}

void AISkirmishPlayer::enterRecoveryLastStand()
{
	const Bool maintenance = m_recoveryImpossible;
	m_recoveryEvacuationDeadline = 0;
	// Hunt deliberately searches enemies without line-of-sight checks. Use a
	// known objective and ordinary attack-move acquisition for the last stand.
	Coord3D target;
	if (!getKnownEnemyPosition(getAiEnemy(), &target)) {
		// Objective discovery can lag the recovery decision on a fogged map.
		// Keep this state retryable instead of permanently abandoning the
		// surviving forces without issuing any command.
		m_recoveryConstructionID = INVALID_ID;
		m_recoveryReserveCost = 0;
		m_recoveryNextAttemptFrame = GetSkirmishAIRecoveryRetryFrame(
			TheGameLogic->getFrame(), 2 * LOGICFRAMES_PER_SECOND);
		return;
	}
	m_recoveryImpossible = true;
	m_recoveryConstructionID = INVALID_ID;
	m_recoveryReserveCost = 0;
	m_recoveryNextAttemptFrame = GetSkirmishAIRecoveryRetryFrame(
		TheGameLogic->getFrame(), 2 * LOGICFRAMES_PER_SECOND);
	for (Object *object = TheGameLogic->getFirstObject(); object;
		object = object->getNextObject()) {
		if (!IsLiveSkirmishAIRecoveryObject(object, m_player) ||
			object->isKindOf(KINDOF_IMMOBILE) || !object->isAbleToAttack())
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
			"Critical recovery impossible; surviving forces attack a known objective.", false);
}

void AISkirmishPlayer::updateCriticalRecovery()
{
	if (!usesCriticalRecoveryBehavior()) {
		m_recoveryReserveCost = 0;
		m_recoveryEvacuationDeadline = 0;
		return;
	}
	const UnsignedInt frame = TheGameLogic->getFrame();
	if (m_recoveryImpossible) {
		m_recoveryReserveCost = 0;
		m_recoveryEvacuationDeadline = 0;
		if (IsSkirmishAIRecoveryRetryDue(frame, m_recoveryNextAttemptFrame)) {
			enterRecoveryLastStand();
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
		m_recoveryConstructionID = center->getID();
		m_recoveryLocation = *center->getPosition();
		m_recoveryAngle = center->getOrientation();
	}

	// A recovery scaffold remains a live critical objective even after its
	// original builder disappears.  Rebind it through the normal resume command
	// before allowing the ordinary base-building path to observe the scaffold.
	if (hasCenter && hasConstruction && m_recoveryEverCompleted) {
		Object *nativeHole = TheGameLogic->findObjectByID(
			center->getProducerID());
		if (IsSkirmishAIRecoveryPrimaryHole(
			nativeHole, m_player, primaryTemplate, info, center->getID())) {
			// The hole owns the free worker and native isRebuild construction;
			// leave that lifecycle untouched and do not queue a paid duplicate.
			m_recoveryConstructionID = center->getID();
			m_recoveryEvacuationDeadline = 0;
			m_recoveryReserveCost = 0;
			m_recoveryNextAttemptFrame = GetSkirmishAIRecoveryRetryFrame(
				frame, 2 * LOGICFRAMES_PER_SECOND);
			return;
		}
		m_recoveryConstructionID = center->getID();
		Object *assignedBuilder = TheGameLogic->findObjectByID(
			center->getBuilderID());
		Bool assignedUsable = IsLiveSkirmishAIRecoveryObject(
			assignedBuilder, m_player) &&
			!assignedBuilder->isContained() &&
			!assignedBuilder->isDisabledByType(DISABLED_UNMANNED) &&
			assignedBuilder->getAIUpdateInterface() &&
			assignedBuilder->getAIUpdateInterface()->getDozerAIInterface();
		if (assignedUsable) {
			AIUpdateInterface *assignedAI =
				assignedBuilder->getAIUpdateInterface();
			DozerAIInterface *assignedDozer =
				assignedAI->getDozerAIInterface();
			Coord3D assignedActionPosition;
			Object *assignedResumeTarget =
				DozerAIUpdate::findGoodBuildOrRepairPositionAndTarget(
					assignedBuilder, center, assignedActionPosition);
			const Bool assignedPathable = assignedResumeTarget == center &&
				assignedAI->isPathAvailable(&assignedActionPosition);
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
		m_recoveryEvacuationDeadline = GetSkirmishAIRecoveryEvacuationDeadline(
			frame, m_recoveryEvacuationDeadline, hasContainedBuilder,
			2 * LOGICFRAMES_PER_SECOND);
		const Bool evacuationGraceActive =
			IsSkirmishAIRecoveryEvacuationGraceActive(
				frame, m_recoveryEvacuationDeadline, hasContainedBuilder);

		// The old builder is no longer a valid owner of the scaffold.  Clear
		// the binding before asking a different compatible builder to resume it.
		center->setBuilder(nullptr);
		std::vector<Object *> replacementBuilders;
		CollectSkirmishAIRecoveryBuilders(
			m_player, center->getPosition(), primaryTemplate,
			&replacementBuilders);
		for (std::vector<Object *>::iterator replacement =
			replacementBuilders.begin(); replacement != replacementBuilders.end();
			++replacement) {
			Object *replacementBuilder = *replacement;
			AIUpdateInterface *replacementAI =
				replacementBuilder->getAIUpdateInterface();
			Coord3D actionPosition;
			Object *resumeTarget =
				DozerAIUpdate::findGoodBuildOrRepairPositionAndTarget(
					replacementBuilder, center, actionPosition);
			if (!replacementAI || resumeTarget != center ||
				!replacementAI->isPathAvailable(&actionPosition))
				continue;
			if (prepareCriticalRecoveryBuilder(replacementBuilder)) {
				replacementAI->aiResumeConstruction(center, CMD_FROM_AI);
				DozerAIInterface *replacementDozer = replacementAI
					? replacementAI->getDozerAIInterface() : nullptr;
				if (replacementDozer && replacementDozer->isTaskPending(DOZER_TASK_BUILD) &&
					replacementDozer->getTaskTarget(DOZER_TASK_BUILD) == center->getID()) {
					m_recoveryEvacuationDeadline = 0;
					m_recoveryReserveCost = 0;
					m_recoveryNextAttemptFrame = GetSkirmishAIRecoveryRetryFrame(
						frame, 2 * LOGICFRAMES_PER_SECOND);
					return;
				}
			}
		}
		if (!replacementBuilders.empty()) {
			// Live compatible replacements remain retryable even when every
			// cancellation/resume transition is temporarily blocked.
			m_recoveryReserveCost = 0;
			m_recoveryNextAttemptFrame = GetSkirmishAIRecoveryRetryFrame(
				frame, 2 * LOGICFRAMES_PER_SECOND);
			return;
		}
		Bool builderQueuedPaid = false;
		ObjectID queuedFactoryID = INVALID_ID;
		const Bool builderQueued = hasRecoveryBuilderQueued(
			primaryTemplate, &builderQueuedPaid, &queuedFactoryID);
		const ThingTemplate *replacementTemplate = nullptr;
		Object *replacementFactory = nullptr;
		Bool hasPotentialFactory = false;
		findRecoveryBuilderTemplateAndFactory(
			primaryTemplate, &replacementTemplate, &replacementFactory,
			&hasPotentialFactory);
		const Int replacementCost = replacementTemplate
			? replacementTemplate->calcCostToBuild(m_player) : 0;
		if (evacuationGraceActive) {
			m_recoveryReserveCost = replacementCost > 0 ? replacementCost : 0;
			m_recoveryNextAttemptFrame = m_recoveryEvacuationDeadline;
			return;
		}
		if ((!builderQueued || !builderQueuedPaid) && replacementFactory &&
			replacementTemplate && replacementCost >= 0 &&
			m_player->getMoney()->countMoney() >= replacementCost &&
			TheBuildAssistant->canMakeUnit(
				replacementFactory, replacementTemplate) == CANMAKE_OK &&
			queueRecoveryBuilder(replacementTemplate, replacementFactory)) {
			m_recoveryReserveCost = replacementCost;
			m_recoveryNextAttemptFrame = GetSkirmishAIRecoveryRetryFrame(
				frame, 2 * LOGICFRAMES_PER_SECOND);
			return;
		}
		if (builderQueuedPaid || hasPotentialFactory) {
			m_recoveryReserveCost = replacementCost > 0 ? replacementCost : 0;
			m_recoveryNextAttemptFrame = GetSkirmishAIRecoveryRetryFrame(
				frame, 2 * LOGICFRAMES_PER_SECOND);
			return;
		}
		enterRecoveryLastStand();
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
			primaryTemplate, &builderQueuedPaid, &queuedFactoryID);
		const Int desiredBuilders =
			(!hasConstruction && m_player->getMoney()->countMoney() >=
				TheAI->getAiData()->m_resourcesWealthy) ? 2 : 1;
		if (builderCount < desiredBuilders && !builderQueued) {
			const ThingTemplate *builderTemplate = nullptr;
			Object *factory = nullptr;
			Bool hasPotentialFactory = false;
			if (findRecoveryBuilderTemplateAndFactory(
				primaryTemplate,
				&builderTemplate, &factory, &hasPotentialFactory) && factory && builderTemplate) {
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
		enterRecoveryLastStand();
		return;
	}
	if (m_recoveryLocation.x == 0.0f && m_recoveryLocation.y == 0.0f &&
		m_recoveryLocation.z == 0.0f)
		m_recoveryLocation = *info->getLocation();
	if (m_recoveryAngle == 0.0f)
		m_recoveryAngle = info->getAngle();

	Bool builderQueuedPaid = false;
	ObjectID queuedFactoryID = INVALID_ID;
	const Bool builderQueued = hasRecoveryBuilderQueued(
		primaryTemplate, &builderQueuedPaid, &queuedFactoryID);
	const ThingTemplate *builderTemplate = nullptr;
	Object *builderFactory = nullptr;
	Bool hasPotentialFactory = false;
	findRecoveryBuilderTemplateAndFactory(
		primaryTemplate,
		&builderTemplate, &builderFactory, &hasPotentialFactory);
	Object *builder = findRecoveryBuilder(&m_recoveryLocation, primaryTemplate);
	const Bool hasBuilder = builder != nullptr;
	const Bool hasContainedBuilder =
		RecoverSkirmishAIContainedBuilders(m_player, primaryTemplate);
	m_recoveryEvacuationDeadline = GetSkirmishAIRecoveryEvacuationDeadline(
		frame, m_recoveryEvacuationDeadline, hasContainedBuilder,
		2 * LOGICFRAMES_PER_SECOND);
	const Bool evacuationGraceActive = IsSkirmishAIRecoveryEvacuationGraceActive(
		frame, m_recoveryEvacuationDeadline, hasContainedBuilder);
	const Bool retryDue = IsSkirmishAIRecoveryRetryDue(
		TheGameLogic->getFrame(), m_recoveryNextAttemptFrame);
	const Int commandCenterCost = primaryTemplate->calcCostToBuild(m_player);
	const Int builderCost = builderTemplate
		? builderTemplate->calcCostToBuild(m_player) : 0;
	const Int money = m_player->getMoney()->countMoney();
	if (!hasBuilder && evacuationGraceActive) {
		const Int recoveryCost = builderQueuedPaid
			? commandCenterCost
			: AddSkirmishAIRecoveryCost(commandCenterCost, builderCost);
		m_recoveryReserveCost = max(protectedReserve, recoveryCost);
		m_recoveryNextAttemptFrame = m_recoveryEvacuationDeadline;
		return;
	}

	SkirmishAIRecoveryPolicyInput input;
	input.enabled = true;
	input.everCompleted = m_recoveryEverCompleted;
	input.hasPrimaryCommandCenter = false;
	input.hasConstruction = hasConstruction;
	input.hasBuilder = hasBuilder;
	input.builderQueued = builderQueued;
	input.builderQueuePaid = builderQueuedPaid;
	input.hasBuilderFactory = builderFactory != nullptr;
	input.noBuilderPath = IsSkirmishAIRecoveryBuilderPathUnavailable(
		hasBuilder, evacuationGraceActive, builderQueuedPaid,
		hasPotentialFactory);
	input.builderAffordable = builderFactory && builderTemplate && retryDue &&
		TheBuildAssistant->canMakeUnit(builderFactory, builderTemplate) == CANMAKE_OK &&
		money >= builderCost;
	input.commandCenterAffordable = commandCenterCost >= 0 &&
		money >= commandCenterCost;
	input.placementReady = builder && retryDue;
	input.commandCenterCost = commandCenterCost;
	input.builderCost = builderCost;
	input.protectedReserve = protectedReserve;
	SkirmishAIRecoveryPolicyResult decision = DecideSkirmishAIRecovery(input);
	m_recoveryReserveCost = decision.reserveCost;

	if (decision.recoveryImpossible) {
		enterRecoveryLastStand();
		return;
	}
	if (decision.shouldQueueBuilder) {
		if (queueRecoveryBuilder(builderTemplate, builderFactory)) {
			m_recoveryNextAttemptFrame = GetSkirmishAIRecoveryRetryFrame(
				TheGameLogic->getFrame(), 2 * LOGICFRAMES_PER_SECOND);
			Bool paid = false;
			ObjectID factoryID = INVALID_ID;
			if (hasRecoveryBuilderQueued(primaryTemplate, &paid, &factoryID))
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
		std::vector<Object *> constructionBuilders;
		CollectSkirmishAIRecoveryBuilders(
			m_player, &m_recoveryLocation, primaryTemplate,
			&constructionBuilders);
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
			if (info->isPriorityBuild()) {
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
			if (!info->isAutomaticBuild()) {
				continue; // marked to not build automatically.
			}
			Object *dozer = findDozer(info->getLocation());
			if (dozer==nullptr) {
				if (isUnderPowered) {
					queueDozer();
				}
				continue;
			}
			if (TheBuildAssistant->canMakeUnit(dozer, GetSkirmishAutomaticConstructionPlan(curPlan, bldgPlan))!=CANMAKE_OK) {
				if (info->isBuildable()) {
					AsciiString bldgName = info->getTemplateName();
					bldgName.concat(" - Dozer unable to build - money or technology missing.");
					TheScriptEngine->AppendDebugMessage(bldgName, false);
				}
				continue;
			}
			// check if this building has any "rebuilds" left
			if (info->isBuildable())
			{
				if (bldgPlan == nullptr) {
					bldgPlan = curPlan;
					bldgInfo = info;
				}
			}
		}
		if (powerInfo && powerPlan && !powerPlan->isEquivalentTo(bldgPlan)) {
			if (!powerUnderConstruction) {
				bldgPlan = powerPlan;
				bldgInfo = powerInfo;
				DEBUG_LOG(("Forcing build of power plant."));
			}
		}
		if (bldgPlan && bldgInfo) {
#ifdef USE_DOZER
			// dozer-construct the building
			bldg = buildStructureWithDozer(bldgPlan, bldgInfo);
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
				if (m_player->getMoney()->countMoney() >= cost)
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
void AISkirmishPlayer::onUnitProduced( Object *factory, Object *unit )
{
	AIPlayer::onUnitProduced(factory, unit);
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
	// While recovery holds a positive reserve for a missing or interrupted
	// primary-center path, it owns compatible builder admission.  Suppress a
	// second ordinary builder order, but preserve explicit GLA resource-worker
	// orders; healthy recovery scaffolds clear this reserve.
	if (order && !order->m_isResourceGatherer &&
		order->m_thing && order->m_thing->isKindOf(KINDOF_DOZER) &&
		usesCriticalRecoveryBehavior() && m_recoveryEverCompleted &&
		!m_recoveryImpossible &&
		m_recoveryReserveCost > 0 && m_player->getPlayerTemplate()) {
		const ThingTemplate *primaryTemplate = TheThingFactory->findTemplate(
			m_player->getPlayerTemplate()->getStartingBuilding());
		if (primaryTemplate && HasSkirmishAICommandSetForTemplate(
			order->m_thing->friend_getCommandSetString(), primaryTemplate))
			return false;
	}
	Object *factory = findFactory(order->m_thing, busyOK);
	if( factory )
	{
		ProductionUpdateInterface *pu = factory->getProductionUpdateInterface();
		if (pu && pu->queueCreateUnit( order->m_thing, pu->requestUniqueUnitID() )) {
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
	return AIPlayer::selectTeamToReinforce(minPriority);
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
	const TeamTemplateInfo *info = proto->getTemplateInfo();
	for (Int i = 0; i < info->m_numUnitsInfo; ++i) {
		const TCreateUnitsInfo *unitInfo = &info->m_unitsInfo[i];
		const ThingTemplate *thing = TheThingFactory->findTemplate(unitInfo->unitThingName);
		if (!thing)
			continue;
		if (!findFactory(thing, true))
			return false;
		if (findFactory(thing, false))
			anyIdleFactory = true;
	}
	if (!anyIdleFactory)
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

Int AISkirmishPlayer::getCriticalRebuildReserve(Bool *canStartNow)
{
	if (canStartNow)
		*canStartNow = false;
	Int cheapestCost = 0;
	Bool isUnderPowered = !m_player->getEnergy()->hasSufficientPower();
	for (BuildListInfo *info = m_player->getBuildList(); info; info = info->getNext()) {
		const ThingTemplate *plan = TheThingFactory->findTemplate(info->getTemplateName());
		if (!plan)
			continue;
		Bool critical = plan->isKindOf(KINDOF_COMMANDCENTER) ||
			plan->isKindOf(KINDOF_FS_POWER) ||
			plan->isKindOf(KINDOF_FS_SUPPLY_CENTER) ||
			plan->isKindOf(KINDOF_FS_FACTORY) ||
			plan->isKindOf(KINDOF_FS_BARRACKS) ||
			plan->isKindOf(KINDOF_FS_WARFACTORY) ||
			plan->isKindOf(KINDOF_FS_AIRFIELD);
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
	if (usesCriticalRecoveryBehavior() && m_recoveryEverCompleted &&
		!m_recoveryImpossible && m_recoveryReserveCost > cheapestCost) {
		cheapestCost = m_recoveryReserveCost;
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
	for (BuildListInfo *build = m_player->getBuildList(); build; build = build->getNext()) {
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
	Int reserve = GetSkirmishAIReserve(poorReserve, rebuildReserve);
	Bool rebuildReserveApplied = reserve > GetSkirmishAIReserve(poorReserve, 0);
	Int resources = m_player->getMoney()->countMoney();
	SkirmishAIDecisionDifficulty difficulty = getDecisionDifficulty();

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
				!IsSkirmishAIAffordable(resources, candidateIt->costRange.minimumCost, reserve))
				continue;

			SkirmishAITeamScoreInput input;
			input.configuredPriority = priority;
			input.counterFitScore = getCandidateCounterFit(
				prototype, enemyAircraftValue, enemyVehicleValue, enemyInfantryValue);
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
			SkirmishAITeamScoreResult score = ScoreSkirmishAITeam(input);

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
	if (m_currentEnemy) {
		return m_currentEnemy->getPlayerIndex();
	}
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
	std::vector<SkirmishEnemyCandidate> candidates;
	Int maximumKnownAssetValue = 0;
	Int minimumDistance = 2147483647;
	Int maximumDistance = 0;
	Bool hasKnownDistance = false;
	Object *representative = findEnemyRouteRepresentative();
	for (Int i = 0; i < ThePlayerList->getPlayerCount(); ++i) {
		Player *candidatePlayer = ThePlayerList->getNthPlayer(i);
		if (!candidatePlayer ||
			m_player->getRelationship(candidatePlayer->getDefaultTeam()) != ENEMIES ||
			!candidatePlayer->hasAnyObjects())
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

	Bool currentEnemyInvalid = m_currentEnemy &&
		(m_player->getRelationship(m_currentEnemy->getDefaultTeam()) != ENEMIES ||
		 !m_currentEnemy->hasAnyObjects());
	if (ShouldEvaluateSkirmishAITarget(
		currentEnemyInvalid, TheGameLogic->getFrame(), m_frameToCheckEnemy, true)) {
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
			getPlayerStructureBounds(&bounds, getMyEnemyPlayerIndex());
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
	if (ShouldUseCurrentSkirmishAIBehavior())
		getAiEnemy();
	AIPlayer::update();
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

	Region2D bounds;
	getPlayerStructureBounds(&bounds, playerNdx);

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
			getPlayerStructureBounds(&bounds, getMyEnemyPlayerIndex());
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

// ------------------------------------------------------------------------------------------------
/** CRC */
// ------------------------------------------------------------------------------------------------
void AISkirmishPlayer::crc( Xfer *xfer )
{
	if (!usesCriticalRecoveryBehavior())
		return;
	xfer->xferBool(&m_recoveryEverCompleted);
	xfer->xferBool(&m_recoveryImpossible);
	xfer->xferObjectID(&m_recoveryConstructionID);
	xfer->xferInt(&m_recoveryPlacementAttempt);
	xfer->xferUnsignedInt(&m_recoveryNextAttemptFrame);
	// Active evacuation grace is already mirrored by the retry frame.  Keep the
	// v4 save field out of CRC so epoch-3 recordings retain their CRC layout.
	xfer->xferCoord3D(&m_recoveryLocation);
	xfer->xferReal(&m_recoveryAngle);
	xfer->xferInt(&m_recoveryReserveCost);
}

// ------------------------------------------------------------------------------------------------
/** Xfer method
	* Version Info;
	* 1: Initial version
	* 2: Current enemy and next enemy evaluation frame
	* 3: Critical command-center recovery state
	* 4: Contained-builder evacuation grace deadline */
// ------------------------------------------------------------------------------------------------
void AISkirmishPlayer::xfer( Xfer *xfer )
{

	// version
	XferVersion currentVersion = 4;
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
	m_recoveryAuthorizedThing = nullptr;

}

// ------------------------------------------------------------------------------------------------
/** Load post process */
// ------------------------------------------------------------------------------------------------
void AISkirmishPlayer::loadPostProcess()
{
	m_currentEnemy = nullptr;
	for (Int i = 0; i < ThePlayerList->getPlayerCount(); ++i) {
		Player *player = ThePlayerList->getNthPlayer(i);
		if (player && player->getPlayerIndex() == m_currentEnemyPlayerIndex) {
			m_currentEnemy = player;
			break;
		}
	}
	if (!m_currentEnemy)
		m_currentEnemyPlayerIndex = -1;
	m_recoveryAuthorizedThing = nullptr;
	if (m_recoveryPlacementAttempt < 0)
		m_recoveryPlacementAttempt = 0;
	if (m_recoveryReserveCost < 0)
		m_recoveryReserveCost = 0;
}

