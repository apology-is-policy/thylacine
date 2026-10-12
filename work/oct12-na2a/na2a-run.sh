#!/bin/sh
# NA-2a guest legs (the EL0 bridge probe), in one lease window:
#   CONTROL-D  default --config ci kernel: the whole suite passes, loom.bridge_probe
#              PASSes, and the probe itself printed its default verdict (70) -- a
#              SKIP for a missing binary would also PASS, so the line is required.
#   CONTROL-Q  PRIVATE_ASYNC_QUAL=y in its own kernel dir: the same, with the
#              qualification verdict (71) and four private rings retired.
#   M10        default-image mutant: a private request reaches an ordinary ring.
#   M8 M9 M11  qualification-image mutants: the params-copyout rollback skipped;
#              the record copyout blind to faults; the test's lever pin inverted.
#              Each is predicted to FAIL exactly loom.bridge_probe, and the
#              probe's own line names which check caught it (M11: none -- the
#              probe passes honestly and the pin alone fails it).
#   REBUILD    both kernels from the committed tree, compared with the controls.
# Not proven here: SMP, sanitizers (the Mac-axis residual, 2026-10-07 vote).
# Derived from work/oct12-na1/na1-run.sh.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
STAMP=$(date -u '+%Y%m%dT%H%M%SZ')
OUT=work/oct12-na2a/run-$STAMP
mkdir -p "$OUT/pristine"
YIP="${THYLA_WAKE_YIP:-$(command -v yip || echo "$HOME/.local/bin/yip")}"
FLOOR_MB=${FLOOR_MB:-8192}
SUITE_EXT='EXTINCTION: kernel test suite failed'
STRATUM_PIN=7dd103ddbccabe95dfe1d36fdddb4d6a1bfd99dc
export STRATUM_SRC=$HOME/projects/stratum-astra/v2
MUT=work/oct12-na2a/na2a-mutants.py
FILES=$(python3 "$MUT" --files)
QDIR=build/kernel-private-async-qual
QUAL_LINE='PRIVATE-ASYNC QUALIFICATION build'
PROBE=usr/loom-bridge-probe/loom-bridge-probe.c
KTEST=kernel/test/test_loom_bridge_probe.c
TESTS='loom.bridge_probe loom.private_bridge loom.setup_rollback_settles'
VERDICT="loom-bridge-probe's verdict matches this kernel's qualification lever"
LINE_D='loom-bridge-probe: default image refuses every private route'
LINE_Q='loom-bridge-probe: qualification image routes every private syscall from EL0'
P8='both rollbacks closed the handle and unmapped the ring'
P9="a read-only record's result copyout faults"
P10='params echo the granted flags'
MUTATED=0

record() { echo "$(date -u '+%Y-%m-%dT%H:%M:%SZ')  $*" | tee -a "$OUT/stages.txt"; }
free_mb() { df -m . | awk 'NR==2 {print $4}'; }
floor() {
  _f=$(free_mb); record "disk $1: $_f MiB free (floor $FLOOR_MB)"
  [ "$_f" -ge "$FLOOR_MB" ] || { echo "REFUSING at $1: under the floor"; exit 3; }
}
lease() {
  "$YIP" resources > "$OUT/lease-$1.txt" 2>&1 || true
  /usr/bin/grep -qE '^mac +HELD by you' "$OUT/lease-$1.txt" || {
    echo "STOPPING before $1: the mac lease is not mine"; exit 6; }
}
restore() {
  for f in $FILES; do cp "$OUT/pristine/$(basename "$f")" "$f"; done
  git diff --quiet -- $FILES || { echo "!! $FILES differ from HEAD after restore"; return 1; }
}
on_exit() {
  _rc=$?
  if [ "$MUTATED" = 1 ]; then
    restore && echo "-- mutated files restored; git diff clean" || _rc=9
    echo "!! build/ may hold a MUTANT kernel. Rebuild both before any other boot:"
    echo "   STRATUM_SRC=$STRATUM_SRC tools/build.sh kernel --config ci [--set PRIVATE_ASYNC_QUAL=y]"
  fi
  record "exit $_rc"
  exit $_rc
}
trap on_exit EXIT

