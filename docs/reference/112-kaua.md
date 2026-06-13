# 112 — Kaua, the native console TUI substrate (`usr/lib/kaua`)

**Status:** as-built through **T-2** (the core + the widget/layout/event-source
layer); Kaua itself is unchanged since. Its first consumer, the **`nora`**
editor, landed at **T-3** and proves the substrate against a real full-screen
app (`docs/reference/113-nora.md`). The `ut` raw-mode dance landed at **T-4**
(@77386f7), and the focused I-27 audit (the Kaua backend + the dance) is
**closed** — Opus-4.8-max, 0 P0 / 0 P1 / 0 P2 / 3 P3: I-27 preserved (the dance
spawns the child with fd_list `[0,1,2]` only, never re-forwarding consctl or
conferring console-attach), the restore-on-every-exit backstop is complete on
the dance path, and the VT input parser is memory-safe (O(1) state, CSI-overflow
latch, total `feed`); the 3 P3 robustness notes (incl. a `>256`-byte-paste
split-sequence mis-key) are tracked #106. Design scripture: `docs/KAUA.md`
(binding); this is the as-built complement. Audit-trigger row: ARCH §25.4 /
CLAUDE.md ("Kaua console-TUI substrate").

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
| `event` | `KeyCode`/`Mods`/`KeyEvent`/`Event` | always | yes | no |
| `input` | the VT/ANSI `Parser` | always | yes | **yes** (bounds) |
| `encode` | diff → escape bytes | always | yes | no |
| `layout` | `Constraint`/`Layout` splitter | always | yes | no |
| `widget` | `Widget` + Paragraph/Block/List/StatusLine | always | yes | no |
| `source` | `EventSource` + `PollSource` (fd-0 input) | `backend` | no (device) | **yes** (fd discipline) |
| `term` | the fd-1 `Terminal` (screen output) | `backend` | no (device) | **yes** (fd discipline) |

