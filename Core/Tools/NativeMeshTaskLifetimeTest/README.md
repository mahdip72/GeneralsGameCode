# Native mesh task lifetime CPU contract

Target and CTest: `core_native_mesh_task_lifetime_tests`. No GPU or game assets.

The build mechanically extracts the actual shared `PolyRenderTaskClass` class and
`DX8TextureCategoryClass::{Add_Render_Task,Render,Clear_Render_List}` and
`DX8RigidFVFCategoryContainer::Render` from current `nativew3dmeshrenderer.cpp`.
Extraction uses balanced braces while ignoring comment/literal braces, rejects
missing/ambiguous/truncated methods, and emits body hashes/first-line receipts.
CMake depends on the production source and extractor; generated bodies are not
checked into source. The source class retains its real Add_Ref/Release_Ref calls.

The harness supplies explicit geometry/material/camera/renderer doubles and a
scripted sticky failed FlushGameRigidDraws. It executes the full production
traversal/deletion code. It checks real callback identity sequences and reference
balances, cancellation at entry/mid/final barriers, failure propagation across two
categories through actual rigid-container traversal, no stale visibility on a
recovered pass, and healthy prefix/interior/all-overflow deferral with order and
exact-once subsequent drain. Repeated empty cleanup is checked too.

The mock failure is an owner boundary result, not GPU failure injection. This test
does not qualify backend draw execution, pixel parity, performance or the general
native owner recovery state machine. Other native tests cover those contracts.

Root-owned RED qualification may extract from a frozen pre-fix source tree by
passing `-MeshSourceRoot` to ExtractMethods.ps1, preserving the current harness.
Normal CMake never sets that option or relies on historical Git refs. RED must
fail the reference/visibility/retained-list assertions; GREEN must pass against
current production source. This is an executable failure-path test, not a regex
assertion that certain cleanup statements appear in source.
