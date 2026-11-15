#!/usr/bin/env bash
# tools/test-stratum-src.sh -- tools/stratum-src.sh against made-up checkouts: which
# tree is built, which is refused and why, and when a build dir is dropped; and
# build.sh's own stale check on a refused tree. Every case fails without the behavior
# it checks; STRATUM_SRC_SH and BUILD_SH point it at sabotaged copies to prove that.
# No build, no network. Under a second.
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
STRATUM_SRC_SH="${STRATUM_SRC_SH:-$REPO_ROOT/tools/stratum-src.sh}"
BUILD_SH="${BUILD_SH:-$REPO_ROOT/tools/build.sh}"
# shellcheck disable=SC1090
. "$STRATUM_SRC_SH"

pass=0; fail=0
ok()  { printf '  PASS  %s\n' "$1"; pass=$((pass+1)); }
bad() { printf '  FAIL  %s\n' "$1"; fail=$((fail+1)); }
assert_eq() { if [[ "$1" == "$2" ]]; then ok "$3"; else bad "$3 (got '$1' want '$2')"; fi; }

W="$(mktemp -d "${TMPDIR:-/tmp}/stratum-src.XXXXXX")" || exit 2
trap 'rm -rf "${W:?}"' EXIT
# Three homes: the tree at the checkout's root, a checkout from before the move (the
# tree under v2/, the old prototype's CMakeLists.txt at the root), and no checkout.
mkdir -p "$W/new/projects/stratum/src/btree_engine" "$W/old/projects/stratum/v2/src/btree_engine" "$W/none"
: > "$W/new/projects/stratum/CMakeLists.txt"; : > "$W/new/projects/stratum/src/btree_engine/engine.c"
: > "$W/old/projects/stratum/CMakeLists.txt"; : > "$W/old/projects/stratum/v2/CMakeLists.txt"
: > "$W/old/projects/stratum/v2/src/btree_engine/engine.c"
where() { ( HOME="$W/$1"; unset STRATUM_SRC; stratum_src_dir ); }

echo "== stratum_src_dir"
assert_eq "$(where new)"  "$W/new/projects/stratum"  "the checkout's root is the tree named"
assert_eq "$(where old)"  "$W/old/projects/stratum"  "a checkout from before the move is named as it is, never its v2/"
assert_eq "$(where none)" "$W/none/projects/stratum" "no checkout: the root is named, for the build to report as missing"
assert_eq "$(HOME="$W/old"; STRATUM_SRC=/somewhere/else; stratum_src_dir)" "/somewhere/else" "STRATUM_SRC is taken as given"

echo "== stratum_src_ok"
# Each verdict is asked with the reason it gives: a refusal for the wrong reason would
# send the reader to the wrong fix.
verdict() { if stratum_src_ok "$1" builder 2> "$W/why"; then echo ok; else echo refused; fi; }
assert_eq "$(verdict "$W/new/projects/stratum")" ok "the tree at a checkout's root is Stratum's"
assert_eq "$(verdict "$W/old/projects/stratum/v2")" ok "an old checkout's v2/, named outright, is Stratum's too"
assert_eq "$(verdict "$W/old/projects/stratum")" refused "a checkout from before the move is refused"
why="$(cat "$W/why")"
assert_eq "$([[ "$why" == "==> builder: $W/old/projects/stratum is a Stratum checkout from before"* ]] && echo said || echo not)" said "and the refusal says so, as the caller"
assert_eq "$([[ "$why" == *"git -C $W/old/projects/stratum pull --ff-only"* ]] && echo said || echo not)" said "and names the pull that brings it up to date"
mkdir -p "$W/elsewhere"; : > "$W/elsewhere/CMakeLists.txt"
assert_eq "$(verdict "$W/elsewhere")" refused "a tree with a CMakeLists.txt but none of Stratum's source is refused"
assert_eq "$([[ "$(cat "$W/why")" == *"is not a Stratum source tree"* ]] && echo said || echo not)" said "as not Stratum's tree"
assert_eq "$(verdict "$W/none/projects/stratum")" refused "a tree that is not there is refused"
assert_eq "$([[ "$(cat "$W/why")" == *"source tree not found at $W/none/projects/stratum"* ]] && echo said || echo not)" said "as not found"

