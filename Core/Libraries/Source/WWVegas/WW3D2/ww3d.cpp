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
 *                     $Archive:: /Commando/Code/ww3d2/ww3d.cpp                               $*
 *                                                                                             *
 *                   Org Author:: Greg_h                                                       *
 *                                                                                             *
 *                       Author : Kenny Mitchell                                               *
 *                                                                                             *
 *								$Modtime:: 08/05/02 10:03a                                             $*
 *                                                                                             *
 *                    $Revision:: 98                                                          $*
 *                                                                                             *
 * 07/01/02 KM Scalable shader library integration				                               *
 * 08/05/02 KM Texture class redesign
 *---------------------------------------------------------------------------------------------*
 * Functions:                                                                                  *
 *   WW3D::Init -- Initialize the WW3D Library                                                 *
 *   WW3D::Shutdown -- shutdown the WW3D Library                                               *
 *   WW3D::Set_Render_Device -- set the render device being currently used                     *
 *   WW3D::Set_Next_Render_Device -- just go to the next device in the list                    *
 *   WW3D::Set_Device_Resolution -- set the current resolution and bitdepth                    *
 *   WW3D::Get_Render_Device -- Get the index of the current render device                     *
 *   WW3D::Get_Render_Device_Desc -- returns description of the current render device          *
 *   WW3D::Get_Render_Device_Count -- returns the number of render devices available           *
 *   WW3D::Get_Render_Device_Name -- returns the name of the n-th render device                *
 *	  WW3D::Get_Render_Target_Resolution -- get the resolution and bitdepth of the current target*
 *   WW3D::Get_Device_Resolution -- get the current resolution and bitdepth                    *
 *   WW3D::Begin_Render -- mark the start of rendering for a new frame                         *
 *   WW3D::Render -- Render a 3D Scene using the given camera                                  *
 *   WW3D::Render -- Render a single render object                                             *
 *   WW3D::End_Render -- Mark the completion of a frame                                        *
 *   WW3D::Sync -- Time synchronization                                                        *
 *   WW3D::Set_Ext_Swap_Interval -- Sets the swap interval the device should aim sync for.     *
 *   WW3D::Get_Ext_Swap_Interval -- Queries the swap interval the device is aiming sync for.   *
 *   WW3D::Get_Polygon_Mode -- returns the current rendering mode                              *
 *   WW3D::Set_Collision_Box_Display_Mask -- control rendering of collision boxes              *
 *   WW3D::Get_Collision_Box_Display_Mask -- returns the current display mask for collision bo *
 *   WW3D::Normalize_Coordinates -- Convert pixel coords to normalized screen coords 0..1      *
 *   WW3D::Update_Render_Device_Description -- updates the description of the current render d *
 *   WW3D::Make_Screen_Shot -- saves a screenshot with the given base filename                 *
 *   WW3D::Start_Movie_Capture -- begins dumping frames to a movie                             *
 *   WW3D::Stop_Movie_Capture -- ends dumping frames to a movie                                *
 *   WW3D::Toggle_Movie_Capture -- toggles movie capture...                                    *
 *   WW3D::Start_Single_Frame_Movie_Capture -- starts capturing a single frame movie           *
 *   WW3D::Capture_Next_Movie_Frame -- tells ww3d to grab another frame for the movie          *
 *   WW3D::Pause_Movie -- pauses/unpauses movie capturing                                      *
 *   WW3D::Is_Movie_Paused -- returns whether the movie capture system is paused               *
 *   WW3D::Is_Recording_Next_Frame -- returns whether the next frame will be dumped to a movie *
 *   WW3D::Is_Movie_Ready -- returns whether the movie capture system is ready                 *
 *   WW3D::Update_Movie_Capture -- dumps the current frame into the movie                      *
 *   WW3D::Get_Movie_Capture_Frame_Rate -- returns the framerate at which the movie is being c *
 *   WW3D::Set_Texture_Reduction -- sets the (hacky) texture reduction factor                  *
 *   WW3D::Get_Texture_Reduction -- gets the (hacky) texture reduction factor                  *
 *   WW3D::Flush_Texture_Cache -- dump all textures from the texture cache                     *
 *   WW3D::Allocate_Debug_Resources -- allocates the debug resources					              *
 *   WW3D::Release_Debug_Resources -- releases the debug resources									  *
 *   WW3D::Get_Last_Frame_Poly_Count -- returns the number of polys submitted in the previous  *
 *   WW3D::Flush -- Process all pending rendering tasks                                        *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */


#include "ww3d.h"
#include "rinfo.h"
#include "assetmgr.h"
#include "boxrobj.h"
#include "predlod.h"
#include "camera.h"
#include "scene.h"
#include "WWLib/registry.h"
#include "segline.h"
#include "shader.h"
#include "vertmaterial.h"
#include "WWDebug/wwdebug.h"
#include "WWDebug/wwprofile.h"
#include "WWDebug/wwmemlog.h"
#include "shattersystem.h"
#include "textureloader.h"
#include "statistics.h"
#include "pointgr.h"
#include "WWLib/ffactory.h"
#include "WWLib/INI.h"
#include "dazzle.h"
#include "meshmdl.h"
#include "render2d.h"
#include "WWLib/bound.h"
#include "rddesc.h"
#include "WWMath/Vector3i.h"
#include "Renderer/RenderGameClient.h"
#include "WWLib/TARGA.h"
#include "WWLib/thread.h"
#include "WWLib/cpudetect.h"
#include "animatedsoundmgr.h"
#include "static_sort_list.h"
#include "shdlib.h"
#include "framgrab.h"

#include <vector>
#include <limits>
#include "Lib/BaseType.h"

