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
 *                 Project Name : Native texture loader                                          *
 *                                                                                             *
 *                     $Archive:: /Commando/Code/ww3d2/textureloader.h                            $*
 *                                                                                             *
 *              Original Author:: vss_sync                                                   *
 *                                                                                             *
 *                       Author : Kenny Mitchell                                               *
 *                                                                                             *
 *								$Modtime:: 08/05/02 10:03a                                             $*
 *                                                                                             *
 *                    $Revision:: 3                                                           $*
 *                                                                                             *
 * 06/27/02 KM Texture class abstraction																			*
 * 08/05/02 KM Texture class redesign (revisited)
 *---------------------------------------------------------------------------------------------*
 * Functions:                                                                                  *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#include "textureloader.h"
#include "nativew3dsampledtexture.h"
#include "Renderer/NativeW3DResources.h"
#include "Lib/JobSystem.h"
#include "Lib/PipelineExecutionPolicy.h"
#define RTS_ASYNC_RESOURCE_IO 1
#include "Lib/ResourceIoPipeline.h"
#include "Lib/ModelAssetBytes.h"
#include "WWLib/mutex.h"
#include "WWLib/thread.h"
#include "WWDebug/wwdebug.h"
#include "texture.h"
#include "WWLib/ffactory.h"
#include "WWLib/wwstring.h"
#include	"WWLib/bufffile.h"
#include "ww3d.h"
#include "assetmgr.h"
#include "WWLib/TARGA.h"
#include "WWDebug/wwmemlog.h"
#include "formconv.h"
#include "texturethumbnail.h"
#include "ddsfile.h"
#include "bitmaphandler.h"
#include "WWDebug/wwprofile.h"


bool TextureLoader::TextureLoadSuspended;
int TextureLoader::TextureInactiveOverrideTime = 0;

static unsigned _TextureOwnerThreadID = 0;

#define USE_MANAGED_TEXTURES

////////////////////////////////////////////////////////////////////////////////
//
// TextureLoadTaskListClass implementation
//
////////////////////////////////////////////////////////////////////////////////

TextureLoadTaskListClass::TextureLoadTaskListClass()
: Root()
{
	Root.Next = Root.Prev = &Root;
}

void TextureLoadTaskListClass::Push_Front	(TextureLoadTaskClass *task)
{
	// task should non-null and not on any list
	WWASSERT(task != nullptr && task->Next == nullptr && task->Prev == nullptr);

	// update inserted task to point to list
	task->Next			= Root.Next;
	task->Prev			= &Root;
	task->List			= this;

	// update list to point to inserted task
	Root.Next->Prev	= task;
	Root.Next			= task;
}

void TextureLoadTaskListClass::Push_Back(TextureLoadTaskClass *task)
{
	// task should be non-null and not on any list
	WWASSERT(task != nullptr && task->Next == nullptr && task->Prev == nullptr);

	// update inserted task to point to list
	task->Next			= &Root;
	task->Prev			= Root.Prev;
	task->List			= this;

	// update list to point to inserted task
	Root.Prev->Next	= task;
	Root.Prev			= task;
}

TextureLoadTaskClass *TextureLoadTaskListClass::Pop_Front()
{
	// exit early if list is empty
	if (Is_Empty()) {
		return nullptr;
	}

	// otherwise, grab first task and remove it.
	TextureLoadTaskClass *task = (TextureLoadTaskClass *)Root.Next;
	Remove(task);
	return task;

}

TextureLoadTaskClass *TextureLoadTaskListClass::Pop_Back()
{
	// exit early if list is empty
	if (Is_Empty()) {
		return nullptr;
	}

	// otherwise, grab last task and remove it.
	TextureLoadTaskClass *task = (TextureLoadTaskClass *)Root.Prev;
	Remove(task);
	return task;
}

TextureLoadTaskClass *TextureLoadTaskListClass::Peek_Front() const
{
	return Is_Empty() ? nullptr : (TextureLoadTaskClass *)Root.Next;
}

void TextureLoadTaskListClass::Remove(TextureLoadTaskClass *task)
{
	// exit early if task is not on this list.
	if (task->List != this) {
		return;
	}

	// update list to skip task
	task->Prev->Next = task->Next;
	task->Next->Prev = task->Prev;

	// update task to no longer point at list
	task->Prev	= nullptr;
	task->Next	= nullptr;
	task->List	= nullptr;
}


////////////////////////////////////////////////////////////////////////////////
//
// SynchronizedTextureLoadTaskListClass implementation
//
////////////////////////////////////////////////////////////////////////////////

SynchronizedTextureLoadTaskListClass::SynchronizedTextureLoadTaskListClass()
:	TextureLoadTaskListClass(),
	CriticalSection(),
	ReadyQueue()
{
}

void SynchronizedTextureLoadTaskListClass::Push_Front(TextureLoadTaskClass *task)
{
	FastCriticalSectionClass::LockClass lock(CriticalSection);
	TextureLoadTaskListClass::Push_Front(task);
}

void SynchronizedTextureLoadTaskListClass::Push_Back(TextureLoadTaskClass *task)
{
	FastCriticalSectionClass::LockClass lock(CriticalSection);
	TextureLoadTaskListClass::Push_Back(task);
}

void SynchronizedTextureLoadTaskListClass::Publish_Completed(TextureLoadTaskClass *task)
{
	FastCriticalSectionClass::LockClass lock(CriticalSection);
	task->Set_Prepare_Runtime_Task(nullptr);
	if (task->Get_Priority() == TextureLoadTaskClass::PRIORITY_HIGH)
	{
		TextureLoadTaskListClass::Push_Front(task);
	}
	else
	{
		// Ready streaming work remains FIFO and publishes before requests that
		// have not started, without overtaking explicit foreground requests.
		ReadyQueue.Push_Back(task);
	}
	task->Complete_Async_Prepare();
}

void SynchronizedTextureLoadTaskListClass::Publish_Failed(TextureLoadTaskClass *task)
{
	FastCriticalSectionClass::LockClass lock(CriticalSection);
	task->Set_Prepare_Runtime_Task(nullptr);
	if (task->Get_Priority() == TextureLoadTaskClass::PRIORITY_HIGH)
	{
		TextureLoadTaskListClass::Push_Front(task);
	}
	else
	{
		ReadyQueue.Push_Back(task);
	}
	task->Fail_Async_Prepare();
}

void SynchronizedTextureLoadTaskListClass::Publish_Thumbnail(
	TextureLoadTaskClass *task, TextureLoadTaskClass *loadTask)
{
	FastCriticalSectionClass::LockClass lock(CriticalSection);
	if (loadTask != nullptr && loadTask->Is_Async_Prepare_Complete())
	{
		task->Set_State(TextureLoadTaskClass::STATE_COMPLETE);
	}
	TextureLoadTaskListClass::Push_Back(task);
}

bool SynchronizedTextureLoadTaskListClass::Has_Prepare_Job(
	TextureLoadTaskClass *task)
{
	FastCriticalSectionClass::LockClass lock(CriticalSection);
	return task->Get_Prepare_Runtime_Task() != nullptr;
}

void SynchronizedTextureLoadTaskListClass::Set_Prepare_Job(
	TextureLoadTaskClass *task, void *prepareJob)
{
	FastCriticalSectionClass::LockClass lock(CriticalSection);
	task->Set_Prepare_Runtime_Task(prepareJob);
}

bool SynchronizedTextureLoadTaskListClass::Promote_Prepare_Job(
	TextureLoadTaskClass *task)
{
	FastCriticalSectionClass::LockClass lock(CriticalSection);
	task->Set_Priority(TextureLoadTaskClass::PRIORITY_HIGH);
	rts::Job *prepareJob = static_cast<rts::Job *>(
		task->Get_Prepare_Runtime_Task());
	if (prepareJob != nullptr)
	{
		return rts::JobSystem::instance().tryPromote(prepareJob,
			rts::JOB_PRIORITY_FRAME_CRITICAL);
	}
	if (task->Get_List() == this)
	{
		TextureLoadTaskListClass::Remove(task);
		TextureLoadTaskListClass::Push_Front(task);
		return true;
	}
	if (task->Get_List() == &ReadyQueue)
	{
		ReadyQueue.Remove(task);
		TextureLoadTaskListClass::Push_Front(task);
		return true;
	}
	return false;
}

TextureLoadTaskClass *SynchronizedTextureLoadTaskListClass::Pop_Front()
{
	FastCriticalSectionClass::LockClass lock(CriticalSection);
	TextureLoadTaskClass *task = TextureLoadTaskListClass::Peek_Front();
	if (task != nullptr && task->Get_Priority() == TextureLoadTaskClass::PRIORITY_HIGH)
	{
		return TextureLoadTaskListClass::Pop_Front();
	}
	task = ReadyQueue.Pop_Front();
	if (task != nullptr)
	{
		return task;
	}
	return TextureLoadTaskListClass::Pop_Front();

}

TextureLoadTaskClass *SynchronizedTextureLoadTaskListClass::Pop_Back()
{
	FastCriticalSectionClass::LockClass lock(CriticalSection);
	TextureLoadTaskClass *task = TextureLoadTaskListClass::Pop_Back();
	return task != nullptr ? task : ReadyQueue.Pop_Back();
}

void SynchronizedTextureLoadTaskListClass::Remove(TextureLoadTaskClass *task)
{
	FastCriticalSectionClass::LockClass lock(CriticalSection);
	TextureLoadTaskListClass::Remove(task);
	ReadyQueue.Remove(task);
}

bool SynchronizedTextureLoadTaskListClass::Is_Empty()
{
	FastCriticalSectionClass::LockClass lock(CriticalSection);
	return TextureLoadTaskListClass::Is_Empty() && ReadyQueue.Is_Empty();
}


// Locks

// To prevent deadlock, threads should acquire locks in the order in which
// they are defined below. No ordering is necessary for the task list locks,
// since one thread can never hold two at once.

static FastCriticalSectionClass					_ForegroundCriticalSection;
static bool _AcceptingTextureRequests = false;
// Lists

static SynchronizedTextureLoadTaskListClass	_ForegroundQueue;

static TextureLoadTaskListClass					_TexLoadFreeList;
static TextureLoadTaskListClass					_CubeTexLoadFreeList;
static TextureLoadTaskListClass					_VolTexLoadFreeList;


static rts::JobGroup _TexturePrepareGroup;
static rts::ResourceIoPipeline _TextureResourcePipeline;
static rts::ModelAssetReadQueue _ModelResourceQueue;
static TextureLoadTaskListClass _ResourceQueue;
static bool _ResourcePipelineStarted = false;
static rts::JobMetricCounter _ResourceOwnerFallbacks = 0;
static TexturePrepareMemoryBudget _TexturePrepareMemoryBudget(64u * 1024u * 1024u);
static const unsigned long _TextureForegroundSliceMilliseconds = 4;
static const unsigned _TextureForegroundMaximumTasksPerUpdate = 8;

static void Record_Texture_Reference_Fallback()
{
	rts::JobSystem::instance().recordSerialFallback();
	++_ResourceOwnerFallbacks;
}

static bool Try_Load_For_Owner(TextureLoadTaskClass *task)
{
	try
	{
		return task->Load();
	}
	catch (...)
	{
		// Owner-side fallbacks must still publish a failed preparation so the
		// existing End_Load path applies the missing texture and releases all
		// staged resources. Worker execution keeps its separate catch/publication
		// path below so worker ownership is unchanged.
		task->Fail_Async_Prepare();
		return false;
	}
}

static bool Is_Format_Compressed(WW3DFormat texture_format, bool allow_compression)
{
	if (!allow_compression)
	{
		return false;
	}
	return texture_format == WW3D_FORMAT_UNKNOWN ||
		texture_format == WW3D_FORMAT_DXT1 ||
		texture_format == WW3D_FORMAT_DXT2 ||
		texture_format == WW3D_FORMAT_DXT3 ||
		texture_format == WW3D_FORMAT_DXT4 ||
		texture_format == WW3D_FORMAT_DXT5;
}


