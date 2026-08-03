---
id: sub-nora-engine
type: sub
title: "nora's editor engine — char-addressed text, an editor that raises requests instead of acting, and 238 tests the record called stranded"
parent: moc-userspace-shell-tui
code:
  - usr/nora/src/lib.rs
  - usr/nora/src/text.rs
  - usr/nora/src/editor.rs
  - usr/nora/src/wrap.rs
audit: light
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

What a keystroke does. The buffer that holds the text, the modal state machine
that interprets keys against it, and the soft-wrap arithmetic both the scroller
and the renderer share. This is the innermost of nora's three layers — the
rendering half and the process half are separate dossiers — and it is the layer
that decides what "pressing `d`" means.

**The organizing property is that it can neither act nor fail.** No function here
performs I/O, and no function here returns a `Result`. A save is not a save: it
is a `Request::Save` left in a field for someone else to execute. An out-of-range
cursor is not an error: it is clamped. Those two absences are what make the layer
exhaustively testable without a terminal, a filesystem, or a process — and unlike
most of the native tree, the tests genuinely run.

It sits on [[sub-kaua]] for exactly one thing — the `KeyEvent` type it matches on
— and on nothing else. No libthyla-rs, no syscall, no allocator beyond `alloc`.

## Contract

**Every position is a character index, never a byte offset.** `Pos` is
`(row, col)` with `col` a char index in `[0, char_len(row)]`. The engine is
char-addressed end to end so multi-byte UTF-8 can never split a grapheme, and the
two conversion functions (`byte_to_char_col` / `char_col_to_byte`) are the
declared bridge for anything arriving byte-addressed — a language server's
position, a byte-oriented tool's column. This is a contract rather than a
convention because the failure mode is invisible: on ASCII the two coordinates
coincide, so a mixup survives every ASCII test and lands the cursor off by
(bytes − chars) on the first accented character.

**The editor requests; it does not do.** File I/O is raised as a `Request`
(`Save` / `Open` / `ListDir`) for the binary to execute and report back through
`mark_saved` / `open_buffer` / `set_status`. Language-server queries are a
*separate* axis (`LspRequest`), and debugger commands a third (`DapRequest`). The
separation is deliberate and its reason is stated at the type: a `Request` is
synchronous work the binary completes, while the other two go to a child that may
be absent, slow, or never answer — so keeping them apart means the audited
save/open path is untouched by the language-server feature, and a server that
never replies can never wedge a save.

**The buffer is never empty and the cursor is always legal.** `lines` always
holds at least one line (an empty document is one empty line); `set_cursor`
clamps row to a valid line and col to that line's `[0, char_len]`. Deleting the
last line clears it rather than removing it.

**`rev` is a change detector, not a content hash.** It bumps on every content
mutation and never on cursor movement. Equal revisions imply equal content; a
different revision only implies a mutation happened — `replace_content` and a
successful `undo` both move it *forward*, because the content did change. It
exists so the LSP document-sync check on the typing path is O(1) instead of an
O(buffer) compare per keystroke.

**Undo is one user action, not one keystroke.** The editor pushes a checkpoint at
the start of an edit session — entering insert, before a structural normal-mode
edit — so a single `u` reverts a whole `3x` or `2d`. The stack is capped at 64.

**`readonly` disables edits, and the enforcement is at the doors.** See
Invariants.

## Mechanism

**Key dispatch is a fixed pipeline before it is a mode switch.** `handle_key`
clears the transient status, dismisses any hover popup *without consuming the
key* (so an unbidden popup — it lands whenever the server answers — never costs a
keystroke), then routes: an in-progress `s` pattern prompt wins first, then a
one-key pending prefix, then the mode's own handler. Eight modes have handlers;
`Menu` and `DebugMenu` share one.

