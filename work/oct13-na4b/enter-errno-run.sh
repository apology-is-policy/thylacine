#!/bin/sh
# NA-4b audit round 5 F1, operator vote 6: Loom's ENTER and legacy REGISTER
# answer -T_E_BADF for a descriptor that names no ring, and an unknown REGISTER
# op -T_E_INVAL (they answered a bare -1, against ERRORS.md). The guest legs,
# in one lease window:
#   CONTROL  the committed tree: the whole suite passes, loom.register_rejects,
#            loom.register_buffers_rejects and loom.bridge_probe PASS.
#   E1  ENTER's dead-descriptor check answers -1 again. Predicted: exactly
#       register_rejects ("ENTER on a bad descriptor answers -EBADF") and
#       bridge_probe (its EL0 ENTER on a closed ring's number) FAIL.
#   E2  ENTER's non-Loom check answers -1. Predicted: exactly register_rejects
#       at "ENTER on a non-Loom descriptor answers -EBADF".
#   R1  the legacy HANDLES REGISTER's dead-descriptor check answers -1.
#       Predicted: register_rejects at "bad loom_fd answers -EBADF" and
#       bridge_probe (its EL0 REGISTER on a closed ring's number).
#   R2  its non-Loom check answers -1. Predicted: register_rejects at
#       "non-Loom loom_fd answers -EBADF".
#   R3  the legacy BUFFERS REGISTER's dead-descriptor check answers -1.
#       Predicted: register_buffers_rejects at "BUFFERS on a bad loom_fd
#       answers -EBADF".
#   R4  its non-Loom check answers -1. Predicted: register_buffers_rejects at
#       "BUFFERS on a non-Loom loom_fd answers -EBADF".
#   U1  an unknown REGISTER op answers -1. Predicted: bridge_probe only.
#   CONTROL-Q and U1Q  the same two in the qualification image (the lever on,
#       its own kernel dir): the probe runs its qualification path, whose op-10
#       check must agree with the kernel (audit round 7 F1).
# A bridge_probe failure prints one kernel message whichever EL0 check failed,
# so each mutant that fails it also names the probe's own FAIL line.
# Each predicted message is checked, before any build, to lie inside the
# function test.c registers under the predicted test name (the first run
# named loom.register_handles, a test that holds none of these assertions).
#   SHAPE    tools/check-production.sh --all: the KERNEL_TESTS=OFF kernel builds.
# A failed suite always ends in "EXTINCTION: kernel test suite failed"
# (kernel/main.c): a mutant run must show exactly that extinction and no other.
# Mac axis, one boot per run. Derived from work/oct13-na4b/kernel-run.sh.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
STAMP=$(date -u '+%Y%m%dT%H%M%SZ')
OUT=work/oct13-na4b/enter-errno-$STAMP
mkdir -p "$OUT"
YIP="${THYLA_WAKE_YIP:-$(command -v yip || echo "$HOME/.local/bin/yip")}"
FLOOR_MB=${FLOOR_MB:-8192}
T_REG=loom.register_rejects
T_BUF=loom.register_buffers_rejects
T_BRG=loom.bridge_probe
W_ENTER_BAD='ENTER on a bad descriptor answers -EBADF'
W_ENTER_KIND='ENTER on a non-Loom descriptor answers -EBADF'
W_REG_BAD='bad loom_fd answers -EBADF'
W_REG_KIND='non-Loom loom_fd answers -EBADF'
W_BUF_BAD='BUFFERS on a bad loom_fd answers -EBADF'
W_BUF_KIND='BUFFERS on a non-Loom loom_fd answers -EBADF'
W_BRG="loom-bridge-probe's verdict matches this kernel's qualification lever"
P_ENTER="ENTER on a closed ring's number is -EBADF (got -1)"
P_REG="REGISTER on a closed ring's number is -EBADF (got -1)"
P_UNK="an unknown register op is -EINVAL (got -1)"
P_UNK_Q="op 10 is no private op: -EINVAL (got -1)"
QDIR=build/kernel-private-async-qual
QUAL_LINE='PRIVATE-ASYNC QUALIFICATION build'
SUITE_EXT='EXTINCTION: kernel test suite failed'
STRATUM_PIN=7dd103ddbccabe95dfe1d36fdddb4d6a1bfd99dc
export STRATUM_SRC=$HOME/projects/stratum-astra/v2
FILES="kernel/syscall.c"
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
    echo "   STRATUM_SRC=$STRATUM_SRC tools/build.sh kernel --config ci [--set PRIVATE_ASYNC_QUAL=y]"
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
run_suite() { # run_suite <label> [test.sh args]
  _l=$1; shift
  rm -f build/test-boot.log
  ( set +e; tools/test.sh "$@" > "$OUT/$_l-test.log" 2>&1; echo $? > "$OUT/$_l.rc" )
  suite_rc=$(cat "$OUT/$_l.rc")
  [ -f build/test-boot.log ] || { echo "!! no build/test-boot.log after $_l"; exit 1; }
  cp build/test-boot.log "$OUT/$_l-boot.log"
  record "$_l: test.sh exit $suite_rc"
}
probe_status() { # probe_status <boot log> <status>: the probe's verdict line, once
  _c=$(/usr/bin/grep -cF "loom-bridge-probe status=$2 expected=$2" "$1" || true)
  [ "$_c" = 1 ] || { echo "no single 'status=$2 expected=$2' in $1"; /usr/bin/grep -F 'loom-bridge-probe' "$1" | head -4; exit 1; }
}
inputs() { # inputs <label> [kernel dir]: hash what the boot consumed
  _k=${2:-build/kernel}
  for f in "$_k/thylacine.bin" "$_k/thylacine.elf" build/ramfs.cpio build/disk.img; do
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
for w in "$W_ENTER_BAD" "$W_ENTER_KIND" "$W_REG_BAD" "$W_REG_KIND" "$W_BUF_BAD" "$W_BUF_KIND" "$W_BRG"; do
  [ -n "$w" ] || { echo "REFUSING: empty prediction"; exit 3; }
  c=$(cat kernel/test/test_loom.c kernel/test/test_loom_bridge_probe.c | /usr/bin/grep -cF "\"$w\"" || true)
  [ "$c" = 1 ] || { echo "REFUSING: '$w' occurs $c times in the two test sources"; exit 3; }
done
for t in "$T_REG" "$T_BUF" "$T_BRG"; do
  /usr/bin/grep -qF "{ \"$t\"," kernel/test/test.c || { echo "REFUSING: $t not registered"; exit 3; }
done
python3 - "$T_REG" "$W_ENTER_BAD" "$T_REG" "$W_ENTER_KIND" "$T_REG" "$W_REG_BAD" \
  "$T_REG" "$W_REG_KIND" "$T_BUF" "$W_BUF_BAD" "$T_BUF" "$W_BUF_KIND" "$T_BRG" "$W_BRG" <<'MEMBER' || exit 3
import re, sys
reg = dict(re.findall(r'\{\s*"([^"]+)",\s*(\w+),', open('kernel/test/test.c').read()))
src = open('kernel/test/test_loom.c').read() + open('kernel/test/test_loom_bridge_probe.c').read()
a = sys.argv[1:]
for test, msg in zip(a[0::2], a[1::2]):
    fn = reg.get(test)
    m = fn and re.search(r'^void ' + fn + r'\(void\) \{\n(.*?)^\}', src, re.S | re.M)
    if not m or ('"%s"' % msg) not in m.group(1):
        raise SystemExit("REFUSING: '%s' is not inside %s (registered as %s)" % (msg, fn, test))
    print("member: %s (%s) holds '%s'" % (test, fn, msg))
MEMBER
for w in "$P_ENTER" "$P_REG" "$P_UNK" "$P_UNK_Q"; do
  c=$(/usr/bin/grep -cF "\"${w% (got -1)}\"" usr/loom-bridge-probe/loom-bridge-probe.c || true)
  [ "$c" = 1 ] || { echo "REFUSING: probe check '${w% (got -1)}' occurs $c times"; exit 3; }
done
record "predictions derived: each message inside its registered test's function; four probe checks present"
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
# Every mutant's anchor must occur exactly once in the committed file BEFORE
# the control is built: a rotten anchor found after the control wastes the
# lease (it did, 2026-10-13T09:07Z: E1's anchor occurred 0 times).
ANCHORS=$(awk '/^mutant [a-z0-9]+ kernel\/syscall.c \\$/ {print $2}' "$0" | tr '\n' ' ')
[ "$(echo $ANCHORS | wc -w | tr -d ' ')" = 8 ] || { echo "REFUSING: found mutants '$ANCHORS', expected 8"; exit 3; }
python3 - "$0" kernel/syscall.c <<'CENSUS' || exit 3
import re, sys
script, target = open(sys.argv[1]).read(), open(sys.argv[2]).read()
calls = re.findall(r"^mutant (\w+) kernel/syscall\.c \\\n'(.*?)' \\\n'(.*?)'\n", script, re.S | re.M)
bad = 0
for lab, old, new in calls:
    n, m = target.count(old), target.count(new)
    print("census %s: anchor x%d, mutant text x%d, MUTANT in new: %s" % (lab, n, m, "MUTANT" in new))
    if n != 1 or m != 0 or "MUTANT" not in new: bad += 1
if len(calls) != 8 or bad:
    raise SystemExit("REFUSING: census %d calls, %d bad" % (len(calls), bad))
CENSUS
record "anchor census: 8 mutants, each anchor once in kernel/syscall.c"
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
for t in "$T_REG" "$T_BUF" "$T_BRG"; do
  v=$(verdict "$B" "$t")
  [ "$v" = PASS ] || { echo "control: $t verdict '$v'"; exit 1; }
done
probe_status "$B" 70
/usr/bin/grep -qF "$QUAL_LINE" "$OUT/control-build.log" && { echo "CONTROL: the default build saw the lever"; exit 1; } || true
record "CONTROL GREEN: $EXPECT/$EXPECT, $T_REG, $T_BUF and $T_BRG PASS, probe status 70, bin ${CONTROL_BIN%"${CONTROL_BIN#????????????????}"}"
echo "=== CONTROL-Q ==="
lease control-q
tools/build.sh kernel --config ci --set PRIVATE_ASYNC_QUAL=y > "$OUT/control-q-build.log" 2>&1 || { echo "control-q build failed"; exit 1; }
/usr/bin/grep -qF "$QUAL_LINE" "$OUT/control-q-build.log" || { echo "CONTROL-Q: cmake never saw the lever"; exit 1; }
floor post-control-q-build
inputs control-q "$QDIR"
Q_BIN=$(shasum -a 256 "$QDIR/thylacine.bin" | cut -d' ' -f1)
[ "$Q_BIN" != "$CONTROL_BIN" ] || { echo "CONTROL-Q: the qualification kernel equals the default one"; exit 1; }
[ "$(shasum -a 256 build/kernel/thylacine.bin | cut -d' ' -f1)" = "$CONTROL_BIN" ] || { echo "CONTROL-Q: the qual build replaced the default kernel"; exit 1; }
run_suite control-q --private-async-qual
B=$OUT/control-q-boot.log
/usr/bin/grep -q '^EXTINCTION:' "$B" && { echo "CONTROL-Q EXTINCTED"; fails "$B" | head; exit 1; } || true
set -- $(tally "$B")
[ "${2:-}" = "$EXPECT" ] && [ "${1:-}" = "$EXPECT" ] || { echo "control-q tally '$*' != $EXPECT/$EXPECT"; fails "$B" | head; exit 1; }
[ "$suite_rc" = 0 ] || { echo "control-q test.sh exit $suite_rc"; exit 1; }
for t in "$T_REG" "$T_BUF" "$T_BRG"; do
  v=$(verdict "$B" "$t")
  [ "$v" = PASS ] || { echo "control-q: $t verdict '$v'"; exit 1; }
done
probe_status "$B" 71
record "CONTROL-Q GREEN: $EXPECT/$EXPECT, $T_REG, $T_BUF and $T_BRG PASS, probe status 71 (qualification path), bin ${Q_BIN%"${Q_BIN#????????????????}"}"
SEEN_BINS="$CONTROL_BIN $Q_BIN"
# mutant <label> <file> <python old> <python new> -- applied to the PRISTINE
# file, built, booted; sets B to its boot log. The caller checks the prediction.
# A label ending in q builds and boots the qualification image.
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
  case $1 in
    *q) _kd=$QDIR; _set="--set PRIVATE_ASYNC_QUAL=y"; _ta=--private-async-qual ;;
    *)  _kd=build/kernel; _set=; _ta= ;;
  esac
  tools/build.sh kernel --config ci $_set > "$OUT/$1-build.log" 2>&1 || { echo "$1 build failed"; exit 5; }
  if [ -n "$_set" ]; then
    /usr/bin/grep -qF "$QUAL_LINE" "$OUT/$1-build.log" || { echo "$1: cmake never saw the lever"; exit 5; }
  else
    ! /usr/bin/grep -qF "$QUAL_LINE" "$OUT/$1-build.log" || { echo "$1: the default build saw the lever"; exit 5; }
  fi
  floor "post-$1-build"
  inputs "$1" "$_kd"
  _bin=$(shasum -a 256 "$_kd/thylacine.bin" | cut -d' ' -f1)
  case " $SEEN_BINS " in *" $_bin "*) echo "REFUSING: the $1 kernel equals an earlier kernel of this run"; exit 5;; esac
  SEEN_BINS="$SEEN_BINS $_bin"
  mkdir -p "$OUT/$1-artifacts-DO-NOT-BOOT"
  cp "$_kd/thylacine.bin" "$OUT/$1-artifacts-DO-NOT-BOOT/"
  echo "MUTANT $1 kernel. Never boot or promote." > "$OUT/$1-artifacts-DO-NOT-BOOT/WARNING.txt"
  run_suite "$1" $_ta
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
expect_probe() { # expect_probe <label> <why>: the probe's one FAIL line names <why>
  _pl=$(/usr/bin/grep -F 'loom-bridge-probe: FAIL line ' "$B" | tr -d '\r')
  [ "$(printf '%s\n' "$_pl" | /usr/bin/grep -c .)" = 1 ] || { echo "$1: probe FAIL lines: '$_pl'"; exit 2; }
  case "$_pl" in *": $2") ;; *) echo "$1: probe said '$_pl', predicted ': $2'"; exit 2;; esac
}

