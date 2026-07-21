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
| Normal | `i`/`a`/`o`/`A` → Insert; `v` → Visual; `:` / `/` → Command; `Space` → Palette; `h j k l` + arrows, `0`/`$`/`gg`/`ge`/`G`/`{n}G`/PageUp/PageDown nav; `gd` go-to-definition, `K` hover, `]d`/`[d` diagnostics (see the language-server section); `w`/`b`/`e`/`W` punctuation-aware word motions; `f`/`F`/`t`/`T` find-char (+ `;`/`,` repeat / reverse-repeat); `mi<o>`/`ma<o>` inner/around text objects (`o` ∈ word/`(`/`[`/`{`/`"`/…) + `mm` jump-to-match-bracket; a numeric **count prefix** (`3w`, `5j`, `2dd`); `%`/`s`/`,` multi-cursor (select-all / split-into-carets / collapse) + `c` multi-edit; `x` delete char; `d` delete line (or the selection / every caret); `y` yank; `p` paste register; `u` undo; `n` repeat search |
| Insert | printable → insert; Enter → split; Backspace/Delete; Tab → 4 spaces (soft tab); Esc → Normal |
| Visual | `h j k l 0 $ gg ge G w b` extend the selection (the same goto prefix as Normal, so `gg`/`ge` mean one thing everywhere); `y` yank; `d`/`x` cut; Esc → cancel |
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
- **Multi-cursor** (`editor.rs` `carets: Vec<Sel>`, the Helix `%`/`s`/`c` flow) is
  empty == single-cursor (the legacy `cursor`/`selection` path, untouched) and
  non-empty == multi, with `carets[0]` the primary (it drives scrolling + the
  block cursor). `%` selects the whole buffer, `s` splits the selection's matches
  into one caret each, `c` enters a simultaneous insert applied at every caret per
  keystroke, and `,` (or any mode switch) collapses back to the primary. A
  `MultiEdit` (Char/Newline/Backspace) re-runs at each caret; `shift_after`
  re-bases the not-yet-processed carets as the buffer mutates so their offsets stay
  valid (the v1 backspace-across-carets case is the documented narrow seam).
- **Text objects + find-char + counts** are Normal-mode parse state, not new
  modes: a `Pending` (e.g. `FindChar(FindKind)`, the `m`-prefix object) holds the
  half-typed operator until its argument key arrives; a numeric prefix accumulates
  a `count` consumed by the next motion/operator (`3w`, `2dd`, `{n}G`).
- **Syntax highlighting** is a native UT lexer highlighter (the KAUA §12 lexer
  model, not tree-sitter at v1.0): the view colors tokens from the Bonfire syntax
  hues. Off the edit hot-path (re-lexed per redraw, at human input speed).

## Format-on-save (gofmt, Go Stage 6)

A `:w`/`:wq` of a path ending `.go` pipes the buffer THROUGH gofmt before the
durable write (`main.rs::gofmt_source` -> `/goroot/bin/gofmt`, stdin->stdout
mode): one write lands the formatted bytes; unformatted content never touches
disk; no tempfile, no `-w`-then-reload. On success with changed output,
`TextBuffer::replace_content` swaps the buffer (an undo checkpoint first --
one `u` restores the pre-format text; cursor clamped to the new bounds).

**A formatter can never block or lose a save**: a gofmt reject (the normal
mid-edit syntax error) or an absent toolchain (non-bake image, confined
namespace -> spawn fails) saves the buffer AS-IS; a reject additionally notes
the first diagnostic line in the status bar ("saved unformatted; gofmt: ...").
Guards on the adopt path: empty-output-for-nonempty-input and non-UTF-8 both
refuse the result (Unavailable) rather than clobber the buffer; the stdout
read is capped at MAX_FILE (slurp_capped errors on overflow, never truncates).

Deadlock-freedom: gofmt slurps ALL stdin before emitting (processFile does
io.ReadAll first), so write-all -> close -> read-stdout -> read-stderr is
safe; go/parser bails after ~10 errors, keeping stderr far under the 4 KiB
kernel pipe ring while stdout is being drained. All three child stdio slots
are `Piped`: the child can never scribble on the raw-mode alt-screen or read
nora's keystrokes (the I-27 posture is unchanged -- nora still never touches
consctl). nora is not self-managing (no notes fd), so a died-early gofmt's
`pipe` note is dropped by the kernel (LS-5: only `interrupt`
default-terminates) and the failed stdin write surfaces as a plain Err. The
child is ALWAYS reaped (nora is long-lived; an unreaped child is a zombie
leak). Proven end-to-end by `tools/interactive/go6.exp` (a tab appears in the
saved file that was never typed).

