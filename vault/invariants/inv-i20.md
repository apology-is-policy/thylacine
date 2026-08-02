---
id: inv-i20
type: inv
title: "I-20 — a pseudoterminal neither loses a byte nor turns one into two things"
number: I-20
guards: [sub-ptyfs, sub-kernel-proc]
validated-by: [spec-pty, spec-pty-stop, prose, gate-smp]
strength: spec
created: 2026-08-02
updated: 2026-08-02
---
## Statement

A pty is two byte rings and a line discipline between them, and the
invariant is that the discipline is **exact**. Four clauses:

- **Byte conservation.** A byte written to one end reaches the other, in
  order, once. Nothing is torn, duplicated, or silently swallowed by the
  cook.
- **Signal XOR byte.** An input character that raises a signal is
  *consumed* by raising it: it never also reaches the slave, and is
  never echoed. Conversely, a character that becomes data raises
  nothing.
- **The foreground group, and only it.** A cooked signal reaches exactly
  the controlling terminal's foreground process group. The server that
  cooked it cannot name any group at all.
- **Drain, then EOF; hang up once.** A closing endpoint leaves the
  queued bytes readable — the peer drains what is there and *then* sees
  end-of-file, never end-of-file first. A master's departure is carrier
  loss, raised exactly once per pts lifetime.

The stop that a suspend character produces is I-20's entry into a park
that belongs to [[inv-i39]] — and the composition, two independent
owners of one park, is [[spec-pty-stop]]'s.

## Enforcement

Split across the boundary on purpose. The **userspace** [[sub-ptyfs]]
owns the rings and the discipline; the **kernel** owns sessions, process
groups, the controlling terminal, and the note routing.

**The third clause is structural, not checked.** ptyfs's only signal
power is a syscall taking `(pts_id, class)` — there is no process-group
parameter to get wrong. The kernel resolves the pts to its controlling
session and thence to the foreground group. A server cannot escape its
own pts because it was never given a way to name anything outside it.
That is the I-1/I-22 bound realized as an absent argument.

**Signal XOR byte** is one `continue` in the input cook, and the classes
it collects are a three-bit *set* rather than a queue — so repeats dedup
and a distinct class can never be lost behind a same-class run.

**Echo passes one chokepoint**, gated at the top. With echo cleared,
nothing reaches the master — not the typed byte, not the erase redraw.
That is the password mask, and a second echo path would silently undo
it.

**Drain-then-EOF** is a property of the drain, not of the close: the
ring returns data while it is non-empty *regardless* of whether the peer
has closed, and end-of-file only on an empty ring. Nothing needs to
happen at close time for queued bytes to survive it.

**Hang up once** is not a counter. It is a four-link structural chain:
masters are minted by the clone open and never resolved by a walk; 9P
forbids walking from an opened fid; a walk to an already-bound fid is
rejected; therefore at most one master fd per pts can ever exist, and
the last-master edge fires at most once. The third link is a two-line
protocol check carrying a safety property — worth knowing before
deleting it as hygiene.

## Validation

[[spec-pty]] for the data path — `SignalXorByte`, `RingConserved`,
`SignalToFgOnly` and `HupAtMostOnce`, each with a buggy cfg that
violates exactly it. [[spec-pty-stop]] for the stop composition. The
in-server selftest drives the whole discipline truth table before the
listener posts, and gates the boot on it; the `pty-probe` E2E drives a
live controlling session end to end.

**blind-to:** the model's conservation clause covers the *raw* arm's
back-pressure, not the *cooked* arm's deliberate overrun drops — a byte
past the line bound, and a line flushed into a full ring, are consumed
and discarded, which is the classic tty semantic and outside what the
model represents. See [[spec-pty]] and task #48. Also beneath it: the
character transforms themselves, the ctl grammar's atomicity, and the
fid refcount discipline that keeps a pts alive.
