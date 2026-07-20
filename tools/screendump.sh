#!/usr/bin/env bash
# tools/screendump.sh -- the "agentic eyes" capture step (G-0; TAPESTRY.md
# section 18.9). Captures the running dev VM's virtio-gpu scanout to a PNG
# over the QMP control socket tools/run-vm.sh already opens by default
# (build/qmp.sock; the P4-K-events wiring).
#
# Headless-safe by design: QEMU maintains the QemuConsole surface for a
# bound scanout regardless of display backend, so `screendump` works under
# the standing -nographic invocation (no -display sdl/gtk needed) -- the
# empirical proof of that property is this harness's -v gate.
#
#   tools/screendump.sh out.png                    # capture gpu0 head 0
#   tools/screendump.sh -v out.png                 # + verify the P4-L pattern
#   tools/screendump.sh -c out.png                 # + verify the Aurora console
#   tools/screendump.sh -s /path/qmp.sock out.png  # explicit socket
#
# -v additionally issues a PPM-format sibling dump and asserts the P4-L
# 4-quadrant test pattern (TL red / TR green / BL blue / BR white) by
# sampling the four quadrant centers -- the G-0 deliverable check that the
# capture path sees real guest-driven pixels, not a blank surface. (Kept as
# a dev tool for tapestry-demo runs; no default boot paints it since G-4.)
#
# -c (G-4) verifies the AURORA CONSOLE signature statistically + exactly:
# the Bonfire background (#0e0c0c, UTOPIA-VISUAL.md section 1.1) must
# DOMINATE (>= 40% of pixels -- glyph coverage never approaches that), and
# rendered TEXT must exist (>= 200 pixels of exact default-fg #e4ddd8 --
# anti-aliased glyphs have solid full-coverage cores, so real text always
# lands exact-fg pixels; the login banner alone is thousands). Content-
# independent (any session text passes; a blank/black/garbage/demo screen
# fails), deterministic (exact colors, statistical coverage).
#
# -c additionally runs a BLEND-INTEGRITY pass (G-5; the #35 class): the
# bg/fg counts above are structurally blind to ANTIALIASED EDGE pixels --
# exactly where the #35 packed-lane blend bug lived (glyph cores stayed
# exact via the a=255 short-circuit while edge channels scattered, so the
# G-4 check passed a violet-fringed screen). Every pixel 8-adjacent to an
# exact-fg core that is neither exact bg nor exact fg is overwhelmingly
# default-fg antialiasing, and a CORRECT blend is a per-channel convex
# combination -- each channel must lie inside the [bg,fg] envelope (+/-
# tolerance). The #35 formula scatters ~15% of default-fg edges outside
# the envelope (~72% for saturated text colors); legitimate cross-color
# glyph junctions measure ~2% -- the 5% threshold splits both with ~3x
# margin either way. Skipped below 100 checked edges (no AA mass).
#
# -F PPM verifies an existing P6 PPM offline (no QMP, no VM) -- the probe
# path tools/test-screendump-edge.sh uses to keep the blend-integrity
# pass non-vacuous (a synthesized #35-buggy frame must FAIL).
#
# Exit: 0 on success (and verify pass when -v/-c); nonzero otherwise.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

sock="$REPO_ROOT/build/qmp.sock"
device="gpu0"
head=0
verify=0
offline=""

usage() {
    cat >&2 <<EOF
usage: tools/screendump.sh [-s QMP_SOCK] [-d DEVICE_ID] [-H HEAD] [-v|-c] OUT.png
       tools/screendump.sh -F FILE.ppm (-v|-c)

  -s QMP_SOCK   QMP unix socket (default: build/qmp.sock -- what
                tools/run-vm.sh opens unless THYLACINE_NO_QMP=1)
  -d DEVICE_ID  display device qdev id (default: gpu0, matching
                run-vm.sh's -device virtio-gpu-device,id=gpu0)
  -H HEAD       scanout head (default: 0)
  -v            verify the P4-L 4-quadrant test pattern via a PPM
                sibling dump (TL red, TR green, BL blue, BR white)
  -c            verify the Aurora console signature (Bonfire bg dominant
                + exact default-fg text + blend-integrity edges)
  -F FILE.ppm   offline mode: run the -v/-c verification against an
                existing P6 PPM instead of a live VM (no QMP; no
                OUT.png positional; the file is not deleted)
EOF
    exit 2
}

while getopts "s:d:H:vcF:h" opt; do
    case "$opt" in
        s) sock="$OPTARG" ;;
        d) device="$OPTARG" ;;
        H) head="$OPTARG" ;;
        v) verify=1 ;;
        c) verify=2 ;;
        F) offline="$OPTARG" ;;
        *) usage ;;
    esac
done
shift $((OPTIND - 1))

