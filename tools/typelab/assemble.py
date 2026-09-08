#!/usr/bin/env python3
"""Assemble the Halcyon Type Lab page from typelab's out/ tree.

Reads out/index.tsv + out/metrics.tsv, embeds the selected renders as data
URIs, sets the page in the two Plex faces under study, and writes the HTML.
"""
import base64
import csv
import html
import os
import sys

OUT = sys.argv[1] if len(sys.argv) > 1 else "out"
PAGE = sys.argv[2] if len(sys.argv) > 2 else "halcyon-type-lab.html"
REPO = "/Users/northkillpd/projects/thylacine"
PLEX_TEXT = f"{REPO}/third_party/ibm-plex/ttf/IBMPlexSans-Text.ttf"
PLEX_ITALIC = f"{REPO}/third_party/ibm-plex/ttf/IBMPlexSans-Italic.ttf"

DESC = {
    "A-asbuilt": "halcyond today: fontdue coverage, whole-pixel pen, blend in sRGB",
    "A2-asbuilt-embolden": "as-built + mask emboldening, 0.25 px per side",
    "B-gamma": "as-built, blended in linear light",
    "B2-gamma145": "as-built, blended in a gamma-1.45 space",
    "C-gamma-subpx": "linear blend + third-pixel positioning",
    "D-gamma-subpx-embolden": "linear blend + third-pixel positioning + mask emboldening",
    "D2-srgb-subpx-embolden": "sRGB blend + third-pixel positioning + mask emboldening",
    "E-skrifa-unhinted-gamma-subpx": "skrifa/zeno, unhinted, exact positioning, linear blend",
    "E2-skrifa-unhinted-srgb-subpx": "skrifa/zeno, unhinted, exact positioning, sRGB blend",
    "F-skrifa-light-gamma-subpx": "skrifa autohinter LIGHT (vertical only), linear blend",
    "F2-skrifa-light-srgb-subpx": "skrifa autohinter LIGHT (vertical only), sRGB blend",
    "H-skrifa-interp-gamma-int": "skrifa TrueType interpreter (Plex's own instructions), whole-pixel pen",
    "N0-skrifa-srgb-subpx-stroke015": "candidate: unhinted, exact positioning, sRGB blend, outline stroke 0.015 em",
    "N1-skrifa-srgb-subpx-stroke020": "candidate: outline stroke 0.020 em",
    "N2-skrifa-srgb-subpx-stroke030": "candidate: outline stroke 0.030 em",
    "N3-skrifa-srgb-subpx-stroke040": "candidate: outline stroke 0.040 em",
    "N4-skrifa-light-srgb-subpx-stroke030": "stroke 0.030 em + autohinter LIGHT",
    "N5-skrifa-gamma-subpx-stroke030": "stroke 0.030 em + linear-light blend",
    "N6-skrifa-gamma145-subpx-stroke020": "stroke 0.020 em + gamma-1.45 blend",
    "I-ft-light-srgb-int": "FreeType autohint LIGHT, whole-pixel pen, sRGB blend (the GTK/cairo desktop)",
    "J-ft-light-gamma-subpx": "FreeType LIGHT, third-pixel positioning, linear blend",
    "K-ft-lightdark-gamma-subpx": "FreeType LIGHT + stem darkening, linear blend",
    "L-ft-normal-gamma-int": "FreeType v40 TrueType interpreter, whole-pixel pen, linear blend",
    "M-ft-nohint-srgb-subpx": "FreeType unhinted, third-pixel positioning, sRGB blend (the control)",
    "CT-smooth-subpx": "CoreText: font smoothing ON, subpixel positioning ON (the macOS default)",
    "CT-nosmooth-subpx": "CoreText: font smoothing OFF, subpixel positioning ON",
    "CT-smooth-quant": "CoreText: font smoothing ON, positions quantized to whole pixels",
    "CT-nosmooth-quant": "CoreText: smoothing OFF, whole-pixel positions",
}

