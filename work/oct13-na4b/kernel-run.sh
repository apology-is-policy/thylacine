#!/bin/sh
# NA-4b kernel REAP of a walked FID: the guest legs, in one lease window.
#   CONTROL  the committed tree: loom.service_fid_reap, loom.private_native_operations
#            and loom.private_native_isolation PASS, the whole suite passes.
#   M1       the REAP dispatcher no longer sends a non-RESERVED FID to the FID
#            primitive: the clunked FID passes the lookup, falls to
#            node_reap_unused, which needs a LIVE slot, and answers -ENOENT.
#            Predicted: exactly native_operations and native_isolation FAIL at
#            "native clunked FID reaps once while its ring lives"; the fixture
#            calls loom_fid_reap_retired directly and PASSES.
#   M3       the REAP lookup no longer accepts a RETIRING FID (the shape of
#            a2b463fdd, whose first guest boot failed this way): CLUNK left the
#            slot RETIRING, control_exact wants LIVE, REAP answers -ENOENT.
#            Predicted: the same two FAIL at the same assertion, the fixture PASSES.
#   M2       loom_fid_reap_retired refuses every unclunked FID instead of retiring
#            it under a RETIRED scope. Predicted: exactly three FAIL --
#            service_fid_reap at "the FIDs of a retired scope reap and release it",
#            native_operations and native_isolation at "native walked FID reaps
#            once its aborted scope retires".
#   SHAPE    tools/check-production.sh --all: the KERNEL_TESTS=OFF kernel builds.
# A failed suite always ends in "EXTINCTION: kernel test suite failed"
# (kernel/main.c): a mutant run must show exactly that extinction and no other.
# Not proven here: SMP, sanitizers, a real EL0 client (NA-4d). Mac axis, one
# boot per run. Derived from work/oct12-sq1/sq1-run.sh.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
STAMP=$(date -u '+%Y%m%dT%H%M%SZ')
OUT=work/oct13-na4b/kernel-$STAMP
mkdir -p "$OUT"
YIP="${THYLA_WAKE_YIP:-$(command -v yip || echo "$HOME/.local/bin/yip")}"
FLOOR_MB=${FLOOR_MB:-8192}
T_FIX=loom.service_fid_reap
T_OPS=loom.private_native_operations
T_ISO=loom.private_native_isolation
WANT_CLUNKED='native clunked FID reaps once while its ring lives'
WANT_ORPHAN='native walked FID reaps once its aborted scope retires'
WANT_FIXTURE='the FIDs of a retired scope reap and release it'
SUITE_EXT='EXTINCTION: kernel test suite failed'
STRATUM_PIN=7dd103ddbccabe95dfe1d36fdddb4d6a1bfd99dc
export STRATUM_SRC=$HOME/projects/stratum-astra/v2
FILES="kernel/loom_service_control.c kernel/loom_service_fids.c"
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
pristine() { echo "$OUT/pristine/$(basename "$1")"; }
restore() {
  for f in $FILES; do
    _p=$(pristine "$f")
    [ -f "$_p" ] || continue
    cp "$_p" "$f"
    [ "$(shasum -a 256 "$f" | cut -d' ' -f1)" = "$(shasum -a 256 "$_p" | cut -d' ' -f1)" ] || {
      echo "!! $f did NOT return to pristine"; return 1; }
    git diff --quiet -- "$f" || { echo "!! $f differs from HEAD"; return 1; }
  done
}
on_exit() {
  _rc=$?
  if [ "$MUTATED" = 1 ]; then
    restore && echo "-- $FILES restored and verified against HEAD" || _rc=9
    echo "!! build/ may hold the MUTANT kernel. Rebuild before any other boot:"
    echo "   STRATUM_SRC=$STRATUM_SRC tools/build.sh kernel --config ci"
  fi
  record "exit $_rc"
  exit $_rc
}
trap on_exit EXIT

