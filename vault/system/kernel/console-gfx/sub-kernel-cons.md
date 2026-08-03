---
id: sub-kernel-cons
type: sub
parent: moc-kernel-console-gfx
title: "The console — four rings, three roles, and an interrupt that may not do the work"
code:
  - kernel/cons.c
  - kernel/include/thylacine/cons.h
audit: hard
guarded-by: [inv-i27, inv-i9]
validated-by: [spec-cons-poll, prose, gate-interactive, gate-smp]
locks: [lock-cons, lock-cons-tx, lock-poll-list, lock-rendez, lock-proc-table]
abis: []
design:
  - "docs/ARCHITECTURE.md section 23.5.1 (pollable console + line discipline)"
  - "docs/ARCHITECTURE.md section 23.5.2 (console write atomicity)"
  - "docs/ARCHITECTURE.md section 23.5.3 (the console winsize)"
  - "docs/IDENTITY-DESIGN.md section 9.8 (the trusted path)"
  - "docs/TRUSTED-PATH.md"
  - "docs/TAPESTRY.md section 18.7 (the renderer drain/feed)"
  - "docs/LIFE-SUPPORT.md LS-8"
created: 2026-08-02
updated: 2026-08-03
---
## Purpose

The kernel console: one physical UART presented as `/dev/cons`, with a line
discipline, a control file, a window size, and — since the compositor landed —
a mirror of its output and an injection point for its input.

It is the machine's original and last-resort interface. It is also the trusted
path ([[inv-i27]]), which is why a device that would otherwise be a hundred
lines is sixteen hundred.

## The organizing fact

**The console's producer is an interrupt handler that is not allowed to do any
of the work.**

A received byte arrives in interrupt context. Almost everything the console
wants to do with it is illegal there:

| Wanted | Why it cannot run in the handler |
|---|---|
| wake a poller | [[lock-poll-list]] is non-irqsave and nests a wake |
| post the interrupt note | needs [[lock-proc-table]] |
| perform the attention-key transition | same |
| wait for room to echo | interrupt context may not sleep |

So every one of them is *deferred*: the handler mutates state under
[[lock-cons]], sets a flag, and calls `wakeup` — the single interrupt-safe wake
primitive — and a boot-spawned **manager kthread** performs the real work in
process context. That relay is the console's signature shape, it occurs four
times, and it is the one part of the console with a model
([[spec-cons-poll]]).

Every remaining oddity in the file is the same fact seen from another angle:
the transmit ring exists because echo may not block; the diagnostic emitters
exist because a bounded spin in the handler is still a spin; the locks are
leaves because a leaf is what an interrupt handler can take.

## Contract

Two front doors, **one implementation**. A syscall mints a console fd directly;
a namespace path walks to `/dev/cons` ([[sub-kernel-devdev]]). Both call the
same read and write entry points, which is what makes the single-reader guard
total — there is no second reader path that could race the first.

- **read** — blocks until at least one byte, returns what is buffered. A second
  concurrent reader gets `-1` rather than parking (below).
- **write** — accepts the whole buffer, or fewer bytes if a stalled consumer or
  a dying thread cuts it short. Never blocks forever.
- **poll** — readable when the ring is non-empty; **always** writable, since the
  transmit path never reports back-pressure to a poller.
- **fstat** — reports a character device carrying the is-a-console qid marker.

## Mechanism

### Four rings

| Ring | Direction | Size | Full behaviour |
|---|---|---|---|
| receive | in | 512 | back-pressures **both** producers (below) |
| canonical line | in | 256 chars | a byte past the line limit drops; a full ring back-pressures |
| transmit | out | 8192 | writer waits; echo drops |
| drain | out | 8192 | drops **oldest** |

The receive ring is sized to hold one worst-case cooked flush — the line limit
plus its terminator — and a static assertion ties the two together, because the
obvious per-byte admission check would have refused a maximal line forever and
traded a bounded drop for an unbounded wedge.

The drain's disposition is the interesting one: it drops the *oldest*, not the
newest, so the prompt the user is waiting on survives a burst. Input never drops
for want of room: it refuses, and tells the producer so.

### The line discipline

Five independent flags — canonical mode, echo, signal generation, and the two
newline translations. The boot default is **signal generation only**: byte at a
time, interrupt cooked, nothing echoed, nothing translated. That is exactly the
behaviour that existed before the discipline landed, so the mechanism is inert
until a consumer opts in.

Cooking order matters and is fixed: carriage-return translation happens
*first*, so that Enter-as-CR both terminates a canonical line and echoes as a
newline; then signal generation; then canonical assembly or the raw path.

