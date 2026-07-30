#!/usr/bin/env bash
# A CROSS llvm-config for aarch64-thylacine (Clade CL-7a; LLVM-DESIGN section 16.19).
#
# WHY THIS EXISTS. meson -- like autotools and cmake before it -- discovers LLVM by
# RUNNING llvm-config and believing what it says. A cross build has no runnable
# llvm-config for the target: the cross tree's own bin/llvm-config is an
# aarch64-thylacine static binary that the builder cannot execute, and the fork's
# HOST llvm-config runs fine but answers with the HOST tree's paths, version and
# target facts. Handing a cross configure either of those produces a build that
# looks configured and links the wrong LLVM. A shim is the standard technique.
#
# THE SPLIT. Two kinds of question arrive on this argv, and they have different
# authorities:
#
#   * the component GRAPH ("which archives does 'orcjit' pull in?") is a property
#     of the LLVM VERSION, identical in both trees because both are built from the
#     same source -- so it is delegated to the host tool, which is the real
#     implementation and cannot drift from LLVM's own dependency tables.
#   * every PATH and every TARGET fact (version, triples, targets built, RTTI,
#     build mode, shared-vs-static) is a property of the CROSS TREE -- so it is
#     answered from that tree's own generated headers and CMakeCache, never
#     hardcoded here and never delegated. Hardcoding would let the shim keep
#     claiming RTTI=ON after someone flips the flag back off.
#
# THREE DELIBERATE BEHAVIOURS, each earned the hard way (see LLVM-DESIGN 16.19):
#
#   1. Every archive this shim names in a --libs answer is CHECKED to exist, and a
#      miss is a loud FATAL naming the missing files. Mesa's meson reports any
#      llvm-config error as the flat "dependency LLVM found: NO", so without this
#      the diagnosis is a guess. The shim's own stderr and log carry the truth.
#   2. --shared-mode answers "static" WITHOUT enumerating every component. The real
#      llvm-config walks all ~130 components for that query and errors on the first
#      absent archive, which is why a partially-built LLVM is rejected wholesale
#      even when every requested module resolves. "static" is true here by
#      construction (build_clade sets BUILD_SHARED_LIBS=OFF, LLVM_ENABLE_PIC=OFF,
#      LLVM_LINK_LLVM_DYLIB=OFF), and check (1) covers the set that actually gets
#      linked -- so the answer is honest and the completeness check moves to where
#      it is load-bearing instead of forcing a build of 130 unused archives.
#   3. Unknown queries are delegated, path-rewritten, and logged as UNHANDLED
#      rather than silently answered. An unknown query answered confidently is the
#      failure shape this whole file is defending against.
#
# Log every invocation with CLADE_LLVM_CONFIG_LOG=/path -- that turns "what does
# the consumer actually ask for" from inference into data.

set -euo pipefail

repo_root() { cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd; }
ROOT="$(repo_root)"

# The cross tree (build_clade's $bdir).
TARGET_TREE="${CLADE_LLVM_TARGET_TREE:-$ROOT/build/clade/llvm-build}"
LOG="${CLADE_LLVM_CONFIG_LOG:-}"

die() {
    echo "clade-llvm-config-cross: FATAL: $*" >&2
    [[ -n "$LOG" ]] && echo "FATAL: $*" >> "$LOG"
    exit 1
}

log() { [[ -n "$LOG" ]] && printf '%s\n' "$*" >> "$LOG"; return 0; }

# Record the argv BEFORE any precondition can bail. A shim that dies early and
# logs nothing is invisible exactly when it matters most: meson reports the flat
# "llvm-config found: NO" and the log looks like the tool was never called.
log "ARGV: $*"

[[ -d "$TARGET_TREE" ]] || die "no cross LLVM tree at $TARGET_TREE (set CLADE_LLVM_TARGET_TREE)"

