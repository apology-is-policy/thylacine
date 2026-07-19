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
# Exit: 0 on success (and verify pass when -v/-c); nonzero otherwise.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

sock="$REPO_ROOT/build/qmp.sock"
device="gpu0"
head=0
verify=0

usage() {
    cat >&2 <<EOF
usage: tools/screendump.sh [-s QMP_SOCK] [-d DEVICE_ID] [-H HEAD] [-v] OUT.png

  -s QMP_SOCK   QMP unix socket (default: build/qmp.sock -- what
                tools/run-vm.sh opens unless THYLACINE_NO_QMP=1)
  -d DEVICE_ID  display device qdev id (default: gpu0, matching
                run-vm.sh's -device virtio-gpu-device,id=gpu0)
  -H HEAD       scanout head (default: 0)
  -v            verify the P4-L 4-quadrant test pattern via a PPM
                sibling dump (TL red, TR green, BL blue, BR white)
  -c            verify the Aurora console signature (Bonfire bg dominant
                + exact default-fg text pixels present)
EOF
    exit 2
}

while getopts "s:d:H:vch" opt; do
    case "$opt" in
        s) sock="$OPTARG" ;;
        d) device="$OPTARG" ;;
        H) head="$OPTARG" ;;
        v) verify=1 ;;
        c) verify=2 ;;
        *) usage ;;
    esac
done
shift $((OPTIND - 1))
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

exec python3 - "$sock" "$device" "$head" "$out" "$verify" <<'PYEOF'
import json, os, socket, sys

sock_path, device, head, out = sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4]
verify = int(sys.argv[5])   # 0 = none, 1 = -v quadrants, 2 = -c console


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


q = Qmp(sock_path)
q.cmd("screendump", filename=out, device=device, head=head, format="png")
if not (os.path.getsize(out) > 8
        and open(out, "rb").read(8) == b"\x89PNG\r\n\x1a\n"):
    print(f"screendump: {out} is not a PNG", file=sys.stderr)
    sys.exit(1)
print(f"screendump: wrote {out} "
      f"({os.path.getsize(out)} bytes; device={device} head={head})")

if verify == 1:
    ppm = out + ".verify.ppm"
    q.cmd("screendump", filename=ppm, device=device, head=head, format="ppm")
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
    ppm = out + ".verify.ppm"
    q.cmd("screendump", filename=ppm, device=device, head=head, format="ppm")
    w, h, px = parse_ppm(ppm)
    BG = (0x0E, 0x0C, 0x0C)
    FG = (0xE4, 0xDD, 0xD8)
    total = w * h
    nbg = nfg = 0
    view = memoryview(px)[: total * 3]
    for off in range(0, total * 3, 3):
        r, g, b = view[off], view[off + 1], view[off + 2]
        if (r, g, b) == BG:
            nbg += 1
        elif (r, g, b) == FG:
            nfg += 1
    os.unlink(ppm)
    bg_pct = 100.0 * nbg / total
    print(f"screendump: console check {w}x{h}: bg {nbg} ({bg_pct:.1f}%), "
          f"exact-fg {nfg}")
    if bg_pct < 40.0 or nfg < 200:
        print(f"screendump: CONSOLE VERIFY FAIL -- want bg >= 40% and "
              f"exact-fg >= 200 px", file=sys.stderr)
        sys.exit(1)
    print(f"screendump: CONSOLE VERIFY OK -- the Aurora console is rendered")
PYEOF
