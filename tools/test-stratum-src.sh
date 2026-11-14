#!/usr/bin/env bash
# tools/test-stratum-src.sh -- tools/stratum-src.sh against made-up checkouts: which
# tree is built in each layout, and when a build dir is dropped. Every case fails
# without the behavior it checks; STRATUM_SRC_SH points it at a sabotaged copy to
# prove that. No build, no network. Under a second.
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
STRATUM_SRC_SH="${STRATUM_SRC_SH:-$REPO_ROOT/tools/stratum-src.sh}"
# shellcheck disable=SC1090
. "$STRATUM_SRC_SH"

pass=0; fail=0
ok()  { printf '  PASS  %s\n' "$1"; pass=$((pass+1)); }
bad() { printf '  FAIL  %s\n' "$1"; fail=$((fail+1)); }
assert_eq() { if [[ "$1" == "$2" ]]; then ok "$3"; else bad "$3 (got '$1' want '$2')"; fi; }

W="$(mktemp -d "${TMPDIR:-/tmp}/stratum-src.XXXXXX")" || exit 2
trap 'rm -rf "${W:?}"' EXIT
# Three homes: the tree at the checkout's root, the tree under v2/ beside an older
# one at the root, and no checkout at all.
mkdir -p "$W/new/projects/stratum/src/btree_engine" "$W/old/projects/stratum/v2/src/btree_engine" "$W/none"
: > "$W/new/projects/stratum/CMakeLists.txt"; : > "$W/new/projects/stratum/src/btree_engine/engine.c"
: > "$W/old/projects/stratum/CMakeLists.txt"; : > "$W/old/projects/stratum/v2/CMakeLists.txt"
: > "$W/old/projects/stratum/v2/src/btree_engine/engine.c"
where() { ( HOME="$W/$1"; unset STRATUM_SRC; stratum_src_dir ); }

echo "== stratum_src_dir"
assert_eq "$(where new)"  "$W/new/projects/stratum"    "the tree at the checkout's root is the one built"
assert_eq "$(where old)"  "$W/old/projects/stratum/v2" "a checkout not yet moved is built from v2/"
assert_eq "$(where none)" "$W/none/projects/stratum"   "no checkout: the root is named, for the build to report as missing"
assert_eq "$(HOME="$W/old"; STRATUM_SRC=/somewhere/else; stratum_src_dir)" "/somewhere/else" "STRATUM_SRC is taken as given"
# A root that has the tree is built from the root even with a v2/ left beside it.
mkdir -p "$W/both/projects/stratum/src/btree_engine" "$W/both/projects/stratum/v2"
: > "$W/both/projects/stratum/src/btree_engine/engine.c"; : > "$W/both/projects/stratum/v2/CMakeLists.txt"
assert_eq "$(where both)" "$W/both/projects/stratum"   "the root wins over a v2/ left beside it"

echo "== stratum_build_dir_fits"
cache() { mkdir -p "$1"; printf 'CMAKE_HOME_DIRECTORY:INTERNAL=%s\n' "$2" > "$1/CMakeCache.txt"; : > "$1/object.o"; }
# build.sh runs under set -euo pipefail and calls the helper as a plain statement, where
# any failing command ends the whole build without a word; every case calls it so. Never
# inside an if or a ||: errexit is ignored there, in subshells too.
fits() {
    ( set -euo pipefail; stratum_build_dir_fits "$@" )
    local rc=$?
    [[ "$rc" == 0 ]] || bad "the call for ${1##*/} ended the shell under set -e (rc $rc), as it would end build.sh"
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
mkdir -p "$W/b-noname"; printf 'CMAKE_BUILD_TYPE:STRING=Release\n' > "$W/b-noname/CMakeCache.txt"; : > "$W/b-noname/object.o"
fits "$W/b-noname" "$src_new" 2>/dev/null
assert_eq "$([[ -f "$W/b-noname/object.o" ]] && echo kept || echo dropped)" kept "a cache that names no source tree is left alone"
mkdir -p "$W/b-plain"; : > "$W/b-plain/object.o"
fits "$W/b-plain" "$src_new" 2>/dev/null
assert_eq "$([[ -f "$W/b-plain/object.o" ]] && echo kept || echo dropped)" kept "a dir that is no CMake build dir is left alone"

echo "== $pass passed, $fail failed"
[[ "$fail" == 0 ]]