SHORT = {k: k.split("-", 1)[0] for k in DESC}


def data_uri(path, mime="image/png"):
    with open(path, "rb") as f:
        return f"data:{mime};base64," + base64.b64encode(f.read()).decode("ascii")


index = {}  # (scale, sample, variant) -> (full, {crop: path})
with open(os.path.join(OUT, "index.tsv")) as f:
    for row in csv.reader(f, delimiter="\t"):
        scale, sample, variant, full, crops = row
        cd = {}
        for c in crops.split(","):
            if "=" in c:
                k, v = c.split("=", 1)
                cd[k] = v
        index[(scale, sample, variant)] = (full, cd)

metrics = {}
with open(os.path.join(OUT, "metrics.tsv")) as f:
    rd = csv.DictReader(f, delimiter="\t")
    for r in rd:
        metrics[(r["scale"], r["sample"], r["variant"])] = r

embedded = {}


def img(path, devpx, cls="", alt=""):
    p = os.path.join(OUT, path)
    if p not in embedded:
        embedded[p] = data_uri(p)
    return f'<img src="{embedded[p]}" data-devpx="{devpx}" class="{cls}" alt="{html.escape(alt)}" loading="lazy">'


def card(scale, variant, crops=("heading", "prose"), full=True, sample="tour", note=None, mark=False):
    full_path, cd = index[(scale, sample, variant)]
    parts = [f'<figure class="card{" card--mark" if mark else ""}">']
    parts.append(f'<figcaption><span class="tag">{html.escape(SHORT.get(variant, variant))}</span> '
                 f'<span class="vname">{html.escape(variant)}</span><br>'
                 f'<span class="vdesc">{html.escape(DESC.get(variant, ""))}</span>'
                 + (f'<br><span class="vnote">{html.escape(note)}</span>' if note else "") + '</figcaption>')
    if full:
        parts.append(f'<div class="strip">{img(full_path, scale, "strip__img", variant)}</div>')
    if crops:
        parts.append('<div class="crops">')
        for c in crops:
            if c in cd:
                parts.append(f'<div class="crop">{img(cd[c], scale, "crop__img", f"{variant} {c}")}</div>')
        parts.append('</div>')
    parts.append('</figure>')
    return "".join(parts)


def grid(cards, cols=None):
    style = f' style="--cols:{cols}"' if cols else ""
    return f'<div class="grid"{style}>' + "".join(cards) + '</div>'


def probe_row(scale, probe, variants):
    cells = []
    for v in variants:
        key = (scale, probe, v)
        if key not in index:
            continue
        _, cd = index[key]
        m = metrics.get(key, {})
        cells.append('<div class="probe">'
                     f'{img(cd["glyph"], scale, "probe__img", f"{v} {probe}")}'
                     f'<div class="probe__label"><span class="tag">{html.escape(SHORT.get(v, v))}</span> '
                     f'<span class="num">w {float(m.get("weight_L", 0)):.2f}</span> '
                     f'<span class="num">fringe {float(m.get("fringe_ink", 0)):.2f} × {float(m.get("fringe_n", 0)):.1f}</span></div>'
                     '</div>')
    return '<div class="probes">' + "".join(cells) + '</div>'


def table(variants, probes):
    head = ['<table class="metrics"><thead><tr><th>variant</th>']
    for (scale, probe, label) in probes:
        head.append(f'<th colspan="3">{html.escape(label)}</th>')
    head.append('</tr><tr><th></th>')
    for _ in probes:
        head.append('<th class="num">weight</th><th class="num">fringe ink</th><th class="num">fringe px</th>')
    head.append('</tr></thead><tbody>')
    rows = []
    for v in variants:
        r = [f'<tr><td><span class="tag">{html.escape(SHORT.get(v, v))}</span> {html.escape(v)}</td>']
        for (scale, probe, _) in probes:
            m = metrics.get((scale, probe, v))
            if not m:
                r.append('<td class="num">–</td>' * 3)
                continue
            r.append(f'<td class="num">{float(m["weight_L"]):.2f}</td>'
                     f'<td class="num">{float(m["fringe_ink"]):.2f}</td>'
                     f'<td class="num">{float(m["fringe_n"]):.1f}</td>')
        r.append('</tr>')
        rows.append("".join(r))
    return "".join(head) + "".join(rows) + '</tbody></table>'


