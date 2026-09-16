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
  - usr/halcyond/src/session_init.rs
  - usr/halcyond/src/tile.rs
  - usr/halcyond/src/tiles.rs
  - usr/halcyond/src/grid.rs
  - usr/halcyond/src/downq.rs
  - usr/halcyond/src/picker.rs
  - usr/halcyond/src/dialog.rs
  - usr/halcyond/src/help.rs
  - usr/halcyond/src/rail.rs
  - usr/halcyond/src/railset.rs
  - usr/halcyond/src/outline.rs
  - usr/halcyond/src/indicator.rs
  - usr/halcyond/Cargo.toml
audit: hard
guarded-by: []
validated-by: [prose, gate-interactive]
locks: []
hazards: [haz-budget-stored-not-derived]
abis: [abi-halcyon-palette]
design: ["docs/HALCYON.md", "docs/BEACON.md", "docs/KAUA-TERM.md", "docs/HALCYON-INSTRUMENT.md"]
created: 2026-09-05
updated: 2026-09-16
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

**The session publishes its palette to `/env/HALCYON_PALETTE` (s7a-3).** Beside
the `/env/HALCYON_SESSION` marker, at session start and before the first tile
spawn, the compositor writes `libhalcyon::theme::daylight_env_palette()` -- the
Daylight roles as `role=RRGGBB` text ([[abi-halcyon-palette]]) -- to
`/env/HALCYON_PALETTE`. A tile's hosted program (nora) inherits it via `/env` and
adopts it, so an editor in a session tile follows the session theme instead of
painting a hardcoded palette on the Daylight ground. Best-effort by design: the
write failing just leaves the value unset, and a hosted program keeps its own
default ([[sub-nora-host]]).

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
past the buffer. `Tile::render` composes alt-screen (the live grid alone, still
the mono `paint_grid`) or normal.

**The normal-screen tail is now PROPORTIONAL (PL-3/PL-4), not the mono grid.**
The live grid carries its soft-wrap state (PL-4a), `Transcript::live_block`
joins the soft-wrapped grid rows into logical lines (PL-4b-i), and the normal
render lays them through `live_block -> layout_block -> render_block` --
retiring the mono `paint_grid` tail there (it survives only on the alt screen).
So the tail flows in the same proportional layout as scrollback, with a
char-index caret, proportional selection banding, and obj underline. The
untrusted-drop discipline is unchanged -- the OOB clamp lives in `grid.rs`,
below the layout swap. `paint_grid` remains for the alt screen and for the
repainting-TUI case; `main.rs`'s console renderer is untouched (it drives the
`Transcript` run path, not this one).

