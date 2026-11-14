#!/bin/sh
# Verify every payer cfg reports the verdict its header claims.
#
# The shape is specs/check-capacity.sh's: the CLEAN cfg explores the whole
# state space, so its distinct- and generated-state counts are a deterministic
# fingerprint (a change means the MODEL or an ACTION changed); a BUGGY cfg
# halts at the first violation and is judged on its verdict -- the exit status
# plus the NAME of the invariant that fired. Three cfgs list laws that must
# HOLD ahead of the one they are judged on, and a report naming one of those
# would mean the model no longer says what its header says:
#   loser_keeps_charge  ChargeExact ahead of RecordIsResidency -- the payer
#                       pays the record exactly; the record counts a page
#                       that does not exist.
#   unmap_releases      RecordIsResidency and ChargeExact ahead of
#                       NoFreedLeaf -- the books stay exact while a page a
#                       peer still maps is freed; no counter can see it.
#   unmap_lapses        ChargeExact ahead of LapseOnlyAtDeath -- the books
#                       balance; only the vote's rule is broken.
#
# What this script CANNOT see, said so the green reads no larger: one Burrow;
# no cap, exemption, node pages or pool (they bound how much is charged, not
# to whom); the descriptor's lifetime (the lapse before the last lifetime
# drop is the host fixture's, tools/host-tests/addrspace-lifetime.c); and no
# geometry below "which slots a space maps" (splits, protects and the payer
# flag's carriage through them are the kernel tests').
set -u
cd "$(dirname "$0")"
export PATH="/opt/homebrew/opt/openjdk/bin:$PATH"
JAR=${TLA_JAR:-/tmp/tla2tools.jar}
TMP=$(mktemp -d) || exit 1
trap 'rm -rf "$TMP"' EXIT
STAMP="$TMP/stamp"; : > "$STAMP"

# clean: cfg, expected distinct states, expected generated states ("-" = do not pin)
CLEAN="payer:15603:89443"

# buggy: cfg, invariant that must be the one reported
BUGGY="payer_buggy_handover_no_owed:ChargeExact
payer_buggy_no_adopt:ChargeExact
payer_buggy_fork_copies_payer:ChargeExact
payer_buggy_loser_keeps_charge:RecordIsResidency
payer_buggy_unmap_releases:NoFreedLeaf
payer_buggy_no_lapse:PayerIsLive
payer_buggy_unmap_lapses:LapseOnlyAtDeath"

run() {  # $1 = cfg basename -> sets RC and LOG
    LOG="$TMP/$1.log"
    java -cp "$JAR" tlc2.TLC -workers auto -deadlock -metadir "$TMP/$1.meta" \
        -config "$1.cfg" payer.tla > "$LOG" 2>&1
    RC=$?
}

echo "== clean (must run to completion) =="
echo "$CLEAN" | while IFS=: read -r cfg want wantgen; do
    run "$cfg"
    got=$(grep -o '[0-9]* distinct states found' "$LOG" | tail -1 | awk '{print $1}')
    gen=$(grep -o '[0-9]* states generated' "$LOG" | tail -1 | awk '{print $1}')
    if [ "$RC" -ne 0 ]; then
        echo "FAIL $cfg: rc=$RC (expected 0)"; sed -n '/Error/,+4p' "$LOG" | head -8
        echo fail > "$TMP/failed"
    elif [ "$want" != "-" ] && [ "$got" != "$want" ]; then
        echo "FAIL $cfg: $got distinct states, expected $want -- the model CHANGED"
        echo fail > "$TMP/failed"
    elif [ "$wantgen" != "-" ] && [ "$gen" != "$wantgen" ]; then
        echo "FAIL $cfg: $gen states generated, expected $wantgen -- an action CHANGED"
        echo fail > "$TMP/failed"
    else
        echo "ok   $cfg: rc=0, $got distinct states, $gen generated"
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
find . -maxdepth 1 -name 'payer_TTrace_*' -newer "$STAMP" -exec rm -f {} +

fail=0
[ -f "$TMP/failed" ] && fail=1
echo
if [ "$fail" -eq 0 ]; then echo "payer: ALL CFGS AS CLAIMED"; else echo "payer: FAILURES ABOVE"; fi
exit $fail
