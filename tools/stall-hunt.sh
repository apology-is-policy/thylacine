#!/usr/bin/env bash
# tools/stall-hunt.sh -- #125: loop one LS-CI scenario with the non-UART
# observer attached, and PRESERVE the evidence when a boot stalls.
#
# #125 is a boot that goes silent mid-line and never reaches login, at a rate
# of roughly 1 in 33. Two things make it hard to study, and this script exists
# for both:
#
#   1. THE EVIDENCE GETS OVERWRITTEN. LS-CI writes build/ls-ci-<scen>.log per
#      attempt, so the ONE captured stall was destroyed by the next green run
#      before it could be read twice. Every failing attempt here is copied to
#      its own directory before the next boot starts.
#   2. THE CONSOLE CANNOT DIAGNOSE A DEAD CONSOLE. tools/stall-watch.py samples
#      the vCPU register files over QMP, so a stalled boot is classified H-A
#      (guest alive, channel dead) vs H-B (guest wedged) from outside.
#
# Retries are FORCED OFF (LS_CI_ATTEMPTS=1): the whole point is to see the
# stall, and a retry would heal it into a PASS and hide the rate.
#
# Host contention is RECORDED, not assumed away: each attempt notes whether a
# sibling worktree had QEMU running. That is a condition of the measurement,
# never an explanation for a result (CLAUDE.md: no "host load").
#
# usage: stall-hunt.sh [N] [scenario]

set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
N="${1:-30}"
SCEN="${2:-ls-ci}"
OUTDIR="$REPO_ROOT/build/stall-hunt"
LOG="$REPO_ROOT/build/ls-ci-$SCEN.log"
QMP="$REPO_ROOT/build/qmp.sock"
ELF="$REPO_ROOT/build/kernel/thylacine.elf"

mkdir -p "$OUTDIR"
SUMMARY="$OUTDIR/summary.txt"
: > "$SUMMARY"

echo "==> stall-hunt: N=$N scenario=$SCEN"
echo "==> evidence -> $OUTDIR ; summary -> $SUMMARY"
[[ -f "$ELF" ]] || echo "==> note: no kernel ELF at $ELF -- PCs stay raw (unsymbolized)"

stalls=0
for i in $(seq 1 "$N"); do
    # Record contention as a MEASUREMENT CONDITION. Scoped to sibling trees so
    # this never matches our own boot (the #89 unscoped-pattern rule).
    others=$(pgrep -fc "qemu-system-aarch64.*thylacine-aux/build/" 2>/dev/null || echo 0)

    rm -f "$LOG"
    python3 "$REPO_ROOT/tools/stall-watch.py" \
        --sock "$QMP" --log "$LOG" --elf "$ELF" \
        --out "$OUTDIR/watch-current.log" \
        --quiet-s 25 --sample-s 2 --deadline-s 400 \
        > "$OUTDIR/watch-stdout.log" 2>&1 &
    watcher=$!

    start=$(date +%s)
    LS_CI_ATTEMPTS=1 "$REPO_ROOT/tools/test-interactive.sh" "$SCEN" \
        > "$OUTDIR/run-current.log" 2>&1
    rc=$?
    took=$(( $(date +%s) - start ))

    kill "$watcher" 2>/dev/null; wait "$watcher" 2>/dev/null

    # The watcher's own verdict is the payload; grep it out UNCONDITIONALLY so
    # a nothing-case is visibly a nothing-case, never mistaken for a pass.
    verdict=$(grep -aoE 'H-[AB]: [^;]*' "$OUTDIR/watch-current.log" 2>/dev/null | tail -1)
    [[ -n "$verdict" ]] || verdict="(console never went quiet -- no sample taken)"

    if [[ $rc -ne 0 ]]; then
        stalls=$((stalls + 1))
        keep="$OUTDIR/stall-$(printf '%03d' "$i")"
        mkdir -p "$keep"
        cp -f "$LOG" "$keep/console.log" 2>/dev/null
        cp -f "$OUTDIR/watch-current.log" "$keep/watch.log" 2>/dev/null
        cp -f "$OUTDIR/run-current.log" "$keep/run.log" 2>/dev/null
        printf 'attempt %3d  FAIL rc=%d  %4ds  others=%s  %s  [kept: %s]\n' \
            "$i" "$rc" "$took" "$others" "$verdict" "${keep#"$REPO_ROOT"/}" | tee -a "$SUMMARY"
    else
        printf 'attempt %3d  pass       %4ds  others=%s  %s\n' \
            "$i" "$took" "$others" "$verdict" | tee -a "$SUMMARY"
    fi
    rm -f "$OUTDIR/watch-current.log"
done

echo "==> stall-hunt: $stalls stall(s) in $N attempt(s) of '$SCEN'" | tee -a "$SUMMARY"
[[ $stalls -eq 0 ]] && echo "==> NOT a clean bill: 0/$N only bounds the rate below ~1/$N" | tee -a "$SUMMARY"
exit 0