namespace
{
bool Checked_Multiply(size_t left, size_t right, size_t *result)
{
	if (result == 0 || (left != 0 && right >
		std::numeric_limits<size_t>::max() / left))
	{
		return false;
	}
	*result = left * right;
	return true;
}

struct GameScreenshotCapture
{
	char filename[80];
	float gamma;
	WW3D::ScreenShotFormatEnum format;
	unsigned int width;
	unsigned int height;
};

// A movie owns a fixed-size AVI buffer. Keep that ownership and the
// dimensions used to allocate it in the queued request rather than passing
// the raw movie pointer through the asynchronous bridge. This makes a
// resize, device recovery, or stop deterministic instead of allowing a later
// frame to write past the old buffer.
struct GameMovieCaptureRequest
{
	FrameGrabClass *movie;
	unsigned int width;
	unsigned int height;
};

std::vector<GameMovieCaptureRequest *> s_gameMovieRequests;
FrameGrabClass *s_gameMovieOwner = 0;
unsigned int s_gameMovieWidth = 0;
unsigned int s_gameMovieHeight = 0;
bool s_gameMovieStopPending = false;

bool Remove_GameMovie_Request(GameMovieCaptureRequest *request)
{
	for (std::vector<GameMovieCaptureRequest *>::iterator it =
		s_gameMovieRequests.begin(); it != s_gameMovieRequests.end(); ++it)
	{
		if (*it == request)
		{
			s_gameMovieRequests.erase(it);
			return true;
		}
	}
	return false;
}

void Complete_GameScreenshot(void *consumer,
	const rts::render::RenderCaptureHandle *, unsigned int width,
	unsigned int height, size_t rowPitch, rts::render::RenderFormat format,
	const void *pixels, size_t pixelBytes)
{
	GameScreenshotCapture *capture =
		static_cast<GameScreenshotCapture *>(consumer);
	if (capture == 0)
	{
		return;
	}
	size_t rowBytes = 0;
	size_t sourceBytes = 0;
	size_t pixelCount = 0;
	size_t imageBytes = 0;
	if (pixels == 0 || format != rts::render::RENDER_FORMAT_B8G8R8A8_UNORM ||
		width == 0 || height == 0 || width != capture->width ||
		height != capture->height || !Checked_Multiply(width, 4, &rowBytes) ||
		rowPitch < rowBytes || !Checked_Multiply(rowPitch, height,
			&sourceBytes) || pixelBytes < sourceBytes ||
		!Checked_Multiply(static_cast<size_t>(width), height, &pixelCount) ||
		!Checked_Multiply(pixelCount, 3, &imageBytes))
	{
		WWDEBUG_SAY(("Native screenshot completion had invalid pixels"));
		delete capture;
		return;
	}
	unsigned char *image = 0;
	try
	{
		image = W3DNEWARRAY unsigned char[imageBytes];
	}
	catch (...)
	{
		WWDEBUG_SAY(("Native screenshot allocation failed"));
		delete capture;
		return;
	}
	unsigned char gammaLut[256];
	float recip = 1.0f;
	if (capture->gamma > WWMATH_EPSILON)
	{
		recip = 1.0f / capture->gamma;
	}
	for (unsigned int value = 0; value < 256; ++value)
	{
		gammaLut[value] = static_cast<unsigned char>(
			256.0f * powf(value / 256.0f, recip));
	}
	const unsigned char *source = static_cast<const unsigned char *>(pixels);
	for (unsigned int y = 0; y < height; ++y)
	{
		for (unsigned int x = 0; x < width; ++x)
		{
			const size_t output = 3 * (static_cast<size_t>(x) +
				static_cast<size_t>(y) * width);
			const unsigned char *pixel = source +
				static_cast<size_t>(y) * rowPitch + static_cast<size_t>(x) * 4;
			image[output] = gammaLut[pixel[2]];
			image[output + 1] = gammaLut[pixel[1]];
			image[output + 2] = gammaLut[pixel[0]];
		}
	}
	if (capture->format == WW3D::TGA)
	{
		Targa targ;
		memset(&targ.Header, 0, sizeof(targ.Header));
		targ.Header.Width = width;
		targ.Header.Height = height;
		targ.Header.PixelDepth = 24;
		targ.Header.ImageType = TGA_TRUECOLOR;
		targ.SetImage(reinterpret_cast<char *>(image));
		targ.YFlip();
		FileClass *file = _TheWritingFileFactory->Get_File(capture->filename);
		if (file != 0)
		{
			file->Create();
			file->Close();
			_TheWritingFileFactory->Return_File(file);
		}
		targ.Save(capture->filename, TGAF_IMAGE, false);
	}
	else
	{
		BITMAPFILEHEADER fileheader;
		BITMAPINFOHEADER header;
		size_t unpaddedRowBytes = 0;
		size_t rowBytes = 0;
		size_t imageFileBytes = 0;
		const size_t headerBytes = sizeof(BITMAPFILEHEADER) +
			sizeof(BITMAPINFOHEADER);
		if (width > static_cast<unsigned int>(std::numeric_limits<long>::max()) ||
			height > static_cast<unsigned int>(std::numeric_limits<long>::max()) ||
			!Checked_Multiply(static_cast<size_t>(width), 3,
				&unpaddedRowBytes) || unpaddedRowBytes >
				std::numeric_limits<size_t>::max() - 3)
		{
			WWDEBUG_SAY(("Native BMP screenshot dimensions are invalid"));
			delete[] image;
			delete capture;
			return;
		}
		rowBytes = (unpaddedRowBytes + 3) & ~static_cast<size_t>(3);
		if (!Checked_Multiply(rowBytes, static_cast<size_t>(height),
			&imageFileBytes) || imageFileBytes >
			std::numeric_limits<size_t>::max() - headerBytes ||
			headerBytes + imageFileBytes >
			static_cast<size_t>(std::numeric_limits<unsigned long>::max()))
		{
			WWDEBUG_SAY(("Native BMP screenshot file is too large"));
			delete[] image;
			delete capture;
			return;
		}
		memset(&header, 0, sizeof(header));
		header.biSize = sizeof(BITMAPINFOHEADER);
		header.biWidth = static_cast<LONG>(width);
		header.biHeight = static_cast<LONG>(height);
		header.biPlanes = 1;
		header.biBitCount = 24;
		header.biCompression = BI_RGB;
		header.biXPelsPerMeter = 0xB12;
		header.biYPelsPerMeter = 0xB12;
		memset(&fileheader, 0, sizeof(fileheader));
		fileheader.bfType = 19778;
		fileheader.bfOffBits = sizeof(BITMAPFILEHEADER) +
			sizeof(BITMAPINFOHEADER);
		fileheader.bfSize = static_cast<DWORD>(headerBytes + imageFileBytes);
		FileClass *file = _TheWritingFileFactory->Get_File(capture->filename);
		if (file != 0)
		{
			file->Create();
			file->Open(FileClass::WRITE);
			file->Write(&fileheader, sizeof(BITMAPFILEHEADER));
			file->Write(&header, sizeof(BITMAPINFOHEADER));
			char *temp = 0;
			try
			{
				temp = new char[rowBytes];
			}
			catch (...)
			{
				WWDEBUG_SAY(("Native BMP screenshot row allocation failed"));
				file->Close();
				_TheWritingFileFactory->Return_File(file);
				delete[] image;
				delete capture;
				return;
			}
			memset(temp, 0, rowBytes);
			for (unsigned int y = 0; y < height; ++y)
			{
				memcpy(temp, &image[3 * width * (height - y - 1)],
					unpaddedRowBytes);
				for (unsigned int x = 0; x < width; ++x)
				{
					char swap = temp[3 * x];
					temp[3 * x] = temp[3 * x + 2];
					temp[3 * x + 2] = swap;
				}
				file->Write(temp, rowBytes);
			}
			delete[] temp;
			file->Close();
			_TheWritingFileFactory->Return_File(file);
		}
	}
	delete[] image;
	delete capture;
}

void Cancel_GameScreenshot(void *consumer,
	const rts::render::RenderCaptureHandle *, rts::render::RenderResult reason)
{
	delete static_cast<GameScreenshotCapture *>(consumer);
	WWDEBUG_SAY(("Game screenshot capture cancelled: %d",
		static_cast<int>(reason)));
}

void Complete_GameMovie(void *consumer,
	const rts::render::RenderCaptureHandle *, unsigned int width,
	unsigned int height, size_t rowPitch, rts::render::RenderFormat format,
	const void *pixels, size_t pixelBytes)
{
	GameMovieCaptureRequest *request =
		static_cast<GameMovieCaptureRequest *>(consumer);
	if (request == 0)
	{
		return;
	}
	// Detach the request before doing any error handling. Stopping the movie
	// from this owner-thread callback may synchronously cancel the remaining
	// FIFO entries and mutate the request list.
	Remove_GameMovie_Request(request);
	FrameGrabClass *movie = request->movie;
	const bool dimensionsMatch = width == request->width &&
		height == request->height;
	if (movie == 0 || movie != s_gameMovieOwner || pixels == 0 ||
		!dimensionsMatch ||
		format != rts::render::RENDER_FORMAT_B8G8R8A8_UNORM || width == 0 ||
		height == 0)
	{
		WWDEBUG_SAY(("Native movie capture stopped because the visible frame "
			"does not match its AVI dimensions"));
		delete request;
		if (movie == s_gameMovieOwner)
		{
			s_gameMovieStopPending = true;
		}
		return;
	}
	size_t rowBytes = 0;
	size_t requiredBytes = 0;
	size_t outputRowBytes = 0;
	size_t aviRowBytes = 0;
	size_t aviBytes = 0;
	if (!Checked_Multiply(static_cast<size_t>(width), 4, &rowBytes) ||
		rowPitch < rowBytes || !Checked_Multiply(rowPitch,
			static_cast<size_t>(height), &requiredBytes) ||
		pixelBytes < requiredBytes || !Checked_Multiply(
			static_cast<size_t>(width), 3, &outputRowBytes) ||
		outputRowBytes > std::numeric_limits<size_t>::max() - 3)
	{
		WWDEBUG_SAY(("Native movie capture completion had invalid pixels"));
		delete request;
		if (movie == s_gameMovieOwner)
		{
			s_gameMovieStopPending = true;
		}
		return;
	}
	aviRowBytes = (outputRowBytes + 3) & ~static_cast<size_t>(3);
	if (!Checked_Multiply(aviRowBytes, static_cast<size_t>(height),
		&aviBytes) || aviBytes == 0 || movie->GetBuffer() == 0)
	{
		WWDEBUG_SAY(("Native movie capture buffer dimensions are invalid"));
		delete request;
		if (movie == s_gameMovieOwner)
		{
			s_gameMovieStopPending = true;
		}
		return;
	}
	char *image = reinterpret_cast<char *>(movie->GetBuffer());
	const unsigned char *source = static_cast<const unsigned char *>(pixels);
	for (unsigned int y = 0; y < height; ++y)
	{
		const size_t destinationRow = static_cast<size_t>(height - y - 1);
		char *destination = image + destinationRow * aviRowBytes;
		for (unsigned int x = 0; x < width; ++x)
		{
			const unsigned char *pixel = source + static_cast<size_t>(y) *
				rowPitch + static_cast<size_t>(x) * 4;
			destination[static_cast<size_t>(x) * 3] =
				static_cast<char>(pixel[0]);
			destination[static_cast<size_t>(x) * 3 + 1] =
				static_cast<char>(pixel[1]);
			destination[static_cast<size_t>(x) * 3 + 2] =
				static_cast<char>(pixel[2]);
		}
		if (aviRowBytes > outputRowBytes)
		{
			memset(destination + outputRowBytes, 0,
				aviRowBytes - outputRowBytes);
		}
	}
	movie->Grab(image);
	delete request;
}

void Cancel_GameMovie(void *consumer,
	const rts::render::RenderCaptureHandle *, rts::render::RenderResult reason)
{
	GameMovieCaptureRequest *request =
		static_cast<GameMovieCaptureRequest *>(consumer);
	if (request != 0)
	{
		WWDEBUG_SAY(("Native movie capture frame cancelled: %d",
			static_cast<int>(reason)));
		Remove_GameMovie_Request(request);
		if (request->movie == s_gameMovieOwner)
		{
			s_gameMovieStopPending = true;
		}
		delete request;
	}
}
}

namespace
{
WW3DErrorType ToWW3DError(rts::render::RenderResult result)
{
	return result == rts::render::RENDER_RESULT_OK ?
		WW3D_ERROR_OK : WW3D_ERROR_GENERIC;
}

rts::render::GameRenderColor ToGameRenderColor(const Vector3 &color,
	float alpha)
{
	rts::render::GameRenderColor result;
	result.red = color.X;
	result.green = color.Y;
	result.blue = color.Z;
	result.alpha = alpha;
	return result;
}

std::vector<RenderDeviceDescClass> s_gameDeviceDescriptions;
RenderDeviceDescClass s_emptyGameDeviceDescription;

bool PopulateGameDeviceDescription(int deviceidx)
{
	const int deviceCount = rts::render::GetGameRenderDeviceCount();
	if (deviceidx < 0 || deviceidx >= deviceCount)
	{
		return false;
	}
	try
	{
		if (static_cast<size_t>(deviceidx) >= s_gameDeviceDescriptions.size())
		{
			s_gameDeviceDescriptions.resize(static_cast<size_t>(deviceidx) + 1);
		}
	}
	catch (...)
	{
		return false;
	}

	rts::render::GameRenderDeviceDesc gameDescription;
	memset(&gameDescription, 0, sizeof(gameDescription));
	unsigned int resolutionCount = 0;
	rts::render::RenderResult result =
		rts::render::GetGameRenderDeviceDesc(deviceidx, &gameDescription,
			0, 0, &resolutionCount);
	if (result != rts::render::RENDER_RESULT_OK)
	{
		return false;
	}
	// A malformed owner must not make this ABI adapter allocate an unbounded
	// amount of memory. Real owners enumerate at most the display modes they
	// expose; this upper bound only protects the caller-owned bridge buffer.
	if (resolutionCount > 4096)
	{
		return false;
	}
	std::vector<rts::render::GameRenderResolutionDesc> resolutions;
	try
	{
		resolutions.resize(resolutionCount);
	}
	catch (...)
	{
		return false;
	}
	if (resolutionCount != 0)
	{
		result = rts::render::GetGameRenderDeviceDesc(deviceidx,
			&gameDescription, &resolutions[0], resolutionCount,
			&resolutionCount);
		if (result != rts::render::RENDER_RESULT_OK)
		{
			return false;
		}
	}

	RenderDeviceDescClass &description = s_gameDeviceDescriptions[deviceidx];
	description.set_device_name(gameDescription.deviceName);
	description.set_device_vendor(gameDescription.deviceVendor);
	description.set_device_platform(gameDescription.devicePlatform);
	description.set_driver_name(gameDescription.driverName);
	description.set_driver_vendor(gameDescription.driverVendor);
	description.set_driver_version(gameDescription.driverVersion);
	description.set_hardware_name(gameDescription.hardwareName);
	description.set_hardware_vendor(gameDescription.hardwareVendor);
	description.set_hardware_chipset(gameDescription.hardwareChipset);
	description.reset_resolution_list();
	for (unsigned int i = 0; i < resolutionCount; ++i)
	{
		description.add_resolution(resolutions[i].width, resolutions[i].height,
			resolutions[i].bitDepth);
		const DynamicVectorClass<ResolutionDescClass> &stored_resolutions =
			description.Enumerate_Resolutions();
		for (int j = 0; j < stored_resolutions.Count(); ++j)
		{
			const ResolutionDescClass &stored = stored_resolutions[j];
			if (stored.Width == resolutions[i].width &&
				stored.Height == resolutions[i].height &&
				stored.BitDepth == resolutions[i].bitDepth)
			{
				description.set_resolution_refresh_rate(j,
					resolutions[i].refreshRate);
				break;
			}
		}
	}
	return true;
}
}


const char* DAZZLE_INI_FILENAME="DAZZLE.INI";

#define DEFAULT_DEBUG_SHADER_BITS	(		SHADE_CNST(\
												ShaderClass::PASS_LEQUAL,\
												ShaderClass::DEPTH_WRITE_ENABLE,\
												ShaderClass::COLOR_WRITE_ENABLE,\
												ShaderClass::SRCBLEND_ONE,\
												ShaderClass::DSTBLEND_ZERO,\
												ShaderClass::FOG_DISABLE,\
												ShaderClass::GRADIENT_MODULATE,\
												ShaderClass::SECONDARY_GRADIENT_DISABLE,\
												ShaderClass::TEXTURING_DISABLE,\
												ShaderClass::ALPHATEST_DISABLE,\
												ShaderClass::CULL_MODE_ENABLE, \
												ShaderClass::DETAILCOLOR_DISABLE,\
												ShaderClass::DETAILALPHA_DISABLE) )

#define LIGHTMAP_DEBUG_SHADER_BITS	(		SHADE_CNST(\
												ShaderClass::PASS_LEQUAL,\
												ShaderClass::DEPTH_WRITE_ENABLE,\
												ShaderClass::COLOR_WRITE_ENABLE,\
												ShaderClass::SRCBLEND_ONE,\
												ShaderClass::DSTBLEND_ZERO,\
												ShaderClass::FOG_DISABLE,\
												ShaderClass::GRADIENT_DISABLE,\
												ShaderClass::SECONDARY_GRADIENT_DISABLE,\
												ShaderClass::TEXTURING_ENABLE,\
												ShaderClass::ALPHATEST_DISABLE,\
												ShaderClass::CULL_MODE_ENABLE, \
												ShaderClass::DETAILCOLOR_DISABLE,\
												ShaderClass::DETAILALPHA_DISABLE) )



