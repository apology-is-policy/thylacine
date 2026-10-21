#!/bin/sh
# Verify every dev9p.poll cfg -- net_poll (the SAMPLE/ARM protocol) and
# net_poll_teardown (the arm's cancel-at-close) -- reports the verdict its
# header claims.
#
# The shape and the reasoning are specs/check-poll.sh's: a CLEAN cfg explores
# the whole state space (a liveness cfg is 'clean' too); a RED cfg halts at the
# first violation and is judged on its verdict -- the exit status plus the NAME
# of the property that fired. A red cfg that starts failing a DIFFERENT property
# has stopped documenting its bug.
#
# net_poll_failsafe_fires is red on purpose and is not a bug: against a hung
# server the fail-safe must fire, and FailSafeSilent failing is the proof that
# it is reachable. A counter that nothing can make move proves nothing.
# TLC_WORKERS overrides -workers auto when the host is shared.
set -u
cd "$(dirname "$0")"
export PATH="/opt/homebrew/opt/openjdk/bin:$PATH"
JAR=${TLA_JAR:-/tmp/tla2tools.jar}
TMP=$(mktemp -d) || exit 1
trap 'rm -rf "$TMP"' EXIT
STAMP="$TMP/stamp"; : > "$STAMP"

# clean: module, cfg
CLEAN="net_poll:net_poll
net_poll:net_poll_notimeout
net_poll:net_poll_liveness
net_poll:net_poll_liveness_timeout
net_poll:net_poll_hung
net_poll:net_poll_armfail
net_poll:net_poll_armfail_liveness
net_poll:net_poll_armfail_liveness_timeout
net_poll_teardown:net_poll_teardown
net_poll_teardown:net_poll_teardown_liveness"

# red: module, cfg, the property that must be the one reported
RED="net_poll:net_poll_failsafe_fires:FailSafeSilent
net_poll:net_poll_buggy_cache_only_sample:NoFalseNotReady
net_poll:net_poll_buggy_stale_cache:NoFalseReady
net_poll:net_poll_buggy_settle_cut_by_deadline:NoFalseNotReady
net_poll:net_poll_buggy_gc_snapshot:NoFalseNotReady
net_poll:net_poll_buggy_lost_ready:NoMissedNetPoll
net_poll:net_poll_buggy_edge_arm:PollerEventuallyServed
net_poll:net_poll_buggy_no_retry:NoMissedNetPoll
net_poll_teardown:net_poll_teardown_buggy_leak:Liveness
net_poll_teardown:net_poll_teardown_buggy_split_gc:Liveness
net_poll_teardown:net_poll_teardown_buggy_no_closer:Liveness"

run() {  # $1 = module, $2 = cfg basename -> sets RC and LOG
    LOG="$TMP/$2.log"
    java -cp "$JAR" tlc2.TLC -workers "${TLC_WORKERS:-auto}" -deadlock -metadir "$TMP/$2.meta" \
        -config "$2.cfg" "$1.tla" > "$LOG" 2>&1
    RC=$?
}

# Did $LOG violate $2, the property red cfg $1 claims? An invariant always
# names itself, and so does a temporal property under the documented TLC
# (SPEC-POLICY's v1.8.0). An older build (2.19, Aug 2024) says only "Temporal
# properties were violated.", which identifies $2 only when the cfg checks no
# other property -- so that is what is required of it.
violated_as_claimed() {
    grep -qE "Invariant $2 is violated|Temporal property $2 was violated" "$LOG" && return 0
    grep -q 'Temporal properties were violated' "$LOG" || return 1
    props=$(awk '/^\\\*/{next} /^PROPERT(Y|IES)/{p=1;next} /^[A-Z]/{p=0} p && NF{print $1}' "$1.cfg")
    [ "$props" = "$2" ]
}

# The actions of the counterexample, in order -- the mechanism, not just the
# verdict, so a red cfg that fires by an unintended path is visible.
path() {
    grep -oE '^State [0-9]+: <[A-Za-z]+|^Back to state [0-9]+|^State [0-9]+: Stuttering' "$LOG" \
        | sed -E 's/^State [0-9]+: <//; s/^State [0-9]+: Stuttering/(stutter)/; s/^Back to state ([0-9]+)/(loop to \1)/' \
        | grep -v '^Initial' | tr '\n' ' '
}

echo "== clean (must run to completion) =="
echo "$CLEAN" | while IFS=: read -r mod cfg; do
    run "$mod" "$cfg"
    got=$(grep -o '[0-9]* distinct states found' "$LOG" | tail -1 | awk '{print $1}')
    if [ "$RC" -ne 0 ]; then
        echo "FAIL $cfg: rc=$RC (expected 0)"; sed -n '/Error/,+4p' "$LOG" | head -8
        echo fail > "$TMP/failed"
    elif [ -z "$got" ]; then
        echo "FAIL $cfg: rc=0 but no state count -- did TLC run at all?"
        echo fail > "$TMP/failed"
    else
        echo "ok   $cfg: rc=0, $got distinct states"
    fi
done

echo "== red (must violate, and violate the NAMED property) =="
echo "$RED" | while IFS=: read -r mod cfg want; do
    run "$mod" "$cfg"
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
find . -maxdepth 1 -name 'net_poll*_TTrace_*' -newer "$STAMP" -exec rm -f {} +

fail=0
[ -f "$TMP/failed" ] && fail=1
echo
if [ "$fail" -eq 0 ]; then echo "net_poll: ALL CFGS AS CLAIMED"; else echo "net_poll: FAILURES ABOVE"; fi
exit $fail