**Beacon presentation rides the span serial, parser-free (H-4d).** A tile renders
obj/em/hdr markup over its cell grid without a second Beacon parser: the producer
stamps each `vt::Cell` with the serial of the last Beacon frame ([[sub-lib-vt]]'s
span mechanism), and the tile keeps a `SpanMap` -- an 8192-entry ring of `serial
-> SpanTag { block, obj, em, hdr }` noted as it feeds the SAME forwarded frames in
order (R5: one parser, and it is the console's). The ring allocates **lazily**
(16-byte `SpanSlot`s, `SPAN_MAP_BYTES` = 128 KiB): 0 bytes for a native tile that
never sees a Beacon frame, `SPAN_MAP_BYTES` once for a rich one -- and it sits
OUTSIDE the scrollback cost budget, so it is not double-counted against it (H-arc
round-1 B-F4). A cell resolves its presentation from its serial however late it
scrolls off and across the grid's zone straddle; a serial that fell off the ring
resolves to no span (the bound is reached only by a repainting TUI, which lives on
the alt screen where no span is read). `push_scrolled_rows(rows, &spans)` carries
the tags into scrollback, and `local_obj` copies the open block's obj into the
landing block -- through a `BTreeMap` remap cache (B-F3) -- so a scrolled row keeps
its reference. `Transcript::{span_tag, block_by_id, obj_in_block}` and
`Tile::{grid_runs, grid_run, grid_run_obj, grid_hit}` (the GRID_KEY render arms)
are the readers; `select::flatten_with_grid` folds the grid tail into selection as
a `GRID_BLOCK`.

### The session tile: Normal mode, selection, and the tile menu (H-4d)

A session tile spawns its `kaua-term` with `--beacon rich`, so the shell it hosts
emits the frames the SpanMap reads. Beyond hosting, the tile carries a modal
interaction of its own. `Mode` is `Insert` (keys flow to the pts) or `Normal` (a
selection state): Esc enters Normal, but only when the VT is on its normal screen
-- a full-screen app owns Esc -- and a `Record::Mode(AltScreen)` (the app switching
TO full-screen) leaves Normal on the spot (B-F5), so a selection cannot outlive the
screen it was made on. Since s7 F3, that same ingest point -- where `Record::Mode`
is matched just before `tile.apply(rec)` -- emits a test-mode
`halcyond: session tile leaf=<n> screenmode -> AltScreen`/`-> Normal` witness: a
`#[cfg(feature = "test-mode")]` `say!`, inert without the feature and with no
render effect either way, that gives the compositor-side proof a hosted app (nora)
entered its alt screen and restored it -- observed at the compositor's ingest,
distinct from the child's own EXIT markers on the serial ([[sub-nora-host]]). `normal_input` is the navigator -- the console's
Normal keys minus yank/paste, moving a cursor that starts on the grid's prompt row
with the view following it (`render(.., &mut scroll_up, Option<Mark>)`: the
selection band and the ember underline). `Sel` is the selection; `Tile.frame`
records the last render's block placement so `Tile::hit` / `grid_hit` turn a
`click` at display coordinates into a block or a grid run. Since the tail went
proportional (PL-4), a grid run's screen rectangle is no longer a cell product:
`grid_hit` + `grid_run_rect` invert the *cached laid tail* to map a position to a
run and back to its rectangle, and both are wired at BOTH menu-summon sites --
the mouse `click()` and the self-caught keyboard `act()` (the twin that a naive
one-site fix would have missed). Enter on an obj run (or
a click on one) summons the verb menu -- the same H-3c-2 `MenuSet` on the session's
shared ring, placed at the run's display coordinates (`menu::step_run_with`) -- and
a `Command` choice is typed back into the tile it was opened over as ONE
`Input::Text` `^E^U<cmd>\n` (clearing a half-typed draft, preserving the tile's own
line). `layout::laid_line_for` is shared with the console bin so a tile and the
console lay a Beacon line identically.

### The caret's blink and the motion lever (HALCYON-INSTRUMENT 10 + 9.5; I-8c-2)

Section 10 gives the caret `steps(2, start)` over 1100 ms with opacity 0 at
55 %, and 9.5 as amended at I-8 makes motion ON by default with
`/env/HALCYON_MOTION=0` the opt-out. Both live in the SESSION
(`session.rs`): the console renderer paints no caret at all -- `main.rs` has
its own render path and never calls `Tile::render` -- so at I-8c-2 there is
nothing on the console to animate and it takes neither the lever nor a
frame deadline.

The phase is FREE-RUNNING off `monotonic_ns`, with no per-caret origin: the
mockup's animation has no restart trigger, so there is nothing for an origin
to be relative to. `caret_on = !motion || caret_visible(now_ms)` is resolved
once per pass and PUSHED into each tile through `Tile::set_caret_on`, which
returns whether that tile must repaint. That push is the load-bearing part:
both render loops are DIRTY-GATED (`render_if_dirty` returns early unless
`self.dirty`), so **a tick that marks nothing paints nothing** -- a frame
deadline alone would wake the loop and change no pixel.

`Tile::paints_caret` is ONE predicate answering "is there a caret here at
all" -- the grid's own cursor visibility, and 14.6's rule that a retained
tile has none under Instrument -- and both the painter and the dirty rule
call it. The alternative was a second copy of that conjunction in the
session loop, which is the shape that has to be re-pointed by hand whenever
the painter's rule moves. `caret_on` is the separate second conjunct
("is it up right now"); folding the two would make every step mark every
tile, caret or no caret.

The profile word here is `inst_profile`, not the sheet's, because it is what
decides whether a dead child's tile is RETAINED, and `paints_caret` reads
the `fate` retention sets. A caret judging itself by a different word than
retention used could suppress on a tile the session never retained.

**The deadline is the STEP, not the frame.** `motion::caret_next_step_ms`
gives the distance to the next edge (605 / 495 ms alternating, never zero,
so the fold cannot spin) and it is folded into the session poll only while
`motion` holds AND some tile actually paints a caret. `FRAME_MS` = 16 is for
the CONTINUOUS tweens of section 10 (I-8c-3); applied to a square wave it
would wake about 34 times per visible change and paint nothing on 33 of
them.

**The preference is read ONCE and used TWICE (I-8c-3a).** `env_motion_word`
reads `/env/HALCYON_MOTION` unconditionally, because halcyond's own caret
needs it whether or not the declare took; `request_env_motion` then forwards
`motion::stated`'s word to the compositor as the gated `motion` verb, but only
once declared and hosting, which is the only state in which that verb is
accepted. It is sent even when it agrees with the compositor's default,
because the compositor cannot otherwise tell "the user asked for motion" from
"nobody has said anything yet". What travels is the WORD, not `admitted`'s
verdict: the clock conjunct is the reader's, and the compositor's clock is its
own frame tick.

**The cost, stated.** An idle session with a live cursor now wakes and
repaints about 1.8 times a second forever, where before it slept to the
minute clock. That is the price of a blinking caret and the lever is the
only thing that removes it -- under `HALCYON_MOTION=0` the resting value is
`true` for every tile on every pass, so no step ever marks anything and no
deadline is ever folded: the opt-out costs nothing rather than costing less.
A dead clock lands on the same static caret, which is the mode 9.5 already
specifies.

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
  owner by `#wedge`). Since I-7 `menuset` carries a `Model` -- the verb menu,
  the theme Picker, or a modal Dialog -- so ONE surface, one grab and one
  dismiss path serve all three; the model decides only what is painted and how
  a key / wheel / hover / click reads (`model_key` / `model_wheel` /
  `model_hover` / `model_click`). Since 2026-09-16 (HALCYON-INSTRUMENT
  section 10 revised, `2147d618`) the model also names the card's CLASS to the
  compositor: `summon` writes `menu place <id> <x> <y> dialog` for
  `Model::Dialog` and `Model::Help` -- which take the kit's backdrop and the
  help card's shadow -- and a bare placement for `Model::Verbs` and
  `Model::Picker`, which take only the theme menu's shadow. The word is the
  whole of halcyond's part: the compositor lays the effects on at upload
  ([[sub-tapestryd]], "Nothing freezes"), so nothing behind a menu or a dialog
  stops drawing.
