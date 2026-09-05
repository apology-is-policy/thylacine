---
id: sub-halcyond
type: sub
title: "halcyond — the Halcyon environment client: the transcript renderer and the per-user session compositor"
parent: moc-userspace-shell-tui
code:
  - usr/halcyond/src/lib.rs
  - usr/halcyond/src/main.rs
  - usr/halcyond/src/transcript.rs
  - usr/halcyond/src/layout.rs
  - usr/halcyond/src/raster.rs
  - usr/halcyond/src/input.rs
  - usr/halcyond/src/select.rs
  - usr/halcyond/src/chrome.rs
  - usr/halcyond/src/chromeset.rs
  - usr/halcyond/src/menu.rs
  - usr/halcyond/src/menuset.rs
  - usr/halcyond/src/status.rs
  - usr/halcyond/src/statusset.rs
  - usr/halcyond/src/session.rs
  - usr/halcyond/src/tile.rs
  - usr/halcyond/src/tiles.rs
  - usr/halcyond/src/grid.rs
  - usr/halcyond/src/downq.rs
  - usr/halcyond/Cargo.toml
audit: hard
guarded-by: []
validated-by: [prose, gate-interactive]
locks: []
hazards: [haz-budget-stored-not-derived]
abis: []
design: ["docs/HALCYON.md", "docs/BEACON.md", "docs/KAUA-TERM.md"]
created: 2026-09-05
updated: 2026-09-05
---
## Purpose

`halcyond` is the rich-transcript console renderer -- the first inhabitant of
the HALCYON.md process architecture, "the only place that thinks". It owns the
transcript state, the Beacon parse ([[sub-beacon]]), the SGR subset, layout,
the fontdue rasterizer + atlas cache, the paper-light stylesheet, and the
per-frame **cartoon** display list ([[sub-cartoon]]) that an in-process CPU
executor weaves into a [[sub-libtapestry]] surface. It is the format-fuzz
frontier for the display: every byte it renders is untrusted app output.

## Contract

**Two roles, selected at startup by a `--session` operand.**

- **The joey-spawned console renderer** (the default; the G-4 slot,
  `T_SPAWN_PERM_CONSOLE_RENDERER`) is chosen when `/lib/halcyon/renderer` reads
  `halcyond` -- anything else (absent, short read, unknown token) fail-safes to
  [[sub-aurora]]. It opens the `/dev/cons` drain/feed/consctl trio, holds the
  console-renderer role, advertises `beacon rich` on consctl, and weaves the
  `/dev/cons` transcript.
