# Stage 5 Radar Terrain Raster Preparation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to execute this plan.

**Goal:** Make `W3DRadar::buildTerrainTexture` use a bounded synchronous
fork-join for CPU radar shading while preserving exact legacy raster bytes and
keeping live game/render state on the owner thread.

**Architecture:** The owner snapshots every raw center/neighbor input into an
immutable POD batch.  A D3D-free `ShadeRadarPixel`/`ShadeRadarRows` kernel
performs the existing branch selection, 3x3 traversal, interpolation,
averaging, and format packing.  A small shared `RenderPrepareRuntime` submits
exactly two disjoint row tasks through a private `TaskRuntime` lease and the
owner waits for that private batch only.  Failure to start, allocate, or submit
runs the same pixel kernel serially; the owner then locks and uploads the
staged bytes.  Both W3DDisplay variants explicitly initialize and drain the
reusable, exclusive render-preparation service.

**Tech Stack:** C++98/VC6-compatible C++, existing `TaskRuntime`, existing
Win32/pthread synchronization, CMake/CTest, D3D-free behavioral tests, and the
existing Generals/GeneralsMD build/replay lanes.

## Global constraints

- Work only in the listed repository files and preserve unrelated user edits.
- Keep Direct3D, `The*` globals, game objects, terrain services, UI, and live
  pointers on the owner thread.
- Workers perform bounded CPU work over immutable POD only; they do not allocate,
  wait, log, use RNG, replay/save/network state, or call title code.
- Use at most two workers and exactly one private render-preparation batch at a
  time.  Do not introduce an asynchronous “latest generation” queue.
- `RenderPrepareRuntime` is shared only through one exclusive acquire/release
  lease; Stage 5 creates exactly two row tasks, and no task survives return or
  shutdown.
- Startup, admission, and allocation use the deterministic `2 workers -> 1
  worker -> serial` sequence: retry the same two row ranges with one worker,
  then invoke the pure serial oracle if the retry cannot be admitted.
- Preserve the current `radarToWorld2D`/`radarToWorld` distinction, bridge and
  water branch order, neighbor traversal order, interpolation argument order,
  floating evaluation order, format packing, and surface bytes.
- Check all size arithmetic against the fixed per-batch budget before
  allocation.  A rejected worker path must produce the same serial bytes.
- Keep docs, test output, and comments free of machine-specific paths,
  profile names, and local log locations.

---

## Task 1: Add a D3D-free kernel and red/green test scaffold

**Files:**

- Add `Core/Libraries/Include/Lib/RadarTerrainKernel.h`.
- Add `Core/Libraries/Source/TaskRuntime/RadarTerrainKernel.cpp`.
- Add `Core/Tools/RadarTerrainPrepareTest/RadarTerrainPrepareTest.cpp`.
- Add the matching `Core/Tools/RadarTerrainPrepareTest/CMakeLists.txt` and
  register it from the existing tools/test CMake entry point.

**Steps:**

1. Write the failing behavioral tests first.  Build a small synthetic
   `RadarTerrainSnapshot` with edge coordinates, bridge/water/regular center
   branches, equal height ranges, a missing-template white bridge color, and
   all supported surface format codes.  Include underwater state plus
   surface/bottom water heights, bridge RGB/height, map extrema/average, water
   color, and the final WW3D format in the POD.  Compare a serial full-raster call with
   a two-range call byte-for-byte, including 24-bit output and guard bytes
   before/after each output row.
2. Define C++98 PODs for RGB, one per-coordinate raw input, the scalar/map
   snapshot, and the output format/row width.  Make ownership explicit in the
   comments: the caller owns `cells` and output, the kernel borrows them only
   during the call, and no POD stores a live engine pointer.
3. Implement the pure color interpolation helper with the legacy equal-height
   guards and the existing parameter order.  Do not normalize or reorder the
   four height arguments.
4. Implement `ShadeRadarPixel` with the exact 3x3 clipped traversal and branch
   order.  Implement format packing equivalent to `GameMakeColor` and
   `ARGB_Color_To_WW3D_Color` without using a D3D object or global state.
5. Implement `ShadeRadarRows(snapshot, output, yBegin, yEnd)` as a row-range
   loop that calls the single pixel helper.  Reject invalid range/size inputs
   without writing output.
