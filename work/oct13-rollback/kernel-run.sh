#!/bin/sh
# The Loom setup-rollback identity chunk (2026-10-13): the guest legs, in one
# lease window. Derived from work/oct13-v78/kernel-run.sh.
#   CONTROL    the committed tree, ci image: the whole suite passes, every named
#              test PASSes, the probe reports status 70.
#   CONTROL-Q  the qualification image (the lever on, its own kernel dir): the
#              same, probe status 71.
#   M1/M1Q     handle_close_obj closes whatever the slot holds: the rollback
#              closes the file that reused the ring's number (ordinary ring;
#              in the qualification image the private ring too).
#   M2/M2Q     the ring detach removes the range with no identity check: the
#              rollback removes the region a sibling mapped at the ring's VA.
#   M3         a setup refused for a slot refunds the ring explicitly as well:
#              the refund lands twice, and a trimmed ring's surviving mapping
#              is left uncharged while it is mapped.
# Each predicted kernel message is checked, before any build, to lie inside the
# function test.c registers under the predicted test name, or inside the
# fixture that function calls. Each mutant anchor is censused before the
# control build. Every leg's loom-bridge-probe binary must hash equal to its
# control's: ramfs.cpio differs on every bake (the random pool key), the probe
# must not.
#   SHAPE      tools/check-production.sh --all: the KERNEL_TESTS=OFF kernel builds.
# A failed suite always ends in "EXTINCTION: kernel test suite failed"
# (kernel/main.c): a mutant run must show exactly that extinction and no other.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
STAMP=$(date -u '+%Y%m%dT%H%M%SZ')
OUT=work/oct13-rollback/kernel-$STAMP
mkdir -p "$OUT"
YIP="${THYLA_WAKE_YIP:-$(command -v yip || echo "$HOME/.local/bin/yip")}"
FLOOR_MB=${FLOOR_MB:-8192}
T_RB=loom.setup_rollback_settles
T_ID=loom.setup_rollback_identity
T_PID=loom.private_setup_rollback_identity
T_RES=loom.setup_resource_refusals
T_CLR=loom.setup_close_race
T_REG=loom.register_rejects
T_BRG=loom.bridge_probe
T_PRB=loom.private_bridge
W_NUMBER="the rollback spares a file that reused the ring's number"
W_VA="the rollback spares a region mapped at the ring's VA"
W_TRIM="a refused setup's ring stays charged while a mapping of it survives"
W_REFUND='and its pages were refunded'
W_PNUMBER="the private rollback spares a file that reused the ring's number"
W_PVA="the private rollback spares a region mapped at the ring's VA"
PROBE_BIN=build/ramfs-src/bin/loom-bridge-probe
QDIR=build/kernel-private-async-qual
QUAL_LINE='PRIVATE-ASYNC QUALIFICATION build'
SUITE_EXT='EXTINCTION: kernel test suite failed'
STRATUM_PIN=7dd103ddbccabe95dfe1d36fdddb4d6a1bfd99dc
export STRATUM_SRC=$HOME/projects/stratum-astra/v2
FILES="kernel/handle.c kernel/syscall.c"
NMUT=5
MUTATED=0
record() { echo "$(date -u '+%Y-%m-%dT%H:%M:%SZ')  $*" | tee -a "$OUT/stages.txt"; }
free_mb() { df -m . | awk 'NR==2 {print $4}'; }
floor() {
  _f=$(free_mb); record "disk $1: $_f MiB free (floor $FLOOR_MB)"
  [ "$_f" -ge "$FLOOR_MB" ] || { echo "REFUSING at $1: under the floor"; exit 3; }
}
lease() {
  [ -z "${DRYRUN:-}" ] || return 0
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
  for f in "$_k/thylacine.bin" "$_k/thylacine.elf" build/ramfs.cpio build/disk.img "$PROBE_BIN"; do
    [ -f "$f" ] && shasum -a 256 "$f"
  done > "$OUT/$1-inputs.sha256"
}
probe_hash() { shasum -a 256 "$PROBE_BIN" | cut -d' ' -f1; }
probe_same() { # probe_same <label> <reference hash>: the probe this leg booted is the control's
  _ph=$(probe_hash)
  [ "$_ph" = "$2" ] || { echo "$1: loom-bridge-probe hash $_ph differs from its control's $2"; exit 2; }
}
all_pass() { # all_pass <label> <boot log>: every named test PASSes
  for t in $NAMED; do
    v=$(verdict "$2" "$t")
    [ "$v" = PASS ] || { echo "$1: $t verdict '$v'"; exit 1; }
  done
}

