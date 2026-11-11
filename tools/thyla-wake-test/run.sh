#!/bin/bash
# Controls for tools/thyla-wake.sh. T0-T4 read the real yip (`watch pi` only reads; no lease is
# taken); T5-T19 run against fakeyip.sh, so every lease state is canned, no real lease moves and
# no real agent is rung.
# T10 judges exact screen bytes through a stand-in tmux (tmuxshim.sh, fixtures.py).
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
export THYLA_WAKE_YIP=$T/fakeyip.sh FAKEYIP_STATE=$WORK/fy THYLA_WAKE_POLL=1 THYLA_WAKE_HOLD_TICK=1
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

echo "== T10 the box parser on exact bytes (a stand-in tmux replays each fixture; f1 is a measured idle pane)"
FX=$WORK/fx; mkdir -p "$FX/bin"; cp "$T/tmuxshim.sh" "$FX/bin/tmux"; python3 -I "$T/fixtures.py" "$FX"
nfx=0
for f in "$FX"/*.txt; do
    want=$(basename "$f" .txt); want=${want##*.}; nfx=$((nfx+1))
    got=$(SHIM_CAPTURE=$f PATH=$FX/bin:$PATH "$W" probe %7 | sed -n 's/^box *: //p')
    [ "$got" = "$want" ] && pass "T10 $(basename "$f" .txt) reads $want" || fail "T10 $(basename "$f") reads '$got', want $want"
done
[ "$nfx" = 8 ] && pass "T10 all 8 fixtures judged" || fail "T10 judged $nfx fixtures, want 8"
got=$(SHIM_CAPTURE=$FX/f1_measured.empty.txt PATH=$FX/bin:$PATH "$W" probe %7 | sed -n 's/^boxtext *: //p')
[ "$got" = 'placeholder="keep going"' ] && pass "T10 probe names the dim suggestion" || fail "T10 boxtext '$got'"

echo "== T11 a dim suggestion in a real pane is an empty box: the wake is typed"
setres "HELD by main for 2m, 1.0h left"
o=$WORK/o11; : > "$o"; p=$(pane "python3 -I $T/fakebox.py $o placeholder"); sleep 1
b=$(boxof "$p"); [ "$b" = empty ] && pass "T11 the suggestion box reads empty" || fail "T11 suggestion box reads '$b'"
TMUX_PANE=$p "$W" watch pi --say "T11" >/dev/null || fail "T11 arm"
sleep 5
grep -q '^\[thyla-wake\] pi changed at .*: pi  *FREE -- T11$' "$o" && [ "$(lines "$o")" = 1 ] \
    && pass "T11 one wake line" || fail "T11 got: $(cat "$o")"

echo "== T12 hold granted but the box stays typed past the held bound -> released, then told"
: > "$F/calls"; setres "HELD by you for 0s, 6.0h left"; echo "HELD" > "$F/hold.out"; echo 0 > "$F/hold.rc"; echo 1 > "$F/hold.sleep"
o=$WORK/o12; : > "$o"; p=$(pane "python3 -I $T/fakebox.py $o draft"); sleep 1
THYLA_WAKE_HELD_BOUND=4 TMUX_PANE=$p "$W" hold mac "T12" --say "T12" >/dev/null || fail "T12 arm"
sleep 9
grep -qx 'release mac' "$F/calls" && pass "T12 released at the bound" || fail "T12 calls: $(cat "$F/calls")"
[ ! -s "$o" ] && pass "T12 nothing typed into the draft" || fail "T12 typed into the draft: $(cat "$o")"
grep -q "	released	the wake could not be typed in 4s" "$THYLA_WAKE_DIR/log.tsv" && pass "T12 the release is logged" || fail "T12 no released line"
grep -q '	waiting	input box: typed text=" hello draft"' "$THYLA_WAKE_DIR/log.tsv" && pass "T12 the wait logs what the box held" || fail "T12 no box text in the waiting line"
sleep 10
grep -q '^\[thyla-wake\] mac WAS yours from [0-9:]*Z, but your input box was not usable for 4s, so the lease was RELEASED at [0-9:]*Z .* -- T12$' "$o" \
    && [ "$(lines "$o")" = 1 ] && pass "T12 told, once the box emptied" || fail "T12 got: $(cat "$o")"

echo "== T13 cancel stops only the caller's watchers; --all stops a peer's too"
: > "$F/calls"; setres "HELD by main for 2m, 1.0h left"; echo 60 > "$F/hold.sleep"
sleep 120 & peer=$!
printf 'mode=hold res=mac pane=%%99998 prog=x me=corona armed=00:00Z\n' > "$THYLA_WAKE_DIR/$peer.meta"
o=$WORK/o13; : > "$o"; p=$(pane "python3 -I $T/fakebox.py $o empty"); sleep 1
TMUX_PANE=$p "$W" watch mac >/dev/null || fail "T13 arm"
sleep 1
out=$("$W" cancel)
grep -q '1 watcher(s) stopped (armed as aux; 1 of other agents left alone)' <<< "$out" && pass "T13 cancel stopped aux's one" || fail "T13 cancel said: $out"
kill -0 "$peer" 2>/dev/null && [ -e "$THYLA_WAKE_DIR/$peer.meta" ] && pass "T13 the peer's watcher and its meta survive" || fail "T13 the peer's watcher was stopped"
"$W" cancel --all >/dev/null
sleep 1
! kill -0 "$peer" 2>/dev/null && pass "T13 cancel --all stopped the peer's" || { fail "T13 --all left the peer"; kill "$peer" 2>/dev/null; }

# ---- an EXPIRED lease in the way. yip's own line, and one line under it as the entry's detail.
exp_line='EXPIRED -- main held it 14.2h, TTL ran out 13.4h ago. Not free: `steal` it with a reason, or ask them.'
setexp() { printf 'mac   %s\n        runner: host pid 4242 dead\npi    FREE\n' "$exp_line" > "$F/resources"; }
ncalls() { grep -c '^call ' "$F/calls"; }

echo "== T14 hold behind an EXPIRED lease: the holder is rung once, the pane told once; the grant still wakes"
: > "$F/calls"; rm -f "$F/call.body" "$F/call.rc"; setexp
echo "HELD: mac is yours for 6.0h." > "$F/hold.out"; echo 0 > "$F/hold.rc"; echo 12 > "$F/hold.sleep"
o=$WORK/o14; : > "$o"; p=$(pane "python3 -I $T/fakebox.py $o empty"); sleep 1
THYLA_WAKE_EXPIRED_POLL=2 TMUX_PANE=$p "$W" hold mac "T14" --say "T14" >/dev/null || fail "T14 arm"
sleep 9
[ "$(ncalls)" = 1 ] && grep -qx 'call main mac: your lease EXPIRED and aux is queued behind it' "$F/calls" \
    && pass "T14 the holder was rung exactly once" || fail "T14 calls: $(cat "$F/calls")"
[ "$(grep -c '	expired-' "$THYLA_WAKE_DIR/log.tsv")" = 1 ] && grep -q "	expired-rang	main; mac   EXPIRED" "$THYLA_WAKE_DIR/log.tsv" \
    && pass "T14 the ring is logged, once" || fail "T14 expired lines: $(grep -c '	expired-' "$THYLA_WAKE_DIR/log.tsv")"
grep -q '^mac   EXPIRED -- main held it' "$F/call.body" && grep -q 'runner: host pid 4242 dead' "$F/call.body" \
    && grep -q 'yip release mac' "$F/call.body" && ! grep -q '^pi ' "$F/call.body" \
    && pass "T14 the ring carries yip's entry for mac, and the way out" || fail "T14 body: $(cat "$F/call.body" 2>/dev/null)"
grep -q '^\[thyla-wake\] mac is EXPIRED under main, which is not free: .*(main was rung once)\. yip: mac   EXPIRED -- main held it 14\.2h, TTL ran out 13\.4h ago\. .* -- T14$' "$o" \
    && [ "$(lines "$o")" = 1 ] && pass "T14 the pane was told once" || fail "T14 got: $(cat "$o")"
! grep -q release "$F/calls" && pass "T14 nothing released, nothing stolen" || fail "T14 a release behind an EXPIRED lease: $(tr '\n' ';' < "$F/calls")"
setres "HELD by you for 0s, 6.0h left"
sleep 8
[ "$(lines "$o")" = 2 ] && tail -1 "$o" | grep -q '^\[thyla-wake\] mac is yours: HELD by aux since ' \
    && pass "T14 the grant still wakes" || fail "T14 after the grant: $(cat "$o")"
[ "$(ncalls)" = 1 ] && pass "T14 still one ring" || fail "T14 calls after: $(cat "$F/calls")"

echo "== T15 control, one word apart: a peer's HELD lease, looked at as often, rings nobody and types nothing"
: > "$F/calls"; rm -f "$F/call.body"
printf 'mac   HELD by main for 14.2h, 1.0h left\n        runner: host pid 4242 dead\npi    FREE\n' > "$F/resources"
echo "WAITING: queue 1, timed out" > "$F/hold.out"; echo 3 > "$F/hold.rc"; echo 8 > "$F/hold.sleep"
o=$WORK/o15; : > "$o"; p=$(pane "python3 -I $T/fakebox.py $o empty"); sleep 1
THYLA_WAKE_EXPIRED_POLL=2 TMUX_PANE=$p "$W" hold mac "T15" --say "T15" >/dev/null || fail "T15 arm"
sleep 6
[ "$(ncalls)" = 0 ] && [ ! -s "$o" ] && pass "T15 silent while the lease is HELD" || fail "T15 calls: $(cat "$F/calls"); typed: $(cat "$o")"
grep -qx 'hold mac T15 --for 2h --wait 4h' "$F/calls" && pass "T15 its hold was running: the silence is not a dead watcher" || fail "T15 calls: $(cat "$F/calls")"
sleep 6
grep -q '^\[thyla-wake\] mac hold ended WITHOUT the lease ' "$o" && [ "$(lines "$o")" = 1 ] && [ "$(ncalls)" = 0 ] \
    && pass "T15 and it was watching to the end" || fail "T15 got: $(cat "$o")"

echo "== T16 watch behind an EXPIRED lease: the pane is told once, nobody is rung"
: > "$F/calls"; setexp
o=$WORK/o16; : > "$o"; p=$(pane "python3 -I $T/fakebox.py $o empty"); sleep 1
TMUX_PANE=$p "$W" watch mac --say "T16" >/dev/null || fail "T16 arm"
sleep 8
grep -q '^\[thyla-wake\] mac is EXPIRED under main, which is not free: .*(main was not rung: a watch claims nothing)\. .* -- T16$' "$o" \
    && [ "$(lines "$o")" = 1 ] && pass "T16 told once across several looks" || fail "T16 got: $(cat "$o")"
[ "$(ncalls)" = 0 ] && pass "T16 nobody rung" || fail "T16 a watch rang: $(tr '\n' ';' < "$F/calls")"
"$W" cancel >/dev/null; sleep 1

echo "== T17 the ring fails: the pane is told that it did"
: > "$F/calls"; setexp; echo 1 > "$F/call.rc"; echo 60 > "$F/hold.sleep"
o=$WORK/o17; : > "$o"; p=$(pane "python3 -I $T/fakebox.py $o empty"); sleep 1
THYLA_WAKE_EXPIRED_POLL=2 TMUX_PANE=$p "$W" hold mac "T17" --say "T17" >/dev/null || fail "T17 arm"
sleep 7
grep -q '^\[thyla-wake\] mac is EXPIRED under main, .*(ringing main FAILED)\. .* -- T17$' "$o" && [ "$(lines "$o")" = 1 ] \
    && pass "T17 told, with the failed ring named" || fail "T17 got: $(cat "$o")"
grep -q "	expired-ring-failed	main; " "$THYLA_WAKE_DIR/log.tsv" && pass "T17 the failure is logged" || fail "T17 no expired-ring-failed line"
"$W" cancel >/dev/null; sleep 1; rm -f "$F/call.rc"

echo "== T18 this agent's own EXPIRED lease rings nobody and tells nothing"
: > "$F/calls"; printf 'mac   EXPIRED -- aux held it 3.0h, TTL ran out 1.0h ago. Not free.\npi    FREE\n' > "$F/resources"
o=$WORK/o18; : > "$o"; p=$(pane "python3 -I $T/fakebox.py $o empty"); sleep 1
THYLA_WAKE_EXPIRED_POLL=2 TMUX_PANE=$p "$W" hold mac "T18" --say "T18" >/dev/null || fail "T18 arm"
sleep 6
[ "$(ncalls)" = 0 ] && [ ! -s "$o" ] && pass "T18 silent" || fail "T18 calls: $(cat "$F/calls"); typed: $(cat "$o")"
"$W" cancel >/dev/null; sleep 1

echo "== T19 a renewed lease that expires again is a new expiry: rung and told again"
: > "$F/calls"; setexp
o=$WORK/o19; : > "$o"; p=$(pane "python3 -I $T/fakebox.py $o empty"); sleep 1
THYLA_WAKE_EXPIRED_POLL=2 TMUX_PANE=$p "$W" hold mac "T19" --say "T19" >/dev/null || fail "T19 arm"
sleep 6
[ "$(ncalls)" = 1 ] && [ "$(lines "$o")" = 1 ] && pass "T19 first expiry: one ring, one line" || fail "T19 first: calls $(ncalls), lines $(lines "$o")"
setres "HELD by main for 0s, 2.0h left"
sleep 5
setexp
sleep 7
[ "$(ncalls)" = 2 ] && [ "$(lines "$o")" = 2 ] && pass "T19 second expiry: a second ring, a second line" || fail "T19 second: calls $(ncalls), lines $(lines "$o")"
"$W" cancel >/dev/null; sleep 1

echo "== log"; cat "$THYLA_WAKE_DIR/log.tsv" | cut -c1-200
tmux kill-session -t thyla-wake-test 2>/dev/null
echo "work dir: $WORK"
echo "RESULT ok=$ok bad=$bad"
