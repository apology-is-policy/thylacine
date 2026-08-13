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
# It builds, it does not boot -- compiling is not booting. (Until #230 this note
# also said the lean shape had no drivers, no network and no compositor. That
# was true, and it was the bug: the warden was parked inside the probe gate. It
# is unconditional now, so a lean boot brings up its hardware. The two are still
# separate claims and this script only makes the first one.)
#
# #229 added the tripwire that keeps the shape clean between runs:
# -Werror=unused-function on the joey target, verified below to be armed. A
# probe helper defined outside the gate is now a build failure rather than a
# warning nobody reads.
#
#   tools/check-production.sh          # joey at THYLA_BOOT_PROBES=OFF (~2 s)
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

# #229: the lean build's guarantee is -Werror=unused-function on the joey
# target, not this script's diligence. Read that off the flags CMake actually
# GENERATED -- not off usr/joey/CMakeLists.txt, which is the same source the
# build reads, so agreeing with it would only prove the file agrees with itself
# (#143). If someone drops the option, every later run here would pass while
# silently going back to accumulating warnings.
FLAGS="$OUT/joey/CMakeFiles/joey.dir/flags.make"
if [[ ! -f "$FLAGS" ]]; then
    echo "check-production: FAIL -- $FLAGS absent; cannot confirm the #229 tripwire is armed." >&2
    exit 1
fi
if ! grep -q -- '-Werror=unused-function' "$FLAGS"; then
    echo "check-production: FAIL -- the #229 tripwire is DISARMED." >&2
    echo "    joey is being compiled without -Werror=unused-function, so a probe" >&2
    echo "    helper defined outside the gate is a warning again instead of an" >&2
    echo "    error -- and this script would keep saying PASS while they pile up." >&2
    echo "    Restore target_compile_options(joey PRIVATE -Werror=unused-function)" >&2
    echo "    in usr/joey/CMakeLists.txt." >&2
    exit 1
fi

if ! cmake --build "$OUT" --target joey > "$OUT.build.log" 2>&1; then
    echo "check-production: FAIL -- joey does not compile with THYLA_BOOT_PROBES=OFF." >&2
    if grep -q "unused function" "$OUT.build.log"; then
        # #229: the tripwire fired. This is a DIFFERENT defect from the one below
        # and wants the opposite remedy, so say which one happened.
        echo "    The #229 tripwire fired: a helper below is defined outside the" >&2
        echo "    probe gate and called only from inside it, so the lean image" >&2
        echo "    compiles it and can never reach it. Move the DEFINITION inside" >&2
        echo "    the '#if THYLA_BOOT_PROBES -- the boot-test probe helpers'" >&2
        echo "    region in usr/joey/joey.c (it sits below every production" >&2
        echo "    helper precisely so anything can move into it)." >&2
    else
        echo "    The usual cause is a probe block appended AFTER its gate's #endif:" >&2
        echo "    it then calls helpers that only exist inside the gate. Move the" >&2
        echo "    #endif, do not move the helpers -- the lean image must not carry" >&2
        echo "    the probe ladder." >&2
    fi
    grep -E "error:" "$OUT.build.log" | sed "s|$REPO_ROOT/||" | head -20 >&2
    exit 1
fi
# No warning count here any more, deliberately: with the tripwire armed the
# count is 0 on every build that reaches this line, so printing it would be a
# number that can only ever say one thing -- the shape of a check that has
# stopped being able to fail (#212).
echo "  ok -- joey builds lean; #229 tripwire armed (-Werror=unused-function)"

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
