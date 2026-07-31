#!/usr/bin/env bash
# Clade stage 1: build the LLVM FORK's own HOST toolchain.
#
# WHY THIS IS ITS OWN FILE. Stage 1 is the one part of the Clade build that
# `tools/build.sh` cannot express -- build.sh's targets (sysroot/libcxx/clade)
# all CONSUME a fork clang that already knows `aarch64-thylacine`, and teaching
# a compiler that triple is exactly what the CL-3 fork commits do. So the
# recipe has to live somewhere, and it must live in exactly ONE place: two
# builder drivers need it (the disposable GCP VM and the permanent one), and a
# hand-copied second mirror is the failure mode this tree has already been
# bitten by -- the copy passes its own checks while silently disagreeing with
# the original (`struct t_stat`, #100). This file is that single original;
# `clade-gcp-build.sh` and `clade-keep-build.sh` both call it.
#
# A stock clang only BOOTSTRAPS this. The product is the fork's own LLVM, and
# it supplies the binutils + ld.lld too, on purpose: build.sh reaches for
# llvm-ar/ranlib/nm/strip/readelf and ld.lld under $LLVM_PREFIX/$LLD_PREFIX,
# and pointing those at a distro llvm-18 would build the pouch sysroot with a
# compiler four majors adrift from the one that builds everything linked
# against it. That skew is not hypothetical here: the v8.0 userspace floor
# (#71) rides -moutline-atomics and task #91 is open precisely because an LSE
# regression there is SILENT.
#
# NO "skip if bin/clang exists" GUARD. Such a guard makes a resumed run WRONG
# rather than merely slow -- a tree from an earlier run can have clang and still
# lack the binutils/lld this stage also produces, and the guard would skip past
# them into a stage 2 that needs them. ninja already IS the incremental guard;
# hand-rolling a second one only adds a staleness bug.
#
# Usage:
#   tools/clade-stage1.sh --src DIR [--build DIR] [--jobs N]
#                         [--patches DIR --tag REF]
#
#   --src      the LLVM fork checkout (upstream tag + the Thylacine commits)
#   --build    where to build (default: $src/build). If it differs from
#              $src/build, a symlink is established -- see below.
#   --jobs     ninja parallelism (default: nproc)
#   --patches  if given, reconcile --src's series against the *.patch there (the
#              VM flow ships the fork as a patch series rather than a whole tree)
#   --tag      the upstream base the series applies onto; REQUIRED with
#              --patches, because reconciling means knowing where the series
#              starts
set -euo pipefail

SRC=""; BUILD=""; JOBS=""; PATCHES=""; TAG=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --src)     SRC="$2";     shift 2 ;;
        --build)   BUILD="$2";   shift 2 ;;
        --jobs)    JOBS="$2";    shift 2 ;;
        --patches) PATCHES="$2"; shift 2 ;;
        --tag)     TAG="$2";     shift 2 ;;
        *) echo "clade-stage1: unknown argument '$1'" >&2; exit 2 ;;
    esac
done

die() { printf 'clade-stage1: %s\n' "$*" >&2; exit 1; }
phase() { echo; echo "=== [$(date -u +%H:%M:%S)] stage1: $* ==="; }

[[ -n "$SRC" ]] || die "--src is required"
[[ -f "$SRC/llvm/CMakeLists.txt" ]] || die "no LLVM tree at $SRC (missing llvm/CMakeLists.txt)"
BUILD="${BUILD:-$SRC/build}"
JOBS="${JOBS:-$(nproc 2>/dev/null || sysctl -n hw.ncpu)}"

