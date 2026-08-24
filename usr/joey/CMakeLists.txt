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

# BUILD-CONFIG-DESIGN.md 4.5: DEV_ACCOUNTS is the account-provisioning axis, split from
# BOOT_PROBES so a lean image (no probe ladder) is still loginnable. When ON and
# BOOT_PROBES is OFF, joey runs the minimal provision_dev_accounts (create michael);
# when BOOT_PROBES is ON its full ladder provisions instead (see the #if in joey.c).
if(THYLA_DEV_ACCOUNTS)
    target_compile_definitions(joey PRIVATE THYLA_DEV_ACCOUNTS)
endif()

# #229: the tripwire on that gate. A probe helper typed OUTSIDE the region above
# is dead code in the lean image, and the only signal was a warning in a build
# log nothing reads -- eleven had accumulated before anyone looked, because the
# lean shape is built by nothing in the normal loop. -Werror makes the build
# REFUSE, which is the form of the signal that cannot be scrolled past.
#
# This is what actually prevents a recurrence. Moving the helpers into their own
# region (or their own file) does not: the next person still types the new one
# wherever they happen to be editing. The compiler is the only reader that is
# always present. A function genuinely meant to be uncalled says so with
# __attribute__((unused)); silence is not one of the options.
target_compile_options(joey PRIVATE -Werror=unused-function)

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
