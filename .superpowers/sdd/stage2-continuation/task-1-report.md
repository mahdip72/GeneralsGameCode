# Task 1A Report — Correct invalid legacy blend translation

## Root cause

The legacy state decoder intentionally copies the decoded color blend factors
into the alpha factors. `D3D11RenderDevice` previously translated both fields
with the RGB `TranslateBlend` table, so color-only factors such as
`D3D11_BLEND_SRC_COLOR` were submitted in `D3D11_RENDER_TARGET_BLEND_DESC`
alpha slots. `CreateBlendState` rejects those descriptors. The fix keeps the
legacy shader decode unchanged and adds an alpha-specific translator that maps
legacy color factors to the corresponding alpha factors.

The temporary diagnostic result remaps were removed by restoring
`TranslateResult(...)` propagation for transform-map, blend, depth, rasterizer,
input-layout, and second constant-map failures. The bridge failure log now
clamps its diagnostic loop to the layout capacity and only reads elements that
are present.

## Red test

Command:

```text
ctest --test-dir H:\CodexBuilds\GeneralsGameCode\fm-s2-d3d11-w32d -C Debug --output-on-failure -R "^core_renderer_contract_tests$"
```

Result: exit code 1. Before the production fix, the focused real-device test
reported three `CreateBlendState` rejections:

```text
legacy blend case SOURCE_COLOR with ZERO returned RenderResult 5; CreateBlendState rejected the alpha factor
FAIL: legacy blend state remains bindable: SOURCE_COLOR with ZERO
legacy blend case ZERO with SOURCE_COLOR returned RenderResult 5; CreateBlendState rejected the alpha factor
FAIL: legacy blend state remains bindable: ZERO with SOURCE_COLOR
legacy blend case ONE with INVERSE_SOURCE_COLOR returned RenderResult 5; CreateBlendState rejected the alpha factor
FAIL: legacy blend state remains bindable: ONE with INVERSE_SOURCE_COLOR
0% tests passed, 1 tests failed out of 1
```

The color-write-disabled `ZERO/ONE` descriptor remained valid in the red run.

## Files changed

- `Core/Libraries/Source/Renderer/D3D11RenderDevice.cpp`: added the
  alpha-channel blend translator and used it only for alpha descriptor fields.
- `Core/Libraries/Source/WWVegas/WW3D2/d3d11legacybridge.cpp`: retained the
  truthful state result and made layout diagnostics bounded by element count
  and array capacity.
- `Core/Tools/RendererContractTest/RendererContractTest.cpp`: added the
  focused real-D3D11 blend-state contract covering all required factor pairs.

## Green verification

Build command:

```text
cmake --build H:\CodexBuilds\GeneralsGameCode\fm-s2-d3d11-w32d --config Debug --target core_renderer_contract_tests -j 4
```

The target built successfully. The focused test then passed:

```text
1/1 Test #12: core_renderer_contract_tests .....   Passed    1.31 sec
100% tests passed, 0 tests failed out of 1
```

Additional compatibility checks passed:

- Win32 Debug `z_ww3d2` build, including `d3d11legacybridge.cpp`.
- VC6 Release `z_ww3d2` build, including the legacy bridge translation unit.
- `git diff --check`.

## Commit SHA

`e86c99627`

## Self-review

- RGB translation remains unchanged.
- Alpha mappings cover `SOURCE_COLOR`, `INVERSE_SOURCE_COLOR`,
  `DESTINATION_COLOR`, and `INVERSE_DESTINATION_COLOR`; all other factors use
  their existing equivalents.
- Legacy shader enum decoding and replay-facing state semantics were not
  reinterpreted.
- All temporary arbitrary `RenderResult` remaps are absent.
- Bridge logging never indexes beyond the reported element count or the fixed
  layout array capacity.
- No game process, installed runtime, registry, push, PR, merge, or unrelated
  worktree was touched.

## Concerns

The installed-runtime visual shell run was intentionally not performed because
the task explicitly forbids running the game; the next visual-debugging task
should consume this green renderer contract result.