- **The login-spawned per-user session compositor** (`--session`, KT-1.5d-1a)
  is spawned AS the authenticated user (identity only -- no `CAP_SET_IDENTITY`,
  no renderer perm; login's is the only identity stamp), selected by
  `/lib/halcyon/session` reading `on`. It holds NO console-renderer role,
  connects to the system tapestryd as an ordinary-user `Actor::Session`,
  presents a fullscreen surface, and hosts the session's terminals as
  [[sub-kaua-term]] processes it spawns as itself.

The public data surface is the file-walk bias (no custom read verb): the
console trio; on `/srv/tapestry` the `layout` file (visible leaves), `pane/<id>/
{tagbar,tag,status,geometry,claim}`, and `ctl` (`display W H`). Levers:
`/lib/halcyon/renderer` (joey's system-renderer choice) and
`/lib/halcyon/session` (login's per-user choice) -- both one-token, fail-safe.

## Mechanism

### lib is the brain, bin is the body

lib+bin from birth (the H-2a lesson: a no_std bin's tests are dormant). The
LIB is pure logic over injected bytes -- zero syscalls, host-tested
(`cargo test -p halcyond --lib --no-default-features`); the BIN
(`required-features = ["guest"]`) owns the syscalls, the Surface, and the event
loop. The `test-mode` feature (default on) gates the renderer's test levers
(the `#wedge` internal verb -- THE GATE's wedged-owner proof). Each bin module
(`chromeset` / `menuset` / `statusset` / `session` / `main`) is the syscalling
twin of a lib module and nothing else (the H-3b-4 split: both halves in the lib
broke the host-test build).

### The console data flow

```
/dev/consdrain --bytes--> Transcript::feed  (wire::parse beacon + the VT scan)
  -> blocks (cells + styles + objs + tables)
  -> LaidBlock (layout_block, cached by block id)
  -> Cartoon (render_block)  --cartoon::execute-->  surf.pixels()  --present-->
```

Input: `TEV_KEY` -> (Insert) `key_bytes` -> `/dev/consfeed` with the held-feed
discipline; (Normal) the selection state.

### The session compositor (`--session`; KT-1.5d + the KT-1 audit)

`session::run` connects to `/srv/tapestry`, takes a fullscreen surface
(`EventRing::connect_sqpoll` + `Surface::fullscreen_on` + first-present-before-
wait), and hosts one [[sub-kaua-term]] + content Surface + `Tile` per compositor
leaf, keyed by leaf id, reconciled off the `layout` file each relayout (the pure
diff is `tiles::plan_tiles`, host-tested). The loop reconciles / renders / waits
on ONE `poll { ring.poll_fd() | up_0..up_N }` / ingests each readable tile /
routes input to the focused tile for free (tapestryd delivers `TEV_KEY` only to
the focused surface) / resizes / contains a tile's death / logs out when the last
tile is gone.

**The declaration is the display handoff (the KT-1 audit reshaped this).**
Before its first surface the compositor writes `session on` via
`EventRing::global_ctl` on the conn every tile surface shares: that ACT, not its
principal, backgrounds the console renderer. The seat is held by a conn WHILE IT
HOSTS -- an idle declaration is taken over by anyone; a holder with live tiles
keeps it against every newcomer. A refusal (E_BUSY held seat / E_PERM non-session
principal) is retried `DECLARE_TRIES` (40 x 25 ms) and then TOLERATED: the
session runs UNDECLARED beside the console rather than exiting (login treats
halcyond's exit as logout, so exiting would re-prompt the seat forever --
`seam-login-halcyond-fallback`). Once the first surface hosts, `connect`
re-writes `session on` and takes THAT verdict as `declared`.

**Death containment (14.11.10).** A clean `Control::Exit(0)` CLOSES the leaf (a
`close` layout verb) and reaps the tile; a `WireError` / non-clean exit /
abnormal EOF (a crash of the isolated parser) FREEZES the tile as an affordance
(last frame held, pipe skipped, `kaua-term` killed), reaped when the user closes
the leaf. Neither ends the environment. A `closed` leaf-id set is the permanent
respawn guard (leaf ids never reuse).

### The tile model (the untrusted record stream)

`tile.rs` holds one `Tile` per leaf: a live grid (`grid.rs`) + a scrollback
`Transcript`, separate because the grid spans zone boundaries. `Tile::apply`
dispatches: `CellDiff` -> the grid; `ScrollOff` -> `Transcript::push_scrolled_
rows`; `Control(Osc1936Raw)` -> `Transcript::feed` (the SAME Beacon parser the
console uses -- the format-fuzz surface stays ONE audited parser, not N);
`Control(Title/Exit/Bell)` -> tile fields; `Mode` -> the render mode. A tile is
untrusted (14.11.12): a producer's out-of-bounds `CellDiff` write is DROPPED in
`grid.rs` and the cursor clamped on read, so a hostile `kaua-term` cannot index
past the buffer. `Tile::render` composes alt-screen (the live grid alone) or
normal (scrollback flowing above a fixed mono grid tail, bottom-anchored).

### The chrome, the menu, the status bar

- **Chrome (H-3b)**: `chrome` (rules) + `chromeset` (surfaces): one
  `Role::Chrome` surface per visible leaf carrying a Daylight tag-bar strip,
  placed by the compositor at the leaf's `tagbar` rect. `key_for(focused,
  status)` derives the strip key from the COMPOSITOR's `status` record (never a
  private copy), so strip and the live hairline cannot disagree; the status feed
  sends `tag <own-pane> status ok|err` via `global_ctl` from the transcript's
  latched exit code (a latch, not a queue; a refusal is display-only, retried
  next mark).
- **Menu (H-3c, THE GATE)**: `menu` (rules) + `menuset` (surface): an obj run is
  the cells of one flat row sharing one `Style.obj` index; the verb table
  (`beacon::verbs::parse` over `/lib/beacon/verbs`) expands a typed rule into a
  `Command` (rc-quoted ref) or an `Internal` action (`#...`, test-mode only).
  The list carries the resolved ref on the title row (anti-clickjack).
  `MenuSet::open` mints `Surface::menu_on`, writes `menu place`, THEN paints +
  presents once. A `Command` choice feeds `^E ^U` + the command (preserving a
  half-typed draft). The compositor owns the dismiss (proven against a wedged
  owner by `#wedge`).
- **Status bar (H-3d)**: `status` + `statusset` (one `Surface::status_on`): the
  focused leaf's name + status, the transcript's cwd + last command (OSC 7 + ut's
  `mark k=cmd`), the UTC clock; paints only on a change (a say line lands in the
  transcript and would shift the keyed row).

### The event set (H-3c-2)

halcyond opens ONE `tapestry::EventRing` (one session + one Loom ring, SQPOLL
since KT-1.5b-i) and every surface it owns lives on it (console / tag bars /
menu / status). The loop blocks in ONE `poll(2)` over `EventRing::poll_fd()`
AND `/dev/consdrain`, so console output wakes the renderer at once instead of
at the next frame tick, and a tile's CONFIGURE no longer lands only at the next
pane-tree RPC (a Loom wait pumps ONE session -- the H-3b two-sessions latency
bug). See [[sub-libtapestry]] for the ring side.

## Data structures

- `Transcript` -- zones -> `Block`s (cells + styles + objs + tables); the
  streaming feeder with the escape-holdback scanner (`safe_cut`); the block
  deque with `ITEM_OVERHEAD`-charged per-line cost and eviction.
- `Tile` -- one leaf's live `grid.rs` buffer (fixed rows x cols `vt::Cell`, the
  OOB-drop) + a scrollback `Transcript`; the per-block HEIGHT cache (`heights`,
  keyed by width, aligned to the frozen deque) driving the windowed render.
- `DownQueue` (`downq.rs`) -- the per-tile down-channel: keys bounded to
  `DOWN_PENDING_MAX` (4096 B, drop-newest), the geometry record never dropped
  (latest-wins, ahead of keys), delivered one byte per ready POLLOUT.
- `EventRing` (from [[sub-libtapestry]]) -- the one SQPOLL session + ring every
  surface shares.
- Budget constants: `SESSION_SCROLLBACK_BUDGET` = 32 MiB (shared by tile count
  via `set_max_cost`), `OPEN_BLOCK_MAX_COST` = 512 KiB (freezes a newline-free
  open block), `POLL_MAX_NFDS` = 64 (the unified-poll fan cap), `DECLARE_TRIES`
  = 40.

## Concurrency

Single-threaded. The event loop is one thread over an `EventRing`
(`Rc<RefCell>`) plus the pipes; no locks. The session loop's fan-out is bounded:
the unified poll registers a POLLOUT entry per tile with pending input, stopping
at `POLL_MAX_NFDS` (64; 1 ring + 32 up + 32 down = 65 would return -1, read as
"compositor gone"). The down channel drains one byte per ready POLLOUT
(`DownQueue::drain_down`: POLLOUT means >= 1 free byte, a one-byte write from the
sole writer cannot block -- a parked compositor is a dead seat).

## Invariants enforced

None of the enumerated §28 invariants directly -- halcyond is a userspace
client that UPHOLDS, not enforces, the display's security posture (the audit
anchors are the H-2 / H-3b / H-3c / H-3d / KT-1 trigger rows +
`vault/record/audits/adt-kt1-r{1,2,3}.md`). What it upholds, prosecuted below:

- **The streaming property**: any chunking of a byte stream yields the identical
  transcript. `safe_cut` protects the escape OPEN at buffer end (the last-ESC
  heuristic was a real bug -- an OSC's ST is itself a later ESC). The
  byte-by-byte fingerprint test is the regression.
- **The robustness contract** (BEACON.md, inherited): never panic, never buffer
  unboundedly (`FRAME_MAX + 16` flushes an over-long partial), every malformed
  reference skips fail-safe.
- **Span hygiene**: Beacon spans die at block boundaries; the SGR pen persists.
  A program dying mid-`em` must not restyle the next prompt.
- **The grid containment**: an untrusted tile's OOB cell write is dropped, the
  cursor clamped.
- **Budgets bound memory against any input**: block eviction + stored cost
  (`ITEM_OVERHEAD` per line) + a per-block line cap + `OPEN_BLOCK_MAX_COST`; in
  the session one `SESSION_SCROLLBACK_BUDGET` shared by tile count, evicting AT
  ONCE.

## Error paths

A tile's crash is contained (frozen affordance), not fatal. A refused claim
(surface pool at cap) closes the leaf rather than leaving the keyboard routed
into a focused empty leaf. A refused `session on` runs undeclared. A refused
chrome/menu/status mint or verb is said once and retried on the next reconcile.
The down queue drops the NEWEST key past `DOWN_PENDING_MAX` (said once) but
NEVER the geometry record (a dropped Resize stranded the tile at the old size,
B2-F3).

## Performance

One render brain (a `GlyphSource` + the Daylight sheet + cartoon) reused across
the console and every tile. The windowed render (`Tile::render`) lays out only
the blocks intersecting the view plus the open block (at most two laid blocks
alive at once), off the per-block HEIGHT cache -- the fix for the round-2 P1
where the whole-history layout transient was ~1.8x the retained bytes and OOM'd
a session whose STORED bytes were well under budget
([[haz-budget-stored-not-derived]]). v0 presents full frames; damage-rect
presents are a recorded optimization.

## Prosecution

- **The decoder / parser against hostile input.** Malformed / oversize /
  truncated Beacon frames and record streams; the grid OOB-drop; the streaming
  fingerprint across every chunk boundary; the span-death at block boundaries.
- **The budgets against a flooding producer.** `ESC [ N S` and `?1049h/l`
  amplifiers reach the tile through the same [[sub-kaua-term]] bound; here the
  per-line `ITEM_OVERHEAD`, `OPEN_BLOCK_MAX_COST`, and the shared `set_max_cost`
  (evicting at once) bound the retained set, and the windowed render bounds the
  transient.
- **The identity of spawned tiles.** halcyond spawns every kaua-term with
  `.caps(!T_CAP_SET_IDENTITY)`; the kernel intersects with login's `SHELL_CAPS`,
  so no tile program can spawn as another principal (the C-F1 P0: `Command`
  inherits all caps by default).
- **The declared seat.** The takeover rule (idle vs hosting), the retry +
  undeclared fallback, the re-declare after the first mint; a refusal must never
  exit into the login loop.
- **Death containment.** A clean exit closes the leaf; a crash freezes the tile;
  the `closed` set prevents respawn; the whole must not end the environment.
- **The down channel.** The sole-writer POLLOUT one-byte discipline (never
  blocks); the geometry record never dropped; the POLLOUT set capped at
  `POLL_MAX_NFDS`.

## Seams

- Raw-VT panes (H-3; `raw_vt_intent` latches today), compose (H-5), the vk
  executor + the display-list wire (H-6), images/`Embed` (H-7) are unbuilt; the
  executor carries `Image`/`Embed` ops no transcript path emits yet.
- The session-tier settings verbs (the settings push) are unbuilt.
- Damage-rect presents are the recorded present-path optimization.

## Caveats

- `GlyphSource::regen()` must be the ONLY eviction: cache, table and pages move
  together; the executor's gen check is the belt.
- The layout cache keys (width, sheet.gen, atlas gen) by block id; the open
  block + pending line NEVER cache.
- The consfeed held-queue discipline is aurora's #129/#135/#136 verbatim; its
  policy lives in `input.rs` so the host tests pin it -- do not inline a
  "simpler" retry loop in the bin.
- The held-feed demux starvation (`memory/bug_held_feed_path_never_demuxes.md`)
  is ADDRESSED by the SQPOLL ring (KT-1.5b-i): the kernel poll-thread demuxes
  the console's parked reply on a frame-boundary deadline independent of
  halcyond's loop branch. A targeted repro is owed.

## Tests

- **Host: 99 `#[test]` across the twelve lib modules** (`cargo test -p halcyond
  --lib --no-default-features` -- the reference doc's "55" predates the KT-1
  rounds; transcript 23, tile 13, input 12, tiles 8, menu 7, layout 7, grid 6,
  raster 6, chrome 5, downq/status/select 4 each). They pin the streaming
  determinism, wrap/alignment/boxes, the word-through-executor leg, the
  held-feed arms, the obj-run walk + `run_rect`/`hit_run` agreement, the menu cap
  + window, the windowed render (a warm render lays <= 4 blocks / <= 12 lines for
  200 blocks of history; the content height equals the whole-history sum), the
  DownQueue policies, the grid OOB-drop.
- **In-guest** (`gate-interactive`, all lever-gated + SKIP-clean on a default
  image): `ls-halcyon` (joey's choice, the rich advertisement, the
  screendump/ink, the split/zoom reflow, the tag bars + the section-4.2 keys,
  the menu open/Esc-heal/click-away/`#wedge` GATE, the command path with the
  draft preserved, the event-set 3-leaf key-swap, `surfaces 1` after zoom);
  `ls-gfx-session` (the session tile spawn, the ingest, caps-probe's two-arm
  identity witness, zoom survival, the geometry legs); `ls-gfx-panes` (the
  negative twins: `role=chrome`/`role=menu` create -> E_PERM/E_INVAL, the gated
  verbs -> E_PERM).

## Provenance
(generated -- incoming `touched` backlinks, newest first; never hand-written)
