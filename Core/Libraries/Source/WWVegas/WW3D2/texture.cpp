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
 *                     $Archive:: /Commando/Code/ww3d2/texture.cpp                            $*
 *                                                                                             *
 *                  $Org Author:: Steve_t                                                     $*
 *                                                                                             *
 *                       Author : Kenny Mitchell                                               *
 *                                                                                             *
 *                     $Modtime:: 08/05/02 1:27p                                              $*
 *                                                                                             *
 *                    $Revision:: 85                                                          $*
 *                                                                                             *
 * 06/27/02 KM Texture class abstraction																			*
 * 08/05/02 KM Texture class redesign (revisited)
 *---------------------------------------------------------------------------------------------*
 * Functions:                                                                                  *
 *   FileListTextureClass::Load_Frame_Surface -- Load source texture                           *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#include "texture.h"

#include <d3d8.h>
#include "dx8wrapper.h"
#include "WWLib/TARGA.h"
#include <WWLib/nstrdup.h>
#include "w3d_file.h"
#include "assetmgr.h"
#include "formconv.h"
#include "textureloader.h"
#include "missingtexture.h"
#include "WWLib/ffactory.h"
#include "dx8caps.h"
#include "dx8texman.h"
#include "meshmatdesc.h"
#include "texturethumbnail.h"
#include "WWDebug/wwprofile.h"
#if defined(_WIN64)
#include "nativew3dsampledtexture.h"
#include "nativew3dtextureowner.h"
#include "texturemipbuffer.h"
#include "texturemipgenerator.h"
#include <new>
#include <vector>
#endif

const unsigned DEFAULT_INACTIVATION_TIME=20000;

/*
** Definitions of static members:
*/

static unsigned unused_texture_id;

#if defined(_WIN64)
struct NativeTextureStorage
{
	NativeTextureStorage() : owner(), descriptor(), pixels(), rowPitches(),
		slicePitches(), gpuLease(), sourceFormat(WW3D_FORMAT_UNKNOWN),
		missing(false) {}

	rts::render::NativeW3DTextureOwner owner;
	rts::render::TextureDescriptor descriptor;
	std::vector<std::vector<unsigned char> > pixels;
	std::vector<size_t> rowPitches;
	std::vector<size_t> slicePitches;
	mutable rts::render::NativeW3DGpuContentLease gpuLease;
	WW3DFormat sourceFormat;
	bool missing;
};

static bool Apply_Native_Empty_Texture(TextureBaseClass *texture,
	unsigned int width, unsigned int height, WW3DFormat format,
	MipCountType requested_mips, unsigned int array_count, bool render_target)
{
	if (texture == nullptr || width == 0 || height == 0 || array_count == 0 ||
		!rts::render::NativeW3DSampledTextureUpload::SupportsSourceFormat(format))
		return false;
	unsigned int mip_count = requested_mips == MIP_LEVELS_ALL ?
		CalculateTextureMipLevelCount(width, height) :
		static_cast<unsigned int>(requested_mips);
	if (mip_count == 0 || mip_count > MIP_LEVELS_MAX || array_count > 6)
		return false;

	TextureMipBuffer buffers[6][MIP_LEVELS_MAX];
	rts::render::NativeW3DSampledTextureMipView
		views[6 * MIP_LEVELS_MAX];
	for (unsigned int slice = 0; slice < array_count; ++slice)
	{
		unsigned int mip_width = width;
		unsigned int mip_height = height;
		for (unsigned int mip = 0; mip < mip_count; ++mip)
		{
			const unsigned int index = slice * mip_count + mip;
			if (!buffers[slice][mip].allocate(format, mip_width, mip_height, 1))
				return false;
			memset(buffers[slice][mip].data(), 0,
				buffers[slice][mip].layout().dataSize);
			views[index].data = buffers[slice][mip].data();
			views[index].dataSize = buffers[slice][mip].layout().dataSize;
			views[index].rowPitch = buffers[slice][mip].layout().rowPitch;
			ReduceTextureMipDimensions(mip_width, mip_height);
		}
	}

	rts::render::NativeW3DSampledTextureUpload upload;
	if (!upload.Prepare(format, width, height, mip_count, array_count, views,
		array_count * mip_count)) return false;
	rts::render::TextureDescriptor descriptor = upload.Descriptor();
	if (render_target)
	{
		descriptor.binding |= rts::render::RENDER_TEXTURE_RENDER_TARGET;
		descriptor.usage = rts::render::RENDER_USAGE_DEFAULT;
	}
	return texture->Apply_Native_Texture(descriptor, upload.Subresources(),
		upload.SubresourceCount(), format, true);
}
#endif

// This throttles submissions to the background texture loading queue.
static unsigned TexturesAppliedPerFrame;
const unsigned MAX_TEXTURES_APPLIED_PER_FRAME=2;


/*!
 * KM General base constructor for texture classes
 */
TextureBaseClass::TextureBaseClass
(
	unsigned int width,
	unsigned int height,
	enum MipCountType mip_level_count,
	enum PoolType pool,
	bool rendertarget,
	bool reducible
)
:	MipLevelCount(mip_level_count),
	D3DTexture(nullptr),
#if defined(_WIN64)
	NativeTexture(nullptr),
#endif
	Initialized(false),
   Name(""),
	FullPath(""),
	texture_id(unused_texture_id++),
	IsLightmap(false),
	IsProcedural(false),
	IsReducible(reducible),
	IsCompressionAllowed(false),
	InactivationTime(0),
	ExtendedInactivationTime(0),
	LastInactivationSyncTime(0),
	LastAccessed(0),
	Width(width),
	Height(height),
	Pool(pool),
	Dirty(false),
	TextureLoadTask(nullptr),
	ThumbnailLoadTask(nullptr),
	HSVShift(0.0f,0.0f,0.0f)
{
}


//**********************************************************************************************
//! Base texture class destructor
/*! KJM
*/
TextureBaseClass::~TextureBaseClass()
{
	delete TextureLoadTask;
	TextureLoadTask=nullptr;
	delete ThumbnailLoadTask;
	ThumbnailLoadTask=nullptr;

	Release_D3D_Texture();
#if defined(_WIN64)
	Release_Native_Texture();
#endif

	DX8TextureManagerClass::Remove(this);
}

#if defined(_WIN64)
void TextureBaseClass::Release_Native_Texture()
{
	if (NativeTexture != nullptr)
	{
		NativeTexture->owner.Reset();
		delete NativeTexture;
		NativeTexture = nullptr;
	}
}

bool TextureBaseClass::Apply_Native_Texture(
	const rts::render::TextureDescriptor &descriptor,
	const rts::render::TextureSubresourceData *subresources,
	unsigned int subresource_count, WW3DFormat source_format,
	bool initialized, bool disable_auto_invalidation, bool missing_texture)
{
	const unsigned int expected_count = descriptor.mipCount * descriptor.arrayCount;
	if (descriptor.width == 0 || descriptor.height == 0 ||
		descriptor.mipCount == 0 || descriptor.arrayCount == 0 ||
		subresources == nullptr || subresource_count != expected_count)
	{
		return false;
	}
	// TextureClass retains the complete canonical CPU image specifically so a
	// recovered native device can republish it. D3D11 immutable resources cannot
	// be refreshed in place, so promote prepared immutable uploads to DEFAULT at
	// this product boundary while preserving every other descriptor field.
	rts::render::TextureDescriptor product_descriptor = descriptor;
	if (product_descriptor.usage == rts::render::RENDER_USAGE_IMMUTABLE)
		product_descriptor.usage = rts::render::RENDER_USAGE_DEFAULT;

	NativeTextureStorage *storage = NativeTexture;
	if (storage == nullptr)
	{
		storage = new(std::nothrow) NativeTextureStorage;
		if (storage == nullptr) return false;
	}

	rts::render::NativeW3DTextureCandidate candidate;
	if (storage->owner.CreateCandidate(product_descriptor, subresources,
		subresource_count, &candidate) != rts::render::RENDER_RESULT_OK)
	{
		if (NativeTexture == nullptr) delete storage;
		return false;
	}

	std::vector<std::vector<unsigned char> > pixels;
	std::vector<size_t> row_pitches;
	std::vector<size_t> slice_pitches;
	try
	{
		pixels.resize(subresource_count);
		row_pitches.resize(subresource_count);
		slice_pitches.resize(subresource_count);
		for (unsigned int index = 0; index < subresource_count; ++index)
		{
			if (subresources[index].data == nullptr ||
				subresources[index].rowPitch == 0 ||
				subresources[index].slicePitch == 0)
			{
				if (NativeTexture == nullptr) delete storage;
				return false;
			}
			pixels[index].resize(subresources[index].slicePitch);
			memcpy(&pixels[index][0], subresources[index].data,
				subresources[index].slicePitch);
			row_pitches[index] = subresources[index].rowPitch;
			slice_pitches[index] = subresources[index].slicePitch;
		}
	}
	catch (...)
	{
		if (NativeTexture == nullptr) delete storage;
		return false;
	}

	const unsigned int publication = storage->owner.PublicationGeneration();
	if (storage->owner.PublishCandidate(&candidate, publication) !=
		rts::render::RENDER_RESULT_OK)
	{
		if (NativeTexture == nullptr) delete storage;
		return false;
	}
	storage->descriptor = product_descriptor;
	storage->pixels.swap(pixels);
	storage->rowPitches.swap(row_pitches);
	storage->slicePitches.swap(slice_pitches);
	storage->gpuLease = rts::render::NativeW3DGpuContentLease();
	storage->sourceFormat = source_format;
	storage->missing = missing_texture;
	NativeTexture = storage;
	Release_D3D_Texture();
	Width = static_cast<int>(product_descriptor.width);
	Height = static_cast<int>(product_descriptor.height);
	MipLevelCount = static_cast<MipCountType>(product_descriptor.mipCount);
	if (initialized) Initialized = true;
	if (disable_auto_invalidation) InactivationTime = 0;
	return true;
}