verdict() { # verdict <boot log> <test>: PASS, FAIL: <msg>, or NONE
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
fails() { /usr/bin/grep -E '^FAIL:|\[test\] [^ ]+ \.\.\. FAIL:' "$1" || true; }
tally() { /usr/bin/grep -E '  tests: [0-9]+/[0-9]+' "$1" | tail -1 | sed -E 's/.*tests: ([0-9]+)\/([0-9]+).*/\1 \2/'; }
lines() { tr -d '\r' < "$1" | /usr/bin/grep -cF -- "$2" || true; }
run_suite() { # run_suite <label> [test.sh args]
  _l=$1; shift
  ( set +e; tools/test.sh "$@" > "$OUT/$_l-test.log" 2>&1; echo $? > "$OUT/$_l.rc" )
  suite_rc=$(cat "$OUT/$_l.rc")
  [ -f build/test-boot.log ] || { echo "!! no build/test-boot.log after $_l"; exit 1; }
  cp build/test-boot.log "$OUT/$_l-boot.log"
  record "$_l: test.sh exit $suite_rc"
}
inputs() { # inputs <label> <kernel dir>
  for f in "$2/thylacine.bin" "$2/thylacine.elf" build/ramfs.cpio build/disk.img; do
    [ -f "$f" ] && shasum -a 256 "$f"
  done > "$OUT/$1-inputs.sha256"
}
build() { # build <label> default|qual
  if [ "$2" = qual ]; then set -- "$1" --set PRIVATE_ASYNC_QUAL=y; else set -- "$1"; fi
  _l=$1; shift
  tools/build.sh kernel --config ci "$@" > "$OUT/$_l-build.log" 2>&1 || { echo "$_l build failed"; exit 1; }
}
baked() { # the probe in the ramfs is the one built from this tree
  _c=$(cpio -it < build/ramfs.cpio 2>/dev/null | /usr/bin/grep -cx 'bin/loom-bridge-probe' || true)
  [ "$_c" = 1 ] || { echo "$1: bin/loom-bridge-probe is in the ramfs $_c times"; exit 1; }
  ( cd "$OUT" && cpio -id --quiet bin/loom-bridge-probe < "$ROOT/build/ramfs.cpio" && mv bin/loom-bridge-probe "$1-probe" && rmdir bin )
  cmp -s "$OUT/$1-probe" build/usr/loom-bridge-probe/loom-bridge-probe || { echo "$1: the baked probe is not the built one"; exit 1; }
}
status_line() { # status_line <boot log> <status> <expected>
  _s="loom-bridge-probe status=$2 expected=$3"
  [ "$(lines "$1" "$_s")" = 1 ] || { echo "no single '$_s' in $1"; /usr/bin/grep -F 'loom-bridge-probe' "$1" | head; exit 2; }
}
green() { # green <label> <boot log> <probe line> <status>
  _l=$1; _b=$2
  /usr/bin/grep -q '^EXTINCTION:' "$_b" && { echo "$_l EXTINCTED"; /usr/bin/grep '^EXTINCTION:' "$_b"; exit 1; } || true
  _t=$(tally "$_b")
  [ "$_t" = "$EXPECT $EXPECT" ] || { echo "$_l tally '$_t' != $EXPECT/$EXPECT"; fails "$_b" | head; exit 1; }
  [ "$suite_rc" = 0 ] || { echo "$_l test.sh exit $suite_rc"; exit 1; }
  for _x in $TESTS; do
    _v=$(verdict "$_b" "$_x"); [ "$_v" = PASS ] || { echo "$_l: $_x verdict '$_v'"; exit 1; }
  done
  [ "$(lines "$_b" "$3")" = 1 ] || { echo "$_l: the probe did not print '$3' once"; exit 1; }
  [ "$(lines "$_b" 'loom-bridge-probe: FAIL')" = 0 ] || { echo "$_l: a probe FAIL line"; exit 1; }
  status_line "$_b" "$4" "$4"
}

echo "=== preconditions ==="
[ -x "$YIP" ] || { echo "REFUSING: no yip at $YIP"; exit 3; }
lease pre
dirty=$(git status --porcelain --untracked-files=no | wc -l | tr -d ' ')
[ "$dirty" = 0 ] || { echo "REFUSING: $dirty tracked modification(s); commit first"; exit 3; }
record "HEAD $(git rev-parse HEAD)"
python3 "$MUT" --check > "$OUT/anchors.txt" || { cat "$OUT/anchors.txt"; echo "REFUSING: mutant anchors"; exit 3; }
for w in "$P8" "$P9" "$P10"; do
  c=$(/usr/bin/grep -cF "\"$w\"" "$PROBE" || true)
  [ "$c" = 1 ] || { echo "REFUSING: probe check '$w' occurs $c times"; exit 3; }
done
[ "$(/usr/bin/grep -cF "\"$VERDICT\"" "$KTEST")" = 1 ] || { echo "REFUSING: the verdict message"; exit 3; }
for t in $TESTS; do
  /usr/bin/grep -qF "{ \"$t\"," kernel/test/test.c || { echo "REFUSING: $t not registered"; exit 3; }
done
EXPECT=$(/usr/bin/grep -c -E '^[[:space:]]*\{[[:space:]]*"[^"]+"' kernel/test/test.c)
[ "$EXPECT" -ge 1000 ] || { echo "REFUSING: derived only $EXPECT registrations"; exit 3; }
record "expected tests (derived from test.c): $EXPECT"
[ "$(git -C "$STRATUM_SRC" rev-parse HEAD)" = "$STRATUM_PIN" ] || { echo "REFUSING: Stratum HEAD is not $STRATUM_PIN"; exit 3; }
[ -z "$(git -C "$STRATUM_SRC" status --porcelain)" ] || { echo "REFUSING: Stratum tree dirty"; exit 3; }
for f in $FILES; do cp "$f" "$OUT/pristine/$(basename "$f")"; done
floor pre

echo "=== CONTROL-D ==="
lease control-d
build control-d default
/usr/bin/grep -qF "$QUAL_LINE" "$OUT/control-d-build.log" && { echo "CONTROL-D: the default build saw the lever"; exit 1; } || true
floor post-control-d-build
baked control-d
inputs control-d build/kernel
D_BIN=$(shasum -a 256 build/kernel/thylacine.bin | cut -d' ' -f1)
run_suite control-d
green CONTROL-D "$OUT/control-d-boot.log" "$LINE_D" 70
record "CONTROL-D GREEN: $EXPECT/$EXPECT; the probe ran from the ramfs and printed its default verdict"

echo "=== CONTROL-Q ==="
lease control-q
build control-q qual
/usr/bin/grep -qF "$QUAL_LINE" "$OUT/control-q-build.log" || { echo "CONTROL-Q: cmake never saw the lever"; exit 1; }
floor post-control-q-build
baked control-q
inputs control-q "$QDIR"
Q_BIN=$(shasum -a 256 "$QDIR/thylacine.bin" | cut -d' ' -f1)
[ "$Q_BIN" != "$D_BIN" ] || { echo "CONTROL-Q: the qualification kernel equals the default one"; exit 1; }
[ "$(shasum -a 256 build/kernel/thylacine.bin | cut -d' ' -f1)" = "$D_BIN" ] || { echo "CONTROL-Q: the qual build replaced the default kernel"; exit 1; }
run_suite control-q --private-async-qual
green CONTROL-Q "$OUT/control-q-boot.log" "$LINE_Q" 71
record "CONTROL-Q GREEN: $EXPECT/$EXPECT; the probe drove every private route from EL0 and its four rings retired"
SEEN_BINS="$D_BIN $Q_BIN"

mutant() { # mutant <label> default|qual: patch, build, boot; sets B
  lease "$1"; floor "pre-$1"
  restore || exit 9
  MUTATED=1
  python3 "$MUT" "$1" > "$OUT/$1.files" || exit 5
  [ "$(cat $FILES | /usr/bin/grep -c 'MUTANT')" = 1 ] || { echo "REFUSING: not exactly one MUTANT marker"; exit 5; }
  git diff -- $FILES > "$OUT/$1.diff"
  build "$1" "$2"
  floor "post-$1-build"
  if [ "$2" = qual ]; then _k=$QDIR; _a=--private-async-qual; else _k=build/kernel; _a=; fi
  inputs "$1" "$_k"
  _bin=$(shasum -a 256 "$_k/thylacine.bin" | cut -d' ' -f1)
  case " $SEEN_BINS " in *" $_bin "*) echo "REFUSING: the $1 kernel equals an earlier kernel of this run"; exit 5;; esac
  SEEN_BINS="$SEEN_BINS $_bin"
  run_suite "$1" $_a
  B=$OUT/$1-boot.log
  [ "$suite_rc" != 0 ] || { echo "HARNESS: test.sh exit 0 under $1"; exit 2; }
  restore || exit 9
  MUTATED=0
}
only_probe_fails() { # only_probe_fails <label>: exactly loom.bridge_probe, at its verdict
  _ext=$(/usr/bin/grep -E '^EXTINCTION:' "$B" | tr -d '\r')
  [ "$_ext" = "$SUITE_EXT" ] || { echo "$1: the extinction set is not exactly the suite's own:"; /usr/bin/grep -nE '^EXTINCTION:' "$B" | head -4; exit 2; }
  _t=$(tally "$B")
  [ "$_t" = "$((EXPECT - 1)) $EXPECT" ] || { echo "$1 tally '$_t', predicted $((EXPECT - 1)) $EXPECT"; fails "$B"; exit 2; }
  _v=$(verdict "$B" loom.bridge_probe)
  [ "$_v" = "FAIL: $VERDICT" ] || { echo "$1: loom.bridge_probe verdict '$_v'"; exit 2; }
  _nf=$(fails "$B" | wc -l | tr -d ' ')
  [ "$_nf" = 1 ] || { echo "$1: $_nf FAIL lines, predicted 1"; fails "$B"; exit 2; }
}
caught_by() { # caught_by <label> <probe check>: the probe's one FAIL line names it
  [ "$(lines "$B" 'loom-bridge-probe: FAIL')" = 1 ] || { echo "$1: not exactly one probe FAIL line"; /usr/bin/grep -F 'loom-bridge-probe' "$B" | head; exit 2; }
  tr -d '\r' < "$B" | /usr/bin/grep -qE "loom-bridge-probe: FAIL line [0-9]+: $2 \(got " || {
    echo "$1: the probe failed elsewhere:"; /usr/bin/grep -F 'loom-bridge-probe: FAIL' "$B"; exit 2; }
}

