#!/usr/bin/env bash
# tools/build.sh — Thylacine top-level build wrapper.
#
# Per CLAUDE.md "Build + test commands" and TOOLING.md §9: thin wrapper around
# CMake (kernel) and Cargo (userspace; appears post-P1-A as Rust components
# arrive).
#
# Usage:
#   tools/build.sh kernel        — build the kernel ELF
#   tools/build.sh sysroot       — build musl + sysroot (Phase 5+)
#   tools/build.sh userspace     — build all Rust userspace components (Phase 3+)
#   tools/build.sh disk          — assemble build/disk.img (Phase 4+)
#   tools/build.sh all           — kernel + sysroot + userspace + disk
#   tools/build.sh clean         — remove build artifacts
#
# Options:
#   --release        — Release build (-O2, no debug). Default is Debug (with assertions).
#   --hardening-full — Enable P1-H hardening flags (lands at P1-H).
#   --kaslr          — Enable KASLR (lands at P1-C).
#   --verbose        — Verbose CMake/Cargo output.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$REPO_ROOT/build"
KERNEL_BUILD="$BUILD_DIR/kernel"
TOOLCHAIN_FILE="$REPO_ROOT/cmake/Toolchain-aarch64-thylacine.cmake"

target="${1:-all}"
shift || true

build_type="Debug"
hardening_full="OFF"
kaslr="OFF"
verbose=""
extra_cmake_args=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        --release)
            build_type="Release"
            shift
            ;;
        --hardening-full)
            hardening_full="ON"
            shift
            ;;
        --kaslr)
            kaslr="ON"
            shift
            ;;
        --verbose)
            verbose="--verbose"
            shift
            ;;
        --)
            shift
            extra_cmake_args+=("$@")
            break
            ;;
        *)
            echo "Unknown option: $1" >&2
            exit 1
            ;;
    esac
done

build_kernel() {
    echo "==> Building kernel (build_type=$build_type, hardening=$hardening_full, kaslr=$kaslr)"
    cmake -S "$REPO_ROOT" -B "$KERNEL_BUILD" \
        -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE" \
        -DCMAKE_BUILD_TYPE="$build_type" \
        -DTHYLACINE_HARDENING_FULL="$hardening_full" \
        -DTHYLACINE_KASLR="$kaslr" \
        ${extra_cmake_args[@]+"${extra_cmake_args[@]}"}
    cmake --build "$KERNEL_BUILD" $verbose
    echo "==> Kernel built: $KERNEL_BUILD/thylacine.elf"
    ls -la "$KERNEL_BUILD/thylacine.elf"
}

build_sysroot() {
    echo "==> sysroot is a Phase 5 deliverable; not yet implemented."
    exit 1
}

build_userspace() {
    echo "==> userspace is a Phase 3 deliverable; not yet implemented."
    exit 1
}

build_disk() {
    echo "==> disk is a Phase 4 deliverable; not yet implemented."
    exit 1
}

build_all() {
    build_kernel
    # Other components will be enabled as their phases land.
}

clean() {
    echo "==> Removing $BUILD_DIR"
    rm -rf "$BUILD_DIR"
}

case "$target" in
    kernel)    build_kernel    ;;
    sysroot)   build_sysroot   ;;
    userspace) build_userspace ;;
    disk)      build_disk      ;;
    all)       build_all       ;;
    clean)     clean           ;;
    *)
        echo "Unknown target: $target" >&2
        echo "Valid: kernel, sysroot, userspace, disk, all, clean" >&2
        exit 1
        ;;
esac
