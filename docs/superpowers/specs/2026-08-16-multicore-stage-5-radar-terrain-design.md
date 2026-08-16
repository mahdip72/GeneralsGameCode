# Stage 5: Radar Terrain Raster Preparation Design

**Date:** 2026-08-16
**Status:** Design for implementation
**Scope:** `W3DRadar::buildTerrainTexture` only

## Goal

Move the CPU work that prepares a radar terrain raster into a bounded,
synchronous fork-join operation.  The game/render owner keeps all live game
objects, terrain services, Direct3D objects, and the final upload.  At most two
workers shade disjoint row ranges from an immutable, owner-built snapshot.  A
serial execution of the same pixel kernel is used when the worker runtime cannot
be started or a submission/allocation fails.  For every input and output format,
the staged path must produce the exact bytes produced by the current
`buildTerrainTexture` implementation.

This is render preparation, not simulation parallelism.  It does not change
radar dimensions, sampling, water/bridge decisions, color interpolation,
format packing, texture lifetime, refresh timing, or replay/save/network state.

## Non-goals and hard constraints

- Do not parallelize `Radar::newMap`, `Radar::refreshTerrain`, shroud updates,
  view reconstruction, or any simulation update.
- Do not move a live pointer to a worker.  Workers receive only plain data with
  ownership held by the active batch.
- Workers must not call Direct3D, `The*` globals, terrain/game logic, UI,
  allocation, waits, RNG, replay, save, or network code.
- Keep the implementation C++98 and compatible with the VC6/Win32 lane.  Use
  checked unsigned-size arithmetic and local `try`/`catch (...)` allocation
  helpers that return null.  VC6 does not provide every array nothrow overload
  needed by this code.
- Keep one active render-preparation batch per shared service.  A Stage 6--8
  consumer may not overlap a radar batch or use the service after shutdown.
- Do not “correct” existing interpolation argument order or change floating
  point evaluation order.  Serial-versus-two-worker byte equality is the gate.

## Existing output contract

`W3DRadar::buildTerrainTexture(TerrainLogic *terrain)` currently executes on
the owner thread and writes a 128x128 surface.  The implementation must retain
this ordered behavior:

1. Set `m_reconstructViewBox` and copy the radar water color from
   `TheWaterTransparency` before sampling.
2. Obtain the surface and description on the owner.  The worker path stages
   tightly packed rows in CPU memory; it does not hold a locked D3D surface
   while workers execute.
3. For each `(x, y)`, call the equivalent of `radarToWorld2D` for the center
   and preserve its x/y clamping and sampling.  The center bridge query is
   performed before the center water query.  A bridge is “working” only when
   its game object/body exists and its body damage state is not rubble.
4. For a working bridge, use the bridge template radar color and the average of
   its four bridge corner heights for every in-bounds neighbor.  Preserve the
   missing-template assertion and white fallback.
5. For a non-bridge underwater center, use the center water height and the
   water color for each underwater neighbor.  Preserve the exact two
   `isUnderwater` call forms and their output arguments.
6. Otherwise, use the equivalent of `radarToWorld` for each neighbor, which
   supplies ground height through `TheTerrainLogic`.  Sample the terrain color
   from `TheTerrainVisual` and apply the current height interpolation.
7. Iterate neighbors in `j = y - 1 .. y + 1`, then `i = x - 1 .. x + 1`, skip
   out-of-bounds cells, accumulate RGB in that order, use one sample when no
   neighbor was valid, average, call the existing format conversion, and write
   the exact `bytesPerPixel` bytes.

The current calls to `interpolateColorForHeight` pass the terrain average and
map extrema in a historically unusual parameter order.  That order, its equal
height guards, `Real` operations, byte conversion, alpha value, and format
packing are all part of the compatibility contract.  The shared kernel must
make this order obvious in one place and tests must lock it down.

## Ownership model

### Owner-only phase

`W3DRadar::buildTerrainTexture` owns the complete lifecycle of one batch:

1. Validate the fixed dimensions, surface format, bytes per pixel, checked
   cell/output sizes, and the maximum staging budget.
2. Read every live input listed in the snapshot manifest below and copy the
   resulting scalar/color values into the batch.  No pointer is retained in the
   batch.
3. Start or acquire the dedicated render-preparation runtime, create the two
   row tasks, and submit them as one all-or-none batch.
4. Wait for that private runtime to become idle.  This is the only wait in the
   operation; workers never wait for one another.
