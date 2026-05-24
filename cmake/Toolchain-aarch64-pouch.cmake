# Cross-compile toolchain for Thylacine pouch — the POSIX environment.
#
# Phase 6 (Pouch). Builds C programs against the pouch libc (the
# Thylacine-native POSIX libc) for the aarch64-thylacine target. Used by
# CMake-based pouch consumers (stratumd, the pouch test programs).
# Plain-Makefile / autotools consumers use tools/pouch-clang instead;
# both converge on the same target + sysroot.
#
# Siblings:
#   cmake/Toolchain-aarch64-thylacine.cmake  — the KERNEL toolchain.
#   cmake/Toolchain-aarch64-userspace.cmake  — native freestanding
#                                              userspace (joey, libt, ...).
# This file — pouch-hosted userspace (POSIX C programs).
#
# The triple is aarch64-thylacine (distinct from the freestanding
# aarch64-none-elf used by the kernel + native userspace), so pouch-built
# binaries are distinguishable and the toolchain can carry pouch defaults.
# clang treats "thylacine" as an unknown OS — exactly right: clang applies
# no foreign-OS defaults; pouch + this file drive everything explicitly
# (invariant P-1 — POUCH-DESIGN.md §11).
#
# The sysroot (build/sysroot) is created by `tools/build.sh sysroot` and
# populated by Pouch sub-chunks 2-5 (musl headers + libc.a + CRT objects).
# Until then this toolchain compiles -nostdinc / header-less translation
# units. See docs/POUCH-DESIGN.md §9 + docs/phase6-status.md.

set(CMAKE_SYSTEM_NAME      Generic)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

# Homebrew install paths on Apple Silicon. Override via -DLLVM_PREFIX=/path.
if(NOT DEFINED LLVM_PREFIX)
    set(LLVM_PREFIX /opt/homebrew/opt/llvm)
endif()
if(NOT DEFINED LLD_PREFIX)
    set(LLD_PREFIX /opt/homebrew/opt/lld)
endif()

# The pouch sysroot. Override via -DTHYLACINE_SYSROOT=/path.
if(NOT DEFINED THYLACINE_SYSROOT)
    set(THYLACINE_SYSROOT "${CMAKE_CURRENT_LIST_DIR}/../build/sysroot")
endif()

set(CMAKE_C_COMPILER   ${LLVM_PREFIX}/bin/clang)
set(CMAKE_ASM_COMPILER ${LLVM_PREFIX}/bin/clang)
set(CMAKE_LINKER       ${LLD_PREFIX}/bin/ld.lld)
set(CMAKE_OBJCOPY      ${LLVM_PREFIX}/bin/llvm-objcopy)
set(CMAKE_OBJDUMP      ${LLVM_PREFIX}/bin/llvm-objdump)
set(CMAKE_READELF      ${LLVM_PREFIX}/bin/llvm-readelf)
set(CMAKE_AR           ${LLVM_PREFIX}/bin/llvm-ar)
set(CMAKE_RANLIB       ${LLVM_PREFIX}/bin/llvm-ranlib)
set(CMAKE_NM           ${LLVM_PREFIX}/bin/llvm-nm)
set(CMAKE_STRIP        ${LLVM_PREFIX}/bin/llvm-strip)

# CMake passes --target= for these.
set(CMAKE_C_COMPILER_TARGET   aarch64-thylacine)
set(CMAKE_ASM_COMPILER_TARGET aarch64-thylacine)

# CMake passes --sysroot= for this.
set(CMAKE_SYSROOT "${THYLACINE_SYSROOT}")

# Skip CMake's compiler probes — until Pouch sub-chunks 2-5 install
# libc.a + CRT objects there is nothing to link a probe executable
# against. Mirrors the native-userspace toolchain.
set(CMAKE_C_COMPILER_WORKS   TRUE)
set(CMAKE_ASM_COMPILER_WORKS TRUE)

set(CMAKE_CROSSCOMPILING TRUE)
set(CMAKE_FIND_ROOT_PATH "${THYLACINE_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

# Compile flags — applied automatically to every C / ASM compile.
#
#   -march: the Thylacine hardware baseline (ARMv8-A + LSE atomics +
#           pointer auth + BTI). Matches the kernel + native userspace.
#   -nostdlibinc + -isystem <sysroot>/include: pouch owns the libc + system
#           include paths; no host system headers leak in. Unlike -nostdinc,
#           this keeps clang's OWN resource headers on the path (stdint.h /
#           stdarg.h / arm_neon.h / unwind.h — all compiler-provided), which
#           portable C libraries (xxHash, BLAKE3) reference unconditionally.
#   -D__thylacine__: the Thylacine platform identifier. Programs that
#           need a Thylacine-specific arm (stratumd's peer_creds.c,
#           future ports) gate on this macro. Set unconditionally — the
#           triple is aarch64-thylacine; the macro is the C-visible peer.
#   -D_GNU_SOURCE=1: pouch is musl-based, and musl's headers expose the
#           Linux/glibc-compatible API surface (struct ucred, getrandom,
#           fallocate, ...) only when _GNU_SOURCE is defined. Projects
#           ported to pouch (stratumd, libsodium) generally assume the
#           full POSIX + GNU surface; setting it at the toolchain level
#           keeps each consumer from re-stating it.
#   hardening: stack protector, stack-clash, PAC+BTI branch protection —
#           Thylacine hardening discipline; on for the projects we own.
#
# NOT set here: -ffreestanding (pouch programs are HOSTED — pouch is the
# libc); -std (each project picks its own); -O / -g (CMAKE_BUILD_TYPE
# drives those).
set(_pouch_c_flags
    "-march=armv8-a+lse+pauth+bti"
    "-nostdlibinc"
    "-isystem ${THYLACINE_SYSROOT}/include"
    "-D__thylacine__=1"
    "-D_GNU_SOURCE=1"
    "-fno-common"
    "-fno-omit-frame-pointer"
    "-fstack-protector-strong"
    "-mbranch-protection=pac-ret+bti"
)
string(JOIN " " CMAKE_C_FLAGS_INIT ${_pouch_c_flags})
set(CMAKE_ASM_FLAGS_INIT "-march=armv8-a+lse+pauth+bti")

# Link via tools/pouch-ld — the link half of the pouch toolchain. Drives
# ld.lld directly with the static/W^X/non-PIE link line + auto-supplies
# the CRT objects (crt1/crti/crtn) + libc.a + libclang_rt.builtins.a from
# the sysroot. clang cannot drive the ELF link for aarch64-thylacine on
# a macOS host — its driver mis-selects the host Darwin toolchain. See
# tools/pouch-ld + docs/reference/78-pouch.md.
#
# Override CMAKE_C_LINK_EXECUTABLE rather than CMAKE_EXE_LINKER_FLAGS_INIT
# because pouch-ld is not a clang front-end — it's a direct ld.lld driver
# (no -Wl, prefix on its args, takes objects + -o + extra libs).
set(CMAKE_C_LINK_EXECUTABLE
    "${CMAKE_CURRENT_LIST_DIR}/../tools/pouch-ld <OBJECTS> -o <TARGET> <LINK_LIBRARIES>")
