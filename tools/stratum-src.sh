# tools/stratum-src.sh -- where Stratum's source tree is, and whether a build dir is its.
#
# Stratum's tree is at the root of its repository. It used to sit under v2/ there, and
# a checkout that has not been moved still has it so. Both are found, so the day the
# shared checkout moves no worktree's build breaks; the v2 arm goes once no checkout
# is left that needs it. Sourced by build.sh; tools/test-stratum-src.sh runs it
# against made-up checkouts.
#
# bash 3.2 safe.

# The tree to build: $STRATUM_SRC when set, else the checkout beside the others.
stratum_src_dir() {
    if [[ -n "${STRATUM_SRC:-}" ]]; then
        printf '%s\n' "$STRATUM_SRC"
        return 0
    fi
    local root="$HOME/projects/stratum"
    if [[ ! -f "$root/src/btree_engine/engine.c" && -f "$root/v2/CMakeLists.txt" ]]; then
        printf '%s\n' "$root/v2"
    else
        printf '%s\n' "$root"
    fi
}

# A CMake build dir remembers the source tree it was configured from and refuses any
# other, so one left from a tree that has moved is dropped here: it is a cache, and
# the next configure makes it again. Said aloud, because that build is a cold one.
stratum_build_dir_fits() {   # BUILD_DIR SOURCE_DIR
    local dir="$1" src="$2" cached was now
    [[ -f "$dir/CMakeCache.txt" ]] || return 0
    cached="$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "$dir/CMakeCache.txt")"
    [[ -n "$cached" ]] || return 0
    # A cd that fails must not fail the assignment: build.sh runs under set -e, and a
    # tree that is gone is the case this exists for.
    was="$(cd "$cached" 2>/dev/null && pwd -P)" || was=""
    now="$(cd "$src" 2>/dev/null && pwd -P)" || now=""
    # A source that is not there is the caller's to report; nothing is dropped for it.
    [[ -n "$now" && "$was" != "$now" ]] || return 0
    echo "==> stratum: $dir was configured from $cached, not $src -- dropping it for a fresh configure" >&2
    rm -rf "${dir:?}"
}
