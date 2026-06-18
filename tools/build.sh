#!/usr/bin/env bash
# tools/build.sh — Thylacine top-level build wrapper (CMake kernel + Cargo/clang
# userspace + pouch POSIX sysroot). Per CLAUDE.md "Build + test commands" +
# TOOLING.md §9.
#
# ============================================================================
# WHAT EACH TARGET (RE)BUILDS — READ THIS BEFORE BLAMING A "STALE" ARTIFACT.
# ============================================================================
# The bootable image (build/disk.img + build/ramfs.cpio) is what tools/test.sh
# boots. `kernel` and `all` both produce it; they are effectively the SAME
# command because `kernel` pulls the whole chain in this order:
#
#   all -> kernel -> { userspace, pouch-progs, stratumd, pool-fixture,
#                      ramfs, disk }
#
# Per-target (run `tools/build.sh <target>`):
#   all          full bootable image. ALIAS for `kernel` (same chain). USE THIS.
#   kernel       kernel ELF (CMake) + the entire userspace/ramfs/disk chain.
#   userspace    native libt C binaries (usr/*, incl. joey) + the Rust workspace
#                (usr/Cargo.toml: corvus, libthyla-rs, ut, ...). NOT the kernel.
#   sysroot      the pouch POSIX libc: copies pristine third_party/musl, applies
#                the usr/lib/pouch/patches/ series, cross-builds libc.a +
#                compiler-rt + libsodium into build/sysroot/. SLOW (~1-2 min).
#   pouch-progs  the pouch-hello-* test binaries (need the sysroot).
#   stratumd     the Stratum FS daemon (pouch binary; links the sysroot libc).
#   pool         regenerate build/fixtures/pool.img + system.key, THEN re-bake
#                the ramfs (so /system.key matches the fresh pool key).
#   ramfs        assemble build/ramfs.cpio (bakes the userspace bins + key).
#   disk         assemble build/disk.img.
#   clean        rm -rf build/  (the ONLY true from-scratch reset).
#
# ----------------------------------------------------------------------------
# CACHING / STALENESS — the two footguns that bite sessions:
# ----------------------------------------------------------------------------
# 1. The pouch SYSROOT (musl libc) is CACHED. `kernel`/`all` REUSE build/sysroot
#    if present -- EXCEPT this script now AUTO-REBUILDS it when any pouch patch
#    is newer than the built libc.a (see sysroot_is_stale). So editing a
#    usr/lib/pouch/patches/*.patch and running `all` rebuilds the libc + every
#    pouch consumer (stratumd) in lockstep. (Before this check, a patched ABI
#    silently linked against a STALE libc -- e.g. the A-2a t_stat 72->80 growth
#    overflowing stratumd's stale 72-byte buffer.) To FORCE it: `clean` then
#    `all`, or run `sysroot` directly.
# 2. The pool fixture's system.key is RANDOM per regeneration. The `pool` target
#    couples the ramfs re-bake to the pool re-bake so /system.key always matches
#    (a mismatch = STM_EBADTAG at mount; the year-long "AEGIS corruption" ghost).
#    A bare `kernel`/`all` regenerates BOTH together, so they always agree.
# 3. `ramfs.cpio` is NOT re-baked by `disk` (nor by `userspace`) alone -- only by
#    `ramfs`, `pool`, and the `kernel`/`all` chain. The devramfs (QEMU -initrd)
#    holds the PRE-PIVOT binaries: joey's boot chain + every boot-test probe that
#    runs BEFORE the pivot to the disk-backed FS (coreutil-smoke, the u-* tests,
#    the login E2E). So after editing a userspace binary, `userspace` + `disk`
#    boots the STALE pre-pivot binary from the old ramfs -- the change reaches
#    only the POST-pivot disk image. Re-bake with `ramfs` (or just use
#    `kernel`/`all`, which chains build_ramfs). TELL: a probe's self-reported
#    count/output does not move though you "rebuilt" (e.g. a newly-added
#    coreutil-smoke check still reports the old total).
#
# Every run prints a "SUMMARY for target ..." block at the END listing exactly
# what was BUILT / REUSED / PRESERVED -- read that to know the resulting state.
#
# Options:
#   --release        Release build (-O2, no debug). Default Debug (assertions on).
#   --hardening-full Enable P1-H hardening flags.
#   --kaslr          Enable KASLR.
#   --sanitize=ubsan UBSan kernel build (separate build dir).
#   --production     V1.0 lean boot shape: drop the in-kernel test suite
#                    (KERNEL_TESTS=OFF) + joey's boot-test probe ladder (#61).
#   --verbose        Verbose CMake/Cargo output.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$REPO_ROOT/build"
KERNEL_BUILD="$BUILD_DIR/kernel"
USR_BUILD="$BUILD_DIR/usr"
USR_RS_BUILD="$BUILD_DIR/usr-rs"
# Generated build artifacts (e.g. the A-5c-c system-recovery-phrase header that
# joey #includes for the live RECOVER(system) boot E2E). Not in git.
GEN_DIR="$BUILD_DIR/generated"
CORVUS_RECOVERY_HEADER="$GEN_DIR/corvus_system_recovery_phrase.h"
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
# #61 (RW-11 R4-F1/F2): production boot shape. ON (default) keeps the in-kernel
# test suite + joey's boot-test probe ladder (dev/CI); --production flips both
# OFF for the lean V1.0 boot-to-getty.
kernel_tests="ON"
boot_probes="ON"
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
        --production)
            # #61 (RW-11 R4-F1/F2): the V1.0 production boot shape. Drops both
            # the in-kernel test suite (KERNEL_TESTS=OFF) and joey's boot-test
            # probe ladder (THYLA_BOOT_PROBES=OFF), so the lean image boots
            # straight to the login getty.
            kernel_tests="OFF"
            boot_probes="OFF"
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

# --- build ledger -------------------------------------------------------------
# So a session reading the build output afterwards can see EXACTLY what was
# (re)built vs reused vs skipped -- without having to know each target's
# internal chain. Every top-level step records one line; the dispatch prints
# the SUMMARY at the very end. `ledger` both echoes live and accumulates.
BUILD_LEDGER=""
ledger() {
    BUILD_LEDGER="${BUILD_LEDGER}    - $*"$'\n'
    echo "==> [build.sh] $*"
}

# sysroot_is_stale — true (0) iff the pouch POSIX sysroot must be rebuilt: it
# is MISSING, or any boundary-line patch / the series file is NEWER than the
# built libc.a (i.e. a pouch patch was edited since the last sysroot build).
#
# This is the cache-invalidation that makes `all` (and any target that links
# pouch code) TRUSTWORTHY. Without it, editing a pouch patch left `all` silently
# reusing a STALE libc -- the exact footgun that masked the A-2a t_stat 72->80
# ABI growth (the kernel wrote 80 bytes into stratumd's stale 72-byte buffer, a
# silent stack overflow a "passing" boot hid). Same class as the pool/key
# coupling on the `pool` target. A genuine full nuke is `build.sh clean`.
sysroot_is_stale() {
    local libc="$BUILD_DIR/sysroot/lib/libc.a"
    [[ -f "$libc" ]] || return 0
    [[ -f "$BUILD_DIR/sysroot/lib/libclang_rt.builtins.a" ]] || return 0
    if [[ -n "$(find "$REPO_ROOT/usr/lib/pouch/patches" -type f -newer "$libc" 2>/dev/null)" ]]; then
        return 0
    fi
    return 1
}

# stratum_host_tools_stale -- true (0) iff the host-native stratum tools
# (stratum-mkfs / stratumd / stratum-fs) must be rebuilt: a binary is missing,
# OR a Stratum C source / header is newer than the built stratumd. Mirrors
# sysroot_is_stale (the A-2a stale-consumer footgun fix). The prior guard in
# build_stratum_pool_fixture rebuilt ONLY when a binary was missing, so a
# Stratum source edit (e.g. A-3's --bake-owner-uid flag) silently shipped a
# stale host stratumd and the pool bake failed with "unknown option".
stratum_host_tools_stale() {
    local stratum_src="${STRATUM_SRC:-$HOME/projects/stratum/v2}"
    local hb="$BUILD_DIR/host-stratum"
    local sd="$hb/src/cmd/stratumd/stratumd"
    [[ -x "$hb/src/cmd/stratum-mkfs/stratum-mkfs" ]] || return 0
    [[ -x "$sd" ]]                                   || return 0
    [[ -x "$hb/src/cmd/stratum-fs/stratum-fs" ]]     || return 0
    if [[ -n "$(find "$stratum_src/src" "$stratum_src/include" \
                     "$stratum_src/CMakeLists.txt" -newer "$sd" 2>/dev/null)" ]]; then
        return 0
    fi
    return 1
}