font_text = data_uri(PLEX_TEXT, "font/ttf")
font_italic = data_uri(PLEX_ITALIC, "font/ttf")

CSS = """
@font-face { font-family: "Plex Lab"; font-weight: 450; font-style: normal; src: url(%s) format("truetype"); }
@font-face { font-family: "Plex Lab"; font-weight: 400; font-style: italic; src: url(%s) format("truetype"); }
:root {
  --floor:#8a7660; --surface:#f2ebe0; --header:#cec4b6; --raised:#bdb0a0; --border:#a89880;
  --fg:#1a120a; --fg-dim:#3a2e22; --fg-muted:#6a5a48; --fg-subtle:#9a8878;
  --ember:#e07840; --ember-deep:#c86030; --sage:#1e5844; --cinnabar:#982818;
  --card:#f6f1e8; --code:#e6ddcf;
}
@media (prefers-color-scheme: dark) {
  :root:not([data-theme="light"]) {
    --floor:#0f0c0a; --surface:#1b1511; --header:#2a2119; --raised:#3a2e24; --border:#5a4a3c;
    --fg:#ece2d2; --fg-dim:#cdbfaa; --fg-muted:#a08e78; --fg-subtle:#6e5f4e;
    --ember:#e07840; --ember-deep:#f08a50; --sage:#8fc4ae; --cinnabar:#e08a78;
    --card:#221b16; --code:#2e2620;
  }
}
:root[data-theme="dark"] {
  --floor:#0f0c0a; --surface:#1b1511; --header:#2a2119; --raised:#3a2e24; --border:#5a4a3c;
  --fg:#ece2d2; --fg-dim:#cdbfaa; --fg-muted:#a08e78; --fg-subtle:#6e5f4e;
  --ember:#e07840; --ember-deep:#f08a50; --sage:#8fc4ae; --cinnabar:#e08a78;
  --card:#221b16; --code:#2e2620;
}
html { background: var(--surface); }
body { margin:0; background: var(--surface); color: var(--fg);
  font-family: "Plex Lab", "IBM Plex Sans", "Helvetica Neue", Arial, sans-serif; font-weight: 450;
  font-size: 15px; line-height: 1.55; -webkit-font-smoothing: auto; }
main { max-width: 1160px; margin: 0 auto; padding: 32px 24px 96px; }
h1, h2, h3 { font-family: "Plex Lab", "IBM Plex Sans", "Helvetica Neue", Arial, sans-serif; font-weight: 400; font-style: italic; margin: 0; color: var(--fg); text-wrap: balance; }
h1 { font-size: 34px; line-height: 1.15; margin-bottom: 6px; }
h2 { font-size: 24px; margin-top: 56px; padding-top: 14px; border-top: 1px solid var(--border); }
h3 { font-size: 18px; margin-top: 28px; }
p { max-width: 70ch; margin: 12px 0; }
.lede { font-size: 17px; color: var(--fg-dim); max-width: 74ch; }
.meta { color: var(--fg-muted); font-size: 13px; letter-spacing: 0.02em; text-transform: uppercase; margin-bottom: 20px; }
code, .mono { font-family: "Cornucopia", "Iosevka", ui-monospace, Menlo, Consolas, monospace; font-size: 0.92em; background: var(--code); padding: 0 4px; border-radius: 0; }
.aside { border-left: 2px solid var(--border); padding: 4px 0 4px 14px; color: var(--fg-dim); max-width: 70ch; margin: 14px 0; }
.grid { display: grid; gap: 18px; grid-template-columns: repeat(var(--cols, auto-fill), minmax(300px, 1fr)); margin: 18px 0; }
.card { margin: 0; background: var(--card); border: 1px solid var(--border); padding: 12px 12px 10px; display: flex; flex-direction: column; gap: 10px; }
.card--mark { border-color: var(--ember-deep); box-shadow: 0 0 0 1px var(--ember-deep) inset; }
figcaption { font-size: 13px; line-height: 1.4; color: var(--fg-dim); }
.tag { display: inline-block; min-width: 2.2em; text-align: center; background: var(--header); color: var(--fg); padding: 0 5px; font-weight: 450; font-size: 12px; letter-spacing: 0.04em; }
.card--mark .tag { background: var(--ember); color: #1a120a; }
.vname { color: var(--fg-muted); font-size: 12px; }
.vdesc { color: var(--fg); }
.vnote { color: var(--ember-deep); }
.strip { overflow-x: auto; background: #f2ebe0; border: 1px solid var(--border); }
.strip img { display: block; image-rendering: auto; }
.crops { display: flex; gap: 8px; flex-wrap: wrap; }
.crop { overflow-x: auto; background: #f2ebe0; border: 1px solid var(--border); flex: 1 1 200px; }
.crop img, .probe img { display: block; image-rendering: pixelated; image-rendering: crisp-edges; }
.probes { display: flex; gap: 10px; flex-wrap: wrap; margin: 12px 0 4px; }
.probe { background: var(--card); border: 1px solid var(--border); padding: 6px; }
.probe__label { font-size: 12px; color: var(--fg-dim); margin-top: 4px; display: flex; gap: 8px; align-items: baseline; }
.num { font-variant-numeric: tabular-nums; text-align: right; }
table.metrics { border-collapse: collapse; font-size: 13px; margin: 14px 0; }
table.metrics th, table.metrics td { padding: 4px 9px; border-bottom: 1px solid var(--header); white-space: nowrap; }
table.metrics thead th { border-bottom: 1px solid var(--border); font-weight: 450; color: var(--fg-muted); text-align: left; }
table.metrics thead th.num { text-align: right; }
.tablewrap { overflow-x: auto; }
ul { max-width: 74ch; padding-left: 20px; } li { margin: 6px 0; }
.kv { display: grid; grid-template-columns: max-content 1fr; gap: 4px 18px; max-width: 74ch; font-size: 14px; }
.kv dt { color: var(--fg-muted); } .kv dd { margin: 0; }
a { color: var(--ember-deep); }
.hairline { border: 0; border-top: 1px solid var(--border); margin: 24px 0; }
@media (prefers-reduced-motion: reduce) { * { scroll-behavior: auto; } }
""" % (font_text, font_italic)

