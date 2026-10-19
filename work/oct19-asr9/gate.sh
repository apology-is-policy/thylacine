#!/bin/sh
# Gate for the AS-R9 backport to main: branch corona/asr9-main in the worktree
# ~/projects/thylacine-asr9, against canonical Stratum main at 7dd103d and the
# SHARED Go fork (no pin: main lands it against its own fork, so the fork's HEAD
# is recorded and must not move during the run). The legs main named on yip 0232:
#   DEFAULT  bare tools/build.sh kernel (the Halcyon image, the merge rule's
#            "default build"): the whole suite, zero FAIL in the boot log, the
#            five AS-R9 tests PASS, total = main's base + 5, the five names in
#            the ELF (the denominator control: a suite that never compiled them
#            would pass every other check).
#   FAULT    tools/test-fault.sh: every hardening witness fires.
#   CI       --config ci: the whole suite again on the gate image.
#   UBSAN    --config ci --sanitize=ubsan: the suite in UBSan trapping mode.
#   SMP      tools/ci-smp-gate.sh, the full matrix at N=10.
# The specs run first, separately: work/oct19-asr9/specs.sh.
# Run from the corona checkout root (no cd): sh work/oct19-asr9/gate.sh
set -e
HERE=/Users/northkillpd/projects/thylacine-corona
ROOT=/Users/northkillpd/projects/thylacine-asr9
[ "$(pwd)" = "$HERE" ] || { echo "REFUSING: run from $HERE, not $(pwd)"; exit 3; }
STAMP=$(date -u '+%Y%m%dT%H%M%SZ')
OUT=$HERE/work/oct19-asr9/gate-$STAMP
mkdir -p "$OUT"
YIP="${THYLA_WAKE_YIP:-$(command -v yip || echo "$HOME/.local/bin/yip")}"
FLOOR_MB=${FLOOR_MB:-6144}
KILL_MB=${KILL_MB:-3072}
ASR9_PIN=${ASR9_PIN:?set ASR9_PIN to the full sha under test}
BASE=11546588847f36d4f77a36b08f87b6b46a814cfd
STRATUM_PIN=7dd103ddbccabe95dfe1d36fdddb4d6a1bfd99dc
export STRATUM_SRC=/Users/northkillpd/projects/stratum/v2
STRATUM_DIRTY_OK='v2/.audit_r172_findings.md v2/specs/compound_ops_per_inode.tla'
STRATUMD_LINE="==> building stratumd (aarch64-thylacine) from $STRATUM_SRC"
GOFORK="${GOFORK:-$HOME/projects/go-thylacine}"
NAMED="burrow.settled_drop_retains_nonfinal_charge burrow.settled_drop_exact_payer burrow.settled_mapping_drop_defers_free burrow.unmap_failure_leaves_mapping_attached burrow.unmap_interior_start_refused"
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
# The shared host's disk: a bake that runs it dry breaks every agent's shell
# (2026-10-05, 121 MB). Kill this tree's own processes first.
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
trap 'kill $WD 2>/dev/null || true; record "exit $?"' EXIT
fails() { /usr/bin/grep -E '^FAIL:|\[test\] [^ ]+ \.\.\. FAIL' "$1" || true; }
tally() { /usr/bin/grep -E '  tests: [0-9]+/[0-9]+' "$1" | tail -1 | sed -E 's/.*tests: ([0-9]+)\/([0-9]+).*/\1 \2/'; }
verdict() { # verdict <boot log> <test>: prints PASS, FAIL: <msg>, or NONE
  awk -v t="$2" '
    /\[test\] [^ ]+ \.\.\./ {
      if (inblk) exit
      if (index($0, "[test] " t " ... ") > 0) {
        inblk = 1; rest = substr($0, index($0, " ... ") + 5)
        sub(/\r$/, "", rest)
        if (rest ~ /^(PASS|FAIL)/) { print rest; found = 1; exit }
        next
      }
    }
    inblk { sub(/\r$/, ""); if ($0 ~ /^(PASS|FAIL)/) { print; found = 1; exit } }
    END { if (!found) print "NONE" }
  ' "$1"
}
run_suite() { # run_suite <label> [test.sh args]
  _l=$1; shift
  rm -f "$ROOT/build/test-boot.log"
  ( set +e; "$ROOT/tools/test.sh" "$@" > "$OUT/$_l-test.log" 2>&1; echo $? > "$OUT/$_l.rc" )
  suite_rc=$(cat "$OUT/$_l.rc")
  [ -f "$ROOT/build/test-boot.log" ] || { echo "!! no build/test-boot.log after $_l"; exit 1; }
  cp "$ROOT/build/test-boot.log" "$OUT/$_l-boot.log"
  record "$_l: test.sh exit $suite_rc"
}
image() { # image <label> <boot log>
  B=$2
  /usr/bin/grep -q '^EXTINCTION:' "$B" && { echo "$1 EXTINCTED"; /usr/bin/grep -E '^EXTINCTION:' "$B" | head; fails "$B" | head; exit 1; } || true
  _fl=$(fails "$B" | wc -l | tr -d ' ')
  [ "$_fl" = 0 ] || { echo "$1: $_fl FAIL line(s)"; fails "$B" | head -20; exit 1; }
  set -- "$1" "$2" $(tally "$B")
  [ "${3:-}" = "$EXPECT" ] && [ "${4:-}" = "$EXPECT" ] || { echo "$1 tally '${3:-} ${4:-}' != $EXPECT/$EXPECT"; exit 1; }
  [ "$suite_rc" = 0 ] || { echo "$1 test.sh exit $suite_rc"; exit 1; }
  for t in $NAMED; do
    v=$(verdict "$B" "$t")
    [ "$v" = PASS ] || { echo "$1: $t verdict '$v'"; exit 1; }
  done
  record "$1 GREEN: $EXPECT/$EXPECT, 0 FAIL lines, no EXTINCTION, the five AS-R9 tests PASS"
}
in_elf() { # in_elf <label> <elf>: every NAMED test string is compiled in
  for t in $NAMED; do
    LC_ALL=C /usr/bin/grep -c -a -F "$t" "$2" > /dev/null || { echo "$1: '$t' is not in $2"; exit 1; }
  done
  record "$1: the five AS-R9 test names are in $(basename "$2")"
}
stratum_dirty() { git -C "$STRATUM_SRC" status --porcelain --untracked-files=no | sed 's/^...//' | LC_ALL=C sort | tr '\n' ' ' | sed 's/ $//'; }
stratum_inputs() { git -C "$STRATUM_SRC" status --porcelain --untracked-files=all | /usr/bin/grep '^??' | sed 's/^?? //' | /usr/bin/grep -E '\.(c|h|cc|cpp|S|s|cmake)$|CMakeLists\.txt$' || true; }
stratum_ok() {
  _h=$(git -C "$STRATUM_SRC" rev-parse HEAD)
  [ "$_h" = "$STRATUM_PIN" ] || { echo "REFUSING at $1: Stratum HEAD $_h is not $STRATUM_PIN"; exit 3; }
  [ "$(stratum_dirty)" = "$STRATUM_DIRTY_OK" ] || { echo "REFUSING at $1: Stratum tracked edits '$(stratum_dirty)'"; exit 3; }
  [ -z "$(stratum_inputs)" ] || { echo "REFUSING at $1: untracked Stratum build inputs: $(stratum_inputs)"; exit 3; }
}
cache_ok() { # cache_ok <CMakeCache.txt> <dir it must name>
  _h=$(/usr/bin/grep -m1 '^CMAKE_HOME_DIRECTORY' "$1" | cut -d= -f2)
  [ "$_h" = "$2" ] || { echo "$1 is configured against '$_h', not $2"; exit 1; }
}
built_right() { # built_right <label> <build log>
  _c=$(/usr/bin/grep -cF "$STRATUMD_LINE" "$2" || true)
  [ "$_c" = 1 ] || { echo "$1: the stratumd line occurs $_c times"; exit 1; }
  cache_ok "$ROOT/build/host-stratum/CMakeCache.txt" "$STRATUM_SRC"
  cache_ok "$ROOT/build/pouch/stratumd-cmake/CMakeCache.txt" "$STRATUM_SRC"
  # A cloned cache that still names another checkout builds THAT tree's
  # sources into this image. The two external ones (clade's llvm-build,
  # pouch's cxx-runtimes) name pristine sources outside every checkout.
  for cc in $(find "$ROOT/build" -maxdepth 6 -name CMakeCache.txt); do
    _h=$(/usr/bin/grep -m1 '^CMAKE_HOME_DIRECTORY' "$cc" | cut -d= -f2)
    case "$_h" in
      "$ROOT"|"$ROOT"/*) ;;
      /Users/northkillpd/projects/thylacine|/Users/northkillpd/projects/thylacine/*|/Users/northkillpd/projects/thylacine-*)
        echo "$1: $cc names another checkout: $_h"; exit 1 ;;
    esac
  done
}
fork_state() { echo "$(git -C "$GOFORK" rev-parse HEAD) dirty=$(git -C "$GOFORK" status --porcelain | wc -l | tr -d ' ')"; }

echo "=== preconditions ==="
[ -x "$YIP" ] || { echo "REFUSING: no yip at $YIP"; exit 3; }
lease pre
[ "$(git -C "$ROOT" status --porcelain --untracked-files=no | wc -l | tr -d ' ')" = 0 ] || { echo "REFUSING: asr9 has tracked modifications"; exit 3; }
[ "$(git -C "$ROOT" rev-parse HEAD)" = "$ASR9_PIN" ] || { echo "REFUSING: asr9 HEAD is not $ASR9_PIN"; exit 3; }
[ "$(git -C "$ROOT" merge-base HEAD "$BASE")" = "$BASE" ] || { echo "REFUSING: $BASE is not an ancestor"; exit 3; }
record "asr9 HEAD $ASR9_PIN on main $BASE, clean"
for t in $NAMED; do
  /usr/bin/grep -qF "{ \"$t\"," "$ROOT/kernel/test/test.c" || { echo "REFUSING: $t not registered"; exit 3; }
done
EXPECT=$(/usr/bin/grep -c -E '^[[:space:]]*\{[[:space:]]*"[^"]+"' "$ROOT/kernel/test/test.c")
BASE_N=$(git -C "$ROOT" show "$BASE:kernel/test/test.c" | /usr/bin/grep -c -E '^[[:space:]]*\{[[:space:]]*"[^"]+"')
[ "$EXPECT" -eq $((BASE_N + 5)) ] || { echo "REFUSING: $EXPECT registrations, main has $BASE_N (+5 expected)"; exit 3; }
record "expected tests: $EXPECT (main's $BASE_N + the five)"
stratum_ok pre
record "stratum at $STRATUM_PIN, tracked edits exactly '$STRATUM_DIRTY_OK', no untracked build input"
FORK0=$(fork_state)
case "$FORK0" in *" dirty=0") ;; *) echo "REFUSING: the shared fork $GOFORK is mid-edit ($FORK0)"; exit 3 ;; esac
export GOFORK
record "go fork $GOFORK at $FORK0 (shared, unpinned)"
floor pre

echo "=== DEFAULT ==="
lease default
"$ROOT/tools/build.sh" kernel > "$OUT/default-build.log" 2>&1 || { echo "default build failed"; tail -20 "$OUT/default-build.log"; exit 1; }
floor post-default-build
built_right DEFAULT "$OUT/default-build.log"; stratum_ok post-default-build
in_elf DEFAULT "$ROOT/build/kernel/thylacine.elf"
record "default kernel $(shasum -a 256 "$ROOT/build/kernel/thylacine.bin" | cut -d' ' -f1)"
run_suite default
image default "$OUT/default-boot.log"

echo "=== FAULT ==="
lease fault
( set +e; "$ROOT/tools/test-fault.sh" > "$OUT/fault.log" 2>&1; echo $? > "$OUT/fault.rc" )
[ "$(cat "$OUT/fault.rc")" = 0 ] || { echo "test-fault.sh exit $(cat "$OUT/fault.rc")"; tail -30 "$OUT/fault.log"; exit 1; }
record "FAULT GREEN: $(tail -3 "$OUT/fault.log" | tr '\n' ' ')"
floor post-fault

echo "=== CI ==="
lease ci
"$ROOT/tools/build.sh" kernel --config ci > "$OUT/ci-build.log" 2>&1 || { echo "ci build failed"; tail -20 "$OUT/ci-build.log"; exit 1; }
floor post-ci-build
built_right CI "$OUT/ci-build.log"; stratum_ok post-ci-build
in_elf CI "$ROOT/build/kernel/thylacine.elf"
CI_BIN=$(shasum -a 256 "$ROOT/build/kernel/thylacine.bin" | cut -d' ' -f1)
record "ci kernel $CI_BIN"
run_suite ci
image ci "$OUT/ci-boot.log"

echo "=== UBSAN ==="
lease ubsan
"$ROOT/tools/build.sh" kernel --config ci --sanitize=ubsan > "$OUT/ubsan-build.log" 2>&1 || { echo "ubsan build failed"; tail -20 "$OUT/ubsan-build.log"; exit 1; }
/usr/bin/grep -qF "sanitize='undefined'" "$OUT/ubsan-build.log" || { echo "UBSAN: the build never selected the sanitizer"; exit 1; }
floor post-ubsan-build
built_right UBSAN "$OUT/ubsan-build.log"; stratum_ok post-ubsan-build
in_elf UBSAN "$ROOT/build/kernel-undefined/thylacine.elf"
U_BIN=$(shasum -a 256 "$ROOT/build/kernel-undefined/thylacine.bin" | cut -d' ' -f1)
[ "$U_BIN" != "$CI_BIN" ] || { echo "UBSAN: the sanitized kernel equals the ci one"; exit 1; }
record "ubsan kernel $U_BIN"
run_suite ubsan --sanitize=ubsan
image ubsan "$OUT/ubsan-boot.log"

echo "=== SMP ==="
lease smp
( set +e; "$ROOT/tools/ci-smp-gate.sh" > "$OUT/smp.log" 2>&1; echo $? > "$OUT/smp.rc" )
[ "$(cat "$OUT/smp.rc")" = 0 ] || { echo "ci-smp-gate.sh exit $(cat "$OUT/smp.rc")"; tail -40 "$OUT/smp.log"; exit 1; }
record "SMP GREEN: $(tail -6 "$OUT/smp.log" | tr '\n' ' ')"

[ "$(fork_state)" = "$FORK0" ] || { echo "the shared fork moved during the run: $FORK0 -> $(fork_state)"; exit 8; }
stratum_ok end
[ "$(git -C "$ROOT" rev-parse HEAD)" = "$ASR9_PIN" ] && [ "$(git -C "$ROOT" status --porcelain --untracked-files=no | wc -l | tr -d ' ')" = 0 ] \
  || { echo "asr9 HEAD moved or the tree changed during the run"; exit 8; }
record "ALL GREEN: asr9 $ASR9_PIN on main $BASE, stratum $STRATUM_PIN, go fork $FORK0"