echo "=== preconditions ==="
[ -x "$YIP" ] || { echo "REFUSING: no yip at $YIP"; exit 3; }
lease pre
dirty=$(git status --porcelain --untracked-files=no | wc -l | tr -d ' ')
[ "$dirty" = 0 ] || { echo "REFUSING: $dirty tracked modification(s); commit first"; exit 3; }
HEAD_SHA=$(git rev-parse HEAD)
record "HEAD $HEAD_SHA"
NAMED="$T_RB $T_ID $T_PID $T_RES $T_CLR $T_REG $T_BRG $T_PRB"
for t in $NAMED; do
  /usr/bin/grep -qF "{ \"$t\"," kernel/test/test.c || { echo "REFUSING: $t not registered"; exit 3; }
done
python3 - "$T_ID" "$W_NUMBER" "$T_ID" "$W_VA" "$T_ID" "$W_TRIM" "$T_RES" "$W_REFUND" \
  "$T_PID" "$W_PNUMBER" "$T_PID" "$W_PVA" <<'MEMBER' || exit 3
import re, sys
reg = dict(re.findall(r'\{\s*"([^"]+)",\s*(\w+),', open('kernel/test/test.c').read()))
src = ''.join(open('kernel/test/' + f).read() for f in ('test_loom.c', 'loom_bridge_fixture.h'))
def body(fn, ret=r'(?:void|static const char \*)'):
    m = re.search(r'^' + ret + r' ?' + fn + r'\((?:void|[^)]*)\) \{\n(.*?)^\}', src, re.S | re.M)
    return m and m.group(1)
a = sys.argv[1:]
for test, msg in zip(a[0::2], a[1::2]):
    q = '"%s"' % msg
    if src.count(q) != 1:
        raise SystemExit("REFUSING: '%s' occurs %d times" % (msg, src.count(q)))
    fn = reg.get(test)
    b = fn and body(fn)
    ok = b and q in b
    if b and not ok:
        # one level of fixture: the test body calls a fixture whose body holds it
        for fx in re.findall(r'(\w+_fixture_run)\(\)', b):
            fb = body(fx, r'static const char \*')
            if fb and q in fb:
                ok = True; fn = fn + ' -> ' + fx
    if not ok:
        raise SystemExit("REFUSING: '%s' is not reached from %s (registered as %s)" % (msg, fn, test))
    print("member: %s (%s) holds '%s'" % (test, fn, msg))
MEMBER
record "predictions derived: each kernel message reached from its registered test or its fixture"
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
# Every mutant's anchor must occur exactly once in its committed file BEFORE the
# control is built: a rotten anchor found after the control wastes the lease.
python3 - "$0" "$NMUT" <<'CENSUS' || exit 3
import re, sys
script, want = open(sys.argv[1]).read(), int(sys.argv[2])
calls = re.findall(r"^mutant (\w+) (kernel/\S+) \\\n'(.*?)' \\\n'(.*?)'\n", script, re.S | re.M)
bad = 0
for lab, path, old, new in calls:
    target = open(path).read()
    n, m = target.count(old), target.count(new)
    print("census %s %s: anchor x%d, mutant text x%d, MUTANT in new: %s" % (lab, path, n, m, "MUTANT" in new))
    if n != 1 or m != 0 or "MUTANT" not in new: bad += 1
if len(calls) != want or bad or len(set(c[0] for c in calls)) != len(calls):
    raise SystemExit("REFUSING: census %d calls (want %d), %d bad" % (len(calls), want, bad))