build_kernel() {
    echo "==> Building kernel (build_type=$build_type, hardening=$hardening_full, kaslr=$kaslr, sanitize='${sanitize_cmake}', tests=$kernel_tests, dir=$KERNEL_BUILD)"
    cmake -S "$REPO_ROOT" -B "$KERNEL_BUILD" \
        -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE" \
        -DCMAKE_BUILD_TYPE="$build_type" \
        -DTHYLACINE_HARDENING_FULL="$hardening_full" \
        -DTHYLACINE_KASLR="$kaslr" \
        -DTHYLACINE_SANITIZE="$sanitize_cmake" \
        -DKERNEL_TESTS="$kernel_tests" \
        ${extra_cmake_args[@]+"${extra_cmake_args[@]}"}
    cmake --build "$KERNEL_BUILD" $verbose
    # HX-2: bake the live symbol table from the linked ELF + re-link (two-pass;
    # shared with tools/test-fault.sh so every dump path is symbolized).
    "$REPO_ROOT/tools/regen-halls-symtab.sh" "$KERNEL_BUILD" "$verbose"
    echo "==> Kernel built: $KERNEL_BUILD/thylacine.elf"
    ledger "kernel ELF: BUILT ($build_type, hardening=$hardening_full, kaslr=$kaslr, sanitize='${sanitize_cmake:-none}', tests=$kernel_tests)"
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
    # P6-pouch-stratumd-boot (sub-chunk 16a): cross-build stratumd so it
    # lands in the ramfs alongside the pouch hello binaries. Incremental
    # on no-source-change rebuilds (CMake/ninja dep tracking inside
    # $stratumd_build keep this <5s warm; cold rebuild is ~2-3 min).
    build_stratumd
    # P6-pouch-stratumd-boot sub-chunk 16b-beta: produce the boot pool
    # fixture (pool.img + system.key) before build_ramfs so the keyfile
    # gets copied into the cpio at /etc/stratum/system.key.
    build_stratum_pool_fixture
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
    # U-6e-a: the `source` builtin's read fixture (/u-builtin-test sources
    # this and asserts the assignment + fn registration persist into the
    # caller's Env).
    cat > "$ramfs_src/builtin-test.rc" <<'EOF'
let sourced_var = ok
fn sourced_fn { true }
EOF

    # D2 demo: a `#!/bin/ut` field-report script, baked 0755 so a logged-in
    # user can run it as a bare `fun.ut` (the cpio root binds at /bin post-
    # pivot). Exercises the shebang + `ut SCRIPT` execution path. The heredoc
    # is QUOTED so `$cwd` / `$(...)` stay literal for the script to interpret.
    cat > "$ramfs_src/fun.ut" <<'EOF'
#!/bin/ut
# A little Thylacine field report -- a demo of script execution.
#   fun.ut      (it lives in /bin; runs from anywhere)

echo 'A thylacine field report:'
date

# Count the stripes on the pelt with a for-loop + arithmetic.
let stripes = $(seq 1 16)
let n = 0
for (s in $stripes) {
    let n = (( $n + 1 ))
}
echo "stripes on the pelt: $n"

# Shout the project creed through a pipe (echo | tr).
let creed = $(echo the thylacine is real | tr a-z A-Z)
echo "the creed: $creed"

# Sort the night's quarry by how its name ends, with case.
let quarry = $(echo wallaby potoroo eucalypt spinifex)
for (q in $quarry) {
    case $q {
        *oo => { echo "  hunt:  $q" }
        *by => { echo "  hunt:  $q" }
        *   => { echo "  graze: $q" }
    }
}

echo "the lair is at: $cwd"
echo 'the thylacine yips, and is gone.'
EOF
    chmod 0755 "$ramfs_src/fun.ut"

    # P4-Ia1: copy any built C-side userspace binaries from build/usr
    # into the cpio root. The list is curated below (not glob) so an
    # accidental CMake byproduct doesn't get shipped. Each binary's
    # source-of-truth comment lives in usr/<name>/CMakeLists.txt.
    local usr_bins=( "hello" "joey" "pipe-probe" "attach-probe" "stratumd-stub" "stub-driver" "stub-fs-probe" "stub-walk-probe" "thread-probe" "thread-fault-probe" )
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
    local usr_rs_bins=( "hello-rs" "mmio-probe" "irq-probe" "virtio-blk-probe" "virtio-blk-rw" "virtio-net-probe" "virtio-net-arp" "virtio-net-loop" "netdev-driver" "netd" "warden" "menagerie-probe" "crash-probe" "virtio-mmio-source" "virtio-input" "virtio-gpu" "irq-bench" "corvus" "alloc-smoke" "burrow-torture" "u-test" "u-redir-test" "u-builtin-test" "u-readdir-test" "u-glob-test" "u-subst-test" "u-repl-test" "u-6-test" "u-job-test" "u-7-test" "argv-smoke" "coreutil-smoke" "fs-mut-smoke" "echo" "cat" "wc" "head" "tail" "true" "false" "seq" "sort" "uniq" "tr" "cut" "grep" "ls" "stat" "chmod" "clear" "mkdir" "rmdir" "rm" "touch" "cp" "mv" "tee" "basename" "dirname" "pwd" "sleep" "hexdump" "cmp" "yes" "realpath" "which" "env" "uname" "ns" "id" "whoami" "date" "pipe-src" "pipe-sink" "legate-prover" "login" "ut" "nora" "loom-smoke" "loom-stress" "loom-bench" )
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
    local pouch_bins=( "pouch-hello" "pouch-hello-stdio" "pouch-hello-printf" "pouch-hello-malloc" "pouch-hello-mallocng-torture" "pouch-hello-threads" "pouch-hello-exitgroup" "pouch-hello-poll" "pouch-hello-getrandom" "pouch-hello-sockets" "pouch-hello-signals" "pouch-hello-sodium" "pouch-hello-argv" "pouch-hello-fault" )
    local pouch_progs="$BUILD_DIR/pouch/progs"
    for bin in "${pouch_bins[@]}"; do
        local src="$pouch_progs/$bin"
        if [[ -f "$src" ]]; then
            cp "$src" "$ramfs_src/$bin"
            chmod 0755 "$ramfs_src/$bin"
        fi
    done

    # P6-pouch-stratumd-boot (sub-chunk 16a): copy the cross-built stratumd
    # daemon binary if build_stratumd has produced it. Separate from
    # pouch_bins because stratumd is the real daemon (not a hello test
    # binary) — by sub-chunk 16c it will be spawned by joey + drive the
    # /sysroot mount + ramfs pivot. At 16a it's a "does it actually run
    # in Thylacine" probe.
    local pouch_daemon_bins=( "stratumd" )
    for bin in "${pouch_daemon_bins[@]}"; do
        local src="$pouch_progs/$bin"
        if [[ -f "$src" ]]; then
            cp "$src" "$ramfs_src/$bin"
            chmod 0755 "$ramfs_src/$bin"
        fi
    done

    # P6-pouch-stratumd-boot (sub-chunk 16b-gamma): copy the boot pool
    # keyfile into the ramfs root as /system.key per the K1 initramfs-
    # literal-key boot decision (scripture commit e82e945; v1.0 root
    # placement per 16b-gamma scope reduction — devramfs's flat-cpio
    # constraint defers FHS-shaped /etc/stratum/ to a v1.x lift).
    # joey passes this path to stratumd via argv[--keyfile]. The
    # fixture is generated by build_stratum_pool_fixture before this.
    local keyfile_src="$BUILD_DIR/fixtures/system.key"
    if [[ -f "$keyfile_src" ]]; then
        cp "$keyfile_src" "$ramfs_src/system.key"
        chmod 0400 "$ramfs_src/system.key"
    fi

    python3 "$REPO_ROOT/tools/mkcpio.py" "$ramfs_src" "$ramfs_out"
    echo "==> ramfs cpio: $ramfs_out"
    ledger "ramfs.cpio: REBUILT (bakes the current userspace binaries + system.key)"
}

# A-5c-c: emit the host-baked system recovery phrase as a C header BEFORE the
# userspace build, so joey #includes it for the live RECOVER(system) boot E2E.
# corvus-mint derives the phrase from CORVUS_SYSTEM_RECOVERY_SEED (or its
# default) -- the SAME seed the pool-stage bake uses for system-recovery-wrap,
# so the header phrase opens the baked wrap by construction. Pure derivation (no
# pool/keypair), so it can run here, before the pool bake. Best-effort: a build
# failure is fatal (it would silently drop the E2E), but cargo is required for
# the Rust workspace anyway.
emit_corvus_recovery_header() {
    local cm_manifest="$REPO_ROOT/tools/corvus-mint/Cargo.toml"
    local cm_bin="$REPO_ROOT/tools/corvus-mint/target/release/corvus-mint"
    echo "==> Generating system recovery phrase header (corvus-mint emit-phrase)"
    cargo build --manifest-path "$cm_manifest" --release $verbose \
        || { echo "==> corvus-mint BUILD FAILED (recovery header)" >&2; exit 1; }
    mkdir -p "$GEN_DIR"
    "$cm_bin" emit-phrase "$CORVUS_RECOVERY_HEADER" \
        || { echo "==> corvus-mint emit-phrase FAILED" >&2; exit 1; }
    ledger "system recovery phrase header: GENERATED ($CORVUS_RECOVERY_HEADER; seed=${CORVUS_SYSTEM_RECOVERY_SEED:-corvus-mint default})"
}

