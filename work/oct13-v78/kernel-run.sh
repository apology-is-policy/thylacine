#!/bin/sh
# Operator votes 7-9 (2026-10-13): the guest legs for the Loom refusal errnos
# (vote 7), the argument-width refusals (vote 8) and the mid-CLUNK REAP/QUERY
# witness (vote 9), in one lease window. Derived from
# work/oct13-na4b/enter-errno-run.sh.
#   CONTROL    the committed tree, ci image: the whole suite passes, every named
#              test PASSes, the probe reports status 70.
#   CONTROL-Q  the qualification image (the lever on, its own kernel dir): the
#              same, probe status 71.
#   M1-M7, M13, M16  one refusal answers the bare -1 again. Each is predicted to
#              fail exactly its kernel pin and, where the probe reaches it, the
#              probe at its own CHECK line.
#   M8-M12     a pre-route or width check is removed or answers -1: the probe
#              alone sees it, at the line its own CHECK names.
#   M10Q       M10 in the qualification image: ordinary_refusals runs on the
#              qualification path too.
#   M14        REAP no longer looks up a RETIRING FID (NA-4b's M3): the native
#              tests now fail FIRST at the mid-CLUNK REAP witness.
#   M15        QUERY sees a RETIRING FID (the option vote 9 rejected): the native
#              tests fail at the mid-CLUNK QUERY witness.
# Each predicted kernel message is checked, before any build, to lie inside the
# function test.c registers under the predicted test name (or, for the native
# tests, inside loom_native_operations_case, which their fixture kinds call).
# Each probe message is checked to occur once in the probe source. Every anchor
# is censused before the control build.
#   SHAPE      tools/check-production.sh --all: the KERNEL_TESTS=OFF kernel builds.
# A failed suite always ends in "EXTINCTION: kernel test suite failed"
# (kernel/main.c): a mutant run must show exactly that extinction and no other.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
STAMP=$(date -u '+%Y%m%dT%H%M%SZ')
OUT=work/oct13-v78/kernel-$STAMP
mkdir -p "$OUT"
YIP="${THYLA_WAKE_YIP:-$(command -v yip || echo "$HOME/.local/bin/yip")}"
FLOOR_MB=${FLOOR_MB:-8192}
T_SET=loom.setup_rejects
T_RES=loom.setup_resource_refusals
T_SQP=loom.sqpoll_charges_thread_budget
T_REG=loom.register_rejects
T_BUF=loom.register_buffers_rejects
T_UND=9p_client.loom_sqpoll_refuses_undeadlined
T_BRG=loom.bridge_probe
T_OPS=loom.private_native_operations
T_ISO=loom.private_native_isolation
T_FIX=loom.service_fid_reap
W_ENTRIES='entries 0 answers -EINVAL'
W_CAP='the page cap answers -ENOMEM'
W_FULL='a full handle table answers -EMFILE'
W_SQP='SQPOLL setup at the thread cap answers -EAGAIN'
W_LISTED='a listed non-Spoor answers -EBADF'
W_EFLAG='ENTER with an unknown flag bit answers -EINVAL'
W_LEN0='len 0 answers -EINVAL'
W_UND='an SQPOLL ring refuses a 9P handle without a recv deadline: -EOPNOTSUPP'
W_BRG="loom-bridge-probe's verdict matches this kernel's qualification lever"
W_REAP_MID='native REAP of a FID whose CLUNK is in flight answers -EBUSY'
W_QUERY_MID='native QUERY of a FID whose CLUNK is in flight answers -ENOENT'
P_KVA='params at a kernel address: -EFAULT'
P_WIDE_ENTRIES='setup entries above 32 bits: -EINVAL'
P_COPYOUT='ordinary setup, read-only params: the copyout faults'
P_LISTED='HANDLES: a listed fd naming nothing: -EBADF'
P_ZLEN='BUFFERS: a zero-length buffer: -EINVAL'
P_EFLAG='ordinary ENTER: an unknown flag: -EINVAL'
P_WIDE_FD='REGISTER fd above 32 bits: -EBADF'
P_WIDE_OP='REGISTER op above 32 bits: -EINVAL'
P_WIDE_SUBMIT='ENTER to_submit above 32 bits: -EINVAL'
QDIR=build/kernel-private-async-qual
QUAL_LINE='PRIVATE-ASYNC QUALIFICATION build'
SUITE_EXT='EXTINCTION: kernel test suite failed'
STRATUM_PIN=7dd103ddbccabe95dfe1d36fdddb4d6a1bfd99dc
export STRATUM_SRC=$HOME/projects/stratum-astra/v2
FILES="kernel/syscall.c kernel/loom.c kernel/loom_service_control.c"
NMUT=17
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
  for f in "$_k/thylacine.bin" "$_k/thylacine.elf" build/ramfs.cpio build/disk.img; do
    [ -f "$f" ] && shasum -a 256 "$f"
  done > "$OUT/$1-inputs.sha256"
}
all_pass() { # all_pass <label> <boot log>: every named test PASSes
  for t in "$T_SET" "$T_RES" "$T_SQP" "$T_REG" "$T_BUF" "$T_UND" "$T_BRG" "$T_OPS" "$T_ISO" "$T_FIX"; do
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
for t in "$T_SET" "$T_RES" "$T_SQP" "$T_REG" "$T_BUF" "$T_UND" "$T_BRG" "$T_OPS" "$T_ISO" "$T_FIX"; do
  /usr/bin/grep -qF "{ \"$t\"," kernel/test/test.c || { echo "REFUSING: $t not registered"; exit 3; }
done
python3 - "$T_SET" "$W_ENTRIES" "$T_RES" "$W_CAP" "$T_RES" "$W_FULL" "$T_SQP" "$W_SQP" \
  "$T_REG" "$W_LISTED" "$T_REG" "$W_EFLAG" "$T_BUF" "$W_LEN0" "$T_UND" "$W_UND" \
  "$T_BRG" "$W_BRG" "$T_OPS" "$W_REAP_MID" "$T_ISO" "$W_REAP_MID" \
  "$T_OPS" "$W_QUERY_MID" "$T_ISO" "$W_QUERY_MID" <<'MEMBER' || exit 3
import re, sys
reg = dict(re.findall(r'\{\s*"([^"]+)",\s*(\w+),', open('kernel/test/test.c').read()))
src = ''.join(open('kernel/test/' + f).read() for f in
              ('test_loom.c', 'test_loom_bridge_probe.c', 'test_9p_client.c'))
def body(fn, ret=r'(?:void|static const char \*)'):
    m = re.search(r'^' + ret + r' ?' + fn + r'\((?:void|[^)]*)\) \{\n(.*?)^\}', src, re.S | re.M)
    return m and m.group(1)
# The native tests reach their assertions through the protocol fixture's kinds.
via = {'loom.private_native_operations': 'loom_protocol_fixture(17)',
       'loom.private_native_isolation': 'kind=100;kind<=101'}
case = body('loom_native_operations_case', r'static const char \*')
dispatch = ('if (kind==17 || kind==18) {error=loom_native_operations_case' in src and
            'if (kind==100 || kind==101) {' in src)
a = sys.argv[1:]
for test, msg in zip(a[0::2], a[1::2]):
    if src.count('"%s"' % msg) != 1:
        raise SystemExit("REFUSING: '%s' occurs %d times" % (msg, src.count('"%s"' % msg)))
    fn = reg.get(test)
    b = fn and body(fn)
    if test in via:
        ok = b and via[test] in b and case and ('"%s"' % msg) in case and dispatch
    else:
        ok = b and ('"%s"' % msg) in b
    if not ok:
        raise SystemExit("REFUSING: '%s' is not reached from %s (registered as %s)" % (msg, fn, test))
    print("member: %s (%s) holds '%s'" % (test, fn, msg))
MEMBER
for w in "$P_KVA" "$P_WIDE_ENTRIES" "$P_COPYOUT" "$P_LISTED" "$P_ZLEN" "$P_EFLAG" \
         "$P_WIDE_FD" "$P_WIDE_OP" "$P_WIDE_SUBMIT"; do
  c=$(/usr/bin/grep -cF "\"$w\"" usr/loom-bridge-probe/loom-bridge-probe.c || true)
  [ "$c" = 1 ] || { echo "REFUSING: probe check '$w' occurs $c times"; exit 3; }
done
record "predictions derived: each kernel message reached from its registered test; nine probe checks present"
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
run_suite control
B=$OUT/control-boot.log
/usr/bin/grep -q '^EXTINCTION:' "$B" && { echo "CONTROL EXTINCTED"; fails "$B" | head; exit 1; } || true
set -- $(tally "$B")
[ "${2:-}" = "$EXPECT" ] && [ "${1:-}" = "$EXPECT" ] || { echo "control tally '$*' != $EXPECT/$EXPECT"; fails "$B" | head; exit 1; }
[ "$suite_rc" = 0 ] || { echo "control test.sh exit $suite_rc"; exit 1; }
all_pass control "$B"
probe_status "$B" 70
/usr/bin/grep -qF "$QUAL_LINE" "$OUT/control-build.log" && { echo "CONTROL: the default build saw the lever"; exit 1; } || true
record "CONTROL GREEN: $EXPECT/$EXPECT, the ten named tests PASS, probe status 70, bin ${CONTROL_BIN%"${CONTROL_BIN#????????????????}"}"
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
all_pass control-q "$B"
probe_status "$B" 71
record "CONTROL-Q GREEN: $EXPECT/$EXPECT, the ten named tests PASS, probe status 71 (qualification path), bin ${Q_BIN%"${Q_BIN#????????????????}"}"
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

echo "=== M1: SETUP's entries check answers -1 ==="
mutant m1 kernel/syscall.c \
'    if (entries == 0 || entries > LOOM_MAX_ENTRIES)  return -T_E_INVAL;
' \
'    if (entries == 0 || entries > LOOM_MAX_ENTRIES)  return -1; // MUTANT M1
'
expect_fails M1 1 "$T_SET|$W_ENTRIES"
ok M1 "exactly $T_SET at '$W_ENTRIES'"
echo "=== M2: SETUP's page-cap refusal answers -1 ==="
mutant m2 kernel/syscall.c \
'        return -T_E_NOMEM;                           // over the per-Proc page cap
' \
'        return -1; // MUTANT M2: over the per-Proc page cap
'
expect_fails M2 1 "$T_RES|$W_CAP"
ok M2 "exactly $T_RES at '$W_CAP'"
echo "=== M3: SETUP's full-table refusal answers -1 ==="
mutant m3 kernel/syscall.c \
'        loom_unref(l);
        loom_unref(l);
        return -T_E_MFILE;
' \
'        loom_unref(l);
        loom_unref(l);
        return -1; // MUTANT M3
'
expect_fails M3 1 "$T_RES|$W_FULL"
ok M3 "exactly $T_RES at '$W_FULL'"
echo "=== M4: SETUP's SQPOLL thread-cap refusal answers -1 ==="
mutant m4 kernel/syscall.c \
'            return -T_E_AGAIN;                       // as a thread create over the cap
' \
'            return -1; // MUTANT M4
'
expect_fails M4 1 "$T_SQP|$W_SQP"
ok M4 "exactly $T_SQP at '$W_SQP'"
echo "=== M5: a listed descriptor that names no Spoor answers -1 ==="
mutant m5 kernel/syscall.c \
'    int rc = -T_E_BADF;   // a listed descriptor names nothing, or not a Spoor
' \
'    int rc = -1; // MUTANT M5
'
expect_fails M5 2 "$T_REG|$W_LISTED" "$T_BRG|$W_BRG"
expect_probe M5 "$P_LISTED" -1
ok M5 "exactly $T_REG at '$W_LISTED' and $T_BRG, the probe at '$P_LISTED'"
echo "=== M6: a buffer that does not resolve answers -1 ==="
mutant m6 kernel/loom.c \
'        if (loom_resolve_buf(p, &bufs[done], &fresh[done]) != 0) { rc = -T_E_INVAL; break; }
' \
'        if (loom_resolve_buf(p, &bufs[done], &fresh[done]) != 0) { rc = -1; break; } // MUTANT M6
'
expect_fails M6 2 "$T_BUF|$W_LEN0" "$T_BRG|$W_BRG"
expect_probe M6 "$P_ZLEN" -1
ok M6 "exactly $T_BUF at '$W_LEN0' and $T_BRG, the probe at '$P_ZLEN'"
echo "=== M7: ENTER's unknown flag answers -1 ==="
mutant m7 kernel/loom.c \
'    if (flags & ~LOOM_ENTER_VALID)    return -T_E_INVAL;
' \
'    if (flags & ~LOOM_ENTER_VALID)    return -1; // MUTANT M7
'
expect_fails M7 2 "$T_REG|$W_EFLAG" "$T_BRG|$W_BRG"
expect_probe M7 "$P_EFLAG" -1
ok M7 "exactly $T_REG at '$W_EFLAG' and $T_BRG, the probe at '$P_EFLAG'"
echo "=== M8: SETUP's pre-route params check answers -1 ==="
mutant m8 kernel/syscall.c \
'    if (!sys_validate_user_buf(params_va, sizeof(struct loom_params))) return -T_E_FAULT;
' \
'    if (!sys_validate_user_buf(params_va, sizeof(struct loom_params))) return -1; // MUTANT M8
'
expect_fails M8 1 "$T_BRG|$W_BRG"
expect_probe M8 "$P_KVA" -1
ok M8 "exactly $T_BRG, the probe at '$P_KVA'"
echo "=== M9: SETUP truncates a wide entries count ==="
mutant m9 kernel/syscall.c \
'    if (entries_raw > 0xffffffffull)                 return -T_E_INVAL;
' \
'    // MUTANT M9: the entries width check dropped
'
expect_fails M9 1 "$T_BRG|$W_BRG"
expect_probe M9 "$P_WIDE_ENTRIES" ANY
ok M9 "exactly $T_BRG, the probe at '$P_WIDE_ENTRIES' (a ring handed out)"
echo "=== M10: a wide descriptor is truncated to an index ==="
mutant m10 kernel/syscall.c \
'    return raw > 0x7fffffffull ? (hidx_t)-1 : (hidx_t)raw;
' \
'    return (hidx_t)raw; // MUTANT M10: truncated
'
expect_fails M10 1 "$T_BRG|$W_BRG"
expect_probe M10 "$P_WIDE_FD" 0
ok M10 "exactly $T_BRG, the probe at '$P_WIDE_FD' (got 0)"
echo "=== M10Q: the same, qualification image ==="
mutant m10q kernel/syscall.c \
'    return raw > 0x7fffffffull ? (hidx_t)-1 : (hidx_t)raw;
' \
'    return (hidx_t)raw; // MUTANT M10Q: truncated
'
expect_fails M10Q 1 "$T_BRG|$W_BRG"
expect_probe M10Q "$P_WIDE_FD" 0
ok M10Q "in the qualification image, exactly $T_BRG, the probe at '$P_WIDE_FD' (got 0)"
echo "=== M11: REGISTER truncates a wide op ==="
mutant m11 kernel/syscall.c \
'    if (op_raw > 0xffffffffull)                      return -T_E_INVAL;
' \
'    // MUTANT M11: the op width check dropped
'
expect_fails M11 1 "$T_BRG|$W_BRG"
expect_probe M11 "$P_WIDE_OP" 0
ok M11 "exactly $T_BRG, the probe at '$P_WIDE_OP' (got 0)"
echo "=== M12: ENTER truncates its wide counts and flags ==="
mutant m12 kernel/syscall.c \
'    if ((to_submit_raw | min_complete_raw | flags_raw) > 0xffffffffull)
        return -T_E_INVAL;
' \
'    // MUTANT M12: the ENTER width check dropped
'
expect_fails M12 1 "$T_BRG|$W_BRG"
expect_probe M12 "$P_WIDE_SUBMIT" 0
ok M12 "exactly $T_BRG, the probe at '$P_WIDE_SUBMIT' (got 0)"
echo "=== M13: SETUP's copyout fault answers -1 ==="
mutant m13 kernel/syscall.c \
'            sys_loom_setup_rollback(p, fd, kp.ring_va, kp.ring_size);
            return -T_E_FAULT;
' \
'            sys_loom_setup_rollback(p, fd, kp.ring_va, kp.ring_size);
            return -1; // MUTANT M13
'
expect_fails M13 1 "$T_BRG|$W_BRG"
expect_probe M13 "$P_COPYOUT" -1
ok M13 "exactly $T_BRG, the probe at '$P_COPYOUT'"
echo "=== M14: REAP no longer finds a RETIRING FID ==="
mutant m14 kernel/loom_service_control.c \
'    if(!s)s=control_retiring_fid(l,in->object);
' \
'    if(!s && in->flags)s=control_retiring_fid(l,in->object); // MUTANT M14: no RETIRING FID lookup (control_input refused any flags)
'
expect_fails M14 2 "$T_OPS|$W_REAP_MID" "$T_ISO|$W_REAP_MID"
still_pass M14 "$T_FIX"
ok M14 "exactly $T_OPS and $T_ISO at '$W_REAP_MID'; $T_FIX PASS"
echo "=== M15: QUERY reports a RETIRING FID ==="
mutant m15 kernel/loom_service_control.c \
'    struct loom_service_slot *s=control_exact(l,in->object);
    if(!s)rc=-T_E_NOENT;
    else if(s->kind==LOOM_SERVICE_TARGET)' \
'    struct loom_service_slot *s=control_exact(l,in->object);
    if(!s)s=control_retiring_fid(l,in->object); // MUTANT M15: QUERY sees a RETIRING FID
    if(!s)rc=-T_E_NOENT;
    else if(s->kind==LOOM_SERVICE_TARGET)'
expect_fails M15 2 "$T_OPS|$W_QUERY_MID" "$T_ISO|$W_QUERY_MID"
still_pass M15 "$T_FIX"
ok M15 "exactly $T_OPS and $T_ISO at '$W_QUERY_MID'; $T_FIX PASS"
echo "=== M16: an SQPOLL ring's undeadlined 9P handle answers -1 ==="
mutant m16 kernel/loom.c \
'                !p9_client_recv_is_deadline_capable(cl)) {
                return -T_E_OPNOTSUPP;
' \
'                !p9_client_recv_is_deadline_capable(cl)) {
                return -1; // MUTANT M16
'
expect_fails M16 1 "$T_UND|$W_UND"
ok M16 "exactly $T_UND at '$W_UND'"
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