CENSUS
record "anchor census: $NMUT mutants, each anchor once in its file"
[ -z "${DRYRUN:-}" ] || { record "DRYRUN: the preconditions hold; stopping before any build"; exit 0; }
floor pre
echo "=== CONTROL ==="
lease control
tools/build.sh kernel --config ci > "$OUT/control-build.log" 2>&1 || { echo "control build failed"; exit 1; }
floor post-control-build
inputs control
CONTROL_BIN=$(shasum -a 256 build/kernel/thylacine.bin | cut -d' ' -f1)
CONTROL_PROBE=$(probe_hash)
run_suite control
B=$OUT/control-boot.log
/usr/bin/grep -q '^EXTINCTION:' "$B" && { echo "CONTROL EXTINCTED"; fails "$B" | head; exit 1; } || true
set -- $(tally "$B")
[ "${2:-}" = "$EXPECT" ] && [ "${1:-}" = "$EXPECT" ] || { echo "control tally '$*' != $EXPECT/$EXPECT"; fails "$B" | head; exit 1; }
[ "$suite_rc" = 0 ] || { echo "control test.sh exit $suite_rc"; exit 1; }
all_pass control "$B"
probe_status "$B" 70
/usr/bin/grep -qF "$QUAL_LINE" "$OUT/control-build.log" && { echo "CONTROL: the default build saw the lever"; exit 1; } || true
record "CONTROL GREEN: $EXPECT/$EXPECT, the eight named tests PASS, probe status 70, probe ${CONTROL_PROBE%"${CONTROL_PROBE#????????????????}"}, bin ${CONTROL_BIN%"${CONTROL_BIN#????????????????}"}"
echo "=== CONTROL-Q ==="
lease control-q
tools/build.sh kernel --config ci --set PRIVATE_ASYNC_QUAL=y > "$OUT/control-q-build.log" 2>&1 || { echo "control-q build failed"; exit 1; }
/usr/bin/grep -qF "$QUAL_LINE" "$OUT/control-q-build.log" || { echo "CONTROL-Q: cmake never saw the lever"; exit 1; }
floor post-control-q-build
inputs control-q "$QDIR"
Q_BIN=$(shasum -a 256 "$QDIR/thylacine.bin" | cut -d' ' -f1)
Q_PROBE=$(probe_hash)
[ "$Q_BIN" != "$CONTROL_BIN" ] || { echo "CONTROL-Q: the qualification kernel equals the default one"; exit 1; }
[ "$(shasum -a 256 build/kernel/thylacine.bin | cut -d' ' -f1)" = "$CONTROL_BIN" ] || { echo "CONTROL-Q: the qual build replaced the default kernel"; exit 1; }
run_suite control-q --private-async-qual
B=$OUT/control-q-boot.log
/usr/bin/grep -q '^EXTINCTION:' "$B" && { echo "CONTROL-Q EXTINCTED"; fails "$B" | head; exit 1; } || true
set -- $(tally "$B")
[ "${2:-}" = "$EXPECT" ] && [ "${1:-}" = "$EXPECT" ] || { echo "control-q tally '$*' != $EXPECT/$EXPECT"; fails "$B" | head; exit 1; }
[ "$suite_rc" = 0 ] || { echo "control-q test.sh exit $suite_rc"; exit 1; }
all_pass control-q "$B"
probe_status "$B" 71
record "CONTROL-Q GREEN: $EXPECT/$EXPECT, the eight named tests PASS, probe status 71 (qualification path), probe ${Q_PROBE%"${Q_PROBE#????????????????}"}, bin ${Q_BIN%"${Q_BIN#????????????????}"}"
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
    *q) _kd=$QDIR; _set="--set PRIVATE_ASYNC_QUAL=y"; _ta=--private-async-qual; _pr=$Q_PROBE ;;
    *)  _kd=build/kernel; _set=; _ta=; _pr=$CONTROL_PROBE ;;
  esac
  tools/build.sh kernel --config ci $_set > "$OUT/$1-build.log" 2>&1 || { echo "$1 build failed"; exit 5; }
  if [ -n "$_set" ]; then
    /usr/bin/grep -qF "$QUAL_LINE" "$OUT/$1-build.log" || { echo "$1: cmake never saw the lever"; exit 5; }
  else
    ! /usr/bin/grep -qF "$QUAL_LINE" "$OUT/$1-build.log" || { echo "$1: the default build saw the lever"; exit 5; }
  fi
  floor "post-$1-build"
  inputs "$1" "$_kd"
  probe_same "$1" "$_pr"
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
# The probe's one FAIL line names <why> by MESSAGE, and its value: <got>, or ANY
# non-negative number (a descriptor the refused call handed out instead).
expect_probe() { # expect_probe <label> <why> <got|ANY>
  _pl=$(/usr/bin/grep -F 'loom-bridge-probe: FAIL line ' "$B" | tr -d '\r')
  [ "$(printf '%s\n' "$_pl" | /usr/bin/grep -c .)" = 1 ] || { echo "$1: probe FAIL lines: '$_pl'"; exit 2; }
  _pre=${_pl%% (got *}; _got=${_pl##* (got }; _got=${_got%)}
  case "$_pre" in *": $2") ;; *) echo "$1: probe said '$_pl', predicted ': $2'"; exit 2;; esac
  if [ "$3" = ANY ]; then
    case "$_got" in ''|*[!0-9]*) echo "$1: probe value '$_got', predicted a non-negative number"; exit 2;; esac
  else
    [ "$_got" = "$3" ] || { echo "$1: probe value '$_got', predicted '$3'"; exit 2; }
  fi
}
still_pass() { # still_pass <label> <test>...: these tests PASS under the mutant
  _lab=$1; shift
  for _t in "$@"; do
    _v=$(verdict "$B" "$_t"); [ "$_v" = PASS ] || { echo "$_lab: $_t verdict '$_v', predicted PASS"; exit 2; }
  done
}
ok() { record "$1 DISCRIMINATED: $2; extinction set = the suite's own"; }

