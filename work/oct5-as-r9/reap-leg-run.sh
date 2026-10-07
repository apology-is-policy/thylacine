#!/bin/sh
# The unpinned-reap leg: CONTROL then MUTANT, in one lease window.
# PREPARED OFF-LEASE, deliberately, so the scarce Mac lease is spent executing.
#
# WHAT IT PROVES, and the two halves are not interchangeable:
#   CONTROL  the leg passes on the real tree -- which on its own is worth little,
#            because a leg that asserts nothing would also pass.
#   MUTANT   with the ring's own AddrSpace lifetime reference removed (balanced:
#            the get in addrspace_private_begin AND the put in _end, keeping
#            ++private_rings), the boot must die with ONE NAMED invariant
#            failure: "AddrSpace final lifetime drop with private rings"
#            (kernel/addrspace.c). Requiring the exact message rather than a
#            nonzero exit is what makes this discriminating instead of merely
#            detecting: a crash, a hang, a different extinction or a plain FAIL
#            are all DIFFERENT outcomes and each is a finding to investigate,
#            not a pass (astra, yip 0161 t53).
#
# WHAT IT DOES NOT PROVE: that the ring RELEASES the reference it takes. The
# balanced mutant tests ACQUISITION; the release half has no witness here and is
# recorded as open in the fixture's own comment (astra, yip 0161 t55).
#
# RECOVERY IS AN ALL-EXIT REQUIREMENT, NOT A FINAL STAGE (astra, yip 0161 t55).
# A mutant edits a production file AND leaves a mutant kernel in build/, and a
# clean TREE is not a clean ARTIFACT -- test.sh boots whatever build/ holds. So
# every exit path, including a failed build, a refused floor and a finding in
# stage 2, runs the same recover():
#   1. QUARANTINE FIRST. The mutant images are RENAMED out of build/ -- same
#      volume, so it needs no cores and no disk headroom and is the one step
#      that can never be refused. build/ is then imageless, which a later
#      test.sh must repair by building, and cannot satisfy by booting a mutant.
#   2. The source is restored and hash-verified, and a mismatch is NONZERO
#      rather than a printed remark.
#   3. The clean rebuild is attempted ONLY while the disk floor holds AND the
#      lease is still mine, both re-measured at that moment. When it runs, the
#      kernel must come back BYTE-IDENTICAL to the control or the run FAILS
#      CLOSED; when it is refused, the quarantine is already the safe state.
#   4. The original exit status is preserved; an incomplete recovery overrides
#      it with 9, so "the experiment worked but the tree is dirty" cannot exit 0.
# THE LEASE: this script never releases it -- a finding is investigated with the
# machine held, and a re-acquire means the back of a 24h queue. It prints the
# release line on every exit path instead, and the holder (thyla-wake, or me)
# releases the moment the cores are no longer needed, failure paths included.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
STAMP=$(date -u '+%Y%m%dT%H%M%SZ')
OUT=work/oct5-as-r9/reap-leg-$STAMP
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
WANT='AddrSpace final lifetime drop with private rings'
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

# THE LEG MUST BE IN THE FIXTURE. Otherwise this script cheerfully measures the
# tree as it was before the leg was written.
legs=$(/usr/bin/grep -c 'unpinned-reap' kernel/test/loom_private_fixture.h || true)
[ "${legs:-0}" -ge 3 ] || {
  echo "REFUSING: kernel/test/loom_private_fixture.h names 'unpinned-reap' only"
  echo "   ${legs:-0} time(s) -- the leg under test is not in this tree."; exit 3; }
/usr/bin/grep -q 'retires exactly once' kernel/test/loom_private_fixture.h || {
  echo "REFUSING: the exact-once delta assertion is absent from the fixture."; exit 3; }
