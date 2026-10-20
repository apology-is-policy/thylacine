---
id: sub-kaua
type: sub
title: "kaua — the console TUI substrate: a cell diff, a VT parser, and the crate whose tests actually run"
parent: moc-userspace-shell-tui
code:
  - usr/lib/kaua/src/lib.rs
  - usr/lib/kaua/src/term.rs
  - usr/lib/kaua/src/source.rs
  - usr/lib/kaua/src/intake.rs
  - usr/lib/kaua/src/query.rs
  - usr/lib/kaua/src/input.rs
  - usr/lib/kaua/src/encode.rs
  - usr/lib/kaua/src/buffer.rs
  - usr/lib/kaua/src/event.rs
  - usr/lib/kaua/src/style.rs
  - usr/lib/kaua/src/rect.rs
  - usr/lib/kaua/src/layout.rs
  - usr/lib/kaua/src/widget.rs
audit: light
guarded-by: [inv-i9, inv-i27]
validated-by: [prose, gate-interactive]
locks: []
hazards: []
abis: []
design: []
created: 2026-08-03
updated: 2026-10-08
area: userspace
---
## Purpose

The text weave — the immediate-mode, double-buffered TUI substrate a full-screen
native program draws on. The app redraws a whole back buffer each frame; kaua
diffs it against what the screen currently shows and emits only the changed
cells, as one batched escape frame. That is the ratatui model brought native, and
`nora` is its first consumer.

It is the counterpart to [[sub-utopia-interactive]] rather than a competitor:
`ut` owns the console's *mode* and hands a raw terminal to a child; kaua is what
that child uses to paint on it. Neither owns the other's half, and the split is
deliberate — kaua never touches the line discipline.

**Structurally this crate is two crates.** Nine modules are pure values and pure
functions with no I/O at all; three (`term`, `source`, `query`) sit behind a
`backend` feature because they are the only ones needing libthyla-rs. That is not
tidiness — it is what makes the pure nine host-testable, and unlike most of the
native tree, here the tests genuinely run.

## Contract

**The Terminal acquires the SCREEN, never the line discipline.** It writes fd 1
and nothing else: alternate screen, cursor visibility, autowrap, SGR, glyphs. Raw
termios is set by `ut` through its private consctl fd *before* the app is
spawned; kaua assumes bytes already arrive raw and never asks for that to be
true. So a kaua app is never console-attached, and the same API is honest for a
trusted or an untrusted caller.

**Input never takes a byte past the event in hand.** A kaua app borrows its
terminal: when it quits, or hands the terminal to a child, what was typed behind
its last key belongs to whoever reads fd 0 next. The source reads one byte at a
time and stops at the byte that completes an event, and the app stops asking at
an event that may end it.

**Input is a separate object from output.** `PollSource` reads fd 0; `Terminal`
writes fd 1; they share no state. The separation exists so the Loom seam is real
— a future `LoomSource` implementing the same `EventSource` trait replaces the
input half without touching the diff-to-fd-1 output half.

**The restore is best-effort, and kaua says whose job the real one is.** `Drop`
restores the screen on a clean return and `leave()` is idempotent. But a native
binary is `panic = abort`, so `Drop` does not run on a crash — `ut`'s post-reap
restore is the authoritative backstop. Both are idempotent precisely so both can
fire.

**The input parser is total.** `Parser::feed` accepts any byte sequence, holds
O(1) state, and never panics, loops unboundedly, or grows memory. The file states
this as its load-bearing property, and it is the right one to state: the bytes
come from a terminal the app does not control.

## Mechanism

**The frame cycle.** `draw(f)` resets the back buffer, lets the app paint into
it, then flushes. The flush collects the changed cells — the back-vs-front diff,
or every cell when a repaint is pending after `clear()`/`resize()` — and walks
them through `encode::render_cells`, which emits a cursor move only when the pen
is not already in place and an SGR only when the style changed from the previous
cell. The whole frame lands in one reused scratch buffer, written to fd 1 in a
single `write_all`, after which the real cursor is placed (or hidden) and front
takes back's contents.

