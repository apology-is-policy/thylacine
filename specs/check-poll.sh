#!/bin/sh
# Verify every poll cfg reports the verdict its header claims.
#
# The shape and the reasoning are specs/check-tapestry.sh's: a CLEAN cfg
# explores the whole state space, so its distinct-state count is a
# deterministic fingerprint (a change means the MODEL changed); a BUGGY cfg
# halts at the first violation, so it is judged on its verdict -- the exit
# status plus the NAME of the invariant that fired. "Something was violated"
# is not the claim any of these cfgs makes: a buggy cfg that starts failing a
# DIFFERENT invariant has stopped documenting its bug.
#
# The liveness properties were shown able to FAIL before being trusted
# (SPEC-TO-CODE.md, the poll.tla section); a liveness cfg is 'clean' here.
# DeathTerminates, StopHonoured and CaughtTerminates also have buggy cfgs of
# their own, judged like the invariant ones: a TEMPORAL violation of the
# named property. The
# documented TLC (SPEC-POLICY's v1.8.0) names it; an older build (2.19, Aug
# 2024) says only "Temporal properties were violated.", and that form is
# accepted only from a cfg that checks that one property and no other.
#
# IrqLatencyBounded and the whole poll_cpu module are GONE (ARCH 8.12). They
# existed because a syscall body ran IRQ-MASKED, so an unprivileged producer
# could hold a CPU's interrupts -- the SAK included -- for as long as it kept a
# poll(-1) awake. Bodies now run interrupts-on throughout: there is no masked
# span here to bound, poll_cpu's stated premise is false so the module went
# VACUOUS rather than wrong, and the CPU-level obligation (whose it always was,
# per round-7 F2) is specs/syscall_irqs.tla's CpuGetsItsInterrupts, gated by
# specs/check-syscall-irqs.sh.
# TLC_WORKERS overrides -workers auto when the host is shared.
set -u
cd "$(dirname "$0")"
export PATH="/opt/homebrew/opt/openjdk/bin:$PATH"
JAR=${TLA_JAR:-/tmp/tla2tools.jar}
TMP=$(mktemp -d) || exit 1
trap 'rm -rf "$TMP"' EXIT
STAMP="$TMP/stamp"; : > "$STAMP"

# clean: cfg, expected distinct states ("-" = do not pin)
CLEAN="poll:-
poll_notimeout:-
poll_liveness:-
poll_liveness_notimeout:-
poll_local:-
poll_armfail:-
poll_armfail_liveness:-
poll_armfail_liveness_notimeout:-"

# buggy: cfg, invariant that must be the one reported
BUGGY="poll_buggy_check_before_register:NoMissedPoll
poll_buggy_no_wake:NoMissedPoll
poll_buggy_clear_after_sample:NoMissedPoll
poll_buggy_lazy_unregister:NoStaleHook
poll_buggy_return_on_wake:NoSpuriousZero
poll_buggy_no_loop_die_check:DeathTerminates
poll_buggy_no_loop_stop_check:StopHonoured
poll_buggy_verdict_before_settle:NoFalseNotReady
poll_buggy_sweep_leaves_snapshot:NoSnapshotOutlivesCall
poll_buggy_no_retry:NoMissedPoll
poll_buggy_retry_is_timeout:NoSpuriousZero
poll_buggy_no_loop_caught_check:CaughtTerminates
poll_buggy_caught_before_ready:EintrNotOverReady
poll_buggy_deadline_before_caught:NoZeroOverCaught"

run() {  # $1 = cfg basename -> sets RC and LOG
    LOG="$TMP/$1.log"
    MOD=poll
    java -cp "$JAR" tlc2.TLC -workers "${TLC_WORKERS:-auto}" -deadlock -metadir "$TMP/$1.meta" \
        -config "$1.cfg" "$MOD.tla" > "$LOG" 2>&1
    RC=$?
}

# Did $LOG violate $2, the property buggy cfg $1 claims? An invariant always
# names itself; a temporal violation is $2's only if $1 checks nothing else.
violated_as_claimed() {
    grep -qE "Invariant $2 is violated|Temporal property $2 was violated" "$LOG" && return 0
    grep -q 'Temporal properties were violated' "$LOG" || return 1
    props=$(awk '/^\\\*/{next} /^PROPERT(Y|IES)/{p=1;next} /^[A-Z]/{p=0} p && NF{print $1}' "$1.cfg")
    [ "$props" = "$2" ]
}

# The counterexample's actions in order: the mechanism, not just the verdict.
path() {
    grep -oE '^State [0-9]+: <[A-Za-z]+|^Back to state [0-9]+|^State [0-9]+: Stuttering' "$LOG" \
        | sed -E 's/^State [0-9]+: <//; s/^State [0-9]+: Stuttering/(stutter)/; s/^Back to state ([0-9]+)/(loop to \1)/' \
        | grep -v '^Initial' | tr '\n' ' '
}

echo "== clean (must run to completion) =="
echo "$CLEAN" | while IFS=: read -r cfg want; do
    run "$cfg"
    got=$(grep -o '[0-9]* distinct states found' "$LOG" | tail -1 | awk '{print $1}')
    if [ "$RC" -ne 0 ]; then
        echo "FAIL $cfg: rc=$RC (expected 0)"; sed -n '/Error/,+4p' "$LOG" | head -8
        echo fail > "$TMP/failed"
    elif [ "$want" != "-" ] && [ "$got" != "$want" ]; then
        echo "FAIL $cfg: $got distinct states, expected $want -- the model CHANGED"
        echo fail > "$TMP/failed"
    else
        echo "ok   $cfg: rc=0, $got distinct states"
    fi
done

echo "== buggy (must violate, and violate the NAMED invariant) =="
echo "$BUGGY" | while IFS=: read -r cfg want; do
    run "$cfg"
    if [ "$RC" -eq 0 ]; then
        echo "FAIL $cfg: rc=0 -- the counterexample did NOT fire; $want is unguarded"
        echo fail > "$TMP/failed"
    elif ! violated_as_claimed "$cfg" "$want"; then
        echo "FAIL $cfg: rc=$RC but not via $want -- got: $(grep -oE 'Invariant [A-Za-z]* is violated|Temporal propert[a-z]* [A-Za-z ]*violated' "$LOG" | head -1)"
        echo fail > "$TMP/failed"
    else
        echo "ok   $cfg: rc=$RC, $want violated as claimed"
        echo "       path: $(path)"
    fi
done

# A violation drops a <module>_TTrace_<epoch>.{tla,bin} pair beside the spec.
# They are gitignored, but quaestor's spec census counts files: sweep the ones
# THIS run made, and only those.
find . -maxdepth 1 -name 'poll_TTrace_*' -newer "$STAMP" -exec rm -f {} +

fail=0
[ -f "$TMP/failed" ] && fail=1
echo
if [ "$fail" -eq 0 ]; then echo "poll: ALL CFGS AS CLAIMED"; else echo "poll: FAILURES ABOVE"; fi
exit $fail