echo "-- fixture carries the leg ($legs references) and its delta assertion"

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
These are MUTANT kernel images from $OUT, built with the ring's AddrSpace
lifetime reference deliberately removed. They exist as evidence of what stage 2
booted. NEVER boot them and never promote them: a boot of this kernel is
expected to die on "${WANT:-<unset -- see reap-leg-run.sh>}".
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
# test.c prints "    [test] <name> ... " with NO newline (kernel/test/test.c
# :4481-4483), then runs the test, then prints "PASS\n" or "FAIL: <msg>\n"
# (:4605-4610). Everything the test itself prints therefore lands BETWEEN the
# two, and the arrival marker's own newline pushes the verdict onto a later
# line -- so a one-line grep for `<name> ... PASS` cannot match an instrumented
# HEALTHY run, and the first version of this oracle would have refused the
# control for that reason alone (astra, yip 0161 t63). Every assertion about the
# leg is made against this block, which also makes the attribution positive: a
# marker or a verdict from anywhere else in the log cannot satisfy it.
# The block runs from the leg's announcement to the next test's announcement, or
# to the end of the log when the boot died inside the leg.
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
# The verdict as a STATE, read from the block: PASS, FAIL or NONE. NONE is the
# mutant's expected state -- the boot died before test.c could print either --
# and it is a different thing from FAIL, which the first oracle could not say.
leg_verdict() { # leg_verdict <block file>
  if /usr/bin/grep -qE '^FAIL:' "$1"; then echo FAIL
  elif /usr/bin/grep -qE '^PASS' "$1"; then echo PASS
  else echo NONE; fi
}
# The last test the boot ANNOUNCED. The pattern must match announcements ONLY:
# the suite also prints summary lines beginning "[test] " -- e.g.
# "[test] yield-waits: 724 invoked, ..." -- and a bare prefix match picks one of
# those up as "the last test", a pattern matching the wrong thing and returning
# a confident wrong answer. An announcement is `[test] <name> ... `.
last_announced() { /usr/bin/grep -E '^[[:space:]]*\[test\] [^ ]+ \.\.\.' "$1" | tail -1; }

check_control() { # check_control <boot log>; 0 = green, nonzero = refuse
  _b=$1; _blk=$OUT/control-leg-block.txt
  if ! leg_block "$_b" "$_blk"; then
    echo "   THE CONTROL NEVER ANNOUNCED $LEG. The last test it announced was:"
    echo "     $(last_announced "$_b")"
    return 1
  fi
  # A QUIET CHECK FAILURE FIRST, because it is the most specific thing the log
  # can say -- it NAMES the check. A failure before the target drop also leaves
  # the arrival marker absent, so an arrival-first order answers a refusal with
  # "it never arrived" and sends the reader hunting for a reason the log is
  # already holding. Measured on the early-check-failure arm, which came back
  # with the arrival message until this was reordered.
  # Note what the discriminating text is NOT: "the cleanup ran". `done:` is the
  # normal fallthrough too, so a passing run prints a cleanup marker and an
  # oracle refusing on ANY cleanup marker refuses everything (astra, 0161 t63).
  if /usr/bin/grep -qF 'cleanup-owner-drop after-check-failure:' "$_blk"; then
    echo "   A CHECK INSIDE THE LEG FAILED and the fixture recovered quietly."
    echo "   The marker names it:"
    /usr/bin/grep -nF 'after-check-failure:' "$_blk" | head -2
    return 1
  fi
  # ARRIVAL. A control whose leg never reached the drop would pass without
  # exercising the operation stage 2 is about to test, which would make the
  # comparison meaningless in the quietest possible way.
  if ! /usr/bin/grep -qF '[lp-mark] unpinned-reap-owner-drop' "$_blk"; then
    echo "   THE CONTROL's leg never reached its owner drop -- the arrival"
    echo "   marker is absent from the leg's own block, so it passed WITHOUT"
    echo "   exercising what the mutant tests. Refusing before stage 2."
    return 1
  fi
  _v=$(leg_verdict "$_blk")
  [ "$_v" = PASS ] || {
    echo "   THE LEG DID NOT PASS -- the verdict in its own block is $_v."
    echo "   The block, which carries the failing check's message:"
    sed -n '1,12p' "$_blk" | sed 's/^/     /'
    return 1; }
  # And the NORMAL teardown marker must be present on a passing run: its absence
  # would mean the emission moved, so the discrimination just above is no longer
  # being made at all -- a check that cannot fail, passing.
  /usr/bin/grep -qF 'cleanup-owner-drop normal-fallthrough' "$_blk" || {
    echo "   THE LEG PASSED, but the normal-fallthrough marker is absent, so"
    echo "   the fixture no longer distinguishes a quiet check failure from an"
    echo "   ordinary fallthrough and the check above is vacuous. Refusing."
    return 1; }
  echo "-- control: leg announced, arrival marker present, teardown normal,"
  echo "   verdict PASS in the leg's own block"
  return 0
}

