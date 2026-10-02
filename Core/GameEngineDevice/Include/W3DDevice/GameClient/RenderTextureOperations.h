/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2026 TheSuperHackers
**
** Renderer-neutral operations for procedural game-client textures.
*/

#pragma once

class TextureClass;

// Complete the mip chain after a game-client texture has been written through
// SurfaceClass.  The product-facing caller is deliberately independent of the
// backend; the selected renderer owns the platform implementation.
bool Generate_Render_Texture_Mip_Levels(TextureClass *texture);

// Apply the renderer's texture LOD policy without exposing a backend handle.
void Set_Render_Texture_LOD(TextureClass *texture, int lod);

#if defined(_WIN64)
// Apply the current texture-reduction default unless a caller explicitly
// selected an LOD for this procedural texture.
void Set_Render_Texture_Default_LOD(TextureClass *texture, int lod);

// Restore owner-held selection on a newly created procedural atlas. When no
// explicit selection exists, use the latest global texture-reduction default.
void Apply_Render_Texture_LOD_Policy(TextureClass *texture,
	int requested_lod, bool has_explicit_selection, int default_lod);
#endif

// Bind an alias texture to another texture on the legacy compatibility path.
// Native products retain the source TextureClass and apply it directly.
void Bind_Render_Texture_Alias(TextureClass *destination, TextureClass *source);
