#!/usr/bin/env bash
# tools/test-configure.sh -- discrimination tests for the build wizard
# (tools/configure.sh). Every case is a CONTROL that FAILS without the behavior
# it checks (M-PIN: a check that cannot fail proves nothing). No QEMU, no build --
# pure host bash; runs in ~1 s.
#
# Isolation: every run points BC_DIR_CONFIGS at a temp copy of configs/, so reads
# (--from/--edit) resolve and writes land THERE -- the real configs/ is never
# touched. Case "isolation" asserts exactly that (it is the regression guard for
# the env-override-clobber bug found while building this).
#
# Interactive answer streams are schema-ROBUST: the number of Enter-accepts before
# a target symbol is computed from the live schema (bc_index_of), so inserting a
# new symbol shifts nothing. A hard guard asserts the schema shape the streams
# assume, so a real drift fails LOUD instead of testing the wrong symbol.
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# Overridable so the discrimination of each case can be PROVEN: point CONFIGURE at
# a sabotaged copy and confirm the matching case fails (the fail-without-fix check).
CONFIGURE="${CONFIGURE:-$REPO_ROOT/tools/configure.sh}"

# source the schema core so the test can locate symbols the same way the wizard does
BC_DIR_CONFIGS="$REPO_ROOT/configs"
# shellcheck disable=SC1090
. "$REPO_ROOT/tools/build-config.sh"
bc_reset

TMP="$(mktemp -d)"
cleanup() { rm -rf "$TMP"; }
trap cleanup EXIT
cp "$REPO_ROOT"/configs/*.config "$TMP"/

pass=0; fail=0
ok()  { printf '  PASS  %s\n' "$1"; pass=$((pass+1)); }
bad() { printf '  FAIL  %s\n' "$1"; fail=$((fail+1)); }
assert_eq()   { if [[ "$1" == "$2" ]]; then ok "$3"; else bad "$3 (got '$1' want '$2')"; fi; }
assert_grep() { if printf '%s' "$1" | grep -qE "$2"; then ok "$3"; else bad "$3 (missing /$2/)"; fi; }
assert_ngrep(){ if printf '%s' "$1" | grep -qE "$2"; then bad "$3 (unexpected /$2/)"; else ok "$3"; fi; }

# value of KEY in an emitted .config (strips the '= ' and trailing '# desc')
cfg_val() { grep -E "^$2[[:space:]]*=" "$1" | head -1 | sed -E 's/^[^=]*=[[:space:]]*//; s/[[:space:]]*#.*$//; s/[[:space:]]*$//'; }
nblank()  { local i n="$1"; for ((i=0; i<n; i++)); do echo; done; }
run()     { BC_DIR_CONFIGS="$TMP" "$CONFIGURE" "$@"; }   # stdout+file land in $TMP

# real configs/ snapshot -- the isolation guard compares against this at the end
real_before="$(ls "$REPO_ROOT"/configs/ | sort)"

echo "== test-configure =="

# --- schema-shape guard: the interactive streams below assume this layout ----
# (fail LOUD if the schema drifts, rather than silently accepting defaults for
#  the wrong symbol -- the premise-drift discipline).
assert_eq "$(bc_index_of BUILD_TYPE)"   0 "schema: BUILD_TYPE is first"
assert_eq "$(bc_get BOOT_PROBES)"       n "schema: BOOT_PROBES default n"
assert_eq "$(bc_get DEV_ACCOUNTS)"      y "schema: DEV_ACCOUNTS default y"
assert_eq "$(bc_get KASLR)"             n "schema: KASLR default n"
assert_eq "$(bc_get CHUNK_GOROOT)"      y "schema: CHUNK_GOROOT default y"

# --- 1. seed fidelity (non-interactive --from) -------------------------------
# production differs from bare defaults on BUILD_TYPE/KASLR/HARDENING_FULL, so a
# wizard that ignored --from would write the default (debug/n/n) and fail these.
run --defaults --from production seed1 >/dev/null 2>&1
assert_eq "$(cfg_val "$TMP/seed1.config" BUILD_TYPE)"     release "1 seed: BUILD_TYPE=release"
assert_eq "$(cfg_val "$TMP/seed1.config" KASLR)"          y       "1 seed: KASLR=y"
assert_eq "$(cfg_val "$TMP/seed1.config" HARDENING_FULL)" y       "1 seed: HARDENING_FULL=y"

# --- 2. input honoring (interactive) -----------------------------------------
# custom base, then Enter-accept up to KASLR, answer KASLR=y (default is n). A
# wizard that dropped stdin writes n.
{ echo 4; nblank "$(bc_index_of KASLR)"; echo y; } | run --name in2 >/dev/null 2>&1
assert_eq "$(cfg_val "$TMP/in2.config" KASLR)" y "2 input: interactive KASLR=y honored (default n)"

