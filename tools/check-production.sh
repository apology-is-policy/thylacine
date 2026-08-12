#!/usr/bin/env bash
# tools/check-production.sh -- prove the lean production shape still BUILDS (#228).
#
# WHY THIS EXISTS. `--production` (KERNEL_TESTS=OFF + THYLA_BOOT_PROBES=OFF) is
# the scripture-named v1.0 lean boot shape -- TOOLING.md section 9, `make
# production`, docs/holotype/11-performance.md -- and on 2026-08-12 it was found
# not to compile at all: joey failed with 11 errors, because probe blocks had
# been appended AFTER their gate's #endif and called helpers that only exist
# inside it. It had been broken for some time and nothing noticed, because
# NOTHING BUILDS THIS SHAPE: not tools/test.sh, not the SMP gate, not the
# interactive harness, not any Makefile default.
#
# That is #212's disease one level up. There, a gate that did not run reported
# PASS; here, a build shape nobody builds reported nothing at all. So this
# exists to be run -- cheaply, by anyone, without booting.
#
# It builds, it does not boot. Compiling is not booting: the lean shape spawns
# no warden, so it has no drivers, no network and no compositor, and what that
# SHOULD be is a design question (tracked), not something a compile can answer.
#
#   tools/check-production.sh          # joey at THYLA_BOOT_PROBES=OFF (~30 s)
#   tools/check-production.sh --all    # + the KERNEL_TESTS=OFF kernel (~3 min)
#
# Exit: 0 builds, 1 does not, 2 usage/setup.
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$REPO_ROOT/build/prodcheck"
GEN_DIR="$REPO_ROOT/build/generated"
TOOLCHAIN="$REPO_ROOT/cmake/Toolchain-aarch64-userspace.cmake"

do_all=0
[[ "${1:-}" == "--all" ]] && do_all=1

# The generated headers are a build product of the normal path (the corvus
# recovery-phrase header among them). Without them this cannot even configure,
# and the failure would read like a source error rather than a missing input.
if [[ ! -d "$GEN_DIR" ]]; then
    echo "check-production: $GEN_DIR missing -- run tools/build.sh userspace first." >&2
    exit 2
fi

echo "== check-production: joey at THYLA_BOOT_PROBES=OFF =="
rm -rf "$OUT"
if ! cmake -S "$REPO_ROOT/usr" -B "$OUT" \
        -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
        -DCMAKE_BUILD_TYPE=Debug \
        -DTHYLA_GENERATED_DIR="$GEN_DIR" \
        -DTHYLA_BOOT_PROBES=OFF > "$OUT.cmake.log" 2>&1; then
    echo "check-production: FAIL -- cmake configure failed; see $OUT.cmake.log" >&2
    exit 1
fi

if ! cmake --build "$OUT" --target joey > "$OUT.build.log" 2>&1; then
    echo "check-production: FAIL -- joey does not compile with THYLA_BOOT_PROBES=OFF." >&2
    echo "    The usual cause is a probe block appended AFTER its gate's #endif:" >&2
    echo "    it then calls helpers that only exist inside the gate. Move the" >&2
    echo "    #endif, do not move the helpers -- the lean image must not carry" >&2
    echo "    the probe ladder." >&2
    grep -E "error:" "$OUT.build.log" | sed "s|$REPO_ROOT/||" | head -20 >&2
    exit 1
fi
echo "  ok -- joey builds lean ($(grep -c 'warning: unused function' "$OUT.build.log") unused-function warnings; see #229)"

if (( do_all )); then
    echo "== check-production: the full --production build =="
    if ! THYLACINE_MKFS_PRESERVE=1 "$REPO_ROOT/tools/build.sh" all --production \
            > "$OUT.full.log" 2>&1; then
        echo "check-production: FAIL -- tools/build.sh all --production failed; see $OUT.full.log" >&2
        tail -20 "$OUT.full.log" >&2
        exit 1
    fi
    echo "  ok -- kernel (KERNEL_TESTS=OFF) + userspace + ramfs + disk built"
    echo "  NOTE: the tree now holds PRODUCTION artifacts. Rebuild the default"
    echo "        shape before running tools/test.sh for anything else."
fi

echo "== check-production: PASS =="
