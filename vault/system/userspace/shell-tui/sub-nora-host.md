---
id: sub-nora-host
type: sub
title: "nora's process half — the only file that touches a terminal, and two children that must not outlive it"
parent: moc-userspace-shell-tui
code:
  - usr/nora/src/main.rs
  - usr/nora/src/lsp_host.rs
  - usr/nora/src/dap_host.rs
audit: light
guarded-by: []
validated-by: [prose, gate-interactive]
locks: []
hazards: []
abis: [abi-halcyon-palette]
design: []
created: 2026-08-03
updated: 2026-10-29
area: userspace
---
## Purpose

Everything the rest of nora refuses to do. [[sub-nora-engine]] raises a save as a
request and cannot perform it; [[sub-nora-view]] returns a cursor coordinate and
cannot place it. This is the layer that acts: it acquires the screen, reads the
keyboard, opens and writes files, and owns two child processes — a language
server and a debugger.

It is also the only part of nora with a boundary worth guarding, which is why it
carries an `audit` level the other two do not. It holds no capability and makes
no privileged call — the libthyla-rs surfaces it touches are allocation,
arguments, errors, files, I/O, notes, poll and process, and nothing else — but it
takes the console screen, spawns programs, and is the point where three other
programs' output enters the editor.

## Contract

One entry point (`rs_main`) and an event loop. Argument parsing accepts an
optional read-only flag and one filename. Exit is a status code; the screen is
restored on the way out, twice, by two independent mechanisms.

Externally the layer promises three things: that it never blocks the editor on a
server, that neither child outlives it, and that it never touches the console's
line discipline.

## Mechanism

### The console discipline is a division of labour with the shell

nora acquires the **screen** on stdout and reads bytes on stdin. It never opens
the console control file and never changes the line discipline — raw mode is set
by `ut` through its own private control fd before nora is spawned, and nora reads
stdin assuming the bytes already arrive raw. So nora is never console-attached,
and the elevation gate that keys on console attachment is untouched by it.

Restoration is doubled because one mechanism is not enough. On a clean exit the
terminal object's destructor restores the screen, and the exit path also calls
the restore explicitly; both are idempotent. Neither runs on a **crash**: a
native binary aborts on panic, so destructors do not run, which is why the
shell's post-reap restore is the real backstop. The layer is written knowing its
own cleanup is best-effort.

### The session palette is adopted once, before the first render

Before it parses arguments, `main` calls `adopt_session_palette`. It starts from
`theme::BONFIRE`, applies the session's `/env/HALCYON_PALETTE` roles over it if
present ([[abi-halcyon-palette]]), then the user's `$HOME/.config/nora/palette`
dotfile over that -- so precedence is **dotfile > /env > BONFIRE** (the dotfile
wins because it is applied last), and any source that is absent or unparseable
simply does not apply. It calls `theme::set_palette` exactly once, before the
first render, which is the whole basis of that global's soundness
([[sub-nora-view]] Concurrency). An unthemed console keeps Bonfire; an editor in
a Halcyon session tile follows the session theme without nora knowing anything
about Halcyon beyond the role vocabulary.

### Sizing is a round-trip, because there is no syscall to ask

There is no window-size syscall, so the terminal's dimensions are measured at
launch by a cursor-position round-trip with a deadline, clamped to sane bounds
so a garbled reply cannot drive a huge allocation, and falling back to a fixed
80x24 when nothing answers. A reply that arrives *after* the deadline is not
lost: the input source delivers a late unsolicited report as a resize event, and
the loop applies it — the steady-state backstop for a slow serial link under
hypervisor emulation, where the launch probe alone was not enough.

Live resize has a second source: the console posts a note when the renderer
reweaves, and the loop re-reads the authoritative geometry file. On a serial
console, where that file reports nothing, it falls back to re-issuing the
round-trip — the same late-reply path, reused.

### One poll covers the keyboard, the note queue and both children

Each round builds one descriptor set — stdin, the note queue, and whatever
descriptors the live servers offer — and blocks. A keystroke, a diagnostic, a
debugger stop and a console resize all wake the loop identically. **There is no
tick**, which is the design's sharp edge stated in its own comment: a message
that nothing polls for is a message that never repaints.