## Language server (gopls, Stage 8e-2)

A Go buffer brings up a persistent `gopls` alongside the editor and shows its
diagnostics inline. The client lives in `parley` (`docs/reference/141-parley.md`
has the full picture); nora's side is `src/lsp_host.rs` plus `src/diag.rs`.

- **The loop polls fd 0 and the server's pipes together** (one `poll(2)` via
  `parley::transport::Mux`), so an arriving diagnostic wakes the editor exactly
  like a keystroke. There is no tick -- a message nothing polls for never
  repaints.
- **The editor never blocks on it.** No language server (gopls absent, spawn
  refused, a non-Go buffer) is a fully supported state: nora behaves exactly as
  it did before 8e. A server that dies is reaped and dropped; editing continues.
- **`nora::diag` is protocol-free** -- line, CHARACTER columns, severity,
  message. `lsp_host` converts, spending the negotiated LSP position encoding
  against the real line text and then `text::byte_to_char_col`. Both halves are
  load-bearing: a server offset is bytes (or UTF-16 units) while every position
  in nora is a character column, and stopping at the byte offset is invisible on
  ASCII but lands the cursor inside a multi-byte character on the first accented
  line. (8e-2b stored byte columns; nothing read them, so it was latent until
  `]d` needed to move the cursor. Fixed in 8e-2c with the conversion helpers and
  their round-trip tests.)
- **Rendering**: the gutter number recolors (rust = error, gold = warning), the
  cursor line's message takes the status centre (an explicit status message
  still outranks it), and an `NE MW` tally rides the right slot.
- **Document sync at typing boundaries** -- on save and on leaving Insert, never
  per keystroke (the `NORA-IDE-UX` section 7 byte-storm discipline). The change
  test is `TextBuffer::rev()`, an O(1) counter bumped by content mutations only.

### The editor affordances (8e-2c)

| Key | Does |
|---|---|
| `]d` / `[d` | next / previous diagnostic, wrapping; echoes its message. **No server round-trip** -- the published set is already local, so it answers instantly even while gopls is busy or gone. |
| `K` | hover: describe the symbol under the cursor. Non-modal -- the popup is drawn last, over everything, and the next key dismisses it *and still does its job*, so hover never costs a keystroke. |
| `gd` | go to definition. Same-file moves the cursor; cross-file raises `Request::Open` and parks the position, which `open_buffer` applies once the buffer exists. |
| `Ctrl-N` (Insert) | completions. `Ctrl-N`/`Ctrl-P` or the arrows move, Enter/Tab accepts (replacing the typed word prefix, under one undo checkpoint), Esc cancels, any other key closes the popup and types. Every exit returns to Insert. |

**`g` is now the Helix goto prefix** (`gg` top, `ge` end, `gd` definition).
Bare `g` used to be "top"; `gg` is the Helix binding and `G` still goes to the
bottom, so the motion costs at most one extra key. A count still binds directly
(`42g` is goto-line, resolved before the prefix is ever set), and an unknown
second key cancels silently rather than doing something else.

The requests flow out through `Editor::take_lsp_request` -- a **separate axis
from `Request`** on purpose. `Request` is file I/O the binary performs
synchronously and reports back; an `LspRequest` goes to a server that may be
absent, slow, or never answer. Keeping them apart means the audited save/open
path is untouched by the language-server feature, and a client that never
answers can never wedge a save. The editor names neither the file nor the
position: it says "definition at my cursor" and `lsp_host` translates.

Answers are equally defensive: an empty hover reports instead of opening a blank
box; an empty completion list says so rather than opening a popup the user must
escape to learn nothing; and a completion answer arriving after the user left
Insert is **dropped**, because completing into a mode that never asked would be
an edit the user did not initiate.

**`gr` (references) is not built.** The client has no `textDocument/references`
request, and N locations need a picker; both are additive when wanted.

## Debugger (Ambush, Stage 8e-3e)

nora drives the Ambush (Delve) debugger from `:` commands — headless, before the
8f dashboard (NORA-IDE-UX §9: prove the debugger *loop* first). The design + the
`dap_host` state machine live in `141-parley.md`; the editor's part is small and
deliberately protocol-blind, exactly like the LSP seam:

