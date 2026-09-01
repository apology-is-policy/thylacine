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
| `chrome` | the tag-bar RULES (H-3b): the `layout`/rect parsers, the §4.2 key derivation `key_for(focused, status)`, the per-key colours, the strip display list `strip_list` | 5 (leaf parse incl. hidden/malformed, rects, the key table, the colour table, the list shape) |

The bin adds `chromeset` (`src/chromeset.rs`, `mod chromeset;` in main.rs): the
tag-bar SURFACES — one `Role::Chrome` conn per strip, the pane-tree reads, the
event pump, the paint into `surf.pixels()`. It is the syscalling half of
`chrome` and nothing else; H-3b-3 first put both halves in the lib, which broke
the lib's host-test build (the recipe above) until H-3b-4 split them.

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

## The chrome (H-3b-3/H-3b-4): the per-leaf tag bar

`src/chrome.rs` (lib, the rules) + `src/chromeset.rs` (bin, the surfaces) —
halcyond owns one `Role::Chrome` tapestry surface per visible leaf that
carries a Daylight tag-bar strip, paints the whole strip, and the compositor
PLACES it at the leaf's `tagbar` rect (the H-3b-2 `surface_target` arm;
`create W H role=chrome bind=<pane-id>` is renderer-gated, and halcyond is
spawned with `T_SPAWN_PERM_CONSOLE_RENDERER` by joey's renderer-choice block).
DISPLAY-only: pills are commands (H-3c).

```rust
// lib: halcyond::chrome
pub struct Leaf { pub id: u32, pub focused: bool, pub surface: Option<u32> }
pub fn parse_leaves(layout: &str) -> Vec<Leaf>;         // "<id>[*] leaf ..." lines; `hidden` skipped
pub fn parse_rect(s: &str) -> Option<(u32, u32, u32, u32)>;
pub enum Key { Resting, Sage, Cinnabar }                 // the section 4.2 rows
pub fn key_for(focused: bool, status: &str) -> Key;     // (live, last exit) -> key
pub fn key_colors(key: Key) -> (Argb, Argb, Argb);      // ground, separator, name ink
pub fn strip_list(key: Key, name: &str, w: u32, h: u32, gs: &mut GlyphSource) -> Cartoon;
// bin: chromeset
pub struct ChromeSet { /* BTreeMap<pane id, Tile{ surf, key, name, dirty, dead }>, own_pane */ }
impl ChromeSet {
    pub fn new() -> ChromeSet;
    /// The leaf hosting the console surface (the status verb's target).
    pub fn own_pane(&self) -> Option<u32>;
    /// Bring the tiles in line with the layout; paints every dirty tile.
    pub fn reconcile(&mut self, troot: i64, own_surface: u32, gs: &mut GlyphSource);
    /// Drain every tile's events (non-blocking). True = a CONFIGURE was
    /// seen; the caller then reconciles in the same pass.
    pub fn pump(&mut self) -> bool;
    pub fn len(&self) -> usize;
}
```

**Data sources — the §13.7 file-walk bias, no new read verb.** `troot` is a
second `/srv/tapestry` root fd (the console `Surface` keeps its own
private). `layout` gives the visible leaves (lines `<id>[*] leaf ...`; a
trailing `hidden` is skipped; `*` marks the focused leaf); `pane/<id>/tagbar`
gives the strip as `x y w h` (ZERO = bar-free: a single fullscreen leaf, or
one too short to spare it); `pane/<id>/tag` gives the NAME (every file is read to EOF, never by one bounded read); `pane/<id>/status`
gives the tile's RECORDED last-command status (`resting|ok|err`), read only for
the focused leaf — the only tile a status can show on. halcyond names its own
pane once through the `tag` file (`"halcyon"` — HALCYON-VISUAL §4.1: the name
is "the tile's program"; §13.6 names the tag file as the source); every other
tile shows its `tag` or nothing.

**The states (§4.2) and the one authority.** `key_for(focused, status)`: the
LIVE tile — the focused leaf, the one tile holding input — is `Sage` (exit 0,
or nothing has run yet) or `Cinnabar` (the recorded status is exactly `err`);
every other leaf is `Resting` — a resting pane's sole tile, "the tile a
resting pane would return to" (header ground, `ember_deep` separator, `fg`
name). The plain Resting row (border separator, muted name) belongs to a
stack's collapsed tiles, which do not exist before tile stacking lands. The
key is derived from the COMPOSITOR's record (the `status` file), the same
record its live hairline reads, never from a private copy: strip and hairline
cannot disagree.

