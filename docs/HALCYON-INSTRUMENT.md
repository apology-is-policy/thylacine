# HALCYON-INSTRUMENT — the Instrument profile: Carbon Optics and the twelve, on Halcyon as built

**Status: DESIGN (2026-09-14; Astra's round-2 amendments folded in the same
day, §3.1), the operator's rulings recorded in §2; the residue in §13 is
for the operator's word.** This is Thylacine's own reading
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
and its answer `docs/halcyon-carbon-handoff/round2/RESPONSE-TO-FABLE.md`
(folded in as §3.1 and §14; the goldens are ours to capture, §11).

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

The afternoon's rulings, on the §13 residue (2026-09-14, after round 2):

10. **A shell tile flows proportionally, and only that** (§13.1, option A):
    HALCYON.md §14.13 stands as the sole presentation; the mockup's mono
    shell tiles are a reference for colours and padding only. No per-tile
    presentation flag, no toggle pill. The mono grid remains the alternate
    screen's (a raw application, §14.7) and `pre` islands stay mono
    inside the flow (§7.6).
11. **Mono faces: Cornucopia Regular + the true Italic; "500" is the
    Regular** (§13.2, §13.8): two subsets embedded, real italics for
    lifetimes and comments, no shear, no SemiBold (§7.1).
12. **The 45 contrast replacements are adopted** for the native target
    (§13.7; §3.1).
13. **Chords** (§13.5): Super+T the picker (tabbed moves to
    Super+Shift+T), Super+/ help, Super+Q close, Super+1..9 workspaces,
    Super+Shift+1..9 move the focused tile (§9.3).
14. **The turnstile is `secondary`** (§13.3): one amber glyph per prompt.
15. **Kerning lands in I-5** (§13.4), with the rich document.
16. The three Plex faces are vendored (§13.6, done at `d3958d68`); the two
    strogg ANSI retunes stand by default (§13.9).

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
| Four-face 2 px NNW bevel + inner hairline + floor gap; corner mitres | Flat 1 px `pane_border` frame; 3 px outer pad; 7 px divider track with a 2 px rule and a 7 × 7 joint (5 + a 1 px border); no bevel, no hairline, no shadow | §5.1 |
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

### 3.1 Round-2 amendments (Astra's reply, 2026-09-14; `round2/RESPONSE-TO-FABLE.md`, committed @`009b9062`)

Round 2 sits at the kit's tier of the precedence and amends the kit where
the two disagree; where it disagrees with THIS document the row says which
wins and why. Every number here was recomputed from the package's own data
before it was adopted (JOURNAL run 46o, "Round 2"): the bundle verified
and its `dist/` compared byte-for-byte, the 45 ratios and the 208 ANSI
contrasts re-derived, the build script re-run to a byte-identical
package, the 13 stock files loaded by the tree's parser.

| Subject | The kit / this document said | Round 2 establishes | Verdict |
|---|---|---|---|
| Provenance | the pinned `074bc564` unverifiable | `instrument-panel.bundle`: complete history (5 commits, 2026-09-13), HEAD `074bc564`, its `dist/*` byte-identical to `reference/` | closed |
| Divider joint | 5 × 5 at (1,1) | `* { box-sizing }` never selects a pseudo-element, so `.divider::after` is 5 px content + a 1 px border = **7 × 7 outer** at (1,1) (`styles.css:406,481`); the divider paints above the panes (z-index 4), so the joint overpaints 1 px of the trailing pane's frame across the track | adopted (§5.1, §5.7) |
| ANSI-16 | the kit's derived arrays; this document: designed here by rule | 13 hand-authored tables: all 208 slots ≥ 3.46:1 against `terminal_bg`, sixteen distinct per theme, bright polarity and per-ramp black/white extremes | **Astra's tables adopted**; our tool becomes the lint (Appendix A, §13.9) |
| Contrast | the 45 pairs < 4.5:1 outside Carbon kept as the design (§13.7) | 45 replacements (12 `dim`, 12 `syntax-number`, 12 `syntax-comment`, 5 `syntax-attribute`, 3 `syntax-function`, 1 `syntax-keyword`), each the old value tinted toward the ink pole until ≥ 4.6:1 against its measured ground (`header` for `dim`, `code_bg` for syntax); Carbon unchanged | adopted for the native target (§13.7); historical captures stay unamended |
| Scrollbar | a provisional 4 px `structure` thumb (§7.7) | the position-indicator contract: lane 8, thumb 3, `dim`, no track, no pointer, overflow-only | adopted (§7.7) |
| Goldens | Astra's captures | none captured (no font bytes, no controlled browser); `capture/capture.mjs`, a 98-scenario matrix, syntax-checked only | the oracle is our own native-mode run of that harness (§11) |
| Fonts | Plex Mono for mono | Cornucopia in EVERY mono role; mono advance, baseline and pitch retuned to Cornucopia's metrics; the browser goldens are a geometric and chromatic reference, never a demand to distort Cornucopia | as ruled; the whole family is on this machine (§7.1) |
| Scale convention | "1440 × 900 at 200 %" | a LOGICAL viewport at a backing factor: 200 % is a 2880 × 1800 framebuffer (`effectiveDpr = scale/100 × baseDpr`) | adopted; identical to HALCYON-SCALE |
| Prompt | `λ path ⊢ input` (ruling 2) | the same, with the inks fixed: λ `amber`, path `terminal_path`, ⊢ `secondary`, input `text`; single spaces; a running command never replaces λ with a status glyph; never a fake prefix over bytes the shell renders | as ruled (§7.4) |
| Chords | Super (ruling 3) | Super; footer labels GENERATED from the registry, never literals | as ruled (§8.2) |
| Workspaces, metadata | rulings 4 and 6 | accepted; the chips of §14.1; header metadata is live data, never `MODIFIED` / `RUNNING` / `PASSED` as literals | as ruled |
| The nine surfaces | not in the kit | one specified design each | adopted with the deltas named (§14) |
| Pseudo boxes, baselines | — | the capture dumps DERIVE pseudo-element boxes (no `getBoundingClientRect`) and report `Range` fragments, never baselines | recorded in §11 |

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
bundle. The system word is baked by `THYLACINE_HALCYON_PROFILE=<legacy|instrument>`
(`tools/build.sh`, I-2; constrained to the two words before it is written);
absent, no file is written and the floor is `legacy`.

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