| `:` command | Does |
|---|---|
| `:debug <prog>` | start debugging a compiled Go binary (stops at entry) |
| `:break <func>` | set a breakpoint by function name (`main.parkLoop`) |
| `:cont` / `:c` | continue until the next breakpoint or exit |
| `:next` / `:n`, `:step` / `:s`, `:stepout` / `:so` | step over / into / out |
| `:bt` | show the call stack in a `*backtrace*` scratch buffer |
| `:print <expr>` / `:p` | evaluate an expression at the stopped frame |
| `:kill` | end the session |

A verb sets `Editor::dap_request` (a `DapRequest`, the `LspRequest` sibling — a
**third async axis** alongside `Request`/`LspRequest`, for the same reason: the
debugger is a persistent child that may be absent, slow, or exit); the binary
drains it with `take_dap_request` and hands it to `dap_host`. The editor holds no
session state and speaks no protocol — it relays verbs and shows a status line
(state, stops, evaluate results) or the `*backtrace*` scratch (`open_scratch`, the
generalized `:help` pattern — a read-only buffer refreshed in place). Where the
Go toolchain is absent, `:debug` reports "debugger not installed" and editing is
unaffected. Ambush is baked at `/goroot/bin/ambush` (disk, on the login PATH,
beside the toolchain), reachable by a POST-pivot nora.

## Debugger dashboard (8f-2, NORA-IDE-UX §2)

While a session is live the editor splits into the ratified IDE dashboard:
`[editor | right sidebar]` over a full-width bottom Console. The sidebar stacks
three Kaua tiles — **Variables** (a `Tree`), **Call Stack** (a `Table`),
**Goroutines** (a `Table`) — and the Console is a `Tabs` strip (`Program`/`Debug`)
over the scrollback. It **auto-collapses** to a full-width editor when no session
is live (`Editor::debug_view()` is `None`), and collapses again on a terminal
below the floor (`< 50×14`, where the debug data still reaches the status line +
`:bt`). `Tab` cycles keyboard focus (editor → tiles → Console → editor); the
focused tile takes an ember border.

The architecture keeps the renderer pure. `dap_host` already pushes into the
`Editor` (`set_status`, `open_scratch`); the dashboard adds one more push — a
protocol-free `DebugView` snapshot (`nora::debug`: `StackRow`/`VarRow`/
`GoroutineRow` + a console log), rebuilt and set via `Editor::set_debug_view`
once per poll-wake and after each `:` command. `view::render` reads it and, when
present + roomy, splits the screen and draws the tiles (the pre-8f full-width
render is the `None`/too-small path, byte-for-byte); `view::editor_area` gives
the binary the editor sub-rect so `scroll_to` matches the width `render` draws.
On a stop the host chains `stackTrace → scopes → variables` (the locals) and a
`threads` fetch (the goroutines), so the tiles fill over a few wakes; a resume
clears them. Opening a session expands the dashboard immediately (empty tiles +
a "launching" status); ending it (`:kill`, exit, a dead stream) collapses back.

**8f-2a landed the skeleton** — the split + collapse + `Tab` focus + all three
tiles + the Console rendering live data at a basic level. **8f-2b-1 made the
tiles navigable** — a focused sidebar tile takes a row cursor (`j`/`k` move it,
`g`/`G` jump to the ends), the Variables `locals` group opens/shuts with `l`/`h`
(the per-variable nested expand of a struct/slice is a later leg), `l`/`h` steps
the Console `Program`/`Debug` tabs, and `Esc` returns focus to the editor; a tile
whose content overflows draws an ember scrollbar and scrolls to keep the
selection visible. The row cursor shows on the **focused tile only**; the
selection clamps to the live row count each render, so a stop that shrinks the
data can never leave it pointing off the end. The routing lives entirely in
`Editor::normal` (a focused-tile branch off the top, reachable only while
debugging — with no session the focus is always the editor, so ordinary editing
is byte-unchanged). **8f-2b-2 wired the tile actions** — `Enter` on the **Call
Stack** raises a `SelectFrame` (the host jumps the editor to that frame's source
and re-scopes the Variables tile to it via `scopes → variables`), `Enter` on
**Goroutines** raises a `SelectGoroutine` (the host switches the inspected thread
and re-roots the stack via `stackTrace`); `Enter` carries the row index clamped to
the live count so a stale selection names a live row, and the host resolves it
against its own cached list (a non-stopped target or an out-of-range index is a
reported no-op, never a wrong-frame jump). The editor half — `Enter` raising the
right request with the right index — is pure state and host-tested; the host half
is a binary-side DAP round-trip (`dap_host::select_frame` / `select_goroutine`)
covered by the `dap-nora` E2E. Still pending: **8f-2b-3** the nested-lazy
Variables tree (a `VarNode` tree replacing the flat locals, children fetched on
expand); **8f-2c** the `F5`/`F10`/`F11` hot-keys + `[Space]d` toggles; **8f-3**
the cross-boundary `── kernel ──` divider (§5). Kernel byte-unchanged; consumes
I-39; no new §28 invariant.

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
  the private `anchor` (visual), `register` (yank), `last_search`, `request` +
  the debugger dashboard state: `debug: Option<DebugView>` (`Some` == a session
  is live == the dashboard is shown) and `dash: DashState` (the `DashPane` focus,
  the Console tab, the per-tile row selections `var_sel`/`stack_sel`/`gor_sel`,
  and `locals_expanded` — kept across data refreshes, reset only on session
  start/end; the selections clamp to the live row count at render).
