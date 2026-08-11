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
#   tools/warp-host.sh quake     # Warp-4 gate: GLQuake on virgl, both present arms
#   tools/warp-host.sh decomp gl|2d  # #196: unpaced per-arm figures + CPU attribution
#   tools/warp-host.sh wedge     # #210: direct-arm wedge autopsy (paced vs unpaced)
#
# Verification is fail-closed: each leg greps for its own positive evidence
# and exits non-zero without it. `bench` runs FOREGROUND (~20-45 min under
# TCG) -- detach at the caller if needed. WARP_HOST overrides the alias;
# WARP_ACCEL=kvm pins the expect legs' accel (default tcg) on KVM GL hosts;
# WARP_BOOT_TIMEOUT / WARP_BOOT_POLLS widen the boot budgets (slow pools).
#
# The build host stays the Mac: the VM has no KVM for ARM64 guests (TCG
# only), so `sync` pushes artifacts built here; it never builds remotely.
set -euo pipefail

HOST="${WARP_HOST:-thyla-gl}"
# Remote-env prefix for the expect legs: WARP_ACCEL rides as THYLACINE_ACCEL
# (the .exp default is tcg; KVM GL hosts like thyla-pi pass kvm) and
# WARP_BOOT_TIMEOUT as LS_CI_BOOT_TIMEOUT (the .exp files floor it at 900;
# a larger caller value passes through -- SD-backed pools boot slower than
# that). boot-probe legs auto-detect accel; WARP_BOOT_POLLS is their bound.
RENV="${WARP_ACCEL:+THYLACINE_ACCEL=$WARP_ACCEL }${WARP_BOOT_TIMEOUT:+LS_CI_BOOT_TIMEOUT=$WARP_BOOT_TIMEOUT }"
RPOLLS="${WARP_BOOT_POLLS:+WARP_BOOT_POLLS=$WARP_BOOT_POLLS }"
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
RREPO='~/projects/thylacine'

usage() { sed -n '2,18p' "$0"; exit 2; }
[[ $# -ge 1 ]] || usage
cmd="$1"

sync_all() {
    # git archive HEAD: exactly the committed tree (no build/, no .git, no
    # local junk). Uncommitted script changes ride separately below so a
    # dirty iteration loop still tests what you edited.
    ssh "$HOST" "mkdir -p $RREPO/build/kernel $RREPO/build/fixtures $RREPO/share"
    git -C "$REPO_ROOT" archive HEAD | ssh "$HOST" "tar -x -C $RREPO"
    for f in tools/run-vm.sh tools/warp/boot-probe.sh tools/warp/glq-bench.exp tools/warp/warp-prove.exp tools/warp/virgl-prove.exp tools/warp/glq-virgl.exp tools/warp/glq-decomp.exp tools/warp/glq-wedge-probe.exp tools/interactive/gfx_strip.py; do
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
    ssh "$HOST" "${RPOLLS}bash $RREPO/tools/warp/boot-probe.sh smoke"
    ;;
bench)
    out="$REPO_ROOT/build/warp-bench.log"
    ssh "$HOST" "cd $RREPO && ${RENV}expect tools/warp/glq-bench.exp" | tee "$out"
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
    ssh "$HOST" "${RPOLLS}bash $RREPO/tools/warp/boot-probe.sh capset virtio-gpu-gl-pci egl-headless" |
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
    ssh "$HOST" "cd $RREPO && ${RENV}expect tools/warp/warp-prove.exp" | tee "$out" || true
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
tri)
    out="$REPO_ROOT/build/warp-tri.log"
    ssh "$HOST" "cd $RREPO && ${RENV}expect tools/warp/virgl-prove.exp" | tee "$out" || true
    echo "== tri verdict =="
    # The Warp-3 gate, same two-line shape as prove: the prover's own pass
    # (GL_RENDERER discriminator + clear/triangle pixels through the fenced
    # readback, asserted in-guest) AND the scenario pass around it. The
    # prover cannot pass on a llvmpipe fallback by construction (no CAP_JIT
    # walk), so the anchor's job is only "the success path printed it" --
    # and only the success path can (#186: the expect script's own failure
    # text quotes the pattern it waited for, which is why the SCENARIO line
    # is required alongside).
    # The scenario anchor is lc_pass's OWN prefix ("LS-CI PASS: virgl-prove:"),
    # NOT the command path: lc_run_expect's TIMEOUT text is the only place the
    # path appears next to PASS ("waiting for [...] during 'output-of:
    # /clade/bin/virgl-prove'"), so the original path-shaped anchor matched no
    # passing run and could never fire (first contact: triangle rendered,
    # verdict UNVERIFIED).
    if grep -q "VIRGL-PROVE PASS" "$out" && grep -q "LS-CI PASS: virgl-prove:" "$out"; then
        echo "WARP-3 GATE: VERIFIED"
    else
        echo "WARP-3 GATE: UNVERIFIED (need VIRGL-PROVE PASS + the scenario pass line)"
        exit 1
    fi
    ;;
quake)
    out="$REPO_ROOT/build/warp-quake.log"
    ssh "$HOST" "cd $RREPO && ${RENV}expect tools/warp/glq-virgl.exp" | tee "$out" || true
    echo "== quake verdict =="
    # The Warp-4 gate: the tri two-line shape plus the swap certification.
    # GLQ-VIRGL PASS = every gated leg held in-script (virgl renderer +
    # composed dump + the direct-switch say line + direct dump + demo
    # completion + the eviction leg); the LS-CI line = the scenario around
    # it (boot + login + teardown) held; SWAPPED = the figures measured
    # swapping, so the run is void regardless of the other two.
    if grep -q "GLQ-VIRGL SWAPPED" "$out"; then
        echo "WARP-4 GATE: SWAPPED -- figures measure swapping, DISCARD"
        exit 1
    fi
    if grep -q "GLQ-VIRGL PASS" "$out" && grep -q "LS-CI PASS: glq-virgl:" "$out"; then
        grep "GLQ-VIRGL" "$out"
        echo "WARP-4 GATE: VERIFIED"
    else
        echo "WARP-4 GATE: UNVERIFIED (need GLQ-VIRGL PASS + the scenario pass line)"
        exit 1
    fi
    # Fetch the dumps: the orientation strips are calibrated look-first
    # against the real frames before any threshold gates on them.
    scp -q "$HOST:warp/glq-virgl-1.png" "$HOST:warp/glq-virgl-2.png" \
        "$REPO_ROOT/build/" 2>/dev/null || echo "(dump fetch failed -- non-fatal)"
    ;;
