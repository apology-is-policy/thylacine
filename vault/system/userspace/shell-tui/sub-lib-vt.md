---
id: sub-lib-vt
type: sub
title: "vt -- the shared VT interpreter core, extracted and now host-tested"
parent: moc-userspace-shell-tui
code:
  - usr/lib/vt/src/lib.rs
  - usr/lib/vt/Cargo.toml
audit: light
guarded-by: []
validated-by: [prose]
locks: []
hazards: []
abis: []
design: ["docs/AURORA.md", "docs/HALCYON.md section 13.4", "docs/HALCYON.md section 14.3", "docs/KAUA-TERM.md", "docs/UTOPIA-VISUAL.md section 1", "docs/AURORA-CONFIG.md"]
created: 2026-09-05
updated: 2026-09-28
---
## Purpose

A byte stream in, a cell grid out. This is the screen-side of the terminal
protocol -- [[sub-kaua]] is the app-side that *emits* the escape sequences,
and vt is what interprets them back into a grid of coloured cells. It covers
the VT100 core plus exactly the subset the tree's own emitters produce
(libutopia's ANSI + truecolour SGR, Kaua's cursor/erase/alt-screen, login's
plain lines); anything else is parsed and dropped rather than allowed to
desync the stream.

It exists as a crate because three consumers need the identical
interpretation and cannot afford to drift: [[sub-aurora]] hosts one `Vt` per
console surface, and halcyond hosts one per raw-VT pane plus a second,
SGR-only instance for its transcript. Extracted from aurora's `vt.rs` at H-2a
(behaviour-preserving) and the SGR pen split out at H-2a+1, so one `CSI ... m`
implementation drives every consumer.

The headline of the extraction is not reuse -- it is testability. As a module
inside the unconditionally-`no_std` aurora crate the parser could not be
compiled for the host at all; here it is a pure `no_std` + `alloc` crate with
zero dependencies, and the whole byte machine is exercised by 90 host tests
(2026-09-28; `cargo test -p vt --lib --no-default-features --target
aarch64-apple-darwin` from `usr/`, or `tools/test-rust.sh vt`). The most exposed surface
in the terminal stack -- the machine that eats every byte any program writes
to the console -- went from untestable to covered by the move alone.

