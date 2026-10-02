---
id: sub-halcyond
type: sub
title: "halcyond — the Halcyon environment client: the transcript renderer and the per-user session compositor"
parent: moc-userspace-shell-tui
code:
  - usr/halcyond/src/lib.rs
  - usr/halcyond/src/clipboard.rs
  - usr/halcyond/src/clipbroker.rs
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
  - usr/halcyond/src/placesrv.rs
  - usr/halcyond/src/paneplace.rs
  - usr/halcyond/src/inlinecache.rs
  - usr/halcyond/src/paneroute.rs
  - usr/halcyond/src/inlineaccum.rs
  - usr/halcyond/Cargo.toml
  - usr/halcyond/src/viewtest.rs
audit: hard
guarded-by: []
validated-by: [prose, gate-interactive]
locks: []
hazards: [haz-budget-stored-not-derived]
abis: [abi-halcyon-palette]
design: ["docs/HALCYON.md", "docs/BEACON.md", "docs/KAUA-TERM.md", "docs/HALCYON-INSTRUMENT.md"]
created: 2026-09-05
updated: 2026-10-02
---
## Clipboard seat-ordering seam

`Broker::seat(None)` cancels locally when notified. Current Lictor/Tapestry
exclusion does not order that call before trusted input: an earlier HIA1 receipt
can already be queued to the broker. Application activation remains disabled
pending implementation of approved option A in
[the seat review](../../../../docs/HALCYON-INTERACTION-SEAT-REVIEW.md).
Display/key isolation does not itself establish clipboard cancellation.

The current session owner blocks in `Surface::submit_present` until Tapestry's
Rwrite. Tapestry can itself block in a normal GPU RPC that Lictor parks outside
NORMAL. A cancellation barrier executed by those same owners would introduce
a circular wait. The approved execution-ownership amendment is specified in
[the progress prerequisite](../../../../docs/HALCYON-INTERACTION-SEAT-PROGRESS.md);
no independent clipboard executor or control route is implemented.

## Pending clipboard admission (October 1)

`clipbroker` owns the Clipboard and one pending compositor decision. Routing
supplies authenticated Authority and an exact connection/fid-incarnation/request
Target; these are not peer-decoded credentials. Get pins bytes and Commit
validates the upload and generation before CHECK. A matching reply consumes
that pending action once. Wrong/late/duplicate replies cannot authorize a new
action. The 30-second deadline joins the payload store's next deadline.

Ordered focus-loss epochs cancel Begin and unprepared writes. Get/Commit keep
the earliest loss boundary and may finish only with a receipt that precedes it;
later returned focus cannot revive them. A read already admitted can finish
after focus changes. Controller loss, peer disconnect, exact cancellation and
trusted-seat generation changes release pending tickets; SAK cancels all
transfers, retaining only the committed clipboard value. Public service
activation still requires peer/controller binding and delivery of ordered
revocation/seat notifications to this owner. Native probe success does not
mean an application clipboard endpoint is enabled.

## Terminal observer registration (October 1)

The session accepts one binding announcement from its sealed Kaua child and
registers it with the actual child PID, leaf and owning Tapestry connection.
Duplicate announcements retire the terminal. Refusal leaves ordinary input
alive without interaction authority. This one-time synchronous setup does not
nominate an app. The pure asynchronous broker and native channel are implemented; application
nomination, session notification wiring, clipboard activation and the mode
widget remain unfinished.


## October 1 integration verification

Qualified Main/Aux are composed with the owned PollWorker/Stream service:
nonblocking acceptance and principal admission precede connection publication;
watch retirement precedes reuse; buffered work remains runnable; failed service
publication still ends the poster. Aux's limit read and Quiet counters preserve
that ownership path. Tokens route same-principal media; they do not authorize
foreground clipboard access. No live clipboard endpoint is enabled.

The combined tree passes the full 2275-test Mac host gate, 1830-test boot,
native observer/readiness/service-wire, graphical media/Lantern/manual, three
F10 SAK scenarios at 1280x800, and all 50 default/UBSan SMP boots with no failure
classifications. One ignored Haul and 69 stranded libutopia host tests remain;
no new Pi hardware, Linux host or minimum-display qualification is claimed.
The review is single-agent. Evidence: `work/oct1-reconciliation/` and
`docs/HALCYON-INTERACTION-STATUS.md`.


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

## Prepared connection admission

The pure library exports `servicepool`, owned by [[sub-halcyond-service-wire]].
It reserves controller/media/handshake capacity and retains retiring quotas until
watch reclamation. No production adapter calls it yet: session media keeps two
connections and clipboard remains unavailable. Capacity checks cannot replace
kernel host ownership or ordered Tapestry focus admission.

## Mechanism

### Session clipboard storage (HI-1b; no live endpoint yet)

`clipboard::Clipboard` is the pure storage half. Its nonzero session generation
must be freshly chosen by the future session broker. `Owner` combines the
broker's authenticated connection identity with the issued controller/context
scope; it is not taken on trust from request bytes. Exact owner equality gates
chunks, cancellation and admission completion. One transfer per controller is
shared across its contexts, and each session has two write slots and two read
slots. The current value plus these four slots conservatively reserve at most
5 MiB payload. Capacity, not written length, is counted; shared read buffers may
be counted twice. This is the storage ledger only: connection buffers and the
complete 7.375 MiB service ledger remain an adapter obligation.

BeginCopy follows a successful broker focus check. It reserves the full declared
capacity before returning a transfer. Contiguous nonempty chunks cannot exceed
16 KiB or the declared extent. `prepare_commit` requires full canonical UTF-8,
compares the expected generation and serializes all pending commits. Its ticket
freezes staging before the broker asks for authoritative focus admission.
`publish` consumes that ticket, checks its lifetime and moves the preallocated
immutable buffer into the current value without allocation or text scanning.
Generation exhaustion refuses rather than wrapping. Cancellation cannot undo an
already published value. Source exit retains the current value.

Get pins the current `Rc<Vec<u8>>` before the focus request; the pending read is
unreadable until its matching ticket is admitted. Later copies cannot mix its
bytes or alter its generation. Admitted reads can finish after focus loss;
unfinished writes are cancelled by the ordered focus-loss callback. Peer/context
loss and trusted-seat takeover cancel pending and admitted transfers, making
late tickets stale. The future adapter must serialize those callbacks with
Tapestry replies; these pure methods neither authenticate focus nor prove that
integration. They are not connected to the pane service yet.

Thirty-second idle and 120-second absolute deadlines use monotonic milliseconds.
`next_deadline` integrates with an event-loop timeout without a polling timer.
An operation that observes expiry returns Timeout and retires its slot; an ID
already reaped by `expire` is Gone. Wrong owner is Denied, generation mismatch
Conflict, pool/identifier exhaustion Busy, malformed offsets/text Invalid,
oversize input TooLarge, and refused payload reservation NoMemory. Chunk and
read failures leave contents untouched and do not refresh deadlines. The fixed
slot arrays, transfer metadata and bounded Rc control blocks do not grow with
traffic; the final adapter must include their sizes in its metadata ledger.

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

