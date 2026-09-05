#!/usr/bin/env bash
# tools/configure.sh -- the Thylacine build wizard (guided profile creation).
#
# A host-side, interactive, LINEAR Q&A for a newcomer who knows nothing about
# Thylacine: pick a base, then walk every option grouped, each with its name +
# description + what-it-enables help, and write a named profile you then build
# with `tools/build.sh --config <name>`. Design: docs/BUILD-CONFIG-DESIGN.md 4.6.
#
# This is pure ergonomics OVER the schema + presets in tools/build-config.sh --
# it drives bc_reset/bc_apply_preset/bc_set_one/bc_resolve/bc_emit_config and
# introduces NO config semantics of its own. Named the plain `configure` on
# purpose (4.6): the standard-name-wins discipline, since the audience does not
# know the project's identity.
#
# bash 3.2 SAFE (macOS /usr/bin/env bash is 3.2.57): no associative arrays, no
# mapfile, no ${var,,}. NOT `set -e` -- interactive read-loops + the schema's
# conditional bc_* returns make it a footgun; reads guard EOF explicitly.
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
GOFORK="${GOFORK:-$HOME/projects/go-thylacine}"

# The wizard reads the schema + presets from the same core build.sh uses.
# Overridable so tools/test-configure.sh can isolate reads AND writes to a temp
# dir and never touch the real configs/ (its default is the real one).
BC_DIR_CONFIGS="${BC_DIR_CONFIGS:-$REPO_ROOT/configs}"
# shellcheck disable=SC1090
. "$REPO_ROOT/tools/build-config.sh"

usage() {
    cat <<'EOF'
tools/configure.sh -- guided Thylacine build-profile creation.

  configure.sh                     interactive: pick a base, walk every option,
                                   name + write configs/<name>.config
  configure.sh --from <preset>     seed from a preset, then walk interactively
  configure.sh --edit <name>       revisit an existing configs/<name>.config
  configure.sh --defaults <name>   non-interactive: accept the seed as-is, write
                                   configs/<name>.config (combine with --from)
  configure.sh --name <name>       preset the profile name (interactive default)
  configure.sh --help

Presets: production | dev | everything | custom (custom = defaults, walk all).
The written profile builds with: tools/build.sh --config <name>
EOF
}

# --- human labels over the schema's terse group keys ------------------------
wz_group_title() {
    case "$1" in
        compile) echo "Compile-time shape -- what the kernel IS" ;;
        bake)    echo "Bake content -- what ships IN the image" ;;
        pool)    echo "Pool / disk control" ;;
        *)       echo "$1" ;;
    esac
}

wz_type_hint() {
    case "$1" in
        bool)     echo "y/n" ;;
        choice:*) printf '%s' "${1#choice:}" | tr ',' '/' ;;
        string)   echo "text" ;;
        *)        echo "$1" ;;
    esac
}

# --- live constraints (4.6 step 3) ------------------------------------------
# MVP has exactly one: BOOT_PROBES=y implies DEV_ACCOUNTS=y (the boot-test ladder
# authenticates the dev accounts, so it cannot run without them). Mirrors the
# authoritative enforcement in bc_resolve -- the wizard announces + pins it live
# so the newcomer sees WHY, rather than being silently overridden at resolve.
wz_forced_value() {
    case "$1" in
        DEV_ACCOUNTS) [[ "$(bc_get BOOT_PROBES)" == y ]] && echo y ;;
    esac
    return 0
}
wz_forced_reason() {
    case "$1" in
        DEV_ACCOUNTS) echo "BOOT_PROBES=y requires it" ;;
    esac
}
wz_after_set() {
    # Announce the implication whenever BOOT_PROBES is SELECTED y -- not only when
    # DEV_ACCOUNTS happened to be off (its default is y, so the guarded form would
    # stay silent in a from-defaults walk, hiding the constraint the newcomer needs
    # to see). The pin at the DEV_ACCOUNTS prompt reinforces it; bc_resolve is the
    # authoritative enforcer either way.
    if [[ "$1" == BOOT_PROBES && "$(bc_get BOOT_PROBES)" == y ]]; then
        [[ "$(bc_get DEV_ACCOUNTS)" == n ]] && bc__set_raw DEV_ACCOUNTS y
        printf '    -> enables DEV_ACCOUNTS (the boot-test ladder logs in; required).\n'
    fi
}