# THE LOAD-BEARING SYMLINK. build.sh resolves the fork toolchain as
# "$fork/build/bin/..." -- and while POUCH_CC/POUCH_CXX can override the
# compilers, the host tblgens cannot: build_clade reads
# `$fork/build/bin/llvm-tblgen` and `$fork/build/bin/clang-tblgen` with no env
# override at all (tools/build.sh, the build_clade preamble). So overriding
# POUCH_CC/POUCH_CXX would get libcxx past its gate and then still skip clade
# at a different one. Making $src/build RESOLVE to the real output directory is
# the only fix that covers every "$fork/build/..." reference uniformly.
#
# This matters because the skip is SILENT and returns 0: build.sh prints
# "fork clang not built -- skipping" and succeeds, so `set -e` cannot catch it.
# An out-of-tree build directory without this link produces a green run that
# built nothing (observed: the first permanent-builder stage 2).
if [[ "$BUILD" != "$SRC/build" ]]; then
    if [[ -e "$SRC/build" && ! -L "$SRC/build" ]]; then
        die "$SRC/build exists and is not a symlink -- refusing to replace a real build tree"
    fi
    ln -sfn "$BUILD" "$SRC/build"
    phase "linked $SRC/build -> $BUILD (build.sh hardcodes \$fork/build)"
fi

