/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 TheSuperHackers
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

#include "W3DDevice/GameClient/W3DScreenshot.h"
#include "W3DDevice/GameClient/W3DScreenshotCodec.h"
#include "Common/GlobalData.h"
#include "GameClient/GameText.h"
#include "GameClient/InGameUI.h"
#include "Lib/JobSystem.h"
#include "WW3D2/dx8wrapper.h"
#include "WW3D2/ww3d.h"
#include "WW3D2/surfaceclass.h"
#include "WWLib/mpsc_intrusive_queue.h"
#include "rts/profile.h"
#include <stb_image_write.h>
#include <limits.h>
#include <string>

static bool checkedScreenshotMultiply(size_t left, size_t right, size_t* result)
{
	if (result == 0 || (left != 0 && right > (size_t)-1 / left))
	{
		return false;
	}
	*result = left * right;
	return true;
}

struct ScreenshotWrittenMessage
{
	ScreenshotWrittenMessage* next;
	char leafname[_MAX_FNAME];
};
static MPSCIntrusiveQueue<ScreenshotWrittenMessage> s_screenshotWrittenQueue;

static void deleteScreenshotWrittenMessages(ScreenshotWrittenMessage* message)
{
	while (message != 0)
	{
		ScreenshotWrittenMessage* next = message->next;
		delete message;
		message = next;
	}
}

// VC6 has no nothrow overload for array new. Keep this allocation local so the
// owner-thread capture path can report allocation failure on every supported
// toolchain without introducing a global operator-new overload.
static unsigned char* allocateScreenshotBuffer(size_t size)
{
	try
	{
		return new unsigned char[size];
	}
	catch (...)
	{
		return 0;
	}
}

class ScreenshotBatch
{
public:
	ScreenshotBatch(unsigned char* pixelData, unsigned char* image,
		ScreenshotWrittenMessage* completion, unsigned width, unsigned height,
		unsigned pitch, ScreenshotSourceFormat sourceFormat, const char* outputDirectory,
		const char* outputPath, const char* leafname, int quality, ScreenshotFormat format)
		: m_pixelData(pixelData),
		  m_image(image),
		  m_completion(completion),
		  m_quality(quality),
		  m_format(format),
		  m_remainingTasks(0),
		  m_failed(0)
	{
		m_source.pixels = pixelData;
		m_source.width = width;
		m_source.height = height;
		m_source.pitch = pitch;
		m_source.format = sourceFormat;
		strlcpy(m_outputDirectory, outputDirectory, ARRAY_SIZE(m_outputDirectory));
		strlcpy(m_outputPath, outputPath, ARRAY_SIZE(m_outputPath));
		strlcpy(m_leafname, leafname, ARRAY_SIZE(m_leafname));
		strlcpy(m_completion->leafname, leafname, ARRAY_SIZE(m_completion->leafname));
	}

	~ScreenshotBatch()
	{
		delete[] m_pixelData;
		delete[] m_image;
		delete m_completion;
	}

	void setTaskCount(unsigned taskCount)
	{
		m_remainingTasks = (LONG)taskCount;
		m_failed = 0;
	}

	void convert(unsigned yBegin, unsigned yEnd)
	{
		bool converted = true;
		try
		{
			PROFILER_SECTION_NAME("Screenshot.Convert");
			ConvertScreenshotRows(m_source, yBegin, yEnd, m_image);
		}
		catch (...)
		{
			converted = false;
		}

		if (!converted)
		{
#if defined(_WIN32) && defined(_MSC_VER) && _MSC_VER < 1300
			InterlockedExchange(const_cast<LONG *>(&m_failed), 1);
#else
			InterlockedExchange(&m_failed, 1);
#endif
		}

		finish();
	}

	private:
	void finish()
	{
		if (InterlockedDecrement(&m_remainingTasks) == 0)
		{
			if (m_failed == 0)
			{
				try
				{
					encodeAndReport();
				}
				catch (...)
				{
					DEBUG_LOG(("Failed to encode screenshot %s", m_outputPath));
				}
			}
			delete this;
		}
	}

	public:
	const char* leafname() const
	{
		return m_leafname;
	}

