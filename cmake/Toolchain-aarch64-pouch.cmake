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
#   -nostdinc + -isystem <sysroot>/include: pouch owns the entire include
#           path; no host headers leak in. (Empty until sub-chunk 3.)
#   hardening: stack protector, stack-clash, PAC+BTI branch protection —
#           Thylacine hardening discipline; on for the projects we own.
#
# NOT set here: -ffreestanding (pouch programs are HOSTED — pouch is the
# libc); -std (each project picks its own); -O / -g (CMAKE_BUILD_TYPE
# drives those).
set(_pouch_c_flags
    "-march=armv8-a+lse+pauth+bti"
    "-nostdinc"
    "-isystem ${THYLACINE_SYSROOT}/include"
    "-fno-common"
    "-fno-omit-frame-pointer"
    "-fstack-protector-strong"
    "-fstack-clash-protection"
    "-mbranch-protection=pac-ret+bti"
)
string(JOIN " " CMAKE_C_FLAGS_INIT ${_pouch_c_flags})
set(CMAKE_ASM_FLAGS_INIT "-march=armv8-a+lse+pauth+bti")

# Link flags — applied automatically to every executable link.
#
#   -fuse-ld=lld: the LLVM linker.
#   -static: v1.0 pouch is static-only (no dynamic linker / .so).
#   W^X + hardening linker flags: mirror the native-userspace toolchain.
#
# NOT yet set: the CRT objects (crt1/crti/crtn) + -lc. clang does not
# auto-locate them for the unknown "thylacine" OS; pouch link lines name
# them explicitly once sub-chunks 2-5 install them into the sysroot.
# See docs/POUCH-DESIGN.md §14.
set(_pouch_ld_flags
    "-fuse-ld=lld"
    "--ld-path=${LLD_PREFIX}/bin/ld.lld"
    "-static"
    "-Wl,-z,text"
    "-Wl,-z,noexecstack"
    "-Wl,-z,max-page-size=4096"
    "-Wl,--no-dynamic-linker"
    "-Wl,--build-id=none"
)
string(JOIN " " CMAKE_EXE_LINKER_FLAGS_INIT ${_pouch_ld_flags})
