#!/usr/bin/env bash
# Milestone B3 witness -- prove `git clone https://` works end to end under the
# VIVARIUM Linux phenotype: DNS (github.com via netd -> slirp) + TLS (OpenSSL over
# the socket, validated against the baked Mozilla CA bundle) + smart-http + pack,
# against a real external remote (github's canonical tiny octocat/Hello-World).
#
# NOT hermetic + NOT in `make test`. It needs live host internet (the guest reaches
# github through slirp's host-side NAT), so it is quarantined exactly like
# tools/phenonet/curl-demo.exp -- an operator-run witness for network git, never a
# member of the hermetic ladder (test.sh / the SMP gate / LS-CI). It bakes with
# THYLACINE_BAKE_GITNET=1, which stages the git-net container (the DEFAULT bake
# OMITS it, so joey's do_git_https_gate SOFT-SKIPS and the hermetic suite stays
# internet-free), boots, and asserts joey's `git-https gate PASS`.
#
# Usage:  tools/test-git-https.sh            # bake + boot + assert
#         BOOT_TIMEOUT=480 tools/test-git-https.sh
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"
LOG="build/test-boot.log"

# Pre-flight: the guest reaches github through THIS host's network, so if the host
# itself cannot, the bake would boot into a clone that can never succeed. SKIP
# (exit 77, the curl-demo convention) rather than redden -- a network test on a
# network-less host is unrun, not failed.
echo "==> git-https (B3): pre-flight -- can this host reach github over https?"
if ! git ls-remote --heads https://github.com/octocat/Hello-World.git >/dev/null 2>&1; then
    echo "==> git-https (B3): SKIP -- this host cannot reach github (run on a networked host)"
    exit 77
fi
echo "==> git-https (B3): host reaches github OK"

# NO /etc/hosts pin: net-4d (unconnected-UDP DNS-by-name) LANDED, so the clone
# RESOLVES github.com by name over musl's getaddrinfo -> the phenotype UDP path
# (vivarium recvmsg + non-blocking sendto) -> netd -> slirp 10.0.2.3. The gate now
# proves the FULL chain, DNS included; the GITHTTPS-DNS marker (a getent probe)
# pinpoints a resolver regression distinctly from a transport one. To re-isolate
# transport for debugging, set THYLACINE_GITNET_HOSTS="<ip> github.com" by hand --
# build.sh still honours it -- but the default is name resolution, as it should be.

# An EXPLICIT full bake -- NOT via test.sh, which builds only when the kernel ELF
# is MISSING (test.sh:132) and would otherwise boot a stale image, ignoring the
# bake-time env below. build.sh all runs stage_viv_bundles (stages git-net under
# BAKE_GITNET=1) + a fresh pool populate (PRESERVE=0) + re-bakes the ramfs with the
# rebuilt joey (the do_git_https_gate leg).
echo "==> git-https (B3): explicit full bake (build.sh all, BAKE_GITNET=1, fresh pool) ..."
if ! THYLACINE_MKFS_PRESERVE=0 THYLACINE_BAKE_GITNET=1 \
        caffeinate -i "$REPO_ROOT/tools/build.sh" all; then
    echo "==> git-https (B3): FAIL -- build.sh all errored"; exit 1
fi
# A bake-trap is ABSENT CONTENT -- verify by CONTENT, never the build's exit code.
# If the git-net bundle did not land, the gate would silently SKIP and the run
# would look green-but-vacuous.
if [[ ! -f "$REPO_ROOT/build/vivarium/git-net/config.json" ]]; then
    echo "==> git-https (B3): FAIL -- git-net bundle NOT staged (BAKE_GITNET guard or stage_viv_bundles did not run)"; exit 1
fi
echo "==> git-https (B3): git-net staged OK; booting the fresh image ..."
# test.sh now finds the fresh ELF present and just boots (build.sh above did the
# bake); it gives us the banner assertion + timeout + classification.
BOOT_TIMEOUT="${BOOT_TIMEOUT:-360}" caffeinate -i "$REPO_ROOT/tools/test.sh"
tsh_rc=$?
echo "==> git-https (B3): test.sh rc=$tsh_rc; scanning $LOG for the gate verdict ..."

if grep -q "joey: git-https gate PASS" "$LOG" 2>/dev/null; then
    n="$(grep -c '^GITHTTPS-' "$LOG" 2>/dev/null | tr -d ' ')"
    echo "==> git-https (B3): PASS -- $n GITHTTPS-* markers, network git works under the phenotype"
    grep -E "^GITHTTPS-|joey: git-https gate" "$LOG"
    exit 0
fi

echo "==> git-https (B3): FAIL -- the gate did not report PASS. Diagnosis follows."
echo "--- GITHTTPS-* markers + gate line (how far the clone got) ---"
grep -nE "^GITHTTPS-|joey: git-https gate" "$LOG" 2>/dev/null | head -20
echo "--- phenotype-gap signals to check (eventfd2 / ENOSYS / snare / resolver / TLS) ---"
grep -nE "eventfd|ENOSYS|snare:|Could not resolve|SSL|TLS|certificate|Couldn't connect|unable to|fatal:" "$LOG" 2>/dev/null | head -30
exit 1
