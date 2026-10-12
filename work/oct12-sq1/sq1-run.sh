#!/bin/sh
# SQ-1 guest legs, in one lease window:
#   CONTROL  the committed tree: loom.sq_release_after_copy and
#            loom.sq_release_after_index PASS, the whole suite passes.
#   M1       loom_sq_release moved back above the SQ-index read -- the ordering
#            loom_drain_sq shipped through a7353c36f. Predicted: exactly those two
#            tests FAIL, each at its own named assertion, and nothing else.
#   SHAPE    kernel/loom.c compiled with KERNEL_TESTS off: no hook symbol, and
#            the production loom_drain_sq disassembly retained for the record.
# Not proven here: real userspace scheduling (the producer is a deterministic
# hook at the release), SMP, sanitizers. Mac axis, one boot per run.
# Derived from work/oct5-as-r9/inner-leg-run.sh (lease, floor, pristine restore,
# verdict-block parsing); the Stratum pin is the CURRENT clean stratum-astra/v2.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
STAMP=$(date -u '+%Y%m%dT%H%M%SZ')
OUT=work/oct12-sq1/run-$STAMP
mkdir -p "$OUT"
YIP="${THYLA_WAKE_YIP:-$(command -v yip || echo "$HOME/.local/bin/yip")}"
FLOOR_MB=${FLOOR_MB:-8192}
T_COPY=loom.sq_release_after_copy
T_INDEX=loom.sq_release_after_index
WANT_COPY='the original SQE was copied before its release'
WANT_INDEX='an out-of-range original index is not consumed as the refill'
STRATUM_PIN=7dd103ddbccabe95dfe1d36fdddb4d6a1bfd99dc
export STRATUM_SRC=$HOME/projects/stratum-astra/v2
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
PRISTINE=$OUT/loom.c.pristine
restore() {
  [ -f "$PRISTINE" ] || return 0
  cp "$PRISTINE" kernel/loom.c
  [ "$(shasum -a 256 kernel/loom.c | cut -d' ' -f1)" = "$HASH_LOOM" ] || {
    echo "!! kernel/loom.c did NOT return to pristine"; return 1; }
  git diff --quiet -- kernel/loom.c || { echo "!! kernel/loom.c differs from HEAD"; return 1; }
}
on_exit() {
  _rc=$?
  if [ "$MUTATED" = 1 ]; then
    restore && echo "-- kernel/loom.c restored and hash-verified" || _rc=9
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
for w in "$WANT_COPY" "$WANT_INDEX"; do
  [ -n "$w" ] || { echo "REFUSING: empty prediction"; exit 3; }
  c=$(/usr/bin/grep -cF "\"$w\"" kernel/test/test_loom.c || true)
  [ "$c" = 1 ] || { echo "REFUSING: '$w' occurs $c times in test_loom.c"; exit 3; }
done
[ "$WANT_COPY" != "$WANT_INDEX" ] || { echo "REFUSING: identical predictions"; exit 3; }
for t in "$T_COPY" "$T_INDEX"; do
  /usr/bin/grep -qF "{ \"$t\"," kernel/test/test.c || { echo "REFUSING: $t not registered"; exit 3; }
done
EXPECT=$(/usr/bin/grep -c -E '^[[:space:]]*\{[[:space:]]*"[^"]+"' kernel/test/test.c)
[ "$EXPECT" -ge 1000 ] || { echo "REFUSING: derived only $EXPECT registrations"; exit 3; }
record "expected tests (derived from test.c): $EXPECT"
[ "$(git -C "$STRATUM_SRC" rev-parse HEAD)" = "$STRATUM_PIN" ] || { echo "REFUSING: Stratum HEAD is not $STRATUM_PIN"; exit 3; }
[ -z "$(git -C "$STRATUM_SRC" status --porcelain)" ] || { echo "REFUSING: Stratum tree dirty"; exit 3; }
record "stratum $STRATUM_SRC at $STRATUM_PIN, clean"
cp kernel/loom.c "$PRISTINE"
HASH_LOOM=$(shasum -a 256 "$PRISTINE" | cut -d' ' -f1)
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
for t in "$T_COPY" "$T_INDEX"; do
  v=$(verdict "$B" "$t")
  [ "$v" = PASS ] || { echo "control: $t verdict '$v'"; exit 1; }
done
record "CONTROL GREEN: $EXPECT/$EXPECT, both SQ-1 tests PASS, bin ${CONTROL_BIN%"${CONTROL_BIN#????????????????}"}"

echo "=== M1: release before the index read ==="
lease m1
floor pre-m1
MUTATED=1
python3 - <<'PY' || exit 5
p = 'kernel/loom.c'
s = open(p).read()
old = """        u32 ring_pos = l->sq_head & (l->sq_entries - 1u);   // kernel-private mask
        u32 sqe_idx = __atomic_load_n(&sq_array[ring_pos], __ATOMIC_ACQUIRE);
        have = (sqe_idx < l->sq_entries);
        if (have) ksqe = sqes[sqe_idx];                     // COPY to kernel memory (TOCTOU)
        else __atomic_store_n(&h->dropped, h->dropped + 1u, __ATOMIC_RELEASE);
        loom_sq_release(l, h);                              // only after the copy
"""
new = """        u32 ring_pos = l->sq_head & (l->sq_entries - 1u);   // kernel-private mask
        loom_sq_release(l, h);                              // MUTANT M1: release before the copy
        u32 sqe_idx = __atomic_load_n(&sq_array[ring_pos], __ATOMIC_ACQUIRE);
        have = (sqe_idx < l->sq_entries);
        if (have) ksqe = sqes[sqe_idx];                     // COPY to kernel memory (TOCTOU)
        else __atomic_store_n(&h->dropped, h->dropped + 1u, __ATOMIC_RELEASE);
"""
n = s.count(old)
if n != 1:
    raise SystemExit("REFUSING: M1 anchor occurs %d times" % n)
open(p, 'w').write(s.replace(old, new, 1))
PY
[ "$(/usr/bin/grep -c 'MUTANT' kernel/loom.c)" = 1 ] || { echo "REFUSING: not exactly one MUTANT site"; exit 5; }
diff -u "$PRISTINE" kernel/loom.c > "$OUT/m1.diff" || true
tools/build.sh kernel --config ci > "$OUT/m1-build.log" 2>&1 || { echo "m1 build failed"; exit 5; }
floor post-m1-build
inputs m1
M1_BIN=$(shasum -a 256 build/kernel/thylacine.bin | cut -d' ' -f1)
[ "$M1_BIN" != "$CONTROL_BIN" ] || { echo "REFUSING: M1 kernel identical to control"; exit 5; }
mkdir -p "$OUT/m1-artifacts-DO-NOT-BOOT"
cp build/kernel/thylacine.bin "$OUT/m1-artifacts-DO-NOT-BOOT/"
echo "MUTANT M1 kernel. Never boot or promote." > "$OUT/m1-artifacts-DO-NOT-BOOT/WARNING.txt"
run_suite m1
B=$OUT/m1-boot.log
/usr/bin/grep -q '^EXTINCTION:' "$B" && { echo "M1 EXTINCTED: not the prediction"; exit 2; } || true
[ "$suite_rc" != 0 ] || { echo "HARNESS: test.sh exit 0 under M1"; exit 2; }
set -- $(tally "$B")
[ "${2:-}" = "$EXPECT" ] && [ "${1:-}" = "$((EXPECT - 2))" ] || { echo "M1 tally '$*', predicted $((EXPECT - 2))/$EXPECT"; fails "$B"; exit 2; }
v=$(verdict "$B" "$T_COPY");  [ "$v" = "FAIL: $WANT_COPY" ]  || { echo "M1: $T_COPY verdict '$v'"; exit 2; }
v=$(verdict "$B" "$T_INDEX"); [ "$v" = "FAIL: $WANT_INDEX" ] || { echo "M1: $T_INDEX verdict '$v'"; exit 2; }
nf=$(fails "$B" | wc -l | tr -d ' ')
[ "$nf" = 2 ] || { echo "M1: $nf FAIL lines, predicted 2"; fails "$B"; exit 2; }
restore || exit 9
MUTATED=0
record "M1 DISCRIMINATED: $((EXPECT - 2))/$EXPECT, exactly $T_COPY and $T_INDEX FAIL at their named assertions"

echo "=== rebuild the committed tree, then SHAPE ==="
lease rebuild
tools/build.sh kernel --config ci > "$OUT/rebuild.log" 2>&1 || { echo "rebuild failed"; exit 1; }
REB_BIN=$(shasum -a 256 build/kernel/thylacine.bin | cut -d' ' -f1)
[ "$REB_BIN" = "$CONTROL_BIN" ] && record "rebuild byte-identical to the control kernel" \
  || record "rebuild differs from the control kernel (recorded, not claimed identical)"
SH=$OUT/shape; mkdir -p "$SH"
CCX=$(sed -n 's/^set(CMAKE_C_COMPILER "\(.*\)")$/\1/p' build/kernel/CMakeFiles/*/CMakeCCompiler.cmake | head -1)
TOOLS=$(dirname "$CCX")
FM=build/kernel/kernel/CMakeFiles/thylacine.elf.dir/flags.make
DEF=$(/usr/bin/grep '^C_DEFINES' "$FM" | cut -d= -f2- | tr ' ' '\n' | /usr/bin/grep -vx -- '-DKERNEL_TESTS' | tr '\n' ' ')
INC=$(/usr/bin/grep '^C_INCLUDES' "$FM" | cut -d= -f2-)
FL=$(/usr/bin/grep '^C_FLAGS' "$FM" | cut -d= -f2-)
case " $DEF " in *KERNEL_TESTS*) echo "REFUSING: KERNEL_TESTS survived"; exit 7;; esac
SHA=$(cd "$SH" && pwd -P)
cp kernel/loom.c "$SH/loom.c"
( cd build/kernel/kernel && eval "\"$CCX\" $DEF $INC -iquote \"$ROOT/kernel\" $FL -c \"$SHA/loom.c\" -o \"$SHA/loom.o\"" ) \
  > "$SH/compile.log" 2>&1 || { echo "REFUSING: production compile failed"; exit 7; }
"$TOOLS/llvm-nm" "$SH/loom.o" > "$SH/loom.nm"
/usr/bin/grep -q 'loom_enter' "$SH/loom.nm" || { echo "REFUSING: nm shows no loom_enter -- nm did not read the object"; exit 7; }
if /usr/bin/grep -E 'sq_release_hook' "$SH/loom.nm"; then echo "a test hook survived production"; exit 7; fi
"$TOOLS/llvm-objdump" -d -r "$SH/loom.o" > "$SH/loom.dis"
record "SHAPE: no sq_release hook symbol with KERNEL_TESTS off; disassembly retained"
echo "-- done: $OUT"
