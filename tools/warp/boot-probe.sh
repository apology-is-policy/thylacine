#!/bin/bash
# Boot Thylacine headless ON THIS HOST (the GL host of docs/GPU-HOST-SETUP.md)
# and assert positive evidence. The Warp-1 remote half; driven over ssh by
# tools/warp-host.sh, runnable by hand.
#
#   boot-probe.sh <tag> [gpu_dev] [display]
#
# e.g.  boot-probe.sh smoke
#       boot-probe.sh capset virtio-gpu-gl-pci egl-headless
#
# Working copies live under $WARP_WORK on DISK, never /tmp -- Debian 13 /tmp
# is tmpfs, and a ~1 GiB pool copy there would eat guest RAM. Fixture
# isolation per #78/#85: the synced images are never booted directly.
# Exit 0 iff 'Thylacine boot OK' was observed.
set -u

TAG="${1:?usage: boot-probe.sh <tag> [gpu_dev] [display]}"
GPU_DEV="${2:-}"
DISP="${3:-}"
REPO="${WARP_REPO:-$HOME/projects/thylacine}"
WORK="${WARP_WORK:-$HOME/warp}"
# 5 s per poll. 180 fits the TCG GL hosts; an SD-backed pool's boot-test
# suite (two go-probe legs are FS-round-trip-bound) needs more wall clock.
POLLS="${WARP_BOOT_POLLS:-180}"

mkdir -p "$WORK"
cd "$REPO"
# Checked copies (audit W1 F4): an unchecked cp against a missing/partial
# sync would boot the PREVIOUS run's fixture -- or a pool-less guest --
# under today's tag, and the banner-gated PASS would not notice.
cp --sparse=always build/fixtures/pool.img "$WORK/pool-$TAG.img" || {
    echo "BOOT-$TAG: FIXTURE-COPY-FAILED (pool)"
    exit 2
}
cp build/disk.img "$WORK/disk-$TAG.img" || {
    echo "BOOT-$TAG: FIXTURE-COPY-FAILED (disk)"
    exit 2
}
LOG="$WORK/$TAG-boot.log"

env THYLACINE_POOL_IMG="$WORK/pool-$TAG.img" \
    THYLACINE_DISK_IMG="$WORK/disk-$TAG.img" \
    THYLACINE_NOSTORM=1 THYLACINE_NO_QMP=1 THYLACINE_NO_SHARE=1 \
    ${GPU_DEV:+THYLACINE_GPU_DEV="$GPU_DEV"} \
    ${DISP:+THYLACINE_DISPLAY="$DISP"} \
    tools/run-vm.sh ${WARP_QEMU_DBG:-} </dev/null > "$LOG" 2>&1 &
VMPID=$!

# Bounded poll (#134: deadline + named failure conventions), kill by PID.
ok=0
i=0
for i in $(seq 1 "$POLLS"); do
    if ! kill -0 "$VMPID" 2>/dev/null; then
        echo "BOOT-$TAG: QEMU-EXITED-EARLY"
        break
    fi
    if grep -aq "Thylacine boot OK" "$LOG"; then
        ok=1
        break
    fi
    if grep -aq "EXTINCTION:" "$LOG"; then
        echo "BOOT-$TAG: EXTINCTION"
        break
    fi
    sleep 5
done
echo "BOOT-$TAG: polls=$i (~$((i * 5))s)"

kill "$VMPID" 2>/dev/null
sleep 1
kill -9 "$VMPID" 2>/dev/null
wait "$VMPID" 2>/dev/null

if [ "$ok" -eq 1 ]; then
    echo "BOOT-$TAG: PASS"
else
    echo "BOOT-$TAG: FAIL"
    tail -40 "$LOG"
fi
# The gpu driver's own evidence lines (capset probe etc.) + the warp server's
# host3d-ring self-test line (V-3b-1c-2a lives in server.rs, prefix "warp", not
# "gpu") + the ring-recreate self-test line (V-3b-3c-1, the F1 ridx-reuse
# regression witness) + the mem-recreate self-test line (V-3b-3c-2, the
# device-memory handle-reuse witness) -- emitted for the caller to classify;
# grep -a because a serial log can carry binary bytes. The alternatives are
# scoped to the specific self-test prefixes so the many routine
# `tapestryd: warp ...` ctx/ring diagnostics do not flood the log.
#
# EVERY STRING A VERDICT ARM GREPS FOR MUST HAVE AN ALTERNATIVE HERE. This is
# the CAPTURE half of the gate and it is the half a crafted-log suite cannot
# test: `tools/test-venus-verdict.sh` writes the witness lines into its
# fixtures by hand, so a line the verdict demands and this filter drops is
# GREEN there and RED on every real boot (W-3c-1 audit F2 -- the presentable
# arm was added to the verdict and not to this line, which would have made
# `warp-host.sh venus` unpassable on a healthy host, deterministically).
#
# IT HAPPENED AGAIN AT ROUND 3, IN THE OTHER DIRECTION, WHICH IS WHY THIS
# PARAGRAPH IS NOT ENOUGH ON ITS OWN. Round 2 added a verdict arm greping for
# "UNBIND REFUSED by the device" and did not add an alternative here -- and
# the reason it slipped is the instructive part: the refusal say-line was
# MOVED from `wimg_teardown` (prefix `tapestryd: warp presentable`, already
# captured) into `gl_evict_res`, where it correctly gained the prefix
# `tapestryd: warp display` because it now serves every family. The pairing
# was verified BEFORE the move and the comment asserting it was left behind,
# true about a line that no longer existed. **A prefix change is a capture
# change.** When you move a say-line between functions, re-check this filter.
grep -aE "tapestryd: gpu|tapestryd: warp host3d-ring|tapestryd: warp ring-recreate|tapestryd: warp mem-recreate|tapestryd: warp scanout-blob|tapestryd: warp presentable|tapestryd: warp display|tapestryd: scanout|THYLACINE-VENUS-PROVE|venus-prove:|THYLACINE-VK-SDL-PROVE|vk-sdl-prove:" "$LOG" || true
exit $((1 - ok))
