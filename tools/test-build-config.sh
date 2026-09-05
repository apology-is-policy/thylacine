#!/usr/bin/env bash
# tools/test-build-config.sh -- unit test for the build configurator core.
#
# DISCRIMINATION (per CLAUDE.md / the control discipline): the load-bearing tests
# below FAIL WITHOUT the fix they witness --
#   T-dev-orthogonal:  the "accounts, no tests" shape impossible in the old
#                      --production/--dev bundles (finding #1, the orthogonality).
#   T-implies:         bc_resolve must auto-raise DEV_ACCOUNTS when BOOT_PROBES=y;
#                      delete the implies rule in bc_resolve and this test fails.
#   T-precedence:      a fragment must override a preset, and --set must override
#                      a fragment; break the call-order/last-writer-wins and it fails.
#   T-export:          the map from clean symbols onto build.sh's heterogeneous
#                      knobs (incl. the TICKLESS->no_tickless INVERSION and the
#                      DEV_ACCOUNTS CMake def); a wrong mapping fails here.
set -u
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"
export BC_DIR_CONFIGS="configs"
# shellcheck disable=SC1091
. tools/build-config.sh

fail=0
ok()   { printf '  ok   %s\n' "$1"; }
bad()  { printf '  FAIL %s\n' "$1"; fail=1; }
eq()   { if [[ "$2" == "$3" ]]; then ok "$1 ($2)"; else bad "$1: want '$3', got '$2'"; fi; }

echo "== defaults =="
bc_reset
eq "default KASLR"        "$(bc_get KASLR)"        "n"
eq "default DEV_ACCOUNTS" "$(bc_get DEV_ACCOUNTS)" "y"
eq "default BUILD_TYPE"   "$(bc_get BUILD_TYPE)"   "debug"

echo "== preset: production =="
bc_reset; bc_apply_preset production
eq "prod BUILD_TYPE"    "$(bc_get BUILD_TYPE)"    "release"
eq "prod KASLR"         "$(bc_get KASLR)"         "y"
eq "prod HARDENING"     "$(bc_get HARDENING_FULL)" "y"
eq "prod TESTS"         "$(bc_get TESTS)"         "n"
eq "prod DEV_ACCOUNTS"  "$(bc_get DEV_ACCOUNTS)"  "y"

echo "== preset: default (bare-build backward-compat: tests+probes ON) =="
bc_reset; bc_apply_preset default
eq "default preset TESTS"        "$(bc_get TESTS)"        "y"
eq "default preset BOOT_PROBES"  "$(bc_get BOOT_PROBES)"  "y"
eq "default preset DEV_ACCOUNTS" "$(bc_get DEV_ACCOUNTS)" "y"

echo "== T-dev-orthogonal: accounts WITHOUT tests (finding #1) =="
bc_reset; bc_apply_preset dev
eq "dev DEV_ACCOUNTS"   "$(bc_get DEV_ACCOUNTS)"  "y"
eq "dev BOOT_PROBES"    "$(bc_get BOOT_PROBES)"   "n"
eq "dev TESTS"          "$(bc_get TESTS)"         "n"

echo "== T-precedence: fragment overrides preset, --set overrides fragment =="
bc_reset; bc_apply_preset dev
eq "dev KASLR (pre)"    "$(bc_get KASLR)"         "n"
bc_apply_fragment kaslr
eq "frag KASLR override" "$(bc_get KASLR)"        "y"
bc_set BUILD_TYPE=release
eq "cli BUILD_TYPE override" "$(bc_get BUILD_TYPE)" "release"

echo "== T-implies: BOOT_PROBES=y auto-raises DEV_ACCOUNTS =="
bc_reset; bc_set BOOT_PROBES=y; bc_set DEV_ACCOUNTS=n
eq "pre-resolve DEV_ACCOUNTS" "$(bc_get DEV_ACCOUNTS)" "n"
bc_resolve 2>/dev/null
eq "post-resolve DEV_ACCOUNTS" "$(bc_get DEV_ACCOUNTS)" "y"

echo "== validation: bad value + unknown symbol rejected =="
bc_reset
if bc_set KASLR=maybe 2>/dev/null; then bad "bad value accepted"; else ok "bad value rejected"; fi
if bc_set_one BOGUS y 2>/dev/null; then bad "unknown symbol accepted"; else ok "unknown symbol rejected"; fi

echo "== T-export: symbols -> build.sh knobs (incl. TICKLESS inversion + DEV_ACCOUNTS) =="
build_type=""; kernel_tests=""; boot_probes=""; hardening_full=""; kaslr=""
sanitize="__unset__"; no_tickless=""; dev_accounts=""; extra_cmake_args=()
unset THYLACINE_BAKE_GOROOT THYLACINE_BAKE_CLADE 2>/dev/null || true
bc_reset; bc_apply_preset production; bc_resolve 2>/dev/null; bc_export
eq "export build_type"    "$build_type"    "Release"
eq "export kernel_tests"  "$kernel_tests"  "OFF"
eq "export boot_probes"   "$boot_probes"   "OFF"
eq "export hardening_full" "$hardening_full" "ON"
eq "export kaslr"         "$kaslr"         "ON"
eq "export sanitize (none->empty)" "$sanitize" ""
eq "export no_tickless (TICKLESS=y -> OFF)" "$no_tickless" "OFF"
eq "export dev_accounts (DEV_ACCOUNTS=y -> ON)" "$dev_accounts" "ON"
eq "export THYLACINE_BAKE_GOROOT" "${THYLACINE_BAKE_GOROOT:-}" "1"
eq "export THYLACINE_BAKE_CLADE"  "${THYLACINE_BAKE_CLADE:-}"  "0"

echo "== T-honor-env: a pre-set legacy env var is honored (D-b transition shim) =="
export THYLACINE_BAKE_CLADE=1                       # production sets CLADE=n (-> 0)
bc_reset; bc_apply_preset production; bc_export
eq "honor pre-set THYLACINE_BAKE_CLADE" "${THYLACINE_BAKE_CLADE:-}" "1"
unset THYLACINE_BAKE_CLADE

echo "== emit: build/.config is written + grouped =="
tmp="$(mktemp)"; bc_reset; bc_apply_preset dev; bc_resolve 2>/dev/null; bc_emit_config "$tmp"
if grep -q '^DEV_ACCOUNTS' "$tmp" && grep -q '# \[compile\]' "$tmp"; then ok "emit shape"; else bad "emit shape"; fi
rm -f "$tmp"

echo
if [[ "$fail" == 0 ]]; then echo "ALL PASS"; exit 0; else echo "FAILURES"; exit 1; fi