- **The picker and the dialog family (I-7; HALCYON-INSTRUMENT 9.4 / 14.5)**:
  two more models on that one surface. `picker` is the display-theme control --
  the registry IS the gallery directory, re-read at every open (`read_gallery`,
  so a theme dropped in appears without a restart), grouped and ordered by the
  optional `[meta]` group/rank with the id breaking a tie, each row's miniature
  painted in THAT theme's own four colours (desktop / open / structure / amber).
  `dialog` is the modal family (the RESET confirmation, the running-close
  confirmation): a header, a wrapped `secondary` body, right-aligned buttons; the
  default carries an `amber` border and `text` ink, a destructive one `error` and
  is NEVER pre-focused. Esc and click-away are the compositor's, and for a dialog
  both read as Cancel.

  **A commit moves the seat only after the compositor ACCEPTS the push (9.4 (b)).**
  `push_theme` writes the gated `theme <wire>` verb, retrying `VERB_RETRIES` on
  `Busy`; only on `Ok` does the sheet advance a generation and the chrome, rail
  and status invalidate -- so chrome and panes can never disagree, and a refusal
  (`E_PERM`, or the busy cadence spent) leaves the previous bundle whole and says
  so. An empty gallery opens nothing (`NO THEMES`).

  **The two seats are deliberately asymmetric.** The SESSION seat also persists
  the pick -- `write_user_pick` does tmp -> `t_fsync` -> rename -> fsync into
  `$HOME/lib/halcyon/theme` -- re-themes each tile's retained history in place
  (`set_palette`) and queues an `Input::Palette` to its pts host, then re-publishes
  [[abi-halcyon-palette]] to `/env` for FUTURE spawns only (a running program is
  not reached). The CONSOLE seat has no user home, so it applies live and persists
  NOTHING, re-themes its own `Transcript` via `remap_palette`, and says
  `(console; not persisted)` / `NOT SAVED` rather than leaving the user to wonder.

  **The registry read is a format-fuzz surface, and the preview set equals the
  committable set.** A dirent's stem must pass `is_gallery_id` before it is read,
  which `gallery_bundle` re-validates at commit; that also holds the name to a
  single path component, so a hostile 9P server bound over the gallery directory
  cannot inject a `../` name (defense in depth -- halcyond holds only the user's
  own authority). A file that fails to load, or is a legacy-schema theme, is
  skipped.

  **The close dialog re-derives its guard at RESOLUTION, not at open.** The
  `close` tag re-reads `tile_count` when the button is activated rather than
  trusting the snapshot taken when the dialog was summoned, so the final-tile
  protection holds against the CURRENT tree even if the layout changed while the
  modal was up; a dismissed dialog drops the pending close.
- **The keyboard reference (I-7b; HALCYON-INSTRUMENT 9.5)**: the FOURTH model,
  and a DIFFERENT frame from the dialog family's -- 540 wide against 480, a
  header carrying a close x, a two-column key grid, a footer paragraph -- which
  is why `help` is its own module rather than a `Dialog` variant.

  **Its rows are the chords IN FORCE, not a literal.** (`new-tile` joined the
  table's vocabulary 2026-09-16 -- "Open a new tile in the focused pane", the
  compositor's Super+N, HALCYON-INSTRUMENT 6.1 -- so the reference shows it the
  moment the `chords` file names it.) `Help::from_chords`
  parses the compositor's `chords` file (the same text `rail::hints_from_chords`
  reads for the footer hints) at EVERY open, so a rebind is a rebind of the
  reference, an action with no binding has no row at all, and an empty or
  unreadable file opens nothing and says `NO CHORDS PUBLISHED` rather than
  presenting an empty card. The four-arrow focus and move sets each collapse to
  one `SUPER + ARROWS` row exactly when all four sit on the four arrow keys --
  the footer hint's own rule, so the two surfaces cannot disagree. The caps
  spell the arrows as WORDS because the mono subset carries no arrow glyphs
  (its extras are the lambda, the check, the guillemets, the minus, the command
  mark and the box set); the grammar's punctuation names read as the glyph they
  stand for (`slash` -> `/`).

  **Focus is trapped by construction**: the reference is the one grabbed
  surface, so no key of it reaches a pts -- H-3c's grab, not a second
  mechanism. Esc is the compositor's dismiss, its x is this side's
  (`MenuEvent::HelpClosed` -> `menu dismiss`), and Enter/Space close it too
  since the x is its only control. When the display is too short for the whole
  card the BODY scrolls under the fixed header (wheel, arrows, j/k, Home/End,
  clamped at both ends) rather than putting rows out of reach; the body is
  painted BEFORE the header in the display list, so a scrolled row can never
  show through it -- an ordered list is the clip the executor does not give us.
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

**The poll TIMEOUT is a fold, not a hand-written reducer (I-8c-1).** Two
sources already shorten the wait -- the rails' minute clock
(`statusset::clock_timeout_ms`) and a transient status notice's deadline
(`notice_timeout_ms`) -- and the two loops reduced them DIFFERENTLY: the
console's timeout is always positive and used `min`, while the session's
carries a `-1`-means-infinite sentinel and needed `timeout < 0 ||` guards.
Both now fold through `libhalcyon::motion::fold_timeout`, whose guarded form
is the general case and of which the console's `min` is the positive-`current`
specialisation; each call site expands to its original verbatim, so the
adoption changed no behaviour. It exists because I-8c-2's frame clock is a
THIRD source, and folding it in by hand would have meant writing the same idea
twice more in two shapes.

**A timeout wake alone paints NOTHING, and that is the integration point.**
Both loops are dirty-gated -- the console paints iff `t.seq != last_seq ||
dirty`, and `render_if_dirty` returns early unless the tile's own `dirty` is
set -- so an animation frame must also mark the flag for what it animates
(per-tile `dirty`, `chrome_dirty`). The granularity is the payoff: an
animation marks only its own surface rather than forcing a whole-display
repaint per frame.

