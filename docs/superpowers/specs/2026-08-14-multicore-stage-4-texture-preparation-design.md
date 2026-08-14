# Multicore Stage 4: CPU Texture Preparation Design

## Goal

Move texture pixel conversion and mip preparation onto a bounded two-worker runtime while keeping every Direct3D operation, texture reference change, and engine-global access on the render-owner thread.

Stage 4 is one independently testable rendering-asset slice. It does not change simulation, pathfinding, networking, replay serialization, random-number generation, map parsing, or save data. Stage 5 remains undefined until Stage 4 is manually accepted.

## Existing architecture and problem

`TextureLoader` currently has one legacy `LoaderThreadClass`. The render thread calls `Begin_Load()`, creates a D3D texture, locks all mip surfaces, and places a `TextureLoadTaskClass` on a background queue. The legacy worker dereferences the live `TextureBaseClass`, reads DDS or TGA data, and writes pixels directly through D3D lock pointers. The render thread later unlocks and applies the texture.

This design has three unsafe boundaries:

- the worker sees a mutable, reference-counted engine texture;
- the worker writes D3D-owned memory even though DX8 ownership belongs to the render thread;
- shutdown uses the legacy `ThreadClass` stop mechanism instead of a bounded drain and join.

The existing Stage 1 `rts::TaskRuntime` already provides bounded admission, joined shutdown, exact task ownership, and portable C++98 behavior. Stage 4 replaces only the texture loader's background execution with that runtime.

## Approaches considered

### Selected: staged source plus CPU mip buffers

The render owner snapshots metadata and loads the source DDS/TGA into request-owned memory. Up to two workers convert independent textures into tightly packed CPU mip buffers. The render owner creates and fills D3D textures after completion.

This is the only approach that gives genuine parallel CPU preparation while enforcing the renderer ownership boundary. It temporarily holds source and prepared copies at the same time, so bounded admission and a small queue are required.

### Rejected: only replace `ThreadClass` with `TaskRuntime`

This would modernize lifecycle management but preserve worker access to live textures and D3D lock pointers. It does not establish the safety boundary required before wider multicore work.

### Rejected: parallelize individual mip levels

Splitting one texture into child tasks adds fences, increases scheduling overhead, and risks worker starvation in a bounded pool. Map loads already provide many independent textures, which are the natural unit of parallelism.

## Ownership model

### Render-owner-only state

Only the render owner may:

- read or change `TextureBaseClass` and its reference count;
- query `DX8Wrapper` capabilities;
- choose the final hardware-supported format and dimensions;
- create, lock, upload, unlock, release, or apply D3D textures;
- interact with `_TheFileFactory`, `ThumbnailManagerClass`, or other engine globals;
- create, recycle, or destroy `TextureLoadTaskClass` instances.

### Worker-owned state

A submitted preparation task may access only:

- an already loaded request-owned `DDSFileClass` or `Targa` image;
- copied scalar metadata: format, dimensions, mip count, reduction, HSV shift, and texture kind;
- request-owned CPU output buffers;
- the synchronized completion queue used to return the opaque owner task pointer.

The worker may not dereference `TextureBaseClass`, call DX8/D3D, consult engine globals, create follow-up tasks, or wait for other work.

## Data flow

1. The render owner creates or reuses a `TextureLoadTaskClass` and copies all immutable fields needed by preparation.
2. `Begin_Load()` selects compressed DDS or uncompressed TGA using the existing rules. On the owner thread it loads the complete source into an owned object, computes final metadata, and allocates tightly packed CPU output surfaces.
3. A low-priority request is wrapped in one `rts::Task` and submitted to a private `TaskRuntime` with `min(2, logicalProcessorCount)` workers and a fixed capacity of eight queued tasks.
4. The worker runs the existing DDS conversion or TGA conversion/mipmap algorithms against CPU output buffers. It records success and pushes the owning texture task back to the synchronized foreground queue.
5. The render owner consumes the completion, creates the appropriate supported 2D or cube D3D texture, locks each destination surface, copies rows from the CPU buffers, unlocks, applies the result, and releases all staged memory.
6. A foreground request takes its wrapper back when it is still queued and finishes preparation synchronously; if its worker is already active, it waits only for that texture. Upload remains entirely on the owner thread.

