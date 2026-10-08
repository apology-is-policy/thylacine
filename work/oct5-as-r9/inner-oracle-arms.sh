#!/bin/sh
# Drive inner-leg-run.sh's REAL oracles against constructed boot logs, every
# one built by editing a REAL guest log -- the healthy control from the reap run
# and a retained real FAILING run of this same fixture -- so the \r\n endings,
# the [runnable-dump] lines a FAIL emits and the suite-failed tail are the
# guest's own, not my idea of them. Each arm asserts its rc AND which gate's
# message fired: an arm refused at an earlier gate covers nothing past it.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
RUNNER=${RUNNER:-work/oct5-as-r9/inner-leg-run.sh}
REAL_CONTROL=${REAL_CONTROL:-work/oct5-as-r9/reap-leg-20261007T204646Z/control-boot.log}
REAL_FAIL=${REAL_FAIL:-work/oct5-as-r9/private-owner-logs/red-legs/20261007T103115Z/uncond-refund-serial.log}
REAL_DIED_EARLY=${REAL_DIED_EARLY:-work/oct5-as-r9/reap-leg-20261007T190621Z/mutant-boot.log}
REAL_DIED_INSIDE=${REAL_DIED_INSIDE:-work/oct5-as-r9/reap-leg-20261007T204646Z/mutant-boot.log}
W=${ARMS_DIR:-${TMPDIR:-/tmp}/inner-leg-oracle-arms}
TRANSCRIPT=${TRANSCRIPT:-work/oct5-as-r9/inner-oracle-arms-transcript.txt}
for f in "$RUNNER" "$REAL_CONTROL" "$REAL_FAIL" "$REAL_DIED_EARLY" "$REAL_DIED_INSIDE"; do
  [ -f "$f" ] || { echo "REFUSING: missing input $f"; exit 1; }
done
rm -rf "$W"; mkdir -p "$W"

# ---- EXTRACT the oracle from the live runner, with a denominator control. ----
w=$(/usr/bin/grep -n "^WANT_M1='" "$RUNNER" | cut -d: -f1)
a=$(/usr/bin/grep -n '^LEG=loom\.private_owner_lifecycle$' "$RUNNER" | cut -d: -f1)
b=$(awk -v a="$a" 'NR>a && /^check_mutant\(\) \{/ {f=1} f && /^\}$/ {print NR; exit}' "$RUNNER")
for n in "$w" "$a" "$b"; do
  case $n in ''|*[!0-9]*) echo "REFUSING: an extraction anchor is not one line number: '$n'"; exit 1;; esac
done
{ sed -n "${w},$((w + 1))p" "$RUNNER"; echo 'WANT=$WANT_M1'; echo 'MUT_LABEL=M1'; sed -n "${a},${b}p" "$RUNNER"; } > "$W/oracle.sh"
for need in "WANT_M1='the inner ring failure frees the unpublished Loom'" \
            "WANT_M2='a watched large free fires the watch'" \
            'leg_block() {' 'leg_verdict() {' 'last_announced() {' 'all_fails() {' \
            'check_control() {' 'check_mutant() {' 'after-check-failure:' \
            'normal-fallthrough' 'kernel test suite failed' 'DISCRIMINATED'; do
  /usr/bin/grep -qF "$need" "$W/oracle.sh" || { echo "REFUSING: extraction lost: $need"; exit 1; }
done
EXPECT_TESTS=$(/usr/bin/grep -c -E '^[[:space:]]*\{[[:space:]]*"[^"]+"' kernel/test/test.c)
echo "-- extracted the oracle: WANT at $w, lines $a-$b; EXPECT_TESTS derived = $EXPECT_TESTS"