The pure layers (everything but `source`/`term`) are terminal-free values — a
bug there corrupts only the app's own screen, never a privilege/safety boundary
— so they are unit-tested, not audit-gated. `source` + `term` are the
device/capability layers: **input** (fd 0, the poll + parser) lives in `source`,
**output** (fd 1, the diff + alt-screen) in `term`. They are split precisely so
the Loom seam is real — a future `LoomSource` swaps the input half without
touching the output half (`docs/KAUA.md` §4.4).

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
enum Event { Key(KeyEvent), Resize(u16, u16), Tick }   // the loop's message
```

A Ctrl-letter is `Char(lowercase)` + `Mods::CTRL` (so Ctrl-C is `Char('c')` +
CTRL, `is_ctrl('c')`); an Alt-letter is the letter + `Mods::ALT`; Shift on a
printable is baked into the char (`'A'` vs `'a'`), so `SHIFT` is set only for the
non-text keys a terminal encodes it on (modified arrows, BackTab). `Event` is the
event-loop message; `Resize` + `Tick` are v1.0 seams (no winsize signal nor a
timer source yet) kept in the type for the match and the future.

### `kaua::input`

```rust
struct Parser { /* fixed-size state */ }
fn new() -> Parser                                // const
fn feed(&mut self, b: u8) -> Option<KeyEvent>     // one byte; Some on a complete key
fn flush(&mut self) -> Option<KeyEvent>           // end-of-chunk: a dangling ESC -> Esc
```

### `kaua::layout`

```rust
enum Constraint { Length(u16), Min(u16), Pct(u16), Fill }
enum Direction { Vertical, Horizontal }
struct Layout<'a> { /* direction + &[Constraint] + margin */ }
fn vertical(&[Constraint]) -> Layout      // const; also horizontal()
fn margin(self, u16) -> Layout
fn split(&self, area: Rect) -> Vec<Rect>  // one Rect per constraint, along the axis
```

A single-axis greedy solver, not cassowary (`docs/KAUA.md` §3.2): `Length`/`Pct`
resolve to fixed sizes, then the remainder is shared equally among the flex set
(`Min` floors + `Fill`). Sizes clamp cumulatively (no rect spills past the area);
if nothing flexes, trailing space is left unassigned (a fixed layout the caller
chose). `margin` shrinks all four sides first.

### `kaua::widget`

```rust
trait Widget { fn render(self, area: Rect, buf: &mut Buffer); }   // consumes self
struct Span<'a> { text: &'a str, style: Style }                   // raw/styled
struct Paragraph<'a> { /* text */ }   // new/style/wrap(bool)/scroll(u16)
struct Block<'a> { /* frame */ }      // new/title/borders(bool)/border_style/title_style + inner(area)
struct List<'a> { /* &[&str] */ }     // new/select(Option<usize>)/style/selected_style/offset
struct StatusLine<'a> { /* bar */ }   // new/left(Span)/center(Span)/right(Span)/fill(Style)
```

Each widget is a pure value that paints into a `Buffer` over a `Rect` (the
ratatui model). `Paragraph` splits on `\n`, optionally hard-char-wraps at the
area width, and skips `scroll` leading visual lines (bounded by `scroll +
height`, never the whole text). `Block` draws a box-drawing border (only when the
area is ≥ 2×2) + a top-edge title; `inner(area)` is the content rect. `List`
highlights the selected row across its full width and scrolls by `offset`.
`StatusLine` fills one row then places left / right / center segments (center
wins on a narrow line).

### `kaua::source` (feature `backend`)

```rust
trait EventSource {
    fn poll(&mut self, timeout: PollTimeout) -> Result<Vec<Event>>;  // a batch of decoded Events
    fn is_eof(&self) -> bool;                                        // fd 0 reported EOF/HUP
}
struct PollSource { /* fd-0 poll + parser */ }    // new/default; the v1.0 EventSource
```

`PollSource` is the LS-8c poll over fd 0 (the pollable cons, LS-8a) feeding the
VT parser; one `poll` returns every key the available bytes decode to (a single
read can carry many). The console read is **#811 death-interruptible**. The trait
is the **Loom seam** (`docs/KAUA.md` §4.4): a future `LoomSource` (input as a
multishot `LOOM_OP_READ`) implements it with zero change to the output `Terminal`
or any widget/app code.

### `kaua::term` (feature `backend`)

```rust
struct Terminal { /* front/back buffers + fd-1 writer (output only) */ }
fn enter(area: Rect) -> Result<Terminal>          // alt-screen, hide cursor, no-autowrap, clear
fn area(&self) -> Rect
fn back_mut(&mut self) -> &mut Buffer             // the draw target
fn draw<F: FnOnce(&mut Buffer)>(&mut self, f: F) -> Result<()>  // reset back, f draws, flush
fn flush(&mut self) -> Result<()>                 // diff -> one batched escape frame -> fd 1
fn set_cursor(&mut self, pos: Option<(u16,u16)>)  // shown after flush; None = hidden
fn clear(&mut self)                               // force a full repaint next flush
fn resize(&mut self, area: Rect)                  // front+back resize + repaint (winsize seam)
fn leave(&mut self) -> Result<()>                 // restore (also via Drop)
```

`Result` is `libthyla_rs::err::Result`. `Terminal` is **output only** — input
moved to `kaua::source` (the Loom-seam split); pair a `Terminal` with a
`PollSource` in the loop. The console size is fixed at `enter()` (no
winsize-query syscall at v1.0 — a CPR round-trip + a resize note is a seam;
`resize()` is the future signal's entry point).

### The v1.0 event loop

```rust
let mut term = Terminal::enter(area)?;            // alt-screen, hide cursor
let mut src  = PollSource::new();                 // poll fd 0
loop {
    term.draw(|buf| app.render(buf))?;            // widgets paint; diff + flush
    for ev in src.poll(PollTimeout::Block)? {
        if let Event::Key(k) = ev { app.on_key(k); }
    }
    if src.is_eof() || app.quit() { break; }
}                                                 // term.leave() via Drop; ut restore = backstop
```

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
`Utf8` continuation state. `PollSource` (`kaua::source`) reads a chunk from fd 0,
`feed`s each byte, then `flush`es. Recognizes: UTF-8 text → `Char`; C0 controls → `Enter`
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

### The layout solver (`layout`)

`solve()` walks the constraints once: `Length(n)` → `n`, `Pct(p)` → `p·extent/100`,
`Min(n)` → `n` (a floor), `Fill` → `0`; the remainder after the sum is shared
equally across the flex set (`Min` + `Fill`), the first slots taking the `+1`
from any non-divisible remainder. `split()` then walks the resolved sizes,
**clamping each cumulatively to the axis extent** so an over-subscribed set never
spills past the area (the last slots shrink, then go zero-extent). The cross-axis
gets the full inner extent. `margin(m)` applies `Rect::inner(m)` first.

### Widgets (`widget`)

Each is a pure value implementing `Widget::render(self, area, buf)` (consumes
self — the ratatui model). All clip to `area` and to `buffer` bounds (a write
past the edge is a no-op):

- **`Paragraph`** splits on `\n`; per logical line it produces visual sub-lines
  (one, or char-width chunks when `wrap`), skips `scroll` of them, and stops once
  `area.height` rows are filled — so work is bounded by `scroll + height`, never
  the whole text (a windowed `text` is the O(window)-scroll seam).
- **`Block`** draws box-drawing corners/edges (only when `area` is ≥ 2×2) +
  a title inset past the left corner and clipped before the right; `inner(area)`
  returns the framed content rect (`area` shrunk by 1, or `area` when no border).
- **`List`** renders `items[offset..]` row by row; the selected row is filled
  full-width with `selected_style` (the highlight bar) before its text.
- **`StatusLine`** fills the row with `fill`, then places `left` (from x),
  `right` (flush right), and `center` (centered, painted last so it wins a narrow
  overlap); each segment's fg/attr paints over the bar bg unless it overrides bg.

### The event source + the Loom seam (`source`)

`PollSource` owns the input half: a `PollSet` on fd 0 + the `Parser`. `poll()`
blocks up to the timeout, and on readiness reads one chunk, `feed`s every byte
(collecting `Event::Key`s), then `flush`es — so one call returns the whole batch
a read carried (no key dropped). HUP/EOF on fd 0 latches `is_eof()`. Input lives
here, **separate from the output `Terminal`**, so a future `LoomSource` (the
`EventSource` trait's other implementation) swaps the input half — input as a
multishot `LOOM_OP_READ` draining a CQ — without touching the diff→fd1 output
(`docs/KAUA.md` §4.4). That decoupling is the seam; building it needs Loom to
drive local Devs and is deferred until a verified need.

## Data structures

- `Cell` — `{ symbol: char, style: Style }`. `Default` = space + default style.
- `Buffer` — `{ area: Rect, content: Vec<Cell> }`, row-major; `content.len() ==
  area.area()`.
- `Parser` — `{ state, csi: [u8; 24], csi_len, csi_overflow, utf8: [u8; 4],
  utf8_have, utf8_need }`. **Fixed-size** — the audit invariant (below).
- `Terminal` (output) — front/back `Buffer`, a `Stdout`, a reused `scratch:
  Vec<u8>`, `cursor`, `repaint`, `entered`.
- `PollSource` (input) — a `PollSet` (fd 0 / POLLIN), a `Stdin`, a `Parser`, a
  reused `inbuf: [u8; 1024]`, `eof`.
- `Layout<'a>` — `{ direction, &[Constraint], margin }`. Widgets are small
  borrowed-data values (`Paragraph`/`Block`/`List`/`StatusLine`/`Span`).

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
- **I-27 (consumed, not introduced).** The backend acquires the **screen**
  (`term`, fd 1) and **reads input** (`source`, fd 0), never the **line
  discipline**: neither opens/holds `/dev/consctl`, neither is ever
  console-attached. Raw termios is set by `ut` (T-4) before the app spawns;
  `PollSource` reads fd 0 assuming raw. The backend only ever touches fd 0/1, so
  it works identically for a trusted or untrusted caller — keeping the API honest
  for the future SAK-overlay consumer (`docs/KAUA.md` §7).
