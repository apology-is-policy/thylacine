# /thread-probe — end-to-end test of SYS_THREAD_SPAWN + SYS_THREAD_EXIT
# + the clear-child-tid join handshake (P6-pouch-threads sub-chunk 9a).
#
# Pattern matches /attach-probe: static-PIE ELF for aarch64-none-elf,
# linked against libt for _start + canary stubs + syscall stubs.

add_executable(thread-probe thread-probe.c)
target_link_libraries(thread-probe PRIVATE t)

set_target_properties(thread-probe PROPERTIES
    SUFFIX ""
    OUTPUT_NAME thread-probe
)