echo "=== E1: ENTER answers -1 for a dead descriptor ==="
mutant e1 kernel/syscall.c \
'    if (handle_get(p, loom_fd, &lh) != 0)  return -T_E_BADF;
    if (lh.kind != KOBJ_LOOM)              { handle_put(&lh); return -T_E_BADF; }
    struct Loom *l = (struct Loom *)lh.obj;
    int rc = loom_is_private(l) ? loom_private_enter(l, p, to_submit, min_complete, flags)
' \
'    if (handle_get(p, loom_fd, &lh) != 0)  return -1; // MUTANT E1
    if (lh.kind != KOBJ_LOOM)              { handle_put(&lh); return -T_E_BADF; }
    struct Loom *l = (struct Loom *)lh.obj;
    int rc = loom_is_private(l) ? loom_private_enter(l, p, to_submit, min_complete, flags)
'
expect_fails E1 2 "$T_REG|$W_ENTER_BAD" "$T_BRG|$W_BRG"
expect_probe E1 "$P_ENTER"
record "E1 DISCRIMINATED: $((EXPECT - 2))/$EXPECT, exactly $T_REG and $T_BRG FAIL at their named assertions, the probe at '$P_ENTER'; extinction set = the suite's own"
echo "=== E2: ENTER answers -1 for a non-Loom descriptor ==="
mutant e2 kernel/syscall.c \
'    if (handle_get(p, loom_fd, &lh) != 0)  return -T_E_BADF;
    if (lh.kind != KOBJ_LOOM)              { handle_put(&lh); return -T_E_BADF; }
    struct Loom *l = (struct Loom *)lh.obj;
    int rc = loom_is_private(l) ? loom_private_enter(l, p, to_submit, min_complete, flags)
