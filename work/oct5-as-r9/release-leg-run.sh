#!/bin/sh
# Private retirement lifetime-PUT sensitivity: control then omitted-put-only
# mutant. Uses the retained live creator and exact AddrSpace reference count;
# does not claim final descriptor/page-table reclamation. The mutant intentionally
# leaks one lifetime reference until the disposable VM ends. It does not remove
# the matching get or the private-ring decrement. Derived from refusal-leg-run
# with its source/Stratum/lease/floor checks and quarantine-first recovery.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
STAMP=$(date -u '+%Y%m%dT%H%M%SZ')
OUT=work/oct5-as-r9/release-leg-$STAMP
mkdir -p "$OUT"
YIP="${THYLA_WAKE_YIP:-$(command -v yip || echo "$HOME/.local/bin/yip")}"
FLOOR_GB=${FLOOR_GB:-8}
# THE PREDICTED FAILURE, defined ONCE and before anything can need it. The
# oracle requires this exact string and the quarantine warning names it to the
# next reader, and those two must not be able to disagree: a warning promising a
# different extinction than the experiment requires is a warning that will be
# believed over the code. Defined above the EXIT trap so no exit path can reach
# recover() with it unset, rather than being safe only because of a conditional
# somewhere else.
WANT='private retirement releases its image lifetime reference'
MUTATED=0
RECOVERED=0
RECOVERY_FAILED=0
CONTROL_BIN=
free_gb() { df -g . | awk 'NR==2 {print $4}'; }
free_mb() { df -m . | awk 'NR==2 {print $4}'; }
# EVERY DISK READING IS RETAINED, not just printed. These floor readings are the
# only record of what a --config ci bake actually costs in THIS tree, and when a
# peer asked exactly that today I could not answer it: the figures had gone to a
# terminal, which a compaction does not keep, while every other piece of this
# run's evidence lands in $OUT. A gate has two halves, the verdict and the
# capture, and I had built only the verdict.
# The GiB figure is PASSED IN rather than re-measured, so the number recorded is
# the one the decision was actually made on; MiB is recorded beside it because
# df -g truncates and the draw between two stages is smaller than its resolution
# (a fall from 8.9 to 8.1 GiB reads as "8" at both ends).
# The label is recorded as ONE token: "stage 0" as written would make the MiB
# figure field 4 on that row and field 3 on every other, and the first thing I
# did with this table was read it by column and get a wrong answer.
disk_record() { # disk_record <label> <the GiB value the caller decided on>
  printf '%s  %-22s %6s MiB free (df -g %s, floor %s GiB)\n' \
    "$(date -u '+%Y-%m-%dT%H:%M:%SZ')" \
    "$(printf '%s' "$1" | tr ' ' '-')" "$(free_mb)" "$2" "$FLOOR_GB" \
    >> "$OUT/disk.txt"
}
floor() { # floor <label>
  _f=$(free_gb)
  disk_record "$1" "$_f"
  echo "-- disk at $1: ${_f} GiB free (floor $FLOOR_GB)"
  [ "$_f" -ge "$FLOOR_GB" ] || {
    echo "   REFUSING at $1: ${_f} GiB is under the floor. A bake here could push"
    echo "   the shared volume below a PEER's own 6 GiB floor mid-landing."; exit 3; }
}

echo "=== stage 0: the preconditions, each one able to refuse ==="
# THE LEASE. Not a formality: a peer's gate and an idle machine are identical on
# every measurable dimension, so only the declared lease settles it.
[ -x "$YIP" ] || { echo "REFUSING: no yip at $YIP"; exit 3; }
"$YIP" resources > "$OUT/lease.txt" 2>&1 || true
if ! /usr/bin/grep -qE '^mac +HELD by you' "$OUT/lease.txt"; then
  echo "REFUSING: the mac lease is not mine. yip says:"
  sed -n '1,4p' "$OUT/lease.txt"; exit 3
fi
echo "-- lease: mine ($(/usr/bin/grep -m1 '^mac' "$OUT/lease.txt"))"

