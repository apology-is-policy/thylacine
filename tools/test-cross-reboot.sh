#!/usr/bin/env bash
# tools/test-cross-reboot.sh — A-1b corvus identity-DB cross-reboot persistence.
#
# The regression guard for the three-bug masking stack that masqueraded as
# "AEGIS-256 corruption" for ~a year (#710/712/714/H1) and was killed at the
# A-1b close (Thylacine 573b984 + Stratum 91ae5d8):
#   1. bdev_thylacine op_write zero-padded the partial tail sector -> clobbered
#      an adjacent on-disk object  (Stratum, op_write partial-tail RMW).
#   2. corvus persist_keypair_wrap fsync'd the file but not the containing dirs
#      -> the wrap dirent was not durable across a non-clean unmount
#      (Thylacine, CORVUS-DESIGN.md section 16.6).
#   3. fs_read_regular_locked passed a raw file offset to stm_sync_read_extent
#      -> EINVAL on the 2nd chunked Tread (offset 2048)  (Stratum, the new
#      fs_read_extent_aligned_locked helper).
#
# WHY a dedicated cross-reboot harness: the original A-1b "close" was hollow
# because the single-boot E2E never read a secret extent BACK from disk on a
# cold cache. A same-boot write-then-read masks both the read-offset bug (warm
# DMA buffer returns stale-but-correct bytes) and the dirent-durability bug
# (the dirent is in the warm FS cache). Only a genuine reboot on the same pool
# exercises the cold read-back + dirent durability + identity.db rename-swap
# all at once. This is exactly the gap that let the bug hide. See
# docs/DEBUGGING-PLAYBOOK.md.
#
# Mechanism: run-vm.sh attaches build/fixtures/pool.img as `format=raw` with NO
# -snapshot, so two consecutive test.sh runs share + mutate one pool. boot1
# starts from a freshly-baked (clean) pool so it is a genuine first-create;
# boot2 runs on the SAME pool (no re-bake) so it must LOAD what boot1 wrote.
#
# Flake handling: the 16c boot path is bimodally reliable -- stratumd
# occasionally does not bind /srv/stratum-fs before joey's retry budget lapses
# (QEMU-TCG worker-thread scheduling; see tools/test.sh BOOT_TIMEOUT comments).
# That is an INFRA flake unrelated to persistence, and it fires BEFORE corvus
# runs (joey never reaches the corvus harness), so the pool is untouched. We
# therefore retry a boot that fails to reach "Thylacine boot OK" up to
# MAX_BOOT_TRIES, re-baking before each boot1 try so boot1 is always a clean
# first-create. A boot that DOES reach "Thylacine boot OK" but is missing a
# persistence marker is a REAL regression -> hard fail, never retried. A flaky
# guard that gets ignored is worse than no guard; this keeps the failure signal
# trustworthy.
#
# Prerequisite: a current kernel + ramfs in build/ (run `tools/build.sh all`
# or at least `kernel`+`userspace` first). test.sh auto-builds the kernel only
# if the ELF is entirely missing -- it does NOT detect a stale ELF.
#
# Coupling: the assertions below match the joey corvus-harness boot lines
# (usr/joey/joey.c). If that harness changes the probe users or wording, update
# the markers here in the same change -- a loud break is correct.

set -uo pipefail   # NOT -e: test.sh returns non-zero on a boot flake we retry.

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$REPO_ROOT/build"
LOG_FILE="$BUILD_DIR/test-boot.log"          # test.sh overwrites this each run
BOOT1_LOG="$BUILD_DIR/test-cross-reboot-boot1.log"
BOOT2_LOG="$BUILD_DIR/test-cross-reboot-boot2.log"

MAX_BOOT_TRIES="${CROSS_REBOOT_BOOT_TRIES:-6}"   # absorb the 16c boot flake
# The 16c boot path is slow under QEMU-TCG and slower still on a loaded host
# (a competing CPU hog starves the emulated vCPUs). Give each boot generous
# headroom so a slow-but-correct boot is not mis-counted as a timeout flake.
# Overridable; the per-boot retry above still absorbs the genuine stratumd-bind
# flake on top of this.
export BOOT_TIMEOUT="${BOOT_TIMEOUT:-120}"

# boot1 (fresh pool): susan is created for the first time.
BOOT1_CREATE_MARKER="USER_CREATE susan ok"
# boot2 (same pool, after reboot): the identity.db survived AND the secret
# keypair wrap reloaded from disk + the AEAD DEK round-trip verified. The first
# proves identity.db rename-swap + dirent durability; the second proves the
# cold secret-extent read-back (fixes 1 + 3).
BOOT2_PERSIST_MARKER="USER_CREATE susan already provisioned"
BOOT2_WRAP_MARKER="UNWRAP users/michael ok (DEK round-trip verified)"
BOOT_OK_MARKER="Thylacine boot OK"

