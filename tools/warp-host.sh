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
#   tools/warp-host.sh reject    # #240: is a REJECTED command stream observable in-guest?
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
RENV="${WARP_ACCEL:+THYLACINE_ACCEL=$WARP_ACCEL }${WARP_BOOT_TIMEOUT:+LS_CI_BOOT_TIMEOUT=$WARP_BOOT_TIMEOUT }${GLQ_FPS_WAIT:+GLQ_FPS_WAIT=$GLQ_FPS_WAIT }"
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
    # dirty iteration loop still tests what you edited.
    ssh "$HOST" "mkdir -p $RREPO/build/kernel $RREPO/build/fixtures $RREPO/share"
    git -C "$REPO_ROOT" archive HEAD | ssh "$HOST" "tar -x -C $RREPO"
    for f in tools/run-vm.sh tools/warp/boot-probe.sh tools/warp/glq-bench.exp tools/warp/warp-prove.exp tools/warp/warp-reject.exp tools/warp/virgl-prove.exp tools/warp/glq-virgl.exp tools/warp/glq-decomp.exp tools/warp/glq-wedge-probe.exp tools/warp/quarry-bench.exp tools/warp/quarry-wedge.exp tools/warp/native-gl-bench.c tools/warp/native-gl-bench.sh tools/interactive/gfx_strip.py; do
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
    # Four terms, each covering a different way to be wrong:
    #   ANSWER=       the #240 measurement ran at all
    #   DETECT PASS   the detector DISCRIMINATES (rejected 1, healthy 0)
    #   STICKY PASS   a SECOND REAL probe still reads (1 0)
    #   F1 DEFENDED   a client that WRITES the probe's mark cannot blind it
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
    grep -E "C0-REJECT|C0-DETECT|C0-F1" "$out" || true
    ok=1
    for pat in "C0-REJECT ANSWER=" "C0-DETECT PASS" "C0-DETECT STICKY PASS" "C0-F1 DEFENDED" "LS-CI PASS: warp-prove reject"; do
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
