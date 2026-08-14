# Multicore Stages 0-3 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Deliver the first compatibility-preserving multicore vertical slice: serial ownership diagnostics, a portable bounded worker runtime, managed screenshot encoding, and parallel screenshot pixel conversion.

**Architecture:** Existing game execution remains a single authoritative owner. A new C++98 Core task runtime accepts only finite tasks over owned CPU data. Screenshot capture remains on the DX8 owner, while RGB conversion, image writing, and completion delivery use that runtime without reading live engine state.

**Tech Stack:** C++98, VC6, Win32 `_beginthreadex`/synchronization, pthreads on Unix, CMake/CTest, existing Tracy macros, Direct3D 8, stb_image_write.

## Global Constraints

- Preserve Generals 1.08 and Zero Hour 1.04 replay, CRC, save/load, lockstep, and default gameplay behavior.
- `-jobs` remains replay-process parallelism; do not add or reuse an in-process worker command-line switch.
- Keep Logic, RNG, MessageStream, Network frame commits, Recorder, Xfer/CRC, save/load, UI, DX8, and WW3D on their existing owner threads.
- Build public and VC6-compiled code as C++98: no `std::thread`, `std::atomic`, lambda, `std::function`, or exception-based task control flow.
- Do not use or modify `ThreadClass`, `MutexClass`, `CriticalSectionClass`, or the existing MPSC queue as the generic worker implementation.
- Worker code receives owned or immutable CPU data only and cannot dereference `The*`, D3D, UI, or live game-memory pointers.
- Every accepted `rts::Task` is deleted by `TaskRuntime` after `execute()` returns; a rejected task remains caller-owned.
- Keep each stage independently committed with a conventional-commit message after its fresh checks succeed.
- Run heavyweight configure/build/test commands sequentially; do not run compilers, replay jobs, or game instances concurrently.

---

## File map

| File | Responsibility |
|---|---|
| `Core/GameEngine/Include/Common/GameThreadOwnership.h` | C++98 diagnostic owner-thread API and macro. |
| `Core/GameEngine/Source/Common/GameThreadOwnership.cpp` | Win32 thread-ID implementation with inert Unix behavior. |
| `Core/Libraries/Include/Lib/TaskRuntime.h` | Public C++98 task and runtime contract. |
| `Core/Libraries/Source/TaskRuntime/TaskRuntime.cpp` | PIMPL-backed Win32 and pthread bounded FIFO runtime. |
| `Core/Tools/TaskRuntimeTest/TaskRuntimeTest.cpp` | CTest console coverage for runtime lifetime and queue behavior. |
| `Core/GameEngineDevice/Include/W3DDevice/GameClient/W3DScreenshotCodec.h` | D3D-free source-pixel descriptor and row conversion declaration. |
| `Core/GameEngineDevice/Source/W3DDevice/GameClient/W3DScreenshotCodec.cpp` | ARGB32/RGB565-to-RGB conversion implementation. |
| `Core/Tools/ScreenshotCodecTest/ScreenshotCodecTest.cpp` | Byte-exact serial and parallel screenshot conversion tests. |
| `Core/GameEngineDevice/Source/W3DDevice/GameClient/W3DScreenshot.cpp` | Private screenshot task service, task/batch ownership, encode/write, and main-thread completion delivery. |
| Both game `W3DDisplay.cpp` files | Explicit screenshot service shutdown before display/UI/DX8 destruction. |
| CMake and CI files listed below | Target registration, test registration, and test execution. |
| `TESTING.md` | Stage 0-3 manual and replay validation instructions. |

## Task 1: Add Stage 0 owner diagnostics

**Files:**
- Create: `Core/GameEngine/Include/Common/GameThreadOwnership.h`
- Create: `Core/GameEngine/Source/Common/GameThreadOwnership.cpp`
- Modify: `Core/GameEngine/CMakeLists.txt`
- Modify: `Generals/Code/GameEngine/Source/Common/GameMain.cpp`
- Modify: `GeneralsMD/Code/GameEngine/Source/Common/GameMain.cpp`
- Modify: `Generals/Code/GameEngine/Source/Common/GameEngine.cpp`
- Modify: `GeneralsMD/Code/GameEngine/Source/Common/GameEngine.cpp`
- Modify: `Generals/Code/GameEngine/Source/GameLogic/System/GameLogic.cpp`
- Modify: `GeneralsMD/Code/GameEngine/Source/GameLogic/System/GameLogic.cpp`
- Modify: `Generals/Code/GameEngine/Source/GameClient/GameClient.cpp`
- Modify: `GeneralsMD/Code/GameEngine/Source/GameClient/GameClient.cpp`
- Modify: `Core/GameEngineDevice/Source/Win32Device/Common/Win32GameEngine.cpp`
- Modify: `TESTING.md`

