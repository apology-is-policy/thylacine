#!/bin/sh
# Drive the REAL control and mutant oracles against constructed boot logs.
# Every log is built by MUTATING TODAY'S REAL ONE, so the surrounding context,
# the \r\n line endings and test.c's own emission shape are the guest's, not my
# idea of them -- which is precisely what the marker-only synthetic arms got
# wrong (astra, yip 0161 t63).
set -e
ROOT=/Users/northkillpd/projects/thylacine-corona
cd "$ROOT"
W=${ARMS_DIR:-${TMPDIR:-/tmp}/reap-leg-oracle-arms}
rm -rf "$W"; mkdir -p "$W"
S=work/oct5-as-r9/reap-leg-run.sh
# Overridable so the refusal below can be DRIVEN -- a refusal nothing exercises
# is a refusal I cannot claim works, and the default paths exist on this host.
REAL_CONTROL=${REAL_CONTROL:-work/oct5-as-r9/reap-leg-20261007T190621Z/control-boot.log}
REAL_MUTANT=${REAL_MUTANT:-work/oct5-as-r9/reap-leg-20261007T190621Z/mutant-boot.log}
# THE BASE LOGS ARE RUN ARTIFACTS, not tracked files -- they are the 19:00Z
# control and the earlier unattributable mutant, kept under work/. Every arm is
# built by editing them, so their absence is a refusal with a name on it rather
# than a stack trace: rebuild them by running the leg, or point these at another
# run's logs.
for _f in "$REAL_CONTROL" "$REAL_MUTANT"; do
  [ -f "$_f" ] || { echo "REFUSING: the base boot log is missing: $_f"; exit 3; }
done

# ---- EXTRACT the oracle from the live file, with a denominator control. ----
a=$(/usr/bin/grep -n '^LEG=loom\.private_owner_lifecycle$' "$S" | cut -d: -f1)
b=$(awk -v a="$a" 'NR>a && /^check_mutant\(\) \{/ {f=1} f && /^\}$/ {print NR; exit}' "$S")
sed -n "${a},${b}p" "$S" > "$W/oracle.sh"
echo "-- extracted the oracle: lines $a-$b, $(wc -l < "$W/oracle.sh" | tr -d ' ') lines"
for need in 'leg_block() {' 'leg_verdict() {' 'last_announced() {' \
            'check_control() {' 'check_mutant() {' \
            'after-check-failure:' 'normal-fallthrough' 'DISCRIMINATED'; do
  /usr/bin/grep -qF "$need" "$W/oracle.sh" || { echo "REFUSING: extraction lost: $need"; exit 1; }
done
echo "-- extraction carries both oracles and all three discriminating strings"

# ---- BUILD the arms by editing the real log at the leg's own line. ----
python3 - "$W" "$REAL_CONTROL" "$REAL_MUTANT" <<'PY'
import sys, os
W, real_control, real_mutant = sys.argv[1], sys.argv[2], sys.argv[3]
L = open(real_control, newline='').read().split('\n')   # keeps the \r on each line
leg = [i for i, s in enumerate(L) if 'loom.private_owner_lifecycle' in s]
assert len(leg) == 1, "the real control log names the leg %d times" % len(leg)
i = leg[0]
assert L[i] == '    [test] loom.private_owner_lifecycle ... PASS\r', repr(L[i])
ANN = '    [test] loom.private_owner_lifecycle ... '
ARR = '[lp-mark] unpinned-reap-owner-drop'
NORM = '[lp-mark] cleanup-owner-drop normal-fallthrough'
def FAILED(msg): return '[lp-mark] cleanup-owner-drop after-check-failure: ' + msg
EXT = ['EXTINCTION: AddrSpace final lifetime drop with private rings',
       'HALLS: --- Halls of Extinction (crash dump) ---',
       'HALLS: cpu 0  source: bare extinction (no exception frame)']
def write(name, block, truncate=False, append_after=None):
    out = L[:i] + [s + '\r' for s in block]
    if not truncate:
        rest = L[i+1:]
        if append_after is not None:
            rest = rest[:append_after] + [s + '\r' for s in EXT] + rest[append_after:]
        out = out + rest
    open(os.path.join(W, name), 'w', newline='').write('\n'.join(out))

# A: healthy instrumented control -- arrival, NORMAL fallthrough, then PASS.
write('A-control-success.log', [ANN + ARR, NORM, 'PASS'])
# B: a check BEFORE the target drop fails -- no arrival marker at all.
write('B-early-check-failure.log',
      [ANN + FAILED('no retirement in flight at the snapshot'),
       'FAIL: no retirement in flight at the snapshot'])
# C: a check AFTER arrival fails -- arrival present AND the diagnostic variant.
write('C-late-check-failure.log',
      [ANN + ARR, FAILED('the reaped creator\'s ring retires exactly once'),
       'FAIL: the reaped creator\'s ring retires exactly once'])