////////////////////////////////////////////////////////////////////////////////
//
// TextureLoader implementation
//
////////////////////////////////////////////////////////////////////////////////

void TextureLoader::Init()
{
	_TextureOwnerThreadID = ThreadClass::_Get_Current_Thread_ID();
	ThumbnailManagerClass::Init();

	rts::JobSystem &system = rts::JobSystem::instance();
	if (rts::UseParallelPipelines() && system.ensureStarted())
	{
		_TexturePrepareGroup = system.createGroup();
	}
	_ResourcePipelineStarted = rts::UseParallelPipelines() &&
		_TextureResourcePipeline.start(rts::ResourceIoConfig(), _TexturePrepareGroup);
	{
		FastCriticalSectionClass::LockClass lock(_ForegroundCriticalSection);
		_AcceptingTextureRequests = true;
	}
	TextureInactiveOverrideTime = 0;
}


void TextureLoader::Deinit()
{
	Stop_Async_Resource_Loading();
	{
		FastCriticalSectionClass::LockClass lock(_ForegroundCriticalSection);
		_AcceptingTextureRequests = false;
	}
	if (_TexturePrepareGroup.isValid())
	{
		rts::JobSystem::instance().wait(_TexturePrepareGroup);
		_TexturePrepareGroup = rts::JobGroup();
	}

	TextureLoadTaskClass *task = nullptr;
	while ((task = _ForegroundQueue.Pop_Front()) != nullptr)
	{
		switch (task->Get_Type())
		{
			case TextureLoadTaskClass::TASK_THUMBNAIL:
				Process_Foreground_Thumbnail(task);
				break;

			case TextureLoadTaskClass::TASK_LOAD:
				// JobSystem shuts down before W3DDisplay. Retire any queued
				// owner-side load synchronously so teardown cannot restart the
				// scheduler or publish into a deinitialized texture subsystem.
				task->Finish_Load();
				task->Destroy();
				break;
		}
	}

	ThumbnailManagerClass::Deinit();
	TextureLoadTaskClass::Delete_Free_Pool();
	_TextureOwnerThreadID = 0;
}


bool TextureLoader::Is_DX8_Thread()
{
	const unsigned owner_thread = _TextureOwnerThreadID;
	return owner_thread == 0 ||
		owner_thread == ThreadClass::_Get_Current_Thread_ID();
}


// ----------------------------------------------------------------------------
//
// Modify given texture size to nearest valid size on current hardware.
//
// ----------------------------------------------------------------------------

static unsigned Native_Next_Power_Of_Two(unsigned value, unsigned maximum)
{
	unsigned result = 1;
	while (result < value && result < maximum)
	{
		result <<= 1;
	}
	return result > maximum ? maximum : result;
}

void TextureLoader::Validate_Texture_Size
(
	unsigned& width,
	unsigned& height,
	unsigned& depth
)
{
	// Native sampled textures retain the legacy power-of-two sizing policy,
	// while using native resource limits instead of querying a legacy device.
	const unsigned max_texture_dimension = 16384u;
	const unsigned max_volume_extent = 2048u;
	width = Native_Next_Power_Of_Two(width, max_texture_dimension);
	height = Native_Next_Power_Of_Two(height, max_texture_dimension);
	depth = Native_Next_Power_Of_Two(depth, max_volume_extent);
}


void *TextureLoader::Load_Thumbnail(const StringClass& filename, const Vector3& hsv_shift)
{
	WWASSERT(Is_DX8_Thread());
	(void)filename;
	(void)hsv_shift;
	// Native thumbnails are represented by the regular prepared-mip
	// publication; there is no separate graphics object to return.
	return nullptr;
}


// ----------------------------------------------------------------------------
//
// Load image to a surface. The function tries to create texture that matches
// targa format. If suitable format is not available, it selects closest matching
// format and performs color space conversion.
//
// ----------------------------------------------------------------------------
void *TextureLoader::Load_Surface_Immediate(
	const StringClass& filename,
	WW3DFormat texture_format,
	bool allow_compression)
{
	WWASSERT(Is_DX8_Thread());
	(void)filename;
	(void)texture_format;
	(void)allow_compression;
	// Native callers publish prepared mip memory through TextureBaseClass.
	return nullptr;
}


void TextureLoader::Request_Thumbnail(TextureBaseClass *tc)
{
	// A thumbnail request promotes the same prepared-mip pipeline used by the
	// full typed texture publication.
	if (tc == nullptr) return;
	rts::render::NativeW3DTextureHandle native_handle;
	if (!tc->Acquire_Native_Texture(&native_handle))
		Request_Foreground_Loading(tc);
	return;
}


void TextureLoader::Request_Background_Loading(TextureBaseClass *tc)
{
	WWPROFILE(("TextureLoader::Request_Background_Loading()"));
	// Grab the foreground lock. This prevents the foreground thread
	// from retiring any tasks related to this texture. It also
	// serializes calls to Request_Background_Loading from other
	// threads.
	FastCriticalSectionClass::LockClass foreground_lock(_ForegroundCriticalSection);
	if (!_AcceptingTextureRequests)
	{
		return;
	}

	// Has the texture already been loaded?
	if (tc->Is_Initialized()) {
		return;
	}

	TextureLoadTaskClass *task = tc->TextureLoadTask;

	// if texture already has a load task, we don't need to create another one.
	if (task) {
		return;
	}

	task = TextureLoadTaskClass::Create(tc, TextureLoadTaskClass::TASK_LOAD, TextureLoadTaskClass::PRIORITY_LOW);

	// Full texture reads and staging can be expensive on rotational storage.
	// Queue low-priority work even when requested by the render owner so Update
	// can enforce its per-frame foreground budget. A later foreground request
	// still takes the task back and completes it immediately when required.
	_ForegroundQueue.Push_Back(task);
}


void TextureLoader::Request_Foreground_Loading(TextureBaseClass *tc)
{
	WWPROFILE(("TextureLoader::Request_Foreground_Loading()"));
	// Grab the foreground lock. This prevents the foreground thread
	// from retiring the load tasks for this texture. It also
	// serializes calls to Request_Foreground_Loading from other
	// threads.
	FastCriticalSectionClass::LockClass foreground_lock(_ForegroundCriticalSection);
	if (!_AcceptingTextureRequests)
	{
		return;
	}

	// Has the texture already been loaded?
	if (tc->Is_Initialized()) {
		return;
	}

	TextureLoadTaskClass *task			= tc->TextureLoadTask;
	TextureLoadTaskClass *task_thumb = tc->ThumbnailLoadTask;

	if (Is_DX8_Thread()) {

		// since we're in the render-owner thread, we can load the entire
		// texture right now.

		// if we have a thumbnail task waiting, kill it.
		if (task_thumb) {
			_ForegroundQueue.Remove(task_thumb);
			task_thumb->Destroy();
		}

		if (task) {
			// Take queued work back from the runtime so a foreground request is
			// not blocked behind unrelated textures. If this task is already
			// active, wait only for that preparation.
			if (task->ResourceRequest != nullptr)
			{
				task->Wait_For_Async_Prepare();
			}
			else if (!task->Is_Async_Prepare_Complete())
			{
				if (_ForegroundQueue.Has_Prepare_Job(task))
				{
					task->Wait_For_Async_Prepare();
				}
			}
			_ForegroundQueue.Remove(task);
		} else {
			// Since the task manages all the state associated with loading
			// a texture, we temporarily create one.
			task = TextureLoadTaskClass::Create(tc, TextureLoadTaskClass::TASK_LOAD, TextureLoadTaskClass::PRIORITY_HIGH);
		}

		// finish loading the task and destroy it.
		task->Finish_Load();
		task->Destroy();

	} else {
		// we are not in the render-owner thread. We need to add a high-priority loading
		// task to the foreground queue.

		// if we have a thumbnail task, we should cancel it. Since we are not
		// the foreground thread, we are not allowed to call Destroy(). Instead,
		// leave it queued in the completed state so it will be destroyed by Update().
		if (task_thumb) {
			task_thumb->Set_State(TextureLoadTaskClass::STATE_COMPLETE);
		}

		if (task) {
			// Promote queued preparation ahead of streaming work. If a worker
			// already owns it, publication proceeds normally.
			_ForegroundQueue.Promote_Prepare_Job(task);
			if (task->ResourceRequest != nullptr)
				_TextureResourcePipeline.promote(*static_cast<rts::ResourceIoTicket*>(task->ResourceRequest));

		} else {
			// allocate high priority load task
			task = TextureLoadTaskClass::Create(tc, TextureLoadTaskClass::TASK_LOAD, TextureLoadTaskClass::PRIORITY_HIGH);

			// Keep an explicit foreground request ahead of streaming work.
			_ForegroundQueue.Push_Front(task);
		}
	}
}


void TextureLoader::Flush_Pending_Load_Tasks()
{
	// This function can only be called from the main thread.
	// (Only the main thread can make the render-thread calls necessary
	// to complete texture loading. If we wanted to flush
	// the pending tasks from another thread, we'd probably
	// want to set a bool that is checked by Update().
	WWASSERT(Is_DX8_Thread());

	for (;;) {
		Pump_Resource_Loads();
		if (_TexturePrepareGroup.isValid())
		{
			rts::JobSystem::instance().wait(_TexturePrepareGroup);
		}
		if (_ForegroundQueue.Is_Empty()
			&& _ResourceQueue.Is_Empty()
		) {
			break;
		}
		Update();
		if (TextureLoadTaskClass *pending = _ResourceQueue.Peek_Front())
			_TextureResourcePipeline.wait(*static_cast<rts::ResourceIoTicket*>(pending->ResourceRequest));
	}
}


void TextureLoader::Discard_Pending_Background_Load_Tasks()
{
	WWASSERT(Is_DX8_Thread());
	FastCriticalSectionClass::LockClass lock(_ForegroundCriticalSection);
	// A generation boundary rejects every old read/decode before factories,
	// archive catalogs, or the device can be reset. Keep foreground intent,
	// but resolve its source again in the new generation.
	_TextureResourcePipeline.advanceGeneration();
	_ModelResourceQueue.discard(_TextureResourcePipeline);
	while (TextureLoadTaskClass *resourceTask = _ResourceQueue.Pop_Front())
	{
		resourceTask->Cancel_Resource_Read();
		if (resourceTask->Get_Priority() == TextureLoadTaskClass::PRIORITY_LOW)
			resourceTask->Destroy();
		else _ForegroundQueue.Push_Back(resourceTask);
	}
	// A prepare job temporarily owns its task outside the foreground queue.
	// Drain the group before filtering so an outgoing-map task cannot publish
	// after reset and upload its prepared surfaces into the next map.
	if (_TexturePrepareGroup.isValid())
	{
		rts::JobSystem::instance().wait(_TexturePrepareGroup);
	}
	TextureLoadTaskListClass retainedTasks;
	TextureLoadTaskClass *task = nullptr;
	while ((task = _ForegroundQueue.Pop_Front()) != nullptr)
	{
		if (task->Get_Type() == TextureLoadTaskClass::TASK_LOAD &&
			task->Get_Priority() == TextureLoadTaskClass::PRIORITY_LOW)
		{
			task->Destroy();
		}
		else
		{
			retainedTasks.Push_Back(task);
		}
	}
	while ((task = retainedTasks.Pop_Front()) != nullptr)
	{
		_ForegroundQueue.Push_Back(task);
	}
}


// Nework update macro for texture loader.
#pragma warning(disable:4201) // warning C4201: nonstandard extension used : nameless struct/union
#include <mmsystem.h>
#define UPDATE_NETWORK 											\
	if (network_callback) {                            \
		unsigned long time2 = timeGetTime();            \
		if (time2 - time > 20) {                        \
			network_callback();                          \
			time = time2;                                \
		}                                               \
	}                                                  \


