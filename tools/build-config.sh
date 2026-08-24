# tools/build-config.sh -- the Thylacine build configurator core.
#
# The single typed config artifact + orthogonal axes + preset/fragment merge that
# replaces build.sh's overlapping flag-bundles and scattered THYLACINE_* env vars.
# Design: docs/BUILD-CONFIG-DESIGN.md (Buildroot/Kconfig-lite). Sourced by build.sh;
# also read by tools/configure.sh (the wizard), tools/test-build-config.sh, and
# tools/test-configure.sh.
#
# bash 3.2 SAFE (macOS /usr/bin/env bash is 3.2.57): no associative arrays, no
# mapfile, no ${var,,}. The schema is parallel INDEXED arrays; config values are
# held in per-symbol CFG_<NAME> variables (printf -v + ${!indirect}).
#
# Precedence is call ORDER (last writer wins), which build.sh drives as:
#   bc_reset            # built-in defaults
#   bc_apply_preset X   # < a preset file
#   bc_apply_fragment Y # < fragment overlays, in order
#   bc_set K=V          # < explicit CLI
#   bc_resolve          # implies-constraints + final validation
#   bc_export           # map resolved symbols onto build.sh's knobs

# --- the schema (parallel indexed arrays) ------------------------------------
BC_GROUP=(); BC_NAME=(); BC_TYPE=(); BC_DEFAULT=(); BC_MAP=(); BC_DESC=(); BC_HELP=()

# bc_def GROUP NAME TYPE DEFAULT MAP DESC HELP
#   TYPE  = bool | choice:<a,b,...> | string
#   MAP   = how bc_export threads the symbol into build.sh:
#           var:<name>       -> shell var, y/n -> ON/OFF (bool) or raw (string/choice)
#           varinv:<name>    -> shell var, y/n -> OFF/ON  (inverted bool)
#           buildtype:<name> -> shell var, debug/release -> Debug/Release
#           sanitize:<name>  -> shell var, none -> "" else the value
#           def:<NAME>       -> a -DNAME=ON/OFF CMake define (via extra_cmake_args)
#           env:<NAME>       -> exported env var, y/n -> 1/0 (bool) or raw (string)
#           want:<NAME>      -> exported want-flag (1 when y); orchestration deferred
#                               to the manifest/wiring sub-chunk (no clean toggle today)
bc_def() {
    BC_GROUP+=("$1"); BC_NAME+=("$2"); BC_TYPE+=("$3"); BC_DEFAULT+=("$4")
    BC_MAP+=("$5"); BC_DESC+=("$6"); BC_HELP+=("$7")
}

# compile-shape ---------------------------------------------------------------
bc_def compile BUILD_TYPE "choice:debug,release" debug "buildtype:build_type" \
  "Kernel build type" \
  "debug keeps assertions + symbols (the dev default); release is -O2 with assertions off. Debug is what you want unless you are measuring performance."
bc_def compile TESTS bool n "var:kernel_tests" \
  "In-kernel test suite" \
  "Compiles and runs the kernel's built-in unit tests at boot (KERNEL_TESTS). On for CI; off for a normal image -- it just adds boot time otherwise."
bc_def compile BOOT_PROBES bool n "var:boot_probes" \
  "Boot-test probe ladder" \
  "joey's boot-time self-test E2Es (login, recover, on-device toolchain, ...). On for CI/regression; off for a normal image. Requires DEV_ACCOUNTS (the probes log in), so turning this on turns that on too."
bc_def compile DEV_ACCOUNTS bool y "var:dev_accounts" \
  "Bake a dev login account" \
  "Provisions the daily-use login accounts (michael + cora) at first boot so a lean image is loginnable -- without it, --production has no accounts and you cannot log in (the finding-#1 fix; before this axis, accounts rode BOOT_PROBES). The full michael/susan/cora/wheel fixture set is created only when the boot-probe ladder (BOOT_PROBES) also runs. Turn OFF only for a bare image an installer/first-boot flow will provision."
