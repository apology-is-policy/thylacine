#!/usr/bin/env bash
# tools/warp-host.sh -- the Warp GL-host leg (GPU-DESIGN.md section 12,
# Warp-1). Drives the remote GL host (docs/GPU-HOST-SETUP.md; default the
# `thyla-gl` ssh alias) from the build host: artifact sync, a plain smoke
# boot, the llvmpipe baseline bench, and the virtio-gpu-gl capset boot.
#
#   tools/warp-host.sh sync      # repo (git archive HEAD) + boot artifacts
#   tools/warp-host.sh smoke     # boot to 'Thylacine boot OK' (2D device)
#   tools/warp-host.sh bench     # llvmpipe GLQuake baseline (paced + unpaced x2)
#   tools/warp-host.sh capset    # virtio-gpu-gl-pci + egl-headless capset probe
#   tools/warp-host.sh prove     # Warp-2 gate: /warp-prove on the virgl device
#
# Verification is fail-closed: each leg greps for its own positive evidence
# and exits non-zero without it. `bench` runs FOREGROUND (~20-45 min under
# TCG) -- detach at the caller if needed. WARP_HOST overrides the alias.
#
# The build host stays the Mac: the VM has no KVM for ARM64 guests (TCG
# only), so `sync` pushes artifacts built here; it never builds remotely.
set -euo pipefail

HOST="${WARP_HOST:-thyla-gl}"
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
RREPO='~/projects/thylacine'

usage() { sed -n '2,16p' "$0"; exit 2; }
[[ $# -ge 1 ]] || usage
cmd="$1"

sync_all() {
    # git archive HEAD: exactly the committed tree (no build/, no .git, no
    # local junk). Uncommitted script changes ride separately below so a
    # dirty iteration loop still tests what you edited.
    ssh "$HOST" "mkdir -p $RREPO/build/kernel $RREPO/build/fixtures $RREPO/share"
    git -C "$REPO_ROOT" archive HEAD | ssh "$HOST" "tar -x -C $RREPO"
    for f in tools/run-vm.sh tools/warp/boot-probe.sh tools/warp/glq-bench.exp tools/warp/warp-prove.exp; do
        scp -q "$REPO_ROOT/$f" "$HOST:$RREPO/$(dirname "$f")/"
    done
    echo "== artifacts =="
    scp -q "$REPO_ROOT"/build/kernel/thylacine.bin "$REPO_ROOT"/build/kernel/thylacine.elf \
        "$HOST:$RREPO/build/kernel/"
    scp -q "$REPO_ROOT"/build/ramfs.cpio "$REPO_ROOT"/build/disk.img "$HOST:$RREPO/build/"
    # The pool travels as gzip -> dd conv=sparse: macOS ships openrsync
    # (no dependable --sparse), and a raw copy would inflate 906M-in-5G
    # to the full 5G on the wire and on the VM disk.
    echo "== pool.img (sparse) =="
    gzip -1 -c "$REPO_ROOT"/build/fixtures/pool.img |
        ssh "$HOST" "gunzip -c | dd of=\$HOME/projects/thylacine/build/fixtures/pool.img conv=sparse bs=1M status=none"
    ssh "$HOST" "du -h $RREPO/build/fixtures/pool.img; ls -l $RREPO/build/kernel/thylacine.bin $RREPO/build/ramfs.cpio"
    echo "SYNC-DONE"
}

case "$cmd" in
sync)
    sync_all
    ;;
smoke)
    ssh "$HOST" "bash $RREPO/tools/warp/boot-probe.sh smoke"
    ;;
bench)
    out="$REPO_ROOT/build/warp-bench.log"
    ssh "$HOST" "cd $RREPO && expect tools/warp/glq-bench.exp" | tee "$out"
    echo "== bench verdict =="
    if ! grep -q "WARP-BENCH PASS" "$out"; then
        echo "BENCH UNVERIFIED -- no PASS line"
        exit 1
    fi
    if grep -q "WARP-BENCH SWAPPED" "$out"; then
        echo "BENCH SWAPPED -- figures measure swapping, DISCARD"
        exit 1
    fi
    grep "WARP-BENCH" "$out"
    ;;
capset)
    out="$REPO_ROOT/build/warp-capset.log"
    ssh "$HOST" "bash $RREPO/tools/warp/boot-probe.sh capset virtio-gpu-gl-pci egl-headless" |
        tee "$out" || true
    echo "== capset verdict =="
    # The Warp-1 gate: a GET_CAPSET response round-tripped in-guest. The
    # boot must ALSO have passed -- a capset line from a boot that then
    # extincted proves nothing.
    if grep -q "BOOT-capset: PASS" "$out" && grep -q "gpu GET_CAPSET id=" "$out"; then
        echo "CAPSET GATE: VERIFIED"
    else
        echo "CAPSET GATE: UNVERIFIED (need BOOT-capset PASS + a GET_CAPSET line)"
        exit 1
    fi
    ;;
prove)
    out="$REPO_ROOT/build/warp-prove.log"
    ssh "$HOST" "cd $RREPO && expect tools/warp/warp-prove.exp" | tee "$out" || true
    echo "== prove verdict =="
    # The Warp-2 gate: the prover's OWN pass line (sentinel readback + the
    # post-destroy ctx count are asserted in-guest) AND the scenario pass
    # (the boot + login around it held). Either alone is not the gate.
    if grep -q "WARP-PROVE PASS" "$out" && grep -q "PASS: warp-prove" "$out"; then
        echo "WARP-2 GATE: VERIFIED"
    else
        echo "WARP-2 GATE: UNVERIFIED (need WARP-PROVE PASS + the scenario pass line)"
        exit 1
    fi
    ;;
*)
    usage
    ;;
esac