bool TextureBaseClass::Apply_Native_Missing_Texture()
{
	static const unsigned char pixels[16] = {
		0xff, 0x00, 0xff, 0xff, 0x00, 0x00, 0x00, 0xff,
		0x00, 0x00, 0x00, 0xff, 0xff, 0x00, 0xff, 0xff
	};
	rts::render::TextureDescriptor descriptor;
	descriptor.width = 2;
	descriptor.height = 2;
	descriptor.mipCount = 1;
	descriptor.arrayCount = 1;
	descriptor.dimension = rts::render::RENDER_TEXTURE_2D;
	descriptor.format = rts::render::RENDER_FORMAT_B8G8R8A8_UNORM;
	descriptor.binding = rts::render::RENDER_TEXTURE_SHADER_RESOURCE;
	descriptor.usage = rts::render::RENDER_USAGE_IMMUTABLE;
	rts::render::TextureSubresourceData subresource;
	subresource.data = pixels;
	subresource.rowPitch = 8;
	subresource.slicePitch = sizeof(pixels);
	return Apply_Native_Texture(descriptor, &subresource, 1,
		WW3D_FORMAT_A8R8G8B8, true, false, true);
}

bool TextureBaseClass::Acquire_Native_Texture(
	rts::render::NativeW3DTextureHandle *handle,
	rts::render::NativeW3DGpuContentLease *gpu_lease) const
{
	if (handle == nullptr || NativeTexture == nullptr) return false;
	rts::render::NativeW3DGpuContentLease *lease = gpu_lease == nullptr ?
		&NativeTexture->gpuLease : gpu_lease;
	const bool caller_requested_generation = handle->isValid() ||
		(gpu_lease != nullptr && gpu_lease->isValid());
	if (NativeTexture->owner.AcquireForSampling(handle, lease) ==
		rts::render::RENDER_RESULT_OK) return true;
	if (caller_requested_generation) return false;
	if (!Refresh_Native_CPU_Content()) return false;
	*handle = rts::render::NativeW3DTextureHandle();
	*lease = rts::render::NativeW3DGpuContentLease();
	return NativeTexture->owner.AcquireForSampling(handle, lease) ==
		rts::render::RENDER_RESULT_OK;
}

bool TextureBaseClass::Acquire_Native_Surface(unsigned int mip_level,
	unsigned int array_slice, bool for_output,
	rts::render::NativeW3DSurfaceHandle *surface,
	rts::render::NativeW3DGpuContentLease *gpu_lease) const
{
	if (surface == nullptr || NativeTexture == nullptr) return false;
	if (for_output)
	{
		if (NativeTexture->owner.AcquireOutputSurface(mip_level, array_slice,
			surface) == rts::render::RENDER_RESULT_OK) return true;
		// A cached typed surface expires across backend recovery. The owner has
		// cleared it on failure, so reacquire the same logical output once.
		return NativeTexture->owner.AcquireOutputSurface(mip_level, array_slice,
			surface) == rts::render::RENDER_RESULT_OK;
	}
	rts::render::NativeW3DGpuContentLease *lease = gpu_lease == nullptr ?
		&NativeTexture->gpuLease : gpu_lease;
	const bool caller_requested_generation = surface->isValid() ||
		(gpu_lease != nullptr && gpu_lease->isValid());
	if (NativeTexture->owner.AcquireSurface(mip_level, array_slice, surface,
		lease) == rts::render::RENDER_RESULT_OK) return true;
	if (caller_requested_generation) return false;
	if (!Refresh_Native_CPU_Content()) return false;
	*surface = rts::render::NativeW3DSurfaceHandle();
	*lease = rts::render::NativeW3DGpuContentLease();
	return NativeTexture->owner.AcquireSurface(mip_level, array_slice, surface,
		lease) == rts::render::RENDER_RESULT_OK;
}

bool TextureBaseClass::Publish_Native_Output(
	rts::render::NativeW3DSurfaceHandle surface,
	rts::render::NativeW3DGpuContentLease *gpu_lease) const
{
	if (NativeTexture == nullptr) return false;
	rts::render::NativeW3DGpuContentLease *lease = gpu_lease == nullptr ?
		&NativeTexture->gpuLease : gpu_lease;
	return NativeTexture->owner.PublishOutputWrite(surface, lease) ==
		rts::render::RENDER_RESULT_OK;
}

bool TextureBaseClass::Copy_Native_Active_Color_Target()
{
	if (NativeTexture == nullptr) return false;
	return NativeTexture->owner.CopyActiveColorTarget(
		&NativeTexture->gpuLease) == rts::render::RENDER_RESULT_OK;
}

bool TextureBaseClass::Publish_Native_BGRA8(const void *data,
	size_t row_pitch, size_t slice_pitch)
{
	if (NativeTexture == nullptr || data == nullptr ||
		NativeTexture->descriptor.dimension != rts::render::RENDER_TEXTURE_2D ||
		NativeTexture->descriptor.format !=
			rts::render::RENDER_FORMAT_B8G8R8A8_UNORM ||
		NativeTexture->descriptor.arrayCount != 1 ||
		NativeTexture->descriptor.mipCount != 1)
	{
		return false;
	}
	return Update_Native_Subresource_Data(0, 0,
		static_cast<const unsigned char *>(data), row_pitch, slice_pitch);
}

bool TextureBaseClass::Generate_Native_Mip_Levels()
{
	if (NativeTexture == nullptr || NativeTexture->descriptor.width == 0 ||
		NativeTexture->descriptor.height == 0) return false;
	rts::render::NativeW3DTextureHandle cpu_handle;
	if (NativeTexture->owner.AcquireForSampling(&cpu_handle) !=
		rts::render::RENDER_RESULT_OK) return false;
	const unsigned int mip_count = NativeTexture->descriptor.mipCount;
	if (mip_count < 2) return true;
	const WW3DFormat mip_format = NativeTexture->descriptor.format ==
		rts::render::RENDER_FORMAT_B8G8R8A8_UNORM ?
		WW3D_FORMAT_A8R8G8B8 : NativeTexture->sourceFormat;
	const unsigned int array_count = NativeTexture->descriptor.arrayCount;
	const unsigned int count = mip_count * array_count;
	if (count != NativeTexture->pixels.size() ||
		count != NativeTexture->rowPitches.size() ||
		count != NativeTexture->slicePitches.size()) return false;
	for (unsigned int slice = 0; slice < array_count; ++slice)
	{
		unsigned int width = NativeTexture->descriptor.width;
		unsigned int height = NativeTexture->descriptor.height;
		for (unsigned int mip = 1; mip < mip_count; ++mip)
		{
			const unsigned int source = slice * mip_count + mip - 1;
			const unsigned int destination = source + 1;
			if (NativeTexture->pixels[source].empty() ||
				NativeTexture->pixels[destination].empty() ||
				NativeTexture->rowPitches[source] > UINT_MAX ||
				NativeTexture->rowPitches[destination] > UINT_MAX ||
				!Generate_Texture_Mip_Level_Box(
					&NativeTexture->pixels[source][0],
					static_cast<unsigned int>(NativeTexture->rowPitches[source]),
					width, height, &NativeTexture->pixels[destination][0],
					static_cast<unsigned int>(NativeTexture->rowPitches[destination]),
					mip_format))
			{
				return false;
			}
			ReduceTextureMipDimensions(width, height);
		}
	}
	std::vector<rts::render::TextureSubresourceData> subresources;
	try { subresources.resize(count); }
	catch (...) { return false; }
	for (unsigned int index = 0; index < count; ++index)
	{
		subresources[index].data = &NativeTexture->pixels[index][0];
		subresources[index].rowPitch = NativeTexture->rowPitches[index];
		subresources[index].slicePitch = NativeTexture->slicePitches[index];
	}
	return Apply_Native_Texture(NativeTexture->descriptor, &subresources[0],
		count, NativeTexture->sourceFormat, true, InactivationTime == 0,
		NativeTexture->missing);
}

bool TextureBaseClass::Refresh_Native_CPU_Content() const
{
	if (NativeTexture == nullptr) return false;
	const unsigned int count = NativeTexture->descriptor.mipCount *
		NativeTexture->descriptor.arrayCount;
	if (count == 0 || count != NativeTexture->pixels.size() ||
		count != NativeTexture->rowPitches.size() ||
		count != NativeTexture->slicePitches.size()) return false;
	std::vector<rts::render::TextureSubresourceData> subresources;
	try { subresources.resize(count); }
	catch (...) { return false; }
	for (unsigned int index = 0; index < count; ++index)
	{
		if (NativeTexture->pixels[index].empty()) return false;
		subresources[index].data = &NativeTexture->pixels[index][0];
		subresources[index].rowPitch = NativeTexture->rowPitches[index];
		subresources[index].slicePitch = NativeTexture->slicePitches[index];
	}
	const bool refreshed = NativeTexture->owner.RefreshCpuContent(
		NativeTexture->descriptor, &subresources[0], count) ==
		rts::render::RENDER_RESULT_OK;
	if (refreshed)
		NativeTexture->gpuLease = rts::render::NativeW3DGpuContentLease();
	return refreshed;
}

bool TextureBaseClass::Get_Native_Subresource_Data(unsigned int mip_level,
	unsigned int array_slice, const unsigned char **data, size_t *row_pitch,
	size_t *slice_pitch) const
{
	if (data == nullptr || row_pitch == nullptr || slice_pitch == nullptr)
		return false;
	*data = nullptr;
	*row_pitch = 0;
	*slice_pitch = 0;
	if (NativeTexture == nullptr || mip_level >= NativeTexture->descriptor.mipCount ||
		array_slice >= NativeTexture->descriptor.arrayCount)
		return false;
	// A retained upload image is authoritative only while the registry still
	// reports CPU content. Never expose the stale pre-render bytes of a GPU
	// render target as a lockable surface view.
	rts::render::NativeW3DTextureHandle cpu_handle;
	if (NativeTexture->owner.AcquireForSampling(&cpu_handle) !=
		rts::render::RENDER_RESULT_OK) return false;
	const unsigned int index = array_slice * NativeTexture->descriptor.mipCount +
		mip_level;
	if (index >= NativeTexture->pixels.size() ||
		NativeTexture->pixels[index].empty()) return false;
	*data = &NativeTexture->pixels[index][0];
	*row_pitch = NativeTexture->rowPitches[index];
	*slice_pitch = NativeTexture->slicePitches[index];
	return true;
}

