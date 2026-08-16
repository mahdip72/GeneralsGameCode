# Test Replays

The GeneralsReplays folder contains replays and the required maps that are tested in CI to ensure that the game is retail compatible.

You can also test with these replays locally:
- Copy the replays into a subfolder in your `%USERPROFILE%/Documents/Command and Conquer Generals Zero Hour Data/Replays` folder.
- Copy the maps into `%USERPROFILE%/Documents/Command and Conquer Generals Zero Hour Data/Maps`
- Start the test with this: (copy into a .bat file next to your executable)
```
START /B /W generalszh.exe -jobs 4 -headless -replay subfolder/*.rep > replay_check.log
echo %errorlevel%
PAUSE
```
It will run the game in the background and check that each replay is compatible. You need to use a VC6 build with optimizations and RTS_BUILD_OPTION_DEBUG = OFF, otherwise the game won't be compatible.

Zero Hour records the skirmish-AI behavior epoch as a suffix in the replay header's existing variable-length build-time field. Unmarked retail replays use the legacy AI behavior. Replays marked `[SkirmishAILiveness=1]` use only the PR6 liveness fixes, and new recordings marked `[SkirmishAIEpoch=2]` use the current PR7-PR9 AI behavior as well. Unknown, malformed, mixed, or duplicate markers fall back to legacy behavior. Replays produced by transitional PR7-PR9 builds carried only the older liveness marker despite using later AI behavior; those recordings cannot be identified reliably and are unsupported.

# Stage 0 ownership and profiling checks

Use an optimized VC6 release-log build for the replay and CRC diagnostic:

```
cmake --preset vc6-releaselog -DRTS_BUILD_ZEROHOUR=ON -DRTS_BUILD_GENERALS=OFF
cmake --build --preset vc6-releaselog
START /B /W generalszh.exe -jobs 4 -headless -replay subfolder/*.rep > replay_check.log
echo %errorlevel%
```

Run the last two commands from the directory containing the `vc6-releaselog` Zero Hour executable after copying the replay corpus and maps as described above. A zero exit code and no CRC or ownership crash in `replay_check.log` are required. `-jobs` starts independent replay processes only; it does not create or control in-process game workers.

For Tracy frame-phase capture, build the profile configuration, start Tracy v0.13.1, then launch a game and capture several shell and in-game frames:

```
cmake --preset win32-profile -DRTS_BUILD_ZEROHOUR=ON -DRTS_BUILD_GENERALS=OFF
cmake --build --preset win32-profile
```

The capture must show `Engine.Update` and its `Radar`, `Audio`, `Client`, `MessageStream`, `Network`, `GameLogic`, and `ClientStep` child zones, plus `Platform.Win32Messages` and `Engine.FramePacer`. If Tracy cannot connect, remove `dbghelp.dll` from the game binary directory and retry.

Manually smoke-test Zero Hour first, then Generals: launch a skirmish, play through several updates, pause and resume, save and load, take a screenshot, return to the map or shell, alt-tab/minimize and restore, then quit. In crash-enabled diagnostic builds, no owner-thread assertion is expected during these flows.

# Stage 2 managed screenshot checks

Configure a modern x86 Debug build with both games and the Core test executables, then build the two games and focused tests:

```
cmake --preset win32-debug -DRTS_BUILD_GENERALS=ON -DRTS_BUILD_ZEROHOUR=ON -DRTS_BUILD_CORE_EXTRAS=ON
cmake --build build/win32-debug --config Debug --target g_generals z_generals core_task_runtime_tests core_screenshot_codec_tests
ctest --test-dir build/win32-debug -C Debug -R "^core_(task_runtime|screenshot_codec)_tests$" --output-on-failure
```

The two CTest targets must pass. Confirm the screenshot implementation has no unmanaged thread creation and that the worker task contains no live engine, render, or UI references:

```
rg -n "CreateThread|ThreadClass" Core/GameEngineDevice/Source/W3DDevice/GameClient/W3DScreenshot.cpp
rg -U -P "(?ms)^class (ScreenshotBatch|ScreenshotConvertTask).*?^};" Core/GameEngineDevice/Source/W3DDevice/GameClient/W3DScreenshot.cpp | rg -n "The[A-Z]|DX8|WW3D|SurfaceClass|GameText|InGameUI|waitUntilIdle"
```

Both source-audit commands must produce no matches. Manually verify JPEG and PNG captures in each game: issue several rapid captures, confirm successful `GUI:ScreenCapture` messages and files under the user-data `Screenshots` folder, then quit immediately after a capture to exercise draining teardown.

