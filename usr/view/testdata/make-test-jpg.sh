#!/usr/bin/env bash
# make-test-jpg.sh -- (re)generate the I-47 JPEG fixtures.
#
# JPEG has no pure-stdlib encoder (unlike make-test-png.py's zlib PNG), so this
# needs a system JPEG encoder: macOS `sips` (used here) or an equivalent. The
# outputs are COMMITTED (testdata/test.jpg beside test.png; src/testdata/quad.jpg
# beside 2x2.png), so the pool bake and the host tests need NO encoder at build
# time -- this script is reproducibility documentation, run only when a fixture
# changes.
#
#   testdata/test.jpg     = the 640x400 witness card (test.png -> JPEG q85), the
#                           inline/gallery E2E fixture (recognizable in a shot).
#   src/testdata/quad.jpg = a 32x32 four-quadrant image (red/green // blue/white),
#                           the decode_jpeg host-test fixture. JPEG is lossy, so
#                           the test asserts APPROXIMATE colors at quadrant
#                           centers (away from the 8x8-block boundaries).
set -euo pipefail
cd "$(dirname "$0")/.."   # the view crate root (usr/view)

command -v sips >/dev/null \
    || { echo "make-test-jpg.sh needs sips (macOS) or an equivalent JPEG encoder" >&2; exit 1; }

# The E2E card: re-encode the committed PNG witness card as JPEG.
sips -s format jpeg -s formatOptions 85 testdata/test.png --out testdata/test.jpg >/dev/null
echo "wrote testdata/test.jpg ($(wc -c < testdata/test.jpg | tr -d ' ') B, from test.png)"

# The lib fixture: a 32x32 four-quadrant source PNG (stdlib), then JPEG q90.
python3 - <<'PY'
import struct, zlib
W = H = 32
def px(x, y):
    top, left = y < H // 2, x < W // 2
    if top and left: return (0xE0, 0x20, 0x20)  # red   TL
    if top:          return (0x20, 0xE0, 0x20)  # green TR
    if left:         return (0x20, 0x20, 0xE0)  # blue  BL
    return (0xF0, 0xF0, 0xF0)                    # white BR
raw = bytearray()
for y in range(H):
    raw.append(0)  # filter type 0 per scanline
    for x in range(W): raw.extend(px(x, y))
def chunk(t, d):
    o = struct.pack(">I", len(d)) + t + d
    return o + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
png = (b"\x89PNG\r\n\x1a\n"
       + chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 2, 0, 0, 0))
       + chunk(b"IDAT", zlib.compress(bytes(raw), 9))
       + chunk(b"IEND", b""))
open("src/testdata/quad.png", "wb").write(png)
PY
sips -s format jpeg -s formatOptions 90 src/testdata/quad.png --out src/testdata/quad.jpg >/dev/null
rm -f src/testdata/quad.png
echo "wrote src/testdata/quad.jpg ($(wc -c < src/testdata/quad.jpg | tr -d ' ') B, 32x32 quadrants)"