	unsigned height() const
	{
		return m_source.height;
	}

private:
	void encodeAndReport()
	{
		int success = 0;

		{
			PROFILER_SECTION_NAME("Screenshot.Encode");
			CreateDirectory(m_outputDirectory, 0);

			switch (m_format)
			{
				case SCREENSHOT_JPEG:
					success = stbi_write_jpg(m_outputPath, (int)m_source.width, (int)m_source.height,
						3, m_image, m_quality);
					break;
				case SCREENSHOT_PNG:
					success = stbi_write_png(m_outputPath, (int)m_source.width, (int)m_source.height,
						3, m_image, (int)(m_source.width * 3));
					break;
			}
		}

		if (success)
		{
			s_screenshotWrittenQueue.Push(m_completion);
			m_completion = 0;
		}
		else
		{
			DEBUG_LOG(("Failed to write screenshot %s", m_outputPath));
		}
	}
	unsigned char* m_pixelData;
	unsigned char* m_image;
	ScreenshotWrittenMessage* m_completion;
	ScreenshotPixelSource m_source;
	char m_outputDirectory[_MAX_PATH];
	char m_outputPath[_MAX_PATH];
	char m_leafname[_MAX_FNAME];
	int m_quality;
	ScreenshotFormat m_format;
	LONG m_remainingTasks;
	volatile LONG m_failed;
};

class ScreenshotConvertTask : public rts::Job
{
public:
	ScreenshotConvertTask(ScreenshotBatch* batch, unsigned yBegin, unsigned yEnd)
		: m_batch(batch), m_yBegin(yBegin), m_yEnd(yEnd)
	{
	}

	virtual void execute(rts::JobContext &)
	{
		m_batch->convert(m_yBegin, m_yEnd);
	}

private:
	ScreenshotBatch* m_batch;
	unsigned m_yBegin;
	unsigned m_yEnd;
};

class ScreenshotTaskService
{
public:
	void submit(ScreenshotBatch* batch)
	{
		rts::JobSystem& system = rts::JobSystem::instance();
		if (!system.ensureStarted())
		{
			system.recordSerialFallback();
			batch->setTaskCount(1);
			batch->convert(0, batch->height());
			return;
		}
		if (!m_group.isValid())
		{
			m_group = system.createGroup();
			if (!m_group.isValid())
			{
				system.recordSerialFallback();
				batch->setTaskCount(1);
				batch->convert(0, batch->height());
				return;
			}
		}

		const unsigned workerCount = system.workerCount();
		unsigned rangeCapacity = workerCount <= UINT_MAX / 2 ?
			workerCount * 2 : workerCount;
		if (rangeCapacity == 0)
		{
			rangeCapacity = 1;
		}
		ScreenshotRowRange* ranges = 0;
		ScreenshotConvertTask** tasks = 0;
		rts::JobSubmission* submissions = 0;
		rts::JobHandle* handles = 0;
		try
		{
			ranges = new ScreenshotRowRange[rangeCapacity];
			tasks = new ScreenshotConvertTask*[rangeCapacity];
			submissions = new rts::JobSubmission[rangeCapacity];
			handles = new rts::JobHandle[rangeCapacity];
		}
		catch (...)
		{
			delete[] handles;
			delete[] submissions;
			delete[] tasks;
			delete[] ranges;
			system.recordSerialFallback();
			batch->setTaskCount(1);
			batch->convert(0, batch->height());
			return;
		}
		const unsigned taskCount = BuildScreenshotRowRanges(batch->height(),
			workerCount, ranges, rangeCapacity);
		unsigned index;

		for (index = 0; index < taskCount; ++index)
		{
			try
			{
				tasks[index] = new ScreenshotConvertTask(batch,
					ranges[index].yBegin, ranges[index].yEnd);
			}
			catch (...)
			{
				tasks[index] = 0;
			}
			if (tasks[index] == 0)
			{
				while (index > 0)
				{
					delete tasks[--index];
				}
				delete[] handles;
				delete[] submissions;
				delete[] tasks;
				delete[] ranges;
				system.recordSerialFallback();
				batch->setTaskCount(1);
				batch->convert(0, batch->height());
				return;
			}
			submissions[index].job = tasks[index];
			submissions[index].priority = rts::JOB_PRIORITY_BACKGROUND;
		}

		batch->setTaskCount(taskCount);
		if (taskCount == 0 || !system.trySubmitBatch(submissions, taskCount,
			m_group, handles))
		{
			for (index = 0; index < taskCount; ++index)
			{
				delete tasks[index];
			}
			system.recordSerialFallback();
			batch->setTaskCount(1);
			batch->convert(0, batch->height());
		}
		delete[] handles;
		delete[] submissions;
		delete[] tasks;
		delete[] ranges;
	}

