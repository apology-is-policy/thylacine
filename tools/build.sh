#!/usr/bin/env bash
# tools/build.sh — Thylacine top-level build wrapper.
#
# Per CLAUDE.md "Build + test commands" and TOOLING.md §9: thin wrapper around
# CMake (kernel) and Cargo (userspace; appears post-P1-A as Rust components
# arrive).
#
# Usage:
#   tools/build.sh kernel        — build the kernel ELF
#   tools/build.sh userspace     — build native userspace binaries from usr/ (P4-Ia1+)
#   tools/build.sh ramfs         — assemble build/ramfs.cpio
#   tools/build.sh sysroot       — build musl + Linux-compat sysroot (Phase 6+)
#   tools/build.sh disk          — assemble build/disk.img (Phase 4+)
#   tools/build.sh all           — kernel + userspace + ramfs
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
USR_BUILD="$BUILD_DIR/usr"
USR_RS_BUILD="$BUILD_DIR/usr-rs"
TOOLCHAIN_FILE="$REPO_ROOT/cmake/Toolchain-aarch64-thylacine.cmake"
USR_TOOLCHAIN_FILE="$REPO_ROOT/cmake/Toolchain-aarch64-userspace.cmake"
USR_RS_TARGET="aarch64-unknown-none"

target="${1:-all}"
shift || true

build_type="Debug"
hardening_full="OFF"
kaslr="OFF"
sanitize=""
build_dir_override=""
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
        --sanitize=*)
            # P1-I: opt-in sanitizer build. Currently supports
            # --sanitize=undefined (UBSan trapping). KASAN deferred.
            sanitize="${1#--sanitize=}"
            shift
            ;;
        --build-dir=*)
            build_dir_override="${1#--build-dir=}"
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

# Translate user-friendly --sanitize values to the CMake variable.
sanitize_cmake=""
case "$sanitize" in
    "")              sanitize_cmake="" ;;
    ubsan|undefined) sanitize_cmake="undefined" ;;
    *)
        echo "Unknown --sanitize value: $sanitize (valid: ubsan/undefined)" >&2
        exit 1
        ;;
esac

# Allow overriding the kernel build directory so a sanitizer build doesn't
# clobber the production build's CMake cache.
if [[ -n "$build_dir_override" ]]; then
    KERNEL_BUILD="$build_dir_override"
elif [[ -n "$sanitize_cmake" ]]; then
    KERNEL_BUILD="$BUILD_DIR/kernel-${sanitize_cmake}"
fi

build_kernel() {
    echo "==> Building kernel (build_type=$build_type, hardening=$hardening_full, kaslr=$kaslr, sanitize='${sanitize_cmake}', dir=$KERNEL_BUILD)"
    cmake -S "$REPO_ROOT" -B "$KERNEL_BUILD" \
        -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE" \
        -DCMAKE_BUILD_TYPE="$build_type" \
        -DTHYLACINE_HARDENING_FULL="$hardening_full" \
        -DTHYLACINE_KASLR="$kaslr" \
        -DTHYLACINE_SANITIZE="$sanitize_cmake" \
        ${extra_cmake_args[@]+"${extra_cmake_args[@]}"}
    cmake --build "$KERNEL_BUILD" $verbose
    echo "==> Kernel built: $KERNEL_BUILD/thylacine.elf"
    ls -la "$KERNEL_BUILD/thylacine.elf"

    # P4-E: build the ramfs cpio alongside the kernel so QEMU's
    # -initrd has something to load. The cpio is independent of
    # the kernel ELF (loaded separately by the bootloader); rebuilding
    # it on every kernel build keeps the file table reproducible.
    #
    # P4-Ia1: build userspace first so build_ramfs picks up the latest
    # /hello (and future /<bin>) without an extra invocation.
    build_userspace
    build_ramfs
}

