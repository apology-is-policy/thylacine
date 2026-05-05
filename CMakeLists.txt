# Thylacine OS — top-level CMake build.
#
# Per ARCHITECTURE.md §3: CMake (kernel) + Cargo (Rust userspace).
# This file only configures the kernel build; Rust components have their own
# Cargo workspace at the project root (appears in P1-A onward as drivers land).
#
# Invoke via tools/build.sh (preferred) or directly:
#   cmake -S . -B build/kernel \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/Toolchain-aarch64-thylacine.cmake \
#         -DCMAKE_BUILD_TYPE=Debug
#   cmake --build build/kernel

cmake_minimum_required(VERSION 3.20)

# Project metadata. Version mirrors VISION revision history.
project(thylacine
    VERSION 0.1.0
    DESCRIPTION "Plan 9-heritage operating system, ARM64"
    LANGUAGES C ASM
)

# Phase tag — bumped per phase exit. Influences boot banner.
set(THYLACINE_PHASE "P1-I" CACHE STRING "Current implementation phase")

# THYLACINE_SANITIZE — opt-in sanitizer build (P1-I).
#
# Empty / unset (default): production build. No sanitizer instrumentation.
# "undefined":              UBSan trapping mode. -fsanitize=undefined
#                           -fsanitize-trap=undefined. Undefined behavior
#                           triggers a BRK instruction that the kernel's
#                           sync exception handler routes to extinction
#                           ("brk instruction (assertion?)"). No runtime
#                           support needed (no __ubsan_handle_*).
#
# Future: "address" would enable kernel ASan, but that needs a custom
# shadow allocator (freestanding kernel; no host ASan runtime). Deferred
# to post-v1.0 per ARCH §24.5 (userspace gets ASan via dev builds; kernel
# ASan is out-of-scope at v1.0).
#
# Use via:
#   cmake -B build/kernel-ubsan -DCMAKE_TOOLCHAIN_FILE=... -DTHYLACINE_SANITIZE=undefined
# Or:
#   tools/test.sh --sanitize=ubsan
set(THYLACINE_SANITIZE "" CACHE STRING "Sanitizer mode: '', 'undefined'")
if(THYLACINE_SANITIZE STREQUAL "undefined")
    add_compile_options(-fsanitize=undefined -fsanitize-trap=undefined)
endif()

# Build-time options. Each defaults to off at P1-A and gets enabled at later phases.
option(THYLACINE_HARDENING_FULL "Enable full hardening (CFI/PAC/MTE/BTI/LSE/canaries) — P1-H+" OFF)
option(THYLACINE_KASLR          "Enable KASLR — P1-C+" OFF)
option(THYLACINE_DEBUG_BUILD    "Debug build with extra assertions" ON)

# Compile/link flag plumbing comes from the toolchain file. We re-export here
# so subdirectories can compose with target_compile_options().
add_compile_options(${THYLACINE_KERNEL_C_FLAGS})
add_link_options(${THYLACINE_KERNEL_LD_FLAGS})

# Phase token gets baked into a compile-time #define for kernel/main.c.
add_compile_definitions(
    THYLACINE_PHASE_STRING="${THYLACINE_PHASE}"
    THYLACINE_VERSION_STRING="${PROJECT_VERSION}"
)

# Subdirectories.
add_subdirectory(kernel)
