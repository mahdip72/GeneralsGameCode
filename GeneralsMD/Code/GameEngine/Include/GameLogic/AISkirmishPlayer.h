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

// AISkirmishPlayer.h
// Computerized opponent
// Author: Michael S. Booth, January 2002

#pragma once

#include <map>
#include <vector>

#include "Common/GameMemory.h"
#include "GameLogic/AIPlayer.h"
#include "GameLogic/SkirmishAIDecision.h"
#include "GameLogic/SkirmishAIStrategy.h"
#include "GameLogic/SkirmishAITunnelRoute.h"

class BuildListInfo;
class SpecialPowerTemplate;
class ThingTemplate;
enum ProductionID CPP_11(: Int);


/**
 * The computer-controlled opponent.
 */
class AISkirmishPlayer : public AIPlayer
{
	MEMORY_POOL_GLUE_WITH_USERLOOKUP_CREATE( AISkirmishPlayer, "AISkirmishPlayer"  )

public:	 // AISkirmish specific methods.

	AISkirmishPlayer( Player *p );							///< constructor
	virtual Bool computeSuperweaponTarget(const SpecialPowerTemplate *power, Coord3D *pos, Int playerNdx, Real weaponRadius) override; ///< Calculates best pos for weapon given radius.
	virtual Bool shouldUseSkirmishSpecialPowerSource(Object *source, const SpecialPowerTemplate *power) override;
	virtual void resolveSpecialPowerDispatchAttempt(Object *source,
		const SpecialPowerTemplate *power, Bool accepted) override;
	virtual void notifySpecialPowerFired(Object *source, const SpecialPowerTemplate *power) override;

public:	// AIPlayer interface methods.

	virtual void update() override;											///< simulates the behavior of a player

	virtual void newMap() override;											///< New map loaded call.

	/// Invoked when a unit I am training comes into existence
	virtual void onUnitProduced( Object *factory, Object *unit, Int productionID ) override;

	/// Invoked when a structure I am building becomes complete.
	virtual void onStructureProduced( Object *factory, Object *structure ) override;

	virtual void buildSpecificAITeam(TeamPrototype *teamProto, Bool priorityBuild) override; ///< Builds this team immediately.

	virtual void buildSpecificAIBuilding(const AsciiString &thingName) override; ///< Builds this building as soon as possible.

	virtual void buildAIBaseDefense(Bool flank) override; ///< Builds base defense on front or flank of base.

	virtual void buildAIBaseDefenseStructure(const AsciiString &thingName, Bool flank) override; ///< Builds base defense on front or flank of base.

	virtual void recruitSpecificAITeam(TeamPrototype *teamProto, Real recruitRadius) override; ///< Builds this team immediately.

	virtual Bool isSkirmishAI() override {return true;}
	Bool usesCriticalRecoveryBehavior() const;
	Bool canSpendForCriticalRecovery(Int cost, const ThingTemplate *thing,
		Bool isUpgrade, Bool refreshProductionReserve);

	virtual Bool checkBridges(Object *unit, Waypoint *way) override;

	virtual Player *getAiEnemy() override;	///< Solo AI attacks based on scripting.  Only skirmish auto-acquires an enemy at this point.  jba.
	virtual Player *getCachedAiEnemy() const override { return m_currentEnemy; }

protected:

	// snapshot methods
	virtual void crc( Xfer *xfer ) override;
	virtual void xfer( Xfer *xfer ) override;
	virtual void loadPostProcess() override;

	virtual void doBaseBuilding() override;
	virtual void checkReadyTeams() override;
	virtual Bool canActivateReadyTeam( const TeamInQueue *team ) const override;
	virtual void checkQueuedTeams() override;
	virtual void doTeamBuilding() override;
	virtual Object *findDozer(const Coord3D *pos) override;
	virtual void queueDozer() override;

protected:

