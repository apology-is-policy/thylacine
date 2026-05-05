#!/usr/bin/env bash
# tools/verify-kaslr.sh — verify KASLR offset varies across N boots.
#
# Per ROADMAP §4.2 exit criterion: "KASLR: kernel base address differs
# across boots (verified across 10 boots)". This script boots the
# kernel N times (default 10), extracts the KASLR offset from each
# banner, and PASSes iff:
#   - All N boots reach "Thylacine boot OK" (no extinctions / crashes).
#   - The set of distinct offsets has size ≥ floor(N * 0.7) — allowing
#     for the rare collision (with 8192 buckets at 13 bits and 10 boots,
#     probability of a collision is ~0.55%, but we don't want to fail
#     PASS criteria over a single accidental collision in CI).
#
# Per CLAUDE.md verify-step pattern. Stricter than tools/test.sh (which
# accepts any single boot) — exercises the entropy chain across boots.
#
# Usage:
#   tools/verify-kaslr.sh           — 10 boots
#   tools/verify-kaslr.sh -n 25     — N boots
#   tools/verify-kaslr.sh -v        — print each offset

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"

n=10
verbose=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        -n) n="$2"; shift 2 ;;
        -v|--verbose) verbose=1; shift ;;
        *) echo "Usage: $0 [-n N] [-v]" >&2; exit 2 ;;
    esac
done

if (( n < 2 )); then
    echo "verify-kaslr.sh: N must be ≥ 2 (got $n)" >&2
    exit 2
fi

declare -a offsets=()
declare -i ok=0 fail=0

# tools/test.sh tails the log; banner growth (P2-A added kproc/kthread lines)
# can push the KASLR offset out of view. Read the full log file instead so
# verify-kaslr.sh stays robust against future banner additions.
LOG_FILE="$REPO_ROOT/build/test-boot.log"

for i in $(seq 1 "$n"); do
    out="$("$REPO_ROOT/tools/test.sh" 2>&1 || true)"
    if grep -q "^Thylacine boot OK" <<<"$out"; then
        ok+=1
        # KASLR offset: parse "kernel base: 0xVA (KASLR offset 0xN, ...)"
        # from the full log file; the test.sh tail may not contain it.
        offset="$(grep -oE 'KASLR offset 0x[0-9a-fA-F]+' "$LOG_FILE" | head -1 | awk '{print $3}')"
        offsets+=("$offset")
        if (( verbose )); then echo "boot $i: PASS offset=$offset"; fi
    else
        fail+=1
        if (( verbose )); then echo "boot $i: FAIL (no boot OK line)"; fi
        if grep -q "^EXTINCTION:" <<<"$out"; then
            echo "boot $i: EXTINCTION observed:" >&2
            grep "^EXTINCTION:" <<<"$out" >&2
        fi
    fi
done

# Distinct offsets count.
distinct_count=$(printf '%s\n' "${offsets[@]}" | sort -u | wc -l | tr -d ' ')
threshold=$(( n * 7 / 10 ))     # 70% of N; floor

echo "==> ${ok}/${n} boots reached 'Thylacine boot OK'"
echo "==> ${distinct_count} distinct KASLR offsets out of ${ok}"

if (( fail > 0 )); then
    echo "==> FAIL: ${fail} boots failed to reach the banner."
    exit 1
fi
if (( distinct_count < threshold )); then
    echo "==> FAIL: distinct offsets ${distinct_count} below 70% threshold ${threshold}/${n}."
    echo "    (Suggests entropy chain regression — investigate kaslr.c.)"
    printf '    seen: %s\n' "${offsets[@]}" | sort -u
    exit 1
fi

echo "==> PASS: KASLR varies adequately across ${n} boots"
exit 0
