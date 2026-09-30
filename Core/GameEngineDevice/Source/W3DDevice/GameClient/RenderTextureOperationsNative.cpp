/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2026 TheSuperHackers
*/

#include "W3DDevice/GameClient/RenderTextureOperations.h"
#include <WW3D2/texture.h>

bool Generate_Render_Texture_Mip_Levels(TextureClass *texture)
{
	return texture != nullptr && texture->Generate_Native_Mip_Levels();
}

void Set_Render_Texture_LOD(TextureClass *texture, int lod)
{
	if (texture != nullptr) texture->Set_Native_Texture_LOD(lod);
}

void Set_Render_Texture_Default_LOD(TextureClass *texture, int lod)
{
	if (texture != nullptr) texture->Set_Default_Native_Texture_LOD(lod);
}

void Apply_Render_Texture_LOD_Policy(TextureClass *texture,
	int requested_lod, bool has_explicit_selection, int default_lod)
{
	if (texture == nullptr) return;
	if (has_explicit_selection)
		Set_Render_Texture_LOD(texture, requested_lod);
	else
		Set_Render_Texture_Default_LOD(texture, default_lod);
}

void Bind_Render_Texture_Alias(TextureClass *destination, TextureClass *source)
{
	(void)destination;
	(void)source;
}
