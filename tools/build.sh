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
#   tools/build.sh sysroot       — build the pouch POSIX sysroot (Phase 6)
#   tools/build.sh pouch-progs   — build the pouch POSIX test programs (Phase 6)
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
# LLVM install prefix for the pouch sysroot build (clang/llvm-ar/llvm-ranlib).
# Mirrors cmake/Toolchain-aarch64-pouch.cmake + tools/pouch-clang.
LLVM_PREFIX="${LLVM_PREFIX:-/opt/homebrew/opt/llvm}"
# lld install prefix — ld.lld links the pouch test programs (Homebrew ships
# lld as a package separate from llvm). Mirrors the cmake toolchains.
LLD_PREFIX="${LLD_PREFIX:-/opt/homebrew/opt/lld}"

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
    # P6-pouch-hello-smoke: build the pouch POSIX test programs too, so
    # build_ramfs ships /pouch-hello + /pouch-hello-stdio.
    build_userspace
    build_pouch_progs
    build_ramfs

    # P4-Ic5b2: produce build/disk.img alongside the kernel so
    # tools/test.sh's QEMU launch can attach it as virtio-blk-device's
    # backing store. Regeneration is cheap (~1 MiB sparse-write) and
    # keeps the on-disk signature byte-identical across rebuilds.
    build_disk
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
    local usr_bins=( "hello" "joey" "pipe-probe" "attach-probe" "stratumd-stub" "stub-driver" "stub-fs-probe" "stub-walk-probe" )
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
    local usr_rs_bins=( "hello-rs" "mmio-probe" "irq-probe" "virtio-blk-probe" "virtio-blk-rw" "virtio-net-probe" "virtio-net-arp" "virtio-net-loop" "virtio-input" "virtio-gpu" "irq-bench" "corvus" )
    local rs_release="$USR_RS_BUILD/$USR_RS_TARGET/release"
    for bin in "${usr_rs_bins[@]}"; do
        local src="$rs_release/$bin"
        if [[ -f "$src" ]]; then
            cp "$src" "$ramfs_src/$bin"
            chmod 0755 "$ramfs_src/$bin"
        fi
    done

    # P6-pouch-hello-smoke: copy the pouch POSIX test binaries (built
    # against the pouch sysroot by build_pouch_progs) into the cpio root.
    # Same curation discipline — explicit list, not a glob.
    local pouch_bins=( "pouch-hello" "pouch-hello-stdio" )
    local pouch_progs="$BUILD_DIR/pouch/progs"
    for bin in "${pouch_bins[@]}"; do
        local src="$pouch_progs/$bin"
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
    # Phase 6 (Pouch) — build the pouch POSIX libc + cross-compilation sysroot.
    #
    # pouch is a musl derivative: vendored-pristine musl (third_party/musl/)
    # plus the boundary-line patch series (usr/lib/pouch/patches/). This
    # target copies the vendored tree to a disposable working copy, applies
    # the series, cross-builds the libc for aarch64-thylacine, and installs
    # headers + libc.a + CRT objects into build/sysroot/. third_party/musl/
    # is never edited. See docs/POUCH-DESIGN.md §4-5 + docs/reference/78-pouch.md.
    #
    # The build is from-scratch each run (~1-2 min): the working copy and
    # object tree are removed and rebuilt so a stale patch never lingers.
    local sysroot="$BUILD_DIR/sysroot"
    local pouch_dir="$BUILD_DIR/pouch"
    local musl_src="$pouch_dir/musl-src"
    local musl_obj="$pouch_dir/musl-obj"
    local vendored="$REPO_ROOT/third_party/musl"
    local patches_dir="$REPO_ROOT/usr/lib/pouch/patches"
    local clang="$LLVM_PREFIX/bin/clang"
    local jobs
    jobs="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"

    if [[ ! -x "$clang" ]]; then
        echo "==> pouch sysroot: clang not found at $clang" >&2
        echo "    override with LLVM_PREFIX=/path tools/build.sh sysroot" >&2
        exit 1
    fi
    if [[ ! -d "$vendored" ]]; then
        echo "==> pouch sysroot: vendored musl missing at $vendored" >&2
        exit 1
    fi

    echo "==> pouch sysroot: $sysroot"

    # 1. fresh working copy — third_party/musl/ stays pristine.
    rm -rf "$pouch_dir" "$sysroot"
    mkdir -p "$pouch_dir" "$musl_obj" "$sysroot"
    cp -R "$vendored" "$musl_src"

    # 2. apply the boundary-line patch series. A failed patch aborts the
    #    build (set -e); a rejected hunk also leaves a .rej file, caught
    #    below. The read loop's || [[ -n ]] guard handles a final
    #    newline-less line in series.
    echo "==> applying pouch patch series"
    while IFS= read -r patch_line || [[ -n "$patch_line" ]]; do
        case "$patch_line" in ''|\#*) continue ;; esac
        echo "    patch: $patch_line"
        patch -p1 -t -d "$musl_src" -i "$patches_dir/$patch_line"
    done < "$patches_dir/series"
    local rej
    rej="$(find "$musl_src" -name '*.rej')"
    if [[ -n "$rej" ]]; then
        echo "==> pouch sysroot: patch series left rejected hunks:" >&2
        echo "$rej" >&2
        exit 1
    fi

    # 3. configure the pouch libc out-of-tree for aarch64-thylacine. Same
    #    toolchain tools/pouch-clang wraps; clang treats "thylacine" as an
    #    unknown OS, so pouch drives the target explicitly (invariant P-1).
    echo "==> configuring musl (aarch64-thylacine)"
    ( cd "$musl_obj" && sh "$musl_src/configure" \
        --target=aarch64-thylacine \
        --prefix="$sysroot" \
        --disable-shared \
        CC="$clang --target=aarch64-thylacine" \
        AR="$LLVM_PREFIX/bin/llvm-ar" \
        RANLIB="$LLVM_PREFIX/bin/llvm-ranlib" )

    # 4. build, then install headers + libc.a + CRT objects. install-tools
    #    is skipped — pouch ships tools/pouch-clang, not musl's musl-gcc.
    echo "==> building pouch libc (-j$jobs)"
    ( cd "$musl_obj" && make -j"$jobs" )
    ( cd "$musl_obj" && make install-libs install-headers )

    # 5. verify the install, that the syscall-seam retarget landed (all eight
    #    1:1 numbers + the sentinel — pins the awk retarget against
    #    kernel/include/thylacine/syscall.h), and that the cancellable-path
    #    sentinel guard is present.
    local syscall_h="$sysroot/include/bits/syscall.h"
    local fail=0
    [[ -f "$sysroot/lib/libc.a" ]]   || { echo "    MISSING: lib/libc.a"   >&2; fail=1; }
    [[ -f "$sysroot/lib/crt1.o" ]]   || { echo "    MISSING: lib/crt1.o"   >&2; fail=1; }
    [[ -f "$sysroot/lib/crti.o" ]]   || { echo "    MISSING: lib/crti.o"   >&2; fail=1; }
    [[ -f "$sysroot/lib/crtn.o" ]]   || { echo "    MISSING: lib/crtn.o"   >&2; fail=1; }
    [[ -f "$sysroot/include/stdio.h" ]] || { echo "    MISSING: include/stdio.h" >&2; fail=1; }
    if [[ -f "$syscall_h" ]]; then
        local seam
        for seam in 'SYS_read 9' 'SYS_write 10' 'SYS_close 11' 'SYS_exit 0' \
                    'SYS_exit_group 0' 'SYS_mlockall 16' 'SYS_getrandom 20' \
                    'SYS_set_tid_address 36' 'SYS_writev 0xFFFF' \
                    'SYS_socket 0xFFFF'; do
            grep -q "^#define $seam\$" "$syscall_h" || {
                echo "    SEAM: '#define $seam' missing from bits/syscall.h" >&2
                fail=1
            }
        done
    else
        echo "    MISSING: include/bits/syscall.h" >&2; fail=1
    fi
    grep -q 'POUCH_SYSCALL_UNIMPL' "$musl_src/src/thread/__syscall_cp.c" || {
        echo "    SEAM: cancellable-path sentinel guard missing in __syscall_cp.c" >&2
        fail=1
    }
    if [[ "$fail" -ne 0 ]]; then
        echo "==> pouch sysroot FAILED verification" >&2
        exit 1
    fi

    echo "==> pouch sysroot ready:"
    echo "    libc.a   $(wc -c < "$sysroot/lib/libc.a" | tr -d ' ') bytes"
    echo "    CRT      crt1.o crti.o crtn.o"
    echo "    headers  $(find "$sysroot/include" -name '*.h' | wc -l | tr -d ' ') files"
    echo "    seam     syscall table retargeted to the Thylacine ABI"
}