**Interfaces:**
- Produces `GameThreadOwnership::AttachCurrentThread`, `DetachCurrentThread`, `IsAttached`, `IsCurrentThread`, and `AssertCurrentThread` for use by the guarded frame boundaries.
- Produces `ASSERT_GAME_THREAD(boundary)`, which is compiled out unless `DEBUG_CRASHING` is defined.

- [ ] **Step 1: Add the header, an empty implementation translation unit, and source registration**

```cpp
class GameThreadOwnership
{
public:
	static void AttachCurrentThread();
	static void DetachCurrentThread();
	static bool IsAttached();
	static bool IsCurrentThread();
	static void AssertCurrentThread(const char *boundary);
};

#if defined(DEBUG_CRASHING)
#define ASSERT_GAME_THREAD(boundary) GameThreadOwnership::AssertCurrentThread(boundary)
#else
#define ASSERT_GAME_THREAD(boundary) ((void)0)
#endif
```

- [ ] **Step 2: Attach and detach from both game-main lifetimes before implementation**

Place `AttachCurrentThread()` before the game engine initialization path and `DetachCurrentThread()` after engine destruction in both game `GameMain.cpp` files. Do not move existing initialization or teardown statements.

Run: `cmake --preset win32-debug -DRTS_BUILD_ZEROHOUR=ON -DRTS_BUILD_GENERALS=OFF`

Then run: `cmake --build build/win32-debug --config Debug`

Expected: configuration succeeds and linking fails because the declared ownership methods have no definitions.

- [ ] **Step 3: Implement C++98-safe ownership tracking**

```cpp
void GameThreadOwnership::AttachCurrentThread();
void GameThreadOwnership::DetachCurrentThread();
bool GameThreadOwnership::IsCurrentThread();
void GameThreadOwnership::AssertCurrentThread(const char *boundary);
```

On Windows, keep one private `DWORD` thread ID, initialize it only from `AttachCurrentThread`, and assert only when it is nonzero and differs from `GetCurrentThreadId()`. On `_UNIX`, retain inert implementations. Do not allocate, lock, or invoke `ThreadClass`.

- [ ] **Step 4: Guard the authoritative boundaries and add non-invasive profile scopes**

At each listed entry point, call `ASSERT_GAME_THREAD` as the first statement. In both `GameEngine::update` implementations include `rts/profile.h` and surround existing statements, without reordering them, with the named zones from the design specification. Add `Platform.Win32Messages` to the Win32 message service and `Engine.FramePacer` to the frame-pacer call site.

- [ ] **Step 5: Write Stage 0 testing instructions**

Document the VC6 replay command, `vc6-releaselog` CRC diagnostic invocation, Tracy capture expectations, and the Zero Hour/Generals manual smoke checklist in `TESTING.md`. State explicitly that `-jobs` starts replay processes only.

- [ ] **Step 6: Build and inspect Stage 0**

Run the configured modern Win32 debug build. When the VC6 toolchain is available, build `vc6-releaselog` for Zero Hour. Run `git diff --check` and inspect the Stage 0 diff to confirm no frame-order condition, data member, replay format, or command-line parser changed.

- [ ] **Step 7: Commit Stage 0**

Stage only Task 1 files and derive a conventional commit message from the staged diff. Use the `perf` or `refactor` type only if the diff contains no user-visible behavior; otherwise use the narrowest valid type.

## Task 2: Create failing tests and CMake plumbing for the task runtime

