#!/bin/sh
# The vote-8 argument-width sweep (2026-10-13): the guest legs, in one lease
# window. Derived from work/oct13-rollback/kernel-run.sh.
#   CONTROL    the committed tree, ci image: the whole suite passes, every named
#              test PASSes, argwidth-probe reports 72 and loom-bridge-probe 70.
#   CONTROL-Q  the qualification image: the same, loom-bridge-probe 71.
#   M1         close casts its descriptor down (annotated, so the checker lets
#              it build): the probe's wide close closes the narrow descriptor.
#   M2         getpgid casts its pid down: the wide getpgid answers a pgid.
#   M3         lseek drops the whence compare: the wide whence seeks.
#   M4         sys_word32_arg accepts only a zero-extended word: torpor refuses
#              the sign-extended word every pouch lock passes.
#   M5         the vivarium's close row loses its width mask.
#   M6         the vivarium's readlinkat shell loses its bufsiz check.
#   M7         the vivarium's getrandom shell passes flags whole.
#   M8         sys_s32_arg accepts only a zero-extended word: a sign-extended
#              -1 pid is refused (other tests may fail too; the set is recorded).
#   M9         M1's cast with no note: the build must FAIL at the width checker.
#   M10        rfork drops its flags compare (annotated): the wide flags word
#              reaches the fork as RFPROC.
#   M11        M10 with no note: the build must FAIL at the width checker, on
#              the register-word rule.
#   M12        sys_resolve_dir_wr casts its fd down (annotated): the probe's wide
#              unlink reaches the directory.
#   M13        M12 with no note: the build refuses -- the checker now reads a
#              function that returns a pointer.
#   M14        sys_lookup_root_source (chroot, pivot_root) casts its fd down with
#              no note: the build refuses. Its only witness is static (a probe
#              chroot would move the root of the namespace it shares).
#   M15        the writev shell passes Linux's fd whole: the native handler
#              refuses what Linux reads as the low half.
#   M16        a T1 call no longer restores the guest's x1-x5 and x8.
#   M17        the vivarium reads x8 whole: a number with junk above bit 31 is
#              refused where Linux runs the call its low half names.
#   M18        lseek's whence guard stops refusing (a statement that does not
#              return): the build refuses -- a compare is not a guard.
#   M19        the console gate's per-CPU mark is a flag again (store 1 / store
#              0), so a nested byte's exit clears the outer byte's mark.
#   M20        the gate stops holding the byte's thread on its CPU (no preempt
#              hold across enter..exit).
#   B1         uart_putc without the console gate: tools/test-fault.sh's
#              extinction_peer_chatter must FAIL (the banner is torn), after the
#              chatter ran.
#   B4         extinction() claims the console with IRQs still on: the variant
#              must FAIL on the forbidden "console-claim: taken with irqs on".
#   B2         the committed gate: the same variant PASSes, the banner whole.
#   B3         every test-fault variant PASSes (the claim now waits for peers).
# Each predicted kernel message is checked, before any build, to lie inside the
# function test.c registers under the predicted test name; each predicted probe
# message is checked to be one the probe can print. Each mutant anchor is
# censused before the control build. Both probes must hash equal to their
# control's in every leg (a kernel mutant must not move userspace).
#   SHAPE      tools/check-production.sh --all: the KERNEL_TESTS=OFF kernel builds.
# A failed suite always ends in "EXTINCTION: kernel test suite failed"
# (kernel/main.c): a mutant run must show exactly that extinction and no other.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
STAMP=$(date -u '+%Y%m%dT%H%M%SZ')
OUT=work/oct13-width/kernel-$STAMP
mkdir -p "$OUT"
YIP="${THYLA_WAKE_YIP:-$(command -v yip || echo "$HOME/.local/bin/yip")}"
FLOOR_MB=${FLOOR_MB:-8192}
T_PRB=syscall.argwidth_probe
T_T1=vivarium.t1_narrows_linux_widths
T_T2=vivarium.t2_shells_narrow_linux_widths
T_BRG=loom.bridge_probe
T_FORK=fork.rfork_arg_rejection
T_REG=vivarium.t1_keeps_guest_registers
T_EXG=extinction.tx_gate_admits_only_the_owner_once_claimed
T_EXW=extinction.tx_wait_waits_out_a_peer_byte
T_EXN=extinction.tx_gate_counts_nested_bytes
T_EXP=extinction.tx_enter_holds_the_thread_on_its_cpu
W_PRB="every too-wide argument the probe passed was refused"
W_T1CLOSE="close"
W_T2NEG="readlinkat: a negative bufsiz is refused before the path"
W_T2RND="getrandom: a high half in flags is Linux's to drop"
W_FORK="a flags word wider than 32 bits must be refused, not cut down to RFPROC"
W_T2WV="writev: a high half on the fd is Linux's to drop"
W_REG="a T1 call returns the guest's x1-x5 and x8 untouched, and reads x8 by its low half"
W_EXN="a nested byte on the same cpu is counted, not merged with the outer one"
W_EXP="an admitted byte holds its thread's preempt count: a switch mid-byte would let another thread's exit clear the cpu's mark, or move the byte to a cpu that never raised it"
PROBE_SRC=usr/argwidth-probe/argwidth-probe.c
AW_BIN=build/ramfs-src/bin/argwidth-probe
BR_BIN=build/ramfs-src/bin/loom-bridge-probe
QDIR=build/kernel-private-async-qual
QUAL_LINE='PRIVATE-ASYNC QUALIFICATION build'
SUITE_EXT='EXTINCTION: kernel test suite failed'
STRATUM_PIN=7dd103ddbccabe95dfe1d36fdddb4d6a1bfd99dc
export STRATUM_SRC=$HOME/projects/stratum-astra/v2
FILES="kernel/syscall.c kernel/vivarium.c arch/arm64/uart.c kernel/extinction.c"
NMUT=22
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
NAMED="$T_PRB $T_T1 $T_T2 $T_BRG $T_FORK $T_REG $T_EXG $T_EXW $T_EXN $T_EXP"
for t in $NAMED; do
  /usr/bin/grep -qF "{ \"$t\"," kernel/test/test.c || { echo "REFUSING: $t not registered"; exit 3; }