# ---- BUILD the arms. ----
python3 - "$W" "$REAL_CONTROL" "$REAL_FAIL" "$EXPECT_TESTS" <<'PY'
import sys, os
W, rc, rf, n = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4])
WANT = 'the inner ring failure frees the unpublished Loom'
WANT2 = 'a watched large free fires the watch'
ANN = '    [test] loom.private_owner_lifecycle ... '
MARK = '[lp-mark] cleanup-owner-drop after-check-failure: '
def lines(p): return open(p, newline='').read().split('\n')   # keeps each \r
def put(name, L): open(os.path.join(W, name), 'w', newline='').write('\n'.join(L))
C = lines(rc); F = lines(rf)
ci = [i for i, s in enumerate(C) if 'loom.private_owner_lifecycle' in s]
fi = [i for i, s in enumerate(F) if 'loom.private_owner_lifecycle' in s]
assert len(ci) == 1 and len(fi) == 1, (ci, fi)
ci, fi = ci[0], fi[0]
# The real FAIL block is three lines: announcement+runnable-dump, cpu, FAIL.
assert F[fi+2].startswith('FAIL: a nonfinal ring drop refunds the metadata only'), repr(F[fi+2])
assert F[fi+3].startswith('    [test] '), repr(F[fi+3])
cpu = F[fi+1]
def failblock(marker_msg, verdict_msg, marker=True):
    first = ANN + (MARK + marker_msg if marker else '')
    return [first + '\r', '  [runnable-dump ' + verdict_msg + ']\r', cpu, 'FAIL: ' + verdict_msg + '\r']
def with_block(blk, L=F, i=fi): return L[:i] + blk + L[i+3:]
tally_fail = [j for j, s in enumerate(F) if s.startswith('  tests: ')]
assert len(tally_fail) == 1 and F[tally_fail[0]] == '  tests: %d/%d FAIL\r' % (n-1, n), repr(F[tally_fail[0]])
D = with_block(failblock(WANT, WANT))
put('D-predicted-mutant.log', D)
put('D2-predicted-M2.log', with_block(failblock(WANT2, WANT2)))
put('B-marker-and-verdict-disagree.log', with_block(failblock('the inner ring failure returns the charge', WANT)))
put('C-no-cleanup-marker.log', with_block(failblock(WANT, WANT, marker=False)))
put('V-different-assertion.log', with_block(failblock('the inner ring failure returns the charge',
                                                      'the inner ring failure returns the charge')))
# E: a SECOND test fails too -- the next test's PASS becomes a FAIL, tally adjusted.
E = list(D); k = fi + 4
assert E[k].endswith(' ... PASS\r'), repr(E[k])
E[k] = E[k][:-len('PASS\r')] + 'FAIL: some other test\r'
t = [j for j, s in enumerate(E) if s.startswith('  tests: ')][0]; E[t] = '  tests: %d/%d FAIL\r' % (n-2, n)
put('E-second-failing-test.log', E)
T = list(D); t = [j for j, s in enumerate(T) if s.startswith('  tests: ')][0]
T[t] = '  tests: %d/%d FAIL\r' % (n-1, n+1); put('T-tally-total-wrong.log', T)
X = list(D); x = [j for j, s in enumerate(X) if s.startswith('EXTINCTION: kernel test suite failed')]
assert len(x) == 1
X.insert(x[0], 'EXTINCTION: AddrSpace final lifetime drop with private rings\r'); put('X-extra-extinction.log', X)
Z = [s for s in D if not s.startswith('EXTINCTION: ')]; put('Z-no-extinction.log', Z)
# Control-side arms, from the real healthy control.
NORM = [j for j in range(ci, ci+4) if 'cleanup-owner-drop normal-fallthrough' in C[j]]
assert len(NORM) == 1, NORM
put('N-pass-without-normal-marker.log', C[:NORM[0]] + C[NORM[0]+1:])
print('-- built arms from %s (leg at %d) and %s (leg at %d)' % (rc, ci+1, rf, fi+1))
PY
cp "$REAL_CONTROL" "$W/R-real-control.log"
cp "$REAL_FAIL" "$W/S-real-fail-other-assertion.log"
cp "$REAL_DIED_EARLY" "$W/I-real-never-announced.log"
cp "$REAL_DIED_INSIDE" "$W/J-real-died-inside-leg.log"

