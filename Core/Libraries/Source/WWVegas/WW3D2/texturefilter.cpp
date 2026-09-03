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

/***********************************************************************************************
 ***              C O N F I D E N T I A L  ---  W E S T W O O D  S T U D I O S               ***
 ***********************************************************************************************
 *                                                                                             *
 *                 Project Name : WW3D                                                         *
 *                                                                                             *
 *                     $Archive:: ww3d2/texturefilter.cpp												$*
 *                                                                                             *
 *                  $Org Author:: Kenny Mitchell                                              $*
 *                                                                                             *
 *                       Author : Kenny Mitchell                                               *
 *                                                                                             *
 *                     $Modtime:: 08/05/02 1:27p                                              $*
 *                                                                                             *
 *                    $Revision:: 1                                                          $*
 *                                                                                             *
 * 08/05/02 KM Texture filter class abstraction																			*
 *---------------------------------------------------------------------------------------------*
 * Functions:                                                                                  *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#include "Utility/CppMacros.h"
#include "texturefilter.h"
#include "Renderer/RenderGameClient.h"

const char* const TextureFilterClass::TextureFilterModeString[TEXTURE_FILTER_COUNT] = {
	"None",
	"Point",
	"Bilinear",
	"Trilinear",
	"Anisotropic"
};

TextureFilterClass::TextureFilterMode TextureFilterClass::getTextureFilterMode(const char* str) {
	for (int i = 0; i < TextureFilterClass::TEXTURE_FILTER_COUNT; ++i) {
		if (stricmp(str, TextureFilterClass::TextureFilterModeString[i]) == 0) {
			return (TextureFilterClass::TextureFilterMode)i;
		}
	}

	return TextureFilterClass::TEXTURE_FILTER_NONE;
}

unsigned _MinTextureFilters[rts::render::LEGACY_TEXTURE_STAGE_COUNT][TextureFilterClass::FILTER_TYPE_COUNT];
unsigned _MagTextureFilters[rts::render::LEGACY_TEXTURE_STAGE_COUNT][TextureFilterClass::FILTER_TYPE_COUNT];
unsigned _MipMapFilters[rts::render::LEGACY_TEXTURE_STAGE_COUNT][TextureFilterClass::FILTER_TYPE_COUNT];

/*************************************************************************
**                             TextureFilterClass
*************************************************************************/
TextureFilterClass::TextureFilterClass(MipCountType mip_level_count)
:	TextureMinFilter(FILTER_TYPE_DEFAULT),
	TextureMagFilter(FILTER_TYPE_DEFAULT),
	UAddressMode(TEXTURE_ADDRESS_REPEAT),
	VAddressMode(TEXTURE_ADDRESS_REPEAT)
{
	if (mip_level_count!=MIP_LEVELS_1)
	{
		MipMapFilter=FILTER_TYPE_DEFAULT;
	}
	else
	{
		MipMapFilter=FILTER_TYPE_NONE;
	}
}

//**********************************************************************************************
//! Apply filters (legacy)
/*!
*/
void TextureFilterClass::Apply(unsigned int stage)
{
	if (stage >= rts::render::LEGACY_TEXTURE_STAGE_COUNT)
	{
		// Let the seam own invalid-argument reporting instead of indexing the
		// process-wide filter tables out of bounds.
		rts::render::SetGameTextureStageState(
			stage, rts::render::GAME_TEXTURE_STAGE_MINIFICATION_FILTER,
			rts::render::RENDER_TEXTURE_FILTER_NONE);
		return;
	}

	rts::render::SetGameTextureStageState(stage,
		rts::render::GAME_TEXTURE_STAGE_MINIFICATION_FILTER,
		_MinTextureFilters[stage][TextureMinFilter]);
	rts::render::SetGameTextureStageState(stage,
		rts::render::GAME_TEXTURE_STAGE_MAGNIFICATION_FILTER,
		_MagTextureFilters[stage][TextureMagFilter]);
	rts::render::SetGameTextureStageState(stage,
		rts::render::GAME_TEXTURE_STAGE_MIP_FILTER,
		_MipMapFilters[stage][MipMapFilter]);

	const unsigned int addressU = Get_U_Addr_Mode() == TEXTURE_ADDRESS_CLAMP ?
		rts::render::RENDER_TEXTURE_ADDRESS_CLAMP :
		rts::render::RENDER_TEXTURE_ADDRESS_WRAP;
	const unsigned int addressV = Get_V_Addr_Mode() == TEXTURE_ADDRESS_CLAMP ?
		rts::render::RENDER_TEXTURE_ADDRESS_CLAMP :
		rts::render::RENDER_TEXTURE_ADDRESS_WRAP;
	rts::render::SetGameTextureStageState(stage,
		rts::render::GAME_TEXTURE_STAGE_ADDRESS_U, addressU);
	rts::render::SetGameTextureStageState(stage,
		rts::render::GAME_TEXTURE_STAGE_ADDRESS_V, addressV);
}

