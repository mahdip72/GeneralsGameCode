# Multicore Stage 4 Texture Preparation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Prepare DDS/TGA texture mip data on up to two bounded workers while retaining all engine-global, texture-reference, and Direct3D work on the render-owner thread.

**Architecture:** The render owner loads and snapshots source images, then submits one self-contained CPU preparation task per texture to the Stage 1 runtime. Workers write tightly packed CPU mip buffers and publish completion through the existing synchronized texture foreground queue; the render owner creates and uploads the D3D texture. High-priority, rejected, and runtime-start-failure paths run the same preparation synchronously.

**Tech Stack:** C++98-compatible Core/WW3D2 code, DirectX 8 owner-thread upload, `rts::TaskRuntime`, CMake/CTest, VC6 and modern x86 MSVC, Tracy, optimized VC6 replay validation.

## Global Constraints

- Use `H:\Games` for all build trees, runtime copies, logs, replay state, and disposable artifacts.
- Never build, launch, or modify runtime state while `generalszh.exe` or `generalsv.exe` is running.
- Workers must not dereference `TextureBaseClass`, call DX8/D3D, access `_TheFileFactory`, access thumbnail/global engine state, or wait for other tasks.
- Preserve C++98 and VC6 compatibility; do not use `std::thread`, lambdas, atomics, `std::nothrow` array new, or post-C++98 library APIs.
- Preserve texture bytes by reusing `DDSFileClass` and `BitmapHandlerClass` conversion code.
- The texture-preparation implementation must not modify simulation, pathfinding, networking, replay/save serialization, RNG, map parsing, or state layout. The post-Stage-4 replay epoch is a base-integration exception required to preserve retail replay determinism after PR #3; it must not change live-game AI behavior.
- Miles audio completion callbacks must publish only fixed-size records to the bounded preallocated ring. They must not allocate, wait, call Miles APIs, or enter `MilesAudioManager`; the owner drains and recovers completions during `update()`.
- Run heavy validation sequentially and only after focused tests are stable.
- Stage 5 remains out of scope until the user manually accepts Stage 4.

---

### Task 1: D3D-free mip layout and upload-copy primitive

**Files:**
- Create: `Core/Libraries/Source/WWVegas/WW3D2/texturemipbuffer.h`
- Create: `Core/Libraries/Source/WWVegas/WW3D2/texturemipbuffer.cpp`
- Create: `Core/Tools/TextureMipBufferTest/CMakeLists.txt`
- Create: `Core/Tools/TextureMipBufferTest/TextureMipBufferTest.cpp`
- Modify: `Core/Libraries/Source/WWVegas/WW3D2/CMakeLists.txt`
- Modify: `Core/Tools/CMakeLists.txt`

**Interfaces:**
- Produces: `TextureMipLayout { size_t rowPitch, rowCount, slicePitch, dataSize; }`.
- Produces: `bool CalculateTextureMipLayout(WW3DFormat, unsigned width, unsigned height, unsigned depth, TextureMipLayout&)`.
- Produces: `bool CopyTextureMipData(const unsigned char* source, const TextureMipLayout& sourceLayout, unsigned char* destination, const TextureMipLayout& destinationLayout, unsigned depth)`.
- Consumes: `WW3DFormat` only; neither function includes or calls D3D.

- [ ] **Step 1: Add failing layout tests**

Create a console test that asserts exact layouts for `A8R8G8B8`, `R5G6B5`, `R8G8B8`, `L8`, `DXT1`, and `DXT5`; odd/sub-block sizes; zero dimensions; and multiplication overflow. Assert guard bytes remain unchanged when copying two uncompressed slices and DXT block rows into a padded destination.

- [ ] **Step 2: Configure the focused test and verify RED**

Run:

```powershell
cmake -S . -B H:\Games\GeneralsGameCode-Stage4-build-debug -G "Ninja Multi-Config" -A Win32 -DRTS_BUILD_GENERALS=ON -DRTS_BUILD_ZEROHOUR=ON -DRTS_BUILD_CORE_EXTRAS=ON
cmake --build H:\Games\GeneralsGameCode-Stage4-build-debug --config Debug --target core_texture_mip_buffer_tests --parallel 2
```

Expected: compilation fails because `texturemipbuffer.h` and its functions do not exist.

- [ ] **Step 3: Implement the minimum pure helper**

Use checked multiplication and the exact DXT block rules from the design. Reject unknown or unsupported formats, zero dimensions, pitch mismatch, and overflow. Copy each row with `memcpy`, advancing by explicit row and slice pitches.

- [ ] **Step 4: Verify GREEN and existing focused tests**

Run:

```powershell
cmake --build H:\Games\GeneralsGameCode-Stage4-build-debug --config Debug --target core_texture_mip_buffer_tests core_task_runtime_tests core_screenshot_codec_tests --parallel 2
ctest --test-dir H:\Games\GeneralsGameCode-Stage4-build-debug -C Debug -R "^core_(texture_mip_buffer|task_runtime|screenshot_codec)_tests$" --output-on-failure
```

Expected: 3/3 tests pass.

