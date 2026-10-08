#!/bin/sh
# Verify every reader_frame cfg reports the verdict its header claims.
#
# The shape is specs/check-loom-role.sh's: a CLEAN cfg explores the whole state
# space, so its distinct-state count is a deterministic fingerprint (a change
# means the MODEL changed); a BUGGY cfg halts at the first violation and is
# judged on its verdict -- the exit status plus the NAME of the property that
# fired. Every run is `-workers 1 -lncheck final`, so each count is the one
# specs/SPEC-TO-CODE.md records.
#
# What this script CANNOT see, said so the green reads no larger: the model
# takes each transport recv to return the bytes it copied or none, and leaves
# out tags, the dying op's flush and more than one frame; the kernel tests
# rendez.reader_recv_* and 9p_srvconn_transport.reader_unwinds_mid_frame_*
# hold the code to it.
set -u
cd "$(dirname "$0")"
export PATH="/opt/homebrew/opt/openjdk/bin:$PATH"
JAR=${TLA_JAR:-/tmp/tla2tools.jar}
TMP=$(mktemp -d) || exit 1
trap 'rm -rf "$TMP"' EXIT
STAMP="$TMP/stamp"; : > "$STAMP"

# clean: cfg, expected distinct states ("-" = do not pin)
CLEAN="reader_frame:39
reader_frame_delivery:39
reader_frame_blockthrough_fair:34"

# buggy: cfg, invariant that must be the one reported, distinct states at the
# halt (deterministic under -workers 1; "-" = do not pin)
BUGGY="reader_frame_buggy:NoDesync:41"

# temporal: cfg, the property that must be the one reported, expected distinct
# states ("-" = do not pin)
TEMPORAL="reader_frame_blockthrough:EventuallyUnwinds:34"

run() {  # $1 = cfg basename -> sets RC, LOG and GOT (distinct states)
    LOG="$TMP/$1.log"
    java -cp "$JAR" tlc2.TLC -workers 1 -deadlock -lncheck final -metadir "$TMP/$1.meta" \
        -config "$1.cfg" reader_frame.tla > "$LOG" 2>&1
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
echo "$BUGGY" | while IFS=: read -r cfg want count; do
    run "$cfg"
    if [ "$RC" -eq 0 ]; then
        fail "FAIL $cfg: rc=0 -- the counterexample did NOT fire; $want is unguarded"
    elif ! grep -q "Invariant $want is violated" "$LOG"; then
        fail "FAIL $cfg: rc=$RC but not via $want -- got: $(grep -o -E '(Invariant|property) [A-Za-z]* (is|was) violated' "$LOG" | head -1)"
    elif [ "$count" != "-" ] && [ "$GOT" != "$count" ]; then
        fail "FAIL $cfg: $GOT distinct states at the halt, expected $count -- the model CHANGED"
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
find . -maxdepth 1 -name 'reader_frame_TTrace_*' -newer "$STAMP" -exec rm -f {} +

status=0
[ -f "$TMP/failed" ] && status=1
echo
if [ "$status" -eq 0 ]; then echo "reader_frame: ALL CFGS AS CLAIMED"; else echo "reader_frame: FAILURES ABOVE"; fi
exit $status