' \
'    if (handle_get(p, loom_fd, &lh) != 0)  return -T_E_BADF;
    if (lh.kind != KOBJ_LOOM)              { handle_put(&lh); return -1; } // MUTANT E2
    struct Loom *l = (struct Loom *)lh.obj;
    int rc = loom_is_private(l) ? loom_private_enter(l, p, to_submit, min_complete, flags)
'
expect_fails E2 1 "$T_REG|$W_ENTER_KIND"
record "E2 DISCRIMINATED: $((EXPECT - 1))/$EXPECT, exactly $T_REG FAILs at '$W_ENTER_KIND'; extinction set = the suite's own"
echo "=== R1: HANDLES REGISTER answers -1 for a dead descriptor ==="
mutant r1 kernel/syscall.c \
'    if (handle_get(p, loom_fd, &lh) != 0)  return -T_E_BADF;
    if (lh.kind != KOBJ_LOOM)              { handle_put(&lh); return -T_E_BADF; }
    struct Loom *l = (struct Loom *)lh.obj;
    // A private ring names its services through registered targets instead.
' \
'    if (handle_get(p, loom_fd, &lh) != 0)  return -1; // MUTANT R1
    if (lh.kind != KOBJ_LOOM)              { handle_put(&lh); return -T_E_BADF; }
    struct Loom *l = (struct Loom *)lh.obj;
    // A private ring names its services through registered targets instead.