**A whole-screen erase pins the view (TC-1, HALCYON 14.13).** The tail is laid
only through its content rows and the view is bottom-anchored, so after a clear a
tile with history showed that history filling the view above a short tail: the
operator's "lantern doesn't clear" (2026-09-24), and `clear` broken the same
way. The producer now reports the erase as `Control::ScreenErased`, after the
erased rows (an ordinary `ScrollOff`, so the history keeps them) and the blank
(a `CellDiff`). `apply_control` sets the tile's `pinned` latch on it in NORMAL
mode only -- an erase claimed on the alt screen is ignored, whatever the
untrusted producer says -- and any `ScrollOff` clears it, because output has
filled the screen; a `Mode` flip leaves it, so `clear`, then an editor, then back
still shows the pinned screen. While pinned, and only when there is history,
`render` puts a `pad_top` gap between the history and the tail and floors the
content height at `viewh + total` INSIDE the lane loop, so the overflow decision
sees it: the history ends at the view's top edge, the tail sits under its own top
padding where a fresh tile's does, the history is reachable by scrolling up, and
the floor is exact whatever the metrics or the padding. The gap rides through
`natural`, the walk, the GRID_KEY mark span and `live_laid` (TC-1a audit F2: the
first cut ended the history AT the tail, so its last `pad_top` pixels -- a whole
line on Instrument -- showed above the slide). It is the
only way the tile learns of a clear; blank cells are never read as one.

**A synchronized frame holds the tile's paint, and nothing else (FL-1, HALCYON
14.3).** `Tile.hold` (`vt::FrameHold`, [[sub-lib-vt]]) follows the program's
frame: `apply_control` opens it on `SyncBegin`, closes it on `SyncEnd` and cuts
it on `Exit`. Every record still applies as it arrives, so the seam's ordering
contracts, the pin above among them, are untouched. The session's render step
skips a tile whose paint is due while `hold.holds(now)` says wait, leaving it
dirty; a skipped tile is not a present, so it counts neither toward the "session
up" witness nor toward the present-failure limit. A gone child's tile (`exit`
set, which also covers a crash that sent no `Exit`) never waits. Only that tile
waits: every other tile paints as usual. A successful present calls `painted()`,
which ends the hold, and test builds then say once per tile `synchronized frame
shown (N paint(s) held)` when the program closed the frame, `... abandoned at its
bound ...` if the 150 ms bound let it through, or `... cut short ...` if a
reconfigure or the program's end cut it, or the paint landed while it was still
open -- so a render step that counts the hold but paints anyway never reads as a
whole frame. That line is the premise the device leg measures: the frame
spanned reads, so without the hold a torn screen would have shown. The poll
timeout folds the nearest `due_ms` over the held tiles, so a program that never
closes its frame gets its tile painted at the bound, not at the next unrelated
event. A surface CONFIGURE (a resize, or the compositor's redraw request) cuts
the hold; a scale change that only reshapes the grid does not, since the
surface still shows the last whole paint. The loop's pre-poll rule -- any dirty
tile loops back to render before
blocking -- excepts a live tile whose paint is `waiting()`: it waits in the poll
for the rest of its frame or the deadline. The first device run had no such
exception, and the held tile spun the loop (91612 held passes in 150 ms) without
ever reading the frame's close, so the bound abandoned every frame; leg 8 now
fails on a held count of 1000 or more. The hold lives here, not in the kaua-term, because halcyond paints
after every read of a tile's pipe (at most `INGEST_BUF` = 8 KiB) and one
full-screen `CellDiff` is about 40 KB.

**Super+K forgets the history, and only the history (TC-1b, HALCYON 14.13).**
The chord is the user's: the compositor delivers `TEV_CHORD` code 4 with the
focused pane's id to the rail's owner ([[sub-tapestryd]]), and `railset` maps it
to `RailAction::ForgetHistory`. The session resolves the id against the tiles it
hosts, the console renderer only against its own pane; any other id is said and
dropped. There is no verb and no fallback. `Transcript::forget` drops every
frozen block, the open block's items, the half-rejoined `scroll_pending`, and
what a byte-fed zone's open `pre` has gathered and its open table has finished
(the header flag with those rows). It keeps the open block's identity, cmd mark,
styles and objs, with its class latched first (a zone is a document once any of
its content was structure), and every piece of in-flight structure (`pre`,
`table`, the em/obj stacks, `table_specs`) with the row still being written, as
it keeps the pending line; so a running command's later rows and its zone close
land where they would have. The bar's `exit N` goes with the command it named.
The live screen keeps its links and its look. A grid cell resolves its obj and
its zone's class through the block that was open when it was written (the span
ring), usually a frozen one, so every block `SpanMap::named` names survives as a
HUSK in `Transcript.husks`: a `Block` with no items or styles, its kind and its
latched class, plus only the named objs, kept sparse by index (`Husk.objs`,
ascending) so an unnamed index resolves to nothing. The store is sorted by
block id (an injected image freezes out of id order) and searched by binary
search. Husks are never laid out or selected; `block_by_id`, `obj_in_block`,
`local_obj` and `live_block` find them. Each costs `HUSK_OVERHEAD` (its
`Block`) plus each kept obj's entry and text, and the budget evicts husks before
any frozen block, including at the end of the forget itself.
`Tile::forget_history` keeps the inline images a ring-named obj names and those
no obj names yet (an upload still to be captioned), releases the rest, and
clears the pin (the history it pinned against is gone) and the height and frame
caches. `clear` then Super+K leaves an empty tile.

**What the budget charges, and what the cap's freeze keeps (TC-1b rounds).** An
obj costs its table slot (`OBJ_OVERHEAD`, one `Obj`) as well as its text at
every site that stores one (`open_op`, `local_obj`, `retained_cost`), and an obj
frame meets the open block's cap before it is pushed, so objects alone freeze a
block where the budget can reach it (a bare obj used to be free). A block-cap
continuation carries the running command's mark while it has no exit (else
`running()` read false and Super+Q closed a running job unasked). In a tile the
cap's continuation keeps an open `pre` and table open -- their text is on the
grid, and the accumulators hold only the state cells are tagged under -- and an
empty accumulator finalizes to nothing (an empty fence would be a history row
that no line added).

**The Normal-mode selection follows its rows (TC-1b, HALCYON 14.11.5).** `Sel`'s
cursor and anchor are flat positions, and both hosts used to clamp them after a
re-flatten, so a budget eviction moved the selection onto other rows, and yank
and Enter acted on those. `Sel` now keeps a `select::Stamp` of the list it
indexes: the transcript's `rows_dropped` (front drops, budget evictions and
forgets alike, counted by `Item::flat_rows`, the one row rule `select::flatten`
uses), `rows_left`, `rewraps` and `repaints` (below), and how many leading rows
were the transcript's. `Sel::rebase` moves a transcript row up by the front
drops. A live-grid row follows its text. It moves up one per row the grid has
SHOWN leaving, and one that left is found through the transcript's record of
the lines scroll-off completed (`ScrollLine { end, added }`, a ring of
`SCROLL_LINES_MAX` = 1024). `Transcript::scrolled_row` counts back from the end
to the history row its line joined (`ScrolledRow::History`: both halves of a
wrapped line, a pre line joining its fence), the grid's first row while its
line is still held (`Held`), nothing once forgotten or evicted (`Gone`), the old
end-anchored estimate past the ring (`Unknown`), or, for a number no row has
reached yet, `Ahead`, which the rebase reports and the tile restarts at the
prompt. Counting back is exact only because every history row a tile holds
arrives through such a line.

