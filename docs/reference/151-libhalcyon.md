# 151 — libhalcyon: the Halcyon environment library (`theme`)

## Purpose

`usr/lib/libhalcyon` is the native (`no_std`, libthyla-rs-family) library that
carries the Halcyon environment's shared code (HALCYON.md §13). At H-3a it
holds exactly one module, `theme`: the Daylight visual scripture
(`docs/HALCYON-VISUAL.md`) as code — every colour and chrome metric the
graphical shell paints, as constants. It is the **single token source** the
ratified H-3 split names: halcyond's transcript `Sheet` + its chrome surfaces
AND tapestryd's pane bevel / hairline / cast-shadow / tag-bar geometry read
their values from here and nowhere else. A value that appears in two places
drifts; the whole point of the crate is that there is one place.

Later H-3/H-4 chunks add the rest of the family here (chrome helpers, the
verbs engine, layout save/restore) as sibling modules.

## Public API

```rust
pub mod theme;                       // lib.rs: the only module at H-3a/H-3b-1

// theme
pub type Argb = u32;                 // 0xAARRGGBB, alpha 0xFF (opaque)
pub struct LiveKey { key, tint, raised, border, fg, fg_dim, fg_muted: Argb }
pub struct Syntax  { slate, sage, sand, moss, ash, dusk, smoke, fen, cinnabar: Argb }
pub struct Theme   { /* ground, ink, bevel, accent, live keys, syntax, status bar */ }
pub struct Metrics { bevel, gap, hairline, header_h, status_h, tag_pad_x, tab_strip_h: i32 }
pub const METRICS: Metrics;          // HALCYON-VISUAL §3.1 / §4.3 (+ the tab strip)
pub const DAYLIGHT: Theme;           // HALCYON-VISUAL §1 / §2 / §6, verbatim
pub const fn hairline(t: &Theme) -> Argb;   // == t.header (§2.4), by construction
pub fn daylight_palette() -> vt::Palette;   // the transcript's Daylight-grounded ANSI-16
```

Contracts:

- **`DAYLIGHT` is the scripture, not a taste.** Every field is the doc's
  `#rrggbb` widened to opaque `Argb`; `theme::tests::daylight_matches_the_scripture`
  pins each one. A change here is a scripture divergence and needs the doc
  first.
- **The four bevel values are one derivation.** `bevel_top/left/right/bottom`
  come from the single NNW light direction (§2.1) and are regenerated together
  or not at all — never adjust a single edge. The test asserts they are four
  distinct values (a two-value diagonal is §2.1 broken).
- **`hairline(t) == t.header`.** The inner hairline (§2.4) is the tag-bar
  background colour so it vanishes beside a tag bar and shows only against
  content; one name for that intent.
- **`daylight_palette()` agrees with the Sheet.** `bg == DAYLIGHT.surface`,
  `fg == DAYLIGHT.fg`, bright-white pinned to `fg`. This agreement is
  load-bearing: halcyond's "default ink" check (`st.fg == sheet.ink`, the hook
  that applies obj/dim semantic colours) fires only when the pen default —
  which comes from this palette — equals `sheet.ink`. Foreign-program SGR
  renders through this palette; halcyon's own output renders through the Sheet.