The keyboard's descriptor is kaua's `poll_fd()` — fd 0 on the console, the pts's
`ready` sibling in a tile. When it fires, nora takes a zero-timeout burst from
kaua's `PollSource` ([[sub-kaua]]), one key at a time, and stops at the key that
sets its quit flag: kaua reads one byte per read, so a command typed straight
after `:q` is never taken into the editor and goes to the shell (`nora-demo.exp`).
The same burst runs once before the loop, for the type-ahead the launch probe
pulled in, which the descriptor would never announce.

A dead server contributes an empty descriptor list, so its fds are never polled
again — safe by construction rather than by bookkeeping, because the set is
rebuilt every round rather than mutated.

The poll has exactly one timeout. While a debug session is ending (below), the
round blocks no longer than that session's deadline, and every wake checks the
deadline. A debugger that never answers is still killed on time, without a
general tick.

### Neither child may outlive the editor

Both hosts carry a dead flag and a reaper that kills and waits once. A failed
write to a child reaps it on the spot: the session is then dropped and never
polled again, so it is the last chance to collect the child. The reason is stated
rather than assumed — an orphaned language server holds the workspace, and an
orphaned debugger holds its debuggee along with its stop.

The language server ends itself too. At exit nora sends LSP `shutdown`, waits
for its answer, then sends `exit` and closes stdin, and once the server has
closed its stdout nora waits for it to exit by itself before reaping it. gopls answers `shutdown` only after it has cancelled the go
commands its workspace snapshots run and reaped them (on Thylacine the interrupt
note gopls posts ends a `go` at once), so a `go list` in flight is ended by gopls
instead of orphaned. The answer can be slow: gopls handles messages in order,
and `initialized` holds the queue until the first workspace load has finished,
so a quit 0.4 s after `initialized` was answered only after 13.2 s on TCG. Most
quits take one to three seconds. If no answer has come five seconds after the
request, nora closes the server's stdin instead. On that EOF gopls closes its
stdout first, then shuts its session down (cancelling the load and reaping its
go commands), then exits: so the EOF alone is never taken as the end, and nora
waits for the exit. Thirty seconds after the request a server that has still
not exited is killed. The answer and stdout waits also end on a timed poll that
waits out its rest with nothing ready, or on a long run of polls across which
the clock reads no time; the exit wait (stdin closed first) counts naps, so a
clock that does not advance still ends it. A failed poll is retried after a
nap: it cannot be a note's EINTR, which poll does not deliver to a native
Proc. Server requests that arrive
meanwhile are answered, stderr leaves the poll at its EOF, and the status line
reads `ending gopls ...`. nora then prints `nora: lsp end=<how> ms=<n>` beside its
`nora: EXIT` line: `goodbye`, `disconnect`, `eof` (it left unasked), `grace` (the
kill), `broken` or `dead`. Before 2026-10-28 nora sent the goodbye and the exit
back to back and killed gopls at once, which orphaned its running `go` to init.
`nora-demo.exp` now requires `goodbye` (within 7 s: an answer later than the
escalation cannot be one) or `disconnect` after each of five quits, and fails on
the kernel's `proc: orphan ... (parent ... name="gopls"` line. After a disconnect
gopls took up to 7.6 s on TCG to finish its own shutdown and exit. And
`lsp-probe` requires every baked server to answer `shutdown` and exit by itself
with status 0 on every boot ([[sub-parley]]). The thirty seconds were sized on
gopls. clangd answers at once, but its exit takes about as long as its session
did: 11.2 s with headers on thyla-pi, and under TCG, where the session alone
runs 17-47 s, a quit with a C++ buffer open can end clangd by `grace`. That
costs the wait, not a leak: nora runs clangd bare, without `--query-driver`, so
it has no child to orphan (OPEN-BUGS 2026-10-29T01:53Z).

The debugger is ended differently, because killing it first strands its
debuggee. `:kill`, a `:debug` over a live session, and quitting the editor all
send DAP `disconnect` with `terminateDebuggee`. Delve answers only after it has
killed the debuggee and reaped it, then exits, and nora reaps Ambush at its
stdout EOF; the status reads "debug session ended" only on that path. `:kill` and
`:debug` never block on it (NORA-IDE-UX section 6): the session is marked ending,
its descriptors stay in the poll, every other debug verb answers "debug session
ending", and a `:debug` waits for the old session to be reaped before it starts
the new one. A quit has no later wake to finish on, so it waits for the same end
in a bounded poll of its own. Either way the bound is two seconds from the
disconnect. Past it nora kills Ambush, and the kernel's die-with-launcher rule
(EXITKILL, I-39) ends the debuggee, so on that path the console still shows the
debuggee's orphan line: it is the fallback for a debugger that never answers. The
deadline does not trust this process's clock alone: a poll that waited out the
session's remaining time with nothing ready has spent the grace by the kernel's
timer, so a clock read that never advances cannot make the bound disappear;
at exit, a long run of polls across which the clock reads no time counts the
same, so a debugger that keeps writing cannot hold the exit open. Before 2026-10-28 every one of these
paths killed Ambush straight after the disconnect, and the kernel ended the
debuggee each time; `dap-nora.exp`'s three teardown legs now fail on the kernel's
`proc: orphan ... name="ambush-child"` line.

