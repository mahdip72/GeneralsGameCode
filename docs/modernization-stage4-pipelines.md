# Modernization Stage 4: owner threads and parallel preparation

This stage builds on the native x64/D3D11 product. It changes execution
ownership and CPU preparation, not the simulation tick, save/replay epoch,
gameplay random streams, or network command ordering. The older numbered
multicore experiments in `docs/superpowers` describe earlier work, not this
modernization stage.

## Ownership

- The game thread owns simulation, UI, pooled game/W3D objects, asset catalogs,
  model/prototype publication, and final prepared-data adoption.
- `ThreadedRenderDevice` owns D3D11 creation, calls, recovery, and destruction
  on a dedicated render thread. The legacy bridge records copied state,
  commands, uploads, and generation-checked logical resource handles. Packets
  never borrow live game objects or transient caller buffers.
- The shared `JobSystem` prepares immutable snapshots or disjoint private
  output ranges. Compute workers do not issue D3D11 calls or perform blocking
  asset reads.
- `ResourceIoPipeline` reads independent loose-file/BIG ranges on its I/O
  owner. Header inspection/admission and publication remain on the game
  thread; supported decoding runs through shared jobs.
- The process-wide XAudio2 service owner submits audio and services native
  voices. Category decisions, event ordering, random choices, and game-facing
  completion handling remain on the game thread.
- The network I/O owner handles bounded raw datagrams. Transport parsing,
  protocol validation, accounting, and lockstep publication remain on the
  game thread. Moving socket I/O does not authenticate peers or change the
  network protocol.

## Frame and resource lifetime

The renderer supports two or three bounded packets in flight. The producer
can prepare the next frame while the render owner consumes an earlier packet.
FIFO resource commands order creation, updates, draws, and destruction.
Generation checks reject stale handles; failed resource work cannot silently
publish a usable result. Completion records carry asynchronous failures back
to the producer.

Capture, resize, recovery, and shutdown have explicit fences. A CPU fence is
not proof of GPU completion; the backend owns the readback/recovery operations
that need it. Window-message servicing during producer waits does not permit
reentrant renderer mutation. Packet payload, command count, resource slots,
completion storage, and worker scratch all have independent bounds.

## Parallel preparation

The shared job system covers visibility, animation/skinning, particle and
water geometry, projected terrain shadow/decal grids, sorting-triangle
preparation, terrain/dynamic-light data, and radar/shroud output. Owners copy
the required inputs, preserve their floating-point state in jobs, fence all
accepted work, and adopt output in the original order. Small work, serial
policy, unavailable workers, and rejected work retain reference paths.

Texture streaming resolves sources on the owner and decodes supported DDS/TGA
data without pooled file/texture objects on workers. Unsupported factories or
formats retain their owner-side path. Model preloading shares that same I/O
pipeline in explicit preload windows: workers validate chunk envelopes and
copy bounded bytes, then the owner uses the existing W3D parser in FIFO order.
The W3D parser itself is not parallel. Native triangle decoding separately
produces packed index, plane, and surface arrays for three owner-side copies.
Both title index widths and serialized plane values are preserved.

Map changes cancel/drain old generations before archive catalogs or renderer
resources are torn down. Model output retained after an I/O ticket is retired
has a separate byte budget, so it cannot strand texture output reservations.
Allocation/admission faults must drain accepted jobs before a serial fallback
or return with no published output.

## Controls and evidence

- `-pipelineMode parallel|serial` selects preparation policy at startup.
  Serial policy retains the dedicated native service owners but fences render
  work and selects reference preparation paths.
- `-workerCount N` and `-workerPolicy auto|all` remain scheduler controls.
  Replay `-jobs` instead starts independent replay processes; it is not an
  in-process worker-count control.
- Existing `-runSkirmishAITest <seed>` remains the 4-vs-3 diagnostic.
  `-runSkirmishAITest4v2 <seed>` selects six AI players plus an observer and
  verifies the loaded setup before reporting completion.
- Focused tests cover byte parity, generation/lifetime boundaries, queue and
  allocation faults, cancellation, shutdown, native render capture/recovery,
  audio service ownership, and raw socket ownership. Keep the VC6 differential
  lane while it remains supported.
- Headless live games and exact-binary paired replays test simulation liveness
  and determinism. They do **not** exercise render/audio overlap or establish
  an interactive performance gain. Headless startup disables lazy compute
  startup before asset loading, including model and pose query fallbacks.
- Native hidden-window renderer tests exercise the actual D3D11 backend but
  are not full-game visual acceptance. The animated shell, water, ships,
  combat, lighting, audio, movies, camera, resize, and long-session behavior
  still require installed-runtime testing at matched settings.

Record the exact source and executable hashes with each gate. Do not infer
full-game scaling from synthetic kernel throughput, and do not treat a serial
reference pass as acceptance of a different parallel executable.