void TextureLoader::Update(void (*network_callback)())
{
	WWASSERT_PRINT(Is_DX8_Thread(), "TextureLoader::Update must be called from the main thread!");

	if (TextureLoadSuspended) {
		return;
	}

	// grab foreground lock to prevent any other thread from
	// modifying texture tasks.
	FastCriticalSectionClass::LockClass lock(_ForegroundCriticalSection);

	unsigned long time = timeGetTime();
	Pump_Resource_Loads();
	const unsigned long budgetStart = time;
	unsigned processedTaskCount = 0;

	// Bound low-priority file reads, staging, and uploads so first-use texture
	// bursts do not consume an entire rendered frame. Flush and Deinit retain
	// their explicit drain semantics by invoking Update repeatedly or bypassing
	// this loop.
	while (TextureLoadTaskClass *task = _ForegroundQueue.Pop_Front()) {
		UPDATE_NETWORK;
		// dispatch to proper task handler
		switch (task->Get_Type()) {
			case TextureLoadTaskClass::TASK_THUMBNAIL:
				Process_Foreground_Thumbnail(task);
				break;

			case TextureLoadTaskClass::TASK_LOAD:
				Process_Foreground_Load(task);
				break;
		}
		++processedTaskCount;
		if (processedTaskCount >= _TextureForegroundMaximumTasksPerUpdate ||
			timeGetTime() - budgetStart >= _TextureForegroundSliceMilliseconds) {
			break;
		}
	}

	TextureBaseClass::Invalidate_Old_Unused_Textures(TextureInactiveOverrideTime);
}

void TextureLoader::Suspend_Texture_Load()
{
	WWASSERT_PRINT(Is_DX8_Thread(),"TextureLoader::Suspend_Texture_Load must be called from the main thread!");
	TextureLoadSuspended=true;
}

void TextureLoader::Continue_Texture_Load()
{
	WWASSERT_PRINT(Is_DX8_Thread(),"TextureLoader::Continue_Texture_Load must be called from the main thread!");
	TextureLoadSuspended=false;
}

void TextureLoader::Process_Foreground_Thumbnail(TextureLoadTaskClass *task)
{
	rts::render::NativeW3DTextureHandle native_handle;
	switch (task->Get_State()) {
		case TextureLoadTaskClass::STATE_NONE:
			// A full load can complete inline if worker admission fails after this
			// thumbnail was queued. Do not replace that full texture afterward.
			if (task->Peek_Texture()->Acquire_Native_Texture(&native_handle))
			{
				task->Destroy();
				break;
			}
			Load_Thumbnail(task->Peek_Texture());
			FALLTHROUGH; // NOTE: fall-through is intentional

		case TextureLoadTaskClass::STATE_COMPLETE:
			task->Destroy();
			break;
	}
}


void TextureLoader::Process_Foreground_Load(TextureLoadTaskClass *task)
{
	// A worker publishes through the synchronized queue before returning.
	// Waiting here guarantees it has completed its final task access before
	// End_Load/Destroy can recycle the pooled object.
	if (task->Get_State() == TextureLoadTaskClass::STATE_LOAD_MIPMAP)
	{
		task->Wait_For_Async_Prepare();
	}

	// Is high-priority task?
	if (task->Get_Priority() == TextureLoadTaskClass::PRIORITY_HIGH) {
		task->Finish_Load();
		task->Destroy();
		return;
	}

	// otherwise, must be a low-priority task.

	switch (task->Get_State()) {
		case TextureLoadTaskClass::STATE_NONE:
			Begin_Load_And_Queue(task);
			break;

		case TextureLoadTaskClass::STATE_LOAD_MIPMAP:
			task->End_Load();
			task->Destroy();
			break;
	}
}


void TextureLoader::Begin_Load_And_Queue(TextureLoadTaskClass *task)
{
	WWASSERT(Is_DX8_Thread());
	if (task->Begin_Resource_Read()) return;
	// Unsupported factories, source-budget pressure, and the reference lane
	// stay entirely on owner. Never send pooled tasks or file objects to jobs.
	Record_Texture_Reference_Fallback();
	task->Finish_Load();
	task->Destroy();
}

void TextureLoader::Load_Thumbnail(TextureBaseClass *tc)
{
	// Thumbnail requests use the same native prepared-mip pipeline as full
	// loads.  Keep this owner-side entry point for source compatibility.
	WWASSERT(Is_DX8_Thread());
	if (tc != nullptr && !tc->Is_Initialized())
	{
		Request_Foreground_Loading(tc);
	}
}


////////////////////////////////////////////////////////////////////////////////
//
// TextureLoaderTaskClass implementation
//
////////////////////////////////////////////////////////////////////////////////

TextureLoadTaskClass::TextureLoadTaskClass()
:	Texture			(nullptr),
	LoadedTextureHandle		(nullptr),
	Format			(WW3D_FORMAT_UNKNOWN),
	Width				(0),
	Height			(0),
	MipLevelCount	(MIP_LEVELS_ALL),
	Reduction		(0),
	SourceFormat		(WW3D_FORMAT_UNKNOWN),
	SourceBytesPerPixel(0),
	CompressionAllowed(false),
	LoadSucceeded	(false),
	DDSFile			(nullptr),
	TargaFile		(nullptr),
	PrepareCompleteEvent(nullptr),
	PrepareRuntimeTask(nullptr),
	ResourceRequest(nullptr),
	ResourceResult(nullptr),
	ResourceTriedDDS(false),
	PrepareMemoryReservation(0),
	Type				(TASK_NONE),
	Priority			(PRIORITY_LOW),
	State				(STATE_NONE),
	HSVShift			(0.0f,0.0f,0.0f)
{
	// because texture load tasks are pooled, the constructor and destructor
	// don't need to do much. The work of attaching a task to a texture is
	// is done by Init() and Deinit().

	for (int i = 0; i < MIP_LEVELS_MAX; ++i) {
		LockedSurfacePtr[i]		= nullptr;
		LockedSurfacePitch[i]	= 0;
	}
	Filename[0] = 0;
}


TextureLoadTaskClass::~TextureLoadTaskClass()
{
	Deinit();
	if (PrepareCompleteEvent != nullptr)
	{
		CloseHandle((HANDLE)PrepareCompleteEvent);
		PrepareCompleteEvent = nullptr;
	}
}


bool TextureLoadTaskClass::Begin_Async_Prepare()
{
	if (PrepareCompleteEvent == nullptr)
	{
		PrepareCompleteEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
		return PrepareCompleteEvent != nullptr;
	}

	return ResetEvent((HANDLE)PrepareCompleteEvent) != FALSE;
}


void TextureLoadTaskClass::Complete_Async_Prepare()
{
	if (PrepareCompleteEvent != nullptr)
	{
		SetEvent((HANDLE)PrepareCompleteEvent);
	}
}


void TextureLoadTaskClass::Fail_Async_Prepare()
{
	LoadSucceeded = false;
	State = STATE_LOAD_MIPMAP;
	Complete_Async_Prepare();
}


bool TextureLoadTaskClass::Is_Async_Prepare_Complete()
{
	if (ResourceRequest != nullptr)
		return _TextureResourcePipeline.isComplete(*static_cast<rts::ResourceIoTicket*>(ResourceRequest));
	return PrepareCompleteEvent != nullptr &&
		WaitForSingleObject((HANDLE)PrepareCompleteEvent, 0) == WAIT_OBJECT_0;
}


void TextureLoadTaskClass::Wait_For_Async_Prepare()
{
	if (ResourceRequest != nullptr)
	{
		WWASSERT(TextureLoader::Is_DX8_Thread());
		_ResourceQueue.Remove(this);
		while (ResourceRequest != nullptr)
		{
			const rts::ResourceIoTicket ticket = *static_cast<rts::ResourceIoTicket*>(ResourceRequest);
			_TextureResourcePipeline.promote(ticket);
			if (!_TextureResourcePipeline.wait(ticket)) TextureLoader::Pump_Resource_Loads();
			if (_TextureResourcePipeline.isComplete(ticket)) Complete_Resource_Read();
			// DDS-to-TGA retry re-enqueues; foreground owns this task until return.
			_ResourceQueue.Remove(this);
		}
		return;
	}
	if (PrepareCompleteEvent != nullptr)
	{
		WaitForSingleObject((HANDLE)PrepareCompleteEvent, INFINITE);
	}
}


TextureLoadTaskClass *TextureLoadTaskClass::Create(TextureBaseClass *tc, TaskType type, PriorityType priority)
{
	// recycle or create a new texture load task with the given type
	// and priority, then associate the texture with the task.

	// pull a load task from front of free list
	TextureLoadTaskClass *task = nullptr;
	switch (tc->Get_Asset_Type())
	{
		case TextureBaseClass::TEX_REGULAR : task=_TexLoadFreeList.Pop_Front(); break;
		case TextureBaseClass::TEX_CUBEMAP : task=_CubeTexLoadFreeList.Pop_Front(); break;
		case TextureBaseClass::TEX_VOLUME : task=_VolTexLoadFreeList.Pop_Front(); break;
		default : WWASSERT(0);
	};

	// if no tasks on free list, allocate a new task
	if (!task)
	{
		switch (tc->Get_Asset_Type())
		{
		case TextureBaseClass::TEX_REGULAR : task=new TextureLoadTaskClass; break;
		case TextureBaseClass::TEX_CUBEMAP : task=new CubeTextureLoadTaskClass; break;
		case TextureBaseClass::TEX_VOLUME : task=new VolumeTextureLoadTaskClass; break;
		default : WWASSERT(0);
		}
	}
	task->Init(tc, type, priority);
	return task;
}


void TextureLoadTaskClass::Destroy()
{
	// detach the task from its texture, and return to free pool.
	Deinit();
	_TexLoadFreeList.Push_Front(this);
}


void TextureLoadTaskClass::Delete_Free_Pool()
{
	// (gth) We should probably just MEMPool these task objects...
	while (TextureLoadTaskClass *task = _TexLoadFreeList.Pop_Front()) {
		delete task;
	}
	while (TextureLoadTaskClass *task = _CubeTexLoadFreeList.Pop_Front()) {
		delete static_cast<CubeTextureLoadTaskClass *>(task);
	}
	while (TextureLoadTaskClass *task = _VolTexLoadFreeList.Pop_Front()) {
		delete static_cast<VolumeTextureLoadTaskClass *>(task);
	}
}


void TextureLoadTaskClass::Init(TextureBaseClass* tc, TaskType type, PriorityType priority)
{
	WWASSERT(tc);

	// NOTE: we must be in the main thread to avoid corrupting the texture's refcount.
	WWASSERT(TextureLoader::Is_DX8_Thread());
	REF_PTR_SET(Texture, tc);

	// Make sure texture has a filename.
	WWASSERT(!Texture->Get_Full_Path().Is_Empty());

	Type				= type;
	Priority			= priority;
	State				= STATE_NONE;

	LoadedTextureHandle		= nullptr;

	TextureClass* tex=Texture->As_TextureClass();

	if (tex)
	{
		Format			= tex->Get_Texture_Format(); // don't assume format yet KM
	}
	else
	{
		Format			= WW3D_FORMAT_UNKNOWN;
	}

	Width				= 0;
	Height			= 0;
	MipLevelCount	= Texture->MipLevelCount;
	Reduction		= Texture->Get_Reduction();
	HSVShift			= Texture->Get_HSV_Shift();
	SourceFormat		= WW3D_FORMAT_UNKNOWN;
	SourceBytesPerPixel = 0;
	CompressionAllowed = Texture->Is_Compression_Allowed();
	LoadSucceeded	= false;
	DDSFile			= nullptr;
	TargaFile		= nullptr;
	PrepareRuntimeTask = nullptr;
	WWASSERT(PrepareMemoryReservation == 0);
	WWASSERT(ResourceRequest == nullptr && ResourceResult == nullptr);
	ResourceTriedDDS = false;
	if (PrepareCompleteEvent != nullptr)
	{
		ResetEvent((HANDLE)PrepareCompleteEvent);
	}
	strlcpy(Filename, Texture->Get_Full_Path().str(), ARRAY_SIZE(Filename));


	for (int i = 0; i < MIP_LEVELS_MAX; ++i)
	{
		LockedSurfacePtr[i]		= nullptr;
		LockedSurfacePitch[i]	= 0;
	}

	switch (Type)
	{
		case TASK_THUMBNAIL:
			WWASSERT(Texture->ThumbnailLoadTask == nullptr);
			Texture->ThumbnailLoadTask = this;
			break;

		case TASK_LOAD:
			WWASSERT(Texture->TextureLoadTask == nullptr);
			Texture->TextureLoadTask = this;
			break;
	}
}