check_mutant() { # check_mutant <boot log>; 0 = discriminated, nonzero = finding
  _m=$1; _blk=$OUT/mutant-leg-block.txt
  # AN EMPTY PREDICTED FAILURE IS NEVER A QUESTION FOR grep. Measured on this
  # host: `/usr/bin/grep -qF "" <file>` returns 0 -- an empty -F pattern matches
  # EVERY line -- so a lost or renamed definition would report the predicted
  # extinction in ANY log at all, which is the one direction that must not fail
  # open. (A first attempt to drive this appeared to show the opposite branch.
  # The cause was not grep: I was driving a STALE extraction, from a directory
  # the harness had stopped writing to when its output path became
  # configurable. Fresh code, stale copy -- and the reading was confident.)
  [ -n "$WANT" ] || {
    echo "   REFUSING: the predicted-failure string is empty, so the test below"
    echo "   would be \`grep -qF \"\"\`, which matches every line. This run can"
    echo "   say nothing about the mutant."
    return 2; }
  if ! /usr/bin/grep -qF "$WANT" "$_m"; then
    echo "   THE MUTANT DID NOT PRODUCE THE PREDICTED FAILURE."
    echo "   This is a FINDING to investigate, not a pass and not a reason to"
    echo "   weaken the guard. What the boot did instead:"
    /usr/bin/grep -nE '^EXTINCTION:|FAIL|tests: ' "$_m" | head -12
    return 2
  fi
  echo "-- MUTANT DIED AS PREDICTED, by name:"
  /usr/bin/grep -nF "$WANT" "$_m" | head -3
  # AND NOTHING ELSE: a second, different extinction would mean the leg is not
  # the thing that fired, and the prediction would be right by accident.
  _others=$(/usr/bin/grep -E '^EXTINCTION:' "$_m" | /usr/bin/grep -vcF "$WANT" || true)
  [ "${_others:-0}" = 0 ] || {
    echo "   BUT $_others OTHER extinction(s) fired too -- read $_m first:"
    /usr/bin/grep -nE '^EXTINCTION:' "$_m" | head -6
    return 2; }
  # ATTRIBUTION -- the half the first version LACKED, and whose absence turned a
  # vacuously-satisfied negative into a false "DISCRIMINATED". The old check only
  # asked that the leg did not report PASS, which is exactly what a leg that
  # NEVER RAN also satisfies: the first mutant killed the boot inside
  # addrspace.proc_alloc_in_shares, ~90 suite lines before this leg, and the
  # runner reported discrimination anyway (reap-leg-20261007T190621Z). A gauge
  # reading zero is satisfied by "it never started", so ask POSITIVELY.
  if ! leg_block "$_m" "$_blk"; then
    echo "   THE EXTINCTION IS NOT ATTRIBUTABLE TO THIS LEG."
    _la=$(last_announced "$_m")
    echo "   The last test the boot announced was:"
    echo "     ${_la:-<the boot announced no test at all>}"
    echo "   so the mutant killed it BEFORE $LEG ran, and this run says NOTHING"
    echo "   about the leg. A FINDING, not a pass, and not a reason to weaken"
    echo "   the mutant until the cause is known."
    return 2
  fi
  # ARRIVAL, NOT POSITION (astra, yip 0161 t61). Test-granularity attribution is
  # necessary and NOT sufficient: LP_CHECK is `goto done`, and the cleanup there
  # unrefs the ring and drops the owner too, so ANY earlier check failure reaches
  # an owner drop with a ring outstanding and raises the SAME named extinction
  # inside the SAME test -- while hiding the check that actually failed. Line
  # order is not execution order.
  if ! /usr/bin/grep -qF '[lp-mark] unpinned-reap-owner-drop' "$_blk"; then
    echo "   THE LEG NEVER REACHED ITS OWNER DROP -- the arrival marker is"
    echo "   absent from the leg's block, so whatever died, it was not this"
    echo "   operation. A FINDING, not a pass."
    # WHOLE LOG here, deliberately, while the assertion above reads the BLOCK:
    # the question has just become "where DID the markers appear", and confining
    # the answer to a block known not to contain them would print nothing.
    /usr/bin/grep -nF '[lp-mark]' "$_m" | head -4
    return 2
  fi
  # ANY cleanup marker, either variant, means the boot got PAST the target drop
  # and died (or recovered) somewhere else -- so under the lethal mutant the
  # marker's presence is itself disqualifying, whichever variant it is
  # (astra, yip 0161 t63).
  if /usr/bin/grep -qF '[lp-mark] cleanup-owner-drop' "$_blk"; then
    echo "   THE CLEANUP PATH RAN, so the boot survived the target drop and this"
    echo "   extinction is not the one this leg predicts. The marker line says"
    echo "   which arrival it was, and names the check if one failed:"
    /usr/bin/grep -nF 'cleanup-owner-drop' "$_blk" | head -2
    return 2
  fi
  # And it must have DIED INSIDE the leg, not completed it: a verdict in the
  # block means the extinction came from somewhere after the leg, a third
  # outcome and equally not discrimination. This is the check the arrival
  # marker's newline silently disabled in the first version, which looked for
  # PASS/FAIL on the announcement line where it can no longer appear.
  _v=$(leg_verdict "$_blk")
  [ "$_v" = NONE ] || {
    echo "   THE LEG COMPLETED (verdict $_v in its own block) and yet the named"
    echo "   extinction fired in the same boot. Investigate; do not call this"
    echo "   discrimination."
    return 2; }
  echo "-- attributed: arrival marker present, no cleanup marker, no verdict --"
  echo "   the boot died inside this leg, at the drop under test"
  echo "-- DISCRIMINATED: the ring's own lifetime reference is load-bearing."
  echo "   ACQUISITION only. The release half has no witness in this run."
  return 0
}