- `DebugView` (`nora::debug`) — the protocol-free dashboard snapshot the DAP host
  pushes: `status`, `frames: Vec<StackRow>`, `locals: Vec<VarRow>`,
  `goroutines: Vec<GoroutineRow>`, `console: Vec<String>`. Plain data (like
  `LineDiag`), so a second backend or a test populates the same struct.

## Tests (`cargo test -p nora --no-default-features --lib --target <host>`)

**195 host unit tests** over the pure engine (the per-module list below is a
partial breakdown of the original core; later chunks added `diag`, completion,
the 8e-3e **debug axis** — 5 tests asserting each `:` debug verb + its aliases
raise the right `DapRequest`, and that the argument-taking verbs report rather
than raise when given no argument — the 8f-2a **dashboard axis** — 3 `editor`
tests (collapse-until-pushed / a mid-session refresh keeps focus / `Tab` cycles
focus only while debugging) + 4 `view` tests (the tiles+editor coexist when
roomy, the collapse on a small terminal, no dashboard without a session, the
focused tile takes an ember border) — the 8f-2b-1 **navigation axis** — 6
`editor` tests (`j`/`k`/`g`/`G` move + clamp the selection, the shrink-clamp,
`l`/`h` collapse+expand the locals group, `l`/`h` step the Console tabs, `Esc`
returns focus, and the no-regression `j`-is-a-text-motion-over-a-focused-editor)
+ 4 `view` tests (the focused tile's row is highlighted, an unfocused tile shows
no cursor, an overflowing tile scrolls to the selection + shows a scrollbar,
collapsing the locals group hides the leaves) — and the 8f-2b-2 **actions axis** —
6 `editor` tests (`Enter` on the Call Stack raises `SelectFrame`, on Goroutines
raises `SelectGoroutine`, the clamp-a-stale-selection-before-selecting, inert on
Variables + Console, inert over an empty tile, and the no-regression
`Enter`-raises-no-request-over-a-focused-editor)):

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
| T-3 engine + modal core + renderer + binary | landed |
| T-4 `ut` raw-mode dance + `ls-7` LS-CI | landed |
| 8e-2 LSP client (gopls diagnostics/hover/def/completion, inline) | landed |
| 8e-3e debugger (`:debug` → Ambush, headless) + `dap-nora` LS-CI | landed |
| 8f-2a dashboard skeleton (split + collapse + `Tab` focus + tiles + Console) | landed |
| 8f-2b-1 navigable tiles (`j`/`k`/`g`/`G` select, `l`/`h` expand, scrollbars) | landed |
| 8f-2b-2 tile actions (Call Stack `Enter` → frame jump + re-scope, Goroutines `Enter` → re-root) | landed |
| 8f-2b-3 nested-lazy Variables tree, 8f-2c hot-keys, 8f-3 cross-boundary stack | pending |
| audit (Kaua backend + dance) | not started |

## Known caveats / seams

- **Lone-Esc under a long typed burst (task #65)**: nora repaints a full frame
  per keystroke, so a long burst of injected input is still draining when a
  trailing Esc arrives; kaua's parser resolves a lone Esc only at a read
  boundary, so an Esc at an in-burst read tail Alt-joins with the NEXT read's
  first byte -- insert mode never exits (the go6.exp attempt-1..3 failure:
  `:wq` typed into the buffer). Drivers type per-line + double-Esc + gate on
  the `NOR` status chip (see go6.exp); the real fix is kaua-side ESC-timeout
  disambiguation (#65). Human typing is far below the trigger rate.
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