'
expect_fails R1 2 "$T_REG|$W_REG_BAD" "$T_BRG|$W_BRG"
expect_probe R1 "$P_REG"
record "R1 DISCRIMINATED: $((EXPECT - 2))/$EXPECT, exactly $T_REG and $T_BRG FAIL at their named assertions, the probe at '$P_REG'; extinction set = the suite's own"
echo "=== R2: HANDLES REGISTER answers -1 for a non-Loom descriptor ==="
mutant r2 kernel/syscall.c \
'    if (handle_get(p, loom_fd, &lh) != 0)  return -T_E_BADF;
    if (lh.kind != KOBJ_LOOM)              { handle_put(&lh); return -T_E_BADF; }
    struct Loom *l = (struct Loom *)lh.obj;
    // A private ring names its services through registered targets instead.
' \
'    if (handle_get(p, loom_fd, &lh) != 0)  return -T_E_BADF;
    if (lh.kind != KOBJ_LOOM)              { handle_put(&lh); return -1; } // MUTANT R2
    struct Loom *l = (struct Loom *)lh.obj;
    // A private ring names its services through registered targets instead.
'
expect_fails R2 1 "$T_REG|$W_REG_KIND"
record "R2 DISCRIMINATED: $((EXPECT - 1))/$EXPECT, exactly $T_REG FAILs at '$W_REG_KIND'; extinction set = the suite's own"
echo "=== R3: BUFFERS REGISTER answers -1 for a dead descriptor ==="
mutant r3 kernel/syscall.c \
'    if (n > 0 && !bufs)               return -1;

    struct Handle lh;
    if (handle_get(p, loom_fd, &lh) != 0)  return -T_E_BADF;
