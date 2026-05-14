# /joey — the first userspace process.
#
# Static-PIE ELF for aarch64-none-elf; linked against libt for _start +
# canary stubs. The kernel ELF loader (kernel/elf.c) rejects RWX
# segments; the W^X linker script in usr/scripts/aarch64-userspace.ld
# emits two PT_LOAD segments (RX text+rodata, RW data+bss).
#
# Output: build/usr/joey/joey — the ELF that tools/build.sh kernel
# (build_ramfs) copies into the cpio so devramfs exposes it as /joey.
# kernel/joey.c looks it up by name and exec_setup's the bytes into the
# child Proc.

add_executable(joey joey.c)
target_link_libraries(joey PRIVATE t)

# Strip output name so the cpio entry is exactly "joey" (not "joey.elf").
set_target_properties(joey PROPERTIES
    SUFFIX ""
    OUTPUT_NAME joey
)