echo "=== M10: the default image routes a private request into an ordinary ring ==="
mutant m10 default
only_probe_fails M10
caught_by M10 "$P10"
status_line "$B" 1 70
record "M10 DISCRIMINATED: exactly loom.bridge_probe FAILs; the probe caught '$P10'"

echo "=== M8: the params-copyout fault skips its rollback ==="
mutant m8 qual
only_probe_fails M8
caught_by M8 "$P8"
status_line "$B" 1 71
record "M8 DISCRIMINATED: exactly loom.bridge_probe FAILs; the probe caught '$P8'"

echo "=== M9: the record copyout ignores a fault ==="
mutant m9 qual
only_probe_fails M9
caught_by M9 "$P9"
status_line "$B" 1 71
record "M9 DISCRIMINATED: exactly loom.bridge_probe FAILs; the probe caught '$P9'"

echo "=== M11: the test's lever pin inverted ==="
mutant m11 qual
only_probe_fails M11
[ "$(lines "$B" "$LINE_Q")" = 1 ] && [ "$(lines "$B" 'loom-bridge-probe: FAIL')" = 0 ] || {
  echo "M11: the probe itself did not pass honestly"; /usr/bin/grep -F 'loom-bridge-probe' "$B" | head; exit 2; }
status_line "$B" 71 70
record "M11 DISCRIMINATED: the probe passed honestly (71) and the pin alone failed it"

echo "=== REBUILD both kernels from the committed tree ==="
lease rebuild
build rebuild-d default
build rebuild-q qual
RD=$(shasum -a 256 build/kernel/thylacine.bin | cut -d' ' -f1)
RQ=$(shasum -a 256 "$QDIR/thylacine.bin" | cut -d' ' -f1)
[ "$RD" = "$D_BIN" ] && record "rebuild-d byte-identical to CONTROL-D" || record "rebuild-d differs from CONTROL-D (recorded, not claimed identical)"
[ "$RQ" = "$Q_BIN" ] && record "rebuild-q byte-identical to CONTROL-Q" || record "rebuild-q differs from CONTROL-Q (recorded, not claimed identical)"
echo "-- done: $OUT"
