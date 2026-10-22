#!/bin/sh
# Verify every thread_reap cfg reports the verdict its header claims.
#
# The shape is specs/check-debug-stop.sh's: a CLEAN cfg explores the whole state
# space, so its distinct-state count is a deterministic fingerprint (a change
# means the MODEL changed); a BUGGY cfg halts at the first violation, so it is
# judged on its verdict -- the exit status plus the NAME of the invariant that
# fired. Every run is `-workers 1` and `-lncheck final`, so each count is fixed.
#
# What this script CANNOT see, said so the green reads no larger: the model is
# one Proc (the one-thread-lock-per-CPU rule is a kernel assert), and it has no
# death-wake walk (death_wake.tla) and no reap or free (C1).
set -u
cd "$(dirname "$0")"
export PATH="/opt/homebrew/opt/openjdk/bin:$PATH"
JAR=${TLA_JAR:-/tmp/tla2tools.jar}
TMP=$(mktemp -d) || exit 1
trap 'rm -rf "$TMP"' EXIT
STAMP="$TMP/stamp"; : > "$STAMP"

# clean: cfg, expected distinct states ("-" = do not pin)
CLEAN="thread_reap:158"

# buggy: cfg, invariant that must be the one reported
BUGGY="thread_reap_buggy_admit_claim:NoAdmitAfterClaim
thread_reap_buggy_admit_cap:CapExact
thread_reap_buggy_count:NoOrphanedProc
thread_reap_buggy_spawner:CloseStable
thread_reap_buggy_zombie:ZombieImpliesAllExiting"

run() {  # $1 = cfg basename -> sets RC, LOG and GOT (distinct states)
    LOG="$TMP/$1.log"
    java -cp "$JAR" tlc2.TLC -workers 1 -deadlock -lncheck final -metadir "$TMP/$1.meta" \
        -config "$1.cfg" thread_reap.tla > "$LOG" 2>&1
    RC=$?
    GOT=$(grep -o '[0-9]* distinct states found' "$LOG" | tail -1 | awk '{print $1}')
}

fail() { echo "$1"; echo fail > "$TMP/failed"; }

echo "== clean (must run to completion) =="
echo "$CLEAN" | while IFS=: read -r cfg want; do
    run "$cfg"
    if [ "$RC" -ne 0 ]; then
        fail "FAIL $cfg: rc=$RC (expected 0)"; sed -n '/Error/,+4p' "$LOG" | head -8
    elif [ "$want" != "-" ] && [ "$GOT" != "$want" ]; then
        fail "FAIL $cfg: $GOT distinct states, expected $want -- the model CHANGED"
    else
        echo "ok   $cfg: rc=0, $GOT distinct states"
    fi
done

echo "== buggy (must violate, and violate the NAMED invariant) =="
echo "$BUGGY" | while IFS=: read -r cfg want; do
    run "$cfg"
    if [ "$RC" -eq 0 ]; then
        fail "FAIL $cfg: rc=0 -- the counterexample did NOT fire; $want is unguarded"
    elif ! grep -q "Invariant $want is violated" "$LOG"; then
        fail "FAIL $cfg: rc=$RC but not via $want -- got: $(grep -o -E '(Invariant|property) [A-Za-z]* (is|was) violated' "$LOG" | head -1)"
    else
        echo "ok   $cfg: rc=$RC, $want violated as claimed ($GOT distinct states)"
    fi
done

# A violation drops a <module>_TTrace_<epoch>.{tla,bin} pair beside the spec.
# They are gitignored, but quaestor's spec census counts files: sweep the ones
# THIS run made, and only those.
find . -maxdepth 1 -name 'thread_reap_TTrace_*' -newer "$STAMP" -exec rm -f {} +

status=0
[ -f "$TMP/failed" ] && status=1
echo
if [ "$status" -eq 0 ]; then echo "thread_reap: ALL CFGS AS CLAIMED"; else echo "thread_reap: FAILURES ABOVE"; fi
exit $status