6. Add tests for row disjointness, boundary sample counts, color interpolation,
   format bytes, and serial/two-worker equality.  Keep tests C++98 and use the
   repository's event/gate style rather than sleeps.
7. Configure the target with the same warning/standard settings as
   `core_task_runtime_tests`, add it to CTest, and run only this focused target
   plus the existing task-runtime tests.

**Exit criteria:** The D3D-free target is red before implementation and green
after the kernel is implemented; serial and two-range outputs are byte exact
for the synthetic cases, and no production code has been changed yet.

## Task 2: Implement owner-side snapshot and bounded batch storage

**Files:**

- Add `Core/GameEngineDevice/Include/W3DDevice/Common/RadarTerrainPrepare.h`.
- Add `Core/GameEngineDevice/Source/W3DDevice/Common/System/RadarTerrainPrepare.cpp`.
- Update the shared GameEngineDevice source/header list in its CMake file.
- Update `Core/GameEngineDevice/Source/W3DDevice/Common/System/W3DRadar.cpp`.

**Steps:**

1. Define `RadarTerrainBatch` as owner storage for the immutable snapshot,
   tight output bytes, dimensions, row width, and completion state.  Use
   checked multiplication/addition and a fixed constant such as
   `RADAR_TERRAIN_PREP_MAX_BYTES = 8 * 1024 * 1024`.  Allocate before worker
   submission through local VC6-safe `try`/`catch (...)` helpers that return
   null; never allocate in `Task::execute`.
2. Add an owner-only capture function that reads the exact live sources:
   `Radar` sampling/map fields, the `terrain` argument, `TheTerrainLogic`,
   `TheGameLogic`, `TheTerrainRoads`, `TheTerrainVisual`,
   `TheWaterTransparency`, and surface format description.  Copy colors,
   z-values, water flags, bridge working flag, bridge color, and bridge height;
   store no pointer.  Preserve the current bridge/body damage-state checks and
   missing-template fallback.
3. Capture every coordinate needed by the 3x3 neighborhoods once.  Preserve
   center water versus neighbor water call forms, and record neighbor ground z
   through the same `radarToWorld` path.  Initialize terrain color exactly as
   the legacy local does before calling `getTerrainColorAt`.
4. Keep all conversions and bounds consistent with `Radar::radarToWorld2D`:
   clamped x/y, no accidental z use for center water, and the current map
   sample values.  Add assertions or explicit error returns for impossible
   dimensions and zero sample values.
5. Refactor the existing owner loop so it calls the pure kernel through a full
   batch.  Do not change `newMap`, `refreshTerrain`, reconstruct flags, surface
   lifetime, or format selection.
6. Retain the current allocation-free owner raster loop as the final fallback
   when snapshot or output allocation fails.  Do not submit partial staging or
   upload uninitialized bytes.  All failures after a complete snapshot exists
   use the new pure kernel serially.
7. Keep surface acquisition/description, lock, pitch handling, unlock/release,
   and row upload on the owner.  Upload only `rowBytes`; never copy staging
   padding into a surface row.

**Exit criteria:** A serial build of `buildTerrainTexture` through the batch
path produces the same bytes as a reference capture for bridge, water, regular,
edge, and non-32-bit formats.  Allocation-fault tests take the retained legacy
owner path without uninitialized output or worker access.

## Task 3: Add the two-worker synchronous fork-join service

**Files:**

- Update `Core/GameEngineDevice/Include/W3DDevice/Common/RadarTerrainPrepare.h`.
- Update `Core/GameEngineDevice/Source/W3DDevice/Common/System/RadarTerrainPrepare.cpp`.
- Update `Core/GameEngineDevice/Source/W3DDevice/Common/System/W3DRadar.cpp`.
- Extend `Core/Tools/RadarTerrainPrepareTest/RadarTerrainPrepareTest.cpp`.

**Steps:**

1. Define a task type containing only a batch pointer and an exclusive row
   range.  Its `execute` method calls `ShadeRadarRows` and nothing else.  Its
   destructor must not free batch storage owned by the owner.
2. Define the small shared `RenderPrepareRuntime` and its
   `RadarTerrainPrepareService` adapter around a private `TaskRuntime`.
   Initialize with at most two workers and a queue capacity that accepts the
   exactly two row tasks.  Make initialization/shutdown idempotent and reject
   new leases while stopping.  On startup/admission/allocation failure, retry
   the same two rows with one worker before falling back to serial.