**The input cycle.** The source hands the app one event at a time, and reads fd
0 one byte per `read(2)` up to the byte that completes it. A byte read into the
process could not be given back — there is no pushback into a terminal, and one
would be a forgery primitive — so leaving the type-ahead behind a quit key in
the kernel means never reading it. That is less(1)'s trade; an editor's bulk read
is what loses the line typed behind `:q`. The app still paints once per *burst*:
`PollSource::burst` waits the app's timeout for the first event and takes only
what fd 0 already holds for the rest, up to `BURST_MAX` (1024) events, so a paste
is handled as one run and a flood still repaints; bytes that complete no event
(NULs, an over-long CSI) are bounded separately, `QUIET_BYTES_MAX` (4096) per
call, so a writer of nothing cannot hold the app either. A bare ESC changes the next
wait to `ESC_HOLDOFF_MS`, so a split arrow key assembles; with nothing behind it
in that window the ESC is an Escape key. A half-collected CSI, SS3 or UTF-8
sequence waits across calls for its next byte and is never flushed. The decode
loop is `kaua::intake`, pure and driven by a scripted terminal in the host
tests; `kaua::source` supplies its two operations, a readiness wait and a
one-byte read.

**On a pts.** dev9p reports a pts slave's data fd as always readable, so the
source waits on the `/dev/pts/<n>ready` sibling and reads fd 0. Since #98 a
zero-timeout poll of the sibling answers from a fresh snapshot
([[sub-kernel-ninep-dev9p-poll]]), so an app's own mux and the source may both
poll it: nora polls `poll_fd()` beside its server pipes, then runs a
zero-timeout burst. (The port that added the sibling worked around the pre-#98
cache with a trust-the-mux mode; that mode is gone.) The launch probe polls the
same sibling and hands it to the source (`PollSource::with_probe`), so a program
holds one ready fid, the one ptyfs's budget of seven per pts counts
([[sub-ptyfs]]). A
pts slave whose ready file will not open falls back to polling fd 0, where no
timeout ever lapses, and kaua says so on the diagnostic UART.

**The size handshake.** The console has no winsize syscall, so `terminal_size`
does the standard CPR round-trip: save the cursor, park it at a far corner (the
terminal clamps to bottom-right), request its position, and parse the
`ESC[<rows>;<cols>R` reply — the clamped position *is* the screen size. Two
properties were bought the hard way. It is **bounded by a total deadline**,
re-polling the remaining budget per byte, so a reply dribbled a byte at a time by
a hypervisor's serial path still assembles while a slow peer still cannot
multiply the budget by the buffer capacity. On a pts it polls the ready sibling:
the data fd polls as always readable, so a read there waited for the reply or a
key, and a terminal that never answered held the app before its first frame.
On a boot with a display aurora answers every CPR on the console, a pts's
forwarded one included, so the hold needed a console-primary boot (no renderer)
under ptyhost. And it is **lossless**: bytes read
that are not part of the reply are returned as `pending` and replayed through the
steady-state parser, and the read stops at the `R` so later bytes stay in the
kernel ring. If the reply is slower than the whole budget, the steady-state
parser recognizes a late CPR as a resize — so the size still arrives and never
mis-keys.

Since the kernel grew a `/dev/winsize` leaf, `read_winsize` reads the
authoritative geometry directly, and the CPR path is the fallback for the serial
posture (where the leaf reports `0 0` because the host terminal owns the
geometry) and for a namespace too narrow to reach it.

**Layout** is a single-axis greedy solver, not a constraint system: fixed sizes
resolve first, the remainder is shared among the flexible slots. It covers an
editor body plus a status line, and a list pane plus a detail pane, which is
every v1.0 layout.

## Data structures

`Buffer` — a `Rect` plus a flat `Vec<Cell>` in row-major order; `Cell` is a char
plus a `Style`. A pure value with a `diff` yielding the changed positions.

`Style` — truecolor `Color` (`Reset` or `Rgb`) for fg and bg plus an OR-able
`Attr` bitset. `Rect` — four `u16`s in cell coordinates.