done
python3 tools/check-arg-width.py --self-test || { echo "REFUSING: the width checker's self-test fails"; exit 3; }
python3 tools/check-arg-width.py kernel/syscall.c || { echo "REFUSING: the committed tree fails the width checker"; exit 3; }
record "width checker: self-test and the committed syscall.c pass"
python3 - "$T_PRB" "$W_PRB" test_argwidth_probe.c "$T_T1" "$W_T1CLOSE" test_vivarium.c \
  "$T_T2" "$W_T2NEG" test_vivarium.c "$T_T2" "$W_T2RND" test_vivarium.c \
  "$T_FORK" "$W_FORK" test_fork.c "$T_T2" "$W_T2WV" test_vivarium.c \
  "$T_REG" "$W_REG" test_vivarium.c "$T_EXN" "$W_EXN" test_extinction.c "$T_EXP" "$W_EXP" test_extinction.c <<'MEMBER' || exit 3
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
# A predicted probe message is either a literal the probe prints, or a PAIR's
# name followed by the PAIR's fixed suffix.
python3 - "$PROBE_SRC" "close: the wide call is refused" "getpgid: the wide call is refused" \
  "lseek whence: the wide call is refused" "torpor_wait: the sign-extended word matches" \
  "getpgid: a sign-extended -1 is a pid, not a width" "unlink parent fd: the wide call is refused" <<'PROBEMSG' || exit 3
import re, sys
src = open(sys.argv[1]).read()
for why in sys.argv[2:]:
    lit = src.count('"%s"' % why)
    pair = 0
    if why.endswith(': the wide call is refused'):
        what = why[:-len(': the wide call is refused')]
        pair = len(re.findall(r'PAIR\((?:[^;]|\n)*?"%s"\);' % re.escape(what), src))
    if lit + pair != 1:
        raise SystemExit("REFUSING: probe message '%s': %d literal + %d PAIR sites" % (why, lit, pair))
    print("probe message: '%s' (%s)" % (why, 'literal' if lit else 'PAIR'))
