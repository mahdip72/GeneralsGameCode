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
#include <Utility/interlocked_adapter.h>
#include "W3DDevice/GameClient/W3DScreenshotCodec.h"
#include "Common/GlobalData.h"
#include "GameClient/GameClient.h"
#include "Lib/RenderedBattleBenchmarkOptions.h"
#include "GameClient/GameText.h"
#include "GameClient/InGameUI.h"
#include "Lib/JobSystem.h"
#include "Lib/PipelineExecutionPolicy.h"
#include "Renderer/RenderGameClient.h"

// Keep the source-level contract explicit without importing the renderer namespace.
using rts::render::RENDER_CAPTURE_COMPRESSED_SCREENSHOT;
using rts::render::RENDER_FORMAT_B8G8R8A8_UNORM;
using rts::render::RENDER_RESULT_OK;

#include "WW3D2/ww3d.h"
#include "WW3D2/surfaceclass.h"
#include "WWLib/mpsc_intrusive_queue.h"
#include "rts/profile.h"
#include <stb_image_write.h>
#include <io.h>
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
	unsigned diagnosticSample, diagnosticWidth, diagnosticHeight, diagnosticFrame;
	bool diagnosticSuccess;
	const char *diagnosticReason;
	char diagnosticPath[_MAX_PATH];
};
static MPSCIntrusiveQueue<ScreenshotWrittenMessage> s_screenshotWrittenQueue;

static bool recordVisualScreenshotEvent(const char *event, unsigned sample,
	unsigned width, unsigned height, const char *path, int queueResult, const char *reason = "none", unsigned frame = 0)
{
	SYSTEMTIME utc;
	LARGE_INTEGER qpc, frequency;
	GetSystemTime(&utc);
	qpc.QuadPart = 0; frequency.QuadPart = 0;
	const bool clockValid = QueryPerformanceCounter(&qpc) != FALSE && QueryPerformanceFrequency(&frequency) != FALSE;
	char record[1024];
	_snprintf(record, sizeof(record),
		"RENDER_VISUAL_SAMPLE_%s sample=%u utc=%04u-%02u-%02uT%02u:%02u:%02u.%03uZ "
		"qpc=%I64d qpc_frequency=%I64d frame=%u width=%u height=%u queue_result=%d clock_valid=%d reason=%s path=\"%s\"\n",
		event, sample, utc.wYear, utc.wMonth, utc.wDay, utc.wHour, utc.wMinute, utc.wSecond,
		utc.wMilliseconds, qpc.QuadPart, frequency.QuadPart,
		frame, width, height, queueResult, clockValid ? 1 : 0, reason, path);
	record[sizeof(record) - 1] = '\0';
	return rts::rendered_battle::WriteVisualSampleRecord(record) && clockValid;
}

// Owner/worker callbacks transfer existing metadata to the GAME-thread queue;
// they never mutate process sample counters or read game/profile globals.
static void publishDiagnosticFailure(ScreenshotWrittenMessage *&completion, const char *reason)
{
	if (completion && completion->diagnosticSample)
	{
		completion->diagnosticSuccess = false;
		completion->diagnosticReason = reason;
		s_screenshotWrittenQueue.Push(completion);
		completion = 0;
	}
}

static void failVisualScreenshotSynchronously(const char *reason)
{
	if (!rts::rendered_battle::ProcessTestOptions().visualSamples) return;
	rts::rendered_battle::VisualSampleState &state = rts::rendered_battle::ProcessVisualSampleState();
	recordVisualScreenshotEvent("FAILED", state.currentSample, 0, 0, "none", -1, reason,
		TheGameClient ? TheGameClient->getFrame() : 0);
	rts::rendered_battle::VisualSampleTerminal(state, state.currentSample, false,
		rts::rendered_battle::ProcessTestOptions().benchmarkRequested ? 20 : 40);
}

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