# THE TREE. A mutant run must start from a committed state, or the restore has
# nothing to be checked against.
dirty=$(git status --porcelain | /usr/bin/grep -v '^??' | wc -l | tr -d ' ')
[ "$dirty" = 0 ] || { echo "REFUSING: $dirty tracked file(s) modified. Commit first."; exit 3; }
HEAD_SHA=$(git rev-parse HEAD)
echo "-- HEAD $HEAD_SHA, tree clean of tracked modifications"

# THE LEG MUST BE IN THE FIXTURE, or this script measures the tree as it was
# before the leg was written. WANT is checked non-empty FIRST: an empty -F
# pattern matches every line, and this check would then pass on any fixture.
[ -n "$WANT" ] || { echo "REFUSING: the predicted assertion message is empty"; exit 3; }
for _need in "$WANT" \
    'release probe starts with only the creator reference' \
    'release probe returns charges and guard without dropping the owner'; do
  /usr/bin/grep -qF "$_need" kernel/test/loom_private_fixture.h || {
    echo "REFUSING: the fixture lacks: $_need"
    echo "   -- the leg under test is not in this tree."; exit 3; }
done
echo "-- fixture carries the leg, its preconditions and its baseline assertion"
# Match successful retirement, not the shared helper used by earlier tests.
python3 - <<'ORDER'
s = open('kernel/test/loom_private_fixture.h').read()
assert s.index('private retirement releases its image lifetime reference') < s.index('// Rollback:')
t = open('kernel/loom.c').read()
a = '    addrspace_private_end(as);\n\n    __atomic_fetch_add(&g_loom_destroyed'
assert t.count(a) == 1
u = open('kernel/addrspace.c').read()
start=u.index('void addrspace_private_end('); end=u.index('\n}', start)
assert '--as->private_rings;' in u[start:end]
assert 'addrspace_lifetime_put(as); // may free; never under as->lock' in u[start:end]
ORDER

# THE EXPECTED TALLY IS DERIVED, never typed: this leg adds no registration, so
# the total must be UNCHANGED, and deriving it means a stale constant cannot
# make a shrunken suite look whole.
EXPECT_TESTS=$(/usr/bin/grep -c -E '^[[:space:]]*\{[[:space:]]*"[^"]+"' kernel/test/test.c)
[ "$EXPECT_TESTS" -ge 1000 ] || {
  echo "REFUSING: derived only $EXPECT_TESTS registrations from kernel/test/test.c"
  echo "   -- the DERIVATION is broken, not the suite."; exit 3; }
echo "-- expectation DERIVED from kernel/test/test.c: $EXPECT_TESTS registrations"
# THE EXTERNAL STRATUM PIN, ADOPTED BY EXTRACTION rather than duplicated. The
# qualified image was built with STRATUM_SRC pointed at the pinned tree; this
# script left that lever UNSET, so build.sh fell back to its default
# ~/projects/stratum/v2 and would have produced a control image built from a
# DIFFERENT Stratum than the one the leg was qualified against.
#
# It was not caught by reasoning: the first real run of this script died inside
# build.sh with "CMake Error: the source ... does not match the source ... used
# to generate cache", because build/'s stratumd cache was configured from the
# pinned tree at 14:09:56Z today and the default source disagrees with it. That
# refusal is the build protecting the experiment -- without the cache to
# disagree with, the run would have built quietly against the wrong source.
#
# EXTRACTED, never copied, for the same reason preserve_boot_inputs is: a
# duplicated pin drifts from the procedure it is supposed to mirror, and this
# one carries three refusals (contains-the-commit, HEAD-EQUALS-the-commit, and
# a clean worktree) whose load-bearing half is the equality.
awk '/^# THE EXTERNAL STRATUM PIN, which the original handoff did not state and this$/,/^echo "-- stratum pinned: /' work/oct5-as-r9/lease-runbook.sh > "$OUT/stratum-pin.sh"
_pl=$(wc -l < "$OUT/stratum-pin.sh" | tr -d ' ')
[ "${_pl:-0}" -ge 40 ] || {
  echo "REFUSING: the stratum pin block extracted only ${_pl:-0} lines -- the"
  echo "   EXTRACTION is broken, not the runbook."; exit 3; }
