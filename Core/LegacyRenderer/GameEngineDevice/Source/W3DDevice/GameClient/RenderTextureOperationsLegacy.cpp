/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2026 TheSuperHackers
*/

#include "W3DDevice/GameClient/RenderTextureOperations.h"
#include <WW3D2/texture.h>
#include <WW3D2/texturemipgenerator.h>
#include <d3d8.h>

bool Generate_Render_Texture_Mip_Levels(TextureClass *texture)
{
	if (texture == nullptr || texture->Peek_D3D_Texture() == nullptr)
		return false;
	return Generate_DX8_Texture_Mip_Levels(texture->Peek_D3D_Texture()) == 0;
}

void Set_Render_Texture_LOD(TextureClass *texture, int lod)
{
	if (texture != nullptr && texture->Peek_D3D_Texture() != nullptr)
		texture->Peek_D3D_Texture()->SetLOD(lod);
}

void Bind_Render_Texture_Alias(TextureClass *destination, TextureClass *source)
{
	if (destination != nullptr && source != nullptr)
		destination->Set_D3D_Base_Texture(source->Peek_D3D_Base_Texture());
}