fail_regression() {   # boot reached OK but a persistence marker is missing
    echo "==> FAIL (cross-reboot REGRESSION): $1" >&2
    exit 1
}
fail_infra() {        # could not get a clean boot after retries (genuinely slow boot)
    echo "==> FAIL (cross-reboot SLOW-BOOT, not a persistence regression): $1" >&2
    echo "    The boot did not reach boot-OK in $MAX_BOOT_TRIES tries (no extinction, no" >&2
    echo "    corruption signature). Likely host CPU contention vs QEMU-TCG; raise" >&2
    echo "    BOOT_TIMEOUT / free the host and re-run." >&2
    exit 1
}
# Signatures of the recurring content-sensitive stratumd-mount AEAD failure
# (STM_EBADTAG = -201; the "AEGIS-256 corruption"). A boot that hits this is NOT
# a flake and MUST NOT be retried past -- re-baking a fresh pool each retry would
# MASK a content-sensitive correctness bug (the exact anti-pattern that hid this
# for a year). Surface it as a hard failure. Tracked as the EBADTAG DFS.
CORRUPTION_SIG='run failed \(rc=-201\)|STM_EBADTAG|wait_pid returned wrong pid|^EXTINCTION:'
fail_corruption() {
    echo "==> FAIL (cross-reboot CORRUPTION, NOT a flake): $1" >&2
    echo "    Signature of the recurring stratumd-mount AEAD failure (STM_EBADTAG / -201):" >&2
    echo "    a content-sensitive read-back correctness bug. Do NOT dismiss as a flake." >&2
    exit 1
}
check_corruption() {  # $1 = saved boot log; hard-fail (no retry) if the bug fired
    if grep -qE "$CORRUPTION_SIG" "$1" 2>/dev/null; then
        fail_corruption "boot log shows the stratumd-mount AEAD failure (see $1)"
    fi
}

# Boot once; copy the log to $1. Return 0 iff "Thylacine boot OK" was reached.
boot_to_log() {
    local saved="$1"
    local rc=0
    "$REPO_ROOT/tools/test.sh" >/dev/null 2>&1 || rc=$?
    cp -f "$LOG_FILE" "$saved" 2>/dev/null || true
    [[ "$rc" -eq 0 ]] && return 0 || return 1
}

# 1. boot1 -- fresh pool each try (so it is always a first-create), retried past
#    the infra flake. Writes michael + susan + the wrap + identity.db.
boot1_ok=0
for try in $(seq 1 "$MAX_BOOT_TRIES"); do
    echo "==> cross-reboot: re-baking a clean pool, boot1 attempt $try/$MAX_BOOT_TRIES"
    "$REPO_ROOT/tools/build.sh" pool >/dev/null 2>&1 || fail_infra "build.sh pool failed"
    if boot_to_log "$BOOT1_LOG"; then boot1_ok=1; break; fi
    check_corruption "$BOOT1_LOG"   # hard-fail on the EBADTAG bug; never retry-mask it
    echo "==> cross-reboot: boot1 did not reach boot-OK (no corruption sig; slow boot); retrying"
done
[[ "$boot1_ok" -eq 1 ]] || fail_infra "boot1 never reached '$BOOT_OK_MARKER'"
grep -q "$BOOT1_CREATE_MARKER" "$BOOT1_LOG" \
    || fail_regression "boot1 reached boot-OK but is missing '$BOOT1_CREATE_MARKER' -- the pool was NOT clean (susan already existed) or the corvus create path regressed (see $BOOT1_LOG)"
echo "==> cross-reboot: boot1 OK -- susan created fresh, state written to pool.img"

# 2. boot2 -- SAME pool, NO re-bake. Must load the persisted identity.db AND
#    reload michael's secret keypair wrap from disk (the cold read-back).
#    Persistence is idempotent, so retrying boot2 on the same pool is safe.
boot2_ok=0
for try in $(seq 1 "$MAX_BOOT_TRIES"); do
    echo "==> cross-reboot: boot2 (reboot, same pool) attempt $try/$MAX_BOOT_TRIES"
    if boot_to_log "$BOOT2_LOG"; then boot2_ok=1; break; fi
    check_corruption "$BOOT2_LOG"   # hard-fail on the EBADTAG bug; never retry-mask it
    echo "==> cross-reboot: boot2 did not reach boot-OK (no corruption sig; slow boot); retrying"
done
[[ "$boot2_ok" -eq 1 ]] || fail_infra "boot2 never reached '$BOOT_OK_MARKER'"
grep -q "$BOOT2_PERSIST_MARKER" "$BOOT2_LOG" \
    || fail_regression "boot2 missing '$BOOT2_PERSIST_MARKER' -- identity.db did NOT persist across reboot (fix 1/2 regressed?; see $BOOT2_LOG)"
grep -q "$BOOT2_WRAP_MARKER" "$BOOT2_LOG" \
    || fail_regression "boot2 missing '$BOOT2_WRAP_MARKER' -- the secret keypair wrap did NOT reload from disk (fix 1/3 regressed? cold extent read-back broken; see $BOOT2_LOG)"

echo "==> PASS (cross-reboot): identity.db + secret keypair wrap survived a reboot."
echo "    boot1: susan created fresh   ($BOOT1_LOG)"
echo "    boot2: susan persisted + michael wrap reloaded + DEK round-trip ($BOOT2_LOG)"
exit 0