build_pouch_progs() {
    # Phase 6 (Pouch) — cross-compile the pouch POSIX test programs (the
    # hello binaries) against the pouch sysroot. These are the first
    # POSIX C programs Thylacine runs: hosted (musl CRT + libc.a), not
    # freestanding. See docs/reference/78-pouch.md + docs/POUCH-DESIGN.md §14.
    #
    # Two steps, deliberately:
    #   1. compile each .c with tools/pouch-clang (clang as the compiler).
    #   2. link directly with ld.lld. clang as the *link driver* cannot be
    #      used: for the unknown "thylacine" OS on a macOS host clang falls
    #      into the Darwin toolchain and emits Mach-O linker arguments. The
    #      compiler path is unaffected. (docs/reference/78-pouch.md "Build".)
    #
    # The link line produces a static, non-PIE ET_EXEC with page-aligned
    # PT_LOAD file offsets — the layout kernel/elf.c + exec_map_segment
    # accept with no kernel change. -z separate-loadable-segments is what
    # page-aligns every PT_LOAD's file offset (exec_map_segment requires it).
    #
    # The sysroot must exist; it is built on demand here if absent
    # (build_sysroot, ~1-2 min). A patch-series change needs an explicit
    # `tools/build.sh sysroot` to refresh it — this step reuses what it finds.
    local sysroot="$BUILD_DIR/sysroot"
    local progs_out="$BUILD_DIR/pouch/progs"
    local src_dir="$REPO_ROOT/usr/pouch-hello"
    local pouch_clang="$REPO_ROOT/tools/pouch-clang"
    local lld="$LLD_PREFIX/bin/ld.lld"
    local readelf="$LLVM_PREFIX/bin/llvm-readelf"

    if [[ ! -x "$lld" ]]; then
        echo "==> pouch progs: ld.lld not found at $lld" >&2
        echo "    override with LLD_PREFIX=/path tools/build.sh ..." >&2
        exit 1
    fi
    if [[ ! -f "$sysroot/lib/libc.a" ]]; then
        echo "==> pouch progs: sysroot absent — building it first"
        build_sysroot
    else
        echo "==> pouch progs: reusing $sysroot (run 'tools/build.sh sysroot' to refresh)"
    fi

    rm -rf "$progs_out"
    mkdir -p "$progs_out"

    local prog
    for prog in pouch-hello pouch-hello-stdio; do
        echo "==> pouch prog: $prog"
        # 1. compile (clang). -nostdinc + -isystem: pouch owns the include
        #    path. -fno-pie: non-PIC codegen for a fixed-address ET_EXEC.
        "$pouch_clang" -std=gnu11 -O2 -Wall -Wextra \
            -nostdinc -isystem "$sysroot/include" -fno-pie \
            -c "$src_dir/$prog.c" -o "$progs_out/$prog.o"
        # 2. link (ld.lld). Static non-PIE ET_EXEC; the musl CRT objects
        #    in link order (crt1 crti ... crtn); libc.a via -lc.
        "$lld" -static -o "$progs_out/$prog" \
            -z separate-loadable-segments \
            -z max-page-size=4096 \
            -z noexecstack \
            --build-id=none \
            "$sysroot/lib/crt1.o" "$sysroot/lib/crti.o" \
            "$progs_out/$prog.o" \
            -L"$sysroot/lib" -lc \
            "$sysroot/lib/crtn.o"
        # verify the layout the kernel ELF loader requires: ET_EXEC, no
        # PT_DYNAMIC. A loader-incompatible binary fails here, not at boot.
        # readelf output is captured first (a `grep -q` pipeline would
        # SIGPIPE the producer and, under pipefail, read as a failure).
        local elf_hdr elf_phdrs
        elf_hdr="$("$readelf" -h "$progs_out/$prog")"
        elf_phdrs="$("$readelf" -l "$progs_out/$prog")"
        case "$elf_hdr" in
            *"Type:"*EXEC*) ;;
            *) echo "    $prog: not ET_EXEC — kernel/elf.c would reject it" >&2
               exit 1 ;;
        esac
        case "$elf_phdrs" in
            *DYNAMIC*) echo "    $prog: has PT_DYNAMIC — kernel/elf.c would reject it" >&2
                       exit 1 ;;
        esac
        echo "    $prog: $(wc -c < "$progs_out/$prog" | tr -d ' ') bytes (ET_EXEC, static)"
    done
    echo "==> pouch progs built under $progs_out"
}

