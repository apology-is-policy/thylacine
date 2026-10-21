#!/bin/sh
# The SMP leg of the AS-R9 backport gate, run alone. The 09:59Z run
# (gate-20261019T095922Z) passed DEFAULT, FAULT, CI and UBSAN on this commit;
# its SMP leg never ran: ci-smp-gate.sh's own UBSan rebuild hit build.sh's 6 GB
# disk floor (5982 MB free). The operator then had corona's old AS-R9 gate
# inputs deleted (2026-10-19). This runs tools/ci-smp-gate.sh, the full matrix,
# against the same pins and with gate.sh's end checks. One difference from
# gate.sh: the EXIT trap captures the status before anything else runs (gate.sh
# recorded `exit $?` after `kill ... || true`, so its stages.txt says "exit 0"
# for the 09:59Z run, whose real status was 1).
# Run from the corona checkout root (no cd): sh work/oct19-asr9/gate-smp.sh
set -e
HERE=/Users/northkillpd/projects/thylacine-corona
ROOT=/Users/northkillpd/projects/thylacine-asr9
[ "$(pwd)" = "$HERE" ] || { echo "REFUSING: run from $HERE, not $(pwd)"; exit 3; }
STAMP=$(date -u '+%Y%m%dT%H%M%SZ')
OUT=$HERE/work/oct19-asr9/gate-smp-$STAMP
mkdir -p "$OUT"
YIP="${THYLA_WAKE_YIP:-$(command -v yip || echo "$HOME/.local/bin/yip")}"
FLOOR_MB=${FLOOR_MB:-6144}
KILL_MB=${KILL_MB:-3072}
ASR9_PIN=${ASR9_PIN:?set ASR9_PIN to the full sha under test}
BASE=11546588847f36d4f77a36b08f87b6b46a814cfd
STRATUM_PIN=7dd103ddbccabe95dfe1d36fdddb4d6a1bfd99dc
export STRATUM_SRC=/Users/northkillpd/projects/stratum/v2
STRATUM_DIRTY_OK='v2/.audit_r172_findings.md v2/specs/compound_ops_per_inode.tla'
GOFORK="${GOFORK:-$HOME/projects/go-thylacine}"
# What the 09:59Z run recorded; the SMP leg must build against the same fork.
FORK_RECORDED="f67e51e66a99cb02b7418cedee7200665210c58b dirty=0"
record() { echo "$(date -u '+%Y-%m-%dT%H:%M:%SZ')  $*" | tee -a "$OUT/stages.txt"; }
free_mb() { df -m "$ROOT" | awk 'NR==2 {print $4}'; }
floor() {
  _f=$(free_mb); record "disk $1: $_f MiB free (floor $FLOOR_MB)"
  [ "$_f" -ge "$FLOOR_MB" ] || { echo "REFUSING at $1: under the floor"; exit 3; }
}
lease() {
  "$YIP" resources > "$OUT/lease-$1.txt" 2>&1 || true
  /usr/bin/grep -qE '^mac +HELD by you' "$OUT/lease-$1.txt" || {
    echo "STOPPING before $1: the mac lease is not mine"; exit 6; }
}
watchdog() {
  while sleep 15; do
    _f=$(free_mb)
    if [ "$_f" -lt "$KILL_MB" ]; then
      echo "$(date -u '+%Y-%m-%dT%H:%M:%SZ')  WATCHDOG: $_f MiB free < $KILL_MB, killing the asr9 gate" >> "$OUT/stages.txt"
      pkill -TERM -f "thylacine-asr9/" || true
      kill -TERM "$1" 2>/dev/null || true
      exit 0
    fi
  done
}
watchdog $$ &
WD=$!
trap 'rc=$?; kill $WD 2>/dev/null || true; record "exit $rc"' EXIT
stratum_dirty() { git -C "$STRATUM_SRC" status --porcelain --untracked-files=no | sed 's/^...//' | LC_ALL=C sort | tr '\n' ' ' | sed 's/ $//'; }
stratum_inputs() { git -C "$STRATUM_SRC" status --porcelain --untracked-files=all | /usr/bin/grep '^??' | sed 's/^?? //' | /usr/bin/grep -E '\.(c|h|cc|cpp|S|s|cmake)$|CMakeLists\.txt$' || true; }
stratum_ok() {
  _h=$(git -C "$STRATUM_SRC" rev-parse HEAD)
  [ "$_h" = "$STRATUM_PIN" ] || { echo "REFUSING at $1: Stratum HEAD $_h is not $STRATUM_PIN"; exit 3; }
  [ "$(stratum_dirty)" = "$STRATUM_DIRTY_OK" ] || { echo "REFUSING at $1: Stratum tracked edits '$(stratum_dirty)'"; exit 3; }
  [ -z "$(stratum_inputs)" ] || { echo "REFUSING at $1: untracked Stratum build inputs: $(stratum_inputs)"; exit 3; }
}
fork_state() { echo "$(git -C "$GOFORK" rev-parse HEAD) dirty=$(git -C "$GOFORK" status --porcelain | wc -l | tr -d ' ')"; }

echo "=== preconditions ==="
[ -x "$YIP" ] || { echo "REFUSING: no yip at $YIP"; exit 3; }
lease pre
[ "$(git -C "$ROOT" status --porcelain --untracked-files=no | wc -l | tr -d ' ')" = 0 ] || { echo "REFUSING: asr9 has tracked modifications"; exit 3; }
[ "$(git -C "$ROOT" rev-parse HEAD)" = "$ASR9_PIN" ] || { echo "REFUSING: asr9 HEAD is not $ASR9_PIN"; exit 3; }
[ "$(git -C "$ROOT" merge-base HEAD "$BASE")" = "$BASE" ] || { echo "REFUSING: $BASE is not an ancestor"; exit 3; }
record "asr9 HEAD $ASR9_PIN on main $BASE, clean"
stratum_ok pre
record "stratum at $STRATUM_PIN, tracked edits exactly '$STRATUM_DIRTY_OK', no untracked build input"
FORK0=$(fork_state)
[ "$FORK0" = "$FORK_RECORDED" ] || { echo "REFUSING: the shared fork is '$FORK0', not the 09:59Z run's '$FORK_RECORDED'"; exit 3; }
export GOFORK
record "go fork $GOFORK at $FORK0 (shared, unpinned; the 09:59Z run's)"
floor pre

echo "=== SMP ==="
lease smp
( set +e; "$ROOT/tools/ci-smp-gate.sh" > "$OUT/smp.log" 2>&1; echo $? > "$OUT/smp.rc" )
[ "$(cat "$OUT/smp.rc")" = 0 ] || { echo "ci-smp-gate.sh exit $(cat "$OUT/smp.rc")"; tail -40 "$OUT/smp.log"; exit 1; }
record "SMP GREEN: $(tail -6 "$OUT/smp.log" | tr '\n' ' ')"

[ "$(fork_state)" = "$FORK0" ] || { echo "the shared fork moved during the run: $FORK0 -> $(fork_state)"; exit 8; }
stratum_ok end
[ "$(git -C "$ROOT" rev-parse HEAD)" = "$ASR9_PIN" ] && [ "$(git -C "$ROOT" status --porcelain --untracked-files=no | wc -l | tr -d ' ')" = 0 ] \
  || { echo "asr9 HEAD moved or the tree changed during the run"; exit 8; }
record "SMP leg GREEN on asr9 $ASR9_PIN (main $BASE, stratum $STRATUM_PIN, go fork $FORK0); with gate-20261019T095922Z's DEFAULT, FAULT, CI and UBSAN legs, all five legs are green"