	void shutdown()
	{
		if (m_group.isValid())
		{
			rts::JobSystem::instance().wait(m_group);
			m_group = rts::JobGroup();
		}
	}

private:
	rts::JobGroup m_group;
};

static ScreenshotTaskService s_screenshotTaskService;

// Requests are created on the render owner, while encoding is asynchronous.
// Keep process-local reservations so two requests from the same millisecond
// cannot encode to the same path. Reservations intentionally live for the
// process lifetime; this keeps a cancelled request from reusing a path that a
// worker may already have observed.
static std::vector<std::string> s_reservedD3D11ScreenshotPaths;

static bool reserveD3D11ScreenshotName(const char *outputDirectory,
	const char *initialLeafname, char *leafname, size_t leafnameCapacity,
	char *outputPath, size_t outputPathCapacity)
{
	if (outputDirectory == 0 || initialLeafname == 0 || leafname == 0 ||
		outputPath == 0 || leafnameCapacity == 0 || outputPathCapacity == 0)
	{
		return false;
	}
	char baseLeafname[_MAX_FNAME];
	strlcpy(baseLeafname, initialLeafname, ARRAY_SIZE(baseLeafname));
	const char *extension = strrchr(baseLeafname, '.');
	const size_t stemLength = extension == 0 ? strlen(baseLeafname) :
		static_cast<size_t>(extension - baseLeafname);
	for (unsigned int suffix = 0; suffix < 1000000; ++suffix)
	{
		if (suffix == 0)
		{
			strlcpy(leafname, baseLeafname, leafnameCapacity);
		}
		else
		{
			snprintf(leafname, leafnameCapacity, "%.*s_%u%s",
				static_cast<int>(stemLength), baseLeafname, suffix,
				extension == 0 ? "" : extension);
		}
		strlcpy(outputPath, outputDirectory, outputPathCapacity);
		strlcat(outputPath, leafname, outputPathCapacity);
		bool reserved = false;
		for (size_t index = 0; index < s_reservedD3D11ScreenshotPaths.size();
			++index)
		{
			if (s_reservedD3D11ScreenshotPaths[index] == outputPath)
			{
				reserved = true;
				break;
			}
		}
		if (reserved)
		{
			continue;
		}
		try
		{
			s_reservedD3D11ScreenshotPaths.push_back(outputPath);
		}
		catch (...)
		{
			return false;
		}
		return true;
	}
	return false;
}

struct D3D11CompressedScreenshotCapture
{
	unsigned char* pixelData;
	unsigned char* image;
	ScreenshotWrittenMessage* completion;
	unsigned width;
	unsigned height;
	unsigned pitch;
	char outputDirectory[_MAX_PATH];
	char outputPath[_MAX_PATH];
	char leafname[_MAX_FNAME];
	int quality;
	ScreenshotFormat format;
};