struct DiagnosticPngOutput
{
	HANDLE file;
	bool failed;
	DiagnosticPngOutput() : file(INVALID_HANDLE_VALUE), failed(false) {}
	~DiagnosticPngOutput() { if (file != INVALID_HANDLE_VALUE) CloseHandle(file); }
};
static void writeDiagnosticPngBytes(void *context, void *bytes, int size)
{
	DiagnosticPngOutput *output = static_cast<DiagnosticPngOutput *>(context);
	DWORD written = 0;
	if (size < 0 || !WriteFile(output->file, bytes, static_cast<DWORD>(size), &written, NULL) ||
		written != static_cast<DWORD>(size)) output->failed = true;
}
static bool writeDiagnosticPng(const char *directory, const char *path,
	unsigned width, unsigned height, const unsigned char *image)
{
	if (!rts::rendered_battle::IsNonReparseDirectoryTree(directory)) return false;
	DiagnosticPngOutput output;
	output.file = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
	if (output.file == INVALID_HANDLE_VALUE) return false;
	output.failed = false;
	const bool encoded = stbi_write_png_to_func(writeDiagnosticPngBytes, &output,
		static_cast<int>(width), static_cast<int>(height), 3, image, static_cast<int>(width * 3)) != 0;
	const bool flushed = FlushFileBuffers(output.file) != FALSE;
	const bool closed = CloseHandle(output.file) != FALSE;
	output.file = INVALID_HANDLE_VALUE;
	return encoded && !output.failed && flushed && closed;
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
					publishDiagnosticFailure(m_completion, "encode_exception");
				}
			}
			else publishDiagnosticFailure(m_completion, "conversion_failed");
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
			if (!m_completion->diagnosticSample) CreateDirectory(m_outputDirectory, 0);

			switch (m_format)
			{
				case SCREENSHOT_JPEG:
					success = stbi_write_jpg(m_outputPath, (int)m_source.width, (int)m_source.height,
						3, m_image, m_quality);
					break;
				case SCREENSHOT_PNG:
					if (m_completion->diagnosticSample)
						success = writeDiagnosticPng(m_outputDirectory, m_outputPath, m_source.width, m_source.height, m_image);
					else
						success = stbi_write_png(m_outputPath, (int)m_source.width, (int)m_source.height,
							3, m_image, (int)(m_source.width * 3));
					break;
			}
		}

		if (success)
		{
			m_completion->diagnosticSuccess = true;
			m_completion->diagnosticReason = "none";
			s_screenshotWrittenQueue.Push(m_completion);
			m_completion = 0;
		}
		else
		{
			DEBUG_LOG(("Failed to write screenshot %s", m_outputPath));
			publishDiagnosticFailure(m_completion, "png_write_failed");
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
		if (!rts::UseParallelPipelines() || !system.ensureStarted())
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

// Requests are created on the render owner. A monotonic sequence avoids
// in-flight name reuse without retaining every historical path for the life
// of the process. Existing files are skipped so a new process cannot overwrite
// captures from an earlier run.
static unsigned int s_d3d11ScreenshotSequence = 0;

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
	for (unsigned int attempt = 0; attempt < 1000000; ++attempt)
	{
		const unsigned int suffix = s_d3d11ScreenshotSequence++;
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
		if (_access(outputPath, 0) == 0)
		{
			continue;
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
		publishDiagnosticFailure(capture->completion, "invalid_readback");
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
		publishDiagnosticFailure(capture->completion, "batch_allocation_failed");
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
		publishDiagnosticFailure(capture->completion, "capture_cancelled");
		delete[] capture->pixelData;
		delete[] capture->image;
		delete capture->completion;
		delete capture;
	}
}

void W3D_UpdateScreenshotMessages()
{
	ScreenshotWrittenMessage* message = s_screenshotWrittenQueue.Flush();
	if (TheInGameUI == 0 && !rts::rendered_battle::ProcessTestOptions().visualSamples)
	{
		deleteScreenshotWrittenMessages(message);
		return;
	}

	while (message != 0)
	{
		if (message->diagnosticSample)
		{
			const bool receipted = recordVisualScreenshotEvent(message->diagnosticSuccess ? "WRITTEN" : "FAILED",
				message->diagnosticSample, message->diagnosticWidth, message->diagnosticHeight,
				message->diagnosticPath, message->diagnosticSuccess ? 0 : -1, message->diagnosticReason, message->diagnosticFrame);
			rts::rendered_battle::VisualSampleTerminal(rts::rendered_battle::ProcessVisualSampleState(),
				message->diagnosticSample, message->diagnosticSuccess && receipted,
				rts::rendered_battle::ProcessTestOptions().benchmarkRequested ? 20 : 40);
		}
		else if (TheInGameUI)
		{
			UnicodeString ufileName;
			ufileName.translate(message->leafname);
			TheInGameUI->message(TheGameText->fetch("GUI:ScreenCapture"), ufileName.str());
		}
		ScreenshotWrittenMessage* next = message->next;
		delete message;
		message = next;
	}
}

void W3D_ShutdownScreenshotTasks()
{
	s_screenshotTaskService.shutdown();
	if (rts::rendered_battle::ProcessTestOptions().visualSamples)
		W3D_UpdateScreenshotMessages();
	else
		deleteScreenshotWrittenMessages(s_screenshotWrittenQueue.Flush());
}

void W3D_TakeCompressedScreenshot(ScreenshotFormat format, Int jpegQuality)
{
	if (rts::rendered_battle::ProcessTestOptions().visualSamples)
	{
		rts::rendered_battle::VisualSampleState &samples = rts::rendered_battle::ProcessVisualSampleState();
		if (samples.captureDisabled) return;
		if (!TheGlobalData || !rts::rendered_battle::VisualSampleProfileMatches(TheGlobalData->getPath_UserData().str()))
		{
			failVisualScreenshotSynchronously("effective_profile_mismatch");
			return;
		}
	}
	static constexpr const char* const ScreenshotFormatExtensions[] = { "jpg", "png" };
	static_assert(ARRAY_SIZE(ScreenshotFormatExtensions) == SCREENSHOT_FORMAT_COUNT, "Incorrect array size");

	if ((unsigned)format >= ARRAY_SIZE(ScreenshotFormatExtensions))
	{
		DEBUG_LOG(("Screenshot format %d is invalid", (int)format));
		failVisualScreenshotSynchronously("request_failed");
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
	if (rts::rendered_battle::ProcessTestOptions().visualSamples)
	{
		if ((!CreateDirectoryA(outputDirectory, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) ||
			!rts::rendered_battle::IsNonReparseDirectoryTree(outputDirectory))
		{
			failVisualScreenshotSynchronously("unsafe_screenshot_directory");
			return;
		}
		if (rts::rendered_battle::ProcessVisualSampleState().currentSample)
			sprintf(leafname, "sshot_visual_pid%lu_sample%03u.png", GetCurrentProcessId(),
				rts::rendered_battle::ProcessVisualSampleState().currentSample);
	}
	strlcpy(outputPath, outputDirectory, ARRAY_SIZE(outputPath));
	strlcat(outputPath, leafname, ARRAY_SIZE(outputPath));

	{
		if (!rts::rendered_battle::ProcessTestOptions().visualSamples && !reserveD3D11ScreenshotName(outputDirectory, leafname, leafname,
			ARRAY_SIZE(leafname), outputPath, ARRAY_SIZE(outputPath)))
		{
			DEBUG_LOG(("D3D11 screenshot name reservation failed"));
			failVisualScreenshotSynchronously("request_failed");
			return;
		}
		rts::render::RenderBackBufferInfo backBufferInfo;
		const rts::render::RenderResult infoResult =
			rts::render::GetGameBackBufferInfo(&backBufferInfo);
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
			failVisualScreenshotSynchronously("request_failed");
			return;
		}
		if (pitchSize > UINT_MAX)
		{
			DEBUG_LOG(("D3D11 screenshot pitch is too large: %u x %u", width,
				height));
			failVisualScreenshotSynchronously("request_failed");
			return;
		}
		if (rts::rendered_battle::ProcessTestOptions().visualSamples && (width != 1920 || height != 1080))
		{
			failVisualScreenshotSynchronously("backbuffer_dimensions");
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
			failVisualScreenshotSynchronously("request_failed");
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
			failVisualScreenshotSynchronously("request_failed");
			return;
		}
		capture->pixelData = pixelData;
		completion->diagnosticSample = rts::rendered_battle::ProcessTestOptions().visualSamples ?
			rts::rendered_battle::ProcessVisualSampleState().currentSample : 0;
		completion->diagnosticSuccess = false;
		completion->diagnosticReason = "pending";
		completion->diagnosticWidth = width;
		completion->diagnosticHeight = height;
		completion->diagnosticFrame = TheGameClient ? TheGameClient->getFrame() : 0;
		if (completion->diagnosticSample)
			strlcpy(completion->diagnosticPath, outputPath, ARRAY_SIZE(completion->diagnosticPath));
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
		const unsigned diagnosticSample = completion->diagnosticSample;
		const unsigned diagnosticFrame = completion->diagnosticFrame;
		const rts::render::RenderResult queueResult =
			rts::render::QueueGameBackBufferCapture(descriptor, &handle);
		if (diagnosticSample && !recordVisualScreenshotEvent("REQUEST", diagnosticSample, width, height,
			outputPath, static_cast<int>(queueResult), "none", diagnosticFrame))
			failVisualScreenshotSynchronously("request_receipt_failed");
		if (queueResult != rts::render::RENDER_RESULT_OK)
		{
			DEBUG_LOG(("D3D11 screenshot queue rejected %s: result=%d",
				leafname, static_cast<int>(queueResult)));
			cancelD3D11CompressedScreenshot(capture, &handle, queueResult);
		}
		return;
	}

}
