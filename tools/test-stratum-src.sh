#!/usr/bin/env bash
# tools/test-stratum-src.sh -- tools/stratum-src.sh against made-up checkouts: which
# tree is built, which is refused and why, and when a build dir is dropped; and
# build.sh's own functions on a refused tree: the stale check, and the builders
# (cmake stubbed) that must refuse before they configure, drop or delete anything.
# Every case fails without the behavior it checks; STRATUM_SRC_SH and BUILD_SH point
# it at sabotaged copies to prove that. No build, no network. A few seconds.
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
trap 'chmod -R u+rwx "$W" 2>/dev/null; rm -rf "${W:?}"' EXIT
# Three homes: the tree at the checkout's root; a checkout from before the move, with
# the tree under v2/ and the old prototype's tree at the root (its own src/, include/
# and CMakeLists.txt, as the real one has); and no checkout.
mkold() {
    mkdir -p "$1/v2/src/btree_engine" "$1/src/cmd" "$1/include"
    : > "$1/CMakeLists.txt"; : > "$1/src/CMakeLists.txt"; : > "$1/src/cmd/stratum.c"; : > "$1/include/stratum.h"
    : > "$1/v2/CMakeLists.txt"; : > "$1/v2/src/btree_engine/engine.c"
}
mknew() { mkdir -p "$1/src/btree_engine" "$1/include" "$1/third_party"; : > "$1/CMakeLists.txt"; : > "$1/src/btree_engine/engine.c"; }
mknew "$W/new/projects/stratum"; mkold "$W/old/projects/stratum"; mkdir -p "$W/none"
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
said() { [[ "$(cat "$W/why")" == *"$1"* ]] && echo said || echo not; }
assert_eq "$(verdict "$W/new/projects/stratum")" ok "the tree at a checkout's root is Stratum's"
assert_eq "$(verdict "$W/old/projects/stratum/v2")" ok "an old checkout's v2/, named outright, is Stratum's too"
assert_eq "$(verdict "$W/old/projects/stratum")" refused "a checkout from before the move is refused, the old prototype's src/ at its root notwithstanding"
assert_eq "$([[ "$(cat "$W/why")" == "==> builder: $W/old/projects/stratum is a Stratum checkout from before"* ]] && echo said || echo not)" said "and the refusal says so, as the caller"
assert_eq "$(said "git -C $W/old/projects/stratum pull --ff-only")" said "and names the pull that brings it up to date"
assert_eq "$(said "committed or stashed first")" said "and that local changes must be put away for it"
mkold "$W/with space/stratum"
assert_eq "$(verdict "$W/with space/stratum")" refused "a checkout from before the move under a path with a space is refused"
assert_eq "$(said 'git -C '"${W// /\\ }"'/with\ space/stratum pull')" said "and its pull line can be pasted as printed"
mkdir -p "$W/elsewhere"; : > "$W/elsewhere/CMakeLists.txt"
assert_eq "$(verdict "$W/elsewhere")" refused "a tree with a CMakeLists.txt but none of Stratum's source is refused"
assert_eq "$(said "is not a Stratum source tree")" said "as not Stratum's tree"
assert_eq "$(verdict "$W/none/projects/stratum")" refused "a tree that is not there is refused"
assert_eq "$(said "source tree not found at $W/none/projects/stratum")" said "as not found"
: > "$W/afile"
assert_eq "$(verdict "$W/afile")" refused "a regular file is refused"
assert_eq "$(said "is not a directory")" said "as not a directory"
if [[ "$(id -u)" != 0 ]]; then
    mknew "$W/unread"; chmod 000 "$W/unread"
    assert_eq "$(verdict "$W/unread")" refused "a tree that cannot be read is refused"
    assert_eq "$(said "cannot be read")" said "as unreadable, not as some other tree"
    chmod 755 "$W/unread"
fi

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
kept() { [[ -f "$1/object.o" ]] && echo kept || echo dropped; }
src_new="$W/new/projects/stratum"; src_old="$W/old/projects/stratum/v2"
cache "$W/b-same" "$src_new"
fits "$W/b-same" "$src_new" 2>/dev/null
assert_eq "$(kept "$W/b-same")" kept "a build dir configured from this tree is kept"
ln -s "$src_new" "$W/link-to-new"
fits "$W/b-same" "$W/link-to-new" 2>/dev/null
assert_eq "$(kept "$W/b-same")" kept "the same tree under another name is the same tree"
cache "$W/b-other" "$src_old"
fits "$W/b-other" "$src_new" 2> "$W/said"
assert_eq "$([[ -e "$W/b-other" ]] && echo kept || echo dropped)" dropped "a build dir configured from another tree is dropped"
assert_eq "$([[ "$(cat "$W/said")" == *"was configured from $src_old"* ]] && echo said || echo silent)" said "and the drop is said aloud"
cache "$W/b-gone" "$W/a-tree-that-is-gone"
fits "$W/b-gone" "$src_new" 2>/dev/null
assert_eq "$([[ -e "$W/b-gone" ]] && echo kept || echo dropped)" dropped "a build dir whose tree is gone is dropped"
cache "$W/b-nosrc" "$src_old"
fits "$W/b-nosrc" "$W/no-such-source" 2>/dev/null
assert_eq "$(kept "$W/b-nosrc")" kept "nothing is dropped for a source that is not there"
cache "$W/b-oldroot" "$src_old"
fits "$W/b-oldroot" "$W/old/projects/stratum" 2>/dev/null
assert_eq "$(kept "$W/b-oldroot")" kept "nothing is dropped for a refused source (a checkout from before the move)"
mkdir -p "$W/b-noname"; printf 'CMAKE_BUILD_TYPE:STRING=Release\n' > "$W/b-noname/CMakeCache.txt"; : > "$W/b-noname/object.o"
fits "$W/b-noname" "$src_new" 2>/dev/null
assert_eq "$(kept "$W/b-noname")" kept "a cache that names no source tree is left alone"
if [[ "$(id -u)" != 0 ]]; then
    cache "$W/b-unread" "$W/a-tree-that-is-gone"; chmod 000 "$W/b-unread/CMakeCache.txt"
    fits "$W/b-unread" "$src_new" 2>/dev/null
    assert_eq "$(kept "$W/b-unread")" kept "a cache that cannot be read is left alone, for CMake to report"
fi
mkdir -p "$W/b-plain"; : > "$W/b-plain/object.o"
fits "$W/b-plain" "$src_new" 2>/dev/null
assert_eq "$(kept "$W/b-plain")" kept "a dir that is no CMake build dir is left alone"
# Relative names, as STRATUM_SRC may give them: one that starts with a dash is still a
# name, and CDPATH does not send a bare one to another tree.
mkdir -p "$W/cwd" "$W/cdp"
mknew "$W/cwd/-x"; mknew "$W/cwd/stratum"; mknew "$W/cdp/stratum"
cache "$W/b-dash" "$src_old"
cd "$W/cwd" || exit 2
fits "$W/b-dash" -x 2>/dev/null
assert_eq "$([[ -e "$W/b-dash" ]] && echo kept || echo dropped)" dropped "a relative source named like an option is compared as a tree"
cache "$W/b-cdpath" "$W/cwd/stratum"
CDPATH="$W/cdp" fits "$W/b-cdpath" stratum 2>/dev/null
assert_eq "$(kept "$W/b-cdpath")" kept "CDPATH does not make the tree a build dir was configured from look like another"
cd "$REPO_ROOT" || exit 2

echo "== build.sh: one definition of each function sliced from it"
# bash runs a function's LAST definition, sed's slice is the FIRST: they agree only
# when there is one.
defs() { grep -cE "^[[:space:]]*(function[[:space:]]+)?$1[[:space:]]*(\(\))?[[:space:]]*\{" "$BUILD_SH"; }
slice() { sed -n "/^$1() {\$/,/^}\$/p" "$BUILD_SH"; }
FUNCS=""
for f in stratum_host_tools_stale build_stratumd build_stratum_host_tools build_stratum_pool_fixture; do
    n="$(defs "$f")"
    if [[ "$n" == 1 && -n "$(slice "$f")" ]]; then ok "$f is defined once"; FUNCS="$FUNCS$(slice "$f")"$'\n'
    else bad "$f has $n definitions in $BUILD_SH (want exactly one)"; fi
done

echo "== build.sh: stratum_host_tools_stale"
# A refused tree must read as stale, so that the builder runs and refuses it, where host
# tools built from the old tree would otherwise be taken as current; and tools built
# from another real tree are stale, their build dir dropped.
tools() {   # BUILD_DIR SOURCE_DIR: host tools built from SOURCE_DIR, newer than it
    local hb="$1/host-stratum" b
    cache "$hb" "$2"
    for b in stratum-mkfs stratumd stratum-fs; do
        mkdir -p "$hb/src/cmd/$b"; : > "$hb/src/cmd/$b/$b"; chmod +x "$hb/src/cmd/$b/$b"
    done
    : > "$hb/.complete"
}
find "$W/new" "$W/old" -type f -exec touch -t 202001010000 {} +
mkdir -p "$src_new/third_party/BLAKE3"; : > "$src_new/third_party/BLAKE3/blake3.c"; touch -t 202001010000 "$src_new/third_party/BLAKE3/blake3.c"
tools "$W/bd-new" "$src_new"; tools "$W/bd-old" "$src_old"; tools "$W/bd-switch" "$src_old"
stale() { ( eval "$FUNCS"; BUILD_DIR="$1"; HOME="$W/$2"; unset STRATUM_SRC; stratum_host_tools_stale ) 2>/dev/null && echo stale || echo current; }
assert_eq "$(stale "$W/bd-new" new)" current "host tools built from this tree, newer than it, are current"
assert_eq "$(stale "$W/bd-old" old)" stale "under a checkout from before the move they are stale, so the builder refuses it"
assert_eq "$(kept "$W/bd-old/host-stratum")" kept "and their build dir is not dropped for a refused source"
assert_eq "$(stale "$W/bd-switch" new)" stale "tools built from another real tree are stale"
assert_eq "$([[ -e "$W/bd-switch/host-stratum" ]] && echo kept || echo dropped)" dropped "and their build dir is dropped"
touch "$src_new/third_party/BLAKE3/blake3.c"
assert_eq "$(stale "$W/bd-new" new)" stale "a third_party/ source newer than the tools makes them stale"

echo "== build.sh: the builders refuse before they configure, drop or delete"
# Each builder as build.sh has it, under set -euo pipefail, cmake stubbed to record its
# calls, a warm build dir and a pool beside it. A refused source: the builder fails, its
# first word is the refusal, cmake is never called and nothing is touched. The control,
# Stratum's tree, reaches cmake.
builder() {   # FUNCTION SOURCE -> sets brc, bcm (cmake calls), bfirst (first stderr line), btouched
    local B="$W/bb" fn="$1" src="$2" hb sd b
    rm -rf "$B"; mkdir -p "$B/fixtures" "$B/sysroot/lib"
    : > "$B/sysroot/lib/libc.a"; : > "$B/sysroot/lib/libsodium.a"
    echo POOL > "$B/fixtures/pool.img"; echo KEY > "$B/fixtures/system.key"
    tools "$B" "$src_old"; hb="$B/host-stratum"; sd="$B/pouch/stratumd-cmake"; cache "$sd" "$src_old"
    : > "$W/cmake.log"
    ( set -euo pipefail
      eval "$FUNCS"
      cmake() { echo "cmake $*" >> "$W/cmake.log"; return 0; }
      ledger() { :; }
      BUILD_DIR="$B" LLVM_PREFIX=/nonexistent HOME="$W/old"
      if [[ -n "$src" ]]; then export STRATUM_SRC="$src"; else unset STRATUM_SRC; fi
      "$fn"; echo RETURNED ) > "$W/bout" 2> "$W/berr"
    brc=$?
    bcm="$(grep -c . "$W/cmake.log")"
    bfirst="$(head -1 "$W/berr")"
    btouched=no
    for b in "$B/fixtures/pool.img" "$B/fixtures/system.key" "$hb/object.o" "$hb/.complete" "$sd/object.o"; do
        [[ -f "$b" ]] || btouched="$b"
    done
}
for fn in build_stratumd build_stratum_pool_fixture; do
    for c in "old:" "elsewhere:$W/elsewhere" "missing:$W/no-such-source"; do
        label="${c%%:*}"; src="${c#*:}"
        case "$label" in
            old)       want="is a Stratum checkout from before" ;;
            elsewhere) want="is not a Stratum source tree" ;;
            missing)   want="source tree not found" ;;
        esac
        builder "$fn" "$src"
        assert_eq "$([[ "$brc" != 0 && "$bfirst" == "==> "*"$want"* ]] && echo refused || echo "rc=$brc first='$bfirst'")" refused "$fn refuses the $label source first"
        assert_eq "$bcm/$btouched" "0/no" "$fn on the $label source calls no cmake and touches nothing"
    done
    builder "$fn" "$src_new"
    assert_eq "$([[ "$bcm" -ge 1 ]] && echo reached || echo "not reached (rc=$brc first='$bfirst')")" reached "$fn on Stratum's tree reaches cmake (control)"
done
# PRESERVE reuses pool.img and system.key and runs no host tool: it needs no source.
B="$W/bp"; rm -rf "$B"; mkdir -p "$B/fixtures"; echo POOL > "$B/fixtures/pool.img"; echo KEY > "$B/fixtures/system.key"
( set -euo pipefail; eval "$FUNCS"; cmake() { echo "cmake $*" >> "$W/cmake.log"; }; ledger() { :; }
  BUILD_DIR="$B" HOME="$W/none" THYLACINE_MKFS_PRESERVE=1; unset STRATUM_SRC
  build_stratum_pool_fixture; echo RETURNED ) > "$W/pout" 2>/dev/null
assert_eq "$([[ "$(tail -1 "$W/pout")" == RETURNED && "$(cat "$B/fixtures/pool.img")" == POOL ]] && echo kept || echo "not kept: $(tail -1 "$W/pout")")" kept "PRESERVE keeps pool.img and system.key with no Stratum source at all"

echo "== $pass passed, $fail failed"
[[ "$fail" == 0 ]]