void TextureLoadTaskClass::Deinit()
{
	// task should not be on any list when it is being detached from texture.
	WWASSERT(Next == nullptr);
	WWASSERT(Prev == nullptr);

	WWASSERT(LoadedTextureHandle == nullptr);
	Release_Prepared_Surfaces();
	delete DDSFile;
	DDSFile = nullptr;
	delete TargaFile;
	TargaFile = nullptr;
	PrepareRuntimeTask = nullptr;
	Release_Prepare_Memory_Reservation();

	for (int i = 0; i < MIP_LEVELS_MAX; ++i) {
		WWASSERT(LockedSurfacePtr[i] == nullptr);
	}

	if (Texture) {
		switch (Type) {
			case TASK_THUMBNAIL:
				WWASSERT(Texture->ThumbnailLoadTask == this);
				Texture->ThumbnailLoadTask = nullptr;
				break;

			case TASK_LOAD:
				WWASSERT(Texture->TextureLoadTask == this);
				Texture->TextureLoadTask = nullptr;
				break;
		}

		// NOTE: we must be in main thread to avoid corrupting Texture's refcount.
		WWASSERT(TextureLoader::Is_DX8_Thread());
		REF_PTR_RELEASE(Texture);
	}
}


bool TextureLoadTaskClass::Begin_Load()
{
	WWASSERT(TextureLoader::Is_DX8_Thread());

	bool loaded = false;

	// if allowed, begin a compressed load
	if (CompressionAllowed) {
		loaded = Begin_Compressed_Load();
	}

	// otherwise, begin an uncompressed load
	if (!loaded) {
		loaded = Begin_Uncompressed_Load();
	}

	// if not loaded, abort.
	if (!loaded) {
		return false;
	}

	State = STATE_LOAD_BEGUN;

	return true;
}

static bool Add_Prepare_Memory_Bytes(size_t& total, size_t bytes)
{
	if (bytes > (size_t)-1 - total)
	{
		return false;
	}
	total += bytes;
	return true;
}

size_t TextureLoadTaskClass::Get_Prepare_Memory_Byte_Count() const
{
	size_t total = 0;
	unsigned int level;
	for (level = 0; level < MIP_LEVELS_MAX; ++level)
	{
		if (!Add_Prepare_Memory_Bytes(total, PreparedSurface[level].layout().dataSize))
		{
			return (size_t)-1;
		}
	}

	if (DDSFile != nullptr)
	{
		if (!Add_Prepare_Memory_Bytes(total, DDSFile->Get_Retained_Memory_Size()))
		{
			return (size_t)-1;
		}
	}
	else if (TargaFile != nullptr)
	{
		size_t sourceBytes = (size_t)TargaFile->Header.Width;
		if (TargaFile->Header.Height != 0 &&
			sourceBytes > (size_t)-1 / (size_t)TargaFile->Header.Height)
		{
			return (size_t)-1;
		}
		sourceBytes *= (size_t)TargaFile->Header.Height;
		if (SourceBytesPerPixel != 0 && sourceBytes > (size_t)-1 / SourceBytesPerPixel)
		{
			return (size_t)-1;
		}
		sourceBytes *= SourceBytesPerPixel;
		if (!Add_Prepare_Memory_Bytes(total, sourceBytes))
		{
			return (size_t)-1;
		}

		// TGA conversion can temporarily retain a full A8R8G8B8 surface while
		// the source image and prepared mip chain remain live on the worker.
		size_t conversionBytes = (size_t)Width;
		if (Height != 0 && conversionBytes > (size_t)-1 / Height)
		{
			return (size_t)-1;
		}
		conversionBytes *= Height;
		if (conversionBytes > (size_t)-1 / 4)
		{
			return (size_t)-1;
		}
		if (!Add_Prepare_Memory_Bytes(total, conversionBytes * 4))
		{
			return (size_t)-1;
		}
	}

	return total;
}

bool TextureLoadTaskClass::Reserve_Prepare_Memory()
{
	WWASSERT(PrepareMemoryReservation == 0);
	const size_t bytes = Get_Prepare_Memory_Byte_Count();
	if (bytes == 0 || !_TexturePrepareMemoryBudget.tryReserve(bytes))
	{
		return false;
	}
	PrepareMemoryReservation = bytes;
	return true;
}

void TextureLoadTaskClass::Release_Prepare_Memory_Reservation()
{
	if (PrepareMemoryReservation != 0)
	{
		const bool released = _TexturePrepareMemoryBudget.release(PrepareMemoryReservation);
		WWASSERT(released);
		(void)released;
		PrepareMemoryReservation = 0;
	}
}


// ----------------------------------------------------------------------------
//
// Load mipmap levels to a pre-generated and locked texture object based on
// information in load task object. Try loading from a DDS file first and if
// that fails try a TGA.
//
// ----------------------------------------------------------------------------
bool TextureLoadTaskClass::Load()
{
	PROFILER_SECTION_NAME("Texture.Prepare");

	bool loaded = false;

	// if allowed, try to load compressed mipmaps
	if (CompressionAllowed) {
		loaded = Load_Compressed_Mipmap();
	}

	// otherwise, load uncompressed mipmaps
	if (!loaded) {
		loaded = Load_Uncompressed_Mipmap();
	}

	State = STATE_LOAD_MIPMAP;
	LoadSucceeded = loaded;

	return loaded;
}


void TextureLoadTaskClass::End_Load()
{
	WWASSERT(TextureLoader::Is_DX8_Thread());

	if (LoadSucceeded && Publish_Native_Prepared_Texture())
	{
		PROFILER_SECTION_NAME("Texture.Upload.Native");
	}
	else
	{
		Apply_Missing_Texture();
	}

	Release_Prepared_Surfaces();
	delete DDSFile;
	DDSFile = nullptr;
	delete TargaFile;
	TargaFile = nullptr;

	State = STATE_LOAD_COMPLETE;
}


void TextureLoadTaskClass::Finish_Load()
{
	switch (State) {
		// NOTE: fall-through below is intentional.

		case STATE_NONE:
			if (!Begin_Load()) {
				Apply_Missing_Texture();
				break;
			}
			FALLTHROUGH;

		case STATE_LOAD_BEGUN:
			Try_Load_For_Owner(this);
			FALLTHROUGH;

		case STATE_LOAD_MIPMAP:
			End_Load();
			FALLTHROUGH;

		default:
			break;
	}
}


void TextureLoadTaskClass::Apply_Missing_Texture()
{
	WWASSERT(TextureLoader::Is_DX8_Thread());
	WWASSERT(!LoadedTextureHandle);

	if (!Texture->Apply_Native_Missing_Texture())
	{
		Texture->Apply_New_Surface(nullptr, false);
	}
}


void TextureLoadTaskClass::Apply(bool initialize)
{
	(void)initialize;
	WWASSERT(!LoadedTextureHandle);
	return;
}


static unsigned Get_Requested_Reduction(unsigned width, unsigned height, unsigned mip_count)
{
	// Figure out correct reduction
	unsigned reqReduction = WW3D::Get_Texture_Reduction();

	// Leave only the lowest level
	if (reqReduction >= max(mip_count, 1u))
		reqReduction = mip_count-1;

	// Clamp reduction
	unsigned curReduction = 0;
	unsigned curWidth = width;
	unsigned curHeight = height;
	unsigned minDim = WW3D::Get_Texture_Min_Dimension();

	while (curReduction < reqReduction && curWidth > minDim && curHeight > minDim)
	{
		curWidth >>= 1;
		curHeight >>= 1;
		curReduction++;
	}

	return curReduction;
}


static bool	Get_Texture_Information
(
	const char* filename,
	unsigned& reduction,
	unsigned& w,
	unsigned& h,
	unsigned& d,
	WW3DFormat& format,
	unsigned& mip_count,
	bool compressed
)
{
	ThumbnailClass* thumb=ThumbnailManagerClass::Peek_Thumbnail_Instance_From_Any_Manager(filename);

	if (!thumb)
	{
		if (compressed)
		{
			DDSFileClass dds_file(filename, 0);
			if (!dds_file.Is_Available())
				return false;

			// Destination size will be the next power of two square from the larger width and height...
			w = dds_file.Get_Width(0);
			h = dds_file.Get_Height(0);
			d = dds_file.Get_Depth(0);
			format = dds_file.Get_Format();
			mip_count = dds_file.Get_Mip_Level_Count();
			reduction = Get_Requested_Reduction(w, h, mip_count);

			return true;
		}

		Targa targa;
		if (TARGA_ERROR_HANDLER(targa.Open(filename, TGA_READMODE), filename))
		{
			return false;
		}

		unsigned int bpp;
		WW3DFormat dest_format;
		Get_WW3D_Format(dest_format,format,bpp,targa);

		// Figure out how many mip levels this texture will occupy
		mip_count = 0;
		for (int i=targa.Header.Width, j=targa.Header.Height; i > 0 && j > 0; i>>=1, j>>=1)
				mip_count++;

		// Destination size will be the next power of two square from the larger width and height...
		w = targa.Header.Width;
		h = targa.Header.Height;
		d = 1;
		reduction = Get_Requested_Reduction(w, h, mip_count);

		return true;
	}

	if (compressed &&
		thumb->Get_Original_Texture_Format()!=WW3D_FORMAT_DXT1 &&
		thumb->Get_Original_Texture_Format()!=WW3D_FORMAT_DXT2 &&
		thumb->Get_Original_Texture_Format()!=WW3D_FORMAT_DXT3 &&
		thumb->Get_Original_Texture_Format()!=WW3D_FORMAT_DXT4 &&
		thumb->Get_Original_Texture_Format()!=WW3D_FORMAT_DXT5) {
		return false;
	}

	w=thumb->Get_Original_Texture_Width();
	h=thumb->Get_Original_Texture_Height();
	d=1;
	mip_count=thumb->Get_Original_Texture_Mip_Level_Count();
	format=thumb->Get_Original_Texture_Format();
	reduction=0;

	return true;
}


static void Validate_Reduction(const TextureBaseClass* texture, unsigned& reduction, unsigned mip_count)
{
	if (!texture->Is_Reducible() || texture->MipLevelCount == MIP_LEVELS_1)
	{
		reduction = 0;
	}
	else if (texture->MipLevelCount != MIP_LEVELS_ALL && reduction >= (unsigned)texture->MipLevelCount)
	{
		reduction = (unsigned)texture->MipLevelCount - 1;
	}

	if (reduction >= mip_count)
	{
		reduction = 0; // should not be possible, but check just in case.
	}
}

// Will not present textures smaller than 4 pixels wide or high.
static constexpr const unsigned MinTextureDim = 4u;

