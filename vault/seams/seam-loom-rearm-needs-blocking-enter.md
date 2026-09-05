---
id: seam-loom-rearm-needs-blocking-enter
type: seam
title: "A multishot stream re-arms only in a drive loop, so a non-blocking consumer never resumes it"
status: open
surface: [sub-kernel-loom]
opened-by: chg-2026-08-02-async-sweep
tracker: ""
created: 2026-08-02
updated: 2026-08-02
---
## What

A multishot operation posts a shot from the completion callback, which runs
under the engine's lock and may not re-enter it. So the re-issue is flagged and
deferred to a *drive loop* — and there are exactly two: the wait loop, and the
poll thread's loop.

The submit phase is not one of them. On a ring without a poll thread, a call that
submits and returns — either because it asked for no completions or because it
asked not to block — runs the chain-admission pass but **not** the re-arm pass.
So a stream that posted a shot stays flagged until some later call blocks.

The shot is held, not lost. The reservation accounting is symmetric across the
gap, and the next blocking call resumes the stream exactly where it stopped.

## Why it is the interesting shape

This is the [[moc-kernel-async]] deferral pattern's characteristic bug: not the
deferral, but a context that ought to run the deferred work and does not. It is
the same shape as [[seam-el0-irq-tail-no-notes]] — one of several return paths
missing a hook — and it was found by looking for that shape rather than by
reading for it.

The asymmetry is already known to the code, from the other direction. The barrier
gate carries an explicit note that the submit phase's admission pass "has no
preceding re-arm", and adds a term to the gate so a barrier cannot jump ahead of
a stream sitting in its deferred state. The *ordering* consequence was found and
closed by an audit; the *liveness* consequence — the stream simply not resuming —
was not stated.

## Reachability

Nil today. Every payload opcode rejects the multishot flag outright, because a
real multishot operation needs an event source that replies more than once and
none exists yet; the only opcode that accepts it is a durability barrier used as
a synthetic test vehicle. So no consumer can reach this.

It would bite the first real one, and specifically the *good* one: a
latency-sensitive consumer submitting and reaping without ever blocking is the
natural low-latency idiom for this kind of interface, and it is exactly the
caller that would never run the re-arm.

## Trigger

The first multishot-capable event source, together with a consumer that polls the
completion queue rather than waiting on it. The fix is a line — run the re-arm
pass in the submit phase alongside the admission pass — but it wants to land with
the consumer that can test it, since nothing today can tell the two behaviours
apart.

## No task

No consumer exists, and the deferral holds the shot rather than dropping it.
Recorded so the first multishot source arrives with this already known, instead
of as a stream that mysteriously stops.
