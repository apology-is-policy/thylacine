# Cross-compile toolchain for Thylacine userspace.
#
# Sibling of cmake/Toolchain-aarch64-thylacine.cmake (kernel toolchain).
#
# Target: aarch64-none-elf (same triple as kernel — Thylacine native
# userspace is not Linux, so we don't use aarch64-unknown-linux-musl).
# Compiler: clang from Homebrew LLVM (same as kernel).
# Linker: ld.lld + usr/scripts/aarch64-userspace.ld linker script.
#
# Key differences from the kernel toolchain:
#   - NO -mgeneral-regs-only: userspace gets FP/SIMD (kernel saves the
#     userspace FP context on context switch; per ARCH §3.6).
#   - NO -mcmodel=tiny: tiny restricts symbol references to ±4 GiB which
#     suffices for the kernel image but cramps userspace if/when we
#     introduce shared libraries. small (default) is fine.
#   - NO -fpie at the C-flag level: userspace at v1.0 is static-only
#     (the linker script fixes load base at 0x400000). PIE userspace
#     arrives with ASLR-userspace in Phase 5+.
#   - Hardening stays — userspace gets canaries, PAC, BTI.
#   - W^X enforced at the linker level (the linker script emits text
#     and data as separate PT_LOAD segments).

set(CMAKE_SYSTEM_NAME      Generic)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

# Homebrew install paths on Apple Silicon. Override via -DLLVM_PREFIX=/path
# if elsewhere.
if(NOT DEFINED LLVM_PREFIX)
    set(LLVM_PREFIX /opt/homebrew/opt/llvm)
endif()
if(NOT DEFINED LLD_PREFIX)
    set(LLD_PREFIX /opt/homebrew/opt/lld)
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

# Skip CMake's compiler probes — they'd require linking against a host
# runtime that doesn't exist for our freestanding target.
set(CMAKE_C_COMPILER_WORKS   TRUE)
set(CMAKE_ASM_COMPILER_WORKS TRUE)

set(CMAKE_CROSSCOMPILING TRUE)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

set(THYLACINE_USERSPACE_TARGET_TRIPLE "aarch64-none-elf"
    CACHE STRING "Target triple for userspace cross-compile")

set(THYLACINE_USERSPACE_C_FLAGS
    "--target=${THYLACINE_USERSPACE_TARGET_TRIPLE}"
    "-march=armv8-a+lse+pauth+bti"
    "-ffreestanding"
    "-fno-builtin"
    "-fno-common"
    "-fno-omit-frame-pointer"
    "-fstack-protector-strong"
    "-fstack-clash-protection"
    "-mbranch-protection=pac-ret+bti"
    "-Wall"
    "-Wextra"
    "-Wstrict-prototypes"
    "-Wmissing-prototypes"
    "-Wno-unused-parameter"
    "-std=c99"
    "-O2"
    "-g"
)

# Linker flags.
# -static: no dynamic linking (v1.0 userspace is static).
# -nostdlib + -nostartfiles: no host libc / startup; libt provides _start.
# -Wl,--no-dynamic-linker: no PT_INTERP segment — static executable.
# -Wl,--build-id=none: deterministic builds.
# -Wl,--no-undefined: surface missing-symbol errors at link time.
# -Wl,-z,noexecstack: NX stack.
# -Wl,-z,text: forbid text relocations (forces PIC code generation).
# -Wl,-z,max-page-size=4096: ld.lld defaults to MAXPAGESIZE 0x10000 on
#   aarch64, which forces a 64-KiB zero-filled gap between the text and
#   data PT_LOAD segments in the file image. The kernel maps userspace
#   at 4-KiB granularity (arch/arm64/mmu.c PAGE_SIZE) so the 64-KiB
#   alignment delivers no runtime benefit while bloating every userspace
#   binary by ~64 KiB. Mirror of the Rust-side flag in usr/.cargo/config.toml.
set(THYLACINE_USERSPACE_LD_FLAGS
    "--target=${THYLACINE_USERSPACE_TARGET_TRIPLE}"
    "-fuse-ld=lld"
    "--ld-path=${LLD_PREFIX}/bin/ld.lld"
    "-nostdlib"
    "-nostartfiles"
    "-static"
    "-Wl,-z,text"
    "-Wl,-z,noexecstack"
    "-Wl,-z,max-page-size=4096"
    "-Wl,--no-dynamic-linker"
    "-Wl,--build-id=none"
    "-Wl,--no-undefined"
)