### The rails (HALCYON-INSTRUMENT 8; I-4)

`rail` (rules) + `railset` (surface) is the fourth lib/bin twin, and it carries
BOTH rails: the top `Role::Rail` surface the compositor places at the strip its
carve always reserves, and the bottom rail -- which is H-3d's status bar
restyled here whenever the sheet's profile is Instrument (`status::status_list`
dispatches to `footer_list`). Under legacy no rail exists and none is asked for.

**The numbers are a MEASURED golden's, not the kit's CSS believed.** Every box
in `rail.rs` comes from `build/instrument-goldens/native-r2/matrix-carbon-1440x900-s100-baseDpr1`
-- the DOM boxes of `geometry-styles.json` and the PNG's own rows -- so the brand
mark's ring, the 1 x 12 separator at its half pixel snapped up, the 26-tall
buttons at y 4, the theme swatch with its `Derived.swatch_ring`, and the
footer's glyph box and centred hints are what a browser actually rastered rather
than what a stylesheet was read to mean. It is the same evidence class as
[[sub-libhalcyon]]'s `carve` agreeing with Chromium to the pixel.

**The rail is a POINTER TARGET like a header (9.1).** The compositor routes
MOVE, BTN and LEAVE to it; `railset`'s pump keeps the hover and the pressed
button, repaints in place for them, and turns a primary press into a
`RailAction` the OWNER acts on under ITS own authority -- never the rail's, which
is what keeps a chrome surface from becoming a second seat. It narrows at
`NARROW_W` (820): the top rail keeps the mark, the number and the icons while
the footer drops its hints and yields its label. The footer's hints follow the
`chords` file rather than a private copy, so a chord and its hint cannot
disagree.

**The SUCCESS square carries section 10's glow, and only it (I-8b).** The
footer's condition square is where the effects slice first paints: section
10's sage LITERAL at .25 under a box blur of 8, both scaled, pushed BEFORE
the check so the executor's list order puts the glow underneath it.

**The glow's colour is a literal and the check's ink is a token, and getting
that backwards is a real defect -- it shipped once.** Section 10 says its
effects "stay amber / green literals on every theme (the CSS does not
tokenise them)", so the glow is `instrument::effects::STATUS_SUCCESS`
(`#70A17C`); the check GLYPH is `inst.success`, which 8.2 does specify. The
first cut painted the glow with `inst.success` too. That is wrong on every
theme and wrong even on Carbon, whose success is `#819B85` -- close enough to
the kit's sage to look correct, which is exactly why it passed review. The
sibling case is not close at all and settles the reading: Carbon's `amber` is
`#C7B98B`, a pale sand, against the divider glow's `#D59A42`. The literals
now live in one module with a test pinning each against the token it would
otherwise be mistaken for. WHICH state carries it was
not derivable from the text -- section 10 pins a sage value, 8.2 describes the
square as amber (RUNNING) or hollow `secondary` (READY) and keeps RUNNING
explicitly pulse-free, and the kit's sage-filled square at READY is the
fixture state 8.2 REPLACES, with I-9's parity mask exempting it -- so it was
put to the operator and recorded in section 10's I-8 amendment: SUCCESS
(EXIT 0) alone. The alpha is `pct256(250)` = 64, the same rounding `Derived`
takes, so a glow and a derived opaque that both say ".25" agree to the byte
rather than drifting by one.

Its test carries three NEGATIVES beside the positive (READY, RUNNING and
FAILURE each glow-free), and that asymmetry is deliberate: a witness asserting
only that the glow EXISTS would pass a renderer that glowed every condition,
which is exactly what 8.2 forbids. Both halves are sabotage-measured --
removing the glow fails the positive, and adding the same glow to the RUNNING
arm (the plausible WRONG fix) fails the negatives.

### The outline path -- ONE rasterizer for every tier (HALCYON-TYPE 4; TY-1)

`outline.rs` is the type path: skrifa reads a face and scales its glyph outlines
to a pixel size, zeno fills them and STROKES them by the theme's smoothing
amount -- the em-relative dilation the Mac's "font smoothing" was MEASURED to be
(+18 % stem weight), unioned into the fill rather than approximated. There is no
hinting: the outline lands where the design puts it, at the whole-pixel pen the
atlas caches one raster per (face, size, char) for. Since TY-4 the MONO cells
come through here too (`mono_cell` gives the grid the bake computes and
`raster.rs` clips the glyph into it), so there is ONE rasterizer and therefore
one stroke rule for every tier; the bakes remain only for consumers that must
carry no rasterizer at all.

**The orientation is written down because it bit once.** Font space is y-UP,
zeno's default TopLeft origin wants y-DOWN rows, and a BottomLeft mask is stored
bottom-up. So the pen NEGATES y as it records the outline, the fill renders
upright with top-down rows, and zeno's `Placement.top` is the mask's top edge in
rows BELOW the baseline (negative above it) -- which makes the bearing cartoon
wants, up from the baseline to the first row, its NEGATION. Three conventions
meeting at one function is exactly where a sign error hides.

### The position indicator (HALCYON-INSTRUMENT 7.7; I-5b)

`indicator.rs` is a STATIC report of scroll position, and it is defined as much
by what it refuses as by what it draws: no fade, no drag, no click-to-jump, no
focus of its own, no ink change on hover. Wheel and keys scroll as they always
did; this only says where the view is. Instrument only -- the legacy tile has
none, and `Sheet::indicator` is what the painters key on; a raw full-screen
application owns its grid and gets none either (14.7).

