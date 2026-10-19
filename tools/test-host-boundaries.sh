#!/usr/bin/env bash
# tools/test-host-boundaries.sh -- every host boundary checker, clean and under
# each mutant it declares.
#
# A checker is tools/test-<name>.py beside tools/host-tests/<name>.c: it
# compiles the code it names from this tree with that test and mocked owners,
# and runs it under ASan and UBSan. build.sh runs every clean leg on every
# build (host_boundary_check). This gate adds the mutant legs, which prove each
# checker still refuses the defect it exists for: a checker that passes
# everything passes a broken tree too.
#
# The set is derived, not listed: every tools/host-tests/*.c must have its
# driver, and each driver's mutants are read from its own --help (argparse's
# "--mutant {a,b}"), so a new test or mutant joins without an edit here. A
# driver that declares no mutant fails the gate. A mutant leg passes when the
# driver exits 0, which each driver does only after asserting that its mutant
# was refused.
#
# Drivers whose usage names a `musl` argument get the series-applied musl tree
# that `tools/build.sh sysroot` (or any full build) leaves at
# build/pouch/musl-src; --musl names another.
#
# Usage: tools/test-host-boundaries.sh [--musl DIR]
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LLVM_PREFIX="${LLVM_PREFIX:-/opt/homebrew/opt/llvm}"
MUSL="$REPO_ROOT/build/pouch/musl-src"
if [[ "${1:-}" == --musl ]]; then MUSL="$2"; shift 2; fi
OUT="$REPO_ROOT/build/host-boundaries/gate"

rm -rf "$OUT"; mkdir -p "$OUT"
fail=0
drivers=()
for src in "$REPO_ROOT"/tools/host-tests/*.c; do
    name="$(basename "$src" .c)"
    if [[ ! -f "$REPO_ROOT/tools/test-$name.py" ]]; then
        echo "FAIL $name: tools/host-tests/$name.c has no driver tools/test-$name.py"
        fail=1
        continue
    fi
    drivers+=("$name")
done
if [[ ${#drivers[@]} -eq 0 ]]; then
    echo "FAIL no host boundary checker found under tools/host-tests"
    exit 1
fi

# One driver's legs, sequentially, into $OUT/<name>/<leg>; one result line each.
run_driver() {
    local name="$1" drv="$REPO_ROOT/tools/test-$1.py" usage mutants leg rc
    local pre=()
    usage="$(COLUMNS=10000 python3 "$drv" --help | head -n 1)"
    if [[ " $usage " == *" musl "* ]]; then
        if [[ ! -d "$MUSL/src" ]]; then
            echo "FAIL $name: no patched musl tree at $MUSL (run tools/build.sh sysroot)"
            return
        fi
        pre=("$MUSL")
    fi
    mutants="$(printf '%s\n' "$usage" | sed -n 's/.*--mutant {\([^}]*\)}.*/\1/p' | tr ',' ' ')"
    if [[ -z "$mutants" ]]; then
        echo "FAIL $name: declares no --mutant, so nothing proves it can refuse"
        return
    fi
    mkdir -p "$OUT/$name"
    for leg in clean $mutants; do
        local args=("${pre[@]}" "$OUT/$name/$leg" --cc "$LLVM_PREFIX/bin/clang")
        [[ "$leg" == clean ]] || args+=(--mutant "$leg")
        rc=0
        python3 "$drv" "${args[@]}" > "$OUT/$name/$leg.log" 2>&1 || rc=$?
        if [[ $rc -eq 0 ]]; then
            echo "PASS $name $leg"
        else
            echo "FAIL $name $leg: exit $rc (log: $OUT/$name/$leg.log)"
        fi
    done
}

pids=()
for name in "${drivers[@]}"; do
    run_driver "$name" > "$OUT/$name.result" 2>&1 &
    pids+=("$!")
done
for pid in "${pids[@]}"; do wait "$pid" || fail=1; done

legs=0; failed=0
for name in "${drivers[@]}"; do
    cat "$OUT/$name.result"
    legs=$((legs + $(/usr/bin/grep -c '^PASS \|^FAIL ' "$OUT/$name.result" || true)))
    failed=$((failed + $(/usr/bin/grep -c '^FAIL ' "$OUT/$name.result" || true)))
done
[[ $failed -eq 0 ]] || fail=1
echo "host-boundaries: ${#drivers[@]} checkers, $legs legs, $failed failed"
exit $fail
