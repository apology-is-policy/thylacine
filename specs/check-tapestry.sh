#!/bin/sh
# Verify every tapestry_present cfg reports the verdict its header claims.
#
# Two classes, two different metrics, because they are not the same
# measurement. A CLEAN cfg explores the whole state space, so its
# distinct-state count is a deterministic fingerprint and a change in it means
# the model changed. A BUGGY cfg halts at the FIRST violation, so with parallel
# workers "states explored before tripping" is scheduler noise -- measured
# varying 129/141/155 across three identical runs -- and asserting on it would
# be asserting on the instrument. A buggy cfg is judged on its VERDICT: the
# exit status, plus the NAME of the invariant that fired. "Something was
# violated" is not the claim any of these cfgs makes.
#
# Exit status is the oracle, never the prose: TLC writes both "is violated"
# and "was violated" depending on whether the property is a safety invariant
# or a temporal one, and a grep for one of them reports a false RED on a
# perfect spec.
set -u
cd "$(dirname "$0")"
export PATH="/opt/homebrew/opt/openjdk/bin:$PATH"
JAR=${TLA_JAR:-/tmp/tla2tools.jar}
TMP=$(mktemp -d) || exit 1
trap 'rm -rf "$TMP"' EXIT
fail=0

# clean: cfg, expected distinct states ("-" = do not pin)
CLEAN="tapestry_present:5413
tapestry_present_liveness:5413
tapestry_present_composed:94680
tapestry_present_composed_liveness:94680
tapestry_present_presentable:1557073
tapestry_present_presentable_liveness:1557073"

# buggy: cfg, invariant that must be the one reported
BUGGY="tapestry_present_buggy_premature_reuse:RecycleGate
tapestry_present_buggy_retire_during_transfer:NoTornScanout
tapestry_present_buggy_reweave_without_quiesce:NoTornScanout
tapestry_present_buggy_map_after_retire:NoStaleMap
tapestry_present_buggy_drain_skipped:NoTornCompose
tapestry_present_buggy_blit_during_fill:NoStaleCompose
tapestry_present_buggy_fill_during_blit:NoStaleCompose
tapestry_present_buggy_readback_free:NoTornReadback
tapestry_present_buggy_punbind_skipped:NoTornPresentable
tapestry_present_buggy_pdrain_skipped:NoTornPresentable"

run() {  # $1 = cfg basename -> sets RC and LOG
    LOG="$TMP/$1.log"
    java -cp "$JAR" tlc2.TLC -workers auto -deadlock \
        -config "$1.cfg" tapestry_present.tla > "$LOG" 2>&1
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

[ -f "$TMP/failed" ] && fail=1
echo
if [ "$fail" -eq 0 ]; then echo "tapestry_present: ALL CFGS AS CLAIMED"; else echo "tapestry_present: FAILURES ABOVE"; fi
exit $fail