5. If start, task allocation, or all-or-none submission fails after staging,
   invoke the same pure pixel kernel serially over the batch.  If staging
   allocation fails, invoke the retained allocation-free legacy owner loop.
6. Lock the D3D surface, copy staged rows without touching surface padding,
   unlock, release, and finish the batch lease.  All Direct3D calls and
   `Draw_Pixel`-equivalent byte writes remain here.

The owner must not begin another consumer batch until the current lease is
released.  Refresh/new-map calls remain synchronous and observe the same
immediate output timing as the legacy function.

### Worker phase

Each task receives a pointer to the immutable batch plus `[yBegin, yEnd)`.
The two ranges are disjoint and cover the raster exactly.  A task invokes
`ShadeRadarRows` and writes only its own rows in the owner-allocated output
buffer.  It has no access to the owner object, `TerrainLogic`, a bridge/object,
the render device, or any process-global address.

### Immutable POD snapshot

The implementation should expose a D3D-free, testable shape equivalent to:

```cpp
struct RadarTerrainRgb {
    Real red;
    Real green;
    Real blue;
};

struct RadarTerrainCellInput {
    Real worldX;
    Real worldY;
    Real groundZ;                 // radarToWorld neighbor z
    unsigned char centerUnderwater;
    Real centerWaterSurfaceZ;
    Real centerWaterBottomZ;
    unsigned char neighborUnderwater;
    Real neighborWaterSurfaceZ;
    Real neighborWaterBottomZ;
    unsigned char workingBridge;
    RadarTerrainRgb terrainColor;
    RadarTerrainRgb bridgeColor;
    Real bridgeHeight;
};

struct RadarTerrainSnapshot {
    unsigned width;
    unsigned height;
    unsigned bytesPerPixel;
    unsigned formatCode;          // exact ARGB/WW3D format enum value
    unsigned rowBytes;
    Real terrainAverageZ;
    Real mapHighZ;
    Real mapLowZ;
    RadarTerrainRgb waterColor;
    RadarTerrainCellInput *cells; // owner-owned width * height array
};
```

The final names may follow repository conventions, but the fields and
ownership rules are required.  `cells[y * width + x]` contains all raw inputs
needed when that coordinate is a center or a neighbor.  The center bridge
decision and bridge values are deliberately stored separately from each
neighbor's ground/terrain/water values: the legacy loop selects the center's
branch and then samples the neighbors.

The snapshot must not contain `TerrainLogic *`, `TerrainVisual *`, bridge,
object, surface, device, allocator, mutex, task, or callback fields.  The
output byte buffer and task objects are also owner-owned and are reclaimed only
after the fork-join has completed.

## Snapshot manifest and exact sampling

The owner snapshot is the only place allowed to read live state.  It must
explicitly cover:

| Live source | Values copied | Required legacy behavior |
| --- | --- | --- |
| `Radar` fields | width/height, x/y sample, map high/low z, terrain average z | Preserve `radarToWorld2D` clamping and `radarToWorld` ground-height semantics. |
| `terrain` argument | center and neighbor underwater results and returned water heights | Keep the argument used by the current `isUnderwater` calls; do not silently replace it with a different terrain pointer. |
| `TheTerrainLogic` | ground heights; bridge lookup; bridge object/body and damage-state result | Preserve bridge lookup order and the non-rubble working test. |
| `TheGameLogic` | bridge object lookup by `bridgeObjectID` and body state | Store only the resulting flag. |
| `TheTerrainRoads` | bridge template lookup and template radar RGB | Store color and four-corner average height; retain white fallback on missing template. |
| `TheTerrainVisual` | terrain RGB at each neighbor coordinate | Copy the RGB after the same initialization and call sequence as the legacy local color. |
| `TheWaterTransparency` | radar water RGB | Copy once before the raster loop. |
| surface description | exact format and bytes per pixel | Use the same format packing and output byte width. |

`TheGlobalData` is not read by the current terrain raster function.  “All live
state” means all actual reads in this function, not an invitation to add a
global read to workers.  If an implementation discovers a new required global
read while preserving the function, it must add the resulting scalar/color to
the POD manifest and extend the tests; workers must still see only the copy.

For every cell the owner records both center and neighbor underwater states,
water-surface heights, and water-bottom heights when the legacy call would
request them.  It records ground z through the same
`TheTerrainLogic->getGroundHeight` path used by `radarToWorld`.  It invokes
`TheTerrainVisual->getTerrainColorAt` with the same initial `RGBColor` value as
the current code, because a visual implementation may intentionally leave the
value unchanged.  No pointer to a bridge, body, template, visual, or map is
stored.