//**********************************************************************************************
//! Init filters (legacy)
/*!
*/
void TextureFilterClass::_Init_Filters(TextureFilterMode texture_filter, AnisotropicFilterMode anisotropy_level)
{
	rts::render::GameTextureFilterCapabilities capabilities;
	const bool have_capabilities =
		rts::render::GetGameTextureFilterCapabilities(&capabilities) ==
		rts::render::RENDER_RESULT_OK;
	const bool supportsLinear = have_capabilities &&
		capabilities.supportsLinear;
	const bool supportsAnisotropic = have_capabilities &&
		capabilities.supportsAnisotropic;
	const unsigned int fastFilter = supportsLinear ?
		rts::render::RENDER_TEXTURE_FILTER_LINEAR :
		rts::render::RENDER_TEXTURE_FILTER_POINT;

	// Point filtering is the lowest-cost fallback for non-mipmapped textures.
	_MinTextureFilters[0][FILTER_TYPE_NONE] =
		rts::render::RENDER_TEXTURE_FILTER_POINT;
	_MagTextureFilters[0][FILTER_TYPE_NONE] =
		rts::render::RENDER_TEXTURE_FILTER_POINT;
	_MipMapFilters[0][FILTER_TYPE_NONE] =
		rts::render::RENDER_TEXTURE_FILTER_NONE;

	_MinTextureFilters[0][FILTER_TYPE_FAST] =
		fastFilter;
	_MagTextureFilters[0][FILTER_TYPE_FAST] =
		fastFilter;
	_MipMapFilters[0][FILTER_TYPE_FAST] =
		rts::render::RENDER_TEXTURE_FILTER_POINT;

	_MinTextureFilters[0][FILTER_TYPE_BEST] =
		rts::render::RENDER_TEXTURE_FILTER_ANISOTROPIC;
	_MagTextureFilters[0][FILTER_TYPE_BEST] =
		rts::render::RENDER_TEXTURE_FILTER_ANISOTROPIC;
	_MipMapFilters[0][FILTER_TYPE_BEST] =
		rts::render::RENDER_TEXTURE_FILTER_LINEAR;

	switch (texture_filter) {
	default:
		DEBUG_CRASH(("Invalid filter type passed into TextureFilterClass::_Init_Filters()"));
		FALLTHROUGH;
	case TEXTURE_FILTER_NONE:
		_MinTextureFilters[0][FILTER_TYPE_FAST] =
			_MagTextureFilters[0][FILTER_TYPE_FAST] =
				_MipMapFilters[0][FILTER_TYPE_BEST] =
					_MipMapFilters[0][FILTER_TYPE_FAST] =
						rts::render::RENDER_TEXTURE_FILTER_NONE;
		_MinTextureFilters[0][FILTER_TYPE_BEST] =
			_MagTextureFilters[0][FILTER_TYPE_BEST] =
				rts::render::RENDER_TEXTURE_FILTER_POINT;
			break;
	case TEXTURE_FILTER_POINT:
		_MinTextureFilters[0][FILTER_TYPE_FAST] =
			_MagTextureFilters[0][FILTER_TYPE_FAST] =
			_MipMapFilters[0][FILTER_TYPE_FAST] =
			_MinTextureFilters[0][FILTER_TYPE_BEST] =
			_MagTextureFilters[0][FILTER_TYPE_BEST] =
			_MipMapFilters[0][FILTER_TYPE_BEST] =
				rts::render::RENDER_TEXTURE_FILTER_POINT;
			break;
	case TEXTURE_FILTER_BILINEAR:
		_MinTextureFilters[0][FILTER_TYPE_BEST] =
			_MagTextureFilters[0][FILTER_TYPE_BEST] = supportsLinear ?
				rts::render::RENDER_TEXTURE_FILTER_LINEAR :
				rts::render::RENDER_TEXTURE_FILTER_POINT;
		_MipMapFilters[0][FILTER_TYPE_BEST] =
			rts::render::RENDER_TEXTURE_FILTER_POINT;
			break;
	case TEXTURE_FILTER_TRILINEAR:
		_MinTextureFilters[0][FILTER_TYPE_BEST] =
			_MagTextureFilters[0][FILTER_TYPE_BEST] = supportsLinear ?
				rts::render::RENDER_TEXTURE_FILTER_LINEAR :
				rts::render::RENDER_TEXTURE_FILTER_POINT;
		_MipMapFilters[0][FILTER_TYPE_BEST] = supportsLinear ?
			rts::render::RENDER_TEXTURE_FILTER_LINEAR :
			rts::render::RENDER_TEXTURE_FILTER_POINT;
			break;
	case TEXTURE_FILTER_ANISOTROPIC:
		if (!supportsAnisotropic) {
			_MinTextureFilters[0][FILTER_TYPE_BEST] =
				_MagTextureFilters[0][FILTER_TYPE_BEST] =
					rts::render::RENDER_TEXTURE_FILTER_POINT;
		} else {
			_Set_Max_Anisotropy(anisotropy_level);
		}
		_MipMapFilters[0][FILTER_TYPE_BEST] = supportsLinear ?
			rts::render::RENDER_TEXTURE_FILTER_LINEAR :
			rts::render::RENDER_TEXTURE_FILTER_POINT;
		break;
	}


	// For stages above zero, set best filter to the same as the stage zero
	int i=1;
	for (;i<rts::render::LEGACY_TEXTURE_STAGE_COUNT;++i) {
		_MinTextureFilters[i][FILTER_TYPE_NONE]=_MinTextureFilters[0][FILTER_TYPE_NONE];
		_MagTextureFilters[i][FILTER_TYPE_NONE]=_MagTextureFilters[0][FILTER_TYPE_NONE];
		_MipMapFilters[i][FILTER_TYPE_NONE]=_MipMapFilters[0][FILTER_TYPE_NONE];

		_MinTextureFilters[i][FILTER_TYPE_FAST]=_MinTextureFilters[0][FILTER_TYPE_FAST];
		_MagTextureFilters[i][FILTER_TYPE_FAST]=_MagTextureFilters[0][FILTER_TYPE_FAST];
		_MipMapFilters[i][FILTER_TYPE_FAST]=_MipMapFilters[0][FILTER_TYPE_FAST];

		// When Anisotropic filtering is used, all stages above zero use trilinear filtering
		if (_MagTextureFilters[0][FILTER_TYPE_BEST] ==
			rts::render::RENDER_TEXTURE_FILTER_ANISOTROPIC) {
			_MagTextureFilters[i][FILTER_TYPE_BEST] =
				supportsLinear ? rts::render::RENDER_TEXTURE_FILTER_LINEAR :
				rts::render::RENDER_TEXTURE_FILTER_POINT;
		}
		else {
			_MagTextureFilters[i][FILTER_TYPE_BEST]=_MagTextureFilters[0][FILTER_TYPE_BEST];
		}

		if (_MinTextureFilters[0][FILTER_TYPE_BEST] ==
			rts::render::RENDER_TEXTURE_FILTER_ANISOTROPIC) {
			_MinTextureFilters[i][FILTER_TYPE_BEST] =
				supportsLinear ? rts::render::RENDER_TEXTURE_FILTER_LINEAR :
				rts::render::RENDER_TEXTURE_FILTER_POINT;
		}
		else {
			_MinTextureFilters[i][FILTER_TYPE_BEST]=_MinTextureFilters[0][FILTER_TYPE_BEST];
		}
		_MipMapFilters[i][FILTER_TYPE_BEST]=_MipMapFilters[0][FILTER_TYPE_BEST];

	}

	// Set default to best. The level of best filter mode is controlled by the input parameter.
	for (i=0;i<rts::render::LEGACY_TEXTURE_STAGE_COUNT;++i) {
		_MinTextureFilters[i][FILTER_TYPE_DEFAULT]=_MinTextureFilters[i][FILTER_TYPE_BEST];
		_MagTextureFilters[i][FILTER_TYPE_DEFAULT]=_MagTextureFilters[i][FILTER_TYPE_BEST];
		_MipMapFilters[i][FILTER_TYPE_DEFAULT]=_MipMapFilters[i][FILTER_TYPE_BEST];
	}

}


