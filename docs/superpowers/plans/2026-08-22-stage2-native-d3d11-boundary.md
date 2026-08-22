# Stage 2 Native D3D11 Product Boundary Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the x64 D3D11 product graph independent of D3D8 device creation, D3D8 headers, and D3D8 import libraries while retaining the existing Win32 D3D8 differential lane.

**Architecture:** Keep `core_renderer` as the one owner of D3D11 objects and logical pipeline state. Split its current D3D8-fed compatibility adapter from a native facade: the adapter remains in x86 `DX8Wrapper`, while a new facade consumes `LegacyLogicalState`, typed descriptors, and `GpuHandle` without exposing D3D11 COM pointers to gameplay code.

**Tech Stack:** C++20, MSVC x64, Windows 10/11, D3D11/DXGI, existing `core_renderer`, CMake/Ninja, focused CTests.

**Spec:** User-approved Five-Stage GeneralsGameCode x64, D3D11, and Full-Multicore Conversion plan, Stage 2; `Core/Libraries/Include/Renderer/RendererDevice.h`; `Core/Libraries/Source/Renderer/LegacyRenderState.cpp`.

## Global Constraints

- Preserve the x86 D3D8/VC6 lane as the visual and behavioral differential oracle until final cutover.
- Native game code must not retain raw D3D11 COM pointers; use renderer handles/descriptors only.
- D3D11 immediate-context calls remain render-owner-only.
- Do not run an interactive game, install, promote, or manually test during this implementation slice.
- Do not add a fake D3D8 SDK header or link a D3D8 compatibility DLL into x64.
- Keep WorldBuilder and W3DView outside the x64 runtime product graph.
- Each commit uses the repository `type(scope): Uppercase description` convention and is independently buildable.

---

### Task 1: Establish the core-backend boundary without overclaiming product readiness

**Files:**
- Create: `Core/Tools/NativeRendererGraphTest/CMakeLists.txt`
- Create: `Core/Tools/NativeRendererGraphTest/NativeRendererGraphTest.cpp`
- Modify: `Core/Tools/CMakeLists.txt`

**Interfaces:**
- Consumes: `CMAKE_SIZEOF_VOID_P`, `core_renderer`.
- Produces: a CTest proving the x64 core renderer exposes the D3D11 factory. This is deliberately **not** a product-native claim.

- [ ] **Step 1: Write the failing graph test**

```cpp
// NativeRendererGraphTest.cpp
int main()
{
#if !defined(RTS_RENDERER_HAS_D3D11)
    return 1;
#endif
    IRenderDevice *device = CreateD3D11RenderDevice();
    return device == 0;
}
```

- [ ] **Step 2: Run the focused x64 test and verify it fails**

Run: `cmake --build <x64-core-build> --target core_native_renderer_graph_tests && ctest --test-dir <x64-core-build> -R ^core_native_renderer_graph_tests$ --output-on-failure`

Expected: failure because the D3D11 capability is not yet propagated through the tested core target.

- [ ] **Step 3: Link the test to the actual D3D11-capable core target**

```cmake
target_link_libraries(core_native_renderer_graph_tests PRIVATE core_renderer)
```

The test must consume `RTS_RENDERER_HAS_D3D11` as a public property of the real
renderer target; it must not inject a manually supplied "native product" macro.
`RTS_RENDERER_D3D11_NATIVE` remains reserved for Task 5, after the x64 product
graph has no D3D8/Miles/Bink dependency.

- [ ] **Step 4: Run the graph test and configure both architectures**

Run: `cmake --build <x64-core-build> --target core_native_renderer_graph_tests && ctest --test-dir <x64-core-build> -R ^core_native_renderer_graph_tests$ --output-on-failure`

Expected: PASS. Also configure the existing Win32 build and verify its D3D8 product targets remain generated. Record that the x64 game product is still intentionally unavailable: it retains source-level D3D8, Miles, and Bink dependencies until Tasks 2-5 and Stage 3 service replacement work are complete.

- [ ] **Step 5: Commit**

```text
test(renderer): Verify x64 D3D11 core availability
```

### Task 2: Introduce the native WW3D facade contract

**Files:**
- Create: `Core/Libraries/Include/Renderer/NativeW3DRenderer.h`
- Create: `Core/Libraries/Source/Renderer/NativeW3DRenderer.cpp`
- Modify: `Core/Libraries/Source/Renderer/CMakeLists.txt`
- Create: `Core/Tools/NativeW3DRendererTest/CMakeLists.txt`
- Create: `Core/Tools/NativeW3DRendererTest/NativeW3DRendererTest.cpp`
- Modify: `Core/Tools/CMakeLists.txt`