**Normal mode resolves four things before it looks at the key.** Debug hot-keys
(F5/F10/F11) fire first when a session is live, regardless of which dashboard
pane has focus. Then a focused debug tile claims the navigation keys. Then a live
multi-cursor state collapses — any key but `,` collapses first and *then* acts,
so there is no stuck multi-state. Then a digit accumulates the count prefix,
where `0` counts as a digit only mid-count (bare `0` is the move-home motion),
the accumulation is `saturating_mul(10).saturating_add(d)` and additionally
`min(COUNT_MAX)`, and the running value echoes to the status line.

**The pending-prefix machine handles every two-key sequence** with one
`Option<Pending>`: a find-char target (`f`/`F`/`t`/`T`), the match/text-object
selector (`m` then `i`/`a`/`m`), the Helix goto prefix (`gg`/`ge`/`gd`), and the
bracket motions (`]d`/`[d`). It is `take`n at the top of `handle_key`, so a
prefix is always consumed by exactly the next key.

**Multi-cursor edits apply then shift, and that ordering is the correctness
pivot** — the file says so. Each caret's edit is applied in ascending order and
every *later* caret is then shifted to stay valid against the mutation just made:
`shift_after` for an insert/newline/backspace, `shift_after_delete` for a range
delete. `delete_all_carets` shifts anchor and head *independently*, with the
reason recorded — collapsing both to the head would destroy a multi-char
selection's range. The arithmetic is underflow-safe by construction: every
subtraction sits behind a guard that establishes its operand ordering.

**Soft wrap is a shared coordinate system, not a rendering trick.** `wrap.rs`
holds the visual-line math — a logical line of L chars occupies `ceil(L/tw)`
visual rows, a blank line occupies one — and both the scroll anchor
(`Editor::scroll_to`) and the renderer walk it through the same helpers. Keeping
them in one module is what keeps scroll and render agreeing on the geometry; a
disagreement would desync the on-screen cursor. Vertical movement is wrap-aware
at key time, before the next render, which is why the editor caches the viewport
width from the last `scroll_to`.

**Multiple buffers park and restore through one pair of functions.** The *active*
buffer lives in the `Editor`'s own fields, so every per-mode handler is unchanged
by multi-buffer existing; `save_active` mirrors those fields into `bufs[active]`
on switch-away and `load_active` restores them. `load_active` additionally
*clears* the transients that are keyed to a position in the file being left —
carets, the split prompt, the pending prefix, the count, diagnostics, hover, the
completion list and its prefix — each with its reason recorded at the line.

## Data structures

`TextBuffer` — a `Vec<String>` of logical lines, a `(row, col)` char cursor, a
bounded `Vec<Snapshot>` undo stack, and the `rev` counter. `Clone`, so a
backgrounded buffer parks its full state including undo history.

`Snapshot` — lines plus cursor. A checkpoint clones the whole line vector.

`Editor` — two field groups that are not marked apart by the type system. The
first mirrors `bufs[active]` (text, filename, readonly, modified, the three
scroll anchors, the visual anchor, the last search); the rest is editor-global
(mode, wrap, register, status, the three pending request slots, the debug view,
the dashboard state, the carets, the pending prefix, the count).

`DocState` — the parked half of that first group; nine fields, and the pairing
with `save_active`/`load_active` is by hand.

`Sel { anchor, head }` — one selection, ordered on demand by `range()`. Multi-
cursor is a `Vec<Sel>` where `carets[0]` is primary and synced to the
`TextBuffer` cursor; *empty* means single-cursor mode, so the single-cursor paths
are untouched by the feature existing.

`Mode` — eight variants, three carrying state (the command line, the two picker
states, the completion selection).

`Request` / `LspRequest` / `DapRequest` — the three async axes. `LspRequest` is
deliberately position-free: the editor has no URI and no notion of the server's
position encoding, so it says "definition at my cursor" and the host translates.

`Pending`, `FindKind`, `MultiEdit`, `Class` — small closed enums for the
prefix machine, the find-char direction, the multi-cursor edit kind, and word
motion's character classes.

## Concurrency

