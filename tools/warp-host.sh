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
#   tools/warp-host.sh venus     # Warp-6 V-0 gate: is capset id=4 (VENUS) reachable? (test + control leg)
#   tools/warp-host.sh venus-verdict <ctl.log> <tst.log>  # just the verdict (no boots) -- what tools/test-venus-verdict.sh sabotages
#   tools/warp-host.sh prove     # Warp-2 gate: /warp-prove on the virgl device
#   tools/warp-host.sh composed  # Warp-C C-2b + C-2c + C-3 gate: the composed screen's arm + the witnessed imports + the composed pixels read back, GL vs 2D (both legs)
#   tools/warp-host.sh reject    # #240: is a REJECTED command stream observable in-guest?
#   tools/warp-host.sh readback  # Warp-C C-6 gate: the compositor readback arm is fenced + deferred under a deep queue (+ the F2b measurement)
#   tools/warp-host.sh p1b       # GPU-DESIGN 4.5.4: does ctx_attach permit a cross-context blit? (host-side, no guest)
#   tools/warp-host.sh p2        # GPU-DESIGN 4.5.4: does the blit observe the client's FINISHED frame? (host-side)
#   tools/warp-host.sh quake     # Warp-4 gate: GLQuake on virgl, both present arms
#   tools/warp-host.sh decomp gl|2d  # #196: unpaced per-arm figures + CPU attribution
#   tools/warp-host.sh wedge     # #210: direct-arm wedge autopsy (paced vs unpaced)
#   tools/warp-host.sh wedge-gate # #210 regression gate: BOTH legs must progress
#   tools/warp-host.sh tri       # Warp-3 gate: clear/triangle pixels through the fenced readback
#   tools/warp-host.sh quarry-bench  # in-guest renderer table; QUARRY_LEGS= sweeps resolutions (#215)
#   tools/warp-host.sh quarry-wedge  # #232: does killing a live GL client wedge the console?
#   tools/warp-host.sh native-bench  # GPU-DESIGN 13: the HW-GL exit bar's native anchor (V3D vs llvmpipe, surfaceless)
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
RENV="${WARP_ACCEL:+THYLACINE_ACCEL=$WARP_ACCEL }${WARP_BOOT_TIMEOUT:+LS_CI_BOOT_TIMEOUT=$WARP_BOOT_TIMEOUT }${GLQ_FPS_WAIT:+GLQ_FPS_WAIT=$GLQ_FPS_WAIT }${WARP_DISPLAY:+WARP_GL_DISPLAY=$WARP_DISPLAY }"
RPOLLS="${WARP_BOOT_POLLS:+WARP_BOOT_POLLS=$WARP_BOOT_POLLS }"
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
RREPO='~/projects/thylacine'

