# Multicore Stages 0-3 Design

## Purpose

Establish a compatibility-first multicore foundation for Generals and Zero Hour without changing authoritative simulation behavior. The first four stages prove the execution model on non-authoritative screenshot work before any logic, network, replay, save/load, UI, or Direct3D ownership is changed.

## Compatibility contract

- The default release remains compatible with Generals 1.08 and Zero Hour 1.04 replay, CRC, save/load, and lockstep behavior.
- `-jobs` continues to mean independent replay worker processes. It never controls in-process game workers.
- Logic, RNG, MessageStream, Network frame commits, Recorder, Xfer/CRC, save/load, UI, and all D3D8/WW3D calls remain on their existing owner threads.
- New code is C++98-compatible in the public and VC6-compiled paths. It uses neither `std::thread`, `std::atomic`, lambdas, exceptions for task control flow, nor `ThreadClass`.
- Every worker receives owned or immutable CPU data only. It cannot dereference `The*` globals, a D3D object, live game memory, or a UI object.
- Each stage is independently committed and automatically validated. The user performs the documented game checks after the Stage 0-3 bundle; later stages pause for manual acceptance.

## Chosen approach

The first vertical slice uses compressed screenshots.

The existing path already copies the DX8 back buffer to owned CPU memory on the render owner, converts it in a background thread, writes the image, and returns a main-thread UI notification. Replacing the detached thread with a bounded runtime, then splitting only the CPU row conversion, proves task ownership, backpressure, lifecycle, and multi-core output without making gameplay state concurrent.

Texture loading, map-cache work, render threading, pathfinding, partitioning, and simulation update modules are intentionally excluded. Their current implementations use live D3D surfaces or mutable global state and need later snapshot boundaries.

## Stage 0: serial-frame observability and ownership guardrails

### Outcome

A playable, behaviorally identical build exposes the serial frame phases in Tracy and asserts accidental execution of authoritative work from a foreign thread in existing crash-enabled diagnostic builds.

### Components

1. Add `GameThreadOwnership` in shared `Core/GameEngine`.
   - `AttachCurrentThread()` records the Windows thread ID.
   - `DetachCurrentThread()` clears it on the owning thread.
   - `AssertCurrentThread(const char* boundary)` is inert before attachment and on `_UNIX`.
   - `ASSERT_GAME_THREAD(boundary)` expands only with `DEBUG_CRASHING`; it is a no-op in retail builds.
   - The helper allocates nothing, locks nothing, starts no thread, and has no serialized state.
2. Attach before `TheGameEngine->init()` and detach after game engine destruction in both Generals and Zero Hour `GameMain.cpp` implementations.
3. Add owner assertions at these boundaries only:
   - `GameEngine::init`, `reset`, `update`, and `execute`;
   - `GameLogic::update`;
   - `GameClient::update`;
   - `Win32GameEngine::serviceWindowsOS`.
4. Add non-invasive Tracy zones around the existing `GameEngine::update` phases:
   - `Engine.Update`;
   - `Engine.Update.Radar`;
   - `Engine.Update.Audio`;
   - `Engine.Update.Client`;
   - `Engine.Update.MessageStream`;
   - `Engine.Update.Network`;
   - `Engine.Update.GameLogic`;
   - `Engine.Update.ClientStep`;
   - `Platform.Win32Messages`;
   - `Engine.FramePacer`.
5. Extend `TESTING.md` with the exact Stage 0 replay, CRC, profiling, and manual smoke checks.

### Explicit exclusions

Stage 0 adds no worker count, task runtime, queue, lock, game setting, command-line option, `GlobalData` field, CMake preset, replay format, or CI replay behavior.

### Acceptance

- Existing VC6 replay compatibility checks remain byte/CRC compatible.
- `win32-profile` compiles and exposes the named zones.
- `vc6-releaselog` catches only genuine foreign-thread calls at the guarded boundaries.
- Both titles launch, play a skirmish, save/load, take screenshots, and quit without a new assertion.

## Stage 1: portable bounded task runtime

### Outcome

A standalone, testable Core library executes finite non-authoritative tasks with bounded admission and cooperative lifetime management. It has no game consumer yet.

### Public API

`Core/Libraries/Include/Lib/TaskRuntime.h` defines the following C++98 contract.

```cpp
namespace rts
{
class Task
{
public:
	virtual ~Task();
	virtual void execute() = 0;

private:
	Task(const Task &);
	Task &operator=(const Task &);
};

class TaskRuntime
{
public:
	TaskRuntime();
	~TaskRuntime();

	bool start(unsigned workerCount, unsigned queueCapacity);
	bool trySubmit(Task *task);
	bool trySubmitBatch(Task *const *tasks, unsigned taskCount);
	void waitUntilIdle();
	void shutdown();

	bool isRunning() const;
	unsigned workerCount() const;
	unsigned pendingTaskCount() const;

private:
	TaskRuntime(const TaskRuntime &);
	TaskRuntime &operator=(const TaskRuntime &);

	struct State;
	State *m_state;
};
}
```

Ownership rules:

- `trySubmit` transfers the task to the runtime only when it returns `true`.
- `trySubmitBatch` transfers all tasks only when it returns `true`; a rejection transfers none.
- The runtime deletes every accepted task after `execute()` returns.
- Rejected tasks remain caller-owned and must be deleted by the caller.
- FIFO dequeue is guaranteed; completion order is intentionally unspecified with more than one worker.
- `shutdown()` rejects future work, drains accepted work, joins workers, and is idempotent. It is called only by the owner, never by a task.
- A task is finite, does not throw across `execute()`, and does not wait for tasks it created.

### Implementation

