/*
**	Command & Conquer Generals(tm)
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
 *                 Project Name : WWPhys                                                       *
 *                                                                                             *
 *                     $Archive:: /Commando/Code/ww3d2/matrixmapper.cpp                       $*
 *                                                                                             *
 *              Original Author:: Greg Hjelstrom                                               *
 *                                                                                             *
 *                      $Author:: Greg_h                                                      $*
 *                                                                                             *
 *                     $Modtime:: 6/21/01 10:32a                                              $*
 *                                                                                             *
 *                    $Revision:: 10                                                          $*
 *                                                                                             *
 *---------------------------------------------------------------------------------------------*
 * Functions:                                                                                  *
 *   MatrixMapperClass::MatrixMapperClass -- Constructor                                       *
 *   MatrixMapperClass::Set_Texture_Transform -- Sets the viewspace-to-texturespace transform  *
 *   MatrixMapperClass::Update_View_To_Pixel_Transform -- recomputes ViewToPixel               *
 *   MatrixMapperClass::Compute_Texture_Coordinate -- compute a single texture coord           *
 *   MatrixMapperClass::isActive -- check if this mapper should be active                      *
 *   MatrixMapperClass::process -- process the given VertexPipe                                *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */


#include "matrixmapper.h"
#include "Renderer/RenderGameClient.h"

namespace
{
	const uint32 CANONICAL_MAPPER_MATRIX = 0x100u;

	static rts::render::GameRenderTransformSlot MatrixMapperTextureTransformSlot(
		unsigned int stage)
	{
		return static_cast<rts::render::GameRenderTransformSlot>(
			rts::render::GAME_TRANSFORM_TEXTURE0 + stage);
	}

	static void SetMatrixMapperTextureTransform(unsigned int stage,
		const Matrix3D &matrix)
	{
		rts::render::SetGameTransform(MatrixMapperTextureTransformSlot(stage), matrix);
	}

	static void SetMatrixMapperTextureTransform(unsigned int stage,
		const Matrix4x4 &matrix)
	{
		rts::render::SetGameTransform(MatrixMapperTextureTransformSlot(stage), matrix);
	}

	static void SetMatrixMapperTextureCoordinateCameraPosition(unsigned int stage)
	{
		rts::render::SetGameTextureStageState(stage,
			rts::render::GAME_TEXTURE_STAGE_COORDINATE_INDEX,
		rts::render::GAME_TEXTURE_COORDINATE_CAMERA_POSITION);
	}

	static void SetMatrixMapperTextureCoordinateCameraNormal(unsigned int stage)
	{
		rts::render::SetGameTextureStageState(stage,
			rts::render::GAME_TEXTURE_STAGE_COORDINATE_INDEX,
		rts::render::GAME_TEXTURE_COORDINATE_CAMERA_NORMAL);
	}

	static void SetMatrixMapperTextureTransformCount2(unsigned int stage)
	{
		rts::render::SetGameTextureStageState(stage,
			rts::render::GAME_TEXTURE_STAGE_TRANSFORM_FLAGS,
		rts::render::GAME_TEXTURE_TRANSFORM_COUNT2);
	}

	static void SetMatrixMapperTextureTransformProjectedCount3(unsigned int stage)
	{
		rts::render::SetGameTextureStageState(stage,
			rts::render::GAME_TEXTURE_STAGE_TRANSFORM_FLAGS,
			rts::render::GAME_TEXTURE_TRANSFORM_PROJECTED |
			rts::render::GAME_TEXTURE_TRANSFORM_COUNT3);
	}
}

/***********************************************************************************************
 * MatrixMapperClass::MatrixMapperClass -- Constructor                                         *
 *                                                                                             *
 *    Initializes the member variables to defaults                                             *
 *                                                                                             *
 * INPUT:                                                                                      *
 *                                                                                             *
 * OUTPUT:                                                                                     *
 *                                                                                             *
 * WARNINGS:                                                                                   *
 *                                                                                             *
 * HISTORY:                                                                                    *
 *   11/13/99   gth : created                                                                  *
 *=============================================================================================*/
MatrixMapperClass::MatrixMapperClass(int stage) :
	TextureMapperClass(stage),
	Flags(0),
	Type(ORTHO_PROJECTION),
	ViewToTexture(true),
	ViewToPixel(true),
	GradientUCoord(0.5f)
{
	ViewSpaceProjectionNormal = Vector3(0.0f,0.0f,0.0f);
}

unsigned long MatrixMapperClass::Compute_Canonical_Matrix_CRC(unsigned long crc, uint32 type_id) const
{
	crc = Append_Canonical_Header(crc, type_id);
	crc = Append_Canonical_UInt(crc, Flags);
	crc = Append_Canonical_UInt(crc, static_cast<uint32>(Type));
	crc = Append_Canonical_Matrix4(crc, ViewToTexture);
	return Append_Canonical_Float(crc, GradientUCoord);
}