# D: the lethal mutant -- arrival, then the named extinction, boot dead.
write('D-mutant-target.log', [ANN + ARR] + EXT, truncate=True)
# E: arrival, a NORMAL completion, and an extinction LATER in the boot.
write('E-arrival-then-later-extinction.log', [ANN + ARR, NORM, 'PASS'], append_after=6)
# F: arrival and a PASS with NO cleanup marker -- isolates the verdict check so
#    it cannot be dead code, which is how the first version's completion check
#    ended up unable to fire at all.
write('F-arrival-pass-no-cleanup.log', [ANN + ARR, 'PASS'])
# G: no arrival at all, yet PASS -- the leg passed without exercising the drop.
write('G-no-arrival-but-pass.log', [ANN + NORM, 'PASS'])
# H: PASS with arrival but the normal marker gone -- the emission moved, so the
#    after-check-failure discrimination is no longer being made.
write('H-pass-without-normal-marker.log', [ANN + ARR, 'PASS'])
# K/L: the arms arm F FAILED TO BE. F has no extinction at all, so check_mutant
#      refuses at its first gate and never reaches the verdict check -- leaving
#      that check unexercised, which is precisely how the first version's
#      completion check came to be code that could not fire. These reach it:
#      arrival, NO cleanup marker, a real verdict, and the extinction later.
write('K-arrival-pass-then-extinction.log', [ANN + ARR, 'PASS'], append_after=6)
write('L-arrival-fail-then-extinction.log',
      [ANN + ARR, 'FAIL: the reaped creator\'s ring retires exactly once'], append_after=6)
# M: a FAIL verdict with no cleanup marker, so the control oracle reaches its
#    own verdict branch instead of refusing at the diagnostic gate.
write('M-arrival-fail-no-cleanup.log',
      [ANN + ARR, 'FAIL: the reaped creator\'s ring retires exactly once'])
# N/O: the last two check_mutant gates that nothing above reaches. A gate no
#      arm exercises is a gate I cannot claim works, and this oracle has already
#      shipped one check that could not fire.
# N: the named extinction AND a second, different one -- the prediction would
#    otherwise be right by accident.
write('N-two-extinctions.log',
      [ANN + ARR, EXT[0], 'EXTINCTION: AddrSpace final lifetime drop with live owners'],
      truncate=True)
# O: announced, named extinction, but NO arrival marker -- it died inside the
#    leg somewhere before the drop under test.
write('O-announced-no-arrival.log', [ANN] + EXT, truncate=True)
print('-- built the arms from %s (leg at line %d)' % (real_control, i + 1))
PY
# I: TODAY'S REAL failed mutant log, unmodified -- the leg is never announced and
#    the first oracle called it DISCRIMINATED.
cp "$REAL_MUTANT" "$W/I-real-unattributable.log"
cp "$REAL_CONTROL" "$W/J-real-uninstrumented.log"

# ---- DRIVE both oracles on every arm. ----
# set -e OFF for the drives: a refusing arm is the EXPECTED result on most of
# them, and under set -e the failing command substitution killed the harness
# after the first arm -- silently, which is the worst way for a harness to stop.
set +e
run() { # run <oracle> <log> <expected rc> <required substring>
  _o=$1; _lg=$2; _want=$3; _sub=$4
  _got=$( cd "$W"; OUT=. sh -c ". ./oracle.sh; $_o '$_lg'" 2>&1; echo "rc=$?" )
  _rc=$(echo "$_got" | tail -1 | sed 's/rc=//')
  if [ "$_rc" != "$_want" ]; then
    echo "FAIL  $_o($_lg): rc=$_rc, wanted $_want"; echo "$_got" | sed 's/^/        /'; FAILED=1; return
  fi
  if ! echo "$_got" | /usr/bin/grep -qF "$_sub"; then
    echo "FAIL  $_o($_lg): rc=$_rc correct but the message lacks: $_sub"
    echo "$_got" | sed 's/^/        /'; FAILED=1; return
  fi
  printf 'ok    %-13s %-36s rc=%s  %s\n' "$_o" "$_lg" "$_rc" "$_sub"
}
FAILED=0
echo
echo "=== the CONTROL oracle ==="
run check_control A-control-success.log               0 "verdict PASS in the leg's own block"
run check_control B-early-check-failure.log           1 "A CHECK INSIDE THE LEG FAILED"
run check_control C-late-check-failure.log            1 "A CHECK INSIDE THE LEG FAILED"
run check_control D-mutant-target.log                 1 "THE LEG DID NOT PASS"
run check_control G-no-arrival-but-pass.log           1 "never reached its owner drop"
run check_control H-pass-without-normal-marker.log    1 "normal-fallthrough marker is absent"
run check_control M-arrival-fail-no-cleanup.log       1 "the verdict in its own block is FAIL"
run check_control I-real-unattributable.log           1 "NEVER ANNOUNCED"
run check_control J-real-uninstrumented.log           1 "never reached its owner drop"
echo
echo "=== the MUTANT oracle ==="
run check_mutant  D-mutant-target.log                 0 "DISCRIMINATED"
run check_mutant  A-control-success.log               2 "DID NOT PRODUCE THE PREDICTED FAILURE"
run check_mutant  B-early-check-failure.log           2 "DID NOT PRODUCE THE PREDICTED FAILURE"
run check_mutant  E-arrival-then-later-extinction.log 2 "survived the target drop"
run check_mutant  F-arrival-pass-no-cleanup.log       2 "DID NOT PRODUCE THE PREDICTED FAILURE"
run check_mutant  K-arrival-pass-then-extinction.log   2 "THE LEG COMPLETED (verdict PASS"
run check_mutant  L-arrival-fail-then-extinction.log   2 "THE LEG COMPLETED (verdict FAIL"
run check_mutant  N-two-extinctions.log                2 "OTHER extinction(s) fired too"
run check_mutant  O-announced-no-arrival.log           2 "NEVER REACHED ITS OWNER DROP"
run check_mutant  I-real-unattributable.log           2 "NOT ATTRIBUTABLE TO THIS LEG"
echo
[ "$FAILED" = 0 ] && echo "ALL ARMS BEHAVED AS SPECIFIED" || { echo "SOME ARMS FAILED"; exit 1; }