bool TextureBaseClass::Update_Native_Subresource_Data(unsigned int mip_level,
	unsigned int array_slice, const unsigned char *data, size_t row_pitch,
	size_t slice_pitch)
{
	if (NativeTexture == nullptr || data == nullptr || row_pitch == 0 ||
		slice_pitch == 0 || mip_level >= NativeTexture->descriptor.mipCount ||
		array_slice >= NativeTexture->descriptor.arrayCount) return false;
	rts::render::NativeW3DTextureHandle cpu_handle;
	if (NativeTexture->owner.AcquireForSampling(&cpu_handle) !=
		rts::render::RENDER_RESULT_OK) return false;
	const unsigned int count = NativeTexture->descriptor.mipCount *
		NativeTexture->descriptor.arrayCount;
	const unsigned int replaced = array_slice * NativeTexture->descriptor.mipCount +
		mip_level;
	if (replaced >= count || count != NativeTexture->pixels.size() ||
		count != NativeTexture->rowPitches.size() ||
		count != NativeTexture->slicePitches.size() ||
		row_pitch != NativeTexture->rowPitches[replaced] ||
		slice_pitch != NativeTexture->slicePitches[replaced]) return false;
	std::vector<rts::render::TextureSubresourceData> subresources;
	try { subresources.resize(count); }
	catch (...) { return false; }
	for (unsigned int index = 0; index < count; ++index)
	{
		if (NativeTexture->pixels[index].empty()) return false;
		subresources[index].data = index == replaced ? data :
			&NativeTexture->pixels[index][0];
		subresources[index].rowPitch = NativeTexture->rowPitches[index];
		subresources[index].slicePitch = NativeTexture->slicePitches[index];
	}
	return Apply_Native_Texture(NativeTexture->descriptor, &subresources[0],
		count, NativeTexture->sourceFormat, true, InactivationTime == 0,
		NativeTexture->missing);
}

size_t TextureBaseClass::Get_Native_Texture_Byte_Count() const
{
	if (NativeTexture == nullptr) return 0;
	size_t size = 0;
	for (unsigned int index = 0; index < NativeTexture->slicePitches.size(); ++index)
		size += NativeTexture->slicePitches[index];
	return size;
}
#endif

void TextureBaseClass::Release_D3D_Texture()
{
	if (D3DTexture != nullptr)
	{
		D3DTexture->Release();
		D3DTexture = nullptr;
	}
}




//**********************************************************************************************
//! Invalidate old unused textures
/*!
*/
void TextureBaseClass::Invalidate_Old_Unused_Textures(unsigned invalidation_time_override)
{
	// (gth) If thumbnails are not enabled, then we don't run this code.
	if (WW3D::Get_Thumbnail_Enabled() == false) {
		return;
	}

	// Zero the texture apply count in this function because this is called every frame...(this wasn't in E&B main branch KJM)
	TexturesAppliedPerFrame=0;

	unsigned synctime=WW3D::Get_Sync_Time();
	HashTemplateIterator<StringClass,TextureClass*> ite(WW3DAssetManager::Get_Instance()->Texture_Hash());
	// Loop through all the textures in the manager

	for (ite.First ();!ite.Is_Done();ite.Next ())
	{
		TextureClass* tex=ite.Peek_Value();

		// Consider invalidating if texture has been initialized and defines inactivation time
		if (tex->Initialized && tex->InactivationTime)
		{
			unsigned age=synctime-tex->LastAccessed;

			if (invalidation_time_override)
			{
				if (age>invalidation_time_override)
				{
					tex->Invalidate();
					tex->LastInactivationSyncTime=synctime;
				}
			}
			else
			{
				// Not used in the last n milliseconds?
				if (age>(tex->InactivationTime+tex->ExtendedInactivationTime))
				{
					tex->Invalidate();
					tex->LastInactivationSyncTime=synctime;
				}
			}
		}
	}
}





//**********************************************************************************************
//! Invalidate this texture
/*!
*/
void TextureBaseClass::Invalidate()
{
	if (TextureLoadTask) {
		return;
	}
	if (ThumbnailLoadTask) {
		return;
	}

	// Don't invalidate procedural textures
	if (IsProcedural) {
		return;
	}

	Release_D3D_Texture();
#if defined(_WIN64)
	Release_Native_Texture();
#endif

	Initialized=false;

	LastAccessed=WW3D::Get_Sync_Time();
/*	was battlefield version// If the texture has already been initialised we should exit now
	if (Initialized) return;

	WWPROFILE(("TextureClass::Init()"));

	// If the texture has recently been inactivated, increase the inactivation time (this texture obviously
	// should not have been inactivated yet).

	if (InactivationTime && LastInactivationSyncTime) {
		if ((WW3D::Get_Sync_Time()-LastInactivationSyncTime)<InactivationTime) {
			ExtendedInactivationTime=3*InactivationTime;
		}
		LastInactivationSyncTime=0;
	}

	if (ThumbnailLoadTask)
	{
		return;
	}

	// Don't invalidate procedural textures
	if (IsProcedural)
	{
		return;
	}

	if (D3DTexture)
	{
		D3DTexture->Release();
		D3DTexture = nullptr;
	}

	Initialized=false;

	LastAccessed=WW3D::Get_Sync_Time();*/
}

//**********************************************************************************************
//! Returns a pointer to the d3d texture
/*!
*/
IDirect3DBaseTexture8 * TextureBaseClass::Peek_D3D_Base_Texture() const
{
	LastAccessed=WW3D::Get_Sync_Time();
	return D3DTexture;
}

//**********************************************************************************************
//! Set the d3d texture pointer.  Handles ref counts properly.
/*!
*/
void TextureBaseClass::Set_D3D_Base_Texture(IDirect3DBaseTexture8* tex)
{
	// (gth) Generals does stuff directly with the D3DTexture pointer so lets
	// reset the access timer whenever someon messes with this pointer.
	LastAccessed=WW3D::Get_Sync_Time();

	if (D3DTexture != nullptr) {
		D3DTexture->Release();
	}
	D3DTexture = tex;
	if (D3DTexture != nullptr) {
		D3DTexture->AddRef();
	}
}


//**********************************************************************************************
//! Load locked surface
/*!
*/
void TextureBaseClass::Load_Locked_Surface()
{
	WWPROFILE(("TextureClass::Load_Locked_Surface()"));
	Release_D3D_Texture();
	TextureLoader::Request_Thumbnail(this);
	Initialized=false;
}


//**********************************************************************************************
//! Is missing texture
/*!
*/
bool TextureBaseClass::Is_Missing_Texture()
{
#if defined(_WIN64)
	return NativeTexture != nullptr && NativeTexture->missing;
#else
	bool flag = false;
	IDirect3DBaseTexture8 *missing_texture = MissingTexture::_Get_Missing_Texture();

	if (D3DTexture == missing_texture)
		flag = true;

	if (missing_texture)
	{
		missing_texture->Release();
	}

	return flag;
#endif
}


//**********************************************************************************************
//! Set texture name
/*!
*/
void TextureBaseClass::Set_Texture_Name(const char * name)
{
	Name=name;
}




//**********************************************************************************************
//! Get priority
/*!
*/
unsigned int TextureBaseClass::Get_Priority()
{
#if defined(_WIN64)
	return 0;
#else
	if (!D3DTexture)
	{
		WWASSERT_PRINT(0, "Get_Priority: D3DTexture is null!");
		return 0;
	}

	return D3DTexture->GetPriority();
#endif
}


//**********************************************************************************************
//! Set priority
/*!
*/
unsigned int TextureBaseClass::Set_Priority(unsigned int priority)
{
#if defined(_WIN64)
	(void)priority;
	return 0;
#else
	if (!D3DTexture)
	{
		WWASSERT_PRINT(0, "Set_Priority: D3DTexture is null!");
		return 0;
	}

	return D3DTexture->SetPriority(priority);
#endif
}


//**********************************************************************************************
//! Get reduction mip levels
/*!
*/
unsigned TextureBaseClass::Get_Reduction() const
{
	// don't reduce if the texture is too small already or
	// has no mip map levels
	if (MipLevelCount==MIP_LEVELS_1) return 0;
	if (Width <= 32 || Height <= 32) return 0;

	int reduction=WW3D::Get_Texture_Reduction();

	// 'large texture extra reduction' causes textures above 256x256 to be reduced one more step.
	if (WW3D::Is_Large_Texture_Extra_Reduction_Enabled() && (Width > 256 || Height > 256)) {
		reduction++;
	}
	if (MipLevelCount && reduction>MipLevelCount) {
		reduction=MipLevelCount;
	}
	return reduction;
}



//**********************************************************************************************
//! Apply null texture state
/*!
*/
void TextureBaseClass::Apply_Null(unsigned int stage)
{
	// This function sets the render states for a "null" texture
#if defined(_WIN64)
	DX8Wrapper::Set_Native_Texture(stage, nullptr);
#else
	DX8Wrapper::Set_DX8_Texture(stage, nullptr);
#endif
}

// ----------------------------------------------------------------------------
// Setting HSV_Shift value is always relative to the original texture. This function invalidates the
// texture surface and causes the texture to be reloaded. For thumbnailable textures, the hue shifting
// is done in the background loading thread.
// ----------------------------------------------------------------------------
void TextureBaseClass::Set_HSV_Shift(const Vector3 &hsv_shift)
{
	Invalidate();
	HSVShift=hsv_shift;
}

//**********************************************************************************************
//! Get total locked surface size
/*! KM
*/
int TextureBaseClass::_Get_Total_Locked_Surface_Size()
{
	int total_locked_surface_size=0;

	HashTemplateIterator<StringClass,TextureClass*> ite(WW3DAssetManager::Get_Instance()->Texture_Hash());
	// Loop through all the textures in the manager
	for (ite.First ();!ite.Is_Done();ite.Next ())
	{
		// Get the current texture
		TextureBaseClass* tex=ite.Peek_Value();
		if (!tex->Initialized)
		{
			total_locked_surface_size+=tex->Get_Texture_Memory_Usage();
		}
	}
	return total_locked_surface_size;
}

//**********************************************************************************************
//! Get total texture size
/*! KM
*/
int TextureBaseClass::_Get_Total_Texture_Size()
{
	int total_texture_size=0;

	HashTemplateIterator<StringClass,TextureClass*> ite(WW3DAssetManager::Get_Instance()->Texture_Hash());
	// Loop through all the textures in the manager
	for (ite.First ();!ite.Is_Done();ite.Next ())
	{
		// Get the current texture
		TextureBaseClass* tex=ite.Peek_Value();
		total_texture_size+=tex->Get_Texture_Memory_Usage();
	}
	return total_texture_size;
}

// ----------------------------------------------------------------------------


//**********************************************************************************************
//! Get total lightmap texture size
/*!
*/
int TextureBaseClass::_Get_Total_Lightmap_Texture_Size()
{
	int total_texture_size=0;

	HashTemplateIterator<StringClass,TextureClass*> ite(WW3DAssetManager::Get_Instance()->Texture_Hash());
	// Loop through all the textures in the manager
	for (ite.First ();!ite.Is_Done();ite.Next ())
	{
		// Get the current texture
		TextureBaseClass* tex=ite.Peek_Value();
		if (tex->Is_Lightmap())
		{
			total_texture_size+=tex->Get_Texture_Memory_Usage();
		}
	}
	return total_texture_size;
}


