# Thylacine userspace tree — separate CMake project from the kernel.
#
# Per CLAUDE.md "Build + test commands" and docs/reference/38-userspace.md:
# the kernel and userspace are independent CMake projects because their
# toolchain flag-sets differ (kernel: -ffreestanding, -mgeneral-regs-only,
# -mcmodel=tiny, kernel-side hardening; userspace: -ffreestanding still
# (no hosted libc) but FP allowed, different linker script, different
# load base).
#
# Invoke via tools/build.sh userspace (preferred) or directly:
#   cmake -S usr -B build/usr \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/Toolchain-aarch64-userspace.cmake \
#         -DCMAKE_BUILD_TYPE=Debug
#   cmake --build build/usr
#
# Outputs: build/usr/hello/hello + build/usr/lib/libt.a.
# build_ramfs (in tools/build.sh) copies build/usr/hello/hello into the
# cpio so devramfs exposes it as /hello.

cmake_minimum_required(VERSION 3.20)

project(thylacine_userspace
    VERSION 0.1.0
    DESCRIPTION "Thylacine native userspace tree"
    LANGUAGES C ASM
)

# Compile/link flag plumbing comes from the toolchain file
# (cmake/Toolchain-aarch64-userspace.cmake). We re-export here so
# subdirectories can compose with target_compile_options().
add_compile_options(${THYLACINE_USERSPACE_C_FLAGS})
add_link_options(${THYLACINE_USERSPACE_LD_FLAGS})

# Path to the W^X linker script. Each executable applies it via
# target_link_options(-T ...).
set(USR_LINKER_SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/scripts/aarch64-userspace.ld
    CACHE FILEPATH "Userspace ELF linker script")

# Apply linker script to every executable in this tree. Done here so
# per-binary CMakeLists files stay minimal.
add_link_options(-T${USR_LINKER_SCRIPT})

add_subdirectory(lib/libt)
add_subdirectory(hello)
add_subdirectory(joey)
add_subdirectory(pipe-probe)
add_subdirectory(attach-probe)
add_subdirectory(stratumd-stub)
add_subdirectory(stub-driver)
add_subdirectory(stub-fs-probe)
add_subdirectory(stub-walk-probe)
add_subdirectory(thread-probe)
add_subdirectory(thread-fault-probe)
