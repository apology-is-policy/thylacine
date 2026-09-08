#!/usr/bin/env python3
# gfx_compose.py -- the VERDICT half of the ls-gfx-compose gate: measure a
# Halcyon session screendump against the composition scripture
# (docs/HALCYON-COMPOSITION.md + docs/HALCYON-VISUAL.md) and say whether the
# rendering is mockup-true. No golden image: every check is a PROPERTY the
# scripture pins (the herald is centred, the prose runs at the 1.5 rhythm,
# inline chrome sits at the island height, the rule/status bar/tag bars are
# there, raw output is a mono island, a full-screen program's box drawing
# reaches the cell edges), measured off the pixels with stdlib-only PNG
# decoding (gfx_fp.read_png -- the tools/screendump.sh output format).
#
# Each check was DISCRIMINATED against real captures before it was trusted
# (`--report` on the run-44 rounds: shots1 = phantom rule + no chrome +
# left-aligned deck, shots2 = left-aligned herald + no rule + no pills,
# shots4 = mockup-true), and `--selftest` re-proves the verdict logic on
# synthetic canvases without a boot: a check that cannot fail proves nothing.
#
# Usage:
#   gfx_compose.py --welcome W.png [--raw R.png] [--nora N.png]   # verdict
#   gfx_compose.py --report W.png                                 # measurements
#   gfx_compose.py --selftest
#
# Exit 0 = every requested check passed; 1 = a check failed (each failure is
# printed as `FAIL <check>: <why>`); 2 = usage / unreadable input.

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gfx_fp import read_png  # noqa: E402

# The Daylight tokens the composition paints with (usr/lib/libhalcyon/src/
# theme.rs). RGB, not ARGB: the screendump has no alpha.
SURFACE = (0xF2, 0xEB, 0xE0)      # pane parchment
HEADER = (0xCE, 0xC4, 0xB6)       # tag-bar bg, the island/pill ground
BORDER = (0xA8, 0x98, 0x80)       # the rule, the pill stroke
ISLAND_RULE = (0x7A, 0x68, 0x50)  # `.hal-out` border-left, the island gutter
STATUS_BG = (0x1A, 0x12, 0x0A)    # the status bar ground (= fg)

# HALCYON-COMPOSITION 2-3: 11.5 px prose at line-height 1.5 = a 17 px line
# box, plus the 2 px prose margin between paragraphs (collapsed, as the
# mockup PNG measures). The welcome is mostly one-line paragraphs, so its
# dominant pitch is the PARAGRAPH pitch, 19; the wrapped lines inside a
# paragraph run at 17. The 22 px grid cell (sc1) and the pre-round ~15 px
# face line-height both miss it.
PROSE_PITCH = 19
# The island cell is 6x14 (advance 6, the 12 px em); a pill/code ground is
# the content box + padding: 14-16 px. Anything at the 22 px grid cell is the
# sc1 defect; anything below 12 is not a chrome rect.
CHROME_H = (12, 18)


class Img:
    def __init__(self, w, h, bpp, px):
        self.w, self.h, self.bpp, self.px = w, h, bpp, px

    def at(self, x, y):
        i = (y * self.w + x) * self.bpp
        return (self.px[i], self.px[i + 1], self.px[i + 2])

    @staticmethod
    def blank(w, h, color):
        px = bytearray(w * h * 3)
        for i in range(w * h):
            px[3 * i:3 * i + 3] = bytes(color)
        return Img(w, h, 3, px)

    def fill(self, x0, y0, x1, y1, color):
        for y in range(max(0, y0), min(self.h, y1)):
            for x in range(max(0, x0), min(self.w, x1)):
                i = (y * self.w + x) * 3
                self.px[i:i + 3] = bytes(color)


def near(c, ref, tol=3):
    return all(abs(a - b) <= tol for a, b in zip(c, ref))


def is_ink(c):
    # fg 0x1A120A / fg_dim 0x3A2E22 and their antialiased edges; the pill
    # stroke (0xA89880) and every ground tone are far lighter.
    return (c[0] + c[1] + c[2]) // 3 < 0x70


def runs(flags):
    """[(start, end)] of the True runs in a boolean sequence (end exclusive)."""
    out = []
    start = None
    for i, f in enumerate(flags):
        if f and start is None:
            start = i
        elif not f and start is not None:
            out.append((start, i))
            start = None
    if start is not None:
        out.append((start, len(flags)))
    return out


