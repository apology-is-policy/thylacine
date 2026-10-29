#!/bin/sh
# The host compiler selector's three states (tools/host-sanitizer-cc.py), each
# driven through tools/test-sqpoll-ticket.py, the shortest sanitized-only
# driver: no CC and no LLVM (missing tooling, exit 3), CC=/usr/bin/clang (Apple
# clang, whose ASan runtime does not start: exit 3 from the probe, before any
# case), and CC naming LLVM clang explicitly (honoured and recorded, every
# mutant killed). Each control states its expected exit and text; a control
# that does not see both fails the run.
# Run from the checkout root (no cd), under the mac lease:
#   sh work/oct22-merge/selector-controls.sh
set -u
ROOT=/Users/northkillpd/projects/thylacine-corona
[ "$(pwd)" = "$ROOT" ] || { echo "REFUSING: run from $ROOT, not $(pwd)"; exit 3; }
YIP="${THYLA_WAKE_YIP:-$(command -v yip || echo "$HOME/.local/bin/yip")}"
STAMP=$(date -u '+%Y%m%dT%H%M%SZ')
OUT=work/oct22-merge/selector-$STAMP
mkdir -p "$OUT"
"$YIP" resources > "$OUT/lease.txt" 2>&1 || true
/usr/bin/grep -qE '^mac +HELD by you' "$OUT/lease.txt" || { echo "STOPPING: the mac lease is not mine"; exit 6; }
LLVM=/opt/homebrew/opt/llvm/bin/clang
record() { echo "$(date -u '+%Y-%m-%dT%H:%M:%SZ')  $*" | tee -a "$OUT/stages.txt"; }
record "HEAD $(git rev-parse HEAD), tracked changes: $(git status --porcelain --untracked-files=no | wc -l | tr -d ' ')"
bad=0
control() { # control <name> <want rc> <want text> <command...>
  _n=$1 _rc_want=$2 _text=$3; shift 3
  record "START $_n: $*"
  "$@" > "$OUT/$_n.log" 2>&1
  _rc=$?
  if [ "$_rc" = "$_rc_want" ] && /usr/bin/grep -qF -- "$_text" "$OUT/$_n.log"; then
    record "END $_n rc=$_rc as expected ($_text)"
  else
    record "END $_n rc=$_rc, WANTED rc=$_rc_want and \"$_text\""; bad=1
  fi
}
control missing 3 'SANITIZER TOOLING: no CC and no LLVM clang at /nonexistent/bin/clang' \
  env -u CC LLVM_PREFIX=/nonexistent python3 -I tools/test-sqpoll-ticket.py --logs "$OUT/missing"
control apple-clang 3 'SANITIZER TOOLING: /usr/bin/clang: an empty sanitized program' \
  env CC=/usr/bin/clang python3 -I tools/test-sqpoll-ticket.py --logs "$OUT/apple-clang"
control explicit-llvm 0 'PASS overflow' \
  env CC="$LLVM" python3 -I tools/test-sqpoll-ticket.py --mutants --logs "$OUT/explicit-llvm"
/usr/bin/grep -qxF "source: CC=$LLVM" "$OUT/explicit-llvm/compiler.txt" \
  && record "explicit-llvm compiler.txt records source: CC=$LLVM" \
  || { record "explicit-llvm compiler.txt does not record the explicit CC"; bad=1; }
[ "$bad" = 0 ] && record "SELECTOR CONTROLS PASS" || { record "SELECTOR CONTROLS FAILED"; exit 1; }