	virtual Bool selectTeamToBuild() override;			///< determine the next team to build
	virtual Bool selectTeamToReinforce( Int minPriority ) override;			///< determine the next team to reinforce
	virtual Bool startTraining( WorkOrder *order, Bool busyOK, AsciiString teamName) override;	///< find a production building that can handle the order, and start building

	virtual Bool isAGoodIdeaToBuildTeam( TeamPrototype *proto ) override;		///< return true if team should be built
	virtual void processBaseBuilding() override;		///< do base-building behaviors
	virtual void processTeamBuilding() override;		///< do team-building behaviors

protected:
	void adjustBuildList(BuildListInfo *list);
	Int getMyEnemyPlayerIndex();
	void acquireEnemy();
	void acquireEnemyLegacy();
	Bool isAdaptiveProductionCandidate( TeamPrototype *proto, SkirmishAICostRange *costRange,
		Int *factoryWaitFrames );
	Int getActiveRecoveryReserveCost() const;
	Int getCriticalRebuildReserve( Bool *canStartNow );
	Bool canStartCriticalRebuildNow( BuildListInfo *info, const ThingTemplate *plan );
	void updateCriticalRecovery();
	Bool findPrimaryCommandCenter( const ThingTemplate *primaryTemplate, Object **center ) const;
	BuildListInfo *findPrimaryCommandCenterBuildInfo( const ThingTemplate *primaryTemplate ) const;
	Bool findRecoveryBuilderTemplateAndFactory(
		const ThingTemplate *primaryTemplate,
		const ThingTemplate **builderTemplate, Object **factory,
		Bool *hasPotentialFactory, Bool *hasBoundedFactory);
	void normalizeRecoveryWorkOrders(const ThingTemplate *primaryTemplate);
	Bool hasRecoveryBuilderQueued(
		const ThingTemplate *primaryTemplate, Bool *paid, ObjectID *factoryID,
		ProductionID *productionID );
	Bool queueRecoveryBuilder( const ThingTemplate *builderTemplate, Object *factory );
	void clearRecoveryBuilderProduction();
	void validateRecoveryBuilderProduction();
	void bindRecoveryBuilderProductionIfNeeded(
		Bool hasCompletedPrimaryCenter, Bool paidQueueExists,
		ObjectID factoryID, ProductionID productionID );
	Bool failoverRecoveryBuilderQueue(
		const ThingTemplate *primaryTemplate, Object *boundedFactory,
		ProductionID boundedProductionID );
	Bool cancelRecoveryBuilderQueueForNativeRespawn(
		const ThingTemplate *primaryTemplate );
	Object *findRecoveryBuilder(
		const Coord3D *position, const ThingTemplate *primaryTemplate) const;
	Bool hasCriticalRecoveryPlacementRoute(
		const ThingTemplate *primaryTemplate, Object *builder) const;
	Bool prepareCriticalRecoveryBuilder(Object *builder);
	Bool tryCriticalCommandCenterConstruction(
		const ThingTemplate *primaryTemplate, BuildListInfo *info, Object *builder);
	void enterRecoveryLastStand(Bool permanent);
	Bool estimateTeamProduction( TeamPrototype *proto, Bool planned,
		Int *productionCost, Int *completionFrames );
	void getVisibleEnemyComposition( Int *aircraftValue, Int *vehicleValue, Int *infantryValue,
		Coord3D *routeTarget, Bool *hasRouteTarget );
	Int getCandidateCounterFit( TeamPrototype *proto, Int aircraftValue, Int vehicleValue, Int infantryValue );
	SkirmishAIRouteClass classifyTeamRoute( TeamPrototype *proto, const Coord3D *routeTarget, Bool hasRouteTarget );
	Bool getKnownEnemyPosition( Player *enemy, Coord3D *position ) const;
	Int getKnownEnemyAssetValue( Player *enemy, Bool *hasKnownObject,
		Bool *hasKnownUnit, Bool *hasKnownBuildFacility ) const;
	Object *findEnemyRouteRepresentative() const;
	SkirmishAITargetRouteClass classifyEnemyRoute(
		Object *representative, const Coord3D *enemyPosition, Bool hasEnemyPosition ) const;
	Int countAlliedSkirmishAIsTargeting( Player *enemy ) const;
	SkirmishAIDecisionDifficulty getDecisionDifficulty() const;
	Bool usesStrategyBehavior() const;
	Bool usesProductionBehavior() const;
	void clearStrategySourceCommandLock();
	Bool isStrategySourceCommandLockValid() const;
	Bool hasUsableSupplySource(const Coord3D *position, Real centerRadius) const;
	Bool hasOwnedSupplyCenter(const ThingTemplate *supplyPlan) const;
	Bool hasQueuedSupplyCenter(const ThingTemplate *supplyPlan) const;
	Bool hasUsableSupplyCenterForCollectors() const;
	void cancelDepletedCollectorProduction();
	Bool isSupplyCenterPrerequisiteNeeded(const ThingTemplate *supplyPlan) const;
	void refreshStrategyProductionReserve();
	Bool queueAuthorizedStrategyBuilder(const ThingTemplate *structure);
	void refreshStrategyProductionState();
	Bool updateStrategy();
	void collectStrategyMetrics( SkirmishStrategyMetrics *metrics,
		ObjectID *strategicTargetID );
	void applyStrategyMode( SkirmishStrategyMode previousMode,
		SkirmishStrategyMode currentMode, ObjectID previousTargetID );
	void commandOffensiveTeams( SkirmishStrategyMode mode, Object *target );
	void updateTacticalTeams();
	struct TacticalTeamState;
	Bool tryTunnelBypass(Team *team, Object *target,
		Object *blockingDefense, AIGroup *group,
		TacticalTeamState &state, Object *representative,
		Bool hasAircraft, Int memberCount, UnsignedInt now,
		Int *aggregatePathQueryCount);
	void updateTunnelTransit(Team *team, TacticalTeamState &state,
		UnsignedInt now);
	void updateDefensePatrol();
	const ThingTemplate *findTunnelContainBuildTemplate() const;
	Bool isTunnelBuildBuilderAvailable(Object *builder) const;
	Bool validatePendingTunnelBuild(BuildListInfo *info,
		const ThingTemplate *plan, Object **builder,
		Bool *permanentFailure);
	Bool tryQueueTunnelEndpoint(Team *team, Object *target,
		Object *blockingDefense, Bool homeEndpoint, UnsignedInt now,
		Int *attemptPathQueryCount, Int *aggregatePathQueryCount,
		TacticalTeamState &state, Bool *deferred);
	Bool isPendingTunnelBuildInfo(const BuildListInfo *info,
		const ThingTemplate *plan) const;
	void abandonTunnelBuildPlan(UnsignedInt now, Bool allowRetry);
	void xferTacticalTeams( Xfer *xfer, XferVersion version );
	void crcTacticalTeams( Xfer *xfer );