if [[ -n "$offline" ]]; then
    [[ $# -eq 0 ]] || usage
    if [[ "$verify" -eq 0 ]]; then
        echo "screendump: -F requires -v or -c" >&2
        exit 2
    fi
    if [[ ! -f "$offline" ]]; then
        echo "screendump: $offline: no such file" >&2
        exit 1
    fi
    case "$offline" in
        /*) ;;
        *) offline="$(pwd)/$offline" ;;
    esac
    out=""
else
    [[ $# -eq 1 ]] || usage
    out="$1"

    # QEMU writes the dump relative to ITS cwd -- absolutize against ours.
    case "$out" in
        /*) ;;
        *) out="$(pwd)/$out" ;;
    esac

    if [[ ! -S "$sock" ]]; then
        echo "screendump: no QMP socket at $sock (VM not running, or THYLACINE_NO_QMP=1)" >&2
        exit 1
    fi
fi

exec python3 - "$sock" "$device" "$head" "$out" "$verify" "$offline" <<'PYEOF'
import json, os, socket, sys

sock_path, device, head, out = sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4]
verify = int(sys.argv[5])   # 0 = none, 1 = -v quadrants, 2 = -c console
offline = sys.argv[6]       # non-empty = verify this PPM, no QMP/VM


class Qmp:
    """Minimal QMP client: greeting -> qmp_capabilities -> commands.

    Server messages arrive newline-delimited; async events may interleave
    with command responses and are skipped (this harness registers no
    event interest)."""

    def __init__(self, path):
        self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.s.settimeout(15)
        self.s.connect(path)
        self.buf = b""
        greet = self.recv_msg()
        if "QMP" not in greet:
            raise RuntimeError(f"unexpected QMP greeting: {greet}")
        self.cmd("qmp_capabilities")

    def recv_msg(self):
        while True:
            nl = self.buf.find(b"\n")
            if nl >= 0:
                line = self.buf[:nl].strip()
                self.buf = self.buf[nl + 1:]
                if not line:
                    continue
                return json.loads(line)
            chunk = self.s.recv(65536)
            if not chunk:
                raise RuntimeError("QMP socket EOF")
            self.buf += chunk

    def cmd(self, name, **args):
        msg = {"execute": name}
        if args:
            msg["arguments"] = args
        self.s.sendall((json.dumps(msg) + "\n").encode())
        while True:
            resp = self.recv_msg()
            if "event" in resp:
                continue
            if "error" in resp:
                raise RuntimeError(
                    f"{name}: {resp['error'].get('desc', resp['error'])}")
            if "return" in resp:
                return resp["return"]


def parse_ppm(path):
    data = open(path, "rb").read()
    if not data.startswith(b"P6"):
        raise RuntimeError(f"{path}: not a P6 PPM")
    # Header tokens after the magic: width, height, maxval. '#' comments
    # are legal PPM; QEMU emits none but tolerate them.
    toks, i, n = [], 2, len(data)
    while len(toks) < 3 and i < n:
        c = data[i:i + 1]
        if c in b" \t\r\n":
            i += 1
        elif c == b"#":
            while i < n and data[i:i + 1] != b"\n":
                i += 1
        else:
            j = i
            while j < n and data[j:j + 1] not in b" \t\r\n":
                j += 1
            toks.append(int(data[i:j]))
            i = j
    i += 1  # the single whitespace byte after maxval
    w, h, maxval = toks
    if maxval != 255:
        raise RuntimeError(f"{path}: maxval {maxval} != 255")
    px = data[i:]
    if len(px) < w * h * 3:
        raise RuntimeError(f"{path}: truncated pixel data")
    return w, h, px


def sample(px, w, x, y):
    off = (y * w + x) * 3
    return (px[off], px[off + 1], px[off + 2])


if offline:
    q = None
else:
    q = Qmp(sock_path)
    q.cmd("screendump", filename=out, device=device, head=head, format="png")
    if not (os.path.getsize(out) > 8
            and open(out, "rb").read(8) == b"\x89PNG\r\n\x1a\n"):
        print(f"screendump: {out} is not a PNG", file=sys.stderr)
        sys.exit(1)
    print(f"screendump: wrote {out} "
          f"({os.path.getsize(out)} bytes; device={device} head={head})")

if verify == 1:
    if offline:
        ppm = offline
    else:
        ppm = out + ".verify.ppm"
        q.cmd("screendump", filename=ppm, device=device, head=head,
              format="ppm")
    w, h, px = parse_ppm(ppm)
    quads = [
        ("TL", w // 4,     h // 4,     (255, 0,   0)),
        ("TR", 3 * w // 4, h // 4,     (0,   255, 0)),
        ("BL", w // 4,     3 * h // 4, (0,   0,   255)),
        ("BR", 3 * w // 4, 3 * h // 4, (255, 255, 255)),
    ]
    ok = True
    for name, x, y, want in quads:
        got = sample(px, w, x, y)
        match = all(abs(g - t) <= 8 for g, t in zip(got, want))
        print(f"screendump: {name} @({x},{y}) rgb={got} want~{want} "
              f"{'OK' if match else 'MISMATCH'}")
        ok = ok and match
    if not offline:
        os.unlink(ppm)
    if not ok:
        print(f"screendump: VERIFY FAIL -- {w}x{h} surface does not show "
              f"the 4-quadrant pattern", file=sys.stderr)
        sys.exit(1)
    print(f"screendump: VERIFY OK -- {w}x{h} shows the P4-L "
          f"4-quadrant pattern")
elif verify == 2:
    # The Aurora console signature (G-4). Exact Bonfire colors
    # (UTOPIA-VISUAL.md section 1: bg #0e0c0c, fg #e4ddd8); statistical
    # coverage so any session content passes and any non-console fails.
    if offline:
        ppm = offline
    else:
        ppm = out + ".verify.ppm"
        q.cmd("screendump", filename=ppm, device=device, head=head,
              format="ppm")
    w, h, px = parse_ppm(ppm)
    BG = (0x0E, 0x0C, 0x0C)
    FG = (0xE4, 0xDD, 0xD8)
    total = w * h
    nbg = nfg = 0
    fg_idx = []
    view = memoryview(px)[: total * 3]
    for idx in range(total):
        off = idx * 3
        r, g, b = view[off], view[off + 1], view[off + 2]
        if (r, g, b) == BG:
            nbg += 1
        elif (r, g, b) == FG:
            nfg += 1
            fg_idx.append(idx)

    # The blend-integrity pass (G-5; the #35 packed-lane class). Glyph
    # cores are exact-fg (the a=255 short-circuit), so the counts above
    # are structurally blind to ANTIALIASED EDGES -- exactly where #35
    # lived (edge channels scattered while cores stayed exact, so the
    # G-4 check passed a violet-fringed screen). Every pixel 8-adjacent
    # to an exact-fg core that is neither exact bg nor exact fg is
    # overwhelmingly default-fg antialiasing; a correct blend is a
    # per-channel convex combination, so each channel lies inside
    # [min(bg,fg)-TOL, max(bg,fg)+TOL]. Cross-color glyph junctions
    # (colored run boundaries, box-drawing seams) measure ~2% of checked
    # edges; the #35 formula scatters ~15% out for the default fg (~72%
    # for saturated colors) -- 5% splits both with margin. Skipped below
    # 100 checked edges (no meaningful AA mass to judge).
    TOL = 6
    lo = [min(BG[i], FG[i]) - TOL for i in range(3)]
    hi = [max(BG[i], FG[i]) + TOL for i in range(3)]
    fgmask = bytearray(total)
    for idx in fg_idx:
        fgmask[idx] = 1
    seen = bytearray(total)
    checked = bad = 0
    for idx in fg_idx:
        y, x = divmod(idx, w)
        for dy in (-1, 0, 1):
            ny = y + dy
            if ny < 0 or ny >= h:
                continue
            base = ny * w
            for dx in (-1, 0, 1):
                nx = x + dx
                if nx < 0 or nx >= w:
                    continue
                nidx = base + nx
                if seen[nidx] or fgmask[nidx]:
                    continue
                seen[nidx] = 1
                noff = nidx * 3
                p = (view[noff], view[noff + 1], view[noff + 2])
                if p == BG:
                    continue
                checked += 1
                if not (lo[0] <= p[0] <= hi[0]
                        and lo[1] <= p[1] <= hi[1]
                        and lo[2] <= p[2] <= hi[2]):
                    bad += 1
    if not offline:
        os.unlink(ppm)
    bg_pct = 100.0 * nbg / total
    edge_bad = checked >= 100 and bad > checked * 0.05
    print(f"screendump: console check {w}x{h}: bg {nbg} ({bg_pct:.1f}%), "
          f"exact-fg {nfg}, edges {checked} ({bad} out-of-envelope)")
    if bg_pct < 40.0 or nfg < 200 or edge_bad:
        why = []
        if bg_pct < 40.0:
            why.append("bg < 40%")
        if nfg < 200:
            why.append("exact-fg < 200 px")
        if edge_bad:
            why.append(f"blend integrity: {bad}/{checked} edge pixels "
                       f"outside the [bg,fg] envelope (> 5%)")
        print(f"screendump: CONSOLE VERIFY FAIL -- {'; '.join(why)}",
              file=sys.stderr)
        sys.exit(1)
    print(f"screendump: CONSOLE VERIFY OK -- the Aurora console is rendered")
PYEOF
