#!/usr/bin/env python3
"""The ANSI-16 tables of the Instrument themes (HALCYON-INSTRUMENT Appendix A).

The Astra kit derived its tables by a rule that breaks hue meaning (yellow was
the signal hue, blue the path hue, and the light themes had black and white
inverted). Programs use these slots BY MEANING -- ls, git diff, grep, vim --
so each slot must read as its hue in the theme's temperature. This tool
designs the sixteen per theme from the theme's own 35 roles, in OKLCH, and
checks them, so the tables can be re-derived rather than trusted.

Input: the kit's resolved-tokens.json (the 13 x 35, exact).  Output: a
markdown table (default), `--toml` (one `ansi = [...]` line per theme) or
`--check` (the report only).  No dependencies beyond Python 3.11.

The rule (the appendix states it; this is the executable form):
  - slots 0/7/8/15 are the theme's greys, in the tree's own convention
    (vt::PARCHMENT for light grounds, the dark themes' for dark): black is a
    ground on a dark theme and the lifted ink on a light one; white is the
    secondary ink; bright black is the dim ink; bright white is the terminal
    text (the legacy alias rule, ansi[15] == fg).
  - slots 1..6 keep their HUES (red 25, yellow 90, green 145, cyan 200, blue
    262, magenta 330 degrees in OKLCH) at the theme's lightness register and
    chroma register; an existing role is KEPT for a slot when its hue is
    within 30 degrees of the target and it clears 3:1 against terminal_bg,
    otherwise the slot is synthesised at the target.
  - bright variants are lighter on a dark ground and darker on a light one,
    a little more saturated; every slot clears 3:1; all sixteen are distinct.
"""
import argparse
import json
import math
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TOKENS = ROOT / "docs/halcyon-carbon-handoff/resolved-tokens.json"

HUES = {"red": 25.0, "yellow": 90.0, "green": 145.0, "cyan": 200.0, "blue": 262.0, "magenta": 330.0}
ORDER = ["red", "green", "yellow", "blue", "magenta", "cyan"]  # slots 1..6
# The roles a slot may be KEPT from, in preference order.
CANDIDATES = {
    "red": ["error"],
    "green": ["success"],
    "yellow": ["amber", "code-text"],
    "blue": ["terminal-path", "syntax-type"],
    "magenta": ["syntax-number", "syntax-lifetime"],
    "cyan": ["terminal-path", "syntax-type", "code-text"],
}
HUE_TOLERANCE = 30.0
MIN_CONTRAST = 3.0


# --- colour maths -----------------------------------------------------------

def hex_to_rgb(h):
    h = h.lstrip("#")
    return tuple(int(h[i:i + 2], 16) for i in (0, 2, 4))


def rgb_to_hex(rgb):
    return "#%02X%02X%02X" % rgb


def to_linear(c):
    c /= 255.0
    return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4


def from_linear(v):
    v = min(1.0, max(0.0, v))
    return 12.92 * v if v <= 0.0031308 else 1.055 * v ** (1 / 2.4) - 0.055


def luminance(rgb):
    r, g, b = (to_linear(c) for c in rgb)
    return 0.2126 * r + 0.7152 * g + 0.0722 * b


def contrast(a, b):
    la, lb = sorted((luminance(a), luminance(b)))
    return (lb + 0.05) / (la + 0.05)


def rgb_to_oklab(rgb):
    r, g, b = (to_linear(c) for c in rgb)
    l = 0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b
    m = 0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b
    s = 0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b
    l_, m_, s_ = (v ** (1 / 3) if v > 0 else 0.0 for v in (l, m, s))
    return (
        0.2104542553 * l_ + 0.7936177850 * m_ - 0.0040720468 * s_,
        1.9779984951 * l_ - 2.4285922050 * m_ + 0.4505937099 * s_,
        0.0259040371 * l_ + 0.7827717662 * m_ - 0.8086757660 * s_,
    )


def oklab_to_rgb_linear(L, a, b):
    l_ = L + 0.3963377774 * a + 0.2158037573 * b
    m_ = L - 0.1055613458 * a - 0.0638541728 * b
    s_ = L - 0.0894841775 * a - 1.2914855480 * b
    l, m, s = l_ ** 3, m_ ** 3, s_ ** 3
    return (
        4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s,
        -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s,
        -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s,
    )