echo
echo "=== stage 1: the CONTROL -- the leg must PASS on the real tree ==="
tools/build.sh kernel --config ci
floor "post-control-build"
CONTROL_BIN=$(shasum -a 256 build/kernel/thylacine.bin | cut -d' ' -f1)
echo "-- control kernel $(echo "$CONTROL_BIN" | cut -c1-16)"
# Preserve this generation with the runbook's own step, EXTRACTED from the live
# file so this cannot drift from the step that is tested.
awk '/^PRESERVE_DIR=work\/oct5-as-r9\/boot-inputs$/,/^}$/' work/oct5-as-r9/lease-runbook.sh > "$OUT/preserve-fn.sh"
. "$OUT/preserve-fn.sh"
preserve_boot_inputs "reap-leg-$STAMP" control default || exit 4
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
# THE LEG ITSELF, by name and in its own block -- never by the suite total: a
# suite that lost this test would still total right if something else were
# added, and the total is not what this run is about.
check_control "$B" || exit 1
echo "-- CONTROL GREEN: the leg passes, suite $pass/$total, no extinction"

echo
echo "=== stage 2: the MUTANT -- one named invariant failure, nothing else ==="
# Set BEFORE the edit: a mutation that dies half-written has still mutated.
MUTATED=1
python3 - <<'PY' || exit 5
import sys
# CONFINED TO loom.c, AND BALANCED. Both properties were learned the hard way.
#
# CONFINED, because the first version mutated addrspace_private_begin/_end
# themselves and that is LETHAL IN AN EARLIER TEST: test_addrspace.c's
# private_ring_sharing_failure() ends `addrspace_private_begin(as);
# addrspace_unref(as);` and asserts the space survives OWNERLESS because the
# guard pins it, so with the guard's reference gone that unref is the final drop
# with private_rings == 1 and the named extinction fires there --
# addrspace.proc_alloc_in_shares, ~90 suite lines before this leg. Measured:
# reap-leg-20261007T190621Z died there and the runner called it discrimination.
#
# BALANCED, because the obvious confined form is NOT. Adding addrspace_unpin
# after the begin cancels the ring's GET (net 0 ref, +1 ring at create -- right),
# but loom_private_destroy still calls addrspace_private_end, whose PUT then has
# no matching get, so every ring cycle nets -1 on the AddrSpace refcount. The
# early legs would hit zero while their Proc is still alive and die with
# "AddrSpace final lifetime drop with live owners" or "addrspace_unref of an
# already-released AddrSpace" -- a different extinction, in a different leg,
# which this runner would correctly refuse but which would waste the window.
# So the destroy's put is removed too, by decrementing the guard directly.
#
# AND THE GET-CANCEL SITS AT THE SUCCESSFUL RETURN, not immediately after the
# begin (astra, yip 0161 t61). Cancelling right after begin leaves
# loom_create_private's two rollback exits (!charged, layout-alloc) over-putting
# by one, which I had excused as unreachable in this fixture -- an assumption
# about allocation failures that a mutant run has no business resting on.
# Cancelling after charge AND layout have both succeeded confines the net-zero
# reference change to Looms that actually exist, leaves every rollback path
# balanced, and needs no patch at three failure sites.
#
# WHY TEST-GRANULARITY ATTRIBUTION IS ENOUGH UNDER THIS MUTANT, verified per leg
# rather than asserted: the fixture packs eight create sites into ONE test, so
# "died inside this test" does not by itself name the leg. But a guard can only
# fire where the owner's drop is the FINAL lifetime drop with a ring still
# outstanding, and every earlier leg excludes exactly that -- :120, :138 and :159
# each lp_wait the ring to retirement BEFORE their Proc is dropped, and :171
# takes its own addrspace_pin before dropping at :172, which is the masking this
# leg exists to remove. The unpinned-reap drop at :248 precedes its lp_wait at
# :249 with no pin held, so it is the FIRST lethal point in the test.
#
# AND THE OUTCOME IS DETERMINISTIC, NOT RACY -- verified in the source rather
# than hoped for, because the alternative would have made this run ambiguous.
# Under this mutant the ring holds no lifetime reference, so one might expect a
# race: if the retirer destroyed the ring before the dying Proc reached its
# lifetime drop, private_rings would already be 0, no guard would fire, and the
# leg would simply PASS -- a wasted window with an unreadable result. It cannot
# happen. proc_free releases the address space at kernel/proc.c:699, BEFORE
# handle_table_free at :720 (deliberately -- see its own comment at :686). At
# :699 the handle table is still intact, so the Loom still holds its refcount,
# loom_unref has not run, nothing has been enqueued, and private_rings is 1.
# addrspace_unref then drops the owner, drains the VMAs and puts the lifetime
# reference, so the guard's three checks are reached in the state the prediction
# names: owners 0, vmas drained, private_rings 1. The retirer never gets a turn,
# because the enqueue would only happen at :720 and the boot is already dead.
# That same ordering is what makes this leg's claim true at all.
p='kernel/loom.c'
s=open(p).read()
a="    burrow_charge_record(l->ring, p, backing);\n    return l;\n"
b=("    addrspace_uncharge_pages(as, metadata + refund);\n"
   "    spin_unlock(&as->lock);\n"
   "    addrspace_private_end(as);\n")
