# 92-utopia-line-editor — `libutopia::line_editor` engine core (U-4a)

Per CLAUDE.md "Reference documentation discipline (load-bearing)" — the
technical reference for the line editor engine landed at Phase 7 U-4a.
Binding designs: `docs/UTOPIA-SHELL-DESIGN.md` section 11.2 (the line
editor) + section 19 (the U-* arc).

## Purpose

U-4a is the **first sub-chunk of the U-4 line editor arc**, the second
chunk in the shell half of Phase 7 (the first was U-3, the workspace
skeleton). The U-4 arc as a whole is decomposed into 4 sub-chunks:

| Sub-chunk | Scope |
|---|---|
| **U-4a** | Engine core: `LineEditor` + `EditorAction` + ANSI input parser + emacs editing primitives + UTF-8 multi-byte accumulation + single-line render. |
| U-4b | Multi-line + bracket-balance continuation. |
| U-4c | History buffer + Ctrl-R incremental search. |
| U-4d | Tab completion via pluggable `CompletionSource` trait. |

The strategic framing of the U-4 arc (per `memory/project_next_session.md`):
the line editor is a **pure-logic engine** that consumes byte streams +
produces editor actions. Raw-mode I/O (termios via `/dev/consctl`) is
deferred to **U-6** (the main loop wiring) and **U-PTY** (the substrate).
At v1.0 the kernel has no PTY surface, so a pure-logic engine is the
only thing that can land before U-PTY without blocking on the kernel.

U-4a delivers the engine; U-6 + U-PTY will wire it to actual stdin /
stdout. Until then U-4a is validated as pure-Rust logic by:

- A `flow_line_editor` probe in `/u-test` exercising six end-to-end
  patterns at boot (the runtime regression test).
- `#[cfg(test)] mod tests` covering every binding + the ANSI parser +
  UTF-8 paths (the documentation-grade unit tests; `cargo test` on a
  host target would exercise them).

## Public API

### `libutopia::line_editor::EditorAction`

```rust
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum EditorAction {
    /// Nothing externally visible changed (partial multi-byte sequence
    /// or a no-op key like Tab at U-4a).
    NoChange,
    /// Buffer / cursor changed; the main loop should call render().
    Redraw,
    /// Enter pressed; the engine has reset its internal buffer and
    /// hands back the submitted line.
    Accept(String),
    /// Ctrl-C; current edit discarded; the engine's buffer is empty.
    Cancel,
    /// Ctrl-D on empty buffer; the main loop should exit.
    Eof,
    /// Ctrl-L; the main loop should clear the screen and redraw.
    /// The engine's state is unchanged.
    ClearScreen,
}
```

### `libutopia::line_editor::LineEditor`

```rust
pub struct LineEditor { /* private */ }

impl LineEditor {
    pub fn new() -> Self;
    pub fn reset(&mut self);                          // Clear edit; keep kill_buffer.
    pub fn buffer(&self) -> &str;
    pub fn cursor(&self) -> usize;                    // Byte index into buffer.
    pub fn kill_buffer(&self) -> &str;
    pub fn history(&self) -> &[String];
    pub fn push_history(&mut self, line: String);

    /// Process one input byte; returns the dispatchable EditorAction.
    pub fn feed_byte(&mut self, byte: u8) -> EditorAction;

    /// Process a slice; returns one EditorAction per byte (preserving
    /// order). Useful for tests + for batched stdin reads.
    pub fn feed_bytes(&mut self, bytes: &[u8]) -> Vec<EditorAction>;

    /// Compose an ANSI byte sequence rendering the prompt + current
    /// buffer + cursor positioning. Single-line at U-4a; multi-line
    /// at U-4b. The main loop emits this after any Redraw.
    pub fn render(&self, prompt: &str) -> String;
}
```

### `libutopia::ansi::visible_width`

```rust
/// Count the visible-column width of `s`, treating ANSI CSI escapes
/// as zero-width. Used by the line editor's render() for cursor
/// positioning and by future prompt-emit helpers for prompt-width
/// accounting.
///
/// v1.0 approximation: one column per UTF-8 char. Grapheme-cluster
/// + wcwidth (for double-width CJK and emoji) is v1.x.
pub fn visible_width(s: &str) -> usize;
```

## Implementation

### Keybindings recognised at U-4a

Emacs defaults per `UTOPIA-SHELL-DESIGN.md` section 11.2:

| Byte / sequence | Action |
|---|---|
| `Ctrl-A` (0x01) | Cursor to start of line |
| `Ctrl-B` (0x02) / `CSI D` | Cursor left one char |
| `Ctrl-C` (0x03) | Cancel current edit |
| `Ctrl-D` (0x04) | Eof if buffer empty; delete-char otherwise |
| `Ctrl-E` (0x05) | Cursor to end of line |
| `Ctrl-F` (0x06) / `CSI C` | Cursor right one char |
| `Ctrl-H` (0x08) / `0x7f` | Backspace (delete char before cursor) |
| `Ctrl-I` (0x09) | Tab (deferred to U-4d completion hook; no-op here) |
| `Ctrl-J` (0x0a) / `Ctrl-M` (0x0d) | Enter (Accept) |
| `Ctrl-K` (0x0b) | Kill to end of line (to kill_buffer) |
| `Ctrl-L` (0x0c) | ClearScreen action |
| `Ctrl-N` (0x0e) / `CSI B` | History next |
| `Ctrl-P` (0x10) / `CSI A` | History prev |
| `Ctrl-R` (0x12) | Deferred to U-4c incremental search; no-op |
| `Ctrl-U` (0x15) | Kill to start of line (to kill_buffer) |
| `Ctrl-W` (0x17) | Kill previous word (to kill_buffer) |
| `Ctrl-Y` (0x19) | Yank kill_buffer at cursor |
| `CSI H` / `CSI 1~` / `CSI 7~` | Home |
| `CSI F` / `CSI 4~` / `CSI 8~` | End |
| `CSI 3~` | Delete char at cursor |
| Printable 0x20..=0x7e | Insert as char |
| UTF-8 multi-byte (0xc2..0xf4 + continuations) | Accumulate + insert as char |

History prev / next are recognised at U-4a but produce `EditorAction::NoChange`
when `history.is_empty()`. The U-4c chunk does NOT need to touch the
parser; it just calls `push_history()` after each Accept.

### ANSI input parser state machine

The parser handles three multi-byte cases:

```
State::Ground:
  0x1b -> Escape (CSI sequence prefix)
  0xc2..=0xf4 -> Utf8 (begin multi-byte char)
  control / printable -> Action via the keybinding table

State::Escape:
  '[' -> Csi (start CSI body)
  0x1b -> stay in Escape (ESC ESC restarts)
  other -> Ground (sequence aborted; v1.x adds Alt-key bindings here)

State::Csi { params, param_count, current_has_digits }:
  digit -> accumulate into current param
  ';' -> next param slot
  'A'/'B'/'C'/'D'/'H'/'F' -> apply, Ground
  '~' -> tilde-terminated (1/7 Home; 3 Delete; 4/8 End), Ground
  other -> Ground (unknown CSI; v1.x can add SGR / DSR / etc.)

State::Utf8 { buf, expected, have }:
  0x80..=0xbf -> accumulate; if complete, decode + InsertChar
  other -> Ground (sequence broken; re-process the offending byte)
```

State persists across `feed_byte` calls, so partial sequences split
across stdin reads are handled correctly. The `/u-test` probe verifies
this end-to-end by feeding `\x1b`, then `[`, then `D` as three
separate calls and asserting the cursor moved left after the third.

### UTF-8 buffer invariant

`LineEditor.buffer` is `String`; Rust's invariant guarantees it is
always valid UTF-8. The parser ensures this by:

1. Accumulating multi-byte sequences in a separate `Utf8` state
   (`[u8; 4]`) until complete.
2. Validating the accumulated bytes via `core::str::from_utf8` before
   calling `String::insert`.
3. Dropping the partial sequence + re-processing the offending byte
   if a continuation byte fails to arrive (invalid UTF-8 input is
   tolerated).

`buffer().is_char_boundary(i)` is used by `prev_char_boundary` /
`next_char_boundary` to ensure cursor motion and Backspace / Delete
walk char boundaries, not byte boundaries. A "héllo" buffer where the
cursor is at byte 6 (after 'o') deletes 'o' on Backspace and lands at
byte 5; another Backspace deletes 'l' to byte 4; ... another deletes
the 2-byte 'é' to byte 1.

### Cursor positioning in render()

```
\r            -- cursor to column 0
\x1b[K        -- erase to end of line
prompt        -- prompt bytes (may contain CSI escapes)
buffer        -- buffer bytes (always valid UTF-8)
\r            -- cursor back to column 0
\x1b[<n>C     -- cursor right n columns,
                 where n = visible_width(prompt) + visible_width(buffer[..cursor])
```