# Ranged to the verb block's TERMINATOR, not to a line number. The fixed '2,15p'
# it replaces had silently stopped listing quarry-bench, quarry-wedge and
# native-bench as they were added below it, and wedge-gate never appeared at
# all -- a usage message that omits a third of the verbs, failing in the
# direction where nobody notices. Adding a verb can no longer truncate the list.
usage() { sed -n '2,/^# Verification is fail-closed/p' "$0" | sed '$d'; exit 2; }
[[ $# -ge 1 ]] || usage
cmd="$1"

# The pool moves in independently-retryable CHUNKS, not one stream.
#
# A single 5 G stream is all-or-nothing, and over the Cloudflare tunnel it does
# not survive: three consecutive attempts died after 600-870 MB. That is a
# DETERMINISTIC sustained-transfer limit, not a flake, so a retry budget buys
# nothing -- nothing varies between attempts. Chunking is what varies: each
# 64 MiB range is its own short-lived connection, retried on its own.
#
# Two properties fall out and both matter more than the retry:
#   - the .part never becomes the live pool until it is fully verified, so a
#     failed sync leaves the previous copy (the documented bit-exact restore
#     source, CLAUDE.md "The Pi's build/") untouched;
#   - chunks whose content already matches are SKIPPED, so a re-sync of an
#     unchanged pool -- the common case under THYLACINE_MKFS_PRESERVE=1 --
#     costs a hash pass instead of a 9-minute transfer.
CHUNK_BYTES=$((64 * 1024 * 1024))

# Per-chunk MD5s of a local file, one line per chunk, in order.
local_chunk_hashes() {
    python3 - "$1" "$CHUNK_BYTES" <<'PY'
import hashlib, sys
path, chunk = sys.argv[1], int(sys.argv[2])
with open(path, 'rb') as f:
    while True:
        b = f.read(chunk)
        if not b: break
        print(hashlib.md5(b).hexdigest())
PY
}

sync_pool() {
    local lpool="$REPO_ROOT/build/fixtures/pool.img"
    # $RREPO carries a literal `~`, which only expands UNQUOTED on the remote --
    # and every path here must be quoted (it is passed through nested bash -c).
    # Resolve the remote home once so the path is absolute and quote-safe.
    local rhome rpool
    rhome=$(ssh "$HOST" 'echo $HOME')
    rpool="$rhome/projects/thylacine/build/fixtures/pool.img"
    local lsz nchunks
    lsz=$(stat -f %z "$lpool" 2>/dev/null || stat -c %s "$lpool")
    nchunks=$(( (lsz + CHUNK_BYTES - 1) / CHUNK_BYTES ))
    echo "== pool.img: $lsz bytes in $nchunks x $((CHUNK_BYTES / 1024 / 1024)) MiB chunks =="

    # Base the .part on the existing remote pool when it is the right size, so
    # only genuinely-differing chunks travel; otherwise start from a sparse file
    # of the exact size (holes read as zeros, which is what an all-zero chunk
    # needs anyway). Either way the .part is exactly lsz bytes before any chunk
    # is written, so `seek` lands where it should and the final size is exact.
    ssh "$HOST" "bash -c '
        if [ -f \"$rpool\" ] && [ \"\$(stat -c %s \"$rpool\")\" = \"$lsz\" ]; then
            cp --sparse=always \"$rpool\" \"$rpool.part\"
        else
            rm -f \"$rpool.part\"; truncate -s $lsz \"$rpool.part\"
        fi'"

    local lhashes rhashes
    lhashes=$(local_chunk_hashes "$lpool")
    rhashes=$(ssh "$HOST" "bash -c '
        for i in \$(seq 0 $((nchunks - 1))); do
            dd if=\"$rpool.part\" bs=$CHUNK_BYTES skip=\$i count=1 status=none | md5sum | cut -d\" \" -f1
        done'")

    local sent=0 skipped=0 i=0
    while [ "$i" -lt "$nchunks" ]; do
        local lh rh
        lh=$(printf '%s\n' "$lhashes" | sed -n "$((i + 1))p")
        rh=$(printf '%s\n' "$rhashes" | sed -n "$((i + 1))p")
        if [ -n "$lh" ] && [ "$lh" = "$rh" ]; then
            skipped=$((skipped + 1)); i=$((i + 1)); continue
        fi
        local attempt=1 ok=0
        while [ "$attempt" -le 3 ]; do
            # The RECEIVING dd must not carry `count=1`: reading from a PIPE it
            # short-reads, writes only that first partial read, exits, and
            # SIGPIPEs the whole upstream (observed as rc=141 on every attempt,
            # which retries cannot fix because nothing varies). No count + EOF
            # ends it exactly, since the sender ships precisely one chunk; the
            # SENDING dd reads a regular file, where bs-sized reads are whole.
            if dd if="$lpool" bs=$CHUNK_BYTES skip="$i" count=1 status=none | gzip -1 |
               ssh "$HOST" "bash -c 'set -o pipefail; gunzip -c |
                   dd of=\"$rpool.part\" bs=$CHUNK_BYTES seek=$i conv=notrunc status=none'"; then
                ok=1; break
            fi
            echo "   chunk $i attempt $attempt failed; retrying" >&2
            attempt=$((attempt + 1))
        done
        [ "$ok" = 1 ] || { echo "SYNC-FAILED: chunk $i unsendable after 3 attempts --" \
            "the .part is kept for resume, the previous pool is INTACT" >&2; exit 1; }
        sent=$((sent + 1)); i=$((i + 1))
    done
    echo "   $sent chunks sent, $skipped already current"

    # The verdict is CONTENT, not the transfer rc: hash the assembled .part and
    # require every chunk to match. A short or scrambled .part cannot pass this
    # regardless of what any individual pipeline reported.
    local rverify
    rverify=$(ssh "$HOST" "bash -c '
        for i in \$(seq 0 $((nchunks - 1))); do
            dd if=\"$rpool.part\" bs=$CHUNK_BYTES skip=\$i count=1 status=none | md5sum | cut -d\" \" -f1
        done'")
    if [ "$rverify" != "$lhashes" ]; then
        echo "SYNC-FAILED: assembled pool does not hash-match the local image --" \
             "the .part is kept for inspection, the previous pool is INTACT" >&2
        exit 1
    fi
    ssh "$HOST" "mv -f \"$rpool.part\" \"$rpool\""
    echo "== pool verified: all $nchunks chunks hash-match, renamed into place =="
}

sync_all() {
    # git archive HEAD: exactly the committed tree (no build/, no .git, no
    # local junk). Uncommitted script changes ride separately below so a
    # dirty iteration loop still tests what you edited. `tools/interactive/
    # lib.exp` is in the list because EVERY warp .exp sources it: it was
    # missing until the C-0d Fable close added a proc to it, and the Pi
    # would have run the new scenario against HEAD's lib -- an "invalid
    # command name" whose cause is a list that claimed to carry your edits.
    ssh "$HOST" "mkdir -p $RREPO/build/kernel $RREPO/build/fixtures $RREPO/share"
    git -C "$REPO_ROOT" archive HEAD | ssh "$HOST" "tar -x -C $RREPO"
    for f in tools/run-vm.sh tools/interactive/lib.exp tools/warp/boot-probe.sh tools/warp/glq-bench.exp tools/warp/warp-prove.exp tools/warp/warp-ring.exp tools/warp/warp-reject.exp tools/warp/warp-readback.exp tools/warp/virgl-prove.exp tools/warp/glq-virgl.exp tools/warp/glq-decomp.exp tools/warp/glq-wedge-probe.exp tools/warp/quarry-bench.exp tools/warp/quarry-wedge.exp tools/warp/composed-screen.exp tools/warp/native-gl-bench.c tools/warp/native-gl-bench.sh tools/interactive/gfx_strip.py; do
        scp -q "$REPO_ROOT/$f" "$HOST:$RREPO/$(dirname "$f")/"
    done
    echo "== artifacts =="
    scp -q "$REPO_ROOT"/build/kernel/thylacine.bin "$REPO_ROOT"/build/kernel/thylacine.elf \
        "$HOST:$RREPO/build/kernel/"
    scp -q "$REPO_ROOT"/build/ramfs.cpio "$REPO_ROOT"/build/disk.img "$HOST:$RREPO/build/"
    # Each chunk travels gzipped (macOS ships openrsync, no dependable
    # --sparse; a raw copy would inflate 906M-in-5G to the full 5G on the
    # wire) and lands via dd seek into a pre-sized sparse .part.
    sync_pool

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
venus)
    # Warp-6 V-0 (GPU-DESIGN section 12): is Venus REACHABLE on this host?
    #
    # Two legs differing in the DEVICE DECLARATION alone -- that difference IS
    # the control, the `composed` verb's shape. A test-only leg would pass
    # equally well against a host that advertises capset 4 unconditionally, or
    # against a guest printing a line it did not derive from the device, so the
    # control leg is not a bonus: it is what makes the test leg mean anything.
    #
    # Asserted in BOTH directions -- present WITH the declaration, ABSENT
    # without. A one-directional check ("the test leg saw id=4") is satisfied
    # by a host that always advertises it.
    #
    # venus requires blob AND hostmem together; QEMU refuses the device
    # otherwise and names the requirement itself. Declaring less does not
    # degrade to "no venus" -- it fails to REALISE, a different outcome that
    # must not be read as a negative result.
    ctl="$REPO_ROOT/build/warp-venus-control.log"
    tst="$REPO_ROOT/build/warp-venus-test.log"
    ssh "$HOST" "${RPOLLS}bash $RREPO/tools/warp/boot-probe.sh vencontrol virtio-gpu-gl-pci egl-headless" |
        tee "$ctl" || true
    ssh "$HOST" "${RPOLLS}bash $RREPO/tools/warp/boot-probe.sh venustest 'virtio-gpu-gl-pci,venus=on,blob=on,hostmem=256M' egl-headless" |
        tee "$tst" || true
    # Absolute, not $0: REPO_ROOT is already resolved, and a relative $0 would
    # bind the recursion to the caller's cwd.
    "$REPO_ROOT/tools/warp-host.sh" venus-verdict "$ctl" "$tst"
    ;;
venus-verdict)
    # The venus gate's verdict, as its OWN verb so the discrimination can be
    # sabotage-tested without booting anything. `venus` boots then calls this;
    # tools/test-venus-verdict.sh drives it against crafted logs. One
    # implementation, two callers -- a verdict that only ever runs inside an
    # 8-minute two-boot leg is a verdict nothing can afford to test.
    ctl="${2:?usage: warp-host.sh venus-verdict <control.log> <test.log>}"
    tst="${3:?usage: warp-host.sh venus-verdict <control.log> <test.log>}"
    echo "== venus verdict =="
    vfail=0
    grep -q "BOOT-vencontrol: PASS" "$ctl" || { echo "CONTROL leg did not boot -- no verdict"; vfail=1; }
    grep -q "BOOT-venustest: PASS"  "$tst" || { echo "TEST leg did not boot -- no verdict"; vfail=1; }
    # The control must have MEASURED capsets, not merely failed to see id=4.
    # Without this, a control that fell back to 2D (virgl not negotiated, no
    # capset lines at all) satisfies the negative arm below while measuring
    # nothing -- a negative assertion satisfied by a broken fixture. Requiring
    # the baseline pair makes "id=4 absent" mean "venus absent" and not
    # "capsets absent".
    for base in 1 2; do
        grep -qE "gpu capset\[[0-9]+\] id=$base " "$ctl" || {
            echo "CONTROL leg never enumerated baseline capset id=$base -- it measured nothing, so its lack of id=4 proves nothing"
            vfail=1
        }
    done
    if grep -qE "gpu capset\[[0-9]+\] id=4 " "$ctl"; then
        echo "CONTROL leg saw capset id=4 -- the declaration is NOT what produces it"
        vfail=1
    fi
    grep -qE "gpu capset\[[0-9]+\] id=4 " "$tst" || {
        echo "TEST leg saw no capset id=4 -- Venus NOT reachable on this host"
        vfail=1
    }
    # V-0b: a capset-SELECTED context must actually create. The id=2 (virgl)
    # create is the POSITIVE control and must succeed on BOTH legs -- without it,
    # "the control leg lacks id=4 CREATED" is satisfied by a leg where context
    # creation was broken outright. The id=4 create must succeed WITH venus and
    # must NOT appear WITHOUT it (a device that ignored context_init would create
    # a capset-4 context anyway -- the false pass this rung exists to catch).
    grep -qF "gpu ctx-capset id=2 CREATED" "$ctl" || {
        echo "CONTROL leg: the virgl (id=2) positive-control context did not create -- context creation is broken, so nothing about id=4 is meaningful"
        vfail=1
    }
    grep -qF "gpu ctx-capset id=2 CREATED" "$tst" || {
        echo "TEST leg: the virgl (id=2) positive-control context did not create"
        vfail=1
    }
    grep -qF "gpu ctx-capset id=4 CREATED" "$tst" || {
        echo "TEST leg: a Venus (id=4) context did NOT create -- capset advertised but the context is unreachable (render server? V-0b)"
        vfail=1
    }
    if grep -qF "gpu ctx-capset id=4 CREATED" "$ctl"; then
        echo "CONTROL leg: a Venus (id=4) context created WITHOUT venus -- the device is ignoring context_init, so the test leg's id=4 CREATED proves nothing"
        vfail=1
    fi
    # Positive pair for the negative above (main audit F3): the control leg must
    # POSITIVELY show it SKIPPED the id=4 create because the capset was not
    # enumerated -- not merely lack "id=4 CREATED", which any outcome missing the
    # string satisfies. Anchors the verdict on what only the intended path emits.
    grep -qF "gpu ctx-capset id=4 skipped (capset not enumerated)" "$ctl" || {
        echo "CONTROL leg: no 'id=4 skipped (capset not enumerated)' -- the id=4 create did not take the intended no-venus path; its absence of CREATED proves nothing"
        vfail=1
    }
    # V-1: a GUEST blob must actually create where the feature is negotiated.
    # The venus device offers F_RESOURCE_BLOB, the plain -gl control does not,
    # so blob_probe self-skips there -- the driver gate is keyed on the
    # feature bit, not on venus. Test leg must CREATE; control must POSITIVELY
    # show the skip (not merely lack CREATED -- the same F3 lesson: an absent
    # line is satisfied by a leg where the probe never ran); and control must
    # NOT create (a create there would mean the driver put a blob command on a
    # wire that never negotiated the feature, so the test leg's CREATED would
    # prove nothing about the gate).
    grep -qF "gpu blob-create guest CREATED" "$tst" || {
        echo "TEST leg: a guest blob did NOT create -- F_RESOURCE_BLOB negotiated but RESOURCE_CREATE_BLOB refused (V-1)"
        vfail=1
    }
    if grep -qF "gpu blob-create guest CREATED" "$ctl"; then
        echo "CONTROL leg: a guest blob created WITHOUT F_RESOURCE_BLOB -- the driver sent a blob command it never negotiated, so the test leg's CREATED proves nothing"
        vfail=1
    fi
    grep -qF "gpu blob-create skipped (F_RESOURCE_BLOB not offered)" "$ctl" || {
        echo "CONTROL leg: no 'blob-create skipped (F_RESOURCE_BLOB not offered)' -- the probe did not take the intended no-feature path; its absence of CREATED proves nothing"
        vfail=1
    }
    # V-3b-1a: the HOST3D ring substrate. A HOST3D blob_id=0 mappable blob is
    # the vkr (venus renderer) shm path, reachable ONLY via a capset-4 context;
    # the test leg MAPs it under a venus ctx, and a device-global create is
    # refused -- the negative control that proves the venus-ctx requirement.
    grep -qF "gpu host3d-map venus-ctx MAPPED" "$tst" || {
        echo "TEST leg: HOST3D venus-ctx blob did NOT MAP_BLOB -- the Model B ring substrate is unreachable (V-3b-1a)"
        vfail=1
    }
    grep -qF "gpu host3d-map global create refused" "$tst" || {
        echo "TEST leg: a device-global HOST3D create did NOT refuse -- the negative control proving the venus-ctx requirement is missing; venus-ctx MAP alone could be incidental"
        vfail=1
    }
    if grep -qF "gpu host3d-map venus-ctx MAPPED" "$ctl"; then
        echo "CONTROL leg: a HOST3D blob MAPPED without F_RESOURCE_BLOB/hostmem -- impossible, the gate is wrong"
        vfail=1
    fi
    grep -qF "gpu host3d-map skipped (F_RESOURCE_BLOB not offered)" "$ctl" || {
        echo "CONTROL leg: no 'host3d-map skipped (F_RESOURCE_BLOB not offered)' -- the probe did not take the intended no-feature skip; its absence of MAPPED proves nothing"
        vfail=1
    }
    # V-3b-1c: the persistent hostmem RING ENGINE. The test leg mints TWO HOST3D
    # rings (the allocator must hand distinct offsets), round-trips a sentinel
    # through each guest VA, tears both down, and re-mints to prove the offset
    # free-list reclaims; the control leg (no F_RESOURCE_BLOB) self-skips.
    grep -qF "gpu hostmem-ring MAPPED+ROUNDTRIP x2" "$tst" || {
        echo "TEST leg: the HOST3D ring engine did NOT map+round-trip two rings and reuse a freed offset -- the V-3b-1c mint/teardown lifecycle (SYS_BURROW_FROM_HOSTMEM + the free-list) is broken"
        vfail=1
    }
    if grep -qF "gpu hostmem-ring MAPPED+ROUNDTRIP x2" "$ctl"; then
        echo "CONTROL leg: a hostmem ring round-tripped WITHOUT F_RESOURCE_BLOB/hostmem -- impossible, the gate is wrong"
        vfail=1
    fi
    grep -qF "gpu hostmem-ring skipped (F_RESOURCE_BLOB not offered)" "$ctl" || {
        echo "CONTROL leg: no 'hostmem-ring skipped (F_RESOURCE_BLOB not offered)' -- the probe did not take the intended no-feature skip"
        vfail=1
    }
    # V-3b-1c-2a: the SERVER host3d-ring path (a per-client venus device-ctx +
    # the HOST3D ring flavor in the /srv/warp ring subtree + wring_teardown's
    # host3d arm). The test leg mints a HOST3D ring under a real warp ctx via the
    # persistent engine, round-trips a sentinel at the mapped ring VA, and tears
    # the ctx down (driving drop_host3d_ring + the venus-ctx destroy); the
    # control leg (no F_RESOURCE_BLOB) self-skips. The "venus-ctx=" line is
    # emitted ONLY on a successful round-trip -- its presence IS the proof (a
    # FAIL or a skip emits a different line), so this is the tapestryd-side,
    # no-client witness of the 1c-2a wiring (the client claim is 1c-2b's warp-prove).
    grep -qF "warp host3d-ring venus-ctx=" "$tst" || {
        echo "TEST leg: the SERVER HOST3D ring path did NOT create a venus ctx + map+round-trip a ring -- the V-3b-1c-2a venus-ctx / ring-flavor / teardown wiring is broken"
        vfail=1
    }
    if grep -qF "warp host3d-ring venus-ctx=" "$ctl"; then
        echo "CONTROL leg: a server host3d ring round-tripped under a venus ctx WITHOUT venus/blob -- impossible, the gate is wrong"
        vfail=1
    fi
    grep -qF "warp host3d-ring skipped" "$ctl" || {
        echo "CONTROL leg: no 'warp host3d-ring skipped' -- the server self-test did not take the intended no-feature skip"
        vfail=1
    }
    if [ "$vfail" -eq 0 ]; then
        echo "VENUS GATE: VERIFIED -- capset id=4 present WITH venus=on absent WITHOUT, a Venus context creates (id=4 CREATED with venus, skipped without; id=2 control creates on both), a guest blob creates WITH F_RESOURCE_BLOB and is skipped WITHOUT (V-1), a HOST3D blob_id=0 mappable blob MAPs under a venus ctx while a device-global create is refused (V-3b-1a), a HOST3D blob guest-maps via SYS_BURROW_FROM_HOSTMEM and round-trips a sentinel (V-3b-1b), AND the persistent ring engine mints two rings at distinct offsets, round-trips each guest VA, and reuses a freed offset on re-mint (V-3b-1c), AND the SERVER host3d-ring path creates a per-client venus device-ctx, mints a HOST3D ring in /srv/warp, round-trips its VA, and tears it down through drop_host3d_ring + the venus-ctx destroy (V-3b-1c-2a)"
        grep -hE "gpu capset\[|num_capsets|blob-create" "$ctl" "$tst"
    else
        echo "VENUS GATE: UNVERIFIED"
        exit 1
    fi
    ;;
composed)
    # Warp-C C-2b: does the compositor's SCREEN follow the host's GL
    # capability? Two legs on ONE host differing in the DEVICE alone --
    # that difference IS the control. A GL-only leg would pass equally well
    # against a build that ignored the capability and always minted 3D, so
    # the 2D leg is not a bonus; it is what makes the GL leg mean anything.
    out="$REPO_ROOT/build/warp-composed.log"
    : > "$out"
    for d in virtio-gpu-gl-pci virtio-gpu-pci; do
        echo "== composed leg: $d =="
        ssh "$HOST" "cd $RREPO && ${RENV}THYLACINE_GPU_DEV=$d expect tools/warp/composed-screen.exp" |
            tee -a "$out" || true
    done
    echo "== composed verdict =="
    grep -E "WARP-COMPOSED" "$out" || true
    ok=1
    # Four terms. The SCREEN lines carry the claim; the scenario PASS lines
    # prove each leg RAN TO COMPLETION -- without them a leg that died right
    # after printing its screen line still shows the evidence the gate greps
    # for. The 3D pattern cannot be satisfied by the 2D leg (its arm reads
    # "2D"), and "composed-screen: virtio-gpu-pci" cannot be satisfied by the
    # GL leg (whose device string carries the extra "gl-").
    if ! grep -qE "WARP-COMPOSED SCREEN: res [0-9]+ 3D \(compositor ctx\)" "$out"; then
        echo "C-2b GATE FAIL -- no 3D screen mint on the GL device"
        ok=0
    fi
    if ! grep -qE "WARP-COMPOSED SCREEN: res [0-9]+ 2D \(" "$out"; then
        echo "C-2b GATE FAIL -- no 2D screen mint on the non-GL device"
        ok=0
    fi
    if ! grep -qF "LS-CI PASS: composed-screen: virtio-gpu-gl-pci" "$out"; then
        echo "C-2b GATE FAIL -- the GL leg did not run to completion"
        ok=0
    fi
    if ! grep -qF "LS-CI PASS: composed-screen: virtio-gpu-pci" "$out"; then
        echo "C-2b GATE FAIL -- the non-GL leg did not run to completion"
        ok=0
    fi
    # Fifth term (2026-08-17): BOTH legs bound the display to the screen they
    # minted. SET_SCANOUT is the one virtio-gpu command whose OK consults the
    # renderer, so this is the host-side witness the mint responses are not
    # (QEMU answers OK to CREATE_3D / CTX_ATTACH / ATTACH_BACKING regardless).
    if [ "$(grep -cE 'WARP-COMPOSED BOUND: res [0-9]+' "$out")" != 2 ]; then
        echo "C-2b GATE FAIL -- expected the screen BOUND on both legs"
        ok=0
    fi
    # Sixth + seventh terms (Warp-C C-2c, GPU-DESIGN 4.5.10): the GL leg
    # witnessed the compositor's import of >= 2 surface generations by a
    # slot->sentinel pixel copy inside the compositor ctx (the attach's OK
    # response attests nothing, 4.5.4c); the non-GL leg declared the import
    # skipped and printed no per-surface line -- the control, again.
    if ! grep -qE 'WARP-COMPOSED ATTACH: witnessed [2-9][0-9]* surfaces' "$out"; then
        echo "C-2c GATE FAIL -- the GL leg did not witness >= 2 surface imports"
        ok=0
    fi
    if ! grep -qF 'WARP-COMPOSED ATTACH: skipped (no compositor ctx)' "$out"; then
        echo "C-2c GATE FAIL -- the non-GL leg did not declare the import skipped"
        ok=0
    fi
    # Eighth + ninth terms (Warp-C C-3, GPU-DESIGN 4.5.11): the composed
    # PIXELS. Nine probes per leg read back exact -- through the 3D screen
    # RESOURCE on the GL leg (`via readback`, and >= 1 present GPU-composed
    # by the census), through the screen buffer on the non-GL leg (`via
    # backing`, none GPU-composed). Same coordinates, same colors: the two
    # composition paths agree from outside, measured.
    if ! grep -qE 'WARP-COMPOSED PIXELS: 9 probes via readback ok \(composed gpu [1-9][0-9]* cpu [0-9]+\)' "$out"; then
        echo "C-3 GATE FAIL -- the GL leg's 9 pixel probes did not read back exact via the 3D screen with >= 1 GPU-composed present"
        ok=0
    fi
    if ! grep -qE 'WARP-COMPOSED PIXELS: 9 probes via backing ok \(composed gpu 0 cpu [0-9]+\)' "$out"; then
        echo "C-3 GATE FAIL -- the non-GL leg's 9 pixel probes did not read exact via the buffer with 0 GPU-composed presents"
        ok=0
    fi
    if [ "$ok" != 1 ]; then
        exit 1
    fi
    echo "C-2b/C-2c/C-3 COMPOSED-SCREEN GATE: VERIFIED (3D screen + witnessed imports + 9/9 exact readback pixels on GL, 2D + no import + 9/9 exact buffer pixels without -- discriminates)"
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
ring)
    out="$REPO_ROOT/build/warp-ring.log"
    ssh "$HOST" "cd $RREPO && ${RENV}expect tools/warp/warp-ring.exp" | tee "$out" || true
    echo "== ring verdict =="
    # The Warp-6 V-3a gate: the prover's OWN pass line (the ring round-trip +
    # F2 + I-45 + the I-9 re-scan discrimination are asserted in-guest) AND the
    # scenario pass (the boot + login around it held). Either alone is not it.
    if grep -q "WARP-RING PASS" "$out" && grep -q "PASS: warp-ring" "$out"; then
        echo "WARP-6 V-3a GATE: VERIFIED"
    else
        echo "WARP-6 V-3a GATE: UNVERIFIED (need WARP-RING PASS + the scenario pass line)"
        exit 1
    fi
    ;;
reject)
    out="$REPO_ROOT/build/warp-reject.log"
    ssh "$HOST" "cd $RREPO && ${RENV}expect tools/warp/warp-reject.exp" | tee "$out" || true
    echo "== #240 observation + the C-0d detector gate =="
    # AUDIT F8: this WAS report-only, and stayed that way after C-0d turned
    # it into a gate -- the conjunction below grepped `C0-REJECT` while the
    # detector's lines are prefixed `C0-DETECT`, so the command exited 0 on
    # `C0-DETECT FAIL(vacuous)`: exactly the outcome GPU-DESIGN 4.5.4b names
    # as the thing this gate exists to catch. It caught it once only because
    # a human was reading the output.
    #
    # SIX terms, each covering a different way to be wrong. Keep this
    # enumeration equal to the loop below: it read "Four terms" and listed
    # five for as long as the loop checked five, which is what a maintainer
    # asking "does the gate cover arm X?" actually reads (follow-up round F5).
    #   ANSWER=       the #240 measurement ran at all
    #   DETECT PASS   the detector DISCRIMINATES (rejected 1, healthy 0)
    #   STICKY PASS   a SECOND REAL probe still reads (1 0)
    #   F1 DEFENDED   a client that WRITES the probe's mark cannot blind it
    #   STAGING PASS  the create3d door ADMITS the Mesa staging/MSAA shape
    #                 (one page declared for real geometry) while still
    #                 refusing a malformed one -- the follow-up round's F1,
    #                 where the C-6b close's lower bound broke real clients
    #   LS-CI PASS    the SCENARIO completed -- without a completion term an
    #                 aborted run (ctx_field's fail() path, F9) still shows
    #                 ANSWER= and would read as verified.
    #
    # ROUND-2 F3: that last term was `C0-REJECT DONE`, which is exactly the
    # token warp-reject.exp's own header says never to key a pass on --
    # lc_expect writes its own pattern into its timeout text ("waiting for
    # [C0-REJECT DONE]"), so a guest that hung AFTER `C0-F1 DEFENDED` would
    # have had the fifth term supplied by the failure message itself, over a
    # run that never finished. I wrote that warning and then walked into it.
    # The defence I argued -- "a timeout also costs us the other terms" --
    # assumed the hang can only happen before they print, which is a guess
    # about hang location, not a property of the gate. `lc_pass`'s prefix
    # can only be produced by the success path, and it is what `prove`,
    # `tri` and `quake` already require.
    #
    # C-0d FABLE ROUND F6: the scenario is SELF-GATING now -- warp-prove
    # prints `C0-REJECT DONE` only when every C0 arm passed and
    # `C0-REJECT INCOMPLETE(<arm>)` otherwise, which warp-reject.exp
    # hard-fails on -- so a blind detector no longer reaches lc_pass at all.
    # The six terms stay as the belt to that brace: they name each arm, and
    # a scenario that passed for a reason this list does not know about
    # should still fail here.
    grep -E "C0-REJECT|C0-DETECT|C0-F1|C0-STAGING" "$out" || true
    ok=1
    for pat in "C0-REJECT ANSWER=" "C0-DETECT PASS" "C0-DETECT STICKY PASS" "C0-F1 DEFENDED" "C0-STAGING PASS" "LS-CI PASS: warp-prove reject"; do
        if ! grep -q "$pat" "$out"; then
            echo "#240 GATE FAIL -- missing: $pat"
            ok=0
        fi
    done
    if [ "$ok" != 1 ]; then
        exit 1
    fi
    echo "C-0d DETECTOR GATE: VERIFIED (discriminates + sticky + ran to completion)"
    ;;
readback)
    out="$REPO_ROOT/build/warp-readback.log"
    ssh "$HOST" "cd $RREPO && ${RENV}expect tools/warp/warp-readback.exp" | tee "$out" || true
    echo "== Warp-C C-6: the compositor readback arm =="
    # The scenario is self-gating (`C6-READBACK DONE` prints only when every
    # verdict arm passed; INCOMPLETE(<arm>) hard-fails it), and the terms
    # below are the belt: each names an arm, and the LS-CI line requires
    # the SCENARIO to have completed (lc_pass's prefix, which only the
    # success path prints -- never the DONE token, which the timeout text
    # quotes; #186). F2B is a MEASUREMENT: printed, not required.
    grep -E "C6-RB|C6-READBACK" "$out" || true
    ok=1
    for pat in "C6-RB ARM PASS" "C6-RB GUARD PASS" "C6-RB DEEP PASS" "C6-RB LIVE PASS" "C6-RB DEADLINE PASS" "LS-CI PASS: warp-prove readback"; do
        if ! grep -q "$pat" "$out"; then
            echo "C-6 GATE FAIL -- missing: $pat"
            ok=0
        fi
    done
    if [ "$ok" != 1 ]; then
        exit 1
    fi
    echo "WARP-C C-6 GATE: VERIFIED (readback arm taken + deep queue paid by the device + dispatch answered inside budget + busy read as busy)"
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
    # WARP_DISPLAY=dbus-gl (Warp-C C-4) runs the gl legs on the no-readback
    # display lane; the log name carries the lane so two lanes' figures never
    # overwrite each other.
    out="$REPO_ROOT/build/warp-decomp-$sub${WARP_DISPLAY:+-$WARP_DISPLAY}.log"
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
quarry-bench)
    # The in-guest renderer comparison, and -- with QUARRY_LEGS -- the
    # same-lane RESOLUTION SWEEP for #215.
    #
    # Same-lane is the whole point. The standing figures pair quarry at the
    # engine default against the wedge-probe lane at 1280x800, and those two
    # differ in more than resolution (the rp7 pacer wrapper, -noglcompression),
    # so the apparent resolution-invariance of hw-gl is a LEAD, not a datum.
    # One lane, one boot, several modes settles it.
    #
    #   QUARRY_LEGS="sw@320x240 sw@1280x800 hw-gl@320x240 hw-gl@1280x800" \
    #     WARP_HOST=thyla-pi WARP_ACCEL=kvm tools/warp-host.sh quarry-bench
    #
    # The sw legs are the POSITIVE CONTROL and are not optional: a software
    # rasterizer is fill-bound, so its fps must fall with pixel count. Without
    # them a flat hw-gl curve cannot be told apart from a -width that never
    # took effect -- both look identical. quarry's own per-leg mode-witness
    # (`+vid_describecurrentmode`) is the second, independent check.
    out="$REPO_ROOT/build/quarry-bench.log"
    ssh "$HOST" "cd $RREPO && ${QUARRY_LEGS:+QUARRY_LEGS='$QUARRY_LEGS' }${RENV}expect tools/warp/quarry-bench.exp" | tee "$out" || true
    echo "== mode witnesses (the resolution each leg ACTUALLY ran at) =="
    grep -E "mode-witness" "$out" || echo "(none -- resolutions unverified)"
    echo "== the bench table =="
    sed -n '/^renderer  */,/^ *$/p' "$out" || true
    if grep -q "MISMATCH\|mode-witness ABSENT" "$out"; then
        echo "WARNING: at least one leg did not run at the resolution it was given."
        echo "         Treat the fps column as unattributed until that is explained."
    fi
    ;;
