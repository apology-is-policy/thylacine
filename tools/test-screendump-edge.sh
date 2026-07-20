#!/usr/bin/env bash
# tools/test-screendump-edge.sh -- the offline non-vacuity regression for
# screendump.sh -c's blend-integrity pass (G-5; the #35 packed-lane class).
#
# Synthesizes two Aurora-console-shaped P6 frames off-VM:
#   good.ppm -- exact-fg glyph cores ringed by CORRECTLY blended AA
#               (the post-#35 lane-safe na=256-a, >>8 form)
#   bad.ppm  -- the same frame with the AA ring computed by the LITERAL
#               pre-#35 buggy formula (packed R|B lanes summed then /255:
#               division does not distribute over lanes -- 65536 == 1 mod
#               255 -- so the B lane absorbs R_sum's low byte and edge
#               pixels scatter while cores stay exact)
# then asserts `screendump.sh -c -F` PASSES the good frame and FAILS the
# bad one on the blend-integrity arm specifically. The generator
# self-asserts its fixtures (good: 0 edges out of envelope; bad: >= 10%
# out, comfortably past the 5% threshold) so a drift in the Bonfire
# palette or the formula shapes surfaces here, not as silent vacuity.
#
# Runs anywhere: no QEMU, no VM, no build tree. Exit 0 = regression holds.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

tmp="$(mktemp -d "${TMPDIR:-/tmp}/screendump-edge.XXXXXX")"
trap 'rm -rf "$tmp"' EXIT

python3 - "$tmp" <<'PYEOF'
import sys

tmp = sys.argv[1]
BG = (0x0E, 0x0C, 0x0C)   # Bonfire bg (UTOPIA-VISUAL.md section 1.1)
FG = (0xE4, 0xDD, 0xD8)   # Bonfire default fg
W, H = 200, 100


def blend_good(fg, bg, a):
    # The post-#35 lane-safe form (usr/aurora/src/render.rs): na = 256-a,
    # >>8 -- per-lane sums stay <= 0xFF00, no cross-lane carry.
    na = 256 - a
    return tuple(((fg[i] * a + bg[i] * na) >> 8) & 0xFF for i in range(3))


def blend_bad(fg, bg, a):
    # The LITERAL pre-#35 formula: R|B packed into one word, summed, then
    # /255. 65536 == 1 (mod 255), so the division folds R_sum into the B
    # lane: B_out = (R_sum + (R_sum+B_sum)//255) mod 256 -- scattered.
    na = 255 - a
    frb = (fg[0] << 16) | fg[2]
    brb = (bg[0] << 16) | bg[2]
    rb = ((frb * a + brb * na) // 255) & 0x00FF00FF
    g = (fg[1] * a + bg[1] * na) // 255
    return ((rb >> 16) & 0xFF, g & 0xFF, rb & 0xFF)


TOL = 6
lo = [min(BG[i], FG[i]) - TOL for i in range(3)]
hi = [max(BG[i], FG[i]) + TOL for i in range(3)]


def in_envelope(p):
    return all(lo[i] <= p[i] <= hi[i] for i in range(3))


def frame(blend):
    px = bytearray()
    for _ in range(W * H):
        px.extend(BG)
    def put(x, y, c):
        off = (y * W + x) * 3
        px[off:off + 3] = bytes(c)
    # 8 "glyphs": 6x6 exact-fg cores, each ringed by a 1px AA border at
    # alphas cycling a DENSE spread of coverages (real 8-bit AA is
    # near-continuous; a sparse alpha set under-samples the buggy
    # formula's mod-256 scatter and lands marginal). 8*36 = 288 exact-fg
    # pixels (>= 200); 8*28 = 224 ring pixels (>= 100 checked edges).
    alphas = list(range(8, 250, 8))
    ring_out = ring_total = 0
    for gi in range(8):
        gx, gy = 12 + gi * 22, 40
        for dy in range(6):
            for dx in range(6):
                put(gx + dx, gy + dy, FG)
        k = 0
        for dy in range(-1, 7):
            for dx in range(-1, 7):
                if 0 <= dx < 6 and 0 <= dy < 6:
                    continue
                a = alphas[k % len(alphas)]
                k += 1
                c = blend(FG, BG, a)
                put(gx + dx, gy + dy, c)
                ring_total += 1
                if not in_envelope(c):
                    ring_out += 1
    return bytes(px), ring_out, ring_total


def write_ppm(path, px):
    with open(path, "wb") as f:
        f.write(b"P6\n%d %d\n255\n" % (W, H))
        f.write(px)


good, gout, gtot = frame(blend_good)
bad, bout, btot = frame(blend_bad)

# Fixture self-assertions: the good frame must be fully in-envelope; the
# bad frame must be decisively out (>= 10% vs the checker's 5%).
assert gout == 0, f"good fixture drifted: {gout}/{gtot} out of envelope"
frac = bout / btot
assert frac >= 0.10, f"bad fixture not decisive: {bout}/{btot} = {frac:.2%}"
print(f"fixtures: good {gout}/{gtot} out; bad {bout}/{btot} "
      f"({frac:.0%}) out of the [bg,fg] envelope")

write_ppm(tmp + "/good.ppm", good)
write_ppm(tmp + "/bad.ppm", bad)
PYEOF

fail=0

echo "== good.ppm (correct blend) must PASS =="
if "$SCRIPT_DIR/screendump.sh" -c -F "$tmp/good.ppm"; then
    echo "   PASS (as expected)"
else
    echo "   FAIL: the checker rejected a correctly blended frame" >&2
    fail=1
fi

echo "== bad.ppm (the pre-#35 buggy blend) must FAIL on blend integrity =="
if out="$("$SCRIPT_DIR/screendump.sh" -c -F "$tmp/bad.ppm" 2>&1)"; then
    echo "   FAIL: the checker PASSED the #35-buggy frame (vacuous gate)" >&2
    echo "$out" | sed 's/^/   | /' >&2
    fail=1
elif ! grep -q "blend integrity" <<<"$out"; then
    echo "   FAIL: rejected, but not on the blend-integrity arm:" >&2
    echo "$out" | sed 's/^/   | /' >&2
    fail=1
else
    echo "   FAIL as expected, on the blend-integrity arm:"
    grep "CONSOLE VERIFY FAIL" <<<"$out" | sed 's/^/   | /'
fi

if [[ "$fail" -ne 0 ]]; then
    echo "test-screendump-edge: REGRESSION BROKEN" >&2
    exit 1
fi
echo "test-screendump-edge: OK (the blend-integrity pass is non-vacuous)"
