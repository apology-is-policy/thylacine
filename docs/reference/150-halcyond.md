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
reader. Since KT-1.5d-1a (HALCYON.md §14.12) halcyond has a **second role** —
the per-user session compositor, `login`-spawned with `--session`, which holds
NO console-renderer role (see "The `--session` variant" below).

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

## The `--session` variant: the per-user compositor bootstrap (KT-1.5d-1a)

halcyond has TWO roles, selected by `rs_main`'s first act — a `--session`
operand (`env::args().operands()`):

- **No `--session` (the default, joey-spawned):** the console renderer above —
  it opens the `/dev/consdrain`/`consfeed`/`consctl` trio, holds the
  `g_console_renderer` role, and weaves the `/dev/cons` transcript.
- **`--session` (login-spawned, HALCYON.md §14.12):** the **per-user session
  compositor**. `/sbin/login` spawns `/bin/halcyond --session` AS the
  authenticated user (identity only — no `CAP_SET_IDENTITY`, no
  `SPAWN_PERM_CONSOLE_RENDERER`; zero identity delegation, the only
  identity-stamp is login's). It holds NO console-renderer role: `session_main`
  skips the whole `/dev/cons` trio and never reads the console mirror. It
  connects to the SYSTEM tapestryd (`/srv/tapestry`) as an ordinary-user
  `Actor::Session` (connecting is ungated) and presents a fullscreen surface,
  hosting the session's terminals as `kaua-term` processes it spawns as itself
  (§14.2), ingesting each tile's record stream through the `Tile` model below
  instead of the console drain.

**The bootstrap (d-1a).** `session::run` connects + takes a fullscreen surface
(the connect/surface/present primitives — `EventRing::connect_sqpoll` +
`Surface::fullscreen_on` + the first-present-before-wait discipline + the SQPOLL
unified-poll wait on `ring.poll_fd()`). The witness is `halcyond: session up
NxN px`, printed only inside `Ok(present)`, so it proves connect + present, not
merely that the line ran. The aurora → `Direct(halcyond)` handoff is d-1b
(tapestryd's reconcile backgrounds the console renderer, §14.12 step 4; no new
primitive).

**Multiple session tiles (d-3).** `session::run` (in `session.rs` — the I/O half
of the 13.1 lib/bin split; the pure create/drop diff is host-tested in
`halcyond::tiles::plan_tiles`) hosts ONE `kaua-term` + content `Surface` + `Tile`
per compositor leaf, keyed by leaf id in a `BTreeMap`, reconciled off the
`layout` file each relayout. d-2 was the single-tile special case of this loop.
Each `kaua-term` runs `/bin/ut` AS the session (plain `t_spawn`, no cap, zero
delegation), `Stdio::Piped`: the child's fd 0 = the down pipe (halcyond writes
`Key`/`Resize` `Input` frames), fd 1 = the up pipe (the child writes the `Record`
stream), fd 2 inherits; the pts is internal to the child, so `ut` never sees
these pipes. The loop:

- **reconciles** on every relayout (a `TEV_CONFIGURE` on any tile): reads the
  `layout`, `plan_tiles` diffs the visible leaves against what it hosts. A new
  EMPTY leaf it owns is CLAIMED (`pane/<id>/claim`, the server-side owner +
  emptiness authority, §13.7 — a failed read means "not ours", skip) and hosted
  with `Surface::open_claim_on` at the leaf's `pane/<id>/geometry` size, then a
  `kaua-term` is spawned into it. A vanished leaf's tile is reaped. The bootstrap
  root is the first entry, keyed on the leaf hosting the fullscreen surface.
- **renders** each dirty `Tile` (grid tail + scrollback flow, `Tile::render`
  below) via the reused render brain (a `GlyphSource` + the Daylight sheet +
  cartoon); the FIRST render precedes any wait (first-present-wins).
- **waits** on ONE `poll { ring.poll_fd() | up_0 .. up_N }` — a surface event on
  the ring (`poll_event` pumps the SQPOLL CQEs, demuxed per surface) or records
  on any LIVE tile's up pipe (a crashed/exited tile's pipe is skipped).