# The verdict for one test: on its announcement line when it printed nothing,
# else on a line of its own before the next announcement.
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
fails() { /usr/bin/grep -E '^FAIL:|\[test\] [^ ]+ \.\.\. FAIL:' "$1" || true; }
tally() { /usr/bin/grep -E '  tests: [0-9]+/[0-9]+' "$1" | tail -1 | sed -E 's/.*tests: ([0-9]+)\/([0-9]+).*/\1 \2/'; }
run_suite() { # run_suite <label>
  ( set +e; tools/test.sh > "$OUT/$1-test.log" 2>&1; echo $? > "$OUT/$1.rc" )
  suite_rc=$(cat "$OUT/$1.rc")
  [ -f build/test-boot.log ] || { echo "!! no build/test-boot.log after $1"; exit 1; }
  cp build/test-boot.log "$OUT/$1-boot.log"
  record "$1: test.sh exit $suite_rc"
}
inputs() { # inputs <label>: hash what the boot consumed
  for f in build/kernel/thylacine.bin build/kernel/thylacine.elf build/ramfs.cpio build/disk.img; do
    [ -f "$f" ] && shasum -a 256 "$f"
  done > "$OUT/$1-inputs.sha256"
}

echo "=== preconditions ==="
[ -x "$YIP" ] || { echo "REFUSING: no yip at $YIP"; exit 3; }
lease pre
dirty=$(git status --porcelain --untracked-files=no | wc -l | tr -d ' ')
[ "$dirty" = 0 ] || { echo "REFUSING: $dirty tracked modification(s); commit first"; exit 3; }
HEAD_SHA=$(git rev-parse HEAD)
record "HEAD $HEAD_SHA"
for w in "$WANT_CLUNKED" "$WANT_ORPHAN" "$WANT_FIXTURE"; do
  [ -n "$w" ] || { echo "REFUSING: empty prediction"; exit 3; }
  c=$(cat kernel/test/test_loom.c kernel/test/loom_fids_fixture.h | /usr/bin/grep -cF "\"$w\"" || true)
  [ "$c" = 1 ] || { echo "REFUSING: '$w' occurs $c times in the two test sources"; exit 3; }
done
[ "$WANT_CLUNKED" != "$WANT_ORPHAN" ] && [ "$WANT_CLUNKED" != "$WANT_FIXTURE" ] && [ "$WANT_ORPHAN" != "$WANT_FIXTURE" ] || {
  echo "REFUSING: two predictions are identical"; exit 3; }
for t in "$T_FIX" "$T_OPS" "$T_ISO"; do
  /usr/bin/grep -qF "{ \"$t\"," kernel/test/test.c || { echo "REFUSING: $t not registered"; exit 3; }
done
EXPECT=$(/usr/bin/grep -c -E '^[[:space:]]*\{[[:space:]]*"[^"]+"' kernel/test/test.c)
[ "$EXPECT" -ge 1000 ] || { echo "REFUSING: derived only $EXPECT registrations"; exit 3; }
record "expected tests (derived from test.c): $EXPECT"
[ "$(git -C "$STRATUM_SRC" rev-parse HEAD)" = "$STRATUM_PIN" ] || { echo "REFUSING: Stratum HEAD is not $STRATUM_PIN"; exit 3; }
[ -z "$(git -C "$STRATUM_SRC" status --porcelain)" ] || { echo "REFUSING: Stratum tree dirty"; exit 3; }
record "stratum $STRATUM_SRC at $STRATUM_PIN, clean"
mkdir -p "$OUT/pristine"
for f in $FILES; do
  [ "$(/usr/bin/grep -c MUTANT "$f" || true)" = 0 ] || { echo "REFUSING: $f already names a MUTANT"; exit 3; }
  cp "$f" "$(pristine "$f")"
done
floor pre