def tiles(img):
    """The pane inner regions as (x0, x1, y0, y1), left to right. A pane's
    columns are MOSTLY parchment down the display (text covers a fraction
    of any column; a divider or a border covers none), and its rows are
    mostly parchment across the pane (a text row is; a tag bar, the status
    bar are not). Majorities, not probes: a single probe row or column ends
    a region at the first glyph it crosses."""
    ys = list(range(0, img.h, 4))
    cols = []
    for x in range(img.w):
        n = sum(1 for y in ys if near(img.at(x, y), SURFACE))
        cols.append(n * 2 >= len(ys))
    out = []
    for x0, x1 in runs(cols):
        if x1 - x0 < 200:
            continue
        # The pane's rows: the OUTERMOST rows showing any parchment across
        # it (a text row keeps its margins; a full-width island keeps only
        # its insets, so the bar is low). The tag bar above, the status bar
        # and the border below show none.
        xs = list(range(x0, x1, max(1, (x1 - x0) // 64)))
        inside = []
        for y in range(img.h):
            n = sum(1 for x in xs if near(img.at(x, y), SURFACE))
            inside.append(n * 8 >= len(xs))
        ys = [y for y, f in enumerate(inside) if f]
        if not ys:
            continue
        out.append((x0, x1, ys[0], ys[-1] + 1))
    return out


def ink_bands(img, x0, x1, y0, y1):
    """The rows carrying ink inside the region, as (start, end, xmin, xmax)."""
    bands = []
    cur = None
    for y in range(y0, y1):
        xs = [x for x in range(x0, x1) if is_ink(img.at(x, y))]
        if xs:
            if cur is None:
                cur = [y, y + 1, min(xs), max(xs)]
            else:
                cur[1] = y + 1
                cur[2] = min(cur[2], min(xs))
                cur[3] = max(cur[3], max(xs))
        elif cur is not None:
            bands.append(tuple(cur))
            cur = None
    if cur is not None:
        bands.append(tuple(cur))
    return bands


def vruns(img, x0, x1, y0, y1, color, min_len, tol=3):
    """Vertical runs of one colour inside the region, per column, at least
    `min_len` tall, as (x, y, len). A chrome ground holds text, so its ROW
    runs fragment on every glyph; its padding COLUMNS run its full height."""
    out = []
    for x in range(x0, x1):
        col = [near(img.at(x, y), color, tol) for y in range(y0, y1)]
        for s, e in runs(col):
            if e - s >= min_len:
                out.append((x, s + y0, e - s))
    return out


def hrules(img, x0, x1, y0, y1, color, min_frac, tol=3):
    """Rows holding one run of the colour spanning `min_frac` of the width,
    as (x, y, w) -- a 1-px rule is such a row (a 2-px one, two)."""
    out = []
    w = x1 - x0
    for y in range(y0, y1):
        row = [near(img.at(x, y), color, tol) for x in range(x0, x1)]
        for s, e in runs(row):
            if e - s >= w * min_frac:
                out.append((s + x0, y, e - s))
    return out


def ink_profile(img, x0, x1, y0, y1):
    """Ink pixels per row over the region."""
    return [sum(1 for x in range(x0, x1) if is_ink(img.at(x, y))) for y in range(y0, y1)]


def pitch_of(prof, lo=12, hi=26):
    """The dominant line pitch of an ink profile: the lag in [lo, hi] with the
    strongest autocorrelation -- robust to which glyphs top each line, which
    a band-start measure is not (a mono island's caps sit a pixel above the
    prose caps and scatter the starts)."""
    best, best_v = None, -1
    n = len(prof)
    for lag in range(lo, hi + 1):
        v = sum(prof[y] * prof[y + lag] for y in range(0, n - lag))
        if v > best_v:
            best, best_v = lag, v
    return best


def measure_welcome(img):
    m = {}
    ts = tiles(img)
    m["tiles"] = ts
    if not ts:
        return m
    x0, x1, y0, y1 = ts[0]
    inner = (x0 + 4, x1 - 4, y0, y1)
    bands = ink_bands(img, *inner)
    m["bands"] = bands
    # The herald: the first three bands (title, deck, deck), each centred.
    cx = (inner[0] + inner[1]) / 2
    m["herald_offsets"] = [
        ((b[2] + b[3]) / 2 - cx) for b in bands[:3]
    ]
    # The prose rhythm: the ink profile's dominant pitch.
    m["pitch"] = pitch_of(ink_profile(img, *inner))
    # The inline chrome: header-toned vertical runs (a pill's padding column
    # runs the pill's full height); a full-height column belongs to a wide
    # island, not a pill, and is left out.
    m["chrome"] = [r for r in vruns(img, *inner, HEADER, 8) if r[2] < 40]
    # The rule: a border-toned row spanning most of the inner width.
    m["rules"] = hrules(img, *inner, BORDER, 0.8)
    # The tag bar above the tile: ink (its name) in the 20 rows over the pane.
    tag = ink_bands(img, x0, x1, max(0, y0 - 20), y0)
    m["tag_ink"] = bool(tag)
    # The status bar: the display's bottom strip in the status ground.
    ys = img.h - 12
    m["status_frac"] = sum(
        1 for x in range(img.w) if near(img.at(x, ys), STATUS_BG)
    ) / img.w
    return m


def check_welcome(m, fails):
    ts = m.get("tiles") or []
    if len(ts) < 2:
        fails.append("welcome: expected two panes (the tour beside the shell), found %d" % len(ts))
        return
    offs = m["herald_offsets"]
    if len(offs) < 3 or any(abs(o) > 8 for o in offs):
        fails.append("welcome: the herald (title + deck) is not centred: offsets %s" % [round(o, 1) for o in offs])
    if m["pitch"] != PROSE_PITCH:
        fails.append("welcome: the prose rhythm is %s px, not %d" % (m["pitch"], PROSE_PITCH))
    chrome = m["chrome"]
    island = [r for r in chrome if CHROME_H[0] <= r[2] <= CHROME_H[1]]
    tall = [r for r in chrome if r[2] > CHROME_H[1]]
    if len(island) < 5:
        fails.append("welcome: too few inline chrome columns at the island height (%d of %d)" % (len(island), len(chrome)))
    if tall:
        fails.append("welcome: %d inline chrome column(s) taller than the island (%s)" % (len(tall), tall[:3]))
    rules = m["rules"]
    bands = m["bands"]
    # A 1-px rule is one row; tolerate a 2-px one (two adjacent rows).
    rule_rows = sorted(set(r[1] for r in rules))
    distinct = [y for i, y in enumerate(rule_rows) if i == 0 or y - rule_rows[i - 1] > 1]
    if len(distinct) != 1:
        fails.append("welcome: expected exactly one rule, found %d (rows %s)" % (len(distinct), distinct[:4]))
    elif bands and distinct[0] < bands[0][0]:
        fails.append("welcome: the rule sits ABOVE the title (a phantom rule at y=%d)" % distinct[0])
    if not m["tag_ink"]:
        fails.append("welcome: the tour pane has no tag bar text (chrome failed to create?)")
    if m["status_frac"] < 0.5:
        fails.append("welcome: no status bar along the display bottom (%.0f%% status ground)" % (100 * m["status_frac"]))


def measure_raw(img):
    m = {}
    ts = tiles(img)
    m["tiles"] = ts
    if len(ts) < 2:
        return m
    x0, x1, y0, y1 = ts[-1]
    # The island: header-toned columns at least three mono rows tall across
    # most of the pane's width (its padding columns and the gaps between
    # glyphs); the gutter: an island-rule-toned column of the same height.
    ground = vruns(img, x0, x1, y0, y1, HEADER, 40)
    m["island_cols"] = len(set(r[0] for r in ground))
    m["island_wide"] = m["island_cols"] >= (x1 - x0) * 0.25
    m["gutter"] = bool(vruns(img, x0, x1, y0, y1, ISLAND_RULE, 40))
    return m


def check_raw(m, fails):
    if len(m.get("tiles") or []) < 2:
        fails.append("raw: expected two panes, found %d" % len(m.get("tiles") or []))
        return
    if not m["island_wide"]:
        fails.append("raw: the command's output is not a mono island (%d header-toned columns in the shell pane)" % m["island_cols"])
    elif not m["gutter"]:
        fails.append("raw: the output island has no leading gutter rule")


def measure_nora(img):
    m = {}
    ts = tiles(img)
    m["tiles"] = ts
    # The alt screen replaces the pane's parchment; measure the right half of
    # the display for long straight ink runs (a box frame).
    x0, x1 = img.w // 2, img.w
    y0, y1 = 20, img.h - 20
    hruns = 0
    for y in range(y0, y1):
        row = [is_ink(img.at(x, y)) for x in range(x0, x1)]
        if any(e - s >= 120 for s, e in runs(row)):
            hruns += 1
    vruns = 0
    for x in range(x0, x1):
        col = [is_ink(img.at(x, y)) for y in range(y0, y1)]
        if any(e - s >= 120 for s, e in runs(col)):
            vruns += 1
    m["h_lines"] = hruns
    m["v_lines"] = vruns
    return m


def check_nora(m, fails):
    if m["h_lines"] < 2 or m["v_lines"] < 2:
        fails.append("nora: no box-drawing frame (long horizontal ink rows %d, vertical %d)" % (m["h_lines"], m["v_lines"]))


def load(path):
    try:
        w, h, bpp, px = read_png(path)
    except Exception as e:  # noqa: BLE001
        print("gfx_compose: cannot read %s: %s" % (path, e))
        sys.exit(2)
    return Img(w, h, bpp, px)


# --- the self-test: synthetic canvases one variable away ---------------------

def canvas(w=1280, h=800):
    img = Img.blank(w, h, SURFACE)
    # Two panes with tag bars, a divider, and the status bar.
    img.fill(0, 0, w, 20, HEADER)
    img.fill(638, 0, 642, h, BORDER)
    img.fill(0, h - 20, w, h, STATUS_BG)
    img.fill(8, 8, 60, 14, STATUS_BG)  # the left tag's name
    return img


def synth_welcome(centred=True, pitch=PROSE_PITCH, pill_h=14, rule="ok"):
    img = canvas()
    ink = STATUS_BG
    # The herald: three bands.
    for i, wdt in enumerate((260, 100, 180)):
        y = 44 + i * 17
        x = (8 + 630) // 2 - wdt // 2 if centred else 16
        img.fill(x, y, x + wdt, y + 9, ink)
    # Six one-line paragraphs at the pitch, with a pill on each.
    for i in range(6):
        y = 160 + i * pitch
        img.fill(16, y, 300, y + 9, ink)
        img.fill(320, y - 3, 380, y - 3 + pill_h, HEADER)
    if rule == "ok":
        img.fill(16, 300, 624, 301, BORDER)
    elif rule == "top":
        img.fill(16, 30, 624, 31, BORDER)
    return img


def synth_raw(island=True, gutter=True):
    img = canvas()
    if island:
        img.fill(656, 64, 1264, 180, HEADER)
        if gutter:
            img.fill(656, 64, 658, 180, ISLAND_RULE)
    return img


def synth_nora(frame=True):
    img = canvas()
    if frame:
        img.fill(660, 40, 1260, 41, STATUS_BG)
        img.fill(660, 700, 1260, 701, STATUS_BG)
        img.fill(660, 40, 661, 700, STATUS_BG)
        img.fill(1259, 40, 1260, 700, STATUS_BG)
    return img


def selftest():
    def verdict(kind, img):
        fails = []
        if kind == "welcome":
            check_welcome(measure_welcome(img), fails)
        elif kind == "raw":
            check_raw(measure_raw(img), fails)
        else:
            check_nora(measure_nora(img), fails)
        return fails

    cases = [
        ("welcome mockup-true", "welcome", synth_welcome(), True),
        ("welcome left-aligned herald", "welcome", synth_welcome(centred=False), False),
        ("welcome 22px rhythm", "welcome", synth_welcome(pitch=22), False),
        ("welcome grid-cell pills", "welcome", synth_welcome(pill_h=22), False),
        ("welcome no rule", "welcome", synth_welcome(rule="none"), False),
        ("welcome phantom rule above the title", "welcome", synth_welcome(rule="top"), False),
        ("raw mono island", "raw", synth_raw(), True),
        ("raw proportional (no island)", "raw", synth_raw(island=False), False),
        ("raw island without a gutter", "raw", synth_raw(gutter=False), False),
        ("nora box frame", "nora", synth_nora(), True),
        ("nora no box drawing", "nora", synth_nora(frame=False), False),
    ]
    bad = 0
    for name, kind, img, expect_pass in cases:
        fails = verdict(kind, img)
        ok = (not fails) == expect_pass
        print("%s %s -> %s" % ("ok  " if ok else "BAD ", name, "PASS" if not fails else "FAIL: " + "; ".join(fails)))
        if not ok:
            bad += 1
    if bad:
        print("gfx_compose selftest: %d case(s) did not discriminate" % bad)
        return 1
    print("gfx_compose selftest: %d cases discriminate" % len(cases))
    return 0


def main(argv):
    if "--selftest" in argv:
        return selftest()
    if "--report" in argv:
        img = load(argv[argv.index("--report") + 1])
        for k, v in measure_welcome(img).items():
            if k == "bands":
                print("bands: %d (first %s)" % (len(v), v[:4]))
            elif k == "chrome":
                print("chrome: %d columns, heights %s" % (len(v), sorted(set(r[2] for r in v))))
            elif k == "rules":
                print("rules: rows %s" % sorted(set(r[1] for r in v))[:6])
            else:
                print("%s: %s" % (k, v))
        r = measure_raw(img)
        print("raw island columns: %s wide: %s gutter: %s" % (r.get("island_cols"), r.get("island_wide"), r.get("gutter")))
        n = measure_nora(img)
        print("nora lines: h=%s v=%s" % (n["h_lines"], n["v_lines"]))
        return 0
    fails = []
    ran = 0
    for flag, meas, chk in (
        ("--welcome", measure_welcome, check_welcome),
        ("--raw", measure_raw, check_raw),
        ("--nora", measure_nora, check_nora),
    ):
        if flag in argv:
            chk(meas(load(argv[argv.index(flag) + 1])), fails)
            ran += 1
    if ran == 0:
        print(__doc__ or "usage: see the header")
        return 2
    for f in fails:
        print("FAIL " + f)
    if not fails:
        print("gfx_compose: %d capture(s) mockup-true" % ran)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