//**********************************************************************************************
//! Get total procedural texture size
/*!
*/
int TextureBaseClass::_Get_Total_Procedural_Texture_Size()
{
	int total_texture_size=0;

	HashTemplateIterator<StringClass,TextureClass*> ite(WW3DAssetManager::Get_Instance()->Texture_Hash());
	// Loop through all the textures in the manager
	for (ite.First ();!ite.Is_Done();ite.Next ())
	{
		// Get the current texture
		TextureBaseClass* tex=ite.Peek_Value();
		if (tex->Is_Procedural())
		{
			total_texture_size+=tex->Get_Texture_Memory_Usage();
		}
	}
	return total_texture_size;
}

//**********************************************************************************************
//! Get total texture count
/*!
*/
int TextureBaseClass::_Get_Total_Texture_Count()
{
	int texture_count=0;

	HashTemplateIterator<StringClass,TextureClass*> ite(WW3DAssetManager::Get_Instance()->Texture_Hash());
	// Loop through all the textures in the manager
	for (ite.First ();!ite.Is_Done();ite.Next ())
	{
		texture_count++;
	}

	return texture_count;
}

// ----------------------------------------------------------------------------


//**********************************************************************************************
//! Get total light map texture count
/*!
*/
int TextureBaseClass::_Get_Total_Lightmap_Texture_Count()
{
	int texture_count=0;

	HashTemplateIterator<StringClass,TextureClass*> ite(WW3DAssetManager::Get_Instance()->Texture_Hash());
	// Loop through all the textures in the manager
	for (ite.First ();!ite.Is_Done();ite.Next ())
	{
		if (ite.Peek_Value()->Is_Lightmap())
		{
			texture_count++;
		}
	}

	return texture_count;
}

//**********************************************************************************************
//! Get total procedural texture count
/*!
*/
int TextureBaseClass::_Get_Total_Procedural_Texture_Count()
{
	int texture_count=0;

	HashTemplateIterator<StringClass,TextureClass*> ite(WW3DAssetManager::Get_Instance()->Texture_Hash());
	// Loop through all the textures in the manager
	for (ite.First ();!ite.Is_Done();ite.Next ())
	{
		if (ite.Peek_Value()->Is_Procedural())
		{
			texture_count++;
		}
	}

	return texture_count;
}


//**********************************************************************************************
//! Get total locked surface count
/*!
*/
int TextureBaseClass::_Get_Total_Locked_Surface_Count()
{
	int texture_count=0;

	HashTemplateIterator<StringClass,TextureClass*> ite(WW3DAssetManager::Get_Instance()->Texture_Hash());
	// Loop through all the textures in the manager
	for (ite.First ();!ite.Is_Done();ite.Next ())
	{
		// Get the current texture
		TextureBaseClass* tex=ite.Peek_Value();
		if (!tex->Initialized)
		{
			texture_count++;
		}
	}

	return texture_count;
}

/*************************************************************************
**                             TextureClass
*************************************************************************/
TextureClass::TextureClass
(
	unsigned width,
	unsigned height,
	WW3DFormat format,
	MipCountType mip_level_count,
	PoolType pool,
	bool rendertarget,
	bool allow_reduction
)
:	TextureBaseClass(width, height, mip_level_count, pool, rendertarget,allow_reduction),
	Filter(mip_level_count),
	TextureFormat(format)
{
	Initialized=true;
	IsProcedural=true;
	IsReducible=false;

	switch (format)
	{
	case WW3D_FORMAT_DXT1:
	case WW3D_FORMAT_DXT2:
	case WW3D_FORMAT_DXT3:
	case WW3D_FORMAT_DXT4:
	case WW3D_FORMAT_DXT5:
		IsCompressionAllowed=true;
		break;
	default : break;
	}

#if defined(_WIN64)
	if (!Apply_Native_Empty_Texture(this, width, height, format,
		mip_level_count, 1, rendertarget))
	{
		Initialized = false;
	}
#else
	D3DPOOL d3dpool=(D3DPOOL)0;
	switch(pool)
	{
	case POOL_DEFAULT		: d3dpool=D3DPOOL_DEFAULT; break;
	case POOL_MANAGED		: d3dpool=D3DPOOL_MANAGED; break;
	case POOL_SYSTEMMEM	: d3dpool=D3DPOOL_SYSTEMMEM; break;
	default: WWASSERT(0);
	}

	Poke_Texture
	(
		DX8Wrapper::_Create_DX8_Texture
		(
			width,
			height,
			format,
			mip_level_count,
			d3dpool,
			rendertarget
		)
	);

	if (pool==POOL_DEFAULT)
	{
		Set_Dirty();
		DX8TextureTrackerClass *track=new DX8TextureTrackerClass
		(
			width,
			height,
			format,
			mip_level_count,
			this,
			rendertarget
		);
		DX8TextureManagerClass::Add(track);
	}
#endif
	LastAccessed=WW3D::Get_Sync_Time();
}



// ----------------------------------------------------------------------------
TextureClass::TextureClass
(
	const char *name,
	const char *full_path,
	MipCountType mip_level_count,
	WW3DFormat texture_format,
	bool allow_compression,
	bool allow_reduction
)
:	TextureBaseClass(0, 0, mip_level_count),
	Filter(mip_level_count),
	TextureFormat(texture_format)
{
	IsCompressionAllowed=allow_compression;
	InactivationTime=DEFAULT_INACTIVATION_TIME;		// Default inactivation time 30 seconds
	IsReducible=allow_reduction;

	switch (TextureFormat)
	{
	case WW3D_FORMAT_DXT1:
	case WW3D_FORMAT_DXT2:
	case WW3D_FORMAT_DXT3:
	case WW3D_FORMAT_DXT4:
	case WW3D_FORMAT_DXT5:
		IsCompressionAllowed=true;
		break;
	case WW3D_FORMAT_U8V8:		// Bumpmap
	case WW3D_FORMAT_L6V5U5:	// Bumpmap
	case WW3D_FORMAT_X8L8V8U8:	// Bumpmap
		// If requesting bumpmap format that isn't available we'll just return the surface in whatever color
		// format the texture file is in. (This is illegal case, the format support should always be queried
		// before creating a bump texture!)
		if (!DX8Wrapper::Is_Initted() || !DX8Wrapper::Get_Current_Caps()->Support_Texture_Format(TextureFormat))
		{
			TextureFormat=WW3D_FORMAT_UNKNOWN;
		}
		// If bump format is valid, make sure compression is not allowed so that we don't even attempt to load
		// from a compressed file (quality isn't good enough for bump map). Also disable mipmapping.
		else
		{
			IsCompressionAllowed=false;
			MipLevelCount=MIP_LEVELS_1;
			Filter.Set_Mip_Mapping(TextureFilterClass::FILTER_TYPE_NONE);
		}
		break;
	default:	break;
	}

	WWASSERT_PRINT(name && name[0], "TextureClass CTor: null or empty texture name");
	int len=strlen(name);
	for (int i=0;i<len;++i)
	{
		if (name[i]=='+')
		{
			IsLightmap=true;

			// Set bilinear filtering for lightmaps (they are very stretched and
			// low detail so we don't care for anisotropic or trilinear filtering...)
			Filter.Set_Min_Filter(TextureFilterClass::FILTER_TYPE_FAST);
			Filter.Set_Mag_Filter(TextureFilterClass::FILTER_TYPE_FAST);
			if (mip_level_count!=MIP_LEVELS_1) Filter.Set_Mip_Mapping(TextureFilterClass::FILTER_TYPE_FAST);
			break;
		}
	}
	Set_Texture_Name(name);
	Set_Full_Path(full_path);
	WWASSERT(name[0]!='\0');
	if (!WW3D::Is_Texturing_Enabled())
	{
		Initialized=true;
		Poke_Texture(nullptr);
	}

	// Find original size from the thumbnail (but don't create thumbnail texture yet!)
	ThumbnailClass* thumb=ThumbnailManagerClass::Peek_Thumbnail_Instance_From_Any_Manager(Get_Full_Path());
	if (thumb)
	{
		Width=thumb->Get_Original_Texture_Width();
		Height=thumb->Get_Original_Texture_Height();
 		if (MipLevelCount!=MIP_LEVELS_1) {
 			MipLevelCount=(MipCountType)thumb->Get_Original_Texture_Mip_Level_Count();
 		}
	}

	LastAccessed=WW3D::Get_Sync_Time();

	// If the thumbnails are not enabled, init the texture at this point to avoid stalling when the
	// mesh is rendered.
	if (!WW3D::Get_Thumbnail_Enabled())
	{
		if (TextureLoader::Is_DX8_Thread())
		{
			Init();
		}
	}
}

// ----------------------------------------------------------------------------
TextureClass::TextureClass
(
	SurfaceClass *surface,
	MipCountType mip_level_count
)
:  TextureBaseClass(0,0,mip_level_count),
	Filter(mip_level_count),
	TextureFormat(surface->Get_Surface_Format())
{
	IsProcedural=true;
	Initialized=true;
	IsReducible=false;

	SurfaceClass::SurfaceDescription sd;
	surface->Get_Description(sd);
	Width=sd.Width;
	Height=sd.Height;
	switch (sd.Format)
	{
	case WW3D_FORMAT_DXT1:
	case WW3D_FORMAT_DXT2:
	case WW3D_FORMAT_DXT3:
	case WW3D_FORMAT_DXT4:
	case WW3D_FORMAT_DXT5:
		IsCompressionAllowed=true;
		break;
	default: break;
	}

#if defined(_WIN64)
	int pitch = 0;
	unsigned char *data = static_cast<unsigned char *>(surface->Lock(&pitch));
	TextureMipLayout layout;
	if (data == nullptr || pitch <= 0 ||
		!CalculateTextureMipLayout(sd.Format, sd.Width, sd.Height, 1, layout))
	{
		Initialized = false;
	}
	else
	{
		rts::render::NativeW3DSampledTextureMipView view;
		view.data = data;
		view.rowPitch = static_cast<size_t>(pitch);
		view.dataSize = static_cast<size_t>(pitch) * layout.rowCount;
		rts::render::NativeW3DSampledTextureUpload upload;
		const bool prepared = upload.Prepare(sd.Format, sd.Width, sd.Height,
			1, 1, &view, 1);
		bool published = false;
		if (prepared && mip_level_count == MIP_LEVELS_1)
		{
			published = Apply_Native_Texture(upload.Descriptor(),
				upload.Subresources(), upload.SubresourceCount(), sd.Format, true);
		}
		else if (prepared && Apply_Native_Empty_Texture(this, sd.Width,
			sd.Height, sd.Format, mip_level_count, 1, false))
		{
			const rts::render::TextureSubresourceData &top =
				upload.Subresources()[0];
			published = Update_Native_Subresource_Data(0, 0,
				static_cast<const unsigned char *>(top.data), top.rowPitch,
				top.slicePitch) && Generate_Native_Mip_Levels();
		}
		if (!published)
		{
			Initialized = false;
		}
	}
	if (data != nullptr) surface->Unlock();
#else
	Poke_Texture(DX8Wrapper::_Create_DX8_Texture(surface->Peek_D3D_Surface(),
		mip_level_count));
#endif
	LastAccessed=WW3D::Get_Sync_Time();
}