static void completeD3D11CompressedScreenshot(void *consumer,
	const rts::render::RenderCaptureHandle *, unsigned width, unsigned height,
	size_t rowPitch, rts::render::RenderFormat captureFormat,
	const void *pixels, size_t pixelBytes)
{
	D3D11CompressedScreenshotCapture* capture =
		static_cast<D3D11CompressedScreenshotCapture *>(consumer);
	if (capture == 0)
	{
		return;
	}
	size_t requiredRowBytes = 0;
	size_t requiredBytes = 0;
	if (pixels == 0 || captureFormat !=
		rts::render::RENDER_FORMAT_B8G8R8A8_UNORM ||
		width != capture->width || height != capture->height ||
		!checkedScreenshotMultiply(static_cast<size_t>(width), 4,
			&requiredRowBytes) || rowPitch < requiredRowBytes ||
		!checkedScreenshotMultiply(rowPitch, static_cast<size_t>(height),
			&requiredBytes) || pixelBytes < requiredBytes)
	{
		DEBUG_LOG(("D3D11 compressed screenshot completion had invalid pixels"));
		delete[] capture->pixelData;
		delete[] capture->image;
		delete capture->completion;
		delete capture;
		return;
	}
	const unsigned char *source = static_cast<const unsigned char *>(pixels);
	for (unsigned row = 0; row < height; ++row)
	{
		memcpy(capture->pixelData + static_cast<size_t>(row) * capture->pitch,
			source + static_cast<size_t>(row) * rowPitch, capture->pitch);
	}
	ScreenshotBatch* batch = 0;
	try
	{
		batch = new ScreenshotBatch(capture->pixelData, capture->image,
			capture->completion, capture->width, capture->height, capture->pitch,
			SCREENSHOT_SOURCE_ARGB32, capture->outputDirectory,
			capture->outputPath, capture->leafname, capture->quality,
			capture->format);
	}
	catch (...)
	{
		batch = 0;
	}
	if (batch == 0)
	{
		DEBUG_LOG(("Dropped D3D11 screenshot %s because its batch could not be allocated",
			capture->leafname));
		delete[] capture->pixelData;
		delete[] capture->image;
		delete capture->completion;
		delete capture;
		return;
	}
	capture->pixelData = 0;
	capture->image = 0;
	capture->completion = 0;
	s_screenshotTaskService.submit(batch);
	delete capture;
}

static void cancelD3D11CompressedScreenshot(void *consumer,
	const rts::render::RenderCaptureHandle *, rts::render::RenderResult reason)
{
	D3D11CompressedScreenshotCapture* capture =
		static_cast<D3D11CompressedScreenshotCapture *>(consumer);
	if (capture != 0)
	{
		DEBUG_LOG(("D3D11 compressed screenshot capture cancelled: %d",
			static_cast<int>(reason)));
		delete[] capture->pixelData;
		delete[] capture->image;
		delete capture->completion;
		delete capture;
	}
}

void W3D_UpdateScreenshotMessages()
{
	ScreenshotWrittenMessage* message = s_screenshotWrittenQueue.Flush();
	if (TheInGameUI == 0)
	{
		deleteScreenshotWrittenMessages(message);
		return;
	}

	while (message != 0)
	{
		UnicodeString ufileName;
		ufileName.translate(message->leafname);
		TheInGameUI->message(TheGameText->fetch("GUI:ScreenCapture"), ufileName.str());
		ScreenshotWrittenMessage* next = message->next;
		delete message;
		message = next;
	}
}

void W3D_ShutdownScreenshotTasks()
{
	s_screenshotTaskService.shutdown();
	deleteScreenshotWrittenMessages(s_screenshotWrittenQueue.Flush());
}

