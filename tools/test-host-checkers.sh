#!/usr/bin/env bash
# tools/test-host-checkers.sh -- run every host checker (tools/test-*.py), each
# in every mode it offers, and fail if any one fails.
#
# WHY THIS EXISTS. The host checkers read the tree's own sources -- many find a
# kernel function by name or a header by a text anchor and compile it on the
# host -- so a change ANYWHERE that renames, moves or wraps that text breaks
# them. Each was written to be run by hand by the chunk that owns its surface,
# and nothing else ran them: on 2026-10-27 an unbuilt series had broken two of
# them (a u32 counter the magazines model did not define; an ABI header that
# stopped compiling on its own) with no failure anywhere. One command that runs
# them all is what a change to any of those sources needs.
#
# Modes come from each checker's own argparse: --mutants and --sanitize are
# passed when it takes them, --logs always gets a fresh directory. A checker's
# exit status is its verdict; exit 3 is a sanitizer-tooling stop
# (tools/host-sanitizer-cc.py), reported as TOOLING and still a failure here.
#
# Usage: tools/test-host-checkers.sh [--logs DIR] [CHECKER...]
#   CHECKER: a name such as test-magazines; default every tools/test-*.py.
set -uo pipefail
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
logs=""
names=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --logs) logs="$2"; shift 2 ;;
        -h|--help) sed -n '2,23p' "$0"; exit 0 ;;
        *) names+=("${1%.py}"); shift ;;
    esac
done
[[ -n "$logs" ]] || logs="$REPO_ROOT/build/host-checkers/$(date -u +%Y%m%dT%H%M%SZ)"
mkdir -p "$logs"
if [[ ${#names[@]} -eq 0 ]]; then
    for f in "$REPO_ROOT"/tools/test-*.py; do names+=("$(basename "$f" .py)"); done
fi
[[ ${#names[@]} -gt 0 ]] || { echo "test-host-checkers: no checkers found" >&2; exit 2; }

fails=0
for n in "${names[@]}"; do
    f="$REPO_ROOT/tools/$n.py"
    [[ -f "$f" ]] || { echo "MISSING $n"; fails=$((fails + 1)); continue; }
    args=()
    grep -qE "['\"]--logs['\"]" "$f" && args+=(--logs "$logs/$n")
    grep -qE "['\"]--mutants['\"]" "$f" && args+=(--mutants)
    grep -qE "['\"]--sanitize['\"]" "$f" && args+=(--sanitize)
    t0=$(date -u +%s)
    python3 -I "$f" ${args[@]+"${args[@]}"} > "$logs/$n.out" 2>&1
    rc=$?
    secs=$(( $(date -u +%s) - t0 ))
    if [[ $rc -eq 0 ]]; then verdict=PASS
    elif [[ $rc -eq 3 ]]; then verdict=TOOLING; fails=$((fails + 1))
    else verdict=FAIL; fails=$((fails + 1)); fi
    printf '%-8s %-32s rc=%-3s %4ss  %s\n' "$verdict" "$n" "$rc" "$secs" "$(tail -n 1 "$logs/$n.out" | cut -c1-100)"
done
echo "test-host-checkers: ${#names[@]} run, $fails not passing; logs in $logs"
[[ $fails -eq 0 ]]