bc_def compile HARDENING_FULL bool n "var:hardening_full" \
  "Full P1-H hardening" \
  "Enables the full hardening flag set (PAC/BTI where the CPU has them, plus extra guards). A small size/complexity cost; on for production-like images."
bc_def compile KASLR bool n "var:kaslr" \
  "Kernel ASLR" \
  "Randomizes the kernel base address at each boot (invariant I-16). On for production-like images; off makes crash addresses stable for debugging."
bc_def compile SANITIZE "choice:none,ubsan" none "sanitize:sanitize" \
  "Kernel sanitizer" \
  "ubsan = UndefinedBehaviorSanitizer (trapping); builds into a separate dir so it never clobbers the normal kernel. none for everything else."
bc_def compile TICKLESS bool y "varinv:no_tickless" \
  "Tickless idle" \
  "NO_HZ_IDLE: an idle CPU sleeps without a periodic timer tick. Leave on. Off forces the old 1 kHz-always tick and is a diagnostic-only baseline."

# bake-content ----------------------------------------------------------------
bc_def bake CHUNK_GOROOT bool y "env:THYLACINE_BAKE_GOROOT" \
  "Go runtime (/goroot)" \
  "Bakes a trimmed Go GOROOT (~167 MB) so 'go build' works on-device. Needs the go-thylacine fork staged; if it is absent the chunk is skipped (run 'forage')."
bc_def bake CHUNK_CLADE bool n "env:THYLACINE_BAKE_CLADE" \
  "Clade toolchain (/clade + /storm)" \
  "The on-device LLVM/Clang/lld toolchain (~1.3 GB, a slow first build) -- compile C and C++ ON Thylacine. Also stages /storm. Needs a staged build/clade; absent -> skipped (run 'forage')."
bc_def bake CHUNK_CHASE_W2 bool n "env:THYLACINE_CHASE_W2" \
  "Chase-W2 bench marker (/chase-w2)" \
  "A marker that gates joey's heavy on-device compile benchmark. Dev/bench only."
bc_def bake CHUNK_ALPINE bool n "want:THYLACINE_WANT_ALPINE" \
  "Alpine bundle (VIVARIUM)" \
  "An Alpine minirootfs + busybox bundle for the VIVARIUM Linux-binary phenotype. Needs the two cache inputs (alpine rootfs + busybox apk); the manifest/forage step resolves their paths."
bc_def bake CHUNK_QUAKE bool n "want:THYLACINE_WANT_QUAKE" \
  "Quake (tyrquake, /quake)" \
  "Bakes the tyrquake port + the shareware pak (fetched from the network). A demo/fun chunk; the build runs the tyrquake stage when this is on."
bc_def bake CHUNK_AURORA_CFG bool n "env:THYLACINE_AURORA_CFG4" \
  "Aurora config-4 payload" \
  "An opt-in Aurora renderer config-4 payload."

# pool-control ----------------------------------------------------------------
bc_def pool DISK_SIZE string 16M "env:THYLACINE_DISK_SIZE" \
  "Scratch disk size" \
  "Size of the scratch disk.img (NOT the Stratum pool). Default 16M; rarely changed."
bc_def pool MKFS_SEED string "" "env:THYLACINE_MKFS_SEED" \
  "mkfs RNG seed" \
  "Pin the pool mkfs RNG seed to make pool.img reproducible. Empty = a fresh random seed each pool bake (which also re-keys ramfs -- keep the paired set together)."
bc_def pool MKFS_PRESERVE bool n "env:THYLACINE_MKFS_PRESERVE" \
  "Preserve pool content" \
  "Skip re-populating the pool: reuse the existing pool.img + system.key. Fast rebuilds when only the kernel/userspace changed, not the pool corpus."

# --- schema access -----------------------------------------------------------
bc_count() { printf '%s' "${#BC_NAME[@]}"; }

# bc_index_of NAME -> echoes the index, or nothing + returns 1 if unknown.
bc_index_of() {
    local want="$1" i
    for i in "${!BC_NAME[@]}"; do
        if [[ "${BC_NAME[$i]}" == "$want" ]]; then printf '%s' "$i"; return 0; fi
    done
    return 1
}

