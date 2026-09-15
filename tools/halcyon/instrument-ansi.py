#!/usr/bin/env python3
"""The ANSI-16 lint of the Instrument themes (HALCYON-INSTRUMENT Appendix A).

Programs use the sixteen slots BY MEANING -- ls, git diff, grep, vim -- so
each slot must read as its hue in the theme's temperature, clear the
terminal ground, and keep the conventional polarity. The shipped tables are
Astra's hand-authored set (round 2, 2026-09-14); this tool is the CHECK
they are held to, so a table is verified rather than trusted:

  `--lint PATH`   PATH is an ansi16.json ({theme: [16 hex]}) or a directory
                  of stock palettes (`[terminal] ansi` per <theme>.toml).
                  Exit 1 on any violation. The rule:
    - every slot >= 3:1 against terminal_bg, black included (a readable
      charcoal on a dark ground, the lightest readable neutral on a light
      one -- never the ground itself);
    - all sixteen distinct (bright white MAY equal terminal_text, the
      alias the parser allows);
    - slots 1..6 within 30 degrees of their hue names (OKLCH: red 25,
      green 145, yellow 90, blue 262, magenta 330, cyan 200);
    - bright slots lighter than their normals on a dark theme, darker on
      a light one; within each eight-slot ramp black and white are the
      two extremes.
    A chromatic slot under chroma 0.03 is NOTED as grey, not failed.

The generator (`--toml`, `--check`, the default table) is what main wrote
before round 2 arrived: a first draft for a NEW theme from its 35 roles,
in OKLCH. It over-saturates warm themes (its chroma register is 1.25 x the
theme's accents), so it is never the source of a shipped table; `--check`
holds its output to the same lint.

Input: resolved-tokens.json (the 13 x 35; `--tokens` to point elsewhere).
No dependencies beyond Python 3.11.
"""
import argparse
import json
import math
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
# The adopted 13 x 35 (round 2: the kit's colours + the 45 contrast
# replacements); the kit's own resolved-tokens.json is the historical set.
TOKENS = ROOT / "docs/halcyon-carbon-handoff/round2/resolved-tokens-round2.json"

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
    L_fg, L_dim = oklch(fg)[0], oklch(c["dim"])[0]

    def lift(a, b, floor):
        # a moved toward b in 2% steps until it clears the contrast floor.
        out, t = a, 0.0
        while contrast(out, bg) < floor and t < 1.0:
            t += 0.02
            out = mix(a, b, t)
        return out, t

    # The greys first, because the ramp rule (black and white are each
    # ramp's two extremes) bounds where the chromatic slots may sit.
    if light:
        # Black is the LIGHTEST readable neutral (dim faded toward the ground
        # to just clear 3:1); white the darkest of the normal ramp (the text
        # lifted 12%); bright black is dim; bright white is the text.
        black, t = lift(bg, c["dim"], MIN_CONTRAST)
        white, brblack = mix(fg, bg, 0.12), c["dim"]
        L_black, L_white = oklch(black)[0], oklch(white)[0]
        L_norm = min(max(L_fg + 0.16, L_white + 0.04), L_black - 0.04)
        L_bright = min(max(L_norm - 0.08, L_fg + 0.03), L_dim - 0.03)
        grey_notes = (f"dim faded to {t * 100:.0f}% (black)", "text lifted 12% (white)", "dim (br.black)")
    else:
        # Black is the darker ground lifted toward the text until it reads
        # as ink; white is the secondary ink; bright black is dim.
        black, t = lift(min((c["desktop"], c["pane"]), key=luminance), fg, MIN_CONTRAST)
        white, brblack = c["secondary"], c["dim"]
        L_black, L_white = oklch(black)[0], oklch(white)[0]
        L_norm = min(max(L_fg - 0.12, L_black + 0.04), L_white - 0.03)
        L_bright = min(max(L_fg - 0.03, L_dim + 0.03), L_fg - 0.03)
        grey_notes = (f"ground lifted {t * 100:.0f}% (black)", "secondary (white)", "dim (br.black)")
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

    take(0, black, grey_notes[0])
    take(7, white, grey_notes[1])
    take(8, brblack, grey_notes[2])
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
        if chosen is None or not (min(L_black, L_white) < oklch(chosen)[0] < max(L_black, L_white)):
            # A kept role outside the normal ramp's band is re-synthesised
            # at its hue, so black and white stay the ramp's extremes.
            C_t = max(C_target, floors.get(name, 0.0)) if chosen is None else max(oklch(chosen)[1], 0.03)
            chosen = from_oklch(L_norm, C_t, target if note == "" else oklch(chosen)[2])
            # Raise lightness until the slot clears 3:1 (a dark ground) or
            # lower it (a light one); the hue and chroma are kept.
            L = L_norm
            for _ in range(30):
                if contrast(chosen, bg) >= MIN_CONTRAST:
                    break
                L += -0.02 if light else 0.02
                chosen = from_oklch(L, C_t, target if note == "" else oklch(chosen)[2])
            note = f"synth h={target:.0f}" if note == "" else note + " (re-lit)"
        take(idx, chosen, note)
        # The bright twin: lighter (dark) / darker (light), a little more
        # saturated, at the slot's own hue, inside the bright ramp's band.
        L, C, h = oklch(chosen)
        if light:
            Lb = min(L_bright, L - 0.04)
            Lb = max(Lb, L_fg + 0.03)
        else:
            Lb = max(L_bright, L + 0.04)
            Lb = min(Lb, L_fg - 0.03)
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


