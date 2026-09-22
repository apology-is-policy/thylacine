# HALCYON-TYPE — Type rendering: the Daylight text on the screen

**Status: RATIFIED 2026-09-08 (the operator, on the fit's numbers; §7
records the answers). Implementation: TY-1 opens.** It answers one
charge, verbatim: *"one general allure of mac is the
fact the OS X has a really REALLY well sorted out typesetting and
anti-aliasing — when you compare it to Linux, it's day and night. In
Thylacine we should strive to achieve similar reading and viewing comfort."*

**Scope**: how a glyph becomes pixels — coverage, blending, weight,
horizontal placement, hinting — for the proportional faces and the mono
islands. Not the faces or sizes (HALCYON-VISUAL §7/§8, the operator's), not
the flow (HALCYON-COMPOSITION), not the percent (HALCYON-SCALE). Kerning
and shaping are named where they touch this and otherwise left where they
are (a known gap, `raster.rs` "kern" comment).

**Companions**: HALCYON.md §3 + §13.5 (the rasterizer stance this amends),
HALCYON-VISUAL §7 (IBM Plex Sans Text 450 / Regular 400 italic; Cornucopia),
HALCYON-SCALE §7 (the atlas bound this must keep), AURORA.md §3 (Cornucopia
"one outline, rasterized two ways").

**The evidence**: the Halcyon Type Lab page,
<https://claude.ai/code/artifact/dda8f9fa-73cf-4e56-a8b4-22e1be51a678> — every
recipe below rendered on the same Daylight text next to CoreText on the same
Plex files, at 2.0 and 1.0, with the numbers. The instrument is
`tools/typelab` (host-only; §2). Read the page first; this document is the
reading of it.

---

## 0. The premise, checked

The comparison that raised the charge was not what it looked like. The
mockup viewed "on Mac in a browser" asks for `'IBM Plex Sans', 'Helvetica
Neue', Arial`, and until 2026-09-08 IBM Plex Sans was **not installed** on
the Mac: CoreText resolved the name to Helvetica (measured with
`CTFontCreateWithName`: family `Helvetica`), so the browser set the mockup
in **Helvetica Neue** — a different face, with a heavier colour and
different stem geometry at 11.5–17.5 px. The "icicles vs. smooth" impression
was face and renderer together. Plex is in Font Book now; the lab renders
the same Plex files through CoreText so the rest of this document compares
renderers only.

The premise that survives: the Mac's text *is* more comfortable than ours
on the same face. §3 says by how much and by what mechanism.

**The capture, explained (2026-09-08, later).** The operator then captured
the heading's `n` as the Mac shows it (`n.png`, a magnified view) and found
its raster "unlike anything in the lab": two-pixel stems with hard edges
and almost no fringe. It is Safari's 35 px raster with **every other
device pixel dropped**. Rendering the operator's own page through WebKit
(`tools/typelab/wk.swift`, the engine Safari uses, at the panel's 2x) gives
a 17 x 19 px `n` with 4 px stems; the capture's block grid (16 image px per
block, found from the flat runs of its profiles) holds a 9 x 10 raster;
decimating Safari's raster at one parity reproduces those blocks with an
RMS ink error of **0.019**, while the other three parities, a 2 x 2 box
average, and a 1x rendering all miss by 0.18-0.34
(`tools/typelab/capture-explained.py`). Whatever magnifier made the capture
shows one device pixel per point. The same view produced the earlier
Thylacine capture, so both sides of the eye's comparison were seen at half
resolution -- the lab's crops, one device pixel each, are the first
un-decimated look at either.

---

## 1. Ground truth — the as-built pipeline

Verified in the tree, not from memory:

| Stage | As built | Where |
|---|---|---|
| Rasterizer | fontdue 0.9.4: exact box-filter coverage of the outline, **no hinting** (fontdue has none), one raster per (face, half-px size, char) | `usr/halcyond/src/raster.rs` (`GlyphSource::glyph`), `usr/halcyond/Cargo.toml` |
| Placement | **whole-pixel pen**: `advance = (m.advance_width + 0.5) as i32`; every glyph origin on the pixel grid ("the MVP pen; subpixel positioning is a stylesheet-era refinement") | `raster.rs` lines 8-9, 289, 393 |
| Blend | `cartoon::blend`: a lerp of the **sRGB-encoded** channels (`na = 256 - a`, `>> 8`), no gamma step | `usr/lib/cartoon/src/lib.rs:311` |
| Weight | none added: the outline's own stems | — |
| Kerning | 0 for every pair (Plex carries GPOS only; fontdue reads the legacy `kern` table) | `raster.rs::kern` |
| Mono | Cornucopia **baked**: `tools/bake-cornucopia.py` flattens the outlines, scanline-fills at 4× supersample, box-filters to 8-bit; no hinting; fixed cells per advance (6x14 … 20x44) | `usr/lib/cornucopia/src/lib.rs` |
| Surface | XRGB8888 scanned out by tapestryd through virtio-gpu; the subpixel geometry of the panel is unknown to the guest | HALCYON.md §2 |
| Bound | the atlas: 16 pages of 512² alpha at the reference display, scaled with the display area; a Latin working set is about one page | `raster.rs` `atlas_pages_for`, HALCYON-SCALE §7 |

So: no hinting, whole-pixel placement, gamma-space blending, no added
weight. Three of those four are exactly what macOS does (§3); the other two
are what it does not share with us.

---

## 2. The lab (what was measured, and how)

`tools/typelab` — host-only research tooling, re-runnable in a few seconds
(the lab), plus `wk.swift` (a WebKit snapshot of any HTML at the panel's
scale, with the device-pixel rectangle of every `n` in a selector -- the
same engine Safari renders with), `fit.rs` (§2, the fit) and
`capture-explained.py` (§0, the observation-model test for a magnified
capture):

- **Producers.** fontdue (the as-built rasterizer, including its 3× raster
  for third-pixel placement); **skrifa 0.46 + zeno 0.3** (fontations
  outlines through a scanline rasterizer: unhinted, the FreeType-derived
  autohinter, or the TrueType interpreter; exact fractional placement;
  outline stroking); **FreeType 2.14** via `ftdump.c` (LIGHT autohint,
  LIGHT + stem darkening, the v40 interpreter, unhinted; masks with
  optional third-pixel outline shifts); **CoreText** via `ct.swift`
  (Quartz compositing into an sRGB bitmap with font smoothing and subpixel
  positioning each on/off; kerning off so the shaper is out of the
  comparison).
- **Compositing**, ours for everything but CoreText: the pen (whole-pixel /
  thirds / exact), the blend (sRGB lerp verbatim from `cartoon`, linear
  light, gamma 1.45), emboldening (mask-domain edge motion; or the outline
  stroke unioned with the fill, `f + s - f·s`).
- **Text.** The welcome screen's heading (Plex Italic 17.5 px), a prose
  line with Cornucopia islands (Plex Text 11.5 px, Cornucopia 12 px em),
  the dim closing line — at 1.0 and 2.0. Daylight's ground and ink.
- **Probes.** One glyph at a whole-pixel origin: italic `n` (the operator's
  own magnified glyph) and `l` at the heading size; roman `y`, `v`, `A`,
  `n` at the body size.
- **Measure**, on the middle band of a probe's rows: **weight** = mean row
  ink in CIE L\*; **fringe ink** = mean darkness of the partial pixels
  (0.05 < ink < 0.95); **fringe px** = partial pixels per row; roughness =
  row-to-row change in ink. A straight stroke has constant true ink per
  row, so roughness is the rendering's own texture.

- **The fit** (`fit.rs`, the operator's suggestion: "a stochastic search
  for the parameter set that matches it"): given a reference raster of one
  glyph, random search plus local refinement over hinting (none / LIGHT /
  interpreter), an em-relative outline stroke, the blend-space exponent, a
  coverage curve `a' = a^k`, and the fractional pen; the objective is the
  RMS ink error at the best integer alignment. Any parameter can be pinned
  (`gamma=1` keeps `cartoon::blend`; `stroke=0` needs no outline), which is
  how a recipe the current pipeline can implement is scored against the
  free optimum. Its references are WebKit's own rasters (`wk.swift`), not
  screenshots.

Two instrument bugs were caught by the pictures before any number was
trusted, and are recorded because the class recurs: zeno stores a
`BottomLeft` mask bottom-up (the skrifa glyphs came out upside down — the
`y` in "Thylacine" read as a `λ`); and a Swift spec parser that trimmed a
run's trailing space collapsed CoreText's runs together. Both are the
"paraphrase that is true of the wrong thing" family.

---

## 3. What the references do — measured, then read

### 3.1 The measurements

Italic `n` at the heading size; roman `y` at the body size. Weight in L\*
row-ink; fringe as (mean darkness × pixels per row).

| Recipe | italic n · 35 px | | roman y · 23 px | | italic n · 17.5 px | |
|---|---|---|---|---|---|---|
| | weight | fringe | weight | fringe | weight | fringe |
| **A** as-built (fontdue, whole-px, sRGB lerp) | 5.45 | 0.48 × 3.4 | 3.69 | 0.51 × 3.0 | 2.68 | 0.42 × 4.0 |
| **CT** CoreText, smoothing OFF | 5.49 | 0.49 × 3.4 | 3.70 | 0.49 × 2.9 | 2.71 | 0.43 × 4.0 |
| **CT** CoreText, smoothing ON (the default) | **6.45** | 0.50 × 3.8 | **4.33** | 0.49 × 3.3 | **3.18** | 0.48 × 3.5 |
| **N0** candidate: skrifa unhinted, exact placement, sRGB lerp, stroke 0.015 em | **6.38** | 0.57 × 4.9 | 4.53 | 0.60 × 3.6 | **3.34** | 0.57 × 4.4 |
| **B** as-built, linear-light blend | 4.78 | 0.35 × 3.4 | 3.05 | 0.43 × 3.4 | 2.03 | 0.31 × 3.3 |
| **B2** as-built, gamma-1.45 blend | 5.16 | 0.41 × 3.3 | — | — | — | — |
| **F2** skrifa autohinter LIGHT, sRGB | 5.43 | 0.47 × 3.6 | 3.73 | 0.50 × 3.4 | 2.68 | 0.47 × 4.0 |
| **I** FreeType LIGHT, whole-px, sRGB (the GTK desktop) | 5.44 | 0.47 × 3.6 | 3.73 | 0.51 × 3.4 | 2.69 | 0.47 × 4.0 |
| **A2** as-built + mask emboldening 0.25 px | 6.00 | 0.49 × 4.9 | 4.29 | 0.54 × 3.6 | 3.16 | 0.49 × 4.8 |

(The full table, every variant × four probes, is §9 of the lab page;
`tools/typelab/out/metrics.tsv` after a run.)

### 3.2 The reading

1. **CoreText without its smoothing measures the same as our as-built** —
   weight within 1 %, fringe the same darkness and width, at both scales.
   Quartz rasterizes exact coverage and blends it in the encoded (gamma)
   space, which is what fontdue plus `cartoon::blend` do. **There is no
   gamma-correct blending to copy.** Linear-light blending (B) lightens
   every partial pixel — weight −12 %, fringe darkness 0.48 → 0.35 — the
   classic "gamma-correct text looks thin"; stacks that do it (Skia,
   FreeType-on-Skia) add a contrast boost to come back.
2. **The Mac's smoothing is an em-relative dilation worth ≈ +18 % stem
   weight** (6.45/5.49 at 35 px, 3.18/2.71 at 17.5 px — the same ratio at
   half the size, so the amount scales with the em, not a fixed pixel
   count) with the fringe count nearly unchanged (3.4 → 3.8 px). An outline
   stroke of **0.015 em** (0.0075 em per side: 0.26 px at 35 px, 0.09 px at
   11.5 px) reproduces the weight within 1 % (N0). It does not reproduce
   the fringe exactly — ours comes out darker and a pixel wider — which is
   the residue between "stroke the outline" and whatever Apple does
   inside; by eye the heading crops are near-identical.
2b. **The fit agrees, and refines the constant.** Against Safari's isolated
   `n` (WebKit, the same Plex file), RMS ink error at the best alignment,
   the blend kept as built (`gamma = 1`):

   | recipe (as-built rasterizer, as-built blend, plus...) | 35 px | 17.5 px |
   |---|---|---|
   | nothing (the as-built) | 0.109 | 0.055 |
   | outline stroke 0.015 em (N0) | 0.049 | 0.031 |
   | outline stroke, best single constant: **0.012 em** | 0.046 | 0.027 |
   | coverage curve `a^k`, k free per size (0.39 / 0.64) | 0.040 | 0.024 |
   | coverage curve, one constant k = 0.55 | 0.045 | 0.030 |
   | stroke 0.012 em + a curve | 0.046 | 0.027 (k ≈ 1: the curve adds nothing) |

   Freeing the blend space too reaches 0.040 / 0.021, no better than the
   curve on the as-built blend; the residual floor (0.02-0.04) is the two
   rasterizers' own difference plus the phase. Two mechanisms reproduce the
   Mac equally well: an **outline stroke of 0.012 em** (one constant for
   both sizes -- the size-invariant one, and the documented Apple
   mechanism) or a **coverage curve** (a lookup on the alpha; its best
   exponent drifts with size, 0.39 at 35 px to 0.64 at 17.5 px, so one
   constant costs a little at one end). No hinting mode improved any fit.
   The stroke needs the outline (§4.5); the curve needs nothing the
   as-built pipeline lacks.
3. **Placement.** A single glyph at a whole-pixel origin is identical with
   subpixel positioning on or off (the quantized and positioned CoreText
   probes measure the same); the difference is in *words* — whole-pixel
   advances round per glyph and the rounding accumulates into uneven
   spacing (lab §5). Exact placement keeps the designed spacing.
4. **Hinting adds no weight** (F2, I ≈ A) and is not what the Mac does; it
   snaps x-height and baseline at 1.0. It is a lever for 100 % displays,
   not the answer to the charge.
5. **"Icicles."** What the magnifier shows on a slanted stem — partial
   pixels forming short vertical runs that step sideways every few rows —
   is the coverage of a 12° edge through a square pixel grid, and CoreText
   shows the same runs at the same places. What makes them read as thorns
   on ours and as an edge on the Mac's is the weight: a bolder stem has a
   full-dark core in every row and the runs hang off something; a thin one
   is half runs. So the fix for the thorns is the dilation, not a different
   filter.

### 3.3 Prior art, on the axes that matter

- **Plan 9 (the heritage).** Text is drawn from cached subfont bitmaps;
  `font(6)` admits 8-bit anti-aliased subfonts, and plan9port's `fontsrv`
  serves TrueType by rendering it with the *host's* rasterizer (CoreText on
  macOS, FreeType under X11) into such subfonts. The heritage answer is
  "cache glyph bitmaps, borrow the platform's rasterizer" — the atlas model
  we already have, minus the borrowed rasterizer, which on our own platform
  is ours to choose.
- **macOS / Quartz** (measured above). No hinting; no LCD subpixel AA since
  Mojave (10.14); grayscale coverage blended in gamma space; "font
  smoothing" = an em-relative dilation on by default; subpixel positioning.
- **Linux / FreeType + cairo**: the autohinter's LIGHT target (vertical
  snapping only), no dilation (the autohinter's stem darkening is off by
  default), historically whole-pixel placement, cairo blending without a
  gamma step, and an LCD filter where the panel's subpixel order is known.
  Variant **I** is that desktop: it measures like our as-built. That is the
  "day and night": the Mac adds weight and exact placement; Linux adds
  vertical hinting. The operator's eye prefers the former.
