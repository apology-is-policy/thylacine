#!/bin/sh
# Decide D7's outcome for a run, and report it on its OWN axis.
#
# ASK THE MECHANISM THAT OWNS THE VERDICT. The probe prints its own result
# ("joey: D7 overlapping login probe PASS" / "... FAILED") and narrates its
# progress, so that is the oracle. The earlier version of this script inferred
# the verdict from the provision-dek/install-dek trace instead, and the 10-07
# controlled run showed why that was wrong: with the pinned Stratum
# (stratum-astra 61dde37, per-connection session leases) the guest emits ZERO
# *-dek lines at all -- each connection proves UNWRAP against its own lease
# rather than installing a key. The whole dek vocabulary belongs to the OLD
# stratumd, so a control demanding those lines FAILS a cured run. The DEK trace
# is kept below as ATTRIBUTION evidence for a red, never as the verdict.
#
# exit 0  CURED  -- probe PASS, no extinction
#      20 UNCHANGED known red -- probe FAILED and the dek trace is byte-identical
#                               to the recorded 10-06 baseline
#      22 red, but NOT the known one -- probe FAILED with a different trace
#      21 CHANGED SHAPE -- probe PASS yet joey still extincted: a second cause
#       4 CONTROL FAILED -- no verdict, contradictory verdicts, or the overlap
#                           was never reached, so any reading would be vacuous
set -u
NEW="${1:-}"
BASE="${2:-work/oct5-as-r9/d7-baseline-red.trace}"
if [ -z "$NEW" ] || [ ! -f "$NEW" ]; then
  echo "usage: d7-compare.sh <boot-log> [baseline.trace]"; exit 64
fi
[ -f "$BASE" ] || { echo "REFUSING: baseline $BASE missing"; exit 64; }

n_pass=$(grep -c 'D7 overlapping login probe PASS' "$NEW" || true)
n_fail=$(grep -c 'D7 overlapping login probe FAILED' "$NEW" || true)
n_reach=$(grep -c 'D7 second same-user session ready' "$NEW" || true)
n_setup=$(grep -c 'D7 three distinct login sessions simultaneously ready' "$NEW" || true)
n_cycles=$(grep -c 'D7 twenty distinct login/logout cycles PASS' "$NEW" || true)
n_ext=$(grep -c 'EXTINCTION' "$NEW" || true)
ext_joey=$(grep -c 'EXTINCTION: joey' "$NEW" || true)

# Timestamps stripped: they differ by construction between runs.
T=$(grep -E 'evict-dek|provision-dek|install-dek' "$NEW" | awk '{$1=""; sub(/^ /,""); print}')
n_dek=$(printf '%s\n' "$T" | grep -c 'install-dek' || true)

echo "=== D7 verdict (its own axis; NOT the AS-R9 verdict) ============"
echo "new log          : $NEW"
echo "probe PASS lines : $n_pass"
echo "probe FAILED     : $n_fail"
echo "overlap reached  : $n_reach   (the 'second same-user session ready' line)"
echo "20-cycle PASS    : $n_cycles"
echo "EXTINCTION       : $n_ext (of which joey: $ext_joey)"
echo "install-dek lines: $n_dek   (ZERO is EXPECTED under the session-lease build"
echo "                   -- absence here is a vocabulary change, not a broken search)"
echo

# --- controls, before any verdict -------------------------------------------
if [ $((n_pass + n_fail)) -eq 0 ]; then
  echo "CONTROL FAILED: the probe printed NO verdict at all."
  echo "  It never ran, or never reached its conclusion. Any reading would be"
  echo "  vacuous -- this is neither cured nor reproduced."
  exit 4
fi
if [ "$n_pass" -gt 0 ] && [ "$n_fail" -gt 0 ]; then
  echo "CONTROL FAILED: the probe printed BOTH PASS and FAILED ($n_pass/$n_fail)."
  echo "  Contradictory verdicts; do not pick the convenient one."
  exit 4
fi
# THE ATTEMPT MARKER, NOT THE SUCCESS MARKER. A red run CANNOT print "second
# same-user session ready", because that overlapping session is exactly what
# fails -- so requiring it up front refused the known red (measured: the 10-06
# log reaches "three distinct login sessions simultaneously ready" and then
# FAILED). The universal precondition is that the probe set up its concurrency;
# the success line is checked below, where it is the PASS that could be vacuous.
if [ "$n_setup" -eq 0 ]; then
  echo "CONTROL FAILED: the probe never reported its three concurrent login"
  echo "  sessions ready, so it never set up the condition D7 tests. Vacuous."
  exit 4
fi
echo "controls: verdict present / not contradictory / concurrency set up -- OK"
echo

# --- verdict ----------------------------------------------------------------
if [ "$n_fail" -gt 0 ]; then
  echo "D7: RED -- the probe reports FAILED."
  printf '%s\n' "$T" | grep 'install-dek.*result=err' | sed 's/^/    /' || true
  if printf '%s\n' "$T" | diff -q "$BASE" - >/dev/null 2>&1; then
    echo "  DEK trace is BYTE-IDENTICAL to the 10-06 baseline: the SAME failure."
    echo "  With the Stratum pin honoured, that would mean the attribution is WRONG."
    exit 20
  fi
  echo "  DEK trace DIFFERS from the baseline -- a red, but not the known one:"
  printf '%s\n' "$T" | diff "$BASE" - | sed 's/^/    /' || true
  exit 22
fi
if [ "$ext_joey" -gt 0 ] || [ "$n_ext" -gt 0 ]; then
  echo "D7: CHANGED SHAPE -- the probe PASSED yet the boot still extincted."
  echo "  Do NOT read this as cured; a second cause is in play."
  grep 'EXTINCTION' "$NEW" | sed 's/^/    /'
  exit 21
fi
# A PASS specifically must show the OVERLAPPING session actually became ready,
# or the success is vacuous -- this is the half the attempt marker cannot cover.
if [ "$n_reach" -eq 0 ]; then
  echo "CONTROL FAILED: the probe reports PASS, but never reported the second"
  echo "  same-user session ready. A PASS without the overlap is vacuous."
  exit 4
fi
echo "D7: CURED on this input."
echo "  The probe reports PASS, the overlap became ready, and nothing extincted."
echo "  Consistent with the unequal-Stratum explanation. This says NOTHING about"
echo "  AS-R9 either way -- the SMP gate is reported separately."
exit 0
