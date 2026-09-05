#!/usr/bin/env bash
# Milestone C1 witness -- the self-hosting FLOOR. Prove the NON-INTERACTIVE git
# developer workflow works end to end under the VIVARIUM Linux phenotype:
# branch / checkout / diff / status / merge (fast-forward AND a 3-way with a real
# conflict resolved by editing the marked file, no editor) / rebase
# (non-interactive) / reset / stash / worktree / manual gc.
#
# HERMETIC -- unlike tools/test-git-https.sh, this needs NO network (every verb is
# local), so there is no host-reachability pre-flight. It bakes with
# THYLACINE_BAKE_GITWF=1, which stages the git-workflow container (the DEFAULT bake
# OMITS it while the arc is in flight, so joey's do_git_workflow_gate SOFT-SKIPS
# and the default suite / SMP gate are unaffected), boots, and asserts joey's
# `git-workflow gate PASS`.
#
# It still needs the static-git tarball in build/cache (same input as git-probe);
# absent that, the bundle cannot stage and this SKIPs (exit 77) rather than
# reddening -- a missing fixture is unrun, not failed.
#
# Usage:  tools/test-git-workflow.sh          # bake + boot + assert
#         BOOT_TIMEOUT=480 tools/test-git-workflow.sh
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"
LOG="build/test-boot.log"

# The static-git tarball is the one required input (git-probe's too). Without it
# the git-workflow bundle cannot stage; SKIP rather than redden.
if ! ls build/cache/git-static-*-curl-aarch64-musl.tar.gz >/dev/null 2>&1 \
        && [[ -z "${THYLACINE_STATIC_GIT_TAR:-}" ]]; then
    echo "==> git-workflow (C1): SKIP -- no static-git tarball in build/cache/ (drop git-static-*-curl-aarch64-musl.tar.gz)"
    exit 77
fi

# An EXPLICIT full bake -- NOT via test.sh, which builds only when the kernel ELF
# is MISSING (test.sh:132) and would otherwise boot a stale image, ignoring the
# bake-time env below. build.sh all runs stage_viv_bundles (stages git-workflow
# under BAKE_GITWF=1) + a fresh pool populate (PRESERVE=0) + re-bakes the ramfs
# with the rebuilt joey (the do_git_workflow_gate leg).
echo "==> git-workflow (C1): explicit full bake (build.sh all, BAKE_GITWF=1, fresh pool) ..."
if ! THYLACINE_MKFS_PRESERVE=0 THYLACINE_BAKE_GITWF=1 \
        caffeinate -i "$REPO_ROOT/tools/build.sh" all; then
    echo "==> git-workflow (C1): FAIL -- build.sh all errored"; exit 1
fi
# A bake-trap is ABSENT CONTENT -- verify by CONTENT, never the build's exit code.
# If the git-workflow bundle did not land, the gate would silently SKIP and the
# run would look green-but-vacuous.
if [[ ! -f "$REPO_ROOT/build/vivarium/git-workflow/config.json" ]]; then
    echo "==> git-workflow (C1): FAIL -- git-workflow bundle NOT staged (BAKE_GITWF guard or stage_viv_bundles did not run)"; exit 1
fi
echo "==> git-workflow (C1): git-workflow staged OK; booting the fresh image ..."
# test.sh now finds the fresh ELF present and just boots (build.sh above did the
# bake); it gives us the banner assertion + timeout + classification.
BOOT_TIMEOUT="${BOOT_TIMEOUT:-360}" caffeinate -i "$REPO_ROOT/tools/test.sh"
tsh_rc=$?
echo "==> git-workflow (C1): test.sh rc=$tsh_rc; scanning $LOG for the gate verdict ..."

if grep -q "joey: git-workflow gate PASS" "$LOG" 2>/dev/null; then
    n="$(grep -c '^GITWF-' "$LOG" 2>/dev/null | tr -d ' ')"
    echo "==> git-workflow (C1): PASS -- $n GITWF-* markers, the non-interactive workflow works under the phenotype (self-hosting floor)"
    grep -E "^GITWF-|^WF-DIAG-|joey: git-workflow gate" "$LOG"
    exit 0
fi

echo "==> git-workflow (C1): FAIL -- the gate did not report PASS. Diagnosis follows."
echo "--- GITWF-* markers + the exit-code diagnostic + gate line (how far it got) ---"
grep -nE "^GITWF-|^WF-DIAG-|joey: git-workflow gate" "$LOG" 2>/dev/null | head -40
echo "--- phenotype-gap signals to check (ENOSYS / snare / fatal) ---"
grep -nE "ENOSYS|snare:|fatal:|not a git repo|Function not implemented|cannot" "$LOG" 2>/dev/null | head -30
exit 1