/**********************************************************************************
**
**  WW3D Static Globals
**
***********************************************************************************/

float														WW3D::LogicFrameTimeMs = 1000.0f / WWSyncPerSecond; // initialized to something to avoid division by zero on first use
float															WW3D::FractionalSyncMs = 0.0f;
unsigned int											WW3D::SyncTime = 0;
unsigned int											WW3D::PreviousSyncTime = 0;
bool														WW3D::IsSortingEnabled = true;

float														WW3D::PixelCenterX = 0.0f;
float														WW3D::PixelCenterY = 0.0f;


bool														WW3D::IsInitted = false;
bool														WW3D::IsRendering = false;
bool														WW3D::IsCapturing = false;
bool														WW3D::IsScreenUVBiased = false;

bool														WW3D::AreDecalsEnabled = true;
float														WW3D::DecalRejectionDistance = 1000000.0f;

bool														WW3D::AreStaticSortListsEnabled = false;
bool														WW3D::MungeSortOnLoad = false;

bool														WW3D::OverbrightModifyOnLoad = false;

FrameGrabClass *										WW3D::Movie = nullptr;
bool														WW3D::PauseRecord;
bool														WW3D::RecordNextFrame;

int														WW3D::FrameCount = 0;
long														WW3D::UserStat0 = 0;
long														WW3D::UserStat1 = 0;
long														WW3D::UserStat2 = 0;

float														WW3D::DefaultNativeScreenSize = 1.0f;

StaticSortListClass *								WW3D::DefaultStaticSortLists = nullptr;
StaticSortListClass *								WW3D::CurrentStaticSortLists = nullptr;


VertexMaterialClass *								WW3D::DefaultDebugMaterial  = nullptr;
ShaderClass												WW3D::DefaultDebugShader(DEFAULT_DEBUG_SHADER_BITS);
ShaderClass												WW3D::LightmapDebugShader(LIGHTMAP_DEBUG_SHADER_BITS);

WW3D::PrelitModeEnum									WW3D::PrelitMode = PRELIT_MODE_LIGHTMAP_MULTI_PASS;
bool														WW3D::ExposePrelit = false;

bool														WW3D::SnapshotActivated=false;
bool														WW3D::ThumbnailEnabled=true;

WW3D::MeshDrawModeEnum								WW3D::MeshDrawMode = MESH_DRAW_MODE_OLD;
WW3D::NPatchesGapFillingModeEnum					WW3D::NPatchesGapFillingMode = NPATCHES_GAP_FILLING_ENABLED;
unsigned													WW3D::NPatchesLevel=1;
bool														WW3D::IsTexturingEnabled=true;
bool										WW3D::IsColoringEnabled=false;

static void *												_Hwnd = nullptr;		// Not a member to hide windows from WW3D users
static int												_TextureReduction = 0;
static int												_TextureMinDim = 1;
static bool												_LargeTextureExtraReductionEnabled = false;
int														WW3D::LastFrameMemoryAllocations;
int														WW3D::LastFrameMemoryFrees;

int														WW3D::TextureFilter = TextureFilterClass::TextureFilterMode::TEXTURE_FILTER_BILINEAR;
int														WW3D::AnisotropyLevel = TextureFilterClass::AnisotropicFilterMode::TEXTURE_FILTER_ANISOTROPIC_2X;

bool														WW3D::Lite = false;

/**********************************************************************************
**
**  WW3D Static Functions
**
***********************************************************************************/

void WW3D::Set_NPatches_Gap_Filling_Mode(NPatchesGapFillingModeEnum mode)
{
	if (NPatchesGapFillingMode!=mode) {
		NPatchesGapFillingMode=mode;
		rts::render::InvalidateGameMeshCache();
	}
}

void WW3D::Set_NPatches_Level(unsigned level)
{
	if (level>8) level=8;
	if (level<1) level=1;
	if (NPatchesLevel==1 && level>1) rts::render::InvalidateGameMeshCache();
	if (NPatchesLevel>1 && level==1) rts::render::InvalidateGameMeshCache();
	NPatchesLevel = level;
}

/***********************************************************************************************
 * WW3D::Init -- Initialize the WW3D Library                                                   *
 *                                                                                             *
 * INPUT:                                                                                      *
 *                                                                                             *
 * OUTPUT:                                                                                     *
 *                                                                                             *
 * WARNINGS:                                                                                   *
 *                                                                                             *
 * HISTORY:                                                                                    *
 *   3/24/98    GTH : Created.                                                                 *
 *=============================================================================================*/
WW3DErrorType WW3D::Init(void *hwnd, char *defaultpal, bool lite)
{
	assert(IsInitted == false);
	WWDEBUG_SAY(("WW3D::Init hwnd = %p",hwnd));
	_Hwnd = hwnd;
	Lite = lite;

	/*
	** Initialize the single renderer owner. It also enumerates the available
	** devices and resolutions behind the neutral WW3D contract.
	*/
	const rts::render::RenderResult initResult =
		rts::render::InitializeGameRenderer(_Hwnd, DEFAULT_RESOLUTION_WIDTH,
		DEFAULT_RESOLUTION_HEIGHT, lite, false);
	if (initResult != rts::render::RENDER_RESULT_OK) {
		return ToWW3DError(initResult);
	}
	s_gameDeviceDescriptions.clear();
	WWDEBUG_SAY(("Allocate Debug Resources"));
	Allocate_Debug_Resources();

	MAYBE_UNUSED MMRESULT r=timeBeginPeriod(1);
	WWASSERT(r==TIMERR_NOERROR);
	(void)r;

	/*
	** Initialize the dazzle system
	*/
	if (!lite) {
		WWDEBUG_SAY(("Init Dazzles"));
		FileClass * dazzle_ini_file = _TheFileFactory->Get_File(DAZZLE_INI_FILENAME);
		if (dazzle_ini_file) {
			INIClass dazzle_ini(*dazzle_ini_file);
			DazzleRenderObjClass::Init_From_INI(&dazzle_ini);
			_TheFileFactory->Return_File(dazzle_ini_file);
		}
	}
	/*
	** Initialize the default static sort lists
	** Note that DefaultStaticSortLists[0] is unused.
	*/
	DefaultStaticSortLists = W3DNEW DefaultStaticSortListClass();
	Reset_Current_Static_Sort_Lists_To_Default();

	/*
	** Initialize the animation-triggered sound system
	*/
	if (!lite) {
		AnimatedSoundMgrClass::Initialize ();
		IsInitted = true;
	}
	WWDEBUG_SAY(("WW3D Init completed"));
	return WW3D_ERROR_OK;
}


/***********************************************************************************************
 * WW3D::Shutdown -- shutdown the WW3D Library                                                 *
 *                                                                                             *
 * INPUT:                                                                                      *
 *                                                                                             *
 * OUTPUT:                                                                                     *
 *                                                                                             *
 * WARNINGS:                                                                                   *
 *                                                                                             *
 * HISTORY:                                                                                    *
 *   3/24/98    GTH : Created.                                                                 *
 *=============================================================================================*/
WW3DErrorType WW3D::Shutdown()
{
	assert(Lite || IsInitted == true);
//	WWDEBUG_SAY(("WW3D::Shutdown"));

#ifdef _WIN32
	if (IsCapturing || Movie != nullptr || !s_gameMovieRequests.empty()) {
		Stop_Movie_Capture();
	}
#endif

	//restore the previous timer resolution
	MAYBE_UNUSED MMRESULT r=timeEndPeriod(1);
	WWASSERT(r==TIMERR_NOERROR);
	(void)r;
	/*
	** Free memory in predictive LOD optimizer
	*/
	PredictiveLODOptimizerClass::Free();

	/*
	** Free the DazzleRenderObject class stuff. Whatever it is. ST - 6/11/2001 8:20PM
	*/
	if (!Lite) {
		DazzleRenderObjClass::Deinit ();
	}

	/*
	** Release all of our assets
	*/
	Release_Debug_Resources();
	if (WW3DAssetManager::Get_Instance()) {
		WW3DAssetManager::Get_Instance()->Free_Assets();
	}

	const rts::render::RenderResult rendererResult =
		rts::render::ShutdownGameRenderer();

	/*
	** Clear the default static sort lists
	*/
	delete DefaultStaticSortLists;

	/*
	** Release the animation-triggered sound data
	*/
	AnimatedSoundMgrClass::Shutdown ();

	IsInitted = false;
	return ToWW3DError(rendererResult);
}


/***********************************************************************************************
 * WW3D::Set_Render_Device -- set the render device being currently used                       *
 *                                                                                             *
 * INPUT:                                                                                      *
 *                                                                                             *
 * OUTPUT:                                                                                     *
 *                                                                                             *
 * WARNINGS:                                                                                   *
 *                                                                                             *
 * HISTORY:                                                                                    *
 *   3/24/98    GTH : Created.                                                                 *
 *=============================================================================================*/
WW3DErrorType WW3D::Set_Render_Device( const char * dev_name, int width, int height, int bits, int windowed, bool resize_window )
{
	return ToWW3DError(rts::render::SetGameRenderDeviceByName(dev_name,
		width, height, bits, windowed, resize_window));
}


/***********************************************************************************************
 * WW3D::Set_Any_Render_Device -- set any render device you can find                           *
 *                                                                                             *
 * INPUT:                                                                                      *
 *                                                                                             *
 * OUTPUT:                                                                                     *
 *                                                                                             *
 * WARNINGS:                                                                                   *
 *                                                                                             *
 * HISTORY:                                                                                    *
 *   3/24/98    GTH : Created.                                                                 *
 *=============================================================================================*/
WW3DErrorType WW3D::Set_Any_Render_Device()
{
	return ToWW3DError(rts::render::SetAnyGameRenderDevice());
}


/***********************************************************************************************
 * WW3D::Set_Render_Device -- set the render device being currently used                       *
 *                                                                                             *
 * INPUT:                                                                                      *
 *                                                                                             *
 * OUTPUT:                                                                                     *
 *                                                                                             *
 * WARNINGS:                                                                                   *
 *                                                                                             *
 * HISTORY:                                                                                    *
 *   3/24/98    GTH : Created.                                                                 *
 *=============================================================================================*/
WW3DErrorType WW3D::Set_Render_Device(int dev, int width, int height, int bits, int windowed, bool resize_window, bool reset_device, bool restore_assets )
{
	return ToWW3DError(rts::render::SetGameRenderDeviceByIndex(dev, width,
		height, bits, windowed, resize_window, reset_device, restore_assets));
}


/***********************************************************************************************
 * WW3D::Set_Next_Render_Device -- just go to the next device in the list                      *
 *                                                                                             *
 * INPUT:                                                                                      *
 *                                                                                             *
 * OUTPUT:                                                                                     *
 *                                                                                             *
 * WARNINGS:                                                                                   *
 *                                                                                             *
 * HISTORY:                                                                                    *
 *   3/26/98    GTH : Created.                                                                 *
 *=============================================================================================*/
WW3DErrorType WW3D::Set_Next_Render_Device()
{
	return ToWW3DError(rts::render::SetNextGameRenderDevice());
}

/***********************************************************************************************
 * WW3D::Get_Window -- returns the handle of the render window.										  *
 *                                                                                             *
 * INPUT:                                                                                      *
 *                                                                                             *
 * OUTPUT:                                                                                     *
 *                                                                                             *
 * WARNINGS:                                                                                   *
 *                                                                                             *
 * HISTORY:                                                                                    *
 *   3/28/2001  pds : Created.                                                                 *
 *=============================================================================================*/