**Echo-off is a hard guarantee**: when the flag is clear, no input byte reaches
console output on *any* path — not the raw path, not the canonical path, and
not the erase redraw. That is the password mask, and it is why the erase
sequence is emitted under the flag rather than unconditionally. It also holds
for the renderer, since the drain is fed from the same emit point: nothing
echoed means nothing mirrored.

Erase on an empty line emits nothing, so a backspace at a prompt can never walk
back over the prompt itself.

### The transmit ring and the writer role

Before this existed, a console write looped byte-by-byte into a lock-free UART
put, so two CPUs writing the console interleaved **at byte granularity** —
shredding a multi-byte glyph or an escape sequence. Two separable mechanisms
fix it, and conflating them is the standing hazard:

- The **ring** decouples the writer from the UART's bounded-but-slow full-FIFO
  spin. In the healthy case the post-push kick moves the bytes straight into a
  non-full FIFO and the transmit interrupt is never even armed.
- The **role** makes a whole write call atomic against other writers. It is
  *required* because a write larger than the ring must sleep for room, dropping
  the ring lock — so the ring lock can never span the call. See
  [[lock-cons-tx]].

The role is a sleeping park, not a spinlock: a contending writer parks, so a
long write makes peers wait without pinning a CPU.

A write returns **short** on either a stalled consumer (the room wait is
deadlined) or a dying thread. Short is POSIX-legal and is the inherited
disposition: a bounded-and-lossy console beats a wedged writer. It must never
become a hang.

### Diagnostics take the ring too

The direct UART emitters spin on a full FIFO for a bounded time *per byte* —
and that bound does not compose. Ninety bytes of diagnostic emitted
back-to-back under the global process-table lock is seconds of interrupt-dead
time with that lock held, which is precisely the stall the per-byte bound was
introduced to prevent.

So kernel diagnostics have their own emitters that push into the transmit ring:
never spinning, dropping on a full ring, kicking the FIFO once per call. They
take only leaf locks and wake outside them, which makes them legal from
interrupt context and from under any lock ordered above — the same path echo
already takes from the most constrained context in the kernel.

They deliberately do **not** consult the echo capture buffer, which exists so a
test can assert exactly what was *echoed*; a diagnostic landing in it would
corrupt the assertions it exists for.

### Receive back-pressure

**Both** producers are refused rather than silently dropped, and the admission
answer is returned to them. The serial drain **stops reading the FIFO** and masks
receive, leaving the bytes in hardware; the graphical renderer's keyboard feed
returns a **short count**. The reader resumes the drain after freeing space.
Masking rather than clearing is what makes resumption reader-driven and
idempotent.

Three properties make the refusal honest rather than a narrower drop:

- **The room check is per-operation, not per-byte.** A canonical Enter pushes the
  whole assembled line plus its terminator in one call, so the check reserves
  the whole flush under the console lock before pushing any of it. A per-byte
  check would push until the ring filled and count the remainder — dropping the
  *tail*, terminator included, so the line silently became a different, shorter,
  unterminated line.
- **A refusal changes nothing.** The line stays assembled, the terminator is not
  consumed, and nothing is echoed — the echo moved *inside* the accepted branch,
  because echoing a refused byte shows the user a character the console did not
  take and then shows it twice when the producer re-offers it.
- **A one-byte holdback covers the pre-check race.** The serial drain's room
  check is lockless, so a peer producer can take the room before the under-lock
  push; by then the byte is out of the data register and cannot be put back,
  which is the one way the leave-it-in-the-FIFO guarantee could be escaped. The
  drain parks that byte and re-offers it before touching the FIFO again. Without
  it the fix would merely narrow the loss window rather than close it — and the
  four properties of the holdback all fail *silently*, which is why it is seeded
  and inspected directly by a test rather than left to the counters.

The counters distinguish the two dispositions by name: back-pressure (a raw byte
or a cooked flush refused for room) is not loss and does not arm the report,
while a real drop is a byte past the line limit. A third counter exists to stay
**zero** — a push that fails after the room check succeeded would mean the two
disagree, so it is an invariant witness rather than a statistic.

**A full ring never suppresses the trusted path.** A serial BREAK is recognized
before any admission logic and ungated by the mode flags, because it is a line
condition rather than a data byte; the secure-attention trigger cannot be starved
by filling the ring.

The acyclicity argument is explicit and fragile enough to be worth restating:
the UART receive lock orders *before* [[lock-cons]], and the reader **releases
the console lock before calling the pump** — so no reverse edge exists. That
release is load-bearing, not stylistic.

