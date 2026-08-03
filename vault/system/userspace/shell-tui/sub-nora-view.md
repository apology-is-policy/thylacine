---
id: sub-nora-view
type: sub
title: "nora's renderer and display models — one geometry shared with a layer that cannot see the screen, and five models defined by what they refuse to know"
parent: moc-userspace-shell-tui
code:
  - usr/nora/src/view.rs
  - usr/nora/src/syntax.rs
  - usr/nora/src/theme.rs
  - usr/nora/src/diag.rs
  - usr/nora/src/debug.rs
  - usr/nora/src/vartree.rs
audit: none
guarded-by: []
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

What reaches the screen. One renderer that turns an `Editor` into a grid of
cells, and five display models holding the data it draws — syntax classes, the
palette, diagnostics, the debugger snapshot, and the variable tree. The middle
of nora's three layers: [[sub-nora-engine]] decides what a keystroke means, the
process half owns the terminal and the two child servers, and this is what sits
between them.

**Every file here is defined by something it deliberately refuses to know.** The
renderer knows no terminal — it reads [[sub-kaua]]'s pure layers only, and hands
back a cursor coordinate for someone else to place. `diag` knows no LSP. `debug`
knows no DAP. `vartree` knows no protocol at all. `syntax` refuses to know the
grammar, and is a lexer precisely so it can classify half-typed input that no
parser would accept. Each refusal buys the same thing: the layer can be driven
from a literal in a test, so the 83 assertions here run on a host with no
process, no server, and no screen.

The one place a refusal has a real cost is geometry. The renderer is the only
layer that knows the screen's shape, but the scroller and the cursor-movement
keys need that shape too, and they run at key-time rather than draw-time. That
cost is paid by exporting the calculation rather than duplicating it, and the
whole correctness story of this dossier is that agreement.

## Contract

One entry point for drawing and two for geometry:

- `render(&Editor, Rect, &mut Buffer) -> (u16, u16)` — paint everything, return
  the on-screen cursor. Never fails, never allocates outside `alloc`, never
  touches a file descriptor.
- `text_metrics(Rect, line_count, tabs) -> (gutter_w, text_width, text_height)`
  — the text region's geometry.
- `editor_area(&Editor, Rect) -> Rect` — the editor's sub-rect once the debugger
  dashboard has taken its columns, or the whole area when there is no dashboard.

The models are plain data with small query surfaces: `Diagnostics`
(set / clear / per-line lookup / counts / wrapping navigation), `DebugView` and
its three row types, `VarNode` plus the tree operations (`flatten`,
`visible_count`, `visible_path`, `node_at_path_mut`, `find_by_ref_mut`), `Lang`
(`from_filename`, `highlight_line`, `line_classes`), and `theme`'s style
constructors.

## Mechanism

### The geometry is exported so it cannot be duplicated

Soft-wrap makes the viewport width load-bearing outside the renderer: which
visual row the cursor occupies depends on where lines break, so the scroller
has to break them the same way. Three things guarantee it does.

First, `text_metrics` and `render_editor` derive the text rect by the same two
steps — strip the optional buffer-tab row, then split off the one-row status
line — through a shared `body_area` helper, so neither can drift from the other.
Second, `dash_split` is the single source of the dashboard's column and row
split, consulted by both `render` and `editor_area`, so the width the binary
scrolls to is the width the renderer draws into. Third, the arithmetic itself
lives in one module: both this layer and the engine's scroller call the same
`wrap::forward` / `cursor_visual` / `back_n` / `row_rows`, rather than each
holding a private walk.

The binary closes the loop each frame: take `editor_area`, take `text_metrics`
of that, scroll to it, then render. The comment at that call site names the
consequence of getting it wrong — a mismatch desyncs the wrapped cursor — and
the same warning appears at both exported functions.

### The renderer composes; it does not branch on a mode flag

`render` dispatches through three shapes rather than one conditional tree: zoom
(one focused pane fills the area), the dashboard split (sidebar and console
tiles around a narrowed editor), and full width. All three converge on
`render_editor`, which is the entire pre-dashboard renderer unchanged — so the
dashboard was added by narrowing a rectangle, not by rewriting the drawing.

Within `render_editor` the overlays are ordered by who owns the cursor. The
completion popup, the which-key menu and the two pickers each *return* their own
cursor position and stop; the command popup and the hint draw and fall through;
hover paints last, over everything including the block cursor, and deliberately
does not take the cursor because it is non-modal and the next key dismisses it.

### The scroll offset is computed, not stored

