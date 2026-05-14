# /pipe-probe — first userspace binary exercising the byte I/O
# syscalls (SYS_PIPE + SYS_READ + SYS_WRITE + SYS_CLOSE + SYS_DUP)
# end-to-end. Per P5-fd-syscalls.
#
# Pattern matches /hello: static-PIE ELF for aarch64-none-elf,
# linked against libt for _start + canary stubs + the syscall
# inline-asm stubs.

add_executable(pipe-probe pipe-probe.c)
target_link_libraries(pipe-probe PRIVATE t)

set_target_properties(pipe-probe PROPERTIES
    SUFFIX ""
    OUTPUT_NAME pipe-probe
)
