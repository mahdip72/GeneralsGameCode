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

// FILE: HordeUpdate.cpp ////////////////////////////////////////////////////////////////////////
// Author: Steven Johnson, Feb 2002
// Desc:   HordeUpdate module
///////////////////////////////////////////////////////////////////////////////////////////////////

// INCLUDES ///////////////////////////////////////////////////////////////////////////////////////
#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#define DEFINE_HORDEACTION_NAMES
#include "Common/Player.h"
#include "Common/ThingTemplate.h"
#include "Common/Upgrade.h"
#include "Common/Xfer.h"
#include "Common/BitFlagsIO.h"
#include "GameLogic/PartitionManager.h"
#include "GameLogic/Weapon.h"
#include "GameLogic/Module/AIUpdate.h"
#include "GameLogic/Module/HordeUpdate.h"
#include "GameClient/Drawable.h"

#if defined(_WIN64)
#include <algorithm>
#endif

//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------




static HordeUpdateInterface* getHUI(Object* obj)
{
	for( BehaviorModule** u = obj->getBehaviorModules(); *u; ++u )
	{
		HordeUpdateInterface* hui = (*u)->getHordeUpdateInterface();
		if( hui )
			return hui;
	}
	return nullptr;
}

//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------

class PartitionFilterHordeMember : public PartitionFilter
{
private:
	Object* m_obj;
	const HordeUpdateModuleData* m_data;

public:

	PartitionFilterHordeMember(Object* obj, const HordeUpdateModuleData* data) : m_obj(obj), m_data(data) { }

#if defined(RTS_DEBUG)
	virtual const char* debugGetName() override { return "PartitionFilterHordeMember"; }
#endif

	virtual Bool allow(Object *objOther) override
	{
#if !RETAIL_COMPATIBLE_CRC
		// TheSuperHackers @bugfix Stubbjax 07/08/2026 Prevent dead units from being considered for horde membership.
		if (objOther->isEffectivelyDead())
			return false;
#endif

		// must be exact same type as us (well, maybe)
		if (m_data->m_exactMatch && m_obj->getTemplate() != objOther->getTemplate())
			return false;

		// can only horde with other horde-able things
		HordeUpdateInterface *hui = getHUI(objOther);
		if( !hui )
			return false;

		// must match the kindof flags (if any)
		if (!objOther->isKindOfMulti(m_data->m_kindof, KINDOFMASK_NONE))
			return false;

		// must be our ally (well, maybe)
		if (m_data->m_alliesOnly && m_obj->getRelationship(objOther) != ALLIES)
			return false;

		// doh
		if (m_obj->isOffMap() != objOther->isOffMap())
			return false;

		// guess it's horde-worthy!
		return true;
	}
};

//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------

const Int DEFAULT_UPDATE_RATE = LOGICFRAMES_PER_SECOND;

//-------------------------------------------------------------------------------------------------
HordeUpdateModuleData::HordeUpdateModuleData()
	: m_updateRate(DEFAULT_UPDATE_RATE)
	, m_minCount(0)
	, m_minDist(0.0f)
	, m_rubOffRadius(20.0f)
	, m_alliesOnly(true)
	, m_exactMatch(false)
	, m_allowedNationalism(TRUE)
	, m_action(HORDEACTION_DEFAULT)
{
}

