# /thread-fault-probe -- HOLOTYPE RW-1 C-F1 regression: an EL0 fault in a
# multi-thread Proc terminates the Proc (not the kernel). Static-PIE ELF
# for aarch64-none-elf, linked against libt; mirrors /thread-probe.

add_executable(thread-fault-probe thread-fault-probe.c)
target_link_libraries(thread-fault-probe PRIVATE t)

set_target_properties(thread-fault-probe PROPERTIES
    SUFFIX ""
    OUTPUT_NAME thread-fault-probe
)
