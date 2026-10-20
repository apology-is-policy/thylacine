#!/bin/sh
# Verify every caught_target cfg reports the verdict its header claims.
#
# The shape is specs/check-debug-stop.sh's: a CLEAN cfg explores the whole
# state space, so its distinct-state count is a deterministic fingerprint (a
# change means the MODEL changed); a BUGGY cfg halts at the first violation, so
# it is judged on its verdict -- the exit status plus the NAME of the property
# that fired. The temporal cfg explores the whole space before liveness is
# judged (`-lncheck final`), so its count is pinned too. Every run is `-workers 1`, so each count
# is the one specs/SPEC-TO-CODE.md records.
#
# What this script CANNOT see, said so the green reads no larger: the model
# lets the walk pick ANY candidate, so the preference order (the poster, main,
# a running thread, then a sleeper) is pinned by the kernel tests and the
# probe legs, not here; one note family; the leg order inside a tail is
# tail_order.tla's.
set -u
cd "$(dirname "$0")"
export PATH="/opt/homebrew/opt/openjdk/bin:$PATH"
JAR=${TLA_JAR:-/tmp/tla2tools.jar}
TMP=$(mktemp -d) || exit 1
trap 'rm -rf "$TMP"' EXIT
STAMP="$TMP/stamp"; : > "$STAMP"

# clean: cfg, expected distinct states ("-" = do not pin)
CLEAN="caught_target:613245"

# buggy: cfg, invariant that must be the one reported
BUGGY="caught_target_buggy_double_designate:OneClaimant"

# temporal: cfg, the property that must be the one reported, expected distinct
# states ("-" = do not pin)
TEMPORAL="caught_target_buggy_no_handoff:NoStrand:611617
caught_target_buggy_unlocked_designate:NoStrand:632863
caught_target_buggy_no_rekick:NoStrand:613245
caught_target_buggy_exit_keeps:NoStrand:639673
caught_target_buggy_release_no_wake:NoStrand:165597
caught_target_buggy_irq_no_notes:NoStrand:613245
caught_target_buggy_stale_keeps:NoStrand:616829"

# Every temporal run is `-lncheck final`: liveness is judged once the whole
# space is explored. Without it TLC checks liveness at TIME-triggered points
# mid-run and stops at the first violation, so the count depended on how fast
# the host ran (measured 2026-10-06: 32796 vs 32868 on two quiet runs of
# loom_role_buggy_no_role_hook; 28333 under load). A pin needs a fixed count.
run() {  # $1 = cfg basename -> sets RC, LOG and GOT (distinct states)
    LOG="$TMP/$1.log"
    java -cp "$JAR" tlc2.TLC -workers 1 -deadlock -lncheck final -metadir "$TMP/$1.meta" \
        -config "$1.cfg" caught_target.tla > "$LOG" 2>&1
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
find . -maxdepth 1 -name 'caught_target_TTrace_*' -newer "$STAMP" -exec rm -f {} +

status=0
[ -f "$TMP/failed" ] && status=1
echo
if [ "$status" -eq 0 ]; then echo "caught_target: ALL CFGS AS CLAIMED"; else echo "caught_target: FAILURES ABOVE"; fi
exit $status
