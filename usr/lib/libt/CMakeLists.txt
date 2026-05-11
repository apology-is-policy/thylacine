# libt — Thylacine userspace runtime.
#
# Static archive containing the minimum scaffolding every Thylacine native
# userspace binary needs: _start (program entry), stack-canary stubs. SVC
# wrappers are header-only (thyla/syscall.h).
#
# Per docs/reference/38-userspace.md: libt is the C-side runtime; Rust
# binaries (P4-Ia2+) bring their own minimal runtime crate and don't link
# against libt.

add_library(t STATIC
    src/start.S
)

target_include_directories(t
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Output: libt.a alongside the build tree, easy to inspect.
set_target_properties(t PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib
)