- **`Metrics` are pixels, `i32`.** Consumers cast at the use site
  (`as u32` in tapestryd's geometry).

## Implementation

`theme.rs` is data plus three tiny functions; there is no logic to get wrong
except the values, which is why every one is pinned by a test.

**Where each field is consumed (the reader's map):**

| Field(s) | Consumer | Since |
|---|---|---|
| `surface`, `fg`, `fg_dim`, `fg_muted`, `syntax.*`, `ember`, the `daylight_palette()` | halcyond `daylight_sheet()` + the transcript's vt palette (`usr/halcyond`) | H-3a-1 `6a812348` |
| `floor`, `bevel_*`, `header` (as the hairline), `border` (the cast shadow), `ember`, `ember_deep` | tapestryd `paint_borders` / `paint_strips` (`usr/tapestryd/src/server.rs`) | H-3a-2 `6680ca7f` |
| `METRICS.bevel + hairline` (the ring inset) | tapestryd `pane.rs recompute` | H-3a-2 |
| `METRICS.header_h` (the per-leaf tag-bar carve), `METRICS.tab_strip_h` (the tab/stack indicator), `header` (the tag-bar resting fill) | tapestryd `recompute` / `strip_h` / `paint_borders` | H-3b-1 |
| `sage`, `cinnabar` (the live-tile keys), `status_*` | reserved: the H-3b-4 status hairline / shadow companion; the H-3d status bar | — |

**`header_h` IS the tag-bar height.** The struct comment says so ("tag bar
height (20)") and the H-3b-1 carve reads it directly. The H-3b design plan
spoke of adding a `TAG_BAR_H`; that would have been a second name for the same
20 — do not add one. HALCYON.md §13.6 records the reconciliation.

**`tab_strip_h` was tapestryd-private until H-3b-1** (`pane::TAB_STRIP_H`);
it moved here so halcyond can size/place chrome surfaces against the same
constant the compositor carves with, without a private copy.

## Data structures

`Theme` (all `Argb`): ground `floor / surface / header / raised / border`;
ink `fg / fg_dim / fg_muted / fg_subtle`; bevel `bevel_top / bevel_left /
bevel_right / bevel_bottom`; accent `ember / ember_dim / ember_deep`; live keys
`sage: LiveKey / cinnabar: LiveKey`; `syntax: Syntax`; status bar `status_bg /
status_fg / status_muted / status_idle`. The struct is theme-agnostic
(HALCYON-VISUAL §1.4/§4/§9): a second theme (Frutiger Aero, deferred) is a
second `Theme` const of this exact shape; nothing structural changes.

`Metrics` (`i32`, pixels):

| Field | Value | Meaning |
|---|---|---|
| `bevel` | 2 | pane bevel width (§3.1) |
| `gap` | 2 | inter-pane gap AND workspace padding (§2.3) |
| `hairline` | 1 | structural hairline (§2.4) |
| `header_h` | 20 | **tag bar height** (§4.3) — the H-3b-1 per-leaf carve |
| `status_h` | 20 | status bar height (§6) |
| `tag_pad_x` | 6 | tag bar horizontal padding (§4.3) |
| `tab_strip_h` | 5 | tab/stack indicator strip (G-6c; glyph-free per D7) |

No on-wire or on-disk layout: nothing here needs a `_Static_assert` twin.

## State machines

None.

## Spec cross-reference

No TLA+ module; the binding text is `docs/HALCYON-VISUAL.md` and the tests are
the pin (see Tests). The consuming geometry is described in the `sub-tapestryd`
vault dossier (pane.rs / server.rs are vault-owned) and in
`docs/reference/150-halcyond.md`.

## Tests

Three host unit tests in `theme.rs` (`#[cfg(test)]`):

- `daylight_matches_the_scripture` — every `DAYLIGHT` value against the doc,
  plus the four-distinct-bevel assertion and `hairline(d) == header`.
- `metrics_match_the_scripture` — `bevel`, `gap`, `hairline`, `header_h`,
  `status_h`, `tab_strip_h`.
- `ember_is_the_bonfire_ember` — the accent is shared verbatim with Bonfire
  (`0xFFE07840`), the link between the two surfaces.

Run on the host (the workspace's default target is bare-metal, so override it):

```bash
cd usr && cargo test -p libhalcyon --target aarch64-apple-darwin --release
```

What they do NOT cover: whether a consumer actually reads the field (that is the
interactive net — `ls-halcyon` samples the bevel + parchment, `ls-gfx-panes`
samples the strip + tag-bar colours).

## Error paths

None — pure constants and total functions.

## Performance characteristics

Not applicable.

## Status

- H-3a-1 `6a812348`: the crate + `theme` + halcyond adoption.
- H-3a-2 `6680ca7f`: tapestryd adoption (the ring, the strips, the shadow).
- H-3b-1: `Metrics.tab_strip_h` (moved in from tapestryd); `header_h` consumed
  as the tag-bar carve.

## Known caveats / footguns

- `Cargo.toml`'s header says "no deps"; the crate depends on `vt` for
  `daylight_palette()` (`vt::Palette`, `vt::THEMES`). Pure-constant consumers
  still pay that dependency.
- `Argb` alpha is `0xFF` by convention, not by type — a caller composing with a
  translucent value gets whatever the executor does with it; nothing here
  checks.
- Do not introduce a second name for a value that already has one (`TAG_BAR_H`
  vs `header_h` was the near miss). One value, one name, one test.

## Naming rationale

*Halcyon* is the graphical shell's name (the calm before; the impossible
return); *Daylight* is the scripture's name for its first theme. Neither is a
Thylacine-thematic coinage of this crate — they are inherited from
`docs/HALCYON.md` / `docs/HALCYON-VISUAL.md`, and the crate is named for the
environment it serves.