PROBEMSG
record "predictions derived: each kernel message inside its registered test, each probe message printable once"
EXPECT=$(/usr/bin/grep -c -E '^[[:space:]]*\{[[:space:]]*"[^"]+"' kernel/test/test.c)
[ "$EXPECT" -ge 1000 ] || { echo "REFUSING: derived only $EXPECT registrations"; exit 3; }
record "expected tests (derived from test.c): $EXPECT"
[ "$(git -C "$STRATUM_SRC" rev-parse HEAD)" = "$STRATUM_PIN" ] || { echo "REFUSING: Stratum HEAD is not $STRATUM_PIN"; exit 3; }
[ -z "$(git -C "$STRATUM_SRC" status --porcelain)" ] || { echo "REFUSING: Stratum tree dirty"; exit 3; }
record "stratum $STRATUM_SRC at $STRATUM_PIN, clean"
# The Go fork is baked into every image (tools/build.sh GOFORK). The shared
# ~/projects/go-thylacine is edited live by other tracks, and a bake that reads
# it mid-edit fails or bakes a state nobody committed: pin a clean tree.
GOFORK=${GOFORK:-$HOME/projects/go-thylacine}
export GOFORK
GOFORK_HEAD=$(git -C "$GOFORK" rev-parse HEAD 2>/dev/null) || { echo "REFUSING: GOFORK $GOFORK is not a git tree"; exit 3; }
[ -z "$(git -C "$GOFORK" status --porcelain)" ] || { echo "REFUSING: GOFORK $GOFORK has uncommitted changes"; exit 3; }
record "go fork $GOFORK at $GOFORK_HEAD, clean"
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
calls = re.findall(r"^mutant(?:_build|_fault)? (\w+) ((?:kernel|arch)/\S+) \\\n'(.*?)' \\\n'(.*?)'\n", script, re.S | re.M)
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
/usr/bin/grep -qF '==> argument width check' "$OUT/control-build.log" || { echo "CONTROL: the build never ran the width check"; exit 1; }
floor post-control-build
inputs control
CONTROL_BIN=$(shasum -a 256 build/kernel/thylacine.bin | cut -d' ' -f1)
CONTROL_PROBES=$(probes_hash)
run_suite control
B=$OUT/control-boot.log
/usr/bin/grep -q '^EXTINCTION:' "$B" && { echo "CONTROL EXTINCTED"; fails "$B" | head; /usr/bin/grep -F 'argwidth-probe' "$B" | head -4; exit 1; } || true
set -- $(tally "$B")
[ "${2:-}" = "$EXPECT" ] && [ "${1:-}" = "$EXPECT" ] || { echo "control tally '$*' != $EXPECT/$EXPECT"; fails "$B" | head; exit 1; }
[ "$suite_rc" = 0 ] || { echo "control test.sh exit $suite_rc"; exit 1; }
all_pass control "$B"
status_once "$B" argwidth-probe 72
status_once "$B" loom-bridge-probe 70
/usr/bin/grep -qF "$QUAL_LINE" "$OUT/control-build.log" && { echo "CONTROL: the default build saw the lever"; exit 1; } || true
record "CONTROL GREEN: $EXPECT/$EXPECT, the five named tests PASS, argwidth-probe 72, loom-bridge-probe 70, probes ${CONTROL_PROBES%"${CONTROL_PROBES#????????????????}"}, bin ${CONTROL_BIN%"${CONTROL_BIN#????????????????}"}"
echo "=== CONTROL-Q ==="
lease control-q
tools/build.sh kernel --config ci --set PRIVATE_ASYNC_QUAL=y > "$OUT/control-q-build.log" 2>&1 || { echo "control-q build failed"; exit 1; }
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
record "CONTROL-Q GREEN: $EXPECT/$EXPECT, the five named tests PASS, argwidth-probe 72, loom-bridge-probe 71 (qualification path), bin ${Q_BIN%"${Q_BIN#????????????????}"}"
SEEN_BINS="$CONTROL_BIN $Q_BIN"
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
mutant() {
  apply "$@"
  tools/build.sh kernel --config ci > "$OUT/$1-build.log" 2>&1 || { echo "$1 build failed"; tail -12 "$OUT/$1-build.log"; exit 5; }
  ! /usr/bin/grep -qF "$QUAL_LINE" "$OUT/$1-build.log" || { echo "$1: the default build saw the lever"; exit 5; }
  floor "post-$1-build"
  inputs "$1"
  probes_same "$1" "$CONTROL_PROBES"
  _bin=$(shasum -a 256 build/kernel/thylacine.bin | cut -d' ' -f1)
  case " $SEEN_BINS " in *" $_bin "*) echo "REFUSING: the $1 kernel equals an earlier kernel of this run"; exit 5;; esac
  SEEN_BINS="$SEEN_BINS $_bin"
  mkdir -p "$OUT/$1-artifacts-DO-NOT-BOOT"
  cp build/kernel/thylacine.bin "$OUT/$1-artifacts-DO-NOT-BOOT/"
  echo "MUTANT $1 kernel. Never boot or promote." > "$OUT/$1-artifacts-DO-NOT-BOOT/WARNING.txt"
  run_suite "$1"
  B=$OUT/$1-boot.log
  _ext=$(/usr/bin/grep -E '^EXTINCTION:' "$B" | tr -d '\r')
  if [ "$1" = m8 ]; then
    # M8 breaks every sign-extended pid, so a waiter that orphans its child
    # cascades into later tests' waits (test_caps' flow thunk extincts on the
    # mismatch). Its extinction set is recorded, not predicted.
    printf '%s\n' "$_ext" > "$OUT/m8-extinctions.txt"
  else
    [ "$_ext" = "$SUITE_EXT" ] || { echo "$1: the extinction set is not exactly the suite's own:"; /usr/bin/grep -nE '^EXTINCTION:' "$B" | head -4; exit 2; }
  fi
  [ "$suite_rc" != 0 ] || { echo "HARNESS: test.sh exit 0 under $1"; exit 2; }
  restore || exit 9
  MUTATED=0
}
# mutant_build <label> <file> <old> <new>: the build itself must refuse it, at
# the width checker, naming <file>:<line> -- set CHECK_LINE before the call.
mutant_build() {
  apply "$@"
  ( set +e; tools/build.sh kernel --config ci > "$OUT/$1-build.log" 2>&1; echo $? > "$OUT/$1-build.rc" )
  _brc=$(cat "$OUT/$1-build.rc")
  record "$1: build exit $_brc"
  [ "$_brc" != 0 ] || { echo "$1: the build ACCEPTED an unannotated narrowing cast"; exit 2; }
  /usr/bin/grep -qF '==> argument width check FAILED' "$OUT/$1-build.log" || { echo "$1: the build failed, but not at the width check"; tail -12 "$OUT/$1-build.log"; exit 2; }
  _hits=$(/usr/bin/grep -E ': [a-z_0-9]+ narrows [^ ]+ with no full-width check first$' "$OUT/$1-build.log" || true)
  [ "$(printf '%s\n' "$_hits" | /usr/bin/grep -c .)" = 1 ] || { echo "$1: checker hits: '$_hits'"; exit 2; }
  case "$_hits" in *"$CHECK_LINE"*) ;; *) echo "$1: checker said '$_hits', predicted '$CHECK_LINE'"; exit 2;; esac
  /usr/bin/grep -qF 'check-arg-width: 1 narrowing cast(s)' "$OUT/$1-build.log" || { echo "$1: no checker tally line"; exit 2; }
  restore || exit 9
  MUTATED=0
}
# mutant_fault <label> <file> <old> <new>: tools/test-fault.sh's
# extinction_peer_chatter must FAIL on the mutant (it builds its own kernel
# under build/kernel-fault-*; build/kernel is untouched), for the reason MF_WHY
# (an ERE on the variant's FAIL line), and with MF_SEEN in its boot log when set
# (so a timeout is one the provoker reached). MF_TIMEOUT is its BOOT_TIMEOUT.
FAULT_QLOG=build/test-fault-extinction_peer_chatter.log
mutant_fault() {
  apply "$@"
  ( set +e; BOOT_TIMEOUT=$MF_TIMEOUT tools/test-fault.sh extinction_peer_chatter > "$OUT/$1-fault.log" 2>&1; echo $? > "$OUT/$1-fault.rc" )
  cp "$FAULT_QLOG" "$OUT/$1-qemu.log" 2>/dev/null || true
  record "$1: test-fault exit $(cat "$OUT/$1-fault.rc")"
  [ "$(cat "$OUT/$1-fault.rc")" != 0 ] || { echo "$1: extinction_peer_chatter PASSED on the mutant -- the witness cannot fail"; exit 2; }
  _fl=$(/usr/bin/grep -E '^==> \[extinction_peer_chatter\] FAIL' "$OUT/$1-fault.log" | head -1)
  [ -n "$_fl" ] || { echo "$1: test-fault failed, but not at the variant"; tail -12 "$OUT/$1-fault.log"; exit 2; }
  printf '%s\n' "$_fl" | /usr/bin/grep -qE "$MF_WHY" || { echo "$1: the variant failed for another reason: '$_fl' (predicted /$MF_WHY/)"; exit 2; }
  if [ -n "$MF_SEEN" ]; then
    /usr/bin/grep -qF "$MF_SEEN" "$OUT/$1-qemu.log" || { echo "$1: '$MF_SEEN' never printed -- the provoker did not run"; exit 2; }
  fi
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
# non-negative number.
expect_probe() { # expect_probe <label> <why> <got|ANY>
  _pl=$(/usr/bin/grep -F 'argwidth-probe: FAIL line ' "$B" | tr -d '\r')
  [ "$(printf '%s\n' "$_pl" | /usr/bin/grep -c .)" = 1 ] || { echo "$1: probe FAIL lines: '$_pl'"; exit 2; }
  _pre=${_pl%% (got *}; _got=${_pl##* (got }; _got=${_got%)}
  case "$_pre" in *": $2") ;; *) echo "$1: probe said '$_pl', predicted ': $2'"; exit 2;; esac
  if [ "${3#NOT}" != "$3" ]; then
    case "$_got" in ''|*[!0-9-]*) echo "$1: probe value '$_got' is not a number"; exit 2;; esac
    [ "$_got" != "${3#NOT}" ] || { echo "$1: probe value '$_got', predicted anything but ${3#NOT}"; exit 2; }
  elif [ "$3" = ANY ]; then
    case "$_got" in ''|*[!0-9]*) echo "$1: probe value '$_got', predicted a non-negative number"; exit 2;; esac
  else
    [ "$_got" = "$3" ] || { echo "$1: probe value '$_got', predicted '$3'"; exit 2; }
  fi
}
ok() { record "$1 DISCRIMINATED: $2; extinction set = the suite's own"; }