# Stage 3 parallel screenshot conversion checks

Run the Stage 2 focused build and CTest commands again. The screenshot codec test now starts a two-worker task runtime and verifies that striped conversion is byte-identical to serial conversion for padded, odd-sized ARGB32 and RGB565 images. It also covers the 128-row parallel threshold and deterministic full-image range coverage.

In a profile build on a machine with at least two logical processors, capture 720p or larger JPEG and PNG screenshots while connected to Tracy. Each large capture should show multiple `Screenshot.Convert` worker zones followed by exactly one `Screenshot.Encode` zone. Captures shorter than 128 rows and one-worker fallback runs should show one conversion zone. No conversion task may wait for another task.

Repeat the rapid-capture and immediate-quit manual checks for both games. Compare every output image against the displayed frame for missing, duplicated, or corrupted horizontal bands, with particular attention to rows around stripe boundaries. Successful capture messages must still be delivered from the main thread, and shutdown must drain accepted batches without a hang or access violation.

# Stage 4 background texture preparation checks

Configure a modern x86 Debug build with both games and Core extras, then build the games and all multicore-focused tests:

```
cmake --preset win32-debug -DRTS_BUILD_GENERALS=ON -DRTS_BUILD_ZEROHOUR=ON -DRTS_BUILD_CORE_EXTRAS=ON
cmake --build build/win32-debug --config Debug --target g_generals z_generals core_task_runtime_tests core_screenshot_codec_tests core_texture_mip_buffer_tests
ctest --test-dir build/win32-debug -C Debug -R "^core_(task_runtime|screenshot_codec|texture_mip_buffer)_tests$" --output-on-failure
```

The three CTest targets must pass. The texture mip-buffer suite covers uncompressed and DXT layouts, odd and sub-block dimensions, rectangular mip counts, padded source/destination pitches, overflow rejection, guarded copies, and exact-limit/rejected retained-memory reservations. Two accepted preparation copies are gated concurrently on distinct worker thread IDs and compared byte-for-byte with serial preparation. A saturated runtime also proves that rejected preparation remains caller-owned, executes synchronously on the caller, and is destroyed exactly once. The production loader additionally caps accumulated asynchronous DDS/TGA source, prepared mip, and TGA conversion memory at 64 MiB; byte-admission rejection uses synchronous owner preparation. The TaskRuntime suite separately covers queued-task ownership recovery, rejection, draining, and joins. Confirm that the legacy detached texture loader and its background queue are gone, and that the bounded worker body only prepares CPU data and publishes completion:

```
rg -n "LoaderThreadClass|_TextureLoadThread|_BackgroundQueue|_BackgroundCriticalSection" Core/Libraries/Source/WWVegas/WW3D2/textureloader.cpp
rg -U -P "(?ms)^class TexturePrepareRuntimeTask.*?^};" Core/Libraries/Source/WWVegas/WW3D2/textureloader.cpp | rg -n "DX8|D3D|Texture->|Apply|Lock_Surfaces|Unlock_Surfaces|Create_D3D"
```

Both source-audit commands must produce no matches. Build both games with VC6 as well as modern Win32. Run the optimized `vc6-releaselog` replay command documented in Stage 0 and require a zero exit code with no CRC or ownership failure; the Stage 4 texture-preparation changes must not modify gameplay, replay, network, save, RNG, or serialization state.

For Tracy validation, use a profile build on a machine with at least two logical processors and load texture-heavy maps in both games. The capture should show up to two concurrent `Texture.Prepare` worker zones, with their corresponding `Texture.Upload` zones on the render owner thread. No worker may call Direct3D, dereference a live texture, wait for another worker, or access engine globals.

Manually test Zero Hour and Generals by loading several skirmish maps and armies, moving the camera quickly across previously unseen terrain and units, returning to the shell, loading another map, alt-tabbing during map loading, and quitting during or immediately after a load. Verify that terrain, unit, effect, UI, and cube-map textures have no missing-texture placeholders, corruption, delayed permanent blur, device loss, hang, or access violation. The shipped DDS backends do not expose volume-level memory, so Stage 4 rejects DDS volume preparation through the existing missing-texture path instead of uploading uninitialized data; volume rendering is not claimed as supported. Also repeat screenshot capture and save/load smoke tests to guard the earlier multicore stages.

# Stage 5 radar terrain preparation checks