The count is two counters. `rows_scrolled()` is the live count: the rows
`push_scrolled_rows` took off the grid, plus `rows_shed`, the rows the mirror's
own reflow dropped at a resize that have not arrived yet (a ScrollOff consumes
`rows_shed` before it raises the count). `rows_left` is the published count the
stamp reads: `note_grid_moved` sets it to `rows_scrolled()` at the end of every
CellDiff, and `note_grid_shed` moves it at the mirror's reflow. A ScrollOff and
its repaint can straddle reads, and the session paints between them, so rows
alone move no end until the repaint shows them gone. A shed of n adds n to
`rows_left` but only n - min(n, k) to `rows_shed`, where k = `rows_scrolled()` -
`rows_left` counts the rows that arrived ahead of their repaint (the grid's top
k rows, here already). So `rows_shed` > 0 implies k = 0, and every grid end's
`rows_left` + g is its text's number in the ring. The producer answers each
resize it applies with a `WinsizeAck` and then a full repaint
([[sub-kaua-term]]). That acknowledged repaint at the grid's dims settles the
mirror's guess (`settle_grid_shed`: kaua-term coalesces resizes and can be ahead
of the mirror): the ends move DOWN by the rows the producer kept, and one moved
past the grid's last row is reported. A width change, on either screen (the
producer re-cuts its main screen beneath the alt screen), opens a re-cut window
(`rewraps` odd) until that repaint. Rows cut at other widths are no distance, so
the grid ends keep their places, a run goes at the grid's first repaint
(`repaints`; every repaint in the window bumps `seq`), and the settle restarts
the ends at the prompt. An acknowledged reply at another width opens the window
again, because the ack names no resize. `Tile::resize_selected` slides the grid
ends with their rows at a height change (by the `rows_left` delta), restarts at
the prompt those on rows it slid past or cut off, and restarts all of them at a
width change. A selection whose row went moves to the oldest row left; a grid
end that left the grid drops its run.

