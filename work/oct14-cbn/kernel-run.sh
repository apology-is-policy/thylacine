#!/bin/sh
# The close-by-number sweep (vote 10, slot reservation; 2026-10-14): the guest
# legs, TLC and the mutants, in one lease window. Derived from
# work/oct13-width/kernel-run.sh.
#   CONTROL    the committed tree, ci image: the whole suite passes, every named
#              test PASSes, argwidth-probe reports 72 and loom-bridge-probe 70.
#   CONTROL-Q  the qualification image: the same, loom-bridge-probe 71.
#   TLC        handle_slots.cfg clean; each handle_slots buggy cfg violates the
#              invariant it names; handles.cfg + handles_srv.cfg clean; each
#              handles buggy cfg still finds its counterexample.
#   M1         handle_dup_to stops answering BUSY: dup3 takes a reservation.
#   M2         handle_install_locked's scan takes a reserved slot.
#   M3         close vacates a reserved slot.
#   M4         pipe on a full table publishes the read end, then closes it.
#   M5         pipe2 installs both ends before the copy-out, closes on a fault.
#   M6         the dup arm's refusal keeps its alias row.
#   M7         the F_DUPFD arm's refusal keeps its alias row.
#   M8         the close hook drops a reserved number's row (viv_socktab_drop).
#   M9         a kept Spoor (mkdirat, fchmodat) is installed, then closed.
#   M10        O_DIRECTORY refuses after an install.
#   M11        accept's failed listen open leaks its reservation.
#   M12        socket's claim refusal installs, then closes by number.
#   M13        handle_replace ignores `expect`.
#   M14        connect keys its swap on whatever the number names now.
#   M15        connect stops carrying ctl's O_NONBLOCK onto data.
#   M16        the close hook asks the handle table under the socket table's
#              lock (r1 F1's ABBA): the leaf guard extincts in the first test
#              that closes through the hook with a socket table.
#   M17        the socket arms read the row before the slot (r1 F2).
# Each predicted kernel message is checked, before any build, to lie inside the
# function test.c registers under the predicted test name. Each mutant anchor is
# censused before the control build. Both probes must hash equal to their
# control's in every leg (a kernel mutant must not move userspace).
#   SHAPE      tools/check-production.sh --all: the KERNEL_TESTS=OFF kernel builds.
# A failed suite always ends in "EXTINCTION: kernel test suite failed"
# (kernel/main.c): a mutant run must show exactly that extinction and no other.
# An EXT mutant (mutant_ext) must instead show exactly its predicted extinction,
# no suite tally, and the predicted test as the last one announced.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
STAMP=$(date -u '+%Y%m%dT%H%M%SZ')
OUT=work/oct14-cbn/kernel-$STAMP
mkdir -p "$OUT"
YIP="${THYLA_WAKE_YIP:-$(command -v yip || echo "$HOME/.local/bin/yip")}"
FLOOR_MB=${FLOOR_MB:-8192}
T_RES=handles.reservation
T_REP=handles.replace
T_PIPE=sys_pipe.full_table_publishes_nothing
T_UNW=vivarium.unwinds_publish_nothing
T_CLO=vivarium.close_keeps_a_reserved_row
T_FIN=stalk.open_finish_policy
T_CON=vivsock.connect_keeps_a_reused_number
T_SOC=vivsock.socket_unwind_publishes_nothing
W_RES_BUSY="dup3 onto it is BUSY"
W_RES_ALLOC="an alloc skips the reserved number"
W_RES_CLOSE="close refuses it (Linux: EBADF)"
W_REP="a slot that no longer names the caller's object is refused"
W_PIPE="the refused pipe installed nothing"
W_UNW_P2="pipe2: a faulted copy-out installed nothing"
W_UNW_ROWS="dup, F_DUPFD: the alias rows dropped, the source's kept"
W_UNW_ACC="accept: the number returned"
W_UNW_D3="dup3 onto a reserved number is EBUSY"
W_CLO="a reserved number: close keeps its row, bind is EBADF; a free one's close drops it"
CLO_MASK="viv_close_reserved_child: failed checks mask"
X_LEAF="EXTINCTION: handle table lock taken under a leaf lock"
W_FIN_KEEP="and no descriptor was installed"
W_FIN_DIR="before anything is installed"
W_CON="connect refuses to swap a number that no longer names its socket"
W_CON_NB="control: data carries ctl's O_NONBLOCK across the swap"
W_SOC="socket: the refusal installed nothing"
AW_BIN=build/ramfs-src/bin/argwidth-probe
BR_BIN=build/ramfs-src/bin/loom-bridge-probe
QDIR=build/kernel-private-async-qual
QUAL_LINE='PRIVATE-ASYNC QUALIFICATION build'
SUITE_EXT='EXTINCTION: kernel test suite failed'
STRATUM_PIN=7dd103ddbccabe95dfe1d36fdddb4d6a1bfd99dc
export STRATUM_SRC=$HOME/projects/stratum-astra/v2
TLA_JAR=/tmp/tla2tools.jar
JAVA_BIN=/opt/homebrew/opt/openjdk/bin
FILES="kernel/handle.c kernel/syscall.c kernel/vivarium.c"
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
status_once() { # status_once <boot log> <probe> <status>: the probe's verdict line, once
  _c=$(/usr/bin/grep -cF "$2 status=$3 expected=$3" "$1" || true)
  [ "$_c" = 1 ] || { echo "no single '$2 status=$3 expected=$3' in $1"; /usr/bin/grep -F "$2" "$1" | head -4; exit 1; }
}
inputs() { # inputs <label> [kernel dir]: hash what the boot consumed
  _k=${2:-build/kernel}
  for f in "$_k/thylacine.bin" "$_k/thylacine.elf" build/ramfs.cpio build/disk.img "$AW_BIN" "$BR_BIN"; do
    [ -f "$f" ] && shasum -a 256 "$f"
  done > "$OUT/$1-inputs.sha256"
}
probes_hash() { cat "$AW_BIN" "$BR_BIN" | shasum -a 256 | cut -d' ' -f1; }
probes_same() { # probes_same <label> <reference hash>
  _ph=$(probes_hash)
  [ "$_ph" = "$2" ] || { echo "$1: probe hash $_ph differs from its control's $2"; exit 2; }
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
NAMED="$T_RES $T_REP $T_PIPE $T_UNW $T_CLO $T_FIN $T_CON $T_SOC"
for t in $NAMED; do
  /usr/bin/grep -qF "{ \"$t\"," kernel/test/test.c || { echo "REFUSING: $t not registered"; exit 3; }
done
python3 - "$T_RES" "$W_RES_BUSY" test_handle.c "$T_RES" "$W_RES_ALLOC" test_handle.c \
  "$T_RES" "$W_RES_CLOSE" test_handle.c "$T_REP" "$W_REP" test_handle.c \
  "$T_PIPE" "$W_PIPE" test_sys_pipe.c "$T_UNW" "$W_UNW_P2" test_vivarium.c \
  "$T_UNW" "$W_UNW_ROWS" test_vivarium.c "$T_UNW" "$W_UNW_ACC" test_vivarium.c \
  "$T_UNW" "$W_UNW_D3" test_vivarium.c "$T_CLO" "$W_CLO" test_vivarium.c \
  "$T_FIN" "$W_FIN_KEEP" test_stalk.c "$T_FIN" "$W_FIN_DIR" test_stalk.c \
  "$T_CON" "$W_CON" test_viv_sock.c "$T_CON" "$W_CON_NB" test_viv_sock.c \
  "$T_SOC" "$W_SOC" test_viv_sock.c <<'MEMBER' || exit 3
import re, sys
reg = dict(re.findall(r'\{\s*"([^"]+)",\s*(\w+),', open('kernel/test/test.c').read()))
a = sys.argv[1:]
for test, msg, f in zip(a[0::3], a[1::3], a[2::3]):
    src = open('kernel/test/' + f).read()
    q = '"%s"' % msg
    if src.count(q) != 1:
        raise SystemExit("REFUSING: '%s' occurs %d times in %s" % (msg, src.count(q), f))
    fn = reg.get(test)
    m = fn and re.search(r'^void ' + fn + r'\(void\) \{\n(.*?)^\}', src, re.S | re.M)
    if not (m and q in m.group(1)):
        raise SystemExit("REFUSING: '%s' is not inside %s (registered as %s)" % (msg, fn, test))
    print("member: %s (%s) holds '%s'" % (test, fn, msg))
MEMBER
record "predictions derived: each kernel message inside its registered test"
EXPECT=$(/usr/bin/grep -c -E '^[[:space:]]*\{[[:space:]]*"[^"]+"' kernel/test/test.c)
[ "$EXPECT" -ge 1000 ] || { echo "REFUSING: derived only $EXPECT registrations"; exit 3; }
record "expected tests (derived from test.c): $EXPECT"
[ "$(git -C "$STRATUM_SRC" rev-parse HEAD)" = "$STRATUM_PIN" ] || { echo "REFUSING: Stratum HEAD is not $STRATUM_PIN"; exit 3; }
[ -z "$(git -C "$STRATUM_SRC" status --porcelain)" ] || { echo "REFUSING: Stratum tree dirty"; exit 3; }
record "stratum $STRATUM_SRC at $STRATUM_PIN, clean"
GOFORK=${GOFORK:-$HOME/projects/go-thylacine}
export GOFORK
GOFORK_HEAD=$(git -C "$GOFORK" rev-parse HEAD 2>/dev/null) || { echo "REFUSING: GOFORK $GOFORK is not a git tree"; exit 3; }
[ -z "$(git -C "$GOFORK" status --porcelain)" ] || { echo "REFUSING: GOFORK $GOFORK has uncommitted changes"; exit 3; }
record "go fork $GOFORK at $GOFORK_HEAD, clean"
[ -f "$TLA_JAR" ] && [ -x "$JAVA_BIN/java" ] || { echo "REFUSING: no $TLA_JAR or $JAVA_BIN/java"; exit 3; }
mkdir -p "$OUT/pristine"
for f in $FILES; do
  [ "$(/usr/bin/grep -c MUTANT "$f" || true)" = 0 ] || { echo "REFUSING: $f already names a MUTANT"; exit 3; }
  cp "$f" "$(pristine "$f")"
done
python3 - "$0" "$NMUT" <<'CENSUS' || exit 3
import re, sys
script, want = open(sys.argv[1]).read(), int(sys.argv[2])
calls = re.findall(r"^mutant(?:_ext)? (\w+) (kernel/\S+) \\\n'(.*?)' \\\n'(.*?)'\n", script, re.S | re.M)
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
tools/build.sh kernel --config ci > "$OUT/control-build.log" 2>&1 || { echo "control build failed"; tail -20 "$OUT/control-build.log"; exit 1; }
floor post-control-build
inputs control
CONTROL_BIN=$(shasum -a 256 build/kernel/thylacine.bin | cut -d' ' -f1)
CONTROL_PROBES=$(probes_hash)
run_suite control
B=$OUT/control-boot.log
/usr/bin/grep -q '^EXTINCTION:' "$B" && { echo "CONTROL EXTINCTED"; fails "$B" | head; exit 1; } || true
set -- $(tally "$B")
[ "${2:-}" = "$EXPECT" ] && [ "${1:-}" = "$EXPECT" ] || { echo "control tally '$*' != $EXPECT/$EXPECT"; fails "$B" | head; exit 1; }
[ "$suite_rc" = 0 ] || { echo "control test.sh exit $suite_rc"; exit 1; }
all_pass control "$B"
status_once "$B" argwidth-probe 72
status_once "$B" loom-bridge-probe 70
/usr/bin/grep -qF "$QUAL_LINE" "$OUT/control-build.log" && { echo "CONTROL: the default build saw the lever"; exit 1; } || true
record "CONTROL GREEN: $EXPECT/$EXPECT, the eight named tests PASS, argwidth-probe 72, loom-bridge-probe 70, bin ${CONTROL_BIN%"${CONTROL_BIN#????????????????}"}"
echo "=== CONTROL-Q ==="
lease control-q
tools/build.sh kernel --config ci --set PRIVATE_ASYNC_QUAL=y > "$OUT/control-q-build.log" 2>&1 || { echo "control-q build failed"; tail -20 "$OUT/control-q-build.log"; exit 1; }
/usr/bin/grep -qF "$QUAL_LINE" "$OUT/control-q-build.log" || { echo "CONTROL-Q: cmake never saw the lever"; exit 1; }
floor post-control-q-build
inputs control-q "$QDIR"
Q_BIN=$(shasum -a 256 "$QDIR/thylacine.bin" | cut -d' ' -f1)
probes_same control-q "$CONTROL_PROBES"
[ "$Q_BIN" != "$CONTROL_BIN" ] || { echo "CONTROL-Q: the qualification kernel equals the default one"; exit 1; }
[ "$(shasum -a 256 build/kernel/thylacine.bin | cut -d' ' -f1)" = "$CONTROL_BIN" ] || { echo "CONTROL-Q: the qual build replaced the default kernel"; exit 1; }
run_suite control-q --private-async-qual
B=$OUT/control-q-boot.log
/usr/bin/grep -q '^EXTINCTION:' "$B" && { echo "CONTROL-Q EXTINCTED"; fails "$B" | head; exit 1; } || true
set -- $(tally "$B")
[ "${2:-}" = "$EXPECT" ] && [ "${1:-}" = "$EXPECT" ] || { echo "control-q tally '$*' != $EXPECT/$EXPECT"; fails "$B" | head; exit 1; }
[ "$suite_rc" = 0 ] || { echo "control-q test.sh exit $suite_rc"; exit 1; }
all_pass control-q "$B"
status_once "$B" argwidth-probe 72
status_once "$B" loom-bridge-probe 71
record "CONTROL-Q GREEN: $EXPECT/$EXPECT, the eight named tests PASS, loom-bridge-probe 71 (qualification path), bin ${Q_BIN%"${Q_BIN#????????????????}"}"
SEEN_BINS="$CONTROL_BIN $Q_BIN"

echo "=== TLC ==="
lease tlc
tlc() { # tlc <label> <spec> <cfg>: one model check, its log in $OUT
  ( cd specs && "$JAVA_BIN/java" -XX:+UseParallelGC -cp "$TLA_JAR" tlc2.TLC -workers auto -deadlock \
      -noGenerateSpecTE -metadir "$ROOT/$OUT/tlc-states/$1" -config "$3" "$2" ) > "$OUT/tlc-$1.log" 2>&1 || true
}
tlc_clean() { # tlc_clean <label> <spec> <cfg>
  tlc "$@"
  /usr/bin/grep -qF 'Model checking completed. No error has been found.' "$OUT/tlc-$1.log" \
    || { echo "TLC $1: not clean"; tail -15 "$OUT/tlc-$1.log"; exit 4; }
  record "TLC $1 clean: $(/usr/bin/grep -E 'distinct states found' "$OUT/tlc-$1.log" | tail -1)"
}
tlc_buggy() { # tlc_buggy <label> <spec> <cfg> [invariant]: a counterexample, naming the invariant when given
  tlc "$1" "$2" "$3"
  ! /usr/bin/grep -qF 'No error has been found' "$OUT/tlc-$1.log" || { echo "TLC $1: the buggy cfg found NO counterexample"; exit 4; }
  if [ -n "${4:-}" ]; then
    /usr/bin/grep -qF "Error: Invariant $4 is violated." "$OUT/tlc-$1.log" \
      || { echo "TLC $1: not the predicted '$4' violation"; /usr/bin/grep -E '^Error' "$OUT/tlc-$1.log" | head -3; exit 4; }
  else
    /usr/bin/grep -qE '^Error: ' "$OUT/tlc-$1.log" || { echo "TLC $1: no Error line"; tail -15 "$OUT/tlc-$1.log"; exit 4; }
  fi
  record "TLC $1 counterexample: $(/usr/bin/grep -E '^Error: ' "$OUT/tlc-$1.log" | head -1)"
}
tlc_clean slots handle_slots.tla handle_slots.cfg
tlc_buggy slots-close-by-number handle_slots.tla handle_slots_buggy_close_by_number.cfg NoForeignClose
tlc_buggy slots-published handle_slots.tla handle_slots_buggy_published.cfg FailedPublishesNothing
tlc_buggy slots-dup3 handle_slots.tla handle_slots_buggy_dup3.cfg InstallFillsOwnReservation
tlc_clean handles handles.tla handles.cfg
tlc_clean handles-srv handles.tla handles_srv.cfg
for c in specs/handles_buggy_*.cfg; do
  _b=$(basename "$c" .cfg)
  tlc_buggy "$_b" handles.tla "$_b.cfg"
done

apply() { # apply <label> <file> <old> <new>: the mutation, on the PRISTINE file
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
}
# mutant <label> <file> <python old> <python new> -- built (default image) and
# booted; sets B to its boot log. The caller checks the prediction.
mutant_boot() {
  apply "$@"
  tools/build.sh kernel --config ci > "$OUT/$1-build.log" 2>&1 || { echo "$1 build failed"; tail -12 "$OUT/$1-build.log"; exit 5; }
  ! /usr/bin/grep -qF "$QUAL_LINE" "$OUT/$1-build.log" || { echo "$1: the default build saw the lever"; exit 5; }
  floor "post-$1-build"
  inputs "$1"
  probes_same "$1" "$CONTROL_PROBES"
  _bin=$(shasum -a 256 build/kernel/thylacine.bin | cut -d' ' -f1)
  case " $SEEN_BINS " in *" $_bin "*) echo "REFUSING: the $1 kernel equals an earlier kernel of this run"; exit 5;; esac
  SEEN_BINS="$SEEN_BINS $_bin"
  run_suite "$1"
  B=$OUT/$1-boot.log
  [ "$suite_rc" != 0 ] || { echo "HARNESS: test.sh exit 0 under $1"; exit 2; }
  restore || exit 9
  MUTATED=0
}
mutant() {
  mutant_boot "$@"
  _ext=$(/usr/bin/grep -E '^EXTINCTION:' "$B" | tr -d '\r')
  [ "$_ext" = "$SUITE_EXT" ] || { echo "$1: the extinction set is not exactly the suite's own:"; /usr/bin/grep -nE '^EXTINCTION:' "$B" | head -4; exit 2; }
}
# mutant_ext: the same build and boot; the suite must die with exactly $X_WANT,
# print no tally, and have announced $X_TEST last.
mutant_ext() {
  mutant_boot "$@"
  _ext=$(/usr/bin/grep -E '^EXTINCTION:' "$B" | tr -d '\r')
  [ "$_ext" = "$X_WANT" ] || { echo "$1: extinction set '$_ext', predicted exactly '$X_WANT'"; /usr/bin/grep -nE '^EXTINCTION:' "$B" | head -4; exit 2; }
  [ -z "$(tally "$B")" ] || { echo "$1: the suite printed a tally; predicted it died first"; exit 2; }
  _last=$(/usr/bin/grep -oE '\[test\] [^ ]+ \.\.\.' "$B" | tail -1 | sed -E 's/^\[test\] ([^ ]+) \.\.\./\1/')
  [ "$_last" = "$X_TEST" ] || { echo "$1: last announced test '$_last', predicted '$X_TEST'"; exit 2; }
}
mask_is() { # mask_is <label> <hex>: the close child printed exactly that mask, once
  _mc=$(tr -d '\r' < "$B" | /usr/bin/grep -cF "$CLO_MASK" || true)
  [ "$_mc" = 1 ] || { echo "$1: $_mc mask lines, predicted 1"; exit 2; }
  # Fixed-width hex, so a substring cannot match a longer mask; the child may
  # print on its test's announcement line.
  tr -d '\r' < "$B" | /usr/bin/grep -qF "$CLO_MASK $2" || { echo "$1: mask line"; /usr/bin/grep -F "$CLO_MASK" "$B"; echo "predicted $2"; exit 2; }
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
ok() { record "$1 DISCRIMINATED: $2; extinction set = the suite's own"; }

echo "=== M1: dup3 takes a reservation ==="
mutant m1 kernel/handle.c \
'    if (t->slots[new_h].magic == HANDLE_RESERVED) {
' \
'    if (0 && t->slots[new_h].magic == HANDLE_RESERVED) { // MUTANT M1: dup3 takes a reservation
'
expect_fails M1 2 "$T_RES|$W_RES_BUSY" "$T_UNW|$W_UNW_D3"
ok M1 "exactly $T_RES ($W_RES_BUSY) and $T_UNW ($W_UNW_D3)"

echo "=== M2: an alloc's scan takes a reservation ==="
mutant m2 kernel/handle.c \
'        if (t->slots[i].magic == 0) {
            handle_fill_locked(t, (hidx_t)i, kind, rights, obj, cloexec);
' \
'        if (t->slots[i].magic == 0 || t->slots[i].magic == HANDLE_RESERVED) { // MUTANT M2: an alloc takes a reservation
            handle_fill_locked(t, (hidx_t)i, kind, rights, obj, cloexec);
'
expect_fails M2 1 "$T_RES|$W_RES_ALLOC"
ok M2 "exactly $T_RES ($W_RES_ALLOC)"

echo "=== M3: close vacates a reservation ==="
mutant m3 kernel/handle.c \
'    if (slot->magic != HANDLE_MAGIC ||
        (obj && (slot->kind != kind || slot->obj != obj))) {
' \
'    if ((slot->magic != HANDLE_MAGIC && slot->magic != HANDLE_RESERVED) || // MUTANT M3: close vacates a reservation
        (obj && (slot->kind != kind || slot->obj != obj))) {
'
expect_fails M3 2 "$T_RES|$W_RES_CLOSE" "$T_CLO|$W_CLO"
ok M3 "exactly $T_RES ($W_RES_CLOSE) and $T_CLO"

echo "=== M4: pipe on a full table publishes the read end, then closes it ==="
mutant m4 kernel/syscall.c \
'    hidx_t b = handle_reserve(p, 0);
    if (b < 0) {
        handle_unreserve(p, a);
        return -1;
    }
' \
'    hidx_t b = handle_reserve(p, 0);
    if (b < 0) {
        handle_unreserve(p, a);
        struct Spoor *r0 = NULL, *w0 = NULL;   // MUTANT M4: the read end published, then closed by number
        if (pipe_create(&r0, &w0) == 0) {
            hidx_t f = handle_alloc(p, KOBJ_SPOOR, RIGHT_READ, r0);
            if (f >= 0) handle_close(p, f); else spoor_clunk(r0);
            spoor_clunk(w0);
        }
        return -1;
    }
'
expect_fails M4 1 "$T_PIPE|$W_PIPE"
ok M4 "exactly $T_PIPE ($W_PIPE)"

echo "=== M5: pipe2 installs before the copy-out ==="
mutant m5 kernel/syscall.c \
'        if (uaccess_copy_out(args[0], pair, sizeof(pair)) != 0) {
            sys_pipe_abandon(p, rd, wr, rsp, wsp);
            return -(s64)T_E_FAULT;
        }
' \
'        (void)&sys_pipe_abandon;
        sys_pipe_install(p, rd, wr, rsp, wsp, cloexec);   // MUTANT M5: published before the copy-out
        if (uaccess_copy_out(args[0], pair, sizeof(pair)) != 0) {
            handle_close(p, rd);
            handle_close(p, wr);
            return -(s64)T_E_FAULT;
        }
        return 0;
'
expect_fails M5 1 "$T_UNW|$W_UNW_P2"
ok M5 "exactly $T_UNW ($W_UNW_P2)"

echo "=== M6: the dup arm's refusal keeps its alias row ==="
mutant m6 kernel/syscall.c \
'        if (handle_dup_into_reserved(p, (hidx_t)oldfd, nfd, false) < 0) {
            viv_socktab_drop(stab, (s32)nfd);' \
'        if (handle_dup_into_reserved(p, (hidx_t)oldfd, nfd, false) < 0) {
            (void)stab; /* MUTANT M6: the refused dup keeps its alias row */'
expect_fails M6 1 "$T_UNW|$W_UNW_ROWS"
ok M6 "exactly $T_UNW ($W_UNW_ROWS)"

echo "=== M7: the F_DUPFD arm's refusal keeps its alias row ==="
mutant m7 kernel/syscall.c \
'            if (handle_dup_into_reserved(p, fd, nfd, cloexec) < 0) {
                viv_socktab_drop(dstab, (s32)nfd);' \
'            if (handle_dup_into_reserved(p, fd, nfd, cloexec) < 0) {
                (void)dstab; /* MUTANT M7: the refused F_DUPFD keeps its alias row */'
expect_fails M7 1 "$T_UNW|$W_UNW_ROWS"
ok M7 "exactly $T_UNW ($W_UNW_ROWS)"

echo "=== M8: the close hook drops a reserved number's row ==="
mutant m8 kernel/syscall.c \
'        if (st) viv_socktab_drop_closing(st, p, (s32)(s64)ctx->regs[0]);
' \
'        if (st) viv_socktab_drop(st, (s32)(s64)ctx->regs[0]); // MUTANT M8: the hook drops the row of a reserved number
'
expect_fails M8 1 "$T_CLO|$W_CLO"
mask_is M8 0x0000000000000004
ok M8 "exactly $T_CLO (mask 0x4: the row dropped)"

echo "=== M9: a kept Spoor is installed, then closed ==="
mutant m9 kernel/syscall.c \
'    if (how && how->keep) {
        *how->keep = sp;
        return 0;
    }
' \
'    if (how && how->keep) {
        spoor_ref(sp);   // MUTANT M9: a kept Spoor published, then closed by number
        hidx_t m = handle_alloc(p, KOBJ_SPOOR, r, sp);
        if (m >= 0) handle_close(p, m); else spoor_clunk(sp);
        *how->keep = sp;
        return 0;
    }
'
expect_fails M9 1 "$T_FIN|$W_FIN_KEEP"
ok M9 "exactly $T_FIN ($W_FIN_KEEP)"

echo "=== M10: O_DIRECTORY refuses after an install ==="
mutant m10 kernel/syscall.c \
'    if (how && how->dir_required && !(sp->qid.type & QTDIR)) {
        spoor_clunk(sp);
        return -(s64)T_E_NOTDIR;
    }
' \
'    if (how && how->dir_required && !(sp->qid.type & QTDIR)) {
        hidx_t m = handle_alloc(p, KOBJ_SPOOR, r, sp);   // MUTANT M10: published, then refused
        if (m >= 0) handle_close(p, m); else spoor_clunk(sp);
        return -(s64)T_E_NOTDIR;
    }
'
expect_fails M10 1 "$T_FIN|$W_FIN_DIR"
ok M10 "exactly $T_FIN ($W_FIN_DIR)"

echo "=== M11: accept's failed listen open leaks its reservation ==="
mutant m11 kernel/syscall.c \
'    if (lrc < 0) {
        handle_unreserve(p, dh);
        return lrc;
    }
' \
'    if (lrc < 0) {
        (void)dh; // MUTANT M11: the reservation leaks
        return lrc;
    }
'
expect_fails M11 1 "$T_UNW|$W_UNW_ACC"
ok M11 "exactly $T_UNW ($W_UNW_ACC)"

echo "=== M12: socket's claim refusal installs, then closes by number ==="
mutant m12 kernel/syscall.c \
'    if (!viv_socktab_claim(tab, (s32)fd, proto, n, VIV_SOCK_FRESH)) {
        handle_unreserve(p, fd);
        spoor_clunk(ctl);
        return -(s64)T_E_MFILE;
    }
' \
'    if (!viv_socktab_claim(tab, (s32)fd, proto, n, VIV_SOCK_FRESH)) {
        handle_install_reserved(p, fd, KOBJ_SPOOR, cr, ctl, false);   // MUTANT M12: published, then closed by number
        handle_close(p, fd);
        return -(s64)T_E_MFILE;
    }
'
expect_fails M12 1 "$T_SOC|$W_SOC"
ok M12 "exactly $T_SOC ($W_SOC)"

echo "=== M13: handle_replace ignores expect ==="
mutant m13 kernel/handle.c \
'    if (slot->obj != expect) {
' \
'    if (0 && slot->obj != expect) { // MUTANT M13: the swap ignores the object the caller expects
'
expect_fails M13 2 "$T_REP|$W_REP" "$T_CON|$W_CON"
ok M13 "exactly $T_REP and $T_CON ($W_CON)"

echo "=== M14: connect keys its swap on whatever the number names now ==="
mutant m14 kernel/syscall.c \
'    if (handle_replace(p, (hidx_t)fd_raw, ctl, KOBJ_SPOOR, dr, dsp) < 0) {
' \
'    struct Handle cur_m;   // MUTANT M14: the swap keyed on whatever the number names now
    const void *cur = handle_get(p, (hidx_t)fd_raw, &cur_m) == 0 ? cur_m.obj : NULL;
    if (cur) handle_put(&cur_m);
    if (handle_replace(p, (hidx_t)fd_raw, cur, KOBJ_SPOOR, dr, dsp) < 0) {
'
expect_fails M14 1 "$T_CON|$W_CON"
ok M14 "exactly $T_CON ($W_CON)"

echo "=== M15: connect stops carrying ctl's O_NONBLOCK onto data ==="
mutant m15 kernel/syscall.c \
'    if (spoor_flag_get(ctl) & CNONBLOCK) spoor_flag_set(dsp, CNONBLOCK);
' \
'    (void)0; // MUTANT M15: data does not carry the O_NONBLOCK bit
'
expect_fails M15 1 "$T_CON|$W_CON_NB"
ok M15 "exactly $T_CON ($W_CON_NB)"

echo "=== M16: the close hook nests the handle table inside the socket table ==="
X_WANT=$X_LEAF X_TEST=$T_CLO
mutant_ext m16 kernel/vivarium.c \
'    (void)handle_locked_unless_reserved(p, (hidx_t)fd, socktab_drop_closing_locked, &c);
' \
'    socktab_lock(tab);   // MUTANT M16: the old nesting, the handle table inside the socket table
    if (!(fd >= 0 && handle_is_reserved(p, (hidx_t)fd)))
        c.ready = socktab_clear_slot(tab, socktab_find_locked(tab, fd));
    socktab_unlock(tab);
    (void)socktab_drop_closing_locked;
'
record "M16 DISCRIMINATED: exactly '$X_LEAF', no tally, last announced $T_CLO"

echo "=== M17: the socket arms read the row before the slot ==="
mutant m17 kernel/syscall.c \
'    if ((u32)fd_raw >= (u32)PROC_HANDLE_MAX || handle_get_cloexec(p, (hidx_t)(u32)fd_raw) < 0)
        return -(s64)T_E_BADF;
    if (viv_socktab_get(tab, (s32)(s64)fd_raw, out)) return 0;
' \
'    if (viv_socktab_get(tab, (s32)(s64)fd_raw, out)) return 0; // MUTANT M17: the row before the slot
    if ((u32)fd_raw >= (u32)PROC_HANDLE_MAX || handle_get_cloexec(p, (hidx_t)(u32)fd_raw) < 0)
        return -(s64)T_E_BADF;
'
expect_fails M17 1 "$T_CLO|$W_CLO"
mask_is M17 0x0000000000000100
ok M17 "exactly $T_CLO (mask 0x100: bind on the reserved number)"

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
[ "$(git -C "$GOFORK" rev-parse HEAD)" = "$GOFORK_HEAD" ] && [ -z "$(git -C "$GOFORK" status --porcelain)" ] \
  || { echo "GOFORK moved or changed during the run"; exit 8; }
record "go fork unchanged across the run"
echo "-- done: $OUT"
