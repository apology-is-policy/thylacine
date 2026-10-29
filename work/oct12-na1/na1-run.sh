#!/bin/sh
# NA-1 guest legs (the private Loom syscall bridge), in one lease window:
#   CONTROL-D  default --config ci kernel: the whole suite passes; the bridge's
#              DEFAULT body (everything refuses EOPNOTSUPP) and the four new
#              legacy/frontend witnesses PASS; cmake did NOT see the lever.
#   CONTROL-Q  the same with PRIVATE_ASYNC_QUAL=y, in its own kernel dir: the
#              whole suite passes, the bridge's QUALIFICATION body PASSes; cmake
#              DID see the lever; its kernel differs from CONTROL-D's.
#   M3 M4 M5   default-image mutants (work/oct12-na1/na1-mutants.py): each FAILs
#              exactly its predicted tests at their named assertions, and the
#              only extinction is the suite's own.
#   M6         qualification-image mutant: predicted to EXTINCT with the bridge's
#              own invariant ("private register lost its reservation rollback")
#              while loom.private_bridge is the running test. It is also the
#              proof that --private-async-qual booted the qualification kernel:
#              no log line says which kernel ran, and a default boot cannot
#              reach this extinction.
#   M7         qualification-image mutant: SYS_WEFT_SHARE admits the private
#              ring, so loom.private_bridge FAILs at its weft-share assertion.
#   REBUILD    both kernels from the committed tree, compared with the controls.
#   GUARD      the derived sq_head single-writer guard (SQ-1), re-taken.
#   SHAPE      KERNEL_TESTS-off compiles of the touched files; no setup hook
#              symbol survives; the bridge compiles with the lever on as well.
# Not proven here: userspace reaching the syscalls (the handlers' usercopy is
# exercised only from EL0, owed to NA-2), SMP, sanitizers. Mac axis, one boot each.
# Derived from work/oct12-sq1/sq1-run.sh.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
STAMP=$(date -u '+%Y%m%dT%H%M%SZ')
OUT=work/oct12-na1/run-$STAMP
mkdir -p "$OUT/pristine"
YIP="${THYLA_WAKE_YIP:-$(command -v yip || echo "$HOME/.local/bin/yip")}"
FLOOR_MB=${FLOOR_MB:-8192}
SUITE_EXT='EXTINCTION: kernel test suite failed'
STRATUM_PIN=7dd103ddbccabe95dfe1d36fdddb4d6a1bfd99dc
export STRATUM_SRC=$HOME/projects/stratum-astra/v2
MUT=work/oct12-na1/na1-mutants.py
FILES=$(python3 "$MUT" --files)
QDIR=build/kernel-private-async-qual
QUAL_LINE='PRIVATE-ASYNC QUALIFICATION build'
NEWTESTS='loom.setup_close_race loom.private_sq_release_after_copy loom.private_sq_release_after_index loom.private_bridge loom.setup_rollback_settles'
SQ1TESTS='loom.sq_release_after_copy loom.sq_release_after_index loom.sq_release_after_copy_chain'
W3A='the private original SQE was copied before its release'
W3B='the out-of-range private original index is counted dropped'
W4='the Loom outlives a close that lands at install'
W5A="the rollback settles the ring's charge exactly"
W5B="the ordinary ring's rollback settles its charge exactly"
W6='private register lost its reservation rollback'
W7='a private ring cannot be weft-shared'
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
green() { # green <label> <boot log> <tests...>: full pass + each named test PASS
  _l=$1; _b=$2; shift 2
  /usr/bin/grep -q '^EXTINCTION:' "$_b" && { echo "$_l EXTINCTED"; /usr/bin/grep '^EXTINCTION:' "$_b"; exit 1; } || true
  _t=$(tally "$_b")
  [ "$_t" = "$EXPECT $EXPECT" ] || { echo "$_l tally '$_t' != $EXPECT/$EXPECT"; fails "$_b" | head; exit 1; }
  [ "$suite_rc" = 0 ] || { echo "$_l test.sh exit $suite_rc"; exit 1; }
  for _x in "$@"; do
    _v=$(verdict "$_b" "$_x"); [ "$_v" = PASS ] || { echo "$_l: $_x verdict '$_v'"; exit 1; }
  done
}