If `n == 0` the trailing `\x1b[<0>C` is omitted (`\x1b[0C` would still
work but emitting nothing is cleaner). `visible_width` is the new
helper in `libutopia::ansi`; it walks the bytes, treats CSI sequences
(ESC [ ... final-byte) as zero-width, and counts non-CSI characters
one column each (the v1.0 approximation; v1.x will add grapheme
clusters + wcwidth for proper CJK / emoji width).

### Per-buffer cap

`MAX_BUFFER_LEN = 64 * 1024` -- a defensive cap so a runaway paste
cannot grow the line indefinitely. Insert and Yank refuse to grow
past this cap (returning `EditorAction::NoChange`). 64 KiB is well
above any sensible shell line; the cap is forward-compat for paste
hardening (v1.x bracketed-paste mode can lift it).

## Data structures

### `LineEditor`

```rust
pub struct LineEditor {
    buffer: String,                  // valid UTF-8; the current edit
    cursor: usize,                   // byte index; always on char boundary
    kill_buffer: String,             // most-recent kill for yank
    history: Vec<String>,            // in-memory; empty at U-4a
    history_pos: Option<usize>,      // None = editing current; Some(i) = viewing history[i]
    saved_current: String,           // saved buffer while navigating history
    parser: ParserState,             // ANSI input state machine
}
```

### `ParserState` (private)

```rust
enum ParserState {
    Ground,
    Escape,
    Csi { params: [u32; 4], param_count: u8, current_has_digits: bool },
    Utf8 { buf: [u8; 4], expected: u8, have: u8 },
}
```

### `Action` (private)

The intermediate enum the parser decodes byte streams into. Maps
1:1 onto `LineEditor::do_*` methods; not part of the public surface.

```rust
enum Action {
    InsertChar(char),
    Backspace, DeleteChar,
    CursorLeft, CursorRight, CursorHome, CursorEnd,
    KillToEnd, KillToStart, KillPrevWord,
    Yank,
    HistoryPrev, HistoryNext,
    Accept, Cancel,
    EofOrDelete,                     // Ctrl-D dual behaviour
    ClearScreen,
    Ignore,
}
```

## Tests

### Boot-time integration probe (`flow_line_editor` in `/u-test`)

Six composed probes, runtime-validated at every boot:

1. **Insert + Accept**: feed `"hello"`, verify buffer + cursor, feed `\r`, verify `Accept("hello")` + state reset.
2. **ANSI arrow across byte boundaries**: feed `0x1b`, `b'['`, `b'D'` separately; verify NoChange-NoChange-Redraw + cursor moved left.
3. **Backspace + Ctrl-K + Ctrl-Y**: kill to start, verify kill_buffer holds the killed text, yank, verify buffer restored.
4. **UTF-8 across byte boundaries**: feed `"caf"` + `0xc3` + `0xa9`; verify the partial-state-then-complete pattern; Backspace correctly removes the 2-byte 'é'.
5. **History navigation**: push two entries, type a draft, Ctrl-P twice (newest then older), Ctrl-N twice (back through history then restoring the draft).
6. **render() shape**: verify the `\r\x1b[K<prompt><buffer>\r\x1b[<n>C` output for a 1-char buffer with a 2-char prompt.

Each probe prints `u-test: flow_line_editor: <probe>: <what> FAILED`
on failure + bails the binary with exit 1; joey treats that as a
boot failure. On success, the binary prints `u-test: line editor OK`
between `u-test: hardware + cap OK` and `u-test: all OK`.

### Host-only unit tests (`#[cfg(test)] mod tests`)

Cargo test on a host target would exercise:

- ASCII insert + cursor advance + Accept reset.
- Both LF (0x0a) and CR (0x0d) trigger Accept.
- Ctrl-C cancels + Ctrl-D yields Eof on empty / DeleteChar otherwise.
- Backspace at start of line is no-op.
- All four arrow keys via CSI [ A/B/C/D + Home/End via CSI H/F + Delete via CSI 3~.
- Kill-to-end + Kill-to-start + Kill-previous-word + Yank round-trips.
- UTF-8 multi-byte chars insert at correct cursor offset; Backspace walks UTF-8 char boundaries (not byte boundaries).
- Invalid UTF-8 (leading byte then ASCII) drops the partial sequence + processes the ASCII byte.
- ESC ESC restarts the parser.
- Ctrl-L produces `EditorAction::ClearScreen` without touching state.
- History prev / next with non-empty history navigates correctly and restores the saved current edit when walking past the newest.
- History prev with empty history is no-op.
- `render()` produces the expected `\r\x1b[K<prompt><buffer>\r\x1b[<n>C` shape.
- Tab (Ctrl-I) is no-op at U-4a (U-4d adds the completion hook).

The `cfg(test)` blocks are gated so they compile away under
`cargo build --release --target aarch64-unknown-none` (the production
path).

## Error paths

The engine has no fallible operations. Every `feed_byte` call either:

- Returns an `EditorAction` immediately (the common case).
- Updates internal state + returns `EditorAction::NoChange` (parser
  accumulating a multi-byte sequence).

Insert / Yank refuse to grow the buffer past `MAX_BUFFER_LEN` (returning
`NoChange` silently). Backspace / Delete / cursor motion at buffer
boundaries return `NoChange`.

There is no panic path. The buffer's UTF-8 invariant is maintained by
the parser (multi-byte chars are validated via `core::str::from_utf8`
before insertion); `buffer.insert` cannot fail for valid input.

## Performance characteristics

`feed_byte` is O(buffer.len()) in the worst case (when an insert moves
content right via `String::insert`). For typical interactive editing
the buffer is small (~50-100 chars), so each keystroke is sub-microsecond.

`render()` allocates a String at least as large as `prompt + buffer +
~10 bytes overhead`. For a typical prompt + line this is ~50-200 bytes
per redraw; well within `ThylaAlloc`'s 4 MiB heap.

`visible_width` is O(s.len()). The byte-walk visits every byte once;
CSI bodies are skipped in a tight inner loop.

The engine's heap footprint is `O(buffer.len() + kill_buffer.len() +
history total bytes + saved_current.len())` -- bounded by the actual
data, no fixed-size overhead beyond the `[u8; 4]` UTF-8 buffer + the
small `[u32; 4]` CSI parameter array.

## Status

- **U-4a LANDED** at this chunk.
- `libutopia::line_editor` -- ~640 LOC engine + ~290 LOC `cfg(test)` unit tests.
- `libutopia::ansi::visible_width` -- new helper (~60 LOC including tests).
- `u-test::flow_line_editor` -- ~150 LOC boot-time integration probe.
- New `usr/u-test/Cargo.toml` dependency on `libutopia` (the first
  non-libthyla-rs Rust workspace dependency consumed by a boot-test
  binary).
- Boot log gains `u-test: line editor OK` between `hardware + cap OK`
  and `all OK`.

## Known caveats / footguns

- **No raw-mode I/O at U-4a**: the engine consumes a byte stream + emits
  ANSI bytes for redraw, but it does NOT touch any tty / termios state.
  Hooking it up to actual interactive input is U-6 (main loop) + U-PTY
  (kernel substrate). At U-4a + U-3 the only way to "use" `ut` is the
  boot-time skeleton (banner + exit); the line editor isn't reachable
  from `ut` itself yet.
- **`visible_width` only handles CSI escapes**: OSC (`\x1b]`), DCS
  (`\x1b P`), and other ANSI escape sequences are NOT recognized at
  v1.0. Disciplined Utopia programs emit only CSI 24-bit-color SGR +
  reset, which `visible_width` handles. A future v1.x can extend to
  the full ECMA-48 escape grammar if it earns the complexity.
- **One-column-per-char v1.0 approximation**: emoji + double-width CJK +
  combining marks all count as 1 column in `visible_width`. Their
  visual width on a terminal is typically 2 (emoji, CJK) or 0
  (combining marks). v1.x can add grapheme + wcwidth handling.
- **Bracketed-paste is not recognized**: a paste containing
  `\x1b[200~...\x1b[201~` markers (which most modern terminals send
  when bracketed-paste mode is enabled) would have the markers
  treated as unknown CSI sequences (dropped) but the paste content
  would land as individual chars + ANSI escapes if present.
- **Modifier keys absent**: Ctrl-arrow, Alt-x, Shift-Tab are not bound.
  v1.x can add `\x1b[1;5D` (Ctrl-Left) and friends.
- **History up/down at the same line position doesn't preserve cursor**:
  navigating away from the current edit jumps cursor to end-of-history-line;
  navigating back restores cursor to end-of-saved-current. Reasonable v1.0
  behaviour; v1.x can preserve cursor position across nav.
- **`Action` enum is private**: clients of `LineEditor` only see
  `EditorAction`. The mapping between input bytes and actions is
  internal; v1.x users wanting custom keybindings would extend
  through a public hook (TBD; not at U-4a).

## References

- `docs/UTOPIA-SHELL-DESIGN.md` section 11.2 -- the binding line-editor design.
- `docs/UTOPIA-SHELL-DESIGN.md` section 19 -- the U-* arc.
- `docs/UTOPIA-VISUAL.md` section 3 -- prompt format + continuation glyph (used at U-4b).
- `docs/ROADMAP.md` section 8 -- Phase 7.
- `docs/phase7-status.md` -- per-chunk SHAs.
- `docs/reference/91-utopia.md` -- the U-3 predecessor (libutopia + ut skeleton).
- `usr/utopia/libutopia/src/line_editor.rs` -- the engine source.
- `usr/utopia/libutopia/src/ansi.rs` -- the `visible_width` helper.
- `usr/u-test/src/main.rs` -- the `flow_line_editor` boot probe.
