#!/bin/sh
# Verify every thread_reap cfg reports the verdict its header claims.
#
# The shape is specs/check-cow.sh's: a CLEAN cfg explores the whole state space,
# so its distinct-state count is a deterministic fingerprint (a change means the
# MODEL changed); a BUGGY cfg halts at the first violation, so it is judged on
# its verdict -- the exit status plus the NAME of the invariant that fired.
# Each buggy cfg is built to violate exactly one named invariant.
#
# Counts measured 2026-10-08 on the module as committed at the XT-3b audit
# close (round 1 split the reap into a claim and a commit: 668 -> 808 and
# 4532 -> 5496).
#
# What this script CANNOT see, said so the green reads no larger: the model
# abstracts a Thread's memory to one "freed" state, so it proves WHO may free and
# WHEN, not that the C frees the right bytes; and the walkers of the live list
# are argued in the module header, not stepped. The kernel tests
# (proc.thread_reap_*) and the /thread-torture gate are the witnesses for those.
set -u
cd "$(dirname "$0")"
export PATH="/opt/homebrew/opt/openjdk/bin:$PATH"
JAR=${TLA_JAR:-/tmp/tla2tools.jar}
TMP=$(mktemp -d) || exit 1
trap 'rm -rf "$TMP"' EXIT
STAMP="$TMP/stamp"; : > "$STAMP"

# clean: cfg, expected distinct states ("-" = do not pin)
CLEAN="thread_reap:808
thread_reap_4:5496"

# buggy: cfg, invariant that must be the one reported
BUGGY="thread_reap_buggy_no_oncpu:NoFreeInFlight
thread_reap_buggy_unlocked_claim:OneFreerPerThread
thread_reap_buggy_unlink_at_claim:EveryThreadCounted
thread_reap_buggy_exec_no_drain:TailsOnLiveSpace
thread_reap_buggy_waitpid_skips_retired:TailsOnLiveSpace
thread_reap_buggy_tid_after_ready:NoTidReadAfterFree"

# `-lncheck final`: liveness is judged once the whole space is explored, so the
# clean counts do not depend on how fast the host runs (check-cow.sh measured
# the drift without it).
run() {  # $1 = cfg basename -> sets RC and LOG
    LOG="$TMP/$1.log"
    java -cp "$JAR" tlc2.TLC -workers auto -deadlock -lncheck final -metadir "$TMP/$1.meta" \
        -config "$1.cfg" thread_reap.tla > "$LOG" 2>&1
    RC=$?
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
    elif ! grep -q "Invariant $want is violated" "$LOG"; then
        echo "FAIL $cfg: rc=$RC but not via $want -- got: $(grep -o 'Invariant [A-Za-z]* is violated' "$LOG" | head -1)"
        echo fail > "$TMP/failed"
    else
        echo "ok   $cfg: rc=$RC, $want violated as claimed"
    fi
done

# A violation drops a <module>_TTrace_<epoch>.{tla,bin} pair beside the spec.
# They are gitignored, but quaestor's spec census counts files: sweep the ones
# THIS run made, and only those.
find . -maxdepth 1 -name 'thread_reap_TTrace_*' -newer "$STAMP" -exec rm -f {} +

fail=0
[ -f "$TMP/failed" ] && fail=1
echo
if [ "$fail" -eq 0 ]; then echo "thread_reap: ALL CFGS AS CLAIMED"; else echo "thread_reap: FAILURES ABOVE"; fi
exit $fail
