# Stage 8 Dynamic Terrain-Light Preparation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Parallelize only the CPU portion of dynamic terrain-light preparation while preserving the legacy render-owner boundary and byte-identical output.

**Architecture:** Capture immutable tile/light/vertex inputs on the owner, use the existing bounded `RadarTerrainPrepareService` for disjoint row tasks, validate the staged rows, then perform dynamic D3D writes on the owner. The backup vertex array remains the immutable static-light baseline; any failure uses the original serial lighting path. The default non-optimized vertex layout has no shared hardware vertices; optimized-lighting builds stay on the serial implementation that owns duplicate propagation.

**Tech Stack:** C++98/VC6-compatible POD kernels, existing `TaskRuntime`/`RadarTerrainPrepareService`, CMake Core extras/CTest, DirectX 8 owner-thread upload, modern x86 and VC6 builds.

## Global Constraints

- No worker accesses D3D, `The*` globals, live map/light objects, allocators, logging, replay, network, audio, or UI.
- Shared Generals/Zero Hour source must remain symmetric; no replay serialization or epoch changes.
- Existing serial `updateVBForLightOptimized` (or the compiled nonoptimized reference) is the reference and fallback.
- Worker row ranges are disjoint; workers never wait for child tasks.
- Use only the existing bounded `RadarTerrainPrepareService`, with a distinct consumer id and join-before-release.
- Preserve C++98/VC6 compatibility and run both title builds plus the required replay corpus.
- Keep generated builds and isolated runtimes under operator-local H: paths; never publish those paths in repository files or PR text.

---

### Task 1: Capture the approved design and baseline

**Files:**
- Create: `docs/superpowers/specs/2026-08-17-multicore-stage-8-dynamic-lighting-design.md`
- Create: `docs/superpowers/plans/2026-08-17-multicore-stage-8-dynamic-lighting.md`

- [ ] **Step 1: Verify the branch and clean baseline**

Run `git status --short --branch`, confirm the branch is based on the pathfinding-ready head, and run the existing focused CTest configuration before editing production code.

- [ ] **Step 2: Commit the design and plan**

Stage only the two repository-relative documents and commit them with a conventional `docs(stage8): define dynamic light preparation` subject.

### Task 2: Add the pure dynamic-light POD kernel (TDD)

**Files:**
- Create: `Core/Libraries/Include/Lib/HeightMapDynamicLightKernel.h`
- Create: `Core/Libraries/Source/TaskRuntime/HeightMapDynamicLightKernel.cpp`
- Modify: `Core/Libraries/Source/TaskRuntime/CMakeLists.txt`
- Create: `Core/Tools/HeightMapDynamicLightPrepareTest/HeightMapDynamicLightPrepareTest.cpp`
- Create: `Core/Tools/HeightMapDynamicLightPrepareTest/CMakeLists.txt`
- Modify: `Core/Tools/CMakeLists.txt` (or the existing Core-extras registration)

- [ ] **Step 1: Write failing serial/parity tests**

Construct small POD batches with four-corner vertices and deterministic point, spot, directional, disabled, attenuation, clipping, wrap, alpha, and guard-byte cases. Assert the desired API is missing so the test fails for the expected compile/link reason.

- [ ] **Step 2: Implement the minimal POD types and serial row kernel**

Implement finite/range validation, legacy light order, owner-computed normal preservation, alpha preservation, RGB truncation, and disjoint row writes without engine includes or allocations. Keep the complete lighting oracle in tests while using structural validation on the owner publication path.

- [ ] **Step 3: Add failure/guard tests and validate red/green transitions**

Exercise malformed dimensions/strides, null/overlapping buffers, non-finite values, incomplete sentinel output, arbitrary row splits, and unchanged destinations on rejection. Run the focused executable after each red/green cycle.

- [ ] **Step 4: Commit the kernel and tests**

Use a conventional `perf(stage8): add pure dynamic light kernel` subject after fresh focused-test evidence.

### Task 3: Add the owner capture/row adapter and service consumer

**Files:**
- Create: `Core/GameEngineDevice/Include/W3DDevice/Common/HeightMapDynamicLightPrepare.h`
- Create: `Core/GameEngineDevice/Source/W3DDevice/Common/System/HeightMapDynamicLightPrepare.cpp`
- Modify: `Core/GameEngineDevice/Include/W3DDevice/Common/RadarTerrainPrepare.h`
- Modify: `Core/GameEngineDevice/Source/W3DDevice/Common/System/RadarTerrainPrepare.cpp`
- Modify: `Core/GameEngineDevice/CMakeLists.txt`

- [ ] **Step 1: Add service/adapter lifecycle tests first**

Extend the focused test with gated two-worker execution, distinct worker IDs, rejected-task caller ownership, one-worker retry, and shutdown joining. Verify these tests fail before the new adapter/consumer exists.