build_disk() {
    # P4-Ic5b2 / P4-Ic7: deterministic raw disk image backing QEMU's
    # virtio-blk-device.
    #
    # Layout (mirrored verbatim in tools/mkdisk.py + the userspace
    # verifier in usr/virtio-blk-rw/src/main.rs):
    #   - sector 0  : bytes [0..16) = "THYLACINE-DISK-1" + zeros.
    #                 P4-Ic5b2's virtio-blk-probe verifies sector 0.
    #   - sector k>0: bytes [0..512) = pattern_a(k), the per-sector
    #                 LCG-A stream. P4-Ic7's virtio-blk-rw verifies a
    #                 prefix of sectors and writes a distinct pattern_b
    #                 to a non-overlapping region.
    #
    # Size (via THYLACINE_DISK_SIZE, default 16M):
    #   - 16M default = ample multi-sector range without slowing
    #     `tools/test.sh` (mkdisk.py runtime <500ms).
    #   - Stress (ROADMAP §6.2 exit criterion = read 1 GiB + write 1 GiB
    #     + verify bit-exact): THYLACINE_DISK_SIZE=1G tools/build.sh kernel
    #     + THYLACINE_DISK_SIZE=1G BOOT_TIMEOUT=120 tools/test.sh.
    #     mkdisk.py runtime ~30s for 1 GiB.
    local disk="$BUILD_DIR/disk.img"
    local size_spec="${THYLACINE_DISK_SIZE:-16M}"
    # Parse units (M = MiB, G = GiB). Bytes-only if no suffix.
    local size_bytes
    case "$size_spec" in
        *M) size_bytes=$(( ${size_spec%M} * 1024 * 1024 )) ;;
        *G) size_bytes=$(( ${size_spec%G} * 1024 * 1024 * 1024 )) ;;
        *K) size_bytes=$(( ${size_spec%K} * 1024 )) ;;
        *)  size_bytes="$size_spec" ;;
    esac
    echo "==> Building disk image: $disk (THYLACINE_DISK_SIZE=$size_spec → $size_bytes bytes)"
    mkdir -p "$BUILD_DIR"
    python3 "$REPO_ROOT/tools/mkdisk.py" "$disk" "$size_bytes"
    local actual_size
    actual_size="$(wc -c < "$disk" | tr -d ' ')"
    if [[ "$actual_size" != "$size_bytes" ]]; then
        echo "    ERROR: size mismatch (wanted $size_bytes, got $actual_size)" >&2
        exit 1
    fi
    local readback
    readback="$(head -c 16 "$disk")"
    if [[ "$readback" != "THYLACINE-DISK-1" ]]; then
        echo "    ERROR: block 0 signature mismatch (got '$readback')" >&2
        exit 1
    fi
    echo "    block 0 signature: '$readback' (16 bytes)"
    echo "    size: $actual_size bytes"
}

build_all() {
    # build_kernel calls build_userspace + build_ramfs + build_disk internally.
    build_kernel
}

clean() {
    echo "==> Removing $BUILD_DIR"
    rm -rf "$BUILD_DIR"
}

case "$target" in
    kernel)      build_kernel      ;;
    ramfs)       build_ramfs       ;;
    sysroot)     build_sysroot     ;;
    pouch-progs) build_pouch_progs ;;
    userspace)   build_userspace   ;;
    disk)        build_disk        ;;
    all)         build_all         ;;
    clean)       clean             ;;
    *)
        echo "Unknown target: $target" >&2
        echo "Valid: kernel, ramfs, sysroot, pouch-progs, userspace, disk, all, clean" >&2
        exit 1
        ;;
esac