JS = """
function fit() {
  var dpr = window.devicePixelRatio || 1;
  document.querySelectorAll('img[data-devpx]').forEach(function (im) {
    if (!im.naturalWidth) return;
    var s = im.getAttribute('data-devpx');
    var cls = im.className;
    var w;
    if (cls.indexOf('crop__img') >= 0 || cls.indexOf('probe__img') >= 0) {
      w = Math.min(im.naturalWidth / dpr, im.parentElement.clientWidth || 1e9);   // one device pixel per magnified pixel
    } else if (s === '2x') {
      w = im.naturalWidth / dpr;              // a 2.0 render at exactly one device pixel per pixel
    } else {
      w = im.naturalWidth * 2 / dpr;          // a 1.0 render shown at a 100%-display's pixel size
    }
    im.style.width = w + 'px';
  });
}
window.addEventListener('load', fit);
window.addEventListener('resize', fit);
document.querySelectorAll('img[data-devpx]').forEach(function (im) { im.addEventListener('load', fit); });
var dprq = matchMedia('(resolution: ' + (window.devicePixelRatio || 1) + 'dppx)');
if (dprq.addEventListener) dprq.addEventListener('change', fit);
"""

sec = []
sec.append("""
<p class="meta">Thylacine · Halcyon · typography research, 2026-09-08</p>
<h1>Halcyon Type Lab</h1>
<p class="lede">The same Daylight text pushed through every rendering recipe under study, next to what macOS does with the identical IBM Plex Sans files. Every image is a real render; nothing is mocked. The page is set in the same two Plex faces so your browser's CoreText is one more specimen on the page.</p>
<dl class="kv">
<dt>Text</dt><dd>The welcome screen's heading (Plex Sans Italic, 17.5 px logical), a prose line with Cornucopia islands (Plex Sans Text 11.5 px, Cornucopia 12 px em), the dim closing line. At 2.0 those are 35 / 23 / 24 device px, at 1.0 they are as stated.</dd>
<dt>Ground and ink</dt><dd><code>#f2ebe0</code> surface, <code>#1a120a</code> fg, <code>#3a2e22</code> fg_dim — Daylight's tokens.</dd>
<dt>How to look</dt><dd>The full strips of 2.0 renders are shown at exactly one device pixel per pixel (on this display: <span id="dpr">?</span>× DPR), so they are what the screen would show. The magnified crops are 4× (2.0) and 8× (1.0), nearest-neighbour, one device pixel per magnified pixel. The 1.0 strips are shown at the pixel size of a 100% display.</dd>
<dt>Kerning</dt><dd>Off everywhere, including CoreText, so spacing differences are the renderer's, not the shaper's. (Plex's kerning lives in GPOS, which fontdue does not read — a separate, known gap.)</dd>
</dl>
""")

