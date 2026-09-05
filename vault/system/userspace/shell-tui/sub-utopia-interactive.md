---
id: sub-utopia-interactive
type: sub
title: "ut's interactive layer — the line editor, the REPL loop, and the console the shell must hand back"
parent: moc-userspace-shell-tui
code:
  - usr/utopia/libutopia/src/line_editor.rs
  - usr/utopia/libutopia/src/repl.rs
  - usr/utopia/libutopia/src/completion.rs
  - usr/utopia/libutopia/src/palette.rs
  - usr/utopia/libutopia/src/ansi.rs
  - usr/utopia/libutopia/src/path.rs
  - usr/utopia/libutopia/src/lib.rs
  - usr/utopia/shell/src/main.rs
audit: light
guarded-by: [inv-i9, inv-i19, inv-i20, inv-i27]
validated-by: [prose, gate-interactive]
locks: []
hazards: []
abis: []
design: []
created: 2026-08-03
updated: 2026-09-05
area: userspace
---
## Purpose

The surface a person actually touches. [[sub-utopia-parser]] turns text into an
AST and [[sub-utopia-eval]] turns the AST into effects; this layer produces the
text in the first place — it reads bytes from a terminal, maintains an editable
line, paints it back, and decides when a line is finished.

Three tiers, deliberately separable. The **line editor** is a pure state machine:
bytes in, `EditorAction` out, no syscall anywhere. The **REPL** is fd-agnostic: it
consumes an already-read byte chunk and writes its rendering to an injected sink,
so it can be driven from a `Vec<u8>` in a test. Only the **`ut` binary** owns real
file descriptors, and it is the thinnest of the three.

What makes the layer more than presentation is that it owns the console's *mode*.
The editor draws its own echo, so the shell must put the terminal into raw
byte-at-a-time with kernel echo off — a state the user cannot type their way out
of — and must put it back around every foreground child and at every exit. That
is not a capability question: nothing in the kernel stops the shell from getting
it wrong, and a native binary is `panic = abort`, so a crashed child's own
cleanup never runs. The shell is the authoritative restorer of a console it does
not own.

## Contract

**The editor is pure.** `feed_byte`/`feed_bytes` take bytes and return an
`EditorAction`; `render(prompt)` returns a `String` of terminal bytes. Nothing in
`line_editor.rs` performs I/O, and the module holds no fd. A caller that never
installs a completion source or a command index gets byte-identical output to one
that does — both `set_completion_source` and `set_known_commands` default to
inert, which is what keeps the host tests and the bare-spawn boot check stable.

**The REPL is fd-agnostic.** `Repl::feed(bytes, out)` returns `Some(exit_code)`
when the session should end. Rendering goes to the injected sink; evaluation
*output* does not — a builtin's or a child's stdout goes to fd 1 directly. On a
real terminal `out` *is* fd 1, so the two interleave correctly; in a test the sink
captures only the shell's own painting.

**`ut` owns the descriptors** and the startup order. It decides whether this is a
session (a live fd 1) or the bare-spawn boot check, opens the note queue, runs the
session dance, installs completion and history, and drives the poll loop.

**The session's beacon tier is inherited, not probed (H-4d).** Part of the session
dance is reading `/env/BEACON` -- the render tier its pts host declared and the
kernel deep-copied in at spawn (a tile's `kaua-term --beacon rich` is the word; the
console's own `/dev/beacon` leaf describes a *different* renderer and is not
consulted for a tile). If the inherited word is `rich` AND stdout is a terminal
(`libthyla-rs::stdout_is_terminal()`, i.e. the console `'c'` OR a pts slave `'t'`),
`ut` arms its transcript zones (`set_beacon_rich`); otherwise it stays plain. It is
decided once at startup -- a tile's tier is fixed for its host's life, so there is
no per-prompt re-read. Degradation as everywhere: an unadvertised word or a
non-terminal fd yields plain.