After a full-screen app exits, the mode flip arrives ahead of the main screen's
repaint, and until that repaint lands (`Tile::screen_pending`, set at a flip and
cleared by the next CellDiff) the grid holds the app's last frame.
`Tile::normal_screen_shown` is false then, so the session's Esc gate
(`Tile::modal_key`; an Esc press enters Normal mode, and a held Esc's repeats
in Insert are the program's) leaves Esc to the app, and a resize crops the
frame rather than reflowing it: a reflow would count rows that never left the
normal screen. Starting an app is the mirror of it: the flip to the alt screen
arrives ahead of the app's first paint, and until that lands the grid still
holds the shell's screen, while keys and a resize are already the app's. The
render paints what the grid holds (`Tile::holds_normal_frame`, true when the
mode is Normal XOR `screen_pending`): the app's last frame as the mono grid,
and the shell's last frame as it was, proportional on the sheet's ground. The
producer's diff of that frame carries the main screen's wrap flags
([[sub-kaua-term]]), so its soft-wrapped rows stay joined. The held-fragment
flush and the ScreenErased pin still ask the mode, since they read the
producer's state. The
console's one mid-list insert (a placed inline image freezes in front of the
open block's rows) is followed by `Sel::shift_from`. `select::refresh` is the
one re-flatten and rebase: both hosts call it before a key and before a paint,
and it re-reads on a `seq` change or a grid height change. Every transcript
entry point that changes the flat list bumps `seq`; before TC-1b, scroll-off, a
held half's flush and a budget re-share did not, so a tile's list could index
blocks the budget had already dropped until the next Beacon frame arrived.

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

**An aside is a frame the layout draws, not an item (2026-09-29; BEACON.md 12.2,
HALCYON-VISUAL 8.4, `dec-2026-09-28-beacon-aside`).** Beacon's `aside` is a
passage set apart from the flow, the manual's block quote. `Transcript::open_op`
flushes the pending line and records the aside's EPISODE: byte-fed, the next
value of a counter; in a tile, the serial of the frame that opened it. Every line
until the close is an ordinary `Item::Line` of the block, and `Line.episode`
carries the episode, as it carries a `pre`'s on the pre's lines (0 outside both).
An aside nests no block op (12.1 rule 5). While an aside or a `pre` is open
(`held()`), `open_op` ignores every open but `em`/`obj`, `close_op` ignores every
close but those and the block's own, and `point_op` ignores every point op but
the shell's `cmd`/`exit` mark. That mark ends the aside, since the command died
inside it. Neither block opens inside a heading, which holds inline text only
(12.1): the block's guard would swallow the heading's close, and the heading's
style would run on to the end of the zone. Zone frames are ignored while held, so the one freeze an aside meets
is the cap's continuation, and the aside goes on across it into the next block.
`Transcript::forget` keeps it open, like the other in-flight structure. A tile's
tag carries `TAG_ASIDE`, bit 7 of the `hdr` byte (the `em` byte's bits are all
taken); `row_shape` reads it as `aside`. A registry of `BlockSpec { open_serial,
close_serial, aside }` (`MAX_BLOCK_SPECS` = 32, the oldest dropped first, like
`table_specs`) gives a rebuilt line its block's open serial (`block_episode`), or
`UNKNOWN_EPISODE` (`u32::MAX`) once the spec has gone. That one value is shared
by every such line, so a forgotten block's rows join as a pre's rows did before
the registry; their own serials differ row by row (an `obj` or an `em` per row,
as in `la`'s pre) and would split one block into a frame per row. Two forgotten
blocks that meet are joined, as two `pre`s always were. `place_tagged_line` pushes an aside
line as `Item::Line` with its episode. A pre line joins the last `Item::Pre` only
when the episodes match, so two `pre`s that meet on the grid stay two.

A blank row was never written, so its cells carry span 0 and no tag. A blank line
inside a `pre` or an aside therefore reaches a tile's transcript and `live_block`
as a plain empty line, with episode 0, and it splits the block. The transcript
keeps what the grid carried, and the layout bridges the gap. `frames_of` gives
each item its frame (`Framed::Pre(e)` or `Framed::Aside(e)`) and extends a frame
over a run of empty episode-0 lines between two items of that same frame. A
bridged `pre` stays ONE island, its empty rows laid as empty pre rows. That
fixes the old split: a code block with a blank line in it showed as two islands
in a tile. A bridged aside line is the passage's paragraph break. The rejected
alternative absorbed the blank rows into the item. That removed history rows
after the fact, and `Sel::rebase` maps a history anchor by the front drops, so
an anchor on an absorbed row would have landed on the wrong row.

`layout_block_media` opens an aside's frame with a `pre`'s top margin, collapsed
with the pending bottom (after a prompt, the prompt's own gap), then the hairline
and `pre_pad_y`. The first line inside gets the first-child reset. Its lines are
prose (`Role::Prose`, and `Role::Empty` for an empty line), laid at `x0 = pad_x +
hairline + pre_pad_x` with `right_inset = hairline + pre_pad_x`: the left padding
on both sides, since the legacy `pre_pad_r` is 0. `close_aside` advances
`pre_pad_y` and the hairline, then pushes four hairline rects in `sheet.rule`
over `[pad_x, right_with(measure_cap)]`, with no ground, and the next item gets a
`pre`'s bottom margin. Under Instrument that is margin 18, padding 15 / 17 and
the 720 cap; under legacy margin 2, padding 2 / 8 and no cap. An inline image
in an aside is letterboxed into the frame's inner width and centred there
(`lay_inline_image` takes the columns it may use). A `pre` or an aside
that straddles the scrollback edge is laid as two frames, because the history's
block and the live block are laid apart; a `pre` always was. A byte-fed aside cut
by the cap's continuation is likewise two frames, one per block.

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

**The first conjunct is the CHILD's, and it crosses the whole seam.**
`grid.cursor().2` is DECTCEM -- the hosted program's `ESC[?25l` / `ESC[?25h`
-- so a full-screen child that hides its cursor gets no caret here. The path
is four hops and every one of them has to keep one bit: `vt` records DEC
private `?25` into `cursor_visible` (**distinct from SGR 25, which is
blink-off; the `?` carries the meaning**), kaua-term's `emit_celldiff`
compares the whole `(row, col, visible)` tuple so a **visibility-only change
still emits a record** with zero changed cells, the wire writes and reads
that byte (`wire.rs` `encode_record` / `parse_record`), and `Grid::apply_celldiff`
stores the tuple verbatim. Nothing in the normal stream puts it back: only
RIS (`ESC c`) sets `cursor_visible` true again.

`tile::tests::dectcem_travels_the_whole_seam_to_the_caret_predicate` drives
that whole path, wire round-trip included, and ends on the predicate. It
exists because the caret BLINKS, which makes a screenshot unable to settle
the question in **either** direction -- a frame with no caret may be a frame
caught mid-step, and one with a caret proves only that instant. A run that
read a pre-fix capture as evidence about a post-fix build reported this seam
broken when it was not; the host test is the reading that cannot drift.

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

**What the glow did to a gate nobody ran (2026-09-16).** I-8b-1's commit said
no gate reads the footer's sub-pixel ink. One does: `ls-halcyon-instrument`
reads the success square's 10 x 6 box at (8, 10) and wanted its dominant to be
the bare `rail` ground. The glow puts every pixel of that box inside its
plateau: a 17-tap box window covers the whole 6 px square on both axes, so
coverage is 36/289 and the alpha is 64 * 36 / 289 = 7. That gives
(9, 13, 13), not (7, 9, 10), which is exactly what the gate read on all three
attempts.

The gate now expects the plateau. The host test
`the_console_gate_reads_the_glow_plateau_in_the_success_box` renders
`footer_list`'s own ops through the executor and pins that value next to the
ground one row past the reach. A change to the glow's colour, alpha or radius
therefore fails on the host, naming the gate (a halved alpha reads
(8, 10, 11), sabotaged), instead of surfacing as a red guest run with no
stated cause.

**The click witness (2026-09-16).** A left press on the console transcript
says `halcyond: click at X,Y -> <ty ref>|no run`, and a press on a session
tile says `halcyond: tile N click -> menu|no run`. Both are test-mode only and
said AFTER the hit test. They replace the compositor's top-of-path press say,
whose console write was mirrored into this transcript and rendered before the
press it witnessed arrived. That moved the rows under the aim, and
`ls-halcyon`'s click leg missed on every run from `bd06c0ef` until the fix (see
[[sub-tapestryd]]'s witness section for the measurement). A say emitted here
after the hit test cannot move what the press addressed.

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

### Bounded service transport (HI-1)

Both media servers now explicitly mark accepted connections nonblocking and use
[[sub-halcyond-service-wire]] for framing, reply offsets and bounded turns. A
partial reply is retained until writable; complete buffered input makes the UI
loop runnable without another read edge. Connection order rotates within the
shared service deadline. The session server transfers its listener and accepted
handles into PollWorker and appends one notification fd to the UI poll set.
Retirement keeps handles alive until the old poll returns; server Drop joins.
The console keeps direct polling/RAII close. A published session-service failure
ends the posting compositor: closing a listener alone cannot unpost a service.
The image residual charges the worker's 72 KiB reservation. Existing one-console/
two-session capacity and routing/image rules remain; there is no live clipboard
endpoint. The full 38-connection ledger and session recovery remain separate
activation obligations.

### The inline-media place channel (I-47; the `view` reader side)

halcyond receives decoded rasters from a short-lived `view` process and injects
them into the transcript as `Item::Image` blocks (rendered by the already-built
`cartoon::Op::Image`). The DECODE runs in `view`, never here (the blast-radius
amendment; see [[sub-view]] for the writer + the `inlinewire` contract) -- so
halcyond's exposure is a bounded WRITE of untrusted bytes, not a codec.

- **The server** (`placesrv.rs`, the console `rs_main` path only): once the
  console is up halcyond posts `/srv/halcyon` (a minimal 9P2000.L service; it
  holds the console renderer's `MAY_POST_SERVICE` grant, joey ORs it beside
  `CONSOLE_RENDERER`). The namespace is two nodes -- the root dir and a
  `place` file, written with a picture and read for the channel's current
  per-image limit (2026-09-29). The listener + live conns join the loop's unified
  `poll(2)` (a write wakes the renderer at once), and one non-blocking
  `service()` pass per loop accepts + drains complete frames, exactly like the
  console drain (the same one-pass inject latency). The 9P codec is the shared
  `libthyla_rs::ninep` server codec; the dispatch/fid/frame-read shape is
  nocturned's (`usr/nocturned/src/server.rs`, not yet dossiered).
- **The limit read** (2026-09-29, HALCYON 14.7's refinement of that date): a
  `Tread` on `place` answers the per-image cap the channel's admission applies
  to a new transfer -- `max_pixels` on the console (`set_max_pixels`),
  `budget.max_pixels` in a session -- as `inlinewire::limit_read`'s window:
  ASCII decimal and a LF at offset 0, end of file past the text. `h_read` checks
  what it checked when a read answered end of file (a known, opened fid that is
  not a directory) and allocates nothing. The figure tells a pane's programs the
  display's size and the pane count, which they can already see; on the console
  channel, which has no peer gate, it tells any principal that can open
  `/srv/halcyon` the same. `view` reads it and reduces its raster before the
  upload ([[sub-view]]); the fit legs of `ls-gfx-inline-view` and
  `ls-gfx-session-image` exercise both servers.
- **The accumulator** (`inlineaccum.rs`, the PURE, host-tested brain): a `place`
  write carries an `inlinewire` header (magic/format/w/h) then the ARGB payload.
  `PlaceAccum::write` validates the header -- magic, `FORMAT_ARGB8888`,
  dimensions, and a heap-safe per-image pixel cap (`PLACE_MAX_PIXELS` = 1 Mpx,
  deliberately BELOW `inlinewire::MAX_PIXELS`, so a decoded raster cannot crowd
  halcyond's 64 MiB working budget) -- BEFORE it allocates a byte of payload, and
  `reserve_exact`s the exact `total_len` so the buffer never Vec-doubles (the
  audit-F2 2x overshoot); accumulates sequential writes bounded by the header's
  own declared total; and on completion yields the `w*h` ARGB `Vec<u32>` for
  `Transcript::inject_image`. A clunk mid-transfer discards the partial; a
  malformed / over-cap / non-sequential / trailing-past-total write is refused
  (Rlerror) and the transfer torn down. This is the format-fuzz surface, and it
  is where the tests live (Invariants + Tests below).
  - **The heap budget** (audit F1/F4, the OOM the round found + closed): the cap
    bounds ONE image; `MAX_CONNS` = 1 bounds how many accumulate at once (one), so
    with `reserve_exact` the whole place path peaks at `8 bytes x pixels` (a 4 MiB
    accumulator + a 4 MiB completion `Vec<u32>` at 1 Mpx). The per-image cap is
    DISPLAY-ADAPTIVE (`main.rs place_cap_for` -> `PlaceServer::set_max_pixels` each
    loop): the atlas scales with the scanout (~6 MiB at 1280x800, ~18 MiB at 4K),
    so the cap is the budget RESIDUAL after it -- holding the full 1 Mpx (native-size)
    through 2560x1600 (the operator's HiDPI) and shrinking only past ~3K, where a
    fixed 8 MiB place peak beside the ~18 MiB atlas + 32 MiB transcript would
    OOM. The round-1 defect was `MAX_CONNS`=4 x a doubled 16 MiB = the whole heap;
    the round-2 F4 refinement was the atlas term. Raising `MAX_CONNS` requires a
    real cross-connection byte budget -- do NOT bump it alone.
- **Authority** (the console spike): none beyond reachability. Injecting an image
  into the console transcript is at parity with writing text to `/dev/cons`
  (which any holder of the console already can), so the spike gates on
  format-fuzz safety + the resource bound, not a peer-identity check; the
  per-pane token + quota land with the session-path channel (below).

#### The session-path channel (I-47, HALCYON.md 14.7.2; the per-user, per-pane deployment)

The `--session` compositor owns each tile's transcript itself (the RICH tier),
so inline media in a session tile needs a session-side place server that ROUTES
each raster to the tile it came from. `paneplace.rs` (the syscall shell) +
`paneroute.rs` (the PURE, host-tested routing core) provide it; the payload core
(`inlineaccum` + `inlinewire`) is shared with the console spike unchanged.

- **One per-user service** `/srv/halcyon-<user>` (`PanePlaceServer::post`;
  `session_user()` reads + validates `/env/USER`, login seeds it and grants
  `MAY_POST_SERVICE` one hop). A failed post degrades cleanly -- the session runs
  with inline media unavailable.
- **The per-pane token is a PATH COMPONENT.** For each tile the compositor mints
  a fresh CSPRNG `u128` (`mint_place_token` -> `t_getrandom`), `register`s
  `token -> leaf` in the server, and writes the FULLY-RESOLVED address
  `/srv/halcyon-<user>/<hex(token)>/place` into that pane's `/env/HALCYON_PLACE`
  in `SessionTile::spawn` -- BEFORE `cmd.spawn()` so the child snapshots it via
  the per-Proc `/env` copy-at-spawn (`env_clone_into`), and REMOVED after so the
  compositor's own `/env` and the next tile's snapshot stay clean. Every program
  in the pane (`ut`, then `view`) inherits ITS pane's address transitively. It is
  unguessable, but not a secret among the principal's panes: any Proc of the
  principal reads it through `/proc/<pid>/environ` (the authority invariant below).
- **The namespace is dynamic** (`paneroute::walk_child`): the root's children are
  the live token dirs `<hex>` (validated against the routes map, fail-closed
  `E_NOENT` on an unknown/dead token -- `parse_hex32` accepts ONLY the canonical
  32 lowercase-hex spelling, so a token has no alias), each holding a `place`
  written with a picture and read for the limit. HPL2 adds a placement ID to the raster header; the routing token stays
  outside the payload and is checked at the walk.
- **One authority axis; the token routes.** The PEER-PRINCIPAL gate at accept
  (`t_srv_peer`, inline in `PanePlaceServer::service`) is the authority: a
  connection whose peer is not the session's own user, alive, is refused,
  fail-closed, so no other user places into the session. The token only routes
  ([[dec-2026-09-29-inline-media-one-principal]]). A `view` runs AS the user
  (tiles never elevate, `!T_CAP_SET_IDENTITY`), so a legitimate write always
  passes.
- **Routing + TOCTOU.** Completions are TAGGED with the target leaf
  (`PaneCompletedImage { leaf, .. }`); the compositor drains them each loop and
  stores into `tiles[leaf].tile.media`. A standalone `inline-image` object
  caption in the tile's text stream selects the raster at its ordered position. A token whose tile closed between
  walk and completion resolves to nothing at completion -> the raster is DROPPED
  (the pane is gone), never misrouted. `unregister_leaf` drops a closed tile's
  route at every reap site (reconcile drop, the surface-event reap, the ingest
  reap).
- **The DoS floor.** `MAX_CONNS` = 2 bounds concurrent transfers (a third
  `view` WAITS); the per-image cap is `place_cap(&gs)` = the same heap residual
  as the console DIVIDED by `MAX_CONNS`, so the aggregate in-flight
  (`MAX_CONNS x per-image-peak`) stays within the residual at every display scale
  -- a static bound, no cross-connection byte sum, `inlineaccum` untouched. The
  per-pane stored budget is split equally between transcript and raster cache.
  The cache holds at most 64 images and evicts FIFO under byte pressure. Missing
  rasters render the original caption. Cache changes invalidate layout heights;
  ID zero is reserved for console direct insertion.

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
- `PlaceServer` / `Conn` / `PlaceAccum` (`placesrv.rs` + `inlineaccum.rs`, I-47) --
  the `/srv/halcyon` listener + its bounded conn table (`MAX_CONNS` = 1 -- the
  console spike drives one `view`; a second connection waits, bounded acceptance;
  the audit-F1 heap-budget term, each fid table `MAX_FIDS` = 8) + the
  per-connection single-in-flight place accumulator; completed rasters queue in
  `PlaceServer.completed`, drained per loop into `Transcript::inject_image`.
- `PanePlaceServer` / `Conn` / `Fid{node}` (`paneplace.rs`, I-47, the session
  deployment) -- the `/srv/halcyon-<user>` listener + `routes: BTreeMap<u128,u32>`
  (token -> live leaf) + `principal` (the accept gate) + `user` (the addresses it
  hands panes) + `max_pixels`; `MAX_CONNS` = 2. A fid's node is
  `paneroute::Node` (`Root` | `Dir(token)` | `Place(token)`); completions are
  `PaneCompletedImage { leaf, w, h, argb }`. `paneroute` (PURE): `hex32` /
  `parse_hex32` (the canonical 32-lowercase-hex token codec) + `walk_child` (the
  fail-closed namespace walk) + `Quiet`, the counter that lets a repeatable
  diagnostic through at its 1st, 2nd, 4th ... occurrence (the server's `diag`
  holds one each for an accepted and a refused connect, a walk to an unrouted
  token and an upload whose pane has closed; the session loop holds one each for
  a placed and a cache-refused upload, and the console loop one for a placement).
- `LaidBlock` (`layout.rs`) -- a laid block's lines, rects, `blobs` (its
  resampled rasters, one per cached image and size) and `images` (`LaidImage {
  blob, x, y }`, a placement of one of them, shown at its raster's size:
  `LaidBlock::image_size`).
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
- **Aside containment** (format-fuzz class, 2026-09-29): an aside nests no block
  op, by the same `held()` guard as a `pre`, so no stream makes one gather a
  heading, table, `pre` or zone, or freeze at a zone frame; and neither opens
  inside a heading, whose close the guard would swallow. The block-spec
  registry is bounded at `MAX_BLOCK_SPECS` = 32, however many asides and `pre`s
  a stream opens.
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
- **The place channel validates before it allocates** (I-47, format-fuzz class).
  `PlaceAccum` parses + fully validates the `inlinewire` header (magic, format,
  dimensions, and a heap-safe `PLACE_MAX_PIXELS` cap tighter than the wire's own)
  from the 32-byte `HPL2` header BEFORE buffering a payload byte; each write is
  sequential-only and cannot grow the buffer past the header's declared total;
  a clunk mid-transfer discards the partial. So a hostile / oversize / truncated
  place-request can neither drive a large reserve nor exhaust the renderer's
  working budget -- it is refused (Rlerror) and the transfer torn down.
- **The session-path channel admits only the session's principal and never
  misroutes** (I-47/I-1, the `--session` deployment;
  [[dec-2026-09-29-inline-media-one-principal]]). The accept refuses any peer that
  is not the session's own user (`t_srv_peer`, fail-closed): that is the authority
  axis. A place-request lands only in the live pane whose routing token it walks
  (a CSPRNG `u128` path component, written into the pane's `/env` before its
  spawn), validated fail-closed at the 9P walk against the live routes map; a
  completed raster whose tile is gone is DROPPED, never misrouted. The token is
  routing, not a secret between one principal's panes: any Proc of that principal
  reads it through `/proc/<pid>/environ`, as it can already write any pane's pts
  (Plan 9's rio posture). The aggregate in-flight is bounded statically
  (`MAX_CONNS x` the residual-derived per-image cap); each tile's `InlineCache`
  holds at most 64 rasters in half the tile's content allowance and evicts its
  oldest, the caption staying as text.
- **What a block lays is bounded by the cache, not by its rows** (I-47 (d)).
  `layout_block_media` resamples a cached image once per (id, size) per block
  into `LaidBlock.blobs`, and every row that names it is a `LaidImage` placement
  of that raster; `render_block` adds each raster to the frame's blob table once.
  One picture named on thousands of rows costs one resampled copy.

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
  inherits all caps by default). The terminal host additionally receives
  T_SPAWN_PERM_SEAL atomically at spawn, before its first instruction. Its
  ordinary slave-side application uses the default unsealed spawn; H3+C clears
  seals across that child boundary. No authority binding or clipboard endpoint
  is activated by the seal alone.
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

- **The place channel against a hostile writer** (I-47). The `inlinewire` header
  parse (validate-before-allocate); the `PLACE_MAX_PIXELS` heap cap; the
  sequential-only, bounded-by-declared-total accumulation; the discard on a
  clunk / a malformed-write teardown; the bounded conn + fid tables; the
  single-in-flight-per-conn guard; a flood of refused connects or walks to
  unrouted tokens costs the console a line per power of two, not a line each
  (devsrv admits every 9P-mode connect, so another principal can knock). The
  accumulator is `inlineaccum`, host-tested adversarially (Tests).

## Seams

- Raw-VT panes (H-3; `raw_vt_intent` latches today), compose (H-5), the vk
  executor + the display-list wire (H-6) are unbuilt.
- **Inline images are BUILT** (I-47), on BOTH deployments: the console spike
  (`/srv/halcyon`, single service, `placesrv.rs`) AND the per-user SESSION-path
  channel (`/srv/halcyon-<user>` + per-pane token routing + the peer-principal
  gate + the residual/`MAX_CONNS` cap, `paneplace.rs` + `paneroute.rs`) --
  the transcript emits `Item::Image`, rendered by `cartoon::Op::Image`, and JPEG
  (the `view` decoder) + `--fullscreen` (`gallery`) are done. Remaining
  inline-media seams: `Embed` (the out-of-band pixel surface for video). See
  [[sub-view]].
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
- **Currency (2026-09-29): this dossier was edited for TC-1, TC-1b, FL-1 and the aside only.** The halcyond
  changes between 2026-09-17 and 2026-09-22 (about 940 lines of `tile.rs`
  alone) are not yet described here, beyond what earlier sections already say.
  Dating this edit stopped `quaestor stale` from flagging the dossier, so the
  debt is recorded here instead.
- The byte-fed console lays its pending line (`layout_pending`) as a block of
  its own. While an aside is open, the line not yet ended sits below the
  frame, outside it, until its LF moves it in (aside audit r1 F4; cosmetic, the
  console path only, since a tile's pending text is on its grid). A `pre`'s
  lines there wait for its close. Tracked in OPEN-BUGS.

## Tests

- **HI-1b, September 24:** 335/335 Halcyon library tests pass on Linux/aarch64,
  including nine clipboard storage tests for generations, pinning, ownership,
  deferred publication, cancellation, expiry and capacity bounds. Evidence:
  `work/hi1b-pi-deadline-fixed.log`. No live clipboard adapter is tested yet.




- **The I-47 close (2026-09-29): 445 lib tests, all green.**
  `a_block_lays_one_raster_per_picture_and_width` (one picture on the page and in
  an aside, a second on the page, in one block: three rasters, each placement its
  own; red with the size, or the id, dropped from the key) and
  `a_picture_named_by_many_rows_is_laid_once` (one obj held open over 2,000
  lines naming a 64x64 picture: 2,000 placements, one raster, one frame copy;
  red with the reuse removed, and red with `inline_image_lays_renders_and_reflows`
  when `render_block` copies per placement) and
  `a_repeated_diagnostic_is_written_at_powers_of_two` (red when `Quiet` lets
  every line through).
- **The aside (2026-09-29): 442 lib tests, all green** (host, `cargo test -p
  halcyond --lib`). Byte-fed: `an_aside_s_lines_carry_its_episode` (the line
  pending at the open is outside, the one pending at the close inside, two
  asides are two episodes), `an_aside_nests_no_block_op` (a heading, `pre`,
  table, zone, rule and program mark inside are ignored, `em` still styles),
  `a_shell_mark_ends_an_open_aside` (`exit` and `cmd`),
  `an_aside_goes_on_across_a_continuation_freeze`,
  `a_block_op_inside_a_heading_is_ignored` (a `pre` and an aside; the
  heading's close still ends it), and
  `random_streams_keep_each_aside_s_lines_together` (400 seeded streams of
  every block op in any nesting, a 6-line block cap: one aside's lines are one
  run, across blocks too). Cells mode:
  `random_frames_keep_the_block_registry_in_step_with_the_held_block` (the
  last spec is open exactly while a block is held, no other ever is),
  `the_block_spec_registry_is_bounded` (1000 opens hold 32; the rows of gone
  specs share `UNKNOWN_EPISODE`, so an aside's two serials and a pre's two
  still join), `an_aside_on_the_live_grid_is_one_frame`,
  `an_aside_s_rows_keep_their_episode_when_they_scroll_off`, and the rewritten
  `a_blank_line_inside_a_pre_stays_inside_it_on_the_live_grid` (one island
  across the blank row; two `pre`s with a blank row between, and two on
  adjacent rows, are two). Layout:
  `an_aside_is_a_hairline_frame_at_a_pre_s_margin_and_padding` (Instrument: 18,
  15 / 17, 720, a word too long for a line broken exactly at the inner edge),
  `an_aside_under_the_legacy_sheet_takes_the_island_margin_and_padding`,
  `two_asides_back_to_back_are_two_frames`,
  `an_inline_image_in_an_aside_stays_inside_its_frame` (and, one variable away,
  at the page width outside one). Sabotage: 25 mutants over the
  guards, the episode counter, the registry, the scroll-off episode, the tag
  bit, the bridge and the frame's geometry (among them the right padding taken
  from the legacy `pre_pad_r`, which is 0), each red on exactly the tests
  predicted for it.
- **FL-1 (2026-09-28): 424 lib tests, all green** (`tools/test-rust.sh
  halcyond`). `the_frame_records_open_and_close_the_hold_and_an_exit_closes_it`
  pins the records' effect on `Tile.hold`;
  `a_slide_split_across_two_reads_is_held_until_its_close` runs lantern's slide
  change through the real vt, producer and wire, cut in two reads right after
  the erase: after the first read the tile holds the blank, after the second
  the slide is through. Sabotage: SyncBegin opening nothing (S12) and SyncEnd
  closing nothing (S14) red both; an Exit that leaves the hold open (S13) reds
  the first. The render step's skip, the witness line, the poll deadline and the
  CONFIGURE close live in the bin and are proven on the device
  (`ls-halcyon-lantern`).
- **TC-1b (2026-09-28): 422 lib tests, all green** (`tools/test-rust.sh
  halcyond`). TC-1b's tests pin the forget (what goes, what stays, the husks,
  the budget, the obj charge, the continuation, the cap's freeze in a tile), the
  selection across front drops, a forget, a wrapped scroll-off, a held half and
  the image insert, and, through the real producer and vt on the seam, every
  resize case the audit rounds raised: rows split from their repaint, the shed
  counted once, the settle, the re-cut window on either screen, a reply at
  another width, a row cut off below, a run held in the window, a reply at an
  earlier, taller size landing between two shrinks, the app's last frame (Esc,
  paint, crop), the shell's frame until an app's first paint (paint, ground)
  and the modal gate.
- **TC-1 (2026-09-25): 340 lib tests, all green** (`tools/test-rust.sh
  halcyond`; the per-module figures below are older). TC-1 added ten in
  `tile.rs`: `a_screen_erase_pins_the_live_tail_to_the_top_of_the_view` (the
  tail at `pad_top` while pinned, the history reachable, back to the flow's
  position after a ScrollOff), `the_pin_is_the_normal_screens_and_only_a_
  scrolloff_releases_it` (the latch), and `a_clear_crosses_the_whole_seam_
  keeping_the_erased_screen_as_history` (vt -> producer -> wire -> tile in the
  operator's shape: the command line that started the deck survives the clear),
  and `a_reset_rescues_a_tile_left_on_the_alt_screen` (RIS on the alt screen
  returns the tile to Normal, moves the main screen's text to history, pins).
  Each was seen to fail: a one-mechanism sabotage sweep of 22 legs redded
  exactly the predicted tests, and the RIS fix's own legs red the rescue test.
  The audit close added six: the Instrument pin (no history above the tail,
  the lane reserved by the floor alone), a pinned tile without history laid as
  a fresh one, a mark on the pinned tail, a pinned tail taller than the view,
  typing after `clear` (ut's `\r ESC[J` redraw files nothing), and a restarted
  row 0 that never glues; a 13-leg sweep redded each as predicted. Round 2
  added four seam tests (vt -> producer -> wire -> tile): a reset on the alt
  screen keeps a held line whole, a one-row tile keeps an autowrapped line
  whole, a restart that changes nothing else still ends the line above, and a
  clear (ED 2, ED 3, RIS) keeps the line row 0 continues; a 9-leg sweep redded
  each as predicted. In-guest: `ls-halcyon-lantern` leg 5.
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

- **Host: 227 `#[test]` across the lib modules** (measured `cargo test -p
  halcyond --lib --no-default-features`), including **paneroute's 5** (the I-47
  session-path routing core: the 32-hex token codec round-trips; `parse_hex32`
  accepts ONLY the canonical form -- rejecting short/long/uppercase/non-hex so a
  token has no alias; `walk_child` resolves only LIVE tokens and fail-closes on a
  dead one; `.`/`..` climb; the address tail matches the walk) and
  **inlineaccum's 11** (the I-47 place-request accumulator, adversarial:
  `set_max_pixels_gates_the_reused_accum` -- the F2 regression, discriminating
  that a refreshed cap gates a reused accum's next image; plus a
  one-write and a split-write complete
  to the right pixels; bad magic, over-cap dimensions, a giant-claiming header,
  a non-sequential offset, and trailing bytes past the total each Reject; two
  images on one fid; a partial-then-abandoned stays incomplete; and
  `capacity_is_exact_no_doubling` -- the audit-F2 regression that the buffer
  `reserve_exact`s to `total_len` with no Vec doubling -- the
  validate-before-allocate + bounded-accumulation contract). They pin the
  streaming determinism, wrap/alignment/boxes, the
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

## A backgrounded leaf is no stack member and no RESET target (2026-09-29)

The compositor's `layout` dump marks the console renderer's leaf `backgrounded`
(first in a session's root row, weight 1), and tapestryd's carve skips it: the
Stack arm lays out only the shown members and the Tab arm opens the first shown
one ([[sub-tapestryd]], KT-1.5d-3 F2). halcyond read the same rows without the
token. On a root row stacked by Super+S ([console, tour, shell]) `parse_tree`
numbered the two shown tiles 02 and 03 of a stack of 3, so the last shown tile's
close box -- which refuses only a stack of one (HALCYON-INSTRUMENT 6.5, FINAL
TILE IS PROTECTED) -- closed it, and the session with it; and `reset_plan`
planned a focus on the console leaf, which the compositor refuses, so RESET
reported RESET REFUSED.

`parse_tree` now takes each stack's `index`, `count`, `open` and `last` over the
members the carve shows: a post-pass per stack drops the backgrounded ones and,
when the active member is backgrounded, opens the first shown member, as the
Stack arm does. **Only the displayed number, the count and the two flags move;
every action still names its leaf by ID**, so no header maps back to a raw
position. The count is `n.saturating_sub(backgrounded).max(1)`: a malformed
dump listing more backgrounded members than its `n=` gives a count of 1, never
an overflow panic (release builds keep overflow checks).

`reset_plan`'s rows carry the token: `children_of` skips backgrounded children
and `reexpands` judges a stack against its FIRST SHOWN child, so RESET neither
focuses the console leaf nor re-expands a stack already open on its first shown
tile. A root row whose active child is backgrounded plans nothing for it.

Tests: the Super+S row's numbering and count and the malformed dump
(`chrome.rs`); three `reset_plan` cases, a control without the token, and the
backgrounded-active root (`rail.rs`); each red under a sabotage of its hunk. On
the device, `ls-halcyon-manual` leg 1b: a right press on the shell's header in
the stacked login row says `count: 2`.

## The chrome line says every change of a header's rect (2026-09-29)

In test builds the chrome set says `halcyond: chrome <surface> for pane <id>
at <x>,<y> <w>x<h>` when it mints a header or placard strip and whenever the
strip's rect changes; gates find a header by the LAST such line (a close-box
press at `x + w - 9`, a blank-corner click at `x + w - 30`). The change test
compared the wanted rect with the strip SURFACE's size, but the strip's own
CONFIGURE (handled by the pump) resizes the surface before the layout pass
runs, so a header that changed width at the same position was never said:
after Super+H in a row of two the tour's header went from 632 to 418 px at x 4
and its last line still read 632. The test now compares against the geometry
the line last SAID (`Tile::said`, test builds only), set at the mint and at
each say. `ls-halcyon-manual` leg 4a reads the three headers' widths after the
split and was red on the old rule.

## An empty workspace waits to be asked (2026-09-29)

The session's spawn plan (`tiles::plan_tiles`) makes a tile for every empty,
visible leaf it does not host and has not closed; the claim mint is the
owner-and-emptiness gate. When tapestryd began stamping a session's new
workspace roots with the session's principal ([[sub-tapestryd]]), that root
became claimable and the plan filled it at once: a new workspace came up with
a shell in it, so an empty one could no longer vanish when left (the i3 rule,
HALCYON-WORKSPACES). A first fix keyed the wait on the tree's shape -- the
active workspace's lone empty root waited -- and it swallowed a one-leaf
`halcyon layout restore` onto a new workspace: the placeholder is hosted into
the fresh root, `split` nests it with the anchor, the placeholder closes, the
container dissolves, and the anchor became a lone empty root that waited
behind a placard (Fable round 4, F2). The rule now keys on how a pane came to
be. The compositor's dump marks ` fresh` an empty leaf it made on its own
account ([[sub-tapestryd]]), `chrome::Leaf` carries the token, and
`plan_tiles(leaves, have, closed, opened)` fills a fresh leaf only when the
session's `opened` set holds it (the placard's Open shell); Super+N's ask
clears the mark in the compositor. A leaf a split makes, or a restore builds,
is not fresh and is filled as before. Pinned by
`a_fresh_pane_waits_to_be_asked_and_a_restored_root_does_not`, which parses
real dump rows (a fresh root, its asked control, a restored root, a split's
leaf), red when the rule or the parser is broken.

## A session tile draws its selection (2026-09-29)

A session tile banded only the Normal-mode CURSOR row: `session.rs`'s `mark()`
handed `Tile::render` one `Mark`, while the console renderer bands every row of
`sel.range()`. The `v` anchor existed -- TC-1b rebases it in both hosts -- and
was never drawn, so `v` then `kkk` then `y` changed nothing on screen (the
Operator's Manual chunk's device run 11, leg 10). `Tile::render_selected` now
takes the selection's rows (`tile::selection_bands`, each keyed by
`tile::block_key` as a `Mark` keys the cursor) and bands each row once -- the
frozen blocks, the open block and the live grid -- through the session's
`bands()`; `render` is the no-selection form and paints byte-identically.
Pinned by `a_selection_bands_each_row_it_covers`, red under four sabotages (no
frozen bands; no cursor dedupe; no grid arm; the anchor ignored); on the
device, `ls-halcyon-manual` leg 10a (`v`, `k`, `y` each change the band).

## The layout notice rides any surface (2026-09-29)

The session loop re-planned its tiles on a TEV_LAYOUT only when it arrived on a
TILE's stream; the chrome, bar, rail and menu pumps drop events they do not
handle, and tapestryd sends the notice to the seat's lowest surface slot,
whatever it hosts ([[sub-tapestryd]]). After churn that slot was a chrome
surface, which the chrome reconcile drops whenever its pane leaves the active
tree. On the Operator's Manual chunk's device run 13, `halcyon workspace 4;
halcyon layout restore X` left the restored pane empty behind a placard: the
notice for the restore's released reservation -- no geometry changed, so it
was the only one -- went to chrome 1, and chrome 1 was dropped on the
workspace switch.

Step (3) now also takes the ring's `take_layout_hint()` as a relayout
([[sub-libtapestry]]), and the loop does not block while `layout_hint()` is
set: a notice reaped by the chrome, bar or rail pumps after the reconcile would
otherwise wait for an unrelated wake. The tile loop's own TEV_LAYOUT arm stays.
On the device, `ls-halcyon-manual` leg 16's restore onto workspace 4 is the
witness (red 3/3 on the image before the fix).

## The I-47 close: one raster per block, a routing token, quiet refusals (2026-09-29)

The Fable-diversity round on inline media (FABLE-1) found a layout resampled a
cached picture once per ROW that named it: in a tile every grid row is its own
line, so a program that placed one image and printed its caption object over
thousands of rows made thousands of copies per layout, and more in the frame's
blob table. A block now keeps one resampled raster per image and size and places
it on every row that names it. The same round found the session channel's token
readable by any Proc of the principal; I-47 now says the token routes and the
peer gate is the authority ([[dec-2026-09-29-inline-media-one-principal]]), and
the ARCH row is ENFORCED, and the code's own headers, which still called the
token a secret and an authority axis, say so too. Every place line a client can
repeat (a connect accepted or refused, a walk to an unrouted token, an upload
placed, refused by the cache or orphaned) is logged at powers of two, the
session-image gate's fail-fast pattern now matches the refusal line halcyond
writes, the wire comments say
the header is 32 bytes (`HPL2`), and the three dead-code warnings are gone:
`hdr_track_fx` is test-only, `MenuSet::is_open` and the console's unread
`CompletedImage.id` are removed. The close's own round (Fable reviewing Opus)
found the rest: the retracted secret-token wording surviving in the code's
headers, HALCYON 14.7.2 and these bullets; four repeatable lines still
unthrottled; the size half of the raster key untested, with `LaidImage` carrying
a size its raster could contradict (it now carries none); and view's 16-bit PNG
peak misstated (zune holds the inflated stream beside its output).

## Provenance
(generated -- incoming `touched` backlinks, newest first; never hand-written)


The session media test exercises live inline pixels, Gallery zoom/escape and
manual review captures. Host tests cover 325 Halcyon cases after adding caption
resolution controls (all 325 pass). Test-mode chrome geometry diagnostics now
track moves/resizes as well as initial mint, so interaction gates can use the
current pane tree rather than stale coordinates.

### Lex curiata visual approval (2026-09-17)

The operator approved the Lex curiata visual specification: a fullscreen
trusted takeover, an immutable frozen workspace backdrop dimmed and blurred,
and a central Instrument-styled authorization panel. CORVUS / LEX CURIATA
identifies the trusted scene; Provincia and Term show the requested authority
and its exact lifetime. See `docs/HALCYON-TRUSTED-EPISODE.md` for the display and
input ownership contract. This is an approved visual design, not a claim that
the ordinary userspace compositor is a trusted sink. Serial secure attention
remains the implemented path pending trusted scanout ownership.

Session inline admission uses the smaller of the transient heap cap and the
smallest live raster cache allowance. Text and raster caches already split the
shared content budget; admission must not halve the text half a second time.
Completion rechecks the current cap before replying, so a split during upload
cannot report success for a now-oversized raster. Session uploads require a
nonzero HPL2 image ID, and a token whose pane disappeared returns ENOENT;
the legacy console's id-less wire path remains separate.