echo "=== preconditions ==="
[ -x "$YIP" ] || { echo "REFUSING: no yip at $YIP"; exit 3; }
lease pre
dirty=$(git status --porcelain --untracked-files=no | wc -l | tr -d ' ')
[ "$dirty" = 0 ] || { echo "REFUSING: $dirty tracked modification(s); commit first"; exit 3; }
record "HEAD $(git rev-parse HEAD)"
python3 "$MUT" --check > "$OUT/anchors.txt" || { cat "$OUT/anchors.txt"; echo "REFUSING: mutant anchors"; exit 3; }
for w in "$W3A" "$W3B" "$W4" "$W5A" "$W5B" "$W6" "$W7"; do
  c=$(cat kernel/test/*.c kernel/test/*.h kernel/*.c | /usr/bin/grep -cF "\"$w\"" || true)
  [ "$c" = 1 ] || { echo "REFUSING: '$w' occurs $c times"; exit 3; }
done
for t in $NEWTESTS $SQ1TESTS; do
  /usr/bin/grep -qF "{ \"$t\"," kernel/test/test.c || { echo "REFUSING: $t not registered"; exit 3; }
done
EXPECT=$(/usr/bin/grep -c -E '^[[:space:]]*\{[[:space:]]*"[^"]+"' kernel/test/test.c)
[ "$EXPECT" -ge 1000 ] || { echo "REFUSING: derived only $EXPECT registrations"; exit 3; }
record "expected tests (derived from test.c): $EXPECT"
[ "$(git -C "$STRATUM_SRC" rev-parse HEAD)" = "$STRATUM_PIN" ] || { echo "REFUSING: Stratum HEAD is not $STRATUM_PIN"; exit 3; }
[ -z "$(git -C "$STRATUM_SRC" status --porcelain)" ] || { echo "REFUSING: Stratum tree dirty"; exit 3; }
bash tools/test-build-config.sh > "$OUT/build-config-test.log" 2>&1 || { echo "REFUSING: build-config test"; tail -5 "$OUT/build-config-test.log"; exit 3; }
record "build-config test: $(tail -1 "$OUT/build-config-test.log")"
for f in $FILES; do cp "$f" "$OUT/pristine/$(basename "$f")"; done
floor pre

echo "=== CONTROL-D ==="
lease control-d
build control-d default
/usr/bin/grep -qF "$QUAL_LINE" "$OUT/control-d-build.log" && { echo "CONTROL-D: the default build saw the lever"; exit 1; } || true
floor post-control-d-build
inputs control-d build/kernel
D_BIN=$(shasum -a 256 build/kernel/thylacine.bin | cut -d' ' -f1)
run_suite control-d
green CONTROL-D "$OUT/control-d-boot.log" $NEWTESTS $SQ1TESTS
record "CONTROL-D GREEN: $EXPECT/$EXPECT, the default bridge body and four witnesses PASS"

echo "=== CONTROL-Q ==="
lease control-q
build control-q qual
/usr/bin/grep -qF "$QUAL_LINE" "$OUT/control-q-build.log" || { echo "CONTROL-Q: cmake never saw the lever"; exit 1; }
floor post-control-q-build
inputs control-q "$QDIR"
Q_BIN=$(shasum -a 256 "$QDIR/thylacine.bin" | cut -d' ' -f1)
[ "$Q_BIN" != "$D_BIN" ] || { echo "CONTROL-Q: the qualification kernel equals the default one"; exit 1; }
[ "$(shasum -a 256 build/kernel/thylacine.bin | cut -d' ' -f1)" = "$D_BIN" ] || { echo "CONTROL-Q: the qual build replaced the default kernel"; exit 1; }
run_suite control-q --private-async-qual
green CONTROL-Q "$OUT/control-q-boot.log" $NEWTESTS $SQ1TESTS
record "CONTROL-Q GREEN: $EXPECT/$EXPECT, the qualification bridge body PASSes; default kernel untouched"
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
expect_fails() { # expect_fails <label> <count> "<test>|<message>"...
  _lab=$1; _n=$2; shift 2
  _ext=$(/usr/bin/grep -E '^EXTINCTION:' "$B" | tr -d '\r')
  [ "$_ext" = "$SUITE_EXT" ] || { echo "$_lab: the extinction set is not exactly the suite's own:"; /usr/bin/grep -nE '^EXTINCTION:' "$B" | head -4; exit 2; }
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

echo "=== M3: the frontend releases sq_head before the copy ==="
mutant m3 default
expect_fails M3 2 "loom.private_sq_release_after_copy|$W3A" "loom.private_sq_release_after_index|$W3B"
record "M3 DISCRIMINATED: exactly the two frontend witnesses FAIL at their named assertions"

echo "=== M4: the legacy setup holds no reference across handle_alloc ==="
# The freed Loom's page is untouched (the buddy keeps its lists in struct page and
# never poisons), so setup's post-install stores land harmlessly -- unless another
# CPU allocates that page inside the window. Then the leg can exit 2 with garbage
# instead: re-run it once before treating that as a kernel finding.
mutant m4 default
expect_fails M4 1 "loom.setup_close_race|$W4"
record "M4 DISCRIMINATED: exactly loom.setup_close_race FAILs at its named assertion"

echo "=== M5: the setup rollback uses the unsettled unmap ==="
mutant m5 default
expect_fails M5 2 "loom.setup_rollback_settles|$W5A" "loom.private_bridge|$W5B"
record "M5 DISCRIMINATED: exactly the two rollback witnesses FAIL at their named assertions"

echo "=== M6: a private reservation published before its result copyout ==="
mutant m6 qual
_ext=$(/usr/bin/grep -E '^EXTINCTION:' "$B" | tr -d '\r')
[ "$_ext" = "EXTINCTION: $W6" ] || { echo "M6: extinction set '$_ext', predicted 'EXTINCTION: $W6'"; exit 2; }
_last=$(/usr/bin/grep -oE '\[test\] [^ ]+ \.\.\.' "$B" | tail -1)
[ "$_last" = "[test] loom.private_bridge ..." ] || { echo "M6: the running test was '$_last'"; exit 2; }
record "M6 DISCRIMINATED: the qual boot extincts with exactly '$W6' inside loom.private_bridge"

echo "=== M7: weft share admits a private ring ==="
mutant m7 qual
expect_fails M7 1 "loom.private_bridge|$W7"
record "M7 DISCRIMINATED: exactly loom.private_bridge FAILs at its weft-share assertion"

echo "=== REBUILD both kernels from the committed tree ==="
lease rebuild
build rebuild-d default
build rebuild-q qual
RD=$(shasum -a 256 build/kernel/thylacine.bin | cut -d' ' -f1)
RQ=$(shasum -a 256 "$QDIR/thylacine.bin" | cut -d' ' -f1)
[ "$RD" = "$D_BIN" ] && record "rebuild-d byte-identical to CONTROL-D" || record "rebuild-d differs from CONTROL-D (recorded, not claimed identical)"
[ "$RQ" = "$Q_BIN" ] && record "rebuild-q byte-identical to CONTROL-Q" || record "rebuild-q differs from CONTROL-Q (recorded, not claimed identical)"

echo "=== GUARD: sq_head single writer ==="
PROD=$(find kernel mm arch -name '*.[ch]' -not -path 'kernel/test/*' | sort)
NPROD=$(echo "$PROD" | wc -l | tr -d ' ')
[ "$NPROD" -ge 100 ] || { echo "REFUSING: only $NPROD production sources found"; exit 7; }
echo "$PROD" | xargs /usr/bin/grep -nE '__atomic_(store|exchange|compare_exchange|fetch_[a-z]+|[a-z]+_fetch)(_n)?\(&[^,]*sq_head|sq_head *(\+\+|\+=|-=|=[^=])|(\+\+|--) *[A-Za-z_>.-]*sq_head' > "$OUT/sq_head-writers.txt" || true
FN0=$(/usr/bin/grep -n '^void loom_sq_release(' kernel/loom.c | cut -d: -f1)
FN1=$(awk -v a="$FN0" 'NR > a && /^}/ {print NR; exit}' kernel/loom.c)
NW=$(wc -l < "$OUT/sq_head-writers.txt" | tr -d ' ')
OUTSIDE=$(awk -F: -v a="$FN0" -v b="$FN1" '!($1 == "kernel/loom.c" && $2 > a && $2 < b)' "$OUT/sq_head-writers.txt")
[ "$NW" = 2 ] && [ -z "$OUTSIDE" ] || { echo "GUARD failed:"; cat "$OUT/sq_head-writers.txt"; exit 7; }
record "GUARD: $NPROD production sources; both sq_head writes inside loom_sq_release (loom.c:$FN0-$FN1)"

echo "=== SHAPE: KERNEL_TESTS off ==="
SH=$OUT/shape; mkdir -p "$SH"; SHA=$(cd "$SH" && pwd -P)
CCX=$(sed -n 's/^set(CMAKE_C_COMPILER "\(.*\)")$/\1/p' build/kernel/CMakeFiles/*/CMakeCCompiler.cmake | head -1)
TOOLS=$(dirname "$CCX")
FM=build/kernel/kernel/CMakeFiles/thylacine.elf.dir/flags.make
DEF=$(/usr/bin/grep '^C_DEFINES' "$FM" | cut -d= -f2- | tr ' ' '\n' | /usr/bin/grep -vx -- '-DKERNEL_TESTS' | tr '\n' ' ')
INC=$(/usr/bin/grep '^C_INCLUDES' "$FM" | cut -d= -f2-)
FL=$(/usr/bin/grep '^C_FLAGS' "$FM" | cut -d= -f2-)
case " $DEF " in *KERNEL_TESTS*) echo "REFUSING: KERNEL_TESTS survived"; exit 7;; esac
for f in loom syscall loom_service_syscall loom_service_frontend; do
  for q in "" "-DLOOM_PRIVATE_QUALIFICATION"; do
    o="$SHA/$f${q:+-qual}.o"
    ( cd build/kernel/kernel && eval "\"$CCX\" $DEF $q $INC -iquote \"$ROOT/kernel\" $FL -c \"$ROOT/kernel/$f.c\" -o \"$o\"" ) \
      >> "$SH/compile.log" 2>&1 || { echo "REFUSING: production compile of $f.c $q failed"; exit 7; }
    "$TOOLS/llvm-nm" "$o" > "${o%.o}.nm"
  done
done
/usr/bin/grep -q 'loom_private_setup_for_proc' "$SH/loom_service_syscall.nm" || { echo "REFUSING: nm shows no loom_private_setup_for_proc"; exit 7; }
if cat "$SH"/*.nm | /usr/bin/grep -E 'setup_installed|sq_release_hook'; then echo "a test hook survived production"; exit 7; fi
record "SHAPE: 4 files x {default, lever} compile with KERNEL_TESTS off; no test hook symbol survives"
echo "-- done: $OUT"
