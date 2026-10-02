# Native material override regression

`g_native_material_override_tests` and `z_native_material_override_tests` are
candidate-only CPU tests, registered only in the native x64 extras lane.
They need no baseline Git object, title library, installed game or GPU.

The generator extracts the entire production category `Render` method, each
title's material facade and helpers, its distinct mapper methods, material
accessors, renderer enum declarations, and the native material command case.
Method extraction must be unique and balanced; ambiguous/truncated source fails
the build. Generated files and the 24-fragment SHA256 receipt live exclusively
in the target's binary directory; all source dependencies are explicit.
The complete actual `LegacyRenderState.cpp` supplies state tracking/copies.

Mocks supply math/container/title-object scaffolding, a producer owner-pin model
and device-less draw capture. The override and mapper algorithms are production
code, not handwritten test substitutes. Captured logical state is copied by
value, representing immediate and deferred consumers. This does not validate
real locking/owner lifecycle, title ABI, native sorting, GPU behavior or visual
parity; those remain integrated product qualification.

Tests cover alpha/additive/UV-only/combined overrides, mapper sync and transform
state, restoration for following ordinary/sorted meshes, later mutation
durability, nonlinear mapper custom-UV refusal, facade refusal and pending/disabled task
handling. The pre-existing sorted-first branch is intentionally unchanged:
this repair does not add per-instance override semantics to sorted meshes.