The 8 px lane is reserved INSIDE the content viewport on overflow, so text never
sits under the thumb -- an explicit, deliberate replacement for the CSS's
platform-dependent `scrollbar-width: thin`, at the cost that line breaks may
change when it appears. No track is drawn (the body keeps its own ground); the
thumb is 3 px wide, inset 3 from the right and 4 from either end, at least 24
tall, `dim`, rectangular, and scaled through the sheet's `ipx` like every other
logical size. **Follow-tail puts the thumb's END exactly at the tail inset, and
while the reader is back in history an append keeps them there -- so the
indicator never reports "at end" until they return.** The picker reuses the same
arithmetic through `thumb_raw` with its own smaller floor (`PICKER_MIN_THUMB`).

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
- **Freeze-mid-`pre` style-index soundness** (PL-arc audit R1 F1 [P0] + R2 F6;
  the freeze/pre interaction is format-fuzz class). When `freeze_open` freezes an
  open block while a `pre` is open, the pre is finalized INTO that block -- the
  one whose `styles` vec its cells' style indices name -- never carried to a
  fresh block. `intern_style` is append-only / degrade-to-last, so a handed-out
  index survives even a ScrollOff interning more styles into the block before the
  freeze; `self.open` is reassigned in exactly the one `freeze_open`
  `mem::replace`. Both freeze triggers reach the arm and are now
  regression-witnessed: the tile-split `set_max_cost` and the
  `finalize_scroll_pending` ScrollOff. A stale index otherwise OOB-panics
  `layout_block` (`len is 0 but the index is 0`).
- **The grid containment**: an untrusted tile's OOB cell write is dropped, the
  cursor clamped.
- **One caret predicate, two consumers** (I-8c-2): `Tile::paints_caret` is
  what the painter asks AND what the blink's dirty rule asks, so a step can
  never mark a tile that shows no caret (a retained tile repainting twice a
  second to display nothing) nor skip one that does. Because the painter calls
  that predicate, the host witness cannot test the two for AGREEMENT -- that
  is a function equalling itself -- so it walks the fate x cursor-visibility x
  profile matrix against an expectation written from 14.6 directly.
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
- **The caret's blink at the predicate, not at the painter** (I-8c-2,
  `the_caret_blink_reaches_the_paint_and_its_predicate_matches_it`). The
  fate x cursor-visibility x profile matrix is asserted against an expectation
  written from 14.6 itself, because the painter CALLS `paints_caret` -- so
  requiring the two to agree requires a function to equal itself and can never
  fail. Measured: the retained-tile conjunct dropped from the predicate trips
  the `14.6 at Ended(3) cursor=true inst=true` assertion, which the agreement
  form could not see. The resting value inverted at both `Tile` literals also
  trips `legacy_render_is_byte_identical_to_the_pre_i5b_tree`, which is the
  positive statement of the same thing: `caret_on: true` leaves the whole
  pre-blink render byte-identical.
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
- **The tree's ONLY exhaustive match over `cartoon::Op`.** `tile.rs`'s
  legacy-equality tuple projection is the single place that enumerates EVERY
  op -- every other consumer filters with `_ => None` -- so it must gain an
  arm whenever cartoon's op set grows. It lives under `#[cfg(test)]`, and
  that is the part to prosecute: **a guest build never catches the omission**,
  because the match is not compiled for the guest at all. Only a host test
  run does. Grown at I-8a (`RectAlpha`, `Glow`) and again at I-8b-3b
  (`Blur`); a future op added without touching it fails nothing until
  somebody runs the host suite.

## Seams

- Raw-VT panes (H-3; `raw_vt_intent` latches today), compose (H-5), the vk
  executor + the display-list wire (H-6), images/`Embed` (H-7) are unbuilt; the
  executor carries `Image`/`Embed` ops no transcript path emits yet.
- The session-tier settings verbs (the settings push) are unbuilt.
- The 14.5 dialog family's DIRTY-close variant (`This tile has unsaved
  changes.`) and its one-line prompt have no producer: no program declares
  itself unsaved, and Rename waits on the workspaces mechanism. The
  running-job variant and the keyboard reference both landed at I-7b.
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

- **Host: 301 `#[test]`, all green** (re-measured 2026-09-16: transcript 54,
  raster 42, layout 37, tile 30, chrome 27, input 12, rail 11, grid 11, tiles 10,
  status 10, help 10, menu 9, outline 7, picker 7, session_init 6, dialog 5,
  downq 5, indicator 4, select 4). This row read 287 until 2026-09-16, drifted
  in three places (chrome 16 -> 27 with the W-arc's `workspace_header_tests`,
  help 8 -> 10 at I-7b, rail 10 -> 11 at I-8b-1). Re-derive it with `--
  --list` rather than trusting the figure -- and note that `chrome` carries
  TWO test modules, so a naive `::tests::` aggregation silently under-reports
  it by eleven and still sums to a plausible-looking total. **The command needs an explicit host target** --
  `cargo test -p halcyond --lib --no-default-features --target
  aarch64-apple-darwin`, run from `usr/`: `usr/.cargo/config.toml` pins
  `[build] target = "aarch64-unknown-none"`, so without the override the run dies
  at `E0463: can't find crate for 'test'` (this row said otherwise until
  2026-09-15 -- the stated invocation had never been runnable as written). They pin the streaming determinism, wrap/alignment/boxes, the
  word-through-executor leg, the held-feed arms, the obj-run walk +
  `run_rect`/`hit_run` agreement, the menu cap + window, the windowed render (a
  warm render lays <= 4 blocks / <= 12 lines for 200 blocks of history; the
  content height equals the whole-history sum), the DownQueue policies, the grid
  OOB-drop, and **both freeze-mid-`pre` triggers** (Invariants): the tile-split
  `set_max_cost` and the ScrollOff `finalize_scroll_pending`, each
  discrimination-proven -- sabotaging the finalize arm OOB-panics in
  `layout_block` (`len is 0 but the index is 0` for the ScrollOff twin, `index 1`
  for the set_max_cost twin), the arm makes both lay out clean.
  `railset` has NO host tests and that is not a gap: it is the bin twin of
  `rail`, and a binary-only module's `#[test]` never runs -- its behaviour is
  witnessed by the in-guest rail legs below.