void *WW3D::Get_Window()
{
	return _Hwnd;
}

/***********************************************************************************************
 * WW3D::Is_Windowed -- returns whether we are currently in a windowed mode                    *
 *                                                                                             *
 * INPUT:                                                                                      *
 *                                                                                             *
 * OUTPUT:                                                                                     *
 *                                                                                             *
 * WARNINGS:                                                                                   *
 *                                                                                             *
 * HISTORY:                                                                                    *
 *   1/26/2001  gth : Created.                                                                 *
 *=============================================================================================*/
bool WW3D::Is_Windowed()
{
	int width = 0;
	int height = 0;
	int bits = 0;
	bool windowed = false;
	return rts::render::GetGameRendererResolution(&width, &height, &bits,
		&windowed) == rts::render::RENDER_RESULT_OK && windowed;
}

/***********************************************************************************************
 * WW3D::Toggle_Windowed -- Toggle the current render device between	fullscreen and windowed	  *
 *									 mode.  Note:  Its called '_Windowed' to be consistent with the	  *
 *									 other references inside WW3D, a more descriptive name would		  *
 *									 be Toggle_Fullscreen.															  *
 *                                                                                             *
 * INPUT:                                                                                      *
 *                                                                                             *
 * OUTPUT:                                                                                     *
 *                                                                                             *
 * WARNINGS:                                                                                   *
 *                                                                                             *
 * HISTORY:                                                                                    *
 *   1/11/99    PDS : Created.                                                                 *
 *=============================================================================================*/
WW3DErrorType WW3D::Toggle_Windowed ()
{
	return ToWW3DError(rts::render::ToggleGameRendererWindowed());
}


/***********************************************************************************************
 * WW3D::Get_Render_Device -- Get the index of the current render device                       *
 *                                                                                             *
 * INPUT:                                                                                      *
 *                                                                                             *
 * OUTPUT:                                                                                     *
 *                                                                                             *
 * WARNINGS:                                                                                   *
 *                                                                                             *
 * HISTORY:                                                                                    *
 *   3/24/98    GTH : Created.                                                                 *
 *   1/25/2001  gth : converted to the renderer facade                                        *
 *=============================================================================================*/
int WW3D::Get_Render_Device()
{
	return rts::render::GetGameRenderDeviceIndex();
}


/***********************************************************************************************
 * WW3D::Get_Render_Device_Desc -- returns description of the current render device            *
 *                                                                                             *
 * INPUT:                                                                                      *
 *                                                                                             *
 * OUTPUT:                                                                                     *
 *                                                                                             *
 * WARNINGS:                                                                                   *
 *                                                                                             *
 * HISTORY:                                                                                    *
 *   3/26/98    GTH : Created.                                                                 *
 *   1/25/2001  gth : converted to the renderer facade                                        *
 *=============================================================================================*/
const RenderDeviceDescClass & WW3D::Get_Render_Device_Desc(int deviceidx)
{
	if (deviceidx < 0) {
		deviceidx = Get_Render_Device();
	}
	if (!PopulateGameDeviceDescription(deviceidx)) {
		return s_emptyGameDeviceDescription;
	}
	return s_gameDeviceDescriptions[deviceidx];
}



/***********************************************************************************************
 * WW3D::Get_Render_Device_Count -- returns the number of render devices available             *
 *                                                                                             *
 * INPUT:                                                                                      *
 *                                                                                             *
 * OUTPUT:                                                                                     *
 *                                                                                             *
 * WARNINGS:                                                                                   *
 *                                                                                             *
 * HISTORY:                                                                                    *
 *   5/19/99    GTH : Created.                                                                 *
 *   1/25/2001  gth : converted to the renderer facade                                        *
 *=============================================================================================*/
int WW3D::Get_Render_Device_Count()
{
	return rts::render::GetGameRenderDeviceCount();
}


/***********************************************************************************************
 * WW3D::Get_Render_Device_Name -- returns the name of the n-th render device                  *
 *                                                                                             *
 * INPUT:                                                                                      *
 *                                                                                             *
 * OUTPUT:                                                                                     *
 *                                                                                             *
 * WARNINGS:                                                                                   *
 *                                                                                             *
 * HISTORY:                                                                                    *
 *   5/19/99    GTH : Created.                                                                 *
 *   1/25/2001  gth : converted to the renderer facade                                        *
 *=============================================================================================*/
const char * WW3D::Get_Render_Device_Name(int device_index)
{
	return Get_Render_Device_Desc(device_index).Get_Device_Name();
}


/***********************************************************************************************
 * WW3D::Set_Device_Resolution -- set the current resolution and bitdepth                      *
 *                                                                                             *
 * INPUT:                                                                                      *
 *                                                                                             *
 * OUTPUT:                                                                                     *
 *                                                                                             *
 * WARNINGS:                                                                                   *
 *                                                                                             *
 * HISTORY:                                                                                    *
 *   3/24/98    GTH : Created.                                                                 *
 *=============================================================================================*/
WW3DErrorType WW3D::Set_Device_Resolution(int width,int height,int bits,int windowed, bool resize_window)
{
	return ToWW3DError(rts::render::SetGameRendererResolution(width, height,
		bits, windowed, resize_window));
}


/***********************************************************************************************
 * WW3D::Get_Render_Target_Resolution -- get the resolution and bitdepth of the current target *
 *                                                                                             *
 * INPUT:                                                                                      *
 *                                                                                             *
 * OUTPUT:                                                                                     *
 *                                                                                             *
 * WARNINGS:                                                                                   *
 *                                                                                             *
 * HISTORY:                                                                                    *
 *   3/24/98    GTH : Created.                                                                 *
 *   1/25/2001  gth : converted to the renderer facade                                        *
 *=============================================================================================*/
void WW3D::Get_Render_Target_Resolution(int & set_w,int & set_h,int & set_bits,bool & set_windowed)
{
	set_w = set_h = set_bits = 0;
	set_windowed = false;
	rts::render::GetGameRendererTargetResolution(&set_w, &set_h, &set_bits,
		&set_windowed);
}


/***********************************************************************************************
 * WW3D::Get_Device_Resolution -- get the current resolution and bitdepth                      *
 *                                                                                             *
 * INPUT:                                                                                      *
 *                                                                                             *
 * OUTPUT:                                                                                     *
 *                                                                                             *
 * WARNINGS:                                                                                   *
 *                                                                                             *
 * HISTORY:                                                                                    *
 *   3/24/98    GTH : Created.                                                                 *
 *   1/25/2001  gth : converted to the renderer facade                                        *
 *=============================================================================================*/
void WW3D::Get_Device_Resolution(int & set_w,int & set_h,int & set_bits,bool & set_windowed)
{
	set_w = set_h = set_bits = 0;
	set_windowed = false;
	rts::render::GetGameRendererResolution(&set_w, &set_h, &set_bits,
		&set_windowed);
}


/***********************************************************************************************
 * WW3D::Registry_Save_Render_Device -- Saves settings to Registry
 *                                                                                             *
 * INPUT:                                                                                      *
 *                                                                                             *
 * OUTPUT:                                                                                     *
 *                                                                                             *
 * WARNINGS:                                                                                   *
 *                                                                                             *
 * HISTORY:                                                                                    *
 *   12/3/98    BMG : Created.                                                                 *
 *   1/25/2001  gth : converted to the renderer facade                                        *
 *=============================================================================================*/
WW3DErrorType WW3D::Registry_Save_Render_Device( const char * sub_key )
{
	int width = 0;
	int height = 0;
	int depth = 0;
	bool windowed = false;
	Get_Device_Resolution(width, height, depth, windowed);
	return Registry_Save_Render_Device(sub_key, Get_Render_Device(), width,
		height, depth, windowed, Get_Texture_Bitdepth());
}

/***********************************************************************************************
 * WW3D::Registry_Save_Render_Device -- Saves settings to Registry
 *                                                                                             *
 * INPUT:                                                                                      *
 *                                                                                             *
 * OUTPUT:                                                                                     *
 *                                                                                             *
 * WARNINGS:                                                                                   *
 *                                                                                             *
 * HISTORY:                                                                                    *
 *   12/3/98    BMG : Created.                                                                 *
 *=============================================================================================*/
WW3DErrorType WW3D::Registry_Save_Render_Device( const char *sub_key, int device, int width, int height, int depth, bool windowed, int texture_depth )
{
	static const char *valueDeviceName = "RenderDeviceName";
	static const char *valueWidth = "RenderDeviceWidth";
	static const char *valueHeight = "RenderDeviceHeight";
	static const char *valueDepth = "RenderDeviceDepth";
	static const char *valueWindowed = "RenderDeviceWindowed";
	static const char *valueTextureDepth = "RenderDeviceTextureDepth";
	if (sub_key == 0 || device < 0 || device >= Get_Render_Device_Count() ||
		width <= 0 || height <= 0 || depth <= 0 || texture_depth <= 0)
	{
		return WW3D_ERROR_GENERIC;
	}
	const char *deviceName = Get_Render_Device_Name(device);
	if (deviceName == 0 || *deviceName == 0)
	{
		return WW3D_ERROR_GENERIC;
	}
	RegistryClass registry(sub_key);
	if (!registry.Is_Valid())
	{
		WWDEBUG_SAY(("Error getting Registry"));
		return WW3D_ERROR_GENERIC;
	}
	registry.Set_String(valueDeviceName, deviceName);
	registry.Set_Int(valueWidth, width);
	registry.Set_Int(valueHeight, height);
	registry.Set_Int(valueDepth, depth);
	registry.Set_Int(valueWindowed, windowed ? 1 : 0);
	registry.Set_Int(valueTextureDepth, texture_depth);
	return WW3D_ERROR_OK;
}


/***********************************************************************************************
 * WW3D::Registry_Load_Render_Device -- Loads settings from Registry
 *                                                                                             *
 * INPUT:                                                                                      *
 *                                                                                             *
 * OUTPUT:                                                                                     *
 *                                                                                             *
 * WARNINGS:                                                                                   *
 *                                                                                             *
 * HISTORY:                                                                                    *
 *   12/3/98    BMG : Created.                                                                 *
 *=============================================================================================*/
WW3DErrorType WW3D::Registry_Load_Render_Device( const char * sub_key, bool resize_window )
{
	char name[200];
	int width = -1;
	int height = -1;
	int depth = -1;
	int windowed = -1;
	int textureDepth = -1;
	if (Registry_Load_Render_Device(sub_key, name, sizeof(name), width, height,
		depth, windowed, textureDepth) && *name != 0)
	{
		WWDEBUG_SAY(("Device %s (%d X %d) %d bit windowed:%d", name, width,
			height, depth, windowed));
		if (textureDepth == 16 || textureDepth == 32)
		{
			Set_Texture_Bitdepth(textureDepth);
		}
		else
		{
			WWDEBUG_SAY(("Invalid texture depth %d, switching to 16 bits",
				textureDepth));
			Set_Texture_Bitdepth(16);
		}

		if (Set_Render_Device(name, width, height, depth, windowed,
			resize_window) == WW3D_ERROR_OK)
		{
			return WW3D_ERROR_OK;
		}
		depth = depth == 16 ? 32 : 16;
		if (Set_Render_Device(name, width, height, depth, windowed,
			resize_window) == WW3D_ERROR_OK)
		{
			return WW3D_ERROR_OK;
		}
		depth = depth == 16 ? 32 : 16;
		if (width == 640)
		{
			width = 1024;
			height = 768;
		}
		for (;;)
		{
			if (width > 2048) { width = 2048; height = 1536; }
			else if (width > 1920) { width = 1920; height = 1440; }
			else if (width > 1600) { width = 1600; height = 1200; }
			else if (width > 1280) { width = 1280; height = 1024; }
			else if (width > 1024) { width = 1024; height = 768; }
			else if (width > 800) { width = 800; height = 600; }
			else if (width != 640) { width = 640; height = 480; }
			else { return Set_Any_Render_Device(); }
			for (int i = 0; i < 2; ++i)
			{
				if (Set_Render_Device(name, width, height, depth, windowed,
					resize_window) == WW3D_ERROR_OK)
				{
					return WW3D_ERROR_OK;
				}
				depth = depth == 16 ? 32 : 16;
			}
		}
	}
	WWDEBUG_SAY(("Error getting Registry"));
	return Set_Any_Render_Device();
}

