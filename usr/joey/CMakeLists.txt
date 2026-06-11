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

# #61 (RW-11 R4-F2): when THYLA_BOOT_PROBES is ON (default) compile joey's
# boot-test probe ladder; OFF yields the lean production init (no probes). The
# option is declared in the parent usr/CMakeLists.txt.
if(THYLA_BOOT_PROBES)
    target_compile_definitions(joey PRIVATE THYLA_BOOT_PROBES)
endif()

# A-5c-c: the build generates the host-baked system recovery phrase header here
# (tools/build.sh emit_corvus_recovery_header). joey #includes it (guarded by
# __has_include) to drive the live RECOVER(system) boot E2E. Absent header ->
# the E2E is skipped (graceful, best-effort).
if(DEFINED THYLA_GENERATED_DIR)
    target_include_directories(joey PRIVATE "${THYLA_GENERATED_DIR}")
endif()

# Strip output name so the cpio entry is exactly "joey" (not "joey.elf").
set_target_properties(joey PROPERTIES
    SUFFIX ""
    OUTPUT_NAME joey
)