**Interfaces:**
- Consumes: `rts::render::IRenderDevice`, `LegacyLogicalState`, `RenderPrimitiveTopology`, `GpuHandle`.
- Produces: `rts::render::NativeW3DRenderer::Initialize(HWND, const RenderDeviceDescriptor&)`, `BeginFrame()`, `Submit(const LegacyLogicalState&, const NativeDrawPacket&)`, and `EndFrame(bool)`.

- [ ] **Step 1: Write the failing facade lifecycle test**

```cpp
rts::render::NativeW3DRenderer renderer;
Check(!renderer.BeginFrame(), "cannot begin before initialize");
Check(renderer.Initialize(nullptr, descriptor) == rts::render::RENDER_RESULT_INVALID_ARGUMENT,
      "invalid window is rejected without creating a D3D8 device");
```

- [ ] **Step 2: Run it and verify the missing type/test failure**

Run: `cmake --build <x64-core-build> --target core_native_w3d_renderer_tests`

Expected: compile failure because `NativeW3DRenderer` does not exist.

- [ ] **Step 3: Implement the facade with no D3D8 includes**

```cpp
class NativeW3DRenderer final {
public:
    RenderResult Initialize(HWND window, const RenderDeviceDescriptor &descriptor);
    RenderResult BeginFrame();
    RenderResult Submit(const LegacyLogicalState &state, const NativeDrawPacket &packet);
    RenderResult EndFrame(bool present);
};
```

The implementation delegates only to `IRenderDevice`; it does not include `dx8wrapper.h`, `d3d8.h`, or `d3d8types.h`.

- [ ] **Step 4: Run x64 lifecycle and source-boundary tests**

Run: `ctest --test-dir <x64-core-build> -R "^(core_native_w3d_renderer_tests|core_renderer_bypass_audit)$" --output-on-failure`

Expected: PASS, with the new native files absent from D3D8 audit findings.

- [ ] **Step 5: Commit**

```text
feat(renderer): Add native W3D submission facade
```

### Task 3: Move native resource ownership behind handles

**Files:**
- Modify: `Core/Libraries/Include/Renderer/RendererDevice.h`
- Modify: `Core/Libraries/Source/Renderer/D3D11RenderDevice.cpp`
- Create: `Core/Libraries/Include/Renderer/NativeW3DResources.h`
- Create: `Core/Libraries/Source/Renderer/NativeW3DResources.cpp`
- Create: `Core/Tools/NativeW3DResourcesTest/CMakeLists.txt`
- Create: `Core/Tools/NativeW3DResourcesTest/NativeW3DResourcesTest.cpp`
- Modify: `Core/Tools/CMakeLists.txt`

**Interfaces:**
- Consumes: `GpuHandle`, existing texture/buffer descriptors, `IRenderDevice` creation methods.
- Produces: generation-safe native texture, vertex-buffer, index-buffer, and render-target resource records.

- [ ] **Step 1: Write the failing stale-handle test**

```cpp
const GpuHandle first = resources.CreateTexture(desc, bytes);
resources.Destroy(first);
const GpuHandle second = resources.CreateTexture(desc, bytes);
Check(first != second, "resource generation changes after destruction");
Check(!resources.IsValid(first), "stale handle is rejected");
```

- [ ] **Step 2: Run it and verify it fails**

Run: `cmake --build <x64-core-build> --target core_native_w3d_resources_tests`

Expected: compile failure because `NativeW3DResources` does not exist.

- [ ] **Step 3: Implement bounded handle tables**

Use index-plus-generation `GpuHandle` values. Decode/upload work may run on jobs, but `IRenderDevice::Create*` and destruction/publication run on the render owner only. Never store an `ID3D11*` in a game object.

- [ ] **Step 4: Run resource tests**

Run: `ctest --test-dir <x64-core-build> -R ^core_native_w3d_resources_tests$ --output-on-failure`

Expected: PASS for creation, stale generation rejection, double destroy, and shutdown cancellation.

- [ ] **Step 5: Commit**

```text
feat(renderer): Add native W3D resource handles
```

### Task 4: Split D3D8 adapter sources from native sources

**Files:**
- Modify: `Core/Libraries/Source/WWVegas/WW3D2/CMakeLists.txt`
- Modify: `Generals/Code/Libraries/Source/WWVegas/WW3D2/CMakeLists.txt`
- Modify: `GeneralsMD/Code/Libraries/Source/WWVegas/WW3D2/CMakeLists.txt`
- Create: `Core/Libraries/Source/WWVegas/WW3D2/nativew3d2.cpp`
- Create: `Core/Libraries/Source/WWVegas/WW3D2/nativew3d2.h`
- Modify: `Core/Tools/RendererBypassAudit/RendererBypassAudit.ps1`

**Interfaces:**
- Consumes: Tasks 1-3 native graph/facade/resources.
- Produces: `corei_ww3d2_native`, `g_ww3d2_native`, and `z_ww3d2_native`, with a PCH that excludes `dx8wrapper.h`.