//-------------------------------------------------------------------------------------------------
/*static*/ void HordeUpdateModuleData::buildFieldParse(MultiIniFieldParse& p)
{
	ModuleData::buildFieldParse(p);

	static const FieldParse dataFieldParse[] =
	{
		{ "UpdateRate", INI::parseDurationUnsignedInt, nullptr, offsetof(HordeUpdateModuleData, m_updateRate) },
		{ "KindOf", KindOfMaskType::parseFromINI, nullptr, offsetof(HordeUpdateModuleData, m_kindof) },
		{ "Count", INI::parseInt, nullptr, offsetof(HordeUpdateModuleData, m_minCount) },
		{ "Radius", INI::parseReal, nullptr, offsetof(HordeUpdateModuleData, m_minDist) },
		{ "RubOffRadius", INI::parseReal, nullptr, offsetof(HordeUpdateModuleData, m_rubOffRadius) },
		{ "AlliesOnly", INI::parseBool, nullptr, offsetof(HordeUpdateModuleData, m_alliesOnly) },
		{ "ExactMatch", INI::parseBool, nullptr, offsetof(HordeUpdateModuleData, m_exactMatch) },
		{ "Action", INI::parseIndexList, TheHordeActionTypeNames, offsetof(HordeUpdateModuleData, m_action) },
		{ "FlagSubObjectNames", INI::parseAsciiStringVector, nullptr, offsetof(HordeUpdateModuleData, m_flagSubObjNames) },
		{ "AllowedNationalism", INI::parseBool, nullptr, offsetof(HordeUpdateModuleData, m_allowedNationalism) },
		{ nullptr, nullptr, nullptr, 0 }
	};
	p.add(dataFieldParse);
}

//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------

//-------------------------------------------------------------------------------------------------
HordeUpdate::HordeUpdate( Thing *thing, const ModuleData* moduleData ) : UpdateModule( thing, moduleData )
{
	m_inHorde = FALSE;
	m_hasFlag = FALSE;
	m_lastHordeRefreshFrame = TheGameLogic->getFrame();
	m_trueHordeMember = FALSE;

	UnsignedInt delay = getHordeUpdateModuleData()->m_updateRate;
	setWakeFrame(getObject(), UPDATE_SLEEP(GameLogicRandomValue(1, delay)));
}

//-------------------------------------------------------------------------------------------------
HordeUpdate::~HordeUpdate()
{

}

//-------------------------------------------------------------------------------------------------
Bool HordeUpdate::isAllowedNationalism() const
{
	const HordeUpdateModuleData *data = getHordeUpdateModuleData();
	return data->m_allowedNationalism;
}

//-------------------------------------------------------------------------------------------------
/** @todo I think we should model the horde list ... so we can do all this without doing
  * all this scanning, plus we can give exactly 1 flag to the right person in the
	* center of the horde which I think would look better (CBD)
	*
	* Redesign occurred 10/15, where the flags have been removed replaced by a teraindecal for each horde member
	* Thank You for reading, MLorenzen
*/
// ------------------------------------------------------------------------------------------------
void HordeUpdate::joinOrLeaveHorde(SimpleObjectIterator *iter, Bool join)
{
	// give/remove bonus effects
	if( m_inHorde != join )
	{
		m_inHorde = join;

		if( AIUpdateInterface *ai = getObject()->getAIUpdateInterface() )
		{
			const HordeUpdateModuleData* md = getHordeUpdateModuleData();
			ai->evaluateMoraleBonus(m_inHorde, md->m_allowedNationalism, md->m_action);
		}
		else
		{
			DEBUG_CRASH(( "HordeUpdate::joinOrLeaveHorde - We (%s) must have an AI to benefit from horde",
										getObject()->getTemplate()->getName().str() ));
		}
	}
}

//-------------------------------------------------------------------------------------------------
void HordeUpdate::showHideFlag(Bool show)
{
	const HordeUpdateModuleData* d = getHordeUpdateModuleData();
	if (!d->m_flagSubObjNames.empty())
	{
		Drawable* draw = getObject()->getDrawable();
		Object *obj = getObject();
		if (draw && obj)
		{
			for (std::vector<AsciiString>::const_iterator it = d->m_flagSubObjNames.begin(); it != d->m_flagSubObjNames.end(); ++it)
				draw->showSubObject(*it, show);
			draw->updateSubObjects();
		}
	}
}