# --- chunk-input flagging (4.6 step 4) --------------------------------------
# If an ON bake chunk's input is absent on this host, say so and name the remedy
# BEFORE the build fails on it. The forage collector is the ratified remedy
# (section 5.3, next lane); the concrete manual fallback works today. Non-fatal.
wz_chunk_absent() {   # NAME -> 0 (input absent) / 1 (present or no external input)
    case "$1" in
        CHUNK_GOROOT) [[ -x "$GOFORK/bin/go" ]] && return 1 || return 0 ;;
        CHUNK_CLADE)  [[ -d "$REPO_ROOT/build/clade/stage/bin" ]] && return 1 || return 0 ;;
        CHUNK_ALPINE)
            local t b
            t="$(ls "$REPO_ROOT/build/cache"/alpine-minirootfs-*-aarch64.tar.gz 2>/dev/null | head -1)"
            b="$(ls "$REPO_ROOT/build/cache"/busybox-static-*.apk 2>/dev/null | head -1)"
            [[ -n "$t" && -n "$b" ]] && return 1 || return 0 ;;
        *) return 1 ;;
    esac
}
wz_chunk_remedy() {
    case "$1" in
        CHUNK_GOROOT) echo "tools/forage.sh go       (or set GOFORK=/path/to/go-thylacine)" ;;
        CHUNK_CLADE)  echo "tools/forage.sh clade    (or tools/clade-keep-build.sh)" ;;
        CHUNK_ALPINE) echo "tools/forage.sh alpine   (drop the minirootfs + busybox-static apk in build/cache/)" ;;
    esac
}
wz_flag_absent_chunks() {   # -> prints warnings, returns 0 always
    local i n any=0
    for i in "${!BC_NAME[@]}"; do
        n="${BC_NAME[$i]}"
        [[ "${BC_GROUP[$i]}" == bake ]] || continue
        [[ "$(bc_get "$n")" == y ]] || continue
        if wz_chunk_absent "$n"; then
            if [[ "$any" == 0 ]]; then
                printf '\n! Some selected chunks need inputs that are ABSENT on this host:\n'; any=1
            fi
            printf '    %-16s remedy: %s\n' "$n" "$(wz_chunk_remedy "$n")"
        fi
    done
    [[ "$any" == 1 ]] && printf '    (or turn the chunk off: --set %s=n)\n' "CHUNK_X"
    return 0
}

# --- the interactive pieces --------------------------------------------------
wz_pick_preset() {
    printf '\nStart from which base?\n'
    printf '  [1] production   lean, hardened, release, loginnable (the v1.0 shape)\n'
    printf '  [2] dev          debug, loginnable, Go runtime, no tests\n'
    printf '  [3] everything   every bake chunk on, no boot tests\n'
    printf '  [4] custom       start from defaults and walk every option\n'
    local ans
    while true; do
        printf 'Base [1]: '; ans=""; IFS= read -r ans || true
        [[ -z "$ans" ]] && ans=1
        case "$ans" in
            1|production) bc_apply_preset production; return ;;
            2|dev)        bc_apply_preset dev;        return ;;
            3|everything) bc_apply_preset everything; return ;;
            4|custom)     bc_reset;                   return ;;
            *)            printf '  -- pick 1-4.\n' ;;
        esac
    done
}

wz_walk() {
    local i n type grp last="" cur ans forced
    printf '\nEnter = keep the [default].  Type a value to change it.  ? = full help.\n'
    for i in "${!BC_NAME[@]}"; do
        n="${BC_NAME[$i]}"; type="${BC_TYPE[$i]}"; grp="${BC_GROUP[$i]}"
        if [[ "$grp" != "$last" ]]; then
            printf '\n==== %s ====\n' "$(wz_group_title "$grp")"; last="$grp"
        fi
        forced="$(wz_forced_value "$n")"
        if [[ -n "$forced" ]]; then
            bc__set_raw "$n" "$forced"
            printf '\n  %s = %s  (pinned: %s)\n' "$n" "$forced" "$(wz_forced_reason "$n")"
            continue
        fi
        cur="$(bc_get "$n")"
        printf '\n  %s -- %s\n' "$n" "${BC_DESC[$i]}"
        printf '    %s\n' "${BC_HELP[$i]}"
        while true; do
            printf '  %s [%s] (%s, ? help): ' "$n" "$cur" "$(wz_type_hint "$type")"
            ans=""; IFS= read -r ans || true
            [[ -z "$ans" ]] && break
            if [[ "$ans" == '?' ]]; then printf '\n    %s\n\n' "${BC_HELP[$i]}"; continue; fi
            if bc_set_one "$n" "$ans"; then wz_after_set "$n"; break; fi
            printf '    -- not valid for %s; try again.\n' "$type"
        done
    done
}

