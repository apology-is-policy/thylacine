#!/usr/bin/env bash
# SMP multi-boot soundness gate.
#
# Single boots lie: the #788/#806/#860 SMP context-corruption races are
# layout-/timing-sensitive and pass a single boot most of the time. This re-runs
# tools/test.sh N times against the SAME built kernel (host scheduling jitter
# varies the timing each boot) and CLASSIFIES failures, because a nonzero
# test.sh is NOT automatically a scheduler bug:
#
#   CORRUPTION  -- a real SMP soundness failure (the thing we hunt): the
#                  extinction message matches a ctx/stack-corruption signature
#                  (invalid prev state, stack canary, kernel stack overflow,
#                  wild PC, #860, double-run). These FAIL the gate.
#   TIMING      -- a benign host-fragility failure: a latency/quiescence test
#                  tripped under host oversubscription (irq-latency CI budget,
#                  stalk-lifetime, torpor stall). These are NOTED, not counted
#                  as scheduler failures (DEBUGGING-PLAYBOOK section 6: do not
#                  conflate host-fragile test fails with scheduler corruption).
#   OTHER       -- an unclassified nonzero exit; surfaced for investigation.
#
# Usage:  tools/smp-multiboot.sh <label> <cpus> <N> [undefined]
#   e.g.  tools/smp-multiboot.sh ubsan-smp4 4 15 undefined
#         tools/smp-multiboot.sh default-smp8 8 15
#
# Exit 0 iff 0 CORRUPTION failures across all N boots.
set -u

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LABEL="${1:-default-smp4}"
CPUS="${2:-4}"
N="${3:-10}"
SAN="${4:-}"
sanflag=""
[[ -n "$SAN" ]] && sanflag="--sanitize=$SAN"

LOG="$REPO_ROOT/build/test-boot.log"
FAILDIR="$REPO_ROOT/build/multiboot-fails"
mkdir -p "$FAILDIR"

# Signatures of a real ctx/stack corruption (the SMP soundness bug class).
# Use the EXACT extinction strings -- bare "canary" would match the benign
# "canaries" hardening banner + "canary: initialized" boot line (false positive).
CORRUPT_RE='invalid prev state|stack canary mismatch|kernel stack overflow|already on_cpu|#860|not RUNNABLE-and-off-cpu|corrupted current|sched: deadlock'
# Signatures of benign host-timing fragility (not a scheduler bug).
TIMING_RE='irq-latency|exceeds CI|sanity budget|IRQ-to-userspace|stalk.*lifetime|torpor.*stall|quiescence'

pass=0; corrupt=0; timing=0; other=0
for i in $(seq 1 "$N"); do
    if THYLACINE_TEST_CPUS="$CPUS" "$REPO_ROOT/tools/test.sh" $sanflag >/dev/null 2>&1; then
        # Belt-and-suspenders: even on exit 0, fail if a corruption marker leaked.
        if grep -qE "$CORRUPT_RE" "$LOG"; then
            corrupt=$((corrupt+1)); cp "$LOG" "$FAILDIR/$LABEL-$i-CORRUPT.log"
            echo "  [$LABEL $i/$N] CORRUPTION (despite exit 0): $(grep -oE "$CORRUPT_RE" "$LOG" | head -1)"
        else
            pass=$((pass+1)); echo "  [$LABEL $i/$N] PASS"
        fi
    else
        msg="$(grep -E 'EXTINCTION|tests: [0-9]+/[0-9]+ (FAIL|fail)' "$LOG" | head -1)"
        if grep -qE "$CORRUPT_RE" "$LOG"; then
            corrupt=$((corrupt+1)); cp "$LOG" "$FAILDIR/$LABEL-$i-CORRUPT.log"
            echo "  [$LABEL $i/$N] CORRUPTION: ${msg:-<no extinction line>}"
        elif grep -qE "$TIMING_RE" "$LOG"; then
            timing=$((timing+1)); cp "$LOG" "$FAILDIR/$LABEL-$i-TIMING.log"
            echo "  [$LABEL $i/$N] timing (benign host-fragility): ${msg:-?}"
        else
            other=$((other+1)); cp "$LOG" "$FAILDIR/$LABEL-$i-OTHER.log"
            echo "  [$LABEL $i/$N] OTHER fail: ${msg:-<unclassified>}"
        fi
    fi
done

echo "== $LABEL: $pass PASS / $corrupt CORRUPTION / $timing timing / $other other  (N=$N) =="
[[ $corrupt -eq 0 && $other -eq 0 ]]