// If the size doesn't match, try and see if texture reduction would help...
// (mainly for cases where loaded texture is larger than hardware limit)
static void Apply_Dim_Reduction(unsigned& width, unsigned& height, unsigned& reduction, unsigned mip_count)
{
	unsigned dummy_depth = 1;

	for (unsigned r = reduction; r < mip_count; ++r)
	{
		unsigned w = max(width >> r, MinTextureDim);
		unsigned h = max(height >> r, MinTextureDim);
		unsigned tmp_w = w;
		unsigned tmp_h = h;

		TextureLoader::Validate_Texture_Size(w, h, dummy_depth);

		if (w == tmp_w && h == tmp_h)
		{
			width = w;
			height = h;
			reduction = r;
			break;
		}
	}
}

static void Apply_Mip_Reduction(unsigned& mip_level_count, unsigned reduction, unsigned width, unsigned height, unsigned mip_count)
{
	// If texture wants all mip levels, take as many as the file contains (not necessarily all)
	// Otherwise take as many mip levels as the texture wants, not to exceed the count in file...
	if (mip_level_count == MIP_LEVELS_ALL)
	{
		mip_level_count = mip_count;
	}
	else
	{
		if (mip_level_count > mip_count)
			mip_level_count = mip_count;
	}

	// Reduce requested number by those removed.
	WWASSERT(reduction < mip_level_count);
	mip_level_count -= reduction;

	// Once more, verify that the mip level count is correct (in case it was changed here it might not
	// match the size...well actually it doesn't have to match but it can't be bigger than the size)
	unsigned int max_mip_level_count = 1;
	unsigned int dim = MinTextureDim;

	while (dim < width && dim < height)
	{
		dim <<= 1;
		max_mip_level_count++;
	}

	if (mip_level_count > max_mip_level_count)
		mip_level_count = max_mip_level_count;
}


// No texture, COM interface, FileClass, global factory, or pooled task is
// reachable from this operation. prepare is owner-only; decode sees only this
// captured descriptor, bounded encoded bytes, and its private output buffers.
class TextureResourceDecode : public rts::ResourceDecodeOperation
{
public:
	TextureResourceDecode(TextureBaseClass *texture, WW3DFormat requestedFormat, bool compressed)
		: Format(requestedFormat), Width(0), Height(0), Mips(texture->MipLevelCount),
		Reduction(0), HSV(texture->Get_HSV_Shift()), ReferenceRequired(false), Compressed(compressed),
		Cube(texture->Get_Asset_Type() == TextureBaseClass::TEX_CUBEMAP),
		Reducible(texture->Is_Reducible()), RequestedMips(texture->MipLevelCount),
		HasThumbnail(false), ThumbnailWidth(0), ThumbnailHeight(0), ThumbnailMips(0),
		ThumbnailFormat(WW3D_FORMAT_UNKNOWN), SourceFormat(WW3D_FORMAT_UNKNOWN),
		SourceBpp(0), DDS(nullptr), Prepared(false), Workspace(0)
	{
		ThumbnailClass *thumbnail = ThumbnailManagerClass::Peek_Thumbnail_Instance_From_Any_Manager(texture->Get_Full_Path());
		if (thumbnail != nullptr)
		{
			HasThumbnail = true;
			ThumbnailWidth = thumbnail->Get_Original_Texture_Width();
			ThumbnailHeight = thumbnail->Get_Original_Texture_Height();
			ThumbnailMips = thumbnail->Get_Original_Texture_Mip_Level_Count();
			ThumbnailFormat = thumbnail->Get_Original_Texture_Format();
		}
	}
	~TextureResourceDecode() { delete DDS; }

	bool prepare(const unsigned char *bytes, size_t size, size_t &workspace)
	{
		WWASSERT(TextureLoader::Is_DX8_Thread());
		if (Prepared) { workspace = Workspace; return true; }
		unsigned originalMips = 0;
		if (Compressed)
		{
			DDS = new DDSFileClass(nullptr, 0);
			if (!DDS->Set_Memory_Header(bytes, size) ||
				DDS->Get_Type() != (Cube ? DDS_CUBEMAP : DDS_TEXTURE) ||
				DDS->Get_Full_Width() > 65536 || DDS->Get_Full_Height() > 65536) return false;
			WW3DFormat sourceFormat = HasThumbnail ? ThumbnailFormat : DDS->Get_Format();
			if (sourceFormat != WW3D_FORMAT_DXT1 && sourceFormat != WW3D_FORMAT_DXT2 &&
				sourceFormat != WW3D_FORMAT_DXT3 && sourceFormat != WW3D_FORMAT_DXT4 &&
				sourceFormat != WW3D_FORMAT_DXT5) return false;
			Format = Get_Valid_Texture_Format(sourceFormat, true);
			Width = HasThumbnail ? ThumbnailWidth : DDS->Get_Width(0);
			Height = HasThumbnail ? ThumbnailHeight : DDS->Get_Height(0);
			originalMips = HasThumbnail ? ThumbnailMips : DDS->Get_Mip_Level_Count();
			if (!originalMips || !Width || !Height) return false;
			Reduction = HasThumbnail ? 0 : Get_Requested_Reduction(Width, Height, originalMips);
			if (!Reducible || RequestedMips == MIP_LEVELS_1) Reduction = 0;
			else if (RequestedMips != MIP_LEVELS_ALL && Reduction >= RequestedMips) Reduction = RequestedMips - 1;
			if (Reduction >= originalMips) Reduction = 0;
			Apply_Dim_Reduction(Width, Height, Reduction, originalMips);
			if (Reduction >= (Mips == MIP_LEVELS_ALL ? originalMips : min(Mips, originalMips))) return false;
			Apply_Mip_Reduction(Mips, Reduction, Width, Height, originalMips);
			// Keep the parsed header alive until its replacement is allocated. If
			// allocation throws, the pipeline will destroy this operation after
			// prepare returns false; leaving DDS valid keeps that destruction safe.
			DDSFileClass *replacement = nullptr;
			try { replacement = new DDSFileClass(nullptr, Reduction); }
			catch (...) { return false; }
			delete DDS;
			DDS = replacement;
			if (!DDS->Set_Memory_Header(bytes, size) || Mips > DDS->Get_Mip_Level_Count()) return false;
			const WW3DFormat ddsFormat = DDS->Get_Format();
			if (ddsFormat != WW3D_FORMAT_DXT1 && ddsFormat != WW3D_FORMAT_DXT5 &&
				(Format != ddsFormat || HSV.X != 0 || HSV.Y != 0 || HSV.Z != 0))
			{
				// Legacy DDS conversion has no implementation for DXT2/3/4
				// format conversion or HSV shifts. Leave ReferenceRequired false so
				// completion follows the existing TGA/missing-texture failure path,
				// rather than publishing unwritten owner surfaces as a success.
				return false;
			}
			if (Width != DDS->Get_Width(0) || Height != DDS->Get_Height(0))
			{
				ReferenceRequired = true;
				return false;
			}
			// Includes the owned DDS payload; the pipeline separately accounts for
			// the encoded input range. Conversion routines allocate no scratch.
			Workspace = size;
		}
		else
		{
			if (Cube || size < sizeof(TGAHeader)) return false;
			memcpy(&TGA.Header, bytes, sizeof(TGAHeader));
			if (TGA.Header.Width <= 0 || TGA.Header.Height <= 0) return false;
			Get_WW3D_Format(SourceFormat, SourceBpp, TGA);
			if (SourceFormat == WW3D_FORMAT_UNKNOWN || !SourceBpp) return false;
			Width = HasThumbnail ? ThumbnailWidth : TGA.Header.Width;
			Height = HasThumbnail ? ThumbnailHeight : TGA.Header.Height;
			if (!Width || !Height) return false;
			unsigned depth = 1;
			TextureLoader::Validate_Texture_Size(Width, Height, depth);
			const WW3DFormat originalFormat = HasThumbnail ? ThumbnailFormat : SourceFormat;
			Format = Get_Valid_Texture_Format(Format == WW3D_FORMAT_UNKNOWN ? originalFormat : Format, false);
			const unsigned available = CalculateTextureMipLevelCount(Width, Height);
			if (Mips == MIP_LEVELS_ALL || Mips > available) Mips = available;
			TextureMipLayout sourceLayout, conversionLayout;
			if (!CalculateTextureMipLayout(SourceFormat, TGA.Header.Width, TGA.Header.Height, 1, sourceLayout) ||
				!CalculateTextureMipLayout(WW3D_FORMAT_A8R8G8B8, Width, Height, 1, conversionLayout)) return false;
			Workspace = sourceLayout.dataSize;
			if (!Add_Prepare_Memory_Bytes(Workspace, conversionLayout.dataSize) ||
				!Add_Prepare_Memory_Bytes(Workspace, 1024)) return false;
		}
		if (!Mips || Mips > MIP_LEVELS_MAX) return false;
		unsigned w = Width, h = Height;
		for (unsigned level = 0; level < Mips; ++level)
		{
			TextureMipLayout layout;
			if (!CalculateTextureMipLayout(Format, w, h, 1, layout) ||
				layout.dataSize > (size_t)-1 / (Cube ? 6U : 1U) ||
				!Add_Prepare_Memory_Bytes(Workspace, layout.dataSize * (Cube ? 6U : 1U))) return false;
			ReduceTextureMipDimensions(w, h);
		}
		Prepared = true;
		workspace = Workspace;
		return true;
	}

	bool decode(const unsigned char *bytes, size_t size, const rts::ResourceCancellation &cancel)
	{
		const unsigned faces = Cube ? 6 : 1;
		for (unsigned face = 0; face < faces; ++face)
		{
			unsigned w = Width, h = Height;
			for (unsigned level = 0; level < Mips; ++level)
			{
				if (cancel.isCancelled() || !Surface[face][level].allocate(Format, w, h, 1)) return false;
				ReduceTextureMipDimensions(w, h);
			}
		}
		if (Compressed)
		{
			if (!DDS->Load_From_Memory(bytes, size)) return false;
			for (unsigned face = 0; face < faces; ++face)
			{
				unsigned w = Width, h = Height;
				for (unsigned level = 0; level < Mips; ++level)
				{
					if (cancel.isCancelled()) return false;
					if (Cube) DDS->Copy_CubeMap_Level_To_Surface(face, level, Format, w, h,
						Surface[face][level].data(), (unsigned)Surface[face][level].layout().rowPitch, HSV);
					else DDS->Copy_Level_To_Surface(level, Format, w, h,
						Surface[face][level].data(), (unsigned)Surface[face][level].layout().rowPitch, HSV);
					ReduceTextureMipDimensions(w, h);
				}
			}
			return !cancel.isCancelled();
		}
		if (TGA.Load_From_Memory(bytes, size, true) != 0 || cancel.isCancelled()) return false;
		unsigned srcWidth = TGA.Header.Width, srcHeight = TGA.Header.Height;
		unsigned srcBpp = SourceBpp;
		WW3DFormat srcFormat = SourceFormat;
		unsigned char *source = (unsigned char*)TGA.GetImage();
		Vector3 hsv = HSV;
		if (srcFormat == WW3D_FORMAT_A1R5G5B5 || srcFormat == WW3D_FORMAT_R5G6B5 ||
			srcFormat == WW3D_FORMAT_A4R4G4B4 || srcFormat == WW3D_FORMAT_P8 ||
			srcFormat == WW3D_FORMAT_L8 || srcWidth != Width || srcHeight != Height)
		{
			if (!Conversion.allocate(WW3D_FORMAT_A8R8G8B8, Width, Height, 1)) return false;
			BitmapHandlerClass::Copy_Image(Conversion.data(), Width, Height, Width * 4,
				WW3D_FORMAT_A8R8G8B8, source, srcWidth, srcHeight, srcWidth * srcBpp,
				srcFormat, (unsigned char*)TGA.GetPalette(), TGA.Header.CMapDepth >> 3, false, hsv);
			hsv = Vector3(0, 0, 0);
			source = Conversion.data(); srcFormat = WW3D_FORMAT_A8R8G8B8;
			srcWidth = Width; srcHeight = Height; srcBpp = 4;
		}
		const unsigned sourcePitch = srcWidth * srcBpp;
		unsigned width = Width, height = Height;
		for (unsigned level = 0; level < Mips; ++level)
		{
			if (cancel.isCancelled()) return false;
			BitmapHandlerClass::Copy_Image(Surface[0][level].data(), width, height,
				(unsigned)Surface[0][level].layout().rowPitch, Format, source, srcWidth,
				srcHeight, sourcePitch, srcFormat, nullptr, 0, true, hsv);
			hsv = Vector3(0, 0, 0);
			ReduceTextureMipDimensions(width, height);
			ReduceTextureMipDimensions(srcWidth, srcHeight);
		}
		return !cancel.isCancelled();
	}