**Files:**
- Create: `Core/Libraries/Include/Lib/TaskRuntime.h`
- Create: `Core/Libraries/Source/TaskRuntime/CMakeLists.txt`
- Create: `Core/Libraries/Source/TaskRuntime/TaskRuntime.cpp`
- Create: `Core/Tools/TaskRuntimeTest/CMakeLists.txt`
- Create: `Core/Tools/TaskRuntimeTest/TaskRuntimeTest.cpp`
- Modify: `CMakeLists.txt`
- Modify: `Core/Libraries/CMakeLists.txt`
- Modify: `Core/CMakeLists.txt`
- Modify: `Core/Tools/CMakeLists.txt`
- Modify: `.github/workflows/build-toolchain.yml`

**Interfaces:**
- Consumes only C++98 standard library types and native platform synchronization in implementation files.
- Produces a `core_task_runtime_tests` CTest executable that initially fails to compile because the runtime functions are undefined.

- [ ] **Step 1: Declare the desired public runtime API**

Create `TaskRuntime.h` with `rts::Task`, private copy constructors/assignments, and this surface:

```cpp
bool start(unsigned workerCount, unsigned queueCapacity);
bool trySubmit(Task *task);
bool trySubmitBatch(Task *const *tasks, unsigned taskCount);
void waitUntilIdle();
void shutdown();
bool isRunning() const;
unsigned workerCount() const;
unsigned pendingTaskCount() const;
```

Declare `struct State; State *m_state;` privately. Document the accepted/rejected ownership rules directly above the submit methods.

- [ ] **Step 2: Add the console test target and CTest registration**

Call `include(CTest)` after `project(...)`. Under `RTS_BUILD_CORE_EXTRAS`, add `TaskRuntimeTest`; its target links `core_task_runtime`, calls `add_test(NAME core_task_runtime_tests COMMAND core_task_runtime_tests)`, and has a console subsystem on Windows.

- [ ] **Step 3: Write the failing behavioral tests**

Use a minimal C++98 assertion helper that returns nonzero on failure. Cover these named cases:

```cpp
testInvalidStartArguments();
testExactlyOnceAtOneAndFourWorkers();
testFifoDequeueAtOneWorker();
testBatchAdmissionIsAllOrNothing();
testQueueBackpressure();
testShutdownDrainsAcceptedTasks();
testRuntimeCanRestartAfterShutdown();
testDestructorDrainsAndJoins();
```

Use a platform test gate for the backpressure test. The gate waits on a Win32 event or pthread condition and is opened explicitly by the test; it never uses a sleep to manufacture a race.

For `testDestructorDrainsAndJoins`, Task 2 verifies the observable drain postcondition (one accepted held task executes and is destroyed before the helper can finish). A scheduler-proof assertion that the native join itself occurred is deferred to Task 3: without implementation-side instrumentation, a nonjoining destructor can be preempted after return and is indistinguishable from a joining destructor until the held task is released. Do not add a public API or a global allocation override to work around that boundary.

- [ ] **Step 4: Add the empty runtime translation unit and run the test target before implementation**

Create `TaskRuntime.cpp` containing only its include so the static target is valid but exposes no method definitions. Configure the debug build, then run:

Run: `cmake --build build/win32-debug --target core_task_runtime_tests --config Debug`

Expected: configuration succeeds and linking fails with unresolved `TaskRuntime` methods. Record that failure as the TDD red state.

- [ ] **Step 5: Register the library target but leave behavior unimplemented**

Add `Source/TaskRuntime` to `Core/Libraries/CMakeLists.txt`. Create static `core_task_runtime` with public `Core/Libraries/Include`, private `core_config`, and `Threads::Threads` on Unix. Link it through `corei_always` after `add_subdirectory(Libraries)`. The target must not link WWLib, game-engine, or device libraries.

- [ ] **Step 6: Update CI to run CTest after Core-extras builds**

Add a reusable workflow step that runs:

```powershell
$arguments = @('--test-dir', "build\${{ inputs.preset }}", '--output-on-failure')
if ('${{ inputs.preset }}' -like 'win32*') { $arguments += @('-C', 'Release') }
& ctest @arguments
```

For debug presets choose `Debug`; for non-multi-config VC6 omit `-C`. Guard the step with `inputs.extras` so weekly release builds without Core extras do not attempt to execute absent test targets. Keep it before artifact collection.

## Task 3: Implement the bounded task runtime

**Files:**
- Modify: `Core/Libraries/Source/TaskRuntime/TaskRuntime.cpp`
- Modify: `Core/Libraries/Source/TaskRuntime/CMakeLists.txt`
- Modify: `Core/Tools/TaskRuntimeTest/TaskRuntimeTest.cpp`
- Modify: `Core/Tools/TaskRuntimeTest/CMakeLists.txt`