echo "== stratum_build_dir_fits"
cache() { mkdir -p "$1"; printf 'CMAKE_HOME_DIRECTORY:INTERNAL=%s\n' "$2" > "$1/CMakeCache.txt"; : > "$1/object.o"; }
# build.sh runs under set -euo pipefail and calls the helper as a plain statement, where
# any failing command ends the whole build without a word; every case calls it so. Never
# inside an if or a ||: errexit is ignored there, in subshells too. The call must also be
# seen to return: an exit 0 in the helper would end build.sh with a success.
fits() {
    : > "$W/returned"
    ( set -euo pipefail; stratum_build_dir_fits "$@"; echo returned > "$W/returned" )
    local rc=$?
    [[ "$rc" == 0 && "$(cat "$W/returned")" == returned ]] ||
        bad "the call for ${1##*/} ended the shell (rc $rc) instead of returning, as it would end build.sh"
    return 0
}
src_new="$W/new/projects/stratum"; src_old="$W/old/projects/stratum/v2"
cache "$W/b-same" "$src_new"
fits "$W/b-same" "$src_new" 2>/dev/null
assert_eq "$([[ -f "$W/b-same/object.o" ]] && echo kept || echo dropped)" kept "a build dir configured from this tree is kept"
ln -s "$src_new" "$W/link-to-new"
fits "$W/b-same" "$W/link-to-new" 2>/dev/null
assert_eq "$([[ -f "$W/b-same/object.o" ]] && echo kept || echo dropped)" kept "the same tree under another name is the same tree"
cache "$W/b-other" "$src_old"
fits "$W/b-other" "$src_new" 2> "$W/said"
said="$(cat "$W/said")"
assert_eq "$([[ -e "$W/b-other" ]] && echo kept || echo dropped)" dropped "a build dir configured from another tree is dropped"
assert_eq "$([[ "$said" == *"was configured from $src_old"* ]] && echo said || echo silent)" said "and the drop is said aloud"
cache "$W/b-gone" "$W/a-tree-that-is-gone"
fits "$W/b-gone" "$src_new" 2>/dev/null
assert_eq "$([[ -e "$W/b-gone" ]] && echo kept || echo dropped)" dropped "a build dir whose tree is gone is dropped"
cache "$W/b-nosrc" "$src_old"
fits "$W/b-nosrc" "$W/no-such-source" 2>/dev/null
assert_eq "$([[ -f "$W/b-nosrc/object.o" ]] && echo kept || echo dropped)" kept "nothing is dropped for a source that is not there"
cache "$W/b-oldroot" "$src_old"
fits "$W/b-oldroot" "$W/old/projects/stratum" 2>/dev/null
assert_eq "$([[ -f "$W/b-oldroot/object.o" ]] && echo kept || echo dropped)" kept "nothing is dropped for a checkout from before the move: it is refused, then pulled"
mkdir -p "$W/b-noname"; printf 'CMAKE_BUILD_TYPE:STRING=Release\n' > "$W/b-noname/CMakeCache.txt"; : > "$W/b-noname/object.o"
fits "$W/b-noname" "$src_new" 2>/dev/null
assert_eq "$([[ -f "$W/b-noname/object.o" ]] && echo kept || echo dropped)" kept "a cache that names no source tree is left alone"
if [[ "$(id -u)" != 0 ]]; then
    cache "$W/b-unread" "$W/a-tree-that-is-gone"; chmod 000 "$W/b-unread/CMakeCache.txt"
    fits "$W/b-unread" "$src_new" 2>/dev/null
    assert_eq "$([[ -f "$W/b-unread/object.o" ]] && echo kept || echo dropped)" kept "a cache that cannot be read is left alone, for CMake to report"
fi
mkdir -p "$W/b-plain"; : > "$W/b-plain/object.o"
fits "$W/b-plain" "$src_new" 2>/dev/null
assert_eq "$([[ -f "$W/b-plain/object.o" ]] && echo kept || echo dropped)" kept "a dir that is no CMake build dir is left alone"

echo "== build.sh: stratum_host_tools_stale"
# The function as build.sh has it: a refused tree must read as stale, so that the
# builder runs and refuses it, where host tools built from the old tree would
# otherwise be taken as current.
stale_fn="$(sed -n '/^stratum_host_tools_stale() {$/,/^}$/p' "$BUILD_SH")"
if [[ -z "$stale_fn" ]]; then
    bad "stratum_host_tools_stale not found in $BUILD_SH"
else
    eval "$stale_fn"
    tools() {   # BUILD_DIR SOURCE_DIR: host tools built from SOURCE_DIR, newer than it
        local hb="$1/host-stratum" b
        cache "$hb" "$2"
        for b in stratum-mkfs stratumd stratum-fs; do
            mkdir -p "$hb/src/cmd/$b"; : > "$hb/src/cmd/$b/$b"; chmod +x "$hb/src/cmd/$b/$b"
        done
        : > "$hb/.complete"
    }
    touch -t 202001010000 "$src_new/CMakeLists.txt" "$src_new/src/btree_engine/engine.c" \
        "$W/old/projects/stratum/CMakeLists.txt" "$src_old/CMakeLists.txt" "$src_old/src/btree_engine/engine.c"
    tools "$W/bd-new" "$src_new"
    tools "$W/bd-old" "$src_old"
    stale() { ( BUILD_DIR="$1"; HOME="$W/$2"; unset STRATUM_SRC; stratum_host_tools_stale ) 2>/dev/null && echo stale || echo current; }
    assert_eq "$(stale "$W/bd-new" new)" current "host tools built from this tree, newer than it, are current"
    assert_eq "$(stale "$W/bd-old" old)" stale "under a checkout from before the move they are stale, so the builder refuses it"
    assert_eq "$([[ -f "$W/bd-old/host-stratum/object.o" ]] && echo kept || echo dropped)" kept "and their build dir is kept for after the pull"
fi

echo "== $pass passed, $fail failed"
[[ "$fail" == 0 ]]