- **In-guest** (`gate-interactive`, all lever-gated + SKIP-clean on a default
  image): `ls-halcyon` (joey's choice, the rich advertisement, the
  screendump/ink, the split/zoom reflow, the tag bars + the section-4.2 keys,
  the menu open/Esc-heal/click-away/`#wedge` GATE, the command path with the
  draft preserved, the event-set 3-leaf key-swap, `surfaces 1` after zoom);
  `ls-gfx-session` (the session tile spawn, the ingest, caps-probe's two-arm
  identity witness, zoom survival, the geometry legs); `ls-gfx-panes` (the
  negative twins: `role=chrome`/`role=menu` create -> E_PERM/E_INVAL, the gated
  verbs -> E_PERM); `ls-halcyon-instrument` + `ls-halcyon-session-instrument`
  (I-7: the theme control opens a REAL picker -- the mockup's 286 wide, asserted,
  not a stub -- a commit switches the theme live and the rail re-themes under it;
  the console seat says NOT SAVED while the session seat writes
  `$HOME/lib/halcyon/theme`; Super+T reaches the rail owner as a compositor chord
  and Esc dismisses. **I-7b**: the rail's `?` and `Super+/` each open the
  keyboard reference, asserted at the kit's 540 wide AND at a ROW COUNT taken
  from the placement say -- the derivation's witness, since a stub or a literal
  row list falls through the floor the leg checks; Esc dismisses it and its own
  x closes it from this side (`halcyond: help closed`); and on the session seat
  `Super+Q` over a live `sleep` is DELIVERED (`tapestryd: chord close -> rail
  owner`), ASKS with the 14.5 confirmation rather than closing, and a Cancel
  leaves the tile alive to report its job finishing -- the positive witness that
  the cancelled close closed nothing).

## The bar's two numbers -- reading the workspace header (2026-09-15, W-2a)

The status bar has carried `StatusModel.workspaces` / `active` since I-4, and
the painter has always drawn one chip per workspace with the active one as an
ember box (`status.rs`). Both fields were nevertheless assigned NOWHERE: they
sat at their 1/0 defaults, and `rail.rs`'s footer read the same dead 1. W-2a
is the feed, not the paint.

