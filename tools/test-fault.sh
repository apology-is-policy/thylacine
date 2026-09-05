#!/usr/bin/env bash
# tools/test-fault.sh — deliberate-fault verification (P1-I).
#
# Per ROADMAP §4.2 exit criteria: each v1.0 hardening protection should
# fire under deliberate attack. This script builds N kernels, each with
# exactly one fault provoker enabled (THYLACINE_FAULT_TEST=<variant>),
# runs each, and PASSes iff the kernel EXTINCTIONs with the expected
# diagnostic message.
#
# Variants at v1.0:
#   canary_smash    — stack canary check fires →
#                     "EXTINCTION: stack canary mismatch (smashed stack)"
#   wxe_violation   — kernel-image permission fault →
#                     "EXTINCTION: PTE violates W^X (kernel image)"
#   bti_fault       — Branch Target Exception (FEAT_BTI required) →
#                     "EXTINCTION: BTI fault (...)"
#   kstack_overflow — per-thread kstack guard page fires (P2-Dc) →
#                     "EXTINCTION: kernel stack overflow"
#   secondary_stack_guard — secondary-CPU boot-stack guard page fires
#                     (P5-secondary-stack-guard) →
#                     "EXTINCTION: kernel stack overflow"
#   bootcpu_idle_guard — boot-CPU idle-stack guard page fires →
#                     "EXTINCTION: kernel stack overflow"
#   recursive_kernel_fault — the #806 handler re-entrancy guard fires →
#                     "EXTINCTION: recursive kernel fault"
#   el1_sync_runaway — a fault taken from INSIDE the extinction path drives
#                     the EL1-sync depth to EL1_SYNC_DEPTH_MAX (#246) →
#                     "EXTINCTION: el1-sync recursion"
#
# This list is the one place a reader learns what exists, so keep it equal to
# ALL_VARIANTS below. It was short by two (bootcpu_idle_guard,
# recursive_kernel_fault) until 2026-08-18 -- the same rot #245 fixed one
# level up, where a thing exists and its only description does not say so.
#
# Deferred at v1.0 (post-v1.0 hardening pass):
#   pac_mismatch — needs forged-LR inline asm; the resulting fault
#   depends on FEAT_FPAC and implementation-specific poison-bit
#   patterns. Verified instead via hardening.detect_smoke +
#   manual code review.
#
# Usage:
#   tools/test-fault.sh              — run all variants
#   tools/test-fault.sh canary_smash — run a single variant
#   tools/test-fault.sh -v           — verbose log dumps
#
# Compatible with bash 3.2 (macOS default — no associative arrays).

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR_BASE="$REPO_ROOT/build"
# fault_test_run() fires AFTER the full in-kernel suite (main.c), and the
# suite's irq-bench long-pole alone is ~10s+ -- so the provoker is not even
# reached inside a 10s budget on a normal/loaded host, producing a false
# "timeout" FAIL. Default to 60s (env-overridable; bump further under heavy
# host load, e.g. taskpolicy -b).
BOOT_TIMEOUT="${BOOT_TIMEOUT:-60}"

# variant → expected extinction substring (case-sensitive prefix match
# against the EXTINCTION: line). Keep the case below in sync with this.
ALL_VARIANTS="canary_smash wxe_violation bti_fault kstack_overflow secondary_stack_guard bootcpu_idle_guard recursive_kernel_fault el1_sync_runaway"

# A substring that must NOT appear for a variant to PASS. Empty = no such
# requirement. Paired with expected_for: "the right line appeared" and "the
# wrong line did not" are SEPARATE claims and need separate checks.
forbid_for() {
    case "$1" in
        el1_sync_runaway) echo "console-ring: NOT held" ;;
        *)                echo "" ;;
    esac
}

expected_for() {
    case "$1" in
        canary_smash)    echo "EXTINCTION: stack canary mismatch" ;;
        wxe_violation)   echo "EXTINCTION: PTE violates W^X" ;;
        bti_fault)       echo "EXTINCTION: BTI fault" ;;
        kstack_overflow) echo "EXTINCTION: kernel stack overflow" ;;
        secondary_stack_guard) echo "EXTINCTION: kernel stack overflow" ;;
        bootcpu_idle_guard)    echo "EXTINCTION: kernel stack overflow" ;;
        recursive_kernel_fault) echo "EXTINCTION: recursive kernel fault" ;;
        el1_sync_runaway)      echo "EXTINCTION: el1-sync recursion" ;;
        # ROUND F6: the variant REACHES cons_tx_claim_for_dump's
        # already-ours arm but could not FAIL if it were wrong -- delete that
        # arm and the re-entrant claim burns its bound, returns false, and the
        # banner still prints ("torn beats silent"), so the expected string is
        # present and the variant passes ~20 ms slower. A control must prove
        # discrimination, not detection. `forbid_for` closes it: on this
        # variant the miss line must be ABSENT.
        *)               echo "" ;;
    esac
}

verbose=0
selected=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        -v|--verbose) verbose=1; shift ;;
        -h|--help)
            echo "Usage: $0 [-v] [variant...]"
            echo "Variants: $ALL_VARIANTS"
            exit 0
            ;;
        # DERIVED from ALL_VARIANTS, never a second hand-written list. There
        # were FOUR enumerations of this set (ALL_VARIANTS, expected_for, this
        # arm, and --help); adding a variant updated two of them and the other
        # two silently refused it. A set with four independent spellings has no
        # spelling anything can be checked against.
        *)
            if [[ " $ALL_VARIANTS " == *" $1 "* ]]; then
                selected="$selected $1"; shift
            else
                echo "Unknown arg: $1" >&2
                echo "Variants: $ALL_VARIANTS" >&2
                exit 2
            fi
            ;;
    esac
