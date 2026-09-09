# HALCYON-COMPOSITION — Text flow, spacing, and scale

Implementation guide for `libhalcyon::theme` and the transcript layout code.
This is not new policy — every value below traces back to something already
adopted in HALCYON-VISUAL.md §7/§8 and HALCYON.md §3/§13.5. What this
document adds is the thing neither of those is: a worked composition an
implementing agent can lay text out against directly, and the scale/DPI
arithmetic neither document specifies.

**Companion documents**: HALCYON-VISUAL.md (token source — read first if a
value here looks wrong; that document wins). HALCYON.md §13.5 (the metrics
mixing rule this document makes computable). BEACON.md §3 (the `hdr`/`em`
vocabulary being composed).

---

## 1. Reference scale and DPI

All sizes in HALCYON-VISUAL.md and in this document are **logical pixels at
a 96 DPI reference** — the same convention CSS, X11, and Windows all use.
Halcyon does not get DPI scaling for free the way a normal desktop toolkit
client does: `halcyond` rasterizes its own glyphs (HALCYON.md §3, the
glyph-atlas model), so it is responsible for the logical-to-physical
conversion itself, at every size, on every output.

    physical_px = round(logical_px × scale)
    scale       = output_DPI / 96

**Getting `output_DPI`.** Query the compositor output's reported physical
size (DRM connector `mm_width`/`mm_height`, or the Wayland `wl_output`
equivalent) against its pixel resolution at compositor start and again on
every hotplug/mode-change event:

    DPI = pixel_width / (physical_width_mm / 25.4)

**Snap, don't float.** Round the resulting scale to the nearest 0.25 step
(1.0, 1.25, 1.5, 1.75, 2.0, ...) rather than rendering at the raw continuous
value. The live TTF rasterizer (HALCYON.md §3) can technically produce glyphs
at any size, so nothing forces this — but 1.83× and 2.00× read identically
at arm's length, and an unsnapped scale only buys inconsistent rounding
between sessions and screenshots taken for review. Snap once, at DPI-query
time, and treat the snapped value as `scale` for everything downstream.

**Rounding.** Round half up, to the nearest whole physical pixel, uniformly.
Don't mix rounding strategies across elements sharing a line — a heading and
its trailing chrome rounded by different rules will drift out of baseline
alignment (§4).

**Hairlines are the one exception.** `--hal-hair` (1px) and the bevel
(`--hal-bevel`, 2px) are structural marks, not glyphs, and must stay crisp:

    hairline_px = max(1, round(1 × scale))
    bevel_px    = max(2, round(2 × scale))

Never let a hairline round to zero, and never anti-alias one to a fractional
physical width — a border that disappears at low scale or blurs at a
fractional one has failed at the one thing it exists to do. Ordinary glyph
text does not need this treatment; sub-pixel glyph positioning is normal and
expected, this rule is for flat-color structural lines only.

---

## 2. The proportional type scale, recapped

Pulled from HALCYON-VISUAL.md §7/§8 for convenience — that document is
authoritative if these ever disagree.

| Element | Face | Weight | Style | Size (logical px) | Line-height |
|---|---|---|---|---|---|
| `hdr level=1` | IBM Plex Sans | Regular (400) | italic | 17.5 | 1.25 |
| `hdr level=2` | IBM Plex Sans | Regular (400) | italic | 14.5 | 1.25 |
| `hdr level=3` | IBM Plex Sans | Regular (400) | italic | 12.5 | 1.25 |
| Prose / `obj` | IBM Plex Sans | Text (450) | roman | 11.5 | 1.5 |
| Tag name | IBM Plex Sans | Text (450) | roman | 10.5 | (20px bar) |
| Pill / trail / path / turnstile / cmd / status bar | IBM Plex Sans | Text (450) | roman | 9.5–10 | (20px bar) |
| `em--emph` | IBM Plex Sans | Text (450) | italic | inherits | inherits |
| `em--strong` | IBM Plex Sans | Bold (700) | roman | inherits | inherits |
| `em--dim` | IBM Plex Sans | Text (450) | roman, `fg_dim` | inherits | inherits |
| `em--code` (inline) | Cornucopia | its one weight | roman | inherits | inherits |
| `pre` (block) | Cornucopia | its one weight | roman | 10 | 1.55 |

Headings run lighter (Regular) than the body they sit above (Text). This is
correct, not a mistake to fix — see HALCYON-VISUAL.md §7's rationale before
"balancing" it by bumping heading weight.

---

## 3. Vertical rhythm