wz_summary() {
    local i n grp last="" val def mark
    printf '\n=== your profile ===\n'
    for i in "${!BC_NAME[@]}"; do
        n="${BC_NAME[$i]}"; grp="${BC_GROUP[$i]}"; val="$(bc_get "$n")"; def="${BC_DEFAULT[$i]}"
        if [[ "$grp" != "$last" ]]; then printf '\n  [%s]\n' "$(wz_group_title "$grp")"; last="$grp"; fi
        mark=" "; [[ "$val" != "$def" ]] && mark="*"
        printf '   %s %-16s %s\n' "$mark" "$n" "$val"
    done
    printf '\n  (* = changed from the built-in default)\n'
}

# wz_write NAME [interactive] -> emit configs/NAME.config + print the build command.
wz_write() {
    local name="$1" interactive="${2:-0}" path
    name="$(printf '%s' "$name" | tr -cd 'A-Za-z0-9_-')"
    [[ -n "$name" ]] || name="custom"
    path="$BC_DIR_CONFIGS/$name.config"
    if [[ -e "$path" ]]; then
        case "$name" in
            production|dev|everything|default|ci)
                printf '! configs/%s.config is a SHIPPED preset -- overwriting it.\n' "$name" >&2 ;;
        esac
        if [[ "$interactive" == 1 ]]; then
            local ow=""
            printf 'configs/%s.config exists. Overwrite? [y/N]: ' "$name"; IFS= read -r ow || true
            case "$ow" in y|Y|yes) ;; *) printf 'Aborted; nothing written.\n'; return 1 ;; esac
        fi
    fi
    bc_emit_config "$path"
    printf '\nWrote configs/%s.config\n' "$name"
    printf 'Build it with:\n    tools/build.sh --config %s\n' "$name"
}

# --- argument parsing --------------------------------------------------------
from_preset=""; edit_name=""; accept_defaults=0; out_name=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --from=*)     from_preset="${1#--from=}"; shift ;;
        --from)       from_preset="${2:-}"; shift 2 ;;
        --edit=*)     edit_name="${1#--edit=}"; shift ;;
        --edit)       edit_name="${2:-}"; shift 2 ;;
        --defaults)   accept_defaults=1; shift ;;
        --name=*)     out_name="${1#--name=}"; shift ;;
        --name)       out_name="${2:-}"; shift 2 ;;
        --help|-h)    usage; exit 0 ;;
        -*)           echo "configure: unknown option: $1" >&2; usage >&2; exit 2 ;;
        *)            [[ -z "$out_name" ]] && out_name="$1" || { echo "configure: unexpected argument: $1" >&2; exit 2; }; shift ;;
    esac
done

# --- seed --------------------------------------------------------------------
bc_reset
if [[ -n "$edit_name" ]]; then
    if ! bc_load_file "$BC_DIR_CONFIGS/$edit_name.config"; then
        echo "configure: no such profile to edit: $BC_DIR_CONFIGS/$edit_name.config" >&2; exit 2
    fi
    [[ -z "$out_name" ]] && out_name="$edit_name"
elif [[ -n "$from_preset" ]]; then
    if ! bc_apply_preset "$from_preset"; then
        echo "configure: no such preset: $from_preset" >&2; exit 2
    fi
fi

# --- non-interactive (--defaults): accept the seed, resolve, write -----------
if [[ "$accept_defaults" == 1 ]]; then
    if [[ -z "$out_name" ]]; then
        echo "configure: --defaults needs a profile name (positional or --name X)" >&2; exit 2
    fi
    bc_resolve || { echo "configure: the seed resolved to an invalid config" >&2; exit 1; }
    wz_flag_absent_chunks
    wz_write "$out_name" 0
    exit 0
fi

# --- interactive -------------------------------------------------------------
printf '=== Thylacine build configurator ===\n'
printf 'Answer a few questions; I write a build profile you then feed to build.sh.\n'
# If no seed flag was given, let the newcomer pick a base first (step 1).
if [[ -z "$from_preset" && -z "$edit_name" ]]; then
    wz_pick_preset
else
    printf '\nStarting from %s.\n' "${edit_name:+profile $edit_name}${from_preset:+preset $from_preset}"
fi
wz_walk
bc_resolve || { echo "configure: resolved to an invalid config (should not happen)" >&2; exit 1; }
wz_summary
wz_flag_absent_chunks
printf '\nWrite this profile? [Y/n]: '; confirm=""; IFS= read -r confirm || true
case "$confirm" in n|N|no) printf 'Aborted; nothing written.\n'; exit 0 ;; esac
printf 'Save as profile name [%s]: ' "${out_name:-custom}"; nm=""; IFS= read -r nm || true
[[ -z "$nm" ]] && nm="${out_name:-custom}"
wz_write "$nm" 1