**Interfaces:**
- Consumes exactly the `TaskRuntime.h` API from Task 2.
- Produces a running bounded FIFO task runtime and green `core_task_runtime_tests` results.

- [ ] **Step 1: Implement shared state and lifetime invariants**

Keep these members private in `TaskRuntime::State`: bounded `std::deque<Task*>`, configured capacity, worker count, active task count, acceptance flag, stop flag, and native synchronization/thread-handle storage. Set idle only when queue and active counts are both zero.

`TaskRuntime` construction itself must be allocation-free. Allocate `State` lazily in `start()` with `std::nothrow`, catch construction failures, and leave every forwarding method safe when no state exists. Any `std::vector` reserve under the native lock must catch and unlock before returning `false`; do not publish a partially started state.

- [ ] **Step 2: Implement atomic admission**

Under the native mutex/critical section, reject when runtime is stopped, a null task is supplied, the task count is zero, or the full requested batch exceeds remaining queue capacity. On success append every task in call order, clear idle, and signal worker availability. Do not transfer any task on batch rejection.

If a container allocation fails during admission, roll back every pointer appended by that batch before releasing the lock, then return `false`; the worker cannot observe the provisional entries and the caller retains every task. Do not let allocation exceptions escape `start()` or either submit method.

- [ ] **Step 3: Implement the Windows worker loop**

Use `_beginthreadex` and a static `unsigned __stdcall` entry point. A worker waits without spinning, dequeues one task under `CRITICAL_SECTION`, executes it outside the lock, deletes it, decrements active work under the lock, and signals idle only when no task remains. `shutdown()` rejects submission, signals all waiters, drains accepted work, waits every stored handle, and closes every handle; it never calls `TerminateThread`.

- [ ] **Step 4: Implement the Unix worker loop**

Use `pthread_create`, `pthread_join`, `pthread_mutex_t`, and `pthread_cond_t`. Wait with `while (queue.empty() && !stopping) pthread_cond_wait(...)`; dequeue in FIFO order; broadcast the idle condition after the final active or queued task completes. Link `Threads::Threads` only on Unix.

- [ ] **Step 5: Complete lifecycle methods**

`waitUntilIdle()` blocks the owning caller until the queue and active count are zero. `shutdown()` is idempotent and leaves the object ready for a later `start()`. The destructor calls `shutdown()`. Do not allow a task to invoke `shutdown()` or wait on child work.

Under a private test-build compile definition enabled only for `RTS_BUILD_CORE_EXTRAS`, add an implementation-local observer used by `TaskRuntimeTest.cpp` to record destructor entry and each successful native worker join. Keep it out of `TaskRuntime.h` and retail builds. Strengthen `testDestructorDrainsAndJoins` with that observer: it must see destructor entry before the test releases its held task, and exactly one successful join after the task is executed and destroyed. This is the only scheduler-proof way to distinguish a missing native join without changing the public runtime contract.

Use the same extras-only implementation-local test boundary to inject one-shot allocation failures before lazy state creation, thread-storage reserve, and a selected queue append. Add tests proving the methods return `false`, release their native lock, preserve rejected task ownership, and permit a later successful start/submit/shutdown. Set both runtime/test targets to C++98 and give the runtime CTest a finite timeout so a regressed stranded-lock test fails diagnostically instead of hanging CI.

- [ ] **Step 6: Run the TDD green checks**

Run `core_task_runtime_tests` through CTest in the configured modern build. Confirm every named test passes. Then build the shared Core code in the available VC6 configuration and run the same CTest command there.

- [ ] **Step 7: Commit Stage 1**

Stage the runtime, its CMake/test/CI changes, inspect `git diff --cached --check`, derive a conventional commit message from the exact staged diff, and commit only after fresh test output is green.

## Task 4: Add and test the D3D-free screenshot codec

**Files:**
- Create: `Core/GameEngineDevice/Include/W3DDevice/GameClient/W3DScreenshotCodec.h`
- Create: `Core/GameEngineDevice/Source/W3DDevice/GameClient/W3DScreenshotCodec.cpp`
- Create: `Core/Tools/ScreenshotCodecTest/CMakeLists.txt`
- Create: `Core/Tools/ScreenshotCodecTest/ScreenshotCodecTest.cpp`
- Modify: `Core/GameEngineDevice/CMakeLists.txt`
- Modify: `Core/Tools/CMakeLists.txt`

