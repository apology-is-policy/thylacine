# /stratumd-stub — minimal userspace 9P responder; P5-stratumd-stub demo.
#
# Static-PIE ELF for aarch64-none-elf. Linked against libt for _start +
# t_read/t_write/t_putstr/t_exits. The W^X linker script enforces no
# RWX segments.
#
# Output: build/usr/stratumd-stub/stratumd-stub — the ELF that
# tools/build.sh kernel (build_ramfs) copies into the cpio so devramfs
# exposes it as /stratumd-stub. kernel/test/test_stratumd_stub.c loads
# it via devramfs_lookup and rfork-exec's it with two pipe ends
# pre-installed in the child's handle table.

add_executable(stratumd-stub stratumd-stub.c)
target_link_libraries(stratumd-stub PRIVATE t)

set_target_properties(stratumd-stub PROPERTIES
    SUFFIX ""
    OUTPUT_NAME stratumd-stub
)
