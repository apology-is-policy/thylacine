# /hello — first userspace binary.
#
# Static-PIE ELF for aarch64-none-elf, linked against libt for _start +
# canary stubs. The linker script (usr/scripts/aarch64-userspace.ld)
# enforces W^X by emitting two PT_LOAD segments: text+rodata RX, data+bss
# RW. The kernel's ELF loader (kernel/elf.c) rejects any RWX segment.
#
# Output: build/usr/hello/hello — the ELF that tools/build.sh kernel
# (build_ramfs) copies into the cpio so /hello is visible to devramfs.

add_executable(hello hello.c)
target_link_libraries(hello PRIVATE t)

# Strip output name so the cpio entry is exactly "hello" (not "hello.elf").
set_target_properties(hello PROPERTIES
    SUFFIX ""
    OUTPUT_NAME hello
)