- **Windows / DirectWrite**: ClearType (LCD) with vertical-only TrueType
  hinting, a gamma + "enhanced contrast" adjustment on the coverage
  (the ClearType tuner), subpixel positioning. Not measured; the
  contrast-enhancement idea is the third way to add weight (curve the
  coverage rather than move the edge) and B2/N6 stand in for it.
- **Skia / Chromium**: rasterizes with the platform (FreeType, CoreText,
  DirectWrite), applies a gamma + contrast preblend LUT to masks, keys its
  glyph cache on quarter-pixel horizontal phases (four rasters per glyph).
  On macOS it honours the system's font-smoothing preference — so a
  browser's text is one of the four CoreText specimens on the lab page.
- **The Rust ecosystem, verified on crates.io 2026-09-08**: **fontations**
  (`read-fonts` 0.43, `skrifa` 0.46 — Google Fonts' stack; `no_std` with
  `libm`; the FreeType autohinter ported; the TrueType interpreter;
  variable fonts), **zeno** 0.3 (path rasterizer, fill + stroke, `no_std`
  with `libm`), **swash** 0.2.10 (scaling + shaping, now built on skrifa +
  zeno; `no_std`), **fontdue** 0.9.4 (what we have: fast, no hinting, no
  outline access, no GPOS), **vello** 0.10 (GPU compute rasterization —
  the far end, §5.7), **parley** / **cosmic-text** (layout stacks, not
  rasterizers).