| Transition | Margin |
|---|---|
| Block start → first element | 0 (first-child resets top margin to zero) |
| → `hdr level=1` (non-first-child) | 10px top |
| → `hdr level=2` | 8px top |
| → `hdr level=3` | 6px top |
| Any `hdr` → the content under it | 2px (the heading's own bottom margin) |
| Prose paragraph → prose paragraph | 2px each side (4px total gap) by default |
| → `rule` (`<hr>`, BEACON.md §3) | 8px top, 8px bottom |
| Block → block (`.hal-block`) | 6px |
| Two-column list (e.g. a loaded-systems table) | 4–6px block margin; 28px gap between columns; 3px gap between rows |

**Tightening a related pair.** Two lines that read as one unit — a title's
subtitle line and a stat line under it, say — may close to 0px between them
(set the first line's bottom margin and the second's top margin both to 0)
and take a slightly larger margin (8–10px) before the next unrelated
element. This is a per-instance judgment call, not a token: it says "these
two lines are one thought," which no generic spacing rule can know on its
own.

**The title-page `hdr level=1` pattern.** A heading that opens a splash or
welcome screen — content with nothing above it to read as continuation —
may override the first-child reset with an explicit 10–14px top margin and
`text-align: center`. This is the one sanctioned exception to §3's
first-rule, and it is a local, per-instance override, never a change to the
base `.hal-hdr--1` rule. A heading that titles ordinary content (a `man`
page, a command's output) keeps the default: zero top margin, left-aligned,
first-child.

---

## 4. Baseline alignment for monospace islands

HALCYON.md §13.5 states the rule; this is what it means computationally.

1. At the current logical size, read the proportional face's own vertical
   metrics (hhea/OS-2 ascent, descent, line gap) and compute where its
   baseline sits within the line box.
2. Convert that offset to physical pixels using §1's formula, at the same
   scale as everything else on the line.
3. Position the Cornucopia run so **its** baseline lands on that same
   physical-pixel offset — not on Cornucopia's own natural baseline
   position, which will differ since the two faces have different metrics.
4. The line box's height is set by the proportional face alone. A mono
   island is aligned into that box and, if its own ascent/descent would
   exceed it, clipped rather than allowed to grow the box.

This is "may not stretch the line box" made literal: a `code` span or a
`pre` block never changes the vertical rhythm of the prose around it. Do
this alignment in physical pixels, after scaling, not in logical pixels
before it — rounding twice (once per face, independently) is how baselines
drift by a physical pixel at odd scales.

---

## 5. Worked example: the welcome screen

The canonical composition below is the "Halcyon Terminal of Thylacine OS
1.1" welcome screen (halcyon-daylight-mockups.html, "Welcome screen"
section). Read top to bottom as a sequence of rule applications, not as a
one-off:

1. **`hdr level=1`**, centered, title-page pattern (§3): 17.5px italic
   Regular, 10px top margin (overriding first-child), 2px bottom margin.
   *"Halcyon Terminal of Thylacine OS 1.1"*
2. **Two `prose` lines, `em--dim`, centered, tightened pair** (§3): 11.5px
   Text, `fg_dim`. First line 4px top / 0 bottom; second line 0 top / 8px
   bottom before the next section. *"Booted from Territory 0, aarch64" /
   "512M physical memory · EEVDF, 4 cpus."*
3. **`hdr level=2`**: 14.5px italic Regular, 8px top margin, default
   (non-title-page) placement — this is body content now, not the splash.
   *"Loaded systems"*
4. **Two-column list**: 11.5px Text prose per row, name in `fg`, version in
   `em--dim`; 28px column gap, 3px row gap, 4–6px block margin.
5. **`hdr level=2`**: same rule as step 3. *"Getting started"*
6. **`prose` paragraphs** ordinary spacing (2px/2px), including inline
   `obj` (the listener reference) and inline `em--code` (paths, commands).
7. **`hdr level=3`**: 12.5px italic Regular, 6px top margin, nested under
   the level-2 section above it. *"Keys and clicks"*
8. **`prose` paragraphs**, ordinary spacing, continuing the level-3
   subsection.
9. **`rule`**: 8px top and bottom margin, full width — no heading over it;
   boilerplate doesn't need a section name (§3's own convention, and
   Genera's: the original doesn't header-label its copyright block either).
10. **`prose`, `em--dim`**: the closing license paragraph, 4px top margin.
11. **Trailing prompt**, outside any block: the live input line.

Nothing in this sequence is special-cased beyond what §2/§3 already state,
except step 1 (the title-page override) and the tightened pair in step 2.
Everything else is the default cascade applying itself.

---

## 6. Scale worked example

The same key sizes at three common physical scales, computed per §1
(round half up; hairlines via the `max(1, ...)` rule):

| Element (logical px) | 1.0× (96 DPI) | 1.5× (144 DPI) | 2.0× (192 DPI) |
|---|---|---|---|
| `hdr level=1` (17.5) | 18 | 26 | 35 |
| `hdr level=2` (14.5) | 15 | 22 | 29 |
| `hdr level=3` (12.5) | 13 | 19 | 25 |
| Prose / `obj` (11.5) | 12 | 17 | 23 |
| Tag name (10.5) | 11 | 16 | 21 |
| Chrome minimum (9.5) | 10 | 14 | 19 |
| Tag bar / status bar height (20) | 20 | 30 | 40 |
| Bevel (2) | 2 | 3 | 4 |
| Gap (2) | 2 | 3 | 4 |
| Hairline (1) | 1 | 2 | 2 |

Note the 1.0× column is not simply "unrounded" — `round(17.5 × 1) = 18`, not
17.5 or 17. A physical pixel grid has no fractional pixels even at nominal
scale; the logical values in §2 are exact, their physical realizations are
not, starting at 1×.

---

## 7. Explicitly out of scope here

This document does not cover: font hinting mode or gamma-correction of
anti-aliased glyph edges (a rasterizer-implementation concern, not a
composition one); bidirectional/RTL text flow (not yet a Thylacine target);
variable line-length reflow or wrapping rules for narrow panes (the
transcript's existing `text-overflow`/`white-space` behavior per
halcyon-daylight.css governs until this is revisited); and multi-monitor
mixed-DPI behavior beyond "recompute per output" (moving a pane between
outputs of different scale is a `halcyond` compositor concern, not a text
layout one). Each is a real question; none is answered by making this
document longer.