A tile that overflows shows a scrollbar and scrolls to its selection with no
retained state: the offset is derived so that a selection past the window
bottom-anchors onto the window's last row. The same shape appears in the
completion popup, which windows the candidate list *before* formatting any row —
the comment there states what the other order would silently do, which is drop
the highlight off the end once the selection passes the last formatted row.

### The Call Stack separates a frame index from a row index

The unified user-to-kernel stack draws a divider between the Go frames and the
kernel frames. That divider is a visual-only row, so the selection is a *frame*
index and the renderer maps it forward past the divider — navigation can never
land on the divider, and pressing Enter on a kernel frame selects that frame
rather than the furniture above it.

### Highlighting is a lexer, and its imprecision is stated

Two per-line scanners, one for the shell language and one for Go, each producing
character-indexed class spans. Per-line means a string or block comment spanning
a newline loses its colour at the break — recorded as the v1 imprecision rather
than discovered as a bug. The scanners are resilient by construction: an
unterminated string runs to end-of-line rather than failing, which is the
property a *live* highlighter needs, since a buffer under the cursor is invalid
for most keystrokes.

The shell keyword set is a copy of the parser's, because nora cannot import
`libutopia` — see Caveats for why, and for what the copy is and is not pinned
against.

## Data structures

- **`Diagnostics`** — a flat `Vec<LineDiag>`, deliberately unindexed: a compiler
  stops after a handful of errors per file, and the per-row scan is bounded by
  the viewport height rather than the file length. `Severity` derives `Ord`
  most-severe-first so `min` picks the winner when several land on one line,
  with ties broken on the earliest column so the gutter and the status line
  always agree on which diagnostic they are showing.
- **`LineDiag`** — line, start and end column, severity, message. Columns are
  **character** columns; see Caveats.
- **`DebugView`** — status line, call stack, current frame's locals, goroutines,
  console scrollback. `StackRow` carries a `kernel` flag that drives both the
  divider position and the dimmed style.
- **`VarNode`** — the lazily-fetched variable tree: a DAP reference, an
  `expanded` flag and a separate `fetched` flag so a re-expand does not re-issue
  the request. `flatten` and `visible_path` are inverses over the same
  depth-first order, which is what lets the editor's row cursor be a flat index
  into a tree.
- **`HlSpan` / `HlClass`** — character-indexed runs. `line_classes` expands them
  to one class per character, which is the form the per-cell painter wants.
- **`theme`** — colour constants and style constructors, no state.

## Concurrency

None. Single-threaded, no locks, no shared mutable state, no interior
mutability. The renderer takes `&Editor` and `&mut Buffer` and touches nothing
else.

## Invariants enforced

**None from the enumerated set.** Nothing here holds a capability, performs a
syscall, or names a kernel object.

The layer does carry one property worth stating because it has no invariant
number and is the thing most likely to break silently: **the width the scroller
wraps at and the width the renderer wraps at are the same width.** When they
diverge the cursor lands on the wrong visual row — no crash, no error, just a
cursor that sits somewhere other than the character it is editing. Three
separate mechanisms hold it (above), and all three are commented with that
consequence rather than with what they do.

## Error paths

There are none. No function in these six files returns a `Result` or panics on
its documented inputs. Out-of-range indices clamp, empty inputs produce empty
output, an unrecognized filename yields no highlighting, and a malformed backtrace
line is skipped. `render` on an area too small to hold the status line falls back
to overlapping it with the text region rather than refusing to draw.

This is the same absence [[sub-nora-engine]] has, and for the same reason: a
renderer that can fail gives its caller a decision to make in the middle of a
frame, and there is no good answer.

## Performance

Not a measured surface. Per frame the renderer walks the visible rows only —
the loop is bounded by the viewport height, never by the file length — and
re-lexes just those lines for syntax classes. The diff-and-emit that follows is
[[sub-kaua]]'s. Row strings for the dashboard tiles are formatted per frame and
dropped; the completion popup formats only the rows it will draw.

## Prosecution

- **A new consumer of the viewport width must take it from `text_metrics`.**
  Computing a width locally — even correctly, even once — reintroduces the
  desync as soon as one of the two moves. The same applies to the wrap walk: use
  the shared module, never a private copy.
- **A new dashboard pane must go through `dash_split`**, or `editor_area` and
  `render` will disagree about how much room the editor has.
- **A new string drawn from another program must be sanitized on the way in.**
  The widget path does not do it for you — see Caveats, which is presently a
  live gap rather than a rule already kept.
- **A new selectable overlay must decide whether it owns the cursor** and return
  early if it does; falling through leaves the cursor on the text underneath.
- **A new row type in the Call Stack must preserve the frame-index-to-row-index
  mapping**, or the selection silently addresses the wrong frame.
- **A new mutator of the variable tree must keep `flatten` and `visible_path`
  inverse**, since the editor's row cursor is an index produced by one and
  consumed by the other.

