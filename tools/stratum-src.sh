# tools/stratum-src.sh -- where Stratum's source tree is, whether it is the tree, and
# whether a build dir is its.
#
# Stratum's tree is at the root of its repository. A checkout from before it moved there
# has it under v2/ and the old prototype's tree at the root, which configures but has no
# stratumd target, failing with an error that names no fix; such a checkout is refused
# first, with how to bring it up to date. Sourced by build.sh; tools/test-stratum-src.sh
# runs it against made-up checkouts.
#
# bash 3.2 safe.

# The tree to build: $STRATUM_SRC when set, else the checkout beside the others.
stratum_src_dir() {
    if [[ -n "${STRATUM_SRC:-}" ]]; then
        printf '%s\n' "$STRATUM_SRC"
        return 0
    fi
    printf '%s\n' "$HOME/projects/stratum"
}

# True when SOURCE_DIR is Stratum's tree; otherwise says why, as WHO, and is false.
stratum_src_ok() {   # SOURCE_DIR WHO
    local src="$1" who="$2"
    [[ -f "$src/src/btree_engine/engine.c" ]] && return 0
    if [[ -f "$src/v2/src/btree_engine/engine.c" ]]; then
        echo "==> $who: $src is a Stratum checkout from before its tree moved to the repository root (the tree is under v2/ there)" >&2
        printf '    bring it up to date: git -C %q pull --ff-only (with its local changes committed or stashed first),\n' "$src" >&2
        echo "    or set STRATUM_SRC to a checkout that is up to date" >&2
    elif [[ -d "$src" && ( ! -r "$src" || ! -x "$src" ) ]]; then
        echo "==> $who: $src cannot be read" >&2
    elif [[ -d "$src" ]]; then
        echo "==> $who: $src is not a Stratum source tree (no src/btree_engine/engine.c)" >&2
        echo "    override with STRATUM_SRC=/path/to/stratum" >&2
    elif [[ -e "$src" ]]; then
        echo "==> $who: $src is not a directory" >&2
        echo "    override with STRATUM_SRC=/path/to/stratum" >&2
    else
        echo "==> $who: source tree not found at $src" >&2
        echo "    override with STRATUM_SRC=/path/to/stratum" >&2
    fi
    return 1
}

# A CMake build dir remembers the source tree it was configured from and refuses any
# other, so one left from a tree that has moved is dropped here: it is a cache, and
# the next configure makes it again. Said aloud, because that build is a cold one.
stratum_build_dir_fits() {   # BUILD_DIR SOURCE_DIR
    local dir="$1" src="$2" cached was now
    [[ -f "$dir/CMakeCache.txt" ]] || return 0
    # A source that is not Stratum's tree, or not there, is the caller's to report;
    # nothing is dropped for it.
    stratum_src_ok "$src" stratum 2>/dev/null || return 0
    # No command here may fail its assignment: build.sh runs under set -e, and a tree
    # that is gone is the case this exists for. A cache that cannot be read is kept,
    # for CMake to report.
    cached="$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "$dir/CMakeCache.txt" 2>/dev/null)" || cached=""
    [[ -n "$cached" ]] || return 0
    # CDPATH would send a bare relative name elsewhere and print where; -- keeps a
    # name that starts with a dash a name.
    was="$(CDPATH='' cd -- "$cached" 2>/dev/null && pwd -P)" || was=""
    now="$(CDPATH='' cd -- "$src" 2>/dev/null && pwd -P)" || now=""
    [[ -n "$now" && "$was" != "$now" ]] || return 0
    echo "==> stratum: $dir was configured from $cached, not $src -- dropping it for a fresh configure" >&2
    rm -rf "${dir:?}"
}