**Where the numbers come from.** `chrome::parse_workspaces` reads the `layout`
file's HEADER -- `workspaces N active K` -- which is the ratified channel for
this (HALCYON-WORKSPACES 4: no `workspace/` subtree exists, and the per-pane
rows below the header describe the ACTIVE root only, so they could never say
how many workspaces there are). `ChromeSet` stores the pair in the same
refresh that already reads the layout for `parse_tree`, exposes it exactly as
`pane_count()` is exposed, and both `model_from` call sites (the session's and
the console's) pass it through.

**Two decisions worth stating, because both could have been made carelessly.**

The parser reads ONLY the first line. A container row says `active=1` with an
equals sign and so could never collide with the header's bare `active` token
-- but reading one line makes that a structural property rather than a
property of the spelling, which is what a later format tweak would break. The
test that pins it is the control of the set: a layout whose header lacks the
tokens but whose rows carry `active=1` must parse as None, and without the
first-line rule it returns Some and the bar lights a chip off a pane row.

`parse_workspaces` returns `Option`, and that Option is carried all the way to
`model_from` rather than being resolved at the parse. A compositor older than
W-1a emits no workspace tokens, and the honest response is "keep whatever
default the model has", not "paint a guess". Resolving it at the parse would
have scattered that decision across two call sites; carried, it is made once.

**The one conversion.** The header is ONE-BASED (`active K`, K in 1..=N)
because it sits beside one-based pane ids and the `01`..`09` the rail paints;
`StatusModel.active` is ZERO-BASED because the painter compares `i ==
m.active` over `0..workspaces`. The subtraction lives in `model_from` and
nowhere else -- an off-by-one here lights the wrong chip, and one site is the
only way to keep that checkable.

**Status: UNGATED.** Host tests 295 (289 before; the six new ones are the
parser's) and the guest build is clean, but no runtime witness reads the bar
yet -- that is W-3's `ls-gfx-session` leg (Super+2 creates, the bar reads 2/2,
Super+1 returns, the bar reads 2/1). Compiling is not verifying.

## A workspace switch tore down the session (2026-09-15, W-3)

Found by reading while building W-3's gate leg, then measured end to end
before the fix: on `Super+2` the compositor logged `workspace switch -> 2 of
2` and halcyond logged `session logout (code 0)`. A workspace switch
destroyed the user's whole session.

**The mechanism is one wrong oracle.** `session::reconcile` builds its tile
plan from `parse_leaves_all(&layout)`, and `plan_tiles` computes `drop = have
- leaves`. Since HALCYON-WORKSPACES W-1a the `layout` file's per-pane rows are
**the ACTIVE root's only**, so after a switch every existing tile is absent
from them: `plan.drop` takes all of them, each gets `teardown()`, and the loop
then breaks on `tiles.is_empty()`. Absence from the rows means "not on
screen"; halcyond read it as "gone".

**The right oracle already existed, and W-1a had said so in writing.**
`live_ids` -- and therefore the `pane/` 9P directory, whose readdir
enumerates it -- is global ON PURPOSE: *"those are resource and addressability
facts, while the `layout` dump is the active root's; do not later reconcile
the two."* halcyond was the consumer that reconciled them by accident. The fix
probes `pane/<id>/geometry` per drop candidate: a dormant tile still walks, a
closed one does not.

**Why the inference was corrected rather than deleted.** `TEV_CLOSE` is
already handled (`session.rs`, the `reap` push) and is the authoritative
signal, so the drop arm is a second, weaker witness for one fact -- normally a
thing to remove. But I could not establish that every path by which a leaf
leaves the tree emits `send_close`, and an unproven claim is not a licence to
drop leak protection. `plan_tiles` keeps its pure contract and its host tests;
its `drop` is now documented as CANDIDATES, and the caller confirms.

**The bar's witness, same chunk.** `statusset`'s painted say carries the two
workspace numbers, and -- the part that matters -- they were added to the
`key` tuple that decides whether the line is re-emitted. Without that a switch
changing nothing else would repaint silently and no gate could observe the
move. They print RAW: `workspaces N active0 K`, where `active0` is the model's
ZERO-BASED field. Printing the one-based number the header carries would have
made a missing conversion invisible -- the witness would echo the compositor
and agree with itself. (`ws [..]` earlier in the same line is the workspace
SLOT's geometry, an unrelated field that happens to share three letters.)

## The existence probe needed a control, and the chips were never wired (2026-09-15, HALCYON-WORKSPACES round 1)

The W-arc's first adversarial round (Opus-5 fallback tier; Fable 5.1 died on
its first call to credit exhaustion) returned two findings on this side of the
seam. Both are fixed.

**F5 [P2] -- the W-3 existence probe read "gone" from ANY read failure.** W-3
had correctly established that absence from the `layout` rows is a statement
about the VIEW, not about existence, and probed `pane/<id>/geometry` against
the global tree instead. But `read_file` returns `None` for a failed open, a
failed read and non-UTF-8 alike, so *"it is gone"* and *"I could not ask"*
were the same answer. A transient failure -- fid budget, a wedged conn, an
interrupted read -- during the reconcile that follows a switch would therefore
drop EVERY tile, latch each id into `closed` permanently (ids are never
reused), and then break the session loop on `tiles.is_empty()`: the same full
logout W-3 fixed, reached from an error rather than a keystroke.

The fix is a **positive control one variable away**, not an errno: no errno is
reachable here (`T_E_NOENT` exists only in libthyla-rs comments). `layout` is
served by the same conn and always exists, so if the geometry probe says
absent AND `layout` cannot be read either, the honest answer is "could not
ask" and nothing is torn down this pass. A tile wrongly kept costs one
reconcile; `TEV_CLOSE` remains the authoritative teardown signal regardless.

**F6 [P2] -- the workspace list and the rail chips were pinned to one
workspace, and the gate leg could not fail.** W-2a fed the header's two
numbers to the BAR and nothing else. Both menu owners opened the list with the
literal `workspace_menu(1, 0)`; `RailModel::empty()` sets `workspaces: 1` and
neither construction overrode it; and choosing a row or clicking a chip only
emitted a `say!`. The real pair was already on the `ChromeSet`, feeding the
bar -- it simply never reached the rail. Because the session gate's leg
asserts only that the menu OPENS and Esc DISMISSES, it passed with the count
pinned at 1 forever: an assertion that cannot fail.

The session renderer now passes `chrome.workspaces()` into both `RailModel`
and `workspace_menu`, and routes both the chip click and the menu choice
through `layout_verb(troot, "workspace <n>")` -- the compositor's W-2b verb,
under this session's own authority, the way every other rail button acts. The
header's `active` is ONE-based and `RailModel.active` zero-based; that
conversion lives at the one construction site.

The **console** renderer (`main.rs`) deliberately stays at one workspace --
`docs/HALCYON-WORKSPACES.md` section 4: *"The console (pre-login halcyond)
shows one."*

## Reading a sparse workspace set (2026-09-15, S4)

The compositor's `layout` header stopped carrying a COUNT and started carrying
the ascending LIST of live workspace numbers, because S4 made a number an
identity and the set sparse. Everything on this side moved with it.

**`parse_workspaces` -> `Option<(Vec<u8>, u8)>`.** The old `k > n` refusal
generalizes to *an `active` that is not one of the live numbers*, and the
header is still refused WHOLE on that: a bar lighting a chip with no workspace
behind it is worse than a bar showing its default. `parse_number_list` refuses
an empty field, a zero, a non-number, a repeat or a DESCENT -- it does not sort
or dedupe a malformed header into a plausible one, because a compositor that
emitted `3,1` is wrong about something and repairing it quietly would hide that
while painting confident chips. A pre-S4 compositor's `workspaces 3 active 2`
parses as the single-element list [3] and 2 is not in it, so the reader fails
CLOSED -- asserted by a test, not assumed.

**The models keep POSITIONS and gain NUMBERS.** `StatusModel.workspaces` and
`RailModel.workspaces` are the list; `active` stays a POSITION into it. The
split is deliberate: a position is what a painter needs (`i == active` over the
chips it lays out) and a number is what a LABEL needs. Before S4 they were the
same value plus one, which is exactly why a sparse set broke the labels. The
number-to-position resolution happens once per consumer -- `statusset::model_from`
for the bar, the rail feed for the rail.

**BOTH chip painters moved.** There are two -- the bar's in `status.rs` and the
rail's in `rail.rs` -- and each labelled by `position + 1`. Fixing one would
have repeated W-1a's F2 shape exactly (a guard on one of two implementations,
with the shipped one forgotten), so both take their label from the list.

