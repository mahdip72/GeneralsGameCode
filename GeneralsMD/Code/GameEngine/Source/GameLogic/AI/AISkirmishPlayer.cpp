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
#include "GameLogic/Module/DozerAIUpdate.h"
#include "GameLogic/Module/RebuildHoleBehavior.h"
#include "GameLogic/Module/UpdateModule.h"
#include "GameLogic/PartitionManager.h"
#include "GameLogic/ScriptEngine.h"
#include "GameLogic/SkirmishAIDecision.h"
#include "GameLogic/Weapon.h"
#include "GameLogic/WeaponSet.h"
#include "GameLogic/Module/ProductionUpdate.h"
#include "GameClient/TerrainVisual.h"
#include "GameNetwork/GameInfo.h"


#define USE_DOZER 1

static Bool ShouldUseCurrentSkirmishAIBehavior()
{
	return ShouldUseSkirmishAICurrentBehavior(
		TheGameLogic->isInReplayGame(),
		TheRecorder ? TheRecorder->getSkirmishAIReplayEpoch() : SKIRMISH_AI_REPLAY_EPOCH_LEGACY);
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
	if (entry->getProductionObject())
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
m_currentEnemyPlayerIndex(-1)

{
	m_frameLastBuildingBuilt = TheGameLogic->getFrame();
	p->setCanBuildUnits(true); // turn on ai production by default.
}

AISkirmishPlayer::~AISkirmishPlayer()
{
	clearTeamsInQueue();
}


/**
 * Build our base.
 */
void AISkirmishPlayer::processBaseBuilding()
{
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
}

/**
 * Search the computer player's buildings for one that can build the given request
 * and start training the unit.
 * If busyOK is true, it will queue a unit even if one is building.  This lets
 * script invoked teams "push" to the front of the queue.
 */
Bool AISkirmishPlayer::startTraining( WorkOrder *order, Bool busyOK, AsciiString teamName)
{
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
	return ShouldUseSkirmishAICounterRng(
		TheGameLogic->isInReplayGame(),
		TheRecorder ? TheRecorder->getSkirmishAIReplayEpoch() : SKIRMISH_AI_REPLAY_EPOCH_LEGACY);
}

#endif

#if defined(_WIN64)
Bool AISkirmishPlayer::isEnemyPlanningDue() const
{
	Bool currentEnemyInvalid = m_currentEnemy &&
		(m_player->getRelationship(m_currentEnemy->getDefaultTeam()) != ENEMIES ||
		 !m_currentEnemy->hasAnyObjects());
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
			!candidatePlayer->hasAnyObjects())
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
/** Publish a target ID after all player results have been sorted by their order keys. */
//-------------------------------------------------------------------------------------------------
Bool AISkirmishPlayer::commitEnemyPlanningResult(
	const rts::AIEnemyPlanningSnapshot &snapshot,
	const rts::AIEnemyPlanningResult &result )
{
	if (!validateEnemyPlanningCommit(snapshot, result))
		return false;

	Player *newEnemy = nullptr;
	if (result.selectedPlayerIndex >= 0)
	{
		for (Int i = 0; i < ThePlayerList->getPlayerCount(); ++i)
		{
			Player *candidate = ThePlayerList->getNthPlayer(i);
			if (candidate && candidate->getPlayerIndex() == result.selectedPlayerIndex)
			{
				newEnemy = candidate;
				break;
			}
		}
	}
	m_frameToCheckEnemy = TheGameLogic->getFrame() + 5*LOGICFRAMES_PER_SECOND;

	if (newEnemy != m_currentEnemy)
	{
		m_currentEnemy = newEnemy;
		m_currentEnemyPlayerIndex = m_currentEnemy ? m_currentEnemy->getPlayerIndex() : -1;
		if (m_currentEnemy)
		{
			AsciiString message = TheNameKeyGenerator->keyToName(m_player->getPlayerNameKey());
			message.concat(" acquiring target enemy player: ");
			message.concat(TheNameKeyGenerator->keyToName(m_currentEnemy->getPlayerNameKey()));
			TheScriptEngine->AppendDebugMessage(message, false);
		}
	}
	return true;
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
		candidate->hasAnyObjects();
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
			unit.minUnits = unitInfo->minUnits;
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
	snapshot->initialReserve = GetSkirmishAIReserve(poorReserve, rebuildReserve);
	snapshot->retryReserve = GetSkirmishAIReserve(poorReserve, 0);
	Bool rebuildReserveApplied = snapshot->initialReserve > snapshot->retryReserve;
	snapshot->retryWithoutInitialReserve =
		rebuildReserveApplied && !criticalRebuildCanStart ? 1 : 0;
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
/** Rare overflow lane: preserve epoch-3 scoring and counter-RNG semantics without a fixed array. */
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
	context.initialReserve = GetSkirmishAIReserve(poorReserve, rebuildReserve);
	context.retryReserve = GetSkirmishAIReserve(poorReserve, 0);
	const Bool rebuildReserveApplied = context.initialReserve > context.retryReserve;
	context.retryWithoutInitialReserve =
		rebuildReserveApplied && !criticalRebuildCanStart ? 1U : 0U;
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
		fact.factoryWaitFrames = candidateIt->factoryWaitFrames;
		fact.counterFitScore = getCandidateCounterFit(candidateIt->prototype,
			enemyAircraftValue, enemyVehicleValue, enemyInfantryValue);
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
		!result.valid || !result.hasSelection)
	{
		rts::RecordAIPlanningOwnerCommit(false);
		return false;
	}

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

}

// ------------------------------------------------------------------------------------------------
/** Xfer method
	* Version Info;
	* 1: Initial version
	* 2: Current enemy and next enemy evaluation frame */
// ------------------------------------------------------------------------------------------------
void AISkirmishPlayer::xfer( Xfer *xfer )
{

	// version
	XferVersion currentVersion = 2;
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
}

