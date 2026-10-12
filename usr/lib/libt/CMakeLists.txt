# libt — Thylacine userspace runtime.
#
# Static archive containing the minimum scaffolding every Thylacine native
# userspace binary needs: _start (program entry), stack-canary stubs. SVC
# wrappers are header-only (thyla/syscall.h). The native C Loom client cores
# (thyla/loom_client/) live here too; they allocate nothing, so a program that
# never calls them links none of their objects.
#
# Per docs/reference/38-userspace.md: libt is the C-side runtime; Rust
# binaries (P4-Ia2+) bring their own minimal runtime crate and don't link
# against libt.

add_library(t STATIC
    src/start.S
    src/loom_client/fatal.c
    src/loom_client/fixed_requests.c
    src/loom_client/lease_ledger.c
    src/loom_client/owned_mapping.c
    src/loom_client/paired_cq.c
    src/loom_client/request_book.c
    src/loom_client/retirement_executor.c
    src/loom_client/ring_payload.c
)

target_include_directories(t
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Output: libt.a alongside the build tree, easy to inspect.
set_target_properties(t PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib
)
