#!/bin/sh
# Verify every burrow cfg reports the verdict its header claims.
#
# The shape is specs/check-capacity.sh's: the CLEAN cfg explores the whole
# state space, so its distinct-state count is a deterministic fingerprint (a
# change means the MODEL changed); a BUGGY cfg halts at the first violation, so
# it is judged on its verdict -- the exit status plus the NAME of the invariant
# that fired. Each buggy cfg lists the invariants one by one so TLC names the
# one it broke. The two claim/restore cfgs are the AS-R9 regression: the old
# three-step drop must still be refuted, on the record (NoRecordAfterFree) and,
# judged on the ledger alone, on the refund (RefundExactlyOnce).
#
# What this script CANNOT see, said so the green reads no larger: the model's
# drops are all the payer's own, and shared_out is not modelled -- the kernel
# tests (test_burrow.c's settled-drop tests) are their witness.
set -u
cd "$(dirname "$0")"
export PATH="/opt/homebrew/opt/openjdk/bin:$PATH"
JAR=${TLA_JAR:-/tmp/tla2tools.jar}
TMP=$(mktemp -d) || exit 1
trap 'rm -rf "$TMP"' EXIT
STAMP="$TMP/stamp"; : > "$STAMP"

# clean: cfg, expected distinct states ("-" = do not pin)
CLEAN="burrow:100"

# buggy: cfg, invariant that must be the one reported
BUGGY="burrow_buggy_free_on_close:NoUseAfterFree
burrow_buggy_free_on_unmap:NoUseAfterFree
burrow_buggy_never_free:NoUseAfterFree
burrow_buggy_claim_restore:NoRecordAfterFree
burrow_buggy_claim_restore_ledger:RefundExactlyOnce"

run() {  # $1 = cfg basename -> sets RC and LOG
    LOG="$TMP/$1.log"
    java -cp "$JAR" tlc2.TLC -workers auto -deadlock -metadir "$TMP/$1.meta" \
        -config "$1.cfg" burrow.tla > "$LOG" 2>&1
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
        got=$(grep -o '[0-9]* distinct states found' "$LOG" | tail -1 | awk '{print $1}')
        echo "ok   $cfg: rc=$RC, $want violated as claimed ($got distinct states)"
    fi
done

# A violation drops a <module>_TTrace_<epoch>.{tla,bin} pair beside the spec.
# They are gitignored, but quaestor's spec census counts files: sweep the ones
# THIS run made, and only those.
find . -maxdepth 1 -name 'burrow_TTrace_*' -newer "$STAMP" -exec rm -f {} +

fail=0
[ -f "$TMP/failed" ] && fail=1
echo
if [ "$fail" -eq 0 ]; then echo "burrow: ALL CFGS AS CLAIMED"; else echo "burrow: FAILURES ABOVE"; fi
exit $fail
