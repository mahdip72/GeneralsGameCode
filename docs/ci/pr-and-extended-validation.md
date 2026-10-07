# Pull request and extended validation

`PR CI` runs for every pull request and push to `main`. Require its stable
`PR readiness` check when configuring branch protection. The aggregate requires
change detection and workflow policy validation to succeed, then requires every
applicable lane to succeed. A skipped lane is accepted only when its path filter
says it is irrelevant. Failed, cancelled, unknown and unexpectedly skipped lanes
fail readiness. Superseded pull request runs are cancelled.

Changes under `Generals/` select Generals; changes under `GeneralsMD/` select
Zero Hour. Shared source, dependency, CMake, triplet and workflow changes select
both independent native x64 Release builds. Each build compiles the actual game
executable and only the listed smoke executables. These product presets inherit
normal Release options, with debug, profile, Tracy and ASAN disabled by default.
They retain the production FFmpeg and native runtime dependency closure. The
routine workflow does not install or publish a playable package.

The anchored CTest allowlist contains seventeen tests per selected title:

| Contract | Tests | Build targets |
| --- | --- | --- |
| Threaded renderer | `core_threaded_render_device_tests`, `core_threaded_render_device_native_tests`, `core_threaded_render_pipeline_stall_trace_tests` | First two names |
| Native renderer instancing admission | `core_native_w3d_renderer_tests` | Same name |
| Rigid instancing policy | `core_rigid_instancing_policy_tests` | Same name |
| Game rigid draw batching | `core_game_rigid_draw_batch_tests` | Same name |
| Mesh task failure and recovery | `core_native_mesh_task_lifetime_tests` | Same name |
| Renderer ownership and state | `core_native_w3d_owner_queue_tests`, `core_native_w3d_render_state_tests` | Same names |
| Task runtime | `core_task_runtime_tests` | Same name |
| Parser boundaries | `core_chunkio_value_read_tests`, `core_replay_field_reader_tests` | Same names |
| Deterministic CRC adapter | `core_deterministic_crc_runtime_adapter_tests` | `core_crc_runtime_tests` |
| Title runtime and determinism | `{g,z}_skirmish_ai_runner_contract_tests`, `{g,z}_xfer_crc_snapshot_tests`, `{g,z}_texture_load_queue_contract_tests`, `{g,z}_skirmish_ai_replay_epoch_tests` | `g_skirmish_ai_runner_contract_tests` or `z_runtime_regression_tests` |

The hardware instancing-parity case `d3d11_instancing_parity` remains a local
Tier 2 opt-in through `RTS_BUILD_D3D11_HARDWARE_TESTS`; it is excluded from PR CI.
The pipeline trace case shares its executable with the threaded renderer test.
Title cases share their title runtime harness. The game targets are `g_generals`
and `z_generals`. Native renderer fixtures can use D3D11 WARP on hosted Windows
runners. These fixtures check contracts; they do not establish visual parity,
installed gameplay, replay corpus compatibility or performance improvement.

Changes to `tools/rendering-benchmark/` or its CI policy run the portable Python
unittest suite separately. Workflow policy validation runs for every change,
including documentation-only pull requests, and checks the actual workflow
structure, targets, exact CTest selection and readiness implementation.

`GenCI` remains available through `workflow_dispatch` for broad validation.
Its authoring and legacy matrix, diagnostic presets, broad extras, Stage 5
Acceptance and native-path suites, installed runtime/replay checks and release
qualification jobs remain intact. External replay, lockstep and physical-core
qualification still require their existing opt-ins and prerequisites. Dispatch
this workflow for milestones requiring those gates, after changes affecting
those contracts, or when focused evidence identifies a wider risk. An unavailable
runner or data service is an unmet qualification prerequisite, never a passing
test. Concrete compiler or test failures must be investigated in their matching
lane.

The routine smoke workflow selects exact tests rather than broad prefixes or
negative exclusions. Add a test and its matching target deliberately when the
smoke contract needs to expand. Do not remove useful tests from the repository
to shorten pull request validation. The broad workflow remains the place for
long simulation, cache, security, path and qualification suites.