**The status feed (H-3b-4).** The transcript latches the exit code of each
completed command from its `exit` mark (`Transcript::take_exit`; a latch, not
a queue — only the LAST exit is the tile's status). The event loop's chrome
step sends it as `tag <own-pane> status ok|err` through `Surface::global_ctl`
on the CONSOLE surface's conn (the gate reads the conn's kernel-stamped peer,
and this process holds the renderer role), then reconciles so the strip
re-reads the record. Held while the console is not yet up or the own pane is
not yet known; a newer exit replaces an unsent older one. Display-only: a
refusal drops that exit and is said once (`halcyond: tag status refused`);
the next exit mark tries again (the round's F4: a one-shot latch had turned
one transient refusal into a session-long loss of the live key).

**The reconcile.** Diff the wanted set (every visible leaf with a non-ZERO
strip) against the live tiles: gone or bar-free → drop (a dropped `Surface`
closes its own files, the shared session stays, and the compositor retires
the surface; pane ids are never reused, so a stale key can only mean gone);
new → `Surface::chrome_on_shared(troot, id, w, h)` — minted on the pane-tree
session, never on a session of its own (the H-3b round's R2-F2: a session per
bar exhausted the compositor's conn pool at three windows and turned every
further mint into a blocking connect inside this loop; the renderer's
per-conn surface cap is widened by `MAX_PANES` server-side); a failure is
said once per pane and retried on the next reconcile (it fails fast at the
mint); kept → repaint (focus and names move; a same-size relayout also
blanked the strip to the compositor's resting fill). A tile whose bound pane
closed is told by the compositor (TEV_CLOSE) and dropped on the next pump.

**When it runs.** Only after the console's first successful present — the
scanout is first-present-wins and chrome must never precede it — and then on
every main-surface `TEV_CONFIGURE` (any structural relayout fans one to every
visible hosted surface) or `TEV_FOCUS`, and whenever `pump` saw a CONFIGURE on
a tile. A focus move is a focus-only epoch that fans no relayout CONFIGURE, so
the compositor's focus-only branch fans the visible chrome surfaces a
same-size CONFIGURE (the redraw request, coalesced by replacement); that is
the wake that keeps the "resting, active tile" separator on the focused leaf.
There is no timer.

**The paint** (HALCYON-VISUAL §4.1/§4.2/§4.3), one cartoon list per strip
from `strip_list`: `Op::Clear{ground}`; `Op::Rect` on the bottom row = the
separator; the name as one glyph run in `FACE_BODY` at 10.5 px, x =
`METRICS.tag_pad_x`, baseline centred via `line_metrics` — ground / separator
/ ink per `key_colors` (Resting: header / ember_deep / fg; Sage: sage tint /
sage / sage fg; Cinnabar: cinnabar tint / cinnabar / cinnabar fg); executed
into `surf.pixels()` exactly as the transcript renders, then `present(None)`.
`pump` never paints: painting on a CONFIGURE before the reconcile re-reads the
layout would flash the stale state.

**Events.** `FRAME` is a droppable, coalesced class and `CONFIGURE` coalesces
by replacement, so an idle tile cannot wedge; `KEY`/pointer never reach chrome
(not focused; `surface_at` walks leaf content rects). `TEV_CLOSE` or a dead
stream marks the tile for the next reconcile.

## Tests

- Host: 48 lib tests (the table above; incl. the round's F1 regression
  `open_block_freezes_on_bytes_so_the_budget_can_evict_it` — the OPEN block
  now freezes on bytes, `max_open_cost` = budget/8, so the budget's
  frozen-only eviction can reach a newline-free stream; before it 320 MiB
  could accrue in one open block against a 64 MiB heap) — all `--offline` against the
  vendored registry.
- In-guest: `tools/interactive/ls-halcyon.exp` — GATED on the baked lever
  (`THYLACINE_HALCYON=1` bakes `/lib/halcyon/renderer`; the scenario SKIPs
  cleanly on a default image, the cfg-4 precedent). Proves: joey's choice,
  the rich advertisement, login→ut through the transcript, the
  parchment-dominant screendump + ink, the rich canary + zone/obj/table
  frames on the wire, the split/zoom reflow round-trip (real QMP Super+H /
  Super+F through the compositor), and F10's silence. Since H-3b: the tag
  bars on a split — the pre-split control, the header-with-ink strip, the
  §4.2 keys following focus (resting `ember_deep` vs live sage on strip AND
  content hairline, the top hairline row vanishing into the live tint), a
  failing command (`pwd; false`) keying the live tile cinnabar and a clean
  one keying it back, and zoom dropping the bars (parchment at the top).
- ls-gfx-panes (default image, the battery as a NON-renderer): the negative
  twins — `role=chrome` create → E_PERM, `tag <A> status err` → E_PERM with
  the pane's `status` file still `resting` after the refusal.
- The default image's suite (1435/1435) pins the joey default arm
  (`aurora spawned`, no config).

## Status

H-2 (the transcript MVP on the CPU floor) as of the H-2d close; H-3b-3
added the per-leaf tag-bar chrome and H-3b-4 the live-tile keys + the status
feed (DISPLAY-only; the section above). Not yet:
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