TARGET_LIBDIR="$TARGET_TREE/lib"
TARGET_INCLUDEDIR="$TARGET_TREE/include"
CACHE="$TARGET_TREE/CMakeCache.txt"
GENCONF="$TARGET_TREE/include/llvm/Config/llvm-config.h"

# The host tool that owns the component graph, DERIVED FROM THE CROSS TREE ITSELF:
# CMAKE_C_COMPILER is the clang that built this tree and llvm-config sits beside
# it, so the graph oracle cannot drift from the tree it is describing.
#
# Deriving it also means the shim needs NO ambient environment, which is not a
# nicety: meson execs this as a bare program named in the cross file, so whatever
# $LLVMFORK was set to in the shell that ran meson does not reach here. Trusting
# it produced the flat "llvm-config found: NO" above.
HOST_CONFIG="${CLADE_LLVM_HOST_CONFIG:-}"
if [[ -z "$HOST_CONFIG" ]]; then
    cc_used="$(sed -n 's|^CMAKE_C_COMPILER:[A-Z]*=\(.*\)$|\1|p' "$CACHE" 2>/dev/null | head -1)"
    [[ -n "$cc_used" ]] && HOST_CONFIG="$(dirname "$cc_used")/llvm-config"
fi
if [[ ! -x "$HOST_CONFIG" ]]; then
    fallback="${LLVMFORK:-$HOME/projects/llvm-thylacine}/build/bin/llvm-config"
    [[ -x "$fallback" ]] && HOST_CONFIG="$fallback"
fi
[[ -x "$HOST_CONFIG" ]] || die "no host llvm-config found. Tried, in order:
    \$CLADE_LLVM_HOST_CONFIG (${CLADE_LLVM_HOST_CONFIG:-unset})
    beside the cross tree's CMAKE_C_COMPILER (${cc_used:-not recorded in $CACHE})
    \${LLVMFORK}/build/bin/llvm-config (${LLVMFORK:-unset})
  Set CLADE_LLVM_HOST_CONFIG to a runnable llvm-config of the same LLVM version."

# --- facts read out of the cross tree itself -------------------------------------

# A generated #define, e.g. `#define LLVM_VERSION_STRING "22.1.8"`.
genconf_str() {
    local key="$1" v
    [[ -f "$GENCONF" ]] || die "no generated $GENCONF -- is $TARGET_TREE configured?"
    v="$(sed -n "s/^#define $key \"\\(.*\\)\"\$/\\1/p" "$GENCONF" | head -1)"
    [[ -n "$v" ]] || die "$key not defined in $GENCONF"
    printf '%s' "$v"
}

# CMakeCache entries carry a type: `LLVM_ENABLE_RTTI:BOOL=ON`.
cache_val() {
    local key="$1" v
    [[ -f "$CACHE" ]] || die "no $CACHE -- is $TARGET_TREE configured?"
    v="$(sed -n "s/^$key:[A-Z]*=\\(.*\\)\$/\\1/p" "$CACHE" | head -1)"
    printf '%s' "$v"
}

# ON/1/TRUE/YES all mean on in cmake; anything else (incl. absent) is off. Absent
# is the RTTI default, so treating it as off is the correct reading, not a guess.
cache_bool() {
    case "$(cache_val "$1" | tr '[:lower:]' '[:upper:]')" in
        ON|1|TRUE|YES|Y) printf 'YES' ;;
        *)               printf 'NO'  ;;
    esac
}

# LLVM_TARGETS_TO_BUILD is a cmake list ("AArch64" or "AArch64;X86"); llvm-config
# reports it space-separated. Read it from the tree so a retargeted build cannot
# leave this shim lying about what codegen is present.
targets_built() { cache_val LLVM_TARGETS_TO_BUILD | tr ';' ' '; }

# --- host delegation with path rewriting ----------------------------------------

HOST_PREFIX="$("$HOST_CONFIG" --prefix)"
HOST_LIBDIR="$("$HOST_CONFIG" --libdir)"