def in_gamut(lin):
    return all(-1e-4 <= v <= 1 + 1e-4 for v in lin)


def oklch(rgb):
    L, a, b = rgb_to_oklab(rgb)
    return L, math.hypot(a, b), math.degrees(math.atan2(b, a)) % 360.0


def from_oklch(L, C, h):
    """The nearest in-gamut sRGB for an OKLCH colour: chroma is reduced until
    the colour fits (hue and lightness kept), then rounded."""
    hr = math.radians(h)
    c = C
    for _ in range(64):
        lin = oklab_to_rgb_linear(L, c * math.cos(hr), c * math.sin(hr))
        if in_gamut(lin):
            break
        c *= 0.94
    lin = oklab_to_rgb_linear(L, c * math.cos(hr), c * math.sin(hr))
    return tuple(int(round(from_linear(v) * 255)) for v in lin)


def hue_distance(a, b):
    d = abs(a - b) % 360.0
    return min(d, 360.0 - d)


def mix(a, b, t):
    return tuple(int(round(x * (1 - t) + y * t)) for x, y in zip(a, b))


# --- the design -------------------------------------------------------------

def design(theme):
    """One theme's sixteen, plus a note per slot saying where it came from."""
    c = {k: hex_to_rgb(v) for k, v in theme["colors"].items()}
    light = theme["light"]
    bg, fg = c["terminal-bg"], c["terminal-text"]
    L_fg = oklch(fg)[0]
    # The lightness register: the normal slots sit below the text on a dark
    # ground and above it on a light one; the bright ones nearer the text.
    if light:
        L_norm = min(max(L_fg + 0.16, 0.42), 0.50)
        L_bright = L_norm + 0.08
    else:
        L_norm = min(max(L_fg - 0.12, 0.58), 0.80)
        L_bright = min(max(L_fg - 0.03, 0.66), 0.88)
    # The chroma register: the theme's own accents say how saturated it is.
    C_reg = sum(oklch(c[k])[1] for k in ("error", "success", "amber")) / 3.0
    C_target = min(max(1.25 * C_reg, 0.065), 0.12)
    floors = {"yellow": 0.078, "magenta": 0.072, "blue": 0.07}

    slots, notes = [None] * 16, [None] * 16
    used = set()

    def take(i, rgb, note):
        # Distinctness: nudge lightness in tiny steps until unique.
        L, C, h = oklch(rgb)
        cand = rgb
        step = 0.006 if not light else -0.006
        k = 0
        while cand in used and k < 40:
            k += 1
            cand = from_oklch(L + step * k, C, h)
        slots[i], notes[i] = cand, note + (" (nudged)" if k else "")
        used.add(cand)

    # The greys.
    if light:
        take(0, mix(fg, bg, 0.12), "ink lifted 12% (black)")
        take(7, c["dim"], "dim (white)")
        take(8, mix(c["dim"], bg, 0.35), "dim lifted 35% (br.black)")
    else:
        take(0, min((c["desktop"], c["pane"]), key=luminance), "the darker ground (black)")
        take(7, c["secondary"], "secondary (white)")
        take(8, c["dim"], "dim (br.black)")
    # The six hues, normal then bright.
    for idx, name in enumerate(ORDER, start=1):
        target = HUES[name]
        chosen, note = None, ""
        for role in CANDIDATES[name]:
            rgb = c[role]
            L, C, h = oklch(rgb)
            if hue_distance(h, target) <= HUE_TOLERANCE and contrast(rgb, bg) >= MIN_CONTRAST and C >= 0.03:
                chosen, note = rgb, f"kept {role}"
                break
        if chosen is None:
            C_t = max(C_target, floors.get(name, 0.0))
            chosen = from_oklch(L_norm, C_t, target)
            # Raise lightness until the slot clears 3:1 (a dark ground) or
            # lower it (a light one); the hue and chroma are kept.
            L = L_norm
            for _ in range(30):
                if contrast(chosen, bg) >= MIN_CONTRAST:
                    break
                L += -0.02 if light else 0.02
                chosen = from_oklch(L, C_t, target)
            note = f"synth h={target:.0f}"
        take(idx, chosen, note)
        # The bright twin: lighter (dark) / darker (light), a little more
        # saturated, at the slot's own hue.
        L, C, h = oklch(chosen)
        Lb = L_bright if not light else min(L + 0.08, 0.60)
        if not light and Lb <= L + 0.04:
            Lb = min(L + 0.08, 0.90)
        bright = from_oklch(Lb, min(C * 1.12 + 0.01, 0.16), h)
        for _ in range(30):
            if contrast(bright, bg) >= MIN_CONTRAST:
                break
            Lb += -0.02 if light else 0.02
            bright = from_oklch(Lb, min(C * 1.12 + 0.01, 0.16), h)
        take(idx + 8, bright, f"bright of {name}")
    # Bright white is the terminal text (the alias rule; distinct from fg is
    # NOT required here -- it is the one permitted equality).
    slots[15], notes[15] = fg, "terminal-text (br.white, == fg)"
    return slots, notes