' \
'    if (n > 0 && !bufs)               return -1;

    struct Handle lh;
    if (handle_get(p, loom_fd, &lh) != 0)  return -1; // MUTANT R3
'
expect_fails R3 1 "$T_BUF|$W_BUF_BAD"
record "R3 DISCRIMINATED: $((EXPECT - 1))/$EXPECT, exactly $T_BUF FAILs at '$W_BUF_BAD'; extinction set = the suite's own"
echo "=== R4: BUFFERS REGISTER answers -1 for a non-Loom descriptor ==="
mutant r4 kernel/syscall.c \
'    if (n > 0 && !bufs)               return -1;

    struct Handle lh;
    if (handle_get(p, loom_fd, &lh) != 0)  return -T_E_BADF;
    if (lh.kind != KOBJ_LOOM)              { handle_put(&lh); return -T_E_BADF; }
' \
'    if (n > 0 && !bufs)               return -1;

    struct Handle lh;
    if (handle_get(p, loom_fd, &lh) != 0)  return -T_E_BADF;
    if (lh.kind != KOBJ_LOOM)              { handle_put(&lh); return -1; } // MUTANT R4
'
expect_fails R4 1 "$T_BUF|$W_BUF_KIND"
record "R4 DISCRIMINATED: $((EXPECT - 1))/$EXPECT, exactly $T_BUF FAILs at '$W_BUF_KIND'; extinction set = the suite's own"
echo "=== U1: an unknown REGISTER op answers -1 ==="
mutant u1 kernel/syscall.c \
'    return -T_E_INVAL;   // unknown register op
' \
'    return -1;   // MUTANT U1: unknown register op
'
expect_fails U1 1 "$T_BRG|$W_BRG"
expect_probe U1 "$P_UNK"
record "U1 DISCRIMINATED: $((EXPECT - 1))/$EXPECT, exactly $T_BRG FAILs, the probe at '$P_UNK'; extinction set = the suite's own"
echo "=== U1Q: an unknown REGISTER op answers -1, qualification image ==="
mutant u1q kernel/syscall.c \
'    return -T_E_INVAL;   // unknown register op
' \
'    return -1;   // MUTANT U1Q: unknown register op
'
expect_fails U1Q 1 "$T_BRG|$W_BRG"
expect_probe U1Q "$P_UNK_Q"
record "U1Q DISCRIMINATED: $((EXPECT - 1))/$EXPECT in the qualification image, exactly $T_BRG FAILs, the probe at '$P_UNK_Q'; extinction set = the suite's own"
echo "=== rebuild the committed tree, then SHAPE ==="
lease rebuild
tools/build.sh kernel --config ci > "$OUT/rebuild.log" 2>&1 || { echo "rebuild failed"; exit 1; }
REB_BIN=$(shasum -a 256 build/kernel/thylacine.bin | cut -d' ' -f1)
[ "$REB_BIN" = "$CONTROL_BIN" ] && record "rebuild byte-identical to the control kernel" \
  || record "rebuild differs from the control kernel (recorded, not claimed identical)"
tools/build.sh kernel --config ci --set PRIVATE_ASYNC_QUAL=y > "$OUT/rebuild-q.log" 2>&1 || { echo "rebuild-q failed"; exit 1; }
REB_Q=$(shasum -a 256 "$QDIR/thylacine.bin" | cut -d' ' -f1)
[ "$REB_Q" = "$Q_BIN" ] && record "rebuild-q byte-identical to CONTROL-Q" \
  || record "rebuild-q differs from CONTROL-Q (recorded, not claimed identical)"
lease shape; floor pre-shape
( set +e; tools/check-production.sh --all > "$OUT/production.log" 2>&1; echo $? > "$OUT/production.rc" )
[ "$(cat "$OUT/production.rc")" = 0 ] || { echo "SHAPE: check-production.sh --all exit $(cat "$OUT/production.rc")"; tail -20 "$OUT/production.log"; exit 7; }
record "SHAPE: tools/check-production.sh --all exit 0"
echo "-- done: $OUT"
