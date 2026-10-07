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
# A MUTANT EDITS A PRODUCTION FILE, so the restore is not left to the happy
# path: the pristine copy is hashed before the edit, an EXIT trap restores it
# however this script dies, and the hash is re-verified afterwards. And because
# a clean TREE is not a clean ARTIFACT -- test.sh boots whatever is in build/ --
# stage 3 rebuilds and requires the kernel to come back BYTE-IDENTICAL to the
# control's, which both cleans build/ and is a free determinism datum.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
STAMP=$(date -u '+%Y%m%dT%H%M%SZ')
OUT=work/oct5-as-r9/reap-leg-$STAMP
mkdir -p "$OUT"
YIP="${THYLA_WAKE_YIP:-$(command -v yip || echo "$HOME/.local/bin/yip")}"
FLOOR_GB=${FLOOR_GB:-8}
free_gb() { df -g . | awk 'NR==2 {print $4}'; }
floor() { # floor <label>
  _f=$(free_gb)
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
floor "stage 0"

# THE PRISTINE COPY AND THE TRAP, before anything can mutate.
PRISTINE=$OUT/addrspace.c.pristine
cp kernel/addrspace.c "$PRISTINE"
PRISTINE_HASH=$(shasum -a 256 "$PRISTINE" | cut -d' ' -f1)
restore() {
  cp "$PRISTINE" kernel/addrspace.c
  _h=$(shasum -a 256 kernel/addrspace.c | cut -d' ' -f1)
  if [ "$_h" = "$PRISTINE_HASH" ]; then echo "-- kernel/addrspace.c RESTORED and hash-verified"
  else echo "!! RESTORE FAILED: kernel/addrspace.c is $_h, pristine was $PRISTINE_HASH"; fi
}
trap restore EXIT
echo "-- pristine kernel/addrspace.c held at $PRISTINE ($(echo "$PRISTINE_HASH" | cut -c1-16))"

run_suite() { # run_suite <label>; leaves $OUT/<label>-boot.log and sets suite_rc
  _l=$1
  _rc=$OUT/$_l.rc
  ( set +e; tools/test.sh > "$OUT/$_l-test.log" 2>&1; echo $? > "$_rc" )
  suite_rc=$(cat "$_rc")
  # THE ORACLE IS THE BOOT LOG, not test.sh's stdout -- the suite's own records
  # live in build/test-boot.log and that is what every assertion below reads.
  if [ -f build/test-boot.log ]; then cp build/test-boot.log "$OUT/$_l-boot.log"
  else echo "!! no build/test-boot.log after $_l"; fi
  echo "-- $_l: test.sh exit $suite_rc, boot log $OUT/$_l-boot.log"
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
# BY NAME, not by the total: a suite that lost this test would still total right
# if something else were added, and the total is not what this run is about.
/usr/bin/grep -qE '\[test\] loom\.private_owner_lifecycle \.\.\. PASS' "$B" || {
  echo "   THE LEG DID NOT PASS. Its error string names the failing check:"
  /usr/bin/grep -nE 'loom\.private_owner_lifecycle|unpinned-reap|retirement|FAIL' "$B" | head -10
  exit 1; }
tally=$(/usr/bin/grep -E '  tests: [0-9]+/[0-9]+' "$B" | tail -1)
got=$(echo "$tally" | sed -E 's/.*tests: ([0-9]+)\/([0-9]+).*/\1 \2/')
pass=$(echo "$got" | cut -d' ' -f1); total=$(echo "$got" | cut -d' ' -f2)
echo "-- suite: $tally (derived expectation $EXPECT_TESTS)"
[ "$total" = "$EXPECT_TESTS" ] || { echo "   total $total != derived $EXPECT_TESTS"; exit 1; }
[ "$pass" = "$total" ] || { echo "   $pass of $total passed"; exit 1; }
[ "$suite_rc" = 0 ] || { echo "   test.sh exited $suite_rc on the control"; exit 1; }
echo "-- CONTROL GREEN: the leg passes, suite $pass/$total, no extinction"

echo
echo "=== stage 2: the MUTANT -- one named invariant failure, nothing else ==="
python3 - <<'PY' || exit 5
import sys
p='kernel/addrspace.c'
s=open(p).read()
a="    addrspace_lifetime_get(as);\n    ++as->private_rings;\n"
b="    --as->private_rings;\n    spin_unlock(&as->lock);\n    addrspace_lifetime_put(as); // may free; never under as->lock\n"
if s.count(a)!=1 or s.count(b)!=1:
    sys.exit("REFUSING: mutation anchors are not unique (%d, %d) -- the file moved under this script" % (s.count(a), s.count(b)))
s=s.replace(a, "    ++as->private_rings;  /* MUTANT: the ring takes NO lifetime ref */\n")
s=s.replace(b, "    --as->private_rings;\n    spin_unlock(&as->lock);\n")
open(p,'w').write(s)
print("-- MUTANT applied: balanced removal of the ring's lifetime get/put")
PY
/usr/bin/grep -q 'MUTANT: the ring takes NO lifetime ref' kernel/addrspace.c || {
  echo "REFUSING: the mutation did not land"; exit 5; }
tools/build.sh kernel --config ci
floor "post-mutant-build"
MUTANT_BIN=$(shasum -a 256 build/kernel/thylacine.bin | cut -d' ' -f1)
[ "$MUTANT_BIN" != "$CONTROL_BIN" ] || {
  echo "REFUSING: the mutant kernel is byte-identical to the control -- the"
  echo "   edit did not reach the binary, so stage 2 would prove nothing."; exit 5; }
echo "-- mutant kernel $(echo "$MUTANT_BIN" | cut -c1-16) (differs from the control)"
run_suite mutant
M=$OUT/mutant-boot.log
WANT='AddrSpace final lifetime drop with private rings'
if /usr/bin/grep -qF "$WANT" "$M"; then
  echo "-- MUTANT DIED AS PREDICTED, by name:"
  /usr/bin/grep -nF "$WANT" "$M" | head -3
  # AND NOTHING ELSE: a second, different extinction would mean the leg is not
  # the thing that fired, and the prediction would be right by accident.
  others=$(/usr/bin/grep -E '^EXTINCTION:' "$M" | /usr/bin/grep -vcF "$WANT" || true)
  [ "${others:-0}" = 0 ] || {
    echo "   BUT $others OTHER extinction(s) fired too -- read $M before concluding:"
    /usr/bin/grep -nE '^EXTINCTION:' "$M" | head -6; exit 2; }
  /usr/bin/grep -qE '\[test\] loom\.private_owner_lifecycle \.\.\. PASS' "$M" && {
    echo "   AND THE LEG REPORTED PASS ANYWAY -- the extinction came from"
    echo "   somewhere else entirely. Investigate; do not call this discrimination."
    exit 2; } || true
  echo "-- DISCRIMINATED: the ring's own lifetime reference is load-bearing."
else
  echo "   THE MUTANT DID NOT PRODUCE THE PREDICTED FAILURE."
  echo "   This is a FINDING to investigate, not a pass and not a reason to"
  echo "   weaken the guard. What the boot did instead:"
  /usr/bin/grep -nE '^EXTINCTION:|FAIL|tests: ' "$M" | head -12
  echo "   (test.sh exit was $suite_rc; full log $M)"
  exit 2
fi

echo
echo "=== stage 3: restore, and leave build/ holding a CLEAN kernel ==="
restore
trap - EXIT
tools/build.sh kernel --config ci
REBUILD_BIN=$(shasum -a 256 build/kernel/thylacine.bin | cut -d' ' -f1)
if [ "$REBUILD_BIN" = "$CONTROL_BIN" ]; then
  echo "-- build/ holds the control kernel again, BYTE-IDENTICAL ($(echo "$REBUILD_BIN" | cut -c1-16))"
else
  echo "!! the rebuild is NOT byte-identical to the control:"
  echo "   control $CONTROL_BIN"
  echo "   rebuild $REBUILD_BIN"
  echo "   build/ is clean of the mutant, but the difference needs explaining."
fi
floor "end"
echo
echo "RELEASE THE MAC NOW -- the cores are free from here; the write-up is not"
echo "a reason to hold them. Evidence: $OUT"