echo "=== M1: close casts its descriptor down ==="
mutant m1 kernel/syscall.c \
'    return handle_close(p, sys_hidx_arg(hraw)) == 0 ? 0 : (s64)(-T_E_BADF);
' \
'    return handle_close(p, (hidx_t)hraw) == 0 ? 0 : (s64)(-T_E_BADF); // width: MUTANT M1 truncates
'
expect_fails M1 1 "$T_PRB|$W_PRB"
expect_probe M1 "close: the wide call is refused" 0
ok M1 "exactly $T_PRB, the probe at 'close: the wide call is refused' (got 0)"
echo "=== M2: getpgid casts its pid down ==="
mutant m2 kernel/syscall.c \
'    if (!sys_s32_arg(a0, &pid)) return -T_E_INVAL;
    return (s64)proc_getpgid(p, pid);
' \
'    pid = (s32)a0; // width: MUTANT M2 truncates
    return (s64)proc_getpgid(p, pid);
'
expect_fails M2 1 "$T_PRB|$W_PRB"
expect_probe M2 "getpgid: the wide call is refused" ANY
ok M2 "exactly $T_PRB, the probe at 'getpgid: the wide call is refused'"
echo "=== M3: lseek drops the whence compare ==="
mutant m3 kernel/syscall.c \
'    if (whence_raw > 0xffffffffull)                  return -T_E_INVAL;
    u32 whence = (u32)whence_raw;