	enum
	{
		SKIRMISH_AI_TUNNEL_TRANSIT_NONE = 0,
		SKIRMISH_AI_TUNNEL_TRANSIT_ENTERING = 1,
		SKIRMISH_AI_TUNNEL_TRANSIT_EXITING = 2,
		SKIRMISH_AI_TUNNEL_TRANSIT_FALLBACK_EXIT = 3
	};

	enum
	{
		SKIRMISH_AI_TUNNEL_BUILD_NONE = 0,
		SKIRMISH_AI_TUNNEL_BUILD_HOME_QUEUED = 1,
		SKIRMISH_AI_TUNNEL_BUILD_FORWARD_QUEUED = 2,
		SKIRMISH_AI_TUNNEL_BUILD_HOME_CONSTRUCTING = 3,
		SKIRMISH_AI_TUNNEL_BUILD_FORWARD_CONSTRUCTING = 4,
		SKIRMISH_AI_TUNNEL_BUILD_HOME_RETRY_AVAILABLE = 5,
		SKIRMISH_AI_TUNNEL_BUILD_FORWARD_RETRY_AVAILABLE = 6
	};

	struct TacticalTeamState
	{
		struct TunnelEndpointProbe
		{
			ObjectID objectID;
			Int entryResult;
			Int exitResult;
			TunnelEndpointProbe() : objectID(INVALID_ID),
				entryResult(0), exitResult(0) {}
		};
		ObjectID targetID;
		Real targetHealth;
		Real distanceToTargetSqr;
		UnsignedInt lastProgressFrame;
		UnsignedInt nextCheckFrame;
		UnsignedInt regroupUntilFrame;
		UnsignedInt blockedSinceFrame;
		UnsignedInt retreatStartFrame;
		UnsignedInt nextRetreatFrame;
		UnsignedInt retreatProbeCursor;
		UnsignedInt routeExhaustedUntilFrame;
		ObjectID routeExhaustedTargetID;
		ObjectID alternateProbeAfterID;
		ObjectID alternateAttackIssuedTargetID;
		UnsignedInt tunnelPhaseDeadlineFrame;
		UnsignedInt tunnelCooldownUntilFrame;
		Int approachAttempt;
		Int tunnelTransitPhase;
		Int tunnelMemberCount;
		ObjectID tunnelMemberIDs[32];
		UnsignedInt tunnelPairCursor;
		UnsignedInt tunnelPairSweepRemaining;
		UnsignedInt tunnelPairSweepStartFrame;
		UnsignedInt tunnelPairResumeAfterFrame;
		UnsignedInt tunnelPairRetryAfterFrame;
		UnsignedInt tunnelPairRefreshCursor;
		UnsignedInt tunnelPairRefreshRemaining;
		UnsignedInt tunnelPairRefreshAfterFrame;
		UnsignedInt tunnelPairRefreshCacheAfterFrame;
		Bool tunnelPairRefreshYieldMain;
		UnsignedInt tunnelPairEndpointSignature;
		UnsignedInt tunnelProbeMovementSignature;
		ObjectID tunnelPairSweepTargetID;
		Int tunnelProbeMemberCount;
		ObjectID tunnelProbeMemberIDs[32];
		std::vector<TunnelEndpointProbe> tunnelEndpointProbes;
		ObjectID tunnelWaitTargetID;
		UnsignedInt tunnelSiteCursor;
		UnsignedInt tunnelBuilderWaitUntilFrame;
		UnsignedInt tunnelScaffoldWaitUntilFrame;
		ObjectID tunnelScaffoldWaitObjectID;
		UnsignedInt tunnelBuilderCursor;
		UnsignedInt tunnelBuilderSignature;
		UnsignedInt tunnelBuilderWindowsRemaining;
		UnsignedInt tunnelForwardEntryCursor;
		UnsignedInt tunnelForwardEntryRemaining;
		UnsignedInt tunnelForwardEntrySignature;
		ObjectID tunnelForwardEntryTargetID;
		UnsignedInt tunnelForwardEntryRetryAfterFrame;
		ObjectID tunnelCapacityWaitTargetID;
		UnsignedInt tunnelCapacityWaitDeadlineFrame;
		ObjectID tunnelEntryID;
		ObjectID tunnelExitID;
		ObjectID tunnelTargetID;
		ObjectID tunnelStrategicTargetID;
		UnsignedInt tunnelCommittedTargetLastSeenFrame;
		Bool retreating;
		Bool woundedReserve;
		TacticalTeamState() : targetID(INVALID_ID), targetHealth(0.0f),
			distanceToTargetSqr(0.0f), lastProgressFrame(0),
			nextCheckFrame(0), regroupUntilFrame(0), blockedSinceFrame(0),
			retreatStartFrame(0), nextRetreatFrame(0),
			retreatProbeCursor(0),
			routeExhaustedUntilFrame(0), routeExhaustedTargetID(INVALID_ID),
			alternateProbeAfterID(INVALID_ID),
			alternateAttackIssuedTargetID(INVALID_ID),
			tunnelPhaseDeadlineFrame(0), tunnelCooldownUntilFrame(0),
			approachAttempt(0), tunnelTransitPhase(0),
			tunnelMemberCount(0), tunnelPairCursor(0),
			tunnelPairSweepRemaining(0), tunnelPairSweepStartFrame(0),
			tunnelPairResumeAfterFrame(0), tunnelPairRetryAfterFrame(0),
			tunnelPairRefreshCursor(0), tunnelPairRefreshRemaining(0),
			tunnelPairRefreshAfterFrame(0),
			tunnelPairRefreshCacheAfterFrame(0),
			tunnelPairRefreshYieldMain(false),
			tunnelPairEndpointSignature(0), tunnelProbeMovementSignature(0),
			tunnelPairSweepTargetID(INVALID_ID),
			tunnelProbeMemberCount(0),
			tunnelWaitTargetID(INVALID_ID), tunnelSiteCursor(0),
			tunnelBuilderWaitUntilFrame(0),
			tunnelScaffoldWaitUntilFrame(0),
			tunnelScaffoldWaitObjectID(INVALID_ID),
			tunnelBuilderCursor(0), tunnelBuilderSignature(0),
			tunnelBuilderWindowsRemaining(0),
			tunnelForwardEntryCursor(0), tunnelForwardEntryRemaining(0),
			tunnelForwardEntrySignature(0),
			tunnelForwardEntryTargetID(INVALID_ID),
			tunnelForwardEntryRetryAfterFrame(0),
			tunnelCapacityWaitTargetID(INVALID_ID),
			tunnelCapacityWaitDeadlineFrame(0),
			tunnelEntryID(INVALID_ID),
			tunnelExitID(INVALID_ID), tunnelTargetID(INVALID_ID),
			tunnelStrategicTargetID(INVALID_ID),
			tunnelCommittedTargetLastSeenFrame(0),
			retreating(false), woundedReserve(false) {
			for (Int i = 0; i < 32; ++i) {
				tunnelMemberIDs[i] = INVALID_ID;
				tunnelProbeMemberIDs[i] = INVALID_ID;
			}
		}
	};

protected:
	void xferTunnelEndpointProbes(Xfer *xfer, TacticalTeamState &state);
	void xferGeneratedDefenseBuilds(Xfer *xfer);
	struct GeneratedDefenseBuild
	{
		AsciiString templateName;
		Coord2D location;
		Real angle;
		GeneratedDefenseBuild() : angle(0.0f) {
			location.x = 0.0f;
			location.y = 0.0f;
		}
	};
	std::vector<GeneratedDefenseBuild> m_generatedDefenseBuilds;