sec.append("""
<h2>1. What the eye had been comparing</h2>
<p>The mockup you judged against was not set in Plex. Before this morning IBM Plex Sans was not installed on the Mac, and CoreText resolved the mockup's <code>'IBM Plex Sans', 'Helvetica Neue', Arial</code> stack to <em>Helvetica Neue</em>: a different face with different stem geometry and a heavier colour at these sizes. So the earlier "day and night" was face plus renderer together. With Plex now in Font Book the browser mockup is the fair reference, and this page renders the same Plex files through CoreText directly so the comparison holds without the browser in between.</p>
<p class="aside">Everything measured below is CoreText on this Mac writing into an sRGB bitmap with the same options a window gets: font smoothing on or off, subpixel positioning on or off. Chromium and Safari both draw through CoreText, so what you see in a browser is one of these four.</p>
""")

sec.append("<h2>2. The as-built against the Mac, at 2.0</h2>"
           "<p>Three specimens: what halcyond paints today, what macOS paints with its defaults, and the candidate that reproduces the Mac's recipe on our own substrate. The strips are one device pixel per pixel; the crops magnify the heading's <em>Terminal</em> and the prose's <em>namespaces</em>.</p>"
           + grid([card("2x", "A-asbuilt"), card("2x", "CT-smooth-subpx"), card("2x", "N0-skrifa-srgb-subpx-stroke015", mark=True, note="the candidate")], cols=3))

