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
rg -U -P "(?ms)^class ScreenshotEncodeTask.*?^};" Core/GameEngineDevice/Source/W3DDevice/GameClient/W3DScreenshot.cpp | rg -n "The[A-Z]|DX8|WW3D|SurfaceClass|GameText|InGameUI"
```

Both source-audit commands must produce no matches. Manually verify JPEG and PNG captures in each game: issue several rapid captures, confirm successful `GUI:ScreenCapture` messages and files under the user-data `Screenshots` folder, then quit immediately after a capture to exercise draining teardown.