None. Single-threaded by construction, no lock anywhere, no shared state — the
binary drives `handle_key` from one loop. Every async arrival (a diagnostic, a
completion list, a debugger stop) reaches the engine as a *method call* from that
same loop, never as a concurrent write.

## Invariants enforced

None of the enumerated system invariants — and that is worth stating plainly,
because this dossier is close to unique in the vault for it. The engine performs
no syscall, holds no capability, and touches no kernel surface. A bug here
corrupts the user's own unsaved text and nothing else.

The properties it does enforce are its own, and three are load-bearing enough to
name:

- **`lines` is never empty**, so every accessor can index line 0 unconditionally.
- **`rev` bumps on content mutation and only on content mutation** — enforced by
  a test that enumerates every mutator from a fresh buffer so one cannot mask
  another, and separately asserts that a battery of cursor moves does *not* bump.
  The test names its own purpose: a mutator added later without a bump silently
  costs the LSP client a document sync.
- **A read-only buffer cannot be edited** — but this one is enforced entirely at
  the *doors*, not at the mutators. Every key that would edit carries an explicit
  `if editable` guard, and every entrance to Insert mode is gated: `i`/`a`/`o`/`A`
  in Normal, and `change()` from Visual, which re-checks `readonly` at its top
  even though its only caller already did. There are seven assignments of
  `Mode::Insert` in the file and all seven are accounted for, so the property
  holds today. See Caveats for what that shape costs.

## Error paths

There are none, in the sense of a `Result`. No function in the engine can fail.
Out-of-range inputs clamp: `set_cursor` clamps both coordinates, `line()` returns
`""` past the end, `char_at` returns `None`, `byte_to_char_col` on an offset past
the end yields the line's char length, and an offset landing *inside* a character
rounds down to that character's column — so the result is always a column the
cursor may legally occupy.

The user-facing error channel is the transient `status` string, which the binary
also writes into when an I/O request it executed fails.

The one consequence of total clamping worth stating: a wrong position never
announces itself. It lands somewhere plausible instead.

## Performance

A checkpoint clones the entire line vector, so the undo stack's worst case is
roughly `UNDO_CAP * file_size` — 64 full copies. The cap exists for that reason
and is tested.

`char_len` is `chars().count()` — O(line) per call, and it is called on most
motions. `find_all` is O(buffer) and runs on each `s` prompt commit. Neither has
been a problem at the file sizes nora opens, and both are the honest cost of the
char-addressed contract; an index would be the optimization.

The count prefix is bounded twice over (saturating arithmetic, then a cap), so
`999999999d` cannot spin.

## Prosecution

- **Does every new content mutator bump `rev`?** The enumerating test is the
  guard, and it only guards what it enumerates — a new mutator must be added to
  it, or the LSP client silently stops syncing.
- **Is every door into Insert still gated?** The property is enforced at seven
  sites, not at the mutators; `insert()` itself has no gate and no stated
  precondition. An eighth door added ungated makes a read-only buffer editable.
- **Does `pending_jump` disarm on every path out of a cross-file jump?** It is
  taken by the consumer, so a path that skips the consumer leaves it armed.
- **Do multi-cursor edits still shift anchor and head independently?** Collapsing
  both to the head destroys a multi-char selection, and the loop is the only
  thing keeping the not-yet-processed carets valid.
- **Do `scroll_to` and the renderer still walk the same wrap helpers?** A private
  copy of the visual-row math in either place desyncs the on-screen cursor from
  the text under it.
- **Is `readonly` still copied on both sides of the park/restore pair?** The nine
  `DocState` fields are mirrored by hand in two functions; a field added to one
  and not the other silently loses state on a buffer switch.

## Seams

- **Tab renders as a single space.** A tab is one character in the buffer and
  round-trips to disk unchanged; expanding to a tabstop is the view's decision
  and is not made. Insert-mode Tab inserts four spaces so new content stays
  one-cell-per-char.
- **No system clipboard.** The yank register is internal — Thylacine has none at
  v1.0.
- **A col-0 line-join does not shift other carets** during a multi-cursor
  backspace. Documented as rare in a multi-insert and left.
