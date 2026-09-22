#!/bin/sh
# Verify every syscall_irqs cfg reports the verdict its header claims.
#
# Shape borrowed from check-territory-shed.sh: a CLEAN cfg explores the whole
# state space, so its distinct-state count is a deterministic fingerprint (a
# change means the MODEL changed); a cfg that is supposed to FAIL is judged on
# the NAME of the property that fired, because "something was violated" is not
# the claim any of them makes -- a buggy cfg that starts failing a DIFFERENT
# property has stopped documenting its bug.
#
# ONE ROW IS NOT A BUG, AND READING IT AS ONE INVERTS THE GATE.
# syscall_irqs_kthread is a CONTROL: a kernel thread sets no marker, so the
# same machinery MUST produce an involuntary switch there. Its violation of
# KthreadGetsPreempted IS the pass. It exists because
# NoInvoluntarySwitchInBody is a NEGATIVE invariant, and a negative invariant
# is satisfied in full by a model that cannot do the thing at all. Without
# this row the clean green proves nothing. If someone ever "fixes" the kthread
# row to pass, they have deleted the only evidence that the clean row means
# anything -- and broken #810 besides.
#
# What this script CANNOT see: the model has one CPU and one thread, no locks
# (the lock sweep measured clean -- ARCHITECTURE.md 8.12), and no notion of
# how DEEP a stack gets. The kernel-stack bound is a MEASUREMENT, not a model
# property; its guard is the runtime watermark this chunk adds.
set -u
cd "$(dirname "$0")"
export PATH="/opt/homebrew/opt/openjdk/bin:$PATH"
JAR=${TLA_JAR:-/tmp/tla2tools.jar}
TMP=$(mktemp -d) || exit 1
trap 'rm -rf "$TMP"' EXIT
FAILED=0

# clean: cfg, expected distinct states
CLEAN="syscall_irqs:18
syscall_irqs_liveness:18"

# must-fail: cfg, the property that must be the one reported
MUSTFAIL="syscall_irqs_kthread:KthreadGetsPreempted
syscall_irqs_buggy_marker_ignored:NoInvoluntarySwitchInBody
syscall_irqs_buggy_unmask_before_mark:NoInvoluntarySwitchInBody
syscall_irqs_buggy_late_remask:EretWindowMasked
syscall_irqs_buggy_marker_never_cleared:TailTookItsPreempt
syscall_irqs_buggy_masked_body:CpuGetsItsInterrupts"

# $2 = "checkdeadlock" to OMIT TLC's -deadlock flag (which DISABLES the check).
#
# The clean cfgs run WITH deadlock checking; the must-fail cfgs run without.
# That split is not tidiness, it is the fix for a measured hole. The tail's
# preempt check is modelled as its own step, and OpenEretWindow requires it to
# have run -- so a model that SKIPS the check can never reach the eret window.
# With -deadlock passed, TLC does not report that: it explores the smaller
# graph and prints "No error has been found". Sabotage-measured -- deleting
# TailPreemptCheck from Next passed the gate. Without the flag the same
# sabotage reports "Deadlock reached" at 14 distinct states against the clean
# 18. A must-fail cfg keeps -deadlock, because its job is to violate a NAMED
# property and a deadlock reported first would mask which one.
run() {
    LOG="$TMP/$1.log"
    if [ "${2:-}" = "checkdeadlock" ]; then
        java -cp "$JAR" tlc2.TLC -workers auto -metadir "$TMP/$1.meta" \
            -config "$1.cfg" syscall_irqs.tla > "$LOG" 2>&1
    else
        java -cp "$JAR" tlc2.TLC -workers auto -deadlock -metadir "$TMP/$1.meta" \
            -config "$1.cfg" syscall_irqs.tla > "$LOG" 2>&1
    fi
    RC=$?
}

echo "== clean (must run to completion, deadlock-checked) =="
for row in $CLEAN; do
    cfg=${row%%:*}; want=${row##*:}
    run "$cfg" checkdeadlock
    got=$(grep -oE '[0-9]+ distinct states found' "$LOG" | head -1 | cut -d' ' -f1)
    if [ "$RC" -ne 0 ]; then
        echo "FAIL $cfg: TLC exit $RC (expected a clean run)"; sed -n '/Error/,+6p' "$LOG"; FAILED=1
    elif [ "$got" != "$want" ]; then
        echo "FAIL $cfg: $got distinct states, pinned at $want -- the MODEL changed"; FAILED=1
    else
        echo "ok   $cfg ($got distinct states)"
    fi
done

echo "== must fail, each on its OWN named property =="
for row in $MUSTFAIL; do
    cfg=${row%%:*}; want=${row##*:}
    run "$cfg"
    if [ "$RC" -eq 0 ]; then
        echo "FAIL $cfg: TLC found no error -- this cfg must FAIL on $want"; FAILED=1
        continue
    fi
    got=$(grep -oE "(Invariant|Temporal property) [A-Za-z]+ (is|was) violated" "$LOG" \
          | head -1 | awk '{print $(NF-2)}')
    if [ "$got" != "$want" ]; then
        echo "FAIL $cfg: reported '$got', must report '$want'"; FAILED=1
    else
        echo "ok   $cfg (violates $want, as designed)"
    fi
done

[ "$FAILED" -eq 0 ] && echo "syscall_irqs: all cfgs report their stated verdict" \
                    || echo "syscall_irqs: GATE FAILED"
exit $FAILED
