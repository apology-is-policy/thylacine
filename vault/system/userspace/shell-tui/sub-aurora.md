---
id: sub-aurora
type: sub
title: "aurora — the console renderer, and eighteen tests that cannot compile"
parent: moc-userspace-shell-tui
code:
  - usr/aurora/src/main.rs
  - usr/aurora/src/vt.rs
  - usr/aurora/src/render.rs
  - usr/aurora/src/osd.rs
  - usr/aurora/src/config.rs
  - usr/aurora/Cargo.toml
audit: hard
guarded-by: [inv-i27]
validated-by: [prose, gate-interactive]
locks: []
hazards: []
abis: []
design: ["docs/AURORA.md", "docs/AURORA-CONFIG.md"]
created: 2026-08-04
updated: 2026-08-04
---
## Purpose

The console on a screen. Everything else in this area paints *into* a
terminal; aurora paints the terminal itself — it reads the byte stream
every program writes to the console, interprets it into a cell grid, sets
that grid in the system face, and presents it as pixels through the
compositor.

So it sits at the far end of the same stack: [[sub-kaua]] emits escape
sequences, [[sub-utopia-interactive]] emits a prompt, and aurora is what
turns either into something a person can see. It is an ordinary
[[sub-libtapestry]] client — the same protocol any graphical program
speaks — with two extra file descriptors that make it the console's
renderer.

## Contract

Two kernel leaves define the role. `/dev/consdrain` mirrors every console
output byte — program output and line-discipline echo alike — and aurora
reads, interprets and blits it. `/dev/consfeed` is the reverse: decoded
keyboard runes enter the *existing* line discipline, so cooking, echo and
interrupt handling are unchanged and typing paints itself by coming back
around through the drain.

Both leaves are gated on the renderer role. Aurora is spawned holding it
and nothing else: the role conveys no elevation authority and no
interrupt-target authority. Kernel diagnostics stay on the serial line by
design — the screen shows the *session*.

Beyond the console it reports its cell grid as the window size, applies a
persisted configuration at startup, and offers a settings overlay bound to
one key.

## Mechanism

**The loop blocks on the compositor's event stream, and that is
load-bearing rather than stylistic.** A non-polling async ring's
completions are pumped by the thread blocked in the submit call, so a
loop that never blocks never pumps and no event ever materializes — the
frame clock went silent under a poll-only loop, measured. Each wake
handles its event, drains the rest, services the console drain
non-blockingly with a bounded per-pass budget, then renders and presents.

**Damage is per row, and the present rect must cover exactly what was
just rendered.** Slots rotate on every present, so presenting rows this
pass did not render would transfer stale content from an older slot. The
loop therefore renders the contiguous dirty span and presents exactly
that rectangle — and when a full frame is needed (a resize, an open
overlay) it fills, renders everything, and presents the whole surface.

**Refused input is held rather than dropped, and held input is what
bounds the wait.** The kernel's feed returns a short count when its input
ring is full — back-pressure, not an error, so the refused bytes are
still the sender's. Aurora keeps them and retries at the top of each pass:
unconditionally, because back-pressure clears when the console's *reader*
drains, which is an event aurora never sees. Deliberately one write
attempt per pass, never a retry loop, because a foreground child that
stops reading parks the input path indefinitely by design and spinning
would wedge the compositor for every other client.

**And the two halves of that had to be fixed separately.** Holding the
bytes is useless if the loop stops turning — the compositor's frame ticks
reach visible surfaces only, so a backgrounded aurora receives nothing at
all and an untimed block parks the user's keystrokes until they come
back, whereupon the bytes land in whatever the shell is doing *then*. So
the wait is bounded exactly when the queue is non-empty. The rejected
alternative is recorded: discarding held input on unfocus would also throw
away input that was about to be delivered correctly, since focus loss also
fires while the surface stays visible.

**Over-bound loss drops the NEWEST, and the reasoning is not symmetric
with output.** The kernel's output tap drops oldest, explicitly so the
newest output — the prompt the user needs — survives. That does not
transfer to input: dropping a prefix can leave a complete but *different*
command where dropping a suffix leaves an incomplete one the shell
rejects.

**The interpreter is a plain byte machine over the tree's own emitters
plus the classic core.** Unknown sequences are parsed and dropped rather
than desyncing. Two behaviours are load-bearing rather than cosmetic:
autowrap is honoured (ignoring it made every bottom-right cell paint
scroll the whole screen, once per status repaint), and the cursor-position
report is *answered* — the reply rides the same input queue as keystrokes,
so it cannot overtake typing, and without it every full-screen application
falls back to an 80x24 guess inside the real grid.

**The in-band settings channel is allowlisted twice.** A session can push
cosmetic settings through an escape sequence, and the parser rejects any
control byte in the key or value — because the configuration parser
re-splits values on lines, so an embedded newline once laundered a second
statement past a single-token allowlist and reached the compositor tier,
which a later overlay save would have persisted.

## Data structures

`Vt` is the grid: two cell buffers (main and alternate), cursor state,
saved cursor state including autowrap, the parser state machine, a live
palette, a per-row damage flag vector, and two output queues — bytes the
terminal must answer, and settings the session pushed.

`Cell` bakes *resolved* colours at write time, which is what makes a theme
switch a remap by exact old-to-new match rather than a re-interpretation.
True colour passes through untouched by design.

`Metrics` pairs the cell dimensions with the atlas they came from, so a
font-size change swaps both together — a stale atlas beside new dimensions
would blit the wrong-size alpha slice.