build_ramfs() {
    local ramfs_src="$BUILD_DIR/ramfs-src"
    local ramfs_out="$BUILD_DIR/ramfs.cpio"

    # Rebuild from scratch so removed userspace binaries don't linger.
    rm -rf "$ramfs_src"
    mkdir -p "$ramfs_src"

    # Smoke files (read-side checks for devramfs).
    cat > "$ramfs_src/welcome" <<'EOF'
Welcome to Thylacine ramfs.
EOF
    cat > "$ramfs_src/version" <<'EOF'
Thylacine v0.1-dev
EOF

    # P4-Ia1: copy any built C-side userspace binaries from build/usr
    # into the cpio root. The list is curated below (not glob) so an
    # accidental CMake byproduct doesn't get shipped. Each binary's
    # source-of-truth comment lives in usr/<name>/CMakeLists.txt.
    local usr_bins=( "hello" )
    for bin in "${usr_bins[@]}"; do
        local src="$USR_BUILD/$bin/$bin"
        if [[ -f "$src" ]]; then
            cp "$src" "$ramfs_src/$bin"
            chmod 0755 "$ramfs_src/$bin"
        fi
    done

    # P4-Ia2: copy any built Rust-side userspace binaries from
    # build/usr-rs/<target>/release/. Same curation discipline.
    # Binary name = crate's [[bin]] name = directory under usr/.
    local usr_rs_bins=( "hello-rs" "mmio-probe" "irq-probe" )
    local rs_release="$USR_RS_BUILD/$USR_RS_TARGET/release"
    for bin in "${usr_rs_bins[@]}"; do
        local src="$rs_release/$bin"
        if [[ -f "$src" ]]; then
            cp "$src" "$ramfs_src/$bin"
            chmod 0755 "$ramfs_src/$bin"
        fi
    done

    python3 "$REPO_ROOT/tools/mkcpio.py" "$ramfs_src" "$ramfs_out"
    echo "==> ramfs cpio: $ramfs_out"
}

build_userspace() {
    # C side — CMake.
    echo "==> Building userspace C (dir=$USR_BUILD)"
    cmake -S "$REPO_ROOT/usr" -B "$USR_BUILD" \
        -DCMAKE_TOOLCHAIN_FILE="$USR_TOOLCHAIN_FILE" \
        -DCMAKE_BUILD_TYPE="$build_type" \
        ${extra_cmake_args[@]+"${extra_cmake_args[@]}"}
    cmake --build "$USR_BUILD" $verbose
    echo "==> Userspace C built under $USR_BUILD"
    ls -la "$USR_BUILD/hello/hello" 2>/dev/null || true

    # Rust side — cargo. Optional: if rustup hasn't installed the
    # aarch64-unknown-none target, skip with a notice rather than
    # erroring. Native Thylacine binaries still ship via the C path;
    # Rust binaries (hello-rs, future driver crates) need the target.
    if rustup target list --installed 2>/dev/null | grep -q "^$USR_RS_TARGET$"; then
        echo "==> Building userspace Rust (target=$USR_RS_TARGET, dir=$USR_RS_BUILD)"
        ( cd "$REPO_ROOT/usr" && cargo build --release $verbose )
        echo "==> Userspace Rust built under $USR_RS_BUILD"
        ls -la "$USR_RS_BUILD/$USR_RS_TARGET/release/hello-rs" 2>/dev/null || true
    else
        echo "==> Skipping userspace Rust: rustup target $USR_RS_TARGET not installed."
        echo "    Install via: rustup target add $USR_RS_TARGET"
    fi
}

build_sysroot() {
    echo "==> sysroot (Linux-compat musl) is a Phase 6 deliverable."
    echo "    Native Thylacine userspace lives in usr/ — use 'build.sh userspace'."
    exit 1
}

build_disk() {
    echo "==> disk is a Phase 4 deliverable; not yet implemented."
    exit 1
}

build_all() {
    # build_kernel calls build_userspace + build_ramfs internally.
    build_kernel
}

clean() {
    echo "==> Removing $BUILD_DIR"
    rm -rf "$BUILD_DIR"
}

case "$target" in
    kernel)    build_kernel    ;;
    ramfs)     build_ramfs     ;;
    sysroot)   build_sysroot   ;;
    userspace) build_userspace ;;
    disk)      build_disk      ;;
    all)       build_all       ;;
    clean)     clean           ;;
    *)
        echo "Unknown target: $target" >&2
        echo "Valid: kernel, ramfs, sysroot, userspace, disk, all, clean" >&2
        exit 1
        ;;
esac