- **ingests** each readable tile: one `t_read` per wake → `FrameDecoder` →
  `parse_record` → `Tile::apply` — the trust boundary (a bounds-checked wire).
- **routes input to the focused tile FOR FREE**: tapestryd delivers `TEV_KEY`
  only to the FOCUSED surface (`key_event` → `layout.focused_surface`), so
  draining each tile's surface events and forwarding its `TEV_KEY` (via
  `input::map_key` → an `Input::Key` frame down THAT tile's pipe) is inherently
  focus-routed — halcyond never reads `l.focused` (§14.11.9). The Super chord
  layer (split / focus-move / zoom / close, §18.4) is the compositor's; halcyond
  only reacts to the layout changes it produces.
- **resizes** per tile on `TEV_CONFIGURE`: recompute cols/rows, resize the tile
  grid, send `Resize` down (the kaua-term sets the pts winsize + replies with a
  full CellDiff).
- **contains a tile's death** (§14.11.10): a `Control::Exit(0)` (clean) CLOSES
  the leaf (a `close <leaf>` layout verb — the pane collapses, tmux-style) and
  reaps the tile; a `WireError` / non-clean exit / abnormal EOF (a crash of the
  isolated parser) FREEZES the tile as an affordance (its last frame held, its
  pipe skipped, its `kaua-term` killed), reaped when the user closes the leaf.
  Neither ends the environment. A `closed` leaf-id set is the permanent respawn
  guard: a leaf whose tile is gone is never re-filled (leaf ids never reuse).
- **logs out** when the LAST tile is gone — `session::run` returns, `login`'s
  `wait()` returns → getty → the session leaf retires → aurora un-backgrounds +
  resumes (d-1b).

The witnesses on the diagnostic UART: `session tile leaf=N spawned pid=M WxH`
(per tile), `session up NxN px` (the first successful present), `session tile
ingest live` (the first record applied), `session tile leaf=N exited (code C)
-- closing` (a clean tile close) / `session tile leaf=N crashed -- affordance
held` (a contained crash), and `session logout (code C)` (the last tile gone).

**The session lever.** `login` reads the one-token `/lib/halcyon/session` file:
`on` → the session variant; absent / any other token → the proven `ut` path
(fail-safe, exactly as joey's renderer lever). Distinct from
`/lib/halcyon/renderer` (joey's system-renderer choice, being retired at
§14.12). Baked under `THYLACINE_HALCYON_SESSION=1`; the `ls-gfx-session`
scenario probes the post-login markers + SKIPs cleanly on a default image.

## The tile model (KT-1.5b-ii): the live grid + the scrollback ingest

The data flow above is halcyond's **console** path (one `/dev/consdrain` byte
stream). The **multi-console terminal** (KT-1.5) gives each leaf its own hosted
`kaua-term` process, and halcyond ingests a pre-digested **record** stream per
leaf rather than raw bytes. (This "tile" is the leaf's terminal CONTENT --
distinct from the H-3b chrome tag-bar surfaces, which are also per-leaf and
also called tiles.)

`tile.rs` holds one `Tile` per leaf: a **live grid** + a **scrollback
`Transcript`** (HALCYON 14.11.1). They are separate because the grid spans zone
boundaries -- the last `rows` lines routinely straddle a prompt. The record ->
model dispatch (`Tile::apply`, 14.11.2):

| Record | applied to |
|---|---|
| `CellDiff{changed,cursor}` | the live grid (`grid.rs`) |
| `ScrollOff{rows}` | `Transcript::push_scrolled_rows` (history) |
| `Control(Osc1936Raw)` | `Transcript::feed` -- the SAME beacon parser the console uses |
| `Control(Title/Exit/Bell/WinsizeAck)` | tile fields |
| `Mode(Normal\|AltScreen)` | the render mode |

`grid.rs` is a fixed rows x cols `vt::Cell` buffer -- what `CellDiff` mutates,
running no VT parser (the kaua-term already did). halcyond is the geometry
authority, and a tile is untrusted (14.11.12, the format-fuzz class): an
out-of-bounds cell write is DROPPED and the cursor is clamped on read, so a
buggy/hostile producer cannot index past the buffer.