**Degradation is the house style.** Every optional facility — completion, history,
the pts dance, the consctl mode-set, `$home` — fails to *absent*, never to fatal.
A missing home leaves history in memory; a failed `/bin` scan leaves builtins and
aliases completable; a rejected mode-set leaves the shell in whatever mode it
inherited. Nothing in this layer can fail startup.

## Mechanism

**The byte pipeline.** `ParserState` is a four-state machine — `Ground`, `Escape`,
`Csi`, `Utf8`. Ground maps C0 controls to emacs bindings (`Ctrl-A/E/B/F/K/U/W/Y/D`,
`Ctrl-C`, `Ctrl-L`, `Ctrl-R`, Tab) and enters `Escape` on ESC or `Utf8` on a
multi-byte leading byte. CSI collects up to four numeric parameters and dispatches
on the final byte (arrows, Home/End, Delete via `CSI 3~`). A stray ESC aborts to
Ground; an invalid UTF-8 continuation drops the sequence rather than inserting
garbage. Because a paste arrives byte-at-a-time, the UTF-8 state is what lets a
multi-byte character be inserted as one `char` rather than four broken ones.

**Deciding a line is finished.** Enter does not submit unconditionally. `balance()`
walks the buffer once tracking per-type bracket depth, single- and double-quote
state, backslash escapes, `#` comments, and whether the buffer ends in an
unescaped backslash. Unbalanced means insert a newline instead of submitting; a
trailing backslash is stripped before the newline (the rc/sh continuation). Depth
counters are signed, and `is_balanced` treats negative depth as balanced — a stray
`}` submits and lets the parser produce a real error, which is the natural shell
experience. The tracker is deliberately *not* a tokenizer; [[sub-utopia-parser]]
is authoritative, and this only has to decide "more input?".

**Rendering.** Single-line is `\r` + erase-to-end-of-line + prompt + buffer, then
`\r` and a cursor-right to `prompt_width + visible_columns_before_cursor`.
Multi-line emits the prompt on line 0 and a continuation prefix on each subsequent
line — padding plus a `⋮` at `prompt_width - 2` so the user's text aligns down the
column — then moves up to the cursor's line and right to its column. All column
arithmetic runs through `ansi::visible_width`, which treats CSI sequences as
zero-width, so inserting colour escapes cannot disturb the cursor.

**Command-line validity colouring.** `colorize_line0` takes line 0's first token
and binary-searches the installed command index: found renders `Fen` (green),
missing renders `Cinnabar` (red), live as the user types. A token containing `/` is
left uncoloured — a command-by-path is something a *name* index cannot speak to,
so it is left default rather than mis-flagged. An empty index disables colouring
entirely.

**Completion.** Tab dispatches to a `CompletionSource`. `ShellCompletionSource`
classifies from the buffer: a bare name in command position (start of line, or
after `| ; & { (`) completes against the command index; anything else — a later
argument, or a command-by-path — splits the token at its last `/` and reads the
directory live. `cd` restricts to directories. Each candidate carries its
terminator, a space for a command or file and `/` for a directory, so a unique pick
lands ready for the next token and a directory can be drilled with a second Tab.
The engine then extends to the longest common prefix; when the prefix is already
exhausted it enters the zsh-style cycling menu — apply candidate 0, emit
`MenuShow`, and let the REPL paint a one-line strip below the prompt. Tab cycles,
Enter finalizes without submitting, any other key dismisses and is re-dispatched.

**The command index is built once per accepted line** — builtins plus aliases plus
functions plus a cached `/bin` and `/goroot/bin` scan, sorted and deduped — and the
*same* sorted vector is handed to both the completion source and the validity
colouring. One index, two consumers, which is why a drift in it produces two
symptoms at once.

