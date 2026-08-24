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
# Since the configurator (BUILD-CONFIG-DESIGN.md 4.5) it also builds the lean
# LOGINNABLE shape (BOOT_PROBES=OFF + DEV_ACCOUNTS=ON) and asserts the joey binary
# grows -- proof that provision_dev_accounts (the finding-#1 login fix) actually
# compiled in, a shape nothing else in the routine loop builds.
#
#   tools/check-production.sh          # joey lean (both-off) + lean loginnable (~4 s)
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

# Config B: the lean LOGINNABLE image (BOOT_PROBES=OFF + DEV_ACCOUNTS=ON). This is
# the shape provision_dev_accounts actually compiles into -- the finding-#1 fix, the
# only path that creates a login user in a lean image -- and NOTHING in the routine
# loop builds it either: it is the both-off shape's disease one axis over. Under
# -Werror=unused-function it also proves pda_connect / pda_write_all / pda_read_exact
# and provision_dev_accounts are all REACHED; an unreached one would fail the build.
#
# The size assertion is the discrimination (M-PIN: a check that cannot fail proves
# nothing). Config A above compiles provision_dev_accounts OUT; config B compiles it
# IN. If the two joey binaries come out the SAME size, the DEV_ACCOUNTS define did
# nothing -- the -DTHYLA_DEV_ACCOUNTS wiring silently broke -- and "it built" would
# be a green that means the opposite of what it claims. Same Debug build type in both
# builds so DEV_ACCOUNTS is the only variable between them.
OUT_DA="$OUT.devacct"
echo "== check-production: joey at BOOT_PROBES=OFF + DEV_ACCOUNTS=ON (lean loginnable) =="
rm -rf "$OUT_DA"
if ! cmake -S "$REPO_ROOT/usr" -B "$OUT_DA" \
        -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
        -DCMAKE_BUILD_TYPE=Debug \
        -DTHYLA_GENERATED_DIR="$GEN_DIR" \
        -DTHYLA_BOOT_PROBES=OFF \
        -DTHYLA_DEV_ACCOUNTS=ON > "$OUT_DA.cmake.log" 2>&1; then
    echo "check-production: FAIL -- cmake configure (DEV_ACCOUNTS=ON) failed; see $OUT_DA.cmake.log" >&2
    exit 1
fi
if ! cmake --build "$OUT_DA" --target joey > "$OUT_DA.build.log" 2>&1; then
    echo "check-production: FAIL -- joey does not compile with DEV_ACCOUNTS=ON/BOOT_PROBES=OFF." >&2
    if grep -q "unused function" "$OUT_DA.build.log"; then
        # The tripwire fired on the loginnable shape. Either a DEV_ACCOUNTS-only
        # helper (provision_dev_accounts / a pda_*) is never called, or a probe
        # helper leaked into the shared region. Different remedy from a plain error.
        echo "    The #229 tripwire fired: a helper reachable only under DEV_ACCOUNTS" >&2
        echo "    is defined but not called, or a probe helper leaked out of its gate." >&2
        echo "    Check the '#if defined(THYLA_DEV_ACCOUNTS) && !defined(THYLA_BOOT_PROBES)'" >&2
        echo "    region in usr/joey/joey.c against its call site." >&2
    fi
    grep -E "error:" "$OUT_DA.build.log" | sed "s|$REPO_ROOT/||" | head -20 >&2
    exit 1
fi
JOEY_A="$OUT/joey/joey"
JOEY_B="$OUT_DA/joey/joey"
if [[ ! -f "$JOEY_A" || ! -f "$JOEY_B" ]]; then
    echo "check-production: FAIL -- a joey binary is missing; cannot size-discriminate." >&2
    echo "    ($JOEY_A / $JOEY_B)" >&2
    exit 1
fi
SIZE_A="$(wc -c < "$JOEY_A" | tr -d ' ')"
SIZE_B="$(wc -c < "$JOEY_B" | tr -d ' ')"
if [[ "$SIZE_B" -le "$SIZE_A" ]]; then
    echo "check-production: FAIL -- the DEV_ACCOUNTS=ON joey ($SIZE_B B) is not larger than" >&2
    echo "    the both-off joey ($SIZE_A B). provision_dev_accounts did not compile in --" >&2
    echo "    the -DTHYLA_DEV_ACCOUNTS wiring is silently dead. This check cannot pass on a" >&2
    echo "    broken gate; that is the point." >&2
    exit 1
fi
echo "  ok -- joey builds lean+loginnable; provision_dev_accounts compiled in ($SIZE_A -> $SIZE_B B, +$((SIZE_B - SIZE_A)))"

if (( do_all )); then
    echo "== check-production: the full lean image build (--config production) =="
    if ! "$REPO_ROOT/tools/build.sh" all --config production > "$OUT.full.log" 2>&1; then
        echo "check-production: FAIL -- build.sh all --config production failed; see $OUT.full.log" >&2
        tail -20 "$OUT.full.log" >&2
        exit 1
    fi
    echo "  ok -- lean (release, KASLR, hardened) kernel + userspace + ramfs + fresh loginnable pool"

    # The lean spine (provision_dev_accounts, the finding-#1 login fix) is boot-critical
    # in this shape and compiled OUT of config C, so NOTHING in the routine loop boots
    # it -- the size delta above proves it COMPILED IN, never that it SUCCEEDS. That is
    # #245's "a checker reachable only by hand rots", one level up: the michael-only
    # predecessor shipped for a session with only a compile gate. So boot the lean image
    # and assert BOTH facts a build cannot see -- the spine RAN, and the accounts it
    # created actually authenticate. Needs `expect`; SKIP (not FAIL) without it, exactly
    # as test-interactive does, so a host lacking it stays green.
    if command -v expect >/dev/null 2>&1; then
        echo "== check-production: boot the lean image -- assert the spine provisioned + login =="
        if ! "$REPO_ROOT/tools/test-interactive.sh" dev-accounts > "$OUT.boot.log" 2>&1; then
            echo "check-production: FAIL -- lean-image login proof (dev-accounts) failed; see $OUT.boot.log" >&2
            tail -30 "$OUT.boot.log" >&2
            exit 1
        fi
        # The login alone is NOT enough: dev-accounts is image-agnostic, so a gate that
        # booted config C by mistake would pass it on LADDER-provisioned accounts. This
        # line is printed ONLY by the lean spine (the ladder never emits it), so it is
        # the witness that config B -- and specifically provision_dev_accounts -- ran.
        SPINE_LOG="$REPO_ROOT/build/ls-ci-dev-accounts.log"
        if ! grep -aq "provision_dev_accounts: michael + cora ready" "$SPINE_LOG" 2>/dev/null; then
            echo "check-production: FAIL -- booted + logged in, but the console never showed" >&2
            echo "    'provision_dev_accounts: michael + cora ready'. Either the lean spine did NOT" >&2
            echo "    run (a config-C image was booted -- the login passed on ladder accounts) or the" >&2
            echo "    spine broke before completion. See $SPINE_LOG." >&2
            exit 1
        fi
        echo "  ok -- lean spine provisioned michael+cora at boot; both authenticate (dev-accounts)"
    else
        echo "  SKIP boot proof -- 'expect' not found (install it to gate the lean spine at runtime)"
    fi

    echo "  NOTE: the tree now holds PRODUCTION artifacts. Rebuild the default"
    echo "        shape before running tools/test.sh for anything else."
fi

echo "== check-production: PASS =="