- The implementation is a PIMPL in `Core/Libraries/Source/TaskRuntime/TaskRuntime.cpp` so platform types are absent from the public header.
- Win32, VC6, modern MSVC, and MinGW use `_beginthreadex`, `CRITICAL_SECTION`, manual-reset work/idle events, `WaitForSingleObject`, and joined/closed handles.
- Unix uses `pthread_create`, `pthread_join`, `pthread_mutex_t`, and `pthread_cond_t`; CMake links `Threads::Threads`.
- Both implementations use a bounded `std::deque<Task*>` and never busy-spin, detach workers, or force-terminate a thread.
- The target is `core_task_runtime`, separate from WWLib. `corei_always` links it after `Core/Libraries` has defined the target, so every regular game build compiles it without instantiating it.

### Tests

Add a `core_task_runtime_tests` console target under Core extras and register it with CTest. It verifies invalid start parameters, exact-once execution, single-worker FIFO behavior, atomic batch admission, queue rejection, drain shutdown, reuse after shutdown, and destructor drain/join. The test uses explicit platform events or condition variables for blocking; it never uses sleeps as synchronization.

The reusable CI build workflow runs CTest after every Core-extras build with the correct single- or multi-config arguments.

## Stage 2: managed screenshot encode and write

### Outcome

`W3D_TakeCompressedScreenshot` stops creating an unbounded detached Windows thread for every image. Accepted screenshots run in the bounded Stage 1 runtime; DX8 capture and UI display remain owner-thread-only.

### Data flow

1. The existing render owner reads and copies the DX8 surface exactly as it does now.
2. The main thread constructs a self-contained screenshot request: copied pixels, width, height, pitch, source format, image format, JPEG quality, path, and leaf name.
3. It submits one `rts::Task` to the private screenshot task service.
4. The task serially converts pixels to RGB, writes through `stbi_write_jpg` or `stbi_write_png`, and pushes a plain completion record to the existing MPSC completion queue.
5. `W3D_UpdateScreenshotMessages` remains the sole consumer that talks to `TheInGameUI`.

### Lifecycle and backpressure

- The service starts lazily with `min(2, logicalProcessorCount)` workers and a fixed queue capacity of four. `logicalProcessorCount` is a private Win32 helper using `GetSystemInfo()` and falls back to one when the API reports zero.
- If bounded admission rejects a request, the copied request is released immediately and a debug log records the rejection; the game frame never blocks.
- `W3D_ShutdownScreenshotTasks()` shuts down and drains the service before each game-specific `W3DDisplay` destructor tears down UI, assets, or DX8. It drops queued completion messages during final teardown instead of dereferencing a dying UI.
- No worker makes D3D calls or reads any `The*` global.

### Testability

Extract the CPU pixel conversion into a small D3D-free screenshot codec component. A Core extra test validates known ARGB32 and RGB565 samples with padded pitch and odd dimensions. The live game path continues to own DX8 capture, so codec tests need no game data or graphics device.

## Stage 3: bounded parallel screenshot pixel conversion

### Outcome

Large screenshots use more than one CPU worker for conversion while producing exactly the same RGB data as Stage 2's serial conversion. Compression and file I/O occur only after all conversion rows complete.

### Data flow

1. The main thread captures/copies pixels and creates a `ScreenshotBatch` that owns source bytes, destination RGB bytes, output metadata, and a remaining-row-task count.
2. `ConvertScreenshotRows(source, yBegin, yEnd, destination)` converts a non-overlapping half-open row range without global state.
3. For a multi-core host and a sufficiently large image, the owner creates `min(workerCount * 2, ceil(height / 64))` stripe tasks. A single task remains the path for small images and one worker.
4. The runtime admits the full stripe set with `trySubmitBatch`, guaranteeing all-or-none ownership.
5. Each task writes only its own rows and decrements the batch count with an interlocked operation. The task that observes zero encodes the completed RGB buffer and posts the same main-thread completion record used in Stage 2.
6. The final task releases the batch after image writing; no worker waits for child work and the main thread never waits for conversion.

### Tests and telemetry

- The codec test compares serial, one-worker, and multi-worker conversion bytes for ARGB32/RGB565, odd sizes, padded pitch, and every stripe boundary used by the implementation.
- Worker profile zones distinguish `Screenshot.Convert` from `Screenshot.Encode` in Tracy.
- Manual verification uses 1080p/4K captures, bursts, map transitions, exit, alt-tab, and device reset. It checks files, colors, seams, responsiveness, and safe shutdown.

## Validation matrix

For every code stage:

1. Run its focused console/CTest checks first.
2. Build the affected modern Win32 configuration.
3. Build the VC6 configuration that compiles the changed shared Core code.
4. Run the existing optimized VC6 Zero Hour replay command with `-jobs 4 -headless -replay` when the necessary game data is available.
5. Inspect `git diff --check`, the stage diff, and final status before committing.

At the end of Stage 3:

- run all available focused tests and final builds sequentially;
- run repeated subagent code-review and fix rounds for Critical and Important findings;
- open an unmerged pull request with the compatibility contract, automated evidence, known manual-test requirements, and AI-generated-code disclosure required by `CONTRIBUTING.md`.

## Manual game-test checklist

The post-bundle handoff contains distinct Stage 0-3 checklists. At minimum, test Zero Hour first and Generals second; cover skirmish, pause/resume, save/load, screenshots, map return, alt-tab/minimize/restore, and exit. Run replay validation from `TESTING.md` in an optimized VC6 build. Use a profile build for the Stage 0 zones and Stage 3 conversion-worker trace.

## Deferred work

Stage 4 begins only after the user accepts the Stage 0-3 bundle. It will be designed as a separate, one-stage-at-a-time project and will not be implied by any generic task-runtime API.
