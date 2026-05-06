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
BOOT_TIMEOUT="${BOOT_TIMEOUT:-10}"

# variant → expected extinction substring (case-sensitive prefix match
# against the EXTINCTION: line). Keep the case below in sync with this.
ALL_VARIANTS="canary_smash wxe_violation bti_fault kstack_overflow"

expected_for() {
    case "$1" in
        canary_smash)    echo "EXTINCTION: stack canary mismatch" ;;
        wxe_violation)   echo "EXTINCTION: PTE violates W^X" ;;
        bti_fault)       echo "EXTINCTION: BTI fault" ;;
        kstack_overflow) echo "EXTINCTION: kernel stack overflow" ;;
        *)               echo "" ;;
    esac
}

verbose=0
selected=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        -v|--verbose) verbose=1; shift ;;
        canary_smash|wxe_violation|bti_fault|kstack_overflow)
            selected="$selected $1"; shift ;;
        -h|--help)
            echo "Usage: $0 [-v] [variant...]"
            echo "Variants: canary_smash wxe_violation bti_fault kstack_overflow"
            exit 0
            ;;
        *) echo "Unknown arg: $1" >&2; exit 2 ;;
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
    "$REPO_ROOT/tools/build.sh" kernel \
        --build-dir="$build_dir" \
        -- -DTHYLACINE_FAULT_TEST="$variant" >/dev/null

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
            pass=$((pass + 1))
            echo "==> [$variant] PASS (saw '$expect')"
            (( verbose )) && grep "^EXTINCTION:" "$log_file" | head -3
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
