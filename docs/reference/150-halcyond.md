# 150 — halcyond: the Halcyon environment client (H-2, the CPU floor)

## Purpose

`usr/halcyond` is the rich-transcript console renderer — the first
inhabitant of the HALCYON.md §13.1 process architecture: **the only place
that thinks**. It owns the transcript state, the Beacon parse, the SGR
subset, layout, the fontdue rasterizer + atlas cache, the paper-light
stylesheet, and the per-frame **cartoon** (the display list,
`usr/lib/cartoon`) that the in-process CPU executor weaves into a
`libtapestry` surface. The vk executor (H-6) will execute the same op set
out of process; nothing in this crate assumes otherwise.

It is spawned by joey as the console renderer (the G-4 slot,
`T_SPAWN_PERM_CONSOLE_RENDERER`) when the one-token device-choice file
`/lib/halcyon/renderer` reads `halcyond`; anything else — absent file,
short read, unknown token — spawns aurora (fail-safe: a corrupt config can
only name the proven fbcon). `usr/joey/joey.c` (the G-4 block) is the only
reader.

## Structure — lib is the brain, bin is the body

The crate is lib+bin from birth (the H-2a lesson: a no_std bin crate's
tests are dormant). The LIB is pure logic over injected bytes — zero
syscalls — host-tested via
`cargo test -p halcyond --lib --no-default-features --target aarch64-apple-darwin`;
the BIN (`[[bin]] required-features = ["guest"]`) owns the syscalls, the
Surface, and the event loop.