**Interfaces:**
- Produces a D3D-free source descriptor and row conversion operation for Stage 2/3.

```cpp
enum ScreenshotSourceFormat
{
	SCREENSHOT_SOURCE_ARGB32,
	SCREENSHOT_SOURCE_RGB565
};

struct ScreenshotPixelSource
{
	const unsigned char *pixels;
	unsigned width;
	unsigned height;
	unsigned pitch;
	ScreenshotSourceFormat format;
};

void ConvertScreenshotRows(const ScreenshotPixelSource &source,
	unsigned yBegin, unsigned yEnd, unsigned char *rgbDestination);
```

- [ ] **Step 1: Add known-output tests before codec implementation**

Create explicit ARGB32 and RGB565 source arrays with padded rows and an odd width/height. Assert the exact R, G, B bytes after serial conversion. Add invalid-range assertions only when they can compile out of release behavior without reading out of bounds.

- [ ] **Step 2: Build the screenshot codec test before implementation**

Run: `cmake --build build/win32-debug --target core_screenshot_codec_tests --config Debug`

Expected: build fails because `ConvertScreenshotRows` is declared but undefined.

- [ ] **Step 3: Implement row-only conversion**

Implement the exact existing conversions:

```cpp
rgb[index + 0] = (unsigned char)(argb >> 16);
rgb[index + 1] = (unsigned char)(argb >> 8);
rgb[index + 2] = (unsigned char)argb;
```

and the existing R5G6B5 masks/shifts. Convert only `[yBegin, yEnd)` and use the source pitch for each input row. Do not allocate, write files, access D3D, or reference game globals.

- [ ] **Step 4: Run codec tests and add the target to CTest**

Register `core_screenshot_codec_tests` with CTest under Core extras. Run it in the same build as Task 3. Verify serial conversion byte output matches the hard-coded expected arrays.

## Task 5: Migrate Stage 2 screenshots to the managed runtime

**Files:**
- Modify: `Core/GameEngineDevice/Include/W3DDevice/GameClient/W3DScreenshot.h`
- Modify: `Core/GameEngineDevice/Source/W3DDevice/GameClient/W3DScreenshot.cpp`
- Modify: `Core/Tools/ScreenshotCodecTest/ScreenshotCodecTest.cpp`
- Modify: `Generals/Code/GameEngineDevice/Source/W3DDevice/GameClient/W3DDisplay.cpp`
- Modify: `GeneralsMD/Code/GameEngineDevice/Source/W3DDevice/GameClient/W3DDisplay.cpp`
- Modify: `TESTING.md`

**Interfaces:**
- Consumes `rts::TaskRuntime`, `rts::Task`, and `ConvertScreenshotRows`.
- Produces `W3D_ShutdownScreenshotTasks()` and bounded managed screenshot work.

- [ ] **Step 1: Extend codec tests with the exact Stage 2 serial full-image path**

Add a helper that calls `ConvertScreenshotRows(source, 0, source.height, destination)` and verify it produces the same bytes as the reference arrays. Run it before changing `W3DScreenshot.cpp`; it must pass because this tests the codec from Task 4.

- [ ] **Step 2: Replace the detached thread data with owned task data**

Create a private `ScreenshotEncodeTask : public rts::Task` in `W3DScreenshot.cpp`. It owns copied source pixels, RGB output storage, dimensions, pitch, source format, output path, output leaf name, quality, and target screenshot format. `execute()` converts the full image with the codec, calls the existing stb writer, then pushes a POD completion message. It never reads `TheGlobalData`, `TheInGameUI`, DX8, or any live render object.

- [ ] **Step 3: Add the private bounded screenshot service**

Use a private static service owning one `TaskRuntime`. Start it lazily with `min(2, GetSystemInfo().dwNumberOfProcessors)` workers, falling back to one, and queue capacity four. Submit the completed request using `trySubmit`; on rejection delete the request and `DEBUG_LOG` the dropped capture. Do not block or retry on the frame thread.

- [ ] **Step 4: Preserve main-thread completion delivery**