---

## 4. The design

Five decisions; two are "no change", closed by measurement.

### 4.1 The blend: unchanged

`cartoon::blend` stays a lerp in sRGB. It is what the reference does; a
linear-light blend moves the text away from it and would need a contrast
hack to come back; gamma 1.45 is a half-step in the same wrong direction.
**Fork closed.** (The lane-safety lesson in `cartoon::blend`'s comment
stands untouched.)

### 4.2 Weight: an em-relative outline dilation — the smoothing

Every proportional glyph is rasterized as the union of its fill and a
stroke of its outline of width `type_smooth_em × px` (**0.012 em**, the
fit's single constant across 35 and 17.5 px, §3.2 2b; 0.015 was the
probe-weight estimate; per side 0.006 em), coverage union `f + s − f·s`.
The constant is a **theme token**, not a global: Daylight (dark ink on
light ground) 0.012; a dark-ground theme 0, because gamma-space blending
already fattens light-on-dark text (the same asymmetry the Mac shows
between its appearances). Applies to all four Plex faces; Cornucopia per
§4.5.

**The interim, on the as-built rasterizer.** The fit shows a coverage
curve `a' = a^k` with k = 0.55 lands the same RMS as the stroke at these
sizes (§3.2 2b) and needs no outline: a 256-entry lookup applied to the
glyph alpha at pack time, the `cartoon::blend` untouched. It is the
DirectWrite / Skia family's "enhanced contrast", and it is what TY-2 ships
if TY-1's native build of skrifa + zeno is refused or deferred -- with the
known cost that its one constant is a compromise between sizes where the
stroke's is not.