| Module | Owns | Host tests |
|---|---|---|
| `transcript` | zones→blocks, the column-addressed line discipline, the streaming feeder (escape holdback + cross-feed UTF-8), Beacon spans, table capture, budgets | the streaming-determinism property + 11 more |
| `layout` | the `Sheet` (paper-light), the per-cell face rule, the §13.5 metrics rule, word wrap, ruled tables, the exit badge, `render_block` → cartoon ops | 6 (wrap determinism, alignment, boxes, pixels) |
| `raster` | `GlyphSource`: vendored DejaVu (fontdue) + the baked Cornucopia mono cell through ONE `cartoon::AtlasPacker`; kerning; regen eviction | 6 (incl. the full-stack word-through-executor leg) |
| `input` | the held-feed policy (aurora's #129/#135/#136 discipline, selftest arms as host tests), `key_bytes`, the modal key map | 8 |
| `select` | Helix-modal selection v0: the flat row list, `Sel` (cursor/anchor/range/yank) | 4 |

## The data flow

```
/dev/consdrain ──bytes──▶ Transcript::feed
                            │  wire::parse (beacon) + the VT-subset scan
                            ▼
                  blocks (cells + styles + objs + tables)
                            │  layout_block (cached by block id)
                            ▼
                  LaidBlock (lines/segs with per-glyph x's)
                            │  render_block
                            ▼
                  Cartoon ──cartoon::execute──▶ surf.pixels() ──present──▶ tapestryd
```

Input: TEV_KEY → (Insert) `key_bytes` → `/dev/consfeed` with the held-feed
discipline; (Normal) the selection state. `beacon rich` is advertised on
`/dev/consctl` at startup — halcyond is the tree's first rich advertiser;
ut reads `/dev/beacon` and exports `/env/BEACON` (BEACON.md §12.3).

## Load-bearing invariants (prose; the audit anchors)

- **The streaming property**: feeding a byte stream in any chunking yields
  the identical transcript structure. The holdback scanner (`safe_cut`)
  must protect the escape OPEN at buffer end — the last-ESC heuristic was
  a real bug (an OSC's ST terminator is itself a later ESC; cutting there
  strands the opener, and the wire parser rightly drops it whole). The
  byte-by-byte fingerprint test is the regression.
- **The robustness contract** (BEACON.md §12.8 P3, inherited): never
  panic, never buffer unboundedly (FRAME_MAX + 16 flushes an over-long
  partial through), every malformed reference skips fail-safe.
- **Span hygiene**: Beacon spans (em/obj/hdr) die at block boundaries; the
  SGR pen persists (terminal semantics). A program dying mid-`em` must not
  restyle the next prompt.
- **The §13.5 metrics rule**: the body box owns any mixed line; mono may
  not stretch it; an all-mono line keeps the exact cell box (foreign
  blocks read as a terminal).
- **Budgets**: blocks (deque eviction) + stored cost + a per-block line
  cap that freezes CONTINUATION blocks — no input can grow memory
  unboundedly (§13.3's content budget).
- **raw_vt_intent**: alt-screen (primary) and row-addressed control latch
  it; nothing paints. The pane-class flip consuming it is H-3.

## Deviations from the §13.3 sketch (deliberate, recorded)

- Selection addresses **(block, item-line, col) over CELLS**, not
  (block, run, byte): the line discipline is column-based (`\r`
  overwrite, tabs, EL), so cells are the honest unit; runs derive at
  layout. v0 selection is row-wise; the flat model already addresses
  rows, so column narrowing extends rather than replaces.
- The exit badge renders on FAILURE only (success is silence).
- `Image`/`Embed` ops exist in the executor but no transcript path emits
  them yet (images are H-7).

## The chrome (H-3b-3): the per-leaf tag bar

`src/chrome.rs` — halcyond owns one `Role::Chrome` tapestry surface per
visible leaf that carries a Daylight tag-bar strip, paints the whole strip,
and the compositor PLACES it at the leaf's `tagbar` rect (the H-3b-2
`surface_target` arm; `create W H role=chrome bind=<pane-id>` is
renderer-gated, and halcyond is spawned with `T_SPAWN_PERM_CONSOLE_RENDERER`
by joey's renderer-choice block). DISPLAY-only at H-3b-3: pills are
commands (H-3c) and the live sage/cinnabar states ride the H-3b-4 status verb.

```rust
pub struct ChromeSet { /* BTreeMap<pane id, Tile{ surf, focused, name, dirty, dead }> */ }
impl ChromeSet {
    pub fn new() -> ChromeSet;
    /// Bring the tiles in line with the layout; paints every dirty tile.
    pub fn reconcile(&mut self, troot: i64, own_surface: u32, gs: &mut GlyphSource);
    /// Drain every tile's events (non-blocking). True = a CONFIGURE was
    /// seen; the caller then reconciles in the same pass.
    pub fn pump(&mut self) -> bool;
    pub fn len(&self) -> usize;
}
```

**Data sources — the §13.7 file-walk bias, no new verb.** `troot` is a
second `/srv/tapestry` root fd (the console `Surface` keeps its own
private). `layout` gives the visible leaves (lines `<id>[*] leaf ...`; a
trailing `hidden` is skipped; `*` marks the focused leaf); `pane/<id>/tagbar`
gives the strip as `x y w h` (ZERO = bar-free: a single fullscreen leaf, or
one too short to spare it); `pane/<id>/tag` gives the NAME. halcyond names its
own pane once through that file (`"halcyon"` — HALCYON-VISUAL §4.1: the name
is "the tile's program"; §13.6 names the tag file as the source); every other
tile shows its `tag` or nothing.

**The reconcile.** Diff the wanted set (every visible leaf with a non-ZERO
strip) against the live tiles: gone or bar-free → drop (a dropped `Surface`
closes its conn and the compositor retires the surface; pane ids are never
reused, so a stale key can only mean gone); new → `Surface::chrome_on(id, w,
h)` (a failure is said and skipped — the next relayout retries); kept →
repaint (focus and names move; a same-size relayout also blanked the strip to
the compositor's resting fill).

**When it runs.** Only after the console's first successful present — the
scanout is first-present-wins and chrome must never precede it — and then on
every main-surface `TEV_CONFIGURE` (any structural relayout fans one to every
visible hosted surface) or `TEV_FOCUS`, and whenever `pump` saw a CONFIGURE on
a tile. A focus move is a focus-only epoch that fans no relayout CONFIGURE, so
the compositor's focus-only branch fans the visible chrome surfaces a
same-size CONFIGURE (the redraw request, coalesced by replacement); that is
the wake that keeps the "resting, active tile" separator on the focused leaf.
There is no timer.

**The paint** (HALCYON-VISUAL §4.1/§4.2/§4.3), one cartoon list per strip:
`Op::Clear{header}`; `Op::Rect` on the bottom row = the separator
(`ember_deep` on the focused leaf's tile, else `border`); the name as one
glyph run in `FACE_BODY` at 10.5 px, x = `METRICS.tag_pad_x`, baseline centred
via `line_metrics` (`fg` on the focused tile, `fg_muted` resting); executed
into `surf.pixels()` exactly as the transcript renders, then `present(None)`.
`pump` never paints: painting on a CONFIGURE before the reconcile re-reads the
layout would flash the stale state.

**Events.** `FRAME` is a droppable, coalesced class and `CONFIGURE` coalesces
by replacement, so an idle tile cannot wedge; `KEY`/pointer never reach chrome
(not focused; `surface_at` walks leaf content rects). `TEV_CLOSE` or a dead
stream marks the tile for the next reconcile.

## Tests

- Host: 35 lib tests (the table above) — all `--offline` against the
  vendored registry.
- In-guest: `tools/interactive/ls-halcyon.exp` — GATED on the baked lever
  (`THYLACINE_HALCYON=1` bakes `/lib/halcyon/renderer`; the scenario SKIPs
  cleanly on a default image, the cfg-4 precedent). Proves: joey's choice,
  the rich advertisement, login→ut through the transcript, the
  parchment-dominant screendump + ink, the rich canary + zone/obj/table
  frames on the wire, the split/zoom reflow round-trip (real QMP Super+H /
  Super+F through the compositor), and F10's silence.
- The default image's suite (1435/1435) pins the joey default arm
  (`aurora spawned`, no config).

## Status

H-2 (the transcript MVP on the CPU floor) as of the H-2d close; H-3b-3
added the per-leaf tag-bar chrome (DISPLAY-only; the section above). Not yet:
raw-VT panes (H-3; `raw_vt_intent` latches today), menus (H-3c),
layouts (H-4), compose (H-5), the vk executor + the display-list wire
(H-6), images/`Embed` (H-7). Damage-rect presents are a recorded
optimization (v0 presents full frames).

## Caveats / footguns

- `GlyphSource::regen()` must be the ONLY eviction: cache, table, and
  pages move together, and the executor's gen check is the belt.
- The layout cache keys (width, sheet.gen, atlas gen) by block id; the
  open block + pending line NEVER cache.
- The winsize report is the MONO-grid equivalent of the pixel size —
  programs wrap to columns; foreign content is mono, so that is the
  terminal-compatible answer.
- The consfeed held-queue discipline is aurora's #129/#135/#136 verbatim;
  its policy lives in `input.rs` so the host tests pin it — do not inline
  a "simpler" retry loop in the bin.
