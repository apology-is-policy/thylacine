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
design: ["docs/AURORA.md", "docs/HALCYON.md section 13.4", "docs/UTOPIA-VISUAL.md section 1", "docs/AURORA-CONFIG.md"]
created: 2026-09-05
updated: 2026-09-05
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
zero dependencies, and the whole byte machine is exercised by ~46 host tests
(`cargo test -p vt --target aarch64-apple-darwin`). The most exposed surface
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

Two public queues are the caller's to drain: `reply` holds bytes the terminal
must answer (the CPR report), which the main loop writes into the keyboard
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

## Data structures

`Vt` is the whole interpreter: the two cell buffers (main + alt), cursor and
saved-cursor state including autowrap, the parser state machine and its param
array (`MAX_PARAMS` = 16), the DECSTBM band, the DECOM flag, the two output
queues, UTF-8 assembly, the per-row `dirty` vector, and the KT-1 capture
flag + pending-boundary queue.

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

`Boundary` (Scroll / Bell / Osc / AltEnter / AltLeave) is the KT-1 event
enum, inert when capture is off.

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

- Malformed escape/CSI aborts to `Ground`; the sequence is dropped.
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
hand-rolled decimal (`push_dec`). No allocation occurs on the console path
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

## Seams

- DECOM confines CUP/VPA to the band, but relative moves (CUU/CUD/CUF/CUB)
  are not band-confined -- a full-DECOM refinement, deferred.
- No scrollback in the grid; a normal-mode top-margin scroll hands the
  leaving row out as a `Scroll` boundary (the transcript's job) and forgets
  it.
- Application keypad (DECKPAM/DECKPNM) is deferred with its keycodes -- the
  shared KeyEvent model has no keypad keys to re-encode yet.
- The SGR sub-parameter separator `:` is folded to `;` (adequate for the
  tree's emitters, which use `;`).
- ANSI (non-private) `h`/`l` modes (IRM etc.) are not implemented.
- Heavy/double line-weight box characters render as light at these cell sizes
  in the consumers; diagonal box characters are unsupported. (These are
  rendering seams in [[sub-aurora]], not the parser's.)

## Caveats

- **This crate is the resolution of [[sub-aurora]]'s old "eighteen tests
  cannot compile" caveat (task #153).** Those tests were written against the
  parser while it lived inside the unconditionally-`no_std` aurora crate,
  where `cargo test` could not build them. The extraction made the parser a
  pure host-testable crate, and the suite -- now ~46 tests -- runs. It
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

## Provenance
(generated -- incoming `touched` backlinks, newest first; never hand-written)
