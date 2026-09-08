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
# THE SCALE (HALCYON-SCALE, SC-4): every expectation is a LOGICAL size at
# 96 DPI realized through the operator's one rounding (COMPOSITION 1: round
# half up; hairline max(1), bevel max(2)) at `--scale <pct>` (default 100),
# so the same checks judge a 2.0 capture against the 2.0 table (COMPOSITION
# 6) and -- the discrimination the gate asserts -- REJECT a 1.0 rendering
# judged at 2.0 and a 2.0 rendering judged at 1.0. `--scaled` is the subset
# that holds at any scale on the tour pane's VISIBLE part (a 2.0 tour is
# taller than its pane, so the herald has scrolled off): the tag bar's
# exact bevel / strip / separator rows on both panes (the compositor's carve
# AND halcyond's paint, in one profile), the status bar's height, the prose
# rhythm, the inline chrome at the island height.
#
# Usage:
#   gfx_compose.py [--scale P] --welcome W.png [--raw R.png] [--nora N.png]
#   gfx_compose.py --scale P --scaled S.png [--raw R.png]
#   gfx_compose.py --report W.png                         # measurements
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
BEVEL_TOP = (0xF8, 0xF2, 0xE6)    # the pane bevel's key-light edge
EMBER_DEEP = (0xC8, 0x60, 0x30)   # a resting tile's separator
SAGE_TINT = (0xB8, 0xCC, 0xC4)    # the live tile's strip (exit 0)
SAGE_KEY = (0x1E, 0x58, 0x44)     # its separator
CINNABAR_TINT = (0xDC, 0xB8, 0xB0)  # the live tile's strip (exit != 0)
CINNABAR_KEY = (0x98, 0x28, 0x18)   # its separator

# The strip grounds and their separators (HALCYON-VISUAL 4.2): resting,
# live-sage, live-cinnabar.
STRIPS = ((HEADER, EMBER_DEEP), (SAGE_TINT, SAGE_KEY), (CINNABAR_TINT, CINNABAR_KEY))


def ipx(v, pct):
    """A logical integer size at `pct`, round half up (libhalcyon::scale::ipx)."""
    return (v * pct + 50) // 100


def expectations(pct):
    """Every expectation at a display scale, from the logical table.
    HALCYON-COMPOSITION 2-3: 11.5 px prose at line-height 1.5 = a 17 px line
    box (2.0: 35), plus the 2 px prose margin between paragraphs (collapsed,
    as the mockup PNG measures; 2.0: 4). The welcome is mostly one-line
    paragraphs at 1.0, so its dominant pitch is the PARAGRAPH pitch, 19; the
    wrapped lines inside a paragraph run at 17. The 22 px grid cell (sc1)
    and the pre-round ~15 px face line-height both miss it. The island cell
    is 6x14 at 1.0 (12x27 at 2.0); a pill/code ground is the content box +
    padding: 14-16 px. Anything at the 22 px grid cell is the sc1 defect;
    anything below 12 is not a chrome rect."""
    body = 11.5 * pct / 100.0
    box = int(body * 1.5 + 0.5)
    margin = ipx(2, pct)
    return {
        "pct": pct,
        "box": box,
        "pitch": box + margin,
        "pitch_range": (ipx(12, pct), ipx(26, pct)),
        "chrome": (ipx(12, pct), ipx(18, pct)),
        "chrome_tall": ipx(40, pct),
        "chrome_min": ipx(8, pct),
        "header_h": ipx(20, pct),
        "status_h": ipx(20, pct),
        "hairline": max(1, ipx(1, pct)),
        "bevel": max(2, ipx(2, pct)),
        "gutter_w": ipx(2, pct),
        "island_min": ipx(40, pct),
        "herald_tol": ipx(8, pct),
    }


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


def is_pane_ground(c):
    """Parchment, the island ground (header-toned), or the island's gutter
    rule: a pane's content is any of them. A 2.0 shell pane is MOSTLY
    island (a 17-row grid holds one command's output, and every raw line is
    an island), so a parchment-only majority lost the pane -- or started it
    below its islands -- the first time the scroll-off fix put the islands
    back; and a 4 px gutter down half the pane split its column run."""
    return near(c, SURFACE) or near(c, HEADER) or near(c, ISLAND_RULE)