# A denominator control on the extraction: the equality check is the half that
# discriminates (rev-parse alone succeeds in any tree holding the object), so
# an extraction that lost it would pass vacuously.
/usr/bin/grep -q 'STRATUM_PIN_FULL=' "$OUT/stratum-pin.sh" || {
  echo "REFUSING: the extracted block has no HEAD-equality pin -- it would"
  echo "   accept any tree that merely contains the object."; exit 3; }
. "$OUT/stratum-pin.sh"
echo "-- stratum lever SET for every build below: STRATUM_SRC=$STRATUM_SRC"

floor "stage 0"

# THE PRISTINE COPY AND THE RECOVERY PATH, before anything can mutate.
PRISTINE=$OUT/loom.c.pristine
cp kernel/loom.c "$PRISTINE"
PRISTINE_HASH=$(shasum -a 256 "$PRISTINE" | cut -d' ' -f1)

recover() { # idempotent, and correct before any mutation has happened
  [ "$RECOVERED" = 0 ] || return 0
  RECOVERED=1
  echo
  echo "=== recovery (every exit path arrives here, not just the happy one) ==="

  # 1. QUARANTINE FIRST -- a rename, so it cannot be refused for cores or disk.
  if [ "$MUTATED" = 1 ]; then
    Q=$OUT/mutant-artifacts-DO-NOT-BOOT
    mkdir -p "$Q"
    _moved=0
    for _f in build/kernel/thylacine.elf build/kernel/thylacine.bin; do
      [ -f "$_f" ] || continue
      mv "$_f" "$Q/" && _moved=$((_moved + 1))
    done
    cat > "$Q/WARNING.txt" <<WARN
These are MUTANT kernel images from $OUT, built with successful private retirement's
lifetime put deliberately deleted (get and guard decrement retained). They exist as evidence of what stage 2
booted. NEVER boot them and never promote them: a boot of this kernel is
expected to FAIL the suite at "${WANT:-<unset -- see release-leg-run.sh>}".
The control kernel's hash for this run was: ${CONTROL_BIN:-unmeasured}
WARN
    echo "-- quarantined $_moved mutant image(s) -> $Q"
    if [ -f build/kernel/thylacine.elf ] || [ -f build/kernel/thylacine.bin ]; then
      echo "!! A MUTANT IMAGE IS STILL IN build/ -- failing closed rather than"
      echo "   leaving one where the next test.sh would boot it."
      RECOVERY_FAILED=1
    else
      echo "-- build/ now holds no kernel image, so a later run must BUILD one"
      echo "   and cannot silently boot the mutant."
    fi
  fi

  # 2. THE SOURCE. A failed restore is a nonzero run, not a printed remark.
  cp "$PRISTINE" kernel/loom.c
  _h=$(shasum -a 256 kernel/loom.c | cut -d' ' -f1)
  if [ "$_h" = "$PRISTINE_HASH" ]; then
    echo "-- kernel/loom.c RESTORED and hash-verified"
  else
    echo "!! RESTORE FAILED: kernel/loom.c is $_h, pristine was $PRISTINE_HASH"
    echo "   The pristine copy is kept at $PRISTINE -- restore it by hand."
    RECOVERY_FAILED=1
  fi

  # 3. THE CLEAN REBUILD, only while it is SAFE to spend the resource. Both
  #    conditions are re-measured here: a run that has been going for an hour
  #    cannot inherit stage 0's disk reading or its lease.
  if [ "$MUTATED" = 1 ] && [ "$RECOVERY_FAILED" = 0 ]; then
    _f=$(free_gb)
    disk_record "pre-recovery-build" "$_f"
    "$YIP" resources > "$OUT/lease-recovery.txt" 2>&1 || true
    if [ "$_f" -lt "$FLOOR_GB" ]; then
      echo "-- NO REBUILD: ${_f} GiB free is under the $FLOOR_GB GiB floor. The"
      echo "   quarantine is already the safe state; the next run builds its own."
    elif ! /usr/bin/grep -qE '^mac +HELD by you' "$OUT/lease-recovery.txt"; then
      echo "-- NO REBUILD: the mac lease is no longer mine, and a build without"
      echo "   one would take cores a peer has been handed. Quarantine stands."
    else
      if tools/build.sh kernel --config ci > "$OUT/recovery-build.log" 2>&1; then
        _r=$(shasum -a 256 build/kernel/thylacine.bin | cut -d' ' -f1)
        if [ "$_r" = "$CONTROL_BIN" ]; then
          echo "-- build/ holds the control kernel again, BYTE-IDENTICAL ($(echo "$_r" | cut -c1-16))"
        else
          echo "!! THE REBUILD IS NOT BYTE-IDENTICAL TO THE CONTROL:"
          echo "   control $CONTROL_BIN"
          echo "   rebuild $_r"
          echo "   build/ is clean of the mutant, but byte identity was the stated"
          echo "   requirement, so this run FAILS CLOSED rather than noting it."
          RECOVERY_FAILED=1
        fi
      else
        echo "!! THE RECOVERY BUILD FAILED -- see $OUT/recovery-build.log."
        echo "   build/ is quarantined, so nothing can boot the mutant, but the"
        echo "   tree is not back to a built state."
        RECOVERY_FAILED=1
      fi
    fi
  fi

  # 4. THE LEASE AND THE EVIDENCE, on every path.
  _f=$(free_gb)
  disk_record "exit" "$_f"
  echo "-- disk at exit: ${_f} GiB free"
  if [ -f "$OUT/disk.txt" ]; then
    echo "-- what this run cost the volume (retained at $OUT/disk.txt):"
    sed 's/^/     /' "$OUT/disk.txt"
  fi
  echo "-- evidence: $OUT"
  echo "=== THE MAC IS STILL HELD BY YOU. The cores are free from here and the"
  echo "    write-up is not a reason to hold them:   yip release mac"
}