Starting a server is entirely optional. An absent binary, an unclaimed file
suffix, or a confined namespace all mean "no language server", which is a fully
supported state in which the editor behaves exactly as it did before servers
existed. The debugger is not started at all until the user asks for one.

### Adding a language is a table row

A server is described by four facts: the suffixes it claims, the protocol name
for them, its binary, and its workspace-root markers. The dispatch reads the
table, so the protocol layer needed no changes to gain a second language. The
comment states the corollary as a rule: anything a new language needs beyond
those four facts is a defect in this abstraction rather than a reason to fork it.

Suffix shadowing is addressed explicitly — a suffix match is not a prefix match,
so a shorter extension cannot swallow a longer one, which makes the order within
a row cosmetic while the order *across* rows decides who claims a contested
suffix.

### Format-on-save pipes rather than rewrites

Saving a Go file pipes the buffer through the formatter — write all of stdin,
close, read all of stdout — so one durable write lands formatted bytes and
unformatted content never reaches disk. There is no temporary file and no
write-then-reload.

Three properties make it safe. Deadlock-freedom is argued from the formatter's
own implementation (it reads all of stdin before emitting anything) plus a stderr
cap chosen to sit under the kernel's pipe buffer, so draining stdout before
stderr cannot wedge. All three of the child's stdio slots are pipes, so it can
never paint on nora's alt-screen or steal a keystroke. And the result is guarded:
an empty output for non-empty input, or non-UTF-8, is discarded rather than
adopted, so a pathological formatter cannot clobber the buffer. **A formatter
never blocks a save** — a rejected parse saves the buffer as-is and reports the
first diagnostic line.

### Coordinates convert twice, in both directions

The protocol speaks offsets in a negotiated encoding; nora speaks character
columns. Every crossing is two steps. Outbound, a character column becomes a
byte offset and then the count the server negotiated. Inbound, the server's
offset becomes a byte offset against the real line text and then a character
column. All four crossing sites do both steps.

Two cases are handled by *declining* rather than converting. A diagnostic whose
line is past the end of the buffer is dropped rather than clamped to the last
line, "where it would mark innocent code" — the server being a version behind
the user's edits is normal, and a clamped diagnostic marks the wrong code
confidently. A jump into a file not yet read has no line text to convert
against, so the server's offset is used as a character column directly, with the
error bound stated: exact on an ASCII line, a few columns off on a line with
multi-byte characters before the symbol, and the line is always right.

### The kernel half of the stack is an I-39-authorized `/proc` read

On each stop the debug host reads the debuggee's kernel backtrace from
`/proc/<pid>/kstack` — the 8b settled-thread inspect — and hands the text to the
pure parser ([[sub-nora-view]] owns `parse_kstack`; the divider and dimmed
`StackRow.kernel` rendering are its half too). This is the one place the layer
touches a kernel object, and its authorization is worth stating plainly because
the Invariants section otherwise reads as "no kernel surface at all":

- **The read rides the I-39 owner axis, not a capability.** nora, Ambush and the
  debuggee all run as the same login principal, so the debug-authority invariant
  (I-39) authorizes the inspect through ownership rather than `CAP_DEBUG` —
  exactly the settled-thread tier that is I-39-authorized but not debug-stop-gated
  ([[inv-i39]]). `/proc/<pid>` is reachable in nora's namespace because Ambush,
  which nora spawned, already opens the same file; nora mounts nothing new for it.
- **It is best-effort and fail-open.** No debuggee pid, an unreachable or denied
  `/proc`, or an unparseable read all leave the kernel half empty — the Go frames
  render alone. The read never fails a stop, never hangs, and never fabricates a
  row. The pid arrives out of band: parley decodes the DAP `process` event into
  the debuggee's system pid, so a backend that does not emit one simply yields no
  kernel frames.
