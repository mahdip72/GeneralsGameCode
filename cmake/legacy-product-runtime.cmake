# Stage 5 retirement tombstone.  Keep this module fail-closed so stale external
# scripts cannot silently resurrect the removed Win32 product dependency graph.
include_guard(GLOBAL)

message(FATAL_ERROR
    "Legacy Win32 product runtime has been retired. Native x64 is the only "
    "supported product architecture; historical tools must use a non-product preset.")
