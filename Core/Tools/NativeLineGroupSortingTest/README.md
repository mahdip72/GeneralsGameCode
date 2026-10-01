# Native line-group sorting regression

Native64 CPU-only target `core_native_linegroup_sorting_tests` executes the
mechanically extracted **entire actual MD LineGroupClass::Render**, original
vertex record/color packing, actual shared UP/sorted-UP facade and dispatch
helpers, actual sorted-UP owner handler, and linked **actual NativeSortingRenderer**.
The real logical-state tracker, FVF decoder, color packer and sorting kernel are
used. Source dependencies and generated outputs are explicit; extraction fails
on ambiguous/truncated methods/case boundaries. Candidate CI needs no baseline
Git object. Optional alternate line-group source root serves local frozen RED
qualification only; its receipt identifies both input roots.

Mocks supply title arrays, vectors/matrices/camera, shader/material/texture
objects, immediate draw capture, and owner pin/admission/failure carrier. They do
not rewrite line-group geometry, depth sorting, indexed queue ownership, chunk
assembly or accepted-prefix retry. Material/texture facade scaffolding uses the
real logical tracker but is not the title resource/loading ABI. The scoped owner
handler is production code inside a modeled carrier, **not** the complete
NativeW3D2 completion/recovery/device lifecycle. Title reference counters and
camera math are scaffolding; no GPU, complete title fidelity, FPS, or owner mutex
qualification is inferred from this test.

The source fixtures compare the immediate geometry's full triangle records with
the globally sorted reference stream across translucent mesh-like neighbors,
tetrahedron/prism, world/view-space input, attributes, captured material/texture/
light/fog/transforms, later mutation, restored view and both zero/positive accepted
prefix retries. A 65,544-index group exercises existing flush chunk retirement;
mixed vertex layouts exercise unchanged splitting. Null/size/count/R16/FVF/index,
modeled owner/operational and queue-failure refusals plus no-sort/empty cases remain
negative assertions. The legacy branch is unchanged. Real both-title product and
NativeW3D2 lifecycle/GPU/installed visual acceptance are separate root-owned gates.