	WW3DFormat Format;
	unsigned Width, Height, Mips, Reduction;
	Vector3 HSV;
	bool ReferenceRequired;
	TextureMipBuffer Surface[6][MIP_LEVELS_MAX];
private:
	bool Compressed, Cube, Reducible;
	unsigned RequestedMips;
	bool HasThumbnail;
	unsigned ThumbnailWidth, ThumbnailHeight, ThumbnailMips;
	WW3DFormat ThumbnailFormat, SourceFormat;
	unsigned SourceBpp;
	DDSFileClass *DDS;
	Targa TGA;
	TextureMipBuffer Conversion;
	bool Prepared;
	size_t Workspace;
};

bool TextureLoadTaskClass::Begin_Resource_Read(bool tryCompressed)
{
	if (!_ResourcePipelineStarted || !_AcceptingTextureRequests ||
		Texture->Get_Asset_Type() == TextureBaseClass::TEX_VOLUME || ResourceRequest != nullptr) return false;
	WWASSERT(TextureLoader::Is_DX8_Thread());
	ResourceTriedDDS = tryCompressed && CompressionAllowed;
	char name[_MAX_PATH];
	strlcpy(name, Filename, ARRAY_SIZE(name));
	if (ResourceTriedDDS)
	{
		const size_t length = strlen(name);
		if (length < 4 || name[length - 4] != '.') ResourceTriedDDS = false;
		else memcpy(name + length - 3, "dds", 3);
	}
	rts::ResourceIoSource *source = nullptr;
	{
		file_auto_ptr file(_TheFileFactory, name);
		if (file->Is_Available()) source = file->Capture_Resource_Read_Source();
	}
	if (source == nullptr)
	{
		if (ResourceTriedDDS) return Begin_Resource_Read(false);
		return false;
	}
	TextureResourceDecode *operation = nullptr;
	rts::ResourceIoTicket *ticket = nullptr;
	try
	{
		operation = new TextureResourceDecode(Texture, Format, ResourceTriedDDS);
		ticket = new rts::ResourceIoTicket;
	}
	catch (...) { delete operation; delete source; return false; }
	if (!_TextureResourcePipeline.submit(source, operation, Priority == PRIORITY_HIGH ?
		rts::JOB_PRIORITY_FRAME_CRITICAL : rts::JOB_PRIORITY_STREAMING, *ticket))
	{
		delete operation; delete source; delete ticket;
		return false;
	}
	ResourceRequest = ticket;
	State = STATE_LOAD_BEGUN;
	_ResourceQueue.Push_Back(this);
	return true;
}

bool TextureLoadTaskClass::Complete_Resource_Read()
{
	if (ResourceRequest == nullptr) return true;
	rts::ResourceIoStatus status;
	rts::ResourceDecodeOperation *operation = nullptr;
	const rts::ResourceIoTicket ticket = *static_cast<rts::ResourceIoTicket*>(ResourceRequest);
	if (!_TextureResourcePipeline.take(ticket, status, operation)) return false;
	delete static_cast<rts::ResourceIoTicket*>(ResourceRequest);
	ResourceRequest = nullptr;
	if (status == rts::RESOURCE_IO_SUCCEEDED)
	{
		TextureResourceDecode *result = static_cast<TextureResourceDecode*>(operation);
		ResourceResult = result;
		Format = result->Format; Width = result->Width; Height = result->Height;
		MipLevelCount = result->Mips; Reduction = result->Reduction;
		LoadSucceeded = true;
		State = STATE_LOAD_MIPMAP;
		Complete_Async_Prepare();
		return true;
	}
	const bool referenceRequired = operation != nullptr && static_cast<TextureResourceDecode*>(operation)->ReferenceRequired;
	delete operation;
	if (referenceRequired && status == rts::RESOURCE_IO_DECODE_FAILED)
	{
		State = STATE_NONE;
		Record_Texture_Reference_Fallback();
		if (Begin_Load()) Try_Load_For_Owner(this);
		else Fail_Async_Prepare();
		return true;
	}
	if (status != rts::RESOURCE_IO_CANCELLED && status != rts::RESOURCE_IO_STALE && ResourceTriedDDS)
	{
		if (Begin_Resource_Read(false)) return false;
		// A non-snapshot-capable TGA factory still receives its owner fallback.
		State = STATE_NONE;
		Record_Texture_Reference_Fallback();
		if (Begin_Uncompressed_Load()) { State = STATE_LOAD_BEGUN; Try_Load_For_Owner(this); }
		else Fail_Async_Prepare();
		return true;
	}
	Fail_Async_Prepare();
	return true;
}

void TextureLoadTaskClass::Cancel_Resource_Read()
{
	if (ResourceRequest == nullptr) return;
	const rts::ResourceIoTicket ticket = *static_cast<rts::ResourceIoTicket*>(ResourceRequest);
	_TextureResourcePipeline.cancel(ticket);
	_TextureResourcePipeline.wait(ticket);
	rts::ResourceIoStatus status;
	rts::ResourceDecodeOperation *operation = nullptr;
	_TextureResourcePipeline.take(ticket, status, operation);
	delete operation;
	delete static_cast<rts::ResourceIoTicket*>(ResourceRequest);
	ResourceRequest = nullptr;
	State = STATE_NONE;
	LoadSucceeded = false;
}

void TextureLoader::Pump_Resource_Loads()
{
	_TextureResourcePipeline.pump();
	_ModelResourceQueue.pump(_TextureResourcePipeline);
	unsigned count = 0;
	while (TextureLoadTaskClass *task = _ResourceQueue.Peek_Front())
	{
		const rts::ResourceIoTicket ticket = *static_cast<rts::ResourceIoTicket*>(task->ResourceRequest);
		if (!_TextureResourcePipeline.isComplete(ticket)) break;
		_ResourceQueue.Remove(task);
		if (task->Complete_Resource_Read())
		{
			// Immediate owner publication consumes the result before admitting
			// more decoded output, keeping the retained-byte bound meaningful.
			task->End_Load();
			task->Destroy();
		}
		if (++count >= _TextureForegroundMaximumTasksPerUpdate) break;
	}
}

void TextureLoader::Stop_Async_Resource_Loading()
{
	WWASSERT(Is_DX8_Thread());
	FastCriticalSectionClass::LockClass lock(_ForegroundCriticalSection);
	if (!_ResourcePipelineStarted) return;
	_TextureResourcePipeline.advanceGeneration();
	_ModelResourceQueue.discard(_TextureResourcePipeline);
	while (TextureLoadTaskClass *task = _ResourceQueue.Pop_Front())
	{
		task->Cancel_Resource_Read();
		task->Destroy();
	}
	_TextureResourcePipeline.shutdown();
	_ResourcePipelineStarted = false;
}

bool TextureLoader::Request_Model_Read(const char *filename, rts::ResourceIoTicket &ticket)
{
	WWASSERT(Is_DX8_Thread());
	ticket = rts::ResourceIoTicket();
	if (!_ResourcePipelineStarted || !_AcceptingTextureRequests || !filename || !_TheFileFactory)
		return false;
	Pump_Resource_Loads();
	rts::ResourceIoSource *source = nullptr;
	{
		file_auto_ptr file(_TheFileFactory, filename);
		if (file.get() && file->Is_Available()) source = file->Capture_Resource_Read_Source();
	}
	if (!source) return false;
	if (_ModelResourceQueue.submit(_TextureResourcePipeline, source, ticket)) return true;
	delete source;
	return false;
}

rts::ModelAssetBytes *TextureLoader::Complete_Model_Read(const rts::ResourceIoTicket &ticket,
	bool &succeeded, bool &cancelled)
{
	WWASSERT(Is_DX8_Thread());
	succeeded = false;
	cancelled = true;
	while (_ModelResourceQueue.contains(ticket))
	{
		Pump_Resource_Loads();
		rts::ResourceIoStatus status;
		rts::ModelAssetBytes *bytes = nullptr;
		if (_ModelResourceQueue.take(ticket, status, bytes))
		{
			succeeded = status == rts::RESOURCE_IO_SUCCEEDED;
			cancelled = status == rts::RESOURCE_IO_CANCELLED || status == rts::RESOURCE_IO_STALE;
			return bytes;
		}
		// wait may yield under retained-output pressure. Pump both result
		// owners before retrying; a model must not strand a foreground texture.
		_TextureResourcePipeline.wait(ticket);
	}
	return nullptr;
}

void TextureLoader::Cancel_Model_Read(const rts::ResourceIoTicket &ticket)
{
	WWASSERT(Is_DX8_Thread());
	_TextureResourcePipeline.cancel(ticket);
	bool succeeded, cancelled;
	delete Complete_Model_Read(ticket, succeeded, cancelled);
}

rts::ResourceIoMetrics TextureLoader::Get_Resource_Load_Metrics()
{
	FastCriticalSectionClass::LockClass lock(_ForegroundCriticalSection);
	rts::ResourceIoMetrics metrics = _TextureResourcePipeline.metrics();
	metrics.serialFallbacks += _ResourceOwnerFallbacks;
	return metrics;
}

bool TextureLoadTaskClass::Begin_Compressed_Load()
{
	unsigned orig_width,orig_height,orig_depth,orig_mip_count,orig_reduction;
	WW3DFormat orig_format;
	if (!Get_Texture_Information
		  (
				Filename,
				orig_reduction,
				orig_width,
				orig_height,
				orig_depth,
				orig_format,
				orig_mip_count,
				true
			)
		)
	{
		return false;
	}

	Format = Get_Valid_Texture_Format(orig_format, Texture->Is_Compression_Allowed());

	Reduction = orig_reduction;
	Validate_Reduction(Texture, Reduction, orig_mip_count);

	Width = orig_width;
	Height = orig_height;
	Apply_Dim_Reduction(Width, Height, Reduction, orig_mip_count);

	Apply_Mip_Reduction(MipLevelCount, Reduction, Width, Height, orig_mip_count);

	try
	{
		DDSFile = new DDSFileClass(Filename, Reduction);
	}
	catch (...)
	{
		DDSFile = nullptr;
	}

	if (DDSFile == nullptr || !DDSFile->Is_Available() || !DDSFile->Load())
	{
		delete DDSFile;
		DDSFile = nullptr;
		return false;
	}

	if (!Allocate_Prepared_Surfaces())
	{
		delete DDSFile;
		DDSFile = nullptr;
		return false;
	}

	return true;
}

