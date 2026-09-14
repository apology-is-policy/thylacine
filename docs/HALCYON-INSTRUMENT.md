# HALCYON-INSTRUMENT — the Instrument profile: Carbon Optics and the twelve, on Halcyon as built

**Status: DESIGN (2026-09-14), the operator's rulings recorded in §2; the
residue in §13 is for the operator's word.** This is Thylacine's own reading
of the Astra kit (`docs/halcyon-carbon-handoff/`, committed as shipped at
`75c5b44a`): what the mockup fixes exactly, what the tree already has, what
is new, and in what order it lands. Scripture before code, the
design-conversation pattern; the kit itself stays a record and is never
edited.

**Precedence.** The operator's rulings (§2) > this document > the kit's
`IMPLEMENTATION-SPEC.md` > the frozen mockup CSS/JS (`reference/`) for any
visual detail nobody wrote down. The older Halcyon scripture (`HALCYON-VISUAL`
Daylight, `HALCYON-COMPOSITION`, the Daylight rows of `HALCYON.md` §13.6)
stays binding for the **legacy** profile and for everything non-visual it
decides: process isolation, the seat, the gates, Beacon, bounds, the trusted
path. Where this document amends a visual rule it says which and why (§3).

**Companions.** `HALCYON-THEME` (the file, the loader, the push — extended
here, not replaced), `HALCYON-SCALE` (the percent; every Instrument size
scales through it), `HALCYON-TYPE` (the rasterizer; unchanged), `HALCYON.md`
§14.12/§14.13 (the per-user session compositor and the proportional-live
model — the one ratified rule the mockup collides with, §13.1),
`HALCYON-WORKSPACES` (now IN, §2), `docs/halcyon-carbon-handoff/REQUEST-TO-ASTRA.md`
(the data owed by Astra; nothing in this document is blocked on it except
the goldens of §11).

---

## 1. Ground truth (verified 2026-09-14 against `75c5b44a`; re-verify before building)

The kit was written from documents and the mockup, never from a checkout.
Every claim below was read from the tree.

| Fact | Where | Consequence for Instrument |
|---|---|---|
| The pane tree is N-ary: a container divides its rect **equally** among its foreground children (`rect.w / n`); there is no per-child weight, no ratio, no divider object, no pointer resize anywhere | `pane.rs:1601-1641` (`layout_pane`), no `ratio`/`weight`/drag hit in `pane.rs`, `input.rs`, `server.rs` | Weighted division + a divider track + drag are **new** compositor work (§5.3, §9.2) |
| `Mode::Stacked` shows only its active child; the other children are laid out nowhere (`visible=false`, zero rects); the container carves a glyph-free strip of `tab_strip_h` (5) rows per child that `paint_strips` fills as coloured segments | `pane.rs:1304` (`strip_h`), `:1541-1568`, `server.rs:5998` | The Instrument stack is this container grown: every child keeps a **header rect** of `header_h` while its body is collapsed (§6). HALCYON-VISUAL §3.2 already says every tile contributes its bar; the tree never built the collapsed case (`chrome.rs:24-26`) |
| The chrome ring: iff more than one foreground leaf is visible, each leaf insets `gaps + bevel + hairline` on all four sides and carves `header_h` (20) off the top as its `tagbar`; a lone leaf is borderless and bar-free | `pane.rs:1461-1504`; the ring painted at `server.rs:5786` (`paint_borders`: floor, four bevel faces, hairline, the live key, the cast shadow, the resting tag-bar fill) | Instrument replaces the ring with a 1 px frame, an outer pad and a divider track (§5.1); the lone-leaf exemption goes (§5.6) |
| A tag bar is one `Role::Chrome` surface halcyond paints whole and the compositor places at the leaf's `tagbar` rect; it is invisible while the strip is zero or the leaf hidden | `server.rs:4081-4106` (`surface_target`), `chromeset.rs:138-253` | Reused as the header surface; the placement rule gains the collapsed case (§6.3) |
| The compositor routes pointer events to the hosted surface under the pointer (or a placed menu); a chrome strip is never a pointer target | `server.rs:7676` (`ptr_hit` = `layout.surface_at`), `:7780` (`ptr_route`) | Header clicks (expand, ×, pills) need pointer routing to chrome surfaces (§9.1) |
| The status bar: `create W H role=status`, renderer- or declared-session-gated, W == display, H == `status_h`, carved off the display bottom; the layout lives above it | `server.rs:4202` (`status_rect`), `:6186` (the carve in `reconcile`), `status.rs` | The bottom rail is this surface at 25; the top rail is a sibling (§8) |
| The theme is one `Theme` of 57 keys (`KEYS`), loaded from TOML by a strict `no_std` subset parser (unknown key/table refused; `THEME_MAX` 64 KiB; names ≤ 64 bytes), resolved system-then-user, pushed by the declared session as a 72-field wire the compositor re-validates | `theme.rs:57,338,538,891,914,995,1004,1211`; `server.rs:16322-16352` | One loader, a second schema (§4.2); the wire grows (§4.5). The kit's 13 stock projections **load in this parser today** (measured) and its sidecars are **refused** (unknown key at line 3) — as designed |
| Geometry is a theme decision (`Theme.metrics`), scaled by the one function `Metrics::at(pct)` both painters call; bevel ≥ 2, hairline ≥ 1 | `theme.rs:127-183` | The Instrument profile carries its own metrics table; `at` gains the new fields (§5.7) |
| halcyond embeds four Plex Sans faces (Text 450, Text Italic, Bold, Italic 400) and the Cornucopia subset; rasterizes live through skrifa + zeno with the theme's smoothing stroke and quarter-pixel phases; kerning is 0 (Plex carries GPOS only, nothing reads it) | `halcyond/src/lib.rs:42-48`, `raster.rs:36-40`, `outline.rs`; HALCYON-TYPE §6 TY-1..TY-4 | Regular 400 / Medium 500 / SemiBold 600 must be vendored (§7.1); kerning is a named parity gap (§7.5) |
| The executor has an opaque `Op::Rect` and an alpha glyph blit; no translucent fill, no blur | `cartoon/src/lib.rs:39-43,350,364` | Two small ops for the effects (§10) |
| The chord plane is Super-reserved; the default table binds arrows / shift-arrows / h / v / t / s / f / q / e / tab / 0 / minus / equal to FocusDir, MoveDir, Split, SetMode, Zoom, Close, TabCycle, ScaleStep, ScaleReset; the table is remappable through the gated `chord` verb | `chords.rs:1-140` | The mockup's Alt bindings map onto existing chords (§9.3); nothing new in the plane |
| The status bar's four slots and their sources: workspaces (1/0), the focused context (name · OSC 7 cwd · `mark k=cmd`), the condition (the pane's `status` record, sage/cinnabar), the clock | `status.rs:52-71`, HALCYON.md §13.6 H-3d | The rails' production content is these facts, in the mockup's positions (§8) |
| The layout file is `halcyon-layout v1`: container modes + active child + per-leaf tags; no weights, no expanded-tile field beyond `active` | `libhalcyon/src/layout.rs:1-60` | v2 adds weights (§5.3); the stack's expanded child is already `active` |
| A `pre` block, the alt screen and the editor render mono; everything else on the normal screen renders proportional-live (operator-ratified 2026-09-06) | HALCYON.md §14.13 | The mockup's mono shell tiles collide with this; §13.1 puts it to the operator |
| The workspace proposal (`HALCYON-WORKSPACES`): live trees per workspace on the existing backgrounding machinery, Super+1..9, a bound of 9, the `layout` header line | `HALCYON-WORKSPACES` §4 | IN (§2); the top rail renders them (§8.1) |

