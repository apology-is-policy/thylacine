# 112 — Kaua, the native console TUI substrate (`usr/lib/kaua`)

**Status:** as-built through **T-1** (the core: the cell model + the cons
backend). The widget/layout/event-source layer is **T-2**; the `nora` editor is
**T-3**; the `ut` raw-mode dance + the `ls-7` LS-CI is **T-4**; the focused
I-27 audit is the arc's close. Design scripture: `docs/KAUA.md` (binding); this
is the as-built complement. Audit-trigger row: ARCH §25.4 / CLAUDE.md ("Kaua
console-TUI substrate").

## Purpose

`kaua` is a native (`no_std` + `alloc`, libthyla-rs) immediate-mode text-UI
library: an app redraws a whole cell `Buffer` each frame and the backend diffs
it against the on-screen buffer and emits only the changed cells (the ratatui
model, brought native over Thylacine's device model rather than libc/termios).
It is the console presentation layer for v1.0 — the "text weave" of the Loom
family (`docs/KAUA.md` §1.1: Loom → Tapestry graphics weave → Kaua text weave;
`Weft` reserved). First consumer: `usr/nora` (the editor, T-3).

The crate is `#![cfg_attr(not(test), no_std)]`: `no_std` for the
`aarch64-unknown-none` device target, but `std` under `cargo test`, so the pure
layers run on the host. The cons backend (`kaua::term`) is the only
libthyla-rs-coupled + audit-bearing layer; it sits behind the **default**
`backend` feature so host tests of the pure layers drop libthyla-rs (which does
not build on the host).

## Module map

| Module | Layer | Feature | Host-testable | Audit-bearing |
|---|---|---|---|---|
| `style` | `Color`/`Attr`/`Style` | always | yes | no |
| `rect` | `Rect` (geometry) | always | yes | no |
| `buffer` | `Cell`/`Buffer` + the diff | always | yes | no |
| `event` | `KeyCode`/`Mods`/`KeyEvent` | always | yes | no |
| `input` | the VT/ANSI `Parser` | always | yes | **yes** (bounds) |
| `encode` | diff → escape bytes | always | yes | no |
| `term` | the fd-0/1 `Terminal` | `backend` | no (device) | **yes** (fd discipline) |

The pure layers (everything but `term`) are terminal-free values — a bug there
corrupts only the app's own screen, never a privilege/safety boundary — so they
are unit-tested, not audit-gated. `term` is the device/capability layer.

## Public API

### `kaua::style`

```rust
enum Color { Reset, Rgb(u8, u8, u8) }            // truecolor; Reset = terminal default
struct Attr(u16);                                 // BOLD DIM ITALIC UNDERLINE REVERSE bitset
struct Style { fg: Color, bg: Color, attr: Attr } // .new() (const) + .fg()/.bg()/.attr() builders
```

No 16/256-color indirection at v1.0 — truecolor is the target (the *Bonfire*
palette is hex; the host terminal under QEMU supports `ESC[38;2;r;g;bm`). An
indexed fallback is a seam.

### `kaua::rect`

```rust
struct Rect { x: u16, y: u16, width: u16, height: u16 }
// const: new/area/is_empty/right/bottom/contains (half-open)
// inner(margin) -> shrink every side, clamped (an over-large margin yields a
//                  centred zero-extent rect, never inverts)
```

### `kaua::buffer`

```rust
struct Cell { symbol: char, style: Style }        // width-1 v1.0 (unicode-width is a seam)
struct Buffer { area: Rect, content: Vec<Cell> }  // row-major
// empty/filled/index_of/get/get_mut/set_cell/set_str/reset/resize
fn diff<'a>(&'a self, prev: &Buffer) -> Vec<(u16, u16, &'a Cell)>
```

`diff` returns the cells of `self` (the back/under-construction buffer) that
differ from `prev` (the on-screen front), in row-major order — exactly the
backend's emit list. **A size mismatch returns an empty diff** (a resize is a
caller-driven full repaint, not a cell diff).

### `kaua::event`

```rust
enum KeyCode { Char(char), Enter, Esc, Backspace, Tab, BackTab,
               Left, Right, Up, Down, Home, End, PageUp, PageDown,
               Delete, Insert, F(u8) }
struct Mods(u8);                                  // SHIFT ALT CTRL bitset
struct KeyEvent { code: KeyCode, mods: Mods }     // new/with/char/is_ctrl
```

A Ctrl-letter is `Char(lowercase)` + `Mods::CTRL` (so Ctrl-C is `Char('c')` +
CTRL, `is_ctrl('c')`); an Alt-letter is the letter + `Mods::ALT`; Shift on a
printable is baked into the char (`'A'` vs `'a'`), so `SHIFT` is set only for the
non-text keys a terminal encodes it on (modified arrows, BackTab).

### `kaua::input`

```rust
struct Parser { /* fixed-size state */ }
fn new() -> Parser                                // const
fn feed(&mut self, b: u8) -> Option<KeyEvent>     // one byte; Some on a complete key
fn flush(&mut self) -> Option<KeyEvent>           // end-of-chunk: a dangling ESC -> Esc
```

### `kaua::term` (feature `backend`)

```rust
struct Terminal { /* front/back buffers + fd-0 parser + fd-1 writer + poll */ }
fn enter(area: Rect) -> Result<Terminal>          // alt-screen, hide cursor, no-autowrap, clear
fn area(&self) -> Rect
fn back_mut(&mut self) -> &mut Buffer             // the draw target
fn draw<F: FnOnce(&mut Buffer)>(&mut self, f: F) -> Result<()>  // reset back, f draws, flush
fn flush(&mut self) -> Result<()>                 // diff -> one batched escape frame -> fd 1
fn set_cursor(&mut self, pos: Option<(u16,u16)>)  // shown after flush; None = hidden
fn clear(&mut self)                               // force a full repaint next flush
fn resize(&mut self, area: Rect)                  // front+back resize + repaint (winsize seam)
fn read_keys(&mut self, timeout: PollTimeout) -> Result<Vec<KeyEvent>>  // poll fd 0 + parse
fn eof(&self) -> bool                             // fd 0 reported EOF/HUP
fn leave(&mut self) -> Result<()>                 // restore (also via Drop)
```

`Result` is `libthyla_rs::err::Result`. The console size is fixed at `enter()`
(no winsize-query syscall at v1.0 — a CPR round-trip + a resize note is a seam;
`resize()` is the future signal's entry point).

## Implementation

### The diff renderer (`buffer` + `encode` + `term::flush`)

`Terminal` holds two `Buffer`s: `front` (on-screen) and `back` (the frame under
construction). `draw` resets `back`, the app's closure redraws everything, and
`flush`:

1. Collects the updates — `back.diff(&front)` (the changed cells), or **every**
   back cell when `repaint` is set (after `clear`/`resize`) — as owned
   `(x, y, Cell)` (so no borrow of `back`/`front` is held while writing the
   output buffer; the clone is a `char` + a small `Style`).
2. `encode::render_cells` walks the updates tracking a virtual **pen**: a cursor
   move (`ESC[<y+1>;<x+1>H`) is emitted only when the next cell is not where the
   pen already sits (a contiguous run on one row → one move), and an SGR only
   when the style changed. Each SGR fully establishes the style — reset (`0`),
   then attrs, then fg, then bg — so no prior state leaks. The glyph is appended
   UTF-8.
3. The real cursor is positioned (`set_cursor` → move + show, else hide).
4. The whole buffer is written to fd 1 in **one** `write_all` (flicker-free), then
   `front <- back.clone()`.

Autowrap is disabled at `enter()` (`ESC[?7l`), so writing the right-edge cell
never wraps and desyncs the pen; the next row's cell mismatches the pen and
re-moves explicitly, so correctness never depends on wrap behavior.

### The VT/ANSI input parser (`input`)

A byte-at-a-time state machine: `Ground → Esc → {Csi, Ss3} → Ground`, plus a
`Utf8` continuation state. The Terminal reads a chunk from fd 0, `feed`s each
byte, then `flush`es. Recognizes: UTF-8 text → `Char`; C0 controls → `Enter`
(CR/LF), `Backspace` (DEL/BS), `Tab`, `Ctrl+letter`; `ESC[` CSI (arrows, Home/End,
`~`-navigation + function keys, `;mod` modifier params, BackTab `Z`); `ESC O`
SS3 (arrows, Home/End, F1-F4); `ESC`+printable → Alt+char; a dangling lone `ESC`
(resolved at `flush`) → `Esc`.

**The local-console assumption** (`docs/KAUA.md` §3.5): the kernel console
(LS-8a) delivers each logical input — a keypress or a pasted sequence — within
one ring drain, so a complete escape arrives inside one `feed` chunk and a lone
Escape arrives as a single `0x1b` with nothing after it. That is why `flush`
resolves a pending `ESC` to `Esc`. A sequence split across two reads (not
produced by a local console) would mis-resolve its leading ESC — a documented
benign edge, never a crash.

## Data structures

- `Cell` — `{ symbol: char, style: Style }`. `Default` = space + default style.
- `Buffer` — `{ area: Rect, content: Vec<Cell> }`, row-major; `content.len() ==
  area.area()`.
- `Parser` — `{ state, csi: [u8; 24], csi_len, csi_overflow, utf8: [u8; 4],
  utf8_have, utf8_need }`. **Fixed-size** — the audit invariant (below).
- `Terminal` — front/back `Buffer`, a `Parser`, `Stdin`/`Stdout`, a `PollSet`
  (fd 0 / POLLIN), a reused `scratch: Vec<u8>` + `inbuf: [u8; 1024]`, `cursor`,
  `repaint`, `eof`, `entered`.

## State machines

The `input::Parser` (file header is authoritative):

| State | On byte | → |
|---|---|---|
| `Ground` | `0x1b` | `Esc` |
| `Ground` | `< 0x80` | a key (or none for NUL/low controls), stay `Ground` |
| `Ground` | UTF-8 lead | `Utf8` (need 2-4) |
| `Esc` | `[` / `O` | `Csi` / `Ss3` |
| `Esc` | printable | Alt+char, `Ground` |
| `Esc` | `0x1b` | emit `Esc`, stay `Esc` |
| `Esc` | C0 control | emit `Esc`, drop the byte, `Ground` |
| `Csi` | `0x20..=0x3f` | append param (or latch `csi_overflow`), stay `Csi` |
| `Csi` | `0x40..=0x7e` | dispatch (none if overflowed), `Ground` |
| `Csi` | `0x1b` | abort, `Esc` |
| `Csi` | other control | abort, drop, `Ground` |
| `Ss3` | final | a key (or none), `Ground` |
| `Utf8` | `0x80..=0xbf` | append; on complete → `Char`, `Ground` |
| `Utf8` | other | abort, re-dispatch the byte in `Ground` |

## Invariants

- **AUDIT — `input::Parser` is O(1) state.** No input, however long, malformed,
  or adversarial, grows its memory or makes it loop: a CSI parameter flood
  overflows the fixed `csi` buffer (`csi_overflow` latches) and the sequence is
  consumed to its final byte and yields no event; a UTF-8 run is bounded at 4
  bytes. `feed` is total and never panics. Test:
  `input::csi_param_flood_is_bounded_and_yields_no_event` (10 000 param bytes →
  no event, parser back in `Ground`).
- **I-27 (consumed, not introduced).** `term` acquires the **screen** on fd 1,
  never the **line discipline**: it never opens/holds `/dev/consctl`, is never
  console-attached. Raw termios is set by `ut` (T-4) before the app spawns; the
  Terminal reads fd 0 assuming raw. It only ever touches fd 0/1, so it works
  identically for a trusted or untrusted caller — keeping the API honest for the
  future SAK-overlay consumer (`docs/KAUA.md` §7).
- **Crash backstop.** `Terminal::Drop` restores the screen on a clean return;
  `no_std` `panic = abort` means Drop does NOT run on a panic/kill, so `ut`'s
  post-reap restore (T-4) is the authoritative backstop. Both restores are
  idempotent.
- **No new ARCH §28 invariant** — Kaua consumes I-27, I-9 (LS-8c pollable cons),
  and the LS-8b consctl mechanism (`docs/KAUA.md` §10).

## Tests (`cargo test -p kaua --no-default-features --target <host>`)

43 host unit tests over the pure layers:

- `style` (3) — the `Attr` bitset, `Style` builders, the `Reset/Reset/None`
  default.
- `rect` (3) — area/edges, half-open `contains`, `inner` shrink+clamp.
- `buffer` (7) — `set_str` + clip, out-of-bounds → `None`, `diff` reports only
  changed cells / empty when identical / empty across a resize, `resize`
  clears.
- `event` (2) — the `Mods` bitset, the key constructors + `is_ctrl`.
- `input` (18) — the parser truth table: ASCII, C0 named keys, Ctrl-letters,
  lone-Esc-at-flush, ESC ESC, Alt-char, CSI arrows, SS3 arrows + F-keys,
  `~`-navigation, CSI modifiers, BackTab, CPR-consumed-not-a-key, unknown-final
  consumed, UTF-8 multibyte, UTF-8-then-ASCII, invalid-continuation recovery,
  stray continuation dropped, **the CSI param flood bound**, ESC-mid-CSI fresh,
  a mixed typed line.
- `encode` (10) — decimal, 1-based `move_to`, truecolor + attr SGR, default-style
  SGR, UTF-8 glyph, contiguous run = one move, non-contiguous re-moves,
  style-change re-emits SGR.

The `term` backend has no host test (it needs the device); it is thin glue over
the host-tested pure layers and is validated end-to-end by the `ls-7` LS-CI
(T-4) + the focused audit. A hello-world test harness (a styled bordered box,
flicker-free) is the T-1 driver until the dance lands.

## Error paths

- `term` methods return `libthyla_rs::err::Result`; `enter`/`flush`/`leave`
  propagate an fd-1 write error, `read_keys` an fd-0 read/poll error. EOF/HUP on
  fd 0 latches `eof()` (the loop's quit signal) rather than erroring.
- The pure layers do not error: out-of-bounds `Buffer` access is a `None`/no-op,
  `Parser::feed` is total, the encoder is infallible (it appends to a `Vec`).

## Status

| Item | State |
|---|---|
| T-1 core (this) | landed — `style`/`rect`/`buffer`/`event`/`input`/`encode`/`term` |
| T-2 widgets + layout + event-source | not started |
| T-3 `nora` | not started |
| T-4 ut dance + `ls-7` LS-CI | not started |
| audit | not started |

## Known caveats / seams

- **No winsize query** — size is fixed at `enter()`; `resize()` exists for a
  future signal. A CPR (`ESC[6n` → `ESC[<r>;<c>R`, which the parser already
  consumes safely) round-trip is the seam.
- **Width-1 cells** — wide CJK / combining clusters are a documented seam (the
  full `unicode-width` table); v1.0 assumes one column per `char`.
- **Default `backend` feature** — host tests MUST pass `--no-default-features`
  (libthyla-rs is aarch64-thylacine-only). The device build keeps it on.
- The Loom event source (`docs/KAUA.md` §4.4), the richer layout solver, an
  indexed-color fallback, and the SAK-via-Kaua rework are all seams.