unsigned long MatrixMapperClass::Compute_Canonical_CRC(unsigned long crc) const
{
	return Compute_Canonical_Matrix_CRC(crc, CANONICAL_MAPPER_MATRIX);
}

/***********************************************************************************************
 * MatrixMapperClass::Set_Texture_Transform -- Sets the viewspace-to-texturespace transform    *
 *                                                                                             *
 * INPUT:                                                                                      *
 *                                                                                             *
 * OUTPUT:                                                                                     *
 *                                                                                             *
 * WARNINGS:                                                                                   *
 *                                                                                             *
 * HISTORY:                                                                                    *
 *   6/20/2001  gth : created                                                                  *
 *=============================================================================================*/
void MatrixMapperClass::Set_Texture_Transform(const Matrix3D & view_to_texture,float texsize)
{
	ViewToTexture = Matrix4x4(view_to_texture);
	Update_View_To_Pixel_Transform(texsize);
}

/***********************************************************************************************
 * MatrixMapperClass::Set_Texture_Transform -- Sets the viewspace-to-texturespace transform    *
 *                                                                                             *
 * INPUT:                                                                                      *
 *                                                                                             *
 * OUTPUT:                                                                                     *
 *                                                                                             *
 * WARNINGS:                                                                                   *
 *                                                                                             *
 * HISTORY:                                                                                    *
 *   11/13/99   gth : Created.                                                                 *
 *=============================================================================================*/
void	MatrixMapperClass::Set_Texture_Transform(const Matrix4x4 & view_to_texture,float texsize)
{
	ViewToTexture=view_to_texture;

	Update_View_To_Pixel_Transform(texsize);
}

/***********************************************************************************************
 * MatrixMapperClass::Update_View_To_Pixel_Transform -- recomputes ViewToPixel                 *
 *                                                                                             *
 * INPUT:                                                                                      *
 *                                                                                             *
 * OUTPUT:                                                                                     *
 *                                                                                             *
 * WARNINGS:                                                                                   *
 *                                                                                             *
 * HISTORY:                                                                                    *
 *   1/4/00     gth : Created.                                                                 *
 *=============================================================================================*/
void MatrixMapperClass::Update_View_To_Pixel_Transform(float tex_size)
{
	/*
	** Create a ViewToPixel matrix which also does all of the offsetting and flipping that has
	** to take place to get the actual texture coordinates.
	**
	** Here is a description of the math:
	** If you transform eye-space points with the ViewToTexture matrix, you then need to compute:
	** s = ((x/w) + 1.0f) * 0.5f * ((texwid-2)/texwid)
	** t = (1.0f - (y/w)) * 0.5f * ((texwid-2)/texwid)
	**
	** Let K = 0.5 * ((texwid-2)/texwid), Then:
	** ((x/w) + 1.0f) * K = (K*x + K*w) / w
	** (1.0f - (y/w)) * K = (-K*y + K*w) / w
	**
	** This leads to the following matrix (The KOOK matrix!)
	** | K  0 0 K |
	** | 0 -K 0 K |
	** | 0  0 1 0 |
	** | 0  0 0 1 |
	**
	** The code below manually "optimally" pre-multiplies this matrix with the ViewToTexture matrix
	**
	** In addition, the z-coordinate is modified so that it goes from 0 to 1 rather than -1 to +1.
	** It can also be flipped if the INVERT_DEPTH_GRADIENT flag is on.
	*/
	float k = 0.5 * (tex_size - 2.0f) / tex_size;

	ViewToPixel[0][0] = k * ViewToTexture[0][0] + k * ViewToTexture[3][0];
	ViewToPixel[0][1] = k * ViewToTexture[0][1] + k * ViewToTexture[3][1];
	ViewToPixel[0][2] = k * ViewToTexture[0][2] + k * ViewToTexture[3][2];
	ViewToPixel[0][3] = k * ViewToTexture[0][3] + k * ViewToTexture[3][3];

	ViewToPixel[1][0] = -k * ViewToTexture[1][0] + k * ViewToTexture[3][0];
	ViewToPixel[1][1] = -k * ViewToTexture[1][1] + k * ViewToTexture[3][1];
	ViewToPixel[1][2] = -k * ViewToTexture[1][2] + k * ViewToTexture[3][2];
	ViewToPixel[1][3] = -k * ViewToTexture[1][3] + k * ViewToTexture[3][3];

	if (Get_Flag(INVERT_DEPTH_GRADIENT)) {
		ViewToPixel[2][0] = -0.5f * ViewToTexture[2][0] + 0.5f * ViewToTexture[3][0];
		ViewToPixel[2][1] = -0.5f * ViewToTexture[2][1] + 0.5f * ViewToTexture[3][1];
		ViewToPixel[2][2] = -0.5f * ViewToTexture[2][2] + 0.5f * ViewToTexture[3][2];
		ViewToPixel[2][3] = -0.5f * ViewToTexture[2][3] + 0.5f * ViewToTexture[3][3];
	} else {
		ViewToPixel[2][0] = 0.5f * ViewToTexture[2][0] + 0.5f * ViewToTexture[3][0];
		ViewToPixel[2][1] = 0.5f * ViewToTexture[2][1] + 0.5f * ViewToTexture[3][1];
		ViewToPixel[2][2] = 0.5f * ViewToTexture[2][2] + 0.5f * ViewToTexture[3][2];
		ViewToPixel[2][3] = 0.5f * ViewToTexture[2][3] + 0.5f * ViewToTexture[3][3];
	}

	ViewToPixel[3] = ViewToTexture[3];

	/*
	** Store the view space negative z vector for lighting effects
	*/
	ViewSpaceProjectionNormal.X = -ViewToTexture[2][0];
	ViewSpaceProjectionNormal.Y = -ViewToTexture[2][1];
	ViewSpaceProjectionNormal.Z = -ViewToTexture[2][2];
	ViewSpaceProjectionNormal.Normalize();

}

