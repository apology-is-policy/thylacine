---
id: inv-i19
type: inv
title: "I-19 — a note is delivered in order, consumed once, and kill cannot be refused"
number: I-19
guards: [sub-kernel-notes]
validated-by: [prose, gate-smp]
strength: prose
created: 2026-08-03
updated: 2026-08-03
---
## Statement

Five clauses, numbered N-1 to N-5 in the source, and each has a documented
exception — which is the honest shape of this invariant rather than a weakness
in it.

**N-1, ordering.** Notes are consumed in post order per source. *Except* that
`kill` always goes first regardless of its queue position, and *except* that a
note re-enqueued at the head after a failed user-stack push can reverse against
mask-deferred entries behind it. The second exception is a stated
performance-versus-strictness trade: strict FIFO would need a
re-enqueue-at-original-index primitive.

**N-2, consumed exactly once.** Every non-`kill` note is consumed once across
*both* delivery paths. This is the clause the whole locking design exists for.

**N-3, handler re-entrancy.** While a thread is inside a note handler, nothing
further is delivered to it. *Except* `kill`.

**N-4, `kill` is non-catchable.** A `kill` terminates the Proc at the next
return to userspace regardless of mask, handler, or in-handler state. *Except*
in the narrow window where the target is becoming multi-threaded, where it
stays queued until a thread can take it.

**N-5, fd lifecycle.** Closing a note fd does not disturb the queue or future
opens. The queue belongs to the Proc, not to any fd on it.

## Why two paths make one invariant

Notes are **fd-shaped first**: a Proc opens a note file and reads fixed-size
records, so asynchronous events arrive in the same event loop as everything
else — no async-signal-safety discipline, no handler-reentrancy hazards for
programs that never register a handler.

The async-handler path exists for POSIX compatibility, and it is the one that
rewrites a thread's user context mid-flight.

Both consume the same per-Proc queue, and **the queue lock is the serializer**:
whichever path takes it first wins the note. That is the entire mutual-exclusion
argument for N-2, and it is why the peek and the pop must happen under one
uninterrupted hold — a peek that releases before its pop lets the other path
take the same note.

## The asymmetry that makes kill work

`kill` is scanned for **before** the mask on the handler path, and skipped
**entirely** on the fd path. So a `kill` can neither be deferred by a mask, nor
consumed by a program reading its own note fd, nor blocked by a handler already
running — including a handler that is stuck. Each of those was a real hole
closed by a named audit round.

A consequence worth stating: `NOTE_BIT_KILL` exists in the mask and setting it
does nothing. That matches POSIX, where blocking `SIGKILL` silently succeeds and
has no effect.

## Where it is enforced

[[sub-kernel-notes]] — the queue, both delivery paths, the prefix gates that
keep userspace out of the kernel-synthetic name families, and the EL0-return
tail where a handler frame is built.

## Caveats

**No model.** There is no `notes.tla`; the invariant is held by the queue-lock
discipline, the focused audit rounds, and the test suite. The source says so
plainly. Given how much of this file is audit-round scar tissue — F1, F9, F2,
R2-F2, R2-F7, R3-F3, R3-F4, R3-F5, R4-F1, R4-F6 all left marks — the absence is
worth noting: this is a wait/wake-adjacent surface with two consumers of one
queue, which is the shape that usually earns a module.

**The supported-set constant is decorative.** `NOTE_MASK_SUPPORTED` names the
set of meaningful mask bits and has **zero consumers** — the mask syscall stores
whatever it is given, and unknown bits are inert because nothing looks them up,
not because anything filters them. See [[sub-kernel-notes]] and task #61.