**Startup, in the order `ut` performs it, and the order matters.** Probe fd 1 to
discriminate a session from the boot check. Open the shell's note queue *first* —
the reason is written at both ends: once the session dance seats this shell as a
pts's foreground group, a `^C` posts `interrupt`, and a shell that is not yet
self-managing would be default-terminated by its very first keystroke. Then the
pts dance: if fd 0 is a pts slave, `setsid` → `t_tty_acquire(0)` →
`t_tty_set_fg(0, own_pgid)` → open `/dev/pts/<n>ctl` as the line-discipline fd. Any
partial failure degrades to the plain console path — no job control, no ldisc fd,
the shell still runs. On the console instead, the dance detects nothing and `ut`
uses login's forwarded `--consctl-fd`; a `!jc` gate keeps a stray forwarded fd from
clobbering the pts ctl. Either way the *same* `console_apply_default` writes the
*same* `PROMPT_MODE` vocabulary, defined once in `eval::console` and shared with
the foreground-child restore, so the two cannot drift apart.

**The loop** polls fd 0 and the note fd together. A note wake is serviced first —
a finished background job prints `[N]+ Done`, an idle `Ctrl-C` cancels the line —
and a simultaneous keystroke repaints over the notification. A note fd that errors
is removed rather than allowed to spin, degrading notes to sync-point delivery. A
poll error falls through to the read, which surfaces the same EOF and breaks.

## Data structures

`LineEditor` — the buffer plus a byte cursor always on a UTF-8 boundary, the kill
buffer, in-memory history with a navigation position and a saved current line, the
parser state, the mode, a desired column for vertical navigation, an optional
boxed completion source, and the command index.

`LineEditorMode` — `Normal`, `Search { query, match_index, saved_buffer,
saved_cursor }`, and `Menu { candidates, selected, anchor }`. The saved buffer is
how Ctrl-R cancels restore; the anchor is where the applied candidate begins.

`BalanceState` — three signed depths, two quote flags, and the trailing-backslash
flag.

`EditorAction` — `NoChange`, `Redraw`, `Accept(String)`, `Cancel`, `Eof`,
`ClearScreen`, `MenuShow { candidates, selected }`.

`Completions` — a byte range to replace plus candidate full-replacement strings in
source order.

`Repl` — the `Env`, the editor, the cached `/bin` scan, whether completion was
installed, an optional history path, and whether a menu strip is currently drawn.

`Role` and `Rgb` — nineteen semantic colour roles resolved by a `const fn` match.
The role *names* are the stable interface; hex is not, so a retheme changes one
file. Utopia's own disciplined programs use only four of the nineteen; the rest
exist for third-party and host-editor use, with the shell's validity colouring the
one deliberate exception.

## Concurrency

Single-threaded throughout, and no lock exists anywhere in this layer. The editor
is a value; the REPL owns it; `ut` drives one loop.

The concurrency that *does* exist is asynchronous note arrival, and it is handled
by ordering rather than locking. Notes reach the shell at three points: the poll
loop's idle service, a drain immediately before evaluating an accepted line, and a
drain after the command returns. The pre-evaluation drain is load-bearing and was
an audit finding — an `interrupt` queued while idle at the prompt, left in the
queue, would be forwarded by the next command's interruptible wait to a
just-spawned child and kill it spuriously. Draining first fires any handler and
discards an unhandled idle interrupt, so each command starts from a clean queue.

The layer consumes [[inv-i9]] rather than establishing it: the poll loop's
correctness rests on the kernel's readiness edges not being lost, which is the
console's deferred-wake relay and the note queue's wake.

## Invariants enforced

None of its own. This layer is a consumer:

- [[inv-i27]] — it is a client of the console, never an authority over it. The
  consctl fd is *forwarded* by login; the shell cannot mint one. Holding it confers
  the five mode flags and nothing else — not console attach, not the elevation
  gate.
- [[inv-i20]] — the pts dance is a sequence of kernel-gated calls, each of which
  can refuse; the shell's contribution is to attempt them in the right order and to
  degrade cleanly when one refuses.
- [[inv-i19]] — note delivery order and exactly-once consumption are the kernel's;
  the shell's obligation is to be self-managing *before* it becomes a signal target,
  which is the startup ordering above.
- [[inv-i9]] — the poll loop depends on readiness edges surviving the gap between
  sampling and blocking.