NAMES = ["black", "red", "green", "yellow", "blue", "magenta", "cyan", "white",
         "br.black", "br.red", "br.green", "br.yellow", "br.blue", "br.magenta", "br.cyan", "br.white"]


def report(theme, slots, notes):
    """Appendix A's rule over any sixteen: (head, per-slot lines, problems)."""
    c = {k: hex_to_rgb(v) for k, v in theme["colors"].items()}
    bg, fg = c["terminal-bg"], c["terminal-text"]
    light = theme["light"]
    lines, problems = [], []
    if len(slots) != 16:
        return f"{theme['name']}: {len(slots)} slots, not 16", [], ["count"]
    lums = [luminance(s) for s in slots]
    for i, (rgb, note) in enumerate(zip(slots, notes)):
        L, C, h = oklch(rgb)
        cr = contrast(rgb, bg)
        flags, notes_ = [], []
        if cr < MIN_CONTRAST:
            flags.append("CONTRAST")
        if 1 <= (i % 8) <= 6:
            target = HUES[ORDER[(i % 8) - 1]]
            if hue_distance(h, target) > HUE_TOLERANCE:
                flags.append("HUE")
            if C < 0.03:
                notes_.append("grey")
        if i >= 8 and (lums[i] < lums[i - 8]) != light:
            flags.append("POLARITY")
        problems += [f"{NAMES[i]}:{f}" for f in flags]
        tail = ("  " + " ".join(f + "!" for f in flags) if flags else "") + ("  (" + ", ".join(notes_) + ")" if notes_ else "")
        lines.append(f"  {i:2} {NAMES[i]:10} {rgb_to_hex(rgb)}  L={L:.2f} C={C:.3f} h={h:5.1f}  {cr:4.1f}:1  {note}{tail}")
    for off in (0, 8):
        ramp = lums[off:off + 8]
        if ramp[0] != (max(ramp) if light else min(ramp)):
            problems.append(f"{NAMES[off]}:RAMP (not the {'lightest' if light else 'darkest'})")
        if ramp[7] != (min(ramp) if light else max(ramp)):
            problems.append(f"{NAMES[off + 7]}:RAMP (not the {'darkest' if light else 'lightest'})")
    distinct = len(set(slots)) == 16 or (len(set(slots[:15])) == 15 and slots[15] == fg)
    if not distinct:
        problems.append("distinctness")
    head = f"{theme['name']} ({'light' if light else 'dark'}; bg {rgb_to_hex(bg)})" + ("" if not problems else f"  PROBLEMS: {', '.join(problems)}")
    return head, lines, problems


def load_tables(path):
    """{theme: [16 rgb]} from an ansi16.json or a directory of stock TOMLs."""
    p = Path(path)
    if p.is_dir():
        import tomllib
        out = {}
        for f in sorted(p.glob("*.toml")):
            doc = tomllib.loads(f.read_text())
            out[f.stem] = [hex_to_rgb(x) for x in doc["terminal"]["ansi"]]
        return out
    return {k: [hex_to_rgb(x) for x in v] for k, v in json.load(open(p)).items()}


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--lint", metavar="PATH", help="check the tables in an ansi16.json or a palettes directory")
    ap.add_argument("--toml", action="store_true", help="generator: print one `ansi = [...]` line per theme")
    ap.add_argument("--check", action="store_true", help="generator: lint its own output, per slot")
    ap.add_argument("--tokens", default=str(TOKENS))
    args = ap.parse_args()
    data = json.load(open(args.tokens))
    themes = data["themes"]
    bad = 0
    if args.lint:
        tables = load_tables(args.lint)
        missing = [k for k in themes if k not in tables]
        for key, slots in tables.items():
            if key not in themes:
                print(f"{key}: no such theme in {args.tokens}")
                bad += 1
                continue
            head, lines, problems = report(themes[key], slots, [""] * 16)
            print(head)
            print("\n".join(lines))
            bad += len(problems)
        if missing:
            print(f"not in {args.lint}: {', '.join(missing)}")
            bad += len(missing)
        print(f"\n{'OK' if not bad else 'PROBLEMS: ' + str(bad)}")
        return 1 if bad else 0
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
    print("| Slot | " + " | ".join(themes) + " |")
    print("|---|" + "---|" * len(themes))
    tables = {k: design(v)[0] for k, v in themes.items()}
    for i, n in enumerate(NAMES):
        print(f"| {i} {n} | " + " | ".join(rgb_to_hex(tables[k][i]) for k in themes) + " |")
    for key, th in themes.items():
        _, _, problems = report(th, tables[key], [""] * 16)
        bad += len(problems)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
