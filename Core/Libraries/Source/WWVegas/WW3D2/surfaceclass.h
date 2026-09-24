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
 *                     $Archive:: /VSS_Sync/ww3d2/surfaceclass.h                              $*
 *                                                                                             *
 *              Original Author:: Nathaniel Hoffman                                            *
 *                                                                                             *
 *                      $Author:: Vss_sync                                                    $*
 *                                                                                             *
 *                     $Modtime:: 8/29/01 9:32p                                               $*
 *                                                                                             *
 *                    $Revision:: 17                                                          $*
 *                                                                                             *
 *---------------------------------------------------------------------------------------------*
 * Functions:                                                                                  *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#pragma once

#include "WWLib/always.h"
#include "ww3dformat.h"

class Vector2i;
class Vector3;
class TextureBaseClass;
#if defined(_WIN64)
struct NativeSurfaceStorage;
namespace rts { namespace render {
	struct NativeW3DSurfaceHandle;
	struct NativeW3DGpuContentLease;
} }
#endif

/*************************************************************************
**                             SurfaceClass
**
** This is our surface class, which owns a CPU image and an optional native
** surface view.
**
** Hector Yee 2/12/01 - added in fills, blits etc for font3d class
**
*************************************************************************/
class SurfaceClass : public RefCountClass
{
	W3DMPO_CODE(SurfaceClass)
	public:
		typedef void *LockedSurfacePtr;

		struct SurfaceDescription {
			WW3DFormat		Format;	// Surface format
			unsigned int	Width;	// Surface width in pixels
			unsigned int	Height;	// Surface height in pixels
		};

		// Create surface with desired height, width and format.
		SurfaceClass(unsigned width, unsigned height, WW3DFormat format);

		// Create surface from a file.
		SurfaceClass(const char *filename);

		// Create the surface from an opaque renderer handle.  The native product
		// never interprets this value; the external legacy adapter owns typed
		// renderer interop.
		SurfaceClass(void *surface_handle);

		virtual ~SurfaceClass() override;

		// Get surface description
		void Get_Description(SurfaceDescription &surface_desc);

		// Get the bytes per pixel count
		unsigned int Get_Bytes_Per_Pixel();

		// Lock / unlock the surface
		LockedSurfacePtr Lock(int *pitch);
		LockedSurfacePtr Lock(int *pitch, const Vector2i &min, const Vector2i &max);
		void Unlock();
		// Complete a read-only lock without republishing an unchanged native
		// subresource. The external legacy adapter performs its required unlock.
		void Unlock_Read_Only();
#if defined(_WIN64)
		// Native callers need the publication result so a device/ownership or
		// upload failure can retain its dirty CPU image for a later retry. Keep
		// Unlock() above unchanged for the legacy ABI and callers.
		bool Unlock_Native_Surface();
#endif

		// HY -- The following functions are support functions for font3d
		// zaps the surface memory to zero
		void Clear();

		// copies the contents of one surface to another
		void Copy(
			unsigned int dstx, unsigned int dsty,
			unsigned int srcx, unsigned int srcy,
			unsigned int width, unsigned int height,
			const SurfaceClass *other);
#if defined(_WIN64)
		// Status-returning native counterpart used by migrated callers. The
		// legacy void Copy overload remains source/ABI compatible.
		bool Copy_Native(
			unsigned int dstx, unsigned int dsty,
			unsigned int srcx, unsigned int srcy,
			unsigned int width, unsigned int height,
			const SurfaceClass *other);
		// Mutate the native CPU shadow without publishing it.  Batch callers
		// must finish with Publish_Native_Changes so a multi-region update only
		// refreshes the owning texture once.  A failed publication leaves the
		// retained shadow available for a later retry.
		bool Copy_Native_No_Publish(
			unsigned int dstx, unsigned int dsty,
			unsigned int srcx, unsigned int srcy,
			unsigned int width, unsigned int height,
			const SurfaceClass *other);
		bool Publish_Native_Changes();
#endif

		// support for copying from a byte array
		void Copy(const unsigned char *other);

		// support for copying from a byte array
		void Copy(const Vector2i &min, const Vector2i &max, const unsigned char *other);

		// copies the contents of one surface to another, stretches
		void Stretch_Copy(
			unsigned int dstx, unsigned int dsty, unsigned int dstwidth, unsigned int dstheight,
			unsigned int srcx, unsigned int srcy, unsigned int srcwidth, unsigned int srcheight,
			const SurfaceClass *source);

		// finds the bounding box of non-zero pixels, used in font3d
		void FindBB(Vector2i *min,Vector2i*max);

		// tests a column to see if the alpha is nonzero, used in font3d
		bool Is_Transparent_Column(unsigned int column);

		// makes a copy of the surface into a byte array
		unsigned char *CreateCopy(int *width,int *height,int*size,bool flip=false);

		// For use by renderer adapters.  The native product exposes no typed
		// graphics object through this compatibility accessor.
		void *Peek_Surface_Handle() { return SurfaceHandle; }

		// Attaching and detaching a surface pointer
		void	Attach (void *surface_handle);
		void	Detach ();

		// draws a horizontal line
		void Draw_H_Line(const unsigned int y, const unsigned int x1, const unsigned int x2,
			unsigned int color, unsigned int bytesPerPixel, LockedSurfacePtr pBits, int pitch);

		// draws a pixel
		void Draw_Pixel(const unsigned int x, const unsigned int y, unsigned int color,
			unsigned int bytesPerPixel, LockedSurfacePtr pBits, int pitch);

		// get pixel function
		void Get_Pixel(Vector3 &rgb, int x, int y, LockedSurfacePtr pBits, int pitch);

		void Hue_Shift(const Vector3 &hsv_shift);

		bool Is_Monochrome();

		WW3DFormat Get_Surface_Format() const { return SurfaceFormat; }

#if defined(_WIN64)
		bool Acquire_Native_Surface(bool for_output,
			rts::render::NativeW3DSurfaceHandle *surface,
			rts::render::NativeW3DGpuContentLease *gpu_lease = 0) const;
#endif

	private:
#if defined(_WIN64)
		SurfaceClass(TextureBaseClass *texture, unsigned int mip_level,
			unsigned int array_slice);
#endif

		// Opaque compatibility handle. Native paths leave it null.
		void *SurfaceHandle;
#if defined(_WIN64)
		NativeSurfaceStorage *NativeSurface;
#endif

		WW3DFormat SurfaceFormat;
	friend class TextureClass;
};
