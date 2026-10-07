#!/bin/sh
# Run the recovery harness against the live runner AND three one-variable
# mutants of it, in one command, with every output retained.
#
# WHY THIS EXISTS AND NOT JUST THE TALLIES. I reported "24/0 unmutated, 23/1,
# 22/2, 23/1" in prose; my reviewer could read the arms but not verify the
# numbers without re-running them (astra, 0161 t57). A tally in prose is a claim;
# a retained RESULTS.txt per run is evidence. And the claim that matters is not
# the COUNT but the DISCRIMINATION -- that each mutant reddens its OWN named arm
# and no other -- so this script ASSERTS the expected arm set per mutant and
# fails if a mutant reddens something else. A mutant that reddens the wrong arm
# would mean the harness detects rather than discriminates, which is the whole
# difference between a control and a decoration.
#
# OFF-LEASE: no cores, no build, no QEMU. Shell and text only.
set -e
ROOT=${ROOT:-$(cd "$(dirname "$0")/../.." && pwd)}   # overridable so a COPY of this driver can be driven against the real tree
cd "$ROOT"
STAMP=$(date -u '+%Y%m%dT%H%M%SZ')
EV=work/oct5-as-r9/recovery-test-$STAMP
RUNNER=work/oct5-as-r9/reap-leg-run.sh
HARNESS=work/oct5-as-r9/reap-leg-recovery-test.sh
mkdir -p "$EV"
MAN=$EV/MANIFEST.txt
fail=0

{
  echo "recovery harness: control + three one-variable mutants"
  echo "stamp   : $STAMP"
  echo "runner  : $RUNNER  sha256 $(shasum -a 256 "$RUNNER" | cut -d' ' -f1)"
  echo "harness : $HARNESS sha256 $(shasum -a 256 "$HARNESS" | cut -d' ' -f1)"
  echo "HEAD    : $(git rev-parse HEAD)"
  echo
} > "$MAN"

# Each mutation is a NAMED defect with a UNIQUE anchor, asserted before it is
# applied: an anchor that has stopped matching means the runner moved under this
# script, which must refuse rather than silently test an unmutated copy.
mutate() { # mutate <which> <dest>
  python3 - "$1" "$2" "$RUNNER" <<'PY'
import sys
which, dest, src = sys.argv[1], sys.argv[2], sys.argv[3]
s = open(src).read()
pairs = {
 # A rebuild that does not match the control only PRINTS, never fails closed.
 'm1': ("""          echo "   requirement, so this run FAILS CLOSED rather than noting it."
          RECOVERY_FAILED=1""",
        """          echo "   requirement, so this run FAILS CLOSED rather than noting it.\""""),
 # The disk floor is never re-measured before the recovery rebuild.
 'm2': ("""    if [ "$_f" -lt "$FLOOR_GB" ]; then""",
        """    if false; then"""),
 # A source restore whose hash does not verify only PRINTS.
 'm3': ("""    echo "   The pristine copy is kept at $PRISTINE -- restore it by hand."
    RECOVERY_FAILED=1""",
        """    echo "   The pristine copy is kept at $PRISTINE -- restore it by hand.\""""),
}
old, new = pairs[which]
n = s.count(old)
if n != 1:
    sys.exit("REFUSING: %s's anchor matches %d times, not once -- the runner moved" % (which, n))
open(dest, 'w').write(s.replace(old, new))
PY
}

# run <label> <script> <expected-wrong-count> <expected-arm-prefixes...>
run() {
  _label=$1; _script=$2; _want=$3; shift 3
  _dir=$EV/$_label
  printf '== %s\n' "$_label" >> "$MAN"
  set +e
  EVIDENCE="$ROOT/$_dir" RUN="$ROOT/$_script" sh "$HARNESS" > "$_dir.stdout" 2>&1
  _rc=$?
  set -e
  _res=$_dir/RESULTS.txt
  if [ ! -f "$_res" ]; then
    echo "   NO RESULTS.txt -- the harness did not run (rc $_rc)" >> "$MAN"; fail=1; return
  fi
  _tally=$(/usr/bin/grep '^RESULT:' "$_res" | tail -1)
  _got=$(/usr/bin/grep -c '^  WRONG' "$_res" || true)
  printf '   %s (harness rc %s)\n' "$_tally" "$_rc" >> "$MAN"
  if [ "${_got:-0}" != "$_want" ]; then
    printf '   MISMATCH: expected %s red check(s), got %s\n' "$_want" "${_got:-0}" >> "$MAN"
    fail=1
  fi
  # THE DISCRIMINATION CLAIM: every red check must belong to an EXPECTED arm.
  if [ "$_want" != 0 ]; then
    /usr/bin/grep '^  WRONG' "$_res" | sed 's/^  WRONG: //' > "$_dir.reds"
    while IFS= read -r _line; do
      _hit=0
      for _pfx in "$@"; do
        case "$_line" in "$_pfx"*) _hit=1 ;; esac
      done
      if [ "$_hit" = 1 ]; then
        printf '   expected red: %s\n' "$_line" >> "$MAN"
      else
        printf '   UNEXPECTED RED (the harness is not discriminating as claimed): %s\n' "$_line" >> "$MAN"
        fail=1
      fi
    done < "$_dir.reds"
  fi
  printf '   evidence: %s\n\n' "$_dir" >> "$MAN"
}

run control "$RUNNER" 0
for m in m1 m2 m3; do
  mutate "$m" "$EV/$m.sh"
  case $m in
    m1) run "$m" "$EV/$m.sh" 1 "R3" ;;
    m2) run "$m" "$EV/$m.sh" 2 "R4" ;;
    m3) run "$m" "$EV/$m.sh" 1 "R7" ;;
  esac
done

if [ "$fail" = 0 ]; then
  echo "VERDICT: the control is clean and each mutant reddens ONLY its named arm." >> "$MAN"
else
  echo "VERDICT: FAILED -- read the mismatches above before trusting the harness." >> "$MAN"
fi
cat "$MAN"
[ "$fail" = 0 ]