# --- config values (per-symbol CFG_<NAME> variables) -------------------------
bc_get() { local _n="CFG_$1"; printf '%s' "${!_n-}"; }
bc__set_raw() { printf -v "CFG_$1" '%s' "$2"; }

# bc_validate TYPE VALUE -> 0 if VALUE is legal for TYPE, else 1.
bc_validate() {
    local type="$1" val="$2"
    case "$type" in
        bool) [[ "$val" == y || "$val" == n ]] ;;
        string) return 0 ;;
        choice:*)
            local opts="${type#choice:}" o
            local IFS=,
            for o in $opts; do [[ "$val" == "$o" ]] && return 0; done
            return 1 ;;
        *) return 1 ;;
    esac
}

# bc_set_one NAME VALUE -> validate against the schema, then set. Warns + returns
# 1 on an unknown symbol or an illegal value (the caller decides fatality).
bc_set_one() {
    local name="$1" val="$2" idx
    if ! idx="$(bc_index_of "$name")"; then
        echo "build-config: unknown symbol '$name' (ignored)" >&2; return 1
    fi
    if ! bc_validate "${BC_TYPE[$idx]}" "$val"; then
        echo "build-config: '$val' is not valid for $name (${BC_TYPE[$idx]})" >&2; return 1
    fi
    bc__set_raw "$name" "$val"
}

# bc_set KEY=VALUE (the CLI --set form).
bc_set() {
    local kv="$1"
    [[ "$kv" == *=* ]] || { echo "build-config: --set expects KEY=VALUE, got '$kv'" >&2; return 1; }
    bc_set_one "${kv%%=*}" "${kv#*=}"
}

# bc_reset -> every symbol to its schema default.
bc_reset() {
    local i
    for i in "${!BC_NAME[@]}"; do bc__set_raw "${BC_NAME[$i]}" "${BC_DEFAULT[$i]}"; done
}