**A chip click carries a NUMBER, not a position.** `railset` maps
`RailHit::Chip(position)` through the painted model's list before emitting
`RailAction::Workspace(n)`, so the session hands `n` to the compositor's
`workspace` verb unchanged. Sending the position would switch to the wrong
workspace the moment the set is sparse. That mapping is BIN-ONLY and therefore
has no unit test; the gate is its only witness.

**The test-mode say.** `workspaces <list> active0 <position>`. `active0` is now
the DERIVED position rather than a raw zero-based field, which preserves and
sharpens the original anti-echo rule: the header already carries the active
number, so printing it would merely agree with the compositor, while the
position is a value the compositor never sends. With `workspaces 1,3 active 3`
the witness reads `active0 1` -- neither the number nor a count.

**Bin versus lib, stated because it was quoted wrongly.** `chromeset`,
`menuset`, `railset`, `session` and `statusset` are BIN modules;
`cargo test --lib` never compiles them. Any "halcyond N tests green" figure
covers the lib only -- including through round 1, where F5 (the existence
probe) and F6 (the rail/menu feed) live in bin modules and were witnessed by
the guest build and the interactive gates, never by that count.

## The chip press that resolved to nothing, and a reader with no bound (2026-09-15, HALCYON-WORKSPACES round 2)

**F6 -- a rail chip press was silently swallowed whenever the rail had not
painted.** `railset`'s hit test reads `self.zones`, but the chip arm alone
resolved its NUMBER by indexing the last-painted MODEL (`self.painted`). Those
two are written together on a successful present -- and `painted` is ALSO
nulled on its own by a `TEV_CONFIGURE`, by `invalidate`, by a fresh mint, and
never restored by a dropped present. In that window the press hit-tests fine
against still-valid zones, resolves through an empty `painted` to `None`, and
produces no action, no log and no notice. Every other hit (the mark, the split
twins, the theme control, reset, help, the arrows) still fires, which is what
makes it read as a dead chip rather than a dead rail.

Fixed by keeping the painted workspace LIST in its own field, assigned in the
same place as `zones`, so a hit and its meaning always come from one paint.

**This is the blind spot, not an accident of it.** `railset` is a BIN module
and `cargo test --lib` never compiles it -- measured: all five bin modules
(`chromeset`, `menuset`, `railset`, `session`, `statusset`) carry zero
`#[cfg(test)]`. The position-to-number mapping the whole sparse design rests
on has no unit test, and this defect was found by reading, not by a suite.

**F5 -- the header reader accepted what the compositor can never emit.**
`parse_number_list` refused zero, non-numbers, repeats and descents, but had
no ceiling: `workspaces 1,200 active 200` parsed, and the rail painted a chip
labelled `200` whose press the compositor then refuses -- silent at the only
place a user can see it. The reader now bounds each element by
`MAX_WORKSPACES`, restated locally because halcyond does not link the
compositor.

The LENGTH needs no second rule: distinct ASCENDING values in
`1..=MAX_WORKSPACES` cannot exceed `MAX_WORKSPACES` of them. That matters
because the two chip painters were each in range by a DIFFERENT accident --
`status.rs` applied its floor on the `usize` side (`len().max(1) as u8`, which
at 256 yields zero indicators) and `rail.rs` on the `u8` side. One rule in the
parser now covers both, and `status.rs`'s floor moved to the `u8` side to
match its twin rather than leave the asymmetry standing.

**A comment that denied its own second site.** `statusset`'s number-to-position
resolution described itself as "THE resolution in exactly one place". It is
not: `session.rs`'s `ws_pos` resolves the same number for the RAIL's model.
Two models, two resolutions -- the exact shape that has already produced two
defects in this arc, both of them a guard on one of two chip painters. The
comment now names both.


## The rail's hit map outlived its surface (2026-09-15, round 3, F3)

Round 2's F6 gave the chip arm a `painted_ws` list written beside `zones`, so
the two agree whenever a paint SUCCEEDS. It did not touch the paths where the
SURFACE goes away: the death arm and `ensure`'s re-mint both null `painted` and
left `zones` and `painted_ws` standing. A press arriving on the NEW rail before
its first paint therefore hit-tested against the DEAD rail's zones and resolved
a number from the dead rail's list.

**Round 2 made that worse, not better, and this is the honest reading.** Before
F6 the same window resolved through an empty `painted` and SWALLOWED the press.
After F6 it yields a real `RailAction::Workspace(n)` -- and under S4 any number
in 1..=9 is CREATABLE, so the compositor does not refuse it: a stale chip press
could mint a workspace that had just vanished. A fail-safe window had been
turned fail-unsafe.

Fixed by clearing `zones` and `painted_ws` wherever the surface changes.
**Deliberately NOT cleared in `invalidate`**, and the distinction is the point:
those fields describe a SURFACE, and `invalidate` is a SHEET change -- the
surface is unchanged and its zones are still true of it. Clearing there would
have broken the picker anchor a Super+T chord computes from the same zones.

Both the defect and the fix live in a BIN module, so `cargo test --lib` never
compiles them: the guest build and the interactive gates are the only
witnesses, as they were for round 1's F5/F6 and round 2's F6.

`MAX_WORKSPACES` is no longer hand-copied here -- see [[sub-libhalcyon]].

## Provenance
(generated -- incoming `touched` backlinks, newest first; never hand-written)