3. Implement an exclusive consumer lease.  `W3DRadar` acquires the radar
   consumer ID before submitting; the lease is released after owner upload.
   A second consumer is rejected while active and may use the serial kernel.
4. Allocate both task wrappers on the owner, submit them with
   `trySubmitBatch`, and treat the result as all-or-none.  On success, wait
   only on this private runtime's `waitUntilIdle`; on any start/allocation/
   submission failure, reclaim caller-owned tasks and invoke
   `ShadeRadarRows(snapshot, output, 0, height)` once serially.
5. Add deterministic fault-hook tests for runtime start failure, task
   allocation failure, queue backpressure, all-or-none rollback, and shutdown
   during a submitted batch.  Compare every fallback buffer with the serial
   oracle and verify no task remains accepted after shutdown.
6. Add the source audit that rejects D3D includes/calls, `The` globals, live
   engine pointers, `new`/`delete`, waits, RNG, replay/save/network calls in
   the worker task and row kernel.

**Exit criteria:** Two tasks cover the raster exactly once; successful and
failed submissions are synchronous; every failure mode is byte-equal to the
serial kernel; and the service has no unrelated work for its owner to wait on.

## Task 4: Wire `W3DRadar` owner flow and preserve upload semantics

**Files:**

- Update `Core/GameEngineDevice/Source/W3DDevice/Common/System/W3DRadar.cpp`.
- Update `Core/GameEngine/Include/Common/Radar.h` only if a narrowly scoped
  helper declaration is required; do not alter conversion behavior.
- Extend the focused radar tests or add an owner-side seam test without
  introducing D3D dependencies into the kernel target.

**Steps:**

1. At the beginning of `buildTerrainTexture`, retain the reconstruct flag and
   owner-only surface/format discovery.  Snapshot before workers run and keep
   the surface unlocked during preparation.
2. Run the service fork-join when the batch is valid and the exclusive lease
   is available.  Otherwise call the same serial kernel or retained legacy
   allocation fallback immediately;
   do not queue asynchronous work or change refresh timing.
3. After join/fallback, lock once, copy each tight output row using the actual
   surface pitch, unlock, release, and release the surface.  Preserve existing
   asserts and cleanup behavior on lock failures.
4. Verify no live pointer or global is reachable from a task through the batch,
   and verify that all owner calls happen before the lease is acquired.
5. Add instrumentation only on the owner around snapshot, prepare, and upload
   so Tracy/profiling cannot enter worker code or alter the output.

**Exit criteria:** The function remains synchronous and produces an immediate
texture; owner-only D3D access is visible in source; serial and two-worker
instrumented runs are byte exact.

## Task 5: Integrate shared lifecycle in both display variants

**Files:**

- Update `Generals/Code/GameEngineDevice/Source/W3DDevice/GameClient/W3DDisplay.cpp`.
- Update `GeneralsMD/Code/GameEngineDevice/Source/W3DDevice/GameClient/W3DDisplay.cpp`.
- Update the corresponding GameEngineDevice CMake source lists if the shared
  service files are not already included.
- Inspect `Generals/Code/GameEngine/Source/GameClient/GameClient.cpp` and
  `GeneralsMD/Code/GameEngine/Source/GameClient/GameClient.cpp` destruction
  order; modify only if source proof shows an earlier owner shutdown call is
  required.

**Steps:**

1. Initialize the explicit shared service from each display variant after its
   runtime prerequisites and before any radar refresh.  Handle partial init
   and repeated display init without leaking a runtime.
2. At the first safe point in each display destructor, reject new leases and
   drain/join the private runtime before Direct3D/WW3D/assets are destroyed.
3. Prove the existing terrain-visual-before-display destruction order is safe:
   workers have only POD snapshots, no owner callback is pending, and every
   owner `buildTerrainTexture` call has returned.  If it is not provable, add
   an explicit owner shutdown before terrain visual deletion rather than
   relying on a static destructor.
4. Ensure service shutdown is idempotent and does not interfere with the
   screenshot/task-runtime shutdown path.  No display variant may leave a
   worker alive after its D3D device is gone.
5. Add a lifecycle test or guarded debug assertion for acquire/release,
   display reset, partial initialization, and shutdown with no active batch.

