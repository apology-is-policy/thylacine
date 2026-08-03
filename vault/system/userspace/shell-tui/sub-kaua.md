---
id: sub-kaua
type: sub
title: "kaua — the console TUI substrate: a cell diff, a VT parser, and the crate whose tests actually run"
parent: moc-userspace-shell-tui
code:
  - usr/lib/kaua/src/lib.rs
  - usr/lib/kaua/src/term.rs
  - usr/lib/kaua/src/source.rs
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
updated: 2026-08-03
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

**The input cycle.** One `poll` returns every event decodable from the bytes
available this round. It drains fd 0 repeatedly into a *single retained parser*
before deciding anything, because a paste larger than the console ring arrives
across several reads and flushing between them would mis-key a sequence
straddling a read boundary. Three bounds govern the loop: `DRAIN_MAX` caps the
sweeps per round against an unbounded writer — and on hitting the cap it returns
*without* flushing, so a half-assembled sequence survives to the next round; the
first sweep blocks for the caller's timeout only if no event is already in hand;
and a sweep that finds the parser holding a bare ESC waits `ESC_HOLDOFF_MS`
instead of declaring the drain dry, so a split arrow key assembles rather than
resolving to a spurious Escape. A lone ESC becomes an Escape key only once fd 0
is genuinely empty.

**The size handshake.** The console has no winsize syscall, so `terminal_size`
does the standard CPR round-trip: save the cursor, park it at a far corner (the
terminal clamps to bottom-right), request its position, and parse the
`ESC[<rows>;<cols>R` reply — the clamped position *is* the screen size. Two
properties were bought the hard way. It is **bounded by a total deadline**,
re-polling the remaining budget per byte, so a reply dribbled a byte at a time by
a hypervisor's serial path still assembles while a slow peer still cannot
multiply the budget by the buffer capacity. And it is **lossless**: bytes read
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

`PollSource` — a `PollSet`, stdin, the retained `Parser`, a fixed read chunk, an
EOF flag, and the `pending` replay bytes handed over by the launch probe.

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

The crate consumes [[inv-i9]] rather than establishing it: the drain loop's
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
uses a fixed default. An unreachable or malformed `/dev/winsize` returns `None`
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

The input read chunk is 1 KiB against a 256-byte console ring, so a burst is
normally one read. `DRAIN_MAX` bounds a round at 64 reads — 64 KiB, far above any
real paste. The ESC holdoff costs 50 ms once per genuine lone-Escape press, the
standard terminal tradeoff.

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
  buffer capacity.
- **Is type-ahead still lossless?** The probe's leftover bytes must reach the
  steady-state parser through `with_pending`, and the read must still stop at
  `R`.
- **Does the drain still refuse to flush on the cap?** Flushing a partially
  assembled sequence is how a paste becomes mis-keyed input.

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

**`query.rs` describes its own algorithm three times and one of them is the bug
it fixed.** The module header documents the total-deadline probe and explains
exactly why the previous approach was wrong: it "assumed one-drain delivery... and
gave up the instant the ring went empty mid-reply." The inline comment at the loop
says the same. But the `///` doc comments on `terminal_size` and `read_cpr` still
describe the *old* algorithm — "waiting up to `timeout_ms` for the reply to START
(then non-blocking)" and "the first poll on the full deadline and the rest
non-blocking" — which is precisely the behaviour the header identifies as the
defect. `terminal_size`'s version even points the reader at the module header,
which contradicts it.

The mechanism is visible in the fix commit: it rewrote the `//` header and added
the `//` inline comment, and left both `///` docs untouched. The author updated
the prose they were reading while working, not the prose the reader receives —
rustdoc renders the doc comments, not the header. See [[chg-2026-08-03-kaua-sweep]].

**This crate is the counter-example to a claim the vault itself recorded.** An
earlier sweep asserted that ~878 `#[test]` functions across six native crates
cannot compile, and named kaua specifically as having "unconditional `#![no_std]`"
and failing "on both counts". kaua carries `#![cfg_attr(not(test), no_std)]` — from
its first commit — and an optional libthyla-rs behind a default-on `backend`
feature, and documents the exact host-test command in its own Cargo.toml.
Ninety-two tests pass. So do parley's 73 and libdriver's 86. The stranded count is
627, not 878, and the pattern is in production in four crates rather than proven
in one.

## Provenance
(generated -- incoming `touched` backlinks, newest first; never hand-written)