Reusing `Transcript::feed` for the beacon frames keeps the format-fuzz surface
(parsing an untrusted per-tile stream) ONE audited parser, not N. The ScrollOff
path interns a pre-styled `vt::Cell` via the same `intern_style` `style_idx`
uses, and charges the same block-cap / eviction cost, so a tile that scrolls
forever stays bounded.

**The model landed host-tested at KT-1.5b-ii-a** (grid + tile tests). **The
render + spawn + poll-fold landed at KT-1.5d-2**, inside the per-user
`session_main` (above): the child spawn (one `kaua-term` over a `Stdio::Piped`
pair), folding the up-pipe into the unified `poll { ring | up_pipe }`, and the
render below.

`Tile::render(cart, w, h, gs, sheet, scroll_up) -> content_h` composes the
frame (14.11.3):

- **Alt-screen** (`Mode(AltScreen)`, e.g. `vim`): the live grid alone, full-tile
  from the top-left. Returns the grid height.
- **Normal**: the scrollback flows above (each block laid once via `layout_block`
  and rendered via `render_block` — the SAME proportional path the console
  transcript uses), and the live grid renders as a fixed-height mono tail
  (`grid_rows * cell_h`) at the bottom. The content `[scrollback][grid]` is
  bottom-anchored, raised by `scroll_up` px (0 = the grid sits at the view bottom,
  history off the top; scrolling reveals history — the wheel input is a follow-on,
  `scroll_up` is wired but held at 0 in d-2). Returns the total content height.

The grid painter (`paint_grid`) is a mono cell store: per-cell it paints a bg
Rect only when the cell bg differs from the sheet ground (a blank Daylight grid
is all-ground, so it emits only the cursor beam), then the `FACE_MONO` glyph
(space/NUL skipped), then an underline Rect; `ATTR_REVERSE` swaps fg/bg. The
block cursor beam (2 px, the accent) sits at the clamped cursor cell. Cells carry
resolved RGB in the compositor's palette (the kaua-term stamps `vt::DAYLIGHT`,
§14.6 — the palette is applied at the producer since the seam ships RGB, not
re-mapped here), so the tile composites coherently with the Daylight transcript.

`input::map_key(code, rune, value) -> Option<KeyEvent>` maps a tapestryd
`TEV_KEY` to a kaua `KeyEvent` for the down-channel: a composed `rune` (tapestryd
Ctrl-folds + shift-resolves) passes through as `Char(rune)` (byte-identical to the
console `key_bytes` rune arm); a rune-less nav key (arrows / Home / End / page /
Del / Ins) maps to its `KeyCode` so the kaua-term honors DECCKM; a release
(`value 0`) or an unknown key is `None`.

All of `Tile` + `render` + `paint_grid` + `map_key` are host-tested (the lib
half), so the format/render logic is exercised without a boot.

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

## The menu (H-3c): obj runs, the verb table, the summon

HALCYON.md 13.6 "Menus -- THE GATE" + "obj interaction", BEACON.md 7 as
built (2026-09-02). Same split as the chrome: `halcyond::menu` (lib, pure,
host-tested) thinks; the bin's `menuset.rs` owns the surface.

**Obj runs (`menu.rs`).** A run is the cells of one flat row sharing one
obj index (`Style.obj`, idx+1 into `Block.objs`); the index is minted per
`obj` frame and never shared, so it IS the run's identity.
`runs_on_row(t, fr)` walks a Line's cells or a table row's cells in order;
`obj_of(t, block, obj)` yields the (type, resolved ref); `step_run` moves
the selection `w`/`b` across rows; `run_rect(laid, item, row, obj)` is the
run's union rect over the laid block's (possibly wrapped) lines;
`hit_run(laid, x, y)` is the inverse for a click. `Sel.obj` (select.rs) names
the selected run on the cursor row and clears on every row motion; the
render pass underlines it 2 px in `ember` (`run_mark`).