on_exit() {
  _rc=$?
  set +e
  trap - EXIT
  recover
  if [ "$RECOVERY_FAILED" = 1 ]; then
    echo "!! RECOVERY INCOMPLETE -- exiting 9. The run's own status was $_rc,"
    echo "   and it is preserved in this message rather than in the exit code,"
    echo "   because a dirty tree outranks the experiment's verdict."
    exit 9
  fi
  exit $_rc
}
trap on_exit EXIT
echo "-- pristine kernel/loom.c held at $PRISTINE ($(echo "$PRISTINE_HASH" | cut -c1-16))"

run_suite() { # run_suite <label>; leaves $OUT/<label>-boot.log and sets suite_rc
  _l=$1
  # _rcfile, not _rc: on_exit's `_rc=$?` is the run's exit STATUS, and this is a
  # PATH to a file holding one. sh has no locals, so two meanings for one name in
  # one script is a trap waiting for the first caller that nests them.
  _rcfile=$OUT/$_l.rc
  ( set +e; tools/test.sh > "$OUT/$_l-test.log" 2>&1; echo $? > "$_rcfile" )
  suite_rc=$(cat "$_rcfile")
  # THE ORACLE IS THE BOOT LOG, not test.sh's stdout -- the suite's own records
  # live in build/test-boot.log and that is what every assertion below reads.
  if [ -f build/test-boot.log ]; then cp build/test-boot.log "$OUT/$_l-boot.log"
  else echo "!! no build/test-boot.log after $_l"; fi
  echo "-- $_l: test.sh exit $suite_rc, boot log $OUT/$_l-boot.log"
}