`KeyCode` / `Mods` / `KeyEvent` — the terminal-agnostic key model. `Char` carries
the already-cased grapheme, matching the crossterm convention, so `SHIFT` appears
in `mods` only for the non-text keys where a terminal actually encodes it.

`Parser` — the VT state machine: a fixed `PARAM_CAP` CSI buffer with an overflow
latch, a 4-byte UTF-8 scratch, a pending-escape flag, and a resize slot a
recognized CPR lands in.

`Terminal` — front and back buffers, the app cursor, a repaint flag, a reused
scratch `Vec<u8>`, and an `entered` guard so the restore runs exactly once.

`Intake` — the retained `Parser`, the launch probe's `pending` bytes not yet
replayed, and an EOF flag. A byte completes at most one event (the parser
surfaces a cursor report as a resize and no key), so nothing decoded waits. `PollSource` — a `PollSet` over
the readiness fd, stdin, the `Intake`, and which fd the wait polls. `Burst` — a
borrow of the intake and of fd 0 with a count; it ends at the first empty wait or
at `BURST_MAX`.

The widget set — `Block`, `Paragraph`, `List`, `Table`, `Tree`, `Tabs`,
`Scrollbar`, `StatusLine`, `Span` — are pure painters over a `Buffer` and a
`Rect`.

## Concurrency

None. Single-threaded by construction: an app owns one `Terminal` and one
`PollSource` and drives them from one loop. No lock exists in the crate.

Two ordering obligations are documented rather than enforced. `request_resize_probe`
emits a save/park/request/restore pair that must not interleave with a frame
emit, so it is single-threaded-callers-only. And the launch probe must run before
the `PollSource` exists, handing over its leftover bytes, or type-ahead is lost —
the `with_pending` constructor is what makes that transfer explicit rather than
implicit.

The crate consumes [[inv-i9]] rather than establishing it: the readiness wait's
correctness rests on the kernel's readiness edges not being lost between the
sample and the block. Reads are death-interruptible, so a dying app unwinds.

## Invariants enforced

None of its own — but the relationship to [[inv-i27]] is worth stating precisely
because it is a *negative* one. kaua touches fd 0 and fd 1 and nothing else. It
never opens consctl, never becomes console-attached, and cannot mint either. That
is why a kaua app is safe to run untrusted, and why the trusted-path gate is
unaffected by anything in this crate. The property is preserved by omission,
which makes it exactly the kind that erodes silently — a future module reaching
for consctl to do its own mode-setting would break it with no gate refusing.

The one property the crate states about *itself* is the input parser's O(1)
totality: no input, however long or adversarial, grows its memory or makes it
loop. A CSI parameter flood overflows into a latched flag and the sequence is
consumed to its final byte yielding no event; an invalid UTF-8 lead consumes a
bounded run and resets.

## Error paths

Uniformly degrade-not-fail. A failed size probe returns `None` and the caller
uses a fixed default. A ready file that will not open degrades the readiness
wait to fd 0, and says so on the diagnostic UART rather than on the screen the
app owns. An unreachable or malformed `/dev/winsize` returns `None`
and falls back to CPR. A write error during `enter` leaves `entered` false so the
restore does not fire on a screen never taken. `leave` is guarded by `entered` so
a double call is a no-op, and `Drop` ignores its result because there is nothing
useful to do with an error while unwinding.

The input side propagates real I/O errors from `poll` and `read`, and sets `eof`
on HUP, error, or a zero-length read — which is the loop's quit signal rather
than an error.

## Performance

One `write_all` to fd 1 per frame, from a reused scratch buffer sized 8 KiB at
construction. The diff means an idle screen costs nothing and a single changed
cell costs a cursor move plus a glyph. `render_cells` suppresses redundant moves
and redundant SGRs, so a run of same-styled adjacent cells emits just the glyphs.