sec.append("""
<h2>3. What the Mac actually does — measured</h2>
<p>The probe glyphs below isolate the stroke. For each render the middle band of rows is measured: <strong>weight</strong> is the mean ink per row in CIE L*, <strong>fringe ink</strong> the mean darkness of the partial pixels (the anti-aliasing fringe, 0–1), <strong>fringe px</strong> how many of them a row carries. A light, wide fringe hanging off a stem is what reads as an icicle; a dark, narrow one reads as the stem's edge.</p>
<ul>
<li><strong>CoreText without smoothing measures the same as our as-built.</strong> Weight 5.49 against 5.45 on the italic n, fringe 0.49 against 0.48. Quartz rasterizes exact coverage and blends it in the encoded (gamma) space, exactly as fontdue plus <code>cartoon::blend</code> do. There is no gamma-correct blending to copy.</li>
<li><strong>Its smoothing adds about 18 % stem weight at both scales</strong> (6.45 against 5.49 at 2.0; 3.18 against 2.71 at 1.0) with the fringe count nearly unchanged (3.4 → 3.8 px). A constant relative gain across sizes means the dilation is proportional to the em, not a fixed pixel amount. Fitting our outline stroke to it lands at ≈ 0.015 em (N0: 6.38).</li>
<li><strong>Subpixel positioning</strong> changes nothing on a single glyph (the quantized and positioned CoreText probes are identical); it changes word spacing, which section 5 shows.</li>
<li><strong>Linear-light blending goes the other way.</strong> It lightens every partial pixel (fringe 0.48 → 0.35, weight −12 %): the classic "gamma-correct text looks thin" result. The Mac does not do it; FreeType and Skia stacks that do it compensate with a contrast boost.</li>
</ul>
""")
probe_vs = ["A-asbuilt", "CT-nosmooth-subpx", "CT-smooth-subpx", "N0-skrifa-srgb-subpx-stroke015", "B-gamma", "A2-asbuilt-embolden", "F2-skrifa-light-srgb-subpx", "I-ft-light-srgb-int"]
sec.append("<h3>Italic n, 35 px (the glyph you magnified)</h3>" + probe_row("2x", "probe-italic-n", probe_vs))
sec.append("<h3>Roman y, 23 px</h3>" + probe_row("2x", "probe-roman-y", probe_vs))
sec.append("<h3>Italic n, 17.5 px (1.0)</h3>" + probe_row("1x", "probe-italic-n", probe_vs))

sec.append("<h2>4. Axis: the blend</h2>"
           "<p>Same rasterizer, same whole-pixel pen; only the space the lerp happens in changes. Encoded sRGB is the as-built and the Mac; linear light is what a physically-correct compositor would do; gamma 1.45 is the historical compromise.</p>"
           + grid([card("2x", "A-asbuilt"), card("2x", "B2-gamma145"), card("2x", "B-gamma")], cols=3))

sec.append("<h2>5. Axis: positioning</h2>"
           "<p>Whole-pixel advances put every glyph on the grid and let the rounding accumulate into uneven word spacing; exact placement keeps the designed spacing and pays for it with more rasters per glyph (four at quarter-pixel steps). CoreText's own pair shows the same difference.</p>"
           + grid([card("2x", "A-asbuilt", crops=("prose",)), card("2x", "E2-skrifa-unhinted-srgb-subpx", crops=("prose",)), card("2x", "CT-nosmooth-quant", crops=("prose",)), card("2x", "CT-nosmooth-subpx", crops=("prose",))], cols=2))

sec.append("<h2>6. Axis: weight</h2>"
           "<p>The Mac's smoothing is an outline dilation. Two ways to get it: stroke the outline (needs an outline rasterizer; exact, isotropic, em-relative) or embolden the coverage mask (fontdue has no outline API; edges move but corners and diagonals are approximate). The bracket 0.015–0.040 em spans the Mac's amount and beyond it.</p>"
           + grid([card("2x", "CT-nosmooth-subpx"), card("2x", "CT-smooth-subpx"), card("2x", "N0-skrifa-srgb-subpx-stroke015", mark=True), card("2x", "N1-skrifa-srgb-subpx-stroke020"), card("2x", "N2-skrifa-srgb-subpx-stroke030"), card("2x", "N3-skrifa-srgb-subpx-stroke040"), card("2x", "A2-asbuilt-embolden"), card("2x", "D2-srgb-subpx-embolden"), card("2x", "N5-skrifa-gamma-subpx-stroke030"), card("2x", "N6-skrifa-gamma145-subpx-stroke020")], cols=2))