The one property this layer owes on its own is not an invariant with a number: the
console must end up cooked, with the cursor shown and the alternate screen left,
on every path out — clean exit, error, or a child that died mid-edit.

## Error paths

There is essentially one policy: absent, not fatal.

A missing `$home` returns `None` from the history path and no append ever happens.
An unreadable history file leaves history empty. A failed `/bin` scan leaves the
index with builtins, aliases and functions. A rejected consctl mode-set returns
false and the shell runs in whatever mode it inherited, printing a witness line
either way. Any step of the pts dance failing returns false and the whole session
falls back to the console path. A `read_dir` failure during completion yields no
candidates, so Tab is simply inert. A completion that would exceed the 64 KiB
buffer cap returns `NoChange` rather than truncating. A history append failure is
ignored entirely — append-only, so a torn write loses at most the trailing line.

The two paths that *do* end the session are Ctrl-D on an empty buffer (exit with
the last command's status) and a read returning EOF or an error.

## Performance

`balance()` is one pass per Enter. `render` is O(buffer) with a `visible_width`
pass over the prefix before the cursor. `refresh_command_index` runs after every
accepted line and is deliberately syscall-free — the `/bin` scan is cached at
install and only the alias and function tables are re-walked, then sorted and
deduped. Completion takes exactly one `read_dir` per Tab in argument position and
none in command position. Candidates are capped at 256 per Tab, which bounds both
the work and the menu strip.

The history cap is 10 000 entries in memory, with on-disk history appended
line-by-line at `~/.ut_history`, mode 0600 — the encrypted home already gates
access, and the per-file mode is defence in depth against a future where it does
not.

## Prosecution

- **Does the console get restored on every path?** Including a child that aborts,
  a parse error, and Ctrl-D mid-edit. `panic = abort` means a child's own `Drop`
  never runs, so the shell is the only restorer.
- **Is the startup order preserved?** `open_notes` before the pts dance is a real
  precondition, not a preference: seating the shell as foreground makes it a signal
  target, and an un-self-managing target is terminated by its first `^C`.
- **Do the two console paths stay one grammar?** The console (`--consctl-fd`) and
  pts (`/dev/pts/<n>ctl`) paths must write the same mode vocabulary; the restore
  after a foreground child must be byte-identical to the prompt-mode apply.
- **Does the cursor arithmetic survive colour?** Every escape the shell inserts
  into a *measured* string must be CSI, because `visible_width` recognizes nothing
  else.
- **Is the command index the same set the resolver searches?** Two consumers read
  it — completion and validity colouring — so a divergence produces both a missing
  completion and a wrong colour.
- **Does the completion cap interact correctly with the prefix extension?** The
  engine extends the buffer to the longest common prefix of whatever candidate set
  it is handed.
- **Are the editor's bounds defensive?** The buffer cap, the menu anchor's char
  boundaries, and the CSI parameter array are all fixed-size.

## Seams

- **Multi-line vertical navigation.** Up and Down are history-only; column-
  preserving cursor motion across a multi-line buffer is unbuilt (`desired_col`
  exists for it). This matches bash; zsh and fish do the other thing.
- **Display width is one column per `char`.** Combining marks and double-width
  glyphs render inconsistently. The buffer stays valid UTF-8 either way — this is a
  cursor-position defect, not a corruption one.
- **Non-CSI escapes are unrecognized by `visible_width`** — OSC and DCS would be
  over-counted. Currently safe only because nothing measured contains them.
- **The user's `prompt` function is not called.** `ut` emits a built-in default
  that mirrors the shipped rc prompt; capturing a user function's stdout needs rc
  loading plus function-output capture.
- **Path abbreviation is HOME-prefix only.** Bound-namespace abbreviation and
  width-budget middle-ellipsis truncation are unbuilt.
- **Multi-line history entries are not persisted** — an embedded newline would
  split into separate entries on reload, so such lines are skipped.
- **vi mode, bracketed paste, and modifier-key recognition** (Ctrl-arrow, Alt-x)
  are unbuilt.

## Caveats

**The header of `repl.rs` denies a mechanism its only caller implements.** It
states that `/dev/cons` is "a blocking-read-only Dev with NO `.poll` hook", that
the `ut` loop therefore blocks in `read()`, and that the multi-fd poll across the
input and note fds is a later chunk's work. All three are false. `devcons` carries
a `.poll` slot; `ut` polls both fds and breaks out of the loop on an idle note; and
the comment at that loop says so by name — "LS-8a made /dev/cons pollable (a
`.poll` hook + the deferred poll-wake relay), so the shell now polls stdin AND its
own note fd together". The correction was written, and written by someone looking
at the same subject; it was simply written at the call site instead of at the claim.
See the arc note in [[chg-2026-08-03-utopia-interactive-sweep]].