	Int m_curFrontBaseDefense; // First is 0.
	Int m_curFlankBaseDefense; // First is 0.
	Real m_curFrontLeftDefenseAngle;
	Real m_curFrontRightDefenseAngle;
	Real m_curLeftFlankLeftDefenseAngle;
	Real m_curLeftFlankRightDefenseAngle;
	Real m_curRightFlankLeftDefenseAngle;
	Real m_curRightFlankRightDefenseAngle;

	UnsignedInt m_frameToCheckEnemy;
	Player			*m_currentEnemy;
	Int m_currentEnemyPlayerIndex;
	SkirmishStrategyState m_strategyState;
	Bool m_strategyTargetFallbackPending;
	ObjectID m_strategyTargetFallbackAfterID;
	Int m_strategyTargetFallbackEnemyIndex;
	Int m_strategyProductionReserveCost;
	ObjectID m_strategySuperweaponID;
	const ThingTemplate *m_strategyAuthorizedThing;
	SkirmishAISpendAuthorization m_strategySpendAuthorization;
	Bool m_strategyProductionReserveRefreshing;
	Bool m_strategyProductionReserveLoaded;
	Bool m_strategySourceCommandLocked;
	ObjectID m_strategyLockedSourceID;
	UnsignedInt m_strategyLockedPowerID;
	UnsignedInt m_reinforcementRoundRobinCursor;
	std::map<UnsignedInt, TacticalTeamState> m_tacticalTeams;
	UnsignedInt m_tacticalNextTeamScanFrame;
	Int m_tunnelBuildPhase;
	Bool m_tunnelHomeAttempted; // Retry consumed, or a home endpoint succeeded.
	Bool m_tunnelForwardAttempted; // Retry consumed, or a forward endpoint succeeded.
	Bool m_tunnelForwardRetryConsumed; // Retry belongs to the current forward target.
	ObjectID m_tunnelHomeEndpointID;
	ObjectID m_tunnelForwardEndpointID;
	ObjectID m_tunnelForwardAttemptTargetID;
	ObjectID m_tunnelGeneratedForwardEndpointIDs[
		SkirmishAITunnelRoute::MAX_GENERATED_FORWARD_ENDPOINTS];
	ObjectID m_tunnelGeneratedForwardTargetIDs[
		SkirmishAITunnelRoute::MAX_GENERATED_FORWARD_ENDPOINTS];
	ObjectID m_tunnelExhaustedForwardTargetIDs[
		SkirmishAITunnelRoute::MAX_EXHAUSTED_FORWARD_TARGETS];
	ObjectID m_tunnelBuildBuilderID;
	ObjectID m_tunnelBuildTargetID;
	ObjectID m_tunnelBuildBlockerID;
	ObjectID m_tunnelBuildObjectID;
	ObjectID m_tunnelBuildLockedBuilderID;
	UnsignedInt m_tunnelPendingBuilderCursor;
	Coord3D m_tunnelBuildLocation;
	ObjectID m_defenseBuildLockedBuilderID;
	Coord3D m_defenseBuildLockedLocation;
	UnsignedInt m_tunnelBuildDeadlineFrame;
	UnsignedInt m_tunnelBuildCooldownUntilFrame;
	Int m_defensePatrolRoute;
	UnsignedInt m_defensePatrolTeamID;
	ObjectID m_defensePatrolObjectID;
	UnsignedInt m_defenseNextPatrolFrame;
	Coord3D m_defenseQuietPatrolWaypoint;
	UnsignedInt m_defenseQuietPatrolDeadlineFrame;
	UnsignedInt m_defensePlacementAttempt;
	UnsignedInt m_defensePlacementNextFrame;
	ObjectID m_defensePursuitTargetID;
	UnsignedInt m_defensePursuitStartFrame;
	ObjectID m_defenseInterceptProbeAfterID;
	ObjectID m_defensePatrolRouteMemberAfterID;
	ObjectID m_defenseInterceptMemberAfterID;
	std::map<ObjectID, Bool> m_stage3CollectorRolesToRestore;

	// Critical command-center recovery state. The reserve is serialized because
	// it gates same-frame production before the next AI refresh.
	Bool m_recoveryEverCompleted;
	Bool m_recoveryImpossible;
	ObjectID m_recoveryConstructionID;
	// Modulo the placement offset count is the next site; the next integer band
	// records that this scaffold already received one paid replacement attempt.
	Int m_recoveryPlacementAttempt;
	UnsignedInt m_recoveryNextAttemptFrame;
	UnsignedInt m_recoveryEvacuationDeadline;
	Coord3D m_recoveryLocation;
	Real m_recoveryAngle;
	Int m_recoveryReserveCost;
	ObjectID m_recoveryBuilderFactoryID;
	ProductionID m_recoveryBuilderProductionID;
	Bool m_recoveryBuilderCancellationOwned;
	Bool m_recoveryBuilderFailoverConsumed;
	const ThingTemplate *m_recoveryAuthorizedThing;

};