bool WW3D::Registry_Load_Render_Device( const char * sub_key, char *device, int device_len, int &width, int &height, int &depth, int &windowed, int &texture_depth)
{
	static const char *valueDeviceName = "RenderDeviceName";
	static const char *valueWidth = "RenderDeviceWidth";
	static const char *valueHeight = "RenderDeviceHeight";
	static const char *valueDepth = "RenderDeviceDepth";
	static const char *valueWindowed = "RenderDeviceWindowed";
	static const char *valueTextureDepth = "RenderDeviceTextureDepth";
	if (device == 0 || device_len <= 0)
	{
		return false;
	}
	*device = 0;
	width = -1;
	height = -1;
	depth = -1;
	windowed = -1;
	texture_depth = -1;
	RegistryClass registry(sub_key);
	if (!registry.Is_Valid())
	{
		return false;
	}
	registry.Get_String(valueDeviceName, device, device_len);
	width = registry.Get_Int(valueWidth, -1);
	height = registry.Get_Int(valueHeight, -1);
	depth = registry.Get_Int(valueDepth, -1);
	windowed = registry.Get_Int(valueWindowed, -1);
	texture_depth = registry.Get_Int(valueTextureDepth, -1);
	return true;
}

void WW3D::_Invalidate_Mesh_Cache()
{
	rts::render::InvalidateGameMeshCache();
}

void WW3D::_Invalidate_Textures()
{
	if (!WW3DAssetManager::Get_Instance()) return;

	TextureLoader::Flush_Pending_Load_Tasks();

	HashTemplateIterator<StringClass,TextureClass*> ite(WW3DAssetManager::Get_Instance()->Texture_Hash());

	// Loop through all the textures in the manager
	for (ite.First();!ite.Is_Done();ite.Next()) {
		// Get the current texture
		TextureClass* tex=ite.Peek_Value();
		tex->Invalidate();
	}
}

void WW3D::Set_Texture_Filter(int texture_filter)
{
	TextureFilter = clamp((int)TextureFilterClass::TEXTURE_FILTER_NONE, texture_filter, (int)TextureFilterClass::TEXTURE_FILTER_ANISOTROPIC);
	TextureFilterClass::_Init_Filters(
		(TextureFilterClass::TextureFilterMode)TextureFilter,
		(TextureFilterClass::AnisotropicFilterMode)AnisotropyLevel
	);
}

void WW3D::Set_Anisotropy_Level(int level)
{
	level = clamp((int)TextureFilterClass::TEXTURE_FILTER_ANISOTROPIC_2X, level, (int)TextureFilterClass::TEXTURE_FILTER_ANISOTROPIC_16X);
	level = highestBit(level);

	AnisotropyLevel = level;
	TextureFilterClass::_Set_Max_Anisotropy((TextureFilterClass::AnisotropicFilterMode)AnisotropyLevel);
}

/***********************************************************************************************
 * WW3D::Begin_Render -- mark the start of rendering for a new frame                           *
 *                                                                                             *
 * INPUT:                                                                                      *
 *                                                                                             *
 * OUTPUT:                                                                                     *
 *                                                                                             *
 * WARNINGS:                                                                                   *
 *                                                                                             *
 * HISTORY:                                                                                    *
 *   3/24/98    GTH : Created.                                                                 *
 *=============================================================================================*/
WW3DErrorType WW3D::Begin_Render(bool clear,bool clearz,const Vector3 & color, float dest_alpha, void(*network_callback)())
{
	if (!IsInitted) {
		return(WW3D_ERROR_OK);
	}

	WWPROFILE("WW3D::Begin_Render");
	WWASSERT(IsInitted);

	SNAPSHOT_SAY(("=========================================="));
	SNAPSHOT_SAY(("========== WW3D::Begin_Render ============"));
	SNAPSHOT_SAY(("==========================================\n"));

	// Memory allocation statistics
	LastFrameMemoryAllocations=WWMemoryLogClass::Get_Allocate_Count();
	LastFrameMemoryFrees=WWMemoryLogClass::Get_Free_Count();
	WWMemoryLogClass::Reset_Counters();

	TextureLoader::Update(network_callback);
//	TextureClass::_Reset_Time_Stamp();
	const rts::render::RenderResult resetResult =
		rts::render::ResetGameRenderFrameResources(true);
	if (resetResult != rts::render::RENDER_RESULT_OK)
	{
		return ToWW3DError(resetResult);
	}

	Debug_Statistics::Begin_Statistics();

	if (IsCapturing && (!PauseRecord || RecordNextFrame)) {
		Update_Movie_Capture();
		RecordNextFrame = false;
	}

	WWASSERT(!IsRendering);
	IsRendering = true;

	rts::render::BeginGameDisplayIteration();
	const rts::render::GameRenderColor gameColor =
		ToGameRenderColor(color, dest_alpha);
	const rts::render::RenderResult beginResult =
		rts::render::BeginGameRender(clear, clearz, gameColor, dest_alpha);
	if (beginResult != rts::render::RENDER_RESULT_OK)
	{
		IsRendering = false;
		return ToWW3DError(beginResult);
	}

	return WW3D_ERROR_OK;
}

/***********************************************************************************************
 * WW3D::Render -- Render a list of layers, starting at the back.                              *
 *                                                                                             *
 * INPUT:                                                                                      *
 *                                                                                             *
 * OUTPUT:                                                                                     *
 *                                                                                             *
 * WARNINGS:                                                                                   *
 *                                                                                             *
 * HISTORY:                                                                                    *
 *   4/2/98    EHC : Created.                                                                  *
 *=============================================================================================*/
WW3DErrorType WW3D::Render(const LayerListClass &LayerList)
{
	if (!IsInitted) {
		return(WW3D_ERROR_OK);
	}

	WWASSERT(IsRendering);

	LayerClass *layer = LayerList.Last();

	while (layer->Is_Valid()) {
		WW3DErrorType result = Render(*layer);

		if (result != WW3D_ERROR_OK) {
			return result;
		}

		layer = layer->Prev();
	}

	return WW3D_ERROR_OK;
}

/***********************************************************************************************
 * WW3D::Render -- Render a Layer                                                              *
 *                                                                                             *
 * INPUT:                                                                                      *
 *                                                                                             *
 * OUTPUT:                                                                                     *
 *                                                                                             *
 * WARNINGS:                                                                                   *
 *                                                                                             *
 * HISTORY:                                                                                    *
 *   4/2/98    EHC : Created.                                                                  *
 *=============================================================================================*/
WW3DErrorType WW3D::Render(const LayerClass &Layer)
{
	if (!IsInitted) {
		return(WW3D_ERROR_OK);
	}

	WWASSERT(IsRendering);
	return Render(Layer.Scene, Layer.Camera, Layer.Clear, Layer.ClearZ, Layer.ClearColor);

}


/***********************************************************************************************
 * WW3D::Render -- Render a 3D Scene using the given camera                                    *
 *                                                                                             *
 * INPUT:                                                                                      *
 *                                                                                             *
 * OUTPUT:                                                                                     *
 *                                                                                             *
 * WARNINGS:                                                                                   *
 *                                                                                             *
 * HISTORY:                                                                                    *
 *   3/24/98    GTH : Created.                                                                 *
 *=============================================================================================*/
WW3DErrorType WW3D::Render(SceneClass * scene,CameraClass * cam,bool clear,bool clearz,const Vector3 & color)
{
	if (!IsInitted) {
		return(WW3D_ERROR_OK);
	}

	WWPROFILE("WW3D::Render");
	WWMEMLOG(MEM_GAMEDATA);
	WWASSERT(IsInitted);
	WWASSERT(IsRendering);
	WWASSERT(scene);
	WWASSERT(cam);

	cam->On_Frame_Update();
	RenderInfoClass rinfo(*cam);

	// Apply the camera and viewport (including depth range)
	cam->Apply();

	// Clear the viewport
	if (clear || clearz) {
		const rts::render::GameRenderColor gameColor =
			ToGameRenderColor(color, 0.0f);
		const rts::render::RenderResult clearResult =
			rts::render::ClearGameRenderTargets(clear, clearz, gameColor, 0.0f);
		if (clearResult != rts::render::RENDER_RESULT_OK)
		{
			return ToWW3DError(clearResult);
		}
	}

	// set the rendering mode
	switch(scene->Get_Polygon_Mode()) {
		case SceneClass::POINT:
			rts::render::SetGameRenderState(
				rts::render::GAME_RENDER_STATE_FILL_MODE,
				rts::render::GAME_RENDER_FILL_POINT);
			break;
		case SceneClass::LINE:
			rts::render::SetGameRenderState(
				rts::render::GAME_RENDER_STATE_FILL_MODE,
				rts::render::GAME_RENDER_FILL_WIREFRAME);
			break;
		case SceneClass::FILL:
			rts::render::SetGameRenderState(
				rts::render::GAME_RENDER_STATE_FILL_MODE,
				rts::render::GAME_RENDER_FILL_SOLID);
			break;
	}

	// Set the global ambient light value here.  If the scene is using the LightEnvironment system
	// this setting will get overridden.
	rts::render::SetGameAmbientColor(ToGameRenderColor(
		scene->Get_Ambient_Light(), 1.0f));

	// render the scene

	rts::render::SetGameRenderCamera(static_cast<void *>(&rinfo.Camera));

	scene->Render(rinfo);

	Flush(rinfo);

	return WW3D_ERROR_OK;
}


/***********************************************************************************************
 * WW3D::Render -- Render a single render object                                               *
 *                                                                                             *
 * INPUT:                                                                                      *
 *                                                                                             *
 * OUTPUT:                                                                                     *
 *                                                                                             *
 * WARNINGS:                                                                                   *
 *                                                                                             *
 * HISTORY:                                                                                    *
 *   4/4/2001   gth : Created.                                                                 *
 *=============================================================================================*/
WW3DErrorType WW3D::Render(
	RenderObjClass & obj,
	RenderInfoClass & rinfo
)
{
	if (!IsInitted) {
		return(WW3D_ERROR_OK);
	}

	WWPROFILE("WW3D::Render");
	WWASSERT(IsInitted);
	WWASSERT(IsRendering);

	{
		WWPROFILE("On_Frame_Update");
		rinfo.Camera.On_Frame_Update();
	}

	// Apply the camera and viewport (including depth range)
	rinfo.Camera.Apply();

	// set the rendering mode
	rts::render::SetGameRenderState(
		rts::render::GAME_RENDER_STATE_FILL_MODE,
		rts::render::GAME_RENDER_FILL_SOLID);

	// Install the lighting environment if one is supplied
	if (rinfo.light_environment != nullptr) {
		rts::render::SetGameLightEnvironment(rinfo.light_environment);
	}

	// Render the object
	rts::render::SetGameRenderCamera(static_cast<void *>(&rinfo.Camera));

	obj.Render(rinfo);

	Flush(rinfo);

	return WW3D_ERROR_OK;
}