- **Character indexing is linear.** Every `char_len` / `char_at` walks the line.
- **`find` and `find_all` are literal, single-line, and non-overlapping.** No
  regex, and a pattern is never matched across a newline.

## Caveats

- **`find_all`'s documentation opens with a description of a different
  function.** The doc comment above it reads "Find `pat` starting just after the
  cursor, wrapping to the top. Returns the match-start position" — which is
  `find`'s contract, not `find_all`'s, since `find_all` returns *every* match,
  buffer-wide, and neither starts at the cursor nor wraps. The mechanism is
  visible in the layout: `find_all` was inserted between `find`'s doc comment and
  `find`'s signature, so the comment silently re-parented to the new function and
  `find` was left with none. The rendered docs therefore give `find_all` the
  wrong contract and `find` no contract at all. Sibling of the same shape in
  [[sub-kaua]], by a different mechanism — that one updated the wrong prose, this
  one moved the right prose to the wrong symbol.

- **`find`'s wrap pass is written twice, and the second copy is dead.** The
  search loop runs `0..=n` over n lines, so its final iteration revisits the
  cursor's own line with the start offset at 0 — which *is* the wrap pass, and it
  returns any earlier match on that line. Immediately below it sits an explicit
  `if off == n` block performing the identical search over the identical line,
  which can only run when the loop body just failed to find the same thing. It is
  harmless and it is also the block a reader checks when verifying that wrapping
  works, so the loop bound that actually does the work reads as incidental. A
  later tightening of the bound to `0..n` would leave the wrap silently resting
  on the redundant copy.

- **A failed cross-file jump stays armed and lands on the next file opened.**
  `jump_to` parks the target position in `pending_jump` and raises
  `Request::Open`; `open_buffer` takes it. The take is annotated "so an ordinary
  later `:e` can never inherit a stale jump" — and it delivers that for the
  success path and for the two soft-failure paths (a missing file and a
  `NotFound` both still open an empty buffer, consuming the jump). The binary's
  *hard* read-error path is the exception: it reports the error and returns
  without calling `open_buffer`, leaving `pending_jump` set. The next successful
  `:e`, of any file, then moves the cursor to a line meant for a different one.
  Narrow to reach — it needs a read error on a file that exists — and cosmetic
  when reached. The disarm lives at the consumer, so it fires only when the
  consumer runs. Task #124.

- **`insert()` has no read-only gate and no stated precondition.** Every arm of
  it — Enter, Backspace, Delete, Tab, and the catch-all `Char(c)` — mutates the
  buffer unconditionally. The read-only property is real and currently holds,
  because all seven entrances to Insert mode are gated; but nothing in `insert()`
  records that it depends on its callers, and the surface it would expose is the
  whole typing path. What makes this worth writing down rather than shrugging at
  is that the author's instinct was already the other way: `change()` re-checks
  `readonly` at its top *even though* its only caller has just checked it. The
  defense-in-depth was applied to the three-line function and not the fifty-line
  one. Task #125.

- **The record said this crate's 238 tests could not run. They all run.** An
  earlier sweep listed nora among the crates whose test functions are stranded by
  the workspace's bare-metal target. nora carries `#![cfg_attr(not(test),
  no_std)]`, gates libthyla-rs behind an optional `backend` feature, and
  documents the exact host-test invocation in its own `Cargo.toml` — including
  the parenthetical explaining why `--lib` is required (the bin needs the
  backend; the lib does not). Run that way, all 238 pass. The corrected
  cross-crate figure is 489 running and 389 stranded, and the two crates that
  genuinely cannot host-test — `libutopia` and `tapestryd` — are stranded for a
  different and real reason: both depend on libthyla-rs *unconditionally*, so the
  build reaches its aarch64 `_start` assembly. See
  [[chg-2026-08-03-nora-engine-sweep]] and task #105.

## Provenance
(generated -- incoming `touched` backlinks, newest first; never hand-written)