# The LLVM SOURCE include dir -- where llvm-c/Core.h and every other real header
# lives. A BUILD tree (which both of ours are) splits its headers in two, and this
# is measured, not assumed:
#
#   <source>/include      llvm-c/Core.h                  present, config header  ABSENT
#   <objroot>/include     llvm/Config/llvm-config.h      present, real headers   ABSENT
#
# so a consumer needs BOTH -I paths, and llvm-config's --cppflags emits both.
# The source half is SHARED between the host and cross builds (same fork tree,
# two object dirs), so it must pass through UNREWRITTEN. Rewriting it -- which an
# earlier version of this file did, by mapping the host --includedir onto the
# cross tree -- collapses the pair into one path that has the config header and
# none of the actual headers, and gallivm dies on "'llvm-c/Core.h' file not
# found" a long way from the cause.
SRC_INCLUDEDIR="$(cache_val LLVM_SOURCE_DIR)"
[[ -n "$SRC_INCLUDEDIR" ]] && SRC_INCLUDEDIR="$SRC_INCLUDEDIR/include"
[[ -d "$SRC_INCLUDEDIR" ]] || SRC_INCLUDEDIR="$("$HOST_CONFIG" --includedir)"

# Rewrite the host OBJECT paths to the cross tree -- libdir first, because it sits
# under the prefix and a prefix-first pass would mangle it. Deliberately absent:
# any rule touching the source include dir (see above).
rewrite_paths() {
    sed -e "s|$HOST_LIBDIR|$TARGET_LIBDIR|g" \
        -e "s|$HOST_PREFIX|$TARGET_TREE|g"
}

# Every archive named in a --libs/--libfiles/--libnames answer must exist in the
# cross tree. Accepts all three output forms: -lLLVMFoo, libLLVMFoo.a, and an
# absolute path. A miss lists EVERY missing file (not just the first) so one
# ninja invocation can fix the whole set.
assert_archives() {
    local answer="$1" tok base missing=""
    for tok in $answer; do
        case "$tok" in
            -l*)     base="lib${tok#-l}.a" ;;
            *.a)     base="$(basename "$tok")" ;;
            *)       continue ;;
        esac
        [[ -f "$TARGET_LIBDIR/$base" ]] || missing="$missing $base"
    done
    if [[ -n "${missing// /}" ]]; then
        # Emit the ninja TARGET names, not the filenames: LLVM's build names a
        # static library target by its bare component (the form stage 3 of
        # clade-keep-build.sh uses), so this line is copy-pasteable as-is.
        local targets=""
        for base in $missing; do base="${base#lib}"; targets="$targets ${base%.a}"; done
        log "MISSING ARCHIVES:$missing"
        die "the cross tree is missing these archives, so the answer would be a lie:$missing
    build them:  ninja -C $TARGET_TREE$targets"
    fi
}

# --- argv partition --------------------------------------------------------------
#
# llvm-config argv mixes three things: queries (--version), modifiers
# (--link-static, which changes how a later --libs answers) and bare component
# names (core orcjit). Partitioning them is what lets a single query be
# intercepted while the components still reach the delegated call.

queries=() modifiers=() components=()
for a in "$@"; do
    case "$a" in
        --link-static|--link-shared|--ignore-libllvm) modifiers+=("$a") ;;
        --*)                                          queries+=("$a")   ;;
        *)                                            components+=("$a") ;;
    esac
done