- [ ] **Step 1: Write a failing source-boundary audit case**

```powershell
$nativeFiles = @('nativew3d2.cpp', 'nativew3d2.h')
Assert-NoMatch $nativeFiles 'd3d8\.h|dx8wrapper\.h|IDirect3D.*8'
```

- [ ] **Step 2: Run the audit and verify it fails because the native target is absent**

Run: `ctest --test-dir <x64-core-build> -R ^core_renderer_bypass_audit$ --output-on-failure`

Expected: FAIL with the missing native source set.

- [ ] **Step 3: Define non-overlapping source lists**

```cmake
set(WW3D2_DX8_SRC dx8wrapper.cpp dx8renderer.cpp dx8caps.cpp)
set(WW3D2_NATIVE_SRC nativew3d2.cpp nativew3d2.h)
if(RTS_RENDERER_D3D11_NATIVE)
    target_sources(corei_ww3d2_native INTERFACE ${WW3D2_NATIVE_SRC})
else()
    target_sources(corei_ww3d2 INTERFACE ${WW3D2_DX8_SRC})
endif()
```

Migrate a file into the native list only after its resource/device dependencies use Tasks 2-3 contracts; do not dual-compile a source with two incompatible ownership models.

- [ ] **Step 4: Build the native WW3D target**

Run: `cmake --build <x64-product-probe> --target z_ww3d2_native -j 4`

Expected: PASS without a D3D8 include directory or `rts_d3d8lib` link item.

- [ ] **Step 5: Commit**

```text
refactor(renderer): Split native W3D target sources
```

### Task 5: Wire a D3D11-native x64 product probe

**Files:**
- Modify: `GeneralsMD/Code/GameEngine/CMakeLists.txt`
- Modify: `Generals/Code/GameEngine/CMakeLists.txt`
- Modify: `Core/GameEngineDevice/CMakeLists.txt`
- Modify: `GeneralsMD/Code/Libraries/Source/WWVegas/CMakeLists.txt`
- Modify: `Generals/Code/Libraries/Source/WWVegas/CMakeLists.txt`
- Create: `Core/Tools/NativeProductDependencyAudit/NativeProductDependencyAudit.ps1`
- Modify: `Core/Tools/CMakeLists.txt`

**Interfaces:**
- Consumes: `z_ww3d2_native`, `g_ww3d2_native`, and native renderer contracts.
- Produces: product targets whose x64 link graph contains D3D11/DXGI but no D3D8/Miles/Bink nodes.

- [ ] **Step 1: Write the failing product-link audit**

```powershell
$forbidden = 'rts_d3d8lib', 'd3d8', 'milesstub', 'binkstub'
Assert-LinkGraphDoesNotContain -Target z_gameengine -Names $forbidden
```

- [ ] **Step 2: Run it against the current x64 product probe**

Run: `ctest --test-dir <x64-product-probe> -R ^core_native_product_dependency_audit$ --output-on-failure`

Expected: FAIL because `z_ww3d2` still includes `dx8wrapper.h` and its link graph requires D3D8.

- [ ] **Step 3: Select native targets only for x64**

```cmake
if(RTS_RENDERER_D3D11_NATIVE)
    target_link_libraries(z_gameengine PRIVATE z_ww3d2_native)
else()
    target_link_libraries(z_gameengine PRIVATE z_ww3d2)
endif()
```

Apply the equivalent selection for Generals and GameEngineDevice. Do not unlink Miles/Bink until their native XAudio2/FFmpeg replacements are wired; instead keep these services outside the first renderer-only x64 probe and make the omission explicit in the product configuration.

- [ ] **Step 4: Build the native x64 product probe**

Run: `cmake --build <x64-product-probe> --target z_gameengine -j 4`

Expected: reaches game-engine compilation without `d3d8.h` or `d3d8types.h` include failures. Record the next blocker rather than adding a compatibility shim.

- [ ] **Step 5: Commit**

```text
build(renderer): Route x64 product through native D3D11
```

## Self-Review

- Stage 2 renderer contract coverage: Tasks 1-5 split the D3D8 adapter and native D3D11 facade, resource ownership, source graph, and product selection.
- Visual parity is intentionally preserved by retaining the x86 adapter and shared `LegacyLogicalState`; no task claims visual acceptance without the later installed-runtime visual gate.
- x64 product blockers outside rendering are retained as explicit follow-on work: XAudio2/FFmpeg service replacements and persistence epoch integration belong to Stage 3.
- Placeholder scan: no task contains an unbounded implementation instruction; each has specific files, interfaces, a failing check, a validation command, and a commit subject.
- Interface consistency: Tasks 2-5 use `NativeW3DRenderer`, `NativeW3DResources`, `corei_ww3d2_native`, `g_ww3d2_native`, and `z_ww3d2_native` consistently.
