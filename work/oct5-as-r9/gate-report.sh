#!/bin/sh
# Turn a lease-runbook run log into the numbers astra asked for, by QUOTING the
# run's own asserted output rather than re-deriving anything.
#
# WHY IT QUOTES INSTEAD OF PARSING THE ARTIFACTS AGAIN: stage 5 already measures
# the per-label clean-boot counts and the per-boot D7 witnesses, and it REFUSES
# when the evidence is short, stale or unreadable. A second parser over the same
# logs could disagree with the gate that gated -- and the one that would be
# believed is whichever I pasted into the call. There is also a worked failure
# behind this: on 10-06 I reported witnesses as "enforced in the script" when I
# had counted them by eye. A report built from the script's own lines cannot be
# my eyes again.
#
# Each section carries its own ABSENCE check, because a tidy report with a
# section silently missing is worse than a loud gap.
set -u
LOG="${1:-}"
[ -n "$LOG" ] && [ -r "$LOG" ] || { echo "usage: gate-report.sh <run-log>"; exit 2; }
missing=0

section() {        # section <title> <grep-ere> <min-lines>
  printf '\n== %s ==\n' "$1"
  n=$(grep -cE "$2" "$LOG" || true)
  if [ "$n" -lt "$3" ]; then
    echo "   MISSING: $n line(s) matched, expected at least $3."
    echo "   The run did not reach this far, so there is nothing to report here."
    missing=$((missing + 1))
    return 0
  fi
  grep -E "$2" "$LOG" | sed 's/^ *//'
}

echo "gate-report for $LOG"
echo "run started : $(head -3 "$LOG" | grep -m1 -oE '[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9:]+Z' || echo '(no stamp in the first lines)')"
echo "runbook exit: $(grep -m1 -oE 'runbook exited [0-9]+' "$LOG" || echo '(not recorded in this log)')"

section "ci-smp-gate verdict"        '^(-- ci-smp-gate exit status:|ci-smp-gate: (PASS|FAIL))' 1
section "row verdicts (enumerated)"  '^ *row PASS: |ROW MISSING OR RED' 5
section "per-label tallies, every category" '^ *== (default|ubsan)-smp[0-9]+: [0-9]+ PASS / ' 5
section "clean-boot assertion"       'CLEAN boots|only [0-9]+ of [0-9]+ boots CLEAN|NOT A CLEAN QUALIFICATION' 1
section "per-boot D7 witnesses"      'D7 ladder reached in|D7 TOTALS|D7 IS NOT CLEAN|COVERAGE NOT MET|REFUSING on' 1
# Stage 4's markers, taken from the runbook's own echo strings rather than
# guessed: the suite tally and the D7 verdict live in the BOOT log, and the run
# log carries only the lines stage 4 prints ABOUT them. Reaching for
# "tests: N/N" here would be the same wrong-oracle mistake stage 4 itself made.
section "witness records (stage 4)"  '^ *(RAN\+PASSED: burrow\.|NO PASS RECORD: burrow\.)' 4
section "suite tally and skips"      '^ *(tests: [0-9]+/[0-9]+|\[skip\] lines:)' 1
section "D7 axis (separate verdict)" '^-- D7 (axis|verdict code)|TOLERATED as the UNCHANGED known D7|FATAL: a red boot' 1

printf '\n== provenance blocks recorded ==\n'
np=$(grep -cE '^=== .* -- [0-9]{4}-[0-9]{2}-[0-9]{2}T' "$LOG" || true)
echo "   $np block(s) in this log; the hashes live in work/oct5-as-r9/provenance.log"

# The exit status of THIS script says whether the REPORT is complete, which is a
# different claim from whether the gate was green. State the gate's verdict in
# words so the two can never be read as one.
printf '\n'
if grep -q '^ci-smp-gate: PASS' "$LOG"; then
  echo "GATE VERDICT: PASS (0 corruption across all configs, per the gate's own line)"
elif grep -q '^ci-smp-gate: FAIL' "$LOG"; then
  echo "GATE VERDICT: FAIL -- read build/multiboot-fails/ before reporting anything else"
else
  echo "GATE VERDICT: ABSENT -- the gate never printed a verdict in this log"
fi
if [ "$missing" -gt 0 ]; then
  echo "gate-report: $missing section(s) ABSENT -- the REPORT is incomplete, whatever"
  echo "   the verdict line above says. Do not paste this as a gate result."
  exit 1
fi
echo "gate-report: every section present."