Stage 5 moves only the CPU preparation of the radar terrain raster into a
synchronous, bounded fork-join.  The owner thread still reads live terrain,
bridge, water, and visual state, owns the immutable POD batch, and performs
all Direct3D surface access and the final row upload.  Worker tasks receive no
engine pointer or global and only shade their assigned rows.  This stage does
not parallelize simulation, AI, pathfinding, replay/save/network state, or
radar map mutation.

The following implementation and test coverage is present in the committed
Stage 5 source.  These checkmarks describe source/test evidence only; they do
not claim that the commands below have run successfully:

- [x] The D3D-free kernel preserves the legacy center-branch order, clipped
  3x3 traversal, interpolation guards and argument order, averaging, and
  format bytes.  Serial and split-row calls are compared byte-for-byte,
  including guarded 24-bit rows and the supported 16/32-bit formats.
- [x] The owner batch uses checked sizes and a fixed staging budget, owns the
  cell/output storage, and rejects incomplete or unsupported batches without
  writing output.
- [x] The service admits one consumer, submits exactly two disjoint row
  ranges, and covers two-worker success, one-worker retry, queue rejection,
  submission rollback, task-allocation failure, serial-oracle fallback,
  release, shutdown, and clean restart cases.
- [x] Source audits cover the worker/kernel prohibition on Direct3D, globals,
  live engine pointers, allocation, waits, RNG, replay/save/network calls.
- [x] Both title display variants initialize the service after their display
  prerequisites and shut it down before render teardown, with game-thread
  assertions.  `W3DRadar` keeps acquisition, join/fallback, surface lock,
  upload, unlock, and release on the owner thread.
- [ ] Focused tests, the modern x86 title builds, and the VC6 compatibility
  build have not yet been executed for the final Stage 5 head.
- [ ] The ten-review-lens code review has not yet been completed.
- [ ] The optimized VC6 replay gate has not yet been executed.
- [ ] Interactive manual acceptance is intentionally deferred until the
  complete Stage 5--8 stack is ready; do not launch this intermediate stage.

## Stage 5 focused validation

Run from a clean, repository-relative build directory.  Do not record local
machine paths, profile paths, build-tree paths, or replay logs in repository
documentation or PR text.

First run the source hygiene check and focused modern build/test targets:

```powershell
git diff --check
cmake --preset win32-debug -DRTS_BUILD_GENERALS=ON -DRTS_BUILD_ZEROHOUR=ON -DRTS_BUILD_CORE_EXTRAS=ON
cmake --build build/win32-debug --config Debug --target radar_terrain_prepare_tests core_task_runtime_tests core_texture_mip_buffer_tests g_generals z_generals --parallel 2
ctest --test-dir build/win32-debug -C Debug -R "^(radar_terrain_prepare|core_task_runtime|core_texture_mip_buffer)_tests$" --output-on-failure
```

The radar target is expected to exercise serial-versus-two-range byte parity,
edge/clipping and zero-sample behavior, bridge/water/regular branches,
equal-height interpolation guards, supported format packing, row ownership,
bounded owner storage, exclusive leases, two-worker admission, one-worker
retry, queue/backpressure and rollback fallback, shutdown/restart, and the
worker/owner source audits.  A test/build result must be captured before any
checkmark is added to the execution items above.

Then run the legacy compatibility lane using an actual optimized VC6 build;
do not substitute a modern Win32 executable for replay evidence:

```powershell
cmake --preset vc6 -DRTS_BUILD_GENERALS=ON -DRTS_BUILD_ZEROHOUR=ON -DRTS_BUILD_CORE_EXTRAS=ON
cmake --build build/vc6 --target radar_terrain_prepare_tests core_task_runtime_tests core_texture_mip_buffer_tests g_generals z_generals --parallel 2
```

If the VC6 toolchain or required game data is unavailable, report that exact
external prerequisite and leave the replay gate pending.

## Stage 5 replay gate

After focused/build validation, use the repository's ten distinct replay
fixtures without modifying the live profile.  The corpus must contain several
2v2 matches, one 2v6 Hard-AI stress match, one 2v2v2 match, one 3v3 match, the
golden/reference replay, and two additional corpus replays.  Run the nine
non-stress replays once and the 2v6 stress replay three times in separate CRC
output directories: twelve replay process executions across ten unique
replays.

