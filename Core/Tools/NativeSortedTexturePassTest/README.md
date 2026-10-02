# Native sorted texture pass control regression

CPU-only native64 regression. The actual `GameTextureRenderPass` header is
included directly. `ExtractMethods.ps1` extracts the complete unchanged shared
water `updateRenderTargetTextures` and `renderMirror` methods, and the exact
refresh statement and visible Begin condition from each title, the exact
pre-sampling drawSea admission prefix and the actual bump-frame bound. Eight fragments
have path/line/SHA receipts. Missing, ambiguous or truncated extraction fails;
the literal/comment scanner self-check runs before generation. CMake names the
production source dependencies explicitly and requires no baseline Git object.

The camera/cull/scene, resource pointers and semantic render facade are mocks.
Reflection math uses actual WWMath headers. The test exercises restoration,
clear/render/exception/flush/restore/completion failures, sticky/idempotent Finish,
both title admission conditions and next full regeneration. It does not execute
the full display loop, water shader, actual device, resource publication, pixels
or replay. Its completion outcome is deliberately a control mock, **not** proof
of actual owner completion.

The drawSea prefix tests invisible/resource-less no-ops after a false ctor/reacquire
readiness, visible-not-ready frame failure, and healthy-visible admission. The
fixture's post-prefix marker is NOT a full sea draw or texture sampling/pixel
assertion; resource/visibility carriers are mocks, admission condition text is exact.

That separate boundary is covered by `core_native_sorted_pass_ownership_cpu`:
actual NativeW3D2/resource/sorter/ThreadedRenderDevice linked against a mock CPU
backend in direct, serial and parallel modes. Texture ticket release uses the
actual owner/resource implementation in `core_native_deferred_texture_retention_cpu`.
The existing actual sorter tests retain the qualified geometry/order assertions
and add pass segmentation/retry and linear workspace checks. No GPU is used by
these four selected CPU tests; existing GPU test registrations are unchanged.
