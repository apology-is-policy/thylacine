---
id: inv-i29
type: inv
title: "I-29 — completion integrity: exactly one terminal, never lost, never stale"
number: I-29
guards: [sub-kernel-loom]
validated-by: [spec-loom, spec-loom-multishot, spec-loom-order, spec-loom-devgone, gate-smp]
strength: spec
created: 2026-08-02
updated: 2026-08-02
---
## Statement

Every operation submitted through a ring produces **exactly one terminal
completion**, and no completion is ever lost, duplicated, stale, or written over
one the consumer has not yet reaped.

The four clauses are separable and each has its own way of failing:

1. **At least one** — the operation is never abandoned in flight, including when
   the session it rides dies.
2. **At most one** — a reply and a session death racing produce one completion,
   not two.
3. **Not stale** — nothing posts into a ring being torn down.
4. **Not overwritten** — a full completion queue never overwrites unreaped
   results.

A stream operation refines the first two rather than escaping them: many
completions, exactly one terminal, and nothing after it.

## Why it is stated this way

The consumer's only channel is the completion. A submission carries an opaque
token, and the completion echoing that token is the sole way to learn what
happened. So a dropped submission is *unobservable* — the caller waits forever
on a token nothing will ever answer.

That is why the implementation posts a completion for every failure, including
ones that never reached the engine: a bad opcode, an empty handle slot, a rights
denial, an allocation failure. There is no "rejected at the door" path, because
the door has no other way to say so.

It is also why the terminal flag matters beyond bookkeeping. A consumer treats
the terminal as the signal that its registered buffer is recyclable, so a shot
arriving after one is not a cosmetic ordering error — it is a write into memory
the consumer has already reused.

## Enforcement

**Reserve at submit, not at completion.** The obvious full-queue behaviour —
drop the completion, or overwrite the oldest — loses a result either way. So an
entry is not *consumed* unless the queue can still hold one more completion
beyond every posted-unreaped one and every in-flight operation's eventual one.
Back-pressure lands at the front door: the entry waits for the next call. The
completion-time full check survives as a guard whose counter is meant to stay at
zero.

This is the clearest case in the tree of a model constraining an implementation:
`CqNeverOverfull` forbids the overwrite, and reserving at submit is what
satisfies it.

**A stream reserves per shot.** Each re-arm takes its next slot before
re-issuing, so a full queue *defers* a shot rather than losing one — which is
what makes leaving a deferred-re-arm flag set indefinitely safe.

**The private tail is the write index.** The queue position comes from
kernel-private state and a kernel-private mask, never from the shared header the
consumer can write. A hostile header would otherwise turn a completion into a
kernel write at an attacker-chosen offset — the same discipline [[inv-i30]] states
for the submission side.

**Death is a terminal event.** A session dying sweeps every operation in flight
on it to a completion carrying a faithful reason, distinguishing a vanished
device from a generic failure. The abandon protocol runs under the engine's lock,
making it mutually exclusive with a demultiplex that might be completing the same
operation — which is what keeps clause 2 true when both paths look like the last
word.

**Cancellation is a completion.** An operation cancelled because its linked
predecessor failed posts exactly one cancellation result. A cancellation the
consumer cannot observe is the same defect as a lost completion.

## Validation

[[spec-loom]] carries the base set — no double, no spurious, never overfull, no
stale, bounded in flight — plus the wait-list's no-missed-wake. [[spec-loom-multishot]]
extends it to a stream, where the queue is a multiset and exactly-one-terminal is
the load-bearing clause. [[spec-loom-order]] adds that every operation reaching a
terminal state, *including a cancelled one*, posts exactly one completion.
[[spec-loom-devgone]] covers the death sweep and the faithfulness of its reason.
[[gate-smp]] is the empirical backstop.

**blind-to:** the concurrent-admitter window. Two threads entering one ring can
over-reserve, because the room check and the in-flight bump are not atomic with
each other; the residual is a dropped terminal completion under exact
concurrency. It rests on a single-producer submission contract rather than on
anything enforced, the models each assume a single admitter, and the exact
coordination is owed work.