- **The frame it reads is the target's focus thread** (the M at the stop, else
  main, the oldest thread not exiting), not the
  stopped goroutine's own M — the goroutine-accurate kernel stack is a deferred
  Ambush stitch. At a user breakpoint the kernel rows are the debug-trap path; a
  goroutine blocked in a syscall shows its block path. Selecting a kernel frame
  reports `kernel frame: <name> (no source)` because kernel DWARF is deferred.

## Data structures

- **`Lsp` / `Dap`** — the two session objects. Each owns a child process handle,
  a pure protocol client, a dead flag, and whatever session state its protocol
  needs. Both are `Option` in the loop: absent is the normal state.
- **`ServerSpec`** — the language table row.
- **`Dap`'s session state** — the current phase, thread and frame, the cached
  stack, the variable tree, the goroutine list, a bounded console scrollback, the
  breakpoint lists, and the debuggee's pid (used to read the kernel half of the
  unified stack). While it ends: when the disconnect was sent, whether Delve
  answered it, and a `:debug` program waiting to start.
- No shared state, no globals beyond the allocator.

## Concurrency

Single-threaded and single-process-per-child. There are no locks. The only
asynchrony is the poll loop, and every wake is handled to completion before the
next.

The children are separate processes; the only shared objects are pipes, and each
is read by exactly one side.

## Invariants enforced

**I-27** (trusted path) is *composed*, not enforced: the layer's contribution is
an abstention. It never opens the console control file, so it can never change
the line discipline out from under the shell, and it never becomes
console-attached, so the elevation gate is untouched. Verified rather than
assumed — the control file's name appears exactly once in the whole crate, in
the comment saying nora does not touch it.

**I-39** (debug authority) is *consumed*, not enforced: the kernel-stack read
above inspects `/proc/<pid>/kstack` through the owner axis (the same login
principal), the settled-thread tier that I-39 authorizes without a debug-stop.
The kernel does the enforcing; this layer only exercises the authorization it
already holds by identity.

Nothing else from the enumerated set. No capability and no namespace mutation.

## Error paths

Failures are reported and survived, in a consistent order of preference: report
to the status line, degrade the feature, exit cleanly, and never crash.

A file that cannot be read before the screen is acquired prints to the cooked
console and exits, so the message is visible; after the screen is up, the same
failure becomes a status line. A missing file is not an error — it is a new
buffer, created on save. A server that cannot start is a status line and a
supported state. A broken pipe reaps the session (kill, then wait) before the
session is dropped. Losing stdin exits cleanly rather than spinning.

Saves are durable: create-truncate, write, then sync. A server that rejects the
sync is tolerated, since the bytes are already written and the barrier is
best-effort.