/***********************************************************************************************
 * WW3D::Flush -- Process all pending rendering tasks                                          *
 *                                                                                             *
 *    NOTE: This normally happens AUTOMATICALLY. The user should almost *NEVER* have to call   *
 *    this function.  Anyway, this function causes all of the deferred rendering systems to    *
 *    actually perform all of their rendering tasks.  This includes mesh and sort queues.       *
 *    the sorting system.                                                                      *
 *                                                                                             *
 * INPUT:                                                                                      *
 *                                                                                             *
 * OUTPUT:                                                                                     *
 *                                                                                             *
 * WARNINGS:                                                                                   *
 *    Don't call this unless you know what you're doing                                        *
 *                                                                                             *
 * HISTORY:                                                                                    *
 *   4/17/2001  gth : Created.                                                                 *
 * 07/01/02 KM Scalable shader library integration				                               *
 *=============================================================================================*/
void WW3D::Flush(RenderInfoClass & rinfo)
{
	rts::render::FlushGameRenderMeshes();
	SHD_FLUSH;
	WW3D::Render_And_Clear_Static_Sort_Lists(rinfo);	//draws things like water

	rts::render::FlushGameSortedTriangles();
	rts::render::ClearGameRenderMeshPendingDeletes();
}


/***********************************************************************************************
 * WW3D::End_Render -- Mark the completion of a frame                                          *
 *                                                                                             *
 * INPUT:                                                                                      *
 *                                                                                             *
 * OUTPUT:                                                                                     *
 *                                                                                             *
 * WARNINGS:                                                                                   *
 *                                                                                             *
 * HISTORY:                                                                                    *
 *   3/24/98    GTH : Created.                                                                 *
 *=============================================================================================*/
WW3DErrorType WW3D::End_Render(bool flip_frame)
{
	if (!IsInitted) {
		return(WW3D_ERROR_OK);
	}

	WWPROFILE("WW3D::End_Render");

	WWASSERT(IsRendering);
	WWASSERT(IsInitted);

	// If sorting renderer flush isn't called from within any of the render functions
	// the sorting arrays will overflow!

	rts::render::FlushGameSortedTriangles();

	IsRendering = false;

	const rts::render::RenderResult endResult =
		rts::render::EndGameRender(flip_frame);
	// Capture callbacks run from the owner after the capture queue has
	// detached its completed entries. Defer an error-triggered movie stop until
	// the queue has finished invoking every callback, otherwise the remaining
	// callbacks would observe a deleted FrameGrabClass.
	if (s_gameMovieStopPending)
	{
		s_gameMovieStopPending = false;
		Stop_Movie_Capture();
	}

	FrameCount++;

	{
		WWPROFILE("End_Statistics");
		Debug_Statistics::End_Statistics();
	}

	SNAPSHOT_SAY(("=========================================="));
	SNAPSHOT_SAY(("========== WW3D::End_Render =============="));
	SNAPSHOT_SAY(("==========================================\n"));

	Activate_Snapshot(false);

	// (gth) I've found some cases where its not safe to rely on our "shadow" copy (of
	// matrices for example) across multiple frames.  So even though this is slightly
	// less "optimal", lets just reset the caches each frame.
	rts::render::InvalidateGameRenderStateCache();

	return ToWW3DError(endResult);
}


/***********************************************************************************************
 * WW3D::Flip_To_Primary                                                                       *
 *                                                                                             *
 * INPUT:                                                                                      *
 *                                                                                             *
 * OUTPUT:                                                                                     *
 *                                                                                             *
 * WARNINGS:                                                                                   *
 *                                                                                             *
 * HISTORY:                                                                                    *
 *   6/20/01    DEL : Created.                                                                 *
 *=============================================================================================*/
void WW3D::Flip_To_Primary()
{
	rts::render::FlipGameRenderer();
}


/***********************************************************************************************
 * WW3D::Get_Last_Frame_Poly_Count -- returns the number of polys submitted in the previous fr *
 *                                                                                             *
 * INPUT:                                                                                      *
 *                                                                                             *
 * OUTPUT:                                                                                     *
 *                                                                                             *
 * WARNINGS:                                                                                   *
 *                                                                                             *
 * HISTORY:                                                                                    *
 *   7/28/99    GTH : Created.                                                                 *
 *=============================================================================================*/
unsigned int WW3D::Get_Last_Frame_Poly_Count()
{
	return rts::render::GetGameLastFramePolygonCount();
}

unsigned int WW3D::Get_Last_Frame_Vertex_Count()
{
	return rts::render::GetGameLastFrameVertexCount();
}

void WW3D::Update_Logic_Frame_Time(float milliseconds)
{
	LogicFrameTimeMs = milliseconds;
	FractionalSyncMs += milliseconds;
}


/***********************************************************************************************
 * WW3D::Sync -- Time synchronization                                                          *
 *                                                                                             *
 * INPUT:                                                                                      *
 *                                                                                             *
 * OUTPUT:                                                                                     *
 *                                                                                             *
 * WARNINGS:                                                                                   *
 *                                                                                             *
 * HISTORY:                                                                                    *
 *   3/24/98    GTH : Created.                                                                 *
 *=============================================================================================*/
void WW3D::Sync(bool step)
{
	PreviousSyncTime = SyncTime;

	if (step)
	{
		unsigned int integralSyncMs = (unsigned int)FractionalSyncMs;
		FractionalSyncMs -= integralSyncMs;
		SyncTime += integralSyncMs;
	}
	rts::render::SyncGameRenderer(step);
}

/***********************************************************************************************
 * WW3D::Set_Ext_Swap_Interval -- Sets the swap interval the device should aim sync for.       *
 *                                                                                             *
 * INPUT:                                                                                      *
 *                                                                                             *
 * OUTPUT:                                                                                     *
 *                                                                                             *
 * WARNINGS:   Not supported by all rendering devices.                                         *
 *                                                                                             *
 * HISTORY:                                                                                    *
 *   5/07/98    NH : Created.                                                                  *
 *=============================================================================================*/
void WW3D::Set_Ext_Swap_Interval(long swap)
{
	rts::render::SetGameRendererSwapInterval(swap);
}


/***********************************************************************************************
 * WW3D::Get_Ext_Swap_Interval -- Queries the swap interval the device is aiming sync for.     *
 *                                                                                             *
 * INPUT:                                                                                      *
 *                                                                                             *
 * OUTPUT:                                                                                     *
 *                                                                                             *
 * WARNINGS:   Not supported by all rendering devices.                                         *
 *                                                                                             *
 * HISTORY:                                                                                    *
 *   5/07/98    NH : Created.                                                                  *
 *=============================================================================================*/
long WW3D::Get_Ext_Swap_Interval()
{
	return rts::render::GetGameRendererSwapInterval();
}


/***********************************************************************************************
 * WW3D::Set_Collision_Box_Display_Mask -- control rendering of collision boxes                *
 *                                                                                             *
 * INPUT:                                                                                      *
 *                                                                                             *
 * OUTPUT:                                                                                     *
 *                                                                                             *
 * WARNINGS:                                                                                   *
 *                                                                                             *
 * HISTORY:                                                                                    *
 *   3/17/99    GTH : Created.                                                                 *
 *=============================================================================================*/
void WW3D::Set_Collision_Box_Display_Mask(int mask)
{
	BoxRenderObjClass::Set_Box_Display_Mask(mask);
}

/***********************************************************************************************
 * WW3D::Get_Collision_Box_Display_Mask -- returns the current display mask for collision box  *
 *                                                                                             *
 * INPUT:                                                                                      *
 *                                                                                             *
 * OUTPUT:                                                                                     *
 *                                                                                             *
 * WARNINGS:                                                                                   *
 *                                                                                             *
 * HISTORY:                                                                                    *
 *   6/1/99     GTH : Created.                                                                 *
 *=============================================================================================*/
int WW3D::Get_Collision_Box_Display_Mask()
{
	return BoxRenderObjClass::Get_Box_Display_Mask();
}


/***********************************************************************************************
 * WW3D::Normalize_Coordinates -- Convert pixel coords to normalized screen coords 0..1        *
 *                                                                                             *
 *                                                                                             *
 *                                                                                             *
 *                                                                                             *
 * INPUT:                                                                                      *
 *                                                                                             *
 * OUTPUT:                                                                                     *
 *                                                                                             *
 * WARNINGS:                                                                                   *
 *                                                                                             *
 * HISTORY:                                                                                    *
 *   7/27/99    EHC : Created.                                                                 *
 *=============================================================================================*/
void WW3D::Normalize_Coordinates(int x, int y, float &fx, float &fy)
{
	int width = 0;
	int height = 0;
	int bits = 0;
	bool windowed = false;
	if (rts::render::GetGameRendererResolution(&width, &height, &bits,
		&windowed) != rts::render::RENDER_RESULT_OK || width <= 0 ||
		height <= 0)
	{
		fx = 0.0f;
		fy = 0.0f;
		return;
	}
	// clip the coordinates back into the resolution of the screen
	x = Bound(x, 0, width);
	y = Bound(y, 0, height);

	// now that the coordinates are clipped convert them to their normalized values.
	fx = (float)x / width;
	fy = (float)y / height;
}


/***********************************************************************************************
 * WW3D::Make_Screen_Shot -- saves a screenshot with the given base filename                   *
 *                                                                                             *
 * INPUT:                                                                                      *
 *                                                                                             *
 * OUTPUT:                                                                                     *
 *                                                                                             *
 * WARNINGS:                                                                                   *
 *                                                                                             *
 * HISTORY:                                                                                    *
 *   5/19/99    GTH : Created.                                                                 *
 *   2/26/2001  hy : Updated to the renderer facade                                            *
 *=============================================================================================*/
void WW3D::Make_Screen_Shot( const char * filename_base , const float gamma, const ScreenShotFormatEnum format)
{
	WWASSERT(!IsRendering);

	if (filename_base == 0)
	{
		filename_base = "ScreenShot";
	}
	char filename[80];
	char ext[4];
	switch (format) {
		case TGA:
			sprintf(ext, "tga");
			break;
		case BMP:
			sprintf(ext, "bmp");
			break;
		default:
			WWASSERT(0);
			return;
	}

	static int frame_number = 1;
	bool done = false;
	while (!done) {
		snprintf(filename, ARRAY_SIZE(filename), "%s%.2d.%s", filename_base,
			frame_number++, ext);
		FileClass *file = _TheFileFactory->Get_File(filename);
		if (file != 0) {
			file->Open();
			done = !file->Is_Available();
			_TheFileFactory->Return_File(file);
		} else {
			done = true;
		}
	}

	WWDEBUG_SAY(("Creating Screen Shot %s", filename));
	rts::render::RenderBackBufferInfo backBufferInfo;
	const rts::render::RenderResult infoResult =
		rts::render::GetGameBackBufferInfo(&backBufferInfo);
	if (infoResult != rts::render::RENDER_RESULT_OK ||
		backBufferInfo.format != rts::render::RENDER_FORMAT_B8G8R8A8_UNORM ||
		backBufferInfo.width == 0 || backBufferInfo.height == 0 ||
		(format == TGA && (backBufferInfo.width > 65535 ||
			backBufferInfo.height > 65535)))
	{
		WWDEBUG_SAY(("Screenshot back-buffer dimensions are invalid"));
		return;
	}

	GameScreenshotCapture *capture = 0;
	try
	{
		capture = new GameScreenshotCapture;
	}
	catch (...)
	{
		capture = 0;
	}
	if (capture == 0)
	{
		WWDEBUG_SAY(("Screenshot request allocation failed"));
		return;
	}
	strlcpy(capture->filename, filename, ARRAY_SIZE(capture->filename));
	capture->gamma = gamma;
	capture->format = format;
	capture->width = backBufferInfo.width;
	capture->height = backBufferInfo.height;

	rts::render::RenderCaptureRequestDescriptor descriptor;
	descriptor.kind = rts::render::RENDER_CAPTURE_WW3D_SCREENSHOT;
	descriptor.consumer = capture;
	descriptor.completed = Complete_GameScreenshot;
	descriptor.cancelled = Cancel_GameScreenshot;
	rts::render::RenderCaptureHandle handle;
	const rts::render::RenderResult queueResult =
		rts::render::QueueGameBackBufferCapture(descriptor, &handle);
	if (queueResult != rts::render::RENDER_RESULT_OK)
	{
		WWDEBUG_SAY(("Screenshot queue rejected: %d",
			static_cast<int>(queueResult)));
		Cancel_GameScreenshot(capture, &handle, queueResult);
	}
}


