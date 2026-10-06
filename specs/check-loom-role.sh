#!/bin/sh
# Verify every loom_role cfg reports the verdict its header claims.
#
# The shape is specs/check-tail-order.sh's: a CLEAN cfg explores the whole
# state space, so its distinct-state count is a deterministic fingerprint (a
# change means the MODEL changed); a BUGGY cfg halts at the first violation, so
# it is judged on its verdict -- the exit status plus the NAME of the property
# that fired. A temporal cfg explores the whole space before liveness is
# judged (`-lncheck final`), so its count is pinned too. Every run is `-workers 1`, so each count
# is the one specs/SPEC-TO-CODE.md records.
#
# What this script CANNOT see, said so the green reads no larger: the model
# takes a ready transport to hold a whole frame and leaves out session death
# and the waiter's own stop; the kernel tests 9p_client.loom_enter_* and
# loom.* hold the code to it.
set -u
cd "$(dirname "$0")"
export PATH="/opt/homebrew/opt/openjdk/bin:$PATH"
JAR=${TLA_JAR:-/tmp/tla2tools.jar}
TMP=$(mktemp -d) || exit 1
trap 'rm -rf "$TMP"' EXIT
STAMP="$TMP/stamp"; : > "$STAMP"

# clean: cfg, expected distinct states ("-" = do not pin)
CLEAN="loom_role:32296
loom_role_liveness:32296
loom_role_wide:1297291
loom_role_multi:118774"

# buggy: cfg, invariant that must be the one reported
BUGGY="loom_role_buggy_unready_pump:NoBlindRecv
loom_role_buggy_late_register:NoMissedWake
loom_role_buggy_no_role_wake:NoMissedWake
loom_role_buggy_designates_parked:NoMissedWake
loom_role_buggy_stop_keeps_designation:NoMissedWake
loom_role_buggy_no_ready_hook:NoMissedWake
loom_role_buggy_ready_late_register:NoMissedWake
loom_role_buggy_ready_hook_when_held:NoMissedWake"

# temporal: cfg, the property that must be the one reported, expected distinct
# states ("-" = do not pin)
TEMPORAL="loom_role_buggy_no_role_hook:EnterReturns:34870
loom_role_buggy_first_client_only:EnterReturns:33864"

# Every temporal run is `-lncheck final`: liveness is judged once the whole
# space is explored. Without it TLC checks liveness at TIME-triggered points
# mid-run and stops at the first violation, so the count depended on how fast
# the host ran (measured 2026-10-06: 32796 vs 32868 on two quiet runs of
# loom_role_buggy_no_role_hook; 28333 under load). A pin needs a fixed count.
run() {  # $1 = cfg basename -> sets RC, LOG and GOT (distinct states)
    LOG="$TMP/$1.log"
    java -cp "$JAR" tlc2.TLC -workers 1 -deadlock -lncheck final -metadir "$TMP/$1.meta" \
        -config "$1.cfg" loom_role.tla > "$LOG" 2>&1
    RC=$?
    GOT=$(grep -o '[0-9]* distinct states found' "$LOG" | tail -1 | awk '{print $1}')
    [ -n "${KEEP_LOGS:-}" ] && cp "$LOG" "$KEEP_LOGS/"
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
find . -maxdepth 1 -name 'loom_role_TTrace_*' -newer "$STAMP" -exec rm -f {} +

status=0
[ -f "$TMP/failed" ] && status=1
echo
if [ "$status" -eq 0 ]; then echo "loom_role: ALL CFGS AS CLAIMED"; else echo "loom_role: FAILURES ABOVE"; fi
exit $status