- **Crash backstop.** `Terminal::Drop` restores the screen on a clean return;
  `no_std` `panic = abort` means Drop does NOT run on a panic/kill, so `ut`'s
  post-reap restore (T-4) is the authoritative backstop. Both restores are
  idempotent.
- **No new ARCH §28 invariant** — Kaua consumes I-27, I-9 (LS-8c pollable cons),
  and the LS-8b consctl mechanism (`docs/KAUA.md` §10).

## Tests (`cargo test -p kaua --no-default-features --target <host>`)

63 host unit tests over the pure layers:

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
- `layout` (8) — editor body + status line, Pct→Fill, two-Fill remainder split,
  fixed-only trailing space, over-subscribed clamp, margin, empty, Min floor
  grows.
- `widget` (12) — Block border+title+inner / no-border / too-small-skips,
  Paragraph clip / hard-wrap / scroll / blank lines, List selection highlight /
  offset scroll, StatusLine three segments / fill bg, Span constructors.

The `source` + `term` backends have no host test (they need the device); they are
thin glue over the host-tested pure layers and are validated end-to-end by the
`ls-7` LS-CI (T-4) + the focused audit.

## Error paths

- `term` methods return `libthyla_rs::err::Result`; `enter`/`flush`/`leave`
  propagate an fd-1 write error, `read_keys` an fd-0 read/poll error. EOF/HUP on
  fd 0 latches `eof()` (the loop's quit signal) rather than erroring.
- The pure layers do not error: out-of-bounds `Buffer` access is a `None`/no-op,
  `Parser::feed` is total, the encoder is infallible (it appends to a `Vec`).

## Status

| Item | State |
|---|---|
| T-1 core | landed — `style`/`rect`/`buffer`/`event`/`input`/`encode` + `term` |
| T-2 widgets + layout + event-source | landed — `layout`/`widget`/`source` + `Event`; input split out of `term` into `source` |
| T-3 `nora` (first consumer; `113-nora.md`) | landed — proves the substrate; no change to Kaua itself |
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
- **The `EventSource` seam point exists** (`source.rs`); the `LoomSource`
  implementation behind it does not — it needs Loom to drive local Devs and is
  deferred (`docs/KAUA.md` §4.4). `Event::Resize`/`Event::Tick` are likewise
  type-level seams — `PollSource` never produces them at v1.0.
- **Layout/widget seams** — the richer (cassowary) layout solver, word-wrap +
  a windowed/O(window) `Paragraph` scroll, partial `Block` borders, an
  indexed-color fallback, and more widgets (Table/Tabs/Gauge/scrollbar) all
  accrete later on the same traits.
- The SAK-via-Kaua rework remains a seam (`docs/KAUA.md` §7).
