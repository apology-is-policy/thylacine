---
id: spec-loom-devgone
type: spec
title: "loom_devgone.tla"
models: [sub-kernel-loom]
pins: [inv-i29]
cfgs:
  - "loom_devgone.cfg -- clean: the seven-conjunct safety set"
  - "loom_devgone_liveness.cfg -- EventuallyTerminates: no op hangs, independent of a death"
  - "loom_devgone_buggy_double.cfg -- BUGGY_DOUBLE_ON_DEATH: a death completes an op that already completed (NoDoubleTerminal)"
  - "loom_devgone_buggy_leaks_inflight.cfg -- BUGGY_LEAKS_INFLIGHT: a death leaves an op in flight forever (SessionDeathCompletes)"
  - "loom_devgone_buggy_drops_reason.cfg -- BUGGY_DROPS_REASON: a device-gone death reports the generic I/O error (DeathResultFaithful)"
gate: "any change to session-death completion, the device-gone reason, or the in-flight sweep on death"
created: 2026-08-02
updated: 2026-08-02
---
## Abstraction

A session dies — either ordinarily, or because the underlying device went away —
and every operation still in flight on it must terminate. The model is the
death sweep: which operations complete, with what result, and whether any is
completed twice or not at all.

The sibling that exists because a death is not a reply. The base module reasons
about an operation's own reply arriving; this one reasons about an *external*
event completing an arbitrary set of operations at once, which is a different
shape and a different set of ways to be wrong.

## What it pins

- **`SessionDeathCompletes`** — the safety statement that a death atomically
  completes everything in flight. Stated as safety rather than liveness on
  purpose: "eventually terminates" would permit an unbounded hang, and the
  property wanted is that the death itself is the terminal event.
- **`NoDoubleTerminal`** — an operation whose reply and whose session's death
  race produces one completion, not two. This is the sharp one, because the two
  paths run in different contexts and both look like the last word.
- **`DeathResultFaithful` and `DevgoneOnlyFromDevgoneSession`** — the reported
  reason matches the death's actual cause, and the device-gone reason is
  reachable only from a device-gone session. A generic error where the caller
  could have distinguished a vanished device is a real loss: it is the difference
  between "retry" and "this device is not coming back".

That distinction is why a POSIX-fixed no-such-device error was appended to the
registry for this path. It is emitted only by the asynchronous completion route;
the synchronous front end and the boot path keep the generic I/O error, so the
new reason is additive rather than a change to an established surface.

## What it cannot see

Which subsystem died and why. The model has a session that dies; the real causes
— a server exiting, a transport breaking, a device removal — are all one event
here.

The abandon protocol's own concurrency, in which teardown severs an operation's
link to the engine under the engine's lock while a demultiplex may be completing
it, is an implementation lifetime argument below this abstraction. The model
says a death completes everything in flight; it does not model the lock that
makes the two paths mutually exclusive.

## Binding

`specs/SPEC-TO-CODE.md::loom_devgone.tla`. The death ↔ the client's
mark-dead sweep under its own lock; the per-op terminal ↔ the completion callback
invoked from it; the device-gone reason ↔ the flag the sweep carries into each
completion.