//-------------------------------------------------------------------------------------------------
/**
	this is called whenever a drawable is bound to the object.
	drawable is NOT guaranteed to be non-null.
*/
void HordeUpdate::onDrawableBoundToObject()
{
	/////////////showHideFlag(m_hasFlag);
	showHideFlag(FALSE);//hide the flag
}

//-------------------------------------------------------------------------------------------------
UpdateSleepTime HordeUpdate::update()
{
	return updateImpl(nullptr, 0, FALSE);
}

#if defined(_WIN64)
//-------------------------------------------------------------------------------------------------
Bool HordeUpdate::isObjectComputationScanDue(UnsignedInt frame) const
{
	const Object *obj = getObject();
	const HordeUpdateModuleData *data = getHordeUpdateModuleData();
	return obj != nullptr && (obj->isKindOf(KINDOF_INFANTRY) ||
		frame > m_lastHordeRefreshFrame + data->m_updateRate);
}

//-------------------------------------------------------------------------------------------------
UpdateSleepTime HordeUpdate::objectComputationSleepTime() const
{
	return getObject()->isKindOf(KINDOF_INFANTRY) ?
		UPDATE_SLEEP(getHordeUpdateModuleData()->m_updateRate) :
		UPDATE_SLEEP_NONE;
}

//-------------------------------------------------------------------------------------------------
Bool HordeUpdate::captureObjectComputationOwnerSnapshot(UnsignedInt frame,
	UnsignedInt moduleOrdinal, ObjectComputationOwnerSnapshot &snapshot) const
{
	const Object *obj = getObject();
	if (obj == nullptr || !isObjectComputationScanDue(frame)) return FALSE;
	snapshot.frame = frame;
	snapshot.wakePriority = friend_getPriority();
	snapshot.motionGeneration = obj->getMotionGeneration();
	snapshot.lastRefreshFrame = m_lastHordeRefreshFrame;
	snapshot.moduleOrdinal = moduleOrdinal;
	snapshot.inHorde = m_inHorde;
	snapshot.trueHordeMember = m_trueHordeMember;
	snapshot.moduleData = getHordeUpdateModuleData();
	return TRUE;
}

//-------------------------------------------------------------------------------------------------
Bool HordeUpdate::validateObjectComputationOwnerSnapshot(
	const ObjectComputationOwnerSnapshot &snapshot) const
{
	const Object *obj = getObject();
	return obj != nullptr && TheGameLogic->getFrame() == snapshot.frame &&
		friend_getPriority() == snapshot.wakePriority &&
		obj->getMotionGeneration() == snapshot.motionGeneration &&
		m_lastHordeRefreshFrame == snapshot.lastRefreshFrame &&
		m_inHorde == snapshot.inHorde &&
		m_trueHordeMember == snapshot.trueHordeMember &&
		getHordeUpdateModuleData() == snapshot.moduleData &&
		isObjectComputationScanDue(snapshot.frame);
}

//-------------------------------------------------------------------------------------------------
Bool HordeUpdate::collectObjectComputationCandidateOracle(ObjectID *objectIDs,
	UnsignedInt capacity, UnsignedInt *count) const
{
	if (objectIDs == nullptr || count == nullptr) return FALSE;
	*count = 0;
	PartitionFilter *filters[] = { nullptr };
	SimpleObjectIterator *iter = ThePartitionManager->iterateObjectsInRange(
		getObject(), getHordeUpdateModuleData()->m_minDist,
		FROM_BOUNDINGSPHERE_3D, filters);
	MemoryPoolObjectHolder hold(iter);
	for (Object *candidate = iter->first(); candidate; candidate = iter->next())
	{
		if (*count == capacity) return FALSE;
		objectIDs[(*count)++] = candidate->getID();
	}
	std::sort(objectIDs, objectIDs + *count);
	return TRUE;
}

//-------------------------------------------------------------------------------------------------
UpdateSleepTime HordeUpdate::updateFromObjectComputationCandidates(
	Object *const *candidates, UnsignedInt candidateCount)
{
	return updateImpl(candidates, candidateCount, TRUE);
}
#endif