// ----------------------------------------------------------------------------
TextureClass::TextureClass(IDirect3DBaseTexture8* d3d_texture)
#if defined(_WIN64)
: TextureBaseClass(0, 0, MIP_LEVELS_1), Filter(MIP_LEVELS_1),
	TextureFormat(WW3D_FORMAT_UNKNOWN)
{
	(void)d3d_texture;
	Initialized = Apply_Native_Missing_Texture();
	IsProcedural = true;
	IsReducible = false;
	LastAccessed = WW3D::Get_Sync_Time();
}
#else
:	TextureBaseClass
	(
		0,
		0,
		((MipCountType)d3d_texture->GetLevelCount())
	),
	Filter((MipCountType)d3d_texture->GetLevelCount())
{
	Initialized=true;
	IsProcedural=true;
	IsReducible=false;

	Set_D3D_Base_Texture(d3d_texture);
	IDirect3DSurface8* surface;
	DX8_ErrorCode(Peek_D3D_Texture()->GetSurfaceLevel(0,&surface));
	D3DSURFACE_DESC d3d_desc;
	::ZeroMemory(&d3d_desc, sizeof(D3DSURFACE_DESC));
	DX8_ErrorCode(surface->GetDesc(&d3d_desc));
	Width=d3d_desc.Width;
	Height=d3d_desc.Height;
	TextureFormat=D3DFormat_To_WW3DFormat(d3d_desc.Format);
	switch (TextureFormat)
	{
	case WW3D_FORMAT_DXT1:
	case WW3D_FORMAT_DXT2:
	case WW3D_FORMAT_DXT3:
	case WW3D_FORMAT_DXT4:
	case WW3D_FORMAT_DXT5:
		IsCompressionAllowed=true;
		break;
	default: break;
	}

	LastAccessed=WW3D::Get_Sync_Time();
}
#endif

//**********************************************************************************************
//! Initialise the texture
/*!
*/
void TextureClass::Init()
{
	// If the texture has already been initialised we should exit now
	if (Initialized) return;

	WWPROFILE("TextureClass::Init");

	// If the texture has recently been inactivated, increase the inactivation time (this texture obviously
	// should not have been inactivated yet).
	if (InactivationTime && LastInactivationSyncTime)
	{
		if ((WW3D::Get_Sync_Time()-LastInactivationSyncTime)<InactivationTime)
		{
			ExtendedInactivationTime=3*InactivationTime;
		}
		LastInactivationSyncTime=0;
	}


	bool has_texture = Peek_D3D_Base_Texture() != nullptr;
#if defined(_WIN64)
	rts::render::NativeW3DTextureHandle native_handle;
	has_texture = Acquire_Native_Texture(&native_handle);
#endif
	if (!has_texture)
	{
#if defined(_WIN64)
		TextureLoader::Request_Foreground_Loading(this);
#else
		if (!WW3D::Get_Thumbnail_Enabled() || MipLevelCount==MIP_LEVELS_1)
		{
//		if (MipLevelCount==MIP_LEVELS_1) {
			TextureLoader::Request_Foreground_Loading(this);
		}
		else
		{
			WW3DFormat format=TextureFormat;
			Load_Locked_Surface();
			TextureFormat=format;
		}
#endif
	}

	if (!Initialized)
	{
		TextureLoader::Request_Background_Loading(this);
	}

	LastAccessed=WW3D::Get_Sync_Time();
}

//**********************************************************************************************
//! Apply new surface to texture
/*!
*/
void TextureClass::Apply_New_Surface
(
	IDirect3DBaseTexture8* d3d_texture,
	bool initialized,
	bool disable_auto_invalidation
)
{
#if defined(_WIN64)
	(void)disable_auto_invalidation;
	// A D3D8 pointer is never a valid x64 product publication. The native
	// loader owns conversion; retain a deterministic fallback for residual
	// thumbnail/legacy callers without keeping or dereferencing the COM object.
	if (d3d_texture == nullptr)
	{
		Release_Native_Texture();
		Initialized = false;
		return;
	}
	if (!Apply_Native_Missing_Texture())
	{
		Initialized = false;
	}
	else if (!initialized)
	{
		Initialized = false;
	}
	return;
#else
	if (d3d_texture == nullptr)
	{
		Release_D3D_Texture();
		Initialized = false;
		return;
	}

	// Validate before publishing: even the missing texture can be unavailable
	// after a failed device reset. Preserve the previous texture on failure.
	IDirect3DSurface8* surface = nullptr;
	HRESULT result = static_cast<IDirect3DTexture8*>(d3d_texture)->GetSurfaceLevel(0, &surface);
	if (FAILED(result) || surface == nullptr)
	{
		if (surface != nullptr) surface->Release();
		WWDEBUG_SAY(("Unable to publish texture surface: GetSurfaceLevel returned 0x%08x",
			static_cast<unsigned>(result)));
		return;
	}
	D3DSURFACE_DESC d3d_desc;
	::ZeroMemory(&d3d_desc, sizeof(D3DSURFACE_DESC));
	result = surface->GetDesc(&d3d_desc);
	surface->Release();
	if (FAILED(result))
	{
		WWDEBUG_SAY(("Unable to publish texture surface: GetDesc returned 0x%08x",
			static_cast<unsigned>(result)));
		return;
	}

	Set_D3D_Base_Texture(d3d_texture);
	if (initialized) Initialized=true;
	if (disable_auto_invalidation) InactivationTime = 0;
	if (initialized)
	{
		TextureFormat=D3DFormat_To_WW3DFormat(d3d_desc.Format);
		Width=d3d_desc.Width;
		Height=d3d_desc.Height;
	}
#endif
}


//**********************************************************************************************
//! Apply texture states
/*!
*/
void TextureClass::Apply(unsigned int stage)
{
	// Initialization needs to be done when texture is used if it hasn't been done before.
	// XBOX always initializes textures at creation time.
	if (!Initialized)
	{
		Init();

		/* was in battlefield// Non-thumbnailed textures are always initialized when used
		if (MipLevelCount==MIP_LEVELS_1)
		{
		}
		// Thumbnailed textures have delayed initialization and a background loading system
		else
		{
			// Limit the number of texture initializations per frame to reduce stuttering
			if (TexturesAppliedPerFrame<MAX_TEXTURES_APPLIED_PER_FRAME)
			{
				TexturesAppliedPerFrame++;
				Init();
			}
			else
			{
				// If texture can't be initialized in this frame, at least make sure we have the thumbnail.
				if (!Peek_Texture())
				{
					WW3DFormat format=TextureFormat;
					Load_Locked_Surface();
					TextureFormat=format;
				}
			}
		}*/
	}
	LastAccessed=WW3D::Get_Sync_Time();

	DX8_RECORD_TEXTURE(this);

	// Set texture itself
	if (WW3D::Is_Texturing_Enabled())
	{
#if defined(_WIN64)
		DX8Wrapper::Set_Native_Texture(stage, this);
#else
		DX8Wrapper::Set_DX8_Texture(stage, Peek_D3D_Base_Texture());
#endif
	}
	else
	{
#if defined(_WIN64)
		DX8Wrapper::Set_Native_Texture(stage, nullptr);
#else
		DX8Wrapper::Set_DX8_Texture(stage, nullptr);
#endif
	}

	Filter.Apply(stage);
}

//**********************************************************************************************
//! Get surface from mip level
/*!
*/
SurfaceClass *TextureClass::Get_Surface_Level(unsigned int level)
{
#if defined(_WIN64)
	rts::render::NativeW3DSurfaceHandle surface_handle;
	if (!Acquire_Native_Surface(level, 0, false, &surface_handle))
	{
		return nullptr;
	}
	return new SurfaceClass(this, level, 0);
#else
	if (!Peek_D3D_Texture())
	{
		WWASSERT_PRINT(0, "Get_Surface_Level: D3DTexture is null!");
		return nullptr;
	}

	IDirect3DSurface8 *d3d_surface = nullptr;
	DX8_ErrorCode(Peek_D3D_Texture()->GetSurfaceLevel(level, &d3d_surface));
	SurfaceClass *surface = new SurfaceClass(d3d_surface);
	d3d_surface->Release();

	return surface;
#endif
}

//**********************************************************************************************
//! Get surface description for a mip level
/*!
*/
void TextureClass::Get_Level_Description( SurfaceClass::SurfaceDescription & desc, unsigned int level )
{
	SurfaceClass * surf = Get_Surface_Level(level);
	if (surf != nullptr) {
		surf->Get_Description(desc);
	}
	REF_PTR_RELEASE(surf);
}

//**********************************************************************************************
//! Get D3D surface from mip level
/*!
*/
IDirect3DSurface8 *TextureClass::Get_D3D_Surface_Level(unsigned int level)
{
#if defined(_WIN64)
	(void)level;
	return nullptr;
#else
	if (!Peek_D3D_Texture())
	{
		WWASSERT_PRINT(0, "Get_D3D_Surface_Level: D3DTexture is null!");
		return nullptr;
	}

	IDirect3DSurface8 *d3d_surface = nullptr;
	DX8_ErrorCode(Peek_D3D_Texture()->GetSurfaceLevel(level, &d3d_surface));
	return d3d_surface;
#endif
}