if s.count(a)!=1 or s.count(b)!=1:
    sys.exit("REFUSING: mutation anchors are not unique (%d, %d) -- the file moved under this script" % (s.count(a), s.count(b)))
s=s.replace(a, "    burrow_charge_record(l->ring, p, backing);\n"
               "    addrspace_unpin(as);  /* MUTANT: cancel the ring's lifetime GET */\n"
               "    return l;\n", 1)
s=s.replace(b,
   "    addrspace_uncharge_pages(as, metadata + refund);\n"
   "    --as->private_rings;  /* MUTANT: drop the guard without its PUT */\n"
   "    spin_unlock(&as->lock);\n", 1)
open(p,'w').write(s)
print("-- MUTANT applied: the ring keeps NO lifetime reference, balanced, loom-only")
PY
# BOTH halves must land: a half-applied balanced mutation is the unbalanced one.
for _m in "MUTANT: cancel the ring's lifetime GET" "MUTANT: drop the guard without its PUT"; do
  /usr/bin/grep -qF "$_m" kernel/loom.c || {
    echo "REFUSING: mutation half did not land: $_m"; exit 5; }
done
echo "-- both mutation halves present (an unbalanced half-apply is refused)"
tools/build.sh kernel --config ci
floor "post-mutant-build"
MUTANT_BIN=$(shasum -a 256 build/kernel/thylacine.bin | cut -d' ' -f1)
[ "$MUTANT_BIN" != "$CONTROL_BIN" ] || {
  echo "REFUSING: the mutant kernel is byte-identical to the control -- the"
  echo "   edit did not reach the binary, so stage 2 would prove nothing."; exit 5; }
echo "-- mutant kernel $(echo "$MUTANT_BIN" | cut -c1-16) (differs from the control)"
run_suite mutant
M=$OUT/mutant-boot.log
# The oracle is a FUNCTION so the real code can be driven against
# constructed logs off-lease, which is the only way to find a defect like
# the two astra caught in t63: both were in the INTERACTION between the
# markers and test.c's own output, and marker-only synthetic arms could not
# contain that interaction at all.
check_mutant "$M" || exit 2
# Recovery is NOT a stage here: on_exit runs it on this path and every other.