- [ ] **Step 2: Implement bounded owner-owned batch storage and capture validation**

Copy required vertex, normal, bounds-mask, and light fields on the owner; reject overflow, impossible tile dimensions, and unbounded light counts before runtime admission. Keep all allocations in owner code.

- [ ] **Step 3: Implement the generic row adapter and consumer id**

Use `RadarPrepareRowWork::executeRows`, the shared service lease, and a disjoint two-row split. Retry one worker on start/submission failure and return false for serial fallback. Do not add a second runtime or worker wait.

- [ ] **Step 4: Run focused tests and commit**

Run the new focused CTest and existing task-runtime/terrain tests; commit with `perf(stage8): add dynamic light preparation adapter`.

### Task 4: Integrate owner-side lighting publication in HeightMap

**Files:**
- Modify: `Core/GameEngineDevice/Include/W3DDevice/GameClient/HeightMap.h`
- Modify: `Core/GameEngineDevice/Source/W3DDevice/GameClient/HeightMap.cpp`

- [ ] **Step 1: Add a serial-reference integration fixture**

Expose a test-only comparison seam or common helper that captures a tile and compares staged diffuse values against `updateVBForLightOptimized`, including duplicate vertices and alpha. Keep the production path unchanged while the test is red.

- [ ] **Step 2: Capture on the owner before any worker admission**

Preserve the current light-bound updates and tile intersection logic. Copy backup vertices, owner-computed corner normals, bounds masks, and light snapshots while iterating live engine objects on the owner.

- [ ] **Step 3: Run the batch and validate before locking D3D**

Acquire the Stage 8 consumer, run row tasks, validate all output, lock the tile buffer, scatter only masked RGB/alpha values to hardware, unlock, and release the lease. Never overwrite `m_vertexBufferBackup`: it is the source baseline for the next frame. If any step fails, call the unchanged serial function without touching partial staged output or the backup.

- [ ] **Step 4: Verify reset and device lifecycle**

Confirm update/reset/reacquire/destructor paths drain the service before destroying batch or vertex storage. Add owner assertions where the existing height-map code already asserts ownership.

- [ ] **Step 5: Commit the integration**

Use `perf(stage8): parallelize dynamic terrain lighting` after focused tests and source audit evidence.

### Task 5: Register both titles and lifecycle initialization

**Files:**
- Modify: `TESTING.md`

- [ ] **Step 1: Add lifecycle tests or test hooks**

Cover initialize-before-use, shutdown admission closure, repeated reset/reacquire, and no task surviving display teardown.

- [ ] **Step 2: Reuse the existing shared display lifecycle**

Both title copies already initialize and shut down `RadarTerrainPrepareService` before height-map/D3D destruction. Add only the named Stage 8 consumer-5 lease; do not create a second runtime or duplicate display lifecycle calls. Keep synchronous fallback enabled when the shared service cannot run.

- [ ] **Step 3: Verify reset and teardown ordering**

Confirm the synchronous adapter joins every accepted task before returning, and that existing display shutdown closes admission and joins workers before height-map buffers are freed. Do not add a reset-time shutdown to a synchronous service.

- [ ] **Step 4: Document Stage 8 gates**

Describe pure parity tests, modern/VC6 builds, the 10-fixture/12-execution replay gate, and the manual dynamic-light stress checklist without claiming unexecuted integration coverage.

- [ ] **Step 5: Commit lifecycle/docs**

Use `feat(stage8): enable dynamic lighting worker lifecycle`.

### Task 6: Full verification and review gate

**Files:**
- Modify only files required by test or review findings.

- [ ] **Step 1: Run full focused CTest suites**

Run all Core extras tests in modern x86 Debug/Release/profile configurations and the VC6-compatible lane, then build both `g_generals` and `z_generals` from the exact branch head.

- [ ] **Step 2: Run ten distinct Luna Max review rounds**

Use separate lenses for kernel math, owner/worker boundary, service lifecycle, D3D publication, duplicate vertices, failure/OOM fallback, C++98/VC6/title symmetry, test credibility, performance/budget, and adversarial replay integration. Fix every Critical/Important finding and re-review the fix range before advancing.

- [ ] **Step 3: Run the complete replay gate**

Use the clean VC6/releaselog artifact in an isolated installed runtime. Execute the ten unique replay files with the 2v6 replay twice extra, for 12 total executions, and compare the three 2v6 CRC traces byte-for-byte.

- [ ] **Step 4: Run the live AI-vs-AI smoke gate**

Run the required sequential headless 4-vs-3 skirmish smoke fixtures if the repository runner is available; otherwise record the exact external prerequisite rather than claiming a pass.

- [ ] **Step 5: Prepare manual test handoff**

Push the branch/update its PR without merging, retain the isolated candidate until the user tests, and wait. Do not promote or launch the canonical build without explicit user approval.