**The verb table.** `beacon::verbs::parse` over `/lib/beacon/verbs` (read
once at start, said as `N verb rules loaded`); `build_menu(rules, ty, ref)`
expands each typed rule into a `Command` (the rc-quoted ref substituted) or
an `Internal` action (`#...`, admitted only under the `test-mode` feature).
`menu_size` + `menu_list` produce the Daylight list: `raised` ground,
`border` 1 px stroke, the type (proportional 10.5 px, `fg_muted`) + the
RESOLVED ref (monospace, `fg`) on the title row -- the anti-clickjack line
-- a `border` rule, one monospace row per verb, the selected one on a
`header` band (`fg_dim` for an internal action). `menu_key` maps
Up/Down/j/k/Enter; `Menu::key` moves clamped and yields the chosen action.
Test builds (the `test-mode` feature, default on) also say `act: no obj run
...` / `act: obj N unplaced ...` on a silent no-op, `run at X Y W H rowh R`
on `w`/`b`, `menu N configure -> repaint` and `menu N present failed` in the
pump (the compositor says `menu N present slot S visible V` and `key C
dropped (no focused surface)`).

**The summon (main.rs).** The render pass keeps `frame` = (block id, screen
y, height) per painted block (u64::MAX = the open block) and the open
block's last layout; keyboard: Normal `w`/`b` -> `step_run`, Enter -> the
selected run (the row's first when none) -> `run_rect` + the frame's y ->
`summon` (surface coords + the console pane's `geometry` origin = display
coords) -> `MenuSet::open`; click: `TEV_PTR_MOVE` tracks the pointer,
`TEV_PTR_BTN` (left press) -> the frame block under the pointer ->
`hit_run` -> the same summon at the pointer. `MenuSet::open` mints
`Surface::menu_on_shared(troot, w, h)`, writes `menu place <id> <x> <y>` on
that session (the renderer's peer; the pane-tree key), THEN paints + presents
once (a present before the place composes nowhere; a bare second present
would show the next slot's zeros -- the slots rotate per present -- which is
how the lever first showed a black menu), and says `halcyond: menu N placed
at X Y (WxH) for <type> <ref> run at RX RY RW RH` (the REQUESTED display
coords; the compositor's own line carries the clamped rect). `service(gs,
block_first)` drains the menu's own stream each pass and, while the menu is
up, WAITS on its ring first: a 9P session's replies are read only by a
thread inside a wait/RPC on that session (ARCH 8.8.1.1), the menu and the
chrome tiles live on the pane-tree session, the console on its own -- a loop
parked on the console's ring never saw a menu key, and a tile's CONFIGURE
landed only at the next reconcile's troot reads. The wait is bounded by the
menu's FRAME ticks (>= the idle rate) and the dismiss's EOF; step (1) polls
the console non-blocking meanwhile. Esc drains the console mirror before it
freezes the cursor row (keys are serviced before the drain in a pass).
**The observer effect** (the witnesses): every diagnostic line halcyond or
the compositor prints is mirrored into the console transcript and scrolls
it, so a run rect reported earlier no longer holds; test builds say the
selected run's CURRENT display rect + the mono row height on `w`, and the
click leg subtracts that one row. `pump` drains the menu's
own stream each pass: keys move/choose, CONFIGURE repaints, CLOSE or a dead
stream = the compositor dismissed it (`menu closed by the compositor`). A
`Command` choice: `close` (the owner's `menu dismiss`, then Drop's
`destroy`), then the command + newline is fed to the console and the mode
returns to Insert. An `Internal` `#wedge <ms>` (test-mode): the loop sleeps
with the menu still placed -- THE GATE's wedged-owner lever -- and says
`wedge-test: frozen ... / woke`.

**The event set (H-3c-2, 2026-09-02).** halcyond opens ONE
`tapestry::EventRing` (one session + one Loom ring) and every surface it
owns lives on it: the console (`Surface::fullscreen_on`), each tag-bar
tile (`chrome_on`), the menu (`menu_on`); the pane-tree files are read on
the ring's session root. Since KT-1.5b-i (2026-09-04) the ring is SQPOLL
(`EventRing::connect_sqpoll`), so a kernel poll-thread drives the session
reader and posts completions off-thread. The loop's step (1) takes the
console's next event or, when its queue is empty, blocks in ONE `poll(2)`
over the ring fd (`EventRing::poll_fd`) AND `/dev/consdrain` -- `poll`
reports POLLIN for any surface's event exactly as the old `EventRing::wait`
woke on any completion, AND for console output, so shell output wakes the
renderer at once instead of at the next compositor frame tick (the old
`EventRing::wait` blocked on the ring alone; the console mirror was drained
per-pass only after some other event woke the loop -- the frame-coupled
console latency). The pumps (`ChromeSet::pump`, `MenuSet::service`) drain
their surfaces' queues every pass. This retires
the H-3c session-reader dance (`service(block_first)` waiting on the
menu's own ring while the console was polled) and FIXES the H-3b-3 tiles'
latency: a tile's CONFIGURE used to land only at the next pane-tree RPC
(the console's session was the one the loop waited on, and a Loom wait
pumps one session), so a focus move between two non-console panes
re-keyed no tag bar until a later relayout. Witness: the lever's 3-leaf
leg (Super+Left/Right between the two non-console panes; both bars swap
live/resting with no event to the console).

**The H-3c-2 audit close (2026-09-02).** The ring's routing (now
`libtapestry`'s `ring` module, host-tested) ends a stream on an errored
read as it does on EOF, so a dead compositor reaches the loop's
"compositor gone; exiting" arms instead of livelocking it (the round's F1);
`NormalAct::Act` / `Paste` / `ToggleSelect` fire on a key's PRESS only --
the compositor routes a repeat to the surface that saw the press, so a held
Enter used to re-summon the menu at the autorepeat rate (F6). See 139
"THE EVENT SET" for the ring-side changes.

**The status bar (H-3d, 2026-09-02).** `halcyond::status` (the lib: the
four-slot cartoon `status_list(model, w, h, gs) -> (Cartoon, Slots)`,
`condition_for` off the pane's status text, `context_text` -- name `·`
directory `·` command, empties dropped -- `bar_height` = the theme's
`status_h`; host tests pin the slot order, the colours, the truncation)
+ `statusset` (the bin: ONE `Surface::status_on` on the ring, minted once
the console is up (0d'), pumped per pass (CONFIGURE -> repaint; CLOSE ->
re-mint), `model_from(focused, own_pane, cwd, cmd)` -- the focused leaf's
name + status from `ChromeSet::focused()` (recorded at reconcile), the
transcript's `cwd()` + `last_command()` when that leaf hosts the console,
`clock_hm()` UTC -- and `refresh` paints only on a change, and in test builds
says the four slot rects + the context + the condition when the slot
GEOMETRY changes -- never per paint: every say line lands in the
transcript, and a line after every command shifted the row the H-3c
keyboard leg keys on; the witness needs the rects when they move). The transcript scans OSC 7 (a bounded body; `pct_decode_path`)
and records ut's `mark k=cmd` on the output block (`Block.cmd`).

**The audit close (2026-09-02).** `menu_size(m, gs, max_h)` caps the
surface height at the display (`display_h` reads `ctl`'s `display W H` on
the pane-tree session; the round's F3: the compositor refuses a taller
surface, so a verb-rich type opened NO menu at all) and `item_window`
keeps the selection inside the rows that fit, so a long list scrolls --
j/k/Up/Down, and the wheel (`Menu::wheel` on a SCROLL, whose delta the
compositor sums in the menu's queue). A `Command` choice feeds `^E ^U` +
the command + newline: ut's line editor takes them as CursorEnd +
KillToStart, so a draft half-typed at the prompt moves to the kill buffer
(^Y restores it) instead of being run INTO (`echo fo` + the verb typed
`echo fols -l ...`); a canonical reader sees VKILL, a raw-mode program two
keys. The system-tier templates carry `--` where their programs take it.

## Tests

- Host: 55 lib tests (the table above; H-3c added `menu.rs`'s six: runs per
  obj index in cell order incl. a table cell, stepping across rows both
  ways, `run_rect`/`hit_run` agreeing on the laid geometry, the menu
  showing the resolved ref + typed verbs with an unquotable ref keeping only
  internal actions, clamped keys + Enter, the raised/bordered list growing
  with items; the audit close added the seventh: a 40-verb list caps at the
  display height, its window ends at the selection, the selected band and
  the drawn rows stay inside the surface, the wheel moves and clamps; `input.rs` pins `w`/`b`/Enter; incl. the round's F1 regression
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
  Since H-3c: Enter on a path run opens the menu (the placed rect's dominant
  colour is Daylight `raised`, the ref a resolved `/lib/aurora/` path), Esc
  dismisses it compositor-side (`tapestryd: menu N dismissed (esc)`) and the
  rect heals to parchment; in the split, a real QMP tablet click on the
  run's display rect opens the same menu at the pointer and a click outside
  it dismisses it (`(click-away)`) with NO second placement (the swallowed
  press); THE GATE: the lever's `wedge-test` rule freezes halcyond 6 s with
  the menu up, Esc dismisses it meanwhile, `ipwd` typed over QMP during the
  freeze runs after the wake (`/home/michael`), the rect heals, and no
  `WEDGED` line exists; the audit close's legs: the click-away's press AND
  release are both reported swallowed by the compositor (the lines are
  printed inside the swallow branches), and the command path -- a draft
  half-typed at the prompt, then `ls` chosen on an `ls /lib/aurora` row's run (pwd's output is plain text, no obj):
  `menu ran: ls -l -- '/lib/aurora/<ref>'`, the draft's echo present, `halfls`
  absent, the global operand-error control proving ls took `--`; the event
  set's leg (H-3c-2): a second split makes three leaves and Super+Left /
  Super+Right between the two non-console ones re-key both tag bars each
  way (their rects read off the pane tree through the console); after the
  zoom `cat /dev/tapestry/ctl` reads `surfaces 1` (the dropped bars + every
  menu retired server-side).
- ls-gfx-panes (default image, the battery as a NON-renderer): the negative
  twins — `role=chrome` create → E_PERM, `tag <A> status err` → E_PERM with
  the pane's `status` file still `resting` after the refusal; H-3c:
  `role=menu bind=1` → E_INVAL, `role=menu` → E_PERM, `menu place/dismiss/
  bogus` → E_PERM with `menu none` in the ctl read after.
- The default image's suite (1435/1435) pins the joey default arm
  (`aurora spawned`, no config).

## Status

H-2 (the transcript MVP on the CPU floor) as of the H-2d close; H-3b-3
added the per-leaf tag-bar chrome, H-3b-4 the live-tile keys + the status
feed, H-3c the obj verb menu (keyboard + click; the compositor-owned dismiss
proven against a wedged owner). Not yet: raw-VT panes (H-3; `raw_vt_intent`
latches today), the status bar (H-3d), the session-tier verbs (the settings
push), layouts (H-4), compose (H-5), the vk executor + the display-list wire
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
- ADDRESSED by KT-1.5b-i (the H-3c-2 round F7, pre-existing): the held-feed
  path (`wait_is_bounded`) polls and sleeps; on the OLD non-SQPOLL ring a
  submit-only Loom enter demuxed nothing, so while the feed was held and
  nothing else made an RPC on the session, the console's parked read reply
  sat undemuxed and every KEY typed queued server-side until the compositor's
  128-event cap WEDGE-retired the console (a held key for ~4 s in front of a
  silent, non-reading foreground). The honest primitive named here -- a timed
  Loom enter -- is exactly what the SQPOLL ring now provides: its kernel
  poll-thread drives the session reader on a frame-boundary deadline
  (`p9_client_reader_pump_once_deadline`) and demuxes the parked reply
  continuously, independent of halcyond's loop branch, so the held-feed path
  no longer starves the console's demux. A targeted repro (feed held + silent
  foreground + key flood, asserting no wedge-retire) is owed at the KT-1 audit
  to confirm the close.
  `memory/bug_held_feed_path_never_demuxes.md`.
