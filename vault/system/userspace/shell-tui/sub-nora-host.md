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
abis: []
design: []
created: 2026-08-03
updated: 2026-08-03
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

A dead server contributes an empty descriptor list, so its fds are never polled
again — safe by construction rather than by bookkeeping, because the set is
rebuilt every round rather than mutated.

### Neither child may outlive the editor

Both hosts carry the same lifecycle: a dead flag, a reaper that kills and waits
once, and an orderly shutdown that sends the protocol's goodbye, closes stdin so
the child sees EOF, then kills and waits **unconditionally**. The reason is
stated rather than assumed — an orphaned language server holds the workspace, and
an orphaned debugger holds its debuggee along with its stop.

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

## Data structures

- **`Lsp` / `Dap`** — the two session objects. Each owns a child process handle,
  a pure protocol client, a dead flag, and whatever session state its protocol
  needs. Both are `Option` in the loop: absent is the normal state.
- **`ServerSpec`** — the language table row.
- **`Dap`'s session state** — the current phase, thread and frame, the cached
  stack, the variable tree, the goroutine list, a bounded console scrollback, the
  breakpoint lists, and the debuggee's pid (used to read the kernel half of the
  unified stack).
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

Nothing else from the enumerated set. No capability, no namespace mutation, no
kernel object.

## Error paths

Failures are reported and survived, in a consistent order of preference: report
to the status line, degrade the feature, exit cleanly, and never crash.

A file that cannot be read before the screen is acquired prints to the cooked
console and exits, so the message is visible; after the screen is up, the same
failure becomes a status line. A missing file is not an error — it is a new
buffer, created on save. A server that cannot start is a status line and a
supported state. A broken pipe reaps the session. Losing stdin exits cleanly
rather than spinning.

Saves are durable: create-truncate, write, then sync. A server that rejects the
sync is tolerated, since the bytes are already written and the barrier is
best-effort.

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
  cannot see.
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

## Provenance

[[chg-2026-08-03-nora-host-sweep]].
