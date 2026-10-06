#!/bin/sh
# Compare a confirmation run's DEK trace against the recorded D7 red baseline.
#
# D7's verdict is reported SEPARATELY from the AS-R9 gate result, by agreement
# with astra (yip 0161): the controlled rebuild re-equalises the Stratum input,
# so a change here is evidence about THAT input, not about the charge-settlement
# repair.
#
# The absence of the eaccess line does NOT by itself mean cured: a probe that
# never reaches the second login satisfies that absence trivially. So the
# overlap must be shown to have HAPPENED before its success counts for anything.
set -u
NEW="${1:-}"
BASE="${2:-work/oct5-as-r9/d7-baseline-red.trace}"
if [ -z "$NEW" ] || [ ! -f "$NEW" ]; then
  echo "usage: d7-compare.sh <new-boot-log> [baseline.trace]"; exit 64
fi
[ -f "$BASE" ] || { echo "REFUSING: baseline $BASE missing"; exit 64; }

# Timestamps are stripped: they differ by construction between runs.
T=$(grep -E 'evict-dek|provision-dek|install-dek' "$NEW" | awk '{$1=""; sub(/^ /,""); print}')

cnt() { printf '%s\n' "$T" | grep -c "$1" || true; }
n_install=$(cnt 'install-dek')
n_ds2=$(cnt 'install-dek.*dataset=2')
n_err=$(cnt 'install-dek.*result=err')
n_joey=$(grep -c 'joey' "$NEW" || true)
n_ext=$(grep -c 'EXTINCTION' "$NEW" || true)
ext_joey=$(grep -c 'EXTINCTION: joey' "$NEW" || true)

echo "=== D7 trace comparison ==================================="
echo "new log        : $NEW"
echo "baseline       : $BASE ($(wc -l < "$BASE" | tr -d ' ') lines)"
echo "install-dek    : $n_install   (baseline 4)"
echo "  of dataset=2 : $n_ds2   (baseline 2 -- the OVERLAP)"
echo "  result=err   : $n_err   (baseline 1)"
echo "joey mentions  : $n_joey"
echo "EXTINCTION     : $n_ext (of which joey: $ext_joey)"
echo

# --- controls, before any verdict -------------------------------------------
if [ "$n_install" -eq 0 ]; then
  echo "CONTROL FAILED (C1): zero install-dek lines in the new log."
  echo "  The SEARCH is broken, or stratumd never provisioned. NOT a pass."
  exit 4
fi
if [ "$n_ds2" -lt 2 ]; then
  echo "CONTROL FAILED (C2): dataset=2 installed $n_ds2 time(s), need 2."
  echo "  The overlapping login never happened, so the absence of eaccess is"
  echo "  VACUOUS. D7 is neither cured nor reproduced -- the probe stopped short."
  exit 4
fi
if [ "$n_joey" -eq 0 ]; then
  echo "CONTROL FAILED (C3): joey never appears in the log."
  exit 4
fi
echo "controls: C1 search non-empty OK / C2 overlap reached OK / C3 probe ran OK"
echo

# --- verdict ----------------------------------------------------------------
if [ "$n_err" -eq 0 ] && [ "$ext_joey" -eq 0 ]; then
  echo "D7: CURED on this input."
  echo "  The overlapping dataset=2 install SUCCEEDED and joey did not extinct."
  echo "  Consistent with the unequal-Stratum explanation. This says nothing"
  echo "  about AS-R9 either way -- report the gate separately."
  exit 0
fi
if [ "$n_err" -gt 0 ]; then
  echo "D7: STILL RED -- install-dek refused $n_err time(s)."
  printf '%s\n' "$T" | grep 'install-dek.*result=err' | sed 's/^/    /'
  echo
  if printf '%s\n' "$T" | diff -q "$BASE" - >/dev/null 2>&1; then
    echo "  Trace is BYTE-IDENTICAL to the red baseline: the SAME failure, so the"
    echo "  Stratum pin did not change the outcome and the attribution is WRONG."
    echo "  This, and ONLY this, is the UNCHANGED KNOWN RED (20)."
    exit 20
  fi
  echo "  Trace DIFFERS from the baseline -- an install-dek refusal, but NOT the"
  echo "  known one. Do not treat it as the understood failure (22):"
  printf '%s\n' "$T" | diff "$BASE" - | sed 's/^/    /'
  exit 22
fi
echo "D7: CHANGED SHAPE -- no install-dek error, but joey still extincted."
echo "  Do NOT read this as cured; a second cause is in play."
grep 'EXTINCTION' "$NEW" | sed 's/^/    /'
exit 21