//**********************************************************************************************
//! Get texture memory usage
/*!
*/
unsigned TextureClass::Get_Texture_Memory_Usage() const
{
#if defined(_WIN64)
	size_t size = Get_Native_Texture_Byte_Count();
	return size > static_cast<size_t>(UINT_MAX) ? UINT_MAX :
		static_cast<unsigned int>(size);
#else
	int size=0;
	if (!Peek_D3D_Texture()) return 0;
	for (unsigned i=0;i<Peek_D3D_Texture()->GetLevelCount();++i)
	{
		D3DSURFACE_DESC desc;
		DX8_ErrorCode(Peek_D3D_Texture()->GetLevelDesc(i,&desc));
		size+=desc.Size;
	}
	return size;
#endif
}


// Utility functions
TextureClass* Load_Texture(ChunkLoadClass & cload)
{
	// Assume failure
	TextureClass *newtex = nullptr;

	char name[256];
	if (cload.Open_Chunk () && (cload.Cur_Chunk_ID () == W3D_CHUNK_TEXTURE))
	{

		W3dTextureInfoStruct texinfo;
		bool hastexinfo = false;

		/*
		** Read in the texture filename, and a possible texture info structure.
		*/
		while (cload.Open_Chunk()) {
			switch (cload.Cur_Chunk_ID()) {
				case W3D_CHUNK_TEXTURE_NAME:
					cload.Read(&name,cload.Cur_Chunk_Length());
					break;

				case W3D_CHUNK_TEXTURE_INFO:
					cload.Read(&texinfo,sizeof(W3dTextureInfoStruct));
					hastexinfo = true;
					break;
			};
			cload.Close_Chunk();
		}
		cload.Close_Chunk();

		/*
		** Get the texture from the asset manager
		*/
		if (hastexinfo)
		{

			MipCountType mipcount;

			bool no_lod = ((texinfo.Attributes & W3DTEXTURE_NO_LOD) == W3DTEXTURE_NO_LOD);

			if (no_lod)
			{
				mipcount = MIP_LEVELS_1;
			}
			else
			{
				switch (texinfo.Attributes & W3DTEXTURE_MIP_LEVELS_MASK) {

					case W3DTEXTURE_MIP_LEVELS_ALL:
						mipcount = MIP_LEVELS_ALL;
						break;

					case W3DTEXTURE_MIP_LEVELS_2:
						mipcount = MIP_LEVELS_2;
						break;

					case W3DTEXTURE_MIP_LEVELS_3:
						mipcount = MIP_LEVELS_3;
						break;

					case W3DTEXTURE_MIP_LEVELS_4:
						mipcount = MIP_LEVELS_4;
						break;

					default:
						WWASSERT (false);
						mipcount = MIP_LEVELS_ALL;
						break;
				}
			}

			WW3DFormat format=WW3D_FORMAT_UNKNOWN;

			switch (texinfo.Attributes & W3DTEXTURE_TYPE_MASK)
			{

				case W3DTEXTURE_TYPE_COLORMAP:
					// Do nothing.
					break;

				case W3DTEXTURE_TYPE_BUMPMAP:
				{
					if (DX8Wrapper::Is_Initted() && DX8Wrapper::Get_Current_Caps()->Support_Bump_Envmap())
					{
						// No mipmaps to bumpmap for now
						mipcount=MIP_LEVELS_1;

						if (DX8Wrapper::Get_Current_Caps()->Support_Texture_Format(WW3D_FORMAT_U8V8)) format=WW3D_FORMAT_U8V8;
						else if (DX8Wrapper::Get_Current_Caps()->Support_Texture_Format(WW3D_FORMAT_X8L8V8U8)) format=WW3D_FORMAT_X8L8V8U8;
						else if (DX8Wrapper::Get_Current_Caps()->Support_Texture_Format(WW3D_FORMAT_L6V5U5)) format=WW3D_FORMAT_L6V5U5;
					}
					break;
				}

				default:
					WWASSERT (false);
					break;
			}

			newtex = WW3DAssetManager::Get_Instance()->Get_Texture (name, mipcount, format);

			if (no_lod)
			{
				newtex->Get_Filter().Set_Mip_Mapping(TextureFilterClass::FILTER_TYPE_NONE);
			}
			bool u_clamp = ((texinfo.Attributes & W3DTEXTURE_CLAMP_U) != 0);
			newtex->Get_Filter().Set_U_Addr_Mode(u_clamp ? TextureFilterClass::TEXTURE_ADDRESS_CLAMP : TextureFilterClass::TEXTURE_ADDRESS_REPEAT);
			bool v_clamp = ((texinfo.Attributes & W3DTEXTURE_CLAMP_V) != 0);
			newtex->Get_Filter().Set_V_Addr_Mode(v_clamp ? TextureFilterClass::TEXTURE_ADDRESS_CLAMP : TextureFilterClass::TEXTURE_ADDRESS_REPEAT);

		} else
		{
			newtex = WW3DAssetManager::Get_Instance()->Get_Texture(name);
		}

		WWASSERT(newtex);
	}

	// Return a pointer to the new texture
	return newtex;
}

// Utility function used by Save_Texture
void setup_texture_attributes(TextureClass * tex, W3dTextureInfoStruct * texinfo)
{
	texinfo->Attributes = 0;

	if (tex->Get_Filter().Get_Mip_Mapping() == TextureFilterClass::FILTER_TYPE_NONE) texinfo->Attributes |= W3DTEXTURE_NO_LOD;
	if (tex->Get_Filter().Get_U_Addr_Mode() == TextureFilterClass::TEXTURE_ADDRESS_CLAMP) texinfo->Attributes |= W3DTEXTURE_CLAMP_U;
	if (tex->Get_Filter().Get_V_Addr_Mode() == TextureFilterClass::TEXTURE_ADDRESS_CLAMP) texinfo->Attributes |= W3DTEXTURE_CLAMP_V;
}


void Save_Texture(TextureClass * texture,ChunkSaveClass & csave)
{
	const char * filename;
	W3dTextureInfoStruct texinfo;
	memset(&texinfo,0,sizeof(texinfo));

	filename = texture->Get_Full_Path();

	setup_texture_attributes(texture, &texinfo);

	csave.Begin_Chunk(W3D_CHUNK_TEXTURE_NAME);
	csave.Write(filename,strlen(filename)+1);
	csave.End_Chunk();

	if ((texinfo.Attributes != 0) || (texinfo.AnimType != 0) || (texinfo.FrameCount != 0)) {
		csave.Begin_Chunk(W3D_CHUNK_TEXTURE_INFO);
		csave.Write(&texinfo, sizeof(texinfo));
		csave.End_Chunk();
	}
}


/*!
 *	KJM depth stencil texture constructor
 */
ZTextureClass::ZTextureClass
(
	unsigned width,
	unsigned height,
	WW3DZFormat zformat,
	MipCountType mip_level_count,
	PoolType pool
)
:	TextureBaseClass(width,height, mip_level_count, pool),
	DepthStencilTextureFormat(zformat)
{
#if defined(_WIN64)
	rts::render::TextureDescriptor descriptor;
	descriptor.width = width;
	descriptor.height = height;
	descriptor.mipCount = 1;
	descriptor.arrayCount = 1;
	descriptor.dimension = rts::render::RENDER_TEXTURE_2D;
	descriptor.format = rts::render::RENDER_FORMAT_D24_UNORM_S8_UINT;
	descriptor.binding = rts::render::RENDER_TEXTURE_DEPTH_STENCIL;
	descriptor.usage = rts::render::RENDER_USAGE_DEFAULT;
	std::vector<unsigned char> zero_depth;
	try { zero_depth.resize(static_cast<size_t>(width) * height * 4U, 0); }
	catch (...) { zero_depth.clear(); }
	rts::render::TextureSubresourceData subresource;
	if (!zero_depth.empty())
	{
		subresource.data = &zero_depth[0];
		subresource.rowPitch = static_cast<size_t>(width) * 4U;
		subresource.slicePitch = zero_depth.size();
	}
	Initialized = !zero_depth.empty() && Apply_Native_Texture(descriptor,
		&subresource, 1, WW3D_FORMAT_UNKNOWN, true);
#else
	D3DPOOL d3dpool=(D3DPOOL)0;
	switch (pool)
	{
	case POOL_DEFAULT: d3dpool=D3DPOOL_DEFAULT; break;
	case POOL_MANAGED: d3dpool=D3DPOOL_MANAGED; break;
	case POOL_SYSTEMMEM: d3dpool=D3DPOOL_SYSTEMMEM;	break;
	default:	WWASSERT(0);
	}

	Poke_Texture
	(
		DX8Wrapper::_Create_DX8_ZTexture
		(
			width,
			height,
			zformat,
			mip_level_count,
			d3dpool
		)
	);

	if (pool==POOL_DEFAULT)
	{
		Set_Dirty();
		DX8ZTextureTrackerClass *track=new DX8ZTextureTrackerClass
		(
			width,
			height,
			zformat,
			mip_level_count,
			this
		);
		DX8TextureManagerClass::Add(track);
	}
	Initialized=true;
#endif
	IsProcedural=true;
	IsReducible=false;

	LastAccessed=WW3D::Get_Sync_Time();
}


//**********************************************************************************************
//! Apply depth stencil texture
/*! KM
*/
void ZTextureClass::Apply(unsigned int stage)
{
#if defined(_WIN64)
	DX8Wrapper::Set_Native_Texture(stage, nullptr);
#else
	DX8Wrapper::Set_DX8_Texture(stage, Peek_D3D_Base_Texture());
#endif
}

//**********************************************************************************************
//! Apply new surface to texture
/*! KM
*/
void ZTextureClass::Apply_New_Surface
(
	IDirect3DBaseTexture8* d3d_texture,
	bool initialized,
	bool disable_auto_invalidation
)
{
#if defined(_WIN64)
	(void)d3d_texture;
	(void)initialized;
	(void)disable_auto_invalidation;
	return;
#else
	IDirect3DBaseTexture8* d3d_tex=Peek_D3D_Base_Texture();

	if (d3d_tex) d3d_tex->Release();

	Poke_Texture(d3d_texture);//TextureLoadTask->Peek_D3D_Texture();
	d3d_texture->AddRef();

	if (initialized) Initialized=true;
	if (disable_auto_invalidation) InactivationTime = 0;

	WWASSERT(Peek_D3D_Texture());
	IDirect3DSurface8* surface;
	DX8_ErrorCode(Peek_D3D_Texture()->GetSurfaceLevel(0,&surface));
	D3DSURFACE_DESC d3d_desc;
	::ZeroMemory(&d3d_desc, sizeof(D3DSURFACE_DESC));
	DX8_ErrorCode(surface->GetDesc(&d3d_desc));
	if (initialized)
	{
		DepthStencilTextureFormat=D3DFormat_To_WW3DZFormat(d3d_desc.Format);
		Width=d3d_desc.Width;
		Height=d3d_desc.Height;
	}
	surface->Release();
#endif
}