if [[ ${#queries[@]} -eq 0 ]]; then
    # No query at all: usage, or just modifiers. Delegating keeps the real tool's
    # exit status and text.
    "$HOST_CONFIG" "$@"
    exit $?
fi

# Real llvm-config CONSUMES a component list only for the library queries and
# rejects it everywhere else with "components given, but unused". That is fine for
# meson, which sends one combined invocation (`--libs --ldflags --link-static
# --system-libs <mods>`) where --libs consumes them -- but this shim answers each
# query separately, so it must decide per query. Passing the list to --ldflags is
# what turned a working configure into a FATAL.
query_takes_components() {
    case "$1" in --libs|--libfiles|--libnames) return 0 ;; *) return 1 ;; esac
}

delegate() {
    local q="$1" out err rc
    local -a argv=("${modifiers[@]+"${modifiers[@]}"}" "$q")
    if query_takes_components "$q"; then
        argv+=("${components[@]+"${components[@]}"}")
    fi
    # Capture stderr rather than discard it: the host tool's own message is the
    # only thing that explains a failure, and throwing it away is how a precise
    # error becomes "something went wrong somewhere in llvm-config".
    err="$(mktemp)"
    out="$("$HOST_CONFIG" "${argv[@]}" 2>"$err")"; rc=$?
    if [[ $rc -ne 0 ]]; then
        local msg; msg="$(tr '\n' ' ' < "$err")"; rm -f "$err"
        die "host llvm-config failed (rc=$rc) for: ${argv[*]}
    it said: ${msg:-<nothing on stderr>}"
    fi
    rm -f "$err"
    printf '%s' "$(rewrite_paths <<< "$out")"
}

answers=()
for q in "${queries[@]}"; do
    case "$q" in
        # --- cross-tree facts: never delegated ---
        --version)        answers+=("$(genconf_str LLVM_VERSION_STRING)") ;;
        --prefix)         answers+=("$TARGET_TREE") ;;
        --libdir)         answers+=("$TARGET_LIBDIR") ;;
        # The SOURCE include dir, matching what llvm-config means for a build tree
        # -- not $TARGET_TREE/include, which holds only generated headers.
        --includedir)     answers+=("$SRC_INCLUDEDIR") ;;
        --bindir)         answers+=("$TARGET_TREE/bin") ;;
        --cmakedir)       answers+=("$TARGET_LIBDIR/cmake/llvm") ;;
        --obj-root)       answers+=("$TARGET_TREE") ;;
        --src-root)       answers+=("$(cache_val LLVM_SOURCE_DIR)") ;;
        --host-target)    answers+=("$(genconf_str LLVM_DEFAULT_TARGET_TRIPLE)") ;;
        --targets-built)  answers+=("$(targets_built)") ;;
        --has-rtti)       answers+=("$(cache_bool LLVM_ENABLE_RTTI)") ;;
        --assertion-mode) answers+=("$(cache_bool LLVM_ENABLE_ASSERTIONS)") ;;
        --build-mode)     answers+=("$(cache_val CMAKE_BUILD_TYPE)") ;;
        --build-system)   answers+=("cmake") ;;
        --shared-mode)    answers+=("static") ;;
        --link-static|--link-shared) : ;;  # partitioned out; cannot land here

        # The target's system libraries come from the Thylacine link line (the
        # pouch sysroot's musl libc.a, which also carries libm), NOT from the
        # host's -lrt/-ldl/-lpthread. Answering empty is the correct cross answer;
        # delegating would inject host-only libraries into a static Thylacine link.
        --system-libs)    answers+=("") ;;

        # --- graph + flags: delegated, then path-rewritten ---
        --libs|--libfiles|--libnames)
            a="$(delegate "$q")"
            assert_archives "$a"
            answers+=("$a")
            ;;
        --components|--cppflags|--cflags|--cxxflags|--ldflags)
            answers+=("$(delegate "$q")")
            ;;

        *)
            # Loud, not silent. If this fires, decide whether the host answer is
            # actually right for the target and add an explicit arm.
            log "UNHANDLED QUERY: $q (delegated verbatim to the host tree)"
            echo "clade-llvm-config-cross: note: unhandled query '$q' delegated to the host tree" >&2
            answers+=("$(delegate "$q")")
            ;;
    esac
done

# llvm-config space-joins multiple answers on one line.
out="${answers[*]}"
log "ANSWER: $out"
printf '%s\n' "$out"