## 2. The operator's rulings (2026-09-14)

Recorded verbatim in intent; each one changes what "exact" means, so they
come before the design.

1. **Cornucopia stays for every mono role.** Astra did not know it exists.
   Plex Sans stays for proportional text. Literal type parity is therefore
   the bar for the sans face only; for mono the bar is the box, the pitch
   and the colour, not the glyph (§7.2).
2. **The prompt is `λ <path> ⊢ <input>`**: the lambda leads, the turnstile
   stays as the delimiter before the user's input (§7.4).
3. **Super (meta) and every current chord stay.** The mockup's Alt bindings
   are not adopted; the footer hints name our chords (§9.3).
4. **Workspaces are IN.** `WORKSPACE 01` is a real, switchable workspace;
   the kit's single-workspace scoping is overridden (§8.1).
5. **Tag-bar pills stay** (Acme's executable tag line, HALCYON-VISUAL §4.1)
   — inside the Instrument header, between the name and the metadata (§6.4).
6. **Header metadata is ours**, as designed: the mockup's `MODIFIED` /
   `RUNNING` / `PASSED · 1.8s` are placeholders; the slot shows the facts
   Halcyon already holds (the trail: the tile's cwd or its program's
   status; the exit word), not a new producer channel (§6.4).
7. **Scrollbars exist first as a position indicator** into the buffer, not
   an interactive control (§7.7; the exact geometry is Astra's item 6,
   provisional here).
8. **The kit is committed for the record; our own interpreted docs follow.**
   This is the first of them.
9. Both profiles stay live: **legacy** (Daylight / Nightjar / Aero, the
   as-built chrome) and **instrument** (this document). A profile is a
   choice, never a rebuild (§4).

## 3. What "exact" means here, and what is amended

Exact = **flat RGB8 values, geometry, ordering, text hierarchy and stable
states at a logical viewport and scale**, measured against the frozen CSS
and Astra's goldens (§11). Three things are deliberately not literal:

- **Mono glyphs** are Cornucopia (ruling 1): the mono roles keep the
  mockup's sizes, line boxes, paddings and colours; the glyph shapes and the
  advance differ. Cornucopia's advance is 0.5 em at a 1000-unit em
  (`cornucopia-subset.ttf`, hmtx), Plex Mono's is 0.6 em, so a 12 px
  terminal line holds more columns here than in the browser. The pts
  `cols` follows our cell, as it does today.
- **Kerning.** The browser kerns Plex Sans through GPOS; our layout does not
  read it (HALCYON-TYPE §6, "kern stays 0"). Until §7.5 lands, prose line
  breaks may differ from the browser's by a word.
- **Effects** (the two glows, the split flash, the modal backdrop blur, the
  translucent overlays) land in their own slice (§10) and are absent, not
  approximated, until then. A missing effect is an incomplete milestone,
  never a reason to loosen a bound.

**Amendments to the legacy visual scripture, in force for the Instrument
profile only** (the kit's IMPLEMENTATION-SPEC §2 table, as ratified):

| Legacy (HALCYON-VISUAL) | Instrument | Where |
|---|---|---|
| Daylight default, paper-light | Carbon Optics default, near-black; 13 themes | §4 |
| Fixed ember `#e07840` shared with Bonfire | The theme's `amber` (Carbon `#C7B98B`); the terminal's Bonfire ground is replaced by the theme's `terminal_bg` | §7.3 |
| Four-face 2 px NNW bevel + inner hairline + floor gap; corner mitres | Flat 1 px `pane_border` frame; 3 px outer pad; 7 px divider track with a 2 px rule and a 5×5 joint; no bevel, no hairline, no shadow | §5.1 |
| 20 px tag bar and status bar, one vertical unit | 32 px header; 34 px top rail; 25 px bottom rail | §5.1, §8 |
| No pane-level focus; the live tile's status-coloured content outline + cast shadow | A neutral focused-pane frame (`focus_neutral`) + a 2×20 amber mark on the focused expanded header; **no** status-coloured outline; status moves to the metadata ink and the bottom rail's condition | §7.3 |
| Name (never truncated) │ pills · trail | Index │ name (ellipsised) │ pills │ metadata │ × | §6.4 |
| Plex Sans Text 450 body; italic Regular headings | Sans 400 body / 500 headings and header names / 600 brand; roman headings | §7.1 |
| Cornucopia for mono, Bonfire inside a terminal | Cornucopia for mono, the theme's terminal colours | §7.2 |
| Prompt `⊢` | `λ path ⊢ input` | §7.4 |
| Tabbed / stacked containers with a 5 px glyph-free strip | The stack shows every tile's 32 px header; tabbed stays a legacy mode | §6 |

Everything else in HALCYON-VISUAL that is not a colour or a metric — the
amber discipline (§1.3's scarcity, kept under a new hue), "content is never
dimmed" (§5.2), two status states (§1.4), monospace means preformatted
output (§7), heading rank by size (§8.1) — stands under Instrument too.

## 4. The model: profile × theme × scale

Three independent axes, each with one owner.

- **The scale** is the compositor's percent (`HALCYON-SCALE`). Every
  Instrument size below is a logical pixel at 100 and goes through the same
  round-half-up (`scale::ipx` / `Metrics::at`); structural marks keep their
  floors (a 1 px frame is `max(1, ipx(1))`, a 2 px rule `max(2, ipx(2))`).
- **The profile** decides geometry, the type map, the effects and the
  painters' state machine. Two exist: `legacy` and `instrument`. A profile
  is a compiled table (`libhalcyon::profile::{LEGACY, INSTRUMENT}`) — the
  kit's `instrument-profile.toml` is the human-readable form of that table,
  a build input at most, never a runtime file a theme could point at
  (nothing a theme names may select geometry, fonts or executable content;
  HALCYON-THEME §2's identity rule extended by one axis).
- **The theme** decides colours (+ the ANSI-16 and the smoothing stroke).
  Thirteen Instrument themes ship; the three legacy themes stay.

### 4.1 Selection, and who wins

| Path | Owner | Content |
|---|---|---|
| built-in | the binary | `legacy` + Daylight — the floor; and `instrument` + Carbon, compiled, the Instrument floor |
| `/lib/halcyon/profile` | the system | one word: `legacy` \| `instrument`; absent = `legacy` until the rollout flips it (§12) |
| `$HOME/lib/halcyon/profile` | the user | the same word, overrides the system's |
| `/lib/halcyon/themes/<id>.toml` | the system gallery | every shipped theme, both schemas (§4.2) |
| `/lib/halcyon/theme.toml`, `$HOME/lib/halcyon/theme.toml` | system / user | the selected theme file, as today (HALCYON-THEME §3.4) |
| `$HOME/lib/halcyon/theme` | the user, written by the picker | one word: the selected gallery id (§9.4) |

Resolution is `(profile, theme)` as a **bundle** (the kit's THEME-CONVERSION
§5, kept): the session resolves its profile, then its theme, validates the
pair (an Instrument theme under the legacy profile is projected, §4.3, and
vice versa), and pushes the resolved bundle to the compositor over the
existing gated `theme` verb (§4.5). A malformed component fails the whole
candidate loudly and the next tier is tried (system, then built-in — the
TH-4a policy); a runtime selection that fails leaves the current bundle in
place and says so. A display with no declared session keeps the system's
bundle.

### 4.2 One loader, two schemas

The theme parser dispatches on `[meta] profile`:

- absent → the 57-key legacy schema (`theme::KEYS`), unchanged;
- `profile = "instrument-v1"` → the Instrument schema below.

An old binary refuses an Instrument file at its third line (`unknown key`,
measured) — loud, never a half-apply, which is the failure posture
HALCYON-THEME §4.2 demands. The gallery stays ONE directory; `halcyon theme
lint` reports the schema it found. The kit's two-directory split was a
workaround for a loader it could not change; we can, so one gallery it is.

```toml
[meta]
schema = 1
profile = "instrument-v1"
id = "carbon"                 # [a-z][a-z0-9_-]{0,31}; the gallery filename and the picker's key
name = "Carbon Optics"        # the legacy name rule: presentable Unicode, <= 64 bytes
color_scheme = "dark"         # dark | light; selects the smoothing default and the ANSI polarity checks

[color]                       # exactly the 35 roles of resolved-tokens.json, #RRGGBB, opaque
desktop = "#050607"
# ... the other 34, as shipped in ui-palettes/<id>.toml ...

[terminal]                    # OUR addition to the kit's sidecar: the designed ANSI-16 (Appendix A)
ansi = ["#0B0D0E", ...]       # exactly 16, unique except the legacy rule ansi[15] == terminal_text

[type]
smooth = 0                    # thousandths of an em; dark 0, light 12 (HALCYON-TYPE 4.2)
```

Every key required; no `base`; unknown keys and tables refused; duplicate
keys refused; file ≤ 16 KiB; no geometry section (geometry is the
profile's). The parser is the existing `no_std` subset (HALCYON-THEME §5)
with one more table; it joins the same format-fuzz corpus.

### 4.3 The two projections (pure functions, host-tested)

`libhalcyon::theme::project_legacy(&InstrumentTheme) -> Theme` is the kit's
mapping (THEME-CONVERSION §3) as code: `floor=desktop`, `surface=open`,
`header=header`, `raised=hover`, `border=structure`, `blank=pane`,
`selection=mix(open, amber, 15%)`, `island_rule=amber_muted`, the four
text steps, the bevel faces derived from `desktop` (16 % / 9 % white,
22 % / 46 % black — a coherent light, not sampled values), `ember=amber`,
`ember_dim=ember_deep=amber_muted`, the two live keys with neutral tints,
the nine syntax roles by semantics (slate=keyword, sage=type,
sand=attribute, moss=number, ash=function, dusk=string, smoke=comment,
fen=success, cinnabar=error), the status four, the terminal pair, and the
LEGACY metrics table. It exists so an Instrument theme renders under the
legacy painters and so `/env/HALCYON_PALETTE` (a legacy-shaped export,
HALCYON-THEME §3.5) keeps working for `nora` and every hosted program.

`project_instrument(&Theme) -> InstrumentTheme` is the reverse (desktop=floor,
pane=blank, open=surface, header=header, hover=raised, structure=border,
separator=border, text=fg, secondary=fg_muted, dim=fg_subtle, body_text=
fg_dim, amber=ember, amber_muted=ember_dim, error=cinnabar.key,
success=sage.key, terminal_path=syntax.slate, rail=status_bg,
pane_border=border, focus_neutral=fg_muted, code_text=syntax.dusk,
code_bg=header, code_body=fg, terminal_*=terminal, dialog_bg=raised,
kbd_bg=header, the nine syntax roles back, lifetime=fg_muted,
punctuation=fg_muted), so Daylight renders under Instrument geometry.

Both are approximations **by construction** and are labelled so in the
lint (`projected from <schema>`); exactness holds only in a theme's native
profile. The kit's shipped `palettes/*.toml` are what `project_legacy`
produces for the 13 and are kept as fixtures for its tests, never
installed.

### 4.4 The resolved bundle, one type

```rust
pub struct Visual {           // what every painter is handed; nothing else reads a constant
    pub profile: Profile,     // Legacy | Instrument
    pub theme: Theme,         // the legacy 57 (native, or projected)
    pub inst: Option<InstrumentTheme>,   // the 35 + ansi + smooth (native, or projected)
    pub metrics: Metrics,     // the profile's table, at the scale
}
```

`Sheet` carries a `Visual` instead of a bare `Theme` (`layout.rs:49`);
`Comp.theme` becomes `Comp.visual`. The TH-2 rule holds: no production
site names `DAYLIGHT`, `CARBON` or a metrics constant; both are
`theme-fixture`-gated.

### 4.5 The wire

`to_wire`/`from_wire` (`theme.rs:914-1050`, 72 fields today) gain the
profile word, the 35 colours, the 16 ANSI entries and the smooth — one
line, every field re-validated at the compositor (`from_wire` re-checks
every bound; the seat is still another process). The exhaustive structural
guard TH-6 introduced (`a_distinct_theme_survives_the_wire`) is extended,
not bypassed: `WIRE_FIELDS` moves once, both endpoints in one commit. The
line stays under 2 KiB.

## 5. Geometry — the exact carve

All values logical pixels at 100 %, origin top-left, rectangles half-open,
border widths **inside** the box they belong to (the CSS is `border-box`).
Corner radius 0 everywhere.

### 5.1 The display

| Element | Value | Note |
|---|---:|---|
| Top rail | 34 | includes its 1 px bottom structure line |
| Bottom rail | 25 | includes its 1 px top structure line |
| Workspace | H − 59 | between the rails |
| Outer pad | 3 each edge | the workspace's padding, independent of the track |
| Divider track | 7 | real layout space between siblings, never an overlay |
| Divider rule | 2 | at offset 2 from the track's leading edge, in `structure` |
| Divider joint | 5 × 5 | at the track's leading corner offset (1,1): 1 px `structure` border, `desktop` fill |
| Pane frame | 1 | `pane_border`; `focus_neutral` on the focused pane |
| Header | 32 | index 32 │ name (flex) │ pills │ metadata (intrinsic) │ action 28; gaps 7 |
| Focus mark | 2 × 20 | at the header's x = 0, y 6..26, only on the focused pane's expanded header |
| Index rule | 1 | the index column's right border, `separator` |
| Tile separator | 1 | a collapsed tile's bottom edge, `separator`; the last tile has none |

A display `(0,0,W,H)` yields the workspace root `(3, 37, W−6, H−65)`. The
rails are always present (the bottom rail at 25 replaces H-3d's 20; the
top rail is new, §8), so the layout is always recomputed on `H − 59`.

### 5.2 Trees, weights and the split arithmetic

The pane tree stays N-ary. A container's children get **weights** (`u16`,
sum-normalised; equal by default) — i3's percentages, the mockup's ratio
being the two-child case. Along the axis:

    extent E, n children, track t = 7
    usable U = E − (n − 1)·t
    ideal_i  = U · w_i / Σw          (rational; keep 1/256 px or better)
    boundary_k = origin + Σ_{i<k} ideal_i + k·t   (k = 0..n), snapped ONCE with round-half-up
    child_k = [boundary_k, boundary_{k+1} − t)   (the last child ends at E)

Adjacent extents are differences of snapped boundaries — never two
independently rounded widths — so children partition the parent exactly
minus the tracks. For two children this is the mockup's flex result
`first = r·(E−7)`, `second = (1−r)·(E−7)` (the CSS sums two percentage
bases with a non-shrinking 7 px divider; proportional shrink lands exactly
there). At 1440 × 900 the root divides 1434 at 0.515: first 734.905, track
737.905..744.905, second 692.095; the right column divides 835 at 0.49:
405.72, track 442.72..449.72, 422.28. Those are the reference positions;
Astra's dumps (§11) settle Chromium's own sub-pixel snap, and one native
snap rule is chosen to match it once, never per element.

**Orientation.** The mockup's `vertical` split has a vertical divider —
first child LEFT, second RIGHT — which is our `splith`; its `horizontal`
is our `splitv`. The adapter is tested on rectangles, never on the words.

**Minima (new, production only).** A pane needs 260 wide and, for a stack
of N tiles with the open tile not last, `2 + 32·N + 1 + 54` tall (frame,
headers, the open header's separator, a 54 px body); a split's minimum is
the recursive sum along its axis plus the tracks, the max across. A split
or a drag that cannot satisfy every minimum is **refused atomically** with
a status message; nothing in the tree changes. A layout restored onto a
smaller display keeps its data and clamps (a pane at its minimum scrolls),
never deletes tiles. The mockup's `0.22..0.78` ratio clamp is the two-child
shadow of this rule and is kept as the drag clamp where it is tighter.

**Bounds (I-32).** Weights are `u16`; depth ≤ `MAX_DEPTH` (32); nodes ≤
`MAX_NODES` (256); a `weight` verb rides the per-pass layout-verb budget.

### 5.3 The layout file: v2

`halcyon-layout v2` = v1 plus `w=<weight>` per child row. The v1 reader
stays; a v1 file loads with equal weights; the writer emits v2 only when a
weight is non-default (a saved tree with equal weights is byte-identical
v1, so old readers keep reading it). A `stacked` container's `active` is
already the expanded tile. Nothing else in the format moves.

### 5.4 The stack's header/body allocation

Inside a pane frame with inner height A, N tiles, open tile k:

    collapsed tile box = 32 (its 1 px bottom separator inside it)
    open tile box      = A − 32·(N − 1)
    open header        = 32; its separator b = 1 unless the open tile is last
    open body          = A − 32·N − b

Headers before and after the open tile keep their stable order; the last
tile has no separator; bodies clip to their rectangles. The CSS clips the
collapsed header's last row under its own border — that is the reference's
border-box accounting, reproduced, not corrected.

### 5.5 Who computes, and where it is published

**tapestryd computes every rectangle once** (`recompute`): the rails'
carve, the tracks and joints, each pane's frame, each tile's header rect
(collapsed or open) and the open body. halcyond consumes them from the
pane tree the way it does today (the §13.7 file-walk): per leaf `tagbar`
(now the header rect, also for a collapsed leaf whose content is zero),
`geometry` (the body), a new per-leaf `frame` (the enclosing pane frame
rect — the stack's, for a stacked leaf) and a new per-container `dividers`
(one `x y w h` per track). Paint, hit-test, input, winsize and the
goldens' reader all read those; no painter subtracts a second header or a
legacy ring.

### 5.6 The lone tile

One tile on a display is still a stack of one inside a frame under two
rails. The legacy "single fullscreen leaf is borderless and bar-free"
gate (`pane.rs:1462`) is profile-conditional: off under Instrument. An
application fullscreen (zoom) stays a separate, explicit command and
hides the frame as today.

### 5.7 `Metrics` grows

```rust
pub struct Metrics {
    // legacy (unchanged): bevel, gap, hairline, header_h, status_h, tag_pad_x, tab_strip_h
    // instrument:
    pub rail_h: i32,          // 34
    pub outer_pad: i32,       // 3
    pub track: i32,           // 7
    pub rule: i32,            // 2  (floor 2)
    pub rule_off: i32,        // 2
    pub joint: i32,           // 5
    pub frame: i32,           // 1  (floor 1)
    pub index_w: i32,         // 32
    pub header_gap: i32,      // 7
    pub action_w: i32,        // 28
    pub mark_w: i32,          // 2  (floor 2)
    pub mark_inset_y: i32,    // 6
}
```

`INSTRUMENT_BASE` has `header_h` 32, `status_h` 25, `hairline` 1 (the
separator/index rule) and `bevel`/`gap`/`tab_strip_h` 0; `Metrics::at`
scales the new fields with the same rule and floors. Nothing at 100 moves
for the legacy table (`Metrics::at(100) == METRICS_BASE` stays pinned).

## 6. The stack — one container, every header visible

### 6.1 What it is

The mockup's "pane" is a spatial slot holding an ordered list of tiles,
one expanded. That is `Mode::Stacked` (a container, ordered children, one
`active`) with the collapsed children given a rectangle. Every child stays
a real hosted leaf with its own kaua-term, pts, transcript, selection and
scroll — nothing about a tile's identity moves into the container. The
container owns the frame and the ordered header allocation; only the
active child's body is laid out. **A leaf that is not in a stack renders
as a stack of one**: frame + header + body, so the visual is uniform and
no second container type is invented.

### 6.2 Invariants

1. A visible stack has exactly one expanded tile and one header per tile.
2. Order never changes on expansion: earlier headers above the open body,
   later ones below; the open header is always directly above its body.
3. One globally focused tile; its stack is the focused pane. Every other
   stack keeps its own expanded tile without the mark.
4. Splits form an acyclic tree; children partition the parent minus the
   tracks; no leaf interiors overlap (menus and dialogs are the overlay
   exception, as today).
5. A collapsed body receives no pointer input, contributes no damage and
   ticks no FRAME (the existing dormancy: zero content rect, no CONFIGURE
   fan to its content surface); its process is never killed, restarted or
   starved by the collapse — output keeps draining into its transcript,
   bounded as today (the per-tile up-pipe and its caps).
6. Geometry is computed once (§5.5) and consumed everywhere.
7. Focus, expanded, dirty, last exit, running, hover and keyboard focus are
   independent facts.
8. A tile keeps its scroll anchor, selection and process state across
   focus, expansion, theme, resize and header changes.

### 6.3 The header surface

Each tile's header is its **existing tag-bar surface** (`Role::Chrome`
bound to the leaf), placed at the leaf's header rect — which now exists
while the body is collapsed. `surface_target`'s chrome arm returns the
header rect whenever it is non-empty (today it also demands the leaf be
visible, `server.rs:4102`); the CONFIGURE fan and the structural repaint
reach collapsed headers the same way. No new surface role, no strip-list
surface per stack: the stack's authority over the header GEOMETRY lives in
`recompute`; the header's PIXELS stay halcyond's, whole and opaque (the H-3
vote 1). The compositor's resting fill for a header it has no surface for
is `header`.

A header surface is never hosted, never focusable, never a Direct scanout
candidate (unchanged); it is now a **pointer target** (§9.1). It cannot
take the seat, mint a claim or impersonate a tile: its pane authority is
its owner's session authority over the tile it is bound to, judged per
write as every pane verb is (HALCYON.md §13.6, the trust model).

### 6.4 The header's anatomy

```
 ┃ 02 │ src / renderer.rs   :w  :q     MODIFIED ×
 ^  ^   ^────────────────   ^──────    ^──────  ^
 mark index   name             pills     metadata  action
```

| Region | Width | Face / size | Content |
|---|---|---|---|
| Focus mark | 2 × 20 at x 0, y 6..26 | — | `amber`; only the focused pane's expanded header |
| Index | 32, right rule 1 `separator` | mono 10/1, 500 | the tile's 1-based position, two digits; `dim`, `amber` when focused + expanded |
| Name | flexible, ellipsised | sans 13, 500, +.01 em | the tile's program (`chrome::program_name`), `secondary` collapsed / `text` expanded or hovered |
| Pills | intrinsic, shrink-and-ellipsise before the name does | sans 10, 500 | the tile's commands (HALCYON-VISUAL §4.1; the `pill` mark of HALCYON-WORKSPACES §6): `secondary` on a 1 px `separator` box, the first (active) one `text` on a `structure` box; a click types the command (the H-3c "types the verb" path) |
| Metadata | intrinsic | mono 10/1, 400, +.04 em, uppercase | **our trail**: the shell tile's cwd (`abbrev_home`), a program's status; the exit word when the last command failed. Ink `dim`; `amber` for a modified document (a program-reported dirty fact); `error` overrides when the last exit was non-zero — this is where the sage/cinnabar key lives under Instrument |
| Action | 28 (inner 24 tall) | mono 15 | `×`: hidden on a collapsed idle header, visible on the expanded one or on hover; `dim`, `error` on its own hover, with a 1 px `structure` left rule on hover |

Gaps of 7 between regions. The expanded header's ground is `text` at
1.5 % over `open` (precomputed per theme, §7.3); a collapsed header's is
`header`; hover is `hover` on a collapsed header and does not override the
expanded ground (the CSS's later declaration wins — reproduced).

### 6.5 Close, and the final tile

`×` sends the graceful close through the tile's existing lifecycle (a
`close` on the leaf under the owner's authority); a dirty document or a
foreground job asks first (§9.5). The successor is the tile at the removed
index, else the previous one. A stack's final tile refuses with the status
`FINAL TILE IS PROTECTED`; removing a pane is a separate structural act
(today's `close` of an empty leaf), never `×`.

### 6.6 Migration

Every existing leaf becomes a stack of one visually with no tree change.
An existing `stacked` container is already the model. A `tabbed` container
stays a legacy mode under Instrument (it renders its active child as a
stack of one with a single-row header and its siblings unreachable except
by the cycle chord) until the user converts it (`mode stack`); nothing
silently discards children.

## 7. Type, colour and composition

### 7.1 Faces

| Role | Face | Weight | Vendored today? |
|---|---|---|---|
| Body, rail labels, buttons, pills | IBM Plex Sans | 400 Regular | **no** — `IBMPlexSans-Regular.ttf` (v3.005, same version as the tree's four) |
| Header names, H1/H2, footer strong, picker titles, keywords' weight | IBM Plex Sans | 500 Medium | **no** — `IBMPlexSans-Medium.ttf` |
| Brand (`WORKSPACE`) | IBM Plex Sans | 600 SemiBold | **no** — `IBMPlexSans-SemiBold.ttf` |
| Emphasis (rich text) | IBM Plex Sans | 400 Italic | yes |
| Every mono role | Cornucopia | its one weight | yes (the subset) |

Three files, ~200 KB each, embedded beside the existing four
(`halcyond/src/lib.rs:42-48`; +~600 KB in a 1.98 MB binary — accepted, the
faces are identity and the binary is not size-bound; loading faces from
the pool is a later option, not this arc's). The legacy faces stay for the
legacy profile; nothing is removed. Faces are profile assets, never theme
keys (HALCYON-THEME §2).

### 7.2 The Instrument type map (logical px at 100 %)

| Role | Face | Size / line | Weight / tracking |
|---|---|---|---|
| Rail | Sans | 11 / normal | 400, +.08 em, uppercase |
| Brand | Sans | 11 | 600, +.08 em |
| Rail button | Sans | 10 | 400, +.08 em, uppercase |
| Clock | mono | 11 / 1 | 500 (Cornucopia: its one weight) |
| Footer | mono | 10 / 1 | 500, +.08 em, uppercase |
| Header name | Sans | 13 | 500, +.01 em |
| Index | mono | 10 / 1 | 500 |
| Metadata | mono | 10 / 1 | 400, +.04 em, uppercase |
| Doc path | mono | 10 / 1 | 500, +.09 em, uppercase |
| Body | Sans | 15 / 1.62 (24.3) | 400 |
| H1 | Sans | clamp(23, 2.4 % of the logical display width, 34) / 1.12 | 500, −.025 em |
| H2 | Sans | 17 / 1.3 (22.1) | 500 |
| Inline code | mono | 0.86 × body (12.9) | inherits the body line box (HALCYON-COMPOSITION §4's rule: never grows it) |
| Block code (`pre`) | mono | 12 / 1.65 (19.8) | keywords "500" = the keyword colour only under Cornucopia |
| Terminal | mono | 12 / 1.6 (19.2) | 400 |

Where the mockup asks a mono weight or an italic (lifetimes, comments)
Cornucopia has one weight and no italic: the ROLE keeps its colour and the
slant is synthesised as a 12° shear in the outline path (`outline.rs`, one
transform on the pen) — or, if the operator prefers, left roman (§13.2).
Cornucopia sizes go through the live outline at the cell table
(HALCYON-TYPE TY-4): a 12 px terminal is advance 6 at 100 %, 9 at 150 %,
12 at 200 %, the cell height and baseline the table's.

Smoothing: 0 on dark themes, 12 on the three light ones (`type.smooth`,
HALCYON-TYPE §4.2); phases and the fractional pen unchanged.

### 7.3 Colour: the state matrix and the derived opaques

The 35 roles of `resolved-tokens.json` are the palette; PALETTE-REGISTER.md
lists all 13. The painters' matrix (the kit's §6 as read against the CSS):

| Element / state | Colour |
|---|---|
| Desktop, tracks, joints' fill | `desktop` |
| Pane interior (under the tiles) | `pane` |
| Pane frame | `pane_border`; focused: `focus_neutral` + a 1 px inset line of `text` at 3 % over what it covers |
| Collapsed tile ground | `header`; hovered `hover` |
| Expanded tile ground | `open`; its header `text` at 1.5 % over `open` |
| Separators, the index rule | `separator` |
| Header name | `secondary` collapsed; `text` expanded or hovered |
| Index | `dim`; `amber` when focused + expanded |
| Metadata | `dim`; `amber` dirty; `error` on a failed last command (overrides dirty) |
| Focus mark | `amber`, focused + expanded only |
| × | `dim`; `error` on its hover |
| Divider rule | `structure`; hover `amber_muted`; drag `amber` |
| Rails | `rail` ground, `structure` lines, `secondary` ink, `text` for the brand and the clock, `dim` for hints |
| Rich document | ground `open`; body `body_text`; headings and strong `text`; doc path `amber`; inline code `code_text`; `pre` on `code_bg` with a 2 px `amber_muted` left rule, `code_body` ink and the nine `syntax_*` |
| Terminal view | ground `terminal_bg`; text `terminal_text`; `λ` `amber`; cwd `terminal_path`; typed input `text`; commentary `dim`; success `success`; failure `error`; caret 7 × 14 `amber` block |
| Selection | `amber` at 15 % over `open` (precomputed, painted under the glyphs) + a 1 px `amber_muted` rule under the run |
| Picker | `pane` ground, `structure` frame; heading `dim`; group rows `header`; row hover `hover`; title `text`, subtitle `dim`, check `amber`; each miniature in ITS OWN theme's four colours |
| Help / confirmations | `dialog_bg`, 1 px `focus_neutral`; eyebrow `amber`; keys on `kbd_bg` with a `focus_neutral` frame |

**Derived opaques**, computed once per (theme) at resolve time and carried
in the bundle so no painter blends at paint time where the substrate is
known: `open_header = over(open, text, .015)` (Carbon `#151819`),
`selection = over(open, amber, .15)`, the focus inset over `header` and
over `open`. The inset over a client's body and the effects need real
compositing (§10). Round-nearest RGB8; sRGB lerp, the executor's `blend`.

**Status under Instrument.** The pane's `status` record (`resting|ok|err`,
the H-3b verb) is unchanged as a FACT; its two renderings are the
metadata ink (`error` on `err`) and the bottom rail's condition (§8.2).
The legacy status-coloured outline, tinted bar and cast shadow are not
painted under Instrument (§3).

### 7.4 The prompt

`λ <cwd> ⊢ <input>` — `ut` emits the lambda as the first glyph of its
prompt zone and keeps the turnstile before the editing line. Colours in
the terminal view: `λ` `amber`, the cwd `terminal_path`, `⊢` `secondary`
(one amber glyph per prompt: the discipline; the turnstile is a delimiter,
not a signal — the operator may make it amber, §13.3), input `text`. The
change is in the producer (`libutopia` palette/ansi: the prompt shape,
under `HALCYON_PALETTE`'s existing role export — `Role::Glyph` gains the
lambda; the turnstile keeps its role), never a byte substitution in the
renderer. In the proportional rich presentation the same three glyphs
carry the same roles at the body size.

### 7.5 Composition of a rich document

Body padding: top `clamp(18, 2.2 % of the logical display width, 34)`,
sides `clamp(20, 3 %, 48)`, bottom 50 — **the display's width, not the
pane's** (CSS `vw`); at 1440: 31.68 / 43.2, H1 34. Doc path margin-bottom
28. H1 margin 0/0/14; H2 margin-top 28, bottom 10; paragraphs and lists
carry the UA's 1 em block margins (15 px), **collapsed** between adjacent
blocks (the larger of the two, never the sum) — implement the collapse
explicitly in `layout()`. Widths: H1, p, ul, pre cap at 720 (each block,
not a centred column); H2 is uncapped. Lists: padding-left 20, items
5/0/5 with padding-left 5. `pre`: margin 18/0, padding 15/17, the 2 px
rule inside its box, horizontal overflow scrolls, never wraps. Terminal
lines are `pre-wrap`. These are the Instrument `Sheet` values; the legacy
`Sheet` keeps HALCYON-COMPOSITION's.

**Kerning (the parity gap).** Plex Sans's pair kerning lives in GPOS
PairPos; `read-fonts` (vendored) parses GPOS. A bounded pair-adjustment
reader (format 1 pair sets and format 2 class pairs, the `kern` feature's
lookups only, no shaping) is ~300 lines in `raster.rs` and closes the gap
for Latin text; ligatures stay off in both the browser capture (request
them off, §11) and here. Whether it lands in this arc or after is §13.4.

### 7.6 Presentation: terminal view vs rich document

A tile's content has two presentations under Instrument. **Rich document**
is the proportional flow (HALCYON.md §14.13, the Beacon transcript: prose,
objects, headings, tables, `pre` islands) in the type of §7.2. **Terminal
view** paints the same transcript mono (Cornucopia 12/1.6 on
`terminal_bg`, the mockup's shell tiles), Beacon objects still affordant
(obj runs underlined and coloured, the verb menu on them), `pre` inherent.
The alt screen is the mono grid in both, full-body, as ratified. Which
presentation a shell tile defaults to is the one conflict between the kit
and ratified scripture — §13.1 — and this document does not decide it. The
mechanism is the same either way: a per-tile presentation flag, a header
pill to toggle it, both presentations reading one transcript.

### 7.7 Scrollbar (provisional, pending Astra's item 6)

A passive position indicator at the body's right edge: a 4 px wide thumb
(scaled) in `structure`, inset 2 from the edge, minimum length 24,
proportional to the visible fraction, shown only while the buffer
overflows the body, over rich documents, terminal views and the picker.
No track, no pointer behaviour. The CSS's `scrollbar-color: structure
transparent; scrollbar-width: thin` is the reference; Astra's answer may
replace the numbers.

## 8. The rails

Both are display-level chrome surfaces on the H-3d model: created by the
renderer or the declared hosting session (`role=status` for the bottom
one, as today; `role=rail` for the top one — the same gate, the same
one-per-renderer rule, W == display, H == the profile's unit, E_INVAL
otherwise), carved off the display in `reconcile` (top 34, bottom 25), the
compositor filling `rail` under them from the carve on, `surface_target`
placing them, never hosted, never focusable, pointer-routed like a header
(§9.1) for the top rail's buttons. Under the legacy profile `role=rail` is
refused (no top rail exists there) and `role=status` keeps its 20.

### 8.1 The top rail (34)

Left to right, padding 10 / 8:

- **Brand**: the 13 × 13 mark (`amber_muted` 1 px border, two 1 px `amber`
  strokes at (3,3)-(3,8) and (3,7)-(8,7)) then the workspaces — the
  ruling: `01 02 03` in the brand's 11/600 uppercase, the active one
  `text`, the others `dim`, gap 9; a click switches; the box is 212 wide
  as in the mockup so the context column starts where the mockup's does.
  (The mockup's `WORKSPACE 01` label is the one-workspace rendering of
  this; Astra's item 7 may refine the multi-workspace look.)
- **Context**: `~/systems/ compositor │ src / renderer.rs` = the focused
  tile's cwd (the OSC 7 fact, `abbrev_home`'d, the last component in
  500), a 1 × 12 `structure` separator with 10 px margins, then the
  focused tile's name — H-3d's context slot, moved up. Single line,
  clipped, `secondary` with `text` for the strong fragment.
- **Actions** (auto left margin, gap 2; each 26 tall, padding 9, 10/400
  uppercase, transparent 1 px side borders; hover `hover` ground +
  `structure` borders; pressed ink `amber`): `═ SPLIT H`, `║ SPLIT V`
  (the split chords' pointer twins — new distinct tile in the new pane,
  §9.5), `■ <theme name> ⌄` (the picker, §9.4), `↺ RESET` (§9.5),
  `?` (help, icon-only, min-width 28).
- **Clock**: `HH:MM`, mono 11/500, `text`, padding 10 / 5; UTC as today
  (the RTC's zone; no zone database yet).

### 8.2 The bottom rail (25)

Padding 10 each side, mono 10/500 uppercase:

- **Left, the condition**: a 6 × 6 square + the label — `success` +
  `READY` when the focused tile's last command passed or nothing ran;
  `error` + `EXIT N` when it failed (H-3d's condition slot in the
  mockup's clothes); a transient status message (uppercase, 1800 ms, the
  last message resets the timer; `amber` for an action, `error` for a
  refusal such as `FINAL TILE IS PROTECTED`) replaces the label and
  `READY` returns when it expires. The square's glow is an effect (§10).
- **Centre, the chord hints**: `SUPER + ARROWS  FOCUS · SUPER + TAB
  TILES` — OUR chords (ruling 3), `dim`/`secondary` alternation, gap 8;
  hidden below 820 wide.
- **Right**: the pane count (`3 PANES`), a separator, `LOCAL` (the
  session's host name when one exists; `LOCAL` otherwise).

### 8.3 Narrow displays

At a logical width ≤ 820: the brand shows only its mark and the active
workspace, the context and the button labels hide, the rail gap is 8, the
centre hints hide, the footer type is 9, the picker shifts right by 44
(clamped into the display — a small, recorded safety delta), and the
workspace root keeps a minimum width of 840 with the workspace panning.
At 821 the wide layout returns.

## 9. Input and interaction

### 9.1 Pointer routing to chrome (new)

`ptr_route` gains an arm before the hosted-surface hit: a point inside a
header rect, a divider track or a rail routes to that chrome surface
(header-relative coordinates; the divider to the compositor itself). A
press on a header surface is delivered to its owner (halcyond), which
decides by x: the index or name → `focus <id>` + expand (the container's
`active`, via the pane ctl under its own authority); a pill → type the
command into the tile (the H-3c path); `×` → §6.5. The release follows
its press (the H-3c round F1 rule, unchanged). A click on a body focuses
the tile and still reaches the client (click-to-focus as built).

### 9.2 Dividers (compositor-owned)

Hover paints the rule `amber_muted`; a press captures the pointer for that
split; motion sets the two adjacent weights from the pointer's position
along the axis (the mockup's ratio = position over the FULL extent
including the track, reproduced), clamped by the minima and the
`0.22..0.78` two-child rule; the relayout is coalesced to the frame
cadence and the final position is never dropped; release, cancel, a modal
opening, the split's retirement or logout end the capture. Double-click
restores equal weights. Escape **ends the drag at the current ratio** (the
source's behaviour; a rollback would be a separate, labelled decision).
Keyboard: a divider is not focusable in v1 (no ±0.025 arrow step); the
`weight` verb and a chord may come later. Resizes reach clients through
the existing CONFIGURE / reweave / pts winsize path.

### 9.3 Chords — the mockup's gestures on our plane

| Mockup | Ours (existing) |
|---|---|
| Alt + arrows: focus a neighbour | Super + arrows (`FocusDir`; the compositor's own rule, not the mockup's centre-distance — recorded as a deliberate difference) |
| Alt + J / K: next / previous tile | the cycle chords (`TabCycle`), which walk a stack's children |
| Alt + H / V: split | Super + H / V (`Split`) — the new pane gets a NEW tile (§9.5) |
| Close tile | Super + Q (`Close`) on the focused leaf, with the §6.5 protections |
| Workspace N | Super + 1..9 (HALCYON-WORKSPACES §4, now to be bound; free keys) |
| Theme picker, help, reset | pointer on the rail; chords to be chosen (§13.5) |
| Escape | ends a drag; dismisses a menu / dialog (the compositor's, as built) |

Input priority is unchanged in shape: trusted system chord > modal >
placed menu > divider capture > the Super plane > the focused tile. The
consumed chords never reach a pts (the plane's swallow set).

### 9.4 The theme picker and live switching

The picker is a `Role::Menu` surface halcyond paints and the compositor
places (H-3c's machinery: the grab, click-away and Esc dismiss owned by
the compositor), anchored under the theme control at rail + 5, right
aligned, 286 wide, `min(list, display − 72)` tall and scrolling; its rows
are §7.3's; DARK FIELD / TERMINAL STUDIES / LIGHT FIELD in the mockup's
order, 13 counted from the gallery (the registry decides the count; the
gallery ships 13). Opening focuses the current theme and applies nothing;
arrows wrap, Home/End jump, Enter or Space commits, Escape and click-away
dismiss without change; the toggle regains focus after.

A commit is the existing **visual transaction**: the session stages the
bundle (parse both files, derive the opaques, validate), pushes it over
the gated `theme` verb, the compositor validates independently and
either refuses without mutation or commits and fans (`apply_theme`: the
colour-only fan is already owed on a same-geometry push — the scale
round's F3 rule holds), the session repaints every surface at the new
generation, and only after success writes `$HOME/lib/halcyon/theme` with
the durable-write idiom. A failure keeps the previous check mark and
colours and says why. Hosted programs learn the new palette through the
existing cooperative channel (`/env/HALCYON_PALETTE` for future spawns; a
running `nora` re-reads on its notification); an uncooperative truecolor
program keeps its own pixels and is reported, never respawned. Busy is
retried on the bounded cadence; E_PERM is final.

### 9.5 Production behaviour the mockup only simulates

- **Split**: a new pane gets a NEW tile (a fresh kaua-term running the
  session's shell in the source tile's cwd, through the existing spawn
  path), never a copy; the mockup's catalog-copy is fixture-only.
- **Close** (§6.5) asks before discarding a dirty document or a foreground
  job; the confirmation uses the help dialog's language.
- **Reset** re-equalises weights and re-expands each stack's first tile;
  it never restores a layout file or respawns anything without asking.
- **Scroll**: per-tile anchors survive everything (§6.2 #8); the
  reference's DOM rebuild is a defect, not a behaviour.
- **Limits**: the compositor's caps and the recursive minima, refused
  visibly.
- **Metadata**: authenticated to the owning tile, bounded, sanitised,
  never evaluated.
- **Help**: the modal of §7.3 (min(540, W−32) wide; header ≥ 78 padded
  17/18/15/22; a 190 / rest grid of key rows, gap 18, 10 px vertical
  padding; keys min-width 26 padded 4/6; footer paragraph 15/22/20 at
  1.55); it traps focus, closes on Esc and its ×, restores the invoking
  control, and no key of it reaches a pts. Its rows name OUR chords.
- **Reduced motion**: a static caret and no transient animation are the
  production default until the effects slice; nothing else changes.

## 10. Effects (the last slice)

The source's literal effects, kept exact: divider drag glow
`rgba(213,154,66,.25)` blur 10; split flash `rgba(213,154,66,.04)` fill
with a 1 px `amber` border inset 5 for 250 ms; status pulse glow
`rgba(112,161,124,.25)` blur 8; picker shadow black .32 at (0,20) blur
55; help shadow black .35 at (0,24) blur 80; backdrop `rgb(3,4,4)` at .72
plus a 3 px blur; the swatch's white .12 inset border. They stay amber /
green literals on every theme (the CSS does not tokenise them). Two
executor ops carry them: `Op::RectAlpha { ..., color, alpha }` (straight
alpha over the destination, the existing sRGB `blend`) and
`Op::Glow { rect, color, alpha, radius }` (a separable box-blur mask of
bounded radius ≤ 16 at 100 %, scaled), both clipped and bounded like
`Rect`; the modal backdrop is a bounded downsampled blur of the permitted
scene composited by the compositor (the menu's compose path). The
translucent overlays of §7.3 over client bodies use `RectAlpha`. Motion:
tile expansion 180 ms `cubic-bezier(.2,.8,.2,1)` on the allocated size,
hover 120 ms, body opacity 100 ms after 70 ms, caret 1100 ms `steps(2,
start)` with opacity 0 at 55 % — reduced-motion honoured (§9.5).

## 11. Evidence: the goldens and the gates

- **Oracle.** `docs/halcyon-carbon-handoff/reference/` (frozen; SHA256SUMS)
  and Astra's captures + geometry dumps (REQUEST-TO-ASTRA §2-3): Chromium
  at DPR 1 and 2, fonts confirmed loaded, clock frozen at 09:41, reduced
  motion. Until they arrive, `prototype-offline.html` rendered locally
  with the Mac's Plex Sans is the working reference for sans + geometry
  (its mono is a fallback face; not an oracle for mono).
- **Numerical verdict** (ACCEPTANCE-TESTS §2): opaque interiors exact RGB8
  on ≥ 3 × 3 patches; every structural edge at its snapped boundary and
  thickness; glyph bounding boxes against the pinned faces with a
  per-channel tolerance only inside a tight glyph-edge mask; the small
  features (the mark, one header row, a rule) asserted directly, never
  folded into a percentage. A missing object is a failed setup.
- **Native goldens.** `tools/interactive/ls-gfx-instrument.exp` (a new
  LS-CI scenario on the `--config ci` image with the Instrument lever):
  the 3-pane / 10-tile fixture (`fixtures.json`) at 1280 × 800 and 1440 ×
  900 at 100 and 200; the state matrix S01-S14; the picker cycling all 13
  with PIDs, order, expansion and scroll preserved; the seat's negative
  controls for `role=rail`, `weight` and `theme`; the crash of one
  kaua-term contained to its tile. The pixel reader is `gfx_compose.py`'s,
  extended.
- **Host suites.** libhalcyon: the second schema's mutation suite with a
  positive twin per case, both projections, the wire round trip, the
  weighted split arithmetic (partition, snap, minima, the two-child flex
  identity), the header/body allocation; halcyond: the header list, the
  rails, the rich `Sheet`, margin collapsing, the picker; tapestryd: the
  divider hit-test and capture state machine.
- **Regression lanes**: the legacy profile's gates (ls-halcyon, ls-gfx-*,
  the compose gate at 1.0 and 2.0, Nightjar, Aurora) unchanged — the
  discrimination that Instrument is inert while the profile word says
  `legacy`.

## 12. Slices (each its own commit, status row, tests; rollback = the profile word)

- **I-0 — the oracle and the faces.** Vendor Plex Sans 400/500/600 (OFL,
  from the Mac's v3.005 files, the same version as the tree's); the
  goldens harness against the frozen reference; this document ratified.
  No runtime change.
- **I-1 — the second schema and the bundle.** `InstrumentTheme`, the
  dispatching loader, both projections, the 13 gallery files (Appendix A's
  ANSI included), `Visual`, the wire, `halcyon theme lint` for both.
  Nothing paints differently yet. *Audit-bearing: a new strict parser
  (format-fuzz), the wire.*
- **I-2 — the profile's geometry.** `Metrics` grows; the Instrument carve
  (rails, outer pad, tracks, joints, frame, headers — collapsed included,
  the lone-tile rule); the `frame` / `dividers` files; weights and the
  arithmetic; `halcyon-layout v2`. The legacy carve byte-identical under
  `legacy`. *Audit-bearing: the compositor's geometry + I-32.*
- **I-3 — the stack and the headers.** Collapsed headers placed and fanned;
  the header list (index / name / pills / metadata / ×), the state matrix,
  pointer routing to chrome, expand / close / final-tile, the successor
  rule. *Audit-bearing: pointer routing to chrome, pane authority on header
  actions.*
- **I-4 — the rails.** `role=rail`; the top rail's four zones on our
  facts; the bottom rail; transient status; the narrow branch.
  *Audit-bearing: the gated create + the carve.*
- **I-5 — type and the rich document.** The Instrument `Sheet`, the type
  map, margin collapsing, clamp paddings, the `pre` block, the terminal
  view's colours, `λ … ⊢` in `ut`, the presentation flag and its pill, the
  nine syntax roles in `nora`'s export, the scrollbar indicator.
- **I-6 — dividers and minima.** Capture, drag, double-click, the clamps
  and refusals; winsize through the existing path.
- **I-7 — the picker and live switching.** The menu surface, the
  transaction, persistence, the cooperative repaint.
- **I-8 — effects and motion.** The two ops, the glows, the backdrop, the
  transitions.
- **I-9 — parity gate, audit, rollout.** ACCEPTANCE-TESTS in full against
  Astra's goldens; the Fable round over I-1..I-8 (double-distance batched:
  one round after I-4, one after I-8); `/lib/halcyon/profile` flips to
  `instrument` for fresh images; `legacy` stays selectable for a release.

Rollback at every step is the profile word; a palette rollback never kills
a process; a layout v2 file reads under v1 with equal weights.

## 13. For the operator (the residue the research could not settle)

1. **A shell tile's default presentation** (§7.6). (A) proportional-live
   as ratified 2026-09-06 — the mockup's mono shell tiles are then a
   reference for colours and padding only; (B) the mockup's mono terminal
   view by default, the proportional flow for document tiles and on the
   pill toggle; (C) B's mechanism with A's default. Recommended: **(C)** —
   the mechanism costs the same, the default follows the ratified rule,
   and the toggle gives the mockup's look to anyone who wants it per tile.
2. **Mono italics**: a 12° synthetic shear for lifetimes and comments
   (the browser synthesises too), or roman with the colour alone.
   Recommended: the shear.
3. **The turnstile's ink**: `secondary` (recommended: one amber glyph per
   prompt) or `amber`.
4. **Kerning** (§7.5): in this arc (I-5) or after the parity gate.
   Recommended: I-5, since prose line breaks are part of "exact".
5. **Chord defaults** for the picker, help, close-tile and Super+1..9;
   free keys per `chords.rs`. Proposed: Super+T picker (today `SetMode
   Tabbed` — rebind tabbed to Super+Shift+T), Super+/ help, Super+Q
   close, Super+1..9 workspaces.
6. **Fonts**: vendor 400 / 500 / 600 (recommended; +~600 KB) or keep Text
   450 as the body (a visible deviation).
7. **Contrast**: accept the measured 45 sub-4.5:1 pairs outside Carbon as
   the design (recommended, per Astra's item 8's answer).

## Appendix A — the ANSI-16 tables (designed here, per ruling)

Owed with I-1; generated by `tools/halcyon/instrument-ansi.py` from the
rule below and reviewed by eye per theme before the gallery files land.
The rule, so the tables can be re-derived and checked rather than
trusted:

- Slots 0/7/8/15 are the theme's greys: black = the darkest of
  `desktop`/`pane` on a dark theme and the darkest ink on a light one;
  white = `secondary` (dark) / `dim`-of-the-inverse (light); bright black
  = `dim`; bright white = `terminal_text` (the legacy alias rule).
- Slots 1..6 keep their HUES (red, green, yellow, blue, magenta, cyan) in
  the theme's temperature and saturation, each ≥ 3:1 against
  `terminal_bg`, all sixteen distinct; the bright variants lighter on a
  dark ground and darker on a light one; `red` and `green` agree with
  `error` and `success` where those already carry the hue.
- Where the kit's derived slot is hue-correct it is kept (Carbon's red,
  green, magenta; every theme's red/green); where it is not (yellow = the
  signal hue, blue = the path hue, the light themes' inverted polarity) it
  is redesigned.

The tables themselves land in the gallery files, one per theme, and are
listed in the I-1 commit; this appendix is the rule they are checked
against.