Why a stroke and not a mask trick: the mask-domain emboldening (A2, D2:
edge motion by a fixed 0.25 px) lands the weight at 2.0 and 1.0 but
approximates corners and diagonals and cannot be em-relative without
resampling; the stroke is exact, isotropic, and free once the rasterizer
has the outline. Why not a coverage curve (DirectWrite-style contrast): it
darkens fringes without moving edges, which is the opposite of what
measured — the Mac's fringe darkness does not change; its edges move.

### 4.3 Placement: quarter-pixel horizontal phases

The pen becomes fractional (f32 in layout; the resolved run carries the
glyph origin's whole part and a 2-bit phase); the atlas key gains the
phase; a glyph is rasterized at the phase's offset (0, ¼, ½, ¾ px). The
vertical stays whole-pixel: baselines are integers by HALCYON-COMPOSITION
§1's rounding rule and vertical phases buy nothing on horizontal stems.
Mono keeps integer cells (a fixed cell has no phase).

**The atlas bound (HALCYON-SCALE §7, I-32's in-process face) holds without
change**: the bound is on painted glyph *area*, and a painted glyph
instance paints exactly one phase, so a screen of text packs the same
pages it packs today; the worst case is the same glyph painted at all four
phases, ≤ 4× the per-glyph store, inside the "twice the display area" slack
by construction. Layout still measures through `advance` without packing.

### 4.4 Hinting: none by default; the vertical-only autohinter as a lever

The candidate is unhinted, as the Mac is. skrifa's autohinter in LIGHT mode
(vertical snapping only; `Target::Smooth { mode: Light }`) is available at
one call site and is exposed as a stylesheet lever `type_hint = none |
light` for 100 % displays. The lab's 1.0 strips (§8 of the page) are the
evidence for the vote; the recommendation is **none** — F2 measures no
darker than A and the operator's stated preference is the Mac's stance.

### 4.5 The rasterizer: skrifa + zeno, replacing fontdue

HALCYON.md §13.5 named "fontdue-class" with "ttf-parser + hand-raster" as
the fallback. This design amends that (proposed for the operator's doc; not
edited here): the rasterizer becomes **skrifa (outlines, hinting) + zeno
(fill, stroke)** because §4.2 needs the outline, §4.4 needs the hinter, and
both are `no_std` with `libm` — the same "VERIFY at vendor time by building
against the native target" condition §13.5 already imposes on fontdue
applies, and is the first chunk (§6, TY-1). fontdue's mask emboldening
(D2) is the named fallback if the native build refuses: the weight lands,
the corners are approximate.

Cornucopia: HALCYON.md §3 already says "the TTF outline serves Halcyon at
arbitrary sizes" while the bake serves Aurora / the trusted sink / Halls.
halcyond today serves the bake for its islands and grid (`raster.rs`
`FACE_MONO`), which cannot carry a per-theme dilation without a second bake
set. The design follows the scripture: halcyond rasterizes Cornucopia
**live from a subset TTF** (the 207 baked codepoints subset with fontTools:
~100 KB against the 10.8 MB full font) at the bake's cell geometry table
(`cell_w`, `cell_h`, `baseline` per advance stay the contract the cells
tier shares), with the same stroke rule and the same box-glyph procedural
path. The bake tool is unchanged for its other consumers. (If the operator
prefers the bake, the alternative is `atlas-N-smooth.bin` siblings: +0.8 MB
in the binary, one dilation for every theme.)

### 4.6 LCD / subpixel anti-aliasing: not applicable

The guest does not know the panel's subpixel order through virtio-gpu, the
compositor scales and composes surfaces (a subpixel-rendered glyph does not
survive scaling), and the reference platform dropped it in 2018. Closed.

### 4.7 The GPU path (Halcyon-on-vk)

Unchanged by this design: glyphs are rasterized on the CPU into alpha pages
and the GPU composites pages. Analytic or compute rasterization (vello)
is a different pipeline for a different day; nothing here forecloses it,
and §4.2–4.4 are properties of the pages, not of who samples them.

---

## 5. What it costs

- **Raster work**: a second pass per glyph (the stroke), ≈ 2× fontdue's
  per-glyph time on first paint; rasters are cached, so a screen costs what
  it costs once per (face, size, phase, char). The lab renders 20 variants
  × 8 samples × 2 scales in 2.4 s on the host, stroke included.
- **Atlas**: ≤ 4× per-glyph store worst case, bound unchanged (§4.3).
- **Binary**: skrifa + read-fonts + zeno replace fontdue + ttf-parser;
  Cornucopia subset ≈ 100 KB against 0.8 MB of baked atlases, if §4.5's
  live route is taken.
- **Vendoring**: three crates under `third_party/rust` with manifest +
  forage registration, as fontdue was (HALCYON.md §13.5).

---

## 6. Chunks (in order; each its own commit + status row)

- **TY-1 + TY-2 — LANDED 2026-09-08 @`db1e4ce9`**, one chunk: the swap
  proved the native build by use, not by a dormant dependency. skrifa
  0.46.2 + read-fonts 0.43.3 + font-types 0.12.4 + zeno 0.3.3 (+ bytemuck)
  vendored `no_std`/`libm`; halcyond checked AND release-linked on
  `aarch64-unknown-none` (§13.5's condition met; the fallback unneeded;
  fontdue and its closure removed, 139 crates still).
  `usr/halcyond/src/outline.rs` is the path (§4.5: the y-negating pen so
  zeno's TopLeft renders upright, fill ∪ stroke as f + s − f·s on the
  explicit union box, the bearing = −placement.top); `GlyphSource` on it
  for the four faces, the mono fallback included. The token is
  `Theme.smooth_mem` — thousandths of an em (12 = 0.012 em), integral so
  the theme stays `Eq` — reaching the source as `Sheet.smooth_mem` →
  `GlyphSource::set_smooth` at every sheet (re)build (four sites; a change
  regens, so no cached raster carries a stale amount). **Measured at the
  swap:** the plain fill's ink is fontdue's within 0.12 % (30146 vs 30182 on
  the 35 px italic `n`); the stroke adds **+18 % ink** there — the lab's Mac
  figure (§3.2) exactly — +17 % on the 11.5 px body `n`, +14 % at 17.5 px,
  +11 % on the bold `n`; the box grows one row (17×19 → 17×20), the bearing
  is unchanged, and every advance and line metric is fontdue's to the pixel
  (pinned as literals: 44 line-metric cells, 12 advance strings, 18
  bearings — a drift fails the swap witness). The §4.2 interim LUT was
  never needed. Kern stays 0 (no pair table is read; Plex is GPOS-only).
- **TY-3** Quarter-pixel phases: the fractional pen in layout, the phase in
  the atlas key, the atlas-bound statement re-verified (the existing
  `a_screen_of_the_largest_heading_at_200_packs_under_the_cap` test extended
  to four phases).
  - **TY-3a — LANDED 2026-09-08 @`c0583fb6`**, the substrate, no
    behaviour change: `Face::raster` takes a phase, `GlyphSource::glyph_at`
    puts it in the cache key (`glyph()` is phase 0 and is byte-identical to
    before), `advance_f` is the fractional advance the sub-pixel pen will
    accumulate, and FACE_MONO refuses a phase (a fixed cell has none, and
    phasing it would blur the grid box glyphs join across). **A zeno
    usage trap, found by an ink-conservation assertion:** `Mask::offset`
    moves the rendered BOUNDS and leaves the path where it was, so at
    dx = ¾ the box slid off the glyph and clipped 15 % of its ink, while at
    dx = ¼ it did nothing at all. `render_offset` translates the path;
    the library's own doc says to set both, and the pair reproduces a
    hand-translated command list to within a coverage level. The bound
    claim in §4.3 is now measured, not argued: a painted instance paints
    exactly one phase, so a phased screen packs the *same* pages as an
    unphased one (asserted equal), and one codepoint at four phases costs
    four entries on one page.
  - **TY-3b — LANDED 2026-09-08 @`abbd7900`**, the pen. `LaidGlyph`
    carries the **whole-pixel step to the next glyph** (not the font
    advance) plus its phase, so the executor's integer accumulation
    reproduces the laid `xs` exactly and nothing downstream changed. The
    pen keeps `pen_x` in whole pixels — every width comparison in the
    builder keeps its meaning — beside a remainder in **1/256 px**. The
    finer unit is deliberate and was measured: quantizing the *pen* to
    quarters compounds up to ⅛ px per glyph, which came to **1.4 px over
    one line of prose** — most of the drift sub-pixel placement exists to
    remove. The phase is a per-glyph decision read off the remainder and
    never fed back, so the coarse four-phase grid costs no accumulated
    error. Spilled glyphs are **re-phased** at a wrap: a phase is relative
    to the pen it was laid at, and carrying one across a line break
    offsets the whole tail. Every pre-measure (`run_width`) and every
    single-style run (`shape_run`, used by the chrome strip, the status
    bar and the menu) now shares that accumulator — measuring one way and
    laying another is how a right-aligned run walks off its edge, which is
    exactly what the kv-list test caught. Mono is untouched at every tier:
    a fixed cell has no phase.
- **TY-4 — LANDED 2026-09-08 @`cecfd1e3`** Cornucopia live: the subset
  TTF, `FACE_MONO` on the outline at the cell table; the cells tier
  untouched. `tools/subset-cornucopia.py` cuts the font to
  `usr/lib/cornucopia/src/cornucopia-subset.ttf` (20112 bytes against
  10.8 MB) and reads its codepoint list **out of `atlas.bin`** rather than
  restating it — a constant copied into two tools is a constant that
  drifts, and taking the set from the artifact the other tier serves makes
  "both tiers carry the same glyphs" true by construction. `GlyphSource`
  holds `mono: Option<Face>` and two `MonoCell`s derived by
  `Face::mono_cell`, which re-computes the bake's own formula in integers;
  `the_derived_cell_table_is_the_baked_one` proves the two agree at all
  eleven baked advances, against the blobs, so the cells-tier contract is
  measured and not asserted. The mono tier now carries the theme's
  smoothing stroke: **+18% ink at the shipping cell**, 13–20% across the
  scale table, against the proportional tier's +18% — one rasterizer, one
  stroke rule. `mono_advances` lost its nearest-smaller-bake step (the
  outline serves any advance); no reachable value moved, and the table is
  pinned as literals. **Measured binary effect**, content-verified: the
  subset is embedded, none of the eleven atlases are — halcyond
  2768200 → 1980872 (−787 KB, −28%), **aurora 1041960 → 394312 (−648 KB,
  −62%)**, aurora's being feature unification, since halcyond's request for
  the `scale` bakes had been forcing them into aurora's binary too.
  In passing: the startup font check moved above the `--session` branch,
  where the `cornucopia::verify_all` it replaces had sat *below* the early
  return and so never guarded the session renderer at all.

  **And it surfaced a defect in the cell geometry itself, which is the
  bake's and not this chunk's.** The cell comes from OS/2 `usWinAscent` /
  `usWinDescent` (889 / 208), but Cornucopia's true ink reaches
  `head.yMax` 978 and `yMin` −220 — 89 units short above, 1.07 px at the
  shipping advance — so **all 26 accented Latin-1 capitals plus ® lose the
  top of their diacritic**. The depth is `ceil(978a/500) − ceil(889a/500)`
  and it GROWS with the cell: one row at 100–150%, **two at 175% and 200%,
  four at the largest bake** — and there is a second, *descender* clip
  (zero at the shipping cell, worst at 150% where it takes 19 glyphs).
  TY-6 F4 caught the first version of this paragraph, and the test under
  it, claiming "exactly one row, never more" from the shipping cell alone. The bake clips the identical row (baked
  'Ã' ink 14011 against the live clipped raster's 13844, 1.2% apart, which
  is scanline-vs-zeno), so Aurora, the kernel trusted sink and Halls have
  rendered it that way since G-4. Fixing it means `head.yMax/yMin` in both
  tools, a cell of 6×15 baseline 12 at advance 6, all eleven atlases
  re-baked and the row pitch chased downstream — its own chunk, tracked as
  `bug-mono-cell-clips-every-accented-capital`. Pinned meanwhile by
  `the_cell_clips_the_diacritics_the_bake_clips`, which fails when the
  geometry is corrected so it cannot be fixed silently.
- **TY-5** The hinting lever (if voted).
- **TY-6** The audit: the atlas bound under phases + stroke (I-32's
  in-process face); a hostile stream cannot make a stroke raster exceed
  bbox + 2·stroke; the untrusted-codepoint path unchanged. The
  AUDIT-TRIGGERS row for halcyond's raster surface gets its item.

---

## 7. The vote

**RATIFIED 2026-09-08 (the operator, after the capture explanation and
the fit):** (1) the outline stroke at **0.012 em** as the mechanism
(the coverage lookup stays the recorded interim, not the plan); (2)
**no hinting**; (3) the amount **per theme** (Daylight 0.012, dark
grounds 0); (4) Cornucopia **live from a subset TTF**; (5) the
HALCYON.md §13.5 amendment is the operator's to apply to their document.
The five items as they were put:

1. **The amount and the mechanism.** The fit's answer is an outline stroke
   of 0.012 em (one constant at both sizes; the lab's N0 at 0.015 is the
   eye's bracket next to it), with the coverage curve k = 0.55 as the
   as-built-pipeline interim. Ratify the fit, or pick by eye from the
   lab's §6 bracket (N1 0.020, N2 0.030, N3 0.040).
2. **Hinting at 1.0.** None (recommended) or the vertical-only autohinter,
   from the lab's §8 strips.
3. **Dark grounds.** The token per theme (Daylight 0.015, dark 0), or one
   value everywhere.
4. **Cornucopia.** Live from a subset TTF (recommended; scripture-aligned)
   or a second smoothed bake set.
5. **The rasterizer amendment** to HALCYON.md §13.5 (skrifa + zeno in place
   of fontdue-class), for the operator to apply to their document.

Kerning through GPOS is not on this ballot; it is the same gap it was
(`raster.rs::kern`), and the shaper choice (swash's, or rustybuzz) can be
made when it is, independent of the rasterizer.

---

## 8. Naming (held)

The smoothing constant wants a name that is not "smoothing" (which
describes what it is not — it adds weight, it does not blur). Held for
the operator: `pelt` (the striped coat: the weight the outline wears) is
the candidate; `type_smooth_em` is the placeholder in this document.