echo "=== M1: handle_close_obj closes whatever the slot holds ==="
mutant m1 kernel/handle.c \
'    if (!obj) return -1;
    return handle_close_matching(p, h, kind, obj);
' \
'    if (!obj) return -1;
    return handle_close_matching(p, h, KOBJ_INVALID, NULL); // MUTANT M1: no identity
'
expect_fails M1 1 "$T_ID|$W_NUMBER"
ok M1 "exactly $T_ID at '$W_NUMBER'"
echo "=== M1Q: the same, qualification image ==="
mutant m1q kernel/handle.c \
'    if (!obj) return -1;
    return handle_close_matching(p, h, kind, obj);
' \
'    if (!obj) return -1;
    return handle_close_matching(p, h, KOBJ_INVALID, NULL); // MUTANT M1Q: no identity
'
expect_fails M1Q 2 "$T_ID|$W_NUMBER" "$T_PID|$W_PNUMBER"
ok M1Q "in the qualification image, exactly $T_ID at '$W_NUMBER' and $T_PID at '$W_PNUMBER'"
echo "=== M2: the ring detach removes the range, no identity check ==="
mutant m2 kernel/syscall.c \
'    if (v && v->burrow == l->ring && v->vaddr_start == ring_va &&
        v->vaddr_end == ring_va + (u64)ring_size)
' \
'    (void)v; // MUTANT M2: a range detach, whatever is mapped there
'
expect_fails M2 1 "$T_ID|$W_VA"
ok M2 "exactly $T_ID at '$W_VA'"
echo "=== M2Q: the same, qualification image ==="
mutant m2q kernel/syscall.c \
'    if (v && v->burrow == l->ring && v->vaddr_start == ring_va &&
        v->vaddr_end == ring_va + (u64)ring_size)
' \
'    (void)v; // MUTANT M2Q: a range detach, whatever is mapped there
'
expect_fails M2Q 2 "$T_ID|$W_VA" "$T_PID|$W_PVA"
ok M2Q "in the qualification image, exactly $T_ID at '$W_VA' and $T_PID at '$W_PVA'"
echo "=== M3: a setup refused for a slot also refunds the ring explicitly ==="
mutant m3 kernel/syscall.c \
'        (void)sys_loom_ring_detach(p, l, vaddr, ring_size);
        loom_unref(l);
        loom_unref(l);
        return -T_E_MFILE;
' \
'        (void)sys_loom_ring_detach(p, l, vaddr, ring_size);
        proc_page_uncharge(p, ring_pages); // MUTANT M3: an explicit refund beside the claim
        loom_unref(l);
        loom_unref(l);
        return -T_E_MFILE;
'
expect_fails M3 2 "$T_RES|$W_REFUND" "$T_ID|$W_TRIM"
ok M3 "exactly $T_RES at '$W_REFUND' and $T_ID at '$W_TRIM'"
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