bool TextureLoadTaskClass::Begin_Uncompressed_Load()
{
	unsigned orig_width,orig_height,orig_depth,orig_mip_count,orig_reduction;
	WW3DFormat orig_format;
	if (!Get_Texture_Information
		  (
				Filename,
				orig_reduction,
				orig_width,
				orig_height,
				orig_depth,
				orig_format,
				orig_mip_count,
				false
			)
		)
	{
		return false;
	}

	WW3DFormat src_format=orig_format;
	WW3DFormat dest_format=src_format;
	dest_format=Get_Valid_Texture_Format(dest_format,false);	// No compressed destination format if reading from targa...

   if (	src_format != WW3D_FORMAT_A8R8G8B8
   	&&	src_format != WW3D_FORMAT_R8G8B8
  		&&	src_format != WW3D_FORMAT_X8R8G8B8 )
	{
		WWDEBUG_SAY(("Invalid TGA format used in %s - only 24 and 32 bit formats should be used!", Filename));
	}

	// Destination size will be the next power of two square from the larger width and height...
	unsigned ow = orig_width;
	unsigned oh = orig_height;
	TextureLoader::Validate_Texture_Size(orig_width, orig_height,orig_depth);
	if (orig_width != ow || orig_height != oh)
	{
		WWDEBUG_SAY(("Invalid texture size, scaling required. Texture: %s, size: %d x %d -> %d x %d", Filename, ow, oh, orig_width, orig_height));
	}

	Width		= orig_width;
	Height	= orig_height;
	Reduction = 0;

	if (Format == WW3D_FORMAT_UNKNOWN)
	{
		Format=dest_format;
	}
	else
	{
		Format = Get_Valid_Texture_Format(Format, false);
	}

	const unsigned int availableMipLevels = CalculateTextureMipLevelCount(Width, Height);
	if (MipLevelCount == MIP_LEVELS_ALL || MipLevelCount > availableMipLevels)
	{
		MipLevelCount = availableMipLevels;
	}

	try
	{
		TargaFile = new Targa;
	}
	catch (...)
	{
		TargaFile = nullptr;
	}
	if (TargaFile == nullptr || TARGA_ERROR_HANDLER(TargaFile->Open(Filename, TGA_READMODE), Filename))
	{
		delete TargaFile;
		TargaFile = nullptr;
		return false;
	}

	TargaFile->Header.ImageDescriptor ^= TGAIDF_YORIGIN;
	Get_WW3D_Format(SourceFormat, SourceBytesPerPixel, *TargaFile);
	TargaFile->SetPalette(TargaPalette);
	if (SourceFormat == WW3D_FORMAT_UNKNOWN ||
		TARGA_ERROR_HANDLER(TargaFile->Load(Filename, TGAF_IMAGE, false), Filename))
	{
		TargaFile->Close();
		delete TargaFile;
		TargaFile = nullptr;
		return false;
	}
	TargaFile->Close();

	if (!Allocate_Prepared_Surfaces())
	{
		delete TargaFile;
		TargaFile = nullptr;
		return false;
	}

	return true;
}


bool TextureLoadTaskClass::Allocate_Prepared_Surfaces()
{
	unsigned int width = Width;
	unsigned int height = Height;

	for (unsigned int level = 0; level < MipLevelCount; ++level)
	{
		if (!PreparedSurface[level].allocate(Format, width, height, 1))
		{
			Release_Prepared_Surfaces();
			return false;
		}
		width = max(width >> 1, 1u);
		height = max(height >> 1, 1u);
	}
	return true;
}


bool TextureLoadTaskClass::Create_Texture_Handle()
{
	// Native publication does not create an intermediate graphics object.
	LoadedTextureHandle = nullptr;
	return false;
}


static bool Build_Upload_Layout(const TextureMipLayout& source, size_t rowPitch,
	size_t slicePitch, unsigned depth, TextureMipLayout& destination)
{
	if (rowPitch < source.rowPitch || rowPitch > (size_t)-1 / source.rowCount)
	{
		return false;
	}

	const size_t minimumSlicePitch = rowPitch * source.rowCount;
	if (slicePitch == 0)
	{
		slicePitch = minimumSlicePitch;
	}
	if (slicePitch < minimumSlicePitch || (depth != 0 && slicePitch > (size_t)-1 / depth))
	{
		return false;
	}

	destination.rowPitch = rowPitch;
	destination.rowCount = source.rowCount;
	destination.slicePitch = slicePitch;
	destination.dataSize = slicePitch * depth;
	return true;
}


bool TextureLoadTaskClass::Upload_Prepared_Surfaces()
{
	// Prepared buffers are handed directly to NativeW3DSampledTextureUpload.
	return false;
}


bool TextureLoadTaskClass::Publish_Native_Prepared_Texture()
{
	if (Texture == nullptr || MipLevelCount == 0 ||
		MipLevelCount > MIP_LEVELS_MAX) return false;
	rts::render::NativeW3DSampledTextureMipView views[MIP_LEVELS_MAX];
	for (unsigned int level = 0; level < MipLevelCount; ++level)
	{
		const TextureMipBuffer &source =
			ResourceResult != nullptr ?
				static_cast<TextureResourceDecode *>(ResourceResult)->Surface[0][level] :
			PreparedSurface[level];
		if (source.data() == nullptr || source.layout().dataSize == 0 ||
			source.layout().rowPitch == 0) return false;
		views[level].data = source.data();
		views[level].dataSize = source.layout().dataSize;
		views[level].rowPitch = source.layout().rowPitch;
	}
	rts::render::NativeW3DSampledTextureUpload upload;
	if (!upload.Prepare(Format, Width, Height, MipLevelCount, 1, views,
		MipLevelCount)) return false;
	return Texture->Apply_Native_Texture(upload.Descriptor(),
		upload.Subresources(), upload.SubresourceCount(), Format, true);
}


void TextureLoadTaskClass::Release_Prepared_Surfaces()
{
	delete static_cast<TextureResourceDecode*>(ResourceResult);
	ResourceResult = nullptr;
	for (unsigned int level = 0; level < MIP_LEVELS_MAX; ++level)
	{
		PreparedSurface[level].reset();
	}
	Release_Prepare_Memory_Reservation();
}


bool TextureLoadTaskClass::Lock_Surfaces()
{
	// The native path never exposes lockable graphics surfaces.
	return false;
}


void TextureLoadTaskClass::Unlock_Surfaces()
{
	for (unsigned int i = 0; i < MIP_LEVELS_MAX; ++i)
	{
		LockedSurfacePtr[i] = nullptr;
		LockedSurfacePitch[i] = 0;
	}
}


bool TextureLoadTaskClass::Load_Compressed_Mipmap()
{
	if (DDSFile == nullptr)
	{
		return false;
	}
	DDSFileClass& dds_file = *DDSFile;

	// regular 2d texture
	unsigned int width = Get_Width();
	unsigned int height = Get_Height();

	for (unsigned int level = 0; level < Get_Mip_Level_Count(); ++level)
	{
		WWASSERT(width >= MinTextureDim && height >= MinTextureDim);

		dds_file.Copy_Level_To_Surface
		(
			level,
			Get_Format(),
			width,
			height,
			Get_Locked_Surface_Ptr(level),
			Get_Locked_Surface_Pitch(level),
			HSVShift
		);

		ReduceTextureMipDimensions(width, height);
	}

	return true;
}


bool TextureLoadTaskClass::Load_Uncompressed_Mipmap()
{
	if (!Get_Mip_Level_Count() || TargaFile == nullptr)
	{
		return false;
	}

	Targa& targa = *TargaFile;
	WW3DFormat src_format = SourceFormat;
	unsigned int src_bpp = SourceBytesPerPixel;

	unsigned int src_width	= targa.Header.Width;
	unsigned int src_height	= targa.Header.Height;
	unsigned int width		= Get_Width();
	unsigned int height		= Get_Height();

	unsigned char * src_surface			= (unsigned char*)targa.GetImage();
	unsigned char * converted_surface	= nullptr;

	// No paletted format allowed when generating mipmaps
	Vector3 hsv_shift=HSVShift;
	if (	src_format	== WW3D_FORMAT_A1R5G5B5
		|| src_format	== WW3D_FORMAT_R5G6B5
		|| src_format	== WW3D_FORMAT_A4R4G4B4
		||	src_format	== WW3D_FORMAT_P8
		|| src_format	== WW3D_FORMAT_L8
		|| src_width	!= width
		|| src_height	!= height) {

		try
		{
			converted_surface = new unsigned char[width*height*4];
		}
		catch (...)
		{
			converted_surface = nullptr;
		}
		if (converted_surface == nullptr)
		{
			return false;
		}
		BitmapHandlerClass::Copy_Image(
			converted_surface,
			width,
			height,
			width*4,
			WW3D_FORMAT_A8R8G8B8,	//dest_format,
			src_surface,
			src_width,
			src_height,
			src_width*src_bpp,
			src_format,
			(unsigned char*)targa.GetPalette(),
			targa.Header.CMapDepth>>3,
			false,
			hsv_shift);
		hsv_shift=Vector3(0.0f,0.0f,0.0f);

		src_surface	= converted_surface;
		src_format	= WW3D_FORMAT_A8R8G8B8;	//dest_format;
		src_width	= width;
		src_height	= height;
		src_bpp		= Get_Bytes_Per_Pixel(src_format);
	}

	unsigned src_pitch = src_width * src_bpp;

	if (Reduction)
	{	//texture needs to be reduced so allocate storage for full-sized version.
		unsigned char * destination_surface = nullptr;
		try
		{
			destination_surface = new unsigned char[width*height*4];
		}
		catch (...)
		{
			destination_surface = nullptr;
		}
		if (destination_surface == nullptr)
		{
			delete[] converted_surface;
			return false;
		}
		//generate upper mip-levels that will be dropped in final texture
		for (unsigned int level = 0; level < Reduction; ++level) {
		BitmapHandlerClass::Copy_Image(
			(unsigned char *)destination_surface,
			width,
			height,
			src_pitch,
			Get_Format(),
			src_surface,
			src_width,
			src_height,
			src_pitch,
			src_format,
			nullptr,
			0,
			true,
			hsv_shift);

			ReduceTextureMipDimensions(width, height);
			ReduceTextureMipDimensions(src_width, src_height);
		}
		delete [] destination_surface;
	}

	for (unsigned int level = 0; level < Get_Mip_Level_Count(); ++level) {
		WWASSERT(Get_Locked_Surface_Ptr(level));
		BitmapHandlerClass::Copy_Image(
			Get_Locked_Surface_Ptr(level),
			width,
			height,
			Get_Locked_Surface_Pitch(level),
			Get_Format(),
			src_surface,
			src_width,
			src_height,
			src_pitch,
			src_format,
			nullptr,
			0,
			true,
			hsv_shift);
		hsv_shift=Vector3(0.0f,0.0f,0.0f);

		ReduceTextureMipDimensions(width, height);
		ReduceTextureMipDimensions(src_width, src_height);
	}

	delete[] converted_surface;

	return true;
}


unsigned char * TextureLoadTaskClass::Get_Locked_Surface_Ptr(unsigned int level)
{
	WWASSERT(level<MipLevelCount);
	WWASSERT(PreparedSurface[level].data());
	return PreparedSurface[level].data();
}

// ----------------------------------------------------------------------------
//
// Return locked surface pitch (in bytes) at a specific level. The call will
// assert if level is greater or equal to the number of mip levels or if the
// requested level has not been locked.
//
// ----------------------------------------------------------------------------

unsigned int TextureLoadTaskClass::Get_Locked_Surface_Pitch(unsigned int level) const
{
	WWASSERT(level<MipLevelCount);
	WWASSERT(PreparedSurface[level].layout().rowPitch);
	return (unsigned int)PreparedSurface[level].layout().rowPitch;
}





// CubeTextureLoadTaskClass
CubeTextureLoadTaskClass::CubeTextureLoadTaskClass()
:	TextureLoadTaskClass()
{
	// because texture load tasks are pooled, the constructor and destructor
	// don't need to do much. The work of attaching a task to a texture is
	// is done by Init() and Deinit().

	for (int f=0;f<6;f++)
	{
		for (int i = 0; i < MIP_LEVELS_MAX; ++i)
		{
			LockedCubeSurfacePtr[f][i]		= nullptr;
			LockedCubeSurfacePitch[f][i]	= 0;
		}
	}
}