# --- 3. live constraint: BOOT_PROBES=y announces + pins DEV_ACCOUNTS ----------
out3="$({ echo 4; nblank "$(bc_index_of BOOT_PROBES)"; echo y; } | run --name c3 2>&1)"
assert_grep "$out3" "enables DEV_ACCOUNTS"          "3 constraint: live announcement fires"
assert_eq "$(cfg_val "$TMP/c3.config" BOOT_PROBES)"  y "3 constraint: BOOT_PROBES=y written"
assert_eq "$(cfg_val "$TMP/c3.config" DEV_ACCOUNTS)" y "3 constraint: DEV_ACCOUNTS pinned y"
# negative: a walk that never sets BOOT_PROBES=y must NOT emit the announcement
out3n="$(echo 4 | run --name c3n 2>&1)"
assert_ngrep "$out3n" "enables DEV_ACCOUNTS" "3 constraint: silent when BOOT_PROBES stays n"

# --- 4. ?-help reprint (interactive) -----------------------------------------
# '?' at the first prompt must REPRINT the long help (so it appears twice: the
# always-shown display + the reprint) and must NOT be treated as an invalid value.
# A broken '?' handler passes '?' to the setter -> help shows once + "not valid".
helpphrase="release is -O2 with assertions off"
out4="$(printf '4\n?\n\n' | run --name h4 2>&1)"
cnt4="$(printf '%s\n' "$out4" | grep -c "$helpphrase")"
if [[ "$cnt4" -ge 2 ]]; then ok "4 help: '?' reprints long help (x$cnt4)"; else bad "4 help: '?' did not reprint (x$cnt4, want >=2)"; fi
assert_ngrep "$out4" "not valid for choice" "4 help: '?' is not treated as an invalid value"

# --- 5. chunk-input flagging (host-independent) ------------------------------
# CHUNK_GOROOT defaults ON; a bogus GOFORK makes its input absent -> the wizard
# must name the forage remedy. Negative: the same absent input must NOT be flagged
# when the chunk is OFF.
out5="$(GOFORK=/nonexistent-thyla-test BC_DIR_CONFIGS="$TMP" "$CONFIGURE" --defaults g5 2>&1)"
assert_grep "$out5" "forage\.sh go" "5 flag: absent CHUNK_GOROOT input names the remedy"
printf 'CHUNK_GOROOT=n\n' > "$TMP/nogo.config"
out5n="$(GOFORK=/nonexistent-thyla-test BC_DIR_CONFIGS="$TMP" "$CONFIGURE" --defaults --edit nogo g5n 2>&1)"
assert_ngrep "$out5n" "forage\.sh go" "5 flag: OFF chunk with absent input is not flagged"

# --- 6. usage contract: --defaults needs a name ------------------------------
if run --defaults >/dev/null 2>&1; then bad "6 usage: --defaults with no name should exit nonzero"; else ok "6 usage: --defaults with no name is rejected"; fi

# --- 7. unknown preset is rejected -------------------------------------------
if run --defaults --from nosuchpreset zzz >/dev/null 2>&1; then bad "7 usage: unknown --from preset should exit nonzero"; else ok "7 usage: unknown --from preset is rejected"; fi

# --- 8. --edit loads an existing profile as the seed -------------------------
# seed1 (from case 1) carries KASLR=y; editing it and accepting all must preserve
# that. A wizard that ignored --edit would write bare defaults (KASLR=n).
run --defaults --edit seed1 edit8 >/dev/null 2>&1
assert_eq "$(cfg_val "$TMP/edit8.config" KASLR)" y "8 edit: --edit loads the profile (KASLR=y preserved)"

# --- 9. isolation: the real configs/ is untouched by any run above -----------
real_after="$(ls "$REPO_ROOT"/configs/ | sort)"
assert_eq "$real_after" "$real_before" "9 isolation: real configs/ unchanged (BC_DIR_CONFIGS honored)"

# --- 10. the theme picker DISCOVERS the directory ---------------------------
# The property worth pinning is not "aero/nightjar are offered" -- that is true
# of a hard-coded list too, and would pass while the feature was broken. Point
# the discovery at a temp dir holding a theme that exists NOWHERE in the tree:
# it can only appear in the menu if the directory is actually read.
THEMES="$TMP/themes"; mkdir -p "$THEMES"
printf '[meta]\nname = "Zzz Invented"\n' > "$THEMES/zzz-invented.toml"
menu="$(yes '' | BC_DIR_CONFIGS="$TMP" WZ_DIR_THEMES="$THEMES" "$CONFIGURE" \
          --from dev --name themedisc 2>&1 || true)"