//**********************************************************************************************
//! Get D3D surface from mip level
/*!
*/
IDirect3DSurface8* ZTextureClass::Get_D3D_Surface_Level(unsigned int level)
{
#if defined(_WIN64)
	(void)level;
	return nullptr;
#else
	if (!Peek_D3D_Texture())
	{
		WWASSERT_PRINT(0, "Get_D3D_Surface_Level: D3DTexture is null!");
		return nullptr;
	}

	IDirect3DSurface8 *d3d_surface = nullptr;
	DX8_ErrorCode(Peek_D3D_Texture()->GetSurfaceLevel(level, &d3d_surface));
	return d3d_surface;
#endif
}

//**********************************************************************************************
//! Get texture memory usage
/*!
*/
unsigned ZTextureClass::Get_Texture_Memory_Usage() const
{
#if defined(_WIN64)
	const size_t size = Get_Native_Texture_Byte_Count();
	return size > static_cast<size_t>(UINT_MAX) ? UINT_MAX :
		static_cast<unsigned int>(size);
#else
	int size=0;
	if (!Peek_D3D_Texture()) return 0;
	for (unsigned i=0;i<Peek_D3D_Texture()->GetLevelCount();++i)
	{
		D3DSURFACE_DESC desc;
		DX8_ErrorCode(Peek_D3D_Texture()->GetLevelDesc(i,&desc));
		size+=desc.Size;
	}
	return size;
#endif
}



/*************************************************************************
**                             CubeTextureClass
*************************************************************************/
CubeTextureClass::CubeTextureClass
(
	unsigned width,
	unsigned height,
	WW3DFormat format,
	MipCountType mip_level_count,
	PoolType pool,
	bool rendertarget,
	bool allow_reduction
)
: TextureClass(width, height, format, mip_level_count, pool, rendertarget)
{
	Initialized=true;
	IsProcedural=true;
	IsReducible=false;

	switch (format)
	{
	case WW3D_FORMAT_DXT1:
	case WW3D_FORMAT_DXT2:
	case WW3D_FORMAT_DXT3:
	case WW3D_FORMAT_DXT4:
	case WW3D_FORMAT_DXT5:
		IsCompressionAllowed=true;
		break;
	default : break;
	}

#if defined(_WIN64)
	if (rendertarget || width != height ||
		!Apply_Native_Empty_Texture(this, width, height, format,
			mip_level_count, 6, false))
	{
		Initialized = Apply_Native_Missing_Texture();
	}
#else
	D3DPOOL d3dpool=(D3DPOOL)0;
	switch(pool)
	{
	case POOL_DEFAULT		: d3dpool=D3DPOOL_DEFAULT; break;
	case POOL_MANAGED		: d3dpool=D3DPOOL_MANAGED; break;
	case POOL_SYSTEMMEM	: d3dpool=D3DPOOL_SYSTEMMEM; break;
	default: WWASSERT(0);
	}

	Poke_Texture
	(
		DX8Wrapper::_Create_DX8_Cube_Texture
		(
			width,
			height,
			format,
			mip_level_count,
			d3dpool,
			rendertarget
		)
	);

	if (pool==POOL_DEFAULT)
	{
		Set_Dirty();
		DX8TextureTrackerClass *track=new DX8TextureTrackerClass
		(
			width,
			height,
			format,
			mip_level_count,
			this,
			rendertarget
		);
		DX8TextureManagerClass::Add(track);
	}
#endif
	LastAccessed=WW3D::Get_Sync_Time();
}



// ----------------------------------------------------------------------------
CubeTextureClass::CubeTextureClass
(
	const char *name,
	const char *full_path,
	MipCountType mip_level_count,
	WW3DFormat texture_format,
	bool allow_compression,
	bool allow_reduction
)
:	TextureClass(0,0,mip_level_count, POOL_MANAGED, false, texture_format)
{
	IsCompressionAllowed=allow_compression;
	InactivationTime=DEFAULT_INACTIVATION_TIME;		// Default inactivation time 30 seconds

	switch (TextureFormat)
	{
	case WW3D_FORMAT_DXT1:
	case WW3D_FORMAT_DXT2:
	case WW3D_FORMAT_DXT3:
	case WW3D_FORMAT_DXT4:
	case WW3D_FORMAT_DXT5:
		IsCompressionAllowed=true;
		break;
	case WW3D_FORMAT_U8V8:		// Bumpmap
	case WW3D_FORMAT_L6V5U5:	// Bumpmap
	case WW3D_FORMAT_X8L8V8U8:	// Bumpmap
		// If requesting bumpmap format that isn't available we'll just return the surface in whatever color
		// format the texture file is in. (This is illegal case, the format support should always be queried
		// before creating a bump texture!)
		if (!DX8Wrapper::Is_Initted() || !DX8Wrapper::Get_Current_Caps()->Support_Texture_Format(TextureFormat))
		{
			TextureFormat=WW3D_FORMAT_UNKNOWN;
		}
		// If bump format is valid, make sure compression is not allowed so that we don't even attempt to load
		// from a compressed file (quality isn't good enough for bump map). Also disable mipmapping.
		else
		{
			IsCompressionAllowed=false;
			MipLevelCount=MIP_LEVELS_1;
			Filter.Set_Mip_Mapping(TextureFilterClass::FILTER_TYPE_NONE);
		}
		break;
	default:	break;
	}

	WWASSERT_PRINT(name && name[0], "TextureClass CTor: null or empty texture name");
	int len=strlen(name);
	for (int i=0;i<len;++i)
	{
		if (name[i]=='+')
		{
			IsLightmap=true;

			// Set bilinear filtering for lightmaps (they are very stretched and
			// low detail so we don't care for anisotropic or trilinear filtering...)
			Filter.Set_Min_Filter(TextureFilterClass::FILTER_TYPE_FAST);
			Filter.Set_Mag_Filter(TextureFilterClass::FILTER_TYPE_FAST);
			if (mip_level_count!=MIP_LEVELS_1) Filter.Set_Mip_Mapping(TextureFilterClass::FILTER_TYPE_FAST);
			break;
		}
	}
	Set_Texture_Name(name);
	Set_Full_Path(full_path);
	WWASSERT(name[0]!='\0');
	if (!WW3D::Is_Texturing_Enabled())
	{
		Initialized=true;
		Poke_Texture(nullptr);
	}

	// Find original size from the thumbnail (but don't create thumbnail texture yet!)
	ThumbnailClass* thumb=ThumbnailManagerClass::Peek_Thumbnail_Instance_From_Any_Manager(Get_Full_Path());
	if (thumb)
	{
		Width=thumb->Get_Original_Texture_Width();
		Height=thumb->Get_Original_Texture_Height();
 		if (MipLevelCount!=MIP_LEVELS_1) {
 			MipLevelCount=(MipCountType)thumb->Get_Original_Texture_Mip_Level_Count();
 		}
	}

	LastAccessed=WW3D::Get_Sync_Time();

	// If the thumbnails are not enabled, init the texture at this point to avoid stalling when the
	// mesh is rendered.
	if (!WW3D::Get_Thumbnail_Enabled())
	{
		if (TextureLoader::Is_DX8_Thread())
		{
			Init();
		}
	}
}

// don't know if these are needed
#if 0
// ----------------------------------------------------------------------------
CubeTextureClass::CubeTextureClass
(
	SurfaceClass *surface,
	MipCountType mip_level_count
)
:	TextureClass(0,0,mip_level_count, POOL_MANAGED, false, surface->Get_Surface_Format())
{
	IsProcedural=true;
	Initialized=true;
	IsReducible=false;

	SurfaceClass::SurfaceDescription sd;
	surface->Get_Description(sd);
	Width=sd.Width;
	Height=sd.Height;
	switch (sd.Format)
	{
	case WW3D_FORMAT_DXT1:
	case WW3D_FORMAT_DXT2:
	case WW3D_FORMAT_DXT3:
	case WW3D_FORMAT_DXT4:
	case WW3D_FORMAT_DXT5:
		IsCompressionAllowed=true;
		break;
	default: break;
	}

	Poke_Texture
	(
		DX8Wrapper::_Create_DX8_Cube_Texture
		(
			surface->Peek_D3D_Surface(),
			mip_level_count
		)
	);
	LastAccessed=WW3D::Get_Sync_Time();
}

// ----------------------------------------------------------------------------
CubeTextureClass::CubeTextureClass(IDirect3DBaseTexture8* d3d_texture)
:	TextureBaseClass
	(
		0,
		0,
		((MipCountType)d3d_texture->GetLevelCount())
	),
	Filter((MipCountType)d3d_texture->GetLevelCount())
{
	Initialized=true;
	IsProcedural=true;
	IsReducible=false;

	Peek_Texture()->AddRef();
	IDirect3DSurface8* surface;
	DX8_ErrorCode(Peek_D3D_Texture()->GetSurfaceLevel(0,&surface));
	D3DSURFACE_DESC d3d_desc;
	::ZeroMemory(&d3d_desc, sizeof(D3DSURFACE_DESC));
	DX8_ErrorCode(surface->GetDesc(&d3d_desc));
	Width=d3d_desc.Width;
	Height=d3d_desc.Height;
	TextureFormat=D3DFormat_To_WW3DFormat(d3d_desc.Format);
	switch (TextureFormat)
	{
	case WW3D_FORMAT_DXT1:
	case WW3D_FORMAT_DXT2:
	case WW3D_FORMAT_DXT3:
	case WW3D_FORMAT_DXT4:
	case WW3D_FORMAT_DXT5:
		IsCompressionAllowed=true;
		break;
	default: break;
	}

	LastAccessed=WW3D::Get_Sync_Time();
}
#endif