**Every exit path names itself (s7 F3).** The six ways the editor leaves each
emit one `nora: EXIT path=<name> code=<n> [err=<e>]` line via `t_putstr` before
returning: `redraw1`/`redraw2` (an initial or steady-state repaint failed,
code 1), `eof` (stdin closed, code 0), `pollnone`/`pollerr` (the one poll
returned nothing or an error, code 1), and `quit` (`:q`, code 0). They are the
discriminating witness the s7 gate keys on — "one frame then exit" vs a clean
hold are two different `path=` values, not one anonymous exit. `t_putstr` is a
direct console write and is **not** line-serialized against other console
writers (the [[seam-extinction-line-unserialized]] / #243 class), so a marker
can interleave with concurrent output; acceptable for a diagnostic that only
has to be greppable on the serial log, never rendered.

## Performance

Not a measured surface, and deliberately event-driven — the loop does nothing
between wakes. Two per-frame costs are bounded on purpose: the debug console
scrollback is capped and drains from the front, and a debuggee's output line is
clipped before it is stored.

Document synchronization fires on save and on leaving insert mode, never per
keystroke, because a full-document update per keypress is a byte storm — and
nora renders inside a row-granular framebuffer console, where emitted bytes cost
twice.

## Prosecution

- **The abstention is the invariant.** Any future need to change the line
  discipline from inside nora breaks the division of labour with the shell and
  reopens the trusted-path question. The shell owns the discipline; nora owns the
  screen.
- **A new child must be shut down on every exit path**, with an unconditional
  kill and wait. The stated hazard is real: an orphan holds a resource the user
  cannot see. For a child that holds another process, as the debugger holds its
  debuggee and gopls its go commands, the kill is the bounded fallback after the
  child's own end, never the first step: a kill first orphans the held process.
- **A new poll source must join the one descriptor set.** There is no tick to
  fall back on, so a source that is not polled produces a message that never
  repaints.
- **A new protocol crossing must convert twice**, in both directions. One step
  is invisible on ASCII.
- **A new child's output must be sanitized before it can reach the screen** —
  see Caveats; today it is not, and this is the layer where it enters.
- **A formatter must never block a save.** The current shape degrades on every
  failure mode; a future one that refuses to save on a formatter error inverts
  the priority.

## Seams

- **Live resize on a serial console** depends on the round-trip backstop rather
  than a signal, because there is no window-size notification over a UART.
- **The debugger's program and error streams are unified** into one scrollback.
- **A jump into an unread file approximates the column** (above).
- **The editor under a pseudoterminal would need to re-honour hangup.** Opening
  the note queue makes nora self-managing, which queues hangup and quit instead
  of terminating. That is sound on the console, where no hangup is posted — but a
  future nora under a pts must tear down on one, and the code says so.

## Caveats

- **This layer has no tests, and that is the design rather than an omission.**
  All 238 of nora's host tests live in the other nine files; these three have
  none, verified by count. The split is deliberate and stated in the first
  paragraph of the crate: everything protocol-shaped is in the client library,
  everything terminal-shaped is here, and the engine in between is host-tested.
  The consequence is worth naming anyway — the file that owns the console, spawns
  children and performs every write is the one covered only by the interactive
  end-to-end scenario, so a regression here is caught by a booted VM or not at
  all.

- **The formatter's safety argument rests on a premise the same file inverted.**
  The comment closes by explaining that a broken-pipe note from a died-early
  formatter is dropped by the kernel "since nora is not self-managing (no notes
  fd)". nora opens a note queue at startup, and the comment eleven lines above
  that call states in capitals that doing so makes it self-managing — the change
  arrived so the loop could wake on a console resize. The note is now queued and
  then discarded by the queue's own drain arm, so nothing misbehaves; what is
  wrong is the reasoning, and this comment is a safety argument rather than a
  description. The next author extending the pipe handling inherits a premise
  that no longer describes the program. Task #133.

- **The diagnostic conversion's rustdoc says "byte columns" and its body says
  character columns, four lines apart.** The same stale sentence appears in the
  display model's header, which makes it a pattern from one era rather than a
  slip: in both places the correcting prose sits immediately beneath the wrong
  claim, and in both the code is right. Task #131.

- **A failed cross-file jump leaves the pending jump armed.** The open request
  disarms it inside the buffer-open call, which every path reaches except one —
  the hard read-error arm reports and returns early — so the next successful open
  of any file inherits a cursor target meant for a different one. Confirmed here
  from the consuming side; the mechanism is in the engine. Task #124.

- **The debuggee's output is clipped for length and not for content.** Each
  output line is trimmed and truncated to a fixed character count before it is
  stored in the scrollback, which bounds memory but not what the characters *are*
  — and the scrollback is rendered through a widget that emits characters
  verbatim. This is the concrete producer end of the sanitization gap: a program
  under the debugger printing an escape sequence has it delivered to the terminal
  intact. This layer is where such text enters, so it is the natural place to
  sanitize even though the omission is shared with the renderer. Task #130.

- **A server that closes its stderr while it runs would spin the poll.** The
  steady-state handlers ignore stderr's EOF, and a closed pipe polls readable
  for good; gopls, clangd and Ambush keep fd 2 open, so it is latent. The exit
  goodbye already drops stderr at EOF. Queued: OPEN-BUGS 2026-10-28T11:42Z.
- **gopls can still orphan a `go` that is not nora's doing.** Its imports refresh,
  scheduled thirty seconds or more after the view is created, runs `go list -m`
  that gopls's shutdown does not wait for; and a disconnect that lands while
  `initialized` still runs `go env` lets gopls exit around it. Both are gopls-side.
  Queued: OPEN-BUGS 2026-10-28T12:22Z (two entries).
- **A write to a server blocks.** A full-document sync to a server that has
  stopped reading blocks the loop once the pipe is full, against NORA-IDE-UX
  section 6. Queued: OPEN-BUGS 2026-10-28T11:43Z.

## Provenance

[[chg-2026-08-03-nora-host-sweep]] · [[chg-2026-09-06-s7a-palette-destale]]
(the `adopt_session_palette` startup adoption + [[abi-halcyon-palette]]) ·
[[chg-2026-09-07-s7-f3-dossier-deltas]] (the self-naming exit markers, the F3
discriminating witness).