Input costs a readiness wait and a one-byte read per byte: two local syscalls on
the console, two 9P round trips to ptyfs on a pts. That is the price of leaving
type-ahead in the kernel, and a pager's price. `BURST_MAX` bounds a burst at 1024
events, so a flood still paints. The ESC holdoff costs 50 ms once per genuine
lone-Escape press, the standard terminal tradeoff. Not yet measured: a paste
into nora in a tile, where each byte is two 9P round trips (some 8-12 thousand
for 4 KiB, against a handful of reads before).

## Prosecution

- **Does anything here reach for consctl?** The capability story is preserved by
  omission; a module that starts setting its own modes breaks it silently.
- **Is the restore still idempotent at both ends?** kaua's `Drop`/`leave` and
  `ut`'s post-reap restore must both be safe to run, in either order, because on
  a crash only the second one happens.
- **Does the parser still hold O(1) state?** The stated audit invariant. A new
  escape family with a growable buffer would void it.
- **Is the CPR probe still bounded in total, not per byte?** The distinction is
  the whole fix: a per-byte budget lets a dribbling peer multiply the wait by the
  buffer capacity. On a pts the bound holds only while the probe polls the ready
  sibling; `pts-probe.exp` runs prowl in a shell ptyhost hosts on a
  console-primary boot, where aurora is absent and nothing answers, and expects
  a first frame.
- **Is type-ahead still lossless?** The probe's leftover bytes must reach the
  steady-state parser through `with_probe`, and the read must still stop at
  `R`.
- **Does the source still stop at the event in hand?** A read of more than one
  byte, or a loop that decodes ahead, takes the type-ahead behind a quit key with
  it. `intake::tests` pin it, and turn red under both a drain and a missing
  holdoff.
- **Does every app stop asking at an event that may end it?** The intake leaves
  bytes unread only while the app stops pulling: an app that collects a burst
  before acting on it reintroduces the loss. prowl, quarry and lantern return
  from inside the burst on their quit key, quarry also stops at Play, and nora
  stops at the key that sets its quit flag.
- **Is a half-collected sequence still kept, not flushed, when input pauses?**
  Flushing it is how a split key becomes mis-keyed input.

## Seams

- **Unicode width is one column per char.** Wide CJK and combining clusters
  render inconsistently; the buffer stays coherent. Shared with `ut`'s line
  editor, and the same seam.
- **`LoomSource`.** The `EventSource` trait exists to be implemented a second
  time by a multishot Loom read; v1.0 has one implementation.
- **The layout solver is greedy and single-axis.** A cassowary-class solver is
  the documented richer version.
- **Resize is half-built.** `Terminal::resize` exists and `Event::Resize` is
  produced, but delivering a winch and handling it is the consumer's business —
  the crate provides the mechanism.

## Caveats

**Type-ahead the launch probe read is already in the process.** The size probe
reads one byte at a time and stops at its reply, but what arrived before the
reply is in `pending`; an app that quits among those bytes takes the rest with
it. The window is the probe's few hundred milliseconds at launch.

**This crate is the counter-example to a claim the vault itself recorded.** An
earlier sweep asserted that ~878 `#[test]` functions across six native crates
cannot compile, and named kaua specifically as having "unconditional `#![no_std]`"
and failing "on both counts". kaua carries `#![cfg_attr(not(test), no_std)]` — from
its first commit — and an optional libthyla-rs behind a default-on `backend`
feature, and documents the exact host-test command in its own Cargo.toml.
Ninety-two tests pass.

**And the correction has since been corrected.** The re-census that established
the above put the stranded count at 627 — also wrong, because it recorded nora's
238 as stranded when they run. Measured: **489 run** (kaua 92, parley 73,
libdriver 86, nora 238) and **389 are stranded** (libutopia 385, tapestryd 4).
The two that genuinely cannot host-test fail for a real and different reason —
both depend on libthyla-rs *unconditionally*, so the host build reaches its
aarch64 `_start` assembly — and the pattern that fixes it is in production in
four crates. See [[chg-2026-08-03-nora-engine-sweep]].

## Provenance
(generated -- incoming `touched` backlinks, newest first; never hand-written)