- [ ] **Step 5: Commit the primitive**

```powershell
git add Core/Libraries/Source/WWVegas/WW3D2/texturemipbuffer.* Core/Tools/TextureMipBufferTest Core/Tools/CMakeLists.txt Core/Libraries/Source/WWVegas/WW3D2/CMakeLists.txt
git commit -m "test(texture): Add CPU mip buffer coverage"
```

### Task 2: Owner-staged texture sources and CPU output buffers

**Files:**
- Modify: `Core/Libraries/Source/WWVegas/WW3D2/textureloader.h`
- Modify: `Core/Libraries/Source/WWVegas/WW3D2/textureloader.cpp`
- Modify: `Core/Tools/TextureMipBufferTest/TextureMipBufferTest.cpp`

**Interfaces:**
- Consumes: `CalculateTextureMipLayout` and `CopyTextureMipData` from Task 1.
- Produces: owner-only `Begin_Load()` source staging and prepared-surface allocation.
- Produces: worker-safe `Load()` that touches only staged source objects, scalar snapshots, and CPU buffers.
- Produces: owner-only `End_Load()` D3D creation/upload and exact-once staged-memory release.

- [ ] **Step 1: Add a failing two-worker byte-equivalence test**

Extend the helper test with two `rts::Task` instances copying distinct synthetic mip chains concurrently. Compare every output and guard byte with the serial helper result. The production change that makes this pass is a reentrant pure mip-copy primitive with no shared mutable state.

- [ ] **Step 2: Run the focused test and verify RED**

Run `core_texture_mip_buffer_tests` and verify the new concurrent-chain API/test fixture fails for the missing chain operation, not for test setup.

- [ ] **Step 3: Add texture-task staging state**

In `TextureLoadTaskClass`, snapshot compression permission and filename on the owner. Add owned DDS/TGA source pointers, preparation success, and per-mip prepared buffers/layouts. Cube tasks own six face/mip entries; volume tasks retain explicit slice layouts. Initialize and release every field through the existing pool lifecycle.

- [ ] **Step 4: Move source loading to the owner**

Refactor `Begin_Compressed_Load()` and `Begin_Uncompressed_Load()` so they compute existing format/reduction decisions, load the complete DDS/TGA source while engine globals are legal, close source files, and allocate CPU outputs. Remove D3D creation and surface locking from this phase.

- [ ] **Step 5: Redirect existing conversion to CPU buffers**

Make `Load_Compressed_Mipmap()` and `Load_Uncompressed_Mipmap()` consume only staged source objects and copied fields. Keep the existing conversion calls and mip loop order, but point destinations at tight CPU layouts. Add `Texture.Prepare` profiling around this CPU work.

- [ ] **Step 6: Upload only on the render owner**

In `End_Load()`, assert the DX8 owner, create supported regular/cube D3D resources, lock them, copy prepared rows through `CopyTextureMipData`, unlock, apply, and release staged data. Add `Texture.Upload` profiling. The shipped DDS volume accessor is a null stub, so reject that path through the existing missing-texture fallback instead of uploading undefined data. On failure, release partial resources and apply the existing missing texture.

- [ ] **Step 7: Verify focused tests and both modern game builds**

Run:

```powershell
cmake --build H:\Games\GeneralsGameCode-Stage4-build-debug --config Debug --target core_texture_mip_buffer_tests g_generals z_generals --parallel 2
ctest --test-dir H:\Games\GeneralsGameCode-Stage4-build-debug -C Debug -R "^core_texture_mip_buffer_tests$" --output-on-failure
```

Expected: focused test passes and both game targets compile/link.

- [ ] **Step 8: Commit the staging boundary**

```powershell
git add Core/Libraries/Source/WWVegas/WW3D2/textureloader.h Core/Libraries/Source/WWVegas/WW3D2/textureloader.cpp Core/Tools/TextureMipBufferTest/TextureMipBufferTest.cpp
git commit -m "refactor(texture): Stage mip data outside Direct3D"
```

### Task 3: Bounded two-worker texture preparation service

**Files:**
- Modify: `Core/Libraries/Source/WWVegas/WW3D2/textureloader.cpp`
- Modify: `Core/Libraries/Source/WWVegas/WW3D2/textureloader.h`
- Modify: `Core/Tools/TextureMipBufferTest/TextureMipBufferTest.cpp`

**Interfaces:**
- Consumes: `rts::TaskRuntime::start`, `trySubmit`, `tryTake`, and `shutdown`.
- Produces: private `TexturePrepareTask` whose `execute()` calls only worker-safe preparation and publishes completion.
- Produces: synchronous owner fallback for runtime-start, allocation, queue-admission, and 64 MiB retained-staging admission failure.
- Produces: joined shutdown followed by owner-thread completion draining.

- [ ] **Step 1: Add failing runtime ownership tests**

Add test fixtures proving two accepted preparation tasks run on worker thread IDs, a rejected task remains caller-owned and can execute synchronously, and shutdown drains accepted work exactly once. Use native gates/events, never sleeps for synchronization.

- [ ] **Step 2: Run the focused test and verify RED**