Leave `W3D_UpdateScreenshotMessages` as the only location that calls `TheInGameUI->message`. Make it delete completion records when no UI is available. Preserve existing successful-message text and failure logging behavior.

- [ ] **Step 5: Add explicit display teardown**

Declare `W3D_ShutdownScreenshotTasks()` in the screenshot header. Invoke it at the start of both `W3DDisplay` destructors, before freeing display, asset, renderer, or DX8 objects. In the existing title teardown order, UI has already been destroyed by this point; the function must drain/join workers and free completion records without invoking the UI.

- [ ] **Step 6: Build and run focused checks**

Run both task runtime and screenshot codec CTest targets. Build modern Zero Hour and Generals debug targets, plus the available VC6 release-log target. Confirm no `CreateThread` remains in `W3DScreenshot.cpp` and no worker task contains `The`, `DX8`, `WW3D`, or `TheInGameUI` references.

- [ ] **Step 7: Commit Stage 2**

Stage only the managed screenshot migration, codec, display lifecycle, and testing-documentation changes. Inspect the staged diff and commit with a conventional message derived from it.

## Task 6: Add Stage 3 parallel row conversion

**Files:**
- Modify: `Core/GameEngineDevice/Include/W3DDevice/GameClient/W3DScreenshotCodec.h`
- Modify: `Core/GameEngineDevice/Source/W3DDevice/GameClient/W3DScreenshotCodec.cpp`
- Modify: `Core/Tools/ScreenshotCodecTest/ScreenshotCodecTest.cpp`
- Modify: `Core/GameEngineDevice/Source/W3DDevice/GameClient/W3DScreenshot.cpp`
- Modify: `TESTING.md`

**Interfaces:**
- Consumes `TaskRuntime::trySubmitBatch` and `ConvertScreenshotRows`.
- Produces `BuildScreenshotRowRanges` for exact, testable stripe planning.
- Produces a `ScreenshotBatch` whose owned memory remains valid until the last row task encodes and reports the result.

- [ ] **Step 1: Write the failing multi-worker equivalence test**

Declare this C++98 API in `W3DScreenshotCodec.h` but do not define it yet:

```cpp
struct ScreenshotRowRange
{
	unsigned yBegin;
	unsigned yEnd;
};

unsigned BuildScreenshotRowRanges(unsigned height, unsigned workerCount,
	ScreenshotRowRange *ranges, unsigned rangeCapacity);
```

In `core_screenshot_codec_tests`, call it for 1, 127, 128, and 1080 rows. Assert the returned ranges are contiguous, non-overlapping, cover `[0, height)`, and obey the one-worker/small-image fallback. Then use the returned ranges with a two-worker `TaskRuntime` to compare serial and striped conversion bytes for ARGB32, RGB565, padded pitch, and odd dimensions.

- [ ] **Step 2: Run the test before implementing stripe planning**

Run the screenshot codec CTest target. Expected: it fails at link time because `BuildScreenshotRowRanges` has no implementation.

- [ ] **Step 3: Implement deterministic stripe planning and obtain the green codec test**

For `workerCount() > 1` and images at or above 128 rows, return `min(workerCount * 2, (height + 63) / 64)` contiguous ranges. For all other images return exactly one `[0, height)` range. Make no allocation and return zero only when `ranges` is null, `rangeCapacity` is zero, or `height` is zero. Run the codec CTest target and confirm the serial/parallel byte comparisons are green.

- [ ] **Step 4: Implement private batch ownership in `W3DScreenshot.cpp`**

Create a private `ScreenshotBatch` owning source bytes, RGB destination bytes, image/output metadata, and a `LONG remainingTasks`. Create `ScreenshotConvertTask : public rts::Task` with a batch pointer and fixed `[yBegin, yEnd)` range. The task calls `ConvertScreenshotRows`; after `InterlockedDecrement(&remainingTasks) == 0`, it writes the completed RGB image, pushes the completion record, and deletes the batch.

- [ ] **Step 5: Submit the planned stripe set atomically**

Ask `BuildScreenshotRowRanges` for the bounded stripe set, allocate all task objects first, submit them by `trySubmitBatch`, and delete every task plus the batch on rejection. No worker waits for another task and the main thread never waits for a screenshot.

- [ ] **Step 6: Preserve Stage 2 fallback and add profiling**

