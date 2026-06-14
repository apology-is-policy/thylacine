# 113 — nora, the native modal editor (`usr/nora`)

**Status:** as-built through **T-4** (the editor + the `ut` raw-mode dance that
launches it, committed @77386f7). `nora` is the first consumer of the Kaua
console-TUI substrate (`docs/reference/112-kaua.md`); it proves the substrate by
being a real full-screen application. The focused I-27 audit (the Kaua backend +
the dance) is **closed** (Opus-4.8-max, 0 P0 / 0 P1 / 0 P2 / 3 P3 robustness
notes, all closed at #106). The live `ls-7` LS-CI run is **owed** — the interactive
harness can't reach a login shell yet (a TCG stratumd-mount regression #104; an
HVF PTY-exit #105), both pre-existing infra unrelated to the editor; the kernel
poll path `nora` takes is proven deterministically by
`poll.cons_deferred_block_then_wake` (b7b14d3). Design scripture: `docs/KAUA.md`
§3/§9 (binding).

**Resolved at #106** (the T-4 audit's 3 P3 robustness notes): the input source
now drains every byte available on fd 0 into one retained parser before resolving
a dangling ESC, so a `>256`-byte paste split across console reads no longer
mis-keys at a read boundary; launching a raw child without a forwarded `consctl`
fd (a non-login `ut`) now keeps the screen crash-restore backstop (it skips only
the raw-mode discipline flips); and the cross-crate restore-sequence mirror is
pinned by a `const` assert (libutopia) + a kaua host test against one shared
literal, so a drift on either side fails its own build.

## Purpose

`nora` is a native (`no_std` + `alloc`, libthyla-rs) modal text editor in the
Helix/vim lineage, built on Kaua. Its modal core is adapted ~1:1 from Stratum's
`tui/src/editor.rs` (the design's POC pointer) onto a native text engine that
replaces `tui-textarea` and standalone file I/O that replaces `std::fs`. It is
the runtime editor for v1.0 (`SHELL`-spawnable as `/bin/nora`); the per-app
console handshake (raw termios via `ut`) lands in T-4.

Per the Plan 9 native-vs-ported split (ARCHITECTURE.md §3.5) `nora` is a native
libthyla-rs binary, like `ut` — no musl, no Pouch.

## Crate shape

`usr/nora` is a **library + binary** in one crate, split exactly as Kaua splits
its pure layers from its `backend`:

| Target | Modules | Feature | Host-testable |
|---|---|---|---|
| `lib` | `text` / `editor` / `view` / `theme` | always (pure) | **yes** |
| `bin` (`src/main.rs`) | the Kaua backend (`Terminal`+`PollSource`) + file I/O | `backend` (default) | no (device) |

The library uses only Kaua's pure layers (`Buffer`/`Layout`/widgets/`event`/
`style`) and never touches a terminal or libthyla-rs, so the whole editor engine
runs on the host. The binary is the only file that uses `kaua::term` /
`kaua::source` / `libthyla-rs::fs`; it is gated behind the `backend` feature
(default-on for the device build) so the lib host-tests with the feature
dropped.

```
Build (device):   cargo build -p nora                       # backend on -> bin
Host-test (lib):  cargo test -p nora --no-default-features --lib \
                      --target <host-triple>                 # bin excluded
```

## Public surface (the library)

### `nora::text` — the text engine (`TextBuffer`)

The `tui-textarea` replacement: a `Vec<String>` of logical lines + a `(row,
col)` cursor in **character** coordinates (never bytes — multi-byte UTF-8 can't
be split). The buffer is never empty (an empty document is one empty line);
`col` is always in `[0, char_len(row)]` (the insert position may sit one past
the last char).

```rust
fn new(content: &str) -> TextBuffer        // split('\n'); round-trips via content()
fn content(&self) -> String                // lines.join("\n")
fn cursor(&self) -> (usize, usize)
fn set_cursor(&mut self, row, col)         // clamps to a valid position
// navigation: move_left/right/up/down, move_home/end/top/bottom,
//             move_word_forward/back  (WORD = whitespace-delimited)
// editing:    insert_char/insert_newline/insert_str, backspace, delete_char,
//             delete_line, range_text(a,b), delete_range(a,b)
fn find(&self, pat: &str) -> Option<Pos>   // forward from cursor, wraps
fn checkpoint(&mut self)                   // push an undo snapshot
fn undo(&mut self) -> bool                 // pop one checkpoint
```

`move_left`/`right` wrap across line boundaries (tui-textarea `Back`/`Forward`
semantics, shared by `h`/`l` and the arrows). `range_text`/`delete_range`
operate on an inclusive `[a, b]` span (normalized internally) — the visual-mode
yank/cut payload, with `'\n'` between lines.

### `nora::editor` — the modal core (`Editor`)

```rust
enum Mode { Normal, Insert, Visual, Command(String), Palette { sel: usize } }
enum Request { Save(Option<String>), Open(String) }     // file op for the binary
struct Editor { /* active buffer: */ text, filename, readonly, modified, top, top_sub, anchor, last_search,
                /* global: */ mode, wrap, register, status, quit, request, bufs: Vec<DocState>, active }

fn new(filename: Option<String>, content: &str, readonly: bool) -> Editor
fn handle_key(&mut self, key: kaua::KeyEvent)           // the dispatch
fn take_request(&mut self) -> Option<Request>           // the binary executes it
fn mark_saved(&mut self, path, bytes) / set_status(msg)
fn scroll_to(&mut self, tw, height)                     // adjust the anchor to the cursor
fn toggle_wrap(&mut self)                               // the [Space] palette wrap entry
fn palette_labels(&self) -> Vec<&str> / palette_sel(&self) -> Option<usize>
fn open_buffer(&mut self, filename, content)            // :e -> a new buffer (T3)
fn next_buffer / prev_buffer / close_buffer(force)      // switch / close (T3)
fn buffer_count(&self) -> usize / buffer_indicator(&self) -> Option<String>
fn selection(&self) -> Option<(Pos, Pos)>               // visual span (for the view)
```

**Multiple buffers (T3):** the ACTIVE buffer lives in the Editor's own fields
(so the per-mode handlers are unchanged by multi-buffer); each backgrounded
buffer is parked as a `DocState` in `bufs` and swapped in/out on switch. `:e`
opens a file in a new buffer (replacing a lone pristine unnamed buffer in place);
`:bn`/`:bp`/`:bd[!]` and the `[Space]` palette switch / close. `register` (yank)
and `wrap` are editor-global (cross-buffer); `modified`/`last_search`/scroll are
per-buffer.

`Mode::Palette { sel }` is the `[Space]` command palette (T2), open over Normal.
Soft-wrap (`wrap`) scrolls in VISUAL rows: the anchor is `(top, top_sub)` (a
logical line + its visual sub-row) and `scroll_to` walks visual rows so a long
wrapped line never pushes the cursor off screen; the math lives in `nora::wrap`
(pure, host-tested). `tw` (text width) is computed once via `view::text_metrics`
and shared by `scroll_to` + `render` so they agree on the geometry.

File I/O is **not** done in the editor: `:w` / `:e` raise a `Request` the binary
executes against `libthyla-rs::fs`, keeping the core terminal-free and testable.
The Stratum POC's system clipboard (pbcopy/pbpaste) becomes an internal yank
register (Thylacine has no system clipboard at v1.0).

### `nora::view` — the renderer

```rust
fn render(ed: &Editor, area: Rect, buf: &mut Buffer) -> (u16, u16)   // returns cursor (x,y)
```

Lays the screen out (a `Layout::vertical([Min(1), Length(1)])` split: text
region above a one-row status/command line), paints the gutter (right-aligned
line numbers), the visible text, the visual selection, and the status bar, then
returns the on-screen cursor the binary hands to `Terminal::set_cursor`. Pure —
reads Kaua's pure layers only.

### `nora::theme` — the Bonfire palette

The Bonfire (U-2) roles as `kaua::style::Style` builders: `bg` `#0e0c0c` / `fg`
`#e4ddd8` / `fg_muted` `#9a8f8a` / `ember` `#e07840` / `surface` `#180f0e` /
`border` `#3a2a26`, plus the mode-chip accents drawn from Bonfire's syntax hues
(`moss`/`dusk`/`sand`) and the `[Space]` palette popup styles. `nora` carries its
own copies rather than depend on the whole shell library. *(#124: pre-T2 these
held the retired U-1 "Pale Fire" cold values — `#0e1018`/`#d8e4f4`/`#8898b4`, the
exact residue UTOPIA-VISUAL.md §8 flags; corrected to Bonfire with T2.)*

## Keybindings (the modal grammar)

| Mode | Keys |
|---|---|
| Normal | `i`/`a`/`o`/`A` → Insert; `v` → Visual; `:` / `/` → Command; `Space` → Palette; `h j k l` + arrows, `0`/`$`/`g`/`G`/`w`/`b`/PageUp/PageDown nav; `x` delete char; `d` delete line; `u` undo; `p` paste register; `n` repeat search |
| Insert | printable → insert; Enter → split; Backspace/Delete; Tab → 4 spaces (soft tab); Esc → Normal |
| Visual | `h j k l 0 $ g G w b` extend the selection; `y` yank; `d`/`x` cut; Esc → cancel |
| Command | type the line; Enter runs it; Backspace (erasing `:`/`/` exits); Esc → Normal |
| Palette (`[Space]`) | `j`/`k` + arrows / `Ctrl-n`/`Ctrl-p` move; Enter runs the entry; `Space`/Esc close. Entries: **toggle soft-wrap**, next/prev/close buffer, save, quit. |

Ex commands: `:w` / `:w <name>` (save / save-as), `:q` (guarded on unsaved),
`:q!`, `:wq` / `:wq <name>`, `:e <name>` (opens a new buffer), `:e! <name>`,
`:bn` / `:bp` (next / prev buffer), `:bd` / `:bd!` (close buffer, guarded /
forced). `/<pat>` Enter searches forward (wrapping); `n` repeats. `nora -R
<file>` opens read-only (a viewer): edits are inert and `q`/Esc quit.

## Implementation notes

- **The mode FSM** lives in `editor.rs`: `handle_key` clears the transient
  status then dispatches to one of four per-mode handlers, each a `match` on the
  `KeyCode`. The structure ports the Stratum POC's `handle_normal/insert/visual/
  command` 1:1; only the cursor/edit calls retarget from `tui-textarea` to
  `TextBuffer`, and the key type from crossterm to `kaua::KeyEvent`.
- **Undo granularity** is one user action, not one keystroke: the editor calls
  `text.checkpoint()` at the start of an edit session (entering Insert; before a
  Normal-mode `x`/`d`/`p`), so a single `u` reverts one logical edit. The undo
  stack is capped (`UNDO_CAP = 64` snapshots) so a long session can't grow it
  unbounded (the #65 resource-floor spirit, in userspace).
- **The request seam** keeps file I/O out of the pure core. The binary's loop is
  `handle_key` → `take_request` → execute (`File::open`/`create` +
  `slurp_capped`/`write_all` + `t_fsync`) → report via `mark_saved`/`load`/
  `set_status`. A `:w` is **durable** (write + fsync); a server that rejects
  fsync still has the bytes written (best-effort barrier).
- **The render loop** (`main::redraw`): `scroll_to(text_h)` → reset the back
  buffer → `view::render` (returns the cursor) → `set_cursor` → one `flush`
  diff frame to fd 1. Human-speed input, so the immediate-mode redraw-everything
  cost is irrelevant; the diff keeps the wire small.

## Console discipline (I-27)

`nora` acquires the **screen** on fd 1 (Kaua `Terminal`) and **reads input** on
fd 0 (Kaua `PollSource`); it never touches the line discipline (consctl) and is
never console-attached → I-27 untouched. Raw termios is `ut`'s job, set via its
private consctl fd before `nora` is spawned (the T-4 dance); `nora` reads fd 0
assuming bytes already arrive raw. On a clean exit `Terminal::Drop` restores the
screen; `no_std` `panic = abort` means `Drop` does NOT run on a crash, so
`ut`'s post-reap restore is the authoritative backstop. This is why `nora` is
**not** an audit-bearing surface on its own — the audited capability boundary is
Kaua's `term`/`source` + the `ut` dance (T-4); `nora` is an ordinary fd-0/1
client over them.

## Data structures

- `TextBuffer { lines: Vec<String>, row, col, undo: Vec<Snapshot> }` — `lines`
  never empty; `(row, col)` char-addressed. `Snapshot { lines, row, col }` is a
  full clone (bounded by `UNDO_CAP`).
- `Editor` — the `TextBuffer` + the `Mode` + the file/scroll/status/quit state +
  the private `anchor` (visual), `register` (yank), `last_search`, `request`.

## Tests (`cargo test -p nora --no-default-features --lib --target <host>`)

**41 host unit tests** over the pure engine:

- `text` (19): content round-trip (incl. trailing newline), char-indexed insert
  for UTF-8, newline split, backspace/delete across lines, `dd` keeps one line,
  motion clamp/wrap, word forward/back (incl. cross-line), range text + delete
  (single + multi line + reversed endpoints), find + wrap, undo + cap, cursor
  clamp.
- `editor` (24): insert/escape, Enter split, `o` open-below, hjkl nav, `x`+undo,
  `dd`, visual yank/paste + cut, `:w`/`:w name`/`:wq`/`:e` raise the right
  `Request`, `:q` guards unsaved, command backspace exits, `/`+`n` search,
  read-only blocks edits, scroll follows cursor, `Space` opens/Esc closes the
  palette, palette Enter toggles wrap, palette nav reaches quit + guards unsaved,
  wrap-scroll keeps the cursor on a long line, open-buffer replaces-pristine /
  adds, buffer edits persist across switch, close-buffer guards + removes, the
  buffer indicator shows only when multiple.
- `view` (8): gutter + text + cursor placement, tilde past end, mode chip + bg,
  command line on the status row, control chars → space, selection paints the
  violet bg, digit widths, soft-wrap splits a long line, the palette overlay
  renders a highlighted menu.
- `wrap` (4): `line_rows` ceil-div (blank = 1), `cursor_visual` clamps a
  full-line end to the last sub-row, forward/backward visual walks, `back_n`
  clamps at the document start.

Coverage NOT here (a terminal / file I/O needed): the `Terminal`/`PollSource`
wiring and the on-disk save/open path — exercised by the `ls-7` LS-CI (T-4:
open / edit / `:w` / `cat`).

## Error paths

- Initial file: an absent path → start empty (a new file; `:w` creates it); a
  path that resolves but fails to read → message to the cooked console + exit 1
  (before the screen is touched). "Absent" is decided by a `fs::exists` (stat)
  precheck, NOT the open errno (#114): the kernel returns a flat `-1` (→ `Io`,
  not `NotFound`) for a missing open today (#102, gated on the #20 errno
  re-vote), so a stat is what distinguishes absent from a real read error.
  Until #102, an existing-but-unstattable path is the interim conflation (opens
  as a new buffer). Non-UTF-8 or `> 2 MiB` → reported, never partially loaded.
- `:w` with no filename → status `no file name (use :w <name>)`. A write error →
  the kernel errno on the status line; the editor stays open (no data lost).
- `:e` of a missing/unreadable file → status with the errno; the current buffer
  is untouched.
- fd 0 lost (no console) → the loop exits cleanly rather than spinning; the
  console read is #811 death-interruptible.

## Status

| Sub-chunk | State |
|---|---|
| T-3 engine + modal core + renderer + binary (this) | landed |
| T-4 `ut` raw-mode dance + `ls-7` LS-CI | not started |
| audit (Kaua backend + dance) | not started |

## Known caveats / seams

- **Console size** is measured at launch via a CPR round-trip
  (`kaua::query::terminal_size`, #117) so nora fills the real terminal; it falls
  back to **80×24** when the terminal does not answer (a dumb terminal / the
  non-interactive harness — the size T-4 pins the LS-CI PTY to). The queried size
  is clamped to `[1, 1000]` per side (bounds a garbled reply's buffer alloc).
  Live *resize* mid-edit (no winsize signal over UART) stays a Kaua seam — a
  re-query keybind / periodic poll is the v1.x answer.
- **Tabs** display as a single space (control chars are sanitized to space to
  keep the 1-cell-per-char model); the real char round-trips to disk. Insert-Tab
  inserts 4 soft spaces. Tab-to-tabstop expansion + a configurable tabstop is a
  v1.x nicety.
- **Long lines: clip or soft-wrap** — by default a line longer than the text
  width clips at the right edge (vertical scroll only); the `[Space]` palette's
  *toggle soft-wrap* (T2, #119) reflows long lines live (scrolling in visual
  rows). Horizontal scroll (a third mode) stays a seam.
- **Undo marks `modified`** even when undo returns to the saved state (a precise
  saved-state diff is a v1.x refinement).
- **Word motion** is WORD-granularity (whitespace-delimited); punctuation-class
  boundaries (vim `w` vs `W`) are a v1.x refinement.
- **Syntax highlighting** — a native lexer highlighter (and the tree-sitter
  feasibility, gated on the native-links-C toolchain, #67) is the documented
  v1.x direction (`docs/KAUA.md` §12).