quarry-wedge)
    # #232: does killing a live GL client wedge the console? The scenario is
    # a DISCRIMINATOR, so its job is to answer, not to pass -- a no-wedge run
    # is as informative as a wedge. The gate is therefore "a verdict was
    # reached", and every verdict line is echoed here.
    out="$REPO_ROOT/build/quarry-wedge.log"
    ssh "$HOST" "cd $RREPO && ${RENV}expect tools/warp/quarry-wedge.exp" | tee "$out" || true
    echo "== #232 verdict =="
    grep -E "^\s*(\[step\])?\s*#232 " "$out" || true
    if grep -q "#232 VERDICT" "$out"; then
        echo "#232 DISCRIMINATOR: ANSWERED (see the verdict lines above)"
    else
        echo "#232 DISCRIMINATOR: NO VERDICT (the run died before deciding -- read $out)"
        exit 1
    fi
    ;;
native-bench)
    # GPU-DESIGN 13: the HW-GL exit bar's NATIVE anchor. Runs on the GL host
    # itself (native V3D vs native llvmpipe, surfaceless) -- no guest, no
    # accel plumbing. The ratio it prints is the reference the guest quake/
    # decomp ratio is held against.
    out="$REPO_ROOT/build/warp-native-bench.log"
    ssh "$HOST" "bash $RREPO/tools/warp/native-gl-bench.sh" | tee "$out"
    echo "== native-bench verdict =="
    if grep -q "NATIVE-BENCH RATIO:" "$out"; then
        grep -e "^NGB " -e "NATIVE-BENCH RATIO:" "$out"
        echo "NATIVE-BENCH: MEASURED"
    else
        echo "NATIVE-BENCH: UNVERIFIED (throttled, unparsed, or build failed)"
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
p2)
    # GPU-DESIGN 4.5.4: does a blit on the compositor's ctx observe the client
    # ctx's FINISHED frame with nothing ordering them? Host-side, no guest.
    d='~/warp/p1b'
    ssh "$HOST" "mkdir -p $d && cd $d && for f in virglrenderer.h virgl_protocol.h virgl_hw.h; do
        [ -s \$f ] || curl -sSfL -o \$f https://gitlab.freedesktop.org/virgl/virglrenderer/-/raw/main/src/\$f || exit 1
      done
      [ -s virgl-version.h ] || printf '%s\n' '#define VIRGL_VERSION_MAJOR 1' '#define VIRGL_VERSION_MINOR 9' '#define VIRGL_VERSION_MICRO 0' '#define VIRGL_VERSION_STRING \"1.9.0\"' > virgl-version.h" || {
        echo "P2: header fetch failed"; exit 1; }
    scp -q "$REPO_ROOT/tools/warp/p2-cross-ctx-order.c" "$HOST:$d/" || exit 1

    out="$REPO_ROOT/build/warp-p2.log"
    ssh "$HOST" "cd $d && gcc -O1 -g -Wall -Wextra -I. -o p2 p2-cross-ctx-order.c -l:libvirglrenderer.so.1 || exit 1
      for dep in ${P2_DEPTHS:-24 64 256}; do
        echo \"===== depth=\$dep =====\"
        P2_DEPTH=\$dep P2_TRIALS='${P2_TRIALS:-150}' timeout 900 ./p2 2>&1
      done" | tee "$out" || true

    echo "== P2 verdict =="
    # THE SENSITIVITY TERM IS THE ONE THAT MATTERS. A clean UNSYNCED result is
    # equally consistent with "ordering holds" and "this probe cannot see a
    # stale read" -- so the INVERTED arm, which is stale BY CONSTRUCTION, must
    # have mismatched on every trial before a clean measurement means anything.
    # Requiring only the verdict line would accept a blind probe's silence.
    runs=$(grep -c "^===== depth=" "$out" || true)
    sens=$(grep -c "the probe CAN see a stale read" "$out" || true)
    verd=$(grep -c "REORDERING OBSERVED" "$out" || true)
    reord=$(grep -c "^  REORDERING OBSERVED" "$out" || true)
    instr=$(grep -c "INSTRUMENT FAILURE" "$out" || true)
    if [ "$runs" -ge 1 ] && [ "$sens" = "$runs" ] && [ "$verd" = "$runs" ] && [ "$instr" = 0 ] && [ "$reord" = 0 ]; then
        grep -E "UNSYNCED|NO REORDERING" "$out"
        echo "P2 GATE: PASS -- sensitivity proven on every run, no reordering observed"
    elif [ "$reord" -gt 0 ]; then
        echo "P2 GATE: HAZARD OBSERVED -- the blit did NOT see the finished frame."
        echo "  This is a REAL finding, not a broken run: C-1 must model it before C-4"
        echo "  removes the readback. See $out"
        exit 1
    else
        echo "P2 GATE: NO VERDICT (runs=$runs sensitivity=$sens verdicts=$verd instrument-failures=$instr)"
        echo "  a clean UNSYNCED arm without the sensitivity term is not a result"
        exit 1
    fi
    ;;