# RECONCILE the series -- do not merely "apply it if it looks absent".
#
# The guard this replaces was `git log -1 | grep -q Thylacine`: skip the `git am`
# when the tip is already a Thylacine commit. That is idempotent, and it is also
# WRONG the moment the series GROWS -- which is the normal way this fork moves.
# On the permanent builder (whose whole point is a tree that survives) a sync
# that added a new fork commit would find a Thylacine tip, skip the apply, and
# build the OLD source while reporting success end to end. It is exactly the
# green-but-built-nothing shape this file's header warns about, one guard down.
# Caught before it cost a build, because the new commit was mine and I looked.
#
# Comparison is by `git patch-id --stable`, not by subject: format-patch FOLDS a
# long Subject: across continuation lines, so subject parsing is fragile in
# precisely the case that matters (a descriptive commit message). patch-id
# hashes the DIFF, gives identical values for a patch file and for the commit it
# became (verified on this series, 8/8), and so answers the real question --
# "is this exact change already applied here?"
#
# Three outcomes, and the middle one is why this is not just a reset:
#   identical  -> nothing to do
#   prefix     -> `am` only the new tail. This is the common case (the series is
#                 append-only in practice) and it is worth special-casing: a
#                 reset+re-am would rewrite every file the WHOLE series touches,
#                 including core headers like Triple.h, and both this build tree
#                 AND the cross-LLVM tree built from the same source would then
#                 rebuild the world. Applying just the tail touches just the
#                 tail's files.
#   divergent  -> reset to the base and re-apply. Correct for an amended or
#                 reordered series; pays the rebuild, which is the honest price
#                 of having changed history.
if [[ -n "$PATCHES" ]]; then
    phase "reconcile the fork patch series from $PATCHES"
    local_n="$(ls "$PATCHES"/*.patch 2>/dev/null | wc -l | tr -d ' ')"
    [[ "$local_n" -gt 0 ]] || die "no *.patch files in $PATCHES"
    [[ -n "$TAG" ]] || die "--patches requires --tag (the base the series applies onto)"
    git -C "$SRC" rev-parse --verify -q "$TAG^{commit}" >/dev/null \
        || die "base ref '$TAG' does not exist in $SRC"
    git -C "$SRC" config user.email builder@thylacine.local
    git -C "$SRC" config user.name  "Clade Builder"

    want="$(for p in "$PATCHES"/*.patch; do
                git patch-id --stable < "$p" | cut -d' ' -f1
            done)"
    have="$(git -C "$SRC" log --reverse --format=%H "$TAG..HEAD" | while read -r h; do
                git -C "$SRC" show "$h" | git patch-id --stable | cut -d' ' -f1
            done)"
    n_want="$(grep -c . <<< "$want" || true)"
    n_have="$(grep -c . <<< "$have" || true)"

    if [[ "$want" == "$have" ]]; then
        echo "series already applied verbatim ($n_have commits) -- nothing to do"
    elif [[ "$n_have" -lt "$n_want" && "$(head -n "$n_have" <<< "$want")" == "$have" ]]; then
        # Append-only: apply patches $n_have+1 .. $n_want, in order.
        echo "applied series is a prefix ($n_have of $n_want) -- applying the $(( n_want - n_have )) new patch(es)"
        # A glob straight into an array, not `mapfile`: mapfile is bash 4+, and
        # this script is also run from a bash-3.2 dev host. format-patch's names
        # are zero-padded, so glob order IS series order.
        all_patches=("$PATCHES"/*.patch)
        git -C "$SRC" am "${all_patches[@]:$n_have}"
    else
        echo "applied series DIVERGES from the payload ($n_have applied vs $n_want wanted)"
        echo "resetting to $TAG and re-applying the whole series"
        git -C "$SRC" reset --hard "$TAG"
        git -C "$SRC" am "$PATCHES"/*.patch
    fi

    # Assert the outcome rather than trusting the branch taken above.
    have="$(git -C "$SRC" log --reverse --format=%H "$TAG..HEAD" | while read -r h; do
                git -C "$SRC" show "$h" | git patch-id --stable | cut -d' ' -f1
            done)"
    [[ "$want" == "$have" ]] \
        || die "series reconcile FAILED: $SRC does not match the payload after applying"
    git -C "$SRC" log --oneline -"$local_n" | cat
fi

phase "cmake configure ($BUILD)"
cmake -G Ninja -S "$SRC/llvm" -B "$BUILD" \
  -DCMAKE_BUILD_TYPE=Release \
  -DLLVM_ENABLE_PROJECTS="clang;lld" \
  -DLLVM_TARGETS_TO_BUILD="AArch64" \
  -DLLVM_ENABLE_ASSERTIONS=OFF \
  -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_EXAMPLES=OFF \
  -DLLVM_INCLUDE_BENCHMARKS=OFF -DLLVM_INCLUDE_DOCS=OFF \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DLLVM_USE_LINKER=lld

phase "ninja -j$JOBS"
ninja -C "$BUILD" -j"$JOBS" \
  clang clang-resource-headers llvm-tblgen clang-tblgen \
  llvm-ar llvm-ranlib llvm-nm llvm-strip llvm-readelf lld

# ASSERT ON ARTIFACTS, NOT ON EXIT STATUS. Every tool stage 2 will reach for
# has to be here; a missing one must fail HERE, where it names itself, rather
# than downstream as a silent skip.
phase "verify the artifacts exist"
missing=""
for t in clang clang++ llvm-tblgen clang-tblgen llvm-ar llvm-ranlib llvm-nm \
         llvm-strip llvm-readelf ld.lld; do
    [[ -x "$BUILD/bin/$t" ]] || missing="$missing $t"
done
[[ -z "$missing" ]] || die "stage 1 produced no:$missing"
# `| head -2` under `set -o pipefail` is a SIGPIPE race: the reader exits early,
# and if the writer has anything left it dies on 141 and takes the script with
# it. clang --version is short enough to usually win, which is exactly what
# makes the pattern dangerous -- sed reads to EOF, so there is no race at all.
"$BUILD/bin/clang" --version | sed -n '1,2p'

# The one thing that makes the fork necessary at all. Prove it BEFORE stage 2
# spends an hour on a toolchain that cannot target the platform.
phase "verify the fork driver knows aarch64-thylacine"
probe="$(mktemp -d)"; trap 'rm -rf "$probe"' EXIT
echo 'int main(void){return 0;}' > "$probe/t.c"
if ! "$BUILD/bin/clang" --target=aarch64-thylacine -c "$probe/t.c" -o "$probe/t.o" 2>"$probe/err"; then
    echo "FORK DRIVER DOES NOT KNOW aarch64-thylacine -- the patch series did not apply as expected:" >&2
    cat "$probe/err" >&2
    exit 1
fi
echo "aarch64-thylacine: accepted"
phase "COMPLETE"
