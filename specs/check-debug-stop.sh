#!/bin/sh
# Verify every debug_stop cfg reports the verdict its header claims.
#
# The shape is specs/check-cow.sh's: a CLEAN cfg explores the whole state
# space, so its distinct-state count is a deterministic fingerprint (a change
# means the MODEL changed); a BUGGY cfg halts at the first violation, so it is
# judged on its verdict -- the exit status plus the NAME of the property that
# fired. Four families: invariants, action properties (NoEretIntoDeath,
# ParkEndsOnlyInDeath), and temporal properties, whose cfgs explore the whole
# space before liveness is judged, so their counts are pinned too.
#
# Every run is `-workers 1`, so each printed count is the one
# specs/SPEC-TO-CODE.md records: a multi-worker run stops a safety violation
# at a scheduling-dependent count.
#
# What this script CANNOT see, said so the green reads no larger: the model
# has no note delivery, so what a Thread does with its latch once it runs
# again is not checked here -- the kernel tests and /debug-probe's interrupt
# leg are its witness (DEBUG-FS-DESIGN 5g).
set -u
cd "$(dirname "$0")"
export PATH="/opt/homebrew/opt/openjdk/bin:$PATH"
JAR=${TLA_JAR:-/tmp/tla2tools.jar}
TMP=$(mktemp -d) || exit 1
trap 'rm -rf "$TMP"' EXIT
STAMP="$TMP/stamp"; : > "$STAMP"

# clean: cfg, expected distinct states ("-" = do not pin)
CLEAN="debug_stop:12830
debug_stop_held:17330"

# buggy: cfg, invariant that must be the one reported
BUGGY="debug_stop_buggy_lost_stop:NoLostStop
debug_stop_buggy_double_wake:ExactlyOnceResume
debug_stop_buggy_fault_stop_ungated:StopImpliesOwned
debug_stop_buggy_held_runs_free:NoEL0WhileHeld
debug_stop_buggy_convert_clears_first:NoEL0WhileHeld
debug_stop_buggy_no_death_recheck:NoEL0WhileHeld
debug_stop_buggy_birth_latch_erets:NoEL0WhileHeld
debug_stop_buggy_tail_latch_erets:NoLostStop
debug_stop_buggy_spawner_latch_returns:SpawnReturnsAfterBirth"

# action: cfg, the action property that must be the one reported
ACTION="debug_stop_buggy_no_death_recheck_tail:NoEretIntoDeath
debug_stop_buggy_latch_ends_stop:ParkEndsOnlyInDeath"

# temporal: cfg, the property that must be the one reported, expected distinct
# states ("-" = do not pin)
TEMPORAL="debug_stop_buggy_park_before_die:EventuallyAllDead:1480
debug_stop_buggy_strand_on_debugger_death:EventuallyResumed:1338
debug_stop_buggy_exitkill_ignored:EventuallyLaunchedDies:1516
debug_stop_buggy_stop_skips_sleeper:EventuallyStopSettles:12830
debug_stop_buggy_orphan_hold_strands:EventuallyHoldResolved:17872
debug_stop_buggy_birth_wait_unwoken:BirthWaitReleases:19678"

run() {  # $1 = cfg basename -> sets RC, LOG and GOT (distinct states)
    LOG="$TMP/$1.log"
    java -cp "$JAR" tlc2.TLC -workers 1 -deadlock -metadir "$TMP/$1.meta" \
        -config "$1.cfg" debug_stop.tla > "$LOG" 2>&1
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

echo "== action (must violate, and violate the NAMED action property) =="
echo "$ACTION" | while IFS=: read -r cfg want; do
    run "$cfg"
    if [ "$RC" -eq 0 ]; then
        fail "FAIL $cfg: rc=0 -- the counterexample did NOT fire; $want is unguarded"
    elif ! grep -q "Action property $want is violated" "$LOG"; then
        fail "FAIL $cfg: rc=$RC but not via $want -- got: $(grep -o -E '(Invariant|property) [A-Za-z]* (is|was) violated' "$LOG" | head -1)"
    else
        echo "ok   $cfg: rc=$RC, $want violated as claimed ($GOT distinct states)"
    fi
done

echo "== temporal (the liveness property must fail) =="
echo "$TEMPORAL" | while IFS=: read -r cfg prop want; do
    run "$cfg"
    if grep -q "Invariant [A-Za-z]* is violated" "$LOG"; then
        fail "FAIL $cfg: a SAFETY invariant fired -- this cfg documents a hang, not an unsafe state"
    elif ! grep -q "Temporal property $prop was violated" "$LOG"; then
        fail "FAIL $cfg: rc=$RC, $prop not reported violated -- got: $(grep -o 'Temporal property [A-Za-z]* was violated' "$LOG" | head -1)"
    elif [ "$want" != "-" ] && [ "$GOT" != "$want" ]; then
        fail "FAIL $cfg: $GOT distinct states, expected $want -- the model CHANGED"
    else
        echo "ok   $cfg: rc=$RC, $prop violated as claimed, $GOT distinct states"
    fi
done

# A violation drops a <module>_TTrace_<epoch>.{tla,bin} pair beside the spec.
# They are gitignored, but quaestor's spec census counts files: sweep the ones
# THIS run made, and only those.
find . -maxdepth 1 -name 'debug_stop_TTrace_*' -newer "$STAMP" -exec rm -f {} +

status=0
[ -f "$TMP/failed" ] && status=1
echo
if [ "$status" -eq 0 ]; then echo "debug_stop: ALL CFGS AS CLAIMED"; else echo "debug_stop: FAILURES ABOVE"; fi
exit $status
