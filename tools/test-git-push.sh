#!/usr/bin/env bash
# Milestone B N-6 witness -- prove `git push https://` works end to end under the
# VIVARIUM Linux phenotype, against a real writable remote. This is the LAST of
# milestone B's exit criteria (clone + fetch are already gated by
# tools/test-git-https.sh, which this mirrors).
#
# NOT hermetic + NOT in `make test`: it needs live host internet AND a writable
# remote + a credential, so it is operator-run only, exactly like
# test-git-https.sh. It bakes with THYLACINE_BAKE_GITNET=1 + the push env, boots,
# and asserts joey's `git-https gate PASS` AND the GITHTTPS-PUSH marker.
#
# CREDENTIAL HYGIENE: the PAT is passed ONLY via the env (THYLACINE_GITNET_PAT).
# This script NEVER prints it; build.sh writes it solely into a gitignored guest
# artifact (build/vivarium/git-net/rootfs/tmp/.gitnet-push-token, 0600) that the
# guest reads through an inline credential helper -- the remote URL stays clean,
# so the token reaches no git output and no boot log. Use a FINE-GRAINED PAT
# scoped to just the sandbox repo (Contents: write), and REVOKE it after the run.
#
# Usage:
#   THYLACINE_GITNET_PUSH_URL=https://github.com/<you>/<sandbox>.git \
#   THYLACINE_GITNET_PAT=<fine-grained PAT, Contents:write> \
#       tools/test-git-push.sh
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"
LOG="build/test-boot.log"

: "${THYLACINE_GITNET_PUSH_URL:?set THYLACINE_GITNET_PUSH_URL to your sandbox repo https URL}"
: "${THYLACINE_GITNET_PAT:?set THYLACINE_GITNET_PAT to a fine-grained PAT with Contents write -- never printed or committed}"

# Pre-flight: the guest reaches the remote through THIS host's network, so if the
# host itself cannot reach github, the bake would boot into a push that can never
# succeed. SKIP (exit 77, the curl-demo convention), not redden.
echo "==> git-push (N-6): pre-flight -- can this host reach github over https?"
if ! git ls-remote --heads https://github.com/octocat/Hello-World.git >/dev/null 2>&1; then
    echo "==> git-push (N-6): SKIP -- this host cannot reach github (run on a networked host)"
    exit 77
fi
echo "==> git-push (N-6): host reaches github OK; push target = ${THYLACINE_GITNET_PUSH_URL}"

# An EXPLICIT full bake (NOT via test.sh, which boots a stale image if the ELF is
# present): build.sh all runs stage_viv_bundles (stages git-net under
# BAKE_GITNET=1 + provisions the push token/URL from the env) + a fresh pool
# populate + re-bakes the ramfs.
echo "==> git-push (N-6): explicit full bake (build.sh all, BAKE_GITNET=1 + push env, fresh pool) ..."
if ! THYLACINE_MKFS_PRESERVE=0 THYLACINE_BAKE_GITNET=1 \
        caffeinate -i "$REPO_ROOT/tools/build.sh" all; then
    echo "==> git-push (N-6): FAIL -- build.sh all errored"; exit 1
fi
# A bake-trap is ABSENT CONTENT -- verify by CONTENT, never the exit code. If the
# push token did not land, the push leg SOFT-SKIPS and the run looks green-but-
# vacuous (clone+fetch only).
if [[ ! -f "$REPO_ROOT/build/vivarium/git-net/rootfs/tmp/.gitnet-push-token" ]]; then
    echo "==> git-push (N-6): FAIL -- push token NOT provisioned into the guest (env unset at bake, or the build.sh gate did not run)"; exit 1
fi
echo "==> git-push (N-6): push target provisioned OK; booting the fresh image ..."

BOOT_TIMEOUT="${BOOT_TIMEOUT:-360}" caffeinate -i "$REPO_ROOT/tools/test.sh"
tsh_rc=$?
echo "==> git-push (N-6): test.sh rc=$tsh_rc; scanning $LOG for the push verdict ..."

# The witness needs BOTH: the gate PASS (clone+fetch+push+DONE all fired) AND the
# GITHTTPS-PUSH marker specifically (a green gate WITHOUT it would mean the push
# leg soft-skipped -- not a push proof).
if grep -q "joey: git-https gate PASS" "$LOG" 2>/dev/null && grep -q '^GITHTTPS-PUSH' "$LOG" 2>/dev/null; then
    echo "==> git-push (N-6): PASS -- git push over https works under the phenotype"
    grep -E "^GITHTTPS-(CLONE|FETCH|PUSH|DONE)|joey: git-https gate" "$LOG"
    exit 0
fi

echo "==> git-push (N-6): FAIL -- the push did not land. Diagnosis follows."
echo "--- GITHTTPS-* markers (how far it got) ---"
grep -nE "^GITHTTPS-|joey: git-https gate" "$LOG" 2>/dev/null | head -20
echo "--- phenotype-gap / auth signals (eventfd2 / ENOSYS / snare / 401/403 / TLS) ---"
grep -nE "eventfd|ENOSYS|snare:|denied|permission|403|401|unable to|fatal:|rejected|Authentication" "$LOG" 2>/dev/null | head -30
exit 1