`Settings` spans three tiers that behave differently: renderer-local
(theme, cursor, font — applied directly), compositor (mode, chords, gaps —
pushed through a gated control channel), and the overlay's own state.

## Concurrency

None within the process. One thread, one loop.

Cross-process, it is a client of two servers — the kernel console and the
compositor — and its correctness against both is the loop's ordering, not
a lock.

## Invariants enforced

**[[inv-i27]]** in its third role. The trusted path distinguishes console
*attachment* (the elevation gate) from console *ownership* (the interrupt
target); aurora holds neither. The renderer role is the third, and its
whole content is the drain/feed pair.

That separation is what the dossier should make legible: aurora sees every
byte the console emits and injects every byte the keyboard produces, and
still cannot elevate, cannot receive an interrupt aimed at the session,
and cannot forge the attention key — the kernel hardwires that signal
false on the feed path. A renderer is a very powerful position and a
deliberately unprivileged one.

It also carries the authority *end* of the compositor's gated control
channel: mode changes ride aurora's own connection, because the gate
checks the connection's kernel-stamped peer and a shared mount's peer is
the mounter.

## Error paths

Fail-loud at startup, fail-soft afterwards — and the split is
deliberate. A missing atlas, a refused drain open, a refused feed open or
a degenerate grid all exit before a surface exists, leaving the display to
whoever else might present. After the console is up, almost nothing is
fatal: a failed configuration save logs, a refused mode push heals the
persisted setting back to automatic, a failed window-size write degrades
clients to their own probe.

**A present failure is a dropped frame, not a death.** The dirty rows stay
set so the next pass re-renders and re-presents; only a long run of
consecutive failures exits. Real compositor death ends the event stream
instead, which is the actual exit path.

**A too-large font steps down rather than failing.** A persisted size that
does not fit walks to the largest baked size that does, so a saved
preference can never brick the console — and the runtime fit is not
persisted, so the preference survives a move to a bigger display.

## Performance

Row-granular damage against a 60 Hz frame clock. The expensive operation
is the per-cell alpha blend, and the whole-frame path is taken only for a
resize, an open overlay or a theme change.

The blend has a history worth carrying: it packs two colour lanes into one
word, which is only lane-safe with the specific shift form it now uses.
An earlier divide-based version divided the *packed* word — and integer
division does not distribute over packed lanes — so interiors stayed
exact (the fully-opaque and fully-transparent cases short-circuit) while
every antialiased *edge* pixel got a garbage blue correlated with its red.
Thin glyphs read violet. The bug lived exactly where the short-circuits
did not reach.

## Prosecution

- **The drain and feed opens must precede surface creation.** Without the
  role they fail, and the surface should never exist.
- **A held-input queue and a bounded wait move together.** Either alone is
  a defect: an unbounded queue is unbounded memory, and an unbounded wait
  parks the queue forever on a hidden surface.
- **One write attempt per pass, never a retry loop.** The console reader
  may legitimately have stopped.
- **The over-bound report stays latched.** Diagnostics route through the
  console, which takes the writer role and can wait for room — so an
  unlatched report against a stalled reader makes aurora stall on its own
  logging.
- **Every present rect covers only rows this pass rendered.** Slot
  rotation makes any wider rect a transfer of stale pixels.
- **A shrinking resize invalidates the remembered cursor.** The remembered
  position is loop-local and the grid resize does not touch it, so a stale
  row indexes past a shrunk damage vector — an out-of-bounds panic, which
  under abort-on-panic is a dark console.
- **The settings channel must never gain a persisting or authority-bearing
  key.** Any console writer can emit it.

## Seams

Scroll regions are accepted and ignored (full-screen scroll only). No
scrollback. Heavy and double line weights render as light at these cell
sizes, though the arm mask is exact so joins still connect. Diagonal box
characters are unsupported.

The overlay leaks the auto-repeats of a held close key to the terminal
after closing; a tap does not.

## Caveats

- **Eighteen tests cannot compile, and one module says they run** (task
  #153). The crate is unconditionally no-standard-library, so `cargo test`
  fails with nineteen "can't find crate for test" errors. Three of the
  four modules say so correctly — two of them naming the fourth
  explicitly — and [[sub-cornucopia]] names aurora as its counter-example.
  The dissenter is the interpreter, with nine of the eighteen, over the
  most exposed surface here: the byte machine that eats every byte any
  program writes to the console. Two of its nine are named security
  regressions that have never executed — the escape-laundering fix and the
  out-of-bounds erase fix, both reachable from any console writer. The
  refactor that would fix it is the one its siblings already name.

- **The interpreter's proof is therefore the in-guest end-to-end battery**,
  which is real (it drives keystrokes through the full path and asserts on
  rendered output) but is a different kind of proof: it exercises the
  paths a session takes, not the malformed-input paths the dormant tests
  were written for.

- **The window-size report at boot is silent by construction.** The
  transition from zero to a real grid is a change, so the kernel attempts
  a notification — but the boot console owner sits in a process group the
  delivery path refuses. Correct, and worth knowing before reading the
  boot log for evidence.

- **A resize acked smaller than offered would slip past the floor guard.**
  The sub-floor gate keys on the offered size while the resize adopts the
  acked one. The compositor acks what it offered, so the grids agree; the
  hypothetical is memory-safe either way and only cosmetically small.

- **The theme remap requires slot-uniqueness within a palette.** Cells
  carry resolved colours, so a switch matches old to new exactly — and a
  colour appearing in two slots of one palette but one slot of another
  would mis-map on the round trip. Every palette aliases exactly one slot
  to the foreground, deliberately and consistently.

## Provenance
(generated -- incoming `touched` backlinks, newest first; never hand-written)