done

if [[ -z "$selected" ]]; then
    selected="$ALL_VARIANTS"
fi

pass=0
fail=0

for variant in $selected; do
    expect="$(expected_for "$variant")"
    if [[ -z "$expect" ]]; then
        echo "==> [$variant] no expected diagnostic (script bug)" >&2
        exit 2
    fi
    build_dir="$BUILD_DIR_BASE/kernel-fault-$variant"
    log_file="$BUILD_DIR_BASE/test-fault-$variant.log"

    echo "==> [$variant] building..."
    # #101: capture rather than discard. --build-dir redirects only KERNEL_BUILD,
    # NOT $BUILD_DIR, so `fixtures` stays build/fixtures -- every variant here
    # re-mints the SHARED pool.img from the ambient environment, N times per run,
    # for a harness that never reads the pool at all. That is invisible waste at
    # best and, with a clade toolchain staged but the flag unset, a silent
    # destruction of a clade-baked pool (#101). Surfacing build.sh's bake config
    # line makes both visible without restoring the full build spew.
    bake_log="$BUILD_DIR_BASE/test-fault-$variant-build.log"
    if ! "$REPO_ROOT/tools/build.sh" kernel \
        --build-dir="$build_dir" \
        -- -DTHYLACINE_FAULT_TEST="$variant" > "$bake_log" 2>&1; then
        echo "==> [$variant] BUILD FAILED -- tail of $bake_log:" >&2
        tail -20 "$bake_log" >&2
        exit 2
    fi
    grep -E '^==> (populate pool: bake config|WARNING: a clade)' "$bake_log" || true

    echo "==> [$variant] booting (expecting: $expect)..."
    THYLACINE_BUILD_DIR="$build_dir" "$REPO_ROOT/tools/run-vm.sh" --no-share \
        < /dev/null > "$log_file" 2>&1 &
    pid=$!

    # Wait until either the expected extinction shows up, an
    # unexpected line shows up, or QEMU exits.
    deadline=$(( $(date +%s) + BOOT_TIMEOUT ))
    result="timeout"
    while [[ $(date +%s) -lt $deadline ]]; do
        if [[ -f "$log_file" ]] && grep -qF "$expect" "$log_file"; then
            result="pass"
            break
        fi
        # If we see "Thylacine boot OK" without the expected fault, the
        # provoker didn't fire — that's a FAIL: the protection should
        # have triggered before reaching the success line.
        if [[ -f "$log_file" ]] && grep -q "^Thylacine boot OK" "$log_file"; then
            result="provoker_silent"
            break
        fi
        if [[ -f "$log_file" ]] && grep -q "^EXTINCTION:" "$log_file"; then
            # Race: UART prints character-by-character; we may be
            # mid-line. Wait briefly for the line to complete, then
            # re-check $expect before declaring wrong_extinction.
            sleep 1
            if grep -qF "$expect" "$log_file"; then
                result="pass"
            else
                result="wrong_extinction"
            fi
            break
        fi
        if ! kill -0 "$pid" 2>/dev/null; then
            # QEMU exited; final check.
            if grep -qF "$expect" "$log_file"; then
                result="pass"
            elif grep -q "^EXTINCTION:" "$log_file"; then
                result="wrong_extinction"
            else
                result="qemu_exit"
            fi
            break
        fi
        sleep 0.1
    done
    kill -KILL "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true

    case "$result" in
        pass)
            # ROUND F6: the expected line appearing and the forbidden line
            # being absent are SEPARATE claims. Check the second here, on the
            # arm that otherwise reports success -- a variant that reached its
            # banner the slow way (a broken re-entrant claim burning its bound
            # and emitting unserialized) prints the expected string too.
            forbid="$(forbid_for "$variant")"
            if [[ -n "$forbid" ]] && grep -qF "$forbid" "$log_file"; then
                fail=$((fail + 1))
                echo "==> [$variant] FAIL: saw '$expect' but ALSO '$forbid'"
                echo "    the banner printed, but not the way the variant exists to prove:"
                grep -F "$forbid" "$log_file" | head -2 | sed 's/^/    /'
            else
                pass=$((pass + 1))
                echo "==> [$variant] PASS (saw '$expect'${forbid:+; '"$forbid"' absent})"
                (( verbose )) && grep "^EXTINCTION:" "$log_file" | head -3
            fi
            ;;
        provoker_silent)
            fail=$((fail + 1))
            echo "==> [$variant] FAIL: provoker silent — kernel reached 'Thylacine boot OK' without firing the protection"
            ;;
        wrong_extinction)
            fail=$((fail + 1))
            echo "==> [$variant] FAIL: wrong extinction message:"
            grep "^EXTINCTION:" "$log_file" | head -3
            ;;
        qemu_exit)
            fail=$((fail + 1))
            echo "==> [$variant] FAIL: QEMU exited unexpectedly. Log tail:"
            tail -8 "$log_file"
            ;;
        timeout)
            fail=$((fail + 1))
            echo "==> [$variant] FAIL: timeout (${BOOT_TIMEOUT}s) — neither expected fault nor unrelated termination."
            tail -8 "$log_file"
            ;;
    esac
done

echo
echo "==> Summary: $pass PASS, $fail FAIL out of $((pass + fail))"
[[ $fail -eq 0 ]] || exit 1
exit 0