The name is the reader-expected standard term, resolving HALCYON.md 13.9's
held thematic slot in favour of clarity (the naming discipline's "don't force
it" rule).

## Contract

`feed(bytes)` drives the grid and is all the console renderer needs: it
interprets the whole slice, mutating `cells`, the cursor, and the per-row
`dirty` vector. `feed_until(bytes, &mut pos)` is the resumable variant for the
event-capture consumer (KT-1): it returns at the first `Boundary` with `pos`
advanced past the triggering byte, so a chunk split across reads resumes
correctly from persisted parser state. With capture off it consumes the whole
slice and returns `None` -- byte-for-byte the behaviour of `feed`, which is
the property that lets aurora ignore the entire event machinery.

`new(cols, rows)` births a Bonfire grid; `with_palette(..)` births it in a
given palette (a per-tile kaua-term uses `DAYLIGHT` so its cells carry the
compositor's theme, since the seam ships resolved RGB). `resize(ncols, nrows)`
reweaves content-preserving. `set_theme(idx)` remaps live cells to a new
palette. `app_cursor()` exposes DECCKM for the key re-encoder.
`sync_output()` says a program holds a synchronized frame open (DEC private
mode 2026) and `sync_frames()` counts the frames it has opened, wrapping.
`FrameHold` is a renderer's bound on how long such a frame may hold its paint
(`SYNC_HOLD_NS`, 150 ms): it lives here because both renderers, halcyond and
[[sub-aurora]], depend on vt and share no other crate.

Two public queues are the caller's to drain: `reply` holds bytes the terminal
must answer (the CPR report, a DECRQM mode report), which the main loop writes into the keyboard
wire exactly as a real terminal would; `settings_req` holds `key value` lines
pushed through the in-band config channel. The pixel side -- atlas blit,
damage-to-present -- stays entirely with each consumer; vt never sees a pixel.

## Mechanism

**The parser is a six-state byte machine** (`Ground`/`Esc`/`EscCharset`/
`Csi`/`Osc`/`OscEsc`) with UTF-8 assembled in the ground state. Unknown
finals and malformed sequences abort to `Ground` without touching the grid --
"parse and drop, never desync" is the governing rule, and it is why a hostile
or simply unfamiliar stream can only ever produce wrong-looking output, never
a wedged interpreter.

**A CSI sequence is read whole, marks included (FL-1, DEC STD-070 and
ECMA-48).** A private marker (`?` `<` `=` `>`) counts only as the first byte;
intermediates (0x20-0x2F) are recorded, more than one making the sequence one
no handler knows; a marker after a parameter, or a parameter after an
intermediate, marks it malformed, and it is read to its final and ignored whole.
C0 controls inside a CSI run in place, ESC abandons it and begins the next, CAN
and SUB cancel it. `dispatch_csi` routes on (marker, intermediate): plain to the
ANSI handlers, `?` to the DEC ones (`h`/`l` modes, `J`/`K` as DECSED/DECSEL,
which are ED/EL here since no cell is protected, `n`), `$ p` with or without
`?` to DECRQM, and anything else is ignored whole. The parser used to drop the
marks and run the plain handler, so xterm's `CSI > 4 ; 1 m` printed `4;1m`
(its marker aborted the parse and the tail fell to ground as text), kitty's
`CSI ? u` restored the cursor, and DECCARA (`CSI ... $ r`) reset the scroll
region as a DECSTBM.

**Synchronized output, DEC private mode 2026 (FL-1, HALCYON 14.3).**
`?2026h` opens a frame and `?2026l` closes it; `set_sync` records the mode and,
on a CHANGE only, counts an open (`sync_frames`) and pushes `Boundary::Sync(on)`
under capture, in stream order with the cells, so a second open inside a frame
and a close with none open are nothing. RIS closes an open frame LAST, after its
erase, so the reset's own erase is inside the frame it closes. The parser keeps
applying every byte while a frame is open; only a renderer waits. DECRQM (`CSI
[?] Ps $ p`) answers `CSI [?] Ps ; Pm $ y` into `reply`: 1 set or 2 reset for
the DEC modes the parser tracks (1, 6, 7, 25, 47, 1047, 1049, 2026), 0 for any
other and for every ANSI mode, so a program that asks before using the mode
(neovim, notcurses) gets a true answer.

**`FrameHold` bounds the wait on the renderer's side.** `open()` and `close()`
follow the frame, and `cut()` ends the hold without the frame's close (a
reconfigured surface, the program's end); `holds(now)` asks whether the paint
due at `now` waits, and the first paint it defers starts the bound; `painted()`
ends it and reports `Held::No`, `UntilClose(n)` (the program closed its frame:
shown whole), `Cut(n)` (cut, or painted while the frame was still open) or
`UntilTimeout(n)`, so only the program's own close reads as a whole frame;
`waiting()` says a paint is
deferred inside an open frame, the renderer's cue to block rather than loop
back; `due_ms(now)` is the wait to the deadline, rounded UP so a wakeup never
lands before it and spins, and 0 whenever the next `holds` would paint instead
(a clock gone dead, or run backwards). The bound
survives a close and a reopen, so no due paint waits longer than 150 ms even
under back-to-back frames; a timeout abandons the frame (the same frame is
never held again, so a program that never closes one costs one stall, never a
standing slowdown); and a clock reading 0 (`monotonic_ns` failing soft) or
running backwards never holds.

**The span serial threads Beacon frames to cells, without a second parser
(H-4d).** In capture mode the parser keeps a monotonic `span_serial`: a Beacon
frame (OSC 1936) advances it by one (skipping 0 on wrap) and copies it into
`Vt.span`, which every subsequent `Cell.span` inherits; any other OSC (a title,
0 or 2) leaves it untouched. The frame's raw body rides out on the new
`Boundary::Osc { serial, body }` variant -- the parser NEVER reads a Beacon body
(R5: one parser, and it lives in the consumer). The consumer parses the forwarded
frames in order, maps each serial to the presentation state *after* that frame
(obj / em / hdr), and so recovers a cell's markup from its `span` alone. Blanks
from erase or scroll fill carry span 0. This is what lets a session tile render
Beacon presentation over a pure cell grid without vt growing a Beacon parser.

**Autowrap is deferred, and that is load-bearing rather than cosmetic.** A
glyph written to the last column leaves the cursor *at* `cols` (past the
edge); the wrap happens when the *next* glyph arrives (`put_char` resolves
`cx >= cols` first). Honouring DECAWM reset (`?7l`) matters because Kaua
paints the bottom-right cell deliberately: without the deferred model, every
last-cell paint armed an immediate line-feed and the next run scrolled the
whole screen once per status repaint -- the nora artifact cascade (#37).

**The cursor-position report is answered.** `CSI 6n` pushes `ESC [ row ; col
R` into `reply`. Kaua's size handshake (save, park the cursor far off-screen,
`6n`, restore) reads that report to learn the real grid; an unanswered
request strands every Kaua application at its 80x24 fallback inside a larger
grid (#37). The reply rides the same wire as keystrokes, so it cannot
overtake typing.

**The in-band settings channel is allowlisted twice.** `OSC 7770;aurora;
<key>;<value>` lands in `settings_req` as a config-grammar line; the parser
rejects any control byte in key or value. That second check is not defensive
tidiness: the config parser re-splits values on newlines, so an embedded
newline once laundered a second statement past a single-token allowlist and
reached the compositor tier. The channel is session-scoped by scripture
(never persisted), bounded (256-byte payload, 16-deep queue, drop beyond),
and cosmetic-only -- the xterm dynamic-colours threat model applies because
any console writer can emit it.

**KT-1a widened the covered subset**: DECSTBM scroll regions, DECOM origin
mode, SU/SD band scrolls, double-width glyphs, and the italic/dim/blink/
strike SGR attributes are all honoured. Aurora never sets DECSTBM, so the
full-screen default preserves its behaviour exactly -- the shared-crate
contract is that the console path is unchanged. Full wide/attribute
*rendering* is each consumer's job (KT-1c/1d); vt only tracks the geometry
(`ATTR_WIDE` on the left half) and the pen bits.

**The alt-screen switch carries autowrap.** 1049 is an implicit DECSC on
enter / DECRC on leave, and DEC STD-070 saves autowrap with the cursor -- so
a TUI's `?7l` inside the alt screen cannot leak a wrap-off main screen back
out (the G-5 F5 close). `CSI s`/`u` stays position-only, deliberately.

**Boundary capture is off by default.** With `set_capture_events(false)` --
the console renderer's state -- the leaf handlers push nothing and every path
is byte- and allocation-identical to before the KT-1 machinery existed. The
kaua-term turns it on so `feed_until` yields the ordered seam stream: a
`Scroll` carries the row leaving the top into the transcript, `AltEnter`/
`AltLeave` carry the outgoing/restored buffer so the consumer flushes its
pending diff against the right grid, `Bell` and `Osc` delimit Beacon zones.

**The main screen's wrap flags, whichever screen shows (TC-1b).**
`main_wrapped()` returns the main screen's soft-wrap flags on either screen:
while the alt screen shows, they sit in the swapped-away buffer
(`alt_wrapped`), which `wrapped()` does not read. It pairs with
`main_top_continues()`. kaua-term's `AltEnter` flush sends the outgoing main
screen's last diff with them, because its consumer paints that frame until the
alt screen's blank diff lands ([[sub-kaua-term]]).

**A whole-screen erase is reported, and what it erased is handed over first
(TC-1, HALCYON 14.13).** A clear is ED 2 or ED 3 (one arm; DECSED is ED here,
no cell being protected) or RIS; parameters past 3 are ignored as xterm ignores
them, and ED 0 and ED 1 never count, whatever they cover -- ED 0 from a prompt's
top is ut's per-keystroke redraw (`\r ESC[J`), which read by its effect filed
every keystroke after a clear into the history (TC-1a audit F1). On the normal screen under capture, `screen_to_history` pushes a `Scroll`
for every row through the last one holding a non-space character -- the tile's
own content test -- BEFORE the blank, the last of them with its wrap flag cleared
because nothing continues it once the screen is blank; `note_screen_erased` then
pushes `Boundary::ScreenErased` AFTER it. So the consumer's history receives the
erased screen exactly as if it had scrolled off, and the erase arrives after it:
no clear deletes the record of what ran (the operator's vote, 2026-09-25); the
live screen itself stays the program's to rewrite.
The alt screen reports neither, since it has no history and no pin. With capture
off nothing is pushed and the cells are untouched by the reporting, which a test
proves by feeding one stream with capture on and off and comparing the grids.
RIS keeps history too, unlike xterm's, which drops saved lines. RIS returns to
the main screen FIRST, as xterm, kitty and VTE do: `alt_screen(false)` pushes the
`AltLeave` (under capture) and the erase then acts on the main screen, so `reset`
rescues a screen a crashed TUI left on the alt buffer; RIS also restores autowrap
(`wrap`) and forgets the saved cursor (`saved`, `saved_wrap`). `span` is left
alone: it is the Beacon frame serial, positional by design, not pen state.

**Row 0's restart is reported on its edge (TC-1a audit F5).** When row 0
restarts as a line of its own while it continued a row that scrolled off,
`restart_top` pushes `Boundary::TopRestart` once, on that true-to-false edge,
under capture only. Its callers: a glyph at (0,0), except one this same call's
autowrap put there (on a one-row screen that glyph IS the continuation, round-2
F2); ED reaching row 0's first cell; EL from row 0's column 0; IL/DL at row 0;
RI/SD at the top margin; RIS. The producer flushes on it, so the consumer ends
the fragment it holds before the next row leaves; coalesced into one
`ScrollOff`, the two rows used to glue. Inside an ED 2/3 or RIS byte the erased
rows are pushed BEFORE the restart, so row 0's continuation reaches the
fragment before it is ended (round-2 F4).

**AltLeave carries the main screen as it stood (round-2 F1).** The boundary
holds the restored main's cells, wrap flags, cursor and top flag at the leave,
because the byte that left can go on to change them: RIS leaves, then erases,
and a top flag read after the erase ended a fragment the restored row 0 still
continued.

## Data structures

`Vt` is the whole interpreter: the two cell buffers (main + alt), cursor and
saved-cursor state including autowrap, the parser state machine and its param
array (`MAX_PARAMS` = 16) with the CSI marks (`csi_marker`, `csi_inter` where
0xFF means more than one, `csi_bad`), the DECSTBM band, the DECOM flag, the
mode-2026 state (`sync`, `sync_frames`), the two output queues, UTF-8
assembly, the per-row `dirty` vector, and the KT-1 capture flag +
pending-boundary queue.

`FrameHold` is five words: `open`, `since` (when the oldest paint not yet
shown was first deferred; 0 = none), `held` (paints deferred), `expired` and
`cut` (the hold ended while the frame was open).

`Cell` bakes *resolved* colours at write time (`ch`, `fg`, `bg`, `attrs`),
which is exactly what makes a theme switch a remap-by-exact-match rather than
a re-interpretation; truecolour passes through a switch untouched by design.

`SgrPen` is the fg/bg/attrs triple one `CSI ... m` mutates, extracted at
H-2a+1 so halcyond's transcript drives the same SGR logic per block. The
load-bearing detail is that BOLD promotes a base-tier ANSI foreground to the
bright tier at application time, so `1;31` and `31;1` resolve identically; bg
never promotes; an empty parameter list is the full reset.

`Palette` is `bg` + `fg` + `ansi[16]`. `THEMES` holds the three
user-selectable palettes (Bonfire, Parchment, Spinifex); `DAYLIGHT` is the
compositor's render palette, deliberately *not* in `THEMES` because it is not
a `set_theme` choice. The 16-colour ANSI map derives from the UTOPIA-VISUAL
role table (slate=blue, sage=cyan, cinnabar=red, ember=bright-red); the bright
tier is aurora's own derivation, documented in the source.

`Boundary` (Scroll / Bell / ScreenErased / TopRestart / Sync / Osc / AltEnter / AltLeave) is the
KT-1 event enum, inert when capture is off.

## Concurrency

None. Each consumer owns its `Vt` instances outright and drives them from a
single thread; there is no shared state and no lock. Correctness against the
consumer's servers (the console, the compositor) is that consumer's loop
ordering, not vt's concern.

## Invariants enforced

None of the numbered system invariants. vt is a pure byte-transform library:
no syscall, no capability, no handle. In particular **[[inv-i27]] lives in
[[sub-aurora]], not here** -- the trusted-path drain/feed role is aurora's;
vt is merely the parser aurora feeds, and holds no authority to leak.

Its own load-bearing rules, enforced by construction + the host suite rather
than by a kernel check:

- **The settings channel must never gain a persisting or authority-bearing
  key** -- any console writer can emit `OSC 7770`, so its power is capped at
  cosmetic + session-scoped, and control bytes are rejected before a value is
  ever re-parsed.
- **A glyph occupies exactly its columns** -- one for narrow, two for wide
  (left marked `ATTR_WIDE`, right a pen-carrying blank), zero for combining;
  the grid can never desync from the cursor.
- **Capture-off is byte-identical to the pre-KT-1 machine** -- the property
  that lets the console renderer share the crate at zero behavioural cost.

## Error paths

Everything degrades rather than faults, which is correct for a machine fed
untrusted bytes:

- Malformed escape aborts to `Ground`; a malformed CSI (a marker after a
  parameter, a parameter after an intermediate) and one carrying marks no
  handler expects are read to their final and ignored whole.
- CSI parameters saturate on overflow (`saturating_mul`/`add`) -- no panic
  on `CSI 99999999999m`.
- Zero geometry is clamped to `max(1)` at birth *and* on resize, so a
  compositor-supplied `0x0` cannot underflow `scroll_bot = rows - 1` or the
  `cy*cols+cx` index (F2).
- A double-width glyph in a grid too narrow to hold one (`cols < 2`) degrades
  to single-width, so the continuation write cannot run off the row and `cx`
  cannot exceed `cols` (the F1 P0 that would later underflow ICH/DCH/ECH).
- Erase at a deferred-wrap position on the last row is OOB-safe (a named
  regression test).
- An oversize OSC payload is swallowed and discarded at its terminator; the
  settings queue drops beyond 16.

## Performance

Per-row damage: the consumer re-renders only rows whose `dirty` flag is set.
The byte machine avoids `core::fmt` on the hot path -- the CPR formatter is a
hand-rolled decimal (`push_dec`, and `push_u32` for DECRQM). No allocation occurs on the console path
beyond the two grid buffers and the (empty, on that path) queues.

## Prosecution

- **Deferred wrap must be resolved before any grid mutation that indexes at
  the cursor.** `put_char` does it; the erase/insert/delete paths assume `cx`
  is in range. The last-row deferred-wrap erase is the exact case that was
  OOB before the guard.
- **The OSC 7770 allowlist must reject control bytes in both key and value.**
  The config parser re-splits on newlines downstream; a laundered newline
  reaches the compositor tier and a later overlay-save would persist it.
- **The `cols < 2` double-width degrade must stay.** Removing it lets a wide
  glyph's continuation write off the row end.
- **The alt-screen enter/leave must save and restore autowrap, not just
  position.** Otherwise a full-screen app's `?7l` leaks a wrap-off main
  screen (G-5 F5).
- **`set_theme` depends on slot-uniqueness within a palette.** Cells carry
  resolved colours, so the remap matches old-to-new exactly; a colour
  appearing in two slots of one palette but one of another mis-maps on the
  round trip. Every palette aliases exactly one slot (`ansi[15]`) to `fg`,
  consistently, and no other -- `ansi[0]` is kept distinct from `fg` for
  precisely this reason.
- **Capture-off must remain byte-identical.** Any push into `pending` on the
  console path is a regression against the shared-crate contract; `feed`
  clears `pending` defensively so a stray capture cannot leak into a later
  `feed_until`.
- **A whole-screen erase: the rows BEFORE the blank, the report AFTER it, the
  alt screen never.** Pushed after the blank, the rows would be blank; a report
  before the rows would let the consumer's pin be released by the erase's own
  ScrollOff. Each arm -- the effect test per ED mode, both guards on both
  helpers, the cleared last wrap flag, RIS -- is held by a named test that a
  one-arm sabotage reds (the TC-1a sweep: 22 legs, each redding exactly its
  predicted tests; the RIS fix's alt-leave and each of its three mode resets
  have a leg of their own, the resets checked at their own assertion).
- **A marked or intermediate CSI must reach only a handler that expects its
  marks.** Each aliasing shape (a private marker spilling as text, a marked
  sequence running the plain handler, DECCARA running as DECSTBM, an
  out-of-order sequence, a C0 or ESC inside one) has a named test; all six went
  red on the parser before FL-1 (sabotage S0).
- **Mode 2026: only a change is an event, and RIS closes last.** A set without
  the change check (S2) and a RIS that closes before its erase (S3) each red
  their tests.
- **The hold's bound runs from the first deferred paint, and an open never
  re-arms it.** Re-armed, a stream of back-to-back frames holds the paint
  forever (S6 reds `back_to_back_frames_do_not_extend_the_hold`). A timeout
  that does not abandon the frame re-holds the same frame after every paint
  (S5); a hold on a dead clock never ends (S4); a deadline rounded down wakes
  early and spins (S7); `waiting()` true on an open frame with nothing deferred,
  or a dead clock's deadline read as none (S17, S18). Each sabotage reds exactly
  its predicted tests.

## Seams

- DECOM confines CUP/VPA to the band, but relative moves (CUU/CUD/CUF/CUB)
  are not band-confined -- a full-DECOM refinement, deferred.
- No scrollback in the grid; a normal-mode top-margin scroll hands the
  leaving row out as a `Scroll` boundary (the transcript's job) and forgets
  it, and a whole-screen erase hands out every row through the last with text
  the same way.
- Application keypad (DECKPAM/DECKPNM) is deferred with its keycodes -- the
  shared KeyEvent model has no keypad keys to re-encode yet.
- The SGR sub-parameter separator `:` is folded to `;` (adequate for the
  tree's emitters, which use `;`).
- ANSI (non-private) `h`/`l` modes (IRM etc.) are not implemented, and
  DECRQM says so (Pm 0).
- Heavy/double line-weight box characters render as light at these cell sizes
  in the consumers; diagonal box characters are unsupported. (These are
  rendering seams in [[sub-aurora]], not the parser's.)

## Caveats

- **This crate is the resolution of [[sub-aurora]]'s old "eighteen tests
  cannot compile" caveat (task #153).** Those tests were written against the
  parser while it lived inside the unconditionally-`no_std` aurora crate,
  where `cargo test` could not build them. The extraction made the parser a
  pure host-testable crate, and the suite -- 90 tests on 2026-09-28 -- runs. It
  includes the two named security regressions that had *never executed* as
  aurora tests: the escape-laundering fix and the out-of-bounds erase fix,
  both reachable from any console writer.

- **The host suite covers the parser; it does not cover rendering.** The
  pixel side (`render.rs`, the atlas blit) stays with each consumer and still
  needs the runtime, so it is proven by the in-guest end-to-end batteries,
  not here. vt's tests assert grid state, cursor position, boundary streams,
  and OOB safety -- not what reaches the screen.

- **vt tracks wide/attribute geometry; it does not render it.** `ATTR_WIDE`
  and the pen attributes are set correctly, but drawing a double-width cell
  two-wide, or italic as slanted, is KT-1c/1d work in the consumer.

- **Currency (2026-09-28): this dossier was brought current for TC-1, TC-1b and FL-1 only.**
  The vt's changes between 2026-09-05 and 2026-09-22 (about 1200 lines: the
  PL-3/PL-4 soft-wrap flags, `top_continues`, the reflowing resize, the palette
  seam) are not yet described here. Dating this edit stopped `quaestor stale`
  from flagging the dossier, so the debt is recorded here instead.

## Provenance
(generated -- incoming `touched` backlinks, newest first; never hand-written)
