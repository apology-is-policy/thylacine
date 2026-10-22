#!/bin/sh
# Host legs for the main merge, at MERGE_PIN: every tools/test-*.py, its
# arguments read from its own argparse (--logs or --out into this run's
# directory, --mutants when it has one, and a second run with --sanitize when
# it has that); the loom-client suite; the userspace Rust host tests; corona's
# sims mutants; the burrow spec cfgs; and the Loom service ABI and index
# checkers and the authority ABI checker, which build.sh does not run. Every
# leg runs whatever an earlier one did; each exit status is recorded, and any
# nonzero leg makes the run exit 1, to be read.
# DRYRUN=1 lists the legs and stops (no lease needed, nothing built).
# Run from the checkout root (no cd): MERGE_PIN=<full sha> sh work/oct22-merge/host.sh
set -u
ROOT=/Users/northkillpd/projects/thylacine-corona
[ "$(pwd)" = "$ROOT" ] || { echo "REFUSING: run from $ROOT, not $(pwd)"; exit 3; }
STAMP=$(date -u '+%Y%m%dT%H%M%SZ')
OUT=work/oct22-merge/host-$STAMP
mkdir -p "$OUT"
YIP="${THYLA_WAKE_YIP:-$(command -v yip || echo "$HOME/.local/bin/yip")}"
MERGE_PIN=${MERGE_PIN:?set MERGE_PIN to the full sha under test}
record() { echo "$(date -u '+%Y-%m-%dT%H:%M:%SZ')  $*" | tee -a "$OUT/stages.txt"; }
has_opt() { /usr/bin/grep -qE "add_argument\([[:space:]]*'$2'" "$1"; }
[ "$(git rev-parse HEAD)" = "$MERGE_PIN" ] || { echo "REFUSING: HEAD $(git rev-parse HEAD) is not $MERGE_PIN"; exit 3; }
[ "$(git status --porcelain --untracked-files=no | wc -l | tr -d ' ')" = 0 ] || { echo "REFUSING: tracked modifications"; exit 3; }
# has_opt checks itself: an option a driver declares is found, one it lacks is not.
has_opt tools/test-magazines.py --mutants && ! has_opt tools/test-magazines.py --sanitize \
  || { echo "REFUSING: has_opt misreads tools/test-magazines.py"; exit 3; }
if [ -z "${DRYRUN:-}" ]; then
  "$YIP" resources > "$OUT/lease.txt" 2>&1 || true
  /usr/bin/grep -qE '^mac +HELD by you' "$OUT/lease.txt" || { echo "STOPPING: the mac lease is not mine"; exit 6; }
fi
record "HEAD $MERGE_PIN, clean"

failed=""
legs=0
leg() { # leg <name> <command...>
  _n=$1; shift
  legs=$((legs + 1))
  if [ -n "${DRYRUN:-}" ]; then record "LEG $_n: $*"; return 0; fi
  record "START $_n: $*"
  "$@" > "$OUT/$_n.log" 2>&1
  _rc=$?
  record "END $_n rc=$_rc"
  [ "$_rc" = 0 ] || failed="$failed $_n"
}

for f in tools/test-*.py; do
  s=$(basename "$f" .py)
  set -- python3 -I "$f"
  if has_opt "$f" --logs; then set -- "$@" --logs "$OUT/$s"; fi
  if has_opt "$f" --out; then set -- "$@" --out "$OUT/$s"; fi
  if has_opt "$f" --mutants; then set -- "$@" --mutants; fi
  leg "$s" "$@"
  if has_opt "$f" --sanitize; then
    set -- python3 -I "$f" --sanitize
    if has_opt "$f" --logs; then set -- "$@" --logs "$OUT/$s-san"; fi
    leg "$s-san" "$@"
  fi
done
leg loom-client tools/test-loom-client.sh --out "$OUT/loom-client"
leg rust-host tools/test-rust.sh
leg sims-mutants python3 -I work/oct21-sims/mutants.py "$OUT/sims"
leg burrow-spec sh specs/check-burrow.sh
leg loom-service-index python3 -I tools/check-loom-service-index.py --logs "$OUT/loom-service-index"
leg loom-service-abi python3 -I tools/check-loom-service-abi.py
leg authority-abi python3 -I tools/check-authority-abi.py

[ "$(git rev-parse HEAD)" = "$MERGE_PIN" ] && [ "$(git status --porcelain --untracked-files=no | wc -l | tr -d ' ')" = 0 ] \
  || { record "HEAD moved or the tree changed during the run"; exit 8; }
[ -z "${DRYRUN:-}" ] || { record "DRYRUN: $legs legs"; exit 0; }
if [ -n "$failed" ]; then record "FAILED legs ($legs run):$failed"; exit 1; fi
record "ALL $legs HOST LEGS PASS at $MERGE_PIN"
