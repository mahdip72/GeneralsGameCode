# Stage 7: Static Terrain Geometry Preparation Design

**Date:** 2026-08-16
**Status:** Approved for implementation
**Scope:** Max-LOD `HeightMapRenderObjClass` static vertex geometry and static-lighting preparation

## Goal

Move the CPU-only preparation performed by `HeightMapRenderObjClass::updateVB`
onto the existing bounded two-worker render-preparation service. The render
owner continues to own `WorldHeightMap`, scene lights, global state, vertex
buffer backups, Direct3D resources, device reset, and object lifetime. It
captures an immutable snapshot, workers prepare disjoint cell rows into private
staging memory, and the owner joins and commits the completed vertices.

The prepared vertex bytes must match the legacy path. Stage 7 must not change
terrain tessellation, flip rules, UVs, alpha, cliff/impassable visualization,
lighting order, depth fade, map mutation, camera behavior, simulation, AI,
pathfinding, networking, replay, save/load, RNG, or frame timing.

## Scope boundary

The only parallel slice is the static geometry and static-lighting calculation
currently performed by `HeightMapRenderObjClass::updateVB`, invoked from
`updateBlock`. These remain owner-only:

- height, UV, alpha, flip, cliff, light, water, and origin capture;
- dynamic-lighting updates and `updateVBForLight*`;
- extra-blend, shoreline, impassable-area mutations, and partial-update policy;
- `updateCenter`, LOD selection, map mutation, seismic updates, and terrain state;
- vertex-buffer backup publication and every DX8/D3D create, lock, upload,
  unlock, reset, and release operation.

`FlatHeightMap`, WorldBuilder-only mutation behavior, and lower-LOD
tessellation are non-goals. They keep the serial path.

## Ownership and execution model

For each eligible 32-by-32 vertex-buffer tile, the owner:

1. Validates the half-open terrain-cell rectangle and tile resources.
2. Captures all cell inputs and a one-sample halo into bounded owner-owned POD.
3. Captures global terrain lights and scene lights in iterator order as scalar
   values; no light or iterator pointer crosses the boundary.
4. Allocates private output staging and acquires render-preparation consumer 4.
5. Runs the same pure row kernel over two disjoint row ranges through
   `RadarTerrainPrepareService`, synchronously joining the batch.
6. Validates completion, applies any owner-only visualization overlay, copies
   the complete tile result into `m_vertexBufferBackup`, and uploads it while
   holding the DX8 vertex-buffer lock only on the owner.
7. Releases the lease and batch before returning or processing another tile.

The implementation uses a per-tile batch. One lease may be held across the
outer `updateBlock` tile loop, but each tile is captured, joined, and committed
before the next tile is prepared. Work is admitted only for at least two rows
and at least 512 cells; smaller scroll strips retain the legacy serial path.
The complete snapshot plus output is limited to 8 MiB with checked arithmetic.

Workers receive only immutable POD and a private output pointer. They allocate
nothing, throw nothing, wait on nothing, log nothing, and call no DX8/D3D,
`WorldHeightMap`, `LightClass`, render object, global, replay, save, network,
RNG, allocator, or title-specific API. Each worker writes only its assigned
half-open cell-row range.

## Snapshot contract

The owner snapshot contains enough already-resolved values to reproduce the
legacy calculation without a live engine read:

- cell dimensions, output stride, and assigned bounds;
- final world x/y coordinates, display heights, and neighboring height samples
  needed for all four normalized cross-product normals;
- the two UV sets, alpha values, and triangle-flip flag for each cell;
- terrain ambient/diffuse and the three global terrain-light directions;
- a bounded array of scene lights in exact iterator order, including type,
  position/direction, range and mid-range attenuation values, diffuse, and
  ambient values;
- depth-fade enable/value and captured water height.

The kernel writes a D3D-free 32-byte vertex POD equivalent field-for-field to
`VertexFormatXYZDUV2`: x, y, z, diffuse, u1, v1, u2, and v2. Compile-time and
focused runtime layout checks protect the owner copy. No packing pragma or ABI
reinterpretation is introduced.

If a scene-light snapshot cannot be represented within the checked batch
budget, the tile uses the exact legacy owner path. Lights are never truncated
or reordered.

## Numerical and byte parity

The kernel preserves the legacy operation order, including:

- `FLIP_TRIANGLES` corner ordering;
- the four independent legacy normalized cross products, without averaging or
  reusing a neighboring corner normal;
- existing `Vector3`/`WWMath::Inv_Sqrt` semantics on the x86/VC6 lane;
- scene-light iteration order, attenuation branches, ambient and diffuse sums;
- `REAL_TO_INT` conversion and color-channel clamp order;
- depth-fade and water-height comparisons;
- UV/alpha ordering and duplicated border-vertex behavior.

Tests compare an independent full-range invocation with partitioned row
invocations by exact bytes, including diffuse fields and guard regions. A
generic `sqrtf` rewrite or reordered vector/light calculation is not accepted.

## Commit and fallback policy

No partial output is published. Capture/allocation/layout/budget failure,
service unavailability, lease denial, wrapper allocation failure, all-or-none
submission rejection, worker failure, or output validation failure causes the
entire tile to run through the unchanged serial owner path. The established
service handles two-worker to one-worker retry before reporting failure.

The owner never waits while holding a D3D lock, live scene iterator, or engine
mutex. Correct ordering is capture, lease, submit/join, D3D commit, release.
Synchronous join ensures no task survives `updateBlock`, map-resource release,
LOD replacement, display reset, or shutdown. Display initialization and
shutdown continue to own the shared service in both Generals variants.

## Implementation boundaries

- `Core/Libraries/Include/Lib/HeightMapTerrainKernel.h`
- `Core/Libraries/Source/TaskRuntime/HeightMapTerrainKernel.cpp`
  - D3D-free PODs, validation, numerical helpers, and row kernel.
- `Core/GameEngineDevice/Include/W3DDevice/Common/HeightMapTerrainPrepare.h`
- `Core/GameEngineDevice/Source/W3DDevice/Common/System/HeightMapTerrainPrepare.cpp`
  - checked owner batch, light/cell capture representation, row-work adapter,
    service lease, fallback result, and output validation.
- `Core/GameEngineDevice/Source/W3DDevice/GameClient/HeightMap.cpp`
  - owner capture, eligibility, serial fallback, backup commit, and D3D upload.
- `Core/Tools/HeightMapTerrainPrepareTest/`
  - pure parity, split-row, guard, failure, lifetime, and source-boundary tests.
- shared CMake lists and `TESTING.md` only as needed.

## Validation gates

Stage 7 is ready for manual testing only after:

- red/green focused tests cover geometry, flip/UV/alpha, border/halo,
  static-lighting branches, depth fade, layout, guards, failure, and lifetime;
- existing task-runtime, texture, Stage 5 radar-terrain, and Stage 6 overlay
  tests remain green;
- both game variants build in modern x86 Debug/Profile and VC6-compatible lanes;
- source audits prove the worker boundary is D3D/global/live-pointer/allocation/
  wait/RNG/replay/save/network free;
- ten distinct Luna Max review rounds are complete and all material findings
  are fixed and revalidated;
- ten unique replay fixtures pass in twelve executions, with the 2v6 Hard-AI
  stress replay executed three times and producing identical CRC traces;
- three fresh AI-vs-AI headless smoke games pass when the established runner is
  available;
- the draft PR contains no local machine paths or personal-system details and
  remains unmerged for the user's manual test.
