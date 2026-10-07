#!/bin/bash
# Controls for tools/thyla-wake.sh. T0-T4 read the real yip (`watch pi` only reads; no lease is
# taken); T5-T9 run against fakeyip.sh, so every lease state is canned and no real lease moves.
# Every pane typed into is a throwaway one in the tmux session thyla-wake-test, running fakebox.py.
# Usage: tools/thyla-wake-test/run.sh   (inside tmux, from a checkout on a yip line)
set -u
T=$(cd "$(dirname "$0")" && pwd)
W=$T/../thyla-wake.sh
[ -n "${TMUX:-}" ] || { echo "run.sh: run me inside tmux" >&2; exit 2; }
tmux has-session -t thyla-wake-test 2>/dev/null && { echo "run.sh: a tmux session thyla-wake-test already exists" >&2; exit 2; }
WORK=$(mktemp -d "${TMPDIR:-/tmp}/thyla-wake-test.XXXXXX") || exit 2
export THYLA_WAKE_DIR=$WORK/wd
RUN=t
ok=0; bad=0
pass() { echo "PASS $*"; ok=$((ok+1)); }
fail() { echo "FAIL $*"; bad=$((bad+1)); }
pane() {
    if tmux has-session -t thyla-wake-test 2>/dev/null; then
        tmux new-window -d -P -F '#{pane_id}' -t thyla-wake-test -c "$WORK" "$1"
    else
        tmux new-session -d -P -F '#{pane_id}' -s thyla-wake-test -x 96 -y 24 -c "$WORK" "$1"
    fi
}
boxof() { "$W" probe "$1" | sed -n 's/^box *: //p'; }
lines() { [ -s "$1" ] && wc -l < "$1" | tr -d ' ' || echo 0; }

echo "== T0 syntax + probes"
bash -n "$W" && pass "T0 bash -n" || fail "T0 bash -n"
echo "   (info) this pane's box reads: $(boxof "$TMUX_PANE") -- empty in an idle agent pane, none in a shell"
"$W" probe %99999 2>&1 | grep -q 'program : <no such pane>' && pass "T0 a missing pane is named" || fail "T0 missing pane"
b=$(boxof %99999); [ "$b" = none ] && pass "T0 a missing pane's box is none" || fail "T0 missing pane box '$b'"

echo "== T1 a shell pane, and a missing pane, are refused"
p=$(pane "exec /bin/zsh -f"); sleep 1
out=$(TMUX_PANE=$p "$W" watch pi 2>&1); rc=$?
[ $rc -ne 0 ] && grep -q 'a shell' <<< "$out" && pass "T1 shell refused" || fail "T1 rc=$rc out=$out"
out=$(TMUX_PANE=%99999 "$W" watch pi 2>&1); rc=$?
[ $rc -ne 0 ] && grep -q 'does not exist' <<< "$out" && pass "T1 missing pane refused" || fail "T1 missing rc=$rc out=$out"

echo "== T2 watch pi delivers into an empty box"
o=$WORK/o2; : > "$o"; p=$(pane "python3 -I $T/fakebox.py $o empty"); sleep 1
b=$(boxof "$p"); [ "$b" = empty ] && pass "T2 the fake box reads empty" || fail "T2 fake box reads '$b'"
TMUX_PANE=$p "$W" watch pi --say "T2 control" >/dev/null || fail "T2 arm"
sleep 6
grep -q '^\[thyla-wake\] pi changed at [0-9][0-9]:[0-9][0-9]Z: pi  *FREE -- T2 control$' "$o" && [ "$(lines "$o")" = 1 ] \
    && pass "T2 one wake line, verbatim" || fail "T2 got: $(cat "$o")"

echo "== T3 a half-typed line is waited out, never glued to"
o=$WORK/o3; : > "$o"; p=$(pane "python3 -I $T/fakebox.py $o draft"); sleep 1
b=$(boxof "$p"); [ "$b" = typed ] && pass "T3 the draft box reads typed" || fail "T3 draft box reads '$b'"
TMUX_PANE=$p "$W" watch pi --say "T3 control" >/dev/null || fail "T3 arm"
sleep 4
[ ! -s "$o" ] && pass "T3 nothing typed while the draft stands" || fail "T3 typed into a draft: $(cat "$o")"
sleep 14
grep -q '^\[thyla-wake\] pi changed .* -- T3 control$' "$o" && [ "$(lines "$o")" = 1 ] && ! grep -q hello "$o" \
    && pass "T3 typed once the box emptied" || fail "T3 got: $(cat "$o")"
grep -q "	waiting	input box: typed" "$THYLA_WAKE_DIR/log.tsv" && pass "T3 the wait is logged" || fail "T3 no waiting line"

echo "== T4 a permission dialog is never typed into"
o=$WORK/o4; : > "$o"; p=$(pane "python3 -I $T/fakebox.py $o dialog"); sleep 1
b=$(boxof "$p"); [ "$b" = none ] && pass "T4 the dialog reads none" || fail "T4 dialog reads '$b'"
THYLA_WAKE_DELIVER_BOUND=8 TMUX_PANE=$p "$W" watch pi --say "T4 control" >/dev/null || fail "T4 arm"
sleep 14
[ ! -s "$o" ] && pass "T4 nothing typed into the dialog" || fail "T4 typed into the dialog: $(cat "$o")"
grep -q "	undelivered	box never empty" "$THYLA_WAKE_DIR/log.tsv" && pass "T4 gave up, logged" || fail "T4 no undelivered line"