' \
'    u32 whence = (u32)whence_raw; // width: MUTANT M3 no compare
'
expect_fails M3 1 "$T_PRB|$W_PRB"
expect_probe M3 "lseek whence: the wide call is refused" 0
ok M3 "exactly $T_PRB, the probe at 'lseek whence: the wide call is refused' (got 0)"
echo "=== M4: sys_word32_arg accepts only a zero-extended word ==="
mutant m4 kernel/syscall.c \
'    if (hi != 0 && !(hi == 0xffffffffull && (raw & 0x80000000ull))) return false;
' \
'    if (hi != 0) return false; // MUTANT M4: zero extension only
'
expect_fails M4 1 "$T_PRB|$W_PRB"
expect_probe M4 "torpor_wait: the sign-extended word matches" -22
ok M4 "exactly $T_PRB, the probe at 'torpor_wait: the sign-extended word matches' (got -22)"
echo "=== M5: the vivarium close row loses its width mask ==="
mutant m5 kernel/vivarium.c \
'    { VIV_LINUX_CLOSE,      SYS_CLOSE,      1, 0x1 },
' \
'    { VIV_LINUX_CLOSE,      SYS_CLOSE,      1 }, // MUTANT M5: no width mask
'
expect_fails M5 1 "$T_T1|$W_T1CLOSE"
ok M5 "exactly $T_T1 at '$W_T1CLOSE'"
echo "=== M6: the readlinkat shell loses its bufsiz check ==="
mutant m6 kernel/syscall.c \
'        if (bufsiz <= 0)                               return -(s64)T_E_INVAL;
' \
'        (void)0; // MUTANT M6: no bufsiz check
'
expect_fails M6 1 "$T_T2|$W_T2NEG"
ok M6 "exactly $T_T2 at '$W_T2NEG'"
echo "=== M7: the getrandom shell passes flags whole ==="
mutant m7 kernel/syscall.c \
'        s64 rc = sys_getrandom_handler(args[0], args[1], (u64)(u32)args[2]);
' \
'        s64 rc = sys_getrandom_handler(args[0], args[1], args[2]); // MUTANT M7: flags whole
'
expect_fails M7 1 "$T_T2|$W_T2RND"
ok M7 "exactly $T_T2 at '$W_T2RND'"
echo "=== M8: sys_s32_arg accepts only a zero-extended word ==="
mutant m8 kernel/syscall.c \
'    if ((s64)raw < -0x80000000ll || (s64)raw > 0x7fffffffll) return false;
' \
'    if (raw > 0x7fffffffull) return false; // MUTANT M8: zero extension only
'
# Other callers pass a sign-extended negative too (wait for any child, poll
# forever): the probe's verdict is predicted, the rest is recorded.
_v=$(verdict "$B" "$T_PRB")
[ "$_v" = "FAIL: $W_PRB" ] || { echo "M8: $T_PRB verdict '$_v'"; exit 2; }
expect_probe M8 "getpgid: a sign-extended -1 is a pid, not a width" -22
fails "$B" > "$OUT/m8-fail-set.txt"
record "M8 DISCRIMINATED: $T_PRB, the probe at 'getpgid: a sign-extended -1 is a pid, not a width' (got -22); $(wc -l < "$OUT/m8-fail-set.txt" | tr -d ' ') FAIL line(s) and $(/usr/bin/grep -c . "$OUT/m8-extinctions.txt") extinction line(s) in all, recorded in m8-fail-set.txt and m8-extinctions.txt"
echo "=== M9: M1's cast with no note -- the build refuses ==="
CHECK_LINE="kernel/syscall.c:$(/usr/bin/grep -nF '    return handle_close(p, sys_hidx_arg(hraw)) == 0 ? 0 : (s64)(-T_E_BADF);' "$(pristine kernel/syscall.c)" | cut -d: -f1): sys_close_handler narrows hraw"
mutant_build m9 kernel/syscall.c \
'    return handle_close(p, sys_hidx_arg(hraw)) == 0 ? 0 : (s64)(-T_E_BADF);
' \
'    return handle_close(p, (hidx_t)hraw) == 0 ? 0 : (s64)(-T_E_BADF); // MUTANT M9 no note
'
record "M9 DISCRIMINATED: the build refused at the width check, naming '$CHECK_LINE'"
echo "=== M10: rfork drops its flags compare ==="
mutant m10 kernel/syscall.c \
'    if (ctx->regs[0] > 0xffffffffull) return -(s64)T_E_INVAL;
    return sys_rfork_core(ctx, (unsigned)ctx->regs[0], ctx->regs[1],
' \
'    return sys_rfork_core(ctx, (unsigned)ctx->regs[0], ctx->regs[1], // width: MUTANT M10 no compare
'
expect_fails M10 1 "$T_FORK|$W_FORK"
ok M10 "exactly $T_FORK at '$W_FORK'"
echo "=== M11: M10 with no note -- the build refuses ==="
CHECK_LINE="kernel/syscall.c:$(( $(/usr/bin/grep -nF '    if (ctx->regs[0] > 0xffffffffull) return -(s64)T_E_INVAL;' "$(pristine kernel/syscall.c)" | cut -d: -f1) )): sys_rfork_handler narrows ctx->regs[0]"
mutant_build m11 kernel/syscall.c \
'    if (ctx->regs[0] > 0xffffffffull) return -(s64)T_E_INVAL;
    return sys_rfork_core(ctx, (unsigned)ctx->regs[0], ctx->regs[1],
' \
'    return sys_rfork_core(ctx, (unsigned)ctx->regs[0], ctx->regs[1]  /* MUTANT M11 no note */,
'
record "M11 DISCRIMINATED: the build refused at the width check, naming '$CHECK_LINE'"
echo "=== M12: sys_resolve_dir_wr casts its fd down ==="
mutant m12 kernel/syscall.c \
'    return sys_lookup_spoor(p, sys_hidx_arg(fd_raw), RIGHT_WRITE);
' \
'    return sys_lookup_spoor(p, (hidx_t)fd_raw, RIGHT_WRITE); // width: MUTANT M12 truncates
'
expect_fails M12 1 "$T_PRB|$W_PRB"
expect_probe M12 "unlink parent fd: the wide call is refused" NOT-9
ok M12 "exactly $T_PRB, the probe at 'unlink parent fd: the wide call is refused' (got $_got)"
echo "=== M13: M12 with no note -- the build refuses ==="
CHECK_LINE="kernel/syscall.c:$(/usr/bin/grep -nF '    return sys_lookup_spoor(p, sys_hidx_arg(fd_raw), RIGHT_WRITE);' "$(pristine kernel/syscall.c)" | cut -d: -f1): sys_resolve_dir_wr narrows fd_raw"
mutant_build m13 kernel/syscall.c \
'    return sys_lookup_spoor(p, sys_hidx_arg(fd_raw), RIGHT_WRITE);
' \
'    return sys_lookup_spoor(p, (hidx_t)fd_raw, RIGHT_WRITE); /* MUTANT M13 no note */
'
record "M13 DISCRIMINATED: the build refused at the width check, naming '$CHECK_LINE'"
echo "=== M14: sys_lookup_root_source casts its fd down -- the build refuses ==="
CHECK_LINE="kernel/syscall.c:$(/usr/bin/grep -nF '    struct Spoor *source = sys_lookup_spoor(p, sys_hidx_arg(fd_raw), RIGHT_READ);' "$(pristine kernel/syscall.c)" | cut -d: -f1): sys_lookup_root_source narrows fd_raw"
mutant_build m14 kernel/syscall.c \
'    struct Spoor *source = sys_lookup_spoor(p, sys_hidx_arg(fd_raw), RIGHT_READ);
' \
'    struct Spoor *source = sys_lookup_spoor(p, (hidx_t)fd_raw, RIGHT_READ); /* MUTANT M14 no note */
'
record "M14 DISCRIMINATED: the build refused at the width check, naming '$CHECK_LINE'"
echo "=== M15: the writev shell passes Linux's fd whole ==="
mutant m15 kernel/syscall.c \
'        return viv_writev((u64)(u32)args[0], args[1], args[2]);  // fdget_pos(unsigned int)
' \
'        return viv_writev(args[0], args[1], args[2]);  // MUTANT M15 whole fd
'
expect_fails M15 1 "$T_T2|$W_T2WV"
ok M15 "exactly $T_T2 at '$W_T2WV'"
echo "=== M16: a T1 call keeps the renumbered registers ==="
mutant m16 kernel/syscall.c \
'            for (u32 i = 1; i < VIV_NARGS; i++) ctx->regs[i] = kept[i];
            ctx->regs[8] = kept_x8;
' \
'            (void)kept; (void)kept_x8; // MUTANT M16 no restore
'
expect_fails M16 1 "$T_REG|$W_REG"
ok M16 "exactly $T_REG at '$W_REG'"
echo "=== M17: the vivarium reads x8 whole ==="
mutant m17 kernel/syscall.c \
'    u64 nr = ctx->regs[8] & 0xffffffffull;
' \
'    u64 nr = ctx->regs[8]; // MUTANT M17 whole x8
'
expect_fails M17 1 "$T_REG|$W_REG"
ok M17 "exactly $T_REG at '$W_REG'"
echo "=== M18: lseek's whence compare stops refusing -- the build refuses ==="
CHECK_LINE="kernel/syscall.c:$(( $(/usr/bin/grep -nF '    if (whence_raw > 0xffffffffull)                  return -T_E_INVAL;' "$(pristine kernel/syscall.c)" | cut -d: -f1) + 1 )): sys_lseek_handler narrows whence_raw"
mutant_build m18 kernel/syscall.c \
'    if (whence_raw > 0xffffffffull)                  return -T_E_INVAL;
' \
'    if (whence_raw > 0xffffffffull)                  (void)0; /* MUTANT M18 no refusal */
'
record "M18 DISCRIMINATED: the build refused at the width check, naming '$CHECK_LINE'"
echo "=== M19: the console gate's mark is a flag, not a depth ==="
mutant m19 kernel/extinction.c \
'    (void)__atomic_fetch_add(busy, 1u, __ATOMIC_SEQ_CST);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (__atomic_load_n(word, __ATOMIC_SEQ_CST) == 0u || owner) return 1;
    extinction_tx_exit_core(busy);
    return 0;
}

void extinction_tx_exit_core(volatile u8 *busy) {
    (void)__atomic_fetch_sub(busy, 1u, __ATOMIC_RELEASE);
}
' \
'    __atomic_store_n(busy, 1u, __ATOMIC_SEQ_CST); // MUTANT M19 a flag, not a depth
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (__atomic_load_n(word, __ATOMIC_SEQ_CST) == 0u || owner) return 1;
    __atomic_store_n(busy, 0u, __ATOMIC_RELEASE);
    return 0;
}

void extinction_tx_exit_core(volatile u8 *busy) {
    __atomic_store_n(busy, 0u, __ATOMIC_RELEASE);
}
'
expect_fails M19 1 "$T_EXN|$W_EXN"
ok M19 "exactly $T_EXN at '$W_EXN'"
echo "=== M20: the gate no longer holds the byte's thread on its CPU ==="
mutant m20 kernel/extinction.c \
'int extinction_tx_enter(void) {
    spin_preempt_inc();
    unsigned cpu = smp_cpu_idx_self();
    if (cpu >= DTB_MAX_CPUS) cpu = 0;
    if (extinction_tx_gate_core(&g_extinction_tx_busy[cpu], &g_extinction_console,
                                g_extinction_owner[cpu]))
        return 1;
    spin_preempt_dec();
    return 0;
}

void extinction_tx_exit(void) {
    unsigned cpu = smp_cpu_idx_self();
    if (cpu >= DTB_MAX_CPUS) cpu = 0;
    extinction_tx_exit_core(&g_extinction_tx_busy[cpu]);
    spin_preempt_dec();
}
' \
'int extinction_tx_enter(void) {
    unsigned cpu = smp_cpu_idx_self();   // MUTANT M20 no preempt hold
    if (cpu >= DTB_MAX_CPUS) cpu = 0;
    return extinction_tx_gate_core(&g_extinction_tx_busy[cpu], &g_extinction_console,
                                   g_extinction_owner[cpu]);
}

void extinction_tx_exit(void) {
    unsigned cpu = smp_cpu_idx_self();
    if (cpu >= DTB_MAX_CPUS) cpu = 0;
    extinction_tx_exit_core(&g_extinction_tx_busy[cpu]);
}
'
expect_fails M20 1 "$T_EXP|$W_EXP"
ok M20 "exactly $T_EXP at '$W_EXP'"
echo "=== B1-B4: the extinction banner against peer chatter (tools/test-fault.sh) ==="
lease fault; floor pre-fault
# Without the gate no ^EXTINCTION: line forms, so the variant times out (or,
# if the prefix survives, fails the whole-line check); the chatter must have run.
MF_TIMEOUT=60; MF_WHY='timeout|is not a whole line'; MF_SEEN='chatter: a peer cpu writes the console directly'
mutant_fault b1 arch/arm64/uart.c \
'void uart_putc(char c) {
    if (!extinction_tx_enter()) return;
    uart_putc_wire(c);
    extinction_tx_exit();
}
' \
'void uart_putc(char c) {
    uart_putc_wire(c);   // MUTANT B1 no gate
}
'
record "B1 DISCRIMINATED: without the gate extinction_peer_chatter FAILS: $(/usr/bin/grep -E '^==> \[extinction_peer_chatter\] FAIL' "$OUT/b1-fault.log" | head -1 | cut -c1-160)"
MF_TIMEOUT=300; MF_WHY="ALSO 'console-claim: taken with irqs on'"; MF_SEEN=''
mutant_fault b4 kernel/extinction.c \
'    // restored: every path from here parks.
    irq_mask_local();