[terminal]                    # OUR addition to the kit's sidecar (round 2's carry none): the authored ANSI-16 (Appendix A)
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
pub struct Bundle {           // the RESOLVED, scale-free pair: what crosses the wire
    pub profile: Profile,     // Legacy | Instrument
    pub theme: Theme,         // the legacy 57 (native, or projected)
    pub inst: InstrumentTheme,// the 35 + ansi + smooth + polarity (native, or projected)
}
pub struct Visual {           // what every painter is handed; nothing else reads a constant
    pub profile: Profile,
    pub theme: Theme,
    pub inst: InstrumentTheme,
    pub metrics: Metrics,     // the profile's table, at the scale: `Bundle::at(pct)`
}
```

*(As built at I-1: `inst` is never absent — a bundle always carries both
sides, one native and one projected, so the `Option` this section first
proposed was dropped; `Bundle` is the wire's type and `Visual` = `Bundle`
+ the scaled metrics.)* **As built at I-2:** `Bundle::at(pct)` picks the
table by the PROFILE through one free function,
`instrument::metrics_base(profile, &theme)` — the theme's own
`[geometry]` under `legacy`, the compiled `INSTRUMENT_BASE` under
`instrument`, whatever the projected table says — and both painters read
it: `Comp.metrics` is `bundle.at(scale).metrics` in `new`, `apply_theme`
and `apply_scale`; halcyond's `Sheet` carries the `profile` beside its
`theme` and builds its metrics through the same function
(`sheet_for(&theme, profile, scale)`), so a profile flip moves both
painters together or neither. (The sheet keeps the pieces it needs rather
than a whole `Visual`; `inst` joins it with the Instrument `Sheet` of
I-5.) The TH-2 rule holds: no production site names `DAYLIGHT`, `CARBON`
or a metrics constant; both are `theme-fixture`-gated.

### 4.5 The wire

`to_wire`/`from_wire` (`theme.rs`, 72 fields until I-1) take a `Bundle`:
the legacy 72 in their old positions, then the profile word, the colour
scheme, the 35 colours, the 16 ANSI entries, the smooth, and a literal
`end` — **127 fields**. Every field is re-validated at the compositor
(`from_wire` re-checks every bound, the profile word, the scheme and the
ANSI slot rule; the seat is still another process). The terminator is
new at I-1 and closes a hazard the old line carried: a push cut inside its
LAST integer parses as a different valid integer (`12` → `1`), and the
count alone cannot see it — the legacy line's last field was the one-digit
`tab_strip_h`, so its truncation control passed by the digit count. The
exhaustive structural guard TH-6 introduced
(`a_distinct_theme_survives_the_wire`) is extended, not bypassed:
`WIRE_FIELDS` moved once, both endpoints in one commit. The line is
~1 KiB, under the 2 KiB bound.

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
| Divider joint | 7 × 7 | the OUTER box, at the track's leading corner offset (1,1): a 1 px `structure` border around a 5 × 5 `desktop` fill. It spans 1..8 across the 7 px track, so it overpaints 1 px of the trailing pane's frame (the divider paints above the panes) — round 2's correction of the kit's 5 × 5 (`styles.css:406,481`) |
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

**As built at I-2.** The arithmetic is `libhalcyon::carve` (pure,
host-tested). `split_spans` computes every boundary as an exact rational
over the weight sum and snaps it ONCE (`(num + S/2) / S`, round half up);
children are differences of snapped boundaries. The minima are enforced
there as the flex rule: a child whose ideal share falls below its minimum
is frozen at it and the rest re-share the remainder; when the minima
alone exceed the usable extent every child is laid at its minimum from
the origin and the caller clips — a display that small keeps its data and
never drops a tile. The two-child result IS the mockup's `r·(E−7)`
(pinned). The reference boundaries — the root track on columns 738..744
and the right column's on rows 443..449 at 1440 × 900 — are where the
browser's raster put them (the golden PNG's pixels measured, JOURNAL run
46o "I-2"), and the 200 % row doubles to 1476..1489. The minima are
judged BEFORE a split: `Layout::split_fits(slot, mode)` evaluates the
tree's minimum with the leaf hypothetically split (flattened into a
same-mode parent as one more sibling, nested as a fresh two-way container
otherwise) against the padded workspace; the `split` verb refuses with
`ENOMEM` — the pane-table class: the tree cannot grow here — and nothing
changes. The compositor's own placement (`host_for`, a new surface into an
occupied focused leaf) tries the aspect split, then a STACK when that will
not fit (a same-mode split flattens into an existing stack, so the tile
joins it), then refuses like a full table. The two minima ride `Metrics`
(`min_pane_w` 260, `min_body_h` 54; §5.7) so they scale with the rest.
Weights live on the pane (`Pane.weight`): a newcomer to a container takes
the MEAN of its siblings (an equal share, the siblings' ratios untouched —
i3's `con_fix_percent`); a nesting split's container takes the leaf's
weight and the two inside halve; a dissolved container's survivor takes
the container's; a swap moves the weight with the pane. The verb is
`weight <id> <1..65535>` on the pane ctl and in the `layout` file (the
per-pass budget; authority = the parent's subtree, the `mode` rule, since
it re-divides the parent).

### 5.3 The layout file: v2

`halcyon-layout v2` = v1 plus `w=<weight>` per child row. The v1 reader
stays; a v1 file loads with equal weights; the writer emits v2 only when a
weight is non-default (a saved tree with equal weights is byte-identical
v1, so old readers keep reading it). A `stacked` container's `active` is
already the expanded tile. Nothing else in the format moves.

**As built at I-2.** The token ends its row (after `env`); it is refused
under a v1 header, at 0, above 65535 or with anything but digits
(`ParseError::BadWeight`). The compositor's `layout` dump carries
` w=<n>` after the rect when non-default (every older reader reads past
it; the equal-weight dump is byte-identical), and `from_render_text` reads
it, so `layout save` writes v2 exactly when the tree has a weight. The
restore planner emits `skeleton::Op::Weight { target, w }` once the
node exists (a leaf, or the container a nesting split created; a
container that flattened into its parent is not a node and its weight has
nothing to land on — a saved compositor tree never has one) and the
session tool executes it as the `weight` verb. `prune_env` hands a
dissolved container's weight to its survivor, the compositor's own rule.

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

*As built at I-2: `carve::stack_alloc`, pinned against the reference's
three panes (p1: headers at 38 / 70 / 807 / 839, the body 102..806; p2:
38 / 378 / 410, the body 70..377; p3: 451 / 483 / 839, the body
515..838 — the browser's rows).*

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

**As built at I-2.** `Layout::recompute(area, gaps, metrics, profile)`
dispatches to the pre-profile legacy carve, untouched, or to
`recompute_instrument`; the `area` is the compositor's
(`Comp::workspace_area`: the display less a registered bar under legacy;
the space between the two rails under Instrument, ALWAYS, whether or not
anything is registered on them). Per pane: `rect` is the frame's box (a
stacked or tabbed leaf's is its container's), `tagbar` the header — set
for a collapsed tile too, which is not `visible` and whose `content` is
ZERO (dormancy, §6.2 #5) — `content` the open body, `dividers` a split
container's tracks in child order. Every rect is clipped to its parent's.
The files: `pane/<id>/frame` (`x y w h`, the pane's outer rect — under
legacy the ring's outside) and `pane/<id>/dividers` (one `x y w h` per
track; empty for a leaf, a stack, or under legacy), both read-only. The
joint is derived from its track by every painter (at the track's
(`frame`, `frame`) corner, `joint` square), never published.
`surface_target`'s chrome arm no longer demands visibility, so a
collapsed header is a CONFIGURE target. The compositor's Instrument
painter (`paint_instrument`): the two rails (`rail`, each with its
`structure` line facing the workspace), the pad ring and the tracks
(`desktop`), the rule, the joint (a `hairline` `structure` border around
`desktop`), the frames (`pane_border`; `focus_neutral` on the frame
holding the focused leaf), the resting `header` under every header and
`pane` under an empty body; it returns its rects so the focus-only repaint
and the menu heal push exactly those. A zoomed leaf fills the workspace
between the rails, frame-less.

### 5.6 The lone tile

One tile on a display is still a stack of one inside a frame under two
rails. The legacy "single fullscreen leaf is borderless and bar-free"
gate (the legacy carve's `foreground_leaf_count() > 1`,
`recompute_legacy`) is the legacy carve's alone: the Instrument carve has
no such branch (as built at I-2). An
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
    pub joint: i32,           // 7 outer (a 1 px border around a 5 px fill; floor 3)
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

**As built at I-2** (two amendments to the struct above). (1) Two more
fields, `min_pane_w` (260) and `min_body_h` (54), carry §5.2's minima so
they scale with the table (`Metrics` is 21 × i32; the legacy `[geometry]`
registry stays at 7 — no file carries an Instrument mark, the table is the
profile's constant, `instrument::INSTRUMENT_BASE`). (2) The zero-base
rule in `Metrics::at`: a mark whose base is 0 is ABSENT from that table
and stays 0 at every scale rather than being lifted to its floor — so the
Instrument table has no bevel at any scale and the legacy table no rail,
and each table's `at(100)` is the table itself (`INSTRUMENT_BASE.at(100)
== INSTRUMENT_BASE`, pinned). The legacy floors are unchanged in effect:
every production legacy base is at or above its floor already (the
loader's and the wire's bounds), so the floors bit only below 100, which
is not a v1 scale. `Metrics::legacy(bevel, gap, hairline, header_h,
status_h, tag_pad_x, tab_strip_h)` is the one constructor of a legacy
table (the base, the kit's projected table, the wire). The scaled tables
are pinned: at 125 rail 43, status 31, pad 4, track 9, rule 3, joint 9,
frame 1, header 40; at 150 rail 51, status 38, track 11, rule 3, joint
11, frame 2; at 175 rail 60, status 44, track 12, rule 4, joint 12, frame
2; at 200 everything doubled.

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
header rect whenever it is non-empty (it demanded visibility until I-2);
the CONFIGURE fan and the structural repaint
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

**As built at I-3.** A collapsed leaf carries its `tagbar` while hidden,
and `chromeset::reconcile` (halcyond) mints one `Role::Chrome` surface per
leaf whose `tagbar` is non-empty, reading the tree through
`chrome::parse_tree` (the leaf's index in its stack, the stack's size,
whether it is the open tile, whether it is the last). A container TILE
inside a stack (a split carved into a body slot) has the header rect the
compositor publishes and rests on `header`, but no chrome surface yet:
halcyond decorates leaves only — a residue (a stack holding a split shows
a blank header row for it). The compositor fans a collapsed header its
CONFIGURE like any chrome (`visible_chrome` walks `surface_target`).

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

**As built at I-3 (`chrome::header_list`, measured against the golden's
pixels; JOURNAL run 46o "I-3").** The golden adds two facts the table
above does not state: a collapsed header that is not its stack's last
paints a 1 px `separator` as its LAST row (rows 38..68 `header`, row 69
`separator` at 1440 × 900; the last tile's header has none), and an open
tile's box ends with a 1 px `separator` row after its body (rows 806 and
377) — the row `stack_alloc` reserves — which the compositor paints
(`Pane.separator`, `paint_instrument`). The index box's right rule is
column 31 in `separator` on every header; the focus mark is 2 × 20 at
(0, 6) on the focused open header only; the expanded ground `open_header`
is `text` at 1.5 % over `open` through the executor's lerp
(`instrument::Derived`, `over`; Carbon `#151819` reproduced), carried by
the `Visual` and the `Sheet`; hover paints `hover` under a collapsed
header, lights its name `text` and shows its `×`; the `×` under the
pointer is `error` with the 1 px `structure` rule at its box's left edge.
The action box is RESERVED on every header (the metadata ends 35 px from
the right edge whether or not the `×` shows). Since I-5a the type is the
map's: the index and the metadata in the free-running Cornucopia at 10
(the metadata tracked .04 em — an 8-character run 43 wide, the golden's
43.203), the name in Plex Sans 500 at 13 tracked .01 em (cut to fit by the
tracked measure), the `×` in the 400 at 15 (the CSS's face, §7.2); no
pills (no `pill` mark is built; the region is a hook). The metadata is
`chrome::metadata_for`: a retained tile's word (`EXIT n` in `success` /
`error`, `DISCONNECTED`, `CRASHED`), else `RUNNING` in `secondary`
(§14.3), else `EXIT n` in `error` after a failed last command, else the
trail uppercase in `dim` (`amber` when a program reports the document
dirty — no program does yet).