**The completion index and the command resolver disagree by one directory, and the
doc claims they agree.** `install_completion` scans `/bin` and `/goroot/bin`,
describing itself as "matching `resolve_command`'s search list so a resolvable
command is a completable one". The resolver searches three directories — `/bin`,
`/`, and `/goroot/bin`. Because the same index also drives validity colouring, a
command reachable only via `/` would both fail to complete *and* render cinnabar —
marked unresolvable while running fine. Currently latent: the session root holds
only data files, and the shell that does run from a root-level namespace is the
bare-spawn boot check, which never installs completion.

**The 256-candidate cap is applied before the sort, so which candidates survive is
filesystem-order-dependent.** Path completion iterates `read_dir` in FS order,
breaks at the cap, and *then* sorts — and the comment on that sort says it exists
so "the menu + LCP" are deterministic, which is the exact property the truncation
undoes. The engine computes its longest common prefix over whatever subset
survived, so in a directory with more than 256 matches Tab can extend the line to a
prefix that excludes valid candidates. Command completion is unaffected (its source
vector is already sorted, so the cap takes a deterministic first 256).

**A multi-line render that shrinks leaves stale lines on screen, and the fix was
assigned to a chunk that shipped without it.** The comment describes the defect
exactly and says the next chunk will track the previous render's line count and
emit an erase-to-end-of-screen. That chunk landed — it is `repl.rs` — and neither
the tracking nor the escape exists anywhere in the crate; the only screen clear is
Ctrl-L's full-screen one. What is worth noting is the stated reason it was
acceptable: "the boot probe only checks emitted bytes (not screen state) so this is
invisible". Invisible to the probe. Visible to the user, and nothing ever forced
the issue because the observer that would have complained cannot see screens.

**Construction snapshots again, now outside the eval modules.** `lib.rs` lists
`line_editor` under "modules deferred to later U-* chunks" four lines above the
`pub mod line_editor;` that declares it. `line_editor.rs` lists Tab completion as
deferred, fifty lines above the `MenuShow` variant that implements the menu, and
opens with the strategic claim that "v1.0 has no PTY surface" — the PTY arc has
landed, kernel seam and userspace server both. `path.rs` promises richer
abbreviation "at U-4 (the line editor)", which landed without it. This is the same
family as the eval-module headers and extends it to the crate's front door.

**The Bonfire palette rename reached the definition and nothing else.** The
migration commit touched exactly one file — `palette.rs`, which now opens "Bonfire
supersedes the U-1 *Pale Fire* palette". Twelve descriptions of the palette as
"Pale Fire" survive across seven files, including the shell's own `Cargo.toml` and
the banner in `main.rs`. Every colour is correct; every account of what the colours
are called is stale in six of the seven files that give one.

**`ansi.rs` asserts a discipline its sibling already breaks.** It documents that
non-CSI escapes would be over-counted and rests on "disciplined Utopia programs
emit only CSI 24-bit-color SGR + reset". The REPL's menu strip emits `ESC 7` and
`ESC 8` — save and restore cursor, two-byte non-CSI escapes that `visible_width`
would count as two columns each. Harmless today only because the strip is written
straight to the sink and never measured.

## Provenance
(generated -- incoming `touched` backlinks, newest first; never hand-written)