### The interactive-band promotion

A trusted console reader is promoted to the interactive scheduling band, so its
wake preempts normal work and a keystroke's echo paints promptly. The gate is
**deliberately narrow**: it requires the reader to be console-attached or the
console owner. An ungated promotion would let any unprivileged program that
reads its stdin self-promote above normal work and starve it, since the console
is inherited as stdin by foreground children and the band has no aging.

A band pre-check keeps the (locking) ownership query off the path once a reader
is already promoted, and bounds it to typing frequency for a reader that stays
normal.

The renderer's drain reader gets the same promotion for the same reason — it
*is* the display — and there the gate is structural, since only the bound
renderer can reach that read.

### The control file and the window size

The control surface is a **file with a text grammar**, not an ioctl — the Plan 9
idiom. Whitespace-separated `+name`/`-name` tokens, plus a `winsize` verb.

**The whole write is atomic**: every token is parsed before any is applied, so a
single malformed token rejects the write and leaves the mode unchanged. That is
the guarantee a terminal-attribute API needs, and it is why the parse and the
apply are separate passes.

A mode change **discards any half-assembled canonical line**, so a
canonical→raw→canonical flip cannot strand a fragment that then prepends the
next line. No current consumer flips mid-line, but the kernel is unambiguous
against any writer — and, importantly, the production path and the test hook do
the same thing here, so a fragment-survival regression would be caught.

The window size posts its change note **iff the size actually changed**. An
unchanged rewrite must not post: a repeat-post storm would be a notes-queue
denial of service against the owner's process group. The post happens after the
console lock drops.

### The renderer drain and feed

The output tap sits in the single emit point both program output and echo
already cross, so the renderer sees exactly the byte stream a terminal would
display. On serial-bearing media it is a **mirror** — the UART path continues
byte-identical, so the host terminal, the boot-banner contract and the serial
trusted path all keep working.

Writers never block on the renderer. A stalled or dead renderer must not wedge
every console write, which is why the drain drops rather than back-pressures.

The feed injects the renderer's decoded keystrokes into the *existing* line
discipline — cooking, echo, signal generation, canonical assembly, all
unchanged and backend-independent. Its one hard property is [[inv-i27]]'s: the
line-condition parameter is **hardwired false**, so no feed byte sequence can
synthesize the attention key.

A fresh drain open starts a fresh **epoch**, discarding the previous holder's
bytes so a new renderer never paints a dead one's tail.

## Data structures

Four file-scope statics: the input state, the drain, the transmit ring, and the
capture buffer. Three Rendez (data, manager, drain) plus the transmit room
Rendez, all statics.

**The hook lists are file-scope statics, and that is a real property, not an
accident**: the registered-object-lifetime hazard — a sibling freeing an
embedded list while a poller sleeps on it — structurally cannot arise here,
because the lists are immortal. Every other pollable object in the tree has to
argue this; the console does not.

Three Rendez are **single-waiter** (data, drain, room), each with its own
argument: the two readers by an explicit busy guard, the room by the writer
role's exclusivity. See [[haz-single-waiter-rendez]].

## Concurrency

[[lock-cons]] and [[lock-cons-tx]] carry the full discipline; the drain lock is
[[lock-cons]]'s shape applied to the output ring.

The console-wide rule: **only `wakeup` from interrupt context**. Every other
wake — the poll-hook walks, the note posts — is relayed to the manager thread.

The single-reader guard is a flag, not a Rendez: a second concurrent blocking
read returns `-1` rather than parking, because the data Rendez is single-waiter
and a second sleeper is an extinction. Refusing is strictly better than racing
into that.

## Invariants enforced

**[[inv-i27]]** — the trusted path. This file owns the recognizer (stateless,
unconditional of the mode flags, a line condition rather than data), the
hardwired-false feed parameter, and the deferral that lets the transition run
where it can take the process-table lock.

**[[inv-i9]]** — no lost wake, across the deferral. Modelled by
[[spec-cons-poll]]; the mechanism is register-then-observe at *two* levels, the
poller's and the manager's.

## Error paths

`-1` for bad arguments, for a second concurrent reader on either input ring,
for a second drain open, and for a malformed control write. A **short** count
from a write on a deadline or a death. `0` from a read only on a death with
nothing buffered, or from the drain when disarmed and empty (end of file).

No errno distinction anywhere — the console predates the errno surface and has
not been converted.

## Performance

Per output byte the ring trades an MMIO status read plus a data write for a
spinlock plus a memory store — a win, and especially so under hardware
virtualization where each MMIO is an exit.