## Pure shading and packing kernel

Implement one D3D-free helper for a pixel neighborhood, for example:

```cpp
void ShadeRadarPixel(const RadarTerrainSnapshot &snapshot,
                     unsigned x,
                     unsigned y,
                     const RadarTerrainCellInput *neighbors[3][3],
                     unsigned char *pixelBytes);
```

The helper must:

- visit the 3x3 neighborhood in the legacy nested-loop order;
- select the center bridge, center-water, or regular branch exactly once;
- perform the existing interpolation with the existing `Real` arithmetic and
  argument order;
- average RGB with the same zero-sample guard;
- call a pure format-packing helper equivalent to
  `GameMakeColor` plus `ARGB_Color_To_WW3D_Color` and copy exactly
  `bytesPerPixel` bytes in the same little-endian representation; and
- have no allocation, wait, global access, logging, exception path, or
  synchronization.

`ShadeRadarRows` loops over its assigned y range and calls this helper.  A
serial run invokes `ShadeRadarRows(snapshot, 0, height)` when a full snapshot
exists.  Task allocation and submission failure after a snapshot exists always
uses the full snapshot with `ShadeRadarRows` serially.  If the snapshot or
output cannot be allocated, retain the current allocation-free owner loop as
the final compatibility fallback.  Do not add a second cursor abstraction in
Stage 5; focused tests and reviews guard the staged kernel against drift.

The output staging rows are tightly packed (`rowBytes = width *
bytesPerPixel`).  The owner copies only `rowBytes` per row to the D3D surface;
surface pitch and any padding are owner concerns.

## Bounded synchronous runtime

Use a small shared `RenderPrepareRuntime` around `TaskRuntime`, rather than a
background queue hidden in `W3DRadar`.  Stage 5's radar service is one
exclusive consumer of this runtime; later render-preparation consumers must
use the same acquire/fork/join/owner-consume/release boundary.  A suitable
interface is:

```cpp
class RadarTerrainPrepareService {
public:
    bool initialize(unsigned workerCount, unsigned queueCapacity);
    bool tryAcquire(unsigned consumerId);
    bool runRows(RadarTerrainSnapshot *snapshot,
                 unsigned char *output,
                 unsigned rowBegin,
                 unsigned rowEnd);
    void release(unsigned consumerId);
    void shutdown();
};
```

`RenderPrepareRuntime` owns only bounded CPU row tasks and their joined
worker lifetime.  It is not a general game job pool: one consumer lease is
active at a time, and the owner waits only for the private Stage 5 batch it
submitted.  The runtime may be shared by Stages 5--8 only through this
exclusive lease; no consumer may submit work after shutdown or retain a task
or buffer past `release`.

The concrete API may use a lease object, but these invariants are mandatory:

- `initialize` caps workers at two and uses a private runtime whose queue
  contains only render-preparation tasks.  It is idempotent for the active
  display and cannot silently replace an active runtime.
- `tryAcquire` succeeds for one consumer only.  A radar lease is released
  before any future Stage 6--8 consumer can acquire the service.  Nested or
  cross-consumer batches are rejected and run serially by the caller.
- `runRows` first attempts the two-worker runtime with exactly two owner-side
  task wrappers and submits both with `trySubmitBatch`.  If worker startup,
  admission, or task allocation cannot support that attempt, it tears down
  only that idle attempt and retries the same two disjoint row tasks on a
  one-worker runtime.  If the one-worker startup/admission/allocation attempt
  also fails, it returns false and the owner runs the pure kernel serially.
  On either successful attempt it calls `waitUntilIdle` only after successful
  all-or-none submission.  Since the runtime is private and the lease is
  exclusive, this wait cannot consume unrelated work.
- `release` is called only after row tasks have completed and any owner upload
  has finished.  It never detaches or force-terminates a thread.
- `shutdown` first prevents new acquisition, then drains/joins the private
  runtime and is idempotent.  No batch, task, or worker may survive it.

The service is synchronous by design.  It does not keep a “latest generation,”
publish a future, or allow a second raster to replace the first.  This keeps
the `refreshTerrain`/`newMap` contract immediate and gives Stages 6--8 a safe
shared primitive: acquire, fork, join, owner-consume, release.  A future
consumer must use a different consumer ID and the same exclusive lease; it
must not bypass the service with a second runtime or share another consumer's
batch buffers.

## Lifecycle and display integration

The shared service should be initialized and shut down by both display variants
after source-order verification:

- In the Generals and GeneralsMD `W3DDisplay::init`, initialize the service
  after the display's basic thread/runtime prerequisites are ready and before
  the first radar refresh can occur.
- At the start of each matching `W3DDisplay` destructor, stop acquisition and
  drain the service before Direct3D, WW3D, scene, asset, or screenshot
  teardown.  The destructor must be safe when initialization partially failed.
- Verify the `GameClient` destruction order before implementation.  It
  currently destroys the terrain visual before the display object.  This is
  safe for workers only because the snapshot contains no visual pointer, but
  the owner must never be allowed to enter `buildTerrainTexture` during
  teardown.  If the order changes or a shutdown callback needs live terrain,
  add an explicit pre-display `Shutdown` call at the earlier owner boundary.
- `W3DRadar` must not retain a service lease across `buildTerrainTexture`,
  `newMap`, `refreshTerrain`, display reset, or display destruction.  Any
  exceptional owner path must release the lease before returning.

The lifecycle call sites are deliberately duplicated in the two title display
implementations.  The service implementation and pure kernel remain shared so
both variants receive the same behavior and tests.

## Memory and failure policy

Use a fixed per-batch budget, for example
`RADAR_TERRAIN_PREP_MAX_BYTES = 8 * 1024 * 1024`, covering the snapshot cell
array, tight output bytes, row-task wrappers, and conservative bookkeeping.
The exact constant may be tuned only with a checked size calculation and a
test assertion; it must not be an unbounded allocation policy.  At 128x128,
the normal batch is far below this cap.  Check every multiplication and
addition before allocation, reject dimensions or formats that do not fit, and
never allocate from a worker.

Failure behavior is deterministic and must preserve bytes:

| Failure | Required action |
| --- | --- |
| Two-worker startup/admission/allocation failure | Retry one worker with the same two disjoint row tasks; if that fails, use the serial kernel over the owner batch. |
| One-worker startup/admission/task-allocation failure | Run the pure kernel serially over the completed owner batch. |
| Cell/output allocation failure | Do not submit a partial batch. Run the retained allocation-free legacy owner path; never upload uninitialized staging bytes. |
| Task allocation or all-or-none submission failure | Reclaim caller-owned tasks and run the complete batch serially; do not leave a partial task set admitted. |
| `trySubmitBatch` rejects or rolls back | Reclaim caller-owned rejected tasks and run `ShadeRadarRows` once over the complete batch. |
| Owner lock/upload failure | Follow the existing owner error/assert path; never let a worker touch the surface or retry through a background task. |
| Shutdown/reset while a batch is active | The owner finishes the fork-join or serial fallback before release; service shutdown then drains and joins. |

The serial path must not be described as “best effort.”  It is the reference
execution of the same pure pixel kernel, and its bytes are a required oracle
for every worker test.

## Proposed implementation boundaries

Keep the D3D-free data model/kernel independently testable and keep live
snapshot/upload code in the game-engine device layer.  The implementation
should make the following boundaries visible in the build:

- `Core/Libraries/Include/Lib/RadarTerrainKernel.h` and
  `Core/Libraries/Source/TaskRuntime/RadarTerrainKernel.cpp`: POD input,
  interpolation, averaging, format packing, and row kernel only.
- `Core/GameEngineDevice/Include/W3DDevice/Common/RadarTerrainPrepare.h` and
  its source: owner snapshot, batch storage, service/lease, and TaskRuntime
  row tasks.
- `Core/GameEngineDevice/Source/W3DDevice/Common/System/W3DRadar.cpp`: calls
  the owner snapshot, service, serial fallback, and owner upload; it retains
  the public radar behavior.
- `Generals/Code/GameEngineDevice/Source/W3DDevice/GameClient/W3DDisplay.cpp`
  and the GeneralsMD counterpart: service initialization/shutdown only.
- `Core/Tools/RadarTerrainPrepareTest/RadarTerrainPrepareTest.cpp` and its
  CMake target: D3D-free behavioral tests for the kernel and service fallback.

The exact include directory may follow the repository's existing library
layout, but the kernel must not acquire a dependency on `W3DRadar`, D3D, or a
title-specific global.

## Test and acceptance contract

Tests are behavioral and test-first.  Add a D3D-free target in the existing
CMake/test style, compile it as C++98, and register it with CTest.  It must
cover:

1. serial versus two-worker output byte equality for every supported radar
   format exercised by the current converter, including 24-bit rows;
2. a synthetic edge-heavy raster proving clipped 3x3 neighborhoods, zero
   sample handling, bridge branch, water branch, regular terrain branch,
   missing bridge-template fallback, equal-height interpolation guards, and
   alpha/format bytes;