void W3D_TakeCompressedScreenshot(ScreenshotFormat format, Int jpegQuality)
{
	static constexpr const char* const ScreenshotFormatExtensions[] = { "jpg", "png" };
	static_assert(ARRAY_SIZE(ScreenshotFormatExtensions) == SCREENSHOT_FORMAT_COUNT, "Incorrect array size");

	if ((unsigned)format >= ARRAY_SIZE(ScreenshotFormatExtensions))
	{
		DEBUG_LOG(("Screenshot format %d is invalid", (int)format));
		return;
	}

	// The filename is created here so the timestamp matches the capture time.
	char leafname[_MAX_FNAME];
	const char* extension = ScreenshotFormatExtensions[format];

	SYSTEMTIME st;
	GetLocalTime(&st);
	sprintf(leafname, "sshot_%04d%02d%02d_%02d%02d%02d_%03d.%s",
		st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, extension);

	// The path is captured on the frame thread so workers never read global game data.
	char outputDirectory[_MAX_PATH];
	char outputPath[_MAX_PATH];
	strlcpy(outputDirectory, TheGlobalData->getPath_UserData().str(), ARRAY_SIZE(outputDirectory));
	strlcat(outputDirectory, "Screenshots\\", ARRAY_SIZE(outputDirectory));
	strlcpy(outputPath, outputDirectory, ARRAY_SIZE(outputPath));
	strlcat(outputPath, leafname, ARRAY_SIZE(outputPath));

	if (DX8Wrapper::Is_D3D11_Backend_Active())
	{
		if (!reserveD3D11ScreenshotName(outputDirectory, leafname, leafname,
			ARRAY_SIZE(leafname), outputPath, ARRAY_SIZE(outputPath)))
		{
			DEBUG_LOG(("D3D11 screenshot name reservation failed"));
			return;
		}
		rts::render::RenderBackBufferInfo backBufferInfo;
		const rts::render::RenderResult infoResult =
			DX8Wrapper::Get_D3D11_Back_Buffer_Info(&backBufferInfo);
		const unsigned width = backBufferInfo.width;
		const unsigned height = backBufferInfo.height;
		size_t pitchSize = 0;
		size_t pixelCount = 0;
		size_t pixelDataSize = 0;
		size_t imageSize = 0;
		if (infoResult != rts::render::RENDER_RESULT_OK || width == 0 ||
			height == 0 || backBufferInfo.format !=
			rts::render::RENDER_FORMAT_B8G8R8A8_UNORM ||
			!checkedScreenshotMultiply(static_cast<size_t>(width), 4,
				&pitchSize) ||
			!checkedScreenshotMultiply(static_cast<size_t>(width),
				static_cast<size_t>(height), &pixelCount) ||
			!checkedScreenshotMultiply(pixelCount, 4, &pixelDataSize) ||
			!checkedScreenshotMultiply(pixelCount, 3, &imageSize))
		{
			DEBUG_LOG(("D3D11 screenshot dimensions %u x %u are invalid", width,
				height));
			return;
		}
		if (pitchSize > UINT_MAX)
		{
			DEBUG_LOG(("D3D11 screenshot pitch is too large: %u x %u", width,
				height));
			return;
		}

		const unsigned pitch = static_cast<unsigned>(pitchSize);
		unsigned char* pixelData = allocateScreenshotBuffer(pixelDataSize);
		unsigned char* image = allocateScreenshotBuffer(imageSize);
		ScreenshotWrittenMessage* completion = 0;
		try
		{
			completion = new ScreenshotWrittenMessage;
		}
		catch (...)
		{
			completion = 0;
		}
		if (pixelData == 0 || image == 0 || completion == 0)
		{
			DEBUG_LOG(("Dropped D3D11 screenshot %s because its buffers could not be allocated",
				leafname));
			delete[] pixelData;
			delete[] image;
			delete completion;
			return;
		}

		D3D11CompressedScreenshotCapture* capture = 0;
		try
		{
			capture = new D3D11CompressedScreenshotCapture;
		}
		catch (...)
		{
			capture = 0;
		}
		if (capture == 0)
		{
			delete[] pixelData;
			delete[] image;
			delete completion;
			return;
		}
		capture->pixelData = pixelData;
		capture->image = image;
		capture->completion = completion;
		capture->width = width;
		capture->height = height;
		capture->pitch = pitch;
		capture->quality = jpegQuality;
		capture->format = format;
		strlcpy(capture->outputDirectory, outputDirectory,
			ARRAY_SIZE(capture->outputDirectory));
		strlcpy(capture->outputPath, outputPath,
			ARRAY_SIZE(capture->outputPath));
		strlcpy(capture->leafname, leafname, ARRAY_SIZE(capture->leafname));
		rts::render::RenderCaptureRequestDescriptor descriptor;
		descriptor.kind = rts::render::RENDER_CAPTURE_COMPRESSED_SCREENSHOT;
		descriptor.consumer = capture;
		descriptor.completed = completeD3D11CompressedScreenshot;
		descriptor.cancelled = cancelD3D11CompressedScreenshot;
		rts::render::RenderCaptureHandle handle;
		const rts::render::RenderResult queueResult =
			DX8Wrapper::Queue_D3D11_Back_Buffer_Capture(descriptor, &handle);
		if (queueResult != rts::render::RENDER_RESULT_OK)
		{
			DEBUG_LOG(("D3D11 screenshot queue rejected %s: result=%d",
				leafname, static_cast<int>(queueResult)));
			cancelD3D11CompressedScreenshot(capture, &handle, queueResult);
		}
		return;
	}

	// TheSuperHackers @bugfix xezon 21/05/2025 Get the back buffer and create a copy of the surface.
	// Originally this code took the front buffer and tried to lock it. This does not work when the
	// render view clips outside the desktop boundaries. It crashed the game.
	SurfaceClass* surface = DX8Wrapper::_Get_DX8_Back_Buffer();
	SurfaceClass::SurfaceDescription surfaceDesc;
	surface->Get_Description(surfaceDesc);

	// TheSuperHackers @bugfix bobtista 08/07/2026 Support the 16 bit back buffer format that the
	// game uses when running in 16 bit color mode. Reading it with the 32 bit stride read garbage.
	const bool is32Bit = surfaceDesc.Format == WW3D_FORMAT_A8R8G8B8 || surfaceDesc.Format == WW3D_FORMAT_X8R8G8B8;
	const bool is16Bit = surfaceDesc.Format == WW3D_FORMAT_R5G6B5;

	if (!is32Bit && !is16Bit)
	{
		DEBUG_LOG(("Screenshot does not support back buffer format %d", (int)surfaceDesc.Format));
		surface->Release_Ref();
		return;
	}

	const unsigned bytesPerPixel = is16Bit ? 2 : 4;
	if (surfaceDesc.Width == 0 || surfaceDesc.Height == 0 ||
		surfaceDesc.Width > (unsigned)INT_MAX / 3 ||
		surfaceDesc.Height > (unsigned)INT_MAX ||
		surfaceDesc.Width > UINT_MAX / bytesPerPixel)
	{
		DEBUG_LOG(("Screenshot dimensions %u x %u are invalid", surfaceDesc.Width, surfaceDesc.Height));
		surface->Release_Ref();
		return;
	}

	SurfaceClass* surfaceCopy = NEW_REF(SurfaceClass, (DX8Wrapper::_Create_DX8_Surface(surfaceDesc.Width, surfaceDesc.Height, surfaceDesc.Format)));
	DX8Wrapper::_Copy_DX8_Rects(surface->Peek_D3D_Surface(), nullptr, 0, surfaceCopy->Peek_D3D_Surface(), nullptr);

	surface->Release_Ref();
	surface = nullptr;

	struct Rect
	{
		int Pitch;
		void* pBits;
	} lrect;

	lrect.pBits = surfaceCopy->Lock(&lrect.Pitch);
	if (lrect.pBits == 0)
	{
		surfaceCopy->Release_Ref();
		return;
	}

	const unsigned pitch = (unsigned)lrect.Pitch;
	const size_t maxAllocation = (size_t)-1;
	const size_t width = (size_t)surfaceDesc.Width;
	const size_t height = (size_t)surfaceDesc.Height;
	if (lrect.Pitch <= 0 || pitch < surfaceDesc.Width * bytesPerPixel ||
		height > maxAllocation / pitch || width > maxAllocation / height ||
		width * height > maxAllocation / 3)
	{
		DEBUG_LOG(("Screenshot surface dimensions or pitch overflow an allocation"));
		surfaceCopy->Unlock();
		surfaceCopy->Release_Ref();
		return;
	}

	const size_t pixelDataSize = (size_t)pitch * height;
	const size_t imageSize = 3 * width * height;
	unsigned char* pixelData = allocateScreenshotBuffer(pixelDataSize);
	unsigned char* image = allocateScreenshotBuffer(imageSize);
	ScreenshotWrittenMessage* completion = 0;
	try
	{
		completion = new ScreenshotWrittenMessage;
	}
	catch (...)
	{
		completion = 0;
	}

	if (pixelData == 0 || image == 0 || completion == 0)
	{
		DEBUG_LOG(("Dropped screenshot %s because its buffers could not be allocated", leafname));
		delete[] pixelData;
		delete[] image;
		delete completion;
		surfaceCopy->Unlock();
		surfaceCopy->Release_Ref();
		return;
	}

	memcpy(pixelData, lrect.pBits, pixelDataSize);

	surfaceCopy->Unlock();
	surfaceCopy->Release_Ref();
	surfaceCopy = 0;

	ScreenshotBatch* batch = 0;
	try
	{
		batch = new ScreenshotBatch(pixelData, image, completion,
			surfaceDesc.Width, surfaceDesc.Height, pitch,
			is16Bit ? SCREENSHOT_SOURCE_RGB565 : SCREENSHOT_SOURCE_ARGB32,
			outputDirectory, outputPath, leafname, jpegQuality, format);
	}
	catch (...)
	{
		batch = 0;
	}
	if (batch == 0)
	{
		DEBUG_LOG(("Dropped screenshot %s because its batch could not be allocated", leafname));
		delete[] pixelData;
		delete[] image;
		delete completion;
		return;
	}

	s_screenshotTaskService.submit(batch);
}
