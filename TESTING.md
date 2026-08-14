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

The three CTest targets must pass. The texture mip-buffer suite covers uncompressed and DXT layouts, odd and sub-block dimensions, rectangular mip counts, padded source/destination pitches, overflow rejection, guarded copies, and two worker-submitted outputs compared byte-for-byte with serial preparation. The TaskRuntime suite separately covers queued-task ownership recovery, rejection, draining, and joins. Confirm that the legacy detached texture loader and its background queue are gone, and that the bounded worker body only prepares CPU data and publishes completion:

```
rg -n "LoaderThreadClass|_TextureLoadThread|_BackgroundQueue|_BackgroundCriticalSection" Core/Libraries/Source/WWVegas/WW3D2/textureloader.cpp
rg -U -P "(?ms)^class TexturePrepareRuntimeTask.*?^};" Core/Libraries/Source/WWVegas/WW3D2/textureloader.cpp | rg -n "DX8|D3D|Texture->|Apply|Lock_Surfaces|Unlock_Surfaces|Create_D3D"
```

Both source-audit commands must produce no matches. Build both games with VC6 as well as modern Win32. Run the optimized `vc6-releaselog` replay command documented in Stage 0 and require a zero exit code with no CRC or ownership failure; Stage 4 must not modify gameplay, replay, network, save, RNG, or serialization state.

For Tracy validation, use a profile build on a machine with at least two logical processors and load texture-heavy maps in both games. The capture should show up to two concurrent `Texture.Prepare` worker zones, with their corresponding `Texture.Upload` zones on the render owner thread. No worker may call Direct3D, dereference a live texture, wait for another worker, or access engine globals.

Manually test Zero Hour and Generals by loading several skirmish maps and armies, moving the camera quickly across previously unseen terrain and units, returning to the shell, loading another map, alt-tabbing during map loading, and quitting during or immediately after a load. Verify that terrain, unit, effect, UI, and cube-map textures have no missing-texture placeholders, corruption, delayed permanent blur, device loss, hang, or access violation. The shipped DDS backends do not expose volume-level memory, so Stage 4 rejects DDS volume preparation through the existing missing-texture path instead of uploading uninitialized data; volume rendering is not claimed as supported. Also repeat screenshot capture and save/load smoke tests to guard the earlier multicore stages.