def report(theme, slots, notes):
    c = {k: hex_to_rgb(v) for k, v in theme["colors"].items()}
    bg = c["terminal-bg"]
    lines = []
    names = ["black", "red", "green", "yellow", "blue", "magenta", "cyan", "white",
             "br.black", "br.red", "br.green", "br.yellow", "br.blue", "br.magenta", "br.cyan", "br.white"]
    problems = []
    for i, (rgb, note) in enumerate(zip(slots, notes)):
        L, C, h = oklch(rgb)
        cr = contrast(rgb, bg)
        flag = ""
        if 1 <= (i % 8) <= 6:
            target = HUES[ORDER[(i % 8) - 1]]
            if hue_distance(h, target) > HUE_TOLERANCE:
                flag += " HUE!"
            if cr < MIN_CONTRAST:
                flag += " CONTRAST!"
        if flag:
            problems.append(names[i])
        lines.append(f"  {i:2} {names[i]:10} {rgb_to_hex(rgb)}  L={L:.2f} C={C:.3f} h={h:5.1f}  {cr:4.1f}:1  {note}{flag}")
    distinct = len(set(slots[:15])) == 15 and slots[15] == c["terminal-text"]
    if not distinct:
        problems.append("distinctness")
    head = f"{theme['name']} ({'light' if theme['light'] else 'dark'}; bg {rgb_to_hex(bg)})" + ("" if not problems else f"  PROBLEMS: {', '.join(problems)}")
    return head, lines, problems


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--toml", action="store_true", help="print one `ansi = [...]` line per theme")
    ap.add_argument("--check", action="store_true", help="print the per-slot report only")
    ap.add_argument("--tokens", default=str(TOKENS))
    args = ap.parse_args()
    data = json.load(open(args.tokens))
    themes = data["themes"]
    bad = 0
    if args.toml:
        for key, th in themes.items():
            slots, _ = design(th)
            print(f"# {key}\nansi = [" + ", ".join(f'"{rgb_to_hex(s)}"' for s in slots) + "]")
        return 0
    if args.check:
        for key, th in themes.items():
            slots, notes = design(th)
            head, lines, problems = report(th, slots, notes)
            print(head)
            print("\n".join(lines))
            bad += len(problems)
        print(f"\n{'OK' if not bad else 'PROBLEMS: ' + str(bad)}")
        return 1 if bad else 0
    names = ["black", "red", "green", "yellow", "blue", "magenta", "cyan", "white",
             "br.black", "br.red", "br.green", "br.yellow", "br.blue", "br.magenta", "br.cyan", "br.white"]
    print("| Slot | " + " | ".join(themes) + " |")
    print("|---|" + "---|" * len(themes))
    tables = {k: design(v)[0] for k, v in themes.items()}
    for i, n in enumerate(names):
        print(f"| {i} {n} | " + " | ".join(rgb_to_hex(tables[k][i]) for k in themes) + " |")
    for key, th in themes.items():
        _, _, problems = report(th, tables[key], [""] * 16)
        bad += len(problems)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