sec.append("<h2>7. Axis: hinting</h2>"
           "<p>macOS hints nothing on a Retina display. Linux desktops run FreeType's LIGHT autohinter (vertical snapping only); Windows runs the TrueType interpreter in a similar vertical-only mode. The skrifa autohinter is FreeType's, ported. Vertical snapping crisps the x-height and baseline at 1.0 and does nothing for slanted stems.</p>"
           + grid([card("2x", "E2-skrifa-unhinted-srgb-subpx"), card("2x", "F2-skrifa-light-srgb-subpx"), card("2x", "H-skrifa-interp-gamma-int"), card("2x", "I-ft-light-srgb-int"), card("2x", "L-ft-normal-gamma-int"), card("2x", "K-ft-lightdark-gamma-subpx")], cols=3))

sec.append("<h2>8. At 1.0</h2>"
           "<p>The 100% display is where hinting and weight decisions bite: 11.5 px body, 17.5 px heading. Strips are shown at a 100% display's pixel size, crops at 8×.</p>"
           + grid([card("1x", "A-asbuilt"), card("1x", "CT-smooth-subpx"), card("1x", "N0-skrifa-srgb-subpx-stroke015", mark=True), card("1x", "CT-nosmooth-subpx"), card("1x", "F2-skrifa-light-srgb-subpx"), card("1x", "N4-skrifa-light-srgb-subpx-stroke030"), card("1x", "I-ft-light-srgb-int"), card("1x", "B-gamma")], cols=2))

EXTRA = sys.argv[3] if len(sys.argv) > 3 else None
if EXTRA:
    def ximg(name, alt):
        p = os.path.join(EXTRA, name)
        if p not in embedded:
            embedded[p] = data_uri(p)
        return f'<img src="{embedded[p]}" data-devpx="crop" class="crop__img" alt="{html.escape(alt)}" style="max-width:100%">'
    sec.append("""
<h2>8b. Your capture, explained</h2>
<p>The <code>n.png</code> you captured from the Mac's heading is Safari's 35 px raster with <strong>every other device pixel dropped</strong>. Left: the capture's blocks (its grid is 16 image pixels per block). Middle: the same glyph rendered through WebKit here, decimated at one parity — RMS ink error 0.019 against your blocks; the other parities, a 2 × 2 average and a 1× rendering all miss by 0.18 to 0.34. Right: the full 2× raster the panel actually shows, one device pixel per cell. Whatever magnifier made the capture shows one pixel per point; the crisp two-pixel stems are the anti-aliased raster sampled at half resolution, and the same view produced the earlier Thylacine capture.</p>
""" + ximg("capture-explained.png", "capture blocks | Safari decimated | Safari full raster") + """
<h2>8c. The fit</h2>
<p>Your suggestion, built: a random search with local refinement over hinting, an em-relative outline stroke, the blend space, a coverage curve and the fractional pen, scored by RMS ink error against WebKit's own isolated <em>n</em> at the best alignment. Each triptych is target | best fit | difference (red: the fit is darker, blue: the target is).</p>
<div class="kv">
<dt>35 px, free fit</dt><dd>RMS 0.040 — a coverage curve k ≈ 0.4 on a blend just below encoded space, stroke 0. The as-built scores 0.109, the 0.015 em stroke 0.049.</dd>
<dt>17.5 px, free fit</dt><dd>RMS 0.021 — stroke ≈ 0.004 em, curve k ≈ 0.95, blend exponent 0.68. The as-built scores 0.055, the 0.015 em stroke 0.031.</dd>
<dt>one constant, as-built blend</dt><dd>stroke 0.012 em: 0.046 / 0.027. Curve k = 0.55: 0.045 / 0.030. Either reproduces the Mac to the residual floor; the stroke's constant is the size-invariant one.</dd>
</div>
<div class="grid" style="--cols:2">
<figure class="card"><figcaption><span class="tag">35</span> <span class="vdesc">Safari's n at 35 px · the free fit</span></figcaption>""" + ximg("fit-iso-best.png", "35 px free fit") + """</figure>
<figure class="card"><figcaption><span class="tag">35</span> <span class="vdesc">the same target · the as-built pipeline at its best phase</span></figcaption>""" + ximg("fit-iso-asbuilt.png", "35 px as-built") + """</figure>
<figure class="card"><figcaption><span class="tag">17.5</span> <span class="vdesc">Safari's n at 17.5 px · the free fit</span></figcaption>""" + ximg("fit-half-best.png", "17.5 px free fit") + """</figure>
<figure class="card"><figcaption><span class="tag">17.5</span> <span class="vdesc">the same target · the as-built pipeline at its best phase</span></figcaption>""" + ximg("fit-half-asbuilt.png", "17.5 px as-built") + """</figure>
<figure class="card"><figcaption><span class="tag">35</span> <span class="vdesc">stroke 0.012 em on the as-built blend (one constant)</span></figcaption>""" + ximg("fit-s12-35-best.png", "35 px stroke 0.012") + """</figure>
<figure class="card"><figcaption><span class="tag">17.5</span> <span class="vdesc">stroke 0.012 em on the as-built blend (one constant)</span></figcaption>""" + ximg("fit-s12-17.5-best.png", "17.5 px stroke 0.012") + """</figure>
</div>
""")