LEG=loom.private_owner_lifecycle
# THE LEG'S OWN BLOCK, because the verdict is NOT on the announcement line.
# test.c prints "    [test] <name> ... " with NO newline, runs the test, then
# prints "PASS" or "FAIL: <msg>" -- and on a FAIL a "[runnable-dump <msg>]"
# line and a cpu line come first (measured in the retained real log
# red-legs/20261007T103115Z/uncond-refund-serial.log:601-603). Everything the
# test prints lands between, so every assertion reads this block.
leg_block() { # leg_block <boot log> <out file>; nonzero if the leg never ran
  awk -v leg="$LEG" '
    /^[[:space:]]*\[test\] [^ ]+ \.\.\./ {
      if (index($0, "[test] " leg " ... ") > 0) { inblk = 1; print; next }
      if (inblk) exit
    }
    inblk { print }
  ' "$1" > "$2"
  [ -s "$2" ]
}
leg_verdict() { # leg_verdict <block file>: PASS, FAIL or NONE
  if /usr/bin/grep -qE '^FAIL:' "$1"; then echo FAIL
  elif /usr/bin/grep -qE '^PASS' "$1"; then echo PASS
  else echo NONE; fi
}
last_announced() { /usr/bin/grep -E '^[[:space:]]*\[test\] [^ ]+ \.\.\.' "$1" | tail -1; }
# A verdict of ANY test, wherever test.c put it: on its announcement line when
# the test printed nothing, or on a line of its own when it did.
all_fails() { /usr/bin/grep -E '^FAIL:|\[test\] [^ ]+ \.\.\. FAIL:' "$1"; }

check_control() { # check_control <boot log>; 0 = green, nonzero = refuse
  _b=$1; _blk=$OUT/control-leg-block.txt
  if ! leg_block "$_b" "$_blk"; then
    echo "   THE CONTROL NEVER ANNOUNCED $LEG. The last test it announced was:"
    echo "     $(last_announced "$_b")"
    return 1
  fi
  # The most specific thing the log can say comes first: it NAMES the check.
  if /usr/bin/grep -qF 'cleanup-owner-drop after-check-failure:' "$_blk"; then
    echo "   A CHECK INSIDE THE LEG FAILED and the fixture recovered quietly."
    echo "   The marker names it:"
    /usr/bin/grep -nF 'after-check-failure:' "$_blk" | head -2
    return 1
  fi
  _v=$(leg_verdict "$_blk")
  [ "$_v" = PASS ] || {
    echo "   THE LEG DID NOT PASS -- the verdict in its own block is $_v."
    sed -n '1,12p' "$_blk" | sed 's/^/     /'
    return 1; }
  # A passing run must still print the NORMAL marker, or the check above is no
  # longer being made at all.
  /usr/bin/grep -qF 'cleanup-owner-drop normal-fallthrough' "$_blk" || {
    echo "   THE LEG PASSED, but the normal-fallthrough marker is absent, so"
    echo "   the check for a quiet failure above is vacuous. Refusing."
    return 1; }
  echo "-- control: leg announced, teardown normal, verdict PASS in its own block"
  return 0
}