3. worker row ownership: two tasks cover all rows exactly once and no task
   writes another task's range;
4. runtime start failure, task allocation failure, queue backpressure, and
   all-or-none submission rollback each falling back to the same serial bytes;
5. exclusive lease behavior: a second consumer is rejected while radar owns
   the batch, and acquisition succeeds after release; shutdown drains and
   permits no new work; and
6. a source/compile audit that worker task code has no D3D, `The*` global,
   live engine pointer, allocation, wait, RNG, replay, save, or network use.

Run the focused runtime and radar tests, then the two title builds in the
standard out-of-tree configurations.  The commands are repository-relative
and intentionally use generic build directories:

```powershell
cmake --preset win32-debug
cmake --build build/win32-debug --target radar_terrain_prepare_tests core_task_runtime_tests core_texture_mip_buffer_tests g_generals z_generals --parallel 2
ctest --test-dir build/win32-debug -R "^(radar_terrain_prepare|core_task_runtime|core_texture_mip_buffer)_tests$" --output-on-failure

cmake --preset vc6
cmake --build build/vc6 --target radar_terrain_prepare_tests core_task_runtime_tests core_texture_mip_buffer_tests g_generals z_generals --parallel 2
```

Retain the Stage 4 checks for `core_task_runtime_tests` and texture tests.  Run
the optimized VC6 replay gate over ten unique Zero Hour recordings.  The nine
non-stress recordings run once, while the 2v6 Hard-AI stress recording runs
three times in separate CRC directories.  This is twelve replay process
executions total.  Every process must exit 0 with no CRC/ownership error, and
the three stress CRC file sets and hashes must be identical.  Manual acceptance
of the stacked program is intentionally deferred until Stages 5--8 are ready;
the user will test Stage 5 first at that point.

## Risks requiring implementation review

- The legacy function uses the `terrain` parameter for underwater queries but
  uses `TheTerrainLogic` for ground/bridge work.  Replacing either with a
  convenient cached pointer would change output.
- `TheTerrainVisual::getTerrainColorAt` can preserve its input color on some
  visual paths.  Snapshot the legacy initial value and result rather than
  assuming a default.
- Display destruction and terrain-visual destruction have a non-obvious
  order.  Confirm it in source before choosing the final shutdown call site;
  immutable worker data alone is not proof that owner callbacks are safe.
- Format packing and 24-bit surface writes are endian- and pitch-sensitive.
  Test bytes, not only RGB values, and copy no D3D row padding from staging.
- A future Stage 6--8 user must remain inside the exclusive service lease.
  Reject overlap explicitly so a convenient second consumer cannot reintroduce
  cross-consumer waits or use-after-shutdown behavior.

## Ten review lenses

1. **Owner boundary:** every live, global, terrain, bridge, object, and D3D
   read/write remains on the owner thread.
2. **Snapshot completeness:** every worker input is copied into bounded POD;
   no pointer, callback, allocator, or mutable alias crosses the boundary.
3. **Legacy parity:** center branch order, 3x3 row-major traversal, clamping,
   interpolation argument/evaluation order, averaging, and packing are exact.
4. **Row ownership:** exactly two disjoint ranges cover every row once, with
   checked indices and no output overlap.
5. **Runtime isolation:** `RenderPrepareRuntime` is private to the active
   lease, and the owner waits only for this submitted batch.
6. **Failure determinism:** startup, admission, task allocation, queue
   backpressure, rollback, and storage failures all use the serial oracle.
7. **Memory bounds:** all size arithmetic is checked against the fixed budget;
   workers allocate nothing and no buffer outlives the joined batch.
8. **Lifecycle safety:** both display variants initialize/drain idempotently;
   shutdown prevents new work and no task survives D3D teardown.
9. **Compatibility evidence:** C++98/VC6 and modern Win32 builds, focused
   source audits, twelve executions across ten unique optimized VC6 replays,
   and three identical 2v6 CRC traces are recorded.
10. **Delivery hygiene:** the diff is scoped to Stage 5, uses repository-
    relative paths, and the review PR is created targeting `main` but left
    unmerged until explicit approval.

## Delivery boundary

After implementation and the full validation gate, create the review PR with
the design, ownership contract, focused-test evidence, build commands, deferred
manual checklist, and any unavailable external prerequisites.  The PR must
remain unmerged; promotion and runtime deployment wait until the complete stack
is ready and the user authorizes the Stage 5 launch.