build_userspace() {
    # A-5c-c: the generated recovery-phrase header must exist before joey compiles.
    emit_corvus_recovery_header
    # C side — CMake.
    echo "==> Building userspace C (dir=$USR_BUILD, boot_probes=$boot_probes)"
    cmake -S "$REPO_ROOT/usr" -B "$USR_BUILD" \
        -DCMAKE_TOOLCHAIN_FILE="$USR_TOOLCHAIN_FILE" \
        -DCMAKE_BUILD_TYPE="$build_type" \
        -DTHYLA_GENERATED_DIR="$GEN_DIR" \
        -DTHYLA_BOOT_PROBES="$boot_probes" \
        ${extra_cmake_args[@]+"${extra_cmake_args[@]}"}
    cmake --build "$USR_BUILD" $verbose
    echo "==> Userspace C built under $USR_BUILD"
    ledger "userspace: BUILT (native libt C binaries + the Rust workspace below; boot_probes=$boot_probes)"
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
    #
    # POUCH_MALLOCNG_DIAG=1 in the env opts into the 0013-pouch-mallocng-
    # diag.patch's gated diagnostic: a sc/idx/maplen/stride/p+-8 dump
    # fires from enframe BEFORE the mallocng assertion that would
    # otherwise _Exit(127) silently. Off by default (zero perf cost).
    local musl_cflags=""
    if [[ "${POUCH_MALLOCNG_DIAG:-0}" == "1" ]]; then
        musl_cflags="-DPOUCH_MALLOCNG_DIAG"
        echo "==> pouch sysroot: POUCH_MALLOCNG_DIAG=1 (mallocng enframe-assert dump enabled)"
    fi
    echo "==> configuring musl (aarch64-thylacine)"
    ( cd "$musl_obj" && sh "$musl_src/configure" \
        --target=aarch64-thylacine \
        --prefix="$sysroot" \
        --disable-shared \
        CC="$clang --target=aarch64-thylacine" \
        CFLAGS="$musl_cflags" \
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
        # F5 audit close (P6-pouch-threads-b): the seam list MUST be kept in
        # sync with the patch series' actual retargets. Adding a Thylacine
        # number to bits/syscall.h.in without also adding it here would let
        # a typo (e.g. swapped __NR_torpor_wait and __NR_thread_spawn) compile
        # and link without error — the kernel would receive the wrong syscall
        # number and execute the wrong handler. Programs would mysteriously
        # fail. The seam check IS the structural gate against that.
        for seam in 'SYS_read 9' 'SYS_write 10' 'SYS_close 11' 'SYS_exit 0' \
                    'SYS_exit_group 60' 'SYS_mlockall 16' 'SYS_getrandom 20' \
                    'SYS_set_tid_address 36' 'SYS_writev 0xFFFF' \
                    'SYS_socket 0xFFFF' \
                    'SYS_torpor_wait 39' 'SYS_torpor_wake 40' \
                    'SYS_thread_spawn 41' 'SYS_thread_exit 42' \
                    'SYS_walk_create 54' 'SYS_open 65' \
                    'SYS_poll 29' 'SYS_ppoll 0xFFFF' \
                    'SYS_pselect6 0xFFFF' \
                    'SYS_note_open 44' 'SYS_notify 45' 'SYS_noted 46' \
                    'SYS_postnote 47' 'SYS_note_mask 48' \
                    'SYS_rt_sigaction 0xFFFF' 'SYS_rt_sigprocmask 0xFFFF' \
                    'SYS_tkill 0xFFFF' 'SYS_kill 0xFFFF' \
                    'SYS_rt_sigreturn 0xFFFF' \
                    'SYS_mmap 37' 'SYS_munmap 38' \
                    'SYS_srv_accept 27' 'SYS_srv_peer 28' \
                    'SYS_mmio_create 2' 'SYS_irq_create 3' 'SYS_irq_wait 4' \
                    'SYS_mmio_map 5' 'SYS_dma_create 6' 'SYS_dma_map 7' \
                    'SYS_walk_open 34' \
                    'SYS_fstat 50' 'SYS_lseek 51'; do
            grep -q "^#define $seam\$" "$syscall_h" || {
                echo "    SEAM: '#define $seam' missing from bits/syscall.h" >&2
                fail=1
            }
        done
        # fstat/lseek are #undef'd from their 0xFFFF base then redefined to
        # 50/51 by 0010. The value-grep above proves the 50/51 define exists,
        # but both defines coexist in the header; require the #undef so a
        # re-vendor that drops it (leaving 0xFFFF to win) fails here, not at
        # runtime with a silent ENOSYS on the stratumd keyfile path.
        local undef
        for undef in SYS_fstat SYS_lseek; do
            grep -qE "^#undef[[:space:]]+$undef\$" "$syscall_h" || {
                echo "    SEAM: '#undef $undef' (0010 retarget) missing from bits/syscall.h" >&2
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

    # 6. build the compiler runtime — the compiler-rt builtins. A complete C
    #    cross-toolchain is compiler + libc + CRT + compiler runtime; this
    #    installs libclang_rt.builtins.a alongside libc.a and the CRT objects.
    build_compiler_rt

    # 7. build libsodium (sub-chunk 14) — the first cross-compiled C library
    #    against pouch. Installs libsodium.a + headers alongside libc.a. The
    #    libsodium build needs the libc + CRT + runtime that steps 1-6 install,
    #    so it always runs after them.
    build_libsodium

    echo "==> pouch sysroot ready:"
    echo "    libc.a    $(wc -c < "$sysroot/lib/libc.a" | tr -d ' ') bytes"
    echo "    runtime   libclang_rt.builtins.a $(wc -c < "$sysroot/lib/libclang_rt.builtins.a" | tr -d ' ') bytes"
    echo "    libsodium $(wc -c < "$sysroot/lib/libsodium.a" | tr -d ' ') bytes"
    echo "    CRT       crt1.o crti.o crtn.o"
    echo "    headers   $(find "$sysroot/include" -name '*.h' | wc -l | tr -d ' ') files"
    echo "    seam      syscall table retargeted to the Thylacine ABI"
    ledger "sysroot: REBUILT (pristine musl + pouch patch series + libc.a + compiler-rt + libsodium)"
}

build_compiler_rt() {
    # Phase 6 (Pouch) — build the compiler runtime: the compiler-rt builtins,
    # installed as build/sysroot/lib/libclang_rt.builtins.a.
    #
    # A complete C cross-toolchain is four parts — compiler + libc + CRT +
    # *compiler runtime*. The runtime supplies the low-level support routines
    # clang emits calls to when the target ISA lacks an operation: aarch64 has
    # no hardware binary128 float, and `long double` on aarch64 IS binary128,
    # so printf's vfprintf path references soft-float builtins (__addtf3,
    # __eqtf2, __extenddftf2, ...). Homebrew LLVM ships compiler-rt for the
    # Darwin host only, so pouch builds its own for aarch64-thylacine.
    #
    # The builtins source is vendored pristine at third_party/compiler-rt/ —
    # no patch series (compiler-rt is pure computation; nothing to retarget).
    # The aarch64 source set is the GENERIC_SOURCES + GENERIC_TF_SOURCES +
    # BF16_SOURCES blocks of the vendored CMakeLists (extracted, not
    # transcribed — re-vendor-safe; every file in them is compiled for aarch64
    # by compiler-rt's own build, so it is upstream-guaranteed to compile
    # cleanly), plus the conditional generic additions and the two
    # aarch64-specific files. Deliberately excluded:
    #   - generic fp_mode.c — superseded by aarch64/fp_mode.c, the FPCR-reading
    #     arch override (compiler-rt's filter_builtin_sources drops it likewise).
    #   - aarch64/lse.S outline atomics — pouch-clang compiles every TU with
    #     -march=...+lse, so clang emits LSE atomic instructions inline and the
    #     __aarch64_{cas,swp,ldadd,...} outline helpers are never called.
    #   - aarch64/emupac.cpp + the SME files — PAC emulation / SME ABI support,
    #     referenced only by code built with -mbranch-protection or +sme, which
    #     pouch's -march baseline does not enable.
    local sysroot="$BUILD_DIR/sysroot"
    local crt_src="$REPO_ROOT/third_party/compiler-rt/builtins"
    local crt_obj="$BUILD_DIR/pouch/compiler-rt-obj"
    local clang="$LLVM_PREFIX/bin/clang"
    local archive="$sysroot/lib/libclang_rt.builtins.a"

    if [[ ! -f "$crt_src/CMakeLists.txt" ]]; then
        echo "==> compiler-rt: vendored source missing at $crt_src" >&2
        exit 1
    fi

    echo "==> building compiler-rt builtins (aarch64-thylacine)"
    rm -rf "$crt_obj"
    mkdir -p "$crt_obj"

    # The aarch64 source list, derived from the vendored CMakeLists. Generic
    # fp_mode.c is dropped — aarch64/fp_mode.c supersedes it.
    local sources=()
    local f
    while IFS= read -r f; do
        if [[ "$f" == "fp_mode.c" ]]; then continue; fi
        sources+=( "$f" )
    done < <(awk '/^set\((GENERIC_SOURCES|GENERIC_TF_SOURCES|BF16_SOURCES)$/{c=1;next} c&&/^\)/{c=0;next} c&&$1~/\.c$/{print $1}' "$crt_src/CMakeLists.txt")
    # Conditional generic additions (CMakeLists: non-Fuchsia / non-baremetal),
    # then the two aarch64-specific files.
    sources+=( emutls.c enable_execute_stack.c eprintf.c gcc_personality_v0.c clear_cache.c )
    sources+=( cpu_model/aarch64.c aarch64/fp_mode.c )

    if [[ "${#sources[@]}" -lt 150 ]]; then
        echo "    compiler-rt: source list too short (${#sources[@]}) — CMakeLists parse failed" >&2
        exit 1
    fi

    # Compile flags. -fno-builtin: a builtin must never be lowered into a call
    # to itself. -fno-pic / -fno-stack-protector: pouch links static non-PIE,
    # and the builtins are leaf runtime routines. -nostdlibinc keeps clang's
    # resource headers (stdint.h / limits.h / stdarg.h / unwind.h — all
    # compiler-provided) while -isystem supplies pouch's libc headers for the
    # few OS-touching files (emutls.c, enable_execute_stack.c, ...).
    local cflags=( --target=aarch64-thylacine -march=armv8-a+lse+pauth+bti
                   -std=gnu11 -O2 -fno-builtin -fomit-frame-pointer
                   -fno-stack-protector -fno-pic
                   -nostdlibinc -isystem "$sysroot/include" -I"$crt_src" )

    local n=0 base obj
    for f in "${sources[@]}"; do
        if [[ ! -f "$crt_src/$f" ]]; then
            echo "    compiler-rt: source $f missing from the vendored tree" >&2
            exit 1
        fi
        base="${f%.c}"
        obj="$crt_obj/${base//\//-}.o"
        "$clang" "${cflags[@]}" -c "$crt_src/$f" -o "$obj"
        n=$((n + 1))
    done
    echo "    compiled $n objects"

    # Archive + symbol index.
    mkdir -p "$sysroot/lib"
    rm -f "$archive"
    "$LLVM_PREFIX/bin/llvm-ar" rcs "$archive" "$crt_obj"/*.o

    # Verify the binary128 soft-float builtins printf's vfprintf path needs are
    # defined in the archive — the toolchain-completeness gate. A sysroot with a
    # libc but no runtime links a static hello, but not printf.
    local defined sym fail=0
    defined="$("$LLVM_PREFIX/bin/llvm-nm" --defined-only "$archive" 2>/dev/null)"$'\n'
    for sym in __addtf3 __subtf3 __multf3 __divtf3 __eqtf2 __extenddftf2 \
               __trunctfdf2 __fixtfdi __floatditf; do
        case "$defined" in
            *" T $sym"$'\n'*) ;;
            *) echo "    compiler-rt: builtin $sym not defined in the archive" >&2
               fail=1 ;;
        esac
    done
    if [[ "$fail" -ne 0 ]]; then
        echo "==> compiler-rt FAILED verification" >&2
        exit 1
    fi
}

build_libsodium() {
    # Phase 6 (Pouch) sub-chunk 14 — build libsodium for aarch64-thylacine and
    # install it into the pouch sysroot as libsodium.a + headers. Per
    # POUCH-DESIGN.md §14 this proves the cross-toolchain by cross-compiling
    # a non-trivial real C library against pouch; the proving binary
    # /pouch-hello-sodium then runs a KAT round-trip in Thylacine.
    #
    # libsodium is portable C with autoconf-generated config — pouch has no
    # patch series for it (the OS-touching parts are getentropy(3) and
    # getrandom(2), already wired by sub-chunks 4 + 11). build_libsodium
    # supplies the HAVE_* macros and the version.h that ./configure would
    # normally generate, without running ./configure (autoconf cross-compile
    # on macOS for an unknown OS triple is fragile + slow). Source compiled
    # directly with pouch-clang into build/pouch/libsodium-obj/, archived
    # into build/sysroot/lib/libsodium.a, headers installed into
    # build/sysroot/include/. See docs/reference/84-pouch-libsodium.md.
    local sysroot="$BUILD_DIR/sysroot"
    local sodium_src="$REPO_ROOT/third_party/libsodium/src/libsodium"
    local sodium_obj="$BUILD_DIR/pouch/libsodium-obj"
    local clang="$LLVM_PREFIX/bin/clang"
    local archive="$sysroot/lib/libsodium.a"

    if [[ ! -f "$sodium_src/Makefile.am" ]]; then
        echo "==> libsodium: vendored source missing at $sodium_src" >&2
        exit 1
    fi

    echo "==> building libsodium 1.0.20 (aarch64-thylacine)"
    rm -rf "$sodium_obj"
    mkdir -p "$sodium_obj" "$sodium_obj/gen/sodium"

    # 1. Generate sodium/version.h from version.h.in. ./configure would
    #    AC_SUBST these four placeholders; pouch substitutes them by hand.
    #    SODIUM_LIBRARY_MINIMAL_DEF is empty for a non-MINIMAL build (we want
    #    the full API surface). The generated header is placed FIRST on the
    #    include path so it shadows the vendored .in.
    sed -e 's/@VERSION@/1.0.20/g' \
        -e 's/@SODIUM_LIBRARY_VERSION_MAJOR@/26/g' \
        -e 's/@SODIUM_LIBRARY_VERSION_MINOR@/2/g' \
        -e 's|@SODIUM_LIBRARY_MINIMAL_DEF@||g' \
        "$sodium_src/include/sodium/version.h.in" \
        > "$sodium_obj/gen/sodium/version.h"

    # 2. The compose-list. Curated from src/libsodium/Makefile.am
    #    (libsodium_la_SOURCES base + !HAVE_AMD64_ASM ref-salsa20 +
    #    !EMSCRIPTEN randombytes + !MINIMAL non-minimal arm).
    #    Excluded: x86 ASM (AESNI, SSE, AVX, sandy2x); ARMv8 crypto extension
    #    sources (libarmcrypto_la — needs +crypto march, pouch baseline is
    #    -march=armv8-a+lse+pauth+bti). The portable refs implement every
    #    primitive; performance is fine for a proving binary.
    local sources=(
        crypto_aead/aegis128l/aead_aegis128l.c
        crypto_aead/aegis128l/aegis128l_soft.c
        crypto_aead/aegis256/aead_aegis256.c
        crypto_aead/aegis256/aegis256_soft.c
        crypto_aead/aes256gcm/aead_aes256gcm.c
        crypto_aead/chacha20poly1305/aead_chacha20poly1305.c
        crypto_aead/xchacha20poly1305/aead_xchacha20poly1305.c
        crypto_auth/crypto_auth.c
        crypto_auth/hmacsha256/auth_hmacsha256.c
        crypto_auth/hmacsha512/auth_hmacsha512.c
        crypto_auth/hmacsha512256/auth_hmacsha512256.c
        crypto_box/crypto_box.c
        crypto_box/crypto_box_easy.c
        crypto_box/crypto_box_seal.c
        crypto_box/curve25519xchacha20poly1305/box_curve25519xchacha20poly1305.c
        crypto_box/curve25519xchacha20poly1305/box_seal_curve25519xchacha20poly1305.c
        crypto_box/curve25519xsalsa20poly1305/box_curve25519xsalsa20poly1305.c
        crypto_core/ed25519/core_ed25519.c
        crypto_core/ed25519/core_ristretto255.c
        crypto_core/ed25519/ref10/ed25519_ref10.c
        crypto_core/hchacha20/core_hchacha20.c
        crypto_core/hsalsa20/core_hsalsa20.c
        crypto_core/hsalsa20/ref2/core_hsalsa20_ref2.c
        crypto_core/salsa/ref/core_salsa_ref.c
        crypto_core/softaes/softaes.c
        crypto_generichash/blake2b/generichash_blake2.c
        crypto_generichash/blake2b/ref/blake2b-compress-ref.c
        crypto_generichash/blake2b/ref/blake2b-ref.c
        crypto_generichash/blake2b/ref/generichash_blake2b.c
        crypto_generichash/crypto_generichash.c
        crypto_hash/crypto_hash.c
        crypto_hash/sha256/cp/hash_sha256_cp.c
        crypto_hash/sha256/hash_sha256.c
        crypto_hash/sha512/cp/hash_sha512_cp.c
        crypto_hash/sha512/hash_sha512.c
        crypto_kdf/blake2b/kdf_blake2b.c
        crypto_kdf/crypto_kdf.c
        crypto_kdf/hkdf/kdf_hkdf_sha256.c
        crypto_kdf/hkdf/kdf_hkdf_sha512.c
        crypto_kx/crypto_kx.c
        crypto_onetimeauth/crypto_onetimeauth.c
        crypto_onetimeauth/poly1305/donna/poly1305_donna.c
        crypto_onetimeauth/poly1305/onetimeauth_poly1305.c
        crypto_pwhash/argon2/argon2-core.c
        crypto_pwhash/argon2/argon2-encoding.c
        crypto_pwhash/argon2/argon2-fill-block-ref.c
        crypto_pwhash/argon2/argon2.c
        crypto_pwhash/argon2/blake2b-long.c
        crypto_pwhash/argon2/pwhash_argon2i.c
        crypto_pwhash/argon2/pwhash_argon2id.c
        crypto_pwhash/crypto_pwhash.c
        crypto_pwhash/scryptsalsa208sha256/crypto_scrypt-common.c
        crypto_pwhash/scryptsalsa208sha256/nosse/pwhash_scryptsalsa208sha256_nosse.c
        crypto_pwhash/scryptsalsa208sha256/pbkdf2-sha256.c
        crypto_pwhash/scryptsalsa208sha256/pwhash_scryptsalsa208sha256.c
        crypto_pwhash/scryptsalsa208sha256/scrypt_platform.c
        crypto_scalarmult/crypto_scalarmult.c
        crypto_scalarmult/curve25519/ref10/x25519_ref10.c
        crypto_scalarmult/curve25519/scalarmult_curve25519.c
        crypto_scalarmult/ed25519/ref10/scalarmult_ed25519_ref10.c
        crypto_scalarmult/ristretto255/ref10/scalarmult_ristretto255_ref10.c
        crypto_secretbox/crypto_secretbox.c
        crypto_secretbox/crypto_secretbox_easy.c
        crypto_secretbox/xchacha20poly1305/secretbox_xchacha20poly1305.c
        crypto_secretbox/xsalsa20poly1305/secretbox_xsalsa20poly1305.c
        crypto_secretstream/xchacha20poly1305/secretstream_xchacha20poly1305.c
        crypto_shorthash/crypto_shorthash.c
        crypto_shorthash/siphash24/ref/shorthash_siphash24_ref.c
        crypto_shorthash/siphash24/ref/shorthash_siphashx24_ref.c
        crypto_shorthash/siphash24/shorthash_siphash24.c
        crypto_shorthash/siphash24/shorthash_siphashx24.c
        crypto_sign/crypto_sign.c
        crypto_sign/ed25519/ref10/keypair.c
        crypto_sign/ed25519/ref10/obsolete.c
        crypto_sign/ed25519/ref10/open.c
        crypto_sign/ed25519/ref10/sign.c
        crypto_sign/ed25519/sign_ed25519.c
        crypto_stream/chacha20/ref/chacha20_ref.c
        crypto_stream/chacha20/stream_chacha20.c
        crypto_stream/crypto_stream.c
        crypto_stream/salsa20/ref/salsa20_ref.c
        crypto_stream/salsa20/stream_salsa20.c
        crypto_stream/salsa2012/ref/stream_salsa2012_ref.c
        crypto_stream/salsa2012/stream_salsa2012.c
        crypto_stream/salsa208/ref/stream_salsa208_ref.c
        crypto_stream/salsa208/stream_salsa208.c
        crypto_stream/xchacha20/stream_xchacha20.c
        crypto_stream/xsalsa20/stream_xsalsa20.c
        crypto_verify/verify.c
        randombytes/internal/randombytes_internal_random.c
        randombytes/randombytes.c
        randombytes/sysrandom/randombytes_sysrandom.c
        sodium/codecs.c
        sodium/core.c
        sodium/runtime.c
        sodium/utils.c
        sodium/version.c
    )

    if [[ "${#sources[@]}" -lt 90 ]]; then
        echo "    libsodium: source list too short (${#sources[@]}) — list out of sync?" >&2
        exit 1
    fi

    # 3. Compile flags. The HAVE_* set mirrors what ./configure would AC_DEFINE
    #    for aarch64-thylacine: musl-style libc (1.2.5), clang 22.1, no x86
    #    ASM, no ARMv8 crypto extension, pthreads available. Notes on the
    #    POUCH-specific choices:
    #      - HAVE_MPROTECT / HAVE_MLOCK / HAVE_MADVISE NOT defined — pouch's
    #        sentinel returns ENOSYS for these; libsodium's sodium_mlock would
    #        try to call them and silently return -1, which is benign but
    #        defining them would mislead libsodium consumers about what it
    #        actually does.
    #      - HAVE_NANOSLEEP / HAVE_CLOCK_GETTIME NOT defined — pouch's sentinel
    #        returns ENOSYS for these too; libsodium uses them only on retry
    #        paths and busy-waits adequately without them at v1.0.
    #      - HAVE_GETPID defined — getpid() in pouch returns -1 (sentinel
    #        ENOSYS); libsodium uses it only for fork-detection in the
    #        internal_random path (not the default sysrandom path), where the
    #        -1 value harmlessly never matches a "new" pid.
    #      - HAVE_GETRANDOM + HAVE_GETENTROPY defined — sub-chunks 4 + 11
    #        wired both. The randombytes_sysrandom path picks the getentropy
    #        branch first; both reach the kernel through SYS_GETRANDOM.
    #      - HAVE_CATCHABLE_ABRT / HAVE_CATCHABLE_SEGV NOT defined — pouch's
    #        sigaction surface doesn't support SIGABRT / SIGSEGV (v1.0); the
    #        macros are unused by libsodium 1.0.20 source anyway.
    #      - CONFIGURED=1 — kills the "compiled by an undocumented method"
    #        warning in private/common.h that fires when ./configure didn't
    #        emit a config.h.
    #      - SODIUM_STATIC — expands SODIUM_EXPORT to nothing (no
    #        visibility attribute needed for a static-archive build).
    local cflags=( --target=aarch64-thylacine -march=armv8-a+lse+pauth+bti
                   -std=gnu11 -O2 -fno-pic -fomit-frame-pointer
                   -fno-stack-protector
                   -nostdinc -isystem "$sysroot/include"
                   -I"$sodium_obj/gen/sodium"
                   -I"$sodium_src/include"
                   -I"$sodium_src/include/sodium"
                   -D_GNU_SOURCE=1
                   -DCONFIGURED=1
                   -DSODIUM_STATIC=1
                   -DNATIVE_LITTLE_ENDIAN=1
                   -DHAVE_TI_MODE=1
                   -DHAVE_C_VARARRAYS=1
                   -DHAVE_INTTYPES_H=1
                   -DHAVE_STDINT_H=1
                   -DHAVE_SYS_MMAN_H=1
                   -DHAVE_SYS_PARAM_H=1
                   -DHAVE_SYS_RANDOM_H=1
                   -DHAVE_SYS_AUXV_H=1
                   -DHAVE_PTHREAD=1
                   -DHAVE_WEAK_SYMBOLS=1
                   -DHAVE_C11_MEMORY_FENCES=1
                   -DHAVE_GCC_MEMORY_FENCES=1
                   -DHAVE_ATOMIC_OPS=1
                   -DHAVE_INLINE_ASM=1
                   -DHAVE_MMAP=1
                   -DHAVE_RAISE=1
                   -DHAVE_SYSCONF=1
                   -DHAVE_GETRANDOM=1
                   -DHAVE_GETENTROPY=1
                   -DHAVE_LINUX_COMPATIBLE_GETRANDOM=1
                   -DHAVE_GETPID=1
                   -DHAVE_GETAUXVAL=1
                   -DHAVE_POSIX_MEMALIGN=1
                   -DHAVE_EXPLICIT_BZERO=1
                   -Wno-unknown-pragmas -Wno-unused-function )

    local n=0 base obj
    for f in "${sources[@]}"; do
        if [[ ! -f "$sodium_src/$f" ]]; then
            echo "    libsodium: source $f missing from the vendored tree" >&2
            exit 1
        fi
        base="${f%.c}"
        obj="$sodium_obj/${base//\//-}.o"
        "$clang" "${cflags[@]}" -c "$sodium_src/$f" -o "$obj"
        n=$((n + 1))
    done
    echo "    compiled $n objects"

    # 4. Archive + symbol index.
    rm -f "$archive"
    "$LLVM_PREFIX/bin/llvm-ar" rcs "$archive" "$sodium_obj"/*.o

    # 5. Install the public headers into the sysroot. sodium.h is the umbrella
    #    header; sodium/*.h is the per-primitive subset; the generated
    #    sodium/version.h is installed from $sodium_obj/gen.
    mkdir -p "$sysroot/include/sodium"
    cp "$sodium_src/include/sodium.h" "$sysroot/include/sodium.h"
    local h
    for h in "$sodium_src/include/sodium/"*.h; do
        cp "$h" "$sysroot/include/sodium/$(basename "$h")"
    done
    cp "$sodium_obj/gen/sodium/version.h" "$sysroot/include/sodium/version.h"

    # 6. Verify the archive contains the symbols pouch-hello-sodium will use.
    #    These pin the cross-build's correctness: a missing primary symbol
    #    would surface a vendor / source-list drift before /pouch-hello-sodium
    #    even links.
    local defined sym fail=0
    defined="$("$LLVM_PREFIX/bin/llvm-nm" --defined-only "$archive" 2>/dev/null)"$'\n'
    for sym in sodium_init randombytes_buf \
               crypto_aead_xchacha20poly1305_ietf_encrypt \
               crypto_aead_xchacha20poly1305_ietf_decrypt \
               crypto_hash_sha256 \
               crypto_generichash \
               crypto_sign_keypair crypto_sign_detached crypto_sign_verify_detached \
               sodium_version_string; do
        case "$defined" in
            *" T $sym"$'\n'*) ;;
            *) echo "    libsodium: symbol $sym not defined in the archive" >&2
               fail=1 ;;
        esac
    done
    if [[ "$fail" -ne 0 ]]; then
        echo "==> libsodium FAILED verification" >&2
        exit 1
    fi
}

build_stratumd() {
    # Phase 6 (Pouch) sub-chunk 15 — cross-compile stratumd against the
    # pouch sysroot. The first real non-trivial daemon ported via pouch;
    # exercises stratumd's POSIX + GNU surface (threads, sockets, file I/O,
    # signals) on Thylacine's musl-based libc. Per POUCH-DESIGN.md section 14
    # row 15 + section 10 (the per-OS arms pattern): stratumd stays ~99%
    # portable POSIX with a thin Thylacine arm in peer_creds.c (the new
    # `__thylacine__` branch landed on the Stratum side).
    #
    # Source: $STRATUM_SRC (defaults to ~/projects/stratum/v2). Build dir:
    # $BUILD_DIR/pouch/stratumd-cmake. Output binary: $BUILD_DIR/pouch/
    # stratumd-cmake/src/cmd/stratumd/stratumd, copied alongside the pouch
    # hello binaries under $BUILD_DIR/pouch/progs/ for the ramfs step.
    #
    # Configure flags:
    #   -DCMAKE_TOOLCHAIN_FILE     pouch cross-toolchain (defines
    #                              __thylacine__, _GNU_SOURCE, drives pouch-ld).
    #   -DSTM_ENABLE_PQ=OFF        liboqs not cross-compiled for pouch at v1.0.
    #   -DSTM_ENABLE_IOURING=OFF   io_uring is Linux-only; explicit-off
    #                              avoids the pkg_check_modules probe.
    #   -DSTM_ENABLE_LIBAIO=OFF    libaio likewise Linux-only.
    #   -DSTM_BUILD_TESTS=OFF      tests need a host harness that doesn't exist
    #                              on Thylacine; the cross build only ships the
    #                              stratumd binary.
    #   -DSTM_BUILD_FUZZERS=OFF    libFuzzer not cross-compiled.
    #   -DSTM_WERROR=OFF           pouch's clang flags + musl headers surface
    #                              warnings the upstream Stratum tree hasn't
    #                              hit; cross-compile is best-effort on
    #                              warning hygiene at v1.0.
    #
    # The Stratum source is consumed READ-ONLY from $STRATUM_SRC; no in-tree
    # changes are written. The Stratum-side `__thylacine__` arm in
    # peer_creds.c + the STM_PLATFORM_THYLACINE detection in CMakeLists.txt
    # live on the `thylacine-pouch-arm` branch in the Stratum repo (a
    # coordination artifact records the integration; see Stratum's
    # docs/session-handoff-2026-05-24-thylacine-pouch-arm.md).
    local sysroot="$BUILD_DIR/sysroot"
    local stratum_src="${STRATUM_SRC:-$HOME/projects/stratum/v2}"
    local stratumd_build="$BUILD_DIR/pouch/stratumd-cmake"
    local progs_out="$BUILD_DIR/pouch/progs"
    local toolchain="$REPO_ROOT/cmake/Toolchain-aarch64-pouch.cmake"
    local readelf="$LLVM_PREFIX/bin/llvm-readelf"

    if [[ ! -d "$stratum_src" ]]; then
        echo "==> stratumd: source tree not found at $stratum_src" >&2
        echo "    override with STRATUM_SRC=/path/to/stratum/v2" >&2
        exit 1
    fi
    if [[ ! -f "$stratum_src/CMakeLists.txt" ]]; then
        echo "==> stratumd: $stratum_src/CMakeLists.txt missing" >&2
        exit 1
    fi
    if [[ ! -f "$sysroot/lib/libc.a" || ! -f "$sysroot/lib/libsodium.a" ]]; then
        echo "==> stratumd: sysroot incomplete (libc.a or libsodium.a missing)" >&2
        echo "    run 'tools/build.sh sysroot' first" >&2
        exit 1
    fi

    echo "==> building stratumd (aarch64-thylacine) from $stratum_src"
    # Incremental: keep $stratumd_build across runs so CMake's cache +
    # ninja/make's dep tracking skip unchanged objects. The first build
    # is ~2-3 minutes; incremental on a no-source-change rebuild is <5s.
    mkdir -p "$stratumd_build" "$progs_out"

    # Configure with the pouch toolchain. CMake's tool-probes (compiler,
    # threads) are short-circuited by the toolchain file's
    # CMAKE_C_COMPILER_WORKS + the synthesized Threads::/PkgConfig::LIBSODIUM
    # IMPORTED targets in Stratum's CMakeLists.txt's STM_PLATFORM_THYLACINE
    # branch.
    cmake -S "$stratum_src" -B "$stratumd_build" \
        -DCMAKE_TOOLCHAIN_FILE="$toolchain" \
        -DCMAKE_BUILD_TYPE=Release \
        -DSTM_ENABLE_PQ=OFF \
        -DSTM_ENABLE_IOURING=OFF \
        -DSTM_ENABLE_LIBAIO=OFF \
        -DSTM_BUILD_TESTS=OFF \
        -DSTM_BUILD_FUZZERS=OFF \
        -DSTM_WERROR=OFF \
        -DSTRATUM_BUILD_TESTING_HOOKS=OFF

    cmake --build "$stratumd_build" --target stratumd -j"$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"

    local binary="$stratumd_build/src/cmd/stratumd/stratumd"
    if [[ ! -f "$binary" ]]; then
        echo "==> stratumd: build did not produce $binary" >&2
        exit 1
    fi

    # Verify the layout the kernel ELF loader requires: ET_EXEC, no
    # PT_DYNAMIC. Matches build_pouch_progs's verification.
    local elf_hdr elf_phdrs
    elf_hdr="$("$readelf" -h "$binary")"
    elf_phdrs="$("$readelf" -l "$binary")"
    case "$elf_hdr" in
        *"Type:"*EXEC*) ;;
        *) echo "    stratumd: not ET_EXEC — kernel/elf.c would reject it" >&2
           exit 1 ;;
    esac
    case "$elf_phdrs" in
        *DYNAMIC*) echo "    stratumd: has PT_DYNAMIC — kernel/elf.c would reject it" >&2
                   exit 1 ;;
    esac

    cp "$binary" "$progs_out/stratumd"
    echo "==> stratumd built: $progs_out/stratumd ($(wc -c < "$progs_out/stratumd" | tr -d ' ') bytes, ET_EXEC, static)"
    ledger "stratumd: BUILT (links the pouch libc -- a stale sysroot would ship a stale ABI here)"
}

build_stratum_host_tools() {
    # Phase 6 (Pouch) sub-chunk 16b-beta + 16c. Native host build of the
    # three Stratum CLI tools we drive at build time:
    #   stratum-mkfs   — formats a fresh pool (16b-beta)
    #   stratumd       — runs as a transient 9P server during populate (16c)
    #   stratum-fs     — 9P CLI client used to copy boot corpus into pool (16c)
    # These are NOT the pouch cross-builds (those ship in the ramfs); this
    # builds the host-native binaries that tools/build.sh orchestrates to
    # produce a pre-populated pool.img before QEMU boots.
    #
    # Build dir: $BUILD_DIR/host-stratum. Configured for the host's native
    # platform (Darwin on the dev box; Linux for CI). Disables PQ + io_uring
    # + libaio + tests + fuzzers to keep configuration fast (~10s) and
    # the build small (~30s cold for all three targets).
    local stratum_src="${STRATUM_SRC:-$HOME/projects/stratum/v2}"
    local host_build="$BUILD_DIR/host-stratum"
    local mkfs_bin="$host_build/src/cmd/stratum-mkfs/stratum-mkfs"
    local stratumd_bin="$host_build/src/cmd/stratumd/stratumd"
    local stratum_fs_bin="$host_build/src/cmd/stratum-fs/stratum-fs"

    if [[ ! -d "$stratum_src" ]]; then
        echo "==> stratum host tools: source tree not found at $stratum_src" >&2
        echo "    override with STRATUM_SRC=/path/to/stratum/v2" >&2
        exit 1
    fi

    echo "==> building stratum host tools (mkfs + stratumd + stratum-fs) from $stratum_src"
    mkdir -p "$host_build"
    # No --toolchain — use the host's default compiler. The host tools
    # run on Darwin/Linux x86_64/arm64, not on Thylacine.
    cmake -S "$stratum_src" -B "$host_build" \
        -DCMAKE_BUILD_TYPE=Release \
        -DSTM_ENABLE_PQ=OFF \
        -DSTM_ENABLE_IOURING=OFF \
        -DSTM_ENABLE_LIBAIO=OFF \
        -DSTM_BUILD_TESTS=OFF \
        -DSTM_BUILD_FUZZERS=OFF \
        -DSTM_WERROR=OFF \
        -DSTRATUM_BUILD_TESTING_HOOKS=OFF \
        > /dev/null

    cmake --build "$host_build" \
        --target stratum-mkfs stratumd stratum-fs \
        -j"$(sysctl -n hw.ncpu 2>/dev/null || echo 4)" > /dev/null

    local missing=""
    [[ -f "$mkfs_bin" ]]       || missing="$missing stratum-mkfs"
    [[ -f "$stratumd_bin" ]]   || missing="$missing stratumd"
    [[ -f "$stratum_fs_bin" ]] || missing="$missing stratum-fs"
    if [[ -n "$missing" ]]; then
        echo "==> stratum host tools: build did not produce:$missing" >&2
        exit 1
    fi
    echo "==> stratum host tools built: $mkfs_bin, $stratumd_bin, $stratum_fs_bin"
}

# Backward-compat alias: any caller using the old name still works.
build_stratum_mkfs_host() { build_stratum_host_tools "$@"; }

build_stratum_pool_fixture() {
    # Phase 6 (Pouch) sub-chunk 16b-beta. Generates the boot system pool
    # fixture that QEMU mounts as a second virtio-blk-device at boot. The
    # fixture lives at $BUILD_DIR/fixtures/{pool.img,system.key}; the
    # pool.img is fed to QEMU verbatim via -drive (file=...,format=raw),
    # the keyfile is copied into the ramfs at /etc/stratum/system.key
    # (per the K1 initramfs-literal-key boot decision; sub-chunk 16b-design
    # commit e82e945).
    #
    # We regenerate the fixture each time tools/build.sh kernel runs IF
    # it's missing OR if stratum-mkfs has been rebuilt newer than the
    # fixture. Stable across reruns when nothing's changed (the pool's
    # contents are deterministic modulo the UUID seed which stratum-mkfs
    # derives from time + pid — but the boot-side mount doesn't depend
    # on the UUID, so regenerating across builds is harmless).
    #
    # Size: 64 MiB. Matches stratum-mkfs's DEFAULT_DEVICE_BYTES, large
    # enough for the bootstrap pool + a single root inode with comfortable
    # headroom for tiny FS test writes.
    local stratum_src="${STRATUM_SRC:-$HOME/projects/stratum/v2}"
    local host_build="$BUILD_DIR/host-stratum"
    local mkfs_bin="$host_build/src/cmd/stratum-mkfs/stratum-mkfs"
    local fixtures="$BUILD_DIR/fixtures"
    local pool_img="$fixtures/pool.img"
    local keyfile="$fixtures/system.key"

    # 16c: also need stratumd + stratum-fs for the populate step at the
    # bottom. Rebuild all three host tools if any is missing OR if a Stratum
    # source file is newer than the built stratumd (stratum_host_tools_stale).
    local host_stratumd="$host_build/src/cmd/stratumd/stratumd"
    local host_stratum_fs="$host_build/src/cmd/stratum-fs/stratum-fs"
    if stratum_host_tools_stale; then
        build_stratum_host_tools
    fi

    mkdir -p "$fixtures"

    # PRESERVE mode: if THYLACINE_MKFS_PRESERVE=1 AND both files
    # already exist, skip regeneration entirely. The bytes from the
    # prior build are reused verbatim. This is the only way to get
    # FULL byte-identical pool content across builds (--seed alone
    # pins UUIDs but not the libsodium-randomized keyfile + per-run
    # nonces). Use case: after a build that triggers a content-
    # sensitive bug, save pool.img + system.key and re-run with
    # PRESERVE=1 to get byte-perfect reproduction.
    if [[ "${THYLACINE_MKFS_PRESERVE:-0}" == "1" ]] \
        && [[ -f "$pool_img" ]] && [[ -f "$keyfile" ]]; then
        echo "==> stratum pool fixture: PRESERVED (THYLACINE_MKFS_PRESERVE=1; $(wc -c < "$pool_img" | tr -d ' ') bytes pool.img, $(wc -c < "$keyfile" | tr -d ' ') bytes system.key from prior build)"
        ledger "pool.img + system.key: PRESERVED (THYLACINE_MKFS_PRESERVE=1; the existing key is kept -- rebuild the ramfs to re-bake it)"
        return 0
    fi

    # SEED mode: if THYLACINE_MKFS_SEED is set, pass it through to
    # stratum-mkfs's --seed flag. Pins UUID derivation across runs
    # (but does NOT pin the full pool bytes -- see PRESERVE above
    # for that). Auto-generate a fresh seed if not set, so EVERY
    # build records its seed in the log -- if any build triggers a
    # bug, the log captures the seed for forensic re-runs.
    local mkfs_seed="${THYLACINE_MKFS_SEED:-}"
    if [[ -z "$mkfs_seed" ]]; then
        # Auto-generate: 16 random hex chars from /dev/urandom. Stays
        # within stratum-mkfs's parse_hex64 bound.
        mkfs_seed="0x$(od -An -N8 -tx8 /dev/urandom | tr -d ' \n')"
    fi
    echo "==> stratum pool fixture: seed=$mkfs_seed (set THYLACINE_MKFS_SEED=<hex64> to pin, THYLACINE_MKFS_PRESERVE=1 to reuse existing files)"

    # Reproducible regenerate: blow away existing files so stratum-mkfs
    # doesn't reuse the previous keyfile (which would otherwise mismatch
    # if the pool size or schema changed across builds).
    rm -f "$pool_img" "$keyfile"

    # A-3: PRINCIPAL_SYSTEM == GID_SYSTEM == 4294967294 == (u32)-2. The root
    # inode (mkfs --root-uid/-gid) AND every baked file (stratumd
    # --bake-owner-uid below) are stamped SYSTEM-owned, so the boot chain
    # (PRINCIPAL_SYSTEM) owns the whole baked tree once the OS enforces dev9p
    # rwx (A-3b) -- otherwise joey-as-other cannot create in the root -> brick.
    local bake_owner=4294967294
    echo "==> generating stratum pool fixture ($pool_img, system.key)"
    "$mkfs_bin" "$pool_img" --size 64M --keyfile "$keyfile" \
            --seed "$mkfs_seed" --root-uid "$bake_owner" --root-gid "$bake_owner" \
            >/dev/null 2>&1 || {
        echo "==> stratum-mkfs failed; rerunning with stderr visible" >&2
        "$mkfs_bin" "$pool_img" --size 64M --keyfile "$keyfile" \
            --seed "$mkfs_seed" --root-uid "$bake_owner" --root-gid "$bake_owner"
        exit 2
    }

    if [[ ! -f "$pool_img" || ! -f "$keyfile" ]]; then
        echo "==> stratum pool fixture: mkfs did not produce both pool.img and system.key" >&2
        exit 1
    fi
    echo "==> stratum pool fixture: $(wc -c < "$pool_img" | tr -d ' ') bytes ($pool_img), $(wc -c < "$keyfile" | tr -d ' ') bytes ($keyfile)"
    ledger "pool.img + system.key: REGENERATED (fresh random key, seed=$mkfs_seed) -- the ramfs MUST be re-baked so /system.key matches"

    # P6-pouch-stratumd-boot 16c: populate the freshly-formatted pool with
    # the boot binary corpus + a sentinel for joey's post-pivot probe. The
    # "installer" of the live-medium -> installer -> boot-from-installed
    # pattern (docs/reference/86-pouch-stratumd-boot.md "### The 16c
    # live-medium + host-bake + pivot design"), implemented as host-side
    # orchestration of already-shipping Stratum v2 binaries:
    #   1. start stratumd in the background on a unique temp Unix socket
    #   2. poll for the socket to appear (stratumd has to bind first)
    #   3. stratum-fs write each corpus file under its target path
    #   4. stratum-fs sync (whole-pool commit)
    #   5. SIGTERM stratumd; wait for clean exit
    # No new Stratum-side code; pure shell glue. bake_owner passed explicitly
    # (A-3 audit F4: not via bash dynamic scope) so the data flow is legible.
    populate_stratum_pool "$bake_owner"
}

populate_stratum_pool() {
    # See the trailing block of build_stratum_pool_fixture for the design.
    local bake_owner="${1:?populate_stratum_pool requires the bake-owner uid/gid}"
    local host_build="$BUILD_DIR/host-stratum"
    local stratumd_bin="$host_build/src/cmd/stratumd/stratumd"
    local stratum_fs_bin="$host_build/src/cmd/stratum-fs/stratum-fs"
    local fixtures="$BUILD_DIR/fixtures"
    local pool_img="$fixtures/pool.img"
    local keyfile="$fixtures/system.key"
    # Unique-per-build socket path. The basename includes $$ so two
    # concurrent build runs (unlikely but possible on a CI matrix)
    # don't collide. Path stays under build/fixtures so cleanup is
    # bounded.
    local sock_path="$fixtures/mkfs-populate.$$.sock"
    local stratumd_log="$fixtures/mkfs-populate.stratumd.log"

    if [[ ! -x "$stratumd_bin" || ! -x "$stratum_fs_bin" ]]; then
        # Should be impossible -- build_stratum_pool_fixture's call to
        # build_stratum_host_tools at the top guarantees both. Defensive.
        echo "==> populate_stratum_pool: stratumd or stratum-fs missing from $host_build" >&2
        exit 1
    fi

    # Defensive: rm a stale socket from a prior interrupted run.
    rm -f "$sock_path"

    # A-5c-b: build the host corvus-mint tool BEFORE starting stratumd (its build
    # is independent + can be slow on a cold cargo cache; don't hold the pool open
    # for it). cargo's incremental build handles staleness. corvus-mint reuses
    # corvus's crypto via corvus-crypto, so the wraps it mints are byte-identical
    # to what the on-device corvus reads at boot.
    local cm_manifest="$REPO_ROOT/tools/corvus-mint/Cargo.toml"
    local cm_bin="$REPO_ROOT/tools/corvus-mint/target/release/corvus-mint"
    echo "==> populate pool: building corvus-mint (host system-identity minter)"
    cargo build --manifest-path "$cm_manifest" --release \
        || { echo "==> populate pool: corvus-mint BUILD FAILED" >&2; exit 1; }

    echo "==> populate pool: starting stratumd on $sock_path"
    # Bind to a deterministic dataset (1, the default). Backlog 4 is
    # plenty for the single sequential stratum-fs caller. Logs go to
    # the fixtures dir so a failed populate is forensically inspectable
    # without rerunning the whole build.
    # --listen takes a raw filesystem path (NOT a unix:PATH URL prefix --
    # stratumd's listen_unix calls bind(2) on the path verbatim).
    # A-3: stamp every baked file PRINCIPAL_SYSTEM-owned via --bake-owner-uid
    # (bake_owner is the arg passed by build_stratum_pool_fixture -- the same
    # value it gave mkfs --root-uid; so the root inode AND the baked files are
    # SYSTEM-owned and the boot chain owns the whole tree).
    "$stratumd_bin" "$pool_img" \
        --listen "$sock_path" \
        --keyfile "$keyfile" \
        --root-dataset 1 \
        --backlog 4 \
        --bake-owner-uid "$bake_owner" \
        --bake-owner-gid "$bake_owner" \
        > "$stratumd_log" 2>&1 &
    local stratumd_pid=$!

    # Cleanup trap: SIGTERM stratumd on any path out (including script
    # interrupt). Defensive against build script bailing mid-populate
    # and leaving a zombie stratumd holding the pool open. The trap is
    # cleared after the explicit kill at the end of the happy path so
    # subsequent populate runs in the same shell don't fire it twice.
    trap "kill -TERM $stratumd_pid 2>/dev/null; rm -f \"$sock_path\"" EXIT INT TERM

    # Poll for the socket to appear. stratumd's listen_unix is
    # synchronous (bind -> chmod -> listen -> fcntl) so the socket file
    # exists once the listen() returns. ~5 second window covers cold
    # mount + listen on a slow CI; failure logs the stratumd output.
    local waited=0
    while [[ ! -S "$sock_path" ]]; do
        if [[ $waited -ge 50 ]]; then
            echo "==> populate pool: stratumd did not bind $sock_path within 5s" >&2
            echo "==> populate pool: stratumd log follows:" >&2
            cat "$stratumd_log" >&2 || true
            kill -TERM "$stratumd_pid" 2>/dev/null || true
            exit 1
        fi
        sleep 0.1
        waited=$((waited + 1))
    done
    echo "==> populate pool: stratumd bound; running stratum-fs population"

    # The populate set. v1.0 16c minimum: one sentinel file at
    # /thylacine-version (ROOT-level, single component). joey's
    # pre/post-pivot probes walk + read + content-check this so a
    # regression in SYS_PIVOT_ROOT or SYS_ATTACH_9P_SRV surfaces
    # immediately. The content string is distinct from /version in
    # devramfs (which contains "Thylacine v0.1-dev") so a content-check
    # cannot pass against the WRONG root_spoor.
    #
    # Path constraint: SYS_WALK_OPEN is single-component-only at v1.0,
    # so the sentinel MUST be at the root (no /etc/ prefix). Multi-
    # component path resolution lands with the production open()
    # syscall in a later chunk.
    #
    # Forward-compat populate of the boot binary corpus (joey, corvus,
    # stratumd, pouch hellos, thread-probe) is a deliberate v1.x lift:
    # at v1.0, post-pivot joey does NOT respawn anything from disk
    # (joey is loaded in RAM via cpio and stays running), so no current
    # consumer needs the binaries on the pool. Phase 7's interactive
    # shell (Utopia) is the first consumer that would; this is where
    # the corpus populate lands.
    local sentinel_content="Thylacine 1.0-dev (16c boot-from-disk; populated at host build time)"

    echo "$sentinel_content" | "$stratum_fs_bin" -s "$sock_path" write /thylacine-version \
        || { echo "==> populate pool: write /thylacine-version FAILED" >&2; kill -TERM "$stratumd_pid"; exit 1; }
    "$stratum_fs_bin" -s "$sock_path" sync \
        || { echo "==> populate pool: sync FAILED" >&2; kill -TERM "$stratumd_pid"; exit 1; }
    echo "==> populate pool: /thylacine-version written ($(echo "$sentinel_content" | wc -c | tr -d ' ') bytes)"

    # --- A-5c-b: host-bake the system identity into /var/lib/corvus ---
    # corvus-mint mints the admin keypair + system-wrap (keypair under the
    # build-time system passphrase) + system-recovery-wrap (the same keypair under
    # a fresh BIP-39 recovery phrase) and self-verifies both unwrap. The files are
    # INERT until the A-5c-b corvus runtime (system_identity_load + the real
    # ADMIN_ELEVATE) lands -- baking them now changes no boot behavior.
    local cm_out="$fixtures/corvus-identity.$$"
    rm -rf "$cm_out"; mkdir -p "$cm_out"
    # corvus-mint prints the system recovery phrase to stdout -- log it (forensic,
    # like the mkfs seed). CORVUS_SYSTEM_PASSPHRASE overrides the "thylacine"
    # default (kept the known constant at v1.0 so joey's ADMIN_ELEVATE E2E stays
    # green + the build reproducible; the WRAP is real Argon2id+AEGIS regardless).
    local sys_recovery_phrase
    if ! sys_recovery_phrase="$("$cm_bin" "$cm_out")"; then
        echo "==> populate pool: corvus-mint RUN FAILED" >&2
        kill -TERM "$stratumd_pid"; exit 1
    fi
    echo "==> populate pool: system recovery phrase (hostowner-c): $sys_recovery_phrase"
    # Create /var/lib/corvus top-down (stratum-fs mkdir is single-level, no -p);
    # joey's runtime mkdir_or_open of the same chain then no-ops (idempotent).
    local d
    for d in /var /var/lib /var/lib/corvus; do
        "$stratum_fs_bin" -s "$sock_path" mkdir "$d" \
            || { echo "==> populate pool: mkdir $d FAILED" >&2; kill -TERM "$stratumd_pid"; exit 1; }
    done
    "$stratum_fs_bin" -s "$sock_path" write /var/lib/corvus/system-wrap < "$cm_out/system-wrap" \
        || { echo "==> populate pool: write system-wrap FAILED" >&2; kill -TERM "$stratumd_pid"; exit 1; }
    "$stratum_fs_bin" -s "$sock_path" write /var/lib/corvus/system-recovery-wrap < "$cm_out/system-recovery-wrap" \
        || { echo "==> populate pool: write system-recovery-wrap FAILED" >&2; kill -TERM "$stratumd_pid"; exit 1; }
    "$stratum_fs_bin" -s "$sock_path" sync \
        || { echo "==> populate pool: sync (system identity) FAILED" >&2; kill -TERM "$stratumd_pid"; exit 1; }
    # Byte-verify the round-trip through the pool (catches any binary mangling on
    # the stratum-fs write path NOW, not at the A-5c-b boot-unwrap in the runtime
    # sub-commit). read the blob back + cmp against the minted file.
    "$stratum_fs_bin" -s "$sock_path" read /var/lib/corvus/system-wrap | cmp -s - "$cm_out/system-wrap" \
        || { echo "==> populate pool: system-wrap readback MISMATCH" >&2; kill -TERM "$stratumd_pid"; exit 1; }
    "$stratum_fs_bin" -s "$sock_path" read /var/lib/corvus/system-recovery-wrap | cmp -s - "$cm_out/system-recovery-wrap" \
        || { echo "==> populate pool: system-recovery-wrap readback MISMATCH" >&2; kill -TERM "$stratumd_pid"; exit 1; }
    rm -rf "$cm_out"
    echo "==> populate pool: system identity baked + readback-verified at /var/lib/corvus (system-wrap + system-recovery-wrap, SYSTEM-owned)"

    # --- net-4a: bake the network database at /lib/ndb/local ---
    # netd (a confined warden-bound leaf driver, I-34) compiles in a
    # byte-identical copy and serves /net/cs from it (NET-DESIGN s5: it cannot
    # read /lib, so the live read is the v1.x cs/dns daemon split). This
    # user-readable SYSTEM-owned copy is the canonical ndb(6) file + that v1.x
    # split's live source. mkdir is single-level (no -p); /lib then /lib/ndb.
    local ndb_src="$REPO_ROOT/usr/netd/ndb/local"
    for d in /lib /lib/ndb; do
        "$stratum_fs_bin" -s "$sock_path" mkdir "$d" \
            || { echo "==> populate pool: mkdir $d FAILED" >&2; kill -TERM "$stratumd_pid"; exit 1; }
    done
    "$stratum_fs_bin" -s "$sock_path" write /lib/ndb/local < "$ndb_src" \
        || { echo "==> populate pool: write /lib/ndb/local FAILED" >&2; kill -TERM "$stratumd_pid"; exit 1; }
    "$stratum_fs_bin" -s "$sock_path" sync \
        || { echo "==> populate pool: sync (ndb) FAILED" >&2; kill -TERM "$stratumd_pid"; exit 1; }
    "$stratum_fs_bin" -s "$sock_path" read /lib/ndb/local | cmp -s - "$ndb_src" \
        || { echo "==> populate pool: /lib/ndb/local readback MISMATCH" >&2; kill -TERM "$stratumd_pid"; exit 1; }
    echo "==> populate pool: /lib/ndb/local baked + readback-verified (NET-DESIGN s5 ndb)"

    # Clean stratumd shutdown: SIGTERM then wait. stratumd unmounts the
    # pool + flushes on its way out, so the pool.img bytes after this
    # wait are the steady state joey will see when it mounts via 9P.
    kill -TERM "$stratumd_pid"
    wait "$stratumd_pid" 2>/dev/null || true
    rm -f "$sock_path"
    # Clear the trap; happy path is done.
    trap - EXIT INT TERM
    echo "==> populate pool: stratumd shut down cleanly; pool.img populated"
}

build_pouch_progs() {
    # Phase 6 (Pouch) — cross-compile the pouch POSIX test programs (the
    # hello binaries) against the pouch sysroot. These are the first
    # POSIX C programs Thylacine runs: hosted (musl CRT + libc.a), not
    # freestanding. See docs/reference/78-pouch.md + docs/POUCH-DESIGN.md §14.
    #
    # Two steps, deliberately:
    #   1. compile each .c with tools/pouch-clang (clang as the compiler).
    #   2. link with tools/pouch-ld, which invokes ld.lld directly. clang as
    #      the *link driver* cannot be used: for the unknown "thylacine" OS on
    #      a macOS host clang falls into the Darwin toolchain and emits Mach-O
    #      linker arguments. The compiler path is unaffected.
    #
    # pouch-ld produces a static, non-PIE ET_EXEC with page-aligned PT_LOAD
    # file offsets (-z separate-loadable-segments) — the layout kernel/elf.c +
    # exec_map_segment accept with no kernel change — and supplies the CRT +
    # libc.a. See docs/reference/78-pouch.md.
    #
    # The sysroot must exist; it is built on demand here if absent
    # (build_sysroot, ~1-2 min). A patch-series change needs an explicit
    # `tools/build.sh sysroot` to refresh it — this step reuses what it finds.
    local sysroot="$BUILD_DIR/sysroot"
    local progs_out="$BUILD_DIR/pouch/progs"
    local src_dir="$REPO_ROOT/usr/pouch-hello"
    local pouch_clang="$REPO_ROOT/tools/pouch-clang"
    local pouch_ld="$REPO_ROOT/tools/pouch-ld"
    local readelf="$LLVM_PREFIX/bin/llvm-readelf"

    if sysroot_is_stale; then
        if [[ -f "$sysroot/lib/libc.a" ]]; then
            echo "==> pouch progs: sysroot STALE (a pouch patch is newer than libc.a) — rebuilding it first"
        else
            echo "==> pouch progs: sysroot incomplete — building it first"
        fi
        build_sysroot
    else
        ledger "sysroot: REUSED (cached + up-to-date; force with 'tools/build.sh sysroot', or 'tools/build.sh clean' for a full nuke)"
    fi

    # Preserve stratumd (and other future daemon binaries) installed by
    # build_stratumd into the same $progs_out directory. Only remove the
    # pouch-hello-* binaries we're about to rebuild, by-name.
    mkdir -p "$progs_out"
    rm -f "$progs_out"/pouch-hello*.o "$progs_out"/pouch-hello*

    local prog
    for prog in pouch-hello pouch-hello-stdio pouch-hello-printf pouch-hello-malloc pouch-hello-mallocng-torture pouch-hello-threads pouch-hello-exitgroup pouch-hello-poll pouch-hello-getrandom pouch-hello-sockets pouch-hello-signals pouch-hello-sodium pouch-hello-argv pouch-hello-fault; do
        echo "==> pouch prog: $prog"
        # 1. compile (clang). -nostdinc + -isystem: pouch owns the include
        #    path. -fno-pie: non-PIC codegen for a fixed-address ET_EXEC.
        "$pouch_clang" -std=gnu11 -O2 -Wall -Wextra \
            -nostdinc -isystem "$sysroot/include" -fno-pie \
            -c "$src_dir/$prog.c" -o "$progs_out/$prog.o"
        # 2. link via tools/pouch-ld — it drives ld.lld directly with the
        #    pouch link line and supplies the CRT + libc.a. Per-binary
        #    libsets pull in extra archives (e.g., libsodium for the
        #    sub-chunk 14 proving binary). -L<sysroot>/lib precedes
        #    -lsodium so the linker finds libsodium.a alongside libc.a.
        local extra_libs=()
        case "$prog" in
            pouch-hello-sodium) extra_libs=( -L"$sysroot/lib" -lsodium ) ;;
        esac
        # ${arr[@]+"${arr[@]}"} expands to nothing when arr is empty (which
        # the default `${arr[@]}` cannot under `set -u`); the if-set guard
        # is the canonical bash idiom.
        POUCH_SYSROOT="$sysroot" LLD_PREFIX="$LLD_PREFIX" \
            "$pouch_ld" "$progs_out/$prog.o" \
            ${extra_libs[@]+"${extra_libs[@]}"} \
            -o "$progs_out/$prog"
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
    ledger "disk.img: REBUILT"
}

build_all() {
    # `all` is an ALIAS for `kernel`: build_kernel pulls in userspace +
    # pouch-progs + stratumd + pool-fixture + ramfs + disk internally (and
    # auto-rebuilds the pouch sysroot if a patch is newer than libc.a). The
    # end-of-run SUMMARY reports exactly what was (re)built vs reused.
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
    stratumd)    build_stratumd    ;;
    userspace)   build_userspace   ;;
    disk)        build_disk        ;;
    # The keyfile is a ramfs input: build_ramfs bakes build/fixtures/system.key
    # into the cpio at /system.key. Re-baking the pool regenerates that key
    # (libsodium-random per run), so the ramfs MUST be rebuilt too -- otherwise
    # the VM mounts the FRESH pool with the STALE ramfs key, stratumd derives the
    # wrong metadata key, and the first btree-node AEAD tag verify fails with
    # STM_EBADTAG ("stratumd: run failed (rc=-201)" at mount). That stale-key
    # mismatch masqueraded as a content-sensitive "AEGIS-256 corruption" for ~a
    # year; coupling the ramfs rebuild to the pool re-bake is the fix. See
    # docs/DEBUGGING-PLAYBOOK.md.
    pool)        build_stratum_pool_fixture; build_ramfs ;;
    all)         build_all         ;;
    clean)       clean             ;;
    *)
        echo "Unknown target: $target" >&2
        echo "Valid: kernel, ramfs, sysroot, pouch-progs, stratumd, userspace, disk, pool, all, clean" >&2
        exit 1
        ;;
esac

# --- build summary ------------------------------------------------------------
# Print EXACTLY what this invocation (re)built / reused / preserved, so a session
# reading the tail of the build output knows the resulting state without having
# to re-derive each target's internal chain. (`clean` adds nothing -> no block.)
if [[ -n "$BUILD_LEDGER" ]]; then
    echo ""
    echo "==> [build.sh] SUMMARY for target '$target' (build_type=$build_type):"
    printf '%s' "$BUILD_LEDGER"
    echo "==> [build.sh] note: a pouch ABI/patch change auto-triggers a sysroot REBUILT above"
    echo "    (any patch newer than libc.a); 'tools/build.sh clean' forces a full from-scratch rebuild."
fi