# ---- DRIVE both oracles on every arm. ----
set +e
FAILED=0
run() { # run <oracle> <log> <expected rc> <required substring> [WANT override]
  _o=$1; _lg=$2; _want=$3; _sub=$4
  if [ $# -ge 5 ]; then _wv="WANT='$5'"; else _wv=':'; fi
  _got=$( cd "$W"; OUT=. EXPECT_TESTS=$EXPECT_TESTS sh -c ". ./oracle.sh; $_wv; $_o '$_lg'" 2>&1; echo "rc=$?" )
  _rc=$(echo "$_got" | tail -1 | sed 's/rc=//')
  if [ "$_rc" != "$_want" ]; then
    echo "FAIL  $_o($_lg): rc=$_rc, wanted $_want"; echo "$_got" | sed 's/^/        /'; FAILED=1; return
  fi
  if ! echo "$_got" | /usr/bin/grep -qF "$_sub"; then
    echo "FAIL  $_o($_lg): rc correct but the message lacks: $_sub"
    echo "$_got" | sed 's/^/        /'; FAILED=1; return
  fi
  printf 'ok    %-13s %-36s rc=%s  %s\n' "$_o" "$_lg" "$_rc" "$_sub"
}
{
echo "inner-oracle-arms: runner $(shasum -a 256 "$RUNNER" | cut -c1-16), $(git rev-parse --short HEAD), $(date -u '+%Y-%m-%dT%H:%M:%SZ')"
echo
echo "=== the CONTROL oracle ==="
run check_control R-real-control.log                0 "verdict PASS in its own block"
run check_control D-predicted-mutant.log            1 "A CHECK INSIDE THE LEG FAILED"
run check_control S-real-fail-other-assertion.log   1 "THE LEG DID NOT PASS"
run check_control N-pass-without-normal-marker.log  1 "normal-fallthrough marker is absent"
run check_control I-real-never-announced.log        1 "NEVER ANNOUNCED"
echo
echo "=== the MUTANT oracle ==="
run check_mutant  D-predicted-mutant.log            0 "DISCRIMINATED"
run check_mutant  D2-predicted-M2.log               0 "DISCRIMINATED" "a watched large free fires the watch"
run check_mutant  D-predicted-mutant.log            2 "NOT AT THE PREDICTED ASSERTION" "a watched large free fires the watch"
run check_mutant  D2-predicted-M2.log               2 "NOT AT THE PREDICTED ASSERTION"
run check_mutant  D-predicted-mutant.log            2 "predicted assertion message is empty" ""
run check_mutant  I-real-never-announced.log        2 "NOT ATTRIBUTABLE TO THIS LEG"
run check_mutant  R-real-control.log                2 "the leg PASSED"
run check_mutant  J-real-died-inside-leg.log        2 "THE LEG HAS NO VERDICT"
run check_mutant  S-real-fail-other-assertion.log   2 "NOT AT THE PREDICTED ASSERTION"
run check_mutant  V-different-assertion.log         2 "NOT AT THE PREDICTED ASSERTION"
run check_mutant  B-marker-and-verdict-disagree.log 2 "The two attributions disagree"
run check_mutant  C-no-cleanup-marker.log           2 "The two attributions disagree"
run check_mutant  E-second-failing-test.log         2 "FAIL verdicts in the boot, not 1"
run check_mutant  T-tally-total-wrong.log           2 "THE TALLY DISAGREES"
run check_mutant  X-extra-extinction.log            2 "NOT EXACTLY THE SUITE'S CONSEQUENCE"
run check_mutant  Z-no-extinction.log               2 "NOT EXACTLY THE SUITE'S CONSEQUENCE"
echo
for f in "$W"/*.log; do printf '%s  %s\n' "$(shasum -a 256 "$f" | cut -c1-16)" "$(basename "$f")"; done
echo
[ "$FAILED" = 0 ] && echo "ALL ARMS BEHAVED AS SPECIFIED" || echo "SOME ARMS FAILED"
} | tee "$TRANSCRIPT"
/usr/bin/grep -q '^ALL ARMS BEHAVED AS SPECIFIED$' "$TRANSCRIPT"