echo "=== CONTROL ==="
lease control
tools/build.sh kernel --config ci > "$OUT/control-build.log" 2>&1 || { echo "control build failed"; exit 1; }
floor post-control-build
inputs control
CONTROL_BIN=$(shasum -a 256 build/kernel/thylacine.bin | cut -d' ' -f1)
run_suite control
B=$OUT/control-boot.log
/usr/bin/grep -q '^EXTINCTION:' "$B" && { echo "CONTROL EXTINCTED"; exit 1; } || true
set -- $(tally "$B")
[ "${2:-}" = "$EXPECT" ] && [ "${1:-}" = "$EXPECT" ] || { echo "control tally '$*' != $EXPECT/$EXPECT"; fails "$B" | head; exit 1; }
[ "$suite_rc" = 0 ] || { echo "control test.sh exit $suite_rc"; exit 1; }
for t in "$T_FIX" "$T_OPS" "$T_ISO"; do
  v=$(verdict "$B" "$t")
  [ "$v" = PASS ] || { echo "control: $t verdict '$v'"; exit 1; }
done
record "CONTROL GREEN: $EXPECT/$EXPECT, $T_FIX, $T_OPS and $T_ISO PASS, bin ${CONTROL_BIN%"${CONTROL_BIN#????????????????}"}"
SEEN_BINS=$CONTROL_BIN

# mutant <label> <file> <python old> <python new> -- applied to the PRISTINE
# file, built, booted; sets B to its boot log. The caller checks the prediction.
mutant() {
  lease "$1"; floor "pre-$1"
  restore || exit 9
  MUTATED=1
  P=$2 OLD=$3 NEW=$4 LAB=$1 python3 - <<'PY' || exit 5
import os
p = os.environ['P']
s = open(p).read()
old, new, lab = os.environ['OLD'], os.environ['NEW'], os.environ['LAB']
n = s.count(old)
if n != 1:
    raise SystemExit("REFUSING: %s anchor occurs %d times" % (lab, n))
open(p, 'w').write(s.replace(old, new, 1))
PY
  for f in $FILES; do
    _want=0; [ "$f" = "$2" ] && _want=1
    [ "$(/usr/bin/grep -c 'MUTANT' "$f" || true)" = "$_want" ] || { echo "REFUSING: $f MUTANT count is not $_want"; exit 5; }
  done
  diff -u "$(pristine "$2")" "$2" > "$OUT/$1.diff" || true
  tools/build.sh kernel --config ci > "$OUT/$1-build.log" 2>&1 || { echo "$1 build failed"; exit 5; }
  floor "post-$1-build"
  inputs "$1"
  _bin=$(shasum -a 256 build/kernel/thylacine.bin | cut -d' ' -f1)
  case " $SEEN_BINS " in *" $_bin "*) echo "REFUSING: the $1 kernel equals an earlier kernel of this run"; exit 5;; esac
  SEEN_BINS="$SEEN_BINS $_bin"
  mkdir -p "$OUT/$1-artifacts-DO-NOT-BOOT"
  cp build/kernel/thylacine.bin "$OUT/$1-artifacts-DO-NOT-BOOT/"
  echo "MUTANT $1 kernel. Never boot or promote." > "$OUT/$1-artifacts-DO-NOT-BOOT/WARNING.txt"
  run_suite "$1"
  B=$OUT/$1-boot.log
  _ext=$(/usr/bin/grep -E '^EXTINCTION:' "$B" | tr -d '\r')
  [ "$_ext" = "$SUITE_EXT" ] || { echo "$1: the extinction set is not exactly the suite's own:"; /usr/bin/grep -nE '^EXTINCTION:' "$B" | head -4; exit 2; }
  [ "$suite_rc" != 0 ] || { echo "HARNESS: test.sh exit 0 under $1"; exit 2; }
  restore || exit 9
  MUTATED=0
}
expect_fails() { # expect_fails <label> <count> then "<test>|<message>" pairs
  _lab=$1; _n=$2; shift 2
  _t=$(tally "$B")
  [ "$_t" = "$((EXPECT - _n)) $EXPECT" ] || { echo "$_lab tally '$_t', predicted $((EXPECT - _n)) $EXPECT"; fails "$B"; exit 2; }
  for _p in "$@"; do
    _tn=${_p%%|*}; _m=${_p#*|}
    _v=$(verdict "$B" "$_tn")
    [ "$_v" = "FAIL: $_m" ] || { echo "$_lab: $_tn verdict '$_v', predicted 'FAIL: $_m'"; exit 2; }
  done
  _nf=$(fails "$B" | wc -l | tr -d ' ')
  [ "$_nf" = "$_n" ] || { echo "$_lab: $_nf FAIL lines, predicted $_n"; fails "$B"; exit 2; }
}

echo "=== M1: the dispatcher sends a used FID to node_reap_unused ==="
mutant m1 kernel/loom_service_control.c \
'    if(kind==LOOM_SERVICE_FID && state!=LOOM_SERVICE_RESERVED)return loom_private_fid_reap(l,in->object);
' \
'    // MUTANT M1: no FID dispatch
'
expect_fails M1 2 "$T_OPS|$WANT_CLUNKED" "$T_ISO|$WANT_CLUNKED"
v=$(verdict "$B" "$T_FIX"); [ "$v" = PASS ] || { echo "M1: $T_FIX verdict '$v', predicted PASS"; exit 2; }
record "M1 DISCRIMINATED: $((EXPECT - 2))/$EXPECT, exactly $T_OPS and $T_ISO FAIL at '$WANT_CLUNKED'; $T_FIX PASS; extinction set = the suite's own"

echo "=== M3: the REAP lookup refuses a RETIRING FID ==="
mutant m3 kernel/loom_service_control.c \
'    if(!s)s=control_retiring_fid(l,in->object);
' \
'    if(!s && in->flags)s=control_retiring_fid(l,in->object); // MUTANT M3: no RETIRING FID lookup (control_input refused any flags)
'
expect_fails M3 2 "$T_OPS|$WANT_CLUNKED" "$T_ISO|$WANT_CLUNKED"
v=$(verdict "$B" "$T_FIX"); [ "$v" = PASS ] || { echo "M3: $T_FIX verdict '$v', predicted PASS"; exit 2; }
record "M3 DISCRIMINATED: $((EXPECT - 2))/$EXPECT, exactly $T_OPS and $T_ISO FAIL at '$WANT_CLUNKED'; $T_FIX PASS; extinction set = the suite's own"

echo "=== M2: an unclunked FID of a retired scope is refused ==="
mutant m2 kernel/loom_service_fids.c \
'        int rc=loom_fid_retire_aborted(t,nodes,source);
        if(rc)return rc;
' \
'        return -T_E_BUSY; // MUTANT M2: no retirement under a RETIRED scope
'
expect_fails M2 3 "$T_FIX|$WANT_FIXTURE" "$T_OPS|$WANT_ORPHAN" "$T_ISO|$WANT_ORPHAN"
record "M2 DISCRIMINATED: $((EXPECT - 3))/$EXPECT, exactly $T_FIX, $T_OPS and $T_ISO FAIL at their named assertions; extinction set = the suite's own"

echo "=== rebuild the committed tree, then SHAPE ==="
lease rebuild
tools/build.sh kernel --config ci > "$OUT/rebuild.log" 2>&1 || { echo "rebuild failed"; exit 1; }
REB_BIN=$(shasum -a 256 build/kernel/thylacine.bin | cut -d' ' -f1)
[ "$REB_BIN" = "$CONTROL_BIN" ] && record "rebuild byte-identical to the control kernel" \
  || record "rebuild differs from the control kernel (recorded, not claimed identical)"
lease shape; floor pre-shape
( set +e; tools/check-production.sh --all > "$OUT/production.log" 2>&1; echo $? > "$OUT/production.rc" )
[ "$(cat "$OUT/production.rc")" = 0 ] || { echo "SHAPE: check-production.sh --all exit $(cat "$OUT/production.rc")"; tail -20 "$OUT/production.log"; exit 7; }
record "SHAPE: tools/check-production.sh --all exit 0"
echo "-- done: $OUT"