/***********************************************************************************************
 * WW3D::Start_Movie_Capture -- begins dumping frames to a movie                               *
 *                                                                                             *
 * INPUT:                                                                                      *
 *                                                                                             *
 * OUTPUT:                                                                                     *
 *                                                                                             *
 * WARNINGS:                                                                                   *
 *                                                                                             *
 * HISTORY:                                                                                    *
 *   5/19/99    GTH : Created.                                                                 *
 *   2/26/2001  hy : updated to the renderer facade                                            *
 *=============================================================================================*/
void WW3D::Start_Movie_Capture( const char * filename_base, float frame_rate )
{
#ifdef _WIN32
	if (IsCapturing || Movie != nullptr || !s_gameMovieRequests.empty()) {
		Stop_Movie_Capture();
	}
	WWASSERT(!IsCapturing);
	if (filename_base == 0)
	{
		filename_base = "Movie";
	}

	rts::render::RenderBackBufferInfo backBufferInfo;
	const rts::render::RenderResult infoResult =
		rts::render::GetGameBackBufferInfo(&backBufferInfo);
	if (infoResult != rts::render::RENDER_RESULT_OK ||
		backBufferInfo.width == 0 || backBufferInfo.height == 0 ||
		backBufferInfo.format != rts::render::RENDER_FORMAT_B8G8R8A8_UNORM ||
		backBufferInfo.width > static_cast<unsigned int>(
			std::numeric_limits<int>::max()) ||
		backBufferInfo.height > static_cast<unsigned int>(
			std::numeric_limits<int>::max()))
	{
		WWDEBUG_SAY(("Movie capture could not query the back buffer"));
		return;
	}
	const int width = static_cast<int>(backBufferInfo.width);
	const int height = static_cast<int>(backBufferInfo.height);
	const int depth = 24;

	IsCapturing = true;
	RecordNextFrame = false;
	WWASSERT(Movie == nullptr);
	if (frame_rate == 0.0f) {
		frame_rate = 1.0f;
		PauseRecord = true;
	} else {
		PauseRecord = false;
	}

	Movie = W3DNEW FrameGrabClass(filename_base, FrameGrabClass::AVI, width,
		height, depth, frame_rate);
	if (Movie == 0 || Movie->GetBuffer() == 0)
	{
		WWDEBUG_SAY(("Movie capture could not allocate its AVI buffer"));
		delete Movie;
		Movie = 0;
		IsCapturing = false;
		RecordNextFrame = false;
		return;
	}
	s_gameMovieOwner = Movie;
	s_gameMovieWidth = backBufferInfo.width;
	s_gameMovieHeight = backBufferInfo.height;
	WWDEBUG_SAY(("Starting Movie %s", filename_base));
#endif
}


/***********************************************************************************************
 * WW3D::Stop_Movie_Capture -- ends dumping frames to a movie                                  *
 *                                                                                             *
 * INPUT:                                                                                      *
 *                                                                                             *
 * OUTPUT:                                                                                     *
 *                                                                                             *
 * WARNINGS:                                                                                   *
 *                                                                                             *
 * HISTORY:                                                                                    *
 *   5/19/99    GTH : Created.                                                                 *
 *=============================================================================================*/
void WW3D::Stop_Movie_Capture()
{
#ifdef _WIN32
	if (IsCapturing || Movie != nullptr || !s_gameMovieRequests.empty()) {
		IsCapturing = false;
		RecordNextFrame = false;
		s_gameMovieStopPending = false;
		WWDEBUG_SAY(("Stopping Movie"));
		while (!s_gameMovieRequests.empty())
		{
			GameMovieCaptureRequest *request = s_gameMovieRequests.front();
			const unsigned int cancelled = rts::render::CancelGameBackBufferCaptures(
				request, rts::render::RENDER_RESULT_FAILED);
			if (cancelled == 0 && Remove_GameMovie_Request(request))
			{
				delete request;
			}
		}
		s_gameMovieOwner = 0;
		s_gameMovieWidth = 0;
		s_gameMovieHeight = 0;
		s_gameMovieStopPending = false;
		delete Movie;
		Movie = 0;
	}
#endif
}


/***********************************************************************************************
 * WW3D::Toggle_Movie_Capture -- toggles movie capture...                                      *
 *                                                                                             *
 * INPUT:                                                                                      *
 *                                                                                             *
 * OUTPUT:                                                                                     *
 *                                                                                             *
 * WARNINGS:                                                                                   *
 *                                                                                             *
 * HISTORY:                                                                                    *
 *   5/19/99    GTH : Created.                                                                 *
 *=============================================================================================*/
void WW3D::Toggle_Movie_Capture( const char * filename_base, float frame_rate )
{
	if (IsCapturing) {
		Stop_Movie_Capture();
	} else {
		Start_Movie_Capture( filename_base, frame_rate);
	}
}


/***********************************************************************************************
 * WW3D::Start_Single_Frame_Movie_Capture -- starts capturing a single frame movie             *
 *                                                                                             *
 * INPUT:                                                                                      *
 *                                                                                             *
 * OUTPUT:                                                                                     *
 *                                                                                             *
 * WARNINGS:                                                                                   *
 *                                                                                             *
 * HISTORY:                                                                                    *
 *   5/19/99    GTH : Created.                                                                 *
 *=============================================================================================*/
void WW3D::Start_Single_Frame_Movie_Capture(const char *filename_base)
{
	Start_Movie_Capture(filename_base, 0.0f);
}


/***********************************************************************************************
 * WW3D::Capture_Next_Movie_Frame -- tells ww3d to grab another frame for the movie            *
 *                                                                                             *
 * INPUT:                                                                                      *
 *                                                                                             *
 * OUTPUT:                                                                                     *
 *                                                                                             *
 * WARNINGS:                                                                                   *
 *                                                                                             *
 * HISTORY:                                                                                    *
 *   5/19/99    GTH : Created.                                                                 *
 *=============================================================================================*/
void WW3D::Capture_Next_Movie_Frame()
{
	RecordNextFrame = true;
}


/***********************************************************************************************
 * WW3D::Pause_Movie -- pauses/unpauses movie capturing                                        *
 *                                                                                             *
 * INPUT:                                                                                      *
 *                                                                                             *
 * OUTPUT:                                                                                     *
 *                                                                                             *
 * WARNINGS:                                                                                   *
 *                                                                                             *
 * HISTORY:                                                                                    *
 *   5/19/99    GTH : Created.                                                                 *
 *=============================================================================================*/
void WW3D::Pause_Movie(bool mode)
{
	PauseRecord = mode;
}


/***********************************************************************************************
 * WW3D::Is_Movie_Paused -- returns whether the movie capture system is paused                 *
 *                                                                                             *
 * INPUT:                                                                                      *
 *                                                                                             *
 * OUTPUT:                                                                                     *
 *                                                                                             *
 * WARNINGS:                                                                                   *
 *                                                                                             *
 * HISTORY:                                                                                    *
 *   5/19/99    GTH : Created.                                                                 *
 *=============================================================================================*/
bool WW3D::Is_Movie_Paused()
{
	return PauseRecord;
}


/***********************************************************************************************
 * WW3D::Is_Recording_Next_Frame -- returns whether the next frame will be dumped to a movie   *
 *                                                                                             *
 * INPUT:                                                                                      *
 *                                                                                             *
 * OUTPUT:                                                                                     *
 *                                                                                             *
 * WARNINGS:                                                                                   *
 *                                                                                             *
 * HISTORY:                                                                                    *
 *   5/19/99    GTH : Created.                                                                 *
 *=============================================================================================*/
bool WW3D::Is_Recording_Next_Frame()
{
	return (Movie != nullptr) && (!PauseRecord || RecordNextFrame);
}


/***********************************************************************************************
 * WW3D::Is_Movie_Ready -- returns whether the movie capture system is ready                   *
 *                                                                                             *
 * INPUT:                                                                                      *
 *                                                                                             *
 * OUTPUT:                                                                                     *
 *                                                                                             *
 * WARNINGS:                                                                                   *
 *                                                                                             *
 * HISTORY:                                                                                    *
 *   5/19/99    GTH : Created.                                                                 *
 *=============================================================================================*/
bool WW3D::Is_Movie_Ready()
{
	return Movie != nullptr;
}


/***********************************************************************************************
 * WW3D::Update_Movie_Capture -- dumps the current frame into the movie                        *
 *                                                                                             *
 * INPUT:                                                                                      *
 *                                                                                             *
 * OUTPUT:                                                                                     *
 *                                                                                             *
 * WARNINGS:                                                                                   *
 *                                                                                             *
 * HISTORY:                                                                                    *
 *   5/19/99    GTH : Created.                                                                 *
 *   2/26/2001  hy : Updated to the renderer facade                                            *
 *=============================================================================================*/
void WW3D::Update_Movie_Capture()
{
#ifdef _WIN32
	WWASSERT(IsCapturing);
	WWPROFILE("WW3D::Update_Movie_Capture");
	WWDEBUG_SAY(("Updating"));

	if (rts::render::IsGameRenderingToTexture())
	{
		// Offscreen passes do not present. Queueing here would accumulate
		// duplicate requests that all consume the next visible back buffer.
		return;
	}
	if (Movie == 0)
	{
		WWDEBUG_SAY(("Movie capture has no movie consumer"));
		return;
	}
	if (s_gameMovieOwner != Movie || s_gameMovieWidth == 0 ||
		s_gameMovieHeight == 0)
	{
		WWDEBUG_SAY(("Movie capture has no stable frame dimensions"));
		Stop_Movie_Capture();
		return;
	}

	GameMovieCaptureRequest *request = 0;
	try
	{
		request = new GameMovieCaptureRequest;
	}
	catch (...)
	{
		request = 0;
	}
	if (request == 0)
	{
		WWDEBUG_SAY(("Movie capture request allocation failed"));
		Stop_Movie_Capture();
		return;
	}
	request->movie = Movie;
	request->width = s_gameMovieWidth;
	request->height = s_gameMovieHeight;
	try
	{
		s_gameMovieRequests.push_back(request);
	}
	catch (...)
	{
		delete request;
		WWDEBUG_SAY(("Movie capture request list allocation failed"));
		Stop_Movie_Capture();
		return;
	}

	rts::render::RenderCaptureRequestDescriptor descriptor;
	descriptor.kind = rts::render::RENDER_CAPTURE_MOVIE;
	descriptor.consumer = request;
	descriptor.completed = Complete_GameMovie;
	descriptor.cancelled = Cancel_GameMovie;
	rts::render::RenderCaptureHandle handle;
	const rts::render::RenderResult queueResult =
		rts::render::QueueGameBackBufferCapture(descriptor, &handle);
	if (queueResult != rts::render::RENDER_RESULT_OK)
	{
		WWDEBUG_SAY(("Movie capture queue rejected: %d",
			static_cast<int>(queueResult)));
		if (Remove_GameMovie_Request(request))
		{
			delete request;
		}
		Stop_Movie_Capture();
	}
#endif
}