The echo path kicks the FIFO immediately rather than batching, because typing
latency is user-visible and an echo is at most a few bytes.

Two diagnostic counters are worth watching together: **room waits** counts
writers that back-pressured on a slow consumer, **dropped** counts the ones that
gave up.

## Prosecution

- **Nothing that needs [[lock-proc-table]] or a hook-list walk may be called
  under [[lock-cons]].** This is the whole deferred design; a new deferred
  action joins the flag set, it does not shortcut.
- **A new interrupt-context readiness source must relay.** Do not widen
  [[lock-poll-list]] to irqsave for a console-only need.
- **The two transmit producers keep opposite blocking contracts.** The write
  path may sleep for room; echo must never. Blurring them puts a sleep in an
  interrupt handler or a drop in a program's output.
- **The room Rendez stays single-waiter, or becomes a list.** Its soundness is
  the writer role's exclusivity, not an accident of the current callers.
- **The room wait stays deadlined**, and a write may stay short. Removing either
  converts a lossy console into a wedged one.
- **The ring drain and the transmit-interrupt re-evaluation stay in one critical
  section**, or a non-empty ring can be left with interrupts off — a silent
  console wedge.
- **Echo-off must remain total.** Any new echo site is gated on the flag, or the
  password mask leaks — to the serial line *and* to the renderer's drain.
- **The attention key stays unconditional of the mode flags.** It is a line
  condition; gating it behind a termios bit would make the trusted path
  configurable.
- **The feed's line-condition parameter stays hardwired false.**
- **The control write stays parse-all-then-apply**, and a mode change keeps
  discarding the partial line.
- **The window-size post stays iff-changed.**
- **The reader must keep releasing [[lock-cons]] before pumping the UART**, or
  the receive lock order becomes cyclic.
- **The interactive promotion stays gated on the trusted console roles.**
  Ungating it is a starvation vector from unprivileged code.
- **A new field read from a sleep condition must be a relaxed atomic.**

## Seams

- **A single reader.** The console is one reader at a time by construction. A
  multi-reader lift means replacing two single-waiter Rendez with wait lists.
- **A dedicated revocation note.** The attention key currently signals the
  displaced owner only by removing its attach bit, because the note it used to
  reuse became a real terminating signal. See [[inv-i27]].
- **Per-fd terminal attributes.** The console carries one global termios word;
  per-fd belongs to the pseudoterminal surface.
- **The exclusive board-era output switch.** On a display-only board the serial
  side should be suppressed rather than mirrored; the tap composes with that
  (the selector will gate the UART emit, not the tap).
- **Errno.** Every failure is `-1`.

## Caveats

- **The mode-line render's documented minimum buffer is wrong, and provably
  so.** The header contract says the caller needs at least 34 bytes. The window
  size verb appended a tail whose bound check reserves the worst case
  unconditionally, so the true minimum is **54** — and a caller that allocates
  exactly the documented 34 gets zero bytes back, every time, forever. It is
  fail-safe (the renderer returns nothing rather than truncating, and the one
  production caller passes a comfortable buffer), so nothing is broken today.
  What makes it worth recording is that the *test already encodes the correct
  behaviour* — it asserts that a 40-byte buffer yields zero — so the header and
  the test suite state contradictory contracts, and only the header is what a
  new caller reads. The standalone window-size render has the same drift one
  byte wide: its header says at most 21 bytes, its own bound check and comment
  say exactly 20.
- **The echo capture buffer is not lock-protected.** It is test-only state, and
  the discipline — never enable capture while a live receive interrupt could
  reach the emit path on another CPU — is a rule rather than a mechanism. Sound
  today because capture is strictly single-processor test-time.
- **The manager hold is a test hook that changes a wake condition.** It makes a
  held manager's condition read false so it re-parks *without consuming* pending
  flags, which is what makes the deferred-wake tests deterministic on multiple
  CPUs. It is inert in production, but it is a test hook inside the modelled
  relay, which is a place to be careful.
- **The file's own opening comment is thoroughly stale.** It describes a
  write-only console whose reads return end-of-file and whose control file is
  "held until a later phase" — the state of the world several arcs ago. Every
  one of those has since landed. The per-section comments are current and
  excellent; only the header block is archaeology.
- **The `.wstat` slot refuses unconditionally**, so the reported mode is
  immutable — which is consistent, since the Dev does not enforce it either.
  As with the introspection Devs, the mode bits are documentation and the gate
  is the check at the call site ([[sub-kernel-devdev]]).

## Provenance

[[chg-2026-08-02-console-sweep]].
