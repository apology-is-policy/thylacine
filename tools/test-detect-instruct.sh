#!/usr/bin/env bash
# tools/test-detect-instruct.sh -- guards the tooling -> forage contract.
#
# Two consumers point users at `tools/forage.sh <target>`: build.sh's
# detect-and-instruct (BUILD-CONFIG-DESIGN.md 5.3, via forage_hint) and the
# configure wizard's step-4 chunk-input remedy. If a forage target is ever
# renamed, every hint naming the old one silently rots into a dead instruction
# (the "a lifted constant voids the proofs that named it" hazard). This test
# extracts every forage target named by EITHER consumer and asserts each is a
# REAL forage target, and that the three load-bearing build.sh sites are wired.
# No build; ~1 s.
#
# BUILD_SH / CONFIGURE overridable so the contract can be PROVEN against a
# sabotaged copy.
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_SH="${BUILD_SH:-$REPO_ROOT/tools/build.sh}"
CONFIGURE="${CONFIGURE:-$REPO_ROOT/tools/configure.sh}"
FORAGE="$REPO_ROOT/tools/forage.sh"

pass=0; fail=0
ok()  { printf '  PASS  %s\n' "$1"; pass=$((pass+1)); }
bad() { printf '  FAIL  %s\n' "$1"; fail=$((fail+1)); }

echo "== test-detect-instruct =="

# build.sh: every `forage_hint <target> ...` CALL (the definition line is
# `forage_hint()`, no space before '(', so it is not matched).
bh_targets="$(grep -oE 'forage_hint [a-z][a-z-]*' "$BUILD_SH" | awk '{print $2}' | sort -u)"
# configure.sh: every `forage.sh <target>` the wizard names as a remedy.
cf_targets="$(grep -oE 'forage\.sh [a-z][a-z-]*' "$CONFIGURE" | awk '{print $2}' | sort -u)"
targets="$(printf '%s\n%s\n' "$bh_targets" "$cf_targets" | grep -v '^$' | sort -u)"
if [[ -z "$targets" ]]; then bad "no forage target references found in build.sh or configure.sh"; fi

for t in $targets; do
    # status/all are forage COMMANDS, not chunk targets -- they are valid, so the
    # "unknown target" check passes them anyway; no special-casing needed.
    out="$(FORAGE_DRY=1 "$FORAGE" "$t" 2>&1)"
    if printf '%s' "$out" | grep -q "unknown target"; then
        bad "a consumer names forage target '$t' -- NOT a valid forage target (rotted)"
    else
        ok "consumer -> forage target '$t' is valid"
    fi
done

# the three load-bearing build.sh chunk-input sites must stay wired
for expect in go clade alpine; do
    if printf '%s\n' $bh_targets | grep -qx "$expect"; then ok "build.sh site wired: $expect"; else bad "forage_hint $expect site is missing from build.sh"; fi
done

echo "== $pass passed, $fail failed =="
[[ "$fail" -eq 0 ]]