Each execution must exit with code 0 and have no CRC mismatch, ownership
failure, assertion, crash, or missing-map error.  The three stress executions
must produce identical CRC file sets and byte-for-byte hashes.  `-jobs 4`
starts independent replay processes; it is not an in-process worker-count
switch.  Record only repository-relative commands and aggregate results, not
machine-specific paths or logs.

The ten review lenses for the final Stage 5 head are: owner boundary,
complete immutable snapshot, legacy byte parity, disjoint row ownership,
private-runtime wait isolation, deterministic fallback behavior, bounded
memory and lifetime, both-display lifecycle, C++98/VC6 plus replay evidence,
and scoped delivery/privacy hygiene.  All ten review passes and any resulting
fix/retest cycle must complete before Stage 5 is considered ready for the
stacked-stage handoff.

# Stage 6 radar overlay preparation checks

Stage 6 keeps radar object/shroud capture and every Direct3D operation on the
owner thread.  Workers receive only immutable ordered commands plus owned CPU
pixels, write disjoint row ranges, and join before owner upload.  Small object
lists stay on the direct owner path; large object lists use command-major row
work.  Shroud overflow folds ordered chunks into the owned image, and failed
worker/upload attempts retain a complete image until an owner commit succeeds
or clear/reset/teardown explicitly supersedes it.

Run the repository-relative hygiene, focused, and both-title checks:

```powershell
git diff --check
cmake --preset win32-debug -DRTS_BUILD_GENERALS=ON -DRTS_BUILD_ZEROHOUR=ON -DRTS_BUILD_CORE_EXTRAS=ON
cmake --build build/win32-debug --config Debug --target radar_overlay_prepare_tests radar_terrain_prepare_tests core_task_runtime_tests g_generals z_generals --parallel 2
ctest --test-dir build/win32-debug -C Debug -R "^(radar_overlay_prepare|radar_terrain_prepare|core_task_runtime)_tests$" --output-on-failure
```

The overlay test must cover both supported formats, clipping, inclusive shroud
rectangles, exact object/shroud command order, last-writer-wins, row guards,
serial-versus-split bytes, checked storage limits, lease denial, queue/task
failure fallback, and worker/owner source audits.  Build the same targets with
the optimized VC6 preset before replay validation; a modern executable is not
a substitute for the compatibility lane.

For the Stage 6 replay gate, use the same ten distinct fixtures defined above:
run the nine non-stress replays once and the 2v6 Hard-AI replay three times.
All twelve executions must exit successfully without CRC mismatch, assertion,
crash, missing map, or ownership failure, and the three stress CRC trees must
be byte-identical.  Keep replay state isolated from the playable profile and
record no personal machine paths in commits, documentation, or PR text.

Manual acceptance remains deferred to the user.  When requested, test Stage 5
first and Stage 6 second, checking radar terrain, object markers, stealth
alpha, shroud/fog transitions, radar clears, map return/reload, Alt-Tab/device
reset, dense battles, and clean exit.  Do not start Stage 7 before both manual
test rounds are complete.

# Miles completion callback checks

The Miles EOS callbacks must only publish a fixed-size `{handle, type, generation}` record. They must not enter `TheAudio`, call Miles APIs, allocate, or take the audio-cache mutex. The owner-thread `MilesAudioManager::update()` drains one queue snapshot per frame; reset and shutdown close admission before unregistering callbacks and releasing handles, then clear queued generations. On overflow, the owner drains status-visible stopped handles and uses the compatibility fallback rather than waiting in a callback.

Build and run the bounded queue test with Core extras enabled:

```
cmake --build build/win32-debug --config Debug --target core_miles_audio_completion_tests
ctest --test-dir build/win32-debug -C Debug -R "^core_miles_audio_completion_tests$" --output-on-failure
```

The test covers FIFO delivery, concurrent producers, bounded overflow/recovery, snapshot draining, close/reopen admission, reset generations, and exact-once records. A source audit must show no callback-to-manager call:

```
rg -n "set(Sample|3DSample|Stream)Completed|TheAudio->notifyOfAudioCompletion" Core/GameEngineDevice/Source/MilesAudioDevice/MilesAudioManager.cpp
```

The callback declarations/definitions may match, but the callback bodies must contain only `tryPublish`; all `notifyOfAudioCompletion` calls must be on the owner-thread drain/recovery path. During manual gameplay, stress dense combat and rapid sound effects, reset/return to shell, alt-tab, and exit while sounds are active. The game must remain responsive and must not show the previous audio hang; no callback-thread stack may contain `ScopedMutex::Lock` or `AudioFileCache` operations.
