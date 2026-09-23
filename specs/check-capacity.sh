#!/bin/sh
# Verify every capacity cfg reports the verdict its header claims.
#
# The shape is specs/check-cow.sh's: the CLEAN cfg explores the whole state
# space, so its distinct-state count is a deterministic fingerprint (a change
# means the MODEL changed); a BUGGY cfg halts at the first violation, so it is
# judged on its verdict -- the exit status plus the NAME of the invariant that
# fired. Both buggy cfgs list ChargeConserved AHEAD of NoOrphan and must be
# judged on NoOrphan: the orphan is a slot that lost its mapping while still
# resident, so page_count still equals the resident count and the counter
# cannot see it -- that blindness is the whole reason NoOrphan exists, and a
# run that reported ChargeConserved instead would mean the model no longer
# says so.
#
# What this script CANNOT see, said so the green reads no larger: the model
# has one address space and no metadata. The pagemap's node pages ride the
# slots they index in the code (charged on install, uncharged on the take that
# empties them), the pool is a second bound above page_count, and a dying
# address space's pool return is a path with no counterpart here -- the kernel
# tests (test_capacity.c) are their witness.
set -u
cd "$(dirname "$0")"
export PATH="/opt/homebrew/opt/openjdk/bin:$PATH"
JAR=${TLA_JAR:-/tmp/tla2tools.jar}
TMP=$(mktemp -d) || exit 1
trap 'rm -rf "$TMP"' EXIT
STAMP="$TMP/stamp"; : > "$STAMP"

# clean: cfg, expected distinct states ("-" = do not pin)
CLEAN="capacity:625"

# buggy: cfg, invariant that must be the one reported
BUGGY="capacity_buggy_replace_orphans:NoOrphan
capacity_buggy_detach_no_refund:NoOrphan"

run() {  # $1 = cfg basename -> sets RC and LOG
    LOG="$TMP/$1.log"
    java -cp "$JAR" tlc2.TLC -workers auto -deadlock -metadir "$TMP/$1.meta" \
        -config "$1.cfg" capacity.tla > "$LOG" 2>&1
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
find . -maxdepth 1 -name 'capacity_TTrace_*' -newer "$STAMP" -exec rm -f {} +

fail=0
[ -f "$TMP/failed" ] && fail=1
echo
if [ "$fail" -eq 0 ]; then echo "capacity: ALL CFGS AS CLAIMED"; else echo "capacity: FAILURES ABOVE"; fi
exit $fail