//**********************************************************************************************
//! Apply new surface to texture
/*!
*/
void CubeTextureClass::Apply_New_Surface
(
	IDirect3DBaseTexture8* d3d_texture,
	bool initialized,
	bool disable_auto_invalidation
)
{
#if defined(_WIN64)
	(void)d3d_texture;
	(void)disable_auto_invalidation;
	if (initialized && !Apply_Native_Missing_Texture()) Initialized = false;
	return;
#else
	IDirect3DBaseTexture8* d3d_tex=Peek_D3D_Base_Texture();

	if (d3d_tex) d3d_tex->Release();

	Poke_Texture(d3d_texture);//TextureLoadTask->Peek_D3D_Texture();
	d3d_texture->AddRef();

	if (initialized) Initialized=true;
	if (disable_auto_invalidation) InactivationTime = 0;

	WWASSERT(d3d_texture);
	D3DSURFACE_DESC d3d_desc;
	::ZeroMemory(&d3d_desc, sizeof(D3DSURFACE_DESC));
	DX8_ErrorCode(Peek_D3D_CubeTexture()->GetLevelDesc(0,&d3d_desc));

	if (initialized)
	{
		TextureFormat=D3DFormat_To_WW3DFormat(d3d_desc.Format);
		Width=d3d_desc.Width;
		Height=d3d_desc.Height;
	}
#endif
}


/*************************************************************************
**                             VolumeTextureClass
*************************************************************************/
VolumeTextureClass::VolumeTextureClass
(
	unsigned width,
	unsigned height,
	unsigned depth,
	WW3DFormat format,
	MipCountType mip_level_count,
	PoolType pool,
	bool rendertarget,
	bool allow_reduction
)
: TextureClass(width, height, format, mip_level_count, pool, rendertarget),
  Depth(depth)
{
	Initialized=true;
	IsProcedural=true;
	IsReducible=false;

	switch (format)
	{
	case WW3D_FORMAT_DXT1:
	case WW3D_FORMAT_DXT2:
	case WW3D_FORMAT_DXT3:
	case WW3D_FORMAT_DXT4:
	case WW3D_FORMAT_DXT5:
		IsCompressionAllowed=true;
		break;
	default : break;
	}

#if defined(_WIN64)
	// The neutral product renderer intentionally has no 3D-texture dimension.
	// Publish a visible deterministic fallback instead of retaining a D3D8
	// volume facade or leaving all subsequent textured draws broken.
	(void)pool;
	(void)rendertarget;
	Initialized = Apply_Native_Missing_Texture();
#else
	D3DPOOL d3dpool=(D3DPOOL)0;
	switch(pool)
	{
	case POOL_DEFAULT		: d3dpool=D3DPOOL_DEFAULT; break;
	case POOL_MANAGED		: d3dpool=D3DPOOL_MANAGED; break;
	case POOL_SYSTEMMEM	: d3dpool=D3DPOOL_SYSTEMMEM; break;
	default: WWASSERT(0);
	}

	Poke_Texture
	(
		DX8Wrapper::_Create_DX8_Volume_Texture
		(
			width,
			height,
			depth,
			format,
			mip_level_count,
			d3dpool
		)
	);

	if (pool==POOL_DEFAULT)
	{
		Set_Dirty();
		DX8TextureTrackerClass *track=new DX8TextureTrackerClass
		(
			width,
			height,
			format,
			mip_level_count,
			this,
			rendertarget
		);
		DX8TextureManagerClass::Add(track);
	}
#endif
	LastAccessed=WW3D::Get_Sync_Time();
}



// ----------------------------------------------------------------------------
VolumeTextureClass::VolumeTextureClass
(
	const char *name,
	const char *full_path,
	MipCountType mip_level_count,
	WW3DFormat texture_format,
	bool allow_compression,
	bool allow_reduction
)
:	TextureClass(0,0,mip_level_count, POOL_MANAGED, false, texture_format),
	Depth(0)
{
	IsCompressionAllowed=allow_compression;
	InactivationTime=DEFAULT_INACTIVATION_TIME;		// Default inactivation time 30 seconds

	switch (TextureFormat)
	{
	case WW3D_FORMAT_DXT1:
	case WW3D_FORMAT_DXT2:
	case WW3D_FORMAT_DXT3:
	case WW3D_FORMAT_DXT4:
	case WW3D_FORMAT_DXT5:
		IsCompressionAllowed=true;
		break;
	case WW3D_FORMAT_U8V8:		// Bumpmap
	case WW3D_FORMAT_L6V5U5:	// Bumpmap
	case WW3D_FORMAT_X8L8V8U8:	// Bumpmap
		// If requesting bumpmap format that isn't available we'll just return the surface in whatever color
		// format the texture file is in. (This is illegal case, the format support should always be queried
		// before creating a bump texture!)
		if (!DX8Wrapper::Is_Initted() || !DX8Wrapper::Get_Current_Caps()->Support_Texture_Format(TextureFormat))
		{
			TextureFormat=WW3D_FORMAT_UNKNOWN;
		}
		// If bump format is valid, make sure compression is not allowed so that we don't even attempt to load
		// from a compressed file (quality isn't good enough for bump map). Also disable mipmapping.
		else
		{
			IsCompressionAllowed=false;
			MipLevelCount=MIP_LEVELS_1;
			Filter.Set_Mip_Mapping(TextureFilterClass::FILTER_TYPE_NONE);
		}
		break;
	default:	break;
	}

	WWASSERT_PRINT(name && name[0], "TextureClass CTor: null or empty texture name");
	int len=strlen(name);
	for (int i=0;i<len;++i)
	{
		if (name[i]=='+')
		{
			IsLightmap=true;

			// Set bilinear filtering for lightmaps (they are very stretched and
			// low detail so we don't care for anisotropic or trilinear filtering...)
			Filter.Set_Min_Filter(TextureFilterClass::FILTER_TYPE_FAST);
			Filter.Set_Mag_Filter(TextureFilterClass::FILTER_TYPE_FAST);
			if (mip_level_count!=MIP_LEVELS_1) Filter.Set_Mip_Mapping(TextureFilterClass::FILTER_TYPE_FAST);
			break;
		}
	}
	Set_Texture_Name(name);
	Set_Full_Path(full_path);
	WWASSERT(name[0]!='\0');
	if (!WW3D::Is_Texturing_Enabled())
	{
		Initialized=true;
		Poke_Texture(nullptr);
	}

	// Find original size from the thumbnail (but don't create thumbnail texture yet!)
	ThumbnailClass* thumb=ThumbnailManagerClass::Peek_Thumbnail_Instance_From_Any_Manager(Get_Full_Path());
	if (thumb)
	{
		Width=thumb->Get_Original_Texture_Width();
		Height=thumb->Get_Original_Texture_Height();
 		if (MipLevelCount!=MIP_LEVELS_1) {
 			MipLevelCount=(MipCountType)thumb->Get_Original_Texture_Mip_Level_Count();
 		}
	}

	LastAccessed=WW3D::Get_Sync_Time();

	// If the thumbnails are not enabled, init the texture at this point to avoid stalling when the
	// mesh is rendered.
	if (!WW3D::Get_Thumbnail_Enabled())
	{
		if (TextureLoader::Is_DX8_Thread())
		{
			Init();
		}
	}
}

// don't know if these are needed
#if 0
// ----------------------------------------------------------------------------
CubeTextureClass::CubeTextureClass
(
	SurfaceClass *surface,
	MipCountType mip_level_count
)
:	TextureClass(0,0,mip_level_count, POOL_MANAGED, false, surface->Get_Surface_Format())
{
	IsProcedural=true;
	Initialized=true;
	IsReducible=false;

	SurfaceClass::SurfaceDescription sd;
	surface->Get_Description(sd);
	Width=sd.Width;
	Height=sd.Height;
	switch (sd.Format)
	{
	case WW3D_FORMAT_DXT1:
	case WW3D_FORMAT_DXT2:
	case WW3D_FORMAT_DXT3:
	case WW3D_FORMAT_DXT4:
	case WW3D_FORMAT_DXT5:
		IsCompressionAllowed=true;
		break;
	default: break;
	}

	Poke_Texture
	(
		DX8Wrapper::_Create_DX8_Cube_Texture
		(
			surface->Peek_D3D_Surface(),
			mip_level_count
		)
	);
	LastAccessed=WW3D::Get_Sync_Time();
}

// ----------------------------------------------------------------------------
CubeTextureClass::CubeTextureClass(IDirect3DBaseTexture8* d3d_texture)
:	TextureBaseClass
	(
		0,
		0,
		((MipCountType)d3d_texture->GetLevelCount())
	),
	Filter((MipCountType)d3d_texture->GetLevelCount())
{
	Initialized=true;
	IsProcedural=true;
	IsReducible=false;

	Peek_Texture()->AddRef();
	IDirect3DSurface8* surface;
	DX8_ErrorCode(Peek_D3D_Texture()->GetSurfaceLevel(0,&surface));
	D3DSURFACE_DESC d3d_desc;
	::ZeroMemory(&d3d_desc, sizeof(D3DSURFACE_DESC));
	DX8_ErrorCode(surface->GetDesc(&d3d_desc));
	Width=d3d_desc.Width;
	Height=d3d_desc.Height;
	TextureFormat=D3DFormat_To_WW3DFormat(d3d_desc.Format);
	switch (TextureFormat)
	{
	case WW3D_FORMAT_DXT1:
	case WW3D_FORMAT_DXT2:
	case WW3D_FORMAT_DXT3:
	case WW3D_FORMAT_DXT4:
	case WW3D_FORMAT_DXT5:
		IsCompressionAllowed=true;
		break;
	default: break;
	}

	LastAccessed=WW3D::Get_Sync_Time();
}
#endif




//**********************************************************************************************
//! Apply new surface to texture
/*!
*/
void VolumeTextureClass::Apply_New_Surface
(
	IDirect3DBaseTexture8* d3d_texture,
	bool initialized,
	bool disable_auto_invalidation
)
{
#if defined(_WIN64)
	(void)d3d_texture;
	(void)disable_auto_invalidation;
	if (initialized && !Apply_Native_Missing_Texture()) Initialized = false;
	return;
#else
	IDirect3DBaseTexture8* d3d_tex=Peek_D3D_Base_Texture();

	if (d3d_tex) d3d_tex->Release();

	Poke_Texture(d3d_texture);//TextureLoadTask->Peek_D3D_Texture();
	d3d_texture->AddRef();

	if (initialized) Initialized=true;
	if (disable_auto_invalidation) InactivationTime = 0;

	WWASSERT(d3d_texture);
	D3DVOLUME_DESC d3d_desc;
	::ZeroMemory(&d3d_desc, sizeof(D3DVOLUME_DESC));

	DX8_ErrorCode(Peek_D3D_VolumeTexture()->GetLevelDesc(0,&d3d_desc));

	if (initialized)
	{
		TextureFormat=D3DFormat_To_WW3DFormat(d3d_desc.Format);
		Width=d3d_desc.Width;
		Height=d3d_desc.Height;
		Depth=d3d_desc.Depth;
	}
#endif
}
