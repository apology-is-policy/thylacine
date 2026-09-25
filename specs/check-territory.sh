#!/bin/sh
# Verify every territory.tla cfg reports the verdict its header claims.
#
# The shape and the reasoning are specs/check-territory-shed.sh's: a CLEAN
# cfg explores the whole state space, so its distinct-state count is a
# deterministic fingerprint (a change means the MODEL changed); a BUGGY cfg
# halts at the first violation, so it is judged on its verdict -- the exit
# status plus the NAME of the invariant that fired. Every cfg lists every
# invariant, so a buggy cfg that starts failing a DIFFERENT one has stopped
# documenting its bug. The buggy cfgs run on one worker: single-threaded BFS
# reaches its first violation in the same order every run, so the invariant
# reported is a property of the model, not of which worker won a race.
#
# territory.cfg is the long run: 31 minutes on 16 workers of a 32-core
# host (2026-09-25); hours on the 8-core Mac.
set -u
cd "$(dirname "$0")"
export PATH="/opt/homebrew/opt/openjdk/bin:$PATH"
JAR=${TLA_JAR:-/tmp/tla2tools.jar}
TMP=$(mktemp -d) || exit 1
trap 'rm -rf "$TMP"' EXIT
STAMP="$TMP/stamp"; : > "$STAMP"

# clean: cfg, expected distinct states ("-" = do not pin)
CLEAN="territory_cov_alias:202800
territory_file_point:4380876
territory:8052876"

# buggy: cfg, invariant that must be the one reported
BUGGY="territory_buggy:NoCycle
territory_buggy_mount_no_refbump:MountRefcountConsistency
territory_buggy_unmount_no_refdrop:MountRefcountConsistency
territory_buggy_destroy_leak:MountRefcountConsistency
territory_buggy_chroot_no_refbump:MountRefcountConsistency
territory_buggy_mount_order:OrderCorrect
territory_buggy_walk_last_hit:WalkFirstHit
territory_buggy_readdir_last_wins:ReaddirDedupFirstWins
territory_buggy_create_any_member:CreateTargetCorrect
territory_buggy_remove_mcreate:RemoveTargetCorrect
territory_buggy_union_no_covered:UnionHasCovered
territory_buggy_fresh_after_remove:CoveredOnlyInUnion
territory_buggy_covered_takes_flags:CoveredIsItsPoint
territory_buggy_covered_last:CoveredPlacement
territory_buggy_unmount_orphans_covered:NoOrphanCovered
territory_buggy_self_mount:NoSelfMount
territory_buggy_cover_file:NoCoveredFile"

run() {  # $1 = cfg basename, $2 = workers -> sets RC and LOG
    LOG="$TMP/$1.log"
    java -XX:+UseParallelGC -cp "$JAR" tlc2.TLC -workers "$2" -deadlock \
        -metadir "$TMP/$1.meta" -config "$1.cfg" territory.tla > "$LOG" 2>&1
    RC=$?
}

# Every cfg on disk must be judged here: one added without a line would
# otherwise pass by never running.
for f in territory*.cfg; do
    base=${f%.cfg}
    case "$base" in territory_shed*) continue ;; esac
    if ! printf '%s\n%s\n' "$CLEAN" "$BUGGY" | grep -q "^$base:"; then
        echo "FAIL $base: a cfg this script does not judge"
        : > "$TMP/failed"
    fi
done

echo "== clean (must run to completion) =="
echo "$CLEAN" | while IFS=: read -r cfg want; do
    run "$cfg" auto
    got=$(grep -o '[0-9]* distinct states found' "$LOG" | tail -1 | awk '{print $1}')
    if [ "$RC" -ne 0 ]; then
        echo "FAIL $cfg: rc=$RC (expected 0)"; sed -n '/Error/,+4p' "$LOG" | head -8
        : > "$TMP/failed"
    elif [ "$want" != "-" ] && [ "$got" != "$want" ]; then
        echo "FAIL $cfg: $got distinct states, expected $want -- the model CHANGED"
        : > "$TMP/failed"
    else
        echo "ok   $cfg: rc=0, $got distinct states"
    fi
done

echo "== buggy (must violate, and violate the NAMED invariant) =="
echo "$BUGGY" | while IFS=: read -r cfg want; do
    run "$cfg" 1
    if [ "$RC" -eq 0 ]; then
        echo "FAIL $cfg: rc=0 -- the counterexample did NOT fire; $want is unguarded"
        : > "$TMP/failed"
    elif ! grep -q "Invariant $want is violated" "$LOG"; then
        echo "FAIL $cfg: rc=$RC but not via $want -- got: $(grep -o 'Invariant [A-Za-z]* is violated' "$LOG" | head -1)"
        : > "$TMP/failed"
    else
        echo "ok   $cfg: rc=$RC, $want violated as claimed"
    fi
done

# A violation drops a <module>_TTrace_<epoch>.{tla,bin} pair beside the spec.
# They are gitignored; sweep the ones THIS run made, and only those.
find . -maxdepth 1 -name 'territory_TTrace_*' -newer "$STAMP" -exec rm -f {} +

fail=0
[ -f "$TMP/failed" ] && fail=1
echo
if [ "$fail" -eq 0 ]; then echo "territory: ALL CFGS AS CLAIMED"; else echo "territory: FAILURES ABOVE"; fi
exit $fail