## Seams

- **Tab expansion.** A tab renders as a single space rather than advancing to a
  tab stop, so a tab-indented file shows the wrong column positions. Named in
  the module header.
- **Multi-line syntax state.** A string or block comment crossing a newline
  loses its colour; a stateful scanner is the recorded refinement.
- **Multi-cursor in wrap mode.** Extra carets paint as blocks in plain mode
  only; in soft-wrap they show as highlighted ranges.
- **`HlClass::Operator`** exists and is never emitted — reserved, with the reason
  stated (operator characters appear inside bare words and globs, so a lexical
  scan produces false positives).
- **Kernel frames carry no source location**, because kernel DWARF is deferred.

## Caveats

- **Text from another program reaches the terminal without sanitization, on the
  paths where it matters, while the user's own text is sanitized on the path
  where it does not.** This file's two drawing helpers map any control character
  to a space, and they are used for the buffer text, the command line and the
  debug status line. The twelve widget call sites — hover, completion, the
  console scrollback, the variables tree, the call stack, the goroutine list and
  the status line's centre slot — do not: kaua's widgets write through a buffer
  primitive that passes characters through unchanged, and the terminal encoder
  emits each cell's character verbatim. Adjacent same-styled cells emit
  back-to-back with no move or style bytes between them, so an escape sequence
  written across consecutive cells arrives at the terminal intact. The reachable
  producers are a debuggee's own stdout (it lands in the Console tile), a
  debuggee variable's value, and a language server's hover, diagnostic and
  completion text. The consequence is display integrity rather than privilege —
  nora holds nothing the user does not — but the mitigation already exists in
  this file, applied to the one input that is least untrusted. Task #130.

- **The diagnostics header names the wrong coordinate in its opening sentence,
  six lines before warning about exactly that mistake.** It opens by saying the
  engine knows "byte columns", and describes the binary as converting the
  server's character offsets to byte offsets. Eleven lines later the same header
  states that columns are character columns, that a producer holding byte offsets
  must convert, and that getting this wrong "is invisible on ASCII and lands the
  cursor off by (bytes - chars) on the first non-ASCII line". The second half is
  correct — the field documentation says character column, and the real
  conversion is two steps ending in characters, not one step ending in bytes. So
  the stale sentence describes the intermediate step as the destination, and a
  producer who reads the first paragraph and stops builds precisely the bug the
  second paragraph exists to prevent. Task #131.

- **The backtrace parser's stated guarantee is stronger than its guard.** Its
  documentation says a line without the `#<index>` shape is skipped, "never a
  wrong row". A line with the hash but no index is not skipped: stripping the
  leading digits from a digit-less remainder is a no-op, so the whole remainder
  becomes the frame name. Unreachable today — the sole producer is the kernel's
  own backtrace file, whose format is fixed — and the tests cover the
  hash-with-no-symbol case but not the hash-with-no-index one. Task #132.

- **The keyword list is pinned to itself, not to its source.** The shell keyword
  set is a copy of the parser's, and the test that guards it fixes this copy
  against a literal in the same file — so a careless edit here fails, but the
  parser gaining a keyword fires nothing. The two are in sync today (sixteen
  words, identical sets, verified). What makes this the right call rather than an
  oversight is that the cross-crate test is impossible for the same reason the
  copy exists: `libutopia` depends on the aarch64-only runtime crate
  unconditionally, which is what strands its own tests as well. One dependency
  fact, two visible costs. The module header says all of this and uses the
  accurate verb — the test *fixes* the set, it does not *check* it against
  anything — and names the shared-crate refactor that would make the match a
  compile-time guarantee. Same family as the ABI mirrors each pinned only to
  themselves, with a far smaller consequence: a new keyword simply goes
  uncoloured.

- **`find_by_ref_mut`'s stated precondition is false for the one value that is
  not unique.** Its documentation says references are unique within a stop, which
  holds for every reference except zero — the value every leaf carries. The
  single caller guards it explicitly, with a comment naming the exact hazard, so
  the tree walk is never reached with zero. Worth recording because the guard and
  the claim live in different files, and a second caller would inherit the claim
  without the guard.

- **The palette is clean, and the tracked residue is larger than tracked.** The
  colour file was corrected to the current palette and carries one mention of the
  retired name, which is the correction record itself. The surviving descriptions
  of the old palette are entirely in documentation — measured at 55 occurrences
  across 13 files, against the twelve-across-seven the tracking item claims,
  including a page that still quotes the retired hex values as what the prompt
  renders. Task #113, corrected.

## Provenance

[[chg-2026-08-03-nora-view-sweep]].