/***********************************************************************************************
 * WW3D::Get_Movie_Capture_Frame_Rate -- returns the framerate at which the movie is being cap *
 *                                                                                             *
 * INPUT:                                                                                      *
 *                                                                                             *
 * OUTPUT:                                                                                     *
 *                                                                                             *
 * WARNINGS:                                                                                   *
 *                                                                                             *
 * HISTORY:                                                                                    *
 *   5/19/99    GTH : Created.                                                                 *
 *=============================================================================================*/
float	WW3D::Get_Movie_Capture_Frame_Rate()
{
#ifdef _WIN32
	if (IsCapturing) {
		return Movie->GetFrameRate();
	}
#endif
	return 0;
}


/***********************************************************************************************
 * WW3D::Set_Texture_Reduction -- sets the (hacky) texture reduction factor                    *
 *                                                                                             *
 * INPUT:                                                                                      *
 *                                                                                             *
 * OUTPUT:                                                                                     *
 *                                                                                             *
 * WARNINGS:                                                                                   *
 *                                                                                             *
 * HISTORY:                                                                                    *
 *   5/19/99    GTH : Created.                                                                 *
 *=============================================================================================*/
void	WW3D::Set_Texture_Reduction( int value, int minDim )
{
	if (_TextureReduction != value || _TextureMinDim != minDim) {
		_TextureReduction=value;
		_TextureMinDim=minDim;
		_Invalidate_Textures();
	}
}


void WW3D::Enable_Texturing(bool b)
{
	if (b==IsTexturingEnabled) return;
	IsTexturingEnabled=b;
//	_Invalidate_Textures();
}

void WW3D::Enable_Coloring(unsigned int color)
{
	IsColoringEnabled = (color == 0) ? false : true;
}

/***********************************************************************************************
 * WW3D::Get_Texture_Reduction -- gets the (hacky) texture reduction factor                    *
 *                                                                                             *
 * INPUT:                                                                                      *
 *                                                                                             *
 * OUTPUT:                                                                                     *
 *                                                                                             *
 * WARNINGS:                                                                                   *
 *                                                                                             *
 * HISTORY:                                                                                    *
 *   11/25/99    TSS : Created.                                                                 *
 *=============================================================================================*/
int	WW3D::Get_Texture_Reduction()
{
	return _TextureReduction;
}

/***********************************************************************************************
 * WW3D::Get_Texture_Min_Mip_Levels -- gets the minimum number of mip levels permitted		   *
 *                                                                                             *
 * INPUT:                                                                                      *
 *                                                                                             *
 * OUTPUT:                                                                                     *
 *                                                                                             *
 * WARNINGS:                                                                                   *
 *                                                                                             *
 * HISTORY:                                                                                    *
 *   11/25/99    TSS : Created.                                                                 *
 *=============================================================================================*/
int	WW3D::Get_Texture_Min_Dimension()
{
	return _TextureMinDim;
}

void WW3D::Enable_Large_Texture_Extra_Reduction(bool onoff)
{
	if (_LargeTextureExtraReductionEnabled != onoff) {
		_LargeTextureExtraReductionEnabled = onoff;
		_Invalidate_Textures();
	}
}

bool WW3D::Is_Large_Texture_Extra_Reduction_Enabled()
{
	return _LargeTextureExtraReductionEnabled;
}

/***********************************************************************************************
 * WW3D::Peek_Default_Debug_Material -- returns a pointer to the default debug mtl				  *
 *                                                                                             *
 * INPUT:                                                                                      *
 *                                                                                             *
 * OUTPUT:                                                                                     *
 *                                                                                             *
 * WARNINGS:                                                                                   *
 *                                                                                             *
 * HISTORY:                                                                                    *
 *   7/21/99    GTH : Created.                                                                 *
 *=============================================================================================*/
VertexMaterialClass * WW3D::Peek_Default_Debug_Material()
{
#ifdef WWDEBUG
	WWASSERT(DefaultDebugMaterial);
	return DefaultDebugMaterial;
#else
	return nullptr;
#endif
}

/***********************************************************************************************
 * WW3D::Peek_Default_Debug_Shader -- returns the default shader for debugging.	              *
 *                                                                                             *
 * INPUT:                                                                                      *
 *                                                                                             *
 * OUTPUT:                                                                                     *
 *                                                                                             *
 * WARNINGS:                                                                                   *
 *                                                                                             *
 * HISTORY:                                                                                    *
 *   7/21/99    GTH : Created.                                                                 *
 *=============================================================================================*/
ShaderClass	WW3D::Peek_Default_Debug_Shader()
{
	return DefaultDebugShader;
}

/***********************************************************************************************
 * WW3D::Peek_Lightmap_Debug_Shader -- returns the shader for lightmap debugging.              *
 *                                                                                             *
 * INPUT:                                                                                      *
 *                                                                                             *
 * OUTPUT:                                                                                     *
 *                                                                                             *
 * WARNINGS:                                                                                   *
 *                                                                                             *
 * HISTORY:                                                                                    *
 *   7/21/99    GTH : Created.                                                                 *
 *=============================================================================================*/
ShaderClass	WW3D::Peek_Lightmap_Debug_Shader()
{
	return LightmapDebugShader;
}

/***********************************************************************************************
 * WW3D::Allocate_Debug_Resources -- allocates the debug resources									  *
 *                                                                                             *
 * INPUT:                                                                                      *
 *                                                                                             *
 * OUTPUT:                                                                                     *
 *                                                                                             *
 * WARNINGS:                                                                                   *
 *                                                                                             *
 * HISTORY:                                                                                    *
 *   7/21/99    GTH : Created.                                                                 *
 *=============================================================================================*/
void WW3D::Allocate_Debug_Resources()
{
#ifdef WWDEBUG
	WWASSERT(DefaultDebugMaterial == nullptr);
	DefaultDebugMaterial = W3DNEW VertexMaterialClass;
	DefaultDebugMaterial->Set_Shininess(0.0f);
	DefaultDebugMaterial->Set_Opacity(1.0f);
	DefaultDebugMaterial->Set_Ambient(0,0,0);
	DefaultDebugMaterial->Set_Diffuse(0,0,0);
	DefaultDebugMaterial->Set_Specular(0,0,0);
	DefaultDebugMaterial->Set_Emissive(0,0,0);
#endif
}

/***********************************************************************************************
 * WW3D::Release_Debug_Resources -- releases the debug resources										  *
 *                                                                                             *
 * INPUT:                                                                                      *
 *                                                                                             *
 * OUTPUT:                                                                                     *
 *                                                                                             *
 * WARNINGS:                                                                                   *
 *                                                                                             *
 * HISTORY:                                                                                    *
 *   7/21/99    GTH : Created.                                                                 *
 *=============================================================================================*/
void WW3D::Release_Debug_Resources()
{
#ifdef WWDEBUG
	WWASSERT(DefaultDebugMaterial);
	REF_PTR_RELEASE(DefaultDebugMaterial);
#endif
}


WW3DErrorType WW3D::On_Deactivate_App()
{
	_Invalidate_Textures();
	_Invalidate_Mesh_Cache();

	return WW3D_ERROR_OK;
}


WW3DErrorType WW3D::On_Activate_App()
{
	return WW3D_ERROR_OK;
}


void WW3D::Get_Pixel_Center(float &x, float &y)
{
	x = PixelCenterX; y = PixelCenterY;
}


void WW3D::Update_Pixel_Center()
{
	char name[rts::render::GAME_RENDER_DEVICE_STRING_CAPACITY];
	memset(name, 0, sizeof(name));
	const rts::render::RenderResult result =
		rts::render::GetGameRenderDeviceName(
			rts::render::GetGameRenderDeviceIndex(), name, sizeof(name));
	if (result == rts::render::RENDER_RESULT_OK &&
		(strstr(name, "Direct") != 0 || strstr(name, "D3D") != 0 ||
		 strstr(name, "Native") != 0))
	{
		PixelCenterX = 0.5f;
		PixelCenterY = 0.5f;
	}
	else
	{
		PixelCenterX = 0.0f;
		PixelCenterY = 0.0f;
	}
}

void WW3D::Set_Texture_Bitdepth(int bitdepth)
{
	if (bitdepth != 16 && bitdepth != 32)
	{
		return;
	}
	rts::render::SetGameTextureBitdepth(bitdepth);
}

int WW3D::Get_Texture_Bitdepth()
{
	return rts::render::GetGameTextureBitdepth();
}

void WW3D::Set_MSAA_Mode(MultiSampleModeEnum mode)
{
	switch (mode) {

	default:
	case MULTISAMPLE_MODE_NONE:
		rts::render::SetGameMSAAMode(
			rts::render::GAME_RENDER_MULTISAMPLE_NONE);
		break;

	case MULTISAMPLE_MODE_2X:
		rts::render::SetGameMSAAMode(
			rts::render::GAME_RENDER_MULTISAMPLE_2X);
		break;

	case MULTISAMPLE_MODE_4X:
		rts::render::SetGameMSAAMode(
			rts::render::GAME_RENDER_MULTISAMPLE_4X);
		break;

	case MULTISAMPLE_MODE_8X:
		rts::render::SetGameMSAAMode(
			rts::render::GAME_RENDER_MULTISAMPLE_8X);
		break;

	}
}

WW3D::MultiSampleModeEnum WW3D::Get_MSAA_Mode()
{
	const unsigned int type = rts::render::GetGameMSAAMode();

	switch (type) {

	default:
	case rts::render::GAME_RENDER_MULTISAMPLE_NONE:
		return MULTISAMPLE_MODE_NONE;

	case rts::render::GAME_RENDER_MULTISAMPLE_2X:
		return MULTISAMPLE_MODE_2X;

	case rts::render::GAME_RENDER_MULTISAMPLE_4X:
		return MULTISAMPLE_MODE_4X;

	case rts::render::GAME_RENDER_MULTISAMPLE_8X:
		return MULTISAMPLE_MODE_8X;

	}
}

void WW3D::Add_To_Static_Sort_List(RenderObjClass *robj, unsigned int sort_level)
{
	CurrentStaticSortLists->Add_To_List(robj, sort_level);
}

void WW3D::Render_And_Clear_Static_Sort_Lists(RenderInfoClass & rinfo)
{
	// The ststic sort lists need to be disabled while we are rendering from them otherwise the
	// Render() function will just dump the objects right back on the same lists.
	bool old_enable = AreStaticSortListsEnabled;
	AreStaticSortListsEnabled = false;
	CurrentStaticSortLists->Render_And_Clear(rinfo);
	AreStaticSortListsEnabled = old_enable;
}

void WW3D::Enable_Sorting(bool onoff)
{
	IsSortingEnabled = onoff;
	// Have to invalidate mesh rendering system because
	// meshes are put into different fvfs depending on their sort state
	rts::render::InvalidateGameMeshCache();
}

void WW3D::Override_Current_Static_Sort_Lists(StaticSortListClass * sort_list)
{
	if (sort_list) {
		CurrentStaticSortLists = sort_list;
	} else {
		WWASSERT(sort_list);
	}
}


void WW3D::Reset_Current_Static_Sort_Lists_To_Default()
{
	CurrentStaticSortLists = DefaultStaticSortLists;
}

void WW3D::Set_Gamma(float gamma,float bright,float contrast,bool calibrate)
{
	rts::render::SetGameGamma(gamma, bright, contrast, calibrate);
}