/***********************************************************************************************
 * MatrixMapperClass::Compute_Texture_Coordinate -- compute a single texture coord             *
 *                                                                                             *
 * INPUT:                                                                                      *
 * point - 3D point to calculate the texture coordinate for                                    *
 * set_stq - pointer to texture coordinate, s,t will be stored in X,Y.  q will be stored in Z  *
 *                                                                                             *
 * OUTPUT:                                                                                     *
 *                                                                                             *
 * WARNINGS:                                                                                   *
 *                                                                                             *
 * HISTORY:                                                                                    *
 *   1/25/00    gth : Created.                                                                 *
 *=============================================================================================*/
void MatrixMapperClass::Compute_Texture_Coordinate(const Vector3 & point,Vector3 * set_stq)
{
	set_stq->X = ViewToPixel[0][0]*point.X + ViewToPixel[0][1]*point.Y + ViewToPixel[0][2]*point.Z + ViewToPixel[0][3];
	set_stq->Y = ViewToPixel[1][0]*point.X + ViewToPixel[1][1]*point.Y + ViewToPixel[1][2]*point.Z + ViewToPixel[1][3];
	set_stq->Z = ViewToPixel[3][0]*point.X + ViewToPixel[3][1]*point.Y + ViewToPixel[3][2]*point.Z + ViewToPixel[3][3];
}

void MatrixMapperClass::Apply(int uv_array_index)
{
	Matrix4x4 m;

	switch (Type)
	{
	case ORTHO_PROJECTION:
		/*
		** Orthographic projection
		*/
		SetMatrixMapperTextureTransform(Stage,ViewToPixel);
		SetMatrixMapperTextureCoordinateCameraPosition(uv_array_index);
		SetMatrixMapperTextureTransformCount2(uv_array_index);
		break;
	case PERSPECTIVE_PROJECTION:
		/*
		** Perspective projection
		*/
		m[0]=ViewToPixel[0];
		m[1]=ViewToPixel[1];
		m[2]=ViewToPixel[3];
		SetMatrixMapperTextureTransform(Stage,m);
		SetMatrixMapperTextureCoordinateCameraPosition(uv_array_index);
		SetMatrixMapperTextureTransformProjectedCount3(uv_array_index);
		break;
	case DEPTH_GRADIENT:
		/*
		** Depth gradient, Set up second stage texture coordinates to
		** apply a depth gradient to the projection.  Note that the
		** depth values have been set up to vary from 0 to 1 in the
		** Update_View_To_Pixel_Transform function.
		*/
		m[0].Set(0,0,0,GradientUCoord);
		m[1]=ViewToPixel[2];
		SetMatrixMapperTextureTransform(Stage,m);
		SetMatrixMapperTextureCoordinateCameraPosition(uv_array_index);
		SetMatrixMapperTextureTransformCount2(uv_array_index);
		break;
	case NORMAL_GRADIENT:
		/*
		** Normal Gradient, Set up the second stage texture coordinates to
		** apply a gradient based on the dot product of the vertex normal
		** and the projection direction.  (NOTE: this is basically texture-
		** based diffuse lighting!)
		*/
		m[0].Set(0,0,0,GradientUCoord);
		m[1].Set(ViewSpaceProjectionNormal.X,ViewSpaceProjectionNormal.Y,ViewSpaceProjectionNormal.Z, 0);
		SetMatrixMapperTextureTransform(Stage,m);
		SetMatrixMapperTextureCoordinateCameraNormal(uv_array_index);
		SetMatrixMapperTextureTransformCount2(uv_array_index);
		break;
	}


}