void CubeTextureLoadTaskClass::Destroy()
{
	// detach the task from its texture, and return to free pool.
	Deinit();
	_CubeTexLoadFreeList.Push_Front(this);
}


void CubeTextureLoadTaskClass::Init(TextureBaseClass* tc, TaskType type, PriorityType priority)
{
	WWASSERT(tc);

	// NOTE: we must be in the main thread to avoid corrupting the texture's refcount.
	WWASSERT(TextureLoader::Is_DX8_Thread());
	REF_PTR_SET(Texture, tc);

	// Make sure texture has a filename.
	WWASSERT(!Texture->Get_Full_Path().Is_Empty());

	Type				= type;
	Priority			= priority;
	State				= STATE_NONE;

	LoadedTextureHandle		= nullptr;

	CubeTextureClass* tex=Texture->As_CubeTextureClass();

	if (tex)
	{
		Format			= tex->Get_Texture_Format(); // don't assume format yet KM
	}
	else
	{
		Format			= WW3D_FORMAT_UNKNOWN;
	}

	Width				= 0;
	Height			= 0;
	MipLevelCount	= Texture->MipLevelCount;
	Reduction		= Texture->Get_Reduction();
	HSVShift			= Texture->Get_HSV_Shift();
	SourceFormat		= WW3D_FORMAT_UNKNOWN;
	SourceBytesPerPixel = 0;
	CompressionAllowed = Texture->Is_Compression_Allowed();
	LoadSucceeded	= false;
	DDSFile			= nullptr;
	TargaFile		= nullptr;
	PrepareRuntimeTask = nullptr;
	WWASSERT(PrepareMemoryReservation == 0);
	WWASSERT(ResourceRequest == nullptr && ResourceResult == nullptr);
	ResourceTriedDDS = false;
	strlcpy(Filename, Texture->Get_Full_Path().str(), ARRAY_SIZE(Filename));


	for (int f=0; f<6; f++)
	{
		for (int i = 0; i < MIP_LEVELS_MAX; ++i)
		{
			LockedCubeSurfacePtr[f][i]		= nullptr;
			LockedCubeSurfacePitch[f][i]	= 0;
		}
	}

	switch (Type)
	{
	case TASK_THUMBNAIL:
		WWASSERT(Texture->ThumbnailLoadTask == nullptr);
		Texture->ThumbnailLoadTask = this;
		break;

	case TASK_LOAD:
		WWASSERT(Texture->TextureLoadTask == nullptr);
		Texture->TextureLoadTask = this;
		break;
	}
}


void CubeTextureLoadTaskClass::Deinit()
{
	// task should not be on any list when it is being detached from texture.
	WWASSERT(Next == nullptr);
	WWASSERT(Prev == nullptr);

	WWASSERT(LoadedTextureHandle == nullptr);
	Release_Prepared_Surfaces();
	delete DDSFile;
	DDSFile = nullptr;
	delete TargaFile;
	TargaFile = nullptr;
	PrepareRuntimeTask = nullptr;
	Release_Prepare_Memory_Reservation();

	for (int f=0; f<6; f++)
	{
		for (int i = 0; i < MIP_LEVELS_MAX; ++i)
		{
			WWASSERT(LockedCubeSurfacePtr[f][i] == nullptr);
		}
	}

	if (Texture)
	{
		switch (Type)
		{
			case TASK_THUMBNAIL:
				WWASSERT(Texture->ThumbnailLoadTask == this);
				Texture->ThumbnailLoadTask = nullptr;
				break;

			case TASK_LOAD:
				WWASSERT(Texture->TextureLoadTask == this);
				Texture->TextureLoadTask = nullptr;
				break;
		}

		// NOTE: we must be in main thread to avoid corrupting Texture's refcount.
		WWASSERT(TextureLoader::Is_DX8_Thread());
		REF_PTR_RELEASE(Texture);
	}
}

bool CubeTextureLoadTaskClass::Lock_Surfaces()
{
	// The native path publishes cube faces from prepared CPU memory.
	return false;
}


void CubeTextureLoadTaskClass::Unlock_Surfaces()
{
	for (unsigned int face = 0; face < 6; ++face)
	{
		for (unsigned int level = 0; level < MIP_LEVELS_MAX; ++level)
		{
			LockedCubeSurfacePtr[face][level] = nullptr;
			LockedCubeSurfacePitch[face][level] = 0;
		}
	}
}


bool CubeTextureLoadTaskClass::Begin_Compressed_Load()
{
	unsigned orig_width,orig_height,orig_depth,orig_mip_count,orig_reduction;
	WW3DFormat orig_format;
	if (!Get_Texture_Information
		  (
				Filename,
				orig_reduction,
				orig_width,
				orig_height,
				orig_depth,
				orig_format,
				orig_mip_count,
				true
		  )
		)
	{
		return false;
	}

	Format = Get_Valid_Texture_Format(orig_format, Texture->Is_Compression_Allowed());

	Reduction = orig_reduction;
	Validate_Reduction(Texture, Reduction, orig_mip_count);

	Width = orig_width;
	Height = orig_height;
	Apply_Dim_Reduction(Width, Height, Reduction, orig_mip_count);

	Apply_Mip_Reduction(MipLevelCount, Reduction, Width, Height, orig_mip_count);

	try
	{
		DDSFile = new DDSFileClass(Filename, Reduction);
	}
	catch (...)
	{
		DDSFile = nullptr;
	}
	if (DDSFile == nullptr || !DDSFile->Is_Available() || !DDSFile->Load())
	{
		delete DDSFile;
		DDSFile = nullptr;
		return false;
	}

	if (!Allocate_Prepared_Surfaces())
	{
		delete DDSFile;
		DDSFile = nullptr;
		return false;
	}

	return true;
}

bool CubeTextureLoadTaskClass::Begin_Uncompressed_Load()
{
	// The legacy loader has no defined TGA-to-cubemap source layout.
	return false;
}

bool CubeTextureLoadTaskClass::Load_Compressed_Mipmap()
{
	if (DDSFile == nullptr)
	{
		return false;
	}
	DDSFileClass& dds_file = *DDSFile;

	// load cube map faces
	for (unsigned int face=0; face<6; face++)
	{
		unsigned int width = Get_Width();
		unsigned int height = Get_Height();

		for (unsigned int level=0; level<Get_Mip_Level_Count(); level++)
		{
			WWASSERT(width >= MinTextureDim && height >= MinTextureDim);

			// get cube map surface
			dds_file.Copy_CubeMap_Level_To_Surface
			(
				face,
				level,
				Get_Format(),
				width,
				height,
				Get_Locked_CubeMap_Surface_Pointer(face,level),
				Get_Locked_CubeMap_Surface_Pitch(face,level),
				HSVShift
			);

			ReduceTextureMipDimensions(width, height);
		}
	}

	return true;
}


bool CubeTextureLoadTaskClass::Allocate_Prepared_Surfaces()
{
	for (unsigned int face = 0; face < 6; ++face)
	{
		unsigned int width = Width;
		unsigned int height = Height;
		for (unsigned int level = 0; level < MipLevelCount; ++level)
		{
			if (!PreparedCubeSurface[face][level].allocate(Format, width, height, 1))
			{
				Release_Prepared_Surfaces();
				return false;
			}
			width = max(width >> 1, 1u);
			height = max(height >> 1, 1u);
		}
	}
	return true;
}

size_t CubeTextureLoadTaskClass::Get_Prepare_Memory_Byte_Count() const
{
	size_t total = 0;
	unsigned int face;
	unsigned int level;
	for (face = 0; face < 6; ++face)
	{
		for (level = 0; level < MIP_LEVELS_MAX; ++level)
		{
			if (!Add_Prepare_Memory_Bytes(total,
				PreparedCubeSurface[face][level].layout().dataSize))
			{
				return (size_t)-1;
			}
		}
	}

	if (DDSFile != nullptr)
	{
		if (!Add_Prepare_Memory_Bytes(total, DDSFile->Get_Retained_Memory_Size()))
		{
			return (size_t)-1;
		}
	}

	return total;
}


bool CubeTextureLoadTaskClass::Create_Texture_Handle()
{
	LoadedTextureHandle = nullptr;
	return false;
}


bool CubeTextureLoadTaskClass::Upload_Prepared_Surfaces()
{
	// Native publication consumes prepared face/mip buffers directly.
	return false;
}


bool CubeTextureLoadTaskClass::Publish_Native_Prepared_Texture()
{
	if (Texture == nullptr || Width == 0 || Width != Height ||
		MipLevelCount == 0 || MipLevelCount > MIP_LEVELS_MAX) return false;
	rts::render::NativeW3DSampledTextureMipView views[6 * MIP_LEVELS_MAX];
	for (unsigned int face = 0; face < 6; ++face)
	{
		for (unsigned int level = 0; level < MipLevelCount; ++level)
		{
			const unsigned int index = face * MipLevelCount + level;
			const TextureMipBuffer &source =
				ResourceResult != nullptr ?
					static_cast<TextureResourceDecode *>(ResourceResult)->Surface[face][level] :
				PreparedCubeSurface[face][level];
			if (source.data() == nullptr || source.layout().dataSize == 0 ||
				source.layout().rowPitch == 0) return false;
			views[index].data = source.data();
			views[index].dataSize = source.layout().dataSize;
			views[index].rowPitch = source.layout().rowPitch;
		}
	}
	rts::render::NativeW3DSampledTextureUpload upload;
	if (!upload.Prepare(Format, Width, Height, MipLevelCount, 6, views,
		6 * MipLevelCount)) return false;
	return Texture->Apply_Native_Texture(upload.Descriptor(),
		upload.Subresources(), upload.SubresourceCount(), Format, true);
}


void CubeTextureLoadTaskClass::Release_Prepared_Surfaces()
{
	delete static_cast<TextureResourceDecode*>(ResourceResult);
	ResourceResult = nullptr;
	for (unsigned int face = 0; face < 6; ++face)
	{
		for (unsigned int level = 0; level < MIP_LEVELS_MAX; ++level)
		{
			PreparedCubeSurface[face][level].reset();
		}
	}
	Release_Prepare_Memory_Reservation();
}

unsigned char*	CubeTextureLoadTaskClass::Get_Locked_CubeMap_Surface_Pointer(unsigned int face, unsigned int level)
{
	WWASSERT(face<6 && level<MipLevelCount);
	WWASSERT(PreparedCubeSurface[face][level].data());
	return PreparedCubeSurface[face][level].data();
}

unsigned int CubeTextureLoadTaskClass::Get_Locked_CubeMap_Surface_Pitch(unsigned int face, unsigned int level) const
{
	WWASSERT(face<6 && level<MipLevelCount);
	WWASSERT(PreparedCubeSurface[face][level].layout().rowPitch);
	return (unsigned int)PreparedCubeSurface[face][level].layout().rowPitch;
}







// VolumeTextureLoadTaskClass
void VolumeTextureLoadTaskClass::Destroy()
{
	// detach the task from its texture, and return to free pool.
	Deinit();
	_VolTexLoadFreeList.Push_Front(this);
}

bool VolumeTextureLoadTaskClass::Begin_Compressed_Load()
{
	// Neither shipped DDS backend exposes volume-level source memory. Reject
	// before reading the source, allocating prepared slices, or submitting a
	// worker task so the existing owner-thread missing-texture fallback applies.
	return false;
}

bool VolumeTextureLoadTaskClass::Begin_Uncompressed_Load()
{
	// The legacy loader has no defined TGA-to-volume source layout.
	return false;
}

bool VolumeTextureLoadTaskClass::Publish_Native_Prepared_Texture()
{
	// The neutral renderer does not expose a volume-texture descriptor. Never
	// reinterpret a volume load as a sampleable 2D texture on the x64 product.
	return false;
}
