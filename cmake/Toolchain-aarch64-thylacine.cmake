# Cross-compile toolchain for Thylacine kernel.
#
# Target: aarch64-none-elf (Arm's preferred bare-metal triple).
# Compiler: clang from Homebrew LLVM (Apple Silicon Mac dev host).
# Linker: ld.lld from Homebrew lld.
# Object utilities: llvm-objcopy, llvm-objdump, llvm-readelf.
#
# Per ARCHITECTURE.md §3: kernel is C99; build system is CMake (kernel) + Cargo (Rust).
# Per TOOLING.md §9: thin wrapper convention; this file is the toolchain plumbing.

set(CMAKE_SYSTEM_NAME      Generic)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

# Homebrew install paths on Apple Silicon. Override via -DLLVM_PREFIX=/path if elsewhere.
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

# Skip CMake's compiler probes — they require linking to a host runtime that
# doesn't exist for our freestanding target. CMake otherwise tries to compile
# a "test program" that calls printf, which fails on freestanding aarch64.
set(CMAKE_C_COMPILER_WORKS   TRUE)
set(CMAKE_ASM_COMPILER_WORKS TRUE)

# Tell CMake we're cross-compiling so it doesn't try to run host binaries.
set(CMAKE_CROSSCOMPILING TRUE)

# Don't search the host system for libraries / headers / programs.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

set(THYLACINE_TARGET_TRIPLE "aarch64-none-elf"
    CACHE STRING "Target triple for kernel cross-compile")

# Baseline architecture flags. ARMv8-A baseline at P1-A; hardening flags
# (PAC/MTE/BTI/LSE/CFI/canaries) layered in at P1-H.
#
# -ffreestanding: no hosted environment (no libc; we provide our own).
# -nostdlib + -nostartfiles: no host startup; our start.S is the entry.
# -fno-builtin: don't assume libc semantics for memcpy/strlen/etc.
# -fno-common: BSS variables go in .bss, not COMMON (deterministic linkage).
# -fno-stack-protector: explicitly disabled at P1-A; P1-H enables canaries.
# -mgeneral-regs-only: no FP/SIMD in kernel (saves context-switch overhead;
#   userspace gets full FP).
# -mno-red-zone: ARM64 has no red zone but the flag is harmless on AArch64.
# -fpie: position-independent executable. P1-C-extras Part B switches the
#   kernel to PIE so KASLR can apply R_AARCH64_RELATIVE relocations and
#   slide the kernel's high-VA base by a random offset at boot. PIC code
#   uses PC-relative adrp+add for symbol references, naturally adapting to
#   whichever address the kernel is running at; only absolute pointer
#   references in static data (none in our minimal kernel today, but
#   future-proof) need the relocation table walk.
# -mcmodel=tiny: ARM64 small-code-model relocations (adrp/add only,
#   ±4 GiB code/data spread). Keeps the relocation table small and
#   permits direct PC-relative addressing of every symbol in the
#   image. (The kernel image is < 200 KB; tiny is fine.)
set(THYLACINE_KERNEL_C_FLAGS
    "--target=${THYLACINE_TARGET_TRIPLE}"
    "-march=armv8-a"
    "-ffreestanding"
    "-fno-builtin"
    "-fno-common"
    "-fno-stack-protector"
    "-fpie"
    "-fdirect-access-external-data"
    "-mcmodel=tiny"
    "-mgeneral-regs-only"
    "-mno-outline-atomics"
    "-fno-omit-frame-pointer"
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
# -fuse-ld=lld: use ld.lld (cross-platform ELF linker).
# -nostdlib + -nostartfiles: no host runtime.
# -static: no dynamic linking (kernel is self-contained).
# -Wl,-pie: emit a position-independent executable. Combined with -static,
#   produces a static-pie ELF: PC-relative code, R_AARCH64_RELATIVE entries
#   in .rela.dyn, no PT_INTERP. The kernel is its own dynamic linker.
# -Wl,-z,text: forbid text relocations (force PIC-only code generation).
#   Surfaces any code-side relocation as a build error rather than letting
#   it through silently.
# -Wl,--no-dynamic-linker: don't emit a PT_INTERP segment. The kernel has
#   no dynamic linker; this section would be dead weight in the binary.
# -Wl,--build-id=none: deterministic builds (no per-build hash).
# -Wl,--no-undefined: surface missing-symbol errors at link time, not
#   load time.
set(THYLACINE_KERNEL_LD_FLAGS
    "--target=${THYLACINE_TARGET_TRIPLE}"
    "-fuse-ld=lld"
    "--ld-path=${LLD_PREFIX}/bin/ld.lld"
    "-nostdlib"
    "-nostartfiles"
    "-static"
    "-Wl,-pie"
    "-Wl,-z,text"
    "-Wl,-z,norelro"
    "-Wl,-z,nopack-relative-relocs"
    "-Wl,--no-dynamic-linker"
    "-Wl,--build-id=none"
    "-Wl,--no-undefined"
)