' \
'    // restored: every path from here parks.
    // MUTANT B4 no mask before the claim
'
record "B4 DISCRIMINATED: a claim with IRQs on FAILS the variant: $(/usr/bin/grep -E '^==> \[extinction_peer_chatter\] FAIL' "$OUT/b4-fault.log" | head -1 | cut -c1-160)"
( set +e; BOOT_TIMEOUT=300 tools/test-fault.sh extinction_peer_chatter > "$OUT/b2-fault.log" 2>&1; echo $? > "$OUT/b2-fault.rc" )
[ "$(cat "$OUT/b2-fault.rc")" = 0 ] && /usr/bin/grep -qE '^==> \[extinction_peer_chatter\] PASS' "$OUT/b2-fault.log" \
  || { echo "B2: the gated build did not PASS extinction_peer_chatter"; tail -12 "$OUT/b2-fault.log"; exit 2; }
record "B2: with the gate extinction_peer_chatter PASSes, the banner a whole line"
( set +e; BOOT_TIMEOUT=300 tools/test-fault.sh > "$OUT/b3-fault.log" 2>&1; echo $? > "$OUT/b3-fault.rc" )
[ "$(cat "$OUT/b3-fault.rc")" = 0 ] || { echo "B3: test-fault.sh exit $(cat "$OUT/b3-fault.rc")"; tail -14 "$OUT/b3-fault.log"; exit 2; }
record "B3: $(/usr/bin/grep -E '^==> Summary' "$OUT/b3-fault.log")"
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