p1b)
    # GPU-DESIGN 4.5.4: does an explicit ctx_attach_resource permit the
    # cross-context blit that P1a proved is refused WITHOUT one? Runs entirely
    # host-side, against virglrenderer directly -- no guest, no accel plumbing
    # -- because the in-guest path is circular: P1b gates C-2, and the verb it
    # needs is what C-2 would build.
    #
    # Headers are FETCHED (1.9.0, matching the runtime) rather than taken from
    # Debian's libvirglrenderer-dev, which is 1.1.0. A header from one ABI over
    # a runtime from another is the setup that yields a confident wrong answer.
    d='~/warp/p1b'
    ssh "$HOST" "mkdir -p $d && cd $d && for f in virglrenderer.h virgl_protocol.h virgl_hw.h; do
        [ -s \$f ] || curl -sSfL -o \$f https://gitlab.freedesktop.org/virgl/virglrenderer/-/raw/main/src/\$f || exit 1
      done
      [ -s virgl-version.h ] || printf '%s\n' '#define VIRGL_VERSION_MAJOR 1' '#define VIRGL_VERSION_MINOR 9' '#define VIRGL_VERSION_MICRO 0' '#define VIRGL_VERSION_STRING \"1.9.0\"' > virgl-version.h" || {
        echo "P1B: header fetch failed"; exit 1; }
    scp -q "$REPO_ROOT/tools/warp/p1b-cross-ctx-blit.c" "$HOST:$d/" || exit 1

    out="$REPO_ROOT/build/warp-p1b.log"
    ssh "$HOST" "cd $d && gcc -O1 -g -Wall -Wextra -I. -o p1b p1b-cross-ctx-blit.c -l:libvirglrenderer.so.1 || exit 1
      echo '===== ARM 1: WITH attach ====='
      timeout 120 ./p1b 2>&1
      echo '===== ARM 2: WITHOUT attach (control) ====='
      P1B_NO_ATTACH=1 timeout 120 ./p1b 2>&1" | tee "$out" || true

    echo "== P1b verdict =="
    # FOUR terms, because each single one is satisfiable by a different broken
    # instrument -- the failure the `reject` verb above documents (a gate that
    # grepped the wrong prefix and exited 0 on its own FAIL) is the standing
    # warning here:
    #   WORKS          the attached arm actually moved pixels
    #   CONTROL        the UNATTACHED arm was refused. Without this the whole
    #                  result is vacuous: a renderer that isolates nothing
    #                  passes the first term identically, and the two readings
    #                  are opposite for I-45.
    #   CONFIRMED x2   a SAME-context blit moved pixels in BOTH arms, so a
    #                  mis-encoded blit cannot masquerade as a refusal
    #   no failures    no CHECK() tripped in either arm
    ok_works=$(grep -c "WORKS: an attached cross-context blit" "$out" || true)
    ok_ctl=$(grep -c "CONTROL AS EXPECTED" "$out" || true)
    ok_enc=$(grep -c "blit encoding CONFIRMED working" "$out" || true)
    bad=$(grep -c "checks failed: [1-9]" "$out" || true)
    if [ "$ok_works" -ge 1 ] && [ "$ok_ctl" -ge 1 ] && [ "$ok_enc" -ge 2 ] && [ "$bad" = 0 ]; then
        echo "P1b GATE: PASS -- attach permits the blit, absence of attach refuses it"
    else
        echo "P1b GATE: UNVERIFIED (works=$ok_works control=$ok_ctl encoding=$ok_enc failed=$bad)"
        echo "  a missing CONTROL term is not a weaker pass -- it is NO result"
        exit 1
    fi
    ;;
*)
    usage
    ;;
esac
