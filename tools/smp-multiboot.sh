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
#   INJECT-MISS -- the harness's QMP key-injector failed to deliver the
#                  virtio-input event (#362): the guest reached AWAITING_QMP_
#                  KEY, SKIPped cleanly, and the boot is PROVEN green (banner
#                  present, no extinction, 0 test FAILs) -- test.sh then fails
#                  the boot at its injection-enforcement gate. A host-side
#                  delivery artifact, NOTED, does not fail the gate. The
#                  classification requires the full green-guest proof; a boot
#                  that merely also missed injection stays CORRUPTION/OTHER.
#   TIMING      -- a benign host-fragility soft-warn: anchored on EMITTED warn
#                  strings ('[SOFT-WARN]', the irq-bench CI-budget text) --
#                  NEVER on test names. The pre-#362 regex contained
#                  'stalk.*lifetime', which matched the PASSING test-name line
#                  '[test] stalk.lifetime_no_leak ... PASS' present in every
#                  boot log, making TIMING a catch-all that absorbed ANY
#                  nonzero exit (23/40 inject-misses were buried here -- and a
#                  real unclassified failure would have been too).
#   OTHER       -- an unclassified nonzero exit; surfaced for investigation
#                  and FAILS the gate (feedback_no_host_load: an unexplained
#                  red is surfaced, never absorbed).
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
# Clear THIS label's prior captures so the dir only ever reflects the latest
# run of each label -- stale fail logs from a previous (or since-fixed-buggy)
# run otherwise masquerade as current findings. Per-label (not whole-dir) so a
# gate running multiple labels back-to-back does not wipe a sibling's captures.
rm -f "$FAILDIR/$LABEL-"*.log 2>/dev/null || true

# Signatures of a real ctx/stack corruption (the SMP soundness bug class).
# Use the EXACT extinction strings -- bare "canary" would match the benign
# "canaries" hardening banner + "canary: initialized" boot line (false positive).
CORRUPT_RE='invalid prev state|stack canary mismatch|kernel stack overflow|already on_cpu|#860|not RUNNABLE-and-off-cpu|corrupted current|sched: deadlock'
# Benign host-timing fragility: EMITTED warn strings only, never test names
# (#362 -- see the header). The only current emitter is TEST_SOFT_WARN
# ('[SOFT-WARN] IRQ-to-userspace p99 exceeds CI sanity budget'), which alone
# cannot fail a boot, so this class should stay near-empty.
TIMING_RE='\[SOFT-WARN\]|exceeds CI sanity budget'
# The #362 inject-miss green-guest proof (ALL must hold to classify INJECT).
INJ_SENTINEL='virtio-input: AWAITING_QMP_KEY'
INJ_SKIP='virtio-input: SKIP'
BOOT_OK_RE='Thylacine boot OK'
SUITE_FAIL_RE='tests: [0-9]+/[0-9]+ (FAIL|fail)'
INJECT_LOG="$REPO_ROOT/build/test-inject.log"

# The guest is provably green AND the only defect is the missed key delivery.
inject_miss_green() {
    grep -qF "$INJ_SENTINEL" "$LOG" \
        && grep -qF "$INJ_SKIP" "$LOG" \
        && grep -q "$BOOT_OK_RE" "$LOG" \
        && ! grep -q '^EXTINCTION:' "$LOG" \
        && ! grep -qE "$SUITE_FAIL_RE" "$LOG"
}

# Per-boot pool restore (#362): every boot's go4c probes write GOCACHE/$WORK
# into the Stratum pool with ~6x CoW amplification (#39 -- garbage only a
# commit sweeps), so N cumulative boots age the pool (later boots slow toward
# the timeout, and a long matrix would eventually ENOSPC -> false reds). Each
# boot starts from the baked snapshot instead -- also making per-boot timing
# comparable. cp -c is an APFS clonefile (instant CoW); falls back to a plain
# copy elsewhere. The key twin is validated coherent (the ramfs bakes the key,
# so ONLY the matching pool may be restored). SMP_GATE_POOL_RESTORE=0 opts out.
POOL_IMG="$REPO_ROOT/build/fixtures/pool.img"
POOL_SNAP="$POOL_IMG.baked-snapshot"
KEY_IMG="$REPO_ROOT/build/fixtures/system.key"
KEY_SNAP="$KEY_IMG.baked-snapshot"
pool_restore() {
    [[ "${SMP_GATE_POOL_RESTORE:-1}" == "0" ]] && return 0
    [[ -f "$POOL_SNAP" && -f "$POOL_IMG" ]] || return 0
    if ! cmp -s "$KEY_IMG" "$KEY_SNAP" 2>/dev/null; then
        echo "  [$LABEL] pool restore SKIPPED: system.key does not match its snapshot (stale twins?)" >&2
        return 0
    fi
    cp -c "$POOL_SNAP" "$POOL_IMG" 2>/dev/null || cp "$POOL_SNAP" "$POOL_IMG"
}

pass=0; corrupt=0; inject=0; timing=0; other=0
for i in $(seq 1 "$N"); do
    pool_restore
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
        elif inject_miss_green; then
            inject=$((inject+1)); cp "$LOG" "$FAILDIR/$LABEL-$i-INJECT.log"
            cp "$INJECT_LOG" "$FAILDIR/$LABEL-$i-INJECT-injector.log" 2>/dev/null || true
            echo "  [$LABEL $i/$N] inject-miss (harness delivery; guest green)"
        elif grep -qE "$TIMING_RE" "$LOG"; then
            timing=$((timing+1)); cp "$LOG" "$FAILDIR/$LABEL-$i-TIMING.log"
            echo "  [$LABEL $i/$N] timing (benign host-fragility): ${msg:-?}"
        else
            other=$((other+1)); cp "$LOG" "$FAILDIR/$LABEL-$i-OTHER.log"
            echo "  [$LABEL $i/$N] OTHER fail: ${msg:-<unclassified>}"
        fi
    fi
done

echo "== $LABEL: $pass PASS / $corrupt CORRUPTION / $inject inject-miss / $timing timing / $other other  (N=$N) =="
[[ $corrupt -eq 0 && $other -eq 0 ]]