The final pixel bytes are produced by the same `DDSFileClass` and `BitmapHandlerClass` conversion functions as before. Only their destination changes from a D3D lock pointer to an owned CPU buffer.

## Surface layout

A D3D-free helper calculates and validates mip layouts:

- uncompressed row pitch is `width * Get_Bytes_Per_Pixel(format)`;
- DXT1 row pitch is `max(1, ceil(width / 4)) * 8`;
- DXT2 through DXT5 row pitch is `max(1, ceil(width / 4)) * 16`;
- compressed row count is `max(1, ceil(height / 4))`;
- uncompressed row count is `height`;
- a volume slice pitch is `rowPitch * rowCount`.

All multiplication is overflow-checked before allocation. Upload copies only meaningful row bytes into the driver-provided pitch, preserving padding and preventing overlap.

## Coverage by texture kind

- Regular 2D DDS and TGA textures use staged CPU mip buffers.
- DDS cube textures use six independent face/mip buffers.
- The layout/copy helper supports explicit volume row and slice pitches, but the shipped DDS backends return no volume-level source pointer. Stage 4 therefore rejects DDS volume preparation through the existing missing-texture path rather than uploading undefined bytes.
- Unsupported or malformed sources retain the existing missing-texture behavior.
- Existing uncompressed cube/volume behavior is not expanded.

## Backpressure and failure behavior

The runtime starts during `TextureLoader::Init()`. It tries two workers, then one worker. If startup fails, the loader remains functional through synchronous owner-thread preparation.

Low-priority submission is non-blocking. If the bounded queue rejects a task or its wrapper cannot be allocated, the owner immediately completes that already-staged request synchronously. Rejection never produces a missing texture and never transfers ambiguous ownership.

Allocation, source-load, conversion, or upload failure releases every owned source and CPU buffer exactly once. The texture receives the same missing-texture fallback used by the existing loader when preparation cannot begin.

## Lifecycle

`TextureLoader::Deinit()` stops accepting new texture work, drains and joins the task runtime, then consumes all completed foreground tasks while DX8 and texture references are still valid. Only after those tasks are uploaded or discarded on the owner thread are free pools and thumbnail state destroyed.

No task is detached, force-terminated, or allowed to outlive `_TheFileFactory`, DX8, or the texture free pools.

## Profiling

Workers emit `Texture.Prepare` zones around DDS/TGA conversion and mip generation. The owner emits `Texture.Upload` zones around D3D creation and transfer. A profile capture should show two preparation workers during a cold map load and all uploads on the render owner.

## Automated validation

Add a D3D-free Core extra test for the mip-layout and upload-copy helper. It covers:

- 8/16/24/32-bit uncompressed formats;
- DXT1 and DXT5 block pitches;
- odd and sub-block dimensions;
- padded destination pitches and volume slice pitches;
- overflow and invalid-format rejection;
- byte-identical serial and two-worker preparation copies;
- exact source/destination guard bytes around every copy.

Run tests and builds sequentially:

1. focused Stage 1, screenshot, and texture-preparation CTests;
2. modern Win32 Debug builds for both games and Core extras;
3. optimized/profile Win32 game builds;
4. VC6 Release, Debug, Profile, and release-log configurations that compile shared WW3D2 code;
5. the isolated optimized VC6 Zero Hour replay corpus from `TESTING.md` with captured exit code and log;
6. source audits proving worker code contains no `Texture->`, DX8/D3D, `_TheFileFactory`, thumbnail, or wait calls;
7. `git diff --check`, final diff review, and clean status.

## Manual acceptance

Deploy the profile build to a backed-up playable copy under `H:\Games`. In Zero Hour and then Generals:

- cold-start the shell and enter a skirmish;
- pan across the map and create units from several factions;
- verify no missing, black, flashing, or corrupted textures;
- return to the shell and load a different map repeatedly;
- alt-tab/minimize/restore during map load;
- save/load and finish or leave a skirmish;
- exit immediately after entering a map;
- capture a Tracy session confirming concurrent `Texture.Prepare` zones and owner-only `Texture.Upload` zones.

Stage 4 is accepted only after automated validation and this game test. Stage 5 design begins afterward.
