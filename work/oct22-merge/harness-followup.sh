#!/bin/sh
# Astra's follow-ups on the harness repair (yip 0243 turn 4), under the mac
# lease: the verdict's own controls against real sanitizer reports; every
# driver whose verdict, build receipts or fixture changed, in each mode it
# declares, with its mutants; then the selector controls again (the probe now
# builds with -fno-sanitize-recover=all). Each leg's exit status is recorded;
# any nonzero leg makes the run exit 1.
# Run from the checkout root (no cd), under the mac lease:
#   sh work/oct22-merge/harness-followup.sh
set -u
ROOT=/Users/northkillpd/projects/thylacine-corona
[ "$(pwd)" = "$ROOT" ] || { echo "REFUSING: run from $ROOT, not $(pwd)"; exit 3; }
[ "$(git status --porcelain --untracked-files=no | wc -l | tr -d ' ')" = 0 ] || { echo "REFUSING: tracked modifications"; exit 3; }
YIP="${THYLA_WAKE_YIP:-$(command -v yip || echo "$HOME/.local/bin/yip")}"
STAMP=$(date -u '+%Y%m%dT%H%M%SZ')
OUT=work/oct22-merge/followup-$STAMP
mkdir -p "$OUT"
"$YIP" resources > "$OUT/lease.txt" 2>&1 || true
/usr/bin/grep -qE '^mac +HELD by you' "$OUT/lease.txt" || { echo "STOPPING: the mac lease is not mine"; exit 6; }
record() { echo "$(date -u '+%Y-%m-%dT%H:%M:%SZ')  $*" | tee -a "$OUT/stages.txt"; }
record "HEAD $(git rev-parse HEAD), clean"
failed=''
leg() { # leg <name> <command...>
  _n=$1; shift
  record "START $_n: $*"
  "$@" > "$OUT/$_n.log" 2>&1
  _rc=$?
  record "END $_n rc=$_rc"
  [ "$_rc" = 0 ] || failed="$failed $_n"
}
leg checker python3 -I tools/host-sanitizer-cc.py --logs "$OUT/checker"
for d in service-admission addrspace-lifetime 9p-client-progress 9p-progress loom-service-pool; do
  leg "$d" python3 -I "tools/test-$d.py" --mutants --logs "$OUT/$d"
  leg "$d-san" python3 -I "tools/test-$d.py" --sanitize --mutants --logs "$OUT/$d-san"
done
leg loom-receipts python3 -I tools/test-loom-receipts.py --mutants --logs "$OUT/loom-receipts"
leg sqpoll-ticket python3 -I tools/test-sqpoll-ticket.py --mutants --logs "$OUT/sqpoll-ticket"
leg selector-controls sh work/oct22-merge/selector-controls.sh
[ -z "$failed" ] && record "ALL FOLLOW-UP LEGS PASS at $(git rev-parse HEAD)" || { record "FAILED legs:$failed"; exit 1; }