# bc_load_file PATH -> apply KEY=value lines (comments + blanks ignored). Unknown
# symbols / bad values warn but do not abort the file (forward-compat), matching
# Kconfig's tolerance of a superset .config.
bc_load_file() {
    local path="$1" line key val
    [[ -f "$path" ]] || { echo "build-config: no such config file: $path" >&2; return 1; }
    while IFS= read -r line || [[ -n "$line" ]]; do
        line="${line%%#*}"                       # strip trailing comment
        line="${line#"${line%%[![:space:]]*}"}"  # ltrim
        line="${line%"${line##*[![:space:]]}"}"  # rtrim
        [[ -z "$line" ]] && continue
        [[ "$line" == *=* ]] || { echo "build-config: $path: ignoring '$line' (no =)" >&2; continue; }
        key="${line%%=*}"; val="${line#*=}"
        key="${key%"${key##*[![:space:]]}"}"     # rtrim key
        val="${val#"${val%%[![:space:]]*}"}"     # ltrim val
        bc_set_one "$key" "$val" || true
    done < "$path"
}

BC_DIR_CONFIGS="${BC_DIR_CONFIGS:-configs}"
bc_apply_preset()   { bc_load_file "$BC_DIR_CONFIGS/$1.config"; }
bc_apply_fragment() { bc_load_file "$BC_DIR_CONFIGS/fragments/$1.config"; }

# bc_resolve -> enforce the implies-constraints, then a final validate. MVP has one
# constraint (BOOT_PROBES implies DEV_ACCOUNTS): the boot-test ladder authenticates
# the dev accounts, so it cannot run without them. Auto-raise + warn (never silently
# produce an image whose CI probes would deadlock on a missing login).
bc_resolve() {
    if [[ "$(bc_get BOOT_PROBES)" == y && "$(bc_get DEV_ACCOUNTS)" == n ]]; then
        echo "build-config: BOOT_PROBES=y requires DEV_ACCOUNTS -- raising DEV_ACCOUNTS=y" >&2
        bc__set_raw DEV_ACCOUNTS y
    fi
    local i n v
    for i in "${!BC_NAME[@]}"; do
        n="${BC_NAME[$i]}"; v="$(bc_get "$n")"
        bc_validate "${BC_TYPE[$i]}" "$v" || { echo "build-config: resolved $n='$v' is invalid" >&2; return 1; }
    done
}

# bc_emit_config PATH -> write the resolved KEY=value artifact (build/.config): the
# one-line-per-symbol answer to "what is this image?", grouped + commented.
bc_emit_config() {
    local path="$1" i grp last_grp="" name
    { echo "# Thylacine build config -- generated; edit via tools/configure.sh or --set."
      echo "# See docs/BUILD-CONFIG-DESIGN.md."
      for i in "${!BC_NAME[@]}"; do
          grp="${BC_GROUP[$i]}"; name="${BC_NAME[$i]}"
          if [[ "$grp" != "$last_grp" ]]; then echo; echo "# [$grp]"; last_grp="$grp"; fi
          printf '%-16s = %-8s # %s\n' "$name" "$(bc_get "$name")" "${BC_DESC[$i]}"
      done
    } > "$path"
}

# bc_export -> thread every resolved symbol onto build.sh's existing knobs. Sets the
# shell vars build.sh's kernel stage reads, appends the DEV_ACCOUNTS CMake define to
# extra_cmake_args, and exports the bake env vars the later stages read. This is the
# ONE translation point from the clean config model to the heterogeneous as-built
# knobs. Call from build.sh after bc_resolve.
bc_export() {
    local i name map kind target val
    for i in "${!BC_NAME[@]}"; do
        name="${BC_NAME[$i]}"; map="${BC_MAP[$i]}"; val="$(bc_get "$name")"
        kind="${map%%:*}"; target="${map#*:}"
        case "$kind" in
            var)       printf -v "$target" '%s' "$(bc__onoff "$val")" ;;
            varinv)    printf -v "$target" '%s' "$(bc__onoff_inv "$val")" ;;
            buildtype) printf -v "$target" '%s' "$([[ "$val" == release ]] && echo Release || echo Debug)" ;;
            sanitize)  printf -v "$target" '%s' "$([[ "$val" == none ]] && echo "" || echo "$val")" ;;
            def)       extra_cmake_args+=("-D${target}=$(bc__onoff "$val")") ;;
            env)       bc__export_env "$target" "$(bc__envval "$val")" ;;
            want)      [[ "$val" == y ]] && export "$target"=1 || export "$target"=0 ;;
        esac
    done
}
# y/n bools map to build.sh's ON/OFF shell/CMake convention; everything else is raw.
bc__onoff()     { case "$1" in y) echo ON ;; n) echo OFF ;; *) echo "$1" ;; esac; }
bc__onoff_inv() { case "$1" in y) echo OFF ;; n) echo ON ;; *) echo "$1" ;; esac; }
# env bake vars use 1/0 for bools; a string (DISK_SIZE, MKFS_SEED) passes through.
bc__envval()    { case "$1" in y) echo 1 ;; n) echo 0 ;; *) echo "$1" ;; esac; }
# D-b transition shim: if the caller already set a legacy THYLACINE_* env var, HONOR
# it (do not clobber); otherwise export the config's value. So build-everything.sh's
# pre-set THYLACINE_BAKE_* (e.g. its clade-staged detection) still wins during the
# transition. eval-based existence test for bash 3.2 (no ${!name+x} composition).
bc__export_env() {
    local name="$1" cfgval="$2" isset
    eval "isset=\"\${$name+set}\""
    [[ "$isset" == set ]] || eval "export $name=\"\$cfgval\""
}
# --sanitize=undefined is the legacy spelling of ubsan (both -> the schema's ubsan).
bc__san_alias() { case "$1" in undefined) echo ubsan ;; *) echo "$1" ;; esac; }

# bc_show -> print the resolved config to stdout (the --show-config form).
bc_show() {
    local i name
    for i in "${!BC_NAME[@]}"; do name="${BC_NAME[$i]}"; printf '%-16s %s\n' "$name" "$(bc_get "$name")"; done
}