Use the single stripe fallback for one worker or small images. Add `PROFILER_SECTION_NAME("Screenshot.Convert")` around conversion and `PROFILER_SECTION_NAME("Screenshot.Encode")` around image writing; they compile out outside the existing profile build. Do not modify capture, DX8 locking, UI delivery, or image writer selection.

- [ ] **Step 7: Run green focused checks and source-boundary audit**

Run `core_task_runtime_tests` and `core_screenshot_codec_tests` with CTest. Build modern Zero Hour debug and the available VC6 release-log configuration. Verify the multi-worker test now passes and inspect `W3DScreenshot.cpp` to confirm no task uses `The*`, DX8, UI, or live render memory.

- [ ] **Step 8: Commit Stage 3**

Stage the parallel batch change, tests, and Stage 3 instructions. Run `git diff --cached --check`, derive the commit message from the staged diff with the requested conventional-commit workflow, and commit after fresh green output.

## Task 7: Perform end-to-end validation and prepare review material

**Files:**
- Modify only if verification exposes a documented defect; otherwise no planned source changes.
- Review: `docs/superpowers/specs/2026-08-13-multicore-stages-0-3-design.md`
- Review: this plan

**Interfaces:**
- Consumes all four stage commits.
- Produces evidence for the user, code reviewers, and the eventual unmerged pull request.

- [ ] **Step 1: Stop unneeded local build/test processes and inspect the final worktree**

Run `git status --short --branch`, `git diff --check`, and inspect active processes before final builds. Preserve unrelated processes and do not kill a process without confirming its command line and worktree ownership.

- [ ] **Step 2: Run final focused tests sequentially**

Run CTest for `core_task_runtime_tests` and `core_screenshot_codec_tests` in the modern build, then in VC6 when its toolchain is present. Record exact commands, configurations, and pass/fail counts.

- [ ] **Step 3: Run final build and replay validation where environment permits**

Build modern Win32 Zero Hour and Generals targets sequentially. Build optimized VC6 Zero Hour release-log. If the local original game data and replay corpus are present, run the documented `-jobs 4 -headless -replay` command and retain its output; otherwise report that this environment cannot provide the data-dependent replay proof and rely on CI for it.

- [ ] **Step 4: Prepare the four manual game-test checklists**

Stage 0: Tracy frame zones and release-log ownership/CRC smoke.

Stage 1: launch/map/exit smoke to prove an uninstantiated runtime changes no game behavior.

Stage 2: JPEG/PNG capture, rapid captures, rejection/backpressure, map return, and exit while writes run.

Stage 3: 1080p/4K JPEG/PNG capture, color/seam comparison, active gameplay responsiveness, alt-tab, device reset, and exit with conversion active.

- [ ] **Step 5: Create a complete review package and dispatch reviewers**

Use the merge-base from `ae07b29f4f9df14b2266fc40444d3b1b17b1872e`, include the full diff/stat/log and validation evidence, and dispatch independent subagents for concurrency/lifetime, build/VC6 portability, and gameplay/DirectX boundary review. Treat every Critical or Important finding as a fix-loop blocker.

- [ ] **Step 6: Fix review findings through scoped re-review**

For every Critical or Important finding, add a failing test when the issue is testable, implement the smallest fix, rerun the covering test and affected build, and request a scoped re-review of only the fix range. Repeat until no Critical or Important findings remain or a genuine user decision is required.

- [ ] **Step 7: Open the unmerged pull request**

After fresh final verification, push `codex/multicore-upgrade-plan` and create a non-draft pull request. Its title and commits use conventional format. Its body discloses AI-generated code, lists the four stages, documents replay/manual validation status, explicitly states that it must not be merged yet, and asks reviewers to focus on lifecycle, VC6, and determinism boundaries.

## Plan self-review

- Spec coverage: Tasks 1-6 map directly to Stages 0-3; Task 7 covers validation, review, PR, and the required manual handoff.
- Placeholder scan: no task depends on an unnamed API or an unspecified file.
- Type consistency: Stage 2/3 use only `Task`, `TaskRuntime`, `trySubmitBatch`, `ConvertScreenshotRows`, and `BuildScreenshotRowRanges` declared in earlier tasks.
- Scope: no simulation, renderer, texture-loader, network, replay-format, or save-format work appears in this plan.
