#!/bin/sh
# Verify every cow cfg reports the verdict its header claims.
#
# The shape is specs/check-territory-shed.sh's: a CLEAN cfg explores the whole
# state space, so its distinct-state count is a deterministic fingerprint (a
# change means the MODEL changed); a BUGGY cfg halts at the first violation,
# so it is judged on its verdict -- the exit status plus the NAME of the
# invariant that fired. cow_buggy_vfork is the one cfg whose bug is a HANG,
# not an unsafe state, so its verdict is a temporal violation with Safety
# intact, and it gets its own leg.
#
# The four pre-B-1a cfgs are pinned at the counts measured on the untouched
# module BEFORE the ALLOW_PROTECT extension landed (2026-09-23: 580 / 211 /
# 124 / 231). That is the additivity claim of the extension, stated as a check
# rather than an assertion: a protect action leaking past its gate would move
# one of these numbers.
#
# B-1a' (2026-09-23): cow_leaf is the clean cfg with MODEL_LEAF (the read-only
# leaf + the two-step tail), pinned at its own count; cow_buggy_put_before_replace
# is bug 7, judged by NoReadableFreed. With MODEL_LEAF off the eight older cfgs
# reproduce their counts exactly -- that is the additivity claim of the extension.
#
# What this script CANNOT see, said so the green reads no larger: the ceiling
# (prot <= prot_max, X never a target) is a pure per-call comparison and is
# not in the model at all -- the kernel tests are its witness.
set -u
cd "$(dirname "$0")"
export PATH="/opt/homebrew/opt/openjdk/bin:$PATH"
JAR=${TLA_JAR:-/tmp/tla2tools.jar}
TMP=$(mktemp -d) || exit 1
trap 'rm -rf "$TMP"' EXIT
STAMP="$TMP/stamp"; : > "$STAMP"

# clean: cfg, expected distinct states ("-" = do not pin)
CLEAN="cow:580
cow_protect:10636
cow_leaf:2996"

# buggy: cfg, invariant that must be the one reported
BUGGY="cow_buggy_break:NoAliasedWritable
cow_buggy_teardown:NoUseAfterFree
cow_buggy_protect_keeps_pte:NoWritablePteBeyondProt
cow_buggy_fault_ignores_prot:BreakOnlyWhenWritable
cow_buggy_clone_per_piece:ShareIsHolderCount
cow_buggy_put_before_replace:NoReadableFreed"

# temporal: cfg, the property that must be the one reported, expected distinct
# states (the whole space is explored before liveness is judged, so the count
# is a fingerprint here too; Safety must hold)
TEMPORAL="cow_buggy_vfork:EventuallyReleased:231"

run() {  # $1 = cfg basename -> sets RC and LOG
    LOG="$TMP/$1.log"
    java -cp "$JAR" tlc2.TLC -workers auto -deadlock -metadir "$TMP/$1.meta" \
        -config "$1.cfg" cow.tla > "$LOG" 2>&1
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

echo "== temporal (Safety intact; the liveness property must fail) =="
echo "$TEMPORAL" | while IFS=: read -r cfg prop want; do
    run "$cfg"
    got=$(grep -o '[0-9]* distinct states found' "$LOG" | tail -1 | awk '{print $1}')
    if grep -q "Invariant [A-Za-z]* is violated" "$LOG"; then
        echo "FAIL $cfg: a SAFETY invariant fired -- this cfg documents a hang, not an unsafe state"
        echo fail > "$TMP/failed"
    elif ! grep -q "Temporal property $prop was violated" "$LOG"; then
        echo "FAIL $cfg: rc=$RC, $prop not reported violated -- got: $(grep -o 'Temporal property [A-Za-z]* was violated' "$LOG" | head -1)"
        echo fail > "$TMP/failed"
    elif [ "$want" != "-" ] && [ "$got" != "$want" ]; then
        echo "FAIL $cfg: $got distinct states, expected $want -- the model CHANGED"
        echo fail > "$TMP/failed"
    else
        echo "ok   $cfg: rc=$RC, $prop violated as claimed, $got distinct states"
    fi
done

# A violation drops a <module>_TTrace_<epoch>.{tla,bin} pair beside the spec.
# They are gitignored, but quaestor's spec census counts files: sweep the ones
# THIS run made, and only those.
find . -maxdepth 1 -name 'cow_TTrace_*' -newer "$STAMP" -exec rm -f {} +

fail=0
[ -f "$TMP/failed" ] && fail=1
echo
if [ "$fail" -eq 0 ]; then echo "cow: ALL CFGS AS CLAIMED"; else echo "cow: FAILURES ABOVE"; fi
exit $fail