check_mutant() { # check_mutant <boot log>; 0 = discriminated, nonzero = finding
  _m=$1; _blk=$OUT/mutant-leg-block.txt
  # An empty -F pattern matches EVERY line; never let one reach grep.
  [ -n "$WANT" ] || {
    echo "   REFUSING: the predicted assertion message is empty, so every test"
    echo "   below would be \`grep -qF \"\"\`, which matches every line."
    return 2; }
  if ! leg_block "$_m" "$_blk"; then
    _la=$(last_announced "$_m")
    echo "   THE MUTANT RUN IS NOT ATTRIBUTABLE TO THIS LEG: it never announced it."
    echo "   The last test the boot announced was:"
    echo "     ${_la:-<the boot announced no test at all>}"
    return 2
  fi
  _v=$(leg_verdict "$_blk")
  case $_v in
    PASS)
      echo "   THE MUTANT DID NOT PRODUCE THE PREDICTED FAILURE: the leg PASSED."
      echo "   Either the retirement did not reach private_end, or the"
      echo "   assertion cannot see the leaked lifetime reference. A FINDING, not a pass."
      return 2 ;;
    NONE)
      echo "   THE LEG HAS NO VERDICT: the boot died inside it. Under this mutant"
      echo "   that is not the predicted outcome. What the boot did:"
      /usr/bin/grep -nE '^EXTINCTION:' "$_m" | head -4
      return 2 ;;
  esac
  # The leg FAILED -- now WHICH assertion. Exact message, on the verdict line:
  # a different check failing is a different finding.
  if ! /usr/bin/grep -qxF "FAIL: $WANT" "$_blk" && \
     ! /usr/bin/grep -qxF "FAIL: $WANT$(printf '\r')" "$_blk"; then
    echo "   THE LEG FAILED, BUT NOT AT THE PREDICTED ASSERTION. Its verdict:"
    /usr/bin/grep -nE '^FAIL:' "$_blk" | head -2
    return 2
  fi
  _diag=$(/usr/bin/grep -E '^[[:space:]]*\[runnable-dump ' "$_m" | tr -d '\r' | sed 's/^[[:space:]]*//')
  [ "$_diag" = "[runnable-dump $WANT]" ] || {
    echo "   REFUSING: unexpected or extra failure diagnostics"; return 2; }
  # The fixture's own marker must name the same check, or the emission moved
  # and the verdict line is the only thing standing for the attribution.
  /usr/bin/grep -qF "cleanup-owner-drop after-check-failure: $WANT" "$_blk" || {
    echo "   THE VERDICT NAMES THE ASSERTION, but the fixture's after-check-failure"
    echo "   marker does not. The two attributions disagree. A FINDING."
    /usr/bin/grep -nF 'cleanup-owner-drop' "$_blk" | head -2
    return 2; }
  # NOTHING ELSE FAILED: a second failing test would mean the mutant reached
  # beyond this leg, and the suite-level reading would be confounded.
  _nf=$(all_fails "$_m" | wc -l | tr -d ' ')
  [ "$_nf" = 1 ] || {
    echo "   $_nf FAIL verdicts in the boot, not 1 -- the mutant reached beyond"
    echo "   this leg:"
    all_fails "$_m" | head -6
    return 2; }
  _tally=$(/usr/bin/grep -E '  tests: [0-9]+/[0-9]+' "$_m" | tail -1)
  _got=$(echo "$_tally" | sed -E 's/.*tests: ([0-9]+)\/([0-9]+).*/\1 \2/')
  _p=$(echo "$_got" | cut -d' ' -f1); _t=$(echo "$_got" | cut -d' ' -f2)
  [ "$_t" = "$EXPECT_TESTS" ] && [ "$_p" = $((EXPECT_TESTS - 1)) ] || {
    echo "   THE TALLY DISAGREES: '$_tally', wanted $((EXPECT_TESTS - 1))/$EXPECT_TESTS."
    return 2; }
  # The ONLY extinction is the suite's own consequence of a FAIL. Anything else
  # is a different outcome to investigate.
  _ext=$(/usr/bin/grep -E '^EXTINCTION:' "$_m" | tr -d '\r')
  [ "$_ext" = "EXTINCTION: kernel test suite failed" ] || {
    echo "   THE EXTINCTION SET IS NOT EXACTLY THE SUITE'S CONSEQUENCE:"
    /usr/bin/grep -nE '^EXTINCTION:' "$_m" | head -4
    echo "     (no extinction at all is also wrong: a failed suite must end so)"
    return 2; }
  echo "-- attributed: the leg FAILED at its own assertion, named twice (verdict"
  echo "   and after-check-failure marker); no other test failed; tally"
  echo "   $_p/$_t; the only extinction is the suite's consequence"
  echo "-- DISCRIMINATED: the retirement lifetime put is load-bearing, and the leg's"
  echo "   assertion sees the leaked lifetime reference. Final descriptor and"
  echo "   page-table reclamation remain separate obligations."
  return 0
}