//**********************************************************************************************
//! Set mip mapping filter (legacy)
/*!
*/
void TextureFilterClass::Set_Mip_Mapping(FilterType mipmap)
{
//	if (mipmap != FILTER_TYPE_NONE && Get_Mip_Level_Count() <= 1 && Is_Initialized())
//	{
//		WWASSERT_PRINT(0, "Trying to enable MipMapping on texture w/o Mip levels!");
//		return;
//	}
	MipMapFilter=mipmap;
}

//**********************************************************************************************
//! Set anisotropic filter level
/*!
*/
void TextureFilterClass::_Set_Max_Anisotropy(AnisotropicFilterMode mode)
{
	for (int stage = 0; stage < rts::render::LEGACY_TEXTURE_STAGE_COUNT; ++stage)
		rts::render::SetGameTextureStageState(stage,
			rts::render::GAME_TEXTURE_STAGE_MAX_ANISOTROPY, mode);
}

//**********************************************************************************************
//! Set default min filter (legacy)
/*!
*/
void TextureFilterClass::_Set_Default_Min_Filter(FilterType filter)
{
	for (int i=0;i<rts::render::LEGACY_TEXTURE_STAGE_COUNT;++i)
	{
		_MinTextureFilters[i][FILTER_TYPE_DEFAULT]=_MinTextureFilters[i][filter];
	}
}


//**********************************************************************************************
//! Set default mag filter (legacy)
/*!
*/
void TextureFilterClass::_Set_Default_Mag_Filter(FilterType filter)
{
	for (int i=0;i<rts::render::LEGACY_TEXTURE_STAGE_COUNT;++i)
	{
		_MagTextureFilters[i][FILTER_TYPE_DEFAULT]=_MagTextureFilters[i][filter];
	}
}

//**********************************************************************************************
//! Set default mip filter (legacy)
/*!
*/
void TextureFilterClass::_Set_Default_Mip_Filter(FilterType filter)
{
	for (int i=0;i<rts::render::LEGACY_TEXTURE_STAGE_COUNT;++i)
	{
		_MipMapFilters[i][FILTER_TYPE_DEFAULT]=_MipMapFilters[i][filter];
	}
}
