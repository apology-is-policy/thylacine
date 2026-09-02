# 151 — libhalcyon: the Halcyon environment library (`theme`)

## Purpose

`usr/lib/libhalcyon` is the native (`no_std`, libthyla-rs-family) library that
carries the Halcyon environment's shared code (HALCYON.md §13). It holds
`theme` (H-3a) and `layout` (H-4a). `theme` is the Daylight visual scripture
(`docs/HALCYON-VISUAL.md`) as code — every colour and chrome metric the
graphical shell paints, as constants. It is the **single token source** the
ratified H-3 split names: halcyond's transcript `Sheet` + its chrome surfaces
AND tapestryd's pane bevel / hairline / cast-shadow / tag-bar geometry read
their values from here and nowhere else. A value that appears in two places
drifts; the whole point of the crate is that there is one place.

`layout` (H-4a) is the `halcyon-layout v1` save format: the serializer + a
bounded, no-panic parser for the pane tree's SHAPE (container modes + active
child; per-leaf tag = the command line), PLUS `from_render_text`, which folds
tapestryd's `pane::render_text` dump back into that tree. It lives here (not in
halcyond or the `halcyon` tool) because BOTH restore paths need it — the
pre-login device-tier restore in halcyond and the user-authority session tool
(HALCYON.md §13.7, the D decision). Later H-3/H-4 chunks add the rest of the
family here (chrome helpers, the verbs engine) as sibling modules.

## Public API

```rust
pub mod theme;                       // lib.rs: the H-3a token source
pub mod layout;                      // lib.rs: the H-4a save format (below)

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
- H-4a-1 `cdce7f3f`: the `layout` module — `halcyon-layout v1` serializer + the
  bounded no-panic `parse`.
- H-4a-2 *(pending)*: `layout::from_render_text` (the render_text bridge) + the
  `usr/halcyon` save tool (the session-tier durable write).

## Known caveats / footguns

- `Cargo.toml`'s header says "no deps"; the crate depends on `vt` for
  `daylight_palette()` (`vt::Palette`, `vt::THEMES`). Pure-constant consumers
  still pay that dependency.
- `Argb` alpha is `0xFF` by convention, not by type — a caller composing with a
  translucent value gets whatever the executor does with it; nothing here
  checks.
- Do not introduce a second name for a value that already has one (`TAG_BAR_H`
  vs `header_h` was the near miss). One value, one name, one test.

## The `layout` module (H-4a)

`layout.rs` is the pure half of Halcyon layout save/restore (HALCYON.md §13.7,
the D decision) — no I/O, no libthyla-rs. Two directions of one format:

- **write**: `serialize(&LayoutNode) -> String` emits `halcyon-layout v1`;
- **read**: `parse(&str) -> Result<LayoutNode, ParseError>` reads it back, and
  `from_render_text(&str, tag_of) -> Result<LayoutNode, ParseError>` folds
  tapestryd's live tree dump (`pane::render_text`) into the same tree.

```rust
pub const FMT_HEADER: &str = "halcyon-layout v1";
pub const MAX_DEPTH: usize = 32;     // container nesting cap
pub const MAX_NODES: usize = 256;    // total node cap (== the compositor pane cap)
pub const MAX_TAG_LEN: usize = 1024; // per-leaf command line (Beacon VALUE_MAX order)

pub enum LayoutMode { SplitH, SplitV, Tabbed, Stacked }  // tokens == pane::Mode
pub enum LayoutNode {
    Leaf { tag: String },                                // tag == command line; "" = empty
    Container { mode: LayoutMode, active: u32, children: Vec<LayoutNode> },
}
pub enum ParseError { BadHeader, BadIndent, BadRow, TooDeep, TooMany,
                      TagTooLong, Empty, Trailing, BadChildCount }

pub fn serialize(root: &LayoutNode) -> String;
pub fn parse(input: &str) -> Result<LayoutNode, ParseError>;
pub fn from_render_text(render: &str, tag_of: impl Fn(u32) -> String)
    -> Result<LayoutNode, ParseError>;
