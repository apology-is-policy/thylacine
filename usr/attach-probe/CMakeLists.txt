# /attach-probe — userspace integration test exercising
# SYS_ATTACH_9P + SYS_MOUNT + SYS_UNMOUNT (P5-attach-probe).
#
# Pattern matches /pipe-probe: static-PIE ELF for aarch64-none-elf,
# linked against libt for _start + canary stubs + syscall stubs.

add_executable(attach-probe attach-probe.c)
target_link_libraries(attach-probe PRIVATE t)

set_target_properties(attach-probe PROPERTIES
    SUFFIX ""
    OUTPUT_NAME attach-probe
)