Verify the test fails because the texture preparation service behavior is absent.

- [ ] **Step 3: Replace `LoaderThreadClass` execution**

Remove the legacy loader thread and background queue. Add a private `TaskRuntime` service started with `min(2, logicalProcessorCount)` workers and queue capacity eight. Wrap each low-priority prepared texture in one `TexturePrepareTask`. Before submission, reserve its exact retained DDS/TGA source, prepared mip buffers, and possible TGA conversion scratch against a 64 MiB owner-managed asynchronous budget; include all six cube faces.

- [ ] **Step 4: Implement bounded fallback and completion**

If wrapper allocation, byte-budget reservation, or `trySubmit` fails, run `Load()` synchronously and finish the texture on the owner. Accepted workers atomically publish completion and the opaque texture task to `_ForegroundQueue`; owner processing calls `End_Load()` and recycles it. A foreground request uses `tryTake` to reclaim queued work immediately, or waits only when that texture is already active.

- [ ] **Step 5: Implement deterministic teardown**

Stop accepting new work, call `shutdown()` to drain/join, process every foreground completion on the DX8 owner, then destroy thumbnails and task pools. Assert no prepared source or CPU buffer survives task recycling.

- [ ] **Step 6: Run focused tests and source-boundary audits**

Run the three focused CTests, then inspect the worker class body:

```powershell
rg -U -P "(?ms)^class TexturePrepareTask.*?^};" Core/Libraries/Source/WWVegas/WW3D2/textureloader.cpp | rg -n "Texture->|DX8|D3D|_TheFileFactory|Thumbnail|waitUntilIdle|ThreadClass"
rg -n "LoaderThreadClass|_TextureLoadThread|_BackgroundQueue" Core/Libraries/Source/WWVegas/WW3D2/textureloader.cpp Core/Libraries/Source/WWVegas/WW3D2/textureloader.h
```

Expected: both audits produce no matches.

- [ ] **Step 7: Commit the worker service**

```powershell
git add Core/Libraries/Source/WWVegas/WW3D2/textureloader.h Core/Libraries/Source/WWVegas/WW3D2/textureloader.cpp Core/Tools/TextureMipBufferTest/TextureMipBufferTest.cpp
git commit -m "perf(texture): Prepare mip data on bounded workers"
```

### Task 4: Documentation, compatibility, replay, and deployable build

**Files:**
- Modify: `TESTING.md`

**Interfaces:**
- Produces: exact Stage 4 automated and manual validation commands.
- Produces: playable profile runtime under `H:\Games` with a recoverable pre-Stage-4 backup.

- [ ] **Step 1: Document Stage 4 validation**

Add focused CTest, source-audit, Tracy, lifecycle, and manual game checks from the design to `TESTING.md`.

- [ ] **Step 2: Run modern Debug verification sequentially**

With no game process running, freshly configure the H: Debug tree, build `core_task_runtime_tests`, `core_screenshot_codec_tests`, `core_texture_mip_buffer_tests`, `g_generals`, and `z_generals`, then run the three CTests. Record command output and exit codes.

- [ ] **Step 3: Run Win32 profile/release verification sequentially**

Configure separate H: profile and release trees. Build both games with at most two compiler jobs. Do not run profile/release builds concurrently.

- [ ] **Step 4: Run VC6 matrix verification sequentially**

Using the already available VC6 environment/artifact procedure, configure/build Release, Debug, Profile, and release-log configurations for both affected games and all Core-extra tests. Run CTest for configurations that register the tests. Record unavailable toolchain prerequisites precisely instead of substituting an incompatible compiler.

- [ ] **Step 5: Run isolated optimized replay validation**

Create a disposable runtime/profile under `H:\Games`, copy only already available maps and replays, and run the `TESTING.md` optimized VC6 command with `-jobs 4 -headless -replay`. Capture the exact command, log, exit code, replay count, CRC failures, and error count. Never use or overwrite the playable profile.

- [ ] **Step 6: Inspect and fix any failures test-first**

For each failure, establish whether it reproduces on `origin/main`. If Stage 4 caused it, add a focused failing test, implement the smallest in-scope fix, and rerun the affected focused/build/replay gate before continuing.

- [ ] **Step 7: Deploy the profile build for manual testing**

Back up the existing playable executables/symbols under `H:\Games\GeneralsGameCode-ZeroHour`. Install the Stage 4 profile binary into the playable runtime, verify hashes against the build output, and provide the exact launcher path. Do not launch the game unless the user requests it.

- [ ] **Step 8: Final verification and commit**

Run `git diff --check`, inspect the complete branch diff against `origin/main`, ensure no unrelated files or artifacts are tracked, and commit:

```powershell
git add TESTING.md
git commit -m "docs(testing): Add stage 4 texture validation"
```

- [ ] **Step 9: Push and update the draft PR**

Push `codex/multicore-stage-4-textures` to `mahdip72/GeneralsGameCode` and create an unmerged draft PR targeting `main`. Include the ownership contract, validation evidence, manual checklist, known limitations, and AI-generated-code disclosure required by the repository.