```

**The format.** A header line, then one pre-order row per node, two spaces of
indent per depth:

```
halcyon-layout v1
splith n=2 active=0
  leaf tag="halcyon welcome"
  leaf tag="ut"
```

A leaf is `leaf` (empty tag) or `leaf tag="<escaped>"`; a container is
`<mode> n=<child-count> active=<index>` followed by its children. The tag
escape is exactly three sequences — `\\`, `\"`, `\n` — so the format stays
line-oriented. Surface ids, geometry, and focus are NOT saved: a restored leaf
gets a fresh surface and rect from its respawned program, and geometry is the
compositor's to recompute.

**`from_render_text` is the save-time bridge.** `render_text` prints the SAME
depth-indented pre-order, but each row leads with the pane `<id>` (+ an optional
`*` focus marker), a leaf reads `leaf surface=<n>|empty`, and every row trails a
` [x,y,w,h]` rect. `from_render_text` strips the id/marker, discards the rect,
reads a container's `n=`/`active=` (the SAME tokens `parse` uses), and resolves
each leaf's tag through the `tag_of` closure (the `halcyon` tool reads
`pane/<id>/tag`). A tag longer than `MAX_TAG_LEN` is dropped to empty (never
truncated — a half-command would respawn wrong), so the result always
round-trips through `serialize`/`parse`. Both `parse` and `from_render_text`
feed one shared stack machine (`assemble`), so the tree assembly and the
child-count validation are written once.

**Fail-closed, no-panic.** Every parse path returns an `Err` a caller degrades
on (geometry-only, or no restore) rather than panicking — a panic in a no_std
tool is a silent `exit(1)`. A container's `active` past its child count is
CLAMPED (a slightly-off index must not fail the whole restore); a `n=` that
disagrees with the actual child count is `BadChildCount` (a structural
corruption).

### Consumer: the `halcyon` save tool (H-4a-2)

`usr/halcyon` is the native (libthyla-rs) session tool that carries the SAVE
side (`halcyon layout save <name>`), run AS THE USER (HALCYON.md §13.7). Its
pure lib (`usr/halcyon/src/lib.rs`, host-tested) is argument dispatch, layout
name validation (`name_is_valid`: one path component, `[A-Za-z0-9._-]`, no
leading dot — traversal closed by construction), and session-path building
(`<home>/lib/halcyon/layouts/<name>`). Its bin (`src/main.rs`) reads
`/dev/tapestry/layout` + each `pane/<id>/tag`, calls `from_render_text` then
`serialize`, and writes the file durably into the SESSION tier — the aurora
`config::save` discipline verbatim (write-tmp, content fsync, atomic rename,
then a STRICT metadata fsync on the SAME OWRITE fd; `docs/gfx-status.md` cfg-2a).
The tool takes NO capability, NO SPAWN_PERM, and adds NO server verb: the
authority is the user's own. `layout restore` is H-4b (the tapestryd
`Session(principal)` actor + the claim token — audit-bearing); the tool prints
"not yet implemented" for it today. The device tier (`/lib/halcyon/layouts/`)
is halcyond's / the image bake's, never the user tool's.

**Tests.** `layout.rs` carries host unit tests: round-trip (single leaf, the
two-pane welcome with its serialization pinned, a deep nested tree), the escape
(quotes/backslash/newline), active-clamp, the 12 malformed-input rejections,
the bounds (depth/tag), trailing blanks — plus the `from_render_text` set (the
welcome dump, a single-leaf root, a deep round-trip, the oversize-tag drop, and
the malformed-dump rejections). `usr/halcyon/src/lib.rs` carries the dispatch +
name-validation + session-path tests. Run on the host:

```bash
cd usr && cargo test -p libhalcyon --target aarch64-apple-darwin --release
cd usr && cargo test -p halcyon --no-default-features --lib --target aarch64-apple-darwin
```

## Naming rationale

*Halcyon* is the graphical shell's name (the calm before; the impossible
return); *Daylight* is the scripture's name for its first theme. Neither is a
Thylacine-thematic coinage of this crate — they are inherited from
`docs/HALCYON.md` / `docs/HALCYON-VISUAL.md`, and the crate is named for the
environment it serves.