**Exit criteria:** Both Generals and GeneralsMD initialize and drain the same
service, no service task survives display teardown, and Stage 6--8 can reuse
the service only through a released exclusive consumer lease.

## Task 6: Build, behavioral validation, replay gate, and delivery

**Files:**

- Update `TESTING.md` with the Stage 5 focused test/build/replay checks and the
  manual checklist deferred until the full Stage 5--8 stack is ready.
- Update `CONTRIBUTING.md` only if a concise reusable ownership rule is needed;
  keep documentation generic and repository-relative.

**Steps:**

1. Run `git diff --check`, the focused radar test, `core_task_runtime_tests`,
   and existing texture tests.  Confirm fault-hook tests do not use sleeps or
   leave worker threads.
2. Configure/build both title targets and the focused tests with these exact,
   repository-relative commands (generic build directories only):

   ```powershell
   cmake --preset win32-debug
   cmake --build build/win32-debug --target radar_terrain_prepare_tests core_task_runtime_tests core_texture_mip_buffer_tests g_generals z_generals --parallel 2
   ctest --test-dir build/win32-debug -R "^(radar_terrain_prepare|core_task_runtime|core_texture_mip_buffer)_tests$" --output-on-failure

   cmake --preset vc6
   cmake --build build/vc6 --target radar_terrain_prepare_tests core_task_runtime_tests core_texture_mip_buffer_tests g_generals z_generals --parallel 2
   ```

   Record results without machine-specific paths in documentation.  If the
   VC6 toolchain or required game data is unavailable, report that prerequisite
   precisely and do not substitute a modern build for replay evidence.
3. Run the source audit for worker restrictions and inspect the final diff for
   accidental global/pointer capture, unbounded allocation, row overlap,
   altered interpolation order, or changed `Radar` conversions.
4. Record the deferred manual checklist for both titles: map load and radar
   refresh, movement, alt-tab, save/load, map reset, and immediate exit.  Do
   not launch the interactive game for this intermediate stage; the user will
   test Stage 5 first after the full stack is ready.
5. Run the optimized VC6 corpus over ten unique replays: the nine regular
   recordings once and the 2v6 Hard-AI stress recording three times in separate
   CRC directories.  Require twelve process exits of 0, no CRC/ownership error,
   and identical file sets and hashes for all three stress CRC traces.
6. Run the source/compile audit and inspect the final diff through ten lenses:
   owner boundary, complete POD snapshot, legacy byte parity, disjoint row
   ownership, private-runtime wait isolation, deterministic fallbacks, bounded
   memory, both-display lifecycle, C++98/VC6 plus gate evidence, and scoped
   delivery hygiene.
7. Before reporting completion, verify the final working-tree diff contains
   only the intended Stage 5 implementation/tests/docs.  Create a review PR
   targeting `main` with the evidence, but leave it unmerged.  Commit and push
   are authorized for this staged program; promotion and interactive launch are
   deferred until the user authorizes Stage 5 testing.

**Exit criteria:** Focused and existing tests pass, both title variants build,
the manual checklist is recorded, serial/two-worker bytes are exact, and replay/
deterministic simulation behavior is unchanged.

## Review checklist

- [ ] Every `The*`/live terrain/bridge/object read occurs before the lease and
      is represented by copied POD data.
- [ ] `ShadeRadarPixel` is the only implementation of interpolation, averaging,
      branch order, and format packing.
- [ ] Serial fallback uses that helper after complete staging; snapshot/output
      allocation failure retains the allocation-free legacy owner path.
- [ ] Two worker ranges are disjoint, bounded, and complete.
- [ ] Task code has no D3D/global/live-pointer/allocation/wait/RNG/replay/save/
      network access.
- [ ] The private runtime is the only thing the owner waits on; no cross-
      consumer overlap or stale generation exists.
- [ ] Snapshot/output memory arithmetic is checked against the fixed budget.
- [ ] Owner locks/uploads/unlocks/releases D3D only after fork-join completion.
- [ ] Both display variants initialize and drain the service safely.
- [ ] Behavioral tests compare bytes for all supported formats and all failure
  paths; focused tests, builds, and replay gates pass, with manual checks
  explicitly deferred.
- [ ] Twelve process executions across ten unique optimized VC6 replays pass;
  the 2v6 stress replay's three CRC traces are identical.
- [ ] A review PR is created for `main` and intentionally remains unmerged.