all_variants = sorted({k[2] for k in index}, key=lambda v: (v.startswith("CT"), v))
sec.append("<h2>9. The numbers</h2>"
           "<p>Every variant on the two probes at both scales. Weight is mean row ink in L* (higher = darker stroke); fringe ink is the partial pixels' mean darkness (0–1); fringe px their count per row.</p>"
           '<div class="tablewrap">' + table(all_variants, [("2x", "probe-italic-n", "italic n · 35 px"), ("2x", "probe-roman-y", "roman y · 23 px"), ("1x", "probe-italic-n", "italic n · 17.5 px"), ("1x", "probe-roman-y", "roman y · 11.5 px")]) + '</div>')

sec.append("""
<h2>10. Reading</h2>
<ul>
<li><strong>The blend stays where it is.</strong> Our sRGB-space lerp is the Mac's; switching to linear light moves the text away from the reference and would need a contrast hack to come back. Fork closed by measurement.</li>
<li><strong>The Mac's comfort is weight and placement, not a different anti-aliasing.</strong> An em-relative outline dilation of ≈ 0.015 em reproduces the smoothed weight within 1 %; exact horizontal placement reproduces its spacing. Both are absent today.</li>
<li><strong>The dilation wants an outline.</strong> Stroking the outline is exact and isotropic; emboldening fontdue's mask is the fallback and shows its seams at corners. skrifa + zeno (the fontations stack: <code>no_std</code> with <code>libm</code>, the autohinter included, variable fonts, the same rasterizer family Chromium and Fuchsia are moving to) gives the outline, the hinting lever, and exact placement in one dependency change.</li>
<li><strong>Hinting is a lever, not a default.</strong> No hinting is the Mac stance and the candidate's; vertical-only autohinting is available for 100% displays if the 1.0 strips argue for it. Your call from section 8.</li>
<li><strong>Cornucopia follows the same rule at bake time</strong>: the mono cells are pre-rasterized unhinted at 4× supersampling, so the em-relative dilation applies in the bake, keeping the islands' colour matched to the prose.</li>
</ul>
<p>The design that follows from this lives in <code>docs/HALCYON-TYPE.md</code>, for the vote.</p>
""")

page = f"""<title>Halcyon Type Lab</title>
<style>{CSS}</style>
<main>
{''.join(sec)}
</main>
<script>{JS}
document.getElementById('dpr').textContent = String(window.devicePixelRatio || 1);
</script>
"""
with open(PAGE, "w") as f:
    f.write(page)
print(f"wrote {PAGE}: {os.path.getsize(PAGE) / 1e6:.2f} MB, {len(embedded)} images embedded")
