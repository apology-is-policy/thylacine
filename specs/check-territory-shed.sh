#!/bin/sh
# Verify every territory_shed cfg reports the verdict its header claims.
#
# The shape and the reasoning are specs/check-tapestry.sh's: a CLEAN cfg
# explores the whole state space, so its distinct-state count is a
# deterministic fingerprint (a change means the MODEL changed); a BUGGY cfg
# halts at the first violation, so it is judged on its verdict -- the exit
# status plus the NAME of the invariant that fired. "Something was violated"
# is not the claim any of these cfgs makes: a buggy cfg that starts failing a
# DIFFERENT invariant has stopped documenting its bug.
#
# What this script CANNOT see, said so the green reads no larger (audit r2 F2):
# a seed missing from BOTH the rule (Seeds) and the walker (TrueStart) is
# invisible by construction. The guard against that is prose -- AUDIT-TRIGGERS
# 'Mount-table SHED' item (11) and the WHY comments at kernel/stalk.c's two
# consults of union_snap->point -- not this script.
set -u
cd "$(dirname "$0")"
export PATH="/opt/homebrew/opt/openjdk/bin:$PATH"
JAR=${TLA_JAR:-/tmp/tla2tools.jar}
TMP=$(mktemp -d) || exit 1
trap 'rm -rf "$TMP"' EXIT
STAMP="$TMP/stamp"; : > "$STAMP"

# clean: cfg, expected distinct states ("-" = do not pin)
CLEAN="territory_shed:744864
territory_shed_perwalker:793408"

# buggy: cfg, invariant that must be the one reported
BUGGY="territory_shed_buggy_nontransitive:ShedLosesNothing
territory_shed_buggy_no_union_seed:ShedLosesNothing
territory_shed_buggy_undeclared_per_walker:ShedLosesNothing
territory_shed_buggy_dotdot_escapes:ShedLosesNothing
territory_shed_buggy_keeps_all:NoResidueAfterPivot"

run() {  # $1 = cfg basename -> sets RC and LOG
    LOG="$TMP/$1.log"
    java -cp "$JAR" tlc2.TLC -workers auto -deadlock -metadir "$TMP/$1.meta" \
        -config "$1.cfg" territory_shed.tla > "$LOG" 2>&1
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
find . -maxdepth 1 -name 'territory_shed_TTrace_*' -newer "$STAMP" -exec rm -f {} +

fail=0
[ -f "$TMP/failed" ] && fail=1
echo
if [ "$fail" -eq 0 ]; then echo "territory_shed: ALL CFGS AS CLAIMED"; else echo "territory_shed: FAILURES ABOVE"; fi
exit $fail