def tiles(img):
    """The pane inner regions as (x0, x1, y0, y1), left to right. A pane's
    columns are MOSTLY pane ground (parchment or island) down the display
    (text covers a fraction of any column; a divider or a border covers
    none). Its top is the row after its tag strip's SEPARATOR (the hairline
    in one of the three keys, HALCYON-VISUAL 4.2: the strip's ground is
    header-toned like an island, so the separator, not the ground, marks
    where the content begins); a bar-less pane falls back to its first row
    showing ground across it. Its bottom is the last row showing ground
    across it (the cast shadow, the border and the status bar show none).
    Majorities, not probes: a single probe row or column ends a region at
    the first glyph it crosses."""
    ys = list(range(0, img.h, 4))
    cols = []
    for x in range(img.w):
        n = sum(1 for y in ys if is_pane_ground(img.at(x, y)))
        cols.append(n * 2 >= len(ys))
    out = []
    seps = tuple(sep for _, sep in STRIPS)
    for x0, x1 in runs(cols):
        if x1 - x0 < 200:
            continue
        xs = list(range(x0, x1, max(1, (x1 - x0) // 64)))
        inside = []
        for y in range(img.h):
            n = sum(1 for x in xs if is_pane_ground(img.at(x, y)))
            inside.append(n * 8 >= len(xs))
        rows = [y for y, f in enumerate(inside) if f]
        if not rows:
            continue
        y0 = rows[0]
        # The strip's separator: the first separator-majority row in the
        # display's top quarter; the content starts under it.
        for y in range(0, img.h // 4):
            if any(row_majority(img, xs, y, sep) for sep in seps):
                y0 = y + 1
                while y0 < img.h and any(row_majority(img, xs, y0, sep) for sep in seps):
                    y0 += 1
                break
        y1 = rows[-1] + 1
        if y1 <= y0:
            continue
        out.append((x0, x1, y0, y1))
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


def row_majority(img, xs, y, color, tol=3):
    return sum(1 for x in xs if near(img.at(x, y), color, tol)) * 2 > len(xs)


def tag_bar_profile(img, x0, x1, y0, limit=64):
    """The rows directly above a pane's parchment, walking up, as the run
    lengths of (separator, ground, bevel top) -- the compositor's carve (the
    strip height, the header-toned inner hairline above it [HALCYON-VISUAL
    2.4], the bevel) and halcyond's paint (the strip's ground rows + the
    hairline separator on its bottom edge) in ONE profile: the separator is
    the hairline, the ground run is the strip ground + the inner hairline =
    the tag-bar height, the bevel is the bevel. The strip's ground/separator
    pair is whichever of the three keys the tile wears (4.2). None when no
    strip ground sits there. Profiled on the real 1.0 captures before it was
    trusted: (1, 20, 2)."""
    xs = list(range(x0, x1, max(1, (x1 - x0) // 64)))
    y = y0 - 1
    for ground, sep in STRIPS:
        # The separator: the strip's bottom edge, then the ground above it,
        # then the bevel's key-light edge above the strip.
        sep_n = 0
        while y - sep_n >= 0 and sep_n < limit and row_majority(img, xs, y - sep_n, sep):
            sep_n += 1
        if sep_n == 0:
            continue
        yy = y - sep_n
        ground_n = 0
        while yy - ground_n >= 0 and ground_n < limit and row_majority(img, xs, yy - ground_n, ground):
            ground_n += 1
        yy -= ground_n
        bevel_n = 0
        while yy - bevel_n >= 0 and bevel_n < limit and row_majority(img, xs, yy - bevel_n, BEVEL_TOP):
            bevel_n += 1
        return (sep_n, ground_n, bevel_n)
    return None


def status_rows(img):
    """The contiguous status-ground rows up from the display's bottom edge
    (the bar's carved height; the text and the workspace box are a minority
    of any row)."""
    xs = list(range(0, img.w, max(1, img.w // 128)))
    n = 0
    while n < img.h and n < 128 and row_majority(img, xs, img.h - 1 - n, STATUS_BG):
        n += 1
    return n


def measure_welcome(img, pct=100):
    e = expectations(pct)
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
    m["pitch"] = pitch_of(ink_profile(img, *inner), *e["pitch_range"])
    # The inline chrome: header-toned vertical runs (a pill's padding column
    # runs the pill's full height); a full-height column belongs to a wide
    # island, not a pill, and is left out.
    m["chrome"] = [r for r in vruns(img, *inner, HEADER, e["chrome_min"]) if r[2] < e["chrome_tall"]]
    # The rule: a border-toned row spanning most of the inner width.
    m["rules"] = hrules(img, *inner, BORDER, 0.8)
    # The tag bar above the tile: ink (its name) in the strip's rows over
    # the pane, and the exact profile of the rows above the parchment
    # (separator / ground / bevel) on every pane.
    tag = ink_bands(img, x0, x1, max(0, y0 - e["header_h"]), y0)
    m["tag_ink"] = bool(tag)
    m["tag_profiles"] = [tag_bar_profile(img, t[0], t[1], t[2]) for t in ts]
    # The status bar: the display's bottom strip in the status ground, and
    # its carved height.
    ys = img.h - ipx(12, pct)
    m["status_frac"] = sum(
        1 for x in range(img.w) if near(img.at(x, ys), STATUS_BG)
    ) / img.w
    m["status_rows"] = status_rows(img)
    return m


def check_chrome_at_scale(m, e, fails, kind):
    """The checks that hold at any scale on whatever of the tour is visible:
    two panes; on each, the strip's separator is the hairline, the strip
    ground + separator is the tag-bar height, the bevel above is the bevel;
    the status bar is its height; the prose pitch is the box (wrapped lines)
    or the paragraph pitch; inline chrome sits at the island height."""
    ts = m.get("tiles") or []
    if len(ts) < 2:
        fails.append("%s: expected two panes (the tour beside the shell), found %d" % (kind, len(ts)))
        return False
    for i, prof in enumerate(m["tag_profiles"]):
        want = (e["hairline"], e["header_h"], e["bevel"])
        if prof is None:
            fails.append("%s: pane %d has no tag strip above its parchment" % (kind, i))
        elif prof != want:
            fails.append("%s: pane %d tag bar rows (separator, ground, bevel) = %s, want %s at %d%%" % (kind, i, prof, want, e["pct"]))
    if m["status_rows"] != e["status_h"]:
        fails.append("%s: the status bar is %d rows tall, want %d at %d%%" % (kind, m["status_rows"], e["status_h"], e["pct"]))
    if m["status_frac"] < 0.5:
        fails.append("%s: no status bar along the display bottom (%.0f%% status ground)" % (kind, 100 * m["status_frac"]))
    chrome = m["chrome"]
    island = [r for r in chrome if e["chrome"][0] <= r[2] <= e["chrome"][1]]
    tall = [r for r in chrome if r[2] > e["chrome"][1]]
    if len(island) < 5:
        fails.append("%s: too few inline chrome columns at the island height %s (%d of %d)" % (kind, e["chrome"], len(island), len(chrome)))
    if tall:
        fails.append("%s: %d inline chrome column(s) taller than the island (%s)" % (kind, len(tall), tall[:3]))
    return True


def check_welcome(m, fails, pct=100):
    e = expectations(pct)
    if not check_chrome_at_scale(m, e, fails, "welcome"):
        return
    offs = m["herald_offsets"]
    if len(offs) < 3 or any(abs(o) > e["herald_tol"] for o in offs):
        fails.append("welcome: the herald (title + deck) is not centred: offsets %s" % [round(o, 1) for o in offs])
    if m["pitch"] != e["pitch"]:
        fails.append("welcome: the prose rhythm is %s px, not %d" % (m["pitch"], e["pitch"]))
    rules = m["rules"]
    bands = m["bands"]
    # The rule is the hairline: one row at 1.0, two adjacent at 2.0; a
    # gap of more than a hairline between rule rows is a second rule.
    rule_rows = sorted(set(r[1] for r in rules))
    distinct = [y for i, y in enumerate(rule_rows) if i == 0 or y - rule_rows[i - 1] > e["hairline"]]
    if len(distinct) != 1:
        fails.append("welcome: expected exactly one rule, found %d (rows %s)" % (len(distinct), distinct[:4]))
    elif bands and distinct[0] < bands[0][0]:
        fails.append("welcome: the rule sits ABOVE the title (a phantom rule at y=%d)" % distinct[0])
    if not m["tag_ink"]:
        fails.append("welcome: the tour pane has no tag bar text (chrome failed to create?)")


def check_scaled(m, fails, pct):
    """The any-scale subset (see the header): the visible tour at `pct`."""
    e = expectations(pct)
    if not check_chrome_at_scale(m, e, fails, "scaled"):
        return
    if m["pitch"] not in (e["box"], e["pitch"]):
        fails.append("scaled: the prose rhythm is %s px, not the %d box or the %d paragraph pitch at %d%%" % (m["pitch"], e["box"], e["pitch"], pct))


def measure_raw(img, pct=100):
    e = expectations(pct)
    m = {}
    ts = tiles(img)
    m["tiles"] = ts
    if len(ts) < 2:
        return m
    x0, x1, y0, y1 = ts[-1]
    # The island: header-toned columns at least three mono rows tall across
    # most of the pane's width (its padding columns and the gaps between
    # glyphs); the gutter: an island-rule-toned column of the same height,
    # `gutter_w` columns wide (the 2 px rule at 1.0, 4 at 2.0).
    ground = vruns(img, x0, x1, y0, y1, HEADER, e["island_min"])
    m["island_cols"] = len(set(r[0] for r in ground))
    m["island_wide"] = m["island_cols"] >= (x1 - x0) * 0.25
    gutter = vruns(img, x0, x1, y0, y1, ISLAND_RULE, e["island_min"])
    m["gutter"] = bool(gutter)
    m["gutter_w"] = len(set(r[0] for r in gutter))
    return m


def check_raw(m, fails, pct=100):
    e = expectations(pct)
    if len(m.get("tiles") or []) < 2:
        fails.append("raw: expected two panes, found %d" % len(m.get("tiles") or []))
        return
    if not m["island_wide"]:
        fails.append("raw: the command's output is not a mono island (%d header-toned columns in the shell pane)" % m["island_cols"])
    elif not m["gutter"]:
        fails.append("raw: the output island has no leading gutter rule")
    elif m["gutter_w"] != e["gutter_w"]:
        fails.append("raw: the island's gutter rule is %d px wide, want %d at %d%%" % (m["gutter_w"], e["gutter_w"], pct))


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

def canvas(s=1, w=1280, h=800):
    """Two panes with tag bars, a divider, and the status bar, at the scale
    factor `s` (1 or 2): the compositor's carve (bevel + strip + status bar)
    and halcyond's strip paint (ground + separator), as the probes of the
    real 1.0 captures profile them -- floor, bevel top, ground, separator,
    then the parchment."""
    img = Img.blank(w, h, SURFACE)
    bevel, hair, header_h, status_h = max(2, 2 * s), max(1, 1 * s), 20 * s, 20 * s
    # The floor row, the bevel, the inner hairline (header-toned), the strip
    # (its ground, then its separator on the bottom edge).
    top = 1 + bevel + hair + header_h
    img.fill(0, 0, w, 1, (0x8A, 0x76, 0x60))
    img.fill(0, 1, w, 1 + bevel, BEVEL_TOP)
    img.fill(0, 1 + bevel, w, top - hair, HEADER)
    img.fill(0, top - hair, w, top, EMBER_DEEP)
    img.fill(638, 0, 642, h, BORDER)
    img.fill(0, h - status_h, w, h, STATUS_BG)
    img.fill(8 * s, 4 + 4 * s, 60 * s, 10 + 4 * s, STATUS_BG)  # the left tag's name
    return img, top


def synth_welcome(centred=True, pitch=None, pill_h=None, rule="ok", s=1, bad_sep=False):
    img, top = canvas(s)
    e = expectations(100 * s)
    pitch = e["pitch"] if pitch is None else pitch
    pill_h = 14 * s if pill_h is None else pill_h
    ink = STATUS_BG
    if bad_sep:
        # A separator of the wrong thickness: the 1.0 hairline on a 2.0 strip.
        img.fill(0, top - 2, 640, top - 1, HEADER)
    # The herald: three bands.
    for i, wdt in enumerate((260, 100, 180)):
        y = (44 + i * 17) * s
        x = (8 + 630) // 2 - wdt * s // 2 if centred else 16
        img.fill(x, y, x + wdt * s, y + 9 * s, ink)
    # Six one-line paragraphs at the pitch, with a pill on each.
    for i in range(6):
        y = 160 * s + i * pitch
        img.fill(16, y, 300, y + 9 * s, ink)
        img.fill(320, y - 3 * s, 380, y - 3 * s + pill_h, HEADER)
    if rule == "ok":
        img.fill(16, 300 * s, 624, 300 * s + max(1, s), BORDER)
    elif rule == "top":
        img.fill(16, 30 * s, 624, 30 * s + max(1, s), BORDER)
    return img


def synth_raw(island=True, gutter=True, s=1, gutter_w=None):
    img, _ = canvas(s)
    gutter_w = 2 * s if gutter_w is None else gutter_w
    if island:
        img.fill(656, 64, 1264, 64 + 116 * s, HEADER)
        if gutter:
            img.fill(656, 64, 656 + gutter_w, 64 + 116 * s, ISLAND_RULE)
    return img


def synth_nora(frame=True):
    img, _ = canvas()
    if frame:
        img.fill(660, 40, 1260, 41, STATUS_BG)
        img.fill(660, 700, 1260, 701, STATUS_BG)
        img.fill(660, 40, 661, 700, STATUS_BG)
        img.fill(1259, 40, 1260, 700, STATUS_BG)
    return img


def selftest():
    def verdict(kind, img, pct=100):
        fails = []
        if kind == "welcome":
            check_welcome(measure_welcome(img, pct), fails, pct)
        elif kind == "scaled":
            check_scaled(measure_welcome(img, pct), fails, pct)
        elif kind == "raw":
            check_raw(measure_raw(img, pct), fails, pct)
        else:
            check_nora(measure_nora(img), fails)
        return fails

    cases = [
        ("welcome mockup-true", "welcome", synth_welcome(), 100, True),
        ("welcome left-aligned herald", "welcome", synth_welcome(centred=False), 100, False),
        ("welcome 22px rhythm", "welcome", synth_welcome(pitch=22), 100, False),
        ("welcome grid-cell pills", "welcome", synth_welcome(pill_h=22), 100, False),
        ("welcome no rule", "welcome", synth_welcome(rule="none"), 100, False),
        ("welcome phantom rule above the title", "welcome", synth_welcome(rule="top"), 100, False),
        ("raw mono island", "raw", synth_raw(), 100, True),
        ("raw proportional (no island)", "raw", synth_raw(island=False), 100, False),
        ("raw island without a gutter", "raw", synth_raw(gutter=False), 100, False),
        ("nora box frame", "nora", synth_nora(), 100, True),
        ("nora no box drawing", "nora", synth_nora(frame=False), 100, False),
        # HALCYON-SCALE: the 2.0 table judged at 2.0 passes; a 1.0 rendering
        # judged at 2.0 and a 2.0 rendering judged at 1.0 both FAIL (the
        # discrimination); the wrong separator thickness alone fails.
        ("scaled 2.0 mockup-true at 200", "scaled", synth_welcome(s=2), 200, True),
        ("scaled: a 1.0 rendering judged at 200", "scaled", synth_welcome(), 200, False),
        ("scaled: a 2.0 rendering judged at 100", "scaled", synth_welcome(s=2), 100, False),
        ("scaled: a 1.0 hairline on the 2.0 strip", "scaled", synth_welcome(s=2, bad_sep=True), 200, False),
        ("scaled: 1.0 pills on the 2.0 page", "scaled", synth_welcome(s=2, pill_h=14), 200, False),
        ("welcome 2.0 mockup-true at 200", "welcome", synth_welcome(s=2), 200, True),
        ("raw 2.0 island at 200", "raw", synth_raw(s=2), 200, True),
        ("raw: a 1.0 gutter on the 2.0 island", "raw", synth_raw(s=2, gutter_w=2), 200, False),
        ("raw: a 2.0 island judged at 100", "raw", synth_raw(s=2), 100, False),
    ]
    bad = 0
    for name, kind, img, pct, expect_pass in cases:
        fails = verdict(kind, img, pct)
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
    pct = 100
    if "--scale" in argv:
        try:
            pct = int(argv[argv.index("--scale") + 1])
        except (IndexError, ValueError):
            print("gfx_compose: --scale wants a percent")
            return 2
        if pct not in (100, 125, 150, 175, 200):
            print("gfx_compose: --scale %d is not one of 100/125/150/175/200" % pct)
            return 2
    if "--report" in argv:
        img = load(argv[argv.index("--report") + 1])
        for k, v in measure_welcome(img, pct).items():
            if k == "bands":
                print("bands: %d (first %s)" % (len(v), v[:4]))
            elif k == "chrome":
                print("chrome: %d columns, heights %s" % (len(v), sorted(set(r[2] for r in v))))
            elif k == "rules":
                print("rules: rows %s" % sorted(set(r[1] for r in v))[:6])
            else:
                print("%s: %s" % (k, v))
        r = measure_raw(img, pct)
        print("raw island columns: %s wide: %s gutter: %s (%s px)" % (r.get("island_cols"), r.get("island_wide"), r.get("gutter"), r.get("gutter_w")))
        n = measure_nora(img)
        print("nora lines: h=%s v=%s" % (n["h_lines"], n["v_lines"]))
        return 0
    fails = []
    ran = 0
    for flag, meas, chk in (
        ("--welcome", measure_welcome, check_welcome),
        ("--scaled", measure_welcome, check_scaled),
        ("--raw", measure_raw, check_raw),
    ):
        if flag in argv:
            chk(meas(load(argv[argv.index(flag) + 1]), pct), fails, pct)
            ran += 1
    if "--nora" in argv:
        check_nora(measure_nora(load(argv[argv.index("--nora") + 1])), fails)
        ran += 1
    if ran == 0:
        print(__doc__ or "usage: see the header")
        return 2
    for f in fails:
        print("FAIL " + f)
    if not fails:
        print("gfx_compose: %d capture(s) mockup-true at %d%%" % (ran, pct))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