echo
lease_and_floor() {
  "$YIP" resources > "$OUT/lease-$1.txt" 2>&1 || true
  /usr/bin/grep -qE '^mac +HELD by you' "$OUT/lease-$1.txt" || exit 6
  floor "$1"
}
echo "=== stage 1: the CONTROL -- the leg must PASS on the real tree ==="
lease_and_floor pre-control-build
tools/build.sh kernel --config ci
floor "post-control-build"
CONTROL_BIN=$(shasum -a 256 build/kernel/thylacine.bin | cut -d' ' -f1)
echo "-- control kernel $(echo "$CONTROL_BIN" | cut -c1-16)"
awk '/^PRESERVE_DIR=work\/oct5-as-r9\/boot-inputs$/,/^}$/' work/oct5-as-r9/lease-runbook.sh > "$OUT/preserve-fn.sh"
. "$OUT/preserve-fn.sh"
preserve_boot_inputs "release-leg-$STAMP" control default || exit 4
run_suite control
B=$OUT/control-boot.log
/usr/bin/grep -qE '^EXTINCTION:' "$B" && { echo "   CONTROL EXTINCTED -- stop, read $B"; exit 1; } || true
tally=$(/usr/bin/grep -E '  tests: [0-9]+/[0-9]+' "$B" | tail -1)
got=$(echo "$tally" | sed -E 's/.*tests: ([0-9]+)\/([0-9]+).*/\1 \2/')
pass=$(echo "$got" | cut -d' ' -f1); total=$(echo "$got" | cut -d' ' -f2)
echo "-- suite: $tally (derived expectation $EXPECT_TESTS)"
[ "$total" = "$EXPECT_TESTS" ] || { echo "   total $total != derived $EXPECT_TESTS"; exit 1; }
[ "$pass" = "$total" ] || { echo "   $pass of $total passed"; exit 1; }
[ "$suite_rc" = 0 ] || { echo "   test.sh exited $suite_rc on the control"; exit 1; }
check_control "$B" || exit 1
echo "-- CONTROL GREEN: the leg passes, suite $pass/$total, no extinction"

echo
echo "=== stage 2: the MUTANT -- the leg fails at its own assertion, nothing else ==="
lease_and_floor pre-mutant-build
MUTATED=1
python3 - <<'PY' || exit 5
import sys
# Expand only the successful retirement call to the current helper body,
# omitting only its put. Keep the get, guard decrement, locking and all rollback
# callers. A global omitted put would break earlier direct AddrSpace tests and
# confound attribution. The leak lasts until this disposable VM ends.
p='kernel/loom.c'
s=open(p).read()
a="    addrspace_private_end(as);\n\n    __atomic_fetch_add(&g_loom_destroyed"
u=open('kernel/addrspace.c').read()
start=u.index('void addrspace_private_end(');end=u.index('\n}',start)
body=u[u.index('{',start)+1:end]
put='    addrspace_lifetime_put(as); // may free; never under as->lock'
if body.count(put)!=1 or s.count(a)!=1:
    sys.exit("REFUSING: retirement call or private_end helper drifted")
body=body.replace(put,'    /* MUTANT: retirement omits only its lifetime put */')
s=s.replace(a,body+'\n\n    __atomic_fetch_add(&g_loom_destroyed',1)
open(p,'w').write(s)
print("-- MUTANT applied only to successful retirement: guard decremented, put omitted")
PY
/usr/bin/grep -qF 'MUTANT: retirement omits only its lifetime put' kernel/loom.c || exit 5
[ "$(/usr/bin/grep -c 'MUTANT:' kernel/loom.c)" = 1 ] || exit 5
diff -u "$PRISTINE" kernel/loom.c > "$OUT/mutant.diff" || true
tools/build.sh kernel --config ci
floor "post-mutant-build"
MUTANT_BIN=$(shasum -a 256 build/kernel/thylacine.bin | cut -d' ' -f1)
[ "$MUTANT_BIN" != "$CONTROL_BIN" ] || {
  echo "REFUSING: the mutant kernel is byte-identical to the control -- the"
  echo "   edit did not reach the binary, so stage 2 would prove nothing."; exit 5; }
echo "-- mutant kernel $(echo "$MUTANT_BIN" | cut -c1-16) (differs from the control)"
run_suite mutant
M=$OUT/mutant-boot.log
# Inside an `if`, because under set -e a bare failing call would exit before
# the status check below could run.
if check_mutant "$M"; then _mv=0; else _mv=$?; fi
# The harness status must agree with the guest. A guest FAIL that test.sh
# reported as success is a harness inconsistency, never a silent accept
# (astra, yip 0161 t69).
if [ "$suite_rc" = 0 ]; then
  echo "   HARNESS INCONSISTENCY: test.sh exited 0 on the mutant boot, whatever"
  echo "   the log says. Refusing to call this discrimination."
  exit 2
fi
echo "-- test.sh exited $suite_rc on the mutant, consistent with the guest FAIL"
[ "$_mv" = 0 ] || exit 2
# Recovery is NOT a stage here: on_exit runs it on this path and every other.