decomp)
    # #196: the throughput decomposition -- `decomp gl` (virgl, both
    # present arms) then `decomp 2d` (llvmpipe, the resolution-matched
    # software control + the pegged-CPU calibration). Figures REPORTED;
    # the verdict conjunction gates only the structural legs.
    sub="${2:-}"
    if [[ "$sub" != gl && "$sub" != 2d ]]; then
        echo "usage: tools/warp-host.sh decomp gl|2d"
        exit 2
    fi
    out="$REPO_ROOT/build/warp-decomp-$sub.log"
    ssh "$HOST" "cd $RREPO && WARP_DECOMP_DEV=$sub ${RENV}expect tools/warp/glq-decomp.exp" | tee "$out" || true
    echo "== decomp $sub verdict =="
    if grep -q "GLQ-DECOMP SWAPPED" "$out"; then
        echo "DECOMP $sub: SWAPPED -- discard the flagged figure(s), rerun"
        exit 1
    fi
    if grep -q "GLQ-DECOMP PASS $sub" "$out" && grep -q "LS-CI PASS: glq-decomp-$sub:" "$out"; then
        grep "GLQ-DECOMP" "$out"
        echo "DECOMP $sub: MEASURED"
    else
        echo "DECOMP $sub: UNVERIFIED (need GLQ-DECOMP PASS $sub + the scenario pass line)"
        exit 1
    fi
    ;;
wedge)
    # #210: the direct-arm wedge autopsy. The verdicts are evidence either
    # way (PROGRESSES isolates pacing; WEDGED captures kstacks), so the
    # gate is only that the probe ran to completion.
    out="$REPO_ROOT/build/warp-wedge.log"
    ssh "$HOST" "cd $RREPO && ${RENV}expect tools/warp/glq-wedge-probe.exp" | tee "$out" || true
    echo "== wedge verdict =="
    if grep -q "WEDGE-PROBE PASS" "$out" && grep -q "LS-CI PASS: glq-wedge-probe:" "$out"; then
        grep "WEDGE-PROBE" "$out" | grep -v AUTOPSY- | head -8
        echo "WEDGE PROBE: CAPTURED"
    else
        echo "WEDGE PROBE: UNVERIFIED (need WEDGE-PROBE PASS + the scenario pass line)"
        exit 1
    fi
    ;;
wedge-gate)
    # #210 audit F2: the REGRESSION gate. Both legs must PROGRESS; any
    # WEDGED verdict lc_fails inside the probe (WARP_WEDGE_EXPECT=progress),
    # and this arm additionally requires BOTH progress lines -- so a
    # recurrence of the second-launch fence deadlock is red here, not
    # "captured". The pre-fix code fails leg 2 by construction.
    out="$REPO_ROOT/build/warp-wedge-gate.log"
    ssh "$HOST" "cd $RREPO && WARP_WEDGE_EXPECT=progress ${RENV}expect tools/warp/glq-wedge-probe.exp" | tee "$out" || true
    echo "== wedge-gate verdict =="
    if grep -q "WEDGE-PROBE PACED: PROGRESSES" "$out" \
        && grep -q "WEDGE-PROBE UNPACED: PROGRESSES" "$out" \
        && grep -q "LS-CI PASS: glq-wedge-probe:" "$out"; then
        echo "WEDGE GATE: PASS (both legs progress)"
    else
        echo "WEDGE GATE: FAIL (a leg wedged or the probe died -- the #210 class)"
        exit 1
    fi
    ;;
*)
    usage
    ;;
esac