//-------------------------------------------------------------------------------------------------
UpdateSleepTime HordeUpdate::updateImpl(Object *const *preparedCandidates,
	UnsignedInt preparedCandidateCount, Bool usePreparedCandidates)
{


	//This code handles decals and ONLY decals!
	Object *obj = getObject();

	if ( ! obj )
		return UPDATE_SLEEP_FOREVER;

	const HordeUpdateModuleData *md = getHordeUpdateModuleData();

	Bool wasInHorde = m_inHorde;

	// This is a sticky situation, where refreshing the model state (like from default to damaged, for example)
	// will rebuild the terrain decal and set its size to the default size.... since Vehicles have a special size,
	// we want to keep it fresh, here, but not do the hording test every frame...
	Bool isInfantry = ( obj->isKindOf(KINDOF_INFANTRY) );
	if ( isInfantry || (TheGameLogic->getFrame() > m_lastHordeRefreshFrame + md->m_updateRate) )
	{
		m_lastHordeRefreshFrame = TheGameLogic->getFrame();

		PartitionFilterHordeMember hmFilter(getObject(), md);
		Int eligibleCount = 0;
		Bool rubOff = FALSE;
		MemoryPoolObjectHolder hold;
		if (usePreparedCandidates)
		{
			for (UnsignedInt candidateIndex = 0;
				candidateIndex != preparedCandidateCount; ++candidateIndex)
			{
				Object *other = preparedCandidates[candidateIndex];
				if (other != nullptr && hmFilter.allow(other)) ++eligibleCount;
			}
		}
		else
		{
			PartitionFilter *filters[] = { &hmFilter, nullptr };
			SimpleObjectIterator *iter = ThePartitionManager->iterateObjectsInRange(
				getObject(), md->m_minDist, FROM_BOUNDINGSPHERE_3D, filters);
			hold.hold(iter);
			eligibleCount = iter->getCount();
			if (eligibleCount < md->m_minCount - 1)
			{
				const Real rubOffRadiusSq = sqr(md->m_rubOffRadius);
				for (Object *other = iter->first(); other; other = iter->next())
				{
					HordeUpdateInterface *hui = getHUI(other);
					if (hui != nullptr && hui->isTrueHordeMember() &&
						ThePartitionManager->getDistanceSquared(getObject(), other,
							FROM_CENTER_2D) <= rubOffRadiusSq)
					{
						rubOff = TRUE;
						break;
					}
				}
			}
		}

		if ((eligibleCount >= md->m_minCount - 1) )//we really are in the thick part of the horde
		{
			m_inHorde = TRUE;
			m_trueHordeMember = TRUE;
		}
		else
		{
			m_inHorde = FALSE;
			m_trueHordeMember = FALSE;/// unless...

			if (usePreparedCandidates)
			{
				const Real rubOffRadiusSq = sqr(md->m_rubOffRadius);
				for (UnsignedInt candidateIndex = 0;
					candidateIndex != preparedCandidateCount; ++candidateIndex)
				{
					Object *other = preparedCandidates[candidateIndex];
					if (other == nullptr || !hmFilter.allow(other)) continue;
					HordeUpdateInterface *hui = getHUI(other);
					if (hui != nullptr && hui->isTrueHordeMember() &&
						ThePartitionManager->getDistanceSquared(getObject(), other,
							FROM_CENTER_2D) <= rubOffRadiusSq)
					{
						rubOff = TRUE;
						break;
					}
				}
			}
			m_inHorde = rubOff;
		}

		AIUpdateInterface *ai = getObject()->getAIUpdateInterface();
		if( ai )
			ai->evaluateMoraleBonus(m_inHorde, md->m_allowedNationalism, md->m_action);

	}






	// This is a sticky situation, where refreshing the model state (like from default to damaged, for example)
	// will rebuild the terrain decal and set its size to the default size.... since Vehicles have a special size,
	// we want to keep it fresh, here, but not do the hording test every frame...
	// This is a weak solution, in that It causes this update to fight the default behavior of the model state methods,
	// But in the interest of not breaking the model state changing logic for five hundred other units a week before golden,
	// this is my solution... If anyone gets this note on the next project... please please please, fix the resetting of the
	// shadows/terrain decals in W3DModelDraw. Thanks, ML

	Drawable* draw = getObject()->getDrawable();
	if ( draw && ! obj->isEffectivelyDead() )
	{
		if( TheGameLogic->getDrawIconUI() )
		{
			if(m_inHorde && !obj->isKindOf( KINDOF_PORTABLE_STRUCTURE ) )// this not is a ride-on for overlord
			{

				TerrainDecalType nuType;// uninitialized

				if ( isInfantry )
				{
					if( obj->testWeaponBonusCondition( WEAPONBONUSCONDITION_NATIONALISM ) == TRUE )
          {
            if ( obj->testWeaponBonusCondition( WEAPONBONUSCONDITION_FANATICISM ) == TRUE )
  						nuType = ( TERRAIN_DECAL_HORDE_WITH_FANATICISM_UPGRADE );
            else
  						nuType = ( TERRAIN_DECAL_HORDE_WITH_NATIONALISM_UPGRADE );
          }
					else
						nuType =(TERRAIN_DECAL_HORDE);

					if (nuType != draw->getTerrainDecalType())
						draw->setTerrainDecal(nuType);
				}
				else
				{
					Real size = 3.5f * obj->getGeometryInfo().getMajorRadius();
					draw->setTerrainDecalSize( size, size );

					if( obj->testWeaponBonusCondition( WEAPONBONUSCONDITION_NATIONALISM ) == TRUE )
          {
            if ( obj->testWeaponBonusCondition( WEAPONBONUSCONDITION_FANATICISM ) == TRUE )
  						nuType = ( TERRAIN_DECAL_HORDE_WITH_FANATICISM_UPGRADE );
            else
  						nuType = ( TERRAIN_DECAL_HORDE_WITH_NATIONALISM_UPGRADE );
          }
					else
						nuType = (TERRAIN_DECAL_HORDE_VEHICLE);

					if (nuType != draw->getTerrainDecalType())
						draw->setTerrainDecal(nuType);
				}
			}


		}
		else
		{
			//Scripts have disabled icons so don't draw these!
			draw->setTerrainDecal( TERRAIN_DECAL_NONE );
		}

		if ( ! wasInHorde && m_inHorde  )
				draw->setTerrainDecalFadeTarget(1.0f, 0.03f);
		else if ( wasInHorde && ! m_inHorde )
				draw->setTerrainDecalFadeTarget(0.0f, -0.03f);
	}


	return isInfantry ? UPDATE_SLEEP(md->m_updateRate) : UPDATE_SLEEP_NONE;

}

// ------------------------------------------------------------------------------------------------
/** CRC */
// ------------------------------------------------------------------------------------------------
void HordeUpdate::crc( Xfer *xfer )
{

	// extend base class
	UpdateModule::crc( xfer );

}

// ------------------------------------------------------------------------------------------------
/** Xfer method
	* Version Info:
	* 1: Initial version */
// ------------------------------------------------------------------------------------------------
void HordeUpdate::xfer( Xfer *xfer )
{

	// version
	XferVersion currentVersion = 1;
	XferVersion version = currentVersion;
	xfer->xferVersion( &version, currentVersion );

	// extend base class
	UpdateModule::xfer( xfer );

	xfer->xferBool( &m_inHorde );
	xfer->xferBool( &m_hasFlag );

}

// ------------------------------------------------------------------------------------------------
/** Load post process */
// ------------------------------------------------------------------------------------------------
void HordeUpdate::loadPostProcess()
{

	// extend base class
	UpdateModule::loadPostProcess();

}