### 6.5 Close, and the final tile

`×` sends the graceful close through the tile's existing lifecycle (a
`close` on the leaf under the owner's authority); a dirty document or a
foreground job asks first (§9.5). The successor is the tile at the removed
index, else the previous one. A stack's final tile refuses with the status
`FINAL TILE IS PROTECTED`; removing a pane is a separate structural act
(today's `close` of an empty leaf), never `×`.

**As built at I-3.** `×` yields `ChromeAction::Close { id, count }` to the
owner; a `count` of one is refused with the transient status `FINAL TILE
IS PROTECTED` (§8.2's notice, shown in the H-3d bar's condition slot until
I-4's rail takes it; 1800 ms) and said (`halcyond: final tile is
protected (pane N)`); otherwise the owner writes `close <id>` — the session
through the `layout` file under its `Session` authority, the console
renderer through the pane's `ctl` as the renderer. The successor rule is
`Layout::close_inner`'s (the removed index, else the previous), and I-3
fixed the index shift the rule exposed: removing a child BEFORE the open
one left `active` naming the next tile over
(`closing_a_stacked_tile_keeps_or_hands_on_the_open_one_by_the_successor_rule`).
Super+Q (the compositor's `Close` chord) stays the structural act: it
closes the focused leaf without the final-tile protection — the only
reading under which a pane holding a retained tile can be removed at all
(a §9.3 delta for the operator: its "with the §6.5 protections" is read as
§9.5's dirty-document / foreground-job ask, I-7's dialog).

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
| Body, rail labels, buttons, pills | IBM Plex Sans | 400 Regular | **yes** (`d3958d68`) — `third_party/ibm-plex/ttf/IBMPlexSans-Regular.ttf`, v3.005 (the tree's four are the same version); **embedded at I-5a** (`halcyond::IBM_PLEX_SANS_REGULAR`, `raster::FACE_SANS`) |
| Header names, H1/H2, footer strong, picker titles, keywords' weight | IBM Plex Sans | 500 Medium | **yes** (`d3958d68`) — `IBMPlexSans-Medium.ttf`; **embedded at I-5a** (`FACE_SANS_MEDIUM`) |
| Brand (`WORKSPACE`) | IBM Plex Sans | 600 SemiBold | **yes** (`d3958d68`) — `IBMPlexSans-SemiBold.ttf`; **embedded at I-5a** (`FACE_SANS_SEMIBOLD`) |
| Emphasis (rich text) | IBM Plex Sans | 400 Italic | yes (embedded) |
| Every mono role, incl. the mockup's "500" ones (index, doc path, clock, footer — ruling 11) | Cornucopia | Regular | yes — the embedded subset, **342 codepoints since I-5a** (`usr/lib/cornucopia/src/cornucopia-subset.ttf`, 26 KB: the bake's 207 read out of the atlas plus λ, ✓, ‹ ›, −, ⌘ and U+2500–257F through the tool's `--extra`; 208 and 20 KB before). Served TWO ways: in the CELL (`FACE_MONO`: the alt screen, `pre` islands, raw output) and FREE-RUNNING at any px (`FACE_MONO_TEXT`: the chrome's 10 / 11 px roles) — §7.2's as-built note |
| Mono italic (lifetimes, comments) | Cornucopia | Italic — the true face (ruling 11) | **yes since I-5a** — `cornucopia-subset-italic.ttf` (28 KB), the same 342 codepoints cut with `--match` against the Regular, which refuses a cut whose cell-bearing tables (upem, the OS/2 Windows pair, the advance of `x`) differ; `FACE_MONO_ITALIC`, a CELL face, and halcyond re-checks the shared cell at startup |

The Cornucopia family on this machine is complete: `~/projects/cornucopia-font/`
(the same bytes as `~/Library/Fonts/cornucopia-*.ttf`; MIT, © the operator),
**v34.6.1**, ten faces — Light, SemiLight, Regular, SemiBold, Bold, each
with a true Italic — 7571 codepoints each, upem 1000, ascender 889 /
descender −170 / line gap 38 (a line pitch of 1.097 em; Plex Sans is
1025 / −275 / 0). Plex Mono is on no machine of ours and matters only to
the harness's historical mode (§11).

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

Where the mockup asks a mono weight, the Regular serves (ruling 11: no
Medium exists and no SemiBold is embedded); where it asks an italic
(lifetimes, comments), the true Cornucopia Italic serves, as a second
subset with the Regular's codepoint list, no shear anywhere. Cornucopia
sizes go through the live outline at the cell table (HALCYON-TYPE TY-4): a
12 px terminal is advance 6 at 100 %, 9 at 150 %, 12 at 200 %, the cell
height and baseline the table's — round 2 confirms the target is
Cornucopia's own metrics, never Plex Mono's stretched onto it.

Smoothing: 0 on dark themes, 12 on the three light ones (`type.smooth`,
HALCYON-TYPE §4.2); phases and the fractional pen unchanged.

**As built at I-5a (the substrate and the chrome; JOURNAL run 46o
"I-5a").** Two KINDS of mono slot in `raster.rs`: the CELL faces
(`FACE_MONO`, `FACE_MONO_ITALIC`) rasterize into the fixed cell the cells
tier shares — the alt screen, `pre` islands, raw output — and the TEXT
face (`FACE_MONO_TEXT`) runs Cornucopia free at any px like a
proportional face: the fractional 0.5 em advance, the four phases, the
font's hhea metrics (9 / 2 / 0 at 10 px), the store's stroke; a codepoint
the subset lacks is served by the Instrument Sans at the same px (the
mirror of the rule that serves the turnstile from the cell). The chrome's
mono roles moved onto it at the table's sizes: the header's index and
metadata (10; the metadata tracked .04 em), the clock (11), the footer
(10; 9 narrow), the chips and `?` (10). The Sans roles moved onto the
three new cuts through the `Sheet`'s role table (`face_body` /
`face_medium` / `face_brand` / `face_hdr` / `face_emph` / `face_strong`
/ `face_mono_text` / `face_mono_italic`, `chrome_mono_px` / `clock_px`;
the legacy column is exactly the constants the legacy painters used
before, so legacy is byte-identical by construction): the brand in 600,
the context's basename, the header's name (13, +.01 em) and the menu's
title in 500, the rail's labels and buttons, the placard and the menu's
items in 400. MEASURED against the golden's DOM boxes at 1440 × 900
(`geometry-styles.json`, `matrix-carbon-1440x900-s100-baseDpr1`): the
clock `09:41` 31.9 (the `#clock` box's 46.906 less its 10 / 5 padding),
`READY` 29.0, `3 PANES` 40.609, an 8-character metadata 43.203 (5.4 a
glyph: Cornucopia's 5 at 10 px plus the .04 em), the context 287.219, the
button labels 41.562 / 40.594 / 87.281 / 33.594 — each reproduced within
a pixel by the pen, where the 12 px island cell had laid the clock at 35
and `READY` at 34.8; under the legacy faces the clock, `READY` and
`MODIFIED` witnesses fail (sabotage-measured), and the labels' ±1 alone
does not separate Text 450 from Regular 400 — the context width and the
sheet-role test do. Two deliberate deviations from the tables above: the
`×` is Sans 15, not "mono 15" (the kit's CSS inherits the header's Plex
Sans and sets no mono family on the action, so the CSS wins over §6.4's
cell); and the `═` / `║` icons and the `⌄` chevron stay DRAWN marks of
the golden's footprint although the subset now carries the first two —
the golden's icons are a browser fallback font's glyphs, 8 wide, where
Cornucopia's box glyph at 10 px is 5. In the cell path the procedural box
glyphs are consulted BEFORE the face, so the subset's U+2500–257F never
enter a cell and the joins stay exact (pinned by byte-equality with
`boxglyph::alpha`). Startup: both owners' guards compare the parsed face
count against `raster::VENDORED_FACES`, derived from the one list — the
literal `4` they carried refused to start on the first seven-face image
(the first gate run measured it; no host test can see a bin's guard).
Binary 2,140,744 → 2,785,000 bytes (+644 KB: the three Plex cuts and the
two subsets, accepted at §7.1). Not yet (I-5b): the document's own type
— body 15 / 1.62, the headings, inline code at 0.86 × body, the italic
in mono runs (`ATTR_ITALIC`, which neither the layout nor the grid reads
today — an SGR 3 is silently roman everywhere), the paddings of §7.5.

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
the transcript: `λ` `amber`, the cwd `terminal_path`, `⊢` `secondary`
(ruling 14: one amber glyph per prompt; the turnstile is a delimiter, not
a signal), input `text`. The
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

A tile's content has ONE presentation under Instrument, as under legacy
(ruling 10): the **rich document** — the proportional flow of HALCYON.md
§14.13 (the Beacon transcript: prose, objects, headings, tables, `pre`
islands) in the type of §7.2. The mockup's mono shell tiles are a
reference for their colours and paddings only: the prompt's inks (§7.4),
`terminal_bg` / `terminal_text` behind and inside a `pre` island and the
alternate screen, the 12/1.6 mono line in a `pre`. The alternate screen is
the mono grid, full-body (§14.7), as ratified. There is no per-tile
presentation flag and no toggle pill; a program that wants the mockup's
look emits preformatted output, which is mono by the standing rule
(HALCYON-VISUAL §7).

### 7.7 The position indicator (round 2 §6; ruling 7)

A static indicator of the scroll position, shown only while the content
overflows its viewport, without fade, drag, click-to-jump, resize cursor or
a focus of its own — wheel and keys scroll the content as they do today; it
is not an accessibility slider (expose the region's scroll position
instead) and never changes ink on focus or hover. Logical px at 100 %,
through `Metrics::at`:

| Surface | Lane (right, reserved on overflow) | Thumb | Right inset | End inset | Min thumb |
|---|---:|---:|---:|---:|---:|
| Rich document body | 8 | 3 | 3 | 4 | 24 |
| Terminal transcript | 8 | 3 | 3 | 4 | 24 |
| Picker list | 8 | 3 | 3 | 4 | 18 |

The lane is reserved INSIDE the content viewport on overflow (text never
sits under the thumb; line breaks are allowed to change by it — an explicit
replacement of the CSS's platform-dependent `scrollbar-width: thin`), keeps
the body's or list's own ground (no drawn track), and the thumb is `dim` at
full opacity (the round-2 value outside Carbon; Carbon `#737A76`),
rectangular, radius 0. With content extent C and viewport V: hidden when
C ≤ V + 0.5; L = max(0, V − 8); T = min(L, max(min, L·V/C)); travel = L −
T; offset = clamp(scroll, 0, C − V); the leading edge = 4 + travel ·
offset / (C − V); both edges snapped through the shared scale helper,
travel never negative, T = L when L < min. Follow-tail puts the thumb's end
exactly at V − 4; an append recomputes from the retained anchor and never
reports "at end" while the reader is in history. A raw full-screen
application owns its grid and gets no indicator (§14.7); a terminal-history
overlay shows it once the shell owns the view. A wide `pre` block may carry
the horizontal twin (3 px, an 8 px lane reserved on overflow only, the same
rules, no drag affordance).

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

**As built at I-4.** `Role::Rail` is the second display-bound surface role:
`create … role=rail` takes no bind, is gated exactly as `role=status` (the
renderer, or the declared session compositor while it hosts — `E_PERM`
otherwise, judged before the geometry) and is then admitted by
`pane::admit_rail` (pure, host-tested: one per display, W == the display
width, H == `rail_h`, a display taller than the strip, and the Instrument
profile in force — a legacy display refuses it as MALFORMED, `E_INVAL`,
since it has no strip to be exactly). Registration (`Comp.rail`, gen-pinned
like `Comp.status`) moves no leaf — the carve has reserved the strip since
I-2 — it PLACES the surface (`surface_target`'s rail arm, `rail_rect`) and
keeps the display off Direct as the bar does. The rail follows the DISPLAY
as the bar does: a declaring session retires a SYSTEM renderer's rail, a
session release or takeover retires the holder's, and a scale or theme
change retires a rail whose height is no longer `rail_h` (or that a legacy
bundle leaves stripless) for its owner to re-mint. Pointer routing treats
it as chrome (`chrome_at`, `ptr_crossing`, the test-mode press witness);
the compositor moves no focus on a press. The `rail` file publishes its
rect beside `statusbar`; the new `chords` file publishes the binding table
in force (`Chords::render`, the config grammar) for the hints of §8.2.
The compositor's own fill of the two strips (`paint_instrument`) is
STRUCTURAL-only since I-4, as the legacy strip fill always was: ungated,
it wiped the rail on every focus-only repaint (a latent I-2 defect the
bar's per-command presents had hidden). halcyond's `railset::RailBar`
mints under Instrument only, on the status bar's `rearm` / `ensure`
cadence, and its pump turns a primary press into a
`RailAction` that each owner acts on under its own authority — the console
renderer through `pane/<id>/ctl`, the session through the layout file.

### 8.1 The top rail (34)

Left to right, padding 10 / 8:

- **Brand**: the 13 × 13 mark (`amber_muted` 1 px border, two 1 px `amber`
  strokes at (3,3)-(3,8) and (3,7)-(8,7)), gap 9, then the workspaces in
  a 212-wide cluster so the context column starts where the mockup's
  does: with ONE workspace the mockup's `WORKSPACE 01` label (11/600
  uppercase); with more, the numbered chips of §14.1. The mark is the
  same unanimated glyph for every N and its only action opens the
  workspace list (§14.1).
- **Context**: `~/systems/ compositor │ src / renderer.rs` = the focused
  tile's cwd (the OSC 7 fact, `abbrev_home`'d, the last component in
  500), a 1 × 12 `structure` separator with 10 px margins, then the
  focused tile's name — H-3d's context slot, moved up. Single line,
  `secondary` with `text` for the strong fragment; the cwd is
  middle-ellipsised first, then the title end-ellipsised (§14.3); never a
  static path outside fixture mode.
- **Actions** (auto left margin, gap 2; each 26 tall, padding 9, 10/400
  uppercase, transparent 1 px side borders; hover `hover` ground +
  `structure` borders; pressed ink `amber`): `═ SPLIT H`, `║ SPLIT V`
  (the split chords' pointer twins — new distinct tile in the new pane,
  §9.5), `■ <theme name> ⌄` (the picker, §9.4), `↺ RESET` (§9.5),
  `?` (help, icon-only, min-width 28).
- **Clock**: `HH:MM`, mono 11/500, `text`, padding 10 / 5; UTC as today
  (the RTC's zone; no zone database yet).

**As built at I-4.** `rail::rail_list` (pure; host-tested against the
golden's boxes) paints the mark as marks — the ring and the two strokes at
the measured offsets (+4, +4) 1 × 5 and (+4, +8) 5 × 1 — `WORKSPACE 01` in
the 600 (since I-5a; the body face until then), the context of §14.3 with
its basename in the 500, then the buttons right to left from the 8 px pad:
`?` in mono 10, `↺ RESET`, `■ <theme> ⌄` with the swatch ringed by
`Derived.swatch_ring` (white at 12 % over `amber`: `#CDC199` on Carbon,
the kit's inset shadow reproduced through the executor's lerp), `║ SPLIT
V`, `═ SPLIT H`, and the clock in mono 11 (since I-5a; the 12 px island
cell until then — the golden's `09:41` is 31.9 wide and the run now lays
32). The box-drawing icons and the chevron are drawn marks of the golden's
footprint, and stay so after I-5a's re-subset brought U+2550/2551 into the
mono face: the golden's icons are a browser fallback font's glyphs, 8
wide, where Cornucopia's at 10 px are 5 (§7.2). The
buttons sit 26 tall at y 4 (3.5 snapped half up), the separator at y 11
(10.5 likewise), and every uppercase run is tracked 0.08 em through
`GlyphSource::shape_run_spaced`. Actions as built: SPLIT H / V split the
focused pane (a refusal says `SPLIT REFUSED`); RESET runs
`rail::reset_plan` (§9.5) and says `LAYOUT RESET`; the mark opens
`menu::workspace_menu` (one row until HALCYON-WORKSPACES); the theme
control and `?` are routed and refused visibly (`THEME PICKER NOT
AVAILABLE`, `HELP NOT AVAILABLE`) until I-7. The theme's name is the
resolved bundle's (`built-in` when none was loaded). The minute now WAKES
both owners' polls (`statusset::clock_timeout_ms`); before I-4 the clock
lagged until an unrelated event.

### 8.2 The bottom rail (25)

Padding 10 each side, mono 10/500 uppercase:

- **Left, the condition** (§14.3; H-3d's condition slot in the mockup's
  clothes): a 6 × 6 glyph area, gap 8, the label. Idle: a hollow 1 px
  `secondary` square and `READY` or the last result. Running: a filled
  4 × 4 `amber` square centred in the 6 × 6 and `RUNNING · <command>` in
  `secondary`, no pulse. Success: `✓` in `success` and `EXIT 0 ·
  <command>`. Failure: `!` in `error` and `EXIT <n> · <command>` — a stale
  failure is never shown as running. The command is the sanitised cmd
  mark, ≤ 96 characters, end-ellipsised to the width; an optional measured
  elapsed time follows the dot; no click re-executes. A transient status
  message (uppercase, 1800 ms, the last message resets the timer; `amber`
  for an action, `error` for a refusal such as `FINAL TILE IS PROTECTED`)
  replaces the slot and the LIVE model returns when it expires — never a
  hard-coded `READY`. The square's glow is an effect (§10).
- **Centre, the chord hints**: `SUPER + ARROWS  FOCUS · SUPER + TAB
  TILES` — OUR chords (ruling 3), generated from the registry's bindings,
  never literals; `dim`/`secondary` alternation, gap 8; hidden below 820
  wide. `SUPER + 1–9  WORKSPACES` takes the centre while more than one
  workspace exists and while switching or moving (§14.1).
- **Right**: the pane count (`3 PANES`), a separator, `LOCAL` (the
  session's host name when one exists; `LOCAL` otherwise).

**As built at I-4.** Under Instrument `status::status_list` dispatches to
`rail::footer_list`; the legacy list is byte-identical. The four conditions
are decided by `rail::footer_state` from the focused tile's own facts
(`Transcript::running`, the pane's recorded status, the peeked exit code,
the last command): READY = nothing has run yet (the hollow square — the
golden's fixture shows the kit's filled `success` pulse at READY, a fixture
state this design replaces, so the I-9 parity mask exempts the pulse);
RUNNING = the filled 4 × 4 `amber` square; EXIT 0 = `✓` in the mono face
(since I-5a's re-subset; the body face served it until then); EXIT n =
`!`. The whole footer runs in the free-running Cornucopia at 10 (9 narrow)
tracked .08 em since I-5a: `READY` 29 wide, `3 PANES` 41, the golden's
29.000 / 40.609. The label is `rail::footer_label`
(the sanitised command, ≤ 96 characters, uppercase, end-ellipsised to the
room the centre leaves); no elapsed time yet. The hints are
`rail::hints_from_chords` over the compositor's `chords` file, re-read at
every relayout: `SUPER + ARROWS  FOCUS` when the four focus actions sit on
the arrows, `SUPER + <combo>  TILES` for whatever `cycle` is bound to; the
spans alternate `dim` / `secondary` by POSITION with `·` in `structure`,
as the kit's `nth-child(even)` does, and the centre is centred between the
two end groups (the kit's `space-between`). The pane count is
`rail::pane_count` over the parsed tree (a stack counts once; `1 PANE`
singular) less the leaves the owner calls foreign -- a hidden leaf
hosting a surface it does not describe, which is the console renderer's
backgrounded leaf sharing a session's root (the dump marks it `hidden`
exactly as a zoomed-away pane). No host name exists on the device, so the right group reads
`LOCAL`. The bar's say line (test builds) appends `running` and `panes`.

### 8.3 Narrow displays

At a logical width ≤ 820: the brand shows only its mark and the active
workspace, the context and the button labels hide, the rail gap is 8, the
centre hints hide, the footer type is 9, the picker shifts right by 44
(clamped into the display — a small, recorded safety delta), and the
workspace root keeps a minimum width of 840 with the workspace panning.
At 821 the wide layout returns.

**As built at I-4.** Both lists take the narrow form at a surface width ≤
820 logical (`rail::NARROW_W`): the cluster is 54 wide with the mark and
the active number, the context and the button labels hide (icon-only
buttons of 28; the theme control keeps its swatch and chevron), the
footer's hints hide and its type is 9 (the free-running mono serves 10
and 9 since I-5a; the island floor served both until then). Host-tested at
800 and 821. The workspace root's 840
minimum and the panning are NOT built — the carve has no panning yet (a
residue for I-6, the dividers slice, whose clamps it belongs with) — and no
guest lane drives a narrow display (the `mode` verb is renderer-gated and
no gate holds the renderer's seat).

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

**As built at I-3.** `Comp::chrome_at` finds the chrome surface whose
`surface_target` (the bound pane's `tagbar`) contains the point — a
header, or an empty pane's placard — and `ptr_target` prefers it to the
content hit (`ptr_hit`); `ptr_route`'s ungrabbed arm, `ptr_btn` and
`ptr_scroll` all read it, so a header receives PTR_MOVE (surface-relative,
clamped into the surface), PTR_BTN and SCROLL exactly as a hosted surface
does, the release following its press. A new event kind, `TEV_PTR_LEAVE`
(11), goes ONLY to a chrome surface, when the routed target moves off it
(onto content, another header, nothing, or a placed menu's grab;
`Comp::ptr_over`, gen-pinned): a header un-hovers on it. Click-to-focus is
a CONTENT press's — a chrome hit hosts nothing, so the compositor moves no
focus on a header press; the owner decides: `chromeset::pump` turns a
primary press into `Focus(id)` (the index, the name, the metadata) or
`Close` (the action box, `chrome::header_hit`), a secondary press
anywhere but the `×` into `Menu { id, count, x, y }` (display
coordinates: the tagbar's origin plus the point), and the owner writes the
pane verb the compositor judges per write (`focus` needs `actor_hosts`,
`close` the whole subtree). A header confers no authority its owner
lacks. Dividers are I-6's (no capture yet).

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
| Workspace N | Super + 1..9 switches; Super + Shift + 1..9 moves the focused tile (ruling 13; HALCYON-WORKSPACES §4; free keys) |
| Theme picker | Super + T (ruling 13); `SetMode Tabbed`, which holds Super + T today, moves to Super + Shift + T |
| Help | Super + / (ruling 13) |
| Reset | pointer on the rail only (no chord) |
| Escape | ends a drag; dismisses a menu / dialog (the compositor's, as built) |

The footer's hints are generated from these bindings (§8.2), so a rebind
in `chords.rs` is a rebind of the hint.

Input priority is unchanged in shape: trusted system chord > modal >
placed menu > divider capture > the Super plane > the focused tile. The
consumed chords never reach a pts (the plane's swallow set).

### 9.4 The theme picker and live switching

The picker is a `Role::Menu` surface halcyond paints and the compositor
places (H-3c's machinery: the grab, click-away and Esc dismiss owned by
the compositor), anchored under the theme control at rail + 5, right
aligned, 286 wide, `min(list, display − 72)` tall and scrolling (§7.7's
indicator, min thumb 18); its rows
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

- **Oracle.** `docs/halcyon-carbon-handoff/reference/` (frozen; SHA256SUMS;
  = the bundle's `dist/` at `074bc564`) rendered by
  `round2/capture/capture.mjs` **on this machine** — Astra captured
  nothing (no font bytes, no controlled browser), so the oracle is ours.
  Pinned Playwright + Chromium, fonts injected as exact bytes with their
  SHA256s recorded, remote requests aborted, clock frozen at 09:41,
  `READY`, reduced motion, transitions and animations off, the caret forced
  visible; a 98-scenario matrix (13 geometry rows × baseDpr 1 and 2 at
  Carbon = 26, the 13 themes at 1440 × 900 × 2 = 26, 23 states × 2 =
  46), each with `page.png` +
  `geometry-styles.json` + `metadata.json` and a run `manifest.json`. Two
  modes:
  - **native** — THE ORACLE of this arc ("native-target revision 2"):
    Plex Sans 400/500/600 from `third_party/ibm-plex/ttf/`, the full
    Cornucopia Regular v34.6.1 in every mono role at the mockup's CSS
    sizes, the 45 contrast replacements, `⊢` after the path, Super labels.
    Its mono line pitch is still the CSS's Plex Mono number; the retune to
    Cornucopia's metrics is our own target revision, masked in the diff,
    never pixel-matched to the browser.
  - **historical** — Plex Mono 400/500 at the pinned source: provenance
    only, produced when Plex Mono is at hand; never compared against the
    native raster for mono.
  What the dumps are NOT: `Range` rectangles are line fragments, not
  baselines (recover baselines from the face's metrics); pseudo-element
  boxes are DERIVED from the containing block and marked so; an alpha
  `color-mix` is recorded as rgba and its composited pixel read from the
  PNG. "1440 × 900 at 200 %" is a 2880 × 1800 image at baseDpr 1 and a
  5760 × 3600 one at baseDpr 2, by the stated convention (§3.1).
- **Numerical verdict** (ACCEPTANCE-TESTS §2): opaque interiors exact RGB8
  on ≥ 3 × 3 patches; every structural edge at its snapped boundary and
  thickness; glyph bounding boxes against the pinned faces with a
  per-channel tolerance only inside a tight glyph-edge mask; the small
  features (the mark, one header row, a rule) asserted directly, never
  folded into a percentage. A missing object is a failed setup.
- **Native goldens.** `tools/interactive/ls-gfx-instrument.exp` (a new
  LS-CI scenario on the `--config ci` image with the Instrument lever):
  the 3-pane / 10-tile fixture (`fixtures.json`) at 1280 × 720 and 1440 ×
  900 at 100 and 200 (the harness's rows; there is no 1280 × 800 golden); the state matrix S01-S14; the picker cycling all 13
  with PIDs, order, expansion and scroll preserved; the seat's negative
  controls for `role=rail`, `weight` and `theme`; the crash of one
  kaua-term contained to its tile. The pixel reader is `gfx_compose.py`'s,
  extended. Its first slice exists since I-2:
  `tools/interactive/ls-halcyon-instrument.exp` boots the console-lever
  image with the profile lever (`THYLACINE_HALCYON=1
  THYLACINE_HALCYON_SESSION=0 THYLACINE_HALCYON_PROFILE=instrument`; it
  SKIPs on any other) and reads the carve back through the compositor's
  own files and pixels — the profile arriving through the wire, the
  stack-of-one frame under two rails, the 1:1 split's snapped track, the
  rule, the joint counted as 24 `structure` around 25 `desktop`, the
  focus frame, the empty body, zoom. Its second slice exists since I-4:
  `tools/interactive/ls-halcyon-session-instrument.exp` boots the
  session-lever image with the profile lever (`THYLACINE_HALCYON_SESSION=1
  THYLACINE_HALCYON_PROFILE=instrument`; SKIPs on any other) and drives the
  session path — the seat's two rails, the footer's conditions and pane
  count from the tile's facts, the rail's SPLIT H under the session's
  authority, the workspace list, the retained tile and its Restart, the
  structural close and the logout.
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

- **I-0 — the oracle and the faces.** Plex Sans 400/500/600 vendored
  (`d3958d68`); round 2 ingested and folded (`009b9062`, §3.1); the
  goldens harness run here in native mode (§11), its manifest and hashes
  recorded; this document ratified. No runtime change.
- **I-1 — the second schema and the bundle.** `InstrumentTheme`, the
  dispatching loader, both projections, the 13 gallery files (round 2's
  colours and its authored ANSI, Appendix A), `Bundle` / `Visual`, the
  wire, `halcyon theme lint` for both.
  Nothing paints differently yet. *Audit-bearing: a new strict parser
  (format-fuzz), the wire.* **LANDED** (`libhalcyon::instrument`,
  `theme::load`, the 127-field wire, the gallery generator
  `tools/halcyon/instrument-gallery.py`, `Comp.bundle`, the lint's two
  schemas and the profile / pick tiers; JOURNAL run 46o).
- **I-2 — the profile's geometry.** `Metrics` grows; the Instrument carve
  (rails, outer pad, tracks, joints, frame, headers — collapsed included,
  the lone-tile rule); the `frame` / `dividers` files; weights and the
  arithmetic; `halcyon-layout v2`. The legacy carve byte-identical under
  `legacy`. *Audit-bearing: the compositor's geometry + I-32.* **LANDED**
  (`libhalcyon::carve`, `INSTRUMENT_BASE` + `Bundle::at` by profile,
  `recompute_instrument` + `paint_instrument`, the `weight` verb and the
  minima refusal, the v2 format through the planner and the tool, the
  `THYLACINE_HALCYON_PROFILE` lever, `ls-halcyon-instrument`; the
  as-built notes in §4.4, §5.2–5.7; JOURNAL run 46o "I-2").
- **I-3 — the stack and the headers.** Collapsed headers placed and fanned;
  the header list (index / name / pills / metadata / ×), the state matrix,
  pointer routing to chrome, expand / close / final-tile, the successor
  rule; the tile states of §14.6 (empty, disconnected, crashed, ended);
  the header verb menu (§14.9) on H-3c's surface. *Audit-bearing: pointer
  routing to chrome, pane authority on header actions.* **LANDED**
  (`chrome::parse_tree` / `header_list` / `placard_list` / `metadata_for`
  / `Fate`, `chromeset`'s pointer pump and `ChromeAction`,
  `instrument::Derived`, `Comp::chrome_at` + `TEV_PTR_LEAVE`,
  `Pane.separator` + the placard carve + the successor fix, the retained
  tiles and Restart in the session, `menu::tile_menu` + the Instrument
  look, the status notice; the as-built notes in §6.3–6.5, §9.1, §14.2,
  §14.6, §14.9; JOURNAL run 46o "I-3").
- **I-4 — the rails.** `role=rail`; the top rail's four zones on our
  facts (the workspace chips §14.1, the context formatter §14.3); the
  bottom rail and its marks (§14.3); transient status; the narrow branch.
  *Audit-bearing: the gated create + the carve.* **LANDED**
  (`Role::Rail` + `pane::admit_rail` + `Comp.rail` and the `rail` /
  `chords` files in the compositor, `Chords::render`, `Surface::rail_on`;
  `rail::{rail_list, footer_list, hints_from_chords, reset_plan,
  pane_count}` + `GlyphSource::shape_run_spaced` + `Derived.swatch_ring`;
  `railset::RailBar` and its `RailAction`s in both owners, the footer
  restyle through `status_list`, the minute wake; the as-built notes in
  §8, §8.1–8.3, §14.1, §14.3; the fifth gate image
  `ls-halcyon-session-instrument`; JOURNAL run 46o "I-4").
- **I-5 — type and the rich document.** The Instrument `Sheet`, the type
  map, margin collapsing, clamp paddings, the `pre` block, the terminal
  view's colours, `λ … ⊢` in `ut`, the nine syntax roles in `nora`'s
  export, the position indicator (§7.7), the Cornucopia re-subset (λ, ✓,
  ‹ ›, −, ⌘, U+2500–257F) and the Italic subset (§7.1), GPOS pair kerning
  (§7.5, ruling 15), inline media and the gallery in the Instrument frame
  (§14.4), the raw application grid (§14.7). In sub-slices. **I-5a LANDED**
  (the substrate and the chrome type map: the three Plex cuts embedded,
  the Cornucopia re-subset to 342 codepoints and the Italic subset with the
  shared-cell check, `FACE_SANS` / `FACE_SANS_MEDIUM` / `FACE_SANS_SEMIBOLD`
  / `FACE_MONO_ITALIC` / `FACE_MONO_TEXT`, the box-glyph precedence in the
  cell, the `Sheet`'s role table, the rails / headers / placard / menu on
  the map, the golden-width witnesses, the startup guard derived; the
  as-built notes in §7.1, §7.2, §6.4, §8.1–8.3, §14.2; JOURNAL run 46o
  "I-5a"). Remaining: **I-5b** the document's `Sheet` (§7.2's body / H1 /
  H2 / code sizes, §7.5's paddings and collapse, the `pre` block, the
  terminal view's colours and default inks, the caret, the raw grid §14.7,
  the position indicator §7.7, the italic in mono runs); **I-5c** the
  producers (`λ … ⊢` in `ut` and nora's nine roles through the palette
  export); **I-5d** GPOS kerning. §14.4 waits for the inline-media arc's
  merge (it lives on aux-3, `53fcc14c`).
- **I-6 — dividers and minima.** Capture, drag, double-click, the clamps
  and refusals; winsize through the existing path.
- **I-7 — the picker and live switching.** The menu surface, the
  transaction, persistence, the cooperative repaint; the dialog family
  (§14.5).
- **I-8 — effects and motion.** The two ops, the glows, the backdrop, the
  transitions.
- **I-9 — parity gate, audit, rollout.** ACCEPTANCE-TESTS in full against
  Astra's goldens; the Fable round over I-1..I-8 (double-distance batched:
  one round after I-4, one after I-8); `/lib/halcyon/profile` flips to
  `instrument` for fresh images; `legacy` stays selectable for a release.
  **Round 1 (I-1..I-4, two Opus-5 prosecutors, 2026-09-14): 0 P0 / 1 P1 / 5 P2 / 15 P3, all P1/P2 fixed at the close, DIRTY by count -- the fixes are the I-5 round's focus (`memory/audit_instrument_closed_list.md`). The I-4 open defect (no login prompt after a Super+Q logout) is ARM-6's deadlock on main's kernel (no Part D / A1 -- aux's paused merge); fixed on the session's side: a structural close HANGS the tile UP (the down channel's EOF) and the kaua-term ends + reaps its program before exiting, so no zombie of the user's escapes to joey.**

Rollback at every step is the profile word; a palette rollback never kills
a process; a layout v2 file reads under v1 with equal weights.

## 13. For the operator (the residue the research could not settle)

**All nine RULED 2026-09-14** (§2, rulings 10–16); the items stay as the
record of what was asked and recommended: 1 → A (against the
recommendation); 2 → the true Italic; 3 → `secondary`; 4 → I-5; 5 → the
proposed set; 6 → vendored; 7 → adopt; 8 → the Regular; 9 → the retunes
stand.

1. **A shell tile's default presentation** (§7.6). (A) proportional-live
   as ratified 2026-09-06 — the mockup's mono shell tiles are then a
   reference for colours and padding only; (B) the mockup's mono terminal
   view by default, the proportional flow for document tiles and on the
   pill toggle; (C) B's mechanism with A's default. Recommended: **(C)** —
   the mechanism costs the same, the default follows the ratified rule,
   and the toggle gives the mockup's look to anyone who wants it per tile.
2. **Mono italics**: Cornucopia HAS a true Italic (v34.6.1, on this
   machine, §7.1). Embed an italic subset for lifetimes and comments
   (about +20 KB; recommended — the real face, and the one thing the
   browser goldens cannot show, since the harness requests an italic of a
   family it was given no italic file for), a 12° synthetic shear of the
   Regular, or roman with the colour alone.
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
7. **Contrast**: adopt round 2's 45 replacements for the native target
   (recommended — each recomputed here at ≥ 4.6:1, Carbon untouched,
   historical captures unamended; §3.1), or keep the measured 45
   sub-4.5:1 pairs as the design.
8. **Mono weight "500"** (index, doc path, clock, footer): Cornucopia has
   no Medium. The Regular (recommended: one mono face, as today; at 10–11
   px under our +18 % stroke the browser's Plex Mono 500 reads as the
   Regular's ink) or the SemiBold as a second embedded face.
9. **Two strogg ANSI slots, retuned** (Appendix A): bright cyan `#A2C3B5`
   → `#99C4C3` (the only slot of the 208 outside the lint's 30° hue
   bound; without it the shipped tables fail the shipped lint) and blue
   `#ABB8C9` → `#A3B8D7` (chroma 0.028 → 0.050, the one near-grey
   chromatic slot), both at Astra's lightness. Done by default in
   `tools/halcyon/ansi16.json`; a revert is two values — for the
   operator's eye when I-1's gallery lands ("warmer and drier" is that
   theme's stated design).

## 14. The round-2 surfaces (Astra §7, adopted 2026-09-14 with the deltas named)

Nine surfaces the kit did not draw, specified in
`round2/RESPONSE-TO-FABLE.md` §7 and adopted here as designs. The numbers
below are the binding ones (logical px at 100 %, through `Metrics::at`);
prose detail not restated is read from that section. Where the tree
already has the mechanism the delta is named and the mechanism wins. All
share the theme's inks; no new saturated colour anywhere.

### 14.1 Workspaces (HALCYON-WORKSPACES mechanism (A): live roots, per-workspace focus, bound 9)

In the top rail's 212-wide brand cluster (§8.1): N = 1 keeps the mockup's
`WORKSPACE 01`; N > 1 replaces the word with chips `01`..`09` in a
189-wide horizontal viewport — chips 26 × 24 at y 5, gap 4, mono 10, no
outline, no capsule; active = `hover` ground, `text` label, a 2 px `amber`
bottom edge inset 4; inactive = transparent, `secondary`; hover on an
inactive = `hover` + `text`; keyboard focus = a 1 px `amber` inset 2,
independent of which is active. Past 189: 16 px ‹ › buttons at each end
(`secondary`; `amber` pressed; `dim` disabled; they reveal, never switch)
and a 157 viewport; the newly active chip is always scrolled fully into
view and never hidden behind an ellipsis. At ≤ 820 the cluster shrinks to
54, showing the mark and the active number even at N = 1; clicking the
number opens the numbered list as a §14.2 menu, width 160. The mark is the
same unanimated glyph for every N and its only action is that list
(label "Workspaces"). Switching: a click on a chip; Super+1..9 from the
registry (Super+Shift+1..9 moves the focused tile if the registry adopts
it); the status `WORKSPACE 03 · <name>` for 1800 ms (no dot and no name
when unnamed); the footer centre `SUPER + 1–9  WORKSPACES` while N > 1 —
labels from the binding lookup, never a string assumed correct. Model:
dormant processes kept; an inactive EMPTY workspace may vanish, the active
one never; after a compaction the next switch message announces the new
number and the Super digits follow the displayed labels; a tile move is an
ownership-preserving structural operation, never save / restore / respawn;
a failed switch or create keeps the current root and reports `WORKSPACE
UNAVAILABLE`. Layout names name layouts, not workspaces.

**As built at I-4.** N = 1 today (no workspace mechanism exists): the rail
shows `WORKSPACE 01` and the mark opens `menu::workspace_menu` — one
enabled row, `01`, whose choice is a no-op said as `workspace 1 is active`.
The chip painter for N > 1 is built and host-tested
(`the_chips_lay_out_and_scroll_to_the_active_one`: 26 × 24 chips at y 5,
the active one's `hover` ground and inset amber edge, the ‹ › reveal past
189 with a 157 viewport, the active chip always in view) but nothing yet
produces N > 1; the keyboard-focus ring, `Super+1..9`, the `WORKSPACE 03`
status and the footer's workspace hint wait on HALCYON-WORKSPACES.

### 14.2 The object verb menu (H-3c's `Role::Menu` surface in the round-2 look)

Square; min-width 224, max-width 320, padding 4, `pane` ground, 1 px
`structure` border; the shadow (black .24, offset 0/8, blur 24) is an
effect (§10). Anchored at the object's first visible fragment: its left,
its bottom + 4; clamped to a 4 px display margin; placed above by 4 when it
does not fit below; capped and scrolling with §7.7's indicator when
neither fits. Pointer invocation picks the fragment under the pointer,
keyboard invocation the first visible one. Title row 24 tall, horizontal
padding 10, Sans 500 11 `text`, a 1 px `separator` below — a bounded,
ellipsised object label, never a raw untrusted path. Items 28 tall,
padding 10 / 10, label Sans 400 13 and an optional hint in Cornucopia 10
`secondary` (blank when unbound), gap 16, no icon column. Hover or
keyboard focus: `hover` ground, `text` label, a 2 px `amber` left mark at
y 6..22. A checked item may carry a literal check before its hint; no
checkboxes on ordinary commands. Disabled: `dim` ink, no fill, no mark,
skipped by keyboard activation, still legible. Separators 1 px
`separator`, vertical margin 4, inset 8. A destructive item is ordinary
until focused, then `error` ink — never a red block. Up/Down skip disabled
items; Home/End; Enter; Esc returns exact object focus; a pointer-away
dismiss consumes the release under the existing grab discipline (H-3c). No
submenus in v1 (a longer verb gets an ellipsis or a dialog). The object
keeps its selection while the menu is up; the transcript is never dimmed.
Carbon: `#0B0D0E`, focused row `#191C1D`, mark `#C7B98B`; Genera:
`#E8E9E3`, `#CDD1CB`, `#3D526F`. Names are labels; execution routes through
Beacon's typed-object verb engine under the user's authority.

**As built at I-3 (the look, on H-3c's surface).** `menu::menu_size_inst`
/ `menu_list_inst` under the Instrument profile: `pane` ground, the 1 px
`structure` border, the 24 title row (the type in `secondary`, the label
in `text`, end-ellipsised), a `separator` rule, 28 rows in Sans 13 `text`
(`dim` disabled — no fill, no mark, skipped by Up / Down / Home / End,
never activated by Enter), the selected row on `hover` with the 2 × 16
`amber` mark at y 6, separators 1 px with margin 4 and inset 8, the width
clamped to 224..320, padding 4 / 10; the legacy list is unchanged under
legacy. `MenuItem` gained `enabled` and `separator_before`. Since I-5a the
title row is Sans 500 and the items Sans 400 (the sheet's `face_medium` /
`face_body`). Not yet: the anchoring rules (the placement clamps to the
display as H-3c's did), the shadow (§10), the scroll indicator (§7.7), the
hint column (no `MenuItem` carries a hint yet).

### 14.3 Directory, command and running marks (H-3d's status feed; the rails of §8)

Top context: `<cwd> │ <focused tile title>` in Sans 11 — the cwd
`secondary` with its basename segment `text` where the formatter can
identify it, literal slashes, the 1 × 12 `structure` separator with 10 px
margins; the cwd is middle-ellipsised first, then the title end-ellipsised;
no static `~/systems/compositor` outside fixture mode. Footer left: the
four conditions of §8.2 (idle: a hollow square and `READY` or the last
result; running: the filled 4 × 4 `amber` square and `RUNNING · cmd`;
success: `✓` and `EXIT 0 · cmd`; failure: `!` and `EXIT n · cmd`), the
command the sanitised H-3d mark ≤ 96 characters, the elapsed time optional
after the dot, no click re-executes, a transient message for 1800 ms and
then the LIVE model. Centre: the registry-derived hints, the workspace
hint taking precedence while switching or moving. Right: the pane count
and `LOCAL`; the clock stays top-right. The active tile's header metadata
may say `RUNNING` in `secondary` and an exit in `error`; the header is
never tinted whole; a tile's dirty flag and its command status are
independent facts, and under width pressure dirty / attention outranks
elapsed detail.

**As built at I-4.** The context formatter is `rail::rail_list`'s: the cwd
(the focused tile's OSC 7 fact, `abbrev_home`'d) split at its last `/` —
the leading segments in `dim` (the kit's `.muted`; §8.1's "`secondary`"
names it loosely, and the golden's pixels are `dim`), the basename in
`text`, the title in `secondary` after the 1 × 12 separator; the cwd's
lead middle-ellipsised first (`fit_middle`), then the title cut from its
end, then — only when even that overflows — the basename; the title alone
when there is no cwd, and no separator then. The header's `RUNNING`
metadata and the footer's condition read the same transcript facts.

### 14.4 Inline image and the small gallery (the inline-media arc's `view` / `gallery`, in the Instrument frame)

A rich block inside the 720 content width: a 1 px `separator` frame, no
shadow, no radius, `code_bg` behind the pixels; native aspect ratio, width
= min(intrinsic logical width, available width), no upscale unless asked,
initial height ≤ 360 with contain scaling; transparency composites on
`pane` (no checkerboard; a transparency-inspection tool can be a later
verb); an unloaded image reserves its declared aspect between 80 and 240
tall with `Loading image…` in `secondary`; a decode failure is the error
glyph plus plain text, never a toolkit's broken-bitmap icon. Caption: gap
6, Sans 12 / 1.45 `secondary`; an optional index or path line in
Cornucopia 10 at gap 2. Block margin 18 above and below. Selected: the
frame turns `amber` and a 2 px leading `amber` mark appears; the pixels
never change; keyboard focus adds the usual inset outline OUTSIDE the
image, never a wash over it. Verbs (Open / Copy reference / Save, as
permitted) come through §14.2; selecting is not consent to execute.
Gallery: 2 columns at content width ≥ 420, else 1; gap 10 both ways;
uniform 4:3 cells containing the image without crop; captions wrap per
cell; at most 6 items decoded initially, then a `Show all N images` action
in the menu/link style — no hidden unbounded decode; arrows move the
selection while the gallery owns focus (Super stays the workspace plane);
alt text and bounded dimensions required; full fidelity regardless of
pane focus.

### 14.5 The dialog family (help's frame: `dialog_bg`, a 1 px `focus_neutral` border, the existing backdrop)

Square; max width 480 for a confirmation, 420 for a one-line prompt, never
wider than the viewport − 32. Header padding 18 / 20 / 12: an eyebrow in
Cornucopia 10 `amber` with tracking .12 em, the title Sans 500 23 with
margin-top 7. Body padding 0 / 20 / 18, Sans 14 / 1.5 `secondary`. Footer:
a 1 px `separator` above, padding 12 / 20, gap 8, buttons right-aligned.
Buttons 30 tall, padding 0 / 12, Sans 500 12, a 1 px `structure` border,
transparent ground; hover `hover`; the default button an `amber` border
and `text` ink (never a filled amber rectangle); keyboard focus an `amber`
outline inset 2; a destructive button `error` border and ink, never
pre-focused; Escape always cancels. Texts: dirty close — `Close <tile>?`
/ `This tile has unsaved changes.`, `Cancel` (default) · `Discard` · `Save
and close` only when a real save operation exists, else `Cancel` · `Close
without saving`; a save failure keeps the dialog open with `error` text
and never closes anyway. Active job — `A process is still running.` plus
the validated process label, `Cancel` · `Close tile`; "force" wording only
after a graceful close has failed and the action IS force termination.
Reset — `Reset workspace layout?` / `Rearrange this workspace. Running
tiles will remain open.`, `Cancel` · `Reset layout`, geometry only by
design; anything that starts or stops processes must be titled `Restore
saved layout?` and disclose it. One-line prompt (e.g. `Rename workspace`):
label Sans 12 `secondary`, gap 6, a field 32 tall in `kbd_bg` with a 1 px
`structure` border, padding 6 / 9, Sans 14 `text`, `amber` border on
focus, the existing selection role; an error below at gap 6 in Sans 12
`error`; `Cancel` · `Apply`, Apply disabled while invalid; Enter submits
only when valid and no IME composition is active. Caret and selection
belong to the field, never to the terminal beneath.

### 14.6 Empty, disconnected, crashed and ended tiles (H-3b's status feed; the exit latch)

**Empty pane** — the explicit N = 0 exception to §6.2's non-empty stack:
the flat frame and `pane` ground, NO 32 px header (no tile exists); at the
content's top-left, padding 20: `Empty pane` in Sans 500 17 `text`, gap 8,
`Open a shell to start here.` in Sans 13 `secondary`, then one text-style
action `Open shell` in `amber`, 28 tall, horizontal padding 8, hover
`hover`; a focused empty pane gets the neutral frame but no mark and no
index; spawning is the user's ordinary authority and the action disables
while pending. **Disconnected** — header, order, body transcript and title
retained; metadata `DISCONNECTED` in `error`; a notice strip prepended to
the body: min height 32, `header` ground, a 1 px `separator` below, padding
8 / 12, Sans 12 `secondary` with an `error` `!` — `Connection lost. The
last output is preserved.`; no blinking caret; the verb menu offers
Reconnect only when meaningful, Restart as a distinct NEW process, Copy
output, Close; output is never cleared and a failed command is never
restarted automatically. **Crashed** (a parser or renderer) — the same
retained frame, metadata `CRASHED`, a safe reason or code and nothing else;
one tile's failure never repaints the screen; the focus mark stays `amber`
while the metadata is `error` — focus and failure are two facts. **Ended**
— metadata `EXIT 0` in `success` or `EXIT n` in `error`; the body frozen
with no caret and a final line `Process ended · exit n` in Sans 12
`secondary`; retained until closed or restarted. A child command ending
inside a still-running shell is NOT an ended tile — that is §14.3's marks.
Status strings, durations and process labels are data, never the fixture
literals.

**As built at I-3.** The empty pane: `Layout::place_frame` gives a lone
EMPTY leaf no header row — its `tagbar` is the whole interior and its
`content` ZERO — and the compositor rests it on `pane`; halcyond's chrome
surface there paints `chrome::placard_list` (the title, the hint, and
`Open shell` only where the owner may spawn: the session, which re-admits
the leaf to its spawn plan on the press; the console renderer never). An
empty leaf inside a stack of several keeps a header row like any tile.
The retained tiles: the session judges the stream
(`Ingested::{Ended(n), Disconnected, Crashed}`: an exit record, an EOF
without one, a wire error) and under Instrument RETAINS the tile
(`SessionTile::retain`: the child killed and reaped, the pipe left out of
the poll, the `Fate` on the tile and the header); the header's metadata
says the word; the body (`Tile::render`) paints no caret, an ended tile's
`Process ended · exit n` line after its content, a disconnected tile's
notice strip (≥ 32 tall, `header`, the `separator` rule, the `error` `!`)
prepended, a crashed tile nothing beyond the word. Under legacy the tmux
rule stands byte for byte (a clean exit closes the leaf, anything else
freezes the affordance). Restart (the tile menu): `SessionTile::into_parts`
keeps the SURFACE — the leaf's place, weight and frame — and spawns the
tile's command line again as a new process with a fresh transcript. Not
built: Reconnect, Copy output, the disconnected tile's other verbs.

### 14.7 A full-screen terminal application inside a tile (the raw path)

The 32 px header stays exactly as for any active tile. The raw grid fills
the content rect below it in `terminal_bg`: no code-fence inset, no 2 px
amber rule, no rich padding, no §7.7 indicator; only the 1 px pane frame
and the header separator bound it. The grid rounds DOWN to whole cells and
the right/bottom remainder is `terminal_bg`, never a stretched cell; an
application's own status rows live inside its grid, apart from the OS
footer. Cursor shape, colour and visibility follow the application's
protocol — its explicit colour kept, otherwise `amber`; block, bar and
underline derive from the Cornucopia cell, not the specimen's 7 × 14. No
`λ path ⊢` and no second caret in the alternate screen; focus loss keeps
the text's colours under the existing cursor-visibility policy, no grid
dimming; the return to normal mode restores the prompt and the transcript
position through the mode protocol, never from a picture of old pixels.

### 14.8 Login and the pre-login console — unchanged

Aurora, the pre-login console and the trusted path keep their identity,
palette and authority: no Carbon login, no user theme before
authentication, no new authority for a session theme file; logout returns
the console to its own theme. "Carbon default" is the Halcyon USER SESSION
(§4.1's scope), not the boot or authentication environment.

### 14.9 Header commands: the tile verb menu (no hamburger, no command pills)

The reference header anatomy stands (§6.4; the tag-bar PILLS of ruling 5
are state, not commands, and stay). A secondary click anywhere on the
header except `×`, or the registered context-menu chord on the focused
header, opens a §14.2 menu for the tile: first the program-provided,
permitted commands (e.g. Save / Save as), a separator, then the shell-owned
Rename tile / Move to workspace / Restart / Close as applicable; unavailable
items disabled visibly; with nothing program-provided, the shell's alone.
Commands are typed actions scoped to the owning tile through the existing
verbs path: a program's string is a label or a validated operand, never
compositor authority; program commands register through a bounded existing
protocol or a reviewed registry entry — no `pill` mark is assumed built.
Middle-click executable-text semantics stay inside the transcript; the
header's primary click still selects or opens the tile.

**As built at I-3.** `menu::tile_menu(id, name, count, retained)`: Restart
(enabled for a retained tile), Close (enabled with a sibling), then Rename
tile… and Move to workspace… disabled behind a separator (the dialog
family and the workspace mechanism are I-7's and I-4's); each an INTERNAL
action `tile <verb> <id>` the owner interprets — never a shell command —
through the same `ChromeAction` path a header press takes. No
program-provided commands register yet.

## Appendix A — the ANSI-16 tables (Astra's authored set, adopted 2026-09-14; the tool is the lint)

The operator delegated the tables to main (2026-09-14, "design them
yourself"); Astra delivered a hand-authored set in round 2 regardless
(`round2/ansi16.json`, one table per theme, also in
`round2/palettes/*.toml`). Both were measured before choosing (JOURNAL run
46o, "Round 2"):

- **Astra's**: all 208 slots ≥ 3.46:1 against `terminal_bg`, sixteen
  distinct per theme, every bright slot lighter (dark) / darker (light)
  than its normal, black and white the extremes of each eight-slot ramp,
  every chromatic slot within 30° of its name — except strogg blue (chroma
  0.028) and strogg bright cyan (hue 168), §13.9. Mean chroma 0.056–0.082
  per theme: the instrument's register, and "black" a readable charcoal.
- **Ours** (`tools/halcyon/instrument-ansi.py`, the generator): hue-exact
  by construction, but its chroma register (1.25 × the theme's own accent
  chroma) over-saturates the warm themes — strogg bright red `#FB8274`
  (chroma 0.150) and bright cyan `#1FC1C8`, signal bright yellow `#EBA32D`,
  mean chroma up to 0.118 — and its black on a dark theme is the pane
  ground, unreadable as ink.

**Verdict: Astra's tables are the gallery values**, with two strogg slots
retuned at Astra's own lightness — bright cyan `#A2C3B5` → `#99C4C3` (hue
168° → 195°: the one slot of the 208 outside the 30° bound below, so
"verbatim" and "the lint passes" could not both hold) and blue `#ABB8C9`
→ `#A3B8D7` (chroma 0.028 → 0.050: the one chromatic slot the lint notes
as grey). The adopted set is **`tools/halcyon/ansi16.json`** (= `round2/ansi16.json`
+ those two values; the record in `round2/` is never edited); I-1's
gallery files are written from it. The tool is demoted to the CHECK:
`tools/halcyon/instrument-ansi.py --lint <ansi16.json | palettes-dir>`
applies the rule below to any table and exits non-zero on a violation
(six sabotaged tables — swapped hues, black as the ground, a darker
bright, a duplicate, a white that is not the extreme, fifteen slots —
each fail for their own reason); the generator stays as a first draft for
a NEW theme, held to the same lint, never the source of a shipped table.
The rule, so a table is checked rather than trusted:

- Every slot ≥ 3:1 against `terminal_bg` — black included: on a
  near-black ground "black" is a readable charcoal, on a light ground the
  lightest readable neutral, never the ground itself (SGR 30 text must
  read; a program wanting RGB 0 says so in truecolor).
- All sixteen distinct. Bright white may equal `terminal_text` (the alias
  the parser allows) but need not.
- Slots 1..6 within 30° of their hue names (OKLCH: red 25, green 145,
  yellow 90, blue 262, magenta 330, cyan 200), in the theme's
  temperature; no semantic slot is the theme's accent merely because the
  accent exists.
- Bright slots lighter than their normals on a dark theme, darker on a
  light one; within each eight-slot ramp black is the darkest (dark) /
  lightest (light) and white the opposite extreme.
- The terminal's default fg/bg are `terminal_text` / `terminal_bg`,
  separate from slots 7 and 15.