# ---- fake yip from here on
export THYLA_WAKE_YIP=$T/fakeyip.sh FAKEYIP_STATE=$WORK/fy THYLA_WAKE_POLL=1
mkdir -p "$FAKEYIP_STATE"; F=$FAKEYIP_STATE
setres() { printf 'mac   %s\npi    FREE\n' "$1" > "$F/resources"; }

echo "== T5 watch: a peer's lease never fires; this agent's own ('HELD by you') does"
setres "HELD by main for 1m, 1.0h left"
o=$WORK/o5; : > "$o"; p=$(pane "python3 -I $T/fakebox.py $o empty"); sleep 1
TMUX_PANE=$p "$W" watch mac --say "T5" >/dev/null || fail "T5 arm"
sleep 4
[ ! -s "$o" ] && pass "T5 silent while main holds" || fail "T5 fired on main's lease: $(cat "$o")"
setres "HELD by you for 0s, 6.0h left"
sleep 5
grep -q '^\[thyla-wake\] mac changed at .*: mac   HELD by you for 0s, 6.0h left -- T5$' "$o" && [ "$(lines "$o")" = 1 ] \
    && pass "T5 fired on its own lease" || fail "T5 got: $(cat "$o")"

echo "== T6 hold: granted -> the wake says so, and nothing is released"
: > "$F/calls"; setres "HELD by you for 0s, 6.0h left"; echo "HELD: mac is yours for 6.0h." > "$F/hold.out"; echo 0 > "$F/hold.rc"; echo 1 > "$F/hold.sleep"
o=$WORK/o6; : > "$o"; p=$(pane "python3 -I $T/fakebox.py $o empty"); sleep 1
TMUX_PANE=$p "$W" hold mac "T6 reason with spaces" --for 6h --wait 4h --say "T6" >/dev/null || fail "T6 arm"
sleep 6
grep -q '^\[thyla-wake\] mac is yours: HELD by aux since [0-9:]*Z (yip hold rc=0)\. .* -- T6$' "$o" && [ "$(lines "$o")" = 1 ] \
    && pass "T6 granted wake" || fail "T6 got: $(cat "$o")"
grep -qx 'hold mac T6 reason with spaces --for 6h --wait 4h' "$F/calls" && ! grep -q release "$F/calls" \
    && pass "T6 one hold with the reason intact, no release" || fail "T6 calls: $(cat "$F/calls")"

echo "== T7 hold: not granted -> the wake says WITHOUT, nothing is released"
: > "$F/calls"; setres "HELD by main for 2m, 1.0h left"; echo "WAITING: queue 1, timed out" > "$F/hold.out"; echo 3 > "$F/hold.rc"
o=$WORK/o7; : > "$o"; p=$(pane "python3 -I $T/fakebox.py $o empty"); sleep 1
TMUX_PANE=$p "$W" hold mac "T7" --say "T7" >/dev/null || fail "T7 arm"
sleep 6
grep -q '^\[thyla-wake\] mac hold ended WITHOUT the lease at .*rc=3: WAITING: queue 1, timed out).*mac   HELD by main.* -- T7$' "$o" \
    && pass "T7 not-granted wake" || fail "T7 got: $(cat "$o")"
! grep -q release "$F/calls" && pass "T7 no release" || fail "T7 released"

echo "== T8 hold granted but the agent is gone -> the lease is released"
: > "$F/calls"; setres "HELD by you for 0s, 6.0h left"; echo "HELD" > "$F/hold.out"; echo 0 > "$F/hold.rc"
o=$WORK/o8; : > "$o"; p=$(pane "python3 -I $T/fakebox.py $o dialog 5"); sleep 1
TMUX_PANE=$p "$W" hold mac "T8" >/dev/null || fail "T8 arm"
sleep 10
[ ! -s "$o" ] && pass "T8 nothing typed" || fail "T8 typed: $(cat "$o")"
grep -qx 'release mac' "$F/calls" && pass "T8 released" || fail "T8 calls: $(cat "$F/calls")"
grep -q "	agent-gone	pane $p no longer exists" "$THYLA_WAKE_DIR/log.tsv" && pass "T8 agent-gone logged" || fail "T8 no agent-gone line"

echo "== T9 cancel stops a waiting hold and a watch; neither types"
: > "$F/calls"; setres "HELD by main for 2m, 1.0h left"; echo 60 > "$F/hold.sleep"
o=$WORK/o9; : > "$o"; p=$(pane "python3 -I $T/fakebox.py $o empty"); sleep 1
TMUX_PANE=$p "$W" hold mac "T9" >/dev/null || fail "T9 arm hold"
TMUX_PANE=$p "$W" watch mac >/dev/null || fail "T9 arm watch"
sleep 2
n=$("$W" status | grep -c '^  armed ')
[ "$n" = 2 ] && pass "T9 status shows 2 armed" || fail "T9 status shows $n"
"$W" cancel
sleep 2
[ "$("$W" status | grep -c '^  armed ')" = 0 ] && pass "T9 none armed after cancel" || fail "T9 still armed"
[ "$(grep -c '	cancelled	' "$THYLA_WAKE_DIR/log.tsv")" = 2 ] && pass "T9 two cancelled lines" || fail "T9 cancelled lines: $(grep -c cancelled "$THYLA_WAKE_DIR/log.tsv")"
[ ! -s "$o" ] && pass "T9 nothing typed" || fail "T9 typed: $(cat "$o")"
pgrep -f "fakeyip.sh hold mac T9" >/dev/null && fail "T9 the hold child outlived cancel" || pass "T9 hold child gone"

echo "== log"; cat "$THYLA_WAKE_DIR/log.tsv" | cut -c1-200
tmux kill-session -t thyla-wake-test 2>/dev/null
echo "work dir: $WORK"
echo "RESULT ok=$ok bad=$bad"