case "$menu" in
    *"zzz-invented"*) assert_eq "found" "found" "10 theme menu READS the directory (invented theme offered)" ;;
    *)                assert_eq "missing" "found" "10 theme menu READS the directory (invented theme offered)" ;;
esac
# ... and shows the theme's OWN [meta] name beside it, so the operator does not
# have to open each file to learn what it is.
case "$menu" in
    *"Zzz Invented"*) assert_eq "found" "found" "11 theme menu labels each entry with its [meta] name" ;;
    *)                assert_eq "missing" "found" "11 theme menu labels each entry with its [meta] name" ;;
esac
# CONTROL, one variable away: with the directory EMPTY the same run must offer
# no themes at all. Without this, a menu that printed every *.toml on the host
# -- or ignored the override entirely -- would still pass case 10.
EMPTY="$TMP/themes-empty"; mkdir -p "$EMPTY"
menu2="$(yes '' | BC_DIR_CONFIGS="$TMP" WZ_DIR_THEMES="$EMPTY" "$CONFIGURE" \
           --from dev --name themeempty 2>&1 || true)"
case "$menu2" in
    *"zzz-invented"*|*"nightjar"*)
        assert_eq "leaked" "clean" "12 CONTROL: an empty theme dir offers nothing" ;;
    *)  assert_eq "clean" "clean" "12 CONTROL: an empty theme dir offers nothing" ;;
esac

# --- 13. each theme is tagged with the SCHEMA its own file declares ----------
# Two invented themes one line apart: only the file's [meta] profile line can
# tell them apart, so a tag computed from anything else (the name, a list in
# the script) fails one of the two. TEMPLATE.toml is an authoring skeleton and
# must not be offered at all.
SCH="$TMP/themes-schema"; mkdir -p "$SCH"
printf '[meta]\nname = "Inv Instrument"\nprofile = "instrument-v1"\n' > "$SCH/inv-instr.toml"
printf '[meta]\nname = "Inv Legacy"\n'                                > "$SCH/inv-legacy.toml"
printf '[meta]\nname = "Skeleton"\n'                                  > "$SCH/TEMPLATE.toml"
menu3="$(yes '' | BC_DIR_CONFIGS="$TMP" WZ_DIR_THEMES="$SCH" "$CONFIGURE" \
           --from dev --name themeschema 2>&1 || true)"
tag_of() { printf '%s\n' "$menu3" | grep -- "$1" | head -1 | sed -E 's/.*\[([a-z]+) *\].*/\1/'; }
assert_eq "$(tag_of inv-instr)"  "instrument" "13 theme menu tags an Instrument-schema file"
assert_eq "$(tag_of inv-legacy)" "legacy"     "13 theme menu tags a legacy-schema file"
case "$menu3" in
    *TEMPLATE*) assert_eq "offered" "hidden" "13 the authoring TEMPLATE is not offered as a theme" ;;
    *)          assert_eq "hidden"  "hidden" "13 the authoring TEMPLATE is not offered as a theme" ;;
esac
# --- 14. a cross-schema pairing is SAID, and a matching one is not ----------
# Seeded profiles, one variable apart: the same instrument profile with a
# legacy-schema theme and with an Instrument-schema one. A note that always
# prints is as useless as one that never does, so the control is the point.
printf 'HALCYON_PROFILE=instrument\nHALCYON_THEME=inv-legacy\n' > "$TMP/pairmis.config"
printf 'HALCYON_PROFILE=instrument\nHALCYON_THEME=inv-instr\n'  > "$TMP/pairok.config"
note_mis="$(BC_DIR_CONFIGS="$TMP" WZ_DIR_THEMES="$SCH" "$CONFIGURE" --defaults --edit pairmis pairmis-out 2>&1 || true)"
note_ok="$(BC_DIR_CONFIGS="$TMP" WZ_DIR_THEMES="$SCH" "$CONFIGURE" --defaults --edit pairok pairok-out 2>&1 || true)"
case "$note_mis" in
    *PROJECTED*) assert_eq "said" "said" "14 a legacy theme under the instrument profile is called out" ;;
    *)           assert_eq "silent" "said" "14 a legacy theme under the instrument profile is called out" ;;
esac
case "$note_ok" in
    *PROJECTED*) assert_eq "said" "silent" "14 CONTROL: a matching pairing prints no projection note" ;;
    *)           assert_eq "silent" "silent" "14 CONTROL: a matching pairing prints no projection note" ;;
esac
assert_eq "$(cfg_val "$TMP/pairmis-out.config" HALCYON_PROFILE)" instrument "14 the profile option round-trips through a written profile"

echo "== $pass passed, $fail failed =="
[[ "$fail" -eq 0 ]]
